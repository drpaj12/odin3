/*
 * test_project_paths.c — unit tests for lexical path resolution (PRJ-6): normalization like
 * os.path.normpath, resolution, stat, odin3_project_path's missing_file, extension tables, OOM.
 */
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "ir/design.h"
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

enum { LONG_EXT = 4096, PATH_BUF = 512, MSG_MAX = 2048, OOM_LIMIT = 64, DIR_MODE = 0700 };

static odin3_design *design;
static odin3_project *proj;
static odin3_strbuf out;
static char tmp[PATH_BUF / 2];
static char captured[MSG_MAX];
static int errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

/* tmp/<rel> as a path. */
static const char *in_tmp(const char *rel, char *buf) {
    (void)snprintf(buf, PATH_BUF, "%s/%s", tmp, rel);
    return buf;
}

static void make_file(const char *rel) {
    char path[PATH_BUF];
    FILE *fp = fopen(in_tmp(rel, path), "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    errors = 0;
    odin3_strbuf_init(&out);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_paths_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_path_normalize(odin3_bytes_cstr(tmp), &out));
    (void)snprintf(tmp, sizeof tmp, "%s", out.data); /* resolved paths are normalized */
    char path[PATH_BUF];
    char target[PATH_BUF];
    TEST_ASSERT_EQUAL_INT(0, mkdir(in_tmp("real", path), DIR_MODE));
    TEST_ASSERT_EQUAL_INT(0, mkdir(in_tmp("real/sub", path), DIR_MODE));
    make_file("real/f.v");
    make_file("top.v");
    TEST_ASSERT_EQUAL_INT(0, symlink(in_tmp("real/sub", target), in_tmp("link", path)));
    TEST_ASSERT_EQUAL_INT(0, symlink(in_tmp("real/f.v", target), in_tmp("flink.v", path)));
    TEST_ASSERT_EQUAL_INT(0, symlink(in_tmp("nowhere", target), in_tmp("dangling", path)));
}

void tearDown(void) {
    static const char *const files[] = {"dangling", "flink.v", "link", "top.v", "real/f.v"};
    static const char *const dirs[] = {"real/sub", "real"};
    char path[PATH_BUF];
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        (void)unlink(in_tmp(files[i], path));
    }
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        (void)rmdir(in_tmp(dirs[i], path));
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_strbuf_free(&out);
    odin3_design_destroy(design);
}

static const char *normalized(const char *path) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_path_normalize(odin3_bytes_cstr(path), &out));
    TEST_ASSERT_EQUAL_size_t(strlen(out.data), out.len);
    return out.data;
}

static const char *resolved(const char *base, const char *given) {
    odin3_path_join join = {odin3_bytes_cstr(base), odin3_bytes_cstr(given)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_path_resolve(join, &out));
    return out.data;
}

/* Every expected value below is Python 3.12's posixpath.normpath of the input. */
static void test_normalize_like_normpath(void) {
    static const char *const cases[][2] = {
        {"a/./b", "a/b"},
        {"a/../../b", "../b"},
        {"/r/a/../../b", "/b"},
        {"/..//x", "/x"},
        {"/a//b", "/a/b"},
        {"//a/b", "//a/b"},
        {"///a", "/a"},
        {"", "."},
        {".", "."},
        {"a/..", "."},
        {"../..", "../.."},
        {"/", "/"},
        {"//", "//"},
        {"a/b/", "a/b"},
        {"./a", "a"},
        {"//..", "//"},
        {"a/b/../..", "."},
        {"../a/../b", "../b"},
        {"/./", "/"},
        {"a", "a"},
        {"a\rb/./c", "a\rb/c"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i][1], normalized(cases[i][0]), cases[i][0]);
    }
}

static void test_normalize_replaces_contents(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&out, odin3_bytes_cstr("junk")));
    TEST_ASSERT_EQUAL_STRING("x", normalized("x/"));
}

static void test_resolve(void) {
    TEST_ASSERT_EQUAL_STRING("/p/src/a.v", resolved("/p", "src/a.v"));
    TEST_ASSERT_EQUAL_STRING("/p/a.v", resolved("/p/q/", "../a.v"));
    TEST_ASSERT_EQUAL_STRING("/x/y", resolved("/p", "/x/./y")); /* absolute given stands */
    TEST_ASSERT_EQUAL_STRING("/b", resolved("/", "a/../../b"));
    TEST_ASSERT_EQUAL_STRING("a", resolved("", "./a"));
    TEST_ASSERT_EQUAL_STRING("//x", resolved("/p", "//x"));
}

static void test_path_dir(void) {
    static const char *const cases[][2] = {
        {"/a/b/c.f", "/a/b"}, {"/c.f", "/"}, {"c.f", ""}, {"a/b", "a"}, {"//x", "//"},
        {"///x", "///"},      {"a//b", "a"}, {"a/", "a"}, {"/", "/"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        odin3_bytes dir = odin3_path_dir(odin3_bytes_cstr(cases[i][0]));
        TEST_ASSERT_EQUAL_size_t(strlen(cases[i][1]), dir.len);
        if (dir.len > 0) {
            TEST_ASSERT_EQUAL_MEMORY(cases[i][1], dir.ptr, dir.len);
        }
    }
}

static void test_stat_follows_symlinks(void) {
    char path[PATH_BUF];
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_FILE, odin3_path_stat(in_tmp("top.v", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_DIR, odin3_path_stat(in_tmp("real", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_DIR, odin3_path_stat(in_tmp("link", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_FILE, odin3_path_stat(in_tmp("flink.v", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_NONE, odin3_path_stat(in_tmp("dangling", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_NONE, odin3_path_stat(in_tmp("nothing", path)));
    TEST_ASSERT_EQUAL_INT(ODIN3_PATH_NONE, odin3_path_stat("/dev/null"));
}

/* --- odin3_project_path ------------------------------------------------------------------- */

/* A located word: offset 4 of line 2 of a registered project file "p.o3proj". */
static odin3_loc word_loc(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_intern(proj, odin3_bytes_cstr("p.o3proj"), &name));
    odin3_srcfile_spec spec = {.name = name, .len = 20};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, 10));
    return odin3_srcman_loc(sm, buf, 14);
}

static odin3_status path_of(const char *base, const char *given, odin3_proj_want want,
                            odin3_proj_path *got) {
    odin3_proj_path_req req = {odin3_bytes_cstr(base), odin3_bytes_cstr(given), word_loc(), want};
    return odin3_project_path(proj, &req, got);
}

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
}

static void test_project_path_found(void) {
    odin3_proj_path got;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "./real/../top.v", ODIN3_PROJ_WANT_FILE, &got));
    char path[PATH_BUF];
    TEST_ASSERT_EQUAL_STRING("./real/../top.v", str_of(got.given));
    TEST_ASSERT_EQUAL_STRING(in_tmp("top.v", path), str_of(got.resolved));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, got.at.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "real", ODIN3_PROJ_WANT_DIR, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "real", ODIN3_PROJ_WANT_ANY, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "top.v", ODIN3_PROJ_WANT_ANY, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of("/elsewhere", path, ODIN3_PROJ_WANT_FILE, &got));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* A symlinked directory is never resolved: link/f.v stays under link, and link/.. folds
 * lexically to tmp (the target's parent would be real/). */
static void test_symlinked_directory_not_resolved(void) {
    char path[PATH_BUF];
    odin3_proj_path got;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "link/../top.v", ODIN3_PROJ_WANT_FILE, &got));
    TEST_ASSERT_EQUAL_STRING(in_tmp("top.v", path), str_of(got.resolved));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(in_tmp("link", path), ".", ODIN3_PROJ_WANT_DIR, &got));
    TEST_ASSERT_EQUAL_STRING(in_tmp("link", path), str_of(got.resolved));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, path_of(tmp, "flink.v", ODIN3_PROJ_WANT_FILE, &got));
    TEST_ASSERT_EQUAL_STRING(in_tmp("flink.v", path), str_of(got.resolved));
}

static void expect_missing(const char *given, odin3_proj_want want, const char *what,
                           const char *rel) {
    errors = 0;
    odin3_proj_path got = {7, 7, {7}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, path_of(tmp, given, want, &got));
    TEST_ASSERT_EQUAL_UINT32(7, got.given); /* untouched */
    char path[PATH_BUF];
    char expected[MSG_MAX];
    (void)snprintf(expected, sizeof expected, "p.o3proj:2:5: error: missing_file: %s: '%s'", what,
                   in_tmp(rel, path));
    TEST_ASSERT_EQUAL_STRING(expected, captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
}

static void test_missing_file_and_directory(void) {
    expect_missing("src/../nothing.v", ODIN3_PROJ_WANT_FILE, "file not found", "nothing.v");
    expect_missing("nodir", ODIN3_PROJ_WANT_DIR, "directory not found", "nodir");
    expect_missing("dangling", ODIN3_PROJ_WANT_ANY, "not found", "dangling");
    expect_missing("real", ODIN3_PROJ_WANT_FILE, "file not found", "real");
    expect_missing("top.v", ODIN3_PROJ_WANT_DIR, "directory not found", "top.v");
}

/* A NUL byte in a path word is a located syntax error before anything is interned, even when
 * normalization would fold it away. */
static void test_nul_in_given_is_syntax(void) {
    static const char given[] = "a\0/../top.v";
    odin3_proj_path_req req = {
        odin3_bytes_cstr(tmp), {given, sizeof given - 1}, word_loc(), ODIN3_PROJ_WANT_FILE};
    odin3_proj_path got = {7, 7, {7}};
    size_t strings = odin3_strtab_count(odin3_design_strtab(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_project_path(proj, &req, &got));
    TEST_ASSERT_EQUAL_STRING("p.o3proj:2:5: error: syntax: NUL byte in word: 'a\\0/../top.v'",
                             captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT32(7, got.given);
    TEST_ASSERT_EQUAL_size_t(strings, odin3_strtab_count(odin3_design_strtab(design)));
}

/* --- extension tables ---------------------------------------------------------------------- */

static void expect_lang(const char *path, int lang) {
    odin3_lang got = (odin3_lang)0;
    bool ok = odin3_lang_of(odin3_bytes_cstr(path), &got);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lang != 0, ok, path);
    if (ok) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(lang, got, path);
    }
}

static void test_language_table(void) {
    expect_lang("a.v", ODIN3_LANG_VERILOG);
    expect_lang("inc/a.vh", ODIN3_LANG_VERILOG);
    expect_lang("x.V", ODIN3_LANG_VERILOG);
    expect_lang("a.sv", ODIN3_LANG_SYSTEMVERILOG);
    expect_lang("X.SV", ODIN3_LANG_SYSTEMVERILOG);
    expect_lang("a.svh", ODIN3_LANG_SYSTEMVERILOG);
    expect_lang("a.vhd", ODIN3_LANG_VHDL);
    expect_lang("a.VHDL", ODIN3_LANG_VHDL);
    expect_lang("n.blif", ODIN3_LANG_BLIF);
    expect_lang("n.vqm", ODIN3_LANG_VQM);
    expect_lang("n.edf", ODIN3_LANG_EDIF);
    expect_lang("n.edif", ODIN3_LANG_EDIF);
    expect_lang("a.b.v", ODIN3_LANG_VERILOG);
    expect_lang(".x.v", ODIN3_LANG_VERILOG);
    expect_lang("x.v/", ODIN3_LANG_VERILOG);
    expect_lang("x.v/.", ODIN3_LANG_VERILOG); /* pathlib drops "." components */
    expect_lang("x.v/./", ODIN3_LANG_VERILOG);
    expect_lang("a/.v/.", 0);
    expect_lang("x.foo", 0);
    expect_lang(".v", 0);
    expect_lang("x/.v", 0);
    expect_lang("a.", 0);
    expect_lang("dir.v/x", 0);
    expect_lang("v", 0);
    expect_lang("", 0);
}

static void expect_lang_name(odin3_lang lang, const char *name) {
    odin3_lang got = (odin3_lang)0;
    TEST_ASSERT_TRUE(odin3_lang_by_name(odin3_bytes_cstr(name), &got));
    TEST_ASSERT_EQUAL_INT(lang, got);
    TEST_ASSERT_EQUAL_STRING(name, odin3_lang_name(lang));
}

static void test_language_names(void) {
    expect_lang_name(ODIN3_LANG_VERILOG, "verilog");
    expect_lang_name(ODIN3_LANG_SYSTEMVERILOG, "systemverilog");
    expect_lang_name(ODIN3_LANG_VHDL, "vhdl");
    expect_lang_name(ODIN3_LANG_BLIF, "blif");
    expect_lang_name(ODIN3_LANG_VQM, "vqm");
    expect_lang_name(ODIN3_LANG_EDIF, "edif");
}

/* A libext is looked up as "x" + ext (§4.1), without a length limit. */
static void expect_ext(const char *ext, int lang) {
    odin3_lang got = (odin3_lang)0;
    bool ok = odin3_lang_of_ext(odin3_bytes_cstr(ext), &got);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lang != 0, ok, ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lang, ok ? got : 0, ext);
}

static void test_libext_languages(void) {
    static char long_ext[LONG_EXT];
    memset(long_ext, 'a', sizeof long_ext - 1);
    memcpy(long_ext + sizeof long_ext - 4, ".sv", 4);
    expect_ext(".v", ODIN3_LANG_VERILOG);
    expect_ext(".SV", ODIN3_LANG_SYSTEMVERILOG);
    expect_ext(".v/", ODIN3_LANG_VERILOG);
    expect_ext("b.vhd", ODIN3_LANG_VHDL);
    expect_ext(long_ext, ODIN3_LANG_SYSTEMVERILOG);
    expect_ext("v", 0);
    expect_ext("/.v", 0);
    expect_ext("a/.v", 0);
    expect_ext(".", 0);
    expect_ext("", 0);
}

static void test_language_names_unknown(void) {
    odin3_lang got = (odin3_lang)0;
    TEST_ASSERT_FALSE(odin3_lang_by_name(odin3_bytes_cstr("Verilog"), &got));
    TEST_ASSERT_FALSE(odin3_lang_by_name(odin3_bytes_cstr("verilo"), &got));
    TEST_ASSERT_NULL(odin3_lang_name((odin3_lang)0));
}

/* kind 0: no arch kind. */
static void expect_arch(const char *path, int kind) {
    uint8_t got = 0;
    bool ok = odin3_arch_kind_of(odin3_bytes_cstr(path), &got);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kind != 0, ok, path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kind, got, path);
}

static void test_arch_kinds(void) {
    expect_arch("arch/k6.XML", ODIN3_PROJ_ARCH_VPR_XML);
    expect_arch("t.o3lib", ODIN3_PROJ_ARCH_O3LIB);
    expect_arch("t.O3Lib", ODIN3_PROJ_ARCH_O3LIB);
    expect_arch("t.xmlx", 0);
    expect_arch("t.v", 0);
}

/* Python's Path(p).suffix, case kept (the import and entry tests are case-sensitive). */
static void expect_suffix(const char *path, const char *suffix) {
    odin3_bytes got = odin3_path_suffix(odin3_bytes_cstr(path));
    char text[64];
    TEST_ASSERT_TRUE(got.len < sizeof text);
    (void)snprintf(text, sizeof text, "%.*s", (int)got.len, (const char *)got.ptr);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(suffix, text, path);
}

static void test_path_suffix(void) {
    expect_suffix("/a/b.qsf", ".qsf");
    expect_suffix("b.QPF", ".QPF");
    expect_suffix("a.b/c", "");
    expect_suffix("a.tar.gz", ".gz");
    expect_suffix(".qsf", "");
    expect_suffix("x.", "");
    expect_suffix("x.qsf/", ".qsf");
    expect_suffix("x.qsf/.", ".qsf");
    expect_suffix("", "");
    expect_suffix("/", "");
    expect_suffix("..", "");
    expect_suffix("a/..qsf", ".qsf");
}

/* --- out of memory ------------------------------------------------------------------------- */

/* One try of odin3_project_path on a fresh design with the k-th allocation failing. */
static bool path_try(long fail_after) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    odin3_proj_path_req req = {odin3_bytes_cstr(tmp), odin3_bytes_cstr("real/./f.v"), word_loc(),
                               ODIN3_PROJ_WANT_FILE};
    odin3_proj_path got = {0, 0, {0}};
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_project_path(proj, &req, &got);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        return false;
    }
    char path[PATH_BUF];
    TEST_ASSERT_EQUAL_STRING(in_tmp("real/f.v", path), str_of(got.resolved));
    return true;
}

static void test_out_of_memory(void) {
    bool succeeded = false;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = path_try(k);
    }
    TEST_ASSERT_TRUE(succeeded);
}

static void test_normalize_out_of_memory(void) {
    for (long k = 0; k < 2; k++) {
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_path_normalize(odin3_bytes_cstr("a/../b/c"), &out);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        TEST_ASSERT_EQUAL_size_t(st == ODIN3_OK ? 3 : 0, out.len);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_normalize_like_normpath);
    RUN_TEST(test_normalize_replaces_contents);
    RUN_TEST(test_resolve);
    RUN_TEST(test_path_dir);
    RUN_TEST(test_stat_follows_symlinks);
    RUN_TEST(test_project_path_found);
    RUN_TEST(test_symlinked_directory_not_resolved);
    RUN_TEST(test_missing_file_and_directory);
    RUN_TEST(test_nul_in_given_is_syntax);
    RUN_TEST(test_language_table);
    RUN_TEST(test_language_names);
    RUN_TEST(test_language_names_unknown);
    RUN_TEST(test_libext_languages);
    RUN_TEST(test_arch_kinds);
    RUN_TEST(test_path_suffix);
    RUN_TEST(test_out_of_memory);
    RUN_TEST(test_normalize_out_of_memory);
    return UNITY_END();
}
