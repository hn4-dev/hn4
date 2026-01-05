/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Chronicle (Immutable Audit Log)
 * HEADER:      hn4_chronicle.h
 * STATUS:      HARDENED / PRODUCTION (v8.6)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * DESCRIPTION:
 * Defines the on-disk structure for the Audit Log, ensuring strict
 * monotonicity and anti-tamper linking via hash-chaining.
 */

#ifndef HN4_CHRONICLE_H
#define HN4_CHRONICLE_H

#include "hn4.h"
#include "hn4_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration from HAL */
typedef struct hn4_hal_device hn4_hal_device_t;

#define HN4_CHRONICLE_MAGIC     0x4C43494E4F524843ULL
#define HN4_CHRONICLE_TAIL_KEY  0xCAFEBABE12345678ULL 
#define HN4_CHRONICLE_VERSION   4

typedef struct HN4_PACKED {
    /* 0x00 */ uint64_t    magic;          /* "CHRONICL" */
    /* 0x08 */ uint64_t    sequence;       /* Monotonic ID */
    /* 0x10 */ uint64_t    timestamp;      /* UTC Nanoseconds */
    
    /* 0x18 */ hn4_addr_t  old_lba;        /* Data Context */
    /* 0x20 */ hn4_addr_t  new_lba;        /* Data Context */
    /* 0x28 */ hn4_addr_t  self_lba;       /* Anti-Replay Binding */
    
    /* 0x30 */ uint32_t    principal_hash32; /* Truncated Hint */
    /* 0x34 */ uint16_t    version;        
    /* 0x36 */ uint16_t    op_code;        
    
    /* 0x38 */ uint32_t    prev_sector_crc;/* Link to N-1 */
    
    /* 0x3C (Offset 60) */ 
    uint32_t    entry_header_crc;          /* Checksum 0x00-0x3B */
} hn4_chronicle_header_t;

/* Operations */
#define HN4_CHRONICLE_OP_INIT       0
#define HN4_CHRONICLE_OP_ROLLBACK   1
#define HN4_CHRONICLE_OP_SNAPSHOT   2
#define HN4_CHRONICLE_OP_WORMHOLE   3
#define HN4_CHRONICLE_OP_FORK       4

/**
 * hn4_chronicle_append
 * Atomically appends a new entry.
 * SAFETY: Enforces strict monotonicity. Fails if sequence wraps.
 */
hn4_result_t hn4_chronicle_append(
    hn4_hal_device_t* dev,
    hn4_volume_t* vol,
    uint32_t op_code,
    hn4_addr_t old_lba,
    hn4_addr_t new_lba,
    uint64_t principal_hash
);

/**
 * hn4_chronicle_verify_integrity
 * Validates the chain. Auto-heals "Phantom Heads" if valid.
 * 
 * SAFETY:
 * - If healing fails to persist, volume is forced to READ-ONLY.
 * - Stops scanning at first invalid block (End of History), does not false-flag.
 */
hn4_result_t hn4_chronicle_verify_integrity(
    hn4_hal_device_t* dev,
    hn4_volume_t* vol
);

#ifdef __cplusplus
}
#endif

#endif /* HN4_CHRONICLE_H */