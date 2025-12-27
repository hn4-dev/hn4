/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      ECC Logic (Hamming/SECDED)
 * SOURCE:      hn4_ecc.c
 * VERSION:     Optimized / Reviewed
 *
 * COPYRIGHT:   (c) 2025 The Hydra-Nexus Team. All rights reserved.
 *
 * DESCRIPTION:
 * Provides Error Correction Code (ECC) primitives for HN4 storage slabs.
 * Implements SECDED (Single Error Correction, Double Error Detection)
 * using 7-bit Hamming codes plus a global parity bit.
 *
 * OPTIMIZATIONS:
 * 1. Hardware Intrinsics: Uses POPCNT/Parity instructions (SSE4.2/ABM).
 * 2. Parallel Bitwise Ops: Vectorizes Hamming mask applications.
 * 3. Branchless Logic: Avoids conditional jumps for parity calculation.
 */

#include "hn4_ecc.h"

/* 
 * --- PLATFORM INTRINSICS CONFIGURATION --- 
 * Priority: Hardware Instructions > Software SWAR
 */
#if defined(_MSC_VER)
    #include <intrin.h>
    /* MSVC typically maps __popcnt to hardware POPCNT instruction */
    #define HN4_PARITY64(v) ((uint8_t)(__popcnt64(v) & 1))
#elif defined(__GNUC__) || defined(__clang__)
    /* GCC/Clang builtin maps to parity flag or POPCNT instruction */
    #define HN4_PARITY64(v) ((uint8_t)(__builtin_parityll(v)))
#else
    /* Fallback: Classic SWAR approach (5-stage shift-xor) */
    static inline uint8_t _swar_parity(uint64_t v) {
        v ^= v >> 32; v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
        return (0x6996 >> (v & 0xF)) & 1;
    }
    #define HN4_PARITY64(v) _swar_parity(v)
#endif

/*
 * --- HAMMING MASKS ---
 * Pre-defined constants for Hamming coverage areas.
 */
#define HN4_MASK_P1  0x5555555555555555ULL /* Bits 1, 3, 5... */
#define HN4_MASK_P2  0x3333333333333333ULL /* Bits 2-3, 6-7... */
#define HN4_MASK_P4  0x0F0F0F0F0F0F0F0FULL /* Bits 4-7, 12-15... */
#define HN4_MASK_P8  0x00FF00FF00FF00FFULL /* Bits 8-15, 24-31... */
#define HN4_MASK_P16 0x0000FFFF0000FFFFULL /* Bits 16-31, 48-63... */
#define HN4_MASK_P32 0x00000000FFFFFFFFULL /* Bits 32-63... */

uint8_t _calc_ecc_hamming(const uint64_t data) 
{
    /* 
     * STEP 1: Calculate Coverage Parity
     * The compiler will vectorize constant loads and parallelize 
     * these operations on OOO (Out-of-Order) CPUs.
     */
    const uint8_t p1  = HN4_PARITY64(data & HN4_MASK_P1);
    const uint8_t p2  = HN4_PARITY64(data & HN4_MASK_P2);
    const uint8_t p4  = HN4_PARITY64(data & HN4_MASK_P4);
    const uint8_t p8  = HN4_PARITY64(data & HN4_MASK_P8);
    const uint8_t p16 = HN4_PARITY64(data & HN4_MASK_P16);
    const uint8_t p32 = HN4_PARITY64(data & HN4_MASK_P32);

    /* 
     * LEGACY LOGIC PRESERVATION:
     * Bit 63 is treated uniquely in HN4 protocol.
     */
    const uint8_t p64 = (uint8_t)((data >> 63) & 1);

    /*
     * STEP 2: Construct Hamming Byte
     * Accumulate parity bits into specific positions.
     */
    const uint8_t hamming = (p1)      | 
                            (p2  << 1) | 
                            (p4  << 2) | 
                            (p8  << 3) | 
                            (p16 << 4) | 
                            (p32 << 5) | 
                            (p64 << 6);

    /*
     * STEP 3: Global Parity (SECDED Support)
     * Calculate parity of the raw data AND the calculated Hamming code.
     * Note: Casting hamming to u64 avoids register width stalls.
     */
    const uint8_t data_parity = HN4_PARITY64(data);
    const uint8_t code_parity = HN4_PARITY64((uint64_t)hamming);

    /* 
     * Result Structure: [Global Parity bit] [7-bit Hamming]
     */
    return (hamming << 1) | (data_parity ^ code_parity);
}
