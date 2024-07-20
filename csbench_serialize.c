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

#define CSBENCH_MAGIC (uint32_t)('C' | ('S' << 8) | ('B' << 16) | ('H' << 24))

static bool load_bench_run_meas_csv_line(const char *str, double **meas,
                                         size_t meas_count) {
    size_t cursor = 0;
    while (cursor < meas_count) {
        char *end = NULL;
        double value = strtod(str, &end);
        if (end == str)
            return false;
        sb_push(meas[cursor], value);
        ++cursor;
        str = end;
        if (*str == '\n' || !*str)
            break;
        if (*str != ',')
            return false;
        ++str;
    }
    return cursor == meas_count ? true : false;
}

static bool load_bench_result_csv(const char *file, struct bench *bench) {
    bool success = false;
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return false;

    size_t n = 0;
    char *line_buffer = NULL;
    // Skip line with measurement names
    if (getline(&line_buffer, &n, f) < 0) {
        error("failed to parse file '%s'", file);
        goto out;
    }
    for (;;) {
        ssize_t read_result = getline(&line_buffer, &n, f);
        if (read_result < 0) {
            if (ferror(f)) {
                error("failed to read line from file '%s'", file);
                goto out;
            }
            break;
        }
        ++bench->run_count;
        if (!load_bench_run_meas_csv_line(line_buffer, bench->meas,
                                          bench->meas_count)) {
            error("failed to parse file '%s'", file);
            goto out;
        }
    }

    success = true;
out:
    fclose(f);
    free(line_buffer);
    return success;
}

bool load_bench_data_csv(const char **files, struct bench_data *data) {
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        const char *file = files[i];
        if (!load_bench_result_csv(file, bench))
            return false;
    }
    return true;
}

#define write_raw__(_arr, _elemsz, _cnt, _f)                                   \
    do {                                                                       \
        size_t CSUNIQIFY(cnt) = (_cnt);                                        \
        if (fwrite(_arr, _elemsz, CSUNIQIFY(cnt), _f) != CSUNIQIFY(cnt))       \
            goto err;                                                          \
    } while (0)

#define write_u64__(_value, _f)                                                \
    do {                                                                       \
        uint64_t CSUNIQIFY(value) = (_value);                                  \
        write_raw__(&CSUNIQIFY(value), sizeof(uint64_t), 1, _f);               \
    } while (0)

#define write_str__(_str, _f)                                                  \
    do {                                                                       \
        const char *CSUNIQIFY(str) = (_str);                                   \
        if (CSUNIQIFY(str) == NULL) {                                          \
            uint32_t CSUNIQIFY(len) = 0;                                       \
            write_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);              \
        } else {                                                               \
            uint32_t CSUNIQIFY(len) = strlen(CSUNIQIFY(str)) + 1;              \
            write_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);              \
            write_raw__(CSUNIQIFY(str), 1, CSUNIQIFY(len), f);                 \
        }                                                                      \
    } while (0)

bool save_bench_data_binary(const struct bench_data *data, FILE *f) {
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
        fseek(f, cursor, SEEK_SET);
        write_str__(data->var->name, f);
        write_u64__(data->var->value_count, f);
        for (size_t i = 0; i < data->var->value_count; ++i)
            write_str__(data->var->values[i], f);

        uint64_t at = ftell(f);
        header.var_size = at - header.var_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    {
        fseek(f, cursor, SEEK_SET);
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

        uint64_t at = ftell(f);
        header.meas_size = at - header.meas_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    if (data->group_count != 0) {
        fseek(f, cursor, SEEK_SET);
        header.groups_offset = cursor;
        for (size_t i = 0; i < data->group_count; ++i) {
            const struct bench_var_group *grp = data->groups + i;
            write_str__(grp->name, f);
            assert(data->var && grp->cmd_count == data->var->value_count);
            write_u64__(grp->cmd_count, f);
            for (size_t j = 0; j < grp->cmd_count; ++j)
                write_u64__(grp->cmd_idxs[j], f);
        }

        uint64_t at = ftell(f);
        header.groups_size = at - header.groups_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    {
        fseek(f, cursor, SEEK_SET);
        header.bench_data_offset = cursor;

        for (size_t i = 0; i < data->bench_count; ++i) {
            const struct bench *bench = data->benches + i;
            write_str__(bench->name, f);
            write_u64__(bench->run_count, f);
            write_raw__(bench->exit_codes, sizeof(int), bench->run_count, f);
            for (size_t j = 0; j < data->meas_count; ++j)
                write_raw__(bench->meas[j], sizeof(double), bench->run_count,
                            f);
        }

        uint64_t at = ftell(f);
        header.bench_data_size = at - header.bench_data_offset;
    }

    fseek(f, 0, SEEK_SET);
    write_raw__(&header, sizeof(header), 1, f);
    return true;
err:
    csperror("IO error when writing csbench data file");
    return false;
}

#define read_raw__(_dst, _elemsz, _cnt, _f)                                    \
    do {                                                                       \
        size_t CSUNIQIFY(cnt) = (_cnt);                                        \
        if (fread(_dst, _elemsz, CSUNIQIFY(cnt), _f) != CSUNIQIFY(cnt))        \
            goto err;                                                          \
    } while (0)

#define read_u64__(_dst, _f)                                                   \
    do {                                                                       \
        uint64_t CSUNIQIFY(value);                                             \
        read_raw__(&CSUNIQIFY(value), sizeof(uint64_t), 1, f);                 \
        (_dst) = CSUNIQIFY(value);                                             \
    } while (0)

#define read_str__(_dst, _f)                                                   \
    do {                                                                       \
        uint32_t CSUNIQIFY(len);                                               \
        read_raw__(&CSUNIQIFY(len), sizeof(uint32_t), 1, f);                   \
        char *CSUNIQIFY(res) = NULL;                                           \
        if (CSUNIQIFY(len) != 0) {                                             \
            CSUNIQIFY(res) = csstralloc(CSUNIQIFY(len) - 1);                   \
            read_raw__(CSUNIQIFY(res), 1, CSUNIQIFY(len), f);                  \
        }                                                                      \
        (_dst) = (const char *)CSUNIQIFY(res);                                 \
    } while (0)

bool load_bench_data_binary(FILE *f, const char *filename,
                            struct bench_binary_data_storage *storage,
                            struct bench_data *data) {
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
        if ((uint64_t)at != header.var_offset + header.var_size) {
            error("csbench data file '%s' is corrupted", filename);
            goto err_raw;
        }
    }

    if (header.meas_count == 0) {
        error("invalid measurement count in csbench data file '%s'", filename);
        goto err_raw;
    }
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
        if ((uint64_t)at != header.meas_offset + header.meas_size) {
            error("csbench data file '%s' is corrupted", filename);
            goto err_raw;
        }
    }

    if (header.group_count) {
        assert(data->var);
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
            assert(grp->cmd_count == data->var->value_count);
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
        if ((uint64_t)at != header.groups_offset + header.groups_size) {
            error("csbench data file '%s' is corrupted", filename);
            goto err_raw;
        }
    }

    if (header.bench_count == 0) {
        error("invalid benchmark count in csbench data file '%s'", filename);
        return false;
    }
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
        if ((uint64_t)at != header.bench_data_offset + header.bench_data_size) {
            error("csbench data file '%s' is corrupted", filename);
            goto err_raw;
        }
    }

    return true;
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
    free_bench_binary_data_storage(storage);
    return false;
}

void free_bench_binary_data_storage(struct bench_binary_data_storage *storage) {
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
