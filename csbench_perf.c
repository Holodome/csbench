// csbench performance counters. See csbench.c for main application code.
// Ilya Vinogradov 2024
// https://github.com/Holodome/csbench
//
// This is header-only library which makes cross-platform interface to
// performance counters.
//
// This file contains definitions for ARM MacOS and linux performance counters.
// Linux performance countes are documented at
// https://perf.wiki.kernel.org/index.php/Main_Page. MacOS performance countes
// were originally reverse-engineered by ibireme
// https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12 and
// popularized by Daniel Lemire
// https://lemire.me/blog/2023/03/21/counting-cycles-and-instructions-on-arm-based-apple-systems/.
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
#ifndef CSBENCH_PERF_H
#define CSBENCH_PERF_H

#include <stdint.h>
#include <sys/types.h>

struct perf_cnt {
    uint64_t cycles;
    uint64_t branches;
    uint64_t missed_branches;
    uint64_t instructions;
};

int init_perf(void);
void deinit_perf(void);

int perf_cnt_collect(pid_t pid, int sync_pipe, struct perf_cnt *cnt);

#endif

//
#define CSBENCH_PERF_IMPL
//

#ifdef CSBENCH_PERF_IMPL
#ifdef __linux__
#error not supported
#elif defined(__APPLE__)
#ifdef __aarch64__

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/kdebug.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define KPC_CLASS_FIXED (0)
#define KPC_CLASS_CONFIGURABLE (1)
#define KPC_CLASS_POWER (2)
#define KPC_CLASS_RAWPMU (3)

#define KPC_CLASS_FIXED_MASK (1u << KPC_CLASS_FIXED)
#define KPC_CLASS_CONFIGURABLE_MASK (1u << KPC_CLASS_CONFIGURABLE)
#define KPC_CLASS_POWER_MASK (1u << KPC_CLASS_POWER)
#define KPC_CLASS_RAWPMU_MASK (1u << KPC_CLASS_RAWPMU)

#define KPC_PMU_ERROR (0)
#define KPC_PMU_INTEL_V3 (1)
#define KPC_PMU_ARM_APPLE (2)
#define KPC_PMU_INTEL_V2 (3)
#define KPC_PMU_ARM_V2 (4)

#define KPC_MAX_COUNTERS 32

#define KPERF_SAMPLER_TH_INFO (1U << 0)
#define KPERF_SAMPLER_TH_SNAPSHOT (1U << 1)
#define KPERF_SAMPLER_KSTACK (1U << 2)
#define KPERF_SAMPLER_USTACK (1U << 3)
#define KPERF_SAMPLER_PMC_THREAD (1U << 4)
#define KPERF_SAMPLER_PMC_CPU (1U << 5)
#define KPERF_SAMPLER_PMC_CONFIG (1U << 6)
#define KPERF_SAMPLER_MEMINFO (1U << 7)
#define KPERF_SAMPLER_TH_SCHEDULING (1U << 8)
#define KPERF_SAMPLER_TH_DISPATCH (1U << 9)
#define KPERF_SAMPLER_TK_SNAPSHOT (1U << 10)
#define KPERF_SAMPLER_SYS_MEM (1U << 11)
#define KPERF_SAMPLER_TH_INSCYC (1U << 12)
#define KPERF_SAMPLER_TK_INFO (1U << 13)

#define KPERF_ACTION_MAX (32)

#define KPERF_TIMER_MAX (8)

typedef uint64_t kpc_config_t;

static int (*kpc_cpu_string)(char *buf, size_t buf_size);
static uint32_t (*kpc_pmu_version)(void);
static uint32_t (*kpc_get_counting)(void);
static int (*kpc_set_counting)(uint32_t classes);
static uint32_t (*kpc_get_thread_counting)(void);
static int (*kpc_set_thread_counting)(uint32_t classes);
static uint32_t (*kpc_get_config_count)(uint32_t classes);
static int (*kpc_get_config)(uint32_t classes, kpc_config_t *config);
static int (*kpc_set_config)(uint32_t classes, kpc_config_t *config);
static uint32_t (*kpc_get_counter_count)(uint32_t classes);
static int (*kpc_get_cpu_counters)(int all_cpus, uint32_t classes, int *curcpu,
                                   uint64_t *buf);
static int (*kpc_get_thread_counters)(uint32_t tid, uint32_t buf_count,
                                      uint64_t *buf);
static int (*kpc_force_all_ctrs_set)(int val);
static int (*kpc_force_all_ctrs_get)(int *val_out);
static int (*kperf_action_count_set)(uint32_t count);
static int (*kperf_action_count_get)(uint32_t *count);
static int (*kperf_action_samplers_set)(uint32_t actionid, uint32_t sample);
static int (*kperf_action_samplers_get)(uint32_t actionid, uint32_t *sample);
static int (*kperf_action_filter_set_by_task)(uint32_t actionid, int32_t port);
static int (*kperf_action_filter_set_by_pid)(uint32_t actionid, int32_t pid);
static int (*kperf_timer_count_set)(uint32_t count);
static int (*kperf_timer_count_get)(uint32_t *count);
static int (*kperf_timer_period_set)(uint32_t actionid, uint64_t tick);
static int (*kperf_timer_period_get)(uint32_t actionid, uint64_t *tick);
static int (*kperf_timer_action_set)(uint32_t actionid, uint32_t timerid);
static int (*kperf_timer_action_get)(uint32_t actionid, uint32_t *timerid);
static int (*kperf_timer_pet_set)(uint32_t timerid);
static int (*kperf_timer_pet_get)(uint32_t *timerid);
static int (*kperf_sample_set)(uint32_t enabled);
static int (*kperf_sample_get)(uint32_t *enabled);
static int (*kperf_reset)(void);
static uint64_t (*kperf_ns_to_ticks)(uint64_t ns);
static uint64_t (*kperf_ticks_to_ns)(uint64_t ticks);
static uint64_t (*kperf_tick_frequency)(void);

__attribute__((used)) static int kperf_lightweight_pet_get(uint32_t *enabled) {
    if (!enabled)
        return -1;
    size_t size = 4;
    return sysctlbyname("kperf.lightweight_pet", enabled, &size, NULL, 0);
}

static int kperf_lightweight_pet_set(uint32_t enabled) {
    return sysctlbyname("kperf.lightweight_pet", NULL, NULL, &enabled, 4);
}

#define KPEP_ARCH_I386 0
#define KPEP_ARCH_X86_64 1
#define KPEP_ARCH_ARM 2
#define KPEP_ARCH_ARM64 3

struct kpep_event {
    const char *name;
    const char *description;
    const char *errata;
    const char *alias;
    const char *fallback;
    uint32_t mask;
    uint8_t number;
    uint8_t umask;
    uint8_t reserved;
    uint8_t is_fixed;
};

struct kpep_db {
    const char *name;
    const char *cpu_id;
    const char *marketing_name;
    void *plist_data;
    void *event_map;
    struct kpep_event *event_arr;
    struct kpep_event **fixed_event_arr;
    void *alias_map;
    size_t reserved_1;
    size_t reserved_2;
    size_t reserved_3;
    size_t event_count;
    size_t alias_count;
    size_t fixed_counter_count;
    size_t config_counter_count;
    size_t power_counter_count;
    uint32_t archtecture;
    uint32_t fixed_counter_bits;
    uint32_t config_counter_bits;
    uint32_t power_counter_bits;
};

struct kpep_config {
    struct kpep_db *db;
    struct kpep_event **ev_arr;
    size_t *ev_map;
    size_t *ev_idx;
    uint32_t *flags;
    uint64_t *kpc_periods;
    size_t event_count;
    size_t counter_count;
    uint32_t classes;
    uint32_t config_counter;
    uint32_t power_counter;
    uint32_t reserved;
};

enum kpep_config_error_code {
    KPEP_CONFIG_ERROR_NONE = 0,
    KPEP_CONFIG_ERROR_INVALID_ARGUMENT = 1,
    KPEP_CONFIG_ERROR_OUT_OF_MEMORY = 2,
    KPEP_CONFIG_ERROR_IO = 3,
    KPEP_CONFIG_ERROR_BUFFER_TOO_SMALL = 4,
    KPEP_CONFIG_ERROR_CUR_SYSTEM_UNKNOWN = 5,
    KPEP_CONFIG_ERROR_DB_PATH_INVALID = 6,
    KPEP_CONFIG_ERROR_DB_NOT_FOUND = 7,
    KPEP_CONFIG_ERROR_DB_ARCH_UNSUPPORTED = 8,
    KPEP_CONFIG_ERROR_DB_VERSION_UNSUPPORTED = 9,
    KPEP_CONFIG_ERROR_DB_CORRUPT = 10,
    KPEP_CONFIG_ERROR_EVENT_NOT_FOUND = 11,
    KPEP_CONFIG_ERROR_CONFLICTING_EVENTS = 12,
    KPEP_CONFIG_ERROR_COUNTERS_NOT_FORCED = 13,
    KPEP_CONFIG_ERROR_EVENT_UNAVAILABLE = 14,
    KPEP_CONFIG_ERROR_ERRNO = 15,
    KPEP_CONFIG_ERROR_MAX
};

static const char *kpep_config_error_names[KPEP_CONFIG_ERROR_MAX] = {
    "none",
    "invalid argument",
    "out of memory",
    "I/O",
    "buffer too small",
    "current system unknown",
    "database path invalid",
    "database not found",
    "database architecture unsupported",
    "database version unsupported",
    "database corrupt",
    "event not found",
    "conflicting events",
    "all counters must be forced",
    "event unavailable",
    "check errno"};

static const char *kpep_config_error_desc(int code) {
    if (0 <= code && code < KPEP_CONFIG_ERROR_MAX) {
        return kpep_config_error_names[code];
    }
    return "unknown error";
}

static int (*kpep_config_create)(struct kpep_db *db,
                                 struct kpep_config **cfg_ptr);
static void (*kpep_config_free)(struct kpep_config *cfg);
static int (*kpep_config_add_event)(struct kpep_config *cfg,
                                    struct kpep_event **ev_ptr, uint32_t flag,
                                    uint32_t *err);
static int (*kpep_config_remove_event)(struct kpep_config *cfg, size_t idx);
static int (*kpep_config_force_counters)(struct kpep_config *cfg);
static int (*kpep_config_events_count)(struct kpep_config *cfg,
                                       size_t *count_ptr);
static int (*kpep_config_events)(struct kpep_config *cfg,
                                 struct kpep_event **buf, size_t buf_size);
static int (*kpep_config_kpc)(struct kpep_config *cfg, kpc_config_t *buf,
                              size_t buf_size);
static int (*kpep_config_kpc_count)(struct kpep_config *cfg, size_t *count_ptr);
static int (*kpep_config_kpc_classes)(struct kpep_config *cfg,
                                      uint32_t *classes_ptr);
static int (*kpep_config_kpc_map)(struct kpep_config *cfg, size_t *buf,
                                  size_t buf_size);
static int (*kpep_db_create)(const char *name, struct kpep_db **db_ptr);
static void (*kpep_db_free)(struct kpep_db *db);
static int (*kpep_db_name)(struct kpep_db *db, const char **name);
static int (*kpep_db_aliases_count)(struct kpep_db *db, size_t *count);
static int (*kpep_db_aliases)(struct kpep_db *db, const char **buf,
                              size_t buf_size);
static int (*kpep_db_counters_count)(struct kpep_db *db, uint8_t classes,
                                     size_t *count);
static int (*kpep_db_events_count)(struct kpep_db *db, size_t *count);
static int (*kpep_db_events)(struct kpep_db *db, struct kpep_event **buf,
                             size_t buf_size);
static int (*kpep_db_event)(struct kpep_db *db, const char *name,
                            struct kpep_event **ev_ptr);
static int (*kpep_event_name)(struct kpep_event *ev, const char **name_ptr);
static int (*kpep_event_alias)(struct kpep_event *ev, const char **alias_ptr);
static int (*kpep_event_description)(struct kpep_event *ev,
                                     const char **str_ptr);

struct perf_lib_symbol {
    const char *name;
    void **impl;
};

#define perf_lib_nelems(x) (sizeof(x) / sizeof((x)[0]))
#define perf_lib_symbol_def(name)                                              \
    { #name, (void **)&name }

static const struct perf_lib_symbol perf_lib_symbols_kperf[] = {
    perf_lib_symbol_def(kpc_pmu_version),
    perf_lib_symbol_def(kpc_cpu_string),
    perf_lib_symbol_def(kpc_set_counting),
    perf_lib_symbol_def(kpc_get_counting),
    perf_lib_symbol_def(kpc_set_thread_counting),
    perf_lib_symbol_def(kpc_get_thread_counting),
    perf_lib_symbol_def(kpc_get_config_count),
    perf_lib_symbol_def(kpc_get_counter_count),
    perf_lib_symbol_def(kpc_set_config),
    perf_lib_symbol_def(kpc_get_config),
    perf_lib_symbol_def(kpc_get_cpu_counters),
    perf_lib_symbol_def(kpc_get_thread_counters),
    perf_lib_symbol_def(kpc_force_all_ctrs_set),
    perf_lib_symbol_def(kpc_force_all_ctrs_get),
    perf_lib_symbol_def(kperf_action_count_set),
    perf_lib_symbol_def(kperf_action_count_get),
    perf_lib_symbol_def(kperf_action_samplers_set),
    perf_lib_symbol_def(kperf_action_samplers_get),
    perf_lib_symbol_def(kperf_action_filter_set_by_task),
    perf_lib_symbol_def(kperf_action_filter_set_by_pid),
    perf_lib_symbol_def(kperf_timer_count_set),
    perf_lib_symbol_def(kperf_timer_count_get),
    perf_lib_symbol_def(kperf_timer_period_set),
    perf_lib_symbol_def(kperf_timer_period_get),
    perf_lib_symbol_def(kperf_timer_action_set),
    perf_lib_symbol_def(kperf_timer_action_get),
    perf_lib_symbol_def(kperf_sample_set),
    perf_lib_symbol_def(kperf_sample_get),
    perf_lib_symbol_def(kperf_reset),
    perf_lib_symbol_def(kperf_timer_pet_set),
    perf_lib_symbol_def(kperf_timer_pet_get),
    perf_lib_symbol_def(kperf_ns_to_ticks),
    perf_lib_symbol_def(kperf_ticks_to_ns),
    perf_lib_symbol_def(kperf_tick_frequency),
};

static const struct perf_lib_symbol perf_lib_symbols_kperfdata[] = {
    perf_lib_symbol_def(kpep_config_create),
    perf_lib_symbol_def(kpep_config_free),
    perf_lib_symbol_def(kpep_config_add_event),
    perf_lib_symbol_def(kpep_config_remove_event),
    perf_lib_symbol_def(kpep_config_force_counters),
    perf_lib_symbol_def(kpep_config_events_count),
    perf_lib_symbol_def(kpep_config_events),
    perf_lib_symbol_def(kpep_config_kpc),
    perf_lib_symbol_def(kpep_config_kpc_count),
    perf_lib_symbol_def(kpep_config_kpc_classes),
    perf_lib_symbol_def(kpep_config_kpc_map),
    perf_lib_symbol_def(kpep_db_create),
    perf_lib_symbol_def(kpep_db_free),
    perf_lib_symbol_def(kpep_db_name),
    perf_lib_symbol_def(kpep_db_aliases_count),
    perf_lib_symbol_def(kpep_db_aliases),
    perf_lib_symbol_def(kpep_db_counters_count),
    perf_lib_symbol_def(kpep_db_events_count),
    perf_lib_symbol_def(kpep_db_events),
    perf_lib_symbol_def(kpep_db_event),
    perf_lib_symbol_def(kpep_event_name),
    perf_lib_symbol_def(kpep_event_alias),
    perf_lib_symbol_def(kpep_event_description),
};

#define perf_lib_path_kperf                                                    \
    "/System/Library/PrivateFrameworks/kperf.framework/kperf"
#define perf_lib_path_kperfdata                                                \
    "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata"

static void *perf_lib_handle_kperf = NULL;
static void *perf_lib_handle_kperfdata = NULL;

static void perf_lib_deinit(void) {
    if (perf_lib_handle_kperf)
        dlclose(perf_lib_handle_kperf);
    if (perf_lib_handle_kperfdata)
        dlclose(perf_lib_handle_kperfdata);
    perf_lib_handle_kperf = NULL;
    perf_lib_handle_kperfdata = NULL;
    for (size_t i = 0; i < perf_lib_nelems(perf_lib_symbols_kperf); i++) {
        const struct perf_lib_symbol *symbol = &perf_lib_symbols_kperf[i];
        *symbol->impl = NULL;
    }
    for (size_t i = 0; i < perf_lib_nelems(perf_lib_symbols_kperfdata); i++) {
        const struct perf_lib_symbol *symbol = &perf_lib_symbols_kperfdata[i];
        *symbol->impl = NULL;
    }
}

static int perf_lib_init(void) {
    perf_lib_handle_kperf = dlopen(perf_lib_path_kperf, RTLD_LAZY);
    if (!perf_lib_handle_kperf) {
        fprintf(stderr, "error: failed to load kperf.framework, message: %s.",
                dlerror());
        goto err;
    }
    perf_lib_handle_kperfdata = dlopen(perf_lib_path_kperfdata, RTLD_LAZY);
    if (!perf_lib_handle_kperfdata) {
        fprintf(stderr,
                "error: failed to load kperfdata.framework, message: %s.",
                dlerror());
        goto err;
    }

    // load symbol address from dynamic library
    for (size_t i = 0; i < perf_lib_nelems(perf_lib_symbols_kperf); i++) {
        const struct perf_lib_symbol *symbol = &perf_lib_symbols_kperf[i];
        *symbol->impl = dlsym(perf_lib_handle_kperf, symbol->name);
        if (!*symbol->impl) {
            fprintf(stderr, "error: failed to load kperf function: %s.",
                    symbol->name);
            goto err;
        }
    }
    for (size_t i = 0; i < perf_lib_nelems(perf_lib_symbols_kperfdata); i++) {
        const struct perf_lib_symbol *symbol = &perf_lib_symbols_kperfdata[i];
        *symbol->impl = dlsym(perf_lib_handle_kperfdata, symbol->name);
        if (!*symbol->impl) {
            fprintf(stderr, "error: failed to load kperfdata function: %s.",
                    symbol->name);
            goto err;
        }
    }

    return 0;
err:
    perf_lib_deinit();
    return -1;
}

#if defined(__arm64__)
typedef uint64_t kd_buf_argtype;
#else
typedef uintptr_t kd_buf_argtype;
#endif

struct kd_buf {
    uint64_t timestamp;
    kd_buf_argtype arg1;
    kd_buf_argtype arg2;
    kd_buf_argtype arg3;
    kd_buf_argtype arg4;
    kd_buf_argtype arg5;
    uint32_t debugid;

#if defined(__LP64__) || defined(__arm64__)
    uint32_t cpuid;
    kd_buf_argtype unused;
#endif
};

#define KDBG_CLASSTYPE 0x10000
#define KDBG_SUBCLSTYPE 0x20000
#define KDBG_RANGETYPE 0x40000
#define KDBG_TYPENONE 0x80000
#define KDBG_CKTYPES 0xF0000

#define KDBG_VALCHECK 0x00200000U

struct kd_regtype {
    unsigned int type;
    unsigned int value1;
    unsigned int value2;
    unsigned int value3;
    unsigned int value4;
};

struct kbufinfo_t {
    int nkdbufs;
    int nolog;
    unsigned int flags;
    int nkdthreads;
    int bufid;
};

static int kdebug_reset(void) {
    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREMOVE};
    return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

static int kdebug_reinit(void) {
    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETUP};
    return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

static int kdebug_setreg(struct kd_regtype *kdr) {
    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETREG};
    size_t size = sizeof(struct kd_regtype);
    return sysctl(mib, 3, kdr, &size, NULL, 0);
}

static int kdebug_trace_setbuf(int nbufs) {
    int mib[4] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETBUF, nbufs};
    return sysctl(mib, 4, NULL, NULL, NULL, 0);
}

static int kdebug_trace_enable(int enable) {
    int mib[4] = {CTL_KERN, KERN_KDEBUG, KERN_KDENABLE, enable};
    return sysctl(mib, 4, NULL, 0, NULL, 0);
}

__attribute__((used)) static int kdebug_get_bufinfo(struct kbufinfo_t *info) {
    if (!info)
        return -1;
    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDGETBUF};
    size_t needed = sizeof(struct kbufinfo_t);
    return sysctl(mib, 3, info, &needed, NULL, 0);
}

static int kdebug_trace_read(void *buf, size_t len, size_t *count) {
    if (count)
        *count = 0;
    if (!buf || !len)
        return -1;

    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREADTR};
    int ret = sysctl(mib, 3, buf, &len, NULL, 0);
    if (ret != 0)
        return ret;
    *count = len;
    return 0;
}

__attribute__((used)) static int kdebug_wait(size_t timeout_ms, int *suc) {
    if (timeout_ms == 0)
        return -1;
    int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDBUFWAIT};
    size_t val = timeout_ms;
    int ret = sysctl(mib, 3, NULL, &val, NULL, 0);
    if (suc)
        *suc = !!val;
    return ret;
}

#define EVENT_NAME_MAX 8
struct event_alias {
    const char *alias;
    const char *names[EVENT_NAME_MAX];
};

static const struct event_alias profile_events[] = {
    {"cycles",
     {
         "FIXED_CYCLES",
         "CPU_CLK_UNHALTED.THREAD",
         "CPU_CLK_UNHALTED.CORE",
     }},
    {"instructions", {"FIXED_INSTRUCTIONS", "INST_RETIRED.ANY"}},
    {"branches",
     {
         "INST_BRANCH",
         "BR_INST_RETIRED.ALL_BRANCHES",
         "INST_RETIRED.ANY",
     }},
    {"branch-misses",
     {
         "BRANCH_MISPRED_NONSPEC",
         "BRANCH_MISPREDICT",
         "BR_MISP_RETIRED.ALL_BRANCHES",
         "BR_INST_RETIRED.MISPRED",
     }},
};

static struct kpep_event *get_event(struct kpep_db *db,
                                    const struct event_alias *alias) {
    for (size_t j = 0; j < EVENT_NAME_MAX; j++) {
        const char *name = alias->names[j];
        if (!name)
            break;
        struct kpep_event *ev = NULL;
        if (kpep_db_event(db, name, &ev) == 0)
            return ev;
    }
    return NULL;
}

#define PERF_KPC (6)
#define PERF_KPC_DATA_THREAD (8)

int init_perf(void) {
    if (perf_lib_init() == -1)
        return -1;

    return 0;
}

void deinit_perf(void) { perf_lib_deinit(); }

int perf_cnt_collect(pid_t pid, int sync_pipe, struct perf_cnt *cnt) {
    int ret;
    // check permission
    int force_ctrs = 0;
    if (kpc_force_all_ctrs_get(&force_ctrs)) {
        fprintf(stderr,
                "error: permission denied, xnu/kpc requires root privileges\n");
        return -1;
    }

    struct kpep_db *db = NULL;
    if ((ret = kpep_db_create(NULL, &db)) != 0) {
        perror("kpep_db_create");
        return -1;
    }

    // create config
    struct kpep_config *cfg = NULL;
    if ((ret = kpep_config_create(db, &cfg)) != 0) {
        fprintf(stderr, "error: failed to create kpep config: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }
    if ((ret = kpep_config_force_counters(cfg)) != 0) {
        fprintf(stderr, "error: failed to force counters: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }

    // get events
    size_t ev_count = sizeof(profile_events) / sizeof(profile_events[0]);
    struct kpep_event
        *ev_arr[sizeof(profile_events) / sizeof(profile_events[0])] = {0};
    for (size_t i = 0; i < ev_count; i++) {
        const struct event_alias *alias = profile_events + i;
        ev_arr[i] = get_event(db, alias);
        if (!ev_arr[i]) {
            fprintf(stderr, "error: failed to find event: %s\n", alias->alias);
            return -1;
        }
    }

    // add event to config
    for (size_t i = 0; i < ev_count; i++) {
        struct kpep_event *ev = ev_arr[i];
        if ((ret = kpep_config_add_event(cfg, &ev, 0, NULL)) != 0) {
            fprintf(stderr, "error: failed to add event: %d (%s)\n", ret,
                    kpep_config_error_desc(ret));
            return -1;
        }
    }

    // prepare buffer and config
    uint32_t classes = 0;
    size_t reg_count = 0;
    kpc_config_t regs[KPC_MAX_COUNTERS] = {0};
    size_t counter_map[KPC_MAX_COUNTERS] = {0};
    if ((ret = kpep_config_kpc_classes(cfg, &classes)) != 0) {
        fprintf(stderr, "error: failed to get kpc classes: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }
    if ((ret = kpep_config_kpc_count(cfg, &reg_count)) != 0) {
        fprintf(stderr, "error: failed to get kpc count: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }
    if ((ret = kpep_config_kpc_map(cfg, counter_map, sizeof(counter_map))) !=
        0) {
        fprintf(stderr, "error: failed to get kpc map: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }
    if ((ret = kpep_config_kpc(cfg, regs, sizeof(regs))) != 0) {
        fprintf(stderr, "error: failed to get kpc registers: %d (%s)\n", ret,
                kpep_config_error_desc(ret));
        return -1;
    }

    // set config to kernel
    if ((ret = kpc_force_all_ctrs_set(1)) != 0) {
        perror("kpc_force_all_ctrs_set(1)");
        return -1;
    }
    if ((classes & KPC_CLASS_CONFIGURABLE_MASK) && reg_count) {
        if ((ret = kpc_set_config(classes, regs))) {
            perror("kpc_set_config");
            return -1;
        }
    }

    uint32_t counter_count = kpc_get_counter_count(classes);
    if (counter_count == 0) {
        fprintf(stderr, "error: no counters found\n");
        return -1;
    }

    // start counting
    if ((ret = kpc_set_counting(classes)) != 0) {
        perror("kpc_set_counting");
        return -1;
    }
    if ((ret = kpc_set_thread_counting(classes)) != 0) {
        perror("kpc_set_thread_counting");
        return -1;
    }

    // action id and timer id
    uint32_t actionid = 1;
    uint32_t timerid = 1;

    // alloc action and timer ids
    if ((ret = kperf_action_count_set(KPERF_ACTION_MAX)) != 0) {
        perror("kperf_action_count_set");
    }
    if ((ret = kperf_timer_count_set(KPERF_TIMER_MAX)) != 0) {
        perror("kperf_timer_count_set");
    }

    // set what to sample: PMC per thread
    if ((ret = kperf_action_samplers_set(actionid, KPERF_SAMPLER_PMC_THREAD)) !=
        0) {
        perror("kperf_action_samplers_set");
    }
    // set filter process
    if ((ret = kperf_action_filter_set_by_pid(actionid, pid)) != 0) {
        perror("kperf_action_filter_set_by_pid");
    }

    // setup PET (Profile Every Thread), start sampler
    double sample_period = 0.001;
    uint64_t tick = kperf_ns_to_ticks(sample_period * 1000000000ul);
    if ((ret = kperf_timer_period_set(actionid, tick)) != 0) {
        perror("kperf_timer_period_set");
    }
    if ((ret = kperf_timer_action_set(actionid, timerid)) != 0) {
        perror("kperf_timer_action_set");
    }
    if ((ret = kperf_timer_pet_set(timerid)) != 0) {
        perror("kperf_timer_pet_set");
    }
    if ((ret = kperf_lightweight_pet_set(1)) != 0) {
        perror("kperf_lightweight_pet_set");
    }
    if ((ret = kperf_sample_set(1))) {
        perror("kperf_sample_set(1)");
    }

    // reset kdebug/ktrace
    if ((ret = kdebug_reset()) != 0) {
        perror("kdebug_reset");
    }

    int nbufs = 1000000;
    if ((ret = kdebug_trace_setbuf(nbufs)) != 0) {
        perror("kdebug_trace_setbuf");
    }
    if ((ret = kdebug_reinit()) != 0) {
        perror("kdebug_reinit");
    }

    // set trace filter: only log PERF_KPC_DATA_THREAD
    struct kd_regtype kdr = {0};
    kdr.type = KDBG_VALCHECK;
    kdr.value1 = KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA_THREAD);
    if ((ret = kdebug_setreg(&kdr)) != 0) {
        perror("kdebug_setreg");
    }
    // start trace
    if ((ret = kdebug_trace_enable(1)) != 0) {
        perror("kdebug_trace_enable");
    }

    // sample and get buffers
    size_t buf_capacity = nbufs * 2;
    struct kd_buf *buf_hdr = malloc(sizeof(struct kd_buf) * buf_capacity);
    struct kd_buf *buf_cur = buf_hdr;

    // signal process to start executing
    write(sync_pipe, "", 1);
    close(sync_pipe);

    siginfo_t info = {0};
    if (waitid(P_PID, pid, &info, WEXITED | WNOWAIT) == -1) {
        perror("waitid");
        return -1;
    } else if (info.si_pid != 0) {
        // ok
    } else {
        // ???
    }
    {
        size_t count = 0;
        kdebug_trace_read(buf_cur, sizeof(struct kd_buf) * nbufs, &count);
        for (struct kd_buf *buf = buf_cur, *end = buf_cur + count; buf < end;
             ++buf) {
            uint32_t debugid = buf->debugid;
            uint32_t cls = KDBG_EXTRACT_CLASS(debugid);
            uint32_t subcls = KDBG_EXTRACT_SUBCLASS(debugid);
            uint32_t code = KDBG_EXTRACT_CODE(debugid);
            // this should always be true because of the settings we did above.
            if (cls != DBG_PERF || subcls != PERF_KPC ||
                code != PERF_KPC_DATA_THREAD)
                continue;
            memmove(buf_cur, buf, sizeof(struct kd_buf));
            ++buf_cur;
        }
    }

    // stop tracing
    kdebug_trace_enable(0);
    kdebug_reset();
    kperf_sample_set(0);
    kperf_lightweight_pet_set(0);

    // stop counting
    kpc_set_counting(0);
    kpc_set_thread_counting(0);
    kpc_force_all_ctrs_set(0);

    // aggregate thread PMC data
    if (buf_cur - buf_hdr == 0) {
        fprintf(stderr, "error: no thread PMC data collected\n");
        return 1;
    }

    struct {
        uint64_t timestamp_0;
        uint64_t timestamp_1;
        uint64_t counters_0[KPC_MAX_COUNTERS];
        uint64_t counters_1[KPC_MAX_COUNTERS];
    } data = {0};

    for (struct kd_buf *buf = buf_hdr; buf < buf_cur; buf++) {
        uint32_t func = buf->debugid & KDBG_FUNC_MASK;
        if (func != DBG_FUNC_START)
            continue;
        uint32_t tid = buf->arg5;
        if (!tid)
            continue;

        // read one counter log
        size_t ci = 0;
        uint64_t counters[KPC_MAX_COUNTERS];
        counters[ci++] = buf->arg1;
        counters[ci++] = buf->arg2;
        counters[ci++] = buf->arg3;
        counters[ci++] = buf->arg4;
        if (ci < counter_count) {
            // counter count larker than 4
            // values are split into multiple buffer entities
            for (struct kd_buf *buf2 = buf + 1; buf2 < buf_cur; buf2++) {
                uint32_t tid2 = buf2->arg5;
                if (tid2 != tid)
                    break;
                uint32_t func2 = buf2->debugid & KDBG_FUNC_MASK;
                if (func2 == DBG_FUNC_START)
                    break;
                if (ci < counter_count)
                    counters[ci++] = buf2->arg1;
                if (ci < counter_count)
                    counters[ci++] = buf2->arg2;
                if (ci < counter_count)
                    counters[ci++] = buf2->arg3;
                if (ci < counter_count)
                    counters[ci++] = buf2->arg4;
                if (ci == counter_count)
                    break;
            }
        }
        if (ci != counter_count)
            continue; // not enough counters, maybe truncated

        if (data.timestamp_0 == 0) {
            data.timestamp_0 = buf->timestamp;
            memcpy(data.counters_0, counters, counter_count * sizeof(uint64_t));
        } else {
            data.timestamp_1 = buf->timestamp;
            memcpy(data.counters_1, counters, counter_count * sizeof(uint64_t));
        }
    }

    for (size_t i = 0; i < ev_count; i++) {
        const struct event_alias *alias = profile_events + i;
        uint64_t val =
            data.counters_1[counter_map[i]] - data.counters_0[counter_map[i]];
        if (strcmp(alias->alias, "cycles") == 0) {
            cnt->cycles = val;
        } else if (strcmp(alias->alias, "instructions") == 0) {
            cnt->instructions = val;
        } else if (strcmp(alias->alias, "branches") == 0) {
            cnt->branches = val;
        } else if (strcmp(alias->alias, "branch-misses") == 0) {
            cnt->missed_branches = val;
        }
    }

    return 0;
}

#else
#error Unsupported Apple architecture
#endif
#endif
#endif