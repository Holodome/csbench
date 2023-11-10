#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void cs_init_time(void);
static double cs_get_time(void);
static double cs_get_cputime(void);

#if defined(__APPLE__)
static void cs_init_time(void) {
}
static double cs_get_time(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
static double cs_to_double(time_value_t time) {
    return time.seconds + time.microseconds / 1e6;
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
static void cs_init_time(void) {
}
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

enum cs_input_policy_kind {
    CS_INPUT_POLICY_NULL,
    CS_INPUT_POLICY_FILE
};

struct cs_input_policy {
    enum cs_input_policy_kind kind;
    const char *file;
};

enum cs_output_policy_kind {
    CS_OUTPUT_POLICY_NULL,
    CS_OUTPUT_POLICY_INHERIT
};

struct cs_output_policy {
    enum cs_output_policy_kind kind;
};

struct cs_settings {
    // Array of commands to benchmark
    const char **commands;
    double time_limit;

    struct cs_input_policy input_policy;
    struct cs_output_policy output_policy;
};

static void cs_print_help_and_exit(int rc) {
    printf("A command-line benchmarking tool\n"
           "\n"
           "Usage: csbench [OPTIONS] <command> ...\n");
    exit(rc);
}

static void cs_print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static void cs_parse_cli_args(int argc, char **argv,
                              struct cs_settings *settings) {
    settings->time_limit = 1.0;

    int cursor = 1;
    while (cursor < argc) {
        const char *opt = argv[cursor++];
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            cs_print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(opt, "--version") == 0) {
            cs_print_version_and_exit();
        } else if (strcmp(opt, "--time-limit") == 0) {
            if (cursor >= argc)
                cs_print_help_and_exit(EXIT_FAILURE);

            const char *runs_str = argv[cursor++];
            char *str_end;
            double value = strtod(runs_str, &str_end);
            if (str_end == runs_str)
                cs_print_help_and_exit(EXIT_FAILURE);

            settings->time_limit = value;
        } else {
            cs_sb_push(settings->commands, opt);
        }
    }

    if (cs_sb_len(settings->commands) == 0) {
        cs_print_help_and_exit(EXIT_FAILURE);
    }
}

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

struct cs_benchmark {
    const char *command;
    const struct cs_measurement_ops *timer;
    double *times;
    int *exit_codes;
};

static int cs_execute_command(const char *command) {
    int rc = system(command);
    return rc;
}

struct cs_outliers {
    size_t samples_seen;
    size_t low_severe;
    size_t low_mild;
    size_t high_mild;
    size_t high_severe;
};

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

    struct cs_outliers result;
    memset(&result, 0, sizeof(result));
    result.samples_seen = count;
    for (size_t i = 0; i < count; ++i) {
        double v = data[i];
        if (v >= los && v < him)
            ++result.low_severe;
        if (v > los && v < lom)
            ++result.low_mild;
        if (v >= him && v < his)
            ++result.high_mild;
        if (v >= his && v > lom)
            ++result.high_severe;
    }

    return result;
}

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
};

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

    free(sorted_array);
}

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

static void cs_measure(struct cs_benchmark *bench, size_t niter) {
    for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
        void *state = bench->timer->allocate_state();
        bench->timer->begin_timing(state);

        int rc = cs_execute_command(bench->command);

        bench->timer->end_timing(state);
        double unit = bench->timer->get_units(state);
        bench->timer->free_state(state);

        cs_sb_push(bench->exit_codes, rc);
        cs_sb_push(bench->times, unit);
    }
}

static void cs_run_benchmark(struct cs_benchmark *bench, double time_limit) {
    double niter_accum = 1;
    size_t niter = 1;

    cs_init_time();
    double start_time = cs_get_time();
    for (size_t count = 0;; ++count) {
        cs_measure(bench, niter);

        // The only loop exit policy is time elapsed
        double end_time = cs_get_time();
        if (end_time - start_time > time_limit && count >= 4)
            break;

        // Select new niter
        {
            niter_accum *= 1.05;
            size_t new_niter = (size_t)floor(niter_accum);
            if (new_niter != niter)
                break;
        }
    }
}

static void cs_analyze_benchmark(struct cs_benchmark *bench) {
    const double *data = bench->times;
    size_t data_size = cs_sb_len(bench->times);

    struct cs_statistics stats;
    memset(&stats, 0, sizeof(stats));

    cs_calculate_statistics(data, data_size, &stats);

    char buf[256];
    print_time(buf, sizeof(buf), stats.mean);
    printf("mean\t%s\n", buf);
    print_time(buf, sizeof(buf), stats.st_dev);
    printf("std dev\t%s\n", buf);
}

int main(int argc, char **argv) {
    struct cs_settings settings;
    memset(&settings, 0, sizeof(settings));
    cs_parse_cli_args(argc, argv, &settings);

    struct cs_benchmark bench;
    memset(&bench, 0, sizeof(bench));
    bench.command = settings.commands[0];
    bench.timer = &cs_timer_gettimeofday_impl;

    cs_run_benchmark(&bench, settings.time_limit);
    cs_analyze_benchmark(&bench);

    return EXIT_SUCCESS;
}
