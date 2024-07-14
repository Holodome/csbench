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

struct analyze_task {
    struct analyze_task_queue *q;
    struct bench_analysis *al;
};

struct analyze_task_queue {
    size_t task_count;
    struct analyze_task *tasks;
    volatile size_t cursor;
};

static void init_analyze_task_queue(struct bench_analysis *als, size_t count,
                                    struct analyze_task_queue *q) {
    memset(q, 0, sizeof(*q));
    q->task_count = count;
    q->tasks = calloc(count, sizeof(*q->tasks));
    for (size_t i = 0; i < count; ++i) {
        q->tasks[i].q = q;
        q->tasks[i].al = als + i;
    }
}

static void free_analyze_task_queue(struct analyze_task_queue *q) {
    free(q->tasks);
}

static struct analyze_task *get_analyze_task(struct analyze_task_queue *q) {
    size_t idx = atomic_fetch_inc(&q->cursor);
    if (idx >= q->task_count)
        return NULL;
    return q->tasks + idx;
}

#ifdef __linux__
static int bench_sort_cmp(const void *ap, const void *bp, void *statep) {
#elif defined(__APPLE__)
static int bench_sort_cmp(void *statep, const void *ap, const void *bp) {
#else
#error
#endif
    const struct bench_sort_state *state = statep;
    size_t a_idx = *(const size_t *)ap;
    size_t b_idx = *(const size_t *)bp;
    if (a_idx == b_idx)
        return 0;

    double va = state->analyses[a_idx].meas[state->meas_idx].mean.point;
    double vb = state->analyses[b_idx].meas[state->meas_idx].mean.point;
    return va > vb;
}

static void compare_benches(struct analysis *al) {
    if (al->bench_count == 1)
        return;
    size_t bench_count = al->bench_count;
    size_t meas_count = al->meas_count;
    assert(meas_count != 0);
    for (size_t i = 0; i < meas_count; ++i) {
        if (al->meas[i].is_secondary)
            continue;

        struct bench_sort_state state = {0};
        state.meas_idx = i;
        state.analyses = al->bench_analyses;
        for (size_t j = 0; j < bench_count; ++j)
            al->meas_analyses[i].fastest[j] = j;
#ifdef __linux__
        qsort_r(al->meas_analyses[i].fastest, bench_count,
                sizeof(*al->meas_analyses[i].fastest), bench_sort_cmp, &state);
#elif defined(__APPLE__)
        qsort_r(al->meas_analyses[i].fastest, bench_count,
                sizeof(*al->meas_analyses[i].fastest), &state, bench_sort_cmp);
#else
#error
#endif
    }
}

static void analyze_var_groups(struct meas_analysis *al) {
    if (al->base->group_count == 0)
        return;
    const struct bench_var *var = al->base->var;
    for (size_t grp_idx = 0; grp_idx < al->base->group_count; ++grp_idx) {
        const struct bench_var_group *grp = al->base->var_groups + grp_idx;
        struct group_analysis *grp_al = al->group_analyses + grp_idx;
        grp_al->group = grp;
        grp_al->data = calloc(var->value_count, sizeof(*grp_al->data));
        bool values_are_doubles = true;
        double slowest = -INFINITY, fastest = INFINITY;
        for (size_t cmd_idx = 0; cmd_idx < var->value_count; ++cmd_idx) {
            const char *value = var->values[cmd_idx];
            size_t bench_idx = grp->cmd_idxs[cmd_idx];
            char *end = NULL;
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
        grp_al->values_are_doubles = values_are_doubles;
        if (values_are_doubles) {
            double *x = calloc(var->value_count, sizeof(*x));
            double *y = calloc(var->value_count, sizeof(*y));
            for (size_t i = 0; i < var->value_count; ++i) {
                x[i] = grp_al->data[i].value_double;
                y[i] = grp_al->data[i].mean;
            }
            ols(x, y, var->value_count, &grp_al->regress);
            free(x);
            free(y);
        }
    }
    for (size_t val_idx = 0; val_idx < al->base->var->value_count; ++val_idx) {
        double fastest_mean = al->group_analyses[0].data[val_idx].mean;
        for (size_t grp_idx = 1; grp_idx < al->base->group_count; ++grp_idx) {
            double t = al->group_analyses[grp_idx].data[val_idx].mean;
            if (t < fastest_mean) {
                fastest_mean = t;
                al->fastest_val[val_idx] = grp_idx;
            }
        }
    }
}

static void calculate_p_values(struct meas_analysis *al) {
    {
        const struct distr *d1;
        if (g_baseline != -1)
            d1 = al->benches[g_baseline];
        else
            d1 = al->benches[al->fastest[0]];
        for (size_t bench_idx = 0; bench_idx < al->base->bench_count;
             ++bench_idx) {
            const struct distr *d2 = al->benches[bench_idx];
            if (d1 == d2)
                continue;
            double p = mwu(d1->data, d1->count, d2->data, d2->count);
            al->p_values[bench_idx] = p;
        }
    }
    if (al->base->group_count > 1) {
        size_t var_value_count = al->base->var->value_count;
        for (size_t val_idx = 0; val_idx < var_value_count; ++val_idx) {
            const struct distr *d1;
            if (g_baseline != -1)
                d1 = al->group_analyses[g_baseline].data[val_idx].distr;
            else
                d1 = al->group_analyses[al->fastest_val[val_idx]]
                         .data[val_idx]
                         .distr;
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                const struct distr *d2 =
                    al->group_analyses[grp_idx].data[val_idx].distr;
                if (d1 == d2)
                    continue;
                double p = mwu(d1->data, d1->count, d2->data, d2->count);
                al->var_p_values[val_idx][grp_idx] = p;
            }
        }
    }
}

static void ref_speed(double u1, double sigma1, double u2, double sigma2,
                      double *ref_u, double *ref_sigma) {
    // propagate standard deviation for formula (t1 / t2)
    double ref = u1 / u2;
    double a = sigma1 / u1;
    double b = sigma2 / u2;
    double ref_st_dev = ref * sqrt(a * a + b * b);
    *ref_u = ref;
    *ref_sigma = ref_st_dev;
}

static void calculate_speedups(struct meas_analysis *al) {
    if (al->base->bench_count == 1)
        return;
    bool flip = false;
    const struct distr *reference;
    if (g_baseline != -1) {
        reference = al->benches[g_baseline];
    } else {
        reference = al->benches[al->fastest[0]];
        flip = true;
    }
    for (size_t bench_idx = 0; bench_idx < al->base->bench_count; ++bench_idx) {
        const struct distr *bench = al->benches[bench_idx];
        if (bench == reference)
            continue;
        struct point_err_est *est = al->speedup + bench_idx;
        if (flip)
            ref_speed(bench->mean.point, bench->st_dev.point,
                      reference->mean.point, reference->st_dev.point,
                      &est->point, &est->err);
        else
            ref_speed(reference->mean.point, reference->st_dev.point,
                      bench->mean.point, bench->st_dev.point, &est->point,
                      &est->err);
    }
    if (al->base->var && al->base->group_count > 1) {
        for (size_t val_idx = 0; val_idx < al->base->var->value_count;
             ++val_idx) {
            const struct group_analysis *reference_group;
            bool flip = false;
            if (g_baseline != -1) {
                reference_group = al->group_analyses + g_baseline;
            } else {
                reference_group = al->group_analyses + al->fastest_val[val_idx];
                flip = true;
            }
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *group =
                    al->group_analyses + grp_idx;
                if (group == reference_group)
                    continue;
                struct point_err_est *est = al->var_speedup[val_idx] + grp_idx;
                if (flip)
                    ref_speed(
                        group->data[val_idx].distr->mean.point,
                        group->data[val_idx].distr->st_dev.point,
                        reference_group->data[val_idx].distr->mean.point,
                        reference_group->data[val_idx].distr->st_dev.point,
                        &est->point, &est->err);
                else
                    ref_speed(
                        reference_group->data[val_idx].distr->mean.point,
                        reference_group->data[val_idx].distr->st_dev.point,
                        group->data[val_idx].distr->mean.point,
                        group->data[val_idx].distr->st_dev.point, &est->point,
                        &est->err);
            }
        }
        if (g_baseline != -1 && !g_regr) {
            const struct group_analysis *baseline_group =
                al->group_analyses + g_baseline;
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *group =
                    al->group_analyses + grp_idx;
                if (group == baseline_group)
                    continue;
                // This uses hand-written error propagation formula, for
                // reference see
                // https://en.wikipedia.org/wiki/Propagation_of_uncertainty.
                double mean_accum = 1;
                double st_dev_accum = 0.0;
                size_t n = al->base->var->value_count;
                for (size_t val_idx = 0; val_idx < n; ++val_idx) {
                    const struct point_err_est *est =
                        al->var_speedup[val_idx] + grp_idx;
                    mean_accum *= est->point;
                    double a = pow(est->point, 1.0 / n - 1.0) * est->err;
                    st_dev_accum += a * a;
                }
                double av = pow(mean_accum, 1.0 / n);
                double av_st_dev = av / n * sqrt(st_dev_accum);
                struct point_err_est *est =
                    al->group_baseline_speedup + grp_idx;
                est->point = av;
                est->err = av_st_dev;
            }
        }
    }
}

static void init_meas_analysis(struct analysis *base, size_t meas_idx,
                               struct meas_analysis *al) {
    al->base = base;
    al->meas = base->meas + meas_idx;
    al->fastest = calloc(base->bench_count, sizeof(*al->fastest));
    al->benches = calloc(base->bench_count, sizeof(*al->benches));
    for (size_t j = 0; j < base->bench_count; ++j)
        al->benches[j] = base->bench_analyses[j].meas + meas_idx;
    al->group_analyses = calloc(base->group_count, sizeof(*al->group_analyses));
    al->speedup = calloc(base->bench_count, sizeof(*al->speedup));
    al->group_baseline_speedup =
        calloc(base->group_count, sizeof(*al->group_baseline_speedup));
    al->p_values = calloc(base->bench_count, sizeof(*al->p_values));
    if (base->var) {
        al->fastest_val =
            calloc(base->var->value_count, sizeof(*al->fastest_val));
        al->var_speedup =
            calloc(base->var->value_count, sizeof(*al->var_speedup));
        for (size_t val_idx = 0; val_idx < base->var->value_count; ++val_idx) {
            al->var_speedup[val_idx] =
                calloc(base->group_count, sizeof(**al->var_speedup));
        }
        al->var_p_values =
            calloc(base->var->value_count, sizeof(*al->var_p_values));
        for (size_t val_idx = 0; val_idx < base->var->value_count; ++val_idx)
            al->var_p_values[val_idx] =
                calloc(base->group_count, sizeof(**al->var_p_values));
    }
}

static void analyze_bench(struct bench_analysis *analysis) {
    const struct bench *bench = analysis->bench;
    size_t count = bench->run_count;
    assert(count != 0);
    for (size_t i = 0; i < analysis->meas_count; ++i) {
        assert(sb_len(bench->meas[i]) == count);
        estimate_distr(bench->meas[i], count, g_nresamp, analysis->meas + i);
    }
}

static void *analyze_bench_worker(void *raw) {
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

static bool execute_analyze_tasks(struct bench_analysis *als, size_t count) {
    size_t thread_count = g_threads;
    if (count < thread_count)
        thread_count = count;
    assert(thread_count > 0);

    struct analyze_task_queue q;
    init_analyze_task_queue(als, count, &q);
    bool success;
    if (thread_count == 1) {
        void *result = analyze_bench_worker(&q);
        success = true;
        assert(result == NULL);
    } else {
        success = spawn_threads(analyze_bench_worker, &q, thread_count);
    }
    free_analyze_task_queue(&q);
    return success;
}

bool analyze_benches(struct analysis *al) {
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
        analyze_var_groups(al->meas_analyses + i);
        calculate_p_values(al->meas_analyses + i);
        calculate_speedups(al->meas_analyses + i);
    }
    return true;
}

void init_analysis(const struct bench_data *data, struct analysis *al) {
    memset(al, 0, sizeof(*al));
    al->meas = data->meas;
    al->meas_count = data->meas_count;
    al->bench_count = data->bench_count;
    al->benches = data->benches;
    al->bench_analyses = calloc(data->bench_count, sizeof(*al->bench_analyses));
    al->var = data->var;
    al->group_count = data->group_count;
    al->var_groups = data->groups;
    for (size_t i = 0; i < al->bench_count; ++i) {
        struct bench_analysis *analysis = al->bench_analyses + i;
        analysis->meas = calloc(al->meas_count, sizeof(*analysis->meas));
        analysis->meas_count = al->meas_count;
        analysis->bench = data->benches + i;
        analysis->name = analysis->bench->name;
    }
}

static void free_bench_meas_analysis(struct meas_analysis *al) {
    free(al->fastest);
    free(al->benches);
    if (al->group_analyses) {
        for (size_t i = 0; i < al->base->group_count; ++i)
            free(al->group_analyses[i].data);
        free(al->group_analyses);
    }
    free(al->speedup);
    free(al->group_baseline_speedup);
    free(al->p_values);
    free(al->fastest_val);
    if (al->var_speedup) {
        for (size_t i = 0; i < al->base->var->value_count; ++i)
            free(al->var_speedup[i]);
        free(al->var_speedup);
    }
    if (al->var_p_values) {
        for (size_t i = 0; i < al->base->var->value_count; ++i)
            free(al->var_p_values[i]);
        free(al->var_p_values);
    }
}

void free_analysis(struct analysis *al) {
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
