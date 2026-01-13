/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Namespace Logic Tests (Spec Compliance)
 * SOURCE:      hn4_namespace_tests.c
 * STATUS:      FIXED / PRODUCTION
 *
 * TEST OBJECTIVE:
 * Verify Spec 6.0 Compliance for Hashing, URI Grammar, Slicing, and Extensions.
 * Self-contained: Includes local helpers to avoid linking errors.
 */

#include "hn4.h"
#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * INTERNAL DEFINITIONS (Local copies for test isolation)
 * ========================================================================= */

#define HN4_NS_NAME_MAX         255
#define HN4_FLAG_EXTENDED       (1ULL << 23)
#define HN4_EXT_TYPE_TAG        0x01
#define HN4_EXT_TYPE_LONGNAME   0x02
#define HN4_NS_MAX_EXT_DEPTH    16

/* Assume public API is available */
extern hn4_result_t hn4_ns_resolve(hn4_volume_t* vol, const char* path, hn4_anchor_t* out_anchor);

/* Local Helper: Generate Tag Mask (Spec 5.1 Logic) */
static uint64_t _local_generate_tag_mask(const char* tag, size_t len) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)tag[i];
        hash *= 0x100000001B3ULL;
    }
    uint64_t bit1 = (hash) & 63;
    uint64_t bit2 = (hash >> 21) & 63;
    uint64_t bit3 = (hash >> 42) & 63;
    return (1ULL << bit1) | (1ULL << bit2) | (1ULL << bit3);
}

/* Local Helper: Hash UUID (Spec 3.1 Logic) */
static uint64_t _local_hash_uuid(hn4_u128_t id) {
    uint64_t h = id.lo ^ id.hi;
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= (h >> 33);
    return h;
}

/* =========================================================================
 * FIXTURE INFRASTRUCTURE
 * ========================================================================= */

#define NS_FIXTURE_SIZE    (32ULL * 1024 * 1024)
#define NS_BLOCK_SIZE      4096
#define NS_SECTOR_SIZE     512

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
} _ns_test_hal_t;

static void _ns_inject_buffer(hn4_hal_device_t* dev, uint8_t* buffer) {
    _ns_test_hal_t* impl = (_ns_test_hal_t*)dev;
    impl->mmio_base = buffer;
}

static hn4_hal_device_t* ns_setup(void) {
    uint8_t* ram = calloc(1, NS_FIXTURE_SIZE);
    hn4_hal_device_t* dev = hn4_hal_mem_alloc(sizeof(_ns_test_hal_t));
    
    hn4_hal_caps_t* caps = (hn4_hal_caps_t*)dev;
#ifdef HN4_USE_128BIT
    caps->total_capacity_bytes.lo = NS_FIXTURE_SIZE;
#else
    caps->total_capacity_bytes = NS_FIXTURE_SIZE;
#endif
    caps->logical_block_size = NS_SECTOR_SIZE;
    caps->hw_flags = HN4_HW_NVM;
    
    _ns_inject_buffer(dev, ram);
    hn4_hal_init();
    hn4_crc_init();
    
    /* Minimal Superblock */
    hn4_superblock_t sb = {0};
    sb.info.magic = HN4_MAGIC_SB;
    sb.info.block_size = NS_BLOCK_SIZE;
    sb.info.lba_cortex_start = hn4_lba_from_sectors(256);
    sb.info.lba_bitmap_start = hn4_lba_from_sectors(512); 
    sb.info.lba_flux_start   = hn4_lba_from_sectors(1024);
    
    memcpy(ram, &sb, sizeof(sb));
    return dev;
}

static void ns_teardown(hn4_hal_device_t* dev) {
    _ns_test_hal_t* impl = (_ns_test_hal_t*)dev;
    free(impl->mmio_base);
    hn4_hal_mem_free(dev);
}

/* 
 * FIX: Updated local writer to include FULL STRUCTURE in CRC calculation.
 * This ensures test data is compatible with the hardened production reader logic
 * (which now includes 'orbit_hints' and matches the fixed hn4_anchor.c).
 */
static void _local_write_anchor(hn4_hal_device_t* dev, hn4_superblock_t* sb, uint64_t slot_idx, hn4_anchor_t* anchor) {
    /* 1. Explicitly zero checksum field */
    anchor->checksum = 0;
    
    /* 2. Hash the ENTIRE 128-byte structure (Fixing the gap) */
    uint32_t c = hn4_crc32(0, anchor, sizeof(hn4_anchor_t));
    
    anchor->checksum = hn4_cpu_to_le32(c);

    uint64_t start = hn4_addr_to_u64(sb->info.lba_cortex_start);
    uint64_t lba = start + (slot_idx * sizeof(hn4_anchor_t) / NS_SECTOR_SIZE);
    uint64_t offset = (slot_idx * sizeof(hn4_anchor_t)) % NS_SECTOR_SIZE;
    
    /* Read-Modify-Write */
    uint8_t sector[NS_SECTOR_SIZE];
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_lba_from_sectors(lba), sector, 1);
    memcpy(sector + offset, anchor, sizeof(hn4_anchor_t));
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(lba), sector, 1);
}

/* =========================================================================
 * TEST CASES
 * ========================================================================= */

/* TEST 1: Hash Pipeline Compliance (Spec 3.1) */
hn4_TEST(Namespace, Hash_Pipeline_End_To_End) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Construct ID: lo=0xCAFEBABE, hi=0xDEADBEEF */
    hn4_u128_t id = { .lo = 0xCAFEBABE, .hi = 0xDEADBEEF };
    
    /* 1. Calculate Expected Slot using Spec Logic (Same as driver) */
    uint64_t h = _local_hash_uuid(id);
    
    /* Calculate Total Slots in Cortex (Start=256, End=512, SS=512) */
    uint64_t cortex_sectors = (512 - 256);
    uint64_t cortex_bytes = cortex_sectors * NS_SECTOR_SIZE;
    uint64_t total_slots = cortex_bytes / sizeof(hn4_anchor_t);
    uint64_t expected_slot = h % total_slots;
    
    /* 2. Plant Anchor at Expected Slot */
    hn4_anchor_t anchor = {0};
    /* Ensure the on-disk ID matches the CPU ID used for hashing */
    anchor.seed_id = hn4_cpu_to_le128(id);
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    
    _local_write_anchor(dev, &vol.sb, expected_slot, &anchor);
    
    /* 3. Ask Driver to Resolve ID */
    /* id: <HI><LO> in hex */
    hn4_anchor_t out;
    const char* id_str = "id:00000000DEADBEEF00000000CAFEBABE";
    
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, id_str, &out));
    ASSERT_EQ(0xCAFEBABE, hn4_le64_to_cpu(out.seed_id.lo));

    ns_teardown(dev);
}


/* TEST 2: URI Grammar - Tag Grouping (Spec 7) */
hn4_TEST(Namespace, URI_Tag_Grouping_And_Pure_Tag_Query) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* 1. Plant Anchor with Tags "A" and "B" */
    uint64_t tag_mask = _local_generate_tag_mask("A", 1) | _local_generate_tag_mask("B", 1);
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 99;
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    anchor.tag_filter = hn4_cpu_to_le64(tag_mask);
    
    /* Write to slot 0 (Resonance Scan sweeps all) */
    _local_write_anchor(dev, &vol.sb, 0, &anchor);

    hn4_anchor_t out;
    
    /* 2. Query using Grouping Syntax "/tag:A+B" */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:A+B", &out));
    ASSERT_EQ(99, out.seed_id.lo);

    ns_teardown(dev);
}

/* TEST 3: Time Slicing - Past vs Future (Spec 7.4) */
hn4_TEST(Namespace, URI_Time_Slice_Validation) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 2;
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)anchor.inline_buffer, "file.txt", 20);
    
    /* Created: T=1000s, Modified: T=2000s */
    anchor.create_clock = hn4_cpu_to_le32(1000); 
    anchor.mod_clock = hn4_cpu_to_le64(2000ULL * 1000000000ULL); /* NS */
    
    _local_write_anchor(dev, &vol.sb, 0, &anchor);

    hn4_anchor_t out;
    
    /* Case A: Query T=1500s (Time Paradox) */
    /* Target: 1500s = 1500000000000 ns */
    ASSERT_EQ(HN4_ERR_TIME_PARADOX, hn4_ns_resolve(&vol, "/file.txt#time:1500000000000", &out));

    /* Case B: Query T=500s (Before Creation) -> Not Found */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "/file.txt#time:500000000000", &out));

    /* Case C: Query T=2500s (Future) -> OK */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/file.txt#time:2500000000000", &out));

    ns_teardown(dev);
}

/* TEST 5: Extension Geometry & Multi-Type */
hn4_TEST(Namespace, Extension_MultiType_and_Geometry) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    vol.vol_block_size = NS_BLOCK_SIZE;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t anchor = {0};
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_EXTENDED);
    
    /* FIX: Align Pointer to Block Size */
    uint32_t spb = NS_BLOCK_SIZE / NS_SECTOR_SIZE; /* 8 */
    uint64_t ext_blk = 2000;
    uint64_t ext_lba = ext_blk * spb; 
    
    uint64_t le_ptr = hn4_cpu_to_le64(ext_lba);
    memcpy(anchor.inline_buffer, &le_ptr, 8);
    strncpy((char*)anchor.inline_buffer + 8, "long", 4);

    _local_write_anchor(dev, &vol.sb, 0, &anchor);

    /* FIX: Zero full block before writing to avoid garbage payload */
    uint8_t ext_buf[NS_BLOCK_SIZE] = {0}; 
    hn4_extension_header_t* ext1 = (hn4_extension_header_t*)ext_buf;
    
    ext1->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    ext1->type = hn4_cpu_to_le32(HN4_EXT_TYPE_TAG);
    
    /* Next ptr */
    uint64_t next_lba = (ext_blk + 1) * spb;
    ext1->next_ext_lba = hn4_cpu_to_le64(next_lba);
    
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(ext_lba), ext_buf, spb);

    /* Second Extension: LONGNAME */
    memset(ext_buf, 0, NS_BLOCK_SIZE);
    ext1->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    ext1->type = hn4_cpu_to_le32(HN4_EXT_TYPE_LONGNAME);
    strcpy((char*)ext1->payload, "_filename");
    ext1->next_ext_lba = 0;

    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(next_lba), ext_buf, spb);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/long_filename", &out));

    ns_teardown(dev);
}


/* TEST 6: Generation Slicing */
hn4_TEST(Namespace, URI_Generation_Slice_Validation) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 3;
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)anchor.inline_buffer, "gen.txt", 20);
    anchor.write_gen = hn4_cpu_to_le32(10); 
    
    _local_write_anchor(dev, &vol.sb, 0, &anchor);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/gen.txt#gen:10", &out));
    ASSERT_EQ(HN4_ERR_TIME_PARADOX, hn4_ns_resolve(&vol, "/gen.txt#gen:9", &out));

    ns_teardown(dev);
}

/* TEST 7: Skip Tombstones */
hn4_TEST(Namespace, Resonance_Skip_Tombstones) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Slot 0: Tombstone */
    hn4_anchor_t tomb = {0};
    tomb.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_FLAG_TOMBSTONE);
    strncpy((char*)tomb.inline_buffer, "file.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &tomb);

    /* Slot 1: Valid */
    hn4_anchor_t valid = {0};
    valid.seed_id.lo = 55;
    valid.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)valid.inline_buffer, "file.txt", 20);
    _local_write_anchor(dev, &vol.sb, 1, &valid);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/file.txt", &out));
    ASSERT_EQ(55, out.seed_id.lo);

    ns_teardown(dev);
}

/* TEST 8: Bloom Filter Rejection */
hn4_TEST(Namespace, Resonance_Bloom_Filter_Reject) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t anchor = {0};
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    anchor.tag_filter = hn4_cpu_to_le64(_local_generate_tag_mask("A", 1));
    strncpy((char*)anchor.inline_buffer, "tagged.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &anchor);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:A/tagged.txt", &out));
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "/tag:B/tagged.txt", &out));

    ns_teardown(dev);
}

/* TEST 9: Empty Name Error */
hn4_TEST(Namespace, Resolve_Empty_Name_Error) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    
    hn4_anchor_t out;
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_ns_resolve(&vol, "", &out));
    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, hn4_ns_resolve(&vol, "/", &out));

    ns_teardown(dev);
}

/* =========================================================================
 * 1. BASIC ANCHOR & IDENTITY TESTS
 * ========================================================================= */

hn4_TEST(Namespace, Anchor_Identity_and_Name_Lookup) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_u128_t cpu_id = { .lo = 0xCAFEBABE, .hi = 0xDEADBEEF };

    hn4_anchor_t a = {0};
    a.seed_id = hn4_cpu_to_le128(cpu_id);
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "config.sys", 20);
    
    /* FIX: Calculate correct slot for ID lookup */
    uint64_t h = _local_hash_uuid(cpu_id);
    uint64_t slots = (256 * NS_SECTOR_SIZE) / sizeof(hn4_anchor_t);
    uint64_t slot = h % slots;

    _local_write_anchor(dev, &vol.sb, slot, &a);

    hn4_anchor_t out;

    /* TEST A: Resolve by Name (Resonance Scan finds it anywhere) */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "config.sys", &out));
    ASSERT_EQ(0xCAFEBABE, hn4_le64_to_cpu(out.seed_id.lo));

    /* TEST B: Resolve by Identity ID (Requires correct slot placement) */
    const char* id_uri = "id:00000000DEADBEEF00000000CAFEBABE";
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, id_uri, &out));

    ns_teardown(dev);
}


/* =========================================================================
 * 2. HUMAN SEMANTIC TAGS (Real World Workflow)
 * ========================================================================= */

hn4_TEST(Namespace, Human_Semantic_Workflow) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Setup File 1 */
    hn4_anchor_t a1 = {0};
    a1.seed_id.lo = 101;
    a1.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a1.inline_buffer, "specs.pdf", 20);
    uint64_t t1 = _local_generate_tag_mask("Titan", 5) | _local_generate_tag_mask("2024", 4);
    a1.tag_filter = hn4_cpu_to_le64(t1);
    _local_write_anchor(dev, &vol.sb, 10, &a1);

    /* Setup File 2 */
    hn4_anchor_t a2 = {0};
    a2.seed_id.lo = 102;
    a2.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a2.inline_buffer, "memo.txt", 20);
    uint64_t t2 = _local_generate_tag_mask("Zeus", 4) | _local_generate_tag_mask("2024", 4);
    a2.tag_filter = hn4_cpu_to_le64(t2);
    _local_write_anchor(dev, &vol.sb, 20, &a2);

    hn4_anchor_t out;

    /* TEST A: Hierarchical Query (/tag:Titan/tag:2024/specs.pdf) */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:Titan/tag:2024/specs.pdf", &out));
    ASSERT_EQ(101, out.seed_id.lo);

    /* TEST B: Combined Query (/tag:Zeus+2024/memo.txt) */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:Zeus+2024/memo.txt", &out));
    ASSERT_EQ(102, out.seed_id.lo);

    /* TEST C: Negative Match (Search for Titan file in Zeus tag) */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "/tag:Zeus/specs.pdf", &out));

    ns_teardown(dev);
}

/* =========================================================================
 * 3. AI TOPOLOGY / TENSOR TUNNEL
 * ========================================================================= */

hn4_TEST(Namespace, AI_Topology_Tunnel_Check) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    
    /* Set Profile to AI to enable topology logic */
    vol.sb.info.format_profile = HN4_PROFILE_AI;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Mock Topology: GPU 0 -> LBA Range [10000, 20000] */
    vol.topo_count = 1;
    vol.topo_map = calloc(1, 64); /* Mock struct size */
    
    /* Assume opaque structure */
    uint64_t* t = (uint64_t*)vol.topo_map;
    t[0] = 0; /* ID 0 */
    t[1] = 20000; /* Start LBA */
    t[2] = 10000; /* Length */

    /* Verify Namespace resolution for "model.bin" returns an anchor */
    hn4_anchor_t a = {0};
    a.seed_id.lo = 9000;
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_HINT_HORIZON); /* Matrix Type */
    strncpy((char*)a.inline_buffer, "model.bin", 20);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "model.bin", &out));
    
    /* Verify it identified as a Matrix/Horizon file */
    uint64_t dc = hn4_le64_to_cpu(out.data_class);
    ASSERT_TRUE((dc & HN4_HINT_HORIZON) != 0);

    free(vol.topo_map);
    ns_teardown(dev);
}

/* =========================================================================
 * 4. PERMISSIONS (WORM & APPEND)
 * ========================================================================= */

hn4_TEST(Namespace, Permission_Flags_Check) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Setup Immutable File (WORM) */
    hn4_anchor_t a1 = {0};
    a1.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    a1.permissions = hn4_cpu_to_le32(HN4_PERM_READ | HN4_PERM_IMMUTABLE);
    strncpy((char*)a1.inline_buffer, "worm.dat", 20);
    _local_write_anchor(dev, &vol.sb, 5, &a1);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "worm.dat", &out));
    
    /* Verify Permission Bits */
    uint32_t p = hn4_le32_to_cpu(out.permissions);
    ASSERT_TRUE((p & HN4_PERM_IMMUTABLE) != 0);
    ASSERT_FALSE((p & HN4_PERM_WRITE) != 0);

    ns_teardown(dev);
}

/* TEST: Fallopian Tubes (Tensor Tunneling) */
hn4_TEST(Namespace, Fallopian_Tube_Tensor_Mapping) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    
    /* 1. Setup AI Profile */
    vol.sb.info.format_profile = HN4_PROFILE_AI;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* 2. Mock Topology */
    vol.topo_count = 1;
    vol.topo_map = calloc(1, 64); 
    
    uint64_t* t = (uint64_t*)vol.topo_map;
    t[0] = 0; /* ID 0 */
    t[1] = 20000; /* Start LBA */
    t[2] = 10000; /* Length */

    /* 3. Create Anchor */
    hn4_anchor_t a = {0};
    a.seed_id.lo = 888;
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_HINT_HORIZON); 
    strncpy((char*)a.inline_buffer, "model.bin", 20);
    
    /* FIX: Gravity Center is Physical BLOCK Index */
    /* LBA 25000 is inside [20000, 30000]. Block = 25000 / 8 */
    uint32_t spb = NS_BLOCK_SIZE / NS_SECTOR_SIZE;
    a.gravity_center = hn4_cpu_to_le64(25000 / spb); 
    
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "model.bin", &out));
    
    uint64_t dc = hn4_le64_to_cpu(out.data_class);
    ASSERT_TRUE((dc & HN4_HINT_HORIZON) != 0);
    
    free(vol.topo_map);
    ns_teardown(dev);
}


/* =========================================================================
 * 6. NAME SEMANTICS
 * ========================================================================= */

hn4_TEST(Namespace, Name_Inline_Only) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0};
    a.seed_id.lo = 601;
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "short.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "short.txt", &out));
    ASSERT_EQ(601, out.seed_id.lo);

    ns_teardown(dev);
}

hn4_TEST(Namespace, Name_Inline_Extension_Stitch) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    vol.vol_block_size = NS_BLOCK_SIZE;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    uint64_t ext_ptr = 3000;
    uint32_t spb = NS_BLOCK_SIZE / NS_SECTOR_SIZE;

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_EXTENDED);
    uint64_t le_ptr = hn4_cpu_to_le64(ext_ptr * spb);
    memcpy(a.inline_buffer, &le_ptr, 8);
    strncpy((char*)a.inline_buffer + 8, "prefix_", 7);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_extension_header_t ext = {0};
    ext.magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    ext.type = hn4_cpu_to_le32(HN4_EXT_TYPE_LONGNAME);
    strcpy((char*)ext.payload, "suffix");
    ext.next_ext_lba = 0;
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_blocks(ext_ptr * spb), &ext, spb);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "prefix_suffix", &out));

    ns_teardown(dev);
}

hn4_TEST(Namespace, Name_Exact_Compare_Case_Sensitivity) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "File.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "File.txt", &out));
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "file.txt", &out));

    ns_teardown(dev);
}

/* =========================================================================
 * 7. RESONANCE SCAN
 * ========================================================================= */

hn4_TEST(Namespace, Resonance_Scan_Modes) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* 1. Pure Name */
    hn4_anchor_t a1 = {0}; a1.seed_id.lo = 1; 
    a1.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a1.inline_buffer, "name_only", 20);
    _local_write_anchor(dev, &vol.sb, 0, &a1);

    /* 2. Pure Tag */
    hn4_anchor_t a2 = {0}; a2.seed_id.lo = 2;
    a2.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    a2.tag_filter = hn4_cpu_to_le64(_local_generate_tag_mask("TagOnly", 7));
    _local_write_anchor(dev, &vol.sb, 1, &a2);

    /* 3. Name + Tag */
    hn4_anchor_t a3 = {0}; a3.seed_id.lo = 3;
    a3.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    a3.tag_filter = hn4_cpu_to_le64(_local_generate_tag_mask("Mixed", 5));
    strncpy((char*)a3.inline_buffer, "mixed_file", 20);
    _local_write_anchor(dev, &vol.sb, 2, &a3);

    hn4_anchor_t out;
    
    /* Test Pure Name */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "name_only", &out));
    ASSERT_EQ(1, out.seed_id.lo);

    /* Test Pure Tag */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:TagOnly", &out));
    ASSERT_EQ(2, out.seed_id.lo);

    /* Test Name + Tag */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:Mixed/mixed_file", &out));
    ASSERT_EQ(3, out.seed_id.lo);

    ns_teardown(dev);
}

hn4_TEST(Namespace, Resonance_Generation_Arbitration) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* FIX: Use SAME ID for arbitration test. */
    hn4_u128_t id = { .lo = 99, .hi = 99 };

    /* Old Version: Gen 5 */
    hn4_anchor_t v1 = {0}; v1.seed_id = id; v1.write_gen = hn4_cpu_to_le32(5);
    v1.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)v1.inline_buffer, "file.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &v1);

    /* New Version: Gen 6 */
    hn4_anchor_t v2 = {0}; v2.seed_id = id; v2.write_gen = hn4_cpu_to_le32(6);
    v2.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)v2.inline_buffer, "file.txt", 20);
    _local_write_anchor(dev, &vol.sb, 1, &v2);

    hn4_anchor_t out;
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "file.txt", &out));
    
    /* Must return Gen 6 */
    ASSERT_EQ(6, hn4_le32_to_cpu(out.write_gen));

    ns_teardown(dev);
}

/* =========================================================================
 * 8. URI GRAMMAR
 * ========================================================================= */

hn4_TEST(Namespace, URI_Grammar_Suite) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_u128_t cpu_id = { .lo = 100, .hi = 0 };

    hn4_anchor_t a = {0}; 
    a.seed_id = hn4_cpu_to_le128(cpu_id);
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "file.txt", 20);
    
    /* FIX: Correct Tag Mask Logic */
    uint64_t tm = _local_generate_tag_mask("Finance", 7) | _local_generate_tag_mask("2024", 4);
    a.tag_filter = hn4_cpu_to_le64(tm);
    
    /* FIX: Write to Correct Hash Slot for ID Lookup */
    uint64_t h = _local_hash_uuid(cpu_id);
    uint64_t total_slots = (256 * NS_SECTOR_SIZE) / sizeof(hn4_anchor_t);
    _local_write_anchor(dev, &vol.sb, h % total_slots, &a);

    hn4_anchor_t out;

    /* tag:Finance+2024/file.txt */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "/tag:Finance+2024/file.txt", &out));

    /* id:<hex> (Correct slot placement allows this to pass) */
    /* 100 decimal = 0x64 hex */
    const char* id_uri = "id:00000000000000000000000000000064"; 
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, id_uri, &out));

    ns_teardown(dev);
}


/* =========================================================================
 * 9. SLICE ENGINE
 * ========================================================================= */

hn4_TEST(Namespace, Slice_Engine_Logic) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "time.txt", 20);
    
    /* Timestamps in Nanoseconds (1000s, 2000s) */
    a.create_clock = hn4_cpu_to_le32(1000);
    a.mod_clock = hn4_cpu_to_le64(2000ULL * 1000000000ULL);
    a.write_gen = hn4_cpu_to_le32(5);
    
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;

    /* 
     * Time: Before Creation (500s).
     * Expect NOT_FOUND as file didn't exist.
     */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "time.txt#time:500000000000", &out));

    /* 
     * Time: Between Creation and Mod (1500s).
     * Expect TIME_PARADOX because current version (T=2000) is newer than requested slice (T=1500).
     */
    ASSERT_EQ(HN4_ERR_TIME_PARADOX, hn4_ns_resolve(&vol, "time.txt#time:1500000000000", &out));

    /* 
     * Gen: Mismatch.
     * Expect TIME_PARADOX.
     */
    ASSERT_EQ(HN4_ERR_TIME_PARADOX, hn4_ns_resolve(&vol, "time.txt#gen:4", &out));

    ns_teardown(dev);
}



/* =========================================================================
 * 10. TEMPORAL CORRECTNESS
 * ========================================================================= */

hn4_TEST(Namespace, Immutable_History_Law) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0}; a.seed_id.lo = 99;
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "hist.txt", 20);
    a.mod_clock = hn4_cpu_to_le64(1000ULL);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    /* Slicing must not mutate the returned anchor ID or content, only validate it */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "hist.txt#time:2000", &out));
    ASSERT_EQ(99, out.seed_id.lo);
    /* Ensure no side effects */
    ASSERT_EQ(1000, hn4_le64_to_cpu(out.mod_clock));

    ns_teardown(dev);
}

/* =========================================================================
 * 11. CORRUPTION DEFENSE
 * ========================================================================= */

hn4_TEST(Namespace, Corruption_Defense_CRC) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "corrupt.txt", 20);
    
    /* Write with valid CRC */
    _local_write_anchor(dev, &vol.sb, 0, &a);
    
    /* Corrupt the data on disk */
    uint64_t lba = hn4_addr_to_u64(vol.sb.info.lba_cortex_start);
    uint8_t sector[512];
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_lba_from_sectors(lba), sector, 1);
    sector[20] ^= 0xFF; /* Flip bits in payload */
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_sectors(lba), sector, 1);

    hn4_anchor_t out;
    /* Should be ignored due to CRC mismatch */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "corrupt.txt", &out));

    ns_teardown(dev);
}

/* =========================================================================
 * 12. GEOMETRY LAW
 * ========================================================================= */

hn4_TEST(Namespace, Geometry_Law_Extension_Ptrs) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_EXTENDED);
    
    /* Ptr before Flux (e.g. 0) -> Rejected */
    uint64_t bad_ptr = 0;
    uint64_t le_ptr = hn4_cpu_to_le64(bad_ptr);
    memcpy(a.inline_buffer, &le_ptr, 8);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    hn4_anchor_t out;
    /* Extension chain broken/rejected -> Name truncated */
    /* Expect match on empty/truncated name? No, get_name fails -> resolve fails */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "any", &out));

    ns_teardown(dev);
}

/* =========================================================================
 * 14. NAMESPACE LAWS
 * ========================================================================= */

hn4_TEST(Namespace, Law_Identity_Primary) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0};
    vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    hn4_u128_t cpu_id = { .lo = 555, .hi = 0 }; /* 555 = 0x22B */

    hn4_anchor_t a = {0}; 
    a.seed_id = hn4_cpu_to_le128(cpu_id);
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    strncpy((char*)a.inline_buffer, "name_A", 20);
    
    /* FIX: Calculate correct slot for ID lookup */
    uint64_t h = _local_hash_uuid(cpu_id);
    uint64_t slots = (256 * NS_SECTOR_SIZE) / sizeof(hn4_anchor_t);
    uint64_t slot = h % slots;

    _local_write_anchor(dev, &vol.sb, slot, &a);

    hn4_anchor_t out;
    /* ID lookup works */
    const char* id_uri = "id:0000000000000000000000000000022B"; 
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, id_uri, &out));

    /* Simulate Rename: Update Anchor in place */
    strncpy((char*)a.inline_buffer, "name_B", 20);
    _local_write_anchor(dev, &vol.sb, slot, &a);

    /* ID lookup STILL works and sees new name */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, id_uri, &out));
    ASSERT_EQ(0, strncmp((char*)out.inline_buffer, "name_B", 6));

    ns_teardown(dev);
}
hn4_TEST(Namespace, Law_Flat_Space_Collision) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Two files, same name "duplicate.txt", different IDs */
    hn4_anchor_t a1 = {0}; a1.seed_id.lo = 1;
    a1.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    a1.write_gen = hn4_cpu_to_le32(10);
    strncpy((char*)a1.inline_buffer, "duplicate.txt", 20);
    _local_write_anchor(dev, &vol.sb, 0, &a1);

    hn4_anchor_t a2 = {0}; a2.seed_id.lo = 2;
    a2.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    a2.write_gen = hn4_cpu_to_le32(20);
    strncpy((char*)a2.inline_buffer, "duplicate.txt", 20);
    _local_write_anchor(dev, &vol.sb, 1, &a2);

    hn4_anchor_t out;
    /* Flat Space Law: Resolution by name is ambiguous but valid.
       Implementation returns HIGHEST GENERATION. */
    ASSERT_EQ(HN4_OK, hn4_ns_resolve(&vol, "duplicate.txt", &out));
    ASSERT_EQ(2, out.seed_id.lo); /* Gen 20 wins */

    ns_teardown(dev);
}

/* =========================================================================
 * 15. ADVERSARIAL TESTS
 * ========================================================================= */

hn4_TEST(Namespace, Adversary_Ouroboros_Extension) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    vol.vol_block_size = NS_BLOCK_SIZE;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    uint64_t ptr = 2000;
    uint32_t spb = NS_BLOCK_SIZE / NS_SECTOR_SIZE;

    hn4_anchor_t a = {0};
    a.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC | HN4_FLAG_EXTENDED);
    uint64_t le_ptr = hn4_cpu_to_le64(ptr * spb);
    memcpy(a.inline_buffer, &le_ptr, 8);
    strncpy((char*)a.inline_buffer + 8, "loop", 4);
    _local_write_anchor(dev, &vol.sb, 0, &a);

    /* Extension points to ITSELF */
    hn4_extension_header_t e = {0};
    e.magic = hn4_cpu_to_le32(HN4_MAGIC_META);
    e.type = hn4_cpu_to_le32(HN4_EXT_TYPE_LONGNAME);
    e.next_ext_lba = hn4_cpu_to_le64(ptr * spb); /* Ouroboros */
    strcpy((char*)e.payload, "a");
    
    hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_lba_from_blocks(ptr * spb), &e, spb);

    hn4_anchor_t out;
    /* Should fail with NOT_FOUND after depth limit, NOT crash/hang */
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, "loop_forever", &out));

    ns_teardown(dev);
}

hn4_TEST(Namespace, Adversary_Probe_Flood) {
    hn4_hal_device_t* dev = ns_setup();
    hn4_volume_t vol = {0}; vol.target_device = dev;
    hn4_hal_sync_io(dev, HN4_IO_READ, hn4_addr_from_u64(0), &vol.sb, 1);

    /* Fill entire Cortex region with valid (but non-matching) entries */
    /* Cortex is 256 sectors -> 128KB -> 1024 slots */
    hn4_anchor_t fill = {0};
    fill.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_STATIC);
    fill.seed_id.lo = 999; /* Non-matching ID */
    
    /* Fill first 1024 slots */
    for(int i=0; i<1024; i++) {
        _local_write_anchor(dev, &vol.sb, i, &fill);
    }

    hn4_anchor_t out;
    /* Lookup non-existent ID. Should stop after MAX_PROBES (1024) */
    /* If logic is broken, it might loop forever if it wraps */
    const char* id_uri = "id:00000000000000000000000000000001";
    ASSERT_EQ(HN4_ERR_NOT_FOUND, hn4_ns_resolve(&vol, id_uri, &out));

    ns_teardown(dev);
}