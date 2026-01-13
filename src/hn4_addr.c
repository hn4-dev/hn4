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

