/*
 * hn4-CAPACITOR: Portable C Test Runner v2.3
 * Fixed: Added MSVC CRT Section logic for automatic test registration.
 * Update: Added ASSERT_NE for compatibility.
 */

#ifndef _HN4_TEST_H_
#define _HN4_TEST_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "hn4_hal.h" 

/* --- CONFIGURATION --- */
#ifndef HN4_ENABLE_COLORS
#define HN4_ENABLE_COLORS    1
#endif

#ifndef HN4_ENABLE_HEAP_SPY
#define HN4_ENABLE_HEAP_SPY  1
#endif

/* --- TYPES --- */

typedef struct hn4_test_ctx {
    const char* suite;
    const char* name;
    bool failed;
    char msg[256];
    
    /* Heap Spy Stats */
    size_t alloc_count;
    size_t free_count;
} hn4_test_ctx_t;

typedef void (*hn4_test_func_t)(hn4_test_ctx_t* _ctx);
typedef void (*hn4_lifecycle_func_t)(void);

typedef struct hn4_registry_node {
    const char* suite;
    const char* name;
    hn4_test_func_t func;
    hn4_lifecycle_func_t setup;
    hn4_lifecycle_func_t teardown;
    struct hn4_registry_node* next;
} hn4_registry_node_t;

/* --- API --- */
void hn4_init(void);
int hn4_run(const char* suite_filter, const char* test_filter, int output_mode);

/* --- HEAP SPY API --- */
void* hn4_malloc(size_t size, hn4_test_ctx_t* ctx);
void hn4_free(void* ptr, hn4_test_ctx_t* ctx);

/* --- ASSERTIONS --- */
#define HN4_FAIL(fmt, ...) do { \
    _ctx->failed = true; \
    snprintf(_ctx->msg, sizeof(_ctx->msg), fmt, ##__VA_ARGS__); \
    return; \
} while(0)

#define ASSERT_TRUE(x)  if (!(x)) HN4_FAIL("TRUE failed: %s", #x)
#define ASSERT_FALSE(x) if (x)    HN4_FAIL("FALSE failed: %s", #x)

#define ASSERT_EQ(e, a) if ((uint64_t)(e) != (uint64_t)(a)) \
    HN4_FAIL("EQ failed: 0x%llx vs 0x%llx", (unsigned long long)(e), (unsigned long long)(a))

#define ASSERT_NEQ(e, a) if ((uint64_t)(e) == (uint64_t)(a)) \
    HN4_FAIL("NEQ failed: Both are 0x%llx", (unsigned long long)(e))

/* Added per request */
#define ASSERT_NE(e, a) if ((uint64_t)(e) == (uint64_t)(a)) \
    HN4_FAIL("NE failed: Both are 0x%llx", (unsigned long long)(e))

#define ASSERT_STR_EQ(e, a) if (strcmp(e, a) != 0) \
    HN4_FAIL("STR_EQ failed: '%s' vs '%s'", e, a)

#define ASSERT_OK(x) do { \
    int _res = (int)(x); \
    if (_res != 0) HN4_FAIL("OK failed: Expected 0, got %d", _res); \
} while(0)

/* --- AUTOMATIC REGISTRATION (PORTABLE) --- */
extern hn4_registry_node_t* _hn4_head;

/* 
 * GCC / Clang / MinGW: Use __attribute__((constructor))
 */
#if defined(__GNUC__) || defined(__clang__)
    #define _HN4_REGISTER(s, n, f, set, tear) \
        static hn4_registry_node_t _hn4_node_##s##_##n = { \
            .suite = #s, .name = #n, .func = f, .setup = set, .teardown = tear, .next = NULL \
        }; \
        static void __attribute__((constructor)) _hn4_reg_##s##_##n(void) { \
            _hn4_node_##s##_##n.next = _hn4_head; \
            _hn4_head = &_hn4_node_##s##_##n; \
        }

/* 
 * MSVC (Visual Studio): Use .CRT$XCU Linker Section
 * This places a pointer to the registration function in the C Runtime initialization table.
 */
#elif defined(_MSC_VER)
    /* Define the section for C initializers */
    #pragma section(".CRT$XCU", read)
    
    #define _HN4_REGISTER(s, n, f, set, tear) \
        static hn4_registry_node_t _hn4_node_##s##_##n = { \
            .suite = #s, .name = #n, .func = f, .setup = set, .teardown = tear, .next = NULL \
        }; \
        static void _hn4_reg_##s##_##n(void) { \
            _hn4_node_##s##_##n.next = _hn4_head; \
            _hn4_head = &_hn4_node_##s##_##n; \
        } \
        /* Create a function pointer in the CRT initialization section */ \
        __declspec(allocate(".CRT$XCU")) void (*_hn4_ptr_##s##_##n)(void) = _hn4_reg_##s##_##n;

#else
    #warning "HN4 Test Runner: Automatic registration not supported on this compiler."
    #define _HN4_REGISTER(s, n, f, set, tear)
#endif

/* --- TEST MACROS --- */
#define hn4_TEST(suite, name) \
    void _hn4_f_##suite##_##name(hn4_test_ctx_t* _ctx); \
    _HN4_REGISTER(suite, name, _hn4_f_##suite##_##name, NULL, NULL) \
    void _hn4_f_##suite##_##name(hn4_test_ctx_t* _ctx)

#define hn4_TEST_F(suite, name, setup_fn, teardown_fn) \
    void _hn4_f_##suite##_##name(hn4_test_ctx_t* _ctx); \
    _HN4_REGISTER(suite, name, _hn4_f_##suite##_##name, setup_fn, teardown_fn) \
    void _hn4_f_##suite##_##name(hn4_test_ctx_t* _ctx)

#ifdef __cplusplus
}
#endif

#endif /* _HN4_TEST_H_ */