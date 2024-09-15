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
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct command_info {
    const char *name;
    const char *cmd;
    struct input_policy input;
    enum output_kind output;
    size_t grp_idx;
    const char *grp_name;
};

__thread uint64_t g_rng_state;
bool g_colored_output = false;
bool g_ignore_failure = false;
bool g_plot = false;
bool g_html = false;
bool g_csv = false;
bool g_plot_src = false;
bool g_use_perf = false;
bool g_progress_bar = false;
bool g_regr = false;
bool g_plot_debug = false;
bool g_save_bin = false;
bool g_rename_all_used = false;
bool g_clear_out_dir = false;
bool g_shuffle_when_runnig = false;
int g_nresamp = 10000;
int g_progress_bar_interval_us = 100000;
int g_threads = 1;
int g_baseline = -1;
int g_desired_plots = 0;
enum sort_mode g_sort_mode = SORT_DEFAULT;
enum statistical_test g_stat_test = STAT_TEST_MWU;
enum plot_backend g_plot_backend_override = PLOT_BACKEND_DEFAULT;
enum app_mode g_mode = APP_BENCH;
struct bench_stop_policy g_warmup_stop = {0.1, 0, 1, 10};
struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};
struct bench_stop_policy g_round_stop = {0, 0, INT_MAX, 0};
// XXX: Mark this as volatile because we rely that this variable is changed
// atomically when creating and destorying threads. Elements of this array could
// only be written by a single thread, and reads are synchronized, so the data
// itself does not need to be volatile.
struct output_anchor *volatile g_output_anchors = NULL;
const char *g_json_export_filename = NULL;
const char *g_out_dir = ".csbench";
const char *g_shell = "/bin/sh";
const char *g_common_argstring = NULL;
const char *g_prepare = NULL;
// XXX: This is hack to use short names for files found in directory specified
// with --inputd (otherwise because we create parameter values which are full
// names). When opening files and this variable is not null, open it relative to
// this directory.
const char *g_inputd = NULL;
const char *g_override_bin_name = NULL;
const char *g_baseline_name = NULL;
const char *g_python_executable = "python3";

static bool replace_param_str(char *buf, size_t buf_size, const char *src, const char *name,
                              const char *value, bool *replaced)
{
    const char *buf_end = buf + buf_size;
    size_t param_name_len = strlen(name);
    char *wr_cursor = buf;
    const char *rd_cursor = src;
    while (*rd_cursor) {
        if (*rd_cursor == '{' && strncmp(rd_cursor + 1, name, param_name_len) == 0 &&
            rd_cursor[param_name_len + 1] == '}') {
            rd_cursor += 2 + param_name_len;
            size_t len = strlen(value);
            if (wr_cursor + len >= buf_end)
                return false;
            memcpy(wr_cursor, value, len);
            wr_cursor += len;
            *replaced = true;
        } else {
            if (wr_cursor >= buf_end)
                return false;
            *wr_cursor++ = *rd_cursor++;
        }
    }
    if (wr_cursor >= buf_end)
        return false;
    *wr_cursor = '\0';
    return true;
}

static char **split_shell_words(const char *cmd)
{
    char **words = NULL;
    char *current_word = NULL;
    enum {
        STATE_DELIMITER,
        STATE_BACKSLASH,
        STATE_UNQUOTED,
        STATE_UNQUOTED_BACKSLASH,
        STATE_SINGLE_QUOTED,
        STATE_DOUBLE_QUOTED,
        STATE_DOUBLE_QUOTED_BACKSLASH,
        STATE_COMMENT
    } state = STATE_DELIMITER;
    for (;;) {
        int c = *cmd++;
        switch (state) {
        case STATE_DELIMITER:
            switch (c) {
            case '\0':
                if (current_word != NULL) {
                    sb_push(current_word, '\0');
                    sb_push(words, current_word);
                }
                goto out;
            case '\'':
                state = STATE_SINGLE_QUOTED;
                break;
            case '"':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '\\':
                state = STATE_BACKSLASH;
                break;
            case '\t':
            case ' ':
            case '\n':
                state = STATE_DELIMITER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                sb_push(current_word, c);
                state = STATE_UNQUOTED;
                break;
            }
            break;
        case STATE_BACKSLASH:
            switch (c) {
            case '\0':
                sb_push(current_word, '\\');
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_DELIMITER;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED:
            switch (c) {
            case '\0':
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\'':
                state = STATE_SINGLE_QUOTED;
                break;
            case '"':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '\\':
                state = STATE_UNQUOTED_BACKSLASH;
                break;
            case '\t':
            case ' ':
            case '\n':
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                current_word = NULL;
                state = STATE_DELIMITER;
                break;
            case '#':
                state = STATE_COMMENT;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_UNQUOTED_BACKSLASH:
            switch (c) {
            case '\0':
                sb_push(current_word, '\\');
                sb_push(current_word, '\0');
                sb_push(words, current_word);
                goto out;
            case '\n':
                state = STATE_UNQUOTED;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_SINGLE_QUOTED:
            switch (c) {
            case '\0':
                goto error;
            case '\'':
                state = STATE_UNQUOTED;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_DOUBLE_QUOTED:
            switch (c) {
            case '\0':
                goto error;
            case '"':
                state = STATE_UNQUOTED;
                break;
            case '\\':
                state = STATE_DOUBLE_QUOTED_BACKSLASH;
                break;
            default:
                sb_push(current_word, c);
                break;
            }
            break;
        case STATE_DOUBLE_QUOTED_BACKSLASH:
            switch (c) {
            case '\0':
                goto error;
            case '\n':
                state = STATE_DOUBLE_QUOTED;
                break;
            case '$':
            case '`':
            case '"':
            case '\\':
                sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            default:
                sb_push(current_word, '\\');
                sb_push(current_word, c);
                state = STATE_DOUBLE_QUOTED;
                break;
            }
            break;
        case STATE_COMMENT:
            switch (c) {
            case '\0':
                if (current_word) {
                    sb_push(current_word, '\0');
                    sb_push(words, current_word);
                }
                goto out;
            case '\n':
                state = STATE_DELIMITER;
                break;
            default:
                break;
            }
            break;
        }
    }
error:
    if (current_word)
        sb_free(current_word);
    for (size_t i = 0; i < sb_len(words); ++i)
        sb_free(words[i]);
    sb_free(words);
    words = NULL;
out:
    return words;
}

static bool extract_exec_and_argv(const char *cmd_str, const char **exec, const char ***argv)
{
    char **words = split_shell_words(cmd_str);
    if (words == NULL) {
        error("invalid command syntax");
        return false;
    }
    *exec = csstrdup(words[0]);
    for (size_t i = 0; i < sb_len(words); ++i) {
        sb_push(*argv, csstrdup(words[i]));
        sb_free(words[i]);
    }
    sb_free(words);
    return true;
}

static bool init_cmd_exec(const char *shell, const char *cmd_str, const char **exec,
                          const char ***argv)
{
    if (shell != NULL) {
        if (!extract_exec_and_argv(shell, exec, argv))
            return false;
        sb_push(*argv, "-c");
        sb_push(*argv, (char *)cmd_str);
        sb_push(*argv, NULL);
    } else {
        if (!extract_exec_and_argv(cmd_str, exec, argv))
            return false;
        sb_push(*argv, NULL);
    }
    return true;
}

static bool init_bench_stdin(const struct input_policy *input, struct bench_run_desc *params)
{
    switch (input->kind) {
    case INPUT_POLICY_NULL:
        params->stdin_fd = -1;
        break;
    case INPUT_POLICY_FILE: {
        char buf[4096];
        const char *file = input->file;
        if (g_inputd) {
            snprintf(buf, sizeof(buf), "%s/%s", g_inputd, input->file);
            file = buf;
        }

        int fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
            csfmtperror("failed to open file '%s' (designated for benchmark input)",
                        input->file);
            return false;
        }
        params->stdin_fd = fd;
        break;
    }
    case INPUT_POLICY_STRING: {
        int fd = tmpfile_fd();
        if (fd == -1)
            return false;

        int len = strlen(input->string);
        int nw = write(fd, input->string, len);
        if (nw != len) {
            csperror("write");
            return false;
        }
        if (lseek(fd, 0, SEEK_SET) == -1) {
            csperror("lseek");
            return false;
        }
        params->stdin_fd = fd;
        break;
    }
    }
    return true;
}

static bool init_run_desc(const struct input_policy *input, enum output_kind output,
                          const struct meas *meas, size_t meas_count, const char *exec,
                          const char **argv, const char *cmd_str,
                          struct bench_run_desc *params)
{
    memset(params, 0, sizeof(*params));
    params->output = output;
    params->meas = meas;
    params->meas_count = meas_count;
    params->exec = exec;
    params->argv = argv;
    params->str = cmd_str;
    params->stdout_fd = -1;
    return init_bench_stdin(input, params);
}

static bool init_bench_stdout(struct bench_run_desc *params)
{
    int fd = tmpfile_fd();
    if (fd == -1)
        return false;
    params->stdout_fd = fd;
    return true;
}

static bool init_command(const struct command_info *cmd, const struct meas *meas,
                         size_t meas_count, struct bench_run_desc *run_desc)
{
    const char *exec = NULL, **argv = NULL;
    if (!init_cmd_exec(g_shell, cmd->cmd, &exec, &argv))
        return false;

    if (!init_run_desc(&cmd->input, cmd->output, meas, meas_count, exec, argv,
                       (char *)cmd->cmd, run_desc)) {
        sb_free(argv);
        return false;
    }
    return true;
}

static bool init_raw_command_infos(const struct settings *settings,
                                   struct command_info **infos)
{
    size_t cmd_count = sb_len(settings->args);
    if (cmd_count == 0) {
        error("no commands specified");
        return false;
    }
    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = settings->args[i];
        if (g_common_argstring) {
            cmd_str = csfmt("%s %s", cmd_str, g_common_argstring);
        }

        struct command_info info;
        memset(&info, 0, sizeof(info));
        info.name = info.cmd = cmd_str;
        info.output = settings->output;
        info.input = settings->input;
        info.grp_name = cmd_str;
        sb_push(*infos, info);
    }
    return true;
}

enum cmd_multiplex_result {
    CMD_MULTIPLEX_ERROR,
    CMD_MULTIPLEX_NO_GROUPS,
    CMD_MULTIPLEX_SUCCESS
};

static enum cmd_multiplex_result
multiplex_command_info_cmd(const struct command_info *src_info, size_t src_idx,
                           const struct bench_param *param, struct command_info **multiplexed)
{
    // Take first value and try to replace it in the command string
    char buf[4096];
    bool replaced = false;
    if (!replace_param_str(buf, sizeof(buf), src_info->cmd, param->name, param->values[0],
                           &replaced)) {
        error("failed to substitute parameter '%s' in string '%s' with value '%s'",
              param->name, src_info->cmd, param->values[0]);
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    // We could reuse the string that is contained in buffer right now,
    // but it is a bit unecessary.
    for (size_t val_idx = 0; val_idx < param->value_count; ++val_idx) {
        const char *param_value = param->values[val_idx];
        if (!replace_param_str(buf, sizeof(buf), src_info->cmd, param->name, param_value,
                               &replaced)) {
            error("failed to substitute parameter '%s' in string '%s' with value '%s'",
                  param->name, src_info->cmd, param_value);
            return CMD_MULTIPLEX_ERROR;
        }
        assert(replaced);
        struct command_info info;
        memcpy(&info, src_info, sizeof(info));
        info.name = info.cmd = csstrdup(buf);
        info.grp_idx = src_idx;
        info.grp_name = src_info->grp_name;
        sb_push(*multiplexed, info);
    }
    return CMD_MULTIPLEX_SUCCESS;
}

static enum cmd_multiplex_result
multiplex_command_info_input(const struct command_info *src_info, size_t src_idx,
                             const struct bench_param *param,
                             struct command_info **multiplexed)
{
    if (src_info->input.kind != INPUT_POLICY_FILE &&
        src_info->input.kind != INPUT_POLICY_STRING)
        return CMD_MULTIPLEX_NO_GROUPS;

    const char *src_string = NULL;
    if (src_info->input.kind == INPUT_POLICY_FILE)
        src_string = src_info->input.file;
    else if (src_info->input.kind == INPUT_POLICY_STRING)
        src_string = src_info->input.string;

    assert(src_string);
    char buf[4096];
    bool replaced = false;
    if (!replace_param_str(buf, sizeof(buf), src_string, param->name, param->values[0],
                           &replaced)) {
        error("failed to substitute parameter '%s' in string '%s' with value '%s'",
              param->name, src_info->cmd, param->values[0]);
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    for (size_t val_idx = 0; val_idx < param->value_count; ++val_idx) {
        const char *param_value = param->values[val_idx];
        if (!replace_param_str(buf, sizeof(buf), src_string, param->name, param_value,
                               &replaced) ||
            !replaced) {
            error("failed to substitute parameter '%s' in string '%s' with value '%s'",
                  param->name, src_info->cmd, param_value);
            return CMD_MULTIPLEX_ERROR;
        }
        struct command_info info;
        memcpy(&info, src_info, sizeof(info));
        info.cmd = src_info->cmd;
        info.grp_idx = src_idx;
        info.grp_name = src_info->grp_name;
        if (src_info->input.kind == INPUT_POLICY_FILE) {
            info.input.file = csstrdup(buf);
            snprintf(buf, sizeof(buf), "%s < %s", info.cmd, info.input.file);
            info.name = csstrdup(buf);
        } else if (src_info->input.kind == INPUT_POLICY_STRING) {
            info.input.string = csstrdup(buf);
            snprintf(buf, sizeof(buf), "%s <<< \"%s\"", info.cmd, info.input.string);
            info.name = csstrdup(buf);
        }
        sb_push(*multiplexed, info);
    }
    return CMD_MULTIPLEX_SUCCESS;
}

static enum cmd_multiplex_result multiplex_command_infos(const struct bench_param *param,
                                                         struct command_info **infos)
{
    struct command_info *multiplexed = NULL;
    for (size_t src_idx = 0; src_idx < sb_len(*infos); ++src_idx) {
        const struct command_info *src_info = *infos + src_idx;
        int ret;

        ret = multiplex_command_info_cmd(src_info, src_idx, param, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        ret = multiplex_command_info_input(src_info, src_idx, param, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        error("command '%s' does not contain parameters", src_info->cmd);
        goto err;
    }

    sb_free(*infos);
    *infos = multiplexed;
    return CMD_MULTIPLEX_SUCCESS;
err:
    sb_free(multiplexed);
    return CMD_MULTIPLEX_ERROR;
}

static bool validate_rename_list(const struct rename_entry *rename_list,
                                 const struct bench_data *data)
{
    if (data->group_count == 0) {
        if (g_rename_all_used) {
            if (sb_len(rename_list) != data->bench_count) {
                error("number (%zu) of benchmarks to be renamed (supplied with --rename-all) "
                      "does not match number of benchmarks (%zu)",
                      sb_len(rename_list), data->bench_count);
                return false;
            }
        } else {
            for (size_t i = 0; i < sb_len(rename_list); ++i) {
                const struct rename_entry *re = rename_list + i;
                if (re->old_name != NULL) {
                    bool found = false;
                    for (size_t j = 0; j < data->bench_count; ++j) {
                        if (strcmp(re->old_name, data->benches[j].name) == 0)
                            found = true;
                    }
                    if (!found) {
                        error("benchmark with name '%s' (to be renamed to '%s') not found",
                              re->old_name, re->name);
                        return false;
                    }
                } else if (re->n >= data->bench_count) {
                    error("number (%zu) of benchmark to be renamed ('%s') is too high",
                          rename_list[i].n + 1, rename_list[i].name);
                    return false;
                }
            }
        }
    } else {
        if (g_rename_all_used) {
            if (sb_len(rename_list) != data->group_count) {
                error("number (%zu) of benchmark groups to be renamed (supplied with "
                      "--rename-all) does not match number of benchmark groups (%zu)",
                      sb_len(rename_list), data->group_count);
                return false;
            }
        } else {
            for (size_t i = 0; i < sb_len(rename_list); ++i) {
                const struct rename_entry *re = rename_list + i;
                if (re->old_name != NULL) {
                    bool found = false;
                    for (size_t j = 0; j < data->group_count; ++j) {
                        if (strcmp(re->old_name, data->groups[j].name) == 0)
                            found = true;
                    }
                    if (!found) {
                        error("benchmark group with name '%s' (to be renamed to '%s') not "
                              "found",
                              re->old_name, re->name);
                        return false;
                    }
                } else if (re->n >= data->group_count) {
                    error("number (%zu) of benchmark group to be renamed ('%s') is too high",
                          rename_list[i].n + 1, rename_list[i].name);
                    return false;
                }
            }
        }
    }
    return true;
}

static bool attempt_rename(const struct rename_entry *rename_list, size_t idx,
                           const char **name)
{
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        const struct rename_entry *re = rename_list + i;
        bool matches = false;
        if (re->old_name != NULL)
            matches = strcmp(re->old_name, *name) == 0 ? true : false;
        else
            matches = re->n == idx;
        if (matches) {
            *name = rename_list[i].name;
            return true;
        }
    }
    return false;
}

static bool handle_renames(const struct rename_entry *rename_list, struct bench_data *data)
{
    if (!validate_rename_list(rename_list, data))
        return false;
    if (data->group_count == 0) {
        for (size_t bench_idx = 0; bench_idx < data->bench_count; ++bench_idx) {
            struct bench *bench = data->benches + bench_idx;
            (void)attempt_rename(rename_list, bench_idx, &bench->name);
        }
    } else {
        const struct bench_param *param = data->param;
        for (size_t grp_idx = 0; grp_idx < data->group_count; ++grp_idx) {
            struct bench_group *grp = data->groups + grp_idx;
            if (attempt_rename(rename_list, grp_idx, &grp->name)) {
                for (size_t grp_bench_idx = 0; grp_bench_idx < grp->bench_count;
                     ++grp_bench_idx) {
                    struct bench *bench = data->benches + grp->bench_idxs[grp_bench_idx];
                    bench->name = csfmt("%s %s=%s", grp->name, param->name,
                                        param->values[grp_bench_idx]);
                }
            }
        }
    }
    return true;
}

static void differentiate_groups_with_equal_names(struct bench_data *data)
{
    if (data->group_count <= 1)
        return;

    const struct bench_param *param = data->param;

    struct same_name {
        const char *name;
        size_t *idxs;
    } *same_names = NULL;

    for (size_t grp_idx = 0; grp_idx < data->group_count; ++grp_idx) {
        const struct bench_group *grp = data->groups + grp_idx;
        bool found = false;
        for (size_t i = 0; i < sb_len(same_names); ++i) {
            if (strcmp(same_names[i].name, grp->name) == 0) {
                sb_push(same_names[i].idxs, grp_idx);
                found = true;
                break;
            }
        }
        if (!found) {
            struct same_name *same_name = sb_new(same_names);
            same_name->name = grp->name;
            same_name->idxs = NULL;
            sb_push(same_name->idxs, grp_idx);
        }
    }

    for (size_t i = 0; i < sb_len(same_names); ++i) {
        struct same_name *same_name = same_names + i;
        size_t count = sb_len(same_name->idxs);
        if (count != 1) {
            for (size_t t_idx = 0; t_idx < count; ++t_idx) {
                size_t grp_idx = same_name->idxs[t_idx];
                struct bench_group *grp = data->groups + grp_idx;
                grp->name = csfmt("%s (%zu)", same_name->name, t_idx + 1);
                for (size_t grp_bench_idx = 0; grp_bench_idx < grp->bench_count;
                     ++grp_bench_idx) {
                    struct bench *bench = data->benches + grp->bench_idxs[grp_bench_idx];
                    bench->name = csfmt("%s %s=%s", grp->name, param->name,
                                        param->values[grp_bench_idx]);
                }
            }
        }
    }

    for (size_t i = 0; i < sb_len(same_names); ++i) {
        sb_free(same_names[i].idxs);
    }
    sb_free(same_names);
}

static void differentiate_benchmarks_with_equal_names(struct bench_data *data)
{
    struct same_name {
        const char *name;
        size_t *idxs;
    } *same_names = NULL;

    for (size_t bench_idx = 0; bench_idx < data->bench_count; ++bench_idx) {
        const struct bench *bench = data->benches + bench_idx;
        bool found = false;
        for (size_t i = 0; i < sb_len(same_names); ++i) {
            if (strcmp(same_names[i].name, bench->name) == 0) {
                sb_push(same_names[i].idxs, bench_idx);
                found = true;
                break;
            }
        }
        if (!found) {
            struct same_name *same_name = sb_new(same_names);
            same_name->name = bench->name;
            same_name->idxs = NULL;
            sb_push(same_name->idxs, bench_idx);
        }
    }

    for (size_t i = 0; i < sb_len(same_names); ++i) {
        struct same_name *same_name = same_names + i;
        size_t count = sb_len(same_name->idxs);
        if (count != 1) {
            for (size_t t_idx = 0; t_idx < count; ++t_idx) {
                size_t bench_idx = same_name->idxs[t_idx];
                data->benches[bench_idx].name = csfmt("%s (%zu)", same_name->name, t_idx + 1);
            }
        }
    }

    for (size_t i = 0; i < sb_len(same_names); ++i) {
        sb_free(same_names[i].idxs);
    }
    sb_free(same_names);
}

// When we initialize benchmarks in init_benches, we set default names. This function
// handles all other renaming that may happen with these names.
static bool set_bench_names(const struct rename_entry *rename_list, struct bench_data *data)
{
    if (!handle_renames(rename_list, data))
        return false;
    differentiate_groups_with_equal_names(data);
    differentiate_benchmarks_with_equal_names(data);
    return true;
}

static bool init_benches(const struct settings *settings,
                         const struct command_info *cmd_infos, bool has_groups,
                         struct bench_data *data)
{
    // Short path when there are no groups
    if (!has_groups) {
        size_t bench_count = sb_len(cmd_infos);
        data->bench_count = bench_count;
        sb_resize(data->run_descs, bench_count);
        sb_resize(data->benches, data->bench_count);
        for (size_t bench_idx = 0; bench_idx < sb_len(cmd_infos); ++bench_idx) {
            const struct command_info *cmd = cmd_infos + bench_idx;
            struct bench_run_desc *run_desc = data->run_descs + bench_idx;
            struct bench *bench = data->benches + bench_idx;
            if (!init_command(cmd, data->meas, data->meas_count, run_desc))
                return false;
            bench->name = cmd->name;
        }
    } else {
        assert(settings->has_param);
        size_t grp_count = sb_last(cmd_infos).grp_idx + 1;
        const struct bench_param *param = &settings->param;
        size_t bench_count = grp_count * param->value_count;
        size_t val_count = param->value_count;
        data->group_count = grp_count;
        data->bench_count = bench_count;
        sb_resize(data->run_descs, bench_count);
        sb_resize(data->benches, data->bench_count);
        sb_resize(data->groups, grp_count);
        for (size_t grp_idx = 0; grp_idx < grp_count; ++grp_idx) {
            struct bench_group *group = data->groups + grp_idx;
            group->name = cmd_infos[grp_idx * val_count].grp_name;
            group->bench_count = param->value_count;
            group->bench_idxs = calloc(val_count, sizeof(*group->bench_idxs));
            for (size_t val_idx = 0; val_idx < val_count; ++val_idx) {
                size_t bench_idx = grp_idx * val_count + val_idx;
                group->bench_idxs[val_idx] = bench_idx;
                struct bench_run_desc *run_desc = data->run_descs + bench_idx;
                const struct command_info *cmd = cmd_infos + bench_idx;
                struct bench *bench = data->benches + bench_idx;
                if (!init_command(cmd, data->meas, data->meas_count, run_desc)) {
                    free(group->bench_idxs);
                    return false;
                }
                bench->name =
                    csfmt("%s %s=%s", group->name, param->name, param->values[val_idx]);
            }
        }
    }
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        bench->meas_count = data->meas_count;
        bench->meas = calloc(bench->meas_count, sizeof(*bench->meas));
    }
    return true;
}

static bool init_commands(const struct settings *settings, struct bench_data *data)
{
    bool success = false;
    struct command_info *command_infos = NULL;
    if (!init_raw_command_infos(settings, &command_infos))
        return false;

    bool has_groups = false;
    if (settings->has_param) {
        int ret = multiplex_command_infos(&settings->param, &command_infos);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        case CMD_MULTIPLEX_SUCCESS:
            has_groups = true;
        }
    }

    if (!init_benches(settings, command_infos, has_groups, data))
        goto err;

    success = true;
err:
    sb_free(command_infos);
    return success;
}

static bool validate_and_set_baseline(const struct bench_data *data)
{
    size_t grp_count = data->group_count;
    size_t bench_count = data->bench_count;
    if (g_baseline == -1) {
        if (g_baseline_name != NULL) {
            if (grp_count <= 1) {
                for (size_t i = 0; i < bench_count; ++i) {
                    if (strcmp(data->benches[i].name, g_baseline_name) == 0) {
                        g_baseline = i;
                        break;
                    }
                }
                if (g_baseline == -1) {
                    error("there is no benchmark with name '%s' (specified as baseline using "
                          "--baseline-name)",
                          g_baseline_name);
                    return false;
                }
            } else {
                for (size_t i = 0; i < grp_count; ++i) {
                    if (strcmp(data->groups[i].name, g_baseline_name) == 0) {
                        g_baseline = i;
                        break;
                    }
                }
                if (g_baseline == -1) {
                    error("there is no benchmark group with name '%s' (specified as baseline "
                          "using --baseline-name)",
                          g_baseline_name);
                    return false;
                }
            }
        }
        return true;
    }

    assert(g_baseline > 0);
    size_t b = g_baseline - 1;
    if (grp_count <= 1) {
        if (b >= bench_count) {
            error("baseline number is too big");
            return false;
        }
    } else {
        // Multiple parameterized benchmarks
        if (b >= grp_count) {
            error("baseline number is too big");
            return false;
        }
    }
    g_baseline = b;
    return true;
}

static void set_sort_mode(void)
{
    if (g_sort_mode == SORT_DEFAULT) {
        if (g_baseline == -1)
            g_sort_mode = SORT_SPEED;
        else
            g_sort_mode = SORT_BASELINE_RAW;
        return;
    }
    assert(g_sort_mode == SORT_RAW || g_sort_mode == SORT_SPEED);
    if (g_baseline != -1) {
        if (g_sort_mode == SORT_RAW)
            g_sort_mode = SORT_BASELINE_RAW;
        else
            g_sort_mode = SORT_BASELINE_SPEED;
    }
}

static bool initialize_global_variables(const struct bench_data *data)
{
    if (!validate_and_set_baseline(data))
        return false;
    set_sort_mode();
    return true;
}

static bool init_run_info(const struct settings *settings, struct bench_data *data)
{
    if (sb_len(settings->meas) == 0) {
        error("no measurements specified");
        return false;
    }

    memset(data, 0, sizeof(*data));
    data->meas_count = sb_len(settings->meas);
    data->meas = settings->meas;
    if (settings->has_param)
        data->param = &settings->param;

    // Silently disable progress bar if output is inherit. The reasoning for
    // this is that inheriting output should only be used when debugging,
    // and user will not care about not having progress bar
    if (settings->output == OUTPUT_POLICY_INHERIT) {
        g_progress_bar = false;
    }

    if (!init_commands(settings, data))
        return false;

    bool has_custom_meas = false;
    for (size_t i = 0; i < sb_len(settings->meas); ++i) {
        if (settings->meas[i].kind == MEAS_CUSTOM ||
            settings->meas[i].kind == MEAS_CUSTOM_RE) {
            has_custom_meas = true;
            break;
        }
    }
    if (has_custom_meas) {
        for (size_t i = 0; i < data->bench_count; ++i) {
            if (!init_bench_stdout(data->run_descs + i))
                goto err;
        }
    }

    return true;
err:
    free_bench_data(data);
    return false;
}

void free_bench_data(struct bench_data *data)
{
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        sb_free(bench->exit_codes);
        for (size_t j = 0; j < data->meas_count; ++j)
            sb_free(bench->meas[j]);
        free(bench->meas);
        if (data->run_descs) {
            struct bench_run_desc *run_desc = data->run_descs + i;
            if (run_desc->stdout_fd != -1)
                close(run_desc->stdout_fd);
            if (run_desc->stdin_fd != -1)
                close(run_desc->stdin_fd);
            sb_free(run_desc->argv);
        }
    }
    sb_free(data->benches);
    sb_free(data->run_descs);
    for (size_t i = 0; i < data->group_count; ++i) {
        free(data->groups[i].bench_idxs);
    }
    sb_free(data->groups);
}

static bool do_save_bin(const struct bench_data *data)
{
    char name_buf[4096];
    if (g_override_bin_name)
        snprintf(name_buf, sizeof(name_buf), "%s", g_override_bin_name);
    else
        snprintf(name_buf, sizeof(name_buf), "%s/data.csbench", g_out_dir);
    FILE *f = fopen(name_buf, "wb");
    if (f == NULL) {
        csfmtperror("failed to open file '%s' for writing", name_buf);
        return false;
    }
    bool success = save_bench_data_binary(data, f);
    fclose(f);
    return success;
}

static bool do_app_bench(const struct settings *settings)
{
    bool success = false;
    struct bench_data data;
    if (!init_run_info(settings, &data))
        return false;
    if (!set_bench_names(settings->rename_list, &data))
        goto err;
    if (!initialize_global_variables(&data))
        goto err;
    if (!run_benches(data.run_descs, data.benches, data.bench_count))
        goto err;
    if (g_save_bin && !do_save_bin(&data))
        goto err;
    if (!do_analysis_and_make_report(&data))
        goto err;
    success = true;
err:
    free_bench_data(&data);
    return success;
}

static bool do_app_load_text(const struct settings *settings)
{
    bool success = false;
    const char **file_list = settings->args;
    if (file_list == NULL) {
        error("no files supplied to --load-text mode");
        return false;
    }
    struct bench_data_storage storage;
    struct bench_data data;
    if (!load_bench_data_text(file_list, &data, &storage))
        return false;
    if (!set_bench_names(settings->rename_list, &data))
        goto err;
    if (!initialize_global_variables(&data))
        goto err;
    if (!do_analysis_and_make_report(&data))
        goto err;
    success = true;
err:
    free_bench_data(&data);
    free_bench_data_storage(&storage);
    return success;
}

static bool get_bin_name(const char *src, const char **name, bool silent)
{
    struct stat st;
    if (stat(src, &st) == -1) {
        if (!silent)
            csfmtperror("failed to get information about file/directory '%s'", src);
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        *name = src;
    } else if (S_ISDIR(st.st_mode)) {
        const char *in_dir = csfmt("%s/data.csbench", src);
        if (access(in_dir, R_OK) == -1) {
            if (!silent)
                csfmtperror(
                    "'%s' is not a csbench data directory (file data.csbench not found)",
                    src);
            return false;
        }
        *name = in_dir;
    } else {
        if (!silent)
            error("file '%s' is invalid (expected regular file or directory)", src);
        return false;
    }
    return true;
}

static const char **calculate_bin_names(const char **args)
{
    // args is a list where each element can either be a directory name
    // which should contain data file with default name (as generated by
    // csbench), or a file itself. This dichotomy is OK because it is quite
    // intuitive and I can't think of a case where it would pose a problem.
    const char **names = NULL;
    for (size_t i = 0; i < sb_len(args); ++i) {
        const char *arg = args[i];
        const char *name = NULL;
        if (strcmp(arg, "-") == 0) {
            error("- command line option is not supported with --load-bin mode");
            goto err;
        } else if (!get_bin_name(arg, &name, false)) {
            goto err;
        }
        sb_push(names, name);
    }
    const char *name = NULL;
    if (names == NULL && get_bin_name(g_out_dir, &name, true))
        sb_push(names, name);
    if (names == NULL)
        error("no source csbench binary data files found");
    return names;
err:
    sb_free(names);
    return NULL;
}

static bool do_app_load_bin(const struct settings *settings)
{
    bool success = false;
    const char **src_list = calculate_bin_names(settings->args);
    if (src_list == NULL)
        return false;
    struct bench_data_storage storage;
    struct bench_data data;
    if (!load_bench_data_binary(src_list, &data, &storage))
        goto err;
    if (!set_bench_names(settings->rename_list, &data))
        goto err;
    if (!initialize_global_variables(&data))
        goto err;
    if (!do_analysis_and_make_report(&data))
        goto err;
    success = true;
err:
    sb_free(src_list);
    free_bench_data(&data);
    free_bench_data_storage(&storage);
    return success;
}

static bool ensure_out_dir_is_created(void)
{
    if (g_clear_out_dir && !rm_rf_dir(g_out_dir))
        return false;
    if (mkdir(g_out_dir, 0766) == -1) {
        if (errno == EEXIST) {
        } else {
            csfmtperror("failed to create directory '%s'", g_out_dir);
            return false;
        }
    }
    return true;
}

static bool run(const struct settings *settings)
{
    if (!ensure_out_dir_is_created())
        return false;

    switch (g_mode) {
    case APP_BENCH:
        return do_app_bench(settings);
    case APP_LOAD_TEXT:
        return do_app_load_text(settings);
    case APP_LOAD_BIN:
        return do_app_load_bin(settings);
    }
    ASSERT_UNREACHABLE();
}

static void sigint_handler(int sig)
{
    if (g_use_perf)
        perf_signal_cleanup();

    // Use default signal handler
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1)
        abort();
    raise(sig);
}

static void prepare(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigint_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1) {
        csperror("sigaction");
        exit(EXIT_FAILURE);
    }

    // --color=auto
    g_colored_output = isatty(STDOUT_FILENO) ? true : false;
    // --progress-bar=auto
    g_progress_bar = isatty(STDOUT_FILENO) ? true : false;

    init_rng_state();
}

int main(int argc, char **argv)
{
    prepare();

    int rc = EXIT_FAILURE;
    struct settings settings;
    parse_cli_args(argc, argv, &settings);

    if (run(&settings))
        rc = EXIT_SUCCESS;

    deinit_perf();
    free_settings(&settings);
    cs_free_strings();
    return rc;
}
