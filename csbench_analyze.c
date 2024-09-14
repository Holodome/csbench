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

struct bench_sort_state {
    size_t meas_idx;
    const struct bench_analysis *analyses;
};

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

static cssort_compar(bench_sort_cmp)
{
    const struct bench_sort_state *state = statep;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    assert(a_idx != b_idx);
    double va = state->analyses[a_idx].meas[state->meas_idx].mean.point;
    double vb = state->analyses[b_idx].meas[state->meas_idx].mean.point;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static void compare_benches(struct analysis *al)
{
    if (al->bench_count == 1)
        return;
    size_t bench_count = al->bench_count;
    size_t meas_count = al->meas_count;
    for (size_t meas_idx = 0; meas_idx < meas_count; ++meas_idx) {
        // We don't do comparison for secondary measurements
        if (al->meas[meas_idx].is_secondary)
            continue;

        struct bench_sort_state state = {0};
        state.meas_idx = meas_idx;
        state.analyses = al->bench_analyses;
        // Initialize array with indexes for sorting
        for (size_t i = 0; i < bench_count; ++i)
            al->meas_analyses[meas_idx].bench_by_mean_time[i] = i;
        cssort_ext(al->meas_analyses[meas_idx].bench_by_mean_time, bench_count,
                   sizeof(size_t), bench_sort_cmp, &state);
    }
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
        size_t bench_idx = grp->cmd_idxs[cmd_idx];
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

static void calculate_per_val_by_speed(struct meas_analysis *al)
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

static size_t reference_bench_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->bench_by_mean_time[0];
}

static size_t reference_per_val_group_idx(struct meas_analysis *al, size_t val_idx)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->val_benches_by_mean_time[val_idx][0];
}

static size_t reference_avg_group_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->groups_by_avg_speed[0];
}

static size_t reference_sum_group_idx(struct meas_analysis *al)
{
    if (g_baseline != -1)
        return g_baseline;
    return al->groups_by_total_speed[0];
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

static void calculate_per_bench_p_values(struct meas_analysis *al)
{
    size_t ref_idx = reference_bench_idx(al);
    const struct distr *ref = al->benches[ref_idx];
    for (size_t bench_idx = 0; bench_idx < al->base->bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        if (ref == distr)
            continue;

        al->bench_cmp.p_values[bench_idx] =
            p_value(ref->data, ref->count, distr->data, distr->count);
    }
}

static void calculate_per_group_p_values(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count <= 1)
        return;

    size_t value_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        size_t ref_idx = reference_per_val_group_idx(al, val_idx);
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

static void calculate_bench_speedups(struct meas_analysis *al)
{
    size_t bench_count = al->base->bench_count;
    if (bench_count == 1)
        return;

    bool flip = false;
    if (g_baseline == -1)
        flip = true;

    size_t ref_idx = al->bench_cmp.ref = reference_bench_idx(al);
    const struct distr *ref = al->benches[ref_idx];
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        if (distr == ref)
            continue;

        struct speedup *sp = al->bench_cmp.speedups + bench_idx;
        calculate_speedup(ref, distr, flip, sp);
    }
}

static void calculate_per_val_avg_speedups(struct meas_analysis *al)
{
    struct analysis *base = al->base;
    if (base->bench_count == 1)
        return;

    size_t grp_count = base->group_count;
    if (!base->param || grp_count <= 1)
        return;

    size_t value_count = base->param->value_count;
    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        bool flip = false;
        if (g_baseline == -1)
            flip = true;
        size_t ref_idx = reference_per_val_group_idx(al, val_idx);
        al->pval_cmps[val_idx].ref = ref_idx;
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

static cssort_compar(accum_idx_sort_cmp)
{
    (void)statep;
    const struct accum_idx *a = ap;
    const struct accum_idx *b = bp;
    assert(a->idx != b->idx);
    if (a->idx < b->idx)
        return -1;
    if (a->idx > b->idx)
        return 1;
    return 0;
}

static void calculate_groups_by_avg_speed(struct meas_analysis *al)
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

    cssort_ext(group_total_accum, grp_count, sizeof(struct accum_idx), accum_idx_sort_cmp,
               NULL);
    for (size_t i = 0; i < grp_count; ++i)
        al->groups_by_avg_speed[i] = group_total_accum[i].idx;

    free(bench_ref_matrix);
    free(group_ref_matrix);
    free(group_total_accum);
}

static void calculate_group_avg_speedups(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    if (base->group_count == 1 || !base->param)
        return;

    size_t ref_idx = reference_avg_group_idx(al);
    al->group_avg_cmp.ref = ref_idx;

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

static void calculate_groups_total_time(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (grp_count == 1 || !base->param)
        return;

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

static void calculate_groups_by_sum_speed(struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    if (base->group_count == 1 || !base->param)
        return;

    size_t ref_idx = reference_sum_group_idx(al);
    al->group_sum_cmp.ref = ref_idx;
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
        al->group_avg_cmp.pval_cmps = calloc(val_count, sizeof(*al->group_avg_cmp.pval_cmps));
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

static bool execute_analyze_tasks(struct bench_analysis *als, size_t count)
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
    if (!execute_analyze_tasks(al->bench_analyses, al->bench_count))
        return false;

    size_t meas_count = al->meas_count;
    size_t primary_meas_count = 0;
    for (size_t i = 0; i < meas_count; ++i)
        if (!al->meas[i].is_secondary)
            ++primary_meas_count;
    al->primary_meas_count = primary_meas_count;

    al->meas_analyses = calloc(meas_count, sizeof(*al->meas_analyses));
    for (size_t i = 0; i < meas_count; ++i) {
        if (al->meas[i].is_secondary)
            continue;
        init_meas_analysis(al, i, al->meas_analyses + i);
    }

    compare_benches(al);
    for (size_t i = 0; i < meas_count; ++i) {
        if (al->meas[i].is_secondary)
            continue;
        struct meas_analysis *mal = al->meas_analyses + i;
        // This has to be done first because other analyses depend on it
        analyze_groups(mal);

        calculate_per_val_by_speed(mal);
        calculate_per_bench_p_values(mal);
        calculate_per_group_p_values(mal);
        calculate_bench_speedups(mal);
        calculate_per_val_avg_speedups(mal);
        calculate_groups_by_avg_speed(mal);
        calculate_group_avg_speedups(mal);
        calculate_group_avg_speedups_p_values(mal);
        calculate_groups_total_time(mal);
        calculate_groups_by_sum_speed(mal);
    }
    return true;
}

static void init_analysis(const struct bench_data *data, struct analysis *al)
{
    memset(al, 0, sizeof(*al));
    al->meas = data->meas;
    al->meas_count = data->meas_count;
    al->bench_count = data->bench_count;
    al->benches = data->benches;
    al->bench_analyses = calloc(data->bench_count, sizeof(*al->bench_analyses));
    al->param = data->param;
    al->group_count = data->group_count;
    al->groups = data->groups;
    for (size_t i = 0; i < al->bench_count; ++i) {
        struct bench_analysis *analysis = al->bench_analyses + i;
        analysis->meas = calloc(al->meas_count, sizeof(*analysis->meas));
        analysis->meas_count = al->meas_count;
        analysis->bench = data->benches + i;
        analysis->name = analysis->bench->name;
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
