/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Anchor Management & Cortex Logic
 * SOURCE:      hn4_anchor.h
 * VERSION:     1.2 (Hardened Genesis)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Provides primitives for creating, validating, and manipulating Anchors.
 */

#ifndef HN4_ANCHOR_H
#define HN4_ANCHOR_H

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_annotations.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * hn4_anchor_write_genesis
 * 
 * Creates the System Root Anchor (ID: 0xFF...FF) and writes it to the
 * start of the Cortex (D0) region.
 * 
 * SAFETY REQUIREMENTS:
 * 1. The Cortex Region MUST be zeroed before calling this. The function
 *    relies on HN4_VOL_METADATA_ZEROED flag and only writes the first block.
 *    (Debug builds will physically verify this, Prod builds trust the flag).
 * 
 * 2. CRC Consistency: This function implements the Split-CRC strategy.
 *    Validators must calculate CRC(0 -> Offset_Checksum) then chain
 *    CRC(Result -> Inline_Buffer).
 * 
 * 3. Barrier: This function issues a FLUSH barrier upon success.
 * 
 * @param dev   The HAL device handle.
 * @param sb    The populated Superblock containing geometry info.
 * @return      HN4_OK on success, error code on failure.
 */
hn4_result_t hn4_anchor_write_genesis(hn4_hal_device_t* dev, const hn4_superblock_t* sb);


/**
 * hn4_write_anchor_atomic
 * Persists a modified Anchor to the Cortex.
 * Handles Read-Modify-Write if anchor size < sector size.
 */
hn4_result_t hn4_write_anchor_atomic(
    HN4_IN hn4_volume_t* vol, 
    HN4_IN hn4_anchor_t* anchor
);


#ifdef __cplusplus
}
#endif

#endif /* HN4_ANCHOR_H */