/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Epoch & Time Management
 * HEADER:      hn4_epoch.h
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * APIs for managing the cyclic Epoch Ring, detecting time dilation,
 * and advancing generation counters.
 */

#ifndef HN4_EPOCH_H
#define HN4_EPOCH_H

#include "hn4.h"
#include "hn4_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * hn4_epoch_write_genesis
 *
 * PURPOSE:
 * Initializes the Epoch Ring by writing ID:1 at the ring start.
 * Handles allocation, CRC calculation, Endianness, and I/O.
 *
 * ARGS:
 *  dev       - Target hardware device
 *  sb        - Populated Superblock (provides geometry and timestamp)
 */
hn4_result_t hn4_epoch_write_genesis(hn4_hal_device_t* dev, const hn4_superblock_t* sb);

/*
 * hn4_epoch_check_ring
 *
 * Validates the integrity of the Epoch Ring based on the Superblock state.
 * Detects Time Dilation, Generation Skew, and Toxic Media.
 *
 * ARGS:
 *  dev      - Hardware device
 *  sb       - The Superblock containing the Ring Pointer
 *  vol_cap  - Volume Capacity (for bounds checking)
 */
hn4_result_t hn4_epoch_check_ring(
    hn4_hal_device_t* dev, 
    const hn4_superblock_t* sb, 
    hn4_size_t vol_cap
);

/**
 * hn4_epoch_advance
 *
 * Persists the next Epoch ID to the ring.
 * Handles Ring Wrap-around, Geometry Validation, and Serialization.
 *
 * @param dev           HAL Device
 * @param sb            Current Superblock (Source of Geometry/Current ID)
 * @param is_read_only  Safety flag to prevent writing if volume is RO
 * @param out_new_id    Returns the new Epoch ID (or old on failure)
 * @param out_new_ptr   Returns the new Block Index of the ring pointer
 *
 * @return HN4_OK, or HN4_ERR_DATA_ROT if ring is corrupted.
 */
hn4_result_t hn4_epoch_advance(
    hn4_hal_device_t* dev, 
    const hn4_superblock_t* sb,
    bool is_read_only,
    uint64_t* out_new_id,
    hn4_addr_t* out_new_ptr
);



#ifdef __cplusplus
}
#endif

#endif /* HN4_EPOCH_H */