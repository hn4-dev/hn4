/*
 * Copyright (c) 2025 Hydra-Nexus Project.
 *
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * COMPONENT:   Hardware Abstraction Layer (HAL)
 * SOURCE:      hn4_hal.c
 * STATUS:      FIXED / LINKING
 */

#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include <stdlib.h>     /* malloc/free (mapped to platform heap) */
#include <string.h>     /* memcpy/memset */
#include <stdatomic.h>
#include <stdbool.h>

/* =========================================================================
 * 0. CONSTANTS & INTERNAL DEFINITIONS
 * ========================================================================= */

/* ZNS Simulation Constants (used if no physical ZNS drive found) */
#define ZNS_SIM_ZONES       64
#define ZNS_SIM_ZONE_SIZE   (256ULL * 1024 * 1024)
#define ZNS_SIM_SECTOR_SIZE 4096

/* Memory Allocator Magic for corruption detection */
static const uint32_t HN4_MEM_MAGIC = 0x484E3421; /* "HN4!" */

/* Architecture Detection & Yield Macros */
#ifndef unlikely
    #if defined(__GNUC__) || defined(__clang__)
        #define unlikely(x) __builtin_expect(!!(x), 0)
    #else
        #define unlikely(x) (x)
    #endif
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define HN4_ARCH_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define HN4_YIELD() _mm_pause()
    #else
        #include <immintrin.h>
        #include <cpuid.h>
        #define HN4_YIELD() _mm_pause()
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define HN4_ARCH_ARM64
    #define HN4_YIELD() __asm__ volatile("yield" ::: "memory")
#else
    #define HN4_YIELD() atomic_signal_fence(memory_order_seq_cst)
#endif

/* =========================================================================
 * 1. INTERNAL STRUCTURES & GLOBALS
 * ========================================================================= */

/* 
 * Concrete implementation of the opaque hn4_hal_device_t.
 */
struct hn4_hal_device {
    hn4_hal_caps_t caps;       /* Public capabilities */
    uint8_t*       mmio_base;  /* Base address for NVM/DAX devices */
    void*          driver_ctx; /* Internal driver context (e.g., NVMe rings) */
};

/* Global State */
static atomic_bool      _hal_initialized = false;
static _Atomic uint64_t _prng_seed;
static _Atomic uint64_t _zns_zone_ptrs[ZNS_SIM_ZONES]; /* Write pointers for ZNS sim */

/* CPU Features Globals */
uint32_t _hn4_cpu_features = 0;

/* =========================================================================
 * 2. INITIALIZATION & HELPERS
 * ========================================================================= */

static void _probe_cpu_persistence_features(void) {
    _hn4_cpu_features = 0;
#if defined(HN4_ARCH_X86)
    int info[4];
    
    /* Standard Leaf 1: Check CLFLUSH */
#if defined(_MSC_VER)
    __cpuid(info, 1);
#else
    __cpuid(1, info[0], info[1], info[2], info[3]);
#endif
    if (info[3] & (1 << 19)) _hn4_cpu_features |= HN4_CPU_X86_CLFLUSH;

    /* Extended Leaf 7: Check CLFLUSHOPT / CLWB */
    info[0]=0; info[1]=0; info[2]=0; info[3]=0;
#if defined(_MSC_VER)
    __cpuidex(info, 7, 0);
#else
    __cpuid_count(7, 0, info[0], info[1], info[2], info[3]);
#endif
    if (info[1] & (1 << 23)) _hn4_cpu_features |= HN4_CPU_X86_CLFLUSHOPT;
    if (info[1] & (1 << 24)) _hn4_cpu_features |= HN4_CPU_X86_CLWB;
#endif
}

static inline void _assert_hal_init(void) {
    if (unlikely(!atomic_load_explicit(&_hal_initialized, memory_order_acquire))) {
        hn4_hal_panic("HN4 HAL Not Initialized");
    }
}

hn4_result_t hn4_hal_init(void) {
    bool expected = false;
    /* Compare-Exchange guarantees single initialization */
    if (!atomic_compare_exchange_strong(&_hal_initialized, &expected, true)) {
        return HN4_OK; /* Already initialized */
    }

    _probe_cpu_persistence_features();
    
    /* Reset ZNS Simulation Pointers */
    for (int i = 0; i < ZNS_SIM_ZONES; i++) {
        atomic_store(&_zns_zone_ptrs[i], 0);
    }
    
    /* Seed PRNG with a non-zero value */
    atomic_store(&_prng_seed, 0xCAFEBABE12345678ULL);
    
    return HN4_OK;
}

void hn4_hal_shutdown(void) {
    atomic_store(&_hal_initialized, false);
}

void hn4_hal_panic(const char* reason) {
    (void)reason;
    /* 
     * In a production kernel/bare-metal env, this would write to 
     * UART or BMC SOS registers. Here we hang.
     */
    while(1) { HN4_YIELD(); }
}

/* =========================================================================
 * 3. IO SUBMISSION LOGIC
 * ========================================================================= */

void hn4_hal_submit_io(hn4_hal_device_t* dev, hn4_io_req_t* req, hn4_io_callback_t cb) {
    _assert_hal_init();
    
    if (unlikely(!dev || !req)) {
        if (cb) cb(req, HN4_ERR_INVALID_ARGUMENT);
        return;
    }

    /* ---------------------------------------------------------------------
     * PATH A: NVM / MEMORY MAPPED IO
     * Direct memcpy with persistence barriers.
     * --------------------------------------------------------------------- */
    if (dev->caps.hw_flags & HN4_HW_NVM) {
        
        if (unlikely(!dev->mmio_base)) {
            if (cb) cb(req, HN4_ERR_INTERNAL_FAULT);
            return;
        }

        /* Calculate offsets. Assumes flat addressing for NVM. */
        uint64_t lba_raw = hn4_addr_to_u64(req->lba);
        uint64_t offset = lba_raw * dev->caps.logical_block_size;
        uint32_t len_bytes = req->length * dev->caps.logical_block_size;

        switch (req->op_code) {
            case HN4_IO_READ:
                memcpy(req->buffer, dev->mmio_base + offset, len_bytes);
                break;

            case HN4_IO_WRITE:
                memcpy(dev->mmio_base + offset, req->buffer, len_bytes);
                /* Strict persistence requirement for NVM Writes */
                hn4_hal_nvm_persist(dev->mmio_base + offset, len_bytes);
                break;

            case HN4_IO_FLUSH:
                /* Global persistence barrier */
                atomic_thread_fence(memory_order_release);
                #if defined(HN4_ARCH_X86)
                    __asm__ volatile("sfence" ::: "memory");
                #elif defined(HN4_ARCH_ARM64)
                    __asm__ volatile("dsb ish" ::: "memory");
                #endif
                break;

            case HN4_IO_DISCARD:
                /* Advisory only on NVM, usually a no-op */
                break;
                
            case HN4_IO_ZONE_RESET:
                /* Emulate Zone Reset by zeroing the range */
                memset(dev->mmio_base + offset, 0, len_bytes);
                hn4_hal_nvm_persist(dev->mmio_base + offset, len_bytes);
                break;
                
            default:
                if (cb) cb(req, HN4_ERR_INVALID_ARGUMENT);
                return;
        }

        req->result_lba = req->lba;
        atomic_thread_fence(memory_order_release);
        if (cb) cb(req, HN4_OK);
        return;
    }

    /* ---------------------------------------------------------------------
     * PATH B: ZNS SIMULATION / BLOCK IO
     * Simulates append logic using atomic counters.
     * --------------------------------------------------------------------- */

    if (req->op_code == HN4_IO_ZONE_APPEND) {
        uint64_t lba_raw = hn4_addr_to_u64(req->lba);
        
        /* Calculate Zone ID based on fixed simulation sizes */
        uint64_t zone_cap_blocks = (ZNS_SIM_ZONE_SIZE / ZNS_SIM_SECTOR_SIZE);
        uint64_t zone_idx = lba_raw / zone_cap_blocks;
        uint64_t zone_start_lba = zone_idx * zone_cap_blocks;
        uint64_t sim_idx = zone_idx % ZNS_SIM_ZONES;

        /* Atomically reserve space in the zone (Zone Append Semantics) */
        uint64_t offset_blocks = atomic_fetch_add(&_zns_zone_ptrs[sim_idx], req->length);
        uint64_t final_lba = zone_start_lba + offset_blocks;

        /* Check for Zone Overflow */
        if (offset_blocks + req->length > zone_cap_blocks) {
             /* Rollback is complex in lockless, treating as error for Sim */
             if (cb) cb(req, HN4_ERR_ZONE_FULL);
             return;
        }

        req->result_lba = hn4_addr_from_u64(final_lba);
    } else {
        req->result_lba = req->lba;
    }

    /* Simulate async latency? No, we are bare metal fast path. */
    atomic_thread_fence(memory_order_release);
    if (cb) cb(req, HN4_OK);
}

/* =========================================================================
 * 4. MEMORY MANAGEMENT
 * ========================================================================= */

typedef struct {
    uint32_t magic;
    void*    raw_ptr;
    uint64_t _pad;   /* Keep header 16-byte aligned */
} alloc_header_t;

void* hn4_hal_mem_alloc(size_t size) {
    _assert_hal_init();
    if (size == 0) return NULL;

    /* Pad for alignment + header space */
    size_t total = size + HN4_HAL_ALIGNMENT + sizeof(alloc_header_t);
    void* raw = malloc(total);
    if (!raw) return NULL;
    
    /* Calculate aligned address */
    uintptr_t aligned_addr = ((uintptr_t)raw + sizeof(alloc_header_t) + (HN4_HAL_ALIGNMENT - 1)) 
                             & ~((uintptr_t)HN4_HAL_ALIGNMENT - 1);
    void* ptr = (void*)aligned_addr;
    
    /* Store header immediately preceding the aligned pointer */
    alloc_header_t* h = (alloc_header_t*)((uint8_t*)ptr - sizeof(alloc_header_t));
    h->magic   = HN4_MEM_MAGIC;
    h->raw_ptr = raw;
    
    /* Safety: Zero memory */
    memset(ptr, 0, size);
    return ptr;
}

void hn4_hal_mem_free(void* ptr) {
    if (!ptr) return;
    
    alloc_header_t* h = (alloc_header_t*)((uint8_t*)ptr - sizeof(alloc_header_t));
    if (unlikely(h->magic != HN4_MEM_MAGIC)) {
        hn4_hal_panic("HN4 Heap Corruption: Invalid Free");
    }
    
    h->magic = 0xDEADBEEF; /* Poison */
    free(h->raw_ptr);
}

/* =========================================================================
 * 5. SYNC IO & EXTENDED HELPERS
 * ========================================================================= */

/* Sync Context for stack-based waiting */
typedef struct { 
    volatile bool         done; 
    volatile hn4_result_t res; 
} sync_ctx_t;

static void _sync_cb(hn4_io_req_t* r, hn4_result_t res) {
    sync_ctx_t* ctx = (sync_ctx_t*)r->user_ctx;
    ctx->res = res;
    atomic_thread_fence(memory_order_release);
    ctx->done = true;
}

hn4_result_t hn4_hal_sync_io(hn4_hal_device_t* dev, uint8_t op, hn4_addr_t lba, void* buf, uint32_t len) {
    sync_ctx_t ctx = { .done = false, .res = HN4_OK };
    hn4_io_req_t req = {0};
    
    req.op_code  = op; 
    req.lba      = lba; 
    req.buffer   = buf; 
    req.length   = len; 
    req.user_ctx = &ctx;
    
    hn4_hal_submit_io(dev, &req, _sync_cb);
    
    /* Spin-wait for completion */
    while(!ctx.done) { 
        HN4_YIELD(); 
        hn4_hal_poll(dev); /* Help the driver if it's polled mode */
    }
    
    atomic_thread_fence(memory_order_acquire);
    return ctx.res;
}

hn4_result_t hn4_hal_barrier(hn4_hal_device_t* dev) {
    /* Issue a 0-length flush command */
    return hn4_hal_sync_io(dev, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
}

hn4_result_t hn4_hal_sync_io_large(hn4_hal_device_t* dev, 
                                   uint8_t op, 
                                   hn4_addr_t start_lba, 
                                   void* buf, 
                                   uint64_t len_bytes,
                                   uint32_t block_size)
{
    if (unlikely(block_size == 0)) return HN4_ERR_INVALID_ARGUMENT;

    /* Ensure request is block-aligned */
    if ((len_bytes & ((uint64_t)block_size - 1)) != 0) {
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    /* 2GB safe chunk limit to avoid overflow in legacy 32-bit DMA descriptors */
    const uint64_t MAX_CHUNK_BYTES = 0x80000000ULL; 
    
    uint64_t remaining_bytes = len_bytes;
    uint64_t byte_offset = 0;
    uint8_t* ptr = (uint8_t*)buf;

    while (remaining_bytes > 0) {
        uint32_t chunk_bytes = (remaining_bytes > MAX_CHUNK_BYTES) 
                             ? (uint32_t)MAX_CHUNK_BYTES 
                             : (uint32_t)remaining_bytes;
        
        /* HAL expects length in blocks */
        uint32_t chunk_blocks = chunk_bytes / block_size;
        
        hn4_addr_t current_lba = hn4_addr_add(start_lba, byte_offset / block_size);
        void*      current_buf = ptr ? (ptr + byte_offset) : NULL;

        hn4_result_t res = hn4_hal_sync_io(dev, op, current_lba, current_buf, chunk_blocks);
        if (res != HN4_OK) return res;

        remaining_bytes -= chunk_bytes;
        byte_offset     += chunk_bytes;
        
        /* 
         * Preventive Yield:
         * If we are moving massive amounts of data synchronously, we must yield 
         * to prevent locking up the core if we are in a non-preemptive environment.
         */
        if (chunk_blocks > 1024) HN4_YIELD();
    }

    return HN4_OK;
}

/* =========================================================================
 * 6. TELEMETRY, LOCKS & CAPS
 * ========================================================================= */

hn4_time_t hn4_hal_get_time_ns(void) {
    /* 
     * Mock implementation for bare metal.
     * Real implementation would read TSC and scale by frequency.
     */
    static _Atomic uint64_t ticks = 0;
    return (hn4_time_t)atomic_fetch_add(&ticks, 100);
}

uint64_t hn4_hal_get_random_u64(void) {
    _assert_hal_init();
    /* Linear Congruential Generator (LCG) - fast, good enough for non-crypto */
    uint64_t c = atomic_load_explicit(&_prng_seed, memory_order_relaxed);
    uint64_t n = c * 6364136223846793005ULL + 1;
    atomic_store_explicit(&_prng_seed, n, memory_order_relaxed);
    return n;
}

const hn4_hal_caps_t* hn4_hal_get_caps(hn4_hal_device_t* dev) {
    _assert_hal_init();
    if (!dev) return NULL;
    return &dev->caps;
}

/* Stubs for sensor/misc functions */
void hn4_hal_poll(hn4_hal_device_t* d) { (void)d; HN4_YIELD(); }
uint32_t hn4_hal_get_temperature(hn4_hal_device_t* d) { (void)d; return 40; } /* Nominal */
void hn4_hal_micro_sleep(uint32_t us) { (void)us; HN4_YIELD(); }

/* Spinlock Implementation */
void hn4_hal_spinlock_init(hn4_spinlock_t* l) { 
    atomic_flag_clear(&l->flag); 
}

void hn4_hal_spinlock_acquire(hn4_spinlock_t* l) { 
    while(atomic_flag_test_and_set_explicit(&l->flag, memory_order_acquire)) {
        HN4_YIELD();
    }
}

void hn4_hal_spinlock_release(hn4_spinlock_t* l) { 
    atomic_flag_clear_explicit(&l->flag, memory_order_release); 
}