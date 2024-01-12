#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

// Stretchy buffer
// This is implementation of type-safe generic vector in C based on
// std_stretchy_buffer.
struct cs_sb_header {
    size_t size;
    size_t capacity;
};

enum cs_input_policy_kind {
    CS_INPUT_POLICY_NULL,
    // load input from file (supplied later)
    CS_INPUT_POLICY_FILE
};

// How to handle input of command?
struct cs_input_policy {
    enum cs_input_policy_kind kind;
    const char *file;
};

enum cs_output_policy_kind {
    CS_OUTPUT_POLICY_NULL,
    // Print output to controlling terminal
    CS_OUTPUT_POLICY_INHERIT,
};

enum cs_export_kind {
    CS_DONT_EXPORT,
    CS_EXPORT_JSON
};

struct cs_export_policy {
    enum cs_export_kind kind;
    const char *filename;
};

enum cs_analyze_mode {
    CS_DONT_ANALYZE,
    CS_ANALYZE_PLOT,
    CS_ANALYZE_HTML
};

struct cs_bench_stop_policy {
    double time_limit;
    size_t runs;
    size_t min_runs;
    size_t max_runs;
};

struct cs_custom_meas {
    const char *name;
    const char *str;
    // if null interpret as seconds
    const char *units;
};

struct cs_bench_param {
    char *name;
    char **values;
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cs_cli_settings {
    const char **cmds;
    struct cs_bench_stop_policy bench_stop;
    double warmup_time;
    size_t nresamp;
    const char *shell;
    struct cs_export_policy export;
    struct cs_custom_meas *custom_meas;
    const char *prepare;
    struct cs_input_policy input_policy;
    enum cs_output_policy_kind output_policy;
    const char *analyze_dir;
    enum cs_analyze_mode analyze_mode;
    struct cs_bench_param *params;
    int plot_src;
    int no_time;
};

// Description of command to benchmark.
// Commands are executed using execve.
struct cs_cmd {
    char *str;
    char *exec;
    char **argv;
    struct cs_input_policy input;
    enum cs_output_policy_kind output;
    struct cs_custom_meas *custom_meas;
};

struct cs_cmd_group {
    char *template;
    const char *var_name;
    size_t count;
    size_t *cmd_idxs;
    const char **var_values;
};

// Information gethered from user input (settings), parsed
// and prepared for benchmarking. Some fields are copied from
// cli settings as is to reduce data dependencies.
struct cs_settings {
    struct cs_cmd *cmds;
    struct cs_cmd_group *cmd_groups;
    struct cs_bench_stop_policy bench_stop;
    double warmup_time;
    size_t nresamp;
    struct cs_custom_meas *custom_meas;
    const char *prepare_cmd;
    struct cs_export_policy export;
    enum cs_analyze_mode analyze_mode;
    const char *analyze_dir;
    int plot_src;
    int no_time;
};

// Boostrap estimate of certain statistic. Contains lower and upper bounds, as
// well as point estimate. Point estimate is commonly obtained from statistic
// calculation over original data, while lower and upper bounds are obtained
// using bootstrapping.
struct cs_est {
    double lower;
    double point;
    double upper;
};

struct cs_outliers {
    double low_severe_x;
    double low_mild_x;
    double high_mild_x;
    double high_severe_x;
    size_t low_severe;
    size_t low_mild;
    size_t high_mild;
    size_t high_severe;
};

// Describes distribution and is useful for passing benchmark data and analysis
// around.
struct cs_distr {
    const double *data;
    size_t count;
    struct cs_est mean;
    struct cs_est st_dev;
    double min;
    double max;
    double q1;
    double q3;
    double p1;
    double p5;
    double p95;
    double p99;
    struct cs_outliers outliers;
    double outlier_var;
};

struct cs_bench {
    // These fields are input options
    const char *prepare;
    const struct cs_cmd *cmd;
    // These fields are collected data
    size_t run_count;
    double *wallclocks;
    double *systimes;
    double *usertimes;
    int *exit_codes;
    size_t custom_meas_count;
    double **custom_meas;
};

struct cs_bench_analysis {
    const struct cs_bench *bench;
    struct cs_distr wall_distr;
    struct cs_est systime_est;
    struct cs_est usertime_est;
    struct cs_distr *custom_meas;
};

enum cs_big_o {
    CS_O_1,
    CS_O_N,
    CS_O_N_SQ,
    CS_O_N_CUBE,
    CS_O_LOGN,
    CS_O_NLOGN,
};

struct cs_cmd_in_group_data {
    const char *value;
    double value_double;
    double mean;
};

struct cs_cmd_group_analysis {
    const struct cs_cmd_group *group;
    size_t cmd_count;
    struct cs_cmd_in_group_data *data;
    int values_are_doubles;
    enum cs_big_o complexity;
    double coef;
    double rms;
};

struct cs_bench_results {
    size_t bench_count;
    struct cs_bench *benches;
    struct cs_bench_analysis *analyses;
    size_t fastest_idx;
    size_t group_count;
    struct cs_cmd_group_analysis *group_analyses;
};

struct cs_cpu_time {
    double user_time;
    double system_time;
};

// data needed to construct kde plot. data here is kde points computed from
// original data
struct cs_kde_plot {
    const struct cs_distr *distr;
    const char *xlabel;
    const char *title;
    double lower;
    double step;
    double *data;
    size_t count;
    double mean;
    double mean_y;
    const char *output_filename;
};

#define cs_sb_header(_a)                                                       \
    ((struct cs_sb_header *)((char *)(_a) - sizeof(struct cs_sb_header)))
#define cs_sb_size(_a) (cs_sb_header(_a)->size)
#define cs_sb_capacity(_a) (cs_sb_header(_a)->capacity)

#define cs_sb_needgrow(_a, _n)                                                 \
    (((_a) == NULL) || (cs_sb_size(_a) + (_n) >= cs_sb_capacity(_a)))
#define cs_sb_maybegrow(_a, _n)                                                \
    (cs_sb_needgrow(_a, _n) ? cs_sb_grow(_a, _n) : 0)
#define cs_sb_grow(_a, _b)                                                     \
    (*(void **)(&(_a)) = cs_sb_grow_impl((_a), (_b), sizeof(*(_a))))

#define cs_sb_free(_a) free((_a) != NULL ? cs_sb_header(_a) : NULL)
#define cs_sb_push(_a, _v)                                                     \
    (cs_sb_maybegrow(_a, 1), (_a)[cs_sb_size(_a)++] = (_v))
#define cs_sb_last(_a) ((_a)[cs_sb_size(_a) - 1])
#define cs_sb_len(_a) (((_a) != NULL) ? cs_sb_size(_a) : 0)
#define cs_sb_pop(_a) ((_a)[--cs_sb_size(_a)])
#define cs_sb_purge(_a) ((_a) ? (cs_sb_size(_a) = 0) : 0)

static __thread uint32_t rng_state;

static void *cs_sb_grow_impl(void *arr, size_t inc, size_t stride) {
    if (arr == NULL) {
        void *result = calloc(sizeof(struct cs_sb_header) + stride * inc, 1);
        struct cs_sb_header *header = result;
        header->size = 0;
        header->capacity = inc;
        return header + 1;
    }

    struct cs_sb_header *header = cs_sb_header(arr);
    size_t double_current = header->capacity * 2;
    size_t min = header->size + inc;

    size_t new_capacity = double_current > min ? double_current : min;
    void *result =
        realloc(header, sizeof(struct cs_sb_header) + stride * new_capacity);
    header = result;
    header->capacity = new_capacity;
    return header + 1;
}

static void cs_print_help_and_exit(int rc) {
    printf(
        "A command-line benchmarking tool\n"
        "\n"
        "Usage: csbench [OPTIONS] <command> ...\n"
        "\n"
        "Where options is one of:\n"
        "--warmup <n>         - specify warmup time in seconds\n"
        "--time-limit <n>     - specify how long to run benchmarks\n"
        "--runs <n>           - make exactly n runs\n"
        "--min-runs <n>       - respect time limit but make at least n runs\n"
        "--max-runs <n>       - respect time limit but make at most n runs\n"
        "--prepare <cmd>      - specify command to be executed before each "
        "benchmark run\n"
        "--nrs <n>            - specify number of resamples for bootstrapping\n"
        "--shell <shell>      - specify shell for command to be executed with. "
        "Can either be none or command resolving to shell (e.g. bash)\n"
        "--output <where>     - specify how to handle each command output. Can "
        "be either null or inherit\n"
        "--input <where>      - specify how each command should recieve its "
        "input. Can be either null or file name\n"
        "--custom <name>      - benchmark custom measurement with given name"
        "By default uses stdout of command to retrieve number\n"
        "--custom-t <name> <cmd> - command to extract custom measurement "
        "value. Value is interpreted as seconds\n"
        "--custom-x <name> <units> <cmd> - command to extract custom "
        "measurement value \n"
        "--scan <i>/<n>/<m>[/<s>] - parameter scan i in range(n, m, s). s can "
        "be omitted\n"
        "--scanl <i>/a[,...]  - parameter scacn comma separated options\n"
        "--export-json <file> - export benchmark results to json\n"
        "--analyze-dir <dir>  - directory where analysis will be saved at\n"
        "--plot               - make plots as images\n"
        "--html               - make html report\n"
        "--plot-src           - dump python scripts used to generate plots\n"
        "--no-time            - do not print default time statistics\n"
        "--help               - print this message\n"
        "--version            - print version\n");
    exit(rc);
}

static void cs_print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static int cs_parse_range_scan_settings(const char *settings, char **namep,
                                        double *lowp, double *highp,
                                        double *stepp) {
    char *name = NULL;
    const char *cursor = settings;
    const char *settings_end = settings + strlen(settings);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return 0;

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
    return 1;
err_free_name:
    free(name);
    return 0;
}

static char **cs_range_to_param_list(double low, double high, double step) {
    assert(high > low);
    char **result = NULL;
    for (double cursor = low; cursor <= high; cursor += step) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%g", cursor);
        cs_sb_push(result, strdup(buf));
    }

    return result;
}

static int cs_parse_scan_list_settings(const char *settings, char **namep,
                                       char **scan_listp) {
    char *name = NULL;
    char *scan_list = NULL;

    const char *cursor = settings;
    const char *settings_end = settings + strlen(settings);
    char *i_end = strchr(cursor, '/');
    if (i_end == NULL)
        return 0;

    size_t name_len = i_end - cursor;
    name = malloc(name_len + 1);
    memcpy(name, cursor, name_len);
    name[name_len] = '\0';

    cursor = i_end + 1;
    if (cursor == settings_end)
        goto err_free_name;

    scan_list = strdup(cursor);

    *namep = name;
    *scan_listp = scan_list;
    return 1;
err_free_name:
    free(name);
    return 0;
}

static char **cs_parse_scan_list(const char *scan_list) {
    char **param_list = NULL;
    const char *cursor = scan_list;
    const char *end = scan_list + strlen(scan_list);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            cs_sb_push(param_list, strdup(cursor));
            break;
        }

        size_t param_len = next - cursor;
        char *param = malloc(param_len + 1);
        memcpy(param, cursor, param_len);
        param[param_len] = '\0';
        cs_sb_push(param_list, param);

        cursor = next + 1;
    }

    return param_list;
}

static void cs_parse_cli_args(int argc, char **argv,
                              struct cs_cli_settings *settings) {
    settings->bench_stop.time_limit = 5.0;
    settings->bench_stop.min_runs = 5;
    settings->warmup_time = 1.0;
    settings->nresamp = 100000;
    settings->shell = "/bin/sh";
    settings->analyze_dir = ".csbench";

    int cursor = 1;
    while (cursor < argc) {
        const char *opt = argv[cursor++];
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            cs_print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(opt, "--version") == 0) {
            cs_print_version_and_exit();
        } else if (strcmp(opt, "--warmup") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --warmup requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str) {
                fprintf(stderr, "error: invalid --warmup argument\n");
                exit(EXIT_FAILURE);
            }
            if (value < 0.0) {
                fprintf(stderr,
                        "error: time limit must be positive number or zero\n");
                exit(EXIT_FAILURE);
            }
            settings->warmup_time = value;
        } else if (strcmp(opt, "--time-limit") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --time-limit requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str) {
                fprintf(stderr, "error: invalid --time-limit argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0.0) {
                fprintf(stderr, "error: time limit must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.time_limit = value;
        } else if (strcmp(opt, "--runs") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --runs requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str) {
                fprintf(stderr, "error: invalid --runs argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                fprintf(stderr, "error: run count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.runs = value;
        } else if (strcmp(opt, "--min-runs") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --min-runs requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str) {
                fprintf(stderr, "error: invalid --min-runs argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.min_runs = value;
        } else if (strcmp(opt, "--max-runs") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --max-runs requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str) {
                fprintf(stderr, "error: invalid --max-runs argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.max_runs = value;
        } else if (strcmp(opt, "--prepare") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --prepare requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *prepare_str = argv[cursor++];
            settings->prepare = prepare_str;
        } else if (strcmp(opt, "--nrs") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --nrs requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *resamples_str = argv[cursor++];
            char *str_end;
            long value = strtol(resamples_str, &str_end, 10);
            if (str_end == resamples_str) {
                fprintf(stderr, "error: invalid --nrs argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->nresamp = value;
        } else if (strcmp(opt, "--shell") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --shell requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *shell = argv[cursor++];
            if (strcmp(shell, "none") == 0)
                settings->shell = NULL;
            else
                settings->shell = shell;
        } else if (strcmp(opt, "--output") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --output requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *out = argv[cursor++];
            if (strcmp(out, "null") == 0)
                settings->output_policy = CS_OUTPUT_POLICY_NULL;
            else if (strcmp(out, "inherit") == 0)
                settings->output_policy = CS_OUTPUT_POLICY_INHERIT;
            else
                cs_print_help_and_exit(EXIT_FAILURE);
        } else if (strcmp(opt, "--input") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --input requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *input = argv[cursor++];
            if (strcmp(input, "null") == 0) {
                settings->input_policy.kind = CS_INPUT_POLICY_NULL;
            } else {
                settings->input_policy.kind = CS_INPUT_POLICY_FILE;
                settings->input_policy.file = input;
            }
        } else if (strcmp(opt, "--custom") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --custom requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            struct cs_custom_meas meas = {0};
            meas.name = name;
            cs_sb_push(settings->custom_meas, meas);
        } else if (strcmp(opt, "--custom-t") == 0) {
            if (cursor + 1 >= argc) {
                fprintf(stderr, "error: --custom-t requires 2 arguments\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct cs_custom_meas meas = {0};
            meas.name = name;
            meas.str = cmd;
            cs_sb_push(settings->custom_meas, meas);
        } else if (strcmp(opt, "--custom-x") == 0) {
            if (cursor + 2 >= argc) {
                fprintf(stderr, "error: --custom-x requires 3 arguments\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *units = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct cs_custom_meas meas = {0};
            meas.name = name;
            meas.str = cmd;
            meas.units = units;
            cs_sb_push(settings->custom_meas, meas);
        } else if (strcmp(opt, "--scan") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --scan requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *scan_settings = argv[cursor++];
            double low, high, step;
            char *name;
            if (!cs_parse_range_scan_settings(scan_settings, &name, &low, &high,
                                              &step)) {
                fprintf(stderr, "error: invalid --scan argument\n");
                exit(EXIT_FAILURE);
            }
            char **param_list = cs_range_to_param_list(low, high, step);
            struct cs_bench_param param = {0};
            param.name = name;
            param.values = param_list;
            cs_sb_push(settings->params, param);
        } else if (strcmp(opt, "--scanl") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --scanl requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *scan_settings = argv[cursor++];
            char *name, *scan_list;
            if (!cs_parse_scan_list_settings(scan_settings, &name,
                                             &scan_list)) {
                fprintf(stderr, "error: invalid --scanl argument\n");
                exit(EXIT_FAILURE);
            }
            char **param_list = cs_parse_scan_list(scan_list);
            free(scan_list);
            struct cs_bench_param param = {0};
            param.name = name;
            param.values = param_list;
            cs_sb_push(settings->params, param);
        } else if (strcmp(opt, "--export-json") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --export-json requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *export_filename = argv[cursor++];
            settings->export.kind = CS_EXPORT_JSON;
            settings->export.filename = export_filename;
        } else if (strcmp(opt, "--analyze-dir") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --analyze-dir requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *dir = argv[cursor++];
            settings->analyze_dir = dir;
        } else if (strcmp(opt, "--html") == 0) {
            settings->analyze_mode = CS_ANALYZE_HTML;
        } else if (strcmp(opt, "--plot") == 0) {
            settings->analyze_mode = CS_ANALYZE_PLOT;
        } else if (strcmp(opt, "--plot-src") == 0) {
            settings->plot_src = 1;
        } else if (strcmp(opt, "--no-time") == 0) {
            settings->no_time = 1;
        } else {
            cs_sb_push(settings->cmds, opt);
        }
    }
}

static void cs_free_cli_settings(struct cs_cli_settings *settings) {
    for (size_t i = 0; i < cs_sb_len(settings->params); ++i) {
        struct cs_bench_param *param = settings->params + i;
        free(param->name);
        for (size_t j = 0; j < cs_sb_len(param->values); ++j)
            free(param->values[j]);
        cs_sb_free(param->values);
    }
    cs_sb_free(settings->cmds);
    cs_sb_free(settings->custom_meas);
    cs_sb_free(settings->params);
}

static void cs_replace_str(char *buf, size_t buf_size, const char *src,
                           const char *name, const char *value) {
    // TODO: this should handle buffer overflow
    (void)buf_size;
    size_t param_name_len = strlen(name);
    char *wr_cursor = buf;
    const char *rd_cursor = src;
    while (*rd_cursor) {
        if (*rd_cursor == '{' &&
            strncmp(rd_cursor + 1, name, param_name_len) == 0 &&
            rd_cursor[param_name_len + 1] == '}') {
            rd_cursor += 2 + param_name_len;
            size_t len = strlen(value);
            memcpy(wr_cursor, value, len);
            wr_cursor += len;
        } else {
            *wr_cursor++ = *rd_cursor++;
        }
    }
    *wr_cursor = '\0';
}

static char **cs_split_shell_words(const char *cmd) {
    char **words = NULL;
    char *current_word = NULL;

    enum {
        STATE_DELIMETER,
        STATE_BACKSLASH,
        STATE_UNQUOTED,
        STATE_UNQUOTED_BACKSLASH,
        STATE_SINGLE_QUOTED,
        STATE_DOUBLE_QUOTED,
        STATE_DOUBLE_QUOTED_BACKSLASH,
        STATE_COMMENT
    } state = STATE_DELIMETER;

    for (;;) {
        int c = *cmd++;
        switch (state) {
        case STATE_DELIMETER:
            switch (c) {
            case '\0':
                if (current_word != NULL) {
                    cs_sb_push(current_word, '\0');
                    cs_sb_push(words, current_word);
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
                state = STATE_DELIMETER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                cs_sb_push(current_word, c);
                state = STATE_UNQUOTED;
                break;
            }
            break;
        case STATE_BACKSLASH:
            switch (c) {
            case '\0':
                cs_sb_push(current_word, '\\');
                cs_sb_push(current_word, '\0');
                cs_sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_DELIMETER;
                break;
            default:
                cs_sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED:
            switch (c) {
            case '\0':
                cs_sb_push(current_word, '\0');
                cs_sb_push(words, current_word);
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
                cs_sb_push(current_word, '\0');
                cs_sb_push(words, current_word);
                current_word = NULL;
                state = STATE_DELIMETER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                cs_sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED_BACKSLASH:
            switch (c) {
            case '\0':
                cs_sb_push(current_word, '\\');
                cs_sb_push(current_word, '\0');
                cs_sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_UNQUOTED;
                break;
            default:
                cs_sb_push(current_word, c);
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
                cs_sb_push(current_word, c);
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
                cs_sb_push(current_word, c);
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
                cs_sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            default:
                cs_sb_push(current_word, '\\');
                cs_sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            }
            break;
        case STATE_COMMENT:
            switch (c) {
            case '\0':
                goto out;
            case '\n':
                state = STATE_DELIMETER;
                break;
            default:
                break;
            }
            break;
        }
    }
error:
    for (size_t i = 0; i < cs_sb_len(words); ++i)
        cs_sb_free(words[i]);
    cs_sb_free(words);
    words = NULL;
out:
    return words;
}

static int cs_extract_exec_and_argv(const char *cmd_str, char **exec,
                                    char ***argv) {
    int rc = 0;
    char **words = cs_split_shell_words(cmd_str);
    if (words == NULL) {
        fprintf(stderr, "error: invalid command syntax\n");
        return -1;
    }

    *exec = strdup(words[0]);
    cs_sb_push(*argv, strdup(words[0]));
    for (size_t i = 1; i < cs_sb_len(words); ++i)
        cs_sb_push(*argv, strdup(words[i]));
    cs_sb_push(*argv, NULL);

    for (size_t i = 0; i < cs_sb_len(words); ++i)
        cs_sb_free(words[i]);
    cs_sb_free(words);

    return rc;
}

static int cs_init_cmd_exec(const char *shell, const char *cmd_str,
                            struct cs_cmd *cmd) {
    if (cs_extract_exec_and_argv(shell, &cmd->exec, &cmd->argv) != 0)
        return -1;
    if (shell) {
        // pop NULL
        (void)cs_sb_pop(cmd->argv);
        cs_sb_push(cmd->argv, strdup("-c"));
        cs_sb_push(cmd->argv, strdup(cmd_str));
        cs_sb_push(cmd->argv, NULL);
    }
    cmd->str = strdup(cmd_str);
    return 0;
}

static void cs_free_settings(struct cs_settings *settings) {
    for (size_t i = 0; i < cs_sb_len(settings->cmds); ++i) {
        struct cs_cmd *cmd = settings->cmds + i;
        free(cmd->exec);
        for (char **word = cmd->argv; *word; ++word)
            free(*word);
        cs_sb_free(cmd->argv);
        free(cmd->str);
    }
    for (size_t i = 0; i < cs_sb_len(settings->cmd_groups); ++i) {
        struct cs_cmd_group *group = settings->cmd_groups + i;
        free(group->template);
        free(group->cmd_idxs);
        free(group->var_values);
    }
    cs_sb_free(settings->cmds);
    cs_sb_free(settings->cmd_groups);
}

static int cs_init_settings(const struct cs_cli_settings *cli,
                            struct cs_settings *settings) {
    settings->bench_stop = cli->bench_stop;
    settings->warmup_time = cli->warmup_time;
    settings->export = cli->export;
    settings->prepare_cmd = cli->prepare;
    settings->nresamp = cli->nresamp;
    settings->analyze_mode = cli->analyze_mode;
    settings->analyze_dir = cli->analyze_dir;
    settings->custom_meas = cli->custom_meas;
    settings->plot_src = cli->plot_src;
    settings->no_time = cli->no_time;

    // try to catch invalid file as early as possible,
    // because later error handling can become troublesome (after fork()).
    if (cli->input_policy.kind == CS_INPUT_POLICY_FILE &&
        access(cli->input_policy.file, R_OK) == -1) {
        fprintf(stderr,
                "error: file specified as command input is not accessable "
                "(%s)\n",
                cli->input_policy.file);
        return -1;
    }

    size_t cmd_count = cs_sb_len(cli->cmds);
    if (cmd_count == 0) {
        fprintf(stderr, "error: no commands specified\n");
        return -1;
    }

    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->cmds[i];
        int found_param = 0;
        for (size_t j = 0; j < cs_sb_len(cli->params); ++j) {
            const struct cs_bench_param *param = cli->params + j;
            char buf[256];
            snprintf(buf, sizeof(buf), "{%s}", param->name);
            if (strstr(cmd_str, buf) == NULL)
                continue;

            size_t its_in_group = cs_sb_len(param->values);
            found_param = 1;
            struct cs_cmd_group group = {0};
            group.count = its_in_group;
            group.cmd_idxs = calloc(its_in_group, sizeof(*group.cmd_idxs));
            group.var_values = calloc(its_in_group, sizeof(*group.var_values));
            for (size_t k = 0; k < its_in_group; ++k) {
                const char *param_value = param->values[k];
                cs_replace_str(buf, sizeof(buf), cmd_str, param->name,
                               param_value);

                struct cs_cmd cmd = {0};
                cmd.input = cli->input_policy;
                cmd.output = cli->output_policy;
                cmd.custom_meas = settings->custom_meas;
                if (cs_init_cmd_exec(cli->shell, buf, &cmd) == -1) {
                    free(group.cmd_idxs);
                    free(group.var_values);
                    goto err_free_settings;
                }

                group.cmd_idxs[k] = cs_sb_len(settings->cmds);
                group.var_values[k] = param_value;
                cs_sb_push(settings->cmds, cmd);
            }
            group.var_name = param->name;
            group.template = strdup(cmd_str);
            cs_sb_push(settings->cmd_groups, group);
        }

        if (!found_param) {
            struct cs_cmd cmd = {0};
            cmd.input = cli->input_policy;
            cmd.output = cli->output_policy;
            cmd.custom_meas = settings->custom_meas;
            if (cs_init_cmd_exec(cli->shell, cmd_str, &cmd) == -1)
                goto err_free_settings;

            cs_sb_push(settings->cmds, cmd);
        }
    }

    return 0;
err_free_settings:
    cs_free_settings(settings);
    return -1;
}

#if defined(__APPLE__)
static double cs_get_time(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
#else
static double cs_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static struct cs_cpu_time cs_getcputime(void) {
    struct rusage rus = {0};
    getrusage(RUSAGE_CHILDREN, &rus);
    struct cs_cpu_time time;
    time.user_time = rus.ru_utime.tv_sec + (double)rus.ru_utime.tv_usec / 1e6;
    time.system_time = rus.ru_stime.tv_sec + (double)rus.ru_stime.tv_usec / 1e6;
    return time;
}

static void cs_apply_input_policy(const struct cs_input_policy *policy) {
    switch (policy->kind) {
    case CS_INPUT_POLICY_NULL: {
        close(STDIN_FILENO);
        int fd = open("/dev/null", O_RDONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case CS_INPUT_POLICY_FILE: {
        close(STDIN_FILENO);
        int fd = open(policy->file, O_RDONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    }
}

static void cs_apply_output_policy(enum cs_output_policy_kind policy) {
    switch (policy) {
    case CS_OUTPUT_POLICY_NULL: {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case CS_OUTPUT_POLICY_INHERIT:
        break;
    }
}

static int cs_exec_cmd(const struct cs_cmd *cmd, int stdout_fd) {
    int rc = -1;
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        goto out;
    }

    if (pid == 0) {
        cs_apply_input_policy(&cmd->input);
        // special handling when stdout needs to be piped
        if (stdout_fd != -1) {
            close(STDERR_FILENO);
            int fd = open("/dev/null", O_WRONLY);
            if (fd == -1)
                _exit(-1);
            if (dup2(fd, STDERR_FILENO) == -1)
                _exit(-1);
            if (stdout_fd == -1)
                stdout_fd = fd;
            if (dup2(stdout_fd, STDOUT_FILENO) == -1)
                _exit(-1);
            close(fd);
        } else {
            cs_apply_output_policy(cmd->output);
        }
        if (execvp(cmd->exec, cmd->argv) == -1)
            _exit(-1);
    }

    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        goto out;
    }

    // shell-like exit codes
    if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        rc = 128 + WTERMSIG(status);

out:
    return rc;
}

static int cs_process_finished_correctly(pid_t pid) {
    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        return 0;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;

    return 0;
}

static int cs_execute_prepare(const char *cmd) {
    char *exec = "/bin/sh";
    char *argv[] = {"sh", "-c", NULL, NULL};
    argv[2] = (char *)cmd;

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1 ||
            dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        if (execv(exec, argv) == -1)
            _exit(-1);
    }

    if (!cs_process_finished_correctly(pid))
        return -1;

    return 0;
}

static int cs_execute_custom(struct cs_custom_meas *custom, int in_fd,
                             int out_fd) {
    char *exec = "/bin/sh";
    char *argv[] = {"sh", "-c", NULL, NULL};
    if (custom->str)
        argv[2] = (char *)custom->str;
    else
        argv[2] = "cat";

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        close(STDIN_FILENO);
        if (dup2(in_fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(STDOUT_FILENO);
        if (dup2(out_fd, STDOUT_FILENO) == -1)
            _exit(-1);
        close(STDERR_FILENO);
        if (execv(exec, argv) == -1)
            _exit(-1);
    }

    if (!cs_process_finished_correctly(pid))
        return -1;

    return 0;
}

static int cs_parse_custom_output(int fd, double *valuep) {
    char buf[4096];
    ssize_t nread = read(fd, buf, sizeof(buf));
    if (nread == -1) {
        perror("read");
        return -1;
    }
    if (nread == sizeof(buf)) {
        fprintf(stderr, "error: custom measurement output is too large\n");
        return -1;
    }
    buf[nread] = '\0';

    char *end = NULL;
    double value = strtod(buf, &end);
    if (end == buf) {
        fprintf(stderr, "error: invalid custom measurement output\n");
        return -1;
    }

    *valuep = value;
    return 0;
}

static int cs_do_custom_measurements(struct cs_bench *bench, int stdout_fd) {
    if (bench->cmd->custom_meas == NULL)
        return 0;

    int rc = -1;
    char path[] = "/tmp/csbench_tmp_XXXXXX";
    int custom_output_fd = mkstemp(path);
    if (custom_output_fd == -1) {
        perror("mkstemp");
        goto out;
    }

    size_t custom_count = bench->custom_meas_count;
    for (size_t i = 0; i < custom_count; ++i) {
        struct cs_custom_meas *custom = bench->cmd->custom_meas + i;
        if (lseek(stdout_fd, 0, SEEK_SET) == (off_t)-1 ||
            lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            goto out;
        }

        if (cs_execute_custom(custom, stdout_fd, custom_output_fd) == -1)
            goto out;

        if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            goto out;
        }

        double value;
        if (cs_parse_custom_output(custom_output_fd, &value) == -1)
            goto out;

        cs_sb_push(bench->custom_meas[i], value);
    }
    rc = 0;
out:
    if (custom_output_fd != -1) {
        close(custom_output_fd);
        unlink(path);
    }
    return rc;
}

static int cs_exec_and_measure(struct cs_bench *bench) {
    int ret = -1;
    int stdout_fd = -1;
    char path[] = "/tmp/csbench_out_XXXXXX";
    if (bench->cmd->custom_meas != NULL) {
        stdout_fd = mkstemp(path);
        if (stdout_fd == -1) {
            perror("mkstemp");
            goto out;
        }
    }

    volatile struct cs_cpu_time cpu_start = cs_getcputime();
    volatile double wall_clock_start = cs_get_time();
    volatile int rc = cs_exec_cmd(bench->cmd, stdout_fd);
    volatile double wall_clock_end = cs_get_time();
    volatile struct cs_cpu_time cpu_end = cs_getcputime();

    if (rc == -1) {
        fprintf(stderr, "error: failed to execute command\n");
        goto out;
    }

    ++bench->run_count;
    cs_sb_push(bench->exit_codes, rc);
    cs_sb_push(bench->wallclocks, wall_clock_end - wall_clock_start);
    cs_sb_push(bench->systimes, cpu_end.system_time - cpu_start.system_time);
    cs_sb_push(bench->usertimes, cpu_end.user_time - cpu_start.user_time);

    if (cs_do_custom_measurements(bench, stdout_fd) == -1)
        goto out;

    ret = 0;
out:
    if (stdout_fd != -1) {
        close(stdout_fd);
        unlink(path);
    }
    return ret;
}

static int cs_warmup(const struct cs_cmd *cmd, double time_limit) {
    if (time_limit < 0.0)
        return 0;

    double start_time = cs_get_time();
    double end_time;
    do {
        if (cs_exec_cmd(cmd, -1) == -1) {
            fprintf(stderr, "error: failed to execute warmup command\n");
            return -1;
        }
        end_time = cs_get_time();
    } while (end_time - start_time < time_limit);

    return 0;
}

static int cs_run_benchmark(struct cs_bench *bench,
                            const struct cs_bench_stop_policy *policy) {
    if (policy->runs != 0) {
        for (size_t run_idx = 0; run_idx < policy->runs; ++run_idx) {
            if (bench->prepare && cs_execute_prepare(bench->prepare) == -1)
                return -1;
            if (cs_exec_and_measure(bench) == -1)
                return -1;
        }

        return 0;
    }

    double niter_accum = 1;
    size_t niter = 1;
    double start_time = cs_get_time();
    double time_limit = policy->time_limit;
    size_t min_runs = policy->min_runs;
    size_t max_runs = policy->max_runs;
    for (size_t count = 1;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            if (bench->prepare && cs_execute_prepare(bench->prepare) == -1)
                return -1;
            if (cs_exec_and_measure(bench) == -1)
                return -1;
        }

        double end_time = cs_get_time();
        if (((max_runs != 0 ? count >= max_runs : 0) ||
             (end_time - start_time > time_limit)) &&
            (min_runs != 0 ? count >= min_runs : 1))
            break;

        for (;;) {
            niter_accum *= 1.05;
            size_t new_niter = (size_t)floor(niter_accum);
            if (new_niter != niter)
                break;
        }
    }

    return 0;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void cs_resample(const double *src, size_t count, double *dst,
                        uint32_t entropy) {
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[xorshift32(&entropy) % count];
}

static double cs_stat_mean(const double *v, size_t count) {
    double result = 0.0;
    for (size_t i = 0; i < count; ++i)
        result += v[i];
    return result / (double)count;
}

static double cs_stat_st_dev(const double *v, size_t count) {
    double mean = cs_stat_mean(v, count);
    double result = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double t = v[i] - mean;
        result += t * t;
    }
    return sqrt(result / (double)count);
}

#define cs_bootstrap(_name, _stat_fn)                                          \
    static void cs_do_bootstrap_##_name(const double *src, size_t count,       \
                                        double *tmp, size_t resamples,         \
                                        double *min_d, double *max_d) {        \
        double min = INFINITY;                                                 \
        double max = -INFINITY;                                                \
        for (size_t sample = 0; sample < resamples; ++sample) {                \
            cs_resample(src, count, tmp, xorshift32(&rng_state));              \
            double stat = _stat_fn(tmp, count);                                \
            if (stat < min)                                                    \
                min = stat;                                                    \
            if (stat > max)                                                    \
                max = stat;                                                    \
        }                                                                      \
        *min_d = min;                                                          \
        *max_d = max;                                                          \
    }                                                                          \
    static void cs_bootstrap_##_name(const double *data, size_t count,         \
                                     double *tmp, size_t resamples,            \
                                     struct cs_est *est) {                     \
        est->point = _stat_fn(data, count);                                    \
        cs_do_bootstrap_##_name(data, count, tmp, resamples, &est->lower,      \
                                &est->upper);                                  \
    }                                                                          \
    static __attribute__((used)) void cs_estimate_##_name(                     \
        const double *data, size_t count, size_t resamples,                    \
        struct cs_est *est) {                                                  \
        double *tmp = malloc(count * sizeof(*tmp));                            \
        cs_bootstrap_##_name(data, count, tmp, resamples, est);                \
        free(tmp);                                                             \
    }

cs_bootstrap(mean, cs_stat_mean)
cs_bootstrap(st_dev, cs_stat_st_dev)

#undef cs_bootstrap

static struct cs_outliers cs_classify_outliers(const struct cs_distr *distr) {
    double q1 = distr->q1;
    double q3 = distr->q3;
    double iqr = q3 - q1;
    double los = q1 - (iqr * 3.0);
    double lom = q1 - (iqr * 1.5);
    double him = q3 + (iqr * 1.5);
    double his = q3 + (iqr * 3.0);

    struct cs_outliers result = {0};
    result.low_severe_x = los;
    result.low_mild_x = lom;
    result.high_mild_x = him;
    result.high_severe_x = his;
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < los)
            ++result.low_severe;
        else if (v > his)
            ++result.high_severe;
        else if (v < lom)
            ++result.low_mild;
        else if (v > him)
            ++result.high_mild;
    }
    return result;
}

static double cs_c_max(double x, double u_a, double a, double sigma_b_2,
                       double sigma_g_2) {
    double k = u_a - x;
    double d = k * k;
    double ad = a * d;
    double k1 = sigma_b_2 - a * sigma_g_2 + ad;
    double k0 = -a * ad;
    double det = k1 * k1 - 4 * sigma_g_2 * k0;
    return floor(-2.0 * k0 / (k1 + sqrt(det)));
}

static double cs_var_out(double c, double a, double sigma_b_2,
                         double sigma_g_2) {
    double ac = a - c;
    return (ac / a) * (sigma_b_2 - ac * sigma_g_2);
}

static double cs_outlier_variance(double mean, double st_dev, double a) {
    double sigma_b = st_dev;
    double u_a = mean / a;
    double u_g_min = u_a / 2.0;
    double sigma_g = fmin(u_g_min / 4.0, sigma_b / sqrt(a));
    double sigma_g_2 = sigma_g * sigma_g;
    double sigma_b_2 = sigma_b * sigma_b;
    double var_out_min =
        fmin(cs_var_out(1, a, sigma_b_2, sigma_g_2),
             cs_var_out(fmin(cs_c_max(0, u_a, a, sigma_b_2, sigma_g_2),
                             cs_c_max(u_g_min, u_a, a, sigma_b_2, sigma_g_2)),
                        a, sigma_b_2, sigma_g_2)) /
        sigma_b_2;
    return var_out_min;
}

static int cs_compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

static void cs_estimate_distr(const double *data, size_t count, size_t nresamp,
                              struct cs_distr *distr) {
    double *tmp = malloc(count * sizeof(*tmp));
    distr->data = data;
    distr->count = count;
    cs_bootstrap_mean(data, count, tmp, nresamp, &distr->mean);
    cs_bootstrap_st_dev(data, count, tmp, nresamp, &distr->st_dev);
    memcpy(tmp, data, count * sizeof(*tmp));
    qsort(tmp, count, sizeof(*tmp), cs_compare_doubles);
    distr->q1 = tmp[count / 4];
    distr->q3 = tmp[count * 3 / 4];
    distr->p1 = tmp[count / 100];
    distr->p5 = tmp[count * 5 / 100];
    distr->p95 = tmp[count * 95 / 100];
    distr->p99 = tmp[count * 99 / 100];
    distr->min = tmp[0];
    distr->max = tmp[count - 1];
    free(tmp);
    distr->outliers = cs_classify_outliers(distr);
    distr->outlier_var =
        cs_outlier_variance(distr->mean.point, distr->st_dev.point, count);
}

// This monstosity is used to silence warnings.
// Macros are used to explicitly inline.
#define cs_fitting_curve_1(_n) ((double)_n, 1.0)
#define cs_fitting_curve_n(_n) (_n)
#define cs_fitting_curve_n_sq(_n) ((_n) * (_n))
#define cs_fitting_curve_n_cube(_n) ((_n) * (_n) * (_n))
#define cs_fitting_curve_logn(_n) log2(_n)
#define cs_fitting_curve_nlogn(_n) ((_n) * log2(_n))

static double cs_fitting_curve(double n, enum cs_big_o complexity) {
    switch (complexity) {
    case CS_O_1:
        return cs_fitting_curve_1(n);
    case CS_O_N:
        return cs_fitting_curve_n(n);
    case CS_O_N_SQ:
        return cs_fitting_curve_n_sq(n);
    case CS_O_N_CUBE:
        return cs_fitting_curve_n_cube(n);
    case CS_O_LOGN:
        return cs_fitting_curve_logn(n);
    case CS_O_NLOGN:
        return cs_fitting_curve_nlogn(n);
    }
    return 0.0;
}

#define cs_mls(_name, _fitting)                                                \
    static double cs_mls_##_name(const double *x, const double *y,             \
                                 size_t count, double *rmsp) {                 \
        double sigma_gn_sq = 0.0;                                              \
        double sigma_t = 0.0;                                                  \
        double sigma_t_gn = 0.0;                                               \
        for (size_t i = 0; i < count; ++i) {                                   \
            double gn_i = _fitting(x[i]);                                      \
            sigma_gn_sq += gn_i * gn_i;                                        \
            sigma_t += y[i];                                                   \
            sigma_t_gn += y[i] * gn_i;                                         \
        }                                                                      \
        double coef = sigma_t_gn / sigma_gn_sq;                                \
        double rms = 0.0;                                                      \
        for (size_t i = 0; i < count; ++i) {                                   \
            double fit = coef * _fitting(x[i]);                                \
            double a = y[i] - fit;                                             \
            rms += a * a;                                                      \
        }                                                                      \
        double mean = sigma_t / count;                                         \
        *rmsp = sqrt(rms / count) / mean;                                      \
        return coef;                                                           \
    }

cs_mls(1, cs_fitting_curve_1)
cs_mls(n, cs_fitting_curve_n)
cs_mls(n_sq, cs_fitting_curve_n_sq)
cs_mls(n_cube, cs_fitting_curve_n_cube)
cs_mls(logn, cs_fitting_curve_logn)
cs_mls(nlogn, cs_fitting_curve_nlogn)

#undef cs_mls

static enum cs_big_o cs_mls(const double *x, const double *y, size_t count,
                            double *coefp, double *rmsp) {
    enum cs_big_o best_fit = CS_O_1;
    double best_fit_coef, best_fit_rms;
    best_fit_coef = cs_mls_1(x, y, count, &best_fit_rms);

#define cs_check(_name, _e)                                                    \
    do {                                                                       \
        double coef, rms;                                                      \
        coef = _name(x, y, count, &rms);                                       \
        if (rms < best_fit_rms) {                                              \
            best_fit = _e;                                                     \
            best_fit_coef = coef;                                              \
            best_fit_rms = rms;                                                \
        }                                                                      \
    } while (0)

    cs_check(cs_mls_n, CS_O_N);
    cs_check(cs_mls_n_sq, CS_O_N_SQ);
    cs_check(cs_mls_n_cube, CS_O_N_CUBE);
    cs_check(cs_mls_logn, CS_O_LOGN);
    cs_check(cs_mls_nlogn, CS_O_NLOGN);

#undef cs_check

    *coefp = best_fit_coef;
    *rmsp = best_fit_rms;

    return best_fit;
}

static int cs_compare_cmds_in_group(const void *arg1, const void *arg2) {
    const struct cs_cmd_in_group_data *a = arg1;
    const struct cs_cmd_in_group_data *b = arg2;
    if (a->mean < b->mean)
        return -1;
    if (a->mean > b->mean)
        return 1;
    return 0;
}

static void cs_analyze_benchmark(const struct cs_bench *bench, size_t nresamp,
                                 struct cs_bench_analysis *analysis) {
    size_t count = bench->run_count;
    analysis->bench = bench;
    cs_estimate_distr(bench->wallclocks, count, nresamp, &analysis->wall_distr);
    cs_estimate_mean(bench->systimes, count, nresamp, &analysis->systime_est);
    cs_estimate_mean(bench->usertimes, count, nresamp, &analysis->usertime_est);
    for (size_t i = 0; i < bench->custom_meas_count; ++i)
        cs_estimate_distr(bench->custom_meas[i], count, nresamp,
                          analysis->custom_meas + i);
}

static void cs_compare_benches(struct cs_bench_results *results) {
    if (results->bench_count == 1)
        return;

    size_t bench_count = results->bench_count;
    size_t best_idx = 0;
    double best_mean = results->analyses[0].wall_distr.mean.point;
    for (size_t i = 1; i < bench_count; ++i) {
        const struct cs_bench_analysis *analysis = results->analyses + i;
        double mean = analysis->wall_distr.mean.point;
        if (mean < best_mean) {
            best_idx = i;
            best_mean = mean;
        }
    }

    results->fastest_idx = best_idx;
}

static void cs_analyze_cmd_groups(const struct cs_settings *settings,
                                  struct cs_bench_results *results) {
    size_t group_count = results->group_count = cs_sb_len(settings->cmd_groups);
    results->group_count = group_count;
    results->group_analyses =
        calloc(group_count, sizeof(*results->group_analyses));
    for (size_t i = 0; i < group_count; ++i) {
        const struct cs_cmd_group *group = settings->cmd_groups + i;
        size_t cmd_count = group->count;

        struct cs_cmd_group_analysis *analysis = results->group_analyses + i;
        analysis->group = group;
        analysis->cmd_count = cmd_count;
        analysis->data = calloc(analysis->cmd_count, sizeof(*analysis->data));

        int values_are_doubles = 1;
        for (size_t j = 0; j < cmd_count; ++j) {
            const char *value = group->var_values[j];
            const struct cs_cmd *cmd = settings->cmds + group->cmd_idxs[j];
            size_t bench_idx = -1;
            for (size_t k = 0; k < results->bench_count; ++k) {
                if (results->benches[k].cmd == cmd) {
                    bench_idx = k;
                    break;
                }
            }
            assert(bench_idx != (size_t)-1);

            char *end = NULL;
            double value_double = strtod(value, &end);
            if (end == value)
                values_are_doubles = 0;

            analysis->data[j] = (struct cs_cmd_in_group_data){
                value, value_double,
                results->analyses[bench_idx].wall_distr.mean.point};
        }
        qsort(analysis->data, cmd_count, sizeof(*analysis->data),
              cs_compare_cmds_in_group);
        analysis->values_are_doubles = values_are_doubles;
        if (values_are_doubles) {
            double *x = calloc(cmd_count, sizeof(*x));
            double *y = calloc(cmd_count, sizeof(*y));
            for (size_t j = 0; j < cmd_count; ++j) {
                x[j] = analysis->data[j].value_double;
                y[j] = analysis->data[j].mean;
            }

            analysis->complexity =
                cs_mls(x, y, cmd_count, &analysis->coef, &analysis->rms);
            free(x);
            free(y);
        }
    }
}

static void cs_print_exit_code_info(const struct cs_bench *bench) {
    size_t count_nonzero = 0;
    for (size_t i = 0; i < bench->run_count; ++i)
        if (bench->exit_codes[i] != 0)
            ++count_nonzero;

    if (count_nonzero == bench->run_count) {
        printf("all commands have non-zero exit code: %d\n",
               bench->exit_codes[0]);
    } else if (count_nonzero != 0) {
        printf("some runs (%zu) have non-zero exit code\n", count_nonzero);
    }
}

static int cs_print_time(char *dst, size_t sz, double t) {
    int count = 0;
    if (t < 0) {
        t = -t;
        count = snprintf(dst, sz, "-");
        dst += count;
        sz -= count;
    }

    const char *units = "s ";
    if (t >= 1) {
    } else if (t >= 1e-3) {
        units = "ms";
        t *= 1e3;
    } else if (t >= 1e-6) {
        units = "μs";
        t *= 1e6;
    } else if (t >= 1e-9) {
        units = "ns";
        t *= 1e9;
    }

    if (t >= 1e9)
        count += snprintf(dst, sz, "%.4g %s", t, units);
    else if (t >= 1e3)
        count += snprintf(dst, sz, "%.0f %s", t, units);
    else if (t >= 1e2)
        count += snprintf(dst, sz, "%.1f %s", t, units);
    else if (t >= 1e1)
        count += snprintf(dst, sz, "%.2f %s", t, units);
    else
        count += snprintf(dst, sz, "%.3f %s", t, units);

    return count;
}

static void cs_print_time_estimate(const char *name, const struct cs_est *est) {
    char buf1[256], buf2[256], buf3[256];
    cs_print_time(buf1, sizeof(buf1), est->lower);
    cs_print_time(buf2, sizeof(buf2), est->point);
    cs_print_time(buf3, sizeof(buf3), est->upper);
    printf("%7s %s %s %s\n", name, buf1, buf2, buf3);
}

static void cs_print_time_distr(const struct cs_distr *dist) {
    char buf1[256], buf2[256];
    cs_print_time(buf1, sizeof(buf1), dist->min);
    cs_print_time(buf2, sizeof(buf2), dist->max);
    printf("min %s max %s\n", buf1, buf2);
    cs_print_time_estimate("mean", &dist->mean);
    cs_print_time_estimate("st dev", &dist->st_dev);
}

static void cs_print_outliers(const struct cs_outliers *outliers,
                              size_t run_count) {
    size_t outlier_count = outliers->low_mild + outliers->high_mild +
                           outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        printf("found %zu outliers across %zu measurements (%.2f%%)\n",
               outlier_count, run_count,
               (double)outlier_count / run_count * 100.0);
        if (outliers->low_severe)
            printf("%zu (%.2f%%) low severe\n", outliers->low_severe,
                   (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            printf("%zu (%.2f%%) low mild\n", outliers->low_mild,
                   (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            printf("%zu (%.2f%%) high mild\n", outliers->high_mild,
                   (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            printf("%zu (%.2f%%) high severe\n", outliers->high_severe,
                   (double)outliers->high_severe / run_count * 100.0);
    }
}

static const char *cs_outliers_variance_str(double fraction) {
    if (fraction < 0.01)
        return "no";
    else if (fraction < 0.1)
        return "a slight";
    else if (fraction < 0.5)
        return "a moderate";
    return "a severe";
}

static void cs_print_outlier_var(double var) {
    printf("outlying measurements have %s (%.1f%%) effect on estimated "
           "standard deviation\n",
           cs_outliers_variance_str(var), var * 100.0);
}

static void cs_print_estimate(const char *name, const struct cs_est *est) {
    char buf1[256], buf2[256], buf3[256];
    snprintf(buf1, sizeof(buf1), "%.5g", est->lower);
    snprintf(buf2, sizeof(buf1), "%.5g", est->point);
    snprintf(buf3, sizeof(buf1), "%.5g", est->upper);
    printf("%7s %8s %8s %8s\n", name, buf1, buf2, buf3);
}

static void cs_print_distr(const struct cs_distr *dist) {
    printf("min %.5g max %.5g\n", dist->min, dist->max);
    cs_print_estimate("mean", &dist->mean);
    cs_print_estimate("st dev", &dist->st_dev);
}

static void cs_ref_speed(double u1, double sigma1, double u2, double sigma2,
                         double *ref_u, double *ref_sigma) {
    double ref = u1 / u2;
    // propagate standard deviation for formula (t1 / t2)
    double a = sigma1 / u1;
    double b = sigma2 / u2;
    double ref_st_dev = ref * sqrt(a * a + b * b);

    *ref_u = ref;
    *ref_sigma = ref_st_dev;
}

static const char *cs_big_o_str(enum cs_big_o complexity) {
    switch (complexity) {
    case CS_O_1:
        return "constant (O(1))";
    case CS_O_N:
        return "linear (O(N))";
    case CS_O_N_SQ:
        return "quadratic (O(N^2))";
    case CS_O_N_CUBE:
        return "cubic (O(N^3))";
    case CS_O_LOGN:
        return "logarithmic (O(log(N)))";
    case CS_O_NLOGN:
        return "linearithmic (O(N*log(N)))";
    }
    return NULL;
}

static void cs_print_benchmark_info(const struct cs_bench_analysis *analysis,
                                    int no_time) {
    const struct cs_bench *bench = analysis->bench;
    printf("command\t'%s'\n", bench->cmd->str);
    printf("%zu runs\n", bench->run_count);
    if (!no_time) {
        cs_print_exit_code_info(bench);
        cs_print_time_distr(&analysis->wall_distr);
        cs_print_time_estimate("systime", &analysis->systime_est);
        cs_print_time_estimate("usrtime", &analysis->usertime_est);
        cs_print_outliers(&analysis->wall_distr.outliers, bench->run_count);
        cs_print_outlier_var(analysis->wall_distr.outlier_var);
    }
    for (size_t i = 0; i < bench->custom_meas_count; ++i) {
        printf("custom measurement %s\n", bench->cmd->custom_meas[i].name);
        if (analysis->bench->cmd->custom_meas[i].units)
            cs_print_distr(analysis->custom_meas + i);
        else
            cs_print_time_distr(analysis->custom_meas + i);
        cs_print_outliers(&analysis->custom_meas[i].outliers, bench->run_count);
        cs_print_outlier_var(analysis->custom_meas[i].outlier_var);
    }
}

static void cs_print_cmd_comparison(const struct cs_bench_results *results) {
    if (results->bench_count == 1)
        return;

    size_t best_idx = results->fastest_idx;
    const struct cs_bench_analysis *best = results->analyses + best_idx;
    printf("Fastest command '%s'\n", best->bench->cmd->str);
    for (size_t i = 0; i < results->bench_count; ++i) {
        const struct cs_bench_analysis *analysis = results->analyses + i;
        if (analysis == best)
            continue;

        double ref, ref_st_dev;
        cs_ref_speed(analysis->wall_distr.mean.point,
                     analysis->wall_distr.st_dev.point,
                     best->wall_distr.mean.point, best->wall_distr.st_dev.point,
                     &ref, &ref_st_dev);
        printf("%.3f ± %.3f times faster than '%s'\n", ref, ref_st_dev,
               analysis->bench->cmd->str);
    }
}

static void
cs_print_cmd_group_analysis(const struct cs_bench_results *results) {
    for (size_t i = 0; i < results->group_count; ++i) {
        const struct cs_cmd_group_analysis *analysis =
            results->group_analyses + i;
        const struct cs_cmd_group *group = analysis->group;

        printf("command group '%s' with parameter %s\n", group->template,
               group->var_name);
        char buf[256];
        cs_print_time(buf, sizeof(buf), analysis->data[0].mean);
        printf("lowest time %s with %s=%s\n", buf, group->var_name,
               analysis->data[0].value);
        cs_print_time(buf, sizeof(buf),
                      analysis->data[analysis->cmd_count - 1].mean);
        printf("highest time %s with %s=%s\n", buf, group->var_name,
               analysis->data[analysis->cmd_count - 1].value);
        if (analysis->values_are_doubles) {
            printf("mean time is most likely %s in terms of parameter\n",
                   cs_big_o_str(analysis->complexity));
            printf("linear coef %.3f rms %.3f\n", analysis->coef,
                   analysis->rms);
        }
    }
}

static int cs_export_json(const struct cs_settings *settings,
                          const struct cs_bench_results *results,
                          const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        fprintf(stderr, "error: failed to open file '%s' for export\n",
                filename);
        return -1;
    }

    size_t bench_count = results->bench_count;
    const struct cs_bench *benches = results->benches;
    fprintf(f, "{ \"settings\": {");
    fprintf(f,
            "\"time_limit\": %f, \"runs\": %zu, \"min_runs\": %zu, "
            "\"max_runs\": %zu, \"warmup_time\": %f ",
            settings->bench_stop.time_limit, settings->bench_stop.runs,
            settings->bench_stop.min_runs, settings->bench_stop.max_runs,
            settings->warmup_time);
    fprintf(f, "}, \"benches\": [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench *bench = benches + i;
        fprintf(f, "{ ");
        fprintf(f, "\"prepare\": \"%s\", ",
                bench->prepare ? bench->prepare : "");
        fprintf(f, "\"command\": \"%s\", ", bench->cmd->str);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"wallclock\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%f%s", bench->wallclocks[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "], \"sys\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%f%s", bench->systimes[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "], \"user\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%f%s", bench->usertimes[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "], \"exit_codes\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%d%s", bench->exit_codes[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "}");
        if (i != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);

    return 0;
}

static int cs_python_found(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", "--version", NULL) == -1) {
            perror("execlp");
            _exit(-1);
        }
    }

    return cs_process_finished_correctly(pid);
}

static int cs_launch_python_stdin_pipe(FILE **inp, pid_t *pidp) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        close(pipe_fds[1]);
        close(STDIN_FILENO);
        if (dup2(pipe_fds[0], STDIN_FILENO) == -1) {
            perror("dup2");
            _exit(-1);
        }
        // we don't need any output
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", NULL) == -1) {
            perror("execlp");
            _exit(-1);
        }
    }

    close(pipe_fds[0]);
    FILE *script = fdopen(pipe_fds[1], "w");

    *pidp = pid;
    *inp = script;
    return 0;
}
static int cs_python_has_matplotlib(void) {
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid))
        return 0;

    fprintf(script, "import matplotlib.pyplot as plt\n");
    fclose(script);
    return cs_process_finished_correctly(pid);
}

__attribute__((format(printf, 2, 3))) static FILE *
cs_open_file_fmt(const char *mode, const char *fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return fopen(buf, mode);
}

static void cs_make_whisker_plot(const struct cs_bench *benches,
                                 size_t bench_count,
                                 const char *output_filename, FILE *f) {
    fprintf(f, "data = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench *bench = benches + i;
        fprintf(f, "[");
        for (size_t j = 0; j < bench->run_count; ++j)
            fprintf(f, "%f, ", bench->wallclocks[j]);
        fprintf(f, "], ");
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench *bench = benches + i;
        fprintf(f, "'%s', ", bench->cmd->str);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.xlabel('command')\n"
            "plt.ylabel('time [s]')\n"
            "plt.boxplot(data)\n"
            "plt.xticks(list(range(1, len(names) + 1)), names)\n"
            "plt.savefig('%s')\n",
            output_filename);
}

static void cs_make_violin_plot(const struct cs_bench *benches,
                                size_t bench_count, const char *output_filename,
                                FILE *f) {
    fprintf(f, "data = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench *bench = benches + i;
        fprintf(f, "[");
        for (size_t j = 0; j < bench->run_count; ++j)
            fprintf(f, "%f, ", bench->wallclocks[j]);
        fprintf(f, "], ");
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench *bench = benches + i;
        fprintf(f, "'%s', ", bench->cmd->str);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.xlabel('command')\n"
            "plt.ylabel('time [s]')\n"
            "plt.violinplot(data)\n"
            "plt.xticks(list(range(1, len(names) + 1)), names)\n"
            "plt.savefig('%s')\n",
            output_filename);
}

static void cs_make_group_plot(const struct cs_cmd_group_analysis *analysis,
                               const char *output_filename, FILE *f) {
    fprintf(f, "x = [");
    for (size_t i = 0; i < analysis->cmd_count; ++i)
        fprintf(f, "%f, ", analysis->data[i].value_double);
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t i = 0; i < analysis->cmd_count; ++i)
        fprintf(f, "%f, ", analysis->data[i].mean);
    fprintf(f, "]\n");
    fprintf(f, "regr = [");
    for (size_t i = 0; i < analysis->cmd_count; ++i) {
        double v =
            analysis->coef * cs_fitting_curve(analysis->data[i].value_double,
                                              analysis->complexity);
        fprintf(f, "%f, ", v);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.title('%s')\n"
            "plt.plot(x, regr, color='red', alpha=0.3)\n"
            "plt.plot(x, y, '.-')\n"
            "plt.xticks(x)\n"
            "plt.grid()\n"
            "plt.xlabel('%s')\n"
            "plt.ylabel('time [s]')\n"
            "plt.savefig('%s')\n",
            analysis->group->template, analysis->group->var_name,
            output_filename);
}

static void cs_construct_kde(const struct cs_distr *distr, double *kde,
                             size_t kde_size, int is_ext, double *lowerp,
                             double *stepp) {
    size_t count = distr->count;
    double st_dev = distr->st_dev.point;
    double mean = distr->mean.point;
    double iqr = distr->q3 - distr->q1;
    double h = 0.9 * fmin(st_dev, iqr / 1.34) * pow(count, -0.2);

    double lower, upper;
    // just some empyrically selected values plugged here
    if (!is_ext) {
        lower = fmax(mean - 3.0 * st_dev, distr->p5);
        upper = fmin(mean + 3.0 * st_dev, distr->p95);
    } else {
        lower = fmax(mean - 6.0 * st_dev, distr->p1);
        upper = fmin(mean + 6.0 * st_dev, distr->p99);
    }
    double step = (upper - lower) / kde_size;
    double k_mult = 1.0 / sqrt(2.0 * 3.1415926536);
    for (size_t i = 0; i < kde_size; ++i) {
        double x = lower + i * step;
        double kde_value = 0.0;
        for (size_t j = 0; j < count; ++j) {
            double u = (x - distr->data[j]) / h;
            double k = k_mult * exp(-0.5 * u * u);
            kde_value += k;
        }
        kde_value /= count * h;
        kde[i] = kde_value;
    }

    *lowerp = lower;
    *stepp = step;
}
static void cs_init_kde_plot(const struct cs_distr *distr, int is_ext,
                             struct cs_kde_plot *plot) {
    size_t kde_points = 200;
    plot->distr = distr;
    plot->count = kde_points;
    plot->data = malloc(sizeof(*plot->data) * plot->count);
    cs_construct_kde(distr, plot->data, plot->count, is_ext, &plot->lower,
                     &plot->step);
    plot->mean = distr->mean.point;

    // linear interpolate between adjacent points to find height of line
    // with x equal mean
    double x = plot->mean;
    for (size_t i = 0; i < plot->count - 1; ++i) {
        double x1 = plot->lower + i * plot->step;
        double x2 = plot->lower + (i + 1) * plot->step;
        if (x1 <= x && x <= x2) {
            double y1 = plot->data[i];
            double y2 = plot->data[i + 1];
            plot->mean_y = (y1 * (x2 - x) + y2 * (x - x1)) / (x2 - x1);
        }
    }
}
static void cs_make_kde_plot(const struct cs_kde_plot *plot, FILE *f) {
    fprintf(f, "y = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%f, ", plot->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%f, ", plot->lower + plot->step * i);
    fprintf(f, "]\n");

    fprintf(f,
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.title('%s')\n"
            "plt.fill_between(x, y, interpolate=True, alpha=0.25)\n"
            "plt.vlines(%f, [0], [%f])\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s')\n",
            plot->title, plot->mean, plot->mean_y, plot->xlabel,
            plot->output_filename);
}
static void cs_make_kde_plot_ext(const struct cs_kde_plot *plot, FILE *f) {
    double max_y = 0;
    for (size_t i = 0; i < plot->count; ++i)
        if (plot->data[i] > max_y)
            max_y = plot->data[i];
    double max_point_x = 0;
    fprintf(f, "points = [");
    for (size_t i = 0; i < plot->distr->count; ++i) {
        double v = plot->distr->data[i];
        if (v < plot->lower || v > plot->lower + plot->step * plot->count)
            continue;
        if (v > max_point_x)
            max_point_x = v;
        fprintf(f, "(%f, %f), ", v,
                (double)(i + 1) / plot->distr->count * max_y);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "severe_points = list(filter(lambda x: x[0] < %f or x[0] > %f, "
            "points))\n",
            plot->distr->outliers.low_severe_x,
            plot->distr->outliers.high_severe_x);
    fprintf(
        f,
        "mild_points = list(filter(lambda x: (%f < x[0] < %f) or (%f < x[0] < "
        "%f), points))\n",
        plot->distr->outliers.low_severe_x, plot->distr->outliers.low_mild_x,
        plot->distr->outliers.high_mild_x, plot->distr->outliers.high_severe_x);
    fprintf(f, "reg_points = list(filter(lambda x: %f < x[0] < %f, points))\n",
            plot->distr->outliers.low_mild_x,
            plot->distr->outliers.high_mild_x);
    size_t kde_count = 0;
    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i, ++kde_count) {
        double x = plot->lower + plot->step * i;
        if (x > max_point_x)
            break;
        fprintf(f, "%f, ", x);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t i = 0; i < kde_count; ++i)
        fprintf(f, "%f, ", plot->data[i]);
    fprintf(f, "]\n");

    fprintf(f,
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.title('%s')\n"
            "plt.fill_between(x, y, interpolate=True, alpha=0.25)\n"
            "plt.plot(*zip(*severe_points), marker='o', ls='', markersize=2, "
            "color='red')\n"
            "plt.plot(*zip(*mild_points), marker='o', ls='', markersize=2, "
            "color='orange')\n"
            "plt.plot(*zip(*reg_points), marker='o', ls='', markersize=2)\n"
            "plt.axvline(x=%f)\n",
            plot->title, plot->mean);
    if (plot->distr->outliers.low_mild_x > plot->lower)
        fprintf(f, "plt.axvline(x=%f, color='orange')\n",
                plot->distr->outliers.low_mild_x);
    if (plot->distr->outliers.low_severe_x > plot->lower)
        fprintf(f, "plt.axvline(x=%f, color='red')\n",
                plot->distr->outliers.low_severe_x);
    if (plot->distr->outliers.high_mild_x <
        plot->lower + plot->count * plot->step)
        fprintf(f, "plt.axvline(x=%f, color='orange')\n",
                plot->distr->outliers.high_mild_x);
    if (plot->distr->outliers.high_severe_x <
        plot->lower + plot->count * plot->step)
        fprintf(f, "plt.axvline(x=%f, color='red')\n",
                plot->distr->outliers.high_severe_x);
    fprintf(f,
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s')\n"
            "plt.ylabel('runs')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->xlabel, plot->output_filename);
}

static void cs_free_kde_plot(struct cs_kde_plot *plot) { free(plot->data); }

static int cs_whisker_plot(const struct cs_bench *benches, size_t bench_count,
                           const char *output_filename) {
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        return -1;
    }
    cs_make_whisker_plot(benches, bench_count, output_filename, script);
    fclose(script);
    if (!cs_process_finished_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        return -1;
    }
    return 0;
}

static int cs_violin_plot(const struct cs_bench *benches, size_t bench_count,
                          const char *output_filename) {
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        return -1;
    }
    cs_make_violin_plot(benches, bench_count, output_filename, script);
    fclose(script);
    if (!cs_process_finished_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        return -1;
    }
    return 0;
}

static int cs_group_plot(const struct cs_cmd_group_analysis *analysis,
                         const char *output_filename) {
    int ret = 0;
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        return -1;
    }
    cs_make_group_plot(analysis, output_filename, script);
    fclose(script);
    if (!cs_process_finished_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        return -1;
    }

    return ret;
}

static int cs_kde_plot(const struct cs_distr *distr, const char *title,
                       const char *xlabel, const char *output_filename,
                       int is_ext) {
    int ret = 0;
    struct cs_kde_plot plot = {0};
    plot.output_filename = output_filename;
    plot.title = title;
    plot.xlabel = xlabel;
    cs_init_kde_plot(distr, is_ext, &plot);
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        ret = -1;
        goto out;
    }
    if (is_ext)
        cs_make_kde_plot_ext(&plot, script);
    else
        cs_make_kde_plot(&plot, script);
    fclose(script);
    if (!cs_process_finished_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        ret = -1;
    }
out:
    cs_free_kde_plot(&plot);
    return ret;
}

static int cs_dump_plot_src(const struct cs_bench_results *results, int no_time,
                            const char *analyze_dir) {
    size_t bench_count = results->bench_count;
    const struct cs_bench *benches = results->benches;
    char buf[4096];
    FILE *f;

    // FIXME: file name mismatch with plots
    if (!no_time) {
        f = cs_open_file_fmt("w", "%s/whisker.py", analyze_dir);
        if (f == NULL) {
            fprintf(stderr, "error: failed to create file %s/whisker.py\n",
                    analyze_dir);
            return -1;
        }
        snprintf(buf, sizeof(buf), "%s/whisker.svg", analyze_dir);
        cs_make_whisker_plot(benches, bench_count, buf, f);
        fclose(f);

        f = cs_open_file_fmt("w", "%s/violin.py", analyze_dir);
        if (f == NULL) {
            fprintf(stderr, "error: failed to create file %s/violin.py\n",
                    analyze_dir);
            return -1;
        }
        snprintf(buf, sizeof(buf), "%s/violin.svg", analyze_dir);
        cs_make_violin_plot(benches, bench_count, buf, f);
        fclose(f);
        for (size_t i = 0; i < results->group_count; ++i) {
            const struct cs_cmd_group_analysis *analysis =
                results->group_analyses + i;
            f = cs_open_file_fmt("w", "%s/group_plot_%zu.py", analyze_dir,
                                 i + 1);
            if (f == NULL) {
                fprintf(stderr,
                        "error: failed to create file %s/group_plot_%zu.py\n",
                        analyze_dir, i + 1);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/group_plot_%zu.svg", analyze_dir,
                     i + 1);
            cs_make_group_plot(analysis, buf, f);
            fclose(f);
        }
        for (size_t i = 0; i < bench_count; ++i) {
            const struct cs_bench *bench = benches + i;
            const struct cs_bench_analysis *analysis = results->analyses + i;
            struct cs_kde_plot plot = {0};
            plot.output_filename = buf;
            plot.title = bench->cmd->str;
            plot.xlabel = "time [s]";

            f = cs_open_file_fmt("w", "%s/kde_%zu.py", analyze_dir, i + 1);
            if (f == NULL) {
                fprintf(stderr, "error: failed to create file %s/kde_%zu.py\n",
                        analyze_dir, i + 1);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/kde_%zu.svg", analyze_dir, i + 1);
            cs_init_kde_plot(&analysis->wall_distr, 0, &plot);
            cs_make_kde_plot(&plot, f);
            cs_free_kde_plot(&plot);
            fclose(f);

            f = cs_open_file_fmt("w", "%s/kde_ext_%zu.py", analyze_dir, i + 1);
            if (f == NULL) {
                fprintf(stderr,
                        "error: failed to create file %s/kde_ext_%zu.py\n",
                        analyze_dir, i + 1);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/kde_ext_%zu.svg", analyze_dir,
                     i + 1);
            cs_init_kde_plot(&analysis->wall_distr, 1, &plot);
            cs_make_kde_plot_ext(&plot, f);
            cs_free_kde_plot(&plot);
            fclose(f);
        }
    }
    // TODO: custom meas

    return 0;
}

static int cs_make_plots(const struct cs_bench_results *results, int no_time,
                         const char *analyze_dir) {
    size_t bench_count = results->bench_count;
    const struct cs_bench *benches = results->benches;
    const struct cs_bench_analysis *analyses = results->analyses;
    char buf[4096];

#define cs_make_kdes(_distr, _title, _xlabel, _name)                           \
    do {                                                                       \
        snprintf(buf, sizeof(buf), "%s/kde_%zu_%s.svg", analyze_dir, i + 1,    \
                 _name);                                                       \
        if (cs_kde_plot(_distr, _title, _xlabel, buf, 0) == -1)                \
            return -1;                                                         \
        snprintf(buf, sizeof(buf), "%s/kde_ext_%zu_%s.svg", analyze_dir,       \
                 i + 1, _name);                                                \
        if (cs_kde_plot(_distr, _title, _xlabel, buf, 1) == -1)                \
            return -1;                                                         \
    } while (0)

    if (!no_time) {
        snprintf(buf, sizeof(buf), "%s/whisker.svg", analyze_dir);
        if (cs_whisker_plot(benches, bench_count, buf) == -1)
            return -1;

        snprintf(buf, sizeof(buf), "%s/violin.svg", analyze_dir);
        if (cs_violin_plot(benches, bench_count, buf) == -1)
            return -1;

        for (size_t i = 0; i < results->group_count; ++i) {
            const struct cs_cmd_group_analysis *analysis =
                results->group_analyses + i;
            snprintf(buf, sizeof(buf), "%s/group_plot_%zu.svg", analyze_dir,
                     i + 1);
            if (cs_group_plot(analysis, buf) == -1)
                return -1;
        }
        for (size_t i = 0; i < bench_count; ++i) {
            const struct cs_bench_analysis *analysis = analyses + i;
            const char *cmd_str = analysis->bench->cmd->str;
            cs_make_kdes(&analysis->wall_distr, cmd_str, "time [s]", "wall");
        }
    }

    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench_analysis *analysis = analyses + i;
        const char *cmd_str = analysis->bench->cmd->str;
        for (size_t j = 0; j < analysis->bench->custom_meas_count; ++j) {
            const struct cs_custom_meas *meas =
                analysis->bench->cmd->custom_meas + j;
            char buf1[256], buf2[256];
            snprintf(buf1, sizeof(buf1), "%s by %s", cmd_str, meas->name);
            if (meas->units)
                snprintf(buf2, sizeof(buf2), "%s [%s]", meas->name,
                         meas->units);
            else
                snprintf(buf2, sizeof(buf2), "%s [s]", meas->name);

            cs_make_kdes(analysis->custom_meas + j, buf1, buf2, meas->name);
        }
    }
#undef cs_make_kdes

    return 0;
}

static void cs_html_time_estimate(const char *name, const struct cs_est *est,
                                  FILE *f) {
    char buf1[256], buf2[256], buf3[256];
    cs_print_time(buf1, sizeof(buf1), est->lower);
    cs_print_time(buf2, sizeof(buf2), est->point);
    cs_print_time(buf3, sizeof(buf3), est->upper);
    fprintf(f,
            "<tr>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "</tr>",
            name, buf1, buf2, buf3);
}

static void cs_html_outliers(const struct cs_outliers *outliers,
                             double outlier_var, size_t run_count, FILE *f) {
    size_t outlier_count = outliers->low_mild + outliers->high_mild +
                           outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        fprintf(f, "<p>found %zu outliers (%.2f%%)</p><ul>", outlier_count,
                (double)outlier_count / run_count * 100.0);
        if (outliers->low_severe)
            fprintf(f, "<li>%zu (%.2f%%) low severe</li>", outliers->low_severe,
                    (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            fprintf(f, "<li>%zu (%.2f%%) low mild</li>", outliers->low_mild,
                    (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            fprintf(f, "<li>%zu (%.2f%%) high mild</li>", outliers->high_mild,
                    (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            fprintf(f, "<li>%zu (%.2f%%) high severe</li>",
                    outliers->high_severe,
                    (double)outliers->high_severe / run_count * 100.0);
        fprintf(f, "</ul>");
    }
    fprintf(f,
            "<p>outlying measurements have %s (%.1f%%) effect on "
            "estimated "
            "standard deviation</p>",
            cs_outliers_variance_str(outlier_var), outlier_var * 100.0);
}

static void cs_html_wall_distr(const struct cs_bench_analysis *analysis,
                               size_t i, FILE *f) {
    const struct cs_bench *bench = analysis->bench;
    fprintf(f,
            "<div class=\"row\">"
            "<div class=\"col\"><h3>time kde plot</h3>"
            "<a href=\"kde_ext_%zu_wall.svg\"><img "
            "src=\"kde_%zu_wall.svg\"></a></div>",
            i, i);
    fprintf(f,
            "<div class=\"col\"><h3>statistics</h3>"
            "<div class=\"stats\">"
            "<p>%zu runs</p>",
            bench->run_count);
    char buf[256];
    cs_print_time(buf, sizeof(buf), analysis->wall_distr.min);
    fprintf(f, "<p>min %s</p>", buf);
    cs_print_time(buf, sizeof(buf), analysis->wall_distr.max);
    fprintf(f, "<p>max %s</p>", buf);
    fprintf(f, "<table><thead><tr>"
               "<th></th>"
               "<th class=\"est-bound\">lower bound</th>"
               "<th class=\"est-bound\">estimate</th>"
               "<th class=\"est-bound\">upper bound</th>"
               "</tr></thead><tbody>");
    cs_html_time_estimate("mean", &analysis->wall_distr.mean, f);
    cs_html_time_estimate("st dev", &analysis->wall_distr.st_dev, f);
    cs_html_time_estimate("systime", &analysis->systime_est, f);
    cs_html_time_estimate("usrtime", &analysis->usertime_est, f);
    fprintf(f, "</tbody></table>");
    cs_html_outliers(&analysis->wall_distr.outliers,
                     analysis->wall_distr.outlier_var, bench->run_count, f);
    fprintf(f, "</div></div></div>");
}

static void cs_html_estimate(const char *name, const struct cs_est *est,
                             FILE *f) {
    fprintf(f,
            "<tr>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%.5g</td>"
            "<td>%.5g</td>"
            "<td class=\"est-bound\">%.5g</td>"
            "</tr>",
            name, est->lower, est->point, est->upper);
}

static void cs_html_custom_distr(const struct cs_bench *bench,
                                 const struct cs_distr *distr,
                                 const struct cs_custom_meas *info, size_t i,
                                 FILE *f) {
    fprintf(f,
            "<div class=\"row\">"
            "<div class=\"col\"><h3>%s kde plot</h3>"
            "<a href=\"kde_ext_%zu_%s.svg\"><img "
            "src=\"kde_%zu_%s.svg\"></a></div>",
            info->name, i, info->name, i, info->name);
    fprintf(f,
            "<div class=\"col\"><h3>statistics</h3>"
            "<div class=\"stats\">"
            "<p>%zu runs</p>",
            bench->run_count);
    char buf[256];
    if (info->units == NULL)
        cs_print_time(buf, sizeof(buf), distr->min);
    else
        snprintf(buf, sizeof(buf), "%.5g %s", distr->min, info->units);
    fprintf(f, "<p>min %s</p>", buf);
    if (info->units == NULL)
        cs_print_time(buf, sizeof(buf), distr->min);
    else
        snprintf(buf, sizeof(buf), "%.5g", distr->min);
    fprintf(f, "<p>max %s</p>", buf);
    fprintf(f, "<table><thead><tr>"
               "<th></th>"
               "<th class=\"est-bound\">lower bound</th>"
               "<th class=\"est-bound\">estimate</th>"
               "<th class=\"est-bound\">upper bound</th>"
               "</tr></thead><tbody>");
    cs_html_estimate("mean", &distr->mean, f);
    cs_html_estimate("st dev", &distr->st_dev, f);
    fprintf(f, "</tbody></table>");
    cs_html_outliers(&distr->outliers, distr->outlier_var, bench->run_count, f);
    fprintf(f, "</div></div></div>");
}

static void cs_html_compare(const struct cs_bench_analysis *best,
                            const struct cs_bench_analysis *analyses,
                            size_t bench_count, FILE *f) {
    fprintf(f,
            "<h2>comparison</h2>"
            "<div class=\"row\"><div class=\"col\">"
            "<img src=\"violin.svg\"></div>"
            "<div class=\"col stats\"><p>fastest command '%s'</p><ul>",
            best->bench->cmd->str);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_bench_analysis *analysis = analyses + i;
        if (analysis == best)
            continue;

        double ref, ref_st_dev;
        cs_ref_speed(analysis->wall_distr.mean.point,
                     analysis->wall_distr.st_dev.point,
                     best->wall_distr.mean.point, best->wall_distr.st_dev.point,
                     &ref, &ref_st_dev);
        fprintf(f, "<li>%.3f ± %.3f times faster than '%s'</li>", ref,
                ref_st_dev, analysis->bench->cmd->str);
    }
    fprintf(f, "</ul></div></div>");
}

static void cs_html_cmd_group(const struct cs_cmd_group_analysis *analysis,
                              size_t i, FILE *f) {
    const struct cs_cmd_group *group = analysis->group;
    fprintf(f,
            "<h2>group '%s' with parameter %s</h2>"
            "<div class=\"row\"><div class=\"col\">"
            "<img src=\"group_plot_%zu.svg\"></div>",
            group->template, group->var_name, i);
    char buf[256];
    cs_print_time(buf, sizeof(buf), analysis->data[0].mean);
    fprintf(f,
            "<div class=\"col stats\">"
            "<p>lowest time %s with %s=%s</p>",
            buf, group->var_name, analysis->data[0].value);
    cs_print_time(buf, sizeof(buf),
                  analysis->data[analysis->cmd_count - 1].mean);
    fprintf(f, "<p>hightest time %s with %s=%s</p>", buf, group->var_name,
            analysis->data[analysis->cmd_count - 1].value);
    if (analysis->values_are_doubles) {
        fprintf(f,
                "<p>mean time is most likely %s in terms of parameter</p>"
                "<p>linear coef %.3f rms %.3f</p>",
                cs_big_o_str(analysis->complexity), analysis->coef,
                analysis->rms);
    }
    fprintf(f, "</div></div>");
}

static void cs_html_report(const struct cs_bench_results *results, int no_time,
                           FILE *f) {
    fprintf(f,
            "<!DOCTYPE html><html lang=\"en\">"
            "<head><meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, "
            "initial-scale=1.0\">"
            "<title>csbench</title>"
            "<style>body { margin: 40px auto; max-width: 960px; line-height: "
            "1.6; color: #444; padding: 0 10px; font: 14px Helvetica Neue }"
            "h1, h2, h3 { line-height: 1.2; text-align: center }"
            ".est-bound { opacity: 0.5 }"
            "th, td { padding-right: 3px; padding-bottom: 3px }"
            "th { font-weight: 200 }"
            ".col { flex: 50%% }"
            ".row { display: flex }"
            ".stats { margin-top: 80px }"
            "</style></head>");
    fprintf(f, "<body>");
    for (size_t i = 0; i < results->bench_count; ++i) {
        const struct cs_bench *bench = results->benches + i;
        const struct cs_bench_analysis *analysis = results->analyses + i;
        fprintf(f, "<div><h2>command '%s'</h2>", bench->cmd->str);
        if (!no_time)
            cs_html_wall_distr(analysis, i + 1, f);
        for (size_t j = 0; j < bench->custom_meas_count; ++j) {
            const struct cs_distr *distr = analysis->custom_meas + j;
            const struct cs_custom_meas *info = bench->cmd->custom_meas + j;
            cs_html_custom_distr(bench, distr, info, i + 1, f);
        }
        fprintf(f, "</div>");
    }

    if (results->bench_count != 1 && !no_time) {
        size_t best_idx = results->fastest_idx;
        const struct cs_bench_analysis *best = results->analyses + best_idx;
        cs_html_compare(best, results->analyses, results->bench_count, f);
    }

    for (size_t i = 0; i < results->group_count; ++i) {
        const struct cs_cmd_group_analysis *analysis =
            results->group_analyses + i;
        cs_html_cmd_group(analysis, i + 1, f);
    }
    fprintf(f, "</body>");
}

static int cs_run_benches(const struct cs_settings *settings,
                          struct cs_bench_results *results) {
    results->bench_count = cs_sb_len(settings->cmds);
    results->benches = calloc(results->bench_count, sizeof(*results->benches));
    for (size_t i = 0; i < results->bench_count; ++i) {
        struct cs_bench *bench = results->benches + i;
        bench->prepare = settings->prepare_cmd;
        bench->cmd = settings->cmds + i;
        bench->custom_meas_count = cs_sb_len(bench->cmd->custom_meas);
        if (bench->custom_meas_count) {
            bench->custom_meas =
                calloc(bench->custom_meas_count, sizeof(*bench->custom_meas));
        }

        if (cs_warmup(bench->cmd, settings->warmup_time) == -1)
            return -1;
        if (cs_run_benchmark(bench, &settings->bench_stop) == -1)
            return -1;
    }

    return 0;
}

static void cs_analyze_benches(const struct cs_settings *settings,
                               struct cs_bench_results *results) {
    results->analyses =
        calloc(results->bench_count, sizeof(*results->analyses));
    for (size_t i = 0; i < results->bench_count; ++i) {
        struct cs_bench *bench = results->benches + i;
        struct cs_bench_analysis *analysis = results->analyses + i;
        if (bench->custom_meas_count != 0) {
            analysis->custom_meas = calloc(bench->custom_meas_count,
                                           sizeof(*analysis->custom_meas));
        }

        cs_analyze_benchmark(bench, settings->nresamp, analysis);
    }

    cs_compare_benches(results);
    cs_analyze_cmd_groups(settings, results);
}

static void cs_print_analysis(const struct cs_bench_results *results,
                              int no_time) {
    for (size_t i = 0; i < results->bench_count; ++i)
        cs_print_benchmark_info(results->analyses + i, no_time);

    cs_print_cmd_comparison(results);
    cs_print_cmd_group_analysis(results);
}

static int cs_handle_export(const struct cs_settings *settings,
                            const struct cs_bench_results *results) {
    switch (settings->export.kind) {
    case CS_EXPORT_JSON:
        return cs_export_json(settings, results, settings->export.filename);
        break;
    case CS_DONT_EXPORT:
        break;
    }
    return 0;
}

static int cs_make_html_report(const struct cs_bench_results *results,
                               int no_time, const char *analyze_dir) {
    FILE *f = cs_open_file_fmt("w", "%s/index.html", analyze_dir);
    if (f == NULL) {
        fprintf(stderr, "error: failed to create file %s/index.html\n",
                analyze_dir);
        return -1;
    }
    cs_html_report(results, no_time, f);
    fclose(f);
    return 0;
}

static int cs_handle_analyze(const struct cs_bench_results *results,
                             enum cs_analyze_mode mode, const char *analyze_dir,
                             int plot_src, int no_time) {
    if (mode == CS_DONT_ANALYZE)
        return 0;

    if (mkdir(analyze_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            perror("mkdir");
            return -1;
        }
    }

    if (mode == CS_ANALYZE_PLOT || mode == CS_ANALYZE_HTML) {
        if (!cs_python_found()) {
            fprintf(stderr, "error: failed to find python3 executable\n");
            return -1;
        }
        if (!cs_python_has_matplotlib()) {
            fprintf(stderr,
                    "error: python does not have matplotlib installed\n");
            return -1;
        }

        if (plot_src && cs_dump_plot_src(results, no_time, analyze_dir) == -1)
            return -1;

        if (cs_make_plots(results, no_time, analyze_dir) == -1)
            return -1;
    }

    if (mode == CS_ANALYZE_HTML &&
        cs_make_html_report(results, no_time, analyze_dir) == -1)
        return -1;

    return 0;
}

static void cs_free_bench_results(struct cs_bench_results *results) {
    // these ifs are needed because results can be partially initialized in
    // case of failure
    if (results->benches) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            struct cs_bench *bench = results->benches + i;
            cs_sb_free(bench->wallclocks);
            cs_sb_free(bench->systimes);
            cs_sb_free(bench->usertimes);
            cs_sb_free(bench->exit_codes);
            if (bench->custom_meas) {
                for (size_t i = 0; i < bench->custom_meas_count; ++i)
                    cs_sb_free(bench->custom_meas[i]);
                free(bench->custom_meas);
            }
        }
    }
    if (results->analyses) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            const struct cs_bench_analysis *analysis = results->analyses + i;
            free(analysis->custom_meas);
        }
    }
    for (size_t i = 0; i < results->group_count; ++i) {
        struct cs_cmd_group_analysis *analysis = results->group_analyses + i;
        free(analysis->data);
    }

    free(results->benches);
    free(results->analyses);
    free(results->group_analyses);
}

static int cs_run(const struct cs_settings *settings) {
    int ret = -1;
    struct cs_bench_results results = {0};
    if (cs_run_benches(settings, &results) == -1)
        goto out;
    cs_analyze_benches(settings, &results);
    cs_print_analysis(&results, settings->no_time);
    if (cs_handle_export(settings, &results) == -1)
        goto out;
    if (cs_handle_analyze(&results, settings->analyze_mode,
                          settings->analyze_dir, settings->plot_src,
                          settings->no_time) == -1)
        goto out;

    ret = 0;
out:
    cs_free_bench_results(&results);
    return ret;
}

int main(int argc, char **argv) {
    int rc = EXIT_FAILURE;
    struct cs_cli_settings cli = {0};
    cs_parse_cli_args(argc, argv, &cli);

    struct cs_settings settings = {0};
    if (cs_init_settings(&cli, &settings) == -1)
        goto free_cli;

    rng_state = time(NULL);
    if (cs_run(&settings) == -1)
        goto free_settings;

    rc = EXIT_SUCCESS;
free_settings:
    cs_free_settings(&settings);
free_cli:
    cs_free_cli_settings(&cli);
    return rc;
}
