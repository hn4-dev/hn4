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
#define HN4_QMASK_BITS_PER_BLOCK 2
#define HN4_QMASK_BLOCKS_PER_BYTE (8 / HN4_QMASK_BITS_PER_BLOCK)

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
        return HN4_DEV_TAPE;
    }
   
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
    { 128 * HN4_SZ_MB,  2  * HN4_SZ_TB,  65536,    65536,           1, "USB"     },
    
    /* [7] HYPER_CLOUD (Server) 
     * - 64KB Blocks: Optimal for checksum overhead vs throughput.
     * - 1MB Align: RAID/Cloud stripe friendly.
     * - Unlimited Cap: Ready for Quettabytes.
     */
    { 100 * HN4_SZ_GB,  HN4_CAP_UNLIMITED, 65536,  1 * HN4_SZ_MB,   1, "HYPER_CLOUD" }
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
   /* Calculate Aligned Capacity for 128-bit logic */
#ifdef HN4_USE_128BIT
    hn4_u128_t aligned_cap_128;
    {
        hn4_u128_t zones = hn4_u128_div_u64(capacity_bytes, zone_size_bytes);
        aligned_cap_128 = hn4_u128_mul_u64(zones, zone_size_bytes);
    }
#endif

    /* Loop until End of Aligned Capacity */
    while (
#ifdef HN4_USE_128BIT
        hn4_u128_cmp(offset, aligned_cap_128) < 0
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
    hn4_u128_t q = hn4_u128_div_u64(capacity_bytes, bs);
    hn4_u128_t aligned = hn4_u128_mul_u64(q, bs);
    
    /* If reconstruction doesn't match original, it was misaligned. Use aligned value. */
    if (aligned.lo != capacity_bytes.lo || aligned.hi != capacity_bytes.hi) {
        capacity_bytes = aligned;
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
        uint64_t p1 = ah * b; // Represents (High * b) << 32
        
        /* 
         * p1 contributes to both Lo and Hi parts of the 128-bit result.
         * The lower 32 bits of p1 overlap with the upper 32 bits of p0.
         */
        uint64_t p1_lo = (p1 & 0xFFFFFFFF) << 32;
        uint64_t p1_hi = (p1 >> 32);

        total_bytes.lo = p0 + p1_lo;
        /* Handle carry from Lo addition + High part of p1 */
        total_bytes.hi = p1_hi + ((total_bytes.lo < p0) ? 1 : 0);
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

    void* buf = NULL;
    uint32_t buf_sz = 0;

    /* Waterfall Allocation Logic */
    for (int i = 0; i < 3; i++) {
        buf = hn4_hal_mem_alloc(PREF_IO_SIZES[i]);
        if (buf) {
            buf_sz = PREF_IO_SIZES[i];
            break;
        }
    }
    
    if (!buf) return HN4_ERR_NOMEM;

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
    
    /* Alignment Check */
    hn4_u128_t q = hn4_u128_div_u64(byte_len, block_size);
    hn4_u128_t recon = hn4_u128_mul_u64(q, block_size);
    
    if (byte_len.lo != recon.lo || byte_len.hi != recon.hi) {
        return HN4_ERR_ALIGNMENT_FAIL;
    }
#else
    if (byte_len == 0) return HN4_OK;
    if (!HN4_IS_ALIGNED(byte_len, block_size)) return HN4_ERR_ALIGNMENT_FAIL;
#endif

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t ss = caps ? caps->logical_block_size : 512;
    
    if (!HN4_IS_ALIGNED(block_size, ss)) return HN4_ERR_ALIGNMENT_FAIL;

    hn4_size_t phys_cap = caps->total_capacity_bytes;
    hn4_addr_t phys_limit_lba;

#ifdef HN4_USE_128BIT
    phys_limit_lba = hn4_u128_div_u64(phys_cap, ss);
    
    /* If start is already beyond physical, we are in virtual space. No-op. */
    if (hn4_u128_cmp(start_lba, phys_limit_lba) >= 0) return HN4_OK;
#else
    phys_limit_lba = phys_cap / ss;
    if (start_lba >= phys_limit_lba) return HN4_OK;
#endif

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
    
    hn4_size_t remaining = byte_len;
    hn4_addr_t current_lba = start_lba;
    
    hn4_result_t res = HN4_OK;

    while (
#ifdef HN4_USE_128BIT
        remaining.lo > 0 || remaining.hi > 0
#else
        remaining > 0
#endif
    ) {
        uint32_t chunk;
#ifdef HN4_USE_128BIT
        /* Clamp to buffer size */
        if (remaining.hi > 0 || remaining.lo > buf_sz) chunk = buf_sz;
        else chunk = (uint32_t)remaining.lo;
#else
        chunk = (remaining > buf_sz) ? buf_sz : (uint32_t)remaining;
#endif
        
        /* IO must be block aligned */
        uint32_t io_bytes = (chunk / ss) * ss;
        if (io_bytes == 0) break;

        uint32_t io_sectors = io_bytes / ss;
        hn4_addr_t end_lba = hn4_addr_add(current_lba, io_sectors);

#ifdef HN4_USE_128BIT
        if (hn4_u128_cmp(end_lba, phys_limit_lba) > 0) {
            /* 
             * Partial write at the edge. 
             * Calculate sectors remaining until cliff. 
             */
            hn4_u128_t diff = hn4_u128_sub(phys_limit_lba, current_lba);
            /* If we are exactly at edge or past, stop. */
            if (diff.hi == 0 && diff.lo == 0) break;
            
            /* If remaining space is smaller than intended IO, shrink IO */
            if (diff.hi == 0 && diff.lo < io_sectors) {
                io_sectors = (uint32_t)diff.lo;
                io_bytes = io_sectors * ss;
            } else {
                /* Should not happen if logic above is correct, but break to be safe */
                break;
            }
        }
#else
        if (end_lba > phys_limit_lba) {
            uint64_t diff = phys_limit_lba - current_lba;
            if (diff == 0) break;
            if (diff < io_sectors) {
                io_sectors = (uint32_t)diff;
                io_bytes = io_sectors * ss;
            }
        }
#endif
        /* If we clamped to 0, we are done */
        if (io_bytes == 0) break;

        res = hn4_hal_sync_io(dev, HN4_IO_WRITE, current_lba, buffer, io_sectors);
        if (res != HN4_OK) break;

        current_lba = hn4_addr_add(current_lba, io_sectors);

#ifdef HN4_USE_128BIT
        remaining = hn4_u128_sub(remaining, hn4_u128_from_u64(io_bytes));
#else
        remaining -= io_bytes;
#endif
    }
    
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
       if (HN4_UNLIKELY(target_capacity > 2 * HN4_SZ_GB)) {
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
    
    hn4_size_t capacity_bytes;
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

   /* 
     * Determine if Virtual Capacity is requested.
     * Logic adapts to 64-bit scalar vs 128-bit struct.
     */
    bool use_virtual = false;
#ifdef HN4_USE_128BIT
    if (virt_cap.lo != 0 || virt_cap.hi != 0) use_virtual = true;
#else
    if (virt_cap > 0) use_virtual = true;
#endif

    if (use_virtual) {
        /* --- VIRTUAL PATH --- */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
#ifdef HN4_USE_128BIT
            /* 128-bit Alignment Check: (Size % Zone) must be 0 */
            hn4_u128_t zone_sz_128 = hn4_u128_from_u64(caps->zone_size_bytes);
            hn4_u128_t rem = hn4_u128_mod(virt_cap, zone_sz_128);
            
            if (rem.lo != 0 || rem.hi != 0) {
                 HN4_LOG_CRIT("Virtual capacity misaligned with ZNS Zone Size");
                 return HN4_ERR_ALIGNMENT_FAIL;
            }
#else
            /* 64-bit Alignment Check */
            if (!HN4_IS_ALIGNED(virt_cap, caps->zone_size_bytes)) {
                 HN4_LOG_CRIT("Virtual capacity misaligned with ZNS Zone Size");
                 return HN4_ERR_ALIGNMENT_FAIL;
            }
#endif
        }
        capacity_bytes = virt_cap;
    } 
    else {
        /* --- HARDWARE DEFAULT PATH --- */
#ifdef HN4_USE_128BIT
        capacity_bytes = caps->total_capacity_bytes; 
    
        /* If ZNS, Align Down: Cap - (Cap % Zone) */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            hn4_u128_t zone_sz = hn4_u128_from_u64(caps->zone_size_bytes);
            hn4_u128_t rem = hn4_u128_mod(capacity_bytes, zone_sz);
            capacity_bytes = hn4_u128_sub(capacity_bytes, rem);
        }
#else
        capacity_bytes = caps->total_capacity_bytes;
        
        /* If ZNS, Align Down using bitwise/integer math */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
            capacity_bytes = HN4_ALIGN_DOWN(capacity_bytes, caps->zone_size_bytes);
        }
#endif
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
      uint64_t cortex_sz;
    
    #ifdef HN4_USE_128BIT
        /* 128-bit math for cortex size calculation */
        hn4_u128_t cap_128 = capacity_bytes;
    
        /* 2% Logic: (Cap / 100) * 2 */
        hn4_u128_t one_pct = hn4_u128_div_u64(cap_128, 100);
        hn4_u128_t two_pct = hn4_u128_mul_u64(one_pct, 2);
    
        /* Clamp to u64 for metadata region size (practical limit) */
        if (two_pct.hi > 0) cortex_sz = UINT64_MAX;
        else cortex_sz = two_pct.lo;
    #else
        /* Standard 64-bit Logic */
        if (capacity_bytes > (UINT64_MAX / 2)) {
            cortex_sz = (capacity_bytes / 100) * 2;
        } else {
            cortex_sz = (capacity_bytes * 2) / 100;
        }
    #endif
    
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

    /* In _calc_geometry */
    /* QMask (Quality Map) - 2 bits per block */
    uint64_t qmask_bytes = (total_blocks + (HN4_QMASK_BLOCKS_PER_BYTE - 1)) / HN4_QMASK_BLOCKS_PER_BYTE;
    uint64_t qmask_sz    = HN4_ALIGN_UP(qmask_bytes, bs);
    
    /* Use from_sectors */
    sb_out->info.lba_qmask_start = hn4_lba_from_sectors(offset / ss);
    offset += qmask_sz;

    /* 
     * Strict Block Alignment.
     * The alignment target must be a multiple of the Block Size to ensure
     * the Flux region starts on a valid block boundary.
     * 1. If align < bs, upgrade align to bs.
     * 2. If align % bs != 0, round align up to next multiple of bs.
     */
    if (align < bs) align = bs;
    
    if (align % bs != 0) {
        align = HN4_ALIGN_UP(align, bs);
    }

    /* Apply Alignment to Offset */
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
    
    if (HN4_UNLIKELY(capacity_bytes < min_required)) {
        HN4_LOG_ERR("Drive too small for layout. Need %llu bytes.", min_required);
        return HN4_ERR_ENOSPC;
    }

    uint64_t chron_end_offset = HN4_ALIGN_DOWN(capacity_bytes - tail_rsv, bs);
        
    if (chron_end_offset < chronicle_sz) return HN4_ERR_GEOMETRY;

    uint64_t chron_start_offset = chron_end_offset - chronicle_sz;

    if (chron_start_offset < offset) {
        HN4_LOG_ERR("Drive too small. Metadata overlaps Chronicle.");
        return HN4_ERR_ENOSPC;
    }

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
    
   /* Horizon (D1.5) - 10% (or 2% for Archive) */
    uint64_t horizon_pct = (pid == HN4_PROFILE_ARCHIVE) ? 2 : 10;
    uint64_t horizon_sz;

    /* Overflow Guard */
    if (capacity_bytes > (UINT64_MAX / horizon_pct)) {
        horizon_sz = (capacity_bytes / 100) * horizon_pct;
    } else {
        horizon_sz = (capacity_bytes * horizon_pct) / 100;
    }
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
    if (HN4_UNLIKELY(!dev)) return HN4_ERR_INVALID_ARGUMENT;

    const hn4_hal_caps_t* live_caps_ptr = hn4_hal_get_caps(dev);
    if (HN4_UNLIKELY(!live_caps_ptr)) return HN4_ERR_INTERNAL_FAULT;

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

        uint32_t pid = params->target_profile;
        if (pid >= HN4_MAX_PROFILES) return HN4_ERR_INVALID_ARGUMENT;
        
        uint64_t min_limit = PROFILE_SPECS[pid].min_cap;
        
        /* If profile min is unset or generic default, we can enforce a sane minimum */
        if (min_limit == 0) min_limit = 1024 * 1024; // 1MB absolute floor

        if (vcap_check < min_limit) {
            HN4_LOG_CRIT("Wormhole Capacity too small for profile (Val=%llu Min=%llu).", 
                         (unsigned long long)vcap_check, (unsigned long long)min_limit);
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
    if (HN4_UNLIKELY(res != HN4_OK)) return res;

    /* --- STEP 2: SANITIZE (THE NUKE) --- */
    hn4_size_t sb_cap = sb_cpu.info.total_capacity;
    hn4_size_t phys_cap = caps->total_capacity_bytes;
    hn4_size_t wipe_cap;

    /* 
     * Spatial Array / Virtual Capacity Safety:
     * If the FS geometry (sb_cap) exceeds physical storage (phys_cap),
     * we must only sanitize what physically exists to avoid HAL/Hardware errors.
     */
#ifdef HN4_USE_128BIT
    if (hn4_u128_cmp(sb_cap, phys_cap) > 0) {
        wipe_cap = phys_cap;
    } else {
        wipe_cap = sb_cap;
    }
#else
    wipe_cap = (sb_cap > phys_cap) ? phys_cap : sb_cap;
#endif

    if (caps->hw_flags & HN4_HW_ZNS_NATIVE) {
        #ifdef HN4_USE_128BIT

        hn4_u128_t zone_sz_128 = hn4_u128_from_u64(caps->zone_size_bytes);
        
        /* Check alignment of the Logical SB Capacity */
        hn4_u128_t rem = hn4_u128_mod(sb_cap, zone_sz_128);
        if (rem.lo != 0 || rem.hi != 0) {
            HN4_LOG_CRIT("ZNS Format Error: Calculated capacity is not zone-aligned.");
            return HN4_ERR_ALIGNMENT_FAIL;
        }
        
        /* Ensure wipe_cap is also zone aligned (clamp down if phys_cap is weird) */
        rem = hn4_u128_mod(wipe_cap, zone_sz_128);
        if (rem.lo != 0 || rem.hi != 0) {
             wipe_cap = hn4_u128_sub(wipe_cap, rem);
        }

        #else
        if (sb_cap % caps->zone_size_bytes != 0) {
            HN4_LOG_CRIT("ZNS Format Error: Calculated capacity is not zone-aligned.");
            return HN4_ERR_ALIGNMENT_FAIL;
        }
        /* Align wipe cap */
        wipe_cap = HN4_ALIGN_DOWN(wipe_cap, caps->zone_size_bytes);
        #endif
        
        res = _sanitize_zns(dev, wipe_cap, caps->zone_size_bytes, caps->logical_block_size);
    } else {
        res = _sanitize_generic(dev, wipe_cap, caps->logical_block_size);
    }
    
    if (HN4_UNLIKELY(res != HN4_OK)) {
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

    if (HN4_UNLIKELY(snap_cap_val != curr_cap_val || 
                     baseline_ss != current_live_caps->logical_block_size)) 
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

    if (HN4_UNLIKELY(bs < ss || (bs % ss) != 0)) {
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
        uint64_t phys_end_sect_ = caps->total_capacity_bytes / ss; \
        uint64_t safe_end_ = (end_lba); \
        if (safe_end_ > phys_end_sect_) safe_end_ = phys_end_sect_; \
        \
        if (safe_end_ > (start_lba)) { \
            uint64_t count_ = safe_end_ - (start_lba); \
            uint64_t len_ = count_ * ss; \
            if ((res = _zero_region_explicit(dev, start_lba, len_, bs)) != HN4_OK) return res; \
        } \
    } while(0)
#endif

    SAFE_ZERO_REGION(sb_cpu.info.lba_epoch_start,  sb_cpu.info.lba_cortex_start);
    SAFE_ZERO_REGION(sb_cpu.info.lba_cortex_start, sb_cpu.info.lba_bitmap_start);
    SAFE_ZERO_REGION(sb_cpu.info.lba_bitmap_start, sb_cpu.info.lba_qmask_start);
    SAFE_ZERO_REGION(sb_cpu.info.lba_horizon_start, sb_cpu.info.journal_start);
    
    #undef SAFE_ZERO_REGION

    if (HN4_UNLIKELY((res = _survey_silicon_cartography(dev, &sb_cpu)) != HN4_OK)) {
        return res;
    }

    sb_cpu.info.state_flags |= HN4_VOL_METADATA_ZEROED;

    /* Write Genesis Anchors */
    if (HN4_UNLIKELY((res = hn4_anchor_write_genesis(dev, &sb_cpu)) != HN4_OK)) return res;
    if (HN4_UNLIKELY((res = hn4_epoch_write_genesis(dev, &sb_cpu)) != HN4_OK)) return res;

    hn4_hal_barrier(dev);

    /* --- STEP 5: COMMIT SUPERBLOCKS --- */
    uint32_t write_sz = HN4_ALIGN_UP(HN4_SB_SIZE, bs); 
    void* sb_buf = hn4_hal_mem_alloc(write_sz);
    if (HN4_UNLIKELY(!sb_buf)) return HN4_ERR_NOMEM;

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
    /* Use physical capacity for bounds checking */
    uint64_t phys_bytes = hn4_addr_to_u64(caps->total_capacity_bytes);

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

    if (caps->hw_flags & HN4_HW_ZNS_NATIVE) write_south = false;

    /* 
     * If Virtual Layout > Physical Drive, disable mirrors that land in the void.
     * They will be written later by the Array Controller during expansion/rebalance.
     */
    bool write_east = (east_bytes + write_sz <= phys_bytes);
    bool write_west = (west_bytes + write_sz <= phys_bytes);
    if (south_bytes + write_sz > phys_bytes) write_south = false;

#endif

    res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_n, sb_buf, write_sz, bs);
    
    if (res == HN4_OK) {
        hn4_hal_barrier(dev);

        if (res == HN4_OK && write_east) res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_e, sb_buf, write_sz, bs);
        if (res == HN4_OK && write_west) res = hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_w, sb_buf, write_sz, bs);
        
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
   /* Poison on Failure */
    if (HN4_UNLIKELY(res != HN4_OK)) {
        HN4_LOG_CRIT("SB Commit Failed. Poisoning geometry.");
        memset(sb_buf, 0xDE, write_sz); 
        uint32_t* p = (uint32_t*)sb_buf;
        size_t last_idx = (write_sz / sizeof(uint32_t)) - 1;
        p[0] = HN4_POISON_PATTERN;        
        p[last_idx] = HN4_POISON_PATTERN; 

        disk_view = (hn4_superblock_t*)sb_buf;
        disk_view->raw.sb_crc = 0;
        crc = hn4_crc32(0, disk_view, HN4_SB_SIZE - 4);
        disk_view->raw.sb_crc = hn4_cpu_to_le32(crc);
        
        for (int i = 0; i < HN4_WRITE_RETRY_LIMIT; i++) {
            hn4_result_t w_res = HN4_OK;
            
            /* Always poison North (LBA 0) */
            if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_n, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            
            if (write_east) {
                if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_e, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            }
            
            if (write_west) {
                if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_w, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            }
            
            if (write_south) {
                if (hn4_hal_sync_io_large(dev, HN4_IO_WRITE, lba_s, sb_buf, write_sz, bs) != HN4_OK) w_res = HN4_ERR_HW_IO;
            }

            if (w_res == HN4_OK) {
                if (hn4_hal_barrier(dev) == HN4_OK) break;
            } else {
                hn4_hal_barrier(dev); /* Try to unstick queue before next retry */
            }
        }
    } else {
        HN4_LOG_VAL("Format Complete. UUID High", sb_cpu.info.volume_uuid.hi);
    }

    hn4_hal_mem_free(sb_buf);
    return res;
}