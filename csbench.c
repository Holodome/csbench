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
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

// Instruction to rename certain benchmark. 'n' refers to individual benchmark
// when variable is not used, otherwise it refers to benchmark group.
struct rename_entry {
    size_t n;
    const char *name;
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cli_settings {
    const char **args;
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

enum app_mode {
    APP_BENCH,
    APP_LOAD_CSV
};

struct command_info {
    const char *name;
    const char *cmd;
    struct input_policy input;
    enum output_kind output;
    size_t grp_idx;
    const char *grp_name;
};

__thread uint64_t g_rng_state;
static bool g_colored_output = false;
bool g_ignore_failure = false;
int g_threads = 1;
bool g_plot = false;
bool g_html = false;
bool g_csv = false;
bool g_plot_src = false;
int g_nresamp = 10000;
bool g_use_perf = false;
bool g_progress_bar = false;
bool g_regr = false;
bool g_python_output = false;
int g_baseline = -1;
static enum app_mode g_mode = APP_BENCH;
struct bench_stop_policy g_warmup_stop = {0.1, 0, 1, 10};
struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};
struct bench_stop_policy g_round_stop = {5.0, 0, 2, 0};
// XXX: Mark this as volatile because we rely that this variable is changed
// atomically when creating and destorying threads. Elements of this array could
// only be written by a single thread, and reads are synchronized, so the data
// itself does not need to be volatile.
struct output_anchor *volatile g_output_anchors = NULL;
const char *g_json_export_filename = NULL;
const char *g_out_dir = ".csbench";
static const char *g_shell = "/bin/sh";
static const char *g_common_argstring = NULL;
const char *g_prepare = NULL;
// XXX: This is hack to use short names for files found in directory specified
// with --inputd. When opening files and this variable is not null open it
// relative to this directory.
static const char *g_inputd = NULL;

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

void errorv(const char *fmt, va_list args) {
    pthread_t tid = pthread_self();
    for (size_t i = 0; i < sb_len(g_output_anchors); ++i) {
        // Implicitly discard all messages but the first. This should not be an
        // issue, as the only possible message is error, and it (at least it
        // should) is always a single one
        if (pthread_equal(tid, g_output_anchors[i].id) &&
            !atomic_load(&g_output_anchors[i].has_message)) {
            vsnprintf(g_output_anchors[i].buffer,
                      sizeof(g_output_anchors[i].buffer), fmt, args);
            atomic_fence();
            atomic_store(&g_output_anchors[i].has_message, true);
            va_end(args);
            return;
        }
    }
    fprintf_colored(stderr, ANSI_RED, "error: ");
    vfprintf(stderr, fmt, args);
    putc('\n', stderr);
}

void error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    errorv(fmt, args);
    va_end(args);
}

void csperror(const char *msg) {
    int err = errno;
    char errbuf[4096];
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, errbuf, sizeof(errbuf));
#else
    strerror_r(err, errbuf, sizeof(errbuf));
    err_msg = errbuf;
#endif
    error("%s: %s", msg, err_msg);
}

void csfmterror(const char *fmt, ...) {
    int err = errno;
    char errbuf[4096];
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, errbuf, sizeof(errbuf));
#else
    strerror_r(err, errbuf, sizeof(errbuf));
    err_msg = errbuf;
#endif

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    error("%s: %s", buf, err_msg);
}

static void print_help_and_exit(int rc) {
    printf( //
        "A command line benchmarking tool\n"
        "\n"
        "Usage: csbench [subcommand] [OPTIONS] <command>...\n"
        "\n"
        "Arguments:\n"
        "          If first argument is one of the following: 'load', then "
        "consider it a subcommand name. 'load' means not to run benchmarks and "
        "instead load raw benchmark results from csv files. Each file "
        "correspons to a single benchmark, and has the following structure: "
        "the first line optionally contains measurement names, all other lines "
        "contain double values corresponding to measurements.\n"
        "  <command>...\n"
        "          The command to benchmark. Can be a shell command line, like "
        "'ls $(pwd) && echo 1', or a direct executable invocation, like 'sleep "
        "0.5'. Former is not available when --shell none is specified. Can "
        "contain variables in the form 'sleep {n}', see --scan family of "
        "options. If multiple commands are given, their comparison will be "
        "performed.\n "
        "\n"
        "Options:\n");
    printf( //
        "  -T, --time-limit <t>\n"
        "          Run each benchmark for at least <t> seconds. Affected by "
        "--min-runs and --max-runs. Benchmark can be suspended during "
        "execution using round settings.\n"
        "  -R, --runs <n>\n"
        "          Perform exactly <n> benchmark runs of each command. With "
        "this option set options --time-limit, --min-runs and --max-runs are "
        "ignored.\n"
        "  --min-runs <n>\n"
        "          Run each benchmark at least <n> times. Can be used with "
        "--time-limit and --max-runs.\n"
        "  --max-runs <n>\n"
        "          Run each benchmark at most <n> times. Can be used with "
        "--time-limit and --min-runs.\n");
    printf( //
        "  -W, --warmup <t>\n"
        "          Perform warmup for at least <t> seconds without recording "
        "results before each round. Affected by --min-warmup-runs and "
        "--max-warmup-runs.\n"
        "  --warmup-runs <n>\n"
        "          Perform exactly <n> warmup runs. With this option set "
        "options --warmup, --min-warmup-runs and --max-warmup-runs are "
        "ignored.\n"
        "  --min-warmup-runs <n>\n"
        "          During warmup run command at least <n> times. Can be used "
        "with --warmup and --max-warmup-runs.\n"
        "  --max-warmup-runs <n>\n"
        "          During warmup run command at most <n> times. Can be used "
        "with --warmup and --min-warmup-runs.\n"
        "  --no-warmup\n"
        "          Disable warmup.\n");
    printf( //
        "  --round-time <t>\n"
        "          Benchmark will be run at last <t> seconds in a row. After "
        "that it will be suspended and other benchmark will be executed. "
        "Affected by --min-round-runs and --max-round-runs.\n"
        "  --round-runs <n>\n"
        "          In a single round perform exactly <n> runs. With this "
        "option set options --round-time, --min-round-runs and "
        "--max-round-runs are ignored.\n"
        "  --min-round-runs <n>\n"
        "          In a single round perform at least <n> runs. Can be used "
        "with --round-time and --max-round-time.\n"
        "  --max-round-runs <n>\n"
        "          In a single round perform at most <n> runs. Can be used "
        "with --round-time and --min-round-time.\n"
        "  --no-rounds\n"
        "          Do not split execution into rounds.\n");
    printf( //
        "  -P, --prepare <cmd>\n"
        "          Execute <cmd> in default shell before each benchmark run.\n"
        "  --nrs <n>\n"
        "          Specify number of resamples used in bootstrapping. Default "
        "value is 10000.\n"
        "  --common-args <s>\n"
        "          Append string <s> to each command. Useful when executing "
        "different versions of same executable with same flags.\n");
    printf( //
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
        "          Specify how each command should receive its input.\n"
        "  --inputs <str>\n"
        "          Use <str> as input for each benchmark (it is piped to "
        "stdin).\n"
        "  --custom <name>\n"
        "          Add custom measurement with <name>. Attempts to parse real "
        "number from each command's stdout and interprets it in seconds.\n"
        "  --custom-t <name> <cmd>\n"
        "          Add custom measurement with <name>. Pipes each commands "
        "stdout to <cmd> and tries to parse real value from its output and "
        "interprets it in seconds. This can be used to extract a number, for "
        "example, using grep. Alias for --custom-x <name> 's' <cmd>.\n");
    printf( //
        "  --custom-x <name> <units> <cmd>\n"
        "          Add custom measurement with <name>. Pipes each commands "
        "stdout to <cmd> and tries to parse real value from its output and "
        "interprets it in <units>. <units> can be one of the time units 's', "
        "'ms','us', 'ns', or memory units 'b', 'kb', 'mb', 'gb', in which case "
        "results will pretty printed. If <units> is 'none', no units are "
        "printed. Alternatively <units> can be any string.\n"
        "  --scan <i>/<n>/<m>[/<s>]\n"
        "          Add variable with name <i> running in range from <n> to <m> "
        "with step <s>. <s> is optional, default is 1. Can be used from "
        "command in the form '{<i>}'.\n"
        "  --scanl <i>/v[,...]\n"
        "          Add variable with name <i> running values from "
        "comma-separated list <v>.\n"
        "  -j, --jobs <n>\n"
        "          Execute benchmarks in parallel with <n> threads. Default "
        "option is to execute all benchmarks sequentially.\n"
        "  --json <f>\n"
        "          Export benchmark results without analysis as json.\n"
        "  -o, --out-dir <d>\n"
        "          Specify directory where plots, html report and other "
        "analysis results will be placed. Default is '.csbench' in current "
        "directory.\n");
    printf( //
        "  --plot\n"
        "          Generate plots. For each benchmark KDE is generated in two "
        "variants. For each variable (--scan and --scanl) variable values are "
        "plotted against mean time. Single violin plot is produced if multiple "
        "commands are specified. For each measurement (--custom and others) "
        "its own group of plots is generated. Also readme.md file is "
        "generated, which helps to decipher plot file names.\n"
        "  --plot-src\n"
        "          Next to each plot file place python script used to produce "
        "it. Can be used to quickly patch up plots for presentation.\n"
        "  --html\n"
        "          Generate html report. Implies --plot.\n"
        "  --no-default-meas\n"
        "          Exclude wall clock information from command line output, "
        "plots, html report. Commonly used with custom measurements (--custom "
        "and others) when wall clock information is excessive.\n"
        "  -i, --ignore-failure\n"
        "          Accept commands with non-zero exit code. Default behavior "
        "is to abort benchmarking.\n"
        "  --meas <opt>[,...]\n"
        "          List of 'struct rusage' fields or performance counters "
        "(PMC) to include to analysis. Default (if --no-default-meas is not "
        "specified) "
        "are cpu time (stime and utime). Possible rusage values are 'stime', "
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
    printf( //
        "  --python-output\n"
        "          Do not silence python output. Intended for evelopers, as "
        "users should not have to see python output as it should always work "
        "correctly.\n"
        "  --rename <n> <name>\n"
        "          Rename benchmark with number <n> to <name>. This name will "
        "be used in reports instead of default one, which is command "
        "name. \n"
        "   -s, --simple\n"
        "          Preset to run benchmarks in parallel for one second without "
        "warmup. Useful for quickly checking something.\n");
    printf( //
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

static bool parse_range_scan_settings(const char *settings, const char **namep,
                                      double *lowp, double *highp,
                                      double *stepp) {
    const char *cursor = settings;
    const char *settings_end = settings + strlen(settings);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return false;

    size_t name_len = i_end - cursor;
    const char *name = csmkstr(cursor, name_len);

    cursor = i_end + 1;
    char *n_end = strchr(cursor, '/');
    if (n_end == NULL)
        return false;

    char *low_str_end = NULL;
    double low = strtod(cursor, &low_str_end);
    if (low_str_end != n_end)
        return false;

    cursor = n_end + 1;
    char *m_end = strchr(cursor, '/');
    char *high_str_end = NULL;
    double high = strtod(cursor, &high_str_end);
    if (m_end == NULL ? 0 : high_str_end != m_end)
        return false;

    double step = 1.0;
    if (high_str_end != settings_end) {
        cursor = high_str_end + 1;
        char *step_str_end = NULL;
        step = strtod(cursor, &step_str_end);
        if (step_str_end != settings_end)
            return false;
    }

    *namep = name;
    *lowp = low;
    *highp = high;
    *stepp = step;
    return true;
}

static const char **range_to_var_value_list(double low, double high,
                                            double step) {
    assert(high > low);
    const char **result = NULL;
    for (double cursor = low; cursor <= high + 0.000001; cursor += step) {
        const char *str = csfmt("%g", cursor);
        sb_push(result, str);
    }
    return result;
}

static bool parse_comma_separated_settings(const char *str, const char **namep,
                                           const char **scan_listp) {
    const char *cursor = str;
    const char *str_end = str + strlen(str);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return false;

    size_t name_len = i_end - cursor;
    const char *name = csmkstr(cursor, name_len);

    cursor = i_end + 1;
    if (cursor == str_end)
        return false;

    const char *scan_list = cursor;

    *namep = name;
    *scan_listp = scan_list;
    return true;
}

static const char **parse_comma_separated_list(const char *str) {
    const char **value_list = NULL;
    const char *cursor = str;
    const char *end = str + strlen(str);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            const char *new_str = csstripend(cursor);
            sb_push(value_list, new_str);
            break;
        }
        size_t value_len = next - cursor;
        const char *value = csmkstr(cursor, value_len);
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
        units->str = str;
    }
}

static void parse_meas_list(const char *opts, enum meas_kind **meas_list) {
    const char **list = parse_comma_separated_list(opts);
    for (size_t i = 0; i < sb_len(list); ++i) {
        const char *opt = list[i];
        enum meas_kind kind;
        if (strcmp(opt, "wall") == 0) {
            kind = MEAS_WALL;
        } else if (strcmp(opt, "stime") == 0) {
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
    sb_free(list);
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

static int string_cmp(const void *ap, const void *bp) {
    const char *a = ap;
    const char *b = bp;
    return strcmp(a, b);
}

static bool get_input_files_from_dir(const char *dirname, const char ***files) {
    DIR *dir = opendir(dirname);
    if (dir == NULL) {
        csfmterror("failed to open directory '%s' (designated for input)",
                   dirname);
        return false;
    }

    *files = NULL;
    for (;;) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (dirent == NULL && errno != 0) {
            csperror("readdir");
            sb_free(*files);
            break;
        } else if (dirent == NULL) {
            break;
        }

        const char *name = dirent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        const char *path = csstrdup(name);
        sb_push(*files, path);
    }

    qsort(*files, sb_len(*files), sizeof(**files), string_cmp);

    closedir(dir);
    return true;
}

static bool opt_arg(char **argv, int *cursor, const char *opt,
                    const char **arg) {
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

static bool opt_double_nonneg(char **argv, int *cursorp, const char **opt_strs,
                              const char *name, double *valuep) {
    const char *str;
    const char *opt_str = NULL;
    for (;;) {
        opt_str = *opt_strs++;
        if (opt_str == NULL)
            return false;
        if (opt_arg(argv, cursorp, opt_str, &str))
            break;
    }
    char *str_end;
    double value = strtod(str, &str_end);
    if (str_end == str) {
        error("invalid %s argument", opt_str);
        exit(EXIT_FAILURE);
    }
    if (value < 0.0) {
        error("%s must be positive number or zero", name);
        exit(EXIT_FAILURE);
    }
    *valuep = value;
    return true;
}

static bool opt_int_pos(char **argv, int *cursorp, const char **opt_strs,
                        const char *name, int *valuep) {
    const char *str;
    const char *opt_str = NULL;
    for (;;) {
        opt_str = *opt_strs++;
        if (opt_str == NULL)
            return false;
        if (opt_arg(argv, cursorp, opt_str, &str))
            break;
    }
    char *str_end;
    long value = strtol(str, &str_end, 10);
    if (str_end == str) {
        error("invalid %s argument", opt_str);
        exit(EXIT_FAILURE);
    }
    if (value <= 0) {
        error("%s must be positive number", name);
        exit(EXIT_FAILURE);
    }
    *valuep = value;
    return true;
}

static bool opt_bool(char **argv, int *cursorp, const char *opt_str,
                     bool *valuep) {
    if (strcmp(argv[*cursorp], opt_str) == 0) {
        *valuep = true;
        ++(*cursorp);
        return true;
    }
    return false;
}

#define OPT_ARR(...)                                                           \
    (const char *[]) { __VA_ARGS__, NULL }

static void parse_cli_args(int argc, char **argv,
                           struct cli_settings *settings) {
    settings->baseline = -1;
    bool no_wall = false;
    struct meas *meas_list = NULL;
    enum meas_kind *rusage_opts = NULL;

    if (argc == 1)
        print_help_and_exit(EXIT_SUCCESS);

    int cursor = 1;
    const char *str;
    while (cursor < argc) {
        if (strcmp(argv[cursor], "--help") == 0 ||
            strcmp(argv[cursor], "-h") == 0) {
            print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(argv[cursor], "--version") == 0) {
            print_version_and_exit();
        } else if (opt_double_nonneg(argv, &cursor, OPT_ARR("--warmup", "-W"),
                                     "warmup time limit",
                                     &g_warmup_stop.time_limit)) {
        } else if (opt_double_nonneg(argv, &cursor,
                                     OPT_ARR("--time-limit", "-T"),
                                     "time limit", &g_bench_stop.time_limit)) {
        } else if (opt_double_nonneg(argv, &cursor, OPT_ARR("--round-time"),
                                     "round time limit",
                                     &g_round_stop.time_limit)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--warmup-runs"),
                               "warmup run count", &g_warmup_stop.runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--runs", "-R"),
                               "run count", &g_bench_stop.runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--round-runs"),
                               "round run count", &g_round_stop.runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--min-warmup-runs"),
                               "minimal warmup run count",
                               &g_warmup_stop.min_runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--min-runs"),
                               "minimal run count", &g_bench_stop.min_runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--min-round-runs"),
                               "minimal round run count",
                               &g_round_stop.min_runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--max-warmup-runs"),
                               "maximum warmup run count",
                               &g_warmup_stop.max_runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--max-runs"),
                               "maximum run count", &g_bench_stop.max_runs)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--max-round-runs"),
                               "maximum round run count",
                               &g_round_stop.max_runs)) {
        } else if (opt_arg(argv, &cursor, "--prepare", &g_prepare)) {
        } else if (opt_arg(argv, &cursor, "--common-args",
                           &g_common_argstring)) {
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--nrs"),
                               "resamples count", &g_nresamp)) {
        } else if (opt_arg(argv, &cursor, "--shell", &g_shell) ||
                   opt_arg(argv, &cursor, "-S", &g_shell)) {
            if (strcmp(g_shell, "none") == 0)
                g_shell = NULL;
        } else if (strcmp(argv[cursor], "-N") == 0) {
            ++cursor;
            g_shell = NULL;
        } else if (opt_arg(argv, &cursor, "--output", &str)) {
            if (strcmp(str, "null") == 0) {
                settings->output = OUTPUT_POLICY_NULL;
            } else if (strcmp(str, "inherit") == 0) {
                settings->output = OUTPUT_POLICY_INHERIT;
            } else {
                error("invalid --output option");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[cursor], "--no-input") == 0) {
            ++cursor;
            g_inputd = NULL;
            settings->input.kind = INPUT_POLICY_NULL;
        } else if (opt_arg(argv, &cursor, "--input", &str)) {
            g_inputd = NULL;
            settings->input.kind = INPUT_POLICY_FILE;
            settings->input.file = str;
        } else if (opt_arg(argv, &cursor, "--inputs", &str)) {
            settings->input.kind = INPUT_POLICY_STRING;
            settings->input.string = str;
        } else if (opt_arg(argv, &cursor, "--inputd", &str)) {
            // XXX: To reuse old code, --inputd is more like a macro to
            // --input '{file}' with --scanl file/... having list of files.
            const char **files;
            if (!get_input_files_from_dir(str, &files))
                exit(EXIT_FAILURE);

            settings->input.kind = INPUT_POLICY_FILE;
            settings->input.file = "{file}";

            struct bench_var *var = calloc(1, sizeof(*var));
            var->name = "file";
            var->values = files;
            var->value_count = sb_len(files);
            if (settings->var)
                free(settings->var);
            settings->var = var;
            g_inputd = str;
        } else if (opt_arg(argv, &cursor, "--custom", &str)) {
            struct meas meas;
            memset(&meas, 0, sizeof(meas));
            meas.name = str;
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
            meas.name = name;
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
            meas.name = name;
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
            entry->name = name;
        } else if (opt_arg(argv, &cursor, "--rename-all", &str)) {
            const char **list = parse_comma_separated_list(str);
            for (size_t i = 0; i < sb_len(list); ++i) {
                struct rename_entry *entry = sb_new(settings->rename_list);
                entry->n = i;
                entry->name = list[i];
            }
            sb_free(list);
        } else if (opt_arg(argv, &cursor, "--scan", &str)) {
            double low, high, step;
            const char *name;
            if (!parse_range_scan_settings(str, &name, &low, &high, &step)) {
                error("invalid --scan argument");
                exit(EXIT_FAILURE);
            }
            if (settings->var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            const char **value_list = range_to_var_value_list(low, high, step);
            struct bench_var *var = calloc(1, sizeof(*var));
            var->name = name;
            var->values = value_list;
            var->value_count = sb_len(value_list);
            if (settings->var)
                free(settings->var);
            settings->var = var;
        } else if (opt_arg(argv, &cursor, "--scanl", &str)) {
            const char *name, *scan_list;
            if (!parse_comma_separated_settings(str, &name, &scan_list)) {
                error("invalid --scanl argument");
                exit(EXIT_FAILURE);
            }
            if (settings->var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            const char **value_list = parse_comma_separated_list(scan_list);
            struct bench_var *var = calloc(1, sizeof(*var));
            var->name = name;
            var->values = value_list;
            var->value_count = sb_len(value_list);
            if (settings->var)
                free(settings->var);
            settings->var = var;
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--jobs", "-j"),
                               "job count", &g_threads)) {
        } else if (opt_arg(argv, &cursor, "--json", &g_json_export_filename)) {
        } else if (opt_arg(argv, &cursor, "--out-dir", &g_out_dir) ||
                   opt_arg(argv, &cursor, "-o", &g_out_dir)) {
        } else if (opt_bool(argv, &cursor, "--html", &g_html)) {
            g_plot = true;
        } else if (opt_bool(argv, &cursor, "--plot", &g_plot)) {
        } else if (opt_bool(argv, &cursor, "--plot-src", &g_plot_src)) {
        } else if (opt_bool(argv, &cursor, "--no-default-meas", &no_wall)) {
        } else if (opt_bool(argv, &cursor, "--ignore-failure",
                            &g_ignore_failure) ||
                   opt_bool(argv, &cursor, "-i", &g_ignore_failure)) {
        } else if (opt_bool(argv, &cursor, "--csv", &g_csv)) {
        } else if (opt_bool(argv, &cursor, "--regr", &g_regr)) {
        } else if (opt_bool(argv, &cursor, "--python-output",
                            &g_python_output)) {
        } else if (strcmp(argv[cursor], "--no-warmup") == 0) {
            ++cursor;
            // XXX: This is kind of a hack, but whatever
            // Checked in `should_run`
            g_warmup_stop.time_limit = -1;
        } else if (strcmp(argv[cursor], "--no-rounds") == 0) {
            ++cursor;
            // XXX: This is kind of a hack, but whatever
            // Checked in `should_finish_running`
            g_round_stop.min_runs = INT_MAX;
        } else if (strcmp(argv[cursor], "--load-csv") == 0) {
            ++cursor;
            g_mode = APP_LOAD_CSV;
        } else if (strcmp(argv[cursor], "--simple") == 0 ||
                   strcmp(argv[cursor], "-s") == 0) {
            ++cursor;
            g_threads = simple_get_thread_count();
            g_warmup_stop.time_limit = 0.0;
            g_bench_stop.time_limit = 1.0;
        } else if (opt_arg(argv, &cursor, "--meas", &str)) {
            parse_meas_list(str, &rusage_opts);
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--baseline"),
                               "baseline number", &settings->baseline)) {
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

static bool extract_exec_and_argv(const char *cmd_str, const char **exec,
                                  const char ***argv) {
    char **words = split_shell_words(cmd_str);
    if (words == NULL) {
        error("invalid command syntax");
        return false;
    }
    *exec = csstrdup(words[0]);
    for (size_t i = 0; i < sb_len(words); ++i) {
        sb_push(*argv, csstrdup(words[i]));
        sb_free(words[i]);
    }
    sb_free(words);
    return true;
}

static bool init_cmd_exec(const char *shell, const char *cmd_str,
                          const char **exec, const char ***argv) {
    if (shell != NULL) {
        if (!extract_exec_and_argv(shell, exec, argv))
            return false;
        sb_push(*argv, "-c");
        sb_push(*argv, (char *)cmd_str);
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
        sb_free(params->argv);
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
        char buf[4096];
        const char *file = input->file;
        if (g_inputd) {
            snprintf(buf, sizeof(buf), "%s/%s", g_inputd, input->file);
            file = buf;
        }

        int fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
            csfmterror(
                "failed to open file '%s' (designated for benchmark input)",
                input->file);
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

static bool init_bench_params(const char *name,
                              const struct input_policy *input,
                              enum output_kind output, const struct meas *meas,
                              const char *exec, const char **argv,
                              const char *cmd_str,
                              struct bench_params *params) {
    params->name = name;
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
            grp->name = rename_list[i].name;
            return true;
        }
    }
    return false;
}

static bool init_command(const struct command_info *cmd, struct run_info *info,
                         size_t *idx) {
    const char *exec = NULL, **argv = NULL;
    if (!init_cmd_exec(g_shell, cmd->cmd, &exec, &argv))
        return false;

    struct bench_params bench_params = {0};
    if (!init_bench_params(cmd->name, &cmd->input, cmd->output, info->meas,
                           exec, argv, (char *)cmd->cmd, &bench_params)) {
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
        if (g_common_argstring) {
            cmd_str = csfmt("%s %s", cmd_str, g_common_argstring);
        }

        struct command_info info;
        memset(&info, 0, sizeof(info));
        info.name = info.cmd = cmd_str;
        info.output = cli->output;
        info.input = cli->input;
        info.grp_name = cmd_str;
        sb_push(*infos, info);
    }
    return true;
}

enum cmd_multiplex_result {
    CMD_MULTIPLEX_ERROR,
    CMD_MULTIPLEX_NO_GROUPS,
    CMD_MULTIPLEX_SUCCESS
};

static enum cmd_multiplex_result
multiplex_command_info_cmd(const struct command_info *src_info, size_t src_idx,
                           const struct bench_var *var,
                           struct command_info **multiplexed) {
    // Take first value and try to replace it in the command string
    char buf[4096];
    bool replaced = false;
    if (!replace_var_str(buf, sizeof(buf), src_info->cmd, var->name,
                         var->values[0], &replaced)) {
        error("Failed to substitute variable");
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    // We could reuse the string that is contained in buffer right now,
    // but it is a bit unecessary.
    for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
        const char *var_value = var->values[val_idx];
        if (!replace_var_str(buf, sizeof(buf), src_info->cmd, var->name,
                             var_value, &replaced)) {
            error("Failed to substitute variable");
            return CMD_MULTIPLEX_ERROR;
        }
        assert(replaced);
        struct command_info info;
        memcpy(&info, src_info, sizeof(info));
        info.name = info.cmd = csstrdup(buf);
        info.grp_idx = src_idx;
        info.grp_name = src_info->grp_name;
        sb_push(*multiplexed, info);
    }
    return CMD_MULTIPLEX_SUCCESS;
}

static enum cmd_multiplex_result
multiplex_command_info_input(const struct command_info *src_info,
                             size_t src_idx, const struct bench_var *var,
                             struct command_info **multiplexed) {
    if (src_info->input.kind != INPUT_POLICY_FILE &&
        src_info->input.kind != INPUT_POLICY_STRING)
        return CMD_MULTIPLEX_NO_GROUPS;

    const char *src_string;
    if (src_info->input.kind == INPUT_POLICY_FILE)
        src_string = src_info->input.file;
    else if (src_info->input.kind == INPUT_POLICY_STRING)
        src_string = src_info->input.string;

    char buf[4096];
    bool replaced = false;
    if (!replace_var_str(buf, sizeof(buf), src_string, var->name,
                         var->values[0], &replaced)) {
        error("Failed to substitute variable");
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
        const char *var_value = var->values[val_idx];
        if (!replace_var_str(buf, sizeof(buf), src_string, var->name, var_value,
                             &replaced)) {
            error("Failed to substitute variable");
            return CMD_MULTIPLEX_ERROR;
        }
        assert(replaced);
        struct command_info info;
        memcpy(&info, src_info, sizeof(info));
        info.cmd = src_info->cmd;
        info.grp_idx = src_idx;
        info.grp_name = src_info->grp_name;
        if (src_info->input.kind == INPUT_POLICY_FILE) {
            info.input.file = csstrdup(buf);
            snprintf(buf, sizeof(buf), "%s < %s", info.cmd, info.input.file);
            info.name = csstrdup(buf);
        } else if (src_info->input.kind == INPUT_POLICY_STRING) {
            info.input.string = csstrdup(buf);
            snprintf(buf, sizeof(buf), "%s <<< \"%s\"", info.cmd,
                     info.input.string);
            info.name = csstrdup(buf);
        }
        sb_push(*multiplexed, info);
    }
    return CMD_MULTIPLEX_SUCCESS;
}

static enum cmd_multiplex_result
multiplex_command_infos(const struct bench_var *var,
                        struct command_info **infos) {
    int ret = CMD_MULTIPLEX_NO_GROUPS;
    struct command_info *multiplexed = NULL;
    for (size_t src_idx = 0; src_idx < sb_len(*infos); ++src_idx) {
        const struct command_info *src_info = *infos + src_idx;

        ret = multiplex_command_info_cmd(src_info, src_idx, var, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        ret =
            multiplex_command_info_input(src_info, src_idx, var, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        error("command '%s' does not contain variable substitutions",
              src_info->cmd);
        goto err;
    }

    sb_free(*infos);
    *infos = multiplexed;
    return CMD_MULTIPLEX_SUCCESS;
err:
    sb_free(multiplexed);
    return CMD_MULTIPLEX_ERROR;
}

static bool init_benches(const struct cli_settings *cli,
                         const struct command_info *cmd_infos, bool has_groups,
                         struct run_info *info) {
    if (!has_groups) {
        for (size_t cmd_idx = 0; cmd_idx < sb_len(cmd_infos); ++cmd_idx) {
            const struct command_info *cmd = cmd_infos + cmd_idx;
            if (!init_command(cmd, info, NULL))
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
            group.name = cmd_cursor->grp_name;
        group.cmd_idxs = calloc(var->value_count, sizeof(*group.cmd_idxs));
        for (size_t val_idx = 0; val_idx < var->value_count;
             ++val_idx, ++cmd_cursor) {
            assert(cmd_cursor->grp_idx == grp_idx);
            if (!init_command(cmd_cursor, info, group.cmd_idxs + val_idx)) {
                sb_free(group.cmd_idxs);
                return false;
            }
        }
        sb_push(info->groups, group);
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
    struct bench_var *var = cli->var;
    if (var != NULL) {
        int ret = multiplex_command_infos(var, &command_infos);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        case CMD_MULTIPLEX_SUCCESS:
            has_groups = true;
        }
    }

    if (!init_benches(cli, command_infos, has_groups, info))
        goto err;

    result = true;
err:
    sb_free(command_infos);
    return result;
}

static bool validate_and_set_baseline(int baseline,
                                      const struct run_info *info) {
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
    if (!validate_and_set_baseline(cli->baseline, info))
        goto err;

    return true;
err:
    free_run_info(info);
    return false;
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
                           const char **name) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == idx) {
            *name = rename_list[i].name;
            return true;
        }
    }
    return false;
}

static bool attempt_groupped_rename(const struct rename_entry *rename_list,
                                    size_t bench_idx,
                                    const struct bench_var_group *groups,
                                    const struct bench_var *var,
                                    const char **name) {
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
                *name = csfmt("%s %s=%s", rename_list[rename_idx].name,
                              var->name, var->values[val_idx]);
                return true;
            }
        }
    }
    return false;
}

static void set_benchmark_names(const struct cli_settings *cli,
                                const struct run_info *info,
                                struct bench *benches) {
    for (size_t bench_idx = 0; bench_idx < sb_len(info->params); ++bench_idx) {
        const struct bench_params *params = info->params + bench_idx;
        struct bench *bench = benches + bench_idx;
        bool renamed = false;
        if (sb_len(info->groups) == 0) {
            renamed = attempt_rename(cli->rename_list, bench_idx, &bench->name);
        } else {
            renamed =
                attempt_groupped_rename(cli->rename_list, bench_idx,
                                        info->groups, cli->var, &bench->name);
        }
        if (!renamed)
            bench->name = params->name;
        assert(bench->name);
    }
}

static void init_bench_data(size_t bench_count, const struct meas *meas,
                            size_t meas_count,
                            const struct bench_var_group *groups,
                            size_t group_count, const struct bench_var *var,
                            struct bench_data *data) {
    memset(data, 0, sizeof(*data));
    data->bench_count = bench_count;
    data->benches = calloc(bench_count, sizeof(*data->benches));
    data->meas_count = meas_count;
    data->meas = meas;
    data->group_count = group_count;
    data->groups = groups;
    data->var = var;
    for (size_t i = 0; i < bench_count; ++i) {
        struct bench *bench = data->benches + i;
        bench->meas_count = meas_count;
        bench->meas = calloc(meas_count, sizeof(*bench->meas));
    }
}

static void free_bench_data(struct bench_data *data) {
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        sb_free(bench->exit_codes);
        for (size_t i = 0; i < data->meas_count; ++i)
            sb_free(bench->meas[i]);
        free(bench->meas);
        sb_free(bench->stdout_offsets);
    }
    free(data->benches);
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
    struct bench_data data;
    init_bench_data(bench_count, cli->meas, sb_len(cli->meas), info.groups,
                    sb_len(info.groups), info.var, &data);
    set_benchmark_names(cli, &info, data.benches);
    if (!run_benches(info.params, data.benches, bench_count))
        goto err_free_bench_data;
    struct analysis al;
    init_analysis(&data, &al);
    if (!analyze_benches(&al))
        goto err_free_analysis;
    if (!make_report(&al))
        goto err_free_analysis;
    success = true;
err_free_analysis:
    free_analysis(&al);
err_free_bench_data:
    free_bench_data(&data);
err_deinit_perf:
    if (g_use_perf)
        deinit_perf();
err_free_run_info:
    free_run_info(&info);
    return success;
}

static bool load_meas_names(const char *file, const char ***meas_names) {
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

static bool load_meas_from_csv_file(const struct cli_settings *settings,
                                    const char *file, struct meas **meas_list) {
    bool result = false;
    const char **file_meas_names = NULL;
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
        const char *file_meas = file_meas_names[meas_idx];
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
        meas.name = file_meas;
        // Try to guess and use seconds as measurement unit for first
        // measurement
        if (meas_idx == 0)
            meas.units.kind = MU_S;
        sb_push(*meas_list, meas);
    }
    result = true;
out:
    sb_free(file_meas_names);
    return result;
}

static bool load_meas_from_csv(const struct cli_settings *settings,
                               const char **file_list,
                               struct meas **meas_list) {
    for (size_t file_idx = 0; file_idx < sb_len(file_list); ++file_idx) {
        const char *file = file_list[file_idx];
        if (!load_meas_from_csv_file(settings, file, meas_list))
            return false;
    }
    return true;
}

static bool run_app_load_csv(const struct cli_settings *settings) {
    bool result = false;
    const char **file_list = settings->args;
    struct meas *meas_list = NULL;
    if (!load_meas_from_csv(settings, file_list, &meas_list))
        goto err_free_file_list;

    size_t bench_count = sb_len(file_list);
    if (!validate_rename_list(settings->rename_list, bench_count, NULL))
        goto err_free_meas_list;

    struct bench_data data;
    init_bench_data(bench_count, meas_list, sb_len(meas_list), NULL, 0, NULL,
                    &data);
    for (size_t i = 0; i < bench_count; ++i) {
        struct bench *bench = data.benches + i;
        const char *file = file_list[i];
        if (!attempt_rename(settings->rename_list, i, &bench->name))
            bench->name = file;
    }
    if (!load_bench_data_from_csv(file_list, &data))
        goto err_free_bench_data;
    struct analysis al;
    init_analysis(&data, &al);
    if (!analyze_benches(&al))
        goto err_free_analysis;
    if (!make_report(&al))
        goto err_free_analysis;
    result = true;
err_free_bench_data:
    free_bench_data(&data);
err_free_analysis:
    free_analysis(&al);
err_free_meas_list:
    sb_free(meas_list);
err_free_file_list:
    if (file_list && file_list != settings->args)
        sb_free(file_list);
    return result;
}

static bool run(const struct cli_settings *cli) {
    switch (g_mode) {
    case APP_BENCH:
        return run_app_bench(cli);
    case APP_LOAD_CSV:
        return run_app_load_csv(cli);
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

    init_rng_state();
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
    cs_free_strings();
    return rc;
}
