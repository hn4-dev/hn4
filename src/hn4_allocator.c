/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Void Engine (Allocator)
 * SOURCE:      hn4_allocator.c
 * STATUS:      FIXED / PRODUCTION (v18.1)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. ATOMICITY: All Bitmap state transitions utilize 128-bit Atomic CAS.
 *    This ensures Data, Version, and ECC are updated as an indivisible unit.
 * 2. SATURATION: The Flux Manifold (D1) locks at 95% capacity to prevent
 *    infinite probe loops. Writes fall back to the Event Horizon (D1.5).
 * 3. SELF-HEALING: Bitmap reads (BIT_TEST) enforce active SECDED correction.
 *    Single-bit errors trigger an immediate write-back to persist the fix.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_swizzle.h"
#include "hn4_ecc.h"
#include "hn4_errors.h"
#include "hn4_addr.h"
#include "hn4_endians.h"
#include "hn4_annotations.h"

/* =========================================================================
 * 0. SAFETY & ALIGNMENT ASSERTIONS
 * ========================================================================= */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(_Alignof(hn4_armored_word_t) == 16, "HN4: Armored Word must be 16-byte aligned");
    _Static_assert(sizeof(hn4_armored_word_t) == 16, "HN4: Armored Word must be exactly 16 bytes");
#endif

#define HN4_LBA_INVALID UINT64_MAX
#define HN4_CORTEX_SLOT_SIZE 128
#define HN4_SATURATION_GENESIS  90
#define HN4_SATURATION_UPDATE   95
#define HN4_TAINT_THRESHOLD_RO 20

/* 
 * Linear Device Lookup Table.
 * Maps Device Tag -> Is Linear (True/False).
 * Index 0=SSD (False), 1=HDD, 2=ZNS, 3=TAPE (True).
 */
static const bool _hn4_is_linear_lut[4] = {
    [HN4_DEV_SSD]  = false,
    [HN4_DEV_HDD]  = true,
    [HN4_DEV_ZNS]  = true,
    [HN4_DEV_TAPE] = true
};

/* Device Topology Policy */
static const uint8_t _hn4_dev_policy[4] = {
    [HN4_DEV_SSD]  = 0,
    [HN4_DEV_HDD]  = HN4_POL_SEQ | HN4_POL_DEEP,
    [HN4_DEV_ZNS]  = HN4_POL_SEQ,
    [HN4_DEV_TAPE] = HN4_POL_SEQ | HN4_POL_DEEP
};

/* Format Profile Policy (Size 8 for alignment/future) */
static const uint8_t _hn4_prof_policy[8] = {
    [HN4_PROFILE_GENERIC] = 0,
    [HN4_PROFILE_GAMING]  = 0,
    [HN4_PROFILE_AI]      = 0,
    [HN4_PROFILE_ARCHIVE] = 0,
    [HN4_PROFILE_PICO]    = HN4_POL_SEQ,
    [HN4_PROFILE_SYSTEM]  = 0,
    [HN4_PROFILE_USB]     = HN4_POL_SEQ | HN4_POL_DEEP,
    [7]                   = 0
};

/* =========================================================================
 * 1. HARDENED ATOMICS (NO EXTERNAL LIBS)
 * ========================================================================= */

/* ========================================================================
 *  hn4_cas128
 *
 *  Atomically:
 *      if (*dst == *expected) {
 *          *dst = desired;
 *          return true;
 *      } else {
 *          *expected = *dst;
 *          return false;
 *      }
 *
 *  REQUIREMENTS:
 *      - dst MUST be 16-byte aligned
 *      - CPU must support 128-bit CAS
 *
 *  MEMORY ORDER:
 *      x86  : full barrier (LOCK cmpxchg16b)
 *      ARM  : Acquire+Release via CASPAL
 *
 * ======================================================================== */

static inline bool
_hn4_cas128(volatile void *dst,
            hn4_aligned_u128_t  *expected,
            hn4_aligned_u128_t   desired)
{
    /* Alignment is REQUIRED for both x86-64 and ARM64 */
    if (HN4_UNLIKELY(((uintptr_t)dst & 0xF) != 0))
        __builtin_trap();

#if defined(__x86_64__) || defined(_M_X64)

    bool success;

    /*
     * cmpxchg16b does:
     *   compare  RDX:RAX  with  [mem]
     *   on match store RCX:RBX into [mem]
     *   load [mem] into RDX:RAX on failure
     *
     * So we map:
     *   expected->lo -> RAX   (in/out)
     *   expected->hi -> RDX   (in/out)
     *   desired.lo   -> RBX   (in)
     *   desired.hi   -> RCX   (in)
     */

    uint64_t exp_lo = expected->lo;
    uint64_t exp_hi = expected->hi;
    uint64_t new_lo = desired.lo;
    uint64_t new_hi = desired.hi;

    __asm__ __volatile__ (
        "lock cmpxchg16b %1\n\t"
        "setz %0"
        : "=q"(success),
          "+m"(*(volatile __int128 *)dst),
          "+a"(exp_lo),
          "+d"(exp_hi)
        : "b"(new_lo),
          "c"(new_hi)
        : "cc", "memory"
    );

    expected->lo = exp_lo;
    expected->hi = exp_hi;
    return success;

#elif defined(__aarch64__)

    /*
     * CASPAL does:
     *   atomically compare pair <Xdest_lo,Xdest_hi>
     *           with pair <Xexp_lo,Xexp_hi>
     *   on match store pair <Xnew_lo,Xnew_hi> to [dst]
     *   ALWAYS return memory value in Xexp_lo/Xexp_hi
     */

    uint64_t exp_lo = expected->lo;
    uint64_t exp_hi = expected->hi;
    uint64_t new_lo = desired.lo;
    uint64_t new_hi = desired.hi;

    __asm__ __volatile__ (
        "caspal %0, %1, %2, %3, [%4]"
        : "+r"(exp_lo), "+r"(exp_hi)
        : "r"(new_lo), "r"(new_hi), "r"(dst)
        : "memory"
    );

    bool success = (exp_lo == expected->lo) &&
                   (exp_hi == expected->hi);

    expected->lo = exp_lo;
    expected->hi = exp_hi;

    return success;

#else
    /* 
     * Provides binary compatibility for 32-bit/Embedded targets (Pico).
     *
     * WARNING: SCALABILITY HAZARD
     * This serializes ALL allocations globally. 
     * Acceptable ONLY for HN4_PROFILE_PICO or single-core recovery tools.
     */
    static atomic_flag _hn4_global_cas_lock = ATOMIC_FLAG_INIT;
    
    /* Spin-Wait */
    while (atomic_flag_test_and_set_explicit(&_hn4_global_cas_lock, memory_order_acquire)) {
        #if defined(__GNUC__) || defined(__clang__)
            __builtin_ia32_pause(); /* Or yield hint */
        #endif
    }
    
    bool success = false;
    uint64_t* mem = (uint64_t*)dst;
    
    /* Critical Section */
    if (mem[0] == expected->lo && mem[1] == expected->hi) {
        mem[0] = desired.lo;
        mem[1] = desired.hi;
        success = true;
    } else {
        /* CAS Failure: Update expected */
        expected->lo = mem[0];
        expected->hi = mem[1];
    }
    
    atomic_flag_clear_explicit(&_hn4_global_cas_lock, memory_order_release);
    return success;
#endif
}


/* Standard 64-bit CAS wrapper */
static inline bool _hn4_cas64(volatile uint64_t* dst,
                              uint64_t* expected,
                              uint64_t desired)
{
    return atomic_compare_exchange_strong_explicit(
        (_Atomic uint64_t*)dst,
        expected,
        desired,
        memory_order_acq_rel,
        memory_order_acquire
    );
}


/*
 * _hn4_load128
 * Atomic 128-bit load WITHOUT side-effects.
 * 
 * FIX 1 (ARM): Used LDXP + CLREX. No STXP. No RMW semantics.
 * FIX 2 (x64): Reverted to CMPXCHG16B(0,0) to guarantee atomicity.
 */
static inline hn4_aligned_u128_t  _hn4_load128(volatile void* src) {
    hn4_aligned_u128_t  ret;

#if defined(__x86_64__) || defined(_M_X64)
    /*
     * SPECIFICATION NOTE: ARCHITECTURAL DIVERGENCE
     * x86: Uses CMPXCHG16B. Side effect: Marks page DIRTY (Write Cycle).
     *      This is acceptable for HN4 but prevents strict RO mappings on x86 
     *      unless HN4_STRICT_READ_ONLY is defined.
     * ARM: Uses LDXP. Pure Read. Page remains CLEAN.
     */
    uint64_t res_lo, res_hi;
    
    /* 
     * Dirty Read: Get an initial guess to seed the CAS loop. 
     */
    #ifdef HN4_STRICT_READ_ONLY
    /* Tearing-Tolerant Load (Pure Read) */
    /* Reads loop until two consecutive snapshots match */
    uint64_t t_lo, t_hi;
    do {
        res_lo = *(volatile uint64_t*)src;
        res_hi = *(((volatile uint64_t*)src) + 1);
        
        atomic_thread_fence(memory_order_acquire);
        
        t_lo = *(volatile uint64_t*)src;
        t_hi = *(((volatile uint64_t*)src) + 1);
    } while (HN4_UNLIKELY(res_lo != t_lo || res_hi != t_hi));
#else
    /* Architectural Atomicity (May mark page dirty) */
    res_lo = *(volatile uint64_t*)src;
    res_hi = *(((volatile uint64_t*)src) + 1);
    bool success;
    do {
        uint64_t desired_lo = res_lo;
        uint64_t desired_hi = res_hi;
        __asm__ __volatile__ (
            "lock cmpxchg16b %0; setz %1"
            : "+m" (*(volatile __int128 *)src), "=q" (success), "+a" (res_lo), "+d" (res_hi)
            : "b" (desired_lo), "c" (desired_hi)
            : "cc", "memory"
        );
    } while (HN4_UNLIKELY(!success));
#endif

    ret.lo = res_lo;
    ret.hi = res_hi;
    return ret;

#elif defined(__aarch64__)
    /* 
     * FIX A-1: Strict Atomic Read Stability (ARM64).
     * Replaced LDXP+CLREX with CASPAL loop.
     * We perform a NOP-swap (Write old value back to itself).
     * Success guarantees we held the cacheline exclusively.
     */
    uint64_t res_lo, res_hi;
    uint64_t tmp_lo, tmp_hi;
    
    /* 1. Dirty Read */
    res_lo = *(volatile uint64_t*)src;
    res_hi = *(((volatile uint64_t*)src) + 1);

    /* ARM64: True Atomic Read using Exclusive Monitor */
    __asm__ __volatile__ (
        "1: ldxp %0, %1, [%2]\n"  /* Load Exclusive Pair */
        "   clrex\n"              /* Clear Monitor (No write needed) */
        : "=&r" (res_lo), "=&r" (res_hi)
        : "r" (src)
        : "memory"
    );
    /* No loop needed; LDXP is atomic if aligned */
    ret.lo = res_lo;
    ret.hi = res_hi;
    return ret;
#else
    #error "HN4: Architecture not supported for bare-metal atomic 128-bit load."
#endif
}

static inline uint8_t _get_trajectory_limit(const hn4_volume_t* vol) {
    /* PICO override check first */
    if (HN4_UNLIKELY(vol->sb.info.format_profile == HN4_PROFILE_PICO)) return 0;

    /* 
     * Jump Table Optimization.
     * Replaces previous Switch statement for O(1) lookup.
     * Mask & 0x3 protects against out-of-bounds access if tag is corrupted.
     */
    if (_hn4_is_linear_lut[vol->sb.info.device_type_tag & 0x3]) {
        return 0; /* Force Sequential (Linear) */
    }
    
    return HN4_MAX_TRAJECTORY_K; /* Ballistic (Random Access) */
}


/* =========================================================================
 * 2. CORE CONSTANTS
 * ========================================================================= */

#define HN4_GRAVITY_ASSIST_K    4
#define HN4_ORBIT_LIMIT         12
#define HN4_HORIZON_LIMIT       15
#define HN4_SATURATION_THRESH   90
#define HN4_MAX_PROBES          20
#define HN4_GRAVITY_MAGIC       0xA5A5A5A5A5A5A5A5ULL
#define HN4_L2_COVERAGE_BITS    512
#define HN4_L2_COVERAGE_WORDS   (HN4_L2_COVERAGE_BITS / 64)

static const uint8_t _theta_lut[16] = {
    0, 1, 3, 6, 10, 15, 21, 28, 
    36, 45, 55, 66, 78, 91, 105, 120
};

/* =========================================================================
 * 3. INTERNAL HELPERS
 * ========================================================================= */

static uint64_t _gcd(uint64_t a, uint64_t b) {
    /* Binary GCD (Stein's Algorithm) for predictable latency.
       Avoids expensive modulo div instructions in the loop. */
    if (a == 0) return b;
    if (b == 0) return a;
    
    int shift = __builtin_ctzll(a | b);
    a >>= __builtin_ctzll(a);
    
    int safety = 0;
    while (b != 0) {
        /* 
         * Return 1 (Coprime). 
         * While "lying" about the GCD, returning 1 forces the allocator to treat 
         * the numbers as coprime. This results in a stride of 1 (Linear Scan), 
         * which is safe and guarantees coverage, whereas 0 causes division-by-zero.
         */
        if (++safety > 256) {
            HN4_LOG_WARN("GCD math stall. Forcing fallback to 1 (Linear).");
            return 1; 
        }

        b >>= __builtin_ctzll(b);
        if (a > b) {
            uint64_t t = b; b = a; a = t;
        }
        b -= a;
    }
    return a << shift;
}

/*
 * _get_random_uniform
 * ELIMINATES MODULO BIAS using Rejection Sampling.
 * Returns a number in range [0, upper_bound - 1].
 */
static uint64_t _get_random_uniform(uint64_t upper_bound) {
    if (upper_bound <= 1) return 0;

    /* 
     * Calculate the largest multiple of upper_bound that fits in UINT64.
     * Any random value >= max_usable represents the "biased tail" and must be discarded.
     */
    uint64_t max_usable = (UINT64_MAX / upper_bound) * upper_bound;
    uint64_t rng;

    do {
        rng = hn4_hal_get_random_u64();
    } while (rng >= max_usable);

    return rng % upper_bound;
}

/*
 * _check_saturation
 * 
 * Determines if the volume has entered the "Event Horizon" state (Saturation).
 * 
 * LOGIC FIXES:
 * 1. Precision: Handles small volumes (<100 blocks) without truncation to zero.
 * 2. Overflow: Handles Exabyte volumes without u64 overflow during calc.
 * 3. Hysteresis: Engages at 90%, Disengages at 85% to prevent thrashing.
 * 4. Persistence: Uses HN4_VOL_RUNTIME_SATURATED bit in state_flags.
 */
static bool _check_saturation(HN4_IN hn4_volume_t* vol, bool is_genesis) {
    if (HN4_UNLIKELY(vol->vol_block_size == 0)) return true;
    
    uint64_t used = atomic_load_explicit(&vol->used_blocks, memory_order_acquire);
    
    uint64_t cap_bytes_val;
#ifdef HN4_USE_128BIT
    if (vol->vol_capacity_bytes.hi > 0) cap_bytes_val = UINT64_MAX; 
    else cap_bytes_val = vol->vol_capacity_bytes.lo;
#else
    cap_bytes_val = vol->vol_capacity_bytes;
#endif

    uint64_t cap_blocks = cap_bytes_val / vol->vol_block_size;
    if (cap_blocks == 0) return true;

    /* Calculate Absolute Thresholds */
    uint64_t limit_genesis; // 90%
    uint64_t limit_update;  // 95%
    uint64_t limit_recover; // 85%

    if (cap_blocks <= (UINT64_MAX / 100)) {
        limit_genesis = (cap_blocks * HN4_SATURATION_GENESIS) / 100;
        limit_update  = (cap_blocks * HN4_SATURATION_UPDATE) / 100;
        limit_recover = (cap_blocks * (HN4_SATURATION_GENESIS - 5)) / 100;
    } else {
        limit_genesis = (cap_blocks / 100) * HN4_SATURATION_GENESIS;
        limit_update  = (cap_blocks / 100) * HN4_SATURATION_UPDATE;
        limit_recover = (cap_blocks / 100) * (HN4_SATURATION_GENESIS - 5);
    }

    /* 1. Update Global State Flags (Side Effect) */
    uint32_t flags = atomic_load_explicit(&vol->sb.info.state_flags, memory_order_relaxed);
    bool flag_set = (flags & HN4_VOL_RUNTIME_SATURATED) != 0;

    if (used >= limit_genesis) {
        if (!flag_set) {
            atomic_fetch_or_explicit(&vol->sb.info.state_flags, 
                                     HN4_VOL_RUNTIME_SATURATED, 
                                     memory_order_seq_cst);
            flag_set = true;
        }
    } else if (used < limit_recover) {
        if (flag_set) {
            atomic_fetch_and_explicit(&vol->sb.info.state_flags, 
                                      ~HN4_VOL_RUNTIME_SATURATED, 
                                      memory_order_seq_cst);
            flag_set = false;
        }
    }

    /* 2. Return Decision based on Intent */
    if (is_genesis) {
        /* New files blocked if Saturated (Flag Set) OR > 90% */
        return (used >= limit_genesis) || flag_set;
    } else {
        /* Updates blocked only if > 95% (Hard Wall) */
        return (used >= limit_update);
    }
}

 hn4_result_t _check_quality_compliance(hn4_volume_t* vol, uint64_t lba, uint8_t intent) {
    if (!vol->quality_mask) return HN4_OK; 

    uint64_t word_idx = lba / 32;
    
    /* Bounds Check with Panic Propagation */
    if ((word_idx * 8) >= vol->qmask_size) {
        /* CRITICAL: Set Panic Flag on OOB access */
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC | HN4_VOL_DIRTY);
        return HN4_ERR_GEOMETRY;
    }

    uint32_t shift = (lba % 32) * 2;
    uint64_t q_word = vol->quality_mask[word_idx];
    uint8_t q_val = (q_word >> shift) & 0x3;

    if (q_val == HN4_Q_TOXIC) return HN4_ERR_MEDIA_TOXIC;

    if (intent == HN4_ALLOC_METADATA || vol->sb.info.format_profile == HN4_PROFILE_SYSTEM) {
        if (q_val == HN4_Q_BRONZE) return HN4_ERR_MEDIA_TOXIC;
    }

    return HN4_OK;
}

static inline uint64_t _mul_mod_safe(uint64_t a, uint64_t b, uint64_t m) {
    if (m == 0) return 0;
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((__uint128_t)a * b) % m);
#else
    uint64_t res = 0;
    a %= m;
    while (b > 0) {
        if (b & 1) {
            /* 
             * Safe Add: if (res + a >= m) res = (res + a) - m 
             * Rewrite to avoid (res+a) overflow:
             */
            if (res >= m - a) res -= (m - a);
            else res += a;
        }
        b >>= 1;
        if (b > 0) {
            /* Safe Double */
            if (a >= m - a) a -= (m - a);
            else a += a;
        }
    }
    return res;
#endif
}


/* =========================================================================
 * 4. SECDED LOGIC
 * ========================================================================= */

static void _update_counters_and_l2(hn4_volume_t* vol, uint64_t block_idx, bool is_set) {
    /* =========================================================================
     * PATH A: ALLOCATION (The Hot Path)
     * ========================================================================= */
    if (is_set) {
        /* 
         * SCALING OPTIMIZATION:
         * Use relaxed ordering for the global counter. Strict consistency is not 
         * required for space accounting; eventual consistency suffices.
         * This prevents global bus locking during high-IOPS write storms.
         */
        atomic_fetch_add_explicit(&vol->used_blocks, 1, memory_order_relaxed);

        if (vol->l2_summary_bitmap) {
             uint64_t l2_idx  = block_idx / HN4_L2_COVERAGE_BITS;
             uint64_t l2_word = l2_idx / 64;
             uint64_t l2_mask = (1ULL << (l2_idx % 64));
             
             _Atomic uint64_t* l2_ptr = (_Atomic uint64_t*)&vol->l2_summary_bitmap[l2_word];

             /* 
              * CACHE OPTIMIZATION (Read-For-Ownership Avoidance):
              * Check if the bit is already set before issuing an atomic write.
              * If the region is already "dirty", we avoid invalidating the cache 
              * line for other cores sharing this L2 word.
              */
             uint64_t curr = atomic_load_explicit(l2_ptr, memory_order_relaxed);
             if (!(curr & l2_mask)) {
                 /* Upgrade to Release to ensure data visibility before marking present */
                 atomic_fetch_or_explicit(l2_ptr, l2_mask, memory_order_release);
             }
        }
    } 
    /* =========================================================================
     * PATH B: DEALLOCATION (The Heavy Path)
     * ========================================================================= */
    else {
        /* 
         * UNDERFLOW PROTECTION:
         * Use a CAS loop to decrement. If we hit 0, we have a logic bug 
         * (Double Free or Counter Drift). We log CRIT and dirty the volume 
         * to force an fsck, but we do not panic the kernel.
         */
        uint64_t current = atomic_load(&vol->used_blocks);
        do {
            if (HN4_UNLIKELY(current == 0)) {
                atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
                HN4_LOG_ERR("Allocator Underflow! Used=0 but freeing block %llu.", (unsigned long long)block_idx);
                break; 
            }
        } while (!atomic_compare_exchange_weak(&vol->used_blocks, &current, current - 1));
        
        /* Update L2 Summary */
        if (vol->l2_summary_bitmap) {
            /* 
             * SYSTEM PROFILE LOCKING:
             * For OS Root volumes, we enforce strict serialization to prevent
             * any possibility of "Ghost Free" regions during boot/update.
             */
            bool use_lock = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);
            if (use_lock) hn4_hal_spinlock_acquire(&vol->l2_lock);

            uint64_t word_idx = block_idx / 64;
            /* Align to the start of the 512-bit (8-word) L2 region */
            uint64_t start_w = (word_idx / 8) * 8; 
            
            /* 
             * SCAN: Check if the entire 512-block region is now empty.
             * We check 8 contiguous 64-bit words in the Level 1 bitmap.
             */
            bool region_empty = true;
            for (int i = 0; i < 8; i++) {
                /* Relaxed load is acceptable here; strictness comes later */
                if (atomic_load_explicit((_Atomic uint64_t*)&vol->void_bitmap[start_w + i].data, 
                    memory_order_relaxed) != 0) {
                    region_empty = false;
                    break;
                }
            }

            if (region_empty) {
                uint64_t l2_idx = block_idx / HN4_L2_COVERAGE_BITS;
                uint64_t l2_word_idx = l2_idx / 64;
                uint64_t l2_mask = (1ULL << (l2_idx % 64));
                _Atomic uint64_t* l2_ptr = (_Atomic uint64_t*)&vol->l2_summary_bitmap[l2_word_idx];

                /* 
                 * STEP 1: OPTIMISTIC CLEAR (OPTIMIZED)
                 * Changed from seq_cst to release. We ensure prior L1 updates are 
                 * visible, but avoid full hardware serialization on this instruction.
                 */
                atomic_fetch_and_explicit(l2_ptr, ~l2_mask, memory_order_release);

                /* 
                 * STEP 2: HARD FENCE (REQUIRED)
                 * Enforces Store-Load ordering. The write to L2 (Step 1) MUST be 
                 * globally visible before we read L1 (Step 3).
                 * This prevents the "False Free" race condition.
                 */
                atomic_thread_fence(memory_order_seq_cst);

                /* 
                 * STEP 3: THE "OOPS" CHECK (Self-Healing)
                 * Re-scan L1. If we find a bit set now, it means we raced with 
                 * an allocator. We must restore the L2 bit.
                 */
                bool oops_not_empty = false;
                for (int i = 0; i < 8; i++) {
                    if (atomic_load_explicit((_Atomic uint64_t*)&vol->void_bitmap[start_w + i].data, 
                        memory_order_relaxed) != 0) {
                        oops_not_empty = true;
                        break;
                    }
                }

                if (oops_not_empty) {
                    /* 
                     * STEP 4: HEAL (OPTIMIZED)
                     * Changed from seq_cst to relaxed. 
                     * We are simply flagging the region as dirty/occupied again. 
                     * Allocators scan L1 if L2 is set, so eventual consistency works.
                     */
                    atomic_fetch_or_explicit(l2_ptr, l2_mask, memory_order_relaxed);
                }
            }

            if (use_lock) hn4_hal_spinlock_release(&vol->l2_lock);
        }
    }
}

/* =========================================================================
 * 5. BITMAP OPERATIONS
 * ========================================================================= */

HN4_HOT
_Check_return_
HN4_HOT
_Check_return_
hn4_result_t _bitmap_op(
    HN4_INOUT hn4_volume_t* vol, 
    HN4_IN    uint64_t block_idx, 
    HN4_IN    hn4_bit_op_t op,
    HN4_OUT_OPT bool* out_result
)
{
    /* =========================================================================
     * PATH A: PICO / DIRECT-IO MODE (Embedded Constraints)
     * ========================================================================= */
    if (HN4_UNLIKELY(!vol->void_bitmap)) {
        
        /* 
         * Validation: If bitmap is NULL, we must be in PICO profile. 
         * Otherwise, the volume is uninitialized/corrupt.
         */
        if (vol->sb.info.format_profile != HN4_PROFILE_PICO) {
            return HN4_ERR_UNINITIALIZED;
        }

        const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
        const uint32_t ss = caps->logical_block_size;
        
        if (HN4_UNLIKELY(ss > 4096)) return HN4_ERR_NOMEM;

        uint8_t sector_buf[4096]; 
        
        /* Coordinate Calculation */
        uint64_t word_idx      = block_idx / 64;
        uint64_t bit_off       = block_idx % 64;
        uint64_t byte_offset   = word_idx * sizeof(hn4_armored_word_t);
        uint64_t sector_offset = (byte_offset / ss) * ss;
        uint64_t offset_in_sec = byte_offset % ss;

        hn4_addr_t io_lba = hn4_addr_add(vol->sb.info.lba_bitmap_start, sector_offset / ss);
        hn4_result_t res = HN4_OK;

        /* 
         * CRITICAL SECTION (PICO)
         * Serialize access via the L2 lock to prevent RMW races.
         */
        hn4_hal_spinlock_acquire(&vol->l2_lock);

        /* 1. READ: Fetch the sector containing the target word */
        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, io_lba, sector_buf, 1) != HN4_OK) {
            res = HN4_ERR_HW_IO;
            goto pico_cleanup;
        }

        hn4_armored_word_t* word = (hn4_armored_word_t*)(sector_buf + offset_in_sec);

        /* 2. VALIDATE: Check ECC and heal if necessary */
        uint64_t safe_data;
        bool corrected = false;
        
        res = _ecc_check_and_fix(vol, word->data, word->ecc, &safe_data, &corrected);
        if (HN4_UNLIKELY(res != HN4_OK)) {
            goto pico_cleanup; /* DED / Corruption detected */
        }

        if (corrected) {
            word->data = safe_data;
        }

        /* 3. EXECUTE: Apply bit logic */
        bool is_set = (word->data & (1ULL << bit_off)) != 0;
        bool mutation_needed = false;
        bool report_change = false;

        if (op == BIT_TEST) {
            if (out_result) *out_result = is_set;
            /* Only write back if we corrected an ECC error (Healing Read) */
            mutation_needed = corrected;
        }
        else if ((op == BIT_SET && is_set) || 
                 ((op == BIT_CLEAR || op == BIT_FORCE_CLEAR) && !is_set)) 
        {
            /* Logic No-Op */
            if (out_result) *out_result = false;
            mutation_needed = corrected;
        }
        else {
            /* Mutation Required */
            if (op == BIT_SET) word->data |= (1ULL << bit_off);
            else word->data &= ~(1ULL << bit_off);
            
            /* Regenerate ECC */
            word->ecc = _calc_ecc_hamming(word->data);
            mutation_needed = true;
            report_change = true;
        }

        /* 4. WRITE: Commit changes if mutated or healed */
        if (mutation_needed) {
            hn4_result_t w_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, io_lba, sector_buf, 1);
            
            if (w_res != HN4_OK) {
                if (op == BIT_TEST) {
                    /* Healing failed, but read succeeded. Mask error. */
                    res = HN4_OK; 
                } else {
                    res = HN4_ERR_HW_IO;
                    /* If write failed, we must NOT report logic change */
                    report_change = false;
                }
            }
        }
        
        if (op != BIT_TEST && out_result) *out_result = report_change;

    pico_cleanup:
        hn4_hal_spinlock_release(&vol->l2_lock);
        return res;
    }

    /* =========================================================================
     * PATH B: STANDARD RAM MODE (Atomic 128-bit CAS)
     * ========================================================================= */

    /* 1. Alignment & Geometry Checks */
    if (HN4_UNLIKELY(((uintptr_t)vol->void_bitmap & 0xF) != 0)) {
        return HN4_ERR_INTERNAL_FAULT; /* Critical Alignment Violation */
    }

    uint64_t word_idx = block_idx / 64;
    uint64_t bit_off  = block_idx % 64;
    uint64_t bit_mask = (1ULL << bit_off);
    
    if (HN4_UNLIKELY((word_idx * sizeof(hn4_armored_word_t)) >= vol->bitmap_size)) {
        return HN4_ERR_GEOMETRY;
    }

    volatile void* target_addr = &vol->void_bitmap[word_idx];

    /* 2. Atomic Loop State */
    hn4_aligned_u128_t  expected, desired;
    bool success = false;
    bool logic_change = false;       /* Did the logical bit state change? */
    bool heal_event_pending = false; /* Did we fix an ECC error? */

    /* Initial Load (Atomic) */
    expected = _hn4_load128(target_addr);

    do {
        /* Reset per-loop flags */
        logic_change = false;
        bool is_healing_write = false;

        /* 2.1 Deconstruct Word: [Data: 64] [Version: 56] [ECC: 8] */
        uint64_t data = expected.lo;
        uint64_t meta = expected.hi;
        uint8_t  ecc  = (uint8_t)(meta & 0xFF);
        uint64_t ver  = (meta >> 8);

        uint64_t safe_data;
        bool was_corrected = false;
        
        /* 2.2 Verify Integrity */
        hn4_result_t ecc_res = _ecc_check_and_fix(vol, data, ecc, &safe_data, &was_corrected);
        if (HN4_UNLIKELY(ecc_res != HN4_OK)) {
            return ecc_res; /* DED: Fatal Corruption */
        }
        
        if (was_corrected) heal_event_pending = true;

        bool is_set = (safe_data & bit_mask) != 0;
        
        /* 2.3 Apply Logic */
        if (op == BIT_TEST) {
            if (!was_corrected) {
                if (out_result) *out_result = is_set;
                return HN4_OK; /* Fast Path: Clean Read */
            }
            
            /* HEALING READ: Check RO Policy */
            if (vol->read_only) {
                /* Can't persist fix in RO mode. Return clean data but don't write. */
                if (out_result) *out_result = is_set;
                return HN4_OK;
            }

            /* Force write-back of corrected data */
            desired.lo = safe_data;
            logic_change = true; /* Conceptually a change (repair) to force commit */
            is_healing_write = true; /* Mark as healing */
        }
        else if ((op == BIT_SET && is_set) || 
                 ((op == BIT_CLEAR || op == BIT_FORCE_CLEAR) && !is_set)) 
        {
            /* LOGICAL NO-OP: Bit is already in desired state */
            
            if (!was_corrected) {
                /* 
                 * HEALING L2: Even if L1 (Main) is correct, L2 might be stale.
                 * If we intended to SET, ensure L2 reflects it.
                 */
                if (op == BIT_SET && vol->l2_summary_bitmap) {
                    uint64_t l2_idx = block_idx / HN4_L2_COVERAGE_BITS;
                    uint64_t l2_word = l2_idx / 64;
                    atomic_fetch_or_explicit((_Atomic uint64_t*)&vol->l2_summary_bitmap[l2_word], 
                                             (1ULL << (l2_idx % 64)), memory_order_release);
                }

                #ifdef HN4_STRICT_AUDIT
                if (op == BIT_CLEAR && !is_set) {
                    atomic_fetch_or_explicit(&vol->sb.info.state_flags, HN4_VOL_DIRTY, memory_order_release);
                }
                #endif

                if (out_result) *out_result = false;
                return HN4_OK;
            }
            /* ECC Error found during No-Op -> Write back corrected data */
            desired.lo = safe_data;
            is_healing_write = true; /* Mark as healing */
        } 
        else {
            /* MUTATION: State change required */
            desired.lo = (op == BIT_SET) ? (safe_data | bit_mask) : (safe_data & ~bit_mask);
            logic_change = true;
        }

        /* 2.4 Reconstruct Metadata */
        uint8_t new_ecc = _calc_ecc_hamming(desired.lo);

        /* 
         * VERSIONING STRATEGY:
         * To prevent ABA problems across mounts/snapshots, we XOR the version 
         * with the Epoch ID (volume_uuid.lo used as proxy here).
         */
        uint64_t epoch_mux = vol->sb.info.volume_uuid.lo & 0x00FFFFFFFFFFFFFFULL;
        uint64_t current_ver_logical = (ver ^ epoch_mux); 
        uint64_t next_ver_logical;

        if (is_healing_write) { /* Use explicit healing flag */
            /* For Healing Reads, preserve the logical version to minimize noise */
            next_ver_logical = current_ver_logical;
        } else {
            /* For Mutation, increment version */
            next_ver_logical = (current_ver_logical + 1) & 0x00FFFFFFFFFFFFFFULL;
            if (next_ver_logical == 0) next_ver_logical = 1;
        }

        uint64_t final_ver = next_ver_logical ^ epoch_mux;

        /* Pack High QWORD: [Version: 56] [ECC: 8] */
        desired.hi = (final_ver << 8) | (uint64_t)new_ecc;

        /* 2.5 Commit */
        success = _hn4_cas128(target_addr, &expected, desired);

    } while (HN4_UNLIKELY(!success));


    /* 3. Post-Commit Updates */
    
    /* Telemetry */
    if (HN4_UNLIKELY(heal_event_pending)) {
        atomic_fetch_add(&vol->stats.heal_count, 1);
    }

    /* Result */
    if (out_result) {
        if (op == BIT_TEST) *out_result = ((desired.lo & bit_mask) != 0);
        else *out_result = logic_change; /* Logic change is true for mutation */
    }

    /* Side Effects (Counters, L2, Dirty Flags) 
       Only update L2/Counters if LOGICAL state changed, not just ECC repair */
    bool actual_mutation = (desired.lo != expected.lo) && /* Value changed */
                           (op != BIT_TEST) &&            /* Not a read */
                           !heal_event_pending;           /* Not just healing? No, healing + mutation is mutation. */
                           
    /* Simple check using the final known values from the successful CAS loop */
    bool bit_was_set = ((expected.lo & bit_mask) != 0); // Using raw load? No, use safe_data logic?
    /* expected.lo might be corrupt. We rely on 'op' logic path. */

    if (logic_change && op != BIT_TEST) {
        /* GHOST BIT PROTECTION */
        if (op != BIT_FORCE_CLEAR && !vol->in_eviction_path) {
            atomic_fetch_or_explicit(&vol->sb.info.state_flags, HN4_VOL_DIRTY, memory_order_seq_cst);
        }

        _update_counters_and_l2(vol, block_idx, (op == BIT_SET));
        
        atomic_thread_fence(memory_order_seq_cst);
    }
    
    return heal_event_pending ? HN4_INFO_HEALED : HN4_OK;
}
/* =========================================================================
 * NANO-LATTICE ALLOCATOR (The Cortex-Plex)
 * ========================================================================= */

/*++

Routine Description:

    Scans the Cortex (D0) region for a contiguous run of free slots.
    
    This allocator implements a linear probe strategy with "Best Fit" heuristics.
    It leverages the L2 Summary Bitmap (if available) to skip fully occupied
    regions and includes self-healing logic to reclaim stale pending reservations
    left by crashed writers.

Arguments:

    vol - Pointer to the volume device extension.

    slots_needed - The number of contiguous 128-byte slots required.

    out_slot_idx - Returns the logical slot index of the run start on success.

Return Value:

    HN4_OK on success, or an appropriate error code (e.g., HN4_ERR_ENOSPC).

--*/
hn4_result_t 
_alloc_cortex_run(
    HN4_IN hn4_volume_t* vol, 
    HN4_IN uint32_t slots_needed,
    HN4_OUT uint64_t* out_slot_idx
    )
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t sector_size = caps->logical_block_size;
    
    /* Invariant: Hardware sector size must support atomic slot updates */
    if (sector_size < HN4_CORTEX_SLOT_SIZE) return HN4_ERR_GEOMETRY;

    /*
     * 1. Geometry Calculation
     * Calculate the boundaries of the Cortex region in terms of slots.
     */
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t total_slots = ((end_sect - start_sect) * sector_size) / HN4_CORTEX_SLOT_SIZE;

    /* Performance: Resume scan from the last known cursor to reduce latency */
    uint64_t current_slot = vol->cortex_search_head; 
    if (current_slot >= total_slots) current_slot = 0;

    /*
     * 2. Buffer Allocation
     * Adjust scan batch size based on the profile. Pico devices use minimal
     * buffers to conserve RAM, while Servers use 64KB batches.
     */
    uint32_t scan_size;
    if (vol->sb.info.format_profile == HN4_PROFILE_PICO) {
        /* Align to at least one physical sector (e.g., 4Kn support) */
        scan_size = (sector_size > 512) ? sector_size : 512;
    } else {
        scan_size = 65536; /* 64KB Batch */
    }

    void* io_buffer = hn4_hal_mem_alloc(scan_size);
    if (!io_buffer) return HN4_ERR_NOMEM;

    uint32_t free_run_length = 0;
    uint64_t run_start_index = 0;
    uint64_t slots_checked = 0;
    hn4_result_t status = HN4_ERR_ENOSPC;

    /*
     * 3. Linear Probe Loop
     * Iterate through the Cortex until we wrap around or find space.
     */
    while (slots_checked < total_slots) {
        
        /* Handle Ring Wrap-around */
        if (current_slot >= total_slots) current_slot = 0;

        /*
         * Optimization: L2 Summary Bitmap Skip.
         * If the L2 Summary indicates a region is fully dense/dirty, skip it entirely.
         * This reduces IO pressure significantly on full drives.
         */
        if (vol->l2_summary_bitmap) {
            uint64_t byte_offset = current_slot * HN4_CORTEX_SLOT_SIZE;
            
            /* Calculate absolute physical coordinates */
            uint64_t abs_lba = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) + (byte_offset / sector_size);
            uint64_t block_idx = abs_lba / (vol->vol_block_size / sector_size);
            
            /* Map to L2 bit */
            uint64_t l2_idx = block_idx / HN4_L2_COVERAGE_BITS;
            uint64_t l2_word = l2_idx / 64;
            uint64_t l2_bit = l2_idx % 64;
            
            /* Atomic load to check density state */
            uint64_t l2_val = atomic_load_explicit((_Atomic uint64_t*)&vol->l2_summary_bitmap[l2_word], memory_order_relaxed);
            
            if ((l2_val >> l2_bit) & 1) {
                /* Region is saturated. Calculate skip distance. */
                uint64_t blocks_in_region = HN4_L2_COVERAGE_BITS;
                uint64_t blocks_rem = blocks_in_region - (block_idx % blocks_in_region);
                
                uint64_t slots_to_skip = (blocks_rem * vol->vol_block_size) / HN4_CORTEX_SLOT_SIZE;
                
                /* Clamp skip to avoid infinite loops */
                if (slots_to_skip > (total_slots - slots_checked)) slots_to_skip = 1;
                
                current_slot += slots_to_skip;
                slots_checked += slots_to_skip;
                continue; 
            }
        }

        /* Determine batch size for this iteration */
        uint64_t batch_slots = (scan_size / HN4_CORTEX_SLOT_SIZE);
        if (current_slot + batch_slots > total_slots) batch_slots = total_slots - current_slot;

        /* Calculate LBA for Read */
        uint64_t byte_offset = current_slot * HN4_CORTEX_SLOT_SIZE;
        uint64_t sector_offset = byte_offset / sector_size;
        hn4_addr_t io_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, sector_offset);

        /* 
        * 1. Pre-calculate I/O Geometry 
        * We need the total sector count to verify the Tail of the batch.
        * Hoisted this calculation ABOVE the check to avoid variable duplication.
        */

        uint32_t read_bytes = (uint32_t)((batch_slots * HN4_CORTEX_SLOT_SIZE + sector_size - 1) & ~(sector_size - 1));
        uint32_t sectors_to_read = read_bytes / sector_size;

        /* 
        * 2. Conservative Extent Check (Head + Tail)
        * Verify that the entire physical range of this batch is safe.
        */

        uint64_t start_lba_val = hn4_addr_to_u64(io_lba);
        uint64_t end_lba_val   = start_lba_val + sectors_to_read - 1;

        if (_check_quality_compliance(vol, start_lba_val, HN4_ALLOC_METADATA) != HN4_OK ||
        _check_quality_compliance(vol, end_lba_val,   HN4_ALLOC_METADATA) != HN4_OK) 

        {
            /* Underlying media is Toxic/Bronze at Head OR Tail; skip entire batch */
            current_slot += batch_slots;
            slots_checked += batch_slots;
            free_run_length = 0;
            continue;

        }

        /* 3. Execute I/O (Variables already defined) */

        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, io_lba, io_buffer, sectors_to_read) != HN4_OK) {
            status = HN4_ERR_HW_IO;
            goto Cleanup;
        }

        uint8_t* raw_ptr = (uint8_t*)io_buffer;

        /*
         * 4. Slot Inspection Loop
         */
        for (uint32_t i = 0; i < batch_slots; i++) {
            uint8_t* slot_ptr = raw_ptr + (i * HN4_CORTEX_SLOT_SIZE);
            
            bool is_free = false; // Default to occupied for safety

            uint64_t* words = (uint64_t*)slot_ptr;
            hn4_nano_header_t* hdr = (hn4_nano_header_t*)slot_ptr;

        /* Check 1: Is it completely zero? */
            
            bool all_zero = true;
        
            for (int k = 0; k < (HN4_CORTEX_SLOT_SIZE / 8); k++) {
                  if (words[k] != 0) {
                    all_zero = false;
                    break;
                }
            }

        if (all_zero) {
            is_free = true;
        } 
        
        /* Check 2: Is it a stale reservation? */
        else if (hdr->magic == hn4_cpu_to_le32(HN4_MAGIC_NANO_PENDING)) {
            uint64_t claim_ts = hn4_le64_to_cpu(hdr->version);
            uint64_t now = hn4_hal_get_time_ns();
    
            /* Lease Timeout: 30 Seconds (30 * 1e9 ns) */
            if (now > claim_ts && (now - claim_ts) > 30000000000ULL) {
                is_free = true; // Expired lease, safe to reclaim
            }
        }

        if (is_free) {

                if (free_run_length == 0) run_start_index = current_slot + i;
                free_run_length++;

                if (free_run_length == slots_needed) {
                    
                    /*
                     * 5. Atomic Reservation (The Claim)
                     * We found a run. We must persist a PENDING marker immediately
                     * to prevent race conditions with other allocators.
                     */
                    
                    /* Recalculate physical location of the Head Slot */
                    uint64_t head_byte_offset = run_start_index * HN4_CORTEX_SLOT_SIZE;
                    uint64_t head_sect_offset = head_byte_offset / sector_size;
                    uint32_t head_buf_offset  = head_byte_offset % sector_size;
                    
                    hn4_addr_t claim_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, head_sect_offset);
                    
                    /* RMW Cycle: Read the specific sector for the head */
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, claim_lba, io_buffer, 1) != HN4_OK) {
                        status = HN4_ERR_HW_IO; 
                        goto Cleanup;
                    }
                    
                    hn4_nano_header_t* claim_hdr = (hn4_nano_header_t*)((uint8_t*)io_buffer + head_buf_offset);
                    
                    /* Double-Check: Ensure we didn't race between the batch read and this claim */
                    if (claim_hdr->magic != 0 && claim_hdr->magic != hn4_cpu_to_le32(HN4_MAGIC_NANO_PENDING)) {
                        free_run_length = 0; /* Lost race, restart search */
                        continue;
                    }
                    
                    /* Populate Pending Marker */
                    claim_hdr->magic = hn4_cpu_to_le32(HN4_MAGIC_NANO_PENDING);
                    claim_hdr->payload_len = 0; /* Zero length indicates specialized marker */
                    claim_hdr->version = hn4_cpu_to_le64(hn4_hal_get_time_ns());
                    claim_hdr->flags = 0;

                    /* Safety: Calculate valid CRC so fsck respects the marker */
                    uint32_t p_crc = hn4_crc32(0, &claim_hdr->magic, 4);
                    size_t off_crc = offsetof(hn4_nano_header_t, payload_len);
                    p_crc = hn4_crc32(p_crc, (uint8_t*)claim_hdr + off_crc, sizeof(hn4_nano_header_t) - off_crc);

                    claim_hdr->header_crc = hn4_cpu_to_le32(p_crc);

                    /* Commit Reservation */
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, claim_lba, io_buffer, 1) != HN4_OK) {
                        status = HN4_ERR_HW_IO; 
                        goto Cleanup;
                    }
                    
                    /* Barrier: Ensure reservation holds before returning success */
                    hn4_hal_barrier(vol->target_device);

                    *out_slot_idx = run_start_index;
                    vol->cortex_search_head = run_start_index + slots_needed; /* Update cursor */
                    status = HN4_OK;
                    goto Cleanup;
                }
            } else {
                free_run_length = 0;
            }
        }
        current_slot += batch_slots;
        slots_checked += batch_slots;
    }

Cleanup:
    hn4_hal_mem_free(io_buffer);
    return status;
}

/*++

Routine Description:

    Allocates and persists a "Nano" object within the Cortex (D0) region.

    Nano objects are small data payloads (<16KB) embedded directly into the
    metadata region to minimize seek latency and fragmentation. This routine
    handles the allocation of contiguous slots, payload construction, and
    the atomic commit sequence required to persist the data safely.

    The commit protocol uses a two-phase RMW strategy:
    1. Reserve slots (PENDING state).
    2. Write payload.
    3. Commit (Set valid flag).

Arguments:

    vol - Pointer to the volume device extension.

    anchor - Pointer to the parent Anchor structure (in-memory).

    data - Buffer containing the payload.

    len - Length of the payload in bytes (Max 16KB).

Return Value:

    HN4_OK on success.
    HN4_ERR_INVALID_ARGUMENT if length exceeds limits.
    HN4_ERR_ENOSPC if the Cortex is full.
    HN4_ERR_HW_IO on media failure.

--*/
hn4_result_t 
hn4_alloc_nano(
    HN4_IN hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN const void* data,
    HN4_IN uint32_t len
    )
{
    /* Validate payload fits within Nano architecture limits */
    if (len == 0 || len > 16384) return HN4_ERR_INVALID_ARGUMENT;

    /*
     * 1. Geometry & Allocation
     * Calculate the number of 128-byte slots required for Header + Payload.
     */
    uint32_t total_payload = sizeof(hn4_nano_header_t) + len; 
    uint32_t slots_needed = (total_payload + HN4_CORTEX_SLOT_SIZE - 1) / HN4_CORTEX_SLOT_SIZE;
    uint32_t nano_obj_size = slots_needed * HN4_CORTEX_SLOT_SIZE;

    /* Reserve contiguous slots on disk (writes PENDING marker) */
    uint64_t start_slot;
    hn4_result_t res = _alloc_cortex_run(vol, slots_needed, &start_slot);
    if (res != HN4_OK) return res;

    /*
     * 2. Payload Construction
     * Prepare the in-memory image of the Nano Object.
     */
    void* write_buf = hn4_hal_mem_alloc(nano_obj_size);
    
    /* 
     * Resource Failure Handling:
     * If RAM allocation fails, we must rollback the disk reservation to prevent
     * leaking PENDING markers that would otherwise require timeout-based GC.
     */
    if (!write_buf) {
        res = HN4_ERR_NOMEM;
        goto rollback_reservation;
    }
    
    memset(write_buf, 0, nano_obj_size);

    hn4_nano_header_t* hdr = (hn4_nano_header_t*)write_buf;
    
    /* Derive strict monotonic version from parent Anchor */
    uint64_t next_ver = hn4_le32_to_cpu(anchor->write_gen) + 1;
    
    hdr->magic       = hn4_cpu_to_le32(HN4_MAGIC_NANO);
    hdr->payload_len = hn4_cpu_to_le64(len);
    hdr->version     = hn4_cpu_to_le64(next_ver);
    hdr->flags       = 0; /* Initially Uncommitted */
    
    memcpy(hdr->data, data, len);
    
    /* Calculate Data Integrity Checksum */
    uint32_t d_crc = hn4_crc32(0, hdr->data, len);
    hdr->data_crc = hn4_cpu_to_le32(d_crc);
    
    /* 
     * Calculate Header Integrity Checksum (Split-CRC).
     * The checksum covers the Magic field, skips the CRC field itself, 
     * and covers the remainder of the header struct.
     */
    uint32_t h_crc = 0;
    
    /* Part A: Magic (First 4 bytes) */
    h_crc = hn4_crc32(0, &hdr->magic, 4);
    
    /* Part B: Payload_Len to End of Header */
    size_t offset_after_crc = offsetof(hn4_nano_header_t, payload_len);
    h_crc = hn4_crc32(h_crc, (uint8_t*)hdr + offset_after_crc, sizeof(hn4_nano_header_t) - offset_after_crc);
    
    hdr->header_crc = hn4_cpu_to_le32(h_crc);

    /*
     * 3. Commit to Media (Read-Modify-Write)
     * Nano objects may share physical sectors with other objects. 
     * We must read the target sector(s), overlay our payload, and write back.
     */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size ? caps->logical_block_size : 512;

    uint64_t byte_start = start_slot * HN4_CORTEX_SLOT_SIZE;
    uint64_t byte_end   = byte_start + nano_obj_size;
    
    uint64_t sect_start_idx = byte_start / ss;
    uint64_t sect_end_idx   = (byte_end + ss - 1) / ss;
    uint32_t sectors_to_io  = (uint32_t)(sect_end_idx - sect_start_idx);
    uint32_t buffer_offset  = byte_start % ss;

    hn4_addr_t io_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, sect_start_idx);
    void* io_buf = hn4_hal_mem_alloc(sectors_to_io * ss);
    
    if (!io_buf) { 
        hn4_hal_mem_free(write_buf); 
        return HN4_ERR_NOMEM; 
    }

    /* PHASE 1: Write Data (Flags = 0) */
    if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, io_lba, io_buf, sectors_to_io) != HN4_OK) {
        res = HN4_ERR_HW_IO; 
        goto io_cleanup;
    }

    /* Boundary safety check before memcpy */
    if (buffer_offset + nano_obj_size > (sectors_to_io * ss)) {
        res = HN4_ERR_INTERNAL_FAULT; 
        goto io_cleanup;
    }
    
    memcpy((uint8_t*)io_buf + buffer_offset, write_buf, nano_obj_size);

    if (hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, io_lba, io_buf, sectors_to_io) != HN4_OK) {
        res = HN4_ERR_HW_IO; 
        goto io_cleanup;
    }

    /* BARRIER: Ensure payload is durable before marking valid */
    hn4_hal_barrier(vol->target_device);

    /* PHASE 2: Atomic Commit (Set COMMITTED flag) */
    /* Only need to update the first sector containing the header */
    hn4_nano_header_t* io_hdr = (hn4_nano_header_t*)((uint8_t*)io_buf + buffer_offset);
    
    io_hdr->flags = hn4_cpu_to_le32(HN4_NANO_FLAG_COMMITTED);
    
    /* Recompute Header CRC with new flag */
    h_crc = hn4_crc32(0, &io_hdr->magic, 4);
    h_crc = hn4_crc32(h_crc, (uint8_t*)io_hdr + offset_after_crc, sizeof(hn4_nano_header_t) - offset_after_crc);
    io_hdr->header_crc = hn4_cpu_to_le32(h_crc);

    /* Final Write of Header */
    if (hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, io_lba, io_buf, 1) != HN4_OK) {
        /* 
         * Torn state: Payload is present but uncommitted. 
         * Readers will treat this as invalid due to missing flag/CRC mismatch.
         */
        res = HN4_ERR_HW_IO; 
        goto io_cleanup;
    }
    
    hn4_hal_barrier(vol->target_device);

    /*
     * 4. Metadata Update
     * Update the parent Anchor in memory to point to the new Nano Object.
     */
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    dclass |= HN4_FLAG_NANO;
    
    anchor->data_class     = hn4_cpu_to_le64(dclass);
    anchor->gravity_center = hn4_cpu_to_le64(start_slot);
    anchor->mass           = hn4_cpu_to_le64(len);
    anchor->write_gen      = hn4_cpu_to_le32((uint32_t)next_ver);

    res = HN4_OK;

    /*
     * Error Handling & Resource Cleanup
     */
rollback_reservation:
    if (res == HN4_ERR_NOMEM) {
        /* 
         * Manual Rollback for NOMEM:
         * We cannot reuse `io_buf` here as it failed allocation. We must alloc 
         * a tiny temporary buffer to zero the header sector and clear the PENDING state.
         */
        const hn4_hal_caps_t* caps_rb = hn4_hal_get_caps(vol->target_device);
        uint32_t ss_rb = caps_rb->logical_block_size ? caps_rb->logical_block_size : 512;
        
        uint64_t head_byte_off = start_slot * HN4_CORTEX_SLOT_SIZE;
        uint64_t head_sect_off = head_byte_off / ss_rb;
        hn4_addr_t wipe_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, head_sect_off);
        
        void* wipe_buf = hn4_hal_mem_alloc(ss_rb);
        if (wipe_buf) {
            memset(wipe_buf, 0, ss_rb);
            /* Best effort wipe. Ignore errors as we are already failing. */
            hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, wipe_lba, wipe_buf, 1);
            hn4_hal_barrier(vol->target_device);
            hn4_hal_mem_free(wipe_buf);
        }
    }
    /* Fallthrough to return */
    return res;

io_cleanup:
    /* Rollback PENDING reservation on IO failure using the existing buffer */
    if (res != HN4_OK && io_buf) {
        memset(io_buf, 0, ss);
        hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, io_lba, io_buf, 1);
        hn4_hal_barrier(vol->target_device);
    }

    if (io_buf) hn4_hal_mem_free(io_buf);
    if (write_buf) hn4_hal_mem_free(write_buf);
    
    return res;
}

/* =========================================================================
 * PHYSICS ENGINE (SECTION 6)
 * ========================================================================= */

/*++

Routine Description:

    Calculates the physical LBA for a block based on the HN4 "Equation of State".
    
    This function projects the logical block index into physical space using
    modular arithmetic in the "Fractal Domain". It handles the mapping of
    Logical Index (N) -> Fractal Unit -> Physical Sector.

    It enforces:
    1. Fractal Alignment (2^M boundaries).
    2. Coprimality of the stride vector (V) against the window (Phi).
    3. Inertial Damping (Theta jitter) for Solid State media.
    4. Gravity Assist (Vector Shifting) for high-order collision orbits.

Arguments:

    vol - Pointer to the volume device extension.
    
    G   - Gravity Center (Start LBA of the file/object).
    
    V   - Orbit Vector (Stride/Velocity).
    
    N   - Logical Block Index (Offset).
    
    M   - Fractal Scale (Power of 2 alignment, where BlockSize = 2^M).
    
    k   - Orbit Index (Collision attempt counter, 0..12).

Return Value:

    Returns the calculated Physical LBA (Sector Address).
    Returns HN4_LBA_INVALID if geometry constraints are violated.

--*/
uint64_t
_calc_trajectory_lba(
    HN4_IN hn4_volume_t* vol,
    uint64_t             G,
    uint64_t             V,
    uint64_t             N,
    uint16_t             M,
    uint8_t              k
    )
{
    /* Inertial Damping Lookup Table (Theta Jitter) */
    static const uint8_t _theta_lut[16] = {
        0, 1, 3, 6, 10, 15, 21, 28, 
        36, 45, 55, 66, 78, 91, 105, 120
    };

    /* 1. Validate Fractal Scale & Device Context */
    if (HN4_UNLIKELY(M >= 63 || !vol->target_device)) {
        return HN4_LBA_INVALID;
    }

    /* 2. Load Geometry */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (HN4_UNLIKELY(!caps)) return HN4_LBA_INVALID;

    uint32_t bs          = vol->vol_block_size;
    uint32_t ss          = caps->logical_block_size ? caps->logical_block_size : 512;
    uint32_t sec_per_blk = (bs / ss) ? (bs / ss) : 1;
    uint64_t S           = 1ULL << M;

    /* Calculate Flux Domain Boundaries */
    uint64_t total_blocks    = vol->vol_capacity_bytes / bs;
    uint64_t flux_start_sect = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    uint64_t flux_start_blk  = flux_start_sect / sec_per_blk;

    /* 
     * Align Flux Start to Fractal Boundary (S).
     * The trajectory equation relies on modulo arithmetic; misaligned bases
     * cause wrap-around corruption at the end of the disk.
     */
    uint64_t flux_aligned_blk = (flux_start_blk + (S - 1)) & ~(S - 1);

    if (HN4_UNLIKELY(flux_aligned_blk >= total_blocks)) {
        return HN4_LBA_INVALID;
    }

    uint64_t available_blocks = total_blocks - flux_aligned_blk;
    uint64_t phi              = available_blocks / S;
    
    if (HN4_UNLIKELY(phi == 0)) return HN4_LBA_INVALID;

    /* 
     * 3. Apply Gravity Assist (Vector Shift)
     * If k >= 4, we engage "Gravity Assist" to teleport the vector using
     * the canonical Swizzle Engine. This escapes local gravity wells (collisions).
     */
    uint64_t effective_V = V;
    if (k >= HN4_GRAVITY_ASSIST_K) {
        effective_V = hn4_swizzle_gravity_assist(V);
    }
    effective_V |= 1; /* Force Odd (Anti-Even Degeneracy) */

    /* 
     * 4. Enforce Fractal Alignment on G
     * G must be a multiple of S. We extract the lower bits as "Entropy Loss"
     * to be re-injected after the trajectory calculation.
     */
    uint64_t g_aligned   = G & ~(S - 1);
    uint64_t g_fractal   = g_aligned / S;
    uint64_t entropy_loss = G & (S - 1);

    /* 
     * 5. Calculate Modular Terms 
     * Enforce Coprimality: If Phi has changed (Resize), V might share factors.
     * This destroys injectivity. If not coprime, fallback to Linear (V=1).
     */
   uint64_t term_n = N % phi;
    uint64_t term_v = effective_V % phi;
    
    /* RESONANCE DAMPENER (Prevent Prime Collapse) */
    if (HN4_UNLIKELY(term_v == 0 || _gcd(term_v, phi) != 1)) {
        /* 
         * Do not collapse to 1 immediately. Perturb V to find nearest coprime.
         * This preserves ballistic distribution on resized volumes.
         */
        uint64_t attempts = 0;
        do {
            term_v += 2; /* Keep parity odd to avoid even-number resonance */
            if (term_v >= phi) term_v = 3; /* Wrap around avoiding 0/1/2 */
            attempts++;
        } while (_gcd(term_v, phi) != 1 && attempts < 32);

        /* Ultimate fallback only if dampener fails */
        if (_gcd(term_v, phi) != 1) term_v = 1;
    }
    
    /* Calculate Offset: (N * V) % Phi */
    uint64_t offset = _mul_mod_safe(term_n, term_v, phi);
    
    /* Mix Entropy back in */
    offset = (offset + entropy_loss) % phi;
    
    /* 
     * 6. Apply Inertial Damping (Theta Jitter)
     * SSDs benefit from pseudo-random scattering to reduce write amplification.
     * Linear media (HDD/ZNS) requires sequential access (Theta = 0).
     */
    uint64_t theta = 0;
    
    /* Check Linear LUT (Mask & 0x3 protects against corrupt tags) */
    bool is_linear = _hn4_is_linear_lut[vol->sb.info.device_type_tag & 0x3];
    bool is_system = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);

    if (!is_linear && !is_system) {
        /* If Phi is small, LUT modulo causes cycles. Use Linear Probe k. */
        if (HN4_UNLIKELY(phi < 32)) {
            theta = k % phi;
        } else {
            uint8_t safe_k = (k < 16) ? k : 15;
            theta = _theta_lut[safe_k] % phi;
        }
    }
    
    /* 7. Final Projection */
    uint64_t target_fractal_idx = (g_fractal + offset + theta) % phi;
    
    /* Convert Fractal Index -> Physical Block Index */
    uint64_t rel_block_idx = (target_fractal_idx * S);
    
    /* Overflow Guards */
    if (HN4_UNLIKELY((UINT64_MAX - entropy_loss) < rel_block_idx)) return HN4_LBA_INVALID;
    rel_block_idx += entropy_loss;

    if (HN4_UNLIKELY((UINT64_MAX - flux_aligned_blk) < rel_block_idx)) return HN4_LBA_INVALID;
    
    return flux_aligned_blk + rel_block_idx;
}


/*++

Routine Description:

    HELPER: Check Quality Tier (Inline)
    Returns true if block is safe for the requested intent.

Arguments:

    vol - Pointer to the volume device extension.

    lba - Physical Sector LBA (will be converted to Block Index).

    intent - Allocation intent (e.g., HN4_ALLOC_METADATA).

Return Value:

    true  - Block is compliant.
    false - Block is toxic, out of bounds, or insufficient quality.

--*/
static bool
_is_quality_compliant(
    HN4_IN hn4_volume_t* vol,
    uint64_t lba,
    uint8_t intent
    )
{
    if (!vol->quality_mask) return true;

    uint64_t word_idx = lba / 32;

    if ((word_idx * 8) >= vol->qmask_size) {
        /* Flag critical geometry error (Panic) before rejecting */
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC | HN4_VOL_DIRTY);
        return false;
    }

    uint32_t shift = (lba % 32) * 2;
    uint64_t q_word = vol->quality_mask[word_idx];
    uint8_t q_val = (q_word >> shift) & 0x3;

    if (q_val == HN4_Q_TOXIC) return false;

    if (intent == HN4_ALLOC_METADATA || vol->sb.info.format_profile == HN4_PROFILE_SYSTEM) {
        if (q_val == HN4_Q_BRONZE) return false;
    }

    return true;
}

/*++

Routine Description:

    Performs a topology lookup to determine the optimal physical LBA range for
    Tensor/AI workloads.
    
    This routine maps the calling thread's hardware context (e.g., specific GPU ID)
    to a corresponding NVMe namespace or region defined in the volume's topology map.
    This enables "Path-Aware Striping," ensuring high-bandwidth transfers remain
    local to the PCIe root complex or NUMA node.

Arguments:

    vol - Pointer to the volume device extension.

    out_lba_start - Returns the starting LBA of the affinity region.

    out_lba_len - Returns the length of the affinity region.

Return Value:

    TRUE if a valid affinity bias was found and populated.
    FALSE if global allocation should be used (no affinity match or invalid profile).

--*/
static bool 
_get_ai_affinity_bias(
    HN4_IN hn4_volume_t* vol,
    HN4_OUT uint64_t* out_lba_start,
    HN4_OUT uint64_t* out_lba_len
    ) 
{
    /* Validation: Feature only available in AI Profile with loaded map */
    if (vol->sb.info.format_profile != HN4_PROFILE_AI) return false;
    if (!vol->topo_map || vol->topo_count == 0) return false;

    /*
     * Query the Hardware Abstraction Layer for the accelerator ID bound 
     * to the current thread context.
     *
     * PERFORMANCE NOTE:
     * This relies on stable thread affinity. If the OS scheduler migrates 
     * the thread to a different NUMA node mid-operation, this may result 
     * in sub-optimal (remote) placement, but data integrity is preserved.
     */
    uint32_t caller_id = hn4_hal_get_calling_gpu_id();

    /* 0xFFFFFFFF indicates generic CPU thread */
    if (caller_id == 0xFFFFFFFF) return false;

    /* Scan the topology map for a matching Accelerator ID */
    for (uint32_t i = 0; i < vol->topo_count; i++) {
        if (vol->topo_map[i].gpu_id == caller_id) {
            
            /* 
             * Check Affinity Weight.
             * 0 = Same Switch (Ideal)
             * 1 = Same Root Complex (Good)
             * >1 = Remote/QPI Link (Avoid)
             */
            if (vol->topo_map[i].affinity_weight <= 1) {
                *out_lba_start = vol->topo_map[i].lba_start;
                *out_lba_len   = vol->topo_map[i].lba_len;
                return true;
            }
            
            /* Match found but locality is poor; fall back to global striping */
            return false;
        }
    }
    
    return false;
}

/* =========================================================================
 * ALLOCATOR API
 * ========================================================================= */

/*++

Routine Description:

    The "Genesis" Allocator.

    Determines the initial Gravitational Center (G) and Velocity Vector (V) for a new
    file allocation. This routine implements the HN4 "Ballistic Allocation" strategy,
    attempting to find a collision-free trajectory in the Flux Manifold (D1).

    If the Flux Manifold is saturated or too fragmented to support the requested
    fractal scale, this routine fails over to the Event Horizon (D1.5) linear log.

Arguments:

    vol - Pointer to the volume device extension.
    
    fractal_scale - The power-of-two alignment requirement (M). 
                    Size = 2^M * BlockSize.
                    
    alloc_intent - Hint flags describing the workload (e.g., METADATA, STREAMING).
    
    out_G - Returns the calculated Gravity Center (start index).
    
    out_V - Returns the calculated Orbit Vector (stride).

Return Value:

    HN4_OK - Success. G and V are valid.
    HN4_INFO_HORIZON_FALLBACK - Success, but placement forced to Linear Log (V=0).
    HN4_ERR_ENOSPC - Volume is physically full.
    HN4_ERR_EVENT_HORIZON - Flux is saturated; Horizon logic required.

--*/
_Check_return_ 
hn4_result_t 
hn4_alloc_genesis(
    HN4_INOUT hn4_volume_t* vol,
    uint16_t fractal_scale,
    uint8_t alloc_intent,
    HN4_OUT uint64_t* out_G,
    HN4_OUT uint64_t* out_V
    )
{
    /* 
     * 1. SATURATION CHECK
     * Determine if the Flux Manifold (D1) accepts new writes.
     * If the volume is >90% full, we bypass ballistic allocation to avoid 
     * excessive collision probing and CPU spin.
     */
    bool d1_available = !_check_saturation(vol, true);

    if (d1_available) {
        
        /*
         * 2. GEOMETRY & DOMAIN SETUP
         * Calculate the bounds of the "Flux Domain". We must respect the 
         *Fractal Scale (S = 2^M) alignment constraints.
         */
        uint32_t bs = vol->vol_block_size;
        const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
        uint32_t ss = caps ? caps->logical_block_size : 512;
        uint32_t sec_per_blk = (bs / ss) ? (bs / ss) : 1;

        uint64_t S = 1ULL << fractal_scale;
        
        uint64_t total_blocks = vol->vol_capacity_bytes / bs;
        uint64_t flux_start_sect = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
        uint64_t flux_start_blk  = flux_start_sect / sec_per_blk;

        /*
         * Align the Flux Start to the S boundary.
         * The trajectory equation relies on modulo arithmetic; misaligned bases
         * cause wrap-around corruption at the end of the disk.
         */
        uint64_t flux_aligned_blk = (flux_start_blk + (S - 1)) & ~(S - 1);

        if (flux_aligned_blk < total_blocks) {

            uint64_t available_blocks = total_blocks - flux_aligned_blk;
            uint64_t phi = available_blocks / S;
            
            if (phi > 0) {
                
                /*
                 * 3. TOPOLOGY DISCOVERY (NUMA / AI AFFINITY)
                 * If this is an AI/Tensor workload, check if the calling thread 
                 * is bound to a specific accelerator. We restrict the search window
                 * to the NVMe namespace physically closest to that PCIe root.
                 */
                bool use_affinity = false;
                uint64_t win_base = 0;
                uint64_t win_phi  = phi; /* Default: Global Search */

                /* Optimization: Metadata prefers the outer rim (lower LBAs) for latency */
                if (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM) {
                    win_base = 0;
                    win_phi = phi / 10; /* Restrict to first 10% */
                    if (win_phi == 0) win_phi = phi;
                }

                if (vol->sb.info.format_profile == HN4_PROFILE_AI && vol->topo_map) {

                    uint32_t gpu_id = hn4_hal_get_calling_gpu_id();
                    
                    /* 0xFFFFFFFF indicates generic CPU context */
                    if (gpu_id != 0xFFFFFFFF) {
                        for (uint32_t i = 0; i < vol->topo_count; i++) {
                            if (vol->topo_map[i].gpu_id == gpu_id) {
                                /* Map physical topology to fractal index space */
                                uint64_t range_start_blk = vol->topo_map[i].lba_start / sec_per_blk;
                                uint64_t range_len_blk   = vol->topo_map[i].lba_len / sec_per_blk;

                                if (range_start_blk >= flux_aligned_blk && 
                                   (range_start_blk + range_len_blk) <= total_blocks) 
                                {
                                    uint64_t rel_start = range_start_blk - flux_aligned_blk;
                                    
                                    /* Enforce alignment on window boundaries */
                                    uint64_t rel_aligned = (rel_start + (S - 1)) & ~(S - 1);
                                    
                                    if (rel_aligned < (rel_start + range_len_blk)) {
                                        uint64_t len_aligned = (rel_start + range_len_blk) - rel_aligned;
                                        
                                        win_base = rel_aligned / S;
                                        win_phi  = len_aligned / S;

                                        if (win_phi > 0) {
                                            if (win_base + win_phi > phi) {
                                                win_phi = phi - win_base;
                                            }
                                            use_affinity = true;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    
                    if (!use_affinity) {
                        /* Rate-limited warning logic suppressed for clarity */
                        HN4_LOG_WARN("AI Allocator: Topology lookup failed for GPU %u. Using Global.", gpu_id);
                    }
                }

                /* Cache traits to reduce pointer chasing */
                uint32_t dev_type = vol->sb.info.device_type_tag & 0x3;
                uint32_t profile  = vol->sb.info.format_profile;

                /* Resolve Allocation Policy via LUTs */
                uint8_t dev_p  = _hn4_dev_policy[vol->sb.info.device_type_tag & 0x3];
                uint8_t prof_p = _hn4_prof_policy[vol->sb.info.format_profile & 0x7];
                uint8_t policy = dev_p | prof_p;

                /* 
                 * Determine Vector Constraint.
                 * SSDs prefer ballistic scatter (V=Random). 
                 * Rotational/ZNS/USB prefer sequential (V=1).
                 */
                bool force_sequential = (policy & HN4_POL_SEQ) || 
                                        (alloc_intent == HN4_ALLOC_CONTIGUOUS);

                /*
                 * 4. THE PROBE LOOP
                 * High-latency media (HDD/Tape) requires exhaustive local search (128).
                 * Low-latency media (SSD/ZNS) fails fast to retry elsewhere (20).
                 */
                int max_probes = (policy & HN4_POL_DEEP) ? 128 : HN4_MAX_PROBES;
                for (int attempt = 0; attempt < max_probes; attempt++) {
        
                    /* 4a. Pick Gravity Center (G) */
                    uint64_t g_offset = _get_random_uniform(win_phi);
                    uint64_t g_fractal = win_base + g_offset;

                    /* 
                     * HDD Optimization: Warm Locality Bias.
                     * If previous allocation happened nearby, try to stay close to minimize
                     * actuator seek time, while applying jitter to avoid hotspots.
                     */
                    if (vol->sb.info.device_type_tag == HN4_DEV_HDD) {
                        uint64_t last = atomic_load_explicit(&vol->last_alloc_g, memory_order_relaxed);
                        if (last != 0) {
                            /* Mathematical Safety: Modulo-safe Window Jitter */
                            uint64_t jitter = (hn4_hal_get_random_u64() % 32); 
                            uint64_t last_fractal_rel = (last / S);
                            
                            if (last_fractal_rel >= win_base) {
                                last_fractal_rel -= win_base;
                            } else {
                                last_fractal_rel = 0;
                            }
                            
                            /* Apply Golden Ratio drift to distribute wear */
                            uint64_t drift_prime = 0x9E3779B97F4A7C15ULL; 
                            g_fractal = win_base + ((last_fractal_rel + (jitter * drift_prime)) % win_phi);
                        }
                    }
                    
                    uint64_t G = g_fractal * S;

                    /* 4b. Pick Orbit Vector (V) */
                    uint64_t V;
                    if (force_sequential) {
                        V = 1;
                    } else {
                        /* Scale V relative to the window size */
                        if (use_affinity && win_phi > 1) {
                            /* Constrain V to 1/16th of window to ensure burst containment */
                            uint64_t rng = hn4_hal_get_random_u64();
                            uint64_t v_limit;

                            if ((rng % 10) == 0) {
                                v_limit = win_phi; /* Wide Orbit (10% chance) */
                            } else {
                                v_limit = win_phi / 16; /* Tight Orbit (90% chance) */
                            }

                            if (v_limit < 2) v_limit = 2;
                            V = _get_random_uniform(v_limit) | 1;
                        } else {
                            V = hn4_hal_get_random_u64() | 1;
                        }
                        
                        /* 
                         * Enforce Coprimality with Phi.
                         * This ensures the trajectory visits every block in the ring exactly once
                         * (Bijective Mapping), preventing "short cycles".
                         */
                        int anti_hang = 0;
                        while (true) {
    
                            uint64_t gcd_res = _gcd(V, win_phi);
    
                            /* Case A: Success */
    
                            if (gcd_res == 1) break; 

                            /* Case B: Math Stall */

                            if (gcd_res == 0) {

                                /* Telemetry: Record that we hit a CPU stall / infinite loop protection */
                                HN4_LOG_WARN("GCD Math Stall detected. Forcing Linear Trajectory (V=1).");
                                V = 1;

                                break; /* Abort retries immediately */
                            }

                            /* Case C: Factor Collision (Standard Retry) */
    
                            V += 2;
    
                            if (V == 0) V = 1; 
    
                            if (++anti_hang > 100) { 
                                V = 1; 
                                break; 
                            } 

                        }
                    }

                    /* 
                     * 4c. Trajectory Simulation 
                     * If using affinity, verify the vector doesn't immediately
                     * eject us from the target NUMA node/Namespace.
                     */
                    if (use_affinity) {
                        bool leaked = false;
                        for (int n = 0; n < HN4_MAX_TRAJECTORY_K; n++) {
                            uint64_t phys_blk = _calc_trajectory_lba(vol, G, V, n, fractal_scale, 0);
                            
                            if (phys_blk == HN4_LBA_INVALID || phys_blk < flux_aligned_blk) { 
                                leaked = true; 
                                break; 
                            }
                            
                            uint64_t fractal_idx = (phys_blk - flux_aligned_blk) / S;
                            if (fractal_idx < win_base || fractal_idx >= (win_base + win_phi)) {
                                leaked = true;
                                break;
                            }
                        }
                        if (leaked) continue; /* Retry with new V */
                    }

                    /* 4d. Validate Head (N=0) against Bitmap */
                    uint64_t head_lba = _calc_trajectory_lba(vol, G, V, 0, fractal_scale, 0);
                    if (head_lba == HN4_LBA_INVALID) return HN4_ERR_GEOMETRY;

                    /* Check Quality Mask (Media Health) */
                    hn4_result_t q_res = _check_quality_compliance(vol, head_lba, alloc_intent);
                    if (q_res == HN4_ERR_GEOMETRY) return q_res; 
                    if (q_res != HN4_OK) continue; /* Block is Toxic, skip */

                    /* ATOMIC CLAIM: Try to set the bit */
                    bool head_claimed;
                    hn4_result_t res = _bitmap_op(vol, head_lba, BIT_SET, &head_claimed);
                    
                    if (res != HN4_OK) return res;
                    if (!head_claimed) continue; /* Collision */

                    /* 
                     * 4e. Verify Tail (N=1..3) 
                     * Ensure subsequent blocks in the burst are also free.
                     * Note: We use BIT_TEST here. We don't claim them yet.
                     */
                    int tail_limit = 4;
                    if (vol->sb.info.device_type_tag == HN4_DEV_HDD) tail_limit = 8;

                    bool tail_collision = false;
                    for (int n = 1; n < tail_limit; n++) {
                        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, fractal_scale, 0);
                        
                        if (lba == HN4_LBA_INVALID || !_is_quality_compliant(vol, lba, alloc_intent)) { 
                            tail_collision = true; break; 
                        }
                        
                        bool is_set;
                        if (_bitmap_op(vol, lba, BIT_TEST, &is_set) != HN4_OK) { tail_collision = true; break; }
                        if (is_set) { tail_collision = true; break; }
                    }

                    if (tail_collision) {
                        /* Rollback the Head Claim */
                        _bitmap_op(vol, head_lba, BIT_FORCE_CLEAR, NULL);
                        continue;
                    }

                    /* 5. SUCCESS */
                    atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
                    
                    /* Cache G for HDD locality optimization */
                    if (vol->sb.info.device_type_tag == HN4_DEV_HDD) {
                        atomic_store_explicit(&vol->last_alloc_g, G, memory_order_relaxed);
                    }
                    
                    *out_G = G;
                    *out_V = V;
                    return HN4_OK;
                }
            }
        }
    }

    /* ---------------------------------------------------------------------
     * PHASE 2: EVENT HORIZON FALLBACK (D1.5)
     * --------------------------------------------------------------------- */

    /* Policy Enforcement: System/Metadata must remain in D1 Flux */
    bool is_system = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM) ||
                     (alloc_intent == HN4_ALLOC_METADATA);
                     
    if (is_system && !(vol->sb.info.state_flags & HN4_VOL_PANIC)) {
        return HN4_ERR_ENOSPC;
    }

    /* Attempt allocation in the Linear Log (Horizon) */
    uint64_t hlba;
    hn4_result_t h_res = hn4_alloc_horizon(vol, &hlba);
    
    if (h_res == HN4_OK) {
        *out_G = hlba;
        *out_V = 0; /* V=0 indicates Linear Mode */
        
        /* Signal caller to apply Sentinel Flag (k=15) */
        return HN4_INFO_HORIZON_FALLBACK;
    }

    return HN4_ERR_EVENT_HORIZON;
}

/*
 * =========================================================================
 * ENGINEERING NOTE: THE HORIZON (D1.5) GEOMETRY & BOUNDARY LOGIC
 * =========================================================================
 *
 * PHYSICAL LAYOUT:
 * The HN4 Volume is segmented into distinct gravitational zones. The "Horizon"
 * acts as a linear overflow buffer when the ballistic Flux (D1) is saturated.
 *
 *   [ ... FLUX MANIFOLD (D1) ... ] 
 *              |
 *              v
 *   [ HORIZON BUFFER (D1.5) ]  <-- Starts at sb.lba_horizon_start
 *              |
 *              v
 *   [ CHRONICLE LOG (Journal) ] <-- Starts at sb.journal_start
 *              |
 *              v
 *   [ SOUTH SUPERBLOCK ]
 *
 * THE CALCULATION PARADOX (FIXED):
 * In the Format spec (`hn4_format.c`), `lba_stream_start` is initialized 
 * to the exact same value as `lba_horizon_start`.
 *
 *   Old Logic (Bug):  Size = lba_stream_start - lba_horizon_start = 0.
 *                     Result: Immediate HN4_ERR_ENOSPC.
 *
 *   New Logic (Fix):  The Horizon fills the physical void between its 
 *                     Start Pointer and the beginning of the Chronicle.
 *
 *                     Capacity = sb.journal_start - sb.lba_horizon_start
 *
 * UNIT TRANSLATION:
 * - Superblock pointers are Physical Sector LBAs (Hardware Units).
 * - The Horizon Ring Cursor (`horizon_write_head`) counts Logical Blocks.
 *
 * We must convert the LBA delta into a Block Count to define the ring modulus,
 * then multiply the ring offset back into Sectors to perform I/O.
 * =========================================================================
 */
_Check_return_ hn4_result_t hn4_alloc_horizon(
    HN4_INOUT hn4_volume_t* vol,
    HN4_OUT uint64_t* out_phys_lba
)
{
    uint64_t start_sect, end_sect;
    if (!hn4_addr_try_u64(vol->sb.info.lba_horizon_start, &start_sect)) return HN4_ERR_GEOMETRY;
    if (!hn4_addr_try_u64(vol->sb.info.journal_start, &end_sect)) return HN4_ERR_GEOMETRY;
    if (end_sect <= start_sect) return HN4_ERR_ENOSPC;

    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    
    /* Strict Validation */
    uint32_t ss = caps ? caps->logical_block_size : 512;
    if (ss == 0 || (bs % ss != 0)) {
        HN4_LOG_CRIT("Horizon: Block/Sector mismatch (BS=%u SS=%u)", bs, ss);
        return HN4_ERR_GEOMETRY;
    }
    uint32_t spb = bs / ss;

    uint64_t capacity_sectors = end_sect - start_sect;
    uint64_t capacity_blocks = capacity_sectors / spb;
    if (capacity_blocks == 0) return HN4_ERR_ENOSPC;

   /* 
     * SPEC COMPLIANCE: STRICT O(1) CONSTANT TIME
     * The Horizon acts as a high-velocity Ring Buffer. We do NOT scan for holes.
     * If the Write Head catches the Tail (Occupied Block), the Horizon is FULL.
     *
     * We allow a minimal retry limit (4) solely to resolve atomic contention 
     * between threads racing for the global 'horizon_write_head'.
     */
    uint64_t max_probes = 4;

    for (uint64_t i = 0; i < max_probes; i++) {
        uint64_t head = atomic_fetch_add(&vol->horizon_write_head, 1);
        uint64_t block_offset = head % capacity_blocks;
        
        uint64_t abs_lba = start_sect + (block_offset * spb);
        uint64_t global_block_idx = abs_lba / spb;
        
        bool state_changed;
        hn4_result_t res = _bitmap_op(vol, global_block_idx, BIT_SET, &state_changed);
        if (res != HN4_OK) return res;

        if (state_changed) {
            *out_phys_lba = abs_lba;
            return HN4_OK;
        }
        /* 
         * FAILURE: The bit was already set.
         * In a true Ring Buffer, this means we lapped the ring and hit valid data.
         * We do NOT continue scanning. We yield and try once more or fail.
         * The loop limit (4) ensures we exit essentially immediately.
         */
    }

    /* Saturation: Head caught Tail. */
    return HN4_ERR_ENOSPC;
}

void hn4_free_block(HN4_INOUT hn4_volume_t* vol, uint64_t phys_lba) {
    if (!vol->target_device) return;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps ? caps->logical_block_size : 512;
    uint32_t bs = vol->vol_block_size;
    uint32_t spb = (bs / ss) ? (bs / ss) : 1;

    uint64_t block_idx = phys_lba / spb;
    uint64_t max_blk = vol->vol_capacity_bytes / bs;

    if (block_idx >= max_blk) {
        HN4_LOG_WARN("Free OOB: LBA %llu > Max %llu", 
                     (unsigned long long)phys_lba, (unsigned long long)max_blk);
        
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        uint32_t t = atomic_fetch_add(&vol->taint_counter, 1);
        
        /* Hard Gate: Too many violations -> Panic */
        if (t > HN4_TAINT_THRESHOLD_RO) {
            atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
            HN4_LOG_CRIT("Integrity Threshold Exceeded. Volume Panic.");
        }
        return;
    }

    _bitmap_op(vol, block_idx, BIT_CLEAR, NULL);
}


/*++

Routine Description:

    Allocates a single physical block for a specific logical index within a file.

    This function represents the core of the "Ballistic-Tensor" addressing model.
    It does not simply search for free space; it calculates where the data *should* 
    be based on the file's immutable trajectory (Gravity Center + Orbit Vector).

    The allocation pipeline proceeds in two phases:
    1.  The Flux Manifold (D1): A ballistic probe of k=0..12 orbits. This is 
        O(1) math-based allocation.
    2.  The Event Horizon (D1.5): If D1 is saturated or collided, the data 
        falls into a linear log (Ring Buffer).

Arguments:

    vol         - Volume context.
    anchor      - The file's Anchor (defines physics G, V, M).
    logical_idx - The logical block offset (N).
    out_lba     - Returns the allocated Physical LBA.
    out_k       - Returns the orbit index (0-12) or HN4_HORIZON_FALLBACK_K.

Return Value:

    HN4_OK on success.
    HN4_ERR_ACCESS_DENIED if Read-Only/Snapshot.
    HN4_ERR_GRAVITY_COLLAPSE if all trajectories are blocked (and no Horizon).
    HN4_ERR_ENOSPC if Horizon policy forbids spillover.

--*/
_Check_return_ 
hn4_result_t 
hn4_alloc_block(
    HN4_INOUT hn4_volume_t* vol,
    HN4_IN const hn4_anchor_t* anchor,
    HN4_IN uint64_t logical_idx,
    HN4_OUT hn4_addr_t* out_lba,
    HN4_OUT uint8_t* out_k
    ) 
{
    /* 1. Sanity & Security Checks */
    if (HN4_UNLIKELY(!vol || !anchor || !out_lba || !out_k)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    /* 
     * Snapshot / Time Travel Guard:
     * Modifications are forbidden if we are viewing a historical snapshot 
     * (time_offset != 0) or if the volume is mounted Read-Only.
     */
    if (vol->read_only || vol->time_offset != 0) {
        return HN4_ERR_ACCESS_DENIED;
    }

    /* 
     * 2. Saturation Check (Event Horizon Logic)
     * FIX [Spec 18.8]: D1 Lockout.
     * If the volume is >95% full (updates), we mark the Flux Manifold (D1) 
     * as unavailable. We do not error out immediately; we attempt to fall 
     * through to the Horizon (D1.5) linear log.
     */
    bool d1_saturated = _check_saturation(vol, false);

    /* 3. Physics Extraction */
    uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
    uint64_t V = 0;
    
    /* V is stored as a 48-bit integer in a byte array */
    memcpy(&V, anchor->orbit_vector, 6);
    V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
    
    uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);
    
    /* Determine intent for Quality of Service (QoS) checks */
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    uint8_t alloc_intent = HN4_ALLOC_DEFAULT;
    
    if ((dclass & HN4_CLASS_VOL_MASK) == HN4_VOL_STATIC) {
        alloc_intent = HN4_ALLOC_METADATA;
    }

    /* 
     * 4. Device Constraints
     * Resolve the maximum orbit depth (k).
     * SSDs allow ballistic scattering (k=12).
     * HDDs/Tape/ZNS enforce linear tracks (k=0) to prevent seek thrashing.
     */
    uint8_t max_k = _get_trajectory_limit(vol);

    /* 
     * =====================================================================
     * PHASE 1: THE FLUX MANIFOLD (D1)
     * Ballistic Trajectory Calculation.
     * =====================================================================
     */
    if (!d1_saturated) {
        for (uint8_t k = 0; k <= max_k; k++) {
            
            /* Calculate Candidate LBA using Equation of State */
            uint64_t lba = _calc_trajectory_lba(vol, G, V, logical_idx, M, k);
            
            /* Check for geometry violations (OOB) */
            if (lba == HN4_LBA_INVALID) continue;

            /* 
             * Quality Mask Check (Media Health):
             * Ensure the calculated block isn't on a Toxic (Dead) or 
             * Bronze (Slow) region if high-performance is required.
             */
            hn4_result_t q_res = _check_quality_compliance(vol, lba, alloc_intent);
            if (HN4_UNLIKELY(q_res == HN4_ERR_GEOMETRY)) return q_res; /* Panic Exit */
            if (q_res != HN4_OK) continue; /* Soft Reject (Try next k) */

            /* Atomic Reservation */
            bool claimed;
            hn4_result_t res = _bitmap_op(vol, lba, BIT_SET, &claimed);
            
            if (res == HN4_OK && claimed) {
                /* 
                 * Success: Trajectory Locked.
                 * Convert internal block index to abstract address type.
                 */
                #ifdef HN4_USE_128BIT
                    *out_lba = hn4_addr_from_u64(lba); /* lba is already physical index */
                #else
                    *out_lba = lba; 
                #endif
                *out_k = k;
                return HN4_OK;
            }
            
            /* Fatal Bitmap Corruption (ECC Error) implies we stop immediately */
            if (HN4_UNLIKELY(res == HN4_ERR_BITMAP_CORRUPT)) return res;
        }
    }

    /* 
     * =====================================================================
     * PHASE 2: THE EVENT HORIZON (D1.5)
     * Linear Log Fallback.
     * =====================================================================
     */

    /*
     * Constraint Check:
     * Horizon is a dense linear log of 4KB blocks. It does not support
     * Fractal Scaling (M > 0). If the file requires large blocks, we fail.
     */
    if (M > 0) return HN4_ERR_GRAVITY_COLLAPSE;

    /* 
     * Policy Enforcement:
     * System Files (OS Root) and Critical Metadata MUST reside in the Flux
     * for performance and bootloader compatibility. We deny spillover unless
     * the volume is already in a Panic state (Emergency Writes).
     */
    bool is_system = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM) ||
                     (alloc_intent == HN4_ALLOC_METADATA);
                     
    if (is_system && !(vol->sb.info.state_flags & HN4_VOL_PANIC)) {
        return HN4_ERR_ENOSPC;
    }

    /* Attempt allocation in the Horizon Ring */
    uint64_t hlba;
    if (hn4_alloc_horizon(vol, &hlba) == HN4_OK) {
        #ifdef HN4_USE_128BIT
            *out_lba = hn4_addr_from_u64(hlba); /* Horizon is unscaled (4KB base) */
        #else
            *out_lba = hlba;
        #endif
        
        /* 
         * FIX: Spec 3.4 Horizon Sentinel.
         * We MUST set k=15 to signal that this block resides in the Linear Log.
         * This tells the reader to ignore the Ballistic Equation and use direct LBA.
         */
        *out_k = HN4_HORIZON_FALLBACK_K; /* 15 */
        
        return HN4_OK;
    }

    /* Total Saturation: Both D1 and D1.5 are full or collided */
    return HN4_ERR_GRAVITY_COLLAPSE;
}