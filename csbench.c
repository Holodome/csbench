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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

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

enum app_mode {
    APP_BENCH,
    APP_LOAD
};

struct command_info {
    char cmd[4096];
    struct input_policy input;
    enum output_kind output;
    size_t grp_idx;
    const char *grp_name;
};

__thread uint64_t g_rng_state;
static bool g_colored_output = false;
bool g_allow_nonzero = false;
double g_warmup_time = 0.1;
static int g_threads = 1;
bool g_plot = false;
bool g_html = false;
bool g_csv = false;
bool g_plot_src = false;
int g_nresamp = 100000;
bool g_use_perf = false;
bool g_progress_bar = false;
bool g_regr = false;
bool g_python_output = false;
static bool g_loada = false;
int g_baseline = -1;
static enum app_mode g_mode = APP_BENCH;
struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};
static struct output_anchor *g_output_anchors = NULL;
const char *g_json_export_filename = NULL;
const char *g_out_dir = ".csbench";
const char *g_prepare = NULL;

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
        "  --inputs <str>\n"
        "          Use <str> as input for each benchmark (it is piped to "
        "stdin).\n"
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
        "time (stime and utime). Possible rusage values are 'stime', "
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
        "warmup. Useful for quickly checking something.\n");
    printf("  --help\n"
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
        } else if (opt_arg(argv, &cursor, "--inputs", &str)) {
            settings->input.kind = INPUT_POLICY_STRING;
            settings->input.string = str;
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
            free(name);
            var->values = value_list;
            var->value_count = sb_len(value_list);
            if (settings->var)
                free(settings->var);
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
            free(name);
            var->values = value_list;
            var->value_count = sb_len(value_list);
            if (settings->var)
                free(settings->var);
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
            g_json_export_filename = str;
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
        assert(sb_len(var->values) == var->value_count);
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
    for (size_t i = 0; i < sb_len(words); ++i) {
        sb_push(*argv, strdup(words[i]));
        sb_free(words[i]);
    }
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
        if (params->stdout_fd != -1)
            close(params->stdout_fd);
        if (params->stdin_fd != -1)
            close(params->stdin_fd);
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

static bool init_bench_stdin(const struct input_policy *input,
                             struct bench_params *params) {
    switch (input->kind) {
    case INPUT_POLICY_NULL:
        params->stdin_fd = -1;
        break;
    case INPUT_POLICY_FILE: {
        int fd = open(input->file, O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
            csperror("open");
            return false;
        }
        params->stdin_fd = fd;
        break;
    }
    case INPUT_POLICY_STRING: {
        int fd = tmpfile_fd();
        if (fd == -1)
            return false;

        int len = strlen(input->string);
        int nw = write(fd, input->string, len);
        if (nw != len) {
            csperror("write");
            return false;
        }
        if (lseek(fd, 0, SEEK_SET) == -1) {
            csperror("lseek");
            return false;
        }
        params->stdin_fd = fd;
        break;
    }
    }
    return true;
}

static bool init_bench_params(const struct input_policy *input,
                              enum output_kind output, const struct meas *meas,
                              char *exec, char **argv, char *cmd_str,
                              struct bench_params *params) {
    params->output = output;
    params->meas = meas;
    params->meas_count = sb_len(meas);
    params->exec = exec;
    params->argv = argv;
    params->str = cmd_str;
    params->stdout_fd = -1;
    return init_bench_stdin(input, params);
}

static bool init_bench_stdout(struct bench_params *params) {
    int fd = tmpfile_fd();
    if (fd == -1)
        return false;
    params->stdout_fd = fd;
    return true;
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

static bool init_command(const char *shell, const struct command_info *cmd,
                         struct run_info *info, size_t *idx) {
    char *exec = NULL, **argv = NULL;
    if (!init_cmd_exec(shell, cmd->cmd, &exec, &argv)) {
        error("failed to initialize command '%s'", cmd->cmd);
        return false;
    }
    struct bench_params bench_params = {0};
    if (!init_bench_params(&cmd->input, cmd->output, info->meas, exec, argv,
                           strdup(cmd->cmd), &bench_params)) {
        free(exec);
        for (char **word = argv; *word; ++word)
            free(*word);
        sb_free(argv);
        return false;
    }
    sb_push(info->params, bench_params);
    if (idx)
        *idx = sb_len(info->params) - 1;
    return true;
}

static bool init_raw_command_infos(const struct cli_settings *cli,
                                   struct command_info **infos) {
    size_t cmd_count = sb_len(cli->args);
    if (cmd_count == 0) {
        error("no commands specified");
        return false;
    }
    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->args[i];
        struct command_info info;
        memset(&info, 0, sizeof(info));
        strlcpy(info.cmd, cmd_str, sizeof(info.cmd));
        info.output = cli->output;
        info.input = cli->input;
        info.grp_name = cmd_str;
        sb_push(*infos, info);
    }
    return true;
}

static bool multiplex_command_infos(const struct cli_settings *cli,
                                    struct command_info **infos,
                                    bool *has_groups) {
    *has_groups = false;
    if (cli->var == NULL)
        return true;

    const struct bench_var *var = cli->var;
    struct command_info *multiplexed = NULL;
    for (size_t src_idx = 0; src_idx < sb_len(*infos); ++src_idx) {
        const struct command_info *src_info = *infos + src_idx;
        for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
            const char *var_value = var->values[val_idx];
            bool replaced = false;
            char buf[4096];
            if (!replace_var_str(buf, sizeof(buf), src_info->cmd, var->name,
                                 var_value, &replaced) ||
                !replaced) {
                error("command string '%s' does not contain variable "
                      "substitutions",
                      buf);
                sb_free(multiplexed);
                return false;
            }
            struct command_info info;
            memcpy(&info, src_info, sizeof(info));
            strlcpy(info.cmd, buf, sizeof(info.cmd));
            info.grp_idx = src_idx;
            info.grp_name = src_info->grp_name;
            sb_push(multiplexed, info);
        }
    }

    sb_free(*infos);
    *infos = multiplexed;
    *has_groups = true;

    return true;
}

static bool init_benches(const struct cli_settings *cli,
                         const struct command_info *cmd_infos, bool has_groups,
                         struct run_info *info) {
    if (!has_groups) {
        for (size_t cmd_idx = 0; cmd_idx < sb_len(cmd_infos); ++cmd_idx) {
            const struct command_info *cmd = cmd_infos + cmd_idx;
            if (!init_command(cli->shell, cmd, info, NULL))
                return false;
        }
        return true;
    }

    size_t group_count = sb_last(cmd_infos).grp_idx + 1;
    const struct bench_var *var = cli->var;
    const struct command_info *cmd_cursor = cmd_infos;
    for (size_t grp_idx = 0; grp_idx < group_count; ++grp_idx) {
        assert(cmd_cursor->grp_idx == grp_idx);

        struct bench_var_group group;
        memset(&group, 0, sizeof(group));
        if (!attempt_group_rename(cli->rename_list, sb_len(info->groups),
                                  &group))
            strlcpy(group.name, cmd_cursor->grp_name, sizeof(group.name));
        group.cmd_idxs = calloc(var->value_count, sizeof(*group.cmd_idxs));

        for (size_t val_idx = 0; val_idx < var->value_count;
             ++val_idx, ++cmd_cursor) {
            assert(cmd_cursor->grp_idx == grp_idx);
            if (!init_command(cli->shell, cmd_cursor, info,
                              group.cmd_idxs + val_idx)) {
                sb_free(group.cmd_idxs);
                return false;
            }
        }

        sb_push(info->groups, group);
    }

    return true;
}

static bool validate_input_policy(const struct input_policy *policy) {
    if (policy->kind == INPUT_POLICY_FILE) {
        if (access(policy->file, R_OK) == -1) {
            error("failed to open file '%s' (specified for input)",
                  policy->file);
            return false;
        }
    }
    return true;
}

static bool validate_set_baseline(int baseline, const struct run_info *info) {
    if (baseline > 0) {
        // Adjust number from human-readable to indexable
        size_t b = baseline - 1;
        size_t grp_count = sb_len(info->groups);
        size_t cmd_count = sb_len(info->params);
        if (grp_count <= 1) {
            // No parameterized benchmarks specified, just select the
            // command
            if (b >= cmd_count) {
                error("baseline number is too big");
                return false;
            }
        } else {
            // Multiple parameterized benchmarks
            if (b >= grp_count) {
                error("baseline number is too big");
                return false;
            }
        }
        g_baseline = b;
    }
    return true;
}

static bool init_commands(const struct cli_settings *cli,
                          struct run_info *info) {
    bool result = false;
    struct command_info *command_infos = NULL;
    if (!init_raw_command_infos(cli, &command_infos))
        return false;

    bool has_groups = false;
    if (!multiplex_command_infos(cli, &command_infos, &has_groups))
        goto err;

    if (!init_benches(cli, command_infos, has_groups, info))
        goto err;

    result = true;
err:
    sb_free(command_infos);
    return result;
}

static bool init_run_info(const struct cli_settings *cli,
                          struct run_info *info) {
    info->meas = cli->meas;
    info->var = cli->var;
    // Silently disable progress bar if output is inherit. The reasoning for
    // this is that inheriting output should only be used when debugging,
    // and user will not care about not having progress bar
    if (cli->output == OUTPUT_POLICY_INHERIT) {
        g_progress_bar = false;
    }

    if (sb_len(cli->meas) == 0) {
        error("no measurements specified");
        return false;
    }

    if (!init_commands(cli, info))
        return false;

    if (!validate_input_policy(&cli->input))
        return false;

    bool has_custom_meas = false;
    for (size_t i = 0; i < sb_len(cli->meas); ++i) {
        if (cli->meas[i].kind == MEAS_CUSTOM) {
            has_custom_meas = true;
            break;
        }
    }
    if (has_custom_meas) {
        for (size_t i = 0; i < sb_len(info->params); ++i) {
            if (!init_bench_stdout(info->params + i))
                goto err;
        }
    }

    // Validate that baseline number (if specified) is not greater than
    // command count
    if (!validate_set_baseline(cli->baseline, info))
        goto err;

    return true;
err:
    free_run_info(info);
    return false;
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

static void init_progress_bar(struct bench_analysis *als, size_t count,
                              struct progress_bar *bar) {
    bar->count = count;
    bar->benches = calloc(count, sizeof(*bar->benches));
    bar->states = calloc(count, sizeof(*bar->states));
    bar->analyses = als;
    for (size_t i = 0; i < count; ++i) {
        bar->states[i].runs = -1;
        als[i].bench->progress = bar->benches + i;
        size_t name_len = strlen(als[i].name);
        if (name_len > bar->max_name_len)
            bar->max_name_len = name_len;
    }
}

static void free_progress_bar(struct progress_bar *bar) {
    free(bar->benches);
    free(bar->states);
}

// Execute benchmarks, possibly in parallel using worker threads.
// When parallel execution is used, thread pool is created, threads from
// which select a benchmark to run in random order. We shuffle the
// benchmarks here in orderd to get asymptotically OK runtime, as incorrect
// order of tasks in parallel execution can degrade performance (queueing
// theory).
//
// Parallel execution is controlled using 'g_threads' global
// variable.
//
// This function also optionally spawns the thread printing interactive
// progress bar. Logic conserning progress bar may be cumbersome:
// 1. A new thread is spawned, which wakes ones in a while and checks atomic
//  variables storing states of benchmarks
// 2. Each of benchmarks updates its state in corresponding atomic varibales
// 3. Output of benchmarks when progress bar is used is captured (anchored),
// see
//  'error' and 'csperror' functions. This is done in order to not corrupt
//  the output in case such message is printed.
static bool run_benches(const struct bench_params *params,
                        struct bench_analysis *als, size_t count) {
    bool success = false;
    struct progress_bar progress_bar = {0};
    pthread_t progress_bar_thread;
    if (g_progress_bar) {
        init_progress_bar(als, count, &progress_bar);
        if (pthread_create(&progress_bar_thread, NULL,
                           progress_bar_thread_worker, &progress_bar) != 0) {
            error("failed to spawn thread");
            free_progress_bar(&progress_bar);
            return false;
        }
    }

    // Consider the cases where there is either no point in execution
    // benchmarks in parallel, or settings explicitly forbid this.
    if (g_threads <= 1 || count == 1) {
        if (g_progress_bar) {
            sb_resize(g_output_anchors, 1);
            g_output_anchors[0].id = pthread_self();
        }
        for (size_t i = 0; i < count; ++i) {
            if (!run_bench(params + i, als + i)) {
                // In case of benchmark abort we have to explicitly tell
                // progress bar that all benchmarks have finished, otherwise
                // it will spin continiously waiting for it
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
        thread_data[i].analyses = als;
        thread_data[i].indexes = task_indexes;
        thread_data[i].cursor = &cursor;
        thread_data[i].max = count;
    }
    if (g_progress_bar)
        sb_resize(g_output_anchors, thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        // HACK: save thread id to output anchors first. If we do not do it
        // here we would need additional synchronization
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

static bool validate_rename_list(const struct rename_entry *rename_list,
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
                           struct bench_analysis *al) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == idx) {
            strlcpy(al->name, rename_list[i].name, sizeof(al->name));
            return true;
        }
    }
    return false;
}

static bool attempt_groupped_rename(const struct rename_entry *rename_list,
                                    size_t bench_idx,
                                    const struct bench_var_group *groups,
                                    const struct bench_var *var,
                                    struct bench_analysis *al) {
    assert(var);
    assert(groups);
    size_t value_count = var->value_count;
    for (size_t grp_idx = 0; grp_idx < sb_len(groups); ++grp_idx) {
        const struct bench_var_group *grp = groups + grp_idx;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            if (grp->cmd_idxs[val_idx] != bench_idx)
                continue;
            for (size_t rename_idx = 0; rename_idx < sb_len(rename_list);
                 ++rename_idx) {
                if (rename_list[rename_idx].n != grp_idx)
                    continue;
                snprintf(al->name, sizeof(al->name), "%s %s=%s",
                         rename_list[rename_idx].name, var->name,
                         var->values[val_idx]);
                return true;
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
    if (!validate_rename_list(cli->rename_list, bench_count, info.groups))
        goto err_deinit_perf;
    struct analysis al = {0};
    init_analysis(cli->meas, bench_count, info.var, &al);
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct bench_params *params = info.params + bench_idx;
        struct bench_analysis *analysis = al.bench_analyses + bench_idx;
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
    if (!run_benches(info.params, al.bench_analyses, bench_count))
        goto err_free_analysis;
    analyze_benches(&info, &al);
    if (!make_report(&al))
        goto err_free_analysis;
    success = true;
err_free_analysis:
    free_analysis(&al);
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

static bool load_measurements_from_file(const struct cli_settings *settings,
                                        const char *file,
                                        struct meas **meas_list) {
    bool result = false;
    char **file_meas_names = NULL;
    if (!load_meas_names(file, &file_meas_names)) {
        error("failed to load measurement names from file '%s'", file);
        goto out;
    }
    // Check if this is the first file. We only load measurements from the first
    // file, but all others should have the same measurement number.
    if (sb_len(*meas_list) != 0) {
        if (sb_len(file_meas_names) != sb_len(*meas_list)) {
            error("measurement number in different files does not match "
                  "(current file '%s'): %zu vs expected %zu",
                  file, sb_len(file_meas_names), sb_len(*meas_list));
            goto out;
        }
        result = true;
        goto out;
    }

    for (size_t meas_idx = 0; meas_idx < sb_len(file_meas_names); ++meas_idx) {
        char *file_meas = file_meas_names[meas_idx];
        // First check if measurement with same name exists
        bool is_found = false;
        for (size_t test_idx = 0; test_idx < sb_len(*meas_list) && !is_found;
             ++test_idx) {
            if (strcmp(file_meas, (*meas_list)[test_idx].name) == 0)
                is_found = true;
        }
        // We have to add new measurement
        if (is_found)
            continue;

        // Scan measurements from user input
        const struct meas *meas_from_settings = NULL;
        for (size_t test_idx = 0;
             test_idx < sb_len(settings->meas) && meas_from_settings == NULL;
             ++test_idx)
            if (strcmp(file_meas, settings->meas[test_idx].name) == 0)
                meas_from_settings = settings->meas + test_idx;
        if (meas_from_settings) {
            sb_push(*meas_list, *meas_from_settings);
            continue;
        }

        struct meas meas = {"", NULL, {MU_NONE, ""}, MEAS_LOADED, false, 0};
        strlcpy(meas.name, file_meas, sizeof(meas.name));
        // Try to guess and use seconds as measurement unit for first
        // measurement
        if (meas_idx == 0)
            meas.units.kind = MU_S;
        sb_push(*meas_list, meas);
    }
    result = true;
out:
    for (size_t i = 0; i < sb_len(file_meas_names); ++i)
        free(file_meas_names[i]);
    sb_free(file_meas_names);
    return result;
}

static bool load_measurements_from_files(const struct cli_settings *settings,
                                         const char **file_list,
                                         struct meas **meas_list) {
    for (size_t file_idx = 0; file_idx < sb_len(file_list); ++file_idx) {
        const char *file = file_list[file_idx];
        if (!load_measurements_from_file(settings, file, meas_list))
            return false;
    }

    return true;
}

static bool run_app_load(const struct cli_settings *settings) {
    bool result = false;
    const char **file_list = settings->args;
    if (g_loada) {
        file_list = (const char **)find_loada_filenames();
        if (!file_list) {
            error("failed to find files as required for --loada argument");
            return false;
        }
    }

    struct meas *meas_list = NULL;
    if (!load_measurements_from_files(settings, file_list, &meas_list))
        goto err;

    size_t bench_count = sb_len(file_list);
    if (!validate_rename_list(settings->rename_list, bench_count, NULL))
        goto err;

    size_t meas_count = sb_len(meas_list);
    struct analysis al = {0};
    init_analysis(meas_list, bench_count, NULL, &al);
    for (size_t i = 0; i < al.bench_count; ++i) {
        struct bench *bench = al.benches + i;
        struct bench_analysis *analysis = al.bench_analyses + i;
        const char *file = file_list[i];
        if (!attempt_rename(settings->rename_list, i, analysis))
            strlcpy(analysis->name, file, sizeof(analysis->name));
        if (!load_bench_result(file, bench, meas_count))
            goto err;
        analyze_benchmark(analysis, meas_count);
    }
    struct run_info info = {0};
    analyze_benches(&info, &al);
    if (!make_report(&al))
        goto err;
    result = true;
err:
    free_analysis(&al);
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

    if (run(&cli))
        rc = EXIT_SUCCESS;

    deinit_perf();
    free_cli_settings(&cli);
    return rc;
}
