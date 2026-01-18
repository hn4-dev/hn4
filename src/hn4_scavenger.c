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
 * AUDIT STATE (FILE SCOPE)
 * ========================================================================= */
static uint64_t _audit_region_cursor = 0;
#define HN4_AUDIT_REGION_SIZE (1ULL * 1024 * 1024 * 1024) /* 1 GB */

/* =========================================================================
 * CONFIGURATION & THRESHOLDS
 * ========================================================================= */

#define HN4_REAPER_GRACE_NS         (24ULL * 3600ULL * 1000000000ULL) /* 24 Hours */
#define HN4_REAPER_BATCH_SIZE       256                               /* Max blocks per DISCARD */
#define HN4_OSTEOPOROSIS_THRESHOLD  50                                /* Collisions before Medic wakes */
#define HN4_BONE_DENSITY_CRITICAL   8                                 /* K-depth considered "Brittle" */
#define HN4_STREAM_SKIP_DIST        1024                              /* Hyper-Stream Interval */
#define HN4_ZNS_VICTIM_THRESHOLD    80                                /* % Invalid before Zone Reset */
#define HN4_DELTA_TABLE_SIZE 1024 
#define HN4_DELTA_PROBE_LIMIT 32 /* Limit linear probing */


/* =========================================================================
 * DELTA TABLE (ZERO-LOCK MIGRATION)
 * ========================================================================= */

/* 
 * A simple lock-free hash map for redirecting reads while the Scavenger works.
 * Size must be power of 2. 
 */
#define HN4_DELTA_TABLE_SIZE 1024 


static inline uint64_t _delta_hash(uint64_t key) {
    key ^= (key >> 33);
    key *= 0xff51afd7ed558ccdULL;
    key ^= (key >> 33);
    return key;
}

void hn4_scavenger_init_delta_table(void) {
    for (int i = 0; i < HN4_DELTA_TABLE_SIZE; i++) {
        atomic_init(&_delta_table[i].old_lba, 0);
        atomic_init(&_delta_table[i].new_lba, 0);
        atomic_init(&_delta_table[i].version, 0);
    }
}

/* Called by Reader (Hot Path) */
uint64_t hn4_scavenger_lookup_delta(hn4_volume_t* vol, uint64_t logical_lba, uint32_t req_version, uint64_t req_seed_hash) {
    uint64_t start_idx = _delta_hash(logical_lba) & (HN4_DELTA_TABLE_SIZE - 1);

    for (int i = 0; i < HN4_DELTA_PROBE_LIMIT; i++) {
        uint64_t idx = (start_idx + (i * i)) & (HN4_DELTA_TABLE_SIZE - 1);

        uint64_t key = atomic_load_explicit(&vol->redirect.delta_table[idx].old_lba, memory_order_acquire);

        if (key == logical_lba) {
            uint64_t seed = atomic_load_explicit(&vol->redirect.delta_table[idx].seed_hash, memory_order_relaxed);
            
            if (seed == req_seed_hash) {
                uint32_t ver = atomic_load_explicit(&vol->redirect.delta_table[idx].version, memory_order_relaxed);
                
                if (ver == req_version) {
                    /* Payload */
                    return atomic_load_explicit(&vol->redirect.delta_table[idx].new_lba, memory_order_relaxed);
                }
            }
            return 0;
        }
        
        if (key == 0) return 0;
    }
    return 0;
}

static int _register_delta(hn4_volume_t* vol, uint64_t old_lba, uint64_t new_lba, uint32_t version, uint64_t seed_hash) {
    uint64_t start_idx = _delta_hash(old_lba) & (HN4_DELTA_TABLE_SIZE - 1);

    for (int i = 0; i < HN4_DELTA_PROBE_LIMIT; i++) {
        uint64_t idx = (start_idx + (i * i)) & (HN4_DELTA_TABLE_SIZE - 1);
        
        uint64_t existing_key = atomic_load_explicit(&vol->redirect.delta_table[idx].old_lba, memory_order_acquire);
        uint64_t existing_seed = atomic_load_explicit(&vol->redirect.delta_table[idx].seed_hash, memory_order_relaxed);
        
        /* FIX: Use CAS to claim the slot ownership */
        if (existing_key == 0) {
            uint64_t expected = 0;
            /* Tentatively claim slot with the Key using CAS */
            if (!atomic_compare_exchange_strong_explicit(
                &vol->redirect.delta_table[idx].old_lba, 
                &expected, 
                old_lba, 
                memory_order_acq_rel, 
                memory_order_acquire)) 
            {
                /* CAS failed: Slot was taken by another thread. Re-evaluate loop. */
                continue; 
            }
            /* We own the slot now. Proceed to fill data. */
            existing_key = old_lba;
        }

        /* Update payload if we own the slot (or claimed it above) */
        if (existing_key == old_lba) {
            uint64_t current_seed = atomic_load_explicit(&vol->redirect.delta_table[idx].seed_hash, memory_order_relaxed);
            
            /* If this is a collision with a different file (seed mismatch), keep probing */
            if (current_seed != 0 && current_seed != seed_hash) continue;

            atomic_store_explicit(&vol->redirect.delta_table[idx].new_lba, new_lba, memory_order_relaxed);
            atomic_store_explicit(&vol->redirect.delta_table[idx].version, version, memory_order_relaxed);
            atomic_store_explicit(&vol->redirect.delta_table[idx].seed_hash, seed_hash, memory_order_release);
            return 0; 
        }
    }
    return -1; 
}



static void _clear_delta(hn4_volume_t* vol, uint64_t old_lba, uint64_t seed_hash) {
    uint64_t start_idx = _delta_hash(old_lba) & (HN4_DELTA_TABLE_SIZE - 1);

    for (int i = 0; i < HN4_DELTA_PROBE_LIMIT; i++) {
        uint64_t idx = (start_idx + (i * i)) & (HN4_DELTA_TABLE_SIZE - 1);
        uint64_t key = atomic_load(&vol->redirect.delta_table[idx].old_lba);

        if (key == old_lba) {
            uint64_t seed = atomic_load(&vol->redirect.delta_table[idx].seed_hash);
            if (seed == seed_hash) {
                atomic_store_explicit(&vol->redirect.delta_table[idx].old_lba, 0, memory_order_release);
                atomic_store_explicit(&vol->redirect.delta_table[idx].new_lba, 0, memory_order_relaxed);
                atomic_store_explicit(&vol->redirect.delta_table[idx].version, 0, memory_order_relaxed);
                atomic_store_explicit(&vol->redirect.delta_table[idx].seed_hash, 0, memory_order_relaxed);
                return;
            }
        }
        if (key == 0) return;
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

static int _addr_cmp(const void* a, const void* b) {
    hn4_addr_t va = *(const hn4_addr_t*)a;
    hn4_addr_t vb = *(const hn4_addr_t*)b;
#ifdef HN4_USE_128BIT
    return hn4_u128_cmp(va, vb);
#else
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
#endif
}

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
        }
    }

    qsort(batch->lbas, batch->count, sizeof(hn4_addr_t), _addr_cmp);

    /* --- PHASE 1: PHYSICAL SANITIZATION --- */
    uint32_t i = 0;
    while (i < batch->count) {
        hn4_addr_t start = batch->lbas[i];
        uint32_t merged = 1;
        
        /* Look ahead for contiguous blocks */
        while ((i + merged) < batch->count) {
            hn4_addr_t next_expected = hn4_addr_add(start, merged * sectors_per_blk);
            hn4_addr_t next_actual = batch->lbas[i + merged];
            
            /* Check equality */
            #ifdef HN4_USE_128BIT
            if (hn4_u128_cmp(next_expected, next_actual) != 0) break;
            #else
            if (next_expected != next_actual) break;
            #endif
            
            merged++;
        }

        /* Issue Single IO for the Range */
        bool is_zns = (caps->hw_flags & HN4_HW_ZNS_NATIVE);

        if (batch->secure_shred && zero_buf) {
            for (uint32_t k = 0; k < merged; k++) {
                hn4_addr_t target = hn4_addr_add(start, k * sectors_per_blk);
                hn4_hal_sync_io(dev, HN4_IO_WRITE, target, zero_buf, sectors_per_blk);
            }
        } else if (!is_zns) {
            /* Standard Trim - Only for Conventional Block Devices */
            hn4_hal_sync_io(dev, HN4_IO_DISCARD, start, NULL, sectors_per_blk * merged);
        }
        
        i += merged;
    }

    /* --- PHASE 2: THE WALL (Barrier) --- */
    hn4_hal_barrier(dev);

    /* --- PHASE 3: LOGICAL RELEASE --- */
    for (uint32_t k = 0; k < batch->count; k++) {
        hn4_free_block(vol, batch->lbas[k]);
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
     if (batch->count >= HN4_REAPER_BATCH_SIZE) {
        _reaper_flush(vol->target_device, batch, vol);
        hn4_hal_poll(vol->target_device);
    }

    /* Now guaranteed to have space */
    batch->lbas[batch->count++] = phys_sector_lba;
}



/* =========================================================================
 * TASK 1: THE REAPER (TOMBSTONE CLEANUP)
 * ========================================================================= */

static hn4_result_t _reap_tombstone(
    HN4_IN hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN hn4_time_t now,
    HN4_INOUT _reaper_batch_t* unused_batch 
)
{
    _reaper_batch_t local_batch;
    local_batch.count = 0;
    local_batch.block_size = vol->vol_block_size;
    void* vbuf = NULL;
    hn4_result_t res = HN4_OK;

    uint32_t start_gen = hn4_le32_to_cpu(anchor->write_gen);
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    
    if (!(dclass & HN4_FLAG_TOMBSTONE) || !(dclass & HN4_FLAG_VALID)) return HN4_OK; 

    if (dclass & HN4_FLAG_PINNED) return HN4_OK;

    hn4_time_t death_time = (hn4_time_t)hn4_le64_to_cpu(anchor->mod_clock);
    if ((now - death_time) < HN4_REAPER_GRACE_NS) return HN4_OK; 

    local_batch.secure_shred = (dclass & HN4_FLAG_SHRED) ? true : false;
    uint32_t bs = vol->vol_block_size;
    vbuf = hn4_hal_mem_alloc(bs);
    if (!vbuf) return HN4_ERR_NOMEM;

    /* Snapshot Metadata */
    hn4_anchor_t saved_anchor;
    memcpy(&saved_anchor, anchor, sizeof(hn4_anchor_t));

    /* 5. ATOMIC DESTRUCTION */
   hn4_anchor_t dead_anchor;

    memcpy(&dead_anchor, &saved_anchor, sizeof(hn4_anchor_t));
    
    dead_anchor.mass = 0;
    dead_anchor.gravity_center = 0;
    
    /* Clear Name to allow reuse by new files with same name */
    memset(dead_anchor.inline_buffer, 0, sizeof(dead_anchor.inline_buffer));
    
    /* Ensure Tombstone flag remains set */
    uint64_t bleached_dclass = HN4_FLAG_TOMBSTONE | HN4_FLAG_VALID;
    dead_anchor.data_class = hn4_cpu_to_le64(bleached_dclass);

    /* Preserve generation to prevent races */
    dead_anchor.write_gen = anchor->write_gen; 
    
    res = hn4_write_anchor_atomic(vol, &dead_anchor);

    if (res == HN4_OK) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        /* Guard against resurrection race */
        if (hn4_le32_to_cpu(anchor->write_gen) == start_gen) {
            /* FIX: Update RAM with Bleached state, do not zero */
            memcpy(anchor, &dead_anchor, sizeof(hn4_anchor_t));
        } else {
            res = HN4_ERR_GENERATION_SKEW;
        }
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
    }

    if (res != HN4_OK) {
        hn4_hal_mem_free(vbuf);
        return res;
    }

    /* 6. EXECUTE SCAVENGING */
    uint64_t saved_dclass = hn4_le64_to_cpu(saved_anchor.data_class);
    if (saved_dclass & HN4_FLAG_NANO) {
        hn4_hal_mem_free(vbuf);
        return HN4_OK;
    } 
    
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
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
    uint64_t target_gen  = (uint64_t)start_gen; 
    uint64_t seed_hash = target_id.lo ^ target_id.hi; 

    for (uint64_t n = 0; n < blocks_needed; n++) {
        uint64_t found_lba = UINT64_MAX;

        if (saved_dclass & HN4_HINT_HORIZON) {
            found_lba = G + n; 
            bool is_set;

            if (_bitmap_op(vol, found_lba, BIT_TEST, &is_set) != HN4_OK || !is_set) {
                found_lba = UINT64_MAX;
            } else {

                hn4_addr_t phys = hn4_lba_from_blocks(found_lba * sectors_per_blk);
                if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, vbuf, sectors_per_blk) == HN4_OK) {
                    hn4_block_header_t* h = (hn4_block_header_t*)vbuf;
                    hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                    uint64_t disk_gen = hn4_le64_to_cpu(h->generation);
                    if (disk_id.lo != target_id.lo || disk_id.hi != target_id.hi || disk_gen != target_gen) {
                        found_lba = UINT64_MAX; 
                    }
                } else {
                    found_lba = UINT64_MAX; 
                }
            }
        } else {
            /* Ballistic Scan */
            for (uint8_t k=0; k<12; k++) {
                uint64_t candidate = _calc_trajectory_lba(vol, G, V, n, M, k);
                if (candidate == UINT64_MAX) continue;
                bool is_set;
                if (_bitmap_op(vol, candidate, BIT_TEST, &is_set) == HN4_OK && is_set) {
                    hn4_addr_t phys = hn4_lba_from_blocks(candidate * sectors_per_blk);
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, vbuf, sectors_per_blk) == HN4_OK) {
                        hn4_block_header_t* h = (hn4_block_header_t*)vbuf;
                        hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                        uint64_t disk_gen = hn4_le64_to_cpu(h->generation);
                        if (disk_id.lo == target_id.lo && disk_id.hi == target_id.hi && disk_gen == target_gen) {
                            found_lba = candidate;
                            break; 
                        }
                    }
                }
            }
        }

        if (found_lba != UINT64_MAX) {

            if (hn4_scavenger_lookup_delta(vol, found_lba, start_gen, seed_hash) != 0) continue; 
            
            _reaper_add(vol, &local_batch, hn4_lba_from_blocks(found_lba * sectors_per_blk));
        }
    }

    bool safe_to_flush = false;
    
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    /* Anchor should be zeroed (data_class == 0) if Step 5 succeeded. */
    if (hn4_le64_to_cpu(anchor->data_class) & HN4_FLAG_TOMBSTONE) {
        safe_to_flush = true;
    }
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    if (safe_to_flush) {
        _reaper_flush(vol->target_device, &local_batch, vol);
    } else {
        local_batch.count = 0; 
        res = HN4_ERR_GENERATION_SKEW;
    }

    hn4_hal_mem_free(vbuf);
    return res;
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
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint32_t sectors_per_blk = bs / ss;
    uint64_t blocks_in_zone = zone_len_bytes / bs;

    void* io_buf = hn4_hal_mem_alloc(bs);
    if (!io_buf) return;

    uint64_t valid_count = 0;
    uint64_t evacuated_count = 0;

    for (uint64_t i = 0; i < blocks_in_zone; i++) {

        if (caps->queue_count > 1) hn4_hal_micro_sleep(50);

        uint64_t phys_sector = zone_start_lba + (i * sectors_per_blk);
        uint64_t global_blk_idx = phys_sector / sectors_per_blk;

        bool is_set;
        if (_bitmap_op(vol, global_blk_idx, BIT_TEST, &is_set) != HN4_OK || !is_set) continue;

        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, hn4_lba_from_sectors(phys_sector), io_buf, sectors_per_blk) != HN4_OK) {
            continue; 
        }

        hn4_block_header_t* hdr = (hn4_block_header_t*)io_buf;
        if (hn4_le32_to_cpu(hdr->magic) != HN4_BLOCK_MAGIC) continue;

        hn4_u128_t block_id = hn4_le128_to_cpu(hdr->well_id);
        uint64_t block_gen = hn4_le64_to_cpu(hdr->generation);
        
        hn4_anchor_t owner_copy; 
        hn4_anchor_t* owner_ptr = NULL; 
        bool found = false;

        if (vol->nano_cortex) {
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            size_t count = vol->cortex_size / sizeof(hn4_anchor_t);
            hn4_anchor_t* arr = (hn4_anchor_t*)vol->nano_cortex;
            for (size_t k = 0; k < count; k++) {
                hn4_u128_t seed = hn4_le128_to_cpu(arr[k].seed_id);
                if (seed.lo == block_id.lo && seed.hi == block_id.hi) {
                    memcpy(&owner_copy, &arr[k], sizeof(hn4_anchor_t));
                    owner_ptr = &arr[k];
                    found = true;
                    break;
                }
            }
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
        }

        if (found) {
            uint32_t anchor_gen = hn4_le32_to_cpu(owner_copy.write_gen);
            if ((uint32_t)block_gen != anchor_gen) continue; 

            uint64_t logic_seq = hn4_le64_to_cpu(hdr->seq_index);
            uint64_t mass = hn4_le64_to_cpu(owner_copy.mass);
            uint32_t max_payload = HN4_BLOCK_PayloadSize(bs);
            uint32_t move_len = max_payload;
            uint64_t file_offset = logic_seq * max_payload;

            if (file_offset >= mass) continue;
            if (file_offset + max_payload > mass) move_len = (uint32_t)(mass - file_offset);

            valid_count++;

            uint64_t dclass = hn4_le64_to_cpu(owner_copy.data_class);
            hn4_anchor_t shadow_anchor;
            memcpy(&shadow_anchor, &owner_copy, sizeof(hn4_anchor_t));

            if (!(dclass & HN4_HINT_HORIZON)) {
                dclass |= HN4_HINT_HORIZON;
                shadow_anchor.gravity_center = 0; 
                shadow_anchor.data_class = hn4_cpu_to_le64(dclass);
            }

             if (hn4_write_block_atomic(vol, &shadow_anchor, logic_seq, hdr->payload, move_len, 
                                       HN4_PERM_SOVEREIGN | HN4_PERM_WRITE) == HN4_OK) {
                if (hn4_write_anchor_atomic(vol, &shadow_anchor) == HN4_OK) {
                    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);

                    if (owner_ptr && hn4_le32_to_cpu(owner_ptr->write_gen) == anchor_gen && 
                        owner_ptr->seed_id.lo == block_id.lo && 
                        owner_ptr->seed_id.hi == block_id.hi) {
                        memcpy(owner_ptr, &shadow_anchor, sizeof(hn4_anchor_t));
                        evacuated_count++;
                    } else {
                        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
                    }
                    hn4_hal_spinlock_release(&vol->locking.l2_lock);
                } 
            } 
        }
    }

    hn4_hal_mem_free(io_buf);

    if (evacuated_count == valid_count && valid_count > 0) {
        hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_ZONE_RESET,
                                           hn4_lba_from_sectors(zone_start_lba), NULL, 0);
        if (res == HN4_OK) {
            if (hn4_hal_barrier(vol->target_device) == HN4_OK) {
                for (uint64_t i = 0; i < blocks_in_zone; i++) {
                    uint64_t global_blk = (zone_start_lba / sectors_per_blk) + i;
                    _bitmap_op(vol, global_blk, BIT_CLEAR, NULL);
                }
            } else {
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

static void _rollback_delta(hn4_volume_t* vol, uint64_t old_lba, uint64_t seed_hash) {
    uint64_t start_idx = _delta_hash(old_lba) & (HN4_DELTA_TABLE_SIZE - 1);

    for (int i = 0; i < HN4_DELTA_PROBE_LIMIT; i++) {
        uint64_t idx = (start_idx + (i * i)) & (HN4_DELTA_TABLE_SIZE - 1);
        uint64_t key = atomic_load(&vol->redirect.delta_table[idx].old_lba);

        if (key == old_lba) {
            uint64_t seed = atomic_load(&vol->redirect.delta_table[idx].seed_hash);
            if (seed == seed_hash) {
                atomic_store_explicit(&vol->redirect.delta_table[idx].old_lba, 0, memory_order_release);
                return;
            }
        }
    }
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

    /* Snapshot Generation */
    uint32_t start_gen_native = hn4_le32_to_cpu(anchor->write_gen);
    hn4_u128_t seed = hn4_le128_to_cpu(anchor->seed_id);
    uint64_t seed_hash = seed.lo ^ seed.hi;
    
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

    uint64_t mass = hn4_le64_to_cpu(anchor->mass);
    uint32_t payload_sz = HN4_BLOCK_PayloadSize(bs);
    uint64_t total_blocks = (mass + payload_sz - 1) / payload_sz;

    /* 
     * MIGRATION LOOP (O(N))
     */
    bool migration_success = true;
    hn4_anchor_t original_anchor_state;
    memcpy(&original_anchor_state, anchor, sizeof(hn4_anchor_t));
    uint64_t n_processed = 0;

    for (uint64_t n = 0; n < total_blocks; n++) {
        n_processed = n;
        
        /* 1. Read from OLD Trajectory */
        if (hn4_read_block_atomic(vol, anchor, n, buf, bs) != HN4_OK) {
            migration_success = false;
            break;
        }

        /* Check for concurrent modification */
        if (hn4_le32_to_cpu(anchor->write_gen) != start_gen_native) {
            migration_success = false;
            break;
        }

        /* 
         * 2. Write to NEW Trajectory
         */
        uint32_t write_len = payload_sz;
        if (n == total_blocks - 1) {
            uint64_t remainder = mass % payload_sz;
            if (remainder != 0) write_len = (uint32_t)remainder;
        }

        /* Note: hn4_write_block_atomic updates new_anchor in RAM */
         if (hn4_read_block_atomic(vol, anchor, n, buf, bs, HN4_PERM_SOVEREIGN | HN4_PERM_READ) != HN4_OK) {
            migration_success = false;
            break;
        }

        /* 
         * 3. ZERO-LOCK MIGRATION: Register Delta 
         */
        uint64_t old_lba_phys = _resolve_residency_verified(vol, &original_anchor_state, n);
        uint64_t new_lba_phys = _resolve_residency_verified(vol, &new_anchor, n);
        
        if (old_lba_phys != HN4_LBA_INVALID && new_lba_phys != HN4_LBA_INVALID) {
             if (_register_delta(vol, hn4_addr_to_u64(old_lba_phys), hn4_addr_to_u64(new_lba_phys), start_gen_native, seed_hash) != 0) {
                 migration_success = false;
                 break;
             }
        }
    }

    /* ATOMIC COMMIT */
    if (migration_success) {
        /* Re-check Generation one last time */
        if (hn4_le32_to_cpu(anchor->write_gen) != start_gen_native) {
            goto cleanup_deltas;
        }

        if (hn4_write_anchor_atomic(vol, &new_anchor) != HN4_OK) {
            goto cleanup_deltas;
        }

        /* Disk is safe. Now update RAM atomically via Lock + Store Fence */
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        
        if (anchor->write_gen == hn4_cpu_to_le32(start_gen_native)) {
            memcpy(anchor, &new_anchor, sizeof(hn4_anchor_t));
            
            atomic_thread_fence(memory_order_release);
            
            /* Telemetry update */
            if (vol->health.trajectory_collapse_counter > 0)
                atomic_fetch_sub(&vol->health.trajectory_collapse_counter, 1);
        }
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        
    } else {
cleanup_deltas:
        
        /* We must iterate up to n_processed to rollback delta registration */
        for (uint64_t n = 0; n <= n_processed && n < total_blocks; n++) {
             
             /* Calculate where we *tried* to put data */
             uint64_t new_lba_phys = _resolve_residency_verified(vol, &new_anchor, n);
             
             if (new_lba_phys != HN4_LBA_INVALID) {
                 /* Free the orphaned block */
                 hn4_free_block(vol, hn4_addr_to_u64(new_lba_phys));
             }
             
             /* Calculate where data *was* (to clear delta key) */
             uint64_t old_lba_phys = _resolve_residency_verified(vol, &original_anchor_state, n);
             
             if (old_lba_phys != HN4_LBA_INVALID) {
                 _rollback_delta(vol, hn4_addr_to_u64(old_lba_phys), seed_hash);
             }
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
                        
                        /* Since D2/Stream is append-only and immutable payload, only header metadata changes.
                           We update in-place but rely on CRC.
                           To be fully safe against concurrent reads seeing torn writes, 
                           we rely on Sector Atomicity (Header fits in 512B). 
                        */
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
    if (dclass & HN4_FLAG_PINNED) return; 

    uint32_t bs = vol->vol_block_size;
    uint32_t payload_len = HN4_BLOCK_PayloadSize(bs);
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return;

    /* 1. Read existing data (Block 0) */
    if (hn4_read_block_atomic(vol, anchor, 0, buf, bs) == HN4_OK) {

        /* 2. Resolve Old Physical Location (D1.5) for later freeing */
        uint64_t old_lba_phys = _resolve_residency_verified(vol, anchor, 0);

        /* 3. Prepare New State */
        hn4_anchor_t upgraded_anchor;
        memcpy(&upgraded_anchor, anchor, sizeof(hn4_anchor_t));

        /* Clear Horizon Hint -> Forces Allocator to use D1 Ballistic */
        uint64_t new_dc = dclass & ~HN4_HINT_HORIZON;
        upgraded_anchor.data_class = hn4_cpu_to_le64(new_dc);

        /* 4. Atomic Write (Allocates NEW ballistic block) */
        hn4_result_t res = hn4_write_block_atomic(vol, &upgraded_anchor, 0, buf, payload_len, 
                                                  HN4_PERM_SOVEREIGN | HN4_PERM_WRITE);
    
        if (res == HN4_OK) {
            /* 5. Commit Anchor to Disk */
            if (hn4_write_anchor_atomic(vol, &upgraded_anchor) == HN4_OK) {
            
                /* 6. Sync RAM state under lock */
                hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
                memcpy(anchor, &upgraded_anchor, sizeof(hn4_anchor_t));
                hn4_hal_spinlock_release(&vol->locking.l2_lock);
            
                /* 7. Free Old Block (D1.5) with Safety Checks */
                if (old_lba_phys != HN4_LBA_INVALID) {
                    /* Calculate ID Hash and Gen from the *original* anchor state logic */
                    hn4_u128_t seed = hn4_le128_to_cpu(anchor->seed_id);
                    uint64_t seed_hash = seed.lo ^ seed.hi;
                    uint32_t current_gen = hn4_le32_to_cpu(anchor->write_gen);

                    /* Only free if no active readers are redirected via delta table */
                    if (hn4_scavenger_lookup_delta(vol, hn4_addr_to_u64(old_lba_phys), current_gen, seed_hash) == 0) {
                        hn4_free_block(vol, hn4_addr_to_u64(old_lba_phys));
                    }
                }
                HN4_LOG_VAL("Scavenger: Up-Tiered Horizon File", 0);
            }
        }
    }

    hn4_hal_mem_free(buf);
}



/* Audit State Tracking */
static uint64_t _audit_cursor_byte = 0;
#define HN4_AUDIT_WINDOW_SIZE (1ULL * 1024 * 1024 * 1024) /* 1 GB Window */

static void _perform_leak_audit(hn4_volume_t* vol) {
    /* Safety: Cannot audit if bitmaps/cortex aren't loaded */
    if (!vol->nano_cortex || !vol->void_bitmap) return;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint32_t sectors_per_blk = bs / ss;

    /* 1. Define Window */
    uint64_t start_lba = _audit_region_cursor / bs;
    uint64_t end_lba   = (_audit_region_cursor + HN4_AUDIT_REGION_SIZE) / bs;
    
    /* Clamp to capacity */
    uint64_t max_blocks = vol->vol_capacity_bytes / bs;
    if (start_lba >= max_blocks) {
        _audit_region_cursor = 0; /* Reset for next pass */
        return;
    }
    if (end_lba > max_blocks) end_lba = max_blocks;

    uint64_t window_blocks = end_lba - start_lba;
    
    /* 2. Shadow Bitmap Allocation (1 bit per block in window) */
    size_t shadow_sz = (window_blocks + 7) / 8;
    uint8_t* shadow_map = hn4_hal_mem_alloc(shadow_sz);
    if (!shadow_map) return; /* Skip audit on OOM */
    memset(shadow_map, 0, shadow_sz);

    /* 3. Cortex Walk (The Heavy Lift) */
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    size_t count = vol->cortex_size / sizeof(hn4_anchor_t);

    for (size_t i = 0; i < count; i++) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        hn4_anchor_t a = anchors[i]; 
        hn4_hal_spinlock_release(&vol->locking.l2_lock);

        uint64_t dclass = hn4_le64_to_cpu(a.data_class);
        
        /* Skip invalid/deleted files */
        if (!(dclass & HN4_FLAG_VALID) || (dclass & HN4_FLAG_TOMBSTONE)) continue;

        /* Extract Physics */
        uint64_t G = hn4_le64_to_cpu(a.gravity_center);
        uint64_t mass = hn4_le64_to_cpu(a.mass);
        uint16_t M = hn4_le16_to_cpu(a.fractal_scale);
        
        uint64_t V = 0;
        memcpy(&V, a.orbit_vector, 6);
        V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;

        uint32_t payload_sz = HN4_BLOCK_PayloadSize(bs);
        uint64_t total_blocks = (mass + payload_sz - 1) / payload_sz;

        /* RE-PROJECT TRAJECTORY */
        for (uint64_t n = 0; n < total_blocks; n++) {
            uint64_t lba = HN4_LBA_INVALID;

            if (dclass & HN4_HINT_HORIZON) {
                lba = G + n;
            } else {
                /* Ballistic File: Compute shells */
                for (uint8_t k = 0; k < 12; k++) {
                    uint64_t cand = _calc_trajectory_lba(vol, G, V, n, M, k);
                    if (cand != HN4_LBA_INVALID) {
                        /* Check if specific shell allocated in real bitmap */
                        /* If so, assume this is the active shell for this block */
                        bool is_set;
                        /* Note: Calling bitmap_op here is expensive inside double loop. 
                           Optimization: Bounds check against window FIRST. */
                        if (cand >= start_lba && cand < end_lba) {
                            if (_bitmap_op(vol, cand, BIT_TEST, &is_set) == HN4_OK && is_set) {
                                lba = cand;
                                break;
                            }
                        }
                    }
                }
            }
            
            if (lba != HN4_LBA_INVALID && lba >= start_lba && lba < end_lba) {
                uint64_t rel = lba - start_lba;
                
                if (shadow_map[rel / 8] & (1 << (rel % 8))) {
                    HN4_LOG_WARN("Audit: Duplicate Ownership detected at LBA %llu", (unsigned long long)lba);
                    /* Log only. Do not free. Logic ambiguous. */
                }
                
                shadow_map[rel / 8] |= (1 << (rel % 8));
            }
            
            /* Anti-Stall */
            if ((n & 1023) == 0) hn4_hal_poll(vol->target_device);
        }
    }

    /* 4. XOR Comparison & Safety Verification */
    void* io_buf = hn4_hal_mem_alloc(bs);
    
    if (io_buf) {
        for (uint64_t j = 0; j < window_blocks; j++) {
            uint64_t abs_lba = start_lba + j;
            
            bool real_alloc;
            _bitmap_op(vol, abs_lba, BIT_TEST, &real_alloc);
            
            bool shadow_alloc = (shadow_map[j / 8] >> (j % 8)) & 1;

            if (real_alloc && !shadow_alloc) {
                /* 
                 * CANDIDATE LEAK: 
                 * Real bitmap says USED. No scanned anchor claimed it.
                 */
                bool safe_to_free = false;
                
                hn4_addr_t phys_lba = hn4_lba_from_blocks(abs_lba * sectors_per_blk);
                
                if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys_lba, io_buf, sectors_per_blk) == HN4_OK) {
                    hn4_block_header_t* h = (hn4_block_header_t*)io_buf;
                    
                    /* Check Magic */
                    if (hn4_le32_to_cpu(h->magic) != HN4_BLOCK_MAGIC) {
                        /* Garbage data in marked block -> Safe to free */
                        safe_to_free = true;
                    } else {
                        /* Valid Header. Find Owner. */
                        hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                        hn4_anchor_t owner;
                        
                        /* We use the public ID lookup which handles hash probing */
                        if (hn4_ns_get_anchor_by_id(vol, disk_id, &owner) == HN4_OK) {
                            /* Owner Exists. Check Generation. */
                            uint64_t disk_gen = hn4_le64_to_cpu(h->generation);
                            uint32_t anchor_gen = hn4_le32_to_cpu(owner.write_gen);
                            
                            /* If DiskGen >= AnchorGen, pending write. DO NOT FREE. */
                            if (disk_gen < anchor_gen) {
                                safe_to_free = true;
                            }

                            hn4_u128_t seed = hn4_le128_to_cpu(owner.seed_id);
                            uint64_t seed_hash = seed.lo ^ seed.hi;
                            
                            if (hn4_scavenger_lookup_delta(vol, abs_lba, anchor_gen, seed_hash) != 0) {
                                safe_to_free = false;
                            }

                        } else {
                            /* Owner not found (Deleted file). Safe to free. */
                            safe_to_free = true;
                        }
                    }
                } else {
                    /* Read Error. Do not touch. */
                    safe_to_free = false; 
                }

                if (safe_to_free) {
                    HN4_LOG_WARN("Audit: Reclaiming leaked block LBA %llu", (unsigned long long)abs_lba);
                    _bitmap_op(vol, abs_lba, BIT_CLEAR, NULL);
                }
            }
        }
        hn4_hal_mem_free(io_buf);
    }

    hn4_hal_mem_free(shadow_map);
    _audit_region_cursor += HN4_AUDIT_REGION_SIZE;
}


/* =========================================================================
 * PUBLIC API: SCAVENGER PULSE
 * ========================================================================= */

void hn4_scavenger_pulse(HN4_IN hn4_volume_t* vol)
{
    /* 1. Pre-flight Checks */
    if (!vol || vol->read_only) return;
    if (vol->sb.info.state_flags & HN4_VOL_PANIC) return;

    hn4_time_t now = hn4_hal_get_time_ns();

    /* 2. Vital Signs & Mode Detection */
    uint32_t collapse_cnt = atomic_load(&vol->health.trajectory_collapse_counter);
    bool medic_mode = (collapse_cnt > HN4_OSTEOPOROSIS_THRESHOLD);
    bool is_zns = (vol->sb.info.device_type_tag == HN4_DEV_ZNS);

      static uint32_t audit_ticker = 0;
    if (++audit_ticker % 100 == 0) {
        _perform_leak_audit(vol);
    }

    /* 3. Setup Reaper Batch (Stack Allocated) */
    _reaper_batch_t batch;
    batch.count = 0;
    batch.block_size = vol->vol_block_size;
    batch.secure_shred = false; /* Default */

    /* Requires Nano-Cortex (RAM) to operate effectively */
    if (vol->nano_cortex) {
        hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
        size_t count = vol->cortex_size / sizeof(hn4_anchor_t);

        /* 
         * PHASE 1: MEDIC PRIORITY QUEUE (High Priority Surgery)
         * We drain the triage list before scanning for garbage.
         * Limit: 4 surgeries per pulse to prevent IO starvation.
         */
        hn4_hal_spinlock_acquire(&vol->medic_queue.lock);

        int surgeries = 0;
        while (vol->medic_queue.count > 0 && surgeries < 4) {
            /* Find highest priority target */
            uint32_t best_i = 0;
            uint32_t max_score = 0;

            for (uint32_t i = 0; i < vol->medic_queue.count; i++) {
                if (vol->medic_queue.entries[i].score >= max_score) {
                    max_score = vol->medic_queue.entries[i].score;
                    best_i = i;
                }
            }

            uint32_t idx = vol->medic_queue.entries[best_i].anchor_idx;

            /* Remove from queue (unordered remove for speed) */
            vol->medic_queue.count--;
            if (best_i < vol->medic_queue.count) {
                vol->medic_queue.entries[best_i] = vol->medic_queue.entries[vol->medic_queue.count];
            }

            /* Release Lock during IO-heavy surgery */
            hn4_hal_spinlock_release(&vol->medic_queue.lock);

            if (idx < count) {
                /* Perform Osteoplasty (Migration to new vector V') */
                _perform_osteoplasty(vol, &anchors[idx], false);
                surgeries++;
            }

            /* Re-acquire for next iteration */
            hn4_hal_spinlock_acquire(&vol->medic_queue.lock);
        }
        hn4_hal_spinlock_release(&vol->medic_queue.lock);


        /* 
         * PHASE 2: ROUTINE PATROL (Time Sliced)
         * We scan a window of 64 anchors per pulse to distribute CPU load.
         */
        size_t start_idx = vol->alloc.scavenger_cursor;
        vol->alloc.scavenger_cursor = (start_idx + 64) % count;

        /* Scan Window */
        for (size_t i = 0; i < 64; i++) {
            size_t idx = (start_idx + i) % count;
            hn4_anchor_t* a = &anchors[idx];
            uint64_t dclass = hn4_le64_to_cpu(a->data_class);

            /* Skip Empty/Invalid Slots */
            if (dclass == 0) continue;

            /* 
             * A. THE REAPER (Entropy Protocol)
             * Checks if Tombstone grace period has expired.
             * If yes -> Zeros Anchor -> Frees Blocks -> Making Undelete Impossible.
             */
            if (dclass & HN4_FLAG_TOMBSTONE) {
                _reap_tombstone(vol, a, now, &batch);
            }
            
            /* 
             * B. THE MEDIC (Bone Density Check)
             * Only runs if the system is seeing collisions (medic_mode).
             */
            else if (medic_mode && (dclass & HN4_FLAG_VALID)) {
                if (!(dclass & HN4_HINT_HORIZON)) {
                    /* Check D1 Flux Density */
                    uint32_t density = _analyze_bone_density(vol, a);
                    if (density >= HN4_BONE_DENSITY_CRITICAL) {
                        _medic_queue_push(vol, (uint32_t)idx, density);
                    }
                } 
                else {
                    /* Horizon (D1.5) Up-Tiering Opportunity */
                    /* Probabilistic: Try to move back to D1 occasionally */
                    if ((now & 1023) == 0) { 
                        _uptier_horizon_data(vol, a);
                    }
                }
            }

            /* 
             * C. THE STITCHER (Stream Optimization)
             * Adds skip-list pointers to long D2 streams.
             * 128:1 Sampling Ratio.
             */
            if ((dclass & HN4_HINT_STREAM) && (dclass & HN4_FLAG_VALID)) {
                if ((idx & 127) == 0) _stitch_stream(vol, a);
            }
        }
    }

    /* 4. Flush Pending TRIMs (The Reaper's Scythe) */
    /* This physically sends DISCARD commands for blocks collected in _reap_tombstone */
    _reaper_flush(vol->target_device, &batch, vol);

    /* 
     * 5. ZNS EVACUATOR (Zone Compaction)
     * Moves valid data out of fragmented zones so they can be reset.
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

                /* Calculate Safe Zone Start (Skip Metadata Regions) */
                uint64_t flux_start_sector = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
                uint64_t first_safe_zone = (flux_start_sector + zone_sectors - 1) / zone_sectors;

                /* Round-Robin Cursor */
                static uint64_t zone_cursor = 0;
                if (zone_cursor < first_safe_zone || zone_cursor >= total_zones) {
                    zone_cursor = first_safe_zone;
                }

                uint64_t victim_start_lba = zone_cursor * zone_sectors;
                uint64_t victim_len_bytes = caps->zone_size_bytes;

                /* Moves valid data to Horizon, then Resets Zone */
                _evacuate_zns_victim(vol, victim_start_lba, victim_len_bytes);

                zone_cursor++;
            }
        }
    }
}
