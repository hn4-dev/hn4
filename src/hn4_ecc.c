/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      ECC Logic (Hamming/SECDED)
 * SOURCE:      hn4_ecc.c
 * VERSION:     Fixed (Bit 63 Parity Weight)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ALGORITHM:
 * Calculates a 7-bit Hamming code + 1 global parity bit (SECDED).
 * Used for the Armored Bitmap in RAM.
 */

#include "hn4_ecc.h"

//
// -----------------------------------------------------------------------------
// COMPILER INTRINSICS & ABSTRACTION
// -----------------------------------------------------------------------------
//

#if defined(_MSC_VER)
    #include <intrin.h>
    #define HN4_PARITY64(v) ((uint8_t)(__popcnt64(v) & 1))
#elif defined(__GNUC__) || defined(__clang__)
    #define HN4_PARITY64(v) ((uint8_t)(__builtin_parityll(v)))
#else
    /*
     * SWAR (SIMD Within A Register) Fallback.
     * Collapses 64-bit value to a single parity bit using parallel XORs.
     */
    static inline uint8_t _swar_parity(uint64_t v) {
        v ^= v >> 32; 
        v ^= v >> 16; 
        v ^= v >> 8; 
        v ^= v >> 4;
        return (0x6996 >> (v & 0xF)) & 1;
    }
    #define HN4_PARITY64(v) _swar_parity(v)
#endif

//
// -----------------------------------------------------------------------------
// CONSTANTS & PARITY MASKS
// -----------------------------------------------------------------------------
//

// Parity Check Matrices for Hamming(72,64)
static const uint64_t HN4_MASK_P1  = 0x5555555555555555ULL; // 0101...
static const uint64_t HN4_MASK_P2  = 0x3333333333333333ULL; // 0011...
static const uint64_t HN4_MASK_P4  = 0x0F0F0F0F0F0F0F0FULL; // 00001111...
static const uint64_t HN4_MASK_P8  = 0x00FF00FF00FF00FFULL; // 0000000011111111...
static const uint64_t HN4_MASK_P16 = 0x0000FFFF0000FFFFULL; 
static const uint64_t HN4_MASK_P32 = 0x00000000FFFFFFFFULL; 

//
// -----------------------------------------------------------------------------
// CORE IMPLEMENTATION
// -----------------------------------------------------------------------------
//

/*++

Routine Description:

    Calculates the 8-bit ECC syndrome for a 64-bit data word.
    
    The algorithm implements a modified Hamming code with an additional
    global parity bit to support SEC-DED (Single Error Correction, 
    Double Error Detection).

Arguments:

    data - The 64-bit payload to encode.

Return Value:

    Returns an 8-bit ECC byte structured as:
    [Bit 7: Global Parity] [Bits 0-6: Hamming Code]

Algorithm Notes:

    The logic handles a specific edge case regarding Bit 63 of the data.
    Standard Hamming logic covers bits 0-62 naturally. Bit 63 is treated
    as a special extension (p64) to fit within the power-of-two alignment.

--*/
uint8_t
_calc_ecc_hamming(
    const uint64_t data
    ) 
{
    //
    // Phase 1: Calculate Standard Hamming Parity Bits.
    // Use hardware population count (popcnt) to determine parity
    // against the standard masking matrices.
    //
    const uint8_t p1  = HN4_PARITY64(data & HN4_MASK_P1);
    const uint8_t p2  = HN4_PARITY64(data & HN4_MASK_P2);
    const uint8_t p4  = HN4_PARITY64(data & HN4_MASK_P4);
    const uint8_t p8  = HN4_PARITY64(data & HN4_MASK_P8);
    const uint8_t p16 = HN4_PARITY64(data & HN4_MASK_P16);
    const uint8_t p32 = HN4_PARITY64(data & HN4_MASK_P32);
    
    //
    // Phase 2: Handle Bit 63 Extension.
    // Bit 63 is the 64th bit, requiring the 6th parity position (2^6 = 64).
    //
    const uint8_t p64 = (uint8_t)((data >> 63) & 1);

    //
    // Phase 3: Construct the 7-bit Hamming Code.
    //
    const uint8_t hamming = (p1)       | 
                            (p2  << 1) | 
                            (p4  << 2) | 
                            (p8  << 3) | 
                            (p16 << 4) | 
                            (p32 << 5) | 
                            (p64 << 6);

    //
    // Phase 4: Calculate Global Parity (SEC-DED Support).
    //
    // CRITICAL LOGIC:
    // We calculate the parity of the Data XOR the parity of the Code.
    // However, we MUST mask out the p64 bit (Bit 6 of the hamming byte)
    // from the code parity calculation.
    // 
    // Theory of Operation:
    // Bit 63 of the data affects ONLY the p64 parity bit.
    // If Bit 63 flips:
    //   1. Data Parity flips.
    //   2. p64 flips.
    //
    // If p64 were included in the Code Parity calculation, Code Parity 
    // would also flip. The Global Parity (Data ^ Code) would see two flips 
    // and remain unchanged (XOR property). The decoder would then misinterpret
    // this as a Double Bit Error (DED) instead of a Single Bit Error (SEC).
    // 
    // By masking 0x3F (Bits 0-5), we exclude p64. Now if Bit 63 flips:
    //   1. Data Parity flips.
    //   2. Code Parity remains stable (derived only from p1..p32).
    // Result: Global Parity flips. Decoder correctly identifies SEC.
    //
    const uint8_t data_parity = HN4_PARITY64(data);
    const uint8_t code_parity = HN4_PARITY64((uint64_t)(hamming & 0x3F));

    //
    // Final Layout: [Global Parity] [Hamming(6)]
    //
    return (hamming << 1) | (data_parity ^ code_parity);
}