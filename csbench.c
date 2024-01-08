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

struct cs_benchmark_stop_policy {
    double time_limit;
    size_t runs;
    size_t min_runs;
    size_t max_runs;
};

struct cs_custom_meassurement {
    const char *name;
    const char *str;
};

struct cs_benchmark_param {
    char *name;
    char **values;
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cs_cli_settings {
    const char **commands;
    struct cs_benchmark_stop_policy bench_stop;
    double warmup_time;
    size_t nresamples;
    const char *shell;
    struct cs_export_policy export;
    struct cs_custom_meassurement *custom_measuremements;
    const char *prepare;
    struct cs_input_policy input_policy;
    enum cs_output_policy_kind output_policy;
    const char *analyze_dir;
    enum cs_analyze_mode analyze_mode;
    struct cs_benchmark_param *params;
};

// Description of command to benchmark.
// Commands are executed using execve.
struct cs_command {
    char *str;
    char *executable;
    char **argv;
    struct cs_input_policy input_policy;
    enum cs_output_policy_kind output_policy;
    struct cs_custom_meassurement *custom_measuremements;
};

struct cs_command_group {
    char *template;
    size_t *command_idxs;
    const char **var_values;
    const char *var_name;
};

// Information gethered from user input (settings), parsed
// and prepared for benchmarking.
struct cs_settings {
    struct cs_command *commands;
    struct cs_command_group *command_groups;
    struct cs_benchmark_stop_policy bench_stop;
    double warmup_time;
    size_t nresamples;
    struct cs_custom_meassurement *custom_measuremements;
    const char *prepare_command;
    struct cs_export_policy export;
    enum cs_analyze_mode analyze_mode;
    const char *analyze_dir;
};

// Boostrap estimate of certain statistic. Contains lower and upper bounds, as
// well as point estimate. Point estimate is commonly obtained from statistic
// calculation over original data, while lower and upper bounds are obtained
// using bootstrapping.
struct cs_estimate {
    double lower;
    double point;
    double upper;
};

// How outliers affect standard deviation
enum cs_outlier_effect {
    CS_OUTLIERS_UNAFFECTED,
    CS_OUTLIERS_SLIGHT,
    CS_OUTLIERS_MODERATE,
    CS_OUTLIERS_SEVERE
};

struct cs_outlier_variance {
    enum cs_outlier_effect effect;
    const char *desc;
    double fraction;
};

struct cs_outliers {
    size_t low_severe;
    size_t low_mild;
    size_t high_mild;
    size_t high_severe;
};

struct cs_benchmark {
    // These fields are input options
    const char *prepare;
    const struct cs_command *command;
    // These fields are collected data
    size_t run_count;
    double *wallclock_sample;
    double *systime_sample;
    double *usertime_sample;
    int *exit_codes;
    double **custom_measurements;
};

struct cs_benchmark_analysis {
    struct cs_estimate mean_estimate;
    struct cs_estimate st_dev_estimate;
    struct cs_estimate systime_estimate;
    struct cs_estimate usertime_estimate;
    struct cs_estimate *custom_measurement_mean_estimates;
    struct cs_estimate *custom_measurement_st_dev_estimates;
    struct cs_outliers outliers;
    double outlier_variance_fraction;
};

struct cs_benchmark_results {
    size_t bench_count;
    struct cs_benchmark *benches;
    struct cs_benchmark_analysis *analyses;
};

struct cs_cpu_time {
    double user_time;
    double system_time;
};

// data needed to construct whisker (boxplot) or violin plot
struct cs_whisker_plot {
    double **data;
    const char **column_names;
    size_t *widths;
    size_t column_count;
    const char *output_filename;
};

// data needed to construct kde plot. data here is kde points computed from
// original data
struct cs_kde_plot {
    const char *title;
    double lower;
    double step;
    double *data;
    size_t count;
    double mean;
    double mean_y;
    const char *output_filename;
};

enum cs_big_o {
    CS_O_1,
    CS_O_N,
    CS_O_N_SQ,
    CS_O_N_CUBE,
    CS_O_LOGN,
    CS_O_NLOGN,
};

struct cs_command_in_group_data {
    const char *value;
    double value_double;
    double mean;
};

//
// cs_sb interface
//

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

static void *
cs_sb_grow_impl(void *arr, size_t inc, size_t stride) {
    if (arr == NULL) {
        void *result = calloc(sizeof(struct cs_sb_header) + stride * inc, 1);
        struct cs_sb_header *header = result;
        header->size = 0;
        header->capacity = inc;
        return header + 1;
    }

    struct cs_sb_header *header = cs_sb_header(arr);
    size_t double_current = header->capacity * 2;
    size_t min_needed = header->size + inc;

    size_t new_capacity =
        double_current > min_needed ? double_current : min_needed;
    void *result =
        realloc(header, sizeof(struct cs_sb_header) + stride * new_capacity);
    header = result;
    header->capacity = new_capacity;
    return header + 1;
}

#if defined(__APPLE__)
static double
cs_get_time(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
#else
static double
cs_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif
static struct cs_cpu_time
cs_getcputime(void) {
    struct rusage rus = {0};
    getrusage(RUSAGE_CHILDREN, &rus);
    struct cs_cpu_time time;
    time.user_time = rus.ru_utime.tv_sec + (double)rus.ru_utime.tv_usec / 1e6;
    time.system_time = rus.ru_stime.tv_sec + (double)rus.ru_stime.tv_usec / 1e6;
    return time;
}

static void
cs_print_help_and_exit(int rc) {
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
        "--custom <name>      - benchmark custom measurement with given name. "
        "By default uses stdout of command to retrieve number\n"
        "--custom-x <name> <cmd> - command to extract custom measurement "
        "value\n"
        "--scan <i>/<n>/<m>[/<s>] - parameter scan i in range(n, m, s). s can "
        "be omitted\n"
        "--scanl <i>/a[,...]  - parameter scacn comma separated options\n"
        "--export-json <file> - export benchmark results to json\n"
        "--analyze-dir <dir>  - directory where analysis will be saved at\n"
        "--analyze <opt>      - more complex analysis. <opt> can be one of\n"
        "   plot        - make plots as images\n"
        "   html        - make html report\n"
        "--help               - print this message\n"
        "--version            - print version\n");
    exit(rc);
}

static void
cs_print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static int
cs_parse_range_scan_settings(const char *settings, char **namep, double *lowp,
                             double *highp, double *stepp) {
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

static char **
cs_range_to_param_list(double low, double high, double step) {
    assert(high > low);
    char **result = NULL;
    for (double cursor = low; cursor <= high; cursor += step) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%f", cursor);
        cs_sb_push(result, strdup(buf));
    }

    return result;
}

static int
cs_parse_scan_list_settings(const char *settings, char **namep,
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

static char **
cs_parse_scan_list(const char *scan_list) {
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

static void
cs_parse_cli_args(int argc, char **argv, struct cs_cli_settings *settings) {
    settings->bench_stop.time_limit = 5.0;
    settings->bench_stop.min_runs = 5;
    settings->warmup_time = 1.0;
    settings->nresamples = 100000;
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
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value < 0.0) {
                fprintf(stderr,
                        "error: time limit must be positive number or zero\n");
                exit(EXIT_FAILURE);
            }

            settings->warmup_time = value;
        } else if (strcmp(opt, "--time-limit") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value <= 0.0) {
                fprintf(stderr, "error: time limit must be positive number\n");
                exit(EXIT_FAILURE);
            }

            settings->bench_stop.time_limit = value;
        } else if (strcmp(opt, "--runs") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value <= 0) {
                fprintf(stderr, "error: run count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.runs = value;
        } else if (strcmp(opt, "--min-runs") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.min_runs = value;
        } else if (strcmp(opt, "--max-runs") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            long value = strtol(runs_str, &str_end, 10);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->bench_stop.max_runs = value;
        } else if (strcmp(opt, "--prepare") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *prepare_str = argv[cursor++];
            settings->prepare = prepare_str;
        } else if (strcmp(opt, "--nrs") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *resamples_str = argv[cursor++];
            char *str_end;
            long value = strtol(resamples_str, &str_end, 10);
            if (str_end == resamples_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value <= 0) {
                fprintf(stderr,
                        "error: resamples count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            settings->nresamples = value;
        } else if (strcmp(opt, "--shell") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);
            const char *shell = argv[cursor++];
            if (strcmp(shell, "none") == 0)
                settings->shell = NULL;
            else
                settings->shell = shell;
        } else if (strcmp(opt, "--output") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *out = argv[cursor++];
            if (strcmp(out, "null") == 0)
                settings->output_policy = CS_OUTPUT_POLICY_NULL;
            else if (strcmp(out, "inherit") == 0)
                settings->output_policy = CS_OUTPUT_POLICY_INHERIT;
            else
                cs_print_help_and_exit(EXIT_FAILURE);
        } else if (strcmp(opt, "--input") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *input = argv[cursor++];
            if (strcmp(input, "null") == 0) {
                settings->input_policy.kind = CS_INPUT_POLICY_NULL;
            } else {
                settings->input_policy.kind = CS_INPUT_POLICY_FILE;
                settings->input_policy.file = input;
            }
        } else if (strcmp(opt, "--custom") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *name = argv[cursor++];
            cs_sb_push(settings->custom_measuremements,
                       ((struct cs_custom_meassurement){name, NULL}));
        } else if (strcmp(opt, "--custom-x") == 0) {
            if (cursor + 1 >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *name = argv[cursor++];
            const char *cmd = argv[cursor++];
            cs_sb_push(settings->custom_measuremements,
                       ((struct cs_custom_meassurement){name, cmd}));
        } else if (strcmp(opt, "--scan") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *scan_settings = argv[cursor++];
            double low, high, step;
            char *name;
            if (!cs_parse_range_scan_settings(scan_settings, &name, &low, &high,
                                              &step)) {
                fprintf(stderr, "error: invalid --scan argument\n");
                exit(EXIT_FAILURE);
            }

            char **param_list = cs_range_to_param_list(low, high, step);
            cs_sb_push(settings->params,
                       ((struct cs_benchmark_param){name, param_list}));
        } else if (strcmp(opt, "--scanl") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *scan_settings = argv[cursor++];
            char *name, *scan_list;
            if (!cs_parse_scan_list_settings(scan_settings, &name,
                                             &scan_list)) {
                fprintf(stderr, "error: invalid --scanl argument\n");
                exit(EXIT_FAILURE);
            }
            char **param_list = cs_parse_scan_list(scan_list);
            free(scan_list);
            cs_sb_push(settings->params,
                       ((struct cs_benchmark_param){name, param_list}));
        } else if (strcmp(opt, "--export-json") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *export_filename = argv[cursor++];
            settings->export.kind = CS_EXPORT_JSON;
            settings->export.filename = export_filename;
        } else if (strcmp(opt, "--analyze-dir") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *dir = argv[cursor++];
            settings->analyze_dir = dir;
        } else if (strcmp(opt, "--analyze") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *mode = argv[cursor++];
            if (strcmp(mode, "plot") == 0)
                settings->analyze_mode = CS_ANALYZE_PLOT;
            else if (strcmp(mode, "html") == 0)
                settings->analyze_mode = CS_ANALYZE_HTML;
            else
                cs_print_help_and_exit(EXIT_FAILURE);
        } else {
            cs_sb_push(settings->commands, opt);
        }
    }
}

static void
cs_exec_input_policy(const struct cs_input_policy *policy) {
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

static void
cs_exec_output_policy(enum cs_output_policy_kind policy) {
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

static int
cs_exec_command(const struct cs_command *command, int stdout_fd) {
    int rc = -1;
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        goto out;
    }

    if (pid == 0) {
        cs_exec_input_policy(&command->input_policy);
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
            cs_exec_output_policy(command->output_policy);
        }
        if (execvp(command->executable, command->argv) == -1)
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

static char **
cs_split_shell_words(const char *command) {
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
        int c = *command++;
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

static int
cs_compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

static struct cs_outliers
cs_classify_outliers(const double *data, size_t count) {
    double *ssa = malloc(count * sizeof(*ssa));
    memcpy(ssa, data, count * sizeof(*ssa));
    qsort(ssa, count, sizeof(*ssa), cs_compare_doubles);

    double q1 = ssa[count / 4];
    double q3 = ssa[count * 3 / 4];
    double iqr = q3 - q1;
    double los = q1 - (iqr * 3.0);
    double lom = q1 - (iqr * 1.5);
    double him = q3 + (iqr * 1.5);
    double his = q3 + (iqr * 3.0);

    struct cs_outliers result = {0};
    for (size_t i = 0; i < count; ++i) {
        double v = data[i];
        if (v < los)
            ++result.low_severe;
        else if (v > his)
            ++result.high_severe;
        else if (v < lom)
            ++result.low_mild;
        else if (v > him)
            ++result.high_mild;
    }
    free(ssa);
    return result;
}

static double
cs_c_max(double x, double u_a, double a, double sigma_b_2, double sigma_g_2) {
    double k = u_a - x;
    double d = k * k;
    double ad = a * d;
    double k1 = sigma_b_2 - a * sigma_g_2 + ad;
    double k0 = -a * ad;
    double det = k1 * k1 - 4 * sigma_g_2 * k0;
    return floor(-2.0 * k0 / (k1 + sqrt(det)));
}

static double
cs_var_out(double c, double a, double sigma_b_2, double sigma_g_2) {
    double ac = a - c;
    return (ac / a) * (sigma_b_2 - ac * sigma_g_2);
}

static double
cs_outlier_variance(double mean, double st_dev, double a) {
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

static struct cs_outlier_variance
cs_classify_outlier_variance(double fraction) {
    struct cs_outlier_variance variance;
    variance.fraction = fraction;
    if (fraction < 0.01) {
        variance.effect = CS_OUTLIERS_UNAFFECTED;
        variance.desc = "no";
    } else if (fraction < 0.1) {
        variance.effect = CS_OUTLIERS_SLIGHT;
        variance.desc = "a slight";
    } else if (fraction < 0.5) {
        variance.effect = CS_OUTLIERS_MODERATE;
        variance.desc = "a moderate";
    } else {
        variance.effect = CS_OUTLIERS_SEVERE;
        variance.desc = "a severe";
    }
    return variance;
}

static uint32_t
xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static double
cs_stat_mean(const double *v, size_t count) {
    double result = 0.0;
    for (size_t i = 0; i < count; ++i)
        result += v[i];
    return result / (double)count;
}

static double
cs_stat_st_dev(const double *v, size_t count) {
    double mean = cs_stat_mean(v, count);
    double result = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double t = v[i] - mean;
        result += t * t;
    }
    return sqrt(result / (double)count);
}

static double
cs_fitting_curve_1(double n) {
    (void)n;
    return 1.0;
}
static double
cs_fitting_curve_n(double n) {
    return n;
}
static double
cs_fitting_curve_n_sq(double n) {
    return n * n;
}
static double
cs_fitting_curve_n_cube(double n) {
    return n * n * n;
}
static double
cs_fitting_curve_logn(double n) {
    return log2(n);
}
static double
cs_fitting_curve_nlogn(double n) {
    return n * log2(n);
}

static void
cs_resample(const double *src, size_t count, double *dst, uint32_t entropy) {
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[xorshift32(&entropy) % count];
}

#define cs_bootstrap(_name, _stat_fn)                                          \
    static void cs_bootstrap_##_name(const double *src, size_t count,          \
                                     size_t resamples, uint32_t entropy,       \
                                     double *min_d, double *max_d) {           \
        double min = INFINITY;                                                 \
        double max = -INFINITY;                                                \
        double *tmp = malloc(sizeof(*tmp) * count);                            \
        for (size_t sample = 0; sample < resamples; ++sample) {                \
            cs_resample(src, count, tmp, xorshift32(&entropy));                \
            double stat = _stat_fn(tmp, count);                                \
            if (stat < min)                                                    \
                min = stat;                                                    \
            if (stat > max)                                                    \
                max = stat;                                                    \
        }                                                                      \
        *min_d = min;                                                          \
        *max_d = max;                                                          \
        free(tmp);                                                             \
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

// clang-format off

cs_bootstrap(mean, cs_stat_mean)
cs_bootstrap(st_dev, cs_stat_st_dev)

cs_mls(1, cs_fitting_curve_1)
cs_mls(n, cs_fitting_curve_n)
cs_mls(n_sq, cs_fitting_curve_n_sq)
cs_mls(n_cube, cs_fitting_curve_n_cube)
cs_mls(logn, cs_fitting_curve_logn)
cs_mls(nlogn, cs_fitting_curve_nlogn)

#undef cs_bootstrap
#undef cs_mls

    // clang-format on

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

static int
cs_print_time(char *dst, size_t sz, double t) {
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

static int
cs_process_executed_correctly(pid_t pid) {
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

static int
cs_execute_custom_measurement(struct cs_custom_meassurement *custom, int in_fd,
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

    if (!cs_process_executed_correctly(pid))
        return -1;

    return 0;
}

static int
cs_parse_custom_output(int fd, double *valuep) {
    char buffer[4096];
    ssize_t nread = read(fd, buffer, sizeof(buffer));
    if (nread == -1) {
        perror("read");
        return -1;
    }
    if (nread == sizeof(buffer)) {
        fprintf(stderr, "error: custom measurement output is too large\n");
        return -1;
    }
    buffer[nread] = '\0';

    char *end = NULL;
    double value = strtod(buffer, &end);
    if (end == buffer) {
        fprintf(stderr, "error: invalid custom measurement output\n");
        return -1;
    }

    *valuep = value;
    return 0;
}

static int
cs_execute_custom_measurements(struct cs_benchmark *bench, int stdout_fd) {
    int rc = -1;
    char path[] = "/tmp/csbench_tmp_XXXXXX";
    int custom_output_fd = mkstemp(path);
    if (custom_output_fd == -1) {
        perror("mkstemp");
        goto out;
    }

    size_t custom_count = cs_sb_len(bench->command->custom_measuremements);
    for (size_t i = 0; i < custom_count; ++i) {
        struct cs_custom_meassurement *custom =
            bench->command->custom_measuremements + i;
        if (lseek(stdout_fd, 0, SEEK_SET) == (off_t)-1 ||
            lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            goto out;
        }

        if (cs_execute_custom_measurement(custom, stdout_fd,
                                          custom_output_fd) == -1)
            goto out;

        if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            goto out;
        }

        double value;
        if (cs_parse_custom_output(custom_output_fd, &value) == -1)
            goto out;

        cs_sb_push(bench->custom_measurements[i], value);
    }
    rc = 0;
out:
    if (custom_output_fd != -1) {
        close(custom_output_fd);
        unlink(path);
    }
    return rc;
}

static int
cs_exec_and_measure(struct cs_benchmark *bench) {
    int ret = -1;
    int stdout_fd = -1;
    char path[] = "/tmp/csbench_out_XXXXXX";
    if (bench->command->custom_measuremements != NULL) {
        stdout_fd = mkstemp(path);
        if (stdout_fd == -1) {
            perror("mkstemp");
            goto out;
        }
    }

    volatile struct cs_cpu_time cpu_start = cs_getcputime();
    volatile double wall_clock_start = cs_get_time();
    volatile int rc = cs_exec_command(bench->command, stdout_fd);
    volatile double wall_clock_end = cs_get_time();
    volatile struct cs_cpu_time cpu_end = cs_getcputime();

    if (rc == -1) {
        fprintf(stderr, "error: failed to execute command\n");
        goto out;
    }

    ++bench->run_count;
    cs_sb_push(bench->exit_codes, rc);
    cs_sb_push(bench->wallclock_sample, wall_clock_end - wall_clock_start);
    cs_sb_push(bench->systime_sample,
               cpu_end.system_time - cpu_start.system_time);
    cs_sb_push(bench->usertime_sample, cpu_end.user_time - cpu_start.user_time);

    if (bench->command->custom_measuremements != NULL &&
        cs_execute_custom_measurements(bench, stdout_fd) == -1)
        goto out;

    ret = 0;
out:
    if (stdout_fd != -1) {
        close(stdout_fd);
        unlink(path);
    }
    return ret;
}

static int
cs_warmup(struct cs_benchmark *bench, double time_limit) {
    if (time_limit < 0.0)
        return 0;

    double start_time = cs_get_time();
    double end_time;
    do {
        if (cs_exec_command(bench->command, -1) == -1) {
            fprintf(stderr, "error: failed to execute warmup command\n");
            return -1;
        }
        end_time = cs_get_time();
    } while (end_time - start_time < time_limit);

    return 0;
}

static int
cs_execute_prepare(const char *cmd) {
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

    if (!cs_process_executed_correctly(pid))
        return -1;

    return 0;
}

static int
cs_run_benchmark(struct cs_benchmark *bench,
                 const struct cs_benchmark_stop_policy *stop_policy) {
    if (stop_policy->runs != 0) {
        for (size_t run_idx = 0; run_idx < stop_policy->runs; ++run_idx) {
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
    double time_limit = stop_policy->time_limit;
    size_t min_runs = stop_policy->min_runs;
    size_t max_runs = stop_policy->max_runs;
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

static struct cs_estimate
cs_estimate_mean(const double *data, size_t data_size, size_t nresamples) {
    double mean = cs_stat_mean(data, data_size);
    double min_mean, max_mean;
    cs_bootstrap_mean(data, data_size, nresamples, time(NULL), &min_mean,
                      &max_mean);
    return (struct cs_estimate){min_mean, mean, max_mean};
}

static struct cs_estimate
cs_estimate_st_dev(const double *data, size_t data_size, size_t nresamples) {
    double mean_st_dev = cs_stat_st_dev(data, data_size);
    double min_st_dev, max_st_dev;
    cs_bootstrap_st_dev(data, data_size, nresamples, time(NULL), &min_st_dev,
                        &max_st_dev);
    return (struct cs_estimate){min_st_dev, mean_st_dev, max_st_dev};
}

static void
cs_print_time_estimate(const char *name, const struct cs_estimate *est) {
    char buf1[256], buf2[256], buf3[256];
    cs_print_time(buf1, sizeof(buf1), est->lower);
    cs_print_time(buf2, sizeof(buf2), est->point);
    cs_print_time(buf3, sizeof(buf3), est->upper);
    printf("%7s %s %s %s\n", name, buf1, buf2, buf3);
}

static void
cs_print_estimate(const char *name, const struct cs_estimate *est) {
    char buf1[256], buf2[256], buf3[256];
    snprintf(buf1, sizeof(buf1), "%.5g", est->lower);
    snprintf(buf2, sizeof(buf1), "%.5g", est->point);
    snprintf(buf3, sizeof(buf1), "%.5g", est->upper);
    printf("%7s %8s %8s %8s\n", name, buf1, buf2, buf3);
}

static void
cs_print_outliers(const struct cs_outliers *outliers, size_t run_count) {
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

static void
cs_analyze_benchmark(const struct cs_benchmark *bench, size_t nresamples,
                     struct cs_benchmark_analysis *analysis) {
    size_t run_count = bench->run_count;
    analysis->mean_estimate =
        cs_estimate_mean(bench->wallclock_sample, run_count, nresamples);
    analysis->st_dev_estimate =
        cs_estimate_st_dev(bench->wallclock_sample, run_count, nresamples);
    analysis->systime_estimate =
        cs_estimate_mean(bench->systime_sample, run_count, nresamples);
    analysis->usertime_estimate =
        cs_estimate_mean(bench->usertime_sample, run_count, nresamples);
    if (bench->custom_measurements) {
        size_t count = cs_sb_len(bench->command->custom_measuremements);
        for (size_t i = 0; i < count; ++i) {
            analysis->custom_measurement_mean_estimates[i] = cs_estimate_mean(
                bench->custom_measurements[i], run_count, nresamples);
            analysis->custom_measurement_st_dev_estimates[i] =
                cs_estimate_st_dev(bench->custom_measurements[i], run_count,
                                   nresamples);
        }
    }
    analysis->outliers =
        cs_classify_outliers(bench->wallclock_sample, run_count);
    analysis->outlier_variance_fraction =
        cs_outlier_variance(analysis->mean_estimate.point,
                            analysis->st_dev_estimate.point, (double)run_count);
}

static void
cs_print_exit_code_info(const struct cs_benchmark *bench) {
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

static void
cs_print_benchmark_info(const struct cs_benchmark *bench,
                        const struct cs_benchmark_analysis *analysis) {
    printf("command\t'%s'\n", bench->command->str);
    printf("%zu runs\n", bench->run_count);
    cs_print_exit_code_info(bench);
    cs_print_time_estimate("mean", &analysis->mean_estimate);
    cs_print_time_estimate("st dev", &analysis->st_dev_estimate);
    cs_print_time_estimate("systime", &analysis->systime_estimate);
    cs_print_time_estimate("usrtime", &analysis->usertime_estimate);
    if (analysis->custom_measurement_mean_estimates) {
        size_t count = cs_sb_len(bench->command->custom_measuremements);
        for (size_t i = 0; i < count; ++i) {
            printf("custom measurement %s\n",
                   bench->command->custom_measuremements[i].name);
            cs_print_estimate("mean",
                              analysis->custom_measurement_mean_estimates + i);
            cs_print_estimate(
                "st dev", analysis->custom_measurement_st_dev_estimates + i);
        }
    }
    cs_print_outliers(&analysis->outliers, bench->run_count);
    struct cs_outlier_variance var =
        cs_classify_outlier_variance(analysis->outlier_variance_fraction);
    printf("outlying measurements have %s (%.1lf%%) effect on estimated "
           "standard deviation\n",
           var.desc, var.fraction * 100.0);
}

static int
cs_extract_executable_and_argv(const char *command_str, char **executable,
                               char ***argv) {
    int rc = 0;
    char **words = cs_split_shell_words(command_str);
    if (words == NULL) {
        fprintf(stderr, "error: invalid command syntax\n");
        return -1;
    }

    *executable = strdup(words[0]);
    cs_sb_push(*argv, strdup(words[0]));
    for (size_t i = 1; i < cs_sb_len(words); ++i)
        cs_sb_push(*argv, strdup(words[i]));
    cs_sb_push(*argv, NULL);

    for (size_t i = 0; i < cs_sb_len(words); ++i)
        cs_sb_free(words[i]);
    cs_sb_free(words);

    return rc;
}

static int
cs_init_command_exec(const char *shell, const char *cmd_str,
                     struct cs_command *cmd) {
    if (shell) {
        if (cs_extract_executable_and_argv(shell, &cmd->executable,
                                           &cmd->argv) != 0)
            return -1;
        // pop NULL appended by cs_extract_executable_and_path
        (void)cs_sb_pop(cmd->argv);
        cs_sb_push(cmd->argv, strdup("-c"));
        cs_sb_push(cmd->argv, strdup(cmd_str));
        cs_sb_push(cmd->argv, NULL);
    } else {
        if (cs_extract_executable_and_argv(cmd_str, &cmd->executable,
                                           &cmd->argv) != 0)
            return -1;
    }
    cmd->str = strdup(cmd_str);

    return 0;
}

static void
cs_free_settings(struct cs_settings *settings) {
    for (size_t i = 0; i < cs_sb_len(settings->commands); ++i) {
        struct cs_command *cmd = settings->commands + i;
        free(cmd->executable);
        for (char **word = cmd->argv; *word; ++word)
            free(*word);
        cs_sb_free(cmd->argv);
        free(cmd->str);
    }
    cs_sb_free(settings->commands);
}

static void
cs_replace_param_in_cmd(char *buffer, size_t buffer_size, const char *cmd_str,
                        const char *name, const char *value) {
    (void)buffer_size;
    size_t param_name_len = strlen(name);
    char *wr_cursor = buffer;
    const char *rd_cursor = cmd_str;
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
}

static int
cs_init_settings(const struct cs_cli_settings *cli,
                 struct cs_settings *settings) {
    settings->bench_stop = cli->bench_stop;
    settings->warmup_time = cli->warmup_time;
    settings->export = cli->export;
    settings->prepare_command = cli->prepare;
    settings->nresamples = cli->nresamples;
    settings->analyze_mode = cli->analyze_mode;
    settings->analyze_dir = cli->analyze_dir;
    settings->custom_measuremements = cli->custom_measuremements;

    if (cli->input_policy.kind == CS_INPUT_POLICY_FILE &&
        access(cli->input_policy.file, R_OK) == -1) {
        fprintf(stderr,
                "error: file specified as command input is not accessable "
                "(%s)\n",
                cli->input_policy.file);
        return -1;
    }

    size_t cmd_count = cs_sb_len(cli->commands);
    if (cmd_count == 0) {
        fprintf(stderr, "error: no commands specified\n");
        return -1;
    }

    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->commands[i];
        int found_param = 0;
        for (size_t j = 0; j < cs_sb_len(cli->params); ++j) {
            const struct cs_benchmark_param *param = cli->params + j;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "{%s}", param->name);
            if (strstr(cmd_str, buffer) == NULL)
                continue;

            found_param = 1;
            struct cs_command_group group = {0};
            for (size_t k = 0; k < cs_sb_len(param->values); ++k) {
                const char *param_value = param->values[k];
                cs_replace_param_in_cmd(buffer, sizeof(buffer), cmd_str,
                                        param->name, param_value);

                struct cs_command cmd = {0};
                cmd.input_policy = cli->input_policy;
                cmd.custom_measuremements = settings->custom_measuremements;
                cmd.output_policy = cli->output_policy;
                if (cs_init_command_exec(cli->shell, buffer, &cmd) == -1)
                    goto err_free_settings;

                cs_sb_push(settings->commands, cmd);
                cs_sb_push(group.command_idxs,
                           cs_sb_len(settings->commands) - 1);
                cs_sb_push(group.var_values, param_value);
            }
            group.var_name = param->name;
            group.template = strdup(cmd_str);
            cs_sb_push(settings->command_groups, group);
        }

        if (!found_param) {
            struct cs_command cmd = {0};
            cmd.input_policy = cli->input_policy;
            cmd.custom_measuremements = settings->custom_measuremements;
            cmd.output_policy = cli->output_policy;
            if (cs_init_command_exec(cli->shell, cmd_str, &cmd) == -1)
                goto err_free_settings;

            cs_sb_push(settings->commands, cmd);
        }
    }

    return 0;
err_free_settings:
    cs_free_settings(settings);
    return -1;
}

static void
cs_free_bench(struct cs_benchmark *bench) {
    cs_sb_free(bench->wallclock_sample);
    cs_sb_free(bench->systime_sample);
    cs_sb_free(bench->usertime_sample);
    cs_sb_free(bench->exit_codes);
    if (bench->custom_measurements) {
        for (size_t i = 0; i < cs_sb_len(bench->command->custom_measuremements);
             ++i)
            cs_sb_free(bench->custom_measurements[i]);
        free(bench->custom_measurements);
    }
}

static void
cs_free_cli_settings(struct cs_cli_settings *settings) {
    cs_sb_free(settings->commands);
    cs_sb_free(settings->custom_measuremements);
}

static void
cs_compare_benches(struct cs_benchmark_results *results) {
    if (results->bench_count == 1)
        return;

    size_t bench_count = results->bench_count;
    size_t best_idx = 0;
    double best_mean = results->analyses[0].mean_estimate.point;
    for (size_t i = 1; i < bench_count; ++i) {
        const struct cs_benchmark_analysis *analysis = results->analyses + i;
        double mean = analysis->mean_estimate.point;
        if (mean < best_mean) {
            best_idx = i;
            best_mean = mean;
        }
    }

    const struct cs_benchmark *best = results->benches + best_idx;
    const struct cs_benchmark_analysis *best_analysis =
        results->analyses + best_idx;
    printf("Fastest command '%s'\n", best->command->str);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_benchmark *bench = results->benches + i;
        const struct cs_benchmark_analysis *analysis = results->analyses + i;
        if (bench == best)
            continue;

        double ref =
            analysis->mean_estimate.point / best_analysis->mean_estimate.point;
        // propagate standard deviation for formula (t1 / t2)
        double a =
            analysis->st_dev_estimate.point / analysis->mean_estimate.point;
        double b = best_analysis->st_dev_estimate.point /
                   best_analysis->mean_estimate.point;
        double ref_st_dev = ref * sqrt(a * a + b * b);

        printf("%3lf ± %3lf times faster than '%s'\n", ref, ref_st_dev,
               bench->command->str);
    }
}

static int
cs_export_json(const struct cs_settings *app,
               const struct cs_benchmark *benches, size_t bench_count,
               const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        fprintf(stderr, "error: failed to open file '%s' for export\n",
                filename);
        return -1;
    }

    fprintf(f, "{ \"settings\": {");
    fprintf(f,
            "\"time_limit\": %lf, \"runs\": %zu, \"min_runs\": %zu, "
            "\"max_runs\": %zu, \"warmup_time\": %lf ",
            app->bench_stop.time_limit, app->bench_stop.runs,
            app->bench_stop.min_runs, app->bench_stop.max_runs,
            app->warmup_time);
    fprintf(f, "}, \"benches\": [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_benchmark *bench = benches + i;
        fprintf(f, "{ ");
        fprintf(f, "\"prepare\": \"%s\", ",
                bench->prepare ? bench->prepare : "");
        fprintf(f, "\"command\": \"%s\", ", bench->command->str);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"wallclock\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%lf%s", bench->wallclock_sample[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "], \"sys\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%lf%s", bench->systime_sample[i],
                    i != run_count - 1 ? ", " : "");
        fprintf(f, "], \"user\": [");
        for (size_t i = 0; i < run_count; ++i)
            fprintf(f, "%lf%s", bench->usertime_sample[i],
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

static int
cs_handle_export(const struct cs_settings *app,
                 const struct cs_benchmark *benches, size_t bench_count,
                 const struct cs_export_policy *policy) {
    switch (policy->kind) {
    case CS_EXPORT_JSON:
        return cs_export_json(app, benches, bench_count, policy->filename);
        break;
    case CS_DONT_EXPORT:
        break;
    }
    return 0;
}

static void
cs_make_whisker_plot(const struct cs_whisker_plot *plot, FILE *script) {
    fprintf(script, "data = [");
    for (size_t i = 0; i < plot->column_count; ++i) {
        fprintf(script, "[");
        for (size_t j = 0; j < plot->widths[i]; ++j)
            fprintf(script, "%lf, ", plot->data[i][j]);
        fprintf(script, "], ");
    }
    fprintf(script, "]\n");
    fprintf(script, "names = [");
    for (size_t i = 0; i < plot->column_count; ++i)
        fprintf(script, "'%s', ", plot->column_names[i]);
    fprintf(script, "]\n");
    fprintf(script,
            "import matplotlib.pyplot as plt\n"
            "plt.xlabel('command')\n"
            "plt.ylabel('time [s]')\n"
            "plt.boxplot(data)\n"
            "plt.xticks(list(range(1, len(names) + 1)), names)\n"
            "plt.savefig('%s')\n",
            plot->output_filename);
}

static void
cs_make_violin_plot(const struct cs_whisker_plot *plot, FILE *script) {
    fprintf(script, "data = [");
    for (size_t i = 0; i < plot->column_count; ++i) {
        fprintf(script, "[");
        for (size_t j = 0; j < plot->widths[i]; ++j)
            fprintf(script, "%lf, ", plot->data[i][j]);
        fprintf(script, "], ");
    }
    fprintf(script, "]\n");
    fprintf(script, "names = [");
    for (size_t i = 0; i < plot->column_count; ++i)
        fprintf(script, "'%s', ", plot->column_names[i]);
    fprintf(script, "]\n");
    fprintf(script,
            "import matplotlib.pyplot as plt\n"
            "plt.xlabel('command')\n"
            "plt.ylabel('time [s]')\n"
            "plt.violinplot(data)\n"
            "plt.xticks(list(range(1, len(names) + 1)), names)\n"
            "plt.savefig('%s')\n",
            plot->output_filename);
}

static void
cs_make_kde_plot(const struct cs_kde_plot *plot, FILE *script) {
    fprintf(script, "y = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(script, "%lf, ", plot->data[i]);
    fprintf(script, "]\n");
    fprintf(script, "x = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(script, "%lf, ", plot->lower + plot->step * i);
    fprintf(script, "]\n");

    fprintf(script,
            "import matplotlib.pyplot as plt\n"
            "plt.title('%s')\n"
            "plt.fill_between(x, y, interpolate=True, alpha=0.25)\n"
            "plt.vlines(%lf, [0], [%lf])\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('time [s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s')\n",
            plot->title, plot->mean, plot->mean_y, plot->output_filename);
}

static int
cs_python_found(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", "--version", NULL) == -1) {
            perror("dup2");
            _exit(-1);
        }
    }

    return cs_process_executed_correctly(pid);
}

static int
cs_launch_python_stdin_pipe(FILE **inp, pid_t *pidp) {
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
static int
cs_python_has_matplotlib(void) {
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid))
        return 0;

    fprintf(script, "import matplotlib.pyplot as plt\n");
    fclose(script);
    return cs_process_executed_correctly(pid);
}

static void
cs_init_whisker_plot(const struct cs_benchmark *benches, size_t bench_count,
                     struct cs_whisker_plot *plot) {
    plot->column_count = bench_count;
    plot->widths = malloc(sizeof(*plot->widths) * plot->column_count);
    plot->column_names =
        malloc(sizeof(*plot->column_names) * plot->column_count);
    for (size_t i = 0; i < plot->column_count; ++i) {
        plot->widths[i] = benches[i].run_count;
        plot->column_names[i] = benches[i].command->str;
    }

    plot->data = malloc(sizeof(*plot->data) * plot->column_count);
    for (size_t i = 0; i < plot->column_count; ++i) {
        size_t size = sizeof(**plot->data) * plot->widths[i];
        plot->data[i] = malloc(size);
        memcpy(plot->data[i], benches[i].wallclock_sample, size);
    }
}

static void
cs_free_whisker_plot(struct cs_whisker_plot *plot) {
    free(plot->widths);
    free(plot->column_names);
    for (size_t i = 0; i < plot->column_count; ++i)
        free(plot->data[i]);
    free(plot->data);
}

static void
cs_construct_kde(const double *data, size_t data_size, double *kde,
                 size_t kde_size, double *lowerp, double *stepp) {
    double *ssa = malloc(data_size * sizeof(*ssa));
    memcpy(ssa, data, data_size * sizeof(*ssa));
    qsort(ssa, data_size, sizeof(*ssa), cs_compare_doubles);
    double q1 = ssa[data_size / 4];
    double q3 = ssa[data_size * 3 / 4];
    double iqr = q3 - q1;
    double st_dev = cs_stat_st_dev(data, data_size);
    double h = 0.9 * fmin(st_dev, iqr / 1.34) * pow(data_size, -0.2);

    // Calculate bounds for plot. Use 3 sigma rule to reject severe outliers
    // being plotted.
    double mean = cs_stat_mean(data, data_size);
    double lower = fmax(mean - 3.0 * st_dev, ssa[0]);
    double upper = fmin(mean + 3.0 * st_dev, ssa[data_size - 1]);
    double step = (upper - lower) / kde_size;
    double k_mult = 1.0 / sqrt(2.0 * 3.1415926536);
    for (size_t i = 0; i < kde_size; ++i) {
        double x = lower + i * step;
        double kde_value = 0.0;
        for (size_t j = 0; j < data_size; ++j) {
            double u = (x - data[j]) / h;
            double k = k_mult * exp(-0.5 * u * u);
            kde_value += k;
        }
        kde_value /= data_size * h;
        kde[i] = kde_value;
    }

    *lowerp = lower;
    *stepp = step;
    free(ssa);
}

static void
cs_init_kde_plot(const struct cs_benchmark *bench, struct cs_kde_plot *plot) {
    size_t kde_points = 200;
    plot->count = kde_points;
    plot->data = malloc(sizeof(*plot->data) * plot->count);
    cs_construct_kde(bench->wallclock_sample, bench->run_count, plot->data,
                     plot->count, &plot->lower, &plot->step);
    plot->mean = cs_stat_mean(bench->wallclock_sample, bench->run_count);

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

static void
cs_free_kde_plot(struct cs_kde_plot *plot) {
    free(plot->data);
}

static int
cs_whisker_plot(const struct cs_benchmark *benches, size_t bench_count,
                const char *output_filename) {
    int ret = 0;
    struct cs_whisker_plot plot = {0};
    plot.output_filename = output_filename;
    cs_init_whisker_plot(benches, bench_count, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        ret = -1;
        goto out;
    }
    cs_make_whisker_plot(&plot, script);
    fclose(script);
    if (!cs_process_executed_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        ret = -1;
    }

out:
    cs_free_whisker_plot(&plot);
    return ret;
}

static int
cs_violin_plot(const struct cs_benchmark *benches, size_t bench_count,
               const char *output_filename) {
    int ret = 0;
    struct cs_whisker_plot plot = {0};
    plot.output_filename = output_filename;
    cs_init_whisker_plot(benches, bench_count, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        ret = -1;
        goto out;
    }
    cs_make_violin_plot(&plot, script);
    fclose(script);
    if (!cs_process_executed_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        ret = -1;
    }

out:
    cs_free_whisker_plot(&plot);
    return ret;
}

static int
cs_kde_plot(const struct cs_benchmark *bench, const char *output_filename) {
    int ret = 0;
    struct cs_kde_plot plot = {0};
    plot.output_filename = output_filename;
    plot.title = bench->command->str;
    cs_init_kde_plot(bench, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        ret = -1;
        goto out;
    }
    cs_make_kde_plot(&plot, script);
    fclose(script);
    if (!cs_process_executed_correctly(pid)) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        ret = -1;
    }

out:
    cs_free_kde_plot(&plot);
    return ret;
}

static int
cs_analyze_make_plots(const struct cs_benchmark *benches, size_t bench_count,
                      const char *analyze_dir) {
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s/whisker.svg", analyze_dir);
    if (cs_whisker_plot(benches, bench_count, buffer) == -1)
        return -1;

    snprintf(buffer, sizeof(buffer), "%s/violin.svg", analyze_dir);
    if (cs_violin_plot(benches, bench_count, buffer) == -1)
        return -1;

    for (size_t i = 0; i < bench_count; ++i) {
        snprintf(buffer, sizeof(buffer), "%s/kde_%zu.svg", analyze_dir, i + 1);
        if (cs_kde_plot(benches + i, buffer) == -1)
            return -1;
    }

    return 0;
}

static void
cs_print_html_report(const struct cs_benchmark_results *results, FILE *f) {
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
        const struct cs_benchmark *bench = results->benches + i;
        const struct cs_benchmark_analysis *analysis = results->analyses + i;
        fprintf(f, "<h2>command '%s'</h2>", bench->command->str);
        fprintf(f, "<div class=\"row\">");
        fprintf(f, "<div class=\"col\"><h3>time kde plot</h3>");
        fprintf(f, "<img src=\"kde_%zu.svg\"></div>", i + 1);
        fprintf(f, "<div class=\"col\"><h3>statistics</h3>");
        fprintf(f, "<div class=\"stats\">");
        fprintf(f, "<p>made total %zu runs</p>", bench->run_count);
        fprintf(f, "<table>");
        fprintf(f, "<thead><tr>"
                   "<th></th>"
                   "<th class=\"est-bound\">lower bound</th>"
                   "<th class=\"est-bound\">estimate</th>"
                   "<th class=\"est-bound\">upper bound</th>"
                   "</tr></thead><tbody>");
#define cs_html_estimate(_name, _est)                                          \
    do {                                                                       \
        char buf1[256], buf2[256], buf3[256];                                  \
        cs_print_time(buf1, sizeof(buf1), (_est)->lower);                      \
        cs_print_time(buf2, sizeof(buf2), (_est)->point);                      \
        cs_print_time(buf3, sizeof(buf3), (_est)->upper);                      \
        fprintf(f,                                                             \
                "<tr>"                                                         \
                "<td>" _name "</td>"                                           \
                "<td class=\"est-bound\">%s</td>"                              \
                "<td>%s</td>"                                                  \
                "<td class=\"est-bound\">%s</td>"                              \
                "</tr>",                                                       \
                buf1, buf2, buf3);                                             \
    } while (0)
        cs_html_estimate("mean", &analysis->mean_estimate);
        cs_html_estimate("st dev", &analysis->st_dev_estimate);
        cs_html_estimate("systime", &analysis->systime_estimate);
        cs_html_estimate("usrtime", &analysis->usertime_estimate);
#undef cs_html_estimate
        fprintf(f, "</tbody></table>");
        {
            const struct cs_outliers *outliers = &analysis->outliers;
            size_t outlier_count = outliers->low_mild + outliers->high_mild +
                                   outliers->low_severe + outliers->high_severe;
            if (outlier_count != 0) {
                size_t run_count = bench->run_count;
                fprintf(f, "<p>found %zu outliers (%.2f%%)</p><ul>",
                        outlier_count,
                        (double)outlier_count / bench->run_count * 100.0);
                if (outliers->low_severe)
                    fprintf(f, "<li>%zu (%.2f%%) low severe</li>",
                            outliers->low_severe,
                            (double)outliers->low_severe / run_count * 100.0);
                if (outliers->low_mild)
                    fprintf(f, "<li>%zu (%.2f%%) low mild</li>",
                            outliers->low_mild,
                            (double)outliers->low_mild / run_count * 100.0);
                if (outliers->high_mild)
                    fprintf(f, "<li>%zu (%.2f%%) high mild</li>",
                            outliers->high_mild,
                            (double)outliers->high_mild / run_count * 100.0);
                if (outliers->high_severe)
                    fprintf(f, "<li>%zu (%.2f%%) high severe</li>",
                            outliers->high_severe,
                            (double)outliers->high_severe / run_count * 100.0);
                fprintf(f, "</ul>");
            }
            struct cs_outlier_variance var = cs_classify_outlier_variance(
                analysis->outlier_variance_fraction);
            fprintf(f,
                    "<p>outlying measurements have %s (%.1lf%%) effect on "
                    "estimated "
                    "standard deviation</p>",
                    var.desc, var.fraction * 100.0);
        }
        fprintf(f, "</div></div></div>");
    }

    if (results->bench_count != 1) {
        fprintf(f, "<h2>comparison</h2>\n");
        fprintf(f, "<div class=\"row\"><img src=\"violin.svg\"></div>");
        fprintf(f, "</body>");
    }
}

static int
cs_analyze_make_html_report(const struct cs_benchmark_results *results,
                            const char *analyze_dir) {
    char name_buffer[4096];
    snprintf(name_buffer, sizeof(name_buffer), "%s/index.html", analyze_dir);
    FILE *f = fopen(name_buffer, "w");
    if (f == NULL) {
        fprintf(stderr, "error: failed to create file %s\n", name_buffer);
        return -1;
    }

    cs_print_html_report(results, f);
    fclose(f);
    return 0;
}

static int
cs_handle_analyze(const struct cs_benchmark_results *results,
                  enum cs_analyze_mode mode, const char *analyze_dir) {
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

        if (cs_analyze_make_plots(results->benches, results->bench_count,
                                  analyze_dir) == -1)
            return -1;
    }

    if (mode == CS_ANALYZE_HTML &&
        cs_analyze_make_html_report(results, analyze_dir) == -1)
        return -1;

    return 0;
}

static int
cs_compare_commands_in_group(const void *arg1, const void *arg2) {
    const struct cs_command_in_group_data *a = arg1;
    const struct cs_command_in_group_data *b = arg2;
    if (a->mean < b->mean)
        return -1;
    if (a->mean > b->mean)
        return 1;
    return 0;
}

static void
cs_investigate_command_groups(const struct cs_settings *settings,
                              const struct cs_benchmark_results *results) {
    for (size_t i = 0; i < cs_sb_len(settings->command_groups); ++i) {
        const struct cs_command_group *group = settings->command_groups + i;

        size_t cmd_in_group_count = cs_sb_len(group->command_idxs);
        struct cs_command_in_group_data *data =
            calloc(cmd_in_group_count, sizeof(*data));
        int values_are_doubles = 1;
        for (size_t j = 0; j < cmd_in_group_count; ++j) {
            const char *value = group->var_values[j];
            const struct cs_command *cmd =
                settings->commands + group->command_idxs[j];
            size_t bench_idx = -1;
            for (size_t k = 0; k < results->bench_count; ++k) {
                if (results->benches[k].command == cmd) {
                    bench_idx = k;
                    break;
                }
            }
            assert(bench_idx != (size_t)-1);

            char *end = NULL;
            double value_double = strtod(value, &end);
            if (end == value)
                values_are_doubles = 0;

            data[j] = (struct cs_command_in_group_data){
                value, value_double,
                results->analyses[bench_idx].mean_estimate.point};
        }
        qsort(data, cmd_in_group_count, sizeof(*data),
              cs_compare_commands_in_group);
        printf("command group '%s' with parameter %s\n", group->template,
               group->var_name);
        char buf[256];
        cs_print_time(buf, sizeof(buf), data[0].mean);
        printf("lowest time %s with parameter %s\n", buf, data[0].value);
        cs_print_time(buf, sizeof(buf), data[cmd_in_group_count - 1].mean);
        printf("highest time %s with parameter %s\n", buf,
               data[cmd_in_group_count - 1].value);
        if (values_are_doubles) {
            double *x = calloc(cmd_in_group_count, sizeof(*x));
            double *y = calloc(cmd_in_group_count, sizeof(*y));
            for (size_t j = 0; j < cmd_in_group_count; ++j) {
                x[j] = data[j].value_double;
                y[j] = data[j].mean;
            }

            double coef, rms;
            enum cs_big_o complexity =
                cs_mls(x, y, cmd_in_group_count, &coef, &rms);
            switch (complexity) {
            case CS_O_1:
                printf("mean time is most likely constant (O(1)) in terms of "
                       "parameter\n");
                break;
            case CS_O_N:
                printf("mean time is most likely linear (O(N)) in terms of "
                       "parameter\n");
                break;
            case CS_O_N_SQ:
                printf(
                    "mean time is most likely quadratic (O(N^2)) in terms of "
                    "parameter\n");
                break;
            case CS_O_N_CUBE:
                printf("mean time is most likely cubic (O(N^3)) in terms of "
                       "parameter\n");
                break;
            case CS_O_LOGN:
                printf("mean time is most likely logarithmic (O(log(N))) in "
                       "terms of parameter\n");
                break;
            case CS_O_NLOGN:
                printf("mean time is most likely linearithmic (O(N*log(N))) in "
                       "terms of parameter\n");
                break;
            }
            printf("linear coef %.3f rms %.3f\n", coef, rms);
            free(x);
            free(y);
        }

        free(data);
    }
}

static int
cs_run(const struct cs_settings *settings) {
    int ret = -1;
    struct cs_benchmark_results results = {0};
    results.bench_count = cs_sb_len(settings->commands);
    results.benches = calloc(results.bench_count, sizeof(*results.benches));
    results.analyses = calloc(results.bench_count, sizeof(*results.analyses));
    for (size_t i = 0; i < results.bench_count; ++i) {
        struct cs_benchmark *bench = results.benches + i;
        struct cs_benchmark_analysis *analysis = results.analyses + i;
        bench->prepare = settings->prepare_command;
        bench->command = settings->commands + i;
        if (bench->command->custom_measuremements) {
            size_t count = cs_sb_len(bench->command->custom_measuremements);
            bench->custom_measurements =
                calloc(count, sizeof(*bench->custom_measurements));
        }

        if (cs_warmup(bench, settings->warmup_time) == -1)
            goto out;
        if (cs_run_benchmark(bench, &settings->bench_stop) == -1)
            goto out;
        if (bench->command->custom_measuremements) {
            size_t count = cs_sb_len(bench->command->custom_measuremements);
            analysis->custom_measurement_mean_estimates = calloc(
                count, sizeof(*analysis->custom_measurement_mean_estimates));
            analysis->custom_measurement_st_dev_estimates = calloc(
                count, sizeof(*analysis->custom_measurement_st_dev_estimates));
        }

        cs_analyze_benchmark(bench, settings->nresamples, analysis);
        cs_print_benchmark_info(bench, analysis);
    }

    cs_compare_benches(&results);
    cs_investigate_command_groups(settings, &results);

    if (cs_handle_export(settings, results.benches, results.bench_count,
                         &settings->export) == -1)
        goto out;
    if (cs_handle_analyze(&results, settings->analyze_mode,
                          settings->analyze_dir) == -1)
        goto out;

    ret = 0;
out:
    for (size_t i = 0; i < results.bench_count; ++i)
        cs_free_bench(results.benches + i);
    free(results.benches);
    free(results.analyses);
    return ret;
}

int
main(int argc, char **argv) {
    int rc = EXIT_FAILURE;
    struct cs_cli_settings cli = {0};
    cs_parse_cli_args(argc, argv, &cli);

    struct cs_settings settings = {0};
    if (cs_init_settings(&cli, &settings) == -1)
        goto free_cli;

    if (cs_run(&settings) == -1)
        goto free_settings;

    rc = EXIT_SUCCESS;
free_settings:
    cs_free_settings(&settings);
free_cli:
    cs_free_cli_settings(&cli);
    return rc;
}
