/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      ECC Logic
 * HEADER:      hn4_ecc.h
 * STATUS:      OPTIMIZED / REVIEWED
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 */

#ifndef HN4_ECC_H
#define HN4_ECC_H

#include <stdint.h>
#include <stdbool.h>
#include "hn4.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * _calc_ecc_hamming
 * 
 * Calculates a 7-bit Hamming code + 1 global parity bit (SECDED).
 * 
 * Logic:
 * - Bits 0-63: Payload
 * - Encoded Result: [Global Parity] [7-bit Hamming Code]
 * 
 * @param data  The 64-bit payload to encode.
 * @return      Encoded ECC byte.
 */
uint8_t _calc_ecc_hamming(const uint64_t data);

/*
 * _ecc_check_and_fix
 * 
 * Verifies and Corrects (if possible) a 64-bit word against its ECC byte.
 * Detects Single Bit Errors (SEC) and Double Bit Errors (DED).
 * 
 * @param vol               Volume context (for logging/panic). Can be NULL.
 * @param raw_data          Data word read from memory.
 * @param raw_ecc           ECC byte read from memory.
 * @param out_data          [OUT] Corrected data.
 * @param out_was_corrected [OUT] (Optional) True if correction occurred.
 * 
 * @return HN4_OK on success (Clean or Corrected).
 * @return HN4_ERR_BITMAP_CORRUPT on uncorrectable error (DED).
 */
hn4_result_t _ecc_check_and_fix(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  uint64_t raw_data, 
    HN4_IN  uint8_t raw_ecc, 
    HN4_OUT uint64_t* out_data,
    HN4_OUT bool* out_was_corrected
);

#ifdef __cplusplus
}
#endif

#endif /* HN4_ECC_H */