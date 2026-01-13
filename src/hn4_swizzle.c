/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Swizzle Engine (Bit Manipulation)
 * SOURCE:      hn4_swizzle.c
 * VERSION:     5.1 (Reference Standard)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Mathematical primitives for address space permutation.
 * Implements Morton Codes, Gravity Assist vectors, and Tensor striding.
 */

#include "hn4_swizzle.h"
#include "hn4_constants.h"

//
// -----------------------------------------------------------------------------
// GRAVITY ASSIST (COLLISION AVOIDANCE)
// -----------------------------------------------------------------------------
//

static const uint64_t HN4_GRAVITY_MAGIC = 0xA5A5A5A5A5A5A5A5ULL;

/*++

Routine Description:

    Calculates the "Vector Shift" to escape a gravity well collision.
    Implements the deterministic permutation defined in Spec Section 6.6.

Arguments:

    orbit_vector - The current 64-bit orbit trajectory vector (V).

Return Value:

    Returns the next vector (V') in the ballistic sequence.

--*/
uint64_t
hn4_swizzle_gravity_assist(
    uint64_t orbit_vector
    )
{
    //
    // Deterministic Vector Shift:
    // Rotates the prime vector left by 17 bits to realign the bit entropy,
    // then applies an XOR mask to de-correlate address probability.
    //
    return hn4_rotl64(orbit_vector, 17) ^ HN4_GRAVITY_MAGIC;
}

//
// -----------------------------------------------------------------------------
// SPATIAL SWIZZLING (MORTON CODES)
// -----------------------------------------------------------------------------
//

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

/*++

Routine Description:

    Interleaves the bits of two 16-bit coordinates to form a 32-bit Z-Order index.
    Used for 2D spatial locality in tile-based resources.

Arguments:

    x - The X coordinate (0..65535)
    y - The Y coordinate (0..65535)

Return Value:

    Returns the 32-bit Morton code.

--*/
uint32_t
hn4_swizzle_morton_2d(
    uint16_t x,
    uint16_t y
    )
{
    // Interleave bits: YX YX YX ...
    return (_part1by1(y) << 1) | _part1by1(x);
}

/*++

Routine Description:

    Interleaves the bits of three 10-bit coordinates to form a 30-bit Z-Order index.
    Used for volumetric data addressing.

Arguments:

    x, y, z - Coordinates (0..1023). Higher bits are masked off.

Return Value:

    Returns the 30-bit Morton code.

--*/
uint32_t
hn4_swizzle_morton_3d(
    uint16_t x,
    uint16_t y,
    uint16_t z
    )
{
    // Interleave bits: ZYX ZYX ZYX ...
    return (_part1by2(z) << 2) | (_part1by2(y) << 1) | _part1by2(x);
}

//
// -----------------------------------------------------------------------------
// TENSOR STRIDING LOGIC
// -----------------------------------------------------------------------------
//

/*++

Routine Description:

    Calculates the linear byte/element offset for N-Dimensional Tensor data.
    Supports standard linear layouts and blocked (tiled) layouts.

Arguments:

    x, y, z - The coordinates of the element.
    width, height, depth - The dimensions of the tensor volume.
    format - The memory layout format (Row Major, Col Major, Tiled).

Return Value:

    Returns the 64-bit linear index.

--*/
uint64_t
hn4_swizzle_tensor_offset(
    uint32_t x,
    uint32_t y,
    uint32_t z, 
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    uint8_t format
    )
{
    (void)depth; // Unused in linear calculation, implicit in Z coordinate

    /* 1. Basic Coordinate Bounds Check */
    if (x >= width || y >= height) return HN4_OFFSET_INVALID;

    /*
     * Cast dimensions to 64-bit BEFORE multiplication.
     */
    uint64_t w64 = (uint64_t)width;
    uint64_t h64 = (uint64_t)height;
    uint64_t z64 = (uint64_t)z;
    uint64_t y64 = (uint64_t)y;
    uint64_t x64 = (uint64_t)x;

    /*
     * 2. Overflow Protection: Plane Size
     * Check if a single 2D plane (W * H) fits in u64.
     */
    if (w64 > 0 && h64 > (UINT64_MAX / w64)) return HN4_OFFSET_INVALID;
    uint64_t plane_sz = w64 * h64;

    /*
     * 3. Overflow Protection: Volume Base
     * Check if Z * PlaneSize fits.
     */
    if (plane_sz > 0 && z64 > (UINT64_MAX / plane_sz)) return HN4_OFFSET_INVALID;
    uint64_t base_offset = z64 * plane_sz;

    switch (format) {
        case HN4_TENSOR_ROW_MAJOR:
            {
                /* Offset = Base + (Y * W) + X */
                
                /* Y < H, so Y*W cannot exceed PlaneSize, which we validated. */
                uint64_t row_offset = y64 * w64;
                
                /* Check Summation Overflow */
                if ((UINT64_MAX - base_offset) < row_offset) return HN4_OFFSET_INVALID;
                uint64_t temp = base_offset + row_offset;
                
                if ((UINT64_MAX - temp) < x64) return HN4_OFFSET_INVALID;
                return temp + x64;
            }

        case HN4_TENSOR_COL_MAJOR:
            {
                /* Offset = Base + (X * H) + Y */
                /* Assumes Z-planes are stacked linearly even in Col-Major */
                
                uint64_t col_offset = x64 * h64;
                
                if ((UINT64_MAX - base_offset) < col_offset) return HN4_OFFSET_INVALID;
                uint64_t temp = base_offset + col_offset;
                
                if ((UINT64_MAX - temp) < y64) return HN4_OFFSET_INVALID;
                return temp + y64;
            }

        case HN4_TENSOR_TILED:
            {
                /*
                 * 4x4 Tiling Logic.
                 * Breaks the surface into 4x4 blocks for cache locality.
                 */
                uint64_t block_x = x64 >> 2;
                uint64_t block_y = y64 >> 2;
                uint64_t in_x    = x64 & 3;
                uint64_t in_y    = y64 & 3;
                
                /* Round up dimensions to next 4-block boundary: (W + 3) / 4 */
                uint64_t width_in_blocks = (w64 + 3) >> 2;
                uint64_t height_in_blocks = (h64 + 3) >> 2;
                
                /* Check for overflow in block dimensions */
                if (width_in_blocks > 0 && height_in_blocks > (UINT64_MAX / width_in_blocks)) 
                    return HN4_OFFSET_INVALID;
                
                uint64_t blocks_per_plane = width_in_blocks * height_in_blocks;
                
                /* Check Z block offset */
                if (blocks_per_plane > 0 && z64 > (UINT64_MAX / blocks_per_plane)) 
                    return HN4_OFFSET_INVALID;
                
                uint64_t z_block_base = z64 * blocks_per_plane;
                
                /* Calculate Block Index: Base + (BlockY * WidthInBlocks) + BlockX */
                uint64_t y_block_offset = block_y * width_in_blocks;
                
                if ((UINT64_MAX - z_block_base) < y_block_offset) return HN4_OFFSET_INVALID;
                uint64_t temp_idx = z_block_base + y_block_offset;
                
                if ((UINT64_MAX - temp_idx) < block_x) return HN4_OFFSET_INVALID;
                uint64_t block_idx = temp_idx + block_x;
                
                /*
                 * Each block contains 16 elements (4x4).
                 * Offset = (BlockIndex * 16) + (InBlockY * 4) + InBlockX
                 * Check if BlockIndex > (UINT64_MAX / 16)
                 */
                if (block_idx > (UINT64_MAX >> 4)) return HN4_OFFSET_INVALID;
                
                return (block_idx << 4) + (in_y << 2) + in_x;
            }

        default:
            /* Fallback to Row Major (Safe Default) */
            {
                uint64_t row_offset = y64 * w64;
                if ((UINT64_MAX - base_offset) < row_offset) return HN4_OFFSET_INVALID;
                uint64_t temp = base_offset + row_offset;
                if ((UINT64_MAX - temp) < x64) return HN4_OFFSET_INVALID;
                return temp + x64;
            }
    }
}