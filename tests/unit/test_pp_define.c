/*
 * test_pp_define.c — unit tests for `define and `undef (2B spec §7.3): L13's forms, the stored
 * bodies and spelled ranges, the reviewer probes t3/t4/u1/t9, <command line> defines, errors with
 * their exact located messages, the definition caps through the hook, and an OOM sweep.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_PP_PROBES
#error "ODIN3_PP_PROBES must name tests/unit/data/pp/probes"
#endif

enum { PATH_BUF = 1024, MSG_MAX = 4096, MAX_MADE = 256, OOM_LIMIT = 4000 };

static odin3_design *design;
static odin3_srcman *sm;
static odin3_pp *pp;
static odin3_pp_stream stream;
static char tmp[PATH_BUF / 2];
static char captured[MSG_MAX];
static int errors;
static char made[MAX_MADE][PATH_BUF];
static int nmade;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    errors = 0;
    nmade = 0;
    pp = NULL;
    memset(&stream, 0, sizeof stream);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_ppdef_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_pp_destroy(pp);
    for (int i = nmade - 1; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(0, remove(made[i]));
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(str), &got));
    return got;
}

/* Writes tmp/name and stores its path in path. */
static void put(const char *name, odin3_bytes text, char *path) {
    (void)snprintf(path, PATH_BUF, "%s/%s", tmp, name);
    bool fresh = access(path, F_OK) != 0;
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(text.len, fwrite(text.ptr, 1, text.len, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    if (fresh) {
        TEST_ASSERT_TRUE(nmade < MAX_MADE);
        (void)snprintf(made[nmade++], PATH_BUF, "%s", path);
    }
}

static void create_with(const odin3_pp_config *config) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_create(design, config, &pp));
}

static void ensure_pp(void) {
    if (pp == NULL) {
        odin3_pp_config none = {NULL, 0, NULL, 0};
        create_with(&none);
    }
}

/* Runs the preprocessor over the file at path, named name. */
static odin3_status run_path(const char *name, const char *path) {
    ensure_pp();
    odin3_pp_source src = {id(name), id(path), id("work"), {0}};
    return odin3_pp_run(pp, &src, &stream);
}

/* Writes text to tmp/name and runs the preprocessor over it. */
static odin3_status run_bytes(const char *name, odin3_bytes text) {
    char path[PATH_BUF];
    put(name, text, path);
    return run_path(name, path);
}

/* Runs text as a fresh file (the cache keeps every path it read) given as "t.v". */
static odin3_status run(const char *text) {
    static int serial;
    char file[PATH_BUF / 4];
    char path[PATH_BUF];
    (void)snprintf(file, sizeof file, "t%d.v", serial++);
    put(file, odin3_bytes_cstr(text), path);
    return run_path("t.v", path);
}

static odin3_status run_probe(const char *probe) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_PP_PROBES, probe);
    return run_path(probe, path);
}

static const odin3_pp_macro *macro(const char *name) {
    uint32_t index = odin3_pp_macro_find(pp, odin3_bytes_cstr(name));
    return index == 0 ? NULL : (const odin3_pp_macro *)odin3_vec_cat(&pp->macros, index - 1);
}

typedef struct line_col {
    uint32_t line, col;
} line_col;

static void assert_pos(odin3_loc loc, line_col want) {
    odin3_srcpos pos;
    TEST_ASSERT_TRUE(odin3_srcman_decode(sm, loc, &pos));
    TEST_ASSERT_EQUAL_UINT32(want.line, pos.line);
    TEST_ASSERT_EQUAL_UINT32(want.col, pos.col);
}

static void assert_body(const odin3_pp_macro *mac, const char *body) {
    TEST_ASSERT_NOT_NULL(mac);
    TEST_ASSERT_EQUAL_UINT32(strlen(body), mac->len);
    TEST_ASSERT_EQUAL_MEMORY(body, mac->body, mac->len == 0 ? 1 : mac->len);
}

/* The body's spelled bytes equal the file's bytes at [def, def + len). */
static void assert_spelled(const odin3_pp_macro *mac, const char *file_text) {
    TEST_ASSERT_NOT_NULL(mac);
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, mac->def), &info));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, info.kind);
    TEST_ASSERT_EQUAL_MEMORY(file_text + (mac->def.v - info.start), mac->body,
                             mac->len == 0 ? 0 : mac->len);
}

typedef struct formal_want {
    const char *name;
    const char *deflt; /* NULL: no default */
} formal_want;

static void assert_formal(const odin3_pp_macro *mac, uint32_t index, formal_want want) {
    TEST_ASSERT_NOT_NULL(mac);
    TEST_ASSERT_TRUE(index < mac->nformals);
    const odin3_pp_formal *formal = &mac->formals[index];
    TEST_ASSERT_EQUAL_STRING(want.name,
                             odin3_strtab_get(odin3_design_strtab(design), formal->name));
    TEST_ASSERT_EQUAL(want.deflt != NULL, formal->has_default);
    if (want.deflt != NULL) {
        TEST_ASSERT_EQUAL_UINT32(strlen(want.deflt), formal->len);
        TEST_ASSERT_EQUAL_MEMORY(want.deflt, formal->text, formal->len == 0 ? 1 : formal->len);
    }
}

static void assert_text(const char *want) {
    TEST_ASSERT_EQUAL_size_t(strlen(want), stream.text.len);
    TEST_ASSERT_EQUAL_MEMORY(want, stream.text.ptr, strlen(want) == 0 ? 1 : strlen(want));
}

static void assert_error(const char *want) {
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(want, captured);
}

/* --- L13 forms ----------------------------------------------------------------------------- */

static void test_plain_define(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define W 8\nwire w;\n"));
    assert_body(macro("W"), "8");
    const odin3_pp_macro *mac = macro("W");
    TEST_ASSERT_FALSE(mac->has_formals);
    TEST_ASSERT_EQUAL_UINT32(0, mac->nformals);
    assert_pos(mac->def, (line_col){1, 11});
    assert_pos(mac->at, (line_col){1, 1});
    assert_text("\nwire w;\n"); /* the newline ending the define is emitted */
    TEST_ASSERT_EQUAL_UINT32(1, stream.nsegments);
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static void test_define_with_args(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define ADD(a, b ) a + b\n"));
    assert_body(macro("ADD"), "a + b");
    TEST_ASSERT_TRUE(macro("ADD")->has_formals);
    TEST_ASSERT_EQUAL_UINT32(2, macro("ADD")->nformals);
    assert_formal(macro("ADD"), 0, (formal_want){"a", NULL});
    assert_formal(macro("ADD"), 1, (formal_want){"b", NULL});
    assert_pos(macro("ADD")->def, (line_col){1, 20});
}

static void test_empty_formal_list(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define F() 5\n"));
    TEST_ASSERT_TRUE(macro("F")->has_formals);
    TEST_ASSERT_EQUAL_UINT32(0, macro("F")->nformals);
    assert_body(macro("F"), "5");
}

static void test_paren_after_blank_is_body(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define P (a) + 1\n"));
    TEST_ASSERT_FALSE(macro("P")->has_formals);
    assert_body(macro("P"), "(a) + 1");
}

static void test_defaults(void) {
    const char *text = "`define D(a = 1 , b= (2, [3,4]) , c, d = \"x,)\" ) a\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(text));
    assert_formal(macro("D"), 0, (formal_want){"a", "1"});
    assert_formal(macro("D"), 1, (formal_want){"b", "(2, [3,4])"});
    assert_formal(macro("D"), 2, (formal_want){"c", NULL});
    assert_formal(macro("D"), 3, (formal_want){"d", "\"x,)\""});
    const odin3_pp_formal *second = &macro("D")->formals[1];
    assert_pos(second->def, (line_col){1, 22});
    assert_body(macro("D"), "a");
}

static void test_empty_default(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define E(a=) a\n"));
    assert_formal(macro("E"), 0, (formal_want){"a", ""});
    assert_pos(macro("E")->formals[0].def, (line_col){1, 13});
}

static void test_multi_line_define(void) {
    const char *text = "`define M(a) a + \\\n  a \\\n  + 1\nwire x;\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(text));
    assert_body(macro("M"), "a + \\\n  a \\\n  + 1");
    assert_spelled(macro("M"), text);
    assert_text("\nwire x;\n");
    TEST_ASSERT_EQUAL_UINT32(1, stream.nsegments);
}

static void test_crlf_continuation(void) {
    const char *text = "`define X a \\\r\n b\r\nwire x;\r\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(text));
    assert_body(macro("X"), "a \\\r\n b");
    assert_spelled(macro("X"), text);
    assert_text("\r\nwire x;\r\n");
}

static void test_redefinition_is_silent(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define X 1\n`define X(a) 2\n"));
    assert_body(macro("X"), "2");
    TEST_ASSERT_TRUE(macro("X")->has_formals);
    assert_pos(macro("X")->at, (line_col){2, 1});
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static void test_line_comment_ends_the_text(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define X 1 + 2  // the sum \\\nwire y;\n"));
    assert_body(macro("X"), "1 + 2");
    assert_text("\nwire y;\n");
}

static void test_block_comment_kept_in_text(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define X 1 /* one */ + 2 /* two */\n"));
    assert_body(macro("X"), "1 /* one */ + 2 /* two */");
}

static void test_block_comment_across_continuation(void) {
    const char *text = "`define X 1 /* one \\\n two */ + 2\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(text));
    assert_body(macro("X"), "1 /* one \\\n two */ + 2");
}

static void test_string_protects_line_comment(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define S \"a//b\\\" // c\" x // gone\n"));
    assert_body(macro("S"), "\"a//b\\\" // c\" x");
}

static void test_escaped_identifier_in_body(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define E \\a//b x\n"));
    assert_body(macro("E"), "\\a//b x");
}

static void test_empty_body(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define EMPTY\n`define EF(a)   \n`define EC // c\n"));
    assert_body(macro("EMPTY"), "");
    assert_pos(macro("EMPTY")->def, (line_col){1, 14});
    assert_body(macro("EF"), "");
    assert_pos(macro("EF")->def, (line_col){2, 14});
    assert_body(macro("EC"), "");
    assert_text("\n\n\n");
}

static void test_define_at_end_of_file(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("wire a;\n`define LAST 7"));
    assert_body(macro("LAST"), "7");
    assert_text("wire a;\n");
}

static void test_define_mid_line(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("wire a; `define MID 1\nwire b;\n"));
    assert_body(macro("MID"), "1");
    assert_text("wire a; \nwire b;\n");
    TEST_ASSERT_EQUAL_UINT32(2, stream.nsegments);
}

static void test_macros_persist_across_runs(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_bytes("a.v", odin3_bytes_cstr("`define X 1\n")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_bytes("b.v", odin3_bytes_cstr("wire b;\n")));
    assert_body(macro("X"), "1");
    assert_text("wire b;\n");
}

/* --- probes -------------------------------------------------------------------------------- */

static void test_probe_t3_body_spans_the_continuation(void) {
    (void)run_probe("t3.v"); /* the use of `M is Task 8's */
    assert_body(macro("M"), "wire\\\nx;");
    assert_pos(macro("M")->def, (line_col){1, 11});
    const odin3_pp_macro *mac = macro("M");
    assert_pos((odin3_loc){mac->def.v + mac->len - 1}, (line_col){2, 2}); /* the ';' on line 2 */
}

static void test_probe_t4_comment_continues_nothing(void) {
    (void)run_probe("t4.v");
    assert_body(macro("A"),
                "a"); /* the emitted text is covered by test_line_comment_ends_the_text */
}

static void test_probe_u1_multi_line_comment_is_unterminated(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_probe("u1.v"));
    assert_error("u1.v:1:15: error: syntax: unterminated comment in macro text");
    TEST_ASSERT_NULL(macro("FOO"));
    TEST_ASSERT_NULL(stream.text.ptr);
}

static void test_probe_t9_resetall_keeps_macros(void) {
    (void)run_probe("t9.v"); /* its `ifdef and `P use are Tasks 7 and 8 */
    assert_body(macro("X"), "1");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define Y 2\n`resetall\n"));
    assert_body(macro("X"), "1");
    assert_body(macro("Y"), "2");
    assert_text("\n`resetall\n");
}

/* --- undef --------------------------------------------------------------------------------- */

static void test_undef(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define X 1\n`undef X\n`undef NEVER\nwire a;\n"));
    TEST_ASSERT_NULL(macro("X"));
    assert_text("\n\n\nwire a;\n");
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static void test_undef_then_redefine(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define X 1\n`undef  X `define X 2\n"));
    assert_body(macro("X"), "2");
    assert_text("\n \n");
}

static void test_undef_errors(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`undef 3\n"));
    assert_error("t.v:1:8: error: syntax: expected a macro name after `undef");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("\n  `undef timescale\n"));
    assert_error("t.v:2:10: error: syntax: compiler directive 'timescale' cannot be a macro name");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`undef\nX\n"));
    assert_error("t.v:1:7: error: syntax: expected a macro name after `undef");
}

/* --- errors -------------------------------------------------------------------------------- */

typedef struct bad_case {
    const char *text;
    const char *want;
} bad_case;

static void test_define_errors(void) {
    static const bad_case cases[] = {
        {"`define 9X 1\n", "t.v:1:9: error: syntax: expected a macro name after `define"},
        {"`define\n", "t.v:1:8: error: syntax: expected a macro name after `define"},
        {"`define \\esc 1\n", "t.v:1:9: error: syntax: expected a macro name after `define"},
        {"`define ifdef 1\n",
         "t.v:1:9: error: syntax: compiler directive 'ifdef' cannot be a macro name"},
        {"`define define\n",
         "t.v:1:9: error: syntax: compiler directive 'define' cannot be a macro name"},
        {"`define F(1) x\n",
         "t.v:1:11: error: syntax: formal argument of macro 'F' is not an identifier"},
        {"`define F(a,) x\n",
         "t.v:1:13: error: syntax: formal argument of macro 'F' is not an identifier"},
        {"`define F(a b) x\n",
         "t.v:1:13: error: syntax: expected ',' or ')' in the formal list of macro 'F'"},
        {"`define F(a\nwire x;\n",
         "t.v:1:12: error: syntax: expected ',' or ')' in the formal list of macro 'F'"},
        {"`define F(a, a) x\n", "t.v:1:14: error: syntax: duplicate formal 'a' of macro 'F'"},
        {"`define F(a = (1, 2) x\n",
         "t.v:1:15: error: syntax: unterminated default of formal 'a' of macro 'F'"},
        {"`define F(a = \"x) x\n", "t.v:1:15: error: syntax: unterminated string in macro text"},
        {"`define S \"abc\nwire x;\n",
         "t.v:1:11: error: syntax: unterminated string in macro text"},
        {"`define C 1 /* open\n */\n",
         "t.v:1:13: error: syntax: unterminated comment in macro text"},
        {"`define C 1 /* open", "t.v:1:13: error: syntax: unterminated comment in macro text"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        errors = 0;
        captured[0] = '\0';
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run(cases[i].text), cases[i].text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].want, captured, cases[i].text);
        TEST_ASSERT_EQUAL_INT(1, errors);
        TEST_ASSERT_NULL(stream.text.ptr);
        TEST_ASSERT_EQUAL_UINT32(0, stream.stream.v);
    }
}

static void test_sv_operators_in_a_body(void) {
    static const bad_case cases[] = {
        {"`define P a``b\n", "t.v:1:12: error: syntax: '``' is a SystemVerilog macro operator; "
                             "not Verilog-2005 (Phase 5)"},
        {"`define Q(x) `\"x`\"\n", "t.v:1:14: error: syntax: '`\"' is a SystemVerilog macro "
                                   "operator; not Verilog-2005 (Phase 5)"},
        {"`define R(x) `\\`\"x\n", "t.v:1:14: error: syntax: '`\\`\"' is a SystemVerilog macro "
                                   "operator; not Verilog-2005 (Phase 5)"},
        {"`define L a + `__LINE__\n", "t.v:1:15: error: syntax: '`__LINE__' is a SystemVerilog "
                                      "macro operator; not Verilog-2005 (Phase 5)"},
        {"`define F `__FILE__\n", "t.v:1:11: error: syntax: '`__FILE__' is a SystemVerilog macro "
                                  "operator; not Verilog-2005 (Phase 5)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        errors = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run(cases[i].text), cases[i].text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].want, captured, cases[i].text);
    }
    /* inside a string they are plain bytes */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define OK \"a``b `\\\" `__FILE__\"\n"));
    assert_body(macro("OK"), "\"a``b `\\\" `__FILE__\"");
}

/* --- caps ---------------------------------------------------------------------------------- */

static void test_cap_macro_args(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.macro_args = 2;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define F(a, b) a\n"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define G(a, b, c) a\n"));
    assert_error("t.v:1:17: error: syntax: formals of macro 'G' exceeds "
                 "ODIN3_PP_MAX_MACRO_ARGS (2)");
}

static void test_cap_macro_text(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.macro_text = 4;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define F 1234\n"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define G 12345\n"));
    assert_error("t.v:1:11: error: syntax: text of macro 'G' exceeds "
                 "ODIN3_PP_MAX_MACRO_TEXT (4)");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define H(a = 12345) a\n"));
    assert_error("t.v:1:15: error: syntax: text of macro 'H' exceeds "
                 "ODIN3_PP_MAX_MACRO_TEXT (4)");
}

static void test_cap_macros(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.macros = 2;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define A 1\n`define B 1\n`define A 2\n"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define C 1\n"));
    assert_error("t.v:1:9: error: syntax: macros exceeds ODIN3_PP_MAX_MACROS (2)");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`undef A\n`define C 1\n"));
}

static void test_cap_definitions_counts_redefinitions(void) {
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.definitions = 3;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define A 1\n`undef A\n`define A 2\n`define A 3\n"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define A 4\n"));
    assert_error("t.v:1:9: error: syntax: definitions exceeds ODIN3_PP_MAX_DEFINITIONS (3)");
    assert_body(macro("A"), "3");
}

/* A file redefining one macro n times. */
static odin3_status run_redefinitions(uint32_t n, const char *line) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(line)));
    }
    odin3_status st = run_bytes("loop.v", (odin3_bytes){text.data, text.len});
    odin3_strbuf_free(&text);
    return st;
}

static void test_redefinition_loop_is_bounded(void) {
    static const char k_line[] = "`define a(b,c,d,e,f,g,h,i,j,k) b+c+d+e+f+g+h+i+j+k\n";
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.definitions = 100;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_redefinitions(2000, k_line));
    assert_error("loop.v:101:9: error: syntax: definitions exceeds ODIN3_PP_MAX_DEFINITIONS (100)");
    TEST_ASSERT_EQUAL_size_t(100, pp->macros.len);
}

static void test_cap_macro_bytes(void) {
    static const char k_line[] = "`define a(b,c,d,e,f,g,h,i,j,k) b+c+d+e+f+g+h+i+j+k\n";
    enum { CAP = 4 * ODIN3_PP_ARENA_CHUNK };
    ensure_pp();
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.macro_bytes = CAP;
    odin3_pp_test_set_limits(pp, limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run_redefinitions(4000, k_line));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NOT_NULL(strstr(captured, ": error: syntax: macro bytes exceeds "
                                          "ODIN3_PP_MAX_MACRO_BYTES (262144)"));
    TEST_ASSERT_TRUE(odin3_arena_bytes_reserved(pp->arena) <= CAP);
    TEST_ASSERT_TRUE(pp->macros.len > 100 && pp->macros.len < 4000);
}

static void test_redefinitions_within_the_default_caps(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_redefinitions(3000, "`define a(b, c = 1) b + c\n"));
    TEST_ASSERT_EQUAL_size_t(3000, pp->macros.len);
    TEST_ASSERT_TRUE(odin3_arena_bytes_reserved(pp->arena) <= ODIN3_PP_MAX_MACRO_BYTES);
}

static void test_file_and_line_are_not_macro_names(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`define __FILE__ 1\n"));
    assert_error("t.v:1:9: error: syntax: compiler directive '__FILE__' cannot be a macro name");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("`undef __LINE__\n"));
    assert_error("t.v:1:8: error: syntax: compiler directive '__LINE__' cannot be a macro name");
}

/* --- <command line> ------------------------------------------------------------------------ */

static void lower_macro_text(odin3_pp_limits *limits) {
    limits->macro_text = 2;
}

static void lower_macro_args(odin3_pp_limits *limits) {
    limits->macro_args = 1;
}

static void lower_macros(odin3_pp_limits *limits) {
    limits->macros = 2;
}

static void lower_definitions(odin3_pp_limits *limits) {
    limits->definitions = 1;
}

static void test_command_line_defines(void) {
    odin3_pp_define defines[] = {
        {odin3_bytes_cstr("A"), odin3_bytes_cstr("1"), true},
        {odin3_bytes_cstr("B"), {NULL, 0}, false},
        {odin3_bytes_cstr("C"), odin3_bytes_cstr(""), true},
        {odin3_bytes_cstr("D"), odin3_bytes_cstr("a // b \\"), true},
        {odin3_bytes_cstr("E(x,y)"), odin3_bytes_cstr("x /* y"), true},
    };
    odin3_pp_config config = {defines, 5, NULL, 0};
    create_with(&config);
    assert_body(macro("A"), "1");
    assert_body(macro("B"), "");
    assert_body(macro("C"), "");
    assert_body(macro("D"), "a // b \\");
    assert_body(macro("E"), "x /* y");
    TEST_ASSERT_EQUAL_UINT32(2, macro("E")->nformals);
    assert_pos(macro("A")->def, (line_col){1, 11});
    assert_pos(macro("D")->def, (line_col){4, 11});
    odin3_strbuf where;
    odin3_strbuf_init(&where);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_srcman_format(sm, macro("E")->def, &where, ODIN3_SRCFMT_GIVEN));
    TEST_ASSERT_EQUAL_STRING("<command line>:5:16", where.data);
    odin3_strbuf_free(&where);
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(
        odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, macro("A")->def), &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.resolved);
    TEST_ASSERT_EQUAL_UINT32(0, info.library);
    TEST_ASSERT_EQUAL_UINT32(0, info.parent);
    TEST_ASSERT_EQUAL_UINT32(6, info.lines); /* five lines and the empty one after the last LF */
    /* a file sees them, and its own define replaces one */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("`define A 2\n"));
    assert_body(macro("A"), "2");
    assert_body(macro("B"), "");
}

static void test_command_line_errors(void) {
    static const struct {
        const char *name, *value;
        const char *want;
    } cases[] = {
        {"9bad", "1", "<command line>:3:9: error: syntax: expected a macro name after `define"},
        {"S", "\"abc", "<command line>:3:11: error: syntax: unterminated string in macro text"},
        {"F(a b)", "a",
         "<command line>:3:13: error: syntax: expected ',' or ')' in the formal list of macro 'F'"},
        {"F(1)", "a",
         "<command line>:3:11: error: syntax: formal argument of macro 'F' is not an identifier"},
        {"timescale", NULL,
         "<command line>:3:9: error: syntax: compiler directive 'timescale' cannot be a macro "
         "name"},
        {"L", "x\ny",
         "<command line>:3:12: error: syntax: newline in the text of project "
         "define 'L'"},
        {"P", "a``b",
         "<command line>:3:12: error: syntax: '``' is a SystemVerilog macro "
         "operator; not Verilog-2005 (Phase 5)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        odin3_pp_define defines[] = {
            {odin3_bytes_cstr("OK1"), odin3_bytes_cstr("1"), true},
            {odin3_bytes_cstr("OK2"), {NULL, 0}, false},
            {odin3_bytes_cstr(cases[i].name), odin3_bytes_cstr(cases[i].value),
             cases[i].value != NULL},
        };
        odin3_pp_config config = {defines, 3, NULL, 0};
        errors = 0;
        odin3_pp *bad = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, odin3_pp_create(design, &config, &bad),
                                      cases[i].name);
        TEST_ASSERT_NULL(bad);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].want, captured, cases[i].name);
        TEST_ASSERT_EQUAL_INT(1, errors);
    }
}

/* The definition caps apply to project defines too, through odin3_pp_test_create. */
static void test_command_line_caps(void) {
    static const struct {
        void (*lower)(odin3_pp_limits *limits);
        const char *want;
    } cases[] = {
        {lower_macro_text, "<command line>:2:11: error: syntax: text of macro 'B' exceeds "
                           "ODIN3_PP_MAX_MACRO_TEXT (2)"},
        {lower_macro_args, "<command line>:3:13: error: syntax: formals of macro 'C' exceeds "
                           "ODIN3_PP_MAX_MACRO_ARGS (1)"},
        {lower_macros, "<command line>:3:9: error: syntax: macros exceeds ODIN3_PP_MAX_MACROS (2)"},
        {lower_definitions,
         "<command line>:2:9: error: syntax: definitions exceeds ODIN3_PP_MAX_DEFINITIONS (1)"},
    };
    odin3_pp_define defines[] = {
        {odin3_bytes_cstr("A"), odin3_bytes_cstr("1"), true},
        {odin3_bytes_cstr("B"), odin3_bytes_cstr("123"), true},
        {odin3_bytes_cstr("C(x,y)"), odin3_bytes_cstr("x"), true},
    };
    odin3_pp_config config = {defines, 3, NULL, 0};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        odin3_pp_limits limits = odin3_pp_default_limits();
        cases[i].lower(&limits);
        errors = 0;
        odin3_pp *bad = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_pp_test_create(design, &config, limits, &bad));
        TEST_ASSERT_NULL(bad);
        TEST_ASSERT_EQUAL_STRING(cases[i].want, captured);
        TEST_ASSERT_EQUAL_INT(1, errors);
    }
}

static void test_create_misuse(void) {
    odin3_pp_config none = {NULL, 0, NULL, 0};
    odin3_pp *got = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_create(NULL, &none, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_create(design, NULL, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_create(design, &none, NULL));
    odin3_pp_config bad = {NULL, 1, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_create(design, &bad, &got));
    odin3_pp_destroy(NULL);
}

/* --- out of memory ------------------------------------------------------------------------- */

static const char k_oom_text[] = "`define A(x, y = (1, 2)) x + \\\n y // c\n"
                                 "`define B /* b */ 2\n`undef A\n`timescale 1ns/1ps\n"
                                 "module m; wire w; endmodule\n";

/* The outcome of one try: NO_MEMORY with nothing returned, or the full result. */
static bool oom_check(odin3_status st) {
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(stream.text.ptr);
        return false;
    }
    assert_text("\n\n\n`timescale 1ns/1ps\nmodule m; wire w; endmodule\n");
    assert_body(macro("B"), "/* b */ 2");
    TEST_ASSERT_NULL(macro("A"));
    return true;
}

/* One try on a fresh design: create with two defines and run; true when both succeeded. */
static bool oom_try(long fail_after, const char *path) {
    odin3_pp_destroy(pp);
    pp = NULL;
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_pp_define defines[] = {{odin3_bytes_cstr("C"), odin3_bytes_cstr("3"), true},
                                 {odin3_bytes_cstr("D(a)"), odin3_bytes_cstr("a"), true}};
    odin3_pp_config config = {defines, 2, NULL, 0};
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
    TEST_ASSERT_TRUE(tries > 20); /* the sweep really hit many allocation points */
    TEST_ASSERT_EQUAL_INT(0, errors);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_define);
    RUN_TEST(test_define_with_args);
    RUN_TEST(test_empty_formal_list);
    RUN_TEST(test_paren_after_blank_is_body);
    RUN_TEST(test_defaults);
    RUN_TEST(test_empty_default);
    RUN_TEST(test_multi_line_define);
    RUN_TEST(test_crlf_continuation);
    RUN_TEST(test_redefinition_is_silent);
    RUN_TEST(test_line_comment_ends_the_text);
    RUN_TEST(test_block_comment_kept_in_text);
    RUN_TEST(test_block_comment_across_continuation);
    RUN_TEST(test_string_protects_line_comment);
    RUN_TEST(test_escaped_identifier_in_body);
    RUN_TEST(test_empty_body);
    RUN_TEST(test_define_at_end_of_file);
    RUN_TEST(test_define_mid_line);
    RUN_TEST(test_macros_persist_across_runs);
    RUN_TEST(test_probe_t3_body_spans_the_continuation);
    RUN_TEST(test_probe_t4_comment_continues_nothing);
    RUN_TEST(test_probe_u1_multi_line_comment_is_unterminated);
    RUN_TEST(test_probe_t9_resetall_keeps_macros);
    RUN_TEST(test_undef);
    RUN_TEST(test_undef_then_redefine);
    RUN_TEST(test_undef_errors);
    RUN_TEST(test_define_errors);
    RUN_TEST(test_sv_operators_in_a_body);
    RUN_TEST(test_cap_macro_args);
    RUN_TEST(test_cap_macro_text);
    RUN_TEST(test_cap_macros);
    RUN_TEST(test_cap_definitions_counts_redefinitions);
    RUN_TEST(test_redefinition_loop_is_bounded);
    RUN_TEST(test_cap_macro_bytes);
    RUN_TEST(test_redefinitions_within_the_default_caps);
    RUN_TEST(test_file_and_line_are_not_macro_names);
    RUN_TEST(test_command_line_defines);
    RUN_TEST(test_command_line_errors);
    RUN_TEST(test_command_line_caps);
    RUN_TEST(test_create_misuse);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
