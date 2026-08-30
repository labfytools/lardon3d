#ifndef LARDON3D_MATCHER_TASK_BENCHMARK_INTERNAL_H
#define LARDON3D_MATCHER_TASK_BENCHMARK_INTERNAL_H

/* This token exists only in the opt-in real-corpus runner and dedicated
 * Matcher tests. Production matcher_task.c is compiled without the macro and
 * therefore cannot observe or retain the benchmark pipeline selection. */
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
#define LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV \
    "LARDON3D_BENCHMARK_MATCHER_SYNCHRONOUS_FENCE_V1"
#define LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV \
    "LARDON3D_BENCHMARK_MATCHER_INFLIGHT_V1"
#define LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV \
    "LARDON3D_BENCHMARK_MATCHER_BATCH_V1"
#endif

#endif
