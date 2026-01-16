/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Tensor Stream Layer (Virtualization)
 * SOURCE:      hn4_tensor.c
 * STATUS:      HARDENED / PRODUCTION (v20.2)
 * AUTHOR:      Core Systems Engineering
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * -----------------------------------------------------------------------------
 * ARCHITECTURAL CONTRACT & LIMITATIONS
 * -----------------------------------------------------------------------------
 * 1. FAIL-STOP DESIGN:
 *    This layer implements a "Fail-Stop" philosophy. It does not attempt to 
 *    retry I/O, skip bad blocks, or recover from checksum errors. 
 *    Any error reported by the HAL is treated as a fatal stream corruption.
 *
 * 2. VIRTUALIZATION:
 *    Tensor streams present a contiguous byte-addressable view (0..N) composed
 *    of disjoint physical shards (Anchors). The engine handles the O(1) mapping
 *    using a prefix-sum acceleration structure.
 *
 * 3. CONCURRENCY:
 *    Contexts are read-only after creation. Multiple threads may read from the
 *    same context concurrently IF they provide their own locking for the 
 *    output buffers (the context itself is stateless during read).
 * -----------------------------------------------------------------------------
 */

#include "hn4_tensor.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_errors.h"
#include "hn4_constants.h"
#include "hn4_annotations.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* =========================================================================
 * 0. CONSTANTS & CONFIGURATION
 * ========================================================================= */

#define HN4_MAX_TENSOR_SHARDS   4096
#define HN4_SHARD_INVALID       UINT32_MAX

/* =========================================================================
 * 1. INTERNAL HELPERS
 * ========================================================================= */

/**
 * _shard_cmp
 * Sorts shards by 128-bit Seed ID to establish logical monotonicity.
 * This ensures that if shards are scattered on disk, they assemble in
 * a deterministic order based on their creation identity.
 */
static int _shard_cmp(const void* a, const void* b) 
{
    const hn4_anchor_t* sa = (const hn4_anchor_t*)a;
    const hn4_anchor_t* sb = (const hn4_anchor_t*)b;
    
    /* Comparison must be done on Host Endian values */
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
 * The `shard_offsets` array contains (shard_count + 1) entries.
 * entry[i] is the start of shard i.
 * entry[i+1] is the end of shard i (and start of shard i+1).
 *
 * Returns:
 * - [0 .. shard_count-1] on success.
 * - HN4_SHARD_INVALID    on failure (OOB).
 */
static uint32_t _find_shard_idx(const hn4_tensor_ctx_t* ctx, uint64_t global_pos) 
{
    /* Invariant: Global position must be strictly less than total mass */
    if (HN4_UNLIKELY(global_pos >= ctx->total_size_bytes)) {
        return HN4_SHARD_INVALID;
    }

    uint32_t low  = 0;
    uint32_t high = ctx->shard_count - 1;
    
    while (low <= high) {
        uint32_t mid = low + ((high - low) >> 1);
        
        uint64_t start = ctx->shard_offsets[mid];
        uint64_t end   = ctx->shard_offsets[mid + 1];

        if (global_pos >= start && global_pos < end) {
            return mid;
        }
        
        if (global_pos < start) {
            if (HN4_UNLIKELY(mid == 0)) {
                /* Should be caught by global_pos >= total check, but defensive */
                return HN4_SHARD_INVALID;
            }
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    
    return HN4_SHARD_INVALID;
}

/* =========================================================================
 * 2. PUBLIC API IMPLEMENTATION
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

    /* ---------------------------------------------------------------------
     * PHASE 1: Gather (Resonance Scan)
     * --------------------------------------------------------------------- */
    ctx->shards = hn4_hal_mem_alloc(sizeof(hn4_anchor_t) * HN4_MAX_TENSOR_SHARDS);
    if (!ctx->shards) {
        result = HN4_ERR_NOMEM;
        goto failure;
    }

    /* Scan the Cortex for anchors matching the tag */
    result = hn4_ns_gather_tensor_shards(
        vol, model_tag, ctx->shards, HN4_MAX_TENSOR_SHARDS, &found_count);

    if (result != HN4_OK) goto failure;
    
    if (found_count == 0) {
        result = HN4_ERR_NOT_FOUND;
        goto failure;
    }

    /* 
     * SAFETY CRITICAL:
     * If found_count == MAX, we cannot know if the gather was exhaustive 
     * or truncated. We reject ambiguity to prevent serving a partial model.
     */
    if (HN4_UNLIKELY(found_count == HN4_MAX_TENSOR_SHARDS)) {
        HN4_LOG_CRIT("Tensor Open: Shard count hit limit (%u). Ambiguous completeness.", 
                     HN4_MAX_TENSOR_SHARDS);
        result = HN4_ERR_TAG_OVERFLOW;
        goto failure;
    }

    /* ---------------------------------------------------------------------
     * PHASE 2: Sort & Geometry Map Building
     * --------------------------------------------------------------------- */
    qsort(ctx->shards, found_count, sizeof(hn4_anchor_t), _shard_cmp);

    /* Allocate (N + 1) slots for the offset map to hold the EOF sentinel */
    ctx->shard_offsets = hn4_hal_mem_alloc(sizeof(uint64_t) * (found_count + 1));
    if (!ctx->shard_offsets) {
        result = HN4_ERR_NOMEM;
        goto failure;
    }

    for (uint32_t i = 0; i < found_count; i++) {
        /* Store Start Offset */
        ctx->shard_offsets[i] = accumulator;
        
        uint64_t mass = hn4_le64_to_cpu(ctx->shards[i].mass);
        
        /* Check 1: Zero Mass (Ambiguous Topology) */
        if (HN4_UNLIKELY(mass == 0)) {
            HN4_LOG_CRIT("Tensor Open: Shard %u has zero mass.", i);
            result = HN4_ERR_DATA_ROT;
            goto failure;
        }

        /* Check 2: 64-bit Address Space Overflow */
        if (HN4_UNLIKELY((UINT64_MAX - accumulator) < mass)) {
            HN4_LOG_CRIT("Tensor Open: Mass overflow (Exceeds 18 EB).");
            result = HN4_ERR_GEOMETRY;
            goto failure;
        }
        
        accumulator += mass;
    }
    
    /* Store End Sentinel (EOF) at index N */
    ctx->shard_offsets[found_count] = accumulator;

    /* Integrity Check: The universe must start at 0 */
    if (HN4_UNLIKELY(ctx->shard_offsets[0] != 0)) {
        HN4_LOG_CRIT("Tensor Open: Offset map corrupted in RAM.");
        result = HN4_ERR_INTERNAL;
        goto failure;
    }

    /* ---------------------------------------------------------------------
     * PHASE 3: Context Finalization
     * --------------------------------------------------------------------- */
    ctx->vol              = vol;
    ctx->shard_count      = found_count;
    ctx->total_size_bytes = accumulator;
    ctx->block_size       = vol->vol_block_size;
    
    /* 
     * CACHE PAYLOAD CAPACITY:
     * This avoids repeated calculations during the read loop.
     * Logic: BlockSize - HeaderSize
     */
    uint32_t p_cap = HN4_BLOCK_PayloadSize(ctx->block_size);
    
    if (HN4_UNLIKELY(p_cap == 0 || p_cap >= ctx->block_size)) {
        HN4_LOG_CRIT("Tensor Open: Invalid block geometry. BS=%u P=%u", 
                     ctx->block_size, p_cap);
        result = HN4_ERR_GEOMETRY;
        goto failure;
    }
    ctx->payload_cap = p_cap;

    /* Acquire reference to volume to prevent unmount while tensor is open */
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
    if (HN4_UNLIKELY(!ctx || !buf)) return HN4_ERR_INVALID_ARGUMENT;
    if (len == 0) return HN4_OK;

    /* Boundary Check: Strict EOF */
    if (global_offset >= ctx->total_size_bytes) {
        return HN4_ERR_INVALID_ARGUMENT;
    }
    
    /* Clamp Read Length to EOF (Allow Short Read) */
    uint64_t read_len = len;
    if ((global_offset + len) > ctx->total_size_bytes) {
        read_len = ctx->total_size_bytes - global_offset;
    }

    /* 
     * BOUNCE BUFFER ALLOCATION:
     * We MUST allocate a bounce buffer. HN4 blocks contain headers, so the data
     * on disk is not contiguous. We cannot read directly into the user buffer.
     *
     * OPTIMIZATION NOTE: 
     * We allocate per-read here to keep the Context stateless/thread-safe.
     * In extremely high-throughput scenarios, this malloc/free pair is observable overhead.
     * However, it guarantees memory safety.
     */
    void* bounce_buf = hn4_hal_mem_alloc(ctx->block_size);
    if (HN4_UNLIKELY(!bounce_buf)) return HN4_ERR_NOMEM;

    uint8_t* cursor      = (uint8_t*)buf;
    uint64_t remaining   = read_len;
    uint64_t current_pos = global_offset;
    hn4_result_t res     = HN4_OK;

    /* 1. Initial Shard Lookup */
    uint32_t shard_idx = _find_shard_idx(ctx, current_pos);
    
    if (HN4_UNLIKELY(shard_idx == HN4_SHARD_INVALID)) {
        res = HN4_ERR_GEOMETRY;
        goto cleanup;
    }

    /* ---------------------------------------------------------------------
     * STREAM LOOP
     * Iterates through shards until the request is satisfied.
     * --------------------------------------------------------------------- */
    while (remaining > 0 && shard_idx < ctx->shard_count) {
        
        hn4_anchor_t* anchor = &ctx->shards[shard_idx];
        uint64_t shard_start = ctx->shard_offsets[shard_idx];
        uint64_t shard_end   = ctx->shard_offsets[shard_idx + 1];
        uint64_t shard_mass  = shard_end - shard_start;

        /* Calculate offset relative to THIS shard */
        uint64_t local_offset = current_pos - shard_start;

        /* Monotonicity Guard */
        if (HN4_UNLIKELY(local_offset >= shard_mass)) {
            /* Should be impossible if _find_shard_idx works, but defense-in-depth */
            res = HN4_ERR_GEOMETRY;
            goto cleanup;
        }

        /* Inner Loop: Blocks within Shard */
        while (remaining > 0 && local_offset < shard_mass) {
            
            /* Logic to Physical Mapping inside the Shard */
            uint64_t block_idx = local_offset / ctx->payload_cap;
            uint32_t offset_in_blk = (uint32_t)(local_offset % ctx->payload_cap);
            
            /* 
             * Calculate how much valid data is in THIS block.
             */
            uint64_t bytes_available_in_blk = ctx->payload_cap - offset_in_blk;
            uint64_t bytes_left_in_shard    = shard_mass - local_offset;
            uint64_t fetch_len              = bytes_available_in_blk;
            
            /* Clamp to shard boundary */
            if (fetch_len > bytes_left_in_shard) fetch_len = bytes_left_in_shard;
            /* Clamp to user request */
            if (fetch_len > remaining) fetch_len = remaining;

            /* 
             * SAFE DOWNCAST:
             * payload_cap fits in uint32_t. fetch_len <= payload_cap.
             */
            uint32_t chunk = (uint32_t)fetch_len;

            /* 
             * ATOMIC READ: 
             * Uses the standard ballistic read pipeline. 
             * This handles headers, CRCs, decompression, and phantom blocks.
             */
            res = hn4_read_block_atomic(
                ctx->vol, 
                anchor, 
                block_idx, 
                bounce_buf, 
                ctx->block_size
            );

            if (HN4_UNLIKELY(HN4_IS_ERR(res))) {
                /* Error propagated from read pipeline (e.g. DATA_ROT) */
                goto cleanup;
            }

            /* Copy payload to user buffer */
            memcpy(cursor, (uint8_t*)bounce_buf + offset_in_blk, chunk);

            /* Advance Cursors */
            cursor       += chunk;
            current_pos  += chunk;
            local_offset += chunk;
            remaining    -= chunk;
        }

        /* Move to next shard if we still need data */
        shard_idx++;
    }

cleanup:
    hn4_hal_mem_free(bounce_buf);
    return res;
}

void hn4_tensor_close(hn4_tensor_ctx_t* ctx) 
{
    if (ctx) {
        /* Release Reference to Volume */
        if (ctx->vol) {
            atomic_fetch_sub(&ctx->vol->health.ref_count, 1);
        }

        if (ctx->shards)        hn4_hal_mem_free(ctx->shards);
        if (ctx->shard_offsets) hn4_hal_mem_free(ctx->shard_offsets);
        
        /* Wipe context to prevent UAF or leaks if pointer is reused */
        memset(ctx, 0xDD, sizeof(hn4_tensor_ctx_t));
        
        hn4_hal_mem_free(ctx);
    }
}