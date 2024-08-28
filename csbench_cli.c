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
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <unistd.h>

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

static void print_help_and_exit(int rc)
{
    printf( //
        "A commnd line benchmarking tool\n"
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

static void print_version_and_exit(void)
{
    printf("csbench 1.2\n");
    exit(EXIT_SUCCESS);
}

static bool parse_range_scan_settings(const char *settings, const char **namep,
                                      double *lowp, double *highp,
                                      double *stepp)
{
    const char *cursor = settings;
    const char *settings_end = settings + strlen(settings);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return false;

    size_t name_len = i_end - cursor;
    const char *name = csmkstr(cursor, name_len);

    cursor = i_end + 1;
    const char *n_end = strchr(cursor, '/');
    if (n_end == NULL)
        return false;

    char *low_str_end = NULL;
    double low = strtod(cursor, &low_str_end);
    if (low_str_end != n_end)
        return false;

    cursor = n_end + 1;
    const char *m_end = strchr(cursor, '/');
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
                                            double step)
{
    assert(high > low);
    const char **result = NULL;
    for (double cursor = low; cursor <= high + 0.000001; cursor += step) {
        const char *str = csfmt("%g", cursor);
        sb_push(result, str);
    }
    return result;
}

static bool parse_comma_separated_settings(const char *str, const char **namep,
                                           const char **scan_listp)
{
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

static void parse_units_str(const char *str, struct units *units)
{
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

static void parse_meas_list(const char *opts, enum meas_kind **meas_list)
{
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

static size_t simple_get_thread_count(void)
{
    int pipe_fd[2];
    if (!pipe_cloexec(pipe_fd))
        return 1;

    if (!shell_execute_and_wait("nproc", -1, pipe_fd[1], -1)) {
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

static int string_cmp(const void *ap, const void *bp)
{
    const char *a = ap;
    const char *b = bp;
    return strcmp(a, b);
}

static bool get_input_files_from_dir(const char *dirname, const char ***filesp)
{
    bool success = false;
    DIR *dir = opendir(dirname);
    if (dir == NULL) {
        csfmtperror("failed to open directory '%s' (designated for input)",
                    dirname);
        return false;
    }

    const char **files = NULL;
    for (;;) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (dirent == NULL && errno != 0) {
            csperror("readdir");
            sb_free(files);
            goto err;
        } else if (dirent == NULL) {
            break;
        }

        const char *name = dirent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        const char *path = csstrdup(name);
        sb_push(files, path);
    }

    qsort(files, sb_len(files), sizeof(*files), string_cmp);
    *filesp = files;

    success = true;
err:
    closedir(dir);
    return success;
}

static bool opt_arg(char **argv, int *cursor, const char *opt, const char **arg)
{
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
                              const char *name, double *valuep)
{
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
                        const char *name, int *valuep)
{
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
                     bool *valuep)
{
    if (strcmp(argv[*cursorp], opt_str) == 0) {
        *valuep = true;
        ++(*cursorp);
        return true;
    }
    return false;
}

#define OPT_ARR(...)                                                           \
    (const char *[]) { __VA_ARGS__, NULL }

void parse_cli_args(int argc, char **argv, struct settings *settings)
{
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
            if (settings->has_var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            // XXX: To reuse old code, --inputd is more like a macro to
            // --input '{file}' with --scanl file/... having list of files.
            const char **files;
            if (!get_input_files_from_dir(str, &files))
                exit(EXIT_FAILURE);
            settings->input.kind = INPUT_POLICY_FILE;
            settings->input.file = "{file}";
            settings->var.name = "file";
            settings->var.values = files;
            settings->var.value_count = sb_len(files);
            settings->has_var = true;
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
            entry->old_name = NULL;
            entry->n = value - 1;
            entry->name = name;
        } else if (strcmp(argv[cursor], "--renamen") == 0) {
            ++cursor;
            if (cursor + 1 >= argc) {
                error("--renamen requires 2 arguments");
                exit(EXIT_FAILURE);
            }
            const char *old_name = argv[cursor++];
            const char *name = argv[cursor++];
            struct rename_entry *entry = sb_new(settings->rename_list);
            entry->name = name;
            entry->old_name = old_name;
        } else if (opt_arg(argv, &cursor, "--rename-all", &str)) {
            if (g_rename_all_used) {
                sb_free(settings->rename_list);
                settings->rename_list = NULL;
            }
            const char **list = parse_comma_separated_list(str);
            for (size_t i = 0; i < sb_len(list); ++i) {
                struct rename_entry *entry = sb_new(settings->rename_list);
                entry->old_name = NULL;
                entry->n = i;
                entry->name = list[i];
            }
            sb_free(list);
            g_rename_all_used = true;
        } else if (opt_arg(argv, &cursor, "--scan", &str)) {
            if (settings->has_var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            double low, high, step;
            const char *name;
            if (!parse_range_scan_settings(str, &name, &low, &high, &step)) {
                error("invalid --scan argument");
                exit(EXIT_FAILURE);
            }
            const char **value_list = range_to_var_value_list(low, high, step);
            settings->var.name = name;
            settings->var.values = value_list;
            settings->var.value_count = sb_len(value_list);
            settings->has_var = true;
        } else if (opt_arg(argv, &cursor, "--scanl", &str)) {
            if (settings->has_var) {
                error("multiple benchmark variables are forbidden");
                exit(EXIT_FAILURE);
            }
            const char *name, *scan_list;
            if (!parse_comma_separated_settings(str, &name, &scan_list)) {
                error("invalid --scanl argument");
                exit(EXIT_FAILURE);
            }
            const char **value_list = parse_comma_separated_list(scan_list);
            settings->var.name = name;
            settings->var.values = value_list;
            settings->var.value_count = sb_len(value_list);
            settings->has_var = true;
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--jobs", "-j"),
                               "job count", &g_threads)) {
        } else if (opt_arg(argv, &cursor, "--save-bin-name",
                           &g_override_bin_name)) {
        } else if (opt_arg(argv, &cursor, "--json", &g_json_export_filename)) {
        } else if (opt_arg(argv, &cursor, "--out-dir", &g_out_dir) ||
                   opt_arg(argv, &cursor, "-o", &g_out_dir)) {
        } else if (opt_arg(argv, &cursor, "--sort", &str)) {
            if (strcmp(str, "auto") == 0) {
                g_sort_mode = SORT_DEFAULT;
            } else if (strcmp(str, "command") == 0) {
                g_sort_mode = SORT_RAW;
            } else if (strcmp(str, "mean-time") == 0) {
                g_sort_mode = SORT_SPEED;
            } else {
                error("invalid --sort argument");
                exit(EXIT_FAILURE);
            }
        } else if (opt_bool(argv, &cursor, "--html", &g_html)) {
            g_plot = true;
        } else if (opt_bool(argv, &cursor, "--save-bin", &g_save_bin)) {
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
        } else if (strcmp(argv[cursor], "--load-bin") == 0) {
            ++cursor;
            g_mode = APP_LOAD_BIN;
        } else if (strcmp(argv[cursor], "--simple") == 0 ||
                   strcmp(argv[cursor], "-s") == 0) {
            ++cursor;
            g_threads = simple_get_thread_count();
            g_warmup_stop.time_limit = 0.0;
            g_bench_stop.time_limit = 1.0;
        } else if (opt_arg(argv, &cursor, "--meas", &str)) {
            parse_meas_list(str, &rusage_opts);
        } else if (opt_int_pos(argv, &cursor, OPT_ARR("--baseline"),
                               "baseline number", &g_baseline)) {
            g_baseline_name = NULL;
        } else if (opt_arg(argv, &cursor, "--baseline-name", &g_baseline_name)) {
            g_baseline = -1;
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

void free_settings(struct settings *settings)
{
    if (settings->has_var) {
        struct bench_var *var = &settings->var;
        assert(sb_len(var->values) == var->value_count);
        sb_free(var->values);
    }
    sb_free(settings->args);
    sb_free(settings->meas);
    sb_free(settings->rename_list);
}
