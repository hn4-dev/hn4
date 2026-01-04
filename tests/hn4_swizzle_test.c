#include "hn4_test.h"
#include "hn4_swizzle.h"
#include <stdint.h>
#include <string.h>

static void common_setup(void) {
    /* Swizzle module is stateless math; no initialization required. */
}

/* -------------------- TESTS -------------------- */

/*
 * Test 1: Gravity Assist (Determinism)
 * Verifies the vector rotation and magic XOR constant.
 * Input: 0
 * Expected: (0 ROTL 17) ^ MAGIC -> MAGIC (0xA5A5A5A5A5A5A5A5)
 */
hn4_TEST(Swizzle, GravityAssist) {
    common_setup();
    uint64_t input = 0;
    /* The magic constant defined in hn4_swizzle.c implementation */
    uint64_t magic = 0xA5A5A5A5A5A5A5A5ULL;
    
    uint64_t res = hn4_swizzle_gravity_assist(input);
    ASSERT_EQ(magic, res);
    
    /* 
     * Input 1: (1 << 17) ^ MAGIC 
     * 1 rotated left by 17 is 0x20000
     */
    uint64_t res2 = hn4_swizzle_gravity_assist(1);
    ASSERT_EQ((1ULL << 17) ^ magic, res2);
}

/*
 * Test 2: Morton 2D (Full Range)
 * Verifies bit interleaving works up to the 16-bit limit.
 * Input: 0xFFFF, 0xFFFF
 * Expected: 0xFFFFFFFF (All bits set)
 */
hn4_TEST(Swizzle, Morton2D_Saturation) {
    common_setup();
    uint32_t res = hn4_swizzle_morton_2d(0xFFFF, 0xFFFF);
    ASSERT_EQ(0xFFFFFFFF, res);
    
    /* Check bit ordering: x=1, y=0 -> ...001 */
    ASSERT_EQ(1, hn4_swizzle_morton_2d(1, 0));
    /* Check bit ordering: x=0, y=1 -> ...010 */
    ASSERT_EQ(2, hn4_swizzle_morton_2d(0, 1));
}

/*
 * Test 3: Morton 3D (Safety Masking)
 * The API contract states inputs > 1023 are masked to 10 bits.
 * Input: 1025 (bit 10 set, bit 0 set), y=0, z=0
 * Expected: Masked to 1 (bit 0 set). Result = 1.
 */
hn4_TEST(Swizzle, Morton3D_Masking) {
    common_setup();
    /* 1025 is binary 10000000001. 
       If mask 0x3FF works, high bit is dropped, leaving 1. */
    uint32_t res = hn4_swizzle_morton_3d(1025, 0, 0);
    
    ASSERT_EQ(1, res);
    
    /* Check Z axis shift (Shift 2) */
    /* x=0, y=0, z=1 -> ...100 (4) */
    ASSERT_EQ(4, hn4_swizzle_morton_3d(0, 0, 1));
}

/*
 * Test 4: Tensor Row-Major (Overflow Safety)
 * Verifies that internal math promotes to 64-bit before multiplying.
 * Input: W=65536, H=65536. 32-bit mult would be 0.
 * Calculation: z * H * W
 */
hn4_TEST(Swizzle, Tensor_RowMajor_Overflow) {
    common_setup();
    uint32_t w = 65536;
    uint32_t h = 65536;
    uint32_t d = 5;
    
    /* Calculate offset for z=1, y=0, x=0 */
    uint64_t res = hn4_swizzle_tensor_offset(0, 0, 1, w, h, d, HN4_TENSOR_ROW_MAJOR);
    
    /* Expected: 1 * 65536 * 65536 = 4,294,967,296 */
    uint64_t expected = 4294967296ULL;
    
    ASSERT_EQ(expected, res);
}

/*
 * Test 5: Tensor Column-Major (Logic Check)
 * Verifies standard Fortran/Matlab layout (X varies with H stride).
 * Input: x=1, y=2, z=0. W=10, H=10.
 * Formula: z*H*W + x*H + y
 * Expected: 0 + 1*10 + 2 = 12
 */
hn4_TEST(Swizzle, Tensor_ColMajor_Logic) {
    common_setup();
    uint32_t w = 10;
    uint32_t h = 10;
    
    uint64_t res = hn4_swizzle_tensor_offset(1, 2, 0, w, h, 1, HN4_TENSOR_COL_MAJOR);
    ASSERT_EQ(12, res);
}

/*
 * Test 6: Tensor Tiled (Partial Blocks & Z-Stride)
 * Verifies 4x4 tiling logic handles dimensions not divisible by 4.
 * Input: Width=5 (2 blocks), Height=5 (2 blocks).
 */
hn4_TEST(Swizzle, Tensor_Tiled_Partial) {
    common_setup();
    uint32_t w = 5;
    uint32_t h = 5;
    
    /* 1. Check Y Partial: (0, 4, 0) is in 2nd row of blocks (index 2) */
    uint64_t res_y = hn4_swizzle_tensor_offset(0, 4, 0, w, h, 1, HN4_TENSOR_TILED);
    /* BlockIdx=2, elements=32 */
    ASSERT_EQ(32, res_y);

    /* 2. Check Z Stride: (0, 0, 1) skips entire layer (2x2 blocks = 4 blocks) */
    /* If height rounding was buggy (5>>2 = 1), this would be 2 blocks (32) */
    uint64_t res_z = hn4_swizzle_tensor_offset(0, 0, 1, w, h, 1, HN4_TENSOR_TILED);
    /* 4 blocks * 16 elements = 64 */
    ASSERT_EQ(64, res_z);
}

/* -------------------- MORE TESTS (7-16) -------------------- */

/*
 * Test 7: Gravity Assist (High Bits)
 * Verifies behavior with all bits set (0xFF...FF).
 * ROTL(~0) is still ~0. 
 * ~0 ^ MAGIC is equivalent to ~MAGIC.
 */
hn4_TEST(Swizzle, Gravity_HighBits) {
    common_setup();
    uint64_t all_ones = ~0ULL;
    uint64_t magic = 0xA5A5A5A5A5A5A5A5ULL;
    
    uint64_t res = hn4_swizzle_gravity_assist(all_ones);
    ASSERT_EQ(~magic, res);
}

/*
 * Test 8: Morton 2D (Zero)
 * Basic sanity check. 
 * Input: 0, 0 -> Output: 0
 */
hn4_TEST(Swizzle, Morton2D_Zero) {
    common_setup();
    ASSERT_EQ(0, hn4_swizzle_morton_2d(0, 0));
}

/*
 * Test 9: Morton 2D (Striping)
 * Tests perfect bit separation.
 * X = 0 (0000), Y = 0xFFFF (1111).
 * Y is shifted left by 1 (occupies odd bits). X occupies even bits.
 * Result should be 101010... (0xAAAAAAAA).
 */
hn4_TEST(Swizzle, Morton2D_Stripes) {
    common_setup();
    uint32_t res = hn4_swizzle_morton_2d(0, 0xFFFF);
    ASSERT_EQ(0xAAAAAAAA, res);
    
    /* Inverse: X=High, Y=0 -> 0x55555555 */
    ASSERT_EQ(0x55555555, hn4_swizzle_morton_2d(0xFFFF, 0));
}

/*
 * Test 10: Morton 3D (Lane Isolation)
 * Verifies that X, Y, and Z map to bits 0, 1, and 2 respectively.
 */
hn4_TEST(Swizzle, Morton3D_Lanes) {
    common_setup();
    /* X=1 -> ...001 */
    ASSERT_EQ(1, hn4_swizzle_morton_3d(1, 0, 0));
    /* Y=1 -> ...010 (2) */
    ASSERT_EQ(2, hn4_swizzle_morton_3d(0, 1, 0));
    /* Z=1 -> ...100 (4) */
    ASSERT_EQ(4, hn4_swizzle_morton_3d(0, 0, 1));
}

/*
 * Test 11: Morton 3D (Max 10-bit Input)
 * Input: 1023 (0x3FF) on all axes.
 * Expected: 30 bits set (0x3FFFFFFF).
 */
hn4_TEST(Swizzle, Morton3D_Max10Bit) {
    common_setup();
    uint32_t res = hn4_swizzle_morton_3d(1023, 1023, 1023);
    ASSERT_EQ(0x3FFFFFFF, res);
}

/*
 * Test 12: Tensor Flat (1x1 Dimensions)
 * Edge case: Width=1, Height=1.
 * Should behave like a simple linear array.
 */
hn4_TEST(Swizzle, Tensor_Flat_1D) {
    common_setup();
    /* 1x1 Tensor. z=50 should be offset 50. */
    uint64_t res = hn4_swizzle_tensor_offset(0, 0, 50, 1, 1, 100, HN4_TENSOR_ROW_MAJOR);
    ASSERT_EQ(50, res);
}

/*
 * Test 13: Tensor Tiled (Intra-block Addressing)
 * Verifies logic inside a single 4x4 block.
 * Point (3, 2) inside Block (0,0).
 * Offset = (y_inner << 2) + x_inner
 *        = (2 << 2) + 3 = 11.
 */
hn4_TEST(Swizzle, Tensor_Tiled_IntraBlock) {
    common_setup();
    /* W=4, H=4 (Exactly 1 block) */
    uint64_t res = hn4_swizzle_tensor_offset(3, 2, 0, 4, 4, 1, HN4_TENSOR_TILED);
    ASSERT_EQ(11, res);
}

/*
 * Test 14: Tensor Tiled (Exact Alignment)
 * Verifies that dimensions perfectly divisible by 4 don't add extra padding.
 * W=4 (1 block width). H=4 (1 block height).
 * Z=1 should jump exactly 16 elements (1 block).
 */
hn4_TEST(Swizzle, Tensor_Tiled_ExactAlign) {
    common_setup();
    /* x=0, y=0, z=1 */
    uint64_t res = hn4_swizzle_tensor_offset(0, 0, 1, 4, 4, 1, HN4_TENSOR_TILED);
    ASSERT_EQ(16, res);
}

/*
 * Test 15: Tensor Tiled (Tiny Padding)
 * Extreme padding case. W=1, H=1.
 * Still consumes 1 full block (16 elements) per layer due to 4x4 tiling.
 * Z=1 should jump 16, even though W*H is only 1.
 */
hn4_TEST(Swizzle, Tensor_Tiled_TinyPadding) {
    common_setup();
    uint64_t res = hn4_swizzle_tensor_offset(0, 0, 1, 1, 1, 1, HN4_TENSOR_TILED);
    ASSERT_EQ(16, res);
}

/*
 * Test 16: Tensor Invalid Format (Fallback)
 * Passing an unknown format ID (e.g. 99) should default to Row Major.
 */
hn4_TEST(Swizzle, Tensor_InvalidFormat_Fallback) {
    common_setup();
    uint32_t w=10, h=10;
    /* Row Major: 0 + 2*10 + 1 = 21 */
    uint64_t expected = 21;
    
    /* Pass '99' as format */
    uint64_t res = hn4_swizzle_tensor_offset(1, 2, 0, w, h, 1, 99);
    
    ASSERT_EQ(expected, res);
}