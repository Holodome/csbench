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
#include <string.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum plot_kind {
    PLOT_BAR,
    PLOT_GROUP_BAR,
    PLOT_GROUP_REGR,
    PLOT_ALL_GROUPS_REGR,
    PLOT_KDE_SMALL,
    PLOT_KDE,
    PLOT_KDE_CMP_SMALL,
    PLOT_KDE_CMP,
    PLOT_KDE_CMP_ALL_GROUPS,
    PLOT_KDE_CMP_PER_VAL,
    PLOT_KDE_CMP_PER_VAL_SMALL,
};

struct plot_walker_args {
    const struct meas_analysis *analysis;
    enum plot_kind plot_kind;
    pid_t *pids;
    const char **cmds;
    size_t meas_idx;
    size_t bench_idx;
    size_t grp_idx;
    size_t val_idx;
    size_t compared_idx;
    struct plot_maker plot_maker;
};

static bool json_escape(char *buf, size_t buf_size, const char *src)
{
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

static bool export_json(const struct analysis *al, const char *filename)
{
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
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct bench_analysis *analysis = bench_analyses + bench_idx;
        const struct bench *bench = analysis->bench;
        fprintf(f, "{ ");
        if (g_prepare)
            json_escape(buf, sizeof(buf), g_prepare);
        else
            *buf = '\0';
        fprintf(f, "\"prepare\": \"%s\", ", buf);
        json_escape(buf, sizeof(buf), bench_name(al, bench_idx));
        fprintf(f, "\"command\": \"%s\", ", buf);
        size_t run_count = bench->run_count;
        fprintf(f, "\"run_count\": %zu, ", bench->run_count);
        fprintf(f, "\"exit_codes\": [");
        for (size_t j = 0; j < run_count; ++j)
            fprintf(f, "%d%s", bench->exit_codes[j],
                    j != run_count - 1 ? ", " : "");
        fprintf(f, "], \"meas\": [");
        for (size_t j = 0; j < al->meas_count; ++j) {
            const struct meas *meas = al->meas + j;
            json_escape(buf, sizeof(buf), meas->name);
            fprintf(f, "{ \"name\": \"%s\", ", buf);
            json_escape(buf, sizeof(buf), units_str(&meas->units));
            fprintf(f, "\"units\": \"%s\",", buf);
            json_escape(buf, sizeof(buf), meas->cmd);
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
        if (bench_idx != bench_count - 1)
            fprintf(f, ", ");
    }
    fprintf(f, "]}\n");
    fclose(f);
    return true;
}

static bool plot_walker(bool (*walk)(struct plot_walker_args *args),
                        struct plot_walker_args *args)
{
    const struct meas_analysis *al = args->analysis;
    const struct analysis *base = al->base;
    size_t bench_count = base->bench_count;
    size_t grp_count = base->group_count;
    if ((g_desired_plots & MAKE_PLOT_BAR) && bench_count > 1) {
        if (grp_count <= 1) {
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
        if ((g_desired_plots & MAKE_PLOT_ALL_GROUPS_REGR) &&
            base->group_count > 1) {
            const struct group_analysis *grp = al->group_analyses;
            if (grp[0].values_are_doubles) {
                args->plot_kind = PLOT_ALL_GROUPS_REGR;
                if (!walk(args))
                    return false;
            }
        }
        if (g_desired_plots & MAKE_PLOT_GROUP_REGR) {
            for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
                const struct group_analysis *grp = al->group_analyses + grp_idx;
                if (!grp->values_are_doubles)
                    break;
                args->plot_kind = PLOT_GROUP_REGR;
                args->grp_idx = grp_idx;
                if (!walk(args))
                    return false;
            }
        }
    }
    foreach_bench_idx (bench_idx, al) {
        args->plot_kind = PLOT_KDE_SMALL;
        args->bench_idx = bench_idx;
        if ((g_desired_plots & MAKE_PLOT_KDE_SMALL) && !walk(args))
            return false;
        args->plot_kind = PLOT_KDE;
        args->bench_idx = bench_idx;
        if ((g_desired_plots & MAKE_PLOT_KDE) && !walk(args))
            return false;
    }
    if (grp_count <= 1) {
        size_t reference_idx = al->bench_speedups_reference;
        foreach_bench_idx (bench_idx, al) {
            if (bench_idx == reference_idx)
                continue;
            args->compared_idx = bench_idx;
            args->plot_kind = PLOT_KDE_CMP_SMALL;
            if ((g_desired_plots & MAKE_PLOT_KDE_CMP_SMALL) && !walk(args))
                return false;
            args->plot_kind = PLOT_KDE_CMP;
            if ((g_desired_plots & MAKE_PLOT_KDE_CMP) && !walk(args))
                return false;
        }
    } else {
        const struct bench_var *var = base->var;
        size_t val_count = var->value_count;
        for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
            size_t reference_idx = al->val_bench_speedups_references[val_idx];
            foreach_per_val_group_idx (grp_idx, val_idx, al) {
                if (grp_idx == reference_idx)
                    continue;
                args->compared_idx = grp_idx;
                args->val_idx = val_idx;
                args->plot_kind = PLOT_KDE_CMP_PER_VAL;
                if ((g_desired_plots & MAKE_PLOT_KDE_CMP_PER_VAL) &&
                    !walk(args))
                    return false;
                args->plot_kind = PLOT_KDE_CMP_PER_VAL_SMALL;
                if ((g_desired_plots & MAKE_PLOT_KDE_CMP_PER_VAL_SMALL) &&
                    !walk(args))
                    return false;
            }
        }

        size_t reference_idx = al->groups_avg_reference;
        foreach_group_by_avg_idx (grp_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            args->plot_kind = PLOT_KDE_CMP_ALL_GROUPS;
            args->compared_idx = grp_idx;
            if ((g_desired_plots & MAKE_PLOT_KDE_CMP_ALL_GROUPS) && !walk(args))
                return false;
        }
    }
    return true;
}

static void format_plot_name(char *buf, size_t buf_size,
                             const struct plot_walker_args *args,
                             const char *extension)
{
    switch (args->plot_kind) {
    case PLOT_BAR:
        snprintf(buf, buf_size, "%s/bar_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_GROUP_BAR:
        snprintf(buf, buf_size, "%s/group_bar_%zu.%s", g_out_dir,
                 args->meas_idx, extension);
        break;
    case PLOT_GROUP_REGR:
        snprintf(buf, buf_size, "%s/group_%zu_%zu.%s", g_out_dir, args->grp_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_ALL_GROUPS_REGR:
        snprintf(buf, buf_size, "%s/groups_%zu.%s", g_out_dir, args->meas_idx,
                 extension);
        break;
    case PLOT_KDE_SMALL:
        snprintf(buf, buf_size, "%s/kde_small_%zu_%zu.%s", g_out_dir,
                 args->bench_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE:
        snprintf(buf, buf_size, "%s/kde_%zu_%zu.%s", g_out_dir, args->bench_idx,
                 args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP_ALL_GROUPS:
        snprintf(buf, buf_size, "%s/kde_cmp_all_groups_%zu_%zu.%s", g_out_dir,
                 args->compared_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP_SMALL:
        snprintf(buf, buf_size, "%s/kde_cmp_small_%zu_%zu.%s", g_out_dir,
                 args->compared_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP:
        snprintf(buf, buf_size, "%s/kde_cmp_%zu_%zu.%s", g_out_dir,
                 args->compared_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP_PER_VAL:
        snprintf(buf, buf_size, "%s/kde_pval_cmp_%zu_%zu_%zu.%s", g_out_dir,
                 args->compared_idx, args->val_idx, args->meas_idx, extension);
        break;
    case PLOT_KDE_CMP_PER_VAL_SMALL:
        snprintf(buf, buf_size, "%s/kde_pval_cmp_small_%zu_%zu_%zu.%s",
                 g_out_dir, args->compared_idx, args->val_idx, args->meas_idx,
                 extension);
        break;
    }
}

static void make_plot_src(struct plot_walker_args *args, FILE *f)
{
    const struct meas_analysis *al = args->analysis;
    const struct analysis *base = al->base;
    const struct meas *meas = al->meas;
    const struct plot_maker *plot_maker = &args->plot_maker;
    char svg_buf[4096];
    format_plot_name(svg_buf, sizeof(svg_buf), args, "svg");
    struct plot_maker_ctx ctx = {0};
    ctx.image_filename = svg_buf;
    ctx.f = f;
    switch (args->plot_kind) {
    case PLOT_BAR:
        plot_maker->bar(al, &ctx);
        break;
    case PLOT_GROUP_BAR:
        plot_maker->group_bar(al, &ctx);
        break;
    case PLOT_GROUP_REGR:
        plot_maker->group_regr(al, args->grp_idx, &ctx);
        break;
    case PLOT_ALL_GROUPS_REGR:
        plot_maker->group_regr(al, (size_t)-1, &ctx);
        break;
    case PLOT_KDE_SMALL:
        plot_maker->kde_small(al->benches[args->bench_idx], meas, &ctx);
        break;
    case PLOT_KDE:
        plot_maker->kde(al->benches[args->bench_idx], meas,
                        bench_name(base, args->bench_idx), &ctx);
        break;
    case PLOT_KDE_CMP_SMALL:
        plot_maker->kde_cmp_small(al, args->compared_idx, &ctx);
        break;
    case PLOT_KDE_CMP:
        plot_maker->kde_cmp(al, args->compared_idx, &ctx);
        break;
    case PLOT_KDE_CMP_ALL_GROUPS:
        plot_maker->kde_cmp_group(al, args->compared_idx, &ctx);
        break;
    case PLOT_KDE_CMP_PER_VAL:
        plot_maker->kde_cmp_per_val(al, args->compared_idx, args->val_idx,
                                    &ctx);
        break;
    case PLOT_KDE_CMP_PER_VAL_SMALL:
        plot_maker->kde_cmp_per_val_small(al, args->compared_idx, args->val_idx,
                                          &ctx);
        break;
    }
}

static bool make_plot_src_walk(struct plot_walker_args *args)
{
    char src_buf[4096];
    format_plot_name(src_buf, sizeof(src_buf), args,
                     args->plot_maker.src_extension);
    FILE *src_file = fopen(src_buf, "w");
    if (src_file == NULL) {
        error("failed to create file %s", src_buf);
        return false;
    }
    make_plot_src(args, src_file);
    fclose(src_file);
    return true;
}

static bool make_plot_srcs(const struct analysis *al, enum plot_backend backend)
{
    struct plot_walker_args args = {0};
    init_plot_maker(backend, &args.plot_maker);
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        args.analysis = al->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(make_plot_src_walk, &args))
            return false;
    }
    return true;
}

static bool make_plot_walk(struct plot_walker_args *args)
{
    int stdout_fd = -1;
    int stderr_fd = -1;
    if (g_plot_debug) {
        stdout_fd = STDOUT_FILENO;
        stderr_fd = STDERR_FILENO;
    }

    pid_t pid;
    char src_buf[4096];
    format_plot_name(src_buf, sizeof(src_buf), args,
                     args->plot_maker.src_extension);
    const char *cmd = NULL;
    switch (args->plot_maker.backend) {
    case PLOT_BACKEND_MATPLOTLIB:
        cmd = csfmt("%s %s", g_python_executable, src_buf);
        break;
    case PLOT_BACKEND_GNUPLOT:
        cmd = csfmt("gnuplot %s", src_buf);
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    if (!shell_launch(cmd, -1, stdout_fd, stderr_fd, &pid))
        return false;
    sb_push(args->pids, pid);
    sb_push(args->cmds, cmd);
    return true;
}

static bool make_plots(const struct analysis *al,
                       enum plot_backend plot_backend)
{
    bool success = true;
    struct plot_walker_args args = {0};
    init_plot_maker(plot_backend, &args.plot_maker);
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
        if (!process_wait_finished_correctly(args.pids[i], true)) {
            error("'%s' finished with non-zero exit code", args.cmds[i]);
            success = false;
        }
    }
    sb_free(args.pids);
    return success;
}

static bool delete_plot_src_walk(struct plot_walker_args *args)
{
    char src_buf[4096];
    format_plot_name(src_buf, sizeof(src_buf), args,
                     args->plot_maker.src_extension);
    if (remove(src_buf) != 0) {
        csfmtperror("failed to delete file '%s'", src_buf);
        return false;
    }
    return true;
}

static bool delete_plot_srcs(const struct analysis *al,
                             enum plot_backend backend)
{
    struct plot_walker_args args = {0};
    init_plot_maker(backend, &args.plot_maker);
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        args.analysis = al->meas_analyses + meas_idx;
        args.meas_idx = meas_idx;
        if (!plot_walker(delete_plot_src_walk, &args))
            return false;
    }
    return true;
}

static void make_plots_map_meas(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    size_t grp_count = base->group_count;
    size_t bench_count = base->bench_count;
    size_t meas_idx = al->meas_idx;
    fprintf(f, "## measurement %s\n", al->meas->name);
    if ((g_desired_plots & MAKE_PLOT_BAR) && bench_count > 1) {
        if (grp_count <= 1)
            fprintf(f, "- [bar plot](bar_%zu.svg)\n", meas_idx);
        else
            fprintf(f, "- [bar plot](group_bar_%zu.svg)\n", meas_idx);
    }
    if (g_regr && grp_count > 1) {
        fprintf(f, "### regrssion plots\n");
        if (g_desired_plots & MAKE_PLOT_ALL_GROUPS_REGR) {
            const struct group_analysis *grp = al->group_analyses;
            if (grp[0].values_are_doubles) {
                fprintf(f,
                        "- [comparison and regression of all "
                        "groups](groups_%zu.svg)\n",
                        meas_idx);
            }
        }
        if (g_desired_plots & MAKE_PLOT_GROUP_REGR) {
            fprintf(f, "#### group regrssion plots\n");
            for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
                const struct group_analysis *grp = al->group_analyses + grp_idx;
                if (!grp->values_are_doubles)
                    break;
                fprintf(f, "- [group %s regression plot](group_%zu_%zu.svg)\n",
                        bench_group_name(base, grp_idx), grp_idx, meas_idx);
            }
        }
    }
    if (g_desired_plots & MAKE_PLOT_KDE_SMALL) {
        fprintf(f, "### benchmark KDE (small)\n");
        foreach_bench_idx (bench_idx, al) {
            fprintf(f, "- [benchmark %s KDE (small)](kde_small_%zu_%zu.svg)\n",
                    bench_name(base, bench_idx), bench_idx, meas_idx);
        }
    }
    if (g_desired_plots & MAKE_PLOT_KDE) {
        fprintf(f, "### benchmark KDE\n");
        foreach_bench_idx (bench_idx, al) {
            fprintf(f, "- [benchmark %s KDE](kde_%zu_%zu.svg)\n",
                    bench_name(base, bench_idx), bench_idx, meas_idx);
        }
    }
    if (grp_count <= 1) {
        size_t reference_idx = al->bench_speedups_reference;
        const char *reference_name = bench_name(base, reference_idx);
        if (g_desired_plots & MAKE_PLOT_KDE_CMP_SMALL) {
            fprintf(f, "### benchmark KDE comparison (small)\n");
            foreach_bench_idx (bench_idx, al) {
                if (bench_idx == reference_idx)
                    continue;
                fprintf(f,
                        "- [%s vs %s KDE comparison "
                        "(small)](kde_cmp_small_%zu_%zu.svg)\n",
                        reference_name, bench_name(base, bench_idx), bench_idx,
                        meas_idx);
            }
        }
        if (g_desired_plots & MAKE_PLOT_KDE_CMP) {
            fprintf(f, "### benchmark KDE comparison\n");
            foreach_bench_idx (bench_idx, al) {
                if (bench_idx == reference_idx)
                    continue;
                fprintf(f,
                        "- [%s vs %s KDE "
                        "comparison](kde_cmp_%zu_%zu.svg)\n",
                        reference_name, bench_name(base, bench_idx), bench_idx,
                        meas_idx);
            }
        }
    } else if (g_desired_plots &
               (MAKE_PLOT_KDE_CMP_PER_VAL | MAKE_PLOT_KDE_CMP_PER_VAL_SMALL |
                MAKE_PLOT_KDE_CMP_ALL_GROUPS)) {
        const struct bench_var *var = base->var;
        size_t val_count = var->value_count;
        if (g_desired_plots & MAKE_PLOT_KDE_CMP_PER_VAL_SMALL) {
            fprintf(f, "### benchmark KDE comparison (small)\n");
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
                size_t reference_idx =
                    al->val_bench_speedups_references[val_idx];
                const char *reference_name =
                    bench_group_name(base, reference_idx);
                fprintf(f, "#### %s=%s\n", var->name, var->values[val_idx]);
                foreach_per_val_group_idx (grp_idx, val_idx, al) {
                    if (grp_idx == reference_idx)
                        continue;
                    fprintf(f,
                            "- [%s vs %s KDE comparison "
                            "(small)](kde_pval_cmp_small_%zu_%zu_%zu.svg)\n",
                            reference_name, bench_group_name(base, grp_idx),
                            grp_idx, val_idx, meas_idx);
                }
            }
        }
        if (g_desired_plots & MAKE_PLOT_KDE_CMP_PER_VAL) {
            fprintf(f, "### benchmark KDE comparison\n");
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
                size_t reference_idx =
                    al->val_bench_speedups_references[val_idx];
                const char *reference_name =
                    bench_group_name(base, reference_idx);
                fprintf(f, "#### %s=%s\n", var->name, var->values[val_idx]);
                foreach_per_val_group_idx (grp_idx, val_idx, al) {
                    if (grp_idx == reference_idx)
                        continue;
                    fprintf(f,
                            "- [%s vs %s KDE "
                            "comparison](kde_pval_cmp_%zu_%zu_%zu.svg)\n",
                            reference_name, bench_group_name(base, grp_idx),
                            grp_idx, val_idx, meas_idx);
                }
            }
        }
        if (g_desired_plots & MAKE_PLOT_KDE_CMP_ALL_GROUPS) {
            size_t reference_idx = al->groups_avg_reference;
            const char *reference_name = bench_group_name(base, reference_idx);
            fprintf(f, "### groups comparison\n");
            foreach_group_by_avg_idx (grp_idx, al) {
                if (grp_idx == reference_idx)
                    continue;
                fprintf(f,
                        "- [%s vs %s KDE comparison "
                        "aggregation](kde_cmp_all_groups_%zu_%zu.svg)\n",
                        reference_name, bench_group_name(base, grp_idx),
                        grp_idx, meas_idx);
            }
        }
    }
}

static bool make_plots_map(const struct analysis *al)
{
    FILE *f = open_file_fmt("w", "%s/plots_map.md", g_out_dir);
    if (f == NULL) {
        error("failed to create file %s/plots_map.md", g_out_dir);
        return false;
    }
    fprintf(f, "# csbench plot map\n");
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        const struct meas_analysis *mal = al->meas_analyses + meas_idx;
        make_plots_map_meas(mal, f);
    }
    fclose(f);
    return true;
}

static void export_csv_bench_raw(const struct bench *bench,
                                 const struct analysis *al, FILE *f)
{
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        fprintf(f, "%s", al->meas[meas_idx].name);
        if (meas_idx != al->meas_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t run_idx = 0; run_idx < bench->run_count; ++run_idx) {
        for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
            fprintf(f, "%g", bench->meas[meas_idx][run_idx]);
            if (meas_idx != al->meas_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_group(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    assert(base->group_count > 0 && base->var);
    fprintf(f, "%s,", base->var->name);
    for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
        char buf[4096];
        json_escape(buf, sizeof(buf), bench_group_name(base, grp_idx));
        fprintf(f, "%s", buf);
        if (grp_idx != base->group_count - 1)
            fprintf(f, ",");
    }
    fprintf(f, "\n");
    for (size_t val_idx = 0; val_idx < base->var->value_count; ++val_idx) {
        fprintf(f, "%s,", base->var->values[val_idx]);
        for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
            fprintf(f, "%g", al->group_analyses[grp_idx].data[val_idx].mean);
            if (grp_idx != base->group_count - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
}

static void export_csv_bench_stats(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    fprintf(f,
            "cmd,mean_low,mean,mean_high,st_dev_low,st_dev,st_dev_high,min,max,"
            "median,q1,q3,p1,p5,p95,p99,outl\n");
    for (size_t bench_idx = 0; bench_idx < base->bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        char buf[4096];
        json_escape(buf, sizeof(buf), bench_name(base, bench_idx));
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

static void export_csv_group_raw(const struct meas_analysis *al, size_t grp_idx,
                                 FILE *f)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    const struct group_analysis *group = al->group_analyses + grp_idx;
    size_t val_count = var->value_count;
    for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
        const struct distr *distr = group->data[val_idx].distr;
        fprintf(f, "%s=%s", var->name, var->values[val_idx]);
        for (size_t run_idx = 0; run_idx < distr->count; ++run_idx)
            fprintf(f, ",%g", distr->data[run_idx]);
        fprintf(f, "\n");
    }
}

static void export_csv_benches_raw(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    size_t bench_count = base->bench_count;
    for (size_t bench_idx = 0; bench_idx < bench_count; ++bench_idx) {
        const struct distr *distr = al->benches[bench_idx];
        fprintf(f, "%s", bench_name(base, bench_idx));
        for (size_t run_idx = 0; run_idx < distr->count; ++run_idx)
            fprintf(f, ",%g", distr->data[run_idx]);
        fprintf(f, "\n");
    }
}

static bool export_csvs(const struct analysis *al)
{
    for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx) {
        FILE *f =
            open_file_fmt("w", "%s/bench_raw_%zu.csv", g_out_dir, bench_idx);
        if (f == NULL) {
            csfmtperror(
                "failed to open file '%s/bench_raw_%zu.csv' for writing",
                g_out_dir, bench_idx);
            return false;
        }
        export_csv_bench_raw(al->bench_analyses[bench_idx].bench, al, f);
        fclose(f);
    }
    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        const struct meas_analysis *mal = al->meas_analyses + meas_idx;
        {
            FILE *f = open_file_fmt("w", "%s/benches_raw_%zu.csv", g_out_dir,
                                    meas_idx);
            if (f == NULL) {
                csfmtperror(
                    "failed to open file '%s/benches_raw_%zu.csv' for writing",
                    g_out_dir, meas_idx);
                return false;
            }
            export_csv_benches_raw(mal, f);
            fclose(f);
        }
        {
            FILE *f = open_file_fmt("w", "%s/benches_stats_%zu.csv", g_out_dir,
                                    meas_idx);
            if (f == NULL) {
                csfmtperror("failed to open file '%s/benches_stats_%zu.csv' "
                            "for writing",
                            g_out_dir, meas_idx);
                return false;
            }
            export_csv_bench_stats(mal, f);
            fclose(f);
        }
        if (al->group_count) {
            for (size_t grp_idx = 0; grp_idx < al->group_count; ++grp_idx) {
                FILE *f = open_file_fmt("w", "%s/group_raw_%zu_%zu.csv",
                                        g_out_dir, grp_idx, meas_idx);
                if (f == NULL) {
                    csfmtperror("failed to open file "
                                "'%s/group_raw_%zu_%zu.csv' for writing",
                                g_out_dir, grp_idx, meas_idx);
                    return false;
                }
                export_csv_group_raw(mal, grp_idx, f);
                fclose(f);
            }
            {
                FILE *f = open_file_fmt("w", "%s/groups_%zu.csv", g_out_dir,
                                        meas_idx);
                if (f == NULL) {
                    csfmtperror(
                        "failed to open file '%s/groups_%zu.csv' for writing",
                        g_out_dir, meas_idx);
                    return false;
                }
                export_csv_group(mal, f);
                fclose(f);
            }
        }
    }
    {
        FILE *f = open_file_fmt("w", "%s/csv_map.md", g_out_dir);
        if (f == NULL) {
            csfmtperror("failed to open file '%s/csv_map.md' for writing",
                        g_out_dir);
            return false;
        }
        fprintf(f, "# csbench CSV map\n");
        fprintf(f, "## raw data\n");
        for (size_t bench_idx = 0; bench_idx < al->bench_count; ++bench_idx)
            fprintf(f, "- [benchmark %s](bench_raw_%zu.csv)\n",
                    bench_name(al, bench_idx), bench_idx);
        fprintf(f, "## per-measurement analyses\n");
        for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
            if (al->meas[meas_idx].is_secondary)
                continue;
            const struct meas_analysis *mal = al->meas_analyses + meas_idx;
            fprintf(f, "### measurement %s\n", mal->meas->name);
            fprintf(f, "- [raw data](benches_raw_%zu.csv)\n", meas_idx);
            fprintf(f, "- [aggregates statistics](benches_stats_%zu.csv)\n",
                    meas_idx);
            if (al->group_count) {
                fprintf(f, "- [per-value comparison](groups_%zu.csv)\n",
                        meas_idx);
                fprintf(f, "#### per-group raw data\n");
                for (size_t grp_idx = 0; grp_idx < al->group_count; ++grp_idx)
                    fprintf(f, "- [group %s](group_raw_%zu_%zu.csv)\n",
                            bench_group_name(al, grp_idx), grp_idx, meas_idx);
            }
        }
        fclose(f);
    }
    return true;
}

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
        fprintf(f,
                "<li>"
                "<a href=\"#cmp-%zu-%zu\">",
                bench_idx, al->meas_idx);
        if (g_baseline != -1)
            fprintf(f, "  <tt>%s</tt>", bench_name(base, bench_idx));
        else
            fprintf(f, "  <tt>%s</tt>", bench_name(base, reference_idx));
        fprintf(f, " is ");
        if (speedup->is_slower)
            fprintf(f, "%.2f times slower than ", speedup->inv_est.point);
        else
            fprintf(f, "%.2f times faster than ", speedup->est.point);
        if (g_baseline == -1)
            fprintf(f, "<tt>%s</tt>", bench_name(base, bench_idx));
        else
            fprintf(f, "<tt>%s</tt>", bench_name(base, reference_idx));
        fprintf(f, "</a>"
                   "</li>");
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
            fprintf(f, "<a href=\"#cmp-%zu-%zu-%zu\">", grp_idx, val_idx,
                    al->meas_idx);
            if (g_baseline != -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, " is ");
            if (speedup->is_slower)
                fprintf(f, "%.2f times slower than ", speedup->inv_est.point);
            else
                fprintf(f, "%.2f times faster than ", speedup->est.point);
            if (g_baseline == -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, "</a>");
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
            fprintf(f, "<a href=\"#cmpg-%zu-%zu\">", grp_idx, al->meas_idx);
            if (g_baseline != -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, " is ");
            if (speedup->is_slower)
                fprintf(f, "%.2f times slower than ", speedup->inv_est.point);
            else
                fprintf(f, "%.2f times faster than ", speedup->est.point);
            if (g_baseline == -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, "</a>");
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
            fprintf(f, "<a href=\"#cmpg-%zu-%zu\">", grp_idx, al->meas_idx);
            if (g_baseline != -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, " is ");
            if (speedup->is_slower)
                fprintf(f, "%.2f times slower than ", speedup->inv_est.point);
            else
                fprintf(f, "%.2f times faster than ", speedup->est.point);
            if (g_baseline == -1)
                fprintf(f, "  <tt>%s</tt>", bench_group_name(base, grp_idx));
            else
                fprintf(f, "  <tt>%s</tt>",
                        bench_group_name(base, reference_idx));
            fprintf(f, "</a>");
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
            fprintf(f, "<td><a href=\"#cmp-%zu\">comparison</a></td>",
                    bench_idx);
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
                /******/ "<div class=\"stats\">"
                /********/ "<table>"
                /**********/ "<thead><tr>"
                /************/ "<th></th>"
                /************/ "<th><tt>%s</tt></th>"
                /************/ "<th><tt>%s</tt></th>"
                /**********/ "</tr></thead>"
                /**********/ "<tbody>",
                bench_idx, al->meas_idx, a_name, b_name, bench_idx,
                al->meas_idx, bench_idx, al->meas_idx, a_name, b_name);
        char a_mean[256], b_mean[256], a_st_dev[256], b_st_dev[256];
        format_meas(a_mean, sizeof(a_mean), a_distr->mean.point,
                    &al->meas->units);
        format_meas(b_mean, sizeof(b_mean), b_distr->mean.point,
                    &al->meas->units);
        format_meas(a_st_dev, sizeof(a_st_dev), a_distr->st_dev.point,
                    &al->meas->units);
        format_meas(b_st_dev, sizeof(b_st_dev), b_distr->st_dev.point,
                    &al->meas->units);
        fprintf(f,
                "<tr><td>mean</td><td>%s</td><td>%s</td></tr>"
                "<tr><td>st dev</td><td>%s</td><td>%s</td></tr>"
                "</tbody>"
                "</table>",
                a_mean, b_mean, a_st_dev, b_st_dev);

        {
            const struct speedup *speedup = al->bench_speedups + bench_idx;
            fprintf(f, "<p>");
            if (g_baseline != -1)
                fprintf(f, "  <tt>%s</tt>", b_name);
            else
                fprintf(f, "  <tt>%s</tt>", a_name);
            fprintf(f, " is ");
            if (speedup->is_slower)
                fprintf(f, "%.3f ± %.3f times slower than ",
                        speedup->inv_est.point, speedup->inv_est.err);
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
                fprintf(f, "%.2f%% slowdown",
                        (speedup->inv_est.point - 1.0) * 100.0);
            else
                fprintf(f, "%.2f%% speedup ",
                        (speedup->est.point - 1.0) * 100.0);
            fprintf(f, "</p>");
        }
        {
            double p_value = al->p_values[bench_idx];
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

        fprintf(f,
                "</div>" // stats
                "</div>" // cols
                "</div>" // rows
                "</div>" // kde-cmp
        );
    }
    fprintf(f, "</div>"); // kde-cmps
}

static void html_compare_benches(const struct meas_analysis *al, FILE *f)
{
    html_compare_benches_nav(al, f);
    html_compare_benches_kdes(al, f);
}

static void html_compare_groups_nav(const struct meas_analysis *al, FILE *f)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t val_count = var->value_count;
    fprintf(f, "<div id=\"cmps\">"
               /**/ "<div class=\"row\">"
               /****/ "<div class=\"col\">"
               /******/ "<h2>comparisons</h2>"
               /******/ "<h4>groups comparison</h4>");
    {
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
               "</table>"
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
                fprintf(f, "<p>");
                if (g_baseline != -1)
                    fprintf(f, "  <tt>%s</tt>", b_name);
                else
                    fprintf(f, "  <tt>%s</tt>", a_name);
                fprintf(f, " is ");
                if (speedup->is_slower)
                    fprintf(f, "%.3f ± %.3f times slower than ",
                            speedup->inv_est.point, speedup->inv_est.err);
                else
                    fprintf(f, "%.3f ± %.3f times faster than ",
                            speedup->est.point, speedup->est.err);
                if (g_baseline == -1)
                    fprintf(f, "<tt>%s</tt>", b_name);
                else
                    fprintf(f, "<tt>%s</tt>", a_name);
                fprintf(f, "</p>"
                           "<p>");
                if (speedup->is_slower)
                    fprintf(f, "%.2f%% slowdown",
                            (speedup->inv_est.point - 1.0) * 100.0);
                else
                    fprintf(f, "%.2f%% speedup ",
                            (speedup->est.point - 1.0) * 100.0);
                fprintf(f, "</p>");
            }
            fprintf(f, "<p>Average difference by sum:</p>");
            {
                // TODO: This has to use other order
                const struct speedup *speedup =
                    al->group_total_speedups + grp_idx;
                fprintf(f, "<p>");
                if (g_baseline != -1)
                    fprintf(f, "  <tt>%s</tt>", b_name);
                else
                    fprintf(f, "  <tt>%s</tt>", a_name);
                fprintf(f, " is ");
                if (speedup->is_slower)
                    fprintf(f, "%.3f ± %.3f times slower than ",
                            speedup->inv_est.point, speedup->inv_est.err);
                else
                    fprintf(f, "%.3f ± %.3f times faster than ",
                            speedup->est.point, speedup->est.err);
                if (g_baseline == -1)
                    fprintf(f, "<tt>%s</tt>", b_name);
                else
                    fprintf(f, "<tt>%s</tt>", a_name);
                fprintf(f, "</p>"
                           "<p>");
                if (speedup->is_slower)
                    fprintf(f, "%.2f%% slowdown",
                            (speedup->inv_est.point - 1.0) * 100.0);
                else
                    fprintf(f, "%.2f%% speedup ",
                            (speedup->est.point - 1.0) * 100.0);
                fprintf(f, "</p>");
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
                /******/ "<div class=\"stats\">"
                /********/ "<table>"
                /**********/ "<thead><tr>"
                /************/ "<th></th>"
                /************/ "<th><tt>%s</tt></th>"
                /************/ "<th><tt>%s</tt></th>"
                /**********/ "</tr></thead>"
                /**********/ "<tbody>",
                grp_idx, val_idx, al->meas_idx, a_name, b_name, grp_idx,
                val_idx, al->meas_idx, grp_idx, val_idx, al->meas_idx, a_name,
                b_name);
            char a_mean[256], b_mean[256], a_st_dev[256], b_st_dev[256];
            format_meas(a_mean, sizeof(a_mean), a_distr->mean.point,
                        &al->meas->units);
            format_meas(b_mean, sizeof(b_mean), b_distr->mean.point,
                        &al->meas->units);
            format_meas(a_st_dev, sizeof(a_st_dev), a_distr->st_dev.point,
                        &al->meas->units);
            format_meas(b_st_dev, sizeof(b_st_dev), b_distr->st_dev.point,
                        &al->meas->units);
            fprintf(f,
                    "<tr><td>mean</td><td>%s</td><td>%s</td></tr>"
                    "<tr><td>st dev</td><td>%s</td><td>%s</td></tr>"
                    "</tbody>"
                    "</table>",
                    a_mean, b_mean, a_st_dev, b_st_dev);

            {
                const struct speedup *speedup =
                    al->val_bench_speedups[val_idx] + grp_idx;
                fprintf(f, "<p>");
                if (g_baseline != -1)
                    fprintf(f, "  <tt>%s</tt>", b_name);
                else
                    fprintf(f, "  <tt>%s</tt>", a_name);
                fprintf(f, " is ");
                if (speedup->is_slower)
                    fprintf(f, "%.3f ± %.3f times slower than ",
                            speedup->inv_est.point, speedup->inv_est.err);
                else
                    fprintf(f, "%.3f ± %.3f times faster than ",
                            speedup->est.point, speedup->est.err);
                if (g_baseline == -1)
                    fprintf(f, "<tt>%s</tt>", b_name);
                else
                    fprintf(f, "<tt>%s</tt>", a_name);
                fprintf(f, "</p>"
                           "<p>");
                if (speedup->is_slower)
                    fprintf(f, "%.2f%% slowdown",
                            (speedup->inv_est.point - 1.0) * 100.0);
                else
                    fprintf(f, "%.2f%% speedup ",
                            (speedup->est.point - 1.0) * 100.0);
                fprintf(f, "</p>");
            }
            {
                double p_value = al->val_p_values[val_idx][grp_idx];
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

static void html_compare_groups(const struct meas_analysis *al, FILE *f)
{
    html_compare_groups_nav(al, f);
    html_compare_groups_kdes(al, f);
}

static void html_compare(const struct meas_analysis *al, FILE *f)
{
    if (al->base->bench_count == 1)
        return;
    if (al->base->group_count <= 1)
        html_compare_benches(al, f);
    else
        html_compare_groups(al, f);
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

static bool make_html_report(const struct analysis *al)
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

static bool make_reports(const struct analysis *al)
{
    if (g_json_export_filename != NULL &&
        !export_json(al, g_json_export_filename))
        return false;

    if (!g_plot && !g_html && !g_csv)
        return true;

    if (g_plot) {
        enum plot_backend plot_backend;
        if (!get_plot_backend(&plot_backend))
            return false;

        if (!make_plot_srcs(al, plot_backend))
            return false;
        if (!make_plots(al, plot_backend))
            return false;
        if (!g_plot_src && !delete_plot_srcs(al, plot_backend))
            return false;
        if (!make_plots_map(al))
            return false;
    }

    if (g_csv && !export_csvs(al))
        return false;

    if (g_html && !make_html_report(al))
        return false;

    return true;
}

static void print_exit_code_info(const struct bench *bench)
{
    if (!bench->exit_codes)
        return;

    size_t count_nonzero = 0;
    for (size_t i = 0; i < bench->run_count; ++i)
        if (bench->exit_codes[i] != 0)
            ++count_nonzero;

    if (count_nonzero == bench->run_count) {
        printf("all commands have non-zero exit code: %d\n",
               bench->exit_codes[0]);
    } else if (count_nonzero != 0) {
        printf("some runs (%zu) have non-zero exit code\n", count_nonzero);
    }
}

static void print_outliers(const struct outliers *outliers, size_t run_count)
{
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
                           const char *sec_color)
{
    char buf1[256], buf2[256], buf3[256];
    format_meas(buf1, sizeof(buf1), est->lower, units);
    format_meas(buf2, sizeof(buf2), est->point, units);
    format_meas(buf3, sizeof(buf3), est->upper, units);

    printf_colored(prim_color, "%7s", name);
    printf_colored(sec_color, " %8s ", buf1);
    printf_colored(prim_color, "%8s", buf2);
    printf_colored(sec_color, " %8s\n", buf3);
}

static void print_distr(const struct distr *dist, const struct units *units)
{
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
                                 const struct analysis *al)
{
    const struct bench *bench = cur->bench;
    printf("benchmark ");
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

size_t ith_bench_idx(int i, const struct meas_analysis *al)
{
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_BASELINE_RAW:
        return i;
    case SORT_SPEED:
    case SORT_BASELINE_SPEED:
        return al->bench_by_mean_time[i];
    default:
        ASSERT_UNREACHABLE();
    }
}

static void print_bench_comparison(const struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t reference_idx = al->bench_speedups_reference;
    const char *reference_name = bench_name(base, reference_idx);
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_SPEED:
        if (base->bench_count > 2) {
            printf_colored(ANSI_BLUE, "fastest");
            printf(" is ");
            printf_colored(ANSI_BOLD, "%s\n", reference_name);
            printf("slowest is ");
            printf_colored(
                ANSI_BOLD, "%s\n",
                bench_name(base,
                           al->bench_by_mean_time[base->bench_count - 1]));
        }
        break;
    case SORT_BASELINE_RAW:
    case SORT_BASELINE_SPEED:
        printf("baseline is ");
        printf_colored(ANSI_BOLD, "%s\n", reference_name);
        break;
    default:
        ASSERT_UNREACHABLE();
    }
    foreach_bench_idx (bench_idx, al) {
        if (bench_idx == reference_idx)
            continue;
        const struct speedup *speedup = al->bench_speedups + bench_idx;
        const char *name = bench_name(base, bench_idx);
        if (g_baseline != -1)
            printf_colored(ANSI_BOLD, "  %s", name);
        else
            printf_colored(ANSI_BOLD, "  %s", reference_name);
        printf(" is ");
        if (speedup->is_slower) {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->inv_est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->inv_est.err);
            printf(" times slower than ");
        } else {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->est.err);
            printf(" times faster than ");
        }
        if (g_baseline == -1)
            printf_colored(ANSI_BOLD, "%s", name);
        else
            printf("baseline");
        printf(" (p=%.2f)", al->p_values[bench_idx]);
        printf("\n");
    }
    if (base->group_count == 1 && g_regr) {
        const struct group_analysis *grp = al->group_analyses;
        if (grp->values_are_doubles)
            printf("%s complexity (%g)\n", big_o_str(grp->regress.complexity),
                   grp->regress.a);
    }
}

static bool should_abbreviate_names(const struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    size_t length_limit = 5;

    for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
        const char *name = al->group_analyses[grp_idx].group->name;
        if (strlen(name) > length_limit)
            return true;
    }
    return false;
}

static const char *cli_group_name(const struct meas_analysis *al, size_t idx,
                                  bool abbreviate_names)
{
    if (abbreviate_names) {
        // Algorithm below does not handle zeroes on its own
        if (idx == 0)
            return "A";

        size_t base = 'Z' - 'A' + 1;
        size_t power = 0;
        for (size_t t = idx; t != 0; t /= base, ++power)
            ;

        char buf[256];
        assert(power < sizeof(buf));
        buf[power] = '\0';
        char *cursor = buf + power - 1;
        while (idx != 0) {
            *cursor-- = 'A' + (idx % base);
            idx /= base;
        }
        return csstrdup(buf);
    }
    return bench_group_name(al->base, idx);
}

size_t ith_per_val_group_idx(size_t i, size_t val_idx,
                             const struct meas_analysis *al)
{
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_BASELINE_RAW:
        return i;
    case SORT_SPEED:
    case SORT_BASELINE_SPEED:
        return al->val_benches_by_mean_time[val_idx][i];
    default:
        ASSERT_UNREACHABLE();
    }
}

size_t ith_group_by_avg_idx(size_t i, const struct meas_analysis *al)
{
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_BASELINE_RAW:
        return i;
    case SORT_SPEED:
    case SORT_BASELINE_SPEED:
        return al->groups_by_avg_speed[i];
    default:
        ASSERT_UNREACHABLE();
    }
}

size_t ith_group_by_total_idx(size_t i, const struct meas_analysis *al)
{
    switch (g_sort_mode) {
    case SORT_RAW:
    case SORT_BASELINE_RAW:
        return i;
    case SORT_SPEED:
    case SORT_BASELINE_SPEED:
        return al->groups_by_total_speed[i];
    default:
        ASSERT_UNREACHABLE();
    }
}

static void print_group_per_value_speedups(const struct meas_analysis *al,
                                           bool abbreviate_names)
{
    const struct analysis *base = al->base;
    const struct bench_var *var = base->var;
    size_t value_count = var->value_count;

    // Align all var=value strings
    size_t max_var_desc_len = 0;
    for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
        const char *value = var->values[val_idx];
        size_t len = snprintf(NULL, 0, "%s=%s:", var->name, value);
        if (len > max_var_desc_len)
            max_var_desc_len = len;
    }

    for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
        const char *value = var->values[val_idx];
        size_t reference_idx = al->val_bench_speedups_references[val_idx];
        size_t len = printf("%s=%s:", var->name, value);
        for (; len < max_var_desc_len; ++len)
            printf(" ");

        if (base->group_count > 2) {
            printf("\n");
            size_t fastest_idx = al->val_benches_by_mean_time[val_idx][0];
            printf_colored(ANSI_BLUE, "  fastest");
            printf(" is ");
            printf_colored(ANSI_BOLD, "%s",
                           cli_group_name(al, fastest_idx, abbreviate_names));
            if (g_baseline != -1 && fastest_idx == (size_t)g_baseline)
                printf(" (baseline)");
            printf("\n");
            printf("  slowest is ");
            size_t slowest_idx =
                al->val_benches_by_mean_time[val_idx][base->group_count - 1];
            printf_colored(ANSI_BOLD, "%s",
                           cli_group_name(al, slowest_idx, abbreviate_names));
            if (g_baseline != -1 && slowest_idx == (size_t)g_baseline)
                printf(" (baseline)");
            printf("\n");
        }

        if (g_baseline == -1) {
            printf_colored(ANSI_BOLD, "  %s ",
                           cli_group_name(al, reference_idx, abbreviate_names));
            printf("is ");
            if (base->group_count > 2)
                printf("\n");
        }
        foreach_per_val_group_idx (grp_idx, val_idx, al) {
            if (grp_idx == reference_idx)
                continue;
            const struct speedup *speedup =
                al->val_bench_speedups[val_idx] + grp_idx;
            if (g_baseline != -1) {
                printf_colored(ANSI_BOLD, "  %s ",
                               cli_group_name(al, grp_idx, abbreviate_names));
                printf("is ");
            } else if (base->group_count > 2) {
                printf("  ");
            }
            if (speedup->is_slower) {
                printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->inv_est.point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->inv_est.err);
                printf(" times slower than ");
            } else {
                printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->est.point);
                printf(" ± ");
                printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->est.err);
                printf(" times faster than ");
            }
            if (g_baseline == -1)
                printf("%s", cli_group_name(al, grp_idx, abbreviate_names));
            else
                printf("baseline");
            printf(" (p=%.2f)", al->val_p_values[val_idx][grp_idx]);
            printf("\n");
        }
    }
}

static void print_group_average_speedups(const struct meas_analysis *al,
                                         bool abbreviate_names)
{
    const struct analysis *base = al->base;
    if (base->group_count <= 1)
        return;
    printf("in total (avg) ");
    if (base->group_count > 2)
        printf("\n");
    size_t reference_idx = al->groups_avg_reference;
    if (base->group_count > 2) {
        size_t fastest_idx = al->groups_by_avg_speed[0];
        printf_colored(ANSI_BLUE, "  fastest");
        printf(" is ");
        printf_colored(ANSI_BOLD, "%s",
                       cli_group_name(al, fastest_idx, abbreviate_names));
        if (g_baseline != -1 && fastest_idx == (size_t)g_baseline)
            printf(" (baseline)");
        printf("\n");
        printf("  slowest is ");
        size_t slowest_idx = al->groups_by_avg_speed[base->group_count - 1];
        printf_colored(ANSI_BOLD, "%s",
                       cli_group_name(al, slowest_idx, abbreviate_names));
        if (g_baseline != -1 && slowest_idx == (size_t)g_baseline)
            printf(" (baseline)");
        printf("\n");
    }
    if (g_baseline == -1) {
        printf_colored(ANSI_BOLD, "  %s ",
                       cli_group_name(al, reference_idx, abbreviate_names));
        printf("is ");
        if (base->group_count > 2)
            printf("\n");
    }
    foreach_group_by_avg_idx (grp_idx, al) {
        if (grp_idx == reference_idx)
            continue;
        const struct speedup *speedup = al->group_avg_speedups + grp_idx;
        if (base->group_count > 2)
            printf("  ");
        if (g_baseline != -1) {
            printf_colored(ANSI_BOLD, "%s",
                           cli_group_name(al, grp_idx, abbreviate_names));
            printf(" is ");
        }
        if (speedup->is_slower) {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->inv_est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->inv_est.err);
            printf(" times slower than ");
        } else {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->est.err);
            printf(" times faster than ");
        }
        if (g_baseline == -1)
            printf("%s", cli_group_name(al, grp_idx, abbreviate_names));
        else
            printf("baseline");
        printf("\n");
    }
}

static void print_group_total_speedups(const struct meas_analysis *al,
                                       bool abbreviate_names)
{
    const struct analysis *base = al->base;
    if (base->group_count <= 1)
        return;
    printf("in total (sum) ");
    if (base->group_count > 2)
        printf("\n");
    size_t reference_idx = al->groups_total_reference;
    if (base->group_count > 2) {
        size_t fastest_idx = al->groups_by_total_speed[0];
        printf_colored(ANSI_BLUE, "  fastest");
        printf(" is ");
        printf_colored(ANSI_BOLD, "%s",
                       cli_group_name(al, fastest_idx, abbreviate_names));
        if (g_baseline != -1 && fastest_idx == (size_t)g_baseline)
            printf(" (baseline)");
        printf("\n");
        printf("  slowest is ");
        size_t slowest_idx = al->groups_by_total_speed[base->group_count - 1];
        printf_colored(ANSI_BOLD, "%s",
                       cli_group_name(al, slowest_idx, abbreviate_names));
        if (g_baseline != -1 && slowest_idx == (size_t)g_baseline)
            printf(" (baseline)");
        printf("\n");
    }
    if (g_baseline == -1) {
        printf_colored(ANSI_BOLD, "  %s ",
                       cli_group_name(al, reference_idx, abbreviate_names));
        printf("is ");
        if (base->group_count > 2)
            printf("\n");
    }
    foreach_group_by_total_idx (grp_idx, al) {
        if (grp_idx == reference_idx)
            continue;
        const struct speedup *speedup = al->group_total_speedups + grp_idx;
        if (base->group_count > 2)
            printf("  ");
        if (g_baseline != -1) {
            printf_colored(ANSI_BOLD, "%s",
                           cli_group_name(al, grp_idx, abbreviate_names));
            printf(" is ");
        }
        if (speedup->is_slower) {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->inv_est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->inv_est.err);
            printf(" times slower than ");
        } else {
            printf_colored(ANSI_BOLD_GREEN, "%.3f", speedup->est.point);
            printf(" ± ");
            printf_colored(ANSI_BRIGHT_GREEN, "%.3f", speedup->est.err);
            printf(" times faster than ");
        }
        if (g_baseline == -1)
            printf("%s", cli_group_name(al, grp_idx, abbreviate_names));
        else
            printf("baseline");
        printf("\n");
    }
}

static void print_group_comparison(const struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    bool abbreviate_names = should_abbreviate_names(al);
    if (abbreviate_names) {
        for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
            printf("%s = ", cli_group_name(al, grp_idx, true));
            printf_colored(ANSI_BOLD, "%s", bench_group_name(base, grp_idx));
            if (g_baseline != -1 && (size_t)g_baseline == grp_idx)
                printf(" (baseline)");
            printf("\n");
        }
    } else {
        if (g_baseline != -1) {
            printf("baseline group ");
            printf_colored(ANSI_BOLD, "%s\n",
                           al->group_analyses[g_baseline].group->name);
        }
    }

    print_group_per_value_speedups(al, abbreviate_names);
    print_group_average_speedups(al, abbreviate_names);
    print_group_total_speedups(al, abbreviate_names);

    if (g_regr) {
        for (size_t grp_idx = 0; grp_idx < base->group_count; ++grp_idx) {
            const struct group_analysis *grp = al->group_analyses + grp_idx;
            if (grp->values_are_doubles) {
                printf_colored(ANSI_BOLD, "%s ",
                               cli_group_name(al, grp_idx, abbreviate_names));
                printf("%s complexity (%g)\n",
                       big_o_str(grp->regress.complexity), grp->regress.a);
            }
        }
    }
}

static void print_meas_analysis(const struct meas_analysis *al)
{
    const struct analysis *base = al->base;
    if (base->bench_count == 1)
        return;

    if (base->primary_meas_count != 1) {
        printf("measurement ");
        printf_colored(ANSI_YELLOW, "%s\n", al->meas->name);
    }

    if (base->group_count <= 1)
        print_bench_comparison(al);
    else
        print_group_comparison(al);
}

static void print_text_report(const struct analysis *al)
{
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

    for (size_t meas_idx = 0; meas_idx < al->meas_count; ++meas_idx) {
        if (al->meas[meas_idx].is_secondary)
            continue;
        print_meas_analysis(al->meas_analyses + meas_idx);
    }
}

bool make_report(const struct analysis *al)
{
    print_text_report(al);
    return make_reports(al);
}
