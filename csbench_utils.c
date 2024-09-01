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

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

struct string_ll {
    char *str;
    struct string_ll *next;
};

static struct string_ll *string_ll = NULL;

void *sb_grow_impl(void *arr, size_t inc, size_t stride)
{
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

bool units_is_time(const struct units *units)
{
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

int format_time(char *dst, size_t sz, double t)
{
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

int format_memory(char *dst, size_t sz, double t)
{
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
                 const struct units *units)
{
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

const char *outliers_variance_str(double fraction)
{
    if (fraction < 0.01)
        return "no";
    else if (fraction < 0.1)
        return "slight";
    else if (fraction < 0.5)
        return "moderate";
    return "severe";
}

const char *units_str(const struct units *units)
{
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

const char *big_o_str(enum big_o complexity)
{
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
                              double adjust_y, double *rmsp)                   \
    {                                                                          \
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

ols(1, fitting_curve_1) ols(n, fitting_curve_n) ols(n_sq, fitting_curve_n_sq)
    ols(n_cube, fitting_curve_n_cube) ols(logn, fitting_curve_logn)
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

static void resample(const double *src, size_t count, double *dst)
{
    uint64_t entropy = pcg32_fast(&g_rng_state);
    // Resample with replacement
    for (size_t i = 0; i < count; ++i)
        dst[i] = src[pcg32_fast(&entropy) % count];
    g_rng_state = entropy;
}

static void bootstrap_mean_st_dev(const double *src, size_t count, double *tmp,
                                  size_t nresamp, struct est *meane,
                                  struct est *st_deve)
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

void shuffle(size_t *arr, size_t count)
{
    for (size_t i = 0; i < count - 1; ++i) {
        size_t mod = count - i;
        size_t j = pcg32_fast(&g_rng_state) % mod + i;
        size_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static double t_statistic(const double *a, size_t n1, const double *b,
                          size_t n2)
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

double ttest(const double *a, size_t n1, const double *b, size_t n2,
             size_t nresamp)
{
    // This uses algorithm as described in
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

double mwu(const double *a, size_t n1, const double *b, size_t n2)
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
                    double sigma_g_2)
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
    double var_out_min =
        fmin(var_out(1, a, sigma_b_2, sigma_g_2),
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
    outliers->var =
        outlier_variance(distr->mean.point, distr->st_dev.point, distr->count);
}

void estimate_distr(const double *data, size_t count, size_t nresamp,
                    struct distr *distr)
{
    double *tmp = malloc(sizeof(*tmp) * count);
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
    free(tmp);
}

bool process_wait_finished_correctly(pid_t pid, bool silent)
{
    int status = 0;
    pid_t wpid;
    for (;;) {
        if ((wpid = waitpid(pid, &status, 0)) != pid) {
            if (wpid == -1 && errno == EINTR)
                continue;
            if (wpid == -1)
                csperror("waitpid");
            return false;
        }
        break;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    if (!silent)
        error("process finished with non-zero exit code");
    return false;
}

bool check_and_handle_err_pipe(int read_end, int timeout)
{
    struct pollfd pfd;
    pfd.fd = read_end;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret;
    for (;;) {
        ret = poll(&pfd, 1, timeout);
        if (ret == -1 && errno == EINTR)
            continue;
        if (ret == -1) {
            csperror("poll");
            return false;
        }
        break;
    }
    if (ret == 1 && pfd.revents & POLLIN) {
        char buf[4096];
        ret = read(read_end, buf, sizeof(buf));
        if (ret == -1) {
            csperror("read");
            return false;
        }
        if (buf[0] != '\0') {
            error("child process failed to launch: %s", buf);
            return false;
        }
    }
    return true;
}

static bool shell_launch_internal(const char *cmd, int stdin_fd, int stdout_fd,
                                  int stderr_fd, int err_pipe[2], pid_t *pidp)
{
    char *argv[] = {"sh", "-c", NULL, NULL};
    argv[2] = (char *)cmd;

    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        int fd = -1;
        if (stdin_fd == -1 || stdout_fd == -1 || stderr_fd == -1) {
            fd = open("/dev/null", O_RDWR);
            if (fd == -1) {
                csfdperror(err_pipe[1], "open(\"/dev/null\", O_RDWR)");
                _exit(-1);
            }
            if (stdin_fd == -1)
                stdin_fd = fd;
            if (stdout_fd == -1)
                stdout_fd = fd;
            if (stderr_fd == -1)
                stderr_fd = fd;
        }
        if (dup2(stdin_fd, STDIN_FILENO) == -1 ||
            dup2(stdout_fd, STDOUT_FILENO) == -1 ||
            dup2(stderr_fd, STDERR_FILENO) == -1) {
            csfdperror(err_pipe[1], "dup2");
            _exit(-1);
        }
        if (fd != -1)
            close(fd);
        if (write(err_pipe[1], "", 1) < 0)
            _exit(-1);
        if (execv("/bin/sh", argv) == -1) {
            csfdperror(err_pipe[1], "execv");
            _exit(-1);
        }
        __builtin_unreachable();
    }
    *pidp = pid;
    return check_and_handle_err_pipe(err_pipe[0], -1);
}

bool shell_launch(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd,
                  pid_t *pid)
{
    int err_pipe[2];
    if (!pipe_cloexec(err_pipe))
        return false;
    bool success = shell_launch_internal(cmd, stdin_fd, stdout_fd, stderr_fd,
                                         err_pipe, pid);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return success;
}

bool shell_launch_stdin_pipe(const char *cmd, FILE **in_pipe, pid_t *pidp)
{
    int pipe_fds[2];
    if (!pipe_cloexec(pipe_fds))
        return false;

    int stdout_fd = -1;
    int stderr_fd = -1;
    if (g_python_output) {
        stdout_fd = STDOUT_FILENO;
        stderr_fd = STDERR_FILENO;
    }
    bool success = shell_launch(cmd, pipe_fds[0], stdout_fd, stderr_fd, pidp);
    if (!success) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    close(pipe_fds[0]);
    FILE *f = fdopen(pipe_fds[1], "w");
    if (f == NULL) {
        csperror("fdopen");
        // Not a very nice way of handling errors, but it seems correct.
        close(pipe_fds[1]);
        kill(*pidp, SIGKILL);
        for (;;) {
            int err = waitpid(*pidp, NULL, 0);
            if (err == -1 && errno == EINTR)
                continue;
            break;
        }
        return false;
    }
    *in_pipe = f;
    return true;
}

bool shell_execute(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd,
                   bool silent)
{
    pid_t pid = 0;
    bool success = shell_launch(cmd, stdin_fd, stdout_fd, stderr_fd, &pid);
    if (success)
        success = process_wait_finished_correctly(pid, silent);
    return success;
}

size_t csstrlcpy(char *dst, const char *src, size_t size)
{
    size_t ret = strlen(src);
    if (size) {
        size_t len = (ret >= size) ? size - 1 : ret;
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return ret;
}

#if defined(__APPLE__)
double get_time(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
#else
double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

FILE *open_file_fmt(const char *mode, const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return fopen(buf, mode);
}

int open_fd_fmt(int flags, mode_t mode, const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return open(buf, flags, mode);
}

int tmpfile_fd(void)
{
    char path[] = "/tmp/csbench_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        csperror("mkstemp");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        csperror("fcntl");
        return false;
    }
    unlink(path);
    return fd;
}

bool spawn_threads(void *(*worker_fn)(void *), void *param, size_t thread_count)
{
    bool success = false;
    pthread_t *thread_ids = calloc(thread_count, sizeof(*thread_ids));
    // Create worker threads that do running.
    for (size_t i = 0; i < thread_count; ++i) {
        // HACK: save thread id to output anchors first. If we do not do it
        // here we would need additional synchronization
        pthread_t *id = thread_ids + i;
        if (g_progress_bar && g_output_anchors)
            id = &g_output_anchors[i].id;
        if (pthread_create(id, NULL, worker_fn, param) != 0) {
            for (size_t j = 0; j < i; ++j)
                pthread_join(thread_ids[j], NULL);
            error("failed to spawn thread");
            goto out;
        }
        thread_ids[i] = *id;
    }

    success = true;
    for (size_t i = 0; i < thread_count; ++i) {
        void *thread_retval;
        pthread_join(thread_ids[i], &thread_retval);
        if (thread_retval == (void *)-1)
            success = false;
    }

out:
    free(thread_ids);
    return success;
}

void cs_free_strings(void)
{
    for (struct string_ll *lc = string_ll; lc;) {
        free(lc->str);
        struct string_ll *next = lc->next;
        free(lc);
        lc = next;
    }
}

char *csstralloc(size_t len)
{
    struct string_ll *lc = calloc(1, sizeof(*lc));
    char *str = malloc(len + 1);
    lc->str = str;
    lc->next = string_ll;
    string_ll = lc;
    return str;
}

const char *csmkstr(const char *src, size_t len)
{
    char *str = csstralloc(len);
    memcpy(str, src, len);
    str[len] = '\0';
    return str;
}

const char *csstrdup(const char *str) { return csmkstr(str, strlen(str)); }

// TODO: Remove this function and do the same thing in place it is called
const char *csstripend(const char *src)
{
    size_t len = strlen(src);
    char *str = (char *)csmkstr(src, len);
    // XXX: I don't remember why this exists...
    while (len && str[len - 1] == '\n')
        str[len-- - 1] = '\0';
    return str;
}

const char *csfmt(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return csstrdup(buf);
}

void init_rng_state(void)
{
    pthread_t pt = pthread_self();
    if (sizeof(pt) >= sizeof(uint64_t)) {
        uint64_t entropy;
        memcpy(&entropy, &pt, sizeof(uint64_t));
        g_rng_state = time(NULL) * 2 + entropy;
    } else {
        g_rng_state = time(NULL) * 2 + 1;
    }
}

const char **parse_comma_separated_list(const char *str)
{
    const char **value_list = NULL;
    const char *cursor = str;
    const char *end = str + strlen(str);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            const char *new_str = csstripend(cursor);
            sb_push(value_list, new_str);
            break;
        }
        size_t value_len = next - cursor;
        const char *value = csmkstr(cursor, value_len);
        sb_push(value_list, value);
        cursor = next + 1;
    }
    return value_list;
}

void fprintf_colored(FILE *f, const char *how, const char *fmt, ...)
{
    if (g_colored_output) {
        fprintf(f, "\x1b[%sm", how);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\x1b[0m");
    } else {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
    }
}

void errorv(const char *fmt, va_list args)
{
    pthread_t tid = pthread_self();
    for (size_t i = 0; i < sb_len(g_output_anchors); ++i) {
        // Implicitly discard all messages but the first. This should not be an
        // issue, as the only possible message is error, and it (at least it
        // should) is always a single one
        if (pthread_equal(tid, g_output_anchors[i].id) &&
            !atomic_load(&g_output_anchors[i].has_message)) {
            vsnprintf(g_output_anchors[i].buffer,
                      sizeof(g_output_anchors[i].buffer), fmt, args);
            atomic_fence();
            atomic_store(&g_output_anchors[i].has_message, true);
            va_end(args);
            return;
        }
    }
    fprintf_colored(stderr, ANSI_RED, "error: ");
    vfprintf(stderr, fmt, args);
    putc('\n', stderr);
}

void error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    errorv(fmt, args);
    va_end(args);
}

char *csstrerror(char *buf, size_t buf_size, int err)
{
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, buf, buf_size);
#else
    strerror_r(err, buf, buf_size);
    err_msg = buf;
#endif
    return err_msg;
}

void csperror(const char *msg)
{
    int err = errno;
    char errbuf[4096];
    char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);
    error("%s: %s", msg, err_msg);
}

void csfmtperror(const char *fmt, ...)
{
    int err = errno;
    char errbuf[4096];
    char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    error("%s: %s", buf, err_msg);
}

void csfdperror(int fd, const char *msg)
{
    int err = errno;
    char errbuf[4096];
    const char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);
    char buf[4096];
    int len = snprintf(buf, sizeof(buf), "%s: %s", msg, err_msg);
    if (write(fd, buf, len + 1) < 0)
        _exit(-1);
}

bool pipe_cloexec(int fd[2])
{
    if (pipe(fd) == -1) {
        csperror("pipe");
        return false;
    }
    if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(fd[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    return true;
}

void cssort_ext(void *base, size_t nmemb, size_t size, cssort_compar_fn *compar,
                void *arg)
{
#ifdef __linux__
    qsort_r(base, nmemb, size, compar, arg);
#elif defined(__APPLE__)
    qsort_r(base, nmemb, size, arg, compar);
#else
#error
#endif
}
