/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_endians.h
 * STATUS:      REFERENCE STANDARD (v4.2)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * PURPOSE:
 * High-Performance, Type-Safe Endianness Normalization.
 * HN4 is natively LITTLE ENDIAN (LE).
 */

#ifndef _HN4_ENDIANS_H_
#define _HN4_ENDIANS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 0. ENVIRONMENT SAFETY & ASSERTIONS
 * ========================================================================= */

/* Ensure HN4_INLINE is defined safely for headers */
#ifndef HN4_INLINE
    #if defined(_MSC_VER)
        #define HN4_INLINE static __forceinline
    #else
        #define HN4_INLINE static inline __attribute__((always_inline))
    #endif
#endif

/* Verify Environment Assumptions */
#if CHAR_BIT != 8
    #error "HN4 requires 8-bit bytes."
#endif

/* Include the main header to get hn4_u128_t definition */
#include "hn4.h"

/* Verify Struct Layout explicitly */
_Static_assert(sizeof(uint8_t) == 1, "uint8_t must be 1 byte");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t must be 2 bytes");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
_Static_assert(sizeof(hn4_u128_t) == 16, "hn4_u128_t must be 128 bits");
_Static_assert(offsetof(hn4_u128_t, lo) == 0, "hn4_u128_t.lo must be at offset 0");

/* =========================================================================
 * 1. ENDIANNESS DETECTION (STRICT)
 * ========================================================================= */

/* Clear previous definitions to prevent collision */
#undef HN4_IS_BIG_ENDIAN
#undef HN4_IS_LITTLE_ENDIAN

/* GCC / Clang / ICC */
#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || \
    (defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__))
    #define HN4_IS_BIG_ENDIAN 1
    #define HN4_IS_LITTLE_ENDIAN 0

#elif (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || \
    (defined(__LITTLE_ENDIAN__) && !defined(__BIG_ENDIAN__)) || \
    defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64)
    #define HN4_IS_BIG_ENDIAN 0
    #define HN4_IS_LITTLE_ENDIAN 1

#else
    #error "HN4: Endianness could not be strictly determined. Define __LITTLE_ENDIAN__ or __BIG_ENDIAN__ manually."
#endif

/* =========================================================================
 * 2. INTRINSICS & OPTIMIZED SWAPS
 * ========================================================================= */

/* MSVC Intrinsics */
#if defined(_MSC_VER)
    #include <stdlib.h>
    #include <intrin.h>
    #define HN4_BSWAP16_IMPL(x) _byteswap_ushort(x)
    #define HN4_BSWAP32_IMPL(x) _byteswap_ulong(x)
    #define HN4_BSWAP64_IMPL(x) _byteswap_uint64(x)

/* GCC / Clang Intrinsics */
#elif defined(__GNUC__) || defined(__clang__)
    #define HN4_BSWAP16_IMPL(x) __builtin_bswap16(x)
    #define HN4_BSWAP32_IMPL(x) __builtin_bswap32(x)
    #define HN4_BSWAP64_IMPL(x) __builtin_bswap64(x)

/* Generic Fallback (Bitmasks) */
#else
    #warning "HN4: No compiler intrinsics for BSWAP found. Using bitmask fallback (slower)."
    #define HN4_USE_BITMASK_FALLBACK 1
#endif

/* --- Fallback Implementations --- */
#ifdef HN4_USE_BITMASK_FALLBACK

HN4_INLINE uint16_t hn4_bswap16_mask(uint16_t x) {
    return (uint16_t)(((x & 0x00FFU) << 8) | ((x & 0xFF00U) >> 8));
}

HN4_INLINE uint32_t hn4_bswap32_mask(uint32_t x) {
    return  ((x & 0x000000FFUL) << 24) |
            ((x & 0x0000FF00UL) << 8)  |
            ((x & 0x00FF0000UL) >> 8)  |
            ((x & 0xFF000000UL) >> 24);
}

HN4_INLINE uint64_t hn4_bswap64_mask(uint64_t x) {
    return  ((x & 0x00000000000000FFULL) << 56) |
            ((x & 0x000000000000FF00ULL) << 40) |
            ((x & 0x0000000000FF0000ULL) << 24) |
            ((x & 0x00000000FF000000ULL) << 8)  |
            ((x & 0x000000FF00000000ULL) >> 8)  |
            ((x & 0x0000FF0000000000ULL) >> 24) |
            ((x & 0x00FF000000000000ULL) >> 40) |
            ((x & 0xFF00000000000000ULL) >> 56);
}

#define HN4_BSWAP16_IMPL(x) hn4_bswap16_mask(x)
#define HN4_BSWAP32_IMPL(x) hn4_bswap32_mask(x)
#define HN4_BSWAP64_IMPL(x) hn4_bswap64_mask(x)

#endif /* HN4_USE_BITMASK_FALLBACK */

/* =========================================================================
 * 3. TYPE-SAFE SWAP PRIMITIVES
 * ========================================================================= */

HN4_INLINE uint16_t hn4_bswap16(uint16_t x) { return HN4_BSWAP16_IMPL(x); }
HN4_INLINE uint32_t hn4_bswap32(uint32_t x) { return HN4_BSWAP32_IMPL(x); }
HN4_INLINE uint64_t hn4_bswap64(uint64_t x) { return HN4_BSWAP64_IMPL(x); }

/*
 * hn4_bswap128
 * NOTE: hn4_u128_t is a STRUCT, not an __int128.
 *
 * Memory Layout (LE): [lo (8 bytes)] [hi (8 bytes)]
 *
 * To convert to native CPU format while preserving struct member access:
 * - We swap the bytes of 'lo'.
 * - We swap the bytes of 'hi'.
 * - We DO NOT swap the order of 'lo' and 'hi' fields.
 *
 * Why? Because code accessing 'id.lo' expects the "lower 64 bits of logic".
 * If we swapped fields, 'id.lo' would contain the 'hi' data on BE machines.
 */
HN4_INLINE hn4_u128_t hn4_bswap128(hn4_u128_t val) {
    hn4_u128_t res;
    res.lo = hn4_bswap64(val.lo);
    res.hi = hn4_bswap64(val.hi);
    return res;
}

/* =========================================================================
 * 4. CONVERSION FUNCTIONS (LE <-> CPU)
 * ========================================================================= */

/*
 * These are implemented as static inline functions to ensure:
 * 1. Proper type checking (no macro narrowing).
 * 2. Single evaluation of arguments.
 * 3. Debuggability.
 */

#if HN4_IS_LITTLE_ENDIAN
    /* --- NATIVE LE (ZERO COST) --- */
    HN4_INLINE uint16_t hn4_cpu_to_le16(uint16_t x) { return x; }
    HN4_INLINE uint16_t hn4_le16_to_cpu(uint16_t x) { return x; }

    HN4_INLINE uint32_t hn4_cpu_to_le32(uint32_t x) { return x; }
    HN4_INLINE uint32_t hn4_le32_to_cpu(uint32_t x) { return x; }

    HN4_INLINE uint64_t hn4_cpu_to_le64(uint64_t x) { return x; }
    HN4_INLINE uint64_t hn4_le64_to_cpu(uint64_t x) { return x; }

    HN4_INLINE hn4_u128_t hn4_cpu_to_le128(hn4_u128_t x) { return x; }
    HN4_INLINE hn4_u128_t hn4_le128_to_cpu(hn4_u128_t x) { return x; }

#else
    /* --- NATIVE BE (SWAP REQUIRED) --- */
    HN4_INLINE uint16_t hn4_cpu_to_le16(uint16_t x) { return hn4_bswap16(x); }
    HN4_INLINE uint16_t hn4_le16_to_cpu(uint16_t x) { return hn4_bswap16(x); }

    HN4_INLINE uint32_t hn4_cpu_to_le32(uint32_t x) { return hn4_bswap32(x); }
    HN4_INLINE uint32_t hn4_le32_to_cpu(uint32_t x) { return hn4_bswap32(x); }

    HN4_INLINE uint64_t hn4_cpu_to_le64(uint64_t x) { return hn4_bswap64(x); }
    HN4_INLINE uint64_t hn4_le64_to_cpu(uint64_t x) { return hn4_bswap64(x); }

    HN4_INLINE hn4_u128_t hn4_cpu_to_le128(hn4_u128_t x) { return hn4_bswap128(x); }
    HN4_INLINE hn4_u128_t hn4_le128_to_cpu(hn4_u128_t x) { return hn4_bswap128(x); }

#endif

/* =========================================================================
 * 5. BULK CONVERSION PROTOTYPES
 * ========================================================================= */

/**
 * Bulk conversion routines.
 * @param data Pointer to the array of 64-bit integers.
 * @param count NUMBER OF ELEMENTS (not bytes).
 *
 * NOTE: 'data' MUST be 8-byte aligned. Unaligned access will cause faults
 * on strict architectures (ARMv7, RISC-V).
 */
void hn4_bulk_le64_to_cpu(uint64_t* __restrict data, size_t count);
void hn4_bulk_cpu_to_le64(uint64_t* __restrict data, size_t count);

/**
 * Runtime Sanity Check.
 * MUST be called during driver initialization.
 * Verifies that the compiler's view of endianness matches the CPU.
 * @return true if safe, false if configuration mismatch.
 */
bool hn4_endian_sanity_check(void);

/* =========================================================================
 * 6. STRUCTURE CONVERSION PROTOTYPES
 * ========================================================================= */

/**
 * Converts Superblock between CPU and Disk (LE) formats.
 * Works in-place or copy.
 */
void hn4_sb_to_cpu(hn4_superblock_t* sb);
void hn4_sb_to_disk(const hn4_superblock_t* src, hn4_superblock_t* dst);

/**
 * Converts Epoch Header between CPU and Disk (LE) formats.
 */
void hn4_epoch_to_cpu(hn4_epoch_header_t* ep);
void hn4_epoch_to_disk(const hn4_epoch_header_t* src, hn4_epoch_header_t* dst);

/* =========================================================================
 * 6. LOGICAL ADDRESS TRANSLATION (SHARED)
 * ========================================================================= */

/**
 * hn4_addr_to_cpu / hn4_addr_to_le
 * Abstracts the 128-bit vs 64-bit address conversion logic.
 */
HN4_INLINE hn4_addr_t hn4_addr_to_cpu(hn4_addr_t v) {
#ifdef HN4_USE_128BIT
    hn4_addr_t r;
    r.lo = hn4_le64_to_cpu(v.lo);
    r.hi = hn4_le64_to_cpu(v.hi);
    return r;
#else
    return hn4_le64_to_cpu(v);
#endif
}

HN4_INLINE hn4_addr_t hn4_addr_to_le(hn4_addr_t v) {
#ifdef HN4_USE_128BIT
    hn4_addr_t r;
    r.lo = hn4_cpu_to_le64(v.lo);
    r.hi = hn4_cpu_to_le64(v.hi);
    return r;
#else
    return hn4_cpu_to_le64(v);
#endif
}

/* =========================================================================
 * 7. UUID & INTEGRITY HELPERS (SHARED)
 * ========================================================================= */

/**
 * Compares two UUIDs for equality.
 */
HN4_INLINE bool hn4_uuid_equal(hn4_u128_t a, hn4_u128_t b) {
    return (a.lo == b.lo) && (a.hi == b.hi);
}

/**
 * Calculates Superblock CRC (Pure Data, No IO).
 * Wraps the offsetof logic so it isn't repeated 3 times.
 */
uint32_t hn4_sb_calc_crc(const hn4_superblock_t* sb);

/**
 * Calculates Epoch Header CRC.
 */
uint32_t hn4_epoch_calc_crc(const hn4_epoch_header_t* ep);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_ENDIANS_H_ */