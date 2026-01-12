/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      ECC Logic (Hamming/SECDED)
 * SOURCE:      hn4_ecc.c
 * VERSION:     Fixed (Bit 63 Parity Weight + Shared Validation Logic)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ALGORITHM:
 * Calculates a 7-bit Hamming code + 1 global parity bit (SECDED).
 * Used for the Armored Bitmap in RAM.
 */

#include "hn4.h"
#include "hn4_ecc.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"
#include <string.h>
#include <stdatomic.h>

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
// LOOKUP TABLE STATE (For Syndrome Decoding)
// -----------------------------------------------------------------------------
//

/* STATIC LUT STATE */
static int8_t _hn4_ecc_lut[256];
static atomic_bool _hn4_ecc_lut_ready = false;

/* One-time initializer */
static void _init_ecc_lut(void) {
    /* Use stack buffer to avoid half-initialized global reads */
    int8_t tmp[256];
    memset(tmp, -1, sizeof(tmp));

    /* 
     * Map Syndrome -> Bit Index
     * Logic: If Data Bit 'i' flips, the Syndrome (Calc ^ Raw) will be 
     * exactly equal to _calc_ecc_hamming(1 << i).
     */
    for (int i = 0; i < 64; i++) {
        uint8_t syndrome = _calc_ecc_hamming(1ULL << i);
        tmp[syndrome] = (int8_t)i;
    }

    /* Atomic Commit */
    memcpy(_hn4_ecc_lut, tmp, 256);
    atomic_store_explicit(&_hn4_ecc_lut_ready, true, memory_order_release);
}

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
    // Mask out p64 from hamming parity calculation (See Logic in hn4_ecc.c header).
    //
    const uint8_t data_parity = HN4_PARITY64(data);
    const uint8_t code_parity = HN4_PARITY64((uint64_t)(hamming & 0x3F));

    //
    // Final Layout: [Global Parity] [Hamming(6)]
    //
    return (hamming << 1) | (data_parity ^ code_parity);
}

/*++

Routine Description:

    Verifies and Corrects (if possible) a 64-bit word against its ECC byte.
    
    Logic:
    - ODD Parity Error (P=1) + Syndrome!=0 -> SEC (Single Error Correctable)
    - EVEN Parity Error (P=0) + Syndrome!=0 -> DED (Double Error Detection - Fatal)
    - Syndrome==0 -> Clean

Arguments:

    vol - Volume context (for logging/panic flags). Can be NULL if logging skipped.
    raw_data - The data word read from memory.
    raw_ecc - The ECC byte read from memory.
    out_data - Pointer to receive the corrected (or original) data.
    out_was_corrected - (Optional) Set to true if a bit flip was fixed.

Return Value:

    HN4_OK - Data is valid (clean or corrected).
    HN4_ERR_BITMAP_CORRUPT - DED Detected (Uncorrectable).

--*/
HN4_HOT
hn4_result_t _ecc_check_and_fix(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  uint64_t raw_data, 
    HN4_IN  uint8_t raw_ecc, 
    HN4_OUT uint64_t* out_data,
    HN4_OUT bool* out_was_corrected
) {
    uint8_t calc_ecc = _calc_ecc_hamming(raw_data);
    uint8_t diff = calc_ecc ^ raw_ecc;

    /* PREDICTION: 99.9% of reads are clean */
    if (HN4_LIKELY(diff == 0)) {
        *out_data = raw_data;
        if (out_was_corrected) *out_was_corrected = false;
        return HN4_OK;
    }

    /* PREDICTION: Initialization happens once */
    if (HN4_UNLIKELY(!atomic_load_explicit(&_hn4_ecc_lut_ready, memory_order_acquire))) {
        _init_ecc_lut();
    }

    /* Case 1: Parity Bit Only flipped (diff == 1) */
    if (diff == 1) {
        *out_data = raw_data;
        /* Technically the ECC byte was wrong, data is fine. We mark corrected to force writeback of ECC. */
        if (out_was_corrected) *out_was_corrected = true;
        return HN4_OK;
    }

    /* Case 2: Hamming Parity Bit flipped (Power of 2) */
    if ((diff & (diff - 1)) == 0) {
         *out_data = raw_data; 
         /* ECC byte corruption (one of the hamming bits). Data is fine. */
         if (out_was_corrected) *out_was_corrected = true;
         return HN4_OK;
    }

    /* Case 3: Data Bit Flip (Lookup Syndrome) */
    int8_t bit_idx = _hn4_ecc_lut[diff];

    if (bit_idx >= 0) {
        *out_data = raw_data ^ (1ULL << bit_idx);
        if (out_was_corrected) *out_was_corrected = true;
        return HN4_OK;
    }

    /* Case 4: Double Bit Error (DED) or worse */
    if (vol) {
        HN4_LOG_CRIT("ECC: DED Detected! Syndrome 0x%02X", diff);
        atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_PANIC);
    }
    
    *out_data = 0; /* Safety zero */
    if (out_was_corrected) *out_was_corrected = false;
    return HN4_ERR_BITMAP_CORRUPT;
}