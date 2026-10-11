/*
 * test_pp_props.c — the preprocessor's properties (2B spec §10.5), over every micro under
 * tests/micro/verilog, the reviewer's probes and seeded random files:
 *   1. a file without a backtick or translate_off preprocesses to itself, as one segment;
 *   2. re-preprocessing an output (directive-free: every macro use replaced by its expansion
 *      text) gives the same text again, as one segment (idempotence).
 * The pp-fuzz model-based fuzzer (tools/pp-fuzz) and fuzz_pp (tests/fuzz) cover the rest.
 */
#include "frontends/verilog/preproc.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/hash.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ODIN3_PP_MICROS
#error "ODIN3_PP_MICROS must name tests/micro/verilog"
#endif
#ifndef ODIN3_PP_PROBES
#error "ODIN3_PP_PROBES must name tests/unit/data/pp/probes"
#endif

enum {
    MAX_FILES = 1024,
    MAX_DIRS = 64,
    RANDOM_CASES = 2000,
    RANDOM_MAX_LEN = 300,
    GENERATED_CASES = 500,
    TEXT_MAX = 1 << 16,
    /* At least this many micros and probes are checked by each property (the tree holds 539
     * .v files and some headers; a quiet shrink of the sweep would fail here). */
    MIN_MICROS_PLAIN = 300,
    MIN_MICROS_IDEMPOTENT = 450
};

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- the files: every .v and .vh under the micros and the probes, smallest first ---------- */

typedef struct src_file {
    char path[PP_PATH_BUF];
    long size;
} src_file;

static src_file files[MAX_FILES];
static size_t nfiles;

static bool is_source(const char *name) {
    size_t len = strlen(name);
    return (len > 2 && strcmp(name + len - 2, ".v") == 0) ||
           (len > 3 && strcmp(name + len - 3, ".vh") == 0);
}

static void add_file(const char *path, long size) {
    TEST_ASSERT_TRUE(nfiles < MAX_FILES);
    memcpy(files[nfiles].path, path, PP_PATH_BUF);
    files[nfiles++].size = size;
}

/* One directory entry: a subdirectory is pushed, a source file added. */
static void visit(const char *dir, const char *name, char (*stack)[PP_PATH_BUF], int *depth) {
    char path[PP_PATH_BUF];
    int len = snprintf(path, sizeof path, "%s/%s", dir, name);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &info));
    bool subdir = S_ISDIR(info.st_mode);
    TEST_ASSERT_TRUE(!subdir || *depth < MAX_DIRS);
    if (subdir) {
        memcpy(stack[(*depth)++], path, PP_PATH_BUF);
    } else if (S_ISREG(info.st_mode) && is_source(name)) {
        add_file(path, (long)info.st_size);
    }
}

/* Every source file under root (an explicit stack of directories). */
static void collect(const char *root) {
    static char stack[MAX_DIRS][PP_PATH_BUF];
    int depth = 0;
    (void)snprintf(stack[depth++], PP_PATH_BUF, "%s", root);
    while (depth > 0) {
        char dir_path[PP_PATH_BUF];
        memcpy(dir_path, stack[--depth], PP_PATH_BUF);
        DIR *dir = opendir(dir_path);
        TEST_ASSERT_NOT_NULL_MESSAGE(dir, dir_path);
        for (struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                visit(dir_path, ent->d_name, stack, &depth);
            }
        }
        TEST_ASSERT_EQUAL_INT(0, closedir(dir));
    }
}

static int by_size(const void *lhs, const void *rhs) {
    const src_file *left = lhs;
    const src_file *right = rhs;
    if (left->size != right->size) {
        return left->size < right->size ? -1 : 1;
    }
    return strcmp(left->path, right->path);
}

static void collect_all(void) {
    nfiles = 0;
    collect(ODIN3_PP_MICROS);
    collect(ODIN3_PP_PROBES);
    qsort(files, nfiles, sizeof files[0], by_size);
}

/* The bytes of path (malloc'ed, *len set). */
static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));
    long size = ftell(fp);
    TEST_ASSERT_TRUE(size >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_SET));
    char *bytes = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t(size, fread(bytes, 1, (size_t)size, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    bytes[size] = '\0';
    *len = (size_t)size;
    return bytes;
}

static bool contains(const char *bytes, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(bytes + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* --- property 1: a file without directives is its own output ------------------------------ */

/* The last stream is exactly want, as one segment (none for an empty text) at offset 0 of the
 * stream's own file. */
static void assert_identity(const char *what, const char *want, size_t len) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(len, stream.text.len, what);
    if (len > 0) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, stream.text.ptr, len, what);
    }
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(len > 0 ? 1 : 0, count, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(stream.nsegments, count, what);
    if (count == 1) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, segs[0].out_offset, what);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(at_offset(0), segs[0].loc, what);
    }
}

static void test_micros_without_directives_are_their_own_output(void) {
    collect_all();
    size_t checked = 0;
    for (size_t i = 0; i < nfiles; i++) {
        size_t len = 0;
        char *bytes = slurp(files[i].path, &len);
        if (!contains(bytes, len, "`") && !contains(bytes, len, "translate_off")) {
            create_pp(NULL);
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_path(files[i].path, files[i].path),
                                          captured);
            assert_identity(files[i].path, bytes, len);
            checked++;
        }
        free(bytes);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(MIN_MICROS_PLAIN, checked);
}

/* A small seeded generator (xorshift32), the same sequence on every machine. */
static uint32_t rng_state;

static uint32_t next_random(void) {
    uint32_t bits = rng_state;
    bits ^= bits << 13;
    bits ^= bits >> 17;
    bits ^= bits << 5;
    rng_state = bits;
    return bits;
}

/* Appends piece to the text being built (len bytes so far), which must fit. */
static void append(char *text, size_t *len, const char *piece) {
    int added = snprintf(text + *len, TEXT_MAX - *len, "%s", piece);
    TEST_ASSERT_TRUE(added >= 0 && *len + (size_t)added < TEXT_MAX);
    *len += (size_t)added;
}

static uint32_t below(uint32_t n) {
    return next_random() % n;
}

/* Text pieces a directive-free file is made of: identifiers and numbers, every blank (a lone CR
 * too), operators, comments (one unterminated), strings (escapes, one unterminated, `//` inside),
 * escaped identifiers, a stray translate_on comment (a warning, the comment stays), a
 * translate_offset look-alike, a lone backslash and bytes above 0x7f. */
static const char *const k_plain_pieces[] = {
    "module",
    " ",
    "\t",
    "\n",
    "\r\n",
    "\r",
    "w_1",
    "8'hff",
    "$display",
    "(",
    ")",
    ",",
    ";",
    "+",
    "/",
    "*",
    "//",
    "/*",
    "*/",
    "// c\n",
    "/* c\n d */",
    "\"s // t\"",
    "\"\\\"\"",
    "\"open\n",
    "\\esc+id ",
    "\\",
    "// synopsys translate_on\n",
    "/* translate_offset */",
    "\xc3\xa9",
    "\xff",
    "'",
    "#",
    "=",
    "\"",
    "{",
    "}",
    "[",
    "]",
};

static void test_random_text_without_directives_is_its_own_output(void) {
    rng_state = UINT32_C(0x2b10f00d);
    static char text[TEXT_MAX];
    for (int num = 0; num < RANDOM_CASES; num++) {
        size_t len = 0;
        text[0] = '\0';
        uint32_t pieces = below(RANDOM_MAX_LEN / 4);
        for (uint32_t i = 0; i < pieces; i++) {
            append(text, &len,
                   k_plain_pieces[below(sizeof k_plain_pieces / sizeof k_plain_pieces[0])]);
        }
        create_pp(NULL);
        char what[64];
        (void)snprintf(what, sizeof what, "random case %d", num);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_fresh("plain.v", (odin3_bytes){text, len}),
                                      captured);
        assert_identity(what, text, len);
    }
}

/* --- property 2: an output re-preprocesses to itself -------------------------------------- */

/* Runs the last stream's text as a fresh file through a fresh preprocessor: OK, and it is its
 * own output as one segment. */
static void assert_idempotent(const char *what) {
    size_t len = stream.text.len;
    char *first = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(first);
    if (len > 0) {
        memcpy(first, stream.text.ptr, len);
    }
    create_pp(NULL);
    odin3_status st = run_fresh("again.v", (odin3_bytes){first, len});
    char msg[PP_MSG_MAX + 64];
    (void)snprintf(msg, sizeof msg, "%s: %s", what, captured);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, msg);
    assert_identity(what, first, len);
    free(first);
}

static void test_micro_outputs_are_idempotent(void) {
    collect_all();
    size_t checked = 0;
    for (size_t i = 0; i < nfiles; i++) {
        create_pp(NULL);
        if (run_path(files[i].path, files[i].path) == ODIN3_OK) {
            assert_idempotent(files[i].path);
            checked++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(MIN_MICROS_IDEMPOTENT, checked);
}

/* Seeded files of macros (defaults, empty actuals, nesting through bodies and actuals, a body
 * ending in a use whose actuals follow it), conditionals (mid-line too), pass-through
 * directives, comments, strings and escaped identifiers holding backticks. */
static const char k_head[] = "`define A(x, y=1) (x + y)\n"
                             "`define B `A(2)\n"
                             "`define C(p, q) {p, `A(q, p)} // c\n"
                             "`define D `A\n"
                             "`define E\n"
                             "`define F(s) s`E \\\n  + s /* c */\n";
static const char *const k_uses[] = {
    "`A(1)",
    "`A(1, 2)",
    "`A(,3)",
    "`B",
    "`C(`B, (1, 2))",
    "`D(4, 5)",
    "x`E",
    "`F(z)",
    "`C(`A(`B), [1:0])",
    "\"`A(1)\"",
    "/* `B */",
    "\\e`B ",
    "`timescale 1ns/1ps\n",
    "`ifdef E 1 `else 2 `endif",
    "`ifndef E `A(0) `elsif B 3 `endif",
    "\n",
    " ",
};

static void test_generated_outputs_are_idempotent(void) {
    rng_state = UINT32_C(0x5eed1e55);
    static char text[TEXT_MAX];
    for (int num = 0; num < GENERATED_CASES; num++) {
        size_t len = 0;
        append(text, &len, k_head);
        uint32_t uses = below(40);
        for (uint32_t i = 0; i < uses; i++) {
            append(text, &len, k_uses[below(sizeof k_uses / sizeof k_uses[0])]);
            append(text, &len, below(3) == 0 ? "\n" : " ");
        }
        create_pp(NULL);
        char what[64];
        (void)snprintf(what, sizeof what, "generated case %d", num);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_fresh("gen.v", (odin3_bytes){text, len}),
                                      captured);
        assert_idempotent(what);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_micros_without_directives_are_their_own_output);
    RUN_TEST(test_random_text_without_directives_is_its_own_output);
    RUN_TEST(test_micro_outputs_are_idempotent);
    RUN_TEST(test_generated_outputs_are_idempotent);
    return UNITY_END();
}
