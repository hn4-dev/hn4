/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Volume Mount & Recovery Logic
 * SOURCE:      hn4_mount.c
 * VERSION:     7.7 (Hardened / Self-Healing)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * ENGINEERING SPECIFICATION:
 *  1. SELF-HEALING: Cardinal Vote (Quorum) reconstructs damaged mirrors.
 *  2. TAINT DECAY: Counters halve on successful clean mount.
 *  3. EPOCH SAFETY: Validates Journal Lag before accepting state.
 *  4. SOUTHBRIDGE: Small volumes disable South SB to prevent corruption.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_endians.h"
#include "hn4_crc.h"
#include "hn4_ecc.h"
#include "hn4_errors.h"
#include "hn4_epoch.h"
#include "hn4_chronicle.h" 
#include "hn4_annotations.h" 
#include "hn4_constants.h"
#include <string.h>
#include <assert.h>

/* =========================================================================
 * HELPER INLINES & MACROS
 * ========================================================================= */

/* 
 * Address Translation: FS Block Index -> Device Sector LBA 
 * CRITICAL: This is the bridge between FS Logic (Blocks) and HAL Logic (Sectors).
 */
static inline hn4_result_t _phys_lba_from_block(
    uint64_t block_idx, 
    uint32_t block_size, 
    uint32_t sector_size, 
    uint64_t total_capacity_bytes,
    hn4_addr_t* out_addr
) {
    if (block_size == 0 || sector_size == 0) return HN4_ERR_GEOMETRY;
    if (block_size % sector_size != 0) return HN4_ERR_ALIGNMENT_FAIL;

    uint64_t sectors_per_block = block_size / sector_size;
    
    /* 
     * Mathematical Overflow Prevention
     */
    if (block_idx >= (total_capacity_bytes / block_size)) {
        return HN4_ERR_GEOMETRY;
    }

    /* Safe to multiply now */
    uint64_t raw_lba = block_idx * sectors_per_block;

#ifdef HN4_USE_128BIT
    out_addr->lo = raw_lba;
    out_addr->hi = 0;
#else
    *out_addr = raw_lba;
#endif
    return HN4_OK;
}

/* =========================================================================
 * 1. INTERNAL HELPER FOR AI
 * ========================================================================= */

typedef struct {
    uint32_t gpu_id;
    uint32_t affinity_weight;
    uint64_t lba_start;
    uint64_t lba_len;
} _hn4_topo_entry_t;

/* QSort Comparator: Sort by LBA Start */
static int _topo_cmp(const void* a, const void* b) {
    const _hn4_topo_entry_t* ra = (const _hn4_topo_entry_t*)a;
    const _hn4_topo_entry_t* rb = (const _hn4_topo_entry_t*)b;
    if (ra->lba_start < rb->lba_start) return -1;
    if (ra->lba_start > rb->lba_start) return 1;
    return 0;
}
#define HN4_MAX_TOPOLOGY_REGIONS 64

/* =========================================================================
 * 2. SUPERBLOCK VALIDATION
 * ========================================================================= */

/**
 * _validate_sb_integrity
 * Performs strictly Read-Only checks on a memory buffer.
 * Verifies Magic, Poison Patterns, and CRC32C.
 */
/* =========================================================================
 * SUPERBLOCK INTEGRITY VALIDATOR
 * ========================================================================= */
static hn4_result_t _validate_sb_integrity(HN4_IN const void* buffer)
{
    /* Use local copy to prevent unaligned access faults on strict archs */
    uint32_t raw32[4];
    memcpy(raw32, buffer, sizeof(raw32));

    /* Cast to struct to ensure we respect ABI padding/offsets */
    const hn4_superblock_t* sb_view = (const hn4_superblock_t*)buffer;

    /* 1. Magic Check */
    if (HN4_UNLIKELY(hn4_le64_to_cpu(sb_view->info.magic) != HN4_MAGIC_SB)) {
        /* Poison Heuristic: Check first 16 bytes for 0xDEADBEEF repetition */
        if (raw32[0] == HN4_POISON_PATTERN && 
            raw32[1] == HN4_POISON_PATTERN &&
            raw32[2] == HN4_POISON_PATTERN &&
            raw32[3] == HN4_POISON_PATTERN) {
            HN4_LOG_CRIT("Mount Blocked: Volume contains format poison pattern.");
            return HN4_ERR_WIPE_PENDING;
        }
        return HN4_ERR_BAD_SUPERBLOCK;
    }

    /* 2. Reject Zero UUID (Security.ZeroUUID) */
    const hn4_superblock_t* sb = (const hn4_superblock_t*)buffer;
    /* We check raw values. 0 is 0 in Big or Little Endian. */
    if (sb->info.volume_uuid.lo == 0 && sb->info.volume_uuid.hi == 0) {
        HN4_LOG_CRIT("Mount Blocked: Zero UUID detected");
        return HN4_ERR_BAD_SUPERBLOCK;
    }

    /* 3. CRC Check */
    uint32_t stored_crc;
    const uint8_t* byte_ptr = (const uint8_t*)buffer;
    /* CRC is at the very end of the 8KB block */
    memcpy(&stored_crc, byte_ptr + HN4_SB_SIZE - 4, 4);
    stored_crc = hn4_le32_to_cpu(stored_crc);

    /* CRC covers bytes 0 to (Size - 4) */
    uint32_t calc_crc = hn4_crc32(0, byte_ptr, HN4_SB_SIZE - 4);
    if (HN4_UNLIKELY(calc_crc != stored_crc)) {
        HN4_LOG_WARN("SB CRC Mismatch. Stored: %08X, Calc: %08X", stored_crc, calc_crc);
        return HN4_ERR_BAD_SUPERBLOCK;
    }

    return HN4_OK;
}

/**
 * _read_sb_at_lba
 * Reads a Superblock from a physical LBA (Sector Index).
 * Converts disk-format to cpu-format.
 */
static hn4_result_t _read_sb_at_lba(
    HN4_IN hn4_hal_device_t* dev,
    HN4_IN hn4_addr_t lba,
    HN4_IN uint32_t dev_sector_size,
    HN4_IN uint32_t known_block_size,
    HN4_INOUT void* io_buf,
    HN4_OUT hn4_superblock_t* out_sb
)
{
    if (dev_sector_size == 0) return HN4_ERR_GEOMETRY;

    /* 
     * Read logic must encompass the largest potential structure.
     * We read HN4_ALIGN_UP(MAX(BS, SB_SIZE), Sector).
     */

   /* 
     * Read logic must encompass the Superblock structure.
     * Clamp read size. Even if Block Size is huge (ZNS), we only need the SB.
     */

    uint32_t min_bytes = HN4_ALIGN_UP(HN4_SB_SIZE, dev_sector_size);
    
    // Do not expand buffer to Block Size if it exceeds reasonable bounds (e.g. 64KB)
    uint32_t read_bytes = min_bytes;
    if (known_block_size > 0 && known_block_size <= 65536) {
        read_bytes = HN4_MAX(min_bytes, HN4_ALIGN_UP(known_block_size, dev_sector_size));
    }

    uint32_t sectors = read_bytes / dev_sector_size;

    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_READ, lba, io_buf, sectors);
    if (res != HN4_OK) return res;

    res = _validate_sb_integrity(io_buf);
    if (res != HN4_OK) return res;

    memcpy(out_sb, io_buf, sizeof(hn4_superblock_t));
    hn4_sb_to_cpu(out_sb);

    /* Alignment Check: Block Size must be a multiple of Sector Size */
    if (out_sb->info.block_size % dev_sector_size != 0) {
        HN4_LOG_CRIT("Geometry Mismatch: FS_BS %u %% PHY_SS %u != 0", 
                     out_sb->info.block_size, dev_sector_size);
        return HN4_ERR_GEOMETRY;
    }

    return HN4_OK;
}

/* =========================================================================
 * 3. CARDINAL VOTE (QUORUM & SELF-HEALING)
 * ========================================================================= */

/**
 * _calc_south_offset
 * Returns Block Index of South SB, or HN4_OFFSET_INVALID if too small.
 */
static uint64_t _calc_south_offset(uint64_t capacity, uint32_t bs) {
    uint64_t sb_space = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
    
    /* 
     * Heuristic: Capacity must be at least 16x SB Size.
     */
    if (capacity < (sb_space * 16)) return HN4_OFFSET_INVALID;
    
    /* 
     * STRICT ALIGNMENT: 
     * Must match hn4_format.c logic: HN4_ALIGN_DOWN(cap_bytes - write_sz, bs);
     * We subtract first, then align down.
     */
    return HN4_ALIGN_DOWN(capacity - sb_space, bs);
}


static hn4_result_t _execute_cardinal_vote(
    HN4_IN hn4_hal_device_t* dev,
    HN4_IN bool allow_repair,
    HN4_OUT hn4_superblock_t* out_sb
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t sector_sz = caps->logical_block_size;
    uint64_t cap_bytes;
    if (!_addr_to_u64_checked(caps->total_capacity_bytes, &cap_bytes)) return HN4_ERR_GEOMETRY;
    if (sector_sz == 0 || cap_bytes == 0) return HN4_ERR_GEOMETRY;

    size_t current_buf_sz = 65536;
    void* probe_buf = hn4_hal_mem_alloc(current_buf_sz);
    void* heal_buf  = hn4_hal_mem_alloc(current_buf_sz);

    if (!probe_buf || !heal_buf) {
        if (probe_buf) hn4_hal_mem_free(probe_buf);
        if (heal_buf) hn4_hal_mem_free(heal_buf);
        return HN4_ERR_NOMEM;
    }

    hn4_superblock_t best_sb;
    memset(&best_sb, 0, sizeof(best_sb));
    bool found_valid = false;
    uint64_t max_gen = 0;
    uint64_t max_ts = 0;
    hn4_result_t final_res = HN4_ERR_BAD_SUPERBLOCK;
    
    uint32_t probe_sizes[] = { sector_sz, 4096, 16384, 65536, 0, 0 };
    hn4_superblock_t cand;

#ifdef HN4_USE_128BIT
    hn4_addr_t lba0 = {0, 0};
#else
    hn4_addr_t lba0 = 0;
#endif

    /* 1. North Probe */

    hn4_result_t res_north = _read_sb_at_lba(dev, lba0, sector_sz, 0, probe_buf, &cand);

    /* Capture Poison/Wipe Error immediately */
    if (res_north == HN4_ERR_WIPE_PENDING) {
        hn4_hal_mem_free(probe_buf);
        hn4_hal_mem_free(heal_buf);
        return HN4_ERR_WIPE_PENDING;
    }

    /* Reuse the result from the first read */
    if (res_north == HN4_OK) {
        memcpy(&best_sb, &cand, sizeof(cand));
        found_valid = true;
        max_gen = cand.info.copy_generation;
        max_ts = cand.info.last_mount_time;
        probe_sizes[4] = best_sb.info.block_size; 
    }

    /* 2. Mirror Probe */
    for (int p = 0; probe_sizes[p] != 0; p++) {
        uint32_t current_bs = probe_sizes[p];
        if (current_bs < sector_sz || current_bs % sector_sz != 0) continue;
        if (p > 0 && current_bs == probe_sizes[p-1]) continue;

        uint32_t required_sz = HN4_ALIGN_UP(HN4_SB_SIZE, current_bs);
        if (required_sz > current_buf_sz) {
            /* 
             * Set pointers to NULL after free to prevent double-free 
             * in cleanup label if subsequent allocation fails.
             */
            hn4_hal_mem_free(probe_buf); probe_buf = NULL;
            hn4_hal_mem_free(heal_buf);  heal_buf = NULL;
            
            current_buf_sz = required_sz;
            probe_buf = hn4_hal_mem_alloc(current_buf_sz);
            heal_buf = hn4_hal_mem_alloc(current_buf_sz);
            
            if (!probe_buf || !heal_buf) {
                final_res = HN4_ERR_NOMEM;
                goto cleanup;
            }
        }

        uint64_t block_indices[3];
        block_indices[0] = HN4_ALIGN_UP((cap_bytes / 100) * 33, current_bs) / current_bs;
        block_indices[1] = HN4_ALIGN_UP((cap_bytes / 100) * 66, current_bs) / current_bs;
        uint64_t s_off = _calc_south_offset(cap_bytes, current_bs);
        block_indices[2] = (s_off == HN4_OFFSET_INVALID) ? HN4_OFFSET_INVALID : (s_off / current_bs);

        for (int i = 0; i < 3; i++) {
            if (block_indices[i] == HN4_OFFSET_INVALID) continue;

            hn4_addr_t lba;
            if (_phys_lba_from_block(block_indices[i], current_bs, sector_sz, cap_bytes, &lba) != HN4_OK) continue;

            if (_read_sb_at_lba(dev, lba, sector_sz, current_bs, probe_buf, &cand) == HN4_OK) {
                
                if (cand.info.block_size != current_bs) continue; 

                /* --- CRITICAL: SPLIT BRAIN & TAMPER CHECK --- */
               if (found_valid) {
                    /* Check 1: UUID Mismatch (Alien Invasion) */
                    if (!hn4_uuid_equal(best_sb.info.volume_uuid, cand.info.volume_uuid)) {
                        if (best_sb.info.copy_generation == cand.info.copy_generation) {
                            HN4_LOG_CRIT("Tamper: Different UUIDs with same Generation");
                            final_res = HN4_ERR_TAMPERED;
                            found_valid = false;
                            goto cleanup;
                        }
                    } 
                    /* Check 2: Same UUID, Same Generation, Different Reality */
                    else if (best_sb.info.copy_generation == cand.info.copy_generation) {
                        /* 
                         * Strict Consistency Check.
                         * If Generations are equal, Block Size and State/Time logic should align.
                         * Allow small time jitter (<1s) for clock skew, but state divergence is fatal.
                         */
                        if (best_sb.info.block_size != cand.info.block_size) {
                            HN4_LOG_CRIT("Tamper: Same Gen, Different Block Size");
                            final_res = HN4_ERR_TAMPERED;
                            found_valid = false;
                            goto cleanup;
                        }
                        
                        /* Check for Clean/Dirty Split Brain */
                        bool best_clean = (best_sb.info.state_flags & HN4_VOL_CLEAN);
                        bool cand_clean = (cand.info.state_flags & HN4_VOL_CLEAN);
                        if (best_clean != cand_clean) {
                            HN4_LOG_CRIT("Split-Brain: Same Gen, Different State (Clean vs Dirty)");
                            final_res = HN4_ERR_TAMPERED;
                            found_valid = false;
                            goto cleanup;
                        }
                    }
                }

                bool newer = false;
                if (!found_valid) {
                    newer = true;
                } else if (cand.info.copy_generation > max_gen) {
                    if (cand.info.last_mount_time < (max_ts - HN4_REPLAY_WINDOW_NS)) {
                        /* Active Replay Protection */
                        HN4_LOG_CRIT("SECURITY: Replay Attack Detected! Candidate LBA is suspiciously old.");
                        continue; /* Reject this mirror */
                    } else {
                        newer = true;
                    }
                } else if (cand.info.copy_generation == max_gen) {
                
                    /* Check for Split-Brain Time Skew */
                    hn4_time_t diff = (cand.info.last_mount_time > max_ts) 
                                    ? (cand.info.last_mount_time - max_ts) 
                                    : (max_ts - cand.info.last_mount_time);
                      
                if (diff > HN4_REPLAY_WINDOW_NS) {
                    HN4_LOG_CRIT("Tamper: Same Gen, Time Divergence (%lld) > Window", (long long)diff);
                    final_res = HN4_ERR_TAMPERED;
                    found_valid = false;
                    goto cleanup;
                }

                if (cand.info.last_mount_time > max_ts) newer = true;
                
                }

                if (newer) {
                    memcpy(&best_sb, &cand, sizeof(cand));
                    max_gen = cand.info.copy_generation;
                    max_ts = cand.info.last_mount_time;
                    found_valid = true;
                }
            }
        }
    }

    /* 3. Healing Phase */
    if (found_valid) {
        memcpy(out_sb, &best_sb, sizeof(best_sb));
        final_res = HN4_OK;

        if (allow_repair) {
            uint32_t bs = best_sb.info.block_size;
            uint32_t io_sz = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
            
            if (io_sz > current_buf_sz) goto cleanup; 

            int heal_failures = 0;
            _secure_zero(heal_buf, io_sz);
            hn4_sb_to_disk(&best_sb, (hn4_superblock_t*)heal_buf);
            
            hn4_superblock_t* dsb = (hn4_superblock_t*)heal_buf;
            uint32_t crc = hn4_crc32(0, dsb, HN4_SB_SIZE - 4);
            dsb->raw.sb_crc = hn4_cpu_to_le32(crc);

            uint64_t targets[4];
            targets[0] = 0;
            targets[1] = HN4_ALIGN_UP((cap_bytes / 100) * 33, bs) / bs;
            targets[2] = HN4_ALIGN_UP((cap_bytes / 100) * 66, bs) / bs;
            uint64_t s_off = _calc_south_offset(cap_bytes, bs);
            targets[3] = (s_off == HN4_OFFSET_INVALID) ? HN4_OFFSET_INVALID : (s_off / bs);

            for (int i = 0; i < 4; i++) {
                if (i > 0 && targets[i] == HN4_OFFSET_INVALID) continue;

                hn4_addr_t lba;
                if (_phys_lba_from_block(targets[i], bs, sector_sz, cap_bytes, &lba) != HN4_OK) continue;

                bool needs_heal = false;
                hn4_superblock_t check;
                if (_read_sb_at_lba(dev, lba, sector_sz, bs, probe_buf, &check) != HN4_OK) {
                    needs_heal = true; 
                } else {
                    int64_t ts_diff = (int64_t)check.info.last_mount_time - (int64_t)best_sb.info.last_mount_time;
                    if (ts_diff < 0) ts_diff = -ts_diff;

                    if (check.info.copy_generation != best_sb.info.copy_generation) {
                        needs_heal = true;
                    } else if (ts_diff > (HN4_REPLAY_WINDOW_NS * 10)) {
                        needs_heal = true;
                    }
                }

                if (needs_heal) {
                    uint32_t secs = io_sz / sector_sz;
                    if (hn4_hal_sync_io(dev, HN4_IO_WRITE, lba, heal_buf, secs) != HN4_OK) {
                        heal_failures++;
                    } else {
                         hn4_hal_barrier(dev);
                    }
                }
            }
            if (heal_failures > 0) out_sb->info.state_flags |= HN4_VOL_DEGRADED;
        }
    }

cleanup:
    if (probe_buf) hn4_hal_mem_free(probe_buf);
    if (heal_buf) hn4_hal_mem_free(heal_buf);
    return final_res;
}



/* =========================================================================
 * 4. ATOMIC STATE TRANSITION (DIRTY MARKING)
 * ========================================================================= */

static hn4_result_t _mark_volume_dirty_and_sync(HN4_IN hn4_hal_device_t* dev, HN4_INOUT hn4_volume_t* vol)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t sector_sz = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint64_t cap = vol->vol_capacity_bytes;

    if (vol->taint_counter >= HN4_TAINT_THRESHOLD_RO) return HN4_ERR_MEDIA_TOXIC;

    hn4_superblock_t original_sb = vol->sb;
    hn4_superblock_t dirty_sb = vol->sb;
    uint32_t old_taint = vol->taint_counter; 

    if ((dirty_sb.info.state_flags & HN4_VOL_CLEAN) && (dirty_sb.info.state_flags & HN4_VOL_DIRTY)) {
        return HN4_ERR_INTERNAL_FAULT;
    }

    if (dirty_sb.info.copy_generation >= HN4_MAX_GENERATION) return HN4_ERR_EEXIST;

    dirty_sb.info.state_flags |= HN4_VOL_DIRTY;
    dirty_sb.info.state_flags &= ~HN4_VOL_CLEAN;
    dirty_sb.info.copy_generation++;
    dirty_sb.info.last_mount_time = hn4_hal_get_time_ns();

    if (vol->taint_counter > 0) dirty_sb.info.dirty_bits |= HN4_DIRTY_BIT_TAINT;

    uint32_t io_sz = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
    void* io_buf = hn4_hal_mem_alloc(io_sz);
    if (!io_buf) return HN4_ERR_NOMEM;

    _secure_zero(io_buf, io_sz);
    hn4_sb_to_disk(&dirty_sb, (hn4_superblock_t*)io_buf);

    hn4_superblock_t* disk_view = (hn4_superblock_t*)io_buf;
    uint32_t crc = hn4_crc32(0, disk_view, HN4_SB_SIZE - 4);
    disk_view->raw.sb_crc = hn4_cpu_to_le32(crc);

    uint64_t s_offset = _calc_south_offset(cap, bs);

    uint64_t target_blocks[] = {
        0,
        HN4_ALIGN_UP((cap / 100) * 33, bs) / bs,
        HN4_ALIGN_UP((cap / 100) * 66, bs) / bs,
        (s_offset == HN4_OFFSET_INVALID) ? HN4_OFFSET_INVALID : (s_offset / bs)
    };
    uint32_t sectors = io_sz / sector_sz;
    bool north_ok = false;
    int mirrors_ok = 0;

#ifdef HN4_USE_128BIT
    hn4_addr_t lba0 = {0,0};
#else
    hn4_addr_t lba0 = 0;
#endif

    /* 1. Write North */
    if (hn4_hal_sync_io(dev, HN4_IO_WRITE, lba0, io_buf, sectors) == HN4_OK) {
        if (hn4_hal_sync_io(dev, HN4_IO_FLUSH, lba0, NULL, 0) == HN4_OK) {
            north_ok = true;
        }
    }

    /* 2. Write Mirrors */
    for (int i=1; i<4; i++) {
        if (target_blocks[i] == HN4_OFFSET_INVALID) continue;

        /* 
         * Skip mirrors on ZNS to prevent "Write Pointer Violation" errors.
         * Only the Primary SB (North) allows in-place updates in Conventional Zones.
         */
        if (caps->hw_flags & HN4_HW_ZNS_NATIVE) continue;

        hn4_addr_t lba;
        if (_phys_lba_from_block(target_blocks[i], bs, sector_sz, cap, &lba) != HN4_OK) continue;

        if (hn4_hal_sync_io(dev, HN4_IO_WRITE, lba, io_buf, sectors) == HN4_OK) {
            if (hn4_hal_barrier(dev) == HN4_OK) {
                mirrors_ok++;
            }
        }
    }

    /* 
     * Correct Quorum Logic.
     * Spec: North + 1 Mirror OR 3 Mirrors (Southbridge Protocol)
     */
    bool quorum_met = (north_ok && mirrors_ok >= 1) || (mirrors_ok >= 3);

    if (!quorum_met) {
        HN4_LOG_CRIT("Dirty Quorum Failed (N:%d M:%d). Initiating NUCLEAR ROLLBACK.", north_ok, mirrors_ok);
        
        /* Serialize Original SB (Clean State) */
        _secure_zero(io_buf, io_sz);
        hn4_sb_to_disk(&original_sb, (hn4_superblock_t*)io_buf);
        disk_view = (hn4_superblock_t*)io_buf;
        crc = hn4_crc32(0, disk_view, HN4_SB_SIZE - 4);
        disk_view->raw.sb_crc = hn4_cpu_to_le32(crc);

        /* 
         * We must overwrite North AND all potential Mirrors to kill the "Dirty" generation.
         * We ignore errors here because we are already in the failure path.
         */
        
        /* 1. Rollback North */
        hn4_hal_sync_io(dev, HN4_IO_WRITE, lba0, io_buf, sectors);
        
        /* 2. Rollback Mirrors */
        for (int i=1; i<4; i++) {
            if (target_blocks[i] == HN4_OFFSET_INVALID) continue;
            hn4_addr_t lba;
            if (_phys_lba_from_block(target_blocks[i], bs, sector_sz, cap, &lba) == HN4_OK) {
                hn4_hal_sync_io(dev, HN4_IO_WRITE, lba, io_buf, sectors);
            }
        }
        
        hn4_hal_barrier(dev);

        /* Restore In-Memory State */
        vol->sb = original_sb;
        vol->taint_counter = old_taint;

        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    hn4_hal_mem_free(io_buf);

    /* Update RAM struct on success */
    if (vol->sb.info.state_flags & HN4_VOL_CLEAN) {
        if (vol->taint_counter > 0) vol->taint_counter /= 2;
    }

    vol->sb = dirty_sb;
    return HN4_OK;
}

/* =========================================================================
 * 5. RESOURCE LOADING
 * ========================================================================= */

static hn4_result_t _load_bitmap_resources(HN4_IN hn4_hal_device_t* dev, HN4_INOUT hn4_volume_t* vol)
{
    if (vol->sb.info.format_profile == HN4_PROFILE_PICO) return HN4_OK;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;
    uint32_t sect_sz = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint64_t cap = vol->vol_capacity_bytes;

    if (bs == 0 || sect_sz == 0 || bs % sect_sz != 0) return HN4_ERR_ALIGNMENT_FAIL;

    uint64_t cap_blocks = cap / bs;
    size_t armor_words = (cap_blocks + 63) / 64;
    
    vol->void_bitmap = hn4_hal_mem_alloc(armor_words * sizeof(hn4_armored_word_t));
    if (!vol->void_bitmap) return HN4_ERR_NOMEM;
    vol->bitmap_size = armor_words * sizeof(hn4_armored_word_t);

    uint32_t chunk_blocks = (2 * 1024 * 1024) / bs; 
    if (chunk_blocks == 0) chunk_blocks = 1;

    void* io_buf = hn4_hal_mem_alloc(chunk_blocks * bs);
    if (!io_buf) {
        hn4_hal_mem_free(vol->void_bitmap);
        vol->void_bitmap = NULL;
        return HN4_ERR_NOMEM;
    }

    uint64_t start_idx, end_idx;
    _addr_to_u64_checked(vol->sb.info.lba_bitmap_start, &start_idx);
    _addr_to_u64_checked(vol->sb.info.lba_qmask_start, &end_idx);

    uint64_t cur_idx = start_idx;
    size_t words_filled = 0;
    
    uint64_t needed_bytes = (cap_blocks + 7) / 8;
    uint64_t needed_blocks_disk = (needed_bytes + bs - 1) / bs;

    /* Bounds check */
    if ((start_idx + needed_blocks_disk) > end_idx) {
        hn4_hal_mem_free(io_buf);
        hn4_hal_mem_free(vol->void_bitmap);
        vol->void_bitmap = NULL;
        return HN4_ERR_BITMAP_CORRUPT;
    }

    uint64_t blocks_left = needed_blocks_disk;

    while (blocks_left > 0) {
        uint32_t io_n = (blocks_left > chunk_blocks) ? chunk_blocks : (uint32_t)blocks_left;
        
        hn4_addr_t io_lba;
        /* Use capacity check */
        if (_phys_lba_from_block(cur_idx, bs, sect_sz, cap, &io_lba) != HN4_OK) {
             /* Should be caught by bounds check above, but fail safe */
             break;
        }

        uint32_t io_sectors = (io_n * bs) / sect_sz;

        /* HAL Contract - we assume synchronous full read or error */
        if (hn4_hal_sync_io(dev, HN4_IO_READ, io_lba, io_buf, io_sectors) != HN4_OK) {
            hn4_hal_mem_free(io_buf);
            hn4_hal_mem_free(vol->void_bitmap);
            vol->void_bitmap = NULL;
            return HN4_ERR_HW_IO;
        }

        /* Decode */
        uint64_t* raw = (uint64_t*)io_buf;
        size_t u64_count = (io_n * bs) / 8;

        for (size_t i=0; i < u64_count && words_filled < armor_words; i++) {
            uint64_t val = hn4_le64_to_cpu(raw[i]);
            
            if (words_filled == armor_words - 1) {
                size_t bits_in_last = cap_blocks % 64;
                if (bits_in_last > 0) val &= (1ULL << bits_in_last) - 1;
            }

            vol->void_bitmap[words_filled].data = val;
            vol->void_bitmap[words_filled].ecc = _calc_ecc_hamming(val);
            words_filled++;
        }

        blocks_left -= io_n;
        cur_idx += io_n;
    }

    hn4_hal_mem_free(io_buf);
    return HN4_OK;
}

/* 
 * Layout Sanity Check
 * Ensures all internal pointers are within physical volume bounds.
 * Prevents arithmetic overflows later in the driver.
 */
static hn4_result_t _validate_sb_layout(const hn4_superblock_t* sb, const hn4_hal_caps_t* caps) {
    /* 
     * Use hn4_size_t (which adapts to u128/u64) for capacity 
     * to avoid truncation on Quettabyte drives.
     */
    hn4_size_t cap_bytes;
    hn4_size_t hw_cap;

#ifdef HN4_USE_128BIT
    cap_bytes = sb->info.total_capacity;
    hw_cap = caps->total_capacity_bytes;
    
    /* Check: Partition Cap > HW Cap? */
    if (hn4_u128_cmp(cap_bytes, hw_cap) > 0) {
        HN4_LOG_CRIT("Geometry Mismatch: Superblock expects capacity %s, HW reports %s", 
                     /* format helpers omitted */ "Larger", "Smaller");
        /* 
         * Explicitly deny mount on shrink. 
         * HN4 Geometry is immutable without a full migration (fsck).
         * Allowing this mount would cause massive data corruption due to Phi shift.
         */
        return HN4_ERR_GEOMETRY;
    }
    
    /* Check: Min Size (2MB) */
    if (hn4_u128_cmp(cap_bytes, hn4_u128_from_u64(2ULL * 1024 * 1024)) < 0) return HN4_ERR_GEOMETRY;
#else
    cap_bytes = sb->info.total_capacity;
    hw_cap = caps->total_capacity_bytes;
    
    if (cap_bytes > hw_cap) return HN4_ERR_GEOMETRY;
    if (cap_bytes < (2ULL * 1024 * 1024)) return HN4_ERR_GEOMETRY;
#endif

    uint32_t bs = sb->info.block_size;
    if (bs == 0) return HN4_ERR_GEOMETRY;

    /* 
     * The Superblock stores pointers as Sector LBAs (Sector Indices).
     * To convert to bytes, we must multiply by Sector Size (ss), NOT Block Size (bs).
     */
    uint32_t ss = caps->logical_block_size;
    if (ss == 0) ss = 512; /* Safety fallback */

    hn4_addr_t regions[] = {
        sb->info.lba_epoch_start,
        sb->info.lba_cortex_start,
        sb->info.lba_bitmap_start,
        sb->info.lba_qmask_start,
        sb->info.lba_flux_start,
        sb->info.lba_horizon_start
    };

    /* 
     * Perform bounds checking using native address types
     * instead of downcasting to uint64_t.
     */
    for (int i = 0; i < 6; i++) {
        /* Skip unused regions (0) */
#ifdef HN4_USE_128BIT
        if (regions[i].lo == 0 && regions[i].hi == 0) continue;
        
        /* Check: (LBA * SS) >= Capacity */
        /* 
         * Simplified Check for 128-bit without full math library:
         * If LBA >= Capacity, it is definitely out of bounds.
         * (Realistically LBA should be ~Capacity/512).
         */
        if (hn4_u128_cmp(regions[i], cap_bytes) >= 0) return HN4_ERR_GEOMETRY;
#else
        if (regions[i] == 0) continue;
        
        /* 64-bit check with overflow guard */
        
        /* Unit System Corruption
         * Previously: if ((regions[i] * bs) >= cap_bytes) 
         * This was multiplying Sector Index by Block Size (e.g. 4096), 
         * creating an offset 8x larger than reality and failing valid mounts.
         * 
         * Correct: regions[i] * ss (Sector Size)
         */
        if (regions[i] > (UINT64_MAX / ss)) return HN4_ERR_GEOMETRY;
        if ((regions[i] * ss) >= cap_bytes) return HN4_ERR_GEOMETRY;
#endif
    }

    return HN4_OK;
}


/**
 * _check_block_toxicity
 * 
 * Inspects the loaded Quality Mask to determine if a block is physically unsafe.
 * 
 * RISK 1: The allocator/writer must treat 0x00 as strong poison.
 * RISK 3: Endianness is handled by accessing the normalized uint64_t array.
 * 
 * @param vol       Volume context containing loaded Q-Mask
 * @param block_idx The logical block index to check
 * @return          HN4_OK if safe, HN4_ERR_MEDIA_TOXIC if 0x00.
 */
static hn4_result_t _check_block_toxicity(HN4_IN hn4_volume_t* vol, uint64_t block_idx) {
    /* If Q-Mask failed to load (e.g. RO mount), we assume media is suspicious but accessible.
       However, if we are in RW mode, Q-Mask MUST be present. */
    if (!vol->quality_mask) {
        return vol->read_only ? HN4_OK : HN4_ERR_UNINITIALIZED;
    }

    /* 2 bits per block -> 32 blocks per 64-bit word */
    uint64_t word_idx = block_idx / 32;
    uint32_t bit_shift = (block_idx % 32) * 2;

    /* Bounds check against Q-Mask allocation */
    if (word_idx >= (vol->qmask_size / sizeof(uint64_t))) return HN4_ERR_GEOMETRY;

    /* 
     * RISK 3: Endianness 
     * vol->quality_mask is already normalized to CPU-Native uint64_t 
     * by _load_qmask_resources using hn4_bulk_le64_to_cpu.
     * We can safely perform bitwise ops.
     */
    uint64_t word = vol->quality_mask[word_idx];
    uint8_t q_val = (word >> bit_shift) & 0x3;

    /* RISK 1: 0x00 is Strong Poison */
    if (q_val == HN4_Q_TOXIC) {
        HN4_LOG_CRIT("Access Denied to Toxic Block %llu (Q-Mask=00)", block_idx);
        return HN4_ERR_MEDIA_TOXIC;
    }

    return HN4_OK;
}

static hn4_result_t _load_qmask_resources(HN4_IN hn4_hal_device_t* dev, HN4_INOUT hn4_volume_t* vol)
{
    /* Q-Mask is optional for Pico profile */
    if (vol->sb.info.format_profile == HN4_PROFILE_PICO) return HN4_OK;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t sect_sz = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    uint64_t cap = vol->vol_capacity_bytes;

    /* 1. Calculate Size: 2 bits per block */
    uint64_t total_data_blocks = cap / bs;
    uint64_t qmask_bytes_needed = (total_data_blocks * 2 + 7) / 8;
    
    /* Align allocation to 8 bytes for 64-bit word access */
    size_t alloc_sz = HN4_ALIGN_UP(qmask_bytes_needed, 8);
    
    vol->quality_mask = hn4_hal_mem_alloc(alloc_sz);
    if (!vol->quality_mask) return HN4_ERR_NOMEM;
    vol->qmask_size = alloc_sz;
    
    /* Initialize to SILVER (0xAA) - Default Safe State */
    memset(vol->quality_mask, 0xAA, alloc_sz);

    /* 2. Calculate Disk Extents */
    uint64_t start_blk, end_blk;
    _addr_to_u64_checked(vol->sb.info.lba_qmask_start, &start_blk);
    _addr_to_u64_checked(vol->sb.info.lba_flux_start, &end_blk);

    uint64_t qmask_blocks_disk = (qmask_bytes_needed + bs - 1) / bs;

    /* Safety Check: Overlap */
    if (start_blk + qmask_blocks_disk > end_blk) {
        hn4_hal_mem_free(vol->quality_mask);
        vol->quality_mask = NULL;
        return HN4_ERR_GEOMETRY;
    }

    /* 3. IO Buffer Setup */
    uint32_t chunk_len = (2 * 1024 * 1024) / bs; /* 2MB chunks */
    if (chunk_len == 0) chunk_len = 1;
    
    void* io_buf = hn4_hal_mem_alloc(chunk_len * bs);
    if (!io_buf) {
        hn4_hal_mem_free(vol->quality_mask);
        vol->quality_mask = NULL;
        return HN4_ERR_NOMEM;
    }

    /* 4. Read Loop */
    uint64_t cur_blk = start_blk;
    uint64_t blocks_left = qmask_blocks_disk;
    size_t mem_offset = 0;

    while (blocks_left > 0) {
        uint32_t io_n = (blocks_left > chunk_len) ? chunk_len : (uint32_t)blocks_left;
        
        hn4_addr_t io_lba;
        if (_phys_lba_from_block(cur_blk, bs, sect_sz, cap, &io_lba) == HN4_OK) {
            
            uint32_t io_sectors = (io_n * bs) / sect_sz;

            /* 
             * Advance by what we actually copied/planned to copy.
             * Compute step first, clamped to allocation bounds.
             */
            size_t bytes_step = io_n * bs;
            if (mem_offset + bytes_step > alloc_sz) {
                bytes_step = alloc_sz - mem_offset;
            }

            /* Tolerant Read: If fails, we keep 0xAA (Silver) pattern for this chunk */
            if (hn4_hal_sync_io(dev, HN4_IO_READ, io_lba, io_buf, io_sectors) == HN4_OK) {
                memcpy((uint8_t*)vol->quality_mask + mem_offset, io_buf, bytes_step);
            } else {
                HN4_LOG_WARN("Q-Mask Partial Read Fail @ Blk %llu. Defaulting to Silver.", cur_blk);
            }
            
            mem_offset += bytes_step;
        }

        blocks_left -= io_n;
        cur_blk += io_n;
    }

    hn4_hal_mem_free(io_buf);
    hn4_bulk_le64_to_cpu(vol->quality_mask, alloc_sz / 8);

    return HN4_OK;
}

static hn4_result_t _load_topology_resources(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_INOUT hn4_volume_t* vol
) {
    /* 
     * Ensure the internal helper struct matches the volume struct layout.
     * Prevents silent corruption if one definition drifts.
     */
    _Static_assert(sizeof(_hn4_topo_entry_t) == sizeof(*vol->topo_map), 
                   "HN4: Topology struct layout mismatch");

    if (vol->sb.info.format_profile != HN4_PROFILE_AI) return HN4_OK;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;

    if (ss == 0 || bs < ss || (bs % ss) != 0) {
        HN4_LOG_CRIT("AI Topo: Invalid Geometry (BS %u < SS %u or Misaligned)", bs, ss);
        return HN4_ERR_GEOMETRY;
    }
    
    uint32_t spb = bs / ss;
    
    /* 
     * Usable Bounds Calculation
     * We must not let AI regions overlap reserved Metadata (Epoch, Cortex, Bitmaps).
     * Valid Data Region = [Flux Start ... Capacity]
     * Note: We allow overlapping Horizon/Stream tail for flexibility, 
     * as allocator collision logic handles D1.5 interaction.
     */
    uint64_t usable_start_sector;

    if (!_addr_to_u64_checked(vol->sb.info.lba_flux_start, &usable_start_sector)) 
    usable_start_sector = UINT64_MAX;

    uint64_t usable_end_sector;

    if (!_addr_to_u64_checked(caps->total_capacity_bytes, &usable_end_sector)) {
        usable_end_sector = UINT64_MAX; 
    } else {
        usable_end_sector /= ss; // Correct: Bytes -> Sectors
    }

    uint32_t count = hn4_hal_get_topology_count(dev);
    if (count == 0) return HN4_OK; 

    if (count > HN4_MAX_TOPOLOGY_REGIONS) {
        HN4_LOG_WARN("AI Topo: Region count %u > Limit. Disabled.", count);
        return HN4_OK; 
    }

    size_t map_size = count * sizeof(_hn4_topo_entry_t);
    vol->topo_map = hn4_hal_mem_alloc(map_size);
    if (!vol->topo_map) return HN4_ERR_NOMEM;

    hn4_result_t res = hn4_hal_get_topology_data(dev, vol->topo_map, map_size);
    if (res != HN4_OK) {
        hn4_hal_mem_free(vol->topo_map);
        vol->topo_map = NULL;
        vol->topo_count = 0; 
        return res;
    }

    /* Sort for O(N) overlap checking */
    qsort(vol->topo_map, count, sizeof(_hn4_topo_entry_t), _topo_cmp);

    _hn4_topo_entry_t* entries = (_hn4_topo_entry_t*)vol->topo_map;
    uint64_t watermark_end = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t start = entries[i].lba_start;
        uint64_t len   = entries[i].lba_len;
        
        /* 1. Alignment & Size */
        if ((start % spb != 0) || (len % spb != 0) || (len < spb)) {
            HN4_LOG_WARN("AI Topo: Region %u invalid align/size", i);
            goto Fail;
        }

        /* 
         * 2. Bounds (Reserved Area Protection)
         * Must start AFTER Metadata (Flux Start) and end BEFORE Capacity.
         */
        if (start < usable_start_sector) {
            HN4_LOG_WARN("AI Topo: Region %u overlaps Metadata (Start %llu < Flux %llu)", 
                         i, start, usable_start_sector);
            goto Fail;
        }

        if ((start + len) < start || (start + len) > usable_end_sector) {
            HN4_LOG_WARN("AI Topo: Region %u exceeds Capacity", i);
            goto Fail;
        }

        /* Weight Sanity */
        if (entries[i].affinity_weight > 255) {
             entries[i].affinity_weight = 255; /* Clamp to byte range */
        }

        /* 3. Overlap Check */
        if (i > 0 && start < watermark_end) {
            HN4_LOG_WARN("AI Topo: Region %u overlaps previous", i);
            goto Fail;
        }

        watermark_end = start + len;
    }
    
    vol->topo_count = count;
    return HN4_OK;

Fail:
    if (vol->topo_map) {
        hn4_hal_mem_free(vol->topo_map);
        vol->topo_map = NULL;
    }

    /* Reset Count */
    vol->topo_count = 0;
    
    return HN4_OK;
}



/* =========================================================================
 * ROOT ANCHOR VERIFICATION & HEALING (NEW LOGIC)
 * ========================================================================= */

/**
 * _verify_and_heal_root_anchor
 * 
 * Inspects the first block of the Cortex (D0) region.
 * 1. Validates the Root Anchor exists (ID: 0xFF...FF).
 * 2. Validates Integrity (CRC).
 * 3. Attempts REPAIR if corrupt (RW mode only).
 * 4. Fails if corrupt and RO mode.
 */
static hn4_result_t _verify_and_heal_root_anchor(
    HN4_IN hn4_hal_device_t* dev, 
    HN4_INOUT hn4_volume_t* vol,
    HN4_IN bool is_user_ro
)
{
    /* 1. Setup Geometry */
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t ss = caps->logical_block_size;
    uint32_t bs = vol->vol_block_size;
    if (ss == 0 || bs == 0) return HN4_ERR_GEOMETRY;

    hn4_addr_t cortex_lba = vol->sb.info.lba_cortex_start;

    /* 2. Read Root Anchor Candidate */

    /* 
     * In ZNS mode, bs is ~1.7GB. We only need the first sector to check the Root Anchor.
     */
    uint32_t alloc_sz = (bs > 65536) ? 65536 : bs;
    // Ensure we allocate at least one sector
    if (alloc_sz < ss) alloc_sz = ss;

    void* io_buf = hn4_hal_mem_alloc(alloc_sz);
    if (!io_buf) return HN4_ERR_NOMEM;

    uint32_t sector_count = alloc_sz / ss;
    if (sector_count == 0) sector_count = 1;

    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_READ, cortex_lba, io_buf, sector_count);
    if (res != HN4_OK) {
        hn4_hal_mem_free(io_buf);
        return res;
    }

    hn4_anchor_t* root = (hn4_anchor_t*)io_buf;
    
    /* 3. Validation Logic */
    
    /* Step A: Check CRC first (Integrity) */
    uint32_t stored_crc = hn4_le32_to_cpu(root->checksum);
    uint32_t saved_val = root->checksum; /* Save original */
    
    root->checksum = 0;
    
    /* 
     * CRC covers strictly 0 to offsetof(checksum).
     * DO NOT include inline_buffer in verification or calculation.
     * This matches the system-wide Anchor contract.
     */
    uint32_t calc_crc = hn4_crc32(0, root, offsetof(hn4_anchor_t, checksum));
    
    /* Restore for memory consistency */
    root->checksum = saved_val; 
    
    bool crc_ok = (calc_crc == stored_crc);
    
    if (crc_ok) {
        /* Step B: Check Semantics (ID, Flags) */
        bool semantics_ok = true;
        
        if (root->seed_id.lo != 0xFFFFFFFFFFFFFFFFULL || root->seed_id.hi != 0xFFFFFFFFFFFFFFFFULL) {
            semantics_ok = false;
        }
        
        uint64_t dclass = hn4_le64_to_cpu(root->data_class);
        /* Check for VALID flag AND STATIC class */
        if (!(dclass & HN4_FLAG_VALID) || ((dclass & HN4_CLASS_VOL_MASK) != HN4_VOL_STATIC)) {
            semantics_ok = false;
        }
        
        if (!semantics_ok) {
            /* 
             * Integrity OK, but Semantics Bad -> Intentional Tombstone or Config Mismatch.
             * DO NOT HEAL.
             */
            HN4_LOG_CRIT("Root Anchor Semantically Invalid (CRC OK). Mount Denied.");
            hn4_hal_mem_free(io_buf);
            return HN4_ERR_NOT_FOUND;
        }
        
        /* Valid */
        hn4_hal_mem_free(io_buf);
        return HN4_OK;
    }
    
    /* Step C: CRC Failed -> Heal (if RW) */
    if (is_user_ro) {
        HN4_LOG_CRIT("Root Anchor Missing/Corrupt in RO Mode. Refusing Mount.");
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_NOT_FOUND; 
    }

    HN4_LOG_WARN("Healing Root Anchor (Genesis Repair)...");

    memset(io_buf, 0, alloc_sz);
    root = (hn4_anchor_t*)io_buf;

    root->seed_id.lo = 0xFFFFFFFFFFFFFFFFULL;
    root->seed_id.hi = 0xFFFFFFFFFFFFFFFFULL;
    root->public_id  = root->seed_id;
    root->orbit_vector[0] = 1; /* Sequential */

    /* Ensure we write STATIC | VALID */
    uint64_t new_dclass = HN4_VOL_STATIC | HN4_FLAG_VALID;
    root->data_class = hn4_cpu_to_le64(new_dclass);

    uint32_t perms = HN4_PERM_READ | HN4_PERM_WRITE | HN4_PERM_EXEC | 
                     HN4_PERM_IMMUTABLE | HN4_PERM_SOVEREIGN;
    root->permissions = hn4_cpu_to_le32(perms);

    hn4_time_t now = hn4_hal_get_time_ns();
    root->mod_clock = hn4_cpu_to_le64(now);
    root->create_clock = hn4_cpu_to_le32((uint32_t)(now / 1000000000ULL));
    
    strncpy((char*)root->inline_buffer, "ROOT", sizeof(root->inline_buffer)-1);

    /* Recalculate CRC (Strict Mode: No Inline Buffer) */
    root->checksum = 0;
    uint32_t crc = hn4_crc32(0, root, offsetof(hn4_anchor_t, checksum));
    root->checksum = hn4_cpu_to_le32(crc);

    /* Commit Repair */
    res = hn4_hal_sync_io(dev, HN4_IO_WRITE, cortex_lba, io_buf, sector_count);
    
    if (res == HN4_OK) {
        hn4_hal_barrier(dev);

        /* Read back to verify media accepted it */
        void* verify_buf = hn4_hal_mem_alloc(bs);
        if (verify_buf) {
            if (hn4_hal_sync_io(dev, HN4_IO_READ, cortex_lba, verify_buf, sector_count) == HN4_OK) {
                if (memcmp(io_buf, verify_buf, alloc_sz) != 0) {
                    HN4_LOG_CRIT("Root Anchor Repair Failed: Verification Mismatch");
                    res = HN4_ERR_HW_IO;
                } else {
                     vol->sb.info.state_flags |= HN4_VOL_DEGRADED; 
                }
            } else {
                res = HN4_ERR_HW_IO;
            }
            hn4_hal_mem_free(verify_buf);
        } else {
             res = HN4_ERR_NOMEM;
        }
    }

    hn4_hal_mem_free(io_buf);
    return res;
}

/* =========================================================================
 * 7. ZERO-SCAN RECONSTRUCTION (L10 RECOVERY)
 * ========================================================================= */


typedef enum { 
    BIT_SET, 
    BIT_CLEAR, 
    BIT_TEST, 
    BIT_FORCE_CLEAR /* Non-Panic Rollback */ 
} hn4_bit_op_t;

/**
 * _reconstruct_cortex_state
 * 
 * Implements the "Zero-Scan" recovery strategy.
 * 1. Loads the entire Cortex (D0) into the Nano-Cortex cache.
 * 2. Re-projects the Ballistic Trajectory of every valid Anchor.
 * 3. Cross-verifies against the Allocation Bitmap (The Ghost Check).
 * 
 * Rationale:
 * Since V is coprime to the window, the file layout is deterministic.
 * We don't need to scan disk blocks to find files; we just recalculate
 * where they MUST be.
 */
static hn4_result_t _reconstruct_cortex_state(
    HN4_IN hn4_hal_device_t* dev,
    HN4_INOUT hn4_volume_t* vol
)
{
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t bs = vol->vol_block_size;
    uint32_t ss = caps->logical_block_size;
    
    /* 1. Determine Cortex Geometry */
    uint64_t start_blk, end_blk;
    if (!_addr_to_u64_checked(vol->sb.info.lba_cortex_start, &start_blk)) return HN4_ERR_GEOMETRY;
    if (!_addr_to_u64_checked(vol->sb.info.lba_bitmap_start, &end_blk)) return HN4_ERR_GEOMETRY;
    
    uint64_t cortex_sectors = end_blk - start_blk;
    uint64_t cortex_bytes = cortex_sectors * ss;

    /* Safety: Cap Nano-Cortex to 256MB during mount to prevent OOM DOS */
    if (cortex_bytes > (256 * 1024 * 1024)) {
        HN4_LOG_WARN("Cortex too large for RAM cache (%llu bytes). Disabling Zero-Scan.", 
                     (unsigned long long)cortex_bytes);
        /* Proceed without cache - Degraded but functional */
        return HN4_OK; 
    }

    vol->nano_cortex = hn4_hal_mem_alloc(cortex_bytes);
    if (!vol->nano_cortex) return HN4_ERR_NOMEM;
    vol->cortex_size = cortex_bytes;

    /* 3. Linear Read (The fastest op an SSD can perform) */
    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_READ, vol->sb.info.lba_cortex_start, vol->nano_cortex, cortex_sectors);
    if (res != HN4_OK) {
        hn4_hal_mem_free(vol->nano_cortex);
        vol->nano_cortex = NULL;
        return res;
    }

    /* 4. Sequence Verification & Trajectory Re-Projection */
    uint32_t anchor_count = cortex_bytes / sizeof(hn4_anchor_t);
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    uint64_t ghost_repairs = 0;

    for (uint32_t i = 0; i < anchor_count; i++) {
        hn4_anchor_t* anchor = &anchors[i];
        
        /* A. Check Validity (Skip Tombstones/Empty slots) */
        uint64_t dclass = hn4_le64_to_cpu(anchor->data_class);
        if (!(dclass & HN4_FLAG_VALID)) continue;
        if (dclass & HN4_FLAG_TOMBSTONE) continue;

        /* B. Get Ballistic Parameters */
        uint64_t G = hn4_le64_to_cpu(anchor->gravity_center);
        uint64_t mass = hn4_le64_to_cpu(anchor->mass);
        
        /* Extract V (Orbit Vector) */
        uint64_t V = 0;
        memcpy(&V, anchor->orbit_vector, 6);
        V = hn4_le64_to_cpu(V) & 0xFFFFFFFFFFFFULL;
        
        uint16_t M = hn4_le16_to_cpu(anchor->fractal_scale);
        
        /* Calculate Block Count needed for this Mass */
        /* Note: Payload size depends on BS. Assume standard header overhead. */
        uint32_t payload_sz = bs - sizeof(hn4_block_header_t); 
        uint64_t blocks_needed = (mass + payload_sz - 1) / payload_sz;

       /* C. Re-Project Trajectory (Deep-Scan Recovery) */
        
        /* Pre-allocate a scratch buffer for verification reads */
        void* verify_buf = hn4_hal_mem_alloc(bs);
        if (!verify_buf) { /* Handle OOM during recovery */ break; }

        for (uint64_t n = 0; n < blocks_needed; n++) {
            
            bool found_block_n = false;

            /* Scan orbits 0..12 */
            for (uint8_t k = 0; k < HN4_MAX_TRAJECTORY_K; k++) {
                
                uint64_t lba = _calc_trajectory_lba(vol, G, V, n, M, k);
                if (lba == HN4_LBA_INVALID) continue;

                /* D. The Ghost Check */
                bool is_set = false;
                _bitmap_op(vol, lba, BIT_TEST, &is_set);
                
                /* Case 1: Bitmap says used. We assume it's ours or valid collision. */

                if (is_set) {
                    /* Optimization: If k=0 is set, we assume it's ours to save IO. */
                    if (k == 0) { found_block_n = true; break; }
                    hn4_addr_t phys = hn4_lba_from_blocks(lba * (bs / ss));

                    if (hn4_hal_sync_io(dev, HN4_IO_READ, phys, verify_buf, (bs/ss)) == HN4_OK) {
                        hn4_block_header_t* h = (hn4_block_header_t*)verify_buf;
        
                        /* Check Magic + Sequence */
                        if (hn4_le32_to_cpu(h->magic) == HN4_BLOCK_MAGIC &&
                        hn4_le64_to_cpu(h->seq_index) == n) 
                        {
                            /* Complete UUID Verification */
                            hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                            if (disk_id.lo == anchor->seed_id.lo && 
                                disk_id.hi == anchor->seed_id.hi) 
                                {
                                    /* It is OUR data. Mark found and stop probing. */
                                    found_block_n = true;
                                    break; 
                                }
        
                            }
                        }
    
                    /* If IO failed or ID didn't match, it is a collision. Continue probing k+1. */
                    continue; 
                }

                /* Case 2: Bitmap says FREE. This might be a Ghost. Verify Identity. */
                if (!is_set) {
                    hn4_addr_t phys = hn4_lba_from_blocks(lba * (bs / ss));
                    
                    if (hn4_hal_sync_io(dev, HN4_IO_READ, phys, verify_buf, (bs/ss)) == HN4_OK) {
                        hn4_block_header_t* h = (hn4_block_header_t*)verify_buf;
                        
                        /* Verify Magic + Well ID + Sequence ID */
                        if (hn4_le32_to_cpu(h->magic) == HN4_BLOCK_MAGIC &&
                            hn4_le64_to_cpu(h->seq_index) == n) 
                        {
                            hn4_u128_t disk_id = hn4_le128_to_cpu(h->well_id);
                            if (disk_id.lo == anchor->seed_id.lo && 
                                disk_id.hi == anchor->seed_id.hi) 
                            {
                                /* CONFIRMED GHOST: Data exists, Bitmap was lost. */
                                _bitmap_op(vol, lba, BIT_SET, NULL); /* Revive */
                                ghost_repairs++;
                                found_block_n = true;
                                break; /* Found Block N, move to N+1 */
                            }
                        }
                    }
                }
            }
        }
        hn4_hal_mem_free(verify_buf);
    }

    if (ghost_repairs > 0) {
        HN4_LOG_WARN("Zero-Scan Reconstruction: Healed %llu Ghost Allocations.", ghost_repairs);
        /* Mark volume as having been repaired (Taint logic) */
        vol->taint_counter++;
    } else {
        HN4_LOG_VAL("Zero-Scan Complete. State Consistent", anchor_count);
    }

    return HN4_OK;
}

/* =========================================================================
 * 6. MAIN MOUNT ENTRY POINT
 * ========================================================================= */


HN4_SUCCESS(return == HN4_OK)
hn4_result_t hn4_mount(
    HN4_IN hn4_hal_device_t* dev,
    HN4_IN const hn4_mount_params_t* params,
    HN4_OUT hn4_volume_t** out_vol
)
{
    hn4_result_t res = HN4_OK;
    hn4_volume_t* vol = NULL;
    bool force_ro = false;

    if (!dev || !out_vol) return HN4_ERR_INVALID_ARGUMENT;

    /* Spec 10.5: Thermal Awareness */
    uint32_t temp_c = hn4_hal_get_temperature(dev);
    if (temp_c > 85) {
        HN4_LOG_CRIT("Thermal Critical (%u C). Mount Denied.", temp_c);
        return HN4_ERR_THERMAL_CRITICAL;
    } else if (temp_c > 75) {
        HN4_LOG_WARN("High Temperature (%u C). Forcing Read-Only.", temp_c);
        force_ro = true;
    }

    vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    if (!vol) return HN4_ERR_NOMEM;
    memset(vol, 0, sizeof(hn4_volume_t));
    vol->target_device = dev;

    /* Initialize System L2 Lock */
    hn4_hal_spinlock_init(&vol->l2_lock);

    if (params && (params->mount_flags & HN4_MNT_READ_ONLY)) force_ro = true;

    /* --- PHASE 1: CARDINAL VOTE --- */
    res = _execute_cardinal_vote(dev, !force_ro, &vol->sb);
    if (res != HN4_OK) goto cleanup;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    res = _validate_sb_layout(&vol->sb, caps);
    if (res != HN4_OK) {
        HN4_LOG_CRIT("Mount Rejected: Invalid Geometry/Layout in Superblock");
        goto cleanup;
    }

    vol->vol_block_size = vol->sb.info.block_size;
    if (!_addr_to_u64_checked(vol->sb.info.total_capacity, &vol->vol_capacity_bytes)) {
        res = HN4_ERR_GEOMETRY;
        goto cleanup;
    }

    /* 
     * Epoch Check (Phase 3) moved BEFORE State Analysis (Phase 2).
     * We must know if the journal is skewed before we trust the state flags
     * or attempt to mark the volume dirty.
     */

    /* --- PHASE 3: EPOCH VALIDATION --- */
     uint64_t ring_idx;
#ifdef HN4_USE_128BIT
    ring_idx = vol->sb.info.epoch_ring_block_idx.lo;
#else
    ring_idx = vol->sb.info.epoch_ring_block_idx;
#endif
    
    uint64_t total_blocks = (vol->vol_capacity_bytes + vol->vol_block_size - 1) / vol->vol_block_size;

    if (ring_idx >= total_blocks) {
        HN4_LOG_CRIT("Epoch Ring Pointer Out of Bounds (Idx %llu >= Max %llu)", ring_idx, total_blocks);
        res = HN4_ERR_DATA_ROT;
        goto cleanup;
    }

    res = hn4_epoch_check_ring(dev, &vol->sb, vol->vol_capacity_bytes);
    
    switch (res) {
        case HN4_OK: 
            break;
        case HN4_ERR_GENERATION_SKEW:
            HN4_LOG_WARN("Epoch Journal Lag. Forcing RO to prevent Log Ordering violation.");
            force_ro = true;
            res = HN4_OK; 
            break; 
        case HN4_ERR_TIME_DILATION: 
            HN4_LOG_WARN("Time Dilation (Mirror Lag). Forcing RO.");
            force_ro = true;
            vol->taint_counter += 10;
            res = HN4_OK; 
            break;
        case HN4_ERR_EPOCH_LOST:
            HN4_LOG_CRIT("SECURITY: Epoch Ring Lost. Temporal ordering undefined.");
            HN4_LOG_CRIT("Forcing READ-ONLY Quarantine to prevent write phantom/replay.");
            
            /* 1. Mark State as Unsafe */
            vol->sb.info.state_flags |= HN4_VOL_PANIC;
            
            /* 2. FORCE Read-Only (Non-negotiable) */
            force_ro = true;
            
            /* 3. Return OK to allow data extraction */
            res = HN4_OK;
            break;
        default: 
            goto cleanup; /* Fatal */
    }

    /* --- PHASE 3.1: CHRONICLE INTEGRITY CHECK --- */
    /*
     * Verify that the Immutable Audit Log (Chronicle) has not been tampered with.
     * We walk the hash chain on disk. If the crypto-linkage is broken,
     * the volume is untrusted.
     */
    if (!force_ro) {
        const hn4_hal_caps_t* c = hn4_hal_get_caps(dev);
        uint32_t ss = c ? c->logical_block_size : 512;

        uint64_t j_head  = hn4_addr_to_u64(vol->sb.info.journal_ptr);
        uint64_t j_start = hn4_addr_to_u64(vol->sb.info.journal_start);
        uint64_t south_offset_bytes = _calc_south_offset(vol->vol_capacity_bytes, vol->vol_block_size);

        uint64_t j_end;
        
        if (south_offset_bytes != HN4_OFFSET_INVALID) {
            j_end = south_offset_bytes / ss;
        } else {
            j_end = vol->vol_capacity_bytes / ss;
        }

        if (j_head < j_start || j_head >= j_end) {
            HN4_LOG_CRIT("Chronicle Pointer Corrupt: %llu (Valid: %llu-%llu)", j_head, j_start, j_end);
            force_ro = true;
            vol->sb.info.state_flags |= HN4_VOL_PANIC;
        }

        if (!force_ro && j_head != j_start) {
            /* Log is empty. Ensure sequence is reset if SB thinks otherwise. */
            if (vol->sb.info.last_journal_seq != 0) {
                 vol->sb.info.last_journal_seq = 0;
            }
        }

        /* Proceed with Deep Verification */
        if (j_head > j_start) {
            hn4_result_t audit_res = hn4_chronicle_verify_integrity(dev, vol);
            
            if (audit_res != HN4_OK) {
                HN4_LOG_CRIT("SECURITY ALERT: Chronicle Integrity Check Failed (%d).", audit_res);
                HN4_LOG_WARN("Volume Audit Log is broken or tampered. Forcing Read-Only Quarantine.");
                
                force_ro = true;
                
                /* Maximize taint to prevent any accidental write attempts */
                vol->taint_counter = HN4_TAINT_THRESHOLD_RO + 1;
                
                /* We permit the mount for forensics, but mark it compromised */
                vol->sb.info.state_flags |= HN4_VOL_PANIC;
            }
        }
    }

    /* --- PHASE 2: STATE ANALYSIS --- */
    uint32_t st = vol->sb.info.state_flags;
    
    switch (st & (HN4_VOL_PANIC | HN4_VOL_TOXIC | HN4_VOL_LOCKED | HN4_VOL_PENDING_WIPE)) {
        case 0: break; /* OK */
        case HN4_VOL_PENDING_WIPE:
            HN4_LOG_CRIT("Mount Denied: Volume marked for Secure Wipe.");
            res = HN4_ERR_WIPE_PENDING;
            goto cleanup;
        case HN4_VOL_LOCKED: 
        case (HN4_VOL_LOCKED | HN4_VOL_PENDING_WIPE):
            /* LOCKED always wins */
            res = HN4_ERR_VOLUME_LOCKED; 
            goto cleanup;
        default: 
            HN4_LOG_WARN("Volume Flagged Panic/Toxic. Forcing RO.");
            force_ro = true;
    }

   /* ----- HARD FAIL CHECKS (not part of state_flags) ----- */

    if ((vol->sb.info.incompat_flags & ~HN4_SUPPORTED_INCOMPAT_MASK) != 0) {
        HN4_LOG_CRIT("Mount Denied: Unsupported Incompatible Features (0x%llx)",
                 (unsigned long long)(vol->sb.info.incompat_flags & ~HN4_SUPPORTED_INCOMPAT_MASK));
                 res = HN4_ERR_VERSION_INCOMPAT;
                 goto cleanup;
                }

    if (!(st & HN4_VOL_METADATA_ZEROED)) {
        HN4_LOG_CRIT("Mount Denied: Metadata not certified zeroed.");
        res = HN4_ERR_UNINITIALIZED;
        goto cleanup;
    }

    /* ----- FLAG POLICY SWITCH ----- */

    switch (st & (HN4_VOL_CLEAN | HN4_VOL_DIRTY))
    {
        case 0:
            /* Neither clean nor dirty? Weird but legal on fresh format */
            break;
        case HN4_VOL_CLEAN:
            /* ok */
            break;
        case HN4_VOL_DIRTY:
            /* ok — normal mounted volume */
            break;
        case (HN4_VOL_CLEAN | HN4_VOL_DIRTY):
            HN4_LOG_ERR("Invalid Flags (Clean+Dirty). Forcing RO+Taint.");
            force_ro = true;
            vol->taint_counter++;
            break;
    }

    /* ----- TAINT → RO ESCALATION ----- */

    if (vol->taint_counter >= HN4_TAINT_THRESHOLD_RO) {
        HN4_LOG_WARN("Taint Threshold Exceeded (%u). Forcing RO.",
                     vol->taint_counter);
        force_ro = true;
    }

    /* ----- RO-COMPAT FEATURES → FORCE RO ONLY ----- */

    if (vol->sb.info.ro_compat_flags != 0) {
        HN4_LOG_WARN("Detected unknown RO-Compat features (0x%llx). "
                     "Forcing Read-Only.",
                     (unsigned long long)vol->sb.info.ro_compat_flags);
        force_ro = true;
    }

    /* --- PHASE 4: PERSISTENCE (DIRTY BIT) --- */

    if (!force_ro) {
        if (st & HN4_VOL_CLEAN) {
            vol->taint_counter /= 2;
        }

        res = _mark_volume_dirty_and_sync(dev, vol);
        if (res != HN4_OK) {
            HN4_LOG_ERR("Dirty Sync Failed. Fallback RO.");
            force_ro = true;
        }
    }

    /* --- PHASE 5: RESOURCE LOADING --- */
   res = _load_bitmap_resources(dev, vol);
    if (res != HN4_OK) {
        if (!force_ro) {
            HN4_LOG_CRIT("Bitmap Load Failed in RW. Abort.");
            goto cleanup;
        } else {
            HN4_LOG_WARN("Bitmap Load Failed in RO. Continuing degraded.");
            if (vol->void_bitmap) {
                hn4_hal_mem_free(vol->void_bitmap);
                vol->void_bitmap = NULL;
            }
        }
    }

    res = _load_qmask_resources(dev, vol);
    if (res != HN4_OK) {
        if (!force_ro) {
            HN4_LOG_CRIT("Q-Mask Load Failed in RW. Abort.");
            goto cleanup;
        } else {
            HN4_LOG_WARN("Q-Mask Load Failed in RO. Continuing.");
            if (vol->quality_mask) {
                hn4_hal_mem_free(vol->quality_mask);
                vol->quality_mask = NULL;
            }
            res = HN4_OK; 
        }
    }

    /* Load and Validate AI Topology Map (Path-Aware Striping) */
    res = _load_topology_resources(dev, vol);
    /* 
     * Note: Topology load failure is handled internally by disabling the 
     * optimization (returns OK), so we don't abort the mount.
     */
    (void)res; 

     /* 
     * PHASE 6: L10 RECOVERY (ZERO-SCAN RECONSTRUCTION)
     * Rebuild allocation truth from the Cortex Anchors.
     */
    res = _reconstruct_cortex_state(dev, vol);
    if (res != HN4_OK) {
        if (!force_ro) {
            HN4_LOG_CRIT("Cortex Reconstruction Failed in RW mode. Aborting.");
            goto cleanup;
        } else {
            HN4_LOG_WARN("Cortex Reconstruction Failed in RO mode. Continuing raw.");
        }
    }

    res = _verify_and_heal_root_anchor(dev, vol, force_ro);
    if (res != HN4_OK) {
        if (!force_ro) {
            HN4_LOG_CRIT("Root Anchor invalid in RW mode. Aborting mount.");
            goto cleanup;
        } else {
            HN4_LOG_WARN("Root Anchor invalid in RO mode. Continuing degraded.");
        }
    }

    vol->read_only = force_ro;
    *out_vol = vol;
    return HN4_OK;

cleanup:
    if (vol) {
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
        hn4_hal_mem_free(vol);
    }
    return res;
}


