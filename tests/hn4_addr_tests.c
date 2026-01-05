/*
 * HYDRA-NEXUS 4 (HN4) - ADDRESS PRIMITIVES TESTS
 * FILE: hn4_addr_tests.c
 * STATUS: LOGIC VERIFICATION
 */

#include "hn4_test.h"
#include "hn4_addr.h"
#include "hn4_errors.h"
#include <limits.h>

/* =========================================================================
 * TEST 1: Basic Address Conversion (Round Trip)
 * Rationale:
 * Verify that converting a standard 64-bit integer to the internal
 * hn4_addr_t representation and back preserves the value perfectly.
 * This ensures the abstraction layer is transparent for normal values.
 * ========================================================================= */
hn4_TEST(Address, RoundTrip) {
    uint64_t input = 0xDEADBEEFCAFEBABEULL;
    
    hn4_addr_t addr = hn4_addr_from_u64(input);
    uint64_t output = hn4_addr_to_u64(addr);
    
    ASSERT_EQ(input, output);
}

/* =========================================================================
 * TEST 2: Overflow Safety (128-bit to 64-bit Downcast)
 * Rationale:
 * When HN4_USE_128BIT is enabled, the system tracks high-bits.
 * If an address exceeds the 64-bit range (Exabytes), attempting to
 * cast it down to a uint64_t (legacy interface) MUST fail safely
 * to prevent aliasing/wrap-around bugs.
 * 
 * Note: If 128-bit is disabled, this test is trivial but passes.
 * If enabled, we manually construct a high-bit address to test the guard.
 * ========================================================================= */
hn4_TEST(Address, OverflowGuard) {
#ifdef HN4_USE_128BIT
    hn4_addr_t huge_addr;
    huge_addr.lo = 100;
    huge_addr.hi = 1; /* Exceeds 64-bit space */

    uint64_t res = hn4_addr_to_u64(huge_addr);
    
    /* Expect Sentinel Error Value */
    ASSERT_EQ(UINT64_MAX, res);
#else
    /* 
     * In 64-bit native mode, we can't represent an overflow 
     * inside the struct, so this test just verifies identity.
     */
    hn4_addr_t addr = 100;
    uint64_t res = hn4_addr_to_u64(addr);
    ASSERT_EQ(100ULL, res);
#endif
}

/* =========================================================================
 * TEST 3: Arithmetic Carry Propagation
 * Rationale:
 * Verify that adding a value that causes the lower 64-bits to wrap around
 * correctly increments the upper 64-bits (if in 128-bit mode), or
 * behaves as standard modulo arithmetic (if in 64-bit mode).
 * ========================================================================= */
hn4_TEST(Address, ArithmeticCarry) {
    /* Set base to Max uint64 - 10 */
    hn4_addr_t base = hn4_addr_from_u64(UINT64_MAX - 10);
    
    /* Add 20. This causes a wrap-around of the low 64 bits. */
    hn4_addr_t result = hn4_addr_add(base, 20);

#ifdef HN4_USE_128BIT
    /* 
     * 128-bit Logic:
     * Lo should wrap to 9.
     * Hi should increment to 1.
     */
    ASSERT_EQ(9ULL, result.lo);
    ASSERT_EQ(1ULL, result.hi);
#else
    /* 
     * 64-bit Logic:
     * Standard wrap-around behavior.
     */
    ASSERT_EQ(9ULL, result);
#endif
}

/* =========================================================================
 * TEST 4: Semantic Helpers (Blocks vs Sectors)
 * Rationale:
 * Verify that the semantic helpers `hn4_lba_from_blocks` and 
 * `hn4_lba_from_sectors` produce valid hn4_addr_t types and correctly
 * handle input values. While currently simple wrappers, testing them ensures
 * ABI stability if the internal representation changes.
 * ========================================================================= */
hn4_TEST(Address, SemanticHelpers) {
    uint64_t val = 123456789;

    hn4_addr_t from_blk = hn4_lba_from_blocks(val);
    hn4_addr_t from_sec = hn4_lba_from_sectors(val);

    /* Verify conversion back to raw u64 works */
    ASSERT_EQ(val, hn4_addr_to_u64(from_blk));
    ASSERT_EQ(val, hn4_addr_to_u64(from_sec));

#ifdef HN4_USE_128BIT
    ASSERT_EQ(0ULL, from_blk.hi);
#endif
}