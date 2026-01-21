/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Address Primitives
 * SOURCE:      hn4_addr.c
 * STATUS:      HARDENED / REVIEWED
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. OVERFLOW: Operations checking for 64-bit truncation are noisy (LOG_CRIT).
 * 2. ENDIANNESS: Assumes Host Endianness for arithmetic operations.
 *    On-disk conversion must be handled separately by the serializer.
 */

#include "hn4_addr.h"

/* Ensure logging is enabled for critical overflow checks */
#ifndef HN4_LOG_ENABLED
    #define HN4_LOG_ENABLED 1
#endif

/* =========================================================================
 * CORE ARITHMETIC IMPLEMENTATION
 * ========================================================================= */

hn4_addr_t hn4_addr_from_u64(uint64_t val) {
#ifdef HN4_USE_128BIT
    /* Zero-extend 64-bit value to 128-bit structure */
    hn4_addr_t addr = { .lo = val, .hi = 0 };
    return addr;
#else
    /* Passthrough for 64-bit native mode */
    return val;
#endif
}

uint64_t hn4_addr_to_u64(hn4_addr_t addr) {
#ifdef HN4_USE_128BIT
    /*
     * Safety Check:
     * If the high 64-bits are set, we cannot safely downcast to uint64_t.
     * This implies an attempt to read beyond the Exabyte limit on a legacy system.
     */
    if (HN4_UNLIKELY(addr.hi > 0)) {
        HN4_LOG_CRIT("HN4: Address Overflow! 128-bit LBA %llu:%llu truncated.", 
                     (unsigned long long)addr.hi, 
                     (unsigned long long)addr.lo);
        return UINT64_MAX; /* Sentinel error value */
    }
    return addr.lo;
#else
    return addr;
#endif
}

hn4_addr_t hn4_addr_add(hn4_addr_t base, uint64_t inc) 
{
#ifdef HN4_USE_128BIT
    base.lo += inc;
    if (base.lo < inc) base.hi++;
    return base;
#else
    return base + inc;
#endif
}

/* =========================================================================
 * SEMANTIC WRAPPERS
 * ========================================================================= */

hn4_addr_t hn4_lba_from_blocks(uint64_t blocks) {
    return hn4_addr_from_u64(blocks);
}

hn4_addr_t hn4_lba_from_sectors(uint64_t sectors) {
    return hn4_addr_from_u64(sectors);
}

int hn4_u128_cmp(hn4_u128_t a, hn4_u128_t b) {
    if (a.hi > b.hi) return 1;
    if (a.hi < b.hi) return -1;
    if (a.lo > b.lo) return 1;
    if (a.lo < b.lo) return -1;
    return 0;
}

hn4_u128_t hn4_u128_sub(hn4_u128_t a, hn4_u128_t b) {
    hn4_u128_t res;
    res.lo = a.lo - b.lo;
    uint64_t borrow = (a.lo < b.lo) ? 1 : 0;
    res.hi = a.hi - b.hi - borrow;
    
    return res;
}


hn4_u128_t hn4_u128_from_u64(uint64_t v) {
    hn4_u128_t r = { .lo = v, .hi = 0 }; 
    return r;
}

bool hn4_addr_try_u64(hn4_addr_t addr, uint64_t* out) {
#ifdef HN4_USE_128BIT
    /* Safety Check: If high bits are set, we cannot downcast */
    if (HN4_UNLIKELY(addr.hi > 0)) return false;
    *out = addr.lo;
#else
    /* Native 64-bit: Always fits */
    *out = addr;
#endif
    return true;
}

hn4_u128_t hn4_u128_mul_u64(hn4_u128_t a, uint64_t b) {
    hn4_u128_t res = {0};
#if defined(__SIZEOF_INT128__)
    unsigned __int128 r = (unsigned __int128)a.lo * b;
    res.lo = (uint64_t)r;
    res.hi = (uint64_t)(r >> 64) + (a.hi * b);
#else
    /* Correct Decomposition: a * b = (a.hi<<64 + a.lo) * b */
    
    /* 1. Calculate a.lo * b (Full 128-bit result needed for carry) */
    uint64_t al = a.lo & 0xFFFFFFFFULL;
    uint64_t ah = a.lo >> 32;
    uint64_t bl = b & 0xFFFFFFFFULL;
    uint64_t bh = b >> 32;

    uint64_t p0 = al * bl;
    uint64_t p1 = al * bh;
    uint64_t p2 = ah * bl;
    uint64_t p3 = ah * bh;

    /* Sum partials to find carry from low 64 to high 64 */
    /* p1 and p2 overlap in the middle 32 bits */
    uint64_t c0 = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
    uint64_t c1 = (c0 >> 32) + (p1 >> 32) + (p2 >> 32) + p3; /* This is the carry */

    /* 2. Final Result */
    res.lo = a.lo * b; /* CPU wraps low part correctly */
    res.hi = (a.hi * b) + c1; /* Add high part + carry from low */
#endif
    return res;
}

/*
 * hn4_u128_div_u64
 * Performs 128-bit / 64-bit unsigned integer division.
 * 
 * PRODUCTION HARDENING:
 * 1. Handles full 128-bit range correctly without __int128 support.
 * 2. Implements "Knuth's Algorithm D" subset (Short Division via Shift/Subtract).
 * 3. Constant-time characteristics not guaranteed, but suitable for FS math.
 */
hn4_u128_t hn4_u128_div_u64(hn4_u128_t a, uint64_t b) {
    hn4_u128_t q = {0, 0};
    
    /* Guard: Division by zero */
    if (b == 0) {
        /* In kernel/embedded, we might panic or return sentinel.
           Here we return 0xFF..FF to signal error/overflow. */
        q.lo = UINT64_MAX; 
        q.hi = UINT64_MAX;
        return q;
    }

#if defined(__SIZEOF_INT128__)
    /* Compiler Intrinsic Path (Fastest) */
    unsigned __int128 v = ((unsigned __int128)a.hi << 64) | a.lo;
    v /= b;
    q.hi = (uint64_t)(v >> 64);
    q.lo = (uint64_t)v;
#else
    /* Portable Long Division Path */
    
    /* Case 1: Divisor > Dividend (High part) */
    /* If high part is 0, this reduces to simple 64-bit division. */
    if (a.hi == 0) {
        q.lo = a.lo / b;
        q.hi = 0;
        return q;
    }

    /* Case 2: Full 128-bit division needed.
       We treat this as (hi * 2^64 + lo) / b.
       
       Algorithm:
       q.hi = a.hi / b
       r.hi = a.hi % b
       
       Now we need to divide (r.hi * 2^64 + a.lo) by b to get q.lo.
       This is essentially 128-bit / 64-bit where result fits in 64 bits (mostly).
       
       We can perform bitwise long division for the lower part.
    */
    
    q.hi = a.hi / b;
    uint64_t rem = a.hi % b;
    
    /* 
     * Optimization: If remainder is 0, we just divide the low part.
     * BUT we must handle the case where a.lo is large.
     */
    if (rem == 0) {
        q.lo = a.lo / b;
        return q;
    }

    /* 
     * Complex Case: (rem << 64 | a.lo) / b
     * We use a bit-shift restoration method.
     */
    uint64_t low = a.lo;
    uint64_t quotient_lo = 0;
    
    /* Iterate 64 bits */
     for (int i = 0; i < 64; i++) {
        /* 1. Capture the bit moving from Low -> High */
        uint64_t msb_low = (low >> 63);
        
        /* 2. Shift Low Part */
        low = (low << 1);
        
        /* 3. Capture the bit moving out of High (Overflow) */
        uint64_t rem_overflow = (rem >> 63);
        
        /* 4. Shift High Part (Remainder) and inject Low MSB */
        rem = (rem << 1) | msb_low;
        
        /* 5. Shift Quotient */
        quotient_lo = (quotient_lo << 1);

        /* 
         * 6. Check Logic:
         * If rem_overflow is set, the value effectively exceeds 64 bits (>= 2^64).
         * Since b is uint64_t, (2^64 + rem) is ALWAYS >= b.
         * The subtraction `rem - b` relies on unsigned underflow to mathematically
         * equal `(2^64 + rem) - b`.
         */
        if (rem_overflow || rem >= b) {
            rem -= b;
            quotient_lo |= 1;
        }
    }
    q.lo = quotient_lo;

#endif
    return q;
}

hn4_u128_t hn4_u128_mod(hn4_u128_t a, hn4_u128_t b) {
    /* Guard: Modulo by zero */
    if (b.lo == 0 && b.hi == 0) {
        /* Return 0 or Sentinel on DivByZero */
        hn4_u128_t zero = {0, 0};
        return zero;
    }

#if defined(__SIZEOF_INT128__)
    /* 
     * OPTIMIZED PATH: Compiler Intrinsics
     * Compiles to single 'div' instruction on x64
     */
    unsigned __int128 va = ((unsigned __int128)a.hi << 64) | a.lo;
    unsigned __int128 vb = ((unsigned __int128)b.hi << 64) | b.lo;
    
    unsigned __int128 res = va % vb;
    
    hn4_u128_t ret;
    ret.lo = (uint64_t)res;
    ret.hi = (uint64_t)(res >> 64);
    return ret;

#else
    /* 
     * PORTABLE PATH: Binary Long Division
     * Used when __int128 is unavailable.
     */
    
    /* Optimization: If Divisor > Dividend, remainder is Dividend */
    if (hn4_u128_cmp(b, a) > 0) return a;
    
    /* Optimization: If Divisor is equal, remainder is 0 */
    if (hn4_u128_cmp(b, a) == 0) {
        hn4_u128_t zero = {0, 0};
        return zero;
    }

    hn4_u128_t rem = {0, 0};
    
    /* Iterate from MSB (127) down to 0 */
    for (int i = 127; i >= 0; i--) {
        /* Shift remainder left by 1 */
        rem.hi = (rem.hi << 1) | (rem.lo >> 63);
        rem.lo = (rem.lo << 1);
        
        /* Get bit 'i' from Dividend */
        uint64_t bit;
        if (i >= 64) {
            bit = (a.hi >> (i - 64)) & 1;
        } else {
            bit = (a.lo >> i) & 1;
        }
        
        /* Inject bit into LSB of remainder */
        rem.lo |= bit;
        
        /* If Remainder >= Divisor, subtract Divisor */
        if (hn4_u128_cmp(rem, b) >= 0) {
            rem = hn4_u128_sub(rem, b);
        }
    }
    
    return rem;
#endif
}