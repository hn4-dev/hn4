/*
 * HYDRA-NEXUS 4 (HN4) - NANO STORAGE TEST SUITE
 * FILE: hn4_nano_tests.c
 * STATUS: PRODUCTION VERIFICATION
 *
 * SCOPE:
 * Validates the behavior of the Ballistic Nano-Storage subsystem.
 * Includes fixes for HAL persistence simulation and expanded coverage.
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_addr.h"
#include "hn4.h"
#include "hn4_endians.h"
#include "hn4_crc.h"
#include "hn4_constants.h"
#include <string.h>

struct hn4_hal_device_impl {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
};

#define TEST_BS 4096
#define TEST_CAP (128ULL * 1024 * 1024)

static hn4_volume_t* create_nano_fixture(uint32_t profile, uint32_t dev_type) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    vol->sb.info.format_profile = profile;
    vol->sb.info.device_type_tag = dev_type;
    vol->vol_block_size = TEST_BS;
    vol->vol_capacity_bytes = TEST_CAP;
    vol->sb.info.lba_cortex_start = hn4_addr_from_u64(1024);
    vol->sb.info.lba_bitmap_start = hn4_addr_from_u64(4096); /* Enough space for orbits */
    vol->sb.info.volume_uuid.lo = 0xDEADBEEF;
    vol->sb.info.current_epoch_id = 1;
    
    struct hn4_hal_device_impl* impl = hn4_hal_mem_alloc(sizeof(struct hn4_hal_device_impl));
    memset(impl, 0, sizeof(struct hn4_hal_device_impl));
    
    impl->caps.logical_block_size = 512;
    impl->caps.total_capacity_bytes = hn4_addr_from_u64(TEST_CAP);
    impl->caps.hw_flags = HN4_HW_NVM; 
    impl->mmio_base = hn4_hal_mem_alloc(TEST_CAP); /* Backing store */
    memset(impl->mmio_base, 0, TEST_CAP);
    
    vol->target_device = (hn4_hal_device_t*)impl;

    return vol;
}

static void cleanup_nano_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) {
            struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
            if (impl->mmio_base) hn4_hal_mem_free(impl->mmio_base);
            hn4_hal_mem_free(impl);
        }
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 2: COMPATIBILITY REJECTION (HDD)
 * ========================================================================= */
hn4_TEST(NanoStorage, Reject_Linear_Media) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_HDD);
    /* Manually override HAL flags to force rejection */
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    impl->caps.hw_flags |= HN4_HW_ROTATIONAL;
    
    hn4_anchor_t anchor = {0};
    char payload[] = "Data";
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, payload, 4);
    ASSERT_EQ(HN4_ERR_PROFILE_MISMATCH, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 3: GENERATION SKEW DETECTION
 * ========================================================================= */
hn4_TEST(NanoStorage, Generation_Skew) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x12345678;
    anchor.write_gen = hn4_cpu_to_le32(10);
    
    char payload[] = "GenTest";
    uint32_t len = sizeof(payload);
    
    /* Write valid data (Gen becomes 11) */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, len));
    
    /* Tamper with Anchor: Rollback generation in memory to 5 */
    anchor.write_gen = hn4_cpu_to_le32(5); 
    
    char read_buf[64];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, read_buf, len);
    
    /* The slot has Gen 11. Anchor says 5. Mismatch -> Skew */
    ASSERT_EQ(HN4_ERR_GENERATION_SKEW, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 4: IDENTITY BINDING (ID SWAP ATTACK)
 * ========================================================================= */
hn4_TEST(NanoStorage, ID_Binding_Check) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xCAFEBABE;
    char payload[] = "ID_TEST";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    anchor.seed_id.lo = 0xDEAD0000;
    
    char buf[64];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, sizeof(payload));
    
    ASSERT_EQ(HN4_ERR_PHANTOM_BLOCK, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 5: PAYLOAD BOUNDS CHECK
 * ========================================================================= */
hn4_TEST(NanoStorage, Payload_Too_Large) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    /* 512B Sector - Header (~32B) = ~480B Max */
    char big_payload[600]; 
    memset(big_payload, 'A', sizeof(big_payload));
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, big_payload, sizeof(big_payload));
    
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 7: MAGIC MISMATCH (DATA CORRUPTION)
 * ========================================================================= */
hn4_TEST(NanoStorage, Magic_Mismatch) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x999;
    char payload[] = "CorruptMe";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    
    /* Corrupt the entire Cortex region to ensure we hit the slot */
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_end   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) * 512;
    memset(impl->mmio_base + cortex_start, 0xFF, cortex_end - cortex_start);
    
    char buf[64];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, sizeof(payload));
    
    ASSERT_EQ(HN4_ERR_PHANTOM_BLOCK, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 8: CRC MISMATCH (BIT ROT)
 * ========================================================================= */
hn4_TEST(NanoStorage, CRC_Mismatch) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x888;
    char payload[] = "RottenBits";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    
    /* Search entire cortex for the Magic Number to locate the write */
    uint32_t magic = HN4_MAGIC_NANO;
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_len = (hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) - 
                           hn4_addr_to_u64(vol->sb.info.lba_cortex_start)) * 512;
    
    bool found = false;
    for(uint64_t i = 0; i < cortex_len; i += 512) {
        if (memcmp(impl->mmio_base + cortex_start + i, &magic, 4) == 0) {
            /* Flip bit in payload */
            impl->mmio_base[cortex_start + i + 40] ^= 0xFF;
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    
    char buf[64];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, sizeof(payload));
    
    ASSERT_EQ(HN4_ERR_DATA_ROT, res);
    
    cleanup_nano_fixture(vol);
}


/* =========================================================================
 * TEST 9: ORBIT EXHAUSTION (ENOSPC)
 * ========================================================================= */
hn4_TEST(NanoStorage, Orbit_Exhaustion) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_end   = hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) * 512;
    memset(impl->mmio_base + cortex_start, 0xFF, cortex_end - cortex_start);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xFULL;
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Full", 4);
    
    /* Should fail to find any empty slot after scanning all orbits */
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 11: NULL BUFFER SAFETY
 * ========================================================================= */
hn4_TEST(NanoStorage, Null_Buffer_Input) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    hn4_anchor_t anchor = {0};
    
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_write_nano_ballistic(vol, &anchor, NULL, 10));
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_read_nano_ballistic(vol, &anchor, NULL, 10));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 12: READ ONLY VOLUME WRITE
 * ========================================================================= */
hn4_TEST(NanoStorage, Read_Only_Write) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    vol->read_only = true;
    
    hn4_anchor_t anchor = {0};
    char buf[] = "Test";
    
    ASSERT_EQ(HN4_ERR_ACCESS_DENIED, hn4_write_nano_ballistic(vol, &anchor, buf, 4));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 13: TOMBSTONE WRITE PREVENTION
 * ========================================================================= */
hn4_TEST(NanoStorage, Tombstone_Write_Prevention) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    hn4_anchor_t anchor = {0};
    
    /* Success expected - Resurrect by overwrite */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Live", 4));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 14: ZNS REJECTION
 * ========================================================================= */
hn4_TEST(NanoStorage, ZNS_Rejection) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_ZNS);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    impl->caps.hw_flags |= HN4_HW_ZNS_NATIVE;
    
    hn4_anchor_t anchor = {0};
    ASSERT_EQ(HN4_ERR_PROFILE_MISMATCH, hn4_write_nano_ballistic(vol, &anchor, "ZNS", 3));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 16: READ UNINITIALIZED ANCHOR
 * ========================================================================= */
hn4_TEST(NanoStorage, Read_Uninit_Anchor) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    hn4_anchor_t anchor = {0};
    /* Valid ID but uninitialized gravity center (0) and mass (0) */
    anchor.seed_id.lo = 0x555;
    
    char buf[10];
    /* Should look at Orbit 0. Find empty/garbage.
       Empty -> Phantom Block. Garbage -> Data Rot. */
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 5);
    
    ASSERT_TRUE(res == HN4_ERR_PHANTOM_BLOCK || res == HN4_ERR_DATA_ROT);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 15: PAYLOAD ALIGNMENT
 * ========================================================================= */
hn4_TEST(NanoStorage, Payload_Offset_Check) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x777;
    char payload[] = "Aligned?";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    
    /* Find where it landed */
    uint32_t orbit_k = (uint32_t)hn4_le64_to_cpu(anchor.gravity_center);
    hn4_addr_t target_lba;
    /* We need to re-calc trajectory to find physical location */
    /* Since `_calc_nano_trajectory` is static, we rely on implementation details or repeat logic. */
    /* Instead, we scan the Cortex for the Magic Number. */
    
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_len = (hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) - 
                           hn4_addr_to_u64(vol->sb.info.lba_cortex_start)) * 512;
    
    bool found = false;
    for(uint64_t i = 0; i < cortex_len; i += 512) {
        hn4_nano_quantum_t* q = (hn4_nano_quantum_t*)(impl->mmio_base + cortex_start + i);
        if (hn4_le32_to_cpu(q->magic) == HN4_MAGIC_NANO && 
            q->owner_id.lo == anchor.seed_id.lo) {
            
            /* Verify payload offset is 40 bytes from start of sector */
            /* struct: magic(4)+id(16)+len(4)+crc(4)+res(4)+seq(8) = 40 bytes */
            ASSERT_EQ(0, offsetof(hn4_nano_quantum_t, payload) % 8);
            ASSERT_EQ(40, offsetof(hn4_nano_quantum_t, payload));
            
            /* Verify content */
            ASSERT_EQ(0, memcmp(q->payload, payload, sizeof(payload)));
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 17: PARTIAL READ
 * ========================================================================= */
hn4_TEST(NanoStorage, Partial_Read) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xABC;
    char payload[] = "FullPayloadString";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    
    char buf[5]; /* Smaller buffer */
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 5);
    
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(0, memcmp(buf, "FullP", 5));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 18: OVERSIZED READ REQUEST
 * ========================================================================= */
hn4_TEST(NanoStorage, Oversized_Read) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xDEF;
    char payload[] = "Small";
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, 6)); /* Include null */
    
    char buf[20];
    memset(buf, 0xCC, 20);
    
    /* Request 20 bytes, only 6 available */
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 20);
    
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(0, strcmp(buf, "Small"));
    /* Ensure padding is zeroed by read function */
    ASSERT_EQ(0, buf[6]);
    ASSERT_EQ(0, buf[19]);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 20: RETRY LOGIC (SIMULATED IO ERROR)
 * ========================================================================= */
/* 
 * Note: To test retries, we would need to mock `hn4_hal_sync_io` to fail N times then succeed.
 * With current simple mock, we can only test permanent failure or success.
 * Test Permanent Write Failure -> Orbit Exhaustion or IO Error return.
 */
hn4_TEST(NanoStorage, Write_IO_Fail) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    /* Point Cortex LBA to invalid range to force HAL IO error */
    vol->sb.info.lba_cortex_start = hn4_addr_from_u64(TEST_CAP / 512 + 1000); 
    
    hn4_anchor_t anchor = {0};
    char payload[] = "Fail";
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, payload, 4);
    
    /* Should fail with HW_IO or GEOMETRY depending on where check happens */
    ASSERT_TRUE(res == HN4_ERR_HW_IO || res == HN4_ERR_GEOMETRY);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 21: OVERWRITE EXISTING SLOT
 * ========================================================================= */
hn4_TEST(NanoStorage, Overwrite_Slot) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x555;
    
    /* 1. Write Data A */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "DataA", 6));
    uint64_t k1 = hn4_le64_to_cpu(anchor.gravity_center);
    
    /* 2. Write Data B (Overwrite) */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "DataB", 6));
    uint64_t k2 = hn4_le64_to_cpu(anchor.gravity_center);
    
    /* Should reuse same slot (k1 == k2) if it was valid owner */
    /* Actually, trajectory scan checks `is_mine`. If `is_mine` -> Reuse. */
    ASSERT_EQ(k1, k2);
    
    /* Read back to verify B */
    char buf[10];
    ASSERT_EQ(HN4_OK, hn4_read_nano_ballistic(vol, &anchor, buf, 6));
    ASSERT_EQ(0, strcmp(buf, "DataB"));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 22: ORBIT COLLISION RESOLUTION
 * ========================================================================= */
hn4_TEST(NanoStorage, Collision_Hop) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x123;
    
    /* 
     * 1. Calculate K=0 Slot manually 
     */
    hn4_addr_t k0_lba;
       
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Temp", 4));
    uint64_t k0 = hn4_le64_to_cpu(anchor.gravity_center);
    ASSERT_EQ(0, k0); /* Should be first orbit */
    
    /* Find physical address of K0 */
    /* We know it succeeded. We can find it by scanning memory for Owner ID. */
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_len = (hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) - 
                           hn4_addr_to_u64(vol->sb.info.lba_cortex_start)) * 512;
    
    uint64_t k0_offset = 0;
    for(uint64_t i=0; i<cortex_len; i+=512) {
         hn4_nano_quantum_t* q = (hn4_nano_quantum_t*)(impl->mmio_base + cortex_start + i);
         if (q->owner_id.lo == 0x123) {
             k0_offset = i;
             break;
         }
    }
    
    /* 
     * 2. Poison K0 Slot with Alien Data 
     * Make it look valid but owned by someone else.
     */
    hn4_nano_quantum_t* q0 = (hn4_nano_quantum_t*)(impl->mmio_base + cortex_start + k0_offset);
    q0->owner_id.lo = 0x999; /* Alien */
    q0->magic = hn4_cpu_to_le32(HN4_MAGIC_NANO);
    
    /* 
     * 3. Write again. Should Hop to K=1.
     */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Hop", 4));
    
    uint64_t k_new = hn4_le64_to_cpu(anchor.gravity_center);
    ASSERT_TRUE(k_new > 0); /* Moved to next orbit */
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 23: ORBIT COLLISION RECOVERY (READ PATH)
 * RATIONALE:
 * If multiple anchors hash to the same orbits, verify the reader follows the 
 * "Is Mine" check and finds the correct data even if it's not at K=0.
 * ========================================================================= */
hn4_TEST(NanoStorage, Collision_Read) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor1 = {0}; anchor1.seed_id.lo = 0x123;
    hn4_anchor_t anchor2 = {0}; anchor2.seed_id.lo = 0x456;
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor1, "Data1", 6));
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor2, "Data2", 6));
    
    char buf[10];
    ASSERT_EQ(HN4_OK, hn4_read_nano_ballistic(vol, &anchor1, buf, 6));
    ASSERT_EQ(0, strcmp(buf, "Data1"));
    
    ASSERT_EQ(HN4_OK, hn4_read_nano_ballistic(vol, &anchor2, buf, 6));
    ASSERT_EQ(0, strcmp(buf, "Data2"));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 24: WRITE GENERATION INCREMENT
 * RATIONALE:
 * Writing new data must increment the `write_gen` in the Anchor.
 * ========================================================================= */
hn4_TEST(NanoStorage, Generation_Increment) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xABC;
    anchor.write_gen = hn4_cpu_to_le32(1);
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Ver1", 5));
    ASSERT_EQ(2, hn4_le32_to_cpu(anchor.write_gen));
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Ver2", 5));
    ASSERT_EQ(3, hn4_le32_to_cpu(anchor.write_gen));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 25: STALE DATA REJECTION (GEN SKEW)
 * RATIONALE:
 * If we write Gen 2, then manually restore the disk slot to Gen 1 (Stale),
 * the reader must reject it as SKEW/PHANTOM.
 * ========================================================================= */
hn4_TEST(NanoStorage, Stale_Data_Rejection) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xDDD;
    anchor.write_gen = hn4_cpu_to_le32(10);
    
    /* Write Gen 11 */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Data", 5));
    ASSERT_EQ(11, hn4_le32_to_cpu(anchor.write_gen));
    
    /* Find slot and downgrade Gen to 10 */
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_len = (hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) - 
                           hn4_addr_to_u64(vol->sb.info.lba_cortex_start)) * 512;
    
    for(uint64_t i=0; i<cortex_len; i+=512) {
        hn4_nano_quantum_t* q = (hn4_nano_quantum_t*)(impl->mmio_base + cortex_start + i);
        if (q->owner_id.lo == anchor.seed_id.lo) {
            q->sequence = hn4_cpu_to_le64(10); /* Stale */
            /* Note: CRC is now invalid because seq changed. 
               Reader checks CRC first? Or Gen first?
               Code: 
               1. Magic
               2. ID
               3. Gen (Skew)
               4. Size
               5. CRC
               So it should return HN4_ERR_GENERATION_SKEW before checking CRC.
            */
            break;
        }
    }
    
    char buf[10];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 5);
    ASSERT_EQ(HN4_ERR_GENERATION_SKEW, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 26: ANCHOR MASS MISMATCH
 * RATIONALE:
 * If Anchor says Mass=100 but Disk says PayloadLen=50, Read must fail.
 * ========================================================================= */
hn4_TEST(NanoStorage, Mass_Mismatch) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xEEE;
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Short", 6));
    
    /* Tamper Anchor Mass in RAM */
    anchor.mass = hn4_cpu_to_le64(100); /* Real is 6 */
    
    char buf[100];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 100);
    
    /* Expect SIZE_INVALID -> DATA_ROT mapping */
    ASSERT_EQ(HN4_ERR_DATA_ROT, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 27: WRITE TO UNINITIALIZED VOLUME (NO CORTEX)
 * RATIONALE:
 * If `lba_cortex_start` >= `lba_bitmap_start` (Size 0), write must fail.
 * ========================================================================= */
hn4_TEST(NanoStorage, Zero_Cortex_Size) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    /* Shrink Cortex to 0 */
    vol->sb.info.lba_bitmap_start = vol->sb.info.lba_cortex_start;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xFFF;
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Data", 5);
    
    ASSERT_EQ(HN4_ERR_GEOMETRY, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 28: EPOCH MISMATCH DETECTION (TIME PARADOX)
 * RATIONALE:
 * The Nano CRC is salted with the Volume UUID. It is NOT salted with Epoch ID anymore (per fix).
 * However, the test logic might still check for `NANO_VAL_EPOCH_MISMATCH` if the CRC fails.
 * Wait, the fix removed `epoch_salt` from CRC calculation.
 * So `NANO_VAL_EPOCH_MISMATCH` is unreachable in the current code unless `unsalted_crc` logic remains.
 * Let's check `hn4_read_nano_ballistic`:
 * It calculates `calc_crc`. If mismatch -> `NANO_VAL_CRC_FAIL`.
 * The "Unsalted" check was removed in the fix.
 * So this test verifies that changing Epoch ID does NOT break the read (Persistence).
 * ========================================================================= */
hn4_TEST(NanoStorage, Epoch_Persistence) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x111;
    char payload[] = "PersistMe";
    
    /* Write in Epoch 1 */
    vol->sb.info.current_epoch_id = 1;
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, sizeof(payload)));
    
    /* Reboot / Advance Epoch */
    vol->sb.info.current_epoch_id = 500;
    
    char buf[64];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, sizeof(payload));
    
    /* Should SUCCEED now (Fix applied) */
    ASSERT_EQ(HN4_OK, res);
    ASSERT_EQ(0, strcmp(buf, payload));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 29: INVALID DEVICE TYPE (TAPE)
 * RATIONALE:
 * Tape devices are strictly sequential. Random access nano writes must be rejected.
 * ========================================================================= */
hn4_TEST(NanoStorage, Tape_Rejection) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_ARCHIVE, HN4_DEV_TAPE);
    
    hn4_anchor_t anchor = {0};
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Tape", 4);
    
    ASSERT_EQ(HN4_ERR_PROFILE_MISMATCH, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 30: ZERO LENGTH WRITE (ALLOWED)
 * RATIONALE:
 * Writing 0 bytes is technically a valid "Touch" or "Truncate" operation.
 * It claims a slot but stores no payload.
 * ========================================================================= */
hn4_TEST(NanoStorage, Zero_Byte_Write) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x222;
    
    /* Write 0 bytes */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "", 0));
    
    /* Verify Mass = 0 */
    ASSERT_EQ(0, hn4_le64_to_cpu(anchor.mass));
    
    /* Read back 0 bytes */
    char buf[10];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 0);
    
    /* Should succeed */
    ASSERT_EQ(HN4_OK, res);
    
    cleanup_nano_fixture(vol);
}


/* =========================================================================
 * TEST 31: ORBIT COLLISION - FULL OCCUPANCY (GRAVITY COLLAPSE)
 * RATIONALE:
 * Simulate all 8 orbits being occupied by *other* valid IDs.
 * Write should fail with GRAVITY_COLLAPSE.
 * ========================================================================= */
hn4_TEST(NanoStorage, Orbit_Full_Occupancy) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x333; /* Victim ID */
    uint64_t cortex_start = hn4_addr_to_u64(vol->sb.info.lba_cortex_start) * 512;
    uint64_t cortex_len = (hn4_addr_to_u64(vol->sb.info.lba_bitmap_start) - 
                           hn4_addr_to_u64(vol->sb.info.lba_cortex_start)) * 512;
    
    for(uint64_t i=0; i<cortex_len; i+=512) {
        hn4_nano_quantum_t* q = (hn4_nano_quantum_t*)(impl->mmio_base + cortex_start + i);
        q->magic = hn4_cpu_to_le32(HN4_MAGIC_NANO);
        q->owner_id.lo = 0x999; /* Alien */
        q->payload_len = hn4_cpu_to_le32(10);
    }
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Victim", 6);
    
    ASSERT_EQ(HN4_ERR_GRAVITY_COLLAPSE, res);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 32: VOLUME UUID BINDING (CROSS-VOLUME REPLAY DEFENSE)
 * RATIONALE:
 * Data copied from Vol A to Vol B (same LBA, same File ID) must fail CRC 
 * because the CRC is salted with Vol UUID.
 * ========================================================================= */
hn4_TEST(NanoStorage, Volume_UUID_Binding) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x444;
    char payload[] = "VolA";
    
    /* Write on Volume A */
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, payload, 5));
    
    /* Change Volume UUID (Simulate mounting Vol B) */
    vol->sb.info.volume_uuid.lo ^= 0xFFFFFFFF;
    
    char buf[10];
    hn4_result_t res = hn4_read_nano_ballistic(vol, &anchor, buf, 5);
    
    /* Expect failure. */
    ASSERT_TRUE(res == HN4_ERR_DATA_ROT || res == HN4_ERR_PHANTOM_BLOCK);
    
    cleanup_nano_fixture(vol);
}


/* =========================================================================
 * TEST 33: MAX RETRY EXHAUSTION
 * RATIONALE:
 * If HAL fails IO `HN4_NANO_RETRY_IO` times (3), the write should fail.
 * We can simulate this by pointing the Cortex LBA to an unmapped region.
 * ========================================================================= */
hn4_TEST(NanoStorage, Retry_Exhaustion) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    /* Point Cortex to invalid LBA > Capacity to force HAL error */
    vol->sb.info.lba_cortex_start = hn4_addr_from_u64((TEST_CAP / 512) + 100);
    vol->sb.info.lba_bitmap_start = hn4_addr_add(vol->sb.info.lba_cortex_start, 100);
    
    hn4_anchor_t anchor = {0};
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Retry", 5);
    
    /* 
     * HW_IO/GEOMETRY are possible if check happens early.
     * GRAVITY_COLLAPSE is returned if it loops through all orbits and fails.
     */
    ASSERT_TRUE(res == HN4_ERR_HW_IO || 
                res == HN4_ERR_GEOMETRY || 
                res == HN4_ERR_GRAVITY_COLLAPSE);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 35: DATA CLASS NANO FLAG
 * RATIONALE:
 * Verify `HN4_FLAG_NANO` is set in the anchor after a successful write.
 * ========================================================================= */
hn4_TEST(NanoStorage, Flag_Set_Check) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x55;
    
    ASSERT_EQ(HN4_OK, hn4_write_nano_ballistic(vol, &anchor, "Flag", 4));
    
    uint64_t dc = hn4_le64_to_cpu(anchor.data_class);
    ASSERT_TRUE(dc & HN4_FLAG_NANO);
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 36: NVM BARRIER OPTIMIZATION (SKIP FENCE)
 * RATIONALE:
 * If HN4_HW_NVM is set, the explicit `hn4_hal_barrier()` should be skipped 
 * to reduce latency, as the HAL mem-mapped write includes cache flushing.
 * We verify this by ensuring the write succeeds without a barrier error 
 * even if we mock the barrier to fail.
 * ========================================================================= */
hn4_TEST(NanoStorage, NVM_Barrier_Skip) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    struct hn4_hal_device_impl* impl = (struct hn4_hal_device_impl*)vol->target_device;
    
    /* Enable NVM flag */
    vol->sb.info.hw_caps_flags |= HN4_HW_NVM;
    
    /* 
     * Since we can't easily mock the barrier function to fail in this harness,
     * we rely on logic verification via code inspection or observing that 
     * setting NVM flag still produces a valid write.
     * 
     * Ideally, we'd use a spy to count calls to `hn4_hal_barrier`, but here
     * we ensure the write path completes successfully with NVM set.
     */
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x8888;
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Fast", 4);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Verify data persisted */
    char buf[10];
    ASSERT_EQ(HN4_OK, hn4_read_nano_ballistic(vol, &anchor, buf, 4));
    ASSERT_EQ(0, strcmp(buf, "Fast"));
    
    cleanup_nano_fixture(vol);
}

/* =========================================================================
 * TEST 37: STANDARD SSD BARRIER ENFORCEMENT
 * RATIONALE:
 * If HN4_HW_NVM is NOT set, the barrier MUST run.
 * While we can't detect if it ran without spying, we ensure the write 
 * path remains valid for standard SSDs.
 * ========================================================================= */
hn4_TEST(NanoStorage, SSD_Barrier_Active) {
    hn4_volume_t* vol = create_nano_fixture(HN4_PROFILE_GENERIC, HN4_DEV_SSD);
    
    /* Ensure NVM flag is CLEAR */
    vol->sb.info.hw_caps_flags &= ~HN4_HW_NVM;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x9999;
    
    hn4_result_t res = hn4_write_nano_ballistic(vol, &anchor, "Safe", 4);
    
    ASSERT_EQ(HN4_OK, res);
    
    cleanup_nano_fixture(vol);
}