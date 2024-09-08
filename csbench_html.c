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

static void html_estimate(const char *name, const struct est *est,
                          const struct units *units, FILE *f)
{
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);
    fprintf(f,
            "<tr>"
            /**/ "<td>%s</td>"
            /**/ "<td class=\"est-bound\">%s</td>"
            /**/ "<td>%s</td>"
            /**/ "<td class=\"est-bound\">%s</td>"
            "</tr>",
            name, buf1, buf2, buf3);
}

static void html_speedup_explain_small(const struct speedup *speedup,
                                       const char *href, const char *a_name,
                                       const char *b_name, FILE *f)
{
    fprintf(f, "<a href=\"%s\">", href);
    if (g_baseline != -1)
        fprintf(f, "  <tt>%s</tt>", b_name);
    else
        fprintf(f, "  <tt>%s</tt>", a_name);
    fprintf(f, " is ");
    if (speedup->is_slower)
        fprintf(f, "%.2f times slower than ", speedup->inv_est.point);
    else
        fprintf(f, "%.2f times faster than ", speedup->est.point);
    if (g_baseline == -1)
        fprintf(f, "<tt>%s</tt>", b_name);
    else
        fprintf(f, "<tt>%s</tt>", a_name);
    fprintf(f, "</a>");
}

static void html_cmp_mean_stdev(const struct distr *a_distr,
                                const struct distr *b_distr, const char *a_name,
                                const char *b_name, const struct meas *meas,
                                FILE *f)
{
    fprintf(f,
            "<table>"
            /**/ "<thead><tr>"
            /****/ "<th></th>"
            /****/ "<th><tt>%s</tt></th>"
            /****/ "<th><tt>%s</tt></th>"
            /**/ "</tr></thead>"
            /**/ "<tbody>",
            a_name, b_name);
    char a_mean[256], b_mean[256], a_st_dev[256], b_st_dev[256];
    format_meas(a_mean, sizeof(a_mean), a_distr->mean.point, &meas->units);
    format_meas(b_mean, sizeof(b_mean), b_distr->mean.point, &meas->units);
    format_meas(a_st_dev, sizeof(a_st_dev), a_distr->st_dev.point,
                &meas->units);
    format_meas(b_st_dev, sizeof(b_st_dev), b_distr->st_dev.point,
                &meas->units);
    fprintf(f,
            "<tr><td>mean</td><td>%s</td><td>%s</td></tr>"
            "<tr><td>st dev</td><td>%s</td><td>%s</td></tr>"
            "</tbody>"
            "</table>",
            a_mean, b_mean, a_st_dev, b_st_dev);
}

static void html_speedup_explain(const struct speedup *speedup,
                                 const char *a_name, const char *b_name,
                                 FILE *f)
{
    fprintf(f, "<p>");
    if (g_baseline != -1)
        fprintf(f, "  <tt>%s</tt>", b_name);
    else
        fprintf(f, "  <tt>%s</tt>", a_name);
    fprintf(f, " is ");
    if (speedup->is_slower)
        fprintf(f, "%.3f ± %.3f times slower than ", speedup->inv_est.point,
                speedup->inv_est.err);
    else
        fprintf(f, "%.3f ± %.3f times faster than ", speedup->est.point,
                speedup->est.err);
    if (g_baseline == -1)
        fprintf(f, "<tt>%s</tt>", b_name);
    else
        fprintf(f, "<tt>%s</tt>", a_name);
    fprintf(f, "</p>"
               "<p>");
    if (speedup->is_slower)
        fprintf(f, "%.2f%% slowdown", (speedup->inv_est.point - 1.0) * 100.0);
    else
        fprintf(f, "%.2f%% speedup ", (speedup->est.point - 1.0) * 100.0);
    fprintf(f, "</p>");
}

static void html_p_value_explain(double p_value, FILE *f)
{
    fprintf(f, "<p>");
    switch (g_stat_test) {
    case STAT_TEST_MWU:
        fprintf(f, "Mann-Whitney U-test p-value=%.2f", p_value);
        break;
    case STAT_TEST_TTEST:
        fprintf(f, "Welch's t-test p-value=%.2f", p_value);
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    fprintf(f, "</p>"
               "<p>");
    switch (g_stat_test) {
    case STAT_TEST_MWU:
        if (p_value < 0.05)
            fprintf(f, "p-value < 0.05 &#8658 assuming "
                       "distribution is "
                       "different");
        else
            fprintf(f, "p-value > 0.05 &#8658 assuming "
                       "distribution is same");
        break;
    case STAT_TEST_TTEST:
        if (p_value < 0.05)
            fprintf(f, "p-value < 0.05 &#8658 assuming means are "
                       "different");
        else
            fprintf(f, "p-value > 0.05 &#8658 assuming "
                       "means are same");
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    fprintf(f, "</p>");
}

static void html_toc_bench(const struct analysis *al, FILE *f)
{
    if (al->primary_meas_count == 1) {
        const struct meas_analysis *mal = al->meas_analyses + 0;
        fprintf(f, "<ol>");
        if (g_regr)
            fprintf(f, "<li><a href=\"#regr\">parameter analysis</a></li>");
        fprintf(f, "<li><a href=\"#summary\">summary</a></li>"
                   "<li>"
                   /**/ "<a href=\"#benches\">benchmarks</a>"
                   /**/ "<ol>");
        foreach_bench_idx (bench_idx, mal) {
            fprintf(f, "<li><a href=\"#bench-%zu-%zu\"><tt>%s</tt></a></li>",
                    bench_idx, mal->meas_idx, bench_name(al, bench_idx));
        }
        fprintf(f, "</ol>"
                   "</li>");
        if (al->bench_count > 1) {
            fprintf(f, "<li>"
                       /**/ "<a href=\"#cmps\">comparisons</a>"
                       /**/ "<ol>");
            size_t reference_idx = mal->bench_speedups_reference;
            foreach_bench_idx (bench_idx, mal) {
                if (bench_idx == reference_idx)
                    continue;
                fprintf(f,
                        "<li><a href=\"#cmp-%zu\"><tt>%s</tt> vs "
                        "<tt>%s</tt></a></li>",
                        bench_idx, bench_name(al, reference_idx),
                        bench_name(al, bench_idx));
            }
            fprintf(f, "</ol>"
                       "</li>");
        }
        fprintf(f, "</ol>");
    } else {
        assert(!"not implemented");
    }
}

static void html_toc_group(const struct analysis *al, FILE *f)
{
    const struct bench_var *var = al->var;
    size_t val_count = var->value_count;
    if (al->primary_meas_count == 1) {
        const struct meas_analysis *mal = al->meas_analyses + 0;
        fprintf(f, "<ol>");
        if (g_regr)
            fprintf(f, "<li><a href=\"#regr\">parameter analysis</a></li>");
        fprintf(f, "<li><a href=\"#summary\">summary</a></li>"
                   "<li>"
                   /**/ "<a href=\"#benches\">benchmarks</a>"
                   /**/ "<ol>");
        foreach_group_by_avg_idx (grp_idx, mal) {
            fprintf(f,
                    "<li>"
                    "<a href=\"#bench-group-%zu-%zu\">"
                    /**/ "<tt>%s</tt>"
                    "</a>",
                    grp_idx, mal->meas_idx, bench_group_name(al, grp_idx));
            fprintf(f, "<ol>");
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
                fprintf(f,
                        "<li>"
                        /**/ "<a href=\"#bench-%zu-%zu-%zu\">"
                        /****/ "<tt>%s=%s</tt>"
                        /**/ "</a>"
                        "</li>",
                        grp_idx, val_idx, mal->meas_idx, var->name,
                        var->values[val_idx]);
            fprintf(f, "</ol>"
                       "</li>");
        }
        fprintf(f, "</ol>"
                   "</li>");
        if (al->bench_count > 1) {
            fprintf(f, "<li>"
                       /**/ "<a href=\"#cmps\">comparisons</a>"
                       /**/ "<ol>");
            size_t reference_idx = mal->bench_speedups_reference;
            foreach_bench_idx (bench_idx, mal) {
                if (bench_idx == reference_idx)
                    continue;
                fprintf(f,
                        "<li><a href=\"#cmp-%zu-%zu\"><tt>%s</tt> vs "
                        "<tt>%s</tt></a></li>",
                        bench_idx, mal->meas_idx, bench_name(al, reference_idx),
                        bench_name(al, bench_idx));
            }
            fprintf(f, "</ol>"
                       "</li>");
        }
        fprintf(f, "</ol>");
    } else {
        assert(!"not implemented");
    }
}

static void html_toc(const struct analysis *al, FILE *f)
{
    fprintf(f, "<div>"
               /**/ "<h3>Table of contents</h3>");
    if (al->group_count <= 1)
        html_toc_bench(al, f);
    else
        html_toc_group(al, f);
    fprintf(f, "</div>");
}

static void html_regr_bench_group(const struct group_analysis *al,
                                  const struct meas *meas, size_t meas_idx,
                                  size_t grp_idx, const struct bench_var *var,
                                  FILE *f)
{
    char fastest_mean[256], slowest_mean[256];
    format_time(fastest_mean, sizeof(fastest_mean), al->fastest->mean);
    format_time(slowest_mean, sizeof(slowest_mean), al->slowest->mean);
    fprintf(f,
            "<div>"
            /**/ "<h3>group '%s' with value %s</h3>"
            /**/ "<h4>measurement %s</h4>"
            /**/ "<div class=\"row\">"
            /****/ "<div class=\"col\">"
            /******/ "<img src=\"group_%zu_%zu.svg\">"
            /****/ "</div>"
            /****/ "<div class=\"col stats\">"
            /******/ "<p>lowest time %s with %s=%s</p>"
            /******/ "<p>hightest time %s with %s=%s</p>",
            al->group->name, var->name, meas->name, grp_idx, meas_idx,
            fastest_mean, var->name, al->fastest->value, slowest_mean,
            var->name, al->slowest->value);
    if (al->values_are_doubles)
        fprintf(f,
                "<p>mean time is most likely %s in terms of parameter</p>"
                "<p>linear coef %g rms %.3f</p>",
                big_o_str(al->regress.complexity), al->regress.a,
                al->regress.rms);
    fprintf(f,
            "</div>" // col-stats
            "</div>" // row
            "</div>");
}

static void html_var_analysis(const struct meas_analysis *al, FILE *f)
{
    return;
    const struct analysis *base = al->base;
    if (!base->group_count)
        return;
    fprintf(f, "<div id=\"regr\">"
               /**/ "<h2>parameter analysis</h2>");
    if (base->group_count > 1)
        fprintf(f,
                "<div>"
                /**/ "<h3>summary for %s</h3>"
                /**/ "<div class=\"row\">"
                /****/ "<div class=\"col\">"
                /******/ "<img src=\"group_%zu.svg\">"
                /****/ "</div>"
                /****/ "<div class=\"col\">"
                /******/ "<img src=\"group_bar_%zu.svg\">"
                /****/ "</div>"
                /**/ "</div>"
                "</div>",
                al->meas->name, al->meas_idx, al->meas_idx);
    if (g_regr) {
        for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
            const struct meas *meas = al->meas + al->meas_idx;
            const struct group_analysis *group = al->group_analyses + grp_idx;
            html_regr_bench_group(group, meas, al->meas_idx, grp_idx, base->var,
                                  f);
        }
    }
    fprintf(f, "</div>");
}

static void html_bench_summary(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    fprintf(f,
            "<div id=\"summary\">"
            /**/ "<h2>summary</h2>"
            /**/ "<div class=\"row\">"
            /****/ "<div class=\"col\">"
            /******/ "<img src=\"bar_%zu.svg\">"
            /****/ "</div>"
            /****/ "<div class=\"col\">"
            /******/ "<p>executed %zu <a href=\"#benches\">benchmarks</a>:</p>"
            /******/ "<ol>",
            al->meas_idx, base->bench_count);
    foreach_bench_idx (bench_idx, al) {
        fprintf(f,
                "<li>"
                "<a href=\"#bench-%zu\">"
                /**/ "<tt>%s</tt>"
                "</a>",
                bench_idx, bench_name(base, bench_idx));
        switch (g_sort_mode) {
        case SORT_RAW:
        case SORT_SPEED:
            if (bench_idx == al->bench_speedups_reference)
                fprintf(f, " (fastest)");
            else if (bench_idx == al->bench_by_mean_time[base->bench_count - 1])
                fprintf(f, " (slowest)");
            break;
        case SORT_BASELINE_RAW:
        case SORT_BASELINE_SPEED:
            if (bench_idx == (size_t)g_baseline)
                fprintf(f, " (baseline)");
            break;
        default:
            ASSERT_UNREACHABLE();
        }
        fprintf(f, "</li>");
    }
    fprintf(f, "</ol>");
    size_t reference_idx = al->bench_speedups_reference;
    fprintf(f, "<p>performed <a href=\"#cmps\">comparisons</a>:<p>"
               "<ul>");
    foreach_bench_idx (bench_idx, al) {
        if (bench_idx == reference_idx)
            continue;
        const struct speedup *speedup = al->bench_speedups + bench_idx;
        const char *a_name = bench_name(base, reference_idx);
        const char *b_name = bench_name(base, bench_idx);
        char href[256];
        snprintf(href, sizeof(href), "#cmp-%zu-%zu", bench_idx, al->meas_idx);
        fprintf(f, "<li>");
        html_speedup_explain_small(speedup, href, a_name, b_name, f);
        fprintf(f, "</li>");
    }
    fprintf(f,
            "</ul>"
            "</div>" // col
            "</div>" // row
            "</div>");
}

static void html_group_summary(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t val_count = var->value_count;
    fprintf(f,
            "<div id=\"summary\">"
            /**/ "<h2>summary</h2>"
            /**/ "<div class=\"row\">"
            /****/ "<div class=\"col\">"
            /******/ "<img src=\"group_bar_%zu.svg\">"
            /****/ "</div>"
            /****/ "<div class=\"col\">"
            /******/ "<p>used benchmark parameter %s</p>"
            /******/ "<p>executed %zu groups with %zu total <a "
            "href=\"#benches\">benchmarks</a>:</p>"
            /******/ "<ol>",
            al->meas_idx, var->name, base->group_count, base->bench_count);
    foreach_group_by_avg_idx (grp_idx, al) {
        fprintf(f,
                "<li>"
                "<a href=\"#bench-group-%zu-%zu\">"
                /**/ "<tt>%s</tt>"
                "</a>",
                grp_idx, al->meas_idx, bench_group_name(base, grp_idx));
        switch (g_sort_mode) {
        case SORT_RAW:
        case SORT_SPEED:
            if (grp_idx == al->groups_avg_reference)
                fprintf(f, " (fastest)");
            else if (grp_idx == al->groups_by_avg_speed[base->group_count - 1])
                fprintf(f, " (slowest)");
            break;
        case SORT_BASELINE_RAW:
        case SORT_BASELINE_SPEED:
            if (grp_idx == (size_t)g_baseline)
                fprintf(f, " (baseline)");
            break;
        default:
            ASSERT_UNREACHABLE();
        }
        fprintf(f, "<ol>");
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx)
            fprintf(f,
                    "<li>"
                    /**/ "<a href=\"#bench-%zu-%zu-%zu\">"
                    /****/ "<tt>%s=%s</tt>"
                    /**/ "</a>"
                    "</li>",
                    grp_idx, val_idx, al->meas_idx, var->name,
                    var->values[val_idx]);
        fprintf(f, "</ol>"
                   "</li>");
    }
    fprintf(f, "</ol>"
               "<p>per-value comparisons:</p>"
               "<ol>");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        fprintf(f,
                "<li>"
                "<tt>%s=%s</tt>"
                "<ul>",
                var->name, var->values[val_idx]);
        size_t reference_idx = al->val_bench_speedups_references[val_idx];
        foreach_per_val_group_idx (grp_idx, val_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            const struct speedup *speedup =
                al->val_bench_speedups[val_idx] + grp_idx;
            const char *a_name = bench_group_name(base, reference_idx);
            const char *b_name = bench_group_name(base, grp_idx);
            char href[256];
            snprintf(href, sizeof(href), "#cmp-%zu-%zu-%zu", grp_idx, val_idx,
                     al->meas_idx);
            fprintf(f, "<li>");
            html_speedup_explain_small(speedup, href, a_name, b_name, f);
            fprintf(f, "</li>");
        }
        fprintf(f, "</ul>"
                   "</li>");
    }
    fprintf(f, "</ol>"
               "<p>in total (avg):</p>"
               "<ul>");
    {
        size_t reference_idx = al->groups_avg_reference;
        foreach_group_by_avg_idx (grp_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            const struct speedup *speedup = al->group_avg_speedups + grp_idx;
            const char *a_name = bench_group_name(base, reference_idx);
            const char *b_name = bench_group_name(base, grp_idx);
            char href[256];
            snprintf(href, sizeof(href), "#cmpg-%zu-%zu", grp_idx,
                     al->meas_idx);
            fprintf(f, "<li>");
            html_speedup_explain_small(speedup, href, a_name, b_name, f);
            fprintf(f, "</li>");
        }
    }
    fprintf(f, "</ul>"
               "<p>in total (sum):</p>"
               "<ul>");
    {
        size_t reference_idx = al->groups_total_reference;
        foreach_group_by_avg_idx (grp_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            const struct speedup *speedup = al->group_total_speedups + grp_idx;
            const char *a_name = bench_group_name(base, reference_idx);
            const char *b_name = bench_group_name(base, grp_idx);
            char href[256];
            snprintf(href, sizeof(href), "#cmpg-%zu-%zu", grp_idx,
                     al->meas_idx);
            fprintf(f, "<li>");
            html_speedup_explain_small(speedup, href, a_name, b_name, f);
            fprintf(f, "</li>");
        }
    }
    fprintf(f,
            "</ul>"
            "</div>" // col
            "</div>" // row
            "</div>");
}

static void html_summary(const struct meas_analysis *al, FILE *f)
{
    if (al->base->group_count <= 1)
        html_bench_summary(al, f);
    else
        html_group_summary(al, f);
}

static void html_outliers(const struct outliers *outliers, size_t run_count,
                          FILE *f)
{
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
            "estimated standard deviation</p>",
            outliers_variance_str(outliers->var), outliers->var * 100.0);
}

static void html_distr(const struct bench_analysis *analysis, size_t bench_idx,
                       size_t meas_idx, const struct analysis *al, FILE *f)
{
    const struct distr *distr = analysis->meas + meas_idx;
    const struct bench *bench = analysis->bench;
    const struct meas *meas = al->meas + meas_idx;
    assert(!meas->is_secondary);
    char min_buf[256], max_buf[256];
    format_meas(min_buf, sizeof(min_buf), distr->min, &meas->units);
    format_meas(max_buf, sizeof(max_buf), distr->max, &meas->units);
    fprintf(f,
            "<div class=\"row\">"
            /**/ "<div class=\"col\">"
            /****/ "<h3>%s kde plot</h3>"
            /****/ "<a href=\"kde_%zu_%zu.svg\">"
            /******/ "<img src=\"kde_small_%zu_%zu.svg\">"
            /****/ "</a>"
            "</div>"
            "<div class=\"col\">"
            /**/ "<h3>statistics</h3>"
            /**/ "<div class=\"stats\">"
            /****/ "<p>%zu runs</p>"
            /****/ "<p>min %s</p>"
            /****/ "<p>max %s</p>"
            /****/ "<table>"
            /******/ "<thead><tr>"
            /********/ "<th></th>"
            /********/ "<th class=\"est-bound\">lower bound</th>"
            /********/ "<th class=\"est-bound\">estimate</th>"
            /********/ "<th class=\"est-bound\">upper bound</th>"
            /******/ "</tr></thead>"
            /******/ "<tbody>",
            meas->name, bench_idx, meas_idx, bench_idx, meas_idx,
            bench->run_count, min_buf, max_buf);
    html_estimate("mean", &distr->mean, &meas->units, f);
    html_estimate("st dev", &distr->st_dev, &meas->units, f);
    for (size_t j = 0; j < al->meas_count; ++j) {
        if (al->meas[j].is_secondary && al->meas[j].primary_idx == meas_idx)
            html_estimate(al->meas[j].name, &analysis->meas[j].mean,
                          &al->meas->units, f);
    }
    fprintf(f, "</tbody>"
               "</table>");
    html_outliers(&distr->outliers, bench->run_count, f);
    fprintf(f,
            "</div>" // stats
            "</div>" // col
            "</div>" // row
    );
}

static void html_benches(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    fprintf(f, "<div id=\"benches\">"
               "<h2>benchmarks</h2>");
    if (base->group_count <= 1) {
        foreach_bench_idx (bench_idx, al) {
            const struct bench_analysis *bench =
                base->bench_analyses + bench_idx;
            fprintf(f,
                    "<div id=\"bench-%zu-%zu\">"
                    "<h3>benchmark <tt>%s</tt></h3>",
                    bench_idx, al->meas_idx, bench_name(base, bench_idx));
            html_distr(bench, bench_idx, al->meas_idx, base, f);
            fprintf(f, "</div>");
        }
    } else {
        const struct bench_var *var = base->var;
        size_t val_count = var->value_count;
        foreach_group_by_avg_idx (grp_idx, al) {
            const struct group_analysis *grp_al = al->group_analyses + grp_idx;
            fprintf(f,
                    "<div id=\"bench-group-%zu-%zu\">"
                    "<h3>benchmark group <tt>%s</tt></h3>",
                    grp_idx, al->meas_idx, bench_group_name(base, grp_idx));
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
                size_t bench_idx = grp_al->group->cmd_idxs[val_idx];
                const struct bench_analysis *bench_al =
                    base->bench_analyses + bench_idx;
                fprintf(f,
                        "<div id=\"bench-%zu-%zu-%zu\">"
                        "<h4><tt>%s</tt></h4>",
                        grp_idx, val_idx, al->meas_idx, bench_al->name);
                html_distr(bench_al, bench_idx, al->meas_idx, base, f);
                fprintf(f, "</div>");
            }
            fprintf(f, "</div>");
        }
    }
    fprintf(f, "</div>");
}

static void html_compare_benches_nav(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    fprintf(f, "<div id=\"cmps\">"
               /**/ "<div class=\"row\">"
               /****/ "<div class=\"col\">"
               /******/ "<h2>comparisons</h2>");
    size_t reference_idx = al->bench_speedups_reference;
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_SPEED:
        fprintf(f,
                "<p><tt>%s</tt> is fastest, used as reference in "
                "comparisons</p>",
                bench_name(base, reference_idx));
        break;
    case SORT_BASELINE_RAW:
    case SORT_BASELINE_SPEED:
        fprintf(f,
                "<p><tt>%s</tt> is baseline, used as reference in "
                "comparisons</p>",
                bench_name(base, reference_idx));
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    fprintf(f, "<table>"
               /**/ "<thead><tr><th></th>");
    foreach_bench_idx (bench_idx, al) {
        if (reference_idx == bench_idx)
            continue;
        fprintf(f, "<th><tt>%s</tt></th>", bench_name(base, bench_idx));
    }
    fprintf(f, "</tr></thead>"
               "<tbody>");
    {
        fprintf(f,
                "<tr>"
                "<td><tt>%s</tt></td>",
                bench_name(base, reference_idx));
        foreach_bench_idx (bench_idx, al) {
            if (reference_idx == bench_idx)
                continue;
            fprintf(f, "<td><a href=\"#cmp-%zu-%zu\">comparison</a></td>",
                    bench_idx, al->meas_idx);
        }
        fprintf(f, "</tr>");
    }

    fprintf(f,
            "</tbody>"
            "</table>"
            "</div>" // col
            "</div>" // row
            "</div>" // #cmps
    );
}

static void html_compare_benches_kdes(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    size_t reference_idx = al->bench_speedups_reference;
    const struct bench_analysis *reference =
        base->bench_analyses + reference_idx;
    fprintf(f, "<div id=\"kde-cmps\">");
    foreach_bench_idx (bench_idx, al) {
        const struct bench_analysis *bench = base->bench_analyses + bench_idx;
        if (bench == reference)
            continue;
        const char *a_name = bench_name(base, reference_idx);
        const char *b_name = bench_name(base, bench_idx);
        const struct distr *a_distr = al->benches[reference_idx];
        const struct distr *b_distr = al->benches[bench_idx];
        fprintf(f,
                "<div id=\"cmp-%zu-%zu\">"
                /**/ "<h3><tt>%s</tt> vs <tt>%s</tt></h3>"
                /**/ "<div class=\"row\">"
                /****/ "<div class=\"col\">"
                /******/ "<a href=\"kde_cmp_%zu_%zu.svg\">"
                /********/ "<img src=\"kde_cmp_small_%zu_%zu.svg\">"
                /******/ "</a>"
                /****/ "</div>" // col
                /****/ "<div class=\"col\">"
                /******/ "<h3>statistics</h3>"
                /******/ "<div class=\"stats\">",
                bench_idx, al->meas_idx, a_name, b_name, bench_idx,
                al->meas_idx, bench_idx, al->meas_idx);

        html_cmp_mean_stdev(a_distr, b_distr, a_name, b_name, al->meas, f);
        const struct speedup *speedup = al->bench_speedups + bench_idx;
        html_speedup_explain(speedup, a_name, b_name, f);
        double p_value = al->p_values[bench_idx];
        html_p_value_explain(p_value, f);

        fprintf(f,
                "</div>" // stats
                "</div>" // cols
                "</div>" // rows
                "</div>" // kde-cmp
        );
    }
    fprintf(f, "</div>"); // kde-cmps
}

static void html_comapre_groups_group_cmp_nav(const struct meas_analysis *al,
                                              FILE *f)
{
    const struct analysis *base = al->base;
    size_t reference_idx = al->groups_avg_reference;
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_SPEED:
        fprintf(f,
                "<p><tt>%s</tt> is fastest, used as reference in "
                "comparisons</p>",
                bench_group_name(base, reference_idx));
        break;
    case SORT_BASELINE_RAW:
    case SORT_BASELINE_SPEED:
        fprintf(f,
                "<p><tt>%s</tt> is baseline, used as reference in "
                "comparisons</p>",
                bench_group_name(base, reference_idx));
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    fprintf(f, "<table>"
               /**/ "<thead><tr><th></th>");
    foreach_group_by_avg_idx (grp_idx, al) {
        if (reference_idx == grp_idx)
            continue;
        fprintf(f, "<th><tt>%s</tt></th>", bench_group_name(base, grp_idx));
    }
    fprintf(f, "</tr></thead>"
               "<tbody>");
    {
        fprintf(f,
                "<tr>"
                "<td><tt>%s</tt></td>",
                bench_group_name(base, reference_idx));
        foreach_group_by_avg_idx (grp_idx, al) {
            if (reference_idx == grp_idx)
                continue;
            fprintf(f, "<td><a href=\"#cmpg-%zu-%zu\">comparison</a></td>",
                    grp_idx, al->meas_idx);
        }
        fprintf(f, "</tr>");
    }
    fprintf(f, "</tbody>"
               "</table>");
}

static void html_compare_groups_per_val_nav(const struct meas_analysis *al,
                                            FILE *f)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t val_count = var->value_count;
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_SPEED:
        break;
    case SORT_BASELINE_RAW:
    case SORT_BASELINE_SPEED:
        fprintf(f,
                "<p><tt>%s</tt> is baseline, used as reference in "
                "comparisons</p>",
                bench_group_name(base, g_baseline));
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    fprintf(f, "<h4>per-value comparisons</h4>");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        size_t reference_idx = al->val_bench_speedups_references[val_idx];
        fprintf(f,
                "<div>"
                "<h5><tt>%s=%s</tt></h5>",
                var->name, var->values[val_idx]);
        switch (g_sort_mode) {
        case SORT_RAW:
        case SORT_SPEED:
            fprintf(f,
                    "<p><tt>%s</tt> is fastest, used as reference in "
                    "comparisons</p>",
                    bench_group_name(base, reference_idx));
            break;
        case SORT_BASELINE_RAW:
        case SORT_BASELINE_SPEED:
            break;
        default:
            ASSERT_UNREACHABLE();
        }
        fprintf(f, "<table>"
                   /**/ "<thead><tr><th></th>");
        foreach_group_by_avg_idx (grp_idx, al) {
            if (reference_idx == grp_idx)
                continue;
            fprintf(f, "<th><tt>%s</tt></th>", bench_group_name(base, grp_idx));
        }
        fprintf(f, "</tr></thead>"
                   "<tbody>");
        {
            fprintf(f,
                    "<tr>"
                    "<td><tt>%s</tt></td>",
                    bench_group_name(base, reference_idx));
            foreach_group_by_avg_idx (grp_idx, al) {
                if (reference_idx == grp_idx)
                    continue;
                fprintf(f,
                        "<td><a href=\"#cmp-%zu-%zu-%zu\">comparison</a></td>",
                        grp_idx, val_idx, al->meas_idx);
            }
            fprintf(f, "</tr>");
        }
        fprintf(f, "</tbody>"
                   "</table>"
                   "</div>");
    }
    fprintf(f, "</tr></thead>"
               "<tbody>"
               "</tbody>"
               "</table>");
}

static void html_compare_groups_nav(const struct meas_analysis *al, FILE *f)
{
    fprintf(f, "<div id=\"cmps\">"
               /**/ "<div class=\"row\">"
               /****/ "<div class=\"col\">"
               /******/ "<h2>comparisons</h2>"
               /******/ "<h4>groups comparison</h4>");
    html_comapre_groups_group_cmp_nav(al, f);
    html_compare_groups_per_val_nav(al, f);
    fprintf(f,
            "</div>" // col
            "</div>" // row
            "</div>" // #cmps
    );
}

static void html_compare_groups_kdes(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t val_count = var->value_count;
    fprintf(f, "<div id=\"kde-cmps\">"
               "<h3>KDE comparisons</h3>"
               "<h4>groups comparison</h4>");
    {
        size_t reference_idx = al->groups_avg_reference;
        foreach_group_by_avg_idx (grp_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            const char *a_name = bench_group_name(base, reference_idx);
            const char *b_name = bench_group_name(base, grp_idx);
            fprintf(f,
                    "<div id=\"cmpg-%zu-%zu\">"
                    /**/ "<h3><tt>%s</tt> vs <tt>%s</tt></h3>"
                    /**/ "<img src=\"kde_cmp_all_groups_%zu_%zu.svg\">",
                    grp_idx, al->meas_idx, a_name, b_name, grp_idx,
                    al->meas_idx);
            fprintf(f, "<p>Average difference by geometric mean of per-value "
                       "differences:</p>");
            {
                const struct speedup *speedup =
                    al->group_avg_speedups + grp_idx;
                html_speedup_explain(speedup, a_name, b_name, f);
            }
            fprintf(f, "<p>Average difference by sum:</p>");
            {
                // TODO: This has to use other order
                const struct speedup *speedup =
                    al->group_total_speedups + grp_idx;
                html_speedup_explain(speedup, a_name, b_name, f);
            }
            fprintf(f, "</div>");
        }
    }

    fprintf(f, "<h4>per-value comparisons</h4>");
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        size_t reference_idx = al->val_bench_speedups_references[val_idx];
        fprintf(f,
                "<div>"
                "<h5><tt>%s=%s</tt></h5>",
                var->name, var->values[val_idx]);
        foreach_group_by_avg_idx (grp_idx, al) {
            if (reference_idx == grp_idx)
                continue;
            const char *a_name = bench_group_name(base, reference_idx);
            const char *b_name = bench_group_name(base, grp_idx);
            size_t a_bench_idx =
                al->group_analyses[reference_idx].group->cmd_idxs[val_idx];
            size_t b_bench_idx =
                al->group_analyses[grp_idx].group->cmd_idxs[val_idx];
            const struct distr *a_distr = al->benches[a_bench_idx];
            const struct distr *b_distr = al->benches[b_bench_idx];
            fprintf(
                f,
                "<div id=\"cmp-%zu-%zu-%zu\">"
                /**/ "<h6><tt>%s</tt> vs <tt>%s</tt></h6>"
                /**/ "<div class=\"row\">"
                /****/ "<div class=\"col\">"
                /******/ "<a href=\"kde_pval_cmp_%zu_%zu_%zu.svg\">"
                /********/ "<img src=\"kde_pval_cmp_small_%zu_%zu_%zu.svg\">"
                /******/ "</a>"
                /****/ "</div>" // col
                /****/ "<div class=\"col\">"
                /******/ "<h3>statistics</h3>"
                /******/ "<div class=\"stats\">",
                grp_idx, val_idx, al->meas_idx, a_name, b_name, grp_idx,
                val_idx, al->meas_idx, grp_idx, val_idx, al->meas_idx);

            html_cmp_mean_stdev(a_distr, b_distr, a_name, b_name, al->meas, f);
            const struct speedup *speedup =
                al->val_bench_speedups[val_idx] + grp_idx;
            html_speedup_explain(speedup, a_name, b_name, f);
            double p_value = al->val_p_values[val_idx][grp_idx];
            html_p_value_explain(p_value, f);

            fprintf(f,
                    "</div>" // stats
                    "</div>" // cols
                    "</div>" // rows
                    "</div>" // kde-cmp
            );
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</div>"); // kde-cmps
}

static void html_compare(const struct meas_analysis *al, FILE *f)
{
    if (al->base->bench_count == 1)
        return;
    if (al->base->group_count <= 1) {
        html_compare_benches_nav(al, f);
        html_compare_benches_kdes(al, f);
    } else {
        html_compare_groups_nav(al, f);
        html_compare_groups_kdes(al, f);
    }
}

static void html_report(const struct analysis *al, FILE *f)
{
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
    html_toc(al, f);
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        const struct meas_analysis *mal = al->meas_analyses + meas_idx;
        html_var_analysis(mal, f);
        html_summary(mal, f);
        html_benches(mal, f);
        html_compare(mal, f);
    }
    fprintf(f, "</body>");
}

bool make_html_report(const struct analysis *al)
{
    FILE *f = open_file_fmt("w", "%s/index.html", g_out_dir);
    if (f == NULL) {
        error("failed to create file '%s/index.html'", g_out_dir);
        return false;
    }
    html_report(al, f);
    fclose(f);
    return true;
}
