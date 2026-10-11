/*
 * test_pp_include.c — unit tests for `include (2B spec §7.7 PP-11): the search order, cycles,
 * missing files, a file included twice, the includer's directory, the protective space at an
 * included file's end, the include chain in diagnostics, conditionals and translate_off across
 * file boundaries, the file name's syntax, files_read, the include caps and an OOM sweep.
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

enum { PATH_BUF = PP_PATH_BUF, DIR_BUF = 512, OOM_LIMIT = 6000 };

void setUp(void) {
    pp_fixture_set_up();
}

void tearDown(void) {
    pp_fixture_tear_down();
}

/* --- helpers ------------------------------------------------------------------------------- */

/* Writes tmp/name (a fresh path each time, so the file cache never holds an older copy). */
static void put_new(const char *name, const char *text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    TEST_ASSERT_TRUE(access(path, F_OK) != 0);
    put(name, odin3_bytes_cstr(text), NULL);
}

static void make_pp(const char *const *incdirs, uint32_t nincdirs) {
    odin3_pp_config config = {NULL, 0, incdirs, nincdirs};
    create_pp(&config);
}

static void set_limits(odin3_pp_limits limits) {
    ensure_pp();
    odin3_pp_test_set_limits(pp, limits);
}

/* Runs dir/name (dir NULL: the temporary directory) given as name. */
static odin3_status run_file(const char *dir, const char *name) {
    ensure_pp();
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", dir != NULL ? dir : tmp, name);
    odin3_pp_source src = {id(name), id(path), id("work"), {0}};
    return odin3_pp_run(pp, &src, &stream);
}

static void assert_error(const char *dir, const char *name, const char *want) {
    errors = 0;
    captured[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run_file(dir, name), want);
    TEST_ASSERT_EQUAL_STRING(want, captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_NULL(stream.text.ptr);
}

/* The FILE buffer made right after the stream's own (IDs are given in creation order). */
static odin3_srcbuf_id nth_buffer(uint32_t n) {
    return (odin3_srcbuf_id){stream.stream.v + n};
}

static void data_dir(const char *sub, char *out) {
    (void)snprintf(out, DIR_BUF, "%s/%s", ODIN3_PP_INCLUDE_DATA, sub);
}

static void assert_files_read(const char *const *want, uint32_t n) {
    const uint32_t *paths = NULL;
    TEST_ASSERT_EQUAL_UINT32(n, odin3_pp_files_read(pp, &paths));
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], odin3_strtab_get(odin3_design_strtab(design), paths[i]));
    }
}

/* --- the search (PP-11) -------------------------------------------------------------------- */

/* include_order: the includer's directory first, then the incdirs in order (first wins). */
static void test_search_order(void) {
    char inc_a[DIR_BUF];
    char inc_b[DIR_BUF];
    char src[DIR_BUF];
    data_dir("order/incA", inc_a);
    data_dir("order/incB", inc_b);
    data_dir("order/src", src);
    const char *const incdirs[] = {inc_a, inc_b};
    make_pp(incdirs, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(src, "top.v"));
    assert_text("incA_op\n\nsrc_here\n\nincB_only\n\n");
    char want[4][PATH_BUF];
    (void)snprintf(want[0], PATH_BUF, "%s/top.v", src);
    (void)snprintf(want[1], PATH_BUF, "%s/op.vh", inc_a);
    (void)snprintf(want[2], PATH_BUF, "%s/here.vh", src);
    (void)snprintf(want[3], PATH_BUF, "%s/only_b.vh", inc_b);
    const char *const wants[] = {want[0], want[1], want[2], want[3]};
    assert_files_read(wants, 4);
    /* the order of the incdirs decides */
    const char *const reversed[] = {inc_b, inc_a};
    make_pp(reversed, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(src, "top.v"));
    assert_text("incB_op\n\nsrc_here\n\nincB_only\n\n");
}

/* The includer's directory is the directory of the file that holds the `include. */
static void test_includer_directory(void) {
    char dir[DIR_BUF];
    data_dir("nest", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(dir, "top.v"));
    assert_text("sub_b\n\n\n");
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, nth_buffer(2), &info));
    TEST_ASSERT_EQUAL_UINT32(id("b.vh"), info.name); /* as written */
    char want[PATH_BUF];
    (void)snprintf(want, sizeof want, "%s/sub/b.vh", dir);
    TEST_ASSERT_EQUAL_UINT32(id(want), info.resolved);
}

static void test_absolute_name_stands(void) {
    char text[PATH_BUF * 2];
    (void)snprintf(text, sizeof text, "`include \"%s/abs.vh\"\n", tmp);
    put_new("abs.vh", "abs\n");
    put_new("t.v", text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("abs\n\n");
}

static void test_not_found(void) {
    char dir[DIR_BUF];
    data_dir("notfound", dir);
    const char *const incdirs[] = {tmp};
    make_pp(incdirs, 1);
    assert_error(dir, "top.v", "top.v:2:1: error: include_not_found: 'missing.vh'");
    /* a directory is not a regular file */
    put_new("t.v", "x `include \"..\"\n");
    assert_error(NULL, "t.v", "t.v:1:3: error: include_not_found: '..'");
}

static void test_cycle(void) {
    char dir[DIR_BUF];
    data_dir("cycle", dir);
    assert_error(dir, "top.v",
                 "b.vh:2:1: error: include_cycle: 'a.vh'\n"
                 "  included from a.vh:2\n"
                 "  included from top.v:2");
    put_new("self.v", "\n\n`include \"self.v\"\n");
    assert_error(NULL, "self.v", "self.v:3:1: error: include_cycle: 'self.v'");
}

/* Two buffers (with their lines), one cache entry; the stack is popped after each. */
static void test_included_twice(void) {
    char dir[DIR_BUF];
    data_dir("twice", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(dir, "top.v"));
    assert_text("wire w;\nwire v;\n\nwire w;\nwire v;\n\n");
    for (uint32_t i = 1; i <= 2; i++) {
        odin3_srcbuf_info info;
        TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, nth_buffer(i), &info));
        TEST_ASSERT_EQUAL_UINT32(id("w.vh"), info.name);
        TEST_ASSERT_EQUAL_UINT32(3, info.lines);
        TEST_ASSERT_EQUAL_UINT32(odin3_srcman_loc(sm, stream.stream, (i - 1) * 16).v, info.parent);
    }
    TEST_ASSERT_EQUAL_size_t(3, pp->cache.len);    /* <command line>, top.v, w.vh */
    TEST_ASSERT_EQUAL_size_t(0, pp->incstack.len); /* every file popped, the stream's own too */
}

/* --- buffers, segments, the protective space ----------------------------------------------- */

static void test_include_buffer_and_segments(void) {
    put_new("nonl.vh", "endmodule");
    put_new("t.v", "x`include \"nonl.vh\"y\n`include \"nonl.vh\"\nmodule");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("x endmodule y\nendmodule\nmodule");
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, nth_buffer(1), &info));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, info.kind);
    TEST_ASSERT_EQUAL_UINT32(id("nonl.vh"), info.name);
    char resolved[PATH_BUF];
    (void)snprintf(resolved, sizeof resolved, "%s/nonl.vh", tmp);
    TEST_ASSERT_EQUAL_UINT32(id(resolved), info.resolved);
    TEST_ASSERT_EQUAL_UINT32(id("work"), info.library);
    TEST_ASSERT_EQUAL_UINT32(odin3_srcman_loc(sm, stream.stream, 1).v, info.parent);
    odin3_srcbuf_info top;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, stream.stream, &top));
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, (odin3_loc){top.start});
    const odin3_srcfile_rec *file = odin3_srcman_file_of(sm, rec);
    const odin3_srcseg_rec *segs = (const odin3_srcseg_rec *)file->segs.data;
    const uint32_t want[][2] = {
        {0, odin3_srcman_loc(sm, stream.stream, 0).v},
        {1, 0},
        {2, odin3_srcman_loc(sm, nth_buffer(1), 0).v},
        {11, 0},
        {12, odin3_srcman_loc(sm, stream.stream, 19).v},
        {14, odin3_srcman_loc(sm, nth_buffer(2), 0).v},
        {23, odin3_srcman_loc(sm, stream.stream, 39).v},
    };
    TEST_ASSERT_EQUAL_size_t(sizeof want / sizeof want[0], file->segs.len);
    for (size_t i = 0; i < file->segs.len; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i][0], segs[i].out_offset);
        TEST_ASSERT_EQUAL_UINT32(want[i][1], segs[i].loc);
    }
}

static void test_empty_include(void) {
    put_new("empty.vh", "");
    put_new("t.v", "a`include \"empty.vh\"b");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("a b");
}

/* An error inside an included file prints the include chain. */
static void test_error_chain(void) {
    put_new("e.vh", "\n x `endif\n");
    put_new("t.v", "a\n`include \"e.vh\"\n");
    assert_error(NULL, "t.v",
                 "e.vh:2:4: error: syntax: `endif without `ifdef\n  included from t.v:2");
}

/* --- conditionals and translate_off across files (PP-4, PP-10) ----------------------------- */

static void test_endif_cannot_close_includer(void) {
    put_new("e.vh", "b `endif\n");
    put_new("t.v", "`ifndef X\n`include \"e.vh\"\n`endif\n");
    assert_error(NULL, "t.v",
                 "e.vh:1:3: error: syntax: `endif without `ifdef in this file (conditionals do "
                 "not cross `include boundaries)\n  included from t.v:2");
}

static void test_unterminated_in_included_file(void) {
    put_new("o.vh", "`ifdef A\n");
    put_new("t.v", "`include \"o.vh\"\nx\n`endif\n");
    assert_error(NULL, "t.v",
                 "o.vh:1:1: error: syntax: unterminated `ifdef\n  included from t.v:1");
}

/* Each file has its own conditional stack (the depth cap is per file). */
static void test_cond_depth_per_file(void) {
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.cond_depth = 1;
    set_limits(limits);
    put_new("c.vh", "`ifndef B\nb\n`endif\n");
    put_new("t.v", "`ifndef A\n`include \"c.vh\"\n`endif\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("\n\nb\n\n\n\n");
}

static void test_include_in_skipped_region(void) {
    put_new("t.v", "`ifdef X `include \"nowhere.vh\" `include <bad> `endif.");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text(".");
}

/* PP-10's region is the stream's text: it may end in the includer. */
static void test_translate_off_across_include_end(void) {
    put_new("x.vh", "a\n// synopsys translate_off\nb\n");
    put_new("t.v", "`include \"x.vh\"\nc\n// synopsys translate_on\nd\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("a\n\nd\n");
    put_new("y.vh", "a\n// synopsys translate_off\nb\n");
    put_new("u.v", "`include \"y.vh\"\nc\n");
    assert_error(NULL, "u.v",
                 "y.vh:2:1: error: syntax: unterminated translate_off\n  included from u.v:1");
}

/* --- the file name ------------------------------------------------------------------------- */

static void test_file_name_syntax(void) {
    static const struct {
        const char *text;
        const char *want;
    } cases[] = {
        {"`include <x.vh>\n", "t0.v:1:10: error: syntax: `include takes a double-quoted file name"},
        {"`include `F\n", "t1.v:1:10: error: syntax: `include takes a double-quoted file name"},
        {"`include\n", "t2.v:1:9: error: syntax: `include takes a double-quoted file name"},
        {"`include", "t3.v:1:9: error: syntax: `include takes a double-quoted file name"},
        {"`include x.vh\n", "t4.v:1:10: error: syntax: `include takes a double-quoted file name"},
        {"`include \"x.vh\n\"", "t5.v:1:10: error: syntax: unterminated file name of `include"},
        {"`include \"x.vh", "t6.v:1:10: error: syntax: unterminated file name of `include"},
        {"\t`include \t\"\"\n",
         "t7.v:1:12: error: syntax: `include file name is empty or holds a NUL"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char name[PATH_BUF / 4];
        (void)snprintf(name, sizeof name, "t%zu.v", i);
        put_new(name, cases[i].text);
        assert_error(NULL, name, cases[i].want);
    }
}

static void test_files_read_first_read_order(void) {
    put_new("b.vh", "b\n");
    put_new("a.vh", "`include \"b.vh\"a\n");
    put_new("t.v", "`include \"a.vh\"\n`include \"b.vh\"\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "t.v"));
    assert_text("b\na\n\nb\n\n");
    char want[3][PATH_BUF];
    (void)snprintf(want[0], PATH_BUF, "%s/t.v", tmp);
    (void)snprintf(want[1], PATH_BUF, "%s/a.vh", tmp);
    (void)snprintf(want[2], PATH_BUF, "%s/b.vh", tmp);
    const char *const wants[] = {want[0], want[1], want[2]};
    assert_files_read(wants, 3);
}

/* --- caps ---------------------------------------------------------------------------------- */

static void test_cap_include_depth(void) {
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.include_depth = 1;
    set_limits(limits);
    put_new("b.vh", "b\n");
    put_new("a.vh", "`include \"b.vh\"\n");
    put_new("ok.v", "`include \"b.vh\"\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_file(NULL, "ok.v"));
    put_new("t.v", "`include \"a.vh\"\n");
    assert_error(NULL, "t.v",
                 "a.vh:1:1: error: syntax: include depth exceeds ODIN3_PP_MAX_INCLUDE_DEPTH (1)\n"
                 "  included from t.v:1");
}

static void test_cap_includes_per_stream(void) {
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.includes_per_stream = 2; /* the stream's own file counts */
    set_limits(limits);
    put_new("w.vh", "w\n");
    put_new("t.v", "`include \"w.vh\"\n`include \"w.vh\"\n");
    assert_error(NULL, "t.v",
                 "t.v:2:1: error: syntax: files exceeds ODIN3_PP_MAX_INCLUDES_PER_STREAM (2)");
}

static void test_cap_file_cache_bytes(void) {
    odin3_pp_limits limits = odin3_pp_default_limits();
    limits.file_cache_bytes = 24;
    set_limits(limits);
    put_new("big.vh", "0123456789\n");
    put_new("t.v", "\n\n`include \"big.vh\"\n"); /* 20 bytes */
    assert_error(NULL, "t.v",
                 "t.v:3:1: error: syntax: file cache bytes exceeds ODIN3_PP_MAX_FILE_CACHE_BYTES "
                 "(24)");
}

/* --- out of memory ------------------------------------------------------------------------- */

static const char k_oom_want[] = "b\na\n\nb\n\ninc\n\n";

static bool oom_try(long fail_after, const char *path) {
    odin3_pp_destroy(pp);
    pp = NULL;
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    char inc[PATH_BUF];
    char incd[PATH_BUF];
    (void)snprintf(inc, sizeof inc, "%s/nowhere", tmp);
    (void)snprintf(incd, sizeof incd, "%s/incd", tmp);
    const char *const incdirs[] = {inc, incd};
    odin3_pp_config config = {NULL, 0, incdirs, 2};
    odin3_pp_source src = {id("t.v"), id(path), id("work"), {0}};
    memset(&stream, 0, sizeof stream);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_pp_create(design, &config, &pp);
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
    put_new("b.vh", "b\n");
    put_new("a.vh", "`include \"b.vh\"a\n");
    char incd[PATH_BUF];
    (void)snprintf(incd, sizeof incd, "%s/incd", tmp);
    TEST_ASSERT_EQUAL_INT(0, mkdir(incd, 0700));
    remember(incd);
    put_new("incd/inc.vh", "inc\n");
    put_new("t.v", "`include \"a.vh\"\n`include \"b.vh\"\n`include \"inc.vh\"\n");
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.v", tmp);
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(k, path);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 30);
    TEST_ASSERT_EQUAL_INT(0, errors);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_search_order);
    RUN_TEST(test_includer_directory);
    RUN_TEST(test_absolute_name_stands);
    RUN_TEST(test_not_found);
    RUN_TEST(test_cycle);
    RUN_TEST(test_included_twice);
    RUN_TEST(test_include_buffer_and_segments);
    RUN_TEST(test_empty_include);
    RUN_TEST(test_error_chain);
    RUN_TEST(test_endif_cannot_close_includer);
    RUN_TEST(test_unterminated_in_included_file);
    RUN_TEST(test_cond_depth_per_file);
    RUN_TEST(test_include_in_skipped_region);
    RUN_TEST(test_translate_off_across_include_end);
    RUN_TEST(test_file_name_syntax);
    RUN_TEST(test_files_read_first_read_order);
    RUN_TEST(test_cap_include_depth);
    RUN_TEST(test_cap_includes_per_stream);
    RUN_TEST(test_cap_file_cache_bytes);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
