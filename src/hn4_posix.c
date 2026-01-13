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
    for (i = 0; i < n - 1 && src[i] != '\0'; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0'; /* Zero-fill rest */
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
#define HN4_INLINE_NAME_MAX 28
#define HN4_FLAG_IS_DIRECTORY  (1ULL << 63)

typedef struct {
    hn4_anchor_t    cached_anchor;
    uint64_t        current_offset;
    uint64_t        anchor_idx;
    uint32_t        cached_gen; /* Generation Tracking */
    int             open_flags;
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
    if (!path) return -HN4_EINVAL;
    if (!vol->nano_cortex) return -HN4_EIO;
    
    _imp_memset(ctx, 0, sizeof(hn4_lookup_ctx_t));
    
    size_t len = _imp_strlen(path);
    if (len == 0) return -HN4_ENOENT;
    if (len >= HN4_MAX_PATH) return -HN4_ENAMETOOLONG;

    /* Root Detection */
    bool is_root = false;
    if (_imp_strcmp(path, "/") == 0 || _imp_strcmp(path, ".") == 0 || _imp_strcmp(path, "..") == 0) is_root = true;
    else {
        const char* s = path;
        while (*s == '/') s++;
        if (*s == '\0') is_root = true;
    }

    if (is_root) {
        ctx->is_root = true;
        ctx->found = true;
        return 0;
    }

    const char* p = path;
    while (*p == '/') p++;

    size_t name_len = 0;
    const char* end = p;
    while (*end != '\0' && *end != '/') {
        name_len++;
        end++;
    }

    const char* check = end;
    while (*check == '/') check++;
    if (*check != '\0') return -HN4_ENOENT; /* Subdirs forbidden */
    
    if (name_len == 0) return -HN4_ENOENT;
    if (*end == '/') ctx->trailing_slash = true; /* Mark intent */

    if (name_len >= HN4_INLINE_NAME_MAX) return -HN4_ENAMETOOLONG;
    _imp_memcpy(ctx->name, p, name_len);
    ctx->name[name_len] = '\0';

    hn4_hal_spinlock_acquire(&vol->l2_lock);
    _imp_memory_barrier();

    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    size_t count = vol->cortex_size / sizeof(hn4_anchor_t);

    for (size_t i = 0; i < count; i++) {
        hn4_anchor_t* a = &anchors[i];
        
        uint64_t dclass = _imp_atomic_load_u64(&a->data_class);
        dclass = hn4_le64_to_cpu(dclass);

        if (!(dclass & HN4_FLAG_VALID)) continue;
        if (dclass & HN4_FLAG_TOMBSTONE) continue;

        char tmp[HN4_INLINE_NAME_MAX + 1];
        _imp_memcpy(tmp, a->inline_buffer, HN4_INLINE_NAME_MAX);
        tmp[HN4_INLINE_NAME_MAX] = '\0';

        if (_imp_strcmp(tmp, ctx->name) == 0) {
            ctx->anchor = *a;
            ctx->slot_idx = i;
            ctx->found = true;
            hn4_hal_spinlock_release(&vol->l2_lock);
            
            /* Directory Flag Check */
            if (ctx->trailing_slash) {
                if (!(dclass & HN4_FLAG_IS_DIRECTORY)) return -HN4_ENOTDIR;
            }
            return 0;
        }
    }

    hn4_hal_spinlock_release(&vol->l2_lock);
    return -HN4_ENOENT;
}

static int _find_free_slot(hn4_volume_t* vol, uint64_t* slot_idx) {
    hn4_hal_spinlock_acquire(&vol->l2_lock);
    
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    uint64_t count = vol->cortex_size / sizeof(hn4_anchor_t);
    uint64_t i = vol->cortex_search_head;
    uint64_t checked = 0;

    while (checked < count) {
        if (i >= count) i = 0;
        
        uint64_t dclass = _imp_atomic_load_u64(&anchors[i].data_class);
        dclass = hn4_le64_to_cpu(dclass);

        if (!(dclass & HN4_FLAG_VALID) || (dclass & HN4_FLAG_TOMBSTONE)) {
            *slot_idx = i;
            vol->cortex_search_head = i + 1;
            hn4_hal_spinlock_release(&vol->l2_lock);
            return 0;
        }
        i++;
        checked++;
    }

    hn4_hal_spinlock_release(&vol->l2_lock);
    return -HN4_ENOSPC;
}

/* =========================================================================
 * 4. API IMPLEMENTATION
 * ========================================================================= */

int hn4_posix_open(hn4_volume_t* vol, const char* path, int flags, hn4_mode_t mode, hn4_handle_t** out) {
    if (!vol || !path || !out) return -HN4_EINVAL;

    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);

    /* CASE 1: TARGET EXISTS */
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
            
            /* Must check Write/Append permission BEFORE checking Immutable */
            if (!(perms & (HN4_PERM_WRITE | HN4_PERM_APPEND))) return -HN4_EACCES;
            
            /* If writing, Immutable blocks it */
            if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;
        } else {
            /* Read Access Required */
            if (!(perms & HN4_PERM_READ)) return -HN4_EACCES;
        }

        /* O_TRUNC Logic (Files only, Write access already confirmed) */
        if (flags & HN4_O_TRUNC) {
            if ((flags & HN4_O_ACCMODE) == HN4_O_RDONLY) return -HN4_EINVAL;
            if (is_dir) return -HN4_EISDIR;

            lk.anchor.mass = 0;
            lk.anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());
            
            /* Increment generation to invalidate old blocks */
            uint32_t g = hn4_le32_to_cpu(lk.anchor.write_gen);
            lk.anchor.write_gen = hn4_cpu_to_le32(g + 1);

            /* Atomic Commit */
            if (hn4_write_anchor_atomic(vol, &lk.anchor) != HN4_OK) return -HN4_EIO;
            
            /* Update RAM Cache */
            hn4_hal_spinlock_acquire(&vol->l2_lock);
            ((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx] = lk.anchor;
            _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx], sizeof(hn4_anchor_t));
            hn4_hal_spinlock_release(&vol->l2_lock);
        }
    }
    /* CASE 2: TARGET MISSING (CREATE) */
    else if (err == -HN4_ENOENT) {
        if (!(flags & HN4_O_CREAT)) return -HN4_ENOENT;
        if (vol->read_only) return -HN4_EROFS;

        uint64_t slot;
        if (_find_free_slot(vol, &slot) != 0) return -HN4_ENOSPC;

        hn4_anchor_t new_anc;
        _imp_memset(&new_anc, 0, sizeof(hn4_anchor_t));

        new_anc.seed_id.lo = hn4_hal_get_random_u64();
        new_anc.seed_id.hi = hn4_hal_get_random_u64();
        new_anc.public_id  = new_anc.seed_id;

        /* Safe Copy Name */
        _imp_strncpy_safe((char*)new_anc.inline_buffer, lk.name, HN4_INLINE_NAME_MAX);

        new_anc.permissions = hn4_cpu_to_le32(_mode_to_perms(mode));
        
        uint64_t now = hn4_hal_get_time_ns();
        new_anc.create_clock = hn4_cpu_to_le32((uint32_t)(now / 1000000000ULL));
        new_anc.mod_clock = hn4_cpu_to_le64(now);
        new_anc.write_gen = hn4_cpu_to_le32(1);
        new_anc.orbit_vector[0] = 1;
        
        /* Set Flags (Dir/File) */
        uint64_t dclass = HN4_FLAG_VALID | HN4_VOL_ATOMIC;
        if (flags & HN4_O_DIRECTORY) dclass |= HN4_FLAG_IS_DIRECTORY;
        new_anc.data_class = hn4_cpu_to_le64(dclass);

        /* Write to Disk */
        if (hn4_write_anchor_atomic(vol, &new_anc) != HN4_OK) return -HN4_EIO;

        /* Write to RAM */
        hn4_hal_spinlock_acquire(&vol->l2_lock);
        ((hn4_anchor_t*)vol->nano_cortex)[slot] = new_anc;
        _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[slot], sizeof(hn4_anchor_t));
        hn4_hal_spinlock_release(&vol->l2_lock);

        /* Update Context for Handle */
        lk.anchor = new_anc;
        lk.slot_idx = slot;
        lk.is_root = false;
    }
    else {
        /* Other error (Name too long, etc) */
        return err;
    }

    /* Allocate Handle */
    hn4_vfs_handle_t* fh = hn4_hal_mem_alloc(sizeof(hn4_vfs_handle_t));
    if (!fh) return -HN4_ENOMEM;

    fh->cached_anchor = lk.anchor;
    fh->anchor_idx = lk.slot_idx;
    fh->open_flags = flags;
    fh->dirty = false;
    fh->is_directory = lk.is_root || (hn4_le64_to_cpu(lk.anchor.data_class) & HN4_FLAG_IS_DIRECTORY);
    fh->unlinked = false;
    fh->current_offset = 0;
    fh->cached_gen = hn4_le32_to_cpu(lk.anchor.write_gen);

    if (flags & HN4_O_APPEND) {
        fh->current_offset = hn4_le64_to_cpu(lk.anchor.mass);
    }

    *out = (hn4_handle_t*)fh;
    return 0;
}

static bool _is_write_mode(int flags) {
    int acc = flags & HN4_O_ACCMODE;
    return (acc == HN4_O_WRONLY || acc == HN4_O_RDWR);
}

hn4_ssize_t hn4_posix_read(hn4_volume_t* vol, hn4_handle_t* handle, void* buf, size_t count) {
    if (!vol || !handle || !buf) return -HN4_EINVAL;
    
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    if (fh->is_directory) return -HN4_EISDIR;

    /* Check Read Permissions */
    if ((fh->open_flags & HN4_O_ACCMODE) == HN4_O_WRONLY) return -HN4_EBADF;

    uint32_t bs = vol->vol_block_size;
    if (bs == 0) return -HN4_EIO;
    uint32_t payload = HN4_BLOCK_PayloadSize(bs);
    if (payload == 0) return -HN4_EIO;

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

        hn4_result_t res = hn4_read_block_atomic(vol, &fh->cached_anchor, b_idx, io, bs);

        if (res == HN4_OK || res == HN4_INFO_HEALED) {
            _imp_memcpy(ptr, (uint8_t*)io + b_off, chunk);
        } else if (res == HN4_INFO_SPARSE || res == HN4_ERR_NOT_FOUND) {
            _imp_memset(ptr, 0, chunk);
        } else {
            hn4_hal_mem_free(io);
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

hn4_ssize_t hn4_posix_write(hn4_volume_t* vol, hn4_handle_t* handle, const void* buf, size_t count) {
    if (!vol || !handle || !buf) return -HN4_EINVAL;
    if (vol->read_only) return -HN4_EROFS;

    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    if (fh->is_directory) return -HN4_EISDIR;

    /* Validate Write Mode */
    int acc = fh->open_flags & HN4_O_ACCMODE;
    if (acc != HN4_O_WRONLY && acc != HN4_O_RDWR) return -HN4_EBADF;

    /* TOCTOU Check: Re-read perms */
    uint32_t perms = hn4_le32_to_cpu(fh->cached_anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;

    /* Handle Append */
    if (fh->open_flags & HN4_O_APPEND) {
        fh->current_offset = hn4_le64_to_cpu(fh->cached_anchor.mass);
    }

    if (count > 0 && (UINT64_MAX - fh->current_offset < count)) return -HN4_EFBIG;

    uint32_t bs = vol->vol_block_size;
    if (bs == 0) return -HN4_EIO;
    uint32_t payload = HN4_BLOCK_PayloadSize(bs);
    if (payload == 0) return -HN4_EIO;

    const uint8_t* ptr = (const uint8_t*)buf;
    size_t total = 0;
    size_t rem = count;

    void* io = hn4_hal_mem_alloc(bs);
    if (!io) return -HN4_ENOMEM;

    while (rem > 0) {
        /* Re-check Append inside loop to reduce race window */
        if (fh->open_flags & HN4_O_APPEND) {
             fh->current_offset = hn4_le64_to_cpu(fh->cached_anchor.mass);
        }

        uint64_t b_idx = fh->current_offset / payload;
        uint32_t b_off = fh->current_offset % payload;
        uint32_t chunk = payload - b_off;
        if (chunk > rem) chunk = rem;

        bool rmw = (b_off > 0) || (chunk < payload);
        _imp_memset(io, 0, bs);

        if (rmw) {
            hn4_result_t r = hn4_read_block_atomic(vol, &fh->cached_anchor, b_idx, io, bs);
            /* Treat not found as sparse (zeros) for RMW */
            if (r != HN4_OK && r != HN4_INFO_SPARSE && r != HN4_ERR_NOT_FOUND) {
                hn4_hal_mem_free(io);
                return _map_err(r);
            }
        }

        _imp_memcpy((uint8_t*)io + b_off, ptr, chunk);

        hn4_result_t w = hn4_write_block_atomic(vol, &fh->cached_anchor, b_idx, io, payload);
        if (w != HN4_OK) {
            hn4_hal_mem_free(io);
            return _map_err(w);
        }

        ptr += chunk;
        fh->current_offset += chunk;
        rem -= chunk;
        total += chunk;
        fh->dirty = true;

        /* Update Mass Atomically */
        uint64_t mass = hn4_le64_to_cpu(fh->cached_anchor.mass);
        if (fh->current_offset > mass) {
            fh->cached_anchor.mass = hn4_cpu_to_le64(fh->current_offset);
        }
        
        uint64_t now = hn4_hal_get_time_ns();
        fh->cached_anchor.mod_clock = hn4_cpu_to_le64(now);

        /* Update RAM Cache */
        hn4_hal_spinlock_acquire(&vol->l2_lock);
        ((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx] = fh->cached_anchor;
        _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx], sizeof(hn4_anchor_t));
        hn4_hal_spinlock_release(&vol->l2_lock);
    }

    hn4_hal_mem_free(io);
    return (hn4_ssize_t)total;
}



hn4_off_t hn4_posix_lseek(hn4_volume_t* vol, hn4_handle_t* handle, hn4_off_t offset, int whence) {
    if (!vol || !handle) return -HN4_EINVAL;
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    if (fh->is_directory) return -HN4_EISDIR;

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
    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);
    if (err != 0) return err;

    /* Directory Semantics Check */
    uint64_t root_dclass = hn4_le64_to_cpu(lk.anchor.data_class);
    bool is_dir = lk.is_root || (root_dclass & HN4_FLAG_IS_DIRECTORY);

    if (!is_dir) return -HN4_ENOTDIR;

    /* Emit Standard Entries */
    if (filler(buf, ".", NULL, 0)) return 0;
    if (filler(buf, "..", NULL, 0)) return 0;

    /* Flat Namespace: Only Root contains files */
    if (!lk.is_root) return 0;

    if (!vol->nano_cortex) return -HN4_EIO;

    hn4_hal_spinlock_acquire(&vol->l2_lock);
    _imp_memory_barrier();

    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    size_t count = vol->cortex_size / sizeof(hn4_anchor_t);

    for (size_t i = 0; i < count; i++) {
        hn4_anchor_t* a = &anchors[i];
        
        uint64_t dclass = _imp_atomic_load_u64(&a->data_class);
        dclass = hn4_le64_to_cpu(dclass);
        
        if (!(dclass & HN4_FLAG_VALID)) continue;
        if (dclass & HN4_FLAG_TOMBSTONE) continue;

        char name[HN4_INLINE_NAME_MAX + 1];
        _imp_memcpy(name, a->inline_buffer, HN4_INLINE_NAME_MAX);
        name[HN4_INLINE_NAME_MAX] = '\0';
        
        if (name[0] != '\0') {
            hn4_stat_t st;
            _imp_memset(&st, 0, sizeof(st));
            
            /* Inode 0 is reserved; offset by 1 */
            st.st_ino = (hn4_ino_t)(i + 1);
            
            st.st_mode = _perms_to_mode(hn4_le32_to_cpu(a->permissions));
            
            if (dclass & HN4_FLAG_IS_DIRECTORY) st.st_mode |= HN4_S_IFDIR;
            else st.st_mode |= HN4_S_IFREG;
            
            st.st_size = hn4_le64_to_cpu(a->mass);
            
            if (vol->vol_block_size > 0) {
                st.st_blksize = vol->vol_block_size;
                st.st_blocks = (st.st_size + vol->vol_block_size - 1) / vol->vol_block_size;
            }

            st.st_mtime = hn4_le64_to_cpu(a->mod_clock) / 1000000000ULL;
            st.st_ctime = st.st_mtime;

            /* Now valid because filler returns int */
            if (filler(buf, name, &st, 0)) break;
        }
    }

    hn4_hal_spinlock_release(&vol->l2_lock);
    return 0;
}


int hn4_posix_unlink(hn4_volume_t* vol, const char* path) {
    if (!vol || !path) return -HN4_EINVAL;
    if (vol->read_only) return -HN4_EROFS;

    hn4_lookup_ctx_t lk;
    int err = _resolve_path(vol, path, &lk);
    if (err != 0) return err;
    if (lk.is_root) return -HN4_EISDIR;

    uint32_t perms = hn4_le32_to_cpu(lk.anchor.permissions);
    if (perms & HN4_PERM_IMMUTABLE) return -HN4_EPERM;

    uint64_t dclass = hn4_le64_to_cpu(lk.anchor.data_class);
    lk.anchor.data_class = hn4_cpu_to_le64(dclass | HN4_FLAG_TOMBSTONE);
    lk.anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    /* Atomic Tombstone Commit */
    if (hn4_write_anchor_atomic(vol, &lk.anchor) != HN4_OK) return -HN4_EIO;

    hn4_hal_spinlock_acquire(&vol->l2_lock);
    ((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx] = lk.anchor;
    _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[lk.slot_idx], sizeof(hn4_anchor_t));
    hn4_hal_spinlock_release(&vol->l2_lock);

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
    while (*p == '/') p++;
    const char* check = p;
    while (*check) {
        if (*check == '/') return -HN4_EINVAL;
        check++;
    }
    if (_imp_strlen(p) >= HN4_INLINE_NAME_MAX) return -HN4_ENAMETOOLONG;

    hn4_lookup_ctx_t dst;
    if (_resolve_path(vol, newpath, &dst) == 0) {
        if (dst.is_root) return -HN4_EEXIST;
        uint32_t dp = hn4_le32_to_cpu(dst.anchor.permissions);
        if (dp & HN4_PERM_IMMUTABLE) return -HN4_EPERM;
        hn4_posix_unlink(vol, newpath);
    }

    _imp_memset(src.anchor.inline_buffer, 0, HN4_INLINE_NAME_MAX);
    _imp_strncpy_safe((char*)src.anchor.inline_buffer, p, HN4_INLINE_NAME_MAX);
    
    src.anchor.mod_clock = hn4_cpu_to_le64(hn4_hal_get_time_ns());

    if (hn4_write_anchor_atomic(vol, &src.anchor) != HN4_OK) return -HN4_EIO;

    hn4_hal_spinlock_acquire(&vol->l2_lock);
    ((hn4_anchor_t*)vol->nano_cortex)[src.slot_idx] = src.anchor;
    _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[src.slot_idx], sizeof(hn4_anchor_t));
    hn4_hal_spinlock_release(&vol->l2_lock);

    return 0;
}

hn4_posix_close(hn4_volume_t* vol, hn4_handle_t* handle) {
    if (!vol || !handle) return -HN4_EINVAL;
    hn4_vfs_handle_t* fh = (hn4_vfs_handle_t*)handle;
    
    int ret = 0;
    if (fh->dirty && !vol->read_only && !fh->is_directory) {
        hn4_hal_spinlock_acquire(&vol->l2_lock);
        hn4_anchor_t* live = &((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx];
        uint64_t dclass = _imp_atomic_load_u64(&live->data_class);
        uint32_t live_gen = hn4_le32_to_cpu(live->write_gen);
        hn4_hal_spinlock_release(&vol->l2_lock);

        dclass = hn4_le64_to_cpu(dclass);
        
        if ((dclass & HN4_FLAG_TOMBSTONE) || (live_gen > fh->cached_gen)) {
            ret = -HN4_EIO; 
        } else {
            if (hn4_write_anchor_atomic(vol, &fh->cached_anchor) != HN4_OK) {
                ret = -HN4_EIO;
            } else {
                hn4_hal_spinlock_acquire(&vol->l2_lock);
                ((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx] = fh->cached_anchor;
                _imp_dcache_flush(&((hn4_anchor_t*)vol->nano_cortex)[fh->anchor_idx], sizeof(hn4_anchor_t));
                hn4_hal_spinlock_release(&vol->l2_lock);
                fh->dirty = false;
            }
        }
    }
    
    hn4_hal_mem_free(fh);
    return ret;
}
