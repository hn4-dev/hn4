/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Void Engine (Allocator)
 * SOURCE:      hn4_allocator.c
 * STATUS:      PRODUCTION (v18.1)
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
    [HN4_PROFILE_HYPER_CLOUD] = HN4_POL_DEEP
};

/* =========================================================================
 * 1. HARDENED ATOMICS (NO EXTERNAL LIBS)
 * ========================================================================= */

HN4_INLINE bool
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
    if (_hn4_cpu_features & HN4_CPU_ARM_LSE) {
        /* Capture original values for comparison */
        uint64_t old_exp_lo = expected->lo;
        uint64_t old_exp_hi = expected->hi;

        register uint64_t x0_lo asm("x0") = old_exp_lo;
        register uint64_t x1_hi asm("x1") = old_exp_hi;
        register uint64_t x2_lo asm("x2") = desired.lo;
        register uint64_t x3_hi asm("x3") = desired.hi;

        __asm__ __volatile__ (
            "caspal x0, x1, x2, x3, [%4]"
            : "+r"(x0_lo), "+r"(x1_hi)
            : "r"(x2_lo), "r"(x3_hi), "r"(dst)
            : "memory"
        );

        /* Update caller's expected with what hardware returned */
        expected->lo = x0_lo;
        expected->hi = x1_hi;

        /* Compare hardware result (x0/x1) with our original expectation */
        return (x0_lo == old_exp_lo && x1_hi == old_exp_hi);
    } else {
        /* ARMv8.0 Compatible LL/SC Loop */
        /* ARMv8.0 Compatible LL/SC Loop */
    uint64_t old_lo, old_hi;
    uint32_t res; /* Status register */

    /* 
     * Operand Mapping:
     * %0=old_lo, %1=old_hi, %2=res (status)
     * %3=exp_lo, %4=exp_hi, %5=dst
     * %6=new_lo, %7=new_hi
     */
    __asm__ __volatile__(
        "1: ldxp %0, %1, [%5]\n"
        "   cmp %0, %3\n"
        "   ccmp %1, %4, #0, eq\n"
        "   b.ne 2f\n"
        "   stxp %w2, %6, %7, [%5]\n" /* Store result to res (%w2) */
        "   cbnz %w2, 1b\n"
        "2:\n"
        : "=&r"(old_lo), "=&r"(old_hi), "=&r"(res)
        : "r"(expected->lo), "r"(expected->hi), "r"(dst), "r"(desired.lo), "r"(desired.hi)
        : "cc", "memory"
    );
        
        if (old_lo == expected->lo && old_hi == expected->hi) return true;
        
        expected->lo = old_lo;
        expected->hi = old_hi;
        return false;
    }

#else
    /* 
     * Provides binary compatibility for 32-bit/Embedded targets (Pico).
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
    
    if (mem[0] == expected->lo && mem[1] == expected->hi) {
        mem[0] = desired.lo;
        mem[1] = desired.hi;
        success = true;
    } else {
        expected->lo = mem[0];
        expected->hi = mem[1];
    }
    
    atomic_flag_clear_explicit(&_hn4_global_cas_lock, memory_order_release);
    return success;
#endif
}

/*
 * _hn4_ctz64
 * Count Trailing Zeros (64-bit).
 *
 * CONTRACT:
 * - Input: 64-bit unsigned integer.
 * - Output: Index of least significant set bit (0..63).
 * - Safety: Returns 64 if x == 0 (Unifies behavior across platforms).
 *
 * IMPLEMENTATION:
 * - GCC/Clang: __builtin_ctzll (Hardware TZCNT/BSF)
 * - MSVC x64:  _BitScanForward64
 * - MSVC x86:  Split 32-bit _BitScanForward
 * - Fallback:  Branchless De Bruijn Multiplication
 */
HN4_INLINE int _hn4_ctz64(uint64_t x) {
    if (HN4_UNLIKELY(x == 0)) return 64;

#if defined(__GNUC__) || defined(__clang__)
    /* GCC/Clang Intrinsic */
    return __builtin_ctzll(x);

#elif defined(_MSC_VER)
    unsigned long r = 0;
    
    #if defined(_M_X64) || defined(_M_ARM64)
        _BitScanForward64(&r, x);
        return (int)r;
    
    #else
        if (_BitScanForward(&r, (unsigned long)x)) {
            return (int)r;
        }
        /* Scan upper 32 bits (Guaranteed non-zero due to top check) */
        _BitScanForward(&r, (unsigned long)(x >> 32));
        return (int)r + 32;
    #endif

#else
    /* 
     * Portable Branchless Fallback (De Bruijn Multiplication).
     * Replaces branch-heavy if/else chain with constant-time math.
     * Relies on 64-bit overflow wrap-around behavior (standard in C).
     */
    static const int table[64] = {
        0, 1, 48, 2, 57, 49, 28, 3, 61, 58, 50, 42, 38, 29, 17, 4,
        62, 55, 59, 36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12, 5,
        63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11,
        46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19, 9, 13, 8, 7, 6
    };
    
    /* Isolate LSB: (x & -x) */
    uint64_t lsb = x & (~x + 1); 
    
    return table[((uint64_t)(lsb * 0x03F79D71B4CB0A89ULL)) >> 58];
#endif
}


/* Standard 64-bit CAS wrapper */
HN4_INLINE bool _hn4_cas64(volatile uint64_t* dst,
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
 */
HN4_INLINE hn4_aligned_u128_t  _hn4_load128(volatile void* src) {
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

/* 
 * PRE-CALCULATED TRAJECTORY MATRIX
 * Rows: Format Profile (0-7)
 * Cols: Device Type (0-3)
 * Value: Max K limit (0 or 12)
 */
static const uint8_t _hn4_traj_limit_lut[8][4] = {
    /* GENERIC */ {12, 0, 0, 0}, 
    /* GAMING  */ {12, 0, 0, 0},
    /* AI      */ {12, 0, 0, 0},
    /* ARCHIVE */ {12, 0, 0, 0},
    /* PICO    */ { 0, 0, 0, 0}, /* Always 0 */
    /* SYSTEM  */ {12, 0, 0, 0},
    /* USB     */ { 0, 0, 0, 0}, /* Always 0 */
    /* CLOUD   */ {12, 0, 0, 0}
};

HN4_INLINE uint8_t _get_trajectory_limit(const hn4_volume_t* vol) {
    /* 
     * Branchless Lookup:
     * Masking ensures safety even if SB is corrupt. 
     * No 'if' statements, purely memory access.
     */
    uint32_t p = vol->sb.info.format_profile & 0x7;
    uint32_t d = vol->sb.info.device_type_tag & 0x3;
    
    return _hn4_traj_limit_lut[p][d];
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

/*
 * _project_coprime_vector
 * 
 * Deterministically perturbs V to ensure it shares no small factors with Phi.
 * This guarantees O(1) execution (no loops) while pushing the first possible 
 * collision cycle to > 2*3*5*7*11*13 (30,030 blocks).
 * 
 */
HN4_INLINE uint64_t _project_coprime_vector(uint64_t v, uint64_t phi) {
    v |= 1; 

    static const uint8_t primes[] = {3, 5, 7, 11, 13};
    
    for (int i = 0; i < 5; i++) {
        uint8_t p = primes[i];
        uint64_t mask = ((phi % p) == 0) & ((v % p) == 0);

        if (mask) {
            if (v > (UINT64_MAX - 2)) v = 1;
            else v += 2;
        }
    }

    if (phi > 1) {
        if (v >= phi) {
            v %= phi;
            if (v == 0) v = 3;
            v |= 1; 
        }
    }

    return v;
}

/*
 * _get_random_uniform
 * ELIMINATES MODULO BIAS using Rejection Sampling.
 * Returns a number in range [0, upper_bound - 1].
 */
static uint64_t _get_random_uniform(uint64_t upper_bound) {
    if (HN4_UNLIKELY(upper_bound <= 1)) return 0;

#if defined(__SIZEOF_INT128__)
    uint64_t rng = hn4_hal_get_random_u64();
    return (uint64_t)(((__uint128_t)rng * upper_bound) >> 64);
#else
    if ((upper_bound & (upper_bound - 1)) == 0) {
        return hn4_hal_get_random_u64() & (upper_bound - 1);
    }
    return hn4_hal_get_random_u64() % upper_bound;
#endif
}

/*
 * _check_saturation
 * 
 * Determines if the volume has entered the "Event Horizon" state (Saturation).
 */
 bool _check_saturation(HN4_IN hn4_volume_t* vol, bool is_genesis) {
    uint64_t l_gen = vol->alloc.limit_genesis;
    uint64_t l_upd = vol->alloc.limit_update;
    uint64_t l_rec = vol->alloc.limit_recover;

    if (HN4_UNLIKELY(l_upd == 0)) {
        if (HN4_UNLIKELY(vol->vol_block_size == 0)) return true;

#ifdef HN4_USE_128BIT
        hn4_u128_t cap_128 = vol->vol_capacity_bytes;
        hn4_u128_t blocks_128;

        if (vol->sb.info.format_profile == HN4_PROFILE_PICO) {
            #define HN4_PICO_HARD_LIMIT (2ULL * 1024 * 1024 * 1024)
            if (cap_128.hi > 0 || cap_128.lo > HN4_PICO_HARD_LIMIT) {
                cap_128.hi = 0;
                cap_128.lo = HN4_PICO_HARD_LIMIT;
            }
        }

        if ((vol->vol_block_size & (vol->vol_block_size - 1)) == 0) {
            if (vol->vol_block_size == 1) {
                 blocks_128 = cap_128;
            } else {
                int shift = _hn4_ctz64(vol->vol_block_size);
                if (shift >= 64) {
                    blocks_128.lo = (cap_128.hi >> (shift - 64));
                    blocks_128.hi = 0;
                } else if (shift > 0) { /* FIX: Ensure shift is non-zero before subtracting from 64 */
                    blocks_128.lo = (cap_128.lo >> shift) | (cap_128.hi << (64 - shift));
                    blocks_128.hi = (cap_128.hi >> shift);
                } else {
                    /* Fallback for shift == 0 (should be covered by block_size==1 check, but safe) */
                    blocks_128 = cap_128;
                }
            }
        } else {
            blocks_128 = hn4_u128_div_u64(cap_128, vol->vol_block_size);
        }
        
        hn4_u128_t lim_gen_128 = hn4_u128_div_u64(hn4_u128_mul_u64(blocks_128, HN4_SATURATION_GENESIS), 100);
        hn4_u128_t lim_upd_128 = hn4_u128_div_u64(hn4_u128_mul_u64(blocks_128, HN4_SATURATION_UPDATE), 100);
        hn4_u128_t lim_rec_128 = hn4_u128_div_u64(hn4_u128_mul_u64(blocks_128, (HN4_SATURATION_GENESIS - 5)), 100);

        l_gen = (lim_gen_128.hi > 0) ? UINT64_MAX : lim_gen_128.lo;
        l_upd = (lim_upd_128.hi > 0) ? UINT64_MAX : lim_upd_128.lo;
        l_rec = (lim_rec_128.hi > 0) ? UINT64_MAX : lim_rec_128.lo;

#else
        uint64_t cap_64 = vol->vol_capacity_bytes;
        
        if (vol->sb.info.format_profile == HN4_PROFILE_PICO) {
            #define HN4_PICO_HARD_LIMIT (2ULL * 1024 * 1024 * 1024)
            if (cap_64 > HN4_PICO_HARD_LIMIT) {
                cap_64 = HN4_PICO_HARD_LIMIT;
            }
        }

        uint64_t cap_blocks = cap_64 / vol->vol_block_size;
        
        if (cap_blocks <= (UINT64_MAX / HN4_SATURATION_UPDATE)) {
            l_gen = (cap_blocks * HN4_SATURATION_GENESIS) / 100;
            l_upd = (cap_blocks * HN4_SATURATION_UPDATE) / 100;
            l_rec = (cap_blocks * (HN4_SATURATION_GENESIS - 5)) / 100;
        } else {
            l_gen = (cap_blocks / 100) * HN4_SATURATION_GENESIS;
            l_upd = (cap_blocks / 100) * HN4_SATURATION_UPDATE;
            l_rec = (cap_blocks / 100) * (HN4_SATURATION_GENESIS - 5);
        }
#endif
        /* Atomic Commit to Cache (Struct fields updated here) */
        vol->alloc.limit_genesis = l_gen;
        vol->alloc.limit_update  = l_upd;
        vol->alloc.limit_recover = l_rec;
    }

    uint64_t used = atomic_load_explicit(&vol->alloc.used_blocks, memory_order_relaxed);
    
    uint32_t flags = atomic_load_explicit(&vol->sb.info.state_flags, memory_order_relaxed);
    bool flag_set = (flags & HN4_VOL_RUNTIME_SATURATED) != 0;

    if (used >= l_gen) {
        if (!flag_set) {
            atomic_fetch_or_explicit(&vol->sb.info.state_flags, 
                                     HN4_VOL_RUNTIME_SATURATED, 
                                     memory_order_relaxed);
            flag_set = true;
        }
    } else if (used < l_rec) {
        if (flag_set) {
            atomic_fetch_and_explicit(&vol->sb.info.state_flags, 
                                      ~HN4_VOL_RUNTIME_SATURATED, 
                                      memory_order_relaxed);
            flag_set = false;
        }
    }

    if (is_genesis) return flag_set || (used >= l_gen);

    return (used >= l_upd);
}

 /* 
 * Quality Verdict Table
 * Rows: Strict Mode (0=Relaxed, 1=Strict)
 * Cols: Quality Bits (00=Toxic, 01=Bronze, 10=Silver, 11=Gold)
 */
static const hn4_result_t _quality_verdict_lut[2][4] = {
    { HN4_ERR_MEDIA_TOXIC, HN4_OK,              HN4_OK, HN4_OK },
    { HN4_ERR_MEDIA_TOXIC, HN4_ERR_MEDIA_TOXIC, HN4_OK, HN4_OK }
};

hn4_result_t _check_quality_compliance(hn4_volume_t* vol, uint64_t lba, uint8_t intent) {
    if (!vol->quality_mask) return HN4_OK; 

    uint64_t word_idx = lba / 32;
    if ((word_idx * 8) >= vol->qmask_size) {
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC | HN4_VOL_DIRTY);
        return HN4_ERR_GEOMETRY;
    }

    uint32_t shift = (lba % 32) * 2;
    uint64_t q_word = atomic_load_explicit((_Atomic uint64_t*)&vol->quality_mask[word_idx], memory_order_relaxed);
    uint8_t q_val = (q_word >> shift) & 0x3;

    /* 
     * Determine Strictness without branching:
     * strict = (intent == METADATA) | (profile == SYSTEM)
     */
    int is_strict = (intent == HN4_ALLOC_METADATA) | 
                    (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);
    
    /* Normalize to 0 or 1 */
    is_strict = (is_strict != 0);

    return _quality_verdict_lut[is_strict][q_val];
}

HN4_INLINE uint64_t _mul_mod_safe(uint64_t a, uint64_t b, uint64_t m) {
    if (m == 0) return 0;

    if (b == 0 || a < (0xFFFFFFFFFFFFFFFFULL / b)) return (a * b) % m;

#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((__uint128_t)a * b) % m);
#else
    /* Slow path for 32-bit targets without __int128 */
    uint64_t res = 0;
    a %= m;
    while (b > 0) {
        if (b & 1) {
            if (res >= m - a) res -= (m - a);
            else res += a;
        }
        b >>= 1;
        if (b > 0) {
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

/*
 * _calc_nano_trajectory
 * 
 * Maps a Unique ID to a physical sector in the Cortex (D0) region.
 * Uses Quadratic Probing (k^2) to resolve collisions deterministically.
 * 
 * Formula: Slot = (Hash(ID) + 0.5*k + 0.5*k^2) % Total_Slots
 */
 hn4_result_t _calc_nano_trajectory(
    hn4_volume_t* vol,
    hn4_u128_t seed_id,
    uint32_t orbit_k,
    hn4_addr_t* out_lba
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    if (ss == 0) return HN4_ERR_GEOMETRY;

    /* 1. Define the Domain */
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t capacity   = end_sect - start_sect;

    if (capacity == 0) return HN4_ERR_ENOSPC;

    /* 2. Fold Identity into Entropy */
      uint64_t h = seed_id.lo ^ seed_id.hi;
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL; /* Golden Ratio Prime */
    h ^= (h >> 33);

    uint64_t stride = seed_id.hi;

    stride = _project_coprime_vector(stride, capacity);

    uint64_t k_sq = (uint64_t)orbit_k * orbit_k;
    uint64_t k_quad = (orbit_k + k_sq) >> 1; 

    uint64_t k_offset = _mul_mod_safe(k_quad, stride, capacity);
    uint64_t target_idx = (h + k_offset) % capacity;
    
    *out_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, target_idx);
    
    return HN4_OK;
}

static void _update_counters_and_l2(hn4_volume_t* vol, uint64_t block_idx, bool is_set) {
    uint64_t l2_idx      = block_idx >> 9;          /* block_idx / 512 */
    uint64_t l2_word_idx = l2_idx >> 6;             /* l2_idx / 64 */
    uint64_t l2_mask     = 1ULL << (l2_idx & 63);   /* l2_idx % 64 */

    /* =========================================================================
     * PATH A: ALLOCATION (Hot Path)
     * ========================================================================= */
    if (HN4_LIKELY(is_set)) {

        atomic_fetch_add_explicit(&vol->alloc.used_blocks, 1, memory_order_relaxed);

        if (vol->locking.l2_summary_bitmap) {
             _Atomic uint64_t* l2_ptr = (_Atomic uint64_t*)&vol->locking.l2_summary_bitmap[l2_word_idx];
             uint64_t curr = atomic_load_explicit(l2_ptr, memory_order_relaxed);
             if (!(curr & l2_mask)) {
                 atomic_fetch_or_explicit(l2_ptr, l2_mask, memory_order_release);
             }
        }
    } 
    /* =========================================================================
     * PATH B: DEALLOCATION (Heavy Path)
     * ========================================================================= */
    else {

        uint64_t prev = atomic_fetch_sub_explicit(&vol->alloc.used_blocks, 1, memory_order_relaxed);
        
        if (HN4_UNLIKELY(prev == 0)) {
            /* Restore and log error - extremely rare slow path */
            atomic_fetch_add_explicit(&vol->alloc.used_blocks, 1, memory_order_relaxed);
            atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
            HN4_LOG_ERR("Allocator Underflow! Used=0 but freeing block %llu.", (unsigned long long)block_idx);
            return; 
        }
        
        if (vol->locking.l2_summary_bitmap) {
            bool use_lock = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);
            if (use_lock) hn4_hal_spinlock_acquire(&vol->locking.l2_lock);

            uint64_t word_idx = block_idx >> 6;
            uint64_t start_w  = word_idx & ~7ULL; 
            size_t total_words = vol->bitmap_size >> 4;

            bool region_empty = true;
            for (int i = 0; i < 8; i++) {
                if ((start_w + i) >= total_words) break;
                if (atomic_load_explicit((_Atomic uint64_t*)&vol->void_bitmap[start_w + i].data, 
                    memory_order_relaxed) != 0) {
                    region_empty = false;
                    break;
                }
            }

            if (region_empty) {
                _Atomic uint64_t* l2_ptr = (_Atomic uint64_t*)&vol->locking.l2_summary_bitmap[l2_word_idx];

                atomic_fetch_and_explicit(l2_ptr, ~l2_mask, memory_order_release);

                atomic_thread_fence(memory_order_seq_cst);

                bool oops_dirty = false;
                for (int i = 0; i < 8; i++) {
                    if ((start_w + i) >= total_words) break;
                    if (atomic_load_explicit((_Atomic uint64_t*)&vol->void_bitmap[start_w + i].data, 
                        memory_order_relaxed) != 0) {
                        oops_dirty = true;
                        break;
                    }
                }

                if (HN4_UNLIKELY(oops_dirty)) {
                    atomic_fetch_or_explicit(l2_ptr, l2_mask, memory_order_relaxed);
                }
            }

            if (use_lock) hn4_hal_spinlock_release(&vol->locking.l2_lock);
        }
    }
}

/* =========================================================================
 * 5. BITMAP OPERATIONS
 * ========================================================================= */

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
        
        /* Validation */
        if (vol->sb.info.format_profile != HN4_PROFILE_PICO) {
            return HN4_ERR_UNINITIALIZED;
        }

        const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
        const uint32_t ss = caps->logical_block_size;
        
        uint32_t alloc_size = (ss > 4096) ? ss * 2 : 8192; 
        void* sector_buf = hn4_hal_mem_alloc(alloc_size);
        
        if (!sector_buf) return HN4_ERR_NOMEM;
        
        uint64_t word_idx      = block_idx / 64;
        uint64_t bit_off       = block_idx % 64;
        uint64_t byte_offset   = word_idx * sizeof(hn4_armored_word_t);
        uint64_t sector_offset = (byte_offset / ss) * ss;
        uint64_t offset_in_sec = byte_offset % ss;

        hn4_addr_t io_lba = hn4_addr_add(vol->sb.info.lba_bitmap_start, sector_offset / ss);

        uint32_t sectors_to_io = 1;
        if ((offset_in_sec + sizeof(hn4_armored_word_t)) > ss) {
            sectors_to_io = 2;
        }

        hn4_addr_t max_lba = vol->sb.info.lba_qmask_start;
        if (hn4_addr_to_u64(io_lba) + sectors_to_io > hn4_addr_to_u64(max_lba)) {
             hn4_hal_mem_free(sector_buf);
             return HN4_ERR_GEOMETRY;
        }

        hn4_result_t res = HN4_OK;

        uint32_t lock_idx = (uint32_t)(hn4_addr_to_u64(io_lba) % HN4_CORTEX_SHARDS);
        hn4_hal_spinlock_acquire(&vol->locking.shards[lock_idx].lock);

        if (alloc_size < (sectors_to_io * ss)) {
             res = HN4_ERR_NOMEM;
             goto pico_cleanup;
        }

        /* 1. READ */
        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, io_lba, sector_buf, sectors_to_io) != HN4_OK) {
            res = HN4_ERR_HW_IO;
            goto pico_cleanup;
        }

        hn4_armored_word_t* word = (hn4_armored_word_t*)((uint8_t*)sector_buf + offset_in_sec);

        /* 2. VALIDATE */
        uint64_t safe_data;
        bool corrected = false;
        
        res = _ecc_check_and_fix(vol, word->data, word->ecc, &safe_data, &corrected);
        if (HN4_UNLIKELY(res != HN4_OK)) {
            goto pico_cleanup;
        }

        if (corrected) {
            word->data = safe_data;
        }

        /* 3. EXECUTE */
        bool is_set = (word->data & (1ULL << bit_off)) != 0;
        bool mutation_needed = false;
        bool report_change = false;

        if (op == BIT_TEST) {
            if (out_result) *out_result = is_set;
            mutation_needed = corrected;
        } else if ((op == BIT_SET && is_set) || 
                 ((op == BIT_CLEAR || op == BIT_FORCE_CLEAR) && !is_set)) 
        {
            if (out_result) *out_result = false;
            mutation_needed = corrected;
        } else {
            if (op == BIT_SET) word->data |= (1ULL << bit_off);
            else word->data &= ~(1ULL << bit_off);
            
            word->ecc = _calc_ecc_hamming(word->data);
            mutation_needed = true;
            report_change = true;
        }

        /* 4. WRITE */
        if (mutation_needed) {
            hn4_result_t w_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, io_lba, sector_buf, sectors_to_io);
            
            if (w_res != HN4_OK) {
                if (op == BIT_TEST) {
                    res = HN4_OK; 
                } else {
                    res = HN4_ERR_HW_IO;
                    report_change = false;
                }
            }
        }
        
        if (op != BIT_TEST && out_result) *out_result = report_change;

   pico_cleanup:
        hn4_hal_spinlock_release(&vol->locking.shards[lock_idx].lock);
        
        if (sector_buf) hn4_hal_mem_free(sector_buf);
        return res;
    }

    /* =========================================================================
     * PATH B: STANDARD RAM MODE (Atomic 128-bit CAS)
     * ========================================================================= */

    /* 1. Alignment & Geometry Checks */
    if (HN4_UNLIKELY(((uintptr_t)vol->void_bitmap & 0xF) != 0)) {
        return HN4_ERR_INTERNAL_FAULT;
    }

    uint64_t word_idx = block_idx / 64;
    uint64_t bit_off  = block_idx % 64;
    uint64_t bit_mask = (1ULL << bit_off);
    
    if (HN4_UNLIKELY((word_idx * sizeof(hn4_armored_word_t)) >= vol->bitmap_size)) {
        return HN4_ERR_GEOMETRY;
    }

    volatile void* target_addr = &vol->void_bitmap[word_idx];

    hn4_aligned_u128_t  expected, desired;
    bool success = false;
    bool logic_change = false;      
    bool heal_event_pending = false;

    expected = _hn4_load128(target_addr);

    do {
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
            
            if (vol->read_only) {
                if (out_result) *out_result = is_set;
                return HN4_OK;
            }

            desired.lo = safe_data;
            logic_change = true;
            is_healing_write = true;
        }
        else if ((op == BIT_SET && is_set) || 
                 ((op == BIT_CLEAR || op == BIT_FORCE_CLEAR) && !is_set)) 
        {
            if (!was_corrected) {
                /* 
                 * HEALING L2: Even if L1 (Main) is correct, L2 might be stale.
                 * If we intended to SET, ensure L2 reflects it.
                 */
                if (op == BIT_SET && vol->locking.l2_summary_bitmap) {
                    uint64_t l2_idx = block_idx / HN4_L2_COVERAGE_BITS;
                    uint64_t l2_word = l2_idx / 64;
                    atomic_fetch_or_explicit((_Atomic uint64_t*)&vol->locking.l2_summary_bitmap[l2_word], 
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

            desired.lo = safe_data;
            is_healing_write = true;
        } 
        else {
            desired.lo = (op == BIT_SET) ? (safe_data | bit_mask) : (safe_data & ~bit_mask);
            logic_change = true;
        }

        /* 2.4 Reconstruct Metadata */
        uint8_t new_ecc = _calc_ecc_hamming(desired.lo);

      uint64_t next_ver;

        if (is_healing_write) { 
            next_ver = ver;
        } else {
            next_ver = (ver + 1) & 0x00FFFFFFFFFFFFFFULL;
            
            if (HN4_UNLIKELY(next_ver == 0)) next_ver = 1;
        }

        desired.hi = (next_ver << 8) | (uint64_t)new_ecc;

        /* 2.5 Commit */
        success = _hn4_cas128(target_addr, &expected, desired);

    } while (HN4_UNLIKELY(!success));


    /* 3. Post-Commit Updates */
    
    if (HN4_UNLIKELY(heal_event_pending))  atomic_fetch_add(&vol->health.heal_count, 1);

    if (out_result) {
        if (op == BIT_TEST) *out_result = ((desired.lo & bit_mask) != 0);
        else *out_result = logic_change;
    }

     bool commit_side_effects = (logic_change && op != BIT_TEST) || 
                               (heal_event_pending && !vol->read_only);

    if (commit_side_effects) {
        if (op != BIT_FORCE_CLEAR && !vol->locking.in_eviction_path) {
            atomic_fetch_or_explicit(&vol->sb.info.state_flags, HN4_VOL_DIRTY, memory_order_seq_cst);
        }

        if (op != BIT_TEST) { 
            _update_counters_and_l2(vol, block_idx, (op == BIT_SET));
        }
        
        atomic_thread_fence(memory_order_seq_cst);
    }
    
    return heal_event_pending ? HN4_INFO_HEALED : HN4_OK;
}

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
    /* 
     * [INERTIAL DAMPING]
     * Lookup table for Theta Jitter. This scatters writes on SSDs to avoid 
     * hitting the same flash pages repeatedly, reducing wear concentration.
     */
    static const uint8_t _theta_lut[16] = {
        0, 1, 3, 6, 10, 15, 21, 28, 
        36, 45, 55, 66, 78, 91, 105, 120
    };

    /* 1. Validate Fractal Scale & Device Context */
    /* Safety: M >= 63 would cause 1ULL << M to overflow 64 bits. */
    if (HN4_UNLIKELY(M >= 63 || !vol->target_device)) {
        return HN4_LBA_INVALID;
    }

    /* 2. Load Geometry */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (HN4_UNLIKELY(!caps)) return HN4_LBA_INVALID;

    uint32_t bs          = vol->vol_block_size;
    uint32_t ss          = caps->logical_block_size ? caps->logical_block_size : 512;
    uint32_t sec_per_blk = (bs / ss) ? (bs / ss) : 1;
    uint64_t S           = 1ULL << M; /* The Fractal Stride Size */

    /* 
     * [CAPACITY NORMALIZATION]
     * Calculate total usable blocks. Handles the 128-bit Quettabyte build flag.
     */
     uint64_t total_blocks;

#ifdef HN4_USE_128BIT
    hn4_u128_t cap_128 = vol->vol_capacity_bytes;
    hn4_u128_t blocks_128 = hn4_u128_div_u64(cap_128, bs);
    
    if (HN4_UNLIKELY(blocks_128.hi > 0)) total_blocks = UINT64_MAX;
    else total_blocks = blocks_128.lo;
#else
    total_blocks = vol->vol_capacity_bytes / bs;
#endif

    /* 
     * [FLUX DOMAIN ALIGNMENT]
     * Determine where the "Flux" (Data) region starts.
     */
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
    
    /* 
     * OPTIMIZATION 4: Division Elimination
     * Replaced: phi = available_blocks / S;
     * With:     phi = available_blocks >> M;
     * 
     * Since S is defined as (1 << M), division by S is equivalent to right-shift by M.
     * This saves ~40-80 CPU cycles on 64-bit platforms.
     */
    uint64_t phi = available_blocks >> M;
    
    if (HN4_UNLIKELY(phi == 0)) return HN4_LBA_INVALID;

    /* 
     * 3. Apply Gravity Assist (Vector Shift)
     * If we are deep in collision territory (k >= 4), we engage "Gravity Assist" 
     * to teleport the vector using the canonical Swizzle Engine. 
     * This escapes local gravity wells (hash collisions).
     */
    uint64_t effective_V = V;
    if (k >= HN4_GRAVITY_ASSIST_K) {
        effective_V = hn4_swizzle_gravity_assist(V);
    }
    effective_V |= 1; /* Force Odd (Anti-Even Degeneracy) */

    /*
     * [DECOMPOSITION]
     * G is split into the fractal index and the "Entropy Loss" (sub-block offset).
     * The Entropy Loss is re-injected at the end to preserve byte alignment.
     */
    uint64_t g_aligned   = G & ~(S - 1);
    
    /* 
     * OPTIMIZATION 4 (Cont.): Division Elimination 
     * Replaced: g_fractal = g_aligned / S;
     * With:     g_fractal = g_aligned >> M;
     */
    uint64_t g_fractal   = g_aligned >> M;
    
    uint64_t entropy_loss = G & (S - 1);
    uint64_t cluster_idx = N >> 4;
    
    /* UNUSED: uint64_t sub_offset  = N & 0xF; */
    
    uint64_t term_n = cluster_idx % phi;
    uint64_t term_v = effective_V % phi;
    
    /* 
     * [COPRIMALITY ENFORCEMENT]
     * Ensures the vector V covers the entire ring Phi without short cycles.
     * Replaces the old slow GCD loop with the O(1) projection function.
     */
    term_v = _project_coprime_vector(term_v, phi);
    
    uint64_t offset = _mul_mod_safe(term_n, term_v, phi);
    uint64_t theta = 0;
    
    /* 
     * [DEVICE PHYSICS]
     * Check Linear LUT (Mask & 0x3 protects against corrupt tags).
     * ZNS, HDD, and Tape require sequential writes (Linear).
     * SSDs allow scattered writes (Ballistic).
     */
    bool is_linear = _hn4_is_linear_lut[vol->sb.info.device_type_tag & 0x3];
    bool is_system = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);
    
    /* 
     * HARDWARE OVERRIDE:
     * 1. ZNS: Must be sequential to respect Write Pointer.
     * 2. ROTATIONAL: Must be sequential to avoid seek latency (Fragmentation).
     */
    uint64_t linear_mask = HN4_HW_ZNS_NATIVE | HN4_HW_ROTATIONAL;

    if (vol->sb.info.hw_caps_flags & linear_mask) is_linear = true;

    if (!is_linear && !is_system) {
        if (HN4_UNLIKELY(phi < 32)) {
            theta = k % phi;
        } else {
            uint8_t safe_k = (k < 16) ? k : 15;
            theta = _theta_lut[safe_k] % phi;
        }
    }
    
    /* 7. Final Projection */
    uint64_t target_fractal_idx = (g_fractal + offset + theta) % phi;
    uint64_t rel_block_idx = (target_fractal_idx << M);
    
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
static bool _is_quality_compliant(
    HN4_IN hn4_volume_t* vol,
    uint64_t lba,
    uint8_t intent
    )
{
    if (!vol->quality_mask) return true;

    uint64_t word_idx = lba / 32;

    if ((word_idx * 8) >= vol->qmask_size) {
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC | HN4_VOL_DIRTY);
        return false;
    }

    uint32_t shift = (lba % 32) * 2;
    
    /* Atomic Load to prevent torn reads during concurrent repair */
    uint64_t q_word = atomic_load_explicit(
        (_Atomic uint64_t*)&vol->quality_mask[word_idx], 
        memory_order_relaxed
    );
    
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
    if (vol->sb.info.format_profile != HN4_PROFILE_AI) return false;
    if (!vol->topo_map || vol->topo_count == 0) return false;

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

/* 
 * BITMASK SCAN: HDD OPTIMIZATION
 * Scans 64 blocks (1 Word) instantly using CPU intrinsics.
 * Returns relative offset of free bit (0-63) or -1 if full.
 */
static int _scan_word_for_gap(hn4_volume_t* vol, uint64_t block_idx) {
    uint64_t word_idx = block_idx / 64;
    
    /* 
     * OPTIMIZATION: Optimistic 64-bit Peek
     * We load ONLY the data word using a relaxed atomic load.
     * This avoids the expensive 'lock cmpxchg16b' instruction on x86.
     */
    uint64_t raw_data = atomic_load_explicit(
        (_Atomic uint64_t*)&vol->void_bitmap[word_idx].data, 
        memory_order_relaxed
    );

    /* Fail fast if physically full (ignoring potential ECC errors for now) */
    if (raw_data == 0xFFFFFFFFFFFFFFFFULL) return -1;

    /* 
     * Candidate found. NOW pay the cost for full 128-bit atomic load 
     * to get consistent Data + ECC + Version for validation.
     */
    hn4_aligned_u128_t raw = _hn4_load128(&vol->void_bitmap[word_idx]);
    
    /* Re-check data after atomic load (race condition handling) */
    if (raw.lo == 0xFFFFFFFFFFFFFFFFULL) return -1;

    /* ECC Validation */
    uint64_t data;
    if (_ecc_check_and_fix(vol, raw.lo, (uint8_t)(raw.hi & 0xFF), &data, NULL) != HN4_OK) return -1;
    
    uint64_t free_mask = ~data;
    return _hn4_ctz64(free_mask);
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
        uint64_t total_blocks;
        hn4_addr_t end_marker = vol->sb.info.lba_horizon_start;

    #ifdef HN4_USE_128BIT
        hn4_u128_t res = hn4_u128_div_u64(end_marker, sec_per_blk); /* Convert Sector LBA -> Block Index */
    
        if (res.hi > 0) return HN4_ERR_GEOMETRY; 
        total_blocks = res.lo;
    #else
        /* Cast strictly to u64 as hn4_addr_t might be u64 */
        total_blocks = (uint64_t)end_marker / sec_per_blk;
    #endif
        uint64_t flux_start_sect = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
        uint64_t flux_start_blk  = flux_start_sect / sec_per_blk;

        /*
         * Align the Flux Start to the S boundary.
         * The trajectory equation relies on modulo arithmetic; misaligned bases
         * cause wrap-around corruption at the end of the disk.
         */
        uint64_t flux_aligned_blk = (flux_start_blk + (S - 1)) & ~(S - 1);

        if (flux_aligned_blk < total_blocks) {

            if (flux_aligned_blk >= total_blocks) return HN4_ERR_ENOSPC;

            uint64_t available_blocks = total_blocks - flux_aligned_blk;
            uint64_t phi = available_blocks / S;
            
            /* Strict check against 0 to prevent FPE in modulo math later */
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
                                
                                uint64_t range_start_blk = vol->topo_map[i].lba_start / sec_per_blk;
                                uint64_t range_len_blk   = vol->topo_map[i].lba_len / sec_per_blk;

                                if (range_start_blk >= flux_aligned_blk && 
                                   (range_start_blk + range_len_blk) <= total_blocks) 
                                {
                                    uint64_t rel_start = range_start_blk - flux_aligned_blk;
                                    
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
                 * Hardware Override: Rotational Media (HDD)
                 * If the hardware reports spinning platters, force DEEP scan policy.
                 * Local exhaustive search is cheaper than seeking to a new G.
                 */
                if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
                    policy |= HN4_POL_DEEP | HN4_POL_SEQ;
                }

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
                        uint64_t last = atomic_load_explicit(&vol->alloc.last_alloc_g, memory_order_relaxed);
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

                    /* [OPTIMIZATION] HDD: Bitmask Gap Scanning (Stop thrashing) */
                    if (vol->sb.info.device_type_tag == HN4_DEV_HDD) {
                        int gap = _scan_word_for_gap(vol, G);
                        if (gap != -1) {
                            /* Align G to word boundary and add gap offset */
                            uint64_t aligned_G = (G & ~63ULL) + gap;
                            G = aligned_G; /* Snap to nearest hole */
                        }
                    }

                    /* 4b. Pick Orbit Vector (V) */
                    uint64_t V;
                    if (force_sequential) {
                        V = 1;
                    } else {
                        /* Scale V relative to the window size */
                        if (use_affinity && win_phi > 1) {
                            /* Constrain V to 1/16th of window */
                            uint64_t rng = hn4_hal_get_random_u64();
                            uint64_t v_limit = (rng % 10 == 0) ? win_phi : (win_phi / 16);
                            if (v_limit < 2) v_limit = 2;
                            V = _get_random_uniform(v_limit);
                        } else {
                            V = hn4_hal_get_random_u64();
                        }
                        
                        V = _project_coprime_vector(V, win_phi);
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

                    uint64_t head_lba = _calc_trajectory_lba(vol, G, V, 0, fractal_scale, 0);
                    if (head_lba == HN4_LBA_INVALID) return HN4_ERR_GEOMETRY;

                    hn4_result_t q_res = _check_quality_compliance(vol, head_lba, alloc_intent);
                    if (q_res == HN4_ERR_GEOMETRY) return q_res; 
                    if (q_res != HN4_OK) continue; /* Block is Toxic, skip */

                    bool head_claimed;
                    hn4_result_t res = _bitmap_op(vol, head_lba, BIT_SET, &head_claimed);

                    if (HN4_IS_ERR(res)) return res;

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
                        atomic_store_explicit(&vol->alloc.last_alloc_g, G, memory_order_relaxed);
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
    hn4_addr_t hlba_addr;
    
    hn4_result_t h_res = hn4_alloc_horizon(vol, &hlba_addr);
    
    if (h_res == HN4_OK) {
        *out_G = hn4_addr_to_u64(hlba_addr);
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
 * THE CALCULATION PARADOX:
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
    HN4_OUT hn4_addr_t* out_phys_lba
)
{
    /* 
     * 1. LOAD GEOMETRY (Abstract Types)
     * Use native address types from the superblock to support full range.
     */
    hn4_addr_t start_addr = vol->sb.info.lba_horizon_start;
    hn4_addr_t end_addr   = vol->sb.info.journal_start;

    /* 
     * 2. CALCULATE CAPACITY
     * Horizon Capacity = Journal_Start - Horizon_Start
     */
    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    
    uint32_t ss = caps ? caps->logical_block_size : 512;

    if (ss == 0 || (bs % ss != 0)) {
        HN4_LOG_CRIT("Horizon: Block/Sector mismatch (BS=%u SS=%u)", bs, ss);
        return HN4_ERR_GEOMETRY;
    }
    uint32_t spb = bs / ss;

    uint64_t capacity_blocks;

#ifdef HN4_USE_128BIT
    /* 128-bit: Use math primitives for difference and division */
    if (hn4_u128_cmp(end_addr, start_addr) <= 0) return HN4_ERR_ENOSPC;

    hn4_u128_t diff_sectors = hn4_u128_sub(end_addr, start_addr);
    hn4_u128_t cap_blocks_128 = hn4_u128_div_u64(diff_sectors, spb);
  
    if (cap_blocks_128.hi > 0) {
        HN4_LOG_CRIT("Horizon Capacity exceeds 73 Zettabytes. Unsupported.");
        return HN4_ERR_GEOMETRY;
    }
    capacity_blocks = cap_blocks_128.lo;

#else
    /* 64-bit: Standard arithmetic */
    uint64_t start_sect = hn4_addr_to_u64(start_addr);
    uint64_t end_sect   = hn4_addr_to_u64(end_addr);
    
    if (end_sect <= start_sect) return HN4_ERR_ENOSPC;
    
    capacity_blocks = (end_sect - start_sect) / spb;
#endif

    if (capacity_blocks == 0) return HN4_ERR_ENOSPC;

   /* 
     * 3. RING ALLOCATION LOOP
     * The Horizon acts as a high-velocity Ring Buffer.
     */
    uint64_t max_probes = 4;

    for (uint64_t i = 0; i < max_probes; i++) {
        uint64_t head = atomic_fetch_add(&vol->alloc.horizon_write_head, 1);
        uint64_t block_offset = head % capacity_blocks;
        
        /* 
         * 4. CALCULATE ABSOLUTE LBA
         * Abs_LBA = Start + (Offset * SectorsPerBlock)
         * Must use 128-bit aware addition.
         */
#ifdef HN4_USE_128BIT
        hn4_u128_t offset_128 = hn4_u128_from_u64(block_offset);
        hn4_u128_t byte_off_128 = hn4_u128_mul_u64(offset_128, spb);
        hn4_addr_t abs_lba = start_addr;
        uint64_t old_lo = abs_lba.lo;
        abs_lba.lo += byte_off_128.lo;
        abs_lba.hi += byte_off_128.hi + ((abs_lba.lo < old_lo) ? 1 : 0);

#else
        uint64_t abs_lba = hn4_addr_to_u64(start_addr) + (block_offset * spb);
#endif

        /* 
         * 5. BITMAP RESERVATION
         * Calculate global block index for bitmap tracking.
         * Global_Idx = Abs_LBA / SPB
         */
        uint64_t global_block_idx;
        
#ifdef HN4_USE_128BIT
        /* Use div primitive */
        hn4_u128_t g_idx_128 = hn4_u128_div_u64(abs_lba, spb);
        if (g_idx_128.hi > 0) return HN4_ERR_GEOMETRY; /* Bitmap index overflow */
        global_block_idx = g_idx_128.lo;
#else
        global_block_idx = abs_lba / spb;
#endif
        
        bool state_changed;
        hn4_result_t res = _bitmap_op(vol, global_block_idx, BIT_SET, &state_changed);
        if (res != HN4_OK) return res;

        if (state_changed) {
            *out_phys_lba = abs_lba; /* Return abstract type */
            return HN4_OK;
        }
    }

    /* Saturation: Head caught Tail. */
    return HN4_ERR_ENOSPC;
}


void hn4_free_block(HN4_INOUT hn4_volume_t* vol, hn4_addr_t phys_lba) {
    if (!vol->target_device) return;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps ? caps->logical_block_size : 512;
    uint32_t bs = vol->vol_block_size;
    uint32_t spb = (bs / ss) ? (bs / ss) : 1;

    uint64_t block_idx;

#ifdef HN4_USE_128BIT

    hn4_u128_t idx_128 = hn4_u128_div_u64(phys_lba, spb);

    if (idx_128.hi > 0) {
        HN4_LOG_CRIT("Free OOB: Block Index exceeds 64-bits");
        return;
    }
    block_idx = idx_128.lo;
#else
    /* Standard 64-bit math */
    block_idx = phys_lba / spb;
#endif

    uint64_t max_blk = vol->vol_capacity_bytes / bs;

    if (block_idx >= max_blk) {
        HN4_LOG_WARN("Free OOB: LBA %llu > Max %llu", 
                     (unsigned long long)phys_lba, (unsigned long long)max_blk);
        
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        uint32_t t = atomic_fetch_add(&vol->health.taint_counter, 1);
        
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

    if (HN4_UNLIKELY(!vol || !anchor || !out_lba || !out_k)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    const bool is_ro = vol->read_only;
    const uint64_t t_off = vol->time_offset;

    if (HN4_UNLIKELY(is_ro || t_off != 0)) {
        return HN4_ERR_ACCESS_DENIED;
    }

    const bool d1_saturated = _check_saturation(vol, false);

    const uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);

    /* 
     * =====================================================================
     * PHASE 1: THE FLUX MANIFOLD (D1)
     * =====================================================================
     */
    if (HN4_LIKELY(!d1_saturated)) {

        const uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
        
        uint64_t V_raw = 0;

        __builtin_memcpy(&V_raw, anchor->orbit_vector, 6);

        const uint64_t V = hn4_le64_to_cpu(V_raw) & 0xFFFFFFFFFFFFULL;
        const uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
        const uint8_t alloc_intent = ((dclass & HN4_CLASS_VOL_MASK) == HN4_VOL_STATIC) 
                                     ? HN4_ALLOC_METADATA 
                                     : HN4_ALLOC_DEFAULT;

        const uint8_t max_k = _get_trajectory_limit(vol);

        /* Hot Loop */
        for (uint8_t k = 0; k <= max_k; k++) {
            
            uint64_t lba = _calc_trajectory_lba(vol, G, V, logical_idx, M, k);
            
            if (HN4_UNLIKELY(lba == HN4_LBA_INVALID)) continue;

            hn4_result_t q_res = _check_quality_compliance(vol, lba, alloc_intent);
            
            if (HN4_LIKELY(q_res == HN4_OK)) {
                bool claimed;
                hn4_result_t res = _bitmap_op(vol, lba, BIT_SET, &claimed);
                
                if (HN4_LIKELY(res == HN4_OK && claimed)) {
                    #ifdef HN4_USE_128BIT
                        *out_lba = hn4_addr_from_u64(lba);
                    #else
                        *out_lba = lba; 
                    #endif
                    *out_k = k;
                    return HN4_OK;
                }

                /* Fatal Bitmap Corruption check */
                if (HN4_UNLIKELY(res == HN4_ERR_BITMAP_CORRUPT)) return res;
            } else {
                /* Optimization: Check fatal geometry error only on failure path */
                if (HN4_UNLIKELY(q_res == HN4_ERR_GEOMETRY)) return q_res;
            }
        }
    }

    /* 
     * =====================================================================
     * PHASE 2: THE EVENT HORIZON (D1.5)
     * Linear Log Fallback.
     * =====================================================================
     */

    if (HN4_UNLIKELY(M > 0)) {
        return HN4_ERR_GRAVITY_COLLAPSE;
    }

    const bool is_sys_profile = (vol->sb.info.format_profile == HN4_PROFILE_SYSTEM);
    const uint64_t dclass_raw = hn4_le64_to_cpu(anchor->data_class);
    const bool is_meta_intent = ((dclass_raw & HN4_CLASS_VOL_MASK) == HN4_VOL_STATIC);

    if ((is_sys_profile || is_meta_intent) && !(vol->sb.info.state_flags & HN4_VOL_PANIC)) {
        return HN4_ERR_ENOSPC;
    }

    /* Attempt allocation in the Horizon Ring */
    uint64_t hlba;
    if (hn4_alloc_horizon(vol, &hlba) == HN4_OK) {
        #ifdef HN4_USE_128BIT
            *out_lba = hn4_addr_from_u64(hlba);
        #else
            *out_lba = hlba;
        #endif
        *out_k = HN4_HORIZON_FALLBACK_K;
        return HN4_OK;
    }

    /* Total Saturation */
    return HN4_ERR_GRAVITY_COLLAPSE;
}