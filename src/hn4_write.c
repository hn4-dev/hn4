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

typedef struct {
    uint32_t retry_sleep_us;
    int      max_retries;
} hn4_write_policy_t;

/* 
 * Write Retry Policies per Profile & Device Physics
 * Index = (Is_Rotational << 3) | Profile_ID
 * Size = 16 (8 Profiles * 2 Device States)
 */
static const hn4_write_policy_t _write_policy_lut[16] = {
    /* --- SSD / SOLID STATE (Index 0-7) --- */
    /* [0] GENERIC */     { 1000, 2 },
    /* [1] GAMING */      { 10,   5 },
    /* [2] AI */          { 1000, 2 },
    /* [3] ARCHIVE */     { 1000, 2 },
    /* [4] PICO */        { 1000, 2 },
    /* [5] SYSTEM */      { 1000, 2 },
    /* [6] USB */         { 5000, 3 },
    /* [7] HYPER_CLOUD */ { 1000, 2 },

    /* --- HDD / ROTATIONAL (Index 8-15) --- */
    /* Optimization: Enforce min 10ms sleep & 5 retries to handle seek latency */
    /* [8] GENERIC */     { 10000, 5 },
    /* [9] GAMING */      { 10000, 5 },
    /* [10] AI */         { 10000, 5 },
    /* [11] ARCHIVE */    { 10000, 5 },
    /* [12] PICO */       { 10000, 5 },
    /* [13] SYSTEM */     { 10000, 5 },
    /* [14] USB */        { 10000, 5 },
    /* [15] HYPER_CLOUD */{ 10000, 5 }
};

/* 
 * WRITE VALIDATION LOOKUP TABLE (4-bit Index)
 * Bits: [3:Immutable] [2:Tombstone] [1:Panic] [0:ReadOnly]
 * 
 * Logic Precedence matches original code:
 * 1. ReadOnly (Bit 0) -> HN4_ERR_ACCESS_DENIED
 * 2. Panic    (Bit 1) -> HN4_ERR_VOLUME_LOCKED
 * 3. Tomb     (Bit 2) -> HN4_ERR_TOMBSTONE
 * 4. Imm      (Bit 3) -> HN4_ERR_IMMUTABLE
 */
static const hn4_result_t _write_check_lut[16] = {
    /* 0000 */ HN4_OK,                  /* All Good */
    /* 0001 */ HN4_ERR_ACCESS_DENIED,   /* RO */
    /* 0010 */ HN4_ERR_VOLUME_LOCKED,   /* Panic */
    /* 0011 */ HN4_ERR_ACCESS_DENIED,   /* Panic + RO (RO wins) */
    /* 0100 */ HN4_ERR_TOMBSTONE,       /* Tomb */
    /* 0101 */ HN4_ERR_ACCESS_DENIED,   /* Tomb + RO */
    /* 0110 */ HN4_ERR_VOLUME_LOCKED,   /* Tomb + Panic */
    /* 0111 */ HN4_ERR_ACCESS_DENIED,   /* Tomb + Panic + RO */
    /* 1000 */ HN4_ERR_IMMUTABLE,       /* Imm */
    /* 1001 */ HN4_ERR_ACCESS_DENIED,   /* Imm + RO */
    /* 1010 */ HN4_ERR_VOLUME_LOCKED,   /* Imm + Panic */
    /* 1011 */ HN4_ERR_ACCESS_DENIED,   /* Imm + Panic + RO */
    /* 1100 */ HN4_ERR_TOMBSTONE,       /* Imm + Tomb (Tomb wins per original logic order?)  */
    /* 1101 */ HN4_ERR_ACCESS_DENIED,   /* All + RO */
    /* 1110 */ HN4_ERR_VOLUME_LOCKED,   /* All + Panic */
    /* 1111 */ HN4_ERR_ACCESS_DENIED    /* All */
};

/* CORRECTION: Use _Static_assert for C11 compliance matching project style */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(sizeof(_write_check_lut) / sizeof(_write_check_lut[0]) == 16,
                  "HN4: Write check LUT size mismatch");
#endif

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
     /* HYPER_CLOUD: 0 = Allow Ballistic Scatter (High IOPS) */
    [HN4_PROFILE_HYPER_CLOUD] = 0 
};

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/* Internal Helper for Spatial Sorting */
typedef struct {
    uint64_t lba;
    uint8_t  k_index;
} hn4_orbit_target_t;

static void _sort_orbits_spatial(hn4_orbit_target_t* targets, int count) {
    static const int gaps[] = {4, 1, 0}; // Gaps for Shell Sort
    
    for (const int* g = gaps; *g > 0; ++g) {
        for (int i = *g; i < count; i++) {
            hn4_orbit_target_t temp = targets[i];
            int j;
            for (j = i; j >= *g && targets[j - *g].lba > temp.lba; j -= *g) {
                targets[j] = targets[j - *g];
            }
            targets[j] = temp;
        }
    }
}


HN4_INLINE void _pack_header(hn4_block_header_t* h, hn4_u128_t id, 
                                uint64_t seq, uint64_t gen, 
                                uint32_t dcrc, uint32_t meta)
{
    h->magic      = hn4_cpu_to_le32(HN4_BLOCK_MAGIC);
    h->well_id    = hn4_cpu_to_le128(id);
    h->seq_index  = hn4_cpu_to_le64(seq);
    h->generation = hn4_cpu_to_le64(gen);
    h->data_crc   = hn4_cpu_to_le32(dcrc);
    h->comp_meta  = hn4_cpu_to_le32(meta);

    /* CRC covers fields up to header_crc */
    uint32_t hcrc = hn4_crc32(HN4_CRC_SEED_HEADER, h, 
                              offsetof(hn4_block_header_t, header_crc));
    h->header_crc = hn4_cpu_to_le32(hcrc);
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
 */
uint64_t _resolve_residency_verified(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* anchor,
    HN4_IN uint64_t  block_idx
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
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);

    hn4_u128_t my_well_id = hn4_le128_to_cpu(anchor->seed_id);

    uint32_t bs = vol->vol_block_size;
    uint64_t current_gen = (uint64_t)hn4_le32_to_cpu(anchor->write_gen);

    /* 2. Allocate Verification Buffer */
    void* check_buf = hn4_hal_mem_alloc(bs);

    if (!check_buf) return HN4_LBA_INVALID;
    
    uint64_t found_lba = HN4_LBA_INVALID;

    /* =====================================================================
     * PATH A: HORIZON LINEAR LOOKUP (Spec 6.4 / 7.3)
     * If the file is flagged as Horizon, data is sequential starting at G.
     * ===================================================================== */
    if (dclass & HN4_HINT_HORIZON) {
            /* Horizon files use V=1 (stride=1) implicitly if M=0, but use M for scaling */
            uint64_t stride_blocks = (1ULL << M);
        
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
            uint32_t spb = vol->vol_block_size / caps->logical_block_size;

            if (block_idx < (UINT64_MAX / stride_blocks)) {
                uint64_t offset_sectors = (block_idx * stride_blocks) * spb;
            
                if ((UINT64_MAX - G) >= offset_sectors) {
                
                    uint64_t linear_lba = G + offset_sectors;
                
                    bool in_bounds = false;
        #ifdef HN4_USE_128BIT
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
     * OPTIMIZATION: Spatial Sort (C-LOOK) for Rotational Media.
     * ===================================================================== */
    
    hn4_orbit_target_t candidates[HN4_ORBIT_LIMIT];
    int valid_count = 0;

    /* 1. Generate Candidates */
    for (uint8_t k = 0; k < HN4_ORBIT_LIMIT; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, block_idx, M, k);
        
        if (lba != HN4_LBA_INVALID) {
            candidates[valid_count].lba = lba;
            candidates[valid_count].k_index = k;
            valid_count++;
        }
    }

    /* 2. Spatial Sort (Only if Rotational) */
    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        _sort_orbits_spatial(candidates, valid_count);
    }

    /* 3. Probe in Physical Order */
    for (int i = 0; i < valid_count; i++) {
        uint64_t lba = candidates[i].lba;
        
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

   /* 1. Pointer & Geometry Checks (Must be explicit) */
    /* We assume block_idx and vol are checked here because computing the index relies on valid pointers */
    if (HN4_UNLIKELY(!vol || !anchor || !data)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    /* Optimization: Hoist the capacity calculation to avoid div if possible, 
       or assume vol->vol_capacity_blocks is cached. If not, calc is necessary. */
    if (HN4_UNLIKELY(block_idx > (UINT64_MAX / vol->vol_block_size))) {
         return HN4_ERR_INVALID_ARGUMENT;
    }

retry_transaction:;

    /* 2. Setup Transaction Copy (moved up to allow flag extraction) */
    hn4_anchor_t txn_anchor;
    memcpy(&txn_anchor, anchor, sizeof(hn4_anchor_t));

    /* 
     * 3. BITWISE STATE FUSION
     * Extract all 4 conditions into a 4-bit integer.
     * We use !!(cond) to normalize to 0 or 1, then shift.
     * This allows the CPU to use pipeline-friendly ALU ops instead of branches.
     */
    
    uint64_t dclass = hn4_le64_to_cpu(txn_anchor.data_class);
    uint32_t perms  = hn4_le32_to_cpu(anchor->permissions);
    
    uint32_t idx = 0;
    
    /* Bit 0: RO */
    idx |= (vol->read_only ? 1 : 0);
    
    /* Bit 1: Panic */
    idx |= ((vol->sb.info.state_flags & HN4_VOL_PANIC) ? 2 : 0);
    
    /* Bit 2: Tombstone */
    idx |= ((dclass & HN4_FLAG_TOMBSTONE) ? 4 : 0);
    
    /* Bit 3: Immutable */
    idx |= ((perms & HN4_PERM_IMMUTABLE) ? 8 : 0);

    /* 4. Single Branch Resolution */
    hn4_result_t res = _write_check_lut[idx];
    
    if (HN4_UNLIKELY(res != HN4_OK)) {
        HN4_LOG_CRIT("WRITE_ATOMIC: Rejected by State Check (Code %d)", res);
        return res;
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
    if (HN4_UNLIKELY(!io_buf)) {
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
        
         if (HN4_UNLIKELY(!thaw_buf)) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw alloc failed. Aborting to prevent data loss.");
            hn4_hal_mem_free(io_buf); /* Clean up the main buffer before return */
            return HN4_ERR_NOMEM;
        }

        hn4_addr_t old_phys = hn4_lba_from_blocks(old_lba * sectors);
        
        hn4_result_t r_res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, old_phys, thaw_buf, sectors);
        if (HN4_UNLIKELY(r_res != HN4_OK)) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw read failed. Aborting.");
            hn4_hal_mem_free(thaw_buf);
            hn4_hal_mem_free(io_buf);
            return r_res;
        }

        hn4_block_header_t* old_hdr = (hn4_block_header_t*)thaw_buf;
        hn4_block_header_t* new_hdr_view = (hn4_block_header_t*)io_buf;

        if (HN4_UNLIKELY(hn4_le32_to_cpu(old_hdr->magic) != HN4_BLOCK_MAGIC)) {
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

        uint32_t old_dcrc = hn4_le32_to_cpu(old_hdr->data_crc);
        uint32_t cal_dcrc = hn4_crc32(HN4_CRC_SEED_DATA, old_hdr->payload, payload_cap);

        if (old_dcrc != cal_dcrc) {
            HN4_LOG_CRIT("WRITE_ATOMIC: Thaw source has Payload Rot (Bit Rot). Aborting.");
            hn4_hal_mem_free(thaw_buf);
            hn4_hal_mem_free(io_buf);
            return HN4_ERR_PAYLOAD_ROT;
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

    /* OPTIMIZATION: 
     * - ARCHIVE: Always try to compress.
     * - HYPER_CLOUD: Never speculate. Only compress if HINT_COMPRESSED is explicitly set.
     *   (Server workloads are often already compressed/encrypted; avoiding the CPU hit boosts IOPS).
     */
     bool try_compress = (dclass & HN4_HINT_COMPRESSED);
    
    if (vol->sb.info.format_profile == HN4_PROFILE_ARCHIVE) {
        try_compress = true;
    }

    /* 
     * SAFETY FIX: Mutual Exclusion.
     * Encrypted data (high entropy) is incompressible. 
     * Furthermore, the Read Path (_validate_block) explicitly rejects blocks 
     * marked as both Encrypted and Compressed to prevent compression-oracle attacks.
     */
    if (dclass & HN4_HINT_ENCRYPTED) {
        try_compress = false;
    }

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

            if (HN4_UNLIKELY(candidate == HN4_LBA_INVALID)) continue;

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

            if (HN4_UNLIKELY(op_res != HN4_OK)) {
                alloc_res = HN4_ERR_BITMAP_CORRUPT;
                break;
            }

            if (HN4_LIKELY(bit_flipped)) {
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
            uint64_t actual_lba_idx;

            #ifdef HN4_USE_128BIT
                /* FIX: 128-bit aware calculation for Quettabyte ZNS */
                hn4_u128_t blk_128 = hn4_u128_div_u64(phys_sector, sectors);
                /* If high part set, we can't map back to u64 block index anyway */
                if (blk_128.hi > 0) actual_lba_idx = HN4_LBA_INVALID; 
                else actual_lba_idx = blk_128.lo;
            #else
                actual_lba_idx = phys_sector / sectors;
            #endif

            uint64_t max_vol_blocks = vol->vol_capacity_bytes / vol->vol_block_size;

            if (actual_lba_idx >= max_vol_blocks) {
                HN4_LOG_CRIT("ZNS Error: Drive returned OOB LBA %llu", (unsigned long long)actual_lba_idx);
                hn4_hal_mem_free(io_buf);
                return HN4_ERR_GEOMETRY;
            }

            if (actual_lba_idx != target_lba) {
                
                bool fixed = false;

                /* 
                 * Case A: Genesis Drift (Block 0)
                 * If this is the start of the file, we can shift the Gravity Center (G)
                 * to match the drive's Write Pointer without breaking previous blocks.
                 */
                /* Case A: Genesis Drift (Block 0) */
                if (block_idx == 0) {
                    /* 
                     * Calculate new G.
                     * For ZNS, V=1 and Fractal Scale applies.
                     * Formula: LBA = G + (Index * Stride). 
                     * Since Index=0, New_G = Actual_LBA.
                     */
                    uint64_t new_G = actual_lba_idx;

                    anchor->gravity_center = hn4_cpu_to_le64(new_G);
                    txn_anchor.gravity_center = hn4_cpu_to_le64(new_G);
                    
                    /* Update Bitmap: Release 'target' */
                    _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);

                    /* 
                     * Ensure the drive didn't append to a block we think is already valid data.
                     */
                    bool collision = false;
                    _bitmap_op(vol, actual_lba_idx, BIT_TEST, &collision);
                    
                    if (HN4_UNLIKELY(collision)) {
                        HN4_LOG_CRIT("ZNS CRITICAL: Drive appended to allocated LBA %llu. State Desync.", 
                                     (unsigned long long)actual_lba_idx);
                        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
                        hn4_hal_mem_free(io_buf);
                        return HN4_ERR_DATA_ROT;
                    }

                    /* Claim 'actual' */
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

                    _bitmap_op(vol, target_lba, BIT_CLEAR, NULL);
                    
                    _bitmap_op(vol, actual_lba_idx, BIT_SET, NULL);
                    atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);

                    hn4_hal_mem_free(io_buf);
                    return HN4_ERR_GEOMETRY;
                }
            }
        }
    } else {

    uint32_t is_rot = (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) ? 1 : 0;
    
    uint32_t idx = (is_rot << 3) | (profile & 0x7);
    uint32_t retry_sleep = _write_policy_lut[idx].retry_sleep_us;

    int max_retries = _write_policy_lut[idx].max_retries;
    int tries = 0;

        do {
            io_res = _hn4_spatial_router(
                vol, 
                HN4_IO_WRITE, 
                phys_sector, 
                io_buf, 
                sectors, 
                hn4_le128_to_cpu(anchor->seed_id)
            );
            
            if (HN4_UNLIKELY(io_res != HN4_OK)) {
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

        if (io_res == HN4_ERR_ATOMICS_TIMEOUT && !(vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE)) {
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
                        hn4_block_header_t* r_hdr = (hn4_block_header_t*)rescue_buf;

                        /* Verify Generation to prevent resurrecting stale phantom blocks */
                        bool gen_match = (hn4_le64_to_cpu(r_hdr->generation) == next_gen);

                        if (gen_match && memcmp((uint8_t*)io_buf + p_off, (uint8_t*)rescue_buf + p_off, payload_cap) == 0) {
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
     bool skip_barrier = false;

    /* NVM Optimization */
    if ((vol->sb.info.hw_caps_flags & HN4_HW_NVM) &&
        (vol->sb.info.hw_caps_flags & HN4_HW_STRICT_FLUSH)) {
        skip_barrier = true;
    }

    /* 
     * HYPER_CLOUD OPTIMIZATION:
     * Server environments typically have battery-backed write caches.
     * We skip per-block FUA/Flush to maximize IOPS. Durability is 
     * deferred to the Journal/Epoch flush (Async Consistency).
     */
    if (vol->sb.info.format_profile == HN4_PROFILE_HYPER_CLOUD) {
        skip_barrier = true;
    }

    if (skip_barrier) {
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

    if (HN4_UNLIKELY(!atomic_compare_exchange_strong(
        (_Atomic uint32_t*)&anchor->write_gen, 
        &expected_gen_le, 
        new_gen_le)))
    {
        HN4_LOG_WARN("WRITE_ATOMIC: Race detected. Expected Gen %u, Found %u. Retrying.", 
                     current_gen, hn4_le32_to_cpu(expected_gen_le));

        bool is_zns = (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE);

        if (!is_zns) {
            /* Standard Block Device: Safe to reuse LBA */
            if (_bitmap_op(vol, target_lba, BIT_CLEAR, NULL) != HN4_OK) {
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
            }
        } else {
            /* ZNS: Cannot rollback Write Pointer. Leak block and mark dirty. 
               The Scavenger/Evacuator will eventually reclaim the hole. */
            HN4_LOG_WARN("ZNS Race: Leaking LBA %llu to preserve WP alignment.", 
                         (unsigned long long)target_lba);
            atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        }

        hn4_hal_mem_free(io_buf);
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
