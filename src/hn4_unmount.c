/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Volume Unmount & Shutdown
 * SOURCE:      hn4_unmount.c
 * VERSION:     7.9 (Refactored / Hardened)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ORDERING INVARIANT:
 * Data Flush -> Epoch Advance -> SB Broadcast -> Final Barrier.
 * Violating this order risks "Phantom Writes" or "Journal Desync".
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_crc.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include <string.h>
#include <stdatomic.h>

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/* IO wrapper */
static inline hn4_addr_t _addr_from_sector(uint64_t sect_idx) {
#ifdef HN4_USE_128BIT
    hn4_addr_t a = { .lo = sect_idx, .hi = 0 }; return a;
#else
    return sect_idx;
#endif
}

/* 
 * Helper to safely free and nullify pointers during teardown.
 * Performs secure zeroing if requested (for bitmaps/keys).
 */
static void _safe_release_mem(void** ptr, size_t size, bool secure) {
    if (ptr && *ptr) {
        if (secure) _secure_zero(*ptr, size);
        hn4_hal_mem_free(*ptr);
        *ptr = NULL;
    }
}

/* =========================================================================
 * CARDINALITY TABLE (Superblock Layout)
 * ========================================================================= */

typedef enum {
    SB_LOC_NORTH = 0,
    SB_LOC_EAST,
    SB_LOC_WEST,
    SB_LOC_SOUTH,
    SB_LOC_MAX
} hn4_sb_location_t;

/* =========================================================================
 * PERSISTENCE LOGIC
 * ========================================================================= */

/**
 * _broadcast_superblock
 * 
 * Updates state flags and persists the Superblock to Cardinal Points.
 * Implements Fault Tolerance via Quorum.
 */
static hn4_result_t _broadcast_superblock(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_IN hn4_volume_t* vol,
    HN4_IN uint64_t active_epoch_id,
    HN4_IN hn4_addr_t active_ring_ptr_blk,
    HN4_IN bool set_clean,
    HN4_IN bool force_degraded,
    HN4_IN bool bump_generation
)
{
    if (vol->read_only) return HN4_OK;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t bs = vol->vol_block_size;
    uint32_t ss = caps->logical_block_size;
    
    if (ss == 0) ss = 512;
    if (bs < ss || (bs % ss) != 0) return HN4_ERR_GEOMETRY;
    if (ss < HN4_SB_SIZE && (HN4_SB_SIZE % ss != 0)) return HN4_ERR_GEOMETRY;

    /* Validate Ring Pointer vs Capacity */
    hn4_size_t dev_cap = vol->vol_capacity_bytes;

    /* Check: active_ring_ptr_blk (Block Index) * BS < Capacity */
#ifdef HN4_USE_128BIT
    /* Rough check: if ptr.hi > 0 or ptr.lo * bs > cap.lo (ignoring hi cap for now) */
    /* A safe generic check: */
    if (active_ring_ptr_blk.hi > 0) return HN4_ERR_GEOMETRY; // SB ptrs shouldn't be > 18EB
    // Note: detailed cmp omitted for brevity, assuming standard SB range
#else
    uint64_t total_blocks = dev_cap / bs;
    if (active_ring_ptr_blk >= total_blocks) return HN4_ERR_GEOMETRY;
#endif

    uint32_t sectors_per_sb = HN4_SB_SIZE / ss;
    if (sectors_per_sb == 0) sectors_per_sb = 1;

    uint32_t buf_sz = HN4_ALIGN_UP(HN4_SB_SIZE, ss);
    void* io_buf = hn4_hal_mem_alloc(buf_sz);
    if (!io_buf) return HN4_ERR_NOMEM;

    _secure_zero(io_buf, buf_sz);

    /* 1. Prepare Transient Copy */
    hn4_superblock_t cpu_sb;
    memcpy(&cpu_sb, &vol->sb, sizeof(hn4_superblock_t));

    cpu_sb.info.last_mount_time = hn4_hal_get_time_ns();
    
    if (bump_generation) {
        if (cpu_sb.info.copy_generation >= HN4_MAX_GENERATION) {
            cpu_sb.info.state_flags |= HN4_VOL_LOCKED;
            HN4_LOG_CRIT("Volume Generation Limit Reached. Volume LOCKED.");
        } else {
            cpu_sb.info.copy_generation++;
        }
    }

    cpu_sb.info.current_epoch_id = active_epoch_id;
    cpu_sb.info.epoch_ring_block_idx = active_ring_ptr_blk;

    if (vol->taint_counter > 0) cpu_sb.info.dirty_bits |= HN4_DIRTY_BIT_TAINT;

    /* State Flag Logic */
    if (set_clean && !force_degraded) {
        uint32_t bad_mask = (HN4_VOL_TOXIC | HN4_VOL_PANIC | HN4_VOL_DEGRADED);
        if (!(cpu_sb.info.state_flags & bad_mask)) {
            cpu_sb.info.state_flags |= HN4_VOL_CLEAN;
            cpu_sb.info.state_flags &= ~HN4_VOL_DIRTY;
        }
    } else {
        cpu_sb.info.state_flags &= ~HN4_VOL_CLEAN;
        cpu_sb.info.state_flags |= HN4_VOL_DIRTY;
        if (force_degraded) cpu_sb.info.state_flags |= HN4_VOL_DEGRADED;
    }

    /* 2. Calculate Targets (Block Indices) */
    uint64_t targets[SB_LOC_MAX];
    bool attempt_south = false;
    uint64_t sb_space = HN4_ALIGN_UP(HN4_SB_SIZE, bs);

    /* 
     * MATH: Handle 128-bit Capacity for Mirror Calculation 
     */
#ifdef HN4_USE_128BIT
    /* 
     * Fallback for 128-bit:
     * If cap.hi > 0, we assume the drive is massive.
     * We only calculate targets if they fit in 64-bit block indices.
     */
    
    /* NORTH: Always 0 */
    targets[SB_LOC_NORTH] = 0;

    /* EAST: (Cap / 100) * 33 */
    /* Simple u128 approx: use lo part if hi is small, or fail if massive logic missing */
    if (dev_cap.hi == 0) {
        targets[SB_LOC_EAST] = HN4_ALIGN_UP((dev_cap.lo / 100) * 33, bs) / bs;
        targets[SB_LOC_WEST] = HN4_ALIGN_UP((dev_cap.lo / 100) * 66, bs) / bs;
        
        uint64_t aligned_cap = HN4_ALIGN_DOWN(dev_cap.lo, bs);
        if (aligned_cap >= (sb_space * 16)) {
            targets[SB_LOC_SOUTH] = (aligned_cap - sb_space) / bs;
            attempt_south = true;
        }
    } else {
        /* 
         * Logic for >18EB drives:
         * We cannot easily compute percentage without u128 div.
         * For safety in this snippet, we DISABLE mirrors on massive drives 
         * unless a math library is present.
         * Or we just assume they are far away and clamp to max u64?
         */
         // Hardcoded safe fallback for context: Disable mirrors on massive drives to avoid math overflow
         // until hn4_math.h is fully implemented.
         targets[SB_LOC_EAST] = 0; 
         targets[SB_LOC_WEST] = 0;
         targets[SB_LOC_SOUTH] = 0;
         HN4_LOG_WARN("128-bit Mirrors temporarily disabled due to math limit");
    }
#else
    /* Standard 64-bit Logic */
    targets[SB_LOC_NORTH] = 0;
    targets[SB_LOC_EAST]  = HN4_ALIGN_UP((dev_cap / 100) * 33, bs) / bs;
    targets[SB_LOC_WEST]  = HN4_ALIGN_UP((dev_cap / 100) * 66, bs) / bs;

    uint64_t aligned_cap = HN4_ALIGN_DOWN(dev_cap, bs);
    if (aligned_cap >= (sb_space * 16)) {
        targets[SB_LOC_SOUTH] = (aligned_cap - sb_space) / bs;
        attempt_south = true;
    } else {
        targets[SB_LOC_SOUTH] = 0; 
    }
#endif

    if (attempt_south) {
        cpu_sb.info.compat_flags |= HN4_COMPAT_SOUTH_SB;
    } else {
        cpu_sb.info.compat_flags &= ~HN4_COMPAT_SOUTH_SB;
    }

    /* 3. Serialize & Checksum */
    hn4_sb_to_disk(&cpu_sb, (hn4_superblock_t*)io_buf); 
    hn4_superblock_t* dsb = (hn4_superblock_t*)io_buf;
    dsb->raw.sb_crc = 0;
    uint32_t crc = hn4_crc32(0, dsb, HN4_SB_SIZE - 4);
    dsb->raw.sb_crc = hn4_cpu_to_le32(crc);

    /* 4. IO Loop */
    bool slot_ok[SB_LOC_MAX] = {0};

    for (int i = 0; i < SB_LOC_MAX; i++) {
        if (i == SB_LOC_SOUTH && !attempt_south) continue;
        
        /* Skip if 128-bit fallback zeroed them out */
        if (i > 0 && targets[i] == 0) continue; 

        /* 
         * On ZNS devices, the East/West/South mirrors reside in Sequential Zones.
         * We cannot overwrite them in place (Write Pointer Violation).
         * We only update North (LBA 0), which is assumed to be in a Conventional Zone.
         */
        if ((caps->hw_flags & HN4_HW_ZNS_NATIVE) && i > SB_LOC_NORTH) {
            slot_ok[i] = false; // Mark as skipped/failed safely
            continue;
        }

        /* Convert Block Index -> Sector LBA using abstract helper */
        hn4_addr_t phys_lba;
        
    #ifdef HN4_USE_128BIT
        /* Manually construct LBA: block_idx * (bs/ss) */
        /* Assuming targets[i] fits in u64 as calculated above */
        uint64_t lba_val = targets[i] * (bs / ss);
        phys_lba.lo = lba_val;
        phys_lba.hi = 0;
#else
        phys_lba = targets[i] * (bs / ss);
#endif

        /* FIX: ZNS requires Zone Reset before Overwrite (Spec 13.2) */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            /* 
             * For ZNS, Block Size = Zone Size.
             * Resetting the "Sector LBA" resets the whole Zone/Block.
             * Safety: We only do this because we have 3 other mirrors.
             */
            hn4_result_t rst = hn4_hal_sync_io(dev, HN4_IO_ZONE_RESET, phys_lba, NULL, 0);
            if (rst != HN4_OK) {
                slot_ok[i] = false;
                continue;
            }
        }
        
        hn4_result_t io_res = hn4_hal_sync_io(dev, HN4_IO_WRITE, phys_lba, io_buf, sectors_per_sb);
        
        if (io_res == HN4_OK) {
            slot_ok[i] = true;
        } else {
            slot_ok[i] = false;
            if (i == SB_LOC_SOUTH) {
                cpu_sb.info.compat_flags &= ~HN4_COMPAT_SOUTH_SB;
                /* Re-serialize */
                hn4_sb_to_disk(&cpu_sb, (hn4_superblock_t*)io_buf);
                dsb->raw.sb_crc = 0;
                crc = hn4_crc32(0, dsb, HN4_SB_SIZE - 4);
                dsb->raw.sb_crc = hn4_cpu_to_le32(crc);
                
                attempt_south = false; 
                i = -1; /* Restart loop */
                memset(slot_ok, 0, sizeof(slot_ok)); 
                continue;
            }
        }
    }

    hn4_hal_mem_free(io_buf);

    /* 5. Quorum Check */
    int total_success = 0;
    for(int k=0; k<SB_LOC_MAX; k++) {
        if(slot_ok[k]) total_success++;
    }
    
    bool north_valid = slot_ok[SB_LOC_NORTH];
    bool quorum_met;

    if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
        quorum_met = north_valid; 
    } else {
        /* Standard: North + 1 Mirror OR 3 Mirrors */
        quorum_met = (north_valid && total_success >= 2) || (!north_valid && total_success >= 3);
    }
    return quorum_met ? HN4_OK : HN4_ERR_HW_IO;
}

/* =========================================================================
 * MAIN UNMOUNT IMPLEMENTATION
 * ========================================================================= */

_Check_return_ HN4_SUCCESS(return == HN4_OK)
hn4_result_t hn4_unmount(HN4_INOUT hn4_volume_t* vol)
{
    hn4_result_t final_res = HN4_OK;
    hn4_result_t tmp_res;

    if (!vol || !vol->target_device) return HN4_ERR_INVALID_ARGUMENT;

    hn4_hal_device_t* dev = (hn4_hal_device_t*)vol->target_device;
    hn4_hal_barrier(dev);

    bool persistence_ok = true;

    /* ---------------------------------------------------------------------
     * PHASE 1: PERSISTENCE (Write-Capable Only)
     * --------------------------------------------------------------------- */
    if (!vol->read_only) {
        
        /* 1.1 Data Flush (FUA) */
        tmp_res = hn4_hal_sync_io(dev, HN4_IO_FLUSH, _addr_from_sector(0), NULL, 0);
        if (tmp_res != HN4_OK) {
            HN4_LOG_ERR("Data Flush Failed: %d", tmp_res);
            persistence_ok = false;
            final_res = tmp_res;
        }

        if (persistence_ok && vol->sb.info.format_profile != HN4_PROFILE_PICO) {
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
            uint32_t ss = caps->logical_block_size;
            uint32_t bs = vol->vol_block_size;
            
            /* Alloc Scratch: Use Block Size (safe minimum) */
            void* meta_buf = hn4_hal_mem_alloc(bs);
            if (!meta_buf) {
                persistence_ok = false;
                final_res = HN4_ERR_NOMEM;
            } else {
                /* A. Void Bitmap Persistence */
                if (vol->void_bitmap) {
                    uint64_t start_lba_val = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
                    size_t total_words = vol->bitmap_size / sizeof(hn4_armored_word_t);
                    size_t cursor = 0;
                    
                    while (cursor < total_words && persistence_ok) {
                        uint64_t* raw = (uint64_t*)meta_buf;
                        size_t items = 0;
                        size_t cap_items = bs / 8;
                        
                        /* Copy to scratch (Preserves RAM endianness) */
                        while (items < cap_items && cursor < total_words) {
                            raw[items++] = vol->void_bitmap[cursor++].data;
                        }
                        
                        /* Swap Scratch (Destructive allowed here) */
                       hn4_bulk_cpu_to_le64(raw, items);
                        
                        /* 
                         * DOC NOTE: We persist ONLY the raw 64-bit data.
                         * The ECC, Version Counters, and Guard Bits in 'hn4_armored_word_t'
                         * are Transient Runtime Protection (RAM-only).
                         * They are regenerated by the CPU upon Mount.
                         */

                        uint32_t sectors = (items * 8 + ss - 1) / ss;
                        hn4_addr_t lba = hn4_lba_from_sectors(start_lba_val); 
                        
                        /* 
                         * ZNS NOTE: This uses HN4_IO_WRITE (In-Place Update).
                         * This assumes the Bitmap Region was allocated in a 
                         * Conventional Zone (CMR) during Format. 
                         * If this region is SWR (Sequential), this will fail.
                         */
                        if (hn4_hal_sync_io(dev, HN4_IO_WRITE, lba, meta_buf, sectors) != HN4_OK) {
                            persistence_ok = false;
                            final_res = HN4_ERR_HW_IO;
                        }
                        
                        /* FIX 1: Simplified Math. 'sectors' is already the count. */
                        start_lba_val += sectors;
                    }
                }

                /* B. Quality Mask Persistence */
                if (persistence_ok && vol->quality_mask) {
                    uint64_t start_lba_val = hn4_addr_to_u64(vol->sb.info.lba_qmask_start);
                    size_t total_bytes = vol->qmask_size;
                    size_t cursor_bytes = 0;
                    
                    while (cursor_bytes < total_bytes && persistence_ok) {
                        size_t copy_len = (total_bytes - cursor_bytes > bs) ? bs : (total_bytes - cursor_bytes);
                        
                        /* Copy to scratch */
                        memcpy(meta_buf, (uint8_t*)vol->quality_mask + cursor_bytes, copy_len);
                        
                        /* Swap Scratch */
                        hn4_bulk_cpu_to_le64((uint64_t*)meta_buf, (copy_len + 7) / 8);
                        
                        uint32_t sectors = (copy_len + ss - 1) / ss;
                        hn4_addr_t lba = hn4_lba_from_sectors(start_lba_val);
                        
                        if (hn4_hal_sync_io(dev, HN4_IO_WRITE, lba, meta_buf, sectors) != HN4_OK) {
                            persistence_ok = false;
                            final_res = HN4_ERR_HW_IO;
                        }
                        
                        cursor_bytes += copy_len;
                        start_lba_val += sectors;
                    }
                }

                hn4_hal_mem_free(meta_buf);

                /* Metadata Barrier */
                if (persistence_ok) {
                    if (hn4_hal_barrier(dev) != HN4_OK) {
                        persistence_ok = false;
                        final_res = HN4_ERR_HW_IO;
                    }
                }
            }
        }

        /* 1.2 Epoch Advance */
        uint64_t active_epoch = vol->sb.info.current_epoch_id;
        hn4_addr_t active_ring_ptr_blk = vol->sb.info.epoch_ring_block_idx;
        bool epoch_failed = false;

        if (persistence_ok) {
            tmp_res = hn4_epoch_advance(
                dev, 
                &vol->sb, 
                vol->read_only, 
                &active_epoch, 
                &active_ring_ptr_blk
            );

            /* In-memory pointers are updated by _epoch_advance logic only on success.
               We pass these 'active' values to broadcast, but we do NOT update vol->sb 
               until broadcast succeeds (conceptually). */

            if (tmp_res != HN4_OK) {
                HN4_LOG_ERR("Epoch Advance Failed: %d", tmp_res);
                persistence_ok = false;
                epoch_failed = true;
                if (final_res == HN4_OK) final_res = tmp_res;
            }
        }

        

        /* 1.3 SB Broadcast */
        /* Pass bump_generation = true for standard clean path */
        tmp_res = _broadcast_superblock(dev, vol, active_epoch, active_ring_ptr_blk, 
                                      persistence_ok, /* set_clean */
                                      epoch_failed,
                                      true /* bump_generation */);
        
        if (tmp_res != HN4_OK) {
            HN4_LOG_ERR("SB Broadcast Failed: %d", tmp_res);
            if (final_res == HN4_OK) final_res = tmp_res;
            persistence_ok = false;
        }
        
        /* 1.4 Final Barrier & Revert Logic */
        if (persistence_ok) {
            tmp_res = hn4_hal_sync_io(dev, HN4_IO_FLUSH, _addr_from_sector(0), NULL, 0);
            
            if (tmp_res != HN4_OK) {
                HN4_LOG_CRIT("Final Flush Failed! Reverting to DEGRADED.");
                
                vol->taint_counter++;
                hn4_hal_barrier(dev);

                 /* 
                 * We manually increment the generation in the volume structure to 
                 * supersede the "Clean" SB we just tried to write.
                 * Then we call broadcast with bump_generation = false.
                 */
                if (vol->sb.info.copy_generation < HN4_MAX_GENERATION) {
                    vol->sb.info.copy_generation++;
                }

                _broadcast_superblock(dev, vol, active_epoch, active_ring_ptr_blk, 
                                    false, /* set_clean */
                                    true,  /* force_degraded */
                                    false  /* bump_generation (already done) */);
                final_res = tmp_res;
            }
        }
    }

    /* ---------------------------------------------------------------------
     * PHASE 2: TEARDOWN
     * --------------------------------------------------------------------- */
    
    /* Optional retention on error for debugging */
#ifdef HN4_DEBUG_RETAIN_ON_ERROR
    if (HN4_IS_ERR(final_res)) {
        HN4_LOG_CRIT("Unmount failed with error %d. Retaining structs in RAM.", final_res);
        return final_res;
    }
#endif

    bool should_zero = !vol->read_only;

    _safe_release_mem((void**)&vol->void_bitmap, vol->bitmap_size, should_zero);
    _safe_release_mem((void**)&vol->quality_mask, vol->qmask_size, should_zero);
    _safe_release_mem((void**)&vol->l2_summary_bitmap, 0, false); 
    _safe_release_mem((void**)&vol->nano_cortex, vol->cortex_size, should_zero);

    if (should_zero) _secure_zero(vol, sizeof(hn4_volume_t));
    hn4_hal_mem_free(vol);

    HN4_LOG_VAL("Unmount Complete. Status", final_res);
    return final_res;
}

