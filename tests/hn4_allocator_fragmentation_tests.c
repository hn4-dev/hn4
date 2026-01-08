/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR & FRAGMENTATION SUITE
 * FILE: hn4_allocator_fragmentation_tests.c
 * STATUS: PHYSICS VERIFICATION
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
#include "hn4.h"

/* --- FIXTURE INFRASTRUCTURE --- */
#define HN4_BLOCK_SIZE  4096
#define HN4_CAPACITY    (100ULL * 1024ULL * 1024ULL) /* 100 MB */
#define HN4_TOTAL_BLOCKS (HN4_CAPACITY / HN4_BLOCK_SIZE)
#define HN4_BITMAP_BYTES (((HN4_TOTAL_BLOCKS + 63) / 64) * sizeof(hn4_armored_word_t))

/* Duplicate internal enum for white-box testing */
typedef enum { 
    BIT_SET, 
    BIT_CLEAR, 
    BIT_TEST, 
    BIT_FORCE_CLEAR /* FIX 5: Non-Panic Rollback */ 
} hn4_bit_op_t;


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
    dev->caps.total_capacity_bytes = HN4_CAPACITY;
    dev->caps.hw_flags = 0;

    vol->target_device = dev;
    vol->vol_block_size = HN4_BLOCK_SIZE;
    vol->vol_capacity_bytes = HN4_CAPACITY;
    vol->read_only = false;

    vol->bitmap_size = HN4_BITMAP_BYTES;
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);

    vol->qmask_size = ((HN4_TOTAL_BLOCKS * 2 + 63) / 64) * 8; 
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    memset(vol->quality_mask, 0xAA, vol->qmask_size);

    /* Flux starts at Block 100 to leave room for metadata */
    vol->sb.info.lba_flux_start    = 100;
    vol->sb.info.lba_horizon_start = 20000;
    vol->sb.info.journal_start     = 21000;

    atomic_store(&vol->used_blocks, 0);
    atomic_store(&vol->horizon_write_head, 0);
    
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
 * 1. BALLISTIC PHYSICS & SCATTER
 * ========================================================================= */

/* Helper for test context */
static uint64_t _test_gcd(uint64_t a, uint64_t b) {
    while (b) { a %= b; uint64_t t = a; a = b; b = t; }
    return a;
}

/*
 * Test F1: Ballistic Scatter (The Anti-Defrag)
 * RATIONALE:
 * HN4 purposefully fragments data to use parallelism. 
 * If V=17, Block 1 should be at (Block 0 + 17), not (Block 0 + 1).
 * This validates the fundamental equation: LBA = G + (N * V).
 */
hn4_TEST(FragmentationMath, BallisticScatterVerify) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 1000;
    uint64_t V = 17; /* Prime Vector */
    uint16_t M = 0;  /* 4KB Blocks (S=1) */
    
    /* 
     * 1. REPLICATE GEOMETRY (Phi Calculation) 
     * We need to know the domain size to predict the stride behavior.
     */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t bs = vol->vol_block_size;
    uint32_t ss = caps->logical_block_size;
    uint32_t spb = bs / ss;

    /* Get Capacity in Blocks */
    uint64_t total_blocks = vol->vol_capacity_bytes / bs;
    
    /* Get Flux Start Block */
    uint64_t flux_start_sect = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    uint64_t flux_start_blk  = flux_start_sect / spb;
    
    /* Spec 18.10: Align Flux Start to Fractal Scale (S=1 here) */
    uint64_t S = 1ULL << M;
    uint64_t flux_aligned = (flux_start_blk + (S - 1)) & ~(S - 1);
    
    /* Calculate Phi (Available Slots) */
    uint64_t phi = (total_blocks - flux_aligned) / S;

    /* 
     * 2. DETERMINE EXPECTED STRIDE (UPDATED FOR RESONANCE DAMPENER)
     * Replicate the logic from _calc_trajectory_lba (Fix 2).
     * We must apply Anti-Even Degeneracy and the perturbation loop.
     */
    uint64_t effective_V = V | 1; /* Force Odd */
    uint64_t term_v = effective_V % phi;

    /* Dampener Loop Simulation */
    if (term_v == 0 || _test_gcd(term_v, phi) != 1) {
        uint64_t attempts = 0;
        do {
            term_v += 2;
            if (term_v >= phi) term_v = 3; /* Wrap logic */
            attempts++;
        } while (_test_gcd(term_v, phi) != 1 && attempts < 32);

        /* Ultimate fallback if dampener fails */
        if (_test_gcd(term_v, phi) != 1) {
            term_v = 1;
        }
    }
    
    uint64_t expected_stride = term_v;

    /* Calculate LBA for Block 0 and Block 1 */
    uint64_t lba_0 = _calc_trajectory_lba(vol, G, V, 0, M, 0);
    uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, M, 0);
    
    /* 
     * 3. VERIFY STRIDE
     * Handle modulo wrapping (if lba_1 < lba_0) just in case G+V > Phi.
     */
    uint64_t diff;
    if (lba_1 >= lba_0) {
        diff = lba_1 - lba_0;
    } else {
        diff = (lba_1 + phi) - lba_0;
    }
    
    ASSERT_EQ(expected_stride, diff);
    
    /* Verify Flux Offset logic (Spec 6.1) */
    /* LBA should land relative to the aligned flux start plus the randomized G offset */
    ASSERT_TRUE(lba_0 >= (flux_aligned + (G % phi)));

    cleanup_frag_fixture(vol);
}

/*
 * Test F2: Gravity Assist (Collision Teleportation)
 * RATIONALE:
 * When K >= 4, the vector V mutates to escape local gravity wells (clusters).
 * We expect the LBA difference between K=0 and K=4 to be non-linear.
 */
hn4_TEST(FragmentationMath, GravityAssistTeleport) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 5000;
    uint64_t V = 1; 
    uint16_t M = 0;

    /* K=0: Standard V=1. LBA ~ G + 0 */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, 1, M, 0);
    
    /* K=4: Gravity Assist triggers. V mutates. */
    uint64_t lba_k4 = _calc_trajectory_lba(vol, G, V, 1, M, 4);
    
    /* 
     * If V stayed 1, K=4 would just be G + Theta(4) = G + 10.
     * Since V mutates, the result should be massive (random-like jump).
     */
    uint64_t linear_diff = (lba_k4 > lba_k0) ? (lba_k4 - lba_k0) : (lba_k0 - lba_k4);
    
    /* Assert that we jumped far away (> 100 blocks), proving teleportation */
    ASSERT_TRUE(linear_diff > 100); 

    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * 2. FRAGMENTATION RESILIENCE
 * ========================================================================= */

/*
 * Test F3: Checkerboard Stress (Alloc/Free Cycle)
 * RATIONALE:
 * Simulate a worst-case fragmentation scenario: Allocating contiguously,
 * then freeing every other block (Swiss Cheese), then allocating again.
 * HN4 should handle this without performance cliff or accounting errors.
 */
hn4_TEST(FragmentationStress, CheckerboardPattern) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G, V;
    uint64_t allocated_lbas[100];

    /* 1. Allocate 100 Blocks */
    for (int i = 0; i < 100; i++) {
        /* Force V=1 to create a contiguous stripe for the test setup */
        /* Since alloc_genesis randomizes V, we mock the result LBA manually via loop */
        /* To properly test alloc, we just let it pick whatever, but we store the LBAs */
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, 0, &G, &V));
        
        /* Find what it picked (K=0 assumption for clean disk) */
        allocated_lbas[i] = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
        
        /* Actually Mark it USED (Genesis does this, but we reinforce logic check) */
        bool st; 
        _bitmap_op(vol, allocated_lbas[i], BIT_TEST, &st);
        ASSERT_TRUE(st);
    }

    /* 2. Free Every Even Block (Checkerboard) */
    for (int i = 0; i < 100; i += 2) {
        hn4_free_block(vol, allocated_lbas[i] * (4096/4096));
    }

    /* 3. Alloc 50 More (Should fill holes or find new space instantly) */
    for (int i = 0; i < 50; i++) {
        hn4_result_t res = hn4_alloc_genesis(vol, 0, 0, &G, &V);
        ASSERT_EQ(HN4_OK, res);
    }

    /* 4. Verify Usage Count */
    /* 100 Alloc - 50 Free + 50 Alloc = 100 Total Used */
    uint64_t used = atomic_load(&vol->used_blocks);
    ASSERT_EQ(100ULL, used);

    cleanup_frag_fixture(vol);
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
    atomic_store(&vol->used_blocks, threshold);
    
    /* 1. Attempt Standard Alloc -> Expect Redirection Signal */
    uint64_t G, V;
    hn4_result_t res = hn4_alloc_genesis(vol, 0, 0, &G, &V);
    
    /* 
     * CORRECTION: 
     * Expect HN4_INFO_HORIZON_FALLBACK (4), confirming the redirection logic.
     * Do NOT expect HN4_ERR_EVENT_HORIZON (-257), which implies failure.
     */
    ASSERT_EQ(HN4_INFO_HORIZON_FALLBACK, res);
    
    /* Verify Sticky Bit set (HN4_VOL_RUNTIME_SATURATED) */
    uint32_t flags = atomic_load(&vol->sb.info.state_flags);
    ASSERT_TRUE((flags & HN4_VOL_RUNTIME_SATURATED) != 0);

    /* 2. Attempt Horizon Alloc -> Expect Success */
    /* This confirms the Horizon subsystem is actually active and functional */
    uint64_t hlba;
    res = hn4_alloc_horizon(vol, &hlba);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify LBA is in Horizon Region (>= 20000) */
    ASSERT_TRUE(hlba >= 20000);
    ASSERT_TRUE(hlba < 21000); /* Before Journal */

    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * 4. ZNS COMPLIANCE
 * ========================================================================= */

/*
 * Test F5: ZNS Sequential Invariant
 * RATIONALE:
 * On ZNS drives, random writes are illegal. The allocator must FORCE V=1.
 */
hn4_TEST(ZnsLogic, VerifySequentialVector) {
    hn4_volume_t* vol = create_frag_fixture();
    mock_hal_device_t* mdev = (mock_hal_device_t*)vol->target_device;

    /* Set ZNS Flags */
    vol->sb.info.device_type_tag = HN4_DEV_ZNS;
    mdev->caps.hw_flags |= HN4_HW_ZNS_NATIVE;

    uint64_t G, V;
    hn4_alloc_genesis(vol, 0, 0, &G, &V);
    
    /* V MUST be 1 */
    ASSERT_EQ(1ULL, V);

    cleanup_frag_fixture(vol);
}

/*
 * Test F6: Entropy / Vector Weave (Spec 11.7)
 * RATIONALE:
 * With a high-entropy Vector (e.g. V > 1000), logical adjacency should
 * translate to physical distance. LBA_N and LBA_N+1 should be far apart.
 */
hn4_TEST(FragmentationMath, VectorWeaveDistribution) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 0;
    uint64_t V = 49157; /* Large Prime */
    uint16_t M = 0;
    
    uint64_t lba_0 = _calc_trajectory_lba(vol, G, V, 0, M, 0);
    uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, M, 0);
    uint64_t lba_2 = _calc_trajectory_lba(vol, G, V, 2, M, 0);
    
    /* Distances should be large, engaging multiple SSD channels */
    uint64_t d1 = (lba_1 > lba_0) ? (lba_1 - lba_0) : (lba_0 - lba_1);
    uint64_t d2 = (lba_2 > lba_1) ? (lba_2 - lba_1) : (lba_1 - lba_2);
    
    ASSERT_TRUE(d1 > 1000);
    ASSERT_TRUE(d2 > 1000);
    
    cleanup_frag_fixture(vol);
}

/*
 * Test F7: Coprimality Enforcement (Spec 18.2)
 * RATIONALE:
 * The allocator must enforce GCD(V, Phi) == 1 to guarantee the Ballistic 
 * Trajectory covers the full period of the drive (Full Cycle) before repeating.
 * If V shares a factor with Phi, we get orbital resonance (shadow zones).
 */

hn4_TEST(SafetyCheck, CoprimalityEnforcement) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* 
     * Calculate Phi for this fixture 
     * Total=25600, Start=100 -> Phi=25500.
     * 25500 factors: 2, 3, 5, 17...
     */
    uint64_t total_blocks = HN4_TOTAL_BLOCKS;
    uint64_t flux_start = vol->sb.info.lba_flux_start;
    uint64_t phi = total_blocks - flux_start; 
    
    uint64_t G, V;
    
    /* Run a statistical sample of genesis allocations */
    for (int i = 0; i < 50; i++) {
        /* Request allocations with Intent=DEFAULT (allows random V) */
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
        
        /* 
         * Verify the Law: GCD(V, Phi) must be 1.
         * The allocator should have mutated V until this was true.
         */
        uint64_t common = _test_gcd(V, phi);
        ASSERT_EQ(1ULL, common);
        
        /* Reset usage to allow loop to continue without saturation */
        atomic_store(&vol->used_blocks, 0);
    }
    
    cleanup_frag_fixture(vol);
}

/*
 * Test F8: Fractal Alignment (Spec 18.10)
 * RATIONALE:
 * When Fractal Scale M > 0, allocations must align to S = 2^M boundaries.
 * This ensures that large block allocations (e.g. 64KB) map to aligned
 * physical regions for performance (Flash Pages / RAID Stripes).
 */
hn4_TEST(GeometryLogic, FractalAlignmentVerification) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint16_t M = 2; /* 2^2 = 4 blocks (16KB stride) */
    uint64_t S = 1ULL << M;
    uint64_t flux_start = vol->sb.info.lba_flux_start;
    
    uint64_t G, V;
    
    for (int i = 0; i < 10; i++) {
        /* Request M=2 allocation */
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, M, HN4_ALLOC_DEFAULT, &G, &V));
        
        /* Calculate resulting LBA */
        uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, M, 0);
        
        /* 
         * Verify Alignment relative to Flux Start.
         * The allocator must pick G such that (G % Phi) lands on an S-boundary
         * in the physical domain.
         */
        uint64_t rel_offset = lba - flux_start;
        ASSERT_EQ(0ULL, rel_offset % S);
        
        /* G itself should be a multiple of S in the fractal domain calculation */
        ASSERT_EQ(0ULL, G % S);
        
        atomic_store(&vol->used_blocks, 0);
    }
    
    cleanup_frag_fixture(vol);
}

/*
 * Test F9: Trajectory Boundary Wrap (Math)
 * RATIONALE:
 * Verify that the ballistic equation correctly wraps around the capacity (Phi)
 * when the trajectory exceeds the end of the drive. Off-by-one errors here
 * cause corruption (writing to LBA > Capacity) or collisions.
 */
hn4_TEST(FragmentationMath, TrajectoryBoundaryWrap) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t flux_start = vol->sb.info.lba_flux_start; /* 100 */
    uint64_t total = HN4_TOTAL_BLOCKS; /* 25600 */
    uint64_t phi = total - flux_start; /* 25500 */
    
    /* 
     * Setup edge case:
     * G is at the very last index of the flux region.
     * Vector V=1. N=1.
     * Expectation: Wrap to Index 0 (Physical LBA 100).
     */
    uint64_t G = phi - 1; /* 25499 */
    uint64_t V = 1;
    uint16_t M = 0;
    
    /* Check N=0 (The Tail) */
    uint64_t lba_tail = _calc_trajectory_lba(vol, G, V, 0, M, 0);
    ASSERT_EQ(flux_start + 25499, lba_tail);
    
    /* Check N=1 (The Wrap) */
    uint64_t lba_wrap = _calc_trajectory_lba(vol, G, V, 1, M, 0);
    
    /* Should be exactly Flux Start */
    ASSERT_EQ(flux_start, lba_wrap);
    
    /* Check Overflow Logic with large N */
    /* N = Phi. Should wrap back to Start. */
    uint64_t lba_full_cycle = _calc_trajectory_lba(vol, G, V, phi, M, 0);
    ASSERT_EQ(lba_tail, lba_full_cycle); /* Should equal G */

    cleanup_frag_fixture(vol);
}

/*
 * Test F12: Toxic Evasion (Q-Mask Integration)
 * RATIONALE:
 * Verify that the ballistic engine automatically "orbits" around blocks
 * marked TOXIC (0x00) in the Quality Mask.
 * If K=0 lands on a Toxic Block, it must silently skip to K=1 (Gravity Assist)
 * without returning an error, even if the bitmap says the block is free.
 */
hn4_TEST(EdgeCases, ToxicBlockEvasion) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 500;
    uint64_t V = 1;
    uint64_t N = 0;
    
    /* 1. Calculate where K=0 lands */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    /* 2. Poison this exact block in the Q-Mask (Mark as 0x00 TOXIC) */
    /* Q-Mask is 2 bits per block. 0x00 is TOXIC. 0xAA (10) is Silver. */
    /* We need to clear the specific bits for lba_k0 */
    uint64_t word_idx = lba_k0 / 32;
    uint32_t shift = (lba_k0 % 32) * 2;
    
    /* Clear both bits at shift position to make it 00 (TOXIC) */
    vol->quality_mask[word_idx] &= ~(3ULL << shift);
    
    /* Ensure Bitmap is CLEAR (Free) at this location, so only Q-Mask stops it */
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
    
    /* 
     * Expectation: 
     * K=0 was Toxic. 
     * Allocator should have auto-incremented K.
     * Result should be K >= 1.
     */
    ASSERT_TRUE(out_k >= 1);
    
    /* LBA should NOT be the toxic one */
    uint64_t res_lba;
#ifdef HN4_USE_128BIT
    res_lba = out_lba.lo;
#else
    res_lba = out_lba;
#endif
    ASSERT_TRUE(res_lba != lba_k0);

    cleanup_frag_fixture(vol);
}


/* =========================================================================
 * 5. ADVERSARIAL & TEMPORAL STRESS
 * ========================================================================= */

/*
 * Test F13: Pathological Writes (The "Prime Clash")
 * RATIONALE:
 * An adversary (or worst-case workload) occupies every slot in the primary 
 * orbital shell (K=0..3) for a specific trajectory. 
 * This tests the "Gravity Assist" teleportation mechanism.
 * 
 * We calculate where V=1 wants to land for N=0..3, manually burn those slots,
 * then request that exact allocation. The engine MUST teleport (K=4).
 */
hn4_TEST(PathologicalWrites, PrimeClashAndTeleport) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 1000;
    uint64_t V = 1;
    uint16_t M = 0;
    
    /* FIX: Use N=1 to ensure Vector V affects the trajectory */
    uint64_t logical_idx = 1; 

    /* 
     * 1. Adversarial Setup:
     * Manually occupy the first 4 shells (K=0..3) for this specific trajectory.
     */
    for (uint8_t k = 0; k < 4; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, logical_idx, M, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }

    /* 2. Prepare Request */
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    anchor.fractal_scale = hn4_cpu_to_le16(M);

    /* 3. Execute Allocation */
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, logical_idx, &out_lba, &out_k);

    /* 4. Verify Teleportation */
    ASSERT_EQ(HN4_OK, res);
    
    /* It must have skipped K=0..3 */
    ASSERT_TRUE(out_k >= 4);

    uint64_t final_lba_val;
    #ifdef HN4_USE_128BIT
    final_lba_val = out_lba.lo;
    #else
    final_lba_val = out_lba;
    #endif

    /* Calculate where K=3 would have been (the last blocked slot) */
    uint64_t lba_k3 = _calc_trajectory_lba(vol, G, V, logical_idx, M, 3);
    
    /* 
     * Now that N=1, V' (Gravity Assist) is multiplied by 1.
     * V' is huge (rotated), so the distance should be massive.
     */
    uint64_t dist = (final_lba_val > lba_k3) ? (final_lba_val - lba_k3) : (lba_k3 - final_lba_val);
    ASSERT_TRUE(dist > 100); 

    cleanup_frag_fixture(vol);
}


/*
 * Test F14: Temporal Stress (Hot/Cold Churn)
 * RATIONALE:
 * Simulates a filesystem aging process.
 * 1. Fills 50% of disk with interleaved HOT (temp) and COLD (archive) data.
 * 2. Deletes all HOT data (creating Swiss Cheese fragmentation).
 * 3. Attempts to fill the holes with a new "New-Gen" allocation stream.
 * 
 * Verifies that the Ballistic Allocator finds the holes without hitting
 * saturation limits prematurely.
 */
hn4_TEST(TemporalStress, HotColdChurn) {
    hn4_volume_t* vol = create_frag_fixture();
    
    const int ITEM_COUNT = 2000;
    uint64_t cold_lbas[ITEM_COUNT];
    uint64_t hot_lbas[ITEM_COUNT];
    uint64_t G, V;

    /* 1. Allocation Phase: Interleave Cold/Hot */
    for (int i = 0; i < ITEM_COUNT; i++) {
        /* Cold */
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, 0, &G, &V));
        cold_lbas[i] = _calc_trajectory_lba(vol, G, V, 0, 0, 0); // Genesis claims N=0
        
        /* Hot */
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, 0, &G, &V));
        hot_lbas[i] = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    }

    /* Verify Usage: 4000 blocks */
    ASSERT_EQ(4000ULL, atomic_load(&vol->used_blocks));

    /* 2. Aging Phase: Delete all HOT data */
    for (int i = 0; i < ITEM_COUNT; i++) {
        hn4_free_block(vol, hot_lbas[i]);
    }

    /* Verify Usage: 2000 blocks */
    ASSERT_EQ(2000ULL, atomic_load(&vol->used_blocks));

    /* 3. Re-Fill Phase: Allocate 2000 NEW blocks */
    /* If fragmentation handling is poor, this loop will get slower or fail */
    for (int i = 0; i < ITEM_COUNT; i++) {
        hn4_result_t res = hn4_alloc_genesis(vol, 0, 0, &G, &V);
        if (res != HN4_OK) {
            /* Fail immediately if we can't reclaim space */
            ASSERT_EQ(HN4_OK, res);
        }
    }

    /* 4. Final Accounting */
    /* Should be back to 4000, not more (no leaks) */
    ASSERT_EQ(4000ULL, atomic_load(&vol->used_blocks));

    cleanup_frag_fixture(vol);
}

/*
 * Test F15: Q-Depth Saturation (K-Value Histogram)
 * RATIONALE:
 * As the volume fills to 90%, the average K (shell depth) required to find
 * a free block increases.
 * This test fills the volume and ensures that we utilize the higher K-shells
 * (Gravity Assist) before failing. If we fail with K=0..3 slots empty, logic is broken.
 * If we fail only after K hits 12, the logic is holding up.
 */
hn4_TEST(SaturationMetrics, KDepthHistogram) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t total = HN4_TOTAL_BLOCKS;
    uint64_t flux_cap = 5000;
    vol->sb.info.lba_flux_start = total - flux_cap;

    /* 1. SEED OBSTACLES (50% Density) */
    uint64_t G_seed, V_seed;
    for (int i = 0; i < 2500; i++) {
        hn4_alloc_genesis(vol, 0, 0, &G_seed, &V_seed);
    }

    /* 2. SEQUENTIAL WRITE ATTEMPT */
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    anchor.gravity_center = 0;
    uint64_t V = 1; 
    memcpy(anchor.orbit_vector, &V, 6);

    int k_stats[16] = {0}; 
    int successful_allocs = 0;

    /* Fill remainder */
    for (int i = 0; i < flux_cap * 2; i++) {
        hn4_addr_t out_lba;
        uint8_t out_k;
        
        hn4_result_t res = hn4_alloc_block(vol, &anchor, i, &out_lba, &out_k);
        
        if (res == HN4_OK) {
            if (out_k < 16) k_stats[out_k]++;
            successful_allocs++;
        }
        /* FIX: Do NOT break. Continue trying other N indices. */
    }

    /* 3. Validation */
    
    /* We expect to fill most of the remaining 2500 blocks */
    /* Allow some margin for hash collisions that were totally unresolvable */
    ASSERT_TRUE(successful_allocs > 2000);

    /* Verify Gravity Assist (K>=4) was utilized */
    int high_k = 0;
    for (int k = 4; k < 16; k++) high_k += k_stats[k];
    
    ASSERT_TRUE(high_k > (successful_allocs / 100));

    cleanup_frag_fixture(vol);
}


/* =========================================================================
 * 6. COMPARATIVE & PROFILE-SPECIFIC STRESS
 * ========================================================================= */

/*
 * Test F16: PICO Profile (Constrained Physics)
 * SCENARIO:
 * Enable HN4_PROFILE_PICO. This forces K_LIMIT = 0 (Single Shell).
 * We fragment the drive, then ensure allocations never use Gravity Assist
 * (K >= 4) but still succeed by varying G (Gravity Center) via Genesis.
 */
hn4_TEST(ProfileStress, PicoConstraintEnforcement) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* 1. Force PICO Profile */
    vol->sb.info.format_profile = HN4_PROFILE_PICO;

    /* 2. Create Fragmentation (Fill 50% via stride) */
    /* Occupy even blocks so Genesis is forced to pick Odd blocks */
    uint64_t G_fill = 0;
    uint64_t V_fill = 2; 
    uint16_t M = 0;
    
    for (int i = 0; i < 5000; i++) {
        uint64_t lba = _calc_trajectory_lba(vol, G_fill, V_fill, i, M, 0);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }

    uint64_t G_out, V_out;
    hn4_anchor_t anchor; 
    memset(&anchor, 0, sizeof(anchor));

    for (int i = 0; i < 50; i++) {
        /* A. Genesis: Finds a valid G/V where K=0 is free */
        hn4_result_t res = hn4_alloc_genesis(vol, 0, 0, &G_out, &V_out);
        ASSERT_EQ(HN4_OK, res);

        /* 
         * B. Calculate the LBA it claimed 
         */
        uint64_t lba_claimed = _calc_trajectory_lba(vol, G_out, V_out, 0, 0, 0);
        
        /* 
         * C. FREE IT.
         * We must free it because the next step (alloc_block) tries to claim it again.
         * In PICO, we can't jump to K=1, so a collision is fatal.
         */
        _bitmap_op(vol, lba_claimed, BIT_CLEAR, NULL);
        atomic_fetch_sub(&vol->used_blocks, 1);

        /* 
         * D. Verify PICO Constraint using explicit alloc_block.
         * It should succeed at K=0 (since we just freed it).
         */
        anchor.gravity_center = hn4_cpu_to_le64(G_out);
        memcpy(anchor.orbit_vector, &V_out, 6);
        
        hn4_addr_t check_lba;
        uint8_t check_k;
        
        res = hn4_alloc_block(vol, &anchor, 0, &check_lba, &check_k);
        
        ASSERT_EQ(HN4_OK, res);
        ASSERT_EQ(0, check_k); /* PICO MUST stay at K=0 */
        
        /* E. Re-claim it to maintain stress for next iteration */
        /* (Actually alloc_block claimed it, so we are good) */
    }

    cleanup_frag_fixture(vol);
}


/* =========================================================================
 * NEW TEST 1: MULTI-SCALE FRAGMENTATION (The Tetris Constraint)
 * ========================================================================= */
/*
 * RATIONALE:
 * HN4 supports fractal block sizes (M=0..16).
 * If we fill the disk with M=0 (4KB) blocks and free every other one, 
 * we have 50% free space, but NO contiguous slots for M=1 (8KB).
 * 
 * An M=1 allocation attempt MUST fail (or hit Horizon) despite 50% free capacity.
 * This proves strict alignment enforcement.
 */
hn4_TEST(FragmentationStress, Fractal_Tetris_Constraint) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* 1. Fill a region with M=0 (4KB) blocks */
    /* Force V=1 for contiguous packing */
    uint64_t G = 0;
    uint64_t V = 1;
    uint16_t M0 = 0;
    int count = 100;
    
    for(int i=0; i<count; i++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, i, M0, 0);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* 2. Punch holes (Free every ODD block) */
    /* Leaves: [USED] [FREE] [USED] [FREE] ... */
    for(int i=1; i<count; i+=2) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, i, M0, 0);
        _bitmap_op(vol, lba, BIT_CLEAR, NULL);
    }
    
    /* 3. Attempt M=1 (8KB) Allocation */
    /* M=1 requires 2 contiguous/aligned M=0 blocks. 
       Our pattern has no 2 contiguous blocks. */
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    anchor.orbit_vector[0] = 1;
    
    /* Request M=1 */
    /* We iterate through N to scan the holey region */
    bool found_slot = false;
    for(int n=0; n<50; n++) {
        /* alloc_block checks alignment and K-collisions */
        hn4_addr_t lba; uint8_t k;
        anchor.fractal_scale = hn4_cpu_to_le16(1); // M=1
        
        if (hn4_alloc_block(vol, &anchor, n, &lba, &k) == HN4_OK) {
            /* If it succeeds, it must have jumped OUT of the test region 
               (via K-teleport) or found a spot we didn't frag correctly. */
            
            /* Check if result is inside our 100-block test window */
            uint64_t phys = *(uint64_t*)&lba;
            uint64_t start_phys = _calc_trajectory_lba(vol, G, V, 0, M0, 0);
            
            /* If it's inside the window, we have a logic bug */
            if (phys >= start_phys && phys < (start_phys + 100)) {
                found_slot = true; 
                break;
            }
        }
    }
    
    /* It should NOT find a slot inside the Swiss Cheese region */
    ASSERT_FALSE(found_slot);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 2: K-LIMIT HARD STOP (Orbit Exhaustion)
 * ========================================================================= */
/*
 * RATIONALE:
 * The allocator tries K=0..12. If all 13 orbital shells are full, 
 * it MUST fail with HN4_ERR_GRAVITY_COLLAPSE (or fall to Horizon).
 * It must NOT loop infinitely or return a used block.
 */
hn4_TEST(SaturationLogic, K_Limit_Hard_Stop) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* Disable Horizon fallback for strict testing */
    /* (By filling Horizon or mocking the function, but simpler: check error code) */
    
    uint64_t G = 5000;
    uint64_t V = 1;
    uint64_t N = 0;
    uint16_t M = 0;
    
    /* 1. Manually fill K=0 to K=12 (HN4_MAX_TRAJECTORY_K is 12) */
    /* Note: loop <= 12 means 13 slots */
    for(int k=0; k<=12; k++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, N, M, k);
        _bitmap_op(vol, lba, BIT_SET, NULL);
    }
    
    /* 2. Attempt Alloc */
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(G);
    memcpy(anchor.orbit_vector, &V, 6);
    
    hn4_addr_t out_lba;
    uint8_t out_k;
    hn4_result_t res = hn4_alloc_block(vol, &anchor, N, &out_lba, &out_k);
    
    /* 
     * Expectation:
     * If Horizon is open, it returns OK + K=15 (Horizon Marker).
     * We accept either Gravity Collapse OR Horizon Fallback.
     * We specifically check it didn't return K <= 12.
     */
    if (res == HN4_OK) {
        ASSERT_EQ(15, out_k); /* Horizon Marker */
    } else {
        ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    }
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 3: LARGE STRIDE WRAP (V > Phi)
 * ========================================================================= */
/*
 * RATIONALE:
 * Verify modular arithmetic handles Vectors larger than the window size.
 * If V = Phi + 1, the trajectory should behave identically to V = 1.
 * This ensures no overflow or undefined behavior for massive Vectors.
 */
hn4_TEST(FragmentationMath, Large_Vector_Modulo) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t flux_start = vol->sb.info.lba_flux_start;
    uint64_t phi = (vol->vol_capacity_bytes / 4096) - flux_start;
    
    /* Case A: V = 1 */
    uint64_t lba_v1 = _calc_trajectory_lba(vol, 0, 1, 1, 0, 0);
    
    /* Case B: V = Phi + 1 */
    uint64_t V_huge = phi + 1;
    uint64_t lba_huge = _calc_trajectory_lba(vol, 0, V_huge, 1, 0, 0);
    
    /* They must land on the same spot */
    ASSERT_EQ(lba_v1, lba_huge);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 4: ORBITAL RESONANCE (Bad Vector Mitigation)
 * ========================================================================= */
/*
 * RATIONALE:
 * The Allocator forces V to be ODD (`V |= 1`).
 * If we provide an ODD number (5) that shares a factor with Phi (1000),
 * the old math would cycle early (at N=200).
 * 
 * NEW BEHAVIOR:
 * The allocator detects `gcd(5, 1000) != 1` and overrides V to 1.
 * This guarantees full disk coverage.
 */
hn4_TEST(PhysicsFailure, Orbital_Resonance_Mitigation) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* Mock Geometry: Total=1000, Start=0 -> Phi=1000 */
    /* Ensure Block Size matches capacity calc so logic holds */
    vol->vol_block_size = 4096;
    vol->vol_capacity_bytes = 1000 * 4096;
    vol->sb.info.lba_flux_start = 0;
    
    /* 
     * Use V=5. 
     * It is Odd, but divides 1000 (Phi).
     * GCD(5, 1000) = 5. This triggers the Resonance Dampener.
     */
    uint64_t G = 0;
    uint64_t V = 5;
    
    /* Calculate LBA at N=0 and N=200 (The Old Cycle Point: 200*5 = 1000 = 0 mod 1000) */
    uint64_t lba_0 = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    uint64_t lba_check = _calc_trajectory_lba(vol, G, V, 200, 0, 0);
    
    /* 
     * VERIFY RESONANCE MITIGATION
     * Old Bug: (0 + 200*5) % 1000 = 0 (Collision with lba_0).
     * Fixed: V becomes 7. (0 + 200*7) % 1000 = 400 (No collision).
     */
    ASSERT_NEQ(lba_0, lba_check);
    
    /* 
     * VERIFY DAMPENER LOGIC (Not Linear Fallback)
     * The allocator loop tries V+2, V+4... until coprime.
     * V=5 (Fail) -> V=7 (Coprime with 1000).
     * 
     * If V had fallen back to 1 (Linear), stride would be 1.
     * Since it damped to 7, stride is 7.
     */
    uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, 0, 0);
    
    /* 
     * We calculate the delta. Note: lba_1 could wrap if G was high, 
     * but here G=0, so lba_1 - lba_0 is the raw effective V.
     */
    uint64_t effective_stride = lba_1 - lba_0;

    /* Assert we preserved ballistic properties (7) rather than collapsing to linear (1) */
    ASSERT_EQ(7ULL, effective_stride); 
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 6: HORIZON RING EXHAUSTION
 * ========================================================================= */
/*
 * RATIONALE:
 * The Horizon is a linear buffer. If we fill it, it should return ENOSPC.
 * Unlike the Flux D1, it does not support "probing" - it's a simple ring.
 * However, since it's a RING, does it overwrite?
 * Spec 6.4: "Horizon acts as a linear overflow... circular buffer".
 * Safety: We cannot overwrite LIVE data. We check bitmap.
 * If Bitmap is full, alloc_horizon must fail.
 */
hn4_TEST(SaturationLogic, Horizon_Ring_Full) {
    hn4_volume_t* vol = create_frag_fixture();
    
    /* 
     * Setup Small Horizon: Start=20000, End=20010 (10 Blocks) 
     * (Journal starts at 20010 in this test)
     */
    vol->sb.info.lba_horizon_start = 20000;
    vol->sb.info.journal_start = 20010;
    
    /* Fill all 10 blocks */
    bool st;
    for(int i=0; i<10; i++) {
        _bitmap_op(vol, 20000 + i, BIT_SET, &st);
    }
    
    /* Attempt Alloc */
    uint64_t hlba;
    hn4_result_t res = hn4_alloc_horizon(vol, &hlba);
    
    /* Should fail because all bits are set */
    ASSERT_EQ(HN4_ERR_ENOSPC, res);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 7: ZERO-G SINGULARITY (G=0)
 * ========================================================================= */
/*
 * RATIONALE:
 * Ensure G=0 is treated as a valid offset (Start of Flux), 
 * not as a NULL pointer or error condition.
 */
hn4_TEST(EdgeCases, Zero_G_Validity) {
    hn4_volume_t* vol = create_frag_fixture();
    
    uint64_t G = 0;
    uint64_t V = 1;
    
    uint64_t lba = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    
    /* Should equal flux_start */
    uint64_t expected = vol->sb.info.lba_flux_start;
    ASSERT_EQ(expected, lba);
    
    cleanup_frag_fixture(vol);
}

/* =========================================================================
 * NEW TEST 8: MULTI-THREADED K-COLLISION (Race Condition)
 * ========================================================================= */
/*
 * RATIONALE:
 * Two threads request the SAME logical block (N=0) on the SAME anchor.
 * This simulates a race condition (e.g. COW fork).
 * One should succeed, the other should fail (or get a different block if intended).
 * Wait - allocating same block index on same anchor implies overwrite.
 * Allocator should allow it (Shadow Hop). 
 * They should get DIFFERENT LBAs (Shadows).
 */
hn4_TEST(Concurrency, Shadow_Hop_Race) {
    hn4_volume_t* vol = create_frag_fixture();
    
    hn4_anchor_t anchor = {0};
    uint64_t G = 1000;
    anchor.gravity_center = hn4_cpu_to_le64(G);
    
    /* Thread A Alloc */
    hn4_addr_t lba_a; uint8_t k_a;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba_a, &k_a));
    
    /* Thread B Alloc (Same Anchor, Same Index) */
    hn4_addr_t lba_b; uint8_t k_b;
    ASSERT_EQ(HN4_OK, hn4_alloc_block(vol, &anchor, 0, &lba_b, &k_b));
    
    /* 
     * Since Block A is now "Used" in the bitmap, 
     * Alloc B must find a *different* shadow (likely K+1 or via Gravity Assist).
     */
    uint64_t pa = *(uint64_t*)&lba_a;
    uint64_t pb = *(uint64_t*)&lba_b;
    
    ASSERT_NEQ(pa, pb);
    
    /* Verify K increased (or changed) */
    /* Note: If K_A was 0, K_B should be >= 1 */
    ASSERT_TRUE(k_b != k_a);
    
    cleanup_frag_fixture(vol);
}