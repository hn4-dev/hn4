/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Ballistic Read Pipeline (O(1) Access)
 * SOURCE:      hn4_read.c
 * STATUS:      HARDENED / PRODUCTION (v26.3)
 * ARCHITECT:   Core Systems Engineering
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ENGINEERING CONTRACT:
 * 1. O(1) EXECUTION: All loops are bounded by HN4_SHOTGUN_DEPTH (12).
 * 2. ATOMICITY: Reads are verified against Anchor Generation to prevent
 *    Phantom Reads (reading data from a future/past transaction).
 * 3. SELF-HEALING: "Auto-Medic" repairs corrupted replicas if a valid
 *    quorum survivor is found.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_swizzle.h"
#include "hn4_ecc.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include <string.h>


/* =========================================================================
 * PREFETCH OPTIMIZATION TABLES
 * ========================================================================= */

static const uint16_t _hdd_prefetch_lut[32] = {
    /* 0-11: Reserved/Tiny (0) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    
    /* Small Blocks: Aggressive Count */
    [12] = 32, /* 4KB   -> 128KB Total */
    [13] = 16, /* 8KB   -> 128KB Total */
    [14] = 8,  /* 16KB  -> 128KB Total */
    [15] = 4,  /* 32KB  -> 128KB Total */
    [16] = 2,  /* 64KB  -> 128KB Total */
    
    /* Large Blocks: Single Block Prefetch */
    [17] = 1,  /* 128KB */
    [18] = 1,  /* 256KB */
    [19] = 1,  /* 512KB */
    [20] = 1,  /* 1MB   */
    [21] = 1,  /* 2MB   */
    [22] = 1,  /* 4MB   */
    [23] = 1,  /* 8MB   */
    [24] = 1,  /* 16MB  */
    [25] = 1,  /* 32MB  */
    [26] = 1,  /* 64MB  */
    
    /* 27-31: Huge/Reserved */
    [27] = 1, [28] = 1, [29] = 1, [30] = 1, [31] = 1
};

/* =========================================================================
 * CONSTANTS & MACROS
 * ========================================================================= */

#define HN4_SHOTGUN_DEPTH           12

/* =========================================================================
 * ERROR PRIORITY LOGIC (LOOKUP TABLE OPTIMIZATION)
 * ========================================================================= */

 typedef struct {
    hn4_result_t code;
    int          weight;
} hn4_error_weight_t;

static const hn4_error_weight_t _error_weights[] = {
    /* CRITICAL INFRASTRUCTURE (90-100) */
    { HN4_ERR_CPU_INSANITY,      100 },
    { HN4_ERR_HW_IO,             99  },
    { HN4_ERR_NOMEM,             95  },

    /* LOGICAL CONSISTENCY (85-90) */
    { HN4_ERR_GENERATION_SKEW,   85  },
    { HN4_ERR_PHANTOM_BLOCK,     82  },

    /* DATA INTEGRITY (75-80) */
    { HN4_ERR_DATA_ROT,          80  },
    { HN4_ERR_HEADER_ROT,        80  },
    { HN4_ERR_PAYLOAD_ROT,       80  },
    { HN4_ERR_DECOMPRESS_FAIL,   79  },
    { HN4_ERR_ALGO_UNKNOWN,      78  },

    /* LOGICAL MISMATCH (55-70) */
    { HN4_ERR_ID_MISMATCH,       60  },
    { HN4_ERR_VERSION_INCOMPAT,  55  },

    /* EXPECTED / INFO (0-50) */
    { HN4_ERR_NOT_FOUND,         50  },
    { HN4_INFO_SPARSE,           10  },
    { HN4_OK,                    0   }
};

#define HN4_WEIGHT_TABLE_SIZE (sizeof(_error_weights) / sizeof(hn4_error_weight_t))

/*
 * Error Weighting: Higher values take precedence when merging results.
 * Inverted weights: Generation Skew > Data Rot.
 */
static int _get_error_weight(hn4_result_t e)
{
    /* Hot path optimization: OK is 0 */
    if (HN4_LIKELY(e == HN4_OK)) return 0;

    for (size_t i = 0; i < HN4_WEIGHT_TABLE_SIZE; i++) {
        if (_error_weights[i].code == e) {
            return _error_weights[i].weight;
        }
    }
    
    /* Default weight for unknown errors */
    return 40;
}

/* 
 * MECHANICAL SORT (C-LOOK Simulation)
 * Sorts LBAs ascending to ensure the head sweeps in one direction 
 * rather than vibrating back and forth between tracks.
 */
static void _sort_candidates_mechanical(uint64_t* candidates, int count) {
    /* Bubble sort is sufficient for small N (max 12) and keeps stack light */
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (candidates[j] > candidates[j+1]) {
                uint64_t temp = candidates[j];
                candidates[j] = candidates[j+1];
                candidates[j+1] = temp;
            }
        }
    }
}

HN4_INLINE hn4_result_t _merge_error(hn4_result_t current, hn4_result_t new_err)
{
    if (HN4_LIKELY(current == HN4_OK)) return new_err;
    if (HN4_LIKELY(new_err == HN4_OK)) return current;

    int w_cur = _get_error_weight(current);
    int w_new = _get_error_weight(new_err);

    if (w_new > w_cur) return new_err;
    if (w_new < w_cur) return current;

    /* Equal severity → preserve causal first error */
    return current;
}


/* =========================================================================
 * VALIDATION HELPER
 * ========================================================================= */

static hn4_result_t _validate_block(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const void*   buffer,
    HN4_IN uint32_t      len,
    HN4_IN hn4_u128_t    expected_well_id,
    HN4_IN uint64_t      expected_gen,
    HN4_IN uint64_t      anchor_dclass
)
{
    const hn4_block_header_t* hdr = (const hn4_block_header_t*)buffer;

    /* Hardware Defense: Ensure IO buffer is complete */
    if (HN4_UNLIKELY(len < vol->vol_block_size)) {
        HN4_LOG_ERR("Block Validation: Short Read. Got %u, Need %u",
                    len, vol->vol_block_size);
        return HN4_ERR_HW_IO;
    }

    /* 1. Magic Check & Poison Detection */
    uint32_t magic = hn4_le32_to_cpu(hdr->magic);

    if (HN4_UNLIKELY(magic != HN4_BLOCK_MAGIC)) {   
        /* Check for debug poisoning (0xCC) before declaring phantom */
        if (magic == 0xCCCCCCCC) {
            /* Scan first cache line to confirm strict poisoning */
            const uint64_t* scan = (const uint64_t*)buffer;
            bool is_poison = true;
            
            /* Check 64 bytes (8 x 64-bit words) */
            for (int i = 0; i < 8; i++) {
                if (scan[i] != 0xCCCCCCCCCCCCCCCCULL) {
                    is_poison = false;
                    break;
                }
            }

            if (is_poison) {
                HN4_LOG_CRIT("DMA Failure: Buffer contains strict poison pattern.");
                return HN4_ERR_HW_IO;
            }
        }
        return HN4_ERR_PHANTOM_BLOCK;
    }

    /* 2. Header Integrity Check (CRC) */
    uint32_t stored_crc = hn4_le32_to_cpu(hdr->header_crc);
    uint32_t calc_crc   = hn4_crc32(HN4_CRC_SEED_HEADER, hdr, offsetof(hn4_block_header_t, header_crc));

    if (HN4_UNLIKELY(stored_crc != calc_crc)) return HN4_ERR_HEADER_ROT;

    /* 3. Identity Check (Anti-Collision) */
    hn4_u128_t disk_id = hn4_le128_to_cpu(hdr->well_id);
    if (HN4_UNLIKELY(disk_id.lo != expected_well_id.lo || disk_id.hi != expected_well_id.hi)) {
        return HN4_ERR_ID_MISMATCH;
    }

     /* 
     * 4. Freshness Check (STRICT ATOMICITY)
     * Rejects Phantom Reads where Disk Gen != Anchor Gen.
     * High 32-bits must be zero (v1 format constraint).
     */
    uint64_t blk_gen_64 = hn4_le64_to_cpu(hdr->generation);
    
    if ((blk_gen_64 >> 32) != 0) return HN4_ERR_GENERATION_SKEW;

    if (HN4_UNLIKELY((uint32_t)blk_gen_64 != (uint32_t)expected_gen)) {
        return HN4_ERR_GENERATION_SKEW;
    }

    /* 5. Data Integrity & Policy Check */
    uint32_t payload_sz = HN4_BLOCK_PayloadSize(vol->vol_block_size);
    uint32_t comp_meta  = hn4_le32_to_cpu(hdr->comp_meta);
    uint32_t c_size     = comp_meta >> HN4_COMP_SIZE_SHIFT;
    uint8_t  algo       = comp_meta & HN4_COMP_ALGO_MASK;

    if (HN4_UNLIKELY(algo != HN4_COMP_NONE && algo != HN4_COMP_TCC)) {
        HN4_LOG_WARN("Block Validation: Unknown Algo %u", algo);
        return HN4_ERR_ALGO_UNKNOWN;
    }

    if ((anchor_dclass & HN4_HINT_ENCRYPTED) && algo != HN4_COMP_NONE) {
        HN4_LOG_CRIT("Security: Encrypted file contains compressed block. Tamper evidence.");
        return HN4_ERR_TAMPERED;
    }

    if (c_size > payload_sz) {
        HN4_LOG_WARN("Block Validation: Meta Corruption (CSize %u > Payload %u)", c_size, payload_sz);
        return HN4_ERR_HEADER_ROT;
    }

    uint32_t stored_dcrc = hn4_le32_to_cpu(hdr->data_crc);
    uint32_t calc_dcrc   = hn4_crc32(HN4_CRC_SEED_DATA, hdr->payload, payload_sz);

    if (HN4_UNLIKELY(stored_dcrc != calc_dcrc)) {
        HN4_LOG_WARN("Block Validation: Payload CRC Mismatch");
        return HN4_ERR_PAYLOAD_ROT;
    }

    return HN4_OK;
}

/* =========================================================================
 * CORE LOGIC
 * ========================================================================= */

_Check_return_ HN4_NO_INLINE hn4_result_t hn4_read_block_atomic(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  hn4_anchor_t* anchor_ptr,
    HN4_IN  uint64_t      block_idx,
    HN4_OUT void*         out_buffer,
    HN4_IN  uint32_t      buffer_len,
    HN4_IN uint32_t session_perms /* Delegated rights */
)
{
    if (HN4_UNLIKELY(!vol || !anchor_ptr || !out_buffer)) return HN4_ERR_INVALID_ARGUMENT;

    hn4_anchor_t anchor;
    
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    memcpy(&anchor, anchor_ptr, sizeof(hn4_anchor_t));
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    uint32_t payload_cap = HN4_BLOCK_PayloadSize(vol->vol_block_size);

    if (buffer_len < payload_cap) {
        HN4_LOG_ERR("Read Error: Buffer %u < Payload %u", buffer_len, payload_cap);
        return HN4_ERR_INVALID_ARGUMENT;
    }

    /* 1. Permissions Gate */
    uint32_t perms  = hn4_le32_to_cpu(anchor.permissions);
    uint64_t dclass = hn4_le64_to_cpu(anchor.data_class);

    uint32_t effective_perms = perms | session_perms;

    if (HN4_UNLIKELY(!(effective_perms & (HN4_PERM_READ | HN4_PERM_SOVEREIGN))))  return HN4_ERR_ACCESS_DENIED;

    /* 2. Physics & Geometry Extraction */
     uint64_t G = hn4_le64_to_cpu(anchor.gravity_center);

    const uint8_t* raw_v = anchor.orbit_vector;

    uint64_t V = (uint64_t)raw_v[0] |
                 ((uint64_t)raw_v[1] << 8)  |
                 ((uint64_t)raw_v[2] << 16) |
                 ((uint64_t)raw_v[3] << 24) |
                 ((uint64_t)raw_v[4] << 32) |
                 ((uint64_t)raw_v[5] << 40);

    uint16_t   M          = hn4_le16_to_cpu(anchor.fractal_scale);
    hn4_u128_t well_id    = hn4_le128_to_cpu(anchor.seed_id);
    uint64_t   anchor_gen = (uint64_t)hn4_le32_to_cpu(anchor.write_gen);

    uint32_t              bs   = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);

    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    uint32_t ss = caps->logical_block_size;
    if (HN4_UNLIKELY(ss == 0 || (bs % ss) != 0)) return HN4_ERR_ALIGNMENT_FAIL;

    uint32_t sectors = bs / ss;

    /* 3. Hardware Profile Tuning */
    uint8_t  depth_limit   = HN4_SHOTGUN_DEPTH;
    bool     allow_healing = !vol->read_only;
    uint32_t retry_sleep   = 1000;
    uint32_t profile       = vol->sb.info.format_profile;
    uint32_t dev_type      = vol->sb.info.device_type_tag;

   bool is_hdd = (dev_type == HN4_DEV_HDD) || 
                  (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) ||
                  (profile == HN4_PROFILE_ARCHIVE);

    switch (profile) {
        case HN4_PROFILE_PICO:
            depth_limit   = 1;
            allow_healing = false;
            break;
        case HN4_PROFILE_USB:
            depth_limit = 3;
            retry_sleep = 5000;
            break;
        case HN4_PROFILE_GAMING:
            if (hn4_le64_to_cpu(anchor.mass) < 65536) {
                depth_limit = 1;
            }
            retry_sleep = 10;
            break;
        default:
            /* Device-specific overrides */
            if (dev_type == HN4_DEV_HDD || (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL)) {
                depth_limit = 2;
            } else if (dev_type == HN4_DEV_TAPE) {
                depth_limit = 0;
            }
            break;
    }

    /* 4. Candidate Generation */
    uint64_t     candidates[HN4_SHOTGUN_DEPTH] = {0};
    hn4_result_t candidate_errors[HN4_SHOTGUN_DEPTH];
    hn4_result_t probe_error = HN4_OK;
    int          valid_candidates = 0;
    uint64_t     max_blocks = vol->vol_capacity_bytes / bs;

    for (int i = 0; i < HN4_SHOTGUN_DEPTH; i++) candidate_errors[i] = HN4_ERR_NOT_FOUND;

    if (dclass & HN4_HINT_HORIZON) {
        uint16_t safe_M = (M > 63) ? 63 : M;
        uint64_t stride = (1ULL << safe_M);

        if (block_idx < (UINT64_MAX / stride)) {
            uint64_t linear_lba = G + (block_idx * stride);
            if (linear_lba < max_blocks) {
                bool allocated;
                hn4_result_t op_res = _bitmap_op(vol, linear_lba, BIT_TEST, &allocated);

                if (op_res != HN4_OK) {
                    probe_error = _merge_error(probe_error, op_res);
                } else if (allocated) {
                    candidates[0]    = linear_lba;
                    valid_candidates = 1;
                }
            }
        }
    } else {
        if (HN4_UNLIKELY(dev_type == HN4_DEV_TAPE)) return HN4_ERR_GEOMETRY;

        /* Determine target Orbit (k) from Anchor Hint (2 bits per cluster) */
        uint8_t  target_k   = 0;
        uint64_t cluster_idx = block_idx >> 4;

        if (cluster_idx < 16) {
            uint32_t hints = hn4_le32_to_cpu(anchor.orbit_hints);
            uint32_t shift = (uint32_t)(cluster_idx * 2);
            target_k = (uint8_t)((hints >> shift) & 0x3u);
        }

        /* Only scan the specific target orbit */
        uint8_t k = target_k;
        {
            /*
             * Trajectory Jitter.
             * For higher orbits (k >= 8), apply a secondary swizzle to 'G' (Gravity Center)
             * to force candidates into uncorrelated physical regions (Anti-Wordline Bias).
             */
            uint64_t effective_G = (k >= 8) ? (G ^ hn4_swizzle_gravity_assist(G)) : G;
            uint64_t effective_V = (k >= 4) ? hn4_swizzle_gravity_assist(V) : V;

            uint64_t lba = _calc_trajectory_lba(vol, effective_G, effective_V, block_idx, M, k);

            if (lba != HN4_LBA_INVALID && lba < max_blocks) {
                /* Atomic Reservation / Existence Check */
                bool is_allocated = false;
                /* 
                 * If we are Read-Only and the Bitmap failed to load (NULL), we cannot 
                 * check allocation status. We MUST assume the block exists and let 
                 * the Physical Validation (Magic/CRC) determine truth.
                 */
                if (vol->read_only && !vol->void_bitmap) {
                    is_allocated = true; /* Optimistic probe */
                } else {
                    hn4_result_t op_res = _bitmap_op(vol, lba, BIT_TEST, &is_allocated);

                    if (op_res == HN4_ERR_UNINITIALIZED && vol->read_only) {
                        is_allocated = true; 
                        op_res = HN4_OK;
                    }

                    if (op_res != HN4_OK) {
                        probe_error = _merge_error(probe_error, op_res);
                        is_allocated = false;
                    }
                }

                if (is_allocated) {
                    if (valid_candidates < HN4_SHOTGUN_DEPTH) {
                        candidates[valid_candidates++] = lba;
                    }
                }
            }
        }
    }

    /* Trajectory Collapse Detection */
    if (depth_limit >= 2 && valid_candidates == 1) {
        atomic_fetch_add(&vol->health.trajectory_collapse_counter, 1);
        HN4_LOG_WARN("Trajectory Collapse: Only 1 candidate found (Limit %d)", depth_limit);
    }

    /* Sparse Logic */
    if (HN4_UNLIKELY(valid_candidates == 0)) {
        if (probe_error != HN4_OK) {
            /* If probe had hard errors (e.g. HW_IO), report that. Don't assume sparse. */
            return probe_error;
        }
        memset(out_buffer, 0, buffer_len);
        return HN4_INFO_SPARSE;
    }
    
     if (is_hdd && valid_candidates > 1) {
        _sort_candidates_mechanical(candidates, valid_candidates);
    }


    /* 5. The "Shotgun" Read Loop */
    void* io_buf = hn4_hal_mem_alloc(bs);
    if (!io_buf) return HN4_ERR_NOMEM;

    hn4_result_t deep_error = HN4_ERR_NOT_FOUND;
    int          winner_idx = -1;
    uint32_t     failed_mask = 0;

    for (int i = 0; i < valid_candidates; i++) {
        uint64_t target_lba = candidates[i];

        if (target_lba > (UINT64_MAX / sectors)) {
            failed_mask |= (1U << i);
            candidate_errors[i] = HN4_ERR_GEOMETRY;
            deep_error          = _merge_error(deep_error, HN4_ERR_GEOMETRY);
            continue;
        }

    #ifdef HN4_USE_128BIT
        hn4_u128_t blk_128 = hn4_u128_from_u64(target_lba);
        hn4_addr_t phys_sector = hn4_u128_mul_u64(blk_128, sectors);
    #else
        hn4_addr_t phys_sector = hn4_lba_from_blocks(target_lba * sectors);
    #endif

        int max_retries = (vol->sb.info.hw_caps_flags & HN4_HW_NVM) ? 1 : 2;
        int tries = 0;
        hn4_result_t io_res;

        /* THERMAL DECAY CALCULATION */
        uint32_t current_retry_delay = retry_sleep;
        
        if (is_hdd) {

            uint32_t health_score = atomic_load(&vol->health.taint_counter);
            
            #define HN4_HEALTH_THRESHOLD 50
            if (health_score > HN4_HEALTH_THRESHOLD) {
                int shift = (health_score - HN4_HEALTH_THRESHOLD) / 10;
                if (shift > 6) shift = 6; /* Cap at 64x delay */
                current_retry_delay <<= shift;
                
                /* Limit max sleep to 100ms to prevent timeout */
                if (current_retry_delay > 100000) current_retry_delay = 100000;
            }
        }

        do {

            memset(io_buf, 0xCC, 64);

            io_res = _hn4_spatial_router(
                vol, 
                HN4_IO_READ, 
                phys_sector, 
                io_buf, 
                sectors, 
                well_id
            );

            if (HN4_LIKELY(io_res == HN4_OK)) {
                hn4_result_t val_res = _validate_block(vol, io_buf, bs, well_id, anchor_gen, dclass);

                if (HN4_LIKELY(val_res == HN4_OK)) {
                    if (io_res == HN4_INFO_HEALED) {
                        /* Mark candidate for repair if HAL reported soft error */
                        failed_mask |= (1U << i);
                        candidate_errors[i] = HN4_ERR_DATA_ROT;
                    }
                    io_res = HN4_OK;
                } else if (val_res != HN4_ERR_DATA_ROT && val_res != HN4_ERR_PAYLOAD_ROT) {
                    io_res = val_res;
                } else {
                    io_res = val_res;
                }
            }

            if (io_res != HN4_OK && is_hdd) {
                if (io_res == HN4_ERR_HW_IO || io_res == HN4_ERR_ATOMICS_TIMEOUT) {
                    atomic_fetch_add(&vol->health.taint_counter, 1);
                }
            }

            if (++tries < max_retries && io_res != HN4_OK) {
                hn4_hal_micro_sleep(current_retry_delay);
            }
        } while (HN4_UNLIKELY(io_res != HN4_OK && tries < max_retries));

        candidate_errors[i] = io_res;

        if (io_res == HN4_OK) {
            hn4_block_header_t* hdr    = (hn4_block_header_t*)io_buf;
            uint32_t comp_meta         = hn4_le32_to_cpu(hdr->comp_meta);
            uint8_t  algo              = comp_meta & HN4_COMP_ALGO_MASK;
            uint32_t c_size            = comp_meta >> HN4_COMP_SIZE_SHIFT;
            uint32_t max_payload       = payload_cap;
            hn4_result_t decomp_res    = HN4_OK;

            switch (algo) {
                case HN4_COMP_NONE:
                {
                    uint32_t copy_len = (buffer_len < max_payload) ? buffer_len : max_payload;
                    if (buffer_len < max_payload) {
                        HN4_LOG_WARN("READ_ATOMIC: Output truncated.");
                    }
                    memcpy(out_buffer, hdr->payload, copy_len);
                    if (buffer_len > copy_len) {
                        memset((uint8_t*)out_buffer + copy_len, 0, buffer_len - copy_len);
                    }
                    break;
                }

                case HN4_COMP_TCC:
                {
                    uint32_t actual_out_size = 0;
                    decomp_res = hn4_decompress_block(hdr->payload, c_size, out_buffer, buffer_len, &actual_out_size);

                    /* Map internal buffer exhaustion to semantic API error */
                    if (decomp_res == HN4_ERR_NOMEM) {
                        decomp_res = HN4_ERR_DECOMPRESS_FAIL;
                    }

                    if (decomp_res == HN4_OK) {
                        if (buffer_len > actual_out_size) {
                            memset((uint8_t*)out_buffer + actual_out_size, 0, buffer_len - actual_out_size);
                        }
                    }
                    break;
                }

                default:
                    decomp_res = HN4_ERR_ALGO_UNKNOWN;
                    break;
            }

            if (HN4_LIKELY(HN4_IS_OK(decomp_res))) {
                winner_idx = i;
                deep_error = decomp_res;

                /* 
                 * PREFETCH OPTIMIZATION:
                 * - GAMING: Prefetch for asset streaming.
                 * - HYPER_CLOUD: Prefetch for database table scans / blob streaming.
                 */
               bool is_hdd = (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) || 
                          (dev_type == HN4_DEV_HDD);
            
            bool is_streaming = (profile == HN4_PROFILE_GAMING || 
                                 profile == HN4_PROFILE_HYPER_CLOUD);

            if (is_hdd || is_streaming) {
                uint32_t pf_len_sectors = 0;

                if (is_hdd) {

                    int shift = 0;
                    uint32_t v = vol->vol_block_size;
                    while (v >>= 1) shift++;

                    if (shift >= 32) shift = 31;
                    
                    pf_len_sectors = _hdd_prefetch_lut[shift] * sectors;
                } else {
                    pf_len_sectors = sectors;
                }

                if (pf_len_sectors > 0) {
                    uint8_t next_k = 0;
                    uint64_t next_cluster = (block_idx + 1) >> 4;
                    
                    if (next_cluster < 16) {
                        uint32_t h = hn4_le32_to_cpu(anchor.orbit_hints);
                        next_k = (h >> (next_cluster * 2)) & 0x3;
                    }

                    uint64_t next_lba = _calc_trajectory_lba(vol, G, V, block_idx + 1, M, next_k);
                    
                    if (next_lba != HN4_LBA_INVALID && next_lba < max_blocks) {
                         #ifdef HN4_USE_128BIT
                             hn4_u128_t blk_128 = hn4_u128_from_u64(next_lba);
                             hn4_addr_t pf_phys = hn4_u128_mul_u64(blk_128, sectors);
                         #else
                             hn4_addr_t pf_phys = hn4_lba_from_blocks(next_lba * sectors);
                         #endif
                         
                         hn4_hal_prefetch(vol->target_device, pf_phys, pf_len_sectors);
                    }
                }
            }

                break;
            } else {
                failed_mask |= (1ULL << i);
                candidate_errors[i] = decomp_res;
                deep_error = _merge_error(deep_error, decomp_res);
            }

        } else {
            failed_mask |= (1U << i);
            deep_error = _merge_error(deep_error, io_res);

            if (io_res == HN4_ERR_HEADER_ROT || io_res == HN4_ERR_PAYLOAD_ROT || io_res == HN4_ERR_DATA_ROT) {
                atomic_fetch_add(&vol->health.crc_failures, 1);
            }
        }
    }

    /* 6. Auto-Medic */
      if (HN4_UNLIKELY(HN4_IS_OK(deep_error) && failed_mask != 0 && allow_healing && winner_idx >= 0)) {
        hn4_block_header_t* w_hdr = (hn4_block_header_t*)io_buf;
        if (vol->read_only) {
            HN4_LOG_WARN("READ_ATOMIC: Skipping Auto-Medic (RO).");
        } else {
            for (int i = 0; i < valid_candidates; i++) {
                if (i == winner_idx) continue;

                if (failed_mask & (1U << i)) {
                    hn4_result_t err = candidate_errors[i];
                    if (err == HN4_ERR_GENERATION_SKEW || err == HN4_ERR_ID_MISMATCH) continue;

                    uint64_t   bad_lba_idx = candidates[i];
                    hn4_addr_t bad_phys    = hn4_lba_from_blocks(bad_lba_idx * sectors);

                    /* 
                     * Re-verify CRC before writing back to disk.
                     * We calculate based on the RAW payload in the buffer (compressed or not).
                     */
                    size_t h_bound = offsetof(hn4_block_header_t, header_crc);
                    uint32_t saved_crc = w_hdr->header_crc;

                    /* Use payload_cap which matches the on-disk size */
                    uint32_t d_len = HN4_BLOCK_PayloadSize(vol->vol_block_size);
                    w_hdr->data_crc = hn4_cpu_to_le32(hn4_crc32(HN4_CRC_SEED_DATA, w_hdr->payload, d_len));
                    w_hdr->header_crc = 0;
                    w_hdr->header_crc = hn4_cpu_to_le32(hn4_crc32(HN4_CRC_SEED_HEADER, w_hdr, h_bound));

                    if (hn4_repair_block(vol, bad_phys, io_buf, bs) != HN4_OK) {
                        HN4_LOG_WARN("READ_ATOMIC: Auto-Medic failed for candidate %d", i);
                    }

                    w_hdr->header_crc = saved_crc;
                }
            }
        }
    }

    hn4_hal_mem_free(io_buf);

    if (winner_idx == -1) {
        /* Do not wipe output buffer on error */
        return deep_error;
    }

    if (deep_error == HN4_OK && probe_error == HN4_INFO_HEALED) {
        return HN4_INFO_HEALED;
    }

    return deep_error;
}