/*
 * test_pp_mixed.c — the preprocessor's features together (2B spec §7, §10.3): argument
 * collection stopped by an `include and by the end of an included file, macros carried from one
 * stream to the next of a compilation unit, an error in a macro argument inside an included file
 * with the whole 2A §3.4 chain, and the conditional and include stacks empty at each stream's
 * end.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "ir/design.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PATH_BUF = PP_PATH_BUF };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

static void put_text(const char *name, const char *text) {
    put(name, odin3_bytes_cstr(text), NULL);
}

/* Runs tmp/name as a stream of the current preprocessor. */
static odin3_status run_file(const char *name) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    return run_path(name, path);
}

/* Nothing of the stream is left on the preprocessor's stacks once it ended (args and scratch
 * are work vectors, cleared at each use, not stacks). */
static void assert_stacks_empty(void) {
    TEST_ASSERT_EQUAL_size_t(0, pp->frames.len);
    TEST_ASSERT_EQUAL_size_t(0, pp->runs.len);
    TEST_ASSERT_EQUAL_size_t(0, pp->conds.len);
    TEST_ASSERT_EQUAL_size_t(0, pp->incstack.len);
    TEST_ASSERT_EQUAL_UINT32(0, pp->nexpansions);
    TEST_ASSERT_EQUAL_UINT32(0, pp->xoff_loc.v);
    TEST_ASSERT_FALSE(pp->skipping);
}

static void assert_ok(const char *name, const char *want) {
    errors = 0;
    odin3_status st = run_file(name);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(captured);
    }
    assert_text(want);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, errors, name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(want), stream.text.len, name);
    assert_stacks_empty();
}

static void assert_stops(const char *name, const char *want) {
    errors = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run_file(name), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, captured, name);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NULL(stream.text.ptr);
    assert_stacks_empty();
}

/* --- collection across files --------------------------------------------------------------- */

/* An `include met while collecting the actuals ends them (PP-5): no file is entered. */
static void test_include_stops_collection(void) {
    put_text("a.vh", "x\n");
    put_text("t.v", "`define D(x) x\nw = `D(1,\n  `include \"a.vh\"\n);\n");
    ensure_pp();
    assert_stops("t.v", "t.v:2:5: error: syntax: unterminated arguments of macro 'D'");
    const uint32_t *paths = NULL;
    TEST_ASSERT_EQUAL_UINT32(1, odin3_pp_files_read(pp, &paths)); /* a.vh never read */
}

/* An included file's end ends the actuals too, with the include chain of the use. */
static void test_included_file_end_stops_collection(void) {
    put_text("use.vh", "w = `D(1,\n");
    put_text("mid.vh", "`include \"use.vh\"\n");
    put_text("t.v", "`define D(a,b) a+b\n`include \"mid.vh\"\n2);\n");
    ensure_pp();
    assert_stops("t.v", "use.vh:1:5: error: syntax: unterminated arguments of macro 'D'\n"
                        "  included from mid.vh:1\n"
                        "  included from t.v:2");
}

/* A complete use inside an included file still expands, its actual spelled in that file. */
static void test_use_complete_inside_included_file(void) {
    put_text("use.vh", "w = `D(1,\n2);\n");
    put_text("t.v", "`define D(a,b) a+b\n`include \"use.vh\"\nz\n");
    ensure_pp();
    assert_ok("t.v", "\nw = 1+2;\n\nz\n");
}

/* --- one compilation unit ------------------------------------------------------------------ */

/* Macros defined in one stream (in its file or a header it includes) are used in the next;
 * an `undef in the second stream holds in the third. */
static void test_macros_carry_across_streams(void) {
    put_text("defs.vh", "`define W 8\n`define ADD(a,b) ((a)+(b))\n");
    put_text("a.v", "`include \"defs.vh\"\n`define LOCAL 1\nwire a;\n");
    put_text("b.v", "wire [`W-1:0] b = `ADD(`LOCAL, 2);\n`undef LOCAL\n");
    put_text("c.v", "`ifdef LOCAL\nyes\n`else\nno\n`endif\n`ifdef W\nw `W\n`endif\n");
    ensure_pp();
    assert_ok("a.v", "\n\n\n\nwire a;\n");
    assert_ok("b.v", "wire [8-1:0] b = ((1)+(2));\n\n");
    assert_ok("c.v", "\nno\n\n\nw 8\n\n");
    const uint32_t *paths = NULL;
    TEST_ASSERT_EQUAL_UINT32(4, odin3_pp_files_read(pp, &paths));
}

/* A header included by two streams is read once and registered twice; the second stream may
 * include the first stream's own file without a cycle (the include stack is per stream). */
static void test_header_shared_by_streams(void) {
    put_text("h.vh", "`ifndef H_VH\n`define H_VH\nh\n`endif\n");
    put_text("a.v", "`include \"h.vh\"\na\n");
    put_text("b.v", "`include \"h.vh\"\n`include \"a.v\"\nb\n");
    ensure_pp();
    assert_ok("a.v", "\n\nh\n\n\na\n");
    assert_ok("b.v", "\n\n\n\na\n\nb\n");
    const uint32_t *paths = NULL;
    TEST_ASSERT_EQUAL_UINT32(3, odin3_pp_files_read(pp, &paths));
}

/* A failed stream leaves nothing open for the next: its macros up to the error stay defined,
 * its open conditional does not. */
static void test_failed_stream_then_next(void) {
    put_text("a.v", "`define M m\n`ifdef M\n");
    put_text("b.v", "`M\n`endif\n");
    put_text("c.v", "`M\n");
    ensure_pp();
    assert_stops("a.v", "a.v:2:1: error: syntax: unterminated `ifdef");
    assert_stops("b.v", "b.v:2:1: error: syntax: `endif without `ifdef");
    assert_ok("c.v", "m\n");
}

/* --- diagnostics through every layer ------------------------------------------------------- */

/* An undefined macro in an actual of G, passed on to F, used in a file included twice deep:
 * the message carries the 2A §3.4 chain (each argument step, then each include). */
static void test_error_in_argument_inside_included_file(void) {
    put_text("defs.vh", "`define F(a) [a]\n`define G(b) `F(b)\n");
    put_text("use.vh", "wire w =\n  `G(1 + `NOPE);\n");
    put_text("mid.vh", "`include \"use.vh\"\n");
    put_text("t.v", "`include \"defs.vh\"\n`include \"mid.vh\"\n");
    ensure_pp();
    assert_stops("t.v", "use.vh:2:10: error: syntax: undefined macro 'NOPE'\n"
                        "  in argument of macro 'F' at defs.vh:1:15\n"
                        "  in argument of macro 'G' at defs.vh:2:17\n"
                        "  included from mid.vh:1\n"
                        "  included from t.v:2");
}

/* The same chain for a conditional misuse is just the include levels. */
static void test_error_in_included_conditional(void) {
    put_text("bad.vh", "`ifdef A\n`else\n`else\n`endif\n");
    put_text("t.v", "\n`include \"bad.vh\"\n");
    ensure_pp();
    assert_stops("t.v", "bad.vh:3:1: error: syntax: second `else\n"
                        "  included from t.v:2");
}

/* --- stacks at each stream's end ----------------------------------------------------------- */

/* Conditionals, includes, translate_off and expansions all close in each stream. */
static void test_stacks_empty_after_each_stream(void) {
    put_text("i1.vh", "`ifdef A\n`include \"i2.vh\"\n`endif\n");
    put_text("i2.vh",
             "`define B(x) <x>\n// synopsys translate_off\nhidden\n// synopsys translate_on\n");
    put_text("a.v", "`include \"i1.vh\"\n`B(`B(1))\n");
    put_text("b.v", "`ifndef B\nno\n`elsif A\n`include \"i1.vh\"\n`else\nnot\n`endif\n`B(2)\n");
    odin3_pp_define defs[1] = {{odin3_bytes_cstr("A"), {NULL, 0}, false}};
    odin3_pp_config config = {defs, 1, NULL, 0};
    create_pp(&config);
    assert_ok("a.v", "\n\n\n\n\n\n<<1>>\n");
    assert_ok("b.v", "\n\n\n\n\n\n\n\n<2>\n");
}

/* A translate_off region opened in an included file inside an `ifdef hides that file's `endif
 * (spec §7.6: the region is opaque; documented, not a target of support). */
static void test_translate_off_hides_endif_across_include(void) {
    put_text("i.vh", "`ifdef A\n// synopsys translate_off\n`endif\n");
    put_text("t.v", "`include \"i.vh\"\n// synopsys translate_on\n");
    odin3_pp_define defs[1] = {{odin3_bytes_cstr("A"), {NULL, 0}, false}};
    odin3_pp_config config = {defs, 1, NULL, 0};
    create_pp(&config);
    assert_stops("t.v", "i.vh:1:1: error: syntax: unterminated `ifdef\n"
                        "  included from t.v:1");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_include_stops_collection);
    RUN_TEST(test_included_file_end_stops_collection);
    RUN_TEST(test_use_complete_inside_included_file);
    RUN_TEST(test_macros_carry_across_streams);
    RUN_TEST(test_header_shared_by_streams);
    RUN_TEST(test_failed_stream_then_next);
    RUN_TEST(test_error_in_argument_inside_included_file);
    RUN_TEST(test_error_in_included_conditional);
    RUN_TEST(test_stacks_empty_after_each_stream);
    RUN_TEST(test_translate_off_hides_endif_across_include);
    return UNITY_END();
}
