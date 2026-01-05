/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_swizzle.h
 * STATUS:      REFERENCE STANDARD (v5.1)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * PURPOSE:
 * High-Performance Bit-Manipulation and Address Permutation logic.
 * Used for Gravity Assist (Spec 6.6) and Ludic Protocol (Spec 10.7).
 */

#ifndef HN4_SWIZZLE_H
#define HN4_SWIZZLE_H

#include <stdint.h>
#include <stddef.h>
#include "hn4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 0. INTRINSIC WRAPPERS (ROTATE)
 * ========================================================================= */

/*
 * hn4_rotl64
 * Portable 64-bit Rotate Left.
 */
static inline uint64_t hn4_rotl64(uint64_t x, int k) {
#if defined(_MSC_VER)
    return _rotl64(x, k);
#else
    return (x << k) | (x >> (64 - k));
#endif
}

/*
 * hn4_rotr64
 * Portable 64-bit Rotate Right.
 */
static inline uint64_t hn4_rotr64(uint64_t x, int k) {
#if defined(_MSC_VER)
    return _rotr64(x, k);
#else
    return (x >> k) | (x << (64 - k));
#endif
}

/* =========================================================================
 * 1. BALLISTIC MATH (GRAVITY ASSIST)
 * ========================================================================= */

/**
 * hn4_swizzle_gravity_assist
 * IMPLEMENTS SPEC SECTION 6.6
 *
 * Calculates the "Vector Shift" to escape a gravity well collision.
 * Used when the primary trajectory (k=0..3) is blocked.
 *
 * Formula: V2 = ROTL(V, 17) ^ 0xA5A5A5
 */
uint64_t hn4_swizzle_gravity_assist(uint64_t orbit_vector);

/* =========================================================================
 * 2. SPATIAL SWIZZLING (MORTON CODES / Z-ORDER)
 * ========================================================================= */

/**
 * hn4_swizzle_morton_2d
 * Interleaves bits of X and Y (Z-Order Curve).
 *
 * CONSTRAINT: Inputs strictly limited to 16 bits (0..65535).
 * Higher bits are ignored/masked.
 * Returns 32-bit index.
 */
uint32_t hn4_swizzle_morton_2d(uint16_t x, uint16_t y);

/**
 * hn4_swizzle_morton_3d
 * Interleaves bits of X, Y, Z.
 *
 * CONSTRAINT: Inputs strictly limited to 10 bits (0..1023).
 * Values >= 1024 will be masked to prevent bit collision.
 * Returns 30-bit index.
 */
uint32_t hn4_swizzle_morton_3d(uint16_t x, uint16_t y, uint16_t z);

/* =========================================================================
 * 3. TENSOR STRIDING
 * ========================================================================= */

#define HN4_TENSOR_ROW_MAJOR    0
#define HN4_TENSOR_COL_MAJOR    1
#define HN4_TENSOR_TILED        2

/**
 * hn4_swizzle_tensor_offset
 * Calculates linear offset for N-Dimensional Tensor data.
 *
 * SAFETY:
 *  - Calculates using 64-bit math to prevent 32-bit overflow on large tensors.
 *  - Tiled mode assumes 4x4 blocks; partial blocks are padded (rounded up).
 *  - Does NOT perform bounds checking on inputs (Caller Responsibility).
 */
uint64_t hn4_swizzle_tensor_offset(
    uint32_t x, uint32_t y, uint32_t z, 
    uint32_t width, uint32_t height, uint32_t depth,
    uint8_t format
);

#ifdef __cplusplus
}
#endif

#endif /* HN4_SWIZZLE_H */