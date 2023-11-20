#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/time.h>
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

// This structure contains all information
// supplied by user prior to benchmark start.
struct cs_settings {
    // Array of commands to benchmark
    const char **commands;
    double time_limit;
    double warmup_time;

    // Command to run before each timing run
    const char *prepare;

    struct cs_input_policy input_policy;
    struct cs_output_policy output_policy;
};

// Description of command to benchmark.
// This structure should contain all
// information needed to benchmark it (all the corresponding
// stuff from settings) as it behaves as interface for benchmark.
struct cs_command {
    // Command string as supplied by user.
    const char *str;
    // Parsed executable path. If command is not a shell one (called by execve),
    // we need to get full path to executable.
    char *executable;
    // Argv supplied to execve.
    char **argv;

    struct cs_input_policy input_policy;
    struct cs_output_policy output_policy;
};

// Information gethered from user input (settings), parsed
// and prepared for benchmarking.
struct cs_app {
    struct cs_settings settings;
    // List of commands to benchmark
    struct cs_command *commands;

    int has_prepare_command;
    struct cs_command prepare_command;
};

struct cs_measurement_ops {
    void *(*allocate_state)(void);
    void (*begin_timing)(void *state);
    void (*end_timing)(void *state);
    void (*free_state)(void *state);
    double (*get_units)(void *state);
};

struct cs_timer_gettimeofday_state {
    struct timeval start;
    struct timeval end;
};

struct cs_statistics {
    double min;
    double max;
    double sum;
    size_t count;
    double mean;

    double q1;
    double median;
    double q3;
    double iqr;

    double st_dev;
    double max_deviation;
    double confidence_interval;
    double cv;
};

struct cs_sample {
    double *values;
    struct cs_statistics statistics;
};

// Input/output of benchmark.
struct cs_benchmark {
    const struct cs_command *prepare;
    const struct cs_command *command;
    const struct cs_measurement_ops *timer;

    struct cs_sample sample;
    int *exit_codes;
};

struct cs_outliers {
    size_t low_severe;
    size_t low_mild;
    size_t high_mild;
    size_t high_severe;
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

#define cs_sb_free(_a)                                                         \
    (((_a) != NULL)                                                            \
     ? cs_free(cs_sb_header(_a), cs_sb_capacity(_a) * sizeof(*(_a)) +          \
                                     sizeof(struct cs_sb_header)),             \
     0 : 0)
#define cs_sb_push(_a, _v)                                                     \
    (cs_sb_maybegrow(_a, 1), (_a)[cs_sb_size(_a)++] = (_v))
#define cs_sb_last(_a) ((_a)[cs_sb_size(_a) - 1])
#define cs_sb_len(_a) (((_a) != NULL) ? cs_sb_size(_a) : 0)
#define cs_sb_pop(_a) ((_a)[--cs_sb_size(_a)])
#define cs_sb_purge(_a) ((_a) ? (cs_sb_size(_a) = 0) : 0)

static void *cs_sb_grow_impl(void *arr, size_t inc, size_t stride);
static void *cs_realloc(void *ptr, size_t old_size, size_t new_size);

#define cs_free(_ptr, _size) (void)cs_realloc(_ptr, _size, 0)
#define cs_alloc(_size) cs_realloc(NULL, 0, _size)

//
// static vars
//

static int is_verbose;

//
// Begin function definitions
//

// realloc that hololisp uses for all memory allocations internally.
// If new_size is 0, behaves as 'free'.
// If old_size is 0, behaves as 'calloc'
// Otherwise behaves as 'realloc'
static void *cs_sb_grow_impl(void *arr, size_t inc, size_t stride) {
    if (arr == NULL) {
        void *result = cs_alloc(sizeof(struct cs_sb_header) + stride * inc);
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
    void *result = cs_realloc(
        header, sizeof(struct cs_sb_header) + stride * header->capacity,
        sizeof(struct cs_sb_header) + stride * new_capacity);
    header = result;
    header->capacity = new_capacity;
    return header + 1;
}

static void *cs_realloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    if (old_size == 0) {
        assert(ptr == NULL);
        void *result = calloc(1, new_size);
        if (result == NULL) {
            perror("failed to allocate memory");
            exit(EXIT_FAILURE);
        }
        return result;
    }

    if (new_size == 0) {
        free(ptr);
        return NULL;
    }

    void *result = realloc(ptr, new_size);
    if (result == NULL) {
        perror("failed to allocate memory");
        exit(EXIT_FAILURE);
    }
    return result;
}

#if defined(__APPLE__)
static double cs_get_time(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
static double cs_getcputime(void) {
    struct task_thread_times_info thread_info_data;
    mach_msg_type_number_t thread_info_count = TASK_THREAD_TIMES_INFO_COUNT;
    kern_return_t kr =
        task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                  (task_info_t)&thread_info_data, &thread_info_count);
    (void)kr;
    return thread_info_data.user_time.seconds +
           thread_info_data.user_time.microseconds / 1e6 +
           thread_info_data.system_time.seconds +
           thread_info_data.system_time.microseconds / 1e6;
}
#else
static double cs_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double cs_getcputime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static void cs_print_help_and_exit(int rc) {
    printf("A command-line benchmarking tool\n"
           "\n"
           "Usage: csbench [OPTIONS] <command> ...\n"
           "\n"
           "Where options is one of:\n"
           "--warmup <n>     - specify warmup time in seconds\n"
           "--time-limit <n> - specify how long to run benchmarks\n"
           "--verbose        - print debug information\n"
           "--help           - print this message\n"
           "--version        - print version\n");
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

    int cursor = 1;
    while (cursor < argc) {
        const char *opt = argv[cursor++];
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            cs_print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(opt, "--version") == 0) {
            cs_print_version_and_exit();
        } else if (strcmp(opt, "--verbose") == 0) {
            is_verbose = 1;
        } else if (strcmp(opt, "--warmup") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            if (value < 0.0) {
                fprintf(stderr, "time limit must be positive number or zero\n");
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
                fprintf(stderr, "time limit must be positive number\n");
                exit(EXIT_FAILURE);
            }

            settings->time_limit = value;
        } else if (strcmp(opt, "--prepare") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *prepare_str = argv[cursor++];
            settings->prepare = prepare_str;
        } else {
            cs_sb_push(settings->commands, opt);
        }
    }

    if (cs_sb_len(settings->commands) == 0) {
        cs_print_help_and_exit(EXIT_FAILURE);
    }
}

static void *cs_timer_gettimeofday_allocate_state(void) {
    struct cs_timer_gettimeofday_state *state = cs_alloc(sizeof(*state));
    return state;
}

static void cs_timer_gettimeofday_begin_timing(void *s) {
    struct cs_timer_gettimeofday_state *state = s;
    gettimeofday(&state->start, NULL);
}
static void cs_timer_gettimeofday_end_timing(void *s) {
    struct cs_timer_gettimeofday_state *state = s;
    gettimeofday(&state->end, NULL);
}
static void cs_timer_gettimeofday_free_state(void *s) {
    struct cs_timer_gettimeofday_state *state = s;
    cs_free(state, sizeof(*state));
}
static double cs_timer_gettimeofday_get_units(void *s) {
    struct cs_timer_gettimeofday_state *state = s;
    double start = (double)state->start.tv_sec +
                   ((double)state->start.tv_usec / 1000000.0);
    double end =
        (double)state->end.tv_sec + ((double)state->end.tv_usec / 1000000.0);
    return end - start;
}

static const struct cs_measurement_ops cs_timer_gettimeofday_impl = {
    cs_timer_gettimeofday_allocate_state, cs_timer_gettimeofday_begin_timing,
    cs_timer_gettimeofday_end_timing,     cs_timer_gettimeofday_free_state,
    cs_timer_gettimeofday_get_units,
};

static int cs_execute_command_do_exec(const struct cs_command *command) {
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    switch (command->input_policy.kind) {
    case CS_INPUT_POLICY_NULL: {
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

static int cs_execute_command(const struct cs_command *command) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        int child_rc = cs_execute_command_do_exec(command);
        exit(child_rc);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }

    return status;
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
                current_word = NULL;
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
                current_word = NULL;
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
                current_word = NULL;
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

    return result;
}

static void cs_calculate_statistics(const double *values, size_t count,
                                    struct cs_statistics *stats) {
    double *sorted_array = malloc(sizeof(*values) * count);
    for (size_t i = 0; i < count; ++i)
        sorted_array[i] = values[i];

    qsort(sorted_array, count, sizeof(*values), cs_compare_doubles);
    stats->min = sorted_array[0];
    stats->max = sorted_array[count - 1];

    double sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += sorted_array[i];

    stats->sum = sum;
    stats->count = count;
    stats->mean = sum / count;

    stats->q1 = sorted_array[count / 4];
    stats->median = sorted_array[count / 2];
    stats->q3 = sorted_array[count * 3 / 4];
    stats->iqr = stats->q3 - stats->q1;

    double st_dev = 0;
    for (size_t i = 0; i < count; ++i) {
        double temp = sorted_array[i] - stats->mean;
        st_dev += temp * temp;
    }
    st_dev /= count;
    st_dev = sqrt(st_dev);
    stats->st_dev = st_dev;
    stats->max_deviation = fmax(fabs(stats->mean - sorted_array[0]),
                                fabs(stats->mean - sorted_array[count - 1]));
    stats->confidence_interval = 0.95 * stats->st_dev / sqrt(count);
    stats->cv = stats->st_dev / stats->mean;

    free(sorted_array);
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

static double cs_stat_std_dev(const double *v, size_t count) {
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
cs_bootstrap(std_dev, cs_stat_std_dev)

    // clang-format on

    static int print_time(char *dst, size_t sz, double t) {
    int count = 0;
    if (t < 0) {
        t = -t;
        count = snprintf(dst, sz, "-");
        dst += count;
        sz -= count;
    }

    const char *units = "s";
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

static void cs_measure(struct cs_benchmark *bench) {
    void *state = bench->timer->allocate_state();
    bench->timer->begin_timing(state);

    int rc = cs_execute_command(bench->command);

    bench->timer->end_timing(state);
    double unit = bench->timer->get_units(state);
    bench->timer->free_state(state);

    cs_sb_push(bench->exit_codes, rc);
    cs_sb_push(bench->sample.values, unit);
}

static void cs_warmup(struct cs_benchmark *bench, double time_limit) {
    if (time_limit == 0.0) {
        if (is_verbose)
            printf("LOG: skipping warmup\n");
        return;
    }

    if (is_verbose)
        printf("LOG: starting warmup\n");

    double start_time = cs_get_time();
    size_t count = 0;
    for (;; ++count) {
        cs_execute_command(bench->command);

        double end_time = cs_get_time();
        if (end_time - start_time > time_limit)
            break;
    }

    if (is_verbose)
        printf("LOG: finihsed warmup, %zu runs\n", count);
}

static void cs_execute_prepare(const struct cs_command *prepare) {
    if (prepare == NULL)
        return;

    cs_execute_command(prepare);
}

static void cs_run_benchmark(struct cs_benchmark *bench, double time_limit,
                             double warmup_time) {
    cs_warmup(bench, warmup_time);

    double niter_accum = 1;
    size_t niter = 1;

    double start_time = cs_get_time();
    for (size_t count = 0;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            cs_execute_prepare(bench->prepare);
            cs_measure(bench);
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
}

static void cs_analyze_benchmark(struct cs_benchmark *bench) {
    const double *data = bench->sample.values;
    size_t data_size = cs_sb_len(bench->sample.values);

    struct cs_statistics stats = {0};
    cs_calculate_statistics(data, data_size, &stats);

    size_t nresamples = 100000;
    double min_mean, max_mean;
    cs_bootstrap_mean(data, data_size, nresamples, time(NULL), &min_mean,
                      &max_mean);
    double min_std_dev, max_std_dev;
    cs_bootstrap_std_dev(data, data_size, nresamples, time(NULL), &min_std_dev,
                         &max_std_dev);

    printf("command\t'%s'\n", bench->command->str);
    printf("%zu runs\n", data_size);
    char buf1[256], buf2[256], buf3[256];
    print_time(buf1, sizeof(buf1), min_mean);
    print_time(buf2, sizeof(buf2), stats.mean);
    print_time(buf3, sizeof(buf3), max_mean);
    printf("mean   \t%s\t%s\t%s\n", buf1, buf2, buf3);
    print_time(buf1, sizeof(buf1), min_std_dev);
    print_time(buf2, sizeof(buf1), stats.st_dev);
    print_time(buf3, sizeof(buf1), max_std_dev);
    printf("std dev\t%s\t%s\t%s\n", buf1, buf2, buf3);

    struct cs_outliers outliers = cs_classify_outliers(data, data_size);
    size_t outlier_count = outliers.low_mild + outliers.high_mild +
                           outliers.low_severe + outliers.high_severe;
    if (outlier_count != 0) {
        printf("found %zu outliers across %zu measurements (%.2f%%)\n",
               outlier_count, data_size,
               (double)outlier_count / data_size * 100.0);
        if (outliers.low_severe)
            printf("%zu (%.2f%%) low severe\n", outliers.low_severe,
                   (double)outliers.low_severe / data_size * 100.0);
        if (outliers.low_mild)
            printf("%zu (%.2f%%) low mild\n", outliers.low_mild,
                   (double)outliers.low_mild / data_size * 100.0);
        if (outliers.high_mild)
            printf("%zu (%.2f%%) high mild\n", outliers.high_mild,
                   (double)outliers.high_mild / data_size * 100.0);
        if (outliers.high_severe)
            printf("%zu (%.2f%%) high severe\n", outliers.high_severe,
                   (double)outliers.high_severe / data_size * 100.0);
    }
}

static double cs_ols_regress(double *x, double *y, size_t count) {
    double xy = 0.0;
    for (size_t i = 0; i < count; ++i)
        xy += x[i] * y[i];

    double xx = 0.0;
    for (size_t i = 0; i < count; ++i)
        xx += x[i] * x[i];

    return xy / xx;
}

static double cs_r_squared(double *y, size_t count, double slope) {
    double m = slope;
    double n = (double)count;
    double y_bar = 0.0;
    for (size_t i = 0; i < count; ++i)
        y_bar += y[i];
    y_bar /= n;

    double rss = 0.0;
    double tss = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double t = y[i] - m;
        rss += t * t;
        t = y[i] - y_bar;
        tss += t * t;
    }

    return 1.0 - rss / tss;
}

static int cs_extract_executable_and_argv(const char *command_str,
                                          char **executable, char ***argv) {
    char **words = cs_split_shell_words(command_str);
    if (words == NULL) {
        fprintf(stderr, "Invalid command syntax\n");
        return -1;
    }

    if (is_verbose) {
        printf("LOG: split command '%s' into words [", command_str);
        for (size_t i = 0; i < cs_sb_len(words); ++i) {
            printf("'%s'%s", words[i], i != cs_sb_len(words) - 1 ? "," : "");
        }
        printf("]\n");
    }

    char *real_exec_path = NULL;
    const char *exec_path = words[0];
    if (*exec_path == '/') {
        real_exec_path = strdup(exec_path);
    } else if (strchr(exec_path, '/') != NULL) {
        char path[4096];
        if (getcwd(path, sizeof(path)) == NULL) {
            perror("getcwd");
            goto error;
        }

        size_t cwd_len = strlen(path);
        snprintf(path + cwd_len, sizeof(path) - cwd_len, "/%s", exec_path);

        real_exec_path = strdup(path);
    } else {
        const char *path_var = getenv("PATH");
        if (path_var == NULL) {
            fprintf(stderr, "$PATH variable is not set\n");
            goto error;
        }
        const char *path_var_end = path_var + strlen(path_var);
        const char *cursor = path_var;
        while (cursor != path_var_end + 1) {
            const char *next_sep = strchr(cursor, ':');
            if (next_sep == NULL)
                next_sep = path_var_end;

            char path[4096];
            snprintf(path, sizeof(path), "%.*s/%s", (int)(next_sep - cursor),
                     cursor, exec_path);

            if (access(path, X_OK) == 0) {
                real_exec_path = strdup(path);
                break;
            }

            cursor = next_sep + 1;
        }
    }

    if (real_exec_path == NULL) {
        fprintf(stderr, "error: failed to find executable path for command '%s'\n",
                command_str);
        goto error;
    }

    if (is_verbose) {
        printf("LOG: command '%s' has executable path '%s' and arguments [",
               command_str, real_exec_path);
        for (size_t i = 1; i < cs_sb_len(words); ++i) {
            printf("'%s'%s", words[i], i != cs_sb_len(words) - 1 ? "," : "");
        }
        printf("]\n");
    }

    *executable = real_exec_path;
    *argv = words;

    return 0;
error:
    for (size_t i = 0; i < cs_sb_len(words); ++i)
        cs_sb_free(words[i]);
    cs_sb_free(words);
    return -1;
}

static int init_app(const struct cs_settings *settings, struct cs_app *app) {
    app->settings = *settings;

    size_t command_count = cs_sb_len(settings->commands);
    for (size_t command_idx = 0; command_idx < command_count; ++command_idx) {
        struct cs_command command = {0};
        const char *command_str = settings->commands[command_idx];
        command.str = command_str;
        if (cs_extract_executable_and_argv(command_str, &command.executable,
                                           &command.argv) != 0) {
            cs_sb_free(app->commands);
            return -1;
        }
        command.input_policy = settings->input_policy;
        command.output_policy = settings->output_policy;

        cs_sb_push(app->commands, command);
    }

    if (settings->prepare) {
        if (is_verbose)
            printf("LOG: has prepare command: '%s'\n", settings->prepare);

        const char *command_str = settings->prepare;

        app->has_prepare_command = 1;
        if (cs_extract_executable_and_argv(command_str,
                                           &app->prepare_command.executable,
                                           &app->prepare_command.argv) != 0) {
            return -1;
        }
        app->prepare_command.input_policy.kind = CS_INPUT_POLICY_NULL;
        app->prepare_command.output_policy.kind = CS_OUTPUT_POLICY_NULL;
    }

    return -1;
}

int main(int argc, char **argv) {
    struct cs_settings settings = {0};
    cs_parse_cli_args(argc, argv, &settings);

    struct cs_app app = {0};
    init_app(&settings, &app);

    size_t command_count = cs_sb_len(app.commands);
    for (size_t command_idx = 0; command_idx < command_count; ++command_idx) {
        struct cs_benchmark bench = {0};
        if (app.has_prepare_command)
            bench.prepare = &app.prepare_command;
        bench.command = app.commands + command_idx;
        bench.timer = &cs_timer_gettimeofday_impl;
        cs_run_benchmark(&bench, settings.time_limit, settings.warmup_time);
        cs_analyze_benchmark(&bench);
    }

    return EXIT_SUCCESS;
}
