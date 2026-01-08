/*
 * HYDRA-NEXUS 4 (HN4) IMPLEMENTATION STANDARD
 * HEADER FILE: hn4.h
 * STATUS:      REFERENCE STANDARD (v4.2)
 * TARGET:      KERNEL / BARE METAL HAL / EMBEDDED
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ARCHITECTURAL PARADIGM: THE POST-POSIX ERA
 * Defines on-disk structures, constants, and memory layouts for the
 * HN4 "Ballistic-Tensor Manifold". Enforces 1-byte packing.
 */

#ifndef _HN4_H_
#define _HN4_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 0. COMPILER ABSTRACTION & SAFETY
 * ========================================================================= */

/* Check for 128-bit atomic support (Hardware CAS) for the Armored Bitmap */
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16) || defined(__x86_64__) || defined(__aarch64__)
    #define HN4_HW_ATOMICS_128 1
#else
    #define HN4_HW_ATOMICS_128 0
#endif

/* 
 * PACKING STRATEGY
 * Hybrid approach for cross-compiler ABI compatibility:
 * - GCC/Clang: __attribute__((packed)) on structs.
 * - MSVC: #pragma pack(push, 1) globally for the scope of this header.
 */
#if defined(__GNUC__) || defined(__clang__)
    #define HN4_HOT             __attribute__((hot))
    #define HN4_PACKED          __attribute__((packed))
    #define HN4_ALIGNED(x)      __attribute__((aligned(x)))
    #define HN4_INLINE          static inline __attribute__((always_inline))
    #define HN4_LIKELY(x)       __builtin_expect(!!(x), 1)
    #define HN4_UNLIKELY(x)     __builtin_expect(!!(x), 0)
    #define HN4_PREFETCH(x)     __builtin_prefetch(x)
    #define HN4_BARRIER()       __atomic_thread_fence(__ATOMIC_ACQUIRE)
    #define HN4_RESTRICT        __restrict__
#elif defined(_MSC_VER)
    #define HN4_HOT
    /* MSVC uses pragma pack, so the attribute macro is empty */
    #define HN4_PACKED
    #define HN4_ALIGNED(x)      __declspec(align(x))
    #define HN4_INLINE          static __forceinline
    #define HN4_LIKELY(x)       (x)
    #define HN4_UNLIKELY(x)     (x)
    #define HN4_PREFETCH(x)     ((void)0)
    #define HN4_BARRIER()       atomic_thread_fence(memory_order_acquire)
    #define HN4_RESTRICT
    /* CRITICAL: Enforce strict 1-byte packing for MSVC */
    #pragma pack(push, 1)
#else
    #define HN4_HOT
    #define HN4_PACKED
    #define HN4_ALIGNED(x)
    #define HN4_INLINE          static inline
    #define HN4_LIKELY(x)       (x)
    #define HN4_UNLIKELY(x)     (x)
    #define HN4_PREFETCH(x)     ((void)0)
    #define HN4_BARRIER()       atomic_thread_fence(memory_order_acquire)
    #define HN4_RESTRICT 
    #warning "HN4: Unknown compiler. Structure packing is not guaranteed."
#endif

/* =========================================================================
 * 0.1 TELEMETRY & LOGGING SUB-SYSTEM
 * ========================================================================= */

#ifndef HN4_LOG_ENABLED
    #define HN4_LOG_ENABLED 0
#endif

#ifndef HN4_LOG_PRINTF
    #include <stdio.h>
    #define HN4_LOG_PRINTF printf
#endif

#ifndef HN4_LOG_THROTTLE_MASK
    #define HN4_LOG_THROTTLE_MASK 0xFFu
#endif

#if HN4_LOG_ENABLED
    #ifdef HN4_LOG_THROTTLE_LOCAL
        static unsigned int _hn4_log_throttle = 0u;
    #else
        extern unsigned int hn4_log_throttle;
    #endif

    #define HN4_LOG_FMT(fmt, ...)   HN4_LOG_PRINTF(fmt, ##__VA_ARGS__)

    #define HN4_LOG_CRIT(fmt, ...)  HN4_LOG_PRINTF("[HYDRA-CRT] " fmt "\n", ##__VA_ARGS__)
    #define HN4_LOG_WARN(fmt, ...)  HN4_LOG_PRINTF("[HYDRA-WRN] " fmt "\n", ##__VA_ARGS__)
    #define HN4_LOG_ERR(fmt, ...)   HN4_LOG_PRINTF("[HYDRA-ERR] " fmt "\n", ##__VA_ARGS__)
    #define HN4_LOG_VAL(name, val)  HN4_LOG_PRINTF("[HYDRA-VAL] %-25s: %llu\n", (name), (unsigned long long)(val))
#else
    #define HN4_LOG_FMT(fmt, ...)   ((void)0)
    #define HN4_LOG_CRIT(fmt, ...)  ((void)0)
    #define HN4_LOG_WARN(fmt, ...)  ((void)0)
    #define HN4_LOG_ERR(fmt, ...)   ((void)0)
    #define HN4_LOG_VAL(name, val)  ((void)0)
#endif

/* =========================================================================
 * 1. UNIVERSAL CONSTANTS & PRIMITIVE TYPES
 * ========================================================================= */

#define HN4_VERSION_CURRENT     1

/* The Arrow of Time (Nanoseconds since 1970-01-01 UTC) */
typedef int64_t hn4_time_t;

/* Strict 128-bit Identity Structure */
typedef struct HN4_PACKED {
    uint64_t lo; /* Random Entropy (Bits 64-127) */
    uint64_t hi; /* Time + Version (Bits 0-63) */
} hn4_u128_t;

/* The Quettabyte Horizon */
#ifdef HN4_USE_128BIT
    typedef hn4_u128_t hn4_addr_t;    /* 128-bit Physical LBA */
    typedef hn4_u128_t hn4_size_t;    /* 128-bit Size width */
    #define HN4_ADDR_WIDTH 16
    #define HN4_CAPACITY_MAX_LIMIT "3.4e38" /* Quettabytes */
#else
    typedef uint64_t hn4_addr_t;      /* 64-bit Physical LBA (Default) */
    typedef uint64_t hn4_size_t;      /* 64-bit Size width */
    #define HN4_ADDR_WIDTH 8
    #define HN4_CAPACITY_MAX_LIMIT "1.8e19" /* Exabytes */
#endif

typedef uint32_t hn4_crc_t;     /* CRC32C (Castagnoli) */
typedef uint8_t  hn4_byte_t;

/* --- Magic Numbers & Tags --- */
#define HN4_MAGIC_SB            0x48594452415F4E34ULL   /* "HYDRA_N4" */
#define HN4_MAGIC_TAIL          0xEFBEADDEULL
#define HN4_MAGIC_STREAM        0x5354524D              /* "STRM" */
#define HN4_MAGIC_REDIR         0x52444952              /* "RDIR" */
#define HN4_MAGIC_META          0x4D455441              /* "META" */
#define HN4_BLOCK_MAGIC         0x424C4B30
#define HN4_ENDIAN_TAG_LE       0x11223344u
#define HN4_CPU_CHECK_CONST     0xCBF43926

#define HN4_WRITE_RETRY_LIMIT   3

/* --- UUID v7 Constants --- */
#define HN4_UUID_VER_MASK       0xF000
#define HN4_UUID_VER_7          0x7000
#define HN4_NULL_ID_INIT        {0, 0}
#define HN4_ROOT_ID_INIT        {0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF}

/* --- Geometry Constants --- */
#define HN4_SB_LOC_NORTH_LBA    0ULL
#define HN4_SB_LOC_EAST_PCT     33ULL
#define HN4_SB_LOC_WEST_PCT     66ULL
#define HN4_SB_SIZE             8192

#define HN4_EPOCH_RING_START    8192
#define HN4_EPOCH_RING_SIZE     (1024 * 1024) /* 1 MB */
#define HN4_EPOCH_INTERVAL_MS   5000

/* =========================================================================
 * 2. STATE VECTORS & FLAGS
 * ========================================================================= */

/* Wormhole & Mount Intent Flags */
#define HN4_MNT_DEFAULT         0
#define HN4_MNT_WORMHOLE        (1ULL << 0) /* Identity Entanglement / Overlay */
#define HN4_MNT_READ_ONLY       (1ULL << 1)
#define HN4_MNT_VIRTUAL         (1ULL << 2) /* Container is a file, not a device */

/* Quality Tiers */
#define HN4_Q_TOXIC             0x00 /* 00 - DEAD/UNSAFE */
#define HN4_Q_BRONZE            0x01 /* 01 - Noisy/Slow (Temp/Media) */
#define HN4_Q_SILVER            0x02 /* 10 - Standard (User Data) */
#define HN4_Q_GOLD              0x03 /* 11 - Critical (Kernel/Meta) */

/* Extended Hardware Flag */
#define HN4_HW_FILE_BACKED      (1ULL << 63) /* HAL indicates target is a disk image file */

/* Volume State Flags (sb.state_flags) */
#define HN4_VOL_CLEAN           (1 << 0)
#define HN4_VOL_DIRTY           (1 << 1)
#define HN4_VOL_PANIC           (1 << 2)
#define HN4_VOL_DEGRADED        (1 << 3)
#define HN4_VOL_LOCKED          (1 << 4)
#define HN4_VOL_TOXIC           (1 << 5)
#define HN4_VOL_UNMOUNTING      (1 << 6) 
#define HN4_VOL_METADATA_ZEROED (1 << 13)
#define HN4_VOL_NEEDS_UPGRADE   (1 << 14)
#define HN4_VOL_PENDING_WIPE    (1 << 15)
#define HN4_VOL_RUNTIME_SATURATED (1U << 30)

/* Anchor Flags (anchor.data_class bits 8-63 overlay/expansion) */
#define HN4_FLAG_VALID          (1ULL << 8)
#define HN4_FLAG_TOMBSTONE      (1ULL << 9)
#define HN4_FLAG_ROT            (1ULL << 10)
#define HN4_FLAG_DUBIOUS        (1ULL << 11)
#define HN4_FLAG_PINNED         (1ULL << 12)
#define HN4_FLAG_TTL            (1ULL << 13)
#define HN4_FLAG_SHRfED          (1ULL << 14)
#define HN4_FLAG_SEQUENTIAL     (1ULL << 15) /* Force V=1 */
#define HN4_FLAG_VECTOR         (1ULL << 16) /* Vector Embedding present */
#define HN4_HINT_HORIZON        (1ULL << 17) /* Redirect D1->D1.5 */
#define HN4_HINT_STREAM         (1ULL << 18) /* D2 Intent */
#define HN4_HINT_COMPRESSED     (1ULL << 19)
#define HN4_HINT_ENCRYPTED      (1ULL << 20)
#define HN4_HINT_BOOT           (1ULL << 21) /* Force allocation in Hot Zone (0-1GB) */
#define HN4_FLAG_NANO           (1ULL << 22) /* Data resides in Cortex Slots */

/* Add to On-Disk Structures */
#define HN4_MAGIC_NANO          0x4E414E4F   /* "NANO" - Magic for data slots */

/* Format Profiles (sb.format_profile) */
#define HN4_PROFILE_GENERIC     0
#define HN4_PROFILE_GAMING      1
#define HN4_PROFILE_AI          2
#define HN4_PROFILE_ARCHIVE     3
#define HN4_PROFILE_PICO        4
#define HN4_PROFILE_SYSTEM      5
#define HN4_PROFILE_USB         6  

#define HN4_MAX_REPLICAS        4
#define HN4_QUORUM_THRESHOLD    3

/* Device Types (sb.device_type_tag) */
#define HN4_DEV_SSD             0
#define HN4_DEV_HDD             1
#define HN4_DEV_ZNS             2
#define HN4_DEV_TAPE            3

/* Hardware Capability Flags (sb.hw_caps_flags) */
#define HN4_HW_ZNS_NATIVE       (1 << 0)
#define HN4_HW_GPU_DIRECT       (1 << 1)
#define HN4_HW_ROTATIONAL       (1 << 2)
#define HN4_HW_NVM              (1 << 14) /* 0x4000: Byte-Addressable Non-Volatile Memory */

/* =========================================================================
 * 3. ON-DISK STRUCTURES (L1 - PHYSICAL LAYOUT)
 * ========================================================================= */

/* 2.2 Superblock Structure */
typedef union HN4_PACKED {
    struct HN4_PACKED {
        /* --- IDENTITY (32 Bytes) --- */
        uint64_t    magic;              /* HN4_MAGIC_SB */
        uint32_t    version;            /* v4.2 (0x00040200) */
        uint32_t    block_size;         /* Elastic (512 - 65MB) */
        hn4_u128_t  volume_uuid;        /* Unique Identity */

        /* --- GEOMETRY (THE MAP) --- */
        hn4_addr_t  lba_epoch_start; 
        hn4_addr_t  total_capacity;
        hn4_addr_t  lba_cortex_start;   /* D0: Anchor Table Start */
        hn4_addr_t  lba_bitmap_start;
        hn4_addr_t  lba_flux_start;     /* D1: Ballistic Data Start */
        hn4_addr_t  lba_horizon_start;  /* D1.5: Linear Overflow Start */
        hn4_addr_t  lba_stream_start;   /* D2: Sequential Stream Start */
        hn4_addr_t  lba_qmask_start;    /* Start of 2-bit Quality Map */

        /* --- RECOVERY (THE TIME) --- */
        uint64_t    current_epoch_id;
        hn4_addr_t  epoch_ring_block_idx;
        uint64_t    copy_generation;

        /* --- HELIX STATE --- */
        hn4_addr_t  sentinel_cursor;
        uint64_t    hw_caps_flags;      /* ZNS / GPU_DIRECT / ETC */
        uint32_t    state_flags;        /* CLEAN / DIRTY / PANIC */

        /* --- FEATURE COMPATIBILITY --- */
        uint64_t    compat_flags;
        uint64_t    incompat_flags;
        uint64_t    ro_compat_flags;
        uint64_t    mount_intent;
        uint64_t    dirty_bits;         /* Coarse dirty bitmap */
        hn4_time_t  last_mount_time;
        hn4_addr_t  journal_ptr;        /* Reserved for external Journal */
        hn4_addr_t  journal_start;      // [NEW] Points to the absolute start of the log region
        uint32_t    endian_tag;         /* HN4_ENDIAN_TAG_LE */
        uint8_t     volume_label[32];   /* UTF-8 Label */
        uint32_t    format_profile;     /* Gaming / AI / Archive */
        uint32_t    device_type_tag;    /* SSD / HDD / ZNS */
        uint64_t    generation_ts;      /* Timestamp of creation */
        uint64_t    magic_tail;         /* 0xEFBEADDE */
        hn4_addr_t  boot_map_ptr;       /* Pointer to Static Boot Map File */
        uint64_t    last_journal_seq;   /* High-water mark of log sequence */
        /* Pad remaining bytes is implicit in the Union */
    } info;

    /* Guarantee 8KB Size with CRC at the very end */
    struct HN4_PACKED {
        uint8_t     _pad[HN4_SB_SIZE - 4];
        hn4_crc_t   sb_crc;             /* Checksum of this 8KB block */
    } raw;
} hn4_superblock_t;

/* 2.3 The Epoch Ring Header (128 Bytes) */
typedef struct HN4_PACKED {
    uint64_t    epoch_id;
    hn4_time_t  timestamp;
    hn4_crc_t   d0_root_checksum;   /* Root Checksum of Anchor Table */
    uint32_t    flags;
    uint8_t     reserved[100];
    hn4_crc_t   epoch_crc; 
} hn4_epoch_header_t;

/* =========================================================================
 * 4. ON-DISK STRUCTURES (L2 - OBJECTS & METADATA)
 * ========================================================================= */

/* Data Class Constants */
#define HN4_CLASS_TYPE_MASK     0x0FULL
#define HN4_TYPE_UNSTRUCT       0x00ULL
#define HN4_TYPE_MATRIX         0x01ULL /* AI/Scientific */
#define HN4_TYPE_LUDIC          0x02ULL /* Game Assets */

#define HN4_CLASS_VOL_MASK      0xF0ULL
#define HN4_VOL_STATIC          (1ULL << 4)
#define HN4_VOL_EPHEMERAL       (1ULL << 5)
#define HN4_VOL_ATOMIC          (1ULL << 6)

/* Capability Mask */
#define HN4_PERM_READ           (1 << 0)
#define HN4_PERM_WRITE          (1 << 1)
#define HN4_PERM_EXEC           (1 << 2)
#define HN4_PERM_APPEND         (1 << 3)
#define HN4_PERM_IMMUTABLE      (1 << 4)
#define HN4_PERM_SOVEREIGN      (1 << 5)
#define HN4_PERM_ENCRYPTED      (1 << 6)

/* 8.1 The Anchor Layout (128 Bytes) - Aligned to 2x CPU Cache Lines */
typedef struct HN4_PACKED {
    /* 0x00 */
    hn4_u128_t  seed_id;            /* Immutable Physics ID */
    /* 0x10 */
    hn4_u128_t  public_id;          /* Mutable Logical UUID */
    /* 0x20 */
    uint64_t    gravity_center;     /* G: LBA Pointer */
    /* 0x28 */
    uint64_t    mass;               /* Logical Size */
    /* 0x30 */
    uint64_t    data_class;         /* Hints (0-3), Volatility (4-7), Flags (8-63) */
    /* 0x38 */
    uint64_t    tag_filter;         /* Bloom Filter */
    /* 0x40 - 0x47 */
    uint8_t     orbit_vector[6];    /* V: u48 Stride (Stored as byte array) */
    uint16_t    fractal_scale;      /* M: Fractal Power (0..16) */
    /* 0x48 */
    uint32_t    permissions;        /* Capability Bitmask (Section 9.2) */
    uint32_t    sovereign_id;       /* Creator Key Hash */
    /* 0x50 */
    uint64_t    mod_clock;          /* Modified Time (ns) */
    /* 0x58 */
    uint32_t    write_gen;          /* Shadow Hop Counter */
    uint32_t    create_clock;       /* Created Time (Seconds) */
    /* 0x60 */
    uint32_t    checksum;           /* Root CRC32C */
    /* 0x64 */
    uint8_t     inline_buffer[28];  /* Filename or Tiny Data (Section 8.5) */
} hn4_anchor_t;

/* 9.3 Tether Structure - Access Control */
#define HN4_TARGET_FILE_ID      0
#define HN4_TARGET_TAG_HASH     1

typedef struct HN4_PACKED {
    uint32_t    target_type;        /* FILE_ID (0) or TAG_HASH (1) */
    uint32_t    permissions;        /* Granted permissions */
    uint64_t    expiry_ts;          /* 0 = Forever */
    hn4_u128_t  target_value;       /* UUID or Tag Hash */
    uint8_t     signature[64];      /* Ed25519 Signature */
    uint8_t     padding[32];        /* Pad to 128 bytes */
} hn4_tether_t;

/* 8.6 Extension Blocks */
#define HN4_EXT_TYPE_VECTOR     1   /* 384-bit Semantic Vector */
#define HN4_EXT_TYPE_LONGNAME   2   /* Filename > 23 bytes */
#define HN4_EXT_TYPE_TAG        3   /* Tag Metadata Entry */

typedef struct HN4_PACKED {
    uint32_t    magic;              /* HN4_MAGIC_META */
    uint32_t    type;               /* HN4_EXT_TYPE_* */
    uint64_t    next_ext_lba;       /* Chain pointer */
    uint8_t     payload[];          /* Type-specific data */
} hn4_extension_header_t;

/* =========================================================================
 * 5. ON-DISK STRUCTURES (L3 - DATA LAYER)
 * ========================================================================= */

/* Compression Meta Macros */
#define HN4_COMP_ALGO_MASK      0x0F
#define HN4_COMP_NONE           0
#define HN4_COMP_LZ4            1
#define HN4_COMP_ZSTD           2
#define HN4_COMP_SIZE_SHIFT     4   /* Bits 4-31 are size */

/* 3.2 The Flux Manifold (D1 Block Structure) */
typedef struct HN4_PACKED {
    /* 0x00 */
    hn4_u128_t  well_id;            /* Backlink: Must match Anchor.seed_id */
    /* 0x10 */
    uint64_t    seq_index;          /* Logical offset */
    /* 0x18 */
    uint64_t    generation;         /* Snapshot/Write Gen (Phantom Defense) */
    /* 0x20 */
    uint32_t    magic;              /* HN4_BLOCK_MAGIC (0x424C4B30) */
    /* 0x24 */
    uint32_t    data_crc;           /* CRC32C of Payload */
    /* 0x28 */
    uint32_t    comp_meta;          /* Compression Control */
    /* 0x2C */
    uint32_t    header_crc;         /* CRC32C of Header itself (0x00-0x2C) */
    /* 0x30 */
    uint8_t     payload[];          /* Variable size */
} hn4_block_header_t;

/* 3.5 The Hyper-Stream Header (D2 / Horizon) */
#define HN4_STREAM_SKIP_INTERVAL    1024

typedef struct HN4_PACKED {
    /* 0x00 */
    uint32_t    magic;              /* HN4_MAGIC_STREAM or HN4_MAGIC_REDIR */
    /* 0x04 */
    uint32_t    crc;                /* CRC32C of Header + Payload */
    /* 0x08 */
    uint64_t    length;             /* Bytes of valid payload */
    /* 0x10 */
    uint64_t    next_strm;          /* LBA of Block N+1 */
    /* 0x18 */
    uint64_t    hyper_strm;         /* LBA of Block N+1024 (Skip List) */
    /* 0x20 */
    uint64_t    seq_id;             /* Logical Index */
    /* 0x28 */
    uint8_t     reserved[24];       /* Padding */
    /* 0x40 */
    uint8_t     payload[];          /* Data starts at offset 64 */
} hn4_stream_header_t;

/* =========================================================================
 * 6. LOGS & RECOVERY
 * ========================================================================= */

/* 24.5 The Chronicle Entry */
#define HN4_CHRONICLE_OP_ROLLBACK   1
#define HN4_CHRONICLE_OP_SNAPSHOT   2
#define HN4_CHRONICLE_OP_WORMHOLE   3
#define HN4_CHRONICLE_OP_FORK       4

/* 
 * Aligned to ensure it never splits across a 512-byte sector boundary.
 */
typedef struct HN4_PACKED {
    uint64_t    magic;          // 0x4348524F4E49434C "CHRONICL"
    uint64_t    timestamp;      // UTC Nanoseconds
    uint32_t    op_code;        // HN4_CHRONICLE_OP_*
    uint32_t    reserved;
    hn4_addr_t  old_lba;        // Previous state pointer
    hn4_addr_t  new_lba;        // New state pointer
    uint64_t    user_key_hash;  // Who did it
    uint64_t    prev_entry_hash;// Blockchain link (CRC32C of previous entry)
    uint8_t     padding[8];     // Pad to 64 bytes (assuming 64-bit addrs)
} hn4_chronicle_entry_t;

/* 21.4 Triage Log Entry */
#define HN4_TRIAGE_ERR_ROT          1
#define HN4_TRIAGE_ERR_WRITE        2
#define HN4_TRIAGE_ERR_SYNC         3
#define HN4_TRIAGE_ACT_HEAL         1
#define HN4_TRIAGE_ACT_RELOCATE     2
#define HN4_TRIAGE_ACT_PANIC        3

typedef struct HN4_PACKED {
    hn4_time_t  timestamp;
    hn4_addr_t  lba;
    uint32_t    error_type;
    uint32_t    action_taken;
} hn4_triage_log_entry_t;



_Static_assert(sizeof(hn4_chronicle_entry_t) == 64, "Chronicle Entry must be 64 bytes");

/* =========================================================================
 * 7. RUNTIME & ALLOCATION STRUCTURES (RAM ONLY)
 * ========================================================================= */

/* Allocation Intents (Void Engine) */
#define HN4_ALLOC_DEFAULT       0   /* Standard Ballistic (D1) */
#define HN4_ALLOC_METADATA      1   /* Near D0 (Cortex) - Low Latency */
#define HN4_ALLOC_LUDIC         2   /* Outer Rim / SLC Cache */
#define HN4_ALLOC_ARCHIVE       3   /* Force D2 (Stream) - High Density */
#define HN4_ALLOC_TENSOR        4   /* 2MB Aligned - GPU Direct Compatible */
#define HN4_ALLOC_CONTIGUOUS    5   /* Force V=1 (HDD/Tape Mode) */

typedef struct {
    uint8_t     intent;         /* HN4_ALLOC_* */
    uint8_t     orbit_vector;   /* Desired V */
    uint16_t    fractal_scale;  /* Desired M */
    uint32_t    retry_limit;    /* Max probes before Horizon Fallback */
    hn4_size_t  size_hint;      /* Expected Mass */
} hn4_void_request_t;

/* The Armored Bitmap Word (Section 5.3) - 16-byte aligned for 128-bit atomic CAS */
typedef struct HN4_PACKED HN4_ALIGNED(16) {
    uint64_t    data;       /* 64 Bits of Map */
    uint8_t     ecc;        /* 8 Bits Hamming Code */
    uint8_t     reserved;   /* Alignment */
    uint16_t    ver_lo;     /* Version Counter Low */
    uint32_t    ver_hi;     /* Version Counter High (48-bit Total) */
} hn4_armored_word_t;

/* Ghost Hints */
#define HN4_GHOST_DISABLE_PREFETCH  (1 << 0)
#define HN4_GHOST_FORCE_STREAM      (1 << 1)

#if defined(__STDC_NO_ATOMICS__)
    #error "HN4 requires C11 Atomics support. Please enable C11 mode or link -latomic."
#else
    #include <stdatomic.h>
#endif

typedef struct {
    atomic_flag flag;
    uint32_t    pad; 
} hn4_spinlock_t;

/* The Synapse Handle (Open File Context) */
typedef struct {
    hn4_anchor_t    cached_anchor;
    hn4_u128_t      session_token;
    uint64_t        current_offset;
    uint32_t        taint_counter;
    uint32_t        temperature;    /* Hot/Cold Tiering */
    uint32_t        ghost_hints;    /* Runtime Overrides (No Disk Sync) */
    void*           npu_tunnel_ctx; /* GPU Direct Context */
} hn4_handle_t;

/* Runtime Volume Handle */
typedef struct {
    /* Opaque HAL target device */
    void*               target_device;

    /* Cached Geometry */
    uint64_t            vol_capacity_bytes;
    uint32_t            vol_block_size;

    /* Superblock State & Offsets */
    hn4_superblock_t    sb;
    uint64_t            sb_offsets_bytes[4]; 

    /* Memory Structures */
    hn4_armored_word_t* void_bitmap;
    size_t              bitmap_size;
    uint64_t*           quality_mask;   /* 64-bit words containing 32 blocks each */
    size_t              qmask_size;
    
    /* D0 Cortex Cache (Optional/Profile dependent) */
    void*               nano_cortex;
    size_t              cortex_size; 
    
    /* Time & State */
    int64_t             time_offset;
    bool                read_only;

    /* Atomic Counters */
    _Atomic uint64_t    used_blocks; 
    _Atomic uint64_t    horizon_write_head;
    _Atomic uint32_t    taint_counter; /* Session error tracker */
    _Atomic uint64_t    toxic_blocks;  /* Tracks blocks lost to physical rot */
    _Atomic uint64_t    last_alloc_g;  /* Tracks last successful Gravity Center for locality */
 
    uint64_t            cortex_search_head;  /* Cursor for Nano-Allocator */
    /* Optimizations */
    uint64_t*           l2_summary_bitmap; 
    bool                in_eviction_path; 
    hn4_spinlock_t      l2_lock;       /* Protects L2 bitmap for System volumes */

    /* AI Topology Map (Path-Aware Striping) */
    struct {
        uint32_t gpu_id;         /* PCI ID of the GPU */
        uint32_t affinity_weight;/* 0=SameSwitch, 1=SameNuma, 2=Remote */
        uint64_t lba_start;      /* Start of NVMe Namespace physically close */
        uint64_t lba_len;        /* Length of that Namespace */
    } *topo_map;                 /* Array of mapped accelerators */
    uint32_t topo_count;

    /* STEP 2: Add Telemetry & Rate Limiting */
    struct {
        _Atomic uint64_t heal_count;
        _Atomic uint64_t crc_failures;
        _Atomic uint64_t barrier_failures;
        _Atomic uint32_t last_panic_code;
    } stats;

    int64_t last_log_ts; /* For rate limiting */

} hn4_volume_t;

/* =========================================================================
 * 8. API, FORMATTING & BALLISTICS
 * ========================================================================= */

/* Ballistic Engine Types */
#define HN4_MAX_TRAJECTORY_K    12
#define HN4_HORIZON_FALLBACK_K  15

#define HN4_GRAVITY_SHIFT_ROT   17
#define HN4_GRAVITY_SHIFT_XOR   0xA5A5A5

#define HN4_SCALE_4KB           0
#define HN4_SCALE_64KB          4
#define HN4_SCALE_4MB           10
#define HN4_SCALE_64MB          14
#define HN4_SCALE_256MB         16

#define HN4_ORBIT_V_RAIL        1   /* HDD / ZNS / Sequential */
#define HN4_ORBIT_V_CLUSTER     4   /* Game Assets */
#define HN4_ORBIT_V_WEAVE_A     17  /* NVMe Standard */
#define HN4_ORBIT_V_WEAVE_B     19  /* NVMe Alternate */

typedef struct {
    hn4_addr_t  primary_lba;    /* k=0 */
    hn4_addr_t  orbit_lbas[4];  /* k=1..4 (Collision Candidates) */
    uint8_t     orbit_k;        /* Selected Orbit */
    bool        is_horizon;     /* Fallback to D1.5? */
} hn4_trajectory_t;

/* RAID Modes */
#define HN4_RAID_MODE_MIRROR    1
#define HN4_RAID_MODE_SHARD     2
#define HN4_RAID_MODE_PARITY    5

/* VFS Stat */
typedef struct {
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
    uint64_t st_blocks;
    uint32_t st_blksize;
} hn4_vfs_stat_t;

/* Mount Parameters */
typedef struct {
    uint64_t mount_flags;    /* e.g. HN4_MNT_READ_ONLY */
    uint32_t integrity_level; /* Checksum verification strictness */
    uint32_t reserved;
} hn4_mount_params_t;

/* Format Parameters */
#define HN4_HW_STRICT_FLUSH (1ULL << 62)

typedef struct {
    const char* label;
    uint32_t    target_profile;
    
    /* Wormhole: Identity Cloning */
    bool        clone_uuid;         
    hn4_u128_t  specific_uuid;      
    
    /* Wormhole: Mount Intent */
    uint64_t    mount_intent_flags; 
    
    /* Wormhole: Genesis Perms */
    uint32_t    root_perms_or;      

    /* Wormhole: Spatial Overlay (Virtual Geometry) */
    hn4_size_t  override_capacity_bytes;
} hn4_format_params_t;


/* =========================================================================
 * FIXED NANO STRUCTURES & CONSTANTS
 * ========================================================================= */
#define HN4_CORTEX_SLOT_SIZE    128
#define HN4_MAGIC_NANO          0x4E414E4F   /* "NANO" */
#define HN4_MAGIC_NANO_PENDING  0x504E4447   /* "PNDG" */
#define HN4_NANO_FLAG_COMMITTED (1U << 0)

typedef struct HN4_PACKED {
    uint32_t magic;         /* 0x00 */
    uint32_t header_crc;    /* 0x04 */
    uint64_t payload_len;   /* 0x08 */
    uint64_t version;       /* 0x10 (Fix 9) */
    uint32_t data_crc;      /* 0x18 */
    uint32_t flags;         /* 0x1C */
    uint8_t  data[];        /* 0x20 */
} hn4_nano_header_t;

/* =========================================================================
 * 9. STATIC ASSERTIONS (BARE METAL SAFETY)
 * ========================================================================= */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(sizeof(hn4_anchor_t) == 128, "HN4 Anchor must be 128 bytes");
    _Static_assert(sizeof(hn4_superblock_t) == HN4_SB_SIZE, "HN4 Superblock must be 8KB");
    _Static_assert(sizeof(hn4_epoch_header_t) == 128, "HN4 Epoch Header must be 128 bytes");
    _Static_assert(sizeof(hn4_tether_t) == 128, "HN4 Tether must be 128 bytes");
    _Static_assert(sizeof(hn4_armored_word_t) == 16, "HN4 Armored Word must be 16 bytes for 128-bit atomic CAS");
    
    /* Use offsetof() for structs with flexible array members to ensure layout correctness */
    _Static_assert(offsetof(hn4_block_header_t, payload) == 48, "HN4 Block Header Payload offset wrong");
    _Static_assert(offsetof(hn4_stream_header_t, payload) == 64, "HN4 Stream Header Payload offset wrong");
#endif

#ifdef __cplusplus
}
#endif

/* 
 * CRITICAL: MSVC PACKING RESTORATION
 * This must be unconditional regarding the HN4_PACKED macro.
 * It is tied solely to the _MSC_VER condition used at the top of the file.
 */
#if defined(_MSC_VER)
    #pragma pack(pop)
#endif

#endif /* _HN4_H_ */