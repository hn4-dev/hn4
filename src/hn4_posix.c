/*
 * HYDRA-NEXUS 4 (HN4) STORAGE ENGINE
 * MODULE:      POSIX Compatibility Layer
 * SOURCE:      hn4_posix.c
 * VERSION:     28.5
 * ARCHITECT:   Core Systems Engineering
 * COPYRIGHT:   (c) 2026 The Hydra-Nexus Team.
 */

#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_errors.h"
#include "hn4_endians.h"
#include "hn4_addr.h"
#include "hn4_constants.h"

/* Freestanding Headers */
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * 1. BARE METAL ABI DEFINITIONS
 * ========================================================================= */

#define HN4_EPERM        1
#define HN4_ENOENT       2
#define HN4_EIO          5
#define HN4_EBADF        9
#define HN4_ENOMEM       12
#define HN4_EACCES       13
#define HN4_EBUSY        16
#define HN4_EEXIST       17
#define HN4_ENOTDIR      20
#define HN4_EISDIR       21
#define HN4_EINVAL       22
#define HN4_ENOSPC       28
#define HN4_EROFS        30
#define HN4_ENAMETOOLONG 36
#define HN4_EOVERFLOW    75
#define HN4_EFBIG        27

#define HN4_O_RDONLY     00
#define HN4_O_WRONLY     01
#define HN4_O_RDWR       02
#define HN4_O_CREAT      0100
#define HN4_O_EXCL       0200
#define HN4_O_TRUNC      01000
#define HN4_O_APPEND     02000
#define HN4_O_DIRECTORY  0200000
#define HN4_O_ACCMODE    03

#define HN4_S_IFMT       0170000
#define HN4_S_IFDIR      0040000
#define HN4_S_IFREG      0100000
#define HN4_S_IRWXU      00700
#define HN4_S_IRUSR      00400
#define HN4_S_IWUSR      00200
#define HN4_S_IXUSR      00100

#define HN4_SEEK_SET     0
#define HN4_SEEK_CUR     1
#define HN4_SEEK_END     2
/* Constants required for Hash Calculation (from hn4_namespace.c) */
#define HN4_NS_HASH_CONST 0xff51afd7ed558ccdULL
typedef uint32_t hn4_mode_t;
typedef int64_t  hn4_off_t;
typedef int64_t  hn4_ssize_t;
typedef uint64_t hn4_ino_t;

typedef struct {
    hn4_ino_t   st_ino;
    hn4_mode_t  st_mode;
    uint64_t    st_size;
    uint64_t    st_mtime;
    uint64_t    st_ctime;
    uint64_t    st_blksize;
    uint64_t    st_blocks;
} hn4_stat_t;

/* =========================================================================
 * 2. INTERNAL UTILITIES
 * ========================================================================= */

static inline uint64_t _imp_atomic_load_u64(volatile uint64_t* ptr) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
#else
    volatile uint32_t* p32 = (volatile uint32_t*)ptr;
    uint32_t lo, hi, hi2;
    do {
#if defined(HN4_BIG_ENDIAN)
        hi = p32[0]; lo = p32[1]; hi2 = p32[0];
#else
        hi = p32[1]; lo = p32[0]; hi2 = p32[1];
#endif
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
#endif
}

static inline void _imp_memory_barrier(void) {
#if defined(HN4_HAL_MEMORY_BARRIER)
    HN4_HAL_MEMORY_BARRIER();
#elif defined(__GNUC__) || defined(__clang__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static inline void _imp_dcache_flush(void* ptr, size_t len) {
#if defined(HN4_HAL_DCACHE_FLUSH)
    HN4_HAL_DCACHE_FLUSH(ptr, len);
#else
    (void)ptr; (void)len;
#endif
}

static void* _imp_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void* _imp_memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

static size_t _imp_strlen(const char* s) {
    if (!s) return 0;
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void _imp_strncpy_safe(char* dst, const char* src, size_t n) {
    if (n == 0) return;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dst[i] = src[i];
    if (i < n) dst[i] = '\0';
}

static int _imp_strcmp(const char* s1, const char* s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static bool _imp_safe_add_signed(int64_t base, int64_t offset, int64_t* res) {
    if ((offset > 0) && (base > INT64_MAX - offset)) return false;
    if ((offset < 0) && (base < INT64_MIN - offset)) return false;
    *res = base + offset;
    return true;
}

/* =========================================================================
 * 3. INTERNAL HELPERS
 * ========================================================================= */

#define HN4_MAX_PATH        256
#define HN4_INLINE_NAME_MAX 24
#define HN4_FLAG_IS_DIRECTORY  (1ULL << 63)
#define HN4_EXT_TYPE_TETHER     0x03 
#define HN4_FLAG_EXTENDED       (1ULL << 23)
#define HN4_EXT_TYPE_LONGNAME   0x02

typedef struct {
    hn4_anchor_t    cached_anchor;
    uint64_t        current_offset;
    uint64_t        anchor_idx;
    uint32_t        cached_gen;
    int             open_flags;
    uint32_t        session_perms; 
    bool            dirty;
    bool            is_directory;
    bool            unlinked;
} hn4_vfs_handle_t;

typedef struct {
    hn4_anchor_t    anchor;
    uint64_t        slot_idx;
    bool            is_root;
    bool            found;
    bool            trailing_slash;
    char            name[HN4_INLINE_NAME_MAX + 1];
} hn4_lookup_ctx_t;

static int _map_err(hn4_result_t res) {
    switch (res) {
        case HN4_OK: return 0;
        case HN4_ERR_NOT_FOUND: return -HN4_ENOENT;
        case HN4_ERR_ACCESS_DENIED: return -HN4_EACCES;
        case HN4_ERR_IMMUTABLE: return -HN4_EPERM;
        case HN4_ERR_ENOSPC: return -HN4_ENOSPC;
        case HN4_ERR_NOMEM: return -HN4_ENOMEM;
        case HN4_ERR_EEXIST: return -HN4_EEXIST;
        case HN4_ERR_HW_IO: return -HN4_EIO;
        case HN4_ERR_VOLUME_LOCKED: return -HN4_EROFS;
        default: return -HN4_EIO;
    }
}

static uint32_t _mode_to_perms(hn4_mode_t m) {
    uint32_t p = HN4_PERM_SOVEREIGN;
    if (m & HN4_S_IRUSR) p |= HN4_PERM_READ;
    if (m & HN4_S_IWUSR) p |= (HN4_PERM_WRITE | HN4_PERM_APPEND);
    if (m & HN4_S_IXUSR) p |= HN4_PERM_EXEC;
    return p;
}

static hn4_mode_t _perms_to_mode(uint32_t p) {
    hn4_mode_t m = 0;
    if (p & HN4_PERM_READ) m |= HN4_S_IRUSR;
    if (p & HN4_PERM_WRITE) m |= HN4_S_IWUSR;
    if (p & HN4_PERM_EXEC) m |= HN4_S_IXUSR;
    return m;
}

static int _resolve_path(hn4_volume_t* vol, const char* path, hn4_lookup_ctx_t* ctx) {
    if (HN4_UNLIKELY(!path)) return -HN4_EINVAL;
    
    _imp_memset(ctx, 0, sizeof(hn4_lookup_ctx_t));

        /* 1. Root Handling */
        if (_imp_strcmp(path, "/") == 0) {
        ctx->is_root = true;
        ctx->found = true;
    
        /* Synthesize Root Metadata */
        _imp_memset(&ctx->anchor, 0, sizeof(hn4_anchor_t));
        ctx->anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_FLAG_IS_DIRECTORY | HN4_VOL_STATIC);
        ctx->anchor.permissions = hn4_cpu_to_le32(HN4_PERM_READ | HN4_PERM_EXEC | HN4_PERM_SOVEREIGN);
    
        return 0;
    }

    /* 2. Delegate to Resonance Engine (Handles tags, nested paths, slicing) */
    hn4_result_t res = hn4_ns_resolve(vol, path, &ctx->anchor);

    if (res == HN4_OK) {
        ctx->found = true;
        
        /* 
         * 3. Reverse Lookup for Slot Index 
         * POSIX write ops need the physical RAM slot index to update cache.
         * hn4_ns_resolve gives us the data; we hash the ID to find the slot.
         */
        uint64_t slot_idx;
        hn4_u128_t seed = hn4_le128_to_cpu(ctx->anchor.seed_id);
        
        /* Use the internal cortex scanner to find *where* this anchor lives */
        if (_ns_scan_cortex_slot(vol, seed, NULL, &slot_idx) == HN4_OK) {
            ctx->slot_idx = slot_idx;
        } else {
            /* Desync: Found by resolve but missing in scan? */
            return -HN4_EIO;
        }

        /* 4. Directory Semantics */
        size_t len = _imp_strlen(path);
        if (len > 0 && path[len-1] == '/') ctx->trailing_slash = true;
        
        uint64_t dc = hn4_le64_to_cpu(ctx->anchor.data_class);
        if (ctx->trailing_slash && !(dc & HN4_FLAG_IS_DIRECTORY)) {
            return -HN4_ENOTDIR;
        }

        /* Cache the name for creation logic if needed */
        _imp_strncpy_safe(ctx->name, path, HN4_INLINE_NAME_MAX);
        return 0;
    }

    return _map_err(res);
}


static int _find_free_slot(hn4_volume_t* vol, uint64_t* slot_idx) {
    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    uint64_t count = vol->cortex_size / sizeof(hn4_anchor_t);
    uint64_t i = vol->alloc.cortex_search_head;
    uint64_t checked = 0;

    while (checked < count) {
        if (i >= count) i = 0;
        
        uint64_t dclass = _imp_atomic_load_u64(&anchors[i].data_class);
        dclass = hn4_le64_to_cpu(dclass);

        if (!(dclass & HN4_FLAG_VALID) || (dclass & HN4_FLAG_TOMBSTONE)) {
            /* Claim Slot Immediately */
            anchors[i].data_class = hn4_cpu_to_le64(HN4_FLAG_VALID); // Temporary reservation
            *slot_idx = i;
            vol->alloc.cortex_search_head = i + 1;
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
            return 0;
        }
        i++;
        checked++;
    }

    hn4_hal_spinlock_release(&vol->locking.l2_lock);
    return -HN4_ENOSPC;
}

/* =========================================================================
 * 4. API IMPLEMENTATION
 * ========================================================================= */

int hn4_posix_open(hn4_volume_t* vol, const char* path, int flags, hn4_mode_t mode, hn4_handle_t** out) {
    if (!vol || !path || !out) return -HN4_EINVAL;

    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);

    /* ---------------------------------------------------------
     * CASE 1: TARGET EXISTS
     * --------------------------------------------------------- */
    if (err == 0 && lk.found) {
        if ((flags & HN4_O_CREAT) && (flags & HN4_O_EXCL)) return -HN4_EEXIST;

        /* Directory Logic Check */
        uint64_t dclass = hn4_le64_to_cpu(lk.anchor.data_class);
        bool is_dir = (dclass & HN4_FLAG_IS_DIRECTORY) || lk.is_root;

        if (is_dir) {
            /* Cannot open directory for writing */
            if ((flags & HN4_O_WRONLY) || (flags & HN4_O_RDWR)) return -HN4_EISDIR;
        } else {
            /* Cannot open file with O_DIRECTORY */
            if (flags & HN4_O_DIRECTORY) return -HN4_ENOTDIR;
        }

        uint32_t perms = hn4_le32_to_cpu(lk.anchor.permissions);
        
        /* Permission Checks */
        if ((flags & HN4_O_ACCMODE) != HN4_O_RDONLY) {
            /* Write Access Required */
            if (vol->read_only) return -HN4_EROFS;
            if (!(perms & (HN4_PERM_WRITE | HN4_PERM_APPEND))) return -HN4_EACCES;
            if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;
        } else {
            /* Read Access Required */
            if (!(perms & HN4_PERM_READ)) return -HN4_EACCES;
        }

        /* O_TRUNC Logic */
        if (flags & HN4_O_TRUNC) {
            if ((flags & HN4_O_ACCMODE) == HN4_O_RDONLY) return -HN4_EINVAL;
            if (is_dir) return -HN4_EISDIR;

            lk.anchor.mass = 0;
            
            /* Increment generation to invalidate old blocks */
            uint32_t g = hn4_le32_to_cpu(lk.anchor.write_gen);
            lk.anchor.write_gen = hn4_cpu_to_le32(g + 1);

            /* Atomic Commit */
            if (hn4_write_anchor_atomic(vol, &lk.anchor) != HN4_OK) return -HN4_EIO;
            
            /* Update RAM Cache */
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            ((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx] = lk.anchor;
            _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx], sizeof(hn4_anchor_t));
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
        }
    }
    /* ---------------------------------------------------------
     * CASE 2: TARGET MISSING (CREATE)
     * --------------------------------------------------------- */
    else if (err == -HN4_ENOENT) {
        if (!(flags & HN4_O_CREAT)) return -HN4_ENOENT;
        if (vol->read_only) return -HN4_EROFS;

        hn4_anchor_t* ram_base = (hn4_anchor_t*)vol->nano_cortex;
        size_t cortex_cnt = vol->cortex_size / sizeof(hn4_anchor_t);
        uint64_t target_slot = 0;
        int attempts = 0;
        bool slot_reserved = false;

        /* Prepare the Anchor Container */
        hn4_anchor_t new_anc;
        _imp_memset(&new_anc, 0, sizeof(hn4_anchor_t));

        /* --- SUB-STEP 2A: Find Free Slot --- */
        while (attempts++ < 1000) {
            /* Generate Candidate Identity */
            new_anc.seed_id.lo = hn4_hal_get_random_u64();
            new_anc.seed_id.hi = hn4_hal_get_random_u64();
            new_anc.public_id  = new_anc.seed_id;

            /* Calculate Slot */
            uint64_t h = new_anc.seed_id.lo ^ new_anc.seed_id.hi;
            h ^= (h >> 33);
            h *= HN4_NS_HASH_CONST;
            h ^= (h >> 33);
            target_slot = h % cortex_cnt;

            /* Check Availability */
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            
            hn4_anchor_t* slot_ptr = &ram_base[target_slot];
            uint64_t dclass = _imp_atomic_load_u64(&slot_ptr->data_class);
            dclass = hn4_le64_to_cpu(dclass);

            if (!(dclass & HN4_FLAG_VALID) || (dclass & HN4_FLAG_TOMBSTONE)) {
                /* Slot is physically free. Reserve it. */
                memset(slot_ptr->inline_buffer, 0, sizeof(slot_ptr->inline_buffer));
                slot_ptr->permissions = 0;
                slot_ptr->gravity_center = 0;
                slot_ptr->mass = 0;
                slot_ptr->seed_id = new_anc.seed_id; 
                slot_ptr->data_class = hn4_cpu_to_le64(HN4_FLAG_VALID); 
                slot_reserved = true;

                hn4_hal_spinlock_release(&vol->locking.l2_lock);
                break; 
            }
            
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
        }

        if (HN4_UNLIKELY(!slot_reserved)) return -HN4_ENOSPC;

        /* --- SUB-STEP 2B: Populate Metadata & Name --- */
        
        /* Prepare Name Logic */
        size_t path_len = _imp_strlen(path);
        const char* store_name = (path[0] == '/') ? path + 1 : path;
        size_t store_len = _imp_strlen(store_name);
        
        uint64_t dclass_accum = HN4_FLAG_VALID | HN4_VOL_ATOMIC;

        if (store_len <= HN4_INLINE_NAME_MAX) {
            /* Short Name: Inline */
            _imp_strncpy_safe((char*)new_anc.inline_buffer, store_name, HN4_INLINE_NAME_MAX);
        } 
        else {
            /* Long Name: Allocate Extension */
            hn4_addr_t ext_phys;
            if (hn4_alloc_horizon(vol, &ext_phys) == HN4_OK) {
                
                uint32_t bs = vol->vol_block_size;
                void* ext_buf = hn4_hal_mem_alloc(bs);
                
                if (ext_buf) {
                    _imp_memset(ext_buf, 0, bs);
                    hn4_extension_header_t* hdr = (hn4_extension_header_t*)ext_buf;
                    hdr->magic = hn4_cpu_to_le32(HN4_MAGIC_META);
                    hdr->type  = hn4_cpu_to_le32(HN4_EXT_TYPE_LONGNAME);
                    
                    /* Copy Full Path */
                    _imp_strncpy_safe((char*)hdr->payload, store_name, bs - sizeof(hn4_extension_header_t));
                    
                    const hn4_hal_caps_t* caps = hn4_hal_get_caps(vol->target_device);
                    uint32_t spb = bs / caps->logical_block_size;
                    
                    if (hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, ext_phys, ext_buf, spb) == HN4_OK) {
                        /* Link Extension */
                        uint64_t ext_ptr_le = hn4_cpu_to_le64(hn4_addr_to_u64(ext_phys));
                        _imp_memcpy(new_anc.inline_buffer, &ext_ptr_le, sizeof(uint64_t));
                        _imp_strncpy_safe((char*)(new_anc.inline_buffer + 8), store_name, 16);
                        
                        dclass_accum |= HN4_FLAG_EXTENDED;
                    }
                    hn4_hal_mem_free(ext_buf);
                }
            }
        }

        /* Directory Logic (mkdir vs open) */
        if (flags & HN4_O_DIRECTORY) {
            dclass_accum |= HN4_FLAG_IS_DIRECTORY;
            new_anc.mass = 0; /* Directories have no mass */
        }

        /* Finalize Anchor Fields */
        new_anc.permissions = hn4_cpu_to_le32(_mode_to_perms(mode));
        new_anc.data_class  = hn4_cpu_to_le64(dclass_accum);
        
        uint64_t now = hn4_hal_get_time_ns();
        new_anc.create_clock = hn4_cpu_to_le32((uint32_t)(now / 1000000000ULL));
        new_anc.mod_clock = hn4_cpu_to_le64(now);
        new_anc.write_gen = hn4_cpu_to_le32(1);
        new_anc.orbit_vector[0] = 1;

        /* --- SUB-STEP 2C: Persistence --- */
        if (HN4_UNLIKELY(hn4_write_anchor_atomic(vol, &new_anc) != HN4_OK)) {
            /* Rollback */
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
            ram_base[target_slot].data_class = 0; 
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
            return -HN4_EIO;
        }

        /* Update RAM Cache */
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        ram_base[target_slot] = new_anc;
        _imp_dcache_flush(&ram_base[target_slot], sizeof(hn4_anchor_t));
        hn4_hal_spinlock_release(&vol->locking.l2_lock);

        /* Populate Handle Context */
        lk.anchor = new_anc;
        lk.slot_idx = target_slot;
        lk.is_root = false;
    }
    else {
        return err;
    }

    /* ---------------------------------------------------------
     * CASE 3: ALLOCATE HANDLE
     * --------------------------------------------------------- */
    hn4_vfs_handle_t* fh = hn4_hal_mem_alloc(sizeof(hn4_vfs_handle_t));
    if (!fh) return -HN4_ENOMEM;

    fh->cached_anchor = lk.anchor;
    fh->anchor_idx = lk.slot_idx;
    fh->open_flags = flags;
    fh->session_perms = hn4_le32_to_cpu(lk.anchor.permissions); 
    fh->dirty = false;
    fh->is_directory = lk.is_root || (hn4_le64_to_cpu(lk.anchor.data_class) & HN4_FLAG_IS_DIRECTORY);
    fh->unlinked = false;
    fh->current_offset = 0;
    fh->cached_gen = hn4_le32_to_cpu(lk.anchor.write_gen);

    if (flags & HN4_O_APPEND) {
        fh->current_offset = hn4_le64_to_cpu(lk.anchor.mass);
    }

    atomic_fetch_add(&vol->health.ref_count, 1);

    *out = (hn4_handle_t*)fh;
    return 0;
}

static bool _is_write_mode(int flags) {
    int acc = flags & HN4_O_ACCMODE;
    return (acc == HN4_O_WRONLY || acc == HN4_O_RDWR);
}

hn4_ssize_t hn4_posix_read(hn4_volume_t* vol, hn4_handle_t* handle, void* buf, size_t count) {
     if (HN4_UNLIKELY(!vol || !handle || !buf)) return -HN4_EINVAL;
    
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    if (fh->is_directory) return -HN4_EISDIR;

    /* Check Read Permissions */
    if ((fh->open_flags & HN4_O_ACCMODE) == HN4_O_WRONLY) return -HN4_EBADF;

    uint32_t bs = vol->vol_block_size;
    if (bs == 0) return -HN4_EIO;
    uint32_t payload = HN4_BLOCK_PayloadSize(bs);
    if (payload == 0) return -HN4_EIO;

      if (vol->nano_cortex) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        hn4_anchor_t* live = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
        uint32_t live_gen = hn4_le32_to_cpu(live->write_gen);
        hn4_hal_spinlock_release(&vol->locking.l2_lock);

        if (live_gen != fh->cached_gen) {
            return -HN4_EIO; /* File modified externally */
        }
    }

    uint64_t size = hn4_le64_to_cpu(fh->cached_anchor.mass);

    if (fh->current_offset >= size) return 0;

    size_t to_read = count;
    if ((size - fh->current_offset) < to_read) {
        to_read = (size_t)(size - fh->current_offset);
    }
    if (to_read == 0) return 0;

    uint8_t* ptr = (uint8_t*)buf;
    size_t total = 0;

    void* io = hn4_hal_mem_alloc(bs);
    if (!io) return -HN4_ENOMEM;

    while (to_read > 0) {
        uint64_t b_idx = fh->current_offset / payload;
        uint32_t b_off = fh->current_offset % payload;
        uint32_t chunk = payload - b_off;
        if (chunk > to_read) chunk = to_read;

         hn4_result_t res = hn4_read_block_atomic(
             vol, 
             &fh->cached_anchor, 
             b_idx, 
             io, 
             bs, 
             fh->session_perms /* Added 6th argument */
         );

        if (HN4_LIKELY(res == HN4_OK || res == HN4_INFO_HEALED)) {
            _imp_memcpy(ptr, (uint8_t*)io + b_off, chunk);
        } else if (res == HN4_INFO_SPARSE) {
            _imp_memset(ptr, 0, chunk);
        } else {
            hn4_hal_mem_free(io);
            if (total > 0) return (hn4_ssize_t)total;
            
            return _map_err(res);
        }

        ptr += chunk;
        fh->current_offset += chunk;
        to_read -= chunk;
        total += chunk;
    }

    hn4_hal_mem_free(io);
    return (hn4_ssize_t)total;
}

hn4_ssize_t hn4_posix_write(hn4_volume_t* vol, hn4_handle_t* handle, const void* buf, size_t count) 
{
    if (!vol || !handle || !buf) return -HN4_EINVAL;
    
    /* 1. Volume State Checks */
    if (vol->read_only) return -HN4_EROFS;
    if (vol->sb.info.state_flags & HN4_VOL_PANIC) return -HN4_EIO;

    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    
    /* 2. Directory Safety */
    if (fh->is_directory) return -HN4_EISDIR;

    /* 3. Handle Mode Validation */
    int acc = fh->open_flags & HN4_O_ACCMODE;
    if (acc != HN4_O_WRONLY && acc != HN4_O_RDWR) return -HN4_EBADF;

    /* 
     * CRITICAL: SYNC FROM SOURCE OF TRUTH
     * Reload the anchor from the Nano-Cortex (RAM Cache) to ensure we have
     * the latest Write Generation and Mass before we attempt any IO.
     */
    if (vol->nano_cortex) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        _imp_memory_barrier();
        
        size_t max_slots = vol->cortex_size / sizeof(hn4_anchor_t);
        if (fh->anchor_idx < max_slots) {
            hn4_anchor_t* live = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
            
            if (live->seed_id.lo != fh->cached_anchor.seed_id.lo ||
                live->seed_id.hi != fh->cached_anchor.seed_id.hi) 
            {
                hn4_hal_spinlock_release(&vol->locking.l2_lock);
                return -HN4_EBADF; /* Handle is dead */
            }

            fh->cached_anchor = *live;
        }
        
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
    } else {
        /* If Cortex is missing in RW mode, we are in a critical failure state */
        return -HN4_EIO;
    }

    /* 4. Immutable/Permission Check (TOCTOU Defense) */
    uint32_t perms = hn4_le32_to_cpu(fh->cached_anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;

    /* 5. Append Logic */
    if (fh->open_flags & HN4_O_APPEND) {
        fh->current_offset = hn4_le64_to_cpu(fh->cached_anchor.mass);
    }

    /* 6. Size Limit Check */
    if (count > 0 && (UINT64_MAX - fh->current_offset < count)) return -HN4_EFBIG;

    /* 7. Geometry Setup */
    uint32_t bs = vol->vol_block_size;
    if (bs == 0) return -HN4_EIO;
    uint32_t payload = HN4_BLOCK_PayloadSize(bs);
    if (payload == 0) return -HN4_EIO;

    const uint8_t* ptr = (const uint8_t*)buf;
    size_t total_written = 0;
    size_t rem = count;

    /* 8. Allocation */
    void* io = hn4_hal_mem_alloc(bs);
    if (!io) return -HN4_ENOMEM;

    int ret_code = 0;

    /* 9. WRITE LOOP */
    while (rem > 0) {
        /* 
         * Re-check Append inside loop. 
         * If another thread updated Mass between our chunks, we must chase the tail.
         */
        if (fh->open_flags & HN4_O_APPEND) {
             fh->current_offset = hn4_le64_to_cpu(fh->cached_anchor.mass);
        }

        uint64_t b_idx = fh->current_offset / payload;
        
        if (b_idx > (UINT64_MAX / bs)) return -HN4_EFBIG;

        uint32_t b_off = fh->current_offset % payload;
        uint32_t chunk = payload - b_off;
        if (chunk > rem) chunk = rem;

        bool rmw_needed = (b_off > 0) || (chunk < payload);
        
        /* Always zero buffer to prevent data leaks in padding */
        memset(io, 0, bs);

        /* 
         * A. READ-MODIFY-WRITE (RMW) PATH
         * If we are writing a partial block, we must fetch the existing data.
         */
        if (rmw_needed) {
            hn4_result_t r = hn4_read_block_atomic(vol, &fh->cached_anchor, b_idx, io, bs, fh->session_perms);
            
            /* 
             * Sparse/Not Found is acceptable for RMW (treat as zeros).
             * Any other error is fatal.
             */
            if (HN4_UNLIKELY(r != HN4_OK && r != HN4_INFO_SPARSE && r != HN4_ERR_NOT_FOUND && r != HN4_INFO_HEALED)) {
                ret_code = _map_err(r);
                goto cleanup;
            }
        }

        /* B. OVERLAY NEW DATA */
        /* hn4_read_block_atomic outputs payload directly into io. We update it here. */
        memcpy((uint8_t*)io + b_off, ptr, chunk);

        /* 
         * C. ATOMIC WRITE (THE SHADOW HOP)
         */
        uint32_t valid_len = payload;
        
        /* If this is the last block logical index, determine valid byte count */
        if (fh->current_offset + chunk > hn4_le64_to_cpu(fh->cached_anchor.mass)) {
             valid_len = b_off + chunk;
        }

        hn4_anchor_t* target_anchor = &fh->cached_anchor; /* Default to local */
        
        /* If Cortex exists, point directly to global memory to enable CAS concurrency */
        if (vol->nano_cortex && fh->anchor_idx < (vol->cortex_size / sizeof(hn4_anchor_t))) {
            target_anchor = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
        }

        /* Execute Atomic Write */
        hn4_result_t w = hn4_write_block_atomic(vol, target_anchor, b_idx, io, valid_len, fh->session_perms);
        
        if (HN4_UNLIKELY(w < 0)) { /* Error is negative */
            ret_code = _map_err(w);
            goto cleanup;
        }

        /* 
         * D. SYNC LOCAL HANDLE
         * The global anchor has been updated by hn4_write_block_atomic (Generation/Mass).
         * We must refresh our local handle cache to stay consistent.
         */
        if (vol->nano_cortex) {
            uint32_t target_gen = hn4_le32_to_cpu(target_anchor->write_gen);
            if (target_gen < fh->cached_gen) {
                ret_code = -HN4_EIO;
                goto cleanup;
            }
            fh->cached_anchor = *target_anchor;
            fh->cached_gen = target_gen;
        }
        /* 
         * Note: If no Cortex (Direct-IO mode), target_anchor was already &fh->cached_anchor,
         * so it was updated in-place by the write function.
         */

        /* E. Advance Cursors */
        ptr += chunk;
        fh->current_offset += chunk;
        rem -= chunk;
        total_written += chunk;
        fh->dirty = true; /* Mark handle dirty for close() logic */
    }

cleanup:
    hn4_hal_mem_free(io);
    
    /* If we wrote anything, return that count (short write), otherwise return error */
    if (total_written > 0) return (hn4_ssize_t)total_written;
    return (hn4_ssize_t)ret_code;
}


hn4_off_t hn4_posix_lseek(hn4_volume_t* vol, hn4_handle_t* handle, hn4_off_t offset, int whence) {
    if (!vol || !handle) return -HN4_EINVAL;
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    if (fh->is_directory) return -HN4_EISDIR;

    if (vol->nano_cortex) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        hn4_anchor_t* live = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
    
        /* Only update if ID matches (prevent UAF) */
        if (live->seed_id.lo == fh->cached_anchor.seed_id.lo) {
            fh->cached_anchor.mass = live->mass;
        }
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
    }
    uint64_t size = hn4_le64_to_cpu(fh->cached_anchor.mass);
    int64_t current = (int64_t)fh->current_offset;
    int64_t target = 0;

    switch (whence) {
        case HN4_SEEK_SET:
            if (offset < 0) return -HN4_EINVAL;
            target = offset;
            break;
        case HN4_SEEK_CUR:
            if (!_imp_safe_add_signed(current, offset, &target)) return -HN4_EOVERFLOW;
            break;
        case HN4_SEEK_END:
            if (!_imp_safe_add_signed((int64_t)size, offset, &target)) return -HN4_EOVERFLOW;
            break;
        default: 
            return -HN4_EINVAL;
    }

    if (target < 0) return -HN4_EINVAL;
    fh->current_offset = (uint64_t)target;
    return target;
}

int hn4_posix_readdir(hn4_volume_t* vol, const char* path, void* buf, 
                      int (*filler)(void*, const char*, const hn4_stat_t*, hn4_off_t)) 
{
    /* 1. Path Resolution */
    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);
    if (err != 0) return err;

    /* 2. Directory Semantics Check */
    uint64_t root_dclass = hn4_le64_to_cpu(lk.anchor.data_class);
    bool is_dir = lk.is_root || (root_dclass & HN4_FLAG_IS_DIRECTORY);

    if (!is_dir) return -HN4_ENOTDIR;

    /* 3. Emit Standard Entries (.) and (..) */
    if (filler(buf, ".", NULL, 0)) return 0;
    if (filler(buf, "..", NULL, 0)) return 0;

    /* HN4 has a Flat Namespace: Only Root contains files */
    if (!lk.is_root) return 0;

    if (!vol->nano_cortex) return -HN4_EIO;

    /* 
     * 4. SNAPSHOT ITERATION
     * Iterate the Cortex array in chunks. 
     * We copy metadata under lock, release lock, then call the filler.
     */
    uint64_t total_count = vol->cortex_size / sizeof(hn4_anchor_t);
    uint64_t cursor = 0;
    
    #define HN4_READDIR_BATCH 64
    
    typedef struct {
        char name[HN4_INLINE_NAME_MAX + 1];
        hn4_stat_t st;
        bool valid;
    } dir_snap_t;
    
    dir_snap_t* batch = hn4_hal_mem_alloc(sizeof(dir_snap_t) * HN4_READDIR_BATCH);
    if (!batch) return -HN4_ENOMEM;

    /* Ensure we free this before returning! */
    
    while (cursor < total_count) {
        int items_in_batch = 0;

        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        _imp_memory_barrier();
        
        hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
        
        /* Fill batch */
        for (; cursor < total_count && items_in_batch < HN4_READDIR_BATCH; cursor++) {
            hn4_anchor_t* a = &anchors[cursor];
            
            uint64_t dclass = _imp_atomic_load_u64(&a->data_class);
            dclass = hn4_le64_to_cpu(dclass);

            /* Skip Invalid or Deleted entries */
            if (!(dclass & HN4_FLAG_VALID) || (dclass & HN4_FLAG_TOMBSTONE)) {
                continue; 
            }

            /* Valid Entry Found - Copy to Snapshot */
            dir_snap_t* snap = &batch[items_in_batch];
            
            /* Extract Name */
            if (dclass & HN4_FLAG_EXTENDED) {
            hn4_anchor_t temp_anchor = *a;
            hn4_hal_spinlock_release(&vol->locking.l2_lock);
    
            hn4_ns_get_name(vol, &temp_anchor, snap->name, HN4_INLINE_NAME_MAX + 1);
    
            /* Re-acquire lock to continue iteration */
            hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
        } else {
            _imp_memcpy(snap->name, a->inline_buffer, HN4_INLINE_NAME_MAX);
            snap->name[HN4_INLINE_NAME_MAX] = '\0';
        }
            
            /* Skip empty names (shouldn't happen for valid files, but safe guard) */
            if (snap->name[0] == '\0') continue;

            /* Populate Stat */
            _imp_memset(&snap->st, 0, sizeof(hn4_stat_t));
            
            /* Inode 0 is reserved; offset by 1 */
            snap->st.st_ino = (hn4_ino_t)(cursor + 1);
            
            snap->st.st_mode = _perms_to_mode(hn4_le32_to_cpu(a->permissions));
            if (dclass & HN4_FLAG_IS_DIRECTORY) snap->st.st_mode |= HN4_S_IFDIR;
            else snap->st.st_mode |= HN4_S_IFREG;
            
            snap->st.st_size = hn4_le64_to_cpu(a->mass);
            
            if (vol->vol_block_size > 0) {
                snap->st.st_blksize = vol->vol_block_size;
                snap->st.st_blocks = (snap->st.st_size + vol->vol_block_size - 1) / vol->vol_block_size;
            }

            snap->st.st_mtime = hn4_le64_to_cpu(a->mod_clock) / 1000000000ULL;
            snap->st.st_ctime = snap->st.st_mtime;
            
            snap->valid = true;
            items_in_batch++;
        }
        
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        /* --- CRITICAL SECTION END --- */

        /* 
         * Safe Callback Phase
         * Call the VFS filler without holding internal locks.
         */
        for (int i = 0; i < items_in_batch; i++) {
            if (batch[i].valid) {
                if (filler(buf, batch[i].name, &batch[i].st, 0)) {
                    hn4_hal_mem_free(batch);
                    return 0; 
                }
            }
        }
    }

    hn4_hal_mem_free(batch);
    return 0;
}


int hn4_posix_unlink(hn4_volume_t* vol, const char* path) {
    if (!vol || !path) return -HN4_EINVAL;
    if (vol->read_only) return -HN4_EROFS;

    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);
    if (err != 0) return err;
    if (lk.is_root) return -HN4_EBUSY; /* Root is busy */
    if (hn4_le64_to_cpu(lk.anchor.data_class) & HN4_FLAG_IS_DIRECTORY) return -HN4_EISDIR;

    uint32_t perms = hn4_le32_to_cpu(lk.anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;

    uint64_t dclass = hn4_le64_to_cpu(lk.anchor.data_class);
    lk.anchor.data_class = hn4_cpu_to_le64(dclass | HN4_FLAG_TOMBSTONE);
    lk.anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    /* Atomic Tombstone Commit */
    if (hn4_write_anchor_atomic(vol, &lk.anchor) != HN4_OK) return -HN4_EIO;

    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    ((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx] = lk.anchor;
    _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx], sizeof(hn4_anchor_t));
    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    return 0;
}

int hn4_posix_rename(hn4_volume_t* vol, const char* oldpath, const char* newpath) {
    if (!vol || !oldpath || !newpath) return -HN4_EINVAL;
    if (vol->read_only) return -HN4_EROFS;

    hn4_lookup_ctx_t src;
    if (_resolve_path(vol, oldpath, &src) != 0) return -HN4_ENOENT;
    if (src.is_root) return -HN4_EINVAL;

    uint32_t perms = hn4_le32_to_cpu(src.anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;

    const char* p = newpath;
    while (*p == '/') p++; /* Skip leading root slash */

    /* Check for directory separators in filename */
    for (const char* c = p; *c; c++) {
        if (*c == '/') return -HN4_EINVAL; /* Flat namespace violation */
    }
    if (_imp_strlen(p) >= HN4_INLINE_NAME_MAX) return -HN4_ENAMETOOLONG;

    hn4_lookup_ctx_t dst;
    if (_resolve_path(vol, newpath, &dst) == 0) {
        if (dst.is_root) return -HN4_EEXIST;
        uint32_t dp = hn4_le32_to_cpu(dst.anchor.permissions);
        if (dp & HN4_PERM_IMMUTABLE) return -HN4_EPERM;
        hn4_chronicle_append(vol->target_device, vol, HN4_CHRONICLE_OP_FORK, 
                     hn4_addr_from_u64(src.slot_idx), 
                     hn4_addr_from_u64(dst.slot_idx), 
                     0);

/* Perform Swap in Memory */
if (dst.found) {
    /* Mark destination as Tombstone */
    dst.anchor.data_class |= hn4_cpu_to_le64(HN4_FLAG_TOMBSTONE);
    hn4_write_anchor_atomic(vol, &dst.anchor);
}

/* Update Source Name */
_imp_memset(src.anchor.inline_buffer, 0, HN4_INLINE_NAME_MAX);
_imp_strncpy_safe((char*)src.anchor.inline_buffer, p, HN4_INLINE_NAME_MAX);
hn4_write_anchor_atomic(vol, &src.anchor);
    }

    if (vol->nano_cortex) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        src.anchor = ((hn4_anchor_t*)vol->nano_cortex)[src.slot_idx];
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
    }

    _imp_memset(src.anchor.inline_buffer, 0, HN4_INLINE_NAME_MAX);
    _imp_strncpy_safe((char*)src.anchor.inline_buffer, p, HN4_INLINE_NAME_MAX);
    
    src.anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    if (hn4_write_anchor_atomic(vol, &src.anchor) != HN4_OK) return -HN4_EIO;

    hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
    
   /* Verify we still own the slot before overwriting RAM */
    hn4_anchor_t* current_slot = &((hn4_anchor_t*)vol->nano_cortex)[src.slot_idx];
    if (current_slot->seed_id.lo == src.anchor.seed_id.lo && 
        current_slot->seed_id.hi == src.anchor.seed_id.hi) 
    {
        *current_slot = src.anchor;
        _imp_dcache_flush(current_slot, sizeof(hn4_anchor_t));
    }
    else {
        hn4_hal_spinlock_release(&vol->locking.l2_lock);
        
        uint64_t new_slot_idx;
        hn4_result_t find_res = _ns_scan_cortex_slot(vol, hn4_le128_to_cpu(src.anchor.seed_id), NULL, &new_slot_idx);
        
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        
        if (find_res == HN4_OK) {
            /* Found the new location, update it */
            ((hn4_anchor_t*)vol->nano_cortex)[new_slot_idx] = src.anchor;
            _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[new_slot_idx], sizeof(hn4_anchor_t));
        } else {
            /* Total desync: Mark dirty to force eventual reload */
            atomic_fetch_or(&vol->sb.info.state_flags, HN4_VOL_DIRTY);
        }
    }

    hn4_hal_spinlock_release(&vol->locking.l2_lock);

    return 0;
}

int hn4_posix_close(hn4_volume_t* vol, hn4_handle_t* handle) {
    if (!vol || !handle) return -HN4_EINVAL;
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    
    int ret = 0;
    if (fh->dirty && !vol->read_only && !fh->is_directory) {
        hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
        hn4_anchor_t* live = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
        uint64_t dclass = _imp_atomic_load_u64(&live->data_class);
        uint32_t live_gen = hn4_le32_to_cpu(live->write_gen);
        hn4_hal_spinlock_release(&vol->locking.l2_lock);

        dclass = hn4_le64_to_cpu(dclass);
        
        if ((dclass & HN4_FLAG_TOMBSTONE) || (live_gen > fh->cached_gen)) {
            ret = -HN4_EIO; 
        } else {
            if (hn4_write_anchor_atomic(vol, &fh->cached_anchor) != HN4_OK) {
                ret = -HN4_EIO;
            } else {
                hn4_hal_spinlock_acquire(&vol->locking.l2_lock);
                ((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx] = fh->cached_anchor;
                _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx], sizeof(hn4_anchor_t));
                hn4_hal_spinlock_release(&vol->locking.l2_lock);
                fh->dirty = false;
            }
        }
    }
    
    /* Release Volume Reference */
    atomic_fetch_sub(&vol->health.ref_count, 1);

    hn4_hal_mem_free(fh);
    return ret;
}
