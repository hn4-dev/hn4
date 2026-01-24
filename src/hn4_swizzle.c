/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Swizzle Engine (Bit Manipulation)
 * SOURCE:      hn4_swizzle.c
 * VERSION:     5.1 (Reference Standard)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Mathematical primitives for address space permutation.
 * Implements Morton Codes (Z-Order curves), Gravity Assist vectors, 
 * and N-Dimensional Tensor striding logic.
 */

#include "hn4_swizzle.h"
#include "hn4_constants.h"

#if defined(__x86_64__) || defined(_M_X64)
    #if defined(__BMI2__) || (defined(_MSC_VER) && defined(__AVX2__))
        #include <immintrin.h>
        #define HN4_HW_PDEP 1
    #endif
#endif

/* 
 * Magic constant for Gravity Assist.
 * Alternating bit pattern (10100101...) used to decorrelate address bits.
 */
static const uint64_t HN4_GRAVITY_MAGIC = 0xA5A5A5A5A5A5A5A5ULL;

// ============================================================================
// GRAVITY ASSIST (COLLISION AVOIDANCE)
// ============================================================================

/*
 * hn4_swizzle_gravity_assist
 *
 * Calculates the "Vector Shift" to escape a gravity well collision.
 * Implements the deterministic permutation defined in Spec Section 6.6.
 *
 * This is effectively a bijective hash function used to redistribute
 * entries when a hash collision occurs in the primary index.
 */
uint64_t hn4_swizzle_gravity_assist(uint64_t orbit_vector)
{
    // Rotate left 17 bits to realign entropy, then XOR to scramble.
    // 17 is prime, which helps avoid resonance with power-of-2 table sizes.
    return hn4_rotl64(orbit_vector, 17) ^ HN4_GRAVITY_MAGIC;
}

// ============================================================================
// SPATIAL SWIZZLING (MORTON CODES)
// ============================================================================

/*
 * Helper: _spread_bits_16
 * Spreads the lower 16 bits of x to even positions.
 */
HN4_INLINE uint32_t _spread_bits_16(uint16_t x) 
{
#if defined(HN4_HW_PDEP)
    return _pdep_u32(x, 0x55555555);
#else
    /* Fallback: Generic Magic Numbers (~15 cycles) */
    uint32_t n = x;
    n = (n | (n << 8)) & 0x00FF00FF;
    n = (n | (n << 4)) & 0x0F0F0F0F;
    n = (n | (n << 2)) & 0x33333333;
    n = (n | (n << 1)) & 0x55555555;
    return n;
#endif
}

/* 
 * Helper: _spread_bits_10
 * Spreads the lower 10 bits of x to every 3rd position.
 * Used for 3D interleaving.
 * SAFETY: Input is masked to 10 bits to prevent overflow in the 32-bit container.
 */
HN4_INLINE uint32_t _spread_bits_10(uint16_t x) 
{
    uint32_t n = x & 0x3FF; 
    n = (n | (n << 16)) & 0x030000FF;
    n = (n | (n <<  8)) & 0x0300F00F;
    n = (n | (n <<  4)) & 0x030C30C3;
    n = (n | (n <<  2)) & 0x09249249;
    return n;
}

/*
 * hn4_swizzle_morton_2d
 * Interleaves 16-bit X/Y coordinates into a 32-bit Z-Order index.
 * Useful for 2D texture locality.
 */
uint32_t hn4_swizzle_morton_2d(uint16_t x, uint16_t y)
{
    // Layout: YX YX YX ...
    return (_spread_bits_16(y) << 1) | _spread_bits_16(x);
}

/*
 * hn4_swizzle_morton_3d
 * Interleaves 10-bit X/Y/Z coordinates into a 30-bit code.
 * Useful for Voxel data.
 */
uint32_t hn4_swizzle_morton_3d(uint16_t x, uint16_t y, uint16_t z)
{
    // Layout: ZYX ZYX ZYX ...
    return (_spread_bits_10(z) << 2) | 
           (_spread_bits_10(y) << 1) | 
            _spread_bits_10(x);
}

// ============================================================================
// TENSOR STRIDING LOGIC
// ============================================================================

/*
 * hn4_swizzle_tensor_offset
 *
 * Calculates the linear byte offset for N-Dimensional Tensor data.
 * Handles Row-Major, Col-Major, and 4x4 Tiled formats.
 *
 * NOTE: Returns HN4_OFFSET_INVALID on integer overflow.
 */
uint64_t hn4_swizzle_tensor_offset(
    uint32_t x, uint32_t y, uint32_t z, 
    uint32_t width, uint32_t height, uint32_t depth,
    uint8_t format)
{
    // Depth is implicit in the Z calculation, explicitly mark unused to silence warnings
    (void)depth; 

    // 1. Basic Bounds Check
    if (x >= width || y >= height) {
        return HN4_OFFSET_INVALID;
    }

    // Promote to 64-bit immediately to prevent intermediate overflow
    uint64_t w64 = (uint64_t)width;
    uint64_t h64 = (uint64_t)height;
    uint64_t z64 = (uint64_t)z;
    uint64_t y64 = (uint64_t)y;
    uint64_t x64 = (uint64_t)x;

    // 2. Calculate Plane Size (W * H) with overflow protection
    if (w64 > 0 && h64 > (UINT64_MAX / w64)) return HN4_OFFSET_INVALID;
    uint64_t plane_sz = w64 * h64;

    // 3. Calculate Base Z Offset (Z * PlaneSize)
    if (plane_sz > 0 && z64 > (UINT64_MAX / plane_sz)) return HN4_OFFSET_INVALID;
    uint64_t base_offset = z64 * plane_sz;

    switch (format) {
    case HN4_TENSOR_ROW_MAJOR:
    {
        // Offset = Base + (Y * W) + X
        uint64_t row_offset = y64 * w64; // Safe: y < h, and w*h fits in u64

        // Check sum overflow
        if ((UINT64_MAX - base_offset) < row_offset) return HN4_OFFSET_INVALID;
        uint64_t temp = base_offset + row_offset;

        if ((UINT64_MAX - temp) < x64) return HN4_OFFSET_INVALID;
        return temp + x64;
    }

    case HN4_TENSOR_COL_MAJOR:
    {
        // Offset = Base + (X * H) + Y
        // Note: We assume Z-planes are still stacked linearly.
        uint64_t col_offset = x64 * h64;

        if ((UINT64_MAX - base_offset) < col_offset) return HN4_OFFSET_INVALID;
        uint64_t temp = base_offset + col_offset;

        if ((UINT64_MAX - temp) < y64) return HN4_OFFSET_INVALID;
        return temp + y64;
    }

    case HN4_TENSOR_TILED:
    {
        /*
         * 4x4 Tiling Strategy:
         * Breaks surface into 4x4 pixel blocks for cache coherency.
         * Addresses are calculated as: BlockIndex -> InsideBlockOffset
         */
        
        // Split coordinates into Block IDs and In-Block Offsets
        uint64_t block_x = x64 >> 2;
        uint64_t block_y = y64 >> 2;
        uint64_t in_x    = x64 & 3;
        uint64_t in_y    = y64 & 3;

        // Round up dimensions to 4-block boundary: (dim + 3) / 4
        uint64_t w_blocks = (w64 + 3) >> 2;
        uint64_t h_blocks = (h64 + 3) >> 2;

        // Check overflow for total blocks
        if (w_blocks > 0 && h_blocks > (UINT64_MAX / w_blocks)) return HN4_OFFSET_INVALID;
        uint64_t blocks_per_plane = w_blocks * h_blocks;

        // Recalculate Base Z in terms of *blocks*
        if (blocks_per_plane > 0 && z64 > (UINT64_MAX / blocks_per_plane)) return HN4_OFFSET_INVALID;
        uint64_t z_block_base = z64 * blocks_per_plane;

        // Calculate linear Block Index
        uint64_t y_block_offset = block_y * w_blocks;
        
        if ((UINT64_MAX - z_block_base) < y_block_offset) return HN4_OFFSET_INVALID;
        uint64_t temp = z_block_base + y_block_offset;

        if ((UINT64_MAX - temp) < block_x) return HN4_OFFSET_INVALID;
        uint64_t block_idx = temp + block_x;

        // Finalize: Each block is 16 elements (4x4)
        // Check if block_idx * 16 overflows
        if (block_idx > (UINT64_MAX >> 4)) return HN4_OFFSET_INVALID;

        // (BlockIndex * 16) + (InBlockY * 4) + InBlockX
        return (block_idx << 4) + (in_y << 2) + in_x;
    }

    default:
        // Fallback to Row Major if format is unknown (Safety default)
        {
            uint64_t row_offset = y64 * w64;
            if ((UINT64_MAX - base_offset) < row_offset) return HN4_OFFSET_INVALID;
            return base_offset + row_offset + x64;
        }
    }
}