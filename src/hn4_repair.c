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
         * Force electrons into the floating gates. If we read back immediately
         * without a barrier, we might just be reading the controller's DRAM cache.
         */
        res = hn4_hal_barrier(vol->target_device);

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
            do {
                /* Current state of the 2 bits */
                uint64_t old_state = (old_val >> shift) & 0x3;

                /* LATTICE MONOTONICITY: Toxic is sticky */
                if (old_state == HN4_Q_TOXIC) {
                    /* Silicon is dead. Do not resuscitate. */
                    new_val = old_val;
                    /* Pretend success to exit loop, but result remains ERR_MEDIA_TOXIC */
                    success = true;
                    /* If we thought we repaired it, force fail the result code */
                    if (res == HN4_OK) res = HN4_ERR_MEDIA_TOXIC;
                    break;
                }

                /* Clear the 2 bits for this block */
                uint64_t cleared = old_val & ~(3ULL << shift);

                if (res == HN4_OK) {
                    /* REPAIR SUCCESS: Downgrade to BRONZE (01) */
                    new_val = cleared | (1ULL << shift);
                } else {
                    /* REPAIR FAILURE: Downgrade to TOXIC (00) */
                    new_val = cleared; /* 00 */
                }

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
                HN4_LOG_CRIT("Auto-Medic CAS Starvation on LBA %llu. Marking DEGRADED.", (unsigned long long)lba_val);
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DEGRADED);
                /* Default fail safe */
                res = HN4_ERR_ATOMICS_TIMEOUT;
            }
        }
    }

    /* 5. RESULT & TELEMETRY */
    if (res == HN4_OK) {
        HN4_LOG_WARN("[TRIAGE] HEALED LBA %llu. Downgraded to BRONZE.",
                     (unsigned long long)hn4_addr_to_u64(bad_lba));
        atomic_fetch_add(&vol->stats.heal_count, 1);
        return HN4_OK;
    } else {
        /* Filter logic errors that shouldn't mark media toxic */
        if (res == HN4_ERR_INVALID_ARGUMENT || res == HN4_ERR_GEOMETRY || res == HN4_ERR_NOMEM) {
            return res;
        }

        HN4_LOG_CRIT("[TRIAGE] HEAL FAILED LBA %llu. Code %d. Marked TOXIC.",
                     (unsigned long long)hn4_addr_to_u64(bad_lba), res);

        /* Only increment toxic counter if we actually transitioned state to avoid double-counting */
        if (transition_to_toxic) {
            atomic_fetch_add(&vol->toxic_blocks, 1);
        }

        /* Always return TOXIC if the physical repair failed */
        return HN4_ERR_MEDIA_TOXIC;
    }
}
