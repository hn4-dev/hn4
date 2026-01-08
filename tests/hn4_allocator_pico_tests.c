/*
 * HYDRA-NEXUS 4 (HN4) - PICO ALLOCATOR SUITE
 * FILE: hn4_allocator_pico_tests.c
 * STATUS: EMBEDDED CONSTRAINTS VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_endians.h"
#include <string.h>

/* --- BINDINGS --- */
typedef enum { BIT_SET, BIT_CLEAR, BIT_TEST } hn4_bit_op_t;
extern hn4_result_t _bitmap_op(hn4_volume_t* vol, uint64_t idx, hn4_bit_op_t op, bool* chg);
extern uint64_t _calc_trajectory_lba(hn4_volume_t* v, uint64_t G, uint64_t V, uint64_t N, uint16_t M, uint8_t k);
extern hn4_result_t hn4_alloc_block(hn4_volume_t* vol, const hn4_anchor_t* anchor, uint64_t logical_idx, hn4_addr_t* out_lba, uint8_t* out_k);
extern hn4_result_t hn4_alloc_genesis(hn4_volume_t* vol, uint16_t fractal_scale, uint8_t intent, uint64_t* out_G, uint64_t* out_V);

/* --- FIXTURE: 1.44MB FLOPPY SIMULATION --- */
#define PICO_BS 512
#define PICO_CAP (1440 * 1024) /* 1.44 MB */

static hn4_volume_t* create_pico_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    vol->sb.info.format_profile = HN4_PROFILE_PICO;
    vol->vol_block_size = PICO_BS;
    vol->vol_capacity_bytes = PICO_CAP;
    
    /* Bitmap setup */
    uint64_t total_blocks = PICO_CAP / PICO_BS; /* 2880 blocks */
    vol->bitmap_size = ((total_blocks + 63) / 64) * 16;
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);
    
    /* Flux Start (Leave tiny space for metadata) */
    vol->sb.info.lba_flux_start = 100;
    
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
 * TEST 1: VECTOR CONSTRAINT (Force V=1)
 * ========================================================================= */
/*
 * RATIONALE:
 * Pico profiles are designed for simple SD cards where sequential writes are key.
 * Genesis allocation MUST return V=1, regardless of random entropy.
 */
hn4_TEST(PicoLogic, Force_Sequential_Vector) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t G, V;
    
    /* Run multiple times to ensure RNG doesn't accidentally pick 1 */
    for(int i=0; i<50; i++) {
        hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
        ASSERT_EQ(HN4_OK, res);
        
        /* Must be strictly 1 */
        ASSERT_EQ(1ULL, V);
        
        /* Reset used blocks to prevent saturation */
        atomic_store(&vol->used_blocks, 0);
    }
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 2: SINGLE ORBIT ENFORCEMENT (No Gravity Assist)
 * ========================================================================= */
/*
 * RATIONALE:
 * Pico logic disables K-exploration to save CPU cycles and complexity.
 * If K=0 is occupied, `alloc_block` must FAIL (Gravity Collapse) immediately.
 * It should not try K=1..12.
 */
hn4_TEST(PicoLogic, No_Gravity_Assist) {
    hn4_volume_t* vol = create_pico_fixture();
    
    uint64_t G = 500;
    uint64_t V = 1;
    uint64_t N = 0;
    
    /* 1. Manually occupy K=0 */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    _bitmap_op(vol, lba_k0, BIT_SET, NULL);
    
    /* 2. Attempt Alloc */
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &lba, &k);
    
    /* 
     * Expect FAILURE.
     * In Standard profile, this would jump to K=1.
     * In Pico, K is clamped to 0.
     */
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_pico_fixture(vol);
}

/* =========================================================================
 * TEST 3: NO QUALITY MASK (RAM Savings)
 * ========================================================================= */
/*
 * RATIONALE:
 * Pico initializes with `quality_mask = NULL`.
 * The allocator must handle this gracefully and assume all blocks are "Silver" (Safe).
 * It must not crash on NULL pointer dereference.
 */
hn4_TEST(PicoLogic, Null_QMask_Is_Safe) {
    hn4_volume_t* vol = create_pico_fixture();
    
    /* Explicitly ensure NULL (Fixture does this, but be sure) */
    if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
    vol->quality_mask = NULL;
    vol->qmask_size = 0;
    
    /* Attempt Alloc */
    uint64_t G, V;
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify a block was claimed */
    bool is_set;
    uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    _bitmap_op(vol, lba, BIT_TEST, &is_set);
    ASSERT_TRUE(is_set);
    
    cleanup_pico_fixture(vol);
}


/* =========================================================================
 * TEST 5: SEQUENTIAL ALLOCATION PATTERN
 * ========================================================================= */
/*
 * RATIONALE:
 * Since V=1 is forced, logical blocks N=0, N=1, N=2 must map to 
 * physical LBA, LBA+1, LBA+2.
 * This ensures strict linearity for dumb block devices.
 */
hn4_TEST(PicoLogic, Strict_Linearity) {
    hn4_volume_t* vol = create_pico_fixture();
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    uint64_t V = 1;
    
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba0, lba1, lba2;
    uint8_t k;
    
    /* Alloc N=0, 1, 2 */
    hn4_alloc_block(vol, &anchor, 0, &lba0, &k);
    hn4_alloc_block(vol, &anchor, 1, &lba1, &k);
    hn4_alloc_block(vol, &anchor, 2, &lba2, &k);
    
    uint64_t p0 = *(uint64_t*)&lba0;
    uint64_t p1 = *(uint64_t*)&lba1;
    uint64_t p2 = *(uint64_t*)&lba2;
    
    ASSERT_EQ(p0 + 1, p1);
    ASSERT_EQ(p1 + 1, p2);
    
    cleanup_pico_fixture(vol);
}