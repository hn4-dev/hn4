/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Ballistic Nano-Storage
 * SOURCE:      hn4_nano.c
 * STATUS:      HARDENED / PRODUCTION (v3.3)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. ATOMICITY:
 *    Nano writes perform a "Double Commit" strategy:
 *    (1) Write Nano Sector -> (2) Durability Fence -> (3) Update & Persist Anchor.
 *    This ensures that a crash between (1) and (3) leaves the Anchor pointing 
 *    to the old state (or empty state), making the new write a harmless "orphan".
 *
 * 2. DURABILITY:
 *    Relies on hn4_hal_barrier() strictly enforcing NVMe FLUSH / FUA semantics.
 *
 * 3. INTEGRITY:
 *    CRC32C is seeded with (ID ^ Gen ^ UUID ^ Epoch) to bind data to Identity,
 *    Time, and Volume Context. This prevents cross-volume replay attacks and
 *    ensures data validity is tied to the specific Epoch ring state.
 *
 * 4. COMPATIBILITY:
 *    Strictly rejects Linear Media (ZNS/HDD/Tape). Nano-storage requires O(1)
 *    random write access to hash slots without read-modify-write penalties.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include "hn4_anchor.h" 
#include <string.h>
#include <stdatomic.h>

/* =========================================================================
 * 0. CONFIGURATION & CONSTANTS
 * ========================================================================= */

#define HN4_NANO_MAX_ORBITS     8       
#define HN4_NANO_RETRY_IO       3
#define HN4_NANO_MAGIC_SEED     0xff51afd7ed558ccdULL 

/* Validation States for Read Pipeline */
typedef enum {
    NANO_VAL_OK = 0,
    NANO_VAL_MAGIC_FAIL,
    NANO_VAL_ID_MISMATCH,
    NANO_VAL_GEN_SKEW,
    NANO_VAL_SIZE_INVALID,
    NANO_VAL_CRC_FAIL,
    NANO_VAL_EPOCH_MISMATCH
} hn4_nano_val_status_t;

/* Alignment Safety Assertions */
_Static_assert(sizeof(hn4_nano_quantum_t) <= 512, "HN4: Nano Quantum struct exceeds 512 bytes");
_Static_assert(offsetof(hn4_nano_quantum_t, payload) % 8 == 0, "HN4: Nano payload alignment violation");

/* =========================================================================
 * 1. COMPATIBILITY TABLES (LOOKUP OPTIMIZATION)
 * ========================================================================= */

/* 
 * Device Type Compatibility Table.
 * Index corresponds to hn4_sb.info.device_type_tag.
 * 
 * Logic: Only SSDs allow efficient small random writes.
 * HDD/ZNS/Tape require sequential streams (Horizon).
 */
static const bool _nano_dev_compat_lut[4] = {
    [HN4_DEV_SSD]  = true,
    [HN4_DEV_HDD]  = false,
    [HN4_DEV_ZNS]  = false,
    [HN4_DEV_TAPE] = false
};

/*
 * Profile Compatibility Table.
 * Index corresponds to hn4_sb.info.format_profile.
 * 
 * Logic: Archive forbids fragmentation. All other profiles allow Nano.
 */
static const bool _nano_prof_compat_lut[8] = {
    [HN4_PROFILE_GENERIC]     = true,
    [HN4_PROFILE_GAMING]      = true,
    [HN4_PROFILE_AI]          = true,
    [HN4_PROFILE_ARCHIVE]     = false, /* Cold storage -> No small objects */
    [HN4_PROFILE_PICO]        = true,
    [HN4_PROFILE_SYSTEM]      = true,
    [HN4_PROFILE_USB]         = true,
    [HN4_PROFILE_HYPER_CLOUD] = true
};

/* =========================================================================
 * 2. INTERNAL HELPERS
 * ========================================================================= */

/**
 * _is_nano_compatible
 * Determines if the underlying media supports efficient Random Small I/O.
 * Used to reject ZNS, HDD, and Tape devices which must use the Horizon log.
 * 
 * OPTIMIZATION: Uses O(1) table lookup instead of branching logic.
 */
static bool _is_nano_compatible(hn4_volume_t* vol) {
    uint32_t type = vol->sb.info.device_type_tag & 0x3; /* Safe clamp */
    uint32_t prof = vol->sb.info.format_profile & 0x7;  /* Safe clamp */
    uint64_t caps = vol->sb.info.hw_caps_flags;

    /* 
     * Hardware Flag Override:
     * Even if Device Type says SSD, if it reports Rotational or ZNS Native,
     * we must reject it.
     */
    if (caps & (HN4_HW_ZNS_NATIVE | HN4_HW_ROTATIONAL)) return false;

    /* Table Lookup */
    return _nano_dev_compat_lut[type] && _nano_prof_compat_lut[prof];
}

/**
 * _nano_secure_zero
 * Optimized volatile zeroing with alignment checks.
 * Ensures padding bytes in structs do not leak kernel stack data to disk.
 */
static void _nano_secure_zero(void* ptr, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    
    /* Phase 1: Align to 8 bytes */
    while (size > 0 && ((uintptr_t)p & 7)) {
        *p++ = 0; size--;
    }

    /* Phase 2: Bulk 64-bit zeroing (if aligned) */
    if (((uintptr_t)p & 7) == 0) {
        volatile uint64_t* p64 = (volatile uint64_t*)p;
        while (size >= 8) { 
            *p64++ = 0; 
            size -= 8; 
        }
        p = (volatile uint8_t*)p64;
    }

    /* Phase 3: Tail bytes */
    while (size--) { 
        *p++ = 0; 
    }
    
    /* Prevent compiler reordering/DCE */
    atomic_thread_fence(memory_order_seq_cst);
}

static void _safe_free(void* ptr, size_t size) {
    if (ptr) {
        _nano_secure_zero(ptr, size);
        hn4_hal_mem_free(ptr);
    }
}

/**
 * _calc_nano_crc
 * Binds content to Identity, Sequence, Volume UUID, and Epoch.
 * This cryptographic binding ensures that a block from Volume A cannot be 
 * replayed onto Volume B, nor can an old Epoch's data be confused with new.
 */
static uint32_t _calc_nano_crc(
    hn4_volume_t* vol,
    hn4_u128_t id, 
    uint64_t sequence, 
    uint64_t epoch_salt,
    const void* data, 
    uint32_t len
) {
    /* Mix entropy sources into a 64-bit seed */
    uint64_t s = id.lo ^ id.hi ^ sequence ^ vol->sb.info.volume_uuid.lo;
    s ^= epoch_salt;
    
    /* Fold entropy to 32-bits for CRC seed */
    s ^= (s >> 32); 
    
    return hn4_crc32((uint32_t)s, data, len);
}

/**
 * _calc_nano_trajectory
 * Calculates the physical LBA for a Nano object slot.
 * Uses triangular probing to determine slot offsets based on orbit 'k'.
 */
static hn4_result_t _calc_nano_trajectory(
    hn4_volume_t* vol,
    hn4_u128_t seed_id,
    uint32_t orbit_k,
    hn4_addr_t* out_lba
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    uint32_t ss = caps->logical_block_size;
    if (ss == 0) return HN4_ERR_GEOMETRY;

    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);

    /* Underflow Protection */
    if (HN4_UNLIKELY(end_sect <= start_sect)) return HN4_ERR_GEOMETRY;

    uint64_t capacity = end_sect - start_sect;
    if (capacity < HN4_NANO_MAX_ORBITS) return HN4_ERR_ENOSPC;

    /* Avalanche Mixer for Seed (Determinism) */
    uint64_t h = seed_id.lo ^ seed_id.hi;
    h ^= vol->sb.info.volume_uuid.lo; 
    h ^= (h >> 33);
    h *= HN4_NANO_MAGIC_SEED;
    h ^= (h >> 33);

    uint64_t target_idx;

    /* Check if Capacity is Power of 2 */
    if ((capacity & (capacity - 1)) == 0) {
        uint64_t probe_offset = (orbit_k * (orbit_k + 1)) >> 1;
        target_idx = (h + probe_offset) & (capacity - 1);
    } else {
        target_idx = (h + orbit_k) % capacity;
    }
    
    *out_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, target_idx);
    return HN4_OK;
}

/* =========================================================================
 * 3. WRITE PATH (BALLISTIC COMMIT)
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_write_nano_ballistic(
    HN4_IN hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor, 
    HN4_IN const void* data,
    HN4_IN uint32_t len
)
{
    /* 1. Pre-flight Validation */
    if (HN4_UNLIKELY(!vol || !anchor || !data)) return HN4_ERR_INVALID_ARGUMENT;
    if (vol->read_only) return HN4_ERR_ACCESS_DENIED;
    
    /* MEDIA CHECK */
    if (!_is_nano_compatible(vol)) return HN4_ERR_PROFILE_MISMATCH;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t ss = caps->logical_block_size;
    if (ss < sizeof(hn4_nano_quantum_t)) return HN4_ERR_GEOMETRY;

    /* Payload Bounds Check */
    size_t payload_capacity = ss - offsetof(hn4_nano_quantum_t, payload);
    if (len > HN4_NANO_MAX_PAYLOAD || len > payload_capacity) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    void* io_buf = hn4_hal_mem_alloc(ss);
    if (!io_buf) return HN4_ERR_NOMEM;

    hn4_u128_t my_id = hn4_le128_to_cpu(anchor->seed_id);
    hn4_result_t res = HN4_ERR_GRAVITY_COLLAPSE;

    /* Generation Logic: 32-bit Anchor -> 64-bit Slot */
    /* Nano slots use 64-bit sequence to prevent wrap-around collision on small sectors */
    uint32_t cur_gen = hn4_le32_to_cpu(anchor->write_gen);
    uint32_t next_gen_32 = (cur_gen == 0xFFFFFFFF) ? 1 : cur_gen + 1;
    uint64_t next_gen_64 = (uint64_t)next_gen_32; 

    /* 2. Trajectory Scan (Find a home) */
    for (uint32_t k = 0; k < HN4_NANO_MAX_ORBITS; k++) {
        hn4_addr_t target_lba;
        if (_calc_nano_trajectory(vol, my_id, k, &target_lba) != HN4_OK) {
            res = HN4_ERR_GEOMETRY;
            break;
        }

        /* Read / Ownership Check (RMW Safety) */
        int retries = 0;
        hn4_result_t read_res;
        do {
            read_res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, target_lba, io_buf, 1);
        } while (read_res != HN4_OK && ++retries < HN4_NANO_RETRY_IO);

        if (read_res != HN4_OK) continue; 

          uint32_t anchors_per_sector = ss / sizeof(hn4_anchor_t);
        hn4_anchor_t* anchor_view = (hn4_anchor_t*)io_buf;
        
        /* Check if it's already a valid Nano Quantum owned by us */
        hn4_nano_quantum_t* slot = (hn4_nano_quantum_t*)io_buf;
        bool is_mine = (hn4_le32_to_cpu(slot->magic) == HN4_MAGIC_NANO && 
                        slot->owner_id.lo == anchor->seed_id.lo && 
                        slot->owner_id.hi == anchor->seed_id.hi);

        bool is_empty = true;
        if (!is_mine) {
            for (uint32_t i = 0; i < anchors_per_sector; i++) {
                /* Check for non-zero data in the anchor slot */
                /* Optimization: Check key fields (Seed ID and Data Class) */
                if (anchor_view[i].seed_id.lo != 0 || 
                    anchor_view[i].seed_id.hi != 0 || 
                    anchor_view[i].data_class != 0) 
                {
                    is_empty = false;
                    break;
                }
            }
        }

        if (is_empty || is_mine) {
            /* 3. Prepare Write */
            _nano_secure_zero(io_buf, ss); 
            slot = (hn4_nano_quantum_t*)io_buf; 
            
            slot->magic = hn4_cpu_to_le32(HN4_MAGIC_NANO);
            slot->owner_id = anchor->seed_id; 
            slot->payload_len = hn4_cpu_to_le32(len);
            slot->sequence = hn4_cpu_to_le64(next_gen_64);
            
            memcpy(slot->payload, data, len);
            
            /* CRC with Epoch Salt */
            uint32_t crc = _calc_nano_crc(
                vol, my_id, next_gen_64, vol->sb.info.current_epoch_id, slot->payload, len
            );
            slot->data_crc = hn4_cpu_to_le32(crc);

            /* 4. Commit to Media */
            retries = 0;
            hn4_result_t w_res;
            do {
                w_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, target_lba, io_buf, 1);
            } while (w_res != HN4_OK && ++retries < HN4_NANO_RETRY_IO);

            if (w_res == HN4_OK) {
                /* 
                 * Durability Fence (FUA)
                 * Ensure data is on NAND before we point the Anchor to it.
                 */
                hn4_hal_barrier(vol->target_device);

                /* 5. Read-Back Verify (Paranoia Mode) */
                void* verify_buf = hn4_hal_mem_alloc(ss);
                bool verified = false;
                
                if (verify_buf) {
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, target_lba, verify_buf, 1) == HN4_OK) {
                        /* Compare only valid bytes, ignore padding */
                        size_t valid_bytes = offsetof(hn4_nano_quantum_t, payload) + len;
                        if (memcmp(io_buf, verify_buf, valid_bytes) == 0) verified = true;
                    }
                    _safe_free(verify_buf, ss);
                }

                if (!verified) {
                    HN4_LOG_WARN("Nano Write Verification Failed @ LBA %llu. Retrying Orbit.", 
                                 (unsigned long long)hn4_addr_to_u64(target_lba));
                    continue; 
                }

                /* 6. Update Anchor State */
                /* 
                 * We reuse standard fields for Nano specifics:
                 * gravity_center -> stores 'k' (orbit index)
                 * mass           -> stores actual byte length
                 */
                anchor->gravity_center = hn4_cpu_to_le64((uint64_t)k); 
                anchor->mass           = hn4_cpu_to_le64((uint64_t)len);
                anchor->write_gen      = hn4_cpu_to_le32(next_gen_32);
                anchor->mod_clock      = hn4_cpu_to_le64(hn4_hal_get_time_ns());
                
                uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
                dclass |= HN4_FLAG_NANO; 
                anchor->data_class = hn4_cpu_to_le64(dclass);

                /* 7. Atomic Anchor Switch */
                res = hn4_write_anchor_atomic(vol, anchor);
                
                if (res != HN4_OK) {
                    /* Log Orphan for Scavenger */
                    #ifdef HN4_USE_128BIT
                    HN4_LOG_WARN("LEAK: Nano Slot Orphaned @ LBA %llu (Gen %u)", 
                                 (unsigned long long)target_lba.lo, next_gen_32);
                    #else
                    HN4_LOG_WARN("LEAK: Nano Slot Orphaned @ LBA %llu (Gen %u)", 
                                 (unsigned long long)target_lba, next_gen_32);
                    #endif
                    res = HN4_ERR_HW_IO;
                }
                break; /* Success */
            }
        }
    }
    _safe_free(io_buf, ss);
    return res;
}

/* =========================================================================
 * 4. READ PATH (BALLISTIC RETRIEVAL)
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_read_nano_ballistic(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* anchor,
    HN4_OUT void* buf,
    HN4_IN uint32_t len
)
{
    /* 1. Validation */
    if (HN4_UNLIKELY(!vol || !anchor || !buf)) return HN4_ERR_INVALID_ARGUMENT;
    
    /* Media check */
    if (!_is_nano_compatible(vol)) return HN4_ERR_PROFILE_MISMATCH;

    /* Extract Location Info from Anchor */
    uint32_t orbit_k = (uint32_t)hn4_le64_to_cpu(anchor->gravity_center);
    if (orbit_k >= HN4_NANO_MAX_ORBITS) return HN4_ERR_DATA_ROT;

    hn4_u128_t my_id = hn4_le128_to_cpu(anchor->seed_id);
    
    hn4_addr_t target_lba;
    if (_calc_nano_trajectory(vol, my_id, orbit_k, &target_lba) != HN4_OK) {
        return HN4_ERR_GEOMETRY;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    void* io_buf = hn4_hal_mem_alloc(ss);
    if (!io_buf) return HN4_ERR_NOMEM;

    /* 2. Read Execution */
    int retries = 0;
    hn4_result_t res;
    do {
        res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, target_lba, io_buf, 1);
    } while (res != HN4_OK && ++retries < HN4_NANO_RETRY_IO);
    
    if (res == HN4_OK) {
        /* 3. Validation Pipeline */
        hn4_nano_quantum_t* slot = (hn4_nano_quantum_t*)io_buf;
        hn4_nano_val_status_t status = NANO_VAL_OK;
        
        uint32_t stored_len = hn4_le32_to_cpu(slot->payload_len);
        uint64_t slot_seq = hn4_le64_to_cpu(slot->sequence);
        uint32_t anchor_gen = hn4_le32_to_cpu(anchor->write_gen);
        hn4_u128_t slot_id = hn4_le128_to_cpu(slot->owner_id);

        /* Validation Rules */
        if (hn4_le32_to_cpu(slot->magic) != HN4_MAGIC_NANO) status = NANO_VAL_MAGIC_FAIL;
        else if (slot_id.lo != my_id.lo || slot_id.hi != my_id.hi) status = NANO_VAL_ID_MISMATCH;
        
        /* Generation Check (High 32-bits must be 0 for consistency) */
        else if ((slot_seq >> 32) != 0 || (uint32_t)slot_seq != anchor_gen) status = NANO_VAL_GEN_SKEW;
        
        /* Size Checks */
        else if (stored_len != (uint32_t)hn4_le64_to_cpu(anchor->mass)) status = NANO_VAL_SIZE_INVALID;
        else if (stored_len == 0 || stored_len > (ss - offsetof(hn4_nano_quantum_t, payload))) {
            status = NANO_VAL_SIZE_INVALID;
        }
        else {
            /* CRC Check (Seeded) */
            uint32_t stored_crc = hn4_le32_to_cpu(slot->data_crc);
            uint32_t calc_crc = _calc_nano_crc(vol, my_id, slot_seq, 
                                             vol->sb.info.current_epoch_id, 
                                             slot->payload, stored_len);
            
            if (stored_crc != calc_crc) {
                /* Epoch Mismatch Detection (Try without epoch salt to diagnose) */
                uint32_t unsalted_crc = _calc_nano_crc(vol, my_id, slot_seq, 0, slot->payload, stored_len);
                status = (stored_crc == unsalted_crc) ? NANO_VAL_EPOCH_MISMATCH : NANO_VAL_CRC_FAIL;
            }
        }

        /* 4. Result Mapping & Data Extraction */
        switch (status) {
            case NANO_VAL_OK:
                {
                    uint32_t copy_len = (stored_len > len) ? len : stored_len;
                    memcpy(buf, slot->payload, copy_len);
                    
                    /* Zero pad output if request was larger than stored data */
                    if (len > copy_len) {
                        memset((uint8_t*)buf + copy_len, 0, len - copy_len);
                    }
                    res = HN4_OK;
                }
                break;

            case NANO_VAL_MAGIC_FAIL:     res = HN4_ERR_PHANTOM_BLOCK; break;
            case NANO_VAL_ID_MISMATCH:    res = HN4_ERR_ID_MISMATCH; break;
            case NANO_VAL_GEN_SKEW:       res = HN4_ERR_GENERATION_SKEW; break;
            case NANO_VAL_EPOCH_MISMATCH: res = HN4_ERR_TIME_PARADOX; break;
            case NANO_VAL_SIZE_INVALID:
            case NANO_VAL_CRC_FAIL:
            default:
                res = HN4_ERR_DATA_ROT;
                break;
        }
    }

    _safe_free(io_buf, ss);
    return res;
}