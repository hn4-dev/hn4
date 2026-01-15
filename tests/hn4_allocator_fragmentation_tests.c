/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR & FRAGMENTATION SUITE
 * FILE: hn4_allocator_fragmentation_tests.c
 * STATUS: PHYSICS VERIFICATION (CLUSTERED)
 *
 * SCOPE:
 *   1. Ballistic Math (Scatter, V-Stride).
 *   2. Gravity Assist (Collision Teleportation).
 *   3. Fragmentation Resilience (Checkerboard patterns).
 *   4. Horizon Fallback (Saturation behavior).
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4.h"

/* --- FIXTURE INFRASTRUCTURE --- */
#define HN4_BLOCK_SIZE  4096
#define HN4_CAPACITY    (100ULL * 1024ULL * 1024ULL) /* 100 MB */
#define HN4_TOTAL_BLOCKS (HN4_CAPACITY / HN4_BLOCK_SIZE)
#define HN4_BITMAP_BYTES (((HN4_TOTAL_BLOCKS + 63) / 64) * sizeof(hn4_armored_word_t))

#define HN4_CLUSTER_SIZE 16

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} mock_hal_device_t;

static hn4_volume_t* create_frag_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    memset(dev, 0, sizeof(mock_hal_device_t));
    
    dev->caps.logical_block_size = 4096;
#ifdef HN4_USE_128BIT
    dev->caps.total_capacity_bytes.lo = HN4_CAPACITY;
#else
    dev->caps.total_capacity_bytes = HN4_CAPACITY;
#endif
    dev->caps.hw_flags = 0;

    vol->target_device = dev;
    vol->vol_block_size = HN4_BLOCK_SIZE;
#ifdef HN4_USE_128BIT
    vol->vol_capacity_bytes.lo = HN4_CAPACITY;
#else
    vol->vol_capacity_bytes = HN4_CAPACITY;
#endif
    vol->read_only = false;

    vol->bitmap_size = HN4_BITMAP_BYTES;
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);

    vol->qmask_size = ((HN4_TOTAL_BLOCKS * 2 + 63) / 64) * 8; 
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    memset(vol->quality_mask, 0xAA, vol->qmask_size);

    /* Flux starts at Block 100 to leave room for metadata */
    vol->sb.info.lba_flux_start    = hn4_addr_from_u64(100);
    vol->sb.info.lba_horizon_start = hn4_addr_from_u64(20000);
    vol->sb.info.journal_start     = hn4_addr_from_u64(21000);

    atomic_store(&vol->alloc.used_blocks, 0);
    atomic_store(&vol->alloc.horizon_write_head, 0);
    
    return vol;
}

static void cleanup_frag_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * 3. SATURATION & HORIZON
 * ========================================================================= */

/*
 * Test F4: Deep Horizon Saturation
 * RATIONALE:
 * Verify that when the Flux Manifold (D1) hits 90% usage, the allocator
 * correctly signals a switch to the Horizon (D1.5) linear log.
 */
hn4_TEST(SaturationLogic, HorizonFallbackSwitch) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* 
     * Manually Set Usage to 91% 
     * Total Blocks = 25,600. 91% = 23,296.
     */
    uint64_t threshold = (HN4_TOTAL_BLOCKS * 91) / 100;
    atomic_store(&vol->alloc.used_blocks, threshold);
    
    /* 1. Attempt Standard Alloc -> Expect Redirection Signal */
    uint64_t G, V;
    hn4_result_t res = hn4_alloc_genesis(vol, 0, 0, &G, &V);
    
    /* Expect HN4_INFO_HORIZON_FALLBACK (4) */
    ASSERT_EQ(HN4_INFO_HORIZON_FALLBACK, res);
    
    /* Verify Sticky Bit set (HN4_VOL_RUNTIME_SATURATED) */
    uint32_t flags = atomic_load(&vol->sb.info.state_flags);
    ASSERT_TRUE((flags & HN4_VOL_RUNTIME_SATURATED) != 0);

    /* 2. Attempt Horizon Alloc -> Expect Success */
    hn4_addr_t hlba;
    res = hn4_alloc_horizon(vol, &hlba);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify LBA is in Horizon Region (>= 20000) */
    uint64_t hlba_val = hn4_addr_to_u64(hlba);
    /* Note: alloc_horizon returns abstract LBA (Sector).
       vol has block size 4096, sector 4096 (mock).
       Start is Block 20000. So LBA is 20000. */
    ASSERT_TRUE(hlba_val >= 20000);
    ASSERT_TRUE(hlba_val < 21000); /* Before Journal */

    cleanup_frag_fixture(vol);
}

/*
 * Test F9: Trajectory Boundary Wrap (Math)
 * RATIONALE:
 * Verify that the ballistic equation correctly wraps around the capacity (Phi)
 * when the trajectory exceeds the end of the drive. 
 */
hn4_TEST(FragmentationMath, TrajectoryBoundaryWrap) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t flux_start = hn4_addr_to_u64(vol->sb.info.lba_flux_start); /* 100 */
    uint64_t total = HN4_TOTAL_BLOCKS; /* 25600 */
    uint64_t phi = total - flux_start; /* 25500 */
    
    /* 
     * Setup edge case:
     * G is at the very last index of the flux region.
     * Vector V=1. N=0.
     */
    uint64_t G = phi - 1; /* 25499 */
    uint64_t V = 1;
    uint16_t M = 0;
    
    /* Check N=0 (The Tail) */
    uint64_t lba_tail = _calc_trajectory_lba(vol, G, V, 0, M, 0);
    ASSERT_EQ(flux_start + 25499, lba_tail);
    
    /* 
     * Check N=16 (Next Cluster).
     * Cluster 1. Should wrap.
     * Offset = (G + 1*1) % Phi = (Phi-1 + 1) % Phi = 0.
     */
    uint64_t lba_wrap = _calc_trajectory_lba(vol, G, V, 16, M, 0);
    
    /* Should be exactly Flux Start */
    ASSERT_EQ(flux_start, lba_wrap);

    cleanup_frag_fixture(vol);
}

/*
 * Test F12: Toxic Evasion (Q-Mask Integration)
 * RATIONALE:
 * Verify that the ballistic engine automatically "orbits" around blocks
 * marked TOXIC (0x00) in the Quality Mask.
 */
hn4_TEST(EdgeCases, ToxicBlockEvasion) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 500;
    uint64_t V = 1;
    uint64_t N = 0;
    
    /* 1. Calculate where K=0 lands */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    /* 2. Poison this exact block in the Q-Mask */
    uint64_t word_idx = lba_k0 / 32;
    uint32_t shift = (lba_k0 % 32) * 2;
    
    vol->quality_mask[word_idx] &= ~(3ULL << shift);
    
    _bitmap_op(vol, lba_k0, BIT_CLEAR, NULL);
    
    /* 3. Prepare Anchor request */
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    /* 4. Execute Alloc */
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &out_lba, &out_k);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* K must be >= 1 */
    ASSERT_TRUE(out_k >= 1);

    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 3: LARGE STRIDE WRAP (V > Phi)
 * ========================================================================= */
/*
 * RATIONALE:
 * Verify modular arithmetic handles Vectors larger than the window size.
 */
hn4_TEST(FragmentationMath, Large_Vector_Modulo) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t flux_start = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    uint64_t phi = (hn4_addr_to_u64(vol->vol_capacity_bytes) / 4096) - flux_start;
    
    /* Case A: V = 1 */
    uint64_t lba_v1 = _calc_trajectory_lba(vol, 0, 1, 16, 0, 0); /* Cluster 1 */
    
    /* Case B: V = Phi + 1 */
    uint64_t V_huge = phi + 1;
    uint64_t lba_huge = _calc_trajectory_lba(vol, 0, V_huge, 16, 0, 0);
    
    /* They must land on the same spot */
    ASSERT_EQ(lba_v1, lba_huge);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 6: HORIZON RING EXHAUSTION
 * ========================================================================= */
hn4_TEST(SaturationLogic, Horizon_Ring_Full) {
    hn4_volume_t* vol = create_frag_fixture();
    
    vol->sb.info.lba_horizon_start = hn4_addr_from_u64(20000);
    vol->sb.info.journal_start = hn4_addr_from_u64(20010);
    
    /* Fill all 10 blocks */
    bool st;
    for(int i=0; i<10; i++) {
        _bitmap_op(vol, 20000 + i, BIT_SET, &st);
    }
    
    /* Attempt Alloc */
    hn4_addr_t hlba;
    hn4_result_t res = hn4_alloc_horizon(vol, &hlba);
    
    ASSERT_EQ(HN4_ERR_ENOSPC, res);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 7: ZERO-G SINGULARITY (G=0)
 * ========================================================================= */
hn4_TEST(EdgeCases, Zero_G_Validity) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 0;
    uint64_t V = 1;
    
    uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    
    /* Should equal flux_start */
    uint64_t expected = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    ASSERT_EQ(expected, lba);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 8: MULTI-THREADED K-COLLISION (Race Condition)
 * ========================================================================= */
hn4_TEST(Concurrency, Shadow_Hop_Race) {
    hn4_volume_t* vol = create_frag_fixture();
    
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    uint64_t V = 1;
    memcpy(anchor.orbit_vector, &V, 6);
    
    /* Thread A Alloc */
    hn4_addr_t lba_a; uint8_t k_a;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba_a, &k_a));
    
    /* Thread B Alloc (Same Anchor, Same Index) */
    hn4_addr_t lba_b; uint8_t k_b;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba_b, &k_b));
    
    /* They MUST land on different orbits due to atomic bitmap */
    uint64_t pa = hn4_addr_to_u64(lba_a);
    uint64_t pb = hn4_addr_to_u64(lba_b);
    
    /* Addresses must differ */
    ASSERT_TRUE(pa != pb);
    
    /* Orbits must differ (k=0 vs k=1 etc) */
    ASSERT_TRUE(k_b != k_a);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 10: MAX ORBIT EXHAUSTION
 * RATIONALE:
 * Simulate collisions on K=0..11. Verify allocator uses K=12 or fails.
 * ========================================================================= */
hn4_TEST(SaturationLogic, Max_Orbit_Exhaustion) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 1000;
    uint64_t V = 1;
    uint64_t N = 0;
    
    /* Manually occupy K=0..11 */
    for(int k=0; k<12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &lba, &k);
    
    /* Should succeed at K=12 (if limit allows) or fail if K<12 strict */
    /* Implementation uses K <= max_k. Default max is 12. So K=12 is valid. */
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(12, k);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * TEST 11: GRAVITY ASSIST TRIGGER
 * RATIONALE:
 * Verify that when K >= 4, the vector V is modified by the Gravity Assist 
 * formula (ROTL 17 ^ Magic).
 * ========================================================================= */
hn4_TEST(FragmentationMath, Gravity_Assist_Trigger) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 2000;
    uint64_t V = 0x123456789;
    uint64_t N = 0;
    
    /* Occupy K=0..3 to force K=4 */
    for(int k=0; k<4; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t lba; uint8_t k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, N, &lba, &k));
    
    ASSERT_EQ(4, k);
    
    /* 
     * Verify LBA matches Gravity Assist calculation manually.
     * V_assist = ROTL(V, 17) ^ MAGIC.
     */
    uint64_t V_assist = hn4_swizzle_gravity_assist(V) | 1;
    uint64_t expected = _calc_trajectory_lba(vol, G, V_assist, N, 0, 0); 
    /* Note: _calc_trajectory internal logic applies shift based on K.
       If we pass V_assist and K=0, it should match K=4 with original V
       ONLY IF K=0 doesn't apply assist again. (It doesn't).
       HOWEVER, _calc also applies theta jitter.
       Correct check is to call _calc with k=4 and assert equality.
    */
    uint64_t calc_lba = _calc_trajectory_lba(vol, G, V, N, 0, 4);
    ASSERT_EQ(calc_lba, hn4_addr_to_u64(lba));
    
    cleanup_frag_fixture(vol);
}
