// csbench
// command-line benchmarking tool
// Ilya Vinogradov 2024
// https://github.com/Holodome/csbench
//
// csbench is dual-licensed under the terms of the MIT License and the Apache
// License 2.0. This file may not be copied, modified, or distributed except
// according to those terms.
//
// MIT License Notice
//
//    MIT License
//
//    Copyright (c) 2024 Ilya Vinogradov
//
//    Permission is hereby granted, free of charge, to any
//    person obtaining a copy of this software and associated
//    documentation files (the "Software"), to deal in the
//    Software without restriction, including without
//    limitation the rights to use, copy, modify, merge,
//    publish, distribute, sublicense, and/or sell copies of
//    the Software, and to permit persons to whom the Software
//    is furnished to do so, subject to the following
//    conditions:
//
//    The above copyright notice and this permission notice
//    shall be included in all copies or substantial portions
//    of the Software.
//
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//    TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//    PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
//    SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
//    OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
//    IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//    DEALINGS IN THE SOFTWARE.
//
// Apache License (Version 2.0) Notice
//
//    Copyright 2024 Ilya Vinogradov
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
#ifndef CSBENCH_H
#define CSBENCH_H

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// This is implementation of type-safe generic vector in C based on
// std_stretchy_buffer.
struct sb_header {
    size_t size;
    size_t capacity;
};

enum input_kind {
    INPUT_POLICY_NULL,
    INPUT_POLICY_FILE
};

// How to handle input of command?
struct input_policy {
    enum input_kind kind;
    const char *file;
};

enum output_kind {
    OUTPUT_POLICY_NULL,
    // Print output to controlling terminal
    OUTPUT_POLICY_INHERIT,
};

enum units_kind {
    // Time units
    MU_S,
    MU_MS,
    MU_US,
    MU_NS,
    // Memory units
    MU_B,
    MU_KB,
    MU_MB,
    MU_GB,

    MU_CUSTOM,
    MU_NONE
};

struct units {
    enum units_kind kind;
    const char *str;
};

enum meas_kind {
    MEAS_CUSTOM,
    MEAS_WALL,
    MEAS_RUSAGE_UTIME,
    MEAS_RUSAGE_STIME,
    MEAS_RUSAGE_MAXRSS,
    MEAS_RUSAGE_MINFLT,
    MEAS_RUSAGE_MAJFLT,
    MEAS_RUSAGE_NVCSW,
    MEAS_RUSAGE_NIVCSW,
    MEAS_PERF_CYCLES,
    MEAS_PERF_INS,
    MEAS_PERF_BRANCH,
    MEAS_PERF_BRANCHM
};

struct meas {
    const char *name;
    const char *cmd;
    struct units units;
    enum meas_kind kind;
    bool is_secondary;
    size_t primary_idx;
};

// Default measurements. Although these definitions should be used only one
// time, they are put here to make them clearer to see.
#define MEAS_WALL_DEF                                                          \
    ((struct meas){"wall clock time", NULL, {MU_S, NULL}, MEAS_WALL, false, 0})
#define MEAS_RUSAGE_UTIME_DEF                                                  \
    ((struct meas){"usrtime", NULL, {MU_S, NULL}, MEAS_RUSAGE_UTIME, true, 0})
#define MEAS_RUSAGE_STIME_DEF                                                  \
    ((struct meas){"systime", NULL, {MU_S, NULL}, MEAS_RUSAGE_STIME, true, 0})
#define MEAS_RUSAGE_MAXRSS_DEF                                                 \
    ((struct meas){"maxrss", NULL, {MU_B, NULL}, MEAS_RUSAGE_MAXRSS, true, 0})
#define MEAS_RUSAGE_MINFLT_DEF                                                 \
    ((struct meas){                                                            \
        "minflt", NULL, {MU_NONE, NULL}, MEAS_RUSAGE_MINFLT, true, 0})
#define MEAS_RUSAGE_MAJFLT_DEF                                                 \
    ((struct meas){                                                            \
        "majflt", NULL, {MU_NONE, NULL}, MEAS_RUSAGE_MAJFLT, true, 0})
#define MEAS_RUSAGE_NVCSW_DEF                                                  \
    ((struct meas){"nvcsw", NULL, {MU_NONE, NULL}, MEAS_RUSAGE_NVCSW, true, 0})
#define MEAS_RUSAGE_NIVCSW_DEF                                                 \
    ((struct meas){                                                            \
        "nivcsw", NULL, {MU_NONE, NULL}, MEAS_RUSAGE_NIVCSW, true, 0})
#define MEAS_PERF_CYCLES_DEF                                                   \
    ((struct meas){"cycles", NULL, {MU_NONE, NULL}, MEAS_PERF_CYCLES, true, 0})
#define MEAS_PERF_INS_DEF                                                      \
    ((struct meas){"ins", NULL, {MU_NONE, NULL}, MEAS_PERF_INS, true, 0})
#define MEAS_PERF_BRANCH_DEF                                                   \
    ((struct meas){"b", NULL, {MU_NONE, NULL}, MEAS_PERF_BRANCH, true, 0})
#define MEAS_PERF_BRANCHM_DEF                                                  \
    ((struct meas){"bm", NULL, {MU_NONE, NULL}, MEAS_PERF_BRANCHM, true, 0})

// Variable which can be substitued in benchmark string.
struct bench_var {
    char *name;
    char **values;
    size_t value_count;
};

struct bench_var_group {
    char *template;
    size_t *cmd_idxs; // [var->value_count]
};

// Bootstrap estimate of certain statistic. Contains lower and upper bounds, as
// well as point estimate. Point estimate is commonly obtained from statistic
// calculation over original data, while lower and upper bounds are obtained
// using bootstrapping.
struct est {
    double lower;
    double point;
    double upper;
};

struct outliers {
    double var;
    double low_severe_x;
    double low_mild_x;
    double high_mild_x;
    double high_severe_x;
    int low_severe;
    int low_mild;
    int high_mild;
    int high_severe;
};

// Describes distribution and is useful for passing benchmark data and analysis
// around.
struct distr {
    const double *data;
    size_t count;
    struct est mean;
    struct est st_dev;
    double min;
    double max;
    double median;
    double q1;
    double q3;
    double p1;
    double p5;
    double p95;
    double p99;
    struct outliers outliers;
};

struct bench {
    size_t run_count;
    int *exit_codes;
    size_t meas_count;
    double **meas;
    struct progress_bar_bench *progress;
};

struct bench_analysis {
    struct bench *bench;
    struct distr *meas; // [meas_count]
    const char *name;
};

enum big_o {
    O_1,
    O_N,
    O_N_SQ,
    O_N_CUBE,
    O_LOGN,
    O_NLOGN,
};

struct cmd_in_group_data {
    const char *value;
    double value_double;
    double mean;
    const struct bench_analysis *analysis;
};

struct ols_regress {
    enum big_o complexity;
    // function is of the form f(x) = a * F(x - c) + b where F(x) is determined
    // by complexity, a is result of OLS, and b is minimal y value,
    // and c is minimal x value.
    double a;
    double b;
    double c;
    double rms;
};

struct group_analysis {
    const struct meas *meas;
    const struct bench_var_group *group;
    struct cmd_in_group_data *data;
    const struct cmd_in_group_data *slowest;
    const struct cmd_in_group_data *fastest;
    bool values_are_doubles;
    struct ols_regress regress;
};

struct perf_cnt {
    uint64_t cycles;
    uint64_t branches;
    uint64_t missed_branches;
    uint64_t instructions;
};

// Point estimate with error. Standard deviation is used as error.
struct point_err_est {
    double point;
    double err;
};

// Analysis for a single measurement for all benchmarks. We don't do inter-measurement analysis, so this is more or less self-contained.
struct bench_meas_results {
    // Make it easy to pass this structure around as base is always needed
    struct bench_results *base;
    // Index of fastest command
    size_t fastest;
    size_t *fastest_val;                   // [val_count]
    struct group_analysis *group_analyses; // [group_count]
    // If there are only two benchmarks in total compute their p-value
    double pair_p_values;
    // If there are more that two benchmakrs but baseline is specified, compute
    // p-value of each benchmark in reference to baseline. This could reuse
    // 'pair_p_values', but because the logic is so different it is better to
    // split them.
    double *baseline_p_values; // [bench_count]
    // If there are two command groups with variables compute p-value for each
    // parameter value
    double *var_pair_p_values;      // [val_count]
    double **var_baseline_p_values; // [val_count][group_count]

    struct point_err_est *speedup;      // [bench_count]
    struct point_err_est **var_speedup; // [val_count][group_count]
    // Geometric mean of speedup of each benchmark group when baseline is
    // specified
    struct point_err_est *group_baseline_speedup; // [group_count]
};

struct bench_results {
    const struct bench_var *var;
    size_t bench_count;
    size_t meas_count;
    size_t group_count;
    size_t primary_meas_count;
    struct bench *benches;                   // [bench_count]
    struct bench_analysis *analyses;         // [bench_count]
    const struct meas *meas;                 // [meas_count]
    struct bench_meas_results *meas_results; // [meas_count]
};

#define sb_header(_a)                                                          \
    ((struct sb_header *)((char *)(_a) - sizeof(struct sb_header)))
#define sb_size(_a) (sb_header(_a)->size)
#define sb_capacity(_a) (sb_header(_a)->capacity)

#define sb_needgrow(_a, _n)                                                    \
    (((_a) == NULL) || (sb_size(_a) + (_n) >= sb_capacity(_a)))
#define sb_maybegrow(_a, _n) (sb_needgrow(_a, _n) ? sb_grow(_a, _n) : 0)
#define sb_grow(_a, _b)                                                        \
    (*(void **)(&(_a)) = sb_grow_impl((_a), (_b), sizeof(*(_a))))
#define sb_reserve(_a, _n)                                                     \
    ((_a) != NULL                                                              \
         ? (sb_capacity(_a) < (_n) ? sb_grow((_a), (_n)-sb_capacity(_a)) : 0)  \
         : sb_grow((_a), (_n)))
#define sb_resize(_a, _n) (sb_reserve(_a, _n), sb_size(_a) = (_n))

#define sb_free(_a) free((_a) != NULL ? sb_header(_a) : NULL)
#define sb_push(_a, _v) (sb_maybegrow(_a, 1), (_a)[sb_size(_a)++] = (_v))
#define sb_pushfront(_a, _v)                                                   \
    (sb_maybegrow(_a, 1),                                                      \
     memmove((_a) + 1, (_a), sizeof(*(_a)) * sb_size(_a)++), (_a)[0] = (_v))
#define sb_last(_a) ((_a)[sb_size(_a) - 1])
#define sb_len(_a) (((_a) != NULL) ? sb_size(_a) : 0)
#define sb_pop(_a) ((_a)[--sb_size(_a)])
#define sb_purge(_a) ((_a) ? (sb_size(_a) = 0) : 0)

#define ANSI_RED "31"
#define ANSI_GREEN "32"
#define ANSI_YELLOW "33"
#define ANSI_BLUE "34"
#define ANSI_MAGENTA "35"

#define ANSI_BRIGHT_GREEN "92"
#define ANSI_BRIGHT_BLUE "94"
#define ANSI_BRIGHT_CYAN "96"

#define ANSI_BOLD "1"
#define ANSI_BOLD_GREEN "32;1"
#define ANSI_BOLD_BLUE "34;1"
#define ANSI_BOLD_MAGENTA "35;1"
#define ANSI_BOLD_CYAN "36;1"

#define atomic_load(_at) __atomic_load_n(_at, __ATOMIC_SEQ_CST)
#define atomic_store(_at, _x) __atomic_store_n(_at, _x, __ATOMIC_SEQ_CST)
#define atomic_fetch_inc(_at) __atomic_fetch_add(_at, 1, __ATOMIC_SEQ_CST)
#define atomic_fence() __atomic_thread_fence(__ATOMIC_SEQ_CST)

//
// csbench.c
//

// These output functions contain some heavy logic connected to threading which
// is tightly coupled with main execution logic, so they are best kept in main
// file until we decide to split all multithreading elsewhere.
#define printf_colored(...) fprintf_colored(stdout, __VA_ARGS__)
__attribute__((format(printf, 3, 4))) void
fprintf_colored(FILE *f, const char *how, const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void error(const char *fmt, ...);
void csperror(const char *fmt);

extern __thread uint64_t g_rng_state;

//
// csbench_perf.c
//

bool init_perf(void);
void deinit_perf(void);
void perf_signal_cleanup(void);
// collect performance counters for process specified by 'pid'.
// That process is considered blocked on sigwait() when this function is called,
// to wake up process this function sends it SIGUSR1.
// This function runs and collects performance counters until process
// has finished, and consolidates results. Process can still be waited
// after this function has finished executing.
bool perf_cnt_collect(pid_t pid, struct perf_cnt *cnt);

//
// csbench_plot.c
//

void bar_plot(const struct bench_analysis *analyses, size_t count,
              size_t meas_idx, const struct bench_results *results,
              const char *output_filename, FILE *f);
void group_bar_plot(const struct group_analysis *analyses, size_t count,
                    const struct bench_var *var, const char *output_filename,
                    FILE *f);
void group_plot(const struct group_analysis *analyses, size_t count,
                const struct bench_var *var, const char *output_filename,
                FILE *f);
void kde_plot(const struct distr *distr, const struct meas *meas,
              const char *output_filename, FILE *f);
void kde_plot_ext(const struct distr *distr, const struct meas *meas,
                  const char *output_filename, FILE *f);
void kde_cmp_plot(const struct distr *a, const struct distr *b,
                  const struct meas *meas, const char *output_filename,
                  FILE *f);

//
// csbench_utils.c
//

void *sb_grow_impl(void *arr, size_t inc, size_t stride);

bool units_is_time(const struct units *units);
const char *units_str(const struct units *units);

int format_time(char *dst, size_t sz, double t);
int format_memory(char *dst, size_t sz, double t);
void format_meas(char *buf, size_t buf_size, double value,
                 const struct units *units);

const char *outliers_variance_str(double fraction);
const char *big_o_str(enum big_o complexity);

void estimate_distr(const double *data, size_t count, size_t nresamp,
                    struct distr *distr);

double mwu(const double *a, size_t n1, const double *b, size_t n2);

double ols_approx(const struct ols_regress *regress, double n);
void ols(const double *x, const double *y, size_t count,
         struct ols_regress *result);

void shuffle(size_t *arr, size_t count);

#endif
