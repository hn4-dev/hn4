/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      The Scavenger (Background Optimization & GC)
 * SOURCE:      hn4_scavenger.c
 * STATUS:      HARDENED / PRODUCTION (v3.0)
 *
 * ROLES:
 * 1. THE REAPER:   Collects "Eclipsed" blocks and Tombstones for batched TRIM.
 * 2. THE MEDIC:    Performs "Osteoplasty" and "Re-Ballistification" on brittle files.
 * 3. THE EVACUATOR: Packs ZNS zones and handles sequential defragmentation.
 * 4. THE STITCHER: Maintains Hyper-Stream skip lists for D2 Horizon logs.
 *
 * SAFETY INVARIANT:
 * - Scavenger operates in "Stealth Mode" (Nice 19).
 * - Yields immediately if HAL Queue Depth > 1.
 * - Uses Delta Table for zero-lock migration.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_anchor.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include "hn4_swizzle.h"
#include <string.h>
#include <stdatomic.h>


/* =========================================================================
 * CONFIGURATION & THRESHOLDS
 * ========================================================================= */

#define HN4_REAPER_GRACE_NS         (24ULL * 3600ULL * 1000000000ULL) /* 24 Hours */
#define HN4_REAPER_BATCH_SIZE       256                               /* Max blocks per DISCARD */
#define HN4_OSTEOPOROSIS_THRESHOLD  50                                /* Collisions before Medic wakes */
#define HN4_BONE_DENSITY_CRITICAL   8                                 /* K-depth considered "Brittle" */
#define HN4_STREAM_SKIP_DIST        1024                              /* Hyper-Stream Interval */
#define HN4_ZNS_VICTIM_THRESHOLD    80                                /* % Invalid before Zone Reset */

/* =========================================================================
 * DELTA TABLE (ZERO-LOCK MIGRATION)
 * ========================================================================= */

/* 
 * A simple lock-free hash map for redirecting reads while the Scavenger works.
 * Size must be power of 2. 
 */
#define HN4_DELTA_TABLE_SIZE 1024 

typedef struct {
    _Atomic uint64_t old_lba; /* Key */
    _Atomic uint64_t new_lba; /* Value */
    _Atomic uint32_t version;
} hn4_delta_entry_t;

static hn4_delta_entry_t _delta_table[HN4_DELTA_TABLE_SIZE];

void hn4_scavenger_init_delta_table(void) {
    for (int i = 0; i < HN4_DELTA_TABLE_SIZE; i++) {
        atomic_init(&_delta_table[i].old_lba, 0);
        atomic_init(&_delta_table[i].new_lba, 0);
        atomic_init(&_delta_table[i].version, 0);
    }
}

/* Called by Reader (Hot Path) */
uint64_t hn4_scavenger_lookup_delta(uint64_t logical_lba, uint32_t req_version) {
    uint64_t idx = logical_lba & (HN4_DELTA_TABLE_SIZE - 1);

    /* Load Key first (Acquire) */
    uint64_t key = atomic_load_explicit(&_delta_table[idx].old_lba, memory_order_acquire);

    if (key == logical_lba) {
        /* Check Version */
        uint32_t ver = atomic_load_explicit(&_delta_table[idx].version, memory_order_relaxed);
        if (ver == req_version) {
            return atomic_load_explicit(&_delta_table[idx].new_lba, memory_order_relaxed);
        }
    }
    return 0;
}

/* Called by Scavenger */
static void _register_delta(uint64_t old_lba, uint64_t new_lba, uint32_t version) {
    uint64_t idx = old_lba & (HN4_DELTA_TABLE_SIZE - 1);

    uint64_t existing = atomic_load_explicit(&_delta_table[idx].old_lba, memory_order_acquire);
    if (existing != 0 && existing != old_lba) return;

    /* Store Value & Version */
    atomic_store_explicit(&_delta_table[idx].new_lba, new_lba, memory_order_release);
    atomic_store_explicit(&_delta_table[idx].version, version, memory_order_release);

    /* Publish Key */
    atomic_store_explicit(&_delta_table[idx].old_lba, old_lba, memory_order_release);
}

static void _clear_delta(uint64_t old_lba) {
    uint64_t idx = old_lba & (HN4_DELTA_TABLE_SIZE - 1);

    /* Check if the slot actually belongs to us before wiping */
    uint64_t key = atomic_load(&_delta_table[idx].old_lba);

    if (key == old_lba) {
        /* 
         * ORDERING:
         * 1. Clear Key (old_lba) with RELEASE. 
         *    This immediately invalidates the lookup for readers.
         * 2. Clear Payload (new_lba, version) with RELAXED.
         *    Once the key is gone, these are unreachable garbage.
         */
        atomic_store_explicit(&_delta_table[idx].old_lba, 0, memory_order_release);

        /* Wipe payload to prevent ABA/Ghost leaks on reuse */
        atomic_store_explicit(&_delta_table[idx].new_lba, 0, memory_order_relaxed);
        atomic_store_explicit(&_delta_table[idx].version, 0, memory_order_relaxed);
    }
}


/* =========================================================================
 * REAPER CONTEXT (BATCHED TRIM)
 * ========================================================================= */

typedef struct {
    hn4_addr_t lbas[HN4_REAPER_BATCH_SIZE];
    uint32_t count;
    uint32_t block_size;
    bool     secure_shred; /* HN4_FLAG_SHRED support */
} _reaper_batch_t;

/*
 * _reaper_flush
 * Executes the physical destruction of data followed by logical release.
 *
 * SAFETY INVARIANT:
 * We MUST issue the DISCARD/WRITE and wait for the BARRIER before calling
 * hn4_free_block(). If we free the block first, an Allocator on another thread
 * could claim the block and write new data to it, which we would then
 * immediately wipe out with our delayed DISCARD command.
 */
static void _reaper_flush(hn4_hal_device_t* dev, _reaper_batch_t* batch, hn4_volume_t* vol) {
    if (batch->count == 0) return;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    
    if (caps->queue_count > 1) { 
        hn4_hal_micro_sleep(100); 
    }

    uint32_t ss = caps->logical_block_size;
    uint32_t sectors_per_blk = batch->block_size / ss;
    void* zero_buf = NULL;

    if (batch->secure_shred) {
        zero_buf = hn4_hal_mem_alloc(batch->block_size);
        if (zero_buf) {
            _secure_zero(zero_buf, batch->block_size);
        } else {
            HN4_LOG_WARN("Reaper: OOM during Secure Shred. Falling back to DISCARD.");
        }
    }

    /* --- PHASE 1: PHYSICAL SANITIZATION (The Hardware Commit) --- */
    for (uint32_t i = 0; i < batch->count; i++) {
        hn4_addr_t phys = batch->lbas[i];
        
        if (batch->secure_shred && zero_buf) {
            /* Overwrite with Zeros */
            hn4_hal_sync_io(dev, HN4_IO_WRITE, phys, zero_buf, sectors_per_blk);
        } else {
            /* Standard Trim/Unmap */
            hn4_hal_sync_io(dev, HN4_IO_DISCARD, phys, NULL, sectors_per_blk);
        }
    }

    /* --- PHASE 2: THE WALL (Barrier) --- */
    hn4_hal_barrier(dev);

    /* --- PHASE 3: LOGICAL RELEASE (The Bitmap Update) --- */
    for (uint32_t i = 0; i < batch->count; i++) {
        hn4_free_block(vol, batch->lbas[i]);
    }

    if (zero_buf) hn4_hal_mem_free(zero_buf);
    batch->count = 0;
}



static void _reaper_add(hn4_volume_t* vol, _reaper_batch_t* batch, hn4_addr_t phys_sector_lba) {
    /*
     * PICO Profile Exception:
     * Embedded devices often lack RAM for batching or threading. 
     * We perform immediate synchronous free.
     */
    if (vol->sb.info.format_profile == HN4_PROFILE_PICO) {
        hn4_free_block(vol, phys_sector_lba);
        return;
    }

    /* Standard Batching */
    if (batch->count < HN4_REAPER_BATCH_SIZE) {
        batch->lbas[batch->count++] = phys_sector_lba;
    }
    
    /* Flush if full */
    if (batch->count >= HN4_REAPER_BATCH_SIZE) {
        _reaper_flush(vol->target_device, batch, vol);
        hn4_hal_poll(vol->target_device);
    }
}



/* =========================================================================
 * TASK 1: THE REAPER (TOMBSTONE CLEANUP)
 * ========================================================================= */

static hn4_result_t _reap_tombstone(
    HN4_IN hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN hn4_time_t now,
    HN4_INOUT _reaper_batch_t* batch
)
{
    /* 1. Validation & Grace Period Check */
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    
    if (!(dclass & HN4_FLAG_TOMBSTONE) || !(dclass & HN4_FLAG_VALID)) return HN4_OK; 

    hn4_time_t death_time = (hn4_time_t)hn4_le64_to_cpu(anchor->mod_clock);
    
    /* Ignore if not yet decayed */
    if ((now - death_time) < HN4_REAPER_GRACE_NS) return HN4_OK; 

    /* 
     * 2. Batch Policy Enforcement
     * Check if the current file requires Secure Shredding.
     * If the batch already contains items with a DIFFERENT policy, flush it now.
     * We cannot mix TRIM and OVERWRITE in the same batch context.
     */
    bool needs_shred = (dclass & HN4_FLAG_SHRED) ? true : false;

    if (batch->count > 0 && batch->secure_shred != needs_shred) {
        _reaper_flush(vol->target_device, batch, vol);
    }
    batch->secure_shred = needs_shred;

    /* 
     * 3. Resource Allocation (Fail-Safe)
     * We need a buffer to verify block ownership before freeing.
     * If we can't allocate this, we must abort BEFORE destroying the anchor
     * to prevent block leaks (Orphaned blocks with no Anchor).
     */
    uint32_t bs = vol->vol_block_size;
    void* vbuf = hn4_hal_mem_alloc(bs);
    if (!vbuf) return HN4_ERR_NOMEM;

    /* 
     * 4. Snapshot Metadata (The "Will")
     * Save the geometry parameters to the stack so we can calculate
     * block locations after the Anchor is destroyed.
     */
    hn4_anchor_t saved_anchor;
    memcpy(&saved_anchor, anchor, sizeof(hn4_anchor_t));

    /* 
     * 5. ATOMIC DESTRUCTION (The Commit)
     */

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;

    uintptr_t base_addr = (uintptr_t)vol->nano_cortex;
    uintptr_t curr_addr = (uintptr_t)anchor;
    
    if (curr_addr < base_addr || curr_addr >= base_addr + vol->cortex_size) {
        hn4_hal_mem_free(vbuf);
        return HN4_ERR_INTERNAL_FAULT;
    }

    uint64_t offset_bytes = (uint64_t)(curr_addr - base_addr);
    
    /* Calculate physical LBA for the sector containing this anchor */
    hn4_addr_t write_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, offset_bytes / ss);
    uint32_t offset_in_sector = offset_bytes % ss;

    /* 
     * Read-Modify-Write the sector to zero out ONLY this anchor.
     * We cannot use hn4_write_anchor_atomic because we are zeroing the ID,
     * which would hash to the wrong slot if we used the standard API.
     */
    void* sector_buf = hn4_hal_mem_alloc(ss);
    if (!sector_buf) {
        hn4_hal_mem_free(vbuf);
        return HN4_ERR_NOMEM;
    }

    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, write_lba, sector_buf, 1) != HN4_OK) {
        hn4_hal_mem_free(vbuf);
        hn4_hal_mem_free(sector_buf);
        return HN4_ERR_HW_IO;
    }

    /* Zero the anchor in the sector buffer */
    memset((uint8_t*)sector_buf + offset_in_sector, 0, sizeof(hn4_anchor_t));

    /* Commit to disk */
    hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, write_lba, sector_buf, 1);
    
    /* Zero the anchor in RAM immediately to reflect disk state */
    if (res == HN4_OK) {
        _secure_zero(anchor, sizeof(hn4_anchor_t));
    }

    hn4_hal_mem_free(sector_buf);

    /* If writing the zero-anchor failed, we abort. The file remains "Deleted-but-visible". */
    if (res != HN4_OK) {
        hn4_hal_mem_free(vbuf);
        return res;
    }

    /* 
     * 6. EXECUTE SCAVENGING (The Cleanup)
     * Now that the Anchor is dead, it is safe to release the blocks back to the pool.
     */
    uint64_t saved_dclass = hn4_le64_to_cpu(saved_anchor.data_class);

    /* Nano Object Handling */
    if (saved_dclass & HN4_FLAG_NANO) {
        /* 
         * Nano objects reside in Cortex slots or inline buffers.
         * By zeroing the anchor (Step 5), we have effectively freed the storage.
         * The Cortex Allocator treats zeroed slots as free space.
         * No bitmap manipulation required.
         */
        hn4_hal_mem_free(vbuf);
        return HN4_OK;
    } 
    
    /* Standard / Horizon Object Handling */
    uint32_t sectors_per_blk = bs / ss;
    uint32_t payload_sz = HN4_BLOCK_PayloadSize(bs);
    
    uint64_t mass = hn4_le64_to_cpu(saved_anchor.mass);
    uint64_t blocks_needed = (mass + payload_sz - 1) / payload_sz;
    
    uint64_t G = hn4_le64_to_cpu(saved_anchor.gravity_center);
    uint64_t V = 0; 
    memcpy(&V, saved_anchor.orbit_vector, 6); 
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    
    uint16_t M = hn4_le16_to_cpu(saved_anchor.fractal_scale);
    hn4_u128_t target_id = hn4_le128_to_cpu(saved_anchor.seed_id);
    
    bool is_horizon = (saved_dclass & HN4_HINT_HORIZON);

    /* Re-run the trajectory logic using the SNAPSHOT */
    for (uint64_t n = 0; n < blocks_needed; n++) {
        uint64_t found_lba = UINT64_MAX;

        if (is_horizon) {
            /* D1.5 Linear Scan */
            uint64_t stride = (1ULL << M);
            found_lba = G + (n * stride); 
            
            /* Verify ownership before freeing (Safety against corruption) */
            bool is_set;
            if (_bitmap_op(vol, found_lba, BIT_TEST, &is_set) != HN4_OK || !is_set) {
                found_lba = UINT64_MAX;
            } else {
                hn4_addr_t phys = hn4_lba_from_blocks(found_lba * sectors_per_blk);
                if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, vbuf, sectors_per_blk) == HN4_OK) {
                    hn4_block_header_t* h = (hn4_block_header_t*)vbuf;
                    hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                    if (disk_id.lo != target_id.lo || disk_id.hi != target_id.hi) {
                        found_lba = UINT64_MAX; /* ID Mismatch - Do not free */
                    }
                } else {
                    found_lba = UINT64_MAX; /* Read Error */
                }
            }
        } else {
            /* D1 Ballistic Scan (k=0..12) */
            for (uint8_t k=0; k<12; k++) {
                uint64_t candidate = _calc_trajectory_lba(vol, G, V, n, M, k);
                if (candidate == UINT64_MAX) continue;
                
                bool is_set;
                _bitmap_op(vol, candidate, BIT_TEST, &is_set);
                if (is_set) {
                    /* Ownership Verify */
                    hn4_addr_t phys = hn4_lba_from_blocks(candidate * sectors_per_blk);
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, vbuf, sectors_per_blk) == HN4_OK) {
                        hn4_block_header_t* h = (hn4_block_header_t*)vbuf;
                        hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                        if (disk_id.lo == target_id.lo && disk_id.hi == target_id.hi) {
                            found_lba = candidate;
                            break; /* Found the block for 'n', stop searching k */
                        }
                    }
                }
            }
        }

        if (found_lba != UINT64_MAX) {
            #ifdef HN4_USE_128BIT
                hn4_u128_t f_blk = hn4_u128_from_u64(found_lba);
                hn4_addr_t p_sec = hn4_u128_mul_u64(f_blk, sectors_per_blk);
                _reaper_add(vol, batch, hn4_addr_to_u64(p_sec)); 
            #else
                _reaper_add(vol, batch, found_lba * sectors_per_blk);
            #endif
            }
    }

    hn4_hal_mem_free(vbuf);
    return HN4_OK;
}


/* =========================================================================
 * TASK 2: THE EVACUATOR (ZNS ZONE PACKING)
 * ========================================================================= */

/**
 * _evacuate_zns_victim
 * 
 * Production ZNS Garbage Collector.
 * Scans a physical zone. Moves valid data to the Horizon. Resets the zone.
 * 
 * @param vol             Volume Context
 * @param zone_start_lba  Physical Sector LBA of the zone start
 * @param zone_len_bytes  Size of the zone in bytes
 */
static void _evacuate_zns_victim(
    HN4_IN hn4_volume_t* vol,
    HN4_IN uint64_t zone_start_lba,
    HN4_IN uint64_t zone_len_bytes
)
{
    /* 1. Geometry Setup */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint32_t sectors_per_blk = bs / ss;
    uint64_t blocks_in_zone = zone_len_bytes / bs;

    void* io_buf = hn4_hal_mem_alloc(bs);
    if (!io_buf) return;

    /* 
     * COUNTER REFACTOR (Issue 4):
     * 'valid_count' tracks blocks that MUST be moved.
     * 'evacuated_count' tracks blocks successfully moved.
     * We increment valid_count ONLY when we are committed to moving.
     */
    uint64_t valid_count = 0;
    uint64_t evacuated_count = 0;

    for (uint64_t i = 0; i < blocks_in_zone; i++) {

        if (caps->queue_count > 1) hn4_hal_micro_sleep(50);

        uint64_t phys_sector = zone_start_lba + (i * sectors_per_blk);
        uint64_t global_blk_idx = phys_sector / sectors_per_blk;

        bool is_set;
        if (_bitmap_op(vol, global_blk_idx, BIT_TEST, &is_set) != HN4_OK || !is_set) {
            continue;
        }

        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, hn4_lba_from_sectors(phys_sector), io_buf, sectors_per_blk) != HN4_OK) {
            HN4_LOG_CRIT("ZNS Evacuator: Read Error @ %llu. Aborting.", (unsigned long long)phys_sector);
            hn4_hal_mem_free(io_buf);
            return;
        }

        hn4_block_header_t* hdr = (hn4_block_header_t*)io_buf;
        if (hn4_le32_to_cpu(hdr->magic) != HN4_BLOCK_MAGIC) continue;

        hn4_u128_t block_id = hn4_le128_to_cpu(hdr->well_id);
        uint64_t block_gen = hn4_le64_to_cpu(hdr->generation);
        hn4_anchor_t* owner_anchor = NULL;

        if (vol->nano_cortex) {
            size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
            hn4_anchor_t* arr = (hn4_anchor_t*)vol->nano_cortex;
            for (size_t k = 0; k < count; k++) {
                hn4_u128_t seed = hn4_le128_to_cpu(arr[k].seed_id);
                if (seed.lo == block_id.lo && seed.hi == block_id.hi) {
                    owner_anchor = &arr[k];
                    break;
                }
            }
        }

        if (owner_anchor) {
            uint32_t anchor_gen = hn4_le32_to_cpu(owner_anchor->write_gen);

            if ((uint32_t)block_gen != anchor_gen) continue; /* Stale */

            uint64_t logic_seq = hn4_le64_to_cpu(hdr->seq_index);
            uint64_t mass = hn4_le64_to_cpu(owner_anchor->mass);
            uint32_t max_payload = HN4_BLOCK_PayloadSize(bs);
            uint64_t file_offset = logic_seq * max_payload;
            uint32_t move_len = max_payload;

            if (file_offset >= mass) continue; /* EOF Ghost */

            if (file_offset + max_payload > mass) {
                move_len = (uint32_t)(mass - file_offset);
            }

            /* 
             * COMMIT POINT:
             * Only now do we increment the requirement counter.
             */
            valid_count++;

            uint64_t dclass = hn4_le64_to_cpu(owner_anchor->data_class);
            
            hn4_anchor_t shadow_anchor;
            memcpy(&shadow_anchor, owner_anchor, sizeof(hn4_anchor_t));

            /* Prepare shadow for Horizon destination (D1.5) */
            if (!(dclass & HN4_HINT_HORIZON)) {
                dclass |= HN4_HINT_HORIZON;
                shadow_anchor.gravity_center = 0; 
                shadow_anchor.data_class = hn4_cpu_to_le64(dclass);
            }

            /* Perform write using the SHADOW anchor so live physics remain valid */
            if (hn4_write_block_atomic(vol, &shadow_anchor, logic_seq, hdr->payload, move_len) == HN4_OK) {
                
                memcpy(owner_anchor, &shadow_anchor, sizeof(hn4_anchor_t));
                
                hn4_write_anchor_atomic(vol, owner_anchor);
                evacuated_count++;
            } else {
                HN4_LOG_ERR("ZNS Evacuator: Move failed for block %llu.", (unsigned long long)logic_seq);
                hn4_hal_mem_free(io_buf);
                return; /* Hard abort on write failure */
            }
        } else {
            HN4_LOG_WARN("ZNS Evacuator: Orphan Block %llu. Skipping Zone.", (unsigned long long)global_blk_idx);
            hn4_hal_mem_free(io_buf);
            return;
        }
    }

    hn4_hal_mem_free(io_buf);

    if (evacuated_count == valid_count) {

        HN4_LOG_VAL("ZNS Evacuation Complete. Resetting Zone LBA", zone_start_lba);

        uint32_t zone_sectors = zone_len_bytes / ss;
        hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_ZONE_RESET,
                                           hn4_lba_from_sectors(zone_start_lba),
                                           NULL,
                                           zone_sectors);

        if (res == HN4_OK) {
            if (hn4_hal_barrier(vol->target_device) == HN4_OK) {
                for (uint64_t i = 0; i < blocks_in_zone; i++) {
                    uint64_t global_blk = (zone_start_lba / sectors_per_blk) + i;
                    _bitmap_op(vol, global_blk, BIT_FORCE_CLEAR, NULL);
                }
            } else {
                HN4_LOG_CRIT("ZNS Evacuator: Barrier failed after Reset!");
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
            }
        }
    }
}


/* =========================================================================
 * TASK 3: THE MEDIC (OSTEOPLASTY & RE-BALLISTIFICATION)
 * ========================================================================= */

static uint32_t _analyze_bone_density(hn4_volume_t* vol, hn4_anchor_t* anchor) {
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
    
    uint64_t V = 0; 
    memcpy(&V, anchor->orbit_vector, 6); 
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    
    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);

    /* ENTROPY WEIGHTING: Compressed files are denser/more critical */
    uint32_t weight = (dclass & HN4_HINT_COMPRESSED) ? 2 : 1;

    uint32_t total_k = 0;
    uint32_t samples = 0;

    for (uint64_t n = 0; n < 8; n++) {
        for (uint8_t k = 0; k < 12; k++) {
            uint64_t lba = _calc_trajectory_lba(vol, G, V, n, M, k);
            if (lba == UINT64_MAX) continue;
            bool is_set;
            _bitmap_op(vol, lba, BIT_TEST, &is_set);
            if (is_set) {
                total_k += k;
                samples++;
                break;
            }
        }
    }

    uint32_t density = (samples > 0) ? (total_k / samples) : 0;
    return density * weight;
}

static void _medic_queue_push(hn4_volume_t* vol, uint32_t anchor_idx, uint32_t score) {
    hn4_medic_queue_t* q = &vol->medic_queue;

    /* Simple Ring Insertion (Latest replaces Oldest if full) */
    hn4_hal_spinlock_acquire(&q->lock);

    if (q->count < HN4_MEDIC_QUEUE_SIZE) {
        q->entries[q->count].anchor_idx = anchor_idx;
        q->entries[q->count].score = score;
        q->count++;
    } else {
        /* Replace lowest score if new score is higher */
        int min_idx = -1;
        uint32_t min_score = UINT32_MAX;

        for (int i = 0; i < HN4_MEDIC_QUEUE_SIZE; i++) {
            if (q->entries[i].score < min_score) {
                min_score = q->entries[i].score;
                min_idx = i;
            }
        }

        if (min_idx >= 0 && score > min_score) {
            q->entries[min_idx].anchor_idx = anchor_idx;
            q->entries[min_idx].score = score;
        }
    }

    hn4_hal_spinlock_release(&q->lock);
}

/* 
 * SYSTEM INVARIANT NOTE:
 * This function updates the File's Orbit Vector (V).
 * Only Block 0 is physically moved. Tail blocks (1..N) remain at their old locations.
 * 
 * Safety: The hn4_read_block_atomic path supports "Mixed Trajectories" via 
 * the Shotgun Protocol. It will scan K=0..12.
 * The old blocks (using old V) will likely be found at higher K indices (or vice versa)
 * due to the vector shift. As long as K < 12 for the old positions under the new V,
 * they remain readable.
 * 
 * Ideally, a full V2 Osteoplasty would rewrite the entire file chain.
 */
static void _perform_osteoplasty(hn4_volume_t* vol, hn4_anchor_t* anchor, bool full_pivot) {
    uint32_t bs = vol->vol_block_size;
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return;

    /* RE-FETCH CAPS: Device state may change */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (caps->queue_count > 1) hn4_hal_micro_sleep(500); 

    /* Snapshot Generation for OCC */
    uint32_t start_gen_native = hn4_le32_to_cpu(anchor->write_gen);
    
    /* Calculate New Vector */
    uint64_t V = 0; 
    memcpy(&V, anchor->orbit_vector, 6); 
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    
    uint64_t new_V;
    if (full_pivot) new_V = (V ^ 0xDEADBEEFCAFEBABEULL) | 1;
    else new_V = hn4_swizzle_gravity_assist(V);
    
    /* Prepare Transient Anchor */
    hn4_anchor_t new_anchor;
    memcpy(&new_anchor, anchor, sizeof(hn4_anchor_t));
    uint64_t v_le = hn4_cpu_to_le64(new_V);
    memcpy(new_anchor.orbit_vector, &v_le, 6);

    /* Calculate Block Count */
    uint64_t mass = hn4_le64_to_cpu(anchor->mass);
    uint32_t payload_sz = HN4_BLOCK_PayloadSize(bs);
    uint64_t total_blocks = (mass + payload_sz - 1) / payload_sz;

    /* 
     * MIGRATION LOOP (O(N))
     * We must move every block. If any read fails, we abort.
     * If any write fails, we abort. The old file remains valid until the atomic anchor swap.
     */
    bool migration_success = true;

    /* Keep a copy of original anchor state for Delta Cleanup later */
    hn4_anchor_t original_anchor_state;
    memcpy(&original_anchor_state, anchor, sizeof(hn4_anchor_t));

    for (uint64_t n = 0; n < total_blocks; n++) {
        /* 1. Read from OLD Trajectory */
        if (hn4_read_block_atomic(vol, anchor, n, buf, bs) != HN4_OK) {
            HN4_LOG_WARN("Osteoplasty: Read failed at block %llu. Aborting.", (unsigned long long)n);
            migration_success = false;
            break;
        }

        /* Check for concurrent modification during the long loop */
        if (hn4_le32_to_cpu(anchor->write_gen) != start_gen_native) {
            HN4_LOG_WARN("Osteoplasty: User write detected during migration. Aborting.");
            migration_success = false;
            break;
        }

        /* 
         * 2. Write to NEW Trajectory
         * We must calculate the payload length correctly for the tail block.
         */
        uint32_t write_len = payload_sz;
        if (n == total_blocks - 1) {
            uint64_t remainder = mass % payload_sz;
            if (remainder != 0) write_len = (uint32_t)remainder;
        }

        /* 
         * Note: hn4_write_block_atomic updates new_anchor in RAM (mass/clock).
         * This is desirable as we want the new anchor to reflect the new state.
         */
        if (hn4_write_block_atomic(vol, &new_anchor, n, buf, write_len) != HN4_OK) {
            HN4_LOG_WARN("Osteoplasty: Write failed at block %llu. Aborting.", (unsigned long long)n);
            migration_success = false;
            break;
        }

        /* 
         * 3. ZERO-LOCK MIGRATION: Register Delta
         * We must calculate the physical locations to inform readers of the new data.
         */
        uint64_t old_lba_phys = 0;
        uint64_t new_lba_phys = 0;
        uint8_t k_dummy;

        /* Resolve addresses. 
           Note: We use the local 'original_anchor_state' to ensure we calculate the OLD position correctly,
           even if 'anchor' (passed ptr) was mutated by another thread (though gen check guards this). */
        if (hn4_alloc_block(vol, &original_anchor_state, n, &old_lba_phys, &k_dummy) == HN4_OK &&
            hn4_alloc_block(vol, &new_anchor, n, &new_lba_phys, &k_dummy) == HN4_OK) 
        {
            _register_delta(hn4_addr_to_u64(old_lba_phys), 
                            hn4_addr_to_u64(new_lba_phys), 
                            start_gen_native);
        }
    }

    /* ATOMIC COMMIT */
    if (migration_success) {
        /* Re-check Generation one last time before CAS */
        if (hn4_le32_to_cpu(anchor->write_gen) != start_gen_native) goto cleanup_deltas;

        uint32_t expected_raw = anchor->write_gen; /* LE bits */
        
        /* 
         * CAS: If we win this, the file logically "snaps" to the new trajectory.
         * The old blocks are now garbage (to be reaped by Scavenger later).
         */
        if (atomic_compare_exchange_strong((_Atomic uint32_t*)&anchor->write_gen, 
                                           &expected_raw, 
                                           new_anchor.write_gen)) 
        {
            /* Success: Update RAM Source of Truth */
            memcpy(anchor->orbit_vector, new_anchor.orbit_vector, 6);
            anchor->gravity_center = new_anchor.gravity_center;
            anchor->mass = new_anchor.mass; 
            anchor->mod_clock = new_anchor.mod_clock;
            
            /* Persist to Disk */
            hn4_write_anchor_atomic(vol, anchor);
            
            HN4_LOG_CRIT("Osteoplasty Complete. Full file (%llu blocks) migrated.", (unsigned long long)total_blocks);
            
            if (vol->stats.trajectory_collapse_counter > 0)
                atomic_fetch_sub(&vol->stats.trajectory_collapse_counter, 1);
        } else {
            HN4_LOG_WARN("Osteoplasty CAS Failed: Writer Intervened at final commit.");
        }
    }

cleanup_deltas:
    for (uint64_t n = 0; n < total_blocks; n++) {
         uint64_t old_lba_phys = 0;
         uint8_t k_dummy;
         
         /* Re-resolve using the original state to find the keys we registered */
         if (hn4_alloc_block(vol, &original_anchor_state, n, &old_lba_phys, &k_dummy) == HN4_OK) {
             _clear_delta(hn4_addr_to_u64(old_lba_phys));
         }
    }

    hn4_hal_mem_free(buf);
}



/* =========================================================================
 * TASK 4: THE STITCHER (HORIZON STREAM INDEXING)
 * ========================================================================= */

/**
 * _stitch_stream
 * 
 * Scans a sequential Horizon stream (D2) and updates "Hyper-Skip" pointers.
 * This turns O(N) seek times into O(log N) or O(1) for large streams.
 */
static hn4_result_t _stitch_stream(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* anchor
)
{
    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t sectors = bs / ss;

    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return HN4_ERR_NOMEM;

    uint64_t head_blk = hn4_le64_to_cpu(anchor->gravity_center);
    hn4_addr_t current_lba = hn4_lba_from_blocks(head_blk * sectors);

    uint64_t seq = 0;

    uint64_t skip_base_blk = 0;
    hn4_addr_t skip_base_lba = hn4_addr_from_u64(0);
    bool tracking_skip = false;

    while (1) {
        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, current_lba, buf, sectors) != HN4_OK) break;

        hn4_stream_header_t* strm = (hn4_stream_header_t*)buf;
        if (hn4_le32_to_cpu(strm->magic) != HN4_MAGIC_STREAM) break;

        /* 1. Complete previous stitch? */
        if (tracking_skip && (seq >= skip_base_blk + HN4_STREAM_SKIP_DIST)) {

            void* base_buf = hn4_hal_mem_alloc(bs);
            if (base_buf) {
                if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, skip_base_lba, base_buf, sectors) == HN4_OK) {
                    hn4_stream_header_t* base_strm = (hn4_stream_header_t*)base_buf;

                    /* 
                     * Integrity Verification.
                     * We must verify the existing data is valid before modifying it.
                     */
                    uint32_t stored_crc = hn4_le32_to_cpu(base_strm->crc);
                    base_strm->crc = 0;
                    uint32_t calc_crc = hn4_crc32(0, base_strm, bs);
                    base_strm->crc = hn4_cpu_to_le32(stored_crc); /* Restore for check */

                    bool integrity_ok = (stored_crc == calc_crc);

                    if (integrity_ok &&
                        hn4_le32_to_cpu(base_strm->magic) == HN4_MAGIC_STREAM &&
                        base_strm->hyper_strm == 0) {
                        uint64_t current_blk_idx = hn4_addr_to_u64(current_lba) / sectors;
                        base_strm->hyper_strm = hn4_cpu_to_le64(current_blk_idx);

                        /* Re-sign valid data */
                        base_strm->crc = 0;
                        base_strm->crc = hn4_cpu_to_le32(hn4_crc32(0, base_strm, bs));

                        hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, skip_base_lba, base_buf, sectors);
                    } else if (!integrity_ok) {
                        HN4_LOG_WARN("Scavenger: Data Rot detected in stream at LBA %llu. Stitching aborted.",
                                     (unsigned long long)hn4_addr_to_u64(skip_base_lba));
                        /* 
                         * Optional: Trigger self-healing if replicas exist, 
                         * but do NOT modify this block. 
                         */
                    }
                }
                hn4_hal_mem_free(base_buf);
            }
            tracking_skip = false;
        }

        /* 2. Start new tracking? */
        if (!tracking_skip && (seq % HN4_STREAM_SKIP_DIST == 0) && strm->hyper_strm == 0) {
            skip_base_blk = seq;
            skip_base_lba = current_lba;
            tracking_skip = true;
        }

        uint64_t next_blk = hn4_le64_to_cpu(strm->next_strm);
        if (next_blk == 0) break;

        current_lba = hn4_lba_from_blocks(next_blk * sectors);
        seq++;

        /* Safety brake */
        if (seq > 1000000) break;
    }

    hn4_hal_mem_free(buf);
    return HN4_OK;
}

static void _uptier_horizon_data(hn4_volume_t* vol, hn4_anchor_t* anchor) {
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);

    /* Only valid for files currently in Horizon but NOT Pinned to it */
    if (!(dclass & HN4_HINT_HORIZON)) return;
    if (dclass & HN4_FLAG_PINNED) return; /* User requested pinning */

    /* 
     * STRATEGY:
     * Try to rewrite block 0 using standard Ballistic Allocation.
     * If successful, clear the HORIZON hint bit.
     */
    uint32_t bs = vol->vol_block_size;
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return;

    if (hn4_read_block_atomic(vol, anchor, 0, buf, bs) == HN4_OK) {

        hn4_anchor_t upgraded_anchor;
        memcpy(&upgraded_anchor, anchor, sizeof(hn4_anchor_t));

        /* Clear Horizon Hint */
        uint64_t new_dc = dclass & ~HN4_HINT_HORIZON;
        upgraded_anchor.data_class = hn4_cpu_to_le64(new_dc);

        /* 
         * Attempt Atomic Write. 
         * This will trigger the standard Ballistic Allocator because the hint is gone.
         * If D1 is full, it will fail back to Horizon (no harm done).
         */
       hn4_addr_t old_phys_lba = 0;
    uint8_t k_dummy;
    
    /* Using the ORIGINAL anchor state */
    hn4_alloc_block(vol, anchor, 0, &old_phys_lba, &k_dummy);

    /* Attempt Atomic Write (Allocates NEW ballistic block) */
    hn4_result_t res = hn4_write_block_atomic(vol, &upgraded_anchor, 0, buf, bs);
    
    if (res == HN4_OK) {
        hn4_write_anchor_atomic(vol, &upgraded_anchor);
        
        if (hn4_addr_to_u64(old_phys_lba) != 0) {
            hn4_free_block(vol, hn4_addr_to_u64(old_phys_lba));
        }
        
        HN4_LOG_VAL("Scavenger: Up-Tiered Horizon File", 0);
    }
    }

    hn4_hal_mem_free(buf);
}


/* =========================================================================
 * PUBLIC API: SCAVENGER PULSE
 * ========================================================================= */

void hn4_scavenger_pulse(HN4_IN hn4_volume_t* vol)
{
    if (!vol || vol->read_only) return;

    hn4_time_t now = hn4_hal_get_time_ns();

    /* 1. Vital Signs */
    uint32_t collapse_cnt = atomic_load(&vol->stats.trajectory_collapse_counter);
    bool medic_mode = (collapse_cnt > HN4_OSTEOPOROSIS_THRESHOLD);

    /* 2. ZNS Mode Check */
    bool is_zns = (vol->sb.info.device_type_tag == HN4_DEV_ZNS);

    /* 3. Setup Reaper Batch (Local Stack) */
    _reaper_batch_t batch;
    batch.count = 0;
    batch.block_size = vol->vol_block_size;

    if (vol->nano_cortex) {
        hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
        size_t count = vol->cortex_size / sizeof(hn4_anchor_t);

        /* 
         * PHASE 1: MEDIC PRIORITY QUEUE DRAIN (High Priority Surgery)
         * We process the triage list before doing routine scans.
         */
        hn4_hal_spinlock_acquire(&vol->medic_queue.lock);

        /* Process up to 4 critical items per pulse to avoid starving IO */
        int surgeries_performed = 0;

        while (vol->medic_queue.count > 0 && surgeries_performed < 4) {

            uint32_t best_i = 0;
            uint32_t max_score = 0;

            for (uint32_t i = 0; i < vol->medic_queue.count; i++) {
                if (vol->medic_queue.entries[i].score >= max_score) {
                    max_score = vol->medic_queue.entries[i].score;
                    best_i = i;
                }
            }

            uint32_t idx = vol->medic_queue.entries[best_i].anchor_idx;

            vol->medic_queue.count--;
            if (best_i < vol->medic_queue.count) {
                vol->medic_queue.entries[best_i] = vol->medic_queue.entries[vol->medic_queue.count];
            }

            if (idx < count) {
                /* Perform Osteoplasty (Data Migration) */
                _perform_osteoplasty(vol, &anchors[idx], false);
                surgeries_performed++;
            }

            /* Re-acquire for next loop iteration */
            hn4_hal_spinlock_acquire(&vol->medic_queue.lock);
        }
        hn4_hal_spinlock_release(&vol->medic_queue.lock);


        /* 
         * PHASE 2: ROUTINE PATROL (Time Sliced)
         * Scan 64 anchors. If we find problems, push to queue or reap immediately.
         */

        static _Atomic size_t cursor = 0;
        size_t start_idx = atomic_fetch_add(&cursor, 64);

        for (size_t i = 0; i < 64; i++) {
            size_t idx = (start_idx + i) % count;
            hn4_anchor_t* a = &anchors[idx];
            uint64_t dc = hn4_le64_to_cpu(a->data_class);

            /* A. REAPER: Tombstones are deleted immediately */
            if (dc & HN4_FLAG_TOMBSTONE) {
                _reap_tombstone(vol, a, now, &batch);
            }
            /* B. MEDIC: Check Bone Density */
            else if (medic_mode && (dc & HN4_FLAG_VALID)) {

                /* Standard Files (D1 Flux) */
                if (!(dc & HN4_HINT_HORIZON)) {
                    uint32_t density = _analyze_bone_density(vol, a);
                    if (density >= HN4_BONE_DENSITY_CRITICAL) {
                        /* Push to Priority Queue */
                        _medic_queue_push(vol, (uint32_t)idx, density);
                    }
                }
                /* Horizon Files (D1.5) -> Up-Tiering Opportunity */
                else {
                    /* Probabilistic check (~ every 1024 pulses) */
                    if ((now & 1023) == 0) {
                        _uptier_horizon_data(vol, a);
                    }
                }
            }

            if ((dc & HN4_HINT_STREAM) && (dc & HN4_FLAG_VALID)) {
                /* Stitch every 128th pulse to save IO */
                if ((idx & 127) == 0) _stitch_stream(vol, a);
            }
        }
    }

    /* 4. Flush any pending TRIMs collected during Reap */
     _reaper_flush(vol->target_device, &batch, vol);

    /* 
     * 5. ZNS EVACUATOR (Zone Compaction)
     */
    if (is_zns) {
        static uint64_t zns_pulse_ticker = 0;
        zns_pulse_ticker++;

        /* Throttle: Run evacuation logic every 100 pulses */
        if ((zns_pulse_ticker % 100) == 0) {
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);

            if (caps && caps->zone_size_bytes > 0) {
                uint64_t cap_bytes = hn4_addr_to_u64(caps->total_capacity_bytes);
                uint64_t total_zones = cap_bytes / caps->zone_size_bytes;
                uint32_t ss = caps->logical_block_size;
                uint64_t zone_sectors = caps->zone_size_bytes / ss;

                /* Calculate Safe Zone Start (Skip Metadata) */
                uint64_t flux_start_sector = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
                uint64_t first_safe_zone = (flux_start_sector + zone_sectors - 1) / zone_sectors;

                /* Round-Robin Cursor */
                static uint64_t zone_cursor = 0;
                if (zone_cursor < first_safe_zone || zone_cursor >= total_zones) {
                    zone_cursor = first_safe_zone;
                }

                uint64_t victim_start_lba = zone_cursor * zone_sectors;
                uint64_t victim_len_bytes = caps->zone_size_bytes;

                _evacuate_zns_victim(vol, victim_start_lba, victim_len_bytes);

                zone_cursor++;
            }
        }
    }
}