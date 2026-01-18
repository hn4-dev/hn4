/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Lazarus Protocol (Undelete)
 * SOURCE:      hn4_undelete.c
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Implements the "Lazarus Protocol" for recovering data from Tombstone anchors.
 *   
 *   PROTOCOL STAGES:
 *   1. SCAN:        Search RAM Cortex for an anchor marked TOMBSTONE matching the path.
 *   2. PULSE CHECK: Verify the physical block (0) still exists and matches ID/CRC.
 *   3. RESURRECT:   Clear TOMBSTONE flag, update Timestamp.
 *   4. COMMIT:      Atomic write to Disk, then atomic update to RAM.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_anchor.h"
#include "hn4_addr.h"
#include "hn4_constants.h"
#include <string.h>
#include <stdatomic.h>

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/**
 * _hn4_undeletek
 * 
 * Implements Spec 18.5 Step 2: The Pulse Check.
 * Determines if the physical data for a Tombstone is still viable on media.
 * 
 * @param vol    Volume Context
 * @param anchor The Tombstone Anchor to verify
 * @return HN4_OK if valid, HN4_ERR_DATA_ROT/ID_MISMATCH/HEADER_ROT otherwise.
 */
static hn4_result_t _hn4_undeletek(hn4_volume_t* vol, hn4_anchor_t* anchor)
{
    /* 1. Extract Physics Parameters */
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);
    
    uint64_t V = 0;
    memcpy(&V, anchor->orbit_vector, 6);
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;

    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    uint64_t lba    = HN4_LBA_INVALID;

    /* 2. Trajectory Resolution (Horizon vs Ballistic) */
    if (dclass & HN4_HINT_HORIZON) {
        lba = G; 
    } 
    else {
        /* Ballistic Scan: Check orbits k=0..3 for valid residency */
        for (uint8_t k = 0; k < 4; k++) {
            uint64_t cand = _calc_trajectory_lba(vol, G, V, 0, M, k);
            
            if (HN4_UNLIKELY(cand == HN4_LBA_INVALID)) continue;
            
            /* Check Allocation Bitmap */
            bool is_set = false;
            hn4_result_t b_res = _bitmap_op(vol, cand, BIT_TEST, &is_set);

            if (HN4_LIKELY(b_res == HN4_OK && is_set)) {
                lba = cand;
                break;
            }
        }
    }

    if (HN4_UNLIKELY(lba == HN4_LBA_INVALID)) {
        HN4_LOG_WARN("Lazarus: Pulse Check Failed. Block 0 reaped or lost.");
        return HN4_ERR_DATA_ROT;
    }

    /* 3. Physical Verification */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    uint32_t bs = vol->vol_block_size;
    uint32_t ss = caps->logical_block_size;
    
    void* buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!buf)) return HN4_ERR_NOMEM;

    hn4_addr_t phys_addr = hn4_lba_from_blocks(lba * (bs / ss));
    
    hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys_addr, buf, (bs / ss));
    
    if (HN4_LIKELY(res == HN4_OK)) {
        hn4_block_header_t* h = (hn4_block_header_t*)buf;
        
        /* A. Identity Check (Anti-Collision) */
        hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
        hn4_u128_t seed_id = hn4_le128_to_cpu(anchor->seed_id);
        
        if (HN4_UNLIKELY(disk_id.lo != seed_id.lo || disk_id.hi != seed_id.hi)) {
            HN4_LOG_WARN("Lazarus: ID Mismatch at LBA %llu", (unsigned long long)lba);
            res = HN4_ERR_ID_MISMATCH;
        } 
        else {
            /* B. Integrity Check (CRC) */
            uint32_t stored_crc = hn4_le32_to_cpu(h->header_crc);
            uint32_t calc_crc   = hn4_crc32(HN4_CRC_SEED_HEADER, h, offsetof(hn4_block_header_t, header_crc));
            
            if (HN4_UNLIKELY(stored_crc != calc_crc)) {
                HN4_LOG_WARN("Lazarus: Header Rot at LBA %llu", (unsigned long long)lba);
                res = HN4_ERR_HEADER_ROT;
            }
        }
    }

    hn4_hal_mem_free(buf);
    return res;
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_undelete(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char* path
)
{
    /* --- PHASE 0: Pre-Flight --- */
  if (HN4_UNLIKELY(!vol || !path || vol->read_only))
    return (!vol || !path) ? HN4_ERR_INVALID_ARGUMENT : HN4_ERR_ACCESS_DENIED;
    
    if (HN4_UNLIKELY(!vol->nano_cortex)) {
        HN4_LOG_CRIT("Lazarus: Cortex Cache unavailable. Scan impossible.");
        return HN4_ERR_HW_IO;
    }

    /* --- PHASE 1: The Search (RAM Scan) --- */
    /* 
     * We scan the Nano-Cortex for any slot marked TOMBSTONE that matches the name.
     * Name match is restricted to Inline Buffer for v1.0.
     */
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
    int64_t found_idx = -1;

    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
    for (size_t i = 0; i < count; i++) {
        hn4_anchor_t* cand = &anchors[i];
        
        /* Atomic load ensures consistent 64-bit read of flags */
        uint64_t dclass = atomic_load((_Atomic uint64_t*)&cand->data_class);
        dclass = hn4_le64_to_cpu(dclass);

        if (!(dclass & HN4_FLAG_TOMBSTONE)) continue;

        /* Inline Name Match */
        char tmp[25];
        memcpy(tmp, cand->inline_buffer, 24);
        tmp[24] = '\0';

        if (strcmp(tmp, path) == 0) {
            found_idx = i;
            break;
        }
    }
    
    /* Create local working copy */
    hn4_anchor_t zombie;
    if (found_idx != -1) {
        /* Use struct assignment instead of memcpy for type safety */
        zombie = anchors[found_idx];
    }
    
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    if (found_idx == -1) return HN4_ERR_NOT_FOUND;

    /* --- PHASE 2: Pulse Check --- */
    /* Ensure the data hasn't been overwritten by the Reaper */
    hn4_result_t pulse = _hn4_undeletek(vol, &zombie);
    if (HN4_UNLIKELY(pulse != HN4_OK)) {
        return pulse;
    }

    /* --- PHASE 3: Resurrection (State Modification) --- */
    uint64_t dclass = hn4_le64_to_cpu(zombie.data_class);
    dclass &= ~HN4_FLAG_TOMBSTONE; 
    zombie.data_class = hn4_cpu_to_le64(dclass);

    /* Update ModClock to prevent immediate reaping if Reaper is active */
    zombie.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    /* --- PHASE 4: Commit to Disk --- */
    if (HN4_UNLIKELY(hn4_write_anchor_atomic(vol, &zombie) != HN4_OK)) {
        return HN4_ERR_HW_IO;
    }

    /* --- PHASE 5: Update RAM Cache --- */
    /* Critical: Check for race conditions before overwriting RAM */
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
    /* Verify ID still matches to prevent Use-After-Realloc */
    if (anchors[found_idx].seed_id.lo == zombie.seed_id.lo) {
        anchors[found_idx] = zombie; /* Struct Assignment */
    } else {
        HN4_LOG_WARN("Lazarus: Race detected. Slot reused during undelete.");
    }
    
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    HN4_LOG_VAL("Lazarus: Resurrected ID", hn4_le128_to_cpu(zombie.seed_id).lo);
    return HN4_OK;
}