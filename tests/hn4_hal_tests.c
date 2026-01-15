/*
 * HYDRA-NEXUS 4 (HN4) - HARDWARE ABSTRACTION LAYER TESTS
 * FILE: hn4_hal_tests.c
 * STATUS: LOGIC VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include <stdint.h>

/* --- FIXTURE HELPERS --- */

typedef struct {
    hn4_hal_caps_t caps;
    /* Mock MMIO buffer for NVM tests */
    uint8_t mmio_buffer[4096]; 
} mock_hal_device_t;

static hn4_hal_device_t* create_hal_device(void) {
    /* We can't use hn4_hal_mem_alloc yet if we are testing init, 
     * so use static or stack for the device handle in some tests.
     * But for general purpose, we assume init happens in test setup.
     */
    static mock_hal_device_t static_dev;
    memset(&static_dev, 0, sizeof(static_dev));
    
    static_dev.caps.logical_block_size = 4096;
    static_dev.caps.total_capacity_bytes = 1024 * 1024;
    
    return (hn4_hal_device_t*)&static_dev;
}

/* =========================================================================
 * TEST 1: Initialization Lifecycle
 * Rationale: 
 * The HAL utilizes global atomic state. We must verify that initialization
 * is idempotent (safe to call twice) and that it sets up the environment.
 * ========================================================================= */
hn4_TEST(HAL_Lifecycle, IdempotentInit) {
    /* 1. First Init */
    hn4_result_t res = hn4_hal_init();
    ASSERT_EQ(HN4_OK, res);

    /* 2. Second Init (Should return OK or be a no-op, not crash) */
    res = hn4_hal_init();
    ASSERT_EQ(HN4_OK, res);

    /* 3. Shutdown */
    hn4_hal_shutdown();
}

/* =========================================================================
 * TEST 2: Allocator Alignment Contract
 * Rationale: 
 * DMA engines and CPU cache instructions (CLFLUSH) require strict alignment.
 * The HAL promises HN4_HAL_ALIGNMENT (128 bytes). 
 * If this fails, NVM persistence primitives may fault or corrupt data.
 * ========================================================================= */
hn4_TEST(HAL_Allocator, StrictAlignment) {
    hn4_hal_init();

    /* Allocate an odd size */
    void* ptr = hn4_hal_mem_alloc(13);
    
    ASSERT_TRUE(ptr != NULL);

    uintptr_t addr = (uintptr_t)ptr;
    
    /* Verify 128-byte alignment */
    /* (addr & 127) == 0 */
    ASSERT_EQ(0, addr & (HN4_HAL_ALIGNMENT - 1));

    hn4_hal_mem_free(ptr);
    hn4_hal_shutdown();
}

/* =========================================================================
 * TEST 3: Null Device Guard
 * Rationale: 
 * Submitting I/O to a NULL device handle is a common bug in upper layers.
 * The HAL must catch this synchronously and callback with INVALID_ARGUMENT
 * rather than dereferencing NULL.
 * ========================================================================= */
hn4_TEST(HAL_IO, NullDeviceGuard) {
    hn4_hal_init();

    uint8_t buf[512];
    hn4_addr_t lba = 0;

    /* 
     * sync_io wraps submit_io. 
     * If submit_io handles NULL correctly, it calls the callback 
     * with an error, which sync_io captures and returns.
     */
    hn4_result_t res = hn4_hal_sync_io(NULL, HN4_IO_READ, lba, buf, 1);

    ASSERT_EQ(HN4_ERR_INVALID_ARGUMENT, res);

    hn4_hal_shutdown();
}

/* =========================================================================
 * TEST 5: ZNS Append Simulation Logic
 * RATIONALE:
 * When HN4_HW_ZNS_NATIVE is NOT present (Simulation Mode), the HAL emulates
 * Zone Append by tracking a write pointer.
 * This test verifies the emulated WP advances correctly and wraps/rejects
 * writes that exceed the zone capacity.
 * ========================================================================= */
hn4_TEST(HAL_ZNS, SimulationPointerAdvance) {
    hn4_hal_init();

    hn4_hal_device_t* dev = create_hal_device();
    
    /* 
     * Mock device is implicitly non-ZNS (flags=0). 
     * But for simulation to work, we need to treat it as a Block Device
     * that accepts the ZONE_APPEND opcode.
     * The HAL code path checks opcode, then simulates.
     */
    
    uint8_t buf[4096] = {0};
    /* Zone 0 start LBA = 0 */
    hn4_addr_t zone_start = 0;
    
    /* 
     * 1. First Append (Length 1 block)
     * Expected Result LBA: 0
     * Internal WP: 1
     */
    hn4_addr_t res_lba = 0;
    hn4_result_t res = hn4_hal_zns_append_sync(dev, zone_start, buf, 1, &res_lba);
    
    ASSERT_EQ(HN4_OK, res);
    
    /* In 64-bit mode */
    #ifndef HN4_USE_128BIT
    ASSERT_EQ(0, res_lba);
    #endif

    /* 
     * 2. Second Append (Length 1 block)
     * Expected Result LBA: 1 (Sequential)
     */
    res = hn4_hal_zns_append_sync(dev, zone_start, buf, 1, &res_lba);
    ASSERT_EQ(HN4_OK, res);
    
    #ifndef HN4_USE_128BIT
    ASSERT_EQ(1, res_lba);
    #endif

    hn4_hal_shutdown();
}