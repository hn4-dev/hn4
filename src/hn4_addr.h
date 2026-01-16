/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_addr.h
 * MODULE:      Address Primitives
 * STATUS:      REFERENCE STANDARD (v4.2)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Helper definitions for abstracting 64-bit vs 128-bit addressing modes.
 * Ensures arithmetic correctness and type safety across the Quettabyte Horizon.
 */

#ifndef _HN4_ADDR_H_
#define _HN4_ADDR_H_

#include "hn4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ADDRESS MANIPULATION PRIMITIVES
 * ========================================================================= */

/**
 * hn4_addr_from_u64
 * Converts a raw 64-bit integer to the internal address type.
 * Handles 128-bit expansion (zero-extension) if HN4_USE_128BIT is active.
 *
 * @param val: 64-bit integer
 * @return:    hn4_addr_t representation
 */
hn4_addr_t hn4_addr_from_u64(uint64_t val);

/**
 * hn4_addr_to_u64
 * Extracts the 64-bit low part of an address.
 * SAFETY: Returns UINT64_MAX and logs CRIT if the address exceeds 64-bit range.
 *
 * @param addr: Address structure
 * @return:     Lower 64-bits or UINT64_MAX on overflow
 */
uint64_t hn4_addr_to_u64(hn4_addr_t addr);

/**
 * hn4_addr_add
 * Adds a 64-bit increment to an address with overflow/carry handling.
 *
 * @param base: Base address
 * @param inc:  64-bit increment
 * @return:     New address
 */
hn4_addr_t hn4_addr_add(hn4_addr_t base, uint64_t inc);

/* =========================================================================
 * SEMANTIC ALIASES
 * ========================================================================= */

/**
 * hn4_lba_from_blocks
 * Semantic wrapper for converting block counts to LBA types.
 * Purely for code readability to distinguish sizes from offsets.
 */
hn4_addr_t hn4_lba_from_blocks(uint64_t blocks);

/**
 * hn4_lba_from_sectors
 * Semantic wrapper for converting raw sector indices to LBA types.
 * Use this when the input is calculated via (byte_offset / sector_size).
 */
hn4_addr_t hn4_lba_from_sectors(uint64_t sectors);

int hn4_u128_cmp(hn4_u128_t a, hn4_u128_t b);
hn4_u128_t hn4_u128_sub(hn4_u128_t a, hn4_u128_t b);
hn4_u128_t hn4_u128_from_u64(uint64_t v);

/**
 * hn4_addr_try_u64
 * Silent safe downcast. 
 * Returns true if successful, false if address exceeds 64-bit range.
 * Does NOT log critical errors (unlike hn4_addr_to_u64).
 */
bool hn4_addr_try_u64(hn4_addr_t addr, uint64_t* out);


/**
 * hn4_u128_mod
 * Calculates the remainder of a / b (128-bit modulo).
 * Used for aligning capacity to zone boundaries in HN4_USE_128BIT mode.
 */
hn4_u128_t hn4_u128_mod(hn4_u128_t a, hn4_u128_t b);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_ADDR_H_ */