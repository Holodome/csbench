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
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct run_state {
    double start_time;
    const struct bench_stop_policy *stop;
    int current_run;
    double time_run;
    bool ignore_suspend;
};

enum bench_run_result {
    BENCH_RUN_FINISHED,
    BENCH_RUN_ERROR,
    BENCH_RUN_SUSPENDED
};

// This structure contains information that is continiously updated by working
// threads when running a benchmark when progress bar is enabled.
// It is used to track completion status. This structure is read by progress bar
// thread to update display in console. Reads are not synchronized with writes,
// so there may be inconsistences. But they would not lead to any erroneous
// behaviour, and only affect consistency of information displayed. Either way,
// this is not critical.
struct progress_bar_bench {
    int bar;
    int finished;
    int aborted;
    int warmup;
    int suspended;
    union {
        uint64_t u;
        double d;
    } start_time;
    union {
        uint64_t u;
        double d;
    } time_passed;
    union {
        uint64_t u;
        double d;
    } metric;
    pthread_t id;
} __attribute__((aligned(64)));

struct progress_bar_state {
    size_t runs;
    double eta;
    double time;
    bool last_running;
};

struct progress_bar {
    pthread_t thread;
    bool was_drawn;
    size_t max_name_len;
    size_t count;
    struct progress_bar_bench *bar_benches;
    const struct bench *benches;
    struct progress_bar_state *states;
};

struct run_task {
    struct run_task_queue *q;
    const struct bench_params *param;
    struct bench *bench;
};

struct run_task_queue {
    size_t task_count;
    struct run_task *tasks;
    size_t worker_count;
    pthread_mutex_t mutex;
    bool *finished;
    bool *taken;
    size_t remaining_task_count;
};

struct custom_measurement_task {
    struct custom_measurement_task_queue *q;
    const struct bench_params *param;
    struct bench *bench;
};

struct custom_measurement_task_queue {
    size_t task_count;
    struct custom_measurement_task *tasks;
    volatile size_t cursor;
};

static void
init_custom_measurement_task_queue(const struct bench_params *params,
                                   struct bench *benches, size_t count,
                                   struct custom_measurement_task_queue *q) {
    memset(q, 0, sizeof(*q));
    q->task_count = count;
    q->tasks = calloc(count, sizeof(*q->tasks));
    for (size_t i = 0; i < count; ++i) {
        q->tasks[i].q = q;
        q->tasks[i].param = params + i;
        q->tasks[i].bench = benches + i;
    }
}

static void
free_custom_measurement_task_queue(struct custom_measurement_task_queue *q) {
    free(q->tasks);
}

static struct custom_measurement_task *
get_custom_measurement_task(struct custom_measurement_task_queue *q) {
    size_t idx = atomic_fetch_inc(&q->cursor);
    if (idx >= q->task_count)
        return NULL;
    return q->tasks + idx;
}

// Used by 'should_i_suspend'
static __thread struct run_task_queue *g_q;

#define lock_mutex(_mutex)                                                     \
    do {                                                                       \
        /* Mutex would not fail in normal circumstances, we can just use       \
         * assert */                                                           \
        int ret = pthread_mutex_lock(&q->mutex);                               \
        (void)ret;                                                             \
        assert(ret == 0);                                                      \
    } while (0)

#define unlock_mutex(_mutex)                                                   \
    do {                                                                       \
        /* Mutex would not fail in normal circumstances, we can just use       \
         * assert */                                                           \
        int ret = pthread_mutex_unlock(&q->mutex);                             \
        (void)ret;                                                             \
        assert(ret == 0);                                                      \
    } while (0)

static bool init_run_task_queue(const struct bench_params *params,
                                struct bench *benches, size_t count,
                                size_t worker_count, struct run_task_queue *q) {
    assert(worker_count <= count);
    memset(q, 0, sizeof(*q));
    int ret = pthread_mutex_init(&q->mutex, NULL);
    if (ret != 0) {
        errno = ret;
        csperror("pthread_mutex_init");
        return false;
    }

    q->task_count = q->remaining_task_count = count;
    q->tasks = calloc(count, sizeof(*q->tasks));
    q->finished = calloc(count, sizeof(*q->finished));
    q->taken = calloc(count, sizeof(*q->taken));
    q->worker_count = worker_count;
    for (size_t i = 0; i < count; ++i) {
        q->tasks[i].q = q;
        q->tasks[i].param = params + i;
        q->tasks[i].bench = benches + i;
    }
    return true;
}

static void free_run_task_queue(struct run_task_queue *q) {
    free(q->tasks);
    free(q->finished);
    free(q->taken);
    pthread_mutex_destroy(&q->mutex);
}

static void run_task_yield(struct run_task *task) {
    struct run_task_queue *q = task->q;
    size_t task_idx = task - q->tasks;
    lock_mutex(&q->mutex);
    assert(q->taken[task_idx] && !q->finished[task_idx]);
    q->taken[task_idx] = false;
    unlock_mutex(&q->mutex);
}

static void run_task_finish(struct run_task *task) {
    struct run_task_queue *q = task->q;
    size_t task_idx = task - q->tasks;
    lock_mutex(&q->mutex);
    assert(q->taken[task_idx] && !q->finished[task_idx]);
    q->taken[task_idx] = false;
    q->finished[task_idx] = true;
    --q->remaining_task_count;
    unlock_mutex(&q->mutex);
}

static struct run_task *get_run_task(struct run_task_queue *q,
                                     size_t *task_cursor) {
    struct run_task *task = NULL;
    lock_mutex(&q->mutex);

    if (q->remaining_task_count != 0) {
        for (size_t i = 0; i < q->task_count && !task; ++i) {
            size_t idx = (*task_cursor + i) % q->task_count;
            if (!q->finished[idx] && !q->taken[idx])
                task = q->tasks + idx;
        }
        assert(task || (q->worker_count > q->remaining_task_count));
        if (task) {
            size_t idx = task - q->tasks;
            *task_cursor = (idx + 1) % q->task_count;
            q->taken[idx] = true;
            assert(!q->finished[task - q->tasks]);
        }
    }

    unlock_mutex(&q->mutex);
    return task;
}

// There is no point suspending when all tasks are already assigned to workers,
// that would just waste time. Worker threads should call this function when
// they want to suspend.
static bool should_i_suspend(void) {
    struct run_task_queue *q = g_q;
    lock_mutex(&q->mutex);
    bool result = q->worker_count < q->remaining_task_count;
    unlock_mutex(&q->mutex);
    return result;
}

static void apply_input_policy(int stdin_fd, int err_pipe_end) {
    if (stdin_fd == -1) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd == -1) {
            csfdperror(err_pipe_end, "open(\"/dev/null\", O_RDONLY)");
            _exit(-1);
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            csfdperror(err_pipe_end, "dup2");
            _exit(-1);
        }
        close(fd);
    } else {
        if (lseek(stdin_fd, 0, SEEK_SET) == -1) {
            csfdperror(err_pipe_end, "lseek");
            _exit(-1);
        }
        if (dup2(stdin_fd, STDIN_FILENO) == -1) {
            csfdperror(err_pipe_end, "dup2");
            _exit(-1);
        }
    }
}

static void apply_output_policy(enum output_kind policy, int err_pipe_end) {
    switch (policy) {
    case OUTPUT_POLICY_NULL: {
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1) {
            csfdperror(err_pipe_end, "open(\"/dev/null\", O_WRONLY)");
            _exit(-1);
        }
        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
            csfdperror(err_pipe_end, "dup2");
            _exit(-1);
        }
        close(fd);
        break;
    }
    case OUTPUT_POLICY_INHERIT:
        break;
    }
}

static void exec_cmd_child(const struct bench_params *params,
                           struct perf_cnt *pmc, bool is_warmup,
                           int err_pipe_end) {
    apply_input_policy(params->stdin_fd, err_pipe_end);
    if (is_warmup) {
        apply_output_policy(OUTPUT_POLICY_NULL, err_pipe_end);
    } else if (params->stdout_fd != -1) {
        // special handling when stdout needs to be piped
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1) {
            csfdperror(err_pipe_end, "open(\"/dev/null\", O_WRONLY)");
            _exit(-1);
        }
        if (dup2(fd, STDERR_FILENO) == -1 ||
            dup2(params->stdout_fd, STDOUT_FILENO) == -1) {
            csfdperror(err_pipe_end, "dup2");
            _exit(-1);
        }
        close(fd);
    } else {
        apply_output_policy(params->output, err_pipe_end);
    }
    if (pmc != NULL) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        int sig;
        if (sigwait(&set, &sig) != 0) {
            csfdperror(err_pipe_end, "sigwait");
            _exit(-1);
        }
    }
    if (execvp(params->exec, (char **)params->argv) == -1) {
        csfdperror(err_pipe_end, "execvp");
        _exit(-1);
    }

    __builtin_unreachable();
}

static int exec_cmd_internal(const struct bench_params *params,
                             struct rusage *rusage, struct perf_cnt *pmc,
                             bool is_warmup, int err_pipe[2]) {
    bool success = true;

    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return -1;
    }

    if (pid == 0) {
        exec_cmd_child(params, pmc, is_warmup, err_pipe[1]);
    }

    if (pmc != NULL && !perf_cnt_collect(pid, pmc)) {
        success = false;
        kill(pid, SIGKILL);
    }

    int status = 0;
    pid_t wpid;
    for (;;) {
        if ((wpid = wait4(pid, &status, 0, rusage)) != pid) {
            if (wpid == -1 && errno == EINTR)
                continue;
            if (wpid == -1)
                csperror("wait4");
            return -1;
        }
        break;
    }

    if (!check_and_handle_err_pipe(err_pipe[0], 0))
        return -1;

    int ret = -1;
    if (success) {
        // shell-like exit codes
        if (WIFEXITED(status))
            ret = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            ret = 128 + WTERMSIG(status);
    }
    if (ret == -1 && success)
        error("process finished with unexpected status (%d)", status);

    return ret;
}

static int exec_cmd(const struct bench_params *params, struct rusage *rusage,
                    struct perf_cnt *pmc, bool is_warmup) {
    int err_pipe[2];
    if (!pipe_cloexec(err_pipe))
        return -1;
    int result = exec_cmd_internal(params, rusage, pmc, is_warmup, err_pipe);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return result;
}

static void init_run_state(double time, const struct bench_stop_policy *policy,
                           size_t run, double time_already_run,
                           struct run_state *state) {
    memset(state, 0, sizeof(*state));
    state->current_run = run;
    state->start_time = time;
    state->stop = policy;
    state->time_run = time_already_run;
    state->ignore_suspend = false;
}

static bool should_run(const struct bench_stop_policy *policy) {
    if (policy->time_limit <= 0.0)
        return false;

    if (policy->runs == 0 && policy->min_runs == 0)
        return false;

    return true;
}

static bool should_finish_running(struct run_state *state, int advance) {
    state->current_run += advance;

    if (state->stop->runs > 0) {
        if (state->current_run >= state->stop->runs)
            return true;
    }

    if (state->stop->min_runs > 0) {
        if (state->current_run < state->stop->min_runs)
            return false;
    }

    if (state->stop->max_runs > 0) {
        if (state->current_run >= state->stop->max_runs)
            return true;
    }

    double current = get_time();
    double passed = current - state->start_time;
    if (passed > state->stop->time_limit - state->time_run)
        return true;

    return false;
}

static bool should_suspend_round(struct run_state *state) {
    assert(state->stop == &g_round_stop);
    if (state->ignore_suspend)
        return false;

    bool result = should_finish_running(state, 1);
    if (result) {
        if (!should_i_suspend()) {
            state->ignore_suspend = true;
            result = false;
        }
    }

    return result;
}

static bool warmup(const struct bench_params *cmd) {
    if (!should_run(&g_warmup_stop))
        return true;

    struct run_state state;
    init_run_state(get_time(), &g_warmup_stop, 0, 0, &state);
    for (;;) {
        if (exec_cmd(cmd, NULL, NULL, true) == -1) {
            error("failed to execute warmup command");
            return false;
        }
        if (should_finish_running(&state, 1))
            break;
    }
    return true;
}

// Execute benchmark and save output.
//
// This function contains some heavy logic. It handles the following:
// 1. Execute command
//  a. Using specified shell
//  b. Optionally setting stdin
//  c. Setting stdout and stderr, or saving stdout to file in case custom
//       measurements are used
// 2. Collect wall clock time duration of execution
// 2. Collect struct rusage of executed process
// 3. Optionally collect performance counters
// 4. Optionally check that command exit code is not zero
// 5. Collect all stdout outputs to a single file if custom measurements are
//      used and store indexes to be able to identify each run
// 6. Collect all measurements specified
static bool exec_and_measure(const struct bench_params *params,
                             struct bench *bench) {
    struct rusage rusage;
    memset(&rusage, 0, sizeof(rusage));
    struct perf_cnt pmc_ = {0};
    struct perf_cnt *pmc = NULL;
    if (g_use_perf)
        pmc = &pmc_;
    volatile double wall_clock_start = get_time();
    volatile int rc = exec_cmd(params, &rusage, pmc, false);
    volatile double wall_clock_end = get_time();

    // Some internal error
    if (rc == -1)
        return false;

    if (!g_ignore_failure && rc != 0) {
        error("command '%s' finished with non-zero exit code (%d)", params->str,
              rc);
        return false;
    }

    ++bench->run_count;
    sb_push(bench->exit_codes, rc);
    // If we have stdout means we have to save stdout ouput. To do that store
    // offsets and write outputs to one file.
    if (params->stdout_fd != -1) {
        off_t position = lseek(params->stdout_fd, 0, SEEK_CUR);
        if (position == -1) {
            csperror("lseek");
            return false;
        }
        sb_push(bench->stdout_offsets, position);
    }
    for (size_t meas_idx = 0; meas_idx < params->meas_count; ++meas_idx) {
        const struct meas *meas = params->meas + meas_idx;
        // Handled separately
        if (meas->kind == MEAS_CUSTOM)
            continue;
        double val = 0.0;
        switch (meas->kind) {
        case MEAS_WALL:
            val = wall_clock_end - wall_clock_start;
            break;
        case MEAS_RUSAGE_STIME:
            val =
                rusage.ru_stime.tv_sec + (double)rusage.ru_stime.tv_usec / 1e6;
            break;
        case MEAS_RUSAGE_UTIME:
            val =
                rusage.ru_utime.tv_sec + (double)rusage.ru_utime.tv_usec / 1e6;
            break;
        case MEAS_RUSAGE_MAXRSS:
            val = rusage.ru_maxrss;
            break;
        case MEAS_RUSAGE_MINFLT:
            val = rusage.ru_minflt;
            break;
        case MEAS_RUSAGE_MAJFLT:
            val = rusage.ru_majflt;
            break;
        case MEAS_RUSAGE_NVCSW:
            val = rusage.ru_nvcsw;
            break;
        case MEAS_RUSAGE_NIVCSW:
            val = rusage.ru_nivcsw;
            break;
        case MEAS_PERF_CYCLES:
            assert(pmc);
            val = pmc->cycles;
            break;
        case MEAS_PERF_INS:
            assert(pmc);
            val = pmc->instructions;
            break;
        case MEAS_PERF_BRANCH:
            assert(pmc);
            val = pmc->branches;
            break;
        case MEAS_PERF_BRANCHM:
            assert(pmc);
            val = pmc->missed_branches;
            break;
        case MEAS_CUSTOM:
            assert(0);
            break;
        case MEAS_LOADED:
            assert(0);
        }
        sb_push(bench->meas[meas_idx], val);
    }
    return true;
}

static void progress_bar_at_warmup(struct progress_bar_bench *bench) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->suspended, false);
    atomic_store(&bench->warmup, true);
}

static void progress_bar_start(struct progress_bar_bench *bench, double time) {
    if (!g_progress_bar)
        return;
    uint64_t u;
    memcpy(&u, &time, sizeof(u));
    atomic_store(&bench->start_time.u, u);
    atomic_store(&bench->warmup, false);
}

static void progress_bar_abort(struct progress_bar_bench *bench) {
    if (!g_progress_bar)
        return;
    pthread_t id = pthread_self();
    memcpy(&bench->id, &id, sizeof(id));
    atomic_fence();
    atomic_store(&bench->aborted, true);
    atomic_store(&bench->finished, true);
}

static void progress_bar_finished(struct progress_bar_bench *bench) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->finished, true);
}

static void progress_bar_update_time(struct progress_bar_bench *bench,
                                     int percent, double t) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->bar, percent);
    uint64_t metric;
    memcpy(&metric, &t, sizeof(metric));
    atomic_store(&bench->metric.u, metric);
}

static void progress_bar_update_runs(struct progress_bar_bench *bench,
                                     int percent, size_t runs) {
    if (!g_progress_bar)
        return;
    atomic_store(&bench->bar, percent);
    atomic_store(&bench->metric.u, runs);
}

static void progress_bar_suspend(struct progress_bar_bench *bench,
                                 double time_passed) {
    if (!g_progress_bar)
        return;
    uint64_t u;
    memcpy(&u, &time_passed, sizeof(u));
    atomic_store(&bench->time_passed.u, u);
    atomic_store(&bench->suspended, true);
}

static bool run_prepare_if_needed(void) {
    if (g_prepare && !shell_execute_and_wait(g_prepare, -1, -1, -1)) {
        error("failed to execute prepare command");
        return false;
    }
    return true;
}

static enum bench_run_result
run_benchmark_exact_runs(const struct bench_params *params,
                         struct bench *bench) {
    struct run_state round_state;
    init_run_state(get_time(), &g_round_stop, 0, 0, &round_state);
    for (int run_idx = bench->run_count; run_idx < g_bench_stop.runs;
         ++run_idx) {
        if (!run_prepare_if_needed())
            return BENCH_RUN_ERROR;
        if (!exec_and_measure(params, bench))
            return BENCH_RUN_ERROR;
        int percent = (run_idx + 1) * 100 / g_bench_stop.runs;
        progress_bar_update_runs(bench->progress, percent, run_idx + 1);
        // Check if we should suspend, but only if not this is the last round
        if (run_idx != g_bench_stop.runs - 1 &&
            should_suspend_round(&round_state)) {
            bench->time_run += get_time() - round_state.start_time;
            return BENCH_RUN_SUSPENDED;
        }
    }
    progress_bar_update_runs(bench->progress, 100, g_bench_stop.runs);
    return BENCH_RUN_FINISHED;
}

static enum bench_run_result
run_benchmark_adaptive_runs(const struct bench_params *params,
                            struct bench *bench) {
    double start_time = get_time();
    struct run_state state, round_state;
    init_run_state(start_time, &g_bench_stop, bench->run_count, bench->time_run,
                   &state);
    init_run_state(start_time, &g_round_stop, 0, 0, &round_state);
    for (;;) {
        if (!run_prepare_if_needed())
            return BENCH_RUN_ERROR;
        if (!exec_and_measure(params, bench))
            return BENCH_RUN_ERROR;

        double current = get_time();
        double time_in_round_passed = current - start_time;
        double bench_time_passed = time_in_round_passed + bench->time_run;
        int progress = bench_time_passed / g_bench_stop.time_limit * 100;
        progress_bar_update_time(bench->progress, progress, bench_time_passed);

        if (should_finish_running(&state, 1))
            goto out;

        if (should_suspend_round(&round_state)) {
            bench->time_run += time_in_round_passed;
            return BENCH_RUN_SUSPENDED;
        }
    }
    double passed;
out:
    passed = get_time() - start_time;
    progress_bar_update_time(bench->progress, 100, passed + bench->time_run);
    return BENCH_RUN_FINISHED;
}

// This function is entry point to benchmark running. Accepting description of
// benchmark, it runs it according to 'g_bench_stop' policy, saving timing
// results.
//
// There are two main ways to execute benchmark: by time and by run count.
// In the first case benchmark is run until certain wall clock time has passed
// since benchmark start, optionally constrained bu minimum and maximum run
// count. In the second case we run benchmark exactly for specified number of
// runs.
//
// This function also handles progress bar.
// During the benchmark run, progress bar is notified about current state
// of run using atomic variables (lock free and wait free).
// If the progress bar is disabled nothing concerning it shall be done.
static enum bench_run_result run_bench(const struct bench_params *params,
                                       struct bench *bench) {
    progress_bar_at_warmup(bench->progress);

    if (!warmup(params))
        return BENCH_RUN_ERROR;

    assert(should_run(&g_bench_stop));
    progress_bar_start(bench->progress, get_time());
    // Check if we should run fixed number of times. We can't unify these cases
    // because they have different logic of handling progress bar status.
    enum bench_run_result result;
    if (g_bench_stop.runs != 0) {
        result = run_benchmark_exact_runs(params, bench);
    } else {
        result = run_benchmark_adaptive_runs(params, bench);
    }

    switch (result) {
    case BENCH_RUN_FINISHED:
        progress_bar_finished(bench->progress);
        break;
    case BENCH_RUN_ERROR:
        progress_bar_abort(bench->progress);
        break;
    case BENCH_RUN_SUSPENDED:
        progress_bar_suspend(bench->progress, bench->time_run);
        break;
    }
    return result;
}

static bool run_benches_single_threaded(struct run_task_queue *q) {
    g_q = q;
    // Tasks are switched round-robin. For this, store in each runner index of
    // next task that should be processed.
    size_t task_cursor = pcg32_fast(&g_rng_state) % q->task_count;
    for (;;) {
        struct run_task *task = get_run_task(q, &task_cursor);
        if (task == NULL)
            break;

        enum bench_run_result result = run_bench(task->param, task->bench);
        switch (result) {
        case BENCH_RUN_FINISHED:
            run_task_finish(task);
            break;
        case BENCH_RUN_ERROR:
            return false;
        case BENCH_RUN_SUSPENDED:
            run_task_yield(task);
            break;
        }
    }
    g_q = NULL;
    return true;
}

static void *run_bench_worker(void *raw) {
    struct run_task_queue *q = raw;
    init_rng_state();
    if (!run_benches_single_threaded(q))
        return (void *)-1;
    return NULL;
}

static void redraw_progress_bar(struct progress_bar *bar) {
    bool abbr_names = bar->max_name_len > 40;
    int length = 40;
    if (!bar->was_drawn) {
        bar->was_drawn = true;
        if (abbr_names) {
            for (size_t i = 0; i < bar->count; ++i) {
                printf("%c = ", (int)('A' + i));
                printf_colored(ANSI_BOLD, "%s\n", bar->benches[i].name);
            }
        }
    } else {
        printf("\x1b[%zuA\r", bar->count);
    }

    double current_time = get_time();
    for (size_t i = 0; i < bar->count; ++i) {
        struct progress_bar_bench data = {0};
        // explicitly load all atomics to avoid UB (tsan)
        atomic_fence();
        data.bar = atomic_load(&bar->bar_benches[i].bar);
        data.finished = atomic_load(&bar->bar_benches[i].finished);
        data.aborted = atomic_load(&bar->bar_benches[i].aborted);
        data.suspended = atomic_load(&bar->bar_benches[i].suspended);
        data.warmup = atomic_load(&bar->bar_benches[i].warmup);
        data.metric.u = atomic_load(&bar->bar_benches[i].metric.u);
        data.start_time.u = atomic_load(&bar->bar_benches[i].start_time.u);
        data.time_passed.u = atomic_load(&bar->bar_benches[i].time_passed.u);
        if (abbr_names)
            printf("%c ", (int)('A' + i));
        else
            printf_colored(ANSI_BOLD, "%*s ", (int)bar->max_name_len,
                           bar->benches[i].name);
        char buf[41] = {0};
        int c = data.bar * length / 100;
        if (c > length)
            c = length;
        for (int j = 0; j < c; ++j)
            buf[j] = '#';
        printf_colored(ANSI_BRIGHT_BLUE, "%s", buf);
        for (int j = 0; j < length - c; ++j)
            buf[j] = '-';
        buf[length - c] = '\0';
        printf_colored(ANSI_BLUE, "%s", buf);
        if (data.aborted) {
            memcpy(&data.id, &bar->bar_benches[i].id, sizeof(data.id));
            for (size_t j = 0; j < sb_len(g_output_anchors); ++j) {
                const struct output_anchor *anchor = g_output_anchors + j;
                if (pthread_equal(anchor->id, data.id)) {
                    assert(anchor->has_message);
                    printf_colored(ANSI_RED, " error: ");
                    printf("%s", anchor->buffer);
                    break;
                }
            }
            printf("\n");
            continue;
        }

        if (g_bench_stop.runs != 0) {
            char eta_buf[256] = "N/A     ";
            // I don't hope that anyone would be able to understand ETA
            // calculating code, but it is roughly divided in three parts.
            // First, there is some tracker of benchmark time in total
            // (data.time_passed.d and passed_time variable). It should grow
            // linearly and reflect the actual time spent running. Next,
            // we calculate ETA when run count changes using this total passed
            // time. There are corner cases with this number when benchmark goes
            // from suspended to running and vice versa. We have to explicitly
            // recalculate ETA in this case because it will otherwise be
            // inconsistent (bar->states[i].last_running). Lastly, when
            // benchmark is actually running, we subtract from ETA time passed
            // in current round to update it dynamically.
            if (data.start_time.d != 0 &&
                (data.suspended || data.warmup || data.finished)) {
                if (bar->states[i].last_running ||
                    (bar->states[i].runs != data.metric.u)) {
                    bar->states[i].eta = (g_bench_stop.runs - data.metric.u) *
                                         data.time_passed.d / data.metric.u;
                    bar->states[i].runs = data.metric.u;
                }
                double eta = bar->states[i].eta;
                if (isfinite(eta))
                    format_time(eta_buf, sizeof(eta_buf), eta);
                bar->states[i].last_running = false;
            } else if (data.start_time.d != 0.0) {
                // Check if the benchmark updated progress bar since
                // last time we checked and recalculate ETA accordingly
                if (!bar->states[i].last_running ||
                    bar->states[i].runs != data.metric.u) {
                    double passed_time =
                        data.time_passed.d + current_time - data.start_time.d;
                    bar->states[i].eta = (g_bench_stop.runs - data.metric.u) *
                                         passed_time / data.metric.u;
                    bar->states[i].time = current_time;
                    bar->states[i].runs = data.metric.u;
                }
                // Adjust ETA using time that has passed in current run
                double eta =
                    bar->states[i].eta - (current_time - bar->states[i].time);
                if (isfinite(eta))
                    format_time(eta_buf, sizeof(eta_buf), eta);
                bar->states[i].last_running = true;
            }
            char total_buf[256];
            snprintf(total_buf, sizeof(total_buf), "%zu",
                     (size_t)g_bench_stop.runs);
            printf(" %*zu/%s eta %s", (int)strlen(total_buf),
                   (size_t)data.metric.u, total_buf, eta_buf);
        } else {
            char buf1[256], buf2[256];
            format_time(buf1, sizeof(buf1), data.metric.d);
            format_time(buf2, sizeof(buf2), g_bench_stop.time_limit);
            printf(" %s/ %s", buf1, buf2);
        }

        if (data.suspended) {
            printf_colored(ANSI_YELLOW, " S  ");
        } else if (data.warmup) {
            printf_colored(ANSI_MAGENTA, " W  ");
        } else {
            printf("    ");
        }
        printf("\n");
    }
}

static void *progress_bar_thread_worker(void *arg) {
    assert(g_progress_bar);
    struct progress_bar *bar = arg;
    bool is_finished = false;
    redraw_progress_bar(bar);
    do {
        usleep(100000);
        redraw_progress_bar(bar);
        is_finished = true;
        for (size_t i = 0; i < bar->count && is_finished; ++i)
            if (!atomic_load(&bar->bar_benches[i].finished))
                is_finished = false;
    } while (!is_finished);
    redraw_progress_bar(bar);
    return NULL;
}

static void init_progress_bar(struct bench *benches, size_t count,
                              struct progress_bar *bar) {
    memset(bar, 0, sizeof(*bar));
    bar->count = count;
    bar->bar_benches = calloc(count, sizeof(*bar->bar_benches));
    bar->states = calloc(count, sizeof(*bar->states));
    bar->benches = benches;
    for (size_t i = 0; i < count; ++i) {
        bar->states[i].runs = -1;
        benches[i].progress = bar->bar_benches + i;
        size_t name_len = strlen(benches[i].name);
        if (name_len > bar->max_name_len)
            bar->max_name_len = name_len;
    }
}

static void free_progress_bar(struct progress_bar *bar) {
    free(bar->bar_benches);
    free(bar->states);
}

static bool run_benches_multi_threaded(struct run_task_queue *q,
                                       size_t thread_count) {
    return spawn_threads(run_bench_worker, q, thread_count);
}

static bool execute_run_tasks(const struct bench_params *params,
                              struct bench *benches, size_t count,
                              size_t thread_count) {
    struct run_task_queue q;
    if (!init_run_task_queue(params, benches, count, thread_count, &q))
        return false;

    bool success;
    if (thread_count == 1) {
        success = run_benches_single_threaded(&q);
    } else {
        success = run_benches_multi_threaded(&q, thread_count);
    }

    free_run_task_queue(&q);
    return success;
}

static bool parse_custom_output(int fd, double *valuep) {
    char buf[4096];
    ssize_t nread = read(fd, buf, sizeof(buf));
    if (nread == -1) {
        csperror("read");
        return false;
    }
    if (nread == sizeof(buf)) {
        error("custom measurement output is too large");
        return false;
    }
    if (nread == 0) {
        error("custom measurement output is empty");
        return false;
    }
    buf[nread] = '\0';
    char *end = NULL;
    double value = strtod(buf, &end);
    if (end == buf) {
        error("invalid custom measurement output '%s'", buf);
        return false;
    }
    *valuep = value;
    return true;
}

static bool do_custom_measurement(const struct meas *custom, int input_fd,
                                  int output_fd, double *valuep) {
    // XXX: This is optimization to not spawn separate process when custom
    // command just forwards input. We could create separate entry in 'enum
    // meas_kind', but this is really not that important case to design against.
    // Going furhter, we could avoid using file descriptors at all, but this
    // would require noticeable code changes, and I am too lazy for that.
    // Most of the time is spent in spawning processes anyway, so we cut it down
    // significantly either way.
    if (strcmp(custom->cmd, "cat") == 0) {
        double value;
        if (!parse_custom_output(input_fd, &value))
            return false;
        *valuep = value;
        return true;
    }

    if (lseek(output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        return false;
    }

    if (ftruncate(output_fd, 0) == -1) {
        csperror("ftruncate");
        return false;
    }

    if (!shell_execute_and_wait(custom->cmd, input_fd, output_fd, -1))
        return false;

    if (lseek(output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        return false;
    }

    double value;
    if (!parse_custom_output(output_fd, &value))
        return false;

    *valuep = value;
    return true;
}

static bool run_custom_measurements(const struct bench_params *params,
                                    struct bench *bench) {
    bool success = false;
    int all_stdout_fd = params->stdout_fd;
    // If stdout_fd is not set means we have no custom measurements
    if (all_stdout_fd == -1)
        return true;

    if (lseek(all_stdout_fd, 0, SEEK_SET) == -1) {
        csperror("lseek");
        return false;
    }

    size_t max_stdout_size = bench->stdout_offsets[0];
    for (size_t i = 1; i < bench->run_count; ++i) {
        size_t d = bench->stdout_offsets[i] - bench->stdout_offsets[i - 1];
        if (d > max_stdout_size)
            max_stdout_size = d;
    }

    int input_fd = tmpfile_fd();
    if (input_fd == -1)
        return false;
    int output_fd = tmpfile_fd();
    if (output_fd == -1) {
        close(input_fd);
        return false;
    }

    const struct meas **custom_meas_list = NULL;
    for (size_t meas_idx = 0; meas_idx < params->meas_count; ++meas_idx) {
        const struct meas *meas = params->meas + meas_idx;
        if (meas->kind == MEAS_CUSTOM)
            sb_push(custom_meas_list, meas);
    }
    assert(custom_meas_list);
    void *copy_buffer = malloc(max_stdout_size);

    for (size_t run_idx = 0; run_idx < bench->run_count; ++run_idx) {
        size_t run_stdout_len;
        if (run_idx == 0) {
            run_stdout_len = bench->stdout_offsets[run_idx];
        } else {
            run_stdout_len = bench->stdout_offsets[run_idx] -
                             bench->stdout_offsets[run_idx - 1];
        }
        assert(run_stdout_len <= max_stdout_size);

        ssize_t nr = read(all_stdout_fd, copy_buffer, run_stdout_len);
        if (nr != (ssize_t)run_stdout_len) {
            csperror("read");
            goto err;
        }
        ssize_t nw = write(input_fd, copy_buffer, run_stdout_len);
        if (nw != (ssize_t)run_stdout_len) {
            csperror("write");
            goto err;
        }
        if (ftruncate(input_fd, run_stdout_len) == -1) {
            csperror("ftruncate");
            goto err;
        }

        for (size_t m = 0; m < sb_len(custom_meas_list); ++m) {
            const struct meas *meas = custom_meas_list[m];
            double value;
            if (lseek(input_fd, 0, SEEK_SET) == -1) {
                csperror("lseek");
                goto err;
            }
            if (!do_custom_measurement(meas, input_fd, output_fd, &value))
                goto err;
            sb_push(bench->meas[meas - params->meas], value);
        }
        // Reset write cursor before the next loop cycle
        if (run_idx != bench->run_count - 1) {
            if (lseek(input_fd, 0, SEEK_SET) == -1) {
                csperror("lseek");
                goto err;
            }
        }
    }

    success = true;
err:
    close(input_fd);
    close(output_fd);
    free(copy_buffer);
    sb_free(custom_meas_list);
    return success;
}

static void *custom_measurement_bench_worker(void *arg) {
    struct custom_measurement_task_queue *q = arg;
    init_rng_state();
    for (;;) {
        struct custom_measurement_task *task = get_custom_measurement_task(q);
        if (!task)
            break;
        if (!run_custom_measurements(task->param, task->bench))
            return (void *)-1;
    }
    return NULL;
}

static bool execute_custom_measurement_tasks(const struct bench_params *params,
                                             struct bench *benches,
                                             size_t count,
                                             size_t thread_count) {
    struct custom_measurement_task_queue q;
    init_custom_measurement_task_queue(params, benches, count, &q);
    bool success = false;
    if (thread_count == 1) {
        const void *result = custom_measurement_bench_worker(&q);
        if (result == NULL)
            success = true;
        else
            success = false;
    } else {
        success =
            spawn_threads(custom_measurement_bench_worker, &q, thread_count);
    }
    free_custom_measurement_task_queue(&q);
    return success;
}

// Execute benchmarks, possibly in parallel using worker threads.
// When parallel execution is used, thread pool is created, threads from
// which select a benchmark to run in random order. We shuffle the
// benchmarks here in orderd to get asymptotically OK runtime, as incorrect
// order of tasks in parallel execution can degrade performance (queueing
// theory).
//
// Parallel execution is controlled using 'g_threads' global
// variable.
//
// This function also optionally spawns the thread printing interactive
// progress bar. Logic conserning progress bar may be cumbersome:
// 1. A new thread is spawned, which wakes ones in a while and checks atomic
//  variables storing states of benchmarks
// 2. Each of benchmarks updates its state in corresponding atomic varibales
// 3. Output of benchmarks when progress bar is used is captured (anchored),
//   see 'error' and 'csperror' functions. This is done in order to not corrupt
//   the output in case such message is printed.
static bool run_benches_internal(const struct bench_params *params,
                                 struct bench *benches, size_t count) {
    bool success = false;
    struct progress_bar *progress_bar = NULL;
    if (g_progress_bar) {
        progress_bar = calloc(1, sizeof(*progress_bar));
        init_progress_bar(benches, count, progress_bar);
        if (pthread_create(&progress_bar->thread, NULL,
                           progress_bar_thread_worker, progress_bar) != 0) {
            error("failed to spawn thread");
            free_progress_bar(progress_bar);
            free(progress_bar);
            return false;
        }
    }

    size_t thread_count = g_threads;
    if (count < thread_count)
        thread_count = count;
    assert(thread_count > 0);

    if (g_progress_bar) {
        sb_resize(g_output_anchors, thread_count);
        if (thread_count == 1)
            g_output_anchors[0].id = pthread_self();
    }

    if (!execute_run_tasks(params, benches, count, thread_count))
        goto out;

    if (!execute_custom_measurement_tasks(params, benches, count, thread_count))
        goto out;

    success = true;
out:
    if (progress_bar) {
        pthread_join(progress_bar->thread, NULL);
        free_progress_bar(progress_bar);
        free(progress_bar);
    }
    // Clean up anchors after execution
    struct output_anchor *anchors = g_output_anchors;
    g_output_anchors = NULL;
    sb_free(anchors);
    return success;
}

bool run_benches(const struct bench_params *params, struct bench *benches,
                 size_t count) {
    if (g_use_perf && !init_perf())
        return false;
    bool success = run_benches_internal(params, benches, count);
    if (g_use_perf)
        deinit_perf();
    return success;
}

