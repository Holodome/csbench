#include "csbench.h"

#include <math.h>
#include <stdlib.h>
#include <assert.h>

struct prettify_plot {
    const char *units_str;
    double multiplier;
    bool logscale;
};

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

void violin_plot(const struct bench *benches, size_t bench_count,
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

void bar_plot(const struct bench_analysis *analyses, size_t count,
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

void group_plot(const struct cmd_group_analysis *analyses, size_t count,
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
                          size_t kde_size, bool is_ext, double *lowerp,
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

void init_kde_plot_internal(const struct distr *distr, const char *title,
                            const struct meas *meas, bool is_ext,
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

void make_kde_plot(const struct kde_plot *plot, FILE *f) {
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

void make_kde_plot_ext(const struct kde_plot *plot, FILE *f) {
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

void free_kde_plot(struct kde_plot *plot) { free(plot->data); }

