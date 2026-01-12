/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Tensor Stream Layer (Virtualization)
 * HEADER:      hn4_tensor.h
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. IMMUTABILITY: Tensor context is strictly read-only after open.
 * 2. GEOMETRY: Supports variable-size shards via O(1) prefix-sum mapping.
 * 3. LIMITS: Max tensor size 18 EB (64-bit). Max shards: 4096 (Hard Cap).
 */

#ifndef HN4_TENSOR_H
#define HN4_TENSOR_H

#include "hn4.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"


#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Tensor Context
 * Represents a "Mounted" AI Model or Large Binary Object (LBO).
 * 
 * INTERNAL LAYOUT:
 * [ Shards Array ] -> [ Prefix Offsets ] -> [ Geometry Cache ]
 */
typedef struct {
    hn4_volume_t* vol;              /* Parent Volume Reference */
    hn4_anchor_t* shards;           /* Sorted array of Anchors (Topology) */
    
    /* 
     * Geometry Mapping (Prefix Sums)
     * Maps Shard Index -> Global Byte Start Offset.
     * Count is shard_count + 1 (Sentinel at end).
     * shard_offsets[i]   = Start of Shard i.
     * shard_offsets[i+1] = End of Shard i (Start of i+1).
     */
    uint64_t*     shard_offsets;    
    
    uint32_t      shard_count;      /* Number of active shards */
    uint64_t      total_size_bytes; /* Exact logical size (Sum of masses) */
    uint32_t      block_size;       /* Cached volume block size */
    uint32_t      payload_cap;      /* Cached payload capacity per block */
} hn4_tensor_ctx_t;

/**
 * hn4_tensor_open
 * 
 * Triggers a "Resonance Scan" to find all shards for a Model ID.
 * Sorts them by Seed ID and builds the cumulative geometry map.
 * 
 * SAFETY: Enforces monotonicity of shard sizes. Zero-mass shards cause failure.
 * 
 * @param vol       Volume to scan.
 * @param model_tag The model identifier (e.g., "model:llama-3-70b").
 * @param out_ctx   Allocated context handle.
 * @return          HN4_OK, HN4_ERR_NOT_FOUND, HN4_ERR_DATA_ROT (Bad Geometry).
 */
_Check_return_
hn4_result_t hn4_tensor_open(
    HN4_IN  hn4_volume_t* vol, 
    HN4_IN  const char*   model_tag, 
    HN4_OUT hn4_tensor_ctx_t** out_ctx
);

/**
 * hn4_tensor_read
 * 
 * Reads from the virtualized tensor stream.
 * Handles variable shard sizes, boundary crossings, and payload unpacking.
 * 
 * PERFORMANCE:
 * - Allocates one bounce buffer per call (Hoisted).
 * - Uses Binary Search for shard lookup (LogN).
 * 
 * @param ctx           Open tensor context.
 * @param global_offset Virtual byte offset (0 to total_size_bytes).
 * @param buf           Output buffer (User space).
 * @param len           Bytes to read. 
 * @return HN4_OK on success.
 * @return HN4_ERR_INVALID_ARGUMENT if offset is out of bounds.
 */
_Check_return_
hn4_result_t hn4_tensor_read(
    HN4_IN  hn4_tensor_ctx_t* ctx, 
    HN4_IN  uint64_t global_offset, 
    HN4_OUT void*    buf, 
    HN4_IN  uint64_t len
);

/**
 * hn4_tensor_close
 * Releases memory resources associated with the tensor context.
 * Safe to call on NULL or partially initialized contexts.
 */
void hn4_tensor_close(hn4_tensor_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* HN4_TENSOR_H */