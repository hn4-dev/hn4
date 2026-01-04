/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Address Primitives
 * SOURCE:      hn4_addr.c
 * STATUS:      HARDENED / REVIEWED
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