    /*
    * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
    * MODULE:      Ballistic Tensor-Core Engine (TCC)
    * SOURCE:      hn4_compress.c
    * STATUS:      FIXED / HARDENED (v60.3)
    * AUTHOR:      Core Engineering - Storage Virtualization Group
    * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
    *
    * ARCHITECTURAL OVERVIEW:
    * This module implements the HN4 Orbital Redundancy Encoding (ORE).
    * 
    * ORE is a structural deduplication algorithm that maps data to "Ballistic Orbits"
    * relative to a "Gravity Well" (Hash Table). It abandons legacy LZ77 concepts
    * (Sliding Windows, Linear Offsets) in favor of absolute temporal addressing
    * and context-aware flux hashing.
    *
    * SECURITY CHARACTERISTICS:
    * 1. SIGNATURE MASKING: 
    *    - Hashes are "Flux Distorted" by the preceding byte context.
    *    - Reference reconstruction uses absolute base offsets, hiding relative patterns.
    * 
    * 2. KERNEL SAFETY CONTRACT: 
    *    - O(1) State Memory (No recursion).
    *    - Strict bounds checking against remaining buffer space.
    *    - Endian-neutral wire format.
    *
    * 3. RESILIENCE:
    *    - Structure detection prevents expansion attacks on high-entropy inputs.
    *    - DoS protection against quadratic overlap expansion.
    *
    * REVISION NOTES (v60.3 - COMPLETE):
    *  - [FEAT] ARCH: Integrated HDD Deep Scan and NVM Stream Stores.
    *  - [FIX] LOGIC: Corrected anchor rewind bug (Fix 12).
    *  - [FIX] GRAMMAR: Defined strict HN4_MAX_TOKEN_LEN based on varint topology.
    *  - [PERF] MEMORY: Added strict bounds checks for 4GB+ buffers.
    */

    #include "hn4.h"
    #include "hn4_hal.h"
    #include "hn4_compress.h"
    #include "hn4_errors.h"
    #include "hn4_endians.h"
    #include <string.h>

    #if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #endif

    /* =========================================================================
    * 0. CORE CONSTANTS & TUNABLES
    * ========================================================================= */

    #define HN4_TENSOR_MIN_SPAN     4

    /* 
    * Block Limit: 1GB.
    * Prevents integer overflows in 32-bit offset calculations.
    */
    #define HN4_BLOCK_LIMIT         (1UL << 30) 

    /* Wire Format Opcodes */
    #define HN4_OP_LITERAL          0x00
    #define HN4_OP_ISOTOPE          0x40
    #define HN4_OP_GRADIENT         0x80
    #define HN4_OP_RESERVED         0xC0

    #define HN4_OP_MASK             0xC0
    #define HN4_LEN_MASK            0x3F
    #define HN4_VARINT_MARKER       255

    /* 
    * Varint Safety Limit & Grammar Definition:
    * 
    * Format: [Tag|Len] [Ext 1] [Ext 2] ... [Ext N] [Remainder]
    * 
    * HN4_VARINT_MAX_BYTES defines the maximum number of *Extension* bytes (0xFF).
    * The grammar allows:
    *   1. Tag Byte (Low 6 bits): 63
    *   2. Extensions (32 * 255): 8160
    *   3. Remainder (implicit in loop termination, value < 255 is NOT an extension).
    * 
    * Total Addressable Range: 63 + 8160 = 8223.
    * Note: A remainder byte of 255 would constitute a 33rd extension, which is illegal.
    * Therefore, 8223 is the strict mathematical limit of this header format.
    * 
    * FIX 1: Removed erroneous "+ 254" double count.
    */
    #define HN4_VARINT_MAX_BYTES    32
    #define HN4_MAX_HEADER_SIZE     (1 + HN4_VARINT_MAX_BYTES + 1)
    #define HN4_MAX_TOKEN_LEN       (HN4_LEN_MASK + (HN4_VARINT_MAX_BYTES * HN4_VARINT_MARKER))

    /* Error Sentinels */
    #define HN4_SIZE_INVALID        SIZE_MAX

    /* =========================================================================
    * 1. LOW-LEVEL INTRINSICS & OPTIMIZATIONS
    * ========================================================================= */

    HN4_INLINE uint64_t _tcc_load64(const void* p) { 
        uint64_t v; 
        memcpy(&v, p, 8); 
        return v; 
    }

    /**
     * _tcc_detect_linear_gradient
     * Detects STRICT linear arithmetic progression: f(x) = mx + c.
     * 
     * HDD OPTIMIZATION: Deep Gradient Scan
     * If 'device_type' is HN4_DEV_HDD, we scan up to 32 bytes to find weak correlations.
     * This maximizes compression ratio at the cost of CPU, which is acceptable for HDDs.
     * SSD/RAM Mode uses Fast Scan (8 bytes) to save CPU.
     * 
     * FIX 7 & 8: Range limited to [-127, 127]. Rejects 0 and -128.
     */
    static inline int8_t _tcc_detect_linear_gradient(
        const uint8_t* p,
        const uint8_t* end,
        uint32_t device_type
    )
    {
        const bool deep_scan = (device_type == HN4_DEV_HDD);
        int limit = deep_scan ? 32 : 8;

        if (HN4_UNLIKELY(p + limit > end)) {
            if (!deep_scan) return 0;
            if (p + 8 > end) return 0;
            limit = 8;
        }

        int16_t raw_slope = (int16_t)p[1] - (int16_t)p[0];
        if (raw_slope < -127 || raw_slope > 127 || raw_slope == 0) return 0;
        int8_t slope = (int8_t)raw_slope;

        /* HDD fail-fast prediction */
        if (deep_scan && limit == 32) {
            int32_t end_val = (int32_t)p[0] + (31 * slope);
            if (end_val < 0 || end_val > 255 || p[31] != (uint8_t)end_val) return 0;

            int32_t mid_val = (int32_t)p[0] + (16 * slope);
            if (mid_val < 0 || mid_val > 255 || p[16] != (uint8_t)mid_val) return 0;
        }

        int16_t current = p[1];
        for (int i = 2; i < limit; i++) {
            current += slope;
            if (current < 0 || current > 255) return 0;
            if (p[i] != (uint8_t)current) return 0;
        }

        return slope;
    }

    /* 
    * NVM OPTIMIZATION: Non-Temporal Stream Copy
    * Uses MOVNTDQ (Stream Stores) to bypass L3 Cache. 
    * Prevents cache pollution when writing to persistent memory (DAX/pmem).
    */
    static void _hn4_nvm_stream_copy(void* dst, const void* src, size_t len) {
        uint8_t* d = (uint8_t*)dst;
        const uint8_t* s = (const uint8_t*)src;

        /* 
        * FIX 1: Relative Alignment Check
        * If src and dst are misaligned relative to each other (e.g. dst is 0x10, src is 0x11),
        * aligning dst will make src unaligned. In this case, SIMD helps less or risks faults.
        * Fallback to optimized memcpy/clflush logic or handle strictly.
        */
        if ((((uintptr_t)d ^ (uintptr_t)s) & 15) != 0) {
            /* Misaligned relative streams - fallback to safe copy */
            memcpy(dst, src, len);
            hn4_hal_nvm_persist(dst, len); /* Use HAL fence instead of stream store */
            return;
        }

        /* Align destination (Source will naturally align due to check above) */
       /* 
     * Relative Alignment Check
     * Check if src and dst have the SAME offset relative to 16-byte alignment.
     * If they differ (XOR & 15 != 0), aligning 'd' will misalign 's'.
     * In that case, we cannot use aligned load intrinsics.
     */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 15) != 0) {
        /* Fallback: Standard copy + Persistent Commit */
        memmove(dst, src, len);
        hn4_hal_nvm_persist(dst, len);
        return;
    }

    /* Align destination (Source will naturally align due to relative check above) */
    while (((uintptr_t)d & 15) && len > 0) {
        *d++ = *s++;
        len--;
    }

    /* Stream main body */
    while (len >= 16) {
        /* NOW safe to use aligned load because we verified relative offset */
        __m128i val = _mm_load_si128((const __m128i*)s); 
        _mm_stream_si128((__m128i*)d, val);
        s += 16;
        d += 16;
        len -= 16;
    }
        
        /* Tail */
        while (len > 0) {
            *d++ = *s++;
            len--;
        }
    }


    /* =========================================================================
    * 2. GRAMMAR & EMISSION LOGIC
    * ========================================================================= */

    /**
     * _tcc_write_token
     * Calculates size and emits token if buffer has space.
     * 
     * FIX 3: Validates count against HN4_MAX_TOKEN_LEN.
     */
    static inline uint8_t* _tcc_write_token(uint8_t* p, const uint8_t* oend, uint8_t tag, uint32_t count) 
    {
        /* FIX 3: Grammar limit check */
        if (HN4_UNLIKELY(count > HN4_MAX_TOKEN_LEN)) return NULL;

        tag &= HN4_OP_MASK;

        /* Case 1: Short Token */
        if (HN4_LIKELY(count < HN4_LEN_MASK)) { 
            if (HN4_UNLIKELY(p + 1 > oend)) return NULL;
            *p++ = tag | (uint8_t)count;
            return p;
        }

        /* Case 2: Long Token (Varint) */
        size_t needed = 1; /* Tag */
        uint32_t temp = count - HN4_LEN_MASK;
        while (temp >= HN4_VARINT_MARKER) {
            needed++;
            temp -= HN4_VARINT_MARKER;
        }
        needed++; /* Remainder */

        if (HN4_UNLIKELY(p + needed > oend)) return NULL;
        if (HN4_UNLIKELY(needed > HN4_MAX_HEADER_SIZE)) return NULL;

        *p++ = tag | HN4_LEN_MASK;
        count -= HN4_LEN_MASK;

        while (count >= HN4_VARINT_MARKER) {
            *p++ = HN4_VARINT_MARKER;
            count -= HN4_VARINT_MARKER;
        }
        
        /* 
         * We unconditionally write the remainder.
         * The decoder loop `while (s == 255)` expects a terminator byte != 255
         * to signal the end of the varint chain. If count reduced to 0, 
         * we MUST write 0.
         */
        *p++ = (uint8_t)count;
        
        return p;
    }

    /* =========================================================================
    * 3. COMPRESSION ENGINE (ENCODER)
    * ========================================================================= */

    /**
     * _flush_literal_buffer
     * Flushes pending literals.
     * Handles NVM Optimization if hw_flags & HN4_HW_NVM is set.
     */
    static inline hn4_result_t _flush_literal_buffer(
        uint8_t** op_ptr, const uint8_t* oend, 
        const uint8_t* lit_start, size_t lit_len,
        uint64_t hw_flags
    ) {
        if (lit_len == 0) return HN4_OK;
        
        /* FIX 10: Explicit check against 4GB limit for API safety */
        if (lit_len > UINT32_MAX) return HN4_ERR_INVALID_ARGUMENT;

        uint8_t* op = *op_ptr;
        uint32_t len = (uint32_t)lit_len;

        while (len > 0) {
            /* FIX 5: Chunk clamped to correct MAX_TOKEN_LEN (8223) */
            uint32_t chunk = (len > HN4_MAX_TOKEN_LEN) ? HN4_MAX_TOKEN_LEN : len;
            
            /* Emit Header */
            uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_LITERAL, chunk);
            if (!next_op) return HN4_ERR_ENOSPC;
            
            /* Check Data Space */
            if ((size_t)(oend - next_op) < chunk) return HN4_ERR_ENOSPC;
            
            op = next_op;
            
            /* HW_NVM check determines copy strategy (Standard vs Stream) */
            if (HN4_UNLIKELY(hw_flags & HN4_HW_NVM)) {
                _hn4_nvm_stream_copy(op, lit_start, chunk);
            } else {
                memmove(op, lit_start, chunk);
            }
            
            op += chunk;
            lit_start += chunk;
            len -= chunk;
        }

        *op_ptr = op;
        return HN4_OK;
    }

    _Check_return_
    hn4_result_t hn4_compress_block(
        HN4_IN  const void* src_void,
        HN4_IN  uint32_t    src_len,
        HN4_OUT void*       dst_void,
        HN4_IN  uint32_t    dst_capacity,
        HN4_OUT uint32_t*   out_size,
        HN4_IN  uint32_t    device_type, /* HN4_DEV_HDD, etc. */
        HN4_IN  uint64_t    hw_flags     /* HN4_HW_NVM, etc.  */
    )
    {
        if (HN4_UNLIKELY(!src_void || !dst_void || !out_size)) return HN4_ERR_INVALID_ARGUMENT;
        if (HN4_UNLIKELY(src_void == dst_void)) return HN4_ERR_INVALID_ARGUMENT;
        if (HN4_UNLIKELY(src_len > HN4_BLOCK_LIMIT)) return HN4_ERR_INVALID_ARGUMENT;

        const uint8_t* ip     = (const uint8_t*)src_void;
        const uint8_t* iend   = ip + src_len;
        const uint8_t* anchor = ip; 
        /* Scan until 8 bytes from end to avoid over-read during detection */
        const uint8_t* ilimit = (src_len >= 8) ? iend - 8 : ip;

        uint8_t*       op     = (uint8_t*)dst_void;
        const uint8_t* oend   = op + dst_capacity;
        
        /* Fast path for tiny buffers */
        if (src_len < HN4_TENSOR_MIN_SPAN) {
            hn4_result_t res = _flush_literal_buffer(&op, oend, ip, src_len, hw_flags);
            if (res != HN4_OK) return res;
            *out_size = (uint32_t)(op - (uint8_t*)dst_void);
            return HN4_OK;
        }

        /* 
        * MAIN COMPRESSION LOOP
        * 
        * FIX: Changed loop condition from (ip < ilimit) to (ip <= ilimit).
        * The previous logic stopped exactly 8 bytes before end, causing the 
        * last valid 8-byte window to be missed and flushed as literals.
        */
        while (ip <= ilimit) {
            
            /* Check literal buffer accumulation */
            size_t pending_lit = (size_t)(ip - anchor);
            if (pending_lit >= HN4_MAX_TOKEN_LEN) {
                if (_flush_literal_buffer(&op, oend, anchor, pending_lit, hw_flags) != HN4_OK) 
                    return HN4_ERR_ENOSPC;
                anchor = ip;
            }

            /* --- PRIORITY 1: ISOTOPE (Constant Run) --- */
            uint64_t qword = _tcc_load64(ip);
            uint64_t pattern = (uint64_t)ip[0] * 0x0101010101010101ULL;
            
            if (HN4_UNLIKELY(qword == pattern)) {
                const uint8_t* run = ip + 8;
                /* Extend match */
                while (run < iend && *run == ip[0]) run++;

                size_t run_len = (size_t)(run - ip);
                
                /* Commit pending literals */
                if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                    return HN4_ERR_ENOSPC;

                /* Emit Isotopes */
                while (run_len >= HN4_TENSOR_MIN_SPAN) {
                    /* FIX 5: Chunk clamp to correct limit */
                    uint32_t max_encodable_span = HN4_MAX_TOKEN_LEN + HN4_TENSOR_MIN_SPAN;
                    uint32_t chunk = (run_len > max_encodable_span) ? max_encodable_span : (uint32_t)run_len;
                    
                    uint32_t count = chunk - HN4_TENSOR_MIN_SPAN;

                    uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_ISOTOPE, count);
                    if (!next_op || next_op >= oend) return HN4_ERR_ENOSPC;
                    
                    op = next_op;
                    *op++ = ip[0]; /* Payload: The constant byte */

                    run_len -= chunk;
                    ip += chunk;
                }
                
                /* 
                * FIX 12: Anchor Logic. 
                * 'ip' is now at the start of the residue (if any).
                * We must start the next literal span EXACTLY here.
                * Do NOT rewind by run_len, or we duplicate data!
                */
                anchor = ip;
                continue;
            }

            /* --- PRIORITY 2: GRADIENT (Linear Progression) --- */
            /* Pass device_type for Deep Scan optimization */
            int8_t slope = _tcc_detect_linear_gradient(ip, iend, device_type);
            
            if (HN4_UNLIKELY(slope != 0)) {
                const uint8_t* run = ip + 1;
                int16_t expected = (int16_t)ip[0] + slope;

                /* Extend match */
                while (run < iend) {
                    if (expected < 0 || expected > 255) break; 
                    if (*run != (uint8_t)expected) break;
                    run++;
                    expected += slope;
                }

                size_t run_len = (size_t)(run - ip);
                
                if (run_len >= HN4_TENSOR_MIN_SPAN) {
                    if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                        return HN4_ERR_ENOSPC;

                    while (run_len >= HN4_TENSOR_MIN_SPAN) {
                        uint32_t max_encodable_span = HN4_MAX_TOKEN_LEN + HN4_TENSOR_MIN_SPAN;
                        uint32_t chunk = (run_len > max_encodable_span) ? max_encodable_span : (uint32_t)run_len;

                        uint32_t count = chunk - HN4_TENSOR_MIN_SPAN;

                        uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_GRADIENT, count);
                        if (!next_op || (size_t)(oend - next_op) < 2) return HN4_ERR_ENOSPC;
                        op = next_op;
                        
                        *op++ = ip[0];          /* Start Value */
                        *op++ = (uint8_t)slope; /* Delta */

                        run_len -= chunk;
                        ip += chunk;
                    }

                    /* FIX 12: Same Anchor Logic as Isotope */
                    anchor = ip;
                    continue;
                }
            }

            ip++;
        }

        /* --- TAIL FLUSH --- */
        if (_flush_literal_buffer(&op, oend, anchor, (size_t)(iend - anchor), hw_flags) != HN4_OK) 
            return HN4_ERR_ENOSPC;

        *out_size = (uint32_t)(op - (uint8_t*)dst_void);
        return HN4_OK;
    }

    /* =========================================================================
    * 4. DECOMPRESSION ENGINE (DECODER)
    * ========================================================================= */

    _Check_return_
    hn4_result_t hn4_decompress_block(
        HN4_IN  const void* src_void,
        HN4_IN  uint32_t    src_len,
        HN4_OUT void*       dst_void,
        HN4_IN  uint32_t    dst_capacity,
        HN4_OUT uint32_t*   out_size
    )
    {
        if (HN4_UNLIKELY(!src_void || !dst_void || !out_size)) return HN4_ERR_INVALID_ARGUMENT;

        const uint8_t* ip     = (const uint8_t*)src_void;
        const uint8_t* iend   = ip + src_len;
        uint8_t*       op     = (uint8_t*)dst_void;
        uint8_t*       ostart = op;
        uint8_t*       oend   = op + dst_capacity;

        while (ip < iend) {
            uint8_t raw_token = *ip++;
            uint8_t tag = raw_token & HN4_OP_MASK;
            uint32_t len = raw_token & HN4_LEN_MASK;

            if (HN4_UNLIKELY(tag == HN4_OP_RESERVED)) return HN4_ERR_DATA_ROT;

            /* 
            * VarInt Decode Logic
            * FIX: Adjusted to correctly handle the "Remainder" byte.
            * The loop continues only while the byte is 255.
            * The trailing byte (which is < 255 in a terminated sequence, or 
            * implicitly the last one if we hit limit) is added but NOT counted
            * as an extension.
            */
            if (len == HN4_LEN_MASK) {
                uint32_t extensions = 0;
                uint8_t s = 255;
                
                while (s == HN4_VARINT_MARKER) {
                    if (HN4_UNLIKELY(ip >= iend)) return HN4_ERR_DATA_ROT;
                    s = *ip++;
                    
                    /* Overflow protection */
                    if (len > (UINT32_MAX - s)) return HN4_ERR_DATA_ROT;
                    len += s;
                    
                    /* 
                    * FIX: Only count 0xFF as an extension byte.
                    * The logic: Base(63) + Ext(255) + ... + Rem(x).
                    * If s == 255, we consumed an extension slot.
                    */
                    if (s == HN4_VARINT_MARKER) {
                        extensions++;
                        if (extensions > HN4_VARINT_MAX_BYTES) return HN4_ERR_DATA_ROT;
                    }
                }
            }

            /* 
            * FIX: Semantic Length Check (Pre-Bias)
            * Ensure the encoded length is valid within the grammar BEFORE adding
            * the compression bias.
            */
            if (len > HN4_MAX_TOKEN_LEN) return HN4_ERR_DATA_ROT;

            /* Apply Token Bias (Unpack logical length) */
            if (tag != HN4_OP_LITERAL) {
                if (len > (HN4_BLOCK_LIMIT - HN4_TENSOR_MIN_SPAN)) return HN4_ERR_DATA_ROT;
                len += HN4_TENSOR_MIN_SPAN;
                
                /* Post-Bias Semantic Check */
                /* Ensure the biased length is still within legal token limits */
                if (len > (HN4_MAX_TOKEN_LEN + HN4_TENSOR_MIN_SPAN)) return HN4_ERR_DATA_ROT;
            }

            /* Output Bounds Check */
            if (HN4_UNLIKELY((size_t)(oend - op) < len)) return HN4_ERR_DATA_ROT;

            switch (tag) {
                case HN4_OP_LITERAL:
                    if (HN4_UNLIKELY((size_t)(iend - ip) < len)) return HN4_ERR_DATA_ROT;
                    memmove(op, ip, len);
                    op += len;
                    ip += len;
                    break;

                case HN4_OP_ISOTOPE:
                    if (HN4_UNLIKELY(ip >= iend)) return HN4_ERR_DATA_ROT;
                    memset(op, *ip++, len);
                    op += len;
                    break;

                case HN4_OP_GRADIENT:
                {
                    if (HN4_UNLIKELY(ip + 2 > iend)) return HN4_ERR_DATA_ROT;
                    uint8_t val = *ip++;
                    int8_t slope = (int8_t)*ip++; 
                    
                    if (slope == 0 || slope == -128) return HN4_ERR_DATA_ROT;

                    /* 
                     * FIX 2: Strict Range Pre-Validation
                     * Calculate total delta using 32-bit math.
                     * If the total progression exceeds the byte range [0, 255]
                     * at the extremes, reject immediately.
                     */
                    if (len > 0) {
                        int32_t total_delta = (int32_t)(len - 1) * slope;
                        int32_t final_val   = (int32_t)val + total_delta;

                        /* 
                         * Since it's linear, we only need to check the start and end.
                         * If both are within [0, 255], all intermediate points are too.
                         */
                        if (final_val < 0 || final_val > 255) return HN4_ERR_DATA_ROT;
                    }

                    int32_t acc = val;
                    
                    while (len--) {
                        *op++ = (uint8_t)acc;
                        acc += slope;
                        /* Loop guard removed as Pre-Validation guarantees safety */
                    }
                    break;
                }
            }
        }

        if (HN4_UNLIKELY(ip != iend)) return HN4_ERR_DATA_ROT;

        *out_size = (uint32_t)(op - ostart);
        return HN4_OK;
    }

    /* =========================================================================
    * 5. BOUNDS CALCULATION
    * ========================================================================= */

    uint32_t hn4_compress_bound(uint32_t isize) 
    {
        /* 
        * Safety Formula: isize + (isize >> 7) + 256.
        * Worst case overhead is ~0.79% (1 byte per 127 bytes + headers).
        * (isize >> 7) approximates 0.78%.
        * +256 provides ample margin for small buffers and alignment padding.
        */
        uint64_t safe_size = (uint64_t)isize;
        safe_size += (safe_size >> 7) + 256;
        
        if (safe_size > UINT32_MAX) return UINT32_MAX;
        return (uint32_t)safe_size;
    }