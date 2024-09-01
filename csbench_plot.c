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
#include <math.h>
#include <stdlib.h>

// data needed to construct kde plot. Points here are computed from original
// timings.
struct kde_plot {
    const struct distr *distr;
    const struct meas *meas;
    const char *title;
    size_t count;
    double lower;
    double step;
    double *data;
    double mean;
    double mean_y;
    const char *output_filename;
    bool is_small;
};

struct kde_cmp_plot {
    const struct kde_cmp_params *params;
    size_t count;
    double lower;
    double step;
    double *a_kde, *b_kde;
    double a_mean, b_mean;
    double a_mean_y, b_mean_y;
    const char *output_filename;
    bool is_small;
};

struct prettify_plot {
    const char *units_str;
    double multiplier;
    bool logscale;
};

static void prettify_plot(const struct units *units, double min, double max,
                          struct prettify_plot *plot)
{
    if (log10(max / min) > 2.5)
        plot->logscale = 1;

    plot->multiplier = 1;
    if (units_is_time(units)) {
        switch (units->kind) {
        case MU_S:
            break;
        case MU_MS:
            plot->multiplier = 1e-3;
            break;
        case MU_US:
            plot->multiplier = 1e-6;
            break;
        case MU_NS:
            plot->multiplier = 1e-9;
            break;
        default:
            ASSERT_UNREACHABLE();
        }
        if (max < 1e-6 && min < 1e-6) {
            plot->units_str = "ns";
            plot->multiplier *= 1e9;
        } else if (max < 1e-3 && min < 1e-3) {
            plot->units_str = "us";
            plot->multiplier *= 1e6;
        } else if (max < 1.0 && min < 1.0) {
            plot->units_str = "ms";
            plot->multiplier *= 1e3;
        } else {
            plot->units_str = "s";
        }
    } else {
        plot->units_str = units_str(units);
    }
}

static void bar_plot_matplotlib(const struct meas_analysis *analysis,
                                const char *output_filename, FILE *f)
{
    size_t count = analysis->base->bench_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double v = analysis->benches[i]->mean.point;
        if (v > max)
            max = v;
        if (v < min)
            min = v;
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&analysis->meas->units, min, max, &prettify);
    fprintf(f, "data = [");
    for (size_t i = 0; i < count; ++i)
        fprintf(f, "%g, ",
                analysis->benches[i]->mean.point * prettify.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < count; ++i) {
        const struct bench_analysis *bench = analysis->base->bench_analyses + i;
        fprintf(f, "'%s', ", bench->name);
    }
    fprintf(f, "]\n"
               "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
    if (prettify.logscale)
        fprintf(f, "plt.xscale('log')\n");
    fprintf(f,
            "plt.barh(range(len(data)), data)\n"
            "plt.yticks(range(len(data)), names)\n"
            "plt.xlabel('mean %s [%s]')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            analysis->meas->name, prettify.units_str, output_filename);
}

static void group_plot_matplotlib(const struct group_analysis *analyses,
                                  size_t count, const struct meas *meas,
                                  const struct bench_var *var,
                                  const char *output_filename, FILE *f)
{
    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        for (size_t i = 0; i < var->value_count; ++i) {
            double v = analyses[grp_idx].data[i].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&meas->units, min, max, &prettify);

    fprintf(f, "x = [");
    for (size_t i = 0; i < var->value_count; ++i) {
        double v = analyses[0].data[i].value_double;
        fprintf(f, "%g, ", v);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        fprintf(f, "[");
        for (size_t i = 0; i < var->value_count; ++i)
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
        double high = analyses[grp_idx].data[var->value_count - 1].value_double;
        if (high > highest_x)
            highest_x = high;
    }

    double regr_x_step = (highest_x - lowest_x) / nregr;
    fprintf(f, "regrx = [");
    for (size_t i = 0; i < nregr + 1; ++i)
        fprintf(f, "%g, ", lowest_x + regr_x_step * i);
    fprintf(f, "]\n");
    fprintf(f, "regry = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        const struct group_analysis *analysis = analyses + grp_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < nregr + 1; ++i) {
            double regr =
                ols_approx(&analysis->regress, lowest_x + regr_x_step * i);
            fprintf(f, "%g, ", regr * prettify.multiplier);
        }
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
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
            var->name, meas->name, prettify.units_str, output_filename);
}

static void construct_kde(const struct distr *distr, double *kde,
                          size_t kde_size, double lower, double step)
{
    size_t count = distr->count;
    double st_dev = distr->st_dev.point;
    double iqr = distr->q3 - distr->q1;
    double h = 0.9 * fmin(st_dev, iqr / 1.34) * pow(count, -0.2);

    double k_mult = 1.0 / sqrt(2.0 * M_PI);
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
}

static double linear_interpolate(double lower, double step, const double *y,
                                 size_t count, double x)
{
    for (size_t i = 0; i < count - 1; ++i) {
        double x1 = lower + i * step;
        double x2 = lower + (i + 1) * step;
        if (x1 <= x && x <= x2) {
            double y1 = y[i];
            double y2 = y[i + 1];
            return (y1 * (x2 - x) + y2 * (x - x1)) / (x2 - x1);
        }
    }
    return 0.0;
}

static void kde_limits(const struct distr *distr, bool is_small, double *lower,
                       double *upper)
{
    double st_dev = distr->st_dev.point;
    double mean = distr->mean.point;
    if (is_small) {
        *lower = fmax(mean - 3.0 * st_dev, distr->p5);
        *upper = fmin(mean + 3.0 * st_dev, distr->p95);
    } else {
        *lower = fmax(mean - 6.0 * st_dev, distr->p1);
        *upper = fmin(mean + 6.0 * st_dev, distr->p99);
    }
}

#define init_kde_small_plot(_distr, _meas, _output_filename, _plot)            \
    init_kde_plot_internal(_distr, _meas, true, NULL, _output_filename, _plot)
#define init_kde_plot(_distr, _meas, _name, _output_filename, _plot)           \
    init_kde_plot_internal(_distr, _meas, false, _name, _output_filename, _plot)
static void init_kde_plot_internal(const struct distr *distr,
                                   const struct meas *meas, bool is_small,
                                   const char *name,
                                   const char *output_filename,
                                   struct kde_plot *plot)
{
    size_t kde_points = 200;
    double lower, upper;
    kde_limits(distr, is_small, &lower, &upper);
    double step = (upper - lower) / kde_points;

    plot->is_small = is_small;
    plot->title = name;
    plot->output_filename = output_filename;
    plot->meas = meas;
    plot->distr = distr;
    plot->count = kde_points;
    plot->lower = lower;
    plot->step = step;
    plot->data = malloc(sizeof(*plot->data) * plot->count);
    construct_kde(distr, plot->data, plot->count, lower, step);
    plot->mean = distr->mean.point;
    plot->mean_y =
        linear_interpolate(lower, step, plot->data, kde_points, plot->mean);
}

#define init_kde_cmp_small_plot(_params, _output_filename, _plot)              \
    init_kde_cmp_plot_internal(_params, true, _output_filename, _plot)
#define init_kde_cmp_plot(_params, _output_filename, _plot)                    \
    init_kde_cmp_plot_internal(_params, false, _output_filename, _plot)
static void init_kde_cmp_plot_internal(const struct kde_cmp_params *params,
                                       bool is_small,
                                       const char *output_filename,
                                       struct kde_cmp_plot *plot)
{
    size_t kde_points = 200;
    double lower, upper;
    {
        double a_lower, b_lower, a_upper, b_upper;
        kde_limits(params->a, false, &a_lower, &a_upper);
        kde_limits(params->b, false, &b_lower, &b_upper);
        lower = fmin(a_lower, b_lower);
        upper = fmax(a_upper, b_upper);
    }
    double step = (upper - lower) / kde_points;

    plot->params = params;
    plot->count = kde_points;
    plot->lower = lower;
    plot->step = step;
    plot->a_kde = malloc(sizeof(*plot->a_kde) * plot->count);
    plot->b_kde = malloc(sizeof(*plot->b_kde) * plot->count);
    construct_kde(params->a, plot->a_kde, plot->count, lower, step);
    construct_kde(params->b, plot->b_kde, plot->count, lower, step);
    plot->a_mean = params->a->mean.point;
    plot->b_mean = params->b->mean.point;
    plot->a_mean_y =
        linear_interpolate(lower, step, plot->a_kde, kde_points, plot->a_mean);
    plot->b_mean_y =
        linear_interpolate(lower, step, plot->b_kde, kde_points, plot->b_mean);
    plot->output_filename = output_filename;
    plot->is_small = is_small;
}

static void make_kde_small_plot(const struct kde_plot *plot, FILE *f)
{
    assert(plot->is_small);
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
            "plt.fill_between(x, y, interpolate=True, alpha=0.25)\n"
            "plt.vlines(%g, [0], [%g])\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            plot->mean * prettify.multiplier, plot->mean_y, plot->meas->name,
            prettify.units_str, plot->output_filename);
}

static void make_kde_plot(const struct kde_plot *plot, FILE *f)
{
    assert(!plot->is_small);
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
    fprintf(
        f,
        "import matplotlib as mpl\n"
        "mpl.use('svg')\n"
        "import matplotlib.pyplot as plt\n"
        "plt.fill_between(x, y, interpolate=True, alpha=0.25, label='PDF')\n"
        "plt.axvline(x=%f, label='mean')\n"
        "plt.plot(*zip(*reg_points), marker='o', ls='', markersize=2, "
        "label='\"clean\" sample')\n"
        "plt.plot(*zip(*mild_points), marker='o', ls='', markersize=2, "
        "color='orange', label='mild outliers')\n"
        "plt.plot(*zip(*severe_points), marker='o', ls='', markersize=2, "
        "color='red', label='severe outliers')\n",
        plot->mean * prettify.multiplier);
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
            "plt.ylabel('probability density, runs')\n"
            "plt.legend(loc='upper right')\n"
            "plt.title('%s')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->meas->name, prettify.units_str, plot->title,
            plot->output_filename);
}

static void make_kde_cmp_small_plot(const struct kde_cmp_plot *plot, FILE *f)
{
    assert(plot->is_small);
    double min = plot->lower;
    double max = plot->lower + (plot->count - 1) * plot->step;
    struct prettify_plot prettify = {0};
    prettify_plot(&plot->params->meas->units, min, max, &prettify);

    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ",
                (plot->lower + plot->step * i) * prettify.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "ay = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ", plot->a_kde[i]);
    fprintf(f, "]\n");
    fprintf(f, "by = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ", plot->b_kde[i]);
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "plt.fill_between(x, ay, interpolate=True, alpha=0.25)\n"
            "plt.fill_between(x, by, interpolate=True, alpha=0.25, "
            "facecolor='tab:orange')\n"
            "plt.vlines(%g, [0], [%g], color='tab:blue')\n"
            "plt.vlines(%g, [0], [%g], color='tab:orange')\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            plot->a_mean * prettify.multiplier, plot->a_mean_y,
            plot->b_mean * prettify.multiplier, plot->b_mean_y,
            plot->params->meas->name, prettify.units_str,
            plot->output_filename);
}

static void make_kde_cmp_plot(const struct kde_cmp_plot *plot, FILE *f)
{
    assert(!plot->is_small);
    double min = plot->lower;
    double max = plot->lower + (plot->count - 1) * plot->step;
    struct prettify_plot prettify = {0};
    prettify_plot(&plot->params->meas->units, min, max, &prettify);

    double max_y = -INFINITY;
    for (size_t i = 0; i < plot->count; ++i) {
        if (plot->a_kde[i] > max_y)
            max_y = plot->a_kde[i];
        if (plot->b_kde[i] > max_y)
            max_y = plot->b_kde[i];
    }

    fprintf(f, "x = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ",
                (plot->lower + plot->step * i) * prettify.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "ay = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ", plot->a_kde[i]);
    fprintf(f, "]\n");
    fprintf(f, "by = [");
    for (size_t i = 0; i < plot->count; ++i)
        fprintf(f, "%g, ", plot->b_kde[i]);
    fprintf(f, "]\n");
    fprintf(f, "a_points = [");
    for (size_t i = 0; i < plot->params->a->count; ++i) {
        double v = plot->params->a->data[i];
        if (v < plot->lower || v > plot->lower + plot->step * plot->count)
            continue;
        fprintf(f, "(%g, %g), ", v * prettify.multiplier,
                (double)(i + 1) / plot->params->a->count * max_y);
    }
    fprintf(f, "]\n");
    fprintf(f, "b_points = [");
    for (size_t i = 0; i < plot->params->b->count; ++i) {
        double v = plot->params->b->data[i];
        if (v < plot->lower || v > plot->lower + plot->step * plot->count)
            continue;
        fprintf(f, "(%g, %g), ", v * prettify.multiplier,
                (double)(i + 1) / plot->params->b->count * max_y);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "plt.fill_between(x, ay, interpolate=True, alpha=0.25, "
            "label='%s PDF')\n"
            "plt.plot(*zip(*a_points), marker='o', ls='', markersize=2, "
            "color='tab:blue', label='%s sample')\n"
            "plt.axvline(%g, color='tab:blue', label='%s mean')\n"
            "plt.fill_between(x, by, interpolate=True, alpha=0.25, "
            "facecolor='tab:orange', label='%s PDF')\n"
            "plt.plot(*zip(*b_points), marker='o', ls='', markersize=2, "
            "color='tab:orange', label='%s sample')\n"
            "plt.axvline(%g, color='tab:orange', label='%s mean')\n"
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('probability density, runs')\n"
            "plt.legend(loc='upper right')\n"
            "plt.title('%s')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->params->a_name, plot->params->a_name,
            plot->a_mean * prettify.multiplier, plot->params->a_name,
            plot->params->b_name, plot->params->b_name,
            plot->b_mean * prettify.multiplier, plot->params->b_name,
            plot->params->meas->name, prettify.units_str, plot->params->title,
            plot->output_filename);
}

static void free_kde_plot(struct kde_plot *plot) { free(plot->data); }

static void free_kde_cmp_plot(struct kde_cmp_plot *plot)
{
    free(plot->a_kde);
    free(plot->b_kde);
}

static void group_bar_plot_matplotlib(const struct meas_analysis *analysis,
                                      const char *output_filename, FILE *f)
{
    const struct bench_var *var = analysis->base->var;
    size_t count = analysis->base->group_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < var->value_count; ++j) {
            double v = analysis->group_analyses[i].data[j].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct prettify_plot prettify = {0};
    prettify_plot(&analysis->meas->units, min, max, &prettify);

    fprintf(f, "var_values = [");
    for (size_t i = 0; i < var->value_count; ++i)
        fprintf(f, "'%s', ", var->values[i]);
    fprintf(f, "]\n");
    fprintf(f, "times = {");
    for (size_t i = 0; i < count; ++i) {
        fprintf(f, "  '%s': [", analysis->group_analyses[i].group->name);
        for (size_t j = 0; j < var->value_count; ++j)
            fprintf(f, "%g, ",
                    analysis->group_analyses[i].data[j].mean *
                        prettify.multiplier);
        fprintf(f, "],\n");
    }
    fprintf(f, "}\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n"
               "import numpy as np\n"
               "x = np.arange(len(var_values))\n"
               "width = 1.0 / (len(times) + 1)\n"
               "multiplier = 0\n"
               "fig, ax = plt.subplots()\n"
               "for at, meas in times.items():\n"
               "  offset = width * multiplier\n"
               "  rects = ax.bar(x + offset, meas, width, label=at)\n"
               "  multiplier += 1\n");
    if (prettify.logscale)
        fprintf(f, "ax.set_yscale('log')\n");
    fprintf(f,
            "ax.set_ylabel('%s [%s]')\n"
            "plt.xticks(x, var_values)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            analysis->meas->name, prettify.units_str, output_filename);
}

static void kde_small_plot_matplotlib(const struct distr *distr,
                                      const struct meas *meas,
                                      const char *output_filename, FILE *f)
{
    struct kde_plot plot = {0};
    init_kde_small_plot(distr, meas, output_filename, &plot);
    make_kde_small_plot(&plot, f);
    free_kde_plot(&plot);
}

static void kde_plot_matplotlib(const struct distr *distr,
                                const struct meas *meas, const char *name,
                                const char *output_filename, FILE *f)
{
    struct kde_plot plot = {0};
    init_kde_plot(distr, meas, name, output_filename, &plot);
    make_kde_plot(&plot, f);
    free_kde_plot(&plot);
}

static void kde_cmp_small_plot_matplotlib(const struct kde_cmp_params *params,
                                          const char *output_filename, FILE *f)
{
    struct kde_cmp_plot plot = {0};
    init_kde_cmp_small_plot(params, output_filename, &plot);
    make_kde_cmp_small_plot(&plot, f);
    free_kde_cmp_plot(&plot);
}

static void kde_cmp_plot_matplotlib(const struct kde_cmp_params *params,
                                    const char *output_filename, FILE *f)
{
    struct kde_cmp_plot plot = {0};
    init_kde_cmp_plot(params, output_filename, &plot);
    make_kde_cmp_plot(&plot, f);
    free_kde_cmp_plot(&plot);
}

static bool python_found(void)
{
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s --version", g_python_executable);
    return shell_execute(buffer, -1, -1, -1, true);
}

static bool python_has_matplotlib(void)
{
    FILE *f;
    pid_t pid;
    if (!shell_launch_stdin_pipe(g_python_executable, &f, -1, -1, &pid))
        return false;
    fprintf(f, "import matplotlib\n");
    fclose(f);
    return process_wait_finished_correctly(pid, true);
}

bool get_plot_backend(enum plot_backend *backend)
{
    bool found_backend = false;
    if (!python_found()) {
        error("failed to find python executable '%s'", g_python_executable);
        return false;
    }
    if (python_has_matplotlib()) {
        *backend = PLOT_BACKEND_MATPLOTLIB;
        found_backend = true;
        if (g_plot_backend_override == PLOT_BACKEND_MATPLOTLIB)
            return true;
    } else if (g_plot_backend_override == PLOT_BACKEND_MATPLOTLIB) {
        error("selected plot backend (matplotlib) is not available");
        return false;
    }
    if (!found_backend) {
        error("Failed to find backend to use to make plots. 'matplotlib' "
              "has to be installed for '%s' python executable",
              g_python_executable);
        return false;
    }
    return true;
}

void init_plot_maker(enum plot_backend backend, struct plot_maker *maker)
{
    switch (backend) {
    case PLOT_BACKEND_MATPLOTLIB:
        maker->bar = bar_plot_matplotlib;
        maker->group_bar = group_bar_plot_matplotlib;
        maker->group = group_plot_matplotlib;
        maker->kde_small = kde_small_plot_matplotlib;
        maker->kde = kde_plot_matplotlib;
        maker->kde_cmp_small = kde_cmp_small_plot_matplotlib;
        maker->kde_cmp = kde_cmp_plot_matplotlib;
        break;
    default:
        ASSERT_UNREACHABLE();
    }
}
