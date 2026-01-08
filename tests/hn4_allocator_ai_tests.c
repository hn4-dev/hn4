/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR & AI TOPOLOGY SUITE
 * FILE: hn4_allocator_ai_tests.c
 * STATUS: TENSOR TUNNEL VERIFICATION
 *
 * SCOPE:
 *   1. AI Topology Discovery (GPU-ID to LBA mapping).
 *   2. Path-Aware Striping (Affinity Bias).
 *   3. Strict Locality Filtering (Trajectory containment).
 *   4. Context Switching & Isolation.
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

/* Internal Enum needed for mock setup */
typedef enum { 
    BIT_SET, 
    BIT_CLEAR, 
    BIT_TEST, 
    BIT_FORCE_CLEAR 
} hn4_bit_op_t;

/* HAL Simulation bindings (External references to HAL) */
extern void hn4_hal_sim_set_gpu_context(uint32_t gpu_id);
extern void hn4_hal_sim_clear_gpu_context(void);
extern hn4_result_t _bitmap_op(hn4_volume_t* vol, uint64_t block_idx, hn4_bit_op_t op, bool* out_state_changed);

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} mock_hal_device_t;

/*
 * HELPER: create_ai_fixture
 * Sets up a volume with HN4_PROFILE_AI and a predefined Topology Map.
 * 
 * MAP LAYOUT:
 * - Flux Start: Block 100
 * - GPU A (0x10DE): [2000, 10000)  (Size: 8000)
 * - GPU B (0x1002): [12000, 20000) (Size: 8000)
 * - Gaps exist to verify isolation.
 */
static hn4_volume_t* create_ai_fixture(void) {
    /* Reset HAL for RNG determinism */
    hn4_hal_init();

    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    memset(dev, 0, sizeof(mock_hal_device_t));
    
    dev->caps.logical_block_size = 4096;
    dev->caps.total_capacity_bytes = HN4_CAPACITY;
    dev->caps.hw_flags = 0; /* Standard SSD */

    vol->target_device = dev;
    vol->vol_block_size = HN4_BLOCK_SIZE;
    vol->vol_capacity_bytes = HN4_CAPACITY;
    vol->read_only = false;

    vol->bitmap_size = HN4_BITMAP_BYTES;
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);

    /* Allocate QMask but leave it all valid (silver) */
    vol->qmask_size = ((HN4_TOTAL_BLOCKS * 2 + 63) / 64) * 8; 
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    memset(vol->quality_mask, 0xAA, vol->qmask_size);

    /* --- AI SPECIFIC CONFIGURATION --- */
    vol->sb.info.format_profile = HN4_PROFILE_AI;
    
    /* Flux starts at Block 100 */
    vol->sb.info.lba_flux_start = 100; 
    vol->sb.info.lba_horizon_start = HN4_TOTAL_BLOCKS - 1000;

    /* Topology Map Setup */
    vol->topo_count = 2;
    vol->topo_map = hn4_hal_mem_alloc(sizeof(vol->topo_map[0]) * 2);
    
    /* Entry 0: GPU 0x10DE (NVIDIA) - Range 2000 to 10000 */
    vol->topo_map[0].gpu_id = 0x10DE;
    vol->topo_map[0].affinity_weight = 0; 
    vol->topo_map[0].lba_start = 2000 * (HN4_BLOCK_SIZE / 4096); 
    vol->topo_map[0].lba_len   = 8000 * (HN4_BLOCK_SIZE / 4096); 

    /* Entry 1: GPU 0x1002 (AMD) - Range 12000 to 20000 */
    vol->topo_map[1].gpu_id = 0x1002;
    vol->topo_map[1].affinity_weight = 0;
    vol->topo_map[1].lba_start = 12000 * (HN4_BLOCK_SIZE / 4096);
    vol->topo_map[1].lba_len   = 8000 * (HN4_BLOCK_SIZE / 4096);

    atomic_store(&vol->used_blocks, 0);
    
    return vol;
}

static void cleanup_ai_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
        if (vol->topo_map) hn4_hal_mem_free(vol->topo_map);
        hn4_hal_mem_free(vol);
    }
    hn4_hal_sim_clear_gpu_context(); /* Ensure thread clean */
}

/* =========================================================================
 * TEST 1 (FIXED): SPATIAL AFFINITY LOCK
 * ========================================================================= */

/*
 * Test AI-1: Spatial Affinity Lock (Fixed)
 * RATIONALE:
 * When a specific GPU requests allocation, the Void Engine must constrain
 * the search to the topology window defined in the map.
 * 
 * FIX: Increased window size (8000 blocks) to ensure the 8-hop strict locality
 * filter does not reject all 20 random probes due to edge collisions.
 */
hn4_TEST(AiTopology, SpatialAffinityLock) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    uint64_t lba_res;
    
    /* 
     * Scenario A: GPU 0x10DE (NVIDIA)
     * Window: [2000, 10000)
     */
    hn4_hal_sim_set_gpu_context(0x10DE);
    
    for (int i = 0; i < 50; i++) {
        hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
        ASSERT_EQ(HN4_OK, res);
        
        /* Resolve to Absolute LBA */
        lba_res = vol->sb.info.lba_flux_start + G;
        
        /* Verify constraints */
        ASSERT_TRUE(lba_res >= 2000);
        ASSERT_TRUE(lba_res < 10000);
        
        /* Prevent saturation affecting subsequent loops */
        atomic_store(&vol->used_blocks, 0);
        
        /* Clear the bit so we don't accidentally fill the window in this loop */
        _bitmap_op(vol, lba_res, BIT_CLEAR, NULL);
    }

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 2: ISOLATION CONSTRAINTS (CROSS-POLLUTION)
 * ========================================================================= */

/*
 * Test AI-2: Isolation Constraints
 * RATIONALE:
 * Verify that allocations for GPU A *never* land in GPU B's window, and vice versa.
 * This ensures that multi-tenant AI workloads do not suffer from noisy neighbor
 * interference at the physical NAND level.
 */
hn4_TEST(AiTopology, IsolationConstraints) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    uint64_t lba_res;

    /* GPU A: [2000, 10000) */
    hn4_hal_sim_set_gpu_context(0x10DE);
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
        lba_res = vol->sb.info.lba_flux_start + G;
        
        /* Must NOT be in GPU B's range [12000, 20000) */
        ASSERT_FALSE(lba_res >= 12000 && lba_res < 20000);
    }

    /* GPU B: [12000, 20000) */
    hn4_hal_sim_set_gpu_context(0x1002);
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
        lba_res = vol->sb.info.lba_flux_start + G;
        
        /* Must NOT be in GPU A's range */
        ASSERT_FALSE(lba_res >= 2000 && lba_res < 10000);
    }

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 3: DYNAMIC CONTEXT SWITCHING
 * ========================================================================= */

/*
 * Test AI-3: Dynamic Context Switching
 * RATIONALE:
 * A single thread pool might service requests for different accelerators sequentially.
 * We simulate a thread switching contexts (A -> B -> CPU) and verify the allocator
 * adapts immediately to the new thread-local context.
 */
hn4_TEST(AiTopology, ContextSwitching) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V, lba_res;

    /* 1. Context A */
    hn4_hal_sim_set_gpu_context(0x10DE);
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
    lba_res = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba_res >= 2000 && lba_res < 10000);

    /* 2. Context Switch -> B */
    hn4_hal_sim_set_gpu_context(0x1002);
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
    lba_res = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba_res >= 12000 && lba_res < 20000);

    /* 3. Context Clear -> CPU (Global) */
    hn4_hal_sim_clear_gpu_context();
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
    lba_res = vol->sb.info.lba_flux_start + G;
    /* Global alloc could land anywhere valid, just verify it's valid */
    ASSERT_TRUE(lba_res >= 100);

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 4: WINDOW SATURATION & LEAK CONTAINMENT
 * ========================================================================= */

/*
 * Test AI-4: Window Saturation
 * RATIONALE:
 * If an affinity window is full (or highly fragmented), the allocator should fail 
 * rather than silently leaking into the global pool (Strict Locality).
 * We manually fill Window A and verify allocation fails for GPU A.
 */
hn4_TEST(AiTopology, WindowSaturation) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;

    /* 1. Manually Saturate Window A [2000, 10000) */
    /* Note: We don't need to fill every bit, just enough to kill 20 random probes */
    /* A dense fill is safer for deterministic failure */
    for (uint64_t i = 2000; i < 10000; i++) {
        _bitmap_op(vol, i, BIT_SET, NULL);
    }

    /* 2. Request Alloc for GPU A */
    hn4_hal_sim_set_gpu_context(0x10DE);
    
    /* Expect failure (Event Horizon) because strict locality rejects leaks */
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    
    ASSERT_EQ(HN4_ERR_EVENT_HORIZON, res);

    /* 3. Verify GPU B still works (Window B is empty) */
    hn4_hal_sim_set_gpu_context(0x1002);
    res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    ASSERT_EQ(HN4_OK, res);

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 5: TOPOLOGY FALLBACK (UNKNOWN ID)
 * ========================================================================= */

/*
 * Test AI-5: Topology Fallback
 * RATIONALE:
 * If a thread identifies as an accelerator (e.g. GPU 0xCAFE) that is NOT 
 * present in the Volume's Topology Map (e.g. hot-plugged device, or map outdated),
 * the allocator must not fail. It should gracefully fall back to the Global Pool.
 */
hn4_TEST(AiTopology, FallbackOnUnknownID) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    uint64_t lba_res;

    /* Set a GPU ID that does not exist in the map */
    hn4_hal_sim_set_gpu_context(0xCAFE);
    
    /* Allocation should succeed (Global Fallback) */
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    ASSERT_EQ(HN4_OK, res);
    
    lba_res = vol->sb.info.lba_flux_start + G;
    
    /* 
     * Verify it lands somewhere valid in the Flux Manifold.
     * Since it falls back to global, it ignores the specific windows.
     */
    ASSERT_TRUE(lba_res >= 100);
    ASSERT_TRUE(lba_res < HN4_TOTAL_BLOCKS);

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 6: EMPTY MAP RESILIENCE
 * ========================================================================= */

/*
 * Test AI-6: Empty Map Resilience
 * RATIONALE:
 * If the profile is HN4_PROFILE_AI but the Topology Map failed to load 
 * (count=0) or is corrupted, the system must degrade gracefully to 
 * standard allocator behavior rather than crashing or rejecting writes.
 */
hn4_TEST(AiTopology, EmptyMapResilience) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;

    /* Simulate Map Corruption / Load Failure */
    vol->topo_count = 0;
    
    /* Even with a valid GPU context, it has no map to look up */
    hn4_hal_sim_set_gpu_context(0x10DE);
    
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify result is valid */
    uint64_t lba_res = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba_res >= 100);

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 7: VECTOR COPRIMALITY (MATH INVARIANT)
 * ========================================================================= */

static uint64_t _test_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = b; b = a % b; a = t; }
    return a;
}

/*
 * Test AI-7: Vector Coprimality within Window
 * RATIONALE:
 * The Ballistic Allocator guarantees full coverage of a region by ensuring
 * GCD(V, Capacity) == 1.
 * When an Affinity Window is active, the "Capacity" becomes the Window Size (Win_Phi).
 * We must verify that the allocator adjusts V to be coprime to the *Window Size*,
 * not the Global Capacity, otherwise we risk orbital resonance (unreachable blocks)
 * inside the GPU's dedicated region.
 */
hn4_TEST(AiTopology, VectorCoprimality) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    
    /* 
     * Target GPU 0x10DE. 
     * From Fixture: Range [2000, 10000). Window Size (Phi) = 8000.
     * 8000 is divisible by 2, 5. V must NOT be divisible by 2 or 5.
     */
    hn4_hal_sim_set_gpu_context(0x10DE);
    uint64_t window_phi = 8000;

    for (int i = 0; i < 50; i++) {
        ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
        
        /* Check 1: V must be coprime to Window Size */
        uint64_t common = _test_gcd(V, window_phi);
        if (common != 1) {
            printf("FAILURE: V=%llu shares factor %llu with Window=%llu\n", 
                   (unsigned long long)V, (unsigned long long)common, (unsigned long long)window_phi);
        }
        ASSERT_EQ(1ULL, common);

        /* 
         * Check 2: V must be Odd (Anti-Even Degeneracy)
         * We removed the Size check (V <= Phi) because the Anti-Hang fix 
         * (Random Rejection) may return V > Phi. This is safe because 
         * the Physics Engine calculates (V % Phi).
         */
        ASSERT_TRUE((V & 1) == 1);
        
        /* Reset usage */
        atomic_store(&vol->used_blocks, 0);
    }

    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 9: CONTIGUOUS TENSOR ALLOCATION
 * ========================================================================= */

/*
 * Test AI-9: Contiguous Tensor Mode
 * RATIONALE:
 * When streaming weights to a GPU, we want minimal seeking.
 * Requesting HN4_ALLOC_CONTIGUOUS inside a GPU context must force V=1 
 * (Linear Rail) while still respecting the spatial affinity window.
 */
hn4_TEST(AiTopology, ContiguousTensor) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    
    hn4_hal_sim_set_gpu_context(0x10DE); /* Window [2000, 10000) */
    
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_CONTIGUOUS, &G, &V));
    
    /* 1. Verify Vector is Sequential */
    ASSERT_EQ(1ULL, V);
    
    /* 2. Verify Window Containment */
    uint64_t lba = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba >= 2000 && lba < 10000);
    
    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 10: SHARED NAMESPACE (OVERLAPPING TOPOLOGY)
 * ========================================================================= */

/*
 * Test AI-10: Overlapping Topology (NVLink/Shared Memory)
 * RATIONALE:
 * Some architectures share storage pools between GPUs (e.g. DGX).
 * Verify that if two GPUs map to overlapping regions, they can both allocate
 * successfully in the overlap zone (probability permitting).
 */
hn4_TEST(AiTopology, SharedNamespace) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    
    /* Modify Topology to overlap */
    /* GPU A: [2000, 6000) */
    vol->topo_map[0].lba_start = 2000;
    vol->topo_map[0].lba_len   = 4000;
    
    /* GPU B: [4000, 8000) */
    vol->topo_map[1].lba_start = 4000;
    vol->topo_map[1].lba_len   = 4000;
    
    /* Overlap is [4000, 6000) */
    
    /* Alloc for A */
    hn4_hal_sim_set_gpu_context(0x10DE);
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
    uint64_t lba_a = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba_a >= 2000 && lba_a < 6000);
    
    /* Alloc for B */
    hn4_hal_sim_set_gpu_context(0x1002);
    ASSERT_EQ(HN4_OK, hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V));
    uint64_t lba_b = vol->sb.info.lba_flux_start + G;
    ASSERT_TRUE(lba_b >= 4000 && lba_b < 8000);
    
    cleanup_ai_fixture(vol);
}

/* =========================================================================
 * TEST 11: TINY WINDOW CONSTRAINT
 * ========================================================================= */

/*
 * Test AI-11: Tiny Window Survival
 * RATIONALE:
 * The "Strict Locality Filter" requires containment of 8 hops (N=0..7).
 * If a window is tiny (e.g., 16 blocks), the allocator must shrink V to 1
 * and carefully pick G such that G+7 is within bounds.
 * If logic is sloppy, this will fail or hang.
 */
hn4_TEST(AiTopology, TinyWindowSurvival) {
    hn4_volume_t* vol = create_ai_fixture();
    uint64_t G, V;
    
    /* Shrink GPU A window to just 16 blocks: [2000, 2016) */
    vol->topo_map[0].lba_start = 2000;
    vol->topo_map[0].lba_len   = 16;
    
    hn4_hal_sim_set_gpu_context(0x10DE);
    
    /* Should succeed by forcing V=1 and G in [0..8] relative */
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* V must be 1 to fit 8 hops in 16 blocks */
    ASSERT_EQ(1ULL, V);
    
    /* Check Trajectory N=7 is contained */
    /* Absolute LBA = Flux(100) + G + 7 */
    /* Window End = 2016. */
    /* G is relative to 1900 (2000-100) */
    
    uint64_t lba_head = vol->sb.info.lba_flux_start + G;
    uint64_t lba_tail = lba_head + 7;
    
    ASSERT_TRUE(lba_tail < 2016);

    cleanup_ai_fixture(vol);
}

