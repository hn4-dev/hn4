/*
 * hn4_benchmark.h
 * Performance Testing Framework
 */

#ifndef _HN4_BENCHMARK_H_
#define _HN4_BENCHMARK_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs benchmarks.
 * @param filter_name: If NULL, runs all. If set, runs only benchmarks
 *                     containing this substring (case-insensitive).
 */
void hn4_run_benchmarks(const char* filter_name);

#ifdef __cplusplus
}
#endif

#endif /* _HN4_BENCHMARK_H_ */