/*
 * test_abc_adapter.c — the linked engine on the real ABC (3D spec §13.2, §13.6, §9, AB-6, AB-8,
 * AB-16; plan Tasks 5 and 6, point (a)): sessions, the fd capture, a failing command, the refused
 * process control, the plugin entry, the escapes and the adapter assertion; the exchange calls
 * (load_aig, get_luts, name_map) and their checks, the adapter's out-of-memory paths, no threads
 * and one-process determinism on a 40-AND network, and ABC's command splitting.
 *
 * Built from the adapter's sources with the test hooks (ODIN3_ABC_TEST_HOOKS) and linked with the
 * adapter's --wrap set, so the exit() and __assert_fail calls of the ABC commands this file adds
 * (o3test_exit, o3test_assert, o3test_thread_exit) reach the guard exactly as ABC's own would.
 * Each escape, and the adapter assertion, runs in a child process (this program with --escape
 * <case>, ASan's detect_leaks=0): each disables the engine for its process and leaks the session's
 * frame, as the engine contract says. The parent process leaks nothing.
 */
#include "odin3/odin3.h"

#include "bridge.h"
#include "guard.h"
#include "odin3_abc.h"
#include "session.h"
#include "sim/prng.h"
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* glibc's assertion entry, by its symbol: the test link wraps it like ABC's (point (w)). */
_Noreturn void o3test_assert_fail(const char *expr, const char *file, unsigned int line,
                                  const char *func) __asm__("__assert_fail");

enum {
    MAX_LINES = 64,
    LINE_CAP = 256,
    OUT_CAP = 8192,
    SESSIONS = 100,
    EXIT_CODE = 3,
    UNDERSCORE_EXIT_CODE = 4,
    THREAD_EXIT_CODE = 5,
    ASSERT_LINE = 7,
    DECIMAL = 10,
    REFUSED_CALLS = 8,
    GENERATORS_EACH = 4,   /* AB-4's 32-bit and 64-bit generators */
    LIBC_SEED = 1,         /* bridge.c's srand seed */
    DESCRIPTOR_LIMIT = 256 /* the descriptor cap of the no-descriptor case */
};

static const char MARKER[] = "o3test-marker: fd 1 at the child's end";

/* --- helpers -------------------------------------------------------------------------------- */

typedef struct lines {
    char text[MAX_LINES][LINE_CAP];
    size_t count;
} lines;

/* The text callback: keeps every line (cut to LINE_CAP - 1 bytes). */
static void collect(void *user, const char *line) {
    lines *got = user;
    if (got->count == MAX_LINES) {
        return;
    }
    size_t len = strlen(line);
    if (len >= LINE_CAP) {
        len = LINE_CAP - 1;
    }
    memcpy(got->text[got->count], line, len);
    got->text[got->count][len] = '\0';
    got->count++;
}

static bool has_line(const lines *got, const char *line) {
    for (size_t i = 0; i < got->count; i++) {
        if (strcmp(got->text[i], line) == 0) {
            return true;
        }
    }
    return false;
}

static void *self(void) {
    return odin3_abc_session_self();
}

static char log_line[LINE_CAP];

static void log_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t len = strlen(msg);
    if (len >= LINE_CAP) {
        len = LINE_CAP - 1;
    }
    memcpy(log_line, msg, len);
    log_line[len] = '\0';
}

void setUp(void) {
}

void tearDown(void) {
}

/* --- sessions and capture --------------------------------------------------------------------- */

static void test_sessions_100(void) {
    odin3_abc_fail fail;
    for (int i = 0; i < SESSIONS; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
        odin3_abc_session_close(self());
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    odin3_abc_fail nested;
    odin3_status st = odin3_abc_session_open(self(), NULL, NULL, &nested);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_BUSY, nested.kind);
    TEST_ASSERT_FALSE(odin3_guard_disabled());
    /* The nested open left the open session alone: a new one opens. */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    odin3_abc_session_close(self());
}

/* What the test's own pipes on fds 1 and 2 saw: during the call, and after it. */
typedef struct pipe_view {
    ssize_t during_out, during_err;
    ssize_t after_out, after_err;
    char out_byte, err_byte;
} pipe_view;

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* The test's fds 1 and 2 put on fresh non-blocking pipes, and the originals kept. */
typedef struct pipes {
    int out[2], err[2];
    int keep_out, keep_err;
} pipes;

static void pipes_install(pipes *fds) {
    bool ok = pipe(fds->out) == 0 && pipe(fds->err) == 0;
    ok = ok && set_nonblocking(fds->out[0]) && set_nonblocking(fds->err[0]);
    (void)fflush(stdout);
    (void)fflush(stderr);
    fds->keep_out = dup(STDOUT_FILENO);
    fds->keep_err = dup(STDERR_FILENO);
    ok = ok && fds->keep_out >= 0 && fds->keep_err >= 0;
    ok = ok && dup2(fds->out[1], STDOUT_FILENO) >= 0 && dup2(fds->err[1], STDERR_FILENO) >= 0;
    TEST_ASSERT_TRUE(ok);
}

static void pipes_remove(pipes *fds) {
    (void)dup2(fds->keep_out, STDOUT_FILENO);
    (void)dup2(fds->keep_err, STDERR_FILENO);
    int all[] = {fds->keep_out, fds->keep_err, fds->out[0], fds->out[1], fds->err[0], fds->err[1]};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        (void)close(all[i]);
    }
}

/* Runs command with fds 1 and 2 on fresh pipes, reads them during and after, restores them. */
static odin3_status exec_on_pipes(const char *command, odin3_abc_fail *fail, pipe_view *view) {
    pipes fds;
    pipes_install(&fds);
    odin3_status st = odin3_abc_session_exec(self(), command, fail);
    char byte = 0;
    view->during_out = read(fds.out[0], &byte, 1);
    view->during_err = read(fds.err[0], &byte, 1);
    ssize_t wrote_out = write(STDOUT_FILENO, "x", 1);
    ssize_t wrote_err = write(STDERR_FILENO, "y", 1);
    view->after_out = wrote_out == 1 ? read(fds.out[0], &view->out_byte, 1) : -1;
    view->after_err = wrote_err == 1 ? read(fds.err[0], &view->err_byte, 1) : -1;
    pipes_remove(&fds);
    return st;
}

static void test_capture_echo(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    pipe_view view = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status st = exec_on_pipes("echo hello", &fail, &view);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("hello", got.text[0]);
    /* Nothing reached the test's pipes during the call; after it they are fds 1 and 2 again. */
    TEST_ASSERT_TRUE(view.during_out < 0);
    TEST_ASSERT_TRUE(view.during_err < 0);
    TEST_ASSERT_EQUAL_INT(1, (int)view.after_out);
    TEST_ASSERT_EQUAL_INT(1, (int)view.after_err);
    TEST_ASSERT_EQUAL_CHAR('x', view.out_byte);
    TEST_ASSERT_EQUAL_CHAR('y', view.err_byte);
}

/* Output still in stdout's buffer before a call belongs to the original fd 1, not to ABC's lines:
 * capture_begin flushes it there before it moves fd 1. */
static void test_capture_flushes_pending_output(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    char seen[LINE_CAP] = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    pipes fds;
    pipes_install(&fds);
    (void)fputs("o3test-pending", stdout); /* no newline: still buffered, whatever the mode */
    odin3_status st = odin3_abc_session_exec(self(), "echo hello", &fail);
    ssize_t len = read(fds.out[0], seen, sizeof seen - 1);
    pipes_remove(&fds);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("o3test-pending", seen);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("hello", got.text[0]);
}

static void test_capture_partial_line_waits(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status first = odin3_abc_session_exec(self(), "echo -n partial", &fail);
    size_t after_first = got.count;
    odin3_status second = odin3_abc_session_exec(self(), "echo end", &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, second);
    TEST_ASSERT_EQUAL_size_t(0, after_first);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("partial end", got.text[0]);
}

static void test_exec_failure(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    odin3_status st = odin3_abc_session_exec(self(), "strash", &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_COMMAND, fail.kind);
    TEST_ASSERT_NOT_EQUAL_INT(0, fail.code);
    TEST_ASSERT_TRUE(has_line(&got, "Error: Empty network."));
    TEST_ASSERT_FALSE(odin3_guard_disabled());
}

/* --- process control -------------------------------------------------------------------------- */

/* Calls every refused wrapper once; *refused counts those that returned their refusal and EPERM. */
static odin3_status call_refused(void *arg) {
    int *refused = arg;
    static char true_path[] = "/bin/true";
    char *const argv[] = {true_path, NULL};
    errno = 0;
    *refused += odin3_wrap_system("true") == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_popen("true", "r") == NULL && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_fork() == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execv(true_path, argv) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execvp("true", argv) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execl(true_path, true_path, (char *)NULL) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_signal(SIGINT, SIG_IGN) == SIG_ERR && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_dlopen("libabc_o3test.so", 0) == NULL && errno == EPERM;
    return ODIN3_OK;
}

static void test_refused_calls(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    int refused = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status st = odin3_guard_run(call_refused, &refused, &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_INT(REFUSED_CALLS, refused);
    static const char *const expected[REFUSED_CALLS] = {
        "ABC tried to system; refused", "ABC tried to popen; refused",
        "ABC tried to fork; refused",   "ABC tried to execv; refused",
        "ABC tried to execvp; refused", "ABC tried to execl; refused",
        "ABC tried to signal; refused", "ABC tried to dlopen; refused"};
    TEST_ASSERT_EQUAL_size_t(REFUSED_CALLS, got.count);
    for (size_t i = 0; i < REFUSED_CALLS; i++) {
        TEST_ASSERT_EQUAL_STRING(expected[i], got.text[i]);
    }
    TEST_ASSERT_FALSE(odin3_guard_disabled());
}

/* --- the exports ------------------------------------------------------------------------------ */

static void test_plugin_init(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ABI_MISMATCH, odin3_plugin_init(ODIN3_ABI_VERSION + 1));
    /* The registry validates the table: version, every function, the 40-digit commit. */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_plugin_init(ODIN3_ABI_VERSION));
    log_line[0] = '\0';
    odin3_log_set_sink(log_sink, NULL);
    odin3_status again = odin3_abc_adapter_init();
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, again);
    TEST_ASSERT_EQUAL_STRING("odin3_abc_engine_register: abc engine already registered (linked)",
                             log_line);
}

/* --- the generators (AB-8) ------------------------------------------------------------------- */

/* AB-4's generators by symbol (this test links libabc-pic): every open resets them. */
extern unsigned Abc_Random(int reset);
extern uint64_t Abc_RandomW(int reset);
extern unsigned Aig_ManRandom(int reset);
extern uint64_t Aig_ManRandom64(int reset);
extern unsigned Gia_ManRandom(int reset);
extern uint64_t Gia_ManRandomW(int reset);
extern unsigned Random_Int(int reset);
extern uint64_t Random_Word(int reset);

/* The next value of every generator, libc's rand() included. */
typedef struct draw {
    unsigned u32[GENERATORS_EACH];
    uint64_t u64[GENERATORS_EACH];
    int libc;
} draw;

/* rand() through a pointer: cert-msc30-c refuses direct rand() calls (weak randomness), while
 * here the repeatable sequence after srand(1) is what is tested (AB-8), as in bridge.c. */
static int (*const libc_rand)(void) = rand;

/* bridge.c's resets, in its order; the seed is a parameter for the same reason as there. */
static void reset_as_open(unsigned int seed) {
    (void)Abc_Random(1);
    (void)Abc_RandomW(1);
    (void)Aig_ManRandom(1);
    (void)Aig_ManRandom64(1);
    (void)Gia_ManRandom(1);
    (void)Gia_ManRandomW(1);
    (void)Random_Int(1);
    (void)Random_Word(1);
    srand(seed);
}

static void draw_next(draw *next) {
    next->u32[0] = Abc_Random(0);
    next->u64[0] = Abc_RandomW(0);
    next->u32[1] = Aig_ManRandom(0);
    next->u64[1] = Aig_ManRandom64(0);
    next->u32[2] = Gia_ManRandom(0);
    next->u64[2] = Gia_ManRandomW(0);
    next->u32[3] = Random_Int(0);
    next->u64[3] = Random_Word(0);
    next->libc = libc_rand();
}

static void test_open_resets_generators(void) {
    draw expected;
    draw skipped;
    draw after_open;
    odin3_abc_fail fail;
    reset_as_open(LIBC_SEED);
    draw_next(&expected);
    draw_next(&skipped); /* every generator moves past its first value */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    draw_next(&after_open);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected.u32, after_open.u32, GENERATORS_EACH);
    TEST_ASSERT_EQUAL_MEMORY(expected.u64, after_open.u64, sizeof expected.u64);
    TEST_ASSERT_EQUAL_INT(expected.libc, after_open.libc);
}

/* --- the exchange: load_aig, get_luts, name_map (§4.4 steps 3, 5 and 6; AB-16) ---------------- */

enum {
    AIG_NULL = 0x7FFFFFFF, /* MiniAIG's empty slot: a PI is (AIG_NULL, AIG_NULL) */
    LUT_NULL = 0x7FFFFFFF, /* MiniLUT's empty fanin slot */
    LUT_NULL2 = 0x7FFFFFFE /* MiniLUT's slot 1 of a PO */
};

/*
 * The toggle: PIs a (1) and b (2), then the register output r (3); t = a & b (4); r ^ t as
 * !(r & !t) & !(!r & t) (5, 6, 7, complemented); the output y = r ^ t (8) and the register's
 * input r ^ t (9).
 */
static const int32_t TOGGLE[] = {
    AIG_NULL, AIG_NULL, /* 0: the constant */
    AIG_NULL, AIG_NULL, /* 1: a */
    AIG_NULL, AIG_NULL, /* 2: b */
    AIG_NULL, AIG_NULL, /* 3: r, the register output */
    2,        4,        /* 4: t = a & b */
    6,        9,        /* 5: r & !t */
    7,        8,        /* 6: !r & t */
    11,       13,       /* 7: !5 & !6 = !(r ^ t) */
    15,       AIG_NULL, /* 8: y = r ^ t */
    15,       AIG_NULL, /* 9: the register input r ^ t */
};
static const odin3_abc_aig TOGGLE_AIG = {.objs = TOGGLE, .n_objs = 10, .n_regs = 1};
enum { TOGGLE_PIS = 3, TOGGLE_POS = 2, TOGGLE_XOR_LIT = 15 };

/* A MiniLUT copied out of the session (get_luts' arrays live only until the next call). */
typedef struct lut_copy {
    odin3_abc_luts luts; /* points into the arrays below */
    int32_t *fanins;
    uint32_t *truths;
    int32_t *ci_from;
    uint32_t pis; /* PI objects, so ci_from's length */
    size_t fanin_count, truth_count;
} lut_copy;

static size_t truth_words(uint32_t lut_size) {
    return lut_size > 5 ? (size_t)1 << (lut_size - 5) : 1;
}

static void lut_copy_take(lut_copy *out, const odin3_abc_luts *luts) {
    out->fanin_count = (size_t)luts->lut_size * luts->n_objs;
    out->truth_count = truth_words(luts->lut_size) * luts->n_objs;
    out->fanins = malloc(sizeof *out->fanins * out->fanin_count);
    out->truths = malloc(sizeof *out->truths * out->truth_count);
    TEST_ASSERT_TRUE(out->fanins != NULL && out->truths != NULL);
    memcpy(out->fanins, luts->fanins, sizeof *out->fanins * out->fanin_count);
    memcpy(out->truths, luts->truths, sizeof *out->truths * out->truth_count);
    out->pis = 0;
    for (uint32_t i = 2; i < luts->n_objs; i++) {
        out->pis += luts->fanins[(size_t)luts->lut_size * i] == LUT_NULL;
    }
    out->ci_from = NULL;
    if (luts->ci_from != NULL) {
        out->ci_from = malloc(sizeof *out->ci_from * (out->pis + 1));
        TEST_ASSERT_NOT_NULL(out->ci_from);
        memcpy(out->ci_from, luts->ci_from, sizeof *out->ci_from * out->pis);
    }
    out->luts = *luts;
    out->luts.fanins = out->fanins;
    out->luts.truths = out->truths;
    out->luts.ci_from = out->ci_from;
}

static void lut_copy_free(lut_copy *copy) {
    free(copy->fanins);
    free(copy->truths);
    free(copy->ci_from);
    *copy = (lut_copy){0};
}

/* Fanin slot of object obj, and whether obj is a PO. */
static int32_t fanin_of(const odin3_abc_luts *luts, uint32_t obj, uint32_t slot) {
    return luts->fanins[((size_t)luts->lut_size * obj) + slot];
}

static bool is_po(const odin3_abc_luts *luts, uint32_t obj) {
    return obj > 1 && fanin_of(luts, obj, 0) != LUT_NULL && fanin_of(luts, obj, 1) == LUT_NULL2;
}

/* A script: its commands, run after load_aig and &put (and before &get -m -n). */
typedef struct script {
    const char *const *commands;
    size_t count;
    const char *get; /* the command that brings the mapping back to the & space */
} script;

/* Runs aig through script in the open session; the MiniLUT goes to *out. */
static odin3_status run_in_session(const odin3_abc_aig *aig, const script *run, lut_copy *out,
                                   odin3_abc_fail *fail) {
    odin3_status st = odin3_abc_session_load_aig(self(), aig, fail);
    st = st == ODIN3_OK ? odin3_abc_session_exec(self(), "&put", fail) : st;
    for (size_t i = 0; st == ODIN3_OK && i < run->count; i++) {
        st = odin3_abc_session_exec(self(), run->commands[i], fail);
    }
    st = st == ODIN3_OK ? odin3_abc_session_exec(self(), run->get, fail) : st;
    odin3_abc_luts luts;
    st = st == ODIN3_OK ? odin3_abc_session_get_luts(self(), &luts, fail) : st;
    if (st == ODIN3_OK) {
        lut_copy_take(out, &luts);
    }
    return st;
}

/* run_in_session in a session of its own; the test fails with ABC's lines if it fails. */
static void run_script(const odin3_abc_aig *aig, const script *run, lut_copy *out) {
    static lines got;
    odin3_abc_fail fail;
    got.count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    odin3_status st = run_in_session(aig, run, out, &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, fail.text);
}

static const char *const IF_K6[] = {"if -K 6"};
static const script MAP_K6 = {.commands = IF_K6, .count = 1, .get = "&get -m -n"};

/* The fanins of LUT or PO obj, each on an earlier object that is not a PO; returns their count. */
static uint32_t expect_fanins_before(const odin3_abc_luts *luts, uint32_t obj) {
    uint32_t count = 0;
    while (count < luts->lut_size && fanin_of(luts, obj, count) < LUT_NULL2) {
        int32_t fanin = fanin_of(luts, obj, count);
        TEST_ASSERT_TRUE(fanin >= 0 && (uint32_t)fanin < obj);
        TEST_ASSERT_FALSE(is_po(luts, (uint32_t)fanin));
        count++;
    }
    return count;
}

/* Every LUT's fanins come before it, every PO's driver too, and lut_size covers them all. */
static void expect_topological(const odin3_abc_luts *luts) {
    uint32_t widest = 0;
    for (uint32_t obj = 2; obj < luts->n_objs; obj++) {
        if (fanin_of(luts, obj, 0) == LUT_NULL) {
            continue; /* a PI */
        }
        uint32_t count = expect_fanins_before(luts, obj);
        widest = !is_po(luts, obj) && count > widest ? count : widest;
    }
    /* MiniLUT's lut_size is the widest LUT, at least 2 (3D-0 spike finding), not the K asked. */
    TEST_ASSERT_EQUAL_UINT32(widest > 2 ? widest : 2, luts->lut_size);
}

static void test_load_aig_toggle(void) {
    lut_copy out;
    run_script(&TOGGLE_AIG, &MAP_K6, &out);
    TEST_ASSERT_EQUAL_UINT32(1, out.luts.n_regs);
    TEST_ASSERT_EQUAL_UINT32(TOGGLE_PIS, out.pis);
    TEST_ASSERT_TRUE(out.luts.lut_size >= 2 && out.luts.lut_size <= 6);
    expect_topological(&out.luts);
    TEST_ASSERT_NOT_NULL(out.ci_from);
    for (uint32_t k = 0; k < TOGGLE_PIS; k++) {
        TEST_ASSERT_EQUAL_INT32((int32_t)k, out.ci_from[k]); /* ci_from[2] = 2: the register */
    }
    lut_copy_free(&out);
}

/* CI names that are not c<n> (short_names renames the network's CIs) give -1 in ci_from. */
static void test_get_luts_other_names(void) {
    static const char *const RENAMED[] = {"if -K 6", "short_names"};
    static const script MAP_RENAMED = {.commands = RENAMED, .count = 2, .get = "&get -m -n"};
    lut_copy out;
    run_script(&TOGGLE_AIG, &MAP_RENAMED, &out);
    TEST_ASSERT_NOT_NULL(out.ci_from);
    TEST_ASSERT_EQUAL_UINT32(TOGGLE_PIS, out.pis);
    for (uint32_t k = 0; k < TOGGLE_PIS; k++) {
        TEST_ASSERT_EQUAL_INT32(-1, out.ci_from[k]);
    }
    lut_copy_free(&out);
}

/* odin3_abc_test_name_ci under the guard, as every ABC call runs. */
typedef struct ci_name {
    uint32_t ci;
    const char *name;
} ci_name;

static odin3_status name_ci_fn(void *arg) {
    const ci_name *rename = arg;
    return odin3_abc_test_name_ci(rename->ci, rename->name);
}

/* After the toggle's mapping, the & space's CIs renamed to names[0..2]; get_luts' ci_from must
 * equal want. */
static void expect_ci_from(const char *const names[TOGGLE_PIS], const int32_t want[TOGGLE_PIS]) {
    static lines got;
    odin3_abc_fail fail;
    lut_copy out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    odin3_status st = run_in_session(&TOGGLE_AIG, &MAP_K6, &out, &fail);
    lut_copy_free(&out);
    for (uint32_t k = 0; st == ODIN3_OK && k < TOGGLE_PIS; k++) {
        ci_name rename = {.ci = k, .name = names[k]};
        st = odin3_guard_run(name_ci_fn, &rename, &fail);
    }
    odin3_abc_luts luts;
    st = st == ODIN3_OK ? odin3_abc_session_get_luts(self(), &luts, &fail) : st;
    if (st == ODIN3_OK) {
        lut_copy_take(&out, &luts);
    }
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, fail.text);
    TEST_ASSERT_NOT_NULL(out.ci_from);
    TEST_ASSERT_EQUAL_INT32_ARRAY(want, out.ci_from, TOGGLE_PIS);
    lut_copy_free(&out);
}

/* Only canonical c<n> names count: no leading zero, digits only, at most INT32_MAX. */
static void test_ci_names(void) {
    static const char *const names_a[TOGGLE_PIS] = {"c0", "c01", "c2147483648"};
    static const int32_t want_a[TOGGLE_PIS] = {0, -1, -1};
    expect_ci_from(names_a, want_a);
    static const char *const names_b[TOGGLE_PIS] = {"c", "c1x", "c2147483647"};
    static const int32_t want_b[TOGGLE_PIS] = {-1, -1, 2147483647};
    expect_ci_from(names_b, want_b);
    static const char *const names_c[TOGGLE_PIS] = {"d1", "c-1", "c10"};
    static const int32_t want_c[TOGGLE_PIS] = {-1, -1, 10};
    expect_ci_from(names_c, want_c);
}

/* One malformed MiniAIG and the exact RESULT text it gives. */
typedef struct bad_aig {
    int32_t objs[16];
    uint32_t n_objs, n_regs;
    const char *text;
} bad_aig;

static const bad_aig BAD_AIGS[] = {
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 4, 2, 7, AIG_NULL},
     5,
     0,
     "object 3: an AND needs 0 <= lit0 < lit1 < 6, not (4, 2)"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, 4, 4, AIG_NULL},
     4,
     0,
     "object 2: an AND needs 0 <= lit0 < lit1 < 4, not (2, 4)"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, 2, 6, AIG_NULL},
     5,
     0,
     "object 3: an AND needs 0 <= lit0 < lit1 < 6, not (2, 2)"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, -1, 2, 6, AIG_NULL},
     5,
     0,
     "object 3: an AND needs 0 <= lit0 < lit1 < 6, not (-1, 2)"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, AIG_NULL, AIG_NULL, AIG_NULL},
     4,
     0,
     "object 3: a PI after the first AND or PO"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, AIG_NULL, 2, 4},
     5,
     0,
     "object 4: an AND after the first PO"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, AIG_NULL, 4, AIG_NULL},
     4,
     0,
     "object 3: a PO needs a literal from 0 to 3, not 4"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, -1, AIG_NULL},
     3,
     0,
     "object 2: a PO needs a literal from 0 to 3, not -1"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, AIG_NULL, 2, AIG_NULL},
     4,
     2,
     "2 registers, more than the 1 PIs or the 2 POs"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, 3, AIG_NULL, AIG_NULL, 2, AIG_NULL},
     5,
     0,
     "object 3: a PI after the first AND or PO"},
    {{AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 2, AIG_NULL},
     4,
     2,
     "2 registers, more than the 2 PIs or the 1 POs"},
    {{AIG_NULL, 0, AIG_NULL, AIG_NULL},
     2,
     0,
     "object 0: the constant is (2147483647, 0), not (2147483647, 2147483647)"},
    {{0, AIG_NULL, AIG_NULL, AIG_NULL},
     2,
     0,
     "object 0: the constant is (0, 2147483647), not "
     "(2147483647, 2147483647)"},
    {{0}, 0, 0, "an AIG needs from 1 to 536870912 objects, not 0"},
};

/* Each malformed AIG is refused with its text before ABC sees it; the session stays usable. */
static void test_load_aig_rejects(void) {
    static lines got;
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    for (size_t i = 0; i < sizeof BAD_AIGS / sizeof BAD_AIGS[0]; i++) {
        const odin3_abc_aig aig = {
            .objs = BAD_AIGS[i].objs, .n_objs = BAD_AIGS[i].n_objs, .n_regs = BAD_AIGS[i].n_regs};
        odin3_status st = odin3_abc_session_load_aig(self(), &aig, &fail);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_ENGINE, st, BAD_AIGS[i].text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(ODIN3_ABC_FAIL_RESULT, fail.kind, BAD_AIGS[i].text);
        TEST_ASSERT_EQUAL_STRING(BAD_AIGS[i].text, fail.text);
    }
    const odin3_abc_aig no_objs = {.objs = NULL, .n_objs = 1, .n_regs = 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_load_aig(self(), &no_objs, &fail));
    TEST_ASSERT_EQUAL_STRING("an AIG needs from 1 to 536870912 objects, not 1 (objs is NULL)",
                             fail.text);
    /* Over MiniAIG's ceiling: refused before objs is read (it holds 10 objects only). */
    const odin3_abc_aig too_big = {.objs = TOGGLE, .n_objs = (1U << 29) + 1, .n_regs = 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_load_aig(self(), &too_big, &fail));
    TEST_ASSERT_EQUAL_STRING("an AIG needs from 1 to 536870912 objects, not 536870913", fail.text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_load_aig(self(), NULL, &fail));
    TEST_ASSERT_EQUAL_STRING("no AIG (aig is NULL)", fail.text);
    /* Nothing reached ABC: the & space is still empty, and the session works. */
    TEST_ASSERT_FALSE(odin3_guard_disabled());
    odin3_abc_luts luts;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_get_luts(self(), &luts, &fail));
    TEST_ASSERT_EQUAL_STRING("no mapping in the & space", fail.text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_load_aig(self(), &TOGGLE_AIG, &fail));
    /* The limits themselves are accepted: as many registers as PIs and POs, and a PO on the last
     * AND (literal 2 * index - 1 of the object before it). */
    static const int32_t edge[] = {AIG_NULL, AIG_NULL, AIG_NULL, AIG_NULL, 0, 3, 5, AIG_NULL};
    const odin3_abc_aig at_limits = {.objs = edge, .n_objs = 4, .n_regs = 1};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_abc_session_load_aig(self(), &at_limits, &fail),
                                  fail.text);
    odin3_abc_session_close(self());
}

static void test_get_luts_unmapped(void) {
    static lines got;
    odin3_abc_fail fail;
    odin3_abc_luts luts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    odin3_status st = odin3_abc_session_load_aig(self(), &TOGGLE_AIG, &fail);
    st = st == ODIN3_OK ? odin3_abc_session_exec(self(), "&put", &fail) : st;
    st = st == ODIN3_OK ? odin3_abc_session_exec(self(), "strash", &fail) : st;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, fail.text);
    st = odin3_abc_session_get_luts(self(), &luts, &fail);
    odin3_status no_place = odin3_abc_session_get_luts(self(), NULL, &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, no_place);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_RESULT, fail.kind);
    TEST_ASSERT_EQUAL_STRING("nowhere to put the LUTs (luts is NULL)", fail.text);
    TEST_ASSERT_FALSE(odin3_guard_disabled());
}

/* The name map of the toggle against its own (aligned) result: one literal or -1 per object;
 * every PI maps to its own PI's literal, and the LUT driving the register input to r ^ t. */
static bool is_literal_or_none(int32_t lit) {
    return lit == -1 || (lit >= 0 && lit < 2 * (int32_t)TOGGLE_AIG.n_objs);
}

static bool is_pi(const odin3_abc_luts *luts, uint32_t obj) {
    return obj > 1 && fanin_of(luts, obj, 0) == LUT_NULL;
}

/* lits' entry for obj is -1 or a literal of the toggle, and a PI's is its own PI's literal (PI
 * number *pi, counted here). */
static bool name_ok(const odin3_abc_luts *aligned, const int32_t *lits, uint32_t obj,
                    uint32_t *pi) {
    if (!is_literal_or_none(lits[obj])) {
        return false;
    }
    if (!is_pi(aligned, obj)) {
        return true;
    }
    (*pi)++;
    return lits[obj] == (int32_t)(2 * *pi);
}

static void expect_name_map(const odin3_abc_luts *aligned, const int32_t *lits) {
    uint32_t pi = 0;
    for (uint32_t obj = 0; obj < aligned->n_objs; obj++) {
        TEST_ASSERT_TRUE(name_ok(aligned, lits, obj, &pi));
    }
    TEST_ASSERT_EQUAL_UINT32(TOGGLE_PIS, pi);
    uint32_t reg_in = aligned->n_objs - 1;
    TEST_ASSERT_TRUE(is_po(aligned, reg_in));
    int32_t driver = fanin_of(aligned, reg_in, 0);
    TEST_ASSERT_EQUAL_INT32(TOGGLE_XOR_LIT, lits[driver]);
}

/* Which count of the aligned network expect_count_mismatch changes. */
typedef enum count_kind { COUNT_REGS, COUNT_POS, COUNT_PIS } count_kind;

/* name_map on a copy of the toggle's result with one count changed must be RESULT; its text goes
 * to text (LINE_CAP bytes). The session stays open. */
static void expect_count_mismatch(const lut_copy *out, count_kind kind, char *text) {
    odin3_abc_luts other = out->luts;
    int32_t *fanins = malloc(sizeof *fanins * out->fanin_count);
    TEST_ASSERT_NOT_NULL(fanins);
    memcpy(fanins, out->fanins, sizeof *fanins * out->fanin_count);
    other.fanins = fanins;
    if (kind == COUNT_REGS) {
        other.n_regs = 0;
    } else if (kind == COUNT_POS) {
        other.n_objs--;
    } else {
        uint32_t lut = 2;
        while (is_pi(&other, lut)) {
            lut++;
        }
        fanins[(size_t)other.lut_size * lut] = LUT_NULL;
    }
    odin3_abc_fail fail;
    const int32_t *lits = NULL;
    odin3_status st = odin3_abc_session_name_map(self(), &other, &lits, &fail);
    free(fanins);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_RESULT, fail.kind);
    TEST_ASSERT_TRUE(strlen(fail.text) < LINE_CAP);
    memcpy(text, fail.text, strlen(fail.text) + 1);
}

static void test_name_map_aligned(void) {
    static lines got;
    odin3_abc_fail fail;
    lut_copy out = {0};
    const int32_t *lits = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_session_name_map(self(), &out.luts, &lits, &fail));
    TEST_ASSERT_EQUAL_STRING("no AIG was loaded in this session", fail.text);
    odin3_status st = run_in_session(&TOGGLE_AIG, &MAP_K6, &out, &fail);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, fail.text);
    /* No register merged (if -K 6 has no scorr), so the result is its own aligned network. */
    st = odin3_abc_session_name_map(self(), &out.luts, &lits, &fail);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, fail.text);
    TEST_ASSERT_NOT_NULL(lits);
    expect_name_map(&out.luts, lits);
    /* Each count on its own: the registers, the POs (the last PO dropped) and the PIs (the
     * first LUT made a PI); ABC's Gia_ManDup2 would assert on the last two. */
    char regs_text[LINE_CAP];
    char pos_text[LINE_CAP];
    char pis_text[LINE_CAP];
    expect_count_mismatch(&out, COUNT_REGS, regs_text);
    expect_count_mismatch(&out, COUNT_POS, pos_text);
    expect_count_mismatch(&out, COUNT_PIS, pis_text);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_STRING("the aligned network has 3 PIs, 2 POs and 0 registers; the loaded "
                             "AIG has 3, 2 and 1",
                             regs_text);
    TEST_ASSERT_EQUAL_STRING("the aligned network has 3 PIs, 1 POs and 1 registers; the loaded "
                             "AIG has 3, 2 and 1",
                             pos_text);
    TEST_ASSERT_EQUAL_STRING("the aligned network has 4 PIs, 2 POs and 1 registers; the loaded "
                             "AIG has 3, 2 and 1",
                             pis_text);
    lut_copy_free(&out);
}

/* Malformed aligned networks: refused with their text, ABC never called. */
static void test_name_map_rejects(void) {
    static lines got;
    odin3_abc_fail fail;
    lut_copy out = {0};
    const int32_t *lits = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_in_session(&TOGGLE_AIG, &MAP_K6, &out, &fail));
    odin3_abc_luts bad = out.luts;
    bad.lut_size = 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_name_map(self(), &bad, &lits, &fail));
    TEST_ASSERT_EQUAL_STRING("the aligned network needs lut_size 2 to 16 and 2 to 536870912 "
                             "objects, not 1 and 9",
                             fail.text);
    /* The last object (a PO) driven by itself: a fanin that is not earlier. */
    uint32_t last = out.luts.n_objs - 1;
    TEST_ASSERT_TRUE(is_po(&out.luts, last) && is_po(&out.luts, last - 1));
    out.fanins[(size_t)out.luts.lut_size * last] = (int32_t)last;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_session_name_map(self(), &out.luts, &lits, &fail));
    TEST_ASSERT_EQUAL_STRING("aligned object 8: fanin 0 is 8, not an earlier PI or LUT", fail.text);
    /* ... and driven by the PO before it. */
    out.fanins[(size_t)out.luts.lut_size * last] = (int32_t)(last - 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_session_name_map(self(), &out.luts, &lits, &fail));
    TEST_ASSERT_EQUAL_STRING("aligned object 8: fanin 0 is 7, not an earlier PI or LUT", fail.text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_session_name_map(self(), NULL, &lits, &fail));
    odin3_abc_session_close(self());
    TEST_ASSERT_FALSE(odin3_guard_disabled());
    lut_copy_free(&out);
}

/*
 * The test hook fails each of the adapter's own allocations in turn (open's capture file, the
 * AIG copy, ci_from, the aligned copies and the PO marks): each gives ODIN3_ERR_NO_MEMORY, the
 * engine stays enabled, and the whole flow then runs (§13.6).
 */
static odin3_status oom_flow(lut_copy *out, odin3_abc_fail *fail) {
    odin3_status st = odin3_abc_session_open(self(), NULL, NULL, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    st = run_in_session(&TOGGLE_AIG, &MAP_K6, out, fail);
    const int32_t *lits = NULL;
    st = st == ODIN3_OK ? odin3_abc_session_name_map(self(), &out->luts, &lits, fail) : st;
    odin3_abc_session_close(self());
    return st;
}

static void test_adapter_oom(void) {
    odin3_abc_fail fail;
    uint32_t injected = 0;
    for (uint32_t after = 0;; after++) {
        lut_copy out = {0};
        odin3_abc_test_fail_alloc_after(after);
        odin3_status st = oom_flow(&out, &fail);
        lut_copy_free(&out);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, fail.text);
        TEST_ASSERT_FALSE(odin3_guard_disabled());
        injected++;
    }
    odin3_abc_test_fail_alloc_after(UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(6, injected); /* tmpfile, AIG copy, ci_from, 2 aligned copies, marks */
}

/* --- determinism and threads on a 40-AND network (§9, Review Focus 5) ------------------------ */

enum { NET_PIS = 8, NET_REGS = 4, NET_ANDS = 40, NET_OUTS = 4, NET_SEED = 0x3D2 };
enum { NET_CIS = NET_PIS + NET_REGS, NET_OBJS = 1 + NET_CIS + NET_ANDS + NET_OUTS + NET_REGS };

static int32_t net40_objs[2 * NET_OBJS];
static const odin3_abc_aig NET40 = {.objs = net40_objs, .n_objs = NET_OBJS, .n_regs = NET_REGS};

/*
 * 8 PIs and 4 register outputs, then 40 ANDs drawn from odin3_prng (seed 0x3D2): AND j takes the
 * object just before it and an earlier random one, each with a random complement, so all 40 are in
 * the last one's cone. The outputs are ANDs 39, 30, 20 and 10, the register inputs ANDs 35, 25, 15
 * and 5, each with a random complement.
 */
static void build_net40(void) {
    odin3_prng prng;
    odin3_prng_seed(&prng, NET_SEED);
    for (size_t i = 0; i <= NET_CIS; i++) {
        net40_objs[2 * i] = AIG_NULL;
        net40_objs[(2 * i) + 1] = AIG_NULL;
    }
    for (uint32_t j = 0; j < NET_ANDS; j++) {
        size_t i = 1 + NET_CIS + j;
        uint32_t other = 1 + (uint32_t)(odin3_prng_next(&prng) % (i - 2));
        int32_t lit_prev = (int32_t)(2 * (i - 1)) + odin3_prng_bit(&prng);
        int32_t lit_other = (int32_t)(2 * other) + odin3_prng_bit(&prng);
        net40_objs[2 * i] = lit_other; /* other < i - 1, so lit_other < lit_prev */
        net40_objs[(2 * i) + 1] = lit_prev;
    }
    static const uint32_t pos[NET_OUTS + NET_REGS] = {39, 30, 20, 10, 35, 25, 15, 5};
    for (uint32_t k = 0; k < NET_OUTS + NET_REGS; k++) {
        size_t i = 1 + NET_CIS + NET_ANDS + k;
        net40_objs[2 * i] = (int32_t)(2 * (1 + NET_CIS + pos[k])) + odin3_prng_bit(&prng);
        net40_objs[(2 * i) + 1] = AIG_NULL;
    }
}

/* lib/abc/vtr.abc (§7.7) with K = 6, and Review Focus 5's randomised script. */
static const char *const VTR_K6[] = {"strash", "ifraig -v",  "scorr -v", "dc2 -v",
                                     "dch -f", "if -K 6 -v", "mfs2 -v",  "print_stats"};
static const script DEFAULT_K6 = {.commands = VTR_K6, .count = 8, .get = "&get -m -n"};
static const char *const FRAIG_IF_K6[] = {"strash", "fraig", "if -K 6"};
static const script FRAIG_K6 = {.commands = FRAIG_IF_K6, .count = 3, .get = "&get -m -n"};

/* A thread that does nothing: o3test_spawn's, the counter's positive control. */
static void *idle_thread(void *arg) {
    return arg;
}

static int o3test_spawn(int argc, char **argv) {
    (void)argc;
    (void)argv;
    pthread_t thread;
    if (pthread_create(&thread, NULL, idle_thread, NULL) != 0) {
        return 1;
    }
    return pthread_join(thread, NULL) != 0;
}

static void test_no_threads_default_script(void) {
    static bool added;
    if (!added) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_test_add_command("o3test_spawn", o3test_spawn));
        added = true;
    }
    lut_copy out;
    uint32_t before = odin3_abc_test_thread_count();
    run_script(&NET40, &DEFAULT_K6, &out);
    TEST_ASSERT_EQUAL_UINT32(before, odin3_abc_test_thread_count());
    TEST_ASSERT_TRUE(out.luts.n_objs > NET_CIS + 2);
    lut_copy_free(&out);
    /* The counter counts: a command that starts one thread adds one. */
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    odin3_status st = odin3_abc_session_exec(self(), "o3test_spawn", &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(before + 1, odin3_abc_test_thread_count());
}

/* The same script twice in one process gives byte-identical fanins and truths. */
static void expect_same_twice(const script *run) {
    lut_copy first;
    lut_copy second;
    run_script(&NET40, run, &first);
    run_script(&NET40, run, &second);
    TEST_ASSERT_EQUAL_UINT32(first.luts.n_objs, second.luts.n_objs);
    TEST_ASSERT_EQUAL_UINT32(first.luts.n_regs, second.luts.n_regs);
    TEST_ASSERT_EQUAL_UINT32(first.luts.lut_size, second.luts.lut_size);
    TEST_ASSERT_EQUAL_MEMORY(first.fanins, second.fanins, sizeof *first.fanins * first.fanin_count);
    TEST_ASSERT_EQUAL_MEMORY(first.truths, second.truths, sizeof *first.truths * first.truth_count);
    expect_topological(&first.luts);
    lut_copy_free(&first);
    lut_copy_free(&second);
}

static void test_determinism_one_process(void) {
    expect_same_twice(&DEFAULT_K6);
    expect_same_twice(&FRAIG_K6);
}

/* --- ABC's command splitting (Task 11's "checked is what runs") ------------------------------- */

static void test_split(void) {
    char **argv = NULL;
    TEST_ASSERT_EQUAL_INT(-1, odin3_abc_test_split("if -K 6", &argv));
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    int argc = odin3_abc_test_split("  if -K \"6\" -v; strash", &argv);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(4, argc);
    static const char *const want[] = {"if", "-K", "6", "-v"};
    for (int i = 0; i < argc; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], argv[i]);
        free(argv[i]);
    }
    free((void *)argv);
}

/* --- child processes, parent side ------------------------------------------------------------- */

typedef struct child {
    int status;
    char out[OUT_CAP]; /* the child's fds 1 and 2 */
} child;

static char asan_env[LINE_CAP];
static char extra_env[LINE_CAP];

/* ASAN_OPTIONS=<old>:detect_leaks=0 into asan_env (an escape leaks ABC's frame). */
static void set_asan_env(const char *old) {
    static const char key[] = "ASAN_OPTIONS=";
    int len = snprintf(asan_env, sizeof asan_env, "%s%s%sdetect_leaks=0", key, old,
                       old[0] != '\0' ? ":" : "");
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof asan_env);
}

/* environ without its ASAN_OPTIONS, then asan_env, then extra (NULL: none); malloc'd. */
static char **child_env(const char *extra) {
    static const char key[] = "ASAN_OPTIONS=";
    size_t count = 0;
    while (environ[count] != NULL) {
        count++;
    }
    char **env = (char **)calloc(count + 3, sizeof *env);
    const char *old = "";
    size_t kept = 0;
    for (size_t i = 0; env != NULL && i < count; i++) {
        if (strncmp(environ[i], key, sizeof key - 1) == 0) {
            old = environ[i] + sizeof key - 1;
        } else {
            env[kept++] = environ[i];
        }
    }
    TEST_ASSERT_NOT_NULL(env);
    set_asan_env(old);
    env[kept++] = asan_env;
    if (extra != NULL && strlen(extra) < sizeof extra_env) {
        memcpy(extra_env, extra, strlen(extra) + 1);
        env[kept++] = extra_env;
    }
    env[kept] = NULL;
    return env;
}

/* One child run: the --escape case, and one extra environment entry (NULL: none). */
typedef struct child_req {
    const char *which;
    const char *extra;
} child_req;

/* Runs this program with --escape <req->which> and req->extra in its environment, its fds 1 and
 * 2 on one file, which *res receives. */
static void run_child(const child_req *req, child *res) {
    static char exe[] = "/proc/self/exe";
    static char flag[] = "--escape";
    char name[LINE_CAP];
    TEST_ASSERT_TRUE(strlen(req->which) < sizeof name);
    memcpy(name, req->which, strlen(req->which) + 1);
    char *argv[] = {exe, flag, name, NULL};
    FILE *out = tmpfile();
    TEST_ASSERT_NOT_NULL(out);
    posix_spawn_file_actions_t actions;
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 1));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 2));
    char **env = child_env(req->extra);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe, &actions, NULL, argv, env);
    (void)posix_spawn_file_actions_destroy(&actions);
    free((void *)env);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &res->status, 0));
    TEST_ASSERT_EQUAL_INT(0, fseek(out, 0, SEEK_SET));
    size_t got = fread(res->out, 1, OUT_CAP - 1, out);
    res->out[got] = '\0';
    (void)fclose(out);
}

/* The child exited 0 (every check passed) and printed MARKER at its end. */
static void expect_child_ok(const char *which, const char *extra) {
    static child res;
    const child_req req = {.which = which, .extra = extra};
    run_child(&req, &res);
    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(res.status), res.out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(res.status), res.out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res.out, MARKER), res.out);
}

/* The case name and the expected off-thread line of one escaping call. */
typedef struct offthread_names {
    char which[LINE_CAP];
    char line[LINE_CAP];
} offthread_names;

static bool offthread_names_fill(offthread_names *names, const char *call) {
    int which = snprintf(names->which, sizeof names->which, "thread_%s", call);
    int line = snprintf(names->line, sizeof names->line,
                        "odin3: linked ABC called %s outside its session thread; stopping\n", call);
    return which > 0 && (size_t)which < sizeof names->which && line > 0 &&
           (size_t)line < sizeof names->line;
}

/* The case thread_<call> printed "odin3: linked ABC called <call> outside its session thread;
 * stopping", then died by SIGKILL: the off-thread stop that ships until Peter rules on point (a)
 * (a trap would make it SIGILL). */
static void expect_offthread_stop(const char *call) {
    static child res;
    static offthread_names names;
    TEST_ASSERT_TRUE(offthread_names_fill(&names, call));
    const child_req req = {.which = names.which, .extra = NULL};
    run_child(&req, &res);
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(res.status), res.out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGKILL, WTERMSIG(res.status), res.out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res.out, names.line), res.out);
}

static void test_escape_exit(void) {
    expect_child_ok("exit", NULL);
}

static void test_escape_underscore_exit(void) {
    expect_child_ok("_exit", NULL);
}

static void test_escape_abort(void) {
    expect_child_ok("abort", NULL);
}

static void test_escape_assert(void) {
    expect_child_ok("assert", NULL);
}

static void test_escape_thread_exit(void) {
    expect_offthread_stop("exit");
}

static void test_escape_thread_abort(void) {
    expect_offthread_stop("abort");
}

static void test_escape_thread_assert(void) {
    expect_offthread_stop("assert");
}

/* The adapter assertion disables the engine and leaks the session's frame as an escape does, so
 * it runs in a child too. */
static void test_adapter_assert_clean(void) {
    expect_child_ok("invariant", NULL);
}

/* Review finding C1: a libabc_*.so in the working directory, or in $ABC_LIB_PATH, is never
 * loaded (its constructor would exit 9). */
static void test_planted_library_in_cwd(void) {
    expect_child_ok("planted_cwd", NULL);
}

static void test_planted_library_on_abc_lib_path(void) {
    expect_child_ok("planted_env", "ABC_LIB_PATH=" ODIN3_ABC_PLANTED_DIR);
}

static void test_close_without_descriptors(void) {
    expect_child_ok("no_descriptors", NULL);
}

static void test_unguarded_bridge_call(void) {
    expect_child_ok("unguarded", NULL);
}

/* --- child processes, child side -------------------------------------------------------------- */

static int child_failures;

static void child_check(bool ok, const char *what) {
    if (!ok) {
        printf("o3test: FAILED: %s\n", what);
        child_failures++;
    }
}

static long code_arg(int argc, char **argv) {
    return argc > 1 ? strtol(argv[1], NULL, DECIMAL) : 1;
}

static int o3test_exit(int argc, char **argv) {
    long code = code_arg(argc, argv);
    printf("o3test_exit: calling exit(%ld)\n", code);
    exit((int)code);
}

static int o3test_underscore_exit(int argc, char **argv) {
    long code = code_arg(argc, argv);
    printf("o3test__exit: calling _exit(%ld)\n", code);
    (void)fflush(stdout); /* _exit flushes nothing */
    _exit((int)code);
}

static int o3test_abort(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("o3test_abort: calling abort()\n");
    abort();
}

static int o3test_assert(int argc, char **argv) {
    (void)argc;
    (void)argv;
    o3test_assert_fail("o3test assertion", "o3test.c", ASSERT_LINE, "o3test_assert");
}

/* The new thread's escape, chosen by the command's name (o3test_thread_<exit|abort|assert>). */
static void *o3test_thread_body(void *arg) {
    const char *name = arg;
    if (strcmp(name, "o3test_thread_abort") == 0) {
        abort();
    }
    if (strcmp(name, "o3test_thread_assert") == 0) {
        o3test_assert_fail("o3test thread assertion", "o3test.c", ASSERT_LINE, "thread");
    }
    exit(THREAD_EXIT_CODE);
}

static int o3test_thread(int argc, char **argv) {
    (void)argc;
    pthread_t thread;
    if (pthread_create(&thread, NULL, o3test_thread_body, argv[0]) != 0) {
        return 1;
    }
    (void)pthread_join(thread, NULL);
    return 0;
}

static odin3_status call_invariant(void *arg) {
    const bool *holds = arg;
    return odin3_abc_test_invariant(*holds);
}

/* odin3_abc_test_invariant inside a guarded call: OK while it holds; then {ASSERT, 0, "adapter:
 * adapters/abc/guard.c:<line>: o3test invariant"}, a clean return, and DISABLED afterwards. */
static void child_invariant(void) {
    static const char prefix[] = "adapter: adapters/abc/";
    static const char suffix[] = ": o3test invariant";
    odin3_abc_fail fail;
    bool holds = true;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    child_check(odin3_guard_run(call_invariant, &holds, &fail) == ODIN3_OK, "a holding invariant");
    holds = false;
    odin3_status st = odin3_guard_run(call_invariant, &holds, &fail);
    child_check(st == ODIN3_ERR_ENGINE, "a failing invariant gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == ODIN3_ABC_FAIL_ASSERT && fail.code == 0, "kind ASSERT, code 0");
    size_t len = strlen(fail.text);
    bool framed = len > sizeof prefix + sizeof suffix &&
                  strncmp(fail.text, prefix, sizeof prefix - 1) == 0 &&
                  strcmp(fail.text + len - (sizeof suffix - 1), suffix) == 0;
    child_check(framed, fail.text);
    printf("o3test: adapter assertion text: %s\n", fail.text);
    child_check(odin3_guard_disabled(), "the engine is disabled");
    odin3_abc_session_close(self());
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "next open");
}

static bool same_file(int fd, const struct stat *before) {
    struct stat now;
    return fstat(fd, &now) == 0 && now.st_dev == before->st_dev && now.st_ino == before->st_ino;
}

/* Runs command in a fresh session: it must escape as expected (kind, code, text), with fds 1 and
 * 2 restored and the command's own line (said, if not NULL) delivered; then the engine refuses
 * every later call with DISABLED. */
static void child_escape(const char *command, const odin3_abc_fail *expected, const char *said) {
    static lines got;
    odin3_abc_fail fail;
    struct stat out_before;
    struct stat err_before;
    child_check(odin3_abc_session_open(self(), collect, &got, &fail) == ODIN3_OK, "open");
    child_check(fstat(1, &out_before) == 0 && fstat(2, &err_before) == 0, "fstat");
    odin3_status st = odin3_abc_session_exec(self(), command, &fail);
    child_check(same_file(1, &out_before) && same_file(2, &err_before), "fds 1 and 2 restored");
    child_check(st == ODIN3_ERR_ENGINE, "the escape gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == expected->kind && fail.code == expected->code, "kind and code");
    child_check(strcmp(fail.text, expected->text) == 0, fail.text);
    child_check(said == NULL || has_line(&got, said), "the command's line");
    child_check(odin3_guard_disabled(), "the engine is disabled");
    st = odin3_abc_session_exec(self(), "echo late", &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "exec after");
    odin3_abc_session_close(self());
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "next open");
}

static void case_exit(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_EXIT, .code = EXIT_CODE};
    child_escape("o3test_exit 3", &expected, "o3test_exit: calling exit(3)");
}

static void case_underscore_exit(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_EXIT, .code = UNDERSCORE_EXIT_CODE};
    child_escape("o3test__exit 4", &expected, "o3test__exit: calling _exit(4)");
}

static void case_abort(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_ABORT};
    child_escape("o3test_abort", &expected, "o3test_abort: calling abort()");
}

static void case_assert(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_ASSERT,
                                     .text = "o3test.c:7: o3test assertion"};
    child_escape("o3test_assert", &expected, NULL);
}

/* An escape on a thread of its own: the process must stop inside exec. */
static void child_offthread(const char *command) {
    odin3_abc_fail fail;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    (void)odin3_abc_session_exec(self(), command, &fail);
    child_check(false, "the process outlived an off-thread escape");
}

static void case_thread_exit(void) {
    child_offthread("o3test_thread_exit");
}

static void case_thread_abort(void) {
    child_offthread("o3test_thread_abort");
}

static void case_thread_assert(void) {
    child_offthread("o3test_thread_assert");
}

/* A session opened beside libabc_o3test.so (in the cwd or on $ABC_LIB_PATH): the dlopen is
 * refused, nothing is loaded (the library's constructor would exit 9), the session runs. */
static void child_planted(void) {
    static lines got;
    odin3_abc_fail fail;
    child_check(odin3_abc_session_open(self(), collect, &got, &fail) == ODIN3_OK, "open");
    child_check(has_line(&got, "ABC tried to dlopen; refused"), "the dlopen was refused");
    child_check(!odin3_guard_disabled(), "the engine stays enabled");
    odin3_status st = odin3_abc_session_exec(self(), "echo alive", &fail);
    child_check(st == ODIN3_OK && has_line(&got, "alive"), "the session runs");
    odin3_abc_session_close(self());
    for (size_t i = 0; i < got.count; i++) {
        printf("o3test: line: %s\n", got.text[i]);
    }
}

static void case_planted_cwd(void) {
    child_check(chdir(ODIN3_ABC_PLANTED_DIR) == 0, "chdir to the planted library");
    child_planted();
}

static void case_planted_env(void) {
    child_planted();
}

/* A bridge call outside the guard (finding I1 of the Task 6 review) is an adapter assertion:
 * ODIN3_ERR_ENGINE before ABC runs, the assertion's text, and the engine disabled. */
static void case_unguarded(void) {
    odin3_abc_fail fail;
    int rc = 0;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    odin3_status st = odin3_guard_finish(odin3_bridge_exec("echo unguarded", &rc), &fail);
    child_check(st == ODIN3_ERR_ENGINE, "an unguarded bridge call gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == ODIN3_ABC_FAIL_ASSERT && strstr(fail.text, "adapters/abc/bridge.c:") &&
                    strstr(fail.text, ": odin3_guard_active_here()") != NULL,
                fail.text);
    child_check(odin3_guard_disabled(), "the engine is disabled");
    odin3_abc_session_close(self());
}

/* With no descriptor left, exec reports NO_MEMORY, and close still leaves and destroys the
 * frame: the engine stays usable once descriptors are back (review finding 6). */
static void case_no_descriptors(void) {
    odin3_abc_fail fail;
    struct rlimit limit;
    child_check(getrlimit(RLIMIT_NOFILE, &limit) == 0, "getrlimit");
    if (limit.rlim_cur > DESCRIPTOR_LIMIT) {
        limit.rlim_cur = DESCRIPTOR_LIMIT;
        child_check(setrlimit(RLIMIT_NOFILE, &limit) == 0, "setrlimit");
    }
    static int held[DESCRIPTOR_LIMIT];
    size_t count = 0;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    for (int fd = dup(STDERR_FILENO); fd >= 0 && count < DESCRIPTOR_LIMIT;
         fd = dup(STDERR_FILENO)) {
        held[count++] = fd;
    }
    odin3_status st = odin3_abc_session_exec(self(), "echo starved", &fail);
    child_check(st == ODIN3_ERR_NO_MEMORY, "exec without descriptors gives NO_MEMORY");
    odin3_abc_session_close(self());
    child_check(!odin3_guard_disabled(), "the engine stays enabled");
    for (size_t i = 0; i < count; i++) {
        (void)close(held[i]);
    }
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_OK, "the next open works");
    odin3_abc_session_close(self());
}

static const struct {
    const char *name;
    void (*run)(void);
} CHILD_CASES[] = {
    {"exit", case_exit},
    {"_exit", case_underscore_exit},
    {"abort", case_abort},
    {"assert", case_assert},
    {"invariant", child_invariant},
    {"thread_exit", case_thread_exit},
    {"thread_abort", case_thread_abort},
    {"thread_assert", case_thread_assert},
    {"planted_cwd", case_planted_cwd},
    {"planted_env", case_planted_env},
    {"no_descriptors", case_no_descriptors},
    {"unguarded", case_unguarded},
};

static int run_child_case(const char *which) {
    (void)odin3_abc_test_add_command("o3test_exit", o3test_exit);
    (void)odin3_abc_test_add_command("o3test__exit", o3test_underscore_exit);
    (void)odin3_abc_test_add_command("o3test_abort", o3test_abort);
    (void)odin3_abc_test_add_command("o3test_assert", o3test_assert);
    (void)odin3_abc_test_add_command("o3test_thread_exit", o3test_thread);
    (void)odin3_abc_test_add_command("o3test_thread_abort", o3test_thread);
    (void)odin3_abc_test_add_command("o3test_thread_assert", o3test_thread);
    bool known = false;
    for (size_t i = 0; i < sizeof CHILD_CASES / sizeof CHILD_CASES[0]; i++) {
        if (strcmp(which, CHILD_CASES[i].name) == 0) {
            CHILD_CASES[i].run();
            known = true;
        }
    }
    child_check(known, "unknown --escape case");
    printf("%s\n", MARKER);
    return child_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--escape") == 0) {
        return run_child_case(argv[2]);
    }
    UNITY_BEGIN();
    RUN_TEST(test_sessions_100);
    RUN_TEST(test_capture_echo);
    RUN_TEST(test_capture_flushes_pending_output);
    RUN_TEST(test_capture_partial_line_waits);
    RUN_TEST(test_exec_failure);
    RUN_TEST(test_refused_calls);
    RUN_TEST(test_open_resets_generators);
    RUN_TEST(test_plugin_init);
    RUN_TEST(test_load_aig_toggle);
    RUN_TEST(test_get_luts_other_names);
    RUN_TEST(test_ci_names);
    RUN_TEST(test_load_aig_rejects);
    RUN_TEST(test_get_luts_unmapped);
    RUN_TEST(test_name_map_aligned);
    RUN_TEST(test_name_map_rejects);
    RUN_TEST(test_adapter_oom);
    build_net40();
    RUN_TEST(test_no_threads_default_script);
    RUN_TEST(test_determinism_one_process);
    RUN_TEST(test_split);
    RUN_TEST(test_escape_exit);
    RUN_TEST(test_escape_underscore_exit);
    RUN_TEST(test_escape_abort);
    RUN_TEST(test_escape_assert);
    RUN_TEST(test_escape_thread_exit);
    RUN_TEST(test_escape_thread_abort);
    RUN_TEST(test_escape_thread_assert);
    RUN_TEST(test_adapter_assert_clean);
    RUN_TEST(test_planted_library_in_cwd);
    RUN_TEST(test_planted_library_on_abc_lib_path);
    RUN_TEST(test_close_without_descriptors);
    RUN_TEST(test_unguarded_bridge_call);
    return UNITY_END();
}
