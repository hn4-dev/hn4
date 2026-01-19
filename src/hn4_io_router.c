/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Spatial Array Router (Hyper-Cloud Profile)
 * SOURCE:      hn4_io_router.c
 * STATUS:       RE-ENGINEERED (v2.7)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. SNAPSHOT ISOLATION: Topology is copied to stack under lock to prevent races.
 * 2. MIRRORING: Strict consensus. Failure of ANY online mirror degrades the volume.
 * 3. PARITY: Write DISABLED. Read employs symmetric XOR reconstruction.
 * 4. GEOMETRY: 128-bit overflow protection and stripe alignment checks.
 * 5. BOUNDARY SAFETY: Split IOs at stripe unit boundaries to prevent layout violation.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_errors.h"
#include "hn4_addr.h"
#include <string.h> /* memset, memcpy */

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */


 /* Fixed Geometry for HyperCloud Profile (64KB Stripe Unit / 512B Sector) */
#define HN4_RAID5_STRIPE_SECTORS 128 

/* 
 * HELPER: Fast XOR Buffer (Aliasing Safe)
 * Replaces direct pointer casting with memcpy to ensure strict aliasing compliance.
 * Modern compilers optimize this to unaligned vector loads/stores.
 */
static void _xor_buffer_fast(void* dst, const void* src, size_t len) {
    if (dst == src) return;

    uint8_t* d8 = (uint8_t*)dst;
    const uint8_t* s8 = (const uint8_t*)src;
    size_t i = 0;

    /* 
     * UNROLLED BLOCK LOOP (32 bytes per iteration)
     * We use memcpy to load/store 64-bit integers. 
     * This avoids SIGBUS/HardFault on architectures that enforce strict alignment 
     * (like ARMv7 or SPARC), while allowing the compiler to emit 
     * unaligned SIMD loads/stores on x86_64/ARM64.
     */
    while (i + 32 <= len) {
        uint64_t v_dst[4];
        uint64_t v_src[4];

        /* Bulk load 32 bytes */
        memcpy(v_dst, d8 + i, 32);
        memcpy(v_src, s8 + i, 32);

        /* XOR in registers */
        v_dst[0] ^= v_src[0];
        v_dst[1] ^= v_src[1];
        v_dst[2] ^= v_src[2];
        v_dst[3] ^= v_src[3];

        /* Bulk store 32 bytes */
        memcpy(d8 + i, v_dst, 32);
        i += 32;
    }

    /* 
     * SINGLE WORD LOOP (8 bytes per iteration)
     * Handle remaining chunks that didn't fit in the 32-byte block.
     */
    while (i + 8 <= len) {
        uint64_t v_d, v_s;
        
        memcpy(&v_d, d8 + i, 8);
        memcpy(&v_s, s8 + i, 8);
        
        v_d ^= v_s;
        
        memcpy(d8 + i, &v_d, 8);
        i += 8;
    }

    /* 
     * TAIL LOOP (Byte-wise)
     * Handle the final 0-7 bytes.
     */
    while (i < len) {
        d8[i] ^= s8[i];
        i++;
    }
}

static void _mark_device_offline(hn4_volume_t* vol, uint32_t dev_idx) {
    hn4_array_ctx_t* arr = &vol->array;
    if (dev_idx >= HN4_MAX_ARRAY_DEVICES) return;

    /* 
     * Atomic State Transition: ONLINE -> OFFLINE 
     * Uses GCC/Clang built-ins for portability without <stdatomic.h>
     */
    uint32_t expected = HN4_DEV_STAT_ONLINE;
    if (__atomic_compare_exchange_n(
            &arr->devices[dev_idx].status, 
            &expected, 
            HN4_DEV_STAT_OFFLINE, 
            false, 
            __ATOMIC_RELEASE, 
            __ATOMIC_RELAXED)) 
    {
        HN4_LOG_CRIT("ARRAY: Device %u marked OFFLINE due to Critical IO Failure.", dev_idx);
        
        /* Mark volume degraded and dirty to persist the failure state */
        __atomic_fetch_or(&vol->sb.info.state_flags, 
                          HN4_VOL_DEGRADED | HN4_VOL_DIRTY, 
                          __ATOMIC_RELEASE);
    }
}

static uint32_t _resolve_shard_index(hn4_u128_t file_id, uint32_t dev_count) {
    if (dev_count == 0) return 0;
    
    /* Mix hash to ensure distribution */
    uint64_t hash = file_id.lo ^ file_id.hi;
    hash ^= (hash >> 33);
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= (hash >> 33);
    
    return hash % dev_count;
}

static inline bool _is_io_success(hn4_result_t res) {
    return (res == HN4_OK || res == HN4_INFO_SPARSE || res == HN4_INFO_HEALED);
}

static inline bool _is_critical_failure(hn4_result_t res) {
    switch (res) {
        case HN4_ERR_HW_IO:
        case HN4_ERR_DATA_ROT:
        case HN4_ERR_MEDIA_TOXIC:
        case HN4_ERR_ATOMICS_TIMEOUT:
            return true;
        default:
            return false;
    }
}

/* =========================================================================
 * SPATIAL ROUTER (CORE DISPATCH)
 * ========================================================================= */

hn4_result_t _hn4_spatial_router(
    hn4_volume_t* vol, 
    uint8_t op, 
    hn4_addr_t lba, 
    void* buf, 
    uint32_t len, /* Length in SECTORS */
    hn4_u128_t file_id
) {
    /* 1. CHECK PROFILE & CONFIG */
    if (vol->sb.info.format_profile != HN4_PROFILE_HYPER_CLOUD) {
        return hn4_hal_sync_io(vol->target_device, op, lba, buf, len);
    }

    hn4_drive_t snapshot[HN4_MAX_ARRAY_DEVICES];
    uint32_t count = 0;
    uint32_t mode = 0;

    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    count = vol->array.count;
    mode  = vol->array.mode;
    
    if (count > 0 && count <= HN4_MAX_ARRAY_DEVICES) {
        memcpy(snapshot, vol->array.devices, sizeof(hn4_drive_t) * count);
    } else {
        count = 0; /* Safety against corruption */
    }
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    /* 
     * MEMORY BARRIER:
     * Ensure the snapshot copy is globally visible and ordered before we start IO.
     */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (count == 0) {
        return hn4_hal_sync_io(vol->target_device, op, lba, buf, len);
    }

    /* 
     * Check Device Tag OR Hardware Flags to enable seek optimizations.
     */
    bool is_hdd = (vol->sb.info.device_type_tag == HN4_DEV_HDD) || 
                  (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL);

    /* ZNS / USB DETECTION */
    bool is_zns = (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE);
    bool is_usb = (vol->sb.info.format_profile == HN4_PROFILE_USB);

    /* 2. DISPATCH BY MODE */
    switch (mode) {
        
        /* --- MODE 1: MIRROR (GRAVITY WELL ENTANGLEMENT) --- */
        case HN4_ARRAY_MODE_MIRROR: {
            if (op == HN4_IO_READ) {
                uint32_t start_idx = 0;

                /* HDD OPTIMIZATION: LBA Region Affinity */
                if (is_hdd) {
                    /* Map 1GB regions (approx 2^21 sectors) to specific mirrors. */
                    uint64_t region = hn4_addr_to_u64(lba) >> 21;
                    start_idx = region % count;
                }

                /* 4: Mirror Read Retry Policy */
                int attempts = 0;
                const int MAX_MIRROR_RETRIES = 2;

                while (attempts < MAX_MIRROR_RETRIES) {
                    /* READ: Try mirrors sequentially starting from preferred index */
                    for (uint32_t k = 0; k < count; k++) {
                        uint32_t i = (start_idx + k) % count; /* Wrap around */

                        if (snapshot[i].status != HN4_DEV_STAT_ONLINE) continue;

                        hn4_result_t res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);
                        if (_is_io_success(res)) return HN4_OK;
                        
                        /* Critical failure -> Offline -> Degraded Read */
                        if (_is_critical_failure(res)) {
                            _mark_device_offline(vol, i);
                            /* Sync Local Snapshot */
                            snapshot[i].status = HN4_DEV_STAT_OFFLINE;
                        }
                    }
                    /* All online mirrors failed. Sleep and retry if allowed. */
                    attempts++;
                    if (attempts < MAX_MIRROR_RETRIES) hn4_hal_micro_sleep(1000);
                }
                return HN4_ERR_HW_IO;
            } 
            else { 
                /* WRITE / FLUSH / DISCARD */
                int success_count = 0;
                int online_targets = 0;

                for (uint32_t i = 0; i < count; i++) {
                    if (snapshot[i].status != HN4_DEV_STAT_ONLINE) continue;
                    
                    online_targets++;
                    hn4_result_t res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);

                    /* USB OPTIMIZATION: Transient Failure Retry */
                    if (is_usb && !_is_io_success(res) && res != HN4_ERR_MEDIA_TOXIC) {
                        hn4_hal_micro_sleep(5000); /* 5ms chill */
                        res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);
                    }
                    
                    if (_is_io_success(res)) {
                        success_count++;
                    } else {
                        HN4_LOG_CRIT("Mirror Write Failed Dev %u (%d).", i, res);
                        if (_is_critical_failure(res)) {
                            _mark_device_offline(vol, i);
                            /* Sync Local Snapshot */
                            snapshot[i].status = HN4_DEV_STAT_OFFLINE;
                        }
                    }
                }

                /* STRICT CONSENSUS */
                if (online_targets > 0 && success_count == online_targets) {
                    return HN4_OK;
                }
                
                /* 
                 * NOTE: TRANSACTIONAL DIVERGENCE WARNING
                 * If we reach here, some mirrors succeeded (committed) while others failed.
                 * The mirrors are now divergent. 
                 * HN4 relies on the higher-level "Epoch/Generation" check to resolve conflicts
                 * during the next mount/read. We do not rollback partial writes here.
                 */
                return HN4_ERR_HW_IO;
            }
        }

        /* --- MODE 2: SHARD (BALLISTIC STRIPING) --- */
        case HN4_ARRAY_MODE_SHARD: {
            uint32_t target_idx;

            /* HDD OPTIMIZATION: Locality-Aware Sharding */
           bool is_v7 = ((file_id.hi >> 12) & 0xF) == 7;

            if (is_hdd && is_v7) {
                /* Use UUID Timestamp for temporal locality */
                target_idx = file_id.hi % count;
            } else {
                /* SSD or Random UUID: Use Entropy Hash */
                target_idx = _resolve_shard_index(file_id, count);
            }
            
            if (snapshot[target_idx].status != HN4_DEV_STAT_ONLINE) {
                return HN4_ERR_HW_IO;
            }

            hn4_hal_device_t* dev = snapshot[target_idx].dev_handle;
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
            if (!caps) return HN4_ERR_INTERNAL_FAULT;

            uint32_t ss = caps->logical_block_size;
            if (ss == 0) return HN4_ERR_GEOMETRY;

            /* ZNS OPTIMIZATION: Auto-Upgrade to Zone Append */
            if (is_zns && op == HN4_IO_WRITE) {
                if (lba % (caps->zone_size_bytes / ss) == 0) op = HN4_IO_ZONE_APPEND;
            }

            /* Skip tight bounds check for Zone Append (Drive handles it) */
            if (op != HN4_IO_ZONE_APPEND) {
                #ifdef HN4_USE_128BIT
                    hn4_u128_t cap_bytes = caps->total_capacity_bytes;
                    hn4_u128_t max_sectors = hn4_u128_div_u64(cap_bytes, ss);
                    
                    hn4_u128_t req_start = lba;
                    hn4_u128_t req_len = hn4_u128_from_u64(len);
                    hn4_u128_t req_end = hn4_u128_add(req_start, req_len);
                    
                    if (hn4_u128_cmp(req_end, req_start) < 0) return HN4_ERR_GEOMETRY;
                    if (hn4_u128_cmp(req_end, max_sectors) > 0) return HN4_ERR_GEOMETRY;
                #else
                    /* Safe 64-bit comparison */
                    uint64_t max_sectors = caps->total_capacity_bytes / ss;
                    if (len > max_sectors || lba > (max_sectors - len)) {
                        return HN4_ERR_GEOMETRY;
                    }
                #endif
            }

            hn4_result_t res = hn4_hal_sync_io(dev, op, lba, buf, len);
            if (!_is_io_success(res)) {
                if (_is_critical_failure(res)) {
                    _mark_device_offline(vol, target_idx);
                    /* Sync Local Snapshot */
                    snapshot[target_idx].status = HN4_DEV_STAT_OFFLINE;
                }
                return res;
            }
            return HN4_OK;
        }

        /* --- MODE 3: PARITY (RAID-5 CONSTELLATION) --- */
        case HN4_ARRAY_MODE_PARITY: {
            /* ZNS PROTECTION: Parity requires RMW, which ZNS prohibits. */
            if (is_zns) return HN4_ERR_PROFILE_MISMATCH;

            if (op != HN4_IO_READ) {
                /* 
                 * WRITE DISABLED: 
                 * RAID-5 Write Hole protection is not implemented in the Router.
                 * Writes must use Log-Structured allocation or Full Stripe Writes
                 * managed by the Allocator, not this low-level Router.
                 */
                return HN4_ERR_ACCESS_DENIED; 
            }

            /* Cache Geometry Up Front */
            const hn4_hal_caps_t* primary_caps = hn4_hal_get_caps(snapshot[0].dev_handle);
            if (!primary_caps) return HN4_ERR_INTERNAL_FAULT;
            uint32_t stripe_ss = primary_caps->logical_block_size;

            uint32_t data_disks = count - 1;
            if (data_disks == 0) return HN4_ERR_GEOMETRY;

            uint32_t stripe_unit = HN4_RAID5_STRIPE_SECTORS;

            uint64_t stride = (uint64_t)data_disks * stripe_unit;
            if (stride / stripe_unit != data_disks) return HN4_ERR_GEOMETRY;

            /* 
             * STRIPE CROSSING LOOP (Splits IO at boundaries)
             */
            hn4_addr_t current_lba = lba;
            uint32_t   current_len = len;
            uint8_t*   current_buf = (uint8_t*)buf;

            while (current_len > 0) {
                /* Calculate offset within the LOGICAL stripe row */
                #ifdef HN4_USE_128BIT
                hn4_u128_t rem_val = hn4_u128_mod(current_lba, hn4_u128_from_u64(stride));
                uint64_t offset_in_stripe = rem_val.lo;
                #else
                uint64_t offset_in_stripe = current_lba % stride;
                #endif

                /* Calculate Split Point (Stripe Alignment) */
                uint32_t offset_in_col = (uint32_t)(offset_in_stripe % stripe_unit);
                uint32_t sectors_left_in_chunk = stripe_unit - offset_in_col;
                uint32_t chunk_len = (current_len < sectors_left_in_chunk) ? current_len : sectors_left_in_chunk;

                /* --- BEGIN CHUNK PROCESSING --- */
                
                /* Geometry Mapping for THIS chunk */
                #ifdef HN4_USE_128BIT
                hn4_u128_t lba_128 = current_lba;
                hn4_u128_t row_div = hn4_u128_div_u64(lba_128, stride);
                if (row_div.hi > 0) return HN4_ERR_GEOMETRY;
                uint64_t stripe_row = row_div.lo;
                #else
                uint64_t stripe_row = current_lba / stride;
                #endif
                
                uint32_t col_idx = (uint32_t)(offset_in_stripe / stripe_unit);
                uint32_t parity_col = (uint32_t)((count - 1) - (stripe_row % count));
                
                uint32_t phys_col = col_idx;
                if (phys_col >= parity_col) phys_col++; 

                if (phys_col >= count) return HN4_ERR_INTERNAL_FAULT;

                /* Calculate Physical LBA */
                hn4_addr_t target_lba;
                
                #ifdef HN4_USE_128BIT
                hn4_u128_t row_u128 = hn4_u128_from_u64(stripe_row);
                hn4_u128_t base_u128 = hn4_u128_mul_u64(row_u128, stripe_unit);
                hn4_u128_t phys_lba_128 = base_u128;
                uint64_t old_lo = phys_lba_128.lo;
                phys_lba_128.lo += offset_in_col;
                if (phys_lba_128.lo < old_lo) phys_lba_128.hi++;
                target_lba = phys_lba_128;
                #else
                if (stripe_row > (UINT64_MAX / stripe_unit)) return HN4_ERR_GEOMETRY;
                uint64_t phys_block_base = stripe_row * stripe_unit;
                if ((UINT64_MAX - phys_block_base) < offset_in_col) return HN4_ERR_GEOMETRY;
                target_lba = phys_block_base + offset_in_col;
                #endif

                hn4_hal_device_t* phys_dev_handle = snapshot[phys_col].dev_handle;
                const hn4_hal_caps_t* p_caps = hn4_hal_get_caps(phys_dev_handle);
                
                if (p_caps) {
                    #ifdef HN4_USE_128BIT
                    hn4_u128_t dev_cap = p_caps->total_capacity_bytes;
                    uint32_t dev_ss = p_caps->logical_block_size;
                    hn4_u128_t dev_limit = hn4_u128_div_u64(dev_cap, dev_ss);
                    hn4_u128_t req_end = hn4_u128_add(target_lba, hn4_u128_from_u64(chunk_len));
                    if (hn4_u128_cmp(req_end, dev_limit) > 0) return HN4_ERR_GEOMETRY;
                    #else
                    uint64_t dev_limit = p_caps->total_capacity_bytes / p_caps->logical_block_size;
                    if (chunk_len > dev_limit || target_lba > (dev_limit - chunk_len)) return HN4_ERR_GEOMETRY;
                    #endif
                } else {
                    return HN4_ERR_INTERNAL_FAULT;
                }

                /* Fast Path: Read from Data Disk with Retry */
                int retries = 0;
                const int MAX_RETRIES = 2;
                bool read_success = false;
                
                while (retries++ < MAX_RETRIES) {
                    if (snapshot[phys_col].status == HN4_DEV_STAT_ONLINE) {
                        hn4_result_t res = hn4_hal_sync_io(phys_dev_handle, HN4_IO_READ, target_lba, current_buf, chunk_len);
                        
                        if (_is_io_success(res)) {
                            read_success = true;
                            break;
                        }
                        
                        /* Critical failure -> Offline -> Degraded Read */
                        if (_is_critical_failure(res)) {
                            _mark_device_offline(vol, phys_col);
                            snapshot[phys_col].status = HN4_DEV_STAT_OFFLINE;
                            break; 
                        }
                        /* Transient failure, loop loops */
                    } else {
                        break; 
                    }
                }

                if (!read_success) {
                    /* 
                     * DEGRADED READ (XOR Reconstruction)
                     */
                    HN4_LOG_WARN("RAID-5 Degraded Read: Reconstructing Stripe %llu", (unsigned long long)stripe_row);

                    uint64_t total_bytes_u64 = (uint64_t)chunk_len * stripe_ss;
                    if (total_bytes_u64 > (size_t)-1) return HN4_ERR_NOMEM;
                    size_t total_bytes = (size_t)total_bytes_u64;
                    
                    void* peer_buf = hn4_hal_mem_alloc(total_bytes);
                    if (!peer_buf) return HN4_ERR_NOMEM;

                    memset(current_buf, 0, total_bytes);

                    bool reconstruction_ok = true;
                    uint32_t missing_col = phys_col;

                    for (uint32_t c = 0; c < count; c++) {
                        if (c == missing_col) continue;

                        /* Check peer health */
                        if (snapshot[c].status != HN4_DEV_STAT_ONLINE) {
                            HN4_LOG_CRIT("RAID-5 Double Fault at Stripe %llu. Data Lost.", (unsigned long long)stripe_row);
                            reconstruction_ok = false;
                            break;
                        }

                        int peer_retries = 0;
                        hn4_result_t peer_res = HN4_ERR_HW_IO;
                        
                        while (peer_retries++ < MAX_RETRIES) {
                            peer_res = hn4_hal_sync_io(snapshot[c].dev_handle, HN4_IO_READ, target_lba, peer_buf, chunk_len);
                            if (_is_io_success(peer_res)) break;
                        }

                        if (!_is_io_success(peer_res)) {
                            HN4_LOG_CRIT("RAID-5 Peer Read Failed Dev %u. Double Fault.", c);
                            if (_is_critical_failure(peer_res)) {
                                _mark_device_offline(vol, c);
                                snapshot[c].status = HN4_DEV_STAT_OFFLINE; /* Sync Local */
                            }
                            reconstruction_ok = false;
                            break;
                        }

                        /* Safe Fast XOR Accumulate */
                        _xor_buffer_fast(current_buf, peer_buf, total_bytes);
                    }

                    hn4_hal_mem_free(peer_buf);

                    if (!reconstruction_ok) return HN4_ERR_PARITY_BROKEN;

                    /* We can only validate if the read was for a full block size */
                    if (chunk_len * stripe_ss == vol->vol_block_size) {
                        hn4_block_header_t* r_hdr = (hn4_block_header_t*)current_buf;
                        if (hn4_le32_to_cpu(r_hdr->magic) == HN4_BLOCK_MAGIC) {
                            /* Verify Header CRC to catch bad reconstruction */
                            uint32_t stored = hn4_le32_to_cpu(r_hdr->header_crc);
                            /* Note: We use 0 seed here as router lacks context of seeds */
                            uint32_t calc = hn4_crc32(HN4_CRC_SEED_HEADER, r_hdr, offsetof(hn4_block_header_t, header_crc));
                            
                            if (stored != calc) {
                                HN4_LOG_CRIT("RAID-5 Reconstruction resulted in Bad CRC. Stripe Lost.");
                                return HN4_ERR_PARITY_BROKEN;
                            }
                        }
                    }
                }

                /* --- END CHUNK PROCESSING --- */

                /* Advance cursors */
                #ifdef HN4_USE_128BIT
                current_lba = hn4_u128_add(current_lba, hn4_u128_from_u64(chunk_len));
                #else
                current_lba += chunk_len;
                #endif
                
                current_len -= chunk_len;
                current_buf += (chunk_len * stripe_ss); 
            }

            return HN4_OK;
        }

        default:
            return HN4_ERR_INTERNAL_FAULT;
    }
}