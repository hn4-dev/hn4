/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * HEADER:      hn4_constants.h
 * PURPOSE:     Shared constants/macros NOT defined in hn4.h (Source of Truth).
 *              Eliminates duplication between format/mount/unmount modules.
 */

#ifndef _HN4_CONSTANTS_H_
#define _HN4_CONSTANTS_H_

#include "hn4.h" /* Access to types for inline helpers */
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. MATHEMATICAL MACROS (Duplicated in format/mount/unmount)
 * ========================================================================= */

#ifndef HN4_ALIGN_UP
#define HN4_ALIGN_UP(x, a)      (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#endif

#ifndef HN4_ALIGN_DOWN
#define HN4_ALIGN_DOWN(x, a)    ((x) & ~((uint64_t)(a) - 1))
#endif

#ifndef HN4_IS_ALIGNED
#define HN4_IS_ALIGNED(x, a)    (((x) & ((uint64_t)(a) - 1)) == 0)
#endif

#ifndef HN4_MAX
#define HN4_MAX(a, b)           ((a) > (b) ? (a) : (b))
#endif

#ifndef HN4_MIN
#define HN4_MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif

/* =========================================================================
 * 2. CAPACITY UNITS (Used in format/mount)
 * ========================================================================= */

#define HN4_SZ_KB   (1024ULL)
#define HN4_SZ_MB   (1024ULL * 1024ULL)
#define HN4_SZ_GB   (1024ULL * 1024ULL * 1024ULL)
#define HN4_SZ_TB   (1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define HN4_SZ_PB   (1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define HN4_SZ_EB   (1024ULL * HN4_SZ_PB)

#define HN4_CAP_UNLIMITED   UINT64_MAX

/* =========================================================================
 * 3. SHARED FLAGS & MAGIC (Not in hn4.h)
 * ========================================================================= */

/* Poison Pattern (Unified from 0xDEADBEEF in format/mount) */
#define HN4_POISON_PATTERN      0xDEADBEEF

/* Taint Bit (Unified from mount/unmount) */
#define HN4_DIRTY_BIT_TAINT     (1ULL << 63)

/* Compatibility Flags (Unified from format/unmount) */
#define HN4_COMPAT_SOUTH_SB     (1ULL << 0)

/* Sentinel Offsets */
#define HN4_OFFSET_INVALID      0xFFFFFFFFFFFFFFFFULL

/* =========================================================================
 * 4. LOGIC THRESHOLDS
 * ========================================================================= */

/* Max Generation count before volume locks (Safety buffer) */
#define HN4_MAX_GENERATION      (0xFFFFFFFFFFFFFFFFULL - 16)

/* Replay Attack Window: 60 Seconds (in Nanoseconds) */
#define HN4_REPLAY_WINDOW_NS    (60ULL * 1000000000ULL)

/* Taint Threshold for forced Read-Only */
#define HN4_TAINT_THRESHOLD_RO  20

/* =========================================================================
 * 5. SHARED INLINE HELPERS
 * ========================================================================= */

/**
 * _secure_zero
 * Secure Zero with Memory Fence. Prevents compiler DCE.
 * (Duplicated in epoch/mount/unmount)
 */
static inline void _secure_zero(void* ptr, size_t size) {
    if (ptr && size > 0) {
        volatile uint8_t* p = (volatile uint8_t*)ptr;
        while (size--) {
            *p++ = 0;
        }
        atomic_thread_fence(memory_order_seq_cst);
    }
}

/**
 * _addr_to_u64_checked
 * Safe Downcast Helper (128-bit -> 64-bit safe check)
 * (Duplicated in epoch/mount/unmount)
 */
static inline bool _addr_to_u64_checked(hn4_addr_t addr, uint64_t* out) {
#ifdef HN4_USE_128BIT
    if (addr.hi > 0) return false;
    *out = addr.lo;
#else
    *out = addr;
#endif
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* _HN4_CONSTANTS_H_ */