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
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KDE_POINT_COUNT 200

struct plot_view {
    const char *units_str;
    double multiplier;
    bool logscale;
};

struct bar_plot {
    const struct meas_analysis *al;
    struct plot_view view;
};

struct group_bar_plot {
    const struct meas_analysis *al;
    struct plot_view view;
};

struct group_regr_plot {
    const struct meas_analysis *al;
    size_t idx;
    struct plot_view view;
    const struct group_analysis *als;
    size_t count;
    size_t nregr;
    double highest_x;
    double lowest_x;
    double regr_x_step;
};

struct kde_data {
    size_t point_count;
    double min, step, max;
    double *data;
    double mean_x;
    double mean_y;
};

struct kde_plot {
    const struct distr *distr;
    const struct meas *meas;
    struct kde_data kde;
    const char *title;
    struct plot_view view;
    double max_y;
    bool is_small;
    size_t displayed_severe_count;
    size_t displayed_mild_count;
};

struct kde_cmp_plot {
    const struct meas_analysis *al;
    size_t a_idx, b_idx;
    const struct distr *a, *b;
    size_t point_count;
    double min, step, max;
    struct kde_data a_kde, b_kde;
    struct plot_view view;
    const char *title;
    double max_y;
    bool is_small;
    const char *a_name;
    const char *b_name;
};

struct kde_cmp_val {
    const struct distr *a, *b;
    double min, step, max;
    struct kde_data a_kde;
    struct kde_data b_kde;
    struct plot_view view;
    double max_y;
    const char *title;
};

struct kde_cmp_group_plot {
    size_t rows, cols;
    const struct meas_analysis *al;
    size_t ref_idx;
    size_t grp_idx;
    size_t val_count;
    size_t point_count;
    struct kde_cmp_val *cmps;
};

static FILE *gnuplot_data_file(struct plot_maker_ctx *ctx, const char **name)
{
    *name = csfmt("%s/gnuplot-data/%zu.data", g_out_dir, (*ctx->gnuplot_data_idx)++);
    FILE *f = fopen(*name, "w");
    if (f == NULL) {
        csfmtperror("failed to open file '%s' for writing", *name);
        return NULL;
    }
    return f;
}

static void init_plot_view(const struct units *units, double min, double max,
                           struct plot_view *view)
{
    memset(view, 0, sizeof(*view));
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

static void construct_kde(const struct distr *distr, double *kde, size_t kde_size, double min,
                          double step)
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

static double linear_interpolate(double min, double step, const double *y, size_t count,
                                 double x)
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

static void kde_limits(const struct distr *distr, bool is_small, double *min, double *max)
{
    double st_dev = distr->st_dev.point;
    double mean = distr->mean.point;
    if (is_small) {
        *min = fmax(mean - 3.0 * st_dev, distr->p5 - 1e-6);
        *max = fmin(mean + 3.0 * st_dev, distr->p95 + 1e-6);
    } else {
        *min = fmax(mean - 6.0 * st_dev, distr->p1 - 1e-6);
        *max = fmin(mean + 6.0 * st_dev, distr->p99 + 1e-6);
    }
    // Limit so we do not try to plot the points that are TOO far from mean
    if (mean > distr->outliers.low_severe_x)
        *min = fmax(*min, mean - (mean - distr->outliers.low_severe_x) * 2);
    if (mean < distr->outliers.high_severe_x)
        *max = fmin(*max, mean + (distr->outliers.high_severe_x - mean) * 2);
}

static void kde_cmp_limits(const struct distr *a, const struct distr *b, double *min,
                           double *max)
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
    memset(kde, 0, sizeof(*kde));
    assert(point_count);
    kde->point_count = point_count;
    kde->min = min;
    kde->max = max;
    kde->step = (kde->max - kde->min) / (kde->point_count - 1);
    kde->data = calloc(kde->point_count, sizeof(*kde->data));
    construct_kde(distr, kde->data, kde->point_count, kde->min, kde->step);
    kde->mean_x = distr->mean.point;
    kde->mean_y =
        linear_interpolate(kde->min, kde->step, kde->data, kde->point_count, kde->mean_x);
}

static void free_kde_data(struct kde_data *kde)
{
    free(kde->data);
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

static void init_bar_plot(const struct meas_analysis *al, struct bar_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    size_t count = al->base->bench_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double v = al->benches[i]->mean.point;
        if (v > max)
            max = v;
        if (v < min)
            min = v;
    }
    plot->al = al;
    init_plot_view(&al->meas->units, min, max, &plot->view);
}

static void init_group_bar(const struct meas_analysis *al, struct group_bar_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    const struct bench_param *param = al->base->param;
    size_t grp_count = al->base->group_count;
    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        const struct group_analysis *grp_al = al->group_analyses + grp_idx;
        for (size_t j = 0; j < param->value_count; ++j) {
            double v = grp_al->data[j].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }
    plot->al = al;
    init_plot_view(&al->meas->units, min, max, &plot->view);
}

static void init_group_regr(const struct meas_analysis *al, size_t idx,
                            struct group_regr_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    const struct bench_param *param = al->base->param;
    plot->al = al;
    plot->idx = idx;
    if (idx == (size_t)-1) {
        plot->als = al->group_analyses;
        plot->count = al->base->group_count;
    } else {
        plot->als = al->group_analyses + idx;
        plot->count = 1;
    }

    double max = -INFINITY, min = INFINITY;
    for (size_t grp_idx = 0; grp_idx < plot->count; ++grp_idx) {
        for (size_t i = 0; i < param->value_count; ++i) {
            double v = plot->als[grp_idx].data[i].mean;
            if (v > max)
                max = v;
            if (v < min)
                min = v;
        }
    }
    init_plot_view(&al->meas->units, min, max, &plot->view);
    plot->nregr = 100;
    plot->lowest_x = INFINITY;
    plot->highest_x = -INFINITY;
    if (plot->count != 1) {
        foreach_group_by_avg_idx (grp_idx, plot->al) {
            double low = plot->als[grp_idx].data[0].value_double;
            if (low < plot->lowest_x)
                plot->lowest_x = low;
            double high = plot->als[grp_idx].data[param->value_count - 1].value_double;
            if (high > plot->highest_x)
                plot->highest_x = high;
        }
    } else {
        double low = plot->als[0].data[0].value_double;
        if (low < plot->lowest_x)
            plot->lowest_x = low;
        double high = plot->als[0].data[param->value_count - 1].value_double;
        if (high > plot->highest_x)
            plot->highest_x = high;
    }
    plot->regr_x_step = (plot->highest_x - plot->lowest_x) / plot->nregr;
}

#define init_kde_small_plot(_distr, _meas, _plot)                                            \
    init_kde_plot_internal(_distr, _meas, true, NULL, _plot)
#define init_kde_plot(_distr, _meas, _name, _plot)                                           \
    init_kde_plot_internal(_distr, _meas, false, _name, _plot)
static void init_kde_plot_internal(const struct distr *distr, const struct meas *meas,
                                   bool is_small, const char *name, struct kde_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    double min, max;
    kde_limits(distr, is_small, &min, &max);
    plot->is_small = is_small;
    plot->title = name;
    plot->meas = meas;
    plot->distr = distr;
    init_kde_data(distr, min, max, KDE_POINT_COUNT, &plot->kde);
    init_plot_view(&meas->units, min, max, &plot->view);
    double max_y = -INFINITY;
    for (size_t i = 0; i < plot->kde.point_count; ++i) {
        if (plot->kde.data[i] > max_y)
            max_y = plot->kde.data[i];
    }
    plot->max_y = max_y;
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < min || v > max)
            continue;
        if (!(v < distr->outliers.low_severe_x || v > distr->outliers.high_severe_x))
            continue;
        ++plot->displayed_severe_count;
    }
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < min || v > max)
            continue;
        if (!((v > distr->outliers.low_severe_x && v < distr->outliers.low_mild_x) ||
              (v < distr->outliers.high_severe_x && v > distr->outliers.high_mild_x)))
            continue;
        ++plot->displayed_mild_count;
    }
}

static void free_kde_plot(struct kde_plot *plot)
{
    free_kde_data(&plot->kde);
}

static void init_kde_cmp_plot_internal1(const struct meas_analysis *al, size_t a_idx,
                                        size_t b_idx, const struct distr *a,
                                        const struct distr *b, const char *a_name,
                                        const char *b_name, const char *title, bool is_small,
                                        struct kde_cmp_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    size_t point_count = KDE_POINT_COUNT;
    double min, max;
    kde_cmp_limits(a, b, &min, &max);

    plot->al = al;
    plot->a_idx = a_idx;
    plot->b_idx = b_idx;
    plot->a = a;
    plot->b = b;
    plot->a_name = a_name;
    plot->b_name = b_name;
    plot->title = title;
    plot->min = min;
    plot->max = max;
    plot->point_count = point_count;
    plot->step = (max - min) / point_count;
    init_kde_data(a, min, max, point_count, &plot->a_kde);
    init_kde_data(b, min, max, point_count, &plot->b_kde);
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

#define init_kde_cmp_small_plot(_al, _bench_idx, _plot)                                      \
    init_kde_cmp_plot_internal(_al, _bench_idx, true, _plot)
#define init_kde_cmp_plot(_al, _bench_idx, _plot)                                            \
    init_kde_cmp_plot_internal(_al, _bench_idx, false, _plot)
static void init_kde_cmp_plot_internal(const struct meas_analysis *al, size_t bench_idx,
                                       bool is_small, struct kde_cmp_plot *plot)
{
    size_t ref_idx = al->bench_cmp.ref;
    const struct distr *a = al->benches[ref_idx];
    const struct distr *b = al->benches[bench_idx];
    const char *a_name = bench_name(al->base, ref_idx);
    const char *b_name = bench_name(al->base, bench_idx);
    double p_value = al->bench_cmp.p_values[bench_idx];
    double diff = al->bench_cmp.speedups[bench_idx].est.point;
    const char *title = csfmt("%s vs %s p=%.2f diff=%.3fx", a_name, b_name, p_value, diff);
    init_kde_cmp_plot_internal1(al, ref_idx, bench_idx, a, b, a_name, b_name, title, is_small,
                                plot);
}

#define init_kde_cmp_per_val_small_plot(_al, _grp_idx, _val_idx, _plot)                      \
    init_kde_cmp_per_val_plot_internal(_al, _grp_idx, _val_idx, true, _plot)
#define init_kde_cmp_per_val_plot(_al, _grp_idx, _val_idx, _plot)                            \
    init_kde_cmp_per_val_plot_internal(_al, _grp_idx, _val_idx, false, _plot)
static void init_kde_cmp_per_val_plot_internal(const struct meas_analysis *al, size_t grp_idx,
                                               size_t val_idx, bool is_small,
                                               struct kde_cmp_plot *plot)
{
    const struct bench_param *param = al->base->param;
    size_t ref_idx = al->pval_cmps[val_idx].ref;
    const struct group_analysis *a_grp = al->group_analyses + ref_idx;
    const struct group_analysis *b_grp = al->group_analyses + grp_idx;
    const struct distr *a = a_grp->data[val_idx].distr;
    const struct distr *b = b_grp->data[val_idx].distr;
    const char *a_name = bench_group_name(al->base, ref_idx);
    const char *b_name = bench_group_name(al->base, grp_idx);
    double p_value = ref_idx == al->pval_cmps[val_idx].p_values[grp_idx];
    double diff = al->pval_cmps[val_idx].speedups[grp_idx].est.point;
    const char *title = csfmt("%s=%s %s vs %s p=%.2f diff=%.3fx", param->name,
                              param->values[val_idx], a_name, b_name, p_value, diff);
    init_kde_cmp_plot_internal1(al, ref_idx, grp_idx, a, b, a_name, b_name, title, is_small,
                                plot);
}

static void free_kde_cmp_plot(struct kde_cmp_plot *plot)
{
    free_kde_data(&plot->a_kde);
    free_kde_data(&plot->b_kde);
}

static void init_kde_cmp_group_plot(const struct meas_analysis *al, size_t grp_idx,
                                    struct kde_cmp_group_plot *plot)
{
    memset(plot, 0, sizeof(*plot));
    size_t ref_idx = al->group_avg_cmp.ref;
    const struct analysis *base = al->base;
    const struct bench_param *param = base->param;
    size_t val_count = param->value_count;
    size_t point_count = KDE_POINT_COUNT;
    plot->rows = find_closest_lower_square(val_count);
    if (plot->rows > 5)
        plot->rows = 5;
    plot->cols = (val_count + plot->rows - 1) / plot->rows;
    plot->al = al;
    plot->ref_idx = ref_idx;
    plot->grp_idx = grp_idx;
    plot->val_count = val_count;
    plot->cmps = calloc(val_count, sizeof(*plot->cmps));
    plot->point_count = point_count;
    const struct group_analysis *a_grp = al->group_analyses + ref_idx;
    const struct group_analysis *b_grp = al->group_analyses + grp_idx;
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
        double p_value = al->group_avg_cmp.pval_cmps[val_idx].p_values[grp_idx];
        double diff = al->group_avg_cmp.pval_cmps[val_idx].speedups[grp_idx].est.point;
        cmp->title = csfmt("%s=%s p=%.2f diff=%.3fx", al->base->param->name,
                           al->base->param->values[val_idx], p_value, diff);
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

static const char *mpl_nth_color(size_t n)
{
    static const char *colors[] = {"tab:blue",   "tab:orange", "tab:green", "tab:red",
                                   "tab:purple", "tab:brown",  "tab:pink",  "tab:gray",
                                   "tab:olive",  "tab:cyan"};
    return colors[n % (sizeof(colors) / sizeof(colors[0]))];
}

static void make_bar_mpl(const struct bar_plot *plot, struct plot_maker_ctx *ctx)
{
    const struct meas_analysis *al = plot->al;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    fprintf(f, "data = [");
    foreach_bench_idx (bench_idx, al) {
        fprintf(f, "%g,", al->benches[bench_idx]->mean.point * view->multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "names = [");
    foreach_bench_idx (bench_idx, al) {
        fprintf(f, "'%s',", bench_name(al->base, bench_idx));
    }
    fprintf(f, "]\n");
    fprintf(f, "err = [");
    foreach_bench_idx (bench_idx, al) {
        fprintf(f, "%g,", al->benches[bench_idx]->st_dev.point * view->multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "colors = [");
    foreach_bench_idx (bench_idx, al) {
        fprintf(f, "'%s',", mpl_nth_color(bench_idx));
    }
    fprintf(f, "]\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
    if (view->logscale)
        fprintf(f, "plt.yscale('log')\n");
    fprintf(f,
            "plt.rc('axes', axisbelow=True)\n"
            "plt.grid(axis='y')\n"
            "plt.bar(range(len(data)), data, yerr=err, alpha=0.6, color=colors)\n"
            "plt.xticks(range(len(data)), names)\n"
            "plt.ylabel(r'%s [%s]')\n"
            "plt.savefig(r'%s', bbox_inches='tight')\n",
            al->meas->name, view->units_str, //
            ctx->image_filename              //
    );
}

static void make_group_bar_mpl(const struct group_bar_plot *plot, struct plot_maker_ctx *ctx)
{
    const struct meas_analysis *al = plot->al;
    const struct analysis *base = al->base;
    const struct bench_param *param = base->param;
    const struct plot_view *view = &plot->view;
    size_t val_count = param->value_count;
    FILE *f = ctx->f;
    fprintf(f, "param_values = [");
    for (size_t i = 0; i < val_count; ++i)
        fprintf(f, "'%s', ", param->values[i]);
    fprintf(f, "]\n");
    fprintf(f, "times = {");
    foreach_group_by_avg_idx (grp_idx, al) {
        const struct group_analysis *grp_al = al->group_analyses + grp_idx;
        fprintf(f, "  r'%s': ([", bench_group_name(base, grp_idx));
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
            fprintf(f, "%g,", grp_al->data[val_idx].mean * view->multiplier);
        fprintf(f, "],\n");
        fprintf(f, "[");
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
            fprintf(f, "%g,", grp_al->data[val_idx].distr->st_dev.point * view->multiplier);
        fprintf(f, "]),\n");
    }
    fprintf(f, "}\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n"
               "import numpy as np\n"
               "x = np.arange(len(param_values))\n"
               "width = 1.0 / (len(times) + 1)\n"
               "multiplier = 0\n"
               "fig, ax = plt.subplots()\n"
               "for at, (meas, err) in times.items():\n"
               "  offset = width * multiplier\n"
               "  rects = ax.bar(x + offset, meas, width, "
               "label=at, alpha=0.6, yerr=err)\n"
               "  multiplier += 1\n");
    if (view->logscale)
        fprintf(f, "ax.set_yscale('log')\n");
    fprintf(f,
            "ax.set_ylabel(r'%s [%s]')\n"
            "plt.xticks(x + width * (%zu - 1) / 2, param_values)\n"
            "ax.set_axisbelow(True)\n"
            "plt.grid(axis='y')\n"
            "plt.legend(loc='best')\n"
            "plt.savefig(r'%s', dpi=100, bbox_inches='tight')\n",
            al->meas->name, view->units_str, //
            base->group_count,               //
            ctx->image_filename              //
    );
}

static void make_group_regr_mpl(const struct group_regr_plot *plot,
                                struct plot_maker_ctx *ctx)
{
    const struct bench_param *param = plot->al->base->param;
    const struct group_analysis *als = plot->als;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    fprintf(f, "x = [");
    for (size_t i = 0; i < param->value_count; ++i) {
        double v = als[0].data[i].value_double;
        fprintf(f, "%g,", v);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    if (plot->count != 1) {
        foreach_group_by_avg_idx (grp_idx, plot->al) {
            fprintf(f, "[");
            for (size_t i = 0; i < param->value_count; ++i)
                fprintf(f, "%g,", als[grp_idx].data[i].mean * view->multiplier);
            fprintf(f, "],");
        }
    } else {
        fprintf(f, "[");
        for (size_t i = 0; i < param->value_count; ++i)
            fprintf(f, "%g,", als[0].data[i].mean * view->multiplier);
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "regrx = [");
    for (size_t i = 0; i < plot->nregr + 1; ++i)
        fprintf(f, "%g,", plot->lowest_x + plot->regr_x_step * i);
    fprintf(f, "]\n");
    fprintf(f, "regry = [");
    if (plot->count != 1) {
        foreach_group_by_avg_idx (grp_idx, plot->al) {
            const struct group_analysis *analysis = als + grp_idx;
            fprintf(f, "[");
            for (size_t i = 0; i < plot->nregr + 1; ++i) {
                double regr =
                    ols_approx(&analysis->regress, plot->lowest_x + plot->regr_x_step * i);
                fprintf(f, "%g,", regr * view->multiplier);
            }
            fprintf(f, "],");
        }
    } else {
        fprintf(f, "[");
        const struct group_analysis *analysis = als + 0;
        for (size_t i = 0; i < plot->nregr + 1; ++i) {
            double regr =
                ols_approx(&analysis->regress, plot->lowest_x + plot->regr_x_step * i);
            fprintf(f, "%g,", regr * view->multiplier);
        }
        fprintf(f, "],");
    }
    fprintf(f, "]\n");
    fprintf(f, "import matplotlib as mpl\n"
               "mpl.use('svg')\n"
               "import matplotlib.pyplot as plt\n");
    if (plot->count != 1) {
        foreach_group_by_avg_idx (grp_idx, plot->al) {
            const char *color = mpl_nth_color(grp_idx);
            fprintf(f,
                    "plt.plot(x, y[%zu], '.-', color='%s', label=r'%s')\n"
                    "plt.plot(regrx, regry[%zu], color=r'%s', alpha=0.3, label=r'%s "
                    "regression')\n",
                    grp_idx, color, als[grp_idx].group->name, //
                    grp_idx, color, als[grp_idx].group->name  //
            );
        }
    } else {
        size_t grp_idx = als[0].grp_idx;
        const char *color = mpl_nth_color(grp_idx);
        fprintf(
            f,
            "plt.plot(x, y[%zu], '.-', color=r'%s', label=r'%s')\n"
            "plt.plot(regrx, regry[%zu], color='%s', alpha=0.3, label=r'%s regression')\n",
            (size_t)0, color, als[0].group->name, //
            (size_t)0, color, als[0].group->name  //
        );
    }
    if (view->logscale)
        fprintf(f, "plt.yscale('log')\n");
    fprintf(f, "plt.legend(loc='best')\n");
    fprintf(f,
            "plt.xticks(x)\n"
            "plt.grid()\n"
            "plt.xlabel(r'%s')\n"
            "plt.ylabel(r'%s [%s]')\n"
            "plt.savefig(r'%s', bbox_inches='tight')\n",
            param->name,                           //
            plot->al->meas->name, view->units_str, //
            ctx->image_filename                    //
    );
}

static void make_kde_small_plot_mpl(const struct kde_plot *plot, struct plot_maker_ctx *ctx)
{
    assert(plot->is_small);
    const struct kde_data *kde = &plot->kde;
    double min = kde->min;
    double step = kde->step;
    size_t point_count = kde->point_count;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
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
            "plt.xlabel(r'%s [%s]')\n"
            "plt.ylabel('probability density')\n"
            "plt.savefig(r'%s', bbox_inches='tight')\n",
            kde->mean_x * view->multiplier, kde->mean_y, //
            plot->meas->name, view->units_str,           //
            ctx->image_filename                          //
    );
}

static void make_kde_plot_mpl(const struct kde_plot *plot, struct plot_maker_ctx *ctx)
{
    assert(!plot->is_small);
    const struct kde_data *kde = &plot->kde;
    const struct plot_view *view = &plot->view;
    const struct distr *distr = plot->distr;
    double min = kde->min;
    double max = kde->max;
    double step = kde->step;
    size_t point_count = kde->point_count;
    FILE *f = ctx->f;
    fprintf(f, "points = [");
    for (size_t i = 0; i < distr->count; ++i) {
        double v = distr->data[i];
        if (v < min || v > max)
            continue;
        fprintf(f, "(%g,%g), ", v * view->multiplier,
                (double)(i + 1) / distr->count * plot->max_y);
    }
    fprintf(f, "]\n");
    fprintf(f, "severe_points = list(filter(lambda x: x[0] < %g or x[0] > %g, points))\n",
            distr->outliers.low_severe_x * view->multiplier,
            distr->outliers.high_severe_x * view->multiplier);
    fprintf(f,
            "mild_points = list(filter(lambda x: (%g < x[0] < %g) or (%g < x[0] < %f), "
            "points))\n",
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
        fprintf(f, "%g,", x * view->multiplier);
    }
    fprintf(f, "]\n");
    fprintf(f, "y = [");
    for (size_t i = 0; i < kde_count; ++i)
        fprintf(f, "%g,", kde->data[i]);
    fprintf(f, "]\n");
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "plt.fill_between(x, y, interpolate=True, alpha=0.25, label='PDF')\n"
            "plt.axvline(x=%f, label='mean')\n"
            "plt.plot(*zip(*reg_points), marker='o', ls='', markersize=2, label='\"clean\" "
            "sample')\n"
            "plt.plot(*zip(*mild_points), marker='o', ls='', markersize=2, color='orange',  "
            "label='mild outliers')\n"
            "plt.plot(*zip(*severe_points), marker='o', ls='', markersize=2, color='red', "
            "label='severe outliers')\n",
            plot->kde.mean_x * view->multiplier);
    if (distr->outliers.low_mild_x > min && distr->outliers.low_mild != 0)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                distr->outliers.low_mild_x * view->multiplier);
    if (distr->outliers.low_severe_x > min && distr->outliers.low_severe != 0)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                distr->outliers.low_severe_x * view->multiplier);
    if (distr->outliers.high_mild_x < max && distr->outliers.high_mild != 0)
        fprintf(f, "plt.axvline(x=%g, color='orange')\n",
                distr->outliers.high_mild_x * view->multiplier);
    if (distr->outliers.high_severe_x < max && distr->outliers.high_severe != 0)
        fprintf(f, "plt.axvline(x=%g, color='red')\n",
                distr->outliers.high_severe_x * view->multiplier);
    fprintf(f,
            "plt.tick_params(left=False, labelleft=False)\n"
            "plt.xlabel(r'%s [%s]')\n"
            "plt.ylabel('probability density, runs')\n"
            "plt.legend(loc='upper right')\n"
            "plt.title(r'%s')\n"
            "figure = plt.gcf()\n"
            "figure.set_size_inches(13, 9)\n"
            "plt.savefig(r'%s', dpi=100, bbox_inches='tight')\n",
            plot->meas->name, view->units_str, //
            plot->title,                       //
            ctx->image_filename                //
    );
}

static void make_kde_cmp_small_plot_mpl(const struct kde_cmp_plot *plot,
                                        struct plot_maker_ctx *ctx)
{
    assert(plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
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
    const char *a_color = mpl_nth_color(plot->a_idx);
    const char *b_color = mpl_nth_color(plot->b_idx);
    fprintf(
        f,
        "import matplotlib as mpl\n"
        "mpl.use('svg')\n"
        "import matplotlib.pyplot as plt\n"
        "plt.fill_between(x, ay, interpolate=True, alpha=0.25, facecolor='%s', label=r'%s')\n"
        "plt.fill_between(x, by, interpolate=True, alpha=0.25, facecolor='%s', label=r'%s')\n"
        "plt.vlines(%g, [0], [%g], color='%s')\n"
        "plt.vlines(%g, [0], [%g], color='%s')\n"
        "plt.tick_params(left=False, labelleft=False)\n"
        "plt.xlabel(r'%s [%s]')\n"
        "plt.ylabel('probability density')\n"
        "plt.legend(loc='upper right')\n"
        "plt.savefig(r'%s', bbox_inches='tight')\n",
        a_color, plot->a_name,                                    //
        b_color, plot->b_name,                                    //
        a_kde->mean_x * view->multiplier, a_kde->mean_y, a_color, //
        b_kde->mean_x * view->multiplier, b_kde->mean_y, b_color, //
        plot->al->meas->name, view->units_str,                    //
        ctx->image_filename                                       //
    );
}

static void make_kde_cmp_plot_mpl(const struct kde_cmp_plot *plot, struct plot_maker_ctx *ctx)
{
    assert(!plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double max = plot->max;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
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
    const char *a_color = mpl_nth_color(plot->a_idx);
    const char *b_color = mpl_nth_color(plot->b_idx);
    fprintf(
        f,
        "import matplotlib as mpl\n"
        "mpl.use('svg')\n"
        "import matplotlib.pyplot as plt\n"
        "plt.fill_between(x, ay, interpolate=True, alpha=0.25, facecolor='%s', label=r'%s "
        "PDF')\n"
        "plt.plot(*zip(*a_points), marker='o', ls='', markersize=2, color='%s', label=r'%s "
        "sample')\n"
        "plt.axvline(%g, color='%s', label=r'%s mean')\n"
        "plt.fill_between(x, by, interpolate=True, alpha=0.25, facecolor='%s', label=r'%s "
        "PDF')\n"
        "plt.plot(*zip(*b_points), marker='o', ls='', markersize=2, color='%s', label=r'%s "
        "sample')\n"
        "plt.axvline(%g, color='%s', label=r'%s mean')\n"
        "plt.tick_params(left=False, labelleft=False)\n"
        "plt.xlabel(r'%s [%s]')\n"
        "plt.ylabel('probability density, runs')\n"
        "plt.legend(loc='upper right')\n"
        "plt.title(r'%s')\n"
        "figure = plt.gcf()\n"
        "figure.set_size_inches(13, 9)\n"
        "plt.savefig('%s', dpi=100, bbox_inches='tight')\n",
        a_color, plot->a_name,                                   //
        a_color, plot->a_name,                                   //
        a_kde->mean_x * view->multiplier, a_color, plot->a_name, //
        b_color, plot->b_name,                                   //
        b_color, plot->b_name,                                   //
        b_kde->mean_x * view->multiplier, b_color, plot->b_name, //
        plot->al->meas->name, view->units_str,                   //
        plot->title,                                             //
        ctx->image_filename                                      //
    );
}

static void make_kde_cmp_group_plot_mpl(const struct kde_cmp_group_plot *plot,
                                        struct plot_maker_ctx *ctx)
{
    const struct meas_analysis *al = plot->al;
    size_t val_count = plot->val_count;
    size_t point_count = plot->point_count;
    FILE *f = ctx->f;
    fprintf(f, "x = [");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        fprintf(f, "[");
        for (size_t i = 0; i < point_count; ++i)
            fprintf(f, "%g,", (cmp->min + cmp->step * i) * cmp->view.multiplier);
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
    for (size_t val_idx = 0; val_idx < plot->val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        fprintf(f, "r'%s',", cmp->title);
    }
    fprintf(f, "]\n");
    fprintf(f, "xlabels = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "'%s [%s]',", al->meas->name, plot->cmps[i].view.units_str);
    fprintf(f, "]\n");
    fprintf(f, "a_means = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "%g,", plot->cmps[i].a_kde.mean_x * plot->cmps[i].view.multiplier);
    fprintf(f, "]\n");
    fprintf(f, "b_means = [");
    for (size_t i = 0; i < plot->val_count; ++i)
        fprintf(f, "%g,", plot->cmps[i].b_kde.mean_x * plot->cmps[i].view.multiplier);
    fprintf(f, "]\n");
    const char *a_color = mpl_nth_color(plot->ref_idx);
    const char *b_color = mpl_nth_color(plot->grp_idx);
    fprintf(f,
            "def make_plot(x, ay, by, a_mean, b_mean, a_points, b_points, a_name, b_name, "
            "title, xlabel, ax):\n"
            "  ax.fill_between(x, ay, interpolate=True, alpha=0.25, facecolor='%s', "
            "label=a_name)\n"
            "  ax.plot(*zip(*a_points), marker='o', ls='', markersize=2, color='%s')\n"
            "  ax.axvline(a_mean, color='%s')\n"
            "  ax.fill_between(x, by, interpolate=True, alpha=0.25, facecolor='%s', "
            "label=b_name)\n"
            "  ax.plot(*zip(*b_points), marker='o', ls='', markersize=2, color='%s')\n"
            "  ax.axvline(b_mean, color='%s')\n"
            "  ax.tick_params(left=False, labelleft=False)\n"
            "  ax.set_xlabel(xlabel)\n"
            "  ax.set_ylabel('probability density, runs')\n"
            "  ax.legend(loc='upper right')\n"
            "  ax.set_title(title)\n",
            a_color, //
            a_color, //
            a_color, //
            b_color, //
            b_color, //
            b_color  //
    );
    const char *a_name = bench_group_name(al->base, plot->ref_idx);
    const char *b_name = bench_group_name(al->base, plot->grp_idx);
    fprintf(f,
            "import matplotlib as mpl\n"
            "mpl.use('svg')\n"
            "import matplotlib.pyplot as plt\n"
            "fig, axes = plt.subplots(%zu, %zu)\n"
            "if %zu == 1: axes = [axes]\n"
            "row = col = 0\n"
            "for i in range(%zu):\n"
            "  make_plot(x[i], ay[i], by[i], a_means[i], b_means[i], a_points[i], "
            "b_points[i], r'%s', r'%s', titles[i], xlabels[i], axes[row][col])\n"
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
            "plt.savefig(r'%s', dpi=100, bbox_inches='tight')\n",
            plot->rows, plot->cols,         //
            plot->rows,                     //
            plot->val_count,                //
            a_name, b_name,                 //
            plot->cols,                     //
            plot->rows,                     //
            plot->cols,                     //
            plot->cols * 5, plot->rows * 5, //
            ctx->image_filename             //
    );
}

static bool bar_mpl(const struct meas_analysis *al, struct plot_maker_ctx *ctx)
{
    struct bar_plot plot;
    init_bar_plot(al, &plot);
    make_bar_mpl(&plot, ctx);
    return true;
}

static bool group_bar_mpl(const struct meas_analysis *al, struct plot_maker_ctx *ctx)
{
    struct group_bar_plot plot;
    init_group_bar(al, &plot);
    make_group_bar_mpl(&plot, ctx);
    return true;
}

static bool group_regr_mpl(const struct meas_analysis *al, size_t idx,
                           struct plot_maker_ctx *ctx)
{
    struct group_regr_plot plot;
    init_group_regr(al, idx, &plot);
    make_group_regr_mpl(&plot, ctx);
    return true;
}

static bool kde_small_mpl(const struct distr *distr, const struct meas *meas,
                          struct plot_maker_ctx *ctx)
{
    struct kde_plot plot;
    init_kde_small_plot(distr, meas, &plot);
    make_kde_small_plot_mpl(&plot, ctx);
    free_kde_plot(&plot);
    return true;
}

static bool kde_mpl(const struct distr *distr, const struct meas *meas, const char *name,
                    struct plot_maker_ctx *ctx)
{
    struct kde_plot plot;
    init_kde_plot(distr, meas, name, &plot);
    make_kde_plot_mpl(&plot, ctx);
    free_kde_plot(&plot);
    return true;
}

static bool kde_cmp_small_mpl(const struct meas_analysis *al, size_t bench_idx,
                              struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_small_plot(al, bench_idx, &plot);
    make_kde_cmp_small_plot_mpl(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return true;
}

static bool kde_cmp_mpl(const struct meas_analysis *al, size_t bench_idx,
                        struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_plot(al, bench_idx, &plot);
    make_kde_cmp_plot_mpl(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return true;
}

static bool kde_cmp_per_val_small_mpl(const struct meas_analysis *al, size_t grp_idx,
                                      size_t val_idx, struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_per_val_small_plot(al, grp_idx, val_idx, &plot);
    make_kde_cmp_small_plot_mpl(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return true;
}

static bool kde_cmp_per_val_mpl(const struct meas_analysis *al, size_t grp_idx,
                                size_t val_idx, struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_per_val_plot(al, grp_idx, val_idx, &plot);
    make_kde_cmp_plot_mpl(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return true;
}

static bool kde_cmp_group_mpl(const struct meas_analysis *al, size_t grp_idx,
                              struct plot_maker_ctx *ctx)
{
    struct kde_cmp_group_plot plot;
    init_kde_cmp_group_plot(al, grp_idx, &plot);
    make_kde_cmp_group_plot_mpl(&plot, ctx);
    free_kde_cmp_group_plot(&plot);
    return true;
}

static void define_gnuplot_linetypes(double point_size, FILE *f)
{
    // These are the tableau colors from matplotlib
    fprintf(f,
            "set style line 1 lc rgb '#1f77b4' pt 7 ps %f\n"
            "set style line 2 lc rgb '#ff7f0e' pt 7 ps %f\n"
            "set style line 3 lc rgb '#2ca02c' pt 7 ps %f\n"
            "set style line 4 lc rgb '#d62728' pt 7 ps %f\n"
            "set style line 5 lc rgb '#9467bd' pt 7 ps %f\n"
            "set style line 6 lc rgb '#8c564b' pt 7 ps %f\n"
            "set style line 7 lc rgb '#e377c2' pt 7 ps %f\n"
            "set style line 8 lc rgb '#7f7f7f' pt 7 ps %f\n"
            "set style line 9 lc rgb '#bcbd22' pt 7 ps %f\n"
            "set style line 10 lc rgb '#17becf' pt 7 ps %f\n",
            point_size, point_size, point_size, point_size, point_size, point_size,
            point_size, point_size, point_size, point_size);
}

static bool make_bar_gnuplot(const struct bar_plot *plot, struct plot_maker_ctx *ctx)
{
    const struct meas_analysis *al = plot->al;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    const char *dat_name = NULL;
    {
        FILE *dat = gnuplot_data_file(ctx, &dat_name);
        if (dat == NULL)
            return false;
        foreach_bench_idx (bench_idx, al) {
            fprintf(dat, "\"%s\"\t%g\t%g\n", bench_name(al->base, bench_idx),
                    al->benches[bench_idx]->mean.point * view->multiplier,
                    al->benches[bench_idx]->st_dev.point * view->multiplier);
        }
        fclose(dat);
    }
    define_gnuplot_linetypes(0.25, f);
    fprintf(f,
            "set term svg enhanced background rgb 'white'\n"
            "set output '%s'\n"
            "set boxwidth 1\n"
            "set style fill solid 0.6 border\n"
            "set style histogram errorbars gap 2 lw 1\n"
            "set style data histograms\n"
            "set bars front\n"
            "set grid ytics\n"
            "set offset 0, 0, graph 0.05, 0\n"
            "set ylabel '%s [%s]'\n"
            "set yrange [0:*]\n",
            ctx->image_filename, //
            al->meas->name,      //
            view->units_str      //
    );
    if (view->logscale)
        fprintf(f, "set logscale y\n");
    fprintf(f, "plot '%s' using 2:3:xtic(1) ls 1 notitle\n", dat_name);
    return true;
}

static bool make_group_bar_gnuplot(const struct group_bar_plot *plot,
                                   struct plot_maker_ctx *ctx)
{
    const struct meas_analysis *al = plot->al;
    const struct analysis *base = al->base;
    const struct plot_view *view = &plot->view;
    const struct bench_param *param = base->param;
    size_t val_count = param->value_count;
    FILE *f = ctx->f;
    const char *dat_name = NULL;
    {
        FILE *dat = gnuplot_data_file(ctx, &dat_name);
        if (dat == NULL)
            return false;
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
            fprintf(dat, "\"%s\"", param->values[val_idx]);
            foreach_group_by_avg_idx (grp_idx, al) {
                fprintf(dat, "\t%g\t%g",
                        al->group_analyses[grp_idx].data[val_idx].mean * view->multiplier,
                        al->group_analyses[grp_idx].data[val_idx].distr->st_dev.point *
                            view->multiplier);
            }
            fprintf(dat, "\n");
        }
        fclose(dat);
    }
    define_gnuplot_linetypes(0.25, f);
    fprintf(f,
            "set term svg enhanced background rgb 'white'\n"
            "set output '%s'\n"
            "set boxwidth 1\n"
            "set style fill solid 0.6 border\n"
            "set style histogram errorbars gap 2 lw 1\n"
            "set style data histograms\n"
            "set bars front\n"
            "set grid ytics\n"
            "set offset 0, 0, graph 0.05, 0\n"
            "set xlabel '%s'\n"
            "set ylabel '%s [%s]'\n"
            "set yrange [0:*]\n",
            ctx->image_filename,            //
            param->name,                    //
            al->meas->name, view->units_str //
    );
    if (view->logscale)
        fprintf(f, "set logscale y\n");
    fprintf(f, "plot '%s' using 2:3:xtic(1) ls 1 title '%s'", dat_name,
            bench_group_name(base, ith_group_by_avg_idx(0, al)));
    size_t i = 0;
    foreach_group_by_avg_idx (grp_idx, al) {
        if (i++ == 0)
            continue;
        fprintf(f, ",\\\n\t'' using %zu:%zu ls %zu title '%s'", i * 2, 1 + i * 2, i,
                bench_group_name(base, grp_idx));
    }
    fprintf(f, "\n");
    return true;
}

static bool make_group_regr_gnuplot(const struct group_regr_plot *plot,
                                    struct plot_maker_ctx *ctx)
{
    const struct plot_view *view = &plot->view;
    const struct group_analysis *als = plot->als;
    const struct bench_param *param = plot->al->base->param;
    FILE *f = ctx->f;
    const char *dat1_name = NULL, *dat2_name = NULL;
    {
        FILE *dat1 = gnuplot_data_file(ctx, &dat1_name);
        if (dat1 == NULL)
            return false;
        for (size_t val_idx = 0; val_idx < param->value_count; ++val_idx) {
            fprintf(dat1, "%g", als[0].data[val_idx].value_double);
            if (plot->count != 1) {
                foreach_group_by_avg_idx (grp_idx, plot->al)
                    fprintf(dat1, "\t%g", als[grp_idx].data[val_idx].mean * view->multiplier);
            } else {
                fprintf(dat1, "\t%g", als[0].data[val_idx].mean * view->multiplier);
            }
            fprintf(dat1, "\n");
        }
        fclose(dat1);
    }
    {
        FILE *dat2 = gnuplot_data_file(ctx, &dat2_name);
        if (dat2 == NULL)
            return false;
        for (size_t i = 0; i < plot->nregr + 1; ++i) {
            fprintf(dat2, "%g", plot->lowest_x + plot->regr_x_step * i);
            if (plot->count != 1) {
                foreach_group_by_avg_idx (grp_idx, plot->al) {
                    double regr = ols_approx(&als[grp_idx].regress,
                                             plot->lowest_x + plot->regr_x_step * i);
                    fprintf(dat2, "\t%g", regr * view->multiplier);
                }
            } else {
                double regr =
                    ols_approx(&als[0].regress, plot->lowest_x + plot->regr_x_step * i);
                fprintf(dat2, "\t%g", regr * view->multiplier);
            }
            fprintf(dat2, "\n");
        }
        fclose(dat2);
    }
    fprintf(f, "set xtics (");
    for (size_t i = 0; i < param->value_count; ++i) {
        fprintf(f, "%s", param->values[i]);
        if (i != param->value_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, ")\n");
    define_gnuplot_linetypes(0.5, f);
    fprintf(f,
            "set term svg enhanced background rgb 'white'\n"
            "set output '%s'\n"
            "set xlabel '%s'\n"
            "set ylabel '%s [%s]'\n"
            "set grid\n"
            "set offset graph 0.1, graph 0.1, graph 0.1, graph 0.1\n",
            ctx->image_filename,                  //
            param->name,                          //
            plot->al->meas->name, view->units_str //
    );
    if (view->logscale)
        fprintf(f, "set logscale y\n");
    fprintf(f,
            "plot '%s' using 1:2 with linespoints title '%s' ls 1, \\\n"
            "\t'%s' using 1:2 with lines title '%s regression' ls 2",
            dat1_name, als[0].group->name, //
            dat2_name, als[0].group->name  //
    );
    size_t i = 0;
    if (plot->count != 1) {
        foreach_group_by_avg_idx (grp_idx, plot->al) {
            if (i++ == 0)
                continue;
            fprintf(f,
                    ",\\\n\t'%s' using 1:%zu with linespoints ls %zu title '%s',\\\n"
                    "\t'%s' using 1:%zu with lines ls %zu title '%s regression'",
                    dat1_name, 1 + i, 1 + (i * 2), als[grp_idx].group->name, //
                    dat2_name, 1 + i, 2 + (i * 2), als[grp_idx].group->name  //
            );
        }
    }
    fprintf(f, "\n");
    return true;
}

static bool make_kde_small_plot_gnuplot(const struct kde_plot *plot,
                                        struct plot_maker_ctx *ctx)
{
    assert(plot->is_small);
    const struct kde_data *kde = &plot->kde;
    double min = kde->min;
    double step = kde->step;
    size_t point_count = kde->point_count;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    const char *dat_name = NULL;
    {
        FILE *dat = gnuplot_data_file(ctx, &dat_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < point_count; ++i)
            fprintf(dat, "%g\t%g\n", (min + step * i) * view->multiplier, kde->data[i]);
        fclose(dat);
    }
    define_gnuplot_linetypes(0.25, f);
    fprintf(f,
            "set term svg enhanced background rgb 'white'\n"
            "set output '%s'\n"
            "set ylabel 'probability density'\n"
            "set xlabel '%s [%s]'\n"
            "set style fill solid 0.25 noborder\n"
            "unset ytics\n"
            "set xrange [%g:%g]\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls 1\n"
            "set offset 0, 0, graph 0.1, 0\n"
            "plot '%s' using 1:2 with filledcurves above y1=0 notitle ls 1\n",
            ctx->image_filename,                                            //
            plot->meas->name, view->units_str,                              //
            min * view->multiplier, kde->max * view->multiplier,            //
            kde->mean_x * view->multiplier, kde->mean_x * view->multiplier, //
            dat_name                                                        //
    );
    return true;
}

static bool make_kde_plot_gnuplot(struct kde_plot *plot, struct plot_maker_ctx *ctx)
{
    assert(!plot->is_small);
    const struct kde_data *kde = &plot->kde;
    const struct plot_view *view = &plot->view;
    const struct distr *distr = plot->distr;
    double min = kde->min;
    double max = kde->max;
    double step = kde->step;
    size_t point_count = kde->point_count;
    FILE *f = ctx->f;
    const char *kde_name = NULL, *severe_name = NULL, *mild_name = NULL, *reg_name = NULL;
    {
        FILE *kde_dat = gnuplot_data_file(ctx, &kde_name);
        if (kde_dat == NULL)
            return false;
        for (size_t i = 0; i < point_count; ++i)
            fprintf(kde_dat, "%g\t%g\n", (min + step * i) * view->multiplier, kde->data[i]);
        fclose(kde_dat);
    }
    if (plot->displayed_severe_count) {
        FILE *dat = gnuplot_data_file(ctx, &severe_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < distr->count; ++i) {
            double v = distr->data[i];
            if (v < min || v > max)
                continue;
            if (!(v < distr->outliers.low_severe_x || v > distr->outliers.high_severe_x))
                continue;
            fprintf(dat, "%g\t%g\n", v * view->multiplier,
                    (double)(i + 1) / distr->count * plot->max_y);
        }
        fclose(dat);
    }
    if (plot->displayed_mild_count) {
        FILE *dat = gnuplot_data_file(ctx, &mild_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < distr->count; ++i) {
            double v = distr->data[i];
            if (v < min || v > max)
                continue;
            if (!((v > distr->outliers.low_severe_x && v < distr->outliers.low_mild_x) ||
                  (v < distr->outliers.high_severe_x && v > distr->outliers.high_mild_x)))
                continue;
            fprintf(dat, "%g\t%g\n", v * view->multiplier,
                    (double)(i + 1) / distr->count * plot->max_y);
        }
        fclose(dat);
    }
    {
        FILE *reg_dat = gnuplot_data_file(ctx, &reg_name);
        if (reg_dat == NULL)
            return false;
        for (size_t i = 0; i < distr->count; ++i) {
            double v = distr->data[i];
            if (v < min || v > max)
                continue;
            if (!(v > distr->outliers.low_mild_x && v < distr->outliers.high_mild_x))
                continue;
            fprintf(reg_dat, "%g\t%g\n", v * view->multiplier,
                    (double)(i + 1) / distr->count * plot->max_y);
        }
        fclose(reg_dat);
    }
    define_gnuplot_linetypes(0.25, f);
    fprintf(f,
            "set term svg enhanced background rgb 'white' size 960,720\n"
            "set output '%s'\n"
            "set ylabel 'probability density, runs'\n"
            "set xlabel '%s [%s]'\n"
            "set style fill solid 0.25 noborder\n"
            "unset ytics\n"
            "set offset 0, 0, graph 0.1, 0\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls 1\n"
            "set title '%s'\n"
            "set xrange [%g:%g]\n",
            ctx->image_filename,                                            //
            plot->meas->name, view->units_str,                              //
            kde->mean_x * view->multiplier, kde->mean_x * view->multiplier, //
            plot->title,                                                    //
            min * view->multiplier, max * view->multiplier                  //
    );
    if (distr->outliers.low_mild_x > min && distr->outliers.low_mild != 0)
        fprintf(f, "set arrow from %g, graph 0 to %g, graph 1 nohead ls 2\n",
                distr->outliers.low_mild_x * view->multiplier,
                distr->outliers.low_mild_x * view->multiplier);
    if (distr->outliers.low_severe_x > min && distr->outliers.low_severe != 0)
        fprintf(f, "set arrow from %g, graph 0 to %g, graph 1 nohead ls 3\n",
                distr->outliers.low_severe_x * view->multiplier,
                distr->outliers.low_severe_x * view->multiplier);
    if (distr->outliers.high_mild_x < max && distr->outliers.high_mild != 0)
        fprintf(f, "set arrow from %g, graph 0 to %g, graph 1 nohead ls 2\n",
                distr->outliers.high_mild_x * view->multiplier,
                distr->outliers.high_mild_x * view->multiplier);
    if (distr->outliers.high_severe_x < max && distr->outliers.high_severe != 0)
        fprintf(f, "set arrow from %g, graph 0 to %g, graph 1 nohead ls 3\n",
                distr->outliers.high_severe_x * view->multiplier,
                distr->outliers.high_severe_x * view->multiplier);
    fprintf(f,
            "plot '%s' using 1:2 with filledcurves above y1=0 title 'PDF' ls 1,\\\n"
            "\t1/0 ls 1 t 'mean', \\\n"
            "\t'%s' using 1:2 with points ls 1 title '\"clean\" sample'",
            kde_name, reg_name);
    if (plot->displayed_mild_count)
        fprintf(f, ",\\\n\t'%s' using 1:2 with points ls 2 title 'mild outliers'", mild_name);
    if (plot->displayed_severe_count)
        fprintf(f,
                ",\\\n\t'%s' using 1:2 with points ls 3 title 'severe "
                "outliers'",
                severe_name);
    fprintf(f, "\n");
    return true;
}

static bool make_kde_cmp_small_plot_gnuplot(const struct kde_cmp_plot *plot,
                                            struct plot_maker_ctx *ctx)
{
    assert(plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    const char *dat_name = NULL;
    {
        FILE *dat = gnuplot_data_file(ctx, &dat_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < point_count; ++i)
            fprintf(dat, "%g\t%g\t%g\n", (min + step * i) * view->multiplier, a_kde->data[i],
                    b_kde->data[i]);
        fclose(dat);
    }
    define_gnuplot_linetypes(0.25, f);
    size_t a_color = plot->a_idx % 10 + 1;
    size_t b_color = plot->b_idx % 10 + 1;
    fprintf(f,
            "set term svg enhanced background rgb 'white'\n"
            "set output '%s'\n"
            "set ylabel 'probability density'\n"
            "set xlabel '%s [%s]'\n"
            "set style fill solid 0.25 noborder\n"
            "unset ytics\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set xrange [%g:%g]\n"
            "set offset 0, 0, graph 0.1, 0\n"
            "plot '%s' using 1:2 with filledcurves above y1=0 t '%s' ls %zu,\\\n"
            "\t'' using 1:3 with filledcurves above y1=0 t '%s' ls %zu\n",
            ctx->image_filename,                                                         //
            plot->al->meas->name, view->units_str,                                       //
            a_kde->mean_x * view->multiplier, a_kde->mean_x * view->multiplier, a_color, //
            b_kde->mean_x * view->multiplier, b_kde->mean_x * view->multiplier, b_color, //
            min * view->multiplier, plot->max * view->multiplier,                        //
            dat_name, plot->a_name, a_color,                                             //
            plot->b_name, b_color                                                        //
    );
    return true;
}

static bool make_kde_cmp_plot_gnuplot(const struct kde_cmp_plot *plot,
                                      struct plot_maker_ctx *ctx)
{
    assert(!plot->is_small);
    size_t point_count = plot->point_count;
    double min = plot->min;
    double max = plot->max;
    double step = plot->step;
    const struct kde_data *a_kde = &plot->a_kde;
    const struct kde_data *b_kde = &plot->b_kde;
    const struct plot_view *view = &plot->view;
    FILE *f = ctx->f;
    const char *kde_name = NULL, *pts1_name = NULL, *pts2_name = NULL;
    {
        FILE *dat = gnuplot_data_file(ctx, &kde_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < point_count; ++i)
            fprintf(dat, "%g\t%g\t%g\n", (min + step * i) * view->multiplier, a_kde->data[i],
                    b_kde->data[i]);
        fclose(dat);
    }
    {
        FILE *dat = gnuplot_data_file(ctx, &pts1_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < plot->a->count; ++i) {
            double v = plot->a->data[i];
            if (v < min || v > max)
                continue;
            fprintf(dat, "%g\t%g\n", v * view->multiplier,
                    (double)(i + 1) / plot->a->count * plot->max_y);
        }
        fclose(dat);
    }
    {
        FILE *dat = gnuplot_data_file(ctx, &pts2_name);
        if (dat == NULL)
            return false;
        for (size_t i = 0; i < plot->b->count; ++i) {
            double v = plot->b->data[i];
            if (v < min || v > max)
                continue;
            fprintf(dat, "%g\t%g\n", v * view->multiplier,
                    (double)(i + 1) / plot->b->count * plot->max_y);
        }
        fclose(dat);
    }
    define_gnuplot_linetypes(0.25, f);
    size_t a_color = plot->a_idx % 10 + 1;
    size_t b_color = plot->b_idx % 10 + 1;
    fprintf(f,
            "set term svg enhanced background rgb 'white' size 960,720\n"
            "set output '%s'\n"
            "set ylabel 'probability density, runs'\n"
            "set xlabel '%s [%s]'\n"
            "set style fill solid 0.25 noborder\n"
            "unset ytics\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set arrow from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set xrange [%g:%g]\n"
            "set offset 0, 0, graph 0.1, 0\n"
            "set title '%s'\n"
            "plot '%s' using 1:2 with filledcurves above y1=0 t '%s PDF' ls %zu,\\\n"
            "\t'%s' using 1:2 with points ls %zu t '%s sample', \\\n"
            "\t1/0 ls %zu t '%s mean', \\\n"
            "\t'%s' using 1:3 with filledcurves above y1=0 t '%s PDF' ls %zu,\\\n"
            "\t'%s' using 1:2 with points ls %zu t '%s sample', \\\n"
            "\t1/0 ls %zu t '%s mean'\n",
            ctx->image_filename,                                                         //
            plot->al->meas->name, view->units_str,                                       //
            a_kde->mean_x * view->multiplier, a_kde->mean_x * view->multiplier, a_color, //
            b_kde->mean_x * view->multiplier, b_kde->mean_x * view->multiplier, b_color, //
            min * view->multiplier, max * view->multiplier,                              //
            plot->title,                                                                 //
            kde_name, plot->a_name, a_color,                                             //
            pts1_name, a_color, plot->a_name,                                            //
            a_color, plot->a_name,                                                       //
            kde_name, plot->b_name, b_color,                                             //
            pts2_name, b_color, plot->b_name,                                            //
            b_color, plot->b_name                                                        //
    );
    return true;
}

static bool make_kde_cmp_group_plot_gnuplot(const struct kde_cmp_group_plot *plot,
                                            struct plot_maker_ctx *ctx)
{
    bool success = false;
    const struct meas_analysis *al = plot->al;
    size_t val_count = plot->val_count;
    size_t point_count = plot->point_count;
    FILE *f = ctx->f;
    const char **kde_names = NULL, **pts1_names = NULL, **pts2_names = NULL;
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        const struct plot_view *view = &cmp->view;
        const struct distr *a = cmp->a;
        const struct distr *b = cmp->b;
        {
            const char *dat_name = NULL;
            FILE *dat = gnuplot_data_file(ctx, &dat_name);
            if (dat == NULL)
                goto err;
            for (size_t i = 0; i < point_count; ++i)
                fprintf(dat, "%g\t%g\t%g\n", (cmp->min + cmp->step * i) * view->multiplier,
                        cmp->a_kde.data[i], cmp->b_kde.data[i]);
            fclose(dat);
            sb_push(kde_names, dat_name);
        }
        {
            const char *dat_name = NULL;
            FILE *dat = gnuplot_data_file(ctx, &dat_name);
            if (dat == NULL)
                goto err;
            for (size_t i = 0; i < a->count; ++i) {
                double v = a->data[i];
                if (v < cmp->min || v > cmp->max)
                    continue;
                fprintf(dat, "%g\t%g\n", v * view->multiplier,
                        (double)(i + 1) / a->count * cmp->max_y);
            }
            fclose(dat);
            sb_push(pts1_names, dat_name);
        }
        {
            const char *dat_name = NULL;
            FILE *dat = gnuplot_data_file(ctx, &dat_name);
            if (dat == NULL)
                goto err;
            for (size_t i = 0; i < b->count; ++i) {
                double v = b->data[i];
                if (v < cmp->min || v > cmp->max)
                    continue;
                fprintf(dat, "%g\t%g\n", v * view->multiplier,
                        (double)(i + 1) / b->count * cmp->max_y);
            }
            fclose(dat);
            sb_push(pts2_names, dat_name);
        }
    }
    define_gnuplot_linetypes(0.25, f);
    size_t a_color = plot->ref_idx % 10 + 1;
    size_t b_color = plot->grp_idx % 10 + 1;
    fprintf(f,
            "set term svg enhanced background rgb 'white' size %zu,%zu\n"
            "set output '%s'\n"
            "set multiplot layout %zu,%zu rowsfirst\n",
            plot->cols * 400, plot->rows * 400, ctx->image_filename, plot->rows, plot->cols);
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct kde_cmp_val *cmp = plot->cmps + val_idx;
        const struct plot_view *view = &cmp->view;
        const struct kde_data *a_kde = &cmp->a_kde;
        const struct kde_data *b_kde = &cmp->b_kde;
        fprintf(f,
                "set ylabel 'probability density, runs'\n"
                "set xlabel '%s [%s]'\n"
                "set style fill solid 0.25 noborder\n"
                "unset ytics\n",
                plot->al->meas->name, view->units_str);
        if (val_idx != 0)
            fprintf(f, "unset arrow 6\n"
                       "unset arrow 7\n");
        fprintf(
            f,
            "set arrow 6 from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set arrow 7 from %g, graph 0 to %g, graph 1 nohead ls %zu\n"
            "set xrange [%g:%g]\n"
            "set offset 0, 0, graph 0.1, 0\n"
            "set title '%s'\n"
            "plot '%s' using 1:2 with filledcurves above y1=0 t '%s' ls %zu,\\\n"
            "\t'%s' using 1:2 with points ls %zu notitle, \\\n"
            "\t'%s' using 1:3 with filledcurves above y1=0 t '%s' ls %zu,\\\n"
            "\t'%s' using 1:2 with points ls %zu notitle\n",
            a_kde->mean_x * view->multiplier, a_kde->mean_x * view->multiplier, a_color, //
            b_kde->mean_x * view->multiplier, b_kde->mean_x * view->multiplier, b_color, //
            cmp->min * view->multiplier, cmp->max * view->multiplier,                    //
            cmp->title,                                                                  //
            kde_names[val_idx], bench_group_name(al->base, plot->ref_idx), a_color,      //
            pts1_names[val_idx], a_color,                                                //
            kde_names[val_idx], bench_group_name(al->base, plot->grp_idx), b_color,      //
            pts2_names[val_idx], b_color                                                 //
        );
    }
    fprintf(f, "unset multiplot\n");
    success = true;
err:
    sb_free(kde_names);
    sb_free(pts1_names);
    sb_free(pts2_names);
    return success;
}

static bool bar_gnuplot(const struct meas_analysis *al, struct plot_maker_ctx *ctx)
{
    struct bar_plot plot;
    init_bar_plot(al, &plot);
    return make_bar_gnuplot(&plot, ctx);
}

static bool group_bar_gnuplot(const struct meas_analysis *al, struct plot_maker_ctx *ctx)
{
    struct group_bar_plot plot;
    init_group_bar(al, &plot);
    return make_group_bar_gnuplot(&plot, ctx);
}

static bool group_regr_gnuplot(const struct meas_analysis *al, size_t idx,
                               struct plot_maker_ctx *ctx)
{
    struct group_regr_plot plot;
    init_group_regr(al, idx, &plot);
    return make_group_regr_gnuplot(&plot, ctx);
}

static bool kde_small_gnuplot(const struct distr *distr, const struct meas *meas,
                              struct plot_maker_ctx *ctx)
{
    struct kde_plot plot;
    init_kde_small_plot(distr, meas, &plot);
    bool success = make_kde_small_plot_gnuplot(&plot, ctx);
    free_kde_plot(&plot);
    return success;
}

static bool kde_gnuplot(const struct distr *distr, const struct meas *meas, const char *name,
                        struct plot_maker_ctx *ctx)
{
    struct kde_plot plot;
    init_kde_plot(distr, meas, name, &plot);
    bool success = make_kde_plot_gnuplot(&plot, ctx);
    free_kde_plot(&plot);
    return success;
}

static bool kde_cmp_small_gnuplot(const struct meas_analysis *al, size_t bench_idx,
                                  struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_small_plot(al, bench_idx, &plot);
    bool success = make_kde_cmp_small_plot_gnuplot(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return success;
}

static bool kde_cmp_gnuplot(const struct meas_analysis *al, size_t bench_idx,
                            struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_plot(al, bench_idx, &plot);
    bool success = make_kde_cmp_plot_gnuplot(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return success;
}

static bool kde_cmp_per_val_small_gnuplot(const struct meas_analysis *al, size_t grp_idx,
                                          size_t val_idx, struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_per_val_small_plot(al, grp_idx, val_idx, &plot);
    bool success = make_kde_cmp_small_plot_gnuplot(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return success;
}

static bool kde_cmp_per_val_gnuplot(const struct meas_analysis *al, size_t grp_idx,
                                    size_t val_idx, struct plot_maker_ctx *ctx)
{
    struct kde_cmp_plot plot;
    init_kde_cmp_per_val_plot(al, grp_idx, val_idx, &plot);
    bool success = make_kde_cmp_plot_gnuplot(&plot, ctx);
    free_kde_cmp_plot(&plot);
    return success;
}

static bool kde_cmp_group_gnuplot(const struct meas_analysis *al, size_t grp_idx,
                                  struct plot_maker_ctx *ctx)
{
    struct kde_cmp_group_plot plot;
    init_kde_cmp_group_plot(al, grp_idx, &plot);
    bool success = make_kde_cmp_group_plot_gnuplot(&plot, ctx);
    free_kde_cmp_group_plot(&plot);
    return success;
}

static bool python_found(void)
{
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s --version", g_python_executable);
    return shell_execute(buffer, -1, -1, -1, true);
}

static bool has_python_with_mpl(void)
{
    if (!python_found())
        return false;
    FILE *f;
    pid_t pid;
    if (!shell_launch_stdin_pipe(g_python_executable, &f, -1, -1, &pid))
        return false;
    fprintf(f, "import matplotlib\n");
    fclose(f);
    return process_wait_finished_correctly(pid, true);
}

static bool has_gnuplot(void)
{
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "gnuplot --version");
    return shell_execute(buffer, -1, -1, -1, true);
}

bool get_plot_backend(enum plot_backend *backend)
{
    switch (g_plot_backend_override) {
    case PLOT_BACKEND_DEFAULT:
        break;
    case PLOT_BACKEND_MATPLOTLIB:
        if (!has_python_with_mpl()) {
            error("selected plot backend (matplotlib) is not available");
            return false;
        }
        *backend = PLOT_BACKEND_MATPLOTLIB;
        return true;
    case PLOT_BACKEND_GNUPLOT:
        if (!has_gnuplot()) {
            error("selected plot backend (gnuplot) is not available");
            return false;
        }
        *backend = PLOT_BACKEND_GNUPLOT;
        return true;
    }
    bool found_backend = false;
    if (has_python_with_mpl()) {
        *backend = PLOT_BACKEND_MATPLOTLIB;
        found_backend = true;
    }
    if (!found_backend && has_gnuplot()) {
        *backend = PLOT_BACKEND_GNUPLOT;
        found_backend = true;
    }
    if (!found_backend) {
        error("Failed to find backend to use to make plots. 'matplotlib' "
              "has to be installed for '%s' python executable, or 'gnuplot' "
              "available in PATH",
              g_python_executable);
        return false;
    }
    return true;
}

void init_plot_maker(enum plot_backend backend, struct plot_maker *maker)
{
    memset(maker, 0, sizeof(*maker));
    maker->kind = backend;
    switch (backend) {
    case PLOT_BACKEND_MATPLOTLIB:
        maker->src_extension = "py";
        maker->bar = bar_mpl;
        maker->group_bar = group_bar_mpl;
        maker->group_regr = group_regr_mpl;
        maker->kde_small = kde_small_mpl;
        maker->kde = kde_mpl;
        maker->kde_cmp_small = kde_cmp_small_mpl;
        maker->kde_cmp = kde_cmp_mpl;
        maker->kde_cmp_group = kde_cmp_group_mpl;
        maker->kde_cmp_per_val_small = kde_cmp_per_val_small_mpl;
        maker->kde_cmp_per_val = kde_cmp_per_val_mpl;
        break;
    case PLOT_BACKEND_GNUPLOT:
        maker->src_extension = "gp";
        maker->bar = bar_gnuplot;
        maker->group_bar = group_bar_gnuplot;
        maker->group_regr = group_regr_gnuplot;
        maker->kde_small = kde_small_gnuplot;
        maker->kde = kde_gnuplot;
        maker->kde_cmp_small = kde_cmp_small_gnuplot;
        maker->kde_cmp = kde_cmp_gnuplot;
        maker->kde_cmp_group = kde_cmp_group_gnuplot;
        maker->kde_cmp_per_val_small = kde_cmp_per_val_small_gnuplot;
        maker->kde_cmp_per_val = kde_cmp_per_val_gnuplot;
        break;
    case PLOT_BACKEND_DEFAULT:
        ASSERT_UNREACHABLE();
        break;
    }
}
