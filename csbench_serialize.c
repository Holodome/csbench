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
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <regex.h>

struct csbench_binary_header {
    uint32_t magic;
    uint32_t version;

    uint64_t meas_count;
    uint64_t bench_count;
    uint64_t group_count;

    uint8_t has_param;
    uint8_t reserved0[7];

    uint64_t param_offset;
    uint64_t param_size;
    uint64_t meas_offset;
    uint64_t meas_size;
    uint64_t groups_offset;
    uint64_t groups_size;
    uint64_t bench_data_offset;
    uint64_t bench_data_size;
};

struct parsed_text_data_line {
    char *name;
    size_t value_count;
    double *values;
};

struct parsed_text_file {
    const char *filename;
    char *meas_name;
    char *meas_units;
    char *extract_str;
    size_t line_count;
    struct parsed_text_data_line *lines;
};

#define CSBENCH_MAGIC (uint32_t)('C' | ('S' << 8) | ('B' << 16) | ('H' << 24))

#define write_raw__(_arr, _elemsz, _cnt, _f)                                                 \
    do {                                                                                     \
        size_t CSUNIQIFY(cnt) = (_cnt);                                                      \
        if (fwrite(_arr, _elemsz, CSUNIQIFY(cnt), _f) != CSUNIQIFY(cnt))                     \
            goto io_err;                                                                     \
    } while (0)

#define write_u64__(_value, _f)                                                              \
    do {                                                                                     \
        uint64_t CSUNIQIFY(value) = (_value);                                                \
        write_raw__(&CSUNIQIFY(value), sizeof(uint64_t), 1, _f);                             \
    } while (0)

#define write_str__(_str, _f)                                                                \
    do {                                                                                     \
        const char *CSUNIQIFY(str) = (_str);                                                 \
        if (CSUNIQIFY(str) == NULL) {                                                        \
            uint32_t CSUNIQIFY(len) = 0;                                                     \
            write_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);                            \
        } else {                                                                             \
            uint32_t CSUNIQIFY(len) = strlen(CSUNIQIFY(str)) + 1;                            \
            write_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);                            \
            write_raw__(CSUNIQIFY(str), 1, CSUNIQIFY(len), f);                               \
        }                                                                                    \
    } while (0)

bool save_bench_data_binary(const struct bench_data *data, FILE *f)
{
    struct csbench_binary_header header = {0};
    header.magic = CSBENCH_MAGIC;
    header.version = 1;
    header.meas_count = data->meas_count;
    header.bench_count = data->bench_count;
    header.group_count = data->group_count;

    uint64_t cursor = sizeof(struct csbench_binary_header);
    assert(!(cursor & 0x7));

    if (data->param != NULL) {
        header.has_param = 1;
        header.param_offset = cursor;
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        write_str__(data->param->name, f);
        write_u64__(data->param->value_count, f);
        for (size_t i = 0; i < data->param->value_count; ++i)
            write_str__(data->param->values[i], f);

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            return false;
        }
        header.param_size = (uint64_t)at - header.param_offset;
        cursor = ((uint64_t)at + 0x7) & ~0x7;
    }

    {
        assert(data->meas_count);
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        header.meas_offset = cursor;
        for (size_t i = 0; i < data->meas_count; ++i) {
            const struct meas *meas = data->meas + i;
            write_str__(meas->name, f);
            write_str__(meas->cmd, f);
            write_u64__(meas->units.kind, f);
            write_str__(meas->units.str, f);
            write_u64__(meas->kind, f);
            write_u64__(meas->is_secondary, f);
            write_u64__(meas->primary_idx, f);
        }

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            return false;
        }
        header.meas_size = (uint64_t)at - header.meas_offset;
        cursor = ((uint64_t)at + 0x7) & ~0x7;
    }

    if (data->group_count != 0) {
        assert(data->param);
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        header.groups_offset = cursor;
        for (size_t i = 0; i < data->group_count; ++i) {
            const struct bench_group *grp = data->groups + i;
            write_str__(grp->name, f);
            assert(grp->bench_count == data->param->value_count);
            write_u64__(grp->bench_count, f);
            for (size_t j = 0; j < grp->bench_count; ++j)
                write_u64__(grp->bench_idxs[j], f);
        }

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            return false;
        }
        header.groups_size = (uint64_t)at - header.groups_offset;
        cursor = ((uint64_t)at + 0x7) & ~0x7;
    }

    {
        assert(data->bench_count);
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        header.bench_data_offset = cursor;

        for (size_t i = 0; i < data->bench_count; ++i) {
            const struct bench *bench = data->benches + i;
            write_str__(bench->name, f);
            write_u64__(bench->run_count, f);
            write_raw__(bench->exit_codes, sizeof(int), bench->run_count, f);
            for (size_t j = 0; j < data->meas_count; ++j)
                write_raw__(bench->meas[j], sizeof(double), bench->run_count, f);
        }

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            return false;
        }
        header.bench_data_size = (uint64_t)at - header.bench_data_offset;
    }

    if (fseek(f, 0, SEEK_SET) == -1) {
        csperror("fseek");
        return false;
    }
    write_raw__(&header, sizeof(header), 1, f);
    return true;
io_err:
    csperror("IO error when writing csbench data file");
    return false;
}

#undef write_raw__
#undef write_u64__
#undef write_str__

#define read_raw__(_dst, _elemsz, _cnt, _f)                                                  \
    do {                                                                                     \
        size_t CSUNIQIFY(cnt) = (_cnt);                                                      \
        if (fread(_dst, _elemsz, CSUNIQIFY(cnt), _f) != CSUNIQIFY(cnt))                      \
            goto io_err;                                                                     \
    } while (0)

#define read_u64__(_dst, _f)                                                                 \
    do {                                                                                     \
        uint64_t CSUNIQIFY(value);                                                           \
        read_raw__(&CSUNIQIFY(value), sizeof(uint64_t), 1, f);                               \
        (_dst) = CSUNIQIFY(value);                                                           \
    } while (0)

#define read_str__(_dst, _f)                                                                 \
    do {                                                                                     \
        uint32_t CSUNIQIFY(len);                                                             \
        read_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);                                 \
        char *CSUNIQIFY(res) = NULL;                                                         \
        if (CSUNIQIFY(len) != 0) {                                                           \
            CSUNIQIFY(res) = csstralloc(CSUNIQIFY(len) - 1);                                 \
            read_raw__(CSUNIQIFY(res), 1, CSUNIQIFY(len), f);                                \
        }                                                                                    \
        (_dst) = (const char *)CSUNIQIFY(res);                                               \
    } while (0)

static bool load_bench_data_binary_file_internal(FILE *f, const char *filename,
                                                 struct bench_data *data,
                                                 struct bench_data_storage *storage)
{
    memset(storage, 0, sizeof(*storage));
    memset(data, 0, sizeof(*data));

    struct csbench_binary_header header;
    read_raw__(&header, sizeof(header), 1, f);
    if (header.magic != CSBENCH_MAGIC) {
        error("invlaid magic number in csbench data file '%s'", filename);
        return false;
    }
    if (header.version != 1) {
        error("invalid version in csbench data file '%s'", filename);
        return false;
    }

    if (header.has_param) {
        if (fseek(f, header.param_offset, SEEK_SET) == -1)
            goto fseek_err;

        storage->has_param = true;
        read_str__(storage->param.name, f);
        read_u64__(storage->param.value_count, f);
        sb_resize(storage->param.values, storage->param.value_count);
        for (size_t i = 0; i < storage->param.value_count; ++i)
            read_str__(storage->param.values[i], f);
        data->param = &storage->param;

        int at = ftell(f);
        if (at == -1)
            goto ftell_err;
        if ((uint64_t)at != header.param_offset + header.param_size)
            goto corrupted;
    }

    if (header.meas_count == 0)
        goto corrupted;
    {
        if (fseek(f, header.meas_offset, SEEK_SET) == -1)
            goto fseek_err;

        storage->meas_count = header.meas_count;
        sb_resize(storage->meas, header.meas_count);
        for (size_t i = 0; i < header.meas_count; ++i) {
            struct meas *meas = storage->meas + i;
            read_str__(meas->name, f);
            read_str__(meas->cmd, f);
            read_u64__(meas->units.kind, f);
            read_str__(meas->units.str, f);
            read_u64__(meas->kind, f);
            read_u64__(meas->is_secondary, f);
            read_u64__(meas->primary_idx, f);
        }
        data->meas_count = storage->meas_count;
        data->meas = storage->meas;

        int at = ftell(f);
        if (at == -1)
            goto ftell_err;
        if ((uint64_t)at != header.meas_offset + header.meas_size)
            goto corrupted;
    }

    if (header.group_count) {
        if (!data->param)
            goto corrupted;
        if (fseek(f, header.groups_offset, SEEK_SET) == -1)
            goto fseek_err;

        data->group_count = header.group_count;
        sb_resize(data->groups, header.group_count);
        for (size_t i = 0; i < header.group_count; ++i) {
            struct bench_group *grp = data->groups + i;
            read_str__(grp->name, f);
            read_u64__(grp->bench_count, f);
            if (grp->bench_count != data->param->value_count)
                goto corrupted;
            grp->bench_idxs = calloc(grp->bench_count, sizeof(*grp->bench_idxs));
            for (size_t j = 0; j < grp->bench_count; ++j)
                read_u64__(grp->bench_idxs[j], f);
        }

        int at = ftell(f);
        if (at == -1)
            goto ftell_err;
        if ((uint64_t)at != header.groups_offset + header.groups_size)
            goto corrupted;
    }

    if (header.bench_count == 0)
        goto corrupted;
    {
        if (fseek(f, header.bench_data_offset, SEEK_SET) == -1)
            goto fseek_err;

        data->bench_count = header.bench_count;
        sb_resize(data->benches, header.bench_count);
        for (size_t i = 0; i < header.bench_count; ++i) {
            struct bench *bench = data->benches + i;
            read_str__(bench->name, f);
            bench->meas = calloc(data->meas_count, sizeof(*bench->meas));
            read_u64__(bench->run_count, f);
            bench->meas_count = data->meas_count;
            sb_resize(bench->exit_codes, bench->run_count);
            read_raw__(bench->exit_codes, sizeof(int), bench->run_count, f);
            for (size_t j = 0; j < data->meas_count; ++j) {
                sb_resize(bench->meas[j], bench->run_count);
                read_raw__(bench->meas[j], sizeof(double), bench->run_count, f);
            }
        }

        int at = ftell(f);
        if (at == -1)
            goto ftell_err;
        if ((uint64_t)at != header.bench_data_offset + header.bench_data_size)
            goto corrupted;
    }

    return true;
corrupted:
    error("csbench data file '%s' is corrupted", filename);
    goto err_raw;
fseek_err:
    csfmtperror("fseek on '%s'", filename);
    goto err_raw;
ftell_err:
    csfmtperror("ftell on '%s'", filename);
    goto err_raw;
io_err:
    csfmtperror("IO error reading csbench data file '%s'", filename);
    goto err_raw;
err_raw:
    if (data->benches) {
        for (size_t i = 0; i < data->bench_count; ++i) {
            struct bench *bench = data->benches + i;
            sb_free(bench->exit_codes);
            for (size_t j = 0; j < bench->meas_count; ++j)
                sb_free(bench->meas[j]);
            free(bench->meas);
        }
        free(data->benches);
    }
    free_bench_data_storage(storage);
    return false;
}

#undef read_raw__
#undef read_u64__
#undef read_str__

static bool load_bench_data_binary_file(const char *filename, struct bench_data *data,
                                        struct bench_data_storage *storage)
{
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        csfmtperror("failed to open file '%s' for reading", filename);
        return false;
    }
    bool success = load_bench_data_binary_file_internal(f, filename, data, storage);
    fclose(f);
    return success;
}

void free_bench_data_storage(struct bench_data_storage *storage)
{
    if (storage->has_param)
        sb_free(storage->param.values);
    if (storage->meas)
        sb_free(storage->meas);
}

static bool meas_match(const struct meas *a, const struct meas *b)
{
    if (strcmp(a->name, b->name) != 0)
        return false;
    if ((a->cmd != NULL) != (b->cmd != NULL))
        return false;
    if (a->cmd != NULL && strcmp(a->cmd, b->cmd) != 0)
        return false;
    if (a->units.kind != b->units.kind)
        return false;
    if (a->units.str != NULL && strcmp(a->units.str, b->units.str) != 0)
        return false;
    if (a->is_secondary != b->is_secondary)
        return false;
    if (a->primary_idx != b->primary_idx)
        return false;
    return true;
}

static bool params_match(const struct bench_param *a, const struct bench_param *b)
{
    if (strcmp(a->name, b->name) != 0)
        return false;
    if (a->value_count != b->value_count)
        return false;
    for (size_t i = 0; i < a->value_count; ++i) {
        if (strcmp(a->values[i], b->values[i]) != 0)
            return false;
    }
    return true;
}

static bool bench_data_match(const struct bench_data *a, const struct bench_data *b)
{
    if (a->meas_count != b->meas_count)
        return false;
    for (size_t j = 0; j < a->meas_count; ++j) {
        if (!meas_match(a->meas + j, b->meas + j))
            return false;
    }
    if (((a->param != NULL) != (b->param != NULL)))
        return false;
    if (a->param != NULL && !params_match(a->param, b->param))
        return false;
    return true;
}

static bool merge_bench_data(struct bench_data *src_datas,
                             struct bench_data_storage *src_storages, size_t src_count,
                             struct bench_data *data, struct bench_data_storage *storage)
{
    assert(src_count >= 2);
    size_t total_bench_count = src_datas[0].bench_count;
    size_t total_group_count = src_datas[0].group_count;
    for (size_t i = 1; i < src_count; ++i) {
        if (!bench_data_match(src_datas + 0, src_datas + i)) {
            error("loaded benchmarks structure does not match");
            return false;
        }
        total_bench_count += src_datas[i].bench_count;
        total_group_count += src_datas[i].group_count;
    }

    memset(data, 0, sizeof(*data));
    memset(storage, 0, sizeof(*storage));

    // Move measurements and param
    storage->has_param = src_storages[0].has_param;
    if (storage->has_param)
        data->param = &storage->param;
    memcpy(&storage->param, &src_storages[0].param, sizeof(storage->param));
    data->meas_count = storage->meas_count = src_storages[0].meas_count;
    data->meas = storage->meas = src_storages[0].meas;
    src_storages[0].has_param = false;
    src_storages[0].meas_count = 0;
    src_storages[0].meas = NULL;
    // Make new benchmark list
    data->bench_count = total_bench_count;
    sb_resize(data->benches, total_bench_count);
    for (size_t i = 0, bench_cursor = 0; i < src_count; ++i) {
        struct bench_data *src = src_datas + i;
        memcpy(data->benches + bench_cursor, src->benches,
               sizeof(struct bench) * src->bench_count);
        bench_cursor += src->bench_count;
        // Free data in source as if it never had it
        sb_free(src->benches);
        src->benches = NULL;
        src->bench_count = 0;
    }
    // Make new group list
    if (data->param) {
        data->group_count = total_group_count;
        sb_resize(data->groups, total_group_count);
        for (size_t i = 0, group_cursor = 0, bench_cursor = 0; i < src_count; ++i) {
            struct bench_data *src = src_datas + i;
            memcpy(data->groups + group_cursor, src->groups,
                   sizeof(struct bench_group) * src->group_count);
            // Fixup command indexes
            for (size_t j = 0; j < src->group_count; ++j) {
                assert(src->groups[j].bench_count == data->param->value_count);
                for (size_t k = 0; k < src->groups[j].bench_count; ++k)
                    data->groups[group_cursor + j].bench_idxs[k] += bench_cursor;
                bench_cursor += src->groups[j].bench_count;
            }
            group_cursor += src->group_count;
            // Free data in source as if it never had it
            sb_free(src->groups);
            src->group_count = 0;
        }
    }
    return true;
}

static bool load_bench_data_binary_merge(const char **file_list, struct bench_data *data,
                                         struct bench_data_storage *storage)
{
    bool success = false;
    size_t src_count = sb_len(file_list);
    struct bench_data *src_datas = calloc(src_count, sizeof(*src_datas));
    struct bench_data_storage *src_storages = calloc(src_count, sizeof(*src_storages));
    for (size_t i = 0; i < src_count; ++i) {
        if (!load_bench_data_binary_file(file_list[i], src_datas + i, src_storages + i))
            goto err;
    }

    if (!merge_bench_data(src_datas, src_storages, src_count, data, storage))
        goto err;

    success = true;
err:
    for (size_t i = 0; i < src_count; ++i) {
        free_bench_data_storage(src_storages + i);
        free_bench_data(src_datas + i);
    }
    free(src_datas);
    free(src_storages);
    return success;
}

bool load_bench_data_binary(const char **file_list, struct bench_data *data,
                            struct bench_data_storage *storage)
{
    bool success = false;
    size_t src_count = sb_len(file_list);
    assert(src_count > 0);
    if (src_count == 1)
        success = load_bench_data_binary_file(file_list[0], data, storage);
    else
        success = load_bench_data_binary_merge(file_list, data, storage);
    return success;
}

static bool keyword_val(const char *str, const char *kw, const char *filename, char *val_buf,
                        size_t val_buf_size, bool *has_match)
{
    size_t str_len = strlen(str);
    size_t kw_len = strlen(kw);
    bool matches = strncmp(kw, str, kw_len) == 0 ? true : false;
    if (matches) {
        matches = str[kw_len] == '=';
        if (matches) {
            const char *value_cursor = str + kw_len + 1;
            size_t len = (str + str_len) - value_cursor;
            if (*value_cursor == '\'') {
                if (str[str_len - 1] != '\'') {
                    error("unterminated keyword %s value in file '%s'", kw, filename);
                    return false;
                }
                value_cursor += 1;
                len -= 2;
            }
            if (val_buf_size + 1 < len) {
                error("too long keyword %s value in file '%s'", kw, filename);
                return false;
            }
            memcpy(val_buf, value_cursor, len);
            val_buf[len] = '\0';
        }
    }
    *has_match = matches;
    return true;
}

static bool handle_text_header_tok(const char *tok, struct parsed_text_file *file)
{
    char val_buf[256];
    bool has_match;
    {
        if (!keyword_val(tok, "meas", file->filename, val_buf, sizeof(val_buf), &has_match))
            return false;
        if (has_match) {
            file->meas_name = strdup(val_buf);
            return true;
        }
    }
    {
        if (!keyword_val(tok, "units", file->filename, val_buf, sizeof(val_buf), &has_match))
            return false;
        if (has_match) {
            file->meas_units = strdup(val_buf);
            return true;
        }
    }
    {
        if (!keyword_val(tok, "extract", file->filename, val_buf, sizeof(val_buf),
                         &has_match))
            return false;
        if (has_match) {
            file->extract_str = strdup(val_buf);
            return true;
        }
    }
    error("invalid header keyword '%s' found in file '%s'", tok, file->filename);
    return false;
}

static bool parse_text_header(const char *line, struct parsed_text_file *file)
{
    assert(line[0] == '#');
    const char *cursor = line + 1;
    for (;;) {
        while (isspace(*cursor))
            ++cursor;
        if (*cursor == '\0')
            break;
        const char *sep = cursor + 1;
        for (;; ++sep) {
            if (*sep == '\0') {
                sep = NULL;
                break;
            }
            if (*sep == '\'') {
                ++sep;
                for (;; ++sep) {
                    if (*sep == '\0') {
                        sep = NULL;
                        error("unterminated header string in file '%s'", file->filename);
                        return false;
                    }
                    if (*sep == '\'') {
                        ++sep;
                        break;
                    }
                }
            }
            if (*sep == '\t' || *sep == ' ')
                break;
        }
        if (sep == NULL) {
            if (!handle_text_header_tok(cursor, file))
                return false;
            break;
        }
        char buf[4096];
        size_t len = sep - cursor;
        if (len > sizeof(buf) - 1) {
            error("invalid header format in file '%s'", file->filename);
            return false;
        }
        memcpy(buf, cursor, len);
        buf[len] = '\0';
        if (!handle_text_header_tok(buf, file))
            return false;
        cursor = sep + 1;
    }
    return true;
}

static bool parse_text_line(const char *line, struct parsed_text_file *file)
{
    const char *comma = strchr(line, ',');
    if (comma == NULL) {
        error("invalid line format in file '%s'", file->filename);
        return false;
    }

    size_t name_len = comma - line;
    char *name = malloc(name_len + 1);
    memcpy(name, line, name_len);
    name[name_len] = '\0';

    char buf[4096];
    double *values = NULL;
    const char *cursor = comma + 1;
    char *str_end = NULL;
    for (;;) {
        comma = strchr(cursor, ',');
        if (comma == NULL) {
            double v = strtod(cursor, &str_end);
            if (str_end == cursor) {
                error("invalid data format in file '%s'", file->filename);
                goto err;
            }
            sb_push(values, v);
            break;
        }
        size_t len = comma - cursor;
        if (len > sizeof(buf) - 1) {
            error("invalid data format in file '%s'", file->filename);
            goto err;
        }
        memcpy(buf, cursor, len);
        buf[len] = '\0';

        double v = strtod(cursor, &str_end);
        if (str_end == cursor) {
            error("invalid data format in file '%s'", file->filename);
            goto err;
        }
        sb_push(values, v);
        cursor = comma + 1;
    }

    struct parsed_text_data_line data;
    data.name = name;
    data.value_count = sb_len(values);
    data.values = values;
    sb_push(file->lines, data);
    ++file->line_count;
    return true;
err:
    free(name);
    sb_free(values);
    return false;
}

static bool load_parsed_text_file_internal(FILE *f, struct parsed_text_file *file)
{
    bool success = false;
    char *line = NULL;

    for (size_t line_idx = 0;; ++line_idx) {
        size_t len = 0;
        errno = 0;
        ssize_t nread = getline(&line, &len, f);
        if (nread == -1) {
            if (errno != 0) {
                csfmtperror("failed to read line from file '%s'", file->filename);
                goto err;
            }
            break;
        }
        for (ssize_t i = 0; nread > 1 && isspace(line[nread - i - 1]); ++i)
            line[nread - i - 1] = '\0';

        bool parsed = false;
        if (line_idx == 0 && line[0] == '#')
            parsed = parse_text_header(line, file);
        else
            parsed = parse_text_line(line, file);
        if (!parsed)
            goto err;
    }

    success = true;
err:
    free(line);
    return success;
}

static void free_parsed_text_file(struct parsed_text_file *file)
{
    free(file->meas_name);
    free(file->meas_units);
    free(file->extract_str);
    for (size_t i = 0; i < file->line_count; ++i) {
        struct parsed_text_data_line *line = file->lines + i;
        if (line->name)
            free(line->name);
        sb_free(line->values);
    }
    sb_free(file->lines);
}

static bool load_parsed_text_file(const char *filename, struct parsed_text_file *file)
{
    bool is_stdin = strcmp(filename, "-") == 0 ? true : false;
    FILE *f = NULL;
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(filename, "r");
        if (f == NULL) {
            csfmtperror("failed to open file '%s' for reading", filename);
            return false;
        }
    }
    memset(file, 0, sizeof(*file));
    file->filename = filename;
    bool success = load_parsed_text_file_internal(f, file);
    if (!is_stdin)
        fclose(f);
    if (!success)
        free_parsed_text_file(file);
    return success;
}

static void init_parsed_text_meas(const struct parsed_text_file *parsed, struct meas *measp)
{
    if (parsed->meas_name == NULL && parsed->meas_units == NULL) {
        *measp = BUILTIN_MEASUREMENTS[MEAS_WALL];
        return;
    }
    if (parsed->meas_name != NULL) {
        enum meas_kind kind;
        if (parse_meas_str(parsed->meas_name, &kind)) {
            *measp = BUILTIN_MEASUREMENTS[kind];
            return;
        }
    }
    struct meas meas = {"meas", NULL, NULL, {MU_NONE, NULL}, MEAS_CUSTOM, false, 0};
    if (parsed->meas_units) {
        parse_units_str(parsed->meas_units, &meas.units);
        if (meas.units.str != NULL) {
            meas.units.str = csstrdup(meas.units.str);
        }
    }
    if (parsed->meas_name) {
        meas.name = csstrdup(parsed->meas_name);
    }
    *measp = meas;
}

static bool validate_extract_str(const char *extract_str, const char *filename,
                                 bool *has_paramp, char *param_name_buf,
                                 size_t param_name_buf_size)
{
    bool has_name = false;
    bool has_param = false;
    const char *cursor = extract_str;
    for (;;) {
        const char *pat_start = strchr(cursor, '{');
        if (pat_start == NULL)
            break;

        ++pat_start;
        const char *pat_end = strchr(pat_start, '}');
        if (pat_end == NULL) {
            error("unterminated extract str substitution in file '%s'", filename);
            return false;
        }

        size_t pat_len = pat_end - pat_start;
        if (pat_len == 4 && memcmp(pat_start, "name", 4) == 0) {
            if (has_name) {
                error("multiple extract str name substitutions found in file '%s'", filename);
                return false;
            }
            has_name = true;
        } else {
            if (has_param) {
                error("multiple extract str parameter substitutions found in file '%s'",
                      filename);
                return false;
            }
            if (param_name_buf_size < pat_len + 1) {
                error("too long paramater name in file '%s'", filename);
                return false;
            }
            memcpy(param_name_buf, pat_start, pat_len);
            param_name_buf[pat_len] = '\0';
            has_param = true;
        }
        cursor = pat_end + 1;
    }
    if (!has_name && !has_param) {
        error("extract str is missing substitutions in file '%s'", filename);
        return false;
    }
    if (!has_name && has_param) {
        error(
            "extract str has parameter substitution but lacks name substitution in file '%s'",
            filename);
        return false;
    }
    *has_paramp = has_param;
    return true;
}

static char *extract_str_to_regex(const char *src, bool *name_is_first)
{
    char *regex_str = NULL;
    for (size_t subst_idx = 0;;) {
        if (*src == '\0')
            break;
        if (*src == '{') {
            ++src;
            sb_push(regex_str, '(');
            sb_push(regex_str, '.');
            sb_push(regex_str, '*');
            sb_push(regex_str, ')');
            const char *closing = strchr(src, '}');
            if (closing - src == 4 && memcmp(src, "name", 4) == 0) {
                if (subst_idx == 0)
                    *name_is_first = true;
            }
            assert(closing);
            src = closing + 1;
            ++subst_idx;
            continue;
        }
        sb_push(regex_str, *src++);
    }
    sb_push(regex_str, '\0');
    return regex_str;
}

static bool extract_name_and_param(regex_t *regex, const char *regex_str, bool name_is_first,
                                   const char *src, const char *filename, char *name_buf,
                                   char *param_buf, size_t buf_size)
{
    regmatch_t matches[3];
    int ret = regexec(regex, src, 3, matches, 0);
    if (ret != 0 && ret != REG_NOMATCH) {
        char errbuf[4096];
        regerror(ret, regex, errbuf, sizeof(errbuf));
        error("error executing regex '%s': %s", regex_str, errbuf);
        return false;
    } else if (ret == REG_NOMATCH) {
        error("benchmark name does not match extract str in file '%s'", filename);
        return false;
    }
    const regmatch_t *name_match = matches + 1, *param_match = matches + 2;
    if (!name_is_first) {
        name_match = matches + 2;
        param_match = matches + 1;
    }
    {
        size_t name_off = name_match->rm_so;
        size_t name_len = name_match->rm_eo - name_match->rm_so;
        if (buf_size < name_len + 1) {
            error("too long name value in file '%s'", filename);
            return false;
        }
        memcpy(name_buf, src + name_off, name_len);
        name_buf[name_len] = '\0';
    }
    {
        size_t param_off = param_match->rm_so;
        size_t param_len = param_match->rm_eo - param_match->rm_so;
        if (buf_size < param_len + 1) {
            error("too long parameter value in file '%s'", filename);
            return false;
        }
        memcpy(param_buf, src + param_off, param_len);
        param_buf[param_len] = '\0';
    }
    return true;
}

struct extract_str_data {
    struct group_info {
        char *name;
        size_t count;
    } *group_infos;
    char **param_values;
    struct bench_info {
        char *name;
        char *value;
    } *benches;
};

static void free_extract_str_data(struct extract_str_data *data)
{
    for (size_t i = 0; i < sb_len(data->benches); ++i) {
        free(data->benches[i].name);
        free(data->benches[i].value);
    }
    sb_free(data->benches);
    for (size_t i = 0; i < sb_len(data->group_infos); ++i)
        free(data->group_infos[i].name);
    sb_free(data->group_infos);
    for (size_t i = 0; i < sb_len(data->param_values); ++i)
        free(data->param_values[i]);
    sb_free(data->param_values);
}

static bool get_extract_str_data(const struct parsed_text_file *parsed,
                                 struct extract_str_data *data)
{
    bool success = false;
    bool name_is_first = false;
    char *regex_str = extract_str_to_regex(parsed->extract_str, &name_is_first);
    regex_t regex;
    int ret = regcomp(&regex, regex_str, REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        char errbuf[4096];
        regerror(ret, &regex, errbuf, sizeof(errbuf));
        error("error compiling regex '%s': %s", regex_str, errbuf);
        goto err;
    }

    // This should not happen because we have already validated extract str, but who knowns
    // what happens actually
    if (regex.re_nsub != 2) {
        error("regex '%s' contains %zu subexpressions instead of 2", regex_str,
              regex.re_nsub);
        goto err_free_regex;
    }

    memset(data, 0, sizeof(*data));
    for (size_t line_idx = 0; line_idx < parsed->line_count; ++line_idx) {
        const struct parsed_text_data_line *line = parsed->lines + line_idx;
        char name_buf[256], param_buf[256];
        if (!extract_name_and_param(&regex, regex_str, name_is_first, line->name,
                                    parsed->filename, name_buf, param_buf, sizeof(name_buf)))
            goto err_free_regex;
        bool found_group = false;
        for (size_t i = 0; i < sb_len(data->group_infos); ++i) {
            if (strcmp(data->group_infos[i].name, name_buf) == 0) {
                ++data->group_infos[i].count;
                found_group = true;
                break;
            }
        }
        if (!found_group) {
            struct group_info *info = sb_new(data->group_infos);
            info->name = strdup(name_buf);
            info->count = 1;
        }
        bool found_value = false;
        for (size_t i = 0; i < sb_len(data->param_values); ++i) {
            if (strcmp(data->param_values[i], param_buf) == 0) {
                found_value = true;
                break;
            }
        }
        if (!found_value) {
            sb_push(data->param_values, strdup(param_buf));
        }

        struct bench_info *b = sb_new(data->benches);
        b->name = strdup(name_buf);
        b->value = strdup(param_buf);
    }

    size_t val_count = sb_len(data->param_values);
    for (size_t i = 0; i < sb_len(data->group_infos); ++i) {
        if (data->group_infos[i].count != val_count) {
            error("group '%s' benchmark count does not match value count (%zu) in file '%s'",
                  data->group_infos[i].name, val_count, parsed->filename);
            goto err_free_regex;
        }
    }

    success = true;
err_free_regex:
    regfree(&regex);
err:
    sb_free(regex_str);
    return success;
}

static bool init_extract_str_use(const struct parsed_text_file *parsed,
                                 struct bench_data *data, struct bench_data_storage *storage)
{
    char param_name_buf[256];
    bool has_param;
    if (!validate_extract_str(parsed->extract_str, parsed->filename, &has_param,
                              param_name_buf, sizeof(param_name_buf)))
        return false;

    if (!has_param)
        return true;

    struct extract_str_data ex_data;
    if (!get_extract_str_data(parsed, &ex_data))
        return false;

    size_t val_count = sb_len(ex_data.param_values);
    storage->has_param = true;
    storage->param.name = csstrdup(param_name_buf);
    storage->param.value_count = val_count;
    sb_resize(storage->param.values, val_count);
    for (size_t i = 0; i < val_count; ++i)
        storage->param.values[i] = csstrdup(ex_data.param_values[i]);
    data->param = &storage->param;
    data->group_count = sb_len(ex_data.group_infos);
    sb_resize(data->groups, data->group_count);
    for (size_t i = 0; i < data->group_count; ++i) {
        struct bench_group *grp = data->groups + i;
        grp->name = csstrdup(ex_data.group_infos[i].name);
        grp->bench_count = val_count;
        grp->bench_idxs = calloc(val_count, sizeof(*grp->bench_idxs));
    }

    for (size_t line_idx = 0; line_idx < parsed->line_count; ++line_idx) {
        const char *name = ex_data.benches[line_idx].name;
        const char *value = ex_data.benches[line_idx].value;

        size_t grp_idx = 0;
        for (size_t i = 0; i < data->group_count; ++i, ++grp_idx) {
            if (strcmp(data->groups[i].name, name) == 0)
                break;
        }

        size_t val_idx = 0;
        for (size_t i = 0; i < val_count; ++i, ++val_idx) {
            if (strcmp(ex_data.param_values[i], value) == 0)
                break;
        }

        data->groups[grp_idx].bench_idxs[val_idx] = line_idx;
    }

    free_extract_str_data(&ex_data);
    return true;
}

static bool convert_parsed_text_file(const struct parsed_text_file *parsed,
                                     struct bench_data *data,
                                     struct bench_data_storage *storage)
{
    memset(data, 0, sizeof(*data));
    memset(storage, 0, sizeof(*storage));

    storage->has_param = false;
    storage->meas_count = 1;
    sb_resize(storage->meas, 1);
    init_parsed_text_meas(parsed, storage->meas);

    data->meas_count = storage->meas_count;
    data->meas = storage->meas;
    data->group_count = 0;
    data->bench_count = parsed->line_count;
    sb_resize(data->benches, parsed->line_count);

    if (parsed->extract_str && !init_extract_str_use(parsed, data, storage))
        return false;

    for (size_t bench_idx = 0; bench_idx < data->bench_count; ++bench_idx) {
        const struct parsed_text_data_line *line = parsed->lines + bench_idx;
        struct bench *bench = data->benches + bench_idx;
        bench->name = csstrdup(line->name);
        bench->run_count = line->value_count;
        bench->meas_count = storage->meas_count;
        bench->meas = calloc(bench->meas_count, sizeof(*bench->meas));
        for (size_t i = 0; i < bench->run_count; ++i) {
            sb_push(bench->meas[0], line->values[i]);
            sb_push(bench->exit_codes, 0);
        }
    }
    return true;
}

static bool load_bench_data_text_file(const char *filename, struct bench_data *data,
                                      struct bench_data_storage *storage)
{
    struct parsed_text_file parsed;
    if (!load_parsed_text_file(filename, &parsed))
        return false;
    if (!convert_parsed_text_file(&parsed, data, storage))
        return false;
    free_parsed_text_file(&parsed);
    return true;
}

static bool load_bench_data_text_merge(const char **file_list, struct bench_data *data,
                                       struct bench_data_storage *storage)
{
    bool success = false;
    size_t src_count = sb_len(file_list);
    struct bench_data *src_datas = calloc(src_count, sizeof(*src_datas));
    struct bench_data_storage *src_storages = calloc(src_count, sizeof(*src_storages));
    for (size_t i = 0; i < src_count; ++i) {
        if (!load_bench_data_text_file(file_list[i], src_datas + i, src_storages + i))
            goto err;
    }

    if (!merge_bench_data(src_datas, src_storages, src_count, data, storage))
        goto err;

    success = true;
err:
    for (size_t i = 0; i < src_count; ++i) {
        free_bench_data_storage(src_storages + i);
        free_bench_data(src_datas + i);
    }
    free(src_datas);
    free(src_storages);
    return success;
}

bool load_bench_data_text(const char **file_list, struct bench_data *data,
                          struct bench_data_storage *storage)
{
    bool success = false;
    size_t src_count = sb_len(file_list);
    assert(src_count > 0);
    if (src_count == 1)
        success = load_bench_data_text_file(file_list[0], data, storage);
    else
        success = load_bench_data_text_merge(file_list, data, storage);
    return success;
}
