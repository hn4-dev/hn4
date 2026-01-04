/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Swizzle Engine (Bit Manipulation)
 * SOURCE:      hn4_swizzle.c
 * VERSION:     5.1 (Reference Standard)
 *
 * COPYRIGHT:   (c) 2025 The Hydra-Nexus Team. All rights reserved.
 *
 * DESCRIPTION:
 * Provides the mathematical primitives for address space permutation.
 * Implements Morton Codes (Z-Order) for spatial locality, Gravity Assist
 * vectors for collision avoidance, and Tensor striding logic.
 *
 * IMPLEMENTATION NOTES:
 * 1. Safety: Explicit 32-bit overflow protection in tensor math.
 * 2. Accuracy: Fixed tiling alignment logic to preserve partial blocks.
 * 3. Robustness: Enforced bitmasking on inputs to prevent undefined behavior.
 */

#include "hn4_swizzle.h"

/* =========================================================================
 * 1. GRAVITY ASSIST (SPEC 6.6)
 * ========================================================================= */

static const uint64_t HN4_GRAVITY_MAGIC = 0xA5A5A5A5A5A5A5A5ULL;

uint64_t hn4_swizzle_gravity_assist(uint64_t orbit_vector) {
    /* 
     * Spec 6.6: "Deterministic Vector Shift"
     * Rotates the prime vector to a new alignment and applies a bitmask XOR.
     * This de-correlates the address probability from the previous collision.
     */
    return hn4_rotl64(orbit_vector, 17) ^ HN4_GRAVITY_MAGIC;
}

/* =========================================================================
 * 2. SPATIAL SWIZZLING (MORTON CODES)
 * ========================================================================= */

/* 
 * Helper: Spreads the lower 16 bits of x to even positions.
 * Pattern: x...x -> x0x0x0x0
 */
static inline uint32_t _part1by1(uint16_t x) {
    uint32_t n = x;
    n = (n | (n << 8)) & 0x00FF00FF;
    n = (n | (n << 4)) & 0x0F0F0F0F;
    n = (n | (n << 2)) & 0x33333333;
    n = (n | (n << 1)) & 0x55555555;
    return n;
}

/* 
 * Helper: Spreads the lower 10 bits of x to every 3rd position.
 * Pattern: x..x -> x00x00x00
 * SAFETY: Explicitly masks input to 10 bits to prevent caller overflow.
 */
static inline uint32_t _part1by2(uint16_t x) {
    uint32_t n = x & 0x3FF; /* Strict 10-bit clamp */
    n = (n | (n << 16)) & 0x030000FF;
    n = (n | (n <<  8)) & 0x0300F00F;
    n = (n | (n <<  4)) & 0x030C30C3;
    n = (n | (n <<  2)) & 0x09249249;
    return n;
}

uint32_t hn4_swizzle_morton_2d(uint16_t x, uint16_t y) {
    /* Interleave bits: YX YX YX ... using OR for safety */
    return (_part1by1(y) << 1) | _part1by1(x);
}

uint32_t hn4_swizzle_morton_3d(uint16_t x, uint16_t y, uint16_t z) {
    /* Interleave bits: ZYX ZYX ZYX ... */
    return (_part1by2(z) << 2) | (_part1by2(y) << 1) | _part1by2(x);
}

/* =========================================================================
 * 3. TENSOR STRIDING
 * ========================================================================= */

uint64_t hn4_swizzle_tensor_offset(
    uint32_t x, uint32_t y, uint32_t z, 
    uint32_t width, uint32_t height, uint32_t depth,
    uint8_t format
) {
    (void)depth;

    /* FIX: Cast dimensions to uint64_t BEFORE multiplication to prevent 32-bit overflow */
    uint64_t w64 = (uint64_t)width;
    uint64_t h64 = (uint64_t)height;
    uint64_t z64 = (uint64_t)z;
    uint64_t y64 = (uint64_t)y;
    uint64_t x64 = (uint64_t)x;

    switch (format) {
        case HN4_TENSOR_ROW_MAJOR:
            /* Standard C layout: Z, Y, X */
            return (z64 * h64 * w64) + (y64 * w64) + x64;

        case HN4_TENSOR_COL_MAJOR:
            /* Fortran/BLAS layout: X, Y, Z */
            return (z64 * h64 * w64) + (x64 * h64) + y64;

        case HN4_TENSOR_TILED:
            {
                uint64_t block_x = x64 >> 2;
                uint64_t block_y = y64 >> 2;
                uint64_t in_x    = x64 & 3;
                uint64_t in_y    = y64 & 3;
                
                uint64_t width_in_blocks = (w64 + 3) >> 2;
                uint64_t height_in_blocks = (h64 + 3) >> 2;
                
                uint64_t block_idx = (z64 * height_in_blocks * width_in_blocks) + 
                                     (block_y * width_in_blocks) + 
                                     block_x;
                                     
                return (block_idx << 4) + (in_y << 2) + in_x;
            }

        default:
            return (z64 * h64 * w64) + (y64 * w64) + x64;
    }
}
