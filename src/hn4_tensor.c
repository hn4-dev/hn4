/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Tensor Stream Layer & AI Acceleration
 * SOURCE:      hn4_tensor.c
 * STATUS:      HARDENED / PRODUCTION (v20.5)
 * ARCHITECT:   Core Systems Engineering
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * =============================================================================
 *  THEORETICAL FOUNDATION: ELIMINATING THE "COMPUTE GAP"
 * =============================================================================
 *
 * PROBLEM 1: COHERENCE DRIFT (The Memory Wall)
 *   In standard systems, the "State" of an LLM (the KV Cache) exists only in volatile
 *   VRAM. Saving it requires serialization, creating drift.
 *
 *   HN4 SOLUTION: "SYNAPTIC FREEZING" via BALLISTIC MAPPING
 *   1. We use 64MB Huge-Blocks (D1 Flux).
 *   2. We write the KV cache *raw*. No serialization.
 *   3. We use Ballistic Addressing for O(1) streaming.
 *
 * PROBLEM 2: LLVM / JIT LATENCY (The Compilation Wall)
 *   Loading a model involves runtime compilation.
 *
 *   HN4 SOLUTION: "PRE-BAKED TENSOR MANIFOLDS"
 *   1. Tensors are stored "Swizzled" (Tiled) via `hn4_ai_calc_optimal_layout`.
 *   2. The Compute Graph is stored as a binary command list.
 *   3. Loading becomes a single `mmap` / P2P DMA operation.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_tensor.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_constants.h"
#include "hn4_annotations.h"
#include "hn4_addr.h"
#include "hn4_swizzle.h"
#include "hn4_anchor.h" 
#include "hn4_signet.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* =========================================================================
 * 0. CONSTANTS & CONFIGURATION
 * ========================================================================= */

#define HN4_MAX_TENSOR_SHARDS   4096
#define HN4_SHARD_INVALID       UINT32_MAX

#ifndef HN4_FLAG_SIGNED
#define HN4_FLAG_SIGNED (1ULL << 28)
#endif

#define MAX_ROLLBACK_TRACK 1024

/* 
 * 64MB BLOCK ALIGNMENT
 * Matches standard Huge Page sizes (2MB x 32) to minimize TLB misses.
 */
#define HN4_AI_BLOCK_SIZE       (64ULL * 1024 * 1024)
#define HN4_AI_DMA_ALIGNMENT    (2ULL * 1024 * 1024)  /* 2MB Alignment for RDMA/P2P */

/*
 * Semantic Type Flags for Anchors
 * Repurposed bits 24-27 of data_class to avoid collision with system flags.
 */
#define HN4_AI_TYPE_MASK        (0xF000000ULL)
#define HN4_AI_TYPE_WEIGHTS     (0x1000000ULL)
#define HN4_AI_TYPE_KV_CACHE    (0x2000000ULL)
#define HN4_AI_TYPE_GRAPH       (0x3000000ULL)

/* =========================================================================
 * 1. INTERNAL HELPERS: TOPOLOGY & LOOKUP
 * ========================================================================= */

/**
 * _shard_cmp
 * Sorts shards by 128-bit Seed ID to establish logical monotonicity.
 */
static int _shard_cmp(const void* a, const void* b) 
{
    const hn4_anchor_t* sa = (const hn4_anchor_t*)a;
    const hn4_anchor_t* sb = (const hn4_anchor_t*)b;
    
    /* Prioritize explicit Creation Clock for logical ordering */
    uint32_t t_a = hn4_le32_to_cpu(sa->create_clock);
    uint32_t t_b = hn4_le32_to_cpu(sb->create_clock);
    
    if (t_a < t_b) return -1;
    if (t_a > t_b) return 1;

    /* Fallback to Seed ID (High part is nanosecond/time usually) */
    hn4_u128_t id_a = hn4_le128_to_cpu(sa->seed_id);
    hn4_u128_t id_b = hn4_le128_to_cpu(sb->seed_id);

    if (id_a.hi < id_b.hi) return -1;
    if (id_a.hi > id_b.hi) return 1;
    if (id_a.lo < id_b.lo) return -1;
    if (id_a.lo > id_b.lo) return 1;
    return 0;
}

/**
 * _find_shard_idx
 * Resolves global logical offset to shard index using Binary Search.
 *
 * SAFETY: Explicit bounds checking against shard_count.
 */
static uint32_t _find_shard_idx(const hn4_tensor_ctx_t* ctx, uint64_t pos) 
{
    if (pos >= ctx->total_size_bytes || ctx->shard_count == 0) 
        return HN4_SHARD_INVALID;

    int l = 0, r = ctx->shard_count - 1;
    
    while (l <= r) {
        int mid = l + (r - l) / 2;
        uint64_t start = ctx->shard_offsets[mid];
        uint64_t end   = ctx->shard_offsets[mid + 1];

        if (pos >= start && pos < end) return mid;
        if (pos < start) r = mid - 1;
        else l = mid + 1;
    }
    return HN4_SHARD_INVALID;
}


/**
 * _ai_map_p2p_bar
 * 
 * Prepares the PCIe BAR for Peer-to-Peer DMA.
 * SAFETY: Validates both address AND length alignment.
 * HAL CONTRACT: Must return opaque handle or NULL.
 */
static hn4_result_t _ai_map_p2p_bar(
    hn4_hal_device_t* dev, 
    uint32_t gpu_id, 
    void* vram_ptr, 
    uint64_t len,
    void** out_dma_handle
) {
    /* Check HAL capabilities */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    
    if (!(caps->hw_flags & HN4_HW_GPU_DIRECT)) {
        return HN4_ERR_DMA_MAPPING;
    }

    /* 
     * STIFF ALIGNMENT CHECK:
     * DMA engines require both Start Address AND Length to be aligned.
     */
    if ((((uintptr_t)vram_ptr % HN4_AI_DMA_ALIGNMENT) != 0) || 
        ((len % HN4_AI_DMA_ALIGNMENT) != 0)) 
    {
        HN4_LOG_WARN("AI: P2P DMA rejected. Addr/Len must be 2MB aligned.");
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    /* In simulation/bare metal, we use the GPU ID as the handle context */
    *out_dma_handle = (void*)((uintptr_t)gpu_id | 0x8000000000000000ULL); 
    return HN4_OK;
}

/* =========================================================================
 * 2. PUBLIC API: TENSOR VIRTUALIZATION
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_tensor_open(
    HN4_IN  hn4_volume_t* vol, 
    HN4_IN  const char*   model_tag, 
    HN4_OUT hn4_tensor_ctx_t** out_ctx
)
{
    hn4_result_t result = HN4_OK;
    hn4_tensor_ctx_t* ctx = NULL;
    uint32_t found_count = 0;
    uint64_t accumulator = 0;

    if (HN4_UNLIKELY(!vol || !model_tag || !out_ctx)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    ctx = hn4_hal_mem_alloc(sizeof(hn4_tensor_ctx_t));
    if (!ctx) return HN4_ERR_NOMEM;
    memset(ctx, 0, sizeof(hn4_tensor_ctx_t));

    /* --- PHASE 1: GATHER --- */
    ctx->shards = hn4_hal_mem_alloc(sizeof(hn4_anchor_t) * HN4_MAX_TENSOR_SHARDS);
    if (!ctx->shards) {
        result = HN4_ERR_NOMEM;
        goto failure;
    }

    /* Scan the Cortex for anchors matching the tag */
    result = hn4_ns_gather_tensor_shards(
        vol, model_tag, ctx->shards, HN4_MAX_TENSOR_SHARDS, &found_count);

    if (result != HN4_OK) goto failure;

    uint32_t verified_count = 0;
    for (uint32_t i = 0; i < found_count; i++) {
        char name_buf[256];
        /* Resolve name from inline buffer or extension */
        if (hn4_ns_get_name(vol, &ctx->shards[i], name_buf, sizeof(name_buf)) == HN4_OK) {
            if (strcmp(name_buf, model_tag) == 0) {
                /* Valid match: Pack array */
                if (i != verified_count) {
                    ctx->shards[verified_count] = ctx->shards[i];
                }
                verified_count++;
            }
        }
    }
    found_count = verified_count; /* Update count to verified subset */
    
    if (found_count == 0) {
        result = HN4_ERR_NOT_FOUND;
        goto failure;
    }

    if (HN4_UNLIKELY(found_count == HN4_MAX_TENSOR_SHARDS)) {
        HN4_LOG_CRIT("Tensor Open: Shard count hit limit (%u). Ambiguous completeness.", 
                     HN4_MAX_TENSOR_SHARDS);
        result = HN4_ERR_TAG_OVERFLOW;
        goto failure;
    }

    /* --- PHASE 2: GEOMETRY MAP --- */
    qsort(ctx->shards, found_count, sizeof(hn4_anchor_t), _shard_cmp);

    /* Allocate N+1 to hold EOF sentinel */
    ctx->shard_offsets = hn4_hal_mem_alloc(sizeof(uint64_t) * (found_count + 1));
    if (!ctx->shard_offsets) {
        result = HN4_ERR_NOMEM;
        goto failure;
    }

    for (uint32_t i = 0; i < found_count; i++) {
        ctx->shard_offsets[i] = accumulator;
        
        uint64_t mass = hn4_le64_to_cpu(ctx->shards[i].mass);
        
        if (HN4_UNLIKELY(mass == 0)) {
            HN4_LOG_CRIT("Tensor Open: Shard %u has zero mass.", i);
            result = HN4_ERR_DATA_ROT;
            goto failure;
        }

        if (HN4_UNLIKELY((UINT64_MAX - accumulator) < mass)) {
            HN4_LOG_CRIT("Tensor Open: Mass overflow (Exceeds 18 EB).");
            result = HN4_ERR_GEOMETRY;
            goto failure;
        }
        
        accumulator += mass;
    }
    
    /* Sentinel at end */
    ctx->shard_offsets[found_count] = accumulator;

    /* --- PHASE 3: CONTEXT FINALIZATION --- */
    ctx->vol              = vol;
    ctx->shard_count      = found_count;
    ctx->total_size_bytes = accumulator;
    ctx->block_size       = vol->vol_block_size;
    
    uint32_t p_cap = HN4_BLOCK_PayloadSize(ctx->block_size);
    if (HN4_UNLIKELY(p_cap == 0 || p_cap >= ctx->block_size)) {
        HN4_LOG_CRIT("Tensor Open: Invalid block geometry. BS=%u P=%u", 
                     ctx->block_size, p_cap);
        result = HN4_ERR_GEOMETRY;
        goto failure;
    }
    ctx->payload_cap = p_cap;

    atomic_fetch_add(&vol->health.ref_count, 1);

    *out_ctx = ctx;
    return HN4_OK;

failure:
    hn4_tensor_close(ctx);
    return result;
}

_Check_return_
hn4_result_t hn4_tensor_read(
    HN4_IN  hn4_tensor_ctx_t* ctx, 
    HN4_IN  uint64_t global_offset, 
    HN4_OUT void*    buf, 
    HN4_IN  uint64_t len
)
{
    /* 1. Basic Pointer Validation */
    if (HN4_UNLIKELY(!ctx || !buf)) return HN4_ERR_INVALID_ARGUMENT;
    
    /* 2. Context Integrity Validation (The Missing Check) */
    if (HN4_UNLIKELY(!ctx->vol || !ctx->shards || !ctx->shard_offsets)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }
    /* Prevent Div-by-Zero in block_idx calc */
    if (HN4_UNLIKELY(ctx->block_size == 0 || ctx->payload_cap == 0)) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    if (len == 0) return HN4_OK;

    if (global_offset >= ctx->total_size_bytes) {
        return HN4_ERR_INVALID_ARGUMENT;
    }
    
    /* Clamp Read Length to EOF */
    uint64_t read_len = len;
    if ((UINT64_MAX - global_offset) < len || (global_offset + len) > ctx->total_size_bytes) {
        read_len = ctx->total_size_bytes - global_offset;
    }

    void* bounce_buf = hn4_hal_mem_alloc(ctx->block_size);
    if (HN4_UNLIKELY(!bounce_buf)) return HN4_ERR_NOMEM;

    uint8_t* cursor      = (uint8_t*)buf;
    uint64_t remaining   = read_len;
    uint64_t current_pos = global_offset;
    hn4_result_t res     = HN4_OK;

    uint32_t shard_idx = _find_shard_idx(ctx, current_pos);
    
    if (HN4_UNLIKELY(shard_idx == HN4_SHARD_INVALID)) {
        res = HN4_ERR_GEOMETRY;
        goto cleanup;
    }

    while (remaining > 0 && shard_idx < ctx->shard_count) {
        
        hn4_anchor_t* anchor = &ctx->shards[shard_idx];
        uint64_t shard_start = ctx->shard_offsets[shard_idx];
        uint64_t shard_end   = ctx->shard_offsets[shard_idx + 1];
        uint64_t shard_mass  = shard_end - shard_start;

        uint64_t local_offset = current_pos - shard_start;

        while (remaining > 0 && local_offset < shard_mass) {
            
            uint64_t block_idx = local_offset / ctx->payload_cap;
            
            /* Logical Block Bounds Check */
            uint64_t max_blocks = (shard_mass + ctx->payload_cap - 1) / ctx->payload_cap;
            if (block_idx >= max_blocks) {
                res = HN4_ERR_DATA_ROT; /* Mass metadata mismatch */
                goto cleanup;
            }

            uint32_t offset_in_blk = (uint32_t)(local_offset % ctx->payload_cap);
            uint64_t bytes_available_in_blk = ctx->payload_cap - offset_in_blk;
            uint64_t bytes_left_in_shard    = shard_mass - local_offset;
            uint64_t fetch_len              = bytes_available_in_blk;
            
            if (fetch_len > bytes_left_in_shard) fetch_len = bytes_left_in_shard;
            if (fetch_len > remaining) fetch_len = remaining;

            if (HN4_UNLIKELY(fetch_len > UINT32_MAX)) {
                res = HN4_ERR_GEOMETRY;
                goto cleanup;
            }
            uint32_t chunk = (uint32_t)fetch_len;

            /* 
             * ATOMIC READ via HN4 Core
             */
            res = hn4_read_block_atomic(
                ctx->vol, 
                anchor, 
                block_idx, 
                bounce_buf, 
                ctx->block_size,
                HN4_PERM_READ | HN4_PERM_SOVEREIGN
            );

            if (HN4_UNLIKELY(HN4_IS_ERR(res))) {
                goto cleanup;
            }

            /* 
             * FIX: ABI CONTRACT ENFORCEMENT
             * hn4_read_block_atomic returns PURE PAYLOAD in bounce_buf.
             * The header is stripped by the atomic read layer.
             * Do not add offsetof(header) or we read garbage/OOB.
             */
            memcpy(cursor, (uint8_t*)bounce_buf + offset_in_blk, chunk);

            cursor       += chunk;
            current_pos  += chunk;
            local_offset += chunk;
            remaining    -= chunk;
        }
        shard_idx++;
    }

cleanup:
    hn4_hal_mem_free(bounce_buf);
    return res;
}


void hn4_tensor_close(hn4_tensor_ctx_t* ctx) 
{
    if (ctx) {
        if (ctx->vol) {
            atomic_fetch_sub(&ctx->vol->health.ref_count, 1);
        }
        if (ctx->shards)        hn4_hal_mem_free(ctx->shards);
        if (ctx->shard_offsets) hn4_hal_mem_free(ctx->shard_offsets);
        
        memset(ctx, 0xDD, sizeof(hn4_tensor_ctx_t));
        hn4_hal_mem_free(ctx);
    }
}

/* =========================================================================
 * 3. AI ACCELERATION: CONTEXT FREEZING
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_ai_freeze_context(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char*   context_tag,
    HN4_IN const void*   kv_buffer,
    HN4_IN uint64_t      len,
    HN4_IN uint32_t      gpu_id
)
{
    /* 
     * SAFETY CONTRACT:
     * This operation is Write-Atomic only at the final Anchor Commit.
     * If a crash occurs during the write loop:
     * 1. Physical blocks allocated are leaked (orphaned).
     * 2. The Anchor is never written, so the file never technically exists.
     * 3. The Scavenger (Reaper) will reclaim leaked blocks via the Zero-Scan mechanism.
     */

    if (!vol || !context_tag || !kv_buffer) return HN4_ERR_INVALID_ARGUMENT;
    
    /* Strict Profile Enforcement */
    if (vol->sb.info.format_profile != HN4_PROFILE_AI) return HN4_ERR_PROFILE_MISMATCH;

    if (vol->vol_block_size < HN4_AI_BLOCK_SIZE) {
        HN4_LOG_CRIT("AI Freeze: Volume block size too small for Tensor Ops.");
        return HN4_ERR_GEOMETRY;
    }

    if ((((uintptr_t)kv_buffer % HN4_AI_DMA_ALIGNMENT) != 0) || 
        ((len % HN4_AI_DMA_ALIGNMENT) != 0)) 
    {
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    /* 1. Anchor Construction */
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    
    anchor.seed_id.lo = hn4_hal_get_random_u64(); 
    anchor.seed_id.hi = hn4_hal_get_time_ns();
    anchor.tag_filter = hn4_cpu_to_le64(_ns_generate_tag_mask(context_tag, strlen(context_tag)));

    uint64_t dclass = HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_PINNED;
    dclass &= ~HN4_AI_TYPE_MASK; /* Fix bit collision */
    dclass |= HN4_AI_TYPE_KV_CACHE;
    
    anchor.data_class = hn4_cpu_to_le64(dclass);
    anchor.mass = hn4_cpu_to_le64(len);
    anchor.fractal_scale = 0; 
    
    /* Affinity Mapping */
    uint64_t v = 1;
    memcpy(anchor.orbit_vector, &v, 6);
    anchor.orbit_hints = hn4_cpu_to_le32(gpu_id); 

    /* 
     * 1.5. SIGNET BRANDING (Provenance Enforcement - PRE-WRITE)
     * We must brand the anchor BEFORE writing data. 
     * Branding mutates the Orbit Vector (V). Writing data first would use the
     * wrong trajectory, creating phantom blocks unreachable by the signed anchor.
     */
    {
        uint64_t author_id = 0x41495F454E47494E; /* "AI_ENGIN" */
        uint8_t sig[64];
        uint8_t pub[32];
        memset(sig, 0xEE, 64); 
        memset(pub, 0xAA, 32);

        hn4_result_t sig_res = hn4_signet_brand_anchor(vol, &anchor, author_id, sig, 64, pub);
        
        if (sig_res != HN4_OK) {
            HN4_LOG_CRIT("AI Freeze: Signet branding failed (%d). Aborting.", sig_res);
            return sig_res; 
        }
        
        /* Mark Signed */
        uint64_t dc = hn4_le64_to_cpu(anchor.data_class);
        anchor.data_class = hn4_cpu_to_le64(dc | HN4_FLAG_SIGNED);
    }

    /* 2. The Write Pipeline */
    uint64_t lba_log[MAX_ROLLBACK_TRACK];
    uint64_t remaining = len;
    uint64_t offset = 0;

    const uint8_t* ptr = (const uint8_t*)kv_buffer;

    uint32_t bs = vol->vol_block_size;
    uint32_t payload_cap = HN4_BLOCK_PayloadSize(bs); 

    uint64_t block_idx = 0;
    hn4_result_t res = HN4_OK;

    while (remaining > 0) {
        /* FIX: Use payload_cap for comparison and assignment */
        uint64_t chunk_64 = (remaining > payload_cap) ? payload_cap : remaining;
        if (chunk_64 > UINT32_MAX) {
            res = HN4_ERR_GEOMETRY;
            goto rollback;
        }
        uint32_t chunk = (uint32_t)chunk_64;
        
        res = hn4_write_block_atomic(
            vol, 
            &anchor, 
            block_idx, 
            ptr + offset, 
            chunk, 
            HN4_PERM_SOVEREIGN | HN4_PERM_WRITE
        );

        if (res != HN4_OK) goto rollback;

        /* Capture actual allocated LBA for immediate rollback if needed */
        if (block_idx < MAX_ROLLBACK_TRACK) {
             lba_log[block_idx] = _resolve_residency_verified(vol, &anchor, block_idx);
        }

        remaining -= chunk;
        offset += chunk;
        block_idx++;
    }

    /* 3. Final Commit */
    return hn4_write_anchor_atomic(vol, &anchor);

rollback:
    {
        uint64_t rollback_limit = (block_idx < MAX_ROLLBACK_TRACK) ? block_idx : MAX_ROLLBACK_TRACK;
        
        const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
        uint32_t sectors = vol->vol_block_size / caps->logical_block_size;

        for (uint64_t r = 0; r < rollback_limit; r++) {
            if (lba_log[r] != HN4_LBA_INVALID) {
                
                #ifdef HN4_USE_128BIT
                    hn4_u128_t blk = hn4_u128_from_u64(lba_log[r]);
                    hn4_addr_t phys = hn4_u128_mul_u64(blk, sectors);
                #else
                    hn4_addr_t phys = lba_log[r] * sectors;
                #endif

                hn4_free_block(vol, phys);
            }
        }
        
        if (block_idx >= MAX_ROLLBACK_TRACK) {
            HN4_LOG_WARN("AI Freeze: Rollback log overflow (%llu blocks). Scavenger will reclaim.", 
                         (unsigned long long)block_idx);
        }
    }
    return res;
}


/* =========================================================================
 * 4. AI ACCELERATION: PRE-BAKED MANIFOLDS
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_ai_persist_compute_graph(
    HN4_IN hn4_volume_t* vol,
    HN4_IN const char*   model_tag,
    HN4_IN const void*   binary_blob,
    HN4_IN uint64_t      blob_len
)
{
    if (!vol || !model_tag || !binary_blob) return HN4_ERR_INVALID_ARGUMENT;

    /* Create Anchor for the Graph */
    hn4_anchor_t anchor;
    memset(&anchor, 0, sizeof(anchor));
    
    anchor.seed_id.lo = hn4_hal_get_random_u64();
    anchor.seed_id.hi = hn4_hal_get_time_ns();
    anchor.tag_filter = hn4_cpu_to_le64(_ns_generate_tag_mask(model_tag, strlen(model_tag)));

    /* Mark as GRAPH type */
    uint64_t dclass = HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_PINNED | HN4_AI_TYPE_GRAPH;
    anchor.data_class = hn4_cpu_to_le64(dclass);
    anchor.mass = hn4_cpu_to_le64(blob_len);
    
    /* Standard sequential write logic */
    uint64_t v = 1;
    memcpy(anchor.orbit_vector, &v, 6);

    /* 
     * FIX: Apply Signet (Code Signing) BEFORE Write Loop.
     * This establishes the final Orbit Vector V' and ensures the graph data
     * lands on the branded trajectory.
     */
    uint64_t author_id = 0x47524150485F4F50; /* "GRAPH_OP" */
    uint8_t sig[64];
    uint8_t pub[32];
    memset(sig, 0xCC, 64);
    memset(pub, 0xDD, 32);

    hn4_result_t sig_res = hn4_signet_brand_anchor(vol, &anchor, author_id, sig, 64, pub);
    
    if (sig_res != HN4_OK) {
        /* Fail on branding error to prevent unsigned execution code */
        HN4_LOG_CRIT("Compute Graph Branding Failed (%d).", sig_res);
        return sig_res;
    }
    
    /* Mark as Signed */
    anchor.data_class = hn4_cpu_to_le64(hn4_le64_to_cpu(anchor.data_class) | HN4_FLAG_SIGNED);

   /* Write Loop */
    uint64_t remaining = blob_len;
    uint64_t offset = 0;
    const uint8_t* ptr = (const uint8_t*)binary_blob;
    uint32_t bs = vol->vol_block_size;

    uint32_t payload_cap = HN4_BLOCK_PayloadSize(bs);
    uint64_t block_idx = 0;

    while (remaining > 0) {

        uint64_t chunk_64 = (remaining > payload_cap) ? payload_cap : remaining;
        if (chunk_64 > UINT32_MAX) return HN4_ERR_GEOMETRY;

        uint32_t chunk = (uint32_t)chunk_64;
        
        hn4_result_t res = hn4_write_block_atomic(
            vol, &anchor, block_idx, ptr + offset, chunk, HN4_PERM_SOVEREIGN | HN4_PERM_WRITE
        );

        if (res != HN4_OK) return res; 
        
        remaining -= chunk;
        offset += chunk;
        block_idx++;
    }

    /* Final Persist (Commit Anchor) */
    return hn4_write_anchor_atomic(vol, &anchor);
}

void hn4_ai_calc_optimal_layout(
    uint32_t tensor_dims[4],    /* N, C, H, W */
    uint32_t dtype_size,        /* 2 (FP16), 4 (FP32) */
    uint32_t gpu_arch_align,    /* e.g. 256 bytes for cache line */
    hn4_size_t* out_padded_size
)
{
    uint64_t d0 = tensor_dims[0];
    uint64_t d1 = tensor_dims[1];
    uint64_t d2 = tensor_dims[2];
    uint64_t d3 = tensor_dims[3];
    
    /* Check pairs */
    if (d0 > (UINT64_MAX / d1)) goto error;
    uint64_t s01 = d0 * d1;
    
    if (d2 > (UINT64_MAX / d3)) goto error;
    uint64_t s23 = d2 * d3;
    
    /* Check final volume */
    if (s01 > (UINT64_MAX / s23)) goto error;
    uint64_t elements = s01 * s23;
    
    if (elements > (UINT64_MAX / dtype_size)) goto error;
    uint64_t raw_size = elements * dtype_size;
    
    /* 
     * Architecture Padding
     * Pad rows to align with GPU cache lines/Tensor Core tiles.
     */
    uint64_t row_bytes = tensor_dims[3] * dtype_size; /* Width * Element */
    uint64_t padding_per_row = 0;
    
    if (row_bytes % gpu_arch_align != 0) {
        padding_per_row = gpu_arch_align - (row_bytes % gpu_arch_align);
    }
    
    /* Padding Safety Cap: Return Error instead of silent switch */
    uint64_t total_padding = padding_per_row * d2 * d1 * d0;
    if (total_padding > (raw_size * 2)) {
        /* Layout is pathological (sparse/wasteful) */
        #ifdef HN4_USE_128BIT
        out_padded_size->lo = UINT64_MAX; 
        out_padded_size->hi = UINT64_MAX; 
        #else
        *out_padded_size = UINT64_MAX;
        #endif
        return;
    }

    uint64_t optimized_size = raw_size + total_padding;

    /* 
     * Block Alignment
     * Align to 64MB HN4 Block Size.
     */
    uint64_t align_mask = HN4_AI_BLOCK_SIZE - 1;

    /* FIX: Check for overflow before addition */
    if (optimized_size > (UINT64_MAX - align_mask)) goto error;

    uint64_t final_size = (optimized_size + align_mask) & ~align_mask;

#ifdef HN4_USE_128BIT
    out_padded_size->lo = final_size;
    out_padded_size->hi = 0; 
#else
    *out_padded_size = final_size;
#endif
    return;

error:
    #ifdef HN4_USE_128BIT
    out_padded_size->lo = UINT64_MAX; 
    out_padded_size->hi = UINT64_MAX;
    #else
    *out_padded_size = UINT64_MAX;
    #endif
}

/* =========================================================================
 * 5. AI ACCELERATION: HOT-SWAP LOADING
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_ai_load_tensor_direct(
    HN4_IN hn4_tensor_ctx_t* ctx,
    HN4_IN void*             dest_buffer,
    HN4_IN uint64_t          load_len,
    HN4_IN uint32_t          target_gpu_id
)
{
    if (!ctx || !dest_buffer) return HN4_ERR_INVALID_ARGUMENT;
    
    /* Verify we are operating on an optimized AI volume */
    if (ctx->vol->sb.info.format_profile != HN4_PROFILE_AI) return HN4_ERR_PROFILE_MISMATCH;

    if (load_len > ctx->total_size_bytes) {
        return HN4_ERR_INVALID_ARGUMENT;
    }

    /* 
     * 1. Establish P2P Tunnel 
     */
    void* dma_handle = NULL;
    hn4_result_t dma_res = _ai_map_p2p_bar(ctx->vol->target_device, target_gpu_id, dest_buffer, load_len, &dma_handle);
    
    /* If P2P works, we flag the HAL to use the optimized path */
    bool use_p2p = (dma_res == HN4_OK && dma_handle != NULL);

    /* 
     * 2. Prefetching (Hardware Warm-up)
     */
     uint64_t vol_cap_blocks = ctx->vol->vol_capacity_bytes / ctx->block_size;
    for (uint32_t i = 0; i < ctx->shard_count; i++) {
        hn4_anchor_t* s = &ctx->shards[i];
        uint64_t dclass = hn4_le64_to_cpu(s->data_class);
        uint64_t mass   = hn4_le64_to_cpu(s->mass);

        /* FIX: Validate shard is real before prefetching */
        if ((dclass & HN4_FLAG_VALID) && !(dclass & HN4_FLAG_TOMBSTONE) && mass > 0) {
            uint64_t g = hn4_le64_to_cpu(s->gravity_center);
            if (g < vol_cap_blocks) {
                hn4_hal_prefetch(ctx->vol->target_device, hn4_lba_from_blocks(g), 1024);
            }
        }
    }

    uint64_t remaining = load_len;
    uint64_t global_offset = 0;
    uint8_t* cursor = (uint8_t*)dest_buffer;

    while (remaining > 0) {
        
        if (use_p2p) {
            uintptr_t raw_handle = (uintptr_t)dma_handle;
            
            /* Check if the 0x8000... flag is present (sanity check) */
            if (raw_handle & 0x8000000000000000ULL) {
                /* Mask out the flag to get the raw 32-bit ID */
                uint32_t ctx_id = (uint32_t)(raw_handle & 0xFFFFFFFF);
                hn4_hal_sim_set_gpu_context(ctx_id);
            } else {
                /* Invalid handle format - fallback to CPU path or error */
                HN4_LOG_ERR("Invalid DMA Handle format. Disabling P2P.");
                use_p2p = false;
                hn4_hal_sim_clear_gpu_context();
            }
        }

        /* 
         * Execute Read.
         * Tensor Logic handles sharding/RAID. HAL handles DMA.
         * We chunk it by block_size to allow preemption.
         */
        uint64_t chunk = (remaining > ctx->block_size) ? ctx->block_size : remaining;
        
        hn4_result_t res = hn4_tensor_read(ctx, global_offset, cursor, chunk);

        if (use_p2p) {
            /* Clear context to prevent accidental P2P usage for metadata */
            hn4_hal_sim_clear_gpu_context();
        }

        if (res != HN4_OK) return res;

        remaining -= chunk;
        global_offset += chunk;
        cursor += chunk;
    }

    return HN4_OK;
}

/**
 * hn4_manifest_load
 * Reads a MANIFEST anchor and extracts the list of child UUIDs.
 * 
 * @param vol       Volume context
 * @param manifest  The anchor of the manifest file (found via ns_resolve)
 * @param out_ids   Buffer to hold UUIDs
 * @param max_count Buffer capacity
 * @return Count of IDs read
 */
uint32_t hn4_manifest_load(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* manifest,
    HN4_OUT hn4_u128_t*  out_ids,
    HN4_IN uint32_t      max_count
)
{
    /* 1. Type Check */
    uint64_t dclass = hn4_le64_to_cpu(manifest->data_class);
    if ((dclass & HN4_AI_TYPE_MASK) != HN4_AI_TYPE_MANIFEST) {
        return 0; // Not a manifest
    }

    /* 2. Read Block 0 (Manifests are usually small, fit in one block) */
    uint32_t bs = vol->vol_block_size;
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return 0;

    /* Use atomic read to get payload */
    if (hn4_read_block_atomic(vol, manifest, 0, buf, bs, HN4_PERM_READ) != HN4_OK) {
        hn4_hal_mem_free(buf);
        return 0;
    }

    /* 3. Parse Payload */
    hn4_manifest_header_t* hdr = (hn4_manifest_header_t*)buf;
    uint64_t stored_count = hn4_le64_to_cpu(hdr->count);
    
    uint32_t copy_count = (stored_count > max_count) ? max_count : (uint32_t)stored_count;
    
    /* Copy UUIDs to output */
    for (uint32_t i = 0; i < copy_count; i++) {
        out_ids[i] = hn4_le128_to_cpu(hdr->entries[i]);
    }

    hn4_hal_mem_free(buf);
    return copy_count;
}