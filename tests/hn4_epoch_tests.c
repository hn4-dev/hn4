/*
 * HYDRA-NEXUS 4 (HN4) - EPOCH MANAGER TESTS
 * FILE: hn4_epoch_tests.c
 * STATUS: LOGIC VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_epoch.h"
#include "hn4_hal.h"
#include "hn4_errors.h"

/* --- FIXTURE HELPERS --- */

#define TEST_BLOCK_SIZE 4096
#define TEST_SECTOR_SIZE 512
#define TEST_CAPACITY   (100ULL * 1024ULL * 1024ULL) /* 100 MB */

typedef struct {
    hn4_hal_caps_t caps;
} mock_hal_device_t;

static hn4_volume_t* create_epoch_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    memset(dev, 0, sizeof(mock_hal_device_t));
    
    dev->caps.logical_block_size = TEST_SECTOR_SIZE;
    dev->caps.total_capacity_bytes = TEST_CAPACITY;

    vol->target_device = dev;
    vol->sb.info.block_size = TEST_BLOCK_SIZE;
    vol->sb.info.total_capacity = TEST_CAPACITY;
    vol->sb.info.current_epoch_id = 10;
    
    /* 
     * Default Valid Layout:
     * Ring Start: 1MB offset (Block 256)
     * Ring Size: 1MB (256 Blocks)
     */
    vol->sb.info.lba_epoch_start = (1024 * 1024) / TEST_SECTOR_SIZE; 
    vol->sb.info.epoch_ring_block_idx = (1024 * 1024) / TEST_BLOCK_SIZE; 

    return vol;
}

static void cleanup_epoch_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: Genesis Alignment Check
 * Rationale: 
 * hn4_epoch_write_genesis must fail if the Epoch Start LBA is not aligned 
 * to the Volume Block Size boundaries.
 * ========================================================================= */
hn4_TEST(EpochGenesis, AlignmentFail) {
    hn4_volume_t* vol = create_epoch_fixture();
    
    /* 
     * Block Size = 4096, Sector = 512. SPB = 8.
     * Valid LBAs must be multiples of 8.
     * Set LBA to 17 (Not divisible by 8).
     */
    vol->sb.info.lba_epoch_start = 17;

    hn4_result_t res = hn4_epoch_write_genesis(vol->target_device, &vol->sb);
    
    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, res);
    
    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 2: Ring Topology Capacity Overflow
 * Rationale:
 * hn4_epoch_check_ring must calculate the full extent of the 1MB ring.
 * If the ring extends beyond the physical volume capacity, it must reject
 * the configuration to prevent Ghost Writes (writing off end of disk).
 * ========================================================================= */
hn4_TEST(EpochCheck, TopologyOverflow) {
    hn4_volume_t* vol = create_epoch_fixture();
    mock_hal_device_t* mdev = (mock_hal_device_t*)vol->target_device;

    /* Shrink the device to be smaller than the Ring Start + Ring Size */
    /* Ring starts at 1MB. Ring size is 1MB. End is 2MB. */
    /* Set capacity to 1.5 MB. */
    mdev->caps.total_capacity_bytes = (1024 * 1024) + (512 * 1024);

    /* We pass the capacity explicitly as required by the API */
    hn4_result_t res = hn4_epoch_check_ring(
        vol->target_device, 
        &vol->sb, 
        mdev->caps.total_capacity_bytes
    );
    
    ASSERT_EQ(HN4_ERR_GEOMETRY, res);
    
    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 3: Advance Read-Only Guard
 * Rationale:
 * hn4_epoch_advance must immediately reject requests if the volume is mounted
 * Read-Only, preventing state divergence between RAM and Disk.
 * ========================================================================= */
hn4_TEST(EpochAdvance, ReadOnlyGuard) {
    hn4_volume_t* vol = create_epoch_fixture();
    
    uint64_t new_id = 0;
    hn4_addr_t new_ptr = 0;

    hn4_result_t res = hn4_epoch_advance(
        vol->target_device,
        &vol->sb,
        true, /* is_read_only = TRUE */
        &new_id,
        &new_ptr
    );

    ASSERT_EQ(HN4_ERR_MEDIA_TOXIC, res);
    
    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 4: Ring Pointer Wrap-Around Math
 * Rationale:
 * Verify the modular arithmetic correctly wraps the pointer back to the
 * start of the ring when it reaches the end.
 * Ring Size = 1MB. Block Size = 4096. Ring Len = 256 blocks.
 * ========================================================================= */
hn4_TEST(EpochAdvance, RingWrapAround) {
    hn4_volume_t* vol = create_epoch_fixture();

    /* 
     * Setup:
     * Ring Start Block = 256 (1MB offset).
     * Ring Length = 256 blocks.
     * End Block = 512.
     * 
     * Set current pointer to the LAST valid block (511).
     */
    uint64_t start_blk = 256;
    uint64_t ring_len = 256;
    vol->sb.info.epoch_ring_block_idx = start_blk + ring_len - 1; /* Block 511 */

    uint64_t out_id = 0;
    hn4_addr_t out_ptr = 0;

    /* Assume HAL write succeeds */
    hn4_result_t res = hn4_epoch_advance(
        vol->target_device,
        &vol->sb,
        false,
        &out_id,
        &out_ptr
    );

    ASSERT_EQ(HN4_OK, res);
    
    /* 
     * Expectation:
     * (511 - 256 + 1) % 256 = 0.
     * New Ptr = Start (256) + 0 = 256.
     */
#ifdef HN4_USE_128BIT
    ASSERT_EQ(start_blk, out_ptr.lo);
#else
    ASSERT_EQ(start_blk, out_ptr);
#endif
    
    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 5: Geometry Constraint (Block Size vs Header Size)
 * Rationale:
 * The Epoch Header struct is ~128 bytes. If the Block Size is configured 
 * dangerously small (e.g. 64 bytes), the code must reject it to prevent 
 * memory corruption/overflow during the struct serialization.
 * ========================================================================= */
hn4_TEST(EpochAdvance, BlockSizeTooSmall) {
    hn4_volume_t* vol = create_epoch_fixture();
    
    /* Set Block Size to 64 bytes (Smaller than sizeof(hn4_epoch_header_t)) */
    vol->sb.info.block_size = 64; 

    uint64_t new_id = 0;
    hn4_addr_t new_ptr = 0;

    hn4_result_t res = hn4_epoch_advance(
        vol->target_device,
        &vol->sb,
        false,
        &new_id,
        &new_ptr
    );

    ASSERT_EQ(HN4_ERR_GEOMETRY, res);
    
    cleanup_epoch_fixture(vol);
}



/* =========================================================================
 * TEST 8: Ring Integrity Failure (Bad CRC)
 * Rationale:
 * If the Epoch Header at the ring pointer has an invalid CRC, we have lost
 * temporal tracking. The driver must return HN4_ERR_EPOCH_LOST.
 * ========================================================================= */
hn4_TEST(EpochCheck, BadCRC) {
    hn4_volume_t* vol = create_epoch_fixture();
    mock_hal_device_t* mdev = (mock_hal_device_t*)vol->target_device;

    hn4_epoch_header_t ep = {0};
    ep.epoch_id = vol->sb.info.current_epoch_id;
    ep.epoch_crc = 0xDEADBEEF; /* Invalid CRC */

    uint64_t ptr_lba = vol->sb.info.epoch_ring_block_idx * (TEST_BLOCK_SIZE / TEST_SECTOR_SIZE);
    
    void* io_buf = hn4_hal_mem_alloc(TEST_BLOCK_SIZE);
    memcpy(io_buf, &ep, sizeof(ep));
    hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, ptr_lba, io_buf, TEST_BLOCK_SIZE / TEST_SECTOR_SIZE);
    hn4_hal_mem_free(io_buf);

    hn4_result_t res = hn4_epoch_check_ring(
        vol->target_device, 
        &vol->sb, 
        mdev->caps.total_capacity_bytes
    );

    ASSERT_EQ(HN4_ERR_EPOCH_LOST, res);

    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 9: Generation Exhaustion
 * Rationale:
 * hn4_epoch_advance checks the Superblock `copy_generation`. If it nears
 * the 64-bit limit (Safety buffer 0xF0), it must reject the advance to
 * prevent overflow/rollover.
 * ========================================================================= */
hn4_TEST(EpochAdvance, GenerationCap) {
    hn4_volume_t* vol = create_epoch_fixture();
    
    /* Set Generation to Limit */
    vol->sb.info.copy_generation = 0xFFFFFFFFFFFFFFF0ULL;

    uint64_t out_id;
    hn4_addr_t out_ptr;

    hn4_result_t res = hn4_epoch_advance(
        vol->target_device,
        &vol->sb,
        false,
        &out_id,
        &out_ptr
    );

    ASSERT_EQ(HN4_ERR_EEXIST, res);

    cleanup_epoch_fixture(vol);
}

/* =========================================================================
 * TEST 10: PICO Profile Tiny Ring
 * Rationale:
 * HN4_PROFILE_PICO uses a tiny 2-block ring instead of the standard 1MB ring.
 * Verify that the wrapping logic respects the PICO constraint (wrap after 2 blocks).
 * ========================================================================= */
hn4_TEST(EpochAdvance, PicoRingTopology) {
    hn4_volume_t* vol = create_epoch_fixture();
    
    /* 1. Switch to PICO Profile */
    vol->sb.info.format_profile = HN4_PROFILE_PICO;
    
    /* 
     * 2. Setup Ring Geometry
     * Start at Block 100.
     * PICO Ring Size = 2 Blocks.
     * Valid Blocks: 100, 101.
     */
    uint64_t start_blk = 100;
    vol->sb.info.lba_epoch_start = start_blk * (TEST_BLOCK_SIZE / TEST_SECTOR_SIZE);
    
    /* Set current pointer to LAST block (101) */
    vol->sb.info.epoch_ring_block_idx = 101;

    uint64_t out_id;
    hn4_addr_t out_ptr;

    hn4_result_t res = hn4_epoch_advance(
        vol->target_device,
        &vol->sb,
        false,
        &out_id,
        &out_ptr
    );

    ASSERT_EQ(HN4_OK, res);

    /* 
     * Expectation: Wrap around to Start Block (100)
     * (101 - 100 + 1) % 2 = 0 -> Start + 0 = 100.
     */
#ifdef HN4_USE_128BIT
    ASSERT_EQ(start_blk, out_ptr.lo);
#else
    ASSERT_EQ(start_blk, out_ptr);
#endif

    cleanup_epoch_fixture(vol);
}