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

hn4_addr_t hn4_addr_add(hn4_addr_t base, uint64_t inc) {
#ifdef HN4_USE_128BIT
    hn4_addr_t res = base;
    uint64_t old_lo = res.lo;
    
    res.lo += inc;
    
    /* Carry detection: If result is smaller than original, wrap-around occurred */
    if (res.lo < old_lo) {
        res.hi++; 
    }
    return res;
#else
    /* Standard arithmetic */
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
    /* Borrow logic: if result > start, we wrapped around */
    uint64_t borrow = (res.lo > a.lo) ? 1 : 0;
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
    /* Manual 64x64->128 decomposition for lo*b, plus hi*b */
    uint64_t a_lo = a.lo & 0xFFFFFFFF;
    uint64_t a_hi = a.lo >> 32;
    uint64_t b_lo = b & 0xFFFFFFFF;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t carry = (p1 & 0xFFFFFFFF) + (p0 >> 32);
    carry += (p2 & 0xFFFFFFFF); // Lower 32 of p2
    
    res.lo = a.lo * b; // CPU handles low part wrap correctly
    
    // High part accumulation
    res.hi = (a.hi * b) + p3 + (p1 >> 32) + (p2 >> 32) + (carry >> 32);
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
        /* Shift remainder left by 1, pulling in MSB of low */
        uint64_t msb_low = (low >> 63);
        
        /* Check overflow of remainder before shift */
        uint64_t rem_msb = (rem >> 63);
        
        rem = (rem << 1) | msb_low;
        low = (low << 1);
        
        quotient_lo = (quotient_lo << 1);
        
        /* If rem overflows or rem >= b, subtract b and set quotient bit */
        /* Note: rem_msb check handles the case where shifting resulted in a value > 64bit max 
           which conceptually wraps but we track logical value */
        if (rem_msb || rem >= b) {
            rem -= b;
            quotient_lo |= 1;
        }
    }
    q.lo = quotient_lo;

#endif
    return q;
}