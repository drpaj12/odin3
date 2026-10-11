/*
 * test_pp_cond.c — unit tests for the preprocessor's conditionals and translate_off (2B spec
 * §7.3 PP-3/PP-4, §7.6 PP-10): the nested_ifdef micros, mid-line conditionals with their
 * segments, the protective space, elsif chains, skipped regions (gap 22, comments and strings),
 * misuse, the conditional-depth cap, every translate_off form and an OOM sweep.
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
#include <unistd.h>

enum { PATH_BUF = PP_PATH_BUF, MSG_MAX = PP_MSG_MAX, OOM_LIMIT = 4000 };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

/* Creates the preprocessor with the macros named in defines (NULL-terminated, no values). */
static void make_pp(const char *const *defines) {
    odin3_pp_define defs[8];
    uint32_t count = 0;
    for (; defines != NULL && defines[count] != NULL; count++) {
        defs[count] = (odin3_pp_define){odin3_bytes_cstr(defines[count]), {NULL, 0}, false};
    }
    odin3_pp_config config = {defs, count, NULL, 0};
    create_pp(&config);
}

/* Runs text as a fresh file (the cache keeps every path it read) given as "t.v". */
static odin3_status run(const char *text) {
    return run_fresh("t.v", odin3_bytes_cstr(text));
}

static void assert_pp(const char *text, const char *want) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(text), want);
    assert_text(want);
}

/* The segment map is exactly n pairs {out_offset, loc} with loc given as a file offset (or
 * UINT32_MAX for an inserted, loc-0 segment). */
static void assert_segments(const uint32_t (*want)[2], uint32_t n) {
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32(n, count);
    TEST_ASSERT_EQUAL_UINT32(n, stream.nsegments);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i][0], segs[i].out_offset);
        uint32_t loc = want[i][1] == UINT32_MAX ? 0 : at_offset(want[i][1]);
        TEST_ASSERT_EQUAL_UINT32(loc, segs[i].loc);
    }
}

static void assert_error(const char *text, const char *want) {
    errors = 0;
    captured[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run(text), text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, captured, text);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NULL(stream.text.ptr);
}

/* --- the micros ---------------------------------------------------------------------------- */

static const char k_assign[] = "assign out";

/* The first "assign out" of the stream (NUL-terminated copy in text), NULL when none. */
static const char *find_assign(char *text, size_t size) {
    TEST_ASSERT_TRUE(stream.text.len < size);
    memcpy(text, stream.text.ptr, stream.text.len);
    text[stream.text.len] = '\0';
    return strstr(text, k_assign);
}

/* The stream has exactly one "assign out" statement, want (NULL: none). */
static void assert_only_assign(const char *file, const char *want) {
    char text[MSG_MAX * 2];
    const char *first = find_assign(text, sizeof text);
    if (want == NULL) {
        TEST_ASSERT_NULL_MESSAGE(first, file);
        return;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(first, file);
    TEST_ASSERT_NULL_MESSAGE(strstr(first + 1, k_assign), file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(first, want, strlen(want)), file);
}

/* Every conditional micro's surviving branch (expected values measured with iverilog -E). */
static void test_conditional_micros(void) {
    static const struct {
        const char *file;
        const char *assign;
    } cases[] = {
        {"nested_ifdef_alldef.v", "assign out = 3'b111;"},
        {"nested_ifdef_middef.v", "assign out = 3'b010;"},
        {"nested_ifdef_nonedef.v", "assign out = 3'b000;"},
        {"nested_ifdef_outsidedef.v", "assign out = 3'b101;"},
        {"nested_ifdef_twodef.v", "assign out = 3'b011;"},
        {"ifdef_defined.v", "assign out = in;"},
        {"ifdef_else_defined.v", "assign out = in1;"},
        {"ifdef_else_undefined.v", "assign out = in2;"},
        {"ifdef_undefined.v", NULL},
        {"ifndef_defined.v", NULL},
        {"ifndef_else_defined.v", "assign out = in2;"},
        {"ifndef_else_undefined.v", "assign out = in1;"},
        {"ifndef_undefined.v", "assign out = in;"},
        {"elsif_both_defined.v", "assign out = in1;"},
        {"elsif_defined.v", "assign out = in2;"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char path[PATH_BUF];
        (void)snprintf(path, sizeof path, "%s/%s", ODIN3_PP_MICROS, cases[i].file);
        make_pp(NULL); /* macros stay defined across runs: one preprocessor per micro */
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_path(cases[i].file, path), cases[i].file);
        assert_only_assign(cases[i].file, cases[i].assign);
    }
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --- token-based conditionals (PP-3) ------------------------------------------------------- */

static const char k_midline[] = "wire a = 1 `ifdef X + 1 `else + 2 `endif;";

static void test_midline_undefined(void) {
    assert_pp(k_midline, "wire a = 1  + 2 ;");
    static const uint32_t want[][2] = {{0, 0}, {11, 29}, {16, 40}};
    assert_segments(want, 3);
}

static void test_midline_defined(void) {
    static const char *const defines[] = {"X", NULL};
    make_pp(defines);
    assert_pp(k_midline, "wire a = 1  + 1 ;");
    static const uint32_t want[][2] = {{0, 0}, {11, 19}, {16, 40}};
    assert_segments(want, 3);
}

static void test_gap_either_side(void) {
    assert_pp("a `ifdef X b `endif c", "a  c");
    static const uint32_t want[][2] = {{0, 0}, {2, 19}};
    assert_segments(want, 2);
}

static void test_protective_space(void) {
    assert_pp("a`ifdef X b`else(c)`endif;", "a (c) ;");
    static const uint32_t want[][2] = {{0, 0}, {1, UINT32_MAX}, {2, 16}, {5, UINT32_MAX}, {6, 25}};
    assert_segments(want, 5);
    assert_pp("a`ifdef X b`endif c", "a c"); /* no space when either side is blank */
    assert_pp("x`ifndef X`endif\ny", "x\ny");
}

static void test_elsif_chains(void) {
    static const char *const defines[] = {"D", "E", NULL};
    make_pp(defines);
    assert_pp("`ifndef D a `elsif NOPE b `elsif D c `else d `endif", " c ");
    assert_pp("`ifdef NOPE a `elsif NOPE b `else c `endif", " c ");
    assert_pp("`ifdef D a `elsif E b `else c `endif", " a ");
    assert_pp("`ifdef NOPE a `elsif D b `elsif E c `else d `endif", " b ");
    assert_pp("`ifdef NOPE a `elsif NOPE b `endif|", "|");
}

/* Gap 22: a conditional pushed inside a skipped region never takes a branch, even when its
 * name is defined; comments and strings in the skipped region hide `endif. */
static void test_nested_in_skipped_region(void) {
    static const char *const defines[] = {"D", NULL};
    make_pp(defines);
    assert_pp("`ifdef NOPE\n"
              "  `ifdef D in_d `elsif D in_elsif `else in_else `endif\n"
              "  // `endif in a comment\n"
              "  \" `endif in a string \"\n"
              "  /* `else */\n"
              "`elsif D\n"
              "  elsif_d\n"
              "`else\n"
              "  else_\n"
              "`endif\n",
              "\n  elsif_d\n\n");
    assert_pp("`ifndef D `ifndef NOPE x `else y `endif `endif|", "|");
}

/* PP-4: in a skipped region every other directive and macro use is ignored. */
static void test_skipped_region_ignores_directives(void) {
    assert_pp("`ifdef NOPE `define Z 1 `include <x> `undefined ` `8 `line x `__FILE__ `` `endif|"
              "`ifdef Z z `else no_z `endif",
              "| no_z ");
}

static void test_cond_misuse(void) {
    assert_error("`endif\n", "t.v:1:1: error: syntax: `endif without `ifdef");
    assert_error("a `else\n", "t.v:1:3: error: syntax: `else without `ifdef");
    assert_error("`elsif X\n", "t.v:1:1: error: syntax: `elsif without `ifdef");
    assert_error("`ifdef X `else `elsif Y `endif\n", "t.v:1:16: error: syntax: `elsif after `else");
    assert_error("`ifdef X `else `else `endif\n", "t.v:1:16: error: syntax: second `else");
    assert_error("`ifdef X `endif `endif\n", "t.v:1:17: error: syntax: `endif without `ifdef");
    assert_error("`ifdef\nX `endif\n",
                 "t.v:1:7: error: syntax: expected a macro name after `ifdef");
    assert_error("`ifndef 9 `endif\n",
                 "t.v:1:9: error: syntax: expected a macro name after `ifndef");
    assert_error("`ifdef A `elsif\n",
                 "t.v:1:16: error: syntax: expected a macro name after `elsif");
    /* misuse is reported inside skipped regions too */
    assert_error("`ifdef A `ifdef B `else `else `endif `endif\n",
                 "t.v:1:25: error: syntax: second `else");
}

static void test_unterminated(void) {
    assert_error("`ifdef X\n", "t.v:1:1: error: syntax: unterminated `ifdef");
    assert_error("a\n`ifdef A\n  `ifndef B\n`endif\n",
                 "t.v:2:1: error: syntax: unterminated `ifdef");
    assert_error("`ifdef A\n`ifdef B\n", "t.v:2:1: error: syntax: unterminated `ifdef");
}

static void test_cap_cond_depth(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.cond_depth = 2;
    odin3_pp_test_set_limits(pp, limits);
    assert_pp("`ifdef A `ifdef B `endif `endif `ifdef C `endif", " ");
    assert_error("`ifdef A `ifdef B `ifdef C `endif `endif `endif",
                 "t.v:1:19: error: syntax: conditional depth exceeds ODIN3_PP_MAX_COND_DEPTH (2)");
}

/* --- translate_off / translate_on (PP-10) -------------------------------------------------- */

static void test_translate_off_forms(void) {
    static const char *const words[] = {"synopsys", "synthesis", "pragma"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        char text[MSG_MAX];
        (void)snprintf(text, sizeof text, "a // %s translate_off\nb\n//%s translate_on\nc\n",
                       words[i], words[i]);
        assert_pp(text, "a \nc\n");
        (void)snprintf(text, sizeof text, "a/*\t%s  translate_off */b/*%s translate_on\t*/c",
                       words[i], words[i]);
        assert_pp(text, "a c");
        (void)snprintf(text, sizeof text, "a /*%stranslate_off*/ b /* %s translate_on */ c",
                       words[i], words[i]);
        assert_pp(text, "a  c");
    }
    assert_pp("x // synopsys translate_off \r\ny\r\n// synopsys translate_on\t\r\nz\r\n",
              "x \nz\r\n"); /* a dropped line comment takes its CR */
    TEST_ASSERT_EQUAL_INT(0, warnings);
}

/* Probe w4: the region and its comments leave the stream; a following comment stays. */
static void test_translate_off_w4(void) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/w4.v", ODIN3_PP_PROBES);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_path("w4.v", path));
    assert_text("module m(output o);\n\n/* foo */ assign o = 1'b0;\nendmodule\n");
}

static void test_not_translate_off(void) {
    static const char *const texts[] = {
        "// synopsys translate_off now\n", "// Synopsys translate_off\n",
        "// synopsys TRANSLATE_OFF\n",     "/* synopsys translate_off x */\n",
        "/* synopsys translate_off\n",     "// synopsys translate_of\n",
        "// synopsys\ttranslate_offx\n",   "/* synthesis translate_off\n */\n",
        "// pragma: translate_off\n",      "//synopsys translate_off */\n",
        "// lint_off translate_off\n",     "/* synopsys translate_off * /\n",
    };
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        assert_pp(texts[i], texts[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, warnings);
}

/* The region is opaque: no directive, macro use or stray backtick inside it is interpreted. */
static void test_translate_off_is_opaque(void) {
    assert_pp("// synopsys translate_off\n`undefined `ifdef ` `include <x> `endif `else\n"
              "// synopsys translate_on\nok\n",
              "\nok\n");
    /* a `endif inside the region is not seen: the `ifdef stays open */
    static const char *const defines[] = {"X", NULL};
    make_pp(defines);
    assert_error("`ifdef X\n// synopsys translate_off\n`endif\n// synopsys translate_on\n",
                 "t.v:1:1: error: syntax: unterminated `ifdef");
    /* inside a skipped branch the comment is skipped text, never seen */
    assert_pp("`ifdef NOPE\n// synopsys translate_off\n`endif\nok\n", "\nok\n");
}

static void test_translate_off_unterminated(void) {
    assert_error("a\n  // synopsys translate_off\nb\n",
                 "t.v:2:3: error: syntax: unterminated translate_off");
    assert_error("a /* synthesis translate_off */ /* synthesis translate_off */",
                 "t.v:1:3: error: syntax: unterminated translate_off");
}

static void test_stray_translate_on(void) {
    assert_pp("a\n // synopsys translate_on\nb\n", "a\n // synopsys translate_on\nb\n");
    TEST_ASSERT_EQUAL_INT(1, warnings);
    TEST_ASSERT_EQUAL_STRING("t.v:2:2: warning: stray translate_on (no translate_off is open); "
                             "ignored",
                             warned);
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --- out of memory ------------------------------------------------------------------------- */

static const char k_oom_text[] = "`ifdef X a `elsif Y b `else c `ifndef Z d `endif `endif\n"
                                 "// synopsys translate_off\nskipped\n// synopsys translate_on\n"
                                 "e`ifdef W `ifdef V v `endif `endif(f)\n";
static const char k_oom_want[] = " c  d  \n\ne (f)\n";

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
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_conditional_micros);
    RUN_TEST(test_midline_undefined);
    RUN_TEST(test_midline_defined);
    RUN_TEST(test_gap_either_side);
    RUN_TEST(test_protective_space);
    RUN_TEST(test_elsif_chains);
    RUN_TEST(test_nested_in_skipped_region);
    RUN_TEST(test_skipped_region_ignores_directives);
    RUN_TEST(test_cond_misuse);
    RUN_TEST(test_unterminated);
    RUN_TEST(test_cap_cond_depth);
    RUN_TEST(test_translate_off_forms);
    RUN_TEST(test_translate_off_w4);
    RUN_TEST(test_not_translate_off);
    RUN_TEST(test_translate_off_is_opaque);
    RUN_TEST(test_translate_off_unterminated);
    RUN_TEST(test_stray_translate_on);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
