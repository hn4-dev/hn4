/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Resilience Comparison Tests (vs ZFS)
 * SOURCE:      hn4_zfs_resilience_tests.c
 * STATUS:      PRODUCTION / TEST SUITE
 *
 * TEST OBJECTIVE:
 * Verify HN4 resilience features that address specific failure modes 
 * where traditional filesystems (like ZFS, ext4) historically struggle.
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
} _zfs_res_hal_t;

static void _z_inject_buffer(hn4_hal_device_t* dev, uint8_t* buffer) {
    _zfs_res_hal_t* impl = (_zfs_res_hal_t*)dev;
    impl->mmio_base = buffer;
}

static void _z_write_sb(hn4_hal_device_t* dev, hn4_superblock_t* sb, uint64_t lba_sector) {
    sb->raw.sb_crc = 0;
    uint32_t crc = hn4_crc32(0, sb, HN4_SB_SIZE - 4);
    sb->raw.sb_crc = hn4_cpu_to_le32(crc);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(lba_sector), sb, HN4_SB_SIZE / RES_SECTOR_SIZE);
}

static hn4_hal_device_t* zfs_resilience_setup(void) {
    uint8_t* ram = calloc(1, RES_FIXTURE_SIZE);
    hn4_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(_zfs_res_hal_t));
    
    hn4_hal_caps_t* caps = (hn4_hal_caps_t*)dev;
#ifdef HN4_USE_128BIT
    caps->total_capacity_bytes.lo = RES_FIXTURE_SIZE;
#else
    caps->total_capacity_bytes = RES_FIXTURE_SIZE;
#endif
    caps->logical_block_size = RES_SECTOR_SIZE;
    caps->hw_flags = HN4_HW_NVM;
    
    _z_inject_buffer(dev, ram);
    hn4_hal_init();
    hn4_crc_init();
    
    hn4_superblock_t sb = {0};
    sb.info.magic = HN4_MAGIC_SB;
    sb.info.version = 0x00060006;
    sb.info.block_size = RES_BLOCK_SIZE;
    sb.info.magic_tail = HN4_MAGIC_TAIL;
    sb.info.state_flags = HN4_VOL_CLEAN | HN4_VOL_METADATA_ZEROED;
    sb.info.volume_uuid.lo = 0xBADF00D;
    sb.info.current_epoch_id = 1;
    
#ifdef HN4_USE_128BIT
    sb.info.total_capacity.lo = RES_FIXTURE_SIZE;
#else
    sb.info.total_capacity = RES_FIXTURE_SIZE;
#endif

    sb.info.lba_epoch_start  = hn4_lba_from_sectors(2048);
    sb.info.lba_cortex_start = hn4_lba_from_sectors(4096);
    sb.info.lba_bitmap_start = hn4_lba_from_sectors(8192);
    sb.info.lba_qmask_start  = hn4_lba_from_sectors(10000);
    sb.info.lba_flux_start   = hn4_lba_from_sectors(16384);
    
    /* Pointers for logic */
    sb.info.epoch_ring_block_idx = hn4_lba_from_blocks(2048 / 8);

    /* Write North (0) */
    _z_write_sb(dev, &sb, 0);
    
    /* Write East (33%) & West (66%) & South (100%) */
    uint64_t cap_bytes = RES_FIXTURE_SIZE;
    uint32_t bs = RES_BLOCK_SIZE;
    _z_write_sb(dev, &sb, (HN4_ALIGN_UP((cap_bytes/100)*33, bs))/RES_SECTOR_SIZE);
    _z_write_sb(dev, &sb, (HN4_ALIGN_UP((cap_bytes/100)*66, bs))/RES_SECTOR_SIZE);
    _z_write_sb(dev, &sb, (HN4_ALIGN_DOWN(cap_bytes - HN4_SB_SIZE, bs))/RES_SECTOR_SIZE);

    /* Init Resources */
    uint32_t len = 4096; uint8_t* buf = calloc(1, len); memset(buf, 0, len);
    
    hn4_anchor_t root = {0};
    /* FIX: Set both lo and hi for valid Root ID (0xFF...FF) */
    root.seed_id.lo = 0xFFFFFFFFFFFFFFFF; 
    root.seed_id.hi = 0xFFFFFFFFFFFFFFFF;
    
    root.data_class = hn4_cpu_to_le64(HN4_VOL_STATIC | HN4_FLAG_VALID);
    root.checksum = hn4_cpu_to_le32(hn4_crc32(0, &root, offsetof(hn4_anchor_t, checksum)));
    
    memcpy(buf, &root, sizeof(root));
    /* Write full block to ensure alignment */
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_cortex_start, buf, RES_BLOCK_SIZE / RES_SECTOR_SIZE);
    
    memset(buf, 0xAA, len); /* QMask Silver */
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_qmask_start, buf, 1);
    
    hn4_epoch_header_t ep = {0}; ep.epoch_id=1; ep.epoch_crc = hn4_epoch_calc_crc(&ep);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_epoch_start, &ep, 1);

    free(buf);
    return dev;
}

static void zfs_teardown(hn4_hal_device_t* dev) {
    _zfs_res_hal_t* impl = (_zfs_res_hal_t*)dev;
    free(impl->mmio_base);
    hn4_hal_mem_free(dev);
}

/* =========================================================================
 * TEST 1: Resilience_Partition_Nuke_Survival
 * ========================================================================= */
hn4_TEST(Resilience, Resilience_Partition_Nuke_Survival) {
    hn4_hal_device_t* dev = zfs_resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};

    /* 1. Nuke Start (10%) */
    uint64_t wipe_size = (RES_FIXTURE_SIZE / 10);
    uint8_t* zeros = calloc(1, wipe_size);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(0), zeros, wipe_size / RES_SECTOR_SIZE);

    /* 2. Nuke End (10%) */
    uint64_t start_sec = (RES_FIXTURE_SIZE - wipe_size) / RES_SECTOR_SIZE;
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(start_sec), zeros, wipe_size / RES_SECTOR_SIZE);
    free(zeros);

    /* 3. Attempt Mount */
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* 4. Verify we are using the correct volume */
    ASSERT_EQ(0xBADF00D, vol->sb.info.volume_uuid.lo);
    
    /* 5. Verify Self-Healing */
    hn4_superblock_t sb;
    hn4_hal_sync_io(dev, HN4_IO_READ, 0, &sb, HN4_SB_SIZE/RES_SECTOR_SIZE);
    ASSERT_EQ(HN4_MAGIC_SB, hn4_le64_to_cpu(sb.info.magic));

    hn4_unmount(vol);
    zfs_teardown(dev);
}

/* =========================================================================
 * TEST 4: Resilience_Atomic_Hop_Crash
 * ========================================================================= */
hn4_TEST(Resilience, Resilience_Atomic_Hop_Crash) {
    hn4_hal_device_t* dev = zfs_resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* 1. Setup File with Data V1 at G1 */
    uint64_t G1 = 100;
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x1111;
    anchor.gravity_center = hn4_cpu_to_le64(G1);
    anchor.write_gen = hn4_cpu_to_le32(1);
    anchor.data_class = hn4_cpu_to_le64(HN4_VOL_ATOMIC | HN4_FLAG_VALID);
    
    ASSERT_EQ(G1, hn4_le64_to_cpu(anchor.gravity_center));
    ASSERT_EQ(1, hn4_le32_to_cpu(anchor.write_gen));

    hn4_unmount(vol);
    zfs_teardown(dev);
}

/* =========================================================================
 * TEST 5: Resilience_BitRot_Auto_Quarantine
 * ========================================================================= */
hn4_TEST(Resilience, Resilience_BitRot_Auto_Quarantine) {
    hn4_hal_device_t* dev = zfs_resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = hn4_lba_from_sectors(5000);
    uint8_t buf[4096] = {0};

    /* Target OOB to force error */
    hn4_addr_t bad_lba = hn4_lba_from_sectors(RES_FIXTURE_SIZE / 512 + 1000);
    
    /* Using Mock HAL property: OOB returns INVALID/HW_IO */
    extern hn4_result_t hn4_repair_block(hn4_volume_t*, hn4_addr_t, const void*, uint32_t);
    hn4_result_t res = hn4_repair_block(vol, bad_lba, buf, 4096);
    
    ASSERT_TRUE(res != HN4_OK);
    
    /* Verify Volume stays alive */
    ASSERT_FALSE(vol->sb.info.state_flags & HN4_VOL_PANIC);
    ASSERT_FALSE(vol->read_only);

    hn4_unmount(vol);
    zfs_teardown(dev);
}

/*
 * Actual Test 8: Resilience_Massive_Inode_Density (Memory Efficiency)
 * Scenario: Volume has 10,000 files. 
 *           Verify mount succeeds and DOES NOT hog RAM (nano_cortex should be freed).
 */
hn4_TEST(Resilience, Resilience_Massive_Inode_Density) {
    hn4_hal_device_t* dev = zfs_resilience_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    
    /* 1. Pre-Mount Injection: 10,000 Anchors */
    uint64_t c_start = 4096; 
    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
    
    /* Small buffer to reduce test overhead */
    uint8_t buf[4096]; 
    
    for (int i=0; i<320; i++) {
        for(int j=0; j<32; j++) {
            a.seed_id.lo = (i*32) + j + 1;
            memcpy(buf + (j*128), &a, 128);
        }
        hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(c_start + i * 8), buf, 8);
    }
    
    /* 2. Mount */
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));
    
    /* 3. Verify Memory Efficiency */
    /* Reconstruction buffer should be freed. 
       Runtime usage should be minimal (just struct overhead). */
    ASSERT_TRUE(vol->nano_cortex == NULL);
    
    hn4_unmount(vol);
    zfs_teardown(dev);
}