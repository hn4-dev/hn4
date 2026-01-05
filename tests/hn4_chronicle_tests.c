/*
 * HYDRA-NEXUS 4 (HN4) - CHRONICLE (AUDIT LOG) TESTS
 * FILE: hn4_chronicle_tests.c
 * STATUS: LOGIC VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_chronicle.h"
#include "hn4_hal.h"
#include "hn4_errors.h"

/* --- FIXTURE HELPERS --- */

#define CHRON_SECTOR_SIZE 512
#define CHRON_CAPACITY    (10ULL * 1024ULL * 1024ULL) /* 10 MB */

typedef struct {
    hn4_hal_caps_t caps;
} mock_chron_hal_t;

static hn4_volume_t* create_chronicle_fixture(void) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));

    mock_chron_hal_t* dev = hn4_hal_mem_alloc(sizeof(mock_chron_hal_t));
    memset(dev, 0, sizeof(mock_chron_hal_t));
    
    dev->caps.logical_block_size = CHRON_SECTOR_SIZE;
    dev->caps.total_capacity_bytes = CHRON_CAPACITY;

    vol->target_device = dev;
    vol->read_only = false;
    
    /* 
     * Setup Valid Journal Geometry (Sector LBA Based)
     * Start: LBA 100
     * End:   LBA 200
     * Head:  LBA 100 (Empty/Genesis state)
     */
    vol->sb.info.journal_start = 100;
    vol->sb.info.lba_horizon_start = 200; /* End of Journal */
    vol->sb.info.journal_ptr = 100;       /* Current Head */
    
    vol->sb.info.last_journal_seq = 0;

    return vol;
}

static void cleanup_chronicle_fixture(hn4_volume_t* vol) {
    if (vol) {
        if (vol->target_device) hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * TEST 1: Read-Only Guard
 * Rationale: 
 * The Chronicle is an immutable audit log. Appending to a Read-Only volume
 * must be strictly forbidden to prevent in-memory state drift from disk state.
 * ========================================================================= */
hn4_TEST(ChronicleAppend, ReadOnlyGuard) {
    hn4_volume_t* vol = create_chronicle_fixture();
    
    /* Set Volume to Read-Only */
    vol->read_only = true;

    hn4_result_t res = hn4_chronicle_append(
        vol->target_device,
        vol,
        HN4_CHRONICLE_OP_SNAPSHOT,
        1000, 2000, /* Old/New LBA context */
        0xCAFEBABE  /* Principal Hash */
    );
    
    ASSERT_EQ(HN4_ERR_ACCESS_DENIED, res);
    
    cleanup_chronicle_fixture(vol);
}

/* =========================================================================
 * TEST 2: Bounds Check (Head Overflow)
 * Rationale: 
 * If the Superblock indicates the Journal Pointer (Head) is outside the 
 * valid Journal Region (Start to End), the volume structure is corrupt.
 * The driver must panic/fail rather than writing to random disk locations.
 * ========================================================================= */
hn4_TEST(ChronicleAppend, HeadOutOfBounds) {
    hn4_volume_t* vol = create_chronicle_fixture();
    
    /* 
     * Start = 100, End = 200.
     * Set Head = 205 (Out of bounds).
     */
    vol->sb.info.journal_ptr = 205;

    hn4_result_t res = hn4_chronicle_append(
        vol->target_device,
        vol,
        HN4_CHRONICLE_OP_SNAPSHOT,
        0, 0, 0
    );
    
    ASSERT_EQ(HN4_ERR_BAD_SUPERBLOCK, res);
    
    /* Verify it triggered the Panic flag */
    ASSERT_EQ(HN4_VOL_PANIC, vol->sb.info.state_flags & HN4_VOL_PANIC);
    
    cleanup_chronicle_fixture(vol);
}

/* =========================================================================
 * TEST 3: Inverted Region (Geometry Error)
 * Rationale: 
 * If the Journal End (Horizon Start) is less than or equal to the Journal 
 * Start, the region has zero or negative size. This implies a corrupted 
 * Superblock geometry calculation.
 * ========================================================================= */
hn4_TEST(ChronicleAppend, InvertedRegion) {
    hn4_volume_t* vol = create_chronicle_fixture();
    
    /* 
     * Set End (100) <= Start (200).
     */
    vol->sb.info.journal_start = 200;
    vol->sb.info.lba_horizon_start = 100;
    vol->sb.info.journal_ptr = 200;

    hn4_result_t res = hn4_chronicle_append(
        vol->target_device,
        vol,
        HN4_CHRONICLE_OP_INIT,
        0, 0, 0
    );
    
    ASSERT_EQ(HN4_ERR_BAD_SUPERBLOCK, res);
    
    cleanup_chronicle_fixture(vol);
}

/* =========================================================================
 * TEST 4: Sector Size Insufficiency
 * Rationale: 
 * The Chronicle Header + Tail Marker requires ~72 bytes.
 * If the hardware reports an extremely small sector size (e.g. < 72),
 * the structure cannot physically fit, leading to buffer overflows.
 * ========================================================================= */
hn4_TEST(ChronicleAppend, TinySectorSize) {
    hn4_volume_t* vol = create_chronicle_fixture();
    mock_chron_hal_t* mdev = (mock_chron_hal_t*)vol->target_device;

    /* 
     * Set logical block size to 64 bytes.
     * header (64) + tail (8) = 72 bytes required.
     */
    mdev->caps.logical_block_size = 64;

    hn4_result_t res = hn4_chronicle_append(
        vol->target_device,
        vol,
        HN4_CHRONICLE_OP_INIT,
        0, 0, 0
    );
    
    ASSERT_EQ(HN4_ERR_GEOMETRY, res);
    
    cleanup_chronicle_fixture(vol);
}