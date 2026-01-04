/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      ECC Logic (Hamming/SECDED)
 * SOURCE:      hn4_ecc.c
 * VERSION:     Fixed (Bit 63 Parity Weight)
 */

#include "hn4_ecc.h"

/* --- PLATFORM INTRINSICS --- */
#if defined(_MSC_VER)
    #include <intrin.h>
    #define HN4_PARITY64(v) ((uint8_t)(__popcnt64(v) & 1))
#elif defined(__GNUC__) || defined(__clang__)
    #define HN4_PARITY64(v) ((uint8_t)(__builtin_parityll(v)))
#else
    static inline uint8_t _swar_parity(uint64_t v) {
        v ^= v >> 32; v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
        return (0x6996 >> (v & 0xF)) & 1;
    }
    #define HN4_PARITY64(v) _swar_parity(v)
#endif

static const uint64_t HN4_MASK_P1  = 0x5555555555555555ULL; 
static const uint64_t HN4_MASK_P2  = 0x3333333333333333ULL; 
static const uint64_t HN4_MASK_P4  = 0x0F0F0F0F0F0F0F0FULL; 
static const uint64_t HN4_MASK_P8  = 0x00FF00FF00FF00FFULL; 
static const uint64_t HN4_MASK_P16 = 0x0000FFFF0000FFFFULL; 
static const uint64_t HN4_MASK_P32 = 0x00000000FFFFFFFFULL; 

uint8_t _calc_ecc_hamming(const uint64_t data) 
{
    const uint8_t p1  = HN4_PARITY64(data & HN4_MASK_P1);
    const uint8_t p2  = HN4_PARITY64(data & HN4_MASK_P2);
    const uint8_t p4  = HN4_PARITY64(data & HN4_MASK_P4);
    const uint8_t p8  = HN4_PARITY64(data & HN4_MASK_P8);
    const uint8_t p16 = HN4_PARITY64(data & HN4_MASK_P16);
    const uint8_t p32 = HN4_PARITY64(data & HN4_MASK_P32);
    
    /* Legacy/Extension bit for position 63 */
    const uint8_t p64 = (uint8_t)((data >> 63) & 1);

    const uint8_t hamming = (p1)      | 
                            (p2  << 1) | 
                            (p4  << 2) | 
                            (p8  << 3) | 
                            (p16 << 4) | 
                            (p32 << 5) | 
                            (p64 << 6);

    /*
     * We calculate the parity of the Hamming Code to support SEC-DED.
     * However, we MUST mask out the p64 bit (Bit 6) from this calculation.
     * 
     * Why?
     * Bit 63 of data affects ONLY p64. If Bit 63 flips:
     * 1. Data Parity flips.
     * 2. p64 flips.
     * If p64 is included in Code Parity, Code Parity also flips.
     * Result: Global Parity (Data ^ Code) sees two flips and remains unchanged.
     * The decoder then interprets the non-zero syndrome as a Double Bit Error.
     * 
     * By masking 0x3F (Bits 0-5), we exclude p64. Now if Bit 63 flips:
     * 1. Data Parity flips.
     * 2. Code Parity remains stable.
     * Result: Global Parity flips. Decoder sees Single Bit Error (Correct).
     */
    const uint8_t data_parity = HN4_PARITY64(data);
    const uint8_t code_parity = HN4_PARITY64((uint64_t)(hamming & 0x3F));

    return (hamming << 1) | (data_parity ^ code_parity);
}