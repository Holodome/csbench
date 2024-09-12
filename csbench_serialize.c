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

struct csbench_binary_header {
    uint32_t magic;
    uint32_t version;

    uint64_t meas_count;
    uint64_t bench_count;
    uint64_t group_count;

    uint8_t has_var;
    uint8_t reserved0[7];

    uint64_t var_offset;
    uint64_t var_size;
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
    size_t line_count;
    struct parsed_text_data_line *lines;
};

#define CSBENCH_MAGIC (uint32_t)('C' | ('S' << 8) | ('B' << 16) | ('H' << 24))

#define write_raw__(_arr, _elemsz, _cnt, _f)                                                 \
    do {                                                                                     \
        size_t CSUNIQIFY(cnt) = (_cnt);                                                      \
        if (fwrite(_arr, _elemsz, CSUNIQIFY(cnt), _f) != CSUNIQIFY(cnt))                     \
            goto err;                                                                        \
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

    if (data->var != NULL) {
        header.has_var = 1;
        header.var_offset = cursor;
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        write_str__(data->var->name, f);
        write_u64__(data->var->value_count, f);
        for (size_t i = 0; i < data->var->value_count; ++i)
            write_str__(data->var->values[i], f);

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            return false;
        }
        header.var_size = (uint64_t)at - header.var_offset;
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
        assert(data->var);
        if (fseek(f, cursor, SEEK_SET) == -1) {
            csperror("fseek");
            return false;
        }
        header.groups_offset = cursor;
        for (size_t i = 0; i < data->group_count; ++i) {
            const struct bench_var_group *grp = data->groups + i;
            write_str__(grp->name, f);
            assert(grp->cmd_count == data->var->value_count);
            write_u64__(grp->cmd_count, f);
            for (size_t j = 0; j < grp->cmd_count; ++j)
                write_u64__(grp->cmd_idxs[j], f);
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
err:
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
            goto err;                                                                        \
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

    if (header.has_var) {
        if (fseek(f, header.var_offset, SEEK_SET) == -1) {
            csperror("fseek");
            goto err_raw;
        }

        storage->has_var = true;
        read_str__(storage->var.name, f);
        read_u64__(storage->var.value_count, f);
        sb_resize(storage->var.values, storage->var.value_count);
        for (size_t i = 0; i < storage->var.value_count; ++i)
            read_str__(storage->var.values[i], f);
        data->var = &storage->var;

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            goto err_raw;
        }
        if ((uint64_t)at != header.var_offset + header.var_size)
            goto corrupted;
    }

    if (header.meas_count == 0)
        goto corrupted;
    {
        if (fseek(f, header.meas_offset, SEEK_SET) == -1) {
            csperror("fseek");
            goto err_raw;
        }

        storage->meas_count = header.meas_count;
        storage->meas = calloc(header.meas_count, sizeof(*storage->meas));
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
        if (at == -1) {
            csperror("ftell");
            goto err_raw;
        }
        if ((uint64_t)at != header.meas_offset + header.meas_size)
            goto corrupted;
    }

    if (header.group_count) {
        if (!data->var)
            goto corrupted;
        if (fseek(f, header.groups_offset, SEEK_SET) == -1) {
            csperror("fseek");
            goto err_raw;
        }

        storage->group_count = header.group_count;
        storage->groups = calloc(header.group_count, sizeof(*storage->groups));
        for (size_t i = 0; i < header.group_count; ++i) {
            struct bench_var_group *grp = storage->groups + i;
            read_str__(grp->name, f);
            read_u64__(grp->cmd_count, f);
            if (grp->cmd_count != data->var->value_count)
                goto corrupted;
            grp->cmd_idxs = calloc(grp->cmd_count, sizeof(*grp->cmd_idxs));
            for (size_t j = 0; j < grp->cmd_count; ++j)
                read_u64__(grp->cmd_idxs[j], f);
        }

        data->group_count = storage->group_count;
        data->groups = storage->groups;

        int at = ftell(f);
        if (at == -1) {
            csperror("ftell");
            goto err_raw;
        }
        if ((uint64_t)at != header.groups_offset + header.groups_size)
            goto corrupted;
    }

    if (header.bench_count == 0)
        goto corrupted;
    {
        if (fseek(f, header.bench_data_offset, SEEK_SET) == -1) {
            csperror("fseek");
            goto err_raw;
        }

        data->bench_count = header.bench_count;
        data->benches = calloc(header.bench_count, sizeof(*data->benches));
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
        if (at == -1) {
            csperror("ftell");
            goto err_raw;
        }
        if ((uint64_t)at != header.bench_data_offset + header.bench_data_size)
            goto corrupted;
    }

    return true;
corrupted:
    error("csbench data file '%s' is corrupted", filename);
    goto err_raw;
err:
    csperror("IO error reading csbench binary file");
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
    if (storage->has_var) {
        sb_free(storage->var.values);
    }
    if (storage->meas) {
        free(storage->meas);
    }
    if (storage->groups) {
        for (size_t i = 0; i < storage->group_count; ++i)
            free(storage->groups[i].cmd_idxs);
        free(storage->groups);
    }
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

static bool vars_match(const struct bench_var *a, const struct bench_var *b)
{
    if (strcmp(a->name, b->name) != 0)
        return false;
    if (a->value_count != b->value_count)
        return false;
    for (size_t i = 0; i < a->value_count; ++i)
        if (strcmp(a->values[i], b->values[i]) != 0)
            return false;
    return true;
}

static bool bench_data_match(const struct bench_data *a, const struct bench_data *b)
{
    if (a->meas_count != b->meas_count)
        return false;
    for (size_t j = 0; j < a->meas_count; ++j)
        if (!meas_match(a->meas + j, b->meas + j))
            return false;
    if (((a->var != NULL) != (b->var != NULL)))
        return false;
    if (a->var != NULL && !vars_match(a->var, b->var))
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

    // Move measurements and variable
    storage->has_var = src_storages[0].has_var;
    if (storage->has_var)
        data->var = &storage->var;
    memcpy(&storage->var, &src_storages[0].var, sizeof(storage->var));
    data->meas_count = storage->meas_count = src_storages[0].meas_count;
    data->meas = storage->meas = src_storages[0].meas;
    src_storages[0].has_var = false;
    src_storages[0].meas_count = 0;
    src_storages[0].meas = NULL;
    // Make new benchmark list
    data->bench_count = total_bench_count;
    data->benches = calloc(total_bench_count, sizeof(*data->benches));
    for (size_t i = 0, bench_cursor = 0; i < src_count; ++i) {
        struct bench_data *src = src_datas + i;
        memcpy(data->benches + bench_cursor, src->benches,
               sizeof(struct bench) * src->bench_count);
        bench_cursor += src->bench_count;
        // Free data in source as if it never had it
        free(src->benches);
        src->benches = NULL;
        src->bench_count = 0;
    }
    // Make new group list
    if (data->var) {
        data->group_count = storage->group_count = total_group_count;
        data->groups = storage->groups = calloc(total_group_count, sizeof(*storage->groups));
        for (size_t i = 0, group_cursor = 0, bench_cursor = 0; i < src_count; ++i) {
            struct bench_data_storage *src = src_storages + i;
            memcpy(storage->groups + group_cursor, src->groups,
                   sizeof(struct bench_var_group) * src->group_count);
            // Fixup command indexes
            for (size_t j = 0; j < src->group_count; ++j) {
                assert(src->groups[j].cmd_count == data->var->value_count);
                for (size_t k = 0; k < src->groups[j].cmd_count; ++k)
                    storage->groups[group_cursor + j].cmd_idxs[k] += bench_cursor;
                bench_cursor += src->groups[j].cmd_count;
            }
            group_cursor += src->group_count;
            // Free data in source as if it never had it
            free(src->groups);
            src->groups = NULL;
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
    for (size_t i = 0; i < src_count; ++i)
        if (!load_bench_data_binary_file(file_list[i], src_datas + i, src_storages + i))
            goto err;

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

static bool keyword_val(const char *str, const char *kw, const char **val)
{
    size_t kw_len = strlen(kw);
    bool matches = strncmp(kw, str, kw_len) == 0 ? true : false;
    if (matches && (matches = str[kw_len] == '=')) {
        *val = str + kw_len + 1;
    }
    return matches;
}

static bool handle_text_header_tok(const char *tok, struct parsed_text_file *file)
{
    const char *val = NULL;
    if (keyword_val(tok, "meas", &val)) {
        if (strlen(val) == 0) {
            error("empty meas= value found in file '%s'", file->filename);
            return false;
        }
        file->meas_name = strdup(val);
    } else if (keyword_val(tok, "units", &val)) {
        if (strlen(val) == 0) {
            error("empty units= value found in file '%s'", file->filename);
            return false;
        }
        file->meas_units = strdup(val);
    } else {
        error("invalid header keyword '%s' found in file '%s'", tok, file->filename);
        return false;
    }
    return true;
}

static bool parse_text_header(const char *line, struct parsed_text_file *file)
{
    assert(line[0] == '#');
    const char *delimiters = " \t";
    const char *cursor = line + 1;
    for (;;) {
        while (isspace(*cursor))
            ++cursor;
        if (*cursor == '\0')
            break;
        const char *sep = strpbrk(cursor, delimiters);
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
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        csfmtperror("failed to open file '%s' for reading", filename);
        return false;
    }
    memset(file, 0, sizeof(*file));
    file->filename = filename;
    bool success = load_parsed_text_file_internal(f, file);
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

static bool convert_parsed_text_file(const struct parsed_text_file *parsed,
                                     struct bench_data *data,
                                     struct bench_data_storage *storage)
{
    memset(data, 0, sizeof(*data));
    memset(storage, 0, sizeof(*storage));

    // TODO: add support for variables
    storage->has_var = false;
    storage->meas_count = 1;
    storage->meas = calloc(1, sizeof(*storage->meas));
    init_parsed_text_meas(parsed, storage->meas);
    // TODO: add support for groups
    storage->group_count = 0;

    data->meas_count = storage->meas_count;
    data->meas = storage->meas;
    data->bench_count = parsed->line_count;
    data->benches = calloc(parsed->line_count, sizeof(*data->benches));
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

bool load_bench_data_text(const char **file_list, struct bench_data *data,
                          struct bench_data_storage *storage)
{
    if (sb_len(file_list) != 1) {
        error("only one text file is supported");
        return false;
    }
    struct parsed_text_file parsed;
    if (!load_parsed_text_file(file_list[0], &parsed))
        return false;

    convert_parsed_text_file(&parsed, data, storage);
    free_parsed_text_file(&parsed);

    return true;
}
