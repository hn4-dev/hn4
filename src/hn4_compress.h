
/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4_compress.h
 * STATUS:      REFERENCE STANDARD
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 */

#ifndef _HN4_COMPRESS_H_
#define _HN4_COMPRESS_H_

#include "hn4.h"
#include "hn4_annotations.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Returns maximum buffer size needed for worst-case compression.
 * Guarantees no buffer overflow if dst_capacity >= bound.
 */
uint32_t hn4_compress_bound(uint32_t isize);

/* 
 * Compresses data using HN4-TCC.
 * 
 * ATOMICITY CONTRACT:
 * If return value is not HN4_OK, the contents of `dst` are UNDEFINED.
 * Caller MUST discard the buffer contents and fallback to raw storage.
 * 
 * @return HN4_OK on success.
 * @return HN4_ERR_COMPRESSION_INEFFICIENT if output >= input.
 * @return HN4_ERR_ENOSPC if dst buffer too small.
 */
hn4_result_t hn4_compress_block(
    HN4_IN  const void* src,
    HN4_IN  uint32_t    src_len,
    HN4_OUT void*       dst,
    HN4_IN  uint32_t    dst_capacity,
    HN4_OUT uint32_t*   out_size,
    HN4_IN  uint32_t    device_type, /* HN4_DEV_HDD, etc. */
    HN4_IN  uint64_t    hw_flags     /* HN4_HW_NVM, etc.  */
);


/*
 * Decompresses data.
 * Validates stream integrity and safety constraints.
 */
hn4_result_t hn4_decompress_block(
    HN4_IN  const void* src,
    HN4_IN  uint32_t    src_len,
    HN4_OUT void*       dst,
    HN4_IN  uint32_t    dst_capacity,
    HN4_OUT uint32_t*   out_size
);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_COMPRESS_H_ */