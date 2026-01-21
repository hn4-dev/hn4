/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Integrity Primitives (CRC32/CRC64)
 * SOURCE:      hn4_crc.c
 * VERSION:     Reference Optimized (Slice-by-8)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * OPTIMIZATIONS:
 * 1. Slice-by-8 Strategy: Processes 8 bytes per step.
 * 2. Instruction Pipelining: Interleaves load/calculate ops.
 * 3. Prefetching: Explicitly pulls future cache lines.
 */

#include "hn4_crc.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --- CONFIGURATION & MACROS --- */

/* Only enable fast slice-8 on Little Endian (x86, ARM, RISC-V) */
#if (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || \
    (defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86) || defined(_M_ARM64)))
    #define HN4_IS_LE 1
#else
    #define HN4_IS_LE 0
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define HN4_LIKELY(x)     __builtin_expect(!!(x), 1)
    #define HN4_RESTRICT      __restrict__
    #define HN4_ALIGNED(x)    __attribute__((aligned(x)))
    #define HN4_PREFETCH(p)   __builtin_prefetch(p)
#elif defined(_MSC_VER)
    #define HN4_LIKELY(x)     (x)
    #define HN4_RESTRICT      __restrict
    #define HN4_ALIGNED(x)    __declspec(align(x))
    #define HN4_PREFETCH(p)   _mm_prefetch((char*)(p), _MM_HINT_T0)
    #include <xmmintrin.h>
#else
    #define HN4_LIKELY(x)     (x)
    #define HN4_RESTRICT
    #define HN4_ALIGNED(x)
    #define HN4_PREFETCH(p)
#endif

/* Align tables to 128 bytes to ensure no split cache lines on any arch */
HN4_ALIGNED(128) static uint32_t hn4_table32[8][256];

#ifdef HN4_CRC64_ENABLE
HN4_ALIGNED(128) static uint64_t hn4_table64[8][256];
#endif

/* --- INITIALIZATION --- */

static const uint32_t POLY32 = 0xEDB88320U;
static const uint64_t POLY64 = 0xC96C5795D7870F42ULL;

void hn4_crc_init(void) {
    /* Init CRC32 tables */
    for (int i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) 
            c = (c >> 1) ^ ((c & 1) ? POLY32 : 0);
        hn4_table32[0][i] = c;
    }

    for (int i = 0; i < 256; i++) {
        for (int k = 1; k < 8; k++) {
            uint32_t prev = hn4_table32[k - 1][i];
            hn4_table32[k][i] = (prev >> 8) ^ hn4_table32[0][prev & 0xFF];
        }
    }

#ifdef HN4_CRC64_ENABLE
    /* Init CRC64 tables */
    for (int i = 0; i < 256; i++) {
        uint64_t c = i;
        for (int k = 0; k < 8; k++) 
            c = (c >> 1) ^ ((c & 1) ? POLY64 : 0);
        hn4_table64[0][i] = c;
    }

    for (int i = 0; i < 256; i++) {
        for (int k = 1; k < 8; k++) {
            uint64_t prev = hn4_table64[k - 1][i];
            hn4_table64[k][i] = (prev >> 8) ^ hn4_table64[0][prev & 0xFF];
        }
    }
#endif
}

/* --- CRC32 IMPLEMENTATION --- */

uint32_t hn4_crc32(uint32_t seed, const void * HN4_RESTRICT buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = ~seed;

#if HN4_IS_LE
    /* 
     * MAIN LOOP (Slice-by-8 + Prefetch) 
     * Process 8 bytes at a time.
     */
    while (HN4_LIKELY(len >= 8)) {
        uint32_t one, two;

        /* Prefetch next cacheline (offset 320 is approx 5 loops ahead) */
        HN4_PREFETCH(p + 320);

        /* Load 8 bytes unaligned */
        memcpy(&one, p, 4);
        memcpy(&two, p + 4, 4);

        /* XOR CRC into first 4 bytes */
        one ^= crc;

        /* 
         * ILP: The CPU can perform these 8 table lookups in parallel 
         * because they don't depend on each other, only on 'one' and 'two'.
         */
        crc = hn4_table32[7][one & 0xFF] ^
              hn4_table32[6][(one >> 8) & 0xFF] ^
              hn4_table32[5][(one >> 16) & 0xFF] ^
              hn4_table32[4][one >> 24] ^
              hn4_table32[3][two & 0xFF] ^
              hn4_table32[2][(two >> 8) & 0xFF] ^
              hn4_table32[1][(two >> 16) & 0xFF] ^
              hn4_table32[0][two >> 24];

        p += 8;
        len -= 8;
    }
#endif

    /* TAIL / FALLBACK (Byte-wise) */
    while (len--) {
        crc = hn4_table32[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

/* --- CRC64 IMPLEMENTATION --- */

#ifdef HN4_CRC64_ENABLE
uint64_t hn4_crc64(uint64_t seed, const void * HN4_RESTRICT buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t crc = ~seed;

#if HN4_IS_LE
    while (HN4_LIKELY(len >= 8)) {
        uint64_t d;

        HN4_PREFETCH(p + 320);

        /* Load 8 bytes */
        memcpy(&d, p, 8);
        
        /* XOR entire state */
        d ^= crc;

        /* Slice-by-8 for 64-bit */
        crc = hn4_table64[7][d & 0xFF] ^
              hn4_table64[6][(d >> 8) & 0xFF] ^
              hn4_table64[5][(d >> 16) & 0xFF] ^
              hn4_table64[4][(d >> 24) & 0xFF] ^
              hn4_table64[3][(d >> 32) & 0xFF] ^
              hn4_table64[2][(d >> 40) & 0xFF] ^
              hn4_table64[1][(d >> 48) & 0xFF] ^
              hn4_table64[0][d >> 56];

        p += 8;
        len -= 8;
    }
#endif

    while (len--) {
        crc = hn4_table64[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}
#endif