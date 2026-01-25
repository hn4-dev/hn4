/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR COLLISION SUITE
 * FILE: hn4_allocator_collision_tests.c
 * STATUS: FIXED & CALIBRATED
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_endians.h"
#include <pthread.h> /* For Concurrency Tests */
#include <string.h>

#define HN4_LBA_INVALID UINT64_MAX

/* Local helper to verify internal math since _gcd is static */
static uint64_t _test_gcd(uint64_t a, uint64_t b) {
    while (b != 0) { uint64_t t = b; b = a % b; a = t; }
    return a;
}

/* --- FIXTURE REUSE --- */
/* Reuse create_alloc_fixture from previous file or redefine minimal version here */
/* For standalone compilation, we redefine a minimal safe fixture. */

#define FIXTURE_CAPACITY (100ULL * 1024ULL * 1024ULL) /* 100 MB = 25,600 Blocks */
#define FIXTURE_BS       4096

typedef struct {
    hn4_hal_caps_t caps;
    void* mmio;
    void* ctx;
} mock_dev_t;

static hn4_volume_t* create_collision_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    mock_dev_t* dev = hn4_hal_mem_alloc(sizeof(mock_dev_t));
    dev->caps.logical_block_size = 4096;
    dev->caps.total_capacity_bytes = FIXTURE_CAPACITY;
    dev->caps.hw_flags = HN4_HW_NVM; 
    
    vol->target_device = dev;
    vol->vol_block_size = FIXTURE_BS;
    vol->vol_capacity_bytes = FIXTURE_CAPACITY;
    
    vol->sb.info.device_type_tag = HN4_DEV_SSD; 
    vol->sb.info.format_profile  = HN4_PROFILE_GENERIC;
    
    uint64_t total_blocks = FIXTURE_CAPACITY / FIXTURE_BS;
    vol->bitmap_size = ((total_blocks + 63) / 64) * 16;
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);
    
    vol->qmask_size = (total_blocks * 2 + 7) / 8;
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    memset(vol->quality_mask, 0xAA, vol->qmask_size); 
    
    /* Valid Geometry within 100MB */
    vol->sb.info.lba_flux_start    = 100;
    vol->sb.info.lba_horizon_start = 20000; /* ~80MB mark */
    vol->sb.info.journal_start     = 24000; /* ~94MB mark */
    
    return vol;
}

static void cleanup_collision_fixture(hn4_volume_t* vol) {
    if (vol) {
        hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol->void_bitmap);
        hn4_hal_mem_free(vol->quality_mask);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 3: HDD INERTIAL DAMPER (Strict K=0 + Fallback)
 * ========================================================================= */
/*
 * RATIONALE:
 * Verify HDD forces K=0. If K=0 is full, it skips K=1..12 and goes straight 
 * to Horizon (K=15).
 */
hn4_TEST(DevicePhysics, Hdd_InertialDamper_Fallback) {
    hn4_volume_t* vol = create_collision_fixture();
    
    /* Configure as HDD */
    vol->sb.info.device_type_tag = HN4_DEV_HDD;
    mock_dev_t* mdev = (mock_dev_t*)vol->target_device;
    mdev->caps.hw_flags |= HN4_HW_ROTATIONAL;
    
    /* Ensure Horizon is Valid (20000) */
    vol->sb.info.lba_horizon_start = 20000;

    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(1000);
    anchor.orbit_vector[0] = 1; 

    /* 1. Manually Occupy K=0 */
    /* M=0, N=0, K=0 */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, 1000, 1, 0, 0, 0);
    bool state;
    _bitmap_op(vol, lba_k0, BIT_SET, &state);

    /* 2. Attempt Allocation */
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k);

    /* 
     * EXPECTATION: Success via Horizon.
     * If K=1 was attempted, it would succeed at K=1 (Error).
     * If Fallback failed (OOB), it would be GEOMETRY/COLLAPSE (Error).
     */
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(15, out_k); /* Proven Fallback */

    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 1. STRICT K-ORDER GUARANTEES
 * ========================================================================= */
hn4_TEST(Collision, StrictKOrder) {
    hn4_volume_t* vol = create_collision_fixture();
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    uint64_t V = 3;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* 1. Jam K=0..10 */
    bool st;
    for (int k = 0; k <= 10; k++) {
        /* M=0, N=0 */
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, &st);
    }
    
    /* 2. Alloc */
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Expect K=11 */
    ASSERT_EQ(11, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 2. K EXHAUSTION -> HORIZON ONLY AFTER K=12
 * ========================================================================= */
hn4_TEST(Collision, HorizonAfterK12) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    uint64_t V = 3;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* 1. Jam K=0..12 */
    bool st;
    for (int k = 0; k <= 12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, &st);
    }
    
    /* 2. Alloc */
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Expect K=15 (Horizon) */
    ASSERT_EQ(15, out_k);
    
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_TRUE(val >= 20000);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 3. K SLOT REUSE CONSISTENCY
 * ========================================================================= */
hn4_TEST(Collision, SlotReuseConsistency) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    uint64_t V = 3;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* 1. Alloc (Gets K=0) */
    hn4_addr_t lba1;
    uint8_t k1;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba1, &k1));
    ASSERT_EQ(0, k1);
    
    /* 2. Free it */
    uint64_t val = *(uint64_t*)&lba1;
    hn4_free_block(vol, val);
    
    /* 3. Alloc Again */
    hn4_addr_t lba2;
    uint8_t k2;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba2, &k2));
    
    /* Must reuse K=0 */
    ASSERT_EQ(0, k2);
    
    uint64_t val2 = *(uint64_t*)&lba2;
    ASSERT_EQ(val, val2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 4. VECTOR MUTATION PHYSICS (G Stability)
 * ========================================================================= */
hn4_TEST(Collision, VectorMutationDoesNotShiftG) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t G = 1000;
    uint64_t V = 1;
    /* Use N=16 to ensure we are in Cluster 1 where V matters */
    /* With M=4, N=16 (Cluster 1) works. With M=0, N=1 (Cluster 1) works. */
    /* Let's assume M=0 default. */
    uint64_t N = 1; 
    
    /* K=3 (Base V) */
    uint64_t lba3 = _calc_trajectory_lba(vol, G, V, N, 0, 3);
    
    /* K=4 (Mutated V) */
    uint64_t lba4 = _calc_trajectory_lba(vol, G, V, N, 0, 4);
    
    /* 
     * Formula: G + N*V + Theta
     * If G shifted, both would drift.
     * We know V mutates. G must be constant.
     * Let's verify G's contribution.
     * With N=0, Vector term vanishes.
     */
    uint64_t lba3_n0 = _calc_trajectory_lba(vol, G, V, 0, 0, 3);
    uint64_t lba4_n0 = _calc_trajectory_lba(vol, G, V, 0, 0, 4);
    
    /* 
     * At N=0:
     * K=3: G + Theta(3) = G+6
     * K=4: G + Theta(4) = G+10
     * Diff should be 4.
     */
    uint64_t diff = (lba4_n0 >= lba3_n0) ? (lba4_n0 - lba3_n0) : (lba4_n0 + 25000 - lba3_n0); /* Approx modulo handling */
    ASSERT_EQ(4ULL, diff);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 7. G RIGHT ON FLUX BOUNDARY (Wrap Test)
 * ========================================================================= */
hn4_TEST(Collision, G_BoundaryWrap) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t total = FIXTURE_CAPACITY / FIXTURE_BS;
    uint64_t start = vol->sb.info.lba_flux_start;
    uint64_t phi = total - start;
    
    /* G = Phi - 1 */
    uint64_t G = phi - 1;
    uint64_t V = 1;
    
    /* Alloc K=0..2 */
    /* K=0: G + 0 = Phi-1 (End) */
    /* K=1: G + 1 = Phi (Wrap to 0) */
    /* K=2: G + 3 = Phi+2 (Wrap to 2) */
    
    uint64_t lba0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    uint64_t lba1 = _calc_trajectory_lba(vol, G, V, 0, 0, 1);
    uint64_t lba2 = _calc_trajectory_lba(vol, G, V, 0, 0, 2);
    
    ASSERT_EQ(start + phi - 1, lba0);
    ASSERT_EQ(start + 0, lba1);
    ASSERT_EQ(start + 2, lba2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 9. HDD BYPASS ENFORCEMENT AUDIT
 * ========================================================================= */
hn4_TEST(Collision, HDD_BypassEnforcement) {
    hn4_volume_t* vol = create_collision_fixture();
    
    /* Configure as HDD */
    vol->sb.info.device_type_tag = HN4_DEV_HDD;
    mock_dev_t* mdev = (mock_dev_t*)vol->target_device;
    mdev->caps.hw_flags |= HN4_HW_ROTATIONAL;
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1; /* V=1 */
    
    /* Occupy K=0 */
    uint64_t lba0 = _calc_trajectory_lba(vol, 1000, 1, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_SET, NULL);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* HDD must NOT try K=1. It must jump to Horizon (K=15) */
    ASSERT_EQ(15, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 14. TOXIC K SLOT (Quality Mask Interaction)
 * ========================================================================= */
hn4_TEST(Collision, ToxicSlotSkip) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* 
     * K=0 LBA = 1000 + 100 (flux start) = 1100.
     * Mark 1100 as TOXIC in QMask.
     */
    uint64_t lba_k0 = 1100;
    uint64_t word_idx = lba_k0 / 32;
    uint32_t shift = (lba_k0 % 32) * 2;
    
    /* Set TOXIC (00) */
    uint64_t mask = ~(3ULL << shift);
    vol->quality_mask[word_idx] &= mask;
    
    /* Alloc */
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Must skip K=0 and take K=1 */
    ASSERT_NEQ(0, out_k);
    ASSERT_EQ(1, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 16. IDENTICAL ANCHORS -> IDENTICAL LADDER
 * ========================================================================= */
hn4_TEST(Collision, DeterministicLadder) {
    hn4_volume_t* vol = create_collision_fixture();
    
    /* Run 1 */
    hn4_anchor_t a1 = {0};
    a1.gravity_center = hn4_cpu_to_le64(5000);
    a1.orbit_vector[0] = 7;
    
    /* Run 2 */
    hn4_anchor_t a2 = {0};
    a2.gravity_center = hn4_cpu_to_le64(5000);
    a2.orbit_vector[0] = 7;
    
    /* Compare Trajectory Calculations */
    for (int k = 0; k <= 12; k++) {
        uint64_t lba1 = _calc_trajectory_lba(vol, 5000, 7, 0, 0, k);
        uint64_t lba2 = _calc_trajectory_lba(vol, 5000, 7, 0, 0, k);
        
        ASSERT_EQ(lba1, lba2);
    }
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 17. COLLISION -> HORIZON -> BACK TO D1
 * ========================================================================= */
hn4_TEST(Collision, HealingBeatsFallback) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* 1. Jam K=0..12 */
    bool st;
    for(int k=0; k<=12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, 1, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, &st);
    }
    
    /* 2. Alloc -> Horizon */
    hn4_addr_t lba1;
    uint8_t k1;
    hn4_alloc_block(vol, &anchor, 0, &lba1, &k1);
    ASSERT_EQ(15, k1);
    
    /* 3. Free K=0 */
    uint64_t k0_lba = _calc_trajectory_lba(vol, G, 1, 0, 0, 0);
    _bitmap_op(vol, k0_lba, BIT_CLEAR, &st);
    
    /* 4. Alloc Again -> D1 wins */
    hn4_addr_t lba2;
    uint8_t k2;
    hn4_alloc_block(vol, &anchor, 0, &lba2, &k2);
    
    ASSERT_EQ(0, k2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 1. DETERMINISTIC K-ORDERING GUARANTEE
 * ========================================================================= */
hn4_TEST(Collision, DeterministicKOrdering) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000, V = 13;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* Target: K=11. Occupy 0..10. */
    bool st;
    for (int k = 0; k <= 10; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, &st);
    }
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Verify K=11 */
    ASSERT_EQ(11, out_k);
    
    /* Verify LBA matches math */
    uint64_t expected = _calc_trajectory_lba(vol, G, V, 0, 0, 11);
    uint64_t actual = *(uint64_t*)&out_lba;
    ASSERT_EQ(expected, actual);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 2. SAME ANCHOR + SAME N -> SAME ORBIT
 * ========================================================================= */
hn4_TEST(Collision, IdempotentTrajectory) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 2000, V = 7;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* Fill K=0,1,2 */
    for (int k = 0; k <= 2; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Alloc 1 (Should be K=3) */
    hn4_addr_t lba1;
    uint8_t k1;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba1, &k1));
    ASSERT_EQ(3, k1);
    
    /* Free it to reset state for re-alloc */
    uint64_t val = *(uint64_t*)&lba1;
    hn4_result_t res = _bitmap_op(vol, val, BIT_CLEAR, NULL);
    ASSERT_EQ(HN4_OK, res);
    
    /* Alloc 2 (Should also be K=3) */
    hn4_addr_t lba2;
    uint8_t k2;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba2, &k2));
    
    ASSERT_EQ(3, k2);
    uint64_t val2 = *(uint64_t*)&lba2;
    ASSERT_EQ(val, val2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 3. CROSS-ANCHOR NON-INTERFERENCE
 * ========================================================================= */
hn4_TEST(Collision, CrossAnchorIsolation) {
    hn4_volume_t* vol = create_collision_fixture();
    
    hn4_anchor_t A = {0}, B = {0};
    uint64_t GA = 1000, VA = 7;
    uint64_t GB = 5000, VB = 13;
    
    A.gravity_center = hn4_cpu_to_le64(GA); A.orbit_vector[0] = (uint8_t)VA;
    B.gravity_center = hn4_cpu_to_le64(GB); B.orbit_vector[0] = (uint8_t)VB;
    
    /* Force A to collide up to K=5 */
    for (int k = 0; k <= 5; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, GA, VA, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Alloc B (Should be K=0, unaffected by A's collisions) */
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &B, 0, &out_lba, &out_k));
    
    ASSERT_EQ(0, out_k);
    
    cleanup_collision_fixture(vol);
}


/* =========================================================================
 * 6. THETA LUT INTEGRITY
 * ========================================================================= */
hn4_TEST(Collision, ThetaMonotonicity) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t G = 0, V = 1;
    
    /* Theta values: 0, 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 66, 78 */
    /* Delta should be increasing: 1, 2, 3, 4, 5... */
    
    uint64_t prev_lba = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    
    /* Start at k=1 (k=4 mutates vector, so check 0..3 linearity first) */
    for (int k = 1; k < 4; k++) {
        uint64_t curr = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        
        /* Delta = Theta[k] - Theta[k-1] */
        /* k=1: 1-0=1. k=2: 3-1=2. k=3: 6-3=3. */
        uint64_t diff = curr - prev_lba;
        ASSERT_EQ((uint64_t)k, diff);
        
        prev_lba = curr;
    }
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 7. LBA COLLISION MUST ADVANCE K
 * ========================================================================= */
hn4_TEST(Collision, BusyLbaSkipped) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 5000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Calculate K=0 LBA */
    uint64_t lba0 = _calc_trajectory_lba(vol, G, 1, 0, 0, 0);
    
    /* Occupy it */
    bool st;
    _bitmap_op(vol, lba0, BIT_SET, &st);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k);
    
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_NEQ(lba0, val);
    ASSERT_EQ(1, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 10. DEVICE-PHYSICS BRANCH TESTS (HDD)
 * ========================================================================= */
hn4_TEST(DevicePhysics, Hdd_ZeroOrbit) {
    hn4_volume_t* vol = create_collision_fixture();
    vol->sb.info.device_type_tag = HN4_DEV_HDD;
    /* Ensure Horizon fallback available */
    vol->sb.info.lba_horizon_start = 20000;
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(1000);
    anchor.orbit_vector[0] = 1;
    
    /* Occupy K=0 */
    uint64_t lba0 = _calc_trajectory_lba(vol, 1000, 1, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_SET, NULL);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* HDD must NOT try K=1. It must jump to Horizon (K=15) */
    ASSERT_EQ(15, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 1. COLLISION CASCADING STABILITY (Orbit Immutability)
 * ========================================================================= */
hn4_TEST(Collision, OrbitGeometryImmutability) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t G = 1000, V = 13;
    
    /* 1. Pre-Compute Golden Orbits (K=0..7) */
    uint64_t golden[8];
    for(int k=0; k<8; k++) {
        golden[k] = _calc_trajectory_lba(vol, G, V, 0, 0, k);
    }
    
    /* 2. Occupy some slots (2, 3, 6) */
    _bitmap_op(vol, golden[2], BIT_SET, NULL);
    _bitmap_op(vol, golden[3], BIT_SET, NULL);
    _bitmap_op(vol, golden[6], BIT_SET, NULL);
    
    /* 3. Re-Compute and Verify Stability */
    /* Allocating shouldn't change the MATH of the orbit, just the SELECTION */
    /* We verify the math function itself is stateless wrt bitmap */
    for(int k=0; k<8; k++) {
        uint64_t recalc = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        ASSERT_EQ(golden[k], recalc);
    }
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 3. THETA-ONLY COLLISION (V=0 degenerate case)
 * ========================================================================= */
hn4_TEST(Collision, ThetaOnlySpread) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t G = 1000, V = 0; /* Pure Theta mode */
    
    /* Theta LUT: 0, 1, 3, 6, 10, 15... */
    
    /* Record LBA(k) */
    uint64_t lba[6];
    for(int k=0; k<6; k++) {
        lba[k] = _calc_trajectory_lba(vol, G, V, 0, 0, k);
    }
    
    /* Check Deltas match Theta diffs */
    ASSERT_EQ(1ULL, lba[1] - lba[0]); /* 1-0 = 1 */
    ASSERT_EQ(2ULL, lba[2] - lba[1]); /* 3-1 = 2 */
    ASSERT_EQ(3ULL, lba[3] - lba[2]); /* 6-3 = 3 */
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 11. DEAD-ZONE COLLISION (Starvation Stability)
 * ========================================================================= */
hn4_TEST(Collision, DeadZoneDegeneracy) {
    hn4_volume_t* vol = create_collision_fixture();
    /* Force Phi=100 */
    uint64_t start = vol->sb.info.lba_flux_start;
    vol->vol_capacity_bytes = (start + 100) * 4096;
    
    /* V=50. Orbit visits 0, 50, 0, 50... */
    hn4_anchor_t anchor = {0};
    uint64_t G = 0, V = 50;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* 
     * Even if orbit math (N*V) is degenerate, Theta(k) should provide escape.
     * Theta: 0, 1, 3, 6...
     * LBA: (0 + 0 + 0)%100 = 0
     *      (0 + 0 + 1)%100 = 1
     *      (0 + 0 + 3)%100 = 3
     * So K-ladder saves us from V-degeneracy.
     */
    
    /* Block K=0 (LBA 0) */
    uint64_t lba0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_SET, NULL);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Should escape to K=1 */
    ASSERT_EQ(1, out_k);
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_NEQ(val, lba0);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 12. COLLISION AFTER FREE (Healing)
 * ========================================================================= */
hn4_TEST(Collision, HealingPathIntegrity) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000, V = 1;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Force K=5 allocation first */
    for(int k=0; k<5; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Now free K=0 */
    uint64_t k0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    _bitmap_op(vol, k0, BIT_CLEAR, NULL);
    
    /* Alloc. Should grab K=0, not remember K=5 depth */
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k);
    
    ASSERT_EQ(0, out_k);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * FINAL BOSS #1: AVALANCHE COLLISION CASCADE
 * ========================================================================= */
hn4_TEST(Collision, AvalancheCascade) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000, V = 10; /* V=10 ensures spacing */
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 10;
    
    const int COUNT = 100; /* 1000 might be slow for unit test, 100 is enough proof */
    
    for (int n = 0; n < COUNT; n++) {
        /* Jam K=0..3 for this N */
        for(int k=0; k<4; k++) {
            uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, k);
            _bitmap_op(vol, lba, BIT_SET, NULL);
        }
        
        /* Alloc */
         hn4_addr_t out_lba;
        uint8_t out_k;
        ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, n, &out_lba, &out_k));
        
        /* 
         * Verify we skipped the jammed layers (0..3).
         * Due to inter-N collisions in a dense test, it might be > 4.
         * But it MUST be >= 4.
         */
        ASSERT_TRUE(out_k >= 4);
    }
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * FINAL BOSS #2: REVERSE COLLISION (Priority)
 * ========================================================================= */
hn4_TEST(Collision, ReversePriority) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000, V = 1;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Fill K=0..7 */
    for(int k=0; k<=7; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Alloc -> Gets K=8 */
    hn4_addr_t lba8;
    uint8_t k8;
    hn4_alloc_block(vol, &anchor, 0, &lba8, &k8);
    ASSERT_EQ(8, k8);
    
    /* Free K=7 (Higher up the ladder) */
    uint64_t lba7 = _calc_trajectory_lba(vol, G, V, 0, 0, 7);
    _bitmap_op(vol, lba7, BIT_CLEAR, NULL);
    
    /* Also Free K=0 (Lowest) */
    uint64_t lba0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_CLEAR, NULL);
    
    /* Alloc Again. MUST take K=0 (Lowest available), not K=7 or K=9. */
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k);
    
    ASSERT_EQ(0, out_k);
    
    cleanup_collision_fixture(vol);
}


/* =========================================================================
 * 1. SAME-ANCHOR PILE-UP (Fixed G/V, Rising N)
 * ========================================================================= */
hn4_TEST(Collision, SameAnchorPileUp) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000, V = 13;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    uint64_t resolved_lbas[11];
    
    /* Force collisions on K=0 for every N */
    for(int n=0; n<=10; n++) {
        /* Cluster logic N/16. But here we increment N by 1. 
           With M=0, N=0..10 are distinct blocks. 
           If M=4, N=0..10 all map to same cluster -> SAME K=0 LBA.
           Then `bitmap_op` sees it's set.
           The test assumes M=0 (S=1).
        */
        
        uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        _bitmap_op(vol, lba_k0, BIT_SET, NULL);
        
        hn4_addr_t out_lba;
        uint8_t out_k;
        ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, n, &out_lba, &out_k));
        resolved_lbas[n] = *(uint64_t*)&out_lba;
        
        /* Must have escalated past K=0 */
        ASSERT_NEQ(0, out_k);
    }
    
    /* Verify all resolved LBAs are unique */
    for(int i=0; i<=10; i++) {
        for(int j=i+1; j<=10; j++) {
            ASSERT_NEQ(resolved_lbas[i], resolved_lbas[j]);
        }
    }
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 2. CROSS-ANCHOR COLLISION STORM (Different G, Same V)
 * ========================================================================= */
hn4_TEST(Collision, CrossAnchorStorm) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t A = {0}, B = {0};
    uint64_t V = 7;
    A.gravity_center = hn4_cpu_to_le64(1000);
    B.gravity_center = hn4_cpu_to_le64(1004);
    A.orbit_vector[0] = (uint8_t)V;
    B.orbit_vector[0] = (uint8_t)V;
    
    /* Jam K=0..2 for both */
    for(int k=0; k<3; k++) {
        uint64_t lbaA = _calc_trajectory_lba(vol, 1000, V, 0, 0, k);
        uint64_t lbaB = _calc_trajectory_lba(vol, 1004, V, 0, 0, k);
        _bitmap_op(vol, lbaA, BIT_SET, NULL);
        _bitmap_op(vol, lbaB, BIT_SET, NULL);
    }
    
    hn4_addr_t lbaA, lbaB;
    uint8_t kA, kB;
    
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &A, 0, &lbaA, &kA));
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &B, 0, &lbaB, &kB));
    
    /* Both should escalate to K=3 (or higher if they collided with each other) */
    ASSERT_TRUE(kA >= 3);
    ASSERT_TRUE(kB >= 3);
    
    /* Must not be same block */
    uint64_t valA = *(uint64_t*)&lbaA;
    uint64_t valB = *(uint64_t*)&lbaB;
    ASSERT_NEQ(valA, valB);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 4. THETA-ONLY COLLISION AMPLIFIER
 * ========================================================================= */
hn4_TEST(Collision, ThetaOnlyAmplifier) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 2000, V = 1;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Jam K=0..11 */
    for(int k=0; k<12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* Alloc. Should get K=12 */
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    ASSERT_EQ(12, out_k);
    
    /* LBA(12) = G + Theta[12]. Theta[12]=78. */
    /* V=1, N=0 -> G + 78 */
    uint64_t flux_start = vol->sb.info.lba_flux_start;
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_EQ(flux_start + G + 78, val);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 6. DETERMINISM TEST
 * ========================================================================= */
hn4_TEST(Collision, DeterminismCheck) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 3000, V = 11;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* Randomly occupy some K slots */
    _bitmap_op(vol, _calc_trajectory_lba(vol, G, V, 0, 0, 0), BIT_SET, NULL);
    _bitmap_op(vol, _calc_trajectory_lba(vol, G, V, 0, 0, 2), BIT_SET, NULL);
    
    /* Run 1 */
    hn4_addr_t lba1;
    uint8_t k1;
    hn4_alloc_block(vol, &anchor, 0, &lba1, &k1);
    
    /* Reset Allocation only (free the block we just took) */
    uint64_t val1 = *(uint64_t*)&lba1;
    _bitmap_op(vol, val1, BIT_CLEAR, NULL);
    
    /* Run 2 */
    hn4_addr_t lba2;
    uint8_t k2;
    hn4_alloc_block(vol, &anchor, 0, &lba2, &k2);
    
    ASSERT_EQ(k1, k2);
    uint64_t val2 = *(uint64_t*)&lba2;
    ASSERT_EQ(val1, val2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 8. COLLISION UNDER WRAPAROUND
 * ========================================================================= */
hn4_TEST(Collision, WraparoundStability) {
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t total = FIXTURE_CAPACITY / FIXTURE_BS;
    uint64_t start = vol->sb.info.lba_flux_start;
    uint64_t phi = total - start;
    
    /* G at end of ring */
    uint64_t G = phi - 5;
    uint64_t V = 1;
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Jam K=0..5. This will wrap around end of Flux */
    /* K=0: phi-5 */
    /* K=5: phi-5 + 15 = phi+10 -> 10. (Wrap) */
    for(int k=0; k<=5; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    ASSERT_EQ(6, out_k);
    /* LBA should be valid and wrapped */
    /* Theta[6] = 21. G+21 = Phi-5+21 = Phi+16 -> 16 */
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_EQ(start + 16, val);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 9. DELIBERATE VECTOR COLLAPSE (V=0)
 * ========================================================================= */
hn4_TEST(Collision, ZeroVectorDefense) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 4000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 0; /* V=0 */
    
    /* 
     * Even with V=0, Theta(k) ensures movement.
     * Jam K=0 (LBA=G)
     */
    uint64_t lba0 = _calc_trajectory_lba(vol, G, 0, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_SET, NULL);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &out_lba, &out_k));
    
    /* Should move to K=1 */
    ASSERT_EQ(1, out_k);
    uint64_t val = *(uint64_t*)&out_lba;
    ASSERT_NEQ(lba0, val);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 1. K-DISTRIBUTION STABILITY (Randomized Load)
 * ========================================================================= */
hn4_TEST(CollisionStats, K_Distribution_Decay) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    
    uint64_t k_counts[16] = {0};
    int total_allocs = 10000; /* Scaled down for unit test speed */
    
    for (int i = 0; i < total_allocs; i++) {
        /* Randomize G, V, N */
        uint64_t G = hn4_hal_get_random_u64();
        uint64_t V = hn4_hal_get_random_u64() | 1;
        uint64_t N = hn4_hal_get_random_u64();
        
        anchor.gravity_center = hn4_cpu_to_le64(G);
        anchor.orbit_vector[0] = (uint8_t)V;
        anchor.orbit_vector[1] = (uint8_t)(V >> 8);
        
        hn4_addr_t out_lba;
        uint8_t out_k;
        if (hn4_alloc_block(vol, &anchor, N, &out_lba, &out_k) == HN4_OK) {
            if (out_k < 16) k_counts[out_k]++;
        }
    }
    
    /* 
     * Verify Exponential Decay:
     * K=0 should be majority.
     * K=1 should be less.
     * K=15 (Horizon) should be rare (< 1% on empty drive).
     */
    ASSERT_TRUE(k_counts[0] > k_counts[1]);
    ASSERT_TRUE(k_counts[1] >= k_counts[2]); /* Allow >= for randomness noise */
    
    /* Horizon check: Should be very low on empty drive */
    /* < 1% of 10000 = 100 */
    ASSERT_TRUE(k_counts[15] < 100);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 2. ADVERSARIAL PHASE-LOCK INJECTION
 * ========================================================================= */
hn4_TEST(CollisionStats, PhaseLockResilience) {
    /* Create fixture with Power-of-2 Phi to invite resonance */
    /* Phi=4096. V=1024. G=0, 1024, 2048... */
    hn4_volume_t* vol = create_collision_fixture();
    uint64_t start = vol->sb.info.lba_flux_start;
    
    /* Mock Capacity to force Phi=4096 */
    vol->vol_capacity_bytes = (start + 4096) * 4096;
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(0);
    anchor.orbit_vector[0] = 0; /* V=1024 is too big for u8 vector[0], need u16 */
    /* Hack: set V via memcpy to vector array if needed, but alloc_block uses 48-bit load */
    uint64_t V = 1024;
    memcpy(anchor.orbit_vector, &V, 6);
    
    /* 
     * Run rising N allocations.
     * V=1024, Phi=4096. Orbit size = 4096/1024 = 4.
     * Points: 0, 1024, 2048, 3072.
     * Should collide heavily after N=4.
     */
    
    int success_count = 0;
    int horizon_count = 0;
    
    for (int n = 0; n < 20; n++) {
        hn4_addr_t out_lba;
        uint8_t out_k;
        if (hn4_alloc_block(vol, &anchor, n, &out_lba, &out_k) == HN4_OK) {
            success_count++;
            if (out_k == 15) horizon_count++;
        }
    }
    
    /* Should succeed via K-ladder or Horizon */
    ASSERT_EQ(20, success_count);
    
    /* Should not infinite loop */
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 3. CROSS-THREAD COLLISION RACE
 * ========================================================================= */
typedef struct {
    hn4_volume_t* vol;
    hn4_anchor_t anchor;
    uint64_t N;
    atomic_int* failures;
} race_ctx_t;

static void* race_worker(void* arg) {
    race_ctx_t* ctx = (race_ctx_t*)arg;
    hn4_addr_t lba;
    uint8_t k;
    if (hn4_alloc_block(ctx->vol, &ctx->anchor, ctx->N, &lba, &k) != HN4_OK) {
        atomic_fetch_add(ctx->failures, 1);
    }
    return NULL;
}

hn4_TEST(CollisionStats, CrossThreadRace) {
    hn4_volume_t* vol = create_collision_fixture();
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(5000);
    anchor.orbit_vector[0] = 7;
    
    /* Force K=0 collision */
    uint64_t lba0 = _calc_trajectory_lba(vol, 5000, 7, 0, 0, 0);
    _bitmap_op(vol, lba0, BIT_SET, NULL);
    
    pthread_t t1, t2;
    atomic_int failures = 0;
    race_ctx_t ctx = {vol, anchor, 0, &failures};
    
    pthread_create(&t1, NULL, race_worker, &ctx);
    pthread_create(&t2, NULL, race_worker, &ctx);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    ASSERT_EQ(0, atomic_load(&failures));
    
    /* Verify K=1 and K=2 (or K=1 for one, K=2 for other) were taken */
    /* Since N is same, they race for K=1 slot. */
    /* Winner takes K=1. Loser sees K=1 busy -> K=2. */
    
    uint64_t lba1 = _calc_trajectory_lba(vol, 5000, 7, 0, 0, 1);
    uint64_t lba2 = _calc_trajectory_lba(vol, 5000, 7, 0, 0, 2);
    
    bool s1, s2;
    _bitmap_op(vol, lba1, BIT_TEST, &s1);
    _bitmap_op(vol, lba2, BIT_TEST, &s2);
    
    ASSERT_TRUE(s1);
    ASSERT_TRUE(s2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 8. 128-BIT MATH INTEGRITY (Precision Loss)
 * ========================================================================= */
hn4_TEST(CollisionStats, LargeNumberPrecision) {
    hn4_volume_t* vol = create_collision_fixture();
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(0);
    
    /* Huge V and N to force overflow if intermediate math is 64-bit */
    /* V = 2^60. N = 2^60. Product 2^120. */
    /* Phi = 25,000 approx. */
    uint64_t V = 1ULL << 60;
    memcpy(anchor.orbit_vector, &V, 6); /* Only lower 48 bits stored usually, but let's test */
    
    /* 
     * Note: orbit_vector is u48. If we pass V > 2^48, it truncates on load.
     * We must ensure test respects this limit or uses direct injection.
     * Let's use max valid V (2^48 - 1).
     */
    V = (1ULL << 48) - 1;
    memcpy(anchor.orbit_vector, &V, 6);
    
    uint64_t N = 1ULL << 60; 
    
    hn4_addr_t lba1, lba2;
    uint8_t k1, k2;
    
    /* Alloc N */
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, N, &lba1, &k1));
    
    /* Alloc N+1 */
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, N+1, &lba2, &k2));
    
    /* LBA1 and LBA2 must be distinct */
    uint64_t val1 = *(uint64_t*)&lba1;
    uint64_t val2 = *(uint64_t*)&lba2;
    ASSERT_NEQ(val1, val2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 9. COLLISION REPLAY CANONICALITY
 * ========================================================================= */
hn4_TEST(CollisionStats, ReplayCanonicality) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 12345;
    uint64_t V = 67;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* Jam random pattern */
    _bitmap_op(vol, _calc_trajectory_lba(vol, G, V, 0, 0, 0), BIT_SET, NULL);
    _bitmap_op(vol, _calc_trajectory_lba(vol, G, V, 0, 0, 2), BIT_SET, NULL);
    _bitmap_op(vol, _calc_trajectory_lba(vol, G, V, 0, 0, 5), BIT_SET, NULL);
    
    /* Run 1 */
    hn4_addr_t out1;
    uint8_t k1;
    hn4_alloc_block(vol, &anchor, 0, &out1, &k1);
    
    /* Revert Alloc */
    uint64_t val1 = *(uint64_t*)&out1;
    _bitmap_op(vol, val1, BIT_CLEAR, NULL);
    
    /* Run 2 */
    hn4_addr_t out2;
    uint8_t k2;
    hn4_alloc_block(vol, &anchor, 0, &out2, &k2);
    
    ASSERT_EQ(k1, k2);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 11. MONOTONICITY PRESERVATION
 * ========================================================================= */
hn4_TEST(CollisionStats, MonotonicityPreservation) {
    hn4_volume_t* vol = create_collision_fixture();
    /* Use HDD Mode (V=1) to ensure strict order expectation */
    vol->sb.info.device_type_tag = HN4_DEV_HDD;
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1; /* Sequential */
    
    /* Alloc N=0, N=1, N=2 */
    hn4_addr_t lba0, lba1, lba2;
    uint8_t k;
    
    hn4_alloc_block(vol, &anchor, 0, &lba0, &k);
    hn4_alloc_block(vol, &anchor, 1, &lba1, &k);
    hn4_alloc_block(vol, &anchor, 2, &lba2, &k);
    
    uint64_t v0 = *(uint64_t*)&lba0;
    uint64_t v1 = *(uint64_t*)&lba1;
    uint64_t v2 = *(uint64_t*)&lba2;
    
    /* Must be monotonic increasing */
    ASSERT_TRUE(v1 > v0);
    ASSERT_TRUE(v2 > v1);
    
    cleanup_collision_fixture(vol);
}

/* =========================================================================
 * 5. RAPID K-OSCILLATION SUPPRESSION
 * ========================================================================= */
hn4_TEST(CollisionStats, OscillationSuppression) {
    hn4_volume_t* vol = create_collision_fixture();
    hn4_anchor_t anchor = {0};
    uint64_t G = 5000, V = 13;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = (uint8_t)V;
    
    /* Alloc / Free loop */
    uint8_t prev_k = 255;
    int chaotic_jumps = 0;
    
    for (int i = 0; i < 100; i++) {
        hn4_addr_t out;
        uint8_t k;
        hn4_alloc_block(vol, &anchor, 0, &out, &k);
        
        uint64_t val = *(uint64_t*)&out;
        hn4_free_block(vol, val);
        
        if (prev_k != 255) {
            /* If K jumps wildly (e.g. 0 -> 7 -> 0 -> 5), count it */
            if (abs((int)k - (int)prev_k) > 2) chaotic_jumps++;
        }
        prev_k = k;
    }
    
    /* Should be stable (always 0, or consistent K if 0 blocked externally) */
    ASSERT_EQ(0, chaotic_jumps);
    
    cleanup_collision_fixture(vol);
}