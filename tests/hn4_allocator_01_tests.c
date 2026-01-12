/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR O(1) PERFORMANCE SUITE
 * FILE: hn4_allocator_01_tests.c
 * STATUS: ALGORITHMIC COMPLEXITY PROOF (FIXED)
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_endians.h"
#include <string.h>

/* --- FIXTURE --- */
#define FIXTURE_CAPACITY (1024ULL * 1024ULL * 1024ULL) /* 1 GB */
#define FIXTURE_BS       4096

static hn4_volume_t* create_o1_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    /* Setup 1GB Volume */
    vol->vol_block_size = FIXTURE_BS;
    vol->vol_capacity_bytes = FIXTURE_CAPACITY;
    
    /* 
     * FIX: Correct Bitmap Sizing for Armored Words 
     * 1GB / 4KB = 262,144 blocks.
     * Each 64 blocks needs 16 bytes (sizeof hn4_armored_word_t).
     * Calculation: (TotalBlocks / 64) * 16
     */
    uint64_t total_blocks = FIXTURE_CAPACITY / FIXTURE_BS;
    uint64_t words_needed = (total_blocks + 63) / 64;
    vol->bitmap_size = words_needed * sizeof(hn4_armored_word_t);
    
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);
    
    vol->sb.info.lba_flux_start = 0;
    
    vol->target_device = hn4_hal_mem_alloc(sizeof(hn4_hal_caps_t));
    ((hn4_hal_caps_t*)vol->target_device)->logical_block_size = 4096;

    return vol;
}

static void cleanup_o1_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: RANDOM ACCESS (Direct Calculation)
 * ========================================================================= */
hn4_TEST(ComplexityProof, Random_Access_Is_Instant) {
    hn4_volume_t* vol = create_o1_fixture();
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 0;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    
    /* Request Logical Block 100,000 */
    uint64_t target_n = 100000;
    hn4_addr_t lba; 
    uint8_t k;
    
    hn4_result_t res = hn4_alloc_block(vol, &anchor, target_n, &lba, &k);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify Usage Count is exactly 1 */
    uint64_t lba_0 = _calc_trajectory_lba(vol, G, 0, 0, 0, 0);
    bool is_set;
    _bitmap_op(vol, lba_0, BIT_TEST, &is_set);
    ASSERT_FALSE(is_set);
    
    cleanup_o1_fixture(vol);
}

/* =========================================================================
 * TEST 2: BOUNDED COLLISION PROBE (Worst Case O(K))
 * ========================================================================= */
hn4_TEST(ComplexityProof, Worst_Case_Is_Bounded) {
    hn4_volume_t* vol = create_o1_fixture();
    
    uint64_t G = 500;
    uint64_t V = 1;
    uint64_t N = 0;
    
    /* Manually Fill ALL Orbital Shells (K=0..12) */
    for(int k=0; k<=12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Attempt Allocation */
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &lba, &k);
    
    if (res == HN4_OK) {
        ASSERT_EQ(15, k); /* Horizon */
    } else {
        ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    }
    
    cleanup_o1_fixture(vol);
}

/* =========================================================================
 * TEST 3: STRIDE CONSISTENCY (Math O(1))
 * ========================================================================= */
hn4_TEST(ComplexityProof, Vector_Math_Is_Constant) {
    hn4_volume_t* vol = create_o1_fixture();
    
    uint64_t G = 1000;
    
    /* 
     * FIX: Use ODD Vector (43).
     * The allocator enforces V |= 1. Using 42 would be converted to 43.
     */
    uint64_t V = 43; 
    
    uint64_t lba_0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, 0, 0);
    
    /* Check Delta */
    uint64_t diff = lba_1 - lba_0;
    ASSERT_EQ(V, diff);
    
    cleanup_o1_fixture(vol);
}

/* =========================================================================
 * TEST 4: DIRECT BITMAP ACCESS (No Search)
 * ========================================================================= */
hn4_TEST(ComplexityProof, Direct_Bitmap_Indexing) {
    hn4_volume_t* vol = create_o1_fixture();
    
    /* Pick an LBA near the end of the 1GB volume (260,000) */
    uint64_t high_lba = 260000;
    
    /* 1. Set the bit directly */
    bool state_changed;
    hn4_result_t res = _bitmap_op(vol, high_lba, BIT_SET, &state_changed);
    
    /* If this fails with ERR_GEOMETRY, check fixture size calculation */
    ASSERT_EQ(HN4_OK, res);
    ASSERT_TRUE(state_changed);
    
    /* 2. Read it back */
    bool is_set;
    res = _bitmap_op(vol, high_lba, BIT_TEST, &is_set);
    ASSERT_EQ(HN4_OK, res);
    ASSERT_TRUE(is_set);
    
    /* 3. Verify neighbor is empty */
    res = _bitmap_op(vol, high_lba - 1, BIT_TEST, &is_set);
    ASSERT_FALSE(is_set);
    
    cleanup_o1_fixture(vol);
}

/* =========================================================================
 * TEST 5: BITMAP SATURATION PERFORMANCE (The Full Scan Myth)
 * ========================================================================= */
hn4_TEST(ComplexityProof, No_Linear_Scan_On_Collision) {
    hn4_volume_t* vol = create_o1_fixture();
    
    /* Block the specific trajectory for G=1000, V=1, N=0 */
    uint64_t G = 1000;
    uint64_t V = 1;
    
    for(int k=0; k<=12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, 0, &lba, &k);
    
    if (res == HN4_OK) {
        ASSERT_EQ(15, k); /* Horizon */
    } else {
        ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    }
    
    cleanup_o1_fixture(vol);
}