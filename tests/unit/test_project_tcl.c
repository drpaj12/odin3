/*
 * test_project_tcl.c — unit tests for the Tcl word reader (PRJ-13): the oracle TclReader's
 * docstring cases, every word form and error with its line, the §10.1 quirks (the EOF line for an
 * unterminated brace, the next line for an escape before a newline), CRLF folding with offsets
 * into the text as given, the .qip idiom in both forms, and an OOM sweep.
 */
#include "frontends/project/project.h"
#include "frontends/project/tcl.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OUT_MAX = 4096, OOM_LIMIT = 200 };

static odin3_arena *arena;
static odin3_vec commands;
static odin3_tcl_err err;
static char out[OUT_MAX];

static uint32_t max_words; /* odin3_tcl_opts.max_words for run() */

void setUp(void) {
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_vec_init(&commands, sizeof(odin3_tcl_command));
    err = (odin3_tcl_err){0, NULL, false};
    max_words = UINT32_MAX;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_vec_free(&commands);
    odin3_arena_destroy(arena);
}

/* The 1-based line of offset off in text. */
static unsigned line_of(const char *text, uint32_t off) {
    unsigned line = 1;
    for (uint32_t i = 0; i < off && text[i] != '\0'; i++) {
        line += text[i] == '\n';
    }
    return line;
}

static odin3_status run(const char *text, bool qip, size_t len) {
    odin3_tcl_opts opts = {arena, qip, odin3_bytes_cstr("/q/ip"), max_words};
    return odin3_tcl_tokenize((odin3_bytes){text, len}, &opts, &commands, &err);
}

/* Tokenizes text and renders it as "<line>:<w1>|<w2> ; <line>:…". */
static const char *words_of(const char *text, bool qip) {
    odin3_vec_clear(&commands);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(text, qip, strlen(text)));
    size_t len = 0;
    out[0] = '\0';
    for (size_t i = 0; i < commands.len; i++) {
        const odin3_tcl_command *cmd = odin3_vec_cat(&commands, i);
        len += (size_t)snprintf(out + len, sizeof out - len, "%s%u:", i > 0 ? " ; " : "",
                                line_of(text, cmd->off));
        for (uint32_t j = 0; j < cmd->nwords; j++) {
            const odin3_tcl_word *word = &cmd->words[j];
            TEST_ASSERT_EQUAL_CHAR('\0', ((const char *)word->text.ptr)[word->text.len]);
            len += (size_t)snprintf(out + len, sizeof out - len, "%s%.*s", j > 0 ? "|" : "",
                                    (int)word->text.len, (const char *)word->text.ptr);
        }
    }
    return out;
}

/* Tokenizing text fails with what at line; commands keeps its length. */
static void expect_error(const char *text, bool qip, const char *what, unsigned line) {
    odin3_vec_clear(&commands);
    (void)odin3_vec_push(&commands);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run(text, qip, strlen(text)));
    TEST_ASSERT_EQUAL_STRING(what, err.what);
    TEST_ASSERT_EQUAL_UINT(line, line_of(text, err.off));
    TEST_ASSERT_EQUAL_size_t(1, commands.len);
}

/* --- the oracle docstring's forms ---------------------------------------------------------- */

static void test_bare_words_and_separators(void) {
    TEST_ASSERT_EQUAL_STRING("1:a|b ; 1:c|d ; 2:e", words_of("a b;c d\ne", false));
    TEST_ASSERT_EQUAL_STRING("2:a ; 4:b", words_of("\n  a ; ;\n\n\tb\t\n", false));
    TEST_ASSERT_EQUAL_STRING("", words_of("", false));
    TEST_ASSERT_EQUAL_STRING("", words_of(" \t;\n;", false));
    TEST_ASSERT_EQUAL_STRING("1:a|b", words_of("a\rb", false)); /* a lone CR is a blank */
}

static void test_brackets_inside_words_are_ordinary(void) {
    TEST_ASSERT_EQUAL_STRING("1:set|-to|LEDR[0]|x]", words_of("set -to LEDR[0] x]", false));
    TEST_ASSERT_EQUAL_STRING("1:-to|SW[1]", words_of("-to \"SW[1]\"", false));
}

static void test_quoted_words(void) {
    TEST_ASSERT_EQUAL_STRING("1:x|a \"q\" \\ b", words_of("x \"a \\\"q\\\" \\\\ b\"", false));
    TEST_ASSERT_EQUAL_STRING("1:x||y", words_of("x \"\" y", false));
    TEST_ASSERT_EQUAL_STRING("1:x|a;b{c}#", words_of("x \"a;b{c}#\"", false));
    TEST_ASSERT_EQUAL_STRING("1:x|y ; 1:z", words_of("x \"y\";z", false));
}

static void test_braced_words(void) {
    TEST_ASSERT_EQUAL_STRING("1:x|a {b} $c \\d \"", words_of("x {a {b} $c \\d \"}", false));
    TEST_ASSERT_EQUAL_STRING("1:x|a\nb|y", words_of("x {a\nb} y", false));
    TEST_ASSERT_EQUAL_STRING("1:x|", words_of("x {}", false));
}

static void test_continuation(void) {
    TEST_ASSERT_EQUAL_STRING("1:a|b ; 3:c", words_of("a \\\n b\nc", false));
    TEST_ASSERT_EQUAL_STRING("2:a", words_of("\\\na", false));
    TEST_ASSERT_EQUAL_STRING("1:a|b", words_of("a\\\nb", false)); /* ends the bare word */
    TEST_ASSERT_EQUAL_STRING("1:a|b", words_of("\"a\"\\\nb", false));
}

static void test_comments(void) {
    TEST_ASSERT_EQUAL_STRING("2:a", words_of("# c ; d\na", false));
    TEST_ASSERT_EQUAL_STRING("1:a ; 2:b", words_of("a;# c\nb", false));
    TEST_ASSERT_EQUAL_STRING("1:a|#|b", words_of("a # b", false)); /* not at a command start */
    TEST_ASSERT_EQUAL_STRING("2:x", words_of("  # $ { \" [\nx", false));
}

static void test_crlf_folded_offsets_as_given(void) {
    const char *text = "a\r\nb {c\r\nd}\r\n";
    TEST_ASSERT_EQUAL_STRING("1:a ; 2:b|c\nd", words_of(text, false));
    const odin3_tcl_command *second = odin3_vec_cat(&commands, 1);
    TEST_ASSERT_EQUAL_UINT32(3, second->off);
    TEST_ASSERT_EQUAL_UINT32(5, second->words[1].off);
    TEST_ASSERT_EQUAL_STRING("1:a|b", words_of("a \\\r\nb", false));
    expect_error("a\r\nb $", false, "$ in a bare word is not supported", 2);
    TEST_ASSERT_EQUAL_UINT32(5, err.off);
}

/* --- errors -------------------------------------------------------------------------------- */

static void test_bare_word_errors(void) {
    expect_error("a\nb $c", false, "$ in a bare word is not supported", 2);
    TEST_ASSERT_EQUAL_UINT32(4, err.off);
    expect_error("a b\\c", false, "\\ in a bare word is not supported", 1);
    expect_error("a b\\", false, "\\ in a bare word is not supported", 1);
}

static void test_quote_errors(void) {
    expect_error("a\nb \"xy\nz\"", false, "unterminated quote", 2);
    expect_error("a \"xy", false, "unterminated quote", 1);
    expect_error("a \"x$\"", false, "$ substitution is not supported", 1);
    expect_error("a \"\\x\"", false, "bad escape (only \\\" and \\\\)", 1);
    expect_error("a \"\\", false, "bad escape (only \\\" and \\\\)", 1);
    /* the oracle counts the newline first: an escape before a newline is at the next line */
    expect_error("a \"x\\\ny\"", false, "bad escape (only \\\" and \\\\)", 2);
    expect_error("a \"b\"c", false, "extra characters after close-quote", 1);
    expect_error("a \"b\"\"c\"", false, "extra characters after close-quote", 1);
}

static void test_brace_errors(void) {
    /* unterminated: at the line of the end of the text (after a final newline, the next) */
    expect_error("a {b\nc\n", false, "unterminated brace", 3);
    expect_error("a {b\nc", false, "unterminated brace", 2);
    expect_error("a {b {c}", false, "unterminated brace", 1);
    expect_error("a {b}c", false, "extra characters after close-brace", 1);
    expect_error("x\na {b\n}c", false, "extra characters after close-brace", 3);
}

static void test_command_substitution_rejected(void) {
    const char *what = "command substitution [...] is not supported (only the .qip idiom "
                       "[file join $::quartus(qip_path) \"<path>\"])";
    expect_error("a [b]", false, what, 1);
    expect_error("a\n[file join $::quartus(qip_path) \"x.v\"]", false, what, 2);
    expect_error("a [file join $::quartus(qip_path) x.v ]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path)  \"x.v\"]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path) \"x$.v\"]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path) \"\"]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path) x{.v]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path) \"x.v]", true, what, 1);
    expect_error("a [file join $::quartus(qip_path) x\xc2\xa0y]", true, what, 1);
}

static void test_qip_idiom(void) {
    TEST_ASSERT_EQUAL_STRING("1:a|/q/ip/x.v",
                             words_of("a [file join $::quartus(qip_path) \"x.v\"]", true));
    TEST_ASSERT_EQUAL_STRING("1:a|/q/ip/sub/r.qip|b",
                             words_of("a [file join $::quartus(qip_path) sub/r.qip] b", true));
    TEST_ASSERT_EQUAL_STRING(
        "1:a|/q/x.v", words_of("a [file join $::quartus(qip_path) \"./a/../../x.v\"]", true));
    TEST_ASSERT_EQUAL_STRING("1:a|/abs/x.v",
                             words_of("a [file join $::quartus(qip_path) \"/abs//x.v\"]", true));
    TEST_ASSERT_EQUAL_STRING("1:a|/q/ip/x y.v ; 1:b",
                             words_of("a [file join $::quartus(qip_path) \"x y.v\"];b", true));
    /* the quoted form may hold a newline; the next command's line counts it */
    TEST_ASSERT_EQUAL_STRING("1:a|/q/ip/x\ny ; 3:b",
                             words_of("a [file join $::quartus(qip_path) \"x\ny\"]\nb", true));
    expect_error("a [file join $::quartus(qip_path) x.v]c", true,
                 "extra characters after close-bracket", 1);
}

static void test_nul_byte_in_word(void) {
    odin3_vec_clear(&commands);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("ab c\0d", false, 6));
    TEST_ASSERT_EQUAL_STRING("NUL byte in word", err.what);
    TEST_ASSERT_EQUAL_UINT32(3, err.off);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("# a\0b\nc", false, 7)); /* a comment may hold one */
}

static void test_python_spaces(void) {
    static const char *const spaces[] = {" ",
                                         "\t",
                                         "\n",
                                         "\v",
                                         "\f",
                                         "\r",
                                         "\x1c",
                                         "\x1f",
                                         "\xc2\x85",
                                         "\xc2\xa0",
                                         "\xe1\x9a\x80",
                                         "\xe2\x80\x80",
                                         "\xe2\x80\x8a",
                                         "\xe2\x80\xa8",
                                         "\xe2\x80\xa9",
                                         "\xe2\x80\xaf",
                                         "\xe2\x81\x9f",
                                         "\xe3\x80\x80"};
    for (size_t i = 0; i < sizeof spaces / sizeof spaces[0]; i++) {
        TEST_ASSERT_EQUAL_size_t(strlen(spaces[i]),
                                 odin3_py_space_len(odin3_bytes_cstr(spaces[i]), 0));
    }
    static const char *const others[] = {"a",        "\x1b",         "\xc2\x84", "\xe2\x80\x8b",
                                         "\xe2\x80", "\xe3\x80\x81", ""};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        TEST_ASSERT_EQUAL_size_t(0, odin3_py_space_len(odin3_bytes_cstr(others[i]), 0));
    }
}

/* --- out of memory ------------------------------------------------------------------------- */

/* One tokenize on a fresh arena with the k-th allocation failing; true when it succeeded. */
static bool oom_try(const char *text, long fail_after) {
    odin3_arena_destroy(arena);
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_vec_free(&commands);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = run(text, true, strlen(text));
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_size_t(0, commands.len);
    }
    return st == ODIN3_OK;
}

static void test_out_of_memory(void) {
    const char *text = "a \"b\\\"\" {c {d}}\r\ne [file join $::quartus(qip_path) \"x.v\"];f g h "
                       "i j k l m n o p q r s t u v";
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(text, k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 3);
    TEST_ASSERT_EQUAL_size_t(3, commands.len);
}

/* max_words caps the words of the whole text: the next word fails at its first byte, flagged
 * over_cap, and commands keeps its old length. */
static void test_word_cap(void) {
    max_words = 3;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("a b\nc\n", false, 6));
    TEST_ASSERT_FALSE(err.over_cap);
    odin3_vec_clear(&commands);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("a b\nc {d}\n", false, 10));
    TEST_ASSERT_TRUE(err.over_cap);
    TEST_ASSERT_EQUAL_UINT32(6, err.off);
    TEST_ASSERT_EQUAL_size_t(0, commands.len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bare_words_and_separators);
    RUN_TEST(test_brackets_inside_words_are_ordinary);
    RUN_TEST(test_quoted_words);
    RUN_TEST(test_braced_words);
    RUN_TEST(test_continuation);
    RUN_TEST(test_comments);
    RUN_TEST(test_crlf_folded_offsets_as_given);
    RUN_TEST(test_bare_word_errors);
    RUN_TEST(test_quote_errors);
    RUN_TEST(test_brace_errors);
    RUN_TEST(test_command_substitution_rejected);
    RUN_TEST(test_qip_idiom);
    RUN_TEST(test_nul_byte_in_word);
    RUN_TEST(test_python_spaces);
    RUN_TEST(test_word_cap);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
