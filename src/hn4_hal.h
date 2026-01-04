/*
 * Copyright (c) 2025 Hydra-Nexus Project.
 *
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * COMPONENT:   Hardware Abstraction Layer (HAL)
 * HEADER:      hn4_hal.h
 * STATUS:      FIXED / BARE METAL PRODUCTION
 * MAINTAINER:  Core Storage Infrastructure Team
 *
 * DESCRIPTION:
 * Low-level abstractions for NVM interaction, CPU feature detection,
 * memory barriers, and I/O submission queues.
 */

#ifndef _HN4_HAL_H_
#define _HN4_HAL_H_

#include "hn4.h"
#include "hn4_errors.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 0. LIFECYCLE & ERROR HANDLING
 * ========================================================================= */

/**
 * hn4_hal_init
 * Initializes global HAL state, probes CPU features (CLWB, AVX), and
 * seeds entropy sources. Must be called before any other HN4 routine.
 */
hn4_result_t hn4_hal_init(void);

/**
 * hn4_hal_shutdown
 * Cleanly tears down HAL resources.
 */
void hn4_hal_shutdown(void);

/**
 * hn4_hal_panic
 * Irrecoverable error handler. Spins indefinitely or triggers system reset.
 * @param reason: ASCII string describing the fault.
 */
void hn4_hal_panic(const char* reason);

/* =========================================================================
 * 1. CPU FEATURES & NVM PERSISTENCE
 * ========================================================================= */

extern uint32_t _hn4_cpu_features;

/* CPU Feature Bitmasks */
#define HN4_CPU_X86_CLFLUSH     (1 << 0)
#define HN4_CPU_X86_CLFLUSHOPT  (1 << 1)
#define HN4_CPU_X86_CLWB        (1 << 2)

/* Standard cache line alignment for NVM devices */
#define HN4_CACHE_LINE_SIZE     64

/**
 * hn4_hal_nvm_persist
 * Inline primitive to flush CPU caches to persistent domain.
 * Optimized for hot-path usage.
 */
static inline void hn4_hal_nvm_persist(volatile void* ptr, size_t size);

/* =========================================================================
 * 2. DEVICE CONTEXT & CAPABILITIES
 * ========================================================================= */

/* Opaque handle to a hardware device */
typedef struct hn4_hal_device hn4_hal_device_t;

typedef struct {
    hn4_addr_t  total_capacity_bytes;
    uint32_t    logical_block_size;   /* e.g., 4096 */
    uint32_t    optimal_io_boundary;  /* e.g., 4096 or 128k */
    uint32_t    zone_size_bytes;      /* 0 if not ZNS */
    uint64_t    hw_flags;             /* See hn4.h for HW_NVM/HW_ZNS */
    uint32_t    max_transfer_bytes;   /* DMA limit */
    uint32_t    queue_count;          /* Hardware queues available */
} hn4_hal_caps_t;

/**
 * hn4_hal_get_caps
 * Returns a read-only pointer to the device capabilities.
 * Thread-safe.
 */
const hn4_hal_caps_t* hn4_hal_get_caps(hn4_hal_device_t* dev);

/* =========================================================================
 * 3. MEMORY MANAGEMENT
 * ========================================================================= */

#define HN4_HAL_ALIGNMENT 128

/**
 * hn4_hal_mem_alloc / hn4_hal_mem_free
 * Allocates memory aligned to HN4_HAL_ALIGNMENT (usually 128 bytes)
 * to satisfy DMA strictness and cache alignment.
 */
void* hn4_hal_mem_alloc(size_t size);
void  hn4_hal_mem_free(void* ptr);

/* =========================================================================
 * 4. CONCURRENCY PRIMITIVES
 * ========================================================================= */

typedef struct {
    atomic_flag flag;
    uint32_t    pad; /* Maintain alignment */
} hn4_spinlock_t;

void hn4_hal_spinlock_init(hn4_spinlock_t* lock);
void hn4_hal_spinlock_acquire(hn4_spinlock_t* lock);
void hn4_hal_spinlock_release(hn4_spinlock_t* lock);

/* =========================================================================
 * 5. IO KINETICS & SUBMISSION
 * ========================================================================= */

/* Operation Codes */
#define HN4_IO_READ             0
#define HN4_IO_WRITE            1
#define HN4_IO_FLUSH            2
#define HN4_IO_DISCARD          3
#define HN4_IO_ZONE_APPEND      4
#define HN4_IO_ZONE_RESET       5

typedef struct {
    uint8_t     op_code;
    uint8_t     flags;        /* FUA, Priority, etc. */
    uint16_t    queue_id;     /* Submission Queue ID */
    hn4_addr_t  lba;          /* Starting Logical Block Address */
    void*       buffer;       /* DMA-able buffer pointer */
    uint32_t    length;       /* Length in BLOCKS (not bytes) */
    void*       user_ctx;     /* Passthrough context for callback */
    hn4_addr_t  result_lba;   /* Output: Filled on Zone Append */
} hn4_io_req_t;

/* Async completion callback signature */
typedef void (*hn4_io_callback_t)(hn4_io_req_t* req, hn4_result_t result);

/**
 * hn4_hal_submit_io
 * Asynchronous I/O submission.
 * @param dev: Device handle
 * @param req: Request structure (caller must keep alive until cb)
 * @param cb:  Completion callback
 */
void hn4_hal_submit_io(hn4_hal_device_t* dev, 
                       hn4_io_req_t* req, 
                       hn4_io_callback_t cb);

/**
 * hn4_hal_sync_io
 * Blocking wrapper around async submission.
 * WARNING: Spins on completion. Do not call from ISR.
 */
hn4_result_t hn4_hal_sync_io(hn4_hal_device_t* dev, 
                             uint8_t op, 
                             hn4_addr_t lba, 
                             void* buf, 
                             uint32_t len_blocks);

/**
 * hn4_hal_poll
 * Polling hook for drivers that require manual completion harvesting.
 */
void hn4_hal_poll(hn4_hal_device_t* dev);

/* =========================================================================
 * 6. TELEMETRY & UTILITIES
 * ========================================================================= */

hn4_time_t hn4_hal_get_time_ns(void);
uint32_t   hn4_hal_get_temperature(hn4_hal_device_t* dev); /* Celsius */
void       hn4_hal_micro_sleep(uint32_t us);
uint64_t   hn4_hal_get_random_u64(void);

/* =========================================================================
 * INLINE IMPLEMENTATION: NVM PERSIST
 * ========================================================================= */

static inline void hn4_hal_nvm_persist(volatile void* ptr, size_t size) {
    /*
     * Compiler barrier: Prevent reordering of stores across this point.
     * We must ensure data is actually in the store buffers before flushing.
     */
    atomic_signal_fence(memory_order_release);

#if defined(__x86_64__) || defined(_M_X64)
    uintptr_t addr = (uintptr_t)ptr;
    const uintptr_t end = addr + size;

    /* Align start address to cache line boundary */
    addr &= ~(HN4_CACHE_LINE_SIZE - 1);

    while (addr < end) {
        /*
         * Note: In a real deployment, we would check _hn4_cpu_features
         * to decide between CLFLUSH, CLFLUSHOPT, or CLWB.
         * Defaulting to CLFLUSH for compatibility/simplicity here.
         */
        __asm__ volatile("clflush (%0)" :: "r"(addr) : "memory");
        addr += HN4_CACHE_LINE_SIZE;
    }
    
    /* SFENCE ensures the flush instructions generally complete */
    __asm__ volatile("sfence" ::: "memory");

#elif defined(__aarch64__) || defined(_M_ARM64)
    /* ARM64 Data Cache Clean to Point of Persistence */
    uintptr_t addr = (uintptr_t)ptr;
    const uintptr_t end = addr + size;
    addr &= ~(HN4_CACHE_LINE_SIZE - 1);
    
    while (addr < end) {
        __asm__ volatile("dc cvap, %0" : : "r" (addr) : "memory");
        addr += HN4_CACHE_LINE_SIZE;
    }
    __asm__ volatile("dsb ish" ::: "memory");

#else
    /* Fallback: Standard atomic fence (performance penalty likely) */
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

/**
 * hn4_hal_barrier
 * Issues a generic storage barrier (FLUSH/FUA) to ensure ordering.
 */
hn4_result_t hn4_hal_barrier(hn4_hal_device_t* dev);

/**
 * hn4_hal_sync_io_large
 * 
 * Handles I/O requests that may exceed the HAL's maximum transfer size
 * by splitting them into smaller, aligned chunks.
 * 
 * @param len_bytes   Total length in BYTES (API mismatch adapter)
 * @param block_size  Logical block size for alignment calculation
 */
hn4_result_t hn4_hal_sync_io_large(hn4_hal_device_t* dev, 
                                   uint8_t op, 
                                   hn4_addr_t start_lba, 
                                   void* buf, 
                                   uint64_t len_bytes,
                                   uint32_t block_size);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_HAL_H_ */