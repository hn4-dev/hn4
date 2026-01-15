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

    uint64_t max_blocks = vol->vol_capacity_bytes / vol->vol_block_size;
    if (phys_blk_idx >= max_blocks) return false;

    /* 1. Bitmap Filter */
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
    HN4_OUT uint64_t  block_idx
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
        uint64_t stride = (1ULL << M);

        /* Check 1: Multiplication Overflow */
        if (block_idx < (UINT64_MAX / stride)) {
            uint64_t offset = block_idx * stride;
            
            /* Ensure G + offset does not wrap UINT64_MAX */
            if ((UINT64_MAX - G) >= offset) {
                
                uint64_t linear_lba = G + offset;
                
                bool in_bounds = false;
#ifdef HN4_USE_128BIT
                /* 
                 * We can safely assume linear_lba (u64) fits in capacity 
                 * if capacity.hi > 0 or capacity.lo is large enough.
                 * To be precise: max_blocks = capacity / block_size.
                 * Since we don't have a u128_div available here easily, 
                 * check if linear_lba * bs < capacity.
                 */
                hn4_u128_t limit_chk = hn4_u128_mul_u64(hn4_u128_from_u64(linear_lba), vol->vol_block_size);
                if (hn4_u128_cmp(limit_chk, vol->vol_capacity_bytes) < 0) in_bounds = true;
#else
                uint64_t max_vol_blocks = vol->vol_capacity_bytes / vol->vol_block_size;
                if (linear_lba < max_vol_blocks) in_bounds = true;
#endif

                if (in_bounds) {
                    if (_verify_block_at_lba(vol, linear_lba, check_buf, my_well_id, block_idx, current_gen)) {
                        found_lba = linear_lba;
                        goto Cleanup;
                    }
                }
            } else {
                HN4_LOG_WARN("Horizon LBA Wrap detected. File logical offset exceeds 64-bit physical space.");
            }
        }
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
    HN4_IN uint32_t len,
    HN4_IN uint32_t session_perms /* Delegated rights */
)
{
    HN4_LOG_CRIT("WRITE_ATOMIC: Enter. Vol=%p Block=%llu Len=%u", vol, (unsigned long long)block_idx, len);

    /* 1. Pre-flight Validation */
    if (!vol || !anchor || !data) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Invalid Args (NULL ptr)");
        return HN4_ERR_INVALID_ARGUMENT;
    }
retry_transaction:;
    if (vol->read_only) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Volume is RO");
        return HN4_ERR_ACCESS_DENIED;
    }
    if (vol->sb.info.state_flags & HN4_VOL_PANIC) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Volume Panic. Writes disabled.");
        return HN4_ERR_VOLUME_LOCKED;
    }

    hn4_anchor_t txn_anchor;
    memcpy(&txn_anchor, anchor, sizeof(hn4_anchor_t));

    /* Use txn_anchor for logic checks */
    uint64_t dclass_check = hn4_le64_to_cpu(txn_anchor.data_class);

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

    /* Calculate Effective Permissions */
    uint32_t effective_perms = perms | session_perms;

    /* Append-Only Check (Spec 9.2 Bit 3) */
    if ((effective_perms & HN4_PERM_APPEND) && !(effective_perms & HN4_PERM_WRITE)) {
    /* Calculate current logical bounds */
        uint64_t mass       = hn4_le64_to_cpu(anchor->mass);
        uint32_t payload_sz = HN4_BLOCK_PayloadSize(vol->vol_block_size);
        uint64_t max_idx    = (mass + payload_sz - 1) / payload_sz;

        /* Reject overwrite of existing blocks */
        if (block_idx < max_idx) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Violation of Append-Only Constraint (Blk %llu < Max %llu)", 
                         (unsigned long long)block_idx, (unsigned long long)max_idx);
            return HN4_ERR_ACCESS_DENIED;
        }
    }

    /* Basic Write Check */
    if (!(effective_perms & (HN4_PERM_WRITE | HN4_PERM_APPEND | HN4_PERM_SOVEREIGN))) {
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

    /* 
     * THAW PROTOCOL (Spec 20.5): 
     * If overwriting a block partially, we must Read-Modify-Write to preserve data.
     */
    if (old_lba != HN4_LBA_INVALID && len < payload_cap) {
        void* thaw_buf = hn4_hal_mem_alloc(bs);
        
        if (!thaw_buf) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw alloc failed. Aborting to prevent data loss.");
            hn4_hal_mem_free(io_buf); /* Clean up the main buffer before return */
            return HN4_ERR_NOMEM;
        }

        hn4_addr_t old_phys = hn4_lba_from_blocks(old_lba * sectors);
        
        hn4_result_t r_res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, old_phys, thaw_buf, sectors);
        if (r_res != HN4_OK) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw read failed. Aborting.");
            hn4_hal_mem_free(thaw_buf);
            hn4_hal_mem_free(io_buf);
            return r_res;
        }

        hn4_block_header_t* old_hdr = (hn4_block_header_t*)thaw_buf;
        hn4_block_header_t* new_hdr_view = (hn4_block_header_t*)io_buf;

        if (hn4_le32_to_cpu(old_hdr->magic) != HN4_BLOCK_MAGIC) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw source corrupt (Phantom Block). Aborting.");
            hn4_hal_mem_free(thaw_buf);
            hn4_hal_mem_free(io_buf);
            return HN4_ERR_PHANTOM_BLOCK;
        }
        uint32_t old_hcrc = hn4_le32_to_cpu(old_hdr->header_crc);
        uint32_t cal_hcrc = hn4_crc32(HN4_CRC_SEED_HEADER, old_hdr, offsetof(hn4_block_header_t, header_crc));
        
        if (old_hcrc != cal_hcrc) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw source has Header Rot. Aborting.");
            hn4_hal_mem_free(thaw_buf);
            hn4_hal_mem_free(io_buf);
            return HN4_ERR_HEADER_ROT;
        }

        uint32_t meta = hn4_le32_to_cpu(old_hdr->comp_meta);
        uint8_t algo  = meta & HN4_COMP_ALGO_MASK;
        uint32_t csz  = meta >> HN4_COMP_SIZE_SHIFT;
        
        if (algo == HN4_COMP_TCC) {
             uint32_t out_sz = 0;
             hn4_result_t d_res = hn4_decompress_block(old_hdr->payload, csz, new_hdr_view->payload, payload_cap, &out_sz);
             
             if (d_res != HN4_OK) {
                 hn4_hal_mem_free(thaw_buf);
                 hn4_hal_mem_free(io_buf);
                 return HN4_ERR_DECOMPRESS_FAIL;
             }
        } else {
             memcpy(new_hdr_view->payload, old_hdr->payload, payload_cap);
        }

        hn4_hal_mem_free(thaw_buf);
    }

    /* 4. Prepare Payload (With Compression Logic) */
    hn4_block_header_t* hdr = (hn4_block_header_t*)io_buf;

    uint32_t final_algo = HN4_COMP_NONE;
    uint32_t stored_len = len;

    bool try_compress = (dclass_check & HN4_HINT_COMPRESSED) ||
                        (vol->sb.info.format_profile == HN4_PROFILE_ARCHIVE);

    /* 
     * If this is an Overwrite (old_lba valid), do NOT re-compress immediately.
     * Write as RAW to minimize latency. The Scavenger will Refreeze later.
     */
    if (old_lba != HN4_LBA_INVALID) {
        try_compress = false;
    }

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
    const uint8_t* raw_v = anchor->orbit_vector;
    uint64_t V = (uint64_t)raw_v[0] |
                 ((uint64_t)raw_v[1] << 8)  |
                 ((uint64_t)raw_v[2] << 16) |
                 ((uint64_t)raw_v[3] << 24) |
                 ((uint64_t)raw_v[4] << 32) |
                 ((uint64_t)raw_v[5] << 40);
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
    if (dev_type >= 4 || profile >= 8) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Invalid Profile/Device Type (%u/%u) in SB.", profile, dev_type);
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_BAD_SUPERBLOCK;
    }
    
    uint8_t policy_mask = _dev_policy_lut[dev_type] | _prof_policy_lut[profile];

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

        if (atomic_load(&vol->alloc.used_blocks) < threshold) {
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

                /* Update Orbit Hint in RAM Anchor */
                uint64_t c_idx = block_idx >> 4;
                /* Only store hint if it fits in 2 bits (k <= 3). */
                if (c_idx < 16 && k <= 3) {
                    /* Repurpose reserved padding */
                    uint32_t hints = hn4_le32_to_cpu(anchor->orbit_hints);
                    
                    /* Clear old 2 bits */
                    hints &= ~(0x3 << (c_idx * 2));
                    /* Set new k */
                    hints |= (k << (c_idx * 2));
                    
                    anchor->orbit_hints = hn4_cpu_to_le32(hints);
                }

                break;
            }
        }
    }

    /* Fallback to Horizon (D1.5) if Flux (D1) is saturated */
    if (alloc_res != HN4_OK) {
        if (alloc_res == HN4_ERR_GRAVITY_COLLAPSE) {
            HN4_LOG_CRIT("WRITE_ATOMIC: D1 Full. Trying Horizon...");

            hn4_addr_t horizon_phys_addr = hn4_addr_from_u64(0);
            
            alloc_res = hn4_alloc_horizon(vol, &horizon_phys_addr);

            if (alloc_res == HN4_OK) {
                uint64_t h_val = hn4_addr_to_u64(horizon_phys_addr);

                /*
                 * Alignment Assertion.
                 * The Horizon Allocator must return a sector aligned to the Block boundary.
                 */
                if (h_val % sectors != 0) {
                    HN4_LOG_CRIT("WRITE_ATOMIC: Horizon Misalignment (Sect %llu %% %u != 0)",
                                 (unsigned long long)h_val, sectors);

                    /*
                     * Release reservation.
                     * CONTRACT: hn4_free_block expects Physical Sector LBA.
                     */
                    hn4_free_block(vol, h_val);

                    alloc_res = HN4_ERR_ALIGNMENT_FAIL;
                } else {
                    /* Immediate Unit Conversion */
                    target_lba = h_val / sectors; // Convert Sector -> Block Index

                    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
                    dclass |= HN4_HINT_HORIZON;
                    anchor->data_class = hn4_cpu_to_le64(dclass);

                    /* Check if we need to update Gravity Center (G) */
                    uint64_t stride = (1ULL << M);
                    uint64_t offset = block_idx * stride;

                    /* Use the normalized Block Index for math */
                    if (target_lba >= offset) {
                        uint64_t linear_start = target_lba - offset;
                        
                        /* 
                         * SPEC COMPLIANCE (6.3): Zero Metadata Modification on Disk.
                         * We update the Anchor Gravity Center in RAM immediately.
                         * Persistence is handled asynchronously by Epoch Sync or Unmount.
                         */
                        anchor->gravity_center = hn4_cpu_to_le64(linear_start);
                        
                        /* Mark volume dirty to ensure eventual metadata flush */
                        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);

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
   #ifdef HN4_USE_128BIT
    hn4_u128_t blk_128 = hn4_u128_from_u64(target_lba);
    hn4_addr_t phys_sector = hn4_u128_mul_u64(blk_128, sectors);
#else
    hn4_addr_t phys_sector = hn4_lba_from_sectors(target_lba * sectors);
#endif
    hn4_result_t io_res;

    /* OPTIMIZATION: ZNS ZONE APPEND (Spec 13.2) */
    if (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE) {
        /* 1. Calculate Zone Start LBA */
        uint32_t zone_bytes   = caps->zone_size_bytes;
        uint64_t zone_sectors = zone_bytes / ss;
        uint64_t raw_lba      = hn4_addr_to_u64(phys_sector);
        uint64_t zone_start   = (raw_lba / zone_sectors) * zone_sectors;

        /* 2. CLEAN HAL CALL */
        io_res = hn4_hal_zns_append_sync(
            vol->target_device,
            hn4_addr_from_u64(zone_start),
            io_buf,
            sectors,
            &phys_sector /* [OUT] Updated by Drive with actual LBA */
        );

        if (io_res == HN4_OK) {
            /* 3. REVERSE ENGINEER GRAVITY */
            uint64_t actual_lba_idx = hn4_addr_to_u64(phys_sector) / sectors;

            if (actual_lba_idx != target_lba) {
                
                bool fixed = false;

                /* 
                 * Case A: Genesis Drift (Block 0)
                 * If this is the start of the file, we can shift the Gravity Center (G)
                 * to match the drive's Write Pointer without breaking previous blocks.
                 */
                if (block_idx == 0) {
                    /* 
                     * Calculate new G.
                     * For ZNS, V=1 and Fractal Scale applies.
                     * Formula: LBA = G + (Index * Stride). 
                     * Since Index=0, New_G = Actual_LBA.
                     */
                    uint64_t new_G = actual_lba_idx;
                    
                    txn_anchor.gravity_center = hn4_cpu_to_le64(new_G);
                    
                    /* Update Bitmap: Release 'target', Claim 'actual' */
                    _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);
                    _bitmap_op(vol, actual_lba_idx, BIT_SET, NULL);
                    
                    /* Sync internal state variables */
                    target_lba = actual_lba_idx;
                    
                    HN4_LOG_WARN("ZNS Drift Fixed: G shifted to %llu to match Zone WP.", 
                                 (unsigned long long)new_G);
                    fixed = true;
                }

                if (!fixed) {
                    /* 
                     * Case B: Mid-Stream Drift (Block > 0)
                     * We cannot shift G without breaking Blocks 0..N-1.
                     * This indicates a Zone Hole or unauthorized write. Fatal.
                     */
                    HN4_LOG_CRIT("ZNS Drift Fatal: Mid-file deviation (Blk %llu). Exp %llu Got %llu.",
                                 (unsigned long long)block_idx, 
                                 (unsigned long long)target_lba, 
                                 (unsigned long long)actual_lba_idx);

                    /* Rollback the bitmap claim on the predicted block */
                    _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);
                    
                    /* Mark the ACTUAL block as dirty/used to prevent future collisions */
                    _bitmap_op(vol, actual_lba_idx, BIT_SET, NULL);

                    hn4_hal_mem_free(io_buf);
                    return HN4_ERR_GEOMETRY;
                }
            }
        }
    } else {

        uint32_t retry_sleep = 1000; /* Default 1ms */
        int max_retries = 2;

        if (profile == HN4_PROFILE_GAMING) {
            retry_sleep = 10; /* 10us Aggressive Poll */
            max_retries = 5;  /* Try harder before rescue to prevent stutter */
        } else if (profile == HN4_PROFILE_USB) {
            retry_sleep = 5000;
            max_retries = 3;
        }

        int tries = 0;
        do {
            io_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, phys_sector, io_buf, sectors);
            
            if (io_res != HN4_OK) {
                if (++tries < max_retries) {
                    hn4_hal_micro_sleep(retry_sleep);
                }
            }
        } while (io_res != HN4_OK && tries < max_retries);
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
                        size_t p_off = offsetof(hn4_block_header_t, payload);
                        
                        if (memcmp((uint8_t*)io_buf + p_off, (uint8_t*)rescue_buf + p_off, payload_cap) == 0) {
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

            /* Check ZNS capability before freeing bitmap */
            bool is_zns = (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE);

            /* 
             * On ZNS, any write attempt (even failed) might advance the WP. 
             * We MUST leak the block (keep it marked 'used') to preserve sequentiality.
             */
             if (!is_zns && io_res != HN4_ERR_ATOMICS_TIMEOUT) {
                if (_bitmap_op(vol, target_lba, BIT_CLEAR, NULL) != HN4_OK) {
                    HN4_LOG_CRIT("WRITE_ATOMIC: Bitmap corruption during rollback. PANIC.");
                    atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
                }
            } else {
                /* For ZNS or Timeouts, we leak the block and mark dirty for Scavenger */
                HN4_LOG_CRIT("Leaking Block %llu (ZNS/Timeout) to preserve alignment.", 
                             (unsigned long long)target_lba);
                
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);

                /* 2. Downgrade Silicon Quality (Firmware distress signal) */
                if (vol->quality_mask) {
                    uint64_t w_idx = target_lba / 32;
                    if (((w_idx + 1) * sizeof(uint64_t)) <= vol->qmask_size) {
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
        HN4_LOG_CRIT("WRITE_ATOMIC: Barrier Error %d. Leaking block to prevent corruption.", io_res);
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    /* 9. Metadata Update */

    atomic_thread_fence(memory_order_release);

    uint64_t end_byte = (block_idx * payload_cap) + len;
    uint64_t curr_mass_le = atomic_load((_Atomic uint64_t*)&anchor->mass);
    
    while (1) {
        uint64_t curr_mass_cpu = hn4_le64_to_cpu(curr_mass_le);
        /* If existing size is larger, we don't need to extend */
        if (end_byte <= curr_mass_cpu) break;
        
        uint64_t new_mass_le = hn4_cpu_to_le64(end_byte);
        /* Optimistically extend the file size */
        if (atomic_compare_exchange_weak(
            (_Atomic uint64_t*)&anchor->mass, 
            &curr_mass_le, 
            new_mass_le)) break;
    }

    /* Barrier: Ensure Mass update is visible before we seal the transaction */
    atomic_thread_fence(memory_order_release);

    /* NOW commit the generation to make the transaction valid */
    uint32_t expected_gen_le = hn4_cpu_to_le32(current_gen);
    uint32_t new_gen_le      = hn4_cpu_to_le32((uint32_t)next_gen);

    if (!atomic_compare_exchange_strong(
        (_Atomic uint32_t*)&anchor->write_gen, 
        &expected_gen_le, 
        new_gen_le))
    {
        HN4_LOG_WARN("WRITE_ATOMIC: Race detected. Expected Gen %u, Found %u. Retrying.", 
                     current_gen, hn4_le32_to_cpu(expected_gen_le));

         if (_bitmap_op(vol, target_lba, BIT_CLEAR, NULL) != HN4_OK) {
             /* If we can't free the orphan, we must panic to prevent leak accumulation */
             atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
        }

        /* Cleanup memory before jumping back */
        hn4_hal_mem_free(io_buf);

        /* Backoff and Retry */
        hn4_hal_micro_sleep(100);
        goto retry_transaction;
    }

    uint64_t now_le = hn4_cpu_to_le64(hn4_hal_get_time_ns());
    atomic_store((_Atomic uint64_t*)&anchor->mod_clock, now_le);

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