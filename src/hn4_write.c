/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Atomic Write Pipeline (The Shadow Hop)
 * SOURCE:      hn4_write.c
 * STATUS:      PRODUCTION (v25.1)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Implements the "Shadow Hop" atomic write strategy. Data is written to a
 *   new ballistic trajectory (Shadow LBA), followed by a memory-only Anchor
 *   update. The old data is then eclipsed (discarded).
 *
 * SAFETY INVARIANT (Spec 6.3 & 25.2):
 *   1. Write Data to New Shadow LBA.
 *   2. Barrier (FUA).
 *   3. Update Anchor in RAM (Pointer Switch).
 *   4. Eclipse (Atomic Discard of Old LBA).
 *
 * NOTE: Metadata persistence occurs during Unmount or Sync operations, not
 *       during individual block writes.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_swizzle.h"
#include "hn4_ecc.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include <string.h>

/* Internal Constants */
#define HN4_ORBIT_LIMIT           12
#define HN4_ZNS_TIMEOUT_NS        (30ULL * 1000000000ULL)

/*
 * POLICY LOOKUP TABLES
 * Centralized logic for allocation strategies based on Device Type and Profile.
 */

/* Device Type LUT: SSD(0)=0, HDD(1)=SEQ, ZNS(2)=SEQ, TAPE(3)=SEQ */
static const uint8_t _dev_policy_lut[4] = {
    0,
    HN4_POL_SEQ,
    HN4_POL_SEQ,
    HN4_POL_SEQ
};

/* Profile LUT: PICO(4) and USB(6) force SEQ. Others allow Scatter. */
static const uint8_t _prof_policy_lut[8] = {
    [HN4_PROFILE_GENERIC] = 0,
    [HN4_PROFILE_GAMING]  = 0,
    [HN4_PROFILE_AI]      = 0,
    [HN4_PROFILE_ARCHIVE] = 0,
    [HN4_PROFILE_PICO]    = HN4_POL_SEQ,
    [HN4_PROFILE_SYSTEM]  = 0,
    [HN4_PROFILE_USB]     = HN4_POL_SEQ,
    [7]                   = 0
};

/* =========================================================================
 * ZNS HELPER DEFINITIONS
 * ========================================================================= */

/*
 * Asynchronous Context for ZNS Zone Append operations.
 * Allows polling for completion and capturing the resulting LBA.
 */
typedef struct {
    volatile bool         done;
    volatile hn4_result_t res;
} _zns_write_ctx_t;

static void _zns_append_callback(hn4_io_req_t* r, hn4_result_t res)
{
    _zns_write_ctx_t* ctx = (_zns_write_ctx_t*)r->user_ctx;
    ctx->res = res;

    /* Ensure result visibility before signaling completion */
    atomic_thread_fence(memory_order_release);
    ctx->done = true;
}

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

static inline void _pack_header(
    HN4_OUT hn4_block_header_t* hdr,
    HN4_IN  hn4_u128_t          well_id,
    HN4_IN  uint64_t            seq_idx,
    HN4_IN  uint64_t            generation,
    HN4_IN  uint32_t            data_crc,
    HN4_IN  uint32_t            comp_meta
)
{
    hdr->well_id    = hn4_cpu_to_le128(well_id);
    hdr->seq_index  = hn4_cpu_to_le64(seq_idx);
    hdr->generation = hn4_cpu_to_le64(generation);
    hdr->magic      = hn4_cpu_to_le32(HN4_BLOCK_MAGIC);
    hdr->data_crc   = hn4_cpu_to_le32(data_crc);
    hdr->comp_meta  = hn4_cpu_to_le32(comp_meta);

    hdr->header_crc = 0;
    uint32_t hcrc   = hn4_crc32(HN4_CRC_SEED_HEADER, hdr, offsetof(hn4_block_header_t, header_crc));
    hdr->header_crc = hn4_cpu_to_le32(hcrc);
}

/**
 * _verify_block_at_lba
 *
 * Verifies if a physical block index contains valid data belonging to the
 * specified logical file sequence. Distinguishes "Real Data" from
 * "Stale Shadows" or "Hash Collisions".
 *
 * @param vol           Volume Context.
 * @param phys_blk_idx  Physical Block Index (Not Sector LBA).
 * @param io_buf        Pre-allocated buffer of size vol->vol_block_size.
 * @param well_id       Target File ID (Anchor Seed).
 * @param logical_seq   Target Logical Block Index (N).
 * @param expected_gen  Current Write Generation of the Anchor.
 *
 * @return true if the block is a valid resident.
 */
static bool _verify_block_at_lba(
    HN4_IN hn4_volume_t* vol,
    HN4_IN uint64_t      phys_blk_idx,
    HN4_IN void*         io_buf,
    HN4_IN hn4_u128_t    well_id,
    HN4_IN uint64_t      logical_seq,
    HN4_IN uint64_t      expected_gen
)
{
    if (phys_blk_idx == HN4_LBA_INVALID) return false;

    /* 1. Bitmap Filter (Fast Check) */
    /* If the bit is 0, it's physically impossible for valid data to be here */
    bool allocated;
    if (_bitmap_op(vol, phys_blk_idx, BIT_TEST, &allocated) != HN4_OK || !allocated) {
        return false;
    }

    /* 2. Calculate Physical Geometry */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t bs      = vol->vol_block_size;
    uint32_t ss      = caps->logical_block_size;
    uint32_t sectors = bs / ss;

    hn4_addr_t phys_lba = hn4_lba_from_blocks(phys_blk_idx * sectors);

    /* 3. Read Verification */
    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys_lba, io_buf, sectors) != HN4_OK) {
        return false; /* Read error implies verification failed */
    }

    /* 4. Identity Check */
    hn4_block_header_t* h = (hn4_block_header_t*)io_buf;

    /* Magic Check */
    if (hn4_le32_to_cpu(h->magic) != HN4_BLOCK_MAGIC) return false;

    /* Header Integrity Check */
    uint32_t stored_hcrc = hn4_le32_to_cpu(h->header_crc);
    uint32_t calc_hcrc   = hn4_crc32(HN4_CRC_SEED_HEADER, h, offsetof(hn4_block_header_t, header_crc));
    if (stored_hcrc != calc_hcrc) return false;

    /* Ownership Check (Well ID) */
    hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
    if (disk_id.lo != well_id.lo || disk_id.hi != well_id.hi) return false;

    /* Sequence Check (Ghost Defense) */
    if (hn4_le64_to_cpu(h->seq_index) != logical_seq) return false;

    /*
     * Freshness Check (Spec 25.1)
     *
     * ENGINEERING NOTE: Strict Equality Enforcement.
     * We strictly reject (DiskGen != AnchorGen).
     * If a crash occurred after Data Write but before Anchor Update, valid data
     * may exist on disk with a generation higher than the Anchor. We choose to
     * ORPHAN this data (leaving it for FSCK) rather than resurrect it.
     * This ensures the Volume View remains strictly consistent with the last
     * successful Anchor Commit.
     */
    if (hn4_le64_to_cpu(h->generation) != expected_gen) return false;

    return true;
}

/**
 * _resolve_residency_verified
 *
 * Scans the volume to find the current physical location of a logical block.
 * Handles both Ballistic (D1) and Horizon (D1.5) addressing modes.
 *
 * Returns: Physical Block Index or HN4_LBA_INVALID.
 */
uint64_t _resolve_residency_verified(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* anchor,
    HN4_IN uint64_t      block_idx
)
{
    /* 1. Unpack Anchor Physics */
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);

    const uint8_t* raw_v = anchor->orbit_vector;
    uint64_t V = (uint64_t)raw_v[0] |
                 ((uint64_t)raw_v[1] << 8)  |
                 ((uint64_t)raw_v[2] << 16) |
                 ((uint64_t)raw_v[3] << 24) |
                 ((uint64_t)raw_v[4] << 32) |
                 ((uint64_t)raw_v[5] << 40);

    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);

    /* Load Data Class Flags */
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);

    hn4_u128_t my_well_id = hn4_le128_to_cpu(anchor->seed_id);
    uint32_t bs = vol->vol_block_size;

    /* Extract Current Generation for Verification */
    uint64_t current_gen = (uint64_t)hn4_le32_to_cpu(anchor->write_gen);

    /* 2. Allocate Verification Buffer */
    void* check_buf = hn4_hal_mem_alloc(bs);

    /*
     * Resource Safety:
     * Explicitly check for allocation failure to prevent dereferencing NULL.
     */
    if (!check_buf) {
        return HN4_LBA_INVALID;
    }

    uint64_t found_lba = HN4_LBA_INVALID;

    /* =====================================================================
     * PATH A: HORIZON LINEAR LOOKUP (Spec 6.4 / 7.3)
     * If the file is flagged as Horizon, data is sequential starting at G.
     * ===================================================================== */
    if (dclass & HN4_HINT_HORIZON) {
        /*
         * In Horizon Mode:
         * G = Physical Block Index of the start of the chain.
         * Offset = Logical_Index * Fractal_Stride (2^M).
         */
        uint64_t stride = (1ULL << M);

        /* Check for overflow before calc */
        if (block_idx < (UINT64_MAX / stride)) {
            uint64_t linear_lba = G + (block_idx * stride);

            if (_verify_block_at_lba(vol, linear_lba, check_buf, my_well_id, block_idx, current_gen)) {
                found_lba = linear_lba;
                goto Cleanup;
            }
        }
        /*
         * Fallthrough Safety:
         * Even if HINT_HORIZON is set, we continue to check Ballistic Orbits.
         * During a "Gravity Collapse" or "Re-Ballistification" transition,
         * the file might be in a mixed state (some blocks moved, some not).
         */
    }

    /* =====================================================================
     * PATH B: BALLISTIC ORBIT SCAN (Standard)
     * Scan shells k=0..12 for the block.
     * ===================================================================== */
    for (uint8_t k = 0; k < HN4_ORBIT_LIMIT; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, block_idx, M, k);

        /* _verify_block_at_lba performs bounds check, bitmap check, and ID check */
        if (_verify_block_at_lba(vol, lba, check_buf, my_well_id, block_idx, current_gen)) {
            found_lba = lba;
            goto Cleanup;
        }
    }

Cleanup:
    hn4_hal_mem_free(check_buf);
    return found_lba;
}


/* =========================================================================
 * CORE WRITE LOGIC
 * ========================================================================= */

_Check_return_ hn4_result_t hn4_write_block_atomic(
    HN4_IN hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN uint64_t block_idx,
    HN4_IN const void* data,
    HN4_IN uint32_t len
)
{
    HN4_LOG_CRIT("WRITE_ATOMIC: Enter. Vol=%p Block=%llu Len=%u", vol, (unsigned long long)block_idx, len);

    /* 1. Pre-flight Validation */
    if (!vol || !anchor || !data) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Invalid Args (NULL ptr)");
        return HN4_ERR_INVALID_ARGUMENT;
    }
    if (vol->read_only) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Volume is RO");
        return HN4_ERR_ACCESS_DENIED;
    }
    if (vol->sb.info.state_flags & HN4_VOL_PANIC) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Volume Panic. Writes disabled.");
        return HN4_ERR_VOLUME_LOCKED;
    }

    uint64_t dclass_check = hn4_le64_to_cpu(anchor->data_class);

    /*
     * Tombstone Check:
     * We must not write to a file marked for deletion (Tombstone).
     * Doing so would create "Zombie Allocations" that the Reaper might miss.
     */
    if (dclass_check & HN4_FLAG_TOMBSTONE) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Attempted write to Tombstone (Deleted File).");
        return HN4_ERR_TOMBSTONE;
    }

    uint32_t perms = hn4_le32_to_cpu(anchor->permissions);

    /* Immutable Check (Spec 9.4) */
    if (perms & HN4_PERM_IMMUTABLE) {
        HN4_LOG_CRIT("WRITE_ATOMIC: File is Immutable");
        return HN4_ERR_IMMUTABLE;
    }

    /* Append-Only Check (Spec 9.2 Bit 3) */
    if ((perms & HN4_PERM_APPEND) && !(perms & HN4_PERM_WRITE)) {
        /* Calculate current logical bounds */
        uint64_t mass       = hn4_le64_to_cpu(anchor->mass);
        uint32_t payload_sz = HN4_BLOCK_PayloadSize(vol->vol_block_size);
        uint64_t max_idx    = (mass + payload_sz - 1) / payload_sz;

        if (block_idx < max_idx) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Violation of Append-Only Constraint");
            return HN4_ERR_ACCESS_DENIED;
        }
    }

    /* Basic Write Check */
    if (!(perms & (HN4_PERM_WRITE | HN4_PERM_APPEND | HN4_PERM_SOVEREIGN))) {
        return HN4_ERR_ACCESS_DENIED;
    }

    /* 2. Geometry Setup */
    uint32_t bs          = vol->vol_block_size;
    uint32_t payload_cap = HN4_BLOCK_PayloadSize(bs);

    if (len > payload_cap) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Payload too large");
        return HN4_ERR_INVALID_ARGUMENT;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;

    /* Strict Geometry Alignment Check */
    if (ss == 0 || (bs % ss) != 0) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Geometry Error BS=%u SS=%u", bs, ss);
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    uint32_t sectors = bs / ss;

    /*
     * PHASE 0: RESIDENCY RESOLUTION
     * Locate the previous block (if any) so we can eclipse it later.
     */
    uint64_t old_lba = _resolve_residency_verified(vol, anchor, block_idx);
    HN4_LOG_CRIT("WRITE_ATOMIC: Old Residency LBA = %llu", (unsigned long long)old_lba);

    /* 3. Allocate IO Buffer */
    void* io_buf = hn4_hal_mem_alloc(bs);
    if (!io_buf) {
        HN4_LOG_CRIT("WRITE_ATOMIC: OOM allocating IO buffer");
        return HN4_ERR_NOMEM;
    }

    /*
     * INVARIANT: STRICT ZERO FILL REQUIRED
     * The CRC matches the full payload capacity (bs - header).
     * Trailing bytes MUST be zero for checksum consistency.
     */
    memset(io_buf, 0, bs);

    /* 4. Prepare Payload (With Compression Logic) */
    hn4_block_header_t* hdr = (hn4_block_header_t*)io_buf;

    uint32_t final_algo = HN4_COMP_NONE;
    uint32_t stored_len = len;

    /* Check Hints: Do we try to compress? */
    /* Only attempt if hint is set OR profile is Archive */
    bool try_compress = (dclass_check & HN4_HINT_COMPRESSED) ||
                        (vol->sb.info.format_profile == HN4_PROFILE_ARCHIVE);

    if (try_compress && len > 128) {
        /* Calculate worst-case bound */
        uint32_t bound = hn4_compress_bound(len);
        void* comp_scratch = hn4_hal_mem_alloc(bound);

        if (comp_scratch) {
            uint32_t comp_size = 0;

            /* Attempt Compression */
            hn4_result_t c_res = hn4_compress_block(
                data,
                len,
                comp_scratch,
                bound,
                &comp_size,
                vol->sb.info.device_type_tag, /* e.g. HN4_DEV_HDD */
                vol->sb.info.hw_caps_flags    /* e.g. HN4_HW_NVM */
            );

            /*
             * Evaluation:
             * - Must fit in payload_cap.
             * - Must be efficient (comp_size < len).
             * - Must succeed.
             */
            if (c_res == HN4_OK && comp_size < payload_cap && comp_size < len) {
                /* SUCCESS: Commit compressed data */
                memcpy(hdr->payload, comp_scratch, comp_size);

                /* Zero-fill remainder of payload slot is handled by memset(io_buf, 0) above */

                final_algo = HN4_COMP_TCC;
                stored_len = comp_size; /* Store compressed size in meta */
                HN4_LOG_CRIT("WRITE_ATOMIC: Compression Success. %u -> %u bytes.", len, comp_size);
            }
            /* ELSE: Fallback to Raw (Implicit) */

            hn4_hal_mem_free(comp_scratch);
        }
    }

    /* Fallback: If compression failed/skipped, copy raw */
    if (final_algo == HN4_COMP_NONE) {
        memcpy(hdr->payload, data, len);
    }

    /* CRC covers full slot (data + zero padding) */
    uint32_t d_crc = hn4_crc32(HN4_CRC_SEED_DATA, hdr->payload, payload_cap);

    /*
     * 5. The Shadow Hop (Allocation)
     */
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
    uint64_t V = 0;
    memcpy(&V, anchor->orbit_vector, 6);
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);

    /*
     * Wrap Generation Logic (v26.0 Epoch Rotation).
     * We allow the 32-bit generation to wrap to 1.
     *
     * SAFETY PROOF:
     * HN4 uses Ballistic Allocation (Shadow Hop). Every write moves the
     * physical LBA to a new trajectory 'k'. The probability of colliding
     * with a "Phantom Block" from exactly 4,294,967,295 transactions ago
     * at the exact same physical LBA is cryptographically negligible.
     */
    uint32_t current_gen = hn4_le32_to_cpu(anchor->write_gen);
    uint32_t next_gen_32;

    if (current_gen == UINT32_MAX) {
        HN4_LOG_WARN("WRITE_ATOMIC: Generation Wrap (Epoch Rotation). Resetting to 1.");
        next_gen_32 = 1;
    } else {
        next_gen_32 = current_gen + 1;
    }
    uint64_t next_gen = (uint64_t)next_gen_32;

    HN4_LOG_CRIT("WRITE_ATOMIC: Physics G=%llu V=%llu M=%u NextGen=%llu",
                 (unsigned long long)G, (unsigned long long)V, M, (unsigned long long)next_gen);

    uint64_t target_lba = HN4_LBA_INVALID;
    hn4_result_t alloc_res = HN4_ERR_GRAVITY_COLLAPSE;

    /*
     * POLICY MASK DETERMINATION (Refactored to Lookup Table)
     * Replaces extensive if/else logic with O(1) table lookup.
     */
    uint32_t dev_type = vol->sb.info.device_type_tag;
    uint32_t profile  = vol->sb.info.format_profile;

    /*
     * Mask with 0x3 (device) and 0x7 (profile) to prevent OOB access.
     * Combine policies using bitwise OR.
     */
    uint8_t policy_mask = _dev_policy_lut[dev_type & 0x3] | _prof_policy_lut[profile & 0x7];

    /* Allocation Loop */
    uint8_t k_limit = (policy_mask & HN4_POL_SEQ) ? 0 : HN4_ORBIT_LIMIT;

    if (vol->sb.info.state_flags & HN4_VOL_RUNTIME_SATURATED) {
        /*
         * Adjusted Saturation Decay:
         * Raw capacity includes static metadata overhead. Subtract 5% heuristic reserve
         * to normalize usable capacity before calculating the 90% threshold.
         */
        uint64_t raw_blks    = vol->vol_capacity_bytes / vol->vol_block_size;
        uint64_t usable_blks = raw_blks - (raw_blks / 20); /* 5% overhead */
        uint64_t threshold   = (usable_blks * 90) / 100;

        if (atomic_load(&vol->used_blocks) < threshold) {
            atomic_fetch_and(&vol->sb.info.state_flags, ~HN4_VOL_RUNTIME_SATURATED);
        }
    } else {
        for (uint8_t k = 0; k <= k_limit; k++) {
            uint64_t candidate = _calc_trajectory_lba(vol, G, V, block_idx, M, k);

            if (candidate == HN4_LBA_INVALID) continue;

            /* Active Quality Mask Check */
            if (vol->quality_mask) {
                uint64_t word_idx = candidate / 32;

                /* Memory Safety: Validate that the END of the word fits in the buffer */
                if (((word_idx + 1) * sizeof(uint64_t)) <= vol->qmask_size) {
                    uint32_t shift   = (candidate % 32) * 2;
                    uint64_t q_word  = vol->quality_mask[word_idx];
                    uint8_t  q_val   = (q_word >> shift) & 0x3;

                    /* Reject Toxic (00) */
                    if (q_val == HN4_Q_TOXIC) {
                        continue;
                    }

                    /* Priority Check: Reject Bronze (01) if file is Critical */
                    uint64_t dclass       = hn4_le64_to_cpu(anchor->data_class);
                    bool     is_high_prio = (dclass & HN4_FLAG_PINNED) ||
                                            ((dclass & HN4_CLASS_VOL_MASK) == HN4_VOL_STATIC);
                    bool     is_ai        = (vol->sb.info.format_profile == HN4_PROFILE_AI);

                    if ((is_high_prio || is_ai) && q_val == HN4_Q_BRONZE) {
                        continue;
                    }
                }
            }

            /* Atomic Reservation */
            bool bit_flipped; /* Indicates 0->1 transition (Allocation Success) */
            hn4_result_t op_res = _bitmap_op(vol, candidate, BIT_SET, &bit_flipped);

            if (op_res != HN4_OK) {
                alloc_res = HN4_ERR_BITMAP_CORRUPT;
                break;
            }

            if (bit_flipped) {
                atomic_thread_fence(memory_order_release);
                target_lba = candidate;
                alloc_res = HN4_OK;
                break;
            }
        }
    }

    /* Fallback to Horizon (D1.5) if Flux (D1) is saturated */
    if (alloc_res != HN4_OK) {
        if (alloc_res == HN4_ERR_GRAVITY_COLLAPSE) {
            HN4_LOG_CRIT("WRITE_ATOMIC: D1 Full. Trying Horizon...");

            uint64_t horizon_phys_sector = HN4_LBA_INVALID;
            alloc_res = hn4_alloc_horizon(vol, &horizon_phys_sector);

            if (alloc_res == HN4_OK) {
                /*
                 * Alignment Assertion.
                 * The Horizon Allocator must return a sector aligned to the Block boundary.
                 */
                if (horizon_phys_sector % sectors != 0) {
                    HN4_LOG_CRIT("WRITE_ATOMIC: Horizon Misalignment (Sect %llu %% %u != 0)",
                                 (unsigned long long)horizon_phys_sector, sectors);

                    /*
                     * Release reservation.
                     * CONTRACT: hn4_free_block expects Physical Sector LBA.
                     * horizon_phys_sector is already in sectors.
                     */
                    uint64_t lba_to_free = horizon_phys_sector;
                    hn4_free_block(vol, lba_to_free);

                    alloc_res = HN4_ERR_ALIGNMENT_FAIL;
                } else {
                    /* Immediate Unit Conversion */
                    target_lba = horizon_phys_sector / sectors; // Convert Sector -> Block Index

                    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
                    dclass |= HN4_HINT_HORIZON;
                    anchor->data_class = hn4_cpu_to_le64(dclass);

                    /* Check if we need to update Gravity Center (G) */
                    uint64_t stride = (1ULL << M);
                    uint64_t offset = block_idx * stride;

                    /* Use the normalized Block Index for math */
                    if (target_lba >= offset) {
                        uint64_t linear_start = target_lba - offset;
                        anchor->gravity_center = hn4_cpu_to_le64(linear_start);

                        /*
                         * Check Result & Rollback.
                         * If we fail to persist the new Gravity Center, we must release
                         * the Horizon block we just claimed.
                         */
                        hn4_result_t anchor_res = hn4_write_anchor_atomic(vol, anchor);

                        if (anchor_res != HN4_OK) {
                            hn4_free_block(vol, horizon_phys_sector);
                            alloc_res = anchor_res;
                        }
                    } else {
                        alloc_res = HN4_ERR_GEOMETRY;
                    }
                } /* End else (Aligned) */
            }
        }
        if (alloc_res != HN4_OK) {
            hn4_hal_mem_free(io_buf);
            return alloc_res;
        }
    }

    /* 6. Seal Header */
    uint32_t comp_meta = (stored_len << HN4_COMP_SIZE_SHIFT) | final_algo;
    _pack_header(hdr, hn4_le128_to_cpu(anchor->seed_id), block_idx, next_gen, d_crc, comp_meta);

    /* 7. Commit Data to Media (The Shadow Write) */
    hn4_addr_t phys_sector = hn4_lba_from_sectors(target_lba * sectors);
    hn4_result_t io_res;

    /* OPTIMIZATION: ZNS ZONE APPEND (Spec 13.2) */
    if (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE) {

        /* 1. Calculate Zone Start LBA */
        uint32_t zone_bytes   = caps->zone_size_bytes;
        uint64_t zone_sectors = zone_bytes / ss;
        uint64_t raw_lba      = hn4_addr_to_u64(phys_sector);
        uint64_t zone_start   = (raw_lba / zone_sectors) * zone_sectors;

        /* 2. Manual Async Submission */
        hn4_io_req_t req = {0};
        req.op_code = HN4_IO_ZONE_APPEND;
        req.lba     = hn4_addr_from_u64(zone_start); /* Send to Zone Handle */
        req.buffer  = io_buf;
        req.length  = sectors;

        /* Initialize context */
        _zns_write_ctx_t ctx = { .done = false, .res = HN4_OK };
        req.user_ctx = &ctx;

        /* Pass the static function pointer */
        hn4_hal_submit_io(vol->target_device, &req, _zns_append_callback);

        /*
         * TIMEOUT WATCHDOG:
         * Prevent infinite spin if HAL/Device wedges.
         */
        hn4_time_t start_ts = hn4_hal_get_time_ns();

        while (!ctx.done) {
            hn4_hal_poll(vol->target_device);

            if ((hn4_hal_get_time_ns() - start_ts) > HN4_ZNS_TIMEOUT_NS) {
                HN4_LOG_CRIT("ZNS Append Timeout! Device stalled.");
                ctx.res = HN4_ERR_ATOMICS_TIMEOUT;
                break;
            }
        }
        io_res = ctx.res;

        if (io_res == HN4_OK) {
            /* 3. REVERSE ENGINEER GRAVITY */
            /* The drive chose a location. We must update target_lba to match reality. */
            phys_sector = req.result_lba;
            uint64_t actual_lba_idx = hn4_addr_to_u64(phys_sector) / sectors;

            if (actual_lba_idx != target_lba) {
                /*
                 * We cannot update the global Gravity Center (G) based on a single block's
                 * drift, as this would invalidate the trajectory for all other blocks
                 * in the file.
                 *
                 * If the ZNS drive ignores our placement hint, we must reject the write
                 * to preserve the mathematical integrity of the Ballistic Index.
                 * The allocator should fallback to the Horizon (Linear) on retry.
                 */
                HN4_LOG_CRIT("ZNS Drift Detected: Expected %llu, Got %llu. Aborting to protect G.",
                             (unsigned long long)target_lba, (unsigned long long)actual_lba_idx);

                /* Rollback the bitmap claim on the predicted block */
                _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);

                /* We cannot free the actual_lba_idx because we don't own it in the bitmap yet,
                   and the drive has already written to it. This creates a small leak
                   (Space Amplification) to be cleaned by the Scavenger later. */

                return HN4_ERR_GEOMETRY;
            }
        }
    } else {
        /* Standard SSD/HDD Write */
        io_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, phys_sector, io_buf, sectors);
    }

    if (io_res != HN4_OK) {
        /*
         * RESCUE PROTOCOL (v26.0):
         * If we timed out, the drive might have actually written the data
         * but dropped the completion interrupt. Attempt to verify before leaking.
         */
        bool rescued = false;

        if (io_res == HN4_ERR_ATOMICS_TIMEOUT) {
            HN4_LOG_WARN("WRITE_ATOMIC: Timeout. Attempting Rescue Protocol...");

            /* 1. Force Barrier to drain drive queue */
            if (hn4_hal_barrier(vol->target_device) == HN4_OK) {

                /* 2. Read-Back Verify */
                void* rescue_buf = hn4_hal_mem_alloc(bs);
                if (rescue_buf) {
                    hn4_result_t r_res = hn4_hal_sync_io(vol->target_device,
                                                         HN4_IO_READ,
                                                         phys_sector,
                                                         rescue_buf,
                                                         sectors);

                    if (r_res == HN4_OK) {
                        /* Check if the data on disk matches what we intended to write */
                        if (memcmp(io_buf, rescue_buf, bs) == 0) {
                            HN4_LOG_WARN("WRITE_ATOMIC: Rescue Successful! Latent write confirmed.");
                            rescued = true;
                            io_res = HN4_OK; /* Clear Error */
                        }
                    }
                    hn4_hal_mem_free(rescue_buf);
                }
            }
        }

        if (!rescued) {
            HN4_LOG_CRIT("WRITE_ATOMIC: IO Error %d. Rolling back.", io_res);

            /*
             * LATENT WRITE PROTECTION (The Leak):
             * We can only free the bitmap if we are certain the drive stopped.
             * On Timeout, we must leak to prevent data corruption of future allocs.
             */
            if (io_res != HN4_ERR_ATOMICS_TIMEOUT) {
                _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);
            } else {
                HN4_LOG_CRIT("WRITE_ATOMIC: Timeout persists. Leaking Block %llu for Scavenger.",
                             (unsigned long long)target_lba);

                /* Mark Volume as Dirty so Scavenger knows to scan eventually */
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);

                /* 2. Downgrade Silicon Quality (Firmware distress signal) */
                if (vol->quality_mask) {
                    uint64_t w_idx = target_lba / 32;
                    /* Bounds Check */
                    if (((w_idx + 1) * 8) <= vol->qmask_size) {
                        uint32_t shift = (target_lba % 32) * 2;
                        _Atomic uint64_t* q_ptr = (_Atomic uint64_t*)&vol->quality_mask[w_idx];
                        uint64_t old_val = atomic_load_explicit(q_ptr, memory_order_relaxed);
                        uint64_t new_val;
                        int retries = 0;
                        bool success = false;

                        do {
                            uint64_t current_state = (old_val >> shift) & 0x3;
                            if (current_state == HN4_Q_TOXIC) {
                                success = true; /* Already toxic */
                                break;
                            }
                            uint64_t cleared = old_val & ~(3ULL << shift);
                            new_val = cleared | (1ULL << shift); /* Set Bronze */
                            success = atomic_compare_exchange_weak_explicit(q_ptr, &old_val, new_val,
                                            memory_order_release,
                                            memory_order_relaxed);
                        } while (!success && ++retries < 100);
                    }
                }
            }

            hn4_hal_mem_free(io_buf);
            return io_res;
        }
    }

    /* 8. The Wall (Data Persistence Barrier) */
    /*
     * OPTIMIZATION: Skip barrier on NVM only if STRICT_FLUSH is certified.
     * We do not trust the NVM flag alone; the HAL must opt-in to the
     * strict durability contract.
     */
    if ((vol->sb.info.hw_caps_flags & HN4_HW_NVM) &&
        (vol->sb.info.hw_caps_flags & HN4_HW_STRICT_FLUSH))
    {
        io_res = HN4_OK;
    } else {
        /* Default to Safe: Issue explicit flush */
        io_res = hn4_hal_barrier(vol->target_device);
    }

    if (io_res != HN4_OK) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Barrier Error %d. Rolling back.", io_res);
        _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    /* 9. Metadata Update */
    atomic_thread_fence(memory_order_release);

    uint32_t old_gen_le = anchor->write_gen;
    while (!atomic_compare_exchange_weak(
        (_Atomic uint32_t*)&anchor->write_gen, 
        &old_gen_le, 
        hn4_cpu_to_le32((uint32_t)next_gen)));

    uint64_t now_le = hn4_cpu_to_le64(hn4_hal_get_time_ns());
    atomic_store((_Atomic uint64_t*)&anchor->mod_clock, now_le);

    uint64_t end_byte = (block_idx * payload_cap) + len;
    uint64_t curr_mass_le = atomic_load((_Atomic uint64_t*)&anchor->mass);
    
    while (1) {
        uint64_t curr_mass_cpu = hn4_le64_to_cpu(curr_mass_le);
        if (end_byte <= curr_mass_cpu) break;
        
        uint64_t new_mass_le = hn4_cpu_to_le64(end_byte);
        if (atomic_compare_exchange_weak(
            (_Atomic uint64_t*)&anchor->mass, 
            &curr_mass_le, 
            new_mass_le)) break;
    }

    /* 10. THE ECLIPSE (Atomic Discard of Old LBA) */
    if (old_lba != HN4_LBA_INVALID && old_lba != target_lba) {
        /*
         * We removed the synchronous HN4_IO_DISCARD command.
         * Blocking on TRIM/UNMAP during the write path causes severe latency spikes.
         * The old data is logically unreachable once the Anchor is updated (Step 9).
         */

        /* Barrier: Ensure the Anchor update (Step 9) is visible before freeing old space */
        atomic_thread_fence(memory_order_release);

        /* Logically Free the old block. Physical TRIM is delegated to the Scavenger. */
        if (_bitmap_op(vol, old_lba, BIT_CLEAR, NULL) != HN4_OK) {
            atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        }
    }

    HN4_LOG_CRIT("WRITE_ATOMIC: Success.");
    hn4_hal_mem_free(io_buf);
    return HN4_OK;
}
