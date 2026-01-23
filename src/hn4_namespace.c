/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Namespace Logic (Resonance Engine)
 * SOURCE:      hn4_namespace.c
 * VERSION:     6.0 (Reference Standard)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ALIGNMENT WITH DOCUMENTATION v6.0:
 * 1. HASHING: Uses "Folded Multiply" with Spec constant (Doc 3.1).
 * 2. PROBING: Linear probe depth increased to 1024 (Doc 3.3).
 * 3. TAGGING: Bloom filter maps 1 tag to 3 bit positions (Doc 5.1).
 * 4. URI:     Supports `id:` and `tag:` selector prefixes (Doc 7).
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_errors.h"
#include "hn4_addr.h"
#include "hn4_annotations.h"
#include <string.h>

/*
 * OPTIMIZATION TABLE 1: Population Count (Hamming Weight)
 * Maps byte (0-255) to number of set bits. Replaces loop/intrinsics on legacy HW.
 */
static const uint8_t _popcount_lut[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

/*
 * OPTIMIZATION TABLE 2: Path Separator Lookup
 * 1 = Separator (/ or : or \0), 0 = Regular char.
 * Removes branching from the hot path parsing loop.
 */
static const uint8_t _sep_lut[256] = {
    ['\0'] = 1, ['/'] = 1, [':'] = 1
    /* All others default to 0 */
};

/* Optimized Popcount using LUT */
static inline int _ns_popcount(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    /* LUT Path: Unrolled for speed */
    const uint8_t *p = (const uint8_t *)&x;
    return _popcount_lut[p[0]] + _popcount_lut[p[1]] +
           _popcount_lut[p[2]] + _popcount_lut[p[3]] +
           _popcount_lut[p[4]] + _popcount_lut[p[5]] +
           _popcount_lut[p[6]] + _popcount_lut[p[7]];
#endif
}

/* =========================================================================
 * 0. CONSTANTS & TUNABLES
 * ========================================================================= */

#define HN4_NS_NAME_MAX 255      /* Max filename length */
#define HN4_NS_MAX_PROBES 1024   /* Spec 3.3: Bounded distance for Linear Probe */
#define HN4_NS_MAX_EXT_DEPTH 16  /* Spec 6.2: Max extension chain depth */

/* Spec 3.1: Hardware-optimized Hash Constant */
#define HN4_NS_HASH_CONST 0xff51afd7ed558ccdULL

/* Extension Types */
#ifndef HN4_EXT_TYPE_TAG
#define HN4_EXT_TYPE_TAG 0x01
#endif

#ifndef HN4_EXT_TYPE_LONGNAME
#define HN4_EXT_TYPE_LONGNAME 0x02
#endif

/* =========================================================================
 * 1. INTERNAL HELPERS: HASHING & VALIDATION
 * ========================================================================= */

/**
 * _ns_hash_uuid
 * Implements Spec 3.1: "Folded Multiply" Hash with Mixer.
 * ID (128-bit) -> [XOR Fold] -> [Mixer] -> [Multiply] -> [Mixer] -> [Slot Index]
 */
static inline uint64_t _ns_hash_uuid(hn4_u128_t id)
{
    /* 1. XOR Fold */
    uint64_t h = id.lo ^ id.hi;

    /* 2. Mixing Pipeline (Spec 3.1) */
    h ^= (h >> 33);
    h *= HN4_NS_HASH_CONST;
    h ^= (h >> 33);

    return h;
}

/**
 * _ns_generate_tag_mask
 * Implements Spec 5.1: "Hash 'Finance' -> 3 bit positions".
 * Maps a string tag to a 64-bit Bloom Filter mask with 3 bits set.
 */
/* Helper for single token hash (Internal) */
static uint64_t _raw_bloom_hash(const char *tag, size_t len)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= (uint8_t)tag[i];
        hash *= 0x100000001B3ULL;
    }
    uint64_t bit1 = (hash) & 63;
    uint64_t bit2 = (hash >> 21) & 63;
    uint64_t bit3 = (hash >> 42) & 63;
    return (1ULL << bit1) | (1ULL << bit2) | (1ULL << bit3);
}

/**
 * _ns_generate_tag_mask
 * UPGRADE: Hierarchical Path Explosion.
 * Input: "photos/2024/vacation"
 * Output: Mask("photos") | Mask("2024") | Mask("vacation")
 */
uint64_t _ns_generate_tag_mask(const char *tag, size_t len)
{
    uint64_t accum_mask = 0;
    const char *ptr = tag;
    const char *end = tag + len;

    /* Current Segment Hash State */
    uint64_t h = 0xCBF29CE484222325ULL;
    bool has_chars = false;

    while (ptr < end)
    {
        uint8_t c = (uint8_t)*ptr;

        /* Check if current char is a separator */
         if (HN4_LIKELY(!_sep_lut[c]))
        {
            h ^= c;
            h *= 0x100000001B3ULL;

            /*
             * OPTIMIZATION: Unroll 3 bytes ahead if safe and no separators.
             * This reduces branch misprediction on long tag segments.
             */
            if (ptr + 3 < end &&
                !_sep_lut[(uint8_t)ptr[1]] &&
                !_sep_lut[(uint8_t)ptr[2]] &&
                !_sep_lut[(uint8_t)ptr[3]])
            {
                h ^= (uint8_t)ptr[1];
                h *= 0x100000001B3ULL;
                h ^= (uint8_t)ptr[2];
                h *= 0x100000001B3ULL;
                h ^= (uint8_t)ptr[3];
                h *= 0x100000001B3ULL;
                ptr += 3;
            }

            has_chars = true;
        }
        else
        {
            /* Separator hit: commit hash if we have chars */
            if (has_chars)
            {
                uint64_t b1 = (h) & 63;
                uint64_t b2 = (h >> 21) & 63;
                uint64_t b3 = (h >> 42) & 63;
                accum_mask |= (1ULL << b1) | (1ULL << b2) | (1ULL << b3);

                /* Reset for next segment */
                h = 0xCBF29CE484222325ULL;
                has_chars = false;
            }
        }
        ptr++;
    }

    /* Commit tail */
    if (has_chars)
    {
        uint64_t b1 = (h) & 63;
        uint64_t b2 = (h >> 21) & 63;
        uint64_t b3 = (h >> 42) & 63;
        accum_mask |= (1ULL << b1) | (1ULL << b2) | (1ULL << b3);
    }

    return accum_mask;
}

/* FNV-1a 64-bit hash for filenames */
static inline uint64_t _ns_fast_name_hash(const char *name)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    while (*name)
    {
        h ^= (uint8_t)*name++;
        h *= 0x100000001B3ULL;
    }
    return h;
}

/**
 * _ns_parse_hex_u64
 * Helper to parse fixed-length hex strings using a lookup table for speed.
 */
static uint64_t _ns_parse_hex_u64(const char *s, size_t len)
{
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = (uint8_t)s[i];
        int val = -1;

        if (c >= '0' && c <= '9')
            val = c - '0';
        else if (c >= 'a' && c <= 'f')
            val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            val = c - 'A' + 10;

        if (val < 0)
            break;
        v = (v << 4) | val;
    }
    return v;
}

/**
 * _ns_verify_extension_ptr
 * Validates extension pointers against volume geometry to prevent OOB access.
 */
static bool _ns_verify_extension_ptr(hn4_volume_t *vol, uint64_t lba)
{
    /* NOTE: Explicitly check against UINT64_MAX boundary as Sentinel */
    if (HN4_UNLIKELY(lba == UINT64_MAX))
        return false;

    const hn4_hal_caps_t *caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;

    if (HN4_UNLIKELY(ss == 0)) return false;
    
    uint32_t spb = bs / ss;

    /* Alignment Check: Extension blocks must align to FS Block Size */
    if (spb > 0 && HN4_UNLIKELY(lba % spb != 0))
        return false;

    hn4_addr_t addr_lba = hn4_addr_from_u64(lba);
    hn4_addr_t flux_start = vol->sb.info.lba_flux_start;

    /* Lower Bound: Must be after Metadata */
#ifdef HN4_USE_128BIT
    if (HN4_UNLIKELY(hn4_u128_cmp(addr_lba, flux_start) < 0))
        return false;
#else
    if (HN4_UNLIKELY(addr_lba < flux_start))
        return false;
#endif

    /* Upper Bound: Capacity */
    uint64_t max_lba;
#ifdef HN4_USE_128BIT
    if (caps->total_capacity_bytes.hi > 0)
    {
        max_lba = UINT64_MAX;
    }
    else
    {
        max_lba = caps->total_capacity_bytes.lo / ss;
    }
#else
    max_lba = caps->total_capacity_bytes / ss;
#endif

    return (lba < max_lba);
}

/* =========================================================================
 * 2. CORTEX LOOKUP (ID -> ANCHOR)
 * ========================================================================= */

/**
 * _ns_scan_cortex_slot
 * Implements Spec 3.3: Lookup Logic.
 * 
 * ENGINEERING NOTES:
 * 1. Generation Awareness: Finds highest 'write_gen' if duplicates/tombstones exist.
 * 2. Load Factor Safety: Stops probing on 'Empty Slot' to prevent O(N) loops.
 */
hn4_result_t _ns_scan_cortex_slot(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  hn4_u128_t target_seed,
    HN4_OUT hn4_anchor_t* out_anchor,
    HN4_OUT uint64_t* out_slot_idx
) 
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    uint64_t start_sect  = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect    = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t total_bytes = (end_sect - start_sect) * ss;
    uint64_t total_slots = total_bytes / sizeof(hn4_anchor_t);

    if (total_slots == 0) return HN4_ERR_GEOMETRY;

    uint64_t hash       = _ns_hash_uuid(target_seed);
    uint64_t start_slot = hash % total_slots;

    /* =========================================================
     * FAST PATH: RAM RESIDENT (NANO-CORTEX)
     * ========================================================= */
    if (vol->nano_cortex) {
        hn4_anchor_t* ram_base = (hn4_anchor_t*)vol->nano_cortex;
        
        bool found = false;
        uint32_t max_gen = 0;
        hn4_anchor_t best_cand;
        uint64_t best_slot = 0;

        uint64_t target_lo_le = hn4_cpu_to_le64(target_seed.lo);
        uint64_t target_hi_le = hn4_cpu_to_le64(target_seed.hi);

        for (uint32_t i = 0; i < HN4_NS_MAX_PROBES; i++) {
            uint64_t curr_slot = (start_slot + i) % total_slots;
            hn4_anchor_t stack_copy;
            bool match = false;
            
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            
            if (ram_base[curr_slot].seed_id.lo == target_lo_le && 
                ram_base[curr_slot].seed_id.hi == target_hi_le) 
            {
                memcpy(&stack_copy, &ram_base[curr_slot], sizeof(hn4_anchor_t));
                match = true;
            }
            else if (HN4_UNLIKELY(ram_base[curr_slot].seed_id.lo == 0 &&
                 ram_base[curr_slot].seed_id.hi == 0 &&
                 ram_base[curr_slot].data_class == 0))
            {
                hn4_hal_spinlock_release(&vol->locking.l2_lock);
                break;
            }
            
            hn4_hal_spinlock_release(&vol->locking.l2_lock);

            if (!match) continue;

            const hn4_anchor_t* raw = &stack_copy;
            uint64_t dclass = hn4_le64_to_cpu(raw->data_class);
            
            if (!(dclass & HN4_FLAG_VALID) && !(dclass & HN4_FLAG_TOMBSTONE)) continue;

            hn4_anchor_t temp;
            memcpy(&temp, raw, sizeof(hn4_anchor_t));
            
            uint32_t stored_crc = hn4_le32_to_cpu(temp.checksum);
            temp.checksum = 0;
            uint32_t calc_crc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));

            if (HN4_LIKELY(stored_crc == calc_crc)) {
                uint32_t curr_gen = hn4_le32_to_cpu(temp.write_gen);
                
                if (!found || (int32_t)(curr_gen - max_gen) > 0) {
                    memcpy(&best_cand, &temp, sizeof(hn4_anchor_t));
                    best_slot = curr_slot;
                    max_gen = curr_gen;
                    found = true;
                }
            }
        }

        if (found) {
            uint64_t best_dc = hn4_le64_to_cpu(best_cand.data_class);
            if (best_dc & HN4_FLAG_TOMBSTONE) return HN4_ERR_TOMBSTONE;
            
            if (out_anchor) memcpy(out_anchor, &best_cand, sizeof(hn4_anchor_t));
            if (out_slot_idx) *out_slot_idx = best_slot;
            return HN4_OK;
        }
        return HN4_ERR_NOT_FOUND;
    }

    /* =========================================================
     * SLOW PATH: DIRECT IO (FALLBACK)
     * ========================================================= */
    uint32_t io_sz = ss * 2;
    void*    buf   = hn4_hal_mem_alloc(io_sz);
    if (!buf) return HN4_ERR_NOMEM;

    bool         found = false;
    uint32_t     max_gen = 0;
    uint64_t     best_slot = 0;
    hn4_anchor_t best_cand;

    uint32_t effective_probes = HN4_NS_MAX_PROBES;
    uint32_t hdd_batch_limit = 0;
    
    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        hdd_batch_limit = io_sz / sizeof(hn4_anchor_t);
        if (hdd_batch_limit < 1) hdd_batch_limit = 1;
        effective_probes = hdd_batch_limit; 
    }

    for (uint32_t i = 0; i < HN4_NS_MAX_PROBES; i++) {
        
        if (hdd_batch_limit > 0 && i >= effective_probes) {
             effective_probes += hdd_batch_limit;
             if (effective_probes > HN4_NS_MAX_PROBES) effective_probes = HN4_NS_MAX_PROBES;
        }
        if (i >= effective_probes) break;

        uint64_t curr_slot = (start_slot + i) % total_slots;
        
        uint64_t byte_offset = curr_slot * sizeof(hn4_anchor_t);
        uint64_t sector_off  = byte_offset / ss;
        uint64_t byte_in_sec = byte_offset % ss;
        
        hn4_addr_t read_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, sector_off);
        uint32_t   read_n   = (byte_in_sec + sizeof(hn4_anchor_t) > ss) ? 2 : 1;

        if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_READ, read_lba, buf, read_n) != HN4_OK)) continue;

        const hn4_anchor_t* raw = (const hn4_anchor_t*)((uint8_t*)buf + byte_in_sec);
        
        if (raw->seed_id.lo == 0 && raw->seed_id.hi == 0 && raw->data_class == 0) {
            break; 
        }
        
        uint64_t dclass = hn4_le64_to_cpu(raw->data_class);
        bool is_valid = (dclass & HN4_FLAG_VALID);
        bool is_tomb  = (dclass & HN4_FLAG_TOMBSTONE);

        if (!is_valid && !is_tomb) continue;

        hn4_u128_t cand_id = hn4_le128_to_cpu(raw->seed_id);
        if (cand_id.lo == target_seed.lo && cand_id.hi == target_seed.hi) {
            hn4_anchor_t temp;
            memcpy(&temp, raw, sizeof(hn4_anchor_t));
            uint32_t stored = hn4_le32_to_cpu(temp.checksum);
            temp.checksum = 0;
            if (stored == hn4_crc32(0, &temp, sizeof(hn4_anchor_t))) {
                uint32_t g = hn4_le32_to_cpu(temp.write_gen);
                if (!found || (int32_t)(g - max_gen) > 0) {
                    memcpy(&best_cand, &temp, sizeof(hn4_anchor_t));
                    best_slot = curr_slot;
                    max_gen = g;
                    found = true;
                }
            }
        }
    }

    hn4_hal_mem_free(buf);

    if (found) {
        if (hn4_le64_to_cpu(best_cand.data_class) & HN4_FLAG_TOMBSTONE) return HN4_ERR_TOMBSTONE;
        if (out_anchor) memcpy(out_anchor, &best_cand, sizeof(hn4_anchor_t));
        if (out_slot_idx) *out_slot_idx = best_slot;
        return HN4_OK;
    }

    return HN4_ERR_NOT_FOUND;
}

/* =========================================================================
 * 3. NAME RESOLUTION (EXTENSION CHAIN)
 * ========================================================================= */

static hn4_result_t _ns_get_or_compare_name(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  hn4_anchor_t* anchor,
    HN4_IN  const char*   compare_target,
    HN4_OUT char*         out_buf,
    HN4_IN  uint32_t      max_len,
    HN4_INOUT char*       scratch_buf
)
{
    if (!scratch_buf) return HN4_ERR_INVALID_ARGUMENT;
    
    char* name_scratch = scratch_buf;
    memset(name_scratch, 0, HN4_NS_NAME_MAX + 1);

    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    size_t current_len = 0;

    if (HN4_LIKELY(!(dclass & HN4_FLAG_EXTENDED))) {
        /* Fast path: Short name in inline buffer */
        const char* src = (const char*)anchor->inline_buffer;
        size_t i = 0;
        for (; i < sizeof(anchor->inline_buffer) && src[i] != '\0'; i++) {
            name_scratch[i] = src[i];
        }
        name_scratch[i] = '\0';
        current_len = i;
    } else {

        uint64_t ext_lba = 0;

        memcpy(&ext_lba, anchor->inline_buffer, 8);
        ext_lba = hn4_le64_to_cpu(ext_lba);

        const char* frag = (const char*)(anchor->inline_buffer + 8);
        size_t i = 0;
        for (; i < 16 && frag[i] != '\0'; i++) {
            name_scratch[i] = frag[i];
        }
        current_len = i;

        if (ext_lba != 0) {
            const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
            uint32_t ss = caps->logical_block_size;
            uint32_t bs = vol->vol_block_size;
            
            void* ext_buf = hn4_hal_mem_alloc(bs);
            
            if (ext_buf) {
                int depth = 0;

                uint32_t spb = (bs / ss) > 0 ? (bs / ss) : 1;
                uint64_t prev_loop_lba = 0; 

                while (depth < HN4_NS_MAX_EXT_DEPTH && _ns_verify_extension_ptr(vol, ext_lba)) {
                    
                    if (ext_lba == prev_loop_lba) {
                        HN4_LOG_WARN("Namespace: Extension loop detected at LBA %llu", (unsigned long long)ext_lba);
                        break;
                    }
                    prev_loop_lba = ext_lba;

                    hn4_addr_t phys = hn4_addr_from_u64(ext_lba);
                    
                    /* NOTE: Read full block to ensure payload coverage */
                    if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, ext_buf, spb) != HN4_OK)) {
                        break;
                    }

                    hn4_extension_header_t* ext = (hn4_extension_header_t*)ext_buf;
                    if (HN4_UNLIKELY(hn4_le32_to_cpu(ext->magic) != HN4_MAGIC_META)) break;
                    
                    uint32_t type = hn4_le32_to_cpu(ext->type);
                    
                    switch (type) {
                        case HN4_EXT_TYPE_LONGNAME:
                        {
                            size_t hdr_sz = sizeof(hn4_extension_header_t);
                            size_t pay_sz = bs - hdr_sz;
                            size_t j = 0;
                            
                            while (j < pay_sz && current_len < HN4_NS_NAME_MAX) {
                                if (ext->payload[j] == '\0') break;
                                name_scratch[current_len++] = ext->payload[j++];
                            }
                            
                            if (j < pay_sz && ext->payload[j] == '\0') {
                                name_scratch[current_len] = '\0';
                                goto name_complete; 
                            }
                            break;
                        }
                        
                        default:
                            break;
                    }
                    
                    ext_lba = hn4_le64_to_cpu(ext->next_ext_lba);
                    depth++;
                }
            name_complete:
                hn4_hal_mem_free(ext_buf);
            }
            name_scratch[current_len] = '\0';
        }
    }

    if (compare_target) {
        return (strcmp(name_scratch, compare_target) == 0) ? HN4_OK : HN4_ERR_NOT_FOUND;
    }

    if (out_buf) {
        strncpy(out_buf, name_scratch, max_len);
        if (max_len > 0) out_buf[max_len - 1] = '\0';
        return HN4_OK;
    }

    return HN4_ERR_INTERNAL_FAULT;
}

/* =========================================================================
 * 3. RESONANCE SCAN (LINEAR METADATA SWEEP)
 * ========================================================================= */

 hn4_result_t _ns_resonance_scan(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  const char*   target_name, /* Can be NULL for pure tag query */
    HN4_IN  uint64_t      required_tags,
    HN4_IN  uint32_t      threshold_pct, /* 100 = Strict, <100 = Fuzzy */
    HN4_OUT hn4_anchor_t* out_anchor
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t total_sectors = end_sect - start_sect;
    uint32_t batch_bytes;
    
    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        batch_bytes = 256 * 1024; 
    } else {
        batch_bytes = 64 * 1024;
    }

    if (batch_bytes % ss != 0) {
        batch_bytes = (batch_bytes / ss + 1) * ss;
    }
    
    void* buf = hn4_hal_mem_alloc(batch_bytes);
    char* name_scratch_heap = hn4_hal_mem_alloc(HN4_NS_NAME_MAX + 1);

    if (!buf || !name_scratch_heap) {
        if (buf) hn4_hal_mem_free(buf);
        if (name_scratch_heap) hn4_hal_mem_free(name_scratch_heap);
        return HN4_ERR_NOMEM;
    }
    
    uint32_t sectors_per_batch = batch_bytes / ss;
    hn4_addr_t current_lba = vol->sb.info.lba_cortex_start;
    uint64_t sectors_left = total_sectors;

    hn4_result_t res = HN4_ERR_NOT_FOUND;
    
    int      best_score = -1;
    uint32_t best_gen   = 0;
    bool     found_candidate = false;

    int query_bits = _ns_popcount(required_tags);
    int min_score  = (query_bits * threshold_pct) / 100;

    while (sectors_left > 0) {
        uint32_t io_sectors = (sectors_left > sectors_per_batch) ? sectors_per_batch : (uint32_t)sectors_left;
        
        if (sectors_left > io_sectors) {
            hn4_addr_t next_lba = hn4_addr_add(current_lba, io_sectors);
            hn4_hal_prefetch(vol->target_device, next_lba, io_sectors);
        }

        if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_READ, current_lba, buf, io_sectors) != HN4_OK)) {
            res = HN4_ERR_HW_IO;
            break;
        }

        uint8_t* ptr = (uint8_t*)buf;
        uint8_t* end = ptr + (io_sectors * ss);
        
        while (ptr + sizeof(hn4_anchor_t) <= end) {
            hn4_anchor_t* cand = (hn4_anchor_t*)ptr;
            ptr += sizeof(hn4_anchor_t);

            uint64_t dclass = hn4_le64_to_cpu(cand->data_class);
            
            bool is_marked = (dclass & (HN4_FLAG_VALID | HN4_FLAG_TOMBSTONE));

            if (cand->seed_id.lo == 0 && cand->seed_id.hi == 0 && !is_marked) continue;

            if (dclass & HN4_FLAG_TOMBSTONE) {
                hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                if (stored == hn4_crc32(0, &temp, sizeof(hn4_anchor_t))) {
                    continue; 
                }

                continue;
            }

            if (!(dclass & HN4_FLAG_VALID)) continue;

            int current_score = 0;
            
            if (required_tags != 0) {
                uint64_t anchor_tags = hn4_le64_to_cpu(cand->tag_filter);
                uint64_t intersection = anchor_tags & required_tags;
                
                current_score = _ns_popcount(intersection);
                
                if (current_score < min_score) continue;
            } else {
                current_score = 0;
            }

            bool name_match = true;
            if (target_name) {
                if (_ns_get_or_compare_name(vol, cand, target_name, NULL, 0, name_scratch_heap) != HN4_OK) {
                    name_match = false;
                }
            }

            if (name_match) {
                 hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                uint32_t calc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));
                
                if (stored == calc) {
                    uint32_t curr_gen = hn4_le32_to_cpu(temp.write_gen);
                    
                    bool is_better = false;
                    
                    if (!found_candidate) {
                        is_better = true;
                    } else if (current_score > best_score) {
                        is_better = true;
                    } else if (current_score == best_score) {
                        if ((int32_t)(curr_gen - best_gen) > 0) {
                            is_better = true;
                        }
                    }

                    if (is_better) {
                        memcpy(out_anchor, &temp, sizeof(hn4_anchor_t));
                        best_score = current_score;
                        best_gen = curr_gen;
                        found_candidate = true;
                        res = HN4_OK;
                    }
                }
            }
        }
        sectors_left -= io_sectors;
        current_lba = hn4_addr_add(current_lba, io_sectors);
    }

    hn4_hal_mem_free(buf);
    hn4_hal_mem_free(name_scratch_heap);
    return res;
}

static uint64_t _ns_parse_time_slice(const char* s) 
{
    bool is_iso = false;
    for (int i = 0; s[i] && i < 11; i++) {
        if (s[i] == '-' || s[i] == ':') { is_iso = true; break; }
    }

    /* Path A: Raw Nanoseconds */
    if (!is_iso) {
        uint64_t val = 0;
        while (*s >= '0' && *s <= '9') {
            val = (val * 10) + (*s - '0');
            s++;
        }
        return val;
    }

    uint64_t y = 0, m = 0, d = 1;
    
    while (*s >= '0' && *s <= '9') {
        y = (y * 10) + (*s++ - '0');
        if (y > 2260) return 0; 
    }
    if (*s == '-') s++;
    
    while (*s >= '0' && *s <= '9') m = (m * 10) + (*s++ - '0');
    if (*s == '-') s++;
    
    if (*s >= '0' && *s <= '9') {
        d = 0;
        while (*s >= '0' && *s <= '9') d = (d * 10) + (*s++ - '0');
    }

    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    static const uint8_t md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint64_t days = 0;

    for (uint64_t i = 1970; i < y; i++) {
        days += 365 + ((i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) ? 1 : 0);
    }
    
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (uint64_t i = 1; i < m; i++) {
        days += md[i-1];
        if (i == 2 && leap) days++;
    }
    days += (d - 1);

    return days * 86400ULL * 1000000000ULL;
}

/* =========================================================================
 * TENSOR RESONANCE (SHARD GATHERING)
 * ========================================================================= */

/**
 * hn4_ns_gather_tensor_shards
 * 
 * Scans the Cortex for all Anchors resonating with the specified Model Tag.
 * Used to mount distributed tensor shards in a single operation.
 * 
 * @param vol           Volume context.
 * @param model_tag     String identifier (e.g., "model:gpt4-70b").
 * @param out_shards    Array to populate with found Anchors.
 * @param max_count     Capacity of out_shards array.
 * @param out_found     Number of shards actually found.
 */
hn4_result_t hn4_ns_gather_tensor_shards(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  const char*   model_tag,
    HN4_OUT hn4_anchor_t* out_shards,
    HN4_IN  uint32_t      max_count,
    HN4_OUT uint32_t*     out_found
)
{
    if (!vol || !model_tag || !out_shards || !out_found) return HN4_ERR_INVALID_ARGUMENT;

    /* 1. Generate Bloom Filter Mask for the Model ID */
    uint64_t required_mask = _ns_generate_tag_mask(model_tag, strlen(model_tag));

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    /* 2. Setup Linear Scan of Cortex */
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t sectors_left = end_sect - start_sect;
    
    hn4_addr_t current_lba = vol->sb.info.lba_cortex_start;
    
    uint32_t batch_bytes;

    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        batch_bytes = 256 * 1024;
    } else {
        batch_bytes = 64 * 1024;
    }

    if (batch_bytes % ss != 0) batch_bytes = (batch_bytes / ss + 1) * ss;
    
    void* buf = hn4_hal_mem_alloc(batch_bytes);

    if (!buf) return HN4_ERR_NOMEM;
    uint32_t sectors_per_batch = batch_bytes / ss;

    uint32_t found_count = 0;

    /* 3. The Resonance Loop */
    while (sectors_left > 0 && found_count < max_count) {
        uint32_t io_sectors = (sectors_left > sectors_per_batch) ? sectors_per_batch : (uint32_t)sectors_left;
        
        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, current_lba, buf, io_sectors) != HN4_OK) {
            goto advance; 
        }

        uint8_t* ptr = (uint8_t*)buf;
        uint8_t* end = ptr + (io_sectors * ss);
        
        while (ptr + sizeof(hn4_anchor_t) <= end && found_count < max_count) {
            hn4_anchor_t* cand = (hn4_anchor_t*)ptr;
            ptr += sizeof(hn4_anchor_t);

            uint64_t dclass = hn4_le64_to_cpu(cand->data_class);
            if (!(dclass & HN4_FLAG_VALID)) continue;
            if (dclass & HN4_FLAG_TOMBSTONE) continue;

            /* B. Resonance Check (Bloom Filter) */
            uint64_t anchor_tags = hn4_le64_to_cpu(cand->tag_filter);
            
            if ((anchor_tags & required_mask) == required_mask) {
                
                hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                 uint32_t calc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));
                
                if (stored == calc) {
                    memcpy(&out_shards[found_count], &temp, sizeof(hn4_anchor_t));
                    found_count++;
                }
            }
        }

    advance:
        sectors_left -= io_sectors;
        current_lba = hn4_addr_add(current_lba, io_sectors);
    }

    hn4_hal_mem_free(buf);
    
    *out_found = found_count;
    
    return (found_count > 0) ? HN4_OK : HN4_ERR_NOT_FOUND;
}

_Check_return_
hn4_result_t hn4_ns_resolve(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  const char*   path,
    HN4_OUT hn4_anchor_t* out_anchor
)
{
    if (!vol || !path || !out_anchor) return HN4_ERR_INVALID_ARGUMENT;

    const char* cursor = path;
    if (*cursor == '/') cursor++; 

    /* 1. Identity Selector (id:...) - Fast Path */
    if (strncmp(cursor, "id:", 3) == 0) {
        cursor += 3;
        if (strlen(cursor) < 32) return HN4_ERR_INVALID_ARGUMENT;
        
        hn4_u128_t target_id;
        target_id.hi = _ns_parse_hex_u64(cursor, 16);
        target_id.lo = _ns_parse_hex_u64(cursor + 16, 16);
        cursor += 32; 
        
        hn4_result_t res = _ns_scan_cortex_slot(vol, target_id, out_anchor, NULL);
        if (res != HN4_OK) return res;
        
        goto apply_slice;
    }

    /* 2. Semantic Parsing (Faceted Tagging & Filenames) */
    char filename[HN4_NS_NAME_MAX + 1];
    filename[0] = '\0';
    uint64_t tag_accum = 0;
    
    char token[HN4_NS_NAME_MAX + 1];
    int  token_len    = 0;
    bool in_tag_group = false;
    
    while (1) {
        char c = *cursor;
        bool is_delim = (c == '/' || c == '+' || c == '#' || c == '\0');
        
        if (is_delim) {
            token[token_len] = '\0';
            
            if (token_len > 0) {
                if (strncmp(token, "tag:", 4) == 0) {
                    in_tag_group = true;
                    /* Facet Logic: "tag:A/B" -> Mask(A) | Mask(B) */
                    tag_accum |= _ns_generate_tag_mask(token + 4, token_len - 4);
                } else if (in_tag_group) {
                    tag_accum |= _ns_generate_tag_mask(token, token_len);
                } else {
                    strncpy(filename, token, HN4_NS_NAME_MAX);
                }
            }
            
            if (c == '/') in_tag_group = false;
            token_len = 0;
            if (c == '#' || c == '\0') break; 
        } else {
            if (token_len < HN4_NS_NAME_MAX) token[token_len++] = c;
        }
        cursor++;
    }

    hn4_result_t res;
    
    /* Case A: Pure Tag Query (Anonymous) */
    if (filename[0] == '\0') {
        if (tag_accum == 0) return HN4_ERR_INVALID_ARGUMENT;
        res = _ns_resonance_scan(vol, NULL, tag_accum, 100, out_anchor);
    } 
    /* Case B: Named Entity (File lookup) */
    else {
        /* If tags are present, they act as filter (100% strict) */
        res = _ns_resonance_scan(vol, filename, tag_accum, 100, out_anchor);
    }

    if (res != HN4_OK) return res;

apply_slice:
    /* 3. Slice Engine (Time/Gen) */
    if (*cursor == '#') {
        cursor++; 
        
        if (strncmp(cursor, "time:", 5) == 0) {
            uint64_t target_ts = _ns_parse_time_slice(cursor + 5);
            if (target_ts == 0) return HN4_ERR_INVALID_ARGUMENT;

            uint32_t create_sec = hn4_le32_to_cpu(out_anchor->create_clock);
            uint64_t create_ns  = (uint64_t)create_sec * 1000000000ULL;
            uint64_t mod_ns     = hn4_le64_to_cpu(out_anchor->mod_clock);

            if (create_ns > target_ts) return HN4_ERR_NOT_FOUND;
            if (mod_ns > target_ts) return HN4_ERR_TIME_PARADOX;
        }
        else if (strncmp(cursor, "gen:", 4) == 0) {
            uint64_t target_gen  = _ns_parse_time_slice(cursor + 4);
            uint32_t current_gen = hn4_le32_to_cpu(out_anchor->write_gen);
            if (target_gen != current_gen) return HN4_ERR_TIME_PARADOX;
        }
    }

    return HN4_OK;
}


hn4_result_t hn4_ns_get_anchor_by_id(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  hn4_u128_t    seed_id,
    HN4_OUT hn4_anchor_t* out_anchor
)
{
    if (!vol || !out_anchor) return HN4_ERR_INVALID_ARGUMENT;
    return _ns_scan_cortex_slot(vol, seed_id, out_anchor, NULL);
}

hn4_result_t hn4_ns_get_name(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  hn4_anchor_t* anchor,
    HN4_OUT char* buf,
    HN4_IN  uint32_t len
)
{
    char* scratch = hn4_hal_mem_alloc(HN4_NS_NAME_MAX + 1);
    if (!scratch) return HN4_ERR_NOMEM;

    hn4_result_t res = _ns_get_or_compare_name(vol, anchor, NULL, buf, len, scratch);

    hn4_hal_mem_free(scratch);
    return res;
}

/**
 * hn4_ns_get_vector_embedding
 * Retrieves the semantic vector for a given file.
 * 
 * @param vol       Volume context
 * @param anchor    Target file anchor
 * @param out_vec   Float buffer for vector
 * @param max_dims  Capacity of float buffer
 * @return Number of dimensions loaded, or 0 if none.
 */
uint32_t hn4_ns_get_vector_embedding(
    HN4_IN hn4_volume_t* vol,
    HN4_IN hn4_anchor_t* anchor,
    HN4_OUT float*       out_vec,
    HN4_IN uint32_t      max_dims
)
{
    /* 1. check for EXTENDED flag */
    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    if (!(dclass & HN4_FLAG_EXTENDED)) return 0;

    /* 2. Get Head of Extension Chain */
    uint64_t ext_lba = 0;
    memcpy(&ext_lba, anchor->inline_buffer, 8);
    ext_lba = hn4_le64_to_cpu(ext_lba);

    /* 3. Traverse Chain */
    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t spb = bs / caps->logical_block_size;
    void* buf = hn4_hal_mem_alloc(bs);
    
    if (!buf) return 0;

    int depth = 0;
    uint32_t found_dims = 0;

    while (depth < HN4_NS_MAX_EXT_DEPTH && _ns_verify_extension_ptr(vol, ext_lba)) {
        
        hn4_addr_t phys = hn4_addr_from_u64(ext_lba);
        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, buf, spb) != HN4_OK) break;

        hn4_extension_header_t* ext = (hn4_extension_header_t*)buf;

        if (hn4_le32_to_cpu(ext->magic) != HN4_MAGIC_META) break;

        if (hn4_le32_to_cpu(ext->type) == HN4_EXT_TYPE_VECTOR) {
            hn4_vector_payload_t* vec = (hn4_vector_payload_t*)ext->payload;
            
            uint32_t dims = hn4_le32_to_cpu(vec->dims);
            uint32_t copy_n = (dims > max_dims) ? max_dims : dims;
            
            memcpy(out_vec, vec->vector, copy_n * sizeof(float));
            found_dims = copy_n;
            break;
        }

        ext_lba = hn4_le64_to_cpu(ext->next_ext_lba);
        depth++;
    }

    hn4_hal_mem_free(buf);
    return found_dims;
}


