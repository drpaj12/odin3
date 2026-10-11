/*
 * test_pp_check.c — unit tests for odin3_pp_check (2B spec §7.8): the stream invariants on the
 * expansion probes and on every Verilog micro (smallest first: the preprocessor micros, the
 * syntax define micros, then all 610 files under tests/micro/verilog), the per-buffer
 * reachability rule with the DROP/F(a) example, and one corruption per rule through the hidden
 * hook.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#ifndef ODIN3_PP_PROBES
#error "ODIN3_PP_PROBES must name tests/unit/data/pp/probes"
#endif
#ifndef ODIN3_PP_MICROS
#error "ODIN3_PP_MICROS must name tests/micro/verilog"
#endif

enum { PATH_BUF = PP_PATH_BUF };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

static void fresh_pp(void) {
    create_pp(NULL);
}

static odin3_status run(const char *text) {
    return run_fresh("c.v", odin3_bytes_cstr(text));
}

/* Runs text (a fresh preprocessor) and checks the stream explicitly. */
static void assert_checked(const char *text) {
    fresh_pp();
    odin3_status st = run(text);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(captured);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_check(pp, stream.stream));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* A file to check: dir/file and the error it must stop at (NULL: none). */
typedef struct file_case {
    const char *dir;
    const char *file;
    const char *want;
} file_case;

/* Runs a file (a fresh preprocessor) and checks the stream, or requires a PARSE error that
 * contains want. Returns true when the stream was checked. */
static bool check_file(file_case fc) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", fc.dir, fc.file);
    fresh_pp();
    errors = 0;
    captured[0] = '\0';
    odin3_status st = run_path(fc.file, path);
    if (fc.want != NULL) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, st, fc.file);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured, fc.want), captured);
        return false;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, captured);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pp_check(pp, stream.stream), fc.file);
    return true;
}

static const odin3_pp_bufrec *bufrec(uint32_t n) {
    TEST_ASSERT_TRUE(n < pp->bufs.len);
    return odin3_vec_cat(&pp->bufs, n);
}

/* --- the invariants hold ------------------------------------------------------------------- */

static void test_plain_file(void) {
    assert_checked("module m;\n  wire a; // c\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, stream.nsegments);
}

static void test_empty_file(void) {
    assert_checked("");
    TEST_ASSERT_EQUAL_UINT32(0, stream.nsegments);
}

static void test_2a_example(void) {
    assert_checked("`define INNER(p) (p + 1)\n`define OUTER(q) `INNER(q * 2)\n"
                   "assign y = `OUTER(xx);\n");
    for (uint32_t i = 1; i < pp->bufs.len; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(0, bufrec(i)->reach); /* A1 counts through A2's def */
    }
}

static void test_inserted_bytes(void) {
    assert_checked("`define N(x) [x]\n`define M a/**/b\nq = `N(p /* c */ q) `M;\n"
                   "a`undef M`define Z 1\n");
}

/* §7.8: only F's buffer must be reachable; DROP's expansion and a's argument count zero. */
static void test_drop_example_reachability(void) {
    assert_checked("`define DROP(x)\n`define F(x) `DROP(x) z\nq = `F(a);\n");
    TEST_ASSERT_EQUAL_UINT32(4, pp->bufs.len); /* c.v, E(F), A(x <- a), E(DROP) */
    TEST_ASSERT_NOT_EQUAL_UINT32(0, bufrec(1)->reach);
    TEST_ASSERT_EQUAL_UINT32(0, bufrec(2)->reach);
    TEST_ASSERT_EQUAL_UINT32(0, bufrec(3)->reach);
}

static void test_probes(void) {
    static const char *const k_ok[] = {"t1.v", "t2.v", "t3.v", "t6.v", "u2.v", "v1.v",
                                       "w2.v", "w3.v", "w4.v", "w5.v", "t7.v", "t9.v"};
    uint32_t checked = 0;
    for (size_t i = 0; i < sizeof k_ok / sizeof k_ok[0]; i++) {
        checked += check_file((file_case){ODIN3_PP_PROBES, k_ok[i], NULL}) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof k_ok / sizeof k_ok[0], checked);
    (void)check_file((file_case){ODIN3_PP_PROBES, "w1.v", "recursive macro 'N'"});
    (void)check_file((file_case){ODIN3_PP_PROBES, "rec.v", "recursive macro 'R'"});
}

/* --- the micro sweeps ---------------------------------------------------------------------- */

enum { MAX_MICROS = 1024, MAX_DIRS = 64, REL_BUF = 256 };

/* One micro: its path, its path under ODIN3_PP_MICROS, its size and whether some OK run of
 * the sweep read it (as its root or through an `include). */
typedef struct micro {
    char path[PATH_BUF];
    char rel[REL_BUF];
    long size;
    bool reached;
} micro;

static micro micros[MAX_MICROS];
static size_t nmicros;

/* The files a sweep takes: under ODIN3_PP_MICROS/sub (sub "": the whole tree), descending
 * into subdirectories when deep, .v and .vh files whose name holds part (NULL: any). */
typedef struct scope {
    const char *sub;
    const char *part;
    bool deep;
} scope;

static bool ends_with(const char *name, const char *suffix) {
    size_t len = strlen(name);
    size_t slen = strlen(suffix);
    return len > slen && strcmp(name + len - slen, suffix) == 0;
}

/* out = head/tail (one alone when the other is empty), which must fit. */
static void join(char *out, size_t size, const char *head, const char *tail) {
    const char *slash = head[0] != '\0' && tail[0] != '\0' ? "/" : "";
    int len = snprintf(out, size, "%s%s%s", head, slash, tail);
    TEST_ASSERT_TRUE(len >= 0 && (size_t)len < size);
}

static bool takes(scope sc, const char *name) {
    bool source = ends_with(name, ".v") || ends_with(name, ".vh");
    return source && (sc.part == NULL || strstr(name, sc.part) != NULL);
}

static void add_micro(const char *rel, const char *path, long size) {
    TEST_ASSERT_TRUE(nmicros < MAX_MICROS);
    micro *mic = &micros[nmicros++];
    memcpy(mic->rel, rel, REL_BUF);
    memcpy(mic->path, path, PATH_BUF);
    mic->size = size;
    mic->reached = false;
}

/* The directories still to list (an explicit stack, under ODIN3_PP_MICROS). */
typedef struct dir_stack {
    char dirs[MAX_DIRS][REL_BUF];
    int len;
} dir_stack;

/* One entry of rel_dir: a subdirectory is pushed (when sc.deep), a file sc takes is added. */
static void visit(scope sc, const char *rel_dir, const char *name, dir_stack *stack) {
    char rel[REL_BUF];
    char path[PATH_BUF];
    join(rel, sizeof rel, rel_dir, name);
    join(path, sizeof path, ODIN3_PP_MICROS, rel);
    struct stat info;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &info));
    if (S_ISDIR(info.st_mode) && sc.deep) {
        TEST_ASSERT_TRUE(stack->len < MAX_DIRS);
        memcpy(stack->dirs[stack->len++], rel, REL_BUF);
    } else if (S_ISREG(info.st_mode) && takes(sc, name)) {
        add_micro(rel, path, (long)info.st_size);
    }
}

/* Adds every file sc takes, hidden directories (keywords/.generic) included. */
static void collect(scope sc) {
    static dir_stack stack;
    stack.len = 0;
    join(stack.dirs[stack.len++], REL_BUF, "", sc.sub);
    while (stack.len > 0) {
        char rel_dir[REL_BUF];
        memcpy(rel_dir, stack.dirs[--stack.len], REL_BUF);
        char dir_path[PATH_BUF];
        join(dir_path, sizeof dir_path, ODIN3_PP_MICROS, rel_dir);
        DIR *dir = opendir(dir_path);
        TEST_ASSERT_NOT_NULL_MESSAGE(dir, dir_path);
        for (struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                visit(sc, rel_dir, ent->d_name, &stack);
            }
        }
        TEST_ASSERT_EQUAL_INT(0, closedir(dir));
    }
}

/* Smallest first (memory rule), ties by path, so the order is the same on every machine. */
static int by_size(const void *lhs, const void *rhs) {
    const micro *left = lhs;
    const micro *right = rhs;
    if (left->size != right->size) {
        return left->size < right->size ? -1 : 1;
    }
    return strcmp(left->rel, right->rel);
}

/* A micro the preprocessor rejects when run on its own: its path under ODIN3_PP_MICROS, the
 * error's text, and whether another micro includes it (and so preprocesses it in context). */
typedef struct rejected {
    const char *rel;
    const char *want;
    bool included;
} rejected;

static const char k_addition[] = "syntax: undefined macro 'ADDITION_num'";
static const char k_bits[] = "syntax: undefined macro 'BITS'";
static const char k_width[] = "syntax: undefined macro 'WIDTH'";
static const rejected k_rejected[] = {
    /* the three splicing micros, which Parmys rejects too (its golden log: "undefined macro
     * ADDITION_num"; PP-6, PHASE2 #11) */
    {"syntax/preprocessor_complex_define.v", k_addition, false},
    {"syntax/preprocessor_define.v", k_addition, false},
    {"syntax/preprocessor_define_with_comment.v", k_addition, false},
    /* the module files of the four multi-file syntax micros, using `BITS from their includer
     * syntax/<name>.v; the includer's conditional leaves one variant of two unread */
    {"syntax/ifdef-else-syntax/module_a_and.v", k_bits, true},
    {"syntax/ifdef-else-syntax/module_a_or.v", k_bits, false},
    {"syntax/ifdef-else-syntax/module_b.v", k_bits, true},
    {"syntax/ifndef-else-syntax/module_a_and.v", k_bits, false},
    {"syntax/ifndef-else-syntax/module_a_or.v", k_bits, true},
    {"syntax/ifndef-else-syntax/module_b.v", k_bits, true},
    {"syntax/include-syntax/module_a.v", k_bits, true},
    {"syntax/include-syntax/module_b.v", k_bits, true},
    {"syntax/nested-ifdef-syntax/module_b.v", k_bits, true},
    /* the keyword templates, using `WIDTH and `operator from the keyword .vh micros */
    {"keywords/.generic/range_any_width_binary_test.v", k_width, true},
    {"keywords/.generic/range_any_width_unary_test.v", k_width, true},
    {"keywords/.generic/replicate_any_width_binary_test.v", k_width, true},
    {"keywords/.generic/replicate_any_width_unary_test.v", k_width, true},
    /* headers using macros of the .v micros that include them */
    {"keywords/assign/assign.vh", k_width, true},
    {"keywords/for/generate_for.vh", k_width, true},
    {"keywords/generate_endgenerate/generate_case.vh", "syntax: undefined macro 'sel'", true},
    {"keywords/generate_endgenerate/generate_else_if.vh", "syntax: undefined macro 'en'", true},
    {"keywords/generate_endgenerate/generate_if_else.vh", "syntax: undefined macro 'en'", true},
};

static const rejected *find_rejected(const char *rel) {
    for (size_t i = 0; i < sizeof k_rejected / sizeof k_rejected[0]; i++) {
        if (strcmp(k_rejected[i].rel, rel) == 0) {
            return &k_rejected[i];
        }
    }
    return NULL;
}

/* Marks every collected micro the last run read (the root and each included file). */
static void mark_reached(void) {
    const uint32_t *paths = NULL;
    uint32_t count = odin3_pp_files_read(pp, &paths);
    for (uint32_t i = 0; i < count; i++) {
        const char *path = odin3_strtab_get(odin3_design_strtab(design), paths[i]);
        for (size_t j = 0; j < nmicros; j++) {
            micros[j].reached = micros[j].reached || strcmp(micros[j].path, path) == 0;
        }
    }
}

/* Runs one micro with a fresh preprocessor: OK with odin3_pp_check passing (true), or the
 * listed PARSE error (false). */
static bool sweep_one(const micro *mic) {
    const rejected *rej = find_rejected(mic->rel);
    fresh_pp();
    errors = 0;
    captured[0] = '\0';
    odin3_status st = run_path(mic->rel, mic->path);
    if (rej != NULL) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, st, mic->rel);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured, rej->want), captured);
        return false;
    }
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(captured);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pp_check(pp, stream.stream), mic->rel);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, errors, mic->rel);
    mark_reached();
    return true;
}

/* The outcome of a sweep. */
typedef struct sweep_count {
    size_t files;
    size_t checked;  /* OK on their own, odin3_pp_check passing */
    size_t rejected; /* the listed error on their own */
    size_t through;  /* of those, read in context by an OK run that includes them */
} sweep_count;

/* Runs every collected micro on its own, smallest first, then requires each rejected micro
 * marked included to have been read by an OK run (and no other). */
static sweep_count sweep(void) {
    qsort(micros, nmicros, sizeof micros[0], by_size);
    sweep_count count = {nmicros, 0, 0, 0};
    for (size_t i = 0; i < nmicros; i++) {
        bool ok = sweep_one(&micros[i]);
        count.checked += ok ? 1 : 0;
        count.rejected += ok ? 0 : 1;
    }
    for (size_t i = 0; i < nmicros; i++) {
        const rejected *rej = find_rejected(micros[i].rel);
        if (rej != NULL) {
            TEST_ASSERT_EQUAL_MESSAGE(rej->included, micros[i].reached, micros[i].rel);
            count.through += micros[i].reached ? 1 : 0;
        }
    }
    return count;
}

/* Every file of tests/micro/verilog/preprocessor, the nested_ifdef.vh header included. */
static void test_preprocessor_micros(void) {
    nmicros = 0;
    collect((scope){"preprocessor", NULL, false});
    sweep_count count = sweep();
    TEST_ASSERT_EQUAL_size_t(22, count.files);
    TEST_ASSERT_EQUAL_size_t(22, count.checked);
}

/* The syntax micros whose name holds "define": the splicing three stop at `ADDITION_num
 * (PP-6). */
static void test_syntax_define_micros(void) {
    nmicros = 0;
    collect((scope){"syntax", "define", false});
    sweep_count count = sweep();
    TEST_ASSERT_EQUAL_size_t(3, count.files);
    TEST_ASSERT_EQUAL_size_t(3, count.rejected);
}

/* Every Verilog micro under tests/micro/verilog (.v and .vh, keywords/.generic included), each
 * on its own; the listed ones that need their includer's macros also read in context. */
static void test_every_verilog_micro(void) {
    enum { FILES = 610 };
    nmicros = 0;
    collect((scope){"", NULL, true});
    sweep_count count = sweep();
    char line[PP_MSG_MAX];
    (void)snprintf(line, sizeof line,
                   "micro sweep: %zu files: %zu OK and checked, %zu rejected as listed (%zu of "
                   "them read through an includer)",
                   count.files, count.checked, count.rejected, count.through);
    TEST_MESSAGE(line);
    TEST_ASSERT_EQUAL_size_t(FILES, count.files);
    TEST_ASSERT_EQUAL_size_t(sizeof k_rejected / sizeof k_rejected[0], count.rejected);
    TEST_ASSERT_EQUAL_size_t(FILES - count.rejected, count.checked);
}

static void test_check_misuse(void) {
    assert_checked("q;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_check(NULL, stream.stream));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pp_check(pp, (odin3_srcbuf_id){stream.stream.v + 1}));
}

/* --- one corruption per rule --------------------------------------------------------------- */

static const char k_rich[] = "`define DROP(x)\n`define F(x) `DROP(x) z\n`define N(y) [y]\n"
                             "q = `F(a) + `N(p /* c */ r);\n";

static void assert_corrupt(odin3_pp_test_target target, const char *want) {
    assert_checked(k_rich);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_test_corrupt(pp, target));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_pp_check(pp, stream.stream));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(want, captured);
}

static void test_corrupt_segment_start(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_START,
                   "odin3: error: pp_check: the first segment starts at output offset 1, not 0");
}

static void test_corrupt_segment_order(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_ORDER,
                   "odin3: error: pp_check: segment 1 at output offset 0 does not follow output "
                   "offset 0");
}

static void test_corrupt_segment_loc(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_LOC,
                   "odin3: error: pp_check: segment 0 has loc 4294967295, which does not decode");
}

static void test_corrupt_byte(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_BYTE,
                   "odin3: error: pp_check: output byte 0 differs from its spelling");
}

static void test_corrupt_loc0_blank(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_LOC0_BLANK,
                   "odin3: error: pp_check: output byte 15 has loc 0 and is not a blank");
}

static void test_corrupt_loc0_alone(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_LOC0_ALONE,
                   "odin3: error: pp_check: loc-0 segment 7 is 3 bytes, not 1");
}

static void test_corrupt_reach(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_REACH, "odin3: error: pp_check: buffer 2 of the run is "
                                           "reached by emitted bytes but from no segment");
}

static void test_corrupt_needs_a_target(void) {
    assert_checked("q;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pp_test_corrupt(pp, ODIN3_PP_CORRUPT_LOC0_BLANK));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_test_corrupt(pp, ODIN3_PP_CORRUPT_REACH));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_check(pp, stream.stream));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_file);
    RUN_TEST(test_empty_file);
    RUN_TEST(test_2a_example);
    RUN_TEST(test_inserted_bytes);
    RUN_TEST(test_drop_example_reachability);
    RUN_TEST(test_probes);
    RUN_TEST(test_preprocessor_micros);
    RUN_TEST(test_syntax_define_micros);
    RUN_TEST(test_every_verilog_micro);
    RUN_TEST(test_check_misuse);
    RUN_TEST(test_corrupt_segment_start);
    RUN_TEST(test_corrupt_segment_order);
    RUN_TEST(test_corrupt_segment_loc);
    RUN_TEST(test_corrupt_byte);
    RUN_TEST(test_corrupt_loc0_blank);
    RUN_TEST(test_corrupt_loc0_alone);
    RUN_TEST(test_corrupt_reach);
    RUN_TEST(test_corrupt_needs_a_target);
    return UNITY_END();
}
