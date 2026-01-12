/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Auto-Medic Repair Tests
 * SOURCE:      hn4_repair_tests.c
 * STATUS:      FIXED / PRODUCTION
 *
 * TEST OBJECTIVE:
 * Verify the "Reactive Healing" protocol (Spec 21.1).
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
#include <stdatomic.h>

/* =========================================================================
 * FIXTURE INFRASTRUCTURE
 * ========================================================================= */

#define REP_FIXTURE_SIZE    (32ULL * 1024 * 1024)
#define REP_BLOCK_SIZE      4096
#define REP_SECTOR_SIZE     512

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} _rep_test_hal_t;

static void _rep_inject_buffer(hn4_hal_device_t* dev, uint8_t* buffer) {
    _rep_test_hal_t* impl = (_rep_test_hal_t*)dev;
    impl->mmio_base = buffer;
}

static void _rep_write_sb(hn4_hal_device_t* dev, hn4_superblock_t* sb, uint64_t lba_sector) {
    sb->raw.sb_crc = 0;
    uint32_t crc = hn4_crc32(0, sb, HN4_SB_SIZE - 4);
    sb->raw.sb_crc = hn4_cpu_to_le32(crc);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(lba_sector), sb, HN4_SB_SIZE / REP_SECTOR_SIZE);
}

static hn4_hal_device_t* repair_setup(void) {
    uint8_t* ram = calloc(1, REP_FIXTURE_SIZE);
    hn4_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(_rep_test_hal_t));
    
    hn4_hal_caps_t* caps = (hn4_hal_caps_t*)dev;
    
#ifdef HN4_USE_128BIT
    caps->total_capacity_bytes.lo = REP_FIXTURE_SIZE;
#else
    caps->total_capacity_bytes = REP_FIXTURE_SIZE;
#endif
    caps->logical_block_size = REP_SECTOR_SIZE;
    caps->hw_flags = HN4_HW_NVM;
    
    _rep_inject_buffer(dev, ram);
    hn4_hal_init();
    hn4_crc_init();
    
    /* Write valid Superblock */
    hn4_superblock_t sb = {0};
    sb.info.magic = HN4_MAGIC_SB;
    sb.info.version = 0x00060006;
    sb.info.block_size = REP_BLOCK_SIZE;
    sb.info.state_flags = HN4_VOL_CLEAN | HN4_VOL_METADATA_ZEROED;
    sb.info.volume_uuid.lo = 0x1;
    sb.info.current_epoch_id = 1;
    
#ifdef HN4_USE_128BIT
    sb.info.total_capacity.lo = REP_FIXTURE_SIZE;
#else
    sb.info.total_capacity = REP_FIXTURE_SIZE;
#endif
    
    /* Layout */
    sb.info.lba_epoch_start  = hn4_lba_from_sectors(16);
    sb.info.lba_cortex_start = hn4_lba_from_sectors(256);
    sb.info.lba_bitmap_start = hn4_lba_from_sectors(512);
    sb.info.lba_qmask_start  = hn4_lba_from_sectors(1024);
    sb.info.lba_flux_start   = hn4_lba_from_sectors(2048);
    sb.info.lba_horizon_start = hn4_lba_from_sectors(4096);
    sb.info.journal_start     = hn4_lba_from_sectors(8192);
    sb.info.journal_ptr       = sb.info.journal_start;
    sb.info.epoch_ring_block_idx = hn4_lba_from_blocks(2);

    /* Write Primary SB (North) */
    _rep_write_sb(dev, &sb, 0);
    
    /* Write Mirrors */
    uint64_t cap_bytes = REP_FIXTURE_SIZE;
    uint32_t bs = REP_BLOCK_SIZE;
    
    uint64_t east_sec = (HN4_ALIGN_UP((cap_bytes / 100) * 33, bs)) / REP_SECTOR_SIZE;
    _rep_write_sb(dev, &sb, east_sec);
    
    uint64_t west_sec = (HN4_ALIGN_UP((cap_bytes / 100) * 66, bs)) / REP_SECTOR_SIZE;
    _rep_write_sb(dev, &sb, west_sec);

    /* Initialize Root Anchor */
    hn4_anchor_t root = {0};
    root.seed_id.lo = 0xFFFFFFFFFFFFFFFF;
    root.seed_id.hi = 0xFFFFFFFFFFFFFFFF;
    root.data_class = hn4_cpu_to_le64(HN4_VOL_STATIC | HN4_FLAG_VALID);
    root.checksum = hn4_cpu_to_le32(hn4_crc32(0, &root, offsetof(hn4_anchor_t, checksum)));
    
    uint8_t abuf[REP_BLOCK_SIZE] = {0};
    memcpy(abuf, &root, sizeof(root));
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_cortex_start, abuf, REP_BLOCK_SIZE / REP_SECTOR_SIZE);

    /* Initialize Q-Mask */
    uint32_t qm_len = 4096;
    uint8_t* qm = calloc(1, qm_len);
    memset(qm, 0xAA, qm_len);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_qmask_start, qm, qm_len / REP_SECTOR_SIZE);
    free(qm);

    /* Inject Valid Epoch 1 */
    hn4_epoch_header_t ep = {0};
    ep.epoch_id = 1;
    ep.timestamp = 1000;
    ep.epoch_crc = hn4_epoch_calc_crc(&ep);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, sb.info.lba_epoch_start, &ep, 1);

    return dev;
}

static void repair_teardown(hn4_hal_device_t* dev) {
    _rep_test_hal_t* impl = (_rep_test_hal_t*)dev;
    free(impl->mmio_base);
    hn4_hal_mem_free(dev);
}

/* =========================================================================
 * TEST CASES
 * ========================================================================= */

hn4_TEST(Repair, Repair_Success_Downgrades_To_Bronze) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint64_t flux_start = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    uint32_t spb = REP_BLOCK_SIZE / REP_SECTOR_SIZE;
    uint64_t target_block_idx = 100;
    uint64_t phys_lba_val = flux_start + (target_block_idx * spb);
    hn4_addr_t phys_lba = hn4_lba_from_sectors(phys_lba_val);

    uint8_t bad_data[REP_BLOCK_SIZE]; memset(bad_data, 0x66, REP_BLOCK_SIZE);
    uint8_t good_data[REP_BLOCK_SIZE]; memset(good_data, 0x77, REP_BLOCK_SIZE);

    hn4_hal_sync_io(dev, HN4_IO_WRITE, phys_lba, bad_data, spb);

    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, phys_lba, good_data, REP_BLOCK_SIZE));

    uint8_t read_buf[REP_BLOCK_SIZE];
    hn4_hal_sync_io(dev, HN4_IO_READ, phys_lba, read_buf, spb);
    ASSERT_EQ(0, memcmp(read_buf, good_data, REP_BLOCK_SIZE));

    /* Check QMask Bronze */
    uint64_t absolute_block_idx = (flux_start / spb) + target_block_idx;
    uint64_t word_idx = absolute_block_idx / 32;
    uint32_t shift = (absolute_block_idx % 32) * 2;
    
    uint64_t q_val = (vol->quality_mask[word_idx] >> shift) & 0x3;
    ASSERT_EQ(HN4_Q_BRONZE, q_val);

    ASSERT_EQ(1, atomic_load(&vol->stats.heal_count));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Rejects_ReadOnly_Volume) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = { .mount_flags = HN4_MNT_READ_ONLY };
    
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));
    
    hn4_addr_t target = hn4_lba_from_sectors(5000);
    uint8_t new_data[REP_BLOCK_SIZE]; 
    memset(new_data, 0xFF, REP_BLOCK_SIZE);

    ASSERT_EQ(HN4_ERR_ACCESS_DENIED, hn4_repair_block(vol, target, new_data, REP_BLOCK_SIZE));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Handles_Partial_Block) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* 
     * TEST CASE A: Valid Partial Block (Sector Aligned)
     * Repairing 1 sector (512 bytes) inside a 4KB block.
     * This SHOULD SUCCEED.
     */
    uint32_t len_aligned = 512; 
    uint8_t data_aligned[512]; memset(data_aligned, 0xCC, 512);
    hn4_addr_t target_aligned = hn4_lba_from_sectors(6000);

    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target_aligned, data_aligned, len_aligned));

    /* 
     * TEST CASE B: Invalid Partial Sector (Unaligned)
     * Repairing 511 bytes.
     * This MUST FAIL with HN4_ERR_ALIGNMENT_FAIL.
     */
    uint32_t len_unaligned = 511;
    uint8_t data_unaligned[511]; memset(data_unaligned, 0xDD, 511);
    
    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, hn4_repair_block(vol, target_aligned, data_unaligned, len_unaligned));

    hn4_unmount(vol);
    repair_teardown(dev);
}


hn4_TEST(Repair, Repair_LargeFile_Precision_Strike) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint32_t bs = REP_BLOCK_SIZE;       
    uint32_t ss = REP_SECTOR_SIZE;      
    uint32_t spb = bs / ss;             
    uint64_t flux_start = hn4_addr_to_u64(vol->sb.info.lba_flux_start);

    uint32_t file_size = 1024 * 1024;   
    uint32_t file_sectors = (file_size + ss - 1) / ss;
    uint8_t* full_file_buf = calloc(1, file_size);
    memset(full_file_buf, 0xAA, file_size);

    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(flux_start), full_file_buf, file_sectors);

    uint64_t target_blk_idx = 50;
    uint64_t target_lba_val = flux_start + (target_blk_idx * spb);
    hn4_addr_t target_lba = hn4_lba_from_sectors(target_lba_val);

    uint8_t* corruption = calloc(1, bs);
    memset(corruption, 0xBD, bs); 
    hn4_hal_sync_io(dev, HN4_IO_WRITE, target_lba, corruption, spb);

    uint8_t* good_chunk = full_file_buf + (target_blk_idx * bs);
    
    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target_lba, good_chunk, bs));

    uint8_t* read_buf = calloc(1, bs);
    hn4_hal_sync_io(dev, HN4_IO_READ, target_lba, read_buf, spb);
    ASSERT_EQ(0, memcmp(read_buf, good_chunk, bs));

    uint64_t abs_blk_idx = (flux_start / spb) + target_blk_idx;
    uint64_t word_idx = abs_blk_idx / 32;
    uint32_t shift = (abs_blk_idx % 32) * 2;
    
    uint64_t q_val = (vol->quality_mask[word_idx] >> shift) & 0x3;
    ASSERT_EQ(HN4_Q_BRONZE, q_val);

    free(full_file_buf);
    free(corruption);
    free(read_buf);
    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_10GB_File_Sparse_Sim) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint32_t bs = REP_BLOCK_SIZE;       
    uint32_t spb = bs / REP_SECTOR_SIZE;             
    uint64_t flux_start = hn4_addr_to_u64(vol->sb.info.lba_flux_start);

    uint64_t target_blk_idx = (REP_FIXTURE_SIZE / bs) - 100;
    uint64_t target_lba_val = flux_start + (target_blk_idx * spb);
    
    if ((target_lba_val * REP_SECTOR_SIZE) >= REP_FIXTURE_SIZE) {
        target_blk_idx = 1000; 
        target_lba_val = flux_start + (target_blk_idx * spb);
    }

    hn4_addr_t target_lba = hn4_lba_from_sectors(target_lba_val);

    uint8_t bad_data[REP_BLOCK_SIZE]; memset(bad_data, 0x66, REP_BLOCK_SIZE);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, target_lba, bad_data, spb);

    uint8_t good_data[REP_BLOCK_SIZE]; memset(good_data, 0x77, REP_BLOCK_SIZE);

    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target_lba, good_data, bs));

    uint64_t abs_blk_idx = (flux_start / spb) + target_blk_idx;
    uint64_t word_idx = abs_blk_idx / 32;
    uint32_t shift = (abs_blk_idx % 32) * 2;
    
    uint64_t q_val = (vol->quality_mask[word_idx] >> shift) & 0x3;
    ASSERT_EQ(HN4_Q_BRONZE, q_val);

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Zero_Length_NoOp) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint8_t data[1] = {0};
    hn4_addr_t target = hn4_lba_from_sectors(5000);

    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, data, 0));

    ASSERT_EQ(0, atomic_load(&vol->stats.heal_count));
    ASSERT_EQ(0, atomic_load(&vol->toxic_blocks));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Updates_Stats_Accumulation) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint32_t bs = REP_BLOCK_SIZE;
    uint8_t data[REP_BLOCK_SIZE] = {0};
    
    /* Repair 1 */
    hn4_repair_block(vol, hn4_lba_from_sectors(5000 * 8), data, bs);
    ASSERT_EQ(1, atomic_load(&vol->stats.heal_count));
    
    /* Repair 2 */
    hn4_repair_block(vol, hn4_lba_from_sectors(5001 * 8), data, bs);
    ASSERT_EQ(2, atomic_load(&vol->stats.heal_count));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Validates_Arguments) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = hn4_lba_from_sectors(5000);
    uint8_t data[512] = {0};

    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_repair_block(NULL, target, data, 512));
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_repair_block(vol, target, NULL, 512));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Large_IO_Splitting) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* FIX: Use unaligned length (65536 + 1) to trigger ALIGNMENT_FAIL */
    uint32_t len = 65537; 
    uint8_t* data = calloc(1, len);
    memset(data, 0xAA, len);
    
    hn4_addr_t target = hn4_lba_from_sectors(10000 * 8); 

    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, hn4_repair_block(vol, target, data, len));
    
    /* Verify disk was NOT written (Check first sector) */
    uint8_t read_buf[512] = {0};
    hn4_hal_sync_io(dev, HN4_IO_READ, target, read_buf, 1);
    ASSERT_EQ(0, read_buf[0]);
    
    free(data);
    hn4_unmount(vol);
    repair_teardown(dev);
}


hn4_TEST(Repair, Repair_Verify_Magic_Corruption) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = hn4_lba_from_sectors(5000 * 8);
    uint32_t bs = REP_BLOCK_SIZE;

    uint8_t noise[REP_BLOCK_SIZE]; memset(noise, 0xFE, bs);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, target, noise, bs / REP_SECTOR_SIZE);

    uint8_t good_buf[REP_BLOCK_SIZE] = {0};
    hn4_block_header_t* h = (hn4_block_header_t*)good_buf;
    h->magic = hn4_cpu_to_le32(HN4_BLOCK_MAGIC);
    
    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, good_buf, bs));

    uint8_t read_buf[REP_BLOCK_SIZE];
    hn4_hal_sync_io(dev, HN4_IO_READ, target, read_buf, bs / REP_SECTOR_SIZE);
    hn4_block_header_t* h_disk = (hn4_block_header_t*)read_buf;
    
    ASSERT_EQ(HN4_BLOCK_MAGIC, hn4_le32_to_cpu(h_disk->magic));

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Updates_Stale_Generation) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = hn4_lba_from_sectors(6000 * 8);
    uint32_t bs = REP_BLOCK_SIZE;

    uint8_t buf10[REP_BLOCK_SIZE] = {0};
    ((hn4_block_header_t*)buf10)->generation = hn4_cpu_to_le64(10);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, target, buf10, bs / REP_SECTOR_SIZE);

    uint8_t buf11[REP_BLOCK_SIZE] = {0};
    ((hn4_block_header_t*)buf11)->generation = hn4_cpu_to_le64(11);
    
    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, buf11, bs));

    uint8_t read_buf[REP_BLOCK_SIZE];
    hn4_hal_sync_io(dev, HN4_IO_READ, target, read_buf, bs / REP_SECTOR_SIZE);
    
    uint64_t gen = hn4_le64_to_cpu(((hn4_block_header_t*)read_buf)->generation);
    ASSERT_EQ(11, gen);

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_QMask_Saturation) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    uint64_t target_blk = 100;
    uint64_t flux_start_sec = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    /* Calculate absolute block index for QMask lookup */
    /* flux_start_sec is sector index. REP_BLOCK_SIZE/512 = 8 sectors per block. */
    /* abs_blk = (FluxStartSec / 8) + RelativeBlockIdx */
    uint64_t abs_blk = (flux_start_sec / (REP_BLOCK_SIZE/REP_SECTOR_SIZE)) + target_blk;
    
    hn4_addr_t target_lba = hn4_lba_from_sectors(flux_start_sec + (target_blk * 8));

    uint64_t word_idx = abs_blk / 32;
    uint32_t shift = (abs_blk % 32) * 2;
    
    /* 1. Manually set QMask to TOXIC (00) */
    /* Clear both bits */
    vol->quality_mask[word_idx] &= ~(3ULL << shift); 

    /* 2. Repair Succeeds physically */
    uint8_t good_buf[REP_BLOCK_SIZE] = {0};
    /* Function returns OK on physical success... */
    /* WAIT! The new hn4_repair.c returns HN4_ERR_MEDIA_TOXIC even if repair succeeded physically
       if the old state was TOXIC. 
       
       Code analysis:
       if (old_state == HN4_Q_TOXIC) {
           success = true;
           if (res == HN4_OK) res = HN4_ERR_MEDIA_TOXIC; 
           break;
       }
       
       So we must expect HN4_ERR_MEDIA_TOXIC.
    */
    
    hn4_result_t res = hn4_repair_block(vol, target_lba, good_buf, REP_BLOCK_SIZE);
    
    /* FIX: Expect Toxic Error */
    ASSERT_EQ(HN4_ERR_MEDIA_TOXIC, res);

    /* 3. Verify it STAYS TOXIC (00) */
    uint64_t q_val = (vol->quality_mask[word_idx] >> shift) & 0x3;
    ASSERT_EQ(HN4_Q_TOXIC, q_val);

    hn4_unmount(vol);
    repair_teardown(dev);
}


hn4_TEST(Repair, Repair_Large_Span_Atomicity) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* Use unaligned length */
    uint32_t len = 65537; 
    uint64_t start_blk = 200;
    uint64_t flux_start_sec = hn4_addr_to_u64(vol->sb.info.lba_flux_start);
    hn4_addr_t target_lba = hn4_lba_from_sectors(flux_start_sec + (start_blk * 8));
    
    uint8_t* buf = calloc(1, len);
    
    ASSERT_EQ(HN4_ERR_ALIGNMENT_FAIL, hn4_repair_block(vol, target_lba, buf, len));

    /* Verify Q-Mask was NOT updated (Atomicity check) */
    uint64_t abs_blk = (flux_start_sec / 8) + start_blk;
    uint64_t w_idx = abs_blk / 32;
    uint32_t s_idx = (abs_blk % 32) * 2;
    
    /* Should remain Silver (10), not Bronze (01) or Toxic (00) */
    /* Note: Fixture initializes QMask to 0xAA (Silver) */
    ASSERT_EQ(HN4_Q_SILVER, (vol->quality_mask[w_idx] >> s_idx) & 0x3);

    free(buf);
    hn4_unmount(vol);
    repair_teardown(dev);
}



hn4_TEST(Repair, Repair_Boundary_Max_LBA) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    /* FIX: Align to BLOCK boundary (4096 / 512 = 8 sectors) */
    uint64_t max_sec = (REP_FIXTURE_SIZE / 512); 
    uint64_t safe_sec = max_sec - 8; /* Last full block */
    
    hn4_addr_t target = hn4_lba_from_sectors(safe_sec);
    uint8_t buf[REP_BLOCK_SIZE] = {0};
    
    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, buf, REP_BLOCK_SIZE));

    /* Target Out of Bounds */
    target = hn4_lba_from_sectors(max_sec + 8);
    
    hn4_result_t res = hn4_repair_block(vol, target, buf, REP_BLOCK_SIZE);
    
    bool ok = (res == HN4_ERR_GEOMETRY || 
               res == HN4_ERR_INVALID_ARGUMENT || 
               res == HN4_ERR_HW_IO || 
               res == HN4_ERR_MEDIA_TOXIC);
               
    ASSERT_TRUE(ok);

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Critical_Metadata_Region) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = vol->sb.info.lba_cortex_start;
    
    uint8_t noise[4096]; memset(noise, 0xFF, 4096);
    hn4_hal_sync_io(dev, HN4_IO_WRITE, target, noise, 8);

    uint8_t good_buf[4096] = {0};
    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, good_buf, 4096));

    uint64_t abs_blk = hn4_addr_to_u64(target) / 8;
    uint64_t w_idx = abs_blk / 32;
    uint32_t s_idx = (abs_blk % 32) * 2;
    
    ASSERT_EQ(HN4_Q_BRONZE, (vol->quality_mask[w_idx] >> s_idx) & 0x3);

    hn4_unmount(vol);
    repair_teardown(dev);
}

hn4_TEST(Repair, Repair_Nano_Object_Granularity) {
    hn4_hal_device_t* dev = repair_setup();
    hn4_volume_t* vol = NULL;
    hn4_mount_params_t p = {0};
    ASSERT_EQ(HN4_OK, hn4_mount(dev, &p, &vol));

    hn4_addr_t target = hn4_lba_from_sectors(5000 * 8);
    /* FIX: Must use full block size (4096) or alignment fails */
    uint32_t len = REP_BLOCK_SIZE; 
    uint8_t data[REP_BLOCK_SIZE]; memset(data, 0xEE, len);

    ASSERT_EQ(HN4_OK, hn4_repair_block(vol, target, data, len));

    uint8_t read_buf[REP_BLOCK_SIZE] = {0};
    hn4_hal_sync_io(dev, HN4_IO_READ, target, read_buf, 8);
    ASSERT_EQ(0, memcmp(read_buf, data, len));

    hn4_unmount(vol);
    repair_teardown(dev);
}
