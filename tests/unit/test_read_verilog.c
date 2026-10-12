/*
 * test_read_verilog.c — the read_verilog pass (2C spec §4, VP-18/VP-19) through odin3_pass_run
 * on the trees in tests/unit/data/verilog/read and temporary ones: direct files and project
 * records, --parse-only and the hand-over to 2D's entry, duplicate_module, the unnamed-instance
 * rule, -v/-y
 * library resolution, unsupported_language, a missing file, the error cap across files, the
 * Debug check and an OOM sweep. Every located message is asserted exactly.
 */
#include "ast/ast.h"
#include "ast/ast_test.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "frontends/verilog/read_verilog_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
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

#ifndef ODIN3_RV_DATA
#error "ODIN3_RV_DATA must name tests/unit/data/verilog/read"
#endif
#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

#define D ODIN3_RV_DATA
#define C ODIN3_PROJECT_FIXTURES

enum {
    PATH_BUF = 1024,
    FILE_MAX = 1 << 16,
    OOM_LIMIT = 100000,
    ERROR_LINES = 12,
    PP_FILES = 25 /* files of one preprocessor error each, p1.v .. p25.v */
};

static odin3_design *design;
static odin3_strbuf logbuf; /* every message but the pass manager's own, each + '\n' */
static int errors;
static char cwd[PATH_BUF];
static char tmp[PATH_BUF / 4];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (strncmp(msg, "pass ", strlen("pass ")) == 0) {
        return;
    }
    errors += level == ODIN3_LOG_ERROR;
    (void)odin3_strbuf_appendf(&logbuf, "%s\n", msg);
}

static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_strbuf_clear(&logbuf);
    errors = 0;
}

void setUp(void) {
    odin3_strbuf_init(&logbuf);
    odin3_log_set_sink(sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3-read-verilog.XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    design = NULL;
    fresh_design();
    odin3_pass_options opts = odin3_pass_get_options();
    opts.check = true; /* the Debug check also in a Release test build */
    odin3_pass_set_options(opts);
}

/* The files a test may leave in tmp (and p1.v .. p25.v). */
static const char *const k_outputs[] = {"e1.v", "e2.v", "e3.v", "out.v", "u.v", "f.v", "l.o3proj"};

void tearDown(void) {
    char path[PATH_BUF];
    for (size_t i = 0; i < sizeof k_outputs / sizeof k_outputs[0]; i++) {
        (void)snprintf(path, sizeof path, "%s/%s", tmp, k_outputs[i]);
        (void)unlink(path);
    }
    for (int i = 1; i <= PP_FILES; i++) {
        (void)snprintf(path, sizeof path, "%s/p%d.v", tmp, i);
        (void)unlink(path);
    }
    odin3_pass_options opts = odin3_pass_get_options();
    opts.diag_paths = ODIN3_SRCFMT_GIVEN;
    odin3_pass_set_options(opts);
    (void)rmdir(tmp);
    odin3_util_set_alloc_fail_after(-1);
    odin3_rv_test_set_hook(NULL, NULL);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    odin3_design_destroy(design);
    design = NULL;
    odin3_strbuf_free(&logbuf);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void enter(const char *dir) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chdir(dir), dir);
}

static const char *logged(void) {
    return logbuf.data != NULL ? logbuf.data : "";
}

static odin3_status run(const char *pass, const char *args) {
    odin3_strbuf_clear(&logbuf);
    errors = 0;
    return odin3_pass_run(design, pass, odin3_bytes_cstr(args));
}

/* The pass returned want and the log holds needle. */
static void expect_log(odin3_status want, odin3_status got, const char *needle) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, got, logged());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(logged(), needle), logged());
}

/* The store the design holds (exactly one). */
static odin3_ast *held(void) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, odin3_design_ast_count(design), logged());
    return odin3_design_ast_at(design, 0);
}

static const char *unit_name(const odin3_ast *ast, uint32_t idx) {
    return odin3_ast_name_str(ast, odin3_ast_root(ast, idx));
}

/* The held store's UNITs are named want[0 .. n), in order. */
static void expect_units(const char *const *want, uint32_t n) {
    odin3_ast *ast = held();
    TEST_ASSERT_EQUAL_UINT32(n, odin3_ast_root_count(ast));
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], unit_name(ast, i));
    }
}

/* Writes text to tmp/name. */
static void put_tmp(const char *name, odin3_bytes text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_size_t(text.len, fwrite(text.ptr, 1, text.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The whole file tmp/name, NUL-terminated, in buf (FILE_MAX bytes). */
static const char *slurp(const char *name, char *buf) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, FILE_MAX - 1, file);
    buf[got] = '\0';
    (void)fclose(file);
    return buf;
}

/* --- arguments, direct files and project records -------------------------------------------- */

static void test_arguments(void) {
    expect_log(ODIN3_ERR_INVALID_ARG, run("read_verilog", "--bogus a.v"),
               "read_verilog: unknown option '--bogus'\n");
    expect_log(ODIN3_ERR_INVALID_ARG, run("read_verilog", "--parse-only"),
               "read_verilog: no project read\n");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* Two direct files: two UNITs in order, each named by its path as given. */
static void test_two_direct_files(void) {
    enter(D "/proj");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only src/b.v src/c.v"),
                                  logged());
    static const char *const k_want[] = {"src/b.v", "src/c.v"};
    expect_units(k_want, 2);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_library_units_from(held()));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* The log line of step 7: units, library units, nodes, comments, stream bytes, time. */
static void test_summary_line(void) {
    enter(D "/proj");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--parse-only src/c.v"));
    char want[PATH_BUF];
    (void)snprintf(want, sizeof want,
                   "read_verilog: 1 units (0 library), %u nodes, 1 comments, 81 bytes, ",
                   odin3_ast_node_end(held()).v - 1); /* src/c.v is 81 bytes, one comment */
    const char *line = strstr(logged(), want);
    TEST_ASSERT_NOT_NULL_MESSAGE(line, logged());
    char *end = NULL;
    double ms = strtod(line + strlen(want), &end);
    TEST_ASSERT_TRUE(ms >= 0.0);
    TEST_ASSERT_EQUAL_STRING_LEN(" ms\n", end, strlen(" ms\n"));
}

/* A record: its Verilog files in listing order, its define W (8) and include directory. */
static void test_project_record(void) {
    enter(D "/proj");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "p.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/a.v", "src/b.v"};
    expect_units(k_want, 2);
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/out.v", tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog_ast", path));
    static char text[FILE_MAX];
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(slurp("out.v", text), "input wire [8 - 1:0] x"), text);
}

/* Direct files with a record: the record's defines and include directories apply. */
static void test_direct_file_with_record(void) {
    enter(D "/proj");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "p.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only src/a.v"), logged());
    static const char *const k_want[] = {"src/a.v"};
    expect_units(k_want, 1);
}

/* An unfinished or missing record, and one without Verilog design files. */
static void test_no_verilog_files(void) {
    enter(D "/sv");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "none.o3proj"));
    expect_log(ODIN3_ERR_INVALID_ARG, run("read_verilog", "--parse-only"),
               "read_verilog: no Verilog files\n");
}

/* A SystemVerilog file of the record: unsupported_language naming it, at its project word. */
static void test_unsupported_language(void) {
    enter(D "/sv");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "p.o3proj"));
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only"),
               "p.o3proj:3:20: error: unsupported_language: no SystemVerilog reader yet: "
               "'src/b.sv'\n");
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* A missing file: the preprocessor's error (unlocated: a direct file has no naming word); the
 * run goes on to the next file, whose syntax error is reported too. */
static void test_missing_file(void) {
    enter(D "/proj");
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only src/nope.v ../dup/syntax.v"),
               "missing_file: 'src/nope.v'\n"
               "../dup/syntax.v:3:12: error: syntax: unexpected identifier, expecting ';' or ',' "
               "or '=' or '['\n");
    TEST_ASSERT_EQUAL_INT(2, errors);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* A file that exists but cannot be read: its cause is logged, the status is PARSE (§4.2 step
 * 6). */
static void test_unreadable_file(void) {
    if (geteuid() == 0) {
        TEST_IGNORE_MESSAGE("root reads any file");
    }
    put_tmp("u.v", odin3_bytes_cstr("module u;\nendmodule\n"));
    enter(tmp);
    TEST_ASSERT_EQUAL_INT(0, chmod("u.v", 0));
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only u.v"),
               "cannot read 'u.v': Permission denied\n");
    TEST_ASSERT_EQUAL_INT(1, errors);
}

/* --- --parse-only and the 2D entry ------------------------------------------------------------ */

/* --parse-only keeps the store (whatever the keep level) and write_verilog_ast prints it. */
static void test_parse_only_printable(void) {
    enter(D "/proj");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--parse-only src/b.v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, odin3_design_keep_ast(design));
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/out.v", tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog_ast", path));
    static char text[FILE_MAX];
    slurp("out.v", text);
    TEST_ASSERT_EQUAL_STRING_LEN(
        "// odin3 write_verilog_ast: run 1, parsed AST\n// unit src/b.v at ", text,
        strlen("// odin3 write_verilog_ast: run 1, parsed AST\n"
               "// unit src/b.v at "));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "module b"), text);
}

/* An empty module: stage 1 is clean on it while every module item is pending (2D point (l)). */
static const char k_empty_module[] = "module e;\nendmodule\n";

/* Without --parse-only 2D's entry runs: a clean stage 1 reaches stage 2, which elaborates the
 * empty module, sets the top and logs the summary; both stores are freed (keep NONE). */
static void test_elaborates_without_parse_only(void) {
    put_tmp("e1.v", odin3_bytes_cstr(k_empty_module));
    enter(tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "e1.v"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(logged(), "read_verilog: elaborated top 'e': 1 modules"),
                                 logged());
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_design_top(design).v);
}

/* Under --keep-ast=all both stores survive the elaboration (AST-16). */
static void test_keeps_both_under_all(void) {
    put_tmp("e1.v", odin3_bytes_cstr(k_empty_module));
    enter(tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ALL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "e1.v"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_ast_count(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_FORM_PARSED, odin3_ast_form_of(odin3_design_ast_at(design, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_FORM_ELABORATED,
                          odin3_ast_form_of(odin3_design_ast_at(design, 1)));
}

/* --- duplicate_module (step 4) ---------------------------------------------------------------- */

static void test_duplicate_module(void) {
    enter(D "/dup");
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only one.v"),
               "one.v:5:1: error: duplicate_module: module 'm' is already defined\n");
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* Design units are identified by library and name (DESIGN §4.0): one module name in two
 * libraries is no duplicate (2D's lookup picks by library, EL-6); twice in one library is. */
static void test_duplicate_module_per_library(void) {
    put_tmp("e2.v", odin3_bytes_cstr("module m;\nendmodule\n"));
    put_tmp("e3.v", odin3_bytes_cstr("module m;\nendmodule\n"));
    put_tmp("l.o3proj", odin3_bytes_cstr("file verilog e2.v\nfile verilog e3.v library lib2\n"));
    enter(tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "l.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    TEST_ASSERT_EQUAL_INT(0, errors);
    fresh_design();
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only e2.v e3.v"),
               "e3.v:1:1: error: duplicate_module: module 'm' is already defined\n");
}

/* The 2B corpus case: src/other.v:2, names [helper]. */
static void test_duplicate_module_corpus(void) {
    enter(C "/neg_duplicate_module");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "project.o3proj"));
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only"),
               "src/other.v:2:1: error: duplicate_module: module 'helper' is already defined\n");
}

/* A syntax error in file 1 and a duplicate_module both reported; the store is destroyed. */
static void test_syntax_error_and_duplicate(void) {
    enter(D "/dup");
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only syntax.v one.v"),
               "syntax.v:3:12: error: syntax: unexpected identifier, expecting ';' or ',' or '=' "
               "or '['\n"
               "one.v:5:1: error: duplicate_module: module 'm' is already defined\n");
    TEST_ASSERT_EQUAL_INT(2, errors);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* --- unnamed instances (step 5) ----------------------------------------------------------------
 */

static void test_unnamed_udp_instance_passes(void) {
    enter(D "/unnamed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only udp_ok.v"), logged());
}

static void test_unnamed_module_instance_errors(void) {
    enter(D "/unnamed");
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only mod_bad.v"),
               "mod_bad.v:7:9: error: instance of 'sub' has no name (only gate and UDP instances "
               "may be unnamed)\n");
    TEST_ASSERT_EQUAL_INT(1, errors);
}

static void test_unnamed_udp_from_library_file(void) {
    enter(D "/udp_lib");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "p.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"top.v", "udps.v"};
    expect_units(k_want, 2);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_library_units_from(held()));
}

/* --- libraries (step 3) --------------------------------------------------------------------- */

/* -v: every file read at once, the first wins; the second's module stays, with an info line. */
static void test_libfile_first_wins(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "first.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/first_top.v", "lib/a.v", "lib/b.v"};
    expect_units(k_want, 3);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_library_units_from(held()));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(logged(),
               "lib/b.v:2:1: info: library module 'thru' ignored: an earlier library definition "
               "wins\n"),
        logged());
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* -y with libext .vh then .v: and2.vh is found first; mux2.v instantiates inv2 (also -y);
 * or2.v is never read. */
static void test_libdir_libext_order(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "ext.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/ext_top.v", "lib/cells/and2.vh", "lib/cells/mux2.v",
                                         "lib/cells/inv2.v"};
    expect_units(k_want, 4);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_library_units_from(held()));
}

/* A design unit shadows a library module (info line); library_units_from is past every design
 * unit, so 2D never takes a library module as top. */
static void test_design_unit_shadows_library(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "shadow.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/shadow_top.v", "lib/a.v"};
    expect_units(k_want, 2);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_library_units_from(held()));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(logged(),
               "lib/a.v:6:1: info: library module 'inv' ignored: a design unit defines it\n"),
        logged());
}

/* -y is directory-major: lib/d1 with every libext before lib/d2. */
static void test_libdirs_before_libexts(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "dirs.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/dirs_top.v", "lib/d1/x.vh"};
    expect_units(k_want, 2);
}

/* -v before -y: a name both define comes from the -v file; the -y file is never read. */
static void test_libfiles_before_libdirs(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "order.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/order_top.v", "lib/vq.v"};
    expect_units(k_want, 2);
}

/* -v files are read only when a name is undefined: here every name resolves in the design. */
static void test_libfiles_read_lazily(void) {
    enter(D "/lib_v_y");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "lazy.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_want[] = {"src/lazy_top.v"};
    expect_units(k_want, 1);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_library_units_from(held()));
}

/* A design module shadowing a library primitive of the same name is a module: its unnamed
 * instance is an error with the library as without it. */
static void test_design_module_shadows_library_udp(void) {
    static const char k_error[] = "top.v:2:9: error: instance of 'foo' has no name (only gate and "
                                  "UDP instances may be unnamed)\n";
    enter(D "/udp_shadow");
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only top.v"), k_error);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "p.o3proj"));
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only"), k_error);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(logged(), "lib.v:2:1: info: library primitive 'foo' ignored: a design unit "
                         "defines it\n"),
        logged());
    TEST_ASSERT_EQUAL_INT(1, errors);
}

/* The hook's copy of odin3_pp_files_read, relative to the case directory and sorted. */
static char files_read[8][PATH_BUF];
static uint32_t nfiles_read;

static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp(lhs, rhs);
}

static void grab_files_read(odin3_rv *rv, void *user) {
    const char *base = user;
    const uint32_t *paths = NULL;
    nfiles_read = odin3_pp_files_read(rv->pp, &paths);
    TEST_ASSERT_TRUE(nfiles_read <= 8);
    for (uint32_t i = 0; i < nfiles_read; i++) {
        const char *path = odin3_strtab_get(odin3_design_strtab(rv->design), paths[i]);
        TEST_ASSERT_EQUAL_INT(0, strncmp(path, base, strlen(base)));
        (void)snprintf(files_read[i], PATH_BUF, "%s", path + strlen(base) + 1);
    }
    qsort(files_read, nfiles_read, sizeof files_read[0], cmp_str);
}

/* The 2B corpus lib_v_y: the files read equal its resolved.read; or2.v is never read. */
static void test_corpus_lib_v_y_files_read(void) {
    enter(C "/lib_v_y");
    char base[PATH_BUF];
    TEST_ASSERT_NOT_NULL(getcwd(base, sizeof base));
    odin3_rv_test_set_hook(grab_files_read, base);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "project.o3proj"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--parse-only"), logged());
    static const char *const k_read[] = {"lib/cells/and2.v", "lib/cells/mux2.v", "lib/prims.v",
                                         "src/top.v"};
    TEST_ASSERT_EQUAL_UINT32(4, nfiles_read);
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_STRING(k_read[i], files_read[i]);
    }
}

/* --- the error cap across files, the check, OOM ----------------------------------------------- */

/* Writes tmp/name: a module of ERROR_LINES lines that are each one syntax error. */
static void put_errors(const char *name) {
    char text[FILE_MAX / 4] = "module m;\n";
    for (int i = 0; i < ERROR_LINES; i++) {
        (void)strcat(text, "  wire a b;\n");
    }
    (void)strcat(text, "endmodule\n");
    put_tmp(name, odin3_bytes_cstr(text));
}

/* 12 errors in e1.v, then e2.v's: the 21st error is the stop line; e3.v is never read. */
static void test_error_cap_across_files(void) {
    put_errors("e1.v");
    put_errors("e2.v");
    put_errors("e3.v");
    enter(tmp);
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only e1.v e2.v e3.v"),
               "e1.v:13:10: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or "
               "'['\n"
               "e2.v:2:10: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or "
               "'['\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(logged(), "e2.v:9:10: error: syntax: unexpected identifier, expecting ';' or ',' "
                         "or '=' or '['\ne2.v:10:10: error: too many errors; stopping\n"),
        logged());
    TEST_ASSERT_EQUAL_INT(21, errors);
    TEST_ASSERT_NULL(strstr(logged(), "e3.v"));
}

/* Writes tmp/p1.v .. tmp/p<n>.v, each including a missing header (one preprocessor error). */
static void put_pp_errors(int n) {
    for (int i = 1; i <= n; i++) {
        char name[PATH_BUF / 4];
        char text[PATH_BUF / 4];
        (void)snprintf(name, sizeof name, "p%d.v", i);
        (void)snprintf(text, sizeof text, "`include \"nope%d.vh\"\nmodule m%d;\nendmodule\n", i, i);
        put_tmp(name, odin3_bytes_cstr(text));
    }
}

/* Appends " p1.v .. p<count>.v" to args (PATH_BUF bytes). */
static void add_pp_files(char *args, int count) {
    for (int i = 1; i <= count; i++) {
        size_t len = strlen(args);
        (void)snprintf(args + len, PATH_BUF - len, " p%d.v", i);
    }
}

/* Preprocessor errors count against the run's cap: 20, then the stop line, nothing more. */
static void test_error_cap_preprocessor(void) {
    put_pp_errors(PP_FILES);
    enter(tmp);
    char args[PATH_BUF] = "--parse-only";
    add_pp_files(args, PP_FILES);
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", args),
               "p20.v:1:1: error: include_not_found: 'nope20.vh'\ntoo many errors; stopping\n");
    TEST_ASSERT_EQUAL_INT(21, errors);
    TEST_ASSERT_NULL(strstr(logged(), "nope21"));
}

/* Parser and preprocessor errors share the cap: 12 + 8, then the stop line. */
static void test_error_cap_mixed(void) {
    put_errors("e1.v");
    put_pp_errors(PP_FILES);
    enter(tmp);
    char args[PATH_BUF] = "--parse-only e1.v";
    add_pp_files(args, 10);
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", args),
               "p8.v:1:1: error: include_not_found: 'nope8.vh'\ntoo many errors; stopping\n");
    TEST_ASSERT_EQUAL_INT(21, errors);
    TEST_ASSERT_NULL(strstr(logged(), "nope9"));
}

/* With --diag-paths resolved every diagnostic of the run names its file resolved: the
 * preprocessor's, syntax errors, duplicate_module, unnamed instances and library info lines. */
/* Enters dir and stores its absolute path (getcwd) in out (PATH_BUF bytes). */
static void enter_abs(const char *dir, char *out) {
    enter(dir);
    TEST_ASSERT_NOT_NULL(getcwd(out, PATH_BUF));
}

static void set_resolved(void) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.diag_paths = ODIN3_SRCFMT_RESOLVED;
    odin3_pass_set_options(opts);
}

static void test_diag_paths_resolved(void) {
    set_resolved();
    char proj[PATH_BUF];
    char dir[PATH_BUF];
    enter_abs(D "/proj", proj);
    enter_abs(D "/dup", dir);
    char want[PATH_BUF * 4];
    (void)snprintf(want, sizeof want,
                   "missing_file: '%s/src/nope.v'\n%s/syntax.v:3:12: error: syntax: unexpected "
                   "identifier, expecting ';' or ',' or '=' or '['\n%s/one.v:5:1: error: "
                   "duplicate_module: module 'm' is already defined\n",
                   proj, dir, dir);
    expect_log(ODIN3_ERR_PARSE,
               run("read_verilog", "--parse-only ../proj/src/nope.v syntax.v one.v"), want);
    TEST_ASSERT_EQUAL_INT(3, errors);
    enter_abs(D "/unnamed", dir);
    (void)snprintf(want, sizeof want, "%s/mod_bad.v:7:9: error: instance of 'sub'", dir);
    expect_log(ODIN3_ERR_PARSE, run("read_verilog", "--parse-only mod_bad.v"), want);
    enter_abs(D "/lib_v_y", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "first.o3proj"));
    (void)snprintf(want, sizeof want, "%s/lib/b.v:2:1: info: library module 'thru'", dir);
    expect_log(ODIN3_OK, run("read_verilog", "--parse-only"), want);
}

/* Corrupts the first node of the sealed store, before the check. */
static void corrupt_first_node(odin3_rv *rv, void *user) {
    (void)user;
    odin3_ast_test_target target = {ODIN3_AST_TEST_LOC, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_test_corrupt(rv->ast, target));
}

/* The check of step 7 runs: a corrupt store is ODIN3_ERR_CHECK and is destroyed. */
static void test_check_runs(void) {
    enter(D "/proj");
    odin3_rv_test_set_hook(corrupt_first_node, NULL);
    expect_log(ODIN3_ERR_CHECK, run("read_verilog", "--parse-only src/b.v"), "ast check: rule 1");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(design));
}

/* One OOM sweep: the project read first (NULL: none), then read_verilog with args, ending
 * with status want (the store held exactly when want is OK). */
typedef struct oom_case {
    const char *project;
    const char *args;
    odin3_status want;
    uint32_t held; /* stores the design holds after a run that ends in want */
} oom_case;

/* One step: a fresh design, the record, then the read with allocation limit limit; NO_MEMORY or
 * the case's own status, nothing held on failure. */
static odin3_status oom_step(const oom_case *oc, int limit) {
    fresh_design();
    if (oc->project != NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", oc->project));
    }
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = run("read_verilog", oc->args);
    odin3_util_set_alloc_fail_after(-1);
    /* 2C reports a file it could not read (an allocation failed in the preprocessor) as PARSE */
    TEST_ASSERT_TRUE_MESSAGE(st == oc->want || st == ODIN3_ERR_NO_MEMORY || st == ODIN3_ERR_PARSE,
                             logged());
    TEST_ASSERT_EQUAL_UINT32(st == oc->want ? oc->held : 0, odin3_design_ast_count(design));
    return st;
}

/* Fails every allocation in turn until the case runs to its own status; returns the steps. */
static int oom_sweep(const oom_case *oc) {
    int limit = 0;
    while (limit < OOM_LIMIT && oom_step(oc, limit) != oc->want) {
        limit++;
    }
    TEST_ASSERT_TRUE(limit < OOM_LIMIT);
    return limit;
}

/* Every allocation fails in turn: a library run, a duplicate_module run (its map, the error
 * path) and a run through the 2D entry (set_ast, stage 1, drop_ast, stage 2, the elaborated
 * store's hand-over); nothing leaked. */
static void test_oom_sweep(void) {
    enter(D "/lib_v_y");
    oom_case lib = {"ext.o3proj", "--parse-only", ODIN3_OK, 1};
    TEST_ASSERT_TRUE(oom_sweep(&lib) > 50); /* the read itself allocates, not only the run */
    enter(D "/dup");
    oom_case dup = {NULL, "--parse-only one.v", ODIN3_ERR_PARSE, 0};
    (void)oom_sweep(&dup);
    put_tmp("e1.v", odin3_bytes_cstr(k_empty_module));
    enter(tmp);
    oom_case elab = {NULL, "e1.v", ODIN3_OK, 0};
    (void)oom_sweep(&elab);
}

/* The store's accessor: an index at or past the root count is ignored. */
static void test_library_units_from_accessor(void) {
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    odin3_ast_set_library_units_from(ast, 0);
    odin3_ast_set_library_units_from(ast, 3);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_library_units_from(ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_library_units_from(NULL));
    odin3_ast_set_library_units_from(NULL, 1);
    odin3_ast_destroy(ast);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arguments);
    RUN_TEST(test_two_direct_files);
    RUN_TEST(test_summary_line);
    RUN_TEST(test_project_record);
    RUN_TEST(test_direct_file_with_record);
    RUN_TEST(test_no_verilog_files);
    RUN_TEST(test_unsupported_language);
    RUN_TEST(test_missing_file);
    RUN_TEST(test_unreadable_file);
    RUN_TEST(test_parse_only_printable);
    RUN_TEST(test_elaborates_without_parse_only);
    RUN_TEST(test_keeps_both_under_all);
    RUN_TEST(test_duplicate_module);
    RUN_TEST(test_duplicate_module_corpus);
    RUN_TEST(test_duplicate_module_per_library);
    RUN_TEST(test_syntax_error_and_duplicate);
    RUN_TEST(test_unnamed_udp_instance_passes);
    RUN_TEST(test_unnamed_module_instance_errors);
    RUN_TEST(test_unnamed_udp_from_library_file);
    RUN_TEST(test_libfile_first_wins);
    RUN_TEST(test_libdir_libext_order);
    RUN_TEST(test_design_unit_shadows_library);
    RUN_TEST(test_libdirs_before_libexts);
    RUN_TEST(test_libfiles_before_libdirs);
    RUN_TEST(test_libfiles_read_lazily);
    RUN_TEST(test_design_module_shadows_library_udp);
    RUN_TEST(test_corpus_lib_v_y_files_read);
    RUN_TEST(test_error_cap_across_files);
    RUN_TEST(test_error_cap_preprocessor);
    RUN_TEST(test_error_cap_mixed);
    RUN_TEST(test_diag_paths_resolved);
    RUN_TEST(test_check_runs);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_library_units_from_accessor);
    return UNITY_END();
}
