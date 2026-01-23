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

/* =========================================================================
 * NLP HELPERS
 * ========================================================================= */

static const char* _hn4_stopwords[] = {
    "a", "an", "and", "are", "as", "at", "be", "by", "for", "from", 
    "has", "he", "in", "is", "it", "its", "of", "on", "that", "the", 
    "to", "was", "were", "will", "with", NULL
};

typedef struct {
    const char* variant;
    const char* root;
} hn4_synonym_t;

/* Static Concept Map (The Thesaurus) */
static const hn4_synonym_t _hn4_synonyms[] = {
    /* Consumption */
    { "ate", "eat" },   { "eaten", "eat" }, { "dining", "eat" }, { "dinner", "eat" }, { "food", "eat" },
    /* Communication */
    { "asked", "say" }, { "said", "say" },  { "told", "say" },   { "chat", "say" },
    /* Visuals */
    { "pic", "img" },   { "photo", "img" }, { "picture", "img" },
    /* Documents */
    { "doc", "text" },  { "pdf", "text" },  { "note", "text" },
    { NULL, NULL }
};

/**
 * _ns_normalize_token
 * Maps input word to its canonical root if a synonym exists.
 * Returns the original string if no match found.
 */
static const char* _ns_normalize_token(const char* token) {
    for (int i = 0; _hn4_synonyms[i].variant; i++) {
        if (strcasecmp(token, _hn4_synonyms[i].variant) == 0) {
            return _hn4_synonyms[i].root;
        }
    }
    return token;
}

static bool _ns_is_stopword(const char* token) {
    /* Simple linear scan for embedded safety (small list) */
    for (int i = 0; _hn4_stopwords[i]; i++) {
        if (strcasecmp(token, _hn4_stopwords[i]) == 0) return true;
    }
    return false;
}

/* Portable Population Count */
static inline int _ns_popcount(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    /* Kernighan's Algorithm */
    int count = 0;
    while (x) { x &= (x - 1); count++; }
    return count;
#endif
}

/* =========================================================================
 * 0. CONSTANTS & TUNABLES
 * ========================================================================= */

#define HN4_NS_NAME_MAX         255     /* Max filename length */
#define HN4_NS_MAX_PROBES       1024    /* Spec 3.3: Bounded distance for Linear Probe */
#define HN4_NS_MAX_EXT_DEPTH    16      /* Spec 6.2: Max extension chain depth */

/* Spec 3.1: Hardware-optimized Hash Constant */
#define HN4_NS_HASH_CONST       0xff51afd7ed558ccdULL

/* Extension Types */
#ifndef HN4_EXT_TYPE_TAG
#define HN4_EXT_TYPE_TAG        0x01
#endif

#ifndef HN4_EXT_TYPE_LONGNAME
#define HN4_EXT_TYPE_LONGNAME   0x02
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
 uint64_t _ns_generate_tag_mask(const char* tag, size_t len) 
{
    /* FNV-1a Base */
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)tag[i];
        hash *= 0x100000001B3ULL;
    }

    /* 
     * Split 64-bit hash into three 6-bit indices to get 3 positions.
     * We mix the hash slightly for each position to ensure distribution.
     */
    uint64_t bit1 = (hash) & 63;
    uint64_t bit2 = (hash >> 21) & 63;
    uint64_t bit3 = (hash >> 42) & 63;

    return (1ULL << bit1) | (1ULL << bit2) | (1ULL << bit3);
}

/**
 * _ns_parse_hex_u64
 * Helper to parse fixed-length hex strings using a lookup table for speed.
 */
static uint64_t _ns_parse_hex_u64(const char* s, size_t len) 
{
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        int val = -1;

        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;

        if (val < 0) break;
        v = (v << 4) | val;
    }
    return v;
}


/**
 * _ns_verify_extension_ptr
 * Validates extension pointers against volume geometry to prevent OOB access.
 */
static bool _ns_verify_extension_ptr(hn4_volume_t* vol, uint64_t lba) 
{
    /* NOTE: Explicitly check against UINT64_MAX boundary as Sentinel */
    if (HN4_UNLIKELY(lba == UINT64_MAX)) return false;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    
    if (ss == 0) return false;
    uint32_t spb = bs / ss;

    /* Alignment Check: Extension blocks must align to FS Block Size */
    if (spb > 0 && HN4_UNLIKELY(lba % spb != 0)) return false;

    hn4_addr_t addr_lba   = hn4_addr_from_u64(lba);
    
    /* 
     * SECURITY: Cortex Isolation.
     * Explicitly reject pointers that land inside the Anchor Table.
     * Overwriting this region would corrupt the filesystem root.
     */
    hn4_addr_t cortex_start = vol->sb.info.lba_cortex_start;
    hn4_addr_t cortex_end   = vol->sb.info.lba_bitmap_start;

#ifdef HN4_USE_128BIT
    /* Check Cortex Range [Start, End) */
    if (hn4_u128_cmp(addr_lba, cortex_start) >= 0 && 
        hn4_u128_cmp(addr_lba, cortex_end) < 0) {
        return false;
    }
    
    /* General Metadata Guard (Must be >= Flux Start) */
    if (hn4_u128_cmp(addr_lba, vol->sb.info.lba_flux_start) < 0) return false;
#else
    /* Check Cortex Range [Start, End) */
    if (addr_lba >= cortex_start && addr_lba < cortex_end) {
        return false;
    }

    /* General Metadata Guard (Must be >= Flux Start) */
    if (addr_lba < vol->sb.info.lba_flux_start) return false;
#endif

    /* Upper Bound: Capacity */
    uint64_t max_lba;
#ifdef HN4_USE_128BIT
    if (caps->total_capacity_bytes.hi > 0) {
        max_lba = UINT64_MAX;
    } else {
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
    
    /* 1. Geometry Calculation */
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

        /* Lookahead: 4 slots (512 bytes) covers ~8 cache lines */
        #define LOOKAHEAD 4

        for (uint32_t i = 0; i < HN4_NS_MAX_PROBES; i++) {
            uint64_t curr_slot = (start_slot + i) % total_slots;
            hn4_anchor_t stack_copy;
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            memcpy(&stack_copy, &ram_base[curr_slot], sizeof(hn4_anchor_t));
            hn4_hal_spinlock_release(&vol->locking.l2_lock);

            /* Use local copy for all checks */
            const hn4_anchor_t* raw = &stack_copy;
            
            uint64_t dclass_le = raw->data_class;
            
            if (HN4_UNLIKELY(raw->seed_id.lo == 0 && raw->seed_id.hi == 0 && dclass_le == 0)) {
                break; 
            }

            uint64_t dclass = hn4_le64_to_cpu(dclass_le);
            if (!(dclass & HN4_FLAG_VALID) && !(dclass & HN4_FLAG_TOMBSTONE)) continue;

           /* Check Identity (Use fast 64-bit int compare) */
            if (raw->seed_id.lo != hn4_cpu_to_le64(target_seed.lo)) continue;
            if (raw->seed_id.hi != hn4_cpu_to_le64(target_seed.hi)) continue;

            /* Candidate Found: Verify Integrity */
            hn4_anchor_t temp;
            memcpy(&temp, raw, sizeof(hn4_anchor_t));
            
            uint32_t stored_crc = hn4_le32_to_cpu(temp.checksum);
            temp.checksum = 0;
            uint32_t calc_crc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));

            if (HN4_LIKELY(stored_crc == calc_crc)) {
                uint32_t curr_gen = hn4_le32_to_cpu(temp.write_gen);
                
                /* 
                 * SECURITY: Serial Number Arithmetic (RFC 1982)
                 * Handles 32-bit wrap-around. (int32_t)(A - B) > 0 implies A > B.
                 */
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

    /* 
     * [HDD OPTIMIZATION] The Mechanical Wall.
     * On rotational media, probing deep causes track thrashing.
     * We limit the probe to what fits in our IO buffer (2 sectors).
     */
    uint32_t effective_probes = HN4_NS_MAX_PROBES;
    uint32_t hdd_batch_limit = 0;
    
    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        hdd_batch_limit = io_sz / sizeof(hn4_anchor_t);
        if (hdd_batch_limit < 1) hdd_batch_limit = 1;
        /* Start with tight limit, extend if needed inside loop */
        effective_probes = hdd_batch_limit; 
    }

    for (uint32_t i = 0; i < HN4_NS_MAX_PROBES; i++) {
        
        /* 
         * STUTTER PROBE LOGIC:
         * If we reached the HDD batch limit (e.g. 2 sectors), we pause.
         * We only extend the probe if the previous slots were COLLISIONS.
         * The 'break' logic below handles the "Empty Wall" case. 
         * If we are here, it means we haven't hit a Wall or a Match yet.
         */
        if (hdd_batch_limit > 0 && i >= effective_probes) {
             effective_probes += hdd_batch_limit;
             if (effective_probes > HN4_NS_MAX_PROBES) effective_probes = HN4_NS_MAX_PROBES;
        }
        
        /* Absolute hard stop if we exceed max probe depth */
        if (i >= effective_probes) break;

        uint64_t curr_slot = (start_slot + i) % total_slots;
        
        uint64_t byte_offset = curr_slot * sizeof(hn4_anchor_t);
        uint64_t sector_off  = byte_offset / ss;
        uint64_t byte_in_sec = byte_offset % ss;
        
        hn4_addr_t read_lba = hn4_addr_add(vol->sb.info.lba_cortex_start, sector_off);
        uint32_t   read_n   = (byte_in_sec + sizeof(hn4_anchor_t) > ss) ? 2 : 1;

        if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_READ, read_lba, buf, read_n) != HN4_OK)) continue;

        const hn4_anchor_t* raw = (const hn4_anchor_t*)((uint8_t*)buf + byte_in_sec);
        
        /* 
         * THE MECHANICAL WALL:
         * If we hit an empty slot, the hash collision chain is broken.
         * We stop probing immediately to save seeks.
         */
        if (raw->seed_id.lo == 0 && raw->seed_id.hi == 0 && raw->data_class == 0) {
            break; 
        }
        
        /* 2. Decode Flags */
        uint64_t dclass = hn4_le64_to_cpu(raw->data_class);
        bool is_valid = (dclass & HN4_FLAG_VALID);
        bool is_tomb  = (dclass & HN4_FLAG_TOMBSTONE);

        /* 3. Validity Check: Must be Valid or Tombstone to be a candidate.
           If neither, it's a corrupted/intermediate slot, so we skip it but continue probing. */
        if (!is_valid && !is_tomb) continue;

        hn4_u128_t cand_id = hn4_le128_to_cpu(raw->seed_id);
        if (cand_id.lo == target_seed.lo && cand_id.hi == target_seed.hi) {
            hn4_anchor_t temp;
            memcpy(&temp, raw, sizeof(hn4_anchor_t));
            uint32_t stored = hn4_le32_to_cpu(temp.checksum);
            temp.checksum = 0;
            if (stored == hn4_crc32(0, &temp, sizeof(hn4_anchor_t))) {
                uint32_t g = hn4_le32_to_cpu(temp.write_gen);
                if (!found || g > max_gen) {
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
    /* RESOURCE SAFETY: Use caller-provided heap buffer instead of stack */
    if (!scratch_buf) return HN4_ERR_INVALID_ARGUMENT;
    
    char* name_scratch = scratch_buf;
    memset(name_scratch, 0, HN4_NS_NAME_MAX + 1);

    uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
    size_t current_len = 0;

    if (!(dclass & HN4_FLAG_EXTENDED)) {
        /* Fast path: Short name in inline buffer */
        const char* src = (const char*)anchor->inline_buffer;
        size_t i = 0;
        for (; i < sizeof(anchor->inline_buffer) && src[i] != '\0'; i++) {
            name_scratch[i] = src[i];
        }
        name_scratch[i] = '\0';
        current_len = i;
    } else {
        /* Extended path: Pointer + Fragment + Extension Chain */
        uint64_t ext_lba = 0;

        /* Extract LBA from first 8 bytes */
        memcpy(&ext_lba, anchor->inline_buffer, 8);
        ext_lba = hn4_le64_to_cpu(ext_lba);

        /* Extract fragment from remaining 16 bytes */
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
            
            /* NOTE: Allocation is aligned to Block Size */
            void* ext_buf = hn4_hal_mem_alloc(bs);
            
            if (ext_buf) {
                int depth = 0;
                /* NOTE: Calculate sectors per block safely */
                uint32_t spb = (bs / ss) > 0 ? (bs / ss) : 1;
                uint64_t prev_loop_lba = 0; /* Loop Detection */

                while (depth < HN4_NS_MAX_EXT_DEPTH && _ns_verify_extension_ptr(vol, ext_lba)) {
                    
                    /* SECURITY: Circular Reference / Self-Loop Check */
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
                            /* NOTE: Payload uses Block Size remainder */
                            size_t pay_sz = bs - hdr_sz;
                            size_t j = 0;
                            
                            while (j < pay_sz && current_len < HN4_NS_NAME_MAX) {
                                if (ext->payload[j] == '\0') break;
                                name_scratch[current_len++] = ext->payload[j++];
                            }
                            
                            if (j < pay_sz && ext->payload[j] == '\0') {
                                name_scratch[current_len] = '\0';
                                goto name_complete; /* Break out of while loop */
                            }
                            break;
                        }
                        
                        /* Future extensions handled here */
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

static hn4_result_t _ns_resonance_scan(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  const char*   target_name, /* Can be NULL for pure tag query */
    HN4_IN  uint64_t      required_tags,
    HN4_IN  uint32_t      threshold_pct, /* 100 = Strict, <100 = Fuzzy */
    HN4_IN  bool          find_latest,   /* True = Sort by Timestamp, False = Sort by Generation */
    HN4_OUT hn4_anchor_t* out_anchor
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t total_sectors = end_sect - start_sect;
    
    /* 
     * Batch configuration
     * HDD: 256KB "Sled" to minimize actuator stops.
     * SSD: 64KB "Pulse" for low latency preemption.
     */
    uint32_t batch_bytes;
    
    if (vol->sb.info.hw_caps_flags & HN4_HW_ROTATIONAL) {
        batch_bytes = 256 * 1024; 
    } else {
        batch_bytes = 64 * 1024;
    }

    if (batch_bytes % ss != 0) {
        batch_bytes = (batch_bytes / ss + 1) * ss;
    }
    
    /* 
     * RESOURCE SAFETY:
     * Allocate IO buffer AND Scratch buffer on heap to prevent stack overflow.
     * name_scratch_heap is passed to _ns_get_or_compare_name.
     */
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
    
    /* IMPLEMENTATION NOTE: Generation Awareness & Resonance Scoring */
    int      best_score = -1;
    uint32_t best_gen   = 0;
    uint64_t best_ts    = 0;
    bool     found_candidate = false;

    /* Calculate target bit count for thresholding */
    int query_bits = _ns_popcount(required_tags);
    int min_score  = (query_bits * threshold_pct) / 100;

    /* Load Optimization Context (Occupancy Bitmap) */
    uint64_t* occ_bitmap = atomic_load_explicit(&vol->locking.cortex_occupancy_bitmap, memory_order_acquire);
    uint64_t  occ_words  = vol->locking.cortex_bitmap_words;
    uint32_t  slots_per_sec = (ss >= sizeof(hn4_anchor_t)) ? (ss / sizeof(hn4_anchor_t)) : 0;

    while (sectors_left > 0) {
        uint32_t io_sectors = (sectors_left > sectors_per_batch) ? sectors_per_batch : (uint32_t)sectors_left;
        
        /* 
         * OPTIMIZATION: Occupancy Check (Skip Empty Regions)
         * If the bitmap indicates this batch of sectors contains no active anchors,
         * skip the IO entirely.
         */
        if (occ_bitmap && slots_per_sec > 0) {
            uint64_t rel_sector = hn4_addr_to_u64(current_lba) - start_sect;
            uint64_t start_slot = rel_sector * slots_per_sec;
            uint64_t batch_len_slots = io_sectors * slots_per_sec;
            
            uint64_t start_word = start_slot / 64;
            uint64_t end_word   = (start_slot + batch_len_slots + 63) / 64;
            
            if (end_word > occ_words) end_word = occ_words;

            bool is_empty = true;
            for (uint64_t w = start_word; w < end_word; w++) {
                if (occ_bitmap[w] != 0) {
                    is_empty = false;
                    break;
                }
            }

            if (is_empty) {
                /* Skip IO, advance cursors */
                sectors_left -= io_sectors;
                current_lba = hn4_addr_add(current_lba, io_sectors);
                continue; 
            }
        }

        /* 
         * [HDD OPTIMIZATION] The Cortex Sled.
         * Trigger asynchronous readahead for the NEXT batch immediately.
         */
        if (sectors_left > io_sectors) {
            hn4_addr_t next_lba = hn4_addr_add(current_lba, io_sectors);
            hn4_hal_prefetch(vol->target_device, next_lba, io_sectors);
        }

        if (hn4_hal_sync_io(vol->target_device, HN4_IO_READ, current_lba, buf, io_sectors) != HN4_OK) {
            res = HN4_ERR_HW_IO;
            break;
        }

        uint8_t* ptr = (uint8_t*)buf;
        uint8_t* end = ptr + (io_sectors * ss);
        
        while (ptr + sizeof(hn4_anchor_t) <= end) {
            hn4_anchor_t* cand = (hn4_anchor_t*)ptr;
            ptr += sizeof(hn4_anchor_t);

            /* 
             * PHANTOM VALIDITY CHECK:
             * Don't trust the flag alone. Ensure ID is not zero.
             */
            uint64_t dclass = hn4_le64_to_cpu(cand->data_class);
            
            bool is_marked = (dclass & (HN4_FLAG_VALID | HN4_FLAG_TOMBSTONE));

            if (cand->seed_id.lo == 0 && cand->seed_id.hi == 0 && !is_marked) continue;

            /* 
             * SECURITY: Tombstone Ghost Check.
             */
            if (dclass & HN4_FLAG_TOMBSTONE) {
                hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                /* If CRC is valid, it really is a Tombstone. Skip it. */
                if (stored == hn4_crc32(0, &temp, sizeof(hn4_anchor_t))) {
                    continue; 
                }
                /* If CRC is invalid, it's garbage. Skip it. */
                continue;
            }

            if (!(dclass & HN4_FLAG_VALID)) continue;

            /* 
             * --- RESONANCE SCORING --- 
             */
            int current_score = 0;
            
            if (required_tags != 0) {
                uint64_t anchor_tags = hn4_le64_to_cpu(cand->tag_filter);
                uint64_t intersection = anchor_tags & required_tags;
                
                /* Calculate Hamming Weight of intersection */
                current_score = _ns_popcount(intersection);
                
                /* Threshold Check */
                if (current_score < min_score) continue;
            } else {
                /* If no tags required (Pure Name Scan), score is 0 but valid */
                current_score = 0;
            }

            /* 
             * Name Check logic:
             * If target_name is present, we enforce strict matching.
             * If target_name is NULL (Pure Tag Query), we implicitly match everything.
             */
            bool name_match = true;
            if (target_name) {
                /* Pass heap scratch buffer */
                if (_ns_get_or_compare_name(vol, cand, target_name, NULL, 0, name_scratch_heap) != HN4_OK) {
                    name_match = false;
                }
            }

            if (name_match) {
                /* Verify CRC */
                 hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                uint32_t calc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));
                
                if (stored == calc) {
                    uint32_t curr_gen = hn4_le32_to_cpu(temp.write_gen);
                    uint64_t curr_ts  = hn4_le64_to_cpu(temp.mod_clock);
                    
                    /* 
                     * ARBITRATION LOGIC:
                     * 1. Higher Resonance Score wins (Better Semantic Match).
                     * 2. If Scores equal, apply Tie-Breaker (Latest vs Version).
                     */
                    bool is_better = false;
                    
                    if (!found_candidate) {
                        is_better = true;
                    } else if (current_score > best_score) {
                        is_better = true;
                    } else if (current_score == best_score) {
                        if (find_latest) {
                            /* Temporal Sort: Pick highest mod_clock */
                            if (curr_ts > best_ts) is_better = true;
                        } else {
                            /* Version Sort: Pick highest generation */
                            /* Serial Number Arithmetic handles 32-bit wrap */
                            if ((int32_t)(curr_gen - best_gen) > 0) {
                                is_better = true;
                            }
                        }
                    }

                    if (is_better) {
                        memcpy(out_anchor, &temp, sizeof(hn4_anchor_t));
                        best_score = current_score;
                        best_gen   = curr_gen;
                        best_ts    = curr_ts;
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
    /* Auto-detect format: ISO contains separators */
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

    /* Path B: ISO-8601 Subset (YYYY-MM[-DD]) */
    uint64_t y = 0, m = 0, d = 1; /* Default to 1st of month */
    
    /* Parse Year */
    while (*s >= '0' && *s <= '9') {
        y = (y * 10) + (*s++ - '0');
        /* 64-bit nanosecond overflow occurs ~2262 AD */
        if (y > 2260) return 0; 
    }
    if (*s == '-') s++;
    
    /* Parse Month */
    while (*s >= '0' && *s <= '9') m = (m * 10) + (*s++ - '0');
    if (*s == '-') s++;
    
    /* Parse Day (Optional) */
    if (*s >= '0' && *s <= '9') {
        d = 0;
        while (*s >= '0' && *s <= '9') d = (d * 10) + (*s++ - '0');
    }

    /* Calendar Validation */
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    /* Gregorian Epoch Calculation */
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

    /* Convert Days to Nanoseconds */
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
    /* Implements Spec 5.1: Maps string to 3 bit positions */
    uint64_t required_mask = _ns_generate_tag_mask(model_tag, strlen(model_tag));

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    
    /* 2. Setup Linear Scan of Cortex */
    uint64_t start_sect = hn4_addr_to_u64(vol->sb.info.lba_cortex_start);
    uint64_t end_sect   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start);
    uint64_t sectors_left = end_sect - start_sect;
    
    hn4_addr_t current_lba = vol->sb.info.lba_cortex_start;
    
    /* 
     * Batch IO setup 
     * Use larger batches for Rotational media to maintain stream velocity.
     */
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
            /* If read fails, skip this batch and continue (Best Effort) */
            goto advance; 
        }

        uint8_t* ptr = (uint8_t*)buf;
        uint8_t* end = ptr + (io_sectors * ss);
        
        while (ptr + sizeof(hn4_anchor_t) <= end && found_count < max_count) {
            hn4_anchor_t* cand = (hn4_anchor_t*)ptr;
            ptr += sizeof(hn4_anchor_t);

            /* A. Validity Check */
            uint64_t dclass = hn4_le64_to_cpu(cand->data_class);
            if (!(dclass & HN4_FLAG_VALID)) continue;
            if (dclass & HN4_FLAG_TOMBSTONE) continue;

            /* B. Resonance Check (Bloom Filter) */
            /* Does this anchor belong to the requested Model ID? */
            uint64_t anchor_tags = hn4_le64_to_cpu(cand->tag_filter);
            
            if ((anchor_tags & required_mask) == required_mask) {
                
                /* C. Integrity Check */
                hn4_anchor_t temp;
                memcpy(&temp, cand, sizeof(hn4_anchor_t));
                uint32_t stored = hn4_le32_to_cpu(temp.checksum);
                temp.checksum = 0;
                
                 uint32_t calc = hn4_crc32(0, &temp, sizeof(hn4_anchor_t));
                
                if (stored == calc) {
                    /* D. Collect Shard */
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
    
    /* Return OK if we found at least one shard, NOT_FOUND otherwise */
    return (found_count > 0) ? HN4_OK : HN4_ERR_NOT_FOUND;
}

hn4_result_t hn4_ns_resolve(
    HN4_IN  hn4_volume_t* vol,
    HN4_IN  const char*   path,
    HN4_OUT hn4_anchor_t* out_anchor
)
{
    if (!vol || !path || !out_anchor) return HN4_ERR_INVALID_ARGUMENT;

    const char* cursor = path;
    if (*cursor == '/') cursor++; 

    /* 
     * 1. Identity Selector (id:...) - Fast Path 
     */
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

    /* 
     * 1.5. NATURAL LANGUAGE QUERY (Semantic Search)
     * Heuristic: Spaces imply natural language sentences rather than strict paths.
     * Example: "what did I ate last night" -> Tags: [eat, night] + Temporal Mode
     */
    if (strchr(cursor, ' ') != NULL) {
        uint64_t sentence_tags = 0;
        char temp_buf[HN4_NS_NAME_MAX + 1];
        
        /* Safe copy to tokenize (strtok modifies string) */
        strncpy(temp_buf, cursor, HN4_NS_NAME_MAX);
        temp_buf[HN4_NS_NAME_MAX] = '\0';
        
        /* Tokenize by common sentence delimiters */
        char* token = strtok(temp_buf, " ?.,!"); 
        int significant_words = 0;
        
        /* Temporal keywords trigger 'find_latest' mode */
        const char* temp_keywords[] = {"last", "latest", "newest", "recent", NULL};
        bool find_latest = false;

        while (token != NULL) {
            bool is_temporal = false;
            
            /* Check for Temporal Triggers */
            for (int t=0; temp_keywords[t]; t++) {
                if (strcasecmp(token, temp_keywords[t]) == 0) {
                    find_latest = true;
                    is_temporal = true;
                    break;
                }
            }

            /* STOP-WORD FILTERING & Short word rejection */
            if (!is_temporal && !_ns_is_stopword(token) && strlen(token) > 1) {
                
                /* SYNONYM COLLAPSE: "ate" -> "eat" */
                const char* root_word = _ns_normalize_token(token);
                
                sentence_tags |= _ns_generate_tag_mask(root_word, strlen(root_word));
                significant_words++;
            }
            token = strtok(NULL, " ?.,!");
        }

        if (significant_words == 0) return HN4_ERR_INVALID_ARGUMENT;

        /* 
         * FUZZY RESONANCE SCAN
         * Threshold: 50%
         * Allows partial matching (OR-like behavior) for semantic queries.
         * find_latest: If true, arbitration prefers Timestamp over Generation.
         */
        hn4_result_t res = _ns_resonance_scan(vol, NULL, sentence_tags, 50, find_latest, out_anchor);
        
        if (res == HN4_OK) {
            /* Fast-forward cursor to hash or end for slicing logic */
            const char* slice = strchr(path, '#');
            if (slice) cursor = slice; 
            else cursor += strlen(cursor); // End
            
            goto apply_slice;
        }
        return res;
    }

    /* 
     * 2. Standard Semantic Parsing (State Machine)
     */
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

    /* 3. Execute Resolution */
    hn4_result_t res;
    if (filename[0] == '\0') {
        /* Pure Tag Query (Anonymous): Strict Match (100%), Default Sort */
        if (tag_accum == 0) return HN4_ERR_INVALID_ARGUMENT;
        res = _ns_resonance_scan(vol, NULL, tag_accum, 100, false, out_anchor);
    } else {
        /* Named Entity: Strict Match (100%), Default Sort */
        res = _ns_resonance_scan(vol, filename, tag_accum, 100, false, out_anchor);
    }

    if (res != HN4_OK) return res;

apply_slice:
    /* 
     * 4. Slice Engine (Spec 7.4)
     * Handles temporal projection via #time or #gen suffix.
     */
    if (*cursor == '#') {
        cursor++; 
        
        /* Slice: time:<timestamp> */
        if (strncmp(cursor, "time:", 5) == 0) {
            uint64_t target_ts = _ns_parse_time_slice(cursor + 5);
            if (target_ts == 0) return HN4_ERR_INVALID_ARGUMENT;

            uint32_t create_sec = hn4_le32_to_cpu(out_anchor->create_clock);
            uint64_t create_ns  = (uint64_t)create_sec * 1000000000ULL;
            uint64_t mod_ns     = hn4_le64_to_cpu(out_anchor->mod_clock);

            /* Check Bounds: Did it exist? */
            if (create_ns > target_ts) {
                return HN4_ERR_NOT_FOUND;
            }

            /* Check Bounds: Has it mutated? */
            if (mod_ns > target_ts) {
                /* 
                 * The object exists but the version on disk is newer than requested.
                 * Namespace resolves Identity, not History. Use Snapshot mount.
                 */
                return HN4_ERR_TIME_PARADOX;
            }
        }
        
        /* Slice: gen:<id> */
        else if (strncmp(cursor, "gen:", 4) == 0) {
            uint64_t target_gen  = _ns_parse_time_slice(cursor + 4);
            uint32_t current_gen = hn4_le32_to_cpu(out_anchor->write_gen);
            
            if (target_gen != current_gen) {
                return HN4_ERR_TIME_PARADOX;
            }
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
    /* Allocate the scratch buffer required by the refactored helper */
    char* scratch = hn4_hal_mem_alloc(HN4_NS_NAME_MAX + 1);
    if (!scratch) return HN4_ERR_NOMEM;

    /* Call helper with the new signature */
    hn4_result_t res = _ns_get_or_compare_name(vol, anchor, NULL, buf, len, scratch);

    hn4_hal_mem_free(scratch);
    return res;
}