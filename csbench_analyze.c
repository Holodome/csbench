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
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct val_bench_sort_state {
    size_t val_idx;
    const struct group_analysis *group_analyses;
};

struct analyze_task {
    struct analyze_task_queue *q;
    struct bench_analysis *al;
};

struct analyze_task_queue {
    size_t task_count;
    struct analyze_task *tasks;
    volatile size_t cursor;
};

struct accum_idx {
    double accum;
    size_t idx;
};

static void init_analyze_task_queue(struct bench_analysis *als, size_t count,
                                    struct analyze_task_queue *q)
{
    memset(q, 0, sizeof(*q));
    q->task_count = count;
    q->tasks = calloc(count, sizeof(*q->tasks));
    for (size_t i = 0; i < count; ++i) {
        q->tasks[i].q = q;
        q->tasks[i].al = als + i;
    }
}

static void free_analyze_task_queue(struct analyze_task_queue *q)
{
    free(q->tasks);
}

static struct analyze_task *get_analyze_task(struct analyze_task_queue *q)
{
    size_t idx = atomic_fetch_inc(&q->cursor);
    if (idx >= q->task_count)
        return NULL;
    return q->tasks + idx;
}

static int compare_doubles(const void *a, const void *b)
{
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2)
        return -1;
    if (arg1 > arg2)
        return 1;
    return 0;
}

static uint32_t random_bounded(uint32_t range, uint64_t *entropy)
{
    // https://lemire.me/blog/2016/06/30/fast-random-shuffling/
    uint64_t random32bit = (uint64_t)pcg32_fast(entropy);
    uint64_t multiresult = random32bit * range;
    return multiresult >> 32;
}

static void resample(const double *src, size_t count, double *dst)
{
    uint64_t entropy = pcg32_fast(&g_rng_state);
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[random_bounded(count, &entropy)];

    g_rng_state = entropy;
}

#define fitting_curve_1(...) (1.0)
#define fitting_curve_n(_n) (_n)
#define fitting_curve_n_sq(_n) ((_n) * (_n))
#define fitting_curve_n_cube(_n) ((_n) * (_n) * (_n))
#define fitting_curve_logn(_n) log2(_n)
#define fitting_curve_nlogn(_n) ((_n) * log2(_n))

#define ols(_name, _fitting)                                                                \
    static double ols_##_name(const double *x, const double *y, size_t count,               \
                              double adjust_y, double *rmsp)                                \
    {                                                                                       \
        (void)x;                                                                            \
        double sigma_gn_sq = 0.0;                                                           \
        double sigma_t = 0.0;                                                               \
        double sigma_t_gn = 0.0;                                                            \
        for (size_t i = 0; i < count; ++i) {                                                \
            double gn_i = _fitting(x[i] - x[0]);                                            \
            sigma_gn_sq += gn_i * gn_i;                                                     \
            sigma_t += y[i] - adjust_y;                                                     \
            sigma_t_gn += (y[i] - adjust_y) * gn_i;                                         \
        }                                                                                   \
        double coef = sigma_t_gn / sigma_gn_sq;                                             \
        double rms = 0.0;                                                                   \
        for (size_t i = 0; i < count; ++i) {                                                \
            double fit = coef * _fitting(x[i] - x[0]);                                      \
            double a = (y[i] - adjust_y) - fit;                                             \
            rms += a * a;                                                                   \
        }                                                                                   \
        double mean = sigma_t / count;                                                      \
        *rmsp = sqrt(rms / count) / mean;                                                   \
        return coef;                                                                        \
    }

ols(1, fitting_curve_1) ols(n, fitting_curve_n) ols(n_sq, fitting_curve_n_sq)
    ols(n_cube, fitting_curve_n_cube) ols(logn, fitting_curve_logn)
        ols(nlogn, fitting_curve_nlogn)

#undef ols

            double ols_approx(const struct ols_regress *regress, double n)
{
    double f = 1.0;
    n -= regress->c;
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

static void set_ols_rvalue(const double *xp, const double *yp, size_t count,
                           struct ols_regress *result)
{
    double x_mean = 0.0;
    double y_mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
        x_mean += xp[i];
        y_mean += yp[i];
    }
    x_mean /= count;
    y_mean /= count;

    double xm = 0.0;
    double xym = 0.0;
    double ym = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double x = xp[i];
        double y = yp[i];

        xm += (x - x_mean) * (x - x_mean);
        ym += (y - y_mean) * (y - y_mean);
        xym += (x - x_mean) * (y - y_mean);
    }
    xm /= count;
    xym /= count;
    ym /= count;

    double r = 0.0;
    if (xm == 0.0 || ym == 0.0) {
    } else {
        r = xym / sqrt(xm * ym);
        if (r > 1.0)
            r = 1.0;
        else if (r < -1.0)
            r = -1.0;
    }
    result->r = r;
    result->r2 = r * r;
}

static void ols(const double *x, const double *y, size_t count, struct ols_regress *result)
{
    double min_y = INFINITY;
    for (size_t i = 0; i < count; ++i)
        if (y[i] < min_y)
            min_y = y[i];

    enum big_o best_fit = O_1;
    double best_fit_coef, best_fit_rms;
    best_fit_coef = ols_1(x, y, count, min_y, &best_fit_rms);

#define check(_name, _e)                                                                    \
    do {                                                                                    \
        double coef, rms;                                                                   \
        coef = _name(x, y, count, min_y, &rms);                                             \
        if (rms < best_fit_rms) {                                                           \
            best_fit = _e;                                                                  \
            best_fit_coef = coef;                                                           \
            best_fit_rms = rms;                                                             \
        }                                                                                   \
    } while (0)

    check(ols_n, O_N);
    check(ols_n_sq, O_N_SQ);
    check(ols_n_cube, O_N_CUBE);
    check(ols_logn, O_LOGN);
    check(ols_nlogn, O_NLOGN);

#undef check

    result->a = best_fit_coef;
    result->b = min_y;
    result->c = x[0];
    result->rms = best_fit_rms;
    result->complexity = best_fit;
    set_ols_rvalue(x, y, count, result);
}

static double t_statistic(const double *a, size_t n1, const double *b, size_t n2)
{
    double a_mean = 0;
    for (size_t i = 0; i < n1; ++i)
        a_mean += a[i];
    a_mean /= n1;
    double b_mean = 0;
    for (size_t i = 0; i < n2; ++i)
        b_mean += b[i];
    b_mean /= n2;

    double a_s2 = 0;
    for (size_t i = 0; i < n1; ++i) {
        double v = (a[i] - a_mean);
        a_s2 += v * v;
    }
    a_s2 /= (n1 - 1);
    double b_s2 = 0;
    for (size_t i = 0; i < n2; ++i) {
        double v = (b[i] - b_mean);
        b_s2 += v * v;
    }
    b_s2 /= (n2 - 1);

    double t = (a_mean - b_mean) / sqrt(a_s2 / n1 + b_s2 / n2);
    return t;
}

static double ttest(const double *a, size_t n1, const double *b, size_t n2, size_t nresamp)
{
    // This uses algorithm described in
    // https://en.wikipedia.org/wiki/Bootstrapping_(statistics)#Bootstrap_hypothesis_testing
    double t = t_statistic(a, n1, b, n2);
    double a_mean = 0, b_mean = 0, z_mean = 0;
    for (size_t i = 0; i < n1; ++i) {
        a_mean += a[i];
        z_mean += a[i];
    }
    for (size_t i = 0; i < n2; ++i) {
        b_mean += b[i];
        z_mean += b[i];
    }
    a_mean /= n1;
    b_mean /= n2;
    z_mean /= n1 + n2;
    double *a_new_sample = calloc(n1, sizeof(*a_new_sample));
    double *b_new_sample = calloc(n2, sizeof(*b_new_sample));
    for (size_t i = 0; i < n1; ++i)
        a_new_sample[i] = a[i] - a_mean + z_mean;
    for (size_t i = 0; i < n2; ++i)
        b_new_sample[i] = b[i] - b_mean + z_mean;

    double *a_tmp = calloc(n1, sizeof(*a_tmp));
    double *b_tmp = calloc(n2, sizeof(*b_tmp));

    size_t count = 0;
    for (size_t i = 0; i < nresamp; ++i) {
        resample(a_new_sample, n1, a_tmp);
        resample(b_new_sample, n2, b_tmp);
        double t_resampled = t_statistic(a_tmp, n1, b_tmp, n2);
        if (fabs(t_resampled) >= fabs(t))
            ++count;
    }
    double p = (double)count / nresamp;
    free(b_tmp);
    free(a_tmp);
    free(b_new_sample);
    free(a_new_sample);
    return p;
}

static double mwu(const double *a, size_t n1, const double *b, size_t n2)
{
    double *sorted_a = calloc(n1, sizeof(*sorted_a));
    double *sorted_b = calloc(n2, sizeof(*sorted_b));
    for (size_t i = 0; i < n1; ++i)
        sorted_a[i] = a[i];
    for (size_t i = 0; i < n2; ++i)
        sorted_b[i] = b[i];
    qsort(sorted_a, n1, sizeof(*sorted_a), compare_doubles);
    qsort(sorted_b, n2, sizeof(*sorted_b), compare_doubles);

    size_t *a_ranks = calloc(n1, sizeof(*a_ranks));
    size_t *b_ranks = calloc(n2, sizeof(*b_ranks));
    for (size_t rank = 1, a_cursor = 0, b_cursor = 0; a_cursor != n1 || b_cursor != n2;
         ++rank) {
        if (a_cursor == n1)
            b_ranks[b_cursor++] = rank;
        else if (b_cursor == n2)
            a_ranks[a_cursor++] = rank;
        else if (sorted_a[a_cursor] < sorted_b[b_cursor])
            a_ranks[a_cursor++] = rank;
        else
            b_ranks[b_cursor++] = rank;
    }
    size_t r1 = 0;
    for (size_t i = 0; i < n1; ++i)
        r1 += a_ranks[i];

    double u1 = r1 - n1 * (n1 + 1) / 2.0;
    double u2 = n1 * n2 - u1;
    double u = fmax(u1, u2);

    double mu = n1 * n2 / 2.0;
    double sigma_u = sqrt((n1 * n2 * (n1 + n2 + 1)) / 12.0);

    double z = (u - mu - 0.5) / sigma_u;
    double p = 1.0 - 0.5 * erfc(-z / M_SQRT2);
    p *= 2.0;
    if (p < 0.0)
        p = 0.0;
    else if (p > 1.0)
        p = 1.0;

    free(a_ranks);
    free(b_ranks);
    free(sorted_a);
    free(sorted_b);
    return p;
}

static double c_max(double x, double u_a, double a, double sigma_b_2, double sigma_g_2)
{
    double k = u_a - x;
    double d = k * k;
    double ad = a * d;
    double k1 = sigma_b_2 - a * sigma_g_2 + ad;
    double k0 = -a * ad;
    double det = k1 * k1 - 4 * sigma_g_2 * k0;
    return floor(-2.0 * k0 / (k1 + sqrt(det)));
}

static double var_out(double c, double a, double sigma_b_2, double sigma_g_2)
{
    double ac = a - c;
    return (ac / a) * (sigma_b_2 - ac * sigma_g_2);
}

static double outlier_variance(double mean, double st_dev, double a)
{
    double sigma_b = st_dev;
    double u_a = mean / a;
    double u_g_min = u_a / 2.0;
    double sigma_g = fmin(u_g_min / 4.0, sigma_b / sqrt(a));
    double sigma_g_2 = sigma_g * sigma_g;
    double sigma_b_2 = sigma_b * sigma_b;
    double var_out_min = fmin(var_out(1, a, sigma_b_2, sigma_g_2),
                              var_out(fmin(c_max(0, u_a, a, sigma_b_2, sigma_g_2),
                                           c_max(u_g_min, u_a, a, sigma_b_2, sigma_g_2)),
                                      a, sigma_b_2, sigma_g_2)) /
                         sigma_b_2;
    return var_out_min;
}

static void classify_outliers(struct distr *distr)
{
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
    outliers->var = outlier_variance(distr->mean.point, distr->st_dev.point, distr->count);
}

static void bootstrap_mean_st_dev(const double *src, size_t count, double *tmp,
                                  size_t nresamp, struct est *meane, struct est *st_deve)
{
    double *tmp_means = malloc(sizeof(*tmp) * nresamp * 2);
    double *tmp_rss = tmp_means + nresamp;
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
    st_deve->point = sqrt(rss / (count - 1));
    for (size_t sample = 0; sample < nresamp; ++sample) {
        resample(src, count, tmp);
        sum = 0;
        for (size_t i = 0; i < count; ++i)
            sum += tmp[i];
        mean = sum / count;
        tmp_means[sample] = mean;
        rss = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double a = tmp[i] - mean;
            rss += a * a;
        }
        tmp_rss[sample] = rss;
    }
    qsort(tmp_means, nresamp, sizeof(*tmp_means), compare_doubles);
    qsort(tmp_rss, nresamp, sizeof(*tmp_rss), compare_doubles);
    meane->lower = tmp_means[25 * nresamp / 1000];
    meane->upper = tmp_means[975 * nresamp / 1000];
    st_deve->lower = sqrt(tmp_rss[25 * nresamp / 1000] / (count - 1));
    st_deve->upper = sqrt(tmp_rss[975 * nresamp / 1000] / (count - 1));
    free(tmp_means);
}

static void estimate_distr(const double *data, size_t count, size_t nresamp,
                           struct distr *distr)
{
    double *tmp = malloc(sizeof(*tmp) * count);
    distr->data = data;
    distr->count = count;
    bootstrap_mean_st_dev(data, count, tmp, nresamp, &distr->mean, &distr->st_dev);
    memcpy(tmp, data, count * sizeof(*tmp));
    qsort(tmp, count, sizeof(*tmp), compare_doubles);
    distr->median = tmp[count / 2];
    distr->q1 = tmp[count / 4];
    distr->q3 = tmp[count * 3 / 4];
    distr->p1 = tmp[count / 100];
    distr->p5 = tmp[count * 5 / 100];
    distr->p95 = tmp[count * 95 / 100];
    distr->p99 = tmp[count * 99 / 100];
    distr->min = tmp[0];
    distr->max = tmp[count - 1];
    classify_outliers(distr);
    free(tmp);
}

static cssort_compar(bench_sort_cmp)
{
    const struct meas_analysis *al = statep;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    assert(a_idx != b_idx);
    double va = al->benches[a_idx]->mean.point;
    double vb = al->benches[b_idx]->mean.point;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static void compare_benches(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t bench_count = base->bench_count;
    if (bench_count == 1)
        return;

    // Initialize array with indices before sorting
    for (size_t i = 0; i < bench_count; ++i)
        al->bench_by_mean_time[i] = i;
    cssort_ext(al->bench_by_mean_time, bench_count, sizeof(size_t), bench_sort_cmp, al);
}

static void analyze_group(struct meas_analysis *al, const struct bench_group *grp,
                          struct group_analysis *grp_al)
{
    const struct bench_param *param = al->base->param;

    memset(grp_al, 0, sizeof(*grp_al));
    grp_al->group = grp;
    grp_al->data = calloc(param->value_count, sizeof(*grp_al->data));

    // Fill grp->data, settings slowest and fastest entries
    bool values_are_doubles = true;
    double slowest = -INFINITY, fastest = INFINITY;
    for (size_t cmd_idx = 0; cmd_idx < param->value_count; ++cmd_idx) {
        const char *value = param->values[cmd_idx];
        size_t bench_idx = grp->bench_idxs[cmd_idx];
        char *end = NULL;
        // Check if value is a double.
        double value_double = strtod(value, &end);
        if (end == value)
            values_are_doubles = false;
        double mean = al->benches[bench_idx]->mean.point;
        struct cmd_in_group_data *data = grp_al->data + cmd_idx;
        data->distr = al->benches[bench_idx];
        data->mean = mean;
        data->value = value;
        data->value_double = value_double;
        if (mean > slowest) {
            slowest = mean;
            grp_al->slowest = data;
        }
        if (mean < fastest) {
            fastest = mean;
            grp_al->fastest = data;
        }
    }

    // If values are doubles (they don't have to be because we store them as
    // just strings), and --regr flag is passed, do linear regression.
    grp_al->values_are_doubles = values_are_doubles;
    if (values_are_doubles && g_regr) {
        double *x = calloc(param->value_count, sizeof(*x));
        double *y = calloc(param->value_count, sizeof(*y));
        for (size_t i = 0; i < param->value_count; ++i) {
            x[i] = grp_al->data[i].value_double;
            y[i] = grp_al->data[i].mean;
        }
        ols(x, y, param->value_count, &grp_al->regress);
        free(x);
        free(y);
    }
}

static void analyze_groups(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count == 0)
        return;

    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        const struct bench_group *grp = base->groups + grp_idx;
        struct group_analysis *grp_al = al->group_analyses + grp_idx;
        analyze_group(al, grp, grp_al);
        grp_al->grp_idx = grp_idx;
    }
}

static cssort_compar(val_bench_sort_cmp)
{
    const struct val_bench_sort_state *state = statep;
    size_t val_idx = state->val_idx;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    assert(a_idx != b_idx);
    double va = state->group_analyses[a_idx].data[val_idx].mean;
    double vb = state->group_analyses[b_idx].data[val_idx].mean;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static void compare_per_val(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count == 0)
        return;

    const struct bench_param *param = base->param;
    for (size_t val_idx = 0; val_idx < param->value_count; ++val_idx) {
        // Initialize array with indices before sorting
        struct val_bench_sort_state state = {0};
        state.val_idx = val_idx;
        state.group_analyses = al->group_analyses;
        // Initialize array with indexes for sorting
        for (size_t i = 0; i < base->group_count; ++i)
            al->val_benches_by_mean_time[val_idx][i] = i;
        cssort_ext(al->val_benches_by_mean_time[val_idx], base->group_count, sizeof(size_t),
                   val_bench_sort_cmp, &state);
    }
}

static double p_value(const double *a, size_t n1, const double *b, size_t n2)
{
    double p = 0;
    switch (g_stat_test) {
    case STAT_TEST_MWU:
        p = mwu(a, n1, b, n2);
        break;
    case STAT_TEST_TTEST:
        // Note that we use g_nresamp count here, instead of creating separate
        // configurable parameter.
        p = ttest(a, n1, b, n2, g_nresamp);
        break;
    }
    return p;
}

static void calculate_bench_cmp_p_values(struct meas_analysis *al)
{
    size_t ref_idx = al->bench_cmp.ref;
    const struct distr *ref = al->benches[ref_idx];
    for (size_t bench_idx = 0; bench_idx < al->base->bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        if (ref == distr)
            continue;

        al->bench_cmp.p_values[bench_idx] =
            p_value(ref->data, ref->count, distr->data, distr->count);
    }
}

static void calculate_pval_cmps_p_values(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count <= 1)
        return;

    size_t value_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        size_t ref_idx = al->pval_cmps[val_idx].ref;
        const struct distr *ref = al->group_analyses[ref_idx].data[val_idx].distr;
        for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
            const struct distr *distr = al->group_analyses[grp_idx].data[val_idx].distr;
            if (ref == distr)
                continue;

            al->pval_cmps[val_idx].p_values[grp_idx] =
                p_value(ref->data, ref->count, distr->data, distr->count);
        }
    }
}

static void ref_speed(double u1, double sigma1, double u2, double sigma2, double *ref_u,
                      double *ref_sigma)
{
    // propagate standard deviation for formula (t1 / t2)
    double ref = u1 / u2;
    double a = sigma1 / u1;
    double b = sigma2 / u2;
    double ref_st_dev = ref * sqrt(a * a + b * b);
    *ref_u = ref;
    *ref_sigma = ref_st_dev;
}

static void calculate_ref_speed(double ref_mean, double ref_st_dev, double cur_mean,
                                double cur_st_dev, bool flip, struct point_err_est *est)
{
    if (flip)
        ref_speed(cur_mean, cur_st_dev, ref_mean, ref_st_dev, &est->point, &est->err);
    else
        ref_speed(ref_mean, ref_st_dev, cur_mean, cur_st_dev, &est->point, &est->err);
}
static void calculate_ref_speed_distr(const struct distr *ref, const struct distr *distr,
                                      bool flip, struct point_err_est *est)
{
    calculate_ref_speed(ref->mean.point, ref->st_dev.point, distr->mean.point,
                        distr->st_dev.point, flip, est);
}

static void calculate_speedup(const struct distr *ref, const struct distr *distr, bool flip,
                              struct speedup *sp)
{
    calculate_ref_speed_distr(ref, distr, flip, &sp->est);
    calculate_ref_speed_distr(ref, distr, !flip, &sp->inv_est);
    if (sp->est.point < 1.0)
        sp->is_slower = true;
}

static void calculate_bench_cmp_speedups(struct meas_analysis *al)
{
    size_t bench_count = al->base->bench_count;
    bool flip = false;
    if (g_baseline == -1)
        flip = true;

    size_t ref_idx = al->bench_cmp.ref;
    const struct distr *ref = al->benches[ref_idx];
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        if (distr == ref)
            continue;

        struct speedup *sp = al->bench_cmp.speedups + bench_idx;
        calculate_speedup(ref, distr, flip, sp);
    }
}

static void calculate_pval_cmps_speedups(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    size_t value_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        bool flip = false;
        if (g_baseline == -1)
            flip = true;
        size_t ref_idx = al->pval_cmps[val_idx].ref;
        const struct distr *ref = al->group_analyses[ref_idx].data[val_idx].distr;
        for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
            const struct distr *distr = al->group_analyses[grp_idx].data[val_idx].distr;
            if (distr == ref)
                continue;

            struct speedup *sp = al->pval_cmps[val_idx].speedups + grp_idx;
            calculate_speedup(ref, distr, flip, sp);
        }
    }
}

static void calculate_per_value_ref_speed(const struct meas_analysis *al, size_t ref_idx,
                                          size_t grp_idx, bool flip,
                                          struct point_err_est *dst)
{
    assert(ref_idx != grp_idx);
    struct analysis *base = al->base;
    size_t val_count = base->param->value_count;

    // This uses hand-written error propagation formula for geometric mean,
    // for reference see
    // https://en.wikipedia.org/wiki/Propagation_of_uncertainty
    double mean_accum = 1;
    double st_dev_accum = 0.0;
    double n = val_count;
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct distr *ref = al->group_analyses[ref_idx].data[val_idx].distr;
        const struct distr *distr = al->group_analyses[grp_idx].data[val_idx].distr;

        struct speedup sp;
        memset(&sp, 0, sizeof(sp));
        calculate_speedup(ref, distr, g_baseline == -1 ? true : false, &sp);

        const struct point_err_est *est = NULL;
        if (flip)
            est = &sp.inv_est;
        else
            est = &sp.est;

        mean_accum *= est->point;
        double a = pow(est->point, 1.0 / n - 1.0) * est->err;
        st_dev_accum += a * a;
    }
    dst->point = pow(mean_accum, 1.0 / n);
    dst->err = dst->point / n * sqrt(st_dev_accum);
}

static void calculate_per_value_speedup(const struct meas_analysis *al, size_t ref_idx,
                                        size_t grp_idx, struct speedup *sp)
{
    calculate_per_value_ref_speed(al, ref_idx, grp_idx, false, &sp->est);
    calculate_per_value_ref_speed(al, ref_idx, grp_idx, true, &sp->inv_est);
    if (sp->est.point < 1.0)
        sp->is_slower = true;
}

static void calculate_group_sum_speedup(const struct meas_analysis *al, size_t ref_idx,
                                        size_t grp_idx, struct speedup *sp)
{
    bool flip = false;
    if (g_baseline == -1)
        flip = true;
    const struct point_err_est *ref = al->group_sum_cmp.times + ref_idx;
    const struct point_err_est *cur = al->group_sum_cmp.times + grp_idx;
    calculate_ref_speed(ref->point, ref->err, cur->point, cur->err, flip, &sp->est);
    calculate_ref_speed(ref->point, ref->err, cur->point, cur->err, !flip, &sp->inv_est);
    if (sp->est.point < 1.0)
        sp->is_slower = true;
}

static int accum_idx_sort_cmp(const void *ap, const void *bp)
{
    const struct accum_idx *a = ap;
    const struct accum_idx *b = bp;
    assert(a->idx != b->idx);
    if (a->idx < b->idx)
        return -1;
    if (a->idx > b->idx)
        return 1;
    return 0;
}

static void compare_group_avg(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    if (!base->param || base->group_count < 2)
        return;

    size_t grp_count = base->group_count;
    size_t val_count = base->param->value_count;
    size_t grp_count2 = grp_count * grp_count;
    double *bench_ref_matrix = calloc(grp_count2 * val_count, sizeof(*bench_ref_matrix));

    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        for (size_t i = 0; i < grp_count; ++i) {
            double a = al->group_analyses[i].data[val_idx].distr->mean.point;
            for (size_t j = 0; j < grp_count; ++j) {
                if (i == j)
                    continue;
                double b = al->group_analyses[j].data[val_idx].distr->mean.point;
                size_t idx = val_idx * grp_count2 + i * grp_count + j;
                bench_ref_matrix[idx] = a / b;
            }
        }
    }

    double *group_ref_matrix = calloc(grp_count2, sizeof(*group_ref_matrix));
    for (size_t i = 0; i < grp_count; ++i) {
        for (size_t j = 0; j < grp_count; ++j) {
            if (i == j)
                continue;
            size_t idx = i * grp_count + j;
            double accum = 1.0;
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
                accum *= bench_ref_matrix[val_idx * grp_count2 + idx];
            group_ref_matrix[idx] = accum;
        }
    }

    struct accum_idx *group_total_accum = calloc(grp_count, sizeof(*group_total_accum));
    for (size_t i = 0; i < grp_count; ++i) {
        double accum = 1.0;
        for (size_t j = 0; j < grp_count; ++j) {
            if (i == j)
                continue;
            accum *= group_ref_matrix[i * grp_count + j];
        }

        group_total_accum[i].accum = accum;
        group_total_accum[i].idx = i;
    }

    qsort(group_total_accum, grp_count, sizeof(struct accum_idx), accum_idx_sort_cmp);
    for (size_t i = 0; i < grp_count; ++i)
        al->groups_by_avg_speed[i] = group_total_accum[i].idx;

    free(bench_ref_matrix);
    free(group_ref_matrix);
    free(group_total_accum);
}

static void calculate_group_avg_speedups(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t ref_idx = al->group_avg_cmp.ref;
    size_t grp_count = base->group_count;
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        if (grp_idx == ref_idx)
            continue;

        struct speedup *sp = al->group_avg_cmp.speedups + grp_idx;
        calculate_per_value_speedup(al, ref_idx, grp_idx, sp);
    }
}

static cssort_compar(group_total_sort_cmp)
{
    const struct meas_analysis *al = statep;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    assert(a_idx != b_idx);
    double va = al->group_sum_cmp.times[a_idx].point;
    double vb = al->group_sum_cmp.times[b_idx].point;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static void calculate_group_sum_cmp_times(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    size_t val_count = base->param->value_count;
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        const struct group_analysis *group = al->group_analyses + grp_idx;
        struct point_err_est *est = al->group_sum_cmp.times + grp_idx;
        // Propagate standard deviation over sum
        double mean = 0.0;
        double err = 0.0;
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
            const struct distr *distr = group->data[val_idx].distr;
            mean += distr->mean.point;
            double st_dev = distr->st_dev.point;
            err += st_dev * st_dev;
        }
        err = sqrt(err);
        est->point = mean;
        est->err = err;
    }

    // Initialize array with indexes for sorting
    for (size_t i = 0; i < grp_count; ++i)
        al->groups_by_total_speed[i] = i;
    cssort_ext(al->groups_by_total_speed, grp_count, sizeof(size_t), group_total_sort_cmp,
               al);
}

static void calculate_group_sum_cmp_speedups(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    size_t ref_idx = al->group_sum_cmp.ref;
    for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
        if (grp_idx == ref_idx)
            continue;

        struct speedup *sp = al->group_sum_cmp.speedups + grp_idx;
        calculate_group_sum_speedup(al, ref_idx, grp_idx, sp);
    }
}

static void calculate_group_avg_speedups_p_values(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    if (base->bench_count == 1)
        return;

    size_t grp_count = base->group_count;
    if (!base->param || grp_count <= 1)
        return;

    size_t ref_idx = al->group_avg_cmp.ref;
    size_t val_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        bool flip = false;
        if (g_baseline == -1)
            flip = true;
        const struct distr *ref = al->group_analyses[ref_idx].data[val_idx].distr;
        for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
            const struct distr *distr = al->group_analyses[grp_idx].data[val_idx].distr;
            if (grp_idx == ref_idx)
                continue;

            struct speedup *sp = al->group_avg_cmp.pval_cmps[val_idx].speedups + grp_idx;
            calculate_speedup(ref, distr, flip, sp);
            al->group_avg_cmp.pval_cmps[val_idx].p_values[grp_idx] =
                p_value(ref->data, ref->count, distr->data, distr->count);
        }
    }
}

static size_t reference_bench_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->bench_by_mean_time[0];
}

static void do_bench_cmp(struct meas_analysis *al)
{
    size_t bench_count = al->base->bench_count;
    if (bench_count == 1)
        return;

    al->bench_cmp.ref = reference_bench_idx(al);
    calculate_bench_cmp_speedups(al);
    calculate_bench_cmp_p_values(al);
}

static size_t reference_per_val_group_idx(struct meas_analysis *al, size_t val_idx)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->val_benches_by_mean_time[val_idx][0];
}

static void do_pval_cmps(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count <= 1)
        return;

    size_t value_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        size_t ref_idx = reference_per_val_group_idx(al, val_idx);
        al->pval_cmps[val_idx].ref = ref_idx;
    }
    calculate_pval_cmps_speedups(al);
    calculate_pval_cmps_p_values(al);
}

static size_t reference_avg_group_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->groups_by_avg_speed[0];
}

static void do_group_avg_cmp(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count <= 1)
        return;

    size_t ref_idx = reference_avg_group_idx(al);
    al->group_avg_cmp.ref = ref_idx;

    calculate_group_avg_speedups(al);
    calculate_group_avg_speedups_p_values(al);
}

static size_t reference_group_sum_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->groups_by_total_speed[0];
}

static void do_group_sum_cmp(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count <= 1)
        return;

    calculate_group_sum_cmp_times(al);

    size_t ref_idx = reference_group_sum_idx(al);
    al->group_sum_cmp.ref = ref_idx;

    calculate_group_sum_cmp_speedups(al);
}

static void analyze_bench(struct bench_analysis *analysis)
{
    const struct bench *bench = analysis->bench;
    size_t count = bench->run_count;
    assert(count != 0);
    for (size_t i = 0; i < analysis->meas_count; ++i) {
        assert(sb_len(bench->meas[i]) == count);
        estimate_distr(bench->meas[i], count, g_nresamp, analysis->meas + i);
    }
}

static void *analyze_bench_worker(void *raw)
{
    struct analyze_task_queue *q = raw;
    init_rng_state();
    for (;;) {
        struct analyze_task *task = get_analyze_task(q);
        if (task == NULL)
            break;
        analyze_bench(task->al);
    }
    return NULL;
}

static bool parallel_execute_bench_analyses(struct bench_analysis *als, size_t count)
{
    size_t thread_count = g_threads;
    if (count < thread_count)
        thread_count = count;
    assert(thread_count > 0);

    struct analyze_task_queue q;
    init_analyze_task_queue(als, count, &q);
    bool success = false;
    if (thread_count == 1) {
        const void *result = analyze_bench_worker(&q);
        success = true;
        (void)result;
        assert(result == NULL);
    } else {
        success = spawn_threads(analyze_bench_worker, &q, thread_count);
    }
    free_analyze_task_queue(&q);
    return success;
}

static bool analyze_benches(struct analysis *al)
{
    // Benchmarks analyses are done in parallel because they are quite time-consuming because
    // of all the bootstrapping. Ideally we would parallelize the loop here too, but that
    // will require careful handling of threads creation and deletion, so leave it as is for
    // now.
    if (!parallel_execute_bench_analyses(al->bench_analyses, al->bench_count))
        return false;

    for (size_t i = 0; i < al->meas_count; ++i) {
        if (al->meas[i].is_secondary)
            continue;
        struct meas_analysis *mal = al->meas_analyses + i;
        // These things have to be done first because other analyses depend on it
        analyze_groups(mal);
        compare_benches(mal);
        compare_per_val(mal);
        compare_group_avg(mal);

        do_bench_cmp(mal);
        do_pval_cmps(mal);
        do_group_avg_cmp(mal);
        do_group_sum_cmp(mal);
    }
    return true;
}

static void init_meas_analysis(struct analysis *base, size_t meas_idx,
                               struct meas_analysis *al)
{
    size_t bench_count = base->bench_count;
    size_t grp_count = base->group_count;
    memset(al, 0, sizeof(*al));
    al->base = base;
    al->meas_idx = meas_idx;
    al->meas = base->meas + meas_idx;
    al->bench_by_mean_time = calloc(bench_count, sizeof(*al->bench_by_mean_time));
    al->benches = calloc(bench_count, sizeof(*al->benches));
    for (size_t j = 0; j < bench_count; ++j)
        al->benches[j] = base->bench_analyses[j].meas + meas_idx;
    al->group_analyses = calloc(grp_count, sizeof(*al->group_analyses));
    al->bench_cmp.speedups = calloc(bench_count, sizeof(*al->bench_cmp.speedups));
    al->bench_cmp.p_values = calloc(bench_count, sizeof(*al->bench_cmp.p_values));
    const struct bench_param *param = base->param;
    if (param) {
        size_t val_count = param->value_count;
        al->val_benches_by_mean_time =
            calloc(val_count, sizeof(*al->val_benches_by_mean_time));
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
            al->val_benches_by_mean_time[val_idx] =
                calloc(bench_count, sizeof(**al->val_benches_by_mean_time));
        al->pval_cmps = calloc(val_count, sizeof(*al->pval_cmps));
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
            al->pval_cmps[val_idx].speedups =
                calloc(grp_count, sizeof(*al->pval_cmps[val_idx].speedups));
            al->pval_cmps[val_idx].p_values =
                calloc(grp_count, sizeof(*al->pval_cmps[val_idx].p_values));
        }
        al->groups_by_avg_speed = calloc(grp_count, sizeof(*al->groups_by_avg_speed));
        al->group_avg_cmp.speedups = calloc(grp_count, sizeof(*al->group_avg_cmp.speedups));
        al->group_avg_cmp.pval_cmps =
            calloc(val_count, sizeof(*al->group_avg_cmp.pval_cmps));
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
            al->group_avg_cmp.pval_cmps[val_idx].speedups =
                calloc(grp_count, sizeof(*al->group_avg_cmp.pval_cmps[val_idx].speedups));
            al->group_avg_cmp.pval_cmps[val_idx].p_values =
                calloc(grp_count, sizeof(*al->group_avg_cmp.pval_cmps[val_idx].p_values));
        }
        al->group_sum_cmp.times = calloc(grp_count, sizeof(*al->group_sum_cmp.times));
        al->groups_by_total_speed = calloc(grp_count, sizeof(*al->groups_by_total_speed));
        al->group_sum_cmp.speedups = calloc(grp_count, sizeof(*al->group_sum_cmp.speedups));
    }
}

static void init_analysis(const struct bench_data *data, struct analysis *al)
{
    size_t meas_count = data->meas_count;

    memset(al, 0, sizeof(*al));
    al->meas = data->meas;
    al->meas_count = meas_count;
    al->bench_count = data->bench_count;
    al->benches = data->benches;
    al->bench_analyses = calloc(data->bench_count, sizeof(*al->bench_analyses));
    al->param = data->param;
    al->group_count = data->group_count;
    al->groups = data->groups;
    for (size_t i = 0; i < al->bench_count; ++i) {
        struct bench_analysis *analysis = al->bench_analyses + i;
        analysis->meas = calloc(meas_count, sizeof(*analysis->meas));
        analysis->meas_count = meas_count;
        analysis->bench = data->benches + i;
        analysis->name = analysis->bench->name;
    }

    size_t primary_meas_count = 0;
    for (size_t i = 0; i < meas_count; ++i)
        if (!data->meas[i].is_secondary)
            ++primary_meas_count;
    al->primary_meas_count = primary_meas_count;

    al->meas_analyses = calloc(meas_count, sizeof(*al->meas_analyses));
    for (size_t i = 0; i < meas_count; ++i) {
        if (data->meas[i].is_secondary)
            continue;
        init_meas_analysis(al, i, al->meas_analyses + i);
    }
}

static void free_bench_meas_analysis(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    free(al->benches);
    free(al->bench_by_mean_time);
    if (al->val_benches_by_mean_time) {
        for (size_t i = 0; i < base->param->value_count; ++i)
            free(al->val_benches_by_mean_time[i]);
    }
    free(al->val_benches_by_mean_time);
    if (al->group_analyses) {
        for (size_t i = 0; i < base->group_count; ++i)
            free(al->group_analyses[i].data);
        free(al->group_analyses);
    }
    free(al->bench_cmp.speedups);
    free(al->bench_cmp.p_values);
    if (al->pval_cmps) {
        for (size_t i = 0; i < base->param->value_count; ++i) {
            free(al->pval_cmps[i].speedups);
            free(al->pval_cmps[i].p_values);
        }
        free(al->pval_cmps);
    }
    free(al->groups_by_avg_speed);
    free(al->group_avg_cmp.speedups);
    if (al->group_avg_cmp.pval_cmps) {
        for (size_t i = 0; i < base->param->value_count; ++i) {
            free(al->group_avg_cmp.pval_cmps[i].speedups);
            free(al->group_avg_cmp.pval_cmps[i].p_values);
        }
        free(al->group_avg_cmp.pval_cmps);
    }
    free(al->group_sum_cmp.times);
    free(al->groups_by_total_speed);
    free(al->group_sum_cmp.speedups);
}

static void free_analysis(struct analysis *al)
{
    if (al->bench_analyses) {
        for (size_t i = 0; i < al->bench_count; ++i) {
            const struct bench_analysis *analysis = al->bench_analyses + i;
            free(analysis->meas);
        }
        free(al->bench_analyses);
    }
    if (al->meas_analyses) {
        for (size_t i = 0; i < al->meas_count; ++i)
            free_bench_meas_analysis(al->meas_analyses + i);
        free(al->meas_analyses);
    }
}

bool do_analysis_and_make_report(const struct bench_data *data)
{
    bool success = false;
    struct analysis al;
    init_analysis(data, &al);
    if (!analyze_benches(&al))
        goto err;
    if (!make_report(&al))
        goto err;
    success = true;
err:
    free_analysis(&al);
    return success;
}
