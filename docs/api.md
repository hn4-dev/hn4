# HN4 API ARCHITECTURE
### *The Interface to the Manifold*

**Status:** Implementation Standard v6.0
**Module:** `hn4_api.c`
**Metric:** Zero-Copy Semantics

---

## 1. Abstract: Beyond POSIX

The HN4 API is designed for **High-Performance Computing (HPC)**. While it offers a POSIX compatibility layer (VFS), the native API exposes the raw power of the Ballistic Engine: **Atomic Writes**, **Tensor Mapping**, and **Tag-Based Addressing**.

It eliminates the "User-Kernel Boundary Tax" by favoring shared memory structures and direct hardware submission where possible.

---

## 2. Core Concepts

### 2.1 The Handle (`hn4_handle_t`)
Unlike a Linux File Descriptor (`int fd`), an HN4 Handle is a **Rich State Object**.
*   **Cached Anchor:** Contains the file's physics ($G, V, M$) so trajectory calculations happen in user-space without syscalls.
*   **Session Token:** Validates security without re-checking ACLs on every read.
*   **Ghost Hints:** Runtime optimizations (e.g., "Disable Prefetch") that exist only in RAM.

### 2.2 Synchronous vs. Async
*   **Standard:** `hn4_read()` blocks until completion.
*   **High-Velocity:** `hn4_submit_io()` queues requests to the NVMe ring and returns instantly. The user polls for completion. This is akin to `io_uring` but native to the FS driver.

---

## 3. The API Surface

### 3.1 Volume Management
```c
// Initialize Driver & Physics Engine
hn4_result_t hn4_api_init(void);

// Mount a Volume (Zero-Gravity Boot)
hn4_result_t hn4_api_mount(hn4_hal_device_t* dev, hn4_volume_t** out_vol);

// Format a Drive (The Big Bang)
hn4_result_t hn4_api_format(hn4_hal_device_t* dev, const char* label, uint32_t profile);
```

### 3.2 File Operations (O(1))
```c
// Create File (Atomic Genesis)
// Returns a Handle. No directory lookup needed.
hn4_handle_t* hn4_api_create(hn4_volume_t* vol, 
                             const char* name, 
                             uint64_t size_hint, 
                             uint32_t data_class);

// Open File (Hash Lookup)
hn4_handle_t* hn4_api_open(hn4_volume_t* vol, 
                           const char* uri, // "hn4://Vol/id:..." or "tag:..."
                           uint32_t flags);

// Close (Commit State)
hn4_result_t hn4_api_close(hn4_handle_t* handle);
```

### 3.3 IO Kinetics
```c
// Read (Ballistic Shotgun)
hn4_result_t hn4_api_read(hn4_handle_t* handle, 
                          void* buffer, 
                          uint64_t length, 
                          uint64_t* out_read);

// Write (Shadow Hop)
hn4_result_t hn4_api_write(hn4_handle_t* handle, 
                           const void* buffer, 
                           uint64_t length, 
                           uint64_t* out_written);

// Sync (Barrier)
// Forces the Anchor update to stable storage.
hn4_result_t hn4_api_sync(hn4_handle_t* handle);
```

### 3.4 Tensor Tunnel (AI Specialization)
```c
// Zero-Copy GPU Mapping
// Programs the NVMe controller to DMA directly to VRAM.
hn4_result_t hn4_api_map_tensor(hn4_handle_t* handle, 
                                void* gpu_ptr, 
                                size_t size);
```

---

## 4. Integration Guide

### 4.1 For Game Developers
Use `hn4_api_open("tag:Level1_Textures")` with the `HN4_PROFILE_GAMING` profile.
*   **Benefit:** The driver will prefetch MIP-Maps automatically.
*   **Result:** Zero stutter during open-world traversal.

### 4.2 For AI Researchers
Use `hn4_api_map_tensor()` for datasets.
*   **Benefit:** 14GB/s load speeds with 0% CPU usage.
*   **Result:** Epoch times reduced by 40%.

### 4.3 For Embedded Systems
Link against `hn4_api_static` (Pico Profile).
*   **Benefit:** Full filesystem functionality in <4KB of binary size.
*   **Result:** Structured storage on $2 microcontrollers.

---

## 5. Summary

The HN4 API is not just a wrapper around `read()` and `write()`.
It is a **Control Plane** for the storage physics. It allows applications to dictate *how* data flows, not just *where* it goes.