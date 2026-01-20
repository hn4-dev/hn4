/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Spatial Array Router (Hyper-Cloud Profile)
 * SOURCE:      hn4_helix.c
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

#define HN4_STACK_BUF_SIZE 128 
#define HN4_SMALL_ARRAY_LIMIT 8
#define HN4_HELIX_STRIPE_SECTORS 128 

/* =========================================================================
 * GALOIS FIELD MATH ENGINE (PORTABLE)
 * Polynomial: x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
 * ========================================================================= */

static uint8_t _gf_log[256];
static uint8_t _gf_exp[512]; /* Double size to avoid modulo in lookup */
static atomic_bool _gf_ready = false;

/* One-time initialization of math tables */
static void _hn4_gf_init(void) {
    static hn4_spinlock_t _gf_lock = { .flag = ATOMIC_FLAG_INIT };
    
    if (atomic_load_explicit(&_gf_ready, memory_order_acquire)) return;

    hn4_hal_spinlock_acquire(&_gf_lock);

    if (!atomic_load_explicit(&_gf_ready, memory_order_relaxed)) {
        uint32_t v = 1;
        for (int i = 0; i < 255; i++) {
            _gf_exp[i] = (uint8_t)v;
            _gf_exp[i + 255] = (uint8_t)v; 
            _gf_log[v] = (uint8_t)i;
            
            v <<= 1;
            if (v & 0x100) v ^= 0x11D; 
       }
        _gf_log[0] = 0; 
        
        atomic_store_explicit(&_gf_ready, true, memory_order_release);
    }

    hn4_hal_spinlock_release(&_gf_lock);
}


/* 
 * O(1) Galois Multiplication 
 * Performance: ~2-3 cycles (L1 Cache Hit)
 */
static inline uint8_t _gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    if (HN4_UNLIKELY(!atomic_load_explicit(&_gf_ready, memory_order_acquire))) _hn4_gf_init();
    return _gf_exp[(int)_gf_log[a] + (int)_gf_log[b]];
}

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/* 
 * Fast XOR Buffer (Aliasing Safe)
 * Replaces direct pointer casting with memcpy to ensure strict aliasing compliance.
 * Modern compilers optimize this to unaligned vector loads/stores.
 */
 void _xor_buffer_fast(void* dst, const void* src, size_t len) {
    if (dst == src) return;

    uintptr_t d = (uintptr_t)dst;
    uintptr_t s = (uintptr_t)src;
    
    /* 
     * Corrected Overlap Check (Standard Interval Logic).
     * Ensures linear behavior even in overlapping cases (memmove semantics).
     */
    if (!(d + len <= s || s + len <= d)) {
        uint8_t* d8 = (uint8_t*)dst;
        const uint8_t* s8 = (const uint8_t*)src;
        
        /* Slow path: Overlap requires bytewise copy to preserve semantics */
        if (d > s) {
            while (len > 0) { len--; d8[len] ^= s8[len]; }
        } else {
            for (size_t k = 0; k < len; k++) d8[k] ^= s8[k];
        }
        return;
    }

    uint8_t* d8 = (uint8_t*)dst;
    const uint8_t* s8 = (const uint8_t*)src;
    size_t i = 0;

    /* 
     * UNROLLED BLOCK LOOP (32 bytes per iteration)
     * Proof of Linearity: 4x u64 operations per iteration.
     * No alignment checks inside loop (memcpy handles misalignment via hardware/compiler).
     */
    while (i + 32 <= len) {
        uint64_t v_dst[4];
        uint64_t v_src[4];

        memcpy(v_dst, d8 + i, 32);
        memcpy(v_src, s8 + i, 32);

        v_dst[0] ^= v_src[0];
        v_dst[1] ^= v_src[1];
        v_dst[2] ^= v_src[2];
        v_dst[3] ^= v_src[3];

        memcpy(d8 + i, v_dst, 32);
        i += 32;
    }

    /* Tail handling (Linear residue) */
    while (i < len) {
        d8[i] ^= s8[i];
        i++;
    }
}

static void _mark_device_offline(hn4_volume_t* vol, uint32_t dev_idx, hn4_hal_device_t* expected_handle) {
    hn4_array_ctx_t* arr = &vol->array;
    if (dev_idx >= HN4_MAX_ARRAY_DEVICES) return;

    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);

    /* Verify Identity matches the handle we tried to use */
    if (arr->devices[dev_idx].dev_handle != expected_handle) {
        HN4_LOG_WARN("ARRAY: Race detected during offline. Device replaced at slot %u.", dev_idx);
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        return;
    }

    /* Verify Status is still ONLINE before flipping */
    if (arr->devices[dev_idx].status == HN4_DEV_STAT_ONLINE) {
        arr->devices[dev_idx].status = HN4_DEV_STAT_OFFLINE;
        
        HN4_LOG_CRIT("ARRAY: Device %u marked OFFLINE due to Critical IO Failure.", dev_idx);
        
        /* Mark volume degraded */
        vol->sb.info.state_flags |= (HN4_VOL_DEGRADED | HN4_VOL_DIRTY);
    }

    hn4_hal_spinlock_release(&vol->locking.l2_lock);
}

/*
 * _resolve_shard_index
 * 
 * FORMAL CLAIM: 
 *   Produces epsilon-uniform distribution over [0, dev_count).
 *   Bias epsilon <= 2^-32.
 *   Method: SplitMix64 Avalanche -> Lemire Fast Reduction.
 */
static uint32_t _resolve_shard_index(hn4_u128_t file_id, uint32_t dev_count) {
    if (dev_count == 0) return 0;
    
    /* 1. Mix Entropy (SplitMix64 variant) */
    uint64_t k = file_id.lo ^ file_id.hi;
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    
    /* 
     * 2. Lemire Fast Reduction (Multiply-High)
     * Replaces `k % dev_count` to eliminate modulo bias.
     * Maps the uniform u64 range [0, 2^64) onto [0, dev_count).
     */
#if defined(__SIZEOF_INT128__)
    return (uint32_t)(((__uint128_t)k * dev_count) >> 64);
#else
    /* Fallback for 32-bit: Double Mixer + Modulo.
       Bias is acceptable here as dev_count << 2^64. */
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)(k % dev_count);
#endif
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
 * HELIX-D MATH EXTENSIONS
 * ========================================================================= */

/* Inverse in GF(2^8): x^-1 = exp(255 - log(x)) */
static inline uint8_t _gf_inv(uint8_t x) {
    if (HN4_UNLIKELY(x == 0)) {
        hn4_hal_panic("HN4 Helix: GF Inversion Singularity (Div by Zero)");
        return 0; /* Unreachable */
    }
    if (x == 1) return 1; 
    if (HN4_UNLIKELY(!atomic_load_explicit(&_gf_ready, memory_order_acquire))) _hn4_gf_init();
    return _gf_exp[255 - _gf_log[x]];
}
/*
 * _hn4_helix_apply_delta
 * Updates P (XOR) and Q (Galois) in one pass for Read-Modify-Write.
 * 
 * Logic:
 *   P_new = P_old ^ Delta
 *   Q_new = Q_old ^ (Delta * g^coeff)
 * 
 * Uses 4-way unrolling to hide memory latency of GF table lookups.
 */
 void _hn4_helix_apply_delta(
    uint8_t* dst_p, 
    uint8_t* dst_q, 
    const uint8_t* delta, 
    size_t len, 
    uint8_t generator_val, /* Was 'coeff' */
    bool update_p,         /* NEW: Is P valid/online? */
    bool update_q          /* NEW: Is Q valid/online? */
) {
    /* 1. Update P-Parity (XOR) */
    if (update_p) {
        _xor_buffer_fast(dst_p, delta, len);
    }

    /* 2. Update Q-Parity (Galois Field Multiplication) */
    if (update_q) {
        size_t i = 0;
        
        /* Pre-load constants for register allocation */
        /* Ensure init if not already done (though caller should handle) */
        if (HN4_UNLIKELY(!atomic_load_explicit(&_gf_ready, memory_order_acquire))) _hn4_gf_init();

        uint8_t g_element = _gf_exp[generator_val % 255]; 
        uint8_t g_log     = _gf_log[g_element]; 

        const uint8_t* exp_tbl = _gf_exp;
        const uint8_t* log_tbl = _gf_log;

        /* 
         * Unrolled Loop (4 bytes per iter).
         * Since table lookups have latency, doing 4 independent lookups allows
         * the CPU to pipeline memory requests.
         */
        while (i + 4 <= len) {
            uint8_t d0 = delta[i];
            uint8_t d1 = delta[i+1];
            uint8_t d2 = delta[i+2];
            uint8_t d3 = delta[i+3];

            /* Q ^= D * g^coeff -> Q ^= exp[log(D) + log(g)] */
            if (d0) dst_q[i]   ^= exp_tbl[log_tbl[d0] + g_log];
            if (d1) dst_q[i+1] ^= exp_tbl[log_tbl[d1] + g_log];
            if (d2) dst_q[i+2] ^= exp_tbl[log_tbl[d2] + g_log];
            if (d3) dst_q[i+3] ^= exp_tbl[log_tbl[d3] + g_log];

            i += 4;
        }

        /* Tail handling */
        while (i < len) {
            uint8_t d = delta[i];
            if (d) dst_q[i] ^= exp_tbl[log_tbl[d] + g_log];
            i++;
        }
    }
}
/*
 * _hn4_phys_to_logical
 * Maps a physical device index back to the logical data column index.
 * Used to determine the correct GF generator coefficient for Q-Parity.
 */
static inline uint32_t _hn4_phys_to_logical(uint32_t phys, uint32_t p_col, uint32_t q_col) {

    if (phys == p_col || phys == q_col) return UINT32_MAX;

    uint32_t s1 = (p_col < q_col) ? p_col : q_col;
    uint32_t s2 = (p_col < q_col) ? q_col : p_col;
    
    uint32_t log = phys;
    if (phys > s1) log--;
    if (phys > s2) log--;
    return log;
}

/* 
 * _hn4_reconstruct_helix
 * Solves for missing data in the stripe.
 * 
 * Strategy:
 * 1. Census: Identify all failed columns.
 * 2. Scan: Read all surviving columns to build Partial P and Partial Q.
 * 3. Solve: 
 *    - If 1 failure: Use P (XOR).
 *    - If 2 failures (Data+P): Use Q.
 *    - If 2 failures (Data+Data): Use P+Q Algebra.
 */
static hn4_result_t _hn4_reconstruct_helix(
    hn4_volume_t* vol,
    hn4_drive_t* snapshot,
    uint32_t count,
    uint32_t stripe_ss,
    uint32_t p_col,
    uint32_t q_col,
    uint32_t target_col,
    hn4_addr_t io_lba,
    void* result_buf,
    uint32_t len
) {
    /* ---------------------------------------------------------
     * 1. FAILURE CENSUS 
     * --------------------------------------------------------- */
    int fail_cnt = 0;
    int fail_idxs[2] = {-1, -1};
    
    for (uint32_t i = 0; i < count; i++) {
        bool is_failed = (snapshot[i].status != HN4_DEV_STAT_ONLINE);
        
        /* 
         * Treat the target as "missing" (an erasure variable) 
         * even if the drive is physically online (e.g., during RMW read failure).
         * Ideally, target_col is one of the failures, but we must ensure it 
         * enters the equation as an unknown.
         */
        bool is_unknown = is_failed || (i == target_col);

        if (is_unknown) {
            /* We can solve for up to 2 unknowns (Erasures) */
            if (fail_cnt < 2) {
                fail_idxs[fail_cnt++] = i;
            } else {
                /* >2 Erasures = Information Loss (Reed-Solomon Limit) */
                return HN4_ERR_PARITY_BROKEN;
            }
        }
    }

    if (len > SIZE_MAX / stripe_ss) return HN4_ERR_NOMEM;
    size_t byte_len = (size_t)len * stripe_ss;
    
    /* 
     * Buffer Allocation Strategy:
     * Small IOs (Metadata) -> Stack (Speed/Safety).
     * Large IOs (Data) -> Heap.
     */
    uint8_t stack_p[HN4_STACK_BUF_SIZE];
    uint8_t stack_q[HN4_STACK_BUF_SIZE];
    uint8_t stack_t[HN4_STACK_BUF_SIZE];
    
    uint8_t *p_syn = NULL, *q_syn = NULL, *tmp = NULL;
    bool using_heap = false;

    if (byte_len <= HN4_STACK_BUF_SIZE) {
        p_syn = stack_p;
        q_syn = stack_q;
        tmp   = stack_t;
    } else {
        using_heap = true;
        p_syn = hn4_hal_mem_alloc(byte_len);
        q_syn = hn4_hal_mem_alloc(byte_len);
        tmp   = hn4_hal_mem_alloc(byte_len);
        
        if (!p_syn || !q_syn || !tmp) {
            if(p_syn) hn4_hal_mem_free(p_syn);
            if(q_syn) hn4_hal_mem_free(q_syn);
            if(tmp)   hn4_hal_mem_free(tmp);
            return HN4_ERR_NOMEM; 
        }
    }
    
    /* Ensure scratch buffers are clear */
    memset(p_syn, 0, byte_len);
    memset(q_syn, 0, byte_len);

    /* ---------------------------------------------------------
     * 2. OPTIMISTIC PATH: Single Failure (XOR Only)
     * --------------------------------------------------------- */
    if (fail_cnt == 1) {
    /* 
     * Strict Mode. Only use XOR if recovering a DATA column.
     * If Target is P or Q, we route through the solver to ensure
     * consistent handling of syndromes, even if XOR is theoretically possible for P.
     */
    if (target_col != q_col && target_col != p_col) {
        memset(result_buf, 0, byte_len);
        
        for (uint32_t i = 0; i < count; i++) {
            /* Skip the missing drive */
            if (i == target_col) continue;
            
            /* Skip Q (it does not contribute to P-based reconstruction) */
            if (i == q_col) continue;

                /* Read survivor */
                if (hn4_hal_sync_io(snapshot[i].dev_handle, HN4_IO_READ, io_lba, tmp, len) != HN4_OK) {
                    if (using_heap) {
                        hn4_hal_mem_free(p_syn); hn4_hal_mem_free(q_syn); hn4_hal_mem_free(tmp);
                    }
                    return HN4_ERR_PARITY_BROKEN; /* Double fault discovered during read */
                }
                
                /* Accumulate XOR sum directly into result buffer */
                _xor_buffer_fast(result_buf, tmp, byte_len);
            }
            
            if (using_heap) {
                hn4_hal_mem_free(p_syn); hn4_hal_mem_free(q_syn); hn4_hal_mem_free(tmp);
            }
            return HN4_OK;
        }
    }

    /* ---------------------------------------------------------
     * 3. PESSIMISTIC PATH: Dual Failure / Q-Regen
     * --------------------------------------------------------- */
    
    if (HN4_UNLIKELY(!_gf_ready)) _hn4_gf_init();

    /* Scan Survivors to build Syndromes */
    for (uint32_t i = 0; i < count; i++) {
        /* Skip the holes */
        if (i == fail_idxs[0] || i == fail_idxs[1]) continue; 

        /* Clean scratch buffer for read safety */
        memset(tmp, 0, byte_len);

        if (hn4_hal_sync_io(snapshot[i].dev_handle, HN4_IO_READ, io_lba, tmp, len) != HN4_OK) {
            if (using_heap) {
                hn4_hal_mem_free(p_syn); hn4_hal_mem_free(q_syn); hn4_hal_mem_free(tmp);
            }
            return HN4_ERR_PARITY_BROKEN; 
        }

        if (i == p_col) {
            /* P contributes to P-Syndrome */
            _xor_buffer_fast(p_syn, tmp, byte_len);
        } else if (i == q_col) {
            /* Q contributes to Q-Syndrome */
            _xor_buffer_fast(q_syn, tmp, byte_len);
        } else {
            /* Data Drive: Contributes to both P and Q */
            _xor_buffer_fast(p_syn, tmp, byte_len);
            
            uint32_t log_col = _hn4_phys_to_logical(i, p_col, q_col);
            uint8_t g = _gf_exp[log_col % 255];
            
            /* Q_Syndrome ^= Data * g^col */
            for (size_t k = 0; k < byte_len; k++) {
                q_syn[k] ^= _gf_mul(tmp[k], g);
            }
        }
    }

    /* ---------------------------------------------------------
     * 4. ALGEBRAIC SOLVER
     * --------------------------------------------------------- */
    int x = fail_idxs[0];
    int y = fail_idxs[1];
    
    /* Normalize: If one of them is the specific target, ensure it's in 'x' for return */
    if (fail_cnt == 1) {
        x = fail_idxs[0]; /* Only one failure */
    } else {
        if (y == (int)target_col) { int swap = x; x = y; y = swap; }
    }

    /* CASE A: Data + Data Failure */
    if (x != (int)p_col && x != (int)q_col && y != (int)p_col && y != (int)q_col) {
        uint32_t log_x = _hn4_phys_to_logical(x, p_col, q_col);
        uint32_t log_y = _hn4_phys_to_logical(y, p_col, q_col);
        uint8_t g_x = _gf_exp[log_x % 255];
        uint8_t g_y = _gf_exp[log_y % 255];

        if (g_x == g_y) {
            /* Logic error or configuration mismatch */
            if (using_heap) { hn4_hal_mem_free(p_syn); hn4_hal_mem_free(q_syn); hn4_hal_mem_free(tmp); }
            return HN4_ERR_PARITY_BROKEN;
        }

        uint8_t den_base = g_x ^ g_y;
        if (den_base == 0) {
            if (using_heap) { 
                hn4_hal_mem_free(p_syn); 
                hn4_hal_mem_free(q_syn); 
                hn4_hal_mem_free(tmp); 
            }
            return HN4_ERR_PARITY_BROKEN;
        }

        uint8_t den = _gf_inv(den_base);
        uint8_t* res_u8 = (uint8_t*)result_buf;
        
        /* Solve for X: x = (Q_syn ^ (P_syn * g_y)) / (g_x ^ g_y) */
        for (size_t k = 0; k < byte_len; k++) {
            uint8_t num = q_syn[k] ^ _gf_mul(p_syn[k], g_y);
            res_u8[k] = _gf_mul(num, den);
        }
    }
    /* CASE B: Data (x) + Q (y) Failure */
    else if (y == (int)q_col || x == (int)q_col) {
        /* 
         * If Q is dead, we solve X using only P.
         * P_syn currently holds: P ^ Sum(Survivors).
         * Since P = X ^ Sum(Survivors), then P_syn IS X.
         */
        memcpy(result_buf, p_syn, byte_len);
    }
    /* CASE C: Data (x) + P (y) Failure */
    else if (y == (int)p_col || x == (int)p_col) {
        /* 
         * Handle Data+P failure.
         * If P is dead, we solve X using Q.
         * X = Q_syn * g_x^-1
         */
        
        /* Ensure x is the data column (not the parity column) */
        int data_col = (x == (int)p_col) ? y : x;
        
        uint32_t log_x = _hn4_phys_to_logical(data_col, p_col, q_col);
        uint8_t g_inv_x = _gf_inv(_gf_exp[log_x % 255]);
        
        uint8_t* res_u8 = (uint8_t*)result_buf;
        for (size_t k = 0; k < byte_len; k++) {
            res_u8[k] = _gf_mul(q_syn[k], g_inv_x);
        }
    }

    if (using_heap) {
        hn4_hal_mem_free(p_syn); hn4_hal_mem_free(q_syn); hn4_hal_mem_free(tmp);
    }
    return HN4_OK;
}

/* =========================================================================
 * SPATIAL ROUTER (CORE DISPATCH)
 * ========================================================================= */

hn4_result_t _hn4_spatial_router(
    hn4_volume_t* vol, 
    uint8_t op, 
    hn4_addr_t lba, 
    void* buf, 
    uint32_t len, 
    hn4_u128_t file_id
) {
    /* 1. CHECK PROFILE & CONFIG */
    if (vol->sb.info.format_profile != HN4_PROFILE_HYPER_CLOUD) {
        return hn4_hal_sync_io(vol->target_device, op, lba, buf, len);
    }

    /* 
     * Small arrays (<=8 drives) stay on stack. Large arrays go to heap.
     * Prevents kernel stack overflow.
     */
    #define HN4_SMALL_ARRAY_LIMIT 8
    hn4_drive_t stack_snap[HN4_SMALL_ARRAY_LIMIT];
    hn4_drive_t* snapshot = stack_snap;
    bool using_heap = false;

    uint32_t count = 0;
    uint32_t mode = 0;

    /* Snapshot current topology under lock to prevent race with hot-plug/removal */
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
count = vol->array.count;
mode  = vol->array.mode;

if (count > HN4_MAX_ARRAY_DEVICES) count = 0; 

if (count > 0) {
    if (count > HN4_SMALL_ARRAY_LIMIT) {
        snapshot = hn4_hal_mem_alloc(count * sizeof(hn4_drive_t));
        if (!snapshot) {
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
            return HN4_ERR_NOMEM;
        }
        using_heap = true;
    }
    memcpy(snapshot, vol->array.devices, sizeof(hn4_drive_t) * count);
    
    /* Pin devices by incrementing usage counters on the SOURCE array */
    for (uint32_t i = 0; i < count; i++) {
        vol->array.devices[i].usage_counter++;
    }
}
hn4_hal_spinlock_release(&vol->locking.l2_lock);

/* Cleanup macro must now decrement refcounts */
#define CLEANUP_AND_RETURN(res) do { \
    if (count > 0) { \
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock); \
        /* Safety: Check bounds against live array to prevent OOB if array shrank */ \
        uint32_t safe_limit = (count < vol->array.count) ? count : vol->array.count; \
        for (uint32_t i = 0; i < safe_limit; i++) { \
            if (vol->array.devices[i].usage_counter > 0) \
                vol->array.devices[i].usage_counter--; \
        } \
        hn4_hal_spinlock_release(&vol->locking.l2_lock); \
    } \
    if (using_heap) hn4_hal_mem_free(snapshot); \
    return (res); \
} while(0)

    if (count == 0) {
        hn4_result_t res = hn4_hal_sync_io(vol->target_device, op, lba, buf, len);
        CLEANUP_AND_RETURN(res);
    }

    /* Device Feature Checks */
    bool is_hdd = (vol->sb.info.device_type_tag == HN4_DEV_HDD) || 
                  (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL);
    bool is_zns = (vol->sb.info.hw_caps_flags & HN4_HW_ZNS_NATIVE);
    bool is_usb = (vol->sb.info.format_profile == HN4_PROFILE_USB);

    /* 2. DISPATCH BY MODE */
    switch (mode) {
        
        /* --- MODE 1: MIRROR --- */
        case HN4_ARRAY_MODE_MIRROR: {
            if (op == HN4_IO_READ) {
                uint32_t start_idx = 0;
                if (is_hdd) {
                    /* Rotational optimization: Map LBA zones to specific mirrors */
                    uint64_t region = hn4_addr_to_u64(lba) >> 21; // 2MB chunks
                    start_idx = region % count;
                }

                /* 1. Determine Policy based on Profile */
                uint32_t max_retries = 2;
                uint32_t sleep_us = 1000;

                switch (vol->sb.info.format_profile) {
                    case HN4_PROFILE_GAMING:
                    case HN4_PROFILE_AI:
                    case HN4_PROFILE_HYPER_CLOUD:
                        max_retries = 0; /* Fail-over immediately (Total tries: 1) */
                        sleep_us = 0;
                        break;
                        
                    case HN4_PROFILE_USB:
                    case HN4_PROFILE_ARCHIVE:
                        max_retries = 5;
                        sleep_us = 100000; /* 100ms for spin-up/bus reset */
                        break;
                        
                    default:
                        max_retries = 2;
                        sleep_us = 1000;
                        break;
                }

                int attempts = 0;
                
                /* 2. Adaptive Loop */
                while (attempts <= (int)max_retries) {
                    /* Shift start index on retry to avoid hitting the same bad drive first */
                    uint32_t current_start = (start_idx + attempts) % count;

                    for (uint32_t k = 0; k < count; k++) {
                        uint32_t i = (current_start + k) % count;
                        if (snapshot[i].status != HN4_DEV_STAT_ONLINE) continue;

                        hn4_result_t res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);
                        if (_is_io_success(res)) CLEANUP_AND_RETURN(HN4_OK);
                        
                        if (_is_critical_failure(res)) {
                            _mark_device_offline(vol, i, snapshot[i].dev_handle);
                            snapshot[i].status = HN4_DEV_STAT_OFFLINE;
                        }
                    }
                    
                    /* Backoff before next retry pass */
                    attempts++;
                    if (attempts <= (int)max_retries && sleep_us > 0) {
                        hn4_hal_micro_sleep(sleep_us);
                    }
                }
                CLEANUP_AND_RETURN(HN4_ERR_HW_IO);
            }
            else { 
                /* WRITE / FLUSH / DISCARD */
                int success_count = 0;
                int online_targets = 0;

                for (uint32_t i = 0; i < count; i++) {
                    if (snapshot[i].status != HN4_DEV_STAT_ONLINE) continue;
                    
                    online_targets++;
                    hn4_result_t res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);

                    if (is_usb && !_is_io_success(res) && res != HN4_ERR_MEDIA_TOXIC) {
                        /* USB exception: Allow sleep for retry due to bus transients */
                        hn4_hal_micro_sleep(5000); 
                        res = hn4_hal_sync_io(snapshot[i].dev_handle, op, lba, buf, len);
                    }
                    
                    if (_is_io_success(res)) {
                        success_count++;
                    } else {
                        HN4_LOG_CRIT("Mirror Write Failed Dev %u (%d).", i, res);
                        if (_is_critical_failure(res)) {
                            _mark_device_offline(vol, i, snapshot[i].dev_handle);
                            snapshot[i].status = HN4_DEV_STAT_OFFLINE;
                        }
                    }
                }

                if (online_targets > 0 && success_count == online_targets) {
                    CLEANUP_AND_RETURN(HN4_OK);
                }
                
                if (success_count > 0) {
    if (success_count < online_targets) {
        HN4_LOG_CRIT("Mirror Divergence (Success %d/%d). Volume Degraded.", 
                     success_count, online_targets);
        
        /* 1. Mark State */
        __atomic_fetch_or(&vol->sb.info.state_flags, 
                          HN4_VOL_DEGRADED | HN4_VOL_DIRTY, __ATOMIC_RELEASE);
        
        hn4_hal_sync_io(vol->target_device, HN4_IO_FLUSH, 
                        hn4_addr_from_u64(0), NULL, 0);

        /* Return failure to signal lost redundancy to caller */
        CLEANUP_AND_RETURN(HN4_ERR_HW_IO);
    }
    CLEANUP_AND_RETURN(HN4_OK);
}
                
                /* Total Failure */
                CLEANUP_AND_RETURN(HN4_ERR_HW_IO);
            }
        }

        /* --- MODE 2: SHARD --- */
        case HN4_ARRAY_MODE_SHARD: {
            uint32_t target_idx;
            bool is_v7 = ((file_id.hi >> 12) & 0xF) == 7;

            if (is_hdd && is_v7) {
                target_idx = file_id.hi % count;
            } else {
                target_idx = _resolve_shard_index(file_id, count);
            }
            
            /* Offline Remap Logic: Simple Rotate */
            uint32_t attempts = 0;
            while (snapshot[target_idx].status != HN4_DEV_STAT_ONLINE && attempts < count) {
                target_idx = (target_idx + 1) % count;
                attempts++;
            }

            if (snapshot[target_idx].status != HN4_DEV_STAT_ONLINE) {
                CLEANUP_AND_RETURN(HN4_ERR_HW_IO); /* All shards offline */
            }

            hn4_hal_device_t* dev = snapshot[target_idx].dev_handle;
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
            if (!caps) CLEANUP_AND_RETURN(HN4_ERR_INTERNAL_FAULT);

            uint32_t ss = caps->logical_block_size;
            if (ss == 0) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
 
            hn4_addr_t submission_lba = lba;

            if (is_zns && op == HN4_IO_WRITE) {
                uint64_t val_lba = hn4_addr_to_u64(lba);
                uint64_t zone_sectors = caps->zone_size_bytes / ss;
                
                uint64_t zone_start = (val_lba / zone_sectors) * zone_sectors;
                uint64_t zone_end   = zone_start + zone_sectors;

                /* 
                 * 1. Check if IO fits in the remaining space of the zone.
                 * 2. Pass Zone Start LBA (Handle) to HAL, not the specific offset.
                 */
                 if ((val_lba + len) <= zone_end) {
                    op = HN4_IO_ZONE_APPEND;
                    /* Update submission target to be the Zone Handle */
                    submission_lba = hn4_addr_from_u64(zone_start); 
                } else {
                    CLEANUP_AND_RETURN(HN4_ERR_ZONE_FULL);
                }
            }

            if (op != HN4_IO_ZONE_APPEND) {
                #ifdef HN4_USE_128BIT
                    hn4_u128_t cap_bytes = caps->total_capacity_bytes;
                    hn4_u128_t max_sectors = hn4_u128_div_u64(cap_bytes, ss);
                    
                    hn4_u128_t req_start = lba;
                    hn4_u128_t req_len = hn4_u128_from_u64(len);
                    hn4_u128_t req_end = hn4_u128_add(req_start, req_len);
                    
                    if (hn4_u128_cmp(req_end, req_start) < 0) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
                    if (hn4_u128_cmp(req_end, max_sectors) > 0) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
                #else
                    uint64_t max_sectors = caps->total_capacity_bytes / ss;
                    if (len > max_sectors || hn4_addr_to_u64(lba) > (max_sectors - len)) {
                        CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
                    }
                #endif
            }

            hn4_result_t res = hn4_hal_sync_io(dev, op, submission_lba, buf, len);

            if (!_is_io_success(res)) {
                if (_is_critical_failure(res)) {
                    _mark_device_offline(vol, target_idx, snapshot[target_idx].dev_handle);
                    snapshot[target_idx].status = HN4_DEV_STAT_OFFLINE;
                }
                CLEANUP_AND_RETURN(res);
            }
            CLEANUP_AND_RETURN(HN4_OK);
        }

        /* --- MODE 3: PARITY (RAID-5/6 Equivalent) --- */
        case HN4_ARRAY_MODE_PARITY: {
            _hn4_gf_init(); 

            if (count < 4) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);

            const hn4_hal_caps_t* caps = hn4_hal_get_caps(snapshot[0].dev_handle);
            uint32_t stripe_ss = caps->logical_block_size;
            uint32_t stripe_unit = 128; /* 64KB Stripe Unit */
            
            uint32_t data_cols = count - 2; 
            
            /* Guard against integer overflow on huge arrays/corruption */
            if (data_cols > (UINT64_MAX / stripe_unit)) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);

            uint64_t stripe_width = (uint64_t)data_cols * stripe_unit;

            hn4_addr_t current_lba = lba;
            uint32_t   current_len = len;
            uint8_t*   current_buf = (uint8_t*)buf;

            while (current_len > 0) {
                uint64_t row = 0;
                uint64_t offset_in_row = 0;
                
                /* LBA decomposition */
                #ifdef HN4_USE_128BIT
                    hn4_u128_t lba_128 = current_lba;
                    hn4_u128_t width_128 = hn4_u128_from_u64(stripe_width);
                    
                    hn4_u128_t row_128 = hn4_u128_div_u64(lba_128, stripe_width);
                    hn4_u128_t off_128 = hn4_u128_mod(lba_128, width_128);
                    
                    if (row_128.hi > 0) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
                    row = row_128.lo;
                    offset_in_row = off_128.lo;
                #else
                    row = current_lba / stripe_width;
                    offset_in_row = current_lba % stripe_width;
                #endif
                
                uint32_t col_logical = (uint32_t)(offset_in_row / stripe_unit);
                uint32_t offset_in_col = (uint32_t)(offset_in_row % stripe_unit);
                uint32_t chunk = (current_len < (stripe_unit - offset_in_col)) 
                                 ? current_len : (stripe_unit - offset_in_col);

                /* Rotational Parity Layout (Left Symmetric) */
                uint32_t p_col = (count - 1) - (row % count);
                uint32_t q_col = (p_col == 0) ? count - 1 : p_col - 1;
                
                /* 
                 * We must skip the parity columns in the physical array.
                 */
                uint32_t phys_col = col_logical;
                uint32_t s1 = (p_col < q_col) ? p_col : q_col;
                uint32_t s2 = (p_col < q_col) ? q_col : p_col;
                
                if (phys_col >= s1) phys_col++;
                if (phys_col >= s2) phys_col++;

                hn4_addr_t row_base_lba;
                #ifdef HN4_USE_128BIT
                    hn4_u128_t r_128 = hn4_u128_from_u64(row);
                    row_base_lba = hn4_u128_mul_u64(r_128, stripe_unit);
                #else
                    if (row > (UINT64_MAX / stripe_unit)) CLEANUP_AND_RETURN(HN4_ERR_GEOMETRY);
                    row_base_lba = row * stripe_unit;
                #endif

                hn4_addr_t target_lba = hn4_addr_add(row_base_lba, offset_in_col);

                /* 
                 * Acquire lock for the row to prevent concurrent RMW races (Write Hole).
                 */
                /* Simple hash to scatter sequential rows across 64 locks */
                uint64_t mix = row;
                mix ^= (mix >> 33);
                mix *= 0xff51afd7ed558ccdULL;
                mix ^= (mix >> 33);
                uint32_t lock_idx = mix % HN4_CORTEX_SHARDS;
                hn4_spinlock_t* stripe_lock = &vol->locking.shards[lock_idx].lock;

                if (op == HN4_IO_WRITE) {
                    /* RMW Path: Alloc Buffers */
                    size_t io_sz = (size_t)chunk * stripe_ss;
                    
                    if (io_sz > SIZE_MAX / 3) CLEANUP_AND_RETURN(HN4_ERR_NOMEM);

                    uint8_t* scratch = hn4_hal_mem_alloc(io_sz * 3);
                    if (!scratch) CLEANUP_AND_RETURN(HN4_ERR_NOMEM);
                    
                    uint8_t* d_old = scratch;
                    uint8_t* p_old = scratch + io_sz;
                    uint8_t* q_old = scratch + (io_sz * 2);

                     /* ACQUIRE LOCK */
                    hn4_hal_spinlock_acquire(stripe_lock);

                    /* 
                     * IO COMPLEXITY BOUND (Healthy Case):
                     * Reads:  D_old + P_old + Q_old  (3 ops)
                     * WAL:    Log Append + Flush     (2 ops)
                     * Writes: D_new + P_new + Q_new  (3 ops)
                     * Flush:  D + P + Q              (3 ops)
                     * --------------------------------------
                     * Total: 11 Ops (Constant Time O(1))
                     * Independent of Array Width (N).
                     */

                    bool d_ok = (snapshot[phys_col].status == HN4_DEV_STAT_ONLINE);
                    bool p_ok = (snapshot[p_col].status == HN4_DEV_STAT_ONLINE);
                    bool q_ok = (snapshot[q_col].status == HN4_DEV_STAT_ONLINE);

/* 
 * 1. Read Old Data (Robust RMW)
 * We need Old Data to calculate the Delta. If the drive is dead, we must
 * RECONSTRUCT it from survivors.
 */
if (d_ok) {
    if (hn4_hal_sync_io(snapshot[phys_col].dev_handle, HN4_IO_READ, target_lba, d_old, chunk) != HN4_OK) {
        d_ok = false; /* Failed read, treat as offline for write */
        /* Mark offline? In strict mode yes, here we just fallthrough to reconstruct */
    }
}

if (!d_ok) {
    /* DATA DRIVE MISSING: Reconstruct 'd_old' to allow Parity Update */
    hn4_result_t rc_res = _hn4_reconstruct_helix(
        vol, snapshot, count, stripe_ss,
        p_col, q_col, phys_col,
        target_lba, d_old, chunk
    );
    
    if (rc_res != HN4_OK) {
        /* Double fault (Quorum lost) - Cannot calculate delta */
        hn4_hal_spinlock_release(stripe_lock);
        hn4_hal_mem_free(scratch);
        CLEANUP_AND_RETURN(rc_res);
    }
}
                    /* Read Old Parity (Only if drive is alive) */
                    if (p_ok) {
                        if (hn4_hal_sync_io(snapshot[p_col].dev_handle, HN4_IO_READ, target_lba, p_old, chunk) != HN4_OK) p_ok = false;
                    }

                    /* Read Old Q-Parity */
                    if (q_ok) {
                        if (hn4_hal_sync_io(snapshot[q_col].dev_handle, HN4_IO_READ, target_lba, q_old, chunk) != HN4_OK) q_ok = false;
                    }

                   /* 2. Compute Deltas */
                    _xor_buffer_fast(d_old, current_buf, io_sz); /* d_old becomes delta */
                    
                    /* 
                     * Unconditionally apply delta to P/Q buffers.
                     * If p_ok is false, p_old contains garbage -> p_old becomes new garbage.
                     * This is safe because we gate the WRITE (Step 4) on p_ok/q_ok.
                     */
                    _hn4_helix_apply_delta(p_old, q_old, d_old, io_sz, (uint8_t)col_logical, p_ok, q_ok);

                    /* 3. LOG INTENT (WAL) */
                    uint64_t audit_payload = row;
                    
                    /* 
                     * Log the viability map (Who is expected to receive this write).
                     * We do NOT check for write failure here; the log records the *attempt*.
                     */
                    uint8_t  health_map = (d_ok ? 1 : 0) | (p_ok ? 2 : 0) | (q_ok ? 4 : 0);
                    audit_payload |= ((uint64_t)health_map << 56);

                    #ifdef HN4_USE_128BIT
                    hn4_result_t log_res = hn4_chronicle_append(vol->target_device, vol, HN4_CHRONICLE_OP_WORMHOLE, 
                         target_lba, 
                         hn4_addr_from_u64(audit_payload), 
                         0);
                    #else
                    hn4_result_t log_res = hn4_chronicle_append(vol->target_device, vol, HN4_CHRONICLE_OP_WORMHOLE, 
                         hn4_addr_from_u64(target_lba), 
                         hn4_addr_from_u64(audit_payload), 
                         0);
                    #endif

                    if (log_res != HN4_OK) {
                        /* If we failed to log, we must not modify data. Abort. */
                        hn4_hal_spinlock_release(stripe_lock);
                        hn4_hal_mem_free(scratch);
                        CLEANUP_AND_RETURN(HN4_ERR_AUDIT_FAILURE);
                    }

                    /* Final Barrier ensures Log is durable before we touch Data */
                    hn4_hal_sync_io(vol->target_device, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);

                    /* 4. EXECUTE WRITES (Degraded Aware) */
                    if (d_ok) {
                        if (hn4_hal_sync_io(snapshot[phys_col].dev_handle, HN4_IO_WRITE, target_lba, current_buf, chunk) != HN4_OK) {
                            _mark_device_offline(vol, phys_col, snapshot[phys_col].dev_handle);
                        }
                    }

                    if (p_ok) {
                        if (hn4_hal_sync_io(snapshot[p_col].dev_handle, HN4_IO_WRITE, target_lba, p_old, chunk) != HN4_OK) {
                            _mark_device_offline(vol, p_col, snapshot[p_col].dev_handle);
                        }
                    }

                    if (q_ok) {
                        if (hn4_hal_sync_io(snapshot[q_col].dev_handle, HN4_IO_WRITE, target_lba, q_old, chunk) != HN4_OK) {
                            _mark_device_offline(vol, q_col, snapshot[q_col].dev_handle);
                        }
                    }

                    /* 
                     * Durability Barrier.
                     * We must flush the data drives before releasing the lock.
                     * Otherwise, a crash here leaves the WAL committed but data volatile.
                     */
                    if (d_ok) hn4_hal_sync_io(snapshot[phys_col].dev_handle, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
                    if (p_ok) hn4_hal_sync_io(snapshot[p_col].dev_handle, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
                    if (q_ok) hn4_hal_sync_io(snapshot[q_col].dev_handle, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
                    
                    /* RELEASE LOCK */
                    hn4_hal_spinlock_release(stripe_lock);
                    
                    hn4_hal_mem_free(scratch);
                }
                else {
                    /* READ PATH */
                    hn4_result_t res = HN4_ERR_HW_IO;
                    bool is_offline = (snapshot[phys_col].status != HN4_DEV_STAT_ONLINE);

                    if (!is_offline) {
                        res = hn4_hal_sync_io(snapshot[phys_col].dev_handle, op, target_lba, current_buf, chunk);
                    }
                    
                    if (is_offline || !_is_io_success(res)) {
                        
                        if (!is_offline) {
                            /* Mark Primary Failed */
                            _mark_device_offline(vol, phys_col, snapshot[phys_col].dev_handle);
                            snapshot[phys_col].status = HN4_DEV_STAT_OFFLINE;
                        }

                        /* EXECUTE HELIX-D RECONSTRUCTION */
                        res = _hn4_reconstruct_helix(
                            vol, snapshot, count, stripe_ss,
                            p_col, q_col, phys_col,
                            target_lba,
                            current_buf, chunk
                        );

                        if (res != HN4_OK) {
                            CLEANUP_AND_RETURN(res); /* Unrecoverable */
                        }
                    }
                }

                current_len -= chunk;
                current_lba = hn4_addr_add(current_lba, chunk);
                current_buf += ((size_t)chunk * stripe_ss);
            }
            CLEANUP_AND_RETURN(HN4_OK);
        }

        default:
            CLEANUP_AND_RETURN(HN4_ERR_INTERNAL_FAULT);
    }
}
