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
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
int g_threads = 1;
bool g_plot = false;
bool g_html = false;
bool g_csv = false;
bool g_plot_src = false;
int g_nresamp = 10000;
bool g_use_perf = false;
bool g_progress_bar = false;
bool g_regr = false;
bool g_python_output = false;
int g_baseline = -1;
enum app_mode g_mode = APP_BENCH;
struct bench_stop_policy g_warmup_stop = {0.1, 0, 1, 10};
struct bench_stop_policy g_bench_stop = {5.0, 0, 5, 0};
struct bench_stop_policy g_round_stop = {5.0, 0, 2, 0};
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
// with --inputd. When opening files and this variable is not null open it
// relative to this directory.
const char *g_inputd = NULL;

static bool replace_var_str(char *buf, size_t buf_size, const char *src,
                            const char *name, const char *value,
                            bool *replaced) {
    char *buf_end = buf + buf_size;
    size_t var_name_len = strlen(name);
    char *wr_cursor = buf;
    const char *rd_cursor = src;
    while (*rd_cursor) {
        if (*rd_cursor == '{' &&
            strncmp(rd_cursor + 1, name, var_name_len) == 0 &&
            rd_cursor[var_name_len + 1] == '}') {
            rd_cursor += 2 + var_name_len;
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

static char **split_shell_words(const char *cmd) {
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

static bool extract_exec_and_argv(const char *cmd_str, const char **exec,
                                  const char ***argv) {
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

static bool init_cmd_exec(const char *shell, const char *cmd_str,
                          const char **exec, const char ***argv) {
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

static void free_run_info(struct run_info *run_info) {
    for (size_t i = 0; i < sb_len(run_info->params); ++i) {
        struct bench_params *params = run_info->params + i;
        if (params->stdout_fd != -1)
            close(params->stdout_fd);
        if (params->stdin_fd != -1)
            close(params->stdin_fd);
        sb_free(params->argv);
    }
    sb_free(run_info->params);
    for (size_t i = 0; i < sb_len(run_info->groups); ++i) {
        struct bench_var_group *group = run_info->groups + i;
        free(group->cmd_idxs);
    }
    sb_free(run_info->groups);
}

static bool init_bench_stdin(const struct input_policy *input,
                             struct bench_params *params) {
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
            csfmterror(
                "failed to open file '%s' (designated for benchmark input)",
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

static bool init_bench_params(const char *name,
                              const struct input_policy *input,
                              enum output_kind output, const struct meas *meas,
                              const char *exec, const char **argv,
                              const char *cmd_str,
                              struct bench_params *params) {
    params->name = name;
    params->output = output;
    params->meas = meas;
    params->meas_count = sb_len(meas);
    params->exec = exec;
    params->argv = argv;
    params->str = cmd_str;
    params->stdout_fd = -1;
    return init_bench_stdin(input, params);
}

static bool init_bench_stdout(struct bench_params *params) {
    int fd = tmpfile_fd();
    if (fd == -1)
        return false;
    params->stdout_fd = fd;
    return true;
}

static bool attempt_group_rename(const struct rename_entry *rename_list,
                                 size_t grp_idx, struct bench_var_group *grp) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == grp_idx) {
            grp->name = rename_list[i].name;
            return true;
        }
    }
    return false;
}

static bool init_command(const struct command_info *cmd, struct run_info *info,
                         size_t *idx) {
    const char *exec = NULL, **argv = NULL;
    if (!init_cmd_exec(g_shell, cmd->cmd, &exec, &argv))
        return false;

    struct bench_params bench_params = {0};
    if (!init_bench_params(cmd->name, &cmd->input, cmd->output, info->meas,
                           exec, argv, (char *)cmd->cmd, &bench_params)) {
        sb_free(argv);
        return false;
    }
    sb_push(info->params, bench_params);
    if (idx)
        *idx = sb_len(info->params) - 1;
    return true;
}

static bool init_raw_command_infos(const struct cli_settings *cli,
                                   struct command_info **infos) {
    size_t cmd_count = sb_len(cli->args);
    if (cmd_count == 0) {
        error("no commands specified");
        return false;
    }
    for (size_t i = 0; i < cmd_count; ++i) {
        const char *cmd_str = cli->args[i];
        if (g_common_argstring) {
            cmd_str = csfmt("%s %s", cmd_str, g_common_argstring);
        }

        struct command_info info;
        memset(&info, 0, sizeof(info));
        info.name = info.cmd = cmd_str;
        info.output = cli->output;
        info.input = cli->input;
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
                           const struct bench_var *var,
                           struct command_info **multiplexed) {
    // Take first value and try to replace it in the command string
    char buf[4096];
    bool replaced = false;
    if (!replace_var_str(buf, sizeof(buf), src_info->cmd, var->name,
                         var->values[0], &replaced)) {
        error("Failed to substitute variable");
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    // We could reuse the string that is contained in buffer right now,
    // but it is a bit unecessary.
    for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
        const char *var_value = var->values[val_idx];
        if (!replace_var_str(buf, sizeof(buf), src_info->cmd, var->name,
                             var_value, &replaced)) {
            error("Failed to substitute variable");
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
multiplex_command_info_input(const struct command_info *src_info,
                             size_t src_idx, const struct bench_var *var,
                             struct command_info **multiplexed) {
    if (src_info->input.kind != INPUT_POLICY_FILE &&
        src_info->input.kind != INPUT_POLICY_STRING)
        return CMD_MULTIPLEX_NO_GROUPS;

    const char *src_string;
    if (src_info->input.kind == INPUT_POLICY_FILE)
        src_string = src_info->input.file;
    else if (src_info->input.kind == INPUT_POLICY_STRING)
        src_string = src_info->input.string;

    char buf[4096];
    bool replaced = false;
    if (!replace_var_str(buf, sizeof(buf), src_string, var->name,
                         var->values[0], &replaced)) {
        error("Failed to substitute variable");
        return CMD_MULTIPLEX_ERROR;
    }

    if (!replaced)
        return CMD_MULTIPLEX_NO_GROUPS;

    for (size_t val_idx = 0; val_idx < var->value_count; ++val_idx) {
        const char *var_value = var->values[val_idx];
        if (!replace_var_str(buf, sizeof(buf), src_string, var->name, var_value,
                             &replaced)) {
            error("Failed to substitute variable");
            return CMD_MULTIPLEX_ERROR;
        }
        assert(replaced);
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
            snprintf(buf, sizeof(buf), "%s <<< \"%s\"", info.cmd,
                     info.input.string);
            info.name = csstrdup(buf);
        }
        sb_push(*multiplexed, info);
    }
    return CMD_MULTIPLEX_SUCCESS;
}

static enum cmd_multiplex_result
multiplex_command_infos(const struct bench_var *var,
                        struct command_info **infos) {
    int ret = CMD_MULTIPLEX_NO_GROUPS;
    struct command_info *multiplexed = NULL;
    for (size_t src_idx = 0; src_idx < sb_len(*infos); ++src_idx) {
        const struct command_info *src_info = *infos + src_idx;

        ret = multiplex_command_info_cmd(src_info, src_idx, var, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        ret =
            multiplex_command_info_input(src_info, src_idx, var, &multiplexed);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_SUCCESS:
            continue;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        }

        error("command '%s' does not contain variable substitutions",
              src_info->cmd);
        goto err;
    }

    sb_free(*infos);
    *infos = multiplexed;
    return CMD_MULTIPLEX_SUCCESS;
err:
    sb_free(multiplexed);
    return CMD_MULTIPLEX_ERROR;
}

static bool init_benches(const struct cli_settings *cli,
                         const struct command_info *cmd_infos, bool has_groups,
                         struct run_info *info) {
    // Short path when there are no groups
    if (!has_groups) {
        for (size_t cmd_idx = 0; cmd_idx < sb_len(cmd_infos); ++cmd_idx) {
            const struct command_info *cmd = cmd_infos + cmd_idx;
            if (!init_command(cmd, info, NULL))
                return false;
        }
        return true;
    }

    size_t group_count = sb_last(cmd_infos).grp_idx + 1;
    const struct bench_var *var = NULL;
    if (cli->has_var)
        var = &cli->var;
    const struct command_info *cmd_cursor = cmd_infos;
    for (size_t grp_idx = 0; grp_idx < group_count; ++grp_idx) {
        assert(cmd_cursor->grp_idx == grp_idx);
        struct bench_var_group group = {0};
        if (!attempt_group_rename(cli->rename_list, sb_len(info->groups),
                                  &group))
            group.name = cmd_cursor->grp_name;
        group.cmd_idxs = calloc(var->value_count, sizeof(*group.cmd_idxs));
        for (size_t val_idx = 0; val_idx < var->value_count;
             ++val_idx, ++cmd_cursor) {
            assert(cmd_cursor->grp_idx == grp_idx);
            if (!init_command(cmd_cursor, info, group.cmd_idxs + val_idx)) {
                sb_free(group.cmd_idxs);
                return false;
            }
        }
        sb_push(info->groups, group);
    }
    return true;
}

static bool init_commands(const struct cli_settings *cli,
                          struct run_info *info) {
    bool result = false;
    struct command_info *command_infos = NULL;
    if (!init_raw_command_infos(cli, &command_infos))
        return false;

    bool has_groups = false;
    if (cli->has_var) {
        int ret = multiplex_command_infos(&cli->var, &command_infos);
        switch (ret) {
        case CMD_MULTIPLEX_ERROR:
            goto err;
        case CMD_MULTIPLEX_NO_GROUPS:
            break;
        case CMD_MULTIPLEX_SUCCESS:
            has_groups = true;
        }
    }

    if (!init_benches(cli, command_infos, has_groups, info))
        goto err;

    result = true;
err:
    sb_free(command_infos);
    return result;
}

static bool validate_and_set_baseline(int baseline,
                                      const struct run_info *info) {
    if (baseline > 0) {
        // Adjust number from human-readable to indexable
        size_t b = baseline - 1;
        size_t grp_count = sb_len(info->groups);
        size_t cmd_count = sb_len(info->params);
        if (grp_count <= 1) {
            // No parameterized benchmarks specified, just select the
            // command
            if (b >= cmd_count) {
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
    }
    return true;
}

static bool attempt_rename(const struct rename_entry *rename_list, size_t idx,
                           const char **name) {
    for (size_t i = 0; i < sb_len(rename_list); ++i) {
        if (rename_list[i].n == idx) {
            *name = rename_list[i].name;
            return true;
        }
    }
    return false;
}

static bool attempt_groupped_rename(const struct rename_entry *rename_list,
                                    size_t bench_idx,
                                    const struct bench_var_group *groups,
                                    const struct bench_var *var,
                                    const char **name) {
    assert(var);
    assert(groups);
    size_t value_count = var->value_count;
    for (size_t grp_idx = 0; grp_idx < sb_len(groups); ++grp_idx) {
        const struct bench_var_group *grp = groups + grp_idx;
        for (size_t val_idx = 0; val_idx < value_count; ++val_idx) {
            if (grp->cmd_idxs[val_idx] != bench_idx)
                continue;
            for (size_t rename_idx = 0; rename_idx < sb_len(rename_list);
                 ++rename_idx) {
                if (rename_list[rename_idx].n != grp_idx)
                    continue;
                *name = csfmt("%s %s=%s", rename_list[rename_idx].name,
                              var->name, var->values[val_idx]);
                return true;
            }
        }
    }
    return false;
}

static void set_param_names(const struct cli_settings *cli,
                            struct run_info *info) {
    for (size_t bench_idx = 0; bench_idx < sb_len(info->params); ++bench_idx) {
        struct bench_params *params = info->params + bench_idx;
        if (sb_len(info->groups) == 0) {
            attempt_rename(cli->rename_list, bench_idx, &params->name);
        } else {
            assert(cli->has_var);
            attempt_groupped_rename(cli->rename_list, bench_idx, info->groups,
                                    &cli->var, &params->name);
        }
    }
}

static bool validate_rename_list(const struct rename_entry *rename_list,
                                 size_t bench_count,
                                 const struct bench_var_group *groups) {
    if (sb_len(groups) == 0) {
        for (size_t i = 0; i < sb_len(rename_list); ++i) {
            if (rename_list[i].n >= bench_count) {
                error("number (%zu) of benchmark to be renamed ('%s') is too "
                      "high",
                      rename_list[i].n + 1, rename_list[i].name);
                return false;
            }
        }
    } else {
        for (size_t i = 0; i < sb_len(rename_list); ++i) {
            if (rename_list[i].n >= sb_len(groups)) {
                error("number (%zu) of benchmark to be renamed ('%s') is too "
                      "high",
                      rename_list[i].n + 1, rename_list[i].name);
                return false;
            }
        }
    }
    return true;
}

static bool init_run_info(const struct cli_settings *cli,
                          struct run_info *info) {
    info->meas = cli->meas;
    if (cli->has_var)
        info->var = &cli->var;

    // Silently disable progress bar if output is inherit. The reasoning for
    // this is that inheriting output should only be used when debugging,
    // and user will not care about not having progress bar
    if (cli->output == OUTPUT_POLICY_INHERIT) {
        g_progress_bar = false;
    }

    if (sb_len(cli->meas) == 0) {
        error("no measurements specified");
        return false;
    }

    if (!init_commands(cli, info))
        return false;

    bool has_custom_meas = false;
    for (size_t i = 0; i < sb_len(cli->meas); ++i) {
        if (cli->meas[i].kind == MEAS_CUSTOM) {
            has_custom_meas = true;
            break;
        }
    }
    if (has_custom_meas) {
        for (size_t i = 0; i < sb_len(info->params); ++i) {
            if (!init_bench_stdout(info->params + i))
                goto err;
        }
    }

    // Validate that baseline number (if specified) is not greater than
    // command count
    if (!validate_and_set_baseline(cli->baseline, info))
        goto err;

    if (!validate_rename_list(cli->rename_list, sb_len(info->params),
                              info->groups))
        goto err;

    set_param_names(cli, info);

    return true;
err:
    free_run_info(info);
    return false;
}

static void init_bench_data(const struct meas *meas, size_t meas_count,
                            const struct run_info *info,
                            struct bench_data *data) {
    assert(info->params);
    memset(data, 0, sizeof(*data));
    data->meas_count = meas_count;
    data->meas = meas;
    data->group_count = sb_len(info->groups);
    data->groups = info->groups;
    data->var = info->var;
    data->bench_count = sb_len(info->params);
    data->benches = calloc(data->bench_count, sizeof(*data->benches));
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        bench->meas_count = data->meas_count;
        bench->meas = calloc(data->meas_count, sizeof(*bench->meas));
        bench->name = info->params[i].name;
    }
}

static void init_bench_data_csv(const struct meas *meas, size_t meas_count,
                                const struct rename_entry *rename_list,
                                const char **file_list,
                                struct bench_data *data) {
    memset(data, 0, sizeof(*data));
    data->meas_count = meas_count;
    data->meas = meas;
    data->bench_count = sb_len(file_list);
    data->benches = calloc(data->bench_count, sizeof(*data->benches));
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        bench->meas_count = data->meas_count;
        bench->meas = calloc(data->meas_count, sizeof(*bench->meas));
        if (!attempt_rename(rename_list, i, &bench->name))
            bench->name = file_list[i];
    }
}

static void free_bench_data(struct bench_data *data) {
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        sb_free(bench->exit_codes);
        for (size_t i = 0; i < data->meas_count; ++i)
            sb_free(bench->meas[i]);
        free(bench->meas);
        sb_free(bench->stdout_offsets);
    }
    free(data->benches);
}

static bool do_app_bench(const struct cli_settings *cli) {
    bool success = false;
    struct run_info info = {0};
    if (!init_run_info(cli, &info))
        return false;
    struct bench_data data;
    init_bench_data(cli->meas, sb_len(cli->meas), &info, &data);
    if (!run_benches(info.params, data.benches, data.bench_count))
        goto err;
    if (!do_analysis_and_make_report(&data))
        goto err;
    success = true;
err:
    free_bench_data(&data);
    free_run_info(&info);
    return success;
}

static bool load_meas_names(const char *file, const char ***meas_names) {
    bool success = false;
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return false;

    char *line_buffer = NULL;
    size_t n = 0;
    if (getline(&line_buffer, &n, f) < 0)
        goto out;

    char *end = NULL;
    (void)strtod(line_buffer, &end);
    if (end == line_buffer) {
        *meas_names = parse_comma_separated_list(line_buffer);
        success = true;
    } else {
        success = true;
    }

    free(line_buffer);
out:
    fclose(f);
    return success;
}

static bool load_meas_from_csv_file(const struct cli_settings *settings,
                                    const char *file, struct meas **meas_list) {
    bool success = false;
    const char **file_meas_names = NULL;
    if (!load_meas_names(file, &file_meas_names)) {
        error("failed to load measurement names from file '%s'", file);
        goto out;
    }
    // Check if this is the first file. We only load measurements from the first
    // file, but all others should have the same measurement number.
    if (sb_len(*meas_list) != 0) {
        if (sb_len(file_meas_names) != sb_len(*meas_list)) {
            error("measurement number in different files does not match "
                  "(current file '%s'): %zu vs expected %zu",
                  file, sb_len(file_meas_names), sb_len(*meas_list));
            goto out;
        }
        success = true;
        goto out;
    }

    for (size_t meas_idx = 0; meas_idx < sb_len(file_meas_names); ++meas_idx) {
        const char *file_meas = file_meas_names[meas_idx];
        // First check if measurement with same name exists
        bool is_found = false;
        for (size_t test_idx = 0; test_idx < sb_len(*meas_list) && !is_found;
             ++test_idx) {
            if (strcmp(file_meas, (*meas_list)[test_idx].name) == 0)
                is_found = true;
        }
        // We have to add new measurement
        if (is_found)
            continue;

        // Scan measurements from user input
        const struct meas *meas_from_settings = NULL;
        for (size_t test_idx = 0;
             test_idx < sb_len(settings->meas) && meas_from_settings == NULL;
             ++test_idx)
            if (strcmp(file_meas, settings->meas[test_idx].name) == 0)
                meas_from_settings = settings->meas + test_idx;
        if (meas_from_settings) {
            sb_push(*meas_list, *meas_from_settings);
            continue;
        }

        struct meas meas = {"", NULL, {MU_NONE, ""}, MEAS_LOADED, false, 0};
        meas.name = file_meas;
        // Try to guess and use seconds as measurement unit for first
        // measurement
        if (meas_idx == 0)
            meas.units.kind = MU_S;
        sb_push(*meas_list, meas);
    }
    success = true;
out:
    sb_free(file_meas_names);
    return success;
}

static bool load_meas_from_csv(const struct cli_settings *settings,
                               const char **file_list,
                               struct meas **meas_list) {
    for (size_t file_idx = 0; file_idx < sb_len(file_list); ++file_idx) {
        const char *file = file_list[file_idx];
        if (!load_meas_from_csv_file(settings, file, meas_list))
            return false;
    }
    return true;
}

static bool do_app_load_csv(const struct cli_settings *settings) {
    bool success = false;
    const char **file_list = settings->args;
    // We know that each CSV file should belong to separate benchmark,
    // so we can validate rename list before we actually initialize benchmarks
    if (!validate_rename_list(settings->rename_list, sb_len(file_list), NULL))
        return false;
    struct meas *meas_list = NULL;
    if (!load_meas_from_csv(settings, file_list, &meas_list))
        return false;
    struct bench_data data;
    init_bench_data_csv(meas_list, sb_len(meas_list), settings->rename_list,
                        file_list, &data);
    if (!load_bench_data_csv(file_list, &data))
        goto err;
    if (!do_analysis_and_make_report(&data))
        goto err;
    success = true;
err:
    sb_free(meas_list);
    free_bench_data(&data);
    return success;
}

static bool run(const struct cli_settings *cli) {
    switch (g_mode) {
    case APP_BENCH:
        return do_app_bench(cli);
    case APP_LOAD_CSV:
        return do_app_load_csv(cli);
    }

    return false;
}

static void sigint_handler(int sig) {
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

static void prepare(void) {
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

int main(int argc, char **argv) {
    prepare();

    int rc = EXIT_FAILURE;
    struct cli_settings cli = {0};
    parse_cli_args(argc, argv, &cli);

    if (run(&cli))
        rc = EXIT_SUCCESS;

    deinit_perf();
    free_cli_settings(&cli);
    cs_free_strings();
    return rc;
}
