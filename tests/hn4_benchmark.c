/*
 * hn4_benchmark.c
 * HN4 Core Performance Metrics
 * STATUS: OPTIMIZED / LEAK-FREE
 * COPYRIGHT: (c) 2026 The Hydra-Nexus Team.
 */

#include "hn4_benchmark.h"
#include "hn4.h"
#include "hn4_hal.h"
#include "hn4_addr.h"
#include "hn4_errors.h"
#include "hn4_constants.h"
#include "hn4_endians.h"
#include "hn4_anchor.h" 
#include "hn4_tensor.h"
#include "hn4_compress.h"
#include "hn4_swizzle.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Safe Duration Clamp (Prevents Div-by-Zero) */
#define HN4_SAFE_DURATION(d) ((d) < 1e-9 ? 1e-9 : (d))
#define HN4_SAFE_DIV(n, d) ((d) < 1e-9 ? 0.0 : (double)(n)/(d))

/* =========================================================================
 * HIGH-RESOLUTION TIMER
 * ========================================================================= */

/* Add this block at the very top of your file (hn4_benchmark.c) */
#if defined(_WIN32)
    #include <windows.h>
#endif

/* Then your function follows... */
static double _get_time_sec(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&frequency);
        init = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    struct timespec ts;
    /* Use MONOTONIC to measure wall-clock elapsed time, not CPU time */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

/* =========================================================================
 * MOCKING INFRASTRUCTURE
 * ========================================================================= */

typedef struct {
    hn4_hal_caps_t caps;
    uint8_t*       mmio_base;
    void*          driver_ctx;
    uint8_t        _reserved[64];
} mock_hal_device_t;

/* Global RAM disk for persistence across mount cycles */
static uint8_t* _ram_disk_ptr = NULL;
static size_t   _ram_disk_size = 0;

static void _bench_free_ram_disk(void) {
    if (_ram_disk_ptr) {
        free(_ram_disk_ptr); // Standard free for standard malloc
        _ram_disk_ptr = NULL;
        _ram_disk_size = 0;
    }
}

static hn4_volume_t* _bench_create_mock_vol(uint32_t block_size, uint64_t cap_bytes) {
    hn4_volume_t* vol = NULL;
    mock_hal_device_t* mock_dev = NULL;

    /* 1. Allocate Volume Struct */
    vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    if (!vol) goto error;
    memset(vol, 0, sizeof(hn4_volume_t));

    /* Initialize Locks */
    hn4_hal_spinlock_init(&vol->locking.l2_lock);
    hn4_hal_spinlock_init(&vol->medic_queue.lock);

    vol->vol_block_size = block_size;
#ifdef HN4_USE_128BIT
    vol->vol_capacity_bytes.lo = cap_bytes;
#else
    vol->vol_capacity_bytes = cap_bytes;
#endif

    /* 2. Allocate Mock Device */
    mock_dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    if (!mock_dev) goto error;
    memset(mock_dev, 0, sizeof(mock_hal_device_t));

    /* 3. Setup Backing Store (Global for persistence tests, else fresh) */
    if (cap_bytes <= (512ULL * 1024 * 1024)) {
        if (_ram_disk_ptr == NULL || _ram_disk_size != cap_bytes) {
            _bench_free_ram_disk();
            _ram_disk_size = (size_t)cap_bytes;
            _ram_disk_ptr = malloc(_ram_disk_size); /* Use system malloc for backing store */
            if (!_ram_disk_ptr) goto error;
            memset(_ram_disk_ptr, 0, _ram_disk_size);
        } else {
            /* Reuse existing but zero it */
            memset(_ram_disk_ptr, 0, _ram_disk_size);
        }
        
        mock_dev->mmio_base = _ram_disk_ptr;
        mock_dev->caps.hw_flags = HN4_HW_NVM | HN4_HW_STRICT_FLUSH;
    }

    mock_dev->caps.logical_block_size = 4096; /* Force 4Kn */
    mock_dev->caps.total_capacity_bytes = vol->vol_capacity_bytes;
    mock_dev->caps.queue_count = 1;
    
    vol->target_device = (hn4_hal_device_t*)mock_dev;

    /* 4. Allocator Structures */
    uint64_t total_blocks = cap_bytes / block_size;
    size_t armor_words = (total_blocks + 63) / 64;
    vol->bitmap_size = armor_words * sizeof(hn4_armored_word_t);
    
    if (vol->bitmap_size > 512 * 1024 * 1024) {
        printf("!! OOM: Bitmap too large.\n");
        goto error;
    }

    vol->void_bitmap = hn4_hal_mem_alloc(vol->bitmap_size);
    if (!vol->void_bitmap) goto error;
    memset(vol->void_bitmap, 0, vol->bitmap_size);

    vol->qmask_size = (total_blocks * 2 + 7) / 8;
    vol->quality_mask = hn4_hal_mem_alloc(vol->qmask_size);
    if (!vol->quality_mask) goto error;
    memset(vol->quality_mask, 0xAA, vol->qmask_size);

    /* 5. Mock Superblock */
    vol->sb.info.block_size = block_size;
    vol->sb.info.lba_epoch_start   = hn4_lba_from_sectors(8192 / 4096);
    vol->sb.info.lba_cortex_start  = hn4_lba_from_sectors(65536 / 4096);
    vol->sb.info.lba_bitmap_start  = hn4_lba_from_sectors((65536 + 1024*1024) / 4096);
    vol->sb.info.lba_flux_start    = hn4_lba_from_sectors((10 * 1024 * 1024) / 4096);
    vol->sb.info.lba_horizon_start = hn4_lba_from_sectors((cap_bytes / 4096) - 1000);
    vol->sb.info.device_type_tag = HN4_DEV_SSD; 
    vol->sb.info.format_profile = HN4_PROFILE_GENERIC;
    vol->sb.info.state_flags = HN4_VOL_CLEAN | HN4_VOL_METADATA_ZEROED;

    return vol;

error:
    if (vol) {
        if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
        if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
        hn4_hal_mem_free(vol);
    }
    if (mock_dev) hn4_hal_mem_free(mock_dev);
    _bench_free_ram_disk();
    return NULL;
}

static void _bench_destroy_mock_vol(hn4_volume_t* vol) {
    if (!vol) return;
    /* Dev is attached to vol, so extract before free if needed, but here we free explicitly */
    mock_hal_device_t* dev = (mock_hal_device_t*)vol->target_device;
    
    if (vol->void_bitmap) hn4_hal_mem_free(vol->void_bitmap);
    if (vol->quality_mask) hn4_hal_mem_free(vol->quality_mask);
    if (vol->nano_cortex) hn4_hal_mem_free(vol->nano_cortex);
    
    hn4_hal_mem_free(vol);
    if (dev) hn4_hal_mem_free(dev);
    
    /* We retain the RAM disk ptr between calls if needed, otherwise caller must clear */
}

/* =========================================================================
 * BENCHMARK 1: ALLOCATOR
 * ========================================================================= */
static void _bench_allocator_ballistic(void) {
    const uint32_t BS = 4096;
    const uint64_t CAP = 32ULL * 1024 * 1024 * 1024; 
    const int ITERATIONS = 500000;

    hn4_volume_t* vol = _bench_create_mock_vol(BS, CAP);
    if (!vol) return;
    
    hn4_anchor_t anchor = {0};
    anchor.gravity_center = hn4_cpu_to_le64(100);
    uint64_t v_val = 0x1234567890ABCDEF; 
    memcpy(anchor.orbit_vector, &v_val, 6);

    printf("[Allocator] Running %d allocs on 32GB Volume...\n", ITERATIONS);
    double start = _get_time_sec();
    
    int success_cnt = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        hn4_addr_t lba;
        uint8_t k;
        hn4_result_t res = hn4_alloc_block(vol, &anchor, i, &lba, &k);
        if (HN4_LIKELY(res == HN4_OK)) {
            success_cnt++;
        } else if (res == HN4_ERR_ENOSPC) {
            break; /* Volume full, stop metric */
        }
    }

    double end = _get_time_sec();
    double d = HN4_SAFE_DURATION(end - start);
    
    printf("[Allocator] Time: %.6f sec | Rate: %.2f M-Ops/sec\n", d, (double)success_cnt / d / 1e6);
    
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 2: ATOMIC WRITE
 * ========================================================================= */
static void _bench_write_atomic(void) {
    const uint32_t BS = 4096;
    const uint64_t CAP = 64 * 1024 * 1024; 
    const int ITERATIONS = 10000;

    hn4_volume_t* vol = _bench_create_mock_vol(BS, CAP);
    if (!vol) return;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0x1; anchor.seed_id.hi = 0x2;
    anchor.gravity_center = hn4_cpu_to_le64(100);
    uint64_t v_val = 17;
    memcpy(anchor.orbit_vector, &v_val, 6);
    anchor.permissions = hn4_cpu_to_le32(HN4_PERM_WRITE | HN4_PERM_READ | HN4_PERM_SOVEREIGN);
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
    anchor.write_gen = hn4_cpu_to_le32(1);

    uint32_t payload_len = HN4_BLOCK_PayloadSize(BS);
    /* Use HAL Allocator for alignment */
    uint8_t* payload = hn4_hal_mem_alloc(payload_len);
    if(payload) memset(payload, 0xAA, payload_len);
    else {
        _bench_destroy_mock_vol(vol);
        return;
    }

    printf("[Write] Atomic Pipeline: %d blocks (CRC + Header + Memcpy)...\n", ITERATIONS);
    double start = _get_time_sec();
    
    int success_cnt = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        hn4_result_t res = hn4_write_block_atomic(vol, &anchor, i, payload, payload_len, 0);
        if (HN4_LIKELY(res == HN4_OK)) success_cnt++;
        else if (i == 0) printf("!! Write Failed Block 0: %d\n", res);
    }

    double d = _get_time_sec() - start;
    double mb_sec = HN4_SAFE_DIV((double)success_cnt * payload_len, 1024.0 * 1024.0) / d;

    printf("[Write] Time: %.6f sec | IOPS: %.0f | BW: %.2f MB/s\n", 
           d, HN4_SAFE_DIV(success_cnt, d), mb_sec);

    if(payload) hn4_hal_mem_free(payload);
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 3: ATOMIC READ
 * ========================================================================= */
static void _bench_read_atomic(void) {
    const uint32_t BS = 4096;
    const uint64_t CAP = 64 * 1024 * 1024;
    const int WRITE_COUNT = 5000;
    const int READ_ITERS  = 4; 

    hn4_volume_t* vol = _bench_create_mock_vol(BS, CAP);
    if (!vol) return;
    
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 0xA; anchor.seed_id.hi = 0xB;
    anchor.gravity_center = hn4_cpu_to_le64(500); 
    uint64_t v_val = 19;
    memcpy(anchor.orbit_vector, &v_val, 6);
    anchor.permissions = hn4_cpu_to_le32(HN4_PERM_WRITE | HN4_PERM_READ | HN4_PERM_SOVEREIGN);
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
    anchor.write_gen = hn4_cpu_to_le32(1);

    uint32_t payload_len = HN4_BLOCK_PayloadSize(BS);
    uint8_t* payload = hn4_hal_mem_alloc(payload_len);
    if (payload) memset(payload, 0x55, payload_len);
    
    void* read_buf = hn4_hal_mem_alloc(payload_len);

    if (!payload || !read_buf) {
        if(payload) hn4_hal_mem_free(payload);
        if(read_buf) hn4_hal_mem_free(read_buf);
        _bench_destroy_mock_vol(vol);
        return;
    }

    printf("[Read] Pre-populating %d blocks...\n", WRITE_COUNT);
    for (int i = 0; i < WRITE_COUNT; i++) {
        hn4_write_block_atomic(vol, &anchor, i, payload, payload_len, 0);
    }

    printf("[Read] Reading %d blocks (x%d passes) with memcmp...\n", WRITE_COUNT, READ_ITERS);
    
    double start = _get_time_sec();

int op_cnt = 0;
int success_cnt = 0;
volatile int total_valid = 0;

for (int pass = 0; pass < READ_ITERS; pass++) {
    for (int i = 0; i < WRITE_COUNT; i++) {

        hn4_anchor_t temp_anchor = anchor;
        temp_anchor.write_gen = hn4_cpu_to_le32(1);   // ✅ match actual write gen

        hn4_result_t res = hn4_read_block_atomic(vol, &temp_anchor, i,
                                                 read_buf, payload_len, 0);

        op_cnt++;                                    // ✅ count every read

        if (res == HN4_OK) {
            if (memcmp(read_buf, payload, payload_len) == 0)
                success_cnt++;                       // integrity counter only
        }
    }
}

total_valid = success_cnt;

double end = _get_time_sec();
double d = HN4_SAFE_DURATION(end - start);

double iops = HN4_SAFE_DIV(op_cnt, d);
double bw   = HN4_SAFE_DIV((double)op_cnt * payload_len,
                           1024.0 * 1024.0) / d;

printf("[Read] Time: %.6f sec | IOPS: %.0f | BW: %.2f MB/s | OK: %d/%d\n",
       d, iops, bw, success_cnt, op_cnt);

    hn4_hal_mem_free(payload);
    hn4_hal_mem_free(read_buf);
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 4: MOUNT / UNMOUNT CYCLE
 * ========================================================================= */
static void _bench_mount_cycle(void) {
    const uint32_t BS = 4096;
    const uint64_t CAP = 256 * 1024 * 1024; 
    const int CYCLES = 1000;

    /* 1. Allocate Backing Store Manually */
    if (_ram_disk_ptr) free(_ram_disk_ptr);
    _ram_disk_ptr = malloc((size_t)CAP);
    if (!_ram_disk_ptr) return;
    _ram_disk_size = CAP;
    memset(_ram_disk_ptr, 0, (size_t)CAP);

    /* 2. Setup Mock Device Helper */
    mock_hal_device_t* mock_dev = hn4_hal_mem_alloc(sizeof(mock_hal_device_t));
    if (!mock_dev) { free(_ram_disk_ptr); return; }
    memset(mock_dev, 0, sizeof(mock_hal_device_t));
    
    mock_dev->mmio_base = _ram_disk_ptr;
    mock_dev->caps.hw_flags = HN4_HW_NVM | HN4_HW_STRICT_FLUSH;
    mock_dev->caps.logical_block_size = 4096;
    
#ifdef HN4_USE_128BIT
    mock_dev->caps.total_capacity_bytes.lo = CAP;
#else
    mock_dev->caps.total_capacity_bytes = CAP;
#endif
    mock_dev->caps.queue_count = 1;

    printf("[Mount] Formatting Volume (256MB)...\n");
    hn4_format_params_t fmt_p = {0};
    fmt_p.target_profile = HN4_PROFILE_GENERIC;
    fmt_p.label = "BENCH";
    
    if (hn4_format((hn4_hal_device_t*)mock_dev, &fmt_p) != HN4_OK) {
        printf("[Mount] Format failed!\n");
        _bench_free_ram_disk();
        hn4_hal_mem_free(mock_dev);
        return;
    }

    printf("[Mount] Cycling Mount/Unmount %d times...\n", CYCLES);
    double start = _get_time_sec();
    int success_cnt = 0;

    for (int i = 0; i < CYCLES; i++) {
        hn4_volume_t* vol = NULL;
        hn4_mount_params_t mnt_p = {0};
        
        hn4_result_t m_res = hn4_mount((hn4_hal_device_t*)mock_dev, &mnt_p, &vol);
        if (m_res == HN4_OK) {
            if (hn4_unmount(vol) == HN4_OK) success_cnt++;
            else { printf("!! Unmount Fail Cycle %d\n", i); break; }
        } else {
            printf("!! Mount Fail Cycle %d: Error %d\n", i, m_res);
            break;
        }
        
        /* Note: hn4_unmount frees 'vol'. We keep 'mock_dev' alive because we allocated it outside. */
        vol = NULL; 
    }

    double end = _get_time_sec();
    double d = HN4_SAFE_DURATION(end - start);
    
    printf("[Mount] Time: %.6f sec | Rate: %.2f Mounts/sec\n", 
           d, HN4_SAFE_DIV(success_cnt, d));

    hn4_hal_mem_free(mock_dev);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 5: TENSOR STREAM
 * ========================================================================= */
static void _bench_tensor_scatter(void) {
    const uint32_t BS = 4096;
    const uint64_t CAP = 64 * 1024 * 1024;
    const int SHARD_COUNT = 1000;
    const int READ_OPS = 50000;

    hn4_volume_t* vol = _bench_create_mock_vol(BS, CAP);
    if (!vol) return;

    printf("[Tensor] Building %d shards...\n", SHARD_COUNT);

    hn4_tensor_ctx_t* ctx = hn4_hal_mem_alloc(sizeof(hn4_tensor_ctx_t));
    if (!ctx) { _bench_destroy_mock_vol(vol); _bench_free_ram_disk(); return; }
    
    ctx->vol = vol;
    ctx->block_size = BS;
    ctx->payload_cap = HN4_BLOCK_PayloadSize(BS);
    ctx->shard_count = SHARD_COUNT;
    
    ctx->shards = hn4_hal_mem_alloc(sizeof(hn4_anchor_t) * SHARD_COUNT);
    ctx->shard_offsets = hn4_hal_mem_alloc(sizeof(uint64_t) * (SHARD_COUNT + 1));
    
    if (!ctx->shards || !ctx->shard_offsets) {
        if(ctx->shards) hn4_hal_mem_free(ctx->shards);
        if(ctx->shard_offsets) hn4_hal_mem_free(ctx->shard_offsets);
        hn4_hal_mem_free(ctx);
        _bench_destroy_mock_vol(vol);
        _bench_free_ram_disk();
        return;
    }

    uint64_t global_acc = 0;
    for (int i = 0; i < SHARD_COUNT; i++) {
        ctx->shard_offsets[i] = global_acc;
        uint64_t shard_mass = 65536; 
        ctx->shards[i].mass = hn4_cpu_to_le64(shard_mass);
        ctx->shards[i].gravity_center = hn4_cpu_to_le64(i * 100); 
        ctx->shards[i].write_gen = hn4_cpu_to_le32(1);
        ctx->shards[i].permissions = hn4_cpu_to_le32(HN4_PERM_READ);
        ctx->shards[i].seed_id.lo = i + 1;
        global_acc += shard_mass;
    }
    ctx->shard_offsets[SHARD_COUNT] = global_acc;
    ctx->total_size_bytes = global_acc;

    printf("[Tensor] Virtual Size: %.2f MB. Running %d random lookups...\n", 
           (double)global_acc / 1024.0 / 1024.0, READ_OPS);

    void* buf = hn4_hal_mem_alloc(BS);
    if (!buf) {
        hn4_tensor_close(ctx);
        _bench_destroy_mock_vol(vol);
        _bench_free_ram_disk();
        return;
    }

    double start = _get_time_sec();
    int success_cnt = 0;
    
    /* FIX: Replaced 16-bit LFSR with 64-bit Xorshift to cover full memory range */
    uint64_t rng_state = 0xACE1u; 

    for (int i = 0; i < READ_OPS; i++) {
        /* Xorshift64 */
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        
        /* Modulo against the full 64MB size */
        uint64_t offset = rng_state % (global_acc - BS);
        
        hn4_result_t res = hn4_tensor_read(ctx, offset, buf, BS);
        /* Expect failures since data isn't written, checking logic overhead */
        if (res == HN4_OK || res == HN4_ERR_HEADER_ROT || res == HN4_ERR_PHANTOM_BLOCK) success_cnt++;
    }

    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    
    printf("[Tensor] Time: %.6f sec | Rate: %.2f K-Lookups/sec\n", d, HN4_SAFE_DIV(READ_OPS, d) / 1e3);

    hn4_tensor_close(ctx);
    hn4_hal_mem_free(buf);
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 6: COMPRESSION (TCC)
 * ========================================================================= */
static void _bench_compression_tcc(void) {
    const int BUF_SIZE = 65536;
    const int ITERATIONS = 10000;
    
    /* Use HAL Alloc for alignment friendliness */
    uint8_t* src = hn4_hal_mem_alloc(BUF_SIZE);
    uint8_t* dst = hn4_hal_mem_alloc(hn4_compress_bound(BUF_SIZE));
    
    if (!src || !dst) {
        if(src) hn4_hal_mem_free(src);
        if(dst) hn4_hal_mem_free(dst);
        return;
    }

    uint32_t out_len = 0;

    memset(src, 0x77, BUF_SIZE);
    double start = _get_time_sec();
    for(int i=0; i<ITERATIONS; i++) {
        hn4_compress_block(src, BUF_SIZE, dst, BUF_SIZE, &out_len, 0, 0);
    }
    double t_iso = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[TCC] Isotope (All 0x77): %.2f GB/s\n", ((double)ITERATIONS * BUF_SIZE) / 1e9 / t_iso);

    for(int i=0; i<BUF_SIZE; i++) src[i] = (uint8_t)(i % 256);
    start = _get_time_sec();
    for(int i=0; i<ITERATIONS; i++) {
        hn4_compress_block(src, BUF_SIZE, dst, BUF_SIZE, &out_len, 0, 0);
    }
    double t_grad = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[TCC] Gradient (0..255):  %.2f GB/s\n", ((double)ITERATIONS * BUF_SIZE) / 1e9 / t_grad);

    hn4_hal_mem_free(src);
    hn4_hal_mem_free(dst);
}

/* =========================================================================
 * BENCHMARK 7: NAMESPACE LOOKUP
 * ========================================================================= */
static void _bench_namespace_lookup(void) {
    const int ANCHOR_COUNT = 10000;
    const int LOOKUPS = 100000;
    
    hn4_volume_t* vol = _bench_create_mock_vol(4096, 1ULL << 30);
    if (!vol) return;

    size_t cortex_sz = 20 * 1024 * 1024;
    vol->nano_cortex = hn4_hal_mem_alloc(cortex_sz);
    if (!vol->nano_cortex) {
        _bench_destroy_mock_vol(vol);
        _bench_free_ram_disk();
        return;
    }
    vol->cortex_size = cortex_sz;
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    memset(anchors, 0, cortex_sz);

    printf("[Namespace] Populating Cortex with %d anchors...\n", ANCHOR_COUNT);
    for (int i = 0; i < ANCHOR_COUNT; i++) {
        anchors[i].seed_id.lo = i + 1;
        anchors[i].data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
        anchors[i].checksum = hn4_cpu_to_le32(hn4_crc32(0, &anchors[i], sizeof(hn4_anchor_t)));
    }

    printf("[Namespace] Running %d lookups...\n", LOOKUPS);
    double start = _get_time_sec();
    volatile int found_cnt = 0;
    
    for (int i = 0; i < LOOKUPS; i++) {
        hn4_u128_t target;
        target.lo = (i % ANCHOR_COUNT) + 1;
        target.hi = 0;
        hn4_anchor_t out;
        if (_ns_scan_cortex_slot(vol, target, &out, NULL) == HN4_OK) found_cnt++;
    }
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    
    printf("[Namespace] Time: %.6f sec | Rate: %.2f M-Lookups/sec (Hit Rate: %d%%)\n", 
           d, (double)LOOKUPS / d / 1e6, (found_cnt * 100) / LOOKUPS);

    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 8: SCAVENGER STRESS
 * ========================================================================= */
static void _bench_scavenger_stress(void) {
    const int ANCHOR_COUNT = 100000;
    const int TOMBSTONE_RATIO = 20; /* 20% Tombstones */
    
    /* Create mock volume with only RAM Cortex */
    hn4_volume_t* vol = _bench_create_mock_vol(4096, 1ULL << 30);
    if (!vol) return;

    size_t cortex_sz = ANCHOR_COUNT * sizeof(hn4_anchor_t);
    vol->nano_cortex = hn4_hal_mem_alloc(cortex_sz);
    if (!vol->nano_cortex) {
        _bench_destroy_mock_vol(vol);
        _bench_free_ram_disk();
        return;
    }
    vol->cortex_size = cortex_sz;
    
    hn4_anchor_t* anchors = (hn4_anchor_t*)vol->nano_cortex;
    hn4_time_t now = hn4_hal_get_time_ns();

    /* Populate Cortex */
    int dead_cnt = 0;
    for (int i = 0; i < ANCHOR_COUNT; i++) {
        anchors[i].seed_id.lo = i + 1;
        anchors[i].write_gen = hn4_cpu_to_le32(1);
        
        if ((i % 100) < TOMBSTONE_RATIO) {
            /* Mark as Dead, expired grace period */
            uint64_t dclass = HN4_FLAG_TOMBSTONE | HN4_FLAG_VALID;
            anchors[i].data_class = hn4_cpu_to_le64(dclass);
            anchors[i].mod_clock = hn4_cpu_to_le64(now - (25ULL * 3600 * 1000000000)); /* 25h ago */
            dead_cnt++;
        } else {
            /* Live */
            anchors[i].data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
        }
    }

    printf("[Scavenger] Scanning %d anchors (%d%% Tombstones)...\n", ANCHOR_COUNT, TOMBSTONE_RATIO);
    
    /* 
     * FIX: Calculate pulses needed to cover cortex exactly once per pass.
     * Scavenger scans 64 items per pulse.
     */
    int pulses_per_pass = (ANCHOR_COUNT + 63) / 64;

    double start = _get_time_sec();
    
    /* Run 100 full passes to get stable timing */
    int passes = 100;
    for (int p = 0; p < passes; p++) {
        /* Reset cursor to start for consistent linear scan simulation */
        vol->alloc.scavenger_cursor = 0;
        
        /* 
         * FIX: Use fixed count loop instead of checking cursor < COUNT,
         * because cursor wraps around via modulo arithmetic in the driver.
         */
        for (int k = 0; k < pulses_per_pass; k++) {
            hn4_scavenger_pulse(vol);
        }
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    
    /* Total anchors scanned = Count * Passes */
    double total_scanned = (double)ANCHOR_COUNT * passes;
    
    printf("[Scavenger] Time: %.6f sec | Scan Rate: %.2f M-Anchors/sec\n", 
           d, total_scanned / d / 1e6);

    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 11: EPOCH ROTATION
 * ========================================================================= */
static void _bench_epoch_rotation(void) {
    const int ROTATIONS = 10000;
    const uint32_t BS = 4096;
    /* Reduce to 128MB to be safe on RAM */
    hn4_volume_t* vol = _bench_create_mock_vol(BS, 128ULL * 1024 * 1024);
    if (!vol) return;

    /* 
     * FIX: Geometry Calculation for 4Kn Sectors.
     * Mock HAL SS = 4096. BS = 4096. Ratio = 1.
     */
    uint64_t ring_start_blk = 100;
    uint32_t ss = 4096;
    uint32_t spb = BS / ss; /* 1 */
    
    vol->sb.info.lba_epoch_start = hn4_lba_from_blocks(ring_start_blk * spb);
    vol->sb.info.epoch_ring_block_idx = hn4_addr_from_u64(ring_start_blk); 
    vol->sb.info.copy_generation = 1;

    /* Zero the ring region */
    uint8_t* zeros = hn4_hal_mem_alloc(BS * 256);
    if (zeros) {
        memset(zeros, 0, BS * 256);
        /* length in sectors */
        hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, vol->sb.info.lba_epoch_start, zeros, (BS * 256) / ss);
        hn4_hal_mem_free(zeros);
    }

    printf("[Epoch] Performing %d ring rotations...\n", ROTATIONS);
    
    double start = _get_time_sec();
    
    int success_cnt = 0;
    for (int i = 0; i < ROTATIONS; i++) {
        uint64_t new_id = 0;
        hn4_addr_t new_ptr;
        
        hn4_result_t res = hn4_epoch_advance(
            vol->target_device, 
            &vol->sb, 
            false,
            &new_id, 
            &new_ptr
        );
        
        if (HN4_LIKELY(res == HN4_OK)) {
            vol->sb.info.current_epoch_id = new_id;
            vol->sb.info.epoch_ring_block_idx = new_ptr;
            success_cnt++;
        } else {
            printf("!! Epoch Fail %d: %d\n", i, res);
            break;
        }
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[Epoch] Time: %.6f sec | Rate: %.2f K-Rotations/sec\n", d, (double)success_cnt / d / 1e3);

    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}



/* =========================================================================
 * BENCHMARK 9: SWIZZLE MATH THROUGHPUT
 * ========================================================================= */
static void _bench_swizzle_throughput(void) {
    const int ITERATIONS = 10000000;
    
    hn4_volume_t* vol = _bench_create_mock_vol(4096, 1ULL << 30);
    if (!vol) return;

    /* Setup Physics Constants */
    uint64_t G = 1000;
    uint64_t V = 0x1234567890ABCDEF;
    uint16_t M = 0; /* 4KB Scale */
    
    printf("[Swizzle] Computing %d ballistic trajectories...\n", ITERATIONS);
    
    double start = _get_time_sec();
    
    volatile uint64_t sink = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        /* 
         * Simulate typical collision pattern:
         * 90% k=0, 10% k=1..3
         */
        uint8_t k = (i % 10 == 0) ? (i % 4) : 0;
        
        /* Calc LBA for Block Index 'i' */
        uint64_t lba = _calc_trajectory_lba(vol, G, V, i, M, k);
        sink ^= lba;
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    
    printf("[Swizzle] Time: %.6f sec | Rate: %.2f M-Calcs/sec\n", 
           d, (double)ITERATIONS / d / 1e6);
           
    (void)sink;
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 10: CHRONICLE BURST
 * ========================================================================= */
static void _bench_chronicle_burst(void) {
    const int EVENTS = 20000;
    const uint32_t BS = 4096;
    
    hn4_volume_t* vol = _bench_create_mock_vol(BS, 128ULL * 1024 * 1024);
    if (!vol) return;

    /* Setup pointers */
    vol->sb.info.journal_start = hn4_lba_from_blocks(1000);
    vol->sb.info.journal_ptr = vol->sb.info.journal_start; 
    vol->sb.info.lba_horizon_start = hn4_lba_from_blocks(20000); 
    vol->sb.info.last_journal_seq = 0;

    /* [FIX] Ensure SB Capacity is set for wrap-around calc */
#ifdef HN4_USE_128BIT
    vol->sb.info.total_capacity.lo = 128ULL * 1024 * 1024;
#else
    vol->sb.info.total_capacity = 128ULL * 1024 * 1024;
#endif

    /* Zero the journal area */
    uint32_t journal_sz = 1024 * BS; 
    uint8_t* zeros = hn4_hal_mem_alloc(journal_sz);
    if (zeros) {
        memset(zeros, 0, journal_sz);
        hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, vol->sb.info.journal_start, zeros, journal_sz / 4096);
        hn4_hal_mem_free(zeros);
    }

    printf("[Chronicle] Appending %d audit events...\n", EVENTS);
    
    double start = _get_time_sec();
    int success_cnt = 0;

    for (int i = 0; i < EVENTS; i++) {
        hn4_result_t res = hn4_chronicle_append(
            vol->target_device, 
            vol, 
            HN4_CHRONICLE_OP_SNAPSHOT,
            hn4_lba_from_blocks(i), 
            hn4_lba_from_blocks(i+1), 
            0xCAFEBABE
        );
        
        if (HN4_UNLIKELY(res != HN4_OK)) {
            printf("!! Chronicle Fail %d: %d\n", i, res);
            break;
        }
        success_cnt++;
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[Chronicle] Time: %.6f sec | Rate: %.2f K-Events/sec\n", d, (double)success_cnt / d / 1e3);

    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 12: SHADOW HOP LATENCY
 * ========================================================================= */
static void _bench_shadow_hop(void) {
    const int OPS = 50000;
    const uint32_t BS = 4096;
    
    hn4_volume_t* vol = _bench_create_mock_vol(BS, 128ULL * 1024 * 1024);
    if (!vol) return;

    /* Setup Anchor */
    hn4_anchor_t anchor = {0};
    anchor.seed_id.lo = 1;
    anchor.write_gen = hn4_cpu_to_le32(1);
    anchor.permissions = hn4_cpu_to_le32(HN4_PERM_WRITE | HN4_PERM_SOVEREIGN);
    anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);

    /* Setup Data */
    uint8_t* buf = hn4_hal_mem_alloc(BS);
    memset(buf, 0xAA, BS);
    uint32_t payload_len = HN4_BLOCK_PayloadSize(BS);

    printf("[Shadow] Performing %d atomic overwrites (Shadow Hops)...\n", OPS);
    
    double start = _get_time_sec();
    
    for (int i = 0; i < OPS; i++) {
        /* Write to Block 0 repeatedly. Each write triggers a new allocation (Hop). */
        hn4_result_t res = hn4_write_block_atomic(
            vol, &anchor, 0, buf, payload_len, 0
        );
        
        if (res != HN4_OK) {
            printf("!! Shadow Hop Fail %d: %d\n", i, res);
            break;
        }
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    
    printf("[Shadow] Time: %.6f sec | Rate: %.2f K-Hops/sec | Latency: %.2f us\n", 
           d, (double)OPS / d / 1e3, (d / OPS) * 1e6);

    hn4_hal_mem_free(buf);
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 13: METADATA SCAN RATE
 * ========================================================================= */
static void _bench_metadata_scan(void) {
    const int ANCHORS = 500000;
    
    hn4_volume_t* vol = _bench_create_mock_vol(4096, 1ULL << 30);
    if (!vol) return;

    size_t ctx_size = ANCHORS * sizeof(hn4_anchor_t);
    vol->nano_cortex = hn4_hal_mem_alloc(ctx_size);
    vol->cortex_size = ctx_size;
    
    hn4_anchor_t* arr = (hn4_anchor_t*)vol->nano_cortex;
    
    /* Populate with mixed data */
    for(int i=0; i<ANCHORS; i++) {
        arr[i].seed_id.lo = i;
        if (i % 2 == 0) arr[i].data_class = hn4_cpu_to_le64(HN4_FLAG_VALID);
        else arr[i].data_class = hn4_cpu_to_le64(HN4_FLAG_TOMBSTONE | HN4_FLAG_VALID);
    }

    printf("[Meta] Scanning %d anchors for Tombstones (linear memory sweep)...\n", ANCHORS);
    
    double start = _get_time_sec();
    
    volatile int count = 0;
    /* Run 100 passes to heat cache and measure raw throughput */
    for(int k=0; k<100; k++) {
        for(int i=0; i<ANCHORS; i++) {
            if (hn4_le64_to_cpu(arr[i].data_class) & HN4_FLAG_TOMBSTONE) {
                count++;
            }
        }
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    double total_items = (double)ANCHORS * 100;
    
    printf("[Meta] Time: %.6f sec | Rate: %.2f M-Anchors/sec\n", 
           d, total_items / d / 1e6);

    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* =========================================================================
 * BENCHMARK 14: CRC THROUGHPUT
 * ========================================================================= */
static void _bench_crc_throughput(void) {
    const size_t BUF_SIZE = 16 * 1024 * 1024; /* 16 MB Buffer */
    const int ITERATIONS = 100;
    
    uint8_t* buf = hn4_hal_mem_alloc(BUF_SIZE);
    if (!buf) return;
    
    /* Fill with random data to prevent zero-optimization bias */
    for(size_t i=0; i<BUF_SIZE; i+=8) {
        *(uint64_t*)(buf + i) = hn4_hal_get_random_u64();
    }

    printf("[CRC] Hashing %zu MB buffer x %d iterations...\n", BUF_SIZE / (1024*1024), ITERATIONS);
    
    double start = _get_time_sec();
    volatile uint32_t sink = 0;
    
    for(int i=0; i<ITERATIONS; i++) {
        sink ^= hn4_crc32(0, buf, BUF_SIZE);
    }
    
    double d = HN4_SAFE_DURATION(_get_time_sec() - start);
    double total_bytes = (double)BUF_SIZE * ITERATIONS;
    double gb_sec = (total_bytes / d) / (1024.0 * 1024.0 * 1024.0);
    
    printf("[CRC] Time: %.6f sec | Throughput: %.2f GB/s\n", d, gb_sec);
    
    hn4_hal_mem_free(buf);
}

/* =========================================================================
 * BENCHMARK 15: DELETE / UNDELETE LIFECYCLE
 * ========================================================================= */
static void _bench_lifecycle_tombstone(void) {
    const int COUNT = 50000;
    const uint32_t BS = 4096;
    
    /* Setup 256MB Volume to hold bitmaps + blocks */
    hn4_volume_t* vol = _bench_create_mock_vol(BS, 256ULL * 1024 * 1024);
    if (!vol) return;

    /* Setup RAM Cortex (Required for Namespace Scans) */
    size_t cortex_bytes = COUNT * sizeof(hn4_anchor_t) * 2; /* 2x Load Factor */
    vol->nano_cortex = hn4_hal_mem_alloc(cortex_bytes);
    if (!vol->nano_cortex) {
        _bench_destroy_mock_vol(vol);
        return;
    }
    vol->cortex_size = cortex_bytes;
    memset(vol->nano_cortex, 0, cortex_bytes);
    
    hn4_anchor_t* ram_slots = (hn4_anchor_t*)vol->nano_cortex;
    size_t slot_count = cortex_bytes / sizeof(hn4_anchor_t);

    /* Scratch buffer for physical block writes (Pulse Check requirement) */
    void* blk_buf = hn4_hal_mem_alloc(BS);
    if (!blk_buf) {
        _bench_destroy_mock_vol(vol);
        return;
    }

    printf("[Lifecycle] Pre-populating %d files (Write + RAM Inject + Phys Block)...\n", COUNT);

    /* 1. POPULATE */
    for (int i = 0; i < COUNT; i++) {
        hn4_anchor_t anchor = {0};
        char name[32];
        snprintf(name, 32, "%x", i); /* Simple Hex Name */
        
        anchor.seed_id.lo = i + 1;
        anchor.seed_id.hi = 0;
        anchor.data_class = hn4_cpu_to_le64(HN4_FLAG_VALID | HN4_VOL_ATOMIC);
        anchor.permissions = hn4_cpu_to_le32(HN4_PERM_READ | HN4_PERM_WRITE);
        anchor.write_gen = hn4_cpu_to_le32(1);
        anchor.gravity_center = hn4_cpu_to_le64(100 + i); /* G offset to avoid SB */
        anchor.orbit_vector[0] = 1; /* V=1 (Sequential) */
        strncpy((char*)anchor.inline_buffer, name, 23);

        /* Write Anchor to Disk (updates CRC internally) */
        hn4_write_anchor_atomic(vol, &anchor);

        /* Inject into RAM (Simulate Mount Scan) */
        hn4_u128_t seed = hn4_le128_to_cpu(anchor.seed_id);
        uint64_t h = seed.lo ^ seed.hi;
        h ^= (h >> 33); h *= 0xff51afd7ed558ccdULL; h ^= (h >> 33);
        
        size_t start_slot = h % slot_count;
        size_t s = start_slot;
        while(1) {
            if (ram_slots[s].seed_id.lo == 0) {
                ram_slots[s] = anchor;
                break;
            }
            s = (s + 1) % slot_count;
            if (s == start_slot) break;
        }

        /* 
         * Write Valid Physical Block
         * Required for hn4_undelete() integrity check (The Pulse Check)
         */
        hn4_block_header_t* hdr = (hn4_block_header_t*)blk_buf;
        memset(hdr, 0, BS);
        hdr->magic = hn4_cpu_to_le32(HN4_BLOCK_MAGIC);
        hdr->well_id = anchor.seed_id;
        hdr->generation = hn4_cpu_to_le64(1);
        hdr->header_crc = hn4_cpu_to_le32(hn4_crc32(HN4_CRC_SEED_HEADER, hdr, offsetof(hn4_block_header_t, header_crc)));
        
        /* LBA Calc: G + (Index * V) = (100+i) + (0 * 1) = 100+i */
        uint64_t lba_idx = 100 + i;
        uint32_t spb = BS / 4096; /* Mock is 4K/4K = 1 */
        hn4_addr_t lba_phys = hn4_lba_from_blocks(lba_idx * spb);
        
        hn4_hal_sync_io(vol->target_device, HN4_IO_WRITE, lba_phys, blk_buf, spb);
        
        /* Set Bitmap so Undelete sees "Allocated" (Not Reaped) */
        _bitmap_op(vol, lba_idx, BIT_SET, NULL);
    }

    /* 2. BENCHMARK DELETE */
    printf("[Lifecycle] Deleting %d files...\n", COUNT);
    double start = _get_time_sec();
    
    for (int i = 0; i < COUNT; i++) {
        char name[32];
        snprintf(name, 32, "%x", i);
        hn4_delete(vol, name);
    }
    
    double t_del = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[Lifecycle] Delete Rate: %.2f K-Ops/sec\n", (double)COUNT / t_del / 1e3);

    /* 3. BENCHMARK UNDELETE */
    printf("[Lifecycle] Undeleting %d files (includes IO verify)...\n", COUNT);
    start = _get_time_sec();
    
    volatile int success_cnt = 0;
    for (int i = 0; i < COUNT; i++) {
        char name[32];
        snprintf(name, 32, "%x", i);
        if (hn4_undelete(vol, name) == HN4_OK) success_cnt++;
    }
    
    double t_undel = HN4_SAFE_DURATION(_get_time_sec() - start);
    printf("[Lifecycle] Undelete Rate: %.2f K-Ops/sec (Success: %d/%d)\n", 
           (double)COUNT / t_undel / 1e3, success_cnt, COUNT);

    hn4_hal_mem_free(blk_buf);
    _bench_destroy_mock_vol(vol);
    _bench_free_ram_disk();
}

/* Add to REGISTRY array: */



/* =========================================================================
 * REGISTRY
 * ========================================================================= */

typedef void (*bench_func_t)(void);

typedef struct {
    const char* name;
    bench_func_t func;
} benchmark_entry_t;

static benchmark_entry_t REGISTRY[] = {
    { "allocator_ballistic", _bench_allocator_ballistic },
    { "write_atomic",        _bench_write_atomic },
    { "write_read_atomic",         _bench_read_atomic },
    { "mount_cycle",         _bench_mount_cycle },
    { "tensor_scatter",      _bench_tensor_scatter },
    { "compression_tcc",     _bench_compression_tcc },
    { "namespace_lookup",    _bench_namespace_lookup },
    { "epoch_rotation",      _bench_epoch_rotation },
    { "scavenger_stress",    _bench_scavenger_stress },
    { "swizzle_throughput",  _bench_swizzle_throughput },
    { "chronicle_burst",     _bench_chronicle_burst },
    { "shadow_hop_latency",  _bench_shadow_hop },
    { "metadata_scan",       _bench_metadata_scan },
    { "crc_throughput",      _bench_crc_throughput },
    { "lifecycle_tombstone", _bench_lifecycle_tombstone },
    { NULL, NULL }
};

void hn4_run_benchmarks(const char* filter_name) {
    for (int i = 0; REGISTRY[i].name != NULL; i++) {
        if (filter_name == NULL || strstr(REGISTRY[i].name, filter_name) != NULL) {
            printf("\n>>> Running Benchmark: %s\n", REGISTRY[i].name);
            REGISTRY[i].func();
        }
    }
    printf("\n>>> Benchmark Suite Complete.\n");
}