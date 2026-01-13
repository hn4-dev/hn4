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
    uint16_t M = 0;
    
    /* 1. Manually occupy K=0 */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, N, M, 0);
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
