/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Epoch Ring Manager
 * SOURCE:      hn4_epoch.c
 * VERSION:     8.3
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_epoch.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_annotations.h"
#include "hn4_constants.h"
#include "hn4_errors.h"

#include <string.h>

/* =========================================================================
 * CONSTANTS & CONFIGURATIONS
 * ========================================================================= */

_Static_assert(sizeof(hn4_epoch_header_t) <= 512, "HN4: Epoch Header exceeds minimum block size guarantees");

#define HN4_EPOCH_DRIFT_MAX_FUTURE  5000
#define HN4_EPOCH_DRIFT_MAX_PAST    100
#define HN4_EPOCH_WRAP_THRESHOLD    10000

#define HN4_RING_SIZE_BYTES         (1024ULL * 1024ULL) /* 1MB Fixed Ring */

/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

/* 
 * UNIFIED GEOMETRY MAPPER
 * Prevents logic divergence between Read/Write paths.
 * Enforces strict bounds checking against Volume Capacity (in Blocks).
 */
static inline hn4_result_t _epoch_phys_map(
    uint64_t block_idx,
    uint32_t block_size,
    uint32_t sector_size,
    hn4_size_t vol_cap_bytes,
    hn4_addr_t* out_lba,
    uint32_t* out_sector_count
) {
    if (HN4_UNLIKELY(block_size == 0 || sector_size == 0)) return HN4_ERR_GEOMETRY;

    uint64_t sectors_per_block = block_size / sector_size;

#ifdef HN4_USE_128BIT
    hn4_u128_t blk_128 = hn4_u128_from_u64(block_idx);
    hn4_u128_t byte_offset = hn4_u128_mul_u64(blk_128, block_size);
    
    if (HN4_UNLIKELY(hn4_u128_cmp(byte_offset, vol_cap_bytes) >= 0)) {
        return HN4_ERR_GEOMETRY;
    }

    *out_lba = hn4_u128_mul_u64(blk_128, sectors_per_block);
#else
    if (HN4_UNLIKELY(block_idx > (UINT64_MAX / block_size))) return HN4_ERR_GEOMETRY;
    
    uint64_t byte_offset = block_idx * block_size;

    if (HN4_UNLIKELY(byte_offset >= vol_cap_bytes)) return HN4_ERR_GEOMETRY;

    if (HN4_UNLIKELY(block_idx > (UINT64_MAX / sectors_per_block))) return HN4_ERR_GEOMETRY;

    *out_lba = block_idx * sectors_per_block;
#endif

    *out_sector_count = (uint32_t)sectors_per_block;
    
    return HN4_OK;
}

/* =========================================================================
 * GENESIS
 * ========================================================================= */

_Check_return_ hn4_result_t hn4_epoch_write_genesis(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_IN const hn4_superblock_t* sb
)
{
    hn4_result_t        status;
    void*               dma_buffer = NULL;
    hn4_epoch_header_t  cpu_epoch;

    if (HN4_UNLIKELY(!dev || !sb)) return HN4_ERR_INVALID_ARGUMENT;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    const uint32_t bs = sb->info.block_size;
    const uint32_t ss = caps->logical_block_size;

    if (HN4_UNLIKELY(bs < sizeof(hn4_epoch_header_t))) return HN4_ERR_GEOMETRY;
    if (HN4_UNLIKELY(ss == 0 || bs % ss != 0)) return HN4_ERR_ALIGNMENT_FAIL;

    dma_buffer = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!dma_buffer)) return HN4_ERR_NOMEM;

    /* Populate Genesis Data */
    memset(&cpu_epoch, 0, sizeof(cpu_epoch)); 
    cpu_epoch.epoch_id  = 1;
    cpu_epoch.timestamp = sb->info.generation_ts;
    cpu_epoch.flags     = HN4_VOL_CLEAN;
    cpu_epoch.epoch_crc = hn4_epoch_calc_crc(&cpu_epoch);

    _secure_zero(dma_buffer, bs);
    hn4_epoch_to_disk(&cpu_epoch, (hn4_epoch_header_t*)dma_buffer);

    uint64_t start_sect;
    uint64_t vol_cap;
#ifdef HN4_USE_128BIT
    start_sect = sb->info.lba_epoch_start.lo;
    vol_cap = caps->total_capacity_bytes.lo;
#else
    start_sect = sb->info.lba_epoch_start;
    vol_cap = caps->total_capacity_bytes;
#endif

    uint32_t spb = bs / ss;
    if (HN4_UNLIKELY(start_sect % spb != 0)) {
        status = HN4_ERR_ALIGNMENT_FAIL;
        goto Cleanup;
    }

    uint64_t start_blk = start_sect / spb;
    hn4_addr_t target_lba;
    uint32_t io_sectors;

    status = _epoch_phys_map(start_blk, bs, ss, vol_cap, &target_lba, &io_sectors);
    if (HN4_UNLIKELY(status != HN4_OK)) goto Cleanup;

    status = hn4_hal_sync_io(dev, HN4_IO_WRITE, target_lba, dma_buffer, io_sectors);

Cleanup:
    hn4_hal_mem_free(dma_buffer);
    return status;
}

/* =========================================================================
 * RING VALIDATION
 * ========================================================================= */

typedef enum {
    EPOCH_STATE_SYNCED          = 0,
    EPOCH_STATE_FUTURE_DILATION = 1,
    EPOCH_STATE_FUTURE_TOXIC    = 2,
    EPOCH_STATE_PAST_SKEW       = 3,
    EPOCH_STATE_PAST_TOXIC      = 4
} hn4_epoch_drift_state_t;

/* Lookup Table for Error Mapping */
static const hn4_result_t _drift_err_map[] = {
    [EPOCH_STATE_SYNCED]          = HN4_OK,
    [EPOCH_STATE_FUTURE_DILATION] = HN4_ERR_TIME_DILATION,
    [EPOCH_STATE_FUTURE_TOXIC]    = HN4_ERR_MEDIA_TOXIC,
    [EPOCH_STATE_PAST_SKEW]       = HN4_ERR_GENERATION_SKEW,
    [EPOCH_STATE_PAST_TOXIC]      = HN4_ERR_MEDIA_TOXIC
};

_Check_return_ hn4_result_t hn4_epoch_check_ring(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_IN const hn4_superblock_t* sb, 
    HN4_IN uint64_t vol_cap
)
{
    const hn4_hal_caps_t* caps;
    void*                 io_buf = NULL;
    uint64_t              ring_curr_idx;
    uint64_t              ring_start_idx;
    uint32_t              ss;
    uint32_t              bs;
    uint32_t              io_sectors;
    hn4_result_t          status;
    hn4_epoch_header_t    epoch;
    hn4_addr_t            target_lba;

    if (HN4_UNLIKELY(!dev || !sb)) return HN4_ERR_INTERNAL_FAULT;
    caps = hn4_hal_get_caps(dev);
    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    ss = caps->logical_block_size;
    bs = sb->info.block_size;

    if (HN4_UNLIKELY(bs < sizeof(hn4_epoch_header_t))) {
        return HN4_ERR_GEOMETRY;
    }

#ifdef HN4_USE_128BIT
    ring_curr_idx = sb->info.epoch_ring_block_idx.lo;
    uint64_t ring_start_sector = sb->info.lba_epoch_start.lo;
#else
    ring_curr_idx = sb->info.epoch_ring_block_idx;
    uint64_t ring_start_sector = sb->info.lba_epoch_start;
#endif

    if (HN4_UNLIKELY(ss == 0 || (bs % ss != 0))) return HN4_ERR_GEOMETRY;
    
    uint32_t sec_per_blk = bs / ss;
    if (HN4_UNLIKELY(ring_start_sector % sec_per_blk != 0)) return HN4_ERR_ALIGNMENT_FAIL;

    ring_start_idx = ring_start_sector / sec_per_blk;

    uint64_t ring_len_blks = (HN4_RING_SIZE_BYTES + bs - 1) / bs;
    uint64_t ring_end_blk = ring_start_idx + ring_len_blks;
    uint64_t total_vol_blks = vol_cap / bs;

    if (HN4_UNLIKELY(ring_end_blk < ring_start_idx || ring_end_blk > total_vol_blks)) {
        return HN4_ERR_GEOMETRY;
    }

    if (HN4_UNLIKELY(_epoch_phys_map(ring_curr_idx, bs, ss, vol_cap, &target_lba, &io_sectors) != HN4_OK)) {
        return HN4_ERR_GEOMETRY;
    }

    io_buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!io_buf)) return HN4_ERR_NOMEM;

    status = hn4_hal_sync_io(dev, HN4_IO_READ, target_lba, io_buf, io_sectors);
    if (HN4_UNLIKELY(status != HN4_OK)) {
        goto Cleanup;
    }

    memcpy(&epoch, io_buf, sizeof(epoch));
    hn4_epoch_to_cpu(&epoch);

    if (HN4_UNLIKELY(epoch.epoch_crc != hn4_epoch_calc_crc(&epoch))) {
        status = HN4_ERR_EPOCH_LOST;
        goto Cleanup;
    }

    /* ---------------------------------------------------------------------
     * DRIFT ANALYSIS (With Wrap-Around Logic)
     * --------------------------------------------------------------------- */
    uint64_t disk_id = epoch.epoch_id;
    uint64_t mem_id  = sb->info.current_epoch_id;
    hn4_epoch_drift_state_t state;
    
    uint64_t diff;
    bool is_future;

    if (disk_id >= mem_id) {
        /* 
         * Case A: Disk >= Mem. Usually Future.
         * EXCEPTION (Past Wrap): Disk is MAX (e.g. F...FF), Mem is Small (e.g. 5).
         * This implies the Superblock wrapped around and is actually NEWER than the Disk.
         */
        if (HN4_UNLIKELY(disk_id > (UINT64_MAX - HN4_EPOCH_WRAP_THRESHOLD) && 
                         mem_id < HN4_EPOCH_WRAP_THRESHOLD)) 
        {
            /* Correct Diff: (MAX - Disk) + 1 + Mem */
            diff = (UINT64_MAX - disk_id) + 1 + mem_id;
            is_future = false; /* SB is actually ahead (Time passed) */
        } else {
            diff = disk_id - mem_id;
            is_future = true; /* Disk is ahead (Time dilation) */
        }
    } else {
        /* 
         * Case B: Mem > Disk. Usually Past.
         * EXCEPTION (Future Wrap): Mem is MAX, Disk is Small.
         * This implies the Disk wrapped around and is NEWER than the Superblock.
         */
        if (HN4_UNLIKELY(mem_id > (UINT64_MAX - HN4_EPOCH_WRAP_THRESHOLD) && 
                         disk_id < HN4_EPOCH_WRAP_THRESHOLD)) 
        {
            diff = (UINT64_MAX - mem_id) + 1 + disk_id;
            is_future = true; /* Disk is actually ahead */
        } else {
            diff = mem_id - disk_id;
            is_future = false; /* SB is ahead (Normal lag) */
        }
    }

    /* Classify State */
    if (diff == 0) {
        state = EPOCH_STATE_SYNCED;
    } else if (is_future) {
        state = (diff > HN4_EPOCH_DRIFT_MAX_FUTURE) 
                ? EPOCH_STATE_FUTURE_TOXIC 
                : EPOCH_STATE_FUTURE_DILATION;
    } else {
        state = (diff > HN4_EPOCH_DRIFT_MAX_PAST) 
                ? EPOCH_STATE_PAST_TOXIC 
                : EPOCH_STATE_PAST_SKEW;
    }

    /* Map to Result */
    status = _drift_err_map[state];

Cleanup:
    if (io_buf) hn4_hal_mem_free(io_buf);
    return status;
}

/* =========================================================================
 * ADVANCEMENT
 * ========================================================================= */

_Check_return_ hn4_result_t hn4_epoch_advance(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_IN const hn4_superblock_t* sb,
    HN4_IN bool is_read_only,
    HN4_OUT_OPT uint64_t* out_new_id,
    HN4_OUT_OPT hn4_addr_t* out_new_ptr
)
{
    if (HN4_UNLIKELY(is_read_only || (sb->info.state_flags & HN4_VOL_TOXIC))) {
        return HN4_ERR_MEDIA_TOXIC;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (HN4_UNLIKELY(!caps)) return HN4_ERR_INTERNAL_FAULT;

    uint32_t bs = sb->info.block_size;
    uint32_t ss = caps->logical_block_size;
    
    if (HN4_UNLIKELY(bs < sizeof(hn4_epoch_header_t))) return HN4_ERR_GEOMETRY;

    void* io_buf = hn4_hal_mem_alloc(bs);
    if (HN4_UNLIKELY(!io_buf)) return HN4_ERR_NOMEM;

    /* Generation Exhaustion Check */
    if (HN4_UNLIKELY(sb->info.copy_generation >= 0xFFFFFFFFFFFFFFF0ULL)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_EEXIST; 
    }

    /* Prepare Header */
    hn4_epoch_header_t epoch;
    _secure_zero(&epoch, sizeof(epoch));

    uint64_t next_id = sb->info.current_epoch_id + 1;
    epoch.epoch_id = next_id;
    epoch.timestamp = hn4_hal_get_time_ns();
    epoch.flags = HN4_VOL_UNMOUNTING;
    
    uint64_t gen_state = sb->info.copy_generation;
    epoch.d0_root_checksum = hn4_cpu_to_le32(hn4_crc32(0, &gen_state, sizeof(uint64_t)));

    _secure_zero(io_buf, bs);
    hn4_epoch_to_disk(&epoch, io_buf);

    /* Ring Topology */
    hn4_addr_t start_sect_lba = sb->info.lba_epoch_start;
    hn4_addr_t ring_curr_blk  = sb->info.epoch_ring_block_idx;
    hn4_size_t vol_cap        = caps->total_capacity_bytes;

    uint32_t spb = bs / ss; 

    uint64_t ring_start_blk_idx;
    uint64_t ring_curr_blk_idx;

#ifdef HN4_USE_128BIT
    if (HN4_UNLIKELY(start_sect_lba.hi > 0 || ring_curr_blk.hi > 0)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_GEOMETRY;
    }
    if (HN4_UNLIKELY(start_sect_lba.lo % spb != 0)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_ALIGNMENT_FAIL;
    }
    ring_start_blk_idx = start_sect_lba.lo / spb;
    ring_curr_blk_idx  = ring_curr_blk.lo;
#else
    if (HN4_UNLIKELY(start_sect_lba % spb != 0)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_ALIGNMENT_FAIL;
    }
    ring_start_blk_idx = start_sect_lba / spb;
    ring_curr_blk_idx  = ring_curr_blk;
#endif

    uint64_t target_sz = HN4_RING_SIZE_BYTES; 
    if (sb->info.format_profile == HN4_PROFILE_PICO) {
        target_sz = 2 * bs;
    }

    uint64_t ring_len_blks = (target_sz + bs - 1) / bs;
    
    if (HN4_UNLIKELY(ring_curr_blk_idx < ring_start_blk_idx)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_DATA_ROT; 
    }

    /* Advance Pointer */
    uint64_t relative_idx = ring_curr_blk_idx - ring_start_blk_idx;
    uint64_t next_relative_idx = (relative_idx + 1) % ring_len_blks;
    uint64_t write_blk_idx = ring_start_blk_idx + next_relative_idx;

    hn4_addr_t target_lba;
    uint32_t io_sectors;

    if (HN4_UNLIKELY(_epoch_phys_map(write_blk_idx, bs, ss, vol_cap, &target_lba, &io_sectors) != HN4_OK)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_GEOMETRY;
    }

    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_WRITE, target_lba, io_buf, io_sectors);

    if (HN4_LIKELY(res == HN4_OK)) {
        if (out_new_id) *out_new_id = next_id;
        if (out_new_ptr) {
#ifdef HN4_USE_128BIT
            out_new_ptr->lo = write_blk_idx;
            out_new_ptr->hi = 0;
#else
            *out_new_ptr = write_blk_idx;
#endif
        }
    } else {
        if (out_new_id) *out_new_id = sb->info.current_epoch_id;
        if (out_new_ptr) *out_new_ptr = sb->info.epoch_ring_block_idx;
    }

    hn4_hal_mem_free(io_buf);
    return res;
}