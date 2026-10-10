/*
 * test_project_record.c — unit tests for the project record (PRJ-1..4): design ownership, project
 * file loading, builders and their located errors, finish, the JSON dump (hand-built, empty,
 * escapes, the single_file corpus case) and an OOM sweep with the record dropped.
 */
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "frontends/project/project_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

enum { LONG_EXT = 4096, PATH_BUF = 512, MSG_MAX = 4096, OOM_LIMIT = 400, FIXTURE_MAX = 1 << 16 };

static odin3_design *design;
static odin3_project *proj;
static odin3_strbuf text;
static char tmp[PATH_BUF / 2];
static char captured[MSG_MAX];
static char info[MSG_MAX];
static int errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    } else if (level == ODIN3_LOG_INFO) {
        (void)snprintf(info, sizeof info, "%s", msg);
    }
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    info[0] = '\0';
    errors = 0;
    odin3_strbuf_init(&text);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_record_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

static const char *in_tmp(const char *rel, char *buf) {
    (void)snprintf(buf, PATH_BUF, "%s/%s", tmp, rel);
    return buf;
}

void tearDown(void) {
    static const char *const files[] = {"p.f", "q.f", "lines.f", "empty.f", "big.f"};
    char path[PATH_BUF];
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        (void)unlink(in_tmp(files[i], path));
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_strbuf_free(&text);
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

static const char *str_of(uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

static odin3_srcman *srcman(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    return sm;
}

static void write_file(const char *rel, odin3_bytes content) {
    char path[PATH_BUF];
    FILE *fp = fopen(in_tmp(rel, path), "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(content.len, fwrite(content.ptr, 1, content.len, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* {given = rel, resolved = tmp/rel}. */
static odin3_proj_path tmp_path(const char *rel) {
    char path[PATH_BUF];
    return (odin3_proj_path){id(rel), id(in_tmp(rel, path)), {0}};
}

static odin3_proj_path abs_path(const char *given, const char *resolved) {
    return (odin3_proj_path){id(given), id(resolved), {0}};
}

/* A located word: offset 4 of line 2 of a registered project file "p.o3proj". */
static odin3_loc word_loc(void) {
    odin3_srcfile_spec spec = {.name = id("p.o3proj"), .resolved = id("/abs/p.o3proj"), .len = 20};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(srcman(), &spec, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(srcman(), buf, 10));
    return odin3_srcman_loc(srcman(), buf, 14);
}

static const char *dump(void) {
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &text));
    return text.data;
}

static void expect_error(odin3_status st, const char *message) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_STRING(message, captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
    errors = 0;
    captured[0] = '\0';
}

/* --- design ownership (PRJ-1) -------------------------------------------------------------- */

static void test_design_owns_and_drops_the_record(void) {
    odin3_project *again = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &again));
    TEST_ASSERT_EQUAL_PTR(proj, again);
    TEST_ASSERT_EQUAL_PTR(proj, odin3_design_project(design));
    TEST_ASSERT_EQUAL_PTR(design, proj->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_ignored(proj, odin3_bytes_cstr("X")));
    odin3_design_drop_project(design);
    TEST_ASSERT_NULL(odin3_design_project(design));
    odin3_design_drop_project(design); /* nothing to drop */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    TEST_ASSERT_EQUAL_size_t(0, proj->ignored.len);
    TEST_ASSERT_FALSE(proj->finished);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_ignored(proj, odin3_bytes_cstr("Y")));
    /* tearDown destroys the design with the record (ASan checks nothing leaks) */
}

static void test_get_project_out_of_memory(void) {
    odin3_design_drop_project(design);
    odin3_project *got = (odin3_project *)&got;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_design_get_project(design, &got));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_PTR((odin3_project *)&got, got);
    TEST_ASSERT_NULL(odin3_design_project(design));
}

static void test_diag_paths_setting(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_SRCFMT_GIVEN, odin3_design_diag_paths(design));
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_RESOLVED);
    TEST_ASSERT_EQUAL_INT(ODIN3_SRCFMT_RESOLVED, odin3_design_diag_paths(design));
    odin3_design_set_diag_paths(design, (odin3_srcfmt)7);
    TEST_ASSERT_EQUAL_INT(ODIN3_SRCFMT_GIVEN, odin3_design_diag_paths(design));
}

static void test_limits_hook(void) {
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROJ_MAX_FLIST_DEPTH, proj->limits.flist_depth);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROJ_MAX_QIP_DEPTH, proj->limits.qip_depth);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROJ_MAX_WORDS, proj->limits.words);
    odin3_project_test_set_limits(proj, (odin3_proj_limits){2, 3, 4});
    TEST_ASSERT_EQUAL_UINT32(2, proj->limits.flist_depth);
    TEST_ASSERT_EQUAL_UINT32(3, proj->limits.qip_depth);
    TEST_ASSERT_EQUAL_UINT32(4, proj->limits.words);
}

/* --- loading project files (PRJ-5) --------------------------------------------------------- */

static void expect_line(const odin3_proj_text *loaded, uint32_t index, const char *line,
                        uint32_t offset) {
    uint32_t got = UINT32_MAX;
    odin3_bytes view = odin3_proj_text_line(loaded, index, &got);
    TEST_ASSERT_EQUAL_size_t(strlen(line), view.len);
    if (view.len > 0) {
        TEST_ASSERT_EQUAL_MEMORY(line, view.ptr, view.len);
    }
    TEST_ASSERT_EQUAL_UINT32(offset, got);
}

/* a\rb\r\nc: lines "a\rb" and "c" (lone CR kept, CRLF folded); line 2 decodes at offset 5. */
static void test_load_lf_crlf_and_lone_cr(void) {
    write_file("p.f", (odin3_bytes){"a\rb\r\nc", 6});
    odin3_proj_path file = tmp_path("p.f");
    odin3_proj_text loaded;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_load(proj, &file, (odin3_loc){0}, &loaded));
    TEST_ASSERT_EQUAL_UINT32(6, loaded.len);
    TEST_ASSERT_EQUAL_size_t(2, loaded.lines.len);
    expect_line(&loaded, 0, "a\rb", 0);
    expect_line(&loaded, 1, "c", 5);
    odin3_srcpos pos;
    TEST_ASSERT_TRUE(
        odin3_srcman_decode(srcman(), odin3_srcman_loc(srcman(), loaded.buffer, 5), &pos));
    TEST_ASSERT_EQUAL_UINT32(2, pos.line);
    TEST_ASSERT_EQUAL_UINT32(1, pos.col);
    TEST_ASSERT_TRUE(
        odin3_srcman_decode(srcman(), odin3_srcman_loc(srcman(), loaded.buffer, 3), &pos));
    TEST_ASSERT_EQUAL_UINT32(1, pos.line);
    odin3_srcbuf_info buf;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(srcman(), loaded.buffer, &buf));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, buf.kind);
    TEST_ASSERT_EQUAL_STRING("p.f", str_of(buf.name));
    TEST_ASSERT_EQUAL_UINT32(file.resolved, buf.resolved);
    TEST_ASSERT_EQUAL_UINT32(0, buf.library);
    TEST_ASSERT_EQUAL_UINT32(0, buf.parent);
    TEST_ASSERT_EQUAL_UINT32(6, buf.len);
    TEST_ASSERT_EQUAL_UINT32(2, buf.lines);
    odin3_proj_text_free(&loaded);
    odin3_proj_text_free(&loaded);
}

/* Empty lines count; a final newline adds no line; an empty file has none. */
static void test_load_line_counts(void) {
    write_file("lines.f", (odin3_bytes){"x\n\ny\r\n", 6});
    odin3_proj_path file = tmp_path("lines.f");
    odin3_proj_text loaded;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_load(proj, &file, (odin3_loc){0}, &loaded));
    TEST_ASSERT_EQUAL_size_t(3, loaded.lines.len);
    expect_line(&loaded, 0, "x", 0);
    expect_line(&loaded, 1, "", 2);
    expect_line(&loaded, 2, "y", 3);
    odin3_srcbuf_info buf;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(srcman(), loaded.buffer, &buf));
    TEST_ASSERT_EQUAL_UINT32(4, buf.lines); /* the srcman also knows the empty line 4 at EOF */
    odin3_proj_text_free(&loaded);
    write_file("empty.f", (odin3_bytes){"", 0});
    file = tmp_path("empty.f");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_load(proj, &file, (odin3_loc){0}, &loaded));
    TEST_ASSERT_EQUAL_size_t(0, loaded.lines.len);
    TEST_ASSERT_EQUAL_UINT32(0, loaded.len);
    odin3_proj_text_free(&loaded);
}

/* A nested project file's buffer has the naming word as parent: its errors print the chain. */
static void test_load_nested_prints_chain(void) {
    write_file("p.f", (odin3_bytes){"a\n-f q.f\n", 9});
    write_file("q.f", (odin3_bytes){"x\n  bad\n", 8});
    odin3_proj_path outer = tmp_path("p.f");
    odin3_proj_path inner = tmp_path("q.f");
    odin3_proj_text t1;
    odin3_proj_text t2;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_load(proj, &outer, (odin3_loc){0}, &t1));
    odin3_loc word = odin3_srcman_loc(srcman(), t1.buffer, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_load(proj, &inner, word, &t2));
    odin3_srcbuf_info buf;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(srcman(), t2.buffer, &buf));
    TEST_ASSERT_EQUAL_UINT32(word.v, buf.parent);
    odin3_proj_err err = {.kind = ODIN3_PROJ_ERR_SYNTAX,
                          .scope = ODIN3_PROJ_LOCATED,
                          .at = odin3_srcman_loc(srcman(), t2.buffer, 4),
                          .detail = "bad thing"};
    expect_error(odin3_project_fail(proj, &err),
                 "q.f:2:3: error: syntax: bad thing\n  included from p.f:2");
    odin3_proj_text_free(&t1);
    odin3_proj_text_free(&t2);
}

/* One load on a fresh design with the k-th allocation failing. */
static bool load_try(long fail_after) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    odin3_proj_path file = tmp_path("lines.f");
    odin3_proj_text loaded;
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_project_load(proj, &file, (odin3_loc){0}, &loaded);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_size_t(4, loaded.lines.len); /* x, "", y, z */
    } else {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(loaded.bytes);
    }
    odin3_proj_text_free(&loaded);
    return st == ODIN3_OK;
}

static void test_load_out_of_memory(void) {
    write_file("lines.f", (odin3_bytes){"x\n\ny\r\nz", 7});
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = load_try(k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 3);
}

static void test_load_unreadable_and_too_big(void) {
    char path[PATH_BUF];
    char expected[MSG_MAX];
    odin3_proj_path file = tmp_path("none.f");
    odin3_proj_text loaded;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_project_load(proj, &file, (odin3_loc){0}, &loaded));
    TEST_ASSERT_EQUAL_STRING("odin3: error: cannot read 'none.f': No such file or directory",
                             captured);
    TEST_ASSERT_NULL(loaded.bytes);
    odin3_proj_text_free(&loaded);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_project_load(proj, &file, word_loc(), &loaded));
    TEST_ASSERT_EQUAL_STRING("p.o3proj:2:5: error: cannot read 'none.f': No such file or directory",
                             captured);
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_RESOLVED);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_project_load(proj, &file, (odin3_loc){0}, &loaded));
    (void)snprintf(expected, sizeof expected,
                   "odin3: error: cannot read '%s': No such file or directory",
                   in_tmp("none.f", path));
    TEST_ASSERT_EQUAL_STRING(expected, captured);
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_GIVEN);
    int fd = open(in_tmp("big.f", path), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, (off_t)ODIN3_SRC_MAX_FILE_BYTES + 1));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    file = tmp_path("big.f");
    errors = 0;
    expect_error(odin3_project_load(proj, &file, (odin3_loc){0}, &loaded),
                 "odin3: error: syntax: file 'big.f' is 268435457 bytes, over the limit of "
                 "268435456");
    odin3_proj_text_free(&loaded);
}

/* --- builders ------------------------------------------------------------------------------ */

static odin3_proj_file a_file(const char *rel, odin3_lang lang, const char *library) {
    return (odin3_proj_file){abs_path(rel, rel), (uint8_t)lang, library ? id(library) : 0};
}

/* The same resolved path in one library is a duplicate; in two libraries it is two entries. */
static void test_duplicates_in_one_and_two_libraries(void) {
    odin3_proj_file fa = a_file("/p/a.v", ODIN3_LANG_VERILOG, "work");
    odin3_proj_file fb = a_file("/p/b.v", ODIN3_LANG_VERILOG, "work");
    odin3_proj_file a2 = a_file("/p/a.v", ODIN3_LANG_SYSTEMVERILOG, "lib2");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &fa));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &fb));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &fa));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &a2));
    TEST_ASSERT_EQUAL_size_t(3, proj->files.len);
    TEST_ASSERT_EQUAL_size_t(1, proj->duplicates.len);
    const odin3_proj_file *third = odin3_vec_cat(&proj->files, 2);
    TEST_ASSERT_EQUAL_UINT32(id("lib2"), third->library);
    odin3_proj_file fl = a_file("/p/l.v", ODIN3_LANG_VERILOG, NULL);
    odin3_proj_file fl2 = a_file("/p/l.v", ODIN3_LANG_SYSTEMVERILOG, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_libfile(proj, &fl));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_libfile(proj, &fl2));
    TEST_ASSERT_EQUAL_size_t(1, proj->libfiles.len);
    TEST_ASSERT_EQUAL_size_t(2, proj->duplicates.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    TEST_ASSERT_TRUE(proj->finished);
    TEST_ASSERT_EQUAL_size_t(2, proj->duplicates.len);
    TEST_ASSERT_EQUAL_STRING("/p/a.v",
                             str_of(*(const uint32_t *)odin3_vec_cat(&proj->duplicates, 0)));
    TEST_ASSERT_EQUAL_STRING("/p/l.v",
                             str_of(*(const uint32_t *)odin3_vec_cat(&proj->duplicates, 1)));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static const odin3_proj_define *define_at(size_t idx) {
    return odin3_vec_cat(&proj->defines, idx);
}

/* `X=` has an empty value (ID 0, has_value); `X` none; the value is after the first '='. */
static void test_define_forms(void) {
    odin3_loc at = word_loc();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_define(proj, odin3_bytes_cstr("X="), at));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_define(proj, odin3_bytes_cstr("X"), at));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_add_define(proj, odin3_bytes_cstr("A$b=a=b"), at));
    TEST_ASSERT_EQUAL_size_t(3, proj->defines.len);
    TEST_ASSERT_TRUE(define_at(0)->has_value);
    TEST_ASSERT_EQUAL_UINT32(0, define_at(0)->value);
    TEST_ASSERT_EQUAL_STRING("X", str_of(define_at(0)->name));
    TEST_ASSERT_FALSE(define_at(1)->has_value);
    TEST_ASSERT_EQUAL_STRING("X", str_of(define_at(1)->name));
    TEST_ASSERT_EQUAL_STRING("A$b", str_of(define_at(2)->name));
    TEST_ASSERT_EQUAL_STRING("a=b", str_of(define_at(2)->value));
    TEST_ASSERT_EQUAL_UINT32(at.v, define_at(2)->at.v);
    expect_error(odin3_project_add_define(proj, odin3_bytes_cstr("1X=3"), at),
                 "p.o3proj:2:5: error: syntax: bad macro name: '1X=3'");
    expect_error(odin3_project_add_define(proj, odin3_bytes_cstr("=3"), at),
                 "p.o3proj:2:5: error: syntax: bad macro name: '=3'");
    TEST_ASSERT_EQUAL_size_t(3, proj->defines.len);
}

static void test_set_top(void) {
    odin3_loc at = word_loc();
    expect_error(odin3_project_set_top(proj, odin3_bytes_cstr("1a"), at),
                 "p.o3proj:2:5: error: syntax: bad top name: '1a'");
    expect_error(odin3_project_set_top(proj, odin3_bytes_cstr("a.b.c"), at),
                 "p.o3proj:2:5: error: syntax: bad top name: 'a.b.c'");
    expect_error(odin3_project_set_top(proj, odin3_bytes_cstr(""), at),
                 "p.o3proj:2:5: error: syntax: bad top name: ''");
    TEST_ASSERT_EQUAL_UINT32(0, proj->top);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_set_top(proj, odin3_bytes_cstr("a.b"), at));
    TEST_ASSERT_EQUAL_STRING("a.b", str_of(proj->top));
    TEST_ASSERT_EQUAL_UINT32(at.v, proj->top_at.v);
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_set_top(proj, odin3_bytes_cstr("a"), at));
    expect_error(odin3_project_set_top(proj, odin3_bytes_cstr("b"), at),
                 "p.o3proj:2:5: error: syntax: top given twice (first a): 'b'");
    TEST_ASSERT_EQUAL_STRING("a", str_of(proj->top));
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_project_set_top(proj, odin3_bytes_cstr("_x$1.$"), at));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_set_top(proj, odin3_bytes_cstr("_x$1.y$"), at));
    errors = 0;
}

/* A file-level top (a Quartus default) prints "<file>: error: …" in the design's path style. */
static void test_set_top_file_level(void) {
    odin3_proj_path qsf = abs_path("proj/x.qsf", "/abs/proj/x.qsf");
    expect_error(odin3_project_set_top_file(proj, odin3_bytes_cstr("1x"), &qsf),
                 "proj/x.qsf: error: syntax: bad top name: '1x'");
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_RESOLVED);
    expect_error(odin3_project_set_top_file(proj, odin3_bytes_cstr("1x"), &qsf),
                 "/abs/proj/x.qsf: error: syntax: bad top name: '1x'");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_set_top_file(proj, odin3_bytes_cstr("x"), &qsf));
    TEST_ASSERT_EQUAL_UINT32(0, proj->top_at.v);
    TEST_ASSERT_EQUAL_UINT32(qsf.resolved, proj->top_file.resolved);
    expect_error(odin3_project_set_top(proj, odin3_bytes_cstr("y"), word_loc()),
                 "p.o3proj:2:5: error: syntax: top given twice (first x): 'y'");
}

static void test_counts(void) {
    odin3_loc at = word_loc();
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_count(proj, odin3_bytes_cstr("4294967295"), at, &got));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_count(proj, odin3_bytes_cstr("007"), at, &got));
    TEST_ASSERT_EQUAL_UINT32(7, got);
    expect_error(odin3_project_count(proj, odin3_bytes_cstr("4294967296"), at, &got),
                 "p.o3proj:2:5: error: syntax: count too large: '4294967296'");
    expect_error(odin3_project_count(proj, odin3_bytes_cstr("99999999999999999999999"), at, &got),
                 "p.o3proj:2:5: error: syntax: count too large: '99999999999999999999999'");
    expect_error(odin3_project_count(proj, odin3_bytes_cstr("12a"), at, &got),
                 "p.o3proj:2:5: error: syntax: expected a count: '12a'");
    expect_error(odin3_project_count(proj, odin3_bytes_cstr("-1"), at, &got),
                 "p.o3proj:2:5: error: syntax: expected a count: '-1'");
    expect_error(odin3_project_count(proj, odin3_bytes_cstr(""), at, &got),
                 "p.o3proj:2:5: error: syntax: expected a count: ''");
    expect_error(odin3_project_count(proj, odin3_bytes_cstr("\xc2\xb2"), at, &got),
                 "p.o3proj:2:5: error: syntax: expected a count: '\xc2\xb2'");
    TEST_ASSERT_EQUAL_UINT32(7, got);
}

static void test_libext_checked(void) {
    odin3_loc at = word_loc();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_libext(proj, odin3_bytes_cstr(".sv"), at));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_libext(proj, odin3_bytes_cstr(".V"), at));
    expect_error(odin3_project_add_libext(proj, odin3_bytes_cstr(".foo"), at),
                 "p.o3proj:2:5: error: unknown_file_type: no language for extension: '.foo'");
    expect_error(odin3_project_add_libext(proj, odin3_bytes_cstr("v"), at),
                 "p.o3proj:2:5: error: unknown_file_type: no language for extension: 'v'");
    TEST_ASSERT_EQUAL_size_t(2, proj->libext.len);
}

/* A NUL byte in a word is a located syntax error before anything is interned; builders without
 * a location report it unlocated through odin3_project_intern. */
static void test_nul_bytes_rejected(void) {
    static const char define[] = "X=a\0b";
    static const char ext[] = ".\0.v";
    static const char ignored[] = "A\0B";
    odin3_loc at = word_loc();
    size_t strings = odin3_strtab_count(odin3_design_strtab(design));
    expect_error(odin3_project_add_define(proj, (odin3_bytes){define, sizeof define - 1}, at),
                 "p.o3proj:2:5: error: syntax: NUL byte in word: 'X=a\\0b'");
    expect_error(odin3_project_add_libext(proj, (odin3_bytes){ext, sizeof ext - 1}, at),
                 "p.o3proj:2:5: error: syntax: NUL byte in word: '.\\0.v'");
    expect_error(odin3_project_add_ignored(proj, (odin3_bytes){ignored, sizeof ignored - 1}),
                 "odin3: error: syntax: NUL byte in word: 'A\\0B'");
    uint32_t got = 0;
    expect_error(odin3_project_intern(proj, (odin3_bytes){ignored, sizeof ignored - 1}, &got),
                 "odin3: error: syntax: NUL byte in word: 'A\\0B'");
    TEST_ASSERT_EQUAL_size_t(strings, odin3_strtab_count(odin3_design_strtab(design)));
    TEST_ASSERT_EQUAL_size_t(0, proj->defines.len);
    TEST_ASSERT_EQUAL_size_t(0, proj->libext.len);
    TEST_ASSERT_EQUAL_size_t(0, proj->ignored.len);
}

/* A libext is checked as "x" + ext with no length limit. */
static void test_long_libext(void) {
    static char ext[LONG_EXT];
    memset(ext, 'a', sizeof ext - 1);
    memcpy(ext + sizeof ext - 3, ".v", 3);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_add_libext(proj, odin3_bytes_cstr(ext), (odin3_loc){0}));
    TEST_ASSERT_EQUAL_size_t(1, proj->libext.len);
}

/* --- errors -------------------------------------------------------------------------------- */

static void test_fail_forms(void) {
    odin3_bytes names[] = {odin3_bytes_cstr("files.f"), odin3_bytes_cstr("lists/more.f"),
                           odin3_bytes_cstr("files.f")};
    odin3_proj_err err = {.kind = ODIN3_PROJ_ERR_F_CYCLE,
                          .scope = ODIN3_PROJ_LOCATED,
                          .at = word_loc(),
                          .names = names,
                          .nnames = 3};
    expect_error(odin3_project_fail(proj, &err),
                 "p.o3proj:2:5: error: f_cycle: 'files.f', 'lists/more.f', 'files.f'");
    err.detail = "list includes itself";
    expect_error(odin3_project_fail(proj, &err), "p.o3proj:2:5: error: f_cycle: list includes "
                                                 "itself: 'files.f', 'lists/more.f', 'files.f'");
    err = (odin3_proj_err){.kind = ODIN3_PROJ_ERR_UNKNOWN_REVISION,
                           .scope = ODIN3_PROJ_UNLOCATED,
                           .names = names,
                           .nnames = 1};
    expect_error(odin3_project_fail(proj, &err), "odin3: error: unknown_revision: 'files.f'");
    odin3_proj_path qpf = abs_path("p.qpf", "/abs/p.qpf");
    err = (odin3_proj_err){.kind = ODIN3_PROJ_ERR_MISSING_FILE,
                           .scope = ODIN3_PROJ_FILE,
                           .file = &qpf,
                           .detail = "revision file not found"};
    expect_error(odin3_project_fail(proj, &err),
                 "p.qpf: error: missing_file: revision file not found");
    err = (odin3_proj_err){.kind = ODIN3_PROJ_ERR_SYNTAX, .scope = ODIN3_PROJ_UNLOCATED};
    expect_error(odin3_project_fail(proj, &err), "odin3: error: syntax");
}

static void test_error_kind_names(void) {
    static const char *const kinds[] = {
        "missing_file",
        "unknown_key",
        "unknown_option",
        "unknown_file_type",
        "unsupported_input_type",
        "syntax",
        "undefined_variable",
        "f_cycle",
        "qip_cycle",
        "ambiguous_revision",
        "unknown_revision",
        "include_not_found",
        "include_cycle",
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        TEST_ASSERT_EQUAL_STRING(kinds[i], odin3_proj_err_kind_name((odin3_proj_err_kind)(i + 1)));
    }
    TEST_ASSERT_EQUAL_STRING("?", odin3_proj_err_kind_name((odin3_proj_err_kind)0));
    TEST_ASSERT_EQUAL_STRING("?", odin3_proj_err_kind_name((odin3_proj_err_kind)14));
}

/* ignored and duplicates: sorted by bytes (Python's code-point order on UTF-8), unique. */
static void test_finish_sorts_and_uniques(void) {
    static const char *const names[] = {"b", "a", "B", "a", "ab", "\xc3\xa9", "", "b"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_project_add_ignored(proj, odin3_bytes_cstr(names[i])));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    static const char *const sorted[] = {"", "B", "a", "ab", "b", "\xc3\xa9"};
    TEST_ASSERT_EQUAL_size_t(sizeof sorted / sizeof sorted[0], proj->ignored.len);
    for (size_t i = 0; i < proj->ignored.len; i++) {
        TEST_ASSERT_EQUAL_STRING(sorted[i],
                                 str_of(*(const uint32_t *)odin3_vec_cat(&proj->ignored, i)));
    }
}

/* --- dump (PRJ-4) -------------------------------------------------------------------------- */

static const char k_empty[] = "{\n"
                              "  \"files\": [],\n"
                              "  \"libfiles\": [],\n"
                              "  \"libdirs\": [],\n"
                              "  \"libext\": [],\n"
                              "  \"incdirs\": [],\n"
                              "  \"defines\": [],\n"
                              "  \"top\": null,\n"
                              "  \"params\": [],\n"
                              "  \"arch\": [],\n"
                              "  \"rules\": [],\n"
                              "  \"inventory\": [],\n"
                              "  \"overrides\": [],\n"
                              "  \"flow\": null,\n"
                              "  \"output\": null,\n"
                              "  \"ignored\": [],\n"
                              "  \"duplicates\": []\n"
                              "}\n";

static void test_dump_empty_record(void) {
    TEST_ASSERT_EQUAL_STRING(k_empty, dump());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    TEST_ASSERT_EQUAL_STRING(k_empty, dump());
}

/* Generated with Python's json.dumps(value, ensure_ascii=False) per key. */
static const char k_full[] =
    "{\n"
    "  \"files\": [{\"path\": \"/p/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
    "{\"path\": \"/p/b.vhd\", \"language\": \"vhdl\", \"library\": \"lib2\"}],\n"
    "  \"libfiles\": [{\"path\": \"/p/l.v\", \"language\": \"verilog\"}],\n"
    "  \"libdirs\": [\"/p/lib\"],\n"
    "  \"libext\": [\".v\", \".sv\"],\n"
    "  \"incdirs\": [\"/p/inc\", \"/p/inc2\"],\n"
    "  \"defines\": [{\"name\": \"X\", \"value\": null}, {\"name\": \"Y\", \"value\": \"\"}, "
    "{\"name\": \"S\", \"value\": \"\\\"AB\\\"\"}],\n"
    "  \"top\": \"lib2.top\",\n"
    "  \"params\": [{\"scope\": \"top\", \"name\": \"W\", \"value\": \"8'hFF\", \"type\": "
    "\"number\"}, {\"scope\": \"top.u\", \"name\": \"S\", \"value\": \"hi\", \"type\": "
    "\"string\"}],\n"
    "  \"arch\": [{\"kind\": \"o3lib\", \"path\": \"/p/t.o3lib\"}, {\"kind\": \"vpr_xml\", "
    "\"path\": \"/p/k6.xml\"}, {\"kind\": \"device\", \"family\": \"Cyclone V\", \"device\": "
    "\"5CSEMA5\"}, {\"kind\": \"device\", \"family\": null, \"device\": \"X\"}],\n"
    "  \"rules\": [{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", \"action\": "
    "\"to\", \"cells\": [\"multiply\", \"mult_27\"], \"min_width\": 9}, {\"rule\": \"map\", "
    "\"scope\": \"top\", \"subject\": \"$mul\", \"action\": \"soft\", \"cells\": [], "
    "\"max_width\": 4, \"min_depth\": 2}, {\"rule\": \"map\", \"scope\": \"top.u_*\", "
    "\"subject\": \"$mem\", \"action\": \"keep\", \"cells\": []}, {\"rule\": \"split\", "
    "\"subject\": \"$mem\", \"width\": true, \"depth\": \"max\"}, {\"rule\": \"split\", "
    "\"subject\": \"$mem\", \"width\": false, \"depth\": \"8\"}, {\"rule\": \"split\", "
    "\"subject\": \"$x\", \"width\": true, \"depth\": null}, {\"rule\": \"split\", \"subject\": "
    "\"$y\", \"width\": false, \"depth\": \"min\"}, {\"rule\": \"limit\", \"cell\": "
    "\"multiply\", \"count\": 40}, {\"rule\": \"patterns\", \"path\": \"/p/pat.o3lib\"}],\n"
    "  \"inventory\": [{\"cell\": \"multiply\", \"count\": 112}],\n"
    "  \"overrides\": [{\"override\": \"hide\", \"cell\": \"dual\"}, {\"override\": \"cell\", "
    "\"name\": \"mult_27\", \"from\": \"multiply\", \"params\": {\"A_WIDTH\": \"27\", \"B\": "
    "\"\"}}, {\"override\": \"cell\", \"name\": \"c\", \"from\": \"d\", \"params\": {}}],\n"
    "  \"flow\": \"/p/flow.o3\",\n"
    "  \"output\": {\"path\": \"/p/out/x.blif\", \"format\": \"blif\"},\n"
    "  \"ignored\": [\"-hdl_version\", \"LOCATION\"],\n"
    "  \"duplicates\": [\"/p/a.v\"]\n"
    "}\n";

/* The builders below return statuses (no assertions) so the OOM sweep can reuse them. */

static odin3_status intern(const char *str, uint32_t *out) {
    return odin3_project_intern(proj, odin3_bytes_cstr(str), out);
}

static odin3_status make_path(const char *resolved, odin3_proj_path *out) {
    *out = (odin3_proj_path){0, 0, {0}};
    odin3_status st = intern(resolved + 3, &out->given);
    return st != ODIN3_OK ? st : intern(resolved, &out->resolved);
}

static odin3_status build_files(void) {
    static const char *const files[][3] = {
        {"/p/a.v", "work", "1"}, {"/p/b.vhd", "lib2", "3"}, {"/p/a.v", "work", "1"}};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < 3 && st == ODIN3_OK; i++) {
        odin3_proj_file file = {.lang = (uint8_t)(files[i][2][0] - '0')};
        st = make_path(files[i][0], &file.path);
        st = st != ODIN3_OK ? st : intern(files[i][1], &file.library);
        st = st != ODIN3_OK ? st : odin3_project_add_file(proj, &file);
    }
    odin3_proj_file lib = {.lang = ODIN3_LANG_VERILOG};
    st = st != ODIN3_OK ? st : make_path("/p/l.v", &lib.path);
    return st != ODIN3_OK ? st : odin3_project_add_libfile(proj, &lib);
}

static odin3_status build_dirs(void) {
    static const char *const dirs[] = {"/p/lib", "/p/inc", "/p/inc2"};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < 3 && st == ODIN3_OK; i++) {
        odin3_proj_path dir;
        odin3_proj_dir_list list = i == 0 ? ODIN3_PROJ_LIBDIRS : ODIN3_PROJ_INCDIRS;
        st = make_path(dirs[i], &dir);
        st = st != ODIN3_OK ? st : odin3_project_add_dir(proj, list, &dir);
    }
    odin3_loc none = {0};
    st = st != ODIN3_OK ? st : odin3_project_add_libext(proj, odin3_bytes_cstr(".v"), none);
    return st != ODIN3_OK ? st : odin3_project_add_libext(proj, odin3_bytes_cstr(".sv"), none);
}

static odin3_status build_defines_top_params(void) {
    static const char *const defines[] = {"X", "Y=", "S=\"AB\""};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < 3 && st == ODIN3_OK; i++) {
        st = odin3_project_add_define(proj, odin3_bytes_cstr(defines[i]), (odin3_loc){0});
    }
    st = st != ODIN3_OK ? st
                        : odin3_project_set_top(proj, odin3_bytes_cstr("lib2.top"), (odin3_loc){0});
    odin3_proj_param num = {.type = ODIN3_PARAM_NUMBER};
    odin3_proj_param str = {.type = ODIN3_PARAM_STRING};
    st = st != ODIN3_OK ? st : intern("top", &num.scope);
    st = st != ODIN3_OK ? st : intern("W", &num.name);
    st = st != ODIN3_OK ? st : intern("8'hFF", &num.value);
    st = st != ODIN3_OK ? st : intern("top.u", &str.scope);
    st = st != ODIN3_OK ? st : intern("S", &str.name);
    st = st != ODIN3_OK ? st : intern("hi", &str.value);
    st = st != ODIN3_OK ? st : odin3_project_add_param(proj, &num);
    return st != ODIN3_OK ? st : odin3_project_add_param(proj, &str);
}

static odin3_status build_arch(void) {
    odin3_proj_arch o3 = {.kind = ODIN3_PROJ_ARCH_O3LIB};
    odin3_proj_arch xml = {.kind = ODIN3_PROJ_ARCH_VPR_XML};
    odin3_proj_arch dev = {.kind = ODIN3_PROJ_ARCH_DEVICE, .has_family = true, .has_device = true};
    odin3_proj_arch part = {.kind = ODIN3_PROJ_ARCH_DEVICE, .has_device = true};
    odin3_status st = make_path("/p/t.o3lib", &o3.path);
    st = st != ODIN3_OK ? st : make_path("/p/k6.xml", &xml.path);
    st = st != ODIN3_OK ? st : intern("Cyclone V", &dev.family);
    st = st != ODIN3_OK ? st : intern("5CSEMA5", &dev.device);
    st = st != ODIN3_OK ? st : intern("X", &part.device);
    st = st != ODIN3_OK ? st : odin3_project_add_arch(proj, &o3);
    st = st != ODIN3_OK ? st : odin3_project_add_arch(proj, &xml);
    st = st != ODIN3_OK ? st : odin3_project_add_arch(proj, &dev);
    return st != ODIN3_OK ? st : odin3_project_add_arch(proj, &part);
}

static odin3_status add_map(const char *scope, uint8_t action, odin3_proj_ids cells,
                            const uint32_t *thresholds) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_MAP};
    rule.u.map.action = action;
    rule.u.map.has = (uint8_t)thresholds[0];
    rule.u.map.min_width = thresholds[1];
    rule.u.map.max_width = thresholds[2];
    rule.u.map.min_depth = thresholds[3];
    odin3_status st = intern(scope, &rule.u.map.scope);
    st = st != ODIN3_OK
             ? st
             : intern(action == ODIN3_PROJ_ACTION_KEEP ? "$mem" : "$mul", &rule.u.map.subject);
    return st != ODIN3_OK ? st : odin3_project_add_rule(proj, &rule, cells);
}

static odin3_status add_split(const char *subject, bool width, uint8_t depth_kind) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_SPLIT};
    rule.u.split.width = width;
    rule.u.split.depth_kind = depth_kind;
    odin3_status st = intern(subject, &rule.u.split.subject);
    st = st != ODIN3_OK || depth_kind != ODIN3_PROJ_DEPTH_N ? st : intern("8", &rule.u.split.depth);
    return st != ODIN3_OK ? st : odin3_project_add_rule(proj, &rule, (odin3_proj_ids){NULL, 0});
}

static odin3_status build_rules(void) {
    uint32_t cells[2];
    odin3_status st = intern("multiply", &cells[0]);
    st = st != ODIN3_OK ? st : intern("mult_27", &cells[1]);
    static const uint32_t to[] = {ODIN3_PROJ_HAS_MIN_WIDTH, 9, 0, 0};
    static const uint32_t soft[] = {ODIN3_PROJ_HAS_MAX_WIDTH | ODIN3_PROJ_HAS_MIN_DEPTH, 0, 4, 2};
    static const uint32_t none[] = {0, 0, 0, 0};
    st = st != ODIN3_OK ? st : add_map("*", ODIN3_PROJ_ACTION_TO, (odin3_proj_ids){cells, 2}, to);
    st = st != ODIN3_OK ? st
                        : add_map("top", ODIN3_PROJ_ACTION_SOFT, (odin3_proj_ids){NULL, 0}, soft);
    st = st != ODIN3_OK
             ? st
             : add_map("top.u_*", ODIN3_PROJ_ACTION_KEEP, (odin3_proj_ids){NULL, 0}, none);
    st = st != ODIN3_OK ? st : add_split("$mem", true, ODIN3_PROJ_DEPTH_MAX);
    st = st != ODIN3_OK ? st : add_split("$mem", false, ODIN3_PROJ_DEPTH_N);
    st = st != ODIN3_OK ? st : add_split("$x", true, ODIN3_PROJ_DEPTH_NONE);
    st = st != ODIN3_OK ? st : add_split("$y", false, ODIN3_PROJ_DEPTH_MIN);
    odin3_proj_rule limit = {.kind = ODIN3_PROJ_RULE_LIMIT};
    limit.u.limit.cell = cells[0];
    limit.u.limit.count = 40;
    odin3_proj_rule pat = {.kind = ODIN3_PROJ_RULE_PATTERNS};
    st = st != ODIN3_OK ? st : odin3_project_add_rule(proj, &limit, (odin3_proj_ids){NULL, 0});
    st = st != ODIN3_OK ? st : make_path("/p/pat.o3lib", &pat.u.patterns.path);
    return st != ODIN3_OK ? st : odin3_project_add_rule(proj, &pat, (odin3_proj_ids){NULL, 0});
}

static odin3_status build_overrides(void) {
    odin3_proj_inventory inv = {.count = 112};
    odin3_proj_override hide = {.kind = ODIN3_PROJ_OVERRIDE_HIDE};
    odin3_proj_override cell = {.kind = ODIN3_PROJ_OVERRIDE_CELL};
    odin3_proj_override bare = {.kind = ODIN3_PROJ_OVERRIDE_CELL};
    odin3_proj_kv kvs[2] = {{0, 0}, {0, 0}};
    odin3_status st = intern("multiply", &inv.cell);
    st = st != ODIN3_OK ? st : odin3_project_add_inventory(proj, &inv);
    st = st != ODIN3_OK ? st : intern("dual", &hide.name);
    st = st != ODIN3_OK ? st : intern("mult_27", &cell.name);
    st = st != ODIN3_OK ? st : intern("multiply", &cell.from);
    st = st != ODIN3_OK ? st : intern("A_WIDTH", &kvs[0].key);
    st = st != ODIN3_OK ? st : intern("27", &kvs[0].value);
    st = st != ODIN3_OK ? st : intern("B", &kvs[1].key);
    st = st != ODIN3_OK ? st : intern("c", &bare.name);
    st = st != ODIN3_OK ? st : intern("d", &bare.from);
    st = st != ODIN3_OK ? st : odin3_project_add_override(proj, &hide, (odin3_proj_kvs){NULL, 0});
    st = st != ODIN3_OK ? st : odin3_project_add_override(proj, &cell, (odin3_proj_kvs){kvs, 2});
    return st != ODIN3_OK ? st : odin3_project_add_override(proj, &bare, (odin3_proj_kvs){NULL, 0});
}

static odin3_status build_tail(void) {
    odin3_status st = make_path("/p/flow.o3", &proj->flow);
    st = st != ODIN3_OK ? st : intern("/p/out/x.blif", &proj->output.path);
    st = st != ODIN3_OK ? st : intern("blif", &proj->output.format);
    proj->output.present = true;
    proj->output.has_format = true;
    st = st != ODIN3_OK ? st : odin3_project_add_ignored(proj, odin3_bytes_cstr("LOCATION"));
    st = st != ODIN3_OK ? st : odin3_project_add_ignored(proj, odin3_bytes_cstr("-hdl_version"));
    return st != ODIN3_OK ? st : odin3_project_add_ignored(proj, odin3_bytes_cstr("LOCATION"));
}

static odin3_status build_full(void) {
    odin3_status st = build_files();
    st = st != ODIN3_OK ? st : build_dirs();
    st = st != ODIN3_OK ? st : build_defines_top_params();
    st = st != ODIN3_OK ? st : build_arch();
    st = st != ODIN3_OK ? st : build_rules();
    st = st != ODIN3_OK ? st : build_overrides();
    st = st != ODIN3_OK ? st : build_tail();
    return st != ODIN3_OK ? st : odin3_project_finish(proj);
}

static void test_dump_every_field(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_full());
    TEST_ASSERT_EQUAL_STRING(k_full, dump());
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* JSON escapes as Python writes them; each byte outside valid UTF-8 as \u00XX. */
static void test_dump_escapes(void) {
    static const char word[] = "E=q\"b\\\n\t\x01\x1f\b\f\r\x7f\xc3\xa9\xf0\x9f\x99\x82"
                               "\xff\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82\xc3";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_add_define(proj, odin3_bytes_cstr(word), (odin3_loc){0}));
    const char *got = strstr(dump(), "\"defines\": ");
    TEST_ASSERT_NOT_NULL(got);
    static const char expected[] =
        "\"defines\": [{\"name\": \"E\", \"value\": \"q\\\"b\\\\\\n\\t\\u0001\\u001f\\b\\f\\r"
        "\x7f\xc3\xa9\xf0\x9f\x99\x82\\u00ff\\u00c0\\u00af\\u00ed\\u00a0\\u0080\\u00f4\\u0090"
        "\\u0080\\u0080\\u00e2\\u0082\\u00c3\"}],\n";
    TEST_ASSERT_EQUAL_STRING_LEN(expected, got, strlen(expected));
}

/* --- the corpus: single_file's record ------------------------------------------------------ */

/* The "project" object of a case's expected.json, re-indented as a top-level dump. */
static void corpus_project(const char *case_dir, odin3_strbuf *out) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/expected.json", case_dir);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(path, FIXTURE_MAX, &bytes));
    odin3_strbuf json; /* NUL-terminated: odin3_file_read_all's bytes are not */
    odin3_strbuf_init(&json);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&json, (odin3_bytes){bytes.data, bytes.len}));
    odin3_file_bytes_free(&bytes);
    const char *open = strstr(json.data, "\n  \"project\": {\n");
    TEST_ASSERT_NOT_NULL(open);
    const char *at = open + strlen("\n  \"project\": {\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("{\n")));
    while (strncmp(at, "  }", 3) != 0) {
        const char *end = strchr(at, '\n');
        TEST_ASSERT_NOT_NULL(end);
        TEST_ASSERT_EQUAL_MEMORY("  ", at, 2);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at + 2, (size_t)(end - at) - 1}));
        at = end + 1;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("}\n")));
    odin3_strbuf_free(&json);
}

/* Every "<case_dir>/" in text removed (the harness's relativizing, for this case). */
static void relativize(const char *case_dir, odin3_strbuf *out) {
    char prefix[PATH_BUF];
    (void)snprintf(prefix, sizeof prefix, "%s/", case_dir);
    const char *at = text.data;
    for (const char *hit = strstr(at, prefix); hit != NULL; hit = strstr(at, prefix)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_append(out, (odin3_bytes){at, (size_t)(hit - at)}));
        at = hit + strlen(prefix);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(at)));
}

static void test_dump_single_file_matches_corpus(void) {
    odin3_strbuf dir;
    odin3_strbuf_init(&dir);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_path_normalize(odin3_bytes_cstr(ODIN3_PROJECT_FIXTURES "/single_file"), &dir));
    odin3_proj_path_req req = {{dir.data, dir.len},
                               odin3_bytes_cstr("src/counter.v"),
                               (odin3_loc){0},
                               ODIN3_PROJ_WANT_FILE};
    odin3_proj_file file = {.lang = ODIN3_LANG_VERILOG, .library = id("work")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_path(proj, &req, &file.path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &file));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    (void)dump();
    odin3_strbuf got;
    odin3_strbuf want;
    odin3_strbuf_init(&got);
    odin3_strbuf_init(&want);
    relativize(dir.data, &got);
    corpus_project(dir.data, &want);
    TEST_ASSERT_EQUAL_STRING(want.data, got.data);
    odin3_strbuf_free(&got);
    odin3_strbuf_free(&want);
    odin3_strbuf_free(&dir);
}

/* --- out of memory ------------------------------------------------------------------------- */

/* One try on a fresh design with the k-th allocation failing: build, finish and dump; on
 * failure the record is dropped. True when everything succeeded. */
static bool oom_try(long fail_after) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_strbuf_free(&text);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_design_get_project(design, &proj);
    st = st != ODIN3_OK ? st : build_full();
    st = st != ODIN3_OK ? st : odin3_project_dump(proj, &text);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_STRING(k_full, text.data);
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_size_t(0, text.len);
    odin3_design_drop_project(design);
    TEST_ASSERT_NULL(odin3_design_project(design));
    return false;
}

static void test_out_of_memory_drops_the_record(void) {
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 20); /* the sweep really hit many allocation points */
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --- builder atomicity under NO_MEMORY ------------------------------------------------------ */

enum { NVECS = 8 };

static void vec_lengths(size_t lens[NVECS]) {
    const odin3_vec *vecs[NVECS] = {&proj->files,           &proj->libfiles,   &proj->duplicates,
                                    &proj->rules,           &proj->rule_cells, &proj->overrides,
                                    &proj->override_params, &proj->defines};
    for (size_t i = 0; i < NVECS; i++) {
        lens[i] = vecs[i]->len;
    }
}

/* One builder call with its fail_after-th allocation failing. */
typedef struct oom_case {
    int which; /* 0 add_file, 1 add_libfile, 2 add_rule, 3 add_override, 4 add_define */
    long fail_after;
} oom_case;

/* Calls the builder (every argument interned before the failure window); returns its status. */
static odin3_status one_builder(oom_case c_case) {
    uint32_t cells[2] = {id("c1"), id("c2")};
    odin3_proj_kv kvs[1] = {{id("k"), id("v")}};
    odin3_proj_file file = a_file("/p/a.v", ODIN3_LANG_VERILOG, "work");
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_MAP};
    odin3_proj_override ov = {.kind = ODIN3_PROJ_OVERRIDE_CELL, .name = id("n"), .from = id("f")};
    odin3_util_set_alloc_fail_after(c_case.fail_after);
    odin3_status st = ODIN3_OK;
    if (c_case.which == 0) {
        st = odin3_project_add_file(proj, &file);
    } else if (c_case.which == 1) {
        st = odin3_project_add_libfile(proj, &file);
    } else if (c_case.which == 2) {
        st = odin3_project_add_rule(proj, &rule, (odin3_proj_ids){cells, 2});
    } else if (c_case.which == 3) {
        st = odin3_project_add_override(proj, &ov, (odin3_proj_kvs){kvs, 1});
    } else {
        st = odin3_project_add_define(proj, odin3_bytes_cstr("NEWNAME=newvalue"), (odin3_loc){0});
    }
    odin3_util_set_alloc_fail_after(-1);
    return st;
}

/* One try on a fresh record: true when the builder succeeded; on NO_MEMORY every list keeps
 * its length. */
static bool builder_try(oom_case c_case) {
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    size_t before[NVECS];
    size_t after[NVECS];
    vec_lengths(before);
    odin3_status st = one_builder(c_case);
    vec_lengths(after);
    if (st == ODIN3_OK) {
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_MEMORY(before, after, sizeof before);
    return false;
}

/* Sweeps one builder: it fails at least once, every failure leaves the lists unchanged, and it
 * then succeeds. */
static void sweep_builder(int which) {
    long k = 0;
    while (k < OOM_LIMIT && !builder_try((oom_case){which, k})) {
        k++;
    }
    TEST_ASSERT_TRUE(k > 0);         /* an allocation point was really hit */
    TEST_ASSERT_TRUE(k < OOM_LIMIT); /* and the builder then succeeded */
}

/* Every builder leaves every list unchanged when an allocation fails (reserve-before-mutate). */
static void test_builders_unchanged_on_no_memory(void) {
    for (int which = 0; which < 5; which++) {
        sweep_builder(which);
    }
}

/* The dump branches the full record does not take: a file without a library (key omitted) and
 * an output without a format. */
static void test_dump_rare_branches(void) {
    odin3_proj_file file = a_file("/p/x.blif", ODIN3_LANG_BLIF, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_file(proj, &file));
    proj->output.present = true;
    proj->output.path = id("/p/o");
    const char *got = dump();
    TEST_ASSERT_NOT_NULL(
        strstr(got, "  \"files\": [{\"path\": \"/p/x.blif\", \"language\": \"blif\"}],\n"));
    TEST_ASSERT_NOT_NULL(strstr(got, "  \"output\": {\"path\": \"/p/o\", \"format\": null},\n"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_design_owns_and_drops_the_record);
    RUN_TEST(test_get_project_out_of_memory);
    RUN_TEST(test_diag_paths_setting);
    RUN_TEST(test_limits_hook);
    RUN_TEST(test_load_lf_crlf_and_lone_cr);
    RUN_TEST(test_load_line_counts);
    RUN_TEST(test_load_nested_prints_chain);
    RUN_TEST(test_load_out_of_memory);
    RUN_TEST(test_load_unreadable_and_too_big);
    RUN_TEST(test_duplicates_in_one_and_two_libraries);
    RUN_TEST(test_define_forms);
    RUN_TEST(test_set_top);
    RUN_TEST(test_set_top_file_level);
    RUN_TEST(test_counts);
    RUN_TEST(test_libext_checked);
    RUN_TEST(test_nul_bytes_rejected);
    RUN_TEST(test_long_libext);
    RUN_TEST(test_fail_forms);
    RUN_TEST(test_error_kind_names);
    RUN_TEST(test_finish_sorts_and_uniques);
    RUN_TEST(test_dump_empty_record);
    RUN_TEST(test_dump_every_field);
    RUN_TEST(test_dump_escapes);
    RUN_TEST(test_dump_rare_branches);
    RUN_TEST(test_builders_unchanged_on_no_memory);
    RUN_TEST(test_dump_single_file_matches_corpus);
    RUN_TEST(test_out_of_memory_drops_the_record);
    return UNITY_END();
}
