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

#include <math.h>
#include <stdlib.h>
#include <string.h>

void *sb_grow_impl(void *arr, size_t inc, size_t stride) {
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

bool units_is_time(const struct units *units) {
    switch (units->kind) {
    case MU_S:
    case MU_MS:
    case MU_NS:
    case MU_US:
        return true;
    default:
        break;
    }
    return false;
}

int format_time(char *dst, size_t sz, double t) {
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

int format_memory(char *dst, size_t sz, double t) {
    int count = 0;
    // how memory can be negative? anyway..
    if (t < 0) {
        t = -t;
        count = snprintf(dst, sz, "-");
        dst += count;
        sz -= count;
    }

    const char *units = "B ";
    if (t >= (1lu << 30)) {
        units = "GB";
        t /= (1lu << 30);
    } else if (t >= (1lu << 20)) {
        units = "MB";
        t /= (1lu << 20);
    } else if (t >= (1lu << 10)) {
        units = "KB";
        t /= (1lu << 10);
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

void format_meas(char *buf, size_t buf_size, double value,
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
    case MU_B:
        format_memory(buf, buf_size, value);
        break;
    case MU_KB:
        format_memory(buf, buf_size, value * (1lu << 10));
        break;
    case MU_MB:
        format_memory(buf, buf_size, value * (1lu << 20));
        break;
    case MU_GB:
        format_memory(buf, buf_size, value * (1lu << 30));
        break;
    case MU_CUSTOM:
        snprintf(buf, buf_size, "%.5g %s", value, units->str);
        break;
    case MU_NONE:
        snprintf(buf, buf_size, "%.3g", value);
        break;
    }
}

const char *outliers_variance_str(double fraction) {
    if (fraction < 0.01)
        return "no";
    else if (fraction < 0.1)
        return "slight";
    else if (fraction < 0.5)
        return "moderate";
    return "severe";
}

const char *units_str(const struct units *units) {
    switch (units->kind) {
    case MU_S:
        return "s";
    case MU_MS:
        return "ms";
    case MU_US:
        return "μs";
    case MU_NS:
        return "ns";
    case MU_B:
        return "B";
    case MU_KB:
        return "KB";
    case MU_MB:
        return "MB";
    case MU_GB:
        return "GB";
    case MU_CUSTOM:
        return units->str;
    case MU_NONE:
        return "";
    }
    return NULL;
}

const char *big_o_str(enum big_o complexity) {
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

#define fitting_curve_1(...) (1.0)
#define fitting_curve_n(_n) (_n)
#define fitting_curve_n_sq(_n) ((_n) * (_n))
#define fitting_curve_n_cube(_n) ((_n) * (_n) * (_n))
#define fitting_curve_logn(_n) log2(_n)
#define fitting_curve_nlogn(_n) ((_n) * log2(_n))

#define ols(_name, _fitting)                                                   \
    static double ols_##_name(const double *x, const double *y, size_t count,  \
                              double adjust_y, double *rmsp) {                 \
        (void)x;                                                               \
        double sigma_gn_sq = 0.0;                                              \
        double sigma_t = 0.0;                                                  \
        double sigma_t_gn = 0.0;                                               \
        for (size_t i = 0; i < count; ++i) {                                   \
            double gn_i = _fitting(x[i] - x[0]);                               \
            sigma_gn_sq += gn_i * gn_i;                                        \
            sigma_t += y[i] - adjust_y;                                        \
            sigma_t_gn += (y[i] - adjust_y) * gn_i;                            \
        }                                                                      \
        double coef = sigma_t_gn / sigma_gn_sq;                                \
        double rms = 0.0;                                                      \
        for (size_t i = 0; i < count; ++i) {                                   \
            double fit = coef * _fitting(x[i] - x[0]);                         \
            double a = (y[i] - adjust_y) - fit;                                \
            rms += a * a;                                                      \
        }                                                                      \
        double mean = sigma_t / count;                                         \
        *rmsp = sqrt(rms / count) / mean;                                      \
        return coef;                                                           \
    }

ols(1, fitting_curve_1)
ols(n, fitting_curve_n)
ols(n_sq, fitting_curve_n_sq)
ols(n_cube, fitting_curve_n_cube)
ols(logn, fitting_curve_logn)
ols(nlogn, fitting_curve_nlogn)

#undef ols

void ols(const double *x, const double *y, size_t count,
         struct ols_regress *result)
{
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
    result->c = x[0];
    result->rms = best_fit_rms;
    result->complexity = best_fit;
}

double ols_approx(const struct ols_regress *regress, double n) {
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

bool process_finished_correctly(pid_t pid) {
    int status = 0;
    pid_t wpid;
    if ((wpid = waitpid(pid, &status, 0)) != pid) {
        if (wpid == -1)
            csperror("waitpid");
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    return false;
}

static uint32_t pcg32_fast(uint64_t *state) {
    uint64_t x = *state;
    unsigned count = (unsigned)(x >> 61);
    *state = x * UINT64_C(6364136223846793005);
    x ^= x >> 22;
    return (uint32_t)(x >> (22 + count));
}

static void resample(const double *src, size_t count, double *dst) {
    uint64_t entropy = pcg32_fast(&g_rng_state);
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[pcg32_fast(&entropy) % count];
    g_rng_state = entropy;
}

static void bootstrap_mean_st_dev(const double *src, size_t count, double *tmp,
                                  size_t nresamp, struct est *meane,
                                  struct est *st_deve) {
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
    for (size_t sample = 0; sample < nresamp; ++sample) {
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

// Fisher–Yates shuffle algorithm implementation
void shuffle(size_t *arr, size_t count) {
    for (size_t i = 0; i < count - 1; ++i) {
        size_t mod = count - i;
        size_t j = pcg32_fast(&g_rng_state) % mod + i;
        size_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
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

// Performs Mann–Whitney U test.
// Returns p-value.
double mwu(const double *a, size_t n1, const double *b, size_t n2) {
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
    for (size_t rank = 1, a_cursor = 0, b_cursor = 0;
         a_cursor != n1 || b_cursor != n2; ++rank) {
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

void estimate_distr(const double *data, size_t count, double *tmp,
                    size_t nresamp, struct distr *distr) {
    distr->data = data;
    distr->count = count;
    bootstrap_mean_st_dev(data, count, tmp, nresamp, &distr->mean,
                          &distr->st_dev);
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
}
