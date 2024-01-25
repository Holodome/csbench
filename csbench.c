#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

// This is implementation of type-safe generic vector in C based on
// std_stretchy_buffer.
struct sb_header {
    size_t size;
    size_t capacity;
};

enum input_kind {
    INPUT_POLICY_NULL,
    // load input from file (supplied later)
    INPUT_POLICY_FILE
};

// How to handle input of command?
struct input_policy {
    enum input_kind kind;
    const char *file;
};

enum output_kind {
    OUTPUT_POLICY_NULL,
    // Print output to controlling terminal
    OUTPUT_POLICY_INHERIT,
};

enum export_kind {
    DONT_EXPORT,
    EXPORT_JSON
};

struct export_policy {
    enum export_kind kind;
    const char *filename;
};

enum analyze_mode {
    DONT_ANALYZE,
    ANALYZE_PLOT,
    ANALYZE_HTML
};

struct bench_stop_policy {
    double time_limit;
    int runs;
    int min_runs;
    int max_runs;
};

enum units_kind {
    MU_S,
    MU_MS,
    MU_US,
    MU_NS,
    MU_CUSTOM,
    MU_NONE
};

struct units {
    enum units_kind kind;
    const char *str;
};

enum meas_kind {
    MEAS_CUSTOM,
    MEAS_WALL,
    MEAS_RUSAGE_UTIME,
    MEAS_RUSAGE_STIME,
};

struct meas {
    const char *name;
    const char *cmd;
    struct units units;
    enum meas_kind kind;
    int is_secondary;
    size_t primary_idx;
};

struct bench_param {
    char *name;
    char **values;
};

// This structure contains all information
// supplied by user prior to benchmark start.
struct cli_settings {
    const char **cmds;
    const char *shell;
    struct export_policy export;
    struct meas *meas;
    const char *prepare;
    struct input_policy input;
    enum output_kind output;
    const char *out_dir;
    enum analyze_mode analyze_mode;
    struct bench_param *params;
};

// Description of command to benchmark.
// Commands are executed using execve.
struct cmd {
    char *str;
    char *exec;
    char **argv;
    struct input_policy input;
    enum output_kind output;
    struct meas *meas;
};

struct cmd_group {
    char *template;
    const char *var_name;
    size_t count;
    size_t *cmd_idxs;
    const char **var_values;
};

// Information gethered from user input (settings), parsed
// and prepared for benchmarking. Some fields are copied from
// cli settings as is to reduce data dependencies.
struct settings {
    struct cmd *cmds;
    struct cmd_group *cmd_groups;
    struct meas *meas;
    const char *prepare_cmd;
    struct export_policy export;
    enum analyze_mode analyze_mode;
    const char *out_dir;
};

// Boostrap estimate of certain statistic. Contains lower and upper bounds, as
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
    const double *data;
    size_t count;
    struct est mean;
    struct est st_dev;
    double min;
    double max;
    double q1;
    double q3;
    double p1;
    double p5;
    double p95;
    double p99;
    struct outliers outliers;
};

struct bench {
    const char *prepare;
    const struct cmd *cmd;
    size_t run_count;
    int *exit_codes;
    size_t meas_count;
    double **meas;
};

struct bench_analysis {
    const struct bench *bench;
    struct distr *meas;
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
};

struct ols_regress {
    enum big_o complexity;
    // function is of the form f(x) = a * F(x) + b where F(x) is determined
    // by complexity, a is result of OLS, and b is minimal time (seems to make
    // models more consistent in cases where latency is high).
    double a;
    double b;
    double rms;
};

struct cmd_group_analysis {
    const struct meas *meas;
    const struct cmd_group *group;
    size_t cmd_count;
    struct cmd_in_group_data *data;
    const struct cmd_in_group_data *slowest;
    const struct cmd_in_group_data *fastest;
    int values_are_doubles;
    struct ols_regress regress;
};

struct bench_results {
    size_t bench_count;
    struct bench *benches;
    struct bench_analysis *analyses;
    size_t meas_count;
    size_t *fastest_meas;
    const struct meas *meas;
    size_t group_count;
    struct cmd_group_analysis **group_analyses;
};

struct cpu_time {
    double user_time;
    double system_time;
};

// data needed to construct kde plot. data here is kde points computed from
// original data
struct kde_plot {
    const struct distr *distr;
    const char *title;
    const struct meas *meas;
    double lower;
    double step;
    double *data;
    size_t count;
    double mean;
    double mean_y;
    const char *output_filename;
    int is_ext;
};

struct parfor_data {
    pthread_t id;
    void *arr;
    size_t stride;
    size_t low;
    size_t high;
    int (*fn)(void *);
};

struct prettify_plot {
    const char *units_str;
    double multiplier;
    int logscale;
};

#define sb_header(_a)                                                          \
    ((struct sb_header *)((char *)(_a) - sizeof(struct sb_header)))
#define sb_size(_a) (sb_header(_a)->size)
#define sb_capacity(_a) (sb_header(_a)->capacity)

#define sb_needgrow(_a, _n)                                                    \
    (((_a) == NULL) || (sb_size(_a) + (_n) >= sb_capacity(_a)))
#define sb_maybegrow(_a, _n) (sb_needgrow(_a, _n) ? sb_grow(_a, _n) : 0)
#define sb_grow(_a, _b)                                                        \
    (*(void **)(&(_a)) = sb_grow_impl((_a), (_b), sizeof(*(_a))))

#define sb_free(_a) free((_a) != NULL ? sb_header(_a) : NULL)
#define sb_push(_a, _v) (sb_maybegrow(_a, 1), (_a)[sb_size(_a)++] = (_v))
#define sb_last(_a) ((_a)[sb_size(_a) - 1])
#define sb_len(_a) (((_a) != NULL) ? sb_size(_a) : 0)
#define sb_pop(_a) ((_a)[--sb_size(_a)])
#define sb_purge(_a) ((_a) ? (sb_size(_a) = 0) : 0)

static __thread uint32_t g_rng_state;
// These are applicaton settings made global. Only put small settings with
// trivial types that don't require allocation/deallocation.
static int g_allow_nonzero = 0;
static double g_warmup_time = 0.1;
static int g_threads = 1;
static int g_plot_src = 0;
static int g_nresamp = 100000;
static struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};

static void *sb_grow_impl(void *arr, size_t inc, size_t stride) {
    if (arr == NULL) {
        void *result = calloc(sizeof(struct sb_header) + stride * inc, 1);
        struct sb_header *header = result;
        header->size = 0;
        header->capacity = inc;
        return header + 1;
    }

    struct sb_header *header = sb_header(arr);
    size_t double_current = header->capacity * 2;
    size_t min = header->size + inc;

    size_t new_capacity = double_current > min ? double_current : min;
    void *result =
        realloc(header, sizeof(struct sb_header) + stride * new_capacity);
    header = result;
    header->capacity = new_capacity;
    return header + 1;
}

// clang-format off
static void print_help_and_exit(int rc) {
    printf(
"A command line benchmarking tool\n"
"\n"
"Usage: csbench [OPTIONS] <command>...\n"
"\n"
"Arguments:\n"
"  <command>...\n"
"          The command to benchmark. Can be a shell command line, like \n"
"          'ls $(pwd) && echo 1', or a direct executable invocation, like \n"
"          'sleep 0.5'. Former is not available when --shell none is specified.\n" 
"          Can contain parameters in the form 'sleep {n}', see --scan family \n"
"          of options. If multiple commands are given, their comparison will be\n"
"          performed.\n"
"\n"
    );
    printf(
"Options:\n"
"  -W, --warmup <t>\n"
"          Perform warump runs for at least <t> seconds before actual benchmark\n"
"          of each command.\n"
"  -R, --runs <n>\n"
"          Perform exactly <n> benchmark runs of each command. This option \n"
"          overrides --time-limit, --min-runs and --max-runs.\n"
"  -T, --time-limit <t>\n"
"          Run each benchmark for at least <t> seconds.\n"
"  --min-runs <n>\n"
"          Run each benchmark at least <n> times, used in conjunction with \n"
"          --time-limit and --max-runs.\n"
"  --max-runs <n>\n"
"          Run each benchmark at most <n> times, used in conjunction with \n"
"          --time-limit and --min-runs.\n"
"  -P, --prepare <cmd>\n"
"          Execute <cmd> in default shell before each benchmark run.\n"
"  --nrs <n>\n"
"          Specify number of resamples used in boostrapping. Default value is\n"
"          100000\n"
    );
    printf(
"  -S, --shell <cmd>\n"
"          Specify shell used for executing commands. Can be both shell name,\n"
"          like 'bash', or command line like 'bash --norc'. Either way, '-c'\n"
"          and benchmarked command are appended to argument list. <cmd> can\n"
"          also be none specifying that commands should be executed without a\n"
"          shell directly with exec.\n"
"  --output <where>\n"
"          Specify what to do with benchmarked commands' stdout and stdder.\n"
"          Can be set to 'inherit' - output will be printed to terminal, or\n"
"          'none' - output will be piped to /dev/null. The latter is the\n"
"          default option.\n"
"  --input <where>\n"
"          Specify how each command should receive its input. <where> can be a\n"
"          file name, or none. In the latter case /dev/null is piped to stdin.\n"
"  --custom <name>\n"
"          Add custom measurement with <name>. Attempts to parse real value\n"
"          from each command's stdout and interprets it in seconds.\n"
"  --custom-t <name> <cmd>\n"
"          Add custom measurement with <name>. Pipes each commands stdout to\n"
"          <cmd> and tries to parse real value from its output and interprets\n"
"          it in seconds. This can be used to extract a number, for example,\n"
"          using grep. Alias for --custom-x <name> 's' <cmd>.\n"
    );
    printf(
"  --custom-x <name> <units> <cmd>\n"
"          Add custom measurement with <name>. Pipes each commands stdout to\n"
"          <cmd> and tries to parse real value from its output and interprets\n"
"          it in <units>. <units> can be one of the time units 's', 'ms','us',\n"
"          'ns', in which case results will pretty printed. If <units> is\n"
"          'none', no units are printed. Alternatively <units> can be any\n"
"          string.\n"
"  --scan <i>/<n>/<m>[/<s>]\n"
"          Add parameter with name <i> running in range from <n> to <m> with\n"
"          step <s>. <s> is optional, default is 1. Can be used from commandin\n"
"          the form '{<i>}'.\n"
"  --scan <i>/v[,...]\n"
"          Add paramter with name <i> running values from comma separated list\n"
"          <v>.\n"
"  -j, --jobs <n>\n"
"          Execute benchmarks in parallel with <n> threads. Default option is\n"
"          to execute all benchmarks sequentially\n"
"  --export-json <f>\n"
"          Export benchmark results without analysis as json.\n"
"  -o, --out-dir <d>\n"
"          Specify directory where plots, html report and other analysis\n"
"          results will be placed. Default is '.csbench' in current directory.\n"
    );
    printf(
"  --plot\n"
"          Generate plots. For each benchmark KDE is generated in two variants.\n"
"          For each paramter (--scan and --scanl) paramter values are plotted\n"
"          against mean time. Single violin plot is produced if multiple\n"
"          commands are specified. For each measurement (--custom and others)\n"
"          its own group of plots is generated. Also readme.md file is\n"
"          generated, which helps to decipher plot file names.\n"
"  --plot-src\n"
"          Next to each plot file place python script used to produce it. Can\n"
"          be used to quickly patch up plots for presentation.\n"
"  --html\n"
"          Genereate html report. Implies --plot.\n"
"  --no-wall\n"
"          Exclude wall clock information from command line output, plots, html\n"
"          report. Commonly used with custom measurements (--custom and others)\n"
"          when wall clock information is excessive.\n"
"  --allow-nonzero\n"
"          Accept commands with non-zero exit code. Default behaviour is to\n"
"          abort benchmarking.\n"
"  --help\n"
"          Print help.\n"
"  --version\n"
"          Print version.\n"
    );
    exit(rc);
}
// clang-format on

static void print_version_and_exit(void) {
    printf("csbench 0.1\n");
    exit(EXIT_SUCCESS);
}

static int parse_range_scan_settings(const char *settings, char **namep,
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

static char **range_to_param_list(double low, double high, double step) {
    assert(high > low);
    char **result = NULL;
    for (double cursor = low; cursor <= high + 0.000001; cursor += step) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%g", cursor);
        sb_push(result, strdup(buf));
    }
    return result;
}

static int parse_scan_list_settings(const char *settings, char **namep,
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

static char **parse_scan_list(const char *scan_list) {
    char **param_list = NULL;
    const char *cursor = scan_list;
    const char *end = scan_list + strlen(scan_list);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            sb_push(param_list, strdup(cursor));
            break;
        }

        size_t param_len = next - cursor;
        char *param = malloc(param_len + 1);
        memcpy(param, cursor, param_len);
        param[param_len] = '\0';
        sb_push(param_list, param);

        cursor = next + 1;
    }

    return param_list;
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
    } else if (strcmp(str, "none") == 0) {
        units->kind = MU_NONE;
    } else {
        units->kind = MU_CUSTOM;
        units->str = str;
    }
}

static void parse_cli_args(int argc, char **argv,
                           struct cli_settings *settings) {
    settings->shell = "/bin/sh";
    settings->out_dir = ".csbench";
    int no_wall = 0;
    struct meas *meas_list = NULL;

    int cursor = 1;
    while (cursor < argc) {
        const char *opt = argv[cursor++];
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            print_help_and_exit(EXIT_SUCCESS);
        } else if (strcmp(opt, "--version") == 0) {
            print_version_and_exit();
        } else if (strcmp(opt, "--warmup") == 0 || strcmp(opt, "-W") == 0) {
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
            g_warmup_time = value;
        } else if (strcmp(opt, "--time-limit") == 0 || strcmp(opt, "-T") == 0) {
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
            g_bench_stop.time_limit = value;
        } else if (strcmp(opt, "--runs") == 0 || strcmp(opt, "-R") == 0) {
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

            g_bench_stop.runs = value;
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
            g_bench_stop.min_runs = value;
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
            g_bench_stop.max_runs = value;
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
            g_nresamp = value;
        } else if (strcmp(opt, "--shell") == 0 || strcmp(opt, "-S") == 0) {
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
                settings->output = OUTPUT_POLICY_NULL;
            else if (strcmp(out, "inherit") == 0)
                settings->output = OUTPUT_POLICY_INHERIT;
            else
                print_help_and_exit(EXIT_FAILURE);
        } else if (strcmp(opt, "--input") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --input requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *input = argv[cursor++];
            if (strcmp(input, "null") == 0) {
                settings->input.kind = INPUT_POLICY_NULL;
            } else {
                settings->input.kind = INPUT_POLICY_FILE;
                settings->input.file = input;
            }
        } else if (strcmp(opt, "--custom") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --custom requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            struct meas meas = {0};
            meas.name = name;
            meas.cmd = "cat";
            sb_push(meas_list, meas);
        } else if (strcmp(opt, "--custom-t") == 0) {
            if (cursor + 1 >= argc) {
                fprintf(stderr, "error: --custom-t requires 2 arguments\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct meas meas = {0};
            meas.name = name;
            meas.cmd = cmd;
            sb_push(meas_list, meas);
        } else if (strcmp(opt, "--custom-x") == 0) {
            if (cursor + 2 >= argc) {
                fprintf(stderr, "error: --custom-x requires 3 arguments\n");
                exit(EXIT_FAILURE);
            }
            const char *name = argv[cursor++];
            const char *units = argv[cursor++];
            const char *cmd = argv[cursor++];
            struct meas meas = {0};
            meas.name = name;
            meas.cmd = cmd;
            parse_units_str(units, &meas.units);
            sb_push(meas_list, meas);
        } else if (strcmp(opt, "--scan") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --scan requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *scan_settings = argv[cursor++];
            double low, high, step;
            char *name;
            if (!parse_range_scan_settings(scan_settings, &name, &low, &high,
                                           &step)) {
                fprintf(stderr, "error: invalid --scan argument\n");
                exit(EXIT_FAILURE);
            }
            char **param_list = range_to_param_list(low, high, step);
            struct bench_param param = {0};
            param.name = name;
            param.values = param_list;
            sb_push(settings->params, param);
        } else if (strcmp(opt, "--scanl") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --scanl requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *scan_settings = argv[cursor++];
            char *name, *scan_list;
            if (!parse_scan_list_settings(scan_settings, &name, &scan_list)) {
                fprintf(stderr, "error: invalid --scanl argument\n");
                exit(EXIT_FAILURE);
            }
            char **param_list = parse_scan_list(scan_list);
            free(scan_list);
            struct bench_param param = {0};
            param.name = name;
            param.values = param_list;
            sb_push(settings->params, param);
        } else if (strcmp(opt, "--jobs") == 0 || strcmp(opt, "-j") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --jobs requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *threads_str = argv[cursor++];
            char *str_end;
            long value = strtol(threads_str, &str_end, 10);
            if (str_end == threads_str) {
                fprintf(stderr, "error: invalid --jobs argument\n");
                exit(EXIT_FAILURE);
            }
            if (value <= 0) {
                fprintf(stderr, "error: jobs count must be positive number\n");
                exit(EXIT_FAILURE);
            }
            g_threads = value;
        } else if (strcmp(opt, "--export-json") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --export-json requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *export_filename = argv[cursor++];
            settings->export.kind = EXPORT_JSON;
            settings->export.filename = export_filename;
        } else if (strcmp(opt, "--out-dir") == 0 || strcmp(opt, "-o") == 0) {
            if (cursor >= argc) {
                fprintf(stderr, "error: --out-dir requires 1 argument\n");
                exit(EXIT_FAILURE);
            }
            const char *dir = argv[cursor++];
            settings->out_dir = dir;
        } else if (strcmp(opt, "--html") == 0) {
            settings->analyze_mode = ANALYZE_HTML;
        } else if (strcmp(opt, "--plot") == 0) {
            settings->analyze_mode = ANALYZE_PLOT;
        } else if (strcmp(opt, "--plot-src") == 0) {
            g_plot_src = 1;
        } else if (strcmp(opt, "--no-wall") == 0) {
            no_wall = 1;
        } else if (strcmp(opt, "--allow-nonzero") == 0) {
            g_allow_nonzero = 1;
        } else {
            if (*opt == '-') {
                fprintf(stderr, "error: unknown option %s\n", opt);
                exit(EXIT_FAILURE);
            }
            sb_push(settings->cmds, opt);
        }
    }

    if (!no_wall) {
        sb_push(settings->meas,
                ((struct meas){
                    "wall clock time", NULL, {MU_S, NULL}, MEAS_WALL, 0, 0}));
        sb_push(settings->meas,
                ((struct meas){
                    "systime", NULL, {MU_S, NULL}, MEAS_RUSAGE_STIME, 1, 0}));
        sb_push(settings->meas,
                ((struct meas){
                    "usrtime", NULL, {MU_S, NULL}, MEAS_RUSAGE_UTIME, 1, 0}));
    }
    for (size_t i = 0; i < sb_len(meas_list); ++i)
        sb_push(settings->meas, meas_list[i]);
    sb_free(meas_list);
}

static void free_cli_settings(struct cli_settings *settings) {
    for (size_t i = 0; i < sb_len(settings->params); ++i) {
        struct bench_param *param = settings->params + i;
        free(param->name);
        for (size_t j = 0; j < sb_len(param->values); ++j)
            free(param->values[j]);
        sb_free(param->values);
    }
    sb_free(settings->cmds);
    sb_free(settings->meas);
    sb_free(settings->params);
}

static int replace_str(char *buf, size_t buf_size, const char *src,
                       const char *name, const char *value) {
    char *buf_end = buf + buf_size;
    size_t param_name_len = strlen(name);
    char *wr_cursor = buf;
    const char *rd_cursor = src;
    while (*rd_cursor) {
        if (*rd_cursor == '{' &&
            strncmp(rd_cursor + 1, name, param_name_len) == 0 &&
            rd_cursor[param_name_len + 1] == '}') {
            rd_cursor += 2 + param_name_len;
            size_t len = strlen(value);
            if (wr_cursor + len >= buf_end)
                return -1;
            memcpy(wr_cursor, value, len);
            wr_cursor += len;
        } else {
            if (wr_cursor >= buf_end)
                return -1;
            *wr_cursor++ = *rd_cursor++;
        }
    }
    if (wr_cursor >= buf_end)
        return -1;
    *wr_cursor = '\0';
    return 0;
}

static char **split_shell_words(const char *cmd) {
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
                state = STATE_DELIMETER;
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
                state = STATE_DELIMETER;
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
                state = STATE_DELIMETER;
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
    for (size_t i = 0; i < sb_len(words); ++i)
        sb_free(words[i]);
    sb_free(words);
    words = NULL;
out:
    return words;
}

static int extract_exec_and_argv(const char *cmd_str, char **exec,
                                 char ***argv) {
    char **words = split_shell_words(cmd_str);
    if (words == NULL) {
        fprintf(stderr, "error: invalid command syntax\n");
        return -1;
    }

    *exec = strdup(words[0]);
    sb_push(*argv, strdup(words[0]));
    for (size_t i = 1; i < sb_len(words); ++i)
        sb_push(*argv, strdup(words[i]));
    sb_push(*argv, NULL);

    for (size_t i = 0; i < sb_len(words); ++i)
        sb_free(words[i]);
    sb_free(words);
    return 0;
}

static int init_cmd_exec(const char *shell, const char *cmd_str,
                         struct cmd *cmd) {
    if (shell) {
        if (extract_exec_and_argv(shell, &cmd->exec, &cmd->argv) != 0)
            return -1;
        // pop NULL
        (void)sb_pop(cmd->argv);
        sb_push(cmd->argv, strdup("-c"));
        sb_push(cmd->argv, strdup(cmd_str));
        sb_push(cmd->argv, NULL);
    } else {
        if (extract_exec_and_argv(cmd_str, &cmd->exec, &cmd->argv) != 0)
            return -1;
    }
    cmd->str = strdup(cmd_str);
    return 0;
}

static void free_settings(struct settings *settings) {
    for (size_t i = 0; i < sb_len(settings->cmds); ++i) {
        struct cmd *cmd = settings->cmds + i;
        free(cmd->exec);
        for (char **word = cmd->argv; *word; ++word)
            free(*word);
        sb_free(cmd->argv);
        free(cmd->str);
    }
    for (size_t i = 0; i < sb_len(settings->cmd_groups); ++i) {
        struct cmd_group *group = settings->cmd_groups + i;
        free(group->template);
        free(group->cmd_idxs);
        free(group->var_values);
    }
    sb_free(settings->cmds);
    sb_free(settings->cmd_groups);
}

static int init_settings(const struct cli_settings *cli,
                         struct settings *settings) {
    settings->export = cli->export;
    settings->prepare_cmd = cli->prepare;
    settings->analyze_mode = cli->analyze_mode;
    settings->out_dir = cli->out_dir;
    settings->meas = cli->meas;

    // try to catch invalid file as early as possible,
    // because later error handling can become troublesome (after fork()).
    if (cli->input.kind == INPUT_POLICY_FILE &&
        access(cli->input.file, R_OK) == -1) {
        fprintf(stderr,
                "error: file specified as command input is not accessable "
                "(%s)\n",
                cli->input.file);
        return -1;
    }

    size_t cmd_count = sb_len(cli->cmds);
    if (cmd_count == 0) {
        fprintf(stderr, "error: no commands specified\n");
        return -1;
    }
    if (sb_len(cli->meas) == 0) {
        fprintf(stderr, "error: no measurements specified\n");
        return -1;
    }

    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->cmds[i];
        int found_param = 0;
        for (size_t j = 0; j < sb_len(cli->params); ++j) {
            const struct bench_param *param = cli->params + j;
            char buf[4096];
            snprintf(buf, sizeof(buf), "{%s}", param->name);
            if (strstr(cmd_str, buf) == NULL)
                continue;

            size_t its_in_group = sb_len(param->values);
            found_param = 1;
            struct cmd_group group = {0};
            group.count = its_in_group;
            group.cmd_idxs = calloc(its_in_group, sizeof(*group.cmd_idxs));
            group.var_values = calloc(its_in_group, sizeof(*group.var_values));
            for (size_t k = 0; k < its_in_group; ++k) {
                const char *param_value = param->values[k];
                if (replace_str(buf, sizeof(buf), cmd_str, param->name,
                                param_value) == -1) {
                    free(group.cmd_idxs);
                    free(group.var_values);
                    goto err_free_settings;
                }

                struct cmd cmd = {0};
                cmd.input = cli->input;
                cmd.output = cli->output;
                cmd.meas = settings->meas;
                if (init_cmd_exec(cli->shell, buf, &cmd) == -1) {
                    free(group.cmd_idxs);
                    free(group.var_values);
                    goto err_free_settings;
                }

                group.cmd_idxs[k] = sb_len(settings->cmds);
                group.var_values[k] = param_value;
                sb_push(settings->cmds, cmd);
            }
            group.var_name = param->name;
            group.template = strdup(cmd_str);
            sb_push(settings->cmd_groups, group);
        }

        if (!found_param) {
            struct cmd cmd = {0};
            cmd.input = cli->input;
            cmd.output = cli->output;
            cmd.meas = settings->meas;
            if (init_cmd_exec(cli->shell, cmd_str, &cmd) == -1)
                goto err_free_settings;

            sb_push(settings->cmds, cmd);
        }
    }

    return 0;
err_free_settings:
    free_settings(settings);
    return -1;
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
        close(STDIN_FILENO);
        int fd = open("/dev/null", O_RDONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case INPUT_POLICY_FILE: {
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

static void apply_output_policy(enum output_kind policy) {
    switch (policy) {
    case OUTPUT_POLICY_NULL: {
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
    case OUTPUT_POLICY_INHERIT:
        break;
    }
}

static int exec_cmd(const struct cmd *cmd, int stdout_fd,
                    struct rusage *rusage) {
    int rc = -1;
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        goto out;
    }

    if (pid == 0) {
        apply_input_policy(&cmd->input);
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
            apply_output_policy(cmd->output);
        }
        if (execvp(cmd->exec, cmd->argv) == -1)
            _exit(-1);
    }

    int status = 0;
    pid_t wpid;
    if ((wpid = wait4(pid, &status, 0, rusage)) != pid) {
        if (wpid == -1)
            perror("wait4");
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

static int process_finished_correctly(pid_t pid) {
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

static int execute_prepare(const char *cmd) {
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

    if (!process_finished_correctly(pid))
        return -1;

    return 0;
}

static int execute_custom(struct meas *custom, int in_fd, int out_fd) {
    char *exec = "/bin/sh";
    char *argv[] = {"sh", "-c", NULL, NULL};
    argv[2] = (char *)custom->cmd;

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

    if (!process_finished_correctly(pid))
        return -1;

    return 0;
}

static int parse_custom_output(int fd, double *valuep) {
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
    if (nread == 0) {
        fprintf(stderr, "error: custom measurement output is empty\n");
        return -1;
    }

    buf[nread] = '\0';
    char *end = NULL;
    double value = strtod(buf, &end);
    if (end == buf) {
        fprintf(stderr, "error: invalid custom measurement output '%s'\n", buf);
        return -1;
    }

    *valuep = value;
    return 0;
}

static int do_custom_measurement(struct bench *bench, size_t meas_idx,
                                 int stdout_fd) {
    int rc = -1;
    char path[] = "/tmp/csbench_tmp_XXXXXX";
    int custom_output_fd = mkstemp(path);
    if (custom_output_fd == -1) {
        perror("mkstemp");
        goto out;
    }

    struct meas *custom = bench->cmd->meas + meas_idx;
    if (lseek(stdout_fd, 0, SEEK_SET) == (off_t)-1 ||
        lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        perror("lseek");
        goto out;
    }

    if (execute_custom(custom, stdout_fd, custom_output_fd) == -1)
        goto out;

    if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        perror("lseek");
        goto out;
    }

    double value;
    if (parse_custom_output(custom_output_fd, &value) == -1) {
        fprintf(stderr, "note: when trying to execute '%s' on command '%s'\n",
                custom->name, bench->cmd->str);
        goto out;
    }

    sb_push(bench->meas[meas_idx], value);
    rc = 0;
out:
    if (custom_output_fd != -1) {
        close(custom_output_fd);
        unlink(path);
    }
    return rc;
}

static int exec_and_measure(struct bench *bench) {
    int ret = -1;
    int stdout_fd = -1;
    char path[] = "/tmp/csbench_out_XXXXXX";
    stdout_fd = mkstemp(path);
    if (stdout_fd == -1) {
        perror("mkstemp");
        goto out;
    }

    struct rusage rusage = {0};
    volatile double wall_clock_start = get_time();
    volatile int rc = exec_cmd(bench->cmd, stdout_fd, &rusage);
    volatile double wall_clock_end = get_time();

    if (rc == -1) {
        fprintf(stderr, "error: failed to execute command\n");
        goto out;
    }

    if (!g_allow_nonzero && rc != 0) {
        fprintf(stderr,
                "error: command '%s' finished with non-zero exit code\n",
                bench->cmd->str);
        goto out;
    }

    ++bench->run_count;
    sb_push(bench->exit_codes, rc);
    for (size_t meas_idx = 0; meas_idx < bench->meas_count; ++meas_idx) {
        const struct meas *meas = bench->cmd->meas + meas_idx;
        switch (meas->kind) {
        case MEAS_WALL:
            sb_push(bench->meas[meas_idx], wall_clock_end - wall_clock_start);
            break;
        case MEAS_RUSAGE_STIME:
            sb_push(bench->meas[meas_idx],
                    rusage.ru_stime.tv_sec +
                        (double)rusage.ru_stime.tv_usec / 1e6);
            break;
        case MEAS_RUSAGE_UTIME:
            sb_push(bench->meas[meas_idx],
                    rusage.ru_utime.tv_sec +
                        (double)rusage.ru_utime.tv_usec / 1e6);
            break;
        case MEAS_CUSTOM:
            if (do_custom_measurement(bench, meas_idx, stdout_fd) == -1)
                goto out;
            break;
        }
    }

    ret = 0;
out:
    if (stdout_fd != -1) {
        close(stdout_fd);
        unlink(path);
    }
    return ret;
}

static int warmup(const struct cmd *cmd) {
    double time_limit = g_warmup_time;
    if (time_limit < 0.0)
        return 0;

    double start_time = get_time();
    double end_time;
    do {
        if (exec_cmd(cmd, -1, NULL) == -1) {
            fprintf(stderr, "error: failed to execute warmup command\n");
            return -1;
        }
        end_time = get_time();
    } while (end_time - start_time < time_limit);

    return 0;
}

static int run_benchmark(struct bench *bench) {
    if (g_bench_stop.runs != 0) {
        for (int run_idx = 0; run_idx < g_bench_stop.runs; ++run_idx) {
            if (bench->prepare && execute_prepare(bench->prepare) == -1)
                return -1;
            if (exec_and_measure(bench) == -1)
                return -1;
        }

        return 0;
    }

    double niter_accum = 1;
    size_t niter = 1;
    double start_time = get_time();
    double time_limit = g_bench_stop.time_limit;
    size_t min_runs = g_bench_stop.min_runs;
    size_t max_runs = g_bench_stop.max_runs;
    for (size_t count = 1;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            if (bench->prepare && execute_prepare(bench->prepare) == -1)
                return -1;
            if (exec_and_measure(bench) == -1)
                return -1;
        }

        double end_time = get_time();
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

static void resample(const double *src, size_t count, double *dst) {
    uint32_t entropy = xorshift32(&g_rng_state);
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[xorshift32(&entropy) % count];
    g_rng_state = entropy;
}

static void bootstrap_mean_st_dev(const double *src, size_t count, double *tmp,
                                  struct est *meane, struct est *st_deve) {
    double sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += src[i];
    double mean = sum / count;
    meane->point = mean;
    double rss = 0;
    for (size_t i = 0; i < count; ++i) {
        double a = src[i] - mean;
        rss += a * a;
    }
    st_deve->point = sqrt(rss / count);
    double min_mean = INFINITY;
    double max_mean = -INFINITY;
    double min_rss = INFINITY;
    double max_rss = -INFINITY;
    for (int sample = 0; sample < g_nresamp; ++sample) {
        resample(src, count, tmp);
        sum = 0;
        for (size_t i = 0; i < count; ++i)
            sum += tmp[i];
        mean = sum / count;
        if (mean < min_mean)
            min_mean = mean;
        if (mean > max_mean)
            max_mean = mean;
        rss = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double a = tmp[i] - mean;
            rss += a * a;
        }
        if (rss < min_rss)
            min_rss = rss;
        if (rss > max_rss)
            max_rss = rss;
    }
    meane->lower = min_mean;
    meane->upper = max_mean;
    st_deve->lower = sqrt(min_rss / count);
    st_deve->upper = sqrt(max_rss / count);
}

static double c_max(double x, double u_a, double a, double sigma_b_2,
                    double sigma_g_2) {
    double k = u_a - x;
    double d = k * k;
    double ad = a * d;
    double k1 = sigma_b_2 - a * sigma_g_2 + ad;
    double k0 = -a * ad;
    double det = k1 * k1 - 4 * sigma_g_2 * k0;
    return floor(-2.0 * k0 / (k1 + sqrt(det)));
}

static double var_out(double c, double a, double sigma_b_2, double sigma_g_2) {
    double ac = a - c;
    return (ac / a) * (sigma_b_2 - ac * sigma_g_2);
}

static double outlier_variance(double mean, double st_dev, double a) {
    double sigma_b = st_dev;
    double u_a = mean / a;
    double u_g_min = u_a / 2.0;
    double sigma_g = fmin(u_g_min / 4.0, sigma_b / sqrt(a));
    double sigma_g_2 = sigma_g * sigma_g;
    double sigma_b_2 = sigma_b * sigma_b;
    double var_out_min =
        fmin(var_out(1, a, sigma_b_2, sigma_g_2),
             var_out(fmin(c_max(0, u_a, a, sigma_b_2, sigma_g_2),
                          c_max(u_g_min, u_a, a, sigma_b_2, sigma_g_2)),
                     a, sigma_b_2, sigma_g_2)) /
        sigma_b_2;
    return var_out_min;
}

static void classify_outliers(struct distr *distr) {
    struct outliers *outliers = &distr->outliers;
    double q1 = distr->q1;
    double q3 = distr->q3;
    double iqr = q3 - q1;
    double los = q1 - (iqr * 3.0);
    double lom = q1 - (iqr * 1.5);
    double him = q3 + (iqr * 1.5);
    double his = q3 + (iqr * 3.0);

    outliers->low_severe_x = los;
    outliers->low_mild_x = lom;
    outliers->high_mild_x = him;
    outliers->high_severe_x = his;
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < los)
            ++outliers->low_severe;
        else if (v > his)
            ++outliers->high_severe;
        else if (v < lom)
            ++outliers->low_mild;
        else if (v > him)
            ++outliers->high_mild;
    }
    outliers->var =
        outlier_variance(distr->mean.point, distr->st_dev.point, distr->count);
}

static int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

static void estimate_distr(const double *data, size_t count, double *tmp,
                           struct distr *distr) {
    distr->data = data;
    distr->count = count;
    bootstrap_mean_st_dev(data, count, tmp, &distr->mean, &distr->st_dev);
    memcpy(tmp, data, count * sizeof(*tmp));
    qsort(tmp, count, sizeof(*tmp), compare_doubles);
    distr->q1 = tmp[count / 4];
    distr->q3 = tmp[count * 3 / 4];
    distr->p1 = tmp[count / 100];
    distr->p5 = tmp[count * 5 / 100];
    distr->p95 = tmp[count * 95 / 100];
    distr->p99 = tmp[count * 99 / 100];
    distr->min = tmp[0];
    distr->max = tmp[count - 1];
    classify_outliers(distr);
}

#define fitting_curve_1(...) (1.0)
#define fitting_curve_n(_n) (_n)
#define fitting_curve_n_sq(_n) ((_n) * (_n))
#define fitting_curve_n_cube(_n) ((_n) * (_n) * (_n))
#define fitting_curve_logn(_n) log2(_n)
#define fitting_curve_nlogn(_n) ((_n) * log2(_n))

static double ols_approx(const struct ols_regress *regress, double n) {
    double f;
    switch (regress->complexity) {
    case O_1:
        f = fitting_curve_1(n);
        break;
    case O_N:
        f = fitting_curve_n(n);
        break;
    case O_N_SQ:
        f = fitting_curve_n_sq(n);
        break;
    case O_N_CUBE:
        f = fitting_curve_n_cube(n);
        break;
    case O_LOGN:
        f = fitting_curve_logn(n);
        break;
    case O_NLOGN:
        f = fitting_curve_nlogn(n);
        break;
    }
    return regress->a * f + regress->b;
}

#define ols(_name, _fitting)                                                   \
    static double ols_##_name(const double *x, const double *y, size_t count,  \
                              double adjust_y, double *rmsp) {                 \
        (void)x;                                                               \
        double sigma_gn_sq = 0.0;                                              \
        double sigma_t = 0.0;                                                  \
        double sigma_t_gn = 0.0;                                               \
        for (size_t i = 0; i < count; ++i) {                                   \
            double gn_i = _fitting(x[i]);                                      \
            sigma_gn_sq += gn_i * gn_i;                                        \
            sigma_t += y[i] - adjust_y;                                        \
            sigma_t_gn += (y[i] - adjust_y) * gn_i;                            \
        }                                                                      \
        double coef = sigma_t_gn / sigma_gn_sq;                                \
        double rms = 0.0;                                                      \
        for (size_t i = 0; i < count; ++i) {                                   \
            double fit = coef * _fitting(x[i]);                                \
            double a = (y[i] - adjust_y) - fit;                                \
            rms += a * a;                                                      \
        }                                                                      \
        double mean = sigma_t / count;                                         \
        *rmsp = sqrt(rms / count) / mean;                                      \
        return coef;                                                           \
    }

ols(1, fitting_curve_1) ols(n, fitting_curve_n) ols(n_sq, fitting_curve_n_sq)
    ols(n_cube, fitting_curve_n_cube) ols(logn, fitting_curve_logn)
        ols(nlogn, fitting_curve_nlogn)

#undef ols

            static void ols(const double *x, const double *y, size_t count,
                            struct ols_regress *result) {
    double min_y = INFINITY;
    for (size_t i = 0; i < count; ++i)
        if (y[i] < min_y)
            min_y = y[i];

    enum big_o best_fit = O_1;
    double best_fit_coef, best_fit_rms;
    best_fit_coef = ols_1(x, y, count, min_y, &best_fit_rms);

#define check(_name, _e)                                                       \
    do {                                                                       \
        double coef, rms;                                                      \
        coef = _name(x, y, count, min_y, &rms);                                \
        if (rms < best_fit_rms) {                                              \
            best_fit = _e;                                                     \
            best_fit_coef = coef;                                              \
            best_fit_rms = rms;                                                \
        }                                                                      \
    } while (0)

    check(ols_n, O_N);
    check(ols_n_sq, O_N_SQ);
    check(ols_n_cube, O_N_CUBE);
    check(ols_logn, O_LOGN);
    check(ols_nlogn, O_NLOGN);

#undef check

    result->a = best_fit_coef;
    result->b = min_y;
    result->rms = best_fit_rms;
    result->complexity = best_fit;
}

static void analyze_benchmark(struct bench_analysis *analysis) {
    const struct bench *bench = analysis->bench;
    size_t count = bench->run_count;
    assert(count);
    double *tmp = malloc(count * sizeof(*tmp));
    for (size_t i = 0; i < bench->meas_count; ++i) {
        assert(sb_len(bench->meas[i]) == count);
        estimate_distr(bench->meas[i], count, tmp, analysis->meas + i);
    }
    free(tmp);
}

static void compare_benches(struct bench_results *results) {
    if (results->bench_count == 1)
        return;

    size_t bench_count = results->bench_count;
    size_t meas_count = results->meas_count;
    double *best = calloc(meas_count, sizeof(*best));
    for (size_t i = 0; i < meas_count; ++i)
        best[i] = results->analyses[0].meas[i].mean.point;
    results->fastest_meas = calloc(meas_count, sizeof(*results->fastest_meas));
    for (size_t i = 0; i < meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;
        for (size_t j = 1; j < bench_count; ++j) {
            const struct bench_analysis *analysis = results->analyses + j;
            double mean = analysis->meas[i].mean.point;
            if (mean < best[i]) {
                results->fastest_meas[i] = j;
                best[i] = mean;
            }
        }
    }
    free(best);
}

static void analyze_cmd_groups(const struct settings *settings,
                               struct bench_results *results) {
    size_t group_count = results->group_count = sb_len(settings->cmd_groups);
    results->group_count = group_count;
    results->group_analyses =
        calloc(results->meas_count, sizeof(*results->group_analyses));
    for (size_t i = 0; i < results->meas_count; ++i)
        results->group_analyses[i] =
            calloc(group_count, sizeof(*results->group_analyses[i]));

    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        const struct meas *meas = results->meas + meas_idx;
        if (meas->is_secondary)
            continue;
        for (size_t group_idx = 0; group_idx < group_count; ++group_idx) {
            const struct cmd_group *group = settings->cmd_groups + group_idx;
            size_t cmd_count = group->count;
            struct cmd_group_analysis *analysis =
                results->group_analyses[meas_idx] + group_idx;
            analysis->meas = meas;
            analysis->group = group;
            analysis->cmd_count = cmd_count;
            analysis->data = calloc(cmd_count, sizeof(*analysis->data));
            int values_are_doubles = 1;
            double slowest = -INFINITY, fastest = INFINITY;
            for (size_t cmd_idx = 0; cmd_idx < cmd_count; ++cmd_idx) {
                const char *value = group->var_values[cmd_idx];
                const struct cmd *cmd =
                    settings->cmds + group->cmd_idxs[cmd_idx];
                size_t bench_idx = -1;
                for (size_t i = 0; i < results->bench_count; ++i) {
                    if (results->benches[i].cmd == cmd) {
                        bench_idx = i;
                        break;
                    }
                }
                assert(bench_idx != (size_t)-1);
                char *end = NULL;
                double value_double = strtod(value, &end);
                if (end == value)
                    values_are_doubles = 0;
                double mean =
                    results->analyses[bench_idx].meas[meas_idx].mean.point;
                struct cmd_in_group_data *data = analysis->data + cmd_idx;
                data->mean = mean;
                data->value = value;
                data->value_double = value_double;
                if (mean > slowest) {
                    slowest = mean;
                    analysis->slowest = data;
                }
                if (mean < fastest) {
                    fastest = mean;
                    analysis->fastest = data;
                }
            }
            analysis->values_are_doubles = values_are_doubles;
            if (values_are_doubles) {
                double *x = calloc(cmd_count, sizeof(*x));
                double *y = calloc(cmd_count, sizeof(*y));
                for (size_t i = 0; i < cmd_count; ++i) {
                    x[i] = analysis->data[i].value_double;
                    y[i] = analysis->data[i].mean;
                }
                ols(x, y, cmd_count, &analysis->regress);
                free(x);
                free(y);
            }
        }
    }
}

static void print_exit_code_info(const struct bench *bench) {
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

static int format_time(char *dst, size_t sz, double t) {
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

static void format_meas(char *buf, size_t buf_size, double value,
                        const struct units *units) {
    switch (units->kind) {
    case MU_S:
        format_time(buf, buf_size, value);
        break;
    case MU_MS:
        format_time(buf, buf_size, value * 0.001);
        break;
    case MU_US:
        format_time(buf, buf_size, value * 0.000001);
        break;
    case MU_NS:
        format_time(buf, buf_size, value * 0.000000001);
        break;
    case MU_CUSTOM:
        snprintf(buf, buf_size, "%.5g %s", value, units->str);
        break;
    case MU_NONE:
        snprintf(buf, buf_size, "%.5g", value);
        break;
    }
}

static const char *outliers_variance_str(double fraction) {
    if (fraction < 0.01)
        return "no";
    else if (fraction < 0.1)
        return "a slight";
    else if (fraction < 0.5)
        return "a moderate";
    return "a severe";
}

static void print_outliers(const struct outliers *outliers, size_t run_count) {
    int outlier_count = outliers->low_mild + outliers->high_mild +
                        outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        printf("found %d outliers across %zu measurements (%.2f%%)\n",
               outlier_count, run_count,
               (double)outlier_count / run_count * 100.0);
        if (outliers->low_severe)
            printf("%d (%.2f%%) low severe\n", outliers->low_severe,
                   (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            printf("%d (%.2f%%) low mild\n", outliers->low_mild,
                   (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            printf("%d (%.2f%%) high mild\n", outliers->high_mild,
                   (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            printf("%d (%.2f%%) high severe\n", outliers->high_severe,
                   (double)outliers->high_severe / run_count * 100.0);
    }
    printf("outlying measurements have %s (%.1f%%) effect on estimated "
           "standard deviation\n",
           outliers_variance_str(outliers->var), outliers->var * 100.0);
}

static void print_estimate(const char *name, const struct est *est,
                           const struct units *units) {
    char buf1[256], buf2[256], buf3[256];
    switch (units->kind) {
    case MU_S:
        format_time(buf1, sizeof(buf1), est->lower);
        format_time(buf2, sizeof(buf2), est->point);
        format_time(buf3, sizeof(buf3), est->upper);
        break;
    case MU_MS:
        format_time(buf1, sizeof(buf1), est->lower * 0.001);
        format_time(buf2, sizeof(buf2), est->point * 0.001);
        format_time(buf3, sizeof(buf3), est->upper * 0.001);
        break;
    case MU_US:
        format_time(buf1, sizeof(buf1), est->lower * 0.000001);
        format_time(buf2, sizeof(buf2), est->point * 0.000001);
        format_time(buf3, sizeof(buf3), est->upper * 0.000001);
        break;
    case MU_NS:
        format_time(buf1, sizeof(buf1), est->lower * 0.000000001);
        format_time(buf2, sizeof(buf2), est->point * 0.000000001);
        format_time(buf3, sizeof(buf3), est->upper * 0.000000001);
        break;
    case MU_CUSTOM:
    case MU_NONE:
        snprintf(buf1, sizeof(buf1), "%.5g", est->lower);
        snprintf(buf2, sizeof(buf1), "%.5g", est->point);
        snprintf(buf3, sizeof(buf1), "%.5g", est->upper);
        break;
    }

    printf("%7s %8s %8s %8s\n", name, buf1, buf2, buf3);
}

static const char *units_str(const struct units *units) {
    switch (units->kind) {
    case MU_S:
        return "s";
    case MU_MS:
        return "ms";
    case MU_US:
        return "μs";
    case MU_NS:
        return "ns";
    case MU_CUSTOM:
        return units->str;
    case MU_NONE:
        return "";
    }
    return NULL;
}

static void print_distr(const struct distr *dist, const struct units *units) {
    char buf1[256], buf2[256];
    format_meas(buf1, sizeof(buf1), dist->min, units);
    format_meas(buf2, sizeof(buf2), dist->max, units);
    printf("min %s max %s\n", buf1, buf2);
    print_estimate("mean", &dist->mean, units);
    print_estimate("st dev", &dist->st_dev, units);
}

static void ref_speed(double u1, double sigma1, double u2, double sigma2,
                      double *ref_u, double *ref_sigma) {
    double ref = u1 / u2;
    // propagate standard deviation for formula (t1 / t2)
    double a = sigma1 / u1;
    double b = sigma2 / u2;
    double ref_st_dev = ref * sqrt(a * a + b * b);

    *ref_u = ref;
    *ref_sigma = ref_st_dev;
}

static const char *big_o_str(enum big_o complexity) {
    switch (complexity) {
    case O_1:
        return "constant (O(1))";
    case O_N:
        return "linear (O(N))";
    case O_N_SQ:
        return "quadratic (O(N^2))";
    case O_N_CUBE:
        return "cubic (O(N^3))";
    case O_LOGN:
        return "logarithmic (O(log(N)))";
    case O_NLOGN:
        return "linearithmic (O(N*log(N)))";
    }
    return NULL;
}

static void print_benchmark_info(const struct bench_analysis *analysis) {
    const struct bench *bench = analysis->bench;
    size_t run_count = bench->run_count;
    const struct cmd *cmd = bench->cmd;
    printf("command\t'%s'\n", cmd->str);
    printf("%zu runs\n", bench->run_count);
    print_exit_code_info(bench);
    for (size_t i = 0; i < bench->meas_count; ++i) {
        const struct meas *info = cmd->meas + i;
        if (info->is_secondary)
            continue;

        const struct distr *distr = analysis->meas + i;
        print_distr(distr, &info->units);
        for (size_t j = 0; j < bench->meas_count; ++j) {
            if (cmd->meas[j].is_secondary && cmd->meas[j].primary_idx == i)
                print_estimate(cmd->meas[j].name, &analysis->meas[j].mean,
                               &cmd->meas->units);
        }
        print_outliers(&distr->outliers, run_count);
    }
}

static void print_cmd_comparison(const struct bench_results *results) {
    if (results->bench_count == 1)
        return;

    size_t meas_count = results->meas_count;
    for (size_t i = 0; i < meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;
        size_t best_idx = results->fastest_meas[i];
        const struct bench_analysis *best = results->analyses + best_idx;
        const struct meas *meas = results->meas + i;
        printf("measurement %s\n", meas->name);
        printf("fastest command '%s'\n", best->bench->cmd->str);
        for (size_t j = 0; j < results->bench_count; ++j) {
            const struct bench_analysis *analysis = results->analyses + j;
            if (analysis == best)
                continue;

            double ref, ref_st_dev;
            ref_speed(analysis->meas[i].mean.point,
                      analysis->meas[i].st_dev.point, best->meas[i].mean.point,
                      best->meas[i].st_dev.point, &ref, &ref_st_dev);
            printf("%.3f ± %.3f times faster than '%s'\n", ref, ref_st_dev,
                   analysis->bench->cmd->str);
        }
    }
}

static void print_cmd_group_analysis(const struct bench_results *results) {
    for (size_t i = 0; i < results->meas_count; ++i) {
        if (results->meas[i].is_secondary)
            continue;
        for (size_t j = 0; j < results->group_count; ++j) {
            const struct cmd_group_analysis *analysis =
                results->group_analyses[i] + j;
            const struct cmd_group *group = analysis->group;

            printf("command group '%s' with parameter %s\n", group->template,
                   group->var_name);
            char buf[256];
            format_time(buf, sizeof(buf), analysis->data[0].mean);
            printf("lowest time %s with %s=%s\n", buf, group->var_name,
                   analysis->fastest->value);
            format_time(buf, sizeof(buf),
                        analysis->data[analysis->cmd_count - 1].mean);
            printf("highest time %s with %s=%s\n", buf, group->var_name,
                   analysis->slowest->value);
            if (analysis->values_are_doubles) {
                printf("mean time is most likely %s in terms of parameter\n",
                       big_o_str(analysis->regress.complexity));
                printf("linear coef %g rms %.3f\n", analysis->regress.a,
                       analysis->regress.rms);
            }
        }
    }
}

static int json_escape(char *buf, size_t buf_size, const char *src) {
    if (src == NULL) {
        assert(buf_size);
        *buf = '\0';
        return 0;
    }
    char *end = buf + buf_size;
    while (*src) {
        if (buf >= end)
            return -1;

        int c = *src++;
        if (c == '\"') {
            *buf++ = '\\';
            if (buf >= end)
                return -1;
            *buf++ = c;
        } else {
            *buf++ = c;
        }
    }
    if (buf >= end)
        return -1;
    *buf = '\0';
    return 0;
}

static int export_json(const struct bench_results *results,
                       const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        fprintf(stderr, "error: failed to open file '%s' for export\n",
                filename);
        return -1;
    }

    char buf[4096];
    size_t bench_count = results->bench_count;
    const struct bench *benches = results->benches;
    fprintf(f,
            "{ \"settings\": {"
            "\"time_limit\": %f, \"runs\": %d, \"min_runs\": %d, "
            "\"max_runs\": %d, \"warmup_time\": %f, \"nresamp\": %d "
            "}, \"benches\": [",
            g_bench_stop.time_limit, g_bench_stop.runs, g_bench_stop.min_runs,
            g_bench_stop.max_runs, g_warmup_time, g_nresamp);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct bench *bench = benches + i;
        fprintf(f, "{ ");
        if (bench->prepare)
            json_escape(buf, sizeof(buf), bench->prepare);
        else
            *buf = '\0';
        fprintf(f, "\"prepare\": \"%s\", ", buf);
        json_escape(buf, sizeof(buf), bench->cmd->str);
        fprintf(f, "\"command\": \"%s\", ", buf);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"exit_codes\": [");
        for (size_t j = 0; j < run_count; ++j)
            fprintf(f, "%d%s", bench->exit_codes[j],
                    j != run_count - 1 ? ", " : "");
        fprintf(f, "], \"meas\": [");
        for (size_t j = 0; j < bench->meas_count; ++j) {
            const struct meas *info = bench->cmd->meas + j;
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
            if (j != bench->meas_count - 1)
                fprintf(f, ", ");
        }
        fprintf(f, "]}");
        if (i != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);

    return 0;
}

static int python_found(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", "--version", NULL) == -1)
            _exit(-1);
    }

    return process_finished_correctly(pid);
}

static int launch_python_stdin_pipe(FILE **inp, pid_t *pidp) {
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
        if (dup2(pipe_fds[0], STDIN_FILENO) == -1)
            _exit(-1);
        // we don't need any output
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", NULL) == -1)
            _exit(-1);
    }

    close(pipe_fds[0]);
    FILE *f = fdopen(pipe_fds[1], "w");

    *pidp = pid;
    *inp = f;
    return 0;
}

static int python_has_matplotlib(void) {
    FILE *f;
    pid_t pid;
    if (launch_python_stdin_pipe(&f, &pid))
        return 0;

    fprintf(f, "import matplotlib.pyplot as plt\n");
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

static int units_is_time(const struct units *units) {
    switch (units->kind) {
    case MU_S:
    case MU_MS:
    case MU_NS:
    case MU_US:
        return 1;
    default:
        break;
    }
    return 0;
}

static void prettify_plot(const struct units *units, double min, double max,
                          struct prettify_plot *plot) {
    if (log10(max) - log10(min) > 3.0)
        plot->logscale = 1;

    plot->multiplier = 1;
    if (units_is_time(units)) {
        if (max < 1e-6) {
            plot->units_str = "ns";
            plot->multiplier = 1e9;
        } else if (max < 1e-3) {
            plot->units_str = "us";
            plot->multiplier = 1e6;
        } else if (max < 1.0) {
            plot->units_str = "ms";
            plot->multiplier = 1e3;
        } else {
            plot->units_str = "s";
        }
    } else {
        plot->units_str = units_str(units);
    }
}

static void violin_plot(const struct bench *benches, size_t bench_count,
                        size_t meas_idx, const char *output_filename, FILE *f) {
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < bench_count; ++i) {
        for (size_t j = 0; j < benches[i].run_count; ++j) {
            double v = benches[i].meas[meas_idx][j];
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&benches[0].cmd->meas[meas_idx].units, min, max, &prettify);

    const struct meas *meas = benches[0].cmd->meas + meas_idx;
    fprintf(f, "data = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct bench *bench = benches + i;
        fprintf(f, "[");
        for (size_t j = 0; j < bench->run_count; ++j)
            fprintf(f, "%g, ", bench->meas[meas_idx][j] * prettify.multiplier);
        fprintf(f, "], ");
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < bench_count; ++i) {
        const struct bench *bench = benches + i;
        fprintf(f, "'%s', ", bench->cmd->str);
    }
    fprintf(f,
            "]\n"
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.xlabel('command')\n"
            "plt.ylabel('%s [%s]')\n"
            "plt.violinplot(data)\n"
            "plt.xticks(list(range(1, len(names) + 1)), names)\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            meas->name, prettify.units_str, output_filename);
}

static void bar_plot(const struct bench_analysis *analyses, size_t count,
                     size_t meas_idx, const char *output_filename, FILE *f) {
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double v = analyses[i].meas[meas_idx].mean.point;
        if (v > max)
            max = v;
        if (v < min)
            min = v;
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&analyses[0].bench->cmd->meas[meas_idx].units, min, max,
                  &prettify);
    fprintf(f, "data = [");
    for (size_t i = 0; i < count; ++i) {
        const struct bench_analysis *analysis = analyses + i;
        fprintf(f, "%g, ",
                analysis->meas[meas_idx].mean.point * prettify.multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < count; ++i) {
        const struct bench_analysis *analysis = analyses + i;
        fprintf(f, "'%s', ", analysis->bench->cmd->str);
    }
    fprintf(f, "]\n"
               "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
    if (prettify.logscale)
        fprintf(f, "plt.xscale('log')\n");
    fprintf(f,
            "plt.barh(range(len(data)), data)\n"
            "plt.yticks(range(len(data)), labels=names)\n"
            "plt.xlabel('mean %s [%s]')\n"
            "plt.ioff()\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            analyses->bench->cmd->meas[meas_idx].name, prettify.units_str,
            output_filename);
}

static void group_plot(const struct cmd_group_analysis *analyses, size_t count,
                       const char *output_filename, FILE *f) {
    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        for (size_t i = 0; i < analyses[grp_idx].cmd_count; ++i) {
            double v = analyses[grp_idx].data[i].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&analyses[0].meas->units, min, max, &prettify);

    fprintf(f, "x = [");
    for (size_t i = 0; i < analyses[0].cmd_count; ++i) {
        double v = analyses[0].data[i].value_double;
        fprintf(f, "%g, ", v);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        fprintf(f, "[");
        for (size_t i = 0; i < analyses[grp_idx].cmd_count; ++i)
            fprintf(f, "%g, ",
                    analyses[grp_idx].data[i].mean * prettify.multiplier);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    size_t nregr = 100;
    double lowest_x = INFINITY;
    double highest_x = -INFINITY;
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        double low = analyses[grp_idx].data[0].value_double;
        if (low < lowest_x)
            lowest_x = low;
        double high = analyses[grp_idx]
                          .data[analyses[grp_idx].cmd_count - 1]
                          .value_double;
        if (high > highest_x)
            highest_x = high;
    }

    double regr_x_step = (highest_x - lowest_x) / nregr;
    fprintf(f, "regrx = [");
    for (size_t i = 0; i < nregr; ++i)
        fprintf(f, "%g, ", lowest_x + regr_x_step * i);
    fprintf(f, "]\n");
    fprintf(f, "regry = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        const struct cmd_group_analysis *analysis = analyses + grp_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < nregr; ++i) {
            double regr =
                ols_approx(&analysis->regress, lowest_x + regr_x_step * i);
            fprintf(f, "%g, ", regr * prettify.multiplier);
        }
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n"
               "plt.ioff()\n");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        fprintf(f,
                "plt.plot(regrx, regry[%zu], color='red', alpha=0.3)\n"
                "plt.plot(x, y[%zu], '.-')\n",
                grp_idx, grp_idx);
    }
    if (prettify.logscale)
        fprintf(f, "plt.yscale('log')\n");
    fprintf(f,
            "plt.xticks(x)\n"
            "plt.grid()\n"
            "plt.xlabel('%s')\n"
            "plt.ylabel('%s [%s]')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            analyses[0].group->var_name, analyses[0].meas->name,
            prettify.units_str, output_filename);
}

static void construct_kde(const struct distr *distr, double *kde,
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

#define init_kde_plot(_distr, _title, _meas, _output_filename, _plot)          \
    init_kde_plot_internal(_distr, _title, _meas, 0, _output_filename, _plot)
#define init_kde_plot_ext(_distr, _title, _meas, _output_filename, _plot)      \
    init_kde_plot_internal(_distr, _title, _meas, 1, _output_filename, _plot)
static void init_kde_plot_internal(const struct distr *distr, const char *title,
                                   const struct meas *meas, int is_ext,
                                   const char *output_filename,
                                   struct kde_plot *plot) {
    size_t kde_points = 200;
    plot->is_ext = is_ext;
    plot->output_filename = output_filename;
    plot->meas = meas;
    plot->title = title;
    plot->distr = distr;
    plot->count = kde_points;
    plot->data = malloc(sizeof(*plot->data) * plot->count);
    construct_kde(distr, plot->data, plot->count, is_ext, &plot->lower,
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

static void make_kde_plot(const struct kde_plot *plot, FILE *f) {
    assert(!plot->is_ext);
    double min = plot->lower;
    double max = plot->lower + plot->step * (plot->count - 1);
    struct prettify_plot prettify = {0};
    prettify_plot(&plot->meas->units, min, max, &prettify);

    fprintf(f, "y = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ", plot->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ",
                (plot->lower + plot->step * i) * prettify.multiplier);
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "plt.ioff()\n"
            "plt.title('%s')\n"
            "plt.fill_between(x, y, interpolate=True, alpha=0.25)\n"
            "plt.vlines(%g, [0], [%g])\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            plot->title, plot->mean * prettify.multiplier, plot->mean_y,
            plot->meas->name, prettify.units_str, plot->output_filename);
}

static void make_kde_plot_ext(const struct kde_plot *plot, FILE *f) {
    assert(plot->is_ext);
    double min = plot->lower;
    double max = plot->lower + plot->step * (plot->count - 1);
    struct prettify_plot prettify = {0};
    prettify_plot(&plot->meas->units, min, max, &prettify);

    double max_y = -INFINITY;
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
        fprintf(f, "(%g, %g), ", v * prettify.multiplier,
                (double)(i + 1) / plot->distr->count * max_y);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "severe_points = list(filter(lambda x: x[0] < %g or x[0] > %g, "
            "points))\n",
            plot->distr->outliers.low_severe_x * prettify.multiplier,
            plot->distr->outliers.high_severe_x * prettify.multiplier);
    fprintf(f,
            "mild_points = list(filter(lambda x: (%g < x[0] < %g) or (%g < "
            "x[0] < "
            "%f), points))\n",
            plot->distr->outliers.low_severe_x * prettify.multiplier,
            plot->distr->outliers.low_mild_x * prettify.multiplier,
            plot->distr->outliers.high_mild_x * prettify.multiplier,
            plot->distr->outliers.high_severe_x * prettify.multiplier);
    fprintf(f, "reg_points = list(filter(lambda x: %g < x[0] < %g, points))\n",
            plot->distr->outliers.low_mild_x * prettify.multiplier,
            plot->distr->outliers.high_mild_x * prettify.multiplier);
    size_t kde_count = 0;
    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i, ++kde_count) {
        double x = plot->lower + plot->step * i;
        if (x > max_point_x)
            break;
        fprintf(f, "%g, ", x * prettify.multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t i = 0; i < kde_count; ++i)
        fprintf(f, "%g, ", plot->data[i]);
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
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
            plot->title, plot->mean * prettify.multiplier);
    if (plot->distr->outliers.low_mild_x > plot->lower)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                plot->distr->outliers.low_mild_x * prettify.multiplier);
    if (plot->distr->outliers.low_severe_x > plot->lower)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                plot->distr->outliers.low_severe_x * prettify.multiplier);
    if (plot->distr->outliers.high_mild_x <
        plot->lower + plot->count * plot->step)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                plot->distr->outliers.high_mild_x * prettify.multiplier);
    if (plot->distr->outliers.high_severe_x <
        plot->lower + plot->count * plot->step)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                plot->distr->outliers.high_severe_x * prettify.multiplier);
    fprintf(f,
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('runs')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->meas->name, prettify.units_str, plot->output_filename);
}

static void free_kde_plot(struct kde_plot *plot) { free(plot->data); }

#define kde_plot(_distr, _title, _meas, _output_filename, _f)                  \
    do {                                                                       \
        struct kde_plot plot = {0};                                            \
        init_kde_plot(_distr, _title, _meas, _output_filename, &plot);         \
        make_kde_plot(&plot, _f);                                              \
        free_kde_plot(&plot);                                                  \
    } while (0)
#define kde_plot_ext(_distr, _title, _meas, _output_filename, _f)              \
    do {                                                                       \
        struct kde_plot plot = {0};                                            \
        init_kde_plot_ext(_distr, _title, _meas, _output_filename, &plot);     \
        make_kde_plot_ext(&plot, _f);                                          \
        free_kde_plot(&plot);                                                  \
    } while (0)

static int dump_plot_src(const struct bench_results *results,
                         const char *out_dir) {
    size_t bench_count = results->bench_count;
    const struct bench *benches = results->benches;
    const struct bench_analysis *analyses = results->analyses;
    char buf[4096];
    FILE *f;
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = results->meas + meas_idx;
        if (bench_count > 1) {
            f = open_file_fmt("w", "%s/violin_%zu.py", out_dir, meas_idx);
            if (f == NULL) {
                fprintf(stderr,
                        "error: failed to create file %s/violin_%zu.py\n",
                        out_dir, meas_idx);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/violin_%zu.svg", out_dir, meas_idx);
            violin_plot(benches, bench_count, meas_idx, buf, f);
            fclose(f);
        }
        if (bench_count > 1) {
            f = open_file_fmt("w", "%s/bar_%zu.py", out_dir, meas_idx);
            if (f == NULL) {
                fprintf(stderr, "error: failed to create file %s/bar_%zu.py\n",
                        out_dir, meas_idx);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/bar_%zu.svg", out_dir, meas_idx);
            bar_plot(analyses, bench_count, meas_idx, buf, f);
            fclose(f);
        }
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            const struct cmd_group_analysis *analysis =
                results->group_analyses[meas_idx] + grp_idx;
            f = open_file_fmt("w", "%s/group_%zu_%zu.py", out_dir, grp_idx,
                              meas_idx);
            if (f == NULL) {
                fprintf(stderr,
                        "error: failed to create file %s/group_%zu_%zu.py\n",
                        out_dir, grp_idx, meas_idx);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/group_%zu_%zu.svg", out_dir, grp_idx,
                     meas_idx);
            group_plot(analysis, 1, buf, f);
            fclose(f);
        }
        if (results->group_count > 1) {
            const struct cmd_group_analysis *analyses =
                results->group_analyses[meas_idx];
            f = open_file_fmt("w", "%s/group_%zu.py", out_dir, meas_idx);
            if (f == NULL) {
                fprintf(stderr,
                        "error: failed to create file %s/group_%zu.py\n",
                        out_dir, meas_idx);
                return -1;
            }
            snprintf(buf, sizeof(buf), "%s/group_%zu.svg", out_dir, meas_idx);
            group_plot(analyses, results->group_count, buf, f);
            fclose(f);
        }
        for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
            const struct bench_analysis *analysis = analyses + bench_idx;
            const char *cmd_str = analysis->bench->cmd->str;
            {
                f = open_file_fmt("w", "%s/kde_%zu_%zu.py", out_dir, bench_idx,
                                  meas_idx);
                if (f == NULL) {
                    fprintf(stderr,
                            "error: failed to create file %s/kde_%zu_%zu.py\n",
                            out_dir, bench_idx, meas_idx);
                    return -1;
                }
                snprintf(buf, sizeof(buf), "%s/kde_%zu_%zu.svg", out_dir,
                         bench_idx, meas_idx);
                kde_plot(analysis->meas + meas_idx, cmd_str, meas, buf, f);
                fclose(f);
            }
            {
                f = open_file_fmt("w", "%s/kde_ext_%zu_%zu.py", out_dir,
                                  bench_idx, meas_idx);
                if (f == NULL) {
                    fprintf(stderr,
                            "error: failed to create file "
                            "%s/kde_ext_%zu_%zu.py\n",
                            out_dir, bench_idx, meas_idx);
                    return -1;
                }
                snprintf(buf, sizeof(buf), "%s/kde_ext_%zu_%zu.svg", out_dir,
                         bench_idx, meas_idx);
                kde_plot_ext(analysis->meas + meas_idx, cmd_str, meas, buf, f);
                fclose(f);
            }
        }
    }

    return 0;
}

static int make_plots(const struct bench_results *results,
                      const char *out_dir) {
    size_t bench_count = results->bench_count;
    const struct bench *benches = results->benches;
    const struct bench_analysis *analyses = results->analyses;
    char buf[4096];
    pid_t *processes = NULL;
    int ret = -1;
    FILE *f;
    pid_t pid;
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = results->meas + meas_idx;
        if (bench_count > 1) {
            snprintf(buf, sizeof(buf), "%s/violin_%zu.svg", out_dir, meas_idx);
            if (launch_python_stdin_pipe(&f, &pid) == -1) {
                fprintf(stderr, "error: failed to launch python\n");
                goto out;
            }
            violin_plot(benches, bench_count, meas_idx, buf, f);
            fclose(f);
            sb_push(processes, pid);
        }
        if (bench_count > 1) {
            snprintf(buf, sizeof(buf), "%s/bar_%zu.svg", out_dir, meas_idx);
            if (launch_python_stdin_pipe(&f, &pid) == -1) {
                fprintf(stderr, "error: failed to launch python\n");
                goto out;
            }
            bar_plot(analyses, bench_count, meas_idx, buf, f);
            fclose(f);
            sb_push(processes, pid);
        }
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            const struct cmd_group_analysis *analysis =
                results->group_analyses[meas_idx] + grp_idx;
            snprintf(buf, sizeof(buf), "%s/group_%zu_%zu.svg", out_dir, grp_idx,
                     meas_idx);
            if (launch_python_stdin_pipe(&f, &pid) == -1) {
                fprintf(stderr, "error: failed to launch python\n");
                goto out;
            }
            group_plot(analysis, 1, buf, f);
            fclose(f);
            sb_push(processes, pid);
        }
        if (results->group_count > 1) {
            const struct cmd_group_analysis *analyses =
                results->group_analyses[meas_idx];
            snprintf(buf, sizeof(buf), "%s/group_%zu.svg", out_dir, meas_idx);
            if (launch_python_stdin_pipe(&f, &pid) == -1) {
                fprintf(stderr, "error: failed to launch python\n");
                goto out;
            }
            group_plot(analyses, results->group_count, buf, f);
            fclose(f);
            sb_push(processes, pid);
        }
        for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
            const struct bench_analysis *analysis = analyses + bench_idx;
            const char *cmd_str = analysis->bench->cmd->str;
            {
                snprintf(buf, sizeof(buf), "%s/kde_%zu_%zu.svg", out_dir,
                         bench_idx, meas_idx);
                if (launch_python_stdin_pipe(&f, &pid) == -1) {
                    fprintf(stderr, "error: failed to launch python\n");
                    goto out;
                }
                kde_plot(analysis->meas + meas_idx, cmd_str, meas, buf, f);
                fclose(f);
                sb_push(processes, pid);
            }
            {
                snprintf(buf, sizeof(buf), "%s/kde_ext_%zu_%zu.svg", out_dir,
                         bench_idx, meas_idx);
                if (launch_python_stdin_pipe(&f, &pid) == -1) {
                    fprintf(stderr, "error: failed to launch python\n");
                    goto out;
                }
                kde_plot_ext(analysis->meas + meas_idx, cmd_str, meas, buf, f);
                fclose(f);
                sb_push(processes, pid);
            }
        }
    }

    ret = 0;
out:
    for (size_t i = 0; i < sb_len(processes); ++i) {
        if (!process_finished_correctly(processes[i])) {
            fprintf(stderr, "error: python finished with non-zero exit code\n");
            ret = -1;
        }
    }
    sb_free(processes);
    return ret;
}

static int make_plots_readme(const struct bench_results *results,
                             const char *out_dir) {
    FILE *f = open_file_fmt("w", "%s/readme.md", out_dir);
    if (f == NULL) {
        fprintf(stderr, "error: failed to create file %s/readme.md\n", out_dir);
        return -1;
    }
    fprintf(f, "# csbench analyze map\n");
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = results->meas + meas_idx;
        fprintf(f, "## measurement %s\n", meas->name);
        fprintf(f, "* [violin plot](violin_%zu.svg)\n", meas_idx);
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            const struct cmd_group_analysis *analysis =
                results->group_analyses[meas_idx] + grp_idx;
            fprintf(f,
                    "* [command group '%s' regression "
                    "plot](group_%zu_%zu.svg)\n",
                    analysis->group->template, grp_idx, meas_idx);
        }
        fprintf(f, "### KDE plots\n");
        fprintf(f, "#### regular\n");
        for (size_t bench_idx = 0; bench_idx < results->bench_count;
             ++bench_idx) {
            const struct bench_analysis *analysis =
                results->analyses + bench_idx;
            const char *cmd_str = analysis->bench->cmd->str;
            fprintf(f, "* [%s](kde_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
        fprintf(f, "#### extended\n");
        for (size_t bench_idx = 0; bench_idx < results->bench_count;
             ++bench_idx) {
            const struct bench_analysis *analysis =
                results->analyses + bench_idx;
            const char *cmd_str = analysis->bench->cmd->str;
            fprintf(f, "* [%s](kde_ext_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
    }
    fclose(f);
    return 0;
}

#define html_time_estimate(_name, _est, _f)                                    \
    html_estimate(_name, _est, &(struct units){0}, f)
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
                       size_t meas_idx, FILE *f) {
    const struct distr *distr = analysis->meas + meas_idx;
    const struct bench *bench = analysis->bench;
    const struct meas *info = bench->cmd->meas + meas_idx;
    const struct cmd *cmd = bench->cmd;
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
    for (size_t j = 0; j < bench->meas_count; ++j) {
        if (cmd->meas[j].is_secondary && cmd->meas[j].primary_idx == meas_idx)
            html_estimate(cmd->meas[j].name, &analysis->meas[j].mean,
                          &cmd->meas->units, f);
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
#if 0 // TODO: Make this a table
        size_t best_idx = results->fastest_meas[meas_idx];
        const struct bench_analysis *best = results->analyses + best_idx;
        fprintf(f, "<div class=\"col stats\"><p>fastest command '%s'</p><ul>",
                best->bench->cmd->str);
        for (size_t j = 0; j < results->bench_count; ++j) {
            const struct bench_analysis *analysis = results->analyses + j;
            if (analysis == best)
                continue;

            double ref, ref_st_dev;
            ref_speed(analysis->meas[meas_idx].mean.point,
                         analysis->meas[meas_idx].st_dev.point,
                         best->meas[meas_idx].mean.point,
                         best->meas[meas_idx].st_dev.point, &ref, &ref_st_dev);
            fprintf(f, "<li>%.3f ± %.3f times faster than '%s'</li>", ref,
                    ref_st_dev, analysis->bench->cmd->str);
    }
        fprintf(f, "</ul>");
#endif
        fprintf(f, "</div></div></div>");
    }
    fprintf(f, "</div>");
}

static void html_cmd_group(const struct cmd_group_analysis *analysis,
                           const struct meas *meas, size_t meas_idx,
                           size_t grp_idx, FILE *f) {
    const struct cmd_group *group = analysis->group;
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
            buf, group->var_name, analysis->fastest->value);
    format_time(buf, sizeof(buf), analysis->slowest->mean);
    fprintf(f, "<p>hightest time %s with %s=%s</p>", buf, group->var_name,
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

static void html_paramter_analysis(const struct bench_results *results,
                                   FILE *f) {
    if (!results->group_count)
        return;
    fprintf(f, "<div><h2>parameter analysis</h2>");
    for (size_t meas_idx = 0; meas_idx < results->meas_count; ++meas_idx) {
        if (results->meas[meas_idx].is_secondary)
            continue;
        if (results->group_count > 1)
            fprintf(f,
                    "<div><h3>summary for %s</h3>"
                    "<div class=\"row\"><div class=\"col\">"
                    "<img src=\"group_%zu.svg\">"
                    "<div class=\"col\"></div>"
                    "</div></div></div>",
                    results->meas[meas_idx].name, meas_idx);
        for (size_t grp_idx = 0; grp_idx < results->group_count; ++grp_idx) {
            fprintf(f, "<div><h3>group '%s' with parameter %s</h3>",
                    results->group_analyses[0][grp_idx].group->template,
                    results->group_analyses[0][grp_idx].group->var_name);
            const struct meas *meas = results->meas + meas_idx;
            const struct cmd_group_analysis *analysis =
                results->group_analyses[meas_idx] + grp_idx;
            html_cmd_group(analysis, meas, meas_idx, grp_idx, f);
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

    html_paramter_analysis(results, f);
    html_compare(results, f);
    for (size_t bench_idx = 0; bench_idx < results->bench_count; ++bench_idx) {
        const struct bench *bench = results->benches + bench_idx;
        const struct bench_analysis *analysis = results->analyses + bench_idx;
        fprintf(f, "<div><h2>command '%s'</h2>", bench->cmd->str);
        for (size_t meas_idx = 0; meas_idx < bench->meas_count; ++meas_idx) {
            const struct meas *info = bench->cmd->meas + meas_idx;
            if (info->is_secondary)
                continue;
            html_distr(analysis, bench_idx, meas_idx, f);
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</body>");
}

static int run_bench(struct bench_analysis *analysis) {
    struct bench *bench = (void *)analysis->bench;
    if (warmup(bench->cmd) == -1)
        return -1;
    if (run_benchmark(bench) == -1)
        return -1;
    analyze_benchmark(analysis);
    return 0;
}

static int run_benchw(void *data) {
    g_rng_state = time(NULL);
    return run_bench(data);
}

static void *parfor_worker(void *raw) {
    struct parfor_data *data = raw;
    for (size_t i = data->low; i < data->high; ++i)
        if (data->fn((char *)data->arr + data->stride * i) == -1)
            return (void *)-1;
    return NULL;
}

static int parallel_for(int (*body)(void *), void *arr, size_t count,
                        size_t stride) {
    if (g_threads <= 1 || count == 1) {
        for (size_t i = 0; i < count; ++i) {
            void *data = (char *)arr + stride * i;
            if (body(data) == -1)
                return -1;
        }
        return 0;
    }
    int ret = -1;
    size_t thread_count = g_threads;
    if (count < thread_count)
        thread_count = count;

    assert(thread_count > 1);
    struct parfor_data *thread_data =
        calloc(thread_count, sizeof(*thread_data));
    size_t width = count / thread_count;
    size_t remainder = count % thread_count;
    for (size_t i = 0, cursor = 0; i < thread_count; ++i) {
        thread_data[i].fn = body;
        thread_data[i].arr = arr;
        thread_data[i].stride = stride;
        thread_data[i].low = cursor;
        size_t advance = width;
        if (i < remainder)
            ++advance;
        thread_data[i].high = cursor + advance;
        cursor += advance;
    }
    for (size_t i = 0; i < thread_count; ++i) {
        int ret = pthread_create(&thread_data[i].id, NULL, parfor_worker,
                                 thread_data + i);
        if (ret != 0) {
            for (size_t j = 0; j < i; ++j)
                pthread_join(thread_data[j].id, NULL);
            fprintf(stderr, "error: failed to spawn thread\n");
            goto err;
        }
    }
    ret = 0;
    for (size_t i = 0; i < thread_count; ++i) {
        void *thread_retval;
        pthread_join(thread_data[i].id, &thread_retval);
        if (thread_retval == (void *)-1)
            ret = -1;
    }
err:
    free(thread_data);
    return ret;
}

static int run_benches(const struct settings *settings,
                       struct bench_results *results) {
    results->bench_count = sb_len(settings->cmds);
    results->benches = calloc(results->bench_count, sizeof(*results->benches));
    results->analyses =
        calloc(results->bench_count, sizeof(*results->analyses));
    results->meas_count = sb_len(settings->meas);
    results->meas = settings->meas;
    for (size_t bench_idx = 0; bench_idx < results->bench_count; ++bench_idx) {
        struct bench *bench = results->benches + bench_idx;
        bench->prepare = settings->prepare_cmd;
        bench->cmd = settings->cmds + bench_idx;
        bench->meas_count = sb_len(bench->cmd->meas);
        bench->meas = calloc(bench->meas_count, sizeof(*bench->meas));
        struct bench_analysis *analysis = results->analyses + bench_idx;
        analysis->meas = calloc(bench->meas_count, sizeof(*analysis->meas));
        analysis->bench = bench;
    }

    return parallel_for(run_benchw, results->analyses, results->bench_count,
                        sizeof(*results->analyses));
}

static void analyze_benches(const struct settings *settings,
                            struct bench_results *results) {
    compare_benches(results);
    analyze_cmd_groups(settings, results);
}

static void print_analysis(const struct bench_results *results) {
    for (size_t i = 0; i < results->bench_count; ++i)
        print_benchmark_info(results->analyses + i);

    print_cmd_comparison(results);
    print_cmd_group_analysis(results);
}

static int handle_export(const struct settings *settings,
                         const struct bench_results *results) {
    switch (settings->export.kind) {
    case EXPORT_JSON:
        return export_json(results, settings->export.filename);
    case DONT_EXPORT:
        break;
    }
    return 0;
}

static int make_html_report(const struct bench_results *results,
                            const char *out_dir) {
    FILE *f = open_file_fmt("w", "%s/index.html", out_dir);
    if (f == NULL) {
        fprintf(stderr, "error: failed to create file %s/index.html\n",
                out_dir);
        return -1;
    }
    html_report(results, f);
    fclose(f);
    return 0;
}

static int handle_analyze(const struct bench_results *results,
                          enum analyze_mode mode, const char *out_dir) {
    if (mode == DONT_ANALYZE)
        return 0;

    if (mkdir(out_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            perror("mkdir");
            return -1;
        }
    }

    if (mode == ANALYZE_PLOT || mode == ANALYZE_HTML) {
        if (!python_found()) {
            fprintf(stderr, "error: failed to find python3 executable\n");
            return -1;
        }
        if (!python_has_matplotlib()) {
            fprintf(stderr,
                    "error: python does not have matplotlib installed\n");
            return -1;
        }

        if (g_plot_src && dump_plot_src(results, out_dir) == -1)
            return -1;

        if (make_plots(results, out_dir) == -1)
            return -1;

        if (make_plots_readme(results, out_dir) == -1)
            return -1;
    }

    if (mode == ANALYZE_HTML && make_html_report(results, out_dir) == -1)
        return -1;

    return 0;
}

static void free_bench_results(struct bench_results *results) {
    // these ifs are needed because results can be partially initialized in
    // case of failure
    if (results->benches) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            struct bench *bench = results->benches + i;
            sb_free(bench->exit_codes);
            for (size_t i = 0; i < bench->meas_count; ++i)
                sb_free(bench->meas[i]);
            free(bench->meas);
        }
    }
    if (results->analyses) {
        for (size_t i = 0; i < results->bench_count; ++i) {
            const struct bench_analysis *analysis = results->analyses + i;
            free(analysis->meas);
        }
    }
    for (size_t i = 0; i < results->meas_count; ++i) {
        for (size_t j = 0; j < results->group_count; ++j) {
            struct cmd_group_analysis *analysis =
                results->group_analyses[i] + j;
            free(analysis->data);
        }
        free(results->group_analyses[i]);
    }
    free(results->benches);
    free(results->analyses);
    free(results->group_analyses);
    free(results->fastest_meas);
}

static int run(const struct settings *settings) {
    int ret = -1;
    struct bench_results results = {0};
    if (run_benches(settings, &results) == -1)
        goto out;
    analyze_benches(settings, &results);
    print_analysis(&results);
    if (handle_export(settings, &results) == -1)
        goto out;
    if (handle_analyze(&results, settings->analyze_mode, settings->out_dir) ==
        -1)
        goto out;
    ret = 0;
out:
    free_bench_results(&results);
    return ret;
}

int main(int argc, char **argv) {
    int rc = EXIT_FAILURE;
    struct cli_settings cli = {0};
    parse_cli_args(argc, argv, &cli);

    struct settings settings = {0};
    if (init_settings(&cli, &settings) == -1)
        goto free_cli;

    g_rng_state = time(NULL);
    if (run(&settings) == -1)
        goto free_settings;

    rc = EXIT_SUCCESS;
free_settings:
    free_settings(&settings);
free_cli:
    free_cli_settings(&cli);
    return rc;
}
