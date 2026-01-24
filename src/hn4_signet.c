/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Signet Protocol (Cryptographic Provenance)
 * SOURCE:      hn4_signet.c
 * STATUS:      HARDENED / REVIEWED v3.3
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Implements the "Signet" watermarking system. This protocol allows
 *   sovereign keys to cryptographically sign file Anchors without
 *   modifying the underlying data blocks.
 *
 *   The mechanism uses a "Shadow Chain" of extension blocks (Seals)
 *   linked to the Anchor via an entropic mix into the Orbit Vector (The Etch).
 *
 * SAFETY CONTRACT:
 *   1. ALIGNMENT:  All 64-bit accesses are safe (memcpy used where packing varies).
 *   2. DURABILITY: Extension blocks are flushed (FUA) before Anchor mutation.
 *   3. BINDING:    Signatures are bound to (SeedID + VolumeUUID + Topology).
 *   4. LIMITS:     Chain depth capped at 16 to prevent infinite loops.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_anchor.h" 
#include "hn4_errors.h"
#include "hn4_swizzle.h"
#include "hn4_addr.h"
#include "hn4_constants.h"
#include <string.h>

/* =========================================================================
 * 0. CONSTANTS & DATA LAYOUT
 * ========================================================================= */

/* 
 * The Watermark Payload.
 * This structure resides inside the payload[] area of a generic Extension Block.
 * It is strictly packed to ensure consistent hashing across architectures.
 */
typedef struct HN4_PACKED {
    uint32_t    magic;            /* HN4_SIGNET_MAGIC */
    uint32_t    version;          /* Protocol Version (3) */
    uint64_t    author_id;        /* The ID of the signing key/user */
    uint64_t    timestamp;        /* Creation Time (UTC nanoseconds) */
    
    /* --- CRYPTOGRAPHIC BINDING CONTEXT --- */
    /* These fields bind the signature to specific metadata to prevent replays */
    hn4_u128_t  bound_seed_id;    /* Binds to File Identity (Seed) */
    hn4_u128_t  volume_uuid;      /* Binds to Storage Container */
    hn4_u128_t  prev_seal_hash;   /* SipHash-128 of the previous seal (Topology) */
    uint64_t    self_block_idx;   /* Binds to physical location (Physical Anti-Spoof) */
    
    /* --- PROOF --- */
    uint8_t     signature[64];    /* Ed25519 Signature */
    uint8_t     pubkey_fp[32];    /* SipHash Fingerprint of Public Key */
    
    /* --- INTEGRITY --- */
    uint32_t    integrity_crc;    /* Structural Integrity (CRC32C) */
    uint8_t     _pad[12];         /* Padding to align struct size if needed */
} hn4_signet_payload_t;

/* 
 * Architectural Assertion:
 * Ensure the payload fits within the smallest atomic block size (512 bytes).
 */
#ifndef HN4_MIN_BLOCK_SIZE
#define HN4_MIN_BLOCK_SIZE 512
#endif

_Static_assert(sizeof(hn4_extension_header_t) + sizeof(hn4_signet_payload_t) <= HN4_MIN_BLOCK_SIZE, 
               "HN4: Signet Payload exceeds 512B atomic block limit");

/* =========================================================================
 * 1. CRYPTOGRAPHIC PRIMITIVES
 * ========================================================================= */

/**
 * _wyhash_mix
 * A fast, non-cryptographic mixer for vector entropy.
 * Sourced from wyhash (Wang Yi). Used for 'The Etch' (modifying V).
 */
HN4_INLINE uint64_t _wyhash_mix(uint64_t A, uint64_t B)
{
    __uint128_t r = (__uint128_t)A * B;
    return (uint64_t)(r) ^ (uint64_t)(r >> 64);
}

/**
 * _sip_round
 * Standard SipHash ARX (Add-Rotate-Xor) round.
 */
HN4_INLINE void _sip_round(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3)
{
    *v0 += *v1; *v1 = hn4_rotl64(*v1, 13); *v1 ^= *v0; *v0 = hn4_rotl64(*v0, 32);
    *v2 += *v3; *v3 = hn4_rotl64(*v3, 16); *v3 ^= *v2;
    *v0 += *v3; *v3 = hn4_rotl64(*v3, 21); *v3 ^= *v0;
    *v2 += *v1; *v1 = hn4_rotl64(*v1, 17); *v1 ^= *v2; *v2 = hn4_rotl64(*v2, 32);
}

/**
 * _siphash_128
 * Keyed SipHash-2-4 with 128-bit output.
 * 
 * HN4 VARIANT NOTE:
 * The output mixing (v1^v3 in high qword) is specific to HN4 to maximize
 * entropy spread when mapping to 128-bit UUID fields.
 */
static hn4_u128_t _siphash_128(const uint8_t* in, size_t inlen, const hn4_u128_t* key)
{
    /* Initialize State */
    uint64_t k0 = key->lo;
    uint64_t k1 = key->hi;
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    
    /* 128-bit output mode initialization magic */
    v1 ^= 0xee;

    const uint8_t *end = in + (inlen - (inlen % 8));
    const uint8_t *ptr = in;

    /* Compression Loop */
    while (ptr < end) {
        uint64_t m;
        memcpy(&m, ptr, 8); /* Safe unaligned load */
        v3 ^= m;
        _sip_round(&v0, &v1, &v2, &v3);
        _sip_round(&v0, &v1, &v2, &v3);
        v0 ^= m;
        ptr += 8;
    }

    /* Tail Handling (Switch for branch prediction optimization) */
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
    
    /* Finalization (4 rounds for standard SipHash-2-4) */
    v2 ^= 0xee;
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);
    _sip_round(&v0, &v1, &v2, &v3);

    hn4_u128_t out;
    out.lo = v0 ^ v1 ^ v2 ^ v3;
    out.hi = v1 ^ v3; /* HN4 specific mix */
    return out;
}

/* =========================================================================
 * 2. CHAIN VALIDATION & TRAVERSAL
 * ========================================================================= */

/**
 * _validate_chain_and_get_tail
 * 
 * Walks the linked list of Extension Blocks from Head (Newest) to Tail (Oldest).
 * 
 * VALIDATION STEPS:
 * 1. Loop Limit: Caps depth at HN4_SIGNET_MAX_DEPTH to prevent DOS.
 * 2. Monotonicity: Ensures timestamps only decrease (going back in time).
 * 3. Binding: Verifies Seals belong to THIS Anchor and THIS Volume.
 * 4. Topology: Verifies the hash chain (Previous Seal Hash).
 * 
 * RETURNS:
 * - HN4_OK: Chain is valid.
 * - out_prev_hash: The SipHash-128 of the current HEAD (to be used by new seal).
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
    
    /* State for topological verification */
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

        /* Check against physical capacity to prevent OOB read */
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
        
        /* 
         * 3. Calculate Hash of CURRENT block 
         * Used for Topological Verification by the NEWER block.
         * We hash the full block size to capture all opaque data.
         */
        hn4_u128_t current_blk_hash = _siphash_128((const uint8_t*)buf, bs, &vol->sb.info.volume_uuid);

        if (depth == 0) {
            /* The Head of the chain becomes the 'Previous Hash' for the NEW seal we are about to write */
            *out_prev_hash = current_blk_hash;
        }
        else if (check_topology) {
            /* Verify that the older block's hash matches what the newer block claimed */
            if (!hn4_uuid_equal(current_blk_hash, prev_hash_from_newer_block)) {
                res = HN4_ERR_TAMPERED;
                break;
            }
        }
        
        uint32_t type = hn4_le32_to_cpu(h->type);
        
        if (type == HN4_EXT_TYPE_SIGNET) {
            hn4_signet_payload_t* p = (hn4_signet_payload_t*)h->payload;
            
            /* 4. Structural Integrity (CRC) */
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

            /* 5. Protocol & Binding Checks */
            if (hn4_le32_to_cpu(p->version) > HN4_SIGNET_VERSION) {
                res = HN4_ERR_VERSION_INCOMPAT;
                break;
            }

            /* Binds to Volume */
            if (!hn4_uuid_equal(p->volume_uuid, vol->sb.info.volume_uuid)) {
                res = HN4_ERR_ID_MISMATCH; 
                break;
            }
            /* Binds to Anchor Identity */
            if (!hn4_uuid_equal(p->bound_seed_id, anchor->seed_id)) {
                res = HN4_ERR_TAMPERED; 
                break;
            }

            /* 6. Temporal Causality (Monotonicity) */
            uint64_t curr_ts = hn4_le64_to_cpu(p->timestamp);
            
            /* Allow equal timestamps for batch signing, but never increasing (Old > New is impossible) */
            if (curr_ts > last_seen_ts) {
                res = HN4_ERR_TIME_PARADOX;
                break;
            }
            last_seen_ts = curr_ts;

            /* 
             * 7. Topology Prep for Next Iteration
             * Extract the 'prev_seal_hash' that THIS block claims the OLDER block has.
             */
            prev_hash_from_newer_block = hn4_le128_to_cpu(p->prev_seal_hash);
            check_topology = true;

            /* 
             * Genesis Constraint: 
             * If this is the tail (next = 0), it must point to Null Hash.
             */
            if (hn4_le64_to_cpu(h->next_ext_lba) == 0) {
                if (p->prev_seal_hash.lo != 0 || p->prev_seal_hash.hi != 0) {
                    res = HN4_ERR_TAMPERED;
                    break;
                }
            }
        } else {
            /* 
             * Non-Signet Block (e.g. LONGNAME).
             * These blocks do not carry a 'prev_seal_hash', so they interrupt the 
             * cryptographic verification chain.
             */
            check_topology = false;
        }

        /* Next Link */
        curr_lba = hn4_le64_to_cpu(h->next_ext_lba);
        depth++;
    }

    hn4_hal_mem_free(buf);
    return res;
}

/* =========================================================================
 * 3. PUBLIC API: BRANDING
 * ========================================================================= */

_Check_return_
hn4_result_t hn4_signet_brand_anchor(
    HN4_IN    hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN    uint64_t      author_id,
    HN4_IN    const uint8_t* signature, /* Must be 64 bytes */
    HN4_IN    uint32_t      sig_len,
    HN4_IN    const uint8_t* public_key /* Must be 32 bytes */
)
{
    /* 1. Validation */
    if (HN4_UNLIKELY(!vol || !anchor || !signature || !public_key)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(sig_len != 64)) return HN4_ERR_INVALID_ARGUMENT;
    if (HN4_UNLIKELY(vol->read_only)) return HN4_ERR_ACCESS_DENIED;

    /* --- GEOMETRY SETUP (Moved Up) --- */
    uint32_t bs = vol->vol_block_size;
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    uint32_t ss = caps->logical_block_size;
    uint32_t spb = (bs / ss) > 0 ? (bs / ss) : 1;

    /* Extract existing chain pointer (Head of Linked List) */
    uint64_t old_ext_idx = 0;
    uint64_t current_dclass = hn4_le64_to_cpu(anchor->data_class);
    
    if (current_dclass & HN4_FLAG_EXTENDED) {
        memcpy(&old_ext_idx, anchor->inline_buffer, 8);
        old_ext_idx = hn4_le64_to_cpu(old_ext_idx);
    } else {
        /* 
         * MIGRATION LOGIC:
         * Anchor is currently Inline. Check if it holds data (Name).
         * If yes, move it to a new Extension Block so we can start a chain.
         */
        bool has_data = false;
        for(size_t i=0; i<sizeof(anchor->inline_buffer); i++) {
            if (anchor->inline_buffer[i] != 0) { has_data = true; break; }
        }

        if (has_data) {
            /* 1. Allocate Horizon Block for the Name */
            hn4_addr_t name_phys_lba;
            hn4_result_t alloc_res = hn4_alloc_horizon(vol, &name_phys_lba);
            if (alloc_res != HN4_OK) return alloc_res;

            /* 2. Prepare Extension Block */
            void* name_buf = hn4_hal_mem_alloc(bs);
            if (!name_buf) {
                /* Rollback alloc */
                hn4_free_block(vol, hn4_addr_to_u64(name_phys_lba));
                return HN4_ERR_NOMEM;
            }
            memset(name_buf, 0, bs);

            hn4_extension_header_t* head = (hn4_extension_header_t*)name_buf;
            head->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
            head->type  = hn4_cpu_to_le32(HN4_EXT_TYPE_LONGNAME); /* 0x02 */
            head->next_ext_lba = 0; /* Tail of chain */

            /* Copy inline data to payload */
            /* Payload size check: bs - header (16) > 24. Safe for 512B+ blocks. */
            memcpy(head->payload, anchor->inline_buffer, sizeof(anchor->inline_buffer));

            /* Integrity */
            /* Note: Extension blocks don't have a standard CRC field in the header struct 
               defined in hn4.h, they rely on the payload structure. 
               For raw LONGNAME, we don't have a specific CRC field defined in the 
               generic header, but Signet validates its own blocks. 
               Simple migration is trusted via Barrier. */

            /* 3. Write to Disk */
            hn4_result_t io_res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, name_phys_lba, name_buf, spb);
            
            hn4_hal_mem_free(name_buf);

            if (io_res != HN4_OK) {
                hn4_free_block(vol, hn4_addr_to_u64(name_phys_lba));
                return io_res;
            }
            
            /* Barrier ensuring migration persistence */
            hn4_hal_barrier(vol->target_device);

            /* 4. Convert Physical LBA to Block Index */
            uint64_t name_idx;
            if (!_addr_to_u64_checked(name_phys_lba, &name_idx)) return HN4_ERR_GEOMETRY;
            name_idx /= spb;

            /* 5. Set this new block as the "Old Head" for the Signet to link to */
            old_ext_idx = name_idx;
        }
    }

    /* Verify existing chain logic and get Hash of the current Head */
    hn4_u128_t prev_hash = {0, 0};
    hn4_result_t chain_res = _validate_chain_and_get_tail(vol, anchor, old_ext_idx, &prev_hash);
    
    if (HN4_UNLIKELY(chain_res != HN4_OK)) return chain_res;

    /* 2. Allocation (D1.5 Horizon) for SIGNET */
    hn4_addr_t ext_phys_lba;
    hn4_result_t alloc_res = hn4_alloc_horizon(vol, &ext_phys_lba);
    if (HN4_UNLIKELY(alloc_res != HN4_OK)) return alloc_res;

    /* ... (Rest of function remains unchanged) ... */
    
    uint64_t new_ext_idx;
    if (HN4_UNLIKELY(!_addr_to_u64_checked(ext_phys_lba, &new_ext_idx))) return HN4_ERR_GEOMETRY;
    new_ext_idx /= spb;

    /* 3. Construct Seal (Extension Block) */
    void* ext_buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!ext_buf)) {
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_NOMEM;
    }
    memset(ext_buf, 0, bs);

    /* 3a. Generic Extension Header */
    hn4_extension_header_t* head = (hn4_extension_header_t*)ext_buf;
    head->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    head->type  = hn4_cpu_to_le32(HN4_EXT_TYPE_SIGNET);
    head->next_ext_lba = hn4_cpu_to_le64(old_ext_idx); /* Point to Previous Head */

    /* 3b. Signet Payload */
    hn4_signet_payload_t* pay = (hn4_signet_payload_t*)head->payload;
    pay->magic     = hn4_cpu_to_le32(HN4_SIGNET_MAGIC);
    pay->version   = hn4_cpu_to_le32(HN4_SIGNET_VERSION);
    pay->author_id = hn4_cpu_to_le64(author_id);
    pay->timestamp = hn4_cpu_to_le64(hn4_hal_get_time_ns()); 
    
    /* Binding Context */
    pay->volume_uuid    = vol->sb.info.volume_uuid;
    pay->bound_seed_id  = anchor->seed_id; 
    pay->self_block_idx = hn4_cpu_to_le64(new_ext_idx);
    
    /* Topological Link */
    pay->prev_seal_hash = hn4_cpu_to_le128(prev_hash);

    /* Cryptographic Proof */
    memcpy(pay->signature, signature, sig_len);
    
    /* Expand 32-byte public key fingerprint using two passes of SipHash-128 */
    /* This avoids storing the full key but allows verification lookup */
    hn4_u128_t pk_h1 = _siphash_128(public_key, 32, &vol->sb.info.volume_uuid);
    hn4_u128_t pk_h2 = _siphash_128((uint8_t*)&pk_h1, 16, &vol->sb.info.volume_uuid);
    
    memcpy(pay->pubkey_fp, &pk_h1, 16);
    memcpy(pay->pubkey_fp + 16, &pk_h2, 16);

    /* Checksum (Integrity) */
    pay->integrity_crc = 0;
    size_t crc_len = sizeof(hn4_extension_header_t) + offsetof(hn4_signet_payload_t, integrity_crc);
    uint32_t crc = hn4_crc32(HN4_CRC_SEED_HEADER, ext_buf, crc_len);
    pay->integrity_crc = hn4_cpu_to_le32(crc);

    /* 4. Write Seal (Atomic I/O) */
    if (HN4_UNLIKELY(hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, ext_phys_lba, ext_buf, spb) != HN4_OK)) {
        hn4_hal_mem_free(ext_buf);
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_HW_IO;
    }

    /* 5. Barrier (Durability) */
    /* Must ensure the Seal is on media before we point the Anchor to it */
    if (HN4_UNLIKELY(hn4_hal_barrier(vol->target_device) != HN4_OK)) {
        hn4_hal_mem_free(ext_buf);
        hn4_free_block(vol, hn4_addr_to_u64(ext_phys_lba));
        return HN4_ERR_HW_IO;
    }
    
    /* 
     * 6. "The Etch" (In-Memory Anchor Mutation)
     * We cryptographically mix the signature into the Anchor's Orbit Vector (V).
     * This acts as a secondary verification: If the signature block is lost/forged,
     * the file data becomes ballistically unreachable because V will be wrong.
     */
    hn4_anchor_t temp_anchor;
    
    /* Zero-init ensures deterministic checksumming */
    memset(&temp_anchor, 0, sizeof(hn4_anchor_t));
    memcpy(&temp_anchor, anchor, sizeof(hn4_anchor_t));

    /* Extract Current V */
    uint64_t current_V = 0;
    const uint8_t* raw_v = temp_anchor.orbit_vector;
    current_V = (uint64_t)raw_v[0] | ((uint64_t)raw_v[1] << 8) |
                ((uint64_t)raw_v[2] << 16) | ((uint64_t)raw_v[3] << 24) |
                ((uint64_t)raw_v[4] << 32) | ((uint64_t)raw_v[5] << 40);

    /* Calculate Entropy from Signature */
    hn4_u128_t sig_hash_128 = _siphash_128(signature, 64, &vol->sb.info.volume_uuid);
    uint64_t sig_hash = sig_hash_128.lo ^ sig_hash_128.hi;
    
    /* Mix Entropy into V */
    uint64_t mixed_hash = _wyhash_mix(sig_hash, 0xbf58476d1ce4e5b9ULL);
    uint64_t entangled_V = (current_V ^ mixed_hash);
    
    /* Scramble and ensure Odd parity (Fundamental Ballistic requirement) */
    entangled_V = hn4_rotl64(entangled_V, 19); 
    entangled_V |= 1;

    /* Write Back V to Anchor */
    temp_anchor.orbit_vector[0] = (uint8_t)(entangled_V & 0xFF);
    temp_anchor.orbit_vector[1] = (uint8_t)((entangled_V >> 8) & 0xFF);
    temp_anchor.orbit_vector[2] = (uint8_t)((entangled_V >> 16) & 0xFF);
    temp_anchor.orbit_vector[3] = (uint8_t)((entangled_V >> 24) & 0xFF);
    temp_anchor.orbit_vector[4] = (uint8_t)((entangled_V >> 32) & 0xFF);
    temp_anchor.orbit_vector[5] = (uint8_t)((entangled_V >> 40) & 0xFF);

    /* Point Inline Buffer to New Extension Head */
    uint64_t ext_ptr_le = hn4_cpu_to_le64(new_ext_idx);
    
    /* Wipe previous inline data/pointers and set new pointer */
    memset(temp_anchor.inline_buffer, 0, sizeof(temp_anchor.inline_buffer));
    memcpy(temp_anchor.inline_buffer, &ext_ptr_le, 8);

    /* Set EXTENDED Flag in Data Class */
    uint64_t dclass = hn4_le64_to_cpu(temp_anchor.data_class);
    dclass |= HN4_FLAG_EXTENDED; 
    temp_anchor.data_class = hn4_cpu_to_le64(dclass);

    /* Final Checksum Recalculation */
    temp_anchor.checksum = 0;
    temp_anchor.checksum = hn4_cpu_to_le32(hn4_crc32(0, &temp_anchor, sizeof(hn4_anchor_t)));

    /* 
     * 7. Commit to Caller
     * NOTE: This does NOT write the Anchor to disk. That happens via `hn4_write_anchor_atomic`
     * which the caller is responsible for invoking after branding.
     */
    memcpy(anchor, &temp_anchor, sizeof(hn4_anchor_t));
    
    hn4_hal_mem_free(ext_buf);
    return HN4_OK;
}