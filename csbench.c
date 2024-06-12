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
#include "csbench.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

enum export_kind {
    DONT_EXPORT,
    EXPORT_JSON
};

struct export_policy {
    enum export_kind kind;
    const char *filename;
};

struct bench_stop_policy {
    double time_limit;
    int runs;
    int min_runs;
    int max_runs;
};

// Instruction to rename certain benchmark. 'n' refers to individual benchmark
// when variable is not used, otherwise it refers to benchmark group.
struct rename_entry {
    size_t n;
    char name[256];
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cli_settings {
    const char **args;
    const char *shell;
    struct meas *meas;
    struct input_policy input;
    enum output_kind output;
    // Currently we support only one benchmark variable. Not to complicate
    // things, this is one heap-allocated structure. If null, there are no
    // variables.
    struct bench_var *var;
    int baseline;
    struct rename_entry *rename_list;
};

struct bench_params {
    char *str;
    char *exec;
    char **argv;
    struct input_policy input;
    enum output_kind output;
    size_t meas_count;
    const struct meas *meas;
};

struct run_info {
    struct bench_params *params;
    struct bench_var_group *groups;
    const struct bench_var *var;
    const struct meas *meas;
};

// Align this structure as attempt to avoid false sharing and make inconsistent
// data reads less probable.
struct progress_bar_bench {
    int bar;
    int finished;
    int aborted;
    union {
        uint64_t u;
        double d;
    } start_time;
    union {
        uint64_t u;
        double d;
    } metric;
    pthread_t id;
} __attribute__((aligned(64)));

struct progress_bar_state {
    size_t runs;
    double eta;
    double time;
};

struct progress_bar {
    bool was_drawn;
    struct progress_bar_bench *benches;
    size_t count;
    struct bench_analysis *analyses;
    size_t max_name_len;
    struct progress_bar_state *states;
};

// Worker thread in parallel for group. It iterates 'arr' from 'low' to 'high'
// noninclusive, calling 'fn' for each memory block.
struct bench_runner_thread_data {
    pthread_t id;
    struct bench_analysis *analyses;
    const struct bench_params *params;
    const size_t *indexes;
    size_t *cursor;
    size_t max;
};

struct output_anchor {
    pthread_t id;
    char buffer[4096];
    bool has_message;
};

enum plot_kind {
    PLOT_BAR,
    PLOT_GROUP_BAR,
    PLOT_GROUP_SINGLE,
    PLOT_GROUP,
    PLOT_KDE,
    PLOT_KDE_EXT,
    PLOT_KDE_CMP,
    PLOT_KDE_CMPG
};

struct plot_walker_args {
    const struct bench_meas_analysis *analysis;
    enum plot_kind plot_kind;
    pid_t *pids;
    size_t meas_idx;
    size_t bench_idx;
    size_t grp_idx;
    size_t var_value_idx;
};

enum app_mode {
    APP_BENCH,
    APP_LOAD
};

__thread uint64_t g_rng_state;
static bool g_colored_output = false;
static bool g_allow_nonzero = false;
static double g_warmup_time = 0.1;
static int g_threads = 1;
static bool g_plot = false;
static bool g_html = false;
static bool g_csv = false;
static bool g_plot_src = false;
static int g_nresamp = 100000;
static bool g_use_perf = false;
static bool g_progress_bar = false;
static bool g_regr = false;
static bool g_python_output = false;
static bool g_has_custom_meas = false;
static bool g_loada = false;
static int g_baseline = -1;
static enum app_mode g_mode = APP_BENCH;
static struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};
static struct output_anchor *g_output_anchors = NULL;
static struct export_policy g_export = {0};
static const char *g_out_dir = ".csbench";
static const char *g_prepare = NULL;

static const struct meas BUILTIN_MEASUREMENTS[] = {
    /* MEAS_CUSTOM */ {"", NULL, {0}, 0, false, 0},
    /* MEAS_LOADED */ {"", NULL, {0}, 0, false, 0},
    {"wall clock time", NULL, {MU_S, ""}, MEAS_WALL, false, 0},
    {"usrtime", NULL, {MU_S, ""}, MEAS_RUSAGE_UTIME, true, 0},
    {"systime", NULL, {MU_S, ""}, MEAS_RUSAGE_STIME, true, 0},
    {"maxrss", NULL, {MU_B, ""}, MEAS_RUSAGE_MAXRSS, true, 0},
    {"minflt", NULL, {MU_NONE, ""}, MEAS_RUSAGE_MINFLT, true, 0},
    {"majflt", NULL, {MU_NONE, ""}, MEAS_RUSAGE_MAJFLT, true, 0},
    {"nvcsw", NULL, {MU_NONE, ""}, MEAS_RUSAGE_NVCSW, true, 0},
    {"nivcsw", NULL, {MU_NONE, ""}, MEAS_RUSAGE_NIVCSW, true, 0},
    {"cycles", NULL, {MU_NONE, ""}, MEAS_PERF_CYCLES, true, 0},
    {"ins", NULL, {MU_NONE, ""}, MEAS_PERF_INS, true, 0},
    {"b", NULL, {MU_NONE, ""}, MEAS_PERF_BRANCH, true, 0},
    {"bm", NULL, {MU_NONE, ""}, MEAS_PERF_BRANCHM, true, 0},
};

void fprintf_colored(FILE *f, const char *how, const char *fmt, ...) {
    if (g_colored_output) {
        fprintf(f, "\x1b[%sm", how);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\x1b[0m");
    } else {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
    }
}

void error(const char *fmt, ...) {
    pthread_t tid = pthread_self();
    for (size_t i = 0; i < sb_len(g_output_anchors); ++i) {
        // Implicitly discard all messages but the first. This should not be an
        // issue, as the only possible message is error, and it (at least it
        // should) is always a single one
        if (pthread_equal(tid, g_output_anchors[i].id) &&
            !atomic_load(&g_output_anchors[i].has_message)) {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_output_anchors[i].buffer,
                      sizeof(g_output_anchors[i].buffer), fmt, args);
            atomic_fence();
            atomic_store(&g_output_anchors[i].has_message, true);
            va_end(args);
            return;
        }
    }
    fprintf_colored(stderr, ANSI_RED, "error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    putc('\n', stderr);
}

void csperror(const char *fmt) {
    int err = errno;
    char errbuf[4096];
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, errbuf, sizeof(errbuf));
#else
    strerror_r(err, errbuf, sizeof(errbuf));
    err_msg = errbuf;
#endif
    error("%s: %s", fmt, err_msg);
}

static void print_help_and_exit(int rc) {
    printf("A command line benchmarking tool\n"
           "\n"
           "Usage: csbench [subcommand] [OPTIONS] <command>...\n"
           "\n"
           "Arguments:\n"
           "          If first argument is one of the following: 'load', then "
           "consider it a subcommand name. 'load' means not to run benchmarks "
           "and instead load raw benchmark results from csv files. Each file "
           "correspons to a single benchmark, and has the following structure: "
           "the first line optionally contains measurement names, all other "
           "lines contain double values corresponding to measurements.\n"
           "  <command>...\n"
           "          The command to benchmark. Can be a shell command line, "
           "like 'ls $(pwd) && echo 1', or a direct executable invocation, "
           "like 'sleep 0.5'. Former is not available when --shell none is "
           "specified. Can contain variables in the form 'sleep {n}', see "
           "--scan family of options. If multiple commands are given, their "
           "comparison will be performed.\n"
           "\n");
    printf(
        "Options:\n"
        "  -W, --warmup <t>\n"
        "          Perform warmup runs for at least <t> seconds before actual "
        "benchmark of each command.\n"
        "  -R, --runs <n>\n"
        "          Perform exactly <n> benchmark runs of each command. This "
        "option overrides --time-limit, --min-runs and --max-runs.\n"
        "  -T, --time-limit <t>\n"
        "          Run each benchmark for at least <t> seconds.\n"
        "  --min-runs <n>\n"
        "          Run each benchmark at least <n> times, used in conjunction "
        "with \n"
        "          --time-limit and --max-runs.\n"
        "  --max-runs <n>\n"
        "          Run each benchmark at most <n> times, used in conjunction "
        "with --time-limit and --min-runs.\n"
        "  -P, --prepare <cmd>\n"
        "          Execute <cmd> in default shell before each benchmark run.\n"
        "  --nrs <n>\n"
        "          Specify number of resamples used in bootstrapping. Default "
        "value is 100000\n");
    printf(
        "  -S, --shell <cmd>\n"
        "          Specify shell used for executing commands. Can be both "
        "shell name, like 'bash', or command line like 'bash --norc'. Either "
        "way, '-c' and benchmarked command are appended to argument list. "
        "<cmd> can also be none specifying that commands should be executed "
        "without a shell directly with exec.\n"
        "  --output <where>\n"
        "          Specify what to do with benchmarked commands' stdout and "
        "stdder. Can be set to 'inherit' - output will be printed to terminal, "
        "or 'none' - output will be piped to /dev/null. The latter is the "
        "default option.\n"
        "  --input <where>\n"
        "          Specify how each command should receive its input. <where> "
        "can be a file name, or none. In the latter case /dev/null is piped to "
        "stdin.\n"
        "  --custom <name>\n"
        "          Add custom measurement with <name>. Attempts to parse real "
        "value from each command's stdout and interprets it in seconds.\n"
        "  --custom-t <name> <cmd>\n"
        "          Add custom measurement with <name>. Pipes each commands "
        "stdout to <cmd> and tries to parse real value from its output and "
        "interprets it in seconds. This can be used to extract a number, for "
        "example, using grep. Alias for --custom-x <name> 's' <cmd>.\n");
    printf("  --custom-x <name> <units> <cmd>\n"
           "          Add custom measurement with <name>. Pipes each commands "
           "stdout to <cmd> and tries to parse real value from its output and "
           "interprets it in <units>. <units> can be one of the time units "
           "'s', 'ms','us', 'ns', or memory units 'b', 'kb', 'mb', 'gb', in "
           "which case results will pretty printed. If <units> is 'none', no "
           "units are printed. Alternatively <units> can be any string.\n"
           "  --scan <i>/<n>/<m>[/<s>]\n"
           "          Add variable with name <i> running in range from <n> to "
           "<m> with step <s>. <s> is optional, default is 1. Can be used from "
           "command in the form '{<i>}'.\n"
           "  --scan <i>/v[,...]\n"
           "          Add variable with name <i> running values from comma "
           "separated list <v>.\n"
           "  -j, --jobs <n>\n"
           "          Execute benchmarks in parallel with <n> threads. Default "
           "option is to execute all benchmarks sequentially\n"
           "  --export-json <f>\n"
           "          Export benchmark results without analysis as json.\n"
           "  -o, --out-dir <d>\n"
           "          Specify directory where plots, html report and other "
           "analysis results will be placed. Default is '.csbench' in current "
           "directory.\n");
    printf(
        "  --plot\n"
        "          Generate plots. For each benchmark KDE is generated in two "
        "variants. For each variable (--scan and --scanl) variable values "
        "are "
        "plotted against mean time. Single violin plot is produced if multiple "
        "commands are specified. For each measurement (--custom and others) "
        "its own group of plots is generated. Also readme.md file is "
        "generated, which helps to decipher plot file names.\n"
        "  --plot-src\n"
        "          Next to each plot file place python script used to produce "
        "it. Can be used to quickly patch up plots for presentation.\n"
        "  --html\n"
        "          Generate html report. Implies --plot.\n"
        "  --no-wall\n"
        "          Exclude wall clock information from command line output, "
        "plots, html report. Commonly used with custom measurements (--custom "
        "and others) when wall clock information is excessive.\n"
        "  --allow-nonzero\n"
        "          Accept commands with non-zero exit code. Default behavior "
        "is to abort benchmarking.\n"
        "  --meas <opt>[,...]\n"
        "          List of 'struct rusage' fields or performance counters "
        "(PMC) to "
        "include to analysis. Default (if --no-wall is not specified) are cpu "
        "time (ru_stime and ru_utime). Possible rusage values are 'stime', "
        "'utime', 'maxrss', 'minflt', 'majflt', 'nvcsw', 'nivcsw'. See your "
        "system's 'man 2 getrusage' for additional information. Possible PMC "
        "values are 'cycles', 'instructions', 'branches', 'branch-misses'.\n"
        "  --color <when>\n"
        "          Can be one of the 'never', 'always', 'auto', similar to GNU "
        "ls.\n"
        "  --progress-bar <when>\n"
        "          Can be one of the 'never', 'always', 'auto', works similar "
        "to --color option.\n"
        "  --regr\n"
        "          Do linear regression based on benchmark variables.\n"
        "  --baseline <n>\n"
        "          Specify benchmark <n>, from 1 to <benchmark count> to serve "
        "as baseline in comparison. If this option is not set, baseline will "
        "be chosen automatically.\n");
    printf(
        "  --python-output\n"
        "          Do not silence python output. Intended for developers, as "
        "users should not have to see python output as it should always work "
        "correctly.\n"
        "  --load\n"
        "          Interpret <command> list given to csbench as list of csv "
        "files, containing results of benchmarks. In this case csbench does "
        "not run benchmarks, but does provide the usual analysis using data "
        "specified in files.\n"
        "  --loada\n"
        "          Try to load benchmark results from current output "
        "directory.\n"
        "  --rename <n> <name>\n"
        "          Rename benchmark with number <n> to <name>. This name will "
        "be used in reports instead of default one, which is command name. \n"
        "   -s, --simple\n"
        "          Preset to run benchmarks in parallel for one second without "
        "warmup. Useful "
        "for quickly checking something.\n"
        "  --help\n"
        "          Print help.\n"
        "  --version\n"
        "          Print version.\n");
    exit(rc);
}

static void print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static bool parse_range_scan_settings(const char *settings, char **namep,
                                      double *lowp, double *highp,
                                      double *stepp) {
    char *name = NULL;
    const char *cursor = settings;
    const char *settings_end = settings + strlen(settings);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return false;

    size_t name_len = i_end - cursor;
    name = malloc(name_len + 1);
    memcpy(name, cursor, name_len);
    name[name_len] = '\0';

    cursor = i_end + 1;
    char *n_end = strchr(cursor, '/');
    if (n_end == NULL)
        goto err_free_name;

    char *low_str_end = NULL;
    double low = strtod(cursor, &low_str_end);
    if (low_str_end != n_end)
        goto err_free_name;

    cursor = n_end + 1;
    char *m_end = strchr(cursor, '/');
    char *high_str_end = NULL;
    double high = strtod(cursor, &high_str_end);
    if (m_end == NULL ? 0 : high_str_end != m_end)
        goto err_free_name;

    double step = 1.0;
    if (high_str_end != settings_end) {
        cursor = high_str_end + 1;
        char *step_str_end = NULL;
        step = strtod(cursor, &step_str_end);
        if (step_str_end != settings_end)
            goto err_free_name;
    }

    *namep = name;
    *lowp = low;
    *highp = high;
    *stepp = step;
    return true;
err_free_name:
    free(name);
    return false;
}

static char **range_to_var_value_list(double low, double high, double step) {
    assert(high > low);
    char **result = NULL;
    for (double cursor = low; cursor <= high + 0.000001; cursor += step) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%g", cursor);
        sb_push(result, strdup(buf));
    }
    return result;
}

static bool parse_comma_separated_settings(const char *str, char **namep,
                                           char **scan_listp) {
    char *name = NULL;
    char *scan_list = NULL;

    const char *cursor = str;
    const char *str_end = str + strlen(str);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return false;

    size_t name_len = i_end - cursor;
    name = malloc(name_len + 1);
    memcpy(name, cursor, name_len);
    name[name_len] = '\0';

    cursor = i_end + 1;
    if (cursor == str_end)
        goto err_free_name;

    scan_list = strdup(cursor);

    *namep = name;
    *scan_listp = scan_list;
    return true;
err_free_name:
    free(name);
    return false;
}

static char **parse_comma_separated_list(const char *str) {
    char **value_list = NULL;
    const char *cursor = str;
    const char *end = str + strlen(str);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            char *new_str = strdup(cursor);
            size_t len = strlen(new_str);
            while (len && new_str[len - 1] == '\n')
                new_str[len-- - 1] = '\0';
            sb_push(value_list, new_str);
            break;
        }
        size_t value_len = next - cursor;
        char *value = malloc(value_len + 1);
        memcpy(value, cursor, value_len);
        value[value_len] = '\0';
        sb_push(value_list, value);
        cursor = next + 1;
    }
    return value_list;
}

static void parse_units_str(const char *str, struct units *units) {
    if (strcmp(str, "s") == 0) {
        units->kind = MU_S;
    } else if (strcmp(str, "ms") == 0) {
        units->kind = MU_MS;
    } else if (strcmp(str, "us") == 0) {
        units->kind = MU_US;
    } else if (strcmp(str, "ns") == 0) {
        units->kind = MU_NS;
    } else if (strcmp(str, "b") == 0) {
        units->kind = MU_B;
    } else if (strcmp(str, "kb") == 0) {
        units->kind = MU_KB;
    } else if (strcmp(str, "mb") == 0) {
        units->kind = MU_MB;
    } else if (strcmp(str, "gb") == 0) {
        units->kind = MU_GB;
    } else if (strcmp(str, "none") == 0) {
        units->kind = MU_NONE;
    } else {
        units->kind = MU_CUSTOM;
        strlcpy(units->str, str, sizeof(units->str));
    }
}

static void parse_meas_list(const char *opts, enum meas_kind **meas_list) {
    char **list = parse_comma_separated_list(opts);
    for (size_t i = 0; i < sb_len(list); ++i) {
        const char *opt = list[i];
        enum meas_kind kind;
        if (strcmp(opt, "stime") == 0) {
            kind = MEAS_RUSAGE_STIME;
        } else if (strcmp(opt, "utime") == 0) {
            kind = MEAS_RUSAGE_UTIME;
        } else if (strcmp(opt, "maxrss") == 0) {
            kind = MEAS_RUSAGE_MAXRSS;
        } else if (strcmp(opt, "minflt") == 0) {
            kind = MEAS_RUSAGE_MINFLT;
        } else if (strcmp(opt, "majflt") == 0) {
            kind = MEAS_RUSAGE_MAJFLT;
        } else if (strcmp(opt, "nvcsw") == 0) {
            kind = MEAS_RUSAGE_NVCSW;
        } else if (strcmp(opt, "nivcsw") == 0) {
            kind = MEAS_RUSAGE_NIVCSW;
        } else if (strcmp(opt, "cycles") == 0) {
            kind = MEAS_PERF_CYCLES;
        } else if (strcmp(opt, "instructions") == 0) {
            kind = MEAS_PERF_INS;
        } else if (strcmp(opt, "branches") == 0) {
            kind = MEAS_PERF_BRANCH;
        } else if (strcmp(opt, "branch-misses") == 0) {
            kind = MEAS_PERF_BRANCHM;
        } else {
            error("invalid measurement name: '%s'", opt);
            exit(EXIT_FAILURE);
        }
        sb_push(*meas_list, kind);
    }
    for (size_t i = 0; i < sb_len(list); ++i)
        free(list[i]);
    sb_free(list);
}

static bool opt_arg(char **argv, int *cursor, const char *opt, char **arg) {
    if (strcmp(argv[*cursor], opt) == 0) {
        ++(*cursor);
        if (argv[*cursor] == NULL) {
            error("%s requires 1 argument", opt);
            exit(EXIT_FAILURE);
        }

        *arg = argv[*cursor];
        ++(*cursor);
        return true;
    }

    size_t opt_len = strlen(opt);
    if (strncmp(opt, argv[*cursor], opt_len) == 0) {
        if (opt_len == 2) {
            assert(opt[0] == '-');
            assert(isalpha(opt[1]));
            if (argv[*cursor][2] == '=') {
                error("%s syntax is not supported", argv[*cursor]);
                exit(EXIT_FAILURE);
            }
            bool alldigits = true;
            for (size_t i = 2; i < strlen(argv[*cursor]) && alldigits; ++i)
                if (!isdigit(argv[*cursor][i]))
                    alldigits = false;
            if (alldigits) {
                *arg = argv[(*cursor)++] + 2;
                return true;
            } else {
                error("%s syntax is not supported", argv[*cursor]);
                exit(EXIT_FAILURE);
            }
        } else if (argv[*cursor][opt_len] == '=') {
            *arg = argv[(*cursor)++] + opt_len + 1;
            return true;
        }
    }
    return false;
}

static size_t simple_get_thread_count(void) {
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1)
        return 1;

    if (!execute_in_shell("nproc", -1, pipe_fd[1], -1)) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return 1;
    }

    char buffer[4096];
    ssize_t nread = read(pipe_fd[0], buffer, sizeof(buffer));
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    if (nread < 0 || nread == 0 || nread == sizeof(buffer))
        return 1;

    buffer[nread] = '\0';

    char *str_end;
    long value = strtol(buffer, &str_end, 10);
    if (str_end == buffer)
        return 1;

    return value;
}

static void parse_cli_args(int argc, char **argv,
                           struct cli_settings *settings) {
    settings->shell = "/bin/sh";
    settings->baseline = -1;
    bool no_wall = false;
    struct meas *meas_list = NULL;
    enum meas_kind *rusage_opts = NULL;

    if (argc == 1)
        print_help_and_exit(EXIT_SUCCESS);

    char *str;
    int cursor = 1;
    if (strcmp(argv[cursor], "load") == 0) {
        g_mode = APP_LOAD;
        ++cursor;
    }

    while (cursor < argc) {
        if (strcmp(argv[cursor], "--help") == 0 ||
            strcmp(argv[cursor], "-h") == 0) {
            print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(argv[cursor], "--version") == 0) {
            print_version_and_exit();
        } else if (opt_arg(argv, &cursor, "--warmup", &str) ||
                   opt_arg(argv, &cursor, "-W", &str)) {
            char *str_end;
            double value = strtod(str, &str_end);
            if (str_end == str) {
                error("invalid --warmup argument");
                exit(EXIT_FAILURE);
            }
            if (value < 0.0) {
                error("time limit must be positive number or zero");
                exit(EXIT_FAILURE);
            }
            g_warmup_time = value;
        } else if (opt_arg(argv, &cursor, "--time-limit", &str) ||
                   opt_arg(argv, &cursor, "-T", &str)) {
            char *str_end;
            double value = strtod(str, &str_end);
            if (str_end == str) {
                error("invalid --time-limit argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0.0) {
                error("time limit must be positive number");
                exit(EXIT_FAILURE);
            }
            g_bench_stop.time_limit = value;
        } else if (opt_arg(argv, &cursor, "--runs", &str) ||
                   opt_arg(argv, &cursor, "-R", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --runs argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                error("run count must be positive number");
                exit(EXIT_FAILURE);
            }
            g_bench_stop.runs = value;
        } else if (opt_arg(argv, &cursor, "--min-runs", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --min-runs argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                error("resamples count must be positive number");
                exit(EXIT_FAILURE);
            }
            g_bench_stop.min_runs = value;
        } else if (opt_arg(argv, &cursor, "--max-runs", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --max-runs argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                error("resamples count must be positive number");
                exit(EXIT_FAILURE);
            }
            g_bench_stop.max_runs = value;
        } else if (opt_arg(argv, &cursor, "--prepare", &str)) {
            g_prepare = str;
        } else if (opt_arg(argv, &cursor, "--nrs", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --nrs argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                error("resamples count must be positive number");
                exit(EXIT_FAILURE);
            }
            g_nresamp = value;
        } else if (opt_arg(argv, &cursor, "--shell", &str) ||
                   opt_arg(argv, &cursor, "-S", &str)) {
            if (strcmp(str, "none") == 0)
                settings->shell = NULL;
            else
                settings->shell = str;
        } else if (opt_arg(argv, &cursor, "--output", &str)) {
            if (strcmp(str, "null") == 0) {
                settings->output = OUTPUT_POLICY_NULL;
            } else if (strcmp(str, "inherit") == 0) {
                settings->output = OUTPUT_POLICY_INHERIT;
            } else {
                error("invalid --output option");
                exit(EXIT_FAILURE);
            }
        } else if (opt_arg(argv, &cursor, "--input", &str)) {
            if (strcmp(str, "null") == 0) {
                settings->input.kind = INPUT_POLICY_NULL;
            } else {
                settings->input.kind = INPUT_POLICY_FILE;
                settings->input.file = str;
            }
        } else if (opt_arg(argv, &cursor, "--custom", &str)) {
            struct meas meas;
            memset(&meas, 0, sizeof(meas));
            strlcpy(meas.name, str, sizeof(meas.name));
            meas.cmd = "cat";
            sb_push(meas_list, meas);
        } else if (strcmp(argv[cursor], "--custom-t") == 0) {
            ++cursor;
            if (cursor + 1 >= argc) {
                error("--custom-t requires 2 arguments");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct meas meas;
            memset(&meas, 0, sizeof(meas));
            strlcpy(meas.name, name, sizeof(meas.name));
            meas.cmd = cmd;
            sb_push(meas_list, meas);
        } else if (strcmp(argv[cursor], "--custom-x") == 0) {
            ++cursor;
            if (cursor + 2 >= argc) {
                error("--custom-x requires 3 arguments");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *units = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct meas meas;
            memset(&meas, 0, sizeof(meas));
            strlcpy(meas.name, name, sizeof(meas.name));
            meas.cmd = cmd;
            parse_units_str(units, &meas.units);
            sb_push(meas_list, meas);
        } else if (strcmp(argv[cursor], "--rename") == 0) {
            ++cursor;
            if (cursor + 1 >= argc) {
                error("--rename requires 2 arguments");
                exit(EXIT_FAILURE);
            }
            const char *n = argv[cursor++];
            const char *name = argv[cursor++];
            char *str_end;
            long value = strtol(n, &str_end, 10);
            if (str_end == str) {
                error("invalid --rename command number argument");
                exit(EXIT_FAILURE);
            }
            if (value < 1) {
                error("command number must be at least 1");
                exit(EXIT_FAILURE);
            }
            struct rename_entry *entry = sb_new(settings->rename_list);
            entry->n = value - 1;
            strlcpy(entry->name, name, sizeof(entry->name));
        } else if (opt_arg(argv, &cursor, "--rename-all", &str)) {
            char **list = parse_comma_separated_list(str);
            for (size_t i = 0; i < sb_len(list); ++i) {
                struct rename_entry *entry = sb_new(settings->rename_list);
                entry->n = i;
                strlcpy(entry->name, list[i], sizeof(entry->name));
                free(list[i]);
            }
            sb_free(list);
        } else if (opt_arg(argv, &cursor, "--scan", &str)) {
            double low, high, step;
            char *name;
            if (!parse_range_scan_settings(str, &name, &low, &high, &step)) {
                error("invalid --scan argument");
                exit(EXIT_FAILURE);
            }
            if (settings->var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            char **value_list = range_to_var_value_list(low, high, step);
            struct bench_var *var = calloc(1, sizeof(*var));
            strlcpy(var->name, name, sizeof(var->name));
            var->values = value_list;
            var->value_count = sb_len(value_list);
            settings->var = var;
        } else if (opt_arg(argv, &cursor, "--scanl", &str)) {
            char *name, *scan_list;
            if (!parse_comma_separated_settings(str, &name, &scan_list)) {
                error("invalid --scanl argument");
                exit(EXIT_FAILURE);
            }
            if (settings->var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            char **value_list = parse_comma_separated_list(scan_list);
            free(scan_list);
            struct bench_var *var = calloc(1, sizeof(*var));
            strlcpy(var->name, name, sizeof(var->name));
            var->values = value_list;
            var->value_count = sb_len(value_list);
            settings->var = var;
        } else if (opt_arg(argv, &cursor, "--jobs", &str) ||
                   opt_arg(argv, &cursor, "-j", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --jobs argument");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                error("jobs count must be positive number");
                exit(EXIT_FAILURE);
            }
            g_threads = value;
        } else if (opt_arg(argv, &cursor, "--export-json", &str)) {
            g_export.kind = EXPORT_JSON;
            g_export.filename = str;
        } else if (opt_arg(argv, &cursor, "--out-dir", &str) ||
                   opt_arg(argv, &cursor, "-o", &str)) {
            g_out_dir = str;
        } else if (strcmp(argv[cursor], "--html") == 0) {
            ++cursor;
            g_plot = g_html = true;
        } else if (strcmp(argv[cursor], "--plot") == 0) {
            ++cursor;
            g_plot = true;
        } else if (strcmp(argv[cursor], "--plot-src") == 0) {
            ++cursor;
            g_plot_src = true;
        } else if (strcmp(argv[cursor], "--no-wall") == 0) {
            ++cursor;
            no_wall = true;
        } else if (strcmp(argv[cursor], "--allow-nonzero") == 0) {
            ++cursor;
            g_allow_nonzero = true;
        } else if (strcmp(argv[cursor], "--csv") == 0) {
            ++cursor;
            g_csv = true;
        } else if (strcmp(argv[cursor], "--regr") == 0) {
            ++cursor;
            g_regr = true;
        } else if (strcmp(argv[cursor], "--python-output") == 0) {
            ++cursor;
            g_python_output = true;
        } else if (strcmp(argv[cursor], "--load") == 0) {
            ++cursor;
            g_mode = APP_LOAD;
        } else if (strcmp(argv[cursor], "--loada") == 0) {
            ++cursor;
            g_mode = APP_LOAD;
            g_loada = true;
        } else if (strcmp(argv[cursor], "--simple") == 0 ||
                   strcmp(argv[cursor], "-s") == 0) {
            ++cursor;
            g_threads = simple_get_thread_count();
            g_warmup_time = 0.0;
            g_bench_stop.time_limit = 1.0;
        } else if (opt_arg(argv, &cursor, "--meas", &str)) {
            parse_meas_list(str, &rusage_opts);
        } else if (opt_arg(argv, &cursor, "--baseline", &str)) {
            char *str_end;
            long value = strtol(str, &str_end, 10);
            if (str_end == str) {
                error("invalid --baseline argument");
                exit(EXIT_FAILURE);
            }
            // Filter out negative values as we treat this as unsigned number
            // later.
            if (value <= 0) {
                error("baseline number must be positive number");
                exit(EXIT_FAILURE);
            }
            settings->baseline = value;
        } else if (opt_arg(argv, &cursor, "--color", &str)) {
            if (strcmp(str, "auto") == 0) {
                if (isatty(STDIN_FILENO))
                    g_colored_output = true;
                else
                    g_colored_output = false;
            } else if (strcmp(str, "never") == 0) {
                g_colored_output = false;
            } else if (strcmp(str, "always") == 0) {
                g_colored_output = true;
            } else {
                error("invalid --color option");
                exit(EXIT_FAILURE);
            }
        } else if (opt_arg(argv, &cursor, "--progress-bar", &str)) {
            if (strcmp(str, "auto") == 0) {
                if (isatty(STDIN_FILENO))
                    g_progress_bar = true;
                else
                    g_progress_bar = false;
            } else if (strcmp(str, "never") == 0) {
                g_progress_bar = false;
            } else if (strcmp(str, "always") == 0) {
                g_progress_bar = true;
            } else {
                error("invalid --progress_bar option");
                exit(EXIT_FAILURE);
            }
        } else {
            if (*argv[cursor] == '-') {
                error("unknown option %s", argv[cursor]);
                exit(EXIT_FAILURE);
            }
            sb_push(settings->args, argv[cursor++]);
        }
    }

    if (!no_wall) {
        sb_push(settings->meas, BUILTIN_MEASUREMENTS[MEAS_WALL]);
        bool already_has_stime = false, already_has_utime = false;
        for (size_t i = 0; i < sb_len(rusage_opts); ++i) {
            if (rusage_opts[i] == MEAS_RUSAGE_STIME)
                already_has_stime = true;
            else if (rusage_opts[i] == MEAS_RUSAGE_UTIME)
                already_has_utime = true;
        }
        if (!already_has_utime)
            sb_pushfront(rusage_opts, MEAS_RUSAGE_UTIME);
        if (!already_has_stime)
            sb_pushfront(rusage_opts, MEAS_RUSAGE_STIME);
    }
    for (size_t i = 0; i < sb_len(rusage_opts); ++i) {
        enum meas_kind kind = rusage_opts[i];
        sb_push(settings->meas, BUILTIN_MEASUREMENTS[kind]);
    }
    sb_free(rusage_opts);
    for (size_t i = 0; i < sb_len(meas_list); ++i)
        sb_push(settings->meas, meas_list[i]);
    sb_free(meas_list);
}

static void free_cli_settings(struct cli_settings *settings) {
    if (settings->var) {
        struct bench_var *var = settings->var;
        for (size_t j = 0; j < sb_len(var->values); ++j)
            free(var->values[j]);
        sb_free(var->values);
        free(settings->var);
    }
    sb_free(settings->args);
    sb_free(settings->meas);
    sb_free(settings->rename_list);
}

static bool replace_var_str(char *buf, size_t buf_size, const char *src,
                            const char *name, const char *value,
                            bool *replaced) {
    char *buf_end = buf + buf_size;
    size_t var_name_len = strlen(name);
    char *wr_cursor = buf;
    const char *rd_cursor = src;
    while (*rd_cursor) {
        if (*rd_cursor == '{' &&
            strncmp(rd_cursor + 1, name, var_name_len) == 0 &&
            rd_cursor[var_name_len + 1] == '}') {
            rd_cursor += 2 + var_name_len;
            size_t len = strlen(value);
            if (wr_cursor + len >= buf_end)
                return false;
            memcpy(wr_cursor, value, len);
            wr_cursor += len;
            *replaced = true;
        } else {
            if (wr_cursor >= buf_end)
                return false;
            *wr_cursor++ = *rd_cursor++;
        }
    }
    if (wr_cursor >= buf_end)
        return false;
    *wr_cursor = '\0';
    return true;
}

static char **split_shell_words(const char *cmd) {
    char **words = NULL;
    char *current_word = NULL;
    enum {
        STATE_DELIMITER,
        STATE_BACKSLASH,
        STATE_UNQUOTED,
        STATE_UNQUOTED_BACKSLASH,
        STATE_SINGLE_QUOTED,
        STATE_DOUBLE_QUOTED,
        STATE_DOUBLE_QUOTED_BACKSLASH,
        STATE_COMMENT
    } state = STATE_DELIMITER;
    for (;;) {
        int c = *cmd++;
        switch (state) {
        case STATE_DELIMITER:
            switch (c) {
            case '\0':
                if (current_word != NULL) {
                    sb_push(current_word, '\0');
                    sb_push(words, current_word);
                }
                goto out;
            case '\'':
                state = STATE_SINGLE_QUOTED;
                break;
            case '"':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '\\':
                state = STATE_BACKSLASH;
                break;
            case '\t':
            case ' ':
            case '\n':
                state = STATE_DELIMITER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                sb_push(current_word, c);
                state = STATE_UNQUOTED;
                break;
            }
            break;
        case STATE_BACKSLASH:
            switch (c) {
            case '\0':
                sb_push(current_word, '\\');
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_DELIMITER;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED:
            switch (c) {
            case '\0':
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\'':
                state = STATE_SINGLE_QUOTED;
                break;
            case '"':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '\\':
                state = STATE_UNQUOTED_BACKSLASH;
                break;
            case '\t':
            case ' ':
            case '\n':
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                current_word = NULL;
                state = STATE_DELIMITER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED_BACKSLASH:
            switch (c) {
            case '\0':
                sb_push(current_word, '\\');
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_UNQUOTED;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_SINGLE_QUOTED:
            switch (c) {
            case '\0':
                goto error;
            case '\'':
                state = STATE_UNQUOTED;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_DOUBLE_QUOTED:
            switch (c) {
            case '\0':
                goto error;
            case '"':
                state = STATE_UNQUOTED;
                break;
            case '\\':
                state = STATE_DOUBLE_QUOTED_BACKSLASH;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_DOUBLE_QUOTED_BACKSLASH:
            switch (c) {
            case '\0':
                goto error;
            case '\n':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '$':
            case '`':
            case '"':
            case '\\':
                sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            default:
                sb_push(current_word, '\\');
                sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            }
            break;
        case STATE_COMMENT:
            switch (c) {
            case '\0':
                if (current_word) {
                    sb_push(current_word, '\0');
                    sb_push(words, current_word);
                }
                goto out;
            case '\n':
                state = STATE_DELIMITER;
                break;
            default:
                break;
            }
            break;
        }
    }
error:
    if (current_word)
        sb_free(current_word);
    for (size_t i = 0; i < sb_len(words); ++i)
        sb_free(words[i]);
    sb_free(words);
    words = NULL;
out:
    return words;
}

static bool extract_exec_and_argv(const char *cmd_str, char **exec,
                                  char ***argv) {
    char **words = split_shell_words(cmd_str);
    if (words == NULL) {
        error("invalid command syntax");
        return false;
    }
    *exec = strdup(words[0]);
    for (size_t i = 0; i < sb_len(words); ++i)
        sb_push(*argv, strdup(words[i]));

    for (size_t i = 0; i < sb_len(words); ++i)
        sb_free(words[i]);
    sb_free(words);
    return true;
}

static bool init_cmd_exec(const char *shell, const char *cmd_str, char **exec,
                          char ***argv) {
    if (shell != NULL) {
        if (!extract_exec_and_argv(shell, exec, argv))
            return false;
        sb_push(*argv, strdup("-c"));
        sb_push(*argv, strdup(cmd_str));
        sb_push(*argv, NULL);
    } else {
        if (!extract_exec_and_argv(cmd_str, exec, argv))
            return false;
        sb_push(*argv, NULL);
    }
    return true;
}

static void free_run_info(struct run_info *run_info) {
    for (size_t i = 0; i < sb_len(run_info->params); ++i) {
        struct bench_params *params = run_info->params + i;
        free(params->exec);
        for (char **word = params->argv; *word; ++word)
            free(*word);
        sb_free(params->argv);
        free(params->str);
    }
    sb_free(run_info->params);
    for (size_t i = 0; i < sb_len(run_info->groups); ++i) {
        struct bench_var_group *group = run_info->groups + i;
        free(group->cmd_idxs);
    }
    sb_free(run_info->groups);
}

static void init_bench_params(const struct input_policy *input,
                              enum output_kind output, const struct meas *meas,
                              char *exec, char **argv, char *cmd_str,
                              struct bench_params *params) {
    params->input = *input;
    params->output = output;
    params->meas = meas;
    params->meas_count = sb_len(meas);
    params->exec = exec;
    params->argv = argv;
    params->str = cmd_str;
}

static bool attempt_group_rename(const struct rename_entry *rename_list,
                                 size_t grp_idx, struct bench_var_group *grp) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == grp_idx) {
            strlcpy(grp->name, rename_list[i].name, sizeof(grp->name));
            return true;
        }
    }
    return false;
}

static bool init_run_info(const struct cli_settings *cli,
                          struct run_info *info) {
    info->meas = cli->meas;
    info->var = cli->var;
    // Silently disable progress bar if output is inherit. The reasoning for
    // this is that inheriting output should only be used when debugging, and
    // user will not care about not having progress bar
    if (cli->output == OUTPUT_POLICY_INHERIT) {
        g_progress_bar = false;
    }

    size_t cmd_count = sb_len(cli->args);
    if (cmd_count == 0) {
        error("no commands specified");
        return false;
    }
    if (sb_len(cli->meas) == 0) {
        error("no measurements specified");
        return false;
    }

    struct input_policy input_policy = cli->input;
    if (input_policy.kind == INPUT_POLICY_FILE) {
        if (access(input_policy.file, R_OK) == -1) {
            error("failed to open file '%s' (specified for input)",
                  input_policy.file);
            return false;
        }
    }

    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->args[i];
        if (cli->var != NULL) {
            const struct bench_var *var = cli->var;
            char buf[4096];
            size_t value_count = var->value_count;
            struct bench_var_group group;
            memset(&group, 0, sizeof(group));
            group.cmd_idxs = calloc(value_count, sizeof(*group.cmd_idxs));
            for (size_t k = 0; k < value_count; ++k) {
                const char *var_value = var->values[k];
                char *exec = NULL, **argv = NULL;
                bool replaced = false;
                if (!replace_var_str(buf, sizeof(buf), cmd_str, var->name,
                                     var_value, &replaced) ||
                    !replaced ||
                    !init_cmd_exec(cli->shell, buf, &exec, &argv)) {
                    if (replaced) {
                        error("failed to initialize command '%s'", buf);
                    } else {
                        error("command string '%s' does not contain variable "
                              "substitutions",
                              buf);
                    }
                    free(group.cmd_idxs);
                    goto err_free_settings;
                }

                struct bench_params bench_params = {0};
                init_bench_params(&input_policy, cli->output, info->meas, exec,
                                  argv, strdup(buf), &bench_params);
                group.cmd_idxs[k] = sb_len(info->params);
                sb_push(info->params, bench_params);
            }
            if (!attempt_group_rename(cli->rename_list, sb_len(info->groups),
                                      &group))
                strlcpy(group.name, cmd_str, sizeof(group.name));
            sb_push(info->groups, group);
        } else {
            char *exec = NULL, **argv = NULL;
            if (!init_cmd_exec(cli->shell, cmd_str, &exec, &argv)) {
                error("failed to initialize command '%s'", cmd_str);
                goto err_free_settings;
            }
            struct bench_params bench_params = {0};
            init_bench_params(&input_policy, cli->output, info->meas, exec,
                              argv, strdup(cmd_str), &bench_params);
            sb_push(info->params, bench_params);
        }
    }
    if (cli->baseline > 0) {
        size_t baseline = cli->baseline - 1;
        size_t grp_count = sb_len(info->groups);
        size_t cmd_count = sb_len(info->params);
        if (grp_count <= 1) {
            // No parameterized benchmarks specified, just select the command
            if (baseline >= cmd_count) {
                error("baseline number is too big");
                goto err_free_settings;
            }
            g_baseline = baseline;
        } else {
            // Multiple parameterized benchmarks
            if (baseline >= grp_count) {
                error("baseline number is too big");
                goto err_free_settings;
            }
            g_baseline = baseline;
        }
    }
    for (size_t i = 0; i < sb_len(cli->meas); ++i) {
        if (cli->meas[i].kind == MEAS_CUSTOM) {
            g_has_custom_meas = 1;
            break;
        }
    }
    return true;
err_free_settings:
    free_run_info(info);
    return false;
}

#if defined(__APPLE__)
static double get_time(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
#else
static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static void apply_input_policy(const struct input_policy *policy) {
    switch (policy->kind) {
    case INPUT_POLICY_NULL: {
        int fd = open("/dev/null", O_RDWR);
        if (fd == -1)
            _exit(-1);
        close(STDIN_FILENO);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case INPUT_POLICY_FILE: {
        int fd = open(policy->file, O_RDONLY);
        if (fd == -1)
            _exit(-1);
        close(STDIN_FILENO);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    }
}

static void apply_output_policy(enum output_kind policy) {
    switch (policy) {
    case OUTPUT_POLICY_NULL: {
        int fd = open("/dev/null", O_RDWR);
        if (fd == -1)
            _exit(-1);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case OUTPUT_POLICY_INHERIT:
        break;
    }
}

static int exec_cmd(const struct bench_params *params, int stdout_fd,
                    struct rusage *rusage, struct perf_cnt *pmc) {
    bool success = true;
    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return -1;
    }

    if (pid == 0) {
        apply_input_policy(&params->input);
        // special handling when stdout needs to be piped
        if (stdout_fd != -1) {
            int fd = open("/dev/null", O_RDWR);
            if (fd == -1)
                _exit(-1);
            close(STDERR_FILENO);
            if (dup2(fd, STDERR_FILENO) == -1)
                _exit(-1);
            if (dup2(stdout_fd, STDOUT_FILENO) == -1)
                _exit(-1);
            close(fd);
        } else {
            apply_output_policy(params->output);
        }
        if (pmc != NULL) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGUSR1);
            int sig;
            sigwait(&set, &sig);
        }
        if (execvp(params->exec, params->argv) == -1)
            _exit(-1);
    } else {
        if (pmc != NULL && !perf_cnt_collect(pid, pmc)) {
            success = false;
            kill(pid, SIGKILL);
        }
    }

    int status = 0;
    pid_t wpid;
    if ((wpid = wait4(pid, &status, 0, rusage)) != pid) {
        if (wpid == -1)
            csperror("wait4");
        return -1;
    }

    int ret = -1;
    if (success) {
        // shell-like exit codes
        if (WIFEXITED(status))
            ret = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            ret = 128 + WTERMSIG(status);
    }
    if (ret == -1 && success)
        error("process finished with unexpected status");

    return ret;
}

static bool parse_custom_output(int fd, double *valuep) {
    char buf[4096];
    ssize_t nread = read(fd, buf, sizeof(buf));
    if (nread == -1) {
        csperror("read");
        return false;
    }
    if (nread == sizeof(buf)) {
        error("custom measurement output is too large");
        return false;
    }
    if (nread == 0) {
        error("custom measurement output is empty");
        return false;
    }
    buf[nread] = '\0';
    char *end = NULL;
    double value = strtod(buf, &end);
    if (end == buf) {
        error("invalid custom measurement output '%s'", buf);
        return false;
    }
    *valuep = value;
    return true;
}

static int tmpfile_fd(void) {
    char path[] = "/tmp/csbench_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        csperror("mkstemp");
        return -1;
    }
    unlink(path);
    return fd;
}

static bool do_custom_measurement(const struct meas *custom, int stdout_fd,
                                  double *valuep) {
    assert(stdout_fd > 0);
    bool success = false;
    int custom_output_fd = tmpfile_fd();
    if (custom_output_fd == -1)
        return false;

    if (lseek(stdout_fd, 0, SEEK_SET) == (off_t)-1 ||
        lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        goto out;
    }

    if (!execute_in_shell(custom->cmd, stdout_fd, custom_output_fd, -1))
        goto out;

    if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        goto out;
    }

    double value;
    if (!parse_custom_output(custom_output_fd, &value))
        goto out;

    *valuep = value;
    success = true;
out:
    assert(custom_output_fd != -1);
    close(custom_output_fd);
    return success;
}

// Execute benchmark and save output.
//
// This function contains some heavy logic. It handles the following:
// 1. Execute command
//  a. Using specified shell
//  b. Optionally setting stdin
//  c. Setting stdout and stderr, or saving stdout to file in case custom
//   measurements are used
// 2. Collect wall clock time duration of execution
// 2. Collect struct rusage of executed process
// 3. Optionally collect performance counters
// 4. Optionally check that command exit code is not zero
// 5. Execute custom measurements if specified
// 6. Save measurements specified in benchmark settings
static bool exec_and_measure(const struct bench_params *params,
                             struct bench *bench) {
    bool success = false;
    int stdout_fd = -1;
    if (g_has_custom_meas) {
        stdout_fd = tmpfile_fd();
        if (stdout_fd == -1)
            goto out;
    }

    struct rusage rusage;
    memset(&rusage, 0, sizeof(rusage));
    struct perf_cnt pmc_ = {0};
    struct perf_cnt *pmc = NULL;
    if (g_use_perf)
        pmc = &pmc_;
    volatile double wall_clock_start = get_time();
    volatile int rc = exec_cmd(params, stdout_fd, &rusage, pmc);
    volatile double wall_clock_end = get_time();

    if (rc == -1)
        goto out;

    if (!g_allow_nonzero && rc != 0) {
        error("command '%s' finished with non-zero exit code (%d)", params->str,
              rc);
        goto out;
    }

    ++bench->run_count;
    sb_push(bench->exit_codes, rc);
    for (size_t meas_idx = 0; meas_idx < params->meas_count; ++meas_idx) {
        const struct meas *meas = params->meas + meas_idx;
        double val = 0.0;
        switch (meas->kind) {
        case MEAS_WALL:
            val = wall_clock_end - wall_clock_start;
            break;
        case MEAS_RUSAGE_STIME:
            val =
                rusage.ru_stime.tv_sec + (double)rusage.ru_stime.tv_usec / 1e6;
            break;
        case MEAS_RUSAGE_UTIME:
            val =
                rusage.ru_utime.tv_sec + (double)rusage.ru_utime.tv_usec / 1e6;
            break;
        case MEAS_RUSAGE_MAXRSS:
            val = rusage.ru_maxrss;
            break;
        case MEAS_RUSAGE_MINFLT:
            val = rusage.ru_minflt;
            break;
        case MEAS_RUSAGE_MAJFLT:
            val = rusage.ru_majflt;
            break;
        case MEAS_RUSAGE_NVCSW:
            val = rusage.ru_nvcsw;
            break;
        case MEAS_RUSAGE_NIVCSW:
            val = rusage.ru_nivcsw;
            break;
        case MEAS_PERF_CYCLES:
            assert(pmc);
            val = pmc->cycles;
            break;
        case MEAS_PERF_INS:
            assert(pmc);
            val = pmc->instructions;
            break;
        case MEAS_PERF_BRANCH:
            assert(pmc);
            val = pmc->branches;
            break;
        case MEAS_PERF_BRANCHM:
            assert(pmc);
            val = pmc->missed_branches;
            break;
        case MEAS_CUSTOM:
            assert(stdout_fd > 0);
            if (!do_custom_measurement(params->meas + meas_idx, stdout_fd,
                                       &val))
                goto out;
            break;
        case MEAS_LOADED:
            assert(0);
        }
        sb_push(bench->meas[meas_idx], val);
    }

    success = true;
out:
    if (stdout_fd != -1)
        close(stdout_fd);
    return success;
}

static bool warmup(const struct bench_params *cmd) {
    double time_limit = g_warmup_time;
    if (time_limit <= 0.0)
        return true;
    double start_time = get_time();
    do {
        if (exec_cmd(cmd, -1, NULL, NULL) == -1) {
            error("failed to execute warmup command");
            return false;
        }
    } while (get_time() - start_time < time_limit);
    return true;
}

static void progress_bar_start(struct progress_bar_bench *bench, double time) {
    if (!g_progress_bar)
        return;
    uint64_t u;
    memcpy(&u, &time, sizeof(u));
    atomic_store(&bench->start_time.u, u);
}

static void progress_bar_abort(struct progress_bar_bench *bench) {
    if (!g_progress_bar)
        return;
    pthread_t id = pthread_self();
    memcpy(&bench->id, &id, sizeof(id));
    atomic_fence();
    atomic_store(&bench->aborted, true);
    atomic_store(&bench->finished, true);
}

static void progress_bar_finished(struct progress_bar_bench *bench) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->finished, true);
}

static void progress_bar_update_time(struct progress_bar_bench *bench,
                                     int percent, double t) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->bar, percent);
    uint64_t metric;
    memcpy(&metric, &t, sizeof(metric));
    atomic_store(&bench->metric.u, metric);
}

static void progress_bar_update_runs(struct progress_bar_bench *bench,
                                     int percent, size_t runs) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->bar, percent);
    atomic_store(&bench->metric.u, runs);
}

// This function is entry point to benchmark running. Accepting description of
// benchmark, it runs it according to 'g_bench_stop' policy, saving timing
// results.
//
// There are two main ways to execute benchmark: by time and by run count.
// In the first case benchmark is run until certain wall clock time has passed
// since benchmark start, optionally constrained bu minimum and maximum run
// count. In the second case we run benchmark exactly for specified number of
// runs.
//
// This function also handles progress bar (see 'run_benchmark').
// During the benchmark run, progress bar is notified about current state
// of run using atomic variables (lock free and wait free).
// If the progress bar is disabled nothing concerning it shall be done.
static bool run_benchmark(const struct bench_params *params,
                          struct bench *bench) {
    // Check if we should run fixed number of times.
    if (g_bench_stop.runs != 0) {
        progress_bar_start(bench->progress, get_time());
        for (int run_idx = 0; run_idx < g_bench_stop.runs; ++run_idx) {
            if (g_prepare && !execute_in_shell(g_prepare, -1, -1, -1)) {
                error("failed to execute prepare command");
                progress_bar_abort(bench->progress);
                return false;
            }
            if (!exec_and_measure(params, bench)) {
                progress_bar_abort(bench->progress);
                return false;
            }
            int percent = (run_idx + 1) * 100 / g_bench_stop.runs;
            progress_bar_update_runs(bench->progress, percent, run_idx + 1);
        }
        progress_bar_update_runs(bench->progress, 100, g_bench_stop.runs);
        progress_bar_finished(bench->progress);
        return true;
    }
    double niter_accum = 1;
    size_t niter = 1;
    double start_time = get_time();
    double time_limit = g_bench_stop.time_limit;
    size_t min_runs = g_bench_stop.min_runs;
    size_t max_runs = g_bench_stop.max_runs;
    progress_bar_start(bench->progress, start_time);
    for (size_t count = 1;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            if (g_prepare && !execute_in_shell(g_prepare, -1, -1, -1)) {
                error("failed to execute prepare command");
                progress_bar_abort(bench->progress);
                return false;
            }
            if (!exec_and_measure(params, bench)) {
                progress_bar_abort(bench->progress);
                return false;
            }
            double current = get_time();
            double diff = current - start_time;
            int progress = diff / time_limit * 100;
            progress_bar_update_time(bench->progress, progress, diff);
        }
        double current = get_time();
        double diff = current - start_time;
        if (((max_runs != 0 ? count >= max_runs : 0) || (diff > time_limit)) &&
            (min_runs != 0 ? count >= min_runs : 1))
            break;

        for (;;) {
            niter_accum *= 1.05;
            size_t new_niter = (size_t)floor(niter_accum);
            if (new_niter != niter)
                break;
        }
    }
    double passed = get_time() - start_time;
    progress_bar_update_time(bench->progress, 100, passed);
    progress_bar_finished(bench->progress);
    return true;
}

static void analyze_benchmark(struct bench_analysis *analysis,
                              size_t meas_count) {
    const struct bench *bench = analysis->bench;
    size_t count = bench->run_count;
    assert(count != 0);
    for (size_t i = 0; i < meas_count; ++i) {
        assert(sb_len(bench->meas[i]) == count);
        estimate_distr(bench->meas[i], count, g_nresamp, analysis->meas + i);
    }
}

struct bench_sort_state {
    size_t meas_idx;
    const struct bench_analysis *analyses;
};

#ifdef __linux__
static int bench_sort_cmp(const void *ap, const void *bp, void *statep) {
#elif defined(__APPLE__)
static int bench_sort_cmp(void *statep, const void *ap, const void *bp) {
#else
#error
#endif
    const struct bench_sort_state *state = statep;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    if (a_idx == b_idx)
        return 0;

    double va = state->analyses[a_idx].meas[state->meas_idx].mean.point;
    double vb = state->analyses[b_idx].meas[state->meas_idx].mean.point;
    return va > vb;
}

static void compare_benches(struct bench_results *results) {
    if (results->bench_count == 1)
        return;
    size_t bench_count = results->bench_count;
    size_t meas_count = results->meas_count;
    assert(meas_count != 0);
    for (size_t i = 0; i < meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;

        struct bench_sort_state state = {0};
        state.meas_idx = i;
        state.analyses = results->bench_analyses;
        for (size_t j = 0; j < bench_count; ++j)
            results->meas_analyses[i].fastest[j] = j;
#ifdef __linux__
        qsort_r(results->meas_analyses[i].fastest, bench_count,
                sizeof(*results->meas_analyses[i].fastest), bench_sort_cmp,
                &state);
#elif defined(__APPLE__)
        qsort_r(results->meas_analyses[i].fastest, bench_count,
                sizeof(*results->meas_analyses[i].fastest), &state,
                bench_sort_cmp);
#else
#error
#endif
    }
}

static void analyze_var_groups(struct bench_meas_analysis *analysis) {
    if (analysis->base->group_count == 0)
        return;
    const struct bench_var *var = analysis->base->var;
    for (size_t grp_idx = 0; grp_idx < analysis->base->group_count; ++grp_idx) {
        const struct bench_var_group *group =
            analysis->base->var_groups + grp_idx;
        struct group_analysis *grp_analysis =
            analysis->group_analyses + grp_idx;
        grp_analysis->group = group;
        grp_analysis->data =
            calloc(var->value_count, sizeof(*grp_analysis->data));
        bool values_are_doubles = true;
        double slowest = -INFINITY, fastest = INFINITY;
        for (size_t cmd_idx = 0; cmd_idx < var->value_count; ++cmd_idx) {
            const char *value = var->values[cmd_idx];
            size_t bench_idx = group->cmd_idxs[cmd_idx];
            char *end = NULL;
            double value_double = strtod(value, &end);
            if (end == value)
                values_are_doubles = false;
            double mean = analysis->benches[bench_idx]->mean.point;
            struct cmd_in_group_data *data = grp_analysis->data + cmd_idx;
            data->distr = analysis->benches[bench_idx];
            data->mean = mean;
            data->value = value;
            data->value_double = value_double;
            if (mean > slowest) {
                slowest = mean;
                grp_analysis->slowest = data;
            }
            if (mean < fastest) {
                fastest = mean;
                grp_analysis->fastest = data;
            }
        }
        grp_analysis->values_are_doubles = values_are_doubles;
        if (values_are_doubles) {
            double *x = calloc(var->value_count, sizeof(*x));
            double *y = calloc(var->value_count, sizeof(*y));
            for (size_t i = 0; i < var->value_count; ++i) {
                x[i] = grp_analysis->data[i].value_double;
                y[i] = grp_analysis->data[i].mean;
            }
            ols(x, y, var->value_count, &grp_analysis->regress);
            free(x);
            free(y);
        }
    }
    for (size_t val_idx = 0; val_idx < analysis->base->var->value_count;
         ++val_idx) {
        double fastest_mean = analysis->group_analyses[0].data[val_idx].mean;
        for (size_t grp_idx = 1; grp_idx < analysis->base->group_count;
             ++grp_idx) {
            double t = analysis->group_analyses[grp_idx].data[val_idx].mean;
            if (t < fastest_mean) {
                fastest_mean = t;
                analysis->fastest_val[val_idx] = grp_idx;
            }
        }
    }
}

static void ref_speed(double u1, double sigma1, double u2, double sigma2,
                      double *ref_u, double *ref_sigma) {
    // propagate standard deviation for formula (t1 / t2)
    double ref = u1 / u2;
    double a = sigma1 / u1;
    double b = sigma2 / u2;
    double ref_st_dev = ref * sqrt(a * a + b * b);
    *ref_u = ref;
    *ref_sigma = ref_st_dev;
}

static void calculate_speedups(struct bench_meas_analysis *analysis) {
    if (analysis->base->bench_count == 1)
        return;
    bool flip = false;
    const struct distr *reference;
    if (g_baseline != -1) {
        reference = analysis->benches[g_baseline];
    } else {
        reference = analysis->benches[analysis->fastest[0]];
        flip = true;
    }
    for (size_t bench_idx = 0; bench_idx < analysis->base->bench_count;
         ++bench_idx) {
        const struct distr *bench = analysis->benches[bench_idx];
        if (bench == reference)
            continue;
        struct point_err_est *est = analysis->speedup + bench_idx;
        if (flip)
            ref_speed(bench->mean.point, bench->st_dev.point,
                      reference->mean.point, reference->st_dev.point,
                      &est->point, &est->err);
        else
            ref_speed(reference->mean.point, reference->st_dev.point,
                      bench->mean.point, bench->st_dev.point, &est->point,
                      &est->err);
    }
    if (analysis->base->var && analysis->base->group_count > 1) {
        for (size_t val_idx = 0; val_idx < analysis->base->var->value_count;
             ++val_idx) {
            const struct group_analysis *reference_group;
            bool flip = false;
            if (g_baseline != -1) {
                reference_group = analysis->group_analyses + g_baseline;
            } else {
                reference_group =
                    analysis->group_analyses + analysis->fastest_val[val_idx];
                flip = true;
            }
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *group =
                    analysis->group_analyses + grp_idx;
                if (group == reference_group)
                    continue;
                struct point_err_est *est =
                    analysis->var_speedup[val_idx] + grp_idx;
                if (flip)
                    ref_speed(
                        group->data[val_idx].distr->mean.point,
                        group->data[val_idx].distr->st_dev.point,
                        reference_group->data[val_idx].distr->mean.point,
                        reference_group->data[val_idx].distr->st_dev.point,
                        &est->point, &est->err);
                else
                    ref_speed(
                        reference_group->data[val_idx].distr->mean.point,
                        reference_group->data[val_idx].distr->st_dev.point,
                        group->data[val_idx].distr->mean.point,
                        group->data[val_idx].distr->st_dev.point, &est->point,
                        &est->err);
            }
        }
        if (g_baseline != -1 && !g_regr) {
            const struct group_analysis *baseline_group =
                analysis->group_analyses + g_baseline;
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *group =
                    analysis->group_analyses + grp_idx;
                if (group == baseline_group)
                    continue;
                // This uses hand-written error propagation formula, for
                // reference see
                // https://en.wikipedia.org/wiki/Propagation_of_uncertainty.
                double mean_accum = 1;
                double st_dev_accum = 0.0;
                size_t n = analysis->base->var->value_count;
                for (size_t val_idx = 0; val_idx < n; ++val_idx) {
                    const struct point_err_est *est =
                        analysis->var_speedup[val_idx] + grp_idx;
                    mean_accum *= est->point;
                    double a = pow(est->point, 1.0 / n - 1.0) * est->err;
                    st_dev_accum += a * a;
                }
                double av = pow(mean_accum, 1.0 / n);
                double av_st_dev = av / n * sqrt(st_dev_accum);
                struct point_err_est *est =
                    analysis->group_baseline_speedup + grp_idx;
                est->point = av;
                est->err = av_st_dev;
            }
        }
    }
}

static void calculate_p_values(struct bench_meas_analysis *analysis) {
    {
        const struct distr *d1;
        if (g_baseline != -1)
            d1 = analysis->benches[g_baseline];
        else
            d1 = analysis->benches[analysis->fastest[0]];
        for (size_t bench_idx = 0; bench_idx < analysis->base->bench_count;
             ++bench_idx) {
            const struct distr *d2 = analysis->benches[bench_idx];
            if (d1 == d2)
                continue;
            double p = mwu(d1->data, d1->count, d2->data, d2->count);
            analysis->p_values[bench_idx] = p;
        }
    }
    if (analysis->base->group_count > 1) {
        size_t var_value_count = analysis->base->var->value_count;
        for (size_t value_idx = 0; value_idx < var_value_count; ++value_idx) {
            const struct distr *d1;
            if (g_baseline != -1)
                d1 = analysis->group_analyses[g_baseline].data[value_idx].distr;
            else
                d1 = analysis->group_analyses[analysis->fastest_val[value_idx]]
                         .data[value_idx]
                         .distr;
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                const struct distr *d2 =
                    analysis->group_analyses[grp_idx].data[value_idx].distr;
                if (d1 == d2)
                    continue;
                double p = mwu(d1->data, d1->count, d2->data, d2->count);
                analysis->var_p_values[value_idx][grp_idx] = p;
            }
        }
    }
}

static void print_exit_code_info(const struct bench *bench) {
    if (!bench->exit_codes)
        return;

    size_t count_nonzero = 0;
    for (size_t i = 0; i < bench->run_count; ++i)
        if (bench->exit_codes[i] != 0)
            ++count_nonzero;

    assert(g_allow_nonzero ? 1 : count_nonzero == 0);
    if (count_nonzero == bench->run_count) {
        printf("all commands have non-zero exit code: %d\n",
               bench->exit_codes[0]);
    } else if (count_nonzero != 0) {
        printf("some runs (%zu) have non-zero exit code\n", count_nonzero);
    }
}

static void print_outliers(const struct outliers *outliers, size_t run_count) {
    int outlier_count = outliers->low_mild + outliers->high_mild +
                        outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        printf("%d outliers (%.2f%%) %s (%.1f%%) effect on st dev\n",
               outlier_count, (double)outlier_count / run_count * 100.0,
               outliers_variance_str(outliers->var), outliers->var * 100.0);
        if (outliers->low_severe)
            printf("  %d (%.2f%%) low severe\n", outliers->low_severe,
                   (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            printf("  %d (%.2f%%) low mild\n", outliers->low_mild,
                   (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            printf("  %d (%.2f%%) high mild\n", outliers->high_mild,
                   (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            printf("  %d (%.2f%%) high severe\n", outliers->high_severe,
                   (double)outliers->high_severe / run_count * 100.0);
    } else {
        printf("outliers have %s (%.1f%%) effect on st dev\n",
               outliers_variance_str(outliers->var), outliers->var * 100.0);
    }
}

static void print_estimate(const char *name, const struct est *est,
                           const struct units *units, const char *prim_color,
                           const char *sec_color) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);

    printf_colored(prim_color, "%7s", name);
    printf_colored(sec_color, " %8s ", buf1);
    printf_colored(prim_color, "%8s", buf2);
    printf_colored(sec_color, " %8s\n", buf3);
}

static void print_distr(const struct distr *dist, const struct units *units) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), dist->min, units);
    format_meas(buf2, sizeof(buf2), dist->median, units);
    format_meas(buf3, sizeof(buf3), dist->max, units);
    printf_colored(ANSI_BOLD_MAGENTA, " q{024} ");
    printf_colored(ANSI_MAGENTA, "%s ", buf1);
    printf_colored(ANSI_BOLD_MAGENTA, "%s ", buf2);
    printf_colored(ANSI_MAGENTA, "%s\n", buf3);
    print_estimate("mean", &dist->mean, units, ANSI_BOLD_GREEN,
                   ANSI_BRIGHT_GREEN);
    print_estimate("st dev", &dist->st_dev, units, ANSI_BOLD_GREEN,
                   ANSI_BRIGHT_GREEN);
}

static void print_benchmark_info(const struct bench_analysis *analysis,
                                 const struct bench_results *results) {
    const struct bench *bench = analysis->bench;
    size_t run_count = bench->run_count;
    printf("command ");
    printf_colored(ANSI_BOLD, "%s\n", analysis->name);
    // Print runs count only if it not explicitly specified, otherwise it is
    // printed in 'print_analysis'
    if (g_bench_stop.runs == 0)
        printf("%zu runs\n", bench->run_count);
    print_exit_code_info(bench);
    if (results->primary_meas_count != 0) {
        for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
            const struct meas *meas = results->meas + meas_idx;
            if (meas->is_secondary)
                continue;

            if (results->primary_meas_count != 1) {
                printf("measurement ");
                printf_colored(ANSI_YELLOW, "%s\n", meas->name);
            }
            const struct distr *distr = analysis->meas + meas_idx;
            print_distr(distr, &meas->units);
            for (size_t j = 0; j < results->meas_count; ++j) {
                if (results->meas[j].is_secondary &&
                    results->meas[j].primary_idx == meas_idx)
                    print_estimate(results->meas[j].name,
                                   &analysis->meas[j].mean,
                                   &results->meas[j].units, ANSI_BOLD_BLUE,
                                   ANSI_BRIGHT_BLUE);
            }
            print_outliers(&distr->outliers, run_count);
        }
    } else {
        for (size_t i = 0; i < results->meas_count; ++i) {
            const struct meas *info = results->meas + i;
            print_estimate(info->name, &analysis->meas[i].mean, &info->units,
                           ANSI_BOLD_BLUE, ANSI_BRIGHT_BLUE);
        }
    }
}

static void print_cmd_comparison(const struct bench_meas_analysis *analysis) {
    if (analysis->base->bench_count == 1)
        return;

    if (analysis->base->group_count <= 1) {
        const struct bench_analysis *reference;
        if (g_baseline != -1) {
            printf("baseline command ");
            reference = analysis->base->bench_analyses + g_baseline;
        } else {
            printf("fastest command ");
            reference = analysis->base->bench_analyses + analysis->fastest[0];
        }
        printf_colored(ANSI_BOLD, "%s\n", reference->name);
        for (size_t j = 0; j < analysis->base->bench_count; ++j) {
            size_t bench_idx = j;
            if (g_baseline == -1)
                bench_idx = analysis->fastest[j];
            const struct bench_analysis *bench =
                analysis->base->bench_analyses + bench_idx;
            if (bench == reference)
                continue;
            const struct point_err_est *speedup = analysis->speedup + bench_idx;
            if (g_baseline != -1)
                printf_colored(ANSI_BOLD, "  %s", bench->name);
            else
                printf_colored(ANSI_BOLD, "  %s", reference->name);
            printf(" is ");
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->err);
            printf(" times faster than ");
            if (g_baseline == -1)
                printf_colored(ANSI_BOLD, "%s", bench->name);
            else
                printf("baseline");
            printf(" (p=%.2f)", analysis->p_values[bench_idx]);
            printf("\n");
        }
        if (analysis->base->group_count == 1 && g_regr) {
            const struct group_analysis *grp = analysis->group_analyses;
            if (grp->values_are_doubles)
                printf("%s complexity (%g)\n",
                       big_o_str(grp->regress.complexity), grp->regress.a);
        }
    } else {
        for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
             ++grp_idx) {
            printf("%c = ", (int)('A' + grp_idx));
            printf_colored(ANSI_BOLD, "%s\n",
                           analysis->group_analyses[grp_idx].group->name);
        }

        if (g_baseline != -1)
            printf("baseline is %c\n", (int)('A' + g_baseline));
        const struct bench_var *var = analysis->base->var;
        size_t value_count = var->value_count;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            const char *value = var->values[val_idx];
            size_t reference_idx;
            if (g_baseline == -1)
                reference_idx = analysis->fastest_val[val_idx];
            else
                reference_idx = g_baseline;

            printf("%s=%s:\t", var->name, value);
            if (g_baseline == -1) {
                printf("%c is ", (int)('A' + reference_idx));
            }
            const char *ident = "";
            if (analysis->base->group_count > 2) {
                printf("\n");
                ident = "  ";
            }
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                if (grp_idx == reference_idx)
                    continue;
                const struct point_err_est *est =
                    analysis->var_speedup[val_idx] + grp_idx;
                printf("%s", ident);
                if (g_baseline != -1)
                    printf("%c is ", (int)('A' + grp_idx));
                printf_colored(ANSI_BOLD_GREEN, "%6.3f", est->point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", est->err);
                printf(" times faster than ");
                if (g_baseline == -1)
                    printf("%c", (int)('A' + grp_idx));
                else
                    printf("baseline");
                printf(" (p=%.2f)", analysis->var_p_values[val_idx][grp_idx]);
                printf("\n");
            }
        }
        if (g_regr) {
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *grp =
                    analysis->group_analyses + grp_idx;
                if (grp->values_are_doubles) {
                    printf_colored(ANSI_BOLD, "%s ", grp->group->name);
                    printf("%s complexity (%g)\n",
                           big_o_str(grp->regress.complexity), grp->regress.a);
                }
            }
        } else if (g_baseline != -1) {
            printf("on average ");
            const char *ident = "";
            if (analysis->base->group_count > 2) {
                printf("\n");
                ident = "  ";
            }
            for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
                 ++grp_idx) {
                if (grp_idx == (size_t)g_baseline)
                    continue;
                const struct point_err_est *est =
                    analysis->group_baseline_speedup + grp_idx;
                printf("%s%c is ", ident, (int)('A' + grp_idx));
                printf_colored(ANSI_BOLD_GREEN, "%.3f", est->point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", est->err);
                printf(" times faster than baseline\n");
            }
        }
    }
}

static bool json_escape(char *buf, size_t buf_size, const char *src) {
    if (src == NULL) {
        assert(buf_size);
        *buf = '\0';
        return true;
    }
    char *end = buf + buf_size;
    while (*src) {
        if (buf >= end)
            return false;

        int c = *src++;
        if (c == '\"') {
            *buf++ = '\\';
            if (buf >= end)
                return false;
            *buf++ = c;
        } else {
            *buf++ = c;
        }
    }
    if (buf >= end)
        return false;
    *buf = '\0';
    return true;
}

static bool export_json(const struct bench_results *results,
                        const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        error("failed to open file '%s' for export", filename);
        return false;
    }

    char buf[4096];
    size_t bench_count = results->bench_count;
    const struct bench_analysis *bench_analyses = results->bench_analyses;
    fprintf(f,
            "{ \"settings\": {"
            "\"time_limit\": %f, \"runs\": %d, \"min_runs\": %d, "
            "\"max_runs\": %d, \"warmup_time\": %f, \"nresamp\": %d "
            "}, \"benches\": [",
            g_bench_stop.time_limit, g_bench_stop.runs, g_bench_stop.min_runs,
            g_bench_stop.max_runs, g_warmup_time, g_nresamp);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct bench_analysis *analysis = bench_analyses + i;
        const struct bench *bench = analysis->bench;
        fprintf(f, "{ ");
        if (g_prepare)
            json_escape(buf, sizeof(buf), g_prepare);
        else
            *buf = '\0';
        fprintf(f, "\"prepare\": \"%s\", ", buf);
        json_escape(buf, sizeof(buf), analysis->name);
        fprintf(f, "\"command\": \"%s\", ", buf);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"exit_codes\": [");
        for (size_t j = 0; j < run_count; ++j)
            fprintf(f, "%d%s", bench->exit_codes[j],
                    j != run_count - 1 ? ", " : "");
        fprintf(f, "], \"meas\": [");
        for (size_t j = 0; j < results->meas_count; ++j) {
            const struct meas *info = results->meas + j;
            json_escape(buf, sizeof(buf), info->name);
            fprintf(f, "{ \"name\": \"%s\", ", buf);
            json_escape(buf, sizeof(buf), units_str(&info->units));
            fprintf(f, "\"units\": \"%s\",", buf);
            json_escape(buf, sizeof(buf), info->cmd);
            fprintf(f,
                    " \"cmd\": \"%s\", "
                    "\"val\": [",
                    buf);
            for (size_t k = 0; k < run_count; ++k)
                fprintf(f, "%f%s", bench->meas[j][k],
                        k != run_count - 1 ? ", " : "");
            fprintf(f, "]}");
            if (j != results->meas_count - 1)
                fprintf(f, ", ");
        }
        fprintf(f, "]}");
        if (i != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return true;
}

static void export_csv_raw_bench(const struct bench *bench,
                                 const struct bench_results *results, FILE *f) {
    for (size_t i = 0; i < results->meas_count; ++i) {
        fprintf(f, "%s", results->meas[i].name);
        if (i != results->meas_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < bench->run_count; ++i) {
        for (size_t j = 0; j < results->meas_count; ++j) {
            fprintf(f, "%g", bench->meas[j][i]);
            if (j != results->meas_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_group_results(const struct bench_meas_analysis *analysis,
                                     FILE *f) {
    assert(analysis->base->group_count > 1 && analysis->base->var);
    fprintf(f, "%s,", analysis->base->var->name);
    for (size_t i = 0; i < analysis->base->group_count; ++i) {
        char buf[4096];
        json_escape(buf, sizeof(buf), analysis->group_analyses[i].group->name);
        fprintf(f, "%s", buf);
        if (i != analysis->base->group_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < analysis->base->var->value_count; ++i) {
        fprintf(f, "%s,", analysis->base->var->values[i]);
        for (size_t j = 0; j < analysis->base->group_count; ++j) {
            fprintf(f, "%g", analysis->group_analyses[j].data[i].mean);
            if (j != analysis->base->group_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_bench_results(const struct bench_results *results,
                                     size_t meas_idx, FILE *f) {
    fprintf(f,
            "cmd,mean_low,mean,mean_high,st_dev_low,st_dev,st_dev_high,min,max,"
            "median,q1,q3,p1,p5,p95,p99,outl\n");
    for (size_t i = 0; i < results->bench_count; ++i) {
        const struct distr *distr = results->bench_analyses[i].meas + meas_idx;
        char buf[4096];
        json_escape(buf, sizeof(buf), results->bench_analyses[i].name);
        fprintf(f, "%s,", buf);
        fprintf(f, "%g,%g,%g,%g,%g,%g,", distr->mean.lower, distr->mean.point,
                distr->mean.upper, distr->st_dev.lower, distr->st_dev.point,
                distr->st_dev.upper);
        fprintf(f, "%g,%g,%g,%g,%g,%g,%g,%g,%g,", distr->min, distr->max,
                distr->median, distr->q1, distr->q3, distr->p1, distr->p5,
                distr->p95, distr->p99);
        fprintf(f, "%g\n", distr->outliers.var);
    }
}

static bool export_csvs(const struct bench_results *results) {
    char buf[4096];
    for (size_t bench_idx = 0; bench_idx < results->bench_count; ++bench_idx) {
        snprintf(buf, sizeof(buf), "%s/bench_raw_%zu.csv", g_out_dir,
                 bench_idx);
        FILE *f = fopen(buf, "w");
        if (f == NULL)
            return false;
        export_csv_raw_bench(results->bench_analyses[bench_idx].bench, results,
                             f);
        fclose(f);
    }
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        snprintf(buf, sizeof(buf), "%s/bench_%zu.csv", g_out_dir, meas_idx);
        FILE *f = fopen(buf, "w");
        if (f == NULL)
            return false;
        export_csv_bench_results(results, meas_idx, f);
        fclose(f);
    }
    if (results->group_count) {
        for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
            if (results->meas[meas_idx].is_secondary)
                continue;
            snprintf(buf, sizeof(buf), "%s/group_%zu.csv", g_out_dir, meas_idx);
            FILE *f = fopen(buf, "w");
            if (f == NULL)
                return false;
            export_csv_group_results(results->meas_analyses + meas_idx, f);
            fclose(f);
        }
    }
    return true;
}

static bool python_found(void) {
    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", "--version", NULL) == -1)
            _exit(-1);
    }
    return process_finished_correctly(pid);
}

static bool launch_python_stdin_pipe(FILE **inp, pid_t *pidp) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        csperror("pipe");
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[1]);
        close(STDIN_FILENO);
        if (dup2(pipe_fds[0], STDIN_FILENO) == -1)
            _exit(-1);
        if (!g_python_output) {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
        if (execlp("python3", "python3", NULL) == -1)
            _exit(-1);
    }
    close(pipe_fds[0]);
    FILE *f = fdopen(pipe_fds[1], "w");
    if (f == NULL) {
        csperror("fdopen");
        // Not a very nice way of handling errors, but it seems correct.
        close(pipe_fds[1]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }
    *pidp = pid;
    *inp = f;
    return true;
}

static bool python_has_matplotlib(void) {
    FILE *f;
    pid_t pid;
    if (!launch_python_stdin_pipe(&f, &pid))
        return false;
    fprintf(f, "import matplotlib\n");
    fclose(f);
    return process_finished_correctly(pid);
}

__attribute__((format(printf, 2, 3))) static FILE *
open_file_fmt(const char *mode, const char *fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return fopen(buf, mode);
}

static bool plot_walker(bool (*walk)(struct plot_walker_args *args),
                        struct plot_walker_args *args) {
    const struct bench_meas_analysis *analysis = args->analysis;
    if (analysis->base->bench_count > 1) {
        if (analysis->base->group_count <= 1) {
            args->plot_kind = PLOT_BAR;
            if (!walk(args))
                return false;
        } else {
            args->plot_kind = PLOT_GROUP_BAR;
            if (!walk(args))
                return false;
        }
    }
    if (g_regr) {
        for (size_t grp_idx = 0; grp_idx < analysis->base->group_count;
             ++grp_idx) {
            const struct group_analysis *grp =
                analysis->group_analyses + grp_idx;
            if (!grp->values_are_doubles)
                break;
            args->plot_kind = PLOT_GROUP_SINGLE;
            args->grp_idx = grp_idx;
            if (!walk(args))
                return false;
        }
        if (analysis->base->group_count > 1) {
            const struct group_analysis *grp = analysis->group_analyses;
            if (grp[0].values_are_doubles) {
                args->plot_kind = PLOT_GROUP;
                if (!walk(args))
                    return false;
            }
        }
    }
    for (size_t bench_idx = 0; bench_idx < analysis->base->bench_count;
         ++bench_idx) {
        args->plot_kind = PLOT_KDE;
        args->bench_idx = bench_idx;
        if (!walk(args))
            return false;
        args->plot_kind = PLOT_KDE_EXT;
        args->bench_idx = bench_idx;
        if (!walk(args))
            return false;
    }
    if (analysis->base->group_count == 2) {
        size_t value_count = analysis->base->var->value_count;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            args->plot_kind = PLOT_KDE_CMPG;
            args->var_value_idx = val_idx;
            if (!walk(args))
                return false;
        }
    } else if (analysis->base->bench_count == 2) {
        args->plot_kind = PLOT_KDE_CMP;
        if (!walk(args))
            return false;
    }
    return true;
}

static void format_plot_name(char *buf, size_t buf_size,
                             struct plot_walker_args *args,
                             const char *extension) {
    switch (args->plot_kind) {
    case PLOT_BAR:
        snprintf(buf, buf_size, "%s/bar_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_GROUP_BAR:
        snprintf(buf, buf_size, "%s/group_bar_%zu.%s", g_out_dir,
                 args->meas_idx, extension);
        break;
    case PLOT_GROUP_SINGLE:
        snprintf(buf, buf_size, "%s/group_%zu_%zu.%s", g_out_dir, args->grp_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_GROUP:
        snprintf(buf, buf_size, "%s/group_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_KDE:
        snprintf(buf, buf_size, "%s/kde_%zu_%zu.%s", g_out_dir, args->bench_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_KDE_EXT:
        snprintf(buf, buf_size, "%s/kde_ext_%zu_%zu.%s", g_out_dir,
                 args->bench_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMPG:
        snprintf(buf, buf_size, "%s/kde_cmpg_%zu_%zu.%s", g_out_dir,
                 args->var_value_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP:
        snprintf(buf, buf_size, "%s/kde_cmp_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    }
}

static void write_make_plot(struct plot_walker_args *args, FILE *f) {
    char svg_buf[4096];
    size_t grp_idx = args->grp_idx;
    size_t bench_idx = args->bench_idx;
    size_t var_value_idx = args->var_value_idx;
    const struct bench_meas_analysis *analysis = args->analysis;
    const struct meas *meas = analysis->meas;
    format_plot_name(svg_buf, sizeof(svg_buf), args, "svg");
    switch (args->plot_kind) {
    case PLOT_BAR:
        bar_plot(analysis, svg_buf, f);
        break;
    case PLOT_GROUP_BAR:
        group_bar_plot(analysis, svg_buf, f);
        break;
    case PLOT_GROUP_SINGLE: {
        group_plot(analysis->group_analyses + grp_idx, 1, meas,
                   analysis->base->var, svg_buf, f);
        break;
    }
    case PLOT_GROUP: {
        group_plot(analysis->group_analyses, analysis->base->group_count, meas,
                   analysis->base->var, svg_buf, f);
        break;
    }
    case PLOT_KDE: {
        kde_plot(analysis->benches[bench_idx], meas, svg_buf, f);
        break;
    }
    case PLOT_KDE_EXT: {
        kde_plot_ext(analysis->benches[bench_idx], meas, svg_buf, f);
        break;
    }
    case PLOT_KDE_CMPG: {
        const struct group_analysis *a = analysis->group_analyses;
        const struct group_analysis *b = analysis->group_analyses + 1;
        kde_cmp_plot(analysis->benches[a->group->cmd_idxs[var_value_idx]],
                     analysis->benches[b->group->cmd_idxs[var_value_idx]], meas,
                     svg_buf, f);
        break;
    }
    case PLOT_KDE_CMP:
        kde_cmp_plot(analysis->benches[0], analysis->benches[1], meas, svg_buf,
                     f);
        break;
    }
}

static bool dump_plot_walk(struct plot_walker_args *args) {
    char py_buf[4096];
    format_plot_name(py_buf, sizeof(py_buf), args, "py");
    FILE *py_file = fopen(py_buf, "w");
    if (py_file == NULL) {
        error("failed to create file %s", py_buf);
        return false;
    }
    write_make_plot(args, py_file);
    fclose(py_file);
    return true;
}

static bool dump_plot_src(const struct bench_results *results) {
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        struct plot_walker_args args = {0};
        args.analysis = results->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(dump_plot_walk, &args))
            return false;
    }
    return true;
}

static bool make_plot_walk(struct plot_walker_args *args) {
    FILE *f;
    pid_t pid;
    if (!launch_python_stdin_pipe(&f, &pid)) {
        error("failed to launch python");
        return false;
    }
    write_make_plot(args, f);
    fclose(f);
    sb_push(args->pids, pid);
    return true;
}

static bool make_plots(const struct bench_results *results) {
    bool success = true;
    struct plot_walker_args args = {0};
    for (size_t meas_idx = 0; meas_idx < results->meas_count && success;
         ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        args.analysis = results->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(make_plot_walk, &args))
            success = false;
    }
    for (size_t i = 0; i < sb_len(args.pids); ++i) {
        if (!process_finished_correctly(args.pids[i])) {
            error("python finished with non-zero exit code");
            success = false;
        }
    }
    sb_free(args.pids);
    return success;
}

static bool make_plots_readme(const struct bench_results *results) {
    FILE *f = open_file_fmt("w", "%s/readme.md", g_out_dir);
    if (f == NULL) {
        error("failed to create file %s/readme.md", g_out_dir);
        return false;
    }
    fprintf(f, "# csbench analyze map\n");
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = results->meas + meas_idx;
        fprintf(f, "## measurement %s\n", meas->name);
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            const struct group_analysis *analysis =
                results->meas_analyses[meas_idx].group_analyses + grp_idx;
            fprintf(f,
                    "* [command group '%s' regression "
                    "plot](group_%zu_%zu.svg)\n",
                    analysis->group->name, grp_idx, meas_idx);
        }
        fprintf(f, "### KDE plots\n");
        fprintf(f, "#### regular\n");
        for (size_t bench_idx = 0; bench_idx < results->bench_count;
             ++bench_idx) {
            const struct bench_analysis *analysis =
                results->bench_analyses + bench_idx;
            const char *cmd_str = analysis->name;
            fprintf(f, "* [%s](kde_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
        fprintf(f, "#### extended\n");
        for (size_t bench_idx = 0; bench_idx < results->bench_count;
             ++bench_idx) {
            const struct bench_analysis *analysis =
                results->bench_analyses + bench_idx;
            const char *cmd_str = analysis->name;
            fprintf(f, "* [%s](kde_ext_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
    }
    fclose(f);
    return true;
}

static void html_estimate(const char *name, const struct est *est,
                          const struct units *units, FILE *f) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);
    fprintf(f,
            "<tr>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "</tr>",
            name, buf1, buf2, buf3);
}

static void html_outliers(const struct outliers *outliers, size_t run_count,
                          FILE *f) {
    int outlier_count = outliers->low_mild + outliers->high_mild +
                        outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        fprintf(f, "<p>found %d outliers (%.2f%%)</p><ul>", outlier_count,
                (double)outlier_count / run_count * 100.0);
        if (outliers->low_severe)
            fprintf(f, "<li>%d (%.2f%%) low severe</li>", outliers->low_severe,
                    (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            fprintf(f, "<li>%d (%.2f%%) low mild</li>", outliers->low_mild,
                    (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            fprintf(f, "<li>%d (%.2f%%) high mild</li>", outliers->high_mild,
                    (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            fprintf(f, "<li>%d (%.2f%%) high severe</li>",
                    outliers->high_severe,
                    (double)outliers->high_severe / run_count * 100.0);
        fprintf(f, "</ul>");
    }
    fprintf(f,
            "<p>outlying measurements have %s (%.1f%%) effect on "
            "estimated "
            "standard deviation</p>",
            outliers_variance_str(outliers->var), outliers->var * 100.0);
}

static void html_distr(const struct bench_analysis *analysis, size_t bench_idx,
                       size_t meas_idx, const struct bench_results *results,
                       FILE *f) {
    const struct distr *distr = analysis->meas + meas_idx;
    const struct bench *bench = analysis->bench;
    const struct meas *info = results->meas + meas_idx;
    assert(!info->is_secondary);
    fprintf(f,
            "<div class=\"row\">"
            "<div class=\"col\"><h3>%s kde plot</h3>"
            "<a href=\"kde_ext_%zu_%zu.svg\"><img "
            "src=\"kde_%zu_%zu.svg\"></a></div>",
            info->name, bench_idx, meas_idx, bench_idx, meas_idx);
    fprintf(f,
            "<div class=\"col\"><h3>statistics</h3>"
            "<div class=\"stats\">"
            "<p>%zu runs</p>",
            bench->run_count);
    char buf[256];
    format_meas(buf, sizeof(buf), distr->min, &info->units);
    fprintf(f, "<p>min %s</p>", buf);
    format_meas(buf, sizeof(buf), distr->max, &info->units);
    fprintf(f, "<p>max %s</p>", buf);
    fprintf(f, "<table><thead><tr>"
               "<th></th>"
               "<th class=\"est-bound\">lower bound</th>"
               "<th class=\"est-bound\">estimate</th>"
               "<th class=\"est-bound\">upper bound</th>"
               "</tr></thead><tbody>");
    html_estimate("mean", &distr->mean, &info->units, f);
    html_estimate("st dev", &distr->st_dev, &info->units, f);
    for (size_t j = 0; j < results->meas_count; ++j) {
        if (results->meas[j].is_secondary &&
            results->meas[j].primary_idx == meas_idx)
            html_estimate(results->meas[j].name, &analysis->meas[j].mean,
                          &results->meas->units, f);
    }
    fprintf(f, "</tbody></table>");
    html_outliers(&distr->outliers, bench->run_count, f);
    fprintf(f, "</div></div></div>");
}

static void html_compare(const struct bench_results *results, FILE *f) {
    if (results->bench_count == 1)
        return;
    fprintf(f, "<div><h2>measurement comparison</h2>");
    size_t meas_count = results->meas_count;
    for (size_t meas_idx = 0; meas_idx < meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = results->meas + meas_idx;
        fprintf(f,
                "<div><h3>%s comparison</h3>"
                "<div class=\"row\"><div class=\"col\">"
                "<img src=\"bar_%zu.svg\"></div>",
                meas->name, meas_idx);
        if (results->bench_count == 2)
            fprintf(f,
                    "<div class=\"col\">"
                    "<img src=\"kde_cmp_%zu.svg\"></div>",
                    meas_idx);
        fprintf(f, "</div></div></div>");
    }
    fprintf(f, "</div>");
}

static void html_bench_group(const struct group_analysis *analysis,
                             const struct meas *meas, size_t meas_idx,
                             size_t grp_idx, const struct bench_var *var,
                             FILE *f) {
    fprintf(f,
            "<h4>measurement %s</h4>"
            "<div class=\"row\"><div class=\"col\">"
            "<img src=\"group_%zu_%zu.svg\"></div>",
            meas->name, grp_idx, meas_idx);
    char buf[256];
    format_time(buf, sizeof(buf), analysis->fastest->mean);
    fprintf(f,
            "<div class=\"col stats\">"
            "<p>lowest time %s with %s=%s</p>",
            buf, var->name, analysis->fastest->value);
    format_time(buf, sizeof(buf), analysis->slowest->mean);
    fprintf(f, "<p>hightest time %s with %s=%s</p>", buf, var->name,
            analysis->slowest->value);
    if (analysis->values_are_doubles) {
        fprintf(f,
                "<p>mean time is most likely %s in terms of parameter</p>"
                "<p>linear coef %g rms %.3f</p>",
                big_o_str(analysis->regress.complexity), analysis->regress.a,
                analysis->regress.rms);
    }
    fprintf(f, "</div></div>");
}

static void html_var_analysis(const struct bench_results *results, FILE *f) {
    if (!results->group_count)
        return;
    fprintf(f, "<div><h2>variable analysis</h2>");
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        if (results->group_count > 1)
            fprintf(f,
                    "<div><h3>summary for %s</h3>"
                    "<div class=\"row\"><div class=\"col\">"
                    "<img src=\"group_%zu.svg\"></div>"
                    "<div class=\"col\">"
                    "<img src=\"group_bar_%zu.svg\">"
                    "</div></div></div></div>",
                    results->meas[meas_idx].name, meas_idx, meas_idx);
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            fprintf(
                f, "<div><h3>group '%s' with value %s</h3>",
                results->meas_analyses[0].group_analyses[grp_idx].group->name,
                results->var->name);
            const struct meas *meas = results->meas + meas_idx;
            const struct group_analysis *analysis =
                results->meas_analyses[meas_idx].group_analyses + grp_idx;
            html_bench_group(analysis, meas, meas_idx, grp_idx, results->var,
                             f);
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</div>");
}

static void html_report(const struct bench_results *results, FILE *f) {
    fprintf(f,
            "<!DOCTYPE html><html lang=\"en\">"
            "<head><meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, "
            "initial-scale=1.0\">"
            "<title>csbench</title>"
            "<style>body { margin: 40px auto; max-width: 960px; line-height: "
            "1.6; color: #444; padding: 0 10px; font: 14px Helvetica Neue }"
            "h1, h2, h3, h4 { line-height: 1.2; text-align: center }"
            ".est-bound { opacity: 0.5 }"
            "th, td { padding-right: 3px; padding-bottom: 3px }"
            "th { font-weight: 200 }"
            ".col { flex: 50%% }"
            ".row { display: flex }"
            "</style></head>");
    fprintf(f, "<body>");

    html_var_analysis(results, f);
    html_compare(results, f);
    for (size_t bench_idx = 0; bench_idx < results->bench_count; ++bench_idx) {
        const struct bench_analysis *analysis =
            results->bench_analyses + bench_idx;
        fprintf(f, "<div><h2>command '%s'</h2>", analysis->name);
        for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
            const struct meas *info = results->meas + meas_idx;
            if (info->is_secondary)
                continue;
            html_distr(analysis, bench_idx, meas_idx, results, f);
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</body>");
}

static bool run_bench(const struct bench_params *params,
                      struct bench_analysis *analysis) {
    if (!warmup(params))
        return false;
    if (!run_benchmark(params, analysis->bench))
        return false;
    analyze_benchmark(analysis, params->meas_count);
    return true;
}

static void *bench_runner_worker(void *raw) {
    struct bench_runner_thread_data *data = raw;
    g_rng_state = time(NULL) * 2 + 1;
    for (;;) {
        size_t task_idx = atomic_fetch_inc(data->cursor);
        if (task_idx >= data->max)
            break;

        size_t bench_idx = data->indexes[task_idx];
        if (!run_bench(data->params + bench_idx, data->analyses + bench_idx))
            return (void *)-1;
    }
    return NULL;
}

static void redraw_progress_bar(struct progress_bar *bar) {
    bool abbr_names = bar->max_name_len > 40;
    int length = 40;
    if (!bar->was_drawn) {
        bar->was_drawn = true;
        if (abbr_names) {
            for (size_t i = 0; i < bar->count; ++i) {
                printf("%c = ", (int)('A' + i));
                printf_colored(ANSI_BOLD, "%s\n", bar->analyses[i].name);
            }
        }
    } else {
        printf("\x1b[%zuA\r", bar->count);
    }

    double current_time = get_time();
    for (size_t i = 0; i < bar->count; ++i) {
        struct progress_bar_bench data = {0};
        // explicitly load all atomics to avoid UB (tsan)
        atomic_fence();
        data.bar = atomic_load(&bar->benches[i].bar);
        data.finished = atomic_load(&bar->benches[i].finished);
        data.aborted = atomic_load(&bar->benches[i].aborted);
        data.metric.u = atomic_load(&bar->benches[i].metric.u);
        data.start_time.u = atomic_load(&bar->benches[i].start_time.u);
        if (abbr_names)
            printf("%c ", (int)('A' + i));
        else
            printf_colored(ANSI_BOLD, "%*s ", (int)bar->max_name_len,
                           bar->analyses[i].name);
        char buf[41] = {0};
        int c = data.bar * length / 100;
        if (c > length)
            c = length;
        for (int j = 0; j < c; ++j)
            buf[j] = '#';
        printf_colored(ANSI_BRIGHT_BLUE, "%s", buf);
        for (int j = 0; j < 40 - c; ++j)
            buf[j] = '-';
        buf[40 - c] = '\0';
        printf_colored(ANSI_BLUE, "%s", buf);
        if (data.aborted) {
            memcpy(&data.id, &bar->benches[i].id, sizeof(data.id));
            for (size_t i = 0; i < sb_len(g_output_anchors); ++i) {
                if (pthread_equal(g_output_anchors[i].id, data.id)) {
                    assert(g_output_anchors[i].has_message);
                    printf_colored(ANSI_RED, " error: ");
                    printf("%s", g_output_anchors[i].buffer);
                    break;
                }
            }
        } else {
            if (g_bench_stop.runs != 0) {
                char eta_buf[256] = "N/A";
                if (data.start_time.d != 0.0) {
                    double passed_time = current_time - data.start_time.d;
                    if (bar->states[i].runs != data.metric.u) {
                        bar->states[i].eta =
                            (g_bench_stop.runs - data.metric.u) * passed_time /
                            data.metric.u;
                        bar->states[i].runs = data.metric.u;
                        bar->states[i].time = current_time;
                    }
                    double eta = bar->states[i].eta;
                    if (!data.finished)
                        eta -= current_time - bar->states[i].time;
                    // Sometimes we would get -inf here
                    if (eta < 0.0)
                        eta = -eta;
                    if (eta != INFINITY)
                        format_time(eta_buf, sizeof(eta_buf), eta);
                }
                char total_buf[256];
                snprintf(total_buf, sizeof(total_buf), "%zu",
                         (size_t)g_bench_stop.runs);
                printf(" %*zu/%s eta %s", (int)strlen(total_buf),
                       (size_t)data.metric.u, total_buf, eta_buf);
            } else {
                char buf1[256], buf2[256];
                format_time(buf1, sizeof(buf1), data.metric.d);
                format_time(buf2, sizeof(buf2), g_bench_stop.time_limit);
                printf(" %s/ %s", buf1, buf2);
            }
        }
        printf("\n");
    }
}

void *progress_bar_thread_worker(void *arg) {
    assert(g_progress_bar);
    struct progress_bar *bar = arg;
    bool is_finished = false;
    redraw_progress_bar(bar);
    do {
        usleep(100000);
        redraw_progress_bar(bar);
        is_finished = true;
        for (size_t i = 0; i < bar->count && is_finished; ++i)
            if (!atomic_load(&bar->benches[i].finished))
                is_finished = false;
    } while (!is_finished);
    redraw_progress_bar(bar);
    return NULL;
}

static void init_progress_bar(struct bench_analysis *analyses, size_t count,
                              struct progress_bar *bar) {
    bar->count = count;
    bar->benches = calloc(count, sizeof(*bar->benches));
    bar->states = calloc(count, sizeof(*bar->states));
    bar->analyses = analyses;
    for (size_t i = 0; i < count; ++i) {
        bar->states[i].runs = -1;
        analyses[i].bench->progress = bar->benches + i;
        size_t name_len = strlen(analyses[i].name);
        if (name_len > bar->max_name_len)
            bar->max_name_len = name_len;
    }
}

static void free_progress_bar(struct progress_bar *bar) {
    free(bar->benches);
    free(bar->states);
}

// Execute benchmarks, possibly in parallel using worker threads.
// When parallel execution is used, thread pool is created, threads from which
// select a benchmark to run in random order. We shuffle the benchmarks here
// in orderd to get asymptotically OK runtime, as incorrect order of
// tasks in parallel execution can degrade performance (queueing theory).
//
// Parallel execution is controlled using 'g_threads' global
// variable.
//
// This function also optionally spawns the thread printing interactive progress
// bar. Logic conserning progress bar may be cumbersome:
// 1. A new thread is spawned, which wakes ones in a while and checks atomic
//  variables storing states of benchmarks
// 2. Each of benchmarks updates its state in corresponding atomic varibales
// 3. Output of benchmarks when progress bar is used is captured (anchored), see
//  'error' and 'csperror' functions. This is done in order to not corrupt the
//  output in case such message is printed.
static bool run_benches(const struct bench_params *params,
                        struct bench_analysis *analyses, size_t count) {
    bool success = false;
    struct progress_bar progress_bar = {0};
    pthread_t progress_bar_thread;
    if (g_progress_bar) {
        init_progress_bar(analyses, count, &progress_bar);
        if (pthread_create(&progress_bar_thread, NULL,
                           progress_bar_thread_worker, &progress_bar) != 0) {
            error("failed to spawn thread");
            free_progress_bar(&progress_bar);
            return false;
        }
    }

    // Consider the cases where there is either no point in execution benchmarks
    // in parallel, or settings explicitly forbid this.
    if (g_threads <= 1 || count == 1) {
        if (g_progress_bar) {
            sb_resize(g_output_anchors, 1);
            g_output_anchors[0].id = pthread_self();
        }
        for (size_t i = 0; i < count; ++i) {
            if (!run_bench(params + i, analyses + i)) {
                // In case of benchmark abort we have to explicitly tell
                // progress bar that all benchmarks have finished, otherwise it
                // will spin continiously waiting for it
                if (g_progress_bar)
                    for (size_t bench_idx = 0; bench_idx < count; ++bench_idx)
                        progress_bar.benches[bench_idx].finished = true;
                goto free_progress_bar;
            }
        }
        success = true;
        goto free_progress_bar;
    }

    size_t thread_count = g_threads;
    if (count < thread_count)
        thread_count = count;
    assert(thread_count > 1);
    struct bench_runner_thread_data *thread_data =
        calloc(thread_count, sizeof(*thread_data));

    size_t *task_indexes = calloc(count, sizeof(*task_indexes));
    for (size_t i = 0; i < count; ++i)
        task_indexes[i] = i;
    shuffle(task_indexes, count);

    // This variable is shared across threads and acts as a counter used to
    // select the task from 'task_indexes' array.
    size_t cursor = 0;
    for (size_t i = 0; i < thread_count; ++i) {
        thread_data[i].params = params;
        thread_data[i].analyses = analyses;
        thread_data[i].indexes = task_indexes;
        thread_data[i].cursor = &cursor;
        thread_data[i].max = count;
    }
    if (g_progress_bar)
        sb_resize(g_output_anchors, thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        // HACK: save thread id to output anchors first. If we do not do it here
        // we would need additional synchronization
        pthread_t *id = &thread_data[i].id;
        if (g_progress_bar)
            id = &g_output_anchors[i].id;
        if (pthread_create(id, NULL, bench_runner_worker, thread_data + i) !=
            0) {
            for (size_t j = 0; j < i; ++j)
                pthread_join(thread_data[j].id, NULL);
            error("failed to spawn thread");
            goto err;
        }
        thread_data[i].id = *id;
    }
    success = true;
    for (size_t i = 0; i < thread_count; ++i) {
        void *thread_retval;
        pthread_join(thread_data[i].id, &thread_retval);
        if (thread_retval == (void *)-1)
            success = false;
    }
err:
    free(thread_data);
    free(task_indexes);
free_progress_bar:
    if (g_progress_bar) {
        pthread_join(progress_bar_thread, NULL);
        free_progress_bar(&progress_bar);
    }
    struct output_anchor *anchors = g_output_anchors;
    g_output_anchors = NULL;
    sb_free(anchors);
    return success;
}

static void analyze_benches(const struct run_info *info,
                            struct bench_results *results) {
    size_t group_count = sb_len(info->groups);
    results->group_count = group_count;
    results->var_groups = info->groups;

    size_t meas_count = results->meas_count;
    size_t primary_meas_count = 0;
    for (size_t i = 0; i < meas_count; ++i)
        if (!results->meas[i].is_secondary)
            ++primary_meas_count;
    results->primary_meas_count = primary_meas_count;

    results->meas_analyses =
        calloc(meas_count, sizeof(*results->meas_analyses));
    for (size_t i = 0; i < meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;
        results->meas_analyses[i].meas = results->meas + i;
        results->meas_analyses[i].base = results;
        results->meas_analyses[i].fastest = calloc(
            results->bench_count, sizeof(*results->meas_analyses[i].fastest));
        results->meas_analyses[i].benches = calloc(
            results->bench_count, sizeof(*results->meas_analyses[i].benches));
        for (size_t j = 0; j < results->bench_count; ++j)
            results->meas_analyses[i].benches[j] =
                results->bench_analyses[j].meas + i;
        results->meas_analyses[i].group_analyses = calloc(
            group_count, sizeof(*results->meas_analyses[i].group_analyses));
        results->meas_analyses[i].speedup = calloc(
            results->bench_count, sizeof(*results->meas_analyses[i].speedup));
        results->meas_analyses[i].group_baseline_speedup =
            calloc(results->group_count,
                   sizeof(*results->meas_analyses[i].group_baseline_speedup));
        results->meas_analyses[i].p_values = calloc(
            results->bench_count, sizeof(*results->meas_analyses[i].p_values));
        if (results->var) {
            results->meas_analyses[i].fastest_val =
                calloc(results->var->value_count,
                       sizeof(*results->meas_analyses[i].fastest_val));
            results->meas_analyses[i].var_speedup =
                calloc(results->var->value_count,
                       sizeof(*results->meas_analyses[i].var_speedup));
            for (size_t val_idx = 0; val_idx < results->var->value_count;
                 ++val_idx) {
                results->meas_analyses[i].var_speedup[val_idx] =
                    calloc(results->group_count,
                           sizeof(**results->meas_analyses[i].var_speedup));
            }
            results->meas_analyses[i].var_p_values =
                calloc(results->var->value_count,
                       sizeof(*results->meas_analyses[i].var_p_values));
            for (size_t value_idx = 0; value_idx < results->var->value_count;
                 ++value_idx)
                results->meas_analyses[i].var_p_values[value_idx] =
                    calloc(results->group_count,
                           sizeof(**results->meas_analyses[i].var_p_values));
        }
    }

    compare_benches(results);
    for (size_t i = 0; i < meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;
        analyze_var_groups(results->meas_analyses + i);
        calculate_p_values(results->meas_analyses + i);
        calculate_speedups(results->meas_analyses + i);
    }
}

static void print_analysis(const struct bench_results *results) {
    if (g_bench_stop.runs != 0)
        printf("%d runs\n", g_bench_stop.runs);
    if (results->primary_meas_count == 1) {
        const struct meas *meas = NULL;
        for (size_t i = 0; i < results->meas_count && meas == NULL; ++i)
            if (!results->meas[i].is_secondary)
                meas = results->meas + i;
        assert(meas != NULL);
        printf("measurement ");
        printf_colored(ANSI_YELLOW, "%s\n", meas->name);
    }
    for (size_t i = 0; i < results->bench_count; ++i)
        print_benchmark_info(results->bench_analyses + i, results);


    for (size_t i = 0; i < results->meas_count; ++i) {
        const struct meas *meas = results->meas + i;
        if (meas->is_secondary)
            continue;
        if (results->primary_meas_count != 1) {
            printf("measurement ");
            printf_colored(ANSI_YELLOW, "%s\n", meas->name);
        }
        print_cmd_comparison(results->meas_analyses + i);
    }
}

static bool do_export(const struct bench_results *results) {
    switch (g_export.kind) {
    case EXPORT_JSON:
        return export_json(results, g_export.filename);
    case DONT_EXPORT:
        break;
    }
    return true;
}

static bool make_html_report(const struct bench_results *results) {
    FILE *f = open_file_fmt("w", "%s/index.html", g_out_dir);
    if (f == NULL) {
        error("failed to create file %s/index.html", g_out_dir);
        return false;
    }
    html_report(results, f);
    fclose(f);
    return true;
}

static bool do_visualize(const struct bench_results *results) {
    if (!g_plot && !g_html && !g_csv)
        return true;

    if (mkdir(g_out_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            csperror("mkdir");
            return false;
        }
    }

    if (g_plot) {
        if (!python_found()) {
            error("failed to find python3 executable");
            return false;
        }
        if (!python_has_matplotlib()) {
            error("python does not have matplotlib installed");
            return false;
        }

        if (g_plot_src && !dump_plot_src(results))
            return false;
        if (!make_plots(results))
            return false;
        if (!make_plots_readme(results))
            return false;
    }

    if (g_csv && !export_csvs(results))
        return false;

    if (g_html && !make_html_report(results))
        return false;

    return true;
}

static void free_bench_meas_analysis(struct bench_meas_analysis *analysis) {
    free(analysis->fastest);
    free(analysis->fastest_val);
    free(analysis->p_values);
    free(analysis->speedup);
    free(analysis->group_baseline_speedup);
    if (analysis->var_p_values) {
        for (size_t i = 0; i < analysis->base->var->value_count; ++i)
            free(analysis->var_p_values[i]);
        free(analysis->var_p_values);
    }
    if (analysis->group_analyses) {
        for (size_t i = 0; i < analysis->base->group_count; ++i)
            free(analysis->group_analyses[i].data);
        free(analysis->group_analyses);
    }
    if (analysis->var_speedup) {
        for (size_t i = 0; i < analysis->base->var->value_count; ++i)
            free(analysis->var_speedup[i]);
        free(analysis->var_speedup);
    }
}

static void free_bench_results(struct bench_results *results) {
    // these ifs are needed because results can be partially initialized in
    // case of failure
    if (results->benches) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            struct bench *bench = results->benches + i;
            sb_free(bench->exit_codes);
            for (size_t i = 0; i < results->meas_count; ++i)
                sb_free(bench->meas[i]);
            free(bench->meas);
        }
        free(results->benches);
    }
    if (results->bench_analyses) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            const struct bench_analysis *analysis = results->bench_analyses + i;
            free(analysis->meas);
        }
        free(results->bench_analyses);
    }
    if (results->meas_analyses) {
        for (size_t i = 0; i < results->meas_count; ++i)
            free_bench_meas_analysis(results->meas_analyses + i);
        free(results->meas_analyses);
    }
}

static bool make_report(const struct bench_results *results) {
    print_analysis(results);
    if (!do_export(results))
        return false;
    if (!do_visualize(results))
        return false;
    return true;
}

static void init_bench_results(const struct meas *meas_list, size_t bench_count,
                               const struct bench_var *var,
                               struct bench_results *results) {
    results->meas = meas_list;
    results->meas_count = sb_len(meas_list);
    results->bench_count = bench_count;
    results->benches = calloc(bench_count, sizeof(*results->benches));
    results->bench_analyses =
        calloc(bench_count, sizeof(*results->bench_analyses));
    results->var = var;
    for (size_t i = 0; i < results->bench_count; ++i) {
        struct bench *bench = results->benches + i;
        struct bench_analysis *analysis = results->bench_analyses + i;
        bench->meas = calloc(results->meas_count, sizeof(*bench->meas));
        analysis->meas = calloc(results->meas_count, sizeof(*analysis->meas));
        analysis->bench = bench;
    }
}

static bool check_rename_list(const struct rename_entry *rename_list,
                              size_t bench_count,
                              const struct bench_var_group *groups) {
    if (sb_len(groups) == 0) {
        for (size_t i = 0; i < sb_len(rename_list); ++i) {
            if (rename_list[i].n >= bench_count) {
                error("number (%zu) of benchmark to be renamed ('%s') is too "
                      "high",
                      rename_list[i].n + 1, rename_list[i].name);
                return false;
            }
        }
    } else {
        for (size_t i = 0; i < sb_len(rename_list); ++i) {
            if (rename_list[i].n >= sb_len(groups)) {
                error("number (%zu) of benchmark to be renamed ('%s') is too "
                      "high",
                      rename_list[i].n + 1, rename_list[i].name);
                return false;
            }
        }
    }
    return true;
}

static bool attempt_rename(const struct rename_entry *rename_list, size_t idx,
                           struct bench_analysis *analysis) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == idx) {
            strlcpy(analysis->name, rename_list[i].name,
                    sizeof(analysis->name));
            return true;
        }
    }
    return false;
}

static bool attempt_groupped_rename(const struct rename_entry *rename_list,
                                    size_t bench_idx,
                                    const struct bench_var_group *groups,
                                    const struct bench_var *var,
                                    struct bench_analysis *analysis) {
    assert(var);
    assert(groups);
    size_t value_count = var->value_count;
    for (size_t grp_idx = 0; grp_idx < sb_len(groups); ++grp_idx) {
        const struct bench_var_group *grp = groups + grp_idx;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            if (grp->cmd_idxs[val_idx] == bench_idx) {
                for (size_t rename_idx = 0; rename_idx < sb_len(rename_list);
                     ++rename_idx) {
                    if (rename_list[rename_idx].n == grp_idx) {
                        snprintf(analysis->name, sizeof(analysis->name),
                                 "%s %s=%s", rename_list[rename_idx].name,
                                 var->name, var->values[val_idx]);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

static bool run_app_bench(const struct cli_settings *cli) {
    bool success = false;
    struct run_info info = {0};
    if (!init_run_info(cli, &info))
        return false;
    if (g_use_perf && !init_perf())
        goto err_free_run_info;
    size_t bench_count = sb_len(info.params);
    if (!check_rename_list(cli->rename_list, bench_count, info.groups))
        goto err_deinit_perf;
    struct bench_results results = {0};
    init_bench_results(cli->meas, bench_count, info.var, &results);
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct bench_params *params = info.params + bench_idx;
        struct bench_analysis *analysis = results.bench_analyses + bench_idx;
        bool renamed = false;
        if (sb_len(info.groups) == 0) {
            renamed = attempt_rename(cli->rename_list, bench_idx, analysis);
        } else {
            renamed = attempt_groupped_rename(cli->rename_list, bench_idx,
                                              info.groups, cli->var, analysis);
        }
        if (!renamed)
            strlcpy(analysis->name, params->str, sizeof(analysis->name));
    }
    if (!run_benches(info.params, results.bench_analyses, bench_count))
        goto err_free_results;
    analyze_benches(&info, &results);
    if (!make_report(&results))
        goto err_free_results;
    success = true;
err_free_results:
    free_bench_results(&results);
err_deinit_perf:
    if (g_use_perf)
        deinit_perf();
err_free_run_info:
    free_run_info(&info);
    return success;
}

static bool load_meas_names(const char *file, char ***meas_names) {
    bool success = false;
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return false;

    char *line_buffer = NULL;
    size_t n = 0;
    if (getline(&line_buffer, &n, f) < 0)
        goto out;

    char *end = NULL;
    (void)strtod(line_buffer, &end);
    if (end == line_buffer) {
        *meas_names = parse_comma_separated_list(line_buffer);
        success = true;
    } else {
        success = true;
    }

    free(line_buffer);
out:
    fclose(f);
    return success;
}

static bool load_bench_run_meas(const char *str, double **meas,
                                size_t meas_count) {
    size_t cursor = 0;
    while (cursor < meas_count) {
        char *end = NULL;
        double value = strtod(str, &end);
        if (end == str)
            return false;
        sb_push(meas[cursor], value);
        ++cursor;
        str = end;
        if (*str == '\n' || !*str)
            break;
        if (*str != ',')
            return false;
        ++str;
    }
    return cursor == meas_count ? true : false;
}

static bool load_bench_result(const char *file, struct bench *bench,
                              size_t meas_count) {
    bool success = false;
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return false;

    size_t n = 0;
    char *line_buffer = NULL;
    // Skip line with measurement names
    if (getline(&line_buffer, &n, f) < 0) {
        error("failed to parse file '%s'", file);
        goto out;
    }
    for (;;) {
        ssize_t read_result = getline(&line_buffer, &n, f);
        if (read_result < 0) {
            if (ferror(f)) {
                error("failed to read line from file '%s'", file);
                goto out;
            }
            break;
        }
        ++bench->run_count;
        if (!load_bench_run_meas(line_buffer, bench->meas, meas_count)) {
            error("failed to parse file '%s'", file);
            goto out;
        }
    }

    success = true;
out:
    fclose(f);
    free(line_buffer);
    return success;
}

static char **find_loada_filenames(void) {
    DIR *dir = opendir(g_out_dir);
    if (dir == NULL) {
        csperror("opendir");
        return NULL;
    }
    char **list = NULL;
    for (;;) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (dirent == NULL && errno != 0) {
            csperror("readdir");
            for (size_t i = 0; i < sb_len(list); ++i)
                free(list[i]);
            sb_free(list);
            list = NULL;
            break;
        } else if (dirent == NULL) {
            break;
        }
        const char *name = dirent->d_name;
        const char *prefix = "bench_raw_";
        size_t prefix_len = strlen(prefix);
        if (strncmp(prefix, name, prefix_len) == 0) {
            name += prefix_len;
            if (!isdigit(*name))
                continue;
            size_t n = 0;
            do {
                n = n * 10 + (*name - '0');
                ++name;
            } while (isdigit(*name));
            if (strcmp(name, ".csv") == 0) {
                char buffer[4096];
                snprintf(buffer, sizeof(buffer), "%s/%s", g_out_dir,
                         dirent->d_name);
                sb_ensure(list, n + 1);
                list[n] = strdup(buffer);
            }
        }
    }
    closedir(dir);
    return list;
}

static bool run_app_load(const struct cli_settings *settings) {
    bool result = false;
    const char **file_list = settings->args;
    struct meas *meas_list = NULL;
    struct bench_results results = {0};
    if (g_loada) {
        file_list = (const char **)find_loada_filenames();
        if (!file_list) {
            error("failed to find files as required for --loada argument");
            return false;
        }
    }

    for (size_t file_idx = 0; file_idx < sb_len(file_list); ++file_idx) {
        const char *file = file_list[file_idx];
        char **file_meas_names = NULL;
        if (!load_meas_names(file, &file_meas_names)) {
            error("failed to load measurement names from file '%s'", file);
            goto err;
        }
        if (file_idx != 0) {
            if (sb_len(file_meas_names) != sb_len(meas_list)) {
                error("measurement number in different files does not match "
                      "(current file '%s')",
                      file);
                sb_free(file_meas_names);
                goto err;
            }
            continue;
        }

        for (size_t meas_idx = 0; meas_idx < sb_len(file_meas_names);
             ++meas_idx) {
            char *file_meas = file_meas_names[meas_idx];
            // First check if measurement with same name exists
            bool is_found = false;
            for (size_t test_idx = 0; test_idx < sb_len(meas_list) && !is_found;
                 ++test_idx) {
                if (strcmp(file_meas, meas_list[test_idx].name) == 0)
                    is_found = true;
            }
            // We have to add new measurement
            if (is_found)
                continue;

            // Scan measurements from user input
            const struct meas *meas_from_settings = NULL;
            for (size_t test_idx = 0; test_idx < sb_len(settings->meas) &&
                                      meas_from_settings == NULL;
                 ++test_idx)
                if (strcmp(file_meas, settings->meas[test_idx].name) == 0)
                    meas_from_settings = settings->meas + test_idx;
            if (meas_from_settings) {
                sb_push(meas_list, *meas_from_settings);
                continue;
            }

            struct meas meas = {"", NULL, {MU_NONE, ""}, MEAS_LOADED, false, 0};
            strlcpy(meas.name, file_meas, sizeof(meas.name));
            // Try to guess and use seconds as measurement unit for first
            // measurement
            if (meas_idx == 0)
                meas.units.kind = MU_S;
            sb_push(meas_list, meas);
        }
        sb_free(file_meas_names);
    }

    size_t bench_count = sb_len(file_list);
    if (!check_rename_list(settings->rename_list, bench_count, NULL))
        goto err;

    size_t meas_count = sb_len(meas_list);
    init_bench_results(meas_list, bench_count, NULL, &results);
    for (size_t i = 0; i < results.bench_count; ++i) {
        struct bench *bench = results.benches + i;
        struct bench_analysis *analysis = results.bench_analyses + i;
        const char *file = file_list[i];
        if (!attempt_rename(settings->rename_list, i, analysis))
            strlcpy(analysis->name, file, sizeof(analysis->name));
        if (!load_bench_result(file, bench, meas_count))
            goto err;
        analyze_benchmark(analysis, meas_count);
    }
    struct run_info info = {0};
    analyze_benches(&info, &results);
    if (!make_report(&results))
        goto err;
    free_bench_results(&results);
    result = true;
err:
    free_bench_results(&results);
    if (file_list && file_list != settings->args) {
        for (size_t i = 0; i < sb_len(file_list); ++i)
            free((char *)file_list[i]);
        sb_free(file_list);
    }
    if (meas_list)
        sb_free(meas_list);
    return result;
}

static bool run(const struct cli_settings *cli) {
    switch (g_mode) {
    case APP_BENCH:
        return run_app_bench(cli);
    case APP_LOAD:
        return run_app_load(cli);
    }

    return false;
}

static void sigint_handler(int sig) {
    if (g_use_perf)
        perf_signal_cleanup();

    // Use default signal handler
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1)
        abort();
    raise(sig);
}

static void prepare(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigint_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1) {
        csperror("sigaction");
        exit(EXIT_FAILURE);
    }

    // --color=auto
    g_colored_output = isatty(STDOUT_FILENO) ? true : false;
    // --progress-bar=auto
    g_progress_bar = isatty(STDOUT_FILENO) ? true : false;

    g_rng_state = time(NULL) * 2 + 1;
}

int main(int argc, char **argv) {
    prepare();

    int rc = EXIT_FAILURE;
    struct cli_settings cli = {0};
    parse_cli_args(argc, argv, &cli);

    if (!run(&cli))
        goto err_free_cli;

    rc = EXIT_SUCCESS;
err_free_cli:
    deinit_perf();
    free_cli_settings(&cli);
    return rc;
}
