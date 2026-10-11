/*
 * test_project_pass.c — unit tests for the read_project, dump_project and preprocess passes (2B
 * PRJ-16, PRJ-17), the --revision option reaching a read_project inside a script, the
 * --diag-paths option the pass manager applies before each pass, and odin3_diag's file names in
 * either style (PRJ-18).
 */
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/file.h"
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

#ifndef ODIN3_CLI_PROJECT
#error "ODIN3_CLI_PROJECT must name tests/cli/project"
#endif
#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

#define P ODIN3_CLI_PROJECT
#define C ODIN3_PROJECT_FIXTURES

enum { PATH_BUF = 1024, MSG_MAX = 4096, OOM_LIMIT = 20000, FILE_MAX = 1 << 16 };

static odin3_design *design;
static char captured[MSG_MAX]; /* the last error */
static char info[MSG_MAX];     /* the last info line that is not a pass timing */
static int errors;
static char cwd[PATH_BUF];
static char tmp[PATH_BUF / 4];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && errors++ == 0) {
        (void)snprintf(captured, sizeof captured, "%s", msg);
    } else if (level == ODIN3_LOG_INFO && strncmp(msg, "pass ", strlen("pass ")) != 0) {
        (void)snprintf(info, sizeof info, "%s", msg);
    }
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    errors = 0;
    captured[0] = '\0';
    info[0] = '\0';
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3-project-pass.XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    design = NULL;
    fresh_design();
}

/* The files a test may leave in tmp. */
static const char *const k_outputs[] = {"0.v", "0.segments.json", "1.v", "files-read.txt",
                                        "dump.json"};

void tearDown(void) {
    char path[PATH_BUF];
    for (size_t i = 0; i < sizeof k_outputs / sizeof k_outputs[0]; i++) {
        (void)snprintf(path, sizeof path, "%s/%s", tmp, k_outputs[i]);
        (void)unlink(path);
    }
    (void)rmdir(tmp);
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    (void)odin3_pass_set_revision(NULL);
    (void)odin3_pass_set_top_name(NULL);
    odin3_pass_options opts = odin3_pass_get_options();
    opts.diag_paths = ODIN3_SRCFMT_GIVEN;
    odin3_pass_set_options(opts);
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_status run(const char *pass, const char *args) {
    errors = 0;
    captured[0] = '\0';
    return odin3_pass_run(design, pass, odin3_bytes_cstr(args));
}

static odin3_status script(const char *text) {
    errors = 0;
    captured[0] = '\0';
    return odin3_pass_run_script(design, odin3_bytes_cstr(text),
                                 (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND});
}

/* The pass failed with want and its first error was message. */
static void expect(odin3_status want, const char *message, odin3_status got) {
    TEST_ASSERT_EQUAL_INT(want, got);
    TEST_ASSERT_EQUAL_STRING(message, captured);
}

static odin3_project *project(void) {
    odin3_project *proj = odin3_design_project(design);
    TEST_ASSERT_NOT_NULL(proj);
    return proj;
}

static const char *str(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
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

static bool exists(const char *name) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, name);
    struct stat info_buf;
    return stat(path, &info_buf) == 0;
}

/* --- read_project -------------------------------------------------------------------------- */

static void test_read_project_arguments(void) {
    expect(ODIN3_ERR_INVALID_ARG, "read_project: expects one entry file", run("read_project", ""));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: expects one entry file",
           run("read_project", P "/p.o3proj " P "/files.f"));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: expects one entry file",
           run("read_project", "\"\""));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: --format expects o3proj, f, qsf or odin2",
           run("read_project", "--format vhdl " P "/p.o3proj"));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: --format expects o3proj, f, qsf or odin2",
           run("read_project", "--format"));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: --revision expects a revision name",
           run("read_project", P "/p.o3proj --revision"));
    expect(ODIN3_ERR_INVALID_ARG, "read_project: unknown argument '--bogus'",
           run("read_project", "--bogus " P "/p.o3proj"));
    TEST_ASSERT_NULL(odin3_design_project(design)); /* nothing was read */
}

static void test_read_project_reads(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", P "/p.o3proj"));
    TEST_ASSERT_TRUE(project()->finished);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_O3PROJ, project()->format);
    TEST_ASSERT_EQUAL_STRING("top", str(project()->top));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)project()->files.len);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_module_end(design)); /* no front end ran */
    TEST_ASSERT_TRUE(odin3_design_has_project(design));
    TEST_ASSERT_FALSE(odin3_design_has_project(NULL));
    expect(ODIN3_ERR_INVALID_ARG, "odin3: error: project already read from " P "/p.o3proj",
           run("read_project", P "/p.o3proj"));
}

static void test_read_project_format(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", "--format f " P "/files.f"));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_FLIST, project()->format);
    TEST_ASSERT_EQUAL_UINT32(0, project()->top);
    fresh_design();
    expect(ODIN3_ERR_PARSE, P "/files.f:1:1: error: unknown_key: unknown key: '//'",
           run("read_project", "--format o3proj " P "/files.f"));
    TEST_ASSERT_NULL(odin3_design_project(design)); /* a failed read drops the record */
    TEST_ASSERT_FALSE(odin3_design_has_project(design));
}

static void test_read_project_missing_entry(void) {
    expect(ODIN3_ERR_PARSE, "odin3: error: missing_file: project not found: '" P "/none.o3proj'",
           run("read_project", P "/none.o3proj"));
}

/* --revision (the option, as the CLI sets it) reaches a read_project inside a script; the
 * pass's own --revision wins. */
static void test_revision_option(void) {
    const char *read = "read_project " C "/qpf_revisions/proj.qpf";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, script(read));
    TEST_ASSERT_NOT_NULL(strstr(captured, "ambiguous_revision"));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_revision("fast"));
    TEST_ASSERT_TRUE(odin3_pass_get_options().has_revision);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script(read));
    TEST_ASSERT_TRUE(project()->has_revision);
    TEST_ASSERT_EQUAL_STRING("fast", str(project()->revision));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          script("read_project --revision small " C "/qpf_revisions/proj.qpf"));
    TEST_ASSERT_EQUAL_STRING("small", str(project()->revision));
}

/* The setter copies its revision; "" is one, NULL clears it. */
static void test_revision_setter(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_revision("")); /* "" is a revision */
    TEST_ASSERT_TRUE(odin3_pass_get_options().has_revision);
    TEST_ASSERT_EQUAL_STRING("", odin3_pass_get_options().revision);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_revision(NULL));
    TEST_ASSERT_FALSE(odin3_pass_get_options().has_revision);
    TEST_ASSERT_NULL(odin3_pass_get_options().revision);
}

/* 1D's --top replaces the project's top, with an info line. */
static void test_top_option(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top_name("other"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", P "/p.o3proj"));
    TEST_ASSERT_EQUAL_STRING("other", str(project()->top));
    TEST_ASSERT_EQUAL_STRING(P "/p.o3proj: --top other replaces the project top top", info);
}

/* --- dump_project -------------------------------------------------------------------------- */

static void test_dump_project_misuse(void) {
    expect(ODIN3_ERR_INVALID_ARG, "dump_project: no project was read (run read_project first)",
           run("dump_project", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", P "/p.o3proj"));
    expect(ODIN3_ERR_INVALID_ARG, "dump_project: expects at most one path",
           run("dump_project", "a.json b.json"));
}

static void test_dump_project_file(void) {
    char args[2 * PATH_BUF];
    (void)snprintf(args, sizeof args, "read_project " P "/p.o3proj; dump_project %s/dump.json",
                   tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script(args));
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(project(), &want));
    static char got[FILE_MAX];
    TEST_ASSERT_EQUAL_STRING(want.data, slurp("dump.json", got));
    odin3_strbuf_free(&want);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"top\": \"top\""));
}

/* --- --diag-paths -------------------------------------------------------------------------- */

/* The pass manager applies the option before each pass: the same error names its file as given,
 * then resolved. */
static void test_diag_paths_option(void) {
    TEST_ASSERT_EQUAL_INT(0, chdir(P));
    expect(ODIN3_ERR_PARSE, "bad.o3proj:1:1: error: unknown_key: unknown key: 'nonsense'",
           run("read_project", "bad.o3proj"));
    odin3_pass_options opts = odin3_pass_get_options();
    opts.diag_paths = ODIN3_SRCFMT_RESOLVED;
    odin3_pass_set_options(opts);
    expect(ODIN3_ERR_PARSE, P "/bad.o3proj:1:1: error: unknown_key: unknown key: 'nonsense'",
           run("read_project", "bad.o3proj"));
    TEST_ASSERT_EQUAL_INT(ODIN3_SRCFMT_RESOLVED, odin3_design_diag_paths(design));
}

/* odin3_diag prints the header and the include chain in the design's style. */
static void test_diag_style(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    uint32_t names[4] = {0};
    const char *texts[] = {"a.v", "/abs/a.v", "b.vh", "/abs/inc/b.vh"};
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_design_intern(design, odin3_bytes_cstr(texts[i]), &names[i]));
    }
    odin3_srcbuf_id outer = {0};
    odin3_srcbuf_id inner = {0};
    odin3_srcfile_spec spec = {.name = names[0], .resolved = names[1], .len = 10};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &outer));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, outer, 5));
    spec = (odin3_srcfile_spec){.name = names[2],
                                .resolved = names[3],
                                .parent = odin3_srcman_loc(sm, outer, 6).v,
                                .len = 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &inner));
    odin3_loc at = odin3_srcman_loc(sm, inner, 2);
    odin3_diag(design, ODIN3_LOG_ERROR, at, "syntax: x");
    TEST_ASSERT_EQUAL_STRING("b.vh:1:3: error: syntax: x\n  included from a.v:2", captured);
    errors = 0;
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_RESOLVED);
    odin3_diag(design, ODIN3_LOG_ERROR, at, "syntax: x");
    TEST_ASSERT_EQUAL_STRING("/abs/inc/b.vh:1:3: error: syntax: x\n  included from /abs/a.v:2",
                             captured);
}

/* --- preprocess ---------------------------------------------------------------------------- */

static void test_preprocess_misuse(void) {
    expect(ODIN3_ERR_INVALID_ARG, "preprocess: no project was read (run read_project first)",
           run("preprocess", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", P "/p.o3proj"));
    expect(ODIN3_ERR_INVALID_ARG, "preprocess: --segments needs --out <dir>",
           run("preprocess", "--segments"));
    expect(ODIN3_ERR_INVALID_ARG, "preprocess: --out expects a directory",
           run("preprocess", "--out"));
    expect(ODIN3_ERR_INVALID_ARG, "preprocess: unknown argument 'x'", run("preprocess", "x"));
}

static const char k_top_expanded[] =
    "\n\nmodule top (input wire [4-1:0] a, output wire [4-1:0] y);\n"
    "    assign y = ~(a);\n"
    "endmodule\n";

static void test_preprocess_out(void) {
    char args[2 * PATH_BUF];
    (void)snprintf(args, sizeof args, "read_project " P "/p.o3proj; preprocess --out %s --segments",
                   tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script(args));
    static char got[FILE_MAX];
    TEST_ASSERT_EQUAL_STRING(k_top_expanded, slurp("0.v", got));
    TEST_ASSERT_EQUAL_STRING(P "/src/top.v\n" P "/inc/defs.vh\n", slurp("files-read.txt", got));
    TEST_ASSERT_FALSE(exists("1.v"));
}

/* The segment map of the same run: one segment per line, each decoded. */
static void test_preprocess_segments(void) {
    char args[2 * PATH_BUF];
    (void)snprintf(args, sizeof args, "read_project " P "/p.o3proj; preprocess --out %s --segments",
                   tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script(args));
    static char got[FILE_MAX];
    slurp("0.segments.json", got);
    TEST_ASSERT_EQUAL_STRING_LEN("[\n  {\"out_offset\": 0, \"file\": \"", got, 31);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"file\": \"src/top.v\", \"line\": 2, \"col\": 25, "
                                     "\"buffer_kind\": \"expansion\", \"macro\": \"WIDTH\"}"));
    TEST_ASSERT_NOT_NULL(strstr(got, "\"buffer_kind\": \"macro_arg\", \"macro\": \"INV\"}"));
    TEST_ASSERT_NOT_NULL(strstr(got, "\"buffer_kind\": \"file\", \"macro\": null}"));
    TEST_ASSERT_EQUAL_STRING("\n]\n", got + strlen(got) - 3);
}

/* Segment file names follow --diag-paths too. */
static void test_preprocess_segments_resolved(void) {
    odin3_design_set_diag_paths(design, ODIN3_SRCFMT_RESOLVED);
    odin3_pass_options opts = odin3_pass_get_options();
    opts.diag_paths = ODIN3_SRCFMT_RESOLVED;
    odin3_pass_set_options(opts);
    char args[2 * PATH_BUF];
    (void)snprintf(args, sizeof args, "read_project " P "/p.o3proj; preprocess --out %s --segments",
                   tmp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, script(args));
    static char got[FILE_MAX];
    slurp("0.segments.json", got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"file\": \"" P "/src/top.v\""));
    TEST_ASSERT_NULL(strstr(got, "\"file\": \"src/top.v\""));
}

static void test_preprocess_include_error(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_project", C "/neg_include_not_found/project.o3proj"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("preprocess", ""));
    TEST_ASSERT_NOT_NULL(strstr(captured, "error: include_not_found: "));
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Every allocation of read_project, dump_project and preprocess fails in turn: each fails
 * cleanly (ASan finds leaks) until the script runs through. */
static void test_oom_sweep(void) {
    char args[2 * PATH_BUF];
    (void)snprintf(args, sizeof args,
                   "read_project " P "/p.o3proj; dump_project %s/dump.json; preprocess --out %s "
                   "--segments",
                   tmp, tmp);
    odin3_log_set_sink(NULL, NULL);
    (void)odin3_log_set_level(ODIN3_LOG_ERROR);
    odin3_log_set_sink(sink, NULL);
    bool done = false;
    for (int limit = 0; !done && limit < OOM_LIMIT; limit++) {
        fresh_design();
        odin3_util_set_alloc_fail_after(limit);
        odin3_status st = script(args);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, captured);
        done = st == ODIN3_OK;
    }
    TEST_ASSERT_TRUE(done);
    static char got[FILE_MAX];
    TEST_ASSERT_EQUAL_STRING(k_top_expanded, slurp("0.v", got));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_read_project_arguments);
    RUN_TEST(test_read_project_reads);
    RUN_TEST(test_read_project_format);
    RUN_TEST(test_read_project_missing_entry);
    RUN_TEST(test_revision_option);
    RUN_TEST(test_revision_setter);
    RUN_TEST(test_top_option);
    RUN_TEST(test_dump_project_misuse);
    RUN_TEST(test_dump_project_file);
    RUN_TEST(test_diag_paths_option);
    RUN_TEST(test_diag_style);
    RUN_TEST(test_preprocess_misuse);
    RUN_TEST(test_preprocess_out);
    RUN_TEST(test_preprocess_segments);
    RUN_TEST(test_preprocess_segments_resolved);
    RUN_TEST(test_preprocess_include_error);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
