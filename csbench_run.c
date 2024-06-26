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
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static void apply_input_policy(const char *file) {
    if (file == NULL) {
        int fd = open("/dev/null", O_RDWR);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
    } else {
        int fd = open(file, O_RDONLY);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDIN_FILENO) == -1)
            _exit(-1);
        close(fd);
    }
}

static void apply_output_policy(enum output_kind policy) {
    switch (policy) {
    case OUTPUT_POLICY_NULL: {
        int fd = open("/dev/null", O_RDWR);
        if (fd == -1)
            _exit(-1);
        if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1)
            _exit(-1);
        close(fd);
        break;
    }
    case OUTPUT_POLICY_INHERIT:
        break;
    }
}

static int exec_cmd(const struct bench_params *params, struct rusage *rusage,
                    struct perf_cnt *pmc, bool is_warmup) {
    bool success = true;
    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return -1;
    }

    if (pid == 0) {
        apply_input_policy(params->input_file);
        if (is_warmup) {
            apply_output_policy(OUTPUT_POLICY_NULL);
        } else if (params->stdout_fd != -1) {
            // special handling when stdout needs to be piped
            int fd = open("/dev/null", O_RDWR);
            if (fd == -1)
                _exit(-1);
            if (dup2(fd, STDERR_FILENO) == -1)
                _exit(-1);
            if (dup2(params->stdout_fd, STDOUT_FILENO) == -1)
                _exit(-1);
            close(fd);
        } else {
            apply_output_policy(params->output);
        }
        if (pmc != NULL) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGUSR1);
            int sig;
            sigwait(&set, &sig);
        }
        if (execvp(params->exec, params->argv) == -1)
            _exit(-1);
    } else {
        if (pmc != NULL && !perf_cnt_collect(pid, pmc)) {
            success = false;
            kill(pid, SIGKILL);
        }
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

    int ret = -1;
    if (success) {
        // shell-like exit codes
        if (WIFEXITED(status))
            ret = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            ret = 128 + WTERMSIG(status);
    }
    if (ret == -1 && success)
        error("process finished with unexpected status");

    return ret;
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

static int tmpfile_fd(void) {
    char path[] = "/tmp/csbench_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        csperror("mkstemp");
        return -1;
    }
    unlink(path);
    return fd;
}

static bool do_custom_measurement(const struct meas *custom, int input_fd,
                                  double *valuep) {
    bool success = false;
    int custom_output_fd = tmpfile_fd();
    if (custom_output_fd == -1)
        return false;

    if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        goto out;
    }

    if (!execute_in_shell(custom->cmd, input_fd, custom_output_fd, -1))
        goto out;

    if (lseek(custom_output_fd, 0, SEEK_SET) == (off_t)-1) {
        csperror("lseek");
        goto out;
    }

    double value;
    if (!parse_custom_output(custom_output_fd, &value))
        goto out;

    *valuep = value;
    success = true;
out:
    assert(custom_output_fd != -1);
    close(custom_output_fd);
    return success;
}

static bool warmup(const struct bench_params *cmd) {
    double time_limit = g_warmup_time;
    if (time_limit <= 0.0)
        return true;
    double start_time = get_time();
    do {
        if (exec_cmd(cmd, NULL, NULL, true) == -1) {
            error("failed to execute warmup command");
            return false;
        }
    } while (get_time() - start_time < time_limit);
    return true;
}

// Execute benchmark and save output.
//
// This function contains some heavy logic. It handles the following:
// 1. Execute command
//  a. Using specified shell
//  b. Optionally setting stdin
//  c. Setting stdout and stderr, or saving stdout to file in case custom
//   measurements are used
// 2. Collect wall clock time duration of execution
// 2. Collect struct rusage of executed process
// 3. Optionally collect performance counters
// 4. Optionally check that command exit code is not zero
// 5. Execute custom measurements if specified
// 6. Save measurements specified in benchmark settings
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

    if (!g_allow_nonzero && rc != 0) {
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

static void progress_bar_start(struct progress_bar_bench *bench, double time) {
    if (!g_progress_bar)
        return;
    uint64_t u;
    memcpy(&u, &time, sizeof(u));
    atomic_store(&bench->start_time.u, u);
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
// This function also handles progress bar (see 'run_benchmark').
// During the benchmark run, progress bar is notified about current state
// of run using atomic variables (lock free and wait free).
// If the progress bar is disabled nothing concerning it shall be done.
static bool run_benchmark(const struct bench_params *params,
                          struct bench *bench) {
    // Check if we should run fixed number of times.
    if (g_bench_stop.runs != 0) {
        progress_bar_start(bench->progress, get_time());
        for (int run_idx = 0; run_idx < g_bench_stop.runs; ++run_idx) {
            if (g_prepare && !execute_in_shell(g_prepare, -1, -1, -1)) {
                error("failed to execute prepare command");
                progress_bar_abort(bench->progress);
                return false;
            }
            if (!exec_and_measure(params, bench)) {
                progress_bar_abort(bench->progress);
                return false;
            }
            int percent = (run_idx + 1) * 100 / g_bench_stop.runs;
            progress_bar_update_runs(bench->progress, percent, run_idx + 1);
        }
        progress_bar_update_runs(bench->progress, 100, g_bench_stop.runs);
        progress_bar_finished(bench->progress);
        return true;
    }
    double niter_accum = 1;
    size_t niter = 1;
    double start_time = get_time();
    double time_limit = g_bench_stop.time_limit;
    size_t min_runs = g_bench_stop.min_runs;
    size_t max_runs = g_bench_stop.max_runs;
    progress_bar_start(bench->progress, start_time);
    for (size_t count = 1;; ++count) {
        for (size_t run_idx = 0; run_idx < niter; ++run_idx) {
            if (g_prepare && !execute_in_shell(g_prepare, -1, -1, -1)) {
                error("failed to execute prepare command");
                progress_bar_abort(bench->progress);
                return false;
            }
            if (!exec_and_measure(params, bench)) {
                progress_bar_abort(bench->progress);
                return false;
            }
            double current = get_time();
            double diff = current - start_time;
            int progress = diff / time_limit * 100;
            progress_bar_update_time(bench->progress, progress, diff);
        }
        double current = get_time();
        double diff = current - start_time;
        if (((max_runs != 0 ? count >= max_runs : 0) || (diff > time_limit)) &&
            (min_runs != 0 ? count >= min_runs : 1))
            break;

        for (;;) {
            niter_accum *= 1.05;
            size_t new_niter = (size_t)floor(niter_accum);
            if (new_niter != niter)
                break;
        }
    }
    double passed = get_time() - start_time;
    progress_bar_update_time(bench->progress, 100, passed);
    progress_bar_finished(bench->progress);
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

    int tmp_fd = tmpfile_fd();
    if (tmp_fd == -1)
        return false;

    const struct meas **custom_meas_list = NULL;
    for (size_t meas_idx = 0; meas_idx < params->meas_count; ++meas_idx) {
        const struct meas *meas = params->meas + meas_idx;
        if (meas->kind == MEAS_CUSTOM)
            sb_push(custom_meas_list, meas);
    }
    assert(custom_meas_list);
    void *copy_buffer = malloc(max_stdout_size);

    for (size_t run_idx = 0; run_idx < bench->run_count; ++run_idx) {
        size_t count;
        if (run_idx == 0) {
            count = bench->stdout_offsets[run_idx];
        } else {
            count = bench->stdout_offsets[run_idx] -
                    bench->stdout_offsets[run_idx - 1];
        }
        assert(count <= max_stdout_size);

        ssize_t nr = read(all_stdout_fd, copy_buffer, count);
        if (nr != (ssize_t)count) {
            csperror("read");
            goto err;
        }
        ssize_t nw = write(tmp_fd, copy_buffer, count);
        if (nw != (ssize_t)count) {
            csperror("write");
            goto err;
        }
        if (ftruncate(tmp_fd, count) == -1) {
            csperror("ftruncate");
            goto err;
        }

        for (size_t m = 0; m < sb_len(custom_meas_list); ++m) {
            const struct meas *meas = custom_meas_list[m];
            double value;
            if (lseek(tmp_fd, 0, SEEK_SET) == -1) {
                csperror("lseek");
                goto err;
            }
            if (!do_custom_measurement(meas, tmp_fd, &value))
                goto err;
            sb_push(bench->meas[meas - params->meas], value);
        }
        // Reset write cursor before the next loop cycle
        if (run_idx != bench->run_count - 1) {
            if (lseek(tmp_fd, 0, SEEK_SET) == -1) {
                csperror("lseek");
                goto err;
            }
        }
    }

    success = true;
err:
    close(tmp_fd);
    free(copy_buffer);
    sb_free(custom_meas_list);
    return success;
}

bool run_bench(const struct bench_params *params, struct bench_analysis *al) {
    if (!warmup(params))
        return false;
    if (!run_benchmark(params, al->bench))
        return false;
    if (!run_custom_measurements(params, al->bench))
        return false;
    analyze_benchmark(al, params->meas_count);
    return true;
}
