/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * MODULE:      Hardware Abstraction Layer (HAL)
 * HEADER:      hn4_hal.h
 * STATUS:      FIXED / BARE METAL PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * PERSISTENCE CONTRACT:
 * Assumes ADR/eADR platform where Cache Flush + Fence ensures
 * durability on power loss.
 */

#ifndef _HN4_HAL_H_
#define _HN4_HAL_H_

#include "hn4.h"
#include "hn4_errors.h"
#include <stddef.h>
#include <stdbool.h>

#if defined(__STDC_NO_ATOMICS__)
    #error "HN4 requires C11 Atomics support. Please enable C11 mode or link -latomic."
#else
    #include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 0. LIFECYCLE & ERROR HANDLING
 * ========================================================================= */

hn4_result_t hn4_hal_init(void);
void hn4_hal_shutdown(void);
void hn4_hal_panic(const char* reason);

/* =========================================================================
 * 1. CPU FEATURES & NVM PERSISTENCE
 * ========================================================================= */

extern uint32_t _hn4_cpu_features;

#define HN4_CPU_X86_CLFLUSH     (1 << 0)
#define HN4_CPU_X86_CLFLUSHOPT  (1 << 1)
#define HN4_CPU_X86_CLWB        (1 << 2)

#define HN4_CACHE_LINE_SIZE     64
#define HN4_GPU_ID_NONE 0xFFFFFFFFU
/**
 * hn4_hal_nvm_persist
 * Inline primitive to flush CPU caches to persistent domain.
 * 
 * ARM64 IMPLEMENTATION NOTE:
 * Uses DC CVAP (Clean to Point of Persistence). This requires the backing
 * memory to be mapped as Normal Memory and the system to support the
 * PoP concept (ARMv8.2-A+).
 */
static inline void hn4_hal_nvm_persist(volatile void* ptr, size_t size);

/* =========================================================================
 * 2. DEVICE CONTEXT & CAPABILITIES
 * ========================================================================= */

typedef struct hn4_hal_device hn4_hal_device_t;

typedef struct {
    hn4_addr_t  total_capacity_bytes;
    uint32_t    logical_block_size;   
    uint32_t    optimal_io_boundary;  
    uint32_t    zone_size_bytes;      
    uint64_t    hw_flags;             
    uint32_t    max_transfer_bytes;   
    uint32_t    queue_count;          
} hn4_hal_caps_t;

const hn4_hal_caps_t* hn4_hal_get_caps(hn4_hal_device_t* dev);

/* =========================================================================
 * 3. MEMORY MANAGEMENT
 * ========================================================================= */

#define HN4_HAL_ALIGNMENT 128

void* hn4_hal_mem_alloc(size_t size);
void  hn4_hal_mem_free(void* ptr);

/* =========================================================================
 * 4. CONCURRENCY PRIMITIVES
 * ========================================================================= */

void hn4_hal_spinlock_init(hn4_spinlock_t* lock);
void hn4_hal_spinlock_acquire(hn4_spinlock_t* lock);
void hn4_hal_spinlock_release(hn4_spinlock_t* lock);

/* =========================================================================
 * 5. IO KINETICS & SUBMISSION
 * ========================================================================= */

#define HN4_IO_READ             0
#define HN4_IO_WRITE            1
#define HN4_IO_FLUSH            2
#define HN4_IO_DISCARD          3
#define HN4_IO_ZONE_APPEND      4
#define HN4_IO_ZONE_RESET       5

typedef struct {
    uint8_t     op_code;
    uint8_t     flags;        
    uint16_t    queue_id;     
    hn4_addr_t  lba;          
    void*       buffer;       
    uint32_t    length;       /* Length in BLOCKS */
    void*       user_ctx;     
    hn4_addr_t  result_lba;   
} hn4_io_req_t;

typedef void (*hn4_io_callback_t)(hn4_io_req_t* req, hn4_result_t result);

void hn4_hal_submit_io(hn4_hal_device_t* dev, 
                       hn4_io_req_t* req, 
                       hn4_io_callback_t cb);

hn4_result_t hn4_hal_sync_io(hn4_hal_device_t* dev, 
                             uint8_t op, 
                             hn4_addr_t lba, 
                             void* buf, 
                             uint32_t len_blocks);

void hn4_hal_poll(hn4_hal_device_t* dev);

/* =========================================================================
 * 6. TELEMETRY & UTILITIES
 * ========================================================================= */

hn4_time_t hn4_hal_get_time_ns(void);
uint32_t   hn4_hal_get_temperature(hn4_hal_device_t* dev);
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

    addr &= ~(HN4_CACHE_LINE_SIZE - 1);

    while (addr < end) {
        /*
         * Note: In a real deployment, check _hn4_cpu_features
         * to decide between CLFLUSH, CLFLUSHOPT, or CLWB.
         */
        __asm__ volatile("clflush (%0)" :: "r"(addr) : "memory");
        addr += HN4_CACHE_LINE_SIZE;
    }
    
    /* SFENCE ensures the flush instructions complete */
    __asm__ volatile("sfence" ::: "memory");

#elif defined(__aarch64__) || defined(_M_ARM64)
    /* 
     * ARM64 Data Cache Clean to Point of Persistence (CVAP).
     * Requires ARMv8.2-A extensions.
     * If CVAP is not available, this should fall back to CIVAC (Clean/Invalidate to PoC).
     */
    uintptr_t addr = (uintptr_t)ptr;
    const uintptr_t end = addr + size;
    addr &= ~(HN4_CACHE_LINE_SIZE - 1);
    
    /* Ensure all previous stores are observed before the clean */
    __asm__ volatile("dsb ish" ::: "memory");

    while (addr < end) {
        /* DC CVAP: Data Cache Clean by VA to Point of Persistence */
        __asm__ volatile("dc cvap, %0" : : "r" (addr) : "memory");
        addr += HN4_CACHE_LINE_SIZE;
    }

    /* Ensure the cleaning ops complete */
    __asm__ volatile("dsb ish" ::: "memory");

#else
    /* Fallback: Standard atomic fence (performance penalty likely) */
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

hn4_result_t hn4_hal_barrier(hn4_hal_device_t* dev);

hn4_result_t hn4_hal_sync_io_large(hn4_hal_device_t* dev, 
                                   uint8_t op, 
                                   hn4_addr_t start_lba, 
                                   void* buf, 
                                   hn4_size_t len_bytes,
                                   uint32_t block_size);

/**
 * hn4_hal_get_calling_gpu_id
 * 
 * Returns the PCI ID or unique index of the GPU/Accelerator context 
 * triggering the current syscall. Returns 0xFFFFFFFF if CPU context.
 */
uint32_t hn4_hal_get_calling_gpu_id(void);

/**
 * hn4_hal_get_topology_count
 * Returns the number of Affinity Regions (NUMA nodes / PCIe Switches) available.
 */
uint32_t hn4_hal_get_topology_count(hn4_hal_device_t* dev);

/**
 * hn4_hal_get_topology_data
 * Fills the provided buffer with the Affinity Map (hn4_volume_t::topo_map format).
 */
hn4_result_t hn4_hal_get_topology_data(hn4_hal_device_t* dev, void* buffer, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_HAL_H_ */