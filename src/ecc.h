/*
 * HYDRA-NEXUS 4 (HN4) - ECC LOGIC
 * SOURCE FILE: hn4_ecc.h
 * STATUS:      OPTIMIZED / REVIEWED
 * ARCHITECT:   SENIOR ENG
 */

#ifndef HN4_ECC_H
#define HN4_ECC_H

#include <stdint.h>
#include "hn4.h"

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

#endif /* HN4_ECC_H */
