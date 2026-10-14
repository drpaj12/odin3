/*
 * child.c — the child runner of the subprocess path (3D spec §7.9, AB-33; plan Task 9a).
 *
 * One run is four steps, each its own function group:
 *   start:  the request's limits, the minimal environment, the pipe and posix_spawn into a new
 *           process group with stdin on /dev/null, stdout and stderr on the pipe, no core file;
 *   pump:   poll the pipe in short slices, split what arrives into lines for the text callback,
 *           count the bytes against the cap, and watch for the child's exit and the deadline;
 *   reap:   SIGKILL the whole group on every path (so a grandchild cannot outlive the call), then
 *           waitpid the child and drain what is still in the pipe;
 *   result: classify the end (OK, EXIT, SIGNAL, TIMEOUT, OUTPUT) from the wait status and the
 *           pump's verdict.
 * No recursion; the one heap block (buffers and the environment) is allocated before anything is
 * spawned, so an out-of-memory failure never leaves a process behind. CMake compiles this file with
 * _DEFAULT_SOURCE for posix_spawn_file_actions_addchdir_np and _addclosefrom_np.
 */
#include "passes/abc/child.h"

#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/script.h"
#include "util/alloc.h"
#include "util/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum {
    READ_BYTES = 65536, /* one read from the pipe */
    LINE_BYTES = 65536, /* the longest line forwarded whole; longer lines go in pieces */
    ENV_BYTES = 4096,   /* storage of the four environment strings */
    ENV_SLOTS = 5,      /* PATH, HOME, TMPDIR, LC_ALL and the terminator */
    SLICE_MS = 10,      /* the longest wait between two looks at the child */
    MILLIS_PER_SECOND = 1000,
    MIN_FD = 3, /* the pipe lives above stdin, stdout and stderr */
    FIRST_HIGH_FD = 3
};

/* The test hook's limits (zeros: none). */
static odin3_abc_child_limits g_hook;

void odin3_abc_test_set_child_limits(const odin3_abc_child_limits *limits) {
    g_hook = limits != NULL ? *limits : (odin3_abc_child_limits){0, 0};
}

/* How the pump ended. */
typedef enum pump_end { PUMP_EXITED, PUMP_TIMEOUT, PUMP_OVER_CAP } pump_end;

/* Everything one run needs; one heap block, zeroed. */
typedef struct run_state {
    const odin3_abc_child_req *req;
    odin3_abc_child_res *res;
    pid_t pid;
    int fd;          /* the pipe's read end, or -1 once at EOF */
    double deadline; /* odin3_now_ms() at which the wall clock runs out */
    size_t line_len;
    char *envp[ENV_SLOTS];
    char env_store[ENV_BYTES];
    char read_buf[READ_BYTES];
    char line[LINE_BYTES + 1];
} run_state;

/* --- limits and request checks -------------------------------------------------------------- */

static bool text_ok(const char *text) {
    return text != NULL && text[0] != '\0';
}

static bool request_ok(const odin3_abc_child_req *req) {
    return req != NULL && text_ok(req->program) && req->program[0] == '/' && text_ok(req->option) &&
           text_ok(req->argument) && text_ok(req->run_dir);
}

/* Fills the result's header: the run number, the effective limits and the program path. */
static void init_result(const odin3_abc_child_req *req, odin3_abc_child_res *res) {
    memset(res, 0, sizeof *res);
    res->kind = ODIN3_ABC_CHILD_OK;
    res->run = req->run;
    res->seconds = req->seconds != 0 ? req->seconds : ODIN3_ABC_CHILD_SECONDS;
    res->output_limit =
        req->output_bytes != 0 ? req->output_bytes : (uint64_t)ODIN3_ABC_CHILD_OUTPUT_BYTES;
    if (g_hook.seconds != 0 && g_hook.seconds < res->seconds) {
        res->seconds = g_hook.seconds;
    }
    if (g_hook.output_bytes != 0 && g_hook.output_bytes < res->output_limit) {
        res->output_limit = g_hook.output_bytes;
    }
    (void)snprintf(res->program, sizeof res->program, "%s", req->program);
}

/* --- start: environment, pipe, spawn -------------------------------------------------------- */

/* One environment variable of the child: taken from this process, else the fallback. */
typedef struct env_var {
    const char *name;
    const char *fallback;
} env_var;

static const env_var k_env[] = {
    {"PATH", "/usr/bin:/bin"}, {"HOME", "/"}, {"TMPDIR", "/tmp"}, {"LC_ALL", NULL}};

/* The value of v: LC_ALL is always "C"; the others come from this process unless unset, empty or
 * longer than a quarter of the store. */
static const char *env_value(const env_var *var) {
    if (var->fallback == NULL) {
        return "C";
    }
    const char *value = getenv(var->name);
    bool usable = value != NULL && value[0] != '\0' && strlen(value) < ENV_BYTES / ENV_SLOTS;
    return usable ? value : var->fallback;
}

/* The child's environment: PATH, HOME and TMPDIR from this process (or defaults) and LC_ALL=C. */
static void build_env(run_state *st) {
    size_t used = 0;
    size_t count = 0;
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++) {
        size_t room = sizeof st->env_store - used;
        char *slot = st->env_store + used;
        int wrote = snprintf(slot, room, "%s=%s", k_env[i].name, env_value(&k_env[i]));
        if (wrote < 0 || (size_t)wrote >= room) {
            break;
        }
        used += (size_t)wrote + 1;
        st->envp[count++] = slot;
    }
    st->envp[count] = NULL;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* A pipe whose ends are close-on-exec and at or above MIN_FD, so the spawn's dup2 onto 1 and 2
 * never meets an end that already is 1 or 2. */
static int make_pipe(int fds[2]) {
    if (pipe(fds) != 0) {
        return errno;
    }
    for (int i = 0; i < 2; i++) {
        if (fds[i] < MIN_FD) {
            int moved = fcntl(fds[i], F_DUPFD_CLOEXEC, FIRST_HIGH_FD);
            if (moved < 0) {
                int err = errno;
                (void)close(fds[0]);
                (void)close(fds[1]);
                return err;
            }
            (void)close(fds[i]);
            fds[i] = moved;
        } else if (set_cloexec(fds[i]) != 0) {
            int err = errno;
            (void)close(fds[0]);
            (void)close(fds[1]);
            return err;
        }
    }
    return 0;
}

/* The file actions: stdin from /dev/null, stdout and stderr to the pipe, the run directory as the
 * working directory, every other descriptor closed. Returns 0 or an errno. */
static int add_file_actions(posix_spawn_file_actions_t *actions, int write_fd, const char *dir) {
    int err = posix_spawn_file_actions_addopen(actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (err == 0) {
        err = posix_spawn_file_actions_adddup2(actions, write_fd, STDOUT_FILENO);
    }
    if (err == 0) {
        err = posix_spawn_file_actions_adddup2(actions, write_fd, STDERR_FILENO);
    }
    if (err == 0) {
        err = posix_spawn_file_actions_addchdir_np(actions, dir);
    }
    if (err == 0) {
        err = posix_spawn_file_actions_addclosefrom_np(actions, MIN_FD);
    }
    return err;
}

/* The attributes: a new process group (the child's pid), an empty signal mask and the default
 * action for the signals a host may have changed. Returns 0 or an errno. */
static int set_attributes(posix_spawnattr_t *attr) {
    sigset_t mask;
    sigset_t defaults;
    (void)sigemptyset(&mask);
    (void)sigemptyset(&defaults);
    (void)sigaddset(&defaults, SIGPIPE);
    (void)sigaddset(&defaults, SIGINT);
    (void)sigaddset(&defaults, SIGTERM);
    (void)sigaddset(&defaults, SIGHUP);
    (void)sigaddset(&defaults, SIGQUIT);
    int err = posix_spawnattr_setflags(
        attr, (short)(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    if (err == 0) {
        err = posix_spawnattr_setpgroup(attr, 0);
    }
    if (err == 0) {
        err = posix_spawnattr_setsigmask(attr, &mask);
    }
    if (err == 0) {
        err = posix_spawnattr_setsigdefault(attr, &defaults);
    }
    return err;
}

/* The posix_spawn call proper, with this process's core limit at 0 for its duration (the child
 * inherits it) and restored after. Returns 0 or an errno. */
static int spawn_child(run_state *st, int write_fd) {
    const odin3_abc_child_req *req = st->req;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int err = posix_spawn_file_actions_init(&actions);
    if (err != 0) {
        return err;
    }
    err = posix_spawnattr_init(&attr);
    if (err != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return err;
    }
    err = add_file_actions(&actions, write_fd, req->run_dir);
    if (err == 0) {
        err = set_attributes(&attr);
    }
    if (err == 0) {
        char *const argv[] = {(char *)req->program, (char *)req->option, (char *)req->argument,
                              NULL};
        struct rlimit saved;
        struct rlimit none = {0, 0};
        bool have = getrlimit(RLIMIT_CORE, &saved) == 0;
        if (have) {
            none.rlim_max = saved.rlim_max;
            (void)setrlimit(RLIMIT_CORE, &none);
        }
        err = posix_spawn(&st->pid, req->program, &actions, &attr, argv, st->envp);
        if (have) {
            (void)setrlimit(RLIMIT_CORE, &saved);
        }
    }
    (void)posix_spawnattr_destroy(&attr);
    (void)posix_spawn_file_actions_destroy(&actions);
    return err;
}

/* Starts the child. On success st->pid and st->fd (the read end, non-blocking) are set; on a
 * failure the result says SPAWN with the errno and nothing is left open. */
static bool start_child(run_state *st) {
    int fds[2];
    int err = make_pipe(fds);
    if (err == 0) {
        err = spawn_child(st, fds[1]);
        (void)close(fds[1]);
        if (err == 0) {
            err = fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK) == 0 ? 0 : errno;
        }
        if (err != 0) {
            (void)close(fds[0]);
        }
    }
    if (err != 0) {
        st->res->kind = ODIN3_ABC_CHILD_SPAWN;
        st->res->code = err;
        return false;
    }
    st->fd = fds[0];
    return true;
}

/* --- pump: lines, the cap, the exit and the deadline ---------------------------------------- */

static void emit_line(run_state *st) {
    st->line[st->line_len] = '\0';
    if (st->req->text != NULL) {
        st->req->text(st->req->user, st->line);
    }
    st->line_len = 0;
}

/* Splits n bytes of the buffer into lines: a line end completes one, a full line buffer cuts one.
 */
static void feed_lines(run_state *st, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = st->read_buf[i];
        if (ch == '\n') {
            emit_line(st);
            continue;
        }
        st->line[st->line_len++] = ch;
        if (st->line_len == LINE_BYTES) {
            emit_line(st);
        }
    }
}

/* One read, of at most cap + 1 - seen bytes so the overflow is seen at the first byte past the
 * cap. Returns 1 for data (forwarded), 0 for nothing now (EAGAIN), -1 at EOF or on an error. */
static int read_some(run_state *st) {
    uint64_t room = st->res->output_limit + 1 - st->res->output_bytes;
    size_t want = room < READ_BYTES ? (size_t)room : (size_t)READ_BYTES;
    ssize_t got = read(st->fd, st->read_buf, want);
    if (got > 0) {
        st->res->output_bytes += (uint64_t)got;
        feed_lines(st, (size_t)got);
        return 1;
    }
    if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    return -1;
}

/* True once the child has exited (left as a zombie so its pid stays reserved). */
static bool child_exited(const run_state *st) {
    siginfo_t info;
    memset(&info, 0, sizeof info);
    if (waitid(P_PID, (id_t)st->pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
        return true;
    }
    return info.si_pid != 0;
}

static bool over_cap(const run_state *st) {
    return st->res->output_bytes > st->res->output_limit;
}

/* Waits up to one slice for the pipe and reads what is there. Returns false when the cap is
 * passed. */
static bool pump_once(run_state *st, double remaining_ms) {
    struct pollfd pfd = {st->fd, POLLIN, 0};
    int wait_ms = remaining_ms < SLICE_MS ? (int)remaining_ms + 1 : SLICE_MS;
    int ready = poll(&pfd, 1, wait_ms);
    if (ready > 0 && st->fd >= 0) {
        int got = 1;
        while (got == 1 && !over_cap(st)) {
            got = read_some(st);
        }
        if (got < 0) {
            (void)close(st->fd); /* EOF: poll now only sleeps for the slice */
            st->fd = -1;
        }
    }
    return !over_cap(st);
}

/* Reads until the child has exited, the cap or the deadline passes. */
static pump_end pump(run_state *st) {
    for (;;) {
        double remaining = st->deadline - odin3_now_ms();
        if (remaining <= 0) {
            return PUMP_TIMEOUT;
        }
        if (!pump_once(st, remaining)) {
            return PUMP_OVER_CAP;
        }
        if (child_exited(st)) {
            return PUMP_EXITED;
        }
    }
}

/* --- reap: kill the group, wait, drain ------------------------------------------------------ */

/* Reads what the dead group left in the pipe; false when that passes the cap. */
static bool drain(run_state *st) {
    int got = 1;
    while (st->fd >= 0 && got == 1 && !over_cap(st)) {
        got = read_some(st);
    }
    if (st->line_len > 0) {
        emit_line(st);
    }
    return !over_cap(st);
}

/* Kills the process group and waits for the child; returns its wait status. */
static int reap(run_state *st) {
    (void)kill(-st->pid, SIGKILL);
    int status = 0;
    pid_t got;
    do {
        got = waitpid(st->pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? 0 : status;
}

/* --- result --------------------------------------------------------------------------------- */

static odin3_status classify(odin3_abc_child_res *res, int status) {
    if (WIFEXITED(status)) {
        res->code = WEXITSTATUS(status);
        res->kind = res->code == 0 ? ODIN3_ABC_CHILD_OK : ODIN3_ABC_CHILD_EXIT;
    } else {
        res->code = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        res->kind = ODIN3_ABC_CHILD_SIGNAL;
    }
    return res->kind == ODIN3_ABC_CHILD_OK ? ODIN3_OK : ODIN3_ERR_ENGINE;
}

/* Runs a started child to its end and fills the result. */
static odin3_status finish(run_state *st, pump_end end) {
    int status = reap(st);
    bool capped = end == PUMP_OVER_CAP;
    if (!capped) {
        capped = !drain(st);
    }
    if (st->fd >= 0) {
        (void)close(st->fd);
        st->fd = -1;
    }
    if (capped) {
        st->res->kind = ODIN3_ABC_CHILD_OUTPUT;
        return ODIN3_ERR_ENGINE;
    }
    if (end == PUMP_TIMEOUT) {
        st->res->kind = ODIN3_ABC_CHILD_TIMEOUT;
        return ODIN3_ERR_ENGINE;
    }
    return classify(st->res, status);
}

odin3_status odin3_abc_child_run(const odin3_abc_child_req *req, odin3_abc_child_res *res) {
    if (!request_ok(req) || res == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    init_result(req, res);
    run_state *st = odin3_util_calloc(sizeof *st);
    if (st == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    st->req = req;
    st->res = res;
    st->fd = -1;
    build_env(st);
    odin3_status status = ODIN3_ERR_ENGINE;
    double start = odin3_now_ms();
    if (start_child(st)) {
        st->deadline = start + (double)res->seconds * MILLIS_PER_SECOND;
        status = finish(st, pump(st));
    }
    odin3_util_free(st);
    return status;
}

/* --- the route (§7.9) ----------------------------------------------------------------------- */

static odin3_abc_test_route g_route_mode = ODIN3_ABC_TEST_ROUTE_AUTO;

void odin3_abc_test_set_route(odin3_abc_test_route mode) {
    g_route_mode = mode;
}

odin3_abc_route_kind odin3_abc_route(const unsigned char sha256[ODIN3_SHA256_DIGEST_BYTES]) {
    if (g_route_mode == ODIN3_ABC_TEST_ROUTE_ALWAYS_CHILD) {
        return ODIN3_ABC_ROUTE_CHILD;
    }
    if (g_route_mode == ODIN3_ABC_TEST_ROUTE_ALWAYS_LINKED) {
        return ODIN3_ABC_ROUTE_LINKED;
    }
    uint8_t embedded[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_default_sha256(embedded);
    return memcmp(embedded, sha256, sizeof embedded) == 0 ? ODIN3_ABC_ROUTE_LINKED
                                                          : ODIN3_ABC_ROUTE_CHILD;
}

/* --- the program and its pin ---------------------------------------------------------------- */

#ifndef ODIN3_ABC_COMMIT_STR
#define ODIN3_ABC_COMMIT_STR "0000000000000000000000000000000000000000" /* a build without ABC */
#endif

enum { COMMIT_HEX = 40 };

static const char SIBLING_NAME[] = "odin3-abc";
static const char PIN_PREFIX[] = "odin3-abc: commit ";

const char *odin3_abc_child_pin(void) {
    return ODIN3_ABC_COMMIT_STR;
}

/* path := the program named by the environment, made absolute. */
static odin3_status program_from_env(const char *value, char *path, size_t cap) {
    int wrote;
    if (value[0] == '/') {
        wrote = snprintf(path, cap, "%s", value);
    } else {
        char cwd[ODIN3_ABC_CHILD_PATH_BYTES];
        if (getcwd(cwd, sizeof cwd) == NULL) {
            return ODIN3_ERR_IO;
        }
        wrote = snprintf(path, cap, "%s/%s", cwd, value);
    }
    return (wrote > 0 && (size_t)wrote < cap) ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

/* path := odin3-abc beside the running executable. */
static odin3_status program_beside_self(char *path, size_t cap) {
    ssize_t len = readlink("/proc/self/exe", path, cap - 1);
    if (len < 0) {
        return ODIN3_ERR_IO;
    }
    path[len] = '\0';
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return ODIN3_ERR_IO;
    }
    size_t room = cap - (size_t)(slash - path);
    int wrote = snprintf(slash, room, "/%s", SIBLING_NAME);
    return (wrote > 0 && (size_t)wrote < room) ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

odin3_status odin3_abc_child_program(char *path, size_t cap) {
    if (path == NULL || cap < 2) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *value = getenv("ODIN3_ABC_EXE");
    if (value != NULL && value[0] != '\0') {
        return program_from_env(value, path, cap);
    }
    return program_beside_self(path, cap);
}

/* What the pin query's output holds so far. */
typedef struct pin_scan {
    char commit[COMMIT_HEX + 1];
    bool seen;
} pin_scan;

static void scan_pin_line(void *user, const char *line) {
    pin_scan *scan = user;
    size_t prefix = sizeof PIN_PREFIX - 1;
    if (!scan->seen && strncmp(line, PIN_PREFIX, prefix) == 0) {
        (void)snprintf(scan->commit, sizeof scan->commit, "%s", line + prefix);
        scan->seen = true;
    }
}

/* The last program that passed (once per process, spec §7.9). */
static char g_pinned_program[ODIN3_ABC_CHILD_PATH_BYTES];

static odin3_status pin_mismatch(const pin_scan *scan, odin3_abc_child_res *res) {
    res->kind = ODIN3_ABC_CHILD_PIN;
    res->code = 0;
    (void)snprintf(res->found, sizeof res->found, "%.*s", ODIN3_ABC_CHILD_HEX,
                   scan->seen && scan->commit[0] != '\0' ? scan->commit : "unknown");
    return ODIN3_ERR_ENGINE;
}

odin3_status odin3_abc_child_check_pin(const char *path, odin3_abc_child_res *res) {
    if (!text_ok(path) || path[0] != '/' || res == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (strcmp(path, g_pinned_program) == 0) {
        uint32_t run = res->run;
        memset(res, 0, sizeof *res);
        res->run = run;
        (void)snprintf(res->program, sizeof res->program, "%s", path);
        return ODIN3_OK;
    }
    pin_scan scan = {{0}, false};
    odin3_abc_child_req req = {0};
    req.program = path;
    req.option = "-c";
    req.argument = "version";
    req.run_dir = "/";
    req.run = res->run;
    req.text = scan_pin_line;
    req.user = &scan;
    odin3_status status = odin3_abc_child_run(&req, res);
    if (status != ODIN3_OK) {
        return status;
    }
    if (!scan.seen || strcmp(scan.commit, ODIN3_ABC_COMMIT_STR) != 0) {
        return pin_mismatch(&scan, res);
    }
    (void)snprintf(g_pinned_program, sizeof g_pinned_program, "%s", path);
    return ODIN3_OK;
}

/* --- the six texts -------------------------------------------------------------------------- */

static void render_message(const odin3_abc_child_res *res, char *out, size_t cap) {
    switch (res->kind) {
    case ODIN3_ABC_CHILD_SPAWN: {
        char reason[ODIN3_ABC_CHILD_PATH_BYTES];
        if (strerror_r(res->code, reason, sizeof reason) != 0) {
            (void)snprintf(reason, sizeof reason, "error %d", res->code);
        }
        (void)snprintf(out, cap, "abc: ABC subprocess '%s' cannot start: %s", res->program, reason);
        break;
    }
    case ODIN3_ABC_CHILD_PIN:
        (void)snprintf(out, cap, "abc: ABC subprocess '%s' is %s, expected %.*s", res->program,
                       res->found, ODIN3_ABC_CHILD_HEX, ODIN3_ABC_COMMIT_STR);
        break;
    case ODIN3_ABC_CHILD_EXIT:
        (void)snprintf(out, cap, "abc: ABC subprocess exited with status %d during run %u",
                       res->code, (unsigned)res->run);
        break;
    case ODIN3_ABC_CHILD_SIGNAL:
        (void)snprintf(out, cap, "abc: ABC subprocess killed by signal %d during run %u", res->code,
                       (unsigned)res->run);
        break;
    case ODIN3_ABC_CHILD_TIMEOUT:
        (void)snprintf(out, cap, "abc: ABC subprocess exceeded %u s wall clock during run %u",
                       (unsigned)res->seconds, (unsigned)res->run);
        break;
    default: /* ODIN3_ABC_CHILD_OUTPUT */
        (void)snprintf(out, cap, "abc: ABC subprocess wrote more than %llu bytes during run %u",
                       (unsigned long long)res->output_limit, (unsigned)res->run);
        break;
    }
}

odin3_status odin3_abc_child_message(const odin3_abc_child_res *res, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (res == NULL || res->kind == ODIN3_ABC_CHILD_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    render_message(res, out, cap);
    return ODIN3_OK;
}
