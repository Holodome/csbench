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
    // /dev/null as input
    CS_INPUT_POLICY_NULL,
    // load input from file (supplied later)
    CS_INPUT_POLICY_FILE
};

// How to handle input of command?
struct cs_input_policy {
    enum cs_input_policy_kind kind;
    // CS_INPUT_POLICY_FILE
    const char *file;
};

enum cs_output_policy_kind {
    // /dev/null as output
    CS_OUTPUT_POLICY_NULL,
    // Print output to controlling terminal
    CS_OUTPUT_POLICY_INHERIT
};

// How to handle output of command?
struct cs_output_policy {
    enum cs_output_policy_kind kind;
};

enum cs_export_kind {
    CS_DONT_EXPORT,
    CS_EXPORT_JSON
};

struct cs_export_policy {
    enum cs_export_kind kind;
    const char *filename;
};

enum cs_analyse_mode {
    CS_DONT_ANALYSE,
    CS_ANALYSE_PLOT
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cs_settings {
    // Array of commands to benchmark
    const char **commands;
    double time_limit;
    double warmup_time;
    size_t nresamples;

    const char *shell;
    struct cs_export_policy export;

    // Command to run before each timing run
    const char *prepare;

    struct cs_input_policy input_policy;
    struct cs_output_policy output_policy;

    const char *analyse_dir;
    enum cs_analyse_mode analyse_mode;
};

// Description of command to benchmark.
// Commands are executed using execve.
struct cs_command {
    // Command string as supplied by user.
    const char *str;
    // Full path to executagle
    char *executable;
    // Argv supplied to execve.
    char **argv;

    struct cs_input_policy input_policy;
    struct cs_output_policy output_policy;
};

// Information gethered from user input (settings), parsed
// and prepared for benchmarking.
struct cs_app {
    // List of commands to benchmark
    struct cs_command *commands;

    double time_limit;
    double warmup_time;
    size_t nresamples;

    const char *prepare_command;
    struct cs_export_policy export;

    enum cs_analyse_mode analyse_mode;
    const char *analyse_dir;
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
    // These fields are output
    struct cs_estimate mean_estimate;
    struct cs_estimate st_dev_estimate;
    struct cs_estimate systime_estimate;
    struct cs_estimate usertime_estimate;
    struct cs_outliers outliers;
    double outlier_variance_fraction;
};

struct cs_cpu_time {
    double user_time;
    double system_time;
};

struct cs_whisker_plot {
    double **data;
    const char **column_names;
    size_t *widths;
    size_t column_count;
    const char *output_filename;
};

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

static void cs_print_help_and_exit(int rc) {
    printf(
        "A command-line benchmarking tool\n"
        "\n"
        "Usage: csbench [OPTIONS] <command> ...\n"
        "\n"
        "Where options is one of:\n"
        "--warmup <n>         - specify warmup time in seconds\n"
        "--time-limit <n>     - specify how long to run benchmarks\n"
        "--prepare <cmd>      - specify command to be executed before each "
        "benchmark run\n"
        "--nrs <n>            - specify number of resamples for bootstrapping\n"
        "--shell <shell>      - specify shell for command to be executed with. "
        "Can "
        "either be none or command resolving to shell execution\n"
        "--output <where>     - specify how to handle each command output. Can "
        "be either null or inherit\n"
        "--input <where>      - specify how each command should recieve its "
        "input. Can be either null or file name\n"
        "--export-json <file> - export benchmark results to json\n"
        "--analyse-dir <dir>  - directory where analysis will be saved at\n"
        "--analyse <opt>      - more complex analysis. <opt> can be one of\n"
        "   plot        - make plots as images\n"
        "--verbose            - print debug information\n"
        "--help               - print this message\n"
        "--version            - print version\n");
    exit(rc);
}

static void cs_print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static void cs_parse_cli_args(int argc, char **argv,
                              struct cs_settings *settings) {
    settings->time_limit = 5.0;
    settings->warmup_time = 1.0;
    settings->nresamples = 100000;
    settings->shell = "/bin/sh";
    settings->analyse_dir = ".csbench";

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

            settings->time_limit = value;
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
                settings->output_policy.kind = CS_OUTPUT_POLICY_NULL;
            else if (strcmp(out, "inherit") == 0)
                settings->output_policy.kind = CS_OUTPUT_POLICY_INHERIT;
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
        } else if (strcmp(opt, "--export-json") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *export_filename = argv[cursor++];
            settings->export.kind = CS_EXPORT_JSON;
            settings->export.filename = export_filename;
        } else if (strcmp(opt, "--analyse-dir") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *dir = argv[cursor++];
            settings->analyse_dir = dir;
        } else if (strcmp(opt, "--analyse") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *mode = argv[cursor++];
            if (strcmp(mode, "plot") == 0)
                settings->analyse_mode = CS_ANALYSE_PLOT;
            else
                cs_print_help_and_exit(EXIT_FAILURE);
        } else {
            cs_sb_push(settings->commands, opt);
        }
    }
}

static int cs_exec_command_do_exec(const struct cs_command *command) {
    switch (command->input_policy.kind) {
    case CS_INPUT_POLICY_NULL: {
        close(STDIN_FILENO);
        int fd = open("/dev/null", O_RDONLY);
        if (fd == -1) {
            perror("open");
            return -1;
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            return -1;
        }
        close(fd);
        break;
    }
    case CS_INPUT_POLICY_FILE: {
        close(STDIN_FILENO);
        int fd = open(command->input_policy.file, O_RDONLY);
        if (fd == -1) {
            perror("open");
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            return -1;
        }
        close(fd);
        break;
    }
    }

    switch (command->output_policy.kind) {
    case CS_OUTPUT_POLICY_NULL: {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1) {
            perror("open");
            return -1;
        }

        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
            perror("dup2");
            return -1;
        }
        close(fd);
        break;
    }
    case CS_OUTPUT_POLICY_INHERIT:
        break;
    }

    if (execv(command->executable, command->argv) == -1) {
        perror("execv");
        return -1;
    }

    return 0;
}

static int cs_exec_command(const struct cs_command *command) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0)
        _exit(cs_exec_command_do_exec(command));

    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        return -1;
    }

    int result = -1;
    if (WIFEXITED(status))
        result = WEXITSTATUS(result);
    else if (WIFSIGNALED(status))
        result = 128 + WTERMSIG(status);

    return result;
}

static char **cs_split_shell_words(const char *command) {
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

static int cs_compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;

    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

static struct cs_outliers cs_classify_outliers(const double *data,
                                               size_t count) {
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

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

// Resample with replacement
static void cs_resample(const double *src, size_t count, double *dst,
                        uint32_t entropy) {
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

// clang-format off

cs_bootstrap(mean, cs_stat_mean)
cs_bootstrap(st_dev, cs_stat_st_dev)

    // clang-format on

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

    if (t >= 1e9) {
        count += snprintf(dst, sz, "%.4g %s", t, units);
    } else if (t >= 1e3) {
        count += snprintf(dst, sz, "%.0f %s", t, units);
    } else if (t >= 1e2) {
        count += snprintf(dst, sz, "%.1f %s", t, units);
    } else if (t >= 1e1) {
        count += snprintf(dst, sz, "%.2f %s", t, units);
    } else {
        count += snprintf(dst, sz, "%.3f %s", t, units);
    }

    return count;
}

static int cs_exec_and_measure(struct cs_benchmark *bench) {
    volatile struct cs_cpu_time cpu_start = cs_getcputime();
    volatile double wall_clock_start = cs_get_time();
    volatile int rc = cs_exec_command(bench->command);
    volatile double wall_clock_end = cs_get_time();
    volatile struct cs_cpu_time cpu_end = cs_getcputime();

    if (rc == -1) {
        fprintf(stderr, "error: failed to execute command\n");
        return -1;
    }

    ++bench->run_count;
    cs_sb_push(bench->exit_codes, rc);
    cs_sb_push(bench->wallclock_sample, wall_clock_end - wall_clock_start);
    cs_sb_push(bench->systime_sample,
               cpu_end.system_time - cpu_start.system_time);
    cs_sb_push(bench->usertime_sample, cpu_end.user_time - cpu_start.user_time);

    return 0;
}

static int cs_warmup(struct cs_benchmark *bench, double time_limit) {
    if (time_limit < 0.0)
        return 0;

    double start_time = cs_get_time();
    double end_time;
    do {
        if (cs_exec_command(bench->command) == -1) {
            fprintf(stderr, "error: failed to execute warmup command\n");
            return -1;
        }
        end_time = cs_get_time();
    } while (end_time - start_time < time_limit);

    return 0;
}

static int cs_run_benchmark(struct cs_benchmark *bench, double time_limit) {
    double niter_accum = 1;
    size_t niter = 1;

    double start_time = cs_get_time();
    for (size_t count = 0;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            if (bench->prepare)
                system(bench->prepare);
            if (cs_exec_and_measure(bench) == -1)
                return -1;
        }

        double end_time = cs_get_time();
        if (end_time - start_time > time_limit && count >= 4)
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

static struct cs_estimate cs_estimate_mean(const double *data, size_t data_size,
                                           size_t nresamples) {
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

static void cs_print_estimate(const char *name, const struct cs_estimate *est) {
    char buf1[256], buf2[256], buf3[256];
    cs_print_time(buf1, sizeof(buf1), est->lower);
    cs_print_time(buf2, sizeof(buf2), est->point);
    cs_print_time(buf3, sizeof(buf3), est->upper);
    printf("%7s %s %s %s\n", name, buf1, buf2, buf3);
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

static void cs_print_outlier_variance(double fraction) {
    struct cs_outlier_variance var = cs_classify_outlier_variance(fraction);
    printf("Outlying measurements have %s (%.1lf%%) effect on estimated "
           "standard deviation\n",
           var.desc, var.fraction * 100.0);
}

static void cs_analyse_benchmark(struct cs_benchmark *bench,
                                 size_t nresamples) {
    size_t run_count = bench->run_count;
    bench->mean_estimate =
        cs_estimate_mean(bench->wallclock_sample, run_count, nresamples);
    bench->st_dev_estimate =
        cs_estimate_st_dev(bench->wallclock_sample, run_count, nresamples);
    bench->systime_estimate =
        cs_estimate_mean(bench->systime_sample, run_count, nresamples);
    bench->usertime_estimate =
        cs_estimate_mean(bench->usertime_sample, run_count, nresamples);
    bench->outliers = cs_classify_outliers(bench->wallclock_sample, run_count);
    bench->outlier_variance_fraction =
        cs_outlier_variance(bench->mean_estimate.point,
                            bench->st_dev_estimate.point, (double)run_count);
}

static void cs_print_exit_code_info(const struct cs_benchmark *bench) {
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

static void cs_print_benchmark_info(const struct cs_benchmark *bench) {
    printf("command\t'%s'\n", bench->command->str);
    printf("%zu runs\n", bench->run_count);
    cs_print_exit_code_info(bench);
    cs_print_estimate("mean", &bench->mean_estimate);
    cs_print_estimate("st dev", &bench->st_dev_estimate);
    cs_print_estimate("systime", &bench->systime_estimate);
    cs_print_estimate("usrtime", &bench->usertime_estimate);
    cs_print_outliers(&bench->outliers, bench->run_count);
    cs_print_outlier_variance(bench->outlier_variance_fraction);
}

static char *cs_find_full_path(const char *path) {
    char *full = NULL;
    if (*path == '/') {
        full = strdup(path);
    } else if (strchr(path, '/') != NULL) {
        char path[4096];
        if (getcwd(path, sizeof(path)) == NULL) {
            perror("getcwd");
            return NULL;
        }
        size_t cwd_len = strlen(path);
        snprintf(path + cwd_len, sizeof(path) - cwd_len, "/%s", full);
        full = strdup(path);
    } else {
        const char *path_var = getenv("PATH");
        if (path_var == NULL) {
            fprintf(stderr, "error: PATH variable is not set\n");
            return NULL;
        }
        const char *path_var_end = path_var + strlen(path_var);
        const char *cursor = path_var;
        while (cursor != path_var_end + 1) {
            const char *next_sep = strchr(cursor, ':');
            if (next_sep == NULL)
                next_sep = path_var_end;

            char path[4096];
            snprintf(path, sizeof(path), "%.*s/%s", (int)(next_sep - cursor),
                     cursor, path);

            if (access(path, X_OK) == 0) {
                full = strdup(path);
                break;
            }

            cursor = next_sep + 1;
        }
    }

    return full;
}

static int cs_extract_executable_and_argv(const char *command_str,
                                          char **executable, char ***argv) {
    int rc = 0;
    char **words = cs_split_shell_words(command_str);
    if (words == NULL) {
        fprintf(stderr, "error: invalid command syntax\n");
        return -1;
    }

    char *real_exec_path = cs_find_full_path(words[0]);
    if (real_exec_path == NULL) {
        fprintf(stderr,
                "error: failed to find executable path for command '%s'\n",
                command_str);
        rc = -1;
    } else {
        *executable = real_exec_path;
        const char *exec_name = strrchr(real_exec_path, '/');
        if (exec_name == NULL) 
            exec_name = real_exec_path;
        else 
            ++exec_name;
        cs_sb_push(*argv, strdup(exec_name));
        for (size_t i = 1; i < cs_sb_len(words); ++i)
            cs_sb_push(*argv, strdup(words[i]));
        cs_sb_push(*argv, NULL);
    }

    for (size_t i = 0; i < cs_sb_len(words); ++i)
        cs_sb_free(words[i]);
    cs_sb_free(words);

    return rc;
}

static int init_app(const struct cs_settings *settings, struct cs_app *app) {
    app->time_limit = settings->time_limit;
    app->warmup_time = settings->warmup_time;
    app->export = settings->export;
    app->prepare_command = settings->prepare;
    app->nresamples = settings->nresamples;
    app->analyse_mode = settings->analyse_mode;
    app->analyse_dir = settings->analyse_dir;

    if (settings->input_policy.kind == CS_INPUT_POLICY_FILE &&
        access(settings->input_policy.file, R_OK) == -1) {
        fprintf(
            stderr,
            "error: file specified as command input is not accessable (%s)\n",
            settings->input_policy.file);
        return -1;
    }

    size_t command_count = cs_sb_len(settings->commands);
    if (command_count == 0) {
        fprintf(stderr, "error: no commands specified\n");
        return -1;
    }

    for (size_t command_idx = 0; command_idx < command_count; ++command_idx) {
        struct cs_command command = {0};
        const char *command_str = settings->commands[command_idx];
        command.str = command_str;
        command.input_policy = settings->input_policy;
        command.output_policy = settings->output_policy;
        // TODO: Cleanup this code fragment
        if (settings->shell) {
            if (cs_extract_executable_and_argv(
                    settings->shell, &command.executable, &command.argv) != 0) {
                cs_sb_free(app->commands);
                return -1;
            }
            // pop NULL appended by cs_extract_executable_and_path
            cs_sb_pop(command.argv);
            cs_sb_push(command.argv, strdup("-c"));
            cs_sb_push(command.argv, strdup(command_str));
            cs_sb_push(command.argv, NULL);
        } else {
            if (cs_extract_executable_and_argv(command_str, &command.executable,
                                               &command.argv) != 0) {
                cs_sb_free(app->commands);
                return -1;
            }
        }

        cs_sb_push(app->commands, command);
    }

    return 0;
}

static void cs_free_bench(struct cs_benchmark *bench) {
    cs_sb_free(bench->wallclock_sample);
    cs_sb_free(bench->systime_sample);
    cs_sb_free(bench->usertime_sample);
    cs_sb_free(bench->exit_codes);
}

static void cs_free_app(struct cs_app *app) {
    size_t command_count = cs_sb_len(app->commands);
    for (size_t command_idx = 0; command_idx < command_count; ++command_idx) {
        struct cs_command *command = app->commands + command_idx;
        free(command->executable);
        for (char **word = command->argv; *word; ++word)
            free(*word);
        cs_sb_free(command->argv);
    }
    cs_sb_free(app->commands);
}

static void cs_compare_benches(const struct cs_benchmark *benches) {
    size_t bench_count = cs_sb_capacity(benches);
    assert(bench_count > 1);
    size_t best_idx = 0;
    double best_mean = benches[0].mean_estimate.point;
    for (size_t i = 1; i < bench_count; ++i) {
        const struct cs_benchmark *bench = benches + i;
        double mean = bench->mean_estimate.point;
        if (mean < best_mean) {
            best_idx = i;
            best_mean = mean;
        }
    }

    const struct cs_benchmark *best = benches + best_idx;
    printf("Fastest command '%s'\n", best->command->str);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct cs_benchmark *bench = benches + i;
        if (bench == best)
            continue;

        double ref = bench->mean_estimate.point / best->mean_estimate.point;
        // propagate standard deviation for formula (t1 / t2)
        double a = bench->st_dev_estimate.point / bench->mean_estimate.point;
        double b = best->st_dev_estimate.point / best->mean_estimate.point;
        double ref_st_dev = ref * sqrt(a * a + b * b);

        printf("%3lf ± %3lf times faster than '%s'\n", ref, ref_st_dev,
               bench->command->str);
    }
}

static void cs_export_json(const struct cs_app *app,
                           const struct cs_benchmark *benches,
                           const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        fprintf(stderr, "error: failed to open file '%s' for export\n",
                filename);
        return;
    }

    fprintf(f, "{ \"settings\": {");
    fprintf(f, "\"time_limit\": %lf, \"warmup_time\": %lf ", app->time_limit,
            app->warmup_time);
    fprintf(f, "}, \"benches\": [");
    size_t bench_count = cs_sb_len(benches);
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
        fprintf(f,
                "], \"mean_est\": { \"lower\": %lf, \"point\": %lf, \"upper\": "
                "%lf }, ",
                bench->mean_estimate.lower, bench->mean_estimate.point,
                bench->mean_estimate.upper);
        fprintf(f,
                "\"st_dev_est\": { \"lower\": %lf, \"point\": %lf, \"upper\": "
                "%lf }, ",
                bench->st_dev_estimate.lower, bench->st_dev_estimate.point,
                bench->st_dev_estimate.upper);
        fprintf(
            f,
            "\"sys_est\": { \"lower\": %lf, \"point\": %lf, \"upper\": %lf }, ",
            bench->systime_estimate.lower, bench->systime_estimate.point,
            bench->systime_estimate.upper);
        fprintf(f,
                "\"user_est\": { \"lower\": %lf, \"point\": %lf, \"upper\": "
                "%lf }, ",
                bench->usertime_estimate.lower, bench->usertime_estimate.point,
                bench->usertime_estimate.upper);
        fprintf(f,
                "\"outliers\": { \"low_severe\": %zu, \"low_mild\": %zu, "
                "\"high_mild\": %zu, \"high_severe\": %zu, \"variance\": %lf }",
                bench->outliers.low_severe, bench->outliers.low_mild,
                bench->outliers.high_mild, bench->outliers.high_severe,
                bench->outlier_variance_fraction);
        fprintf(f, "}");
        if (i != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);
}

static void cs_handle_export(const struct cs_app *app,
                             const struct cs_benchmark *benches,
                             const struct cs_export_policy *policy) {
    assert(policy->kind != CS_DONT_EXPORT);
    switch (policy->kind) {
    case CS_EXPORT_JSON:
        cs_export_json(app, benches, policy->filename);
        break;
    default:
        assert(0);
    }
}
static void cs_make_whisker_plot(const struct cs_whisker_plot *plot,
                                 FILE *script) {
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

static void cs_make_violin_plot(const struct cs_whisker_plot *plot,
                                FILE *script) {
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

static void cs_make_kde_plot(const struct cs_kde_plot *plot, FILE *script) {
    fprintf(script, "y = [");
    for (size_t i = 0; i < plot->count; ++i) {
        fprintf(script, "%lf, ", plot->data[i]);
    }
    fprintf(script, "]\n");
    fprintf(script, "x = [");
    for (size_t i = 0; i < plot->count; ++i) {
        fprintf(script, "%lf, ", plot->lower + plot->step * i);
    }
    fprintf(script, "]\n");

    fprintf(script,
            "import matplotlib.pyplot as plt\n"
            "plt.title('%s')\n"
            "plt.fill_between(x, y, interpolate=True, color='lightskyblue')\n"
            "plt.vlines(%lf, [0], [%lf], color='blue')\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('time [s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s')\n",
            plot->title, plot->mean, plot->mean_y, plot->output_filename);
}

static int cs_check_python_exists(void) {
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
        /* close(STDOUT_FILENO); */
        /* close(STDERR_FILENO); */
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

static int cs_check_python_has_matplotlib(void) {
    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid))
        return 0;

    fprintf(script, "import matplotlib.pyplot as plt\n");
    fclose(script);

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

static void cs_init_whisker_plot(const struct cs_benchmark *benches,
                                 struct cs_whisker_plot *plot) {
    plot->column_count = cs_sb_len(benches);
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

static void cs_free_whisker_plot(struct cs_whisker_plot *plot) {
    free(plot->widths);
    free(plot->column_names);
    for (size_t i = 0; i < plot->column_count; ++i)
        free(plot->data[i]);
    free(plot->data);
}

static void cs_construct_kde(const double *data, size_t data_size, double *kde,
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
    double lower = fmax(mean - 3 * st_dev, ssa[0]);
    double upper = fmin(mean + 3 * st_dev, ssa[data_size - 1]);
    double step = (upper - lower) / data_size;

    double k_mult = 1.0 / sqrt(2 * 3.1415926536);
    for (size_t i = 0; i < kde_size; ++i) {
        double x = lower + i * step;

        double kde_value = 0;
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

static void cs_init_kde_plot(const struct cs_benchmark *bench,
                             struct cs_kde_plot *plot) {
    size_t kde_points = 200;
    plot->count = kde_points;
    plot->data = malloc(sizeof(*plot->data) * plot->count);
    cs_construct_kde(bench->wallclock_sample, bench->run_count, plot->data,
                     plot->count, &plot->lower, &plot->step);
    plot->mean = cs_stat_mean(bench->wallclock_sample, bench->run_count);

    // linear interpolate between adjacent points to find height of line with x
    // equal mean
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

static void cs_free_kde_plot(struct cs_kde_plot *plot) {
    free(plot->data);
}

static int cs_whisker_plot(const struct cs_benchmark *benches,
                           const char *output_filename) {
    int rc = 0;
    struct cs_whisker_plot plot = {0};
    plot.output_filename = output_filename;
    cs_init_whisker_plot(benches, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        rc = -1;
        goto out;
    }
    cs_make_whisker_plot(&plot, script);

    fclose(script);
    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        rc = -1;
        goto out;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        rc = -1;
    }

out:
    cs_free_whisker_plot(&plot);
    return rc;
}

static int cs_violin_plot(const struct cs_benchmark *benches,
                          const char *output_filename) {
    int rc = 0;
    struct cs_whisker_plot plot = {0};
    plot.output_filename = output_filename;
    cs_init_whisker_plot(benches, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        rc = -1;
        goto out;
    }
    cs_make_violin_plot(&plot, script);

    fclose(script);
    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        rc = -1;
        goto out;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        rc = -1;
    }

out:
    cs_free_whisker_plot(&plot);
    return rc;
}

static int cs_kde_plot(const struct cs_benchmark *bench,
                       const char *output_filename) {
    int rc = 0;
    struct cs_kde_plot plot = {0};
    plot.output_filename = output_filename;
    plot.title = bench->command->str;
    cs_init_kde_plot(bench, &plot);

    FILE *script;
    pid_t pid;
    if (cs_launch_python_stdin_pipe(&script, &pid) == -1) {
        fprintf(stderr, "error: failed to launch python\n");
        rc = -1;
        goto out;
    }
    cs_make_kde_plot(&plot, script);

    fclose(script);
    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            perror("waitpid");
        rc = -1;
        goto out;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "error: python finished with non-zero exit code\n");
        rc = -1;
    }

out:
    cs_free_kde_plot(&plot);
    return rc;
}

static int cs_analyse_make_plots(const struct cs_benchmark *benches,
                                 const char *analyse_dir) {
    char name_buffer[4096];
    snprintf(name_buffer, sizeof(name_buffer), "%s/whisker.svg", analyse_dir);
    if (cs_whisker_plot(benches, name_buffer) == -1) {
        return -1;
    }

    snprintf(name_buffer, sizeof(name_buffer), "%s/violin.svg", analyse_dir);
    if (cs_violin_plot(benches, name_buffer) == -1) {
        return -1;
    }

    for (size_t i = 0; i < cs_sb_len(benches); ++i) {
        snprintf(name_buffer, sizeof(name_buffer), "%s/kde_%zu.svg",
                 analyse_dir, i + 1);
        if (cs_kde_plot(benches, name_buffer) == -1) {
            return -1;
        }
    }

    return 0;
}

static int cs_analyse(const struct cs_benchmark *benches,
                      enum cs_analyse_mode mode, const char *analyse_dir) {
    assert(mode != CS_DONT_ANALYSE);

    if (mkdir(analyse_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            perror("mkdir");
            return -1;
        }
    }

    if (mode == CS_ANALYSE_PLOT) {
        if (!cs_check_python_exists()) {
            fprintf(stderr, "error: failed to find python3 executable\n");
            return -1;
        }
        if (!cs_check_python_has_matplotlib()) {
            fprintf(stderr,
                    "error: python does not have matplotlib installed\n");
            return -1;
        }

        if (cs_analyse_make_plots(benches, analyse_dir) == -1)
            return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    struct cs_app app = {0};
    {
        struct cs_settings settings = {0};
        cs_parse_cli_args(argc, argv, &settings);
        if (init_app(&settings, &app) == -1)
            return EXIT_FAILURE;
    }

    size_t command_count = cs_sb_len(app.commands);
    struct cs_benchmark *benches = NULL;
    for (size_t command_idx = 0; command_idx < command_count; ++command_idx) {
        struct cs_benchmark bench = {0};
        bench.prepare = app.prepare_command;
        bench.command = app.commands + command_idx;

        cs_warmup(&bench, app.warmup_time);
        cs_run_benchmark(&bench, app.time_limit);
        cs_analyse_benchmark(&bench, app.nresamples);
        cs_print_benchmark_info(&bench);
        cs_sb_push(benches, bench);
    }

    if (command_count != 1)
        cs_compare_benches(benches);
    if (app.export.kind != CS_DONT_EXPORT)
        cs_handle_export(&app, benches, &app.export);
    if (app.analyse_mode != CS_DONT_ANALYSE)
        cs_analyse(benches, app.analyse_mode, app.analyse_dir);

    for (size_t i = 0; i < cs_sb_len(benches); ++i)
        cs_free_bench(benches + i);
    cs_sb_free(benches);

    cs_free_app(&app);
}
