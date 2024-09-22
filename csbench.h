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

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define CSCONCAT_(_a, _b) _a##_b
#define CSCONCAT(_a, _b) CSCONCAT_(_a, _b)
#define CSUNIQIFY(_a) CSCONCAT(_a, __LINE__)

// This is implementation of type-safe generic vector in C based on
// std_stretchy_buffer.
struct sb_header {
    size_t size;
    size_t capacity;
};

enum input_kind {
    INPUT_POLICY_NULL,
    INPUT_POLICY_FILE,
    INPUT_POLICY_STRING,
};

// How to handle input of command?
struct input_policy {
    enum input_kind kind;
    const char *file;
    const char *string;
    const char *dir;
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
    // If kind is MU_CUSTOM, contains units name
    const char *str;
};

enum meas_kind {
    MEAS_CUSTOM,
    MEAS_CUSTOM_RE,
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
    // Measurement name that will be used in reports
    const char *name;
    // If measurement is MEAS_CUSTOM, contains command string to be executed in
    // shell to do custom measurement.
    const char *cmd;
    // If measurement is MEAS_CUSTOM_RE, contains regular expresion.
    const char *re;
    struct units units;
    enum meas_kind kind;
    bool is_secondary;
    size_t primary_idx;
};

// Variable which can be substitued in command string.
struct bench_param {
    const char *name;
    const char **values;
    size_t value_count;
};

struct bench_group {
    const char *name;
    size_t bench_count;
    size_t *bench_idxs; // [bench_count]
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
    // This pointer is const, because memory is owned by respective 'struct
    // bench' instance.
    const double *data;
    size_t count;
    struct est mean;
    struct est st_dev;
    double min;
    double max;
    double median;
    // First quartile
    double q1;
    // Third quartile
    double q3;
    // First percentile
    double p1;
    // Fifth percentile
    double p5;
    // 95-th percentile
    double p95;
    // 99-th percentile
    double p99;
    struct outliers outliers;
};

// Runtime information about benchmark. When running, this structure is being
// filled accordinly with results of execution and, in particular, measurement
// values. This is later passed down for analysis.
struct bench {
    const char *name;
    size_t run_count;
    int *exit_codes;
    size_t meas_count;
    double **meas; // [meas_count]
};

struct bench_data_storage {
    bool has_param;
    struct bench_param param;
    size_t meas_count;
    struct meas *meas; // [meas_count]
};

struct bench_data {
    size_t meas_count;
    size_t bench_count;
    size_t group_count;
    const struct bench_param *param;
    const struct meas *meas;          // [meas_count]
    struct bench_run_desc *run_descs; // [bench_count]
    struct bench *benches;            // [bench_count]
    struct bench_group *groups;       // [group_count]
};

struct bench_analysis {
    const struct bench *bench;
    size_t meas_count;
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
    const struct distr *distr;
};

struct ols_regress {
    enum big_o complexity;
    // function is of the form f(x) = a * F(x - c) + b where F(x) is determined
    // by complexity, a is result of OLS, and b is minimal y value,
    // and c is minimal x value.
    double a;
    double b;
    double c;

    double r;
    double r2;
    double rms;
};

struct group_analysis {
    size_t grp_idx;
    const struct bench_group *group;
    struct cmd_in_group_data *data; // [value_count]
    // Pointers to 'data' elements
    const struct cmd_in_group_data *slowest;
    const struct cmd_in_group_data *fastest;
    // Linear regression can only be performed when values are numbers
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

struct speedup {
    struct point_err_est est;
    struct point_err_est inv_est;
    bool is_slower;
};

// Analysis for a single measurement kind for all benchmarks. We don't do
// inter-measurement analysis, so this is more or less self-contained.
struct meas_analysis {
    // Make it easy to pass this structure around as base is always needed
    struct analysis *base;
    const struct meas *meas;
    size_t meas_idx;
    // Array of bench_analysis->meas[meas_idx]
    const struct distr **benches;          // [bench_count]
    struct group_analysis *group_analyses; // [group_count]
    // Indexes of commands sorted by their time (first is the fastest)
    size_t *bench_by_mean_time; // [bench_count]
    // Indexes of fastest command for each value
    size_t **val_benches_by_mean_time; // [val_count][group_count]
    // Groups sorted by average speedups
    size_t *groups_by_avg_speed; // [group_count]
    // Groups sorted by time sum
    size_t *groups_by_total_speed; // [group_count]
    // Comparisons
    // Individual benchmarks
    struct {
        size_t ref;
        struct speedup *speedups; // [bench_count]
        double *p_values;         // [bench_count]
    } bench_cmp;
    // Per-value
    struct {
        size_t ref;
        struct speedup *speedups; // [group_count]
        double *p_values;         // [group_count]
    } *pval_cmps;                 // [val_count]
    // Groups on average
    struct {
        size_t ref;
        struct speedup *speedups; // [group_count]
        struct {
            double *p_values;         // [group_count]
            struct speedup *speedups; // [group_count]
        } *pval_cmps;                 // [val_count]
    } group_avg_cmp;
    // Groups in total
    // Estimates of times it took to execute one group
    struct {
        struct point_err_est *times; // [group_count]
        size_t ref;
        struct speedup *speedups; // [group_count]
    } group_sum_cmp;
};

// This structure hold results of benchmarking across all measurements and
// commands. Basically, it is the output of the program. Once filled via
// machinery in csbench_analyze.c, this structure can be passed down to
// different visualization paths, like plots, html report or command line
// report.
struct analysis {
    size_t bench_count;
    size_t meas_count;
    size_t group_count;
    size_t primary_meas_count;
    const struct bench_group *groups;      // [group_count]
    const struct bench_param *param;       // [bench_count]
    const struct bench *benches;           // [bench_count]
    struct bench_analysis *bench_analyses; // [bench_count]
    const struct meas *meas;               // [meas_count]
    struct meas_analysis *meas_analyses;   // [meas_count]
};

struct bench_stop_policy {
    double time_limit;
    int runs;
    int min_runs;
    int max_runs;
};

// Description of one benchmark, read-only information that is
// used to run it and choose what information to collect.
struct bench_run_desc {
    /* const char *name; */
    // Command string that is executed
    const char *str;
    // 'exec' argument to execve
    const char *exec;
    // 'argv' argument to execve
    const char **argv;
    enum output_kind output;
    // List of measurements to record
    size_t meas_count;
    const struct meas *meas; // [meas_count]
    // If not -1, use this file as stdin, otherwise /dev/null
    int stdin_fd;
    // If not -1, pipe stdout to this file
    int stdout_fd;
};

struct output_anchor {
    pthread_t id;
    char buffer[4096];
    bool has_message;
};

// Instruction to rename certain benchmark. 'n' refers to individual
// benchmark when parameter is not used, otherwise it refers to benchmark
// group.
struct rename_entry {
    size_t n;
    const char *old_name;
    const char *name;
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct settings {
    const char **args;
    struct meas *meas;
    struct input_policy input;
    enum output_kind output;
    bool has_param;
    struct bench_param param;
    struct rename_entry *rename_list;
};

enum app_mode {
    APP_BENCH,
    APP_LOAD_TEXT,
    APP_LOAD_BIN
};

// Decide how output should be sorted
enum sort_mode {
    // This is sentinel value. We expand it to one of the following values
    // during initialization
    SORT_DEFAULT,
    // --sort=command
    SORT_RAW,
    // --sort=mean-time
    SORT_SPEED,
    SORT_BASELINE_RAW,
    SORT_BASELINE_SPEED,
};

enum statistical_test {
    STAT_TEST_MWU,
    STAT_TEST_TTEST,
};

enum plot_backend {
    PLOT_BACKEND_DEFAULT,
    PLOT_BACKEND_MATPLOTLIB,
    PLOT_BACKEND_GNUPLOT
};

#define make_kde_cmp_small_params(_a, _b, _meas)                                             \
    (struct kde_cmp_params)                                                                  \
    {                                                                                        \
        _a, _b, _meas, NULL, NULL, NULL                                                      \
    }
#define make_kde_cmp_params(_a, _b, _meas, _a_name, _b_name, _title)                         \
    (struct kde_cmp_params)                                                                  \
    {                                                                                        \
        _a, _b, _meas, _a_name, _b_name, _title                                              \
    }

struct kde_cmps_params {
    const struct analysis *al;
    size_t a_idx;
    size_t b_idx;
};

struct plot_maker_ctx {
    const char *image_filename;
    FILE *f;
    size_t *gnuplot_data_idx;
};

struct plot_maker {
    enum plot_backend kind;
    const char *src_extension;

    bool (*bar)(const struct meas_analysis *al, struct plot_maker_ctx *ctx);
    bool (*group_bar)(const struct meas_analysis *al, struct plot_maker_ctx *ctx);
    bool (*group_regr)(const struct meas_analysis *al, size_t idx,
                       struct plot_maker_ctx *ctx);
    bool (*kde_small)(const struct distr *distr, const struct meas *meas,
                      struct plot_maker_ctx *ctx);
    bool (*kde)(const struct distr *distr, const struct meas *meas, const char *name,
                struct plot_maker_ctx *ctx);
    bool (*kde_cmp_small)(const struct meas_analysis *al, size_t bench_idx,
                          struct plot_maker_ctx *ctx);
    bool (*kde_cmp)(const struct meas_analysis *al, size_t bench_idx,
                    struct plot_maker_ctx *ctx);
    bool (*kde_cmp_per_val_small)(const struct meas_analysis *al, size_t grp_idx,
                                  size_t val_idx, struct plot_maker_ctx *ctx);
    bool (*kde_cmp_per_val)(const struct meas_analysis *al, size_t grp_idx, size_t val_idx,
                            struct plot_maker_ctx *ctx);
    bool (*kde_cmp_group)(const struct meas_analysis *al, size_t bench_idx,
                          struct plot_maker_ctx *ctx);
};

enum {
    MAKE_PLOT_KDE = 0x1,
    MAKE_PLOT_KDE_SMALL = 0x2,
    MAKE_PLOT_KDE_CMP = 0x4,
    MAKE_PLOT_KDE_CMP_SMALL = 0x8,
    MAKE_PLOT_BAR = 0x10,
    MAKE_PLOT_GROUP_REGR = 0x20,
    MAKE_PLOT_ALL_GROUPS_REGR = 0x40,
    MAKE_PLOT_KDE_CMP_ALL_GROUPS = 0x80,
    MAKE_PLOT_KDE_CMP_PER_VAL = 0x100,
    MAKE_PLOT_KDE_CMP_PER_VAL_SMALL = 0x200,
};

enum parse_time_str_result {
    PARSE_TIME_STR_OK,
    PARSE_TIME_STR_ERR_FORMAT,
    PARSE_TIME_STR_ERR_UNITS,
    PARSE_TIME_STR_ERR_NEG,
};

#define sb_header(_a) ((struct sb_header *)((char *)(_a) - sizeof(struct sb_header)))
#define sb_size(_a) (sb_header(_a)->size)
#define sb_capacity(_a) (sb_header(_a)->capacity)

#define sb_needgrow(_a, _n) (((_a) == NULL) ? true : (sb_size(_a) + (_n) >= sb_capacity(_a)))
#define sb_maybegrow(_a, _n) (sb_needgrow(_a, _n) ? sb_grow(_a, _n) : 0)
#define sb_grow(_a, _b) (*(void **)(&(_a)) = sb_grow_impl((_a), (_b), sizeof(*(_a))))
#define sb_reserve(_a, _n)                                                                   \
    ((_a) != NULL ? (sb_capacity(_a) < (_n) ? sb_grow((_a), (_n) - sb_capacity(_a)) : 0)     \
                  : sb_grow((_a), (_n)))
#define sb_resize(_a, _n) (sb_reserve(_a, _n), sb_size(_a) = (_n))
#define sb_ensure(_a, _n) (((_a) == NULL || sb_size(_a) < (_n)) ? sb_resize(_a, _n) : 0)

#define sb_free(_a) free((_a) != NULL ? sb_header(_a) : NULL)
#define sb_push(_a, _v) (sb_maybegrow(_a, 1), (_a)[sb_size(_a)++] = (_v))
#define sb_pushfront(_a, _v)                                                                 \
    (sb_maybegrow(_a, 1), memmove((_a) + 1, (_a), sizeof(*(_a)) * sb_size(_a)++),            \
     (_a)[0] = (_v))
#define sb_last(_a) ((_a)[sb_size(_a) - 1])
#define sb_len(_a) (((_a) != NULL) ? sb_size(_a) : 0)
#define sb_pop(_a) ((_a)[--sb_size(_a)])
#define sb_purge(_a) ((_a) ? (sb_size(_a) = 0) : 0)
#define sb_new(_a) (sb_maybegrow(_a, 1), (_a) + sb_size(_a)++)

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
#define ANSI_BOLD_UNDERLINE "1;4"

#define atomic_load(_at) __atomic_load_n(_at, __ATOMIC_SEQ_CST)
#define atomic_store(_at, _x) __atomic_store_n(_at, _x, __ATOMIC_SEQ_CST)
#define atomic_fetch_inc(_at) __atomic_fetch_add(_at, 1, __ATOMIC_SEQ_CST)
#define atomic_fence() __atomic_thread_fence(__ATOMIC_SEQ_CST)

// We use this macro to facilitate two kinds of behaviour:
// On release control should never reach it, but luckily on MacOS even then we
// get abort. On debug with UBSan we get nice printout to terminal.
// Also the compiler knows that this code path is unreachable, so we don't have
// to make useless returns to make compiler happy.
#define ASSERT_UNREACHABLE() __builtin_unreachable()

//
// csbench.c
//

extern __thread uint64_t g_rng_state;
extern bool g_colored_output;
extern bool g_ignore_failure;
extern bool g_plot;
extern bool g_html;
extern bool g_csv;
extern bool g_plot_src;
extern bool g_use_perf;
extern bool g_progress_bar;
// Use linear regression to estimate slope when doing parameterized
// benchmark.
extern bool g_regr;
extern bool g_plot_debug;
extern bool g_save_bin;
extern bool g_rename_all_used;
extern bool g_clear_out_dir;
extern bool g_shuffle_when_runnig;
// Number of resamples to use in bootstrapping when estimating
// distributions.
extern int g_nresamp;
extern int g_progress_bar_interval_us;
extern int g_threads;
// Index of benchmark that should be used as baseline or -1.
extern int g_baseline;
extern int g_desired_plots;
extern enum sort_mode g_sort_mode;
extern enum statistical_test g_stat_test;
extern enum plot_backend g_plot_backend_override;
extern enum app_mode g_mode;
extern struct bench_stop_policy g_warmup_stop;
extern struct bench_stop_policy g_bench_stop;
extern struct bench_stop_policy g_round_stop;
extern struct output_anchor *volatile g_output_anchors;
extern const char *g_json_export_filename;
extern const char *g_out_dir;
extern const char *g_shell;
extern const char *g_common_argstring;
extern const char *g_prepare;
extern const char *g_inputd;
extern const char *g_override_bin_name;
extern const char *g_baseline_name;
extern const char *g_python_executable;

void free_bench_data(struct bench_data *data);

extern const struct meas BUILTIN_MEASUREMENTS[];

//
// csbench_cli.c
//

void parse_cli_args(int argc, char **argv, struct settings *settings);
void free_settings(struct settings *settings);

//
// csbench_serialize.c
//

bool save_bench_data_binary(const struct bench_data *data, FILE *f);
bool load_bench_data_binary(const char **file_list, struct bench_data *data,
                            struct bench_data_storage *storage);
void free_bench_data_storage(struct bench_data_storage *storage);

bool load_bench_data_text(const char **file_list, struct bench_data *data,
                          struct bench_data_storage *storage);

//
// csbench_analyze.c
//

bool do_analysis_and_make_report(const struct bench_data *data);

double ols_approx(const struct ols_regress *regress, double n);

//
// csbench_run.c
//

bool run_benches(const struct bench_run_desc *params, struct bench *benches, size_t count);

//
// csbench_report.c
//

bool make_report(const struct analysis *al);

size_t ith_bench_idx(size_t i, const struct meas_analysis *al);
size_t ith_per_val_group_idx(size_t i, size_t val_idx, const struct meas_analysis *al);
size_t ith_group_by_avg_idx(size_t i, const struct meas_analysis *al);
size_t ith_group_by_total_idx(size_t i, const struct meas_analysis *al);

//
// cesbench_html.c
//

bool make_html_report(const struct analysis *al);

//
// csbench_perf.c
//

bool init_perf(void);
void deinit_perf(void);
void perf_signal_cleanup(void);
// collect performance counters for process specified by 'pid'.
// That process is considered blocked on sigwait() when this function is
// called, to wake up process this function sends it SIGUSR1. This function
// runs and collects performance counters until process has finished, and
// consolidates results. Process can still be waited after this function has
// finished executing.
bool perf_cnt_collect(pid_t pid, struct perf_cnt *cnt);

//
// csbench_plot.c
//

bool get_plot_backend(enum plot_backend *backend);
void init_plot_maker(enum plot_backend backend, struct plot_maker *maker);

//
// csbench_utils.c
//

#define printf_colored(...) fprintf_colored(stdout, __VA_ARGS__)
__attribute__((format(printf, 3, 4))) void fprintf_colored(FILE *f, const char *how,
                                                           const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void error(const char *fmt, ...);
void errorv(const char *fmt, va_list args);
void csperror(const char *msg);
void csfmtperror(const char *fmt, ...);
void csfdperror(int fd, const char *msg);

bool pipe_cloexec(int fd[2]);
bool check_and_handle_err_pipe(int read_end, int timeout);

void *sb_grow_impl(void *arr, size_t inc, size_t stride);

double get_time(void);

bool units_is_time(const struct units *units);
const char *units_str(const struct units *units);
enum parse_time_str_result parse_time_str(const char *str, enum units_kind target_units,
                                          double *value);

void parse_units_str(const char *str, struct units *units);
bool parse_meas_str(const char *str, enum meas_kind *kind);

int format_time(char *dst, size_t sz, double t);
int format_memory(char *dst, size_t sz, double t);
void format_meas(char *buf, size_t buf_size, double value, const struct units *units);

const char *outliers_variance_str(double fraction);
const char *big_o_str(enum big_o complexity);

bool process_wait_finished_correctly(pid_t pid, bool silent);
bool shell_launch(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd, pid_t *pid);
bool shell_launch_stdin_pipe(const char *cmd, FILE **in_pipe, int stdout_fd, int stderr_fd,
                             pid_t *pid);
bool shell_execute(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd, bool silent);
bool rm_rf_dir(const char *name);

bool get_term_win_size(size_t *rows, size_t *cols);

int tmpfile_fd(void);

__attribute__((format(printf, 2, 3))) FILE *open_file_fmt(const char *mode, const char *fmt,
                                                          ...);

const char **parse_comma_separated_list(const char *str);

bool spawn_threads(void *(*worker_fn)(void *), void *param, size_t thread_count);

void init_rng_state(void);

static inline uint32_t pcg32_fast(uint64_t *state)
{
    uint64_t x = *state;
    unsigned count = (unsigned)(x >> 61);
    *state = x * UINT64_C(6364136223846793005);
    x ^= x >> 22;
    return (uint32_t)(x >> (22 + count));
}

// This is global interface for allocating and deallocating strings.
// This program is not string-heavy, most of the times they arise during
// configuration parsing and benchmark initialization.
//
// Memory management is too hard. Just allocate all
// strings in global arena and then free at once. This way all strings are
// treated as read-only, so we can safely assign them without copying.
//
// XXX: Marked as const to force the behaviour we want. User should not
// modify strings directly and instead work using this interface.
void cs_free_strings(void);
const char *csstrdup(const char *str);
const char *csmkstr(const char *str, size_t len);
char *csstralloc(size_t len);
__attribute__((format(printf, 1, 2))) const char *csfmt(const char *fmt, ...);

#ifdef __linux__
#define cssort_compar(_name) int _name(const void *ap, const void *bp, void *statep)
#elif defined(__APPLE__)
#define cssort_compar(_name) int _name(void *statep, const void *ap, const void *bp)
#else
#error
#endif
typedef cssort_compar(cssort_compar_fn);
void cssort_ext(void *base, size_t nmemb, size_t size, cssort_compar_fn *compar, void *arg);

//
// Misc definitions
//

static inline const char *bench_name(const struct analysis *al, size_t bench_idx)
{
    return al->benches[bench_idx].name;
}

static inline const char *bench_group_name(const struct analysis *al, size_t grp_idx)
{
    return al->groups[grp_idx].name;
}

#define foreach_bench_idx(_idx, _al)                                                         \
    for (size_t CSUNIQIFY(i) = 0, _idx = ith_bench_idx(0, (_al));                            \
         CSUNIQIFY(i) < (_al)->base->bench_count;                                            \
         ++CSUNIQIFY(i), _idx = CSUNIQIFY(i) < (_al)->base->bench_count                      \
                                    ? ith_bench_idx(CSUNIQIFY(i), (_al))                     \
                                    : 0)

#define foreach_group_by_avg_idx(_idx, _al)                                                  \
    for (size_t CSUNIQIFY(i) = 0, _idx = ith_group_by_avg_idx(0, (_al));                     \
         CSUNIQIFY(i) < (_al)->base->group_count;                                            \
         ++CSUNIQIFY(i), _idx = CSUNIQIFY(i) < (_al)->base->group_count                      \
                                    ? ith_group_by_avg_idx(CSUNIQIFY(i), (_al))              \
                                    : 0)

#define foreach_group_by_total_idx(_idx, _al)                                                \
    for (size_t CSUNIQIFY(i) = 0, _idx = ith_group_by_total_idx(0, (_al));                   \
         CSUNIQIFY(i) < (_al)->base->group_count;                                            \
         ++CSUNIQIFY(i), _idx = CSUNIQIFY(i) < (_al)->base->group_count                      \
                                    ? ith_group_by_total_idx(CSUNIQIFY(i), (_al))            \
                                    : 0)

#define foreach_per_val_group_idx(_idx, _val, _al)                                           \
    for (size_t CSUNIQIFY(i) = 0, _idx = ith_per_val_group_idx(0, (_val), (_al));            \
         CSUNIQIFY(i) < (_al)->base->group_count;                                            \
         ++CSUNIQIFY(i), _idx = CSUNIQIFY(i) < (_al)->base->group_count                      \
                                    ? ith_per_val_group_idx(CSUNIQIFY(i), (_val), (_al))     \
                                    : 0)

#endif // CSBENCH_H
