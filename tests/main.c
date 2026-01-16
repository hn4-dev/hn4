/*
 * hn4_main.c
 * Portable Test & Benchmark Entry Point
 */

#include "hn4_test.h"
#include "hn4_benchmark.h" /* New Include */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char** argv) {
    /* 1. Initialize System (HAL + Locks) */
    hn4_init();

    /* ---------------------------------------------------------
     * MODE: BENCHMARK
     * Usage: hn4 benchmark [NAME]
     * --------------------------------------------------------- */
    if (argc > 1 && strcmp(argv[1], "benchmark") == 0) {
        const char* bench_name = NULL;
        
        /* Check if a specific benchmark name was provided */
        if (argc > 2 && argv[2][0] != '-') {
            bench_name = argv[2];
        }

        printf("HN4 Storage Engine: Starting Performance Benchmarks...\n");
        printf("----------------------------------------------------------------\n");
        
        hn4_run_benchmarks(bench_name);

        hn4_hal_shutdown();
        return EXIT_SUCCESS;
    }

    /* ---------------------------------------------------------
     * MODE: UNIT TESTS
     * Usage: hn4 [SuiteFilter] [TestFilter] [--xml]
     * --------------------------------------------------------- */
    const char* suite_filter = "*";
    const char* test_filter  = "*";
    int output_mode = 0; /* 0 = Console, 1 = XML */

    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--xml") == 0) {
            output_mode = 1;
        } 
        else if (strcmp(suite_filter, "*") == 0) {
            suite_filter = argv[arg_idx];
        } 
        else if (strcmp(test_filter, "*") == 0) {
            test_filter = argv[arg_idx];
        }
        arg_idx++;
    }

    int failures = hn4_run(suite_filter, test_filter, output_mode);

    hn4_hal_shutdown(); 

    return (failures > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}