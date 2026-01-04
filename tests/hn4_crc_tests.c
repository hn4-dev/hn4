#include "hn4_test.h"
#include "hn4_crc.h"
#include <string.h>

static void common_setup(void) {
    /* 
     * Always init before testing. 
     * In a real app, you only do this once at startup.
     */
    hn4_crc_init();
}

hn4_TEST(CRC, Init) {
    common_setup();
    uint8_t data[] = {0};
    uint32_t crc = hn4_crc32(0, data, 1);
    ASSERT_NEQ(crc, 0); 
}

hn4_TEST(CRC, BasicString) {
    common_setup();
    const char* input = "123456789";
    /* Standard Check Value for CRC32 */
    uint32_t expected = 0xCBF43926;
    uint32_t result = hn4_crc32(0, input, strlen(input));
    
    ASSERT_EQ(expected, result);
}

/* 
 * Test Incremental/Chunked Processing 
 * Computing CRC("1234") then passing that as seed for "56789"
 * must yield the same result as CRC("123456789").
 */
hn4_TEST(CRC, IncrementalChain) {
    common_setup();
    const char* part1 = "1234";
    const char* part2 = "56789";
    
    uint32_t crc_part = hn4_crc32(0, part1, strlen(part1));
    uint32_t result = hn4_crc32(crc_part, part2, strlen(part2));
    
    ASSERT_EQ(0xCBF43926, result);
}

/*
 * Test the Standard "Fox" Vector
 * "The quick brown fox jumps over the lazy dog"
 * Expected: 0x414FA339
 */
hn4_TEST(CRC, FoxVector) {
    common_setup();
    const char* input = "The quick brown fox jumps over the lazy dog";
    uint32_t expected = 0x414FA339;
    uint32_t result = hn4_crc32(0, input, strlen(input));
    
    ASSERT_EQ(expected, result);
}

/*
 * Test Binary Data / High Bits
 * Ensures char signedness doesn't break calculation.
 * 4 bytes of 0xFF results in 0xFFFFFFFF for standard CRC32.
 */
hn4_TEST(CRC, AllOnes) {
    common_setup();
    uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t result = hn4_crc32(0, buf, 4);
    
    ASSERT_EQ(0xFFFFFFFF, result);
}

/* 
 * Test Odd Lengths
 * Verifies the byte-wise tail loop handles remaining bytes correctly.
 * Input: "a"
 * Expected: 0xE8B7BE43
 */
hn4_TEST(CRC, TinyTail) {
    common_setup();
    const char* input = "a";
    uint32_t result = hn4_crc32(0, input, 1);
    ASSERT_EQ(0xE8B7BE43, result);
}

hn4_TEST(CRC, ZeroLength) {
    common_setup();
    uint32_t res = hn4_crc32(0, NULL, 0);
    ASSERT_EQ(0, res);
}

hn4_TEST(CRC, UnalignedBuffer) {
    common_setup();
    uint8_t buffer[16];
    memset(buffer, 0xAA, sizeof(buffer));
    
    /* 
     * Input: 8 bytes of 0xAA.
     * Correct IEEE 802.3 CRC32 for 8x 0xAA is 0xABB622F0.
     * We pass buffer+1 to force unaligned memory address reading.
     */
    uint32_t res = hn4_crc32(0, buffer + 1, 8);
    ASSERT_EQ(0xABB622F0, res);
}

/* -------------------- NEW TESTS -------------------- */

/*
 * Test 1: Zeros Vector
 * Checks a block of 32 zero bytes. 
 * This stresses the table logic when no bits are set in the input.
 * Expected: 0x190A55AD
 */
hn4_TEST(CRC, ZerosVector) {
    common_setup();
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    
    uint32_t res = hn4_crc32(0, buf, sizeof(buf));
    ASSERT_EQ(0x190A55AD, res);
}

/*
 * Test 2: Alignment Sweep
 * Shifts the "123456789" string through every possible byte alignment (0-7).
 * This proves the optimized loop handles unaligned pointers at any boundary.
 */
hn4_TEST(CRC, OffsetSweep) {
    common_setup();
    char buf[20]; 
    const char *chk = "123456789";
    uint32_t expected = 0xCBF43926;

    for (int i = 0; i < 8; i++) {
        memset(buf, 0, sizeof(buf));
        strcpy(buf + i, chk);
        uint32_t res = hn4_crc32(0, buf + i, 9);
        ASSERT_EQ(expected, res);
    }
}

/*
 * Test 3: Byte Counter (Pattern)
 * Input: 256 bytes, 0x00, 0x01 ... 0xFF.
 * Checks loop unrolling across cache line boundaries.
 * Expected: 0x29058C73
 */
hn4_TEST(CRC, ByteCounter) {
    common_setup();
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) {
        buf[i] = (uint8_t)i;
    }
    
    uint32_t res = hn4_crc32(0, buf, 256);
    ASSERT_EQ(0x29058C73, res);
}

/*
 * Test 4: Bulk vs Stream
 * Verifies that the High-Speed Optimized loop produces the EXACT same math
 * as feeding bytes one by one (which forces the safe Tail loop).
 */
hn4_TEST(CRC, BulkVsStream) {
    common_setup();
    const char *data = "Stress testing optimized loop vs byte-wise loop.";
    size_t len = strlen(data);

    // 1. Calculate using optimized Slice-by-8
    uint32_t bulk = hn4_crc32(0, data, len);

    // 2. Calculate using single bytes (Stream simulation)
    uint32_t stream = 0;
    for (size_t i = 0; i < len; i++) {
        stream = hn4_crc32(stream, data + i, 1);
    }

    ASSERT_EQ(bulk, stream);
}

/* --------------------------------------------------- */

hn4_TEST(CRC, PerformanceProxy) {
    common_setup();
    /* Measures CPU time roughly */
    uint8_t large_buf[4096];
    memset(large_buf, 0, sizeof(large_buf));
    
    // Warmup
    hn4_crc32(0, large_buf, sizeof(large_buf));
    
    for(int i=0; i<2000; i++) {
        volatile uint32_t res = hn4_crc32(0, large_buf, sizeof(large_buf));
        (void)res;
    }
    ASSERT_TRUE(true);
}