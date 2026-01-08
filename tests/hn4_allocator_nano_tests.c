/*
 * HYDRA-NEXUS 4 (HN4) - NANO-LATTICE ALLOCATOR SUITE
 * FILE: hn4_allocator_nano_tests.c
 * STATUS: FIXED / ALIGNED WITH V1.1 IMPLEMENTATION
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_endians.h"
#include "hn4_crc.h" // Ensure we can calc CRC in tests
#include <string.h>

/* --- FIXTURE --- */
#define FIXTURE_BS 8192 
#define RAM_DISK_SIZE (1024 * 1024) /* 1MB */

static uint8_t g_ram_disk[RAM_DISK_SIZE]; 

typedef struct { 
    hn4_hal_caps_t caps; 
    uint8_t* mmio_base; 
    void* driver_ctx; 
} mock_dev_t;

static hn4_volume_t* create_nano_fixture(void) {
    /* Initialize HAL first to ensure atomic/CRC tables are ready */
    hn4_hal_init();

    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    vol->vol_block_size = FIXTURE_BS;
    vol->vol_capacity_bytes = RAM_DISK_SIZE; 
    
    vol->sb.info.lba_cortex_start = hn4_addr_from_u64(0);
    vol->sb.info.lba_bitmap_start = hn4_addr_from_u64((RAM_DISK_SIZE / 128) - 1); 
    
    memset(g_ram_disk, 0, RAM_DISK_SIZE);
    
    mock_dev_t* dev = hn4_hal_mem_alloc(sizeof(mock_dev_t));
    dev->caps.logical_block_size = 128; /* Align sectors to slots */
    dev->caps.total_capacity_bytes = hn4_addr_from_u64(RAM_DISK_SIZE);
    dev->caps.hw_flags = HN4_HW_NVM; 
    dev->mmio_base = g_ram_disk;
    
    vol->target_device = dev;
    return vol;
}

static void cleanup_nano_fixture(hn4_volume_t* vol) {
    if (vol) {
        hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: EFFICIENCY PROOF
 * ========================================================================= */
hn4_TEST(NanoLattice, Efficiency_3KB_NoBlock) {
    hn4_volume_t* vol = create_nano_fixture();
    
    uint32_t len = 3072;
    void* data = hn4_hal_mem_alloc(len);
    memset(data, 0xAA, len);
    
    hn4_anchor_t anchor = {0};
    
    hn4_result_t res = hn4_alloc_nano(vol, &anchor, data, len);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify Anchor State */
    uint64_t dclass = hn4_le64_to_cpu(anchor.data_class);
    ASSERT_TRUE((dclass & HN4_FLAG_NANO) != 0);
    
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    ASSERT_EQ(0ULL, slot);
    
    hn4_hal_mem_free(data);
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 2: CONTIGUITY SCAN
 * ========================================================================= */
hn4_TEST(NanoLattice, Find_Free_Run) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 
     * Payload: 100 bytes.
     * Header: 32 bytes.
     * Total: 132 bytes.
     * Slot Size: 128 bytes.
     * Slots Needed: 132 / 128 = 1 remainder 4 -> 2 Slots.
     */
    uint32_t len = 100;
    uint8_t data[100] = {0};
    
    hn4_anchor_t a1 = {0};
    hn4_anchor_t a2 = {0};
    
    /* Alloc 1: Takes Slot 0 and Slot 1 */
    hn4_alloc_nano(vol, &a1, data, len);
    
    /* Alloc 2: Should scan past 0, 1 and take Slot 2 */
    hn4_alloc_nano(vol, &a2, data, len);
    
    uint64_t s1 = hn4_le64_to_cpu(a1.gravity_center);
    uint64_t s2 = hn4_le64_to_cpu(a2.gravity_center);
    
    ASSERT_EQ(0ULL, s1);
    
    /* 
     * CORRECTED ASSERTION:
     * Previous failure was expecting 1. 
     * Actual physics dictates 2.
     */
    ASSERT_EQ(2ULL, s2);
    
    cleanup_nano_fixture(vol);
}


/* =========================================================================
 * TEST 3: MAX SIZE CAP
 * ========================================================================= */
hn4_TEST(NanoLattice, Max_Size_Cap) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* Cap is 16KB (16384). Request 17000. */
    uint32_t len = 17000;
    void* data = hn4_hal_mem_alloc(len);
    
    hn4_anchor_t anchor = {0};
    hn4_result_t res = hn4_alloc_nano(vol, &anchor, data, len);
    
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, res);
    
    hn4_hal_mem_free(data);
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 4: FRAGMENTATION SCAN
 * ========================================================================= */
hn4_TEST(NanoLattice, Fragmentation_Gap_Search) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* Small Data: 50 bytes. Total 82 bytes. Fits in 1 Slot. */
    uint8_t data[50] = {0}; 
    
    hn4_anchor_t a1 = {0}, a2 = {0}, a3 = {0}, a4 = {0};
    
    /* 1. Allocate 3 slots (0, 1, 2) */
    hn4_alloc_nano(vol, &a1, data, 50); /* Slot 0 */
    hn4_alloc_nano(vol, &a2, data, 50); /* Slot 1 */
    hn4_alloc_nano(vol, &a3, data, 50); /* Slot 2 */
    
    /* 2. Free Slot 1 manually (Simulate Delete) */
    /* Must zero the header area to be detected as free */
    memset(g_ram_disk + 128, 0, 128);
    
    /* 
     * 3. Allocate a 2-slot object 
     * Needs 2 slots contiguous. 
     * Slot 0 used. Slot 1 free. Slot 2 used.
     * Cannot fit in 1. Must jump to 3.
     */
    uint8_t large_data[200] = {0}; /* 200 + 32 = 232 bytes. 2 Slots. */
    hn4_alloc_nano(vol, &a4, large_data, 200);
    
    uint64_t slot = hn4_le64_to_cpu(a4.gravity_center);
    
    /* Expect Slot 3 */
    ASSERT_EQ(3ULL, slot);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 5: EXACT BOUNDARY
 * ========================================================================= */
hn4_TEST(NanoLattice, Exact_Slot_Boundary) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 
     * Payload: 224 bytes.
     * Header: 32 bytes.
     * Total: 256 bytes.
     * Slots: 256 / 128 = 2.0 -> Exactly 2 slots.
     */
    uint32_t len = 224; 
    uint8_t* data = hn4_hal_mem_alloc(len);
    memset(data, 0x55, len);
    
    hn4_anchor_t anchor = {0};
    hn4_alloc_nano(vol, &anchor, data, len);
    
    /* Alloc next object (Small, 1 slot) */
    hn4_anchor_t probe = {0};
    uint8_t tiny[10] = {0};
    hn4_alloc_nano(vol, &probe, tiny, 10);
    
    uint64_t slot_probe = hn4_le64_to_cpu(probe.gravity_center);
    
    /* Should be Slot 2 (after 0 and 1) */
    ASSERT_EQ(2ULL, slot_probe);
    
    hn4_hal_mem_free(data);
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 6: SATURATION
 * ========================================================================= */
hn4_TEST(NanoLattice, Saturation_Behavior) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* Fill RAM with garbage so no free runs exist */
    memset(g_ram_disk, 0xFF, RAM_DISK_SIZE);
    
    uint8_t data[10] = {0};
    hn4_anchor_t anchor = {0};
    
    hn4_result_t res = hn4_alloc_nano(vol, &anchor, data, 10);
    
    /* Should fail */
    ASSERT_EQ(HN4_ERR_ENOSPC, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 7: INTEGRITY CRC
 * ========================================================================= */
hn4_TEST(NanoLattice, Integrity_CRC_Generation) {
    hn4_volume_t* vol = create_nano_fixture();
    
    char* text = "Hello Cortex";
    uint32_t len = strlen(text);
    
    hn4_anchor_t anchor = {0};
    hn4_alloc_nano(vol, &anchor, text, len);
    
    /* 
     * Structure: [Magic 4][H_CRC 4][Len 8][Ver 8][D_CRC 4]...
     * Data CRC is at offset 24 (0x18).
     */
    uint32_t stored_crc;
    memcpy(&stored_crc, g_ram_disk + 24, 4); 
    stored_crc = hn4_le32_to_cpu(stored_crc);
    
    /* Calculate Expected */
    uint32_t expected = hn4_crc32(0, text, len);
    
    ASSERT_EQ(expected, stored_crc);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 8: LARGE RUN
 * ========================================================================= */
hn4_TEST(NanoLattice, Large_Contiguous_Run) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 
     * 10KB = 10240 bytes.
     * + 32 Header = 10272.
     * / 128 = 80.25 -> 81 Slots.
     */
    uint32_t len = 10240;
    void* data = hn4_hal_mem_alloc(len);
    memset(data, 0xCC, len);
    
    /* Occupy Slot 0 */
    memset(g_ram_disk, 0xFF, 128); 
    
    hn4_anchor_t anchor = {0};
    hn4_result_t res = hn4_alloc_nano(vol, &anchor, data, len);
    
    ASSERT_EQ(HN4_OK, res);
    
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    
    /* Must skip Slot 0 */
    ASSERT_TRUE(slot > 0);
    
    /* Verify end byte */
    /* Offset = (Slot * 128) + 32 (Header) + len - 1 */
    uint64_t end_offset = (slot * 128) + 32 + len - 1;
    ASSERT_EQ(0xCC, g_ram_disk[end_offset]);
    
    hn4_hal_mem_free(data);
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST N-FIX-1: Cortex Dirty Tail Detection
 * RATIONALE:
 * Verify `_alloc_cortex_run` checks the FULL 128 bytes.
 * We dirty byte 64 of Slot 0 in `g_ram_disk`. Allocator must skip it.
 * ========================================================================= */
hn4_TEST(NanoFixes, Tail_Dirty_Prevents_Alloc) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 1. Manually dirty Slot 0 at byte 64 (Tail) */
    /* Slot 0 is at offset 0 of the RAM disk */
    g_ram_disk[64] = 0xFF; 
    
    /* 2. Attempt Alloc */
    hn4_anchor_t anchor = {0};
    uint8_t data[10] = {0};
    hn4_result_t res = hn4_alloc_nano(vol, &anchor, data, 10);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* 3. Verify Slot Index */
    /* Must skip Slot 0 (Dirty) and take Slot 1 */
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    ASSERT_EQ(1ULL, slot);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST N-FIX-2: Pending Reservation Check
 * RATIONALE:
 * Verify allocator respects "PNDG" magic marker (Reservation in progress).
 * We manually write the magic to Slot 0. Allocator must skip it.
 * ========================================================================= */
hn4_TEST(NanoFixes, Reservation_Respects_Pending) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 1. Write PNDG Magic to Slot 0 */
    uint32_t magic = 0x504E4447; /* "PNDG" */
    /* Little Endian write to RAM disk */
    memcpy(g_ram_disk, &magic, 4);
    
    /* 2. Alloc */
    hn4_anchor_t anchor = {0};
    uint8_t data[10] = {0};
    hn4_alloc_nano(vol, &anchor, data, 10);
    
    /* 3. Verify Slot Index */
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    ASSERT_EQ(1ULL, slot);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST N-FIX-3: Two-Phase Commit Flag
 * RATIONALE:
 * Verify that a successful allocation sets the COMMITTED flag (Bit 0) 
 * in the on-disk header.
 * ========================================================================= */
hn4_TEST(NanoFixes, Commit_Flag_Persisted) {
    hn4_volume_t* vol = create_nano_fixture();
    
    hn4_anchor_t anchor = {0};
    uint8_t data[10] = {0xAA};
    
    hn4_alloc_nano(vol, &anchor, data, 10);
    
    /* 1. Get Slot Index */
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    
    /* 2. Read Header from RAM Disk */
    /* Header is at offset: slot * 128 */
    /* HN4_CORTEX_SLOT_SIZE is 128 */
    uint64_t offset = slot * 128;
    
    /* Structure layout: Magic(4), HCrc(4), Len(8), Ver(8), DCrc(4), Flags(4) */
    /* Flags is at offset 28 (0x1C) */
    uint32_t flags;
    memcpy(&flags, g_ram_disk + offset + 28, 4);
    flags = hn4_le32_to_cpu(flags);
    
    /* 3. Verify Bit 0 Set */
    ASSERT_TRUE((flags & 1) != 0);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST N-FIX-4: Version Monotonicity
 * RATIONALE:
 * Verify Nano Object inherits version from Anchor + 1.
 * ========================================================================= */
hn4_TEST(NanoFixes, Version_Inheritance) {
    hn4_volume_t* vol = create_nano_fixture();
    
    /* 1. Setup Anchor with specific Gen */
    hn4_anchor_t anchor = {0};
    anchor.write_gen = hn4_cpu_to_le32(99);
    
    /* 2. Alloc */
    uint8_t data[10] = {0};
    hn4_alloc_nano(vol, &anchor, data, 10);
    
    /* 3. Inspect Disk Header */
    uint64_t slot = hn4_le64_to_cpu(anchor.gravity_center);
    uint64_t offset = slot * 128;
    
    /* Version is at offset 16 (0x10) */
    uint64_t ver;
    memcpy(&ver, g_ram_disk + offset + 16, 8);
    ver = hn4_le64_to_cpu(ver);
    
    /* 4. Verify Increment */
    ASSERT_EQ(100ULL, ver);
    
    /* Verify Anchor updated in RAM */
    ASSERT_EQ(100UL, hn4_le32_to_cpu(anchor.write_gen));
    
    cleanup_nano_fixture(vol);
}
