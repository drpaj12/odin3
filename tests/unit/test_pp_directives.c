/*
 * test_pp_directives.c — unit tests for the preprocessor's scanner and output (2B spec §7.2,
 * §7.5, §7.8): identity with one segment, comments, strings and escaped identifiers copied
 * through, the L18–L22 pass-through directives, `line, the PP-9 operators, stray backticks, the
 * segment rule, the root file's errors, files_read, the stream cap and an OOM sweep.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PATH_BUF = PP_PATH_BUF, MSG_MAX = PP_MSG_MAX, OOM_LIMIT = 4000 };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

/* Runs text as a fresh file (the cache keeps every path it read) given as "t.v". */
static odin3_status run_bytes(odin3_bytes text) {
    return run_fresh("t.v", text);
}

static odin3_status run(const char *text) {
    return run_bytes(odin3_bytes_cstr(text));
}

/* The output equals the file and the map is one segment at the file's first byte. */
static void assert_identity(const char *text) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(text), text);
    assert_text(text);
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count, text);
    TEST_ASSERT_EQUAL_UINT32(1, stream.nsegments);
    TEST_ASSERT_EQUAL_UINT32(0, segs[0].out_offset);
    TEST_ASSERT_EQUAL_UINT32(at_offset(0), segs[0].loc);
}

static void assert_error(const char *text, const char *want) {
    errors = 0;
    captured[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run(text), text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, captured, text);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NULL(stream.text.ptr);
}

/* --- identity ------------------------------------------------------------------------------ */

static void test_file_without_backticks_is_itself(void) {
    assert_identity("module m(input a, output y);\n  assign y = ~a;\nendmodule\n");
    assert_identity("no newline at the end");
    assert_identity("crlf\r\nlines\r\n");
}

static void test_empty_file(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(""));
    TEST_ASSERT_EQUAL_size_t(0, stream.text.len);
    TEST_ASSERT_EQUAL_UINT32(0, stream.nsegments);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, stream.stream.v);
}

static void test_comments_strings_escaped_identifiers_copied(void) {
    assert_identity("// `define X 1 `__FILE__ ` \nwire a; /* `undef ` `` */ wire b;\n");
    assert_identity("/* multi\n `line x\n line */ wire c;\n");
    assert_identity("wire [7:0] s = \"`x \\\" `` `\\`\\\" `\";\n");
    assert_identity("wire \\a`b ; wire \\`c// ;\n");
    assert_identity("a = b / c; d = e /c; // done\n");
    assert_identity("\"unterminated string ` stops at the newline\n");
    assert_identity("/* unterminated comment `\n");
}

static void test_nul_bytes_copied(void) {
    static const char text[] = "wire\0a;\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_bytes((odin3_bytes){text, sizeof text - 1}));
    TEST_ASSERT_EQUAL_size_t(sizeof text - 1, stream.text.len);
    TEST_ASSERT_EQUAL_MEMORY(text, stream.text.ptr, sizeof text - 1);
}

/* --- pass-through (PP-8, L18–L22) ---------------------------------------------------------- */

static void test_pass_through_directives_verbatim(void) {
    static const char *const cases[] = {
        "`default_nettype none\nmodule m; endmodule\n",
        "`timescale 1ns / 1ps\n",
        "`celldefine\nmodule c; endmodule\n`endcelldefine\n",
        "`resetall\n",
        "`begin_keywords \"1364-2005\"\nmodule k; endmodule\n`end_keywords\n",
        "`unconnected_drive pull1\nmodule u; endmodule\n`nounconnected_drive\n",
        "`pragma protect begin\n",
        "module m; `default_nettype wire endmodule",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        assert_identity(cases[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --- `line --------------------------------------------------------------------------------- */

static void test_line_is_dropped(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`line 3 \"x\" 0\nwire a;\n"));
    assert_text("\nwire a;\n");
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_UINT32(at_offset(13), segs[0].loc); /* the newline after the directive */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("wire `line 12\t\"dir/f.v\"  2 b;\n"));
    assert_text("wire  b;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("a`line 1 \"f\" 1\nb"));
    assert_text("a\nb");
}

static void test_line_errors(void) {
    static const char want_fmt[] = "t.v:1:%d: error: syntax: malformed `line directive (expected "
                                   "`line <number> \"<file>\" <0, 1 or 2>)";
    static const struct {
        const char *text;
        int col;
    } cases[] = {
        {"`line x\n", 7},
        {"`line\n", 6},
        {"`line 3\n", 8},
        {"`line 3 x\n", 9},
        {"`line 3 \"x\n", 9},
        {"`line 3 \"x\"\n", 12},
        {"`line 3 \"x\" 3\n", 13},
        {"`line 3 \"x\" 01\n", 13},
        {"`line 3 \"x\" 1a\n", 13},
        {"`line 3x \"f\" 1\n", 7},
        {"`line 99999999999 \"x\" 1\n", 7},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char want[MSG_MAX];
        (void)snprintf(want, sizeof want, want_fmt, cases[i].col);
        assert_error(cases[i].text, want);
    }
}

/* --- SystemVerilog operators (PP-9) and stray backticks ------------------------------------ */

static void test_sv_operators_in_text(void) {
    static const char tail[] = " is a SystemVerilog macro operator; not Verilog-2005 (Phase 5)";
    static const struct {
        const char *text;
        const char *op;
        int col;
    } cases[] = {
        {"wire a``b;\n", "``", 7},
        {"x = `\"s`\";\n", "`\"", 5},
        {"x = `\\`\"s`\\`\";\n", "`\\`\"", 5},
        {"x = `__FILE__;\n", "`__FILE__", 5},
        {"x = `__LINE__;\n", "`__LINE__", 5},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char want[MSG_MAX];
        (void)snprintf(want, sizeof want, "t.v:1:%d: error: syntax: '%s'%s", cases[i].col,
                       cases[i].op, tail);
        assert_error(cases[i].text, want);
    }
}

static void test_stray_backticks(void) {
    assert_error("wire ` a;\n", "t.v:1:6: error: syntax: stray backtick");
    assert_error("x = `8;\n", "t.v:1:5: error: syntax: stray backtick");
    assert_error("x = `\n", "t.v:1:5: error: syntax: stray backtick");
    assert_error("x = `", "t.v:1:5: error: syntax: stray backtick");
    assert_error("x = `(a);\n", "t.v:1:5: error: syntax: stray backtick");
    assert_error("x = `\\y;\n", "t.v:1:5: error: syntax: stray backtick");
    assert_error("\n\tx = `\t;\n", "t.v:2:6: error: syntax: stray backtick");
}

/* --- segments ------------------------------------------------------------------------------ */

static void test_segments_around_consumed_directives(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("a `undef X b\n`define Y 1\nc\n"));
    assert_text("a  b\n\nc\n");
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT32(0, segs[0].out_offset);
    TEST_ASSERT_EQUAL_UINT32(at_offset(0), segs[0].loc);
    TEST_ASSERT_EQUAL_UINT32(2, segs[1].out_offset);
    TEST_ASSERT_EQUAL_UINT32(at_offset(10), segs[1].loc);
    TEST_ASSERT_EQUAL_UINT32(5, segs[2].out_offset);
    TEST_ASSERT_EQUAL_UINT32(at_offset(24), segs[2].loc);
    TEST_ASSERT_EQUAL_UINT32(3, stream.nsegments);
}

static void test_protective_space_after_consumed_directive(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("a`undef X\nb`undef X`undef Y c\n"));
    assert_text("a\nb c\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("a`line 1 \"f\" 0(b)\n"));
    assert_text("a (b)\n");
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT32(1, segs[1].out_offset);
    TEST_ASSERT_EQUAL_UINT32(0, segs[1].loc); /* the inserted space is a loc-0 segment */
    TEST_ASSERT_EQUAL_UINT32(2, segs[2].out_offset);
    TEST_ASSERT_EQUAL_UINT32(at_offset(14), segs[2].loc);
}

/* --- the root file ------------------------------------------------------------------------- */

/* A project word's loc: "files.f" line 2, column 3. */
static odin3_loc project_word(void) {
    odin3_srcfile_spec spec = {id("files.f"), 0, 0, 0, 16, {0, 0}};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, 8));
    return odin3_srcman_loc(sm, buf, 10);
}

static void test_root_missing_is_located(void) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/nope.v", tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("src/nope.v", path, project_word()));
    TEST_ASSERT_EQUAL_STRING("files.f:2:3: error: missing_file: 'src/nope.v'", captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT32(0, stream.stream.v);
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("src/nope.v", path, (odin3_loc){0}));
    TEST_ASSERT_EQUAL_STRING("odin3: error: missing_file: 'src/nope.v'", captured);
}

/* PRJ-6: a path of the wrong kind is missing. */
static void test_root_directory_is_missing(void) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/dir.v", tmp);
    TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0700));
    remember(path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("dir.v", path, project_word()));
    TEST_ASSERT_EQUAL_STRING("files.f:2:3: error: missing_file: 'dir.v'", captured);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_pp_files_read(pp, &(const uint32_t *){NULL}));
}

static void test_files_read_and_cache(void) {
    char path_a[PATH_BUF];
    char path_b[PATH_BUF];
    put("a.v", odin3_bytes_cstr("wire a;\n"), path_a);
    put("b.v", odin3_bytes_cstr("wire b;\n"), path_b);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_at("a.v", path_a, (odin3_loc){0}));
    odin3_srcbuf_id first = stream.stream;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_at("b.v", path_b, (odin3_loc){0}));
    put("a.v", odin3_bytes_cstr("changed on disk\n"), path_a);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_at("a.v", path_a, (odin3_loc){0}));
    assert_text("wire a;\n");                               /* read once, from the cache */
    TEST_ASSERT_NOT_EQUAL_UINT32(first.v, stream.stream.v); /* registered twice */
    const uint32_t *paths = NULL;
    TEST_ASSERT_EQUAL_UINT32(2, odin3_pp_files_read(pp, &paths));
    TEST_ASSERT_EQUAL_UINT32(id(path_a), paths[0]);
    TEST_ASSERT_EQUAL_UINT32(id(path_b), paths[1]);
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, stream.stream, &info));
    TEST_ASSERT_EQUAL_UINT32(id("a.v"), info.name);
    TEST_ASSERT_EQUAL_UINT32(id(path_a), info.resolved);
    TEST_ASSERT_EQUAL_UINT32(id("work"), info.library);
    TEST_ASSERT_EQUAL_UINT32(0, info.parent);
    TEST_ASSERT_EQUAL_UINT32(2, info.lines);
}

static void test_run_misuse(void) {
    ensure_pp();
    odin3_pp_source src = {id("x.v"), id("/x.v"), id("work"), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_run(NULL, &src, &stream));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_run(pp, NULL, &stream));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_run(pp, &src, NULL));
}

/* --- caps ---------------------------------------------------------------------------------- */

static void test_cap_stream_bytes(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.stream_bytes = 8;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("12345678"));
    assert_error("1234\n`define X\n6789", "t.v:3:3: error: syntax: stream bytes exceeds "
                                          "ODIN3_PP_MAX_STREAM_BYTES (8)");
}

static void test_cap_buffers_per_stream(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.buffers_per_stream = 0;
    odin3_pp_test_set_limits(pp, limits);
    char path[PATH_BUF];
    put("t.v", odin3_bytes_cstr("wire a;\n"), path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("t.v", path, project_word()));
    TEST_ASSERT_EQUAL_STRING("files.f:2:3: error: syntax: buffers exceeds "
                             "ODIN3_PP_MAX_BUFFERS_PER_STREAM (0)",
                             captured);
}

typedef struct root_cap {
    odin3_pp_limits limits;
    const char *want;
} root_cap;

/* The stream's own file counts towards the per-stream caps. */
static void test_root_counts_towards_stream_caps(void) {
    root_cap cases[] = {
        {odin3_pp_default_limits(), "files.f:2:3: error: syntax: files exceeds "
                                    "ODIN3_PP_MAX_INCLUDES_PER_STREAM (0)"},
        {odin3_pp_default_limits(), "files.f:2:3: error: syntax: location bytes exceeds "
                                    "ODIN3_PP_MAX_LOC_BYTES_PER_STREAM (8)"},
        {odin3_pp_default_limits(), "files.f:2:3: error: syntax: live runs exceeds "
                                    "ODIN3_PP_MAX_LIVE_RUNS (0)"},
    };
    cases[0].limits.includes_per_stream = 0;
    cases[1].limits.loc_bytes_per_stream = 8; /* the 8-byte file reserves 9 */
    cases[2].limits.live_runs = 0;
    char path[PATH_BUF];
    put("root.v", odin3_bytes_cstr("wire a;\n"), path);
    odin3_loc word = project_word();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ensure_pp();
        odin3_pp_test_set_limits(pp, cases[i].limits);
        errors = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("root.v", path, word));
        TEST_ASSERT_EQUAL_STRING(cases[i].want, captured);
        TEST_ASSERT_EQUAL_INT(1, errors);
    }
    odin3_pp_limits fits = odin3_pp_default_limits();
    fits.loc_bytes_per_stream = 9;
    odin3_pp_test_set_limits(pp, fits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_at("root.v", path, word));
}

static void test_cap_file_cache_bytes(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.file_cache_bytes = 10;
    odin3_pp_test_set_limits(pp, limits);
    char path_a[PATH_BUF];
    char path_b[PATH_BUF];
    put("a.v", odin3_bytes_cstr("wire a;\n"), path_a);
    put("b.v", odin3_bytes_cstr("wire b;\n"), path_b);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_at("a.v", path_a, (odin3_loc){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_at("b.v", path_b, project_word()));
    TEST_ASSERT_EQUAL_STRING("files.f:2:3: error: syntax: file cache bytes exceeds "
                             "ODIN3_PP_MAX_FILE_CACHE_BYTES (10)",
                             captured);
}

/* --- out of memory ------------------------------------------------------------------------- */

static const char k_oom_text[] = "`timescale 1ns/1ps\n`define W 8 // width\nmodule m;\n"
                                 "  wire a; `undef W /* c */ wire b;\n`line 5 \"m.v\" 0\n"
                                 "  `default_nettype none\nendmodule\n";
static const char k_oom_want[] = "`timescale 1ns/1ps\n\nmodule m;\n  wire a;  /* c */ wire b;\n"
                                 "\n  `default_nettype none\nendmodule\n";

static bool oom_try(long fail_after, const char *path) {
    odin3_pp_destroy(pp);
    pp = NULL;
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_pp_config none = {NULL, 0, NULL, 0};
    odin3_pp_source src = {id("t.v"), id(path), id("work"), {0}};
    memset(&stream, 0, sizeof stream);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_pp_create(design, &none, &pp);
    if (st == ODIN3_OK) {
        st = odin3_pp_run(pp, &src, &stream);
    }
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(stream.text.ptr);
        return false;
    }
    assert_text(k_oom_want);
    return true;
}

static void test_out_of_memory(void) {
    char path[PATH_BUF];
    put("t.v", odin3_bytes_cstr(k_oom_text), path);
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(k, path);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 20);
    TEST_ASSERT_EQUAL_INT(0, errors);
    /* a run after a failed run starts clean */
    odin3_util_set_alloc_fail_after(30);
    odin3_pp_source src = {id("t.v"), id(path), id("work"), {0}};
    odin3_status st = odin3_pp_run(pp, &src, &stream);
    odin3_util_set_alloc_fail_after(-1);
    (void)st;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_run(pp, &src, &stream));
    assert_text(k_oom_want);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_file_without_backticks_is_itself);
    RUN_TEST(test_empty_file);
    RUN_TEST(test_comments_strings_escaped_identifiers_copied);
    RUN_TEST(test_nul_bytes_copied);
    RUN_TEST(test_pass_through_directives_verbatim);
    RUN_TEST(test_line_is_dropped);
    RUN_TEST(test_line_errors);
    RUN_TEST(test_sv_operators_in_text);
    RUN_TEST(test_stray_backticks);
    RUN_TEST(test_segments_around_consumed_directives);
    RUN_TEST(test_protective_space_after_consumed_directive);
    RUN_TEST(test_root_missing_is_located);
    RUN_TEST(test_root_directory_is_missing);
    RUN_TEST(test_files_read_and_cache);
    RUN_TEST(test_run_misuse);
    RUN_TEST(test_cap_stream_bytes);
    RUN_TEST(test_cap_buffers_per_stream);
    RUN_TEST(test_root_counts_towards_stream_caps);
    RUN_TEST(test_cap_file_cache_bytes);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
