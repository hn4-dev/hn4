/*
 * HYDRA-NEXUS 4 (HN4) - ALLOCATOR MATH PROOF SUITE
 * FILE: hn4_allocator_math_tests.c
 * STATUS: RIGOROUS MATHEMATICAL PROOFING
 *
 * SCOPE:
 * 1. Group Theory (Coprimality, Periodicity)
 * 2. Algebra Invariants (Associativity, Injectivity)
 * 3. Boundary Hardening (Overflow, Convergence)
 */

#include "hn4_test.h"
#include "hn4_hal.h"
#include "hn4.h"
#include "hn4_endians.h"
#include <string.h>
#include <stdlib.h> /* For abs() */

#define HN4_LBA_INVALID UINT64_MAX

/* --- MATH UTILITIES --- */

/* Extended Euclidean Algorithm for Modular Inverse */
static int64_t _math_extended_gcd(int64_t a, int64_t b, int64_t *x, int64_t *y) {
    if (a == 0) {
        *x = 0; *y = 1;
        return b;
    }
    int64_t x1, y1;
    int64_t gcd = _math_extended_gcd(b % a, a, &x1, &y1);
    *x = y1 - (b / a) * x1;
    *y = x1;
    return gcd;
}

/* Calculate (a^-1) mod m */
static uint64_t _math_mod_inverse(uint64_t a, uint64_t m) {
    int64_t x, y;
    _math_extended_gcd((int64_t)a, (int64_t)m, &x, &y);
    /* Handle negative results from EGCD */
    return (uint64_t)((x % (int64_t)m + (int64_t)m) % (int64_t)m);
}

/* --- FIXTURE SETUP --- */
#define MATH_BS 4096

typedef struct {
    hn4_hal_caps_t caps;
} mock_math_dev_t;

static hn4_volume_t* create_math_fixture(uint64_t phi) {
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    
    mock_math_dev_t* dev = hn4_hal_mem_alloc(sizeof(mock_math_dev_t));
    dev->caps.logical_block_size = 4096;
    dev->caps.total_capacity_bytes = (phi + 1000) * MATH_BS; 
    
    vol->target_device = dev;
    vol->vol_block_size = MATH_BS;
    vol->vol_capacity_bytes = dev->caps.total_capacity_bytes;
    
    /* 
     * Configure Flux Start such that Available Blocks = Phi.
     * Total = (Phi + 1000). Flux Start = 1000. Available = Phi.
     */
    vol->sb.info.lba_flux_start = 1000;
    
    return vol;
}

static void cleanup_math_fixture(hn4_volume_t* vol) {
    if (vol) {
        hn4_hal_mem_free(vol->target_device);
        hn4_hal_mem_free(vol);
    }
}

/* =========================================================================
 * 1. COPRIME ORBIT COVERAGE (Ergodicity Proof)
 * ========================================================================= */
hn4_TEST(Orbit, CoprimeFullCoverage) {
    /* Prime Phi ensures any V < Phi is coprime */
    uint64_t phi = 257; 
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 0;
    uint64_t V = 3; /* gcd(3, 257) = 1 */
    
    uint8_t* visited = hn4_hal_mem_alloc(phi);
    memset(visited, 0, phi);
    
    uint64_t unique_count = 0;
    
    for (uint64_t n = 0; n < phi; n++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        uint64_t offset = lba - vol->sb.info.lba_flux_start;
        
        ASSERT_TRUE(offset < phi);
        
        if (visited[offset] == 0) {
            visited[offset] = 1;
            unique_count++;
        } else {
            /* Duplicate found before Phi steps! */
            ASSERT_TRUE(false); 
        }
    }
    
    /* Ergodicity: Must visit exactly Phi locations */
    ASSERT_EQ(phi, unique_count);
    
    hn4_hal_mem_free(visited);
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 2. NON-COPRIME ORBIT COLLAPSE (Degeneracy Proof)
 * ========================================================================= */
hn4_TEST(Orbit, NonCoprimeDegeneracy_Mitigated) {
    uint64_t phi = 240;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 0;
    /* Use V=15 (Odd). Shares factor 15 with Phi (240). */
    uint64_t V = 15; 
    
    /*
     * OLD BEHAVIOR (Bug):
     * Stride = 15.
     * Orbit Size = Phi / GCD(15, 240) = 240 / 15 = 16.
     * This leaves 224 blocks unreachable (Holes).
     *
     * NEW BEHAVIOR (Fix):
     * Allocator detects GCD(15, 240) != 1.
     * Forces Stride = 1 (Linear).
     * Orbit Size = 240 (Full Coverage).
     */
    
    uint8_t* visited = (uint8_t*)hn4_hal_mem_alloc(phi);
    memset(visited, 0, phi);
    uint64_t unique_count = 0;
    
    for (uint64_t n = 0; n < phi; n++) {
        /* _calc_trajectory_lba now internally applies the fix */
        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        
        if (lba == HN4_LBA_INVALID) continue;

        uint64_t offset = lba - vol->sb.info.lba_flux_start;
        
        /* Sanity check bounds */
        if (offset < phi) {
            if (visited[offset] == 0) {
                visited[offset] = 1;
                unique_count++;
            }
        }
    }
    
    /* 
     * Verify that the mitigation logic restored full injectivity.
     * We expect to visit ALL 240 blocks, not just 16.
     */
    ASSERT_EQ(phi, unique_count);
    
    hn4_hal_mem_free(visited);
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 3. PERIODICITY PROOF (Cycle Length)
 * ========================================================================= */
hn4_TEST(Orbit, FullCyclePeriodicity) {
    uint64_t phi = 127; /* Prime */
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 50;
    uint64_t V = 5;
    
    uint64_t start_lba = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    uint64_t first_repeat = 0;
    
    /* Scan until we hit start again */
    for (uint64_t n = 1; n <= phi * 2; n++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        if (lba == start_lba) {
            first_repeat = n;
            break;
        }
    }
    
    /* Cycle length must equal Capacity for coprime V */
    ASSERT_EQ(phi, first_repeat);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 4. EQUIVALENCE IDENTITY (Algebraic Consistency)
 * ========================================================================= */
hn4_TEST(Orbit, AlgebraicEquivalence) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t base = vol->sb.info.lba_flux_start;
    
    uint64_t G = 12345;
    uint64_t V = 6789;
    
    /* Randomized checks */
    for (int i = 0; i < 100; i++) {
        uint64_t N = (uint64_t)hn4_hal_get_random_u64();
        
        /* 1. Implementation Result */
        uint64_t impl_lba = _calc_trajectory_lba(vol, G, V, N, 0, 0);
        uint64_t impl_off = impl_lba - base;
        
        /* 2. Manual Modulo Arithmetic */
        /* (G + N*V) % Phi */
        /* We simulate the internal 128-bit safe math here */
        uint64_t term_n = N % phi;
        uint64_t term_v = V % phi;
        uint64_t product = (term_n * term_v) % phi;
        uint64_t term_g = (G / 1) % phi; /* Scale M=0 -> S=1 */
        uint64_t manual_off = (term_g + product) % phi;
        
        ASSERT_EQ(manual_off, impl_off);
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 5. THETA INDEPENDENCE (Orbit Stability)
 * ========================================================================= */
hn4_TEST(Orbit, ThetaShiftInvariance) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 10;
    uint64_t V = 1;
    
    /* 
     * Calculate orbit for k=0 and k=1.
     * Theta(0)=0, Theta(1)=1.
     * The orbits should be identical in shape, just shifted by (Theta1 - Theta0).
     */
    uint64_t lba_k0 = _calc_trajectory_lba(vol, G, V, 5, 0, 0);
    uint64_t lba_k1 = _calc_trajectory_lba(vol, G, V, 5, 0, 1);
    
    /* Shift should be exactly 1 block (modulo wrap handled) */
    uint64_t diff = (lba_k1 >= lba_k0) ? (lba_k1 - lba_k0) : (lba_k1 + phi - lba_k0);
    
    ASSERT_EQ(1ULL, diff);
    
    /* Verify for k=2 (Theta=3). Shift from k=0 should be 3. */
    uint64_t lba_k2 = _calc_trajectory_lba(vol, G, V, 5, 0, 2);
    diff = (lba_k2 >= lba_k0) ? (lba_k2 - lba_k0) : (lba_k2 + phi - lba_k0);
    ASSERT_EQ(3ULL, diff);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 6. INJECTIVITY GUARANTEE (No Collision for distinct N)
 * ========================================================================= */
hn4_TEST(Orbit, InjectivityGuarantee) {
    uint64_t phi = 251; /* Prime */
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 100;
    uint64_t V = 10;
    
    uint64_t lba_n10 = _calc_trajectory_lba(vol, G, V, 10, 0, 0);
    uint64_t lba_n20 = _calc_trajectory_lba(vol, G, V, 20, 0, 0);
    
    /* Distinct N must map to distinct LBA (if N < Phi) */
    ASSERT_NEQ(lba_n10, lba_n20);
    
    /* Mathematical Proof: T(N1) == T(N2) implies N1 == N2 mod Phi */
    /* Check modulo equality */
    uint64_t lba_n10_wrap = _calc_trajectory_lba(vol, G, V, 10 + phi, 0, 0);
    ASSERT_EQ(lba_n10, lba_n10_wrap);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 10. 128-BIT OVERFLOW DETERMINISM
 * ========================================================================= */
hn4_TEST(Orbit, OverflowDeterminism) {
    /* Use a small Phi to force wrapping */
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    /* Inputs that exceed 64-bit when multiplied */
    uint64_t N = 0xFFFFFFFFFFFFFFFFULL; /* Max U64 */
    uint64_t V = 0xFFFFFFFFFFFFFFFFULL; /* Max U64 */
    uint64_t G = 0;
    
    /* Calculation 1 */
    uint64_t res1 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    /* Calculation 2 (Repeat) */
    uint64_t res2 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    /* Must be deterministic */
    ASSERT_EQ(res1, res2);
    
    /* Must be valid (fit in Phi) */
    ASSERT_TRUE((res1 - vol->sb.info.lba_flux_start) < phi);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 11. BIJECTIVE RECOVERABILITY (Reversibility)
 * ========================================================================= */
hn4_TEST(Orbit, BijectiveRecoverability) {
    uint64_t phi = 257; /* Prime */
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t base = vol->sb.info.lba_flux_start;
    
    uint64_t G = 50;
    uint64_t V = 3;
    uint64_t N_original = 42;
    
    /* 1. Forward: N -> LBA */
    uint64_t lba = _calc_trajectory_lba(vol, G, V, N_original, 0, 0);
    uint64_t X = lba - base;
    
    /* 2. Reverse: LBA -> N */
    /* Formula: N = (X - G) * V^-1 mod Phi */
    
    /* Calculate modular inverse of V mod Phi */
    uint64_t V_inv = _math_mod_inverse(V, phi);
    
    /* (X - G) mod Phi */
    /* Handle negative wrap carefully */
    int64_t diff = (int64_t)X - (int64_t)G;
    while (diff < 0) diff += phi;
    uint64_t term = (uint64_t)diff % phi;
    
    uint64_t N_recovered = (term * V_inv) % phi;
    
    ASSERT_EQ(N_original, N_recovered);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 13. K-LAYER MONOTONIC ESCAPE
 * ========================================================================= */
hn4_TEST(Orbit, ContextLayerMonotonicity) {
    hn4_volume_t* vol = create_math_fixture(1000);
    uint64_t G = 500;
    uint64_t V = 1;
    
    /* Verify K=0,1,2,3... do not oscillate or stall */
    uint64_t prev = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    
    for (int k = 1; k < 12; k++) {
        uint64_t curr = _calc_trajectory_lba(vol, G, V, 0, 0, k);
        
        /* Must change */
        ASSERT_NEQ(prev, curr);
        
        /* In linear mode (V=1, Theta increasing), it should increase monotonically (wrapping aside) */
        /* Since Phi=1000 and G=500, we won't wrap for small k. */
        ASSERT_TRUE(curr > prev);
        
        prev = curr;
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 15. HORIZON BOUNDARY CONVERGENCE
 * ========================================================================= */
hn4_TEST(Orbit, HorizonBoundaryIntegrity) {
    /* Verify extreme boundary conditions do not leak past Phi */
    uint64_t phi = 10; /* Very small */
    hn4_volume_t* vol = create_math_fixture(phi);
    
    /* Inputs that would naturally land exactly on the boundary or past it */
    uint64_t G = 9;
    uint64_t V = 1;
    uint64_t N = 1; /* Target 10 (Phi) -> Should wrap to 0 */
    
    uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    uint64_t offset = lba - vol->sb.info.lba_flux_start;
    
    /* Must wrap to 0 */
    ASSERT_EQ(0ULL, offset);
    ASSERT_TRUE(offset < phi);
    
    cleanup_math_fixture(vol);
}


/* =========================================================================
 * 16. MULTI-PHI STABILITY SWEEP
 * ========================================================================= */
hn4_TEST(Orbit, MultiPhiStability) {
    /* Sweep Phi from small to large */
    uint64_t phis[] = {17, 255, 256, 1024, 65537, 0}; 
    
    for (int i = 0; phis[i] != 0; i++) {
        uint64_t phi = phis[i];
        hn4_volume_t* vol = create_math_fixture(phi);
        
        for (int j = 0; j < 100; j++) {
            uint64_t G = hn4_hal_get_random_u64() % phi;
            uint64_t V = hn4_hal_get_random_u64() | 1; /* Odd */
            uint64_t N = hn4_hal_get_random_u64();
            
            uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, 0);
            uint64_t offset = lba - vol->sb.info.lba_flux_start;
            
            /* Verify Bounds */
            ASSERT_TRUE(offset < phi);
        }
        cleanup_math_fixture(vol);
    }
}

/* =========================================================================
 * 17. M-SCALE CONSISTENCY
 * ========================================================================= */
hn4_TEST(FractalMath, M_Scale_Consistency) {
    hn4_volume_t* vol = create_math_fixture(1ULL << 20); /* Large Phi */
    uint64_t G = 0;
    uint64_t V = 1;
    
    /* Sweep M 0..16 */
    for (uint16_t M = 0; M <= 16; M++) {
        uint64_t stride = 1ULL << M;
        uint64_t lba_0 = _calc_trajectory_lba(vol, G, V, 0, M, 0);
        uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, M, 0);
        
        /* 
         * Verify stride.
         * Note: _calc_trajectory might wrap if Phi is small relative to M-scale.
         * Our Phi is large (1M blocks), so for small M no wrap.
         * For M=16 (256MB), 1M blocks is ~16 superblocks. No wrap for N=1.
         */
        ASSERT_EQ(stride, lba_1 - lba_0);
    }
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 19. TWO-POINT SEPARATION INVARIANCE
 * ========================================================================= */
hn4_TEST(Orbit, TwoPointSeparation) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 100;
    uint64_t V = 3;
    
    uint64_t N1 = 10;
    uint64_t N2 = 20;
    /* Expected separation mod Phi: (N2 - N1) * V = 10 * 3 = 30 */
    
    uint64_t lba1 = _calc_trajectory_lba(vol, G, V, N1, 0, 0);
    uint64_t lba2 = _calc_trajectory_lba(vol, G, V, N2, 0, 0);
    
    uint64_t diff = (lba2 >= lba1) ? (lba2 - lba1) : (lba2 + phi - lba1);
    
    ASSERT_EQ(30ULL, diff);
    
    /* Shift G (Affine Transform) */
    G += 500;
    uint64_t lba1_b = _calc_trajectory_lba(vol, G, V, N1, 0, 0);
    uint64_t lba2_b = _calc_trajectory_lba(vol, G, V, N2, 0, 0);
    uint64_t diff_b = (lba2_b >= lba1_b) ? (lba2_b - lba1_b) : (lba2_b + phi - lba1_b);
    
    ASSERT_EQ(diff, diff_b);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 20. SMALL-PHI PATHOLOGY
 * ========================================================================= */
hn4_TEST(Orbit, SmallPhiPathology) {
    /* Test Phi 1..16 */
    for (uint64_t phi = 1; phi <= 16; phi++) {
        hn4_volume_t* vol = create_math_fixture(phi);
        
        for (int i = 0; i < 50; i++) {
            uint64_t G = i; 
            uint64_t V = i + 1;
            uint64_t N = i * 2;
            
            uint64_t lba = _calc_trajectory_lba(vol, G, V, N, 0, 0);
            uint64_t offset = lba - vol->sb.info.lba_flux_start;
            
            /* Crucial: Must be < Phi (No OOB) */
            ASSERT_TRUE(offset < phi);
        }
        cleanup_math_fixture(vol);
    }
}

/* =========================================================================
 * 21. CONSECUTIVE-G CONTINUITY
 * ========================================================================= */
hn4_TEST(Orbit, ConsecutiveG_Continuity) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t V = 7;
    uint64_t N = 5;
    
    uint64_t lba_base = _calc_trajectory_lba(vol, 0, V, N, 0, 0);
    
    for (uint64_t g = 1; g < 10; g++) {
        uint64_t lba = _calc_trajectory_lba(vol, g, V, N, 0, 0);
        uint64_t diff = (lba >= lba_base) ? (lba - lba_base) : (lba + phi - lba_base);
        
        /* G acts as linear offset. Delta G = g. Delta LBA should be g. */
        ASSERT_EQ(g, diff);
    }
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 26. IDEMPOTENCE TEST
 * ========================================================================= */
hn4_TEST(Orbit, Idempotence) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 123;
    uint64_t V = 456;
    uint64_t N = 789;
    
    uint64_t first_result = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    for (int i = 0; i < 1000; i++) {
        uint64_t res = _calc_trajectory_lba(vol, G, V, N, 0, 0);
        ASSERT_EQ(first_result, res);
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 27. NUMERICAL STABILITY (Power-of-2 Boundaries)
 * ========================================================================= */
hn4_TEST(Orbit, BinaryBoundaryStability) {
    uint64_t boundaries[] = {255, 256, 257, 0};
    
    for (int i = 0; boundaries[i] != 0; i++) {
        uint64_t phi = boundaries[i];
        hn4_volume_t* vol = create_math_fixture(phi);
        
        uint64_t G = 1;
        uint64_t V = 1;
        
        /* Verify wrapping logic around Phi */
        /* N = Phi-1 -> LBA = Phi-1 + 1 = Phi+0. Wait. G=1. */
        /* (1 + (Phi-1)*1) % Phi = (1 + Phi - 1) % Phi = 0. */
        /* LBA should be Start + 0. */
        
        uint64_t lba = _calc_trajectory_lba(vol, G, V, phi - 1, 0, 0);
        uint64_t offset = lba - vol->sb.info.lba_flux_start;
        
        ASSERT_EQ(0ULL, offset);
        
        cleanup_math_fixture(vol);
    }
}

/* =========================================================================
 * 28. ILLEGAL INPUT GUARDS
 * ========================================================================= */
hn4_TEST(Orbit, IllegalInputGuards) {
    /* Phi = 0 Case (Usually handled by return INVALID or 0) */
    /* Create fixture with 0 capacity? */
    /* Spec says _calc_trajectory checks Phi=0. */
    
    hn4_volume_t* vol = hn4_hal_mem_alloc(sizeof(hn4_volume_t));
    memset(vol, 0, sizeof(hn4_volume_t));
    mock_math_dev_t* dev = hn4_hal_mem_alloc(sizeof(mock_math_dev_t));
    vol->target_device = dev;
    dev->caps.logical_block_size = 4096;
    vol->vol_block_size = 4096;
    
    /* Set Total = Flux Start -> Phi = 0 */
    vol->sb.info.lba_flux_start = 100;
    vol->vol_capacity_bytes = 100 * 4096;
    
    uint64_t lba = _calc_trajectory_lba(vol, 0, 1, 0, 0, 0);
    
    /* Should return Error Sentinel (UINT64_MAX) */
    ASSERT_EQ(UINT64_MAX, lba);
    
    hn4_hal_mem_free(dev);
    hn4_hal_mem_free(vol);
}

/* =========================================================================
 * 34. ORBIT AUTOCORRELATION / UNIFORMITY (Step Delta Histogram)
 * ========================================================================= */
hn4_TEST(Orbit, AutocorrelationUniformity) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 50;
    uint64_t V = 3; /* Coprime */
    
    /* Histogram of step deltas */
    uint64_t* deltas = hn4_hal_mem_alloc(phi * sizeof(uint64_t));
    memset(deltas, 0, phi * sizeof(uint64_t));
    
    uint64_t prev = _calc_trajectory_lba(vol, G, V, 0, 0, 0);
    
    for (uint64_t n = 1; n <= phi; n++) {
        uint64_t curr = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        
        /* Delta mod Phi */
        /* Logic: If curr >= prev, diff = curr-prev. Else diff = curr+phi-prev. */
        uint64_t diff = (curr >= prev) ? (curr - prev) : (curr + phi - prev);
        
        deltas[diff]++;
        prev = curr;
    }
    
    /* 
     * Since V=3 and K=0 (Linear), all steps should be exactly V (3).
     * If Theta or K>0 introduced bias, we'd see variance.
     * For K=0, expect ALL steps to be 3.
     */
    ASSERT_EQ(phi, deltas[3]);
    
    /* Verify no other deltas occurred */
    for (uint64_t i = 0; i < phi; i++) {
        if (i != 3) ASSERT_EQ(0ULL, deltas[i]);
    }
    
    hn4_hal_mem_free(deltas);
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 35. COMPOSED ORBIT EQUIVALENCE (Affine Space)
 * ========================================================================= */
hn4_TEST(Orbit, AffineSpaceProof) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 100;
    uint64_t V = 10;
    uint64_t N = 5;
    
    uint64_t base = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    uint64_t origin = vol->sb.info.lba_flux_start;
    
    /* Property 1: T(G+a) == T(G) + a */
    uint64_t a = 50;
    uint64_t res1 = _calc_trajectory_lba(vol, G + a, V, N, 0, 0);
    uint64_t expected1 = base + a; 
    /* Handle wrap if base+a exceeds boundary, though calc handles it internally */
    /* Let's compare offsets */
    ASSERT_EQ((res1 - origin), (expected1 - origin) % phi);
    
    /* Property 2: T(V+b) == T(V) + b*N */
    uint64_t b = 2;
    uint64_t res2 = _calc_trajectory_lba(vol, G, V + b, N, 0, 0);
    uint64_t offset_shift = (b * N) % phi;
    
    /* Diff modulo phi */
    uint64_t diff = (res2 >= base) ? (res2 - base) : (res2 + phi - base);
    ASSERT_EQ(offset_shift, diff);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 36. COMMUTATIVITY UNDER PROJECTION (Time Composition)
 * ========================================================================= */
hn4_TEST(Orbit, TimeComposition) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint64_t G = 10;
    uint64_t V = 3;
    uint64_t N1 = 5;
    uint64_t N2 = 7;
    
    /* LHS: T(N1 + N2) */
    uint64_t lhs = _calc_trajectory_lba(vol, G, V, N1 + N2, 0, 0);
    
    /* RHS: T( T(N1), N2 ) -- i.e., use end of N1 as new G for N2 */
    /* Note: T(N1) returns absolute LBA. We need relative offset for new G. */
    uint64_t intermediate_lba = _calc_trajectory_lba(vol, G, V, N1, 0, 0);
    uint64_t new_G = intermediate_lba - vol->sb.info.lba_flux_start;
    
    /* Now apply N2 steps from new_G */
    uint64_t rhs = _calc_trajectory_lba(vol, new_G, V, N2, 0, 0);
    
    /* This proves T(G, V, N1+N2) == T( T(G,V,N1).offset, V, N2 ) */
    ASSERT_EQ(lhs, rhs);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 39. TIME-REVERSAL SYMMETRY (Inverse Mapping)
 * ========================================================================= */
hn4_TEST(Orbit, TimeReversalSymmetry) {
    uint64_t phi = 257; /* Prime */
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 20;
    uint64_t V = 5;
    uint64_t N = 10;
    
    /* Forward T(N) -> X */
    uint64_t X_lba = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    uint64_t X = X_lba - start;
    
    /* Inverse Function: T^-1(X) = (X - G) * inv(V) */
    uint64_t V_inv = _math_mod_inverse(V, phi);
    int64_t diff = (int64_t)X - (int64_t)G;
    while(diff < 0) diff += phi;
    
    uint64_t N_recovered = ((uint64_t)diff * V_inv) % phi;
    
    ASSERT_EQ(N, N_recovered);
    
    /* Verify Loop: T( T^-1(X) ) == X */
    uint64_t loop_lba = _calc_trajectory_lba(vol, G, V, N_recovered, 0, 0);
    ASSERT_EQ(X_lba, loop_lba);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 40. NEAR-SINGULARITY (V = Phi - 1)
 * ========================================================================= */
hn4_TEST(Orbit, NearSingularity_ReverseScan) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 50;
    uint64_t V = phi - 1; /* Effectively -1 */
    
    /* N=1. Expected: G - 1 = 49 */
    uint64_t lba_1 = _calc_trajectory_lba(vol, G, V, 1, 0, 0);
    ASSERT_EQ(start + 49, lba_1);
    
    /* N=10. Expected: G - 10 = 40 */
    uint64_t lba_10 = _calc_trajectory_lba(vol, G, V, 10, 0, 0);
    ASSERT_EQ(start + 40, lba_10);
    
    /* N=51. Expected: 50 - 51 = -1 -> Wrap to 99 */
    uint64_t lba_51 = _calc_trajectory_lba(vol, G, V, 51, 0, 0);
    ASSERT_EQ(start + 99, lba_51);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 42. PHI DECOMPOSITION (CRT Behavior)
 * ========================================================================= */
hn4_TEST(Orbit, PhiDecomposition_CRT) {
    /* Phi = 15 = 3 * 5 */
    uint64_t p = 3, q = 5;
    uint64_t phi = p * q;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 0;
    uint64_t V = 1;
    
    /* 
     * Verify that trajectory mod p follows sub-orbit p 
     * and trajectory mod q follows sub-orbit q
     */
    for (uint64_t n = 0; n < phi; n++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        uint64_t val = lba - start;
        
        /* Check mod p property: T(n) % p == (G + N*V) % p */
        /* Here G=0, V=1, so val % p == n % p */
        ASSERT_EQ(val % p, n % p);
        
        /* Check mod q */
        ASSERT_EQ(val % q, n % q);
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 48. ADVERSARIAL V PATTERNS (Hostile Strides)
 * ========================================================================= */
hn4_TEST(Orbit, AdversarialStrides) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 0;
    uint64_t hostiles[] = {5, 25, 125, 995, 0};
    
    for (int i = 0; hostiles[i] != 0; i++) {
        uint64_t V = hostiles[i];
        
        /* 
         * OLD BEHAVIOR: 
         * Orbit size = Phi / gcd(V, Phi). (e.g., V=5 -> Size=200).
         * This resulted in massive aliasing and HN4_ERR_ENOSPC on empty disks.
         *
         * NEW BEHAVIOR (Spec 18.1 Coprimality Enforcement):
         * The allocator detects that V shares factors with Phi (gcd != 1).
         * It dynamically downgrades V to 1 (Linear Scan).
         * Result: Injectivity is restored. We must visit ALL blocks.
         */
        
        uint64_t visited_count = 0;
        uint8_t* map = (uint8_t*)hn4_hal_mem_alloc(phi);
        memset(map, 0, phi);
        
        for (uint64_t n = 0; n < phi; n++) {
            /* Using k=0 (Primary Orbit) */
            uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
            
            /* Sanity check bounds */
            if (lba == HN4_LBA_INVALID) continue;
            
            uint64_t idx = lba - start;
            if (idx < phi) {
                if (map[idx] == 0) {
                    map[idx] = 1;
                    visited_count++;
                }
            }
        }
        
        /* 
         * ASSERTION CHANGE:
         * Verify that the "Bad" vector was neutralized.
         * We expect 100% coverage (phi), not partial coverage.
         */
        ASSERT_EQ(phi, visited_count);
        
        hn4_hal_mem_free(map);
    }
    
    cleanup_math_fixture(vol);
}


/* =========================================================================
 * 51. OFFSET-INVARIANT ORBIT EQUIVALENCE (Topological Homomorphism)
 * ========================================================================= */
hn4_TEST(Orbit, TopologicalHomomorphism) {
    uint64_t phi = 1000;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 100, V = 13, N = 50;
    uint64_t dG = 20, dN = 5;
    
    /* LHS: T(G + dG, V, N + dN) - T(G, V, N) */
    uint64_t lba1 = _calc_trajectory_lba(vol, G + dG, V, N + dN, 0, 0);
    uint64_t lba2 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    
    uint64_t val1 = lba1 - start;
    uint64_t val2 = lba2 - start;
    
    uint64_t diff_lhs = (val1 >= val2) ? (val1 - val2) : (val1 + phi - val2);
    
    /* RHS: dG + dN*V (mod Phi) */
    uint64_t term_dN = (dN * V) % phi;
    uint64_t term_dG = dG % phi;
    uint64_t diff_rhs = (term_dG + term_dN) % phi;
    
    ASSERT_EQ(diff_lhs, diff_rhs);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 52. MULTI-SCALE CONSISTENCY (Fractal Integrity)
 * ========================================================================= */
hn4_TEST(FractalMath, CrossLevelIntegrity) {
    /* Large Phi to accommodate M=5 stride */
    uint64_t phi = 1ULL << 20; 
    hn4_volume_t* vol = create_math_fixture(phi);
    
    uint16_t M1 = 0;
    uint16_t M2 = 5; /* Stride 32 */
    uint64_t stride = 1ULL << (M2 - M1); /* 32 */
    
    uint64_t G = 1024 * 32; /* Aligned base */
    uint64_t V = 1;
    uint64_t N = 10;
    
    /* Term 1: (T(G,V,N,M2) - T(G,V,0,M2)) / 32 */
    uint64_t t1_base = _calc_trajectory_lba(vol, G, V, 0, M2, 0);
    uint64_t t1_curr = _calc_trajectory_lba(vol, G, V, N, M2, 0);
    uint64_t diff1 = (t1_curr - t1_base) / stride;
    
    /* Term 2: T(G/32, V, N, M1) - T(G/32, V, 0, M1) */
    uint64_t g_scaled = G / stride;
    uint64_t t2_base = _calc_trajectory_lba(vol, g_scaled, V, 0, M1, 0);
    uint64_t t2_curr = _calc_trajectory_lba(vol, g_scaled, V, N, M1, 0);
    uint64_t diff2 = t2_curr - t2_base;
    
    /* Self-similarity check */
    ASSERT_EQ(diff1, diff2);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 53. HALO EFFECT GUARD (Edge Stability)
 * ========================================================================= */
hn4_TEST(Orbit, EdgeHuggingStability) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    /* Test G near 0 and G near Phi */
    uint64_t edges[] = {0, 1, 2, phi-1, phi-2, phi-3};
    
    for (int i = 0; i < 6; i++) {
        uint64_t G = edges[i];
        uint64_t V = 1;
        
        /* Check first few steps for valid wrap */
        for (uint64_t n = 0; n < 5; n++) {
            uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
            uint64_t off = lba - start;
            
            /* Must be strictly in bounds */
            ASSERT_TRUE(off < phi);
            
            /* Expected: (G + n) % Phi */
            uint64_t expected = (G + n) % phi;
            ASSERT_EQ(expected, off);
        }
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 58. LATTICE FACTORIZATION ISOLATION
 * ========================================================================= */
hn4_TEST(Orbit, LatticeFactorization) {
    /* Composite Phi = 6 * 7 = 42 */
    uint64_t a = 6, b = 7;
    uint64_t phi = a * b;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 0, V = 1;
    
    /* Verify sub-periodicity */
    /* Orbit mod a should have period a */
    /* Orbit mod b should have period b */
    
    for (uint64_t n = 0; n < phi; n++) {
        uint64_t lba = _calc_trajectory_lba(vol, G, V, n, 0, 0);
        uint64_t val = lba - start;
        
        /* Modulo a */
        ASSERT_EQ(val % a, n % a);
        
        /* Modulo b */
        ASSERT_EQ(val % b, n % b);
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 59. LATIN-SQUARE ORDERING PROOF
 * ========================================================================= */
hn4_TEST(Orbit, LatinSquareProof) {
    uint64_t phi = 17; /* Prime for clean square */
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    uint64_t V = 3;
    
    /* Grid[G][N] */
    uint8_t row_seen[17][17]; /* [Row][Val] */
    uint8_t col_seen[17][17]; /* [Col][Val] */
    memset(row_seen, 0, sizeof(row_seen));
    memset(col_seen, 0, sizeof(col_seen));
    
    for (uint64_t g = 0; g < phi; g++) {
        for (uint64_t n = 0; n < phi; n++) {
            uint64_t lba = _calc_trajectory_lba(vol, g, V, n, 0, 0);
            uint64_t val = lba - start;
            
            /* Row Uniqueness (Fixed G, Vary N) -> Orbit */
            ASSERT_EQ(0, row_seen[g][val]);
            row_seen[g][val] = 1;
            
            /* Column Uniqueness (Fixed N, Vary G) -> Shift */
            ASSERT_EQ(0, col_seen[n][val]);
            col_seen[n][val] = 1;
        }
    }
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 60. SYMMETRY-BREAKING (Anti-Mirroring)
 * ========================================================================= */
hn4_TEST(Orbit, SymmetryBreaking) {
    uint64_t phi = 100;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 10;
    uint64_t V = 3;
    uint64_t N = 5;
    
    /* T(G, V, N) */
    uint64_t lba1 = _calc_trajectory_lba(vol, G, V, N, 0, 0);
    uint64_t v1 = lba1 - start;
    
    /* T(G, -V, Phi - N) */
    /* -V mod Phi = Phi - V */
    uint64_t V_neg = phi - V;
    uint64_t N_inv = phi - N;
    
    uint64_t lba2 = _calc_trajectory_lba(vol, G, V_neg, N_inv, 0, 0);
    uint64_t v2 = lba2 - start;
    
    /* 
     * Calc: G + (Phi-V)*(Phi-N) 
     *     = G + Phi^2 - Phi*N - V*Phi + V*N 
     *     = G + V*N (mod Phi)
     * Wait, mathematically they ARE equal if Theta=0.
     * T(G, -V, -N) = G + (-V)*(-N) = G + VN.
     * 
     * If the goal is to prove they are NOT equal, then Theta must break symmetry
     * or the test prompt implies a specific mirroring property check.
     * 
     * "Orbit is not mirror symmetric".
     * Let's check T(G, V, N) vs T(G, V, Phi-N).
     */
    
    uint64_t lba3 = _calc_trajectory_lba(vol, G, V, phi - N, 0, 0);
    uint64_t v3 = lba3 - start;
    
    /* T(N) should not equal T(-N) unless N=0 or N=Phi/2 */
    ASSERT_NEQ(v1, v3);
    
    cleanup_math_fixture(vol);
}

/* =========================================================================
 * 65. GOLDEN EXAMPLE SUITE (Canonical Values)
 * ========================================================================= */
hn4_TEST(Orbit, GoldenCanonicalSuite) {
    /* 
     * Canonical Case: Phi=31, G=5, V=3
     * Sequence: 5, 8, 11... 
     * 5 + 30*3 = 95. 95 % 31 = 2.
     */
    uint64_t phi = 31;
    hn4_volume_t* vol = create_math_fixture(phi);
    uint64_t start = vol->sb.info.lba_flux_start;
    
    uint64_t G = 5;
    uint64_t V = 3;
    
    struct { uint64_t n; uint64_t expected; } cases[] = {
        {0, 5},
        {1, 8},
        {10, (5 + 30) % 31}, /* 35 % 31 = 4 */
        {30, (5 + 90) % 31}, /* 95 % 31 = 2 */
        {0, 0} /* Sentinel */
    };
    
    /* Fix calc for test struct init limitations in C90/C99 if computed above */
    cases[2].expected = 4;
    cases[3].expected = 2;
    
    for (int i = 0; cases[i].n != 0 || i == 0; i++) {
        if (i > 0 && cases[i].n == 0) break; 
        
        uint64_t lba = _calc_trajectory_lba(vol, G, V, cases[i].n, 0, 0);
        uint64_t val = lba - start;
        
        ASSERT_EQ(cases[i].expected, val);
    }
    
    cleanup_math_fixture(vol);
}