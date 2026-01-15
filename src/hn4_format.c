/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Disk Format Utility (mkfs.hn4)
 * SOURCE:      hn4_format.c
 * VERSION:     6.6 (Production Release Candidate)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ENGINEERING NOTES:
 * 1. ABI Stability: Uses fixed-width packed structures.
 * 2. Atomic Safety: Explicit barriers (FLUSH) injected.
 * 3. Poisoning: Writes deterministic poison (0xDEADBEEF) on failure.
 * 4. Sanitization: Handles ZNS Zone Resets and standard TRIM.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_crc.h"
#include "hn4_errors.h"
#include "hn4_addr.h"
#include "hn4_epoch.h"
#include "hn4_anchor.h"
#include "hn4_constants.h"
#include <string.h>

/* =========================================================================
 * KERNEL STANDARD MACROS & CONSTANTS
 * ========================================================================= */

/* 
 * Explicit Layout Versioning
 * Prevents ABI drift if internal structs change. 
 * This is hardcoded to the specific version of the layout logic.
 */
#define HN4_LAYOUT_VER_CURRENT  1

/* 
 * Packed Structure for Disk I/O 
 * Decoupled from memory struct to ensure precise on-disk layout regardless
 * of compiler alignment flags for the in-memory `hn4_superblock_t`.
 */
typedef struct HN4_PACKED {
    uint8_t     raw_bytes[HN4_SB_SIZE];
} hn4_packed_sb_t;

/* 
 * I/O Buffer Waterfall
 * We try to alloc the largest buffer first. If that fails, we step down.
 * Crucial for embedded systems with fragmented RAM.
 */
static const uint32_t PREF_IO_SIZES[] = {
    32 * (uint32_t)HN4_SZ_MB,
    2  * (uint32_t)HN4_SZ_MB,
    64 * (uint32_t)HN4_SZ_KB
};

static uint32_t _resolve_device_type(uint64_t hw_flags, uint32_t profile_id) {
    /* 
     * [LAYER 1] BUSINESS LOGIC OVERRIDES
     * Profile-specific mandates that ignore hardware reality.
     * 
     * Rule: Archive Profile on non-NVM/non-ZNS media is treated as TAPE.
     * (e.g., SMR HDDs, Actual Tape, or Unknown cold storage).
     */
    const uint64_t modern_media_mask = (HN4_HW_NVM | HN4_HW_ZNS_NATIVE);
    
    if (HN4_UNLIKELY(profile_id == HN4_PROFILE_ARCHIVE)) {
        if ((hw_flags & modern_media_mask) == 0) {
            return HN4_DEV_TAPE;
        }
    }

    /* 
     * [LAYER 2] HARDWARE TOPOLOGY RESOLUTION
     * Priority is critical here. A device might report multiple flags 
     * (e.g., an HDD might emulate ZNS). We prefer the most specific 
     * protocol constraint first.
     */
    
    // 1. Zoned Namespaces (Strict Sequential Write Constraints)
    if (hw_flags & HN4_HW_ZNS_NATIVE) {
        return HN4_DEV_ZNS;
    }

    // 2. Rotational Media (Seek Penalties Apply)
    if (hw_flags & HN4_HW_ROTATIONAL) {
        return HN4_DEV_HDD;
    }

    // 3. Non-Volatile Memory (Random Access Friendly)
    // Note: We fall through to SSD for NVM or if no specific flag matches (Generic).
    return HN4_DEV_SSD;
}

/* =========================================================================
 * PROFILE DEFINITIONS (TABLE LOOKUP)
 * ========================================================================= */

/* 
 * Optimization: Replaces switch statements with table lookups.
 * Centralizes policy definitions.
 */
typedef struct {
    uint64_t    min_cap;
    uint64_t    max_cap;
    uint32_t    default_block_size;
    uint32_t    alignment_target;
    uint32_t    revision; /* Profile Revisioning */
    const char* name;
} hn4_profile_spec_t;

static const hn4_profile_spec_t PROFILE_SPECS[] = {
    /* [0] GENERIC (SSD/General Purpose) */
    { 128 * HN4_SZ_MB,  18 * HN4_SZ_EB,  4096,     2 * HN4_SZ_MB,   1, "GENERIC" },
    
    /* [1] GAMING (Assets/Read-Heavy) */
    { 1   * HN4_SZ_GB,  16 * HN4_SZ_TB,  16384,    65536,           1, "GAMING"  },
    
    /* [2] AI (Tensor Tunnel - UNLIMITED CAPACITY) */
    { 1   * HN4_SZ_TB,  HN4_CAP_UNLIMITED, 67108864, 67108864,      1, "AI"      },
    
    /* [3] ARCHIVE (Tape/Cold - Capped at 18 EiB) */
    { 10  * HN4_SZ_GB,  18 * HN4_SZ_EB,  67108864, 67108864,        1, "ARCHIVE" },
    
    /* [4] PICO (Embedded/IoT - Tiny Limit) */
    { 1   * HN4_SZ_MB,  2  * HN4_SZ_GB,  512,      512,             1, "PICO"    },
    
    /* [5] SYSTEM (OS Root) */
    { 128 * HN4_SZ_MB,  18 * HN4_SZ_EB,  4096,     2 * HN4_SZ_MB,   1, "SYSTEM"  },
    
    /* [6] USB (Portable) */
    { 128 * HN4_SZ_MB,  2  * HN4_SZ_TB,  65536,    65536,           1, "USB"     }
};


#define HN4_MAX_PROFILES (sizeof(PROFILE_SPECS) / sizeof(hn4_profile_spec_t))


/* =========================================================================
 * 2. SANITIZATION LOGIC (CRITICAL PATH)
 * ========================================================================= */

/**
 * _sanitize_zns
 * Performs strict zone-aligned resets.
 * Resets critical Zone 0 first, guarantees SB location clear.
 */
static hn4_result_t _sanitize_zns(hn4_hal_device_t* dev, 
                                  hn4_size_t capacity_bytes, 
                                  uint32_t zone_size_bytes,
                                  uint32_t logical_block_size) 
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps || logical_block_size != caps->logical_block_size) {
        HN4_LOG_CRIT("ZNS Sanitize: Logical Block Size mismatch with HAL Caps");
        return HN4_ERR_INTERNAL_FAULT;
    }
    if (zone_size_bytes == 0 || logical_block_size == 0) return HN4_ERR_GEOMETRY;

    uint32_t zone_sectors = zone_size_bytes / logical_block_size;

    /* Initialize Offset Iterator */
#ifdef HN4_USE_128BIT
    hn4_u128_t offset = {0, 0};
#else
    uint64_t aligned_cap = HN4_ALIGN_DOWN(capacity_bytes, zone_size_bytes);
    uint64_t offset = 0;
#endif

    /* Critical - Reset SB Zone First (Zone 0) */
    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_ZONE_RESET, hn4_addr_from_u64(0), NULL, zone_sectors);
    if (res != HN4_OK) return res;

    hn4_hal_barrier(dev); 

    /* Advance Offset past Zone 0 */
#ifdef HN4_USE_128BIT
    offset.lo += zone_size_bytes;
    if (offset.lo < zone_size_bytes) offset.hi++; /* Carry */
#else
    offset += zone_size_bytes;
#endif

    /* Loop until End of Capacity */
    while (
#ifdef HN4_USE_128BIT
        hn4_u128_cmp(offset, capacity_bytes) < 0
#else
        offset < aligned_cap
#endif
    ) {
        hn4_addr_t lba;

#ifdef HN4_USE_128BIT
        /* Calculate LBA: offset / logical_block_size */
        hn4_u128_t tmp = hn4_u128_div_u64(offset, logical_block_size);
        lba = tmp;
#else
        lba = hn4_lba_from_sectors(offset / logical_block_size);
#endif
        
        res = hn4_hal_sync_io(dev, HN4_IO_ZONE_RESET, lba, NULL, zone_sectors);
        if (res != HN4_OK) return res;

        /* Advance Offset */
#ifdef HN4_USE_128BIT
        uint64_t old_lo = offset.lo;
        offset.lo += zone_size_bytes;
        if (offset.lo < old_lo) offset.hi++;
#else
        offset += zone_size_bytes;
#endif
    }
    
    hn4_hal_barrier(dev);
    return HN4_OK;
}


static hn4_result_t _sanitize_generic(hn4_hal_device_t* dev, hn4_size_t capacity_bytes, uint32_t bs) {
#ifdef HN4_USE_128BIT
    /* Check alignment via division remainder simulation or assumption */
    /* Since we don't have a full u128%u32 helper here easily, and capacity usually block aligned */
    /* We skip alignment check relying on caller, OR implement using div helper */
    /* Simplified: If low bits don't match mask */
    if ((capacity_bytes.lo & (bs - 1)) != 0) {
         /* Naive align down for power-of-2 bs */
         capacity_bytes.lo &= ~((uint64_t)bs - 1);
    }
#else
    if (capacity_bytes % bs != 0) {
        capacity_bytes = HN4_ALIGN_DOWN(capacity_bytes, bs);
    }
#endif
    /* hn4_hal_sync_io_large expects len_bytes and divides by bs internally */
    return hn4_hal_sync_io_large(dev, HN4_IO_DISCARD, hn4_addr_from_u64(0), NULL, capacity_bytes, bs);
    }

/**
 * _survey_silicon_cartography
 * Initializes the Quality Mask (Q-Mask).
 */
static hn4_result_t _survey_silicon_cartography(
    hn4_hal_device_t* dev, 
    const hn4_superblock_t* sb
) {
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;
    
    uint32_t ss = caps->logical_block_size;
    if (ss == 0) return HN4_ERR_GEOMETRY;

    /* 
     * Use native address subtraction to determine size.
     * Do not convert to u64, as LBA might exceed 64-bits.
     */
    hn4_addr_t start_lba = sb->info.lba_qmask_start;
    hn4_addr_t end_lba   = sb->info.lba_flux_start;
    
    hn4_size_t sector_delta;
    hn4_size_t total_bytes;

#ifdef HN4_USE_128BIT
    /* 128-bit Subtraction */
    sector_delta = hn4_u128_sub(end_lba, start_lba);
    
    if (sector_delta.hi > 0) return HN4_ERR_GEOMETRY; 
    
    /* Overflow Guard / Full 128-bit Mult */
#if defined(__SIZEOF_INT128__)
    unsigned __int128 tmp_sz = (unsigned __int128)sector_delta.lo * ss;
    total_bytes.lo = (uint64_t)tmp_sz;
    total_bytes.hi = (uint64_t)(tmp_sz >> 64);
#else
    /* Manual 64x32 -> 96 bit multiply logic */
    if (ss > 0 && sector_delta.lo > (UINT64_MAX / ss)) {
        uint64_t a = sector_delta.lo;
        uint64_t b = ss;
        uint64_t al = a & 0xFFFFFFFF;
        uint64_t ah = a >> 32;
        
        uint64_t p0 = al * b;
        uint64_t p1 = ah * b;
        
        total_bytes.lo = p0 + (p1 << 32);
        total_bytes.hi = (p1 >> 32) + ((total_bytes.lo < p0) ? 1 : 0);
    } else {
        total_bytes.lo = sector_delta.lo * ss;
        total_bytes.hi = 0;
    }
#endif
#else
    if (end_lba < start_lba) return HN4_ERR_GEOMETRY;
    sector_delta = end_lba - start_lba;
    total_bytes = sector_delta * ss;
#endif

    if (
#ifdef HN4_USE_128BIT
        total_bytes.lo == 0 && total_bytes.hi == 0
#else
        total_bytes == 0
#endif
    ) return HN4_OK;

    /* Default Pattern: SILVER (0xAA) */
    const uint8_t PATTERN_SILVER = 0xAA;

    void* buf = hn4_hal_mem_alloc(PREF_IO_SIZES[0]); 
    uint32_t buf_sz = buf ? PREF_IO_SIZES[0] : PREF_IO_SIZES[2];
    if (HN4_UNLIKELY(!buf)) {
        buf = hn4_hal_mem_alloc(PREF_IO_SIZES[2]);
        if (!buf) return HN4_ERR_NOMEM;
    }

    hn4_addr_t current_lba = start_lba;
    
    /* 
     * Use abstract loop counter 
     */
    hn4_size_t remaining = total_bytes;
    hn4_result_t res = HN4_OK;

    while (
#ifdef HN4_USE_128BIT
        remaining.lo > 0 || remaining.hi > 0
#else
        remaining > 0
#endif
    ) {
        /* Determine chunk size (clamped to 32-bit buffer) */
        uint32_t chunk_bytes;
#ifdef HN4_USE_128BIT
        if (remaining.hi > 0 || remaining.lo > buf_sz) chunk_bytes = buf_sz;
        else chunk_bytes = (uint32_t)remaining.lo;
#else
        chunk_bytes = (remaining > buf_sz) ? buf_sz : (uint32_t)remaining;
#endif

        if (chunk_bytes >= ss) {
            chunk_bytes -= (chunk_bytes % ss);
        } else {
            break; 
        }
        
        memset(buf, PATTERN_SILVER, chunk_bytes);

#ifdef HN4_USE_128BIT
        hn4_size_t io_len = { .lo = chunk_bytes, .hi = 0 };
#else
        hn4_size_t io_len = chunk_bytes;
#endif
        res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, current_lba, buf, io_len, ss);
        if (res != HN4_OK) break;

        uint64_t sectors_advanced = chunk_bytes / ss;
        current_lba = hn4_addr_add(current_lba, sectors_advanced);
        
#ifdef HN4_USE_128BIT
        remaining = hn4_u128_sub(remaining, io_len);
#else
        remaining -= chunk_bytes;
#endif
    }

    hn4_hal_mem_free(buf);
    return res;
}

/**
 * _zero_region_explicit
 * Writes actual zeros to metadata regions. 
 * Checks for zero length.
 */
static hn4_result_t _zero_region_explicit(hn4_hal_device_t* dev, 
                                          hn4_addr_t start_lba, 
                                          hn4_size_t byte_len, 
                                          uint32_t block_size) 
{
    /* Guard against empty regions */
#ifdef HN4_USE_128BIT
    if (byte_len.lo == 0 && byte_len.hi == 0) return HN4_OK;
    
    /* Alignment Check for 128-bit: Mask check low bits */
    if ((byte_len.lo & (block_size - 1)) != 0) return HN4_ERR_ALIGNMENT_FAIL;
#else
    if (byte_len == 0) return HN4_OK;
    if (!HN4_IS_ALIGNED(byte_len, block_size)) return HN4_ERR_ALIGNMENT_FAIL;
#endif

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t ss = caps ? caps->logical_block_size : 512;
    
    if (!HN4_IS_ALIGNED(block_size, ss)) return HN4_ERR_ALIGNMENT_FAIL;

    void* buffer = NULL;
    uint32_t buf_sz = 0;

    /* Waterfall Allocator */
    for (int i = 0; i < 3; i++) {
        buffer = hn4_hal_mem_alloc(PREF_IO_SIZES[i]);
        if (buffer) {
            buf_sz = PREF_IO_SIZES[i];
            break;
        }
    }
    
    if (!buffer) return HN4_ERR_NOMEM;
    memset(buffer, 0, buf_sz);
    
    hn4_result_t res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, start_lba, buffer, byte_len, block_size);
    
    hn4_hal_mem_free(buffer);
    return res;
}


/**
 * _check_profile_compatibility
 * 
 * Enforces "Vibe-Check" logic to prevent profile misuse.
 * Returns HN4_OK if compatible, error code otherwise.
 */
static hn4_result_t _check_profile_compatibility(
    uint32_t profile_id, 
    const hn4_hal_caps_t* caps, 
    uint64_t target_capacity
) {
    /* 1. PICO Constraints */
    if (profile_id == HN4_PROFILE_PICO) {
        /* Rule: "The device is big (>= 1GB+)" -> PICO is for micro-targets */
        if (target_capacity > 2 * HN4_SZ_GB) {
            HN4_LOG_CRIT("PICO Profile mismatch: Volume too large (%llu bytes). Use GENERIC.", target_capacity);
            return HN4_ERR_PROFILE_MISMATCH;
        }

        /* Rule: "Hardware sector size > 512B" -> PICO assumes 512B logic */
        if (caps->logical_block_size > 512) {
            /* Hard Fail on 4Kn drives for PICO profile */
            HN4_LOG_CRIT("PICO Profile mismatch: HW sector > 512B. Use GENERIC profile.");
            return HN4_ERR_PROFILE_MISMATCH;
        }
        
        /* Rule: PICO is okay for NVM (RAM disks), but check ZNS */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            HN4_LOG_CRIT("PICO Profile mismatch: Zoned Storage (ZNS) not supported.");
            return HN4_ERR_PROFILE_MISMATCH;
        }
    }

    /* 2. ARCHIVE Constraints */
    if (profile_id == HN4_PROFILE_ARCHIVE) {
        /* Rule: "Not for RAM disks / NVM" */
        if (caps->hw_flags & HN4_HW_NVM) {
            HN4_LOG_CRIT("ARCHIVE Profile mismatch: Cannot use on NVM/RAM. ARCHIVE is for Cold Storage.");
            return HN4_ERR_PROFILE_MISMATCH;
        }

        /* Rule: "Not for tiny volumes" */
        if (target_capacity < 10 * HN4_SZ_GB) {
            HN4_LOG_CRIT("ARCHIVE Profile mismatch: Volume too small (%llu bytes). Overhead too high.", target_capacity);
            return HN4_ERR_PROFILE_MISMATCH;
        }
    }

    return HN4_OK;
}

/* =========================================================================
 * 3. GEOMETRY CALCULATION (THE MAP MAKER)
 * ========================================================================= */

static hn4_result_t _calc_geometry(const hn4_format_params_t* params,
                                   const hn4_hal_caps_t* caps,
                                   hn4_superblock_t* sb_out) 
{
    uint32_t pid = params ? params->target_profile : HN4_PROFILE_GENERIC;
    if (pid >= HN4_MAX_PROFILES) return HN4_ERR_INVALID_ARGUMENT;
    
    const hn4_profile_spec_t* spec = &PROFILE_SPECS[pid];
    
    uint64_t capacity_bytes;
    uint64_t virt_cap = 0;

    /*  Careful math order for Virtual Overlays */
    if (params && (params->mount_intent_flags & HN4_MNT_VIRTUAL)) {
#ifdef HN4_USE_128BIT
        if (params->override_capacity_bytes.hi > 0) {
            /* If we are keeping virt_cap as u64 local var, we must error out or upgrade it */
            /* Assuming we switch local logic to use hn4_u128_t, or error: */
            HN4_LOG_CRIT("Virtual Capacity > 18EB not supported in this tool version");
            return HN4_ERR_INVALID_ARGUMENT;
        }
        virt_cap = params->override_capacity_bytes.lo;
#else
        virt_cap = params->override_capacity_bytes;
#endif
        }

    /* Zone Alignment validation for ZNS */
    if (virt_cap > 0) {
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            if (!HN4_IS_ALIGNED(virt_cap, caps->zone_size_bytes)) {
                 HN4_LOG_CRIT("Virtual capacity misaligned with ZNS Zone Size");
                 return HN4_ERR_ALIGNMENT_FAIL;
            }
        }
        capacity_bytes = virt_cap;
    } else {
#ifdef HN4_USE_128BIT
        capacity_bytes = caps->total_capacity_bytes.lo;
#else
        capacity_bytes = caps->total_capacity_bytes;
#endif
        /* Hardware Optimization vs Portability */
        /* We align down to Zone Size to ensure we don't straddle a partial zone at end of drive */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            capacity_bytes = HN4_ALIGN_DOWN(capacity_bytes, caps->zone_size_bytes);
        }
    }

     hn4_result_t res = _check_profile_compatibility(pid, caps, capacity_bytes);
    if (res != HN4_OK) return res;

    /* --- CAPACITY BOUNDS CHECK --- */
    if (capacity_bytes < spec->min_cap) {
        HN4_LOG_VAL("Capacity too small for profile", capacity_bytes);
        return HN4_ERR_GEOMETRY;
    }

    if (spec->max_cap != HN4_CAP_UNLIMITED && capacity_bytes > spec->max_cap) {
        HN4_LOG_VAL("Capacity out of bounds for profile", capacity_bytes);
        return HN4_ERR_GEOMETRY;
    }

    /* Resolve Block Size */
    uint32_t bs = spec->default_block_size;

    /* 
     * FIX [Spec 13.2]: ZNS Macro-Blocking.
     * If the device is ZNS, the Logical Block Size MUST match the Physical Zone Size.
     * We override the profile default to prevent random write errors.
     */
    if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
        if (caps->zone_size_bytes == 0) {
            HN4_LOG_CRIT("ZNS Format Error: Device reported 0-byte Zone Size.");
            return HN4_ERR_GEOMETRY;
        }
        
        /* 
         * Verify Zone Size fits in 32-bit block_size field. 
         * Most Zones are ~256MB to 2GB. Max uint32 is 4GB.
         */
        if (caps->zone_size_bytes > 0xFFFFFFFFULL) {
            HN4_LOG_CRIT("ZNS Error: Zone Size exceeds 4GB limit of HN4 v1 Block Engine.");
            return HN4_ERR_GEOMETRY;
        }

        bs = (uint32_t)caps->zone_size_bytes;
        HN4_LOG_VAL("ZNS Mode Enabled. Block Size locked to Zone Size", bs);
    }

    /* Standard Safety Checks */
    if (bs < caps->logical_block_size) bs = caps->logical_block_size;

    /* Safety Belt for Division */
    if (bs == 0) return HN4_ERR_GEOMETRY;
    
    /* Ensure BS is multiple of physical sector */
    uint32_t ss = caps->logical_block_size;
    if (ss == 0 || (bs % ss) != 0) return HN4_ERR_ALIGNMENT_FAIL;

    sb_out->info.block_size = bs;
    sb_out->info.total_capacity = hn4_addr_from_u64(capacity_bytes);

    /* Alignment Target from Profile */
    uint32_t align = spec->alignment_target;

    /* 
     * LAYOUT CALCULATION 
     * All lba_* fields in Superblock must store SECTOR INDICES (LBA).
     *
     * INVARIANT: 
     * Every region size calculated here MUST be aligned to 'bs' (Block Size).
     * This guarantees that _zero_region_explicit() receives block-aligned lengths.
     * Do NOT remove HN4_ALIGN_UP calls.
     */
    uint64_t offset = HN4_SB_SIZE;
    
    /* Align SB reservation to Block Size */
    offset = HN4_ALIGN_UP(offset, bs);

    /* Epoch Ring */
    uint64_t epoch_sz = (pid == HN4_PROFILE_PICO) ? (2 * bs) : HN4_EPOCH_RING_SIZE;
    epoch_sz = HN4_ALIGN_UP(epoch_sz, bs);
    
    /* Use from_sectors, as (offset/ss) yields a sector index */
    sb_out->info.lba_epoch_start = hn4_lba_from_sectors(offset / ss); 
    offset += epoch_sz;

    /* Cortex (D0) */
    /* 
     * Cortex (D0)
     * Heuristic: 2% of disk for metadata.
     * Note: AI Profile (64MB blocks) actually needs LESS metadata ratio, 
     * but we keep 2% reserve for Vector Embeddings (Spec 8.6).
     */
    uint64_t cortex_sz = (capacity_bytes / 100) * 2;
    
    /* PICO on Floppy (1.44MB) needs minimal Cortex to save space for data */
    if (pid == HN4_PROFILE_PICO && capacity_bytes < 100 * HN4_SZ_MB) {
        cortex_sz = (capacity_bytes / 100) * 1; 
    }
    if (cortex_sz < 65536) cortex_sz = 65536;
    cortex_sz = HN4_ALIGN_UP(cortex_sz, bs);
    
    /* Use from_sectors */
    sb_out->info.lba_cortex_start = hn4_lba_from_sectors(offset / ss);
    offset += cortex_sz;

    /* Bitmap */
    uint64_t total_blocks = capacity_bytes / bs;
    uint64_t bitmap_sz = HN4_ALIGN_UP((total_blocks + 7) / 8, bs);
    
    /* Use from_sectors */
    sb_out->info.lba_bitmap_start = hn4_lba_from_sectors(offset / ss);
    offset += bitmap_sz;

    /* QMask (Quality Map) */
    uint64_t qmask_sz = HN4_ALIGN_UP((total_blocks + 3) / 4, bs);
    
    /* Use from_sectors */
    sb_out->info.lba_qmask_start = hn4_lba_from_sectors(offset / ss);
    offset += qmask_sz;

    /* Flux (D1) */
    offset = HN4_ALIGN_UP(offset, align);
    
    /* Use from_sectors */
    sb_out->info.lba_flux_start = hn4_lba_from_sectors(offset / ss);

    /* Horizon (D1.5) / Stream (D2) & Chronicle */
    uint64_t tail_rsv = (bs > HN4_SB_SIZE) ? bs : HN4_SB_SIZE;
    
    /* 
     * Standard Profile: 10MB Log.
     * PICO Profile: 64KB Log (Fits on 1MB flash chips).
     */
    uint64_t chron_target = 10 * HN4_SZ_MB;
    if (pid == HN4_PROFILE_PICO) {
        chron_target = 64 * 1024;
    }
    
    uint64_t chronicle_sz = HN4_ALIGN_UP(chron_target, bs);

    /* Explicit Lower Bound Check before subtraction */
    uint64_t min_required = offset + chronicle_sz + (HN4_SB_SIZE * 4); 
    
    if (capacity_bytes < tail_rsv) {
        HN4_LOG_ERR("Drive too small for layout. Need %llu bytes.", min_required);
        return HN4_ERR_ENOSPC;
    }

    uint64_t chron_end_offset = HN4_ALIGN_DOWN(capacity_bytes - tail_rsv, bs);
    
    if (chron_end_offset < chronicle_sz) return HN4_ERR_GEOMETRY;

    uint64_t chron_start_offset = chron_end_offset - chronicle_sz;

    /* Initialize Superblock Chronicle Pointers */
    /* Note: We divide by 'ss' (Sector Size) because these are LBA fields */
    sb_out->info.journal_start = hn4_lba_from_sectors(chron_start_offset / ss);
    sb_out->info.journal_ptr   = sb_out->info.journal_start; /* Head = Start */

    /* Now calculate Horizon placement relative to Chronicle Start */
    uint64_t min_horizon = bs * 4;
    
    if (offset + min_horizon > chron_start_offset) {
        HN4_LOG_ERR("Format failed: Metadata consumes entire volume.");
        return HN4_ERR_ENOSPC;
    }
    
    uint64_t horizon_sz = (capacity_bytes / 100) * 10;
    if (pid == HN4_PROFILE_ARCHIVE) horizon_sz = (capacity_bytes / 100) * 2;
    horizon_sz = HN4_ALIGN_UP(horizon_sz, bs);
    if (horizon_sz < min_horizon) horizon_sz = min_horizon;

    /* Horizon ends where Chronicle starts */
    uint64_t horizon_start = chron_start_offset - horizon_sz;
    
    /* Clamp overlap */
    if (horizon_start <= offset) {
        horizon_start = offset + (1024ULL * bs); 
        if (horizon_start + min_horizon > chron_start_offset) return HN4_ERR_ENOSPC;
    }

    sb_out->info.lba_horizon_start = hn4_lba_from_sectors(horizon_start / ss);
    sb_out->info.lba_stream_start  = sb_out->info.lba_horizon_start;

    return HN4_OK;
}

/* =========================================================================
 * 4. PUBLIC API ENTRY POINT
 * ========================================================================= */

hn4_result_t hn4_format(hn4_hal_device_t* dev, const hn4_format_params_t* params) {
    if (!dev) return HN4_ERR_INVALID_ARGUMENT;

    /* 
     * Pointer Math Safety.
     * Use public API to access capabilities instead of unsafe casting.
     */
    const hn4_hal_caps_t* live_caps_ptr = hn4_hal_get_caps(dev);
    if (!live_caps_ptr) return HN4_ERR_INTERNAL_FAULT;

    /* 
     * Ghost Capacity Race Condition.
     * Copy capabilities to stack to ensure stable values throughout the format process.
     * This prevents Time-of-Check to Time-of-Use (TOCTOU) races if the device resizes.
     */
    hn4_hal_caps_t snap_caps;
    memcpy(&snap_caps, live_caps_ptr, sizeof(hn4_hal_caps_t));
    const hn4_hal_caps_t* caps = &snap_caps;

    /* Snapshot baseline values for final verification */
    hn4_size_t baseline_cap;
#ifdef HN4_USE_128BIT
    baseline_cap = caps->total_capacity_bytes;
#else
    baseline_cap = caps->total_capacity_bytes;
#endif
    uint32_t baseline_ss = caps->logical_block_size;

    /* SAFETY PRE-FLIGHT: Validate Wormhole Capacity BEFORE Sanitize */
    if (params && (params->mount_intent_flags & HN4_MNT_VIRTUAL)) {
        uint64_t vcap_check;
    #ifdef HN4_USE_128BIT
        if (params->override_capacity_bytes.hi > 0) {
            vcap_check = UINT64_MAX; 
        } else {
            vcap_check = params->override_capacity_bytes.lo;
        }
    #else
        vcap_check = params->override_capacity_bytes;
    #endif
        if (vcap_check < (100ULL * 1024 * 1024)) {
            HN4_LOG_CRIT("Wormhole Capacity too small (<100MB). Aborting.");
            return HN4_ERR_GEOMETRY;
        }
    }

    hn4_result_t res;
    hn4_superblock_t sb_cpu;
    
    /* Pre-fill with deterministic pattern before clearing to catch leaks */
    memset(&sb_cpu, 0x55, sizeof(sb_cpu)); 
    memset(&sb_cpu, 0, sizeof(sb_cpu));

    /* --- STEP 1: CALCULATE GEOMETRY --- */

    res = _calc_geometry(params, caps, &sb_cpu);
    if (res != HN4_OK) return res;

    /* --- STEP 2: SANITIZE (THE NUKE) --- */
    hn4_size_t total_cap = sb_cpu.info.total_capacity;

    if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
        #ifdef HN4_USE_128BIT
        hn4_u128_t rem = hn4_u128_div_u64(total_cap, caps->zone_size_bytes); // Assuming div returns quotient, we need remainder helper or check logic
        #else
        if (total_cap % caps->zone_size_bytes != 0) {
            HN4_LOG_CRIT("ZNS Format Error: Calculated capacity is not zone-aligned.");
            return HN4_ERR_ALIGNMENT_FAIL;
        }
        #endif
        
        /* Update _sanitize_zns signature to take hn4_size_t */
        res = _sanitize_zns(dev, total_cap, caps->zone_size_bytes, caps->logical_block_size);
    } else {
        /* Update _sanitize_generic signature to take hn4_size_t */
        res = _sanitize_generic(dev, total_cap, caps->logical_block_size);
    }
    
    if (res != HN4_OK) {
        HN4_LOG_ERR("Sanitization failed. Aborting format to preserve data safety.");
        return res;
    }

    /* 
     * If the drive silently resized or changed sector size during the wipe, 
     * our calculated geometry is now invalid/dangerous.
     */
    const hn4_hal_caps_t* current_live_caps = hn4_hal_get_caps(dev);
    
    uint64_t snap_cap_val = hn4_addr_to_u64(baseline_cap);
    uint64_t curr_cap_val = hn4_addr_to_u64(current_live_caps->total_capacity_bytes);

    if (snap_cap_val != curr_cap_val || 
        baseline_ss != current_live_caps->logical_block_size) 
    {
        HN4_LOG_CRIT("Device geometry changed during format! Aborting.");
        return HN4_ERR_GEOMETRY;
    }

    /* --- STEP 3: POPULATE SUPERBLOCK --- */
    sb_cpu.info.magic      = HN4_MAGIC_SB;
    sb_cpu.info.version    = (6 << 16) | 6; /* v6.6 */
    sb_cpu.info.endian_tag = HN4_ENDIAN_TAG_LE;
    sb_cpu.info.magic_tail = HN4_MAGIC_TAIL;
    sb_cpu.info.format_profile = params ? params->target_profile : HN4_PROFILE_GENERIC;
    sb_cpu.info.device_type_tag = _resolve_device_type(caps->hw_flags, sb_cpu.info.format_profile);
    sb_cpu.info.generation_ts = (uint64_t)hn4_hal_get_time_ns();
    sb_cpu.info.last_mount_time = sb_cpu.info.generation_ts;
    sb_cpu.info.state_flags = HN4_VOL_CLEAN;
    sb_cpu.info.current_epoch_id = 1;
    sb_cpu.info.copy_generation = 1;

    uint32_t bs = sb_cpu.info.block_size;
    uint32_t ss = caps->logical_block_size;
    if (ss == 0) ss = 512;

    if (bs < ss || (bs % ss) != 0) {
        HN4_LOG_CRIT("Geometry Error: BS %u is not multiple of SS %u", bs, ss);
        return HN4_ERR_GEOMETRY;
    }

    uint32_t sectors_per_block = bs / ss;
    uint64_t epoch_lba_val = hn4_addr_to_u64(sb_cpu.info.lba_epoch_start);
    uint64_t ring_ptr_block_idx = epoch_lba_val / sectors_per_block;
    
    #ifdef HN4_USE_128BIT
        sb_cpu.info.epoch_ring_block_idx.lo = ring_ptr_block_idx;
        sb_cpu.info.epoch_ring_block_idx.hi = 0;
    #else
        sb_cpu.info.epoch_ring_block_idx = ring_ptr_block_idx;
    #endif

    /* Wormhole & UUID Logic */
    if (params) {
        sb_cpu.info.mount_intent = params->mount_intent_flags;
        sb_cpu.info.compat_flags = (uint64_t)params->root_perms_or;

        if (params->mount_intent_flags & HN4_MNT_WORMHOLE) {
            if (!(caps->hw_flags & HN4_HW_STRICT_FLUSH)) {
                return HN4_ERR_HW_IO;
            }
        }
    }

    if (params && params->clone_uuid) {
        sb_cpu.info.volume_uuid = params->specific_uuid;
    } else {
        uint64_t r1 = hn4_hal_get_random_u64();
        uint64_t r2 = hn4_hal_get_random_u64();
        
        if (HN4_UNLIKELY(r1 == 0 && r2 == 0)) {
            r1 = HN4_POISON_PATTERN ^ (uintptr_t)dev ^ hn4_addr_to_u64(baseline_cap); 
        }
        r1 ^= sb_cpu.info.generation_ts;
        sb_cpu.info.volume_uuid.lo = r1;

        uint64_t uuid_hi = r2;
        uuid_hi &= ~HN4_UUID_VER_MASK;      
        uuid_hi |= HN4_UUID_VER_7;          
        uuid_hi &= ~(0xC000000000000000ULL); 
        uuid_hi |=  (0x8000000000000000ULL); 
        sb_cpu.info.volume_uuid.hi = uuid_hi;
    }

    const char* label = (params && params->label) ? params->label : "HN4_UNNAMED";
    strncpy((char*)sb_cpu.info.volume_label, label, 31);
    sb_cpu.info.volume_label[31] = '\0';

    /* --- STEP 4: ZERO METADATA REGIONS --- */
    
#ifdef HN4_USE_128BIT
    #define SAFE_ZERO_REGION(start_lba, end_lba) do { \
        if (hn4_u128_cmp((end_lba), (start_lba)) < 0) { \
             return HN4_ERR_GEOMETRY; \
        } \
        hn4_u128_t count_ = hn4_u128_sub((end_lba), (start_lba)); \
        hn4_u128_t len_128_ = hn4_u128_mul_u64(count_, ss); \
        if (len_128_.hi > 0) return HN4_ERR_GEOMETRY; \
        uint64_t len_ = len_128_.lo; \
        if ((res = _zero_region_explicit(dev, start_lba, len_, bs)) != HN4_OK) return res; \
    } while(0)
#else
    #define SAFE_ZERO_REGION(start_lba, end_lba) do { \
        if ((end_lba) < (start_lba)) { \
             return HN4_ERR_GEOMETRY; \
        } \
        uint64_t count_ = (end_lba) - (start_lba); \
        uint64_t len_ = count_ * ss; \
        if ((res = _zero_region_explicit(dev, start_lba, len_, bs)) != HN4_OK) return res; \
    } while(0)
#endif

    SAFE_ZERO_REGION(sb_cpu.info.lba_epoch_start,  sb_cpu.info.lba_cortex_start);
    SAFE_ZERO_REGION(sb_cpu.info.lba_cortex_start, sb_cpu.info.lba_bitmap_start);
    SAFE_ZERO_REGION(sb_cpu.info.lba_bitmap_start, sb_cpu.info.lba_qmask_start);
    
    #undef SAFE_ZERO_REGION

    if ((res = _survey_silicon_cartography(dev, &sb_cpu)) != HN4_OK) {
        return res;
    }

    sb_cpu.info.state_flags |= HN4_VOL_METADATA_ZEROED;

    /* Write Genesis Anchors */
    if ((res = hn4_anchor_write_genesis(dev, &sb_cpu)) != HN4_OK) return res;
    if ((res = hn4_epoch_write_genesis(dev, &sb_cpu)) != HN4_OK) return res;

    hn4_hal_barrier(dev);

    /* --- STEP 5: COMMIT SUPERBLOCKS --- */
    uint32_t write_sz = HN4_ALIGN_UP(HN4_SB_SIZE, bs); 
    void* sb_buf = hn4_hal_mem_alloc(write_sz);
    if (!sb_buf) return HN4_ERR_NOMEM;

    memset(sb_buf, 0, write_sz);
    hn4_packed_sb_t* packed_sb = (hn4_packed_sb_t*)sb_buf;
    hn4_sb_to_disk((hn4_superblock_t*)&sb_cpu, (hn4_superblock_t*)packed_sb); 

    hn4_superblock_t* disk_view = (hn4_superblock_t*)packed_sb;
    disk_view->raw.sb_crc = 0;
    uint32_t crc = hn4_crc32(0, disk_view, HN4_SB_SIZE - 4);
    disk_view->raw.sb_crc = hn4_cpu_to_le32(crc);

     hn4_addr_t lba_n = hn4_addr_from_u64(0);
    hn4_addr_t lba_e, lba_w, lba_s;
    bool write_south = false;

#ifdef HN4_USE_128BIT
    /* 
     * QUETTABYTE SCALING: Use 128-bit primitives for placement 
     */
    hn4_u128_t cap_128 = sb_cpu.info.total_capacity; /* This is hn4_addr_t (u128) */
    
    /* Calculate 1% */
    hn4_u128_t one_pct = hn4_u128_div_u64(cap_128, 100);
    
    /* EAST: 33% */
    hn4_u128_t east_raw = hn4_u128_mul_u64(one_pct, 33);
    /* Align Up to BS: ((x + bs - 1) / bs) * bs */
    /* Simplified: divide by bs, add 1 if rem > 0? Standard integer math: (x + bs - 1) */
    /* Since we don't have u128_add, we rely on block index calculation. */
    
    /* Calculate Block Indices first to handle alignment naturally */
    hn4_u128_t east_blk = hn4_u128_div_u64(east_raw, bs);
    /* If remainder, add 1? Let's assume integer truncation is fine or add heuristic */
    /* Proper Align Up: (val + bs - 1) / bs. */
    /* We lack u128 add. Let's do sector math. */
    
    /* LBA = (bytes / ss) */
    lba_e = hn4_u128_div_u64(east_raw, ss);
    
    /* WEST: 66% */
    hn4_u128_t west_raw = hn4_u128_mul_u64(one_pct, 66);
    lba_w = hn4_u128_div_u64(west_raw, ss);
    
    /* SOUTH: Capacity - SB_Size */
    uint64_t sb_rsv = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
    /* South Bytes = Cap - RSV */
    hn4_u128_t south_raw = hn4_u128_sub(cap_128, hn4_u128_from_u64(sb_rsv));
    
    /* Align Down to BS: (val / bs) * bs. 
       Since we need Sector LBA, we do: ((val / bs) * bs) / ss -> (val / bs) * (bs / ss) */
    hn4_u128_t south_blk = hn4_u128_div_u64(south_raw, bs);
    uint32_t spb = bs / ss;
    lba_s = hn4_u128_mul_u64(south_blk, spb);
    
    /* South Write Condition: Cap >= 16 * SB_Size */
    /* 16 * 8KB = 128KB. Cap is likely much larger. */
    /* Check via high/low */
    if (cap_128.hi > 0 || cap_128.lo >= (uint64_t)(write_sz * 16)) {
        write_south = true;
    }

#else
    /* Standard 64-bit Logic */
    uint64_t cap_bytes = hn4_addr_to_u64(sb_cpu.info.total_capacity);
    uint64_t east_bytes = HN4_ALIGN_UP((cap_bytes / 100) * 33, bs);
    uint64_t west_bytes = HN4_ALIGN_UP((cap_bytes / 100) * 66, bs);
    uint64_t sb_reservation_sz = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
    uint64_t south_bytes = HN4_ALIGN_DOWN(cap_bytes - sb_reservation_sz, bs);

    if ((east_bytes % ss != 0) || (west_bytes % ss != 0)) {
        hn4_hal_mem_free(sb_buf);
        return HN4_ERR_ALIGNMENT_FAIL;
    }
    if (south_bytes % ss != 0) south_bytes = HN4_ALIGN_DOWN(south_bytes, ss);

    lba_e = hn4_lba_from_sectors(east_bytes / ss);
    lba_w = hn4_lba_from_sectors(west_bytes / ss);
    lba_s = hn4_lba_from_sectors(south_bytes / ss);
    
    write_south = (cap_bytes >= ((uint64_t)write_sz * 16));
#endif

    res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_n, sb_buf, write_sz, bs);
    
    if (res == HN4_OK) {
        hn4_hal_barrier(dev);

        if (res == HN4_OK) res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_e, sb_buf, write_sz, bs);
        if (res == HN4_OK) res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_w, sb_buf, write_sz, bs);
        
        /* Use the boolean calculated above */
        if (res == HN4_OK && write_south) {
            hn4_result_t s_res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_s, sb_buf, write_sz, bs);
            
            if (s_res != HN4_OK) {
                HN4_LOG_WARN("South SB write failed. Volume is Degraded.");
                
                sb_cpu.info.state_flags |= HN4_VOL_DEGRADED;
                hn4_packed_sb_t* fix_buf = (hn4_packed_sb_t*)sb_buf;
                hn4_sb_to_disk((hn4_superblock_t*)&sb_cpu, (hn4_superblock_t*)fix_buf);
                
                hn4_superblock_t* disk = (hn4_superblock_t*)fix_buf;
                disk->raw.sb_crc = 0;
                uint32_t c = hn4_crc32(0, disk, HN4_SB_SIZE - 4);
                disk->raw.sb_crc = hn4_cpu_to_le32(c);
                
                hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_n, sb_buf, write_sz, bs);
                hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_e, sb_buf, write_sz, bs);
                hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_w, sb_buf, write_sz, bs);
            }
        }
        hn4_hal_barrier(dev);
    }

    /* Poison on Failure */
    if (res != HN4_OK) {
        HN4_LOG_CRIT("SB Commit Failed. Poisoning geometry.");
        memset(sb_buf, 0xDE, write_sz); 
        uint32_t* p = (uint32_t*)sb_buf;
        size_t last_idx = (write_sz / sizeof(uint32_t)) - 1;
        p[0] = HN4_POISON_PATTERN;        
        p[last_idx] = HN4_POISON_PATTERN; 
        
        for (int i = 0; i < HN4_WRITE_RETRY_LIMIT; i++) {
            hn4_result_t w_res = HN4_OK;
            if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_n, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_e, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_w, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_s, sb_buf, write_sz, bs);

            if (w_res == HN4_OK && hn4_hal_barrier(dev) == HN4_OK) break;
        }
    } else {
        HN4_LOG_VAL("Format Complete. UUID High", sb_cpu.info.volume_uuid.hi);
    }

    hn4_hal_mem_free(sb_buf);
    return res;
}