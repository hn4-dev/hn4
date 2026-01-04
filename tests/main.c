/*
 * hn4_main.c
 * Portable Test Entry Point
 */

#include "hn4_test.h"
#include <string.h>
#include <stdlib.h> /* Required for standard exit codes */

int main(int argc, char** argv) {
    const char* suite_filter = "*";
    const char* test_filter  = "*";
    int output_mode = 0; /* 0 = Console, 1 = XML */

    /* 
     * Robust Argument Parsing (Standard C99)
     * Handles order-independent arguments:
     *   ./main --xml SuiteName
     *   ./main SuiteName --xml
     *   ./main SuiteName TestName
     */
    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--xml") == 0) {
            output_mode = 1;
        } 
        /* Only assign suite if it hasn't been set yet (still default "*") */
        else if (strcmp(suite_filter, "*") == 0) {
            suite_filter = argv[arg_idx];
        } 
        /* Only assign test if suite is set and test is still default */
        else if (strcmp(test_filter, "*") == 0) {
            test_filter = argv[arg_idx];
        }
        arg_idx++;
    }

    /* 1. Initialize System (HAL + Locks) */
    hn4_init();

    /* 2. Run Tests 
     * Returns the number of failed tests.
     * output_mode determines if we print human-readable or CI-readable XML.
     */
    int failures = hn4_run(suite_filter, test_filter, output_mode);

    /* 3. Shutdown & Cleanup */
    /* Note: hn4_hal_shutdown is declared in hn4_hal.h (included via hn4_test.h) */
    hn4_hal_shutdown(); 

    /* 4. Return Exit Code
     * EXIT_SUCCESS (usually 0) and EXIT_FAILURE (usually 1) 
     * are guaranteed to be correct on Windows, Linux, and specialized OSs.
     */
    return (failures > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}