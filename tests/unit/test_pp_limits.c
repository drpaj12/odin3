/*
 * test_pp_limits.c — every preprocessor cap of 2B spec §9, each lowered through
 * odin3_pp_test_set_limits and hit with its exact located message, plus the source manager's
 * 2^28 file cap at the root's `at` and at an `include. The log sink snapshots the guarded
 * counters at the moment the error is logged (before the run is reset), so each test also shows
 * that nothing was created or allocated past the cap.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = PP_PATH_BUF, TEXT_MAX = 8192 };

/* The guarded counters when the first error was logged. */
typedef struct snapshot {
    bool taken;
    size_t out_len;
    size_t out_cap;
    uint64_t srcman_bytes;
    uint32_t nsegments;
    uint32_t nbuffers;
    uint32_t nincludes;
    uint32_t nexpansions;
    uint64_t loc_bytes;
    uint64_t cache_bytes;
    size_t files_read;
    size_t live_runs; /* runs + args + scratch */
    size_t conds;
    size_t incstack;
    size_t definitions;
    size_t live_macros;
    size_t arena;
    size_t sm_bufs; /* source-manager buffers, slot 0 included */
    uint32_t max_chain;
} snapshot;

static snapshot snap;

static uint32_t max_chain(const odin3_pp *state) {
    uint32_t most = 0;
    for (size_t i = 0; i < state->bufs.len; i++) {
        const odin3_pp_bufrec *rec = odin3_vec_cat(&state->bufs, i);
        most = rec->chain > most ? rec->chain : most;
    }
    return most;
}

static void take_snapshot(void) {
    const odin3_pp *state = pp;
    snap = (snapshot){
        .taken = true,
        .out_len = state->out.len,
        .out_cap = state->out.cap,
        .srcman_bytes = state->srcman_bytes,
        .nsegments = state->nsegments,
        .nbuffers = state->nbuffers,
        .nincludes = state->nincludes,
        .nexpansions = state->nexpansions,
        .loc_bytes = state->loc_bytes,
        .cache_bytes = state->cache_bytes,
        .files_read = state->files_read.len,
        .live_runs = state->runs.len + state->args.len + state->scratch.len,
        .conds = state->conds.len,
        .incstack = state->incstack.len,
        .definitions = state->macros.len,
        .live_macros = odin3_u64map_count(state->macro_by_name),
        .arena = odin3_arena_bytes_reserved(state->arena),
        .sm_bufs = sm->bufs.len,
        .max_chain = max_chain(state),
    };
}

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level != ODIN3_LOG_ERROR) {
        return;
    }
    errors++;
    (void)snprintf(captured, sizeof captured, "%s", msg);
    if (pp != NULL && !snap.taken) {
        take_snapshot();
    }
}

void setUp(void) {
    pp_fixture_set_up();
    odin3_log_set_sink(sink, NULL);
    memset(&snap, 0, sizeof snap);
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

/* A fresh preprocessor whose caps are the defaults changed by lower. */
static void lowered(void (*lower)(odin3_pp_limits *limits)) {
    create_pp(NULL);
    odin3_pp_limits limits = odin3_pp_default_limits();
    lower(&limits);
    odin3_pp_test_set_limits(pp, limits);
}

/* Runs tmp/name, which must stop at exactly one error, want (the whole message). */
static void assert_stops(const char *name, const char *want) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run_path(name, path), captured);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, captured, name);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NULL(stream.text.ptr);
    TEST_ASSERT_TRUE(snap.taken);
}

static void put_text(const char *name, const char *text) {
    put(name, odin3_bytes_cstr(text), NULL);
}

/* A sparse file of len bytes at tmp/name (no disk blocks, nothing read). */
static void put_sparse(const char *name, off_t len) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    TEST_ASSERT_TRUE(fd >= 0);
    remember(path);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, len));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

/* A project word's loc: "files.f" line 2, column 3. */
static odin3_loc project_word(void) {
    odin3_srcfile_spec spec = {id("files.f"), 0, 0, 0, 16, {0, 0}};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, 8));
    return odin3_srcman_loc(sm, buf, 10);
}

/* --- the values ---------------------------------------------------------------------------- */

static void test_default_limits_are_the_spec_values(void) {
    odin3_pp_limits got = odin3_pp_default_limits();
    TEST_ASSERT_EQUAL_UINT32(64, got.include_depth);
    TEST_ASSERT_EQUAL_UINT32(1024, got.expansion_chain);
    TEST_ASSERT_EQUAL_UINT32(1024, got.expansion_depth);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 20, got.live_runs);
    TEST_ASSERT_EQUAL_UINT32(1024, got.cond_depth);
    TEST_ASSERT_EQUAL_UINT32(256, got.macro_args);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 20, got.macro_text);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 20, got.macros);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 18, got.definitions);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 26, got.macro_bytes);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 28, got.stream_bytes);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 22, got.buffers_per_stream);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 16, got.includes_per_stream);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 29, got.loc_bytes_per_stream);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 30, got.file_cache_bytes);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 27, got.srcman_bytes_per_stream);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 28, ODIN3_SRC_MAX_FILE_BYTES);
}

/* --- include caps -------------------------------------------------------------------------- */

static void lower_include_depth(odin3_pp_limits *limits) {
    limits->include_depth = 2;
}

static void test_include_depth(void) {
    put_text("d3.vh", "x\n");
    put_text("d2.vh", "`include \"d3.vh\"\n");
    put_text("d1.vh", "`include \"d2.vh\"\n");
    put_text("t.v", "`include \"d1.vh\"\n");
    lowered(lower_include_depth);
    assert_stops("t.v", "d2.vh:1:1: error: syntax: include depth exceeds "
                        "ODIN3_PP_MAX_INCLUDE_DEPTH (2)\n"
                        "  included from d1.vh:1\n"
                        "  included from t.v:1");
    TEST_ASSERT_EQUAL_size_t(3, snap.incstack); /* the stream's file and depth 2 */
    TEST_ASSERT_EQUAL_UINT32(3, snap.nincludes);
}

/* An include bomb of empty headers: h<k> includes h<k+1> twice, h<KBOMB> is empty, so the
 * stream would hold 2^(KBOMB+1) - 1 FILE buffers and no output at all. */
enum { KBOMB = 9 };

static void put_bomb(void) {
    char name[PATH_BUF / 4];
    char text[PATH_BUF];
    for (int level = 0; level <= KBOMB; level++) {
        (void)snprintf(name, sizeof name, "h%d.vh", level);
        text[0] = '\0';
        if (level < KBOMB) {
            (void)snprintf(text, sizeof text, "`include \"h%d.vh\"\n`include \"h%d.vh\"\n",
                           level + 1, level + 1);
        }
        put_text(name, text);
    }
    put_text("t.v", "`include \"h0.vh\"\n");
}

static void lower_buffers(odin3_pp_limits *limits) {
    limits->buffers_per_stream = 100;
}

static void test_buffers_per_stream_include_bomb(void) {
    put_bomb();
    lowered(lower_buffers);
    size_t before = sm->bufs.len;
    assert_stops("t.v", "h4.vh:2:1: error: syntax: buffers exceeds "
                        "ODIN3_PP_MAX_BUFFERS_PER_STREAM (100)\n"
                        "  included from h3.vh:2\n"
                        "  included from h2.vh:1\n"
                        "  included from h1.vh:1\n"
                        "  included from h0.vh:1\n"
                        "  included from t.v:1");
    TEST_ASSERT_EQUAL_UINT32(100, snap.nbuffers);
    TEST_ASSERT_EQUAL_size_t(before + 100, snap.sm_bufs);
    TEST_ASSERT_TRUE(snap.out_len < snap.nbuffers); /* the includers' newlines only */
}

static void lower_includes(odin3_pp_limits *limits) {
    limits->includes_per_stream = 5;
}

static void test_includes_per_stream(void) {
    put_bomb();
    lowered(lower_includes);
    assert_stops("t.v", "h3.vh:1:1: error: syntax: files exceeds "
                        "ODIN3_PP_MAX_INCLUDES_PER_STREAM (5)\n"
                        "  included from h2.vh:1\n"
                        "  included from h1.vh:1\n"
                        "  included from h0.vh:1\n"
                        "  included from t.v:1");
    TEST_ASSERT_EQUAL_UINT32(5, snap.nincludes);
    TEST_ASSERT_EQUAL_UINT32(5, snap.nbuffers);
}

static void lower_file_cache(odin3_pp_limits *limits) {
    limits->file_cache_bytes = 24; /* t.v exactly; a.vh would pass it */
}

static void test_file_cache_bytes(void) {
    put_text("a.vh", "abcdef\n");
    put_text("t.v", "wire t;\n`include \"a.vh\"\n");
    lowered(lower_file_cache);
    assert_stops("t.v", "t.v:2:1: error: syntax: file cache bytes exceeds "
                        "ODIN3_PP_MAX_FILE_CACHE_BYTES (24)");
    TEST_ASSERT_EQUAL_UINT64(24, snap.cache_bytes);
    TEST_ASSERT_EQUAL_size_t(1, snap.files_read);
}

/* The source manager's file cap (2^28, AST §3.5) at the root's `at`: nothing is read. */
static void test_source_file_cap_at_root(void) {
    put_sparse("big.v", (off_t)ODIN3_SRC_MAX_FILE_BYTES + 1);
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/big.v", tmp);
    ensure_pp();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("big.v", path, project_word()));
    TEST_ASSERT_EQUAL_STRING("files.f:2:3: error: syntax: file 'big.v' is 268435457 bytes, over "
                             "the limit of 268435456",
                             captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT64(0, snap.cache_bytes);
    TEST_ASSERT_EQUAL_size_t(0, snap.files_read);
    TEST_ASSERT_EQUAL_UINT32(0, snap.nbuffers);
}

/* The same cap at an `include, located at its backtick. */
static void test_source_file_cap_at_include(void) {
    put_sparse("big.vh", (off_t)ODIN3_SRC_MAX_FILE_BYTES + 1);
    put_text("t.v", "wire t;\n  `include \"big.vh\"\n");
    ensure_pp();
    assert_stops("t.v", "t.v:2:3: error: syntax: file 'big.vh' is 268435457 bytes, over the "
                        "limit of 268435456");
    TEST_ASSERT_EQUAL_size_t(1, snap.files_read); /* t.v only */
    TEST_ASSERT_EQUAL_UINT32(1, snap.nbuffers);
}

/* --- conditional cap ----------------------------------------------------------------------- */

static void lower_cond_depth(odin3_pp_limits *limits) {
    limits->cond_depth = 2;
}

static void test_cond_depth(void) {
    put_text("t.v", "`ifdef A\n`else\n`ifndef B\n  `ifdef C\n`endif\n`endif\n`endif\n");
    lowered(lower_cond_depth);
    assert_stops("t.v", "t.v:4:3: error: syntax: conditional depth exceeds "
                        "ODIN3_PP_MAX_COND_DEPTH (2)");
    TEST_ASSERT_EQUAL_size_t(2, snap.conds);
}

/* --- definition caps ----------------------------------------------------------------------- */

static void lower_macro_args(odin3_pp_limits *limits) {
    limits->macro_args = 2;
}

static void test_macro_args(void) {
    put_text("t.v", "`define OK(a,b) a\n`define F(a,b,c) a\n");
    lowered(lower_macro_args);
    assert_stops("t.v", "t.v:2:15: error: syntax: formals of macro 'F' exceeds "
                        "ODIN3_PP_MAX_MACRO_ARGS (2)");
    TEST_ASSERT_EQUAL_size_t(1, snap.definitions); /* OK only: F has no record */
}

static void lower_macro_text(odin3_pp_limits *limits) {
    limits->macro_text = 4;
}

static void test_macro_text(void) {
    put_text("t.v", "`define OK 1234\n`define F 12345\n");
    lowered(lower_macro_text);
    assert_stops("t.v", "t.v:2:11: error: syntax: text of macro 'F' exceeds "
                        "ODIN3_PP_MAX_MACRO_TEXT (4)");
    TEST_ASSERT_EQUAL_size_t(1, snap.definitions);
}

static void lower_macros(odin3_pp_limits *limits) {
    limits->macros = 2;
}

static void test_macros(void) {
    put_text("t.v", "`define A 1\n`define B 2\n`define A 3\n`define C 4\n");
    lowered(lower_macros);
    assert_stops("t.v", "t.v:4:9: error: syntax: macros exceeds ODIN3_PP_MAX_MACROS (2)");
    TEST_ASSERT_EQUAL_size_t(2, snap.live_macros); /* a redefinition takes no new entry */
    TEST_ASSERT_EQUAL_size_t(3, snap.definitions);
}

static void lower_definitions(odin3_pp_limits *limits) {
    limits->definitions = 3;
}

static void test_definitions(void) {
    put_text("t.v", "`define A 1\n`undef A\n`define A 2\n`define A 3\n`define A 4\n");
    lowered(lower_definitions);
    assert_stops("t.v", "t.v:5:9: error: syntax: definitions exceeds "
                        "ODIN3_PP_MAX_DEFINITIONS (3)");
    TEST_ASSERT_EQUAL_size_t(3, snap.definitions);
}

enum { MACRO_BYTES_CAP = 4 * ODIN3_PP_ARENA_CHUNK };

static void lower_macro_bytes(odin3_pp_limits *limits) {
    limits->macro_bytes = MACRO_BYTES_CAP;
}

static void test_macro_bytes(void) {
    static const char k_line[] = "`define a(b,c,d,e,f,g,h,i,j,k) b+c+d+e+f+g+h+i+j+k\n";
    enum { LINES = 4000 };
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (int i = 0; i < LINES; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(k_line)));
    }
    put("t.v", (odin3_bytes){text.data, text.len}, NULL);
    odin3_strbuf_free(&text);
    lowered(lower_macro_bytes);
    assert_stops("t.v", "t.v:482:9: error: syntax: macro bytes exceeds "
                        "ODIN3_PP_MAX_MACRO_BYTES (262144)");
    TEST_ASSERT_TRUE(snap.arena <= MACRO_BYTES_CAP);
    TEST_ASSERT_EQUAL_size_t(481, snap.definitions);
}

/* --- expansion caps ------------------------------------------------------------------------ */

static void lower_chain(odin3_pp_limits *limits) {
    limits->expansion_chain = 20;
    limits->expansion_depth = 2;
}

/* `P(`P) grows the chain while live frames stay few (C1). */
static void test_expansion_chain(void) {
    put_text("t.v", "`define P(x) x(x)\nq = `P(`P);\n");
    lowered(lower_chain);
    /* the located line, then 20 steps: the chain at the cap */
    char want[PP_MSG_MAX];
    int len = snprintf(want, sizeof want,
                       "t.v:2:8: error: syntax: expansion chain exceeds "
                       "ODIN3_PP_MAX_EXPANSION_CHAIN (20)\n"
                       "  in argument of macro 'P' at t.v:1:14");
    for (int i = 0; i < 19; i++) {
        len += snprintf(want + len, sizeof want - (size_t)len,
                        "\n  in argument of macro 'P' at t.v:1:16");
    }
    assert_stops("t.v", want);
    TEST_ASSERT_EQUAL_UINT32(20, snap.max_chain);
    TEST_ASSERT_TRUE(snap.nexpansions <= 2);
}

static void lower_depth(odin3_pp_limits *limits) {
    limits->expansion_depth = 3;
}

static void test_expansion_depth(void) {
    put_text("t.v", "`define A `B a\n`define B `C b\n`define C `D c\n`define D d\nq = `A;\n");
    lowered(lower_depth);
    assert_stops("t.v", "t.v:5:5: error: syntax: expansion depth exceeds "
                        "ODIN3_PP_MAX_EXPANSION_DEPTH (3)\n"
                        "  expanded from macro 'C' at t.v:3:11\n"
                        "  expanded from macro 'B' at t.v:2:11\n"
                        "  expanded from macro 'A' at t.v:1:11");
    TEST_ASSERT_EQUAL_UINT32(3, snap.nexpansions);
}

static void lower_live_runs(odin3_pp_limits *limits) {
    limits->live_runs = 6;
}

static void test_live_runs(void) {
    put_text("t.v", "`define N(x) x\nq = `N(a/**/b/**/c/**/d/**/e);\n");
    lowered(lower_live_runs);
    assert_stops("t.v", "t.v:2:5: error: syntax: live runs exceeds ODIN3_PP_MAX_LIVE_RUNS (6)");
    TEST_ASSERT_TRUE(snap.live_runs <= 6);
}

/* --- per-stream caps ----------------------------------------------------------------------- */

static void lower_stream_bytes(odin3_pp_limits *limits) {
    limits->stream_bytes = 1000;
}

/* An expansion bomb: each level multiplies the text by four. */
static void test_stream_bytes_bomb(void) {
    put_text("t.v", "`define E e e e e\n`define D `E `E `E `E\n`define C `D `D `D `D\n"
                    "`define B `C `C `C `C\n`define A `B `B `B `B\nq = `A;\n");
    lowered(lower_stream_bytes);
    assert_stops("t.v", "t.v:6:5: error: syntax: stream bytes exceeds "
                        "ODIN3_PP_MAX_STREAM_BYTES (1000)\n"
                        "  expanded from macro 'C' at t.v:3:19\n"
                        "  expanded from macro 'B' at t.v:4:20\n"
                        "  expanded from macro 'A' at t.v:5:14");
    TEST_ASSERT_EQUAL_size_t(1000, snap.out_len);
    TEST_ASSERT_TRUE(snap.out_cap <= 1001); /* the text buffer never grows past the cap */
}

/* A stream exactly at the cap fits, its text buffer exactly cap + 1 bytes. */
static void test_stream_bytes_exactly_at_cap(void) {
    put_text("t.v", "wire a;\n");
    create_pp(NULL);
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.stream_bytes = 8;
    odin3_pp_test_set_limits(pp, limits);
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.v", tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_path("t.v", path));
    assert_text("wire a;\n");
    TEST_ASSERT_EQUAL_size_t(9, pp->out.cap);
}

static void lower_loc_bytes(odin3_pp_limits *limits) {
    limits->loc_bytes_per_stream = 4096;
}

/* `G(x) ignores x, and each level doubles its argument: MACRO_ARG buffers that never reach
 * the output (I10c). */
static void test_loc_bytes_unused_arguments(void) {
    put_text("t.v", "`define G(x) .\n"
                    "`define L1(x) `G(x x)\n`define L2(x) `L1(x x)\n`define L3(x) `L2(x x)\n"
                    "`define L4(x) `L3(x x)\n`define L5(x) `L4(x x)\n`define L6(x) `L5(x x)\n"
                    "`define L7(x) `L6(x x)\n`define L8(x) `L7(x x)\n"
                    "q = `L8(abcdefghijklmnopqrstuvwxyz);\n");
    lowered(lower_loc_bytes);
    assert_stops("t.v", "t.v:10:5: error: syntax: location bytes exceeds "
                        "ODIN3_PP_MAX_LOC_BYTES_PER_STREAM (4096)\n"
                        "  expanded from macro 'L3' at t.v:4:15\n"
                        "  expanded from macro 'L4' at t.v:5:15\n"
                        "  expanded from macro 'L5' at t.v:6:15\n"
                        "  expanded from macro 'L6' at t.v:7:15\n"
                        "  expanded from macro 'L7' at t.v:8:15\n"
                        "  expanded from macro 'L8' at t.v:9:15");
    TEST_ASSERT_TRUE(snap.loc_bytes <= 4096);
    TEST_ASSERT_TRUE(snap.out_len < 400); /* the text before the use only */
}

/* --- source-manager records (final review I2) ------------------------------------------------ */

static void lower_srcman(odin3_pp_limits *limits) {
    limits->srcman_bytes_per_stream = 4096;
}

/* A header of newlines inside a false `ifdef, included again and again: no output, but each
 * FILE buffer re-registers 300 line records (1,200 bytes). */
static void test_srcman_bytes_newline_header_bomb(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("`ifdef NOPE")));
    for (int i = 0; i < 300; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("\n")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("`endif")));
    put("nl.vh", (odin3_bytes){text.data, text.len}, NULL);
    odin3_strbuf_free(&text);
    put_text("t.v", "`include \"nl.vh\"`include \"nl.vh\"`include \"nl.vh\"`include \"nl.vh\"\n");
    lowered(lower_srcman);
    size_t before = sm->bufs.len;
    assert_stops("t.v", "t.v:1:49: error: syntax: source-manager bytes exceeds "
                        "ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM (4096)");
    TEST_ASSERT_TRUE(snap.srcman_bytes <= 4096);
    TEST_ASSERT_EQUAL_size_t(before + 4, snap.sm_bufs); /* t.v and three headers, not a fourth */
}

/* A body of empty block comments, each a loc-0 space and so a segment of its own, expanded a
 * few times. */
static void test_srcman_bytes_comment_body_bomb(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("`define S ")));
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("/**/")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&text, odin3_bytes_cstr("x\nq = `S `S `S `S;\n")));
    put("t.v", (odin3_bytes){text.data, text.len}, NULL);
    odin3_strbuf_free(&text);
    lowered(lower_srcman);
    /* located at the third use: a loc-0 space has no location of its own */
    assert_stops("t.v", "t.v:2:11: error: syntax: source-manager bytes exceeds "
                        "ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM (4096)");
    TEST_ASSERT_TRUE(snap.srcman_bytes <= 4096);
    TEST_ASSERT_TRUE((uint64_t)snap.nsegments * 8 <= 4096);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_limits_are_the_spec_values);
    RUN_TEST(test_include_depth);
    RUN_TEST(test_buffers_per_stream_include_bomb);
    RUN_TEST(test_includes_per_stream);
    RUN_TEST(test_file_cache_bytes);
    RUN_TEST(test_source_file_cap_at_root);
    RUN_TEST(test_source_file_cap_at_include);
    RUN_TEST(test_cond_depth);
    RUN_TEST(test_macro_args);
    RUN_TEST(test_macro_text);
    RUN_TEST(test_macros);
    RUN_TEST(test_definitions);
    RUN_TEST(test_macro_bytes);
    RUN_TEST(test_expansion_chain);
    RUN_TEST(test_expansion_depth);
    RUN_TEST(test_live_runs);
    RUN_TEST(test_stream_bytes_bomb);
    RUN_TEST(test_stream_bytes_exactly_at_cap);
    RUN_TEST(test_srcman_bytes_newline_header_bomb);
    RUN_TEST(test_srcman_bytes_comment_body_bomb);
    RUN_TEST(test_loc_bytes_unused_arguments);
    return UNITY_END();
}
