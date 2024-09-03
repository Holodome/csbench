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

#define KDE_POINT_COUNT 200

struct kde_data {
    size_t point_count;
    double min, step, max;
    double *data;
    double mean_x;
    double mean_y;
};

struct plot_view {
    const char *units_str;
    double multiplier;
    bool logscale;
};

// data needed to construct kde plot. Points here are computed from original
// timings.
struct kde_plot {
    const struct distr *distr;
    const struct meas *meas;
    struct kde_data kde;
    const char *title;
    const char *output_filename;
    struct plot_view view;
    double max_y;
    bool is_small;
};

struct kde_cmp_plot {
    const struct meas_analysis *al;
    const struct distr *a, *b;
    size_t a_idx;
    size_t b_idx;
    size_t point_count;
    double min, step, max;
    struct kde_data a_kde, b_kde;
    const char *output_filename;
    struct plot_view view;
    double max_y;
    bool is_small;
};

struct kde_cmp_val {
    const struct distr *a, *b;
    double min, step, max;
    struct kde_data a_kde;
    struct kde_data b_kde;
    struct plot_view view;
    double max_y;
};

struct kde_cmp_group_plot {
    size_t rows, cols;
    const struct meas_analysis *al;
    size_t a_idx;
    size_t b_idx;
    size_t val_count;
    size_t point_count;
    struct kde_cmp_val *cmps;
    const char *output_filename;
};

static double positive_speedup(const struct speedup *sp)
{
    if (!sp->is_slower)
        return sp->est.point;
    return sp->inv_est.point;
}

static void init_plot_view(const struct units *units, double min, double max,
                           struct plot_view *view)
{
    if (log10(max / min) > 2.5)
        view->logscale = 1;

    view->multiplier = 1;
    if (units_is_time(units)) {
        switch (units->kind) {
        case MU_S:
            break;
        case MU_MS:
            view->multiplier = 1e-3;
            break;
        case MU_US:
            view->multiplier = 1e-6;
            break;
        case MU_NS:
            view->multiplier = 1e-9;
            break;
        default:
            ASSERT_UNREACHABLE();
        }
        if (max < 1e-6 && min < 1e-6) {
            view->units_str = "ns";
            view->multiplier *= 1e9;
        } else if (max < 1e-3 && min < 1e-3) {
            view->units_str = "us";
            view->multiplier *= 1e6;
        } else if (max < 1.0 && min < 1.0) {
            view->units_str = "ms";
            view->multiplier *= 1e3;
        } else {
            view->units_str = "s";
        }
    } else {
        view->units_str = units_str(units);
    }
}

static void bar_plot(const struct meas_analysis *al,
                     const char *output_filename, FILE *f)
{
    size_t count = al->base->bench_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double v = al->benches[i]->mean.point;
        if (v > max)
            max = v;
        if (v < min)
            min = v;
    }

    struct plot_view view = {0};
    init_plot_view(&al->meas->units, min, max, &view);
    fprintf(f, "data = [");
    for (size_t i = 0; i < count; ++i) {
        size_t bench_idx = ith_bench_idx(i, al);
        fprintf(f, "%g,", al->benches[bench_idx]->mean.point * view.multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    for (size_t i = 0; i < count; ++i) {
        size_t bench_idx = ith_bench_idx(i, al);
        const struct bench_analysis *bench =
            al->base->bench_analyses + bench_idx;
        fprintf(f, "'%s',", bench->name);
    }
    fprintf(f, "]\n");
    fprintf(f, "err = [");
    for (size_t i = 0; i < count; ++i) {
        size_t bench_idx = ith_bench_idx(i, al);
        fprintf(f, "%g,",
                al->benches[bench_idx]->st_dev.point * view.multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
    if (view.logscale)
        fprintf(f, "plt.xscale('log')\n");
    fprintf(f,
            "plt.rc('axes', axisbelow=True)\n"
            "plt.grid(axis='x')\n"
            "plt.barh(range(len(data)), data, xerr=err, alpha=0.6)\n"
            "plt.yticks(range(len(data)), names)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            al->meas->name, view.units_str, output_filename);
}

static void group_plot(const struct group_analysis *als, size_t count,
                       const struct meas *meas, const struct bench_var *var,
                       const char *output_filename, FILE *f)
{
    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        for (size_t i = 0; i < var->value_count; ++i) {
            double v = als[grp_idx].data[i].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct plot_view view = {0};
    init_plot_view(&meas->units, min, max, &view);

    fprintf(f, "x = [");
    for (size_t i = 0; i < var->value_count; ++i) {
        double v = als[0].data[i].value_double;
        fprintf(f, "%g,", v);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        fprintf(f, "[");
        for (size_t i = 0; i < var->value_count; ++i)
            fprintf(f, "%g,", als[grp_idx].data[i].mean * view.multiplier);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    size_t nregr = 100;
    double lowest_x = INFINITY;
    double highest_x = -INFINITY;
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        double low = als[grp_idx].data[0].value_double;
        if (low < lowest_x)
            lowest_x = low;
        double high = als[grp_idx].data[var->value_count - 1].value_double;
        if (high > highest_x)
            highest_x = high;
    }

    double regr_x_step = (highest_x - lowest_x) / nregr;
    fprintf(f, "regrx = [");
    for (size_t i = 0; i < nregr + 1; ++i)
        fprintf(f, "%g,", lowest_x + regr_x_step * i);
    fprintf(f, "]\n");
    fprintf(f, "regry = [");
    for (size_t grp_idx = 0; grp_idx < count; ++grp_idx) {
        const struct group_analysis *analysis = als + grp_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < nregr + 1; ++i) {
            double regr =
                ols_approx(&analysis->regress, lowest_x + regr_x_step * i);
            fprintf(f, "%g,", regr * view.multiplier);
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
    if (view.logscale)
        fprintf(f, "plt.yscale('log')\n");
    fprintf(f,
            "plt.xticks(x)\n"
            "plt.grid()\n"
            "plt.xlabel('%s')\n"
            "plt.ylabel('%s [%s]')\n"
            "plt.savefig('%s', bbox_inches='tight')\n",
            var->name, meas->name, view.units_str, output_filename);
}

static void construct_kde(const struct distr *distr, double *kde,
                          size_t kde_size, double min, double step)
{
    size_t count = distr->count;
    double st_dev = distr->st_dev.point;
    double iqr = distr->q3 - distr->q1;
    double h = 0.9 * fmin(st_dev, iqr / 1.34) * pow(count, -0.2);

    double k_mult = 1.0 / sqrt(2.0 * M_PI);
    for (size_t i = 0; i < kde_size; ++i) {
        double x = min + i * step;
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

static double linear_interpolate(double min, double step, const double *y,
                                 size_t count, double x)
{
    for (size_t i = 0; i < count - 1; ++i) {
        double x1 = min + i * step;
        double x2 = min + (i + 1) * step;
        if (x1 <= x && x <= x2) {
            double y1 = y[i];
            double y2 = y[i + 1];
            return (y1 * (x2 - x) + y2 * (x - x1)) / (x2 - x1);
        }
    }
    return 0.0;
}

static void kde_limits(const struct distr *distr, bool is_small, double *min,
                       double *max)
{
    double st_dev = distr->st_dev.point;
    double mean = distr->mean.point;
    if (is_small) {
        *min = fmax(mean - 3.0 * st_dev, distr->p5);
        *max = fmin(mean + 3.0 * st_dev, distr->p95);
    } else {
        *min = fmax(mean - 6.0 * st_dev, distr->p1);
        *max = fmin(mean + 6.0 * st_dev, distr->p99);
    }
}

static void kde_cmp_limits(const struct distr *a, const struct distr *b,
                           double *min, double *max)
{
    double a_min, b_min, a_max, b_max;
    kde_limits(a, false, &a_min, &a_max);
    kde_limits(b, false, &b_min, &b_max);
    *min = fmin(a_min, b_min);
    *max = fmax(a_max, b_max);
}

static void init_kde_data(const struct distr *distr, double min, double max,
                          size_t point_count, struct kde_data *kde)
{
    assert(point_count);
    kde->point_count = point_count;
    kde->min = min;
    kde->max = max;
    kde->step = (kde->max - kde->min) / kde->point_count;
    kde->data = calloc(kde->point_count, sizeof(*kde->data));
    construct_kde(distr, kde->data, kde->point_count, kde->min, kde->step);
    kde->mean_x = distr->mean.point;
    kde->mean_y = linear_interpolate(kde->min, kde->step, kde->data,
                                     kde->point_count, kde->mean_x);
}

static void free_kde_data(struct kde_data *kde) { free(kde->data); }

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
    double min, max;
    kde_limits(distr, is_small, &min, &max);
    plot->is_small = is_small;
    plot->title = name;
    plot->output_filename = output_filename;
    plot->meas = meas;
    plot->distr = distr;
    init_kde_data(distr, min, max, KDE_POINT_COUNT, &plot->kde);
    init_plot_view(&meas->units, min, max, &plot->view);

    double max_y = -INFINITY;
    for (size_t i = 0; i < plot->kde.point_count; ++i)
        if (plot->kde.data[i] > max_y)
            max_y = plot->kde.data[i];
    plot->max_y = max_y;
}

static void free_kde_plot(struct kde_plot *plot) { free_kde_data(&plot->kde); }

#define init_kde_cmp_small_plot(_al, _a_idx, _b_idx, _output_filename, _plot)  \
    init_kde_cmp_plot_internal(_al, _a_idx, _b_idx, true, _output_filename,    \
                               _plot)
#define init_kde_cmp_plot(_al, _a_idx, _b_idx, _output_filename, _plot)        \
    init_kde_cmp_plot_internal(_al, _a_idx, _b_idx, false, _output_filename,   \
                               _plot)
static void init_kde_cmp_plot_internal(const struct meas_analysis *al,
                                       size_t a_idx, size_t b_idx,
                                       bool is_small,
                                       const char *output_filename,
                                       struct kde_cmp_plot *plot)
{
    const struct distr *a = al->benches[a_idx];
    const struct distr *b = al->benches[b_idx];
    size_t point_count = KDE_POINT_COUNT;
    double min, max;
    kde_cmp_limits(a, b, &min, &max);

    plot->al = al;
    plot->a = a;
    plot->b = b;
    plot->a_idx = a_idx;
    plot->b_idx = b_idx;
    plot->min = min;
    plot->max = max;
    plot->point_count = point_count;
    plot->step = (max - min) / point_count;
    init_kde_data(a, min, max, point_count, &plot->a_kde);
    init_kde_data(b, min, max, point_count, &plot->b_kde);
    plot->output_filename = output_filename;
    plot->is_small = is_small;
    init_plot_view(&al->meas->units, min, max, &plot->view);

    double max_y = -INFINITY;
    for (size_t i = 0; i < point_count; ++i) {
        if (plot->a_kde.data[i] > max_y)
            max_y = plot->a_kde.data[i];
        if (plot->b_kde.data[i] > max_y)
            max_y = plot->b_kde.data[i];
    }
    plot->max_y = max_y;
}

static void free_kde_cmp_plot(struct kde_cmp_plot *plot)
{
    free_kde_data(&plot->a_kde);
    free_kde_data(&plot->b_kde);
}

static void make_kde_small_plot(const struct kde_plot *plot, FILE *f)
{
    assert(plot->is_small);
    const struct kde_data *kde = &plot->kde;
    double min = kde->min;
    double step = kde->step;
    size_t point_count = kde->point_count;
    const struct plot_view *view = &plot->view;

    fprintf(f, "y = [");
    for (size_t i = 0; i < point_count; ++i)
        fprintf(f, "%g,", kde->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "x = [");
    for (size_t i = 0; i < point_count; ++i)
        fprintf(f, "%g,", (min + step * i) * view->multiplier);
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
            kde->mean_x * view->multiplier, kde->mean_y, plot->meas->name,
            view->units_str, plot->output_filename);
}

static void make_kde_plot(const struct kde_plot *plot, FILE *f)
{
    assert(!plot->is_small);
    const struct kde_data *kde = &plot->kde;
    const struct plot_view *view = &plot->view;
    const struct distr *distr = plot->distr;
    double min = kde->min;
    double max = kde->max;
    double step = kde->step;
    size_t point_count = kde->point_count;

    double max_point_x = 0;
    fprintf(f, "points = [");
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < min || v > max)
            continue;
        if (v > max_point_x)
            max_point_x = v;
        fprintf(f, "(%g,%g), ", v * view->multiplier,
                (double)(i + 1) / distr->count * plot->max_y);
    }
    fprintf(f, "]\n");
    fprintf(f,
            "severe_points = list(filter(lambda x: x[0] < %g or x[0] > %g, "
            "points))\n",
            distr->outliers.low_severe_x * view->multiplier,
            distr->outliers.high_severe_x * view->multiplier);
    fprintf(f,
            "mild_points = list(filter(lambda x: (%g < x[0] < %g) or (%g < "
            "x[0] < "
            "%f), points))\n",
            distr->outliers.low_severe_x * view->multiplier,
            distr->outliers.low_mild_x * view->multiplier,
            distr->outliers.high_mild_x * view->multiplier,
            distr->outliers.high_severe_x * view->multiplier);
    fprintf(f, "reg_points = list(filter(lambda x: %g < x[0] < %g, points))\n",
            distr->outliers.low_mild_x * view->multiplier,
            distr->outliers.high_mild_x * view->multiplier);
    size_t kde_count = 0;
    fprintf(f, "x = [");
    for (size_t i = 0; i < point_count; ++i, ++kde_count) {
        double x = min + step * i;
        if (x > max_point_x)
            break;
        fprintf(f, "%g,", x * view->multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t i = 0; i < kde_count; ++i)
        fprintf(f, "%g,", kde->data[i]);
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
        plot->kde.mean_x * view->multiplier);
    if (plot->distr->outliers.low_mild_x > plot->kde.min &&
        plot->distr->outliers.low_mild != 0)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                plot->distr->outliers.low_mild_x * view->multiplier);
    if (plot->distr->outliers.low_severe_x > plot->kde.min &&
        plot->distr->outliers.low_severe != 0)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                plot->distr->outliers.low_severe_x * view->multiplier);
    if (plot->distr->outliers.high_mild_x < plot->kde.max &&
        plot->distr->outliers.high_mild != 0)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                plot->distr->outliers.high_mild_x * view->multiplier);
    if (plot->distr->outliers.high_severe_x < plot->kde.max &&
        plot->distr->outliers.high_severe != 0)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                plot->distr->outliers.high_severe_x * view->multiplier);
    fprintf(f,
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel('%s [%s]')\n"
            "plt.ylabel('probability density, runs')\n"
            "plt.legend(loc='upper right')\n"
            "plt.title('%s')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->meas->name, view->units_str, plot->title,
            plot->output_filename);
}

static void make_kde_cmp_small_plot(const struct kde_cmp_plot *plot, FILE *f)
{
    assert(plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;

    fprintf(f, "x = [");
    for (size_t i = 0; i < point_count; ++i)
        fprintf(f, "%g,", (min + step * i) * view->multiplier);
    fprintf(f, "]\n");
    fprintf(f, "ay = [");
    for (size_t i = 0; i < a_kde->point_count; ++i)
        fprintf(f, "%g,", a_kde->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "by = [");
    for (size_t i = 0; i < b_kde->point_count; ++i)
        fprintf(f, "%g,", b_kde->data[i]);
    fprintf(f, "]\n");
    const char *a_name = plot->al->base->bench_analyses[plot->a_idx].name;
    const char *b_name = plot->al->base->bench_analyses[plot->b_idx].name;
    fprintf(
        f,
        "import matplotlib as mpl\n"
        "mpl.use('svg')\n"
        "import matplotlib.pyplot as plt\n"
        "plt.fill_between(x, ay, interpolate=True, alpha=0.25, label='%s')\n"
        "plt.fill_between(x, by, interpolate=True, alpha=0.25, "
        "facecolor='tab:orange', label='%s')\n"
        "plt.vlines(%g, [0], [%g], color='tab:blue')\n"
        "plt.vlines(%g, [0], [%g], color='tab:orange')\n"
        "plt.tick_params(left=False, labelleft=False)\n"
        "plt.xlabel('%s [%s]')\n"
        "plt.ylabel('probability density')\n"
        "plt.legend(loc='upper right')\n"
        "plt.savefig('%s', bbox_inches='tight')\n",
        a_name, b_name, a_kde->mean_x * view->multiplier, a_kde->mean_y,
        b_kde->mean_x * view->multiplier, b_kde->mean_y, plot->al->meas->name,
        view->units_str, plot->output_filename);
}

static void make_kde_cmp_plot(const struct kde_cmp_plot *plot, FILE *f)
{
    assert(!plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double max = plot->max;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;

    fprintf(f, "x = [");
    for (size_t i = 0; i < point_count; ++i)
        fprintf(f, "%g,", (min + step * i) * view->multiplier);
    fprintf(f, "]\n");
    fprintf(f, "ay = [");
    for (size_t i = 0; i < a_kde->point_count; ++i)
        fprintf(f, "%g,", a_kde->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "by = [");
    for (size_t i = 0; i < b_kde->point_count; ++i)
        fprintf(f, "%g,", b_kde->data[i]);
    fprintf(f, "]\n");
    fprintf(f, "a_points = [");
    for (size_t i = 0; i < plot->a->count; ++i) {
        double v = plot->a->data[i];
        if (v < min || v > max)
            continue;
        fprintf(f, "(%g, %g),", v * view->multiplier,
                (double)(i + 1) / plot->a->count * plot->max_y);
    }
    fprintf(f, "]\n");
    fprintf(f, "b_points = [");
    for (size_t i = 0; i < plot->b->count; ++i) {
        double v = plot->b->data[i];
        if (v < plot->min || v > plot->max)
            continue;
        fprintf(f, "(%g, %g),", v * view->multiplier,
                (double)(i + 1) / plot->b->count * plot->max_y);
    }
    fprintf(f, "]\n");
    const char *a_name = plot->al->base->bench_analyses[plot->a_idx].name;
    const char *b_name = plot->al->base->bench_analyses[plot->b_idx].name;
    double p_value = plot->al->bench_speedups_reference == plot->a_idx
                         ? plot->al->p_values[plot->b_idx]
                         : plot->al->p_values[plot->a_idx];
    double diff =
        plot->al->bench_speedups_reference == plot->a_idx
            ? positive_speedup(plot->al->bench_speedups + plot->b_idx)
            : positive_speedup(plot->al->bench_speedups + plot->a_idx);
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
            "plt.title('%s vs %s p=%.2f diff=%.3f')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            a_name, a_name, a_kde->mean_x * view->multiplier, a_name, b_name,
            b_name, b_kde->mean_x * view->multiplier, b_name,
            plot->al->meas->name, view->units_str, a_name, b_name, p_value,
            diff, plot->output_filename);
}

static void group_bar_plot(const struct meas_analysis *al,
                           const char *output_filename, FILE *f)
{
    const struct bench_var *var = al->base->var;
    size_t grp_count = al->base->group_count;
    size_t val_count = var->value_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        const struct group_analysis *grp_al = al->group_analyses + grp_idx;
        for (size_t j = 0; j < var->value_count; ++j) {
            double v = grp_al->data[j].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }

    struct plot_view view = {0};
    init_plot_view(&al->meas->units, min, max, &view);

    fprintf(f, "var_values = [");
    for (size_t i = 0; i < val_count; ++i)
        fprintf(f, "'%s', ", var->values[i]);
    fprintf(f, "]\n");
    fprintf(f, "times = {");
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        const struct group_analysis *grp_al = al->group_analyses + grp_idx;
        fprintf(f, "  '%s': [", grp_al->group->name);
        for (size_t j = 0; j < val_count; ++j)
            fprintf(f, "%g,", grp_al->data[j].mean * view.multiplier);
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
    if (view.logscale)
        fprintf(f, "ax.set_yscale('log')\n");
    fprintf(f,
            "ax.set_ylabel('%s [%s]')\n"
            "plt.xticks(x, var_values)\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            al->meas->name, view.units_str, output_filename);
}

static void kde_small_plot(const struct distr *distr, const struct meas *meas,
                           const char *output_filename, FILE *f)
{
    struct kde_plot plot = {0};
    init_kde_small_plot(distr, meas, output_filename, &plot);
    make_kde_small_plot(&plot, f);
    free_kde_plot(&plot);
}

static void kde_plot(const struct distr *distr, const struct meas *meas,
                     const char *name, const char *output_filename, FILE *f)
{
    struct kde_plot plot = {0};
    init_kde_plot(distr, meas, name, output_filename, &plot);
    make_kde_plot(&plot, f);
    free_kde_plot(&plot);
}

static void kde_cmp_small_plot(const struct meas_analysis *al, size_t a_idx,
                               size_t b_idx, const char *output_filename,
                               FILE *f)
{
    struct kde_cmp_plot plot = {0};
    init_kde_cmp_small_plot(al, a_idx, b_idx, output_filename, &plot);
    make_kde_cmp_small_plot(&plot, f);
    free_kde_cmp_plot(&plot);
}

static void kde_cmp_plot(const struct meas_analysis *al, size_t a_idx,
                         size_t b_idx, const char *output_filename, FILE *f)
{
    struct kde_cmp_plot plot = {0};
    init_kde_cmp_plot(al, a_idx, b_idx, output_filename, &plot);
    make_kde_cmp_plot(&plot, f);
    free_kde_cmp_plot(&plot);
}

static size_t find_closest_lower_square(size_t x)
{
    for (size_t i = 1;; ++i) {
        size_t next = i + 1;
        if (next * next > x)
            return i;
    }
    ASSERT_UNREACHABLE();
}

static void init_kde_cmp_group_plot(const struct meas_analysis *al,
                                    size_t a_idx, size_t b_idx,
                                    const char *output_filename,
                                    struct kde_cmp_group_plot *plot)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t val_count = var->value_count;
    size_t point_count = KDE_POINT_COUNT;
    plot->rows = find_closest_lower_square(val_count);
    if (plot->rows > 5)
        plot->rows = 5;
    plot->cols = (val_count + plot->rows - 1) / plot->rows;
    plot->al = al;
    plot->a_idx = a_idx;
    plot->b_idx = b_idx;
    plot->val_count = val_count;
    plot->output_filename = output_filename;
    plot->cmps = calloc(val_count, sizeof(*plot->cmps));
    plot->point_count = point_count;
    const struct group_analysis *a_grp = al->group_analyses + a_idx;
    const struct group_analysis *b_grp = al->group_analyses + b_idx;
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        struct kde_cmp_val *cmp = plot->cmps + val_idx;
        const struct distr *a = a_grp->data[val_idx].distr;
        const struct distr *b = b_grp->data[val_idx].distr;
        double min, max;
        kde_cmp_limits(a, b, &min, &max);
        double step = (max - min) / point_count;
        cmp->a = a;
        cmp->b = b;
        cmp->min = min;
        cmp->max = max;
        cmp->step = step;
        init_kde_data(a, min, max, point_count, &cmp->a_kde);
        init_kde_data(b, min, max, point_count, &cmp->b_kde);
        init_plot_view(&al->meas->units, min, max, &cmp->view);

        double max_y = -INFINITY;
        for (size_t i = 0; i < point_count; ++i) {
            if (cmp->a_kde.data[i] > max_y)
                max_y = cmp->a_kde.data[i];
            if (cmp->b_kde.data[i] > max_y)
                max_y = cmp->b_kde.data[i];
        }
        cmp->max_y = max_y;
    }
}

static void free_kde_cmp_group_plot(struct kde_cmp_group_plot *plot)
{
    for (size_t i = 0; i < plot->val_count; ++i) {
        free_kde_data(&plot->cmps[i].a_kde);
        free_kde_data(&plot->cmps[i].b_kde);
    }
    free(plot->cmps);
}

static void make_kde_cmp_group_plot(const struct kde_cmp_group_plot *plot,
                                    FILE *f)
{
    const struct meas_analysis *al = plot->al;
    size_t val_count = plot->val_count;
    size_t point_count = plot->point_count;

    fprintf(f, "x = [");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < point_count; ++i)
            fprintf(f, "%g,",
                    (cmp->min + cmp->step * i) * cmp->view.multiplier);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "ay = [");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < point_count; ++i)
            fprintf(f, "%g,", cmp->a_kde.data[i]);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "by = [");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < point_count; ++i)
            fprintf(f, "%g,", cmp->b_kde.data[i]);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "a_points = [");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        const struct distr *a = cmp->a;
        fprintf(f, "[");
        for (size_t i = 0; i < a->count; ++i) {
            double v = a->data[i];
            if (v < cmp->min || v > cmp->max)
                continue;
            fprintf(f, "(%g, %g),", v * cmp->view.multiplier,
                    (double)(i + 1) / a->count * cmp->max_y);
        }
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "b_points = [");
    for (size_t val_idx = 0; val_idx < plot->val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        const struct distr *b = cmp->b;
        fprintf(f, "[");
        for (size_t i = 0; i < b->count; ++i) {
            double v = b->data[i];
            if (v < cmp->min || v > cmp->max)
                continue;
            fprintf(f, "(%g, %g),", v * cmp->view.multiplier,
                    (double)(i + 1) / b->count * cmp->max_y);
        }
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "titles = [");
    for (size_t i = 0; i < plot->val_count; ++i) {
        double p_value = al->val_bench_speedups_references[i] == plot->a_idx
                             ? al->var_p_values[i][plot->b_idx]
                             : al->var_p_values[i][plot->a_idx];
        double speedup =
            al->val_bench_speedups_references[i] == plot->a_idx
                ? positive_speedup(al->val_bench_speedups[i] + plot->b_idx)
                : positive_speedup(al->val_bench_speedups[i] + plot->a_idx);
        fprintf(f, "'%s=%s p=%.2f diff=%.3f',", al->base->var->name,
                al->base->var->values[i], p_value, speedup);
    }
    fprintf(f, "]\n");
    fprintf(f, "xlabels = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "'%s [%s]',", al->meas->name, plot->cmps[i].view.units_str);
    fprintf(f, "]\n");
    fprintf(f, "a_means = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "%g,",
                plot->cmps[i].a_kde.mean_x * plot->cmps[i].view.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "b_means = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "%g,",
                plot->cmps[i].b_kde.mean_x * plot->cmps[i].view.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "def make_plot(x, ay, by, a_mean, b_mean, a_points, b_points, "
               "a_name, b_name, title, xlabel, ax):\n"
               "  ax.fill_between(x, ay, interpolate=True, alpha=0.25, "
               "label=a_name)\n"
               "  ax.plot(*zip(*a_points), marker='o', ls='', markersize=2, "
               "color='tab:blue')\n"
               "  ax.axvline(a_mean, color='tab:blue')\n"
               "  ax.fill_between(x, by, interpolate=True, alpha=0.25, "
               "facecolor='tab:orange', label=b_name)\n"
               "  ax.plot(*zip(*b_points), marker='o', ls='', markersize=2, "
               "color='tab:orange')\n"
               "  ax.axvline(b_mean, color='tab:orange')\n"
               "  ax.tick_params(left=False, labelleft=False)\n"
               "  ax.set_xlabel(xlabel)\n"
               "  ax.set_ylabel('probability density, runs')\n"
               "  ax.legend(loc='upper right')\n"
               "  ax.set_title(title)\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "fig, axes = plt.subplots(%zu, %zu)\n"
            "if %zu == 1: axes = [axes]\n"
            "row = col = 0\n"
            "for i in range(%zu):\n"
            "  make_plot(x[i], ay[i], by[i], a_means[i], b_means[i], "
            "a_points[i], b_points[i], '%s', '%s', titles[i], "
            "xlabels[i], axes[row][col])\n"
            "  col += 1\n"
            "  if col >= %zu:\n"
            "    col = 0\n"
            "    row += 1\n"
            "while True:\n"
            "  if row == %zu: break\n"
            "  axes[row][col].remove()\n"
            "  col += 1\n"
            "  if col >= %zu:\n"
            "    col = 0\n"
            "    row += 1\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(%zu, %zu)\n"
            "fig.tight_layout()\n"
            "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
            plot->rows, plot->cols, plot->rows, plot->val_count,
            al->group_analyses[plot->a_idx].group->name,
            al->group_analyses[plot->b_idx].group->name, plot->cols, plot->rows,
            plot->cols, plot->cols * 5, plot->rows * 5, plot->output_filename);
}

static void kde_cmp_group_plot(const struct meas_analysis *al, size_t a_idx,
                               size_t b_idx, const char *output_filename,
                               FILE *f)
{
    struct kde_cmp_group_plot plot = {0};
    init_kde_cmp_group_plot(al, a_idx, b_idx, output_filename, &plot);
    make_kde_cmp_group_plot(&plot, f);
    free_kde_cmp_group_plot(&plot);
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
        maker->bar = bar_plot;
        maker->group_bar = group_bar_plot;
        maker->group = group_plot;
        maker->kde_small = kde_small_plot;
        maker->kde = kde_plot;
        maker->kde_cmp_small = kde_cmp_small_plot;
        maker->kde_cmp = kde_cmp_plot;
        maker->kde_cmp_group = kde_cmp_group_plot;
        break;
    default:
        ASSERT_UNREACHABLE();
    }
}
