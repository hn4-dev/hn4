# Contributing to HYDRA-NEXUS 4 (HN4)

We welcome contributors who are tired of waiting for `fsck` to finish.
This project is an experiment in **High-Velocity Storage Physics**. We break the rules of traditional file systems, but we respect the laws of engineering.

We don’t expect perfection — we expect thoughtfulness.

---

### ⚠️ The Hard Rules (What NOT To Do)

If your PR does any of the following, it will be closed — safety first.
*   ❌ **Blocking IO:** Do not introduce blocking syscalls or long mutex holds in the IO hot path. We aim for lock-free velocity.
*   ❌ **Dependency Creep:** Do not add libraries (like `openssl` or `glib`) "for convenience." We run on bare metal.
*   ❌ **Safety Trade-offs:** Do not disable CRC checks or ECC to gain 1% throughput. Speed without integrity is just corruption.
*   ❌ **Untested Code:** Do not submit logic changes without a corresponding test case in `tests/`.

*If you’re unsure whether your change breaks a rule, open an Issue first — we’re happy to help guide the design.*

---

### 🔬 How to Prove Your Claims

"It feels faster" is not a benchmark. If you claim a performance improvement, you must provide:
1.  **Workload:** `seq-read`, `rand-read`, `seq-write`, and `rand-write`.
2.  **State:** Benchmark must run at **10%**, **70%**, and **95%** disk utilization to prove stability under load.
3.  **Ratio:** Dataset size must exceed RAM size (e.g., 32GB file on 16GB RAM) to test the disk, not the cache.
4.  **Format:** Output from `fio` or equivalent.

---

### 🛡️ The Guarantee (Failure Semantics)

**HN4 guarantees on-disk consistency at the metadata layer.**
*   A partial write or power failure must **NEVER** render the volume unmountable.
*   Data integrity is enforced via the Holo-Lattice. If data is corrupt, we return `EIO` or repair it; we never return garbage.

---

### 🧠 Tone & Humor

We are building a sci-fi filesystem, so the terminology is colorful (`Gravity Well`, `Event Horizon`, `Void Engine`).
*   **Humor is welcome.** Feel free to name your helper function `warp_speed()`.
*   **Clarity is mandatory.** If the joke makes the code hard to read or debug, the joke dies.

---

### 🔒 Gatekeeper Zones

The following subsystems are the "Nuclear Core" of HN4. Changes here require deep review and sign-off from a core maintainer:
*   **The Allocator:** `hn4_allocator.c` (Void Engine)
*   **The Write Path:** `hn4_write.c` (IO Kinetics)
*   **Crypto/Integrity:** `hn4_crypto.c` (AES-Ballistic / Helix)
*   **Recovery:** `hn4_mount.c` (Epoch Rollback)

Drive-by rewrites of these files will be scrutinized heavily.

---

### 🔰 Good First Issues

Want to help but scared of the math? Start here:
*   **Portability:** Help us compile on weird architectures (RISC-V, MIPS).
*   **Static Analysis:** Fix warnings from `cppcheck` or `scan-build`.
*   **Test Coverage:** Write a test that tries to break the formatter with invalid inputs.
*   **Docs:** Fix typos in the Specification.

---

### 🧪 Threat Model

We design against the following adversaries:
1.  **Entropy (Rot):** Cosmic rays, NAND bitflips, cable noise. (Mitigation: ECC/CRC).
2.  **Physics (Power):** Sudden power loss during write. (Mitigation: Epoch/Shadow Hop).
3.  **User Error:** Accidental deletions. (Mitigation: Time Travel/Tombstones).
4.  **WE DO NOT DEFEND AGAINST:** An attacker with root access to the kernel memory who actively modifies the driver in RAM. If they are in Ring 0, you have lost.

**Let's build the future of storage.**