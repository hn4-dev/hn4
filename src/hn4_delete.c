/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Entropy Protocol (Deletion)
 * SOURCE:      hn4_delete.c
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Implements the "Soft Delete" logic. Marks files as Tombstones.
 * Physical reclamation is deferred to the Reaper (Scavenger).
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_anchor.h"
#include "hn4_constants.h" /* Added for HN4_LBA_INVALID */

/* Constants from hn4_namespace.c for slot calculation */
#define HN4_NS_HASH_CONST 0xff51afd7ed558ccdULL

/**
 * hn4_delete
 * 
 * Implements Section 18.4: The Entropy Protocol.
 * Performs a "Soft Delete" by marking the Anchor as a Tombstone.
 * 
 * SAFETY:
 * 1. Checks PERM_IMMUTABLE.
 * 2. Updates mod_clock to start the Reaper grace period.
 * 3. Updates both Disk and RAM Cache to prevent "Zombie" reads.
 */
_Check_return_
hn4_result_t hn4_delete(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char* path
)
{
    if (!vol || !path) return HN4_ERR_INVALID_ARGUMENT;
    if (vol->read_only) return HN4_ERR_ACCESS_DENIED;

    hn4_anchor_t anchor;
    
    /* 1. Resolve Target (Standard lookup finds live files) */
    hn4_result_t res = hn4_ns_resolve(vol, path, &anchor);
    if (res != HN4_OK) return res;

    /* 2. Permission Check (Spec 9.4) */
    uint32_t perms = hn4_le32_to_cpu(anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return HN4_ERR_IMMUTABLE;

    /* 3. Mark Tombstone (Atomic State Change) */
    uint64_t dclass = hn4_le64_to_cpu(anchor.data_class);
    dclass |= HN4_FLAG_TOMBSTONE;
    anchor.data_class = hn4_cpu_to_le64(dclass);

    /* Update Clock to NOW (Start Reaper Grace Period) */
    anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    /* 4. Commit to Disk (Atomic Write) */
    /* This persists the Tombstone status to the Cortex D0 region */
    res = hn4_write_anchor_atomic(vol, &anchor);
    if (res != HN4_OK) return res;

    /* 5. Update RAM Cache (Nano-Cortex) */
    /* We must locate the slot in RAM to update the state visible to other threads immediately. */
    if (vol->nano_cortex) {
        hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
        size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
        
        /* Re-calculate hash to find start slot */
        hn4_u128_t seed = hn4_le128_to_cpu(anchor.seed_id);
        uint64_t h = seed.lo ^ seed.hi;
        h ^= (h >> 33); h *= HN4_NS_HASH_CONST; h ^= (h >> 33);
        uint64_t start_slot = h % count;

        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        
        /* 
         * Linear probe from hash start to find the exact slot. 
         * We match on Seed ID.
         */
        bool updated_ram = false;
        for (uint32_t i = 0; i < 1024; i++) {
            uint64_t curr = (start_slot + i) % count;
            
            /* Check if this slot holds our file */
            if (anchors[curr].seed_id.lo == anchor.seed_id.lo && 
                anchors[curr].seed_id.hi == anchor.seed_id.hi) 
            {
                anchors[curr] = anchor; /* Update RAM with Tombstone */
                updated_ram = true;
                break;
            }
            
            /* Stop if we hit an empty slot (Chain broken) */
            if (anchors[curr].seed_id.lo == 0 && anchors[curr].seed_id.hi == 0) break;
        }
        
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        
        /* If we failed to update RAM (rare race), we force a sync or warn */
        if (!updated_ram) {
            HN4_LOG_WARN("Delete: RAM Cache desync for ID %llu", (unsigned long long)seed.lo);
        }
    }

    return HN4_OK;
}