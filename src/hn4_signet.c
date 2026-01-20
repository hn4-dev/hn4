/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Signet Protocol (Cryptographic Provenance)
 * SOURCE:      hn4_signet.c
 * STATUS:      HARDENED / REVIEWED v3.3
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * MECHANISM:
 * 1. THE SIGIL: A 64-byte Ed25519 signature binding (SeedID + AuthorID + PrevHash).
 * 2. THE ETCH: The Sigil is crypto-mixed into the Anchor's Orbit Vector (V).
 * 3. THE SEAL: An Extension Block containing the full proof chain.
 *
 * SAFETY CONTRACT:
 * - Alignment: Safe memcpy for all U64 access.
 * - Durability: Extension blocks are flushed (FUA) before Anchor memory mutation.
 * - Binding: Payload strictly binds to Anchor Seed ID and Volume UUID.
 * - Integrity: CRC32C covers Header + Payload + Pointer Context.
 * - Causality: Timestamps must be monotonic (New -> Old); chains cannot loop.
 * - Hard Limit: Chain depth capped at 16 (HN4_SIGNET_MAX_DEPTH).
 *
 * CRYPTOGRAPHIC NOTES:
 * - Hash: HN4-SipHash-128 (Non-standard output mix). 
 * - Keying: SipHash keyed with Volume UUID to prevent pre-computation.
 * - CRC: Binds content integrity. prev_seal_hash binds topological order.
 * - Verification: Chain hash verification is the responsibility of the
 *   external auditor/reader. This module enforces append-only structure.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_anchor.h" 
#include "hn4_errors.h"
#include "hn4_addr.h"
#include "hn4_constants.h"
#include <string.h>

/* =========================================================================
 * CONSTANTS & LAYOUT
 * ========================================================================= */

#define HN4_EXT_TYPE_SIGNET     0x99
#define HN4_SIGNET_MAGIC        0x5349474E /* "SIGN" */
#define HN4_SIGNET_VERSION      3          
#define HN4_SIGNET_MAX_DEPTH    16         /* Hard limit: 16 signatures max */

/* 
 * The Watermark Payload 
 * Strictly packed and aligned.
 */
typedef struct HN4_PACKED {
    uint32_t    magic;            /* HN4_SIGNET_MAGIC */
    uint32_t    version;          /* Protocol Version (3) */
    uint64_t    author_id;        /* Author ID */
    uint64_t    timestamp;        /* Creation Time (UTC ns) */
    
    /* CRYPTOGRAPHIC BINDING CONTEXT */
    hn4_u128_t  bound_seed_id;    /* Binds to File Identity */
    hn4_u128_t  volume_uuid;      /* Binds to Storage Container */
    hn4_u128_t  prev_seal_hash;   /* SipHash-128 of previous seal (Topology) */
    uint64_t    self_block_idx;   /* Physical Location Binding */
    
    uint8_t     signature[64];    /* Ed25519 Signature (External Binding) */
    uint8_t     pubkey_fp[32];    /* SipHash Fingerprint of Public Key */
    
    uint32_t    integrity_crc;    /* Structural Integrity (CRC32C) */
    uint8_t     _pad[12];         /* Align structure size */
} hn4_signet_payload_t;

/* Assert against architectural block limit */
#ifndef HN4_MIN_BLOCK_SIZE
#define HN4_MIN_BLOCK_SIZE 512
#endif

_Static_assert(sizeof(hn4_extension_header_t) + sizeof(hn4_signet_payload_t) <= HN4_MIN_BLOCK_SIZE, 
               "Signet Payload exceeds 512B block limit");

/* =========================================================================
 * CRYPTOGRAPHIC PRIMITIVES
 * ========================================================================= */

/*
 * _wyhash_mix
 * A fast, non-cryptographic mixer for vector entropy.
 * Source: wyhash (Wang Yi). Used for 'The Etch'.
 */
static inline uint64_t _wyhash_mix(uint64_t A, uint64_t B)
{
    __uint128_t r = (__uint128_t)A * B;
    return (uint64_t)(r) ^ (uint64_t)(r >> 64);
}

/*
 * _sip_round
 * Standard SipHash ARX round.
 */
static inline void _sip_round(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3)
{
    *v0 += *v1; *v1 = hn4_rotl64(*v1, 13); *v1 ^= *v0; *v0 = hn4_rotl64(*v0, 32);
    *v2 += *v3; *v3 = hn4_rotl64(*v3, 16); *v3 ^= *v2;
    *v0 += *v3; *v3 = hn4_rotl64(*v3, 21); *v3 ^= *v0;
    *v2 += *v1; *v1 = hn4_rotl64(*v1, 17); *v1 ^= *v2; *v2 = hn4_rotl64(*v2, 32);
}

/*
 * _siphash_128
 * Keyed SipHash-2-4 with 128-bit output.
 * NOTE: Output mixing (v1^v3) is HN4-specific variant.
 */
static hn4_u128_t _siphash_128(const uint8_t* in, size_t inlen, const hn4_u128_t* key)
{
    uint64_t k0 = key->lo;
    uint64_t k1 = key->hi;
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    
    /* 128-bit output mode initialization */
    v1 ^= 0xee;

    const uint8_t *end = in + (inlen - (inlen % 8));
    const uint8_t *ptr = in;

    while (ptr < end) {
        uint64_t m;
        memcpy(&m, ptr, 8);
        v3 ^= m;
        _sip_round(&v0, &v1, &v2, &v3);
        _sip_round(&v0, &v1, &v2, &v3);
        v0 ^= m;
        ptr += 8;
    }

    /* Proper Tail Handling (SipHash Spec) via Switch */
    uint64_t b = ((uint64_t)inlen) << 56;
    
    switch (inlen & 7) {
        case 7: b |= ((uint64_t)ptr[6]) << 48; /* Fallthrough */
        case 6: b |= ((uint64_t)ptr[5]) << 40; /* Fallthrough */
        case 5: b |= ((uint64_t)ptr[4]) << 32; /* Fallthrough */
        case 4: b |= ((uint64_t)ptr[3]) << 24; /* Fallthrough */
        case 3: b |= ((uint64_t)ptr[2]) << 16; /* Fallthrough */
        case 2: b |= ((uint64_t)ptr[1]) << 8;  /* Fallthrough */
        case 1: b |= ((uint64_t)ptr[0]);       /* Fallthrough */
        case 0: break;
    }

    v3 ^= b;
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);
    v0 ^= b;
    
    /* Finalization */
    v2 ^= 0xee;
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);

    hn4_u128_t out;
    out.lo = v0 ^ v1 ^ v2 ^ v3;
    out.hi = v1 ^ v3; /* HN4 Variant Mix */
    return out;
}

/* =========================================================================
 * CHAIN VALIDATION & TRAVERSAL
 * ========================================================================= */

/*
 * _validate_chain_and_get_tail
 * 
 * Walks the linked list of Extension Blocks from Head (Newest) to Tail (Oldest).
 * 1. Checks Loop Limit (16).
 * 2. Checks Timestamp Monotonicity (Time must go backwards or stay equal).
 * 3. Verifies UUID and Seed binding.
 * 4. Captures the hash of the *Head* block (to link the new block to).
 */
static hn4_result_t _validate_chain_and_get_tail(
    hn4_volume_t* vol, 
    hn4_anchor_t* anchor,
    uint64_t start_block_idx,
    hn4_u128_t* out_prev_hash
) {
    /* Genesis Hash is 128-bit Zero */
    out_prev_hash->lo = 0;
    out_prev_hash->hi = 0;
    
    if (start_block_idx == 0) return HN4_OK;

    uint32_t depth = 0;
    uint64_t curr_lba = start_block_idx;
    uint64_t last_seen_ts = UINT64_MAX; /* Start with Max Time */
    
    /* State for topological verification (Fix #1) */
    hn4_u128_t prev_hash_from_newer_block = {0, 0};
    bool check_topology = false;
    
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t bs = vol->vol_block_size;
    uint32_t spb = bs / caps->logical_block_size;
    
    void* buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!buf)) return HN4_ERR_NOMEM;

    hn4_result_t res = HN4_OK;

    /* TRAVERSAL LOOP (Newest -> Oldest) */
    while (curr_lba != 0) {
        
        /* 1. Depth & Bounds Check */
        if (depth >= HN4_SIGNET_MAX_DEPTH) {
            res = HN4_ERR_TAMPERED; /* Chain depth exceeded */
            break;
        }

        if (curr_lba >= (vol->vol_capacity_bytes / bs)) {
            res = HN4_ERR_GEOMETRY;
            break;
        }

        /* 2. Read Block */
        hn4_addr_t phys = hn4_lba_from_blocks(curr_lba * spb);
        if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_READ, phys, buf, spb) != HN4_OK)) {
            res = HN4_ERR_HW_IO;
            break;
        }

        hn4_extension_header_t* h = (hn4_extension_header_t*)buf;
        if (hn4_le32_to_cpu(h->magic) != HN4_MAGIC_META) {
            res = HN4_ERR_DATA_ROT;
            break;
        }
        
        uint32_t type = hn4_le32_to_cpu(h->type);
        
        if (type == HN4_EXT_TYPE_SIGNET) {
            hn4_signet_payload_t* p = (hn4_signet_payload_t*)h->payload;
            
            /* 3. Structural Integrity (CRC) */
            uint32_t stored_crc = hn4_le32_to_cpu(p->integrity_crc);
            p->integrity_crc = 0;
            
            /* CRC covers Header + Payload up to CRC field */
            size_t crc_len = sizeof(hn4_extension_header_t) + offsetof(hn4_signet_payload_t, integrity_crc);
            uint32_t calc_crc = hn4_crc32(HN4_CRC_SEED_HEADER, buf, crc_len);
            p->integrity_crc = hn4_cpu_to_le32(stored_crc);

            if (stored_crc != calc_crc) {
                res = HN4_ERR_DATA_ROT;
                break;
            }

            /* 4. Protocol & Binding Checks */
            if (hn4_le32_to_cpu(p->version) > HN4_SIGNET_VERSION) {
                res = HN4_ERR_VERSION_INCOMPAT;
                break;
            }

            if (!hn4_uuid_equal(p->volume_uuid, vol->sb.info.volume_uuid)) {
                res = HN4_ERR_ID_MISMATCH; /* Wrong Volume */
                break;
            }
            if (!hn4_uuid_equal(p->bound_seed_id, anchor->seed_id)) {
                res = HN4_ERR_TAMPERED; /* Replay Attack (Anchor Mismatch) */
                break;
            }

            /* 5. Temporal Causality (Monotonicity) */
            uint64_t curr_ts = hn4_le64_to_cpu(p->timestamp);
            
            /* Allow equal timestamps for batch signing, but never increasing (Old > New) */
            if (curr_ts > last_seen_ts) {
                res = HN4_ERR_TIME_PARADOX;
                break;
            }
            last_seen_ts = curr_ts;

            /* 
             * 6. Topological Verification
             * We traverse Newest -> Oldest. 
             * We must verify that the CURRENT block matches the hash stored 
             * in the PREVIOUS iteration (the newer block).
             */
            size_t hash_len = sizeof(hn4_extension_header_t) + sizeof(hn4_signet_payload_t);
            hn4_u128_t current_blk_hash = _siphash_128((const uint8_t*)buf, hash_len, &vol->sb.info.volume_uuid);

            if (depth == 0) {
                /* The Head of the chain becomes the 'Previous Hash' for the new block being branded */
                *out_prev_hash = current_blk_hash;
            }
            else if (check_topology) {
                /* Verify link: Hash(Current) == NewerBlock.prev_seal_hash */
                if (!hn4_uuid_equal(current_blk_hash, prev_hash_from_newer_block)) {
                    res = HN4_ERR_TAMPERED;
                    break;
                }
            }

            /* 
             * Genesis Constraint: The oldest block must have 0 as previous hash.
             * We check this before updating state for the next loop.
             */
            if (hn4_le64_to_cpu(h->next_ext_lba) == 0) {
                if (p->prev_seal_hash.lo != 0 || p->prev_seal_hash.hi != 0) {
                    res = HN4_ERR_TAMPERED;
                    break;
                }
            }

            /* Update state: Store THIS block's pointer to verify the NEXT iteration (Older block) */
            prev_hash_from_newer_block = hn4_le128_to_cpu(p->prev_seal_hash);
            check_topology = true;
        }

        curr_lba = hn4_le64_to_cpu(h->next_ext_lba);
        depth++;
    }

    hn4_hal_mem_free(buf);
    return res;
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_signet_brand_anchor(
    HN4_IN    hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN    uint64_t      author_id,
    HN4_IN    const uint8_t* signature, /* 64 bytes */
    HN4_IN    uint32_t      sig_len,
    HN4_IN    const uint8_t* public_key /* 32 bytes */
)
{
    /* 1. Validation */
    if (HN4_UNLIKELY(!vol || !anchor || !signature || !public_key)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(sig_len != 64)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(vol->read_only)) return HN4_ERR_ACCESS_DENIED;

    /* Extract existing chain pointer (Head of Linked List) */
    uint64_t old_ext_idx = 0;
    memcpy(&old_ext_idx, anchor->inline_buffer, 8);
    old_ext_idx = hn4_le64_to_cpu(old_ext_idx);

    /* Verify existing chain and get Hash of the current Head */
    hn4_u128_t prev_hash = {0, 0};
    hn4_result_t chain_res = _validate_chain_and_get_tail(vol, anchor, old_ext_idx, &prev_hash);
    
    if (HN4_UNLIKELY(chain_res != HN4_OK)) return chain_res;

    /* 2. Allocation (Horizon) */
    hn4_addr_t ext_phys_lba;
    hn4_result_t alloc_res = hn4_alloc_horizon(vol, &ext_phys_lba);
    if (HN4_UNLIKELY(alloc_res != HN4_OK)) return alloc_res;

    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t spb = bs / ss;
    
    uint64_t new_ext_idx;
    if (HN4_UNLIKELY(!_addr_to_u64_checked(ext_phys_lba, &new_ext_idx))) return HN4_ERR_GEOMETRY;
    new_ext_idx /= spb;

    /* 3. Construct Seal */
    void* ext_buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!ext_buf)) {
        /* Rollback allocation to prevent leak */
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_NOMEM;
    }
    memset(ext_buf, 0, bs);

    hn4_extension_header_t* head = (hn4_extension_header_t*)ext_buf;
    head->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    head->type  = hn4_cpu_to_le32(HN4_EXT_TYPE_SIGNET);
    head->next_ext_lba = hn4_cpu_to_le64(old_ext_idx); /* Point to Previous Head */

    hn4_signet_payload_t* pay = (hn4_signet_payload_t*)head->payload;
    pay->magic     = hn4_cpu_to_le32(HN4_SIGNET_MAGIC);
    pay->version   = hn4_cpu_to_le32(HN4_SIGNET_VERSION);
    pay->author_id = hn4_cpu_to_le64(author_id);
    pay->timestamp = hn4_cpu_to_le64(hn4_hal_get_time_ns()); 
    
    pay->volume_uuid    = vol->sb.info.volume_uuid;
    pay->bound_seed_id  = anchor->seed_id; 
    pay->self_block_idx = hn4_cpu_to_le64(new_ext_idx);
    
    /* Store topology hash in LE format */
    pay->prev_seal_hash = hn4_cpu_to_le128(prev_hash);

    memcpy(pay->signature, signature, sig_len);
    
    /* Expand 32-byte public key fingerprint using two passes */
    hn4_u128_t pk_h1 = _siphash_128(public_key, 32, &vol->sb.info.volume_uuid);
    hn4_u128_t pk_h2 = _siphash_128((uint8_t*)&pk_h1, 16, &vol->sb.info.volume_uuid);
    
    memcpy(pay->pubkey_fp, &pk_h1, 16);
    memcpy(pay->pubkey_fp + 16, &pk_h2, 16);

    /* Checksum (Integrity) */
    pay->integrity_crc = 0;
    size_t crc_len = sizeof(hn4_extension_header_t) + offsetof(hn4_signet_payload_t, integrity_crc);
    uint32_t crc = hn4_crc32(HN4_CRC_SEED_HEADER, ext_buf, crc_len);
    pay->integrity_crc = hn4_cpu_to_le32(crc);

    /* 4. Write Seal (Atomic) */
    if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, ext_phys_lba, ext_buf, spb) != HN4_OK)) {
        hn4_hal_mem_free(ext_buf);
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_HW_IO;
    }

    /* 5. Barrier (Durability) */
    if (HN4_UNLIKELY(hn4_hal_barrier(vol->target_device) != HN4_OK)) {
        hn4_hal_mem_free(ext_buf);
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_HW_IO;
    }
    
    /* 6. Modify Anchor in Memory (The Etch) */
    hn4_anchor_t temp_anchor;
    
    /* Zero-init ensures checksum deterministic behavior across struct padding */
    memset(&temp_anchor, 0, sizeof(hn4_anchor_t));
    memcpy(&temp_anchor, anchor, sizeof(hn4_anchor_t));

    /* Extract V */
    uint64_t current_V = 0;
    const uint8_t* raw_v = temp_anchor.orbit_vector;
    current_V = (uint64_t)raw_v[0] | ((uint64_t)raw_v[1] << 8) |
                ((uint64_t)raw_v[2] << 16) | ((uint64_t)raw_v[3] << 24) |
                ((uint64_t)raw_v[4] << 32) | ((uint64_t)raw_v[5] << 40);

    /* Mix Signature Entropy into V */
    hn4_u128_t sig_hash_128 = _siphash_128(signature, 64, &vol->sb.info.volume_uuid);
    uint64_t sig_hash = sig_hash_128.lo ^ sig_hash_128.hi;
    
    uint64_t mixed_hash = _wyhash_mix(sig_hash, 0xbf58476d1ce4e5b9ULL);
    uint64_t entangled_V = (current_V ^ mixed_hash);
    
    /* Scramble and ensure Odd parity */
    entangled_V = hn4_rotl64(entangled_V, 19); 
    entangled_V |= 1;

    /* Write Back V */
    temp_anchor.orbit_vector[0] = (uint8_t)(entangled_V & 0xFF);
    temp_anchor.orbit_vector[1] = (uint8_t)((entangled_V >> 8) & 0xFF);
    temp_anchor.orbit_vector[2] = (uint8_t)((entangled_V >> 16) & 0xFF);
    temp_anchor.orbit_vector[3] = (uint8_t)((entangled_V >> 24) & 0xFF);
    temp_anchor.orbit_vector[4] = (uint8_t)((entangled_V >> 32) & 0xFF);
    temp_anchor.orbit_vector[5] = (uint8_t)((entangled_V >> 40) & 0xFF);

    /* Point Inline Buffer to New Head */
    uint64_t ext_ptr_le = hn4_cpu_to_le64(new_ext_idx);
    memcpy(temp_anchor.inline_buffer, &ext_ptr_le, 8);

    /* Mark Extended */
    uint64_t dclass = hn4_le64_to_cpu(temp_anchor.data_class);
    dclass |= (1ULL << 63); 
    temp_anchor.data_class = hn4_cpu_to_le64(dclass);

    /* Final Checksum */
    temp_anchor.checksum = 0;
    temp_anchor.checksum = hn4_cpu_to_le32(hn4_crc32(0, &temp_anchor, sizeof(hn4_anchor_t)));

    /* Commit to caller structure */
    memcpy(anchor, &temp_anchor, sizeof(hn4_anchor_t));
    
    hn4_hal_mem_free(ext_buf);
    return HN4_OK;
}