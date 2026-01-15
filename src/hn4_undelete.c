/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Lazarus Protocol (Undelete)
 * SOURCE:      hn4_undelete.c
 * STATUS:      PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
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

/*
 * _hn4_undeletek
 * Implements Spec 18.5 Step 2: The Pulse Check (Integrity Verify).
 * Checks if the physical data at Block 0 matches the Anchor ID and CRC.
 */
static hn4_result_t hn4_undeletek(hn4_volume_t* vol, hn4_anchor_t* anchor)
{
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
    uint64_t V = 0;
    memcpy(&V, anchor->orbit_vector, 6);
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);
    
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    uint64_t lba = HN4_LBA_INVALID;

    /* 1. Calculate Physical Location of Block 0 */
    if (dclass & HN4_HINT_HORIZON) {
        lba = G; 
    } else {
        /* Ballistic Scan: Check k=0..3 to find where Block 0 landed */
        for (uint8_t k = 0; k < 4; k++) {
            uint64_t cand = _calc_trajectory_lba(vol, G, V, 0, M, k);
            if (cand == HN4_LBA_INVALID) continue;
            
            bool is_set = false;
            /* We check if the block is allocated. Even if deleted, the reaper might not have cleared it yet. */
            /* If reaper ran, is_set will be false, and undelete fails (Correct behavior). */
            if (_bitmap_op(vol, cand, BIT_TEST, &is_set) == HN4_OK && is_set) {
                lba = cand;
                break;
            }
        }
    }

    if (lba == HN4_LBA_INVALID) return HN4_ERR_DATA_ROT;

    /* 2. Physical Verification */
    uint32_t bs = vol->vol_block_size;
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return HN4_ERR_NOMEM;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    hn4_addr_t phys = hn4_lba_from_blocks(lba * (bs / ss));
    
    hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, buf, (bs / ss));
    
    if (res == HN4_OK) {
        hn4_block_header_t* h = (hn4_block_header_t*)buf;
        
        /* A. Identity Check (Is this MY block?) */
        hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
        hn4_u128_t seed_id = hn4_le128_to_cpu(anchor->seed_id);
        
        if (disk_id.lo != seed_id.lo || disk_id.hi != seed_id.hi) {
            res = HN4_ERR_ID_MISMATCH;
        } else {
            /* B. Integrity Check (Spec 18.5) */
            uint32_t stored_crc = hn4_le32_to_cpu(h->header_crc);
            uint32_t calc_crc = hn4_crc32(HN4_CRC_SEED_HEADER, h, offsetof(hn4_block_header_t, header_crc));
            
            if (stored_crc != calc_crc) {
                res = HN4_ERR_HEADER_ROT;
            }
        }
    }

    hn4_hal_mem_free(buf);
    return res;
}

_Check_return_
hn4_result_t hn4_undelete(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char* path
)
{
    if (!vol || !path) return HN4_ERR_INVALID_ARGUMENT;
    if (vol->read_only) return HN4_ERR_ACCESS_DENIED;
    if (!vol->nano_cortex) return HN4_ERR_HW_IO; /* RAM Cache required for Scan */

    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
    int64_t found_idx = -1;

    /* 
     * 1. Search RAM for Tombstone 
     * Note: This scan matches 'path' against 'inline_buffer'. 
     * It supports Short Names or ID strings if stored inline.
     */
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
    for (size_t i = 0; i < count; i++) {
        hn4_anchor_t* cand = &anchors[i];
        
        /* Spec 18.5 Step 1: Scan for HN4_FLAG_TOMBSTONE */
        uint64_t dclass = atomic_load((_Atomic uint64_t*)&cand->data_class);
        dclass = hn4_le64_to_cpu(dclass);

        if (!(dclass & HN4_FLAG_TOMBSTONE)) continue;

        char tmp[25];
        memcpy(tmp, cand->inline_buffer, 24);
        tmp[24] = '\0';

        if (strcmp(tmp, path) == 0) {
            found_idx = i;
            break;
        }
    }
    
    /* Make a local copy to work on */
    hn4_anchor_t zombie;
    if (found_idx != -1) memcpy(&zombie, &anchors[found_idx], sizeof(hn4_anchor_t));
    
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    if (found_idx == -1) return HN4_ERR_NOT_FOUND;

    /* 2. The Pulse Check (Integrity Verify) */
    hn4_result_t pulse = hn4_undelete(vol, &zombie);
    if (pulse != HN4_OK) {
        HN4_LOG_WARN("Undelete Failed: Pulse Check Error %d", pulse);
        return pulse;
    }

    /* 3. Resurrection (State Change) */
    uint64_t dclass = hn4_le64_to_cpu(zombie.data_class);
    dclass &= ~HN4_FLAG_TOMBSTONE; 
    zombie.data_class = hn4_cpu_to_le64(dclass);

    /* Update Timestamp to prevent immediate Reaping if reaper runs */
    zombie.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    /* 4. Commit to Disk */
    if (hn4_write_anchor_atomic(vol, &zombie) != HN4_OK) return HN4_ERR_HW_IO;

    /* 5. Update RAM */
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    /* Verify ID still matches (Race protection) */
    if (anchors[found_idx].seed_id.lo == zombie.seed_id.lo) {
        memcpy(&anchors[found_idx], &zombie, sizeof(hn4_anchor_t));
    }
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    HN4_LOG_VAL("Lazarus Protocol Successful. Revived ID", zombie.seed_id.lo);
    return HN4_OK;
}