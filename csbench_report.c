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
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum plot_kind {
    PLOT_BAR,
    PLOT_GROUP_BAR,
    PLOT_GROUP_SINGLE,
    PLOT_GROUP,
    PLOT_KDE,
    PLOT_KDE_EXT,
    PLOT_KDE_CMP,
    PLOT_KDE_CMPG
};

struct plot_walker_args {
    const struct meas_analysis *analysis;
    enum plot_kind plot_kind;
    pid_t *pids;
    size_t meas_idx;
    size_t bench_idx;
    size_t grp_idx;
    size_t var_value_idx;
};

static bool json_escape(char *buf, size_t buf_size, const char *src) {
    if (src == NULL) {
        assert(buf_size);
        *buf = '\0';
        return true;
    }
    const char *end = buf + buf_size;
    while (*src) {
        if (buf >= end)
            return false;

        int c = *src++;
        if (c == '\"') {
            *buf++ = '\\';
            if (buf >= end)
                return false;
            *buf++ = c;
        } else {
            *buf++ = c;
        }
    }
    if (buf >= end)
        return false;
    *buf = '\0';
    return true;
}

static bool export_json(const struct analysis *al, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        error("failed to open file '%s' for export", filename);
        return false;
    }

    char buf[4096];
    size_t bench_count = al->bench_count;
    const struct bench_analysis *bench_analyses = al->bench_analyses;
    fprintf(f,
            "{ \"settings\": {"
            "\"time_limit\": %f, \"runs\": %d, \"min_runs\": %d, "
            "\"max_runs\": %d, \"warmup_time\": %f, \"nresamp\": %d "
            "}, \"benches\": [",
            g_bench_stop.time_limit, g_bench_stop.runs, g_bench_stop.min_runs,
            g_bench_stop.max_runs, g_warmup_stop.time_limit, g_nresamp);
    for (size_t i = 0; i < bench_count; ++i) {
        const struct bench_analysis *analysis = bench_analyses + i;
        const struct bench *bench = analysis->bench;
        fprintf(f, "{ ");
        if (g_prepare)
            json_escape(buf, sizeof(buf), g_prepare);
        else
            *buf = '\0';
        fprintf(f, "\"prepare\": \"%s\", ", buf);
        json_escape(buf, sizeof(buf), analysis->name);
        fprintf(f, "\"command\": \"%s\", ", buf);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"exit_codes\": [");
        for (size_t j = 0; j < run_count; ++j)
            fprintf(f, "%d%s", bench->exit_codes[j],
                    j != run_count - 1 ? ", " : "");
        fprintf(f, "], \"meas\": [");
        for (size_t j = 0; j < al->meas_count; ++j) {
            const struct meas *info = al->meas + j;
            json_escape(buf, sizeof(buf), info->name);
            fprintf(f, "{ \"name\": \"%s\", ", buf);
            json_escape(buf, sizeof(buf), units_str(&info->units));
            fprintf(f, "\"units\": \"%s\",", buf);
            json_escape(buf, sizeof(buf), info->cmd);
            fprintf(f,
                    " \"cmd\": \"%s\", "
                    "\"val\": [",
                    buf);
            for (size_t k = 0; k < run_count; ++k)
                fprintf(f, "%f%s", bench->meas[j][k],
                        k != run_count - 1 ? ", " : "");
            fprintf(f, "]}");
            if (j != al->meas_count - 1)
                fprintf(f, ", ");
        }
        fprintf(f, "]}");
        if (i != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return true;
}

static bool do_export(const struct analysis *al) {
    if (g_json_export_filename == NULL)
        return true;

    return export_json(al, g_json_export_filename);
}

static bool python_found(void) {
    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (execlp("python3", "python3", "--version", NULL) == -1)
            _exit(-1);
    }
    return process_finished_correctly(pid);
}

static bool launch_python_stdin_pipe(FILE **inp, pid_t *pidp) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        csperror("pipe");
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[1]);
        if (dup2(pipe_fds[0], STDIN_FILENO) == -1)
            _exit(-1);
        if (!g_python_output) {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
        if (execlp("python3", "python3", NULL) == -1)
            _exit(-1);
    }
    close(pipe_fds[0]);
    FILE *f = fdopen(pipe_fds[1], "w");
    if (f == NULL) {
        csperror("fdopen");
        // Not a very nice way of handling errors, but it seems correct.
        close(pipe_fds[1]);
        kill(pid, SIGKILL);
        for (;;) {
            int err = waitpid(pid, NULL, 0);
            if (err == -1 && errno == EINTR)
                continue;
            break;
        }
        return false;
    }
    *pidp = pid;
    *inp = f;
    return true;
}

static bool python_has_matplotlib(void) {
    FILE *f;
    pid_t pid;
    if (!launch_python_stdin_pipe(&f, &pid))
        return false;
    fprintf(f, "import matplotlib\n");
    fclose(f);
    return process_finished_correctly(pid);
}

static bool plot_walker(bool (*walk)(struct plot_walker_args *args),
                        struct plot_walker_args *args) {
    const struct meas_analysis *al = args->analysis;
    if (al->base->bench_count > 1) {
        if (al->base->group_count <= 1) {
            args->plot_kind = PLOT_BAR;
            if (!walk(args))
                return false;
        } else {
            args->plot_kind = PLOT_GROUP_BAR;
            if (!walk(args))
                return false;
        }
    }
    if (g_regr) {
        for (size_t grp_idx = 0; grp_idx < al->base->group_count; ++grp_idx) {
            const struct group_analysis *grp = al->group_analyses + grp_idx;
            if (!grp->values_are_doubles)
                break;
            args->plot_kind = PLOT_GROUP_SINGLE;
            args->grp_idx = grp_idx;
            if (!walk(args))
                return false;
        }
        if (al->base->group_count > 1) {
            const struct group_analysis *grp = al->group_analyses;
            if (grp[0].values_are_doubles) {
                args->plot_kind = PLOT_GROUP;
                if (!walk(args))
                    return false;
            }
        }
    }
    for (size_t bench_idx = 0; bench_idx < al->base->bench_count; ++bench_idx) {
        args->plot_kind = PLOT_KDE;
        args->bench_idx = bench_idx;
        if (!walk(args))
            return false;
        args->plot_kind = PLOT_KDE_EXT;
        args->bench_idx = bench_idx;
        if (!walk(args))
            return false;
    }
    if (al->base->group_count == 2) {
        size_t value_count = al->base->var->value_count;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            args->plot_kind = PLOT_KDE_CMPG;
            args->var_value_idx = val_idx;
            if (!walk(args))
                return false;
        }
    } else if (al->base->bench_count == 2) {
        args->plot_kind = PLOT_KDE_CMP;
        if (!walk(args))
            return false;
    }
    return true;
}

static void format_plot_name(char *buf, size_t buf_size,
                             const struct plot_walker_args *args,
                             const char *extension) {
    switch (args->plot_kind) {
    case PLOT_BAR:
        snprintf(buf, buf_size, "%s/bar_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_GROUP_BAR:
        snprintf(buf, buf_size, "%s/group_bar_%zu.%s", g_out_dir,
                 args->meas_idx, extension);
        break;
    case PLOT_GROUP_SINGLE:
        snprintf(buf, buf_size, "%s/group_%zu_%zu.%s", g_out_dir, args->grp_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_GROUP:
        snprintf(buf, buf_size, "%s/group_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_KDE:
        snprintf(buf, buf_size, "%s/kde_%zu_%zu.%s", g_out_dir, args->bench_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_KDE_EXT:
        snprintf(buf, buf_size, "%s/kde_ext_%zu_%zu.%s", g_out_dir,
                 args->bench_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMPG:
        snprintf(buf, buf_size, "%s/kde_cmpg_%zu_%zu.%s", g_out_dir,
                 args->var_value_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP:
        snprintf(buf, buf_size, "%s/kde_cmp_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    }
}

static void write_make_plot(const struct plot_walker_args *args, FILE *f) {
    const struct meas_analysis *al = args->analysis;
    const struct meas *meas = al->meas;
    char svg_buf[4096];
    format_plot_name(svg_buf, sizeof(svg_buf), args, "svg");
    switch (args->plot_kind) {
    case PLOT_BAR:
        bar_plot(al, svg_buf, f);
        break;
    case PLOT_GROUP_BAR:
        group_bar_plot(al, svg_buf, f);
        break;
    case PLOT_GROUP_SINGLE:
        group_plot(al->group_analyses + args->grp_idx, 1, meas, al->base->var,
                   svg_buf, f);
        break;
    case PLOT_GROUP:
        group_plot(al->group_analyses, al->base->group_count, meas,
                   al->base->var, svg_buf, f);
        break;
    case PLOT_KDE:
        kde_plot(al->benches[args->bench_idx], meas, svg_buf, f);
        break;
    case PLOT_KDE_EXT:
        kde_plot_ext(al->benches[args->bench_idx], meas, svg_buf, f);
        break;
    case PLOT_KDE_CMPG: {
        const struct group_analysis *a = al->group_analyses;
        const struct group_analysis *b = al->group_analyses + 1;
        kde_cmp_plot(al->benches[a->group->cmd_idxs[args->var_value_idx]],
                     al->benches[b->group->cmd_idxs[args->var_value_idx]], meas,
                     svg_buf, f);
        break;
    }
    case PLOT_KDE_CMP:
        kde_cmp_plot(al->benches[0], al->benches[1], meas, svg_buf, f);
        break;
    }
}

static bool dump_plot_walk(struct plot_walker_args *args) {
    char py_buf[4096];
    format_plot_name(py_buf, sizeof(py_buf), args, "py");
    FILE *py_file = fopen(py_buf, "w");
    if (py_file == NULL) {
        error("failed to create file %s", py_buf);
        return false;
    }
    write_make_plot(args, py_file);
    fclose(py_file);
    return true;
}

static bool dump_plot_src(const struct analysis *al) {
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        struct plot_walker_args args = {0};
        args.analysis = al->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(dump_plot_walk, &args))
            return false;
    }
    return true;
}

static bool make_plot_walk(struct plot_walker_args *args) {
    FILE *f;
    pid_t pid;
    if (!launch_python_stdin_pipe(&f, &pid)) {
        error("failed to launch python");
        return false;
    }
    write_make_plot(args, f);
    fclose(f);
    sb_push(args->pids, pid);
    return true;
}

static bool make_plots(const struct analysis *al) {
    bool success = true;
    struct plot_walker_args args = {0};
    for (size_t meas_idx = 0; meas_idx < al->meas_count && success;
         ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        args.analysis = al->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(make_plot_walk, &args))
            success = false;
    }
    for (size_t i = 0; i < sb_len(args.pids); ++i) {
        if (!process_finished_correctly(args.pids[i])) {
            error("python finished with non-zero exit code");
            success = false;
        }
    }
    sb_free(args.pids);
    return success;
}

static bool make_plots_readme(const struct analysis *al) {
    FILE *f = open_file_fmt("w", "%s/readme.md", g_out_dir);
    if (f == NULL) {
        error("failed to create file %s/readme.md", g_out_dir);
        return false;
    }
    fprintf(f, "# csbench analyze map\n");
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = al->meas + meas_idx;
        fprintf(f, "## measurement %s\n", meas->name);
        for (size_t grp_idx = 0; grp_idx < al->group_count; ++grp_idx) {
            const struct group_analysis *analysis =
                al->meas_analyses[meas_idx].group_analyses + grp_idx;
            fprintf(f,
                    "* [command group '%s' regression "
                    "plot](group_%zu_%zu.svg)\n",
                    analysis->group->name, grp_idx, meas_idx);
        }
        fprintf(f, "### KDE plots\n");
        fprintf(f, "#### regular\n");
        for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx) {
            const struct bench_analysis *analysis =
                al->bench_analyses + bench_idx;
            const char *cmd_str = analysis->name;
            fprintf(f, "* [%s](kde_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
        fprintf(f, "#### extended\n");
        for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx) {
            const struct bench_analysis *analysis =
                al->bench_analyses + bench_idx;
            const char *cmd_str = analysis->name;
            fprintf(f, "* [%s](kde_ext_%zu_%zu.svg)\n", cmd_str, bench_idx,
                    meas_idx);
        }
    }
    fclose(f);
    return true;
}

static void export_csv_raw_bench(const struct bench *bench,
                                 const struct analysis *al, FILE *f) {
    for (size_t i = 0; i < al->meas_count; ++i) {
        fprintf(f, "%s", al->meas[i].name);
        if (i != al->meas_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < bench->run_count; ++i) {
        for (size_t j = 0; j < al->meas_count; ++j) {
            fprintf(f, "%g", bench->meas[j][i]);
            if (j != al->meas_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_group_results(const struct meas_analysis *al, FILE *f) {
    assert(al->base->group_count > 0 && al->base->var);
    fprintf(f, "%s,", al->base->var->name);
    for (size_t i = 0; i < al->base->group_count; ++i) {
        char buf[4096];
        json_escape(buf, sizeof(buf), al->group_analyses[i].group->name);
        fprintf(f, "%s", buf);
        if (i != al->base->group_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < al->base->var->value_count; ++i) {
        fprintf(f, "%s,", al->base->var->values[i]);
        for (size_t j = 0; j < al->base->group_count; ++j) {
            fprintf(f, "%g", al->group_analyses[j].data[i].mean);
            if (j != al->base->group_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_bench_results(const struct analysis *al, size_t meas_idx,
                                     FILE *f) {
    fprintf(f,
            "cmd,mean_low,mean,mean_high,st_dev_low,st_dev,st_dev_high,min,max,"
            "median,q1,q3,p1,p5,p95,p99,outl\n");
    for (size_t i = 0; i < al->bench_count; ++i) {
        const struct distr *distr = al->bench_analyses[i].meas + meas_idx;
        char buf[4096];
        json_escape(buf, sizeof(buf), al->bench_analyses[i].name);
        fprintf(f, "%s,", buf);
        fprintf(f, "%g,%g,%g,%g,%g,%g,", distr->mean.lower, distr->mean.point,
                distr->mean.upper, distr->st_dev.lower, distr->st_dev.point,
                distr->st_dev.upper);
        fprintf(f, "%g,%g,%g,%g,%g,%g,%g,%g,%g,", distr->min, distr->max,
                distr->median, distr->q1, distr->q3, distr->p1, distr->p5,
                distr->p95, distr->p99);
        fprintf(f, "%g\n", distr->outliers.var);
    }
}

static bool export_csvs(const struct analysis *al) {
    char buf[4096];
    for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx) {
        snprintf(buf, sizeof(buf), "%s/bench_raw_%zu.csv", g_out_dir,
                 bench_idx);
        FILE *f = fopen(buf, "w");
        if (f == NULL)
            return false;
        export_csv_raw_bench(al->bench_analyses[bench_idx].bench, al, f);
        fclose(f);
    }
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        snprintf(buf, sizeof(buf), "%s/bench_%zu.csv", g_out_dir, meas_idx);
        FILE *f = fopen(buf, "w");
        if (f == NULL)
            return false;
        export_csv_bench_results(al, meas_idx, f);
        fclose(f);
    }
    if (al->group_count) {
        for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
            if (al->meas[meas_idx].is_secondary)
                continue;
            snprintf(buf, sizeof(buf), "%s/group_%zu.csv", g_out_dir, meas_idx);
            FILE *f = fopen(buf, "w");
            if (f == NULL)
                return false;
            export_csv_group_results(al->meas_analyses + meas_idx, f);
            fclose(f);
        }
    }
    return true;
}

static void html_estimate(const char *name, const struct est *est,
                          const struct units *units, FILE *f) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);
    fprintf(f,
            "<tr>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "<td>%s</td>"
            "<td class=\"est-bound\">%s</td>"
            "</tr>",
            name, buf1, buf2, buf3);
}

static void html_outliers(const struct outliers *outliers, size_t run_count,
                          FILE *f) {
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
            "estimated "
            "standard deviation</p>",
            outliers_variance_str(outliers->var), outliers->var * 100.0);
}

static void html_distr(const struct bench_analysis *analysis, size_t bench_idx,
                       size_t meas_idx, const struct analysis *al, FILE *f) {
    const struct distr *distr = analysis->meas + meas_idx;
    const struct bench *bench = analysis->bench;
    const struct meas *info = al->meas + meas_idx;
    assert(!info->is_secondary);
    fprintf(f,
            "<div class=\"row\">"
            "<div class=\"col\"><h3>%s kde plot</h3>"
            "<a href=\"kde_ext_%zu_%zu.svg\"><img "
            "src=\"kde_%zu_%zu.svg\"></a></div>",
            info->name, bench_idx, meas_idx, bench_idx, meas_idx);
    fprintf(f,
            "<div class=\"col\"><h3>statistics</h3>"
            "<div class=\"stats\">"
            "<p>%zu runs</p>",
            bench->run_count);
    char buf[256];
    format_meas(buf, sizeof(buf), distr->min, &info->units);
    fprintf(f, "<p>min %s</p>", buf);
    format_meas(buf, sizeof(buf), distr->max, &info->units);
    fprintf(f, "<p>max %s</p>", buf);
    fprintf(f, "<table><thead><tr>"
               "<th></th>"
               "<th class=\"est-bound\">lower bound</th>"
               "<th class=\"est-bound\">estimate</th>"
               "<th class=\"est-bound\">upper bound</th>"
               "</tr></thead><tbody>");
    html_estimate("mean", &distr->mean, &info->units, f);
    html_estimate("st dev", &distr->st_dev, &info->units, f);
    for (size_t j = 0; j < al->meas_count; ++j) {
        if (al->meas[j].is_secondary && al->meas[j].primary_idx == meas_idx)
            html_estimate(al->meas[j].name, &analysis->meas[j].mean,
                          &al->meas->units, f);
    }
    fprintf(f, "</tbody></table>");
    html_outliers(&distr->outliers, bench->run_count, f);
    fprintf(f, "</div></div></div>");
}

static void html_compare(const struct analysis *al, FILE *f) {
    if (al->bench_count == 1)
        return;
    fprintf(f, "<div><h2>measurement comparison</h2>");
    size_t meas_count = al->meas_count;
    for (size_t meas_idx = 0; meas_idx < meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        const struct meas *meas = al->meas + meas_idx;
        fprintf(f,
                "<div><h3>%s comparison</h3>"
                "<div class=\"row\"><div class=\"col\">"
                "<img src=\"bar_%zu.svg\"></div>",
                meas->name, meas_idx);
        if (al->bench_count == 2)
            fprintf(f,
                    "<div class=\"col\">"
                    "<img src=\"kde_cmp_%zu.svg\"></div>",
                    meas_idx);
        fprintf(f, "</div></div></div>");
    }
    fprintf(f, "</div>");
}

static void html_bench_group(const struct group_analysis *al,
                             const struct meas *meas, size_t meas_idx,
                             size_t grp_idx, const struct bench_var *var,
                             FILE *f) {
    fprintf(f,
            "<h4>measurement %s</h4>"
            "<div class=\"row\"><div class=\"col\">"
            "<img src=\"group_%zu_%zu.svg\"></div>",
            meas->name, grp_idx, meas_idx);
    char buf[256];
    format_time(buf, sizeof(buf), al->fastest->mean);
    fprintf(f,
            "<div class=\"col stats\">"
            "<p>lowest time %s with %s=%s</p>",
            buf, var->name, al->fastest->value);
    format_time(buf, sizeof(buf), al->slowest->mean);
    fprintf(f, "<p>hightest time %s with %s=%s</p>", buf, var->name,
            al->slowest->value);
    if (al->values_are_doubles) {
        fprintf(f,
                "<p>mean time is most likely %s in terms of parameter</p>"
                "<p>linear coef %g rms %.3f</p>",
                big_o_str(al->regress.complexity), al->regress.a,
                al->regress.rms);
    }
    fprintf(f, "</div></div>");
}

static void html_var_analysis(const struct analysis *al, FILE *f) {
    if (!al->group_count)
        return;
    fprintf(f, "<div><h2>variable analysis</h2>");
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        if (al->group_count > 1)
            fprintf(f,
                    "<div><h3>summary for %s</h3>"
                    "<div class=\"row\"><div class=\"col\">"
                    "<img src=\"group_%zu.svg\"></div>"
                    "<div class=\"col\">"
                    "<img src=\"group_bar_%zu.svg\">"
                    "</div></div></div></div>",
                    al->meas[meas_idx].name, meas_idx, meas_idx);
        for (size_t grp_idx = 0; grp_idx < al->group_count; ++grp_idx) {
            fprintf(f, "<div><h3>group '%s' with value %s</h3>",
                    al->meas_analyses[0].group_analyses[grp_idx].group->name,
                    al->var->name);
            const struct meas *meas = al->meas + meas_idx;
            const struct group_analysis *analysis =
                al->meas_analyses[meas_idx].group_analyses + grp_idx;
            html_bench_group(analysis, meas, meas_idx, grp_idx, al->var, f);
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</div>");
}

static void html_report(const struct analysis *al, FILE *f) {
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

    html_var_analysis(al, f);
    html_compare(al, f);
    for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx) {
        const struct bench_analysis *analysis = al->bench_analyses + bench_idx;
        fprintf(f, "<div><h2>command '%s'</h2>", analysis->name);
        for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
            const struct meas *info = al->meas + meas_idx;
            if (info->is_secondary)
                continue;
            html_distr(analysis, bench_idx, meas_idx, al, f);
        }
        fprintf(f, "</div>");
    }
    fprintf(f, "</body>");
}

static bool make_html_report(const struct analysis *al) {
    FILE *f = open_file_fmt("w", "%s/index.html", g_out_dir);
    if (f == NULL) {
        error("failed to create file %s/index.html", g_out_dir);
        return false;
    }
    html_report(al, f);
    fclose(f);
    return true;
}

static bool do_visualize(const struct analysis *al) {
    if (!do_export(al))
        return false;

    if (!g_plot && !g_html && !g_csv)
        return true;

    if (mkdir(g_out_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            csperror("mkdir");
            return false;
        }
    }

    if (g_plot) {
        if (!python_found()) {
            error("failed to find python3 executable");
            return false;
        }
        if (!python_has_matplotlib()) {
            error("python does not have matplotlib installed");
            return false;
        }

        if (g_plot_src && !dump_plot_src(al))
            return false;
        if (!make_plots(al))
            return false;
        if (!make_plots_readme(al))
            return false;
    }

    if (g_csv && !export_csvs(al))
        return false;

    if (g_html && !make_html_report(al))
        return false;

    return true;
}

static void print_exit_code_info(const struct bench *bench) {
    if (!bench->exit_codes)
        return;

    size_t count_nonzero = 0;
    for (size_t i = 0; i < bench->run_count; ++i)
        if (bench->exit_codes[i] != 0)
            ++count_nonzero;

    assert(g_ignore_failure ? 1 : count_nonzero == 0);
    if (count_nonzero == bench->run_count) {
        printf("all commands have non-zero exit code: %d\n",
               bench->exit_codes[0]);
    } else if (count_nonzero != 0) {
        printf("some runs (%zu) have non-zero exit code\n", count_nonzero);
    }
}

static void print_outliers(const struct outliers *outliers, size_t run_count) {
    int outlier_count = outliers->low_mild + outliers->high_mild +
                        outliers->low_severe + outliers->high_severe;
    if (outlier_count != 0) {
        printf("%d outliers (%.2f%%) %s (%.1f%%) effect on st dev\n",
               outlier_count, (double)outlier_count / run_count * 100.0,
               outliers_variance_str(outliers->var), outliers->var * 100.0);
        if (outliers->low_severe)
            printf("  %d (%.2f%%) low severe\n", outliers->low_severe,
                   (double)outliers->low_severe / run_count * 100.0);
        if (outliers->low_mild)
            printf("  %d (%.2f%%) low mild\n", outliers->low_mild,
                   (double)outliers->low_mild / run_count * 100.0);
        if (outliers->high_mild)
            printf("  %d (%.2f%%) high mild\n", outliers->high_mild,
                   (double)outliers->high_mild / run_count * 100.0);
        if (outliers->high_severe)
            printf("  %d (%.2f%%) high severe\n", outliers->high_severe,
                   (double)outliers->high_severe / run_count * 100.0);
    } else {
        printf("outliers have %s (%.1f%%) effect on st dev\n",
               outliers_variance_str(outliers->var), outliers->var * 100.0);
    }
}

static void print_estimate(const char *name, const struct est *est,
                           const struct units *units, const char *prim_color,
                           const char *sec_color) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);

    printf_colored(prim_color, "%7s", name);
    printf_colored(sec_color, " %8s ", buf1);
    printf_colored(prim_color, "%8s", buf2);
    printf_colored(sec_color, " %8s\n", buf3);
}

static void print_distr(const struct distr *dist, const struct units *units) {
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), dist->min, units);
    format_meas(buf2, sizeof(buf2), dist->median, units);
    format_meas(buf3, sizeof(buf3), dist->max, units);
    printf_colored(ANSI_BOLD_MAGENTA, " q{024} ");
    printf_colored(ANSI_MAGENTA, "%s ", buf1);
    printf_colored(ANSI_BOLD_MAGENTA, "%s ", buf2);
    printf_colored(ANSI_MAGENTA, "%s\n", buf3);
    print_estimate("mean", &dist->mean, units, ANSI_BOLD_GREEN,
                   ANSI_BRIGHT_GREEN);
    print_estimate("st dev", &dist->st_dev, units, ANSI_BOLD_GREEN,
                   ANSI_BRIGHT_GREEN);
}

static void print_benchmark_info(const struct bench_analysis *cur,
                                 const struct analysis *al) {
    const struct bench *bench = cur->bench;
    printf("command ");
    printf_colored(ANSI_BOLD, "%s\n", cur->name);
    // Print runs count only if it not explicitly specified, otherwise it is
    // printed in 'print_analysis'
    if (g_bench_stop.runs == 0)
        printf("%zu runs\n", bench->run_count);
    print_exit_code_info(bench);
    if (al->primary_meas_count != 0) {
        for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
            const struct meas *meas = al->meas + meas_idx;
            if (meas->is_secondary)
                continue;

            if (al->primary_meas_count != 1) {
                printf("measurement ");
                printf_colored(ANSI_YELLOW, "%s\n", meas->name);
            }
            const struct distr *distr = cur->meas + meas_idx;
            print_distr(distr, &meas->units);
            for (size_t j = 0; j < al->meas_count; ++j) {
                if (al->meas[j].is_secondary &&
                    al->meas[j].primary_idx == meas_idx)
                    print_estimate(al->meas[j].name, &cur->meas[j].mean,
                                   &al->meas[j].units, ANSI_BOLD_BLUE,
                                   ANSI_BRIGHT_BLUE);
            }
            print_outliers(&distr->outliers, bench->run_count);
        }
    } else {
        for (size_t i = 0; i < al->meas_count; ++i) {
            const struct meas *info = al->meas + i;
            print_estimate(info->name, &cur->meas[i].mean, &info->units,
                           ANSI_BOLD_BLUE, ANSI_BRIGHT_BLUE);
        }
    }
}

static void print_cmd_comparison(const struct meas_analysis *al) {
    if (al->base->bench_count == 1)
        return;

    if (al->base->primary_meas_count != 1) {
        printf("measurement ");
        printf_colored(ANSI_YELLOW, "%s\n", al->meas->name);
    }

    if (al->base->group_count <= 1) {
        const struct bench_analysis *reference;
        if (g_baseline != -1) {
            printf("baseline command ");
            reference = al->base->bench_analyses + g_baseline;
        } else {
            printf("fastest command ");
            reference = al->base->bench_analyses + al->fastest[0];
        }
        printf_colored(ANSI_BOLD, "%s\n", reference->name);
        for (size_t j = 0; j < al->base->bench_count; ++j) {
            size_t bench_idx = j;
            if (g_baseline == -1)
                bench_idx = al->fastest[j];
            const struct bench_analysis *bench =
                al->base->bench_analyses + bench_idx;
            if (bench == reference)
                continue;
            const struct point_err_est *speedup = al->speedup + bench_idx;
            if (g_baseline != -1)
                printf_colored(ANSI_BOLD, "  %s", bench->name);
            else
                printf_colored(ANSI_BOLD, "  %s", reference->name);
            printf(" is ");
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->err);
            printf(" times faster than ");
            if (g_baseline == -1)
                printf_colored(ANSI_BOLD, "%s", bench->name);
            else
                printf("baseline");
            printf(" (p=%.2f)", al->p_values[bench_idx]);
            printf("\n");
        }
        if (al->base->group_count == 1 && g_regr) {
            const struct group_analysis *grp = al->group_analyses;
            if (grp->values_are_doubles)
                printf("%s complexity (%g)\n",
                       big_o_str(grp->regress.complexity), grp->regress.a);
        }
    } else {
        for (size_t grp_idx = 0; grp_idx < al->base->group_count; ++grp_idx) {
            printf("%c = ", (int)('A' + grp_idx));
            printf_colored(ANSI_BOLD, "%s",
                           al->group_analyses[grp_idx].group->name);
            if (g_baseline == (int)grp_idx)
                printf(" (baseline)");
            printf("\n");
        }

        const struct bench_var *var = al->base->var;
        size_t value_count = var->value_count;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            const char *value = var->values[val_idx];
            size_t reference_idx;
            if (g_baseline == -1)
                reference_idx = al->fastest_val[val_idx];
            else
                reference_idx = g_baseline;

            printf("%s=%s:\t", var->name, value);
            if (g_baseline == -1) {
                printf("%c is ", (int)('A' + reference_idx));
            }
            const char *ident = "";
            if (al->base->group_count > 2) {
                printf("\n");
                ident = "  ";
            }
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                if (grp_idx == reference_idx)
                    continue;
                const struct point_err_est *est =
                    al->var_speedup[val_idx] + grp_idx;
                printf("%s", ident);
                if (g_baseline != -1)
                    printf("%c is ", (int)('A' + grp_idx));
                printf_colored(ANSI_BOLD_GREEN, "%6.3f", est->point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", est->err);
                printf(" times faster than ");
                if (g_baseline == -1)
                    printf("%c", (int)('A' + grp_idx));
                else
                    printf("baseline");
                printf(" (p=%.2f)", al->var_p_values[val_idx][grp_idx]);
                printf("\n");
            }
        }
        if (g_regr) {
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                const struct group_analysis *grp = al->group_analyses + grp_idx;
                if (grp->values_are_doubles) {
                    printf_colored(ANSI_BOLD, "%s ", grp->group->name);
                    printf("%s complexity (%g)\n",
                           big_o_str(grp->regress.complexity), grp->regress.a);
                }
            }
        } else if (g_baseline != -1 && al->base->group_count > 1) {
            printf("on average ");
            const char *ident = "";
            if (al->base->group_count > 2) {
                printf("\n");
                ident = "  ";
            }
            for (size_t grp_idx = 0; grp_idx < al->base->group_count;
                 ++grp_idx) {
                if (grp_idx == (size_t)g_baseline)
                    continue;
                const struct point_err_est *est =
                    al->group_baseline_speedup + grp_idx;
                printf("%s%c is ", ident, (int)('A' + grp_idx));
                printf_colored(ANSI_BOLD_GREEN, "%.3f", est->point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", est->err);
                printf(" times faster than baseline\n");
            }
        }
    }
}

static void print_analysis(const struct analysis *al) {
    if (g_bench_stop.runs != 0)
        printf("%d runs\n", g_bench_stop.runs);
    if (al->primary_meas_count == 1) {
        const struct meas *meas = NULL;
        for (size_t i = 0; i < al->meas_count && meas == NULL; ++i)
            if (!al->meas[i].is_secondary)
                meas = al->meas + i;
        assert(meas != NULL);
        printf("measurement ");
        printf_colored(ANSI_YELLOW, "%s\n", meas->name);
    }
    for (size_t i = 0; i < al->bench_count; ++i)
        print_benchmark_info(al->bench_analyses + i, al);

    for (size_t i = 0; i < al->meas_count; ++i) {
        const struct meas *meas = al->meas + i;
        if (meas->is_secondary)
            continue;
        print_cmd_comparison(al->meas_analyses + i);
    }
}

bool make_report(const struct analysis *al) {
    print_analysis(al);
    return do_visualize(al);
}
