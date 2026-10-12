/*
 * test_verilog_lexer.c — the Verilog lexer (2C spec §2, VP-2..VP-10) on hand-built single-segment
 * streams: every token class with its value and exact {loc, end, off}, every located error and
 * warning, comments and metacomments, keyword sets, directives, opaque bodies, the review-focus
 * shapes (netlist names, CRLF, edges of a file, later keywords, non-ASCII), the error-mode
 * binding, the token map and an OOM sweep. Macro- and include-dependent cases are Task 8's.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/parser_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"
#include "verilog_test.h"

#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OOM_LIMIT = 4000, OOM_TAIL = 50, STRING_CAP = 1 << 17 };

static odin3_design *design;
static odin3_ast *ast;
static odin3_vl_run run;
static odin3_vp_stream stream;
static odin3_vl_lexer *lx;
static bool err_flag;
static vt_result captured; /* only its log and counters */
static const char *source; /* the text of the open stream */
static size_t source_len;
static ODIN3_VP_STYPE val;
static odin3_vp_loc loc;

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    odin3_vl_run_init(&run, design);
    lx = NULL;
    memset(&captured, 0, sizeof captured);
    odin3_strbuf_init(&captured.log);
    odin3_log_set_sink(vt_sink, &captured);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_vl_close(lx);
    lx = NULL;
    odin3_log_set_sink(NULL, NULL);
    odin3_strbuf_free(&captured.log);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers --------------------------------------------------------------------------------- */

static const char *logged(void) {
    return captured.log.data != NULL ? captured.log.data : "";
}

/* Opens a lexer over text[0..len) as the file t.v, its error flag bound to err_flag. */
static void open_n(const char *text, size_t len) {
    odin3_vl_close(lx);
    lx = NULL;
    source = text;
    source_len = len;
    vt_stream(design, "t.v", (odin3_bytes){text, len}, &stream);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &stream, &lx));
    err_flag = false;
    odin3_vl_bind_error_mode(lx, &err_flag);
}

static void open_text(const char *text) {
    open_n(text, strlen(text));
}

/* The loc of a stream offset (one segment: the stream is its FILE buffer). */
static odin3_loc at(uint32_t offset) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    return odin3_srcman_loc(sm, stream.stream, offset);
}

/* The offset of the first occurrence of needle in the open stream. */
static uint32_t off_of(const char *needle) {
    const char *hit = strstr(source, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(hit, needle);
    return (uint32_t)(hit - source);
}

static int next(void) {
    return odin3_vl_next(lx, &val, &loc);
}

static void expect_range(uint32_t off, uint32_t len) {
    TEST_ASSERT_EQUAL_UINT32(at(off).v, loc.loc.v);
    TEST_ASSERT_EQUAL_UINT32(at(off + len).v, loc.end.v);
    TEST_ASSERT_EQUAL_UINT32(off, loc.off);
}

/* Stream bytes [off, off + len). */
typedef struct tok_range {
    uint32_t off, len;
} tok_range;

/* The next token is kind over at. */
static void expect_tok(int kind, tok_range where) {
    TEST_ASSERT_EQUAL_INT(kind, next());
    expect_range(where.off, where.len);
}

/* ... with the byte-view value text. */
static void expect_text(int kind, tok_range where, const char *text) {
    expect_tok(kind, where);
    TEST_ASSERT_EQUAL_size_t(strlen(text), val.text.len);
    if (strlen(text) > 0) {
        TEST_ASSERT_EQUAL_MEMORY(text, val.text.ptr, strlen(text));
    }
}

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
}

/* ... with the interned value name. */
static void expect_name(int kind, tok_range where, const char *name) {
    expect_tok(kind, where);
    TEST_ASSERT_EQUAL_STRING(name, str_of(val.u));
}

/* An identifier spelled at its first occurrence, its name the spelling. */
static void expect_ident(const char *word) {
    expect_name(TOK_IDENT, (tok_range){off_of(word), (uint32_t)strlen(word)}, word);
}

/* A token of kind spelled word at its first occurrence. */
static void expect_word(int kind, const char *word) {
    expect_tok(kind, (tok_range){off_of(word), (uint32_t)strlen(word)});
}

/* The end of input after a last token ending at offset end. */
static void expect_eof(uint32_t end) {
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next());
    TEST_ASSERT_EQUAL_UINT32(at(end).v, loc.loc.v);
    TEST_ASSERT_EQUAL_UINT32(at(end).v, loc.end.v);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)source_len, loc.off);
}

/* The comments recorded (seals the store). */
static odin3_ast_comment_span comments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    return odin3_ast_comments_of(ast, stream.stream);
}

static void expect_comment(const odin3_ast_comment *cm, tok_range where, const char *text) {
    TEST_ASSERT_EQUAL_UINT32(at(where.off).v, cm->loc.v);
    TEST_ASSERT_EQUAL_UINT32(at(where.off + where.len).v, cm->end.v);
    TEST_ASSERT_EQUAL_STRING(text, cm->text);
}

/* The value of the REAL token's payload (read through a REAL node) is want. */
static void expect_real(double want) {
    odin3_ast_spec spec = {
        .kind = ODIN3_AST_REAL, .loc = at(0).v, .end = at(1).v, .payload = val.u};
    odin3_ast_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &node));
    double got = odin3_ast_real(ast, node);
    TEST_ASSERT_TRUE_MESSAGE(fabs(got - want) <= 1e-12 * fabs(want), "real value");
}

/* A heap text of n copies of fill between prefix and suffix (caller frees). */
static char *repeat(const char *prefix, char fill, size_t count, const char *suffix) {
    size_t lp = strlen(prefix);
    size_t ls = strlen(suffix);
    char *text = malloc(lp + count + ls + 1);
    TEST_ASSERT_NOT_NULL(text);
    (void)snprintf(text, lp + 1, "%s", prefix);
    memset(text + lp, fill, count);
    (void)snprintf(text + lp + count, ls + 1, "%s", suffix);
    return text;
}

/* --- keywords.c ------------------------------------------------------------------------------ */

static void test_keyword_table_sorted_and_found(void) {
    size_t count = 0;
    const odin3_vl_kw *table = odin3_vl_keywords(&count);
    TEST_ASSERT_EQUAL_size_t(124, count);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(table[i - 1].word, table[i].word) < 0, table[i].word);
        }
        TEST_ASSERT_EQUAL_INT(table[i].token,
                              odin3_vl_keyword(odin3_bytes_cstr(table[i].word), ODIN3_VL_KW_2005));
    }
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("logic"), ODIN3_VL_KW_2005));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("Module"), ODIN3_VL_KW_2005));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("modul"), ODIN3_VL_KW_2005));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr(""), ODIN3_VL_KW_2005));
}

/* The sets of a word per §2.5: the 2001 words, the config words among them, uwire, the rest. */
static uint32_t expected_sets(const char *word) {
    static const char *const new2001[] = {"automatic",
                                          "endgenerate",
                                          "generate",
                                          "genvar",
                                          "localparam",
                                          "noshowcancelled",
                                          "pulsestyle_ondetect",
                                          "pulsestyle_onevent",
                                          "showcancelled",
                                          "signed",
                                          "unsigned"};
    static const char *const config[] = {"cell",    "config",   "design",  "endconfig", "incdir",
                                         "include", "instance", "liblist", "library",   "use"};
    for (size_t i = 0; i < sizeof new2001 / sizeof new2001[0]; i++) {
        if (strcmp(word, new2001[i]) == 0) {
            return ODIN3_VL_KW_2001_NOCONFIG | ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005;
        }
    }
    for (size_t i = 0; i < sizeof config / sizeof config[0]; i++) {
        if (strcmp(word, config[i]) == 0) {
            return ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005;
        }
    }
    if (strcmp(word, "uwire") == 0) {
        return ODIN3_VL_KW_2005;
    }
    return ODIN3_VL_KW_1995 | ODIN3_VL_KW_2001_NOCONFIG | ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005;
}

/* Every word of the table against every set: a keyword exactly in the sets §2.5 gives it. */
static void test_keyword_masks_every_word(void) {
    const uint32_t sets[] = {ODIN3_VL_KW_1995, ODIN3_VL_KW_2001_NOCONFIG, ODIN3_VL_KW_2001,
                             ODIN3_VL_KW_2005};
    size_t count = 0;
    const odin3_vl_kw *table = odin3_vl_keywords(&count);
    size_t in2001 = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t want = expected_sets(table[i].word);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, table[i].mask, table[i].word);
        in2001 += (want & ODIN3_VL_KW_1995) == 0 && want != ODIN3_VL_KW_2005;
        for (size_t j = 0; j < sizeof sets / sizeof sets[0]; j++) {
            int token = (want & sets[j]) != 0 ? table[i].token : 0;
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                token, odin3_vl_keyword(odin3_bytes_cstr(table[i].word), sets[j]), table[i].word);
        }
    }
    TEST_ASSERT_EQUAL_size_t(21, in2001); /* every 2001 word of §2.5 is in the table */
}

static void test_keyword_sets(void) {
    const uint32_t k95 = ODIN3_VL_KW_1995;
    const uint32_t knc = ODIN3_VL_KW_2001_NOCONFIG;
    const uint32_t k01 = ODIN3_VL_KW_2001;
    TEST_ASSERT_EQUAL_INT(TOK_MODULE, odin3_vl_keyword(odin3_bytes_cstr("module"), k95));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("generate"), k95));
    TEST_ASSERT_EQUAL_INT(TOK_GENERATE, odin3_vl_keyword(odin3_bytes_cstr("generate"), knc));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("config"), knc));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("liblist"), knc));
    TEST_ASSERT_EQUAL_INT(TOK_OPAQUE_CONFIG, odin3_vl_keyword(odin3_bytes_cstr("config"), k01));
    TEST_ASSERT_EQUAL_INT(0, odin3_vl_keyword(odin3_bytes_cstr("uwire"), k01));
    TEST_ASSERT_EQUAL_INT(TOK_UWIRE, odin3_vl_keyword(odin3_bytes_cstr("uwire"), ODIN3_VL_KW_2005));
    TEST_ASSERT_EQUAL_INT(TOK_OPAQUE_SPECIFY, odin3_vl_keyword(odin3_bytes_cstr("specify"), k95));
    TEST_ASSERT_EQUAL_INT(TOK_OPAQUE_PRIMITIVE,
                          odin3_vl_keyword(odin3_bytes_cstr("primitive"), k95));
    TEST_ASSERT_EQUAL_INT(TOK_KW_NAND, odin3_vl_keyword(odin3_bytes_cstr("nand"), k95));
    uint32_t mask = 0;
    const char *versions[] = {"\"1364-1995\"", "\"1364-2001-noconfig\"", "\"1364-2001\"",
                              "\"1364-2005\""};
    const uint32_t masks[] = {k95, knc, k01, ODIN3_VL_KW_2005};
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(odin3_vl_keyword_version(odin3_bytes_cstr(versions[i]), &mask));
        TEST_ASSERT_EQUAL_UINT32(masks[i], mask);
    }
    TEST_ASSERT_FALSE(odin3_vl_keyword_version(odin3_bytes_cstr("\"1800-2005\""), &mask));
    TEST_ASSERT_FALSE(odin3_vl_keyword_version(odin3_bytes_cstr("1364-2005"), &mask));
}

/* --- numbers (§2.3) -------------------------------------------------------------------------- */

/* A '_' right after the base is an error at the '_' (PHASE2 #8); the BASED exit puts it back. */
static void test_based_underscore(void) {
    open_text("'h_F");
    expect_text(TOK_BASE, (tok_range){0, 2}, "'h");
    expect_ident("_F");
    TEST_ASSERT_EQUAL_STRING("t.v:1:3: error: digits after 'h cannot start with '_'\n", logged());
    TEST_ASSERT_TRUE(err_flag);
    expect_eof(4);
    TEST_ASSERT_EQUAL_UINT32(1, captured.errors);
}

/* Blanks and comments between the pieces (L8); the comments are recorded. */
static void test_spaced_number_with_comments(void) {
    open_text("8 /*c*/ 'h /*d*/ FF // e\n");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){0, 1}, "8");
    expect_text(TOK_BASE, (tok_range){off_of("'h"), 2}, "'h");
    expect_text(TOK_BASED_DIGITS, (tok_range){off_of("FF"), 2}, "FF");
    expect_eof(off_of("FF") + 2);
    TEST_ASSERT_EQUAL_STRING("", logged());
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(3, span.n);
    expect_comment(&span.items[0], (tok_range){2, 5}, "c");
    expect_comment(&span.items[1], (tok_range){off_of("/*d"), 5}, "d");
    expect_comment(&span.items[2], (tok_range){off_of("//"), 4}, " e");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_BLOCK, span.items[1].kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_LINE, span.items[2].kind);
}

static void test_based_digits(void) {
    open_text("4'sb1?0z 'b12 'hx_1");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){0, 1}, "4");
    expect_text(TOK_BASE, (tok_range){1, 3}, "'sb");
    expect_text(TOK_BASED_DIGITS, (tok_range){4, 4}, "1?0z");
    expect_text(TOK_BASE, (tok_range){9, 2}, "'b"); /* the per-base digit check is number_parse's */
    expect_text(TOK_BASED_DIGITS, (tok_range){11, 2}, "12");
    expect_text(TOK_BASE, (tok_range){14, 2}, "'h");
    expect_text(TOK_BASED_DIGITS, (tok_range){16, 3}, "x_1");
    expect_eof(19);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* A base at the end of input: back to INITIAL, then the end. */
static void test_base_at_end_of_input(void) {
    open_text("8'd");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){0, 1}, "8");
    expect_text(TOK_BASE, (tok_range){1, 2}, "'d");
    expect_eof(3);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_reals(void) {
    open_text("1e3 1.5e-2 1_000.5 2E+1_0");
    expect_tok(TOK_REAL_NUMBER, (tok_range){0, 3});
    expect_real(1000.0);
    expect_tok(TOK_REAL_NUMBER, (tok_range){4, 6});
    expect_real(0.015);
    expect_tok(TOK_REAL_NUMBER, (tok_range){11, 7});
    expect_real(1000.5);
    expect_tok(TOK_REAL_NUMBER, (tok_range){19, 6});
    expect_real(2e10);
    expect_eof(25);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* 1. and .5 are not reals (1364-2005 §3.5.2). */
/* A host's comma-decimal LC_NUMERIC must not change a real (the lexer parses in the C locale)
 * and is still in force afterwards. Needs a comma locale (LOCPATH may point at one); without
 * one only the restoration is checked. */
static void test_reals_ignore_the_host_locale(void) {
    const char *names[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de_DE"};
    locale_t comma = (locale_t)0;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && comma == (locale_t)0; i++) {
        comma = newlocale(LC_NUMERIC_MASK, names[i], (locale_t)0);
    }
    if (comma == (locale_t)0) {
        TEST_MESSAGE("no comma-decimal locale installed: only the restoration is checked");
        comma = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    }
    TEST_ASSERT_TRUE(comma != (locale_t)0);
    locale_t before = uselocale(comma);
    open_text("1.5 2.25e1");
    TEST_ASSERT_EQUAL_INT(TOK_REAL_NUMBER, next());
    expect_real(1.5);
    TEST_ASSERT_EQUAL_INT(TOK_REAL_NUMBER, next());
    expect_real(22.5);
    TEST_ASSERT_TRUE(uselocale((locale_t)0) == comma);
    (void)uselocale(before);
    freelocale(comma);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_not_reals(void) {
    open_text("1. .5 a.5");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){0, 1}, "1");
    expect_tok('.', (tok_range){1, 1});
    expect_tok('.', (tok_range){3, 1});
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){4, 1}, "5");
    expect_name(TOK_IDENT, (tok_range){6, 1}, "a");
    expect_tok('.', (tok_range){7, 1});
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){8, 1}, "5");
    expect_eof(9);
}

static void test_real_out_of_range(void) {
    open_text("1e400 1e-400");
    expect_tok(TOK_REAL_NUMBER, (tok_range){0, 5});
    TEST_ASSERT_EQUAL_UINT32(0, val.u);
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: real literal out of range\n", logged());
    TEST_ASSERT_TRUE(err_flag);
    expect_tok(TOK_REAL_NUMBER, (tok_range){6, 6}); /* underflow is finite */
    TEST_ASSERT_NOT_EQUAL(0, val.u);
    TEST_ASSERT_EQUAL_UINT32(1, captured.errors);
}

/* `5 'b1` and `#5 'b1` lex alike; the grammar tells a delay from a size (§2.3). */
static void test_delay_and_size_lex_alike(void) {
    open_text("5 'b1 #5 'b1");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){0, 1}, "5");
    expect_text(TOK_BASE, (tok_range){2, 2}, "'b");
    expect_text(TOK_BASED_DIGITS, (tok_range){4, 1}, "1");
    expect_tok('#', (tok_range){6, 1});
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){7, 1}, "5");
    expect_text(TOK_BASE, (tok_range){9, 2}, "'b");
    expect_text(TOK_BASED_DIGITS, (tok_range){11, 1}, "1");
    expect_eof(12);
}

/* --- identifiers, system names, keywords (§2.4) -------------------------------------------- */

static void test_escaped_identifiers(void) {
    open_text("\\a+b \\module \\wire\t\\x\\y\r\n");
    expect_name(TOK_IDENT, (tok_range){0, 4}, "a+b");
    expect_name(TOK_IDENT, (tok_range){5, 7}, "module");
    expect_name(TOK_IDENT, (tok_range){13, 5}, "wire"); /* never WIRE */
    expect_name(TOK_IDENT, (tok_range){19, 4}, "x\\y");
    expect_eof(23);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_escaped_identifier_without_name(void) {
    open_text("\\ x \\");
    expect_ident("x");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: empty escaped identifier\n", logged());
    TEST_ASSERT_TRUE(err_flag);
    expect_eof(3);
    TEST_ASSERT_EQUAL_UINT32(2, captured.errors);
}

/* The identifier cap (hook: 8): an error, the token kept at the cap. */
static void test_identifier_cap(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.ident_bytes = 8});
    open_text("abcdefghi \\abcdefghij  abcdefgh");
    expect_name(TOK_IDENT, (tok_range){0, 9}, "abcdefgh");
    expect_name(TOK_IDENT, (tok_range){10, 11}, "abcdefgh");
    expect_name(TOK_IDENT, (tok_range){23, 8}, "abcdefgh");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: identifier longer than 8 bytes\n"
                             "t.v:1:11: error: identifier longer than 8 bytes\n",
                             logged());
}

/* The real cap: 4097 bytes. */
static void test_identifier_cap_default(void) {
    char *text = repeat("", 'a', ODIN3_AST_MAX_IDENT_BYTES + 1, "");
    open_text(text);
    TEST_ASSERT_EQUAL_INT(TOK_IDENT, next());
    TEST_ASSERT_EQUAL_size_t(ODIN3_AST_MAX_IDENT_BYTES, strlen(str_of(val.u)));
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: identifier longer than 4096 bytes\n", logged());
    odin3_vl_close(lx);
    lx = NULL;
    free(text);
}

static void test_system_names(void) {
    open_text("$display $signed $ $a$b");
    expect_name(TOK_SYSTEM_IDENT, (tok_range){0, 8}, "$display");
    expect_name(TOK_SYSTEM_IDENT, (tok_range){9, 7}, "$signed");
    expect_name(TOK_SYSTEM_IDENT, (tok_range){19, 4}, "$a$b");
    TEST_ASSERT_EQUAL_STRING("t.v:1:18: error: unexpected byte 0x24\n", logged());
    expect_eof(23);
}

static void test_keywords_and_identifiers(void) {
    open_text("module wire uwire config_x macromodule nand _x a$1 endspecify");
    expect_word(TOK_MODULE, "module");
    expect_word(TOK_WIRE, "wire");
    expect_word(TOK_UWIRE, "uwire");
    expect_ident("config_x");
    expect_word(TOK_MACROMODULE, "macromodule");
    expect_word(TOK_KW_NAND, "nand");
    expect_name(TOK_IDENT, (tok_range){44, 2}, "_x");
    expect_ident("a$1");
    expect_word(TOK_ENDSPECIFY, "endspecify");
    expect_eof((uint32_t)source_len);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* Review focus 4: SystemVerilog keywords are Verilog-2005 identifiers. */
static void test_later_keywords_are_identifiers(void) {
    open_text("logic bit int byte interface");
    expect_ident("logic");
    expect_ident("bit");
    expect_ident("int");
    expect_ident("byte");
    expect_ident("interface");
    expect_eof((uint32_t)source_len);
}

static void test_operators(void) {
    open_text("<<< >>> === !== ~^ ^~ ~& ~| ** +: -: -> <= >= == != && || << >> "
              "+ - * / % ! ~ & | ^ < > = ? : ; , . ( ) [ ] { } @ #");
    const int ops[] = {TOK_ASHL,        TOK_ASHR,    TOK_CASE_EQ, TOK_CASE_NE, TOK_XNOR,
                       TOK_XNOR,        TOK_NAND,    TOK_NOR,     TOK_POW,     TOK_PLUS_COLON,
                       TOK_MINUS_COLON, TOK_TRIGGER, TOK_LE,      TOK_GE,      TOK_EQ,
                       TOK_NE,          TOK_AND_AND, TOK_OR_OR,   TOK_SHL,     TOK_SHR};
    uint32_t off = 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        uint32_t len = i < 4 ? 3 : 2;
        expect_tok(ops[i], (tok_range){off, len});
        off += len + 1;
    }
    for (const char *punct = "+-*/%!~&|^<>=?:;,.()[]{}@#"; *punct != '\0'; punct++) {
        expect_tok(*punct, (tok_range){off, 1});
        off += 2;
    }
    expect_eof(off - 1);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* --- keyword sets (§2.5) ------------------------------------------------------------------- */

static void test_begin_keywords_1995(void) {
    open_text("`begin_keywords \"1364-1995\"\ngenerate\n`end_keywords\ngenerate");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 27}, "\"1364-1995\"");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_BEGIN_KEYWORDS, odin3_vl_directive_kind(lx, 0));
    TEST_ASSERT_EQUAL_UINT32(1, run.kw_depth);
    expect_name(TOK_IDENT, (tok_range){28, 8}, "generate");
    expect_text(TOK_DIRECTIVE, (tok_range){37, 13}, "");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_END_KEYWORDS, odin3_vl_directive_kind(lx, 37));
    TEST_ASSERT_EQUAL_UINT32(0, run.kw_depth);
    expect_tok(TOK_GENERATE, (tok_range){51, 8});
    expect_eof(59);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_begin_keywords_2001_and_noconfig(void) {
    open_text("`begin_keywords \"1364-2001\" uwire signed\n"
              "`begin_keywords \"1364-2001-noconfig\" config c; endconfig\n"
              "`end_keywords `end_keywords config");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 27});
    expect_ident("uwire");
    expect_word(TOK_SIGNED, "signed");
    expect_text(TOK_DIRECTIVE, (tok_range){off_of("`begin_keywords \"1364-2001-"), 36},
                "\"1364-2001-noconfig\"");
    expect_name(TOK_IDENT, (tok_range){78, 6}, "config"); /* no opaque region */
    expect_name(TOK_IDENT, (tok_range){85, 1}, "c");
    expect_tok(';', (tok_range){86, 1});
    expect_name(TOK_IDENT, (tok_range){88, 9}, "endconfig");
    expect_tok(TOK_DIRECTIVE, (tok_range){98, 13});
    expect_tok(TOK_DIRECTIVE, (tok_range){112, 13});
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next()); /* config under 2005 opens a body: unterminated */
    TEST_ASSERT_EQUAL_STRING("t.v:3:29: error: 'config' has no matching 'endconfig'\n", logged());
}

static void test_begin_keywords_unknown_version(void) {
    open_text("`begin_keywords \"1800-2005\" logic");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 27}, "\"1800-2005\"");
    TEST_ASSERT_EQUAL_UINT32(0, run.kw_depth);
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: unknown `begin_keywords version \"1800-2005\"\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
    expect_ident("logic");
    expect_eof((uint32_t)source_len);
}

static void test_begin_keywords_malformed(void) {
    open_text("`begin_keywords\n`begin_keywords 1364");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 15}, "");
    expect_text(TOK_DIRECTIVE, (tok_range){16, 15}, "");
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){32, 4}, "1364");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: `begin_keywords needs a version string\n"
                             "t.v:2:1: error: `begin_keywords needs a version string\n",
                             logged());
    TEST_ASSERT_EQUAL_UINT32(0, run.kw_depth);
}

static void test_end_keywords_on_empty_stack(void) {
    open_text("`end_keywords");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 13});
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: warning: `end_keywords without a matching `begin_keywords\n",
                             logged());
    TEST_ASSERT_FALSE(err_flag);
    TEST_ASSERT_EQUAL_UINT32(0, run.kw_depth);
}

/* Point (k): `resetall leaves the keyword stack alone (§2.5, §19.11 pairs each set with its
 * `end_keywords). */
static void test_resetall_keeps_the_set(void) {
    open_text("`begin_keywords \"1364-1995\" `resetall generate `end_keywords");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 27});
    expect_text(TOK_DIRECTIVE, (tok_range){off_of("`resetall"), 9}, "");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_RESETALL,
                             odin3_vl_directive_kind(lx, off_of("`resetall")));
    TEST_ASSERT_EQUAL_UINT32(1, run.kw_depth);
    expect_ident("generate");
    expect_tok(TOK_DIRECTIVE, (tok_range){off_of("`end"), 13});
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* A set left open warns at the stream's end and applies to the run's next stream (§2.5). */
static void test_open_set_spans_streams(void) {
    open_text("`begin_keywords \"1364-2001\"\n// c\n");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 27});
    expect_eof(27);
    TEST_ASSERT_EQUAL_STRING("t.v:1:28: warning: `begin_keywords \"1364-2001\" (t.v:1:1) is "
                             "still open at the end of the file\n",
                             logged());
    size_t len = captured.log.len;
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next()); /* reported once */
    TEST_ASSERT_EQUAL_size_t(len, captured.log.len);
    odin3_strbuf_clear(&captured.log);
    open_text("uwire");
    expect_ident("uwire");
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next());
    TEST_ASSERT_EQUAL_STRING("t.v:1:6: warning: `begin_keywords \"1364-2001\" (t.v:1:1) is "
                             "still open at the end of the file\n",
                             logged());
    TEST_ASSERT_FALSE(err_flag);
}

/* The keyword-stack cap (hook: 2): the third set is an error and is not pushed. */
static void test_keyword_depth_cap(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.keyword_depth = 2});
    open_text("`begin_keywords \"1364-1995\"\n`begin_keywords \"1364-2001\"\n"
              "`begin_keywords \"1364-2005\"\n");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 27});
    expect_tok(TOK_DIRECTIVE, (tok_range){28, 27});
    expect_tok(TOK_DIRECTIVE, (tok_range){56, 27});
    TEST_ASSERT_EQUAL_STRING("t.v:3:1: error: `begin_keywords nested deeper than 2\n", logged());
    TEST_ASSERT_EQUAL_UINT32(2, run.kw_depth);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VL_KW_2001, run.kw_mask[1]);
}

/* --- strings (§2.4) ------------------------------------------------------------------------ */

static void test_string_escapes(void) {
    open_text("\"\\n\\t\\\\\\\"\\101\" \"caf\xc3\xa9\" \"\"");
    expect_name(TOK_STRING, (tok_range){0, 14}, "\n\t\\\"A");
    expect_name(TOK_STRING, (tok_range){15, 7}, "caf\xc3\xa9"); /* kept verbatim (review focus 5) */
    expect_name(TOK_STRING, (tok_range){23, 2}, "");
    expect_eof(25);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_string_unknown_escape_warns(void) {
    open_text("\"\\q\" \"\\1x\"");
    expect_name(TOK_STRING, (tok_range){0, 4}, "q");
    expect_name(TOK_STRING, (tok_range){5, 5}, "\001x");
    TEST_ASSERT_EQUAL_STRING("t.v:1:2: warning: unknown escape '\\q' in a string (read as 'q')\n",
                             logged());
    TEST_ASSERT_FALSE(err_flag);
}

/* A byte outside 0x21..0x7e after a backslash is named, never echoed raw into the log. */
static void test_string_unknown_escape_of_raw_byte(void) {
    open_text("\"a\\\x01\" \"\\\xe9\" \"\\ \"");
    expect_name(TOK_STRING, (tok_range){0, 5}, "a\001");
    expect_name(TOK_STRING, (tok_range){6, 4}, "\xe9");
    expect_name(TOK_STRING, (tok_range){11, 4}, " ");
    TEST_ASSERT_EQUAL_STRING(
        "t.v:1:3: warning: unknown escape '\\' before byte 0x01 in a string (read as byte 0x01)\n"
        "t.v:1:8: warning: unknown escape '\\' before byte 0xe9 in a string (read as byte 0xe9)\n"
        "t.v:1:13: warning: unknown escape '\\' before byte 0x20 in a string (read as byte 0x20)\n",
        logged());
}

static void test_string_nul_escape(void) {
    open_text("\"a\\000b\" \"\\777\"");
    expect_name(TOK_STRING, (tok_range){0, 8}, "ab");
    expect_name(TOK_STRING, (tok_range){9, 6}, "");
    TEST_ASSERT_EQUAL_STRING("t.v:1:3: error: a string cannot hold a NUL byte (\\000)\n"
                             "t.v:1:11: error: octal escape \\777 is out of range\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
}

/* A backslash before a raw NUL escapes nothing: the NUL is the "unexpected byte" error a raw NUL
 * in a string gets, never a string byte (an interned NUL was INVALID_ARG; fuzz_verilog). */
static void test_string_backslash_nul(void) {
    open_n("\"a\\\0b\" \"\\\0", 10);
    expect_name(TOK_STRING, (tok_range){0, 6}, "ab");
    expect_name(TOK_STRING, (tok_range){7, 3}, "");
    TEST_ASSERT_EQUAL_STRING("t.v:1:4: error: unexpected byte 0x00\n"
                             "t.v:1:10: error: unexpected byte 0x00\n"
                             "t.v:1:8: error: unterminated string\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
}

static void test_string_unterminated(void) {
    open_text("\"abc\r\nx \"d\\");
    expect_name(TOK_STRING, (tok_range){0, 4}, "abc");
    expect_ident("x");
    expect_name(TOK_STRING, (tok_range){8, 3}, "d");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: unterminated string\n"
                             "t.v:2:3: error: unterminated string\n",
                             logged());
    expect_eof(11);
}

/* The string cap (2^17): an error, the value truncated to the cap. */
static void test_string_cap(void) {
    char *text = repeat("\"", 'a', STRING_CAP + 1, "\"");
    open_text(text);
    TEST_ASSERT_EQUAL_INT(TOK_STRING, next());
    expect_range(0, STRING_CAP + 3);
    TEST_ASSERT_EQUAL_size_t(STRING_CAP, strlen(str_of(val.u)));
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: string longer than 131072 bytes\n", logged());
    odin3_vl_close(lx);
    lx = NULL;
    free(text);
}

/* The string cap through the hook (4): counted after unescaping. */
static void test_string_cap_hook(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.string_bytes = 4});
    open_text("\"a\\tbcdef\" \"\\101\\102\\103\\104\"");
    expect_name(TOK_STRING, (tok_range){0, 10}, "a\tbc");
    expect_name(TOK_STRING, (tok_range){11, 18}, "ABCD");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: string longer than 4 bytes\n", logged());
}

/* --- attributes and events (§2.7) ---------------------------------------------------------- */

static void test_attribute_and_event_tokens(void) {
    open_text("(* *) @* @(*) @( * ) @ *\n@\t(\n*\n)");
    expect_tok(TOK_ATTR_OPEN, (tok_range){0, 2});
    expect_tok(TOK_ATTR_CLOSE, (tok_range){3, 2});
    expect_tok(TOK_AT_STAR, (tok_range){6, 2});
    expect_tok(TOK_AT_STAR, (tok_range){9, 4});
    expect_tok(TOK_AT_STAR, (tok_range){14, 6});
    expect_tok(TOK_AT_STAR, (tok_range){21, 3});
    expect_tok(TOK_AT_STAR, (tok_range){25, 7});
    expect_eof(32);
}

/* A comment inside the parentheses defeats the single token (§2.7). */
static void test_event_star_with_comment(void) {
    open_text("@( /*c*/ * ) @(* /*c*/ ) @( /*c*/ *)");
    expect_tok('@', (tok_range){0, 1});
    expect_tok('(', (tok_range){1, 1});
    expect_tok('*', (tok_range){9, 1});
    expect_tok(')', (tok_range){11, 1});
    expect_tok('@', (tok_range){13, 1});
    expect_tok(TOK_ATTR_OPEN, (tok_range){14, 2});
    expect_tok(')', (tok_range){23, 1});
    expect_tok('@', (tok_range){25, 1});
    expect_tok('(', (tok_range){26, 1});
    expect_tok(TOK_ATTR_CLOSE, (tok_range){34, 2});
    expect_eof(36);
}

/* --- comments and metacomments (§2.6) ------------------------------------------------------ */

static void test_comments_recorded(void) {
    open_text("// x\ny /* 1\n2\n3 */ z");
    expect_name(TOK_IDENT, (tok_range){5, 1}, "y");
    expect_name(TOK_IDENT, (tok_range){off_of("z"), 1}, "z");
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(2, span.n);
    expect_comment(&span.items[0], (tok_range){0, 4}, " x");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_LINE, span.items[0].kind);
    expect_comment(&span.items[1], (tok_range){7, 11}, " 1\n2\n3 ");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_BLOCK, span.items[1].kind);
    TEST_ASSERT_EQUAL_UINT8(0, span.items[1].flags);
}

static void test_unterminated_comment(void) {
    open_text("a /* x\n y");
    expect_ident("a");
    expect_eof(1);
    TEST_ASSERT_EQUAL_STRING("t.v:1:3: error: unterminated comment\n", logged());
    TEST_ASSERT_TRUE(err_flag);
    TEST_ASSERT_EQUAL_UINT32(0, comments().n);
}

/* The comment cap (hook: 16) cuts the text, keeps the range and says nothing. */
static void test_comment_cap(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.comment_bytes = 16});
    open_text("/*0123456789abcdefg*/ //0123456789abcdefg");
    expect_eof(0);
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(2, span.n);
    expect_comment(&span.items[0], (tok_range){0, 21}, "0123456789abcdef");
    expect_comment(&span.items[1], (tok_range){22, 19}, "0123456789abcdef");
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* The real comment cap: a 2^20 + 1-byte text is cut to 2^20, silently. */
static void test_comment_cap_default(void) {
    char *text = repeat("//", 'c', ODIN3_VL_MAX_COMMENT_BYTES + 1, "\n");
    open_text(text);
    expect_eof(0);
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VL_MAX_COMMENT_BYTES, span.items[0].len);
    TEST_ASSERT_EQUAL_UINT32(at(ODIN3_VL_MAX_COMMENT_BYTES + 3).v, span.items[0].end.v);
    TEST_ASSERT_EQUAL_STRING("", logged());
    odin3_vl_close(lx);
    lx = NULL;
    free(text);
}

/* The end of input is located at the last token's end, after a trailing comment. */
static void test_eof_after_trailing_comment(void) {
    open_text("endmodule // c");
    expect_tok(TOK_ENDMODULE, (tok_range){0, 9});
    expect_eof(9);
}

static void test_metacomments(void) {
    open_text("// synopsys full_case parallel_case\n/* synthesis parallel_case */\n"
              "//synopsys   parallel_case   full_case\n// synopsys keep\n"
              "// synopsys full_case foo\n// synthesisfoo\n/*\tsynopsys\nfull_case*/\n");
    expect_eof(0);
    odin3_vec metas;
    odin3_vec_init(&metas, sizeof(odin3_vl_meta));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, 0, UINT32_MAX, &metas));
    TEST_ASSERT_EQUAL_size_t(4, metas.len);
    const odin3_vl_meta *meta = metas.data;
    const char *at_text[] = {"// synopsys full", "/* synthesis", "//synopsys ", "/*\tsyn"};
    const uint8_t keys[] = {3, 2, 3, 1};
    for (size_t i = 0; i < 4; i++) {
        uint32_t off = off_of(at_text[i]);
        TEST_ASSERT_EQUAL_UINT32(off, meta[i].off);
        TEST_ASSERT_EQUAL_UINT32(at(off).v, meta[i].loc.v);
        TEST_ASSERT_EQUAL_UINT8(keys[i], meta[i].keys);
    }
    TEST_ASSERT_EQUAL_UINT32(at(off_of("/* synthesis") + 29).v, meta[1].end.v);
    odin3_vec_free(&metas);
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(7, span.n);
    const uint8_t flags[] = {1, 1, 1, 1, 1, 0, 1};
    for (size_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT8(flags[i], span.items[i].flags);
    }
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* meta_take moves exactly the window [lo, hi) and leaves the rest in order. */
static void test_meta_take_window(void) {
    open_text("// synopsys full_case\n// synopsys parallel_case\n// synthesis full_case\n");
    expect_eof(0);
    uint32_t second = off_of("// synopsys parallel");
    uint32_t third = off_of("// synthesis");
    odin3_vec out;
    odin3_vec_init(&out, sizeof(odin3_vl_meta));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, second, third, &out));
    TEST_ASSERT_EQUAL_size_t(1, out.len);
    TEST_ASSERT_EQUAL_UINT32(second, ((const odin3_vl_meta *)out.data)[0].off);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, second, third, &out));
    TEST_ASSERT_EQUAL_size_t(1, out.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, 1, UINT32_MAX, &out));
    TEST_ASSERT_EQUAL_size_t(2, out.len);
    TEST_ASSERT_EQUAL_UINT32(third, ((const odin3_vl_meta *)out.data)[1].off);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, 0, UINT32_MAX, &out));
    TEST_ASSERT_EQUAL_size_t(3, out.len);
    TEST_ASSERT_EQUAL_UINT32(0, ((const odin3_vl_meta *)out.data)[2].off);
    odin3_vl_meta_clear(lx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, 0, UINT32_MAX, &out));
    TEST_ASSERT_EQUAL_size_t(3, out.len);
    odin3_vec_free(&out);
}

static void test_meta_clear(void) {
    open_text("// synopsys full_case\n");
    expect_eof(0);
    odin3_vl_meta_clear(lx);
    odin3_vec out;
    odin3_vec_init(&out, sizeof(odin3_vl_meta));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_meta_take(lx, 0, UINT32_MAX, &out));
    TEST_ASSERT_EQUAL_size_t(0, out.len);
    odin3_vec_free(&out);
}

/* --- directives (§2.9) --------------------------------------------------------------------- */

static void test_directive_without_argument_then_code(void) {
    open_text("`celldefine module m; `endcelldefine `nounconnected_drive");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 11}, "");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_CELLDEFINE, odin3_vl_directive_kind(lx, 0));
    expect_word(TOK_MODULE, "module");
    expect_name(TOK_IDENT, (tok_range){19, 1}, "m");
    expect_tok(';', (tok_range){off_of(";"), 1});
    expect_word(TOK_DIRECTIVE, "`endcelldefine");
    expect_word(TOK_DIRECTIVE, "`nounconnected_drive");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_NOUNCONNECTED_DRIVE,
                             odin3_vl_directive_kind(lx, off_of("`nounc")));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_COUNT, odin3_vl_directive_kind(lx, 1));
    expect_eof((uint32_t)source_len);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_default_nettype(void) {
    open_text("`default_nettype none\n`default_nettype uwire\n`default_nettype supply0\n"
              "`default_nettype");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 21}, "none");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE, odin3_vl_directive_kind(lx, 0));
    expect_text(TOK_DIRECTIVE, (tok_range){22, 22}, "uwire");
    expect_text(TOK_DIRECTIVE, (tok_range){45, 24}, "supply0"); /* still returned */
    expect_text(TOK_DIRECTIVE, (tok_range){70, 16}, "");
    TEST_ASSERT_EQUAL_STRING(
        "t.v:3:1: error: `default_nettype 'supply0' is not one of wire tri tri0 tri1 wand triand "
        "wor trior trireg uwire none\n"
        "t.v:4:1: error: `default_nettype needs a net type\n",
        logged());
}

static void test_timescale(void) {
    open_text("`timescale 1ns/1ps\n`timescale 1 ns / 1 ps // c\n`timescale 100us/10ns\n"
              "`timescale 2ns/1ps\n`timescale 1ns/1xs\n`timescale");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 18}, "1ns/1ps");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_TIMESCALE, odin3_vl_directive_kind(lx, 0));
    expect_text(TOK_DIRECTIVE, (tok_range){19, 22}, "1 ns / 1 ps");
    expect_text(TOK_DIRECTIVE, (tok_range){47, 21}, "100us/10ns");
    expect_text(TOK_DIRECTIVE, (tok_range){69, 18}, "2ns/1ps");
    expect_text(TOK_DIRECTIVE, (tok_range){88, 18}, "1ns/1xs");
    expect_text(TOK_DIRECTIVE, (tok_range){107, 10}, "");
    const char *want = "<1|10|100><s|ms|us|ns|ps|fs> / <1|10|100><s|ms|us|ns|ps|fs>";
    char expected[512];
    (void)snprintf(expected, sizeof expected,
                   "t.v:4:1: error: malformed `timescale (expected %s)\n"
                   "t.v:5:1: error: malformed `timescale (expected %s)\n"
                   "t.v:6:1: error: malformed `timescale (expected %s)\n",
                   want, want, want);
    TEST_ASSERT_EQUAL_STRING(expected, logged());
    TEST_ASSERT_EQUAL_UINT32(1, comments().n);
}

static void test_unconnected_drive(void) {
    open_text("`unconnected_drive pull1\n`unconnected_drive pull0\n`unconnected_drive weak1");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 24}, "pull1");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_UNCONNECTED_DRIVE, odin3_vl_directive_kind(lx, 0));
    expect_text(TOK_DIRECTIVE, (tok_range){25, 24}, "pull0");
    expect_text(TOK_DIRECTIVE, (tok_range){50, 24}, "weak1");
    TEST_ASSERT_EQUAL_STRING("t.v:3:1: error: `unconnected_drive 'weak1' is not pull0 or pull1\n",
                             logged());
}

/* `pragma takes the rest of the line minus a trailing // comment, which is recorded. */
/* A // inside a string of a `pragma argument is text; only a trailing // comment is cut. */
static void test_pragma_string_with_slashes(void) {
    open_text("`pragma key \"a//b\" c // d\n`pragma \"open // e");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 20}, "key \"a//b\" c");
    expect_text(TOK_DIRECTIVE, (tok_range){26, 13}, "\"open");
    expect_eof(39);
    TEST_ASSERT_EQUAL_UINT32(2, comments().n);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* A raw NUL in a `pragma argument (the only directive whose argument may hold one): a located
 * error, and the argument ends before it, so no NUL is interned (an unlocated INVALID_ARG before;
 * fuzz_verilog review I1). */
static void test_directive_nul(void) {
    open_n("`pragma a\0b\n", 12);
    expect_text(TOK_DIRECTIVE, (tok_range){0, 11}, "a");
    expect_eof(11);
    TEST_ASSERT_EQUAL_STRING("t.v:1:10: error: unexpected byte 0x00\n", logged());
    TEST_ASSERT_TRUE(err_flag);
}

/* The other directive whose argument may hold a NUL (inside its quotes): located the same way,
 * then the cut version is the usual located error. */
static void test_begin_keywords_nul(void) {
    open_n("`begin_keywords \"13\0\"", 21);
    expect_text(TOK_DIRECTIVE, (tok_range){0, 21}, "\"13");
    TEST_ASSERT_EQUAL_STRING("t.v:1:20: error: unexpected byte 0x00\n"
                             "t.v:1:1: error: unknown `begin_keywords version \"13\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
    expect_eof(21);
}

static void test_pragma(void) {
    open_text("`pragma foo bar // c\n`pragma\n`pragma a/b /\n`pragma x\r\n");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 15}, "foo bar");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_PRAGMA, odin3_vl_directive_kind(lx, 0));
    expect_text(TOK_DIRECTIVE, (tok_range){21, 7}, "");
    expect_text(TOK_DIRECTIVE, (tok_range){29, 13}, "a/b /");
    expect_text(TOK_DIRECTIVE, (tok_range){43, 9}, "x");
    expect_eof(52);
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    expect_comment(&span.items[0], (tok_range){16, 4}, " c");
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* The directive cap (hook: 8): an error, the argument cut at the cap. */
static void test_directive_cap(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.directive_bytes = 8});
    open_text("`pragma 123456789");
    expect_text(TOK_DIRECTIVE, (tok_range){0, 17}, "12345678");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: directive argument longer than 8 bytes\n", logged());
}

/* The real directive cap: a 4097-byte argument. */
static void test_directive_cap_default(void) {
    char *text = repeat("`pragma ", 'x', ODIN3_VL_MAX_DIRECTIVE_BYTES + 1, "\n");
    open_text(text);
    TEST_ASSERT_EQUAL_INT(TOK_DIRECTIVE, next());
    TEST_ASSERT_EQUAL_size_t(ODIN3_VL_MAX_DIRECTIVE_BYTES, val.text.len);
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: directive argument longer than 4096 bytes\n",
                             logged());
    odin3_vl_close(lx);
    lx = NULL;
    free(text);
}

static void test_unknown_directive(void) {
    open_text("`foo x `define ` y");
    expect_ident("x");
    expect_ident("y");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: unknown directive `foo\n"
                             "t.v:1:8: error: unknown directive `define\n"
                             "t.v:1:16: error: unexpected byte 0x60\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
}

/* SystemVerilog's `.*` is one located error at its '.', both bytes dropped: `(.*)` is '(' ')'
 * (not '(' '.' ATTR_CLOSE), and `. *` (two tokens) is left to the parser. */
static void test_implicit_port_connection(void) {
    open_text("u (.*); v (. *)");
    expect_ident("u");
    expect_tok('(', (tok_range){2, 1});
    expect_tok(')', (tok_range){5, 1});
    expect_tok(';', (tok_range){6, 1});
    expect_ident("v");
    expect_tok('(', (tok_range){10, 1});
    expect_tok('.', (tok_range){11, 1});
    expect_tok(TOK_ATTR_CLOSE, (tok_range){13, 2});
    TEST_ASSERT_EQUAL_STRING(
        "t.v:1:4: error: '.*' (implicit port connections) is SystemVerilog, not Verilog-2005\n",
        logged());
    TEST_ASSERT_TRUE(err_flag);
}

/* --- opaque bodies (§2.8) ------------------------------------------------------------------ */

/* A body's end keyword right after a base (BASED would take its 'e' as a digit) still ends it. */
static void test_body_end_after_base(void) {
    open_text("specify x = 'h endspecify y primitive p; 'b\nendprimitive 'h endspecifyx");
    expect_tok(TOK_OPAQUE_SPECIFY, (tok_range){0, 25});
    expect_name(TOK_IDENT, (tok_range){26, 1}, "y");
    expect_tok(TOK_OPAQUE_PRIMITIVE, (tok_range){28, 28});
    expect_text(TOK_BASE, (tok_range){57, 2}, "'h");
    expect_name(TOK_IDENT, (tok_range){60, 11}, "endspecifyx"); /* outside a body: a word */
    expect_eof(71);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_specify_body(void) {
    open_text("module m; specify\n // endspecify\n /* endspecify */\n $s(\"endspecify\", 'b_1, "
              "\\endspecify );\n `celldefine \x01\xff\nendspecify x");
    expect_word(TOK_MODULE, "module");
    expect_name(TOK_IDENT, (tok_range){7, 1}, "m");
    expect_tok(';', (tok_range){8, 1});
    uint32_t start = off_of("specify");
    uint32_t end = (uint32_t)source_len - 2;
    expect_tok(TOK_OPAQUE_SPECIFY, (tok_range){start, end - start});
    TEST_ASSERT_EQUAL_PTR(source + start, val.text.ptr);
    TEST_ASSERT_EQUAL_size_t(end - start, val.text.len);
    expect_name(TOK_IDENT, (tok_range){end + 1, 1}, "x");
    expect_eof(end + 2);
    TEST_ASSERT_EQUAL_STRING("", logged());
    TEST_ASSERT_EQUAL_UINT32(0, comments().n); /* comments inside a body are text */
}

static void test_primitive_and_config_names(void) {
    open_text("primitive p (q, a); table 0:1; endtable endprimitive\n"
              "config /* c */ \\c$1 ; design d; endconfig\nprimitive endprimitive");
    expect_tok(TOK_OPAQUE_PRIMITIVE, (tok_range){0, 52});
    TEST_ASSERT_EQUAL_STRING("p", str_of(odin3_vl_opaque_name(lx, 0)));
    expect_tok(TOK_OPAQUE_CONFIG, (tok_range){53, 41});
    TEST_ASSERT_EQUAL_STRING("c$1", str_of(odin3_vl_opaque_name(lx, 53)));
    expect_tok(TOK_OPAQUE_PRIMITIVE, (tok_range){95, 22});
    TEST_ASSERT_EQUAL_UINT32(0, odin3_vl_opaque_name(lx, 95));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_vl_opaque_name(lx, 7));
    expect_eof(117);
    TEST_ASSERT_EQUAL_STRING("t.v:3:1: error: expected a name after 'primitive'\n", logged());
}

static void test_opaque_unterminated(void) {
    open_text("module m; specify (a => b) = 1; /* endspecify");
    expect_word(TOK_MODULE, "module");
    expect_name(TOK_IDENT, (tok_range){7, 1}, "m");
    expect_tok(';', (tok_range){8, 1});
    expect_eof(9);
    TEST_ASSERT_EQUAL_STRING("t.v:1:11: error: 'specify' has no matching 'endspecify'\n", logged());
    TEST_ASSERT_TRUE(err_flag);
}

/* Over the text cap (hook: 16): an error at the keyword, the body dropped. */
static void test_opaque_cap(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.text_bytes = 20});
    open_text("specify a; endspecify config c; endconfig");
    expect_tok(TOK_OPAQUE_SPECIFY, (tok_range){0, 21});
    TEST_ASSERT_EQUAL_size_t(0, val.text.len);
    expect_tok(TOK_OPAQUE_CONFIG, (tok_range){22, 19});
    TEST_ASSERT_EQUAL_size_t(19, val.text.len);
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: specify block longer than 20 bytes\n", logged());
}

/* The real text cap: a body of 2^20 + 1 bytes, scanned token by token, is dropped. */
static void test_opaque_cap_default(void) {
    const size_t body = ODIN3_AST_MAX_TEXT_BYTES + 1 - strlen("specify  endspecify");
    char *text = repeat("specify ", ';', body, " endspecify primitive \\p_1 ; endprimitive");
    open_text(text);
    expect_tok(TOK_OPAQUE_SPECIFY, (tok_range){0, ODIN3_AST_MAX_TEXT_BYTES + 1});
    TEST_ASSERT_EQUAL_size_t(0, val.text.len);
    TEST_ASSERT_EQUAL_INT(TOK_OPAQUE_PRIMITIVE, next());
    TEST_ASSERT_EQUAL_STRING("p_1", str_of(odin3_vl_opaque_name(lx, loc.off)));
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: specify block longer than 1048576 bytes\n", logged());
    odin3_vl_close(lx);
    lx = NULL;
    free(text);
}

/* --- bytes outside the grammar (review focus 5, point (h)) --------------------------------- */

static void test_unexpected_bytes(void) {
    open_n("\x01x\0y", 4);
    expect_ident("x");
    expect_name(TOK_IDENT, (tok_range){3, 1}, "y");
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: unexpected byte 0x01\n"
                             "t.v:1:3: error: unexpected byte 0x00\n",
                             logged());
    TEST_ASSERT_TRUE(err_flag);
}

static void test_bom_is_one_error(void) {
    open_text("\xef\xbb\xbfmodule x\xc3\xa9");
    expect_tok(TOK_MODULE, (tok_range){3, 6});
    expect_ident("x");
    expect_eof(11);
    TEST_ASSERT_EQUAL_STRING("t.v:1:1: error: unexpected byte 0xef\n"
                             "t.v:1:12: error: unexpected byte 0xc3\n",
                             logged());
}

/* Latin-1 (one byte 0xe9): kept verbatim in a comment and a string, one error in code. */
static void test_latin1(void) {
    open_text("// caf\xe9\n\"caf\xe9\" x\xe9 y");
    expect_name(TOK_STRING, (tok_range){8, 6}, "caf\xe9");
    expect_name(TOK_IDENT, (tok_range){15, 1}, "x");
    expect_name(TOK_IDENT, (tok_range){18, 1}, "y");
    TEST_ASSERT_EQUAL_STRING("t.v:2:9: error: unexpected byte 0xe9\n", logged());
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_STRING(" caf\xe9", span.items[0].text);
}

static void test_non_ascii_in_comments_strings_and_escapes(void) {
    open_text("// caf\xc3\xa9\n\"caf\xc3\xa9\" \\caf\xc3\xa9 z");
    expect_name(TOK_STRING, (tok_range){9, 7}, "caf\xc3\xa9");
    TEST_ASSERT_EQUAL_INT(TOK_IDENT, next());
    expect_range(17, 6);
    expect_ident("z");
    TEST_ASSERT_EQUAL_STRING("t.v:2:13: error: unexpected byte 0xc3 in an escaped identifier\n",
                             logged());
    odin3_ast_comment_span span = comments();
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_STRING(" caf\xc3\xa9", span.items[0].text);
}

/* Review focus 1: escaped names as Odin III and Yosys netlists write them. */
static void test_netlist_names(void) {
    open_text("\\u1/q[3] \\$auto$alumacc.cc:474$12 \\a.b [3]");
    expect_name(TOK_IDENT, (tok_range){0, 8}, "u1/q[3]");
    expect_name(TOK_IDENT, (tok_range){9, 24}, "$auto$alumacc.cc:474$12");
    expect_name(TOK_IDENT, (tok_range){34, 4}, "a.b");
    expect_tok('[', (tok_range){39, 1});
    expect_text(TOK_UNSIGNED_NUMBER, (tok_range){40, 1}, "3");
    expect_tok(']', (tok_range){41, 1});
    expect_eof(42);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* --- review focus 2 and 3: CRLF, edges of a file ------------------------------------------ */

/* One token or comment as {kind, line, col, end line, end col, text}. */
typedef struct lexed {
    int kind;
    odin3_srcpos pos, end;
    char text[32];
} lexed;

static void decode(odin3_loc where, odin3_srcpos *pos) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_TRUE(odin3_srcman_decode(sm, where, pos));
}

/* Lexes text to the end: its tokens, then its comments, into out (count returned). */
static size_t lex_all(const char *text, lexed *out, size_t cap) {
    open_text(text);
    size_t count = 0;
    for (int kind = next(); kind != TOK_YYEOF && count < cap; kind = next(), count++) {
        lexed *one = &out[count];
        *one = (lexed){.kind = kind};
        decode(loc.loc, &one->pos);
        decode((odin3_loc){loc.end.v - 1}, &one->end);
        const char *name = kind == TOK_IDENT || kind == TOK_STRING ? str_of(val.u) : "";
        (void)snprintf(one->text, sizeof one->text, "%s", name);
    }
    return count;
}

static void test_crlf_twins(void) {
    lexed lf[8];
    lexed crlf[8];
    size_t count = lex_all("// x\n\\a+b\n\"s\\t\"\n/* y\n*/ z\n", lf, 8);
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_size_t(count,
                             lex_all("// x\r\n\\a+b\r\n\"s\\t\"\r\n/* y\r\n*/ z\r\n", crlf, 8));
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(lf[i].kind, crlf[i].kind);
        TEST_ASSERT_EQUAL_STRING(lf[i].text, crlf[i].text);
        TEST_ASSERT_EQUAL_UINT32(lf[i].pos.line, crlf[i].pos.line);
        TEST_ASSERT_EQUAL_UINT32(lf[i].pos.col, crlf[i].pos.col);
        TEST_ASSERT_EQUAL_UINT32(lf[i].end.line, crlf[i].end.line);
        TEST_ASSERT_EQUAL_UINT32(lf[i].end.col, crlf[i].end.col);
    }
    odin3_ast_comment_span span = comments(); /* the CRLF stream's */
    TEST_ASSERT_EQUAL_UINT32(2, span.n);
    TEST_ASSERT_EQUAL_STRING(" x", span.items[0].text);
    odin3_srcpos end;
    decode((odin3_loc){span.items[0].end.v - 1}, &end);
    TEST_ASSERT_EQUAL_UINT32(4, end.col); /* the \r is not part of the comment */
    TEST_ASSERT_EQUAL_STRING(" y\r\n", span.items[1].text);
    TEST_ASSERT_EQUAL_STRING("", logged());
}

static void test_empty_stream(void) {
    open_text("");
    expect_eof(0);
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next());
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* `// c`, `\abc` and `endmodule` at the end of input with no newline (review focus 3). */
static void test_edges_without_newline(void) {
    open_text("\\abc");
    expect_name(TOK_IDENT, (tok_range){0, 4}, "abc");
    expect_eof(4);
    open_text("endmodule");
    expect_tok(TOK_ENDMODULE, (tok_range){0, 9});
    expect_eof(9);
    open_text("`timescale 1ns/1ps");
    expect_tok(TOK_DIRECTIVE, (tok_range){0, 18});
    expect_eof(18);
    open_text("// c");
    expect_eof(0);
    TEST_ASSERT_EQUAL_UINT32(1, comments().n); /* seals the store: last */
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* --- the error flag, the token map, the driver --------------------------------------------- */

static void test_error_flag_binding(void) {
    open_text("\"\\q\"");
    TEST_ASSERT_EQUAL_INT(TOK_STRING, next());
    TEST_ASSERT_FALSE(err_flag); /* a warning */
    open_text("\x02");
    expect_eof(0);
    TEST_ASSERT_TRUE(err_flag);
    odin3_vl_close(lx); /* unbound: the lexer's own flag */
    vt_stream(design, "u.v", odin3_bytes_cstr("\x02"), &stream);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &stream, &lx));
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, next());
}

/* A lexer error leaves the stream without a UNIT even when the grammar never errs. */
static void test_driver_lexer_error_gives_no_unit(void) {
    odin3_log_set_sink(NULL, NULL);
    vt_result res;
    vt_parse("c.v", "/* open", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_STRING("c.v:1:1: error: unterminated comment\n", res.log.data);
    vt_free(&res);
    vt_parse("d.v", "// fine\n", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    vt_free(&res);
}

static void test_token_map(void) {
    odin3_vec tokens;
    odin3_vec_init(&tokens, sizeof(odin3_vp_loc));
    run.tokens = &tokens;
    open_text("a /* c */ 8'h1 specify endspecify");
    while (next() != TOK_YYEOF) {
    }
    TEST_ASSERT_EQUAL_size_t(6, tokens.len); /* a 8 'h 1 OPAQUE_SPECIFY and the end */
    const odin3_vp_loc *map = tokens.data;
    TEST_ASSERT_EQUAL_UINT32(0, map[0].off);
    TEST_ASSERT_EQUAL_UINT32(10, map[1].off);
    TEST_ASSERT_EQUAL_UINT32(at(14).v, map[3].end.v);
    TEST_ASSERT_EQUAL_UINT32(15, map[4].off);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)source_len, map[5].off);
    run.tokens = NULL;
    odin3_vec_free(&tokens);
}

/* --- OOM ------------------------------------------------------------------------------------ */

/* Every allocating path: an identifier, a real, a string, a comment, a metacomment, an opaque
 * name, the token map, the open-set warning. */
static const char oom_text[] = "`begin_keywords \"1364-2005\" module m; // synopsys full_case\n"
                               "real r = 1.5; x = \"s\\n\"; /* b */ primitive p; endprimitive\n";

/* One step: a fresh design, the limit-th allocation failing. A failure is NO_MEMORY with the
 * run stopped and nothing logged as misuse. */
static bool oom_step(long limit) {
    tearDown();
    setUp();
    odin3_vec tokens;
    odin3_vec_init(&tokens, sizeof(odin3_vp_loc));
    run.tokens = &tokens;
    vt_stream(design, "t.v", odin3_bytes_cstr(oom_text), &stream);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_vl_open(&run, ast, &stream, &lx);
    bool opened = st == ODIN3_OK;
    int count = 0;
    while (opened && next() != TOK_YYEOF) {
        count++;
    }
    if (opened) {
        st = odin3_vl_fatal_status(lx);
    }
    odin3_util_set_alloc_fail_after(-1);
    odin3_vec_free(&tokens);
    run.tokens = NULL;
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_TRUE(!opened || run.stop);
        return false;
    }
    TEST_ASSERT_EQUAL_INT(14, count);
    return true;
}

/* The hook fails one allocation per step; a step whose failure the code tolerates (a diagnostic's
 * formatting) succeeds, so the sweep runs on until OOM_TAIL steps in a row have succeeded. */
static void test_lexer_oom_sweep(void) {
    long last_failure = -1;
    long limit = 0;
    for (; limit < OOM_LIMIT && limit - last_failure <= OOM_TAIL; limit++) {
        if (!oom_step(limit)) {
            last_failure = limit;
        }
    }
    TEST_ASSERT_TRUE(last_failure > 10);
    TEST_ASSERT_TRUE(limit < OOM_LIMIT);
}

/* Two runners: main stays under the function-size limit. */
static void run_tokens(void) {
    RUN_TEST(test_keyword_table_sorted_and_found);
    RUN_TEST(test_keyword_sets);
    RUN_TEST(test_keyword_masks_every_word);
    RUN_TEST(test_based_underscore);
    RUN_TEST(test_spaced_number_with_comments);
    RUN_TEST(test_based_digits);
    RUN_TEST(test_base_at_end_of_input);
    RUN_TEST(test_reals);
    RUN_TEST(test_reals_ignore_the_host_locale);
    RUN_TEST(test_not_reals);
    RUN_TEST(test_real_out_of_range);
    RUN_TEST(test_delay_and_size_lex_alike);
    RUN_TEST(test_escaped_identifiers);
    RUN_TEST(test_escaped_identifier_without_name);
    RUN_TEST(test_identifier_cap);
    RUN_TEST(test_identifier_cap_default);
    RUN_TEST(test_system_names);
    RUN_TEST(test_keywords_and_identifiers);
    RUN_TEST(test_later_keywords_are_identifiers);
    RUN_TEST(test_operators);
    RUN_TEST(test_begin_keywords_1995);
    RUN_TEST(test_begin_keywords_2001_and_noconfig);
    RUN_TEST(test_begin_keywords_unknown_version);
    RUN_TEST(test_begin_keywords_malformed);
    RUN_TEST(test_end_keywords_on_empty_stack);
    RUN_TEST(test_resetall_keeps_the_set);
    RUN_TEST(test_open_set_spans_streams);
    RUN_TEST(test_keyword_depth_cap);
    RUN_TEST(test_string_escapes);
    RUN_TEST(test_string_unknown_escape_warns);
    RUN_TEST(test_string_unknown_escape_of_raw_byte);
    RUN_TEST(test_string_nul_escape);
    RUN_TEST(test_string_backslash_nul);
    RUN_TEST(test_string_unterminated);
    RUN_TEST(test_string_cap);
    RUN_TEST(test_string_cap_hook);
}

static void run_trivia_and_edges(void) {
    RUN_TEST(test_attribute_and_event_tokens);
    RUN_TEST(test_event_star_with_comment);
    RUN_TEST(test_comments_recorded);
    RUN_TEST(test_unterminated_comment);
    RUN_TEST(test_comment_cap);
    RUN_TEST(test_comment_cap_default);
    RUN_TEST(test_eof_after_trailing_comment);
    RUN_TEST(test_metacomments);
    RUN_TEST(test_meta_take_window);
    RUN_TEST(test_meta_clear);
    RUN_TEST(test_directive_without_argument_then_code);
    RUN_TEST(test_default_nettype);
    RUN_TEST(test_timescale);
    RUN_TEST(test_unconnected_drive);
    RUN_TEST(test_pragma);
    RUN_TEST(test_pragma_string_with_slashes);
    RUN_TEST(test_directive_nul);
    RUN_TEST(test_begin_keywords_nul);
    RUN_TEST(test_directive_cap);
    RUN_TEST(test_directive_cap_default);
    RUN_TEST(test_unknown_directive);
    RUN_TEST(test_implicit_port_connection);
    RUN_TEST(test_specify_body);
    RUN_TEST(test_body_end_after_base);
    RUN_TEST(test_primitive_and_config_names);
    RUN_TEST(test_opaque_unterminated);
    RUN_TEST(test_opaque_cap);
    RUN_TEST(test_opaque_cap_default);
    RUN_TEST(test_unexpected_bytes);
    RUN_TEST(test_bom_is_one_error);
    RUN_TEST(test_non_ascii_in_comments_strings_and_escapes);
    RUN_TEST(test_latin1);
    RUN_TEST(test_netlist_names);
    RUN_TEST(test_crlf_twins);
    RUN_TEST(test_empty_stream);
    RUN_TEST(test_edges_without_newline);
    RUN_TEST(test_error_flag_binding);
    RUN_TEST(test_driver_lexer_error_gives_no_unit);
    RUN_TEST(test_token_map);
    RUN_TEST(test_lexer_oom_sweep);
}

int main(void) {
    UNITY_BEGIN();
    run_tokens();
    run_trivia_and_edges();
    return UNITY_END();
}
