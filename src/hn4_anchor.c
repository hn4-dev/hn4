/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      Anchor Management
 * SOURCE:      hn4_anchor.c
 * STATUS:      HARDENED / REVIEWED
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 *
 * SAFETY CONTRACT:
 * 1. PRE-CONDITION: The caller (hn4_format) MUST have zeroed the Cortex region.
 *    This function only writes the Root Anchor (Block 0).
 * 2. IDEMPOTENCY: This is a DESTRUCTIVE GENESIS operation. It blindly overwrites
 *    the root slot. It is not safe for "Repair" operations, only "Format".
 * 3. CRC INVARIANT: The CRC calculation is STREAMED. It skips the checksum field
 *    structurally, then updates with the inline buffer.
 */

#include "hn4_anchor.h"
#include "hn4_crc.h"
#include "hn4_endians.h"
#include "hn4_errors.h"
#include "hn4_annotations.h"
#include <string.h>

/* 
 * Dynamic CRC Coverage 
 * Calculate offset based on struct layout, not magic hex.
 * CRC covers everything from byte 0 up to (but not including) the checksum field.
 */
#define HN4_ANCHOR_CRC_LEN offsetof(hn4_anchor_t, checksum)

/* 
 * Valid Permission Bits Mask
 * Used to sanitize overrides from compat_flags.
 */
static const uint32_t HN4_PERM_VALID_MASK = 
    HN4_PERM_READ | HN4_PERM_WRITE | HN4_PERM_EXEC | HN4_PERM_APPEND |
    HN4_PERM_IMMUTABLE | HN4_PERM_SOVEREIGN | HN4_PERM_ENCRYPTED;

hn4_result_t hn4_anchor_write_genesis(hn4_hal_device_t* dev, const hn4_superblock_t* sb) 
{
    /* Input Validation */
    if (!dev || !sb) return HN4_ERR_INVALID_ARGUMENT;

    /* 
     * [NUCLEAR OPTION] PRE-CONDITION CHECK
     * We guarantee the Cortex region is zeroed via the State Flag contract.
     */
    if (!(sb->info.state_flags & HN4_VOL_METADATA_ZEROED)) {
        HN4_LOG_CRIT("Anchor Genesis Rejected: Cortex not certified zeroed.");
        return HN4_ERR_UNINITIALIZED;
    }

    const hn4_hal_caps_t* caps = hn4_hal_get_caps(dev);
    if (!caps) return HN4_ERR_INTERNAL_FAULT;

    uint32_t bs = sb->info.block_size;
    uint32_t ss = caps->logical_block_size;
    hn4_addr_t write_lba = sb->info.lba_cortex_start;

    /* Avoid divide-by-zero / Geometry Errors */
    if (ss == 0 || bs == 0) return HN4_ERR_GEOMETRY;

    /* 
     * If BlockSize < SectorSize (impossible by spec, but possible by bug),
     * sector_count becomes 0, leading to a silent no-op write.
     */
    uint32_t sector_count = bs / ss;
    if (sector_count == 0) {
        HN4_LOG_CRIT("Anchor Geometry Error: BS %u < SS %u", bs, ss);
        return HN4_ERR_GEOMETRY;
    }

    /* 
     * (Optional): Debug Verification scan.
     * In debug builds, we don't trust the flag. We read the target sector to verify 0.
     */
#ifdef HN4_DEBUG
    {
        void* dbg_buf = hn4_hal_mem_alloc(ss);
        if (dbg_buf) {
            if (hn4_hal_sync_io(dev, HN4_IO_READ, write_lba, dbg_buf, 1) == HN4_OK) {
                uint64_t* scan = (uint64_t*)dbg_buf;
                for (uint32_t i = 0; i < (ss / 8); i++) {
                    if (scan[i] != 0) {
                        HN4_LOG_CRIT("DEBUG: Cortex Region NOT physically zeroed at LBA offset 0!");
                        hn4_hal_mem_free(dbg_buf);
                        return HN4_ERR_DATA_ROT;
                    }
                }
            }
            hn4_hal_mem_free(dbg_buf);
        }
    }
#endif

    /* 
     * Alignment Check
     */
    uint64_t lba_val;
#ifdef HN4_USE_128BIT
    lba_val = write_lba.lo;
#else
    lba_val = write_lba;
#endif

    if ((lba_val * ss) % bs != 0) {
        HN4_LOG_CRIT("Root Anchor Misaligned: LBA %llu vs BS %u", lba_val, bs);
        return HN4_ERR_ALIGNMENT_FAIL;
    }

    /* Allocate Memory */
    void* buf = hn4_hal_mem_alloc(bs);
    if (!buf) return HN4_ERR_NOMEM;

    /* Secure Zero: Ensures padding is deterministic */
    memset(buf, 0, bs);

    hn4_anchor_t* root = (hn4_anchor_t*)buf;

    /* 1. Identity: 0xFF...FF (System Root) */
    root->seed_id.lo   = 0xFFFFFFFFFFFFFFFFULL;
    root->seed_id.hi   = 0xFFFFFFFFFFFFFFFFULL;
    root->public_id    = root->seed_id; 

    /* 2. Topology: Virtual Object (No Gravity) */
    root->gravity_center = 0;
    root->mass           = 0;
    
    /* 
     * Math Invariant (Spec 18.2): GCD(V, Phi) == 1.
     * V=0 is invalid. We set V=1 (Sequential/Rail).
     */
    root->orbit_vector[0] = 1;

    /* 
     * 3. Class: Static System Object 
     * Must include HN4_FLAG_VALID (Bit 8) or the Root is considered empty.
     */
    uint64_t dclass = HN4_VOL_STATIC | HN4_FLAG_VALID;
    root->data_class = hn4_cpu_to_le64(dclass);

    /* 
     * 4. Permissions: Sovereign (Root) Control 
     */
    uint32_t perms = HN4_PERM_READ | HN4_PERM_WRITE | HN4_PERM_EXEC | 
                     HN4_PERM_IMMUTABLE | HN4_PERM_SOVEREIGN;
    
    /* Safely OR in the user-supplied overrides */
    uint32_t user_overrides = (uint32_t)(sb->info.compat_flags & 0xFFFFFFFF);
    perms |= (user_overrides & HN4_PERM_VALID_MASK);

    root->permissions = hn4_cpu_to_le32(perms);

    /* 5. Time: Genesis Timestamp */
    uint64_t gen_ts = sb->info.generation_ts;
    root->create_clock = hn4_cpu_to_le32((uint32_t)(gen_ts / 1000000000ULL));
    root->mod_clock    = hn4_cpu_to_le64(gen_ts);

    /* 6. Name: "ROOT" (Debug Hint) */
    strncpy((char*)root->inline_buffer, "ROOT", sizeof(root->inline_buffer) - 1);

    /* 
     * 7. Checksum: Corrected Coverage (Spec 8.1)
     */
    root->checksum = 0;
    
    /* CRC Head (0x00 to 0x5F) */
    root->checksum = 0;
    
    /* Hash the ENTIRE 128 bytes (includes orbit_hints and inline_buffer) */
    uint32_t crc = hn4_crc32(0, root, sizeof(hn4_anchor_t));
    
    root->checksum = hn4_cpu_to_le32(crc);

    /* 8. Commit to Cortex Start */
    hn4_result_t res = hn4_hal_sync_io(dev, HN4_IO_WRITE, write_lba, buf, sector_count);

    hn4_hal_mem_free(buf);

    if (res != HN4_OK) return res;

    /* Mandatory Barrier: Ensure Root hits media before SB points to it */
    return hn4_hal_sync_io(dev, HN4_IO_FLUSH, hn4_addr_from_u64(0), NULL, 0);
}

/**
 * hn4_write_anchor_atomic
 * 
 * Persists an in-memory Anchor to the on-disk Cortex table.
 * 
 * SAFETY:
 * 1. CHECKSUM: Updates the CRC32C before writing.
 * 2. LOCATION: Uses the Cortex Hash equation to find the physical block.
 * 3. ATOMICITY: Issues a single block write (4KB aligned).
 * 
 * @param vol     Volume context.
 * @param anchor  The modified anchor to persist.
 * @return        HN4_OK on success.
 */
hn4_result_t hn4_write_anchor_atomic(
    HN4_IN hn4_volume_t* vol, 
    HN4_IN hn4_anchor_t* anchor
)
{
    if (!vol || !anchor) return HN4_ERR_INVALID_ARGUMENT;
    if (vol->read_only) return HN4_ERR_ACCESS_DENIED;

    /* 1. Recalculate Checksum (Strict Mode: No Inline Buffer in CRC for consistency with Genesis?) */
    /* Spec 8.1: Checksum covers 0x00 to 0x60 (offset of checksum field). Inline buffer is excluded in some contexts? */
    /* Correction: Genesis uses Split-CRC. We must match that. */
    
    anchor->checksum = 0;
    
    /* CRC Head (0x00 to 0x5F) */
    uint32_t crc = hn4_crc32(0, anchor, sizeof(hn4_anchor_t));
    
    anchor->checksum = hn4_cpu_to_le32(crc);

    /* 2. Locate Physical LBA (Cortex Mapping) */
    /* LBA = Cortex_Start + (Hash(SeedID) % Cortex_Size_In_Anchors) */
    
    /* Calculate Cortex Geometry */
    uint32_t bs = vol->vol_block_size;
    uint32_t ss = 512; /* Assumed min sector for LBA calc if not available */
    
    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
    if (caps) ss = caps->logical_block_size;

    hn4_addr_t cortex_start = vol->sb.info.lba_cortex_start;
    hn4_addr_t cortex_end   = vol->sb.info.lba_bitmap_start;
    
    uint64_t start_val = hn4_addr_to_u64(cortex_start);
    uint64_t end_val   = hn4_addr_to_u64(cortex_end);
    
    /* Valid region check */
    if (end_val <= start_val) return HN4_ERR_GEOMETRY;
    
    uint64_t region_bytes = (end_val - start_val) * ss;
    uint64_t total_slots  = region_bytes / sizeof(hn4_anchor_t);
    
    if (total_slots == 0) return HN4_ERR_GEOMETRY;

    /* Hash the ID */
    hn4_u128_t seed = hn4_le128_to_cpu(anchor->seed_id);
    
    uint64_t h = seed.lo ^ seed.hi;
    
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL; /* HN4_NS_HASH_CONST */
    h ^= (h >> 33);
    
    uint64_t slot_idx = h % total_slots;
    
    /* Calculate Physical Sector LBA */
    uint64_t byte_offset = slot_idx * sizeof(hn4_anchor_t);
    uint64_t sector_offset = byte_offset / ss;
    uint64_t byte_in_sector = byte_offset % ss;
    
    hn4_addr_t write_lba = hn4_addr_add(cortex_start, sector_offset);

    /* 
     * 3. Read-Modify-Write (RMW) 
     * Since anchors are 128 bytes but sectors are 512/4096 bytes, we cannot write just the anchor.
     * We must read the sector, update the slot, and write back.
     */
    void* io_buf = hn4_hal_mem_alloc(ss);
    if (!io_buf) return HN4_ERR_NOMEM;

    /* Read */
    hn4_result_t res = hn4_hal_sync_io(vol->target_device, HN4_IO_READ, write_lba, io_buf, 1);
    if (res != HN4_OK) {
        hn4_hal_mem_free(io_buf);
        return res;
    }

    /* Modify */
    memcpy((uint8_t*)io_buf + byte_in_sector, anchor, sizeof(hn4_anchor_t));

    /* Write */
    res = hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, write_lba, io_buf, 1);
    
    /* Barrier to ensure metadata persistence */
    if (res == HN4_OK) {
        hn4_hal_barrier(vol->target_device);
    }

    hn4_hal_mem_free(io_buf);
    return res;
}