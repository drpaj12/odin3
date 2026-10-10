/*
 * test_project_words.c — unit tests for the .o3proj / -f word splitter (PRJ-7): one test per
 * §4.2 rule in both modes, offsets, failure atomicity and OOM.
 */
#include "frontends/project/words.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { LINE_OFFSET = 100, OOM_LIMIT = 16 };

static const odin3_words_mode O3 = ODIN3_WORDS_O3PROJ;
static const odin3_words_mode FL = ODIN3_WORDS_FLIST;

static odin3_arena *arena;
static odin3_vec words;
static odin3_words_err err;

void setUp(void) {
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_vec_init(&words, sizeof(odin3_word));
    memset(&err, 0, sizeof err);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_vec_free(&words);
    odin3_arena_destroy(arena);
}

static odin3_status split(odin3_words_mode mode, const char *line) {
    odin3_words_opts opts = {.offset = LINE_OFFSET, .mode = mode, .arena = arena};
    return odin3_words_split(odin3_bytes_cstr(line), &opts, &words, &err);
}

static const odin3_word *word(size_t idx) {
    TEST_ASSERT_TRUE(idx < words.len);
    return odin3_vec_cat(&words, idx);
}

/* Asserts the words split so far are exactly the '|'-separated list (empty: none). */
static void expect_words(const char *list) {
    size_t i = 0;
    const char *at = list;
    while (*at != '\0') {
        const char *bar = strchr(at, '|');
        size_t len = bar != NULL ? (size_t)(bar - at) : strlen(at);
        const odin3_word *got = word(i++);
        TEST_ASSERT_EQUAL_size_t(len, got->text.len);
        TEST_ASSERT_EQUAL_MEMORY(at, got->text.ptr, len);
        at += len + (bar != NULL ? 1 : 0);
    }
    TEST_ASSERT_EQUAL_size_t(i, words.len);
}

/* A line and the words it splits into ('|'-separated). */
typedef struct split_case {
    const char *line;
    const char *list;
} split_case;

/* A line and the syntax error it gives: detail and column (0-based) of the offending word. */
typedef struct syntax_case {
    const char *line;
    const char *what;
    uint32_t col;
} syntax_case;

static void expect_ok(odin3_words_mode mode, split_case want) {
    odin3_vec_clear(&words);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, split(mode, want.line));
    expect_words(want.list);
}

static void expect_syntax(odin3_words_mode mode, syntax_case want) {
    odin3_vec_clear(&words);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, split(mode, want.line));
    TEST_ASSERT_NOT_NULL(err.what);
    TEST_ASSERT_EQUAL_STRING(want.what, err.what);
    TEST_ASSERT_EQUAL_UINT32(LINE_OFFSET + want.col, err.off);
    TEST_ASSERT_EQUAL_size_t(0, words.len);
}

static void test_blanks_are_space_and_tab(void) {
    expect_ok(O3, (split_case){"a b\tc  \t d", "a|b|c|d"});
    expect_ok(FL, (split_case){"\ta\t", "a"});
    expect_ok(O3, (split_case){"", ""});
    expect_ok(O3, (split_case){" \t ", ""});
    expect_ok(O3, (split_case){"a\rb", "a\rb"}); /* a lone CR is an ordinary byte */
    expect_ok(O3, (split_case){"a\vb\fc", "a\vb\fc"});
}

static void test_offsets(void) {
    expect_ok(O3, (split_case){"  ab \"c d\"", "ab|c d"});
    TEST_ASSERT_EQUAL_UINT32(LINE_OFFSET + 2, word(0)->off);
    TEST_ASSERT_EQUAL_UINT32(LINE_OFFSET + 5, word(1)->off);
}

static void test_hash_comment_both_modes(void) {
    expect_ok(O3, (split_case){"a #x b", "a"});
    expect_ok(FL, (split_case){"a #x b", "a"});
    expect_ok(O3, (split_case){"#x", ""});
    expect_ok(FL, (split_case){"\t# \"unterminated", ""});
}

static void test_slash_comment_only_in_flist(void) {
    expect_ok(FL, (split_case){"a //x b", "a"});
    expect_ok(FL, (split_case){"//x", ""});
    expect_ok(O3, (split_case){"a //x b", "a|//x|b"});
    expect_ok(FL, (split_case){"a /x", "a|/x"});
}

static void test_comment_markers_inside_a_word_are_ordinary(void) {
    expect_ok(O3, (split_case){"a#b", "a#b"});
    expect_ok(FL, (split_case){"a#b", "a#b"});
    expect_ok(FL, (split_case){"a//b", "a//b"});
    expect_ok(O3, (split_case){"a//b", "a//b"});
    expect_ok(O3, (split_case){"\"#a\" \"//b\"", "#a|//b"});
}

static void test_quoted_word_is_one_word(void) {
    expect_ok(O3, (split_case){"\"a b\" c", "a b|c"});
    TEST_ASSERT_TRUE(word(0)->quoted);
    TEST_ASSERT_FALSE(word(1)->quoted);
    expect_ok(O3, (split_case){"x \"a\tb\"", "x|a\tb"});
}

static void test_quoted_kept_in_o3proj_forgotten_in_flist(void) {
    expect_ok(O3, (split_case){"\"-v\"", "-v"});
    TEST_ASSERT_TRUE(word(0)->quoted);
    expect_ok(FL, (split_case){"\"-v\"", "-v"});
    TEST_ASSERT_FALSE(word(0)->quoted);
}

static void test_escapes_inside_quotes(void) {
    expect_ok(O3, (split_case){"\"a\\\"b\\\\c\"", "a\"b\\c"});
    expect_ok(FL, (split_case){"\"a\\\"b\\\\c\"", "a\"b\\c"});
}

static void test_bad_escape_is_syntax(void) {
    expect_syntax(O3,
                  (syntax_case){"x \"a\\qb\"", "bad escape (only \\\" and \\\\ inside quotes)", 2});
    expect_syntax(FL,
                  (syntax_case){"\"a\\qb\"", "bad escape (only \\\" and \\\\ inside quotes)", 0});
    expect_syntax(O3, (syntax_case){"\"a\\", "bad escape (only \\\" and \\\\ inside quotes)", 0});
}

static void test_empty_quoted_word_is_syntax(void) {
    expect_syntax(O3, (syntax_case){"a \"\"", "empty quoted word", 2});
    expect_syntax(FL, (syntax_case){"\"\"", "empty quoted word", 0});
}

static void test_characters_after_closing_quote(void) {
    expect_syntax(O3, (syntax_case){"\"a\"b", "characters after a closing quote", 0});
    expect_syntax(FL, (syntax_case){"x \"a\"b", "characters after a closing quote", 2});
    expect_syntax(
        O3, (syntax_case){"\"\"x", "characters after a closing quote", 0}); /* the oracle's order */
    expect_ok(O3, (split_case){"\"a\"\tb", "a|b"});
}

static void test_unterminated_quote(void) {
    expect_syntax(O3, (syntax_case){"\"a", "unterminated quote", 0});
    expect_syntax(FL, (syntax_case){"b \"a b", "unterminated quote", 2});
    expect_syntax(O3, (syntax_case){"\"a\\\"", "unterminated quote", 0});
    TEST_ASSERT_EQUAL_size_t(4, err.word.len);
    TEST_ASSERT_EQUAL_MEMORY("\"a\\\"", err.word.ptr, 4);
}

static void test_bare_backslash_is_ordinary(void) {
    expect_ok(O3, (split_case){"a\\b c\\", "a\\b|c\\"});
    expect_ok(FL, (split_case){"\\x", "\\x"});
}

static void test_quote_inside_a_bare_word(void) {
    expect_syntax(O3, (syntax_case){"x a\"b", "quote inside a word", 2});
    TEST_ASSERT_EQUAL_size_t(3, err.word.len);
    TEST_ASSERT_EQUAL_MEMORY("a\"b", err.word.ptr, 3);
    expect_ok(FL, (split_case){"+define+TAG=\"AB\" a\"", "+define+TAG=\"AB\"|a\""});
}

/* A NUL byte in a word is syntax after the oracle's own checks (no strtab string holds NUL);
 * one inside a comment is ignored. */
static void test_nul_byte_in_word(void) {
    static const char bare[] = "a b\0c d";
    static const char quoted[] = "\"b\0c\"";
    static const char comment[] = "a #\0";
    static const char after_quote[] = "a\"\0";
    odin3_words_opts opts = {.offset = LINE_OFFSET, .mode = O3, .arena = arena};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_words_split((odin3_bytes){bare, sizeof bare - 1},
                                                             &opts, &words, &err));
    TEST_ASSERT_EQUAL_STRING("NUL byte in word", err.what);
    TEST_ASSERT_EQUAL_UINT32(LINE_OFFSET + 2, err.off);
    TEST_ASSERT_EQUAL_size_t(3, err.word.len);
    opts.mode = FL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        odin3_words_split((odin3_bytes){quoted, sizeof quoted - 1}, &opts, &words, &err));
    TEST_ASSERT_EQUAL_STRING("NUL byte in word", err.what);
    TEST_ASSERT_EQUAL_UINT32(LINE_OFFSET, err.off);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_words_split((odin3_bytes){comment, sizeof comment - 1},
                                                      &opts, &words, &err));
    expect_words("a");
    opts.mode = O3;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        odin3_words_split((odin3_bytes){after_quote, sizeof after_quote - 1}, &opts, &words, &err));
    TEST_ASSERT_EQUAL_STRING("quote inside a word", err.what);
}

/* Words append to what the vec holds; a failure leaves it as it was. */
static void test_append_and_failure_atomicity(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, split(O3, "a b"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, split(O3, "c"));
    expect_words("a|b|c");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, split(O3, "d e \"f"));
    expect_words("a|b|c");
}

static void test_out_of_memory(void) {
    bool succeeded = false;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        odin3_vec_free(&words);
        odin3_arena_destroy(arena);
        arena = odin3_arena_create(1);
        TEST_ASSERT_NOT_NULL(arena);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, split(O3, "x"));
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = split(O3, "a \"b c\" \"d\\\"e\" f g h i j k l");
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            succeeded = true;
            expect_words("x|a|b c|d\"e|f|g|h|i|j|k|l");
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            expect_words("x");
        }
    }
    TEST_ASSERT_TRUE(succeeded);
}

/* The counter agrees with the split on every line that splits, in both modes. */
static void test_count_matches_split(void) {
    static const char *const lines[] = {
        "",           "   \t",       "a",         " a  b\tc ",
        "a#b # c d",  "a//b // c d", "\"a b\" c", "\"a\\\"b\\\\c\" d",
        "x\\y \"q\"", "a\"b c",      "#only",     "//x y",
    };
    static const odin3_words_mode modes[] = {ODIN3_WORDS_O3PROJ, ODIN3_WORDS_FLIST};
    for (size_t j = 0; j < sizeof modes / sizeof modes[0]; j++) {
        for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            odin3_vec_clear(&words);
            odin3_words_opts opts = {.offset = LINE_OFFSET, .mode = modes[j], .arena = arena};
            size_t over = 0;
            size_t count =
                odin3_words_count(odin3_bytes_cstr(lines[i]), &opts, SIZE_MAX - 1, &over);
            if (odin3_words_split(odin3_bytes_cstr(lines[i]), &opts, &words, &err) == ODIN3_OK) {
                TEST_ASSERT_EQUAL_size_t_MESSAGE(words.len, count, lines[i]);
            }
        }
    }
}

/* Counting stops at room + 1 and names the offset of the word over the cap. */
static void test_count_stops_past_room(void) {
    odin3_words_opts opts = {.offset = 0, .mode = FL, .arena = arena};
    size_t over = 0;
    TEST_ASSERT_EQUAL_size_t(
        2, odin3_words_count(odin3_bytes_cstr("a \"b c\"  d e"), &opts, 1, &over));
    TEST_ASSERT_EQUAL_size_t(2, over);
    TEST_ASSERT_EQUAL_size_t(
        3, odin3_words_count(odin3_bytes_cstr("a \"b c\"  d // e f"), &opts, 5, &over));
    TEST_ASSERT_EQUAL_size_t(9, over);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_blanks_are_space_and_tab);
    RUN_TEST(test_offsets);
    RUN_TEST(test_hash_comment_both_modes);
    RUN_TEST(test_slash_comment_only_in_flist);
    RUN_TEST(test_comment_markers_inside_a_word_are_ordinary);
    RUN_TEST(test_quoted_word_is_one_word);
    RUN_TEST(test_quoted_kept_in_o3proj_forgotten_in_flist);
    RUN_TEST(test_escapes_inside_quotes);
    RUN_TEST(test_bad_escape_is_syntax);
    RUN_TEST(test_empty_quoted_word_is_syntax);
    RUN_TEST(test_characters_after_closing_quote);
    RUN_TEST(test_unterminated_quote);
    RUN_TEST(test_bare_backslash_is_ordinary);
    RUN_TEST(test_quote_inside_a_bare_word);
    RUN_TEST(test_nul_byte_in_word);
    RUN_TEST(test_append_and_failure_atomicity);
    RUN_TEST(test_out_of_memory);
    RUN_TEST(test_count_matches_split);
    RUN_TEST(test_count_stops_past_room);
    return UNITY_END();
}
