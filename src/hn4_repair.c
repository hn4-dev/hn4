/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Auto-Medic (Self-Healing Logic)
 * SOURCE:      hn4_repair.c
 * STATUS:      HARDENED / PRODUCTION (v2.5)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ENGINEERING PRINCIPLES:
 *   1. DMA GHOST DEFENSE: Verify buffers are poisoned (0xDD) before read.
 *   2. LATTICE MONOTONICITY: Health state only degrades. Toxic is sticky.
 *   3. BARRIER-FIRST: Flush to NAND before verifying data.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"
#include "hn4_addr.h"
#include <string.h>
#include <stdatomic.h>

#define HN4_MAX_CAS_RETRIES     100
#define HN4_DMA_POISON_BYTE     0xDD

/* 
 * Repair Outcome Categories 
 */
#define HN4_R_OUTCOME_SUCCESS   0
#define HN4_R_OUTCOME_FAILED    1
#define HN4_R_OUTCOME_ABSTAIN   2

/*
 * Quality Mask Transition Matrix
 * Maps [Outcome][Current_State] -> New_State
 *
 * Rules:
 * 1. TOXIC (00) is sticky (Terminal State).
 * 2. SUCCESS downgrades Silver/Gold to BRONZE (01).
 * 3. FAILURE downgrades everything to TOXIC (00).
 */
static const uint8_t _qmask_trans_lut[3][4] = {
    /* [0] SUCCESS: Heal -> Bronze */
    { HN4_Q_TOXIC, HN4_Q_BRONZE, HN4_Q_BRONZE, HN4_Q_BRONZE },
    
    /* [1] FAILED:  Die  -> Toxic  */
    { HN4_Q_TOXIC, HN4_Q_TOXIC,  HN4_Q_TOXIC,  HN4_Q_TOXIC  },
    
    /* [2] ABSTAIN: No Change      */
    { HN4_Q_TOXIC, HN4_Q_BRONZE, HN4_Q_SILVER, HN4_Q_GOLD   }
};

HN4_INLINE int _map_repair_outcome(hn4_result_t res) {
    if (res == HN4_OK) return HN4_R_OUTCOME_SUCCESS;
    
    /* Logic errors (NOMEM/ARGS) should not mark silicon as Toxic */
    if (res == HN4_ERR_NOMEM || res == HN4_ERR_INVALID_ARGUMENT) {
        return HN4_R_OUTCOME_ABSTAIN;
    }
    
    /* HW_IO, DATA_ROT, MEDIA_TOXIC -> Physical Failure */
    return HN4_R_OUTCOME_FAILED;
}

/* =========================================================================
 * CORE REPAIR LOGIC
 * ========================================================================= */

_Check_return_ hn4_result_t hn4_repair_block(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_addr_t    bad_lba,
    HN4_IN const void*   good_data,
    HN4_IN uint32_t      len
)
{
    /* 1. Pre-flight Validation */
    if (!vol || !good_data) return HN4_ERR_INVALID_ARGUMENT;
    if (len == 0) return HN4_OK;

    if (vol->read_only) {
        return HN4_ERR_ACCESS_DENIED;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;

    /*
     * ALIGNMENT SAFETY:
     * We cannot safely repair partial sectors without RMW, which is dangerous
     * on already corrupted media. Require strict sector padding.
     */
    if (len % ss != 0) {
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    uint32_t sectors = len / ss;
    hn4_result_t res;

    /*
     * 2. ATTEMPT OVERWRITE (The Scrub)
     * We write the good data to the bad location.
     */
    res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, bad_lba, (void*)good_data, sectors);

    /*
     * 3. THE WALL (Barrier & Verify)
     * Only verify if the write appeared to succeed.
     */
   if (res == HN4_OK) {
        /*
         * BARRIER ENFORCEMENT:
         * Force electrons into the floating gates.
         * OPTIMIZATION: Skip if NVM (Byte-addressable persistence).
         */
        if (!(vol->sb.info.hw_caps_flags & HN4_HW_NVM)) {
            res = hn4_hal_barrier(vol->target_device);
        }

        if (res == HN4_OK) {
            void* verify_buf = hn4_hal_mem_alloc(len);

            if (verify_buf) {
                /*
                 * DMA GHOST DEFENSE:
                 * Poison the buffer with 0xDD. If the controller "lies" and says
                 * READ_SUCCESS without actually transferring data (DMA Stall),
                 * our memory will still hold 0xDD, causing memcmp to fail.
                 */
                memset(verify_buf, HN4_DMA_POISON_BYTE, len);

                /* Read back from media */
                res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, bad_lba, verify_buf, sectors);

                if (res == HN4_OK) {
                    /* BITWISE IDENTITY CHECK */
                    if (memcmp(good_data, verify_buf, len) != 0) {
                        /*
                         * ZOMBIE BLOCK DETECTED:
                         * The drive said Write OK, Barrier OK, Read OK...
                         * but the data is wrong. The silicon is lying.
                         */
                        res = HN4_ERR_DATA_ROT;
                    }
                }
                hn4_hal_mem_free(verify_buf);
            } else {
                /* If we can't verify, we can't certify the repair. */
                res = HN4_ERR_NOMEM;
            }
        }
    }

    /*
     * 4. SILICON CARTOGRAPHY (Quality Mask Update)
     * We must record the health of this block.
     * Rules:
     * - Success -> Mark BRONZE (Healed but suspicious).
     * - Failure -> Mark TOXIC (Dead).
     * - TOXIC is TERMINAL (Cannot upgrade to Bronze).
     */
    bool transition_to_toxic = false;

    if (vol->quality_mask) {
        uint32_t bs      = vol->vol_block_size;
        
        uint64_t lba_val = hn4_addr_to_u64(bad_lba);

        /* Calculate containment block (Mask bits cover whole blocks) */
        uint64_t spb       = bs / ss;
        uint64_t block_idx = lba_val / spb;
        uint64_t word_idx  = block_idx / 32;

        /* Bounds Check */
        if (((word_idx + 1) * sizeof(uint64_t)) <= vol->qmask_size) {

            uint32_t          shift = (block_idx % 32) * 2;
            _Atomic uint64_t* q_ptr = (_Atomic uint64_t*)&vol->quality_mask[word_idx];

            uint64_t old_val = atomic_load_explicit(q_ptr, memory_order_relaxed);
            uint64_t new_val;
            bool     success = false;
            int      retries = 0;

            /* CAS Loop: Commit the health status */
               int outcome_idx = _map_repair_outcome(res);

            /* CAS Loop: Commit the health status */
            do {
                /* Current state of the 2 bits */
                uint64_t old_state = (old_val >> shift) & 0x3;

                /* O(1) Table Lookup */
                uint64_t next_state = _qmask_trans_lut[outcome_idx][old_state];

                if (next_state == old_state) {
                    /* Optimization: Don't write if state didn't change */
                    new_val = old_val;
                    /* 
                     * Edge Case: If we tried to repair but the block was ALREADY Toxic,
                     * we must force the return code to reflect failure, even if the 
                     * write succeeded physically. Toxic is terminal.
                     */
                    if (old_state == HN4_Q_TOXIC && res == HN4_OK) {
                        res = HN4_ERR_MEDIA_TOXIC;
                    }
                    success = true;
                    break;
                }

                /* Construct new word */
                uint64_t cleared = old_val & ~(3ULL << shift);
                new_val = cleared | (next_state << shift);

                success = atomic_compare_exchange_weak_explicit(q_ptr, &old_val, new_val,
                                                                memory_order_release,
                                                                memory_order_relaxed);

                if (success) {
                    /* Check if we just killed the block */
                    if (res != HN4_OK) {
                        transition_to_toxic = true;
                    }
                }
            } while (!success && ++retries < HN4_MAX_CAS_RETRIES);

            /*
             * CAS STARVATION CHECK:
             * If we spun out 100 times, the memory bus is saturated or locked.
             * This indicates a severe system instability. Mark volume Degraded.
             */
            if (!success) {
                HN4_LOG_CRIT("Auto-Medic CAS Starvation. Marking DEGRADED.");
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DEGRADED);
                
                /* If data was repaired, return OK despite metadata timeout */
                if (res != HN4_OK) {
                    res = HN4_ERR_ATOMICS_TIMEOUT;
                }
            }
        }
    }

    /* 5. RESULT & TELEMETRY */
    if (res == HN4_OK) {
        HN4_LOG_WARN("[TRIAGE] HEALED LBA %llu. Downgraded to BRONZE.",
                     (unsigned long long)hn4_addr_to_u64(bad_lba));
        atomic_fetch_add(&vol->health.heal_count, 1);
        return HN4_OK;
    } else {
        /* Filter logic errors that shouldn't mark media toxic */
        if (res == HN4_ERR_INVALID_ARGUMENT || res == HN4_ERR_GEOMETRY || res == HN4_ERR_NOMEM) {
            return res;
        }

        HN4_LOG_CRIT("[TRIAGE] HEAL FAILED LBA %llu. Code %d. Marked TOXIC.",
                     (unsigned long long)hn4_addr_to_u64(bad_lba), res);

        /* Ensure we only count TOXIC transitions for actual media errors */
        if (transition_to_toxic && res == HN4_ERR_MEDIA_TOXIC) {
            atomic_fetch_add(&vol->health.toxic_blocks, 1);
        }

        /* Always return TOXIC if the physical repair failed */
        return HN4_ERR_MEDIA_TOXIC;
    }
}
