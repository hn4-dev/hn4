/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Endianness Normalization & Safety
 * SOURCE:      hn4_endians.c
 * VERSION:     4.2 (Reference Standard)
 *
 * COPYRIGHT:   (c) 2025 The Hydra-Nexus Team. All rights reserved.
 *
 * DESCRIPTION:
 * Defines the unified byte-order contract for the HN4 on-disk format (LE).
 * Provides bulk conversion routines optimized for instruction pipelining and
 * includes strict runtime sanity checks to ensure ABI compliance across
 * diverse CPU architectures.
 *
 * IMPLEMENTATION:
 * 1. Bulk Processing: Cache-line optimized routines for array conversion.
 * 2. Manual Unrolling: 4-way unroll to maximize ALU throughput.
 * 3. Safety: Runtime validation of compile-time endianness assumptions.
 */

#include "hn4_endians.h"
#include <assert.h>

/* =========================================================================
 * BULK CONVERSION (CACHE LINE OPTIMIZED)
 * ========================================================================= */

/**
 * hn4_bulk_le64_to_cpu
 * In-place conversion of 64-bit integer arrays.
 * Used for: Armored Bitmap loading, Sector Translation.
 */
void hn4_bulk_le64_to_cpu(uint64_t* __restrict data, size_t count) {
    /* Hard Assertion: Data must be aligned to 8 bytes */
    assert(((uintptr_t)data & 7) == 0 && "HN4: Bulk pointer unaligned!");

#if HN4_IS_BIG_ENDIAN
    size_t i;
    /*
     * 4-way Loop Unrolling.
     * Allows superscalar CPUs to dispatch multiple BSWAP instructions.
     */
    for (i = 0; i < (count & ~3); i += 4) {
        data[i]     = hn4_bswap64(data[i]);
        data[i+1]   = hn4_bswap64(data[i+1]);
        data[i+2]   = hn4_bswap64(data[i+2]);
        data[i+3]   = hn4_bswap64(data[i+3]);
    }
    /* Handle tail elements */
    for (; i < count; i++) {
        data[i] = hn4_bswap64(data[i]);
    }
#else
    /* No-Op on Little Endian Systems */
    (void)data;
    (void)count;
#endif
}

/**
 * hn4_bulk_cpu_to_le64
 * In-place conversion from CPU Native to Disk (LE).
 * Symmetric to le64_to_cpu.
 */
void hn4_bulk_cpu_to_le64(uint64_t* __restrict data, size_t count) {
    /* Operation is symmetric (Swap is its own inverse) */
    hn4_bulk_le64_to_cpu(data, count);
}

/* =========================================================================
 * RUNTIME SANITY CHECK (BARE METAL SAFETY)
 * ========================================================================= */

/**
 * hn4_endian_sanity_check
 * Verifies that Compile-Time Macros match Runtime Reality.
 * Returns: true if safe, false if panic.
 */
bool hn4_endian_sanity_check(void) {
    /* 1. Verify Basic Word Order */
    volatile uint32_t test_val = 0x11223344;
    volatile uint8_t* byte_ptr = (volatile uint8_t*)&test_val;
    bool runtime_is_le = (*byte_ptr == 0x44);

#if HN4_IS_LITTLE_ENDIAN
    if (!runtime_is_le) {
        /* Fatal: Compiled for LE, running on BE */
        return false;
    }
#else
    if (runtime_is_le) {
        /* Fatal: Compiled for BE, running on LE */
        return false;
    }
#endif

    /* 2. Verify 128-bit Struct Layout & Swap Logic */
    hn4_u128_t u128_test;
    u128_test.lo = 0x1122334455667788ULL;
    u128_test.hi = 0x99AABBCCDDEEFF00ULL;

    /*
     * Convert to Disk Format (LE).
     * - If we are on LE host: No change.
     * - If we are on BE host: Bytes inside lo/hi swap, but lo remains lo.
     */
    hn4_u128_t u128_disk = hn4_cpu_to_le128(u128_test);
    const uint8_t* p = (const uint8_t*)&u128_disk;

    /*
     * On a correctly encoded LE disk structure:
     * Offset 0 should be LSB of 'lo' (0x88)
     * Offset 8 should be LSB of 'hi' (0x00)
     */
    if (p[0] != 0x88 || p[8] != 0x00) {
        return false;
    }

    return true;
}

/* =========================================================================
 * STRUCTURE CONVERSIONS
 * ========================================================================= */

void hn4_sb_to_cpu(hn4_superblock_t* sb) {
    /* Optimizes out completely on LE systems */
#if HN4_IS_BIG_ENDIAN
    sb->info.magic = hn4_bswap64(sb->info.magic);
    sb->info.version = hn4_bswap32(sb->info.version);
    sb->info.block_size = hn4_bswap32(sb->info.block_size);
    sb->info.volume_uuid = hn4_bswap128(sb->info.volume_uuid);
    
    sb->info.lba_epoch_start = hn4_bswap128(sb->info.lba_epoch_start);
    sb->info.total_capacity = hn4_bswap128(sb->info.total_capacity);
    sb->info.lba_cortex_start = hn4_bswap128(sb->info.lba_cortex_start);
    sb->info.lba_bitmap_start = hn4_bswap128(sb->info.lba_bitmap_start);
    sb->info.lba_flux_start = hn4_bswap128(sb->info.lba_flux_start);
    sb->info.lba_horizon_start = hn4_bswap128(sb->info.lba_horizon_start);
    sb->info.lba_stream_start = hn4_bswap128(sb->info.lba_stream_start);
    
    sb->info.current_epoch_id = hn4_bswap64(sb->info.current_epoch_id);
    sb->info.epoch_ring_ptr = hn4_bswap128(sb->info.epoch_ring_ptr);
    sb->info.copy_generation = hn4_bswap64(sb->info.copy_generation);
    
    sb->info.sentinel_cursor = hn4_bswap128(sb->info.sentinel_cursor);
    sb->info.hw_caps_flags = hn4_bswap64(sb->info.hw_caps_flags);
    sb->info.state_flags = hn4_bswap32(sb->info.state_flags);
    
    sb->info.compat_flags = hn4_bswap64(sb->info.compat_flags);
    sb->info.incompat_flags = hn4_bswap64(sb->info.incompat_flags);
    sb->info.ro_compat_flags = hn4_bswap64(sb->info.ro_compat_flags);
    sb->info.mount_intent = hn4_bswap64(sb->info.mount_intent);
    sb->info.dirty_bits = hn4_bswap64(sb->info.dirty_bits);
    sb->info.last_mount_time = hn4_bswap64(sb->info.last_mount_time);
    sb->info.journal_ptr = hn4_bswap128(sb->info.journal_ptr);
    sb->info.endian_tag = hn4_bswap32(sb->info.endian_tag);
    sb->info.format_profile = hn4_bswap32(sb->info.format_profile);
    sb->info.device_type_tag = hn4_bswap32(sb->info.device_type_tag);
    sb->info.generation_ts = hn4_bswap64(sb->info.generation_ts);
    sb->info.magic_tail = hn4_bswap64(sb->info.magic_tail);
    
    sb->raw.sb_crc = hn4_bswap32(sb->raw.sb_crc);
#else
    (void)sb; /* Zero Cost */
#endif
}

void hn4_sb_to_disk(const hn4_superblock_t* src, hn4_superblock_t* dst) {
    /* Copy first to handle alignment/overlap safety */
    if (src != dst) {
        /* Use simple struct assignment or memcpy */
        *dst = *src;
    }
    
    /* Calculate CRC BEFORE swapping if needed? 
       Actually, CRC is usually calculated on the LE representation.
       So we swap first, then CRC. 
    */
    
    /* Apply Endian Swap */
    hn4_sb_to_cpu(dst); /* Symmetric swap works both ways */
    
    /* Re-Calculate CRC on the raw LE bytes */
    /* Note: We assume the caller handles CRC calculation if they want it valid on disk. 
       Wait, usually `to_disk` implies preparing for IO. 
       Let's keep it pure conversion. Caller calculates CRC. 
    */
}

void hn4_epoch_to_cpu(hn4_epoch_header_t* ep) {
#if HN4_IS_BIG_ENDIAN
    ep->epoch_id = hn4_bswap64(ep->epoch_id);
    ep->timestamp = hn4_bswap64(ep->timestamp);
    ep->d0_root_checksum = hn4_bswap32(ep->d0_root_checksum);
    ep->epoch_crc = hn4_bswap32(ep->epoch_crc);
#else
    (void)ep;
#endif
}

void hn4_epoch_to_disk(const hn4_epoch_header_t* src, hn4_epoch_header_t* dst) {
    if (src != dst) *dst = *src;
    hn4_epoch_to_cpu(dst); /* Symmetric */
}

/* =========================================================================
 * INTEGRITY HELPERS
 * ========================================================================= */

uint32_t hn4_sb_calc_crc(const hn4_superblock_t* sb) {
    /* 
     * Calculates CRC of the Superblock up to the CRC field.
     * Works on both CPU-native and LE-disk structures (CRC is over bytes).
     */
    return hn4_crc32(0, sb, offsetof(hn4_superblock_t, raw.sb_crc));
}

uint32_t hn4_epoch_calc_crc(const hn4_epoch_header_t* ep) {
    return hn4_crc32(0, ep, offsetof(hn4_epoch_header_t, epoch_crc));
}