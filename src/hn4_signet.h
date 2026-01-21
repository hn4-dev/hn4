/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Signet Protocol (Cryptographic Provenance)
 * HEADER:      hn4_signet.h
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 *   Provides primitives for cryptographically binding an Anchor to an
 *   Author Identity using Ed25519 signatures and topological hashing.
 */

#ifndef _HN4_SIGNET_H_
#define _HN4_SIGNET_H_

#include "hn4.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * hn4_signet_brand_anchor
 * 
 * Applies a Cryptographic Seal (Signet) to an existing Anchor.
 * 
 * MECHANISM:
 * 1. Validates the existing chain of signatures (if any).
 * 2. Allocates a new Extension Block for the signature payload.
 * 3. Links the new block to the previous chain head (Topological Hash).
 * 4. Modifies the Anchor in-memory (The Etch):
 *    - Updates 'orbit_vector' by mixing the signature entropy.
 *    - Updates 'inline_buffer' to point to the new Extension Block.
 *    - Sets the HN4_FLAG_EXTENDED bit.
 * 
 * SAFETY:
 * - The caller MUST persist the modified anchor (hn4_write_anchor_atomic)
 *   after this function returns HN4_OK.
 * - This function handles the persistence of the Extension Block itself
 *   (including Barriers/FUA).
 * 
 * @param vol        Volume Context.
 * @param anchor     The Anchor to modify (In-Memory).
 * @param author_id  64-bit unique ID of the signer.
 * @param signature  64-byte Ed25519 signature buffer.
 * @param sig_len    Must be 64.
 * @param public_key 32-byte Ed25519 public key buffer.
 * 
 * @return HN4_OK on success.
 * @return HN4_ERR_TAMPERED if existing chain is broken.
 */
_Check_return_
hn4_result_t hn4_signet_brand_anchor(
    HN4_IN    hn4_volume_t* vol,
    HN4_INOUT hn4_anchor_t* anchor,
    HN4_IN    uint64_t      author_id,
    HN4_IN    const uint8_t* signature,
    HN4_IN    uint32_t      sig_len,
    HN4_IN    const uint8_t* public_key
);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_SIGNET_H_ */