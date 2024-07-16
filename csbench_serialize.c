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

static bool load_bench_run_meas_from_csv_line(const char *str, double **meas,
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

static bool load_bench_result_from_csv(const char *file, struct bench *bench) {
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
        if (!load_bench_run_meas_from_csv_line(line_buffer, bench->meas,
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

bool load_bench_data_from_csv(const char **files, struct bench_data *data) {
    for (size_t i = 0; i < data->bench_count; ++i) {
        struct bench *bench = data->benches + i;
        const char *file = files[i];
        if (!load_bench_result_from_csv(file, bench))
            return false;
    }
    return true;
}

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
    uint64_t bench_param_offset;
    uint64_t bench_param_size;
    uint64_t groups_offset;
    uint64_t groups_size;
    uint64_t bench_data_offset;
    uint64_t bench_data_size;
};

#define CSBENCH_MAGIC (uint32_t)('C' | ('S' << 8) | ('B' << 16) | ('H' << 24))

static void write_u32(uint32_t value, FILE *f) {
    fwrite(&value, sizeof(value), 1, f);
}

static void write_u64(uint64_t value, FILE *f) {
    fwrite(&value, sizeof(value), 1, f);
}

static void write_string(const char *str, FILE *f) {
    if (str == NULL) {
        uint32_t len = 0;
        fwrite(&len, sizeof(len), 1, f);
    } else {
        uint32_t len = strlen(str) + 1;
        fwrite(&len, sizeof(len), 1, f);
        fwrite(str, len, 1, f);
    }
}

void save_bench_data_binary(const struct bench_data *data, FILE *f) {
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
        uint64_t var_start = (uint64_t)ftell(f);
        write_string(data->var->name, f);
        write_u64(data->var->value_count, f);
        for (size_t i = 0; i < data->var->value_count; ++i)
            write_string(data->var->values[i], f);

        uint64_t at = ftell(f);
        header.var_size = at - header.var_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    {
        fseek(f, cursor, SEEK_SET);
        header.meas_offset = cursor;
        for (size_t i = 0; i < data->meas_count; ++i) {
            const struct meas *meas = data->meas + i;
            write_string(meas->name, f);
            write_string(meas->cmd, f);
            write_u64(meas->units.kind, f);
            write_string(meas->units.str, f);
            write_u64(meas->kind, f);
            write_u64(meas->is_secondary, f);
            write_u64(meas->primary_idx, f);
        }

        uint64_t at = ftell(f);
        header.meas_size = at - header.meas_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    {
        fseek(f, cursor, SEEK_SET);
        header.bench_param_offset = cursor;
        for (size_t i = 0; i < data->bench_count; ++i) {
            const struct bench_params *param = data->benches[i].params;
            write_string(param->name, f);
            write_string(param->str, f);
            write_string(param->exec, f);
            const char **cursor = param->argv;
            do {
                write_string(*cursor, f);
            } while (*cursor != NULL);
            write_u64(param->output, f);
        }

        uint64_t at = ftell(f);
        header.bench_param_size = at - header.bench_param_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    {
        fseek(f, cursor, SEEK_SET);
        header.groups_offset = cursor;
        for (size_t i = 0; i < data->group_count; ++i) {
            const struct bench_var_group *grp = data->groups + i;
            write_string(grp->name, f);
            assert(grp->cmd_count == data->var->value_count);
            write_u64(grp->cmd_count, f);
            for (size_t j = 0; j < grp->cmd_count; ++j)
                write_u64(grp->cmd_idxs[j], f);
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
            write_u64(bench->run_count, f);
            fwrite(bench->exit_codes, sizeof(int), bench->run_count, f);
            for (size_t j = 0; j < data->meas_count; ++j)
                fwrite(bench->meas[j], sizeof(double), bench->run_count, f);
        }

        uint64_t at = ftell(f);
        header.bench_data_size = at - header.bench_data_offset;
        cursor = (ftell(f) + 0x7) & ~0x7;
    }

    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
}
