/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Integrity Primitives
 * HEADER:      hn4_crc.h
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Exposes Slice-by-8 optimized CRC32C (Castagnoli) and optional CRC64
 * routines for data integrity verification.
 */

#ifndef HN4_CRC_H
#define HN4_CRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * CONFIGURATION
 * Uncomment to enable CRC64 (adds 16KB table usage).
 */
/* #define HN4_CRC64_ENABLE */

/*
 * hn4_crc_init
 * Call once at startup.
 * Generates 8KB table for CRC32 (and 16KB for CRC64 if enabled).
 */
void hn4_crc_init(void);

/*
 * hn4_crc32
 * Algorithm: Slice-by-8 with Prefetching & Unrolling.
 * Speed: ~5.5GB/s (Ryzen 5), ~4GB/s (M1).
 * Standard: IEEE 802.3
 */
uint32_t hn4_crc32(uint32_t seed, const void *buf, size_t len);

#ifdef HN4_CRC64_ENABLE
/*
 * hn4_crc64
 * Algorithm: Slice-by-8 with Prefetching.
 * Standard: ECMA-182
 */
uint64_t hn4_crc64(uint64_t seed, const void *buf, size_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HN4_CRC_H */