    /*
    * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
    * MODULE:      Ballistic Tensor-Core Engine (TCC)
    * SOURCE:      hn4_compress.c
    * STATUS:      HARDENED (v60.3)
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

    #define HN4_OP_LITERAL          0x00
    #define HN4_OP_ISOTOPE          0x40
    #define HN4_OP_GRADIENT         0x80
    #define HN4_TSM_GRANULARITY     4    /* 4 bytes (uint32_t) per bit */
    #define HN4_TSM_MIN_SAVINGS     4    /* Minimum bytes saved to justify op */
    #define HN4_OP_BITMASK          0xC0 /* Tensor Sparse Mask */
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
    */
    #define HN4_VARINT_MAX_BYTES    32
    #define HN4_MAX_HEADER_SIZE     (1 + HN4_VARINT_MAX_BYTES + 1)
    #define HN4_MAX_TOKEN_LEN       (HN4_LEN_MASK + (HN4_VARINT_MAX_BYTES * HN4_VARINT_MARKER))

    /* Error Sentinels */
    #define HN4_SIZE_INVALID        SIZE_MAX

    /* --- EXTENSION PROTOCOL (HN4 v61.0) --- */
    #define HN4_EXT_ESCAPE          0x00 
    #define HN4_EXT_OP_LEXICON      0x01
    #define HN4_EXT_OP_MANIFOLD     0x02

typedef struct {
    const char* str;
    size_t      len;
} hn4_lexicon_entry_t;

/* Optimized Table (Pre-calculated lengths) */
#define HN4_LEXICON_COUNT 64

static const hn4_lexicon_entry_t _hn4_lexicon_table[HN4_LEXICON_COUNT] = {
    /* --- TIER 0: Structural --- */
    { "{\"id\":", 6 },       { "http://", 7 },        { "https://", 8 },       { "error", 5 },
    { "false", 5 },          { "true", 4 },           { "null", 4 },           { "value", 5 },
    { "timestamp", 9 },      { " <div class=\"", 13 },{ "background", 10 },    { "function", 8 },
    { "return", 6 },         { "success", 7 },        { "jsonrpc", 7 },        { "application", 11 },

    /* --- TIER 1: JSON & API Metadata --- */
    { "\":\"", 3 },          { "\",\"", 3 },          { "{\"name\":", 8 },     { "{\"type\":", 8 },
    { "content-type", 12 },  { "authorization", 13 }, { "bearer ", 7 },        { "user-agent", 10 },
    { "response", 8 },       { "status", 6 },         { "message", 7 },        { "token", 5 },
    { "created_at", 10 },    { "updated_at", 10 },    { "const ", 6 },         { "struct ", 7 },

    /* --- TIER 2: Systems & Logs --- */
    { "localhost", 9 },      { "127.0.0.1", 9 },      { "192.168.", 8 },       { "[INFO] ", 7 },
    { "[WARN] ", 7 },        { "[ERROR] ", 8 },       { "[DEBUG] ", 8 },       { "exception", 9 },
    { "stacktrace", 10 },    { "undefined", 9 },      { "timeout", 7 },        { "connection", 10 },
    { "database", 8 },       { "server", 6 },         { "client", 6 },         { "password", 8 },

    /* --- TIER 3: Binary & Code Artifacts (Moved Numerics Here) --- */
    { "00000000", 8 },       { "ffffff", 6 },         { "0000000000000000", 16 }, { "FFFFFFFFFFFFFFFF", 16 },
    { "\\u00", 4 },          { "0x", 2 },             { "class ", 6 },         { "import ", 7 },
    { "public ", 7 },        { "private ", 8 },       { "void ", 5 },          { "string", 6 },
    { "uint64_t", 8 },       { "uint32_t", 8 },       { "<tbody>", 7 },        { "</span>", 7 }
};

_Static_assert(sizeof(_hn4_lexicon_table) / sizeof(hn4_lexicon_entry_t) == HN4_LEXICON_COUNT, 
               "HN4: Lexicon table size mismatch");

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
     */
    HN4_INLINE int8_t _tcc_detect_linear_gradient(
        const uint8_t* p,
        const uint8_t* end,
        uint32_t device_type
    )
    {
        if (HN4_UNLIKELY(p + 2 > end)) return 0;
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
     * RELATIVE ALIGNMENT CHECK
     * If the offset of dst from a 16-byte boundary differs from src,
     * we cannot align both simultaneously. Fallback to standard copy.
     */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 15) != 0) {
        memcpy(dst, src, len);
        hn4_hal_nvm_persist(dst, len);
        return;
    }

    /* Align destination (and source implicitly via relative check) */
    while (((uintptr_t)d & 15) && len > 0) {
        *d++ = *s++;
        len--;
    }

    /* Stream main body using Non-Temporal Stores */
    while (len >= 16) {
        /* Safe: Relative check + dst alignment loop guarantees s is aligned */
        __m128i val = _mm_load_si128((const __m128i*)s); 
        _mm_stream_si128((__m128i*)d, val);
        s += 16;
        d += 16;
        len -= 16;
    }

    /* 
     * FENCE: Ensure all WC (Write-Combining) buffers are drained 
     * before we issue the persistence barrier on the tail. 
     */
    #if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
    #endif

    /* Handle Tail */
    while (len > 0) {
        *d++ = *s++;
        len--;
    }
    
    /* 
     * Final Persistence Barrier 
     * Note: _mm_stream writes bypass cache, but we still ensure 
     * durability of the unaligned head/tail bytes.
     */
    hn4_hal_nvm_persist(dst, (size_t)(d - (uint8_t*)dst));
}



    /* =========================================================================
    * 2. GRAMMAR & EMISSION LOGIC
    * ========================================================================= */

    /**
     * _tcc_write_token
     * Calculates size and emits token if buffer has space.
     * 
     */
    HN4_INLINE uint8_t* _tcc_write_token(uint8_t* p, const uint8_t* oend, uint8_t tag, uint32_t count) 
    {
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

    
    /*
 * _tcc_attempt_bitmask
 * Scans for sparse data patterns.
 * Returns bytes consumed from src if successful, or 0 if failed/inefficient.
 *
 * Layout on Wire:
 * [Token Header (Len)] [Bitmask (Len/32 bytes)] [Compacted Data...]
 */
static uint32_t _tcc_attempt_bitmask(
    uint8_t** op_ptr, const uint8_t* oend,
    const uint8_t* ip, const uint8_t* iend
) 
{
    if (((uintptr_t)ip & 3) != 0) return 0;

    size_t avail = (size_t)(iend - ip);
    size_t max_scan = avail & ~(HN4_TSM_GRANULARITY - 1);
    
    if (max_scan > HN4_MAX_TOKEN_LEN) {
        max_scan = HN4_MAX_TOKEN_LEN & ~(HN4_TSM_GRANULARITY - 1);
    }
    
    if (max_scan < 32) return 0;

    /* PASS 1: Analysis */
    uint32_t non_zero_words = 0;
    uint32_t total_words = (uint32_t)(max_scan / HN4_TSM_GRANULARITY);
    
    /* 
     * Optimization: Use aligned access. 
     * Caller guarantees ip is 4-byte aligned via `((uintptr_t)ip & 3) == 0`.
     */
    const uint32_t* word_ptr = (const uint32_t*)ip;
    
    for (uint32_t i = 0; i < total_words; i++) {
        if (word_ptr[i] != 0) non_zero_words++;
    }
    
   /* Require at least 12.5% sparsity (7/8 density) */
   if (non_zero_words > (total_words - (total_words >> 3))) return 0;

    /* Calculate Header Size */
    uint32_t header_sz = 1;
    if (max_scan >= HN4_LEN_MASK) {
        uint32_t temp = (uint32_t)max_scan - HN4_LEN_MASK;
        while (temp >= HN4_VARINT_MARKER) {
            header_sz++;
            temp -= HN4_VARINT_MARKER;
        }
        header_sz++; /* Remainder byte */
    }

    uint32_t mask_bytes = (total_words + 7) / 8;
    uint32_t data_bytes = non_zero_words * HN4_TSM_GRANULARITY;
    uint32_t total_out  = header_sz + mask_bytes + data_bytes;
    
    /* Strict savings check: Must save at least 4 bytes to justify opcode switch */
    if (total_out >= max_scan || (max_scan - total_out) < 4) return 0;

    /* Emit */
    uint8_t* op = *op_ptr;
    if ((op + total_out) > oend) return 0;

    op = _tcc_write_token(op, oend, HN4_OP_BITMASK, (uint32_t)max_scan);
    if (!op) return 0;
    
    uint8_t* mask_out = op;
    uint8_t* data_out = op + mask_bytes;
    
    memset(mask_out, 0, mask_bytes);
    
    /* PASS 2: Encoding */
    for (uint32_t i = 0; i < total_words; i++) {
        uint32_t val = word_ptr[i];
        
        if (val != 0) {
            mask_out[i / 8] |= (1 << (i % 8));
            memcpy(data_out, ip + (i * 4), 4);
            data_out += 4;
        }
    }

    *op_ptr = data_out;
    return (uint32_t)max_scan;
}


    /**
     * _flush_literal_buffer
     * Flushes pending literals.
     * Handles NVM Optimization if hw_flags & HN4_HW_NVM is set.
     */
    HN4_INLINE hn4_result_t _flush_literal_buffer(
    uint8_t** op_ptr, const uint8_t* oend, 
    const uint8_t* lit_start, size_t lit_len,
    uint64_t hw_flags
) {
    if (lit_len == 0) return HN4_OK;
    if (lit_len > UINT32_MAX) return HN4_ERR_INVALID_ARGUMENT;

    uint8_t* op = *op_ptr;
    
    /* 
     * SAFETY CONTRACT: 
     * Caller must ensure [lit_start, lit_len] does not overlap with [op, op+lit_len].
     * TCC design separates Source and Dest buffers entirely.
     */
#ifdef HN4_DEBUG
   uintptr_t ls = (uintptr_t)lit_start;
    uintptr_t le = ls + lit_len;
    uintptr_t os = (uintptr_t)op;
    uintptr_t oe = os + lit_len;

    if ((ls < oe) && (os < le)) {
        return HN4_ERR_INTERNAL_FAULT;
    }
#endif

    uint32_t len = (uint32_t)lit_len;

    while (len > 0) {
        uint32_t chunk = (len > HN4_MAX_TOKEN_LEN) ? HN4_MAX_TOKEN_LEN : len;
        
        uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_LITERAL, chunk);
        if (!next_op) return HN4_ERR_ENOSPC;
        
        if ((size_t)(oend - next_op) < chunk) return HN4_ERR_ENOSPC;
        
        op = next_op;
        
        if (HN4_UNLIKELY(hw_flags & HN4_HW_NVM)) {
            _hn4_nvm_stream_copy(op, lit_start, chunk);
        } else {
            memcpy(op, lit_start, chunk);
        }
        
        op += chunk;
        lit_start += chunk;
        len -= chunk;
    }

    *op_ptr = op;
    return HN4_OK;
}

/* Helper: Write Raw VarInt (For Extended Ops) */
HN4_INLINE uint8_t* _tcc_write_varint(uint8_t* p, const uint8_t* oend, uint32_t val) {
    while (val >= HN4_VARINT_MARKER) {
        if (p >= oend) return NULL;
        *p++ = HN4_VARINT_MARKER;
        val -= HN4_VARINT_MARKER;
    }
    if (p >= oend) return NULL;
    *p++ = (uint8_t)val;
    return p;
}

/* LEXICON: Scan Only (Read-Only, O(1)) */
static int _tcc_scan_lexicon(const uint8_t* ip, const uint8_t* iend) {
    size_t avail = (size_t)(iend - ip);

    if (avail < 4) return -1;

    /* Checks first char against common buckets in _hn4_lexicon_table */
    uint8_t c = ip[0];
    switch (c) {
        case '"': case '<': case '{': case '[': /* Structural */
        case 'h': case 'c': case 'u': case 's': /* Common Keywords */
        case 'f': case 't': case 'n': case 'v': /* JSON bools/null */
        case '0': case '\\': case 'F':          /* Numeric/Binary */
        case 'd': case 'r': case 'e': case 'i': /* misc */
        case 'p': case 'a': case 'j': case 'b': /* misc */
        case ' ': case '1': case 'w': case 'm': /* misc */
            break; 
        default: 
            return -1;
    }

    /* Linear scan of HN4_LEXICON_COUNT (64) */
    for (int i = 0; i < HN4_LEXICON_COUNT; i++) {
        size_t len = _hn4_lexicon_table[i].len;
        if (avail >= len) {
            if (memcmp(ip, _hn4_lexicon_table[i].str, len) == 0) return i;
        }
    }
    return -1;
}



/* LEXICON: Emit (Write Only) */
static uint8_t* _tcc_emit_lexicon(uint8_t* op, const uint8_t* oend, int idx) {
    /* Need 3 bytes: ESC + OP + IDX */
    if (op + 3 > oend) return NULL;
    
    *op++ = HN4_EXT_ESCAPE;     /* 0x00 */
    *op++ = HN4_EXT_OP_LEXICON; /* 0x01 */
    *op++ = (uint8_t)idx;       /* Full byte index (0-255) */
    
    return op;
}

/* MANIFOLD: Scan Only (Heuristic check) */
static uint32_t _tcc_scan_manifold(const uint8_t* ip, const uint8_t* iend, uint32_t stride) {
    size_t avail = (size_t)(iend - ip);
    if (avail < stride * 2 || stride == 0) return 0;
    
    uint32_t check = (avail > 64) ? 64 : (uint32_t)avail;
    int score = 0;
    
    for (uint32_t i = stride; i < check; i++) {
        uint8_t pred = (ip[i-1] + ip[i-stride]) >> 1;
        int delta = (int)ip[i] - (int)pred;
        if (delta >= -4 && delta <= 4) score++;
    }
    
    if (score < (int)((check * 3) / 4)) return 0;
    
    /* Calculate actual run length */
    uint32_t len = stride;
    const uint32_t MAX_LOOKAHEAD = 256; 
    uint32_t limit = (avail > MAX_LOOKAHEAD) ? MAX_LOOKAHEAD : (uint32_t)avail;

   for (; len < limit; len++) {
    /* Safety: Ensure we have 4 bytes remaining relative to IP */
    if (len + 4 > (uint32_t)avail) break;

    uint32_t val;
    memcpy(&val, ip + len, 4);
    if (val == 0) break;
}
    
    return len;
}

/* MANIFOLD: Emit (Write Only) */
static uint8_t* _tcc_emit_manifold(uint8_t* op, const uint8_t* oend, 
                                   const uint8_t* ip, uint32_t len, uint32_t stride) 
{
    /* Header: ESC + OP + STRIDE + VARINT_LEN */
    if (op + 3 + HN4_VARINT_MAX_BYTES > oend) return NULL;
    
    *op++ = HN4_EXT_ESCAPE;
    *op++ = HN4_EXT_OP_MANIFOLD; /* No packed index here */
    *op++ = (uint8_t)stride;
    
    op = _tcc_write_varint(op, oend, len);
    if (!op) return NULL;

    if ((size_t)(oend - op) < len) return NULL;

    /* Row 0: Raw Copy */
    memcpy(op, ip, stride);
    op += stride;
    
    /* Row 1..N: Spatial Delta */
    for (size_t i = stride; i < len; i++) {
        uint8_t pred = (ip[i-1] + ip[i-stride]) >> 1;
        *op++ = ip[i] - pred; /* Implicit unsigned wrap is standard */
    }
    
    return op;
}


  _Check_return_
hn4_result_t hn4_compress_block(
    HN4_IN  const void* src_void,
    HN4_IN  uint32_t    src_len,
    HN4_OUT void*       dst_void,
    HN4_IN  uint32_t    dst_capacity,
    HN4_OUT uint32_t*   out_size,
    HN4_IN  uint32_t    device_type,
    HN4_IN  uint64_t    hw_flags
)
{
    if (HN4_UNLIKELY(!src_void || !dst_void || !out_size)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(src_void == dst_void)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(src_len > HN4_BLOCK_LIMIT)) return HN4_ERR_INVALID_ARGUMENT;

    const uint8_t* ip     = (const uint8_t*)src_void;
    const uint8_t* iend   = ip + src_len;
    const uint8_t* anchor = ip; 
    
    /* Safety margin for lookahead */
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

    while (ip <= ilimit) {
        
        /* Auto-flush literal buffer to prevent overflow of token length */
        if ((size_t)(ip - anchor) >= HN4_MAX_TOKEN_LEN) {
            if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                return HN4_ERR_ENOSPC;
            anchor = ip;
        }

        /* --- PRIORITY 1: ISOTOPE (Constant Run) --- */

        if (HN4_UNLIKELY(ip + 8 > iend)) break;

        uint64_t qword = _tcc_load64(ip);
        uint64_t pattern = (uint64_t)ip[0] * 0x0101010101010101ULL;
        
        if (HN4_UNLIKELY(qword == pattern)) {
            const uint8_t* run = ip + 8;
            while (run < iend && *run == ip[0]) run++;
            size_t run_len = (size_t)(run - ip);
            
            if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                return HN4_ERR_ENOSPC;

            while (run_len >= HN4_TENSOR_MIN_SPAN) {
                uint32_t max_enc = HN4_MAX_TOKEN_LEN + HN4_TENSOR_MIN_SPAN;
                uint32_t chunk = (run_len > max_enc) ? max_enc : (uint32_t)run_len;
                /* Safety check: Should be guaranteed by outer while loop, but safe is better */
                if (chunk < HN4_TENSOR_MIN_SPAN) break; 

                uint32_t count = chunk - HN4_TENSOR_MIN_SPAN;
                uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_ISOTOPE, count);
                
                if (!next_op || next_op >= oend) return HN4_ERR_ENOSPC;
                
                op = next_op;
                *op++ = ip[0];
                run_len -= chunk;
                ip += chunk;
            }
            anchor = ip;
            continue;
        }

        /* --- PRIORITY 2: GRADIENT (Linear Progression) --- */
        int8_t slope = _tcc_detect_linear_gradient(ip, iend, device_type);
        if (HN4_UNLIKELY(slope != 0)) {
            const uint8_t* run = ip + 1;
            int16_t expected = (int16_t)ip[0] + slope;

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
                    uint32_t max_enc = HN4_MAX_TOKEN_LEN + HN4_TENSOR_MIN_SPAN;
                    uint32_t chunk = (run_len > max_enc) ? max_enc : (uint32_t)run_len;
                    uint32_t count = chunk - HN4_TENSOR_MIN_SPAN;

                    uint8_t* next_op = _tcc_write_token(op, oend, HN4_OP_GRADIENT, count);
                    if (!next_op || (size_t)(oend - next_op) < 2) return HN4_ERR_ENOSPC;
                    op = next_op;
                    
                    *op++ = ip[0];
                    *op++ = (uint8_t)slope;
                    run_len -= chunk;
                    ip += chunk;
                }
                anchor = ip;
                continue;
            }
        }

        /* --- PRIORITY 3: BITMASK (Sparse Data) --- */
        if (((uintptr_t)ip & 3) == 0) {
            uint32_t w0, w1;
            memcpy(&w0, ip, 4);
            memcpy(&w1, ip + 4, 4);
            if (w0 == 0 || w1 == 0) {
                if (ip > anchor) {
                    if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                        return HN4_ERR_ENOSPC;
                    anchor = ip;
                }
                
                uint32_t consumed = _tcc_attempt_bitmask(&op, oend, ip, iend);
                if (consumed > 0) {
                    ip += consumed;
                    anchor = ip;
                    continue;
                }
            }
        }

        /* --- PRIORITY 4: LEXICON (Extended Dictionary) --- */
        int lex_idx = _tcc_scan_lexicon(ip, iend);
        if (lex_idx >= 0) {
            size_t match_len = _hn4_lexicon_table[lex_idx].len;

            /* 
             * PROFITABILITY CHECK:
             * The token uses 3 bytes (ESC + OP + IDX).
             * We only emit if the matched string is LONGER than 3 bytes.
             * Otherwise, we let it fall through to Literals (no expansion).
             */
            if (match_len > 3) {
                if (ip > anchor) {
                    if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK)
                        return HN4_ERR_ENOSPC;
                    anchor = ip;
                }
                
                op = _tcc_emit_lexicon(op, oend, lex_idx);
                if (!op) return HN4_ERR_ENOSPC;
                
                ip += match_len;
                anchor = ip;
                continue;
            }
        }

        /* --- PRIORITY 5: MANIFOLD (2D Delta) --- */
        /* Check bounds for ip[1] before access to prevent OOB near buffer end */
        if ((device_type == HN4_DEV_SSD) && (ip + 1 < iend) && (ip[0] != 0 && ip[1] != 0)) {
            uint32_t stride = 64; 
            uint32_t m_len = _tcc_scan_manifold(ip, iend, stride);
            
            if (m_len > 0) {
                if (ip > anchor) {
                     if (_flush_literal_buffer(&op, oend, anchor, (size_t)(ip - anchor), hw_flags) != HN4_OK) 
                       return HN4_ERR_ENOSPC;
                     anchor = ip;
                }
                
                op = _tcc_emit_manifold(op, oend, ip, m_len, stride);
                if (!op) return HN4_ERR_ENOSPC;
                
                ip += m_len;
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
    
    uint64_t cost_counter = 0; 
    const uint64_t cost_limit = (uint64_t)src_len * 16 + 1024;

    while (ip < iend) {
        if (++cost_counter > cost_limit) return HN4_ERR_DATA_ROT;

        uint8_t raw_token = *ip++;
        uint8_t tag = raw_token & HN4_OP_MASK;
        uint32_t len = raw_token & HN4_LEN_MASK;

        /* EXTENSION PROTOCOL: LITERAL with LEN=0 acts as ESCAPE */
        if (tag == HN4_OP_LITERAL && len == 0) {
            if (ip >= iend) return HN4_ERR_DATA_ROT;
            
            uint8_t ext_sig = *ip++;
            
            /* -- DECODE LEXICON (0x01) -- */
            if (ext_sig == HN4_EXT_OP_LEXICON) {
                /* Read Index Byte */
                if (ip >= iend) return HN4_ERR_DATA_ROT;
                uint8_t idx = *ip++;

                if (idx >= HN4_LEXICON_COUNT) return HN4_ERR_DATA_ROT;
                
                const char* word = _hn4_lexicon_table[idx].str;
                size_t wlen      = _hn4_lexicon_table[idx].len;
                
                if ((size_t)(oend - op) < wlen) return HN4_ERR_DATA_ROT;
                memcpy(op, word, wlen);
                op += wlen;
                continue;
            }
            
            /* -- DECODE MANIFOLD (0x02) -- */
            if (ext_sig == HN4_EXT_OP_MANIFOLD) {
                if (ip >= iend) return HN4_ERR_DATA_ROT;
                uint32_t stride = *ip++;
                if (stride == 0) return HN4_ERR_DATA_ROT;

                if (stride > (size_t)(op - ostart)) return HN4_ERR_DATA_ROT;

                /* Read VarInt Length */
                uint64_t m_len_64 = 0;
                uint8_t s = 255;
                while (s == 255) {
                    if (ip >= iend) return HN4_ERR_DATA_ROT;
                    s = *ip++;
                    m_len_64 += s;
                    if (m_len_64 > HN4_MAX_TOKEN_LEN) return HN4_ERR_DATA_ROT;
                }
                uint32_t m_len = (uint32_t)m_len_64;
                
                if ((size_t)(oend - op) < m_len) return HN4_ERR_DATA_ROT;
                if ((size_t)(iend - ip) < m_len) return HN4_ERR_DATA_ROT;
                
                /* Stride must fit within the decoded chunk (cannot reference past start of chunk) */
                if (stride > m_len) return HN4_ERR_DATA_ROT;

                /* Row 0: Literal Copy */
                memcpy(op, ip, stride);
                op += stride; ip += stride;
                
                /* Row 1..N: Decode 2D Spatial Delta */
               size_t rem = m_len - stride;
                while (rem--) {
                    if ((size_t)(op - ostart) < stride) return HN4_ERR_DATA_ROT;

                    /* Pred = Avg(Left, Top) */
                    uint8_t pred = (op[-1] + op[-(int)stride]) >> 1;
                    *op++ = *ip++ + pred; 
                }
                continue;
            }
            
            /* Unknown Extension */
            return HN4_ERR_DATA_ROT; 
        }

        /* VARINT DECODING (Standard Tokens) */
        if (len == HN4_LEN_MASK) {
            uint8_t s = HN4_VARINT_MARKER;
            while (s == HN4_VARINT_MARKER) {
                if (HN4_UNLIKELY(ip >= iend)) return HN4_ERR_DATA_ROT;
                s = *ip++;
                if (len > (UINT32_MAX - s)) return HN4_ERR_DATA_ROT;
                len += s;
            }
        }

        if (len > HN4_MAX_TOKEN_LEN) return HN4_ERR_DATA_ROT;
        
        /* Adjust for Tensor Min Span if algorithmic token */
        if (tag == HN4_OP_ISOTOPE || tag == HN4_OP_GRADIENT) {
            if (len > (HN4_BLOCK_LIMIT - HN4_TENSOR_MIN_SPAN)) return HN4_ERR_DATA_ROT;
            len += HN4_TENSOR_MIN_SPAN;
        }

        if (HN4_UNLIKELY(len > (size_t)(oend - op))) return HN4_ERR_DATA_ROT; 

        /* STANDARD OPCODE DISPATCH */
        switch (tag) {
            case HN4_OP_LITERAL:
                if (HN4_UNLIKELY((size_t)(iend - ip) < len)) return HN4_ERR_DATA_ROT;
                memcpy(op, ip, len);
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
                if (len == 0) return HN4_ERR_DATA_ROT;

                int64_t total_delta = (int64_t)(len - 1) * (int64_t)slope;
                int64_t final_val   = (int64_t)val + total_delta;
                if (final_val < 0 || final_val > 255) return HN4_ERR_DATA_ROT;

                int32_t acc = val;
                while (len--) {
                    *op++ = (uint8_t)acc;
                    acc += slope;
                }
                break;
            }

            case HN4_OP_BITMASK: 
            {
                if (HN4_UNLIKELY(len % HN4_TSM_GRANULARITY != 0)) return HN4_ERR_DATA_ROT;
                if (HN4_UNLIKELY(len == 0)) return HN4_ERR_DATA_ROT;
                
                uint32_t total_words = len / HN4_TSM_GRANULARITY;
                uint32_t mask_bytes = (total_words + 7) / 8;
                
                if (HN4_UNLIKELY((size_t)(iend - ip) < mask_bytes)) return HN4_ERR_DATA_ROT;
                const uint8_t* mask_base = ip;
                
                /* Validation: Check unused bits in last mask byte are zero */
                if (total_words % 8 != 0) {
                    uint8_t last_byte = mask_base[mask_bytes - 1];
                    if (last_byte >> (total_words % 8)) return HN4_ERR_DATA_ROT;
                }

                ip += mask_bytes;
                if (op + len > oend) return HN4_ERR_DATA_ROT;

                /* Calculate required input bytes via population count */
                uint32_t set_bits = 0;
                for (uint32_t i = 0; i < mask_bytes; i++) {
                    uint8_t b = mask_base[i];
                    /* 
                     * Correct Popcount: Explicitly mask garbage bits in last byte 
                     * even though we validated them, to ensure calculation consistency.
                     */
                    if (i == mask_bytes - 1 && (total_words % 8) != 0) {
                        b &= (1 << (total_words % 8)) - 1;
                    }
                    /* Kernighan's method */
                    for (; b; set_bits++) b &= b - 1; 
                }
                
                if (HN4_UNLIKELY((size_t)(iend - ip) < (set_bits * 4))) return HN4_ERR_DATA_ROT;

                for (uint32_t i = 0; i < total_words; i++) {
                    if ((mask_base[i / 8] >> (i % 8)) & 1) {
                        memcpy(op, ip, 4);
                        ip += 4;
                    } else {
                        memset(op, 0, 4);
                    }
                    op += 4;
                }
                break;
            }
            default:
                return HN4_ERR_DATA_ROT;
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
     * Safety Formula: isize + (isize >> 6) + 384.
     * Increased overhead allowance (~1.5%) to account for:
     * 1. VarInt headers (up to 33 bytes).
     * 2. Bitmask Token Overhead (Header + Mask Bytes).
     * 3. Alignment padding.
     */
    uint64_t safe_size = (uint64_t)isize;
    safe_size += (safe_size >> 6) + 384;
    
    if (safe_size > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)safe_size;
}

