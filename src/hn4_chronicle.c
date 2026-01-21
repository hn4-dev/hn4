/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Chronicle (Immutable Audit Log)
 * SOURCE:      hn4_chronicle.c
 * STATUS:      HARDENED / PRODUCTION (v8.6)
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SECURITY INVARIANTS:
 * 1. RATE LIMITING: Logging is throttled (5s) to prevent DOS logs.
 * 2. TIME TRAVEL: Validates monotonic sequence IDs to detect replays.
 * 3. PHANTOM HEADS: Detects and auto-heals detached log tips.
 */
#include "hn4_chronicle.h"
#include "hn4_endians.h"
#include "hn4_crc.h"
#include "hn4_addr.h"
#include "hn4_errors.h"
#include "hn4_hal.h"  
#include "hn4_constants.h"
#include <string.h>
#include <stddef.h>
#include <stdatomic.h>

/* ABI Guards */
_Static_assert(sizeof(hn4_chronicle_header_t) == 64, "Chronicle Header ABI Violation");
_Static_assert(offsetof(hn4_chronicle_header_t, entry_header_crc) == 60, "Chronicle CRC Offset Violation");
_Static_assert(sizeof(hn4_addr_t) == 8, "Chronicle requires 64-bit hn4_addr_t");


/* =========================================================================
 * INTERNAL HELPERS
 * ========================================================================= */

#define HN4_LOG_RATE_LIMIT_NS (5ULL * 1000000000ULL) /* 5 Seconds */
#define HN4_CHRONICLE_MAX_VERIFY_DEPTH 65536

static void _log_ratelimited(hn4_volume_t* vol, const char* msg, uint64_t val) {
    hn4_time_t now = hn4_hal_get_time_ns();
    if ((now - vol->last_log_ts) > HN4_LOG_RATE_LIMIT_NS) {
        HN4_LOG_CRIT("%s (Val: %llu)", msg, (unsigned long long)val);
        vol->last_log_ts = now;
    }
}

HN4_INLINE uint32_t _get_commit_offset(uint32_t sector_size) {
    return sector_size - sizeof(uint64_t);
}

static uint32_t _calc_header_crc(const hn4_chronicle_header_t* h) {
    return hn4_crc32(0, h, offsetof(hn4_chronicle_header_t, entry_header_crc));
}

static uint32_t _calc_sector_link_crc(const void* buf, uint32_t sector_size) {
    return hn4_crc32(0, buf, sector_size);
}

static uint64_t _calc_expected_marker(uint32_t header_crc) {
    return (uint64_t)header_crc ^ HN4_CHRONICLE_TAIL_KEY;
}

static bool _is_sector_valid(const void* buf, uint32_t ss, hn4_addr_t expected_lba) {
    const hn4_chronicle_header_t* h = (const hn4_chronicle_header_t*)buf;
    
    if (HN4_UNLIKELY(hn4_le64_to_cpu(h->magic) != HN4_CHRONICLE_MAGIC)) return false;

    /* Convert on-disk LBA to CPU format */
    hn4_addr_t stored_lba = hn4_addr_to_cpu(h->self_lba);

    /* Compare using 128-bit safe logic */
#ifdef HN4_USE_128BIT
     if (HN4_UNLIKELY(hn4_u128_cmp(stored_lba, expected_lba) != 0)) return false;
#else
   if (HN4_UNLIKELY(stored_lba != expected_lba)) return false;
#endif

    uint32_t stored_crc = hn4_le32_to_cpu(h->entry_header_crc);
    uint32_t calc_crc   = _calc_header_crc(h);
    if (stored_crc != calc_crc) return false;

    const uint64_t* tail_ptr = (const uint64_t*)((const uint8_t*)buf + _get_commit_offset(ss));
    uint64_t stored_marker = hn4_le64_to_cpu(*tail_ptr);
    uint64_t expected_marker = _calc_expected_marker(stored_crc);

    return (stored_marker == expected_marker);
}


/* 
 * Helper to persist SB during Append or Heal.
 * If write fails, escalate to RO/Panic immediately.
 */
static hn4_result_t _persist_superblock_state(hn4_hal_device_t* dev, hn4_volume_t* vol, uint32_t ss) {
    uint32_t sb_alloc_sz = HN4_ALIGN_UP(HN4_SB_SIZE, ss);
    
    void* sb_buf = hn4_hal_mem_alloc(sb_alloc_sz);
    if (!sb_buf) return HN4_ERR_NOMEM;

    memset(sb_buf, 0, sb_alloc_sz);
    hn4_sb_to_disk(&vol->sb, (hn4_superblock_t*)sb_buf);
    
    hn4_superblock_t* dsb = (hn4_superblock_t*)sb_buf;
    dsb->raw.sb_crc = 0;
    uint32_t sb_crc = hn4_crc32(0, dsb, HN4_SB_SIZE - 4);
    dsb->raw.sb_crc = hn4_cpu_to_le32(sb_crc);

    uint32_t sb_sectors = sb_alloc_sz / ss;
    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_WRITE, hn4_addr_from_u64(0), sb_buf, sb_sectors);

    if (res == HN4_OK) {
        hn4_hal_barrier(dev);
    } else {
        /* CRITICAL FAILURE POLICY: Cannot persist journal pointer. */
        HN4_LOG_CRIT("Chronicle: SB Persist Failed. Forcing RO to prevent Journal Desync.");
        vol->sb.info.state_flags |= HN4_VOL_PANIC;
        vol->read_only = true;
    }

    hn4_hal_mem_free(sb_buf);
    return res;
}

/* =========================================================================
 * CORE LOGIC: APPEND
 * ========================================================================= */

hn4_result_t hn4_chronicle_append(
    hn4_hal_device_t* dev,
    hn4_volume_t* vol,
    uint32_t op_code,
    hn4_addr_t old_lba,
    hn4_addr_t new_lba,
    uint64_t principal_hash
)
{
     if (HN4_UNLIKELY(!dev || !vol || vol->read_only))
    return (!dev || !vol) ? HN4_ERR_INVALID_ARGUMENT : HN4_ERR_ACCESS_DENIED;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t ss = caps->logical_block_size;
    if (HN4_UNLIKELY(ss < (sizeof(hn4_chronicle_header_t) + 8))) return HN4_ERR_GEOMETRY;

    /* 
     * Use Abstract Address Types.
     * Do not downcast to uint64_t.
     */
    hn4_addr_t start = vol->sb.info.journal_start;
    hn4_addr_t head  = vol->sb.info.journal_ptr;
    hn4_addr_t end;
    uint32_t bs = vol->sb.info.block_size;
    uint64_t sb_res = HN4_ALIGN_UP(HN4_SB_SIZE, bs);
    
#ifdef HN4_USE_128BIT
    hn4_u128_t cap_safe = hn4_u128_sub(vol->sb.info.total_capacity, hn4_u128_from_u64(sb_res));
    end = hn4_u128_div_u64(cap_safe, ss);
#else
    end = (vol->sb.info.total_capacity - sb_res) / ss;
#endif

    /* 
     * 128-bit Safe Bounds Check 
     */
    bool bounds_fail = false;
#ifdef HN4_USE_128BIT
    if (hn4_u128_cmp(end, start) <= 0 || 
        hn4_u128_cmp(head, start) < 0 || 
        hn4_u128_cmp(head, end) >= 0) 
    {
        bounds_fail = true;
    }
#else
    if (end <= start || head < start || head >= end) {
        bounds_fail = true;
    }
#endif

    if (bounds_fail) {
        vol->sb.info.state_flags |= HN4_VOL_PANIC;
        return HN4_ERR_BAD_SUPERBLOCK;
    }

    /* 
     * Calculate Previous LBA (Back-link)
     * Logic: prev = (head == start) ? (end - 1) : (head - 1)
     */
    hn4_addr_t prev_lba;
#ifdef HN4_USE_128BIT
    if (hn4_u128_cmp(head, start) == 0) {
        /* Wrap around to end - 1 */
        prev_lba = hn4_u128_sub(end, hn4_u128_from_u64(1));
    } else {
        prev_lba = hn4_u128_sub(head, hn4_u128_from_u64(1));
    }
#else
    prev_lba = (head == start) ? (end - 1) : (head - 1);
#endif

    void* io_buf = hn4_hal_mem_alloc(ss);
    if (!io_buf) return HN4_ERR_NOMEM;

    /* 1. Read Previous for Chain Link */
    uint32_t link_crc = 0;
    uint64_t next_seq = 1; 

    /* prev_lba is already the correct type, pass directly */
    hn4_result_t read_res = hn4_hal_sync_io(dev, HN4_IO_READ, prev_lba, io_buf, 1);
    
    if (HN4_LIKELY(read_res == HN4_OK)) {
        /* 
         * Use shared helper to check if LBA fits in u64 for validation.
         * The validation function _is_sector_valid currently expects u64.
         */
        uint64_t prev_lba_u64;
        bool lba_fits = hn4_addr_try_u64(prev_lba, &prev_lba_u64);

        if (lba_fits && _is_sector_valid(io_buf, ss, prev_lba_u64)) {
            hn4_chronicle_header_t* prev = (hn4_chronicle_header_t*)io_buf;
            uint64_t prev_seq_val = hn4_le64_to_cpu(prev->sequence);

            if (HN4_UNLIKELY(prev_seq_val == UINT64_MAX)) {
                HN4_LOG_CRIT("Chronicle: Sequence Overflow. Volume Locked.");
                vol->sb.info.state_flags |= HN4_VOL_LOCKED;
                hn4_hal_mem_free(io_buf);
                return HN4_ERR_GEOMETRY;
            }
            if (HN4_UNLIKELY(prev_seq_val == 0)) {
                 HN4_LOG_CRIT("Chronicle: Invalid zero sequence in chain.");
                 vol->sb.info.state_flags |= HN4_VOL_PANIC;
                 hn4_hal_mem_free(io_buf);
                 return HN4_ERR_DATA_ROT;
            }

            next_seq = prev_seq_val + 1;
            link_crc = _calc_sector_link_crc(io_buf, ss); 
        } else {
            /* Validation failed OR LBA was > 64-bits */
            
            /* Check if head != start (not genesis) */
            bool is_start;
#ifdef HN4_USE_128BIT
            is_start = (hn4_u128_cmp(head, start) == 0);
#else
            is_start = (head == start);
#endif
            
            /* If we are not at genesis, and validation failed (or was impossible due to size), break chain */
            if (!is_start) {
                
                /* Edge case: If 128-bit mode and LBA didn't fit, we might want to skip validation
                   instead of failing, OR implement 128-bit validation logic. 
                   For now, we fail safe (tamper detected). */
                   
                HN4_LOG_CRIT("Chronicle Broken or Validation Overflow. Append Denied.");
                vol->sb.info.state_flags |= HN4_VOL_PANIC;
                hn4_hal_mem_free(io_buf);
                return HN4_ERR_TAMPERED;
            }
        }
    } else {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    /* 2. Construct New Entry */
    memset(io_buf, 0, ss);
    hn4_chronicle_header_t* h = (hn4_chronicle_header_t*)io_buf;

    h->magic     = hn4_cpu_to_le64(HN4_CHRONICLE_MAGIC);
    h->version   = hn4_cpu_to_le16((uint16_t)HN4_CHRONICLE_VERSION);
    h->op_code   = hn4_cpu_to_le16((uint16_t)op_code);
    h->sequence  = hn4_cpu_to_le64(next_seq);
    h->timestamp = hn4_cpu_to_le64(hn4_hal_get_time_ns());
    
    /* hn4_addr_to_le handles struct/int conversion internally */
    h->old_lba   = hn4_addr_to_le(old_lba);
    h->new_lba   = hn4_addr_to_le(new_lba);
    
    h->principal_hash32 = hn4_cpu_to_le32((uint32_t)principal_hash);
    
    /* Self LBA binding */
    h->self_lba  = hn4_addr_to_le(head);
    
    h->prev_sector_crc = hn4_cpu_to_le32(link_crc);
    
    uint32_t header_crc = _calc_header_crc(h);
    h->entry_header_crc = hn4_cpu_to_le32(header_crc);

    atomic_signal_fence(memory_order_release);
    atomic_thread_fence(memory_order_release);

    uint64_t* marker = (uint64_t*)((uint8_t*)io_buf + _get_commit_offset(ss));
    *marker = hn4_cpu_to_le64(_calc_expected_marker(header_crc));

    /* 3. Commit to Media */
    /* Use 'head' directly, do not convert from sectors as it is already an address */
    if (HN4_UNLIKELY(hn4_hal_sync_io(dev, HN4_IO_WRITE, head, io_buf, 1) != HN4_OK)) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    if (hn4_hal_barrier(dev) != HN4_OK) {
        hn4_hal_mem_free(io_buf);
        return HN4_ERR_HW_IO;
    }

    /* 4. Update In-Memory State */
    
    /* Calculate Next Head using address arithmetic */
    hn4_addr_t next_head;
    
    /* Increment */
    next_head = hn4_addr_add(head, 1); 
    
    /* Check Wrap: if (next_head >= end) next_head = start */
#ifdef HN4_USE_128BIT
    if (hn4_u128_cmp(next_head, end) >= 0) {
        next_head = start;
    }
#else
    if (next_head >= end) {
        next_head = start;
    }
#endif
    
    vol->sb.info.journal_ptr = next_head;
    vol->sb.info.last_journal_seq = next_seq;

    /* 5. Persist Superblock */
    hn4_hal_mem_free(io_buf);
    
    return _persist_superblock_state(dev, vol, ss);
}

/* =========================================================================
 * CORE LOGIC: VERIFY (Auto-Healing)
 * ========================================================================= */

hn4_result_t hn4_chronicle_verify_integrity(
    hn4_hal_device_t* dev,
    hn4_volume_t* vol
)
{
     if( HN4_UNLIKELY(!dev || !vol)) return HN4_ERR_INVALID_ARGUMENT;

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    uint32_t ss = caps->logical_block_size;

    /* Use abstract types, do not downcast to u64 */
    hn4_addr_t start = vol->sb.info.journal_start;
    
    hn4_addr_t end;
#ifdef HN4_USE_128BIT
    end = hn4_u128_div_u64(vol->sb.info.total_capacity, ss);
#else
    end = vol->sb.info.total_capacity / ss;
#endif

    /* 
     * HEALING LOOP
     */
    bool healing_needed = true;
    while (healing_needed) {
        healing_needed = false;
        
        /* Head is an abstract address */
        hn4_addr_t head = vol->sb.info.journal_ptr;

        /* 128-bit Bounds Check */
        bool bounds_bad = false;
#ifdef HN4_USE_128BIT
        if (hn4_u128_cmp(head, start) < 0 || hn4_u128_cmp(head, end) >= 0) bounds_bad = true;
#else
        if (head < start || head >= end) bounds_bad = true;
#endif
        if (bounds_bad) return HN4_ERR_BAD_SUPERBLOCK;

        void* buf = hn4_hal_mem_alloc(ss);
        void* prev_buf = hn4_hal_mem_alloc(ss);
        if (!buf || !prev_buf) {
            if (buf) hn4_hal_mem_free(buf);
            if (prev_buf) hn4_hal_mem_free(prev_buf);
            return HN4_ERR_NOMEM;
        }

        /* Check for PHANTOM HEAD */
        if (hn4_hal_sync_io(dev, HN4_IO_READ, hn4_lba_from_sectors(head), buf, 1) == HN4_OK) {
            
            if (_is_sector_valid(buf, ss, head)) {
                hn4_chronicle_header_t* phantom = (hn4_chronicle_header_t*)buf;
                uint64_t ph_seq = hn4_le64_to_cpu(phantom->sequence);
                uint32_t ph_prev_crc = hn4_le32_to_cpu(phantom->prev_sector_crc);

                /* 
                 * Calculate Previous Pointer 
                 * Logic: prev = (head == start) ? (end - 1) : (head - 1)
                 */
                hn4_addr_t prev_ptr;
                hn4_u128_t one = hn4_u128_from_u64(1);
            #ifdef HN4_USE_128BIT
                if (hn4_u128_cmp(head, start) == 0) {
                     prev_ptr = hn4_u128_sub(end, one);
                } else {
                     prev_ptr = hn4_u128_sub(head, one);
                }
            #else
                prev_ptr = (head == start) ? (end - 1) : (head - 1);
            #endif
                
                if (hn4_hal_sync_io(dev, HN4_IO_READ, prev_ptr, prev_buf, 1) == HN4_OK) {
                    
                    bool chain_ok = false;
                    
                    if (_is_sector_valid(prev_buf, ss, prev_ptr)) {
                        hn4_chronicle_header_t* prev = (hn4_chronicle_header_t*)prev_buf;
                        uint64_t pr_seq = hn4_le64_to_cpu(prev->sequence);
                        uint32_t pr_crc = _calc_sector_link_crc(prev_buf, ss);
                        
                        /* Strict Monotonicity Check */
                        if (ph_seq == pr_seq + 1 && ph_prev_crc == pr_crc) {
                            chain_ok = true;
                        }
                    }

                    if (chain_ok) {

                         /* STEP 6: Telemetry */
                        atomic_fetch_add(&vol->health.heal_count, 1);

                         hn4_addr_t next_head = hn4_addr_add(head, 1);
                    #ifdef HN4_USE_128BIT
                        if (hn4_u128_cmp(next_head, end) >= 0) next_head = start;
                    #else
                        if (next_head >= end) next_head = start;
                    #endif
                        
                        /* Logging needs casting for %llu print, use .lo if 128-bit */
                    #ifdef HN4_USE_128BIT
                        HN4_LOG_WARN("Chronicle: Phantom Head @ %llu verified. Healing...", (unsigned long long)head.lo);
                    #else
                        HN4_LOG_WARN("Chronicle: Phantom Head @ %llu verified. Healing...", (unsigned long long)head);
                    #endif

                        vol->sb.info.journal_ptr = next_head;

                        if (next_head >= end) next_head = start;
                        vol->sb.info.journal_ptr = hn4_lba_from_sectors(next_head);
                        vol->sb.info.last_journal_seq = ph_seq; 
                        /* If persist fails, stop immediately */
                        if (_persist_superblock_state(dev, vol, ss) != HN4_OK) {
                             HN4_LOG_CRIT("Chronicle: Healing persist failed. Stopping.");
                             hn4_hal_mem_free(buf); 
                             hn4_hal_mem_free(prev_buf);
                             return HN4_ERR_HW_IO;
                        }

                        healing_needed = true;
                    }
                }
            }
        }
        
        hn4_hal_mem_free(buf);
        hn4_hal_mem_free(prev_buf);
    }

    /* 
     * REVERSE VERIFICATION 
     */
    hn4_addr_t head_addr = vol->sb.info.journal_ptr;
    hn4_addr_t cursor_addr;
    
#ifdef HN4_USE_128BIT
    if (hn4_u128_cmp(head_addr, start) == 0) {
        cursor_addr = hn4_u128_sub(end, hn4_u128_from_u64(1));
    } else {
        cursor_addr = hn4_u128_sub(head_addr, hn4_u128_from_u64(1));
    }
#else
    uint64_t head_val = head_addr;
    uint64_t start_val = start;
    uint64_t end_val = end;
    cursor_addr = (head_val == start_val) ? (end_val - 1) : (head_val - 1);
#endif
    
    void* buf = hn4_hal_mem_alloc(ss);
    void* prev_buf = hn4_hal_mem_alloc(ss);
    if (!buf || !prev_buf) {
        if(buf) hn4_hal_mem_free(buf);
        if(prev_buf) hn4_hal_mem_free(prev_buf);
        return HN4_ERR_NOMEM;
    }

    /* Use cursor_addr (typed) instead of cursor (u64) */
    if (hn4_hal_sync_io(dev, HN4_IO_READ, cursor_addr, buf, 1) != HN4_OK) {
        hn4_hal_mem_free(buf); hn4_hal_mem_free(prev_buf);
        return HN4_ERR_HW_IO;
    }

    /* Pass typed address to validation logic */
     if (!_is_sector_valid(buf, ss, cursor_addr)) {
        
        /* FIX: Handle address comparison based on build mode */
        bool is_empty_log = false;
    #ifdef HN4_USE_128BIT
        if (hn4_u128_cmp(head_addr, start) == 0) is_empty_log = true;
    #else
        if (head_addr == start) is_empty_log = true;
    #endif

        if (is_empty_log) {
            hn4_hal_mem_free(buf); hn4_hal_mem_free(prev_buf);
            return HN4_OK; /* Empty Log */
        } else {
            /* Use Rate Limited Log (cast to u64 for logging if 128bit) */
            uint64_t cursor_val = hn4_addr_to_u64(cursor_addr);
            _log_ratelimited(vol, "Chronicle: Corrupt Tip Detected", cursor_val);
            
            vol->sb.info.state_flags |= HN4_VOL_PANIC;
            hn4_hal_mem_free(buf); hn4_hal_mem_free(prev_buf);
            return HN4_ERR_TAMPERED;
        }
    }

    /* If tip is valid, perform TIME TRAVEL CHECK */
    hn4_chronicle_header_t* tip = (hn4_chronicle_header_t*)buf;
    uint64_t tip_seq = hn4_le64_to_cpu(tip->sequence);
    uint64_t sb_seq  = vol->sb.info.last_journal_seq;

    /* Only check if SB has a recorded sequence (non-zero) */
    if (sb_seq > 0 && HN4_UNLIKELY(tip_seq < sb_seq)) {
        _log_ratelimited(vol, "SECURITY: Time-Travel Detected! Log Seq < SB Seq", tip_seq);
        
        atomic_fetch_add(&vol->health.trajectory_collapse_counter, HN4_ERR_TAMPERED);
        vol->sb.info.state_flags |= HN4_VOL_PANIC;
        
        hn4_hal_mem_free(buf); hn4_hal_mem_free(prev_buf);
        return HN4_ERR_TAMPERED;
    }

    /* Verify backwards */
    hn4_result_t status = HN4_OK;
    uint64_t steps = 0;
    
    /* Calculate max steps using 128-bit safe subtraction */
    uint64_t max_steps;
#ifdef HN4_USE_128BIT
    hn4_u128_t diff_128 = hn4_u128_sub(end, start);
    /* Safe assumption: journal size < 18EB */
    max_steps = diff_128.lo;
#else
    max_steps = (end - start);
#endif
    
    while (steps < max_steps) {

        if (steps >= HN4_CHRONICLE_MAX_VERIFY_DEPTH) {
            HN4_LOG_WARN("Chronicle: Verified recent history (%llu). Deep scan skipped.", 
                         (unsigned long long)steps);
            break;
        }
        hn4_chronicle_header_t* curr = (hn4_chronicle_header_t*)buf;
        uint64_t curr_seq = hn4_le64_to_cpu(curr->sequence);
        uint32_t expected_prev_hash = hn4_le32_to_cpu(curr->prev_sector_crc);

        if (curr_seq == 1) break; /* Genesis reached */

        /* Calculate Previous LBA (Typed) */
        hn4_addr_t prev_lba;
    #ifdef HN4_USE_128BIT
        if (hn4_u128_cmp(cursor_addr, start) == 0) {
            prev_lba = hn4_u128_sub(end, hn4_u128_from_u64(1));
        } else {
            prev_lba = hn4_u128_sub(cursor_addr, hn4_u128_from_u64(1));
        }
    #else
        uint64_t c_val = cursor_addr;
        uint64_t s_val = start;
        uint64_t e_val = end;
        prev_lba = (c_val == s_val) ? (e_val - 1) : (c_val - 1);
    #endif

        if (HN4_UNLIKELY(hn4_hal_sync_io(dev, HN4_IO_READ, prev_lba, prev_buf, 1) != HN4_OK)) {
            status = HN4_ERR_HW_IO;
            break;
        }

        /* 
         * If the previous sector is invalid (garbage/overwritten), we assume 
         * we hit the end of the history buffer. This is NOT tampering.
         * We verify the chain *up to* the oldest valid block.
         */
        if (!_is_sector_valid(prev_buf, ss, prev_lba)) {
            // Hit end of history.
            break; 
        }

        /* Verify Hash Link - Only if valid */
        if (HN4_UNLIKELY(_calc_sector_link_crc(prev_buf, ss) != expected_prev_hash)) {
            uint64_t prev_lba_u64 = hn4_addr_to_u64(prev_lba);
            HN4_LOG_ERR("Chronicle: Hash Mismatch at LBA %llu", prev_lba_u64);
            status = HN4_ERR_TAMPERED;
            break;
        }

        /* Step Back */
        memcpy(buf, prev_buf, ss);
        cursor_addr = prev_lba;
        steps++;
    }

    hn4_hal_mem_free(buf);
    hn4_hal_mem_free(prev_buf);

    if (status != HN4_OK) {
        /* STEP 7: Track Barrier/IO Fails */
        atomic_fetch_add(&vol->health.barrier_failures, 1);
        vol->sb.info.state_flags |= HN4_VOL_PANIC;
    }

    return status;
}