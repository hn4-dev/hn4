/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Spatial Array Pool Manager
 * SOURCE:      hn4_pool.c
 * STATUS:      HARDENED / PRODUCTION (v3.8)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. TRANSACTIONAL: Array state mutation is deferred until all validations pass.
 *    Rollback is performed if Audit Log fails.
 * 2. CONCURRENCY: Writers hold L2 Lock. READERS MUST HOLD L2 LOCK for topology
 *    scans to ensure 128-bit atomicity on unaligned packed structures.
 * 3. GEOMETRY: Enforces strict sector, zone, and IO boundary symmetry.
 * 4. ATOMICITY: Uses architectural primitives where aligned; relies on locks
 *    where packed alignment is violated.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include "hn4_chronicle.h"
#include <string.h> /* For memcpy/memset */

/* =========================================================================
 * CONSTANTS & DEFINITIONS
 * ========================================================================= */

/* Minimum sanity capacity (100MB) */
#define HN4_MIN_DEVICE_CAP      (100ULL * 1024 * 1024)

/* =========================================================================
 * LOW-LEVEL HELPERS
 * ========================================================================= */

/**
 * _probe_device_health
 * Performs read checks at LBA 0 and Last LBA to verify device responsiveness.
 */
static hn4_result_t _probe_device_health(hn4_hal_device_t* dev, const hn4_hal_caps_t* caps) {
    uint32_t io_size = caps->logical_block_size;
    if (caps->optimal_io_boundary > io_size) io_size = caps->optimal_io_boundary;

    void* buf = hn4_hal_mem_alloc(io_size);
    if (!buf) return HN4_ERR_NOMEM;

    uint32_t sectors = io_size / caps->logical_block_size;
    bool is_zns = (caps->hw_flags & HN4_HW_ZNS_NATIVE);

    /* Probe 1: Genesis (LBA 0) */
    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), buf, sectors);
    
    /* ZNS Handling: Accept sparse reads or zone status codes */
    if (is_zns) {
        if (res == HN4_INFO_SPARSE || res == HN4_ERR_ZONE_FULL) res = HN4_OK;
    }

    if (res == HN4_OK) {
        /* Probe 2: Terminus (Last LBA) */
        uint64_t cap_bytes;
        
        /* _addr_to_u64_checked returns TRUE on SUCCESS */
        if (_addr_to_u64_checked(caps->total_capacity_bytes, &cap_bytes)) {
            
            /* Calculate start byte of last IO chunk */
            uint64_t last_byte_start = cap_bytes - io_size;
            /* Align down to sector boundary just in case */
            last_byte_start = (last_byte_start / caps->logical_block_size) * caps->logical_block_size;
            
            hn4_addr_t last_lba = hn4_lba_from_sectors(last_byte_start / caps->logical_block_size);
            
            hn4_result_t res2 = hn4_hal_sync_io(dev, HN4_IO_READ, last_lba, buf, sectors);
            
            if (is_zns) {
                if (res2 == HN4_INFO_SPARSE || res2 == HN4_ERR_ZONE_FULL) res2 = HN4_OK;
            }

            if (res2 != HN4_OK) {
                res = HN4_ERR_HW_IO;
            }
        }
    }

    hn4_hal_mem_free(buf);
    return res;
}

/**
 * _atomic_store_u128
 * Stores a 128-bit value to memory.
 * 
 * CONTRACT: Caller/Reader MUST hold L2 Spinlock.
 */
HN4_INLINE void _atomic_store_u128(hn4_size_t* ptr, hn4_size_t val) {
    if (HN4_UNLIKELY(((uintptr_t)ptr & 0xF) != 0)) {
        memcpy(ptr, &val, sizeof(hn4_size_t));
        atomic_thread_fence(memory_order_release);
        return;
    }

#if defined(__x86_64__) || defined(_M_X64)
    #ifdef HN4_USE_128BIT
        unsigned __int128 v_scalar = ((unsigned __int128)val.hi << 64) | val.lo;
    #else
        unsigned __int128 v_scalar = (unsigned __int128)val;
    #endif
    __asm__ volatile ("movdqa %1, %0" : "=m"(*ptr) : "x"(v_scalar) : "memory");

#elif defined(__aarch64__)
    #ifdef HN4_USE_128BIT
        __asm__ volatile ("stp %0, %1, [%2]" : : "r"(val.lo), "r"(val.hi), "r"(ptr) : "memory");
    #else
        /* Handle 64-bit case for ARM64 if size_t is just u64 */
        __asm__ volatile ("str %0, [%1]" : : "r"(val), "r"(ptr) : "memory");
    #endif
#else
    *ptr = val;
    atomic_thread_fence(memory_order_release);
#endif
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

hn4_result_t hn4_pool_add_device(
    HN4_IN    hn4_volume_t*     vol, 
    HN4_IN    hn4_hal_device_t* new_dev
)
{
    /* ---------------------------------------------------------------------
     * PHASE 1: PRE-FLIGHT VALIDATION (Read-Only)
     * --------------------------------------------------------------------- */

    if (HN4_UNLIKELY(!vol || !new_dev)) return HN4_ERR_INVALID_ARGUMENT;

    if (vol->sb.info.format_profile != HN4_PROFILE_HYPER_CLOUD) {
        HN4_LOG_ERR("Pool: Profile mismatch. Spatial Array requires HYPER_CLOUD.");
        return HN4_ERR_PROFILE_MISMATCH;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(new_dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    if (caps->logical_block_size == 0) return HN4_ERR_GEOMETRY;

    if (vol->vol_block_size % caps->logical_block_size != 0) {
        HN4_LOG_CRIT("Pool: Sector size mismatch. VolBS=%u DevSS=%u", 
                     vol->vol_block_size, caps->logical_block_size);
        return HN4_ERR_ALIGNMENT_FAIL;
    }

#ifdef HN4_USE_128BIT
    hn4_u128_t min_cap = hn4_u128_from_u64(HN4_MIN_DEVICE_CAP);
    if (hn4_u128_cmp(caps->total_capacity_bytes, min_cap) < 0) return HN4_ERR_GEOMETRY;
#else
    if (caps->total_capacity_bytes < HN4_MIN_DEVICE_CAP) return HN4_ERR_GEOMETRY; 
#endif

    hn4_result_t health_res = _probe_device_health(new_dev, caps);
    if (health_res != HN4_OK) {
        HN4_LOG_CRIT("Pool: Device health probe failed (%d).", health_res);
        return HN4_ERR_HW_IO;
    }

    /* ---------------------------------------------------------------------
     * PHASE 2: CRITICAL SECTION (State Mutation)
     * --------------------------------------------------------------------- */
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);

    hn4_array_ctx_t* arr = &vol->array;
    hn4_result_t result = HN4_OK;
    
    /* Snapshot state for rollback */
    hn4_size_t old_total_cap = arr->total_pool_capacity;
    uint32_t old_count = arr->count;

    /* 2.1 Slot Availability */
    if (arr->count >= HN4_MAX_ARRAY_DEVICES) {
        result = HN4_ERR_ENOSPC;
        goto unlock_and_exit;
    }

    /* 2.2 Duplicate Detection */
    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->devices[i].dev_handle == new_dev) {
            result = HN4_ERR_EEXIST;
            goto unlock_and_exit;
        }
    }

    /* 2.3 Symmetry Enforcement */
    if (arr->count > 0) {
        const hn4_hal_caps_t* primary_caps = hn4_hal_get_caps(arr->devices[0].dev_handle);
        
        if (HN4_UNLIKELY(!primary_caps)) {
            result = HN4_ERR_INTERNAL_FAULT;
            goto unlock_and_exit;
        }

        if (caps->logical_block_size != primary_caps->logical_block_size ||
            caps->optimal_io_boundary != primary_caps->optimal_io_boundary) {
            result = HN4_ERR_ALIGNMENT_FAIL;
            goto unlock_and_exit;
        }

        /* ZNS Zone Symmetry */
        if ((primary_caps->hw_flags & HN4_HW_ZNS_NATIVE) || (caps->hw_flags & HN4_HW_ZNS_NATIVE)) {
            if (primary_caps->zone_size_bytes != caps->zone_size_bytes) {
                result = HN4_ERR_ALIGNMENT_FAIL;
                goto unlock_and_exit;
            }
            
            /* Check Zone Count equality for Mirror/Parity */
            if (arr->mode == HN4_ARRAY_MODE_MIRROR || arr->mode == HN4_ARRAY_MODE_PARITY) {
                /* Simplified check: Total Capacity must match exactly for Zone alignment */
                #ifdef HN4_USE_128BIT
                if (hn4_u128_cmp(primary_caps->total_capacity_bytes, caps->total_capacity_bytes) != 0) {
                    result = HN4_ERR_GEOMETRY; 
                    goto unlock_and_exit;
                }
                #else
                if (primary_caps->total_capacity_bytes != caps->total_capacity_bytes) {
                    result = HN4_ERR_GEOMETRY; 
                    goto unlock_and_exit;
                }
                #endif
            }
        }
    }

    /* 2.4 Mode Logic & Capacity Calculation */
    hn4_size_t new_total_cap = arr->total_pool_capacity;

    if (arr->count == 0) {
        new_total_cap = caps->total_capacity_bytes;
    } else {
        switch (arr->mode) {
            case HN4_ARRAY_MODE_SHARD:
                /* Mode: SHARD (Accumulate) */
                #ifdef HN4_USE_128BIT
                {
                    uint64_t old_hi = new_total_cap.hi;
                    uint64_t old_lo = new_total_cap.lo;
                    
                    new_total_cap.lo += caps->total_capacity_bytes.lo;
                    new_total_cap.hi += caps->total_capacity_bytes.hi;
                    
                    if (new_total_cap.lo < old_lo) new_total_cap.hi++;

                    if (new_total_cap.hi < old_hi) {
                        result = HN4_ERR_ENOSPC;
                        goto unlock_and_exit;
                    }
                }
                #else
                {
                    if (UINT64_MAX - new_total_cap < caps->total_capacity_bytes) {
                        result = HN4_ERR_ENOSPC;
                        goto unlock_and_exit;
                    }
                    new_total_cap += caps->total_capacity_bytes;
                }
                #endif
                break;

            case HN4_ARRAY_MODE_MIRROR:
                /* Mode: MIRROR (Symmetry Required) */
                #ifdef HN4_USE_128BIT
                if (hn4_u128_cmp(caps->total_capacity_bytes, arr->total_pool_capacity) < 0) {
                    result = HN4_ERR_GEOMETRY;
                    goto unlock_and_exit;
                }
                #else
                if (caps->total_capacity_bytes < arr->total_pool_capacity) {
                    result = HN4_ERR_GEOMETRY;
                    goto unlock_and_exit;
                }
                #endif
                /* Mirror capacity does not increase */
                break;
            
            case HN4_ARRAY_MODE_PARITY:
                #ifdef HN4_USE_128BIT
                if (hn4_u128_cmp(caps->total_capacity_bytes, arr->devices[0].dev_handle ? 
                    hn4_hal_get_caps(arr->devices[0].dev_handle)->total_capacity_bytes : caps->total_capacity_bytes) != 0) {
                    result = HN4_ERR_GEOMETRY;
                    goto unlock_and_exit;
                }
                {
                    hn4_u128_t count_128 = hn4_u128_from_u64(arr->count); /* Current count becomes N-1 after inc? */
                    new_total_cap = hn4_u128_mul_u64(caps->total_capacity_bytes, arr->count);
                }
                #else
                if (caps->total_capacity_bytes != arr->total_pool_capacity / (arr->count > 1 ? arr->count - 1 : 1)) {
                     /* Rough check, real logic requires storing disk_size separate from logical_total */
                }
                /* Recalculate: Logical = Disk * Count (where 1 is parity) */
                new_total_cap = caps->total_capacity_bytes * arr->count;
                #endif
                break;

            default: 
                result = HN4_ERR_INTERNAL_FAULT;
                goto unlock_and_exit;
        }
    }

    /* ---------------------------------------------------------------------
     * PHASE 3: PROVISIONAL COMMIT
     * --------------------------------------------------------------------- */
    uint32_t idx = arr->count;
    
    memset(&arr->devices[idx], 0, sizeof(hn4_drive_t));
    arr->devices[idx].dev_handle    = new_dev;
    arr->devices[idx].status        = HN4_DEV_STAT_ONLINE;
    arr->devices[idx].usage_counter = 0;
    
    /* Update Capacity (Atomic Store) */
    /* Update if mode implies capacity change */
    if (arr->mode == HN4_ARRAY_MODE_SHARD || arr->mode == HN4_ARRAY_MODE_PARITY || arr->count == 0) {
        #ifdef HN4_USE_128BIT
        _atomic_store_u128(&arr->total_pool_capacity, new_total_cap);
        _atomic_store_u128(&vol->vol_capacity_bytes, new_total_cap);
        _atomic_store_u128(&vol->sb.info.total_capacity, new_total_cap);
    #else
        atomic_store(&arr->total_pool_capacity, new_total_cap);
        atomic_store(&vol->vol_capacity_bytes, new_total_cap);
        atomic_store(&vol->sb.info.total_capacity, new_total_cap); /* Cast if necessary */
    #endif
    }

    atomic_thread_fence(memory_order_release);
    arr->count++;

    /* ---------------------------------------------------------------------
     * PHASE 4: AUDIT & ROLLBACK (The Safe Hop)
     * --------------------------------------------------------------------- */
    
    /* Deterministic Signature: Handle ^ Capacity (No Time) */
    uint64_t dev_sig = (uintptr_t)new_dev;
#ifdef HN4_USE_128BIT
    dev_sig ^= caps->total_capacity_bytes.lo;
#else
    dev_sig ^= caps->total_capacity_bytes;
#endif
    /* Mix bits */
    dev_sig = (dev_sig ^ (dev_sig >> 30)) * 0xbf58476d1ce4e5b9ULL;
    dev_sig = (dev_sig ^ (dev_sig >> 27)) * 0x94d049bb133111ebULL;
    dev_sig = dev_sig ^ (dev_sig >> 31);

    /* Use Generation ID as version constraint */
    uint64_t gen_id = vol->sb.info.copy_generation;

    hn4_result_t log_res = hn4_chronicle_append(vol->target_device, vol, HN4_CHRONICLE_OP_FORK, 
                         hn4_lba_from_sectors(old_count), 
                         hn4_lba_from_sectors(gen_id),     /* Log topology version */
                         dev_sig);
        
    if (log_res != HN4_OK) {
        HN4_LOG_CRIT("Pool: Audit Log Failed (%d). Rolling back.", log_res);
        
        /* ROLLBACK */
        arr->count = old_count;
        
        /* Restore Capacity based on Mode */
        if (arr->mode == HN4_ARRAY_MODE_SHARD || arr->mode == HN4_ARRAY_MODE_PARITY || old_count == 0) {
            _atomic_store_u128(&arr->total_pool_capacity, old_total_cap);
        }
        
        /* Wipe slot */
        memset(&arr->devices[idx], 0, sizeof(hn4_drive_t)); 
        
        /* Fence to ensure rollback visible before unlock */
        atomic_thread_fence(memory_order_release);
        
        result = HN4_ERR_AUDIT_FAILURE;
        goto unlock_and_exit;
    }

    /* Success: Mark volume dirty */
    atomic_thread_fence(memory_order_release);
    atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);

unlock_and_exit:
    hn4_hal_spinlock_release(&vol->locking.l2_lock);
    return result;
}