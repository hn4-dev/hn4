#include "hn4_test.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h> /* For PRIu64 */

/* --- COLORS --- */
#if HN4_ENABLE_COLORS
    #define C_RST "\033[0m"
    #define C_RED "\033[91;1m"
    #define C_GRN "\033[92;1m"
    #define C_YEL "\033[93;1m"
    #define C_CYA "\033[96;1m"
    #define C_GRY "\033[90m"
#else
    #define C_RST ""
    #define C_RED ""
    #define C_GRN ""
    #define C_YEL ""
    #define C_CYA ""
    #define C_GRY ""
#endif

/* --- GLOBALS --- */
hn4_registry_node_t* _hn4_head = NULL;
static uint64_t g_total = 0;
static uint64_t g_pass = 0;
static uint64_t g_fail = 0;

/* --- HEAP SPY --- */
void* hn4_malloc(size_t size, hn4_test_ctx_t* ctx) {
    void* p = malloc(size);
    if (p && ctx) ctx->alloc_count++;
    return p;
}

void hn4_free(void* ptr, hn4_test_ctx_t* ctx) {
    if (ptr) {
        free(ptr);
        if (ctx) ctx->free_count++;
    }
}

/* --- HELPERS --- */
static bool is_match(const char* filter, const char* value) {
    if (!filter || !*filter || strcmp(filter, "*") == 0) return true;
    if (!value) return false;
    return (strstr(value, filter) != NULL);
}

static void print_time(hn4_time_t ns) {
    if (ns < 1000) {
        printf(C_GRY "[%4" PRIu64 " ns]" C_RST, (uint64_t)ns);
    } else if (ns < 1000000) {
        printf(C_GRY "[%4" PRIu64 " us]" C_RST, (uint64_t)(ns / 1000));
    } else if (ns < 1000000000) {
        printf(C_YEL "[%4" PRIu64 " ms]" C_RST, (uint64_t)(ns / 1000000));
    } else {
        /* Show seconds for long runs */
        printf(C_CYA "[%.3f s]" C_RST, (double)ns / 1000000000.0);
    }
}

/* --- XML GENERATOR --- */
static void print_xml_escaped(const char* s) {
    while (s && *s) {
        switch (*s) {
            case '"': printf("&quot;"); break;
            case '\'': printf("&apos;"); break;
            case '<': printf("&lt;"); break;
            case '>': printf("&gt;"); break;
            case '&': printf("&amp;"); break;
            default: putchar(*s);
        }
        s++;
    }
}

static void print_xml_header(void) {
    printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    printf("<testsuites>\n  <testsuite name=\"BareMetalTests\">\n");
}

static void print_xml_result(const char* suite, const char* name, 
                           double time_sec, bool fail, const char* msg) {
    printf("    <testcase name=\"%s\" classname=\"%s\" time=\"%.6f\">\n", name, suite, time_sec);
    if (fail) {
        printf("      <failure message=\"");
        print_xml_escaped(msg);
        printf("\"/>\n");
    }
    printf("    </testcase>\n");
}

static void print_xml_footer(void) {
    printf("  </testsuite>\n</testsuites>\n");
}

/* --- RUNNER --- */
void hn4_init(void) {
    hn4_hal_init();
}

int hn4_run(const char* suite_filter, const char* test_filter, int output_mode) {
    /* Reset counters per run */
    g_total = 0; g_pass = 0; g_fail = 0;
    
    /* 1. Start Global Timer */
    hn4_time_t total_start = hn4_hal_get_time_ns();

    if (output_mode == 1) print_xml_header();
    else printf(C_CYA "\n[ HN4 ] TEST RUNNER v2.4 (Portable)\n" C_RST);

    hn4_registry_node_t* curr = _hn4_head;
    
    while (curr) {
        if (is_match(suite_filter, curr->suite) && is_match(test_filter, curr->name)) {
            g_total++;
            
            hn4_test_ctx_t ctx = {0};
            ctx.suite = curr->suite;
            ctx.name = curr->name;

            if (output_mode == 0) {
                printf(C_GRY "[ RUN      ] " C_RST "%s.%s\n", curr->suite, curr->name);
                fflush(stdout);
            }

            if (curr->setup) curr->setup();
            
            hn4_time_t start = hn4_hal_get_time_ns();
            curr->func(&ctx);
            hn4_time_t end = hn4_hal_get_time_ns();
            
            if (curr->teardown) curr->teardown();

            #if HN4_ENABLE_HEAP_SPY
            if (!ctx.failed && ctx.alloc_count != ctx.free_count) {
                ctx.failed = true;
                snprintf(ctx.msg, sizeof(ctx.msg), "Memory Leak: %zu allocs vs %zu frees", 
                         ctx.alloc_count, ctx.free_count);
            }
            #endif

            hn4_time_t dur = end - start;

            if (ctx.failed) g_fail++;
            else g_pass++;

            /* Output */
            if (output_mode == 1) {
                print_xml_result(curr->suite, curr->name, (double)dur/1e9, ctx.failed, ctx.msg);
            } else {
                if (!ctx.failed) {
                    printf(C_GRN "[     PASS ] " C_RST "%s.%s ", curr->suite, curr->name);
                    print_time(dur);
                    printf("\n");
                } else {
                    printf(C_RED "[     FAIL ] " C_RST "%s.%s ", curr->suite, curr->name);
                    print_time(dur);
                    printf("\n" C_YEL "    >>> %s\n" C_RST, ctx.msg);
                }
            }
        }
        curr = curr->next;
    }

    /* 2. End Global Timer */
    hn4_time_t total_end = hn4_hal_get_time_ns();
    hn4_time_t total_dur = total_end - total_start;

    if (output_mode == 1) {
        print_xml_footer();
    } else {
        printf(C_CYA "================================================\n" C_RST);
        printf("TOTAL: %" PRIu64 " | " C_GRN "PASS: %" PRIu64 C_RST " | " C_RED "FAIL: %" PRIu64 C_RST " | TIME: ",
               g_total, g_pass, g_fail);
        print_time(total_dur);
        printf("\n");
    }

    return (int)g_fail;
}