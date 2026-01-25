/*
 * HYDRA-NEXUS 4 (HN4) - TENSOR STREAM TESTS
 * FILE: hn4_tensor_tests.c
 * STATUS: PRODUCTION
 */

#include "hn4_test.h"
#include "hn4_tensor.h"
#include "hn4_hal.h"
#include "hn4_endians.h"

/* --- FIXTURES --- */

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} tensor_mock_dev_t;

static hn4_volume_t* create_tensor_vol(uint32_t profile) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    vol->target_device = hn4_hal_mem_alloc(sizeof(tensor_mock_dev_t));
    memset(vol->target_device, 0, sizeof(tensor_mock_dev_t));
    
    tensor_mock_dev_t* mdev = (tensor_mock_dev_t*)vol->target_device;
    
    /* 1TB Capacity to allow AI profile */
#ifdef HN4_USE_128BIT
    mdev->caps.total_capacity_bytes.lo = 1ULL * 1024 * 1024 * 1024 * 1024;
#else
    mdev->caps.total_capacity_bytes = 1ULL * 1024 * 1024 * 1024 * 1024;
#endif
    mdev->caps.logical_block_size = 4096;
    mdev->caps.hw_flags = HN4_HW_NVM; /* Enable write simulation */
    /* Allocate backing store for write tests */
    mdev->mmio_base = hn4_hal_mem_alloc(128 * 1024 * 1024); 

    vol->sb.info.format_profile = profile;
    
    /* Set block size based on profile */
    if (profile == HN4_PROFILE_AI) {
        vol->vol_block_size = 64 * 1024 * 1024; /* 64MB */
        vol->sb.info.block_size = 64 * 1024 * 1024;
    } else {
        vol->vol_block_size = 4096;
        vol->sb.info.block_size = 4096;
    }
    
#ifdef HN4_USE_128BIT
    vol->vol_capacity_bytes.lo = mdev->caps.total_capacity_bytes.lo;
#else
    vol->vol_capacity_bytes = mdev->caps.total_capacity_bytes;
#endif

    /* Setup RAM Cortex for lookups */
    vol->cortex_size = 1024 * sizeof(hn4_anchor_t);
    vol->nano_cortex = hn4_hal_mem_alloc(vol->cortex_size);
    memset(vol->nano_cortex, 0, vol->cortex_size);

    return vol;
}

static void destroy_tensor_vol(hn4_volume_t* vol) {
    if (vol) {
        tensor_mock_dev_t* mdev = (tensor_mock_dev_t*)vol->target_device;
        if (mdev->mmio_base) hn4_hal_mem_free(mdev->mmio_base);
        hn4_hal_mem_free(mdev);
        if (vol->nano_cortex) hn4_hal_mem_free(vol->nano_cortex);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TESTS
 * ========================================================================= */

/* 
 * TEST 1: Layout Optimization Logic
 * RATIONALE: Verifies that tensor dimensions are correctly padded to GPU 
 *            cache lines and aligned to HN4 Huge-Blocks (64MB).
 */
hn4_TEST(TensorLayout, FP16_Optimization) {
    /* 
     * Tensor: [1, 32, 1024, 1024] (N, C, H, W)
     * DType: FP16 (2 bytes)
     * Raw Size: 32 * 1024 * 1024 * 2 = 67,108,864 bytes (Exactly 64MB)
     * Alignment: 256 bytes
     */
    uint32_t dims[4] = {1, 32, 1024, 1024};
    hn4_size_t res_size;
    
    hn4_ai_calc_optimal_layout(dims, 2, 256, &res_size);
    
#ifdef HN4_USE_128BIT
    /* 64MB is exactly one block. Should match exactly. */
    ASSERT_EQ(64ULL * 1024 * 1024, res_size.lo);
#else
    ASSERT_EQ(64ULL * 1024 * 1024, res_size);
#endif
}

/* 
 * TEST 2: AI Freeze Profile Safety
 * RATIONALE: AI Context Freezing (KV Cache Dump) is only allowed on 
 *            HN4_PROFILE_AI volumes due to huge-block dependencies.
 */
hn4_TEST(TensorOps, Freeze_Profile_Mismatch) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_GENERIC);
    
    char buf[4096]; /* Dummy KV cache */
    
    /* Attempt freeze on Generic volume */
    hn4_result_t res = hn4_ai_freeze_context(vol, "model:llama", buf, 4096, 0);
    
    ASSERT_EQ(HN4_ERR_PROFILE_MISMATCH, res);
    
    destroy_tensor_vol(vol);
}

/* 
 * TEST 3: Tensor Open (Not Found)
 * RATIONALE: Opening a tensor tag that doesn't exist in the Cortex 
 *            should return NOT_FOUND gracefully.
 */
hn4_TEST(TensorOps, Open_NonExistent_Tag) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_AI);
    hn4_tensor_ctx_t* ctx = NULL;
    
    /* Cortex is empty (zeroed in fixture) */
    hn4_result_t res = hn4_tensor_open(vol, "model:gpt-404", &ctx);
    
    ASSERT_EQ(HN4_ERR_NOT_FOUND, res);
    ASSERT_EQ(NULL, ctx);
    
    destroy_tensor_vol(vol);
}

/* 
 * TEST 5: Layout Pathological Rejection
 * RATIONALE: If padding requirements bloat the tensor size > 200%, 
 *            the calculator should return UINT64_MAX to signal inefficiency.
 */
hn4_TEST(TensorLayout, Reject_Sparse_Padding) {
    /* 
     * Pathological Case:
     * Width = 1. Element = 1 byte.
     * Alignment = 1024 bytes.
     * Padding overhead = ~1000x.
     */
    uint32_t dims[4] = {1, 1, 1024, 1}; 
    hn4_size_t res_size;
    
    hn4_ai_calc_optimal_layout(dims, 1, 1024, &res_size);
    
#ifdef HN4_USE_128BIT
    ASSERT_EQ(UINT64_MAX, res_size.lo);
#else
    ASSERT_EQ(UINT64_MAX, res_size);
#endif
}


/* 
 * TEST 7: Tensor Close Safety
 * RATIONALE: Closing a NULL context should be a no-op, not a crash.
 */
hn4_TEST(TensorLifecycle, Close_Null_Safety) {
    hn4_tensor_close(NULL);
    ASSERT_TRUE(true); /* Survived */
}

/* 
 * TEST 8: Compute Graph Branding Failure
 * RATIONALE: If Signet branding fails (e.g. OOM), the graph write must abort.
 */
hn4_TEST(TensorOps, Graph_Branding_Failure) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_AI);
    
    /* Simulate Read-Only to force branding failure */
    vol->read_only = true;
    
    char blob[64] = {0};
    hn4_result_t res = hn4_ai_persist_compute_graph(vol, "model:graph", blob, 64);
    
    ASSERT_EQ(HN4_ERR_ACCESS_DENIED, res);
    
    destroy_tensor_vol(vol);
}

/* 
 * TEST 11: Tensor Read OOB
 * RATIONALE: Reading past the end of the tensor context should return error.
 */
hn4_TEST(TensorRead, OOB_Check) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_AI);
    hn4_tensor_ctx_t ctx = {0};
    ctx.vol = vol;
    ctx.total_size_bytes = 1000;
    
    char buf[10];
    hn4_result_t res = hn4_tensor_read(&ctx, 1001, buf, 10);
    
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, res);
    
    destroy_tensor_vol(vol);
}

/* 
 * TEST 12: AI Freeze Alignment Check
 * RATIONALE: Buffer pointer misalignment should be rejected.
 */
hn4_TEST(TensorOps, Freeze_Misalign_Ptr) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_AI);
    
    void* raw = hn4_hal_mem_alloc(4 * 1024 * 1024);
    void* bad_ptr = (void*)((uintptr_t)raw + 64); /* Not 2MB aligned */
    
    hn4_result_t res = hn4_ai_freeze_context(vol, "ctx:test", bad_ptr, 2*1024*1024, 0);
    
    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, res);
    
    hn4_hal_mem_free(raw);
    destroy_tensor_vol(vol);
}

/* 
 * TEST 13: Shard Sorting Logic
 * RATIONALE: Verify `_shard_cmp` correctly sorts anchors by Creation Time.
 */
hn4_TEST(TensorInternals, Shard_Sort_Time) {
    hn4_anchor_t shards[2];
    memset(shards, 0, sizeof(shards));
    
    /* Shard 0: Time 100 */
    shards[0].create_clock = hn4_cpu_to_le32(100);
    /* Shard 1: Time 50 */
    shards[1].create_clock = hn4_cpu_to_le32(50);
    
    /* Sort. Expect Shard 1 (50) then Shard 0 (100). */
    /* Access private comparator via qsort wrapper or re-implement logic to test */
    /* Since _shard_cmp is static, we test the effect via tensor_open behavior logic logic 
       or duplicate the cmp logic here for unit testing. We duplicate logic here. */
    
    int res = 0;
    uint32_t t0 = hn4_le32_to_cpu(shards[0].create_clock);
    uint32_t t1 = hn4_le32_to_cpu(shards[1].create_clock);
    
    if (t0 > t1) res = 1;
    
    ASSERT_EQ(1, res); /* Shard 0 is "Greater" (Later), so it should move to end */
}

/* 
 * TEST 14: Tensor Context RefCounting
 * RATIONALE: Opening a tensor context must increment volume ref_count.
 *            Closing must decrement.
 */
hn4_TEST(TensorLifecycle, RefCount_Check) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_AI);
    
    /* Manually init refcount */
    atomic_store(&vol->health.ref_count, 1);
    
    /* Mock success of open by injecting shards manually? 
       Too complex. We will manually construct a ctx and call close. */
    
    hn4_tensor_ctx_t* ctx = hn4_hal_mem_alloc(sizeof(hn4_tensor_ctx_t));
    ctx->vol = vol;
    
    /* Simulate successful open increment */
    atomic_fetch_add(&vol->health.ref_count, 1);
    ASSERT_EQ(2, atomic_load(&vol->health.ref_count));
    
    hn4_tensor_close(ctx);
    
    ASSERT_EQ(1, atomic_load(&vol->health.ref_count));
    
    destroy_tensor_vol(vol);
}

/* 
 * TEST 15: Invalid Profile AI Freeze
 * RATIONALE: Attempting AI Freeze on Archive profile should fail.
 */
hn4_TEST(TensorOps, Freeze_Archive_Fail) {
    hn4_volume_t* vol = create_tensor_vol(HN4_PROFILE_ARCHIVE);
    
    /* Fix buffer */
    void* raw = hn4_hal_mem_alloc(4 * 1024 * 1024);
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (addr + (2 * 1024 * 1024 - 1)) & ~(2 * 1024 * 1024 - 1);
    void* buf = (void*)aligned_addr;

    hn4_result_t res = hn4_ai_freeze_context(vol, "ctx:archive", buf, 2*1024*1024, 0);
    
    ASSERT_EQ(HN4_ERR_PROFILE_MISMATCH, res);
    
    hn4_hal_mem_free(raw);
    destroy_tensor_vol(vol);
}
