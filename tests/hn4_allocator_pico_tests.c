/*
 * HYDRA-NEXUS 4 (HN4) - PICO ALLOCATOR SUITE
 * FILE: hn4_allocator_pico_tests.c
 * STATUS: EMBEDDED CONSTRAINTS VERIFICATION
 *
 * SCOPE:
 * Validates the behavior of the Void Engine under HN4_PROFILE_PICO settings:
 * 1. 1.44MB Capacity (Floppy/Embedded Flash simulation).
 * 2. 512-byte blocks.
 * 3. Restricted Trajectories (K=0 only).
 * 4. Saturation logic for small volumes.
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_addr.h"
#include "hn4.h"
#include "hn4_endians.h"
#include "hn4_ecc.h" 
#include "hn4_constants.h"
#include <string.h>
#include <stdatomic.h>

/* --- FIXTURE: 1.44MB FLOPPY SIMULATION --- */
#define PICO_BS 512
#define PICO_CAP (1440 * 1024) /* 1.44 MB */
#define PICO_TOTAL_BLOCKS (PICO_CAP / PICO_BS)

static hn4_volume_t* create_pico_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    vol->sb.info.format_profile = HN4_PROFILE_PICO;
    vol->vol_block_size = PICO_BS;
    vol->vol_capacity_bytes = PICO_CAP;
    
    /* Bitmap setup */
    /* ((2880 blocks + 63) / 64) * 16 bytes per armored word */
    vol->bitmap_size = ((PICO_TOTAL_BLOCKS + 63) / 64) * sizeof(hn4_armored_word_t);
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);
    
    /* Initialize Bitmap ECC */
    size_t words = vol->bitmap_size / sizeof(hn4_armored_word_t);
    for(size_t i=0; i<words; i++) {
        vol->void_bitmap[i].data = 0;
        vol->void_bitmap[i].ecc = _calc_ecc_hamming(0);
    }
    
    /* Layout Setup */
    vol->sb.info.lba_flux_start = hn4_addr_from_u64(100);
    
    /* Horizon starts near end of disk for Pico */
    vol->sb.info.lba_horizon_start = hn4_addr_from_u64(PICO_TOTAL_BLOCKS - 100);
    vol->sb.info.journal_start = hn4_addr_from_u64(PICO_TOTAL_BLOCKS - 10);
    
    /* Pico often has NO Q-Mask to save RAM */
    vol->quality_mask = NULL;
    vol->qmask_size = 0;
    
    /* Mock Device */
    vol->target_device = hn4_hal_mem_alloc(sizeof(hn4_hal_caps_t));
    ((hn4_hal_caps_t*)vol->target_device)->logical_block_size = 512;

    return vol;
}

static void cleanup_pico_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: SINGLE ORBIT ENFORCEMENT (No Gravity Assist)
 * ========================================================================= */
/*
 * RATIONALE:
 * Pico logic disables K-exploration (Only K=0 allowed).
 * If K=0 is occupied, `alloc_block` normally falls back to Horizon.
 * To force GRAVITY_COLLAPSE, we set Fractal Scale M=1. 
 * Horizon requires M=0 (Linear). Thus, M=1 + Collision = Fail.
 */
hn4_TEST(PicoLogic, No_Gravity_Assist) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t G = 500;
    uint64_t V = 1;
    uint64_t N = 0;
    uint16_t M = 1; /* Fractal Scale 1 forces Ballistic-Only (No Horizon) */
    
    /* 1. Manually occupy K=0 */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, N, M, 0);
    _bitmap_op(vol, lba_k0, BIT_SET, NULL);
    
    /* 2. Attempt Alloc */
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.fractal_scale = hn4_cpu_to_le16(M);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &lba, &k);
    
    /* 
     * Expect FAILURE.
     * In Standard profile, this would jump to K=1.
     * In Pico, K is clamped to 0. Since K=0 is full and M=1 blocks Horizon, it collapses.
     */
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 2: VALID LINEAR ALLOCATION
 * ========================================================================= */
/*
 * RATIONALE:
 * Ensure the basic "Rail" logic (V=1) works on a clean floppy.
 */
hn4_TEST(PicoLogic, Valid_Linear_Allocation) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t G = 200;
    uint64_t V = 1;
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba_out; 
    uint8_t k_out;
    
    /* Alloc Logical Index 0 */
    hn4_result_t res = hn4_alloc_block(vol, &anchor, 0, &lba_out, &k_out);
    
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(0, k_out);
    
    uint64_t phys_idx = hn4_addr_to_u64(lba_out);
    bool is_set;
    _bitmap_op(vol, phys_idx, BIT_TEST, &is_set);
    
    ASSERT_TRUE(is_set);
    ASSERT_TRUE(phys_idx >= 100); /* Must be after Flux Start */
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 3: SATURATION LOCKOUT (95% Rule)
 * ========================================================================= */
/*
 * RATIONALE:
 * alloc_block uses the "Update" limit (95%) for saturation checks.
 * We must inflate usage beyond 95% to trigger the fallback/failure logic.
 */
hn4_TEST(PicoLogic, Saturation_Limit_Genesis) {
    hn4_volume_t* vol = create_pico_fixture();
    
    /* Total Blocks ~2880. 95% is ~2736. Set to 2800. */
    uint64_t limit = 2800;
    
    atomic_store(&vol->alloc.used_blocks, limit);
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(500);
    /* Set M=1 to block Horizon fallback, forcing error if D1 saturated */
    anchor.fractal_scale = hn4_cpu_to_le16(1);
    
    hn4_addr_t lba; uint8_t k;
    
    hn4_result_t res = hn4_alloc_block(vol, &anchor, 0, &lba, &k);
    
    /* 
     * Expectation: D1 is saturated (>95%).
     * Falls to Horizon? No, M=1 prevents it.
     * Returns GRAVITY_COLLAPSE.
     */
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 4: FULL DISK (PHYSICAL COLLISION)
 * ========================================================================= */
/*
 * RATIONALE:
 * Instead of relying on wrapping behavior (which is valid), we test
 * "Physical Exhaustion". We mark the calculated block as USED in the bitmap.
 * Since PICO only tries K=0, if K=0 is used, it must fail (if Horizon disabled).
 */
hn4_TEST(PicoLogic, Full_Disk_Collision) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t G = 1000;
    uint64_t V = 1;
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.fractal_scale = hn4_cpu_to_le16(1); // Force D1 only
    memcpy(anchor.orbit_vector, &V, 6);
    
    /* Pre-calculate where it wants to go */
    uint64_t target = _calc_trajectory_lba(vol, G, V, 0, 1, 0);
    
    /* Mark it as used */
    _bitmap_op(vol, target, BIT_SET, NULL);
    
    hn4_addr_t lba; uint8_t k;
    
    /* Alloc should fail because K=0 is taken and MaxK=0 */
    hn4_result_t res = hn4_alloc_block(vol, &anchor, 0, &lba, &k);
    
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 5: BITMAP ECC SELF-HEALING
 * ========================================================================= */
/*
 * RATIONALE:
 * Verify RAM integrity logic. Corrupt a bit, read it back, verify self-repair.
 */
hn4_TEST(PicoLogic, Bitmap_ECC_Healing) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t test_idx = 500;
    
    /* 1. Set a bit cleanly */
    _bitmap_op(vol, test_idx, BIT_SET, NULL);
    
    /* 2. Locate the word in RAM */
    uint64_t word_idx = test_idx / 64;
    hn4_armored_word_t* w = &vol->void_bitmap[word_idx];
    
    /* 3. Manually flip a bit in Data WITHOUT updating ECC */
    w->data ^= 1ULL; 
    
    /* 4. Read back via API */
    bool is_set = false;
    hn4_result_t res = _bitmap_op(vol, test_idx, BIT_TEST, &is_set);
    
    /* 
     * Expectation:
     * 1. The read succeeds (Healed).
     * 2. The data matches original intent (SET).
     * 3. RAM ECC is consistent again.
     */
    bool healed_ok = (res == HN4_OK || res == HN4_INFO_HEALED);
    ASSERT_TRUE(healed_ok);
    ASSERT_TRUE(is_set); 
    
    uint8_t new_ecc = _calc_ecc_hamming(w->data);
    ASSERT_EQ(new_ecc, w->ecc);
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 6: HORIZON RING WRAP-AROUND
 * ========================================================================= */
/*
 * RATIONALE:
 * Verify Horizon Ring Buffer logic works on small scale.
 */
hn4_TEST(PicoLogic, Horizon_Ring_Allocation) {
    hn4_volume_t* vol = create_pico_fixture();
    
    hn4_addr_t lba1, lba2;
    
    /* 1. Alloc 1 */
    hn4_result_t res1 = hn4_alloc_horizon(vol, &lba1);
    ASSERT_EQ(HN4_OK, res1);
    
    /* 2. Alloc 2 */
    hn4_result_t res2 = hn4_alloc_horizon(vol, &lba2);
    ASSERT_EQ(HN4_OK, res2);
    
    ASSERT_NEQ(hn4_addr_to_u64(lba1), hn4_addr_to_u64(lba2));
    
    /* Check pointer advanced */
    uint64_t head = atomic_load(&vol->alloc.horizon_write_head);
    ASSERT_TRUE(head >= 2);
    
    cleanup_pico_fixture(vol);
}