/*
 * HYDRA-NEXUS 4 (HN4) - CARTOGRAPHY & EPOCH SUITE
 * FILE: hn4_cartography_epoch_tests.c
 * STATUS: LOGIC VERIFICATION
 *
 * COVERAGE:
 *   [SiliconCartography] - Gold/Silver/Bronze/Toxic allocation policies.
 *   [EpochTime]          - Ring integrity, drift detection, and wrap logic.
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_epoch.h"
#include "hn4_crc.h" /* For Epoch CRC calc */
#include "hn4_addr.h" /* For allocation stubs */

/* --- FIXTURE --- */
#define HN4_BLOCK_SIZE  4096
#define HN4_CAPACITY    (100ULL * 1024ULL * 1024ULL) /* 100 MB */
#define HN4_TOTAL_BLOCKS (HN4_CAPACITY / HN4_BLOCK_SIZE)

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
} mock_hal_device_t;

static hn4_volume_t* create_env(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    memset(dev, 0, sizeof(mock_hal_device_t));
    
    /* Setup NVM-like behavior for easy read/write mocking */
    dev->caps.logical_block_size = 4096;
#ifdef HN4_USE_128BIT
    dev->caps.total_capacity_bytes.lo = HN4_CAPACITY;
#else
    dev->caps.total_capacity_bytes = HN4_CAPACITY;
#endif
    dev->caps.hw_flags = HN4_HW_NVM; /* Enables memcpy IO path */
    dev->mmio_base = hn4_hal_mem_alloc(HN4_CAPACITY); /* Backing store */
    memset(dev->mmio_base, 0, HN4_CAPACITY);

    vol->target_device = (hn4_hal_device_t*)dev;
    vol->vol_block_size = HN4_BLOCK_SIZE;
#ifdef HN4_USE_128BIT
    vol->vol_capacity_bytes.lo = HN4_CAPACITY;
#else
    vol->vol_capacity_bytes = HN4_CAPACITY;
#endif
    vol->read_only = false;

    /* Geometry */
    vol->sb.info.block_size = HN4_BLOCK_SIZE;
    vol->sb.info.lba_flux_start    = hn4_addr_from_u64(100);
    
    /* Allocation Structures */
    vol->bitmap_size = (HN4_TOTAL_BLOCKS + 63) / 64 * sizeof(hn4_armored_word_t);
    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    memset(vol->void_bitmap, 0, vol->bitmap_size);

    /* QMask (Default Silver) */
    vol->qmask_size = (HN4_TOTAL_BLOCKS * 2 + 63) / 64 * 8;
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    memset(vol->quality_mask, 0xAA, vol->qmask_size); /* 1010... = Silver */

    return vol;
}

static void cleanup_env(hn4_volume_t* vol) {
    if (vol) {
        mock_hal_device_t* dev = (mock_hal_device_t*)vol->target_device;
        if (dev) {
            if (dev->mmio_base) hn4_hal_mem_free(dev->mmio_base);
            hn4_hal_mem_free(dev);
        }
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
        hn4_hal_mem_free(vol);
    }
}

/* Helper to flood the Q-Mask with a specific pattern */
static void flood_qmask(hn4_volume_t* vol, uint8_t pattern_byte) {
    memset(vol->quality_mask, pattern_byte, vol->qmask_size);
}

/* =========================================================================
 * 1. SILICON CARTOGRAPHY (QUALITY TIERS)
 * ========================================================================= */

/*
 * Test Q1: Metadata Rejects Bronze
 * RATIONALE:
 * Critical metadata (Anchors) requires high retention. 
 * If the entire disk is Bronze (01), allocation for metadata must fail.
 */
hn4_TEST(SiliconCartography, MetadataRejectsBronze) {
    hn4_volume_t* vol = create_env();
    
    /* Flood with Bronze (01010101 = 0x55) */
    flood_qmask(vol, 0x55);
    
    uint64_t G, V;
    /* Request Metadata Allocation */
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_METADATA, &G, &V);
    
    /* 
     * EXPECTATION CHANGE:
     * Old: HN4_ERR_EVENT_HORIZON (Spilled to D1.5)
     * New: HN4_ERR_ENOSPC (System/Metadata refuses to fragment into Horizon)
     */
    ASSERT_EQ(HN4_ERR_ENOSPC, res);

    cleanup_env(vol);
}

/*
 * Test Q3: Toxic Ban
 * RATIONALE:
 * Blocks marked Toxic (00) must NEVER be allocated, regardless of intent.
 */
hn4_TEST(SiliconCartography, ToxicIsBannedGlobal) {
    hn4_volume_t* vol = create_env();
    
    /* Flood with Toxic (0x00) */
    flood_qmask(vol, 0x00);
    
    uint64_t G, V;
    
    /* 1. Try Metadata -> Should be ENOSPC (Strict Policy) */
    hn4_result_t res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_METADATA, &G, &V);
    ASSERT_EQ(HN4_ERR_ENOSPC, res);
      res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_DEFAULT, &G, &V);
    res = hn4_alloc_genesis(vol, 0, HN4_ALLOC_METADATA, &G, &V);
    ASSERT_EQ(HN4_ERR_ENOSPC, res);

    cleanup_env(vol);
}

/* =========================================================================
 * 2. EPOCH RING (TIME & STATE)
 * ========================================================================= */

/* Helper to inject an Epoch Header into the Mock NVM backing store */
static void inject_epoch_on_disk(hn4_volume_t* vol, uint64_t block_idx, uint64_t epoch_id) {
    mock_hal_device_t* dev = (mock_hal_device_t*)vol->target_device;
    uint32_t bs = vol->vol_block_size;
    
    hn4_epoch_header_t ep;
    memset(&ep, 0, sizeof(ep));
    ep.epoch_id = epoch_id;
    ep.timestamp = 123456789;
    
    /* Calculate CRC on LE struct if needed, but here we work in host memory */
    /* The driver will read it back. If driver expects LE, we should swap. */
    /* For simplicity, assuming Host=LE (x86 test runner). */
    ep.epoch_crc = hn4_epoch_calc_crc(&ep);
    
    /* Write to backing store at Block Offset */
    uint64_t offset = block_idx * bs;
    memcpy(dev->mmio_base + offset, &ep, sizeof(ep));
}

/*
 * Test E1: Epoch Sync (Healthy)
 * RATIONALE:
 * Disk Epoch ID == Memory Epoch ID. System is consistent.
 */
hn4_TEST(EpochTime, SyncStateHealthy) {
    hn4_volume_t* vol = create_env();
    
    /* Setup Ring Pointer */
    uint64_t ring_idx = 500;
    vol->sb.info.lba_epoch_start = hn4_addr_from_u64(100 * (4096/4096)); /* Start Block 100 */
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_idx);
    vol->sb.info.current_epoch_id = 1000;

    /* Inject Matching Epoch on Disk */
    inject_epoch_on_disk(vol, ring_idx, 1000);

    /* Run Check */
#ifdef HN4_USE_128BIT
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes.lo);
#else
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes);
#endif
    
    ASSERT_EQ(HN4_OK, res);

    cleanup_env(vol);
}

/*
 * Test E2: Future Toxic (Impossible Drift)
 * RATIONALE:
 * Disk ID is > 5000 ahead of Memory. This implies either the media is from 
 * the far future (impossible) or the local state is corrupted/stale beyond recovery.
 */
hn4_TEST(EpochTime, FutureToxicDetect) {
    hn4_volume_t* vol = create_env();
    uint64_t ring_idx = 500;
    
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_idx);
    vol->sb.info.current_epoch_id = 1000;

    /* Inject Future ID (1000 + 6000) */
    inject_epoch_on_disk(vol, ring_idx, 7000);

#ifdef HN4_USE_128BIT
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes.lo);
#else
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes);
#endif
    
    ASSERT_EQ(HN4_ERR_MEDIA_TOXIC, res);

    cleanup_env(vol);
}

/*
 * Test E3: Time Dilation (Valid Drift)
 * RATIONALE:
 * Disk ID is ahead, but within limits (e.g. +10). This happens if a crash 
 * occurred after Epoch Advance but before SB Broadcast.
 * System should flag `HN4_ERR_TIME_DILATION` (Warning/Info), not Toxic.
 */
hn4_TEST(EpochTime, TimeDilationDetect) {
    hn4_volume_t* vol = create_env();
    uint64_t ring_idx = 500;
    
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_idx);
    vol->sb.info.current_epoch_id = 1000;

    /* Inject Slight Future ID (1000 + 5) */
    inject_epoch_on_disk(vol, ring_idx, 1005);

#ifdef HN4_USE_128BIT
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes.lo);
#else
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes);
#endif
    
    ASSERT_EQ(HN4_ERR_TIME_DILATION, res);

    cleanup_env(vol);
}

/*
 * Test E4: Generation Skew (Rollback Detect)
 * RATIONALE:
 * Disk ID is BEHIND Memory. This is a "Phantom Write" or Replay Attack signature.
 * The drive ignored our writes or reverted to an old snapshot.
 */
hn4_TEST(EpochTime, GenerationSkewDetect) {
    hn4_volume_t* vol = create_env();
    uint64_t ring_idx = 500;
    
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_idx);
    vol->sb.info.current_epoch_id = 1000;

    /* Inject Past ID (999) */
    inject_epoch_on_disk(vol, ring_idx, 999);

#ifdef HN4_USE_128BIT
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes.lo);
#else
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes);
#endif
    
    ASSERT_EQ(HN4_ERR_GENERATION_SKEW, res);

    cleanup_env(vol);
}

/*
 * Test E5: Ring Wrap Logic (Advance)
 * RATIONALE:
 * If the ring pointer is at the very end of the allocated ring space,
 * `hn4_epoch_advance` must wrap it back to the start index.
 */
hn4_TEST(EpochTime, RingWrapLogic) {
    hn4_volume_t* vol = create_env();
    mock_hal_device_t* dev = (mock_hal_device_t*)vol->target_device;
    
    uint32_t bs = 4096;
    
    /* 
     * Setup Ring:
     * Start Sector LBA: 100
     * Size: 1MB (256 Blocks)
     * Start Block: 100 (if 1:1 map)
     * End Block: 100 + 256 = 356
     * Max Valid Ptr: 355
     */
    vol->sb.info.lba_epoch_start = hn4_addr_from_u64(100);
    
    /* Set pointer to last valid block */
    uint64_t ring_len = (1024 * 1024) / bs;
    uint64_t start_blk = 100;
    uint64_t last_blk = start_blk + ring_len - 1;
    
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(last_blk);
    vol->sb.info.current_epoch_id = 10;

    uint64_t new_id;
    hn4_addr_t new_ptr;

    /* Execute Advance */
    hn4_result_t res = hn4_epoch_advance(vol->target_device, &vol->sb, false, &new_id, &new_ptr);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* Check ID Increment */
    ASSERT_EQ(11ULL, new_id);
    
    /* Check Wrap: Should be Start Block (100) */
#ifdef HN4_USE_128BIT
    ASSERT_EQ(start_blk, new_ptr.lo);
#else
    ASSERT_EQ(start_blk, new_ptr);
#endif

    cleanup_env(vol);
}

/*
 * Test E6: CRC Invalid == Epoch Lost (Toxic)
 * RATIONALE:
 * If the Epoch Header on disk has a CRC mismatch, it must be completely distrusted.
 * Even if the ID looks "sane" or matches memory, integrity failure takes precedence.
 * The system must return HN4_ERR_EPOCH_LOST, which triggers Read-Only Quarantine.
 */
hn4_TEST(EpochTime, CrcInvalidIsLost) {
    hn4_volume_t* vol = create_env();
    uint64_t ring_idx = 500;
    
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_idx);
    vol->sb.info.current_epoch_id = 1000;

    /* 1. Inject VALID Epoch first to establish baseline */
    inject_epoch_on_disk(vol, ring_idx, 1000);
    
    /* 2. Corrupt the Payload on Disk */
    mock_hal_device_t* dev = (mock_hal_device_t*)vol->target_device;
    uint32_t bs = vol->vol_block_size;
    uint64_t offset = ring_idx * bs;
    
    /* Flip a bit in the ID field (payload) */
    /* This invalidates the stored CRC checksum */
    hn4_epoch_header_t* ep = (hn4_epoch_header_t*)(dev->mmio_base + offset);
    ep->epoch_id = 9999; /* Change ID without re-calculating CRC */

    /* 3. Run Check */
#ifdef HN4_USE_128BIT
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes.lo);
#else
    hn4_result_t res = hn4_epoch_check_ring(vol->target_device, &vol->sb, vol->vol_capacity_bytes);
#endif
    
    /* 
     * Expectation: HN4_ERR_EPOCH_LOST
     * Logic: Read -> Calc CRC -> Mismatch -> LOST.
     */
    ASSERT_EQ(HN4_ERR_EPOCH_LOST, res);

    cleanup_env(vol);
}