/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * MODULE:      Hardware Abstraction Layer (HAL)
 * SOURCE:      hn4_hal.c
 * STATUS:      HARDENED / PRODUCTION
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Implements low-level IO submission, memory management, and
 * architecture-specific barriers (x86/ARM64/ZNS).
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

/* ZNS Simulation Constants */
#define ZNS_SIM_ZONES       64
#define ZNS_SIM_ZONE_SIZE   (256ULL * 1024 * 1024)
#define ZNS_SIM_SECTOR_SIZE 4096

/* Memory Allocator Magic */
static const uint32_t HN4_MEM_MAGIC = 0x484E3421; /* "HN4!" */

/* Architecture Detection & Yield Macros */

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

struct hn4_hal_device {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
};

/* Global State */
static atomic_bool      _hal_initialized = false;
static _Atomic uint64_t _prng_seed;
static _Atomic uint64_t _zns_zone_ptrs[ZNS_SIM_ZONES];

uint32_t _hn4_cpu_features = 0;

/* =========================================================================
 * 2. INITIALIZATION & HELPERS
 * ========================================================================= */

static void _probe_cpu_persistence_features(void)
{
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
    info[0] = 0; info[1] = 0; info[2] = 0; info[3] = 0;
#if defined(_MSC_VER)
    __cpuidex(info, 7, 0);
#else
    __cpuid_count(7, 0, info[0], info[1], info[2], info[3]);
#endif
    if (info[1] & (1 << 23)) _hn4_cpu_features |= HN4_CPU_X86_CLFLUSHOPT;
    if (info[1] & (1 << 24)) _hn4_cpu_features |= HN4_CPU_X86_CLWB;
#endif
}

static inline void _assert_hal_init(void)
{
    if (HN4_UNLIKELY(!atomic_load_explicit(&_hal_initialized, memory_order_acquire))) {
        hn4_hal_panic("HN4 HAL Not Initialized");
    }
}

hn4_result_t hn4_hal_init(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&_hal_initialized, &expected, true)) {
        return HN4_OK;
    }

    _probe_cpu_persistence_features();

    for (int i = 0; i < ZNS_SIM_ZONES; i++) {
        atomic_store(&_zns_zone_ptrs[i], 0);
    }

    atomic_store(&_prng_seed, 0xCAFEBABE12345678ULL);

    return HN4_OK;
}

void hn4_hal_shutdown(void)
{
    atomic_store(&_hal_initialized, false);
}

void hn4_hal_panic(const char* reason)
{
    (void)reason;
    /* In production, this would write to SOS registers/BMC. */
    while (1) { HN4_YIELD(); }
}

/* =========================================================================
 * 3. IO SUBMISSION LOGIC
 * ========================================================================= */

void hn4_hal_submit_io(hn4_hal_device_t* dev, hn4_io_req_t* req, hn4_io_callback_t cb)
{
    _assert_hal_init();

    if (HN4_UNLIKELY(!dev || !req)) {
        if (cb) cb(req, HN4_ERR_INVALID_ARGUMENT);
        return;
    }

    /* ---------------------------------------------------------------------
     * PATH A: NVM / MEMORY MAPPED IO
     * --------------------------------------------------------------------- */
    if (dev->caps.hw_flags & HN4_HW_NVM) {

        if (HN4_UNLIKELY(!dev->mmio_base)) {
            if (cb) cb(req, HN4_ERR_INTERNAL_FAULT);
            return;
        }

        uint64_t lba_raw   = hn4_addr_to_u64(req->lba);
        uint64_t offset    = lba_raw * dev->caps.logical_block_size;
        uint32_t len_bytes = req->length * dev->caps.logical_block_size;

        /*
         * MMIO Bounds Checking
         * Prevent UB/Segfaults by validating against reported capacity.
         */
        uint64_t max_cap = hn4_addr_to_u64(dev->caps.total_capacity_bytes);

        if (HN4_UNLIKELY((offset + len_bytes) > max_cap)) {
            if (cb) cb(req, HN4_ERR_HW_IO); /* Simulate EIO/Segfault prevention */
            return;
        }

        switch (req->op_code) {
            case HN4_IO_READ:
                memcpy(req->buffer, dev->mmio_base + offset, len_bytes);
                break;

            case HN4_IO_WRITE:
                memcpy(dev->mmio_base + offset, req->buffer, len_bytes);
                /*
                 * Flush Semantics
                 * This ensures data reaches the persistence domain.
                 * ASSUMPTION: Platform supports ADR/eADR or equivalent.
                 */
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
                break;

            case HN4_IO_ZONE_RESET:
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
     * --------------------------------------------------------------------- */

    if (req->op_code == HN4_IO_ZONE_APPEND) {
        uint64_t lba_raw = hn4_addr_to_u64(req->lba);

        /* Calculate Zone ID based on fixed simulation sizes */
        uint64_t zone_cap_blocks = (ZNS_SIM_ZONE_SIZE / ZNS_SIM_SECTOR_SIZE);
        uint64_t zone_idx        = lba_raw / zone_cap_blocks;
        uint64_t zone_start_lba  = zone_idx * zone_cap_blocks;
        uint64_t sim_idx         = zone_idx % ZNS_SIM_ZONES;

        /*
         * CAS Loop for Append Overflow
         * Replaces atomic_fetch_add to prevent pointer leakage past end-of-zone.
         */
        uint64_t old_offset, new_offset;

        do {
            old_offset = atomic_load(&_zns_zone_ptrs[sim_idx]);

            if (old_offset + req->length > zone_cap_blocks) {
                /* Zone is Full */
                if (cb) cb(req, HN4_ERR_ZONE_FULL);
                return;
            }
            new_offset = old_offset + req->length;

        } while (!atomic_compare_exchange_weak(&_zns_zone_ptrs[sim_idx], &old_offset, new_offset));

        uint64_t final_lba = zone_start_lba + old_offset;
        req->result_lba = hn4_addr_from_u64(final_lba);
    } else {
        req->result_lba = req->lba;
    }

    atomic_thread_fence(memory_order_release);
    if (cb) cb(req, HN4_OK);
}

/* =========================================================================
 * 4. MEMORY MANAGEMENT
 * ========================================================================= */

/*
 * Aligned to 16 bytes to ensure strict padding behavior across compilers.
 */
typedef struct {
    uint32_t magic;
    uint32_t _pad32; /* Explicit 4-byte pad */
    void*    raw_ptr;
    uint64_t _pad64; /* Padding to reach 24 bytes? No, force 32 or alignment logic */
} __attribute__((aligned(16))) alloc_header_t;

void* hn4_hal_mem_alloc(size_t size)
{
    _assert_hal_init();
    if (size == 0) return NULL;

    /*
     * Pad for alignment + header space.
     * We allocate enough to slide the pointer to a 128-byte boundary.
     */
    size_t total = size + HN4_HAL_ALIGNMENT + sizeof(alloc_header_t);
    void*  raw   = malloc(total);
    if (!raw) return NULL;

    /* Calculate aligned address */
    uintptr_t raw_addr     = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + sizeof(alloc_header_t) + (HN4_HAL_ALIGNMENT - 1))
                             & ~((uintptr_t)HN4_HAL_ALIGNMENT - 1);

    void* ptr = (void*)aligned_addr;

    /* Store header immediately preceding the aligned pointer */
    alloc_header_t* h = (alloc_header_t*)((uint8_t*)ptr - sizeof(alloc_header_t));

    /* Verify our math didn't corrupt the heap or overlap */
    if ((void*)h < raw) hn4_hal_panic("Allocator Math Underflow");

    h->magic   = HN4_MEM_MAGIC;
    h->raw_ptr = raw;
    h->_pad32  = 0;

    /* Safety: Zero memory */
    memset(ptr, 0, size);
    return ptr;
}

void hn4_hal_mem_free(void* ptr)
{
    if (!ptr) return;

    /* Backtrack to header */
    alloc_header_t* h = (alloc_header_t*)((uint8_t*)ptr - sizeof(alloc_header_t));

    if (HN4_UNLIKELY(h->magic != HN4_MEM_MAGIC)) {
        hn4_hal_panic("HN4 Heap Corruption: Invalid Free");
    }

    h->magic = 0xDEADBEEF; /* Poison */
    free(h->raw_ptr);
}

/* =========================================================================
 * 5. SYNC IO & EXTENDED HELPERS
 * ========================================================================= */

typedef struct {
    volatile bool         done;
    volatile hn4_result_t res;
} sync_ctx_t;

static void _sync_cb(hn4_io_req_t* r, hn4_result_t res)
{
    sync_ctx_t* ctx = (sync_ctx_t*)r->user_ctx;
    ctx->res = res;
    atomic_thread_fence(memory_order_release);
    ctx->done = true;
}

hn4_result_t hn4_hal_sync_io(hn4_hal_device_t* dev, uint8_t op, hn4_addr_t lba, void* buf, uint32_t len)
{
    sync_ctx_t   ctx = { .done = false, .res = HN4_OK };
    hn4_io_req_t req = {0};

    req.op_code  = op;
    req.lba      = lba;
    req.buffer   = buf;
    req.length   = len;
    req.user_ctx = &ctx;

    hn4_hal_submit_io(dev, &req, _sync_cb);

    /* Acquire semantics enforced inside loop condition check */
    /* Note: Casting volatile bool* to _Atomic bool* for standard compliance */
    while (!atomic_load_explicit((_Atomic bool*)&ctx.done, memory_order_acquire)) {
        HN4_YIELD();
        hn4_hal_poll(dev);
    }

    /* Fence is now redundant due to load_acquire above, but harmless to keep */
    atomic_thread_fence(memory_order_acquire);
    return ctx.res;
}

hn4_result_t hn4_hal_barrier(hn4_hal_device_t* dev)
{
    return hn4_hal_sync_io(dev, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
}

/*
 * Prevents Deadlock/OOM by enforcing alignment and advancing pointers.
 */
hn4_result_t hn4_hal_sync_io_large(hn4_hal_device_t* dev,
                                   uint8_t           op,
                                   hn4_addr_t        start_lba,
                                   void*             buf,
                                   hn4_size_t        len_bytes,
                                   uint32_t          block_size)
{
    if (HN4_UNLIKELY(!dev || block_size == 0)) return HN4_ERR_INVALID_ARGUMENT;

    /*
     * SAFEGUARD #1: Alignment Check
     * If the total length is not a multiple of the block size, we cannot
     * subdivide it safely without a read-modify-write buffer.
     * Fail fast to prevent infinite loops at the tail.
     */
#ifdef HN4_USE_128BIT
    /* Assumption: block_size is power-of-2 or small enough that .lo check works for now */
    if ((len_bytes.lo % block_size) != 0) return HN4_ERR_ALIGNMENT_FAIL;
#else
    if ((len_bytes % block_size) != 0) return HN4_ERR_ALIGNMENT_FAIL;
#endif

    /* SAFEGUARD #2: Max Chunk (2GB) must be aligned to block_size */
    const uint64_t MAX_RAW_CAP = 0x80000000ULL;
    /* Round down to nearest block multiple */
    const uint64_t SAFE_CHUNK_CAP = (MAX_RAW_CAP / block_size) * block_size;

    hn4_size_t remaining   = len_bytes;
    hn4_addr_t current_lba = start_lba;
    uint8_t*   buf_cursor  = (uint8_t*)buf;

    while (1) {
        /* 1. Check if done */
#ifdef HN4_USE_128BIT
        if (remaining.hi == 0 && remaining.lo == 0) break;
#else
        if (remaining == 0) break;
#endif

        /* 2. Calculate Chunk Size (Bytes) */
        uint64_t chunk_bytes;

#ifdef HN4_USE_128BIT
        if (remaining.hi > 0 || remaining.lo > SAFE_CHUNK_CAP) {
            chunk_bytes = SAFE_CHUNK_CAP;
        } else {
            chunk_bytes = remaining.lo;
        }
#else
        chunk_bytes = (remaining > SAFE_CHUNK_CAP) ? SAFE_CHUNK_CAP : (uint64_t)remaining;
#endif

        /* 3. Convert to Blocks */
        uint32_t chunk_blocks = (uint32_t)(chunk_bytes / block_size);

        /*
         * SAFEGUARD #3: Zero-Block Trap
         * If we have remaining data but calculated 0 blocks to transfer,
         * we are in an infinite loop (Zeno's Paradox). ABORT.
         */
        if (chunk_blocks == 0) {
            HN4_LOG_CRIT("HAL Deadlock Detected: Remaining bytes < Block Size");
            return HN4_ERR_INTERNAL_FAULT;
        }

        /* 4. Execute IO */
        hn4_result_t res = hn4_hal_sync_io(dev, op, current_lba, (void*)buf_cursor, chunk_blocks);
        if (res != HN4_OK) return res;

        /* 5. Advance State */
        uint64_t bytes_transferred = (uint64_t)chunk_blocks * block_size;

        /* Advance Buffer */
        buf_cursor += bytes_transferred;

        /* Advance LBA */
        current_lba = hn4_addr_add(current_lba, chunk_blocks);

        /* Decrement Remaining */
#ifdef HN4_USE_128BIT
        remaining = hn4_u128_sub(remaining, hn4_u128_from_u64(bytes_transferred));
#else
        remaining -= bytes_transferred;
#endif

        /* Yield on large transfers to prevent watchdog timeouts */
        if (chunk_blocks > 1024) HN4_YIELD();
    }

    return HN4_OK;
}

/* =========================================================================
 * 6. TELEMETRY, LOCKS & CAPS
 * ========================================================================= */

hn4_time_t hn4_hal_get_time_ns(void)
{
    /*
     * This is strictly monotonic but has no correlation to wall-clock time.
     * Suitable for ordering checks, NOT for calendar time.
     */
    static _Atomic uint64_t ticks = 0;
    return (hn4_time_t)atomic_fetch_add(&ticks, 100);
}

uint64_t hn4_hal_get_random_u64(void)
{
    _assert_hal_init();
    uint64_t c = atomic_load_explicit(&_prng_seed, memory_order_relaxed);
    uint64_t n = c * 6364136223846793005ULL + 1;
    atomic_store_explicit(&_prng_seed, n, memory_order_relaxed);
    return n;
}

const hn4_hal_caps_t* hn4_hal_get_caps(hn4_hal_device_t* dev)
{
    _assert_hal_init();
    if (!dev) return NULL;
    return &dev->caps;
}

/* Stubs */
void     hn4_hal_poll(hn4_hal_device_t* d)            { (void)d; HN4_YIELD(); }
uint32_t hn4_hal_get_temperature(hn4_hal_device_t* d) { (void)d; return 40; }
void     hn4_hal_micro_sleep(uint32_t us)             { (void)us; HN4_YIELD(); }

/* Spinlock */
void hn4_hal_spinlock_init(hn4_spinlock_t* l)
{
    atomic_flag_clear(&l->flag);
}

void hn4_hal_spinlock_acquire(hn4_spinlock_t* l)
{
    while (atomic_flag_test_and_set_explicit(&l->flag, memory_order_acquire)) {
        HN4_YIELD();
    }
}

void hn4_hal_spinlock_release(hn4_spinlock_t* l)
{
    atomic_flag_clear_explicit(&l->flag, memory_order_release);
}

/* =========================================================================
 * 7. AI CONTEXT SIMULATION
 * ========================================================================= */

#define HN4_GPU_ID_NONE 0xFFFFFFFFU

/*
 * THREAD SAFETY CONTRACT:
 * 1. Affinity is strictly Thread-Local. Changing it in Thread A does NOT affect Thread B.
 * 2. If the compiler does not support TLS (_Thread_local), AI Affinity is DISABLED
 *    (Always returns HN4_GPU_ID_NONE) to prevent race conditions.
 * 3. Atomic stores are used to prevent compiler reordering/caching issues.
 */

#if defined(__STDC_NO_THREADS__) && !defined(_MSC_VER)
    /*
     * If we cannot guarantee thread isolation, we default to CPU mode.
     */
    #define HN4_NO_TLS_SUPPORT 1
#elif defined(_MSC_VER)
    #define HN4_TLS_ATTR __declspec(thread)
#else
    #define HN4_TLS_ATTR _Thread_local
#endif

#ifndef HN4_NO_TLS_SUPPORT
    /* Atomic ensures consistency against interrupts/signals */
    static HN4_TLS_ATTR _Atomic uint32_t _tl_gpu_context_id = HN4_GPU_ID_NONE;
#endif

/**
 * hn4_hal_sim_set_gpu_context (TEST/SIMULATION ONLY)
 *
 * Sets the affinity for the CURRENT thread.
 *
 * SAFETY:
 * - Returns immediately if TLS is unsupported.
 * - Warns if setting HN4_GPU_ID_NONE (Use clear instead).
 */
void hn4_hal_sim_set_gpu_context(uint32_t gpu_id)
{
#ifdef HN4_NO_TLS_SUPPORT
    (void)gpu_id;
    /* Silently ignore setting affinity on unsafe platforms */
    return;
#else
    /* Semantic validation */
    if (gpu_id == HN4_GPU_ID_NONE) {
        /* User should use clear, but we handle it safely */
        atomic_store_explicit(&_tl_gpu_context_id, HN4_GPU_ID_NONE, memory_order_relaxed);
        return;
    }

    atomic_store_explicit(&_tl_gpu_context_id, gpu_id, memory_order_release);
#endif
}

/**
 * hn4_hal_sim_clear_gpu_context
 *
 * Resets the thread context to CPU mode.
 * MUST be called before thread returns to a pool.
 */
void hn4_hal_sim_clear_gpu_context(void)
{
#ifndef HN4_NO_TLS_SUPPORT
    atomic_store_explicit(&_tl_gpu_context_id, HN4_GPU_ID_NONE, memory_order_release);
#endif
}

/**
 * hn4_hal_get_calling_gpu_id
 *
 * Retrieves the PCI ID or unique index of the Accelerator bound to this thread.
 * Used by the Allocator to calculate Affinity Bias (Path-Aware Striping).
 *
 * CONTRACT:
 * - Allocator MUST validate the returned ID against the loaded Topology Map.
 * - Receiving an ID does not guarantee the device exists or is online.
 *
 * @return HN4_GPU_ID_NONE (0xFFFFFFFF) if CPU context or TLS unavailable.
 * @return GPU_ID if the thread is an accelerator worker.
 */
uint32_t hn4_hal_get_calling_gpu_id(void)
{
#ifdef HN4_NO_TLS_SUPPORT
    return HN4_GPU_ID_NONE;
#else
    return atomic_load_explicit(&_tl_gpu_context_id, memory_order_acquire);
#endif

    /*
     * TODO! OUT OF SCOPE FOR HN4.
     *
     * In a real kernel, this logic is highly specific to the OS/Vendor stack.
     * Example (Linux/NVIDIA):
     *
     *   struct task_struct *t = current;
     *   // 1. Check if Kernel Thread (No GPU context possible)
     *   if (t->flags & PF_KTHREAD) return HN4_GPU_ID_NONE;
     *
     *   // 2. Query Vendor Driver (Hypothetical API)
     *   // This usually requires GPL symbols or proprietary shims.
     *   return nvidia_p2p_get_current_context_id();
     */
}

/* =========================================================================
 * 8. AI TOPOLOGY DISCOVERY (SIMULATION STUBS)
 * ========================================================================= */

uint32_t hn4_hal_get_topology_count(hn4_hal_device_t* dev)
{
    (void)dev;
    /*
     * PRODUCTION NOTE:
     * Queries ACPI SRAT (System Resource Affinity Table) or PCIe Root Complex.
     *
     * Returns 0 by default. Allocator logic handles 0 gracefully
     * by disabling AI optimization.
     */
    return 0;
}

hn4_result_t hn4_hal_get_topology_data(hn4_hal_device_t* dev, void* buffer, size_t buf_len)
{
    (void)dev;
    (void)buffer;
    (void)buf_len;

    /*
     * PRODUCTION NOTE:
     * Populates the buffer with {GPU_ID, Weight, LBA_Start, LBA_Len}.
     * Ensure buffer is cast to correct structure defined in volume header.
     */
    return HN4_OK;
}

void hn4_hal_prefetch(hn4_hal_device_t* dev, hn4_addr_t lba, uint32_t len) {
    if (!dev) return;

    /* 
     * IMPLEMENTATION NOTE: 
     * This is a "Best Effort" hint. Failure is ignored.
     */
#if defined(__linux__) && defined(POSIX_FADV_WILLNEED)
    /* User-space optimization: Hint the OS page cache */
    uint64_t offset = hn4_addr_to_u64(lba) * 512; /* Assuming 512B sector base */
    uint64_t bytes  = len * 512;
    posix_fadvise(dev->fd, (off_t)offset, (off_t)bytes, POSIX_FADV_WILLNEED);

#elif defined(_WIN32)
    /* Windows Prefetch logic (File mapping hint or empty) */
    /* No-op for standard file handles */

#else
    /* Embedded/Bare-metal: No-op unless specific controller registers exist */
    (void)lba;
    (void)len;
#endif
}