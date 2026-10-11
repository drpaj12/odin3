/*
 * test_pp_expand.c — unit tests for macro expansion (2B spec §7.4, PP-5..PP-7): the 2A §3.1
 * buffers and segments exactly, argument runs, comments in arguments, cross-frame collection,
 * defaults and empty actuals, recursion on the spelling ancestry, the expansion caps, the
 * reviewer probes, and an OOM sweep.
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
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_PP_PROBES
#error "ODIN3_PP_PROBES must name tests/unit/data/pp/probes"
#endif

enum { PATH_BUF = PP_PATH_BUF, MSG_MAX = PP_MSG_MAX, OOM_LIMIT = 6000 };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

static const char *source; /* the text of the last run() */

/* Runs text as a fresh file (the cache keeps every path it read) given as "f.v". */
static odin3_status run(const char *text) {
    source = text;
    return run_fresh("f.v", odin3_bytes_cstr(text));
}

static odin3_status run_probe(const char *probe) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_PP_PROBES, probe);
    source = NULL;
    return run_path(probe, path);
}

static void assert_out(const char *want) {
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_NOT_NULL(stream.text.ptr);
    char got[MSG_MAX];
    (void)snprintf(got, sizeof got, "%.*s", (int)stream.text.len, (const char *)stream.text.ptr);
    TEST_ASSERT_EQUAL_STRING(want, got);
    TEST_ASSERT_EQUAL_size_t(strlen(want), stream.text.len);
}

static void assert_ok_text(odin3_status st, const char *want) {
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(captured);
    }
    assert_out(want);
}

static void assert_error(odin3_status st, const char *want) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(want, captured);
    TEST_ASSERT_NULL(stream.text.ptr);
}

/* The loc of "line:col" of the last run()'s file. */
static odin3_loc file_loc(const char *line_col) {
    TEST_ASSERT_NOT_NULL(source);
    char *colon = NULL;
    unsigned long line = strtoul(line_col, &colon, 10);
    TEST_ASSERT_EQUAL_CHAR(':', *colon);
    unsigned long col = strtoul(colon + 1, NULL, 10);
    uint32_t offset = 0;
    for (unsigned long i = 1; i < line; i++) {
        const char *newline = strchr(source + offset, '\n');
        TEST_ASSERT_NOT_NULL(newline);
        offset = (uint32_t)(newline - source) + 1;
    }
    return odin3_srcman_loc(sm, stream.stream, offset + (uint32_t)col - 1);
}

/* The n-th buffer the run made after the stream's own FILE buffer (1 = the first). */
static odin3_srcbuf_id nth(uint32_t n) {
    return (odin3_srcbuf_id){stream.stream.v + n};
}

/* loc of offset off of the n-th buffer of the run. */
static odin3_loc in(uint32_t nth_buf, uint32_t off) {
    odin3_loc loc = odin3_srcman_loc(sm, nth(nth_buf), off);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, loc.v);
    return loc;
}

static uint32_t buffers_made(void) {
    return (uint32_t)pp->bufs.len;
}

typedef struct buf_want {
    uint32_t kind;
    const char *name;
    uint32_t len;
    odin3_loc def, parent, parent_end;
} buf_want;

static void assert_buf(uint32_t n, buf_want want) {
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, nth(n), &info));
    TEST_ASSERT_EQUAL_UINT32(want.kind, info.kind);
    TEST_ASSERT_EQUAL_STRING(want.name, odin3_strtab_get(odin3_design_strtab(design), info.name));
    TEST_ASSERT_EQUAL_UINT32(want.len, info.len);
    TEST_ASSERT_EQUAL_UINT32(want.def.v, info.def);
    TEST_ASSERT_EQUAL_UINT32(want.parent.v, info.parent);
    TEST_ASSERT_EQUAL_UINT32(want.parent_end.v, info.parent_end);
}

static const odin3_srcfile_rec *stream_file(void) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, odin3_srcman_loc(sm, stream.stream, 0));
    TEST_ASSERT_NOT_NULL(rec);
    return odin3_srcman_file_of(sm, rec);
}

static uint32_t nsegs(void) {
    return (uint32_t)stream_file()->segs.len;
}

typedef struct seg_want {
    const char *text;
    odin3_loc loc;
} seg_want;

/* Segments first .. first + n - 1 have the given texts and locs. */
static void assert_segs(uint32_t first, const seg_want *want, uint32_t n) {
    const odin3_srcfile_rec *file = stream_file();
    TEST_ASSERT_TRUE(first + n <= file->segs.len);
    for (uint32_t i = 0; i < n; i++) {
        const odin3_srcseg_rec *seg = odin3_vec_cat(&file->segs, first + i);
        uint32_t end =
            first + i + 1 < file->segs.len
                ? ((const odin3_srcseg_rec *)odin3_vec_cat(&file->segs, first + i + 1))->out_offset
                : (uint32_t)stream.text.len;
        char got[MSG_MAX];
        (void)snprintf(got, sizeof got, "%.*s", (int)(end - seg->out_offset),
                       (const char *)stream.text.ptr + seg->out_offset);
        TEST_ASSERT_EQUAL_STRING(want[i].text, got);
        TEST_ASSERT_EQUAL_UINT32(want[i].loc.v, seg->loc);
    }
}

static odin3_loc none(void) {
    return (odin3_loc){0};
}

/* --- the 2A §3.1 worked example ------------------------------------------------------------ */

static const char k_outer[] = "`define INNER(p) (p + 1)\n"
                              "`define OUTER(q) `INNER(q * 2)\n"
                              "assign y = `OUTER(xx);\n";

static void test_2a_example_buffers(void) {
    assert_ok_text(run(k_outer), "\n\nassign y = (xx * 2 + 1);\n");
    TEST_ASSERT_EQUAL_UINT32(6, buffers_made()); /* f.v, E1, A1, E2, A2, A3 */
    /* E1 = EXPANSION of OUTER */
    assert_buf(1, (buf_want){ODIN3_SRCBUF_EXPANSION, "OUTER", 13, file_loc("2:18"),
                             file_loc("3:12"), file_loc("3:22")});
    /* A1 = MACRO_ARG q <- xx */
    assert_buf(
        2, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "OUTER", 2, file_loc("3:19"), in(1, 7), in(1, 8)});
    /* E2 = EXPANSION of INNER */
    assert_buf(
        3, (buf_want){ODIN3_SRCBUF_EXPANSION, "INNER", 7, file_loc("1:18"), in(1, 0), in(1, 13)});
    /* A2 = xx, A3 = " * 2" */
    assert_buf(4, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "INNER", 2, in(2, 0), in(3, 1), in(3, 2)});
    assert_buf(5, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "INNER", 4, in(1, 8), in(3, 1), in(3, 2)});
}

static void test_2a_example_segments(void) {
    assert_ok_text(run(k_outer), "\n\nassign y = (xx * 2 + 1);\n");
    seg_want want[] = {
        {"\n", file_loc("1:25")},  {"\nassign y = ", file_loc("2:31")},
        {"(", in(3, 0)},           {"xx", in(4, 0)},
        {" * 2", in(5, 0)},        {" + 1)", in(3, 2)},
        {";\n", file_loc("3:22")},
    };
    TEST_ASSERT_EQUAL_UINT32(7, nsegs());
    TEST_ASSERT_EQUAL_UINT32(7, stream.nsegments);
    assert_segs(0, want, 7);
    /* the 2A §3.2 decoding of the example */
    TEST_ASSERT_EQUAL_UINT32(in(2, 0).v, odin3_srcman_spelling(sm, in(4, 0)).v);
    TEST_ASSERT_EQUAL_UINT32(in(1, 9).v, odin3_srcman_spelling(sm, in(5, 1)).v);
    TEST_ASSERT_EQUAL_UINT32(file_loc("3:12").v, odin3_srcman_expansion_loc(sm, in(5, 1)).v);
    TEST_ASSERT_EQUAL_UINT32(file_loc("3:19").v, odin3_srcman_file_loc(sm, in(4, 0)).v);
}

static void test_2a_example_chain(void) {
    assert_ok_text(run(k_outer), "\n\nassign y = (xx * 2 + 1);\n");
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format(sm, in(5, 1), &out, ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format_chain(sm, in(5, 1), &out));
    TEST_ASSERT_EQUAL_STRING("f.v:3:12\n  in argument of macro 'INNER' at f.v:1:19\n"
                             "  expanded from macro 'OUTER' at f.v:2:27",
                             out.data);
    odin3_strbuf_free(&out);
}

/* 2A §3.2: `assign `OUT = a;` — the expansion's parent_end is one past the name. */
static void test_2a_assign_out(void) {
    assert_ok_text(run("`define OUT y\nassign `OUT = a;\n"), "\nassign y = a;\n");
    assert_buf(1, (buf_want){ODIN3_SRCBUF_EXPANSION, "OUT", 1, file_loc("1:13"), file_loc("2:8"),
                             file_loc("2:12")});
    seg_want want[] = {
        {"\nassign ", file_loc("1:14")}, {"y", in(1, 0)}, {" = a;\n", file_loc("2:12")}};
    assert_segs(0, want, 3);
}

/* --- PP-5 uses ----------------------------------------------------------------------------- */

static void test_glued_uses_get_no_space(void) {
    assert_ok_text(run("`define S _y\n`define W ff\nwire x`S = 8'h`W;\n"),
                   "\n\nwire x_y = 8'hff;\n");
    seg_want want[] = {{"\n", file_loc("1:13")}, {"\nwire x", file_loc("2:13")},
                       {"_y", in(1, 0)},         {" = 8'h", file_loc("3:9")},
                       {"ff", in(2, 0)},         {";\n", file_loc("3:17")}};
    assert_segs(0, want, 6);
}

static void test_no_formals_ignores_parens(void) {
    assert_ok_text(run("`define A 7\nx = `A(1);\n"), "\nx = 7(1);\n");
}

static void test_takes_arguments(void) {
    assert_error(run("`define F(x) x\nwire w = `F;\n"),
                 "f.v:2:10: error: syntax: macro 'F' takes arguments");
}

static void test_takes_arguments_at_end_of_file(void) {
    assert_error(run("`define F(x) x\n`F"), "f.v:2:1: error: syntax: macro 'F' takes arguments");
}

static void test_paren_after_blanks_and_newlines(void) {
    assert_ok_text(run("`define F(x) [x]\nw = `F \t\n\n (a);\n"), "\nw = [a];\n");
}

static void test_comment_before_paren_is_not_blank(void) {
    assert_error(run("`define F(x) [x]\nw = `F /* c */ (a);\n"),
                 "f.v:2:5: error: syntax: macro 'F' takes arguments");
}

static void test_undefined_macro(void) {
    assert_error(run("wire w = `NOPE;\n"), "f.v:1:10: error: syntax: undefined macro 'NOPE'");
}

static void test_addition_num_is_one_name(void) {
    assert_error(run("`define ADDITION(_num) `ADDITION_num + 1\nx = `ADDITION(2);\n"),
                 "f.v:2:5: error: syntax: undefined macro 'ADDITION_num'\n"
                 "  expanded from macro 'ADDITION' at f.v:1:24");
}

static void test_actuals_nest_and_trim(void) {
    assert_ok_text(run("`define C(x,y) <x|y>\nq = `C( {a,b} , f(c,d)[1,2] );\n"),
                   "\nq = <{a,b}|f(c,d)[1,2]>;\n");
}

static void test_strings_in_actuals_are_opaque(void) {
    assert_ok_text(run("`define C(x,y) <x|y>\nq = `C(\"a,)\\\",\", b);\n"),
                   "\nq = <\"a,)\\\",\"|b>;\n");
}

static void test_escaped_identifier_in_actual_is_opaque(void) {
    assert_ok_text(run("`define C(x) <x>\nq = `C(\\a,b );\n"), "\nq = <\\a,b>;\n");
}

static void test_newlines_kept_in_actuals(void) {
    assert_ok_text(run("`define C(x,y) <x|y>\nq = `C(a\n+\nb,\nc);\n"), "\nq = <a\n+\nb|c>;\n");
}

/* I4: a comment in an argument becomes one loc-0 space, its own segment, and no MACRO_ARG. */
static void test_comment_in_argument(void) {
    assert_ok_text(run("`define N(x) x\nw = `N(p /* q */ + r);\n"), "\nw = p   + r;\n");
    TEST_ASSERT_EQUAL_UINT32(4, buffers_made()); /* f.v, E, two MACRO_ARGs */
    assert_buf(2, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "N", 2, file_loc("2:8"), in(1, 0), in(1, 1)});
    assert_buf(3, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "N", 4, file_loc("2:17"), in(1, 0), in(1, 1)});
    seg_want want[] = {
        {"p ", in(2, 0)}, {" ", none()}, {" + r", in(3, 0)}, {";\n", file_loc("2:22")}};
    assert_segs(1, want, 4);
}

static void test_line_comment_in_argument(void) {
    assert_ok_text(run("`define N(x) [x]\nw = `N(a // c, d\n);\n"), "\nw = [a];\n");
}

static void test_comment_only_argument(void) {
    assert_ok_text(run("`define N(x) [x]\nw = `N(/* c */);\n"), "\nw = [];\n");
    TEST_ASSERT_EQUAL_UINT32(2, buffers_made());
}

static void test_too_many_actuals(void) {
    assert_error(run("`define D(x,y) {x y}\nw = `D(1,2,3);\n"),
                 "f.v:2:5: error: syntax: macro 'D' expects 2 arguments, got 3");
}

static void test_unterminated_arguments(void) {
    assert_error(run("`define D(x) x\nw = `D(1,\n(2);\n"),
                 "f.v:2:5: error: syntax: unterminated arguments of macro 'D'");
}

static void test_include_while_collecting(void) {
    assert_error(run("`define D(x) x\nw = `D(1 `include \"a.vh\" );\n"),
                 "f.v:2:5: error: syntax: unterminated arguments of macro 'D'");
}

static void test_define_while_collecting(void) {
    assert_error(run("`define D(x) x\nw = `D(1 `define Q 2\n);\n"),
                 "f.v:2:10: error: syntax: directive inside macro text");
}

/* I1: empty actuals substitute empty text without a default and the default with one. */
static void test_probe_v1_empty_actuals(void) {
    assert_ok_text(run_probe("v1.v"),
                   "\n\nmodule m; wire a = { 1'b1}; wire b = {1'b0 }; wire [1:0] "
                   "c; wire d = c[]; endmodule\n");
}

static void test_probe_v2_too_few(void) {
    assert_error(run_probe("v2.v"),
                 "v2.v:2:20: error: syntax: macro 'D' expects 2 arguments, got 1");
}

static void test_defaults(void) {
    assert_ok_text(run("`define D(x=1, y=2) (x+y)\na=`D(,5); b=`D(); c=`D(3); d=`D(4,);\n"),
                   "\na=(1+5); b=(1+2); c=(3+2); d=(4+2);\n");
}

static void test_default_buffers(void) {
    /* a default is substituted from its spelled copy, which lies in the defining line */
    assert_ok_text(run("`define D(x=ab) [x]\nq = `D();\n"), "\nq = [ab];\n");
    assert_buf(2, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "D", 2, file_loc("1:13"), in(1, 1), in(1, 2)});
}

static void test_probe_w2_empty_formal_list(void) {
    assert_ok_text(run_probe("w2.v"), "\nmodule m(output [3:0] o); assign o = 5; endmodule\n");
}

static void test_empty_formal_list_rejects_actuals(void) {
    assert_error(run("`define F() 5\nx = `F(1);\n"),
                 "f.v:2:5: error: syntax: macro 'F' expects 0 arguments, got 1");
}

static void test_probe_t6(void) {
    assert_ok_text(run_probe("t6.v"),
                   "\n\n\n\n\nmodule m; wire abcd; wire q = {{1'b0,1'b1},1'b1}; initial "
                   "$display(\"x is x\"); wire r = (1+5); wire s=(1+2); endmodule\n");
}

/* --- PP-6 substitution --------------------------------------------------------------------- */

static void test_formal_in_string_or_escaped_identifier_untouched(void) {
    assert_ok_text(run("`define S(x) \"x\" \\x x\nq = `S(a);\n"), "\nq = \"x\" \\x a;\n");
}

/* Ruling (Task 6): a body ending in an escaped identifier keeps the blank that ended it, so
 * the identifier never swallows what follows the use (Yosys; Icarus merges). Without a blank
 * both oracles merge, and so does 2B. */
static void test_escaped_identifier_keeps_its_terminator(void) {
    assert_ok_text(run("`define E \\a \t\n`define F \\b // c\n`define G \\c\nq = `E+`F+`G+d;\n"),
                   "\n\n\nq = \\a +\\b +\\c+d;\n");
    assert_buf(1, (buf_want){ODIN3_SRCBUF_EXPANSION, "E", 3, file_loc("1:11"), file_loc("4:5"),
                             file_loc("4:7")});
}

static void test_formal_after_backtick_is_a_name(void) {
    assert_ok_text(run("`define x 9\n`define S(x) `x+x\nq = `S(a);\n"), "\n\nq = 9+a;\n");
}

static void test_formal_inside_a_longer_token_untouched(void) {
    assert_ok_text(run("`define S(x) x1 1x x_ $x x\nq = `S(a);\n"), "\nq = x1 1x x_ $x a;\n");
}

static void test_formal_used_twice_gets_buffers_each(void) {
    assert_ok_text(run("`define SQ(x) (x*x)\nq = `SQ(ab);\n"), "\nq = (ab*ab);\n");
    TEST_ASSERT_EQUAL_UINT32(4, buffers_made());
    assert_buf(2, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "SQ", 2, file_loc("2:9"), in(1, 1), in(1, 2)});
    assert_buf(3, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "SQ", 2, file_loc("2:9"), in(1, 3), in(1, 4)});
}

static void test_body_comment_becomes_a_space(void) {
    assert_ok_text(run("`define M a/* c */b\nwire `M;\n"), "\nwire a b;\n");
    seg_want want[] = {{"a", in(1, 0)}, {" ", none()}, {"b", in(1, 8)}, {";\n", file_loc("2:8")}};
    assert_segs(1, want, 4);
}

/* I2: a continuation in a body drops only the backslash; the newline stays. */
static void test_continuation_drops_only_the_backslash(void) {
    assert_ok_text(run("`define M wire\\\nx;\nmodule m;\n`M\nendmodule\n"),
                   "\nmodule m;\nwire\nx;\nendmodule\n");
    assert_buf(1, (buf_want){ODIN3_SRCBUF_EXPANSION, "M", 8, file_loc("1:11"), file_loc("4:1"),
                             file_loc("4:3")});
    seg_want want[] = {{"wire", in(1, 0)}, {"\nx;", in(1, 5)}, {"\nendmodule\n", file_loc("4:3")}};
    assert_segs(1, want, 3);
}

static void test_crlf_continuation_in_body(void) {
    assert_ok_text(run("`define M a\\\r\nb\r\nq `M;\r\n"), "\r\nq a\r\nb;\r\n");
}

static void test_probe_t3(void) {
    assert_ok_text(run_probe("t3.v"), "\nmodule m;\nwire\nx;\nendmodule\n");
}

static void test_probe_t1_nested_use_in_argument(void) {
    assert_ok_text(run_probe("t1.v"),
                   "\nmodule m(input [3:0] x, y, z, output [3:0] o);\n"
                   "assign o = ((((x)>(y)?(x):(y)))>(z)?(((x)>(y)?(x):(y))):(z));\nendmodule\n");
}

/* An argument whose text spans a nested body yields one MACRO_ARG per spelled run. */
static void test_argument_runs_across_a_nested_body(void) {
    assert_ok_text(run("`define ID(x) x\n`define W(y) `ID(<y>)\nq = `W(a);\n"), "\n\nq = <a>;\n");
    /* f.v, E(W), A(y<-a), E(ID), then ID's actual "<a>": "<" in E(W), "a" in A, ">" in E(W) */
    TEST_ASSERT_EQUAL_UINT32(7, buffers_made());
    assert_buf(4, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "ID", 1, in(1, 4), in(3, 0), in(3, 1)});
    assert_buf(5, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "ID", 1, in(2, 0), in(3, 0), in(3, 1)});
    assert_buf(6, (buf_want){ODIN3_SRCBUF_MACRO_ARG, "ID", 1, in(1, 6), in(3, 0), in(3, 1)});
}

/* --- cross-frame collection (I11) ---------------------------------------------------------- */

static void test_probe_t2_actuals_from_the_enclosing_frame(void) {
    assert_ok_text(run_probe("t2.v"),
                   "\n\nmodule m(input x, y, output o);\nassign o = x+y;\nendmodule\n");
}

static void test_probe_w5_across_newlines(void) {
    assert_ok_text(run_probe("w5.v"),
                   "\n\nmodule m(input x, y, output o);\nassign o = x+y;\nendmodule\n");
}

static void test_probe_u2_body_ending_in_paren(void) {
    assert_ok_text(run_probe("u2.v"), "\n\nmodule m; wire w = 1; endmodule\n");
}

static void test_parent_end_in_another_buffer(void) {
    assert_ok_text(run("`define G(x) [x]\n`define H `G(\nw = `H 1);\n"), "\n\nw = [1];\n");
    /* E(H), E(G) whose parent is in E(H) and parent_end one past the file's ')' */
    assert_buf(2, (buf_want){ODIN3_SRCBUF_EXPANSION, "G", 3, file_loc("1:14"), in(1, 0),
                             file_loc("3:10")});
}

/* --- directives inside macro text (declared divergence) ------------------------------------ */

static void test_probe_t5_conditional_in_body(void) {
    assert_error(run_probe("t5.v"), "t5.v:2:26: error: syntax: directive inside macro text\n"
                                    "  expanded from macro 'B' at t5.v:1:11");
}

static void test_probe_u3_define_in_body(void) {
    assert_error(run_probe("u3.v"), "u3.v:2:1: error: syntax: directive inside macro text\n"
                                    "  expanded from macro 'D1' at u3.v:1:12");
}

static void test_probe_t8_undefined(void) {
    assert_error(run_probe("t8.v"), "t8.v:1:20: error: syntax: undefined macro 'UNDEF'");
}

/* --- PP-7 recursion ------------------------------------------------------------------------ */

static void test_probe_rec(void) {
    assert_error(run_probe("rec.v"), "rec.v:2:26: error: syntax: recursive macro 'R'\n"
                                     "  expanded from macro 'R' at rec.v:1:11");
}

static void test_recursion_through_an_argument(void) {
    assert_error(run("`define A(x) x\n`define B `A(`B)\nq = `B;\n"),
                 "f.v:3:5: error: syntax: recursive macro 'B'\n"
                 "  in argument of macro 'A' at f.v:1:14\n"
                 "  expanded from macro 'B' at f.v:2:14");
}

static void test_probe_w1_rejected_as_declared(void) {
    assert_error(run_probe("w1.v"), "w1.v:5:37: error: syntax: recursive macro 'N'\n"
                                    "  expanded from macro 'K' at w1.v:4:11\n"
                                    "  in argument of macro 'KEEP' at w1.v:2:17\n"
                                    "  expanded from macro 'N' at w1.v:3:20");
}

/* A default is macro text: its MACRO_ARG takes its ancestry from the expansion it is
 * substituted into, so a cycle through a default is recursion (2B Task 8 fix round 1). */
static void test_recursion_through_a_default(void) {
    assert_error(run("`define A(x=`A()) x\nq = `A();\n"),
                 "f.v:1:13: error: syntax: recursive macro 'A'\n"
                 "  in argument of macro 'A' at f.v:1:19");
}

static void test_mutual_recursion_through_defaults(void) {
    odin3_status st = run("`define A(x=`B()) x\n`define B(y=`A()) y\nq = `A();\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    const char *want = "f.v:2:13: error: syntax: recursive macro 'A'\n";
    TEST_ASSERT_EQUAL_STRING_LEN(want, captured, strlen(want));
}

static void test_default_using_another_macro(void) {
    assert_ok_text(run("`define B 7\n`define I(v) (v)\n`define A(x=`B+`I(1)) [x]\n"
                       "q = `A(); r = `A(2);\n"),
                   "\n\n\nq = [7+(1)]; r = [2];\n");
    /* f.v, E(A), the default's MACRO_ARG (chain 1 + E(A)'s), E(B), E(I), ... */
    const odin3_pp_bufrec *recs = pp->bufs.data;
    TEST_ASSERT_EQUAL_UINT32(1, recs[1].chain);
    TEST_ASSERT_EQUAL_UINT32(2, recs[2].chain);
    TEST_ASSERT_TRUE(recs[2].from_default);
    TEST_ASSERT_EQUAL_UINT32(3, recs[3].chain);
}

static void test_same_macro_twice_in_an_argument_is_fine(void) {
    assert_ok_text(run("`define I(x) (x)\nq = `I(`I(`I(a)));\n"), "\nq = (((a)));\n");
}

/* C1: `P(`P) grows the chain while the live frame count stays small. */
static void test_chain_cap_with_few_live_frames(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.expansion_chain = 20;
    limits.expansion_depth = 2; /* live expansion frames <= 2, so frames < 4 throughout */
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define P(x) x(x)\nq = `P(`P);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_INT(1, errors);
    const char *want = "f.v:2:8: error: syntax: expansion chain exceeds "
                       "ODIN3_PP_MAX_EXPANSION_CHAIN (20)\n";
    TEST_ASSERT_EQUAL_STRING_LEN(want, captured, strlen(want));
}

static void test_cap_expansion_depth(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.expansion_depth = 3;
    odin3_pp_test_set_limits(pp, limits);
    const char *text = "`define A `B a\n`define B `C b\n`define C `D c\n`define D d\n";
    char buf[MSG_MAX];
    (void)snprintf(buf, sizeof buf, "%sq = `C;\n", text);
    assert_ok_text(run(buf), "\n\n\n\nq = d c;\n");
    (void)snprintf(buf, sizeof buf, "%sq = `A;\n", text);
    odin3_status st = run(buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    const char *want = "f.v:5:5: error: syntax: expansion depth exceeds "
                       "ODIN3_PP_MAX_EXPANSION_DEPTH (3)\n";
    TEST_ASSERT_EQUAL_STRING_LEN(want, captured, strlen(want));
}

/* Tail uses pop the exhausted frame first, so a long tail chain needs one live frame. */
static void test_tail_uses_keep_one_live_frame(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.expansion_depth = 1;
    odin3_pp_test_set_limits(pp, limits);
    assert_ok_text(run("`define A `B\n`define B `C\n`define C `D\n`define D d\nq = `A;\n"),
                   "\n\n\n\nq = d;\n");
}

static void test_cap_live_runs_while_collecting(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.live_runs = 6;
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define N(x) x\nq = `N(a/**/b/**/c/**/d/**/e);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_STRING("f.v:2:5: error: syntax: live runs exceeds ODIN3_PP_MAX_LIVE_RUNS (6)",
                             captured);
}

static void test_cap_live_runs_while_substituting(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.live_runs = 6;
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define N(x) x x x x x x\nq = `N(a);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_STRING("f.v:2:5: error: syntax: live runs exceeds ODIN3_PP_MAX_LIVE_RUNS (6)",
                             captured);
}

static void test_cap_buffers(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.buffers_per_stream = 3;
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define N(x) x x\nq = `N(a);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_STRING("f.v:2:5: error: syntax: buffers exceeds "
                             "ODIN3_PP_MAX_BUFFERS_PER_STREAM (3)",
                             captured);
}

static void test_cap_stream_bytes_bomb(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.stream_bytes = 1000;
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define A x x x x\n`define B `A `A `A `A\n`define C `B `B `B `B\n"
                          "`define D `C `C `C `C\nq = `D `D `D;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_NOT_NULL(strstr(captured, "stream bytes exceeds ODIN3_PP_MAX_STREAM_BYTES (1000)"));
}

static void test_cap_loc_bytes_unused_arguments(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.loc_bytes_per_stream = 200;
    odin3_pp_test_set_limits(pp, limits);
    odin3_status st = run("`define G(x) `H(x,x)\n`define H(a,b) `I(a,b,a,b)\n`define I(a,b,c,d)\n"
                          "q = `G(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_NOT_NULL(
        strstr(captured, "location bytes exceeds ODIN3_PP_MAX_LOC_BYTES_PER_STREAM (200)"));
}

/* --- mixing with conditionals and include (Task 7) ------------------------------- */

static void test_probe_w3_conditionals_while_collecting(void) {
    assert_ok_text(run_probe("w3.v"), "\nmodule m(output [3:0] o); assign o = 1  + 3; endmodule\n");
}

static void test_probe_t7(void) {
    assert_ok_text(run_probe("t7.v"),
                   "\n\nmodule m; wire a b; wire w = p   + r; wire v = 1  + 2  ; endmodule\n");
}

/* Collection never crosses an `include boundary: the end of an included file stops it. */
static void test_include_end_stops_collection(void) {
    char path[PATH_BUF];
    put("inc.vh", odin3_bytes_cstr("`define ID(x) x\n`ID(a,\n"), path);
    odin3_status st = run("`include \"inc.vh\"\nb);\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    const char *want = "inc.vh:2:1: error: syntax: unterminated arguments of macro 'ID'";
    TEST_ASSERT_EQUAL_STRING_LEN(want, captured, strlen(want));
}

static void test_probe_t7_macros_only(void) {
    assert_ok_text(run("`define M a/* c */b\n`define N(x) x\n"
                       "module m; wire `M; wire w = `N(p /* q */ + r); endmodule\n"),
                   "\n\nmodule m; wire a b; wire w = p   + r; endmodule\n");
}

/* --- runs across calls --------------------------------------------------------------------- */

static void test_macros_from_an_earlier_run_expand(void) {
    assert_ok_text(run("`define A(x) <x>\n"), "\n");
    assert_ok_text(run("q = `A(1);\n"), "q = <1>;\n");
}

static void test_command_line_macros_expand(void) {
    odin3_pp_define defines[] = {{odin3_bytes_cstr("W"), odin3_bytes_cstr("8"), true},
                                 {odin3_bytes_cstr("F(a,b)"), odin3_bytes_cstr("a-b"), true}};
    odin3_pp_config config = {defines, 2, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_create(design, &config, &pp));
    assert_ok_text(run("q = `W + `F(1,2);\n"), "q = 8 + 1-2;\n");
}

static void test_failed_run_then_clean_run(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define F(x) x\nq = `F(1;\n"));
    errors = 0;
    assert_ok_text(run("q = `F(2);\n"), "q = 2;\n");
}

/* --- out of memory ------------------------------------------------------------------------- */

static const char k_oom_text[] = "`define INNER(p) (p + 1)\n"
                                 "`define OUTER(q=0) `INNER(q /* c */ * 2)\n"
                                 "`define CALL `INNER\n"
                                 "assign y = `OUTER(xx) + `OUTER() + `CALL\n(`INNER(z));\n";

static bool oom_check(odin3_status st) {
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(stream.text.ptr);
        return false;
    }
    assert_out("\n\n\nassign y = (xx   * 2 + 1) + (0   * 2 + 1) + ((z + 1) + 1);\n");
    return true;
}

static bool oom_try(long fail_after, const char *path) {
    odin3_pp_destroy(pp);
    pp = NULL;
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_pp_config config = {NULL, 0, NULL, 0};
    odin3_pp_source src = {id("t.v"), id(path), id("work"), {0}};
    memset(&stream, 0, sizeof stream);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_pp_create(design, &config, &pp);
    if (st == ODIN3_OK) {
        st = odin3_pp_run(pp, &src, &stream);
    }
    odin3_util_set_alloc_fail_after(-1);
    return oom_check(st);
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
    TEST_ASSERT_TRUE(tries > 25); /* every allocation point of the run, 32 today */
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static void run_use_tests(void) {
    RUN_TEST(test_2a_example_buffers);
    RUN_TEST(test_2a_example_segments);
    RUN_TEST(test_2a_example_chain);
    RUN_TEST(test_2a_assign_out);
    RUN_TEST(test_glued_uses_get_no_space);
    RUN_TEST(test_no_formals_ignores_parens);
    RUN_TEST(test_takes_arguments);
    RUN_TEST(test_takes_arguments_at_end_of_file);
    RUN_TEST(test_paren_after_blanks_and_newlines);
    RUN_TEST(test_comment_before_paren_is_not_blank);
    RUN_TEST(test_undefined_macro);
    RUN_TEST(test_addition_num_is_one_name);
    RUN_TEST(test_actuals_nest_and_trim);
    RUN_TEST(test_strings_in_actuals_are_opaque);
    RUN_TEST(test_escaped_identifier_in_actual_is_opaque);
    RUN_TEST(test_newlines_kept_in_actuals);
    RUN_TEST(test_comment_in_argument);
    RUN_TEST(test_line_comment_in_argument);
    RUN_TEST(test_comment_only_argument);
    RUN_TEST(test_too_many_actuals);
    RUN_TEST(test_unterminated_arguments);
    RUN_TEST(test_include_while_collecting);
    RUN_TEST(test_define_while_collecting);
    RUN_TEST(test_probe_v1_empty_actuals);
    RUN_TEST(test_probe_v2_too_few);
    RUN_TEST(test_defaults);
    RUN_TEST(test_default_buffers);
    RUN_TEST(test_probe_w2_empty_formal_list);
    RUN_TEST(test_empty_formal_list_rejects_actuals);
    RUN_TEST(test_probe_t6);
    RUN_TEST(test_formal_in_string_or_escaped_identifier_untouched);
    RUN_TEST(test_escaped_identifier_keeps_its_terminator);
    RUN_TEST(test_formal_after_backtick_is_a_name);
    RUN_TEST(test_formal_inside_a_longer_token_untouched);
}

static void run_more_tests(void) {
    RUN_TEST(test_formal_used_twice_gets_buffers_each);
    RUN_TEST(test_body_comment_becomes_a_space);
    RUN_TEST(test_continuation_drops_only_the_backslash);
    RUN_TEST(test_crlf_continuation_in_body);
    RUN_TEST(test_probe_t3);
    RUN_TEST(test_probe_t1_nested_use_in_argument);
    RUN_TEST(test_argument_runs_across_a_nested_body);
    RUN_TEST(test_probe_t2_actuals_from_the_enclosing_frame);
    RUN_TEST(test_probe_w5_across_newlines);
    RUN_TEST(test_probe_u2_body_ending_in_paren);
    RUN_TEST(test_parent_end_in_another_buffer);
    RUN_TEST(test_probe_t5_conditional_in_body);
    RUN_TEST(test_probe_u3_define_in_body);
    RUN_TEST(test_probe_t8_undefined);
    RUN_TEST(test_probe_rec);
    RUN_TEST(test_recursion_through_an_argument);
    RUN_TEST(test_probe_w1_rejected_as_declared);
    RUN_TEST(test_recursion_through_a_default);
    RUN_TEST(test_mutual_recursion_through_defaults);
    RUN_TEST(test_default_using_another_macro);
    RUN_TEST(test_same_macro_twice_in_an_argument_is_fine);
    RUN_TEST(test_chain_cap_with_few_live_frames);
    RUN_TEST(test_cap_expansion_depth);
    RUN_TEST(test_tail_uses_keep_one_live_frame);
    RUN_TEST(test_cap_live_runs_while_collecting);
    RUN_TEST(test_cap_live_runs_while_substituting);
    RUN_TEST(test_cap_buffers);
    RUN_TEST(test_cap_stream_bytes_bomb);
    RUN_TEST(test_cap_loc_bytes_unused_arguments);
    RUN_TEST(test_probe_w3_conditionals_while_collecting);
    RUN_TEST(test_probe_t7);
    RUN_TEST(test_include_end_stops_collection);
    RUN_TEST(test_probe_t7_macros_only);
    RUN_TEST(test_macros_from_an_earlier_run_expand);
    RUN_TEST(test_command_line_macros_expand);
    RUN_TEST(test_failed_run_then_clean_run);
    RUN_TEST(test_out_of_memory);
}

int main(void) {
    UNITY_BEGIN();
    run_use_tests();
    run_more_tests();
    return UNITY_END();
}
