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
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

struct string_ll {
    char *str;
    struct string_ll *next;
};

static struct string_ll *string_ll = NULL;

void *sb_grow_impl(void *arr, size_t inc, size_t stride)
{
    if (arr == NULL) {
        void *result = calloc(sizeof(struct sb_header) + stride * inc, 1);
        struct sb_header *header = result;
        header->size = 0;
        header->capacity = inc;
        return header + 1;
    }

    struct sb_header *header = sb_header(arr);
    size_t double_current = header->capacity * 2;
    size_t min = header->size + inc;

    size_t new_capacity = double_current > min ? double_current : min;
    void *result = realloc(header, sizeof(struct sb_header) + stride * new_capacity);
    header = result;
    header->capacity = new_capacity;
    return header + 1;
}

bool units_is_time(const struct units *units)
{
    switch (units->kind) {
    case MU_S:
    case MU_MS:
    case MU_NS:
    case MU_US:
        return true;
    default:
        break;
    }
    return false;
}

int format_time(char *dst, size_t sz, double t)
{
    int count = 0;
    if (t < 0) {
        t = -t;
        count = snprintf(dst, sz, "-");
        dst += count;
        sz -= count;
    }

    const char *units = "s ";
    if (t >= 1) {
    } else if (t >= 1e-3) {
        units = "ms";
        t *= 1e3;
    } else if (t >= 1e-6) {
        units = "μs";
        t *= 1e6;
    } else if (t >= 1e-9) {
        units = "ns";
        t *= 1e9;
    }

    if (t >= 1e9)
        count += snprintf(dst, sz, "%.4g %s", t, units);
    else if (t >= 1e3)
        count += snprintf(dst, sz, "%.0f %s", t, units);
    else if (t >= 1e2)
        count += snprintf(dst, sz, "%.1f %s", t, units);
    else if (t >= 1e1)
        count += snprintf(dst, sz, "%.2f %s", t, units);
    else
        count += snprintf(dst, sz, "%.3f %s", t, units);

    return count;
}

int format_memory(char *dst, size_t sz, double t)
{
    int count = 0;
    // how memory can be negative? anyway..
    if (t < 0) {
        t = -t;
        count = snprintf(dst, sz, "-");
        dst += count;
        sz -= count;
    }

    const char *units = "B ";
    if (t >= (1lu << 30)) {
        units = "GB";
        t /= (1lu << 30);
    } else if (t >= (1lu << 20)) {
        units = "MB";
        t /= (1lu << 20);
    } else if (t >= (1lu << 10)) {
        units = "KB";
        t /= (1lu << 10);
    }

    if (t >= 1e9)
        count += snprintf(dst, sz, "%.4g %s", t, units);
    else if (t >= 1e3)
        count += snprintf(dst, sz, "%.0f %s", t, units);
    else if (t >= 1e2)
        count += snprintf(dst, sz, "%.1f %s", t, units);
    else if (t >= 1e1)
        count += snprintf(dst, sz, "%.2f %s", t, units);
    else
        count += snprintf(dst, sz, "%.3f %s", t, units);

    return count;
}

void format_meas(char *buf, size_t buf_size, double value, const struct units *units)
{
    switch (units->kind) {
    case MU_S:
        format_time(buf, buf_size, value);
        break;
    case MU_MS:
        format_time(buf, buf_size, value * 0.001);
        break;
    case MU_US:
        format_time(buf, buf_size, value * 0.000001);
        break;
    case MU_NS:
        format_time(buf, buf_size, value * 0.000000001);
        break;
    case MU_B:
        format_memory(buf, buf_size, value);
        break;
    case MU_KB:
        format_memory(buf, buf_size, value * (1lu << 10));
        break;
    case MU_MB:
        format_memory(buf, buf_size, value * (1lu << 20));
        break;
    case MU_GB:
        format_memory(buf, buf_size, value * (1lu << 30));
        break;
    case MU_CUSTOM:
        snprintf(buf, buf_size, "%.5g %s", value, units->str);
        break;
    case MU_NONE:
        snprintf(buf, buf_size, "%.3g", value);
        break;
    }
}

const char *outliers_variance_str(double fraction)
{
    if (fraction < 0.01)
        return "no";
    else if (fraction < 0.1)
        return "slight";
    else if (fraction < 0.5)
        return "moderate";
    return "severe";
}

const char *units_str(const struct units *units)
{
    switch (units->kind) {
    case MU_S:
        return "s";
    case MU_MS:
        return "ms";
    case MU_US:
        return "μs";
    case MU_NS:
        return "ns";
    case MU_B:
        return "B";
    case MU_KB:
        return "KB";
    case MU_MB:
        return "MB";
    case MU_GB:
        return "GB";
    case MU_CUSTOM:
        return units->str;
    case MU_NONE:
        return "";
    }
    return NULL;
}

const char *big_o_str(enum big_o complexity)
{
    switch (complexity) {
    case O_1:
        return "constant (O(1))";
    case O_N:
        return "linear (O(N))";
    case O_N_SQ:
        return "quadratic (O(N^2))";
    case O_N_CUBE:
        return "cubic (O(N^3))";
    case O_LOGN:
        return "logarithmic (O(log(N)))";
    case O_NLOGN:
        return "linearithmic (O(N*log(N)))";
    }
    return NULL;
}

bool process_wait_finished_correctly(pid_t pid, bool silent)
{
    int status = 0;
    pid_t wpid;
    for (;;) {
        if ((wpid = waitpid(pid, &status, 0)) != pid) {
            if (wpid == -1 && errno == EINTR)
                continue;
            if (wpid == -1)
                csperror("waitpid");
            return false;
        }
        break;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    if (!silent)
        error("process finished with non-zero exit code");
    return false;
}

bool check_and_handle_err_pipe(int read_end, int timeout)
{
    struct pollfd pfd;
    pfd.fd = read_end;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret;
    for (;;) {
        ret = poll(&pfd, 1, timeout);
        if (ret == -1 && errno == EINTR)
            continue;
        if (ret == -1) {
            csperror("poll");
            return false;
        }
        break;
    }
    if (ret == 1 && pfd.revents & POLLIN) {
        char buf[4096];
        ret = read(read_end, buf, sizeof(buf));
        if (ret == -1) {
            csperror("read");
            return false;
        }
        if (buf[0] != '\0') {
            error("child process failed to launch: %s", buf);
            return false;
        }
    }
    return true;
}

static bool shell_launch_internal(const char *cmd, int stdin_fd, int stdout_fd,
                                  int stderr_fd, int err_pipe[2], pid_t *pidp)
{
    char *argv[] = {"sh", "-c", NULL, NULL};
    argv[2] = (char *)cmd;

    pid_t pid = fork();
    if (pid == -1) {
        csperror("fork");
        return false;
    }
    if (pid == 0) {
        int fd = -1;
        if (stdin_fd == -1 || stdout_fd == -1 || stderr_fd == -1) {
            fd = open("/dev/null", O_RDWR);
            if (fd == -1) {
                csfdperror(err_pipe[1], "open(\"/dev/null\", O_RDWR)");
                _exit(-1);
            }
            if (stdin_fd == -1)
                stdin_fd = fd;
            if (stdout_fd == -1)
                stdout_fd = fd;
            if (stderr_fd == -1)
                stderr_fd = fd;
        }
        if (dup2(stdin_fd, STDIN_FILENO) == -1 || dup2(stdout_fd, STDOUT_FILENO) == -1 ||
            dup2(stderr_fd, STDERR_FILENO) == -1) {
            csfdperror(err_pipe[1], "dup2");
            _exit(-1);
        }
        if (fd != -1)
            close(fd);
        if (write(err_pipe[1], "", 1) < 0)
            _exit(-1);
        if (execv("/bin/sh", argv) == -1) {
            csfdperror(err_pipe[1], "execv");
            _exit(-1);
        }
        __builtin_unreachable();
    }
    *pidp = pid;
    return check_and_handle_err_pipe(err_pipe[0], -1);
}

bool shell_launch(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd, pid_t *pid)
{
    int err_pipe[2];
    if (!pipe_cloexec(err_pipe))
        return false;
    bool success = shell_launch_internal(cmd, stdin_fd, stdout_fd, stderr_fd, err_pipe, pid);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return success;
}

bool shell_launch_stdin_pipe(const char *cmd, FILE **in_pipe, int stdout_fd, int stderr_fd,
                             pid_t *pidp)
{
    int pipe_fds[2];
    if (!pipe_cloexec(pipe_fds))
        return false;

    bool success = shell_launch(cmd, pipe_fds[0], stdout_fd, stderr_fd, pidp);
    if (!success) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    close(pipe_fds[0]);
    FILE *f = fdopen(pipe_fds[1], "w");
    if (f == NULL) {
        csperror("fdopen");
        // Not a very nice way of handling errors, but it seems correct.
        close(pipe_fds[1]);
        kill(*pidp, SIGKILL);
        for (;;) {
            int err = waitpid(*pidp, NULL, 0);
            if (err == -1 && errno == EINTR)
                continue;
            break;
        }
        return false;
    }
    *in_pipe = f;
    return true;
}

bool shell_execute(const char *cmd, int stdin_fd, int stdout_fd, int stderr_fd, bool silent)
{
    pid_t pid = 0;
    bool success = shell_launch(cmd, stdin_fd, stdout_fd, stderr_fd, &pid);
    if (success)
        success = process_wait_finished_correctly(pid, silent);
    return success;
}

#if defined(__APPLE__)
double get_time(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}
#else
double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

__attribute__((format(printf, 2, 3))) FILE *open_file_fmt(const char *mode, const char *fmt,
                                                          ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return fopen(buf, mode);
}

int tmpfile_fd(void)
{
    char path[] = "/tmp/csbench_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        csperror("mkstemp");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        csfmtperror("fcntl on '%s'", path);
        return false;
    }
    unlink(path);
    return fd;
}

bool spawn_threads(void *(*worker_fn)(void *), void *param, size_t thread_count)
{
    bool success = false;
    pthread_t *thread_ids = calloc(thread_count, sizeof(*thread_ids));
    // Create worker threads that do running.
    for (size_t i = 0; i < thread_count; ++i) {
        // HACK: save thread id to output anchors first. If we do not do it
        // here we would need additional synchronization
        pthread_t *id = thread_ids + i;
        if (g_progress_bar && g_output_anchors)
            id = &g_output_anchors[i].id;
        if (pthread_create(id, NULL, worker_fn, param) != 0) {
            for (size_t j = 0; j < i; ++j)
                pthread_join(thread_ids[j], NULL);
            csfmtperror("failed to spawn thread");
            goto out;
        }
        thread_ids[i] = *id;
    }

    success = true;
    for (size_t i = 0; i < thread_count; ++i) {
        void *thread_retval;
        pthread_join(thread_ids[i], &thread_retval);
        if (thread_retval == (void *)-1)
            success = false;
    }

out:
    free(thread_ids);
    return success;
}

void cs_free_strings(void)
{
    for (struct string_ll *lc = string_ll; lc;) {
        free(lc->str);
        struct string_ll *next = lc->next;
        free(lc);
        lc = next;
    }
}

char *csstralloc(size_t len)
{
    struct string_ll *lc = calloc(1, sizeof(*lc));
    char *str = malloc(len + 1);
    lc->str = str;
    lc->next = string_ll;
    string_ll = lc;
    return str;
}

const char *csmkstr(const char *src, size_t len)
{
    char *str = csstralloc(len);
    memcpy(str, src, len);
    str[len] = '\0';
    return str;
}

const char *csstrdup(const char *str)
{
    return csmkstr(str, strlen(str));
}

const char *csfmt(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return csstrdup(buf);
}

void init_rng_state(void)
{
    pthread_t pt = pthread_self();
    if (sizeof(pt) >= sizeof(uint64_t)) {
        uint64_t entropy;
        memcpy(&entropy, &pt, sizeof(uint64_t));
        g_rng_state = time(NULL) * 2 + entropy;
    } else {
        g_rng_state = time(NULL) * 2 + 1;
    }
}

const char **parse_comma_separated_list(const char *str)
{
    const char **value_list = NULL;
    const char *cursor = str;
    const char *end = str + strlen(str);
    while (cursor != end) {
        const char *next = strchr(cursor, ',');
        if (next == NULL) {
            const char *new_str = csstrdup(cursor);
            sb_push(value_list, new_str);
            break;
        }
        size_t value_len = next - cursor;
        const char *value = csmkstr(cursor, value_len);
        sb_push(value_list, value);
        cursor = next + 1;
    }
    return value_list;
}

void fprintf_colored(FILE *f, const char *how, const char *fmt, ...)
{
    if (g_colored_output) {
        fprintf(f, "\x1b[%sm", how);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\x1b[0m");
    } else {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
    }
}

void strwriter_vprintf(struct string_writer *writer, const char *fmt, va_list args)
{
    int advance = vsnprintf(writer->cursor, writer->end - writer->cursor, fmt, args);
    writer->cursor += advance;
}

__attribute__((format(printf, 2, 3))) void strwriter_printf(struct string_writer *writer,
                                                            const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    strwriter_vprintf(writer, fmt, args);
    va_end(args);
}

__attribute__((format(printf, 3, 4))) void
strwriter_printf_colored(struct string_writer *writer, const char *how, const char *fmt, ...)
{
    if (g_colored_output) {
        strwriter_printf(writer, "\x1b[%sm", how);
        va_list args;
        va_start(args, fmt);
        strwriter_vprintf(writer, fmt, args);
        va_end(args);
        strwriter_printf(writer, "\x1b[0m");
    } else {
        va_list args;
        va_start(args, fmt);
        strwriter_vprintf(writer, fmt, args);
        va_end(args);
    }
}

void errorv(const char *fmt, va_list args)
{
    pthread_t tid = pthread_self();
    for (size_t i = 0; i < sb_len(g_output_anchors); ++i) {
        // Implicitly discard all messages but the first. This should not be an
        // issue, as the only possible message is error, and it (at least it
        // should) is always a single one
        if (pthread_equal(tid, g_output_anchors[i].id) &&
            !atomic_load(&g_output_anchors[i].has_message)) {
            vsnprintf(g_output_anchors[i].buffer, sizeof(g_output_anchors[i].buffer), fmt,
                      args);
            atomic_fence();
            atomic_store(&g_output_anchors[i].has_message, true);
            va_end(args);
            return;
        }
    }
    fprintf_colored(stderr, ANSI_RED, "error: ");
    vfprintf(stderr, fmt, args);
    putc('\n', stderr);
}

void error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    errorv(fmt, args);
    va_end(args);
}

char *csstrerror(char *buf, size_t buf_size, int err)
{
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, buf, buf_size);
#else
    strerror_r(err, buf, buf_size);
    err_msg = buf;
#endif
    return err_msg;
}

void csperror(const char *msg)
{
    int err = errno;
    char errbuf[4096];
    char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);
    error("%s: %s", msg, err_msg);
}

void csfmtperror(const char *fmt, ...)
{
    int err = errno;
    char errbuf[4096];
    char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    error("%s: %s", buf, err_msg);
}

void csfdperror(int fd, const char *msg)
{
    int err = errno;
    char errbuf[4096];
    const char *err_msg = csstrerror(errbuf, sizeof(errbuf), err);
    char buf[4096];
    int len = snprintf(buf, sizeof(buf), "%s: %s", msg, err_msg);
    if (write(fd, buf, len + 1) < 0)
        _exit(-1);
}

bool pipe_cloexec(int fd[2])
{
    if (pipe(fd) == -1) {
        csperror("pipe");
        return false;
    }
    if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(fd[1], F_SETFD, FD_CLOEXEC) == -1) {
        csperror("fcntl");
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    return true;
}

void cssort_ext(void *base, size_t nmemb, size_t size, cssort_compar_fn *compar, void *arg)
{
#ifdef __linux__
    qsort_r(base, nmemb, size, compar, arg);
#elif defined(__APPLE__)
    qsort_r(base, nmemb, size, arg, compar);
#else
#error
#endif
}

enum parse_time_str_result parse_time_str(const char *str, enum units_kind target_units,
                                          double *valuep)
{
    char *str_end;
    double value = strtod(str, &str_end);
    if (str_end == str)
        return PARSE_TIME_STR_ERR_FORMAT;

    if (value < 0.0)
        return PARSE_TIME_STR_ERR_NEG;

    const char *cursor = str_end;
    switch (*cursor) {
    case '\0':
        break;
    case 's':
        if (cursor[1] != '\0')
            return PARSE_TIME_STR_ERR_UNITS;
        break;
    case 'm':
        if (cursor[1] != 's' || cursor[2] != '\0')
            return PARSE_TIME_STR_ERR_UNITS;
        value *= 1e-3;
        break;
    case 'u':
        if (cursor[1] != 's' || cursor[2] != '\0')
            return PARSE_TIME_STR_ERR_UNITS;
        value *= 1e-6;
        break;
    case 'n':
        if (cursor[1] != 's' || cursor[2] != '\0')
            return PARSE_TIME_STR_ERR_UNITS;
        value *= 1e-9;
        break;
    default:
        return PARSE_TIME_STR_ERR_UNITS;
    }

    switch (target_units) {
    case MU_S:
        break;
    case MU_MS:
        value *= 1e3;
        break;
    case MU_US:
        value *= 1e6;
        break;
    case MU_NS:
        value *= 1e9;
        break;
    default:
        assert(0);
    }

    *valuep = value;
    return PARSE_TIME_STR_OK;
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag,
                     struct FTW *ftwbuf)
{
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    int rv = remove(fpath);
    if (rv)
        csfmtperror("failed to delete file '%s'", fpath);
    return rv;
}

bool rm_rf_dir(const char *name)
{
    struct stat st;
    if (stat(name, &st) != 0) {
        if (errno == ENOENT)
            return true;
        csfmtperror("failed to get information about file '%s'", name);
        return false;
    }

    int ret = nftw(name, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    if (ret != 0) {
        csfmtperror("failed to delete out directory '%s'", name);
        return false;
    }
    return true;
}

void parse_units_str(const char *str, struct units *units)
{
    if (strcmp(str, "s") == 0) {
        units->kind = MU_S;
    } else if (strcmp(str, "ms") == 0) {
        units->kind = MU_MS;
    } else if (strcmp(str, "us") == 0) {
        units->kind = MU_US;
    } else if (strcmp(str, "ns") == 0) {
        units->kind = MU_NS;
    } else if (strcmp(str, "b") == 0) {
        units->kind = MU_B;
    } else if (strcmp(str, "kb") == 0) {
        units->kind = MU_KB;
    } else if (strcmp(str, "mb") == 0) {
        units->kind = MU_MB;
    } else if (strcmp(str, "gb") == 0) {
        units->kind = MU_GB;
    } else if (strcmp(str, "none") == 0) {
        units->kind = MU_NONE;
    } else {
        units->kind = MU_CUSTOM;
        units->str = str;
    }
}

bool parse_meas_str(const char *str, enum meas_kind *kind)
{
    if (strcmp(str, "wall") == 0) {
        *kind = MEAS_WALL;
    } else if (strcmp(str, "stime") == 0) {
        *kind = MEAS_RUSAGE_STIME;
    } else if (strcmp(str, "utime") == 0) {
        *kind = MEAS_RUSAGE_UTIME;
    } else if (strcmp(str, "maxrss") == 0) {
        *kind = MEAS_RUSAGE_MAXRSS;
    } else if (strcmp(str, "minflt") == 0) {
        *kind = MEAS_RUSAGE_MINFLT;
    } else if (strcmp(str, "majflt") == 0) {
        *kind = MEAS_RUSAGE_MAJFLT;
    } else if (strcmp(str, "nvcsw") == 0) {
        *kind = MEAS_RUSAGE_NVCSW;
    } else if (strcmp(str, "nivcsw") == 0) {
        *kind = MEAS_RUSAGE_NIVCSW;
    } else if (strcmp(str, "cycles") == 0) {
        *kind = MEAS_PERF_CYCLES;
    } else if (strcmp(str, "instructions") == 0) {
        *kind = MEAS_PERF_INS;
    } else if (strcmp(str, "branches") == 0) {
        *kind = MEAS_PERF_BRANCH;
    } else if (strcmp(str, "branch-misses") == 0) {
        *kind = MEAS_PERF_BRANCHM;
    } else {
        return false;
    }
    return true;
}

bool get_term_win_size(size_t *rows, size_t *cols)
{
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        csperror("ioctl");
        return false;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return true;
}
