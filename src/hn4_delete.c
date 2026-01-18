/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Entropy Protocol (Deletion)
 * SOURCE:      hn4_delete.c
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Implements the "Entropy Protocol" for soft-deletion of Anchors.
 *
 *   PROTOCOL STAGES:
 *   1. RESOLVE:     Locate the target anchor via Namespace.
 *   2. VALIDATE:    Check for WORM (Immutable) constraints.
 *   3. MARK:        Set HN4_FLAG_TOMBSTONE and update timestamp.
 *   4. COMMIT:      Atomic write to Disk (Cortex D0).
 *   5. SYNC:        Atomic update to RAM Cache (Nano-Cortex).
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_anchor.h"
#include "hn4_constants.h"
#include <string.h>

/* Constants from hn4_namespace.c for slot calculation */
#define HN4_NS_HASH_CONST 0xff51afd7ed558ccdULL

_Check_return_
hn4_result_t hn4_delete(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char* path
)
{    
    /* --- PHASE 0: Pre-Flight --- */
   if (HN4_UNLIKELY(!vol || !path || vol->read_only))
    return (!vol || !path) ? HN4_ERR_INVALID_ARGUMENT : HN4_ERR_ACCESS_DENIED;

    hn4_anchor_t anchor;
    
    /* --- PHASE 1: Resolution --- */
    /* Standard lookup finds live files. Returns NOT_FOUND if already deleted. */
    hn4_result_t res = hn4_ns_resolve(vol, path, &anchor);
    if (HN4_UNLIKELY(res != HN4_OK)) return res;
    
    /* --- PHASE 2: Policy Check --- */
    /* Spec 9.4: Immutable files cannot be deleted */
    uint32_t perms = hn4_le32_to_cpu(anchor.permissions);
    
    if (HN4_UNLIKELY(perms & HN4_PERM_IMMUTABLE)) {
        return HN4_ERR_IMMUTABLE;
    }

    /* --- PHASE 3: State Transition (Entropy) --- */
    uint64_t dclass = hn4_le64_to_cpu(anchor.data_class);
    
    dclass |= HN4_FLAG_TOMBSTONE;
    anchor.data_class = hn4_cpu_to_le64(dclass);
    
    /* Update Clock to NOW (Start Reaper Grace Period) */
    anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());
    
    /* --- PHASE 4: Persistence (Disk) --- */
    /* Atomic RMW on the Cortex sector. This updates anchor.checksum. */
    res = hn4_write_anchor_atomic(vol, &anchor);
    if (HN4_UNLIKELY(res != HN4_OK)) return res;

    /* --- PHASE 5: Cache Coherency (RAM) --- */
    if (vol->nano_cortex) {
        hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
        size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
        
        /* Re-calculate hash to find start slot (Determinism) */
        hn4_u128_t seed = hn4_le128_to_cpu(anchor.seed_id);
        uint64_t h = seed.lo ^ seed.hi;
        
        /* Mixer (Spec 3.1) */
        h ^= (h >> 33); 
        h *= HN4_NS_HASH_CONST; 
        h ^= (h >> 33);
        
        uint64_t start_slot = h % count;
        bool updated_ram = false;

        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        
        /* Linear probe to find the exact slot matching ID */
        for (uint32_t i = 0; i < 1024; i++) {
            uint64_t curr = (start_slot + i) % count;
            
            /* Check Identity Match */
            if (anchors[curr].seed_id.lo == anchor.seed_id.lo && 
                anchors[curr].seed_id.hi == anchor.seed_id.hi) 
            {
                /* 
                 * FOUND: Update RAM with Tombstone state.
                 * Note: 'anchor' holds the updated CRC from hn4_write_anchor_atomic.
                 */
                anchors[curr] = anchor; 
                updated_ram = true;
                break;
            }
            
            /* Stop at Wall (Empty Slot) */
            if (anchors[curr].seed_id.lo == 0 && anchors[curr].seed_id.hi == 0) {
                break;
            }
        }
        
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        
        /* If we wrote to disk but missed RAM, we have a desync */
        if (HN4_UNLIKELY(!updated_ram)) {
            HN4_LOG_WARN("Delete: RAM Cache desync for ID %llu", (unsigned long long)seed.lo);
        }
    }

    return HN4_OK;
}