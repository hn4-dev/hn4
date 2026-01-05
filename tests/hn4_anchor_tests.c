/*
 * HYDRA-NEXUS 4 (HN4) - ANCHOR GENESIS TESTS
 * FILE: hn4_anchor_tests.c
 * STATUS: LOGIC VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_anchor.h"
#include "hn4_hal.h"
#include "hn4_constants.h"
#include "hn4_errors.h"

/* --- FIXTURE HELPERS --- */

#define ANCHOR_BLOCK_SIZE 4096
#define ANCHOR_SECTOR_SIZE 512
#define ANCHOR_CAPACITY   (100ULL * 1024ULL * 1024ULL) /* 100 MB */

typedef struct {
    hn4_hal_caps_t caps;
} mock_anchor_hal_t;

static hn4_volume_t* create_anchor_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_anchor_hal_t* dev = hn4_hal_mem_alloc(sizeof(mock_anchor_hal_t));
    memset(dev, 0, sizeof(mock_anchor_hal_t));
    
    dev->caps.logical_block_size = ANCHOR_SECTOR_SIZE;
    dev->caps.total_capacity_bytes = ANCHOR_CAPACITY;

    vol->target_device = dev;
    vol->sb.info.block_size = ANCHOR_BLOCK_SIZE;
    vol->sb.info.total_capacity = ANCHOR_CAPACITY;
    
    /* Default: Valid Cortex Start at Block 10 (LBA 80) */
    vol->sb.info.lba_cortex_start = (10 * ANCHOR_BLOCK_SIZE) / ANCHOR_SECTOR_SIZE;
    
    /* Default: Assume metadata is zeroed (Safety Contract) */
    vol->sb.info.state_flags = HN4_VOL_METADATA_ZEROED;
    vol->sb.info.generation_ts = 123456789ULL;

    return vol;
}

static void cleanup_anchor_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: Uninitialized Cortex Guard
 * Rationale: 
 * hn4_anchor_write_genesis must strictly enforce the "Zeroed" pre-condition.
 * Writing a Root Anchor into non-zeroed memory creates Ghost Anchors.
 * ========================================================================= */
hn4_TEST(AnchorGenesis, RequiresZeroedFlag) {
    hn4_volume_t* vol = create_anchor_fixture();
    
    /* Clear the safety flag */
    vol->sb.info.state_flags &= ~HN4_VOL_METADATA_ZEROED;

    hn4_result_t res = hn4_anchor_write_genesis(vol->target_device, &vol->sb);
    
    ASSERT_EQ(HN4_ERR_UNINITIALIZED, res);
    
    cleanup_anchor_fixture(vol);
}

/* =========================================================================
 * TEST 2: Root Alignment Check
 * Rationale: 
 * The Root Anchor must be aligned to the Volume Block Size, even though
 * LBA addressing is sector-based. Misalignment breaks the D0 Table stride.
 * ========================================================================= */
hn4_TEST(AnchorGenesis, MisalignedStart) {
    hn4_volume_t* vol = create_anchor_fixture();
    
    /* 
     * Block Size = 4096, Sector = 512.
     * Valid LBAs are multiples of 8.
     * Set LBA to 13 (Misaligned).
     */
    vol->sb.info.lba_cortex_start = 13;

    hn4_result_t res = hn4_anchor_write_genesis(vol->target_device, &vol->sb);
    
    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, res);
    
    cleanup_anchor_fixture(vol);
}

/* =========================================================================
 * TEST 3: Invalid Geometry (Block < Sector)
 * Rationale:
 * If Block Size is smaller than Sector Size, the sector_count calculation
 * becomes 0. This must be caught to prevent silent no-op writes.
 * ========================================================================= */
hn4_TEST(AnchorGenesis, ImpossibleGeometry) {
    hn4_volume_t* vol = create_anchor_fixture();
    mock_anchor_hal_t* mdev = (mock_anchor_hal_t*)vol->target_device;

    /* 
     * Sector Size = 512 (Default)
     * Set Block Size = 256
     * sector_count = 256 / 512 = 0
     */
    vol->sb.info.block_size = 256;

    hn4_result_t res = hn4_anchor_write_genesis(vol->target_device, &vol->sb);
    
    ASSERT_EQ(HN4_ERR_GEOMETRY, res);
    
    cleanup_anchor_fixture(vol);
}

/* =========================================================================
 * TEST 4: Happy Path (Genesis Success)
 * Rationale:
 * Verify that when all pre-conditions are met (Zeroed Flag, Alignment, 
 * Geometry), the function returns HN4_OK and does not crash.
 * ========================================================================= */
hn4_TEST(AnchorGenesis, Success) {
    hn4_volume_t* vol = create_anchor_fixture();
    
    /* 
     * Setup:
     * - Flag is set (default in fixture)
     * - Alignment is valid (default in fixture)
     * - Geometry is valid (4096 BS / 512 SS)
     * - Permissions injected via compat_flags
     */
    vol->sb.info.compat_flags = 0; /* Default perms only */

    /* Mock HAL usually returns OK for simple writes */
    hn4_result_t res = hn4_anchor_write_genesis(vol->target_device, &vol->sb);
    
    ASSERT_EQ(HN4_OK, res);
    
    cleanup_anchor_fixture(vol);
}