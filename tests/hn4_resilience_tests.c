/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Resilience & Catastrophe Tests
 * SOURCE:      hn4_resilience_tests.c
 * STATUS:      PRODUCTION / TEST SUITE
 *
 * TEST OBJECTIVE:
 * Verify system behavior under catastrophic hardware failure scenarios.
 */

#include "hn4.h"
#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_constants.h"
#include "hn4_addr.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * FIXTURE INFRASTRUCTURE
 * ========================================================================= */

#define RES_FIXTURE_SIZE    (64ULL * 1024 * 1024)
#define RES_BLOCK_SIZE      4096
#define RES_SECTOR_SIZE     512

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} _res_test_hal_t;

static void _res_inject_buffer(hn4_hal_device_t* dev, uint8_t* buffer) {
    _res_test_hal_t* impl = (_res_test_hal_t*)dev;
    impl->mmio_base = buffer;
}

static void _res_write_sb(hn4_hal_device_t* dev, hn4_superblock_t* sb, uint64_t lba_sector) {
    sb->raw.sb_crc = 0;
    uint32_t crc = hn4_crc32(0, sb, HN4_SB_SIZE - 4);
    sb->raw.sb_crc = hn4_cpu_to_le32(crc);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(lba_sector), sb, HN4_SB_SIZE / RES_SECTOR_SIZE);
}

static hn4_hal_device_t* resilience_setup(void) {
    uint8_t* ram = calloc(1, RES_FIXTURE_SIZE);
    hn4_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(_res_test_hal_t));
    
    hn4_hal_caps_t* caps = (hn4_hal_caps_t*)dev;
#ifdef HN4_USE_128BIT
    caps->total_capacity_bytes.lo = RES_FIXTURE_SIZE;
#else
    caps->total_capacity_bytes = RES_FIXTURE_SIZE;
#endif
    caps->logical_block_size = RES_SECTOR_SIZE;
    caps->hw_flags = HN4_HW_NVM;
    
    _res_inject_buffer(dev, ram);
    hn4_hal_init();
    hn4_crc_init();
    
    /* Standard Layout */
    hn4_superblock_t sb = {0};
    sb.info.magic = HN4_MAGIC_SB;
    sb.info.version = 0x00060006;
    sb.info.block_size = RES_BLOCK_SIZE;
    sb.info.state_flags = HN4_VOL_CLEAN | HN4_VOL_METADATA_ZEROED;
    sb.info.volume_uuid.lo = 0xDEADBEEF;
    sb.info.current_epoch_id = 1;

    /* FIX: Must initialize total_capacity to match HAL caps */
#ifdef HN4_USE_128BIT
    sb.info.total_capacity.lo = RES_FIXTURE_SIZE;
#else
    sb.info.total_capacity = RES_FIXTURE_SIZE;
#endif
    
    sb.info.lba_epoch_start  = hn4_lba_from_sectors(16);
    sb.info.lba_cortex_start = hn4_lba_from_sectors(2048);
    sb.info.lba_bitmap_start = hn4_lba_from_sectors(4096);
    sb.info.lba_qmask_start  = hn4_lba_from_sectors(6144);
    sb.info.lba_flux_start   = hn4_lba_from_sectors(8192);
    sb.info.lba_horizon_start = hn4_lba_from_sectors(32768);
    sb.info.journal_start     = hn4_lba_from_sectors(60000);
    sb.info.journal_ptr       = sb.info.journal_start;

    /* Write North SB (Primary) */
    _res_write_sb(dev, &sb, 0);
    
    /* Write East Mirror (Backup) */
    uint64_t cap_bytes = RES_FIXTURE_SIZE;
    uint32_t bs = RES_BLOCK_SIZE;
    uint64_t east_bytes = HN4_ALIGN_UP((cap_bytes / 100) * 33, bs);
    _res_write_sb(dev, &sb, east_bytes / RES_SECTOR_SIZE);

    /* Init Resources */
    uint32_t qm_size = 4096; uint8_t* qm = calloc(1, qm_size); memset(qm, 0xAA, qm_size);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_qmask_start, qm, qm_size / RES_SECTOR_SIZE);
    free(qm);

    /* Init Root & Epoch */
    hn4_anchor_t root = {0};
    root.seed_id.lo = 0xFFFFFFFFFFFFFFFF;
    root.data_class = hn4_cpu_to_le64(HN4_VOL_STATIC | HN4_FLAG_VALID);
    root.checksum = hn4_cpu_to_le32(hn4_crc32(0, &root, offsetof(hn4_anchor_t, checksum)));
    
    uint8_t abuf[RES_BLOCK_SIZE] = {0}; memcpy(abuf, &root, sizeof(root));
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_cortex_start, abuf, RES_BLOCK_SIZE / RES_SECTOR_SIZE);

    hn4_epoch_header_t ep = {0};
    ep.epoch_id = 1; ep.epoch_crc = hn4_epoch_calc_crc(&ep);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_epoch_start, &ep, 1);

    return dev;
}

static void resilience_teardown(hn4_hal_device_t* dev) {
    _res_test_hal_t* impl = (_res_test_hal_t*)dev;
    free(impl->mmio_base);
    hn4_hal_mem_free(dev);
}

/* =========================================================================
 * TESTS
 * ========================================================================= */

/*
 * Test 1: Split_Brain_Recovery
 * Scenario: North SB has Gen 10. East SB has Gen 11 (Newer).
 *           Mount should detect the skew, pick East, and heal North.
 */
hn4_TEST(Resilience, Split_Brain_Recovery) {
    hn4_hal_device_t* dev = resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};

    /* 1. Read SBs */
    hn4_superblock_t sb;
    hn4_hal_sync_io(dev, HN4_IO_READ, 0, &sb, HN4_SB_SIZE/RES_SECTOR_SIZE);
    
    /* 2. Corrupt North: Set Gen 10 */
    sb.info.copy_generation = 10;
    _res_write_sb(dev, &sb, 0);
    
    /* 3. Update East: Set Gen 11 */
    sb.info.copy_generation = 11;
    uint64_t cap = RES_FIXTURE_SIZE;
    uint64_t east_sec = (HN4_ALIGN_UP((cap / 100) * 33, RES_BLOCK_SIZE)) / RES_SECTOR_SIZE;
    _res_write_sb(dev, &sb, east_sec);

    /* 4. Mount */
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));
    
    /* 5. Verify Winner */
    ASSERT_EQ(11, vol->sb.info.copy_generation);
    
    /* 6. Verify Self-Healing (North should now be 11 on disk) */
    hn4_hal_sync_io(dev, HN4_IO_READ, 0, &sb, HN4_SB_SIZE/RES_SECTOR_SIZE);
    ASSERT_TRUE(sb.info.copy_generation >= 11);

    hn4_unmount(vol);
    resilience_teardown(dev);
}

/*
 * Test 2: Bitflip_Void_Bitmap
 * Scenario: A cosmic ray flips a bit in the Void Bitmap (Free -> Used) for a block
 *           that no Anchor claims.
 *           Mount should succeed (tolerant), but Scavenger/Fsck logic handles it.
 */
hn4_TEST(Resilience, Bitflip_Void_Bitmap) {
    hn4_hal_device_t* dev = resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};

    /* 1. Manually flip bit for Block 500 */
    uint32_t bs = RES_BLOCK_SIZE;
    uint64_t bm_start = 4096; /* LBA */
    
    uint8_t* buf = calloc(1, bs);
    uint64_t* words = (uint64_t*)buf;
    /* Word 7 (500/64), Bit 52 */
    words[7] = hn4_cpu_to_le64(1ULL << 52); 
    
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(bm_start), buf, bs/512);
    free(buf);

    /* 2. Mount */
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));
    
    /* 3. Verify driver loaded the flipped bit */
    ASSERT_TRUE(vol->void_bitmap[7].data & (1ULL << 52));

    hn4_unmount(vol);
    resilience_teardown(dev);
}

/*
 * Test 3: ZFS_Write_Hole_Detection
 * Scenario: Anchor points to New Block, but New Block is garbage 
 *           (e.g. metadata updated, data didn't persist).
 */
hn4_TEST(Resilience, ZFS_Write_Hole_Detection) {
    hn4_hal_device_t* dev = resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* 1. Setup Anchor pointing to Block 100 */
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xAAAA;
    anchor.gravity_center = hn4_cpu_to_le64(100);
    anchor.data_class = hn4_cpu_to_le64(HN4_VOL_ATOMIC | HN4_FLAG_VALID);
    anchor.write_gen = hn4_cpu_to_le32(5);

    /* 2. Read - Block 100 is Garbage (Zero) */
    uint8_t buf[4096];
    hn4_result_t res = hn4_read_block_atomic(vol, &anchor, 0, buf, 4096);
    
    /* 3. Expect Failure (Phantom Block / Rot) */
    ASSERT_TRUE(res != HN4_OK);
    
    hn4_unmount(vol);
    resilience_teardown(dev);
}
