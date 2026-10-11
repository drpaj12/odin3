/*
 * test_project_flist.c — unit tests for the -f/-F file-list reader (PRJ-11): every §5.2 table row
 * and error kind with its exact located message, $VAR timing and forms, -sv scoping, the -f/-F
 * resolution bases (I5), cycles, diamonds, CRLF, the caps through the hook, the corpus's -f
 * cases and an OOM sweep.
 */
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

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif
#ifndef ODIN3_FLIST_DATA
#error "ODIN3_FLIST_DATA must name tests/unit/data/project/flist"
#endif

enum { PATH_BUF = 1024, MSG_MAX = 8192, MAX_MADE = 64, OOM_LIMIT = 2000, FIXTURE_MAX = 1 << 16 };

static odin3_design *design;
static odin3_project *proj;
static odin3_strbuf text;
static char tmp[PATH_BUF / 2];
static char rel_dir[PATH_BUF]; /* the directory the last read's dump is made relative to */
static char captured[MSG_MAX];
static int errors;
static char made[MAX_MADE][PATH_BUF]; /* files and directories made in tmp, in creation order */
static int nmade;

/* Variables the tests set: test-only names are unset after every test, the corpus's (RTL, NOPE)
 * restored to what the process started with, even when a test fails midway. */
static const char *const k_test_vars[] = {"ODIN3_T_UNSET", "ODIN3_T_RTL", "ODIN3_T$X",  "ODIN3_T_A",
                                          "ODIN3_T_A$B",   "ODIN3_T_D",   "ODIN3_T_OPT"};
static const char *const k_corpus_vars[] = {"RTL", "NOPE"};
static char *saved_vars[sizeof k_corpus_vars / sizeof k_corpus_vars[0]];

static void restore_env(void) {
    for (size_t i = 0; i < sizeof k_test_vars / sizeof k_test_vars[0]; i++) {
        TEST_ASSERT_EQUAL_INT(0, unsetenv(k_test_vars[i]));
    }
    for (size_t i = 0; i < sizeof k_corpus_vars / sizeof k_corpus_vars[0]; i++) {
        int got = saved_vars[i] != NULL ? setenv(k_corpus_vars[i], saved_vars[i], 1)
                                        : unsetenv(k_corpus_vars[i]);
        TEST_ASSERT_EQUAL_INT(0, got);
    }
}

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    errors = 0;
    nmade = 0;
    odin3_strbuf_init(&text);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_flist_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    restore_env();
    odin3_log_set_sink(NULL, NULL);
    for (int i = nmade - 1; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(0, remove(made[i]));
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_strbuf_free(&text);
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void remember(const char *path) {
    TEST_ASSERT_TRUE(nmade < MAX_MADE);
    (void)snprintf(made[nmade++], PATH_BUF, "%s", path);
}

/* Writes tmp/rel (making its directories) with content. */
static void put(const char *rel, odin3_bytes content) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", tmp, rel);
    for (char *slash = strchr(path + strlen(tmp) + 1, '/'); slash != NULL;
         slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        struct stat st;
        if (stat(path, &st) != 0) {
            TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0700));
            remember(path);
        }
        *slash = '/';
    }
    struct stat old;
    bool fresh = stat(path, &old) != 0;
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(content.len, fwrite(content.ptr, 1, content.len, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    if (fresh) {
        remember(path);
    }
}

/* An empty file tmp/rel. */
static void touch(const char *rel) {
    put(rel, odin3_bytes_cstr(""));
}

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

/* Reads dir/rel as the entry list (given rel). */
static odin3_status read_in(const char *dir, const char *rel) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", dir, rel);
    odin3_strbuf norm;
    odin3_strbuf_init(&norm);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_path_normalize(odin3_bytes_cstr(path), &norm));
    odin3_proj_path entry = {id(rel), id(norm.data), {0}};
    odin3_strbuf_free(&norm);
    (void)snprintf(rel_dir, sizeof rel_dir, "%s", dir);
    return odin3_read_flist(proj, &entry);
}

/* Writes tmp/files.f with list and reads it. */
static odin3_status read_list(const char *list) {
    put("files.f", odin3_bytes_cstr(list));
    return read_in(tmp, "files.f");
}

static const char *data_dir(const char *tree, char *buf) {
    (void)snprintf(buf, PATH_BUF, "%s/%s", ODIN3_FLIST_DATA, tree);
    return buf;
}

/* The dump with every "<rel_dir>/" removed (the harness's relativizing). */
static const char *dump_rel(void) {
    odin3_strbuf raw;
    odin3_strbuf_init(&raw);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &raw));
    char prefix[PATH_BUF + 2];
    (void)snprintf(prefix, sizeof prefix, "%s/", rel_dir);
    odin3_strbuf_clear(&text);
    const char *at = raw.data;
    for (const char *hit = strstr(at, prefix); hit != NULL; hit = strstr(at, prefix)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_append(&text, (odin3_bytes){at, (size_t)(hit - at)}));
        at = hit + strlen(prefix);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(at)));
    odin3_strbuf_free(&raw);
    return text.data;
}

/* The dump line of key ("  \"key\": …,"), relativized, without its newline. */
static const char *line_of(const char *key) {
    static char line[MSG_MAX];
    char head[PATH_BUF];
    (void)snprintf(head, sizeof head, "\n  \"%s\": ", key);
    const char *at = strstr(dump_rel(), head);
    TEST_ASSERT_NOT_NULL(at);
    at++;
    const char *end = strchr(at, '\n');
    TEST_ASSERT_NOT_NULL(end);
    (void)snprintf(line, sizeof line, "%.*s", (int)(end - at), at);
    return line;
}

static void expect_ok(odin3_status st) {
    TEST_ASSERT_EQUAL_STRING("", captured);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

static void expect_error(odin3_status st, const char *message) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_EQUAL_STRING(message, captured);
    TEST_ASSERT_EQUAL_INT(1, errors);
}

/* message with every "%D" replaced by rel_dir. */
static const char *with_dir(const char *message) {
    static char out[MSG_MAX];
    size_t len = 0;
    for (const char *at = message; *at != '\0'; at++) {
        if (at[0] == '%' && at[1] == 'D') {
            len += (size_t)snprintf(out + len, sizeof out - len, "%s", rel_dir);
            at++;
        } else {
            out[len++] = *at;
        }
        TEST_ASSERT_TRUE(len < sizeof out);
    }
    out[len] = '\0';
    return out;
}

static void expect_error_in(odin3_status st, const char *message) {
    expect_error(st, with_dir(message));
}

static const char *const k_a_file = "  \"files\": [{\"path\": \"a.v\", \"language\": \"verilog\", "
                                    "\"library\": \"work\"}],";

/* --- bare words: sources ------------------------------------------------------------------- */

static void test_bare_words_are_sources_in_work(void) {
    touch("a.v");
    touch("src/b.SV");
    touch("c.vhd");
    touch("n.blif");
    expect_ok(read_list("a.v src/b.SV\n\tc.vhd   n.blif\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.SV\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"c.vhd\", \"language\": \"vhdl\", \"library\": \"work\"}, "
        "{\"path\": \"n.blif\", \"language\": \"blif\", \"library\": \"work\"}],",
        line_of("files"));
}

static void test_bare_word_unknown_file_type(void) {
    touch("notes.txt");
    expect_error(read_list("// c\n  notes.txt\n"),
                 "files.f:2:3: error: unknown_file_type: no language for file: 'notes.txt'");
}

static void test_bare_word_missing_file(void) {
    expect_error_in(read_list("src/missing.v\n"),
                    "files.f:1:1: error: missing_file: file not found: '%D/src/missing.v'");
}

static void test_a_file_listed_twice_is_a_duplicate(void) {
    touch("a.v");
    expect_ok(read_list("a.v\n./a.v\n"));
    TEST_ASSERT_EQUAL_STRING(k_a_file, line_of("files"));
    TEST_ASSERT_EQUAL_STRING("  \"duplicates\": [\"a.v\"]", line_of("duplicates"));
}

/* --- options with an argument -------------------------------------------------------------- */

static void test_v_adds_a_libfile(void) {
    touch("lib/p.v");
    touch("lib/q.vhd");
    expect_ok(read_list("-v lib/p.v\n-v\nlib/q.vhd\n"));
    TEST_ASSERT_EQUAL_STRING("  \"libfiles\": [{\"path\": \"lib/p.v\", \"language\": \"verilog\"}, "
                             "{\"path\": \"lib/q.vhd\", \"language\": \"vhdl\"}],",
                             line_of("libfiles"));
    TEST_ASSERT_EQUAL_STRING("  \"files\": [],", line_of("files"));
}

static void test_v_unknown_file_type_at_the_option(void) {
    touch("lib/p.txt");
    expect_error(read_list("\t-v\n lib/p.txt\n"),
                 "files.f:1:2: error: unknown_file_type: no language for file: 'lib/p.txt'");
}

static void test_v_missing_file(void) {
    expect_error_in(read_list("-v lib/none.v\n"),
                    "files.f:1:1: error: missing_file: file not found: '%D/lib/none.v'");
}

static void test_y_adds_a_libdir(void) {
    touch("lib/cells/c.v");
    expect_ok(read_list("-y lib/cells\n-y lib/cells/../cells\n"));
    TEST_ASSERT_EQUAL_STRING("  \"libdirs\": [\"lib/cells\", \"lib/cells\"],", line_of("libdirs"));
}

static void test_y_wants_a_directory(void) {
    touch("lib/c.v");
    expect_error_in(read_list("-y lib/c.v\n"),
                    "files.f:1:1: error: missing_file: directory not found: '%D/lib/c.v'");
}

static void test_top_and_top_module(void) {
    expect_ok(read_list("-top top\n"));
    TEST_ASSERT_EQUAL_STRING("  \"top\": \"top\",", line_of("top"));
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    expect_ok(read_list("--top-module lib.core\n"));
    TEST_ASSERT_EQUAL_STRING("  \"top\": \"lib.core\",", line_of("top"));
}

static void test_top_errors(void) {
    expect_error(read_list("-top 1x\n"), "files.f:1:1: error: syntax: bad top name: '1x'");
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    errors = 0;
    expect_error(read_list("-top a\n--top-module b\n"),
                 "files.f:2:1: error: syntax: top given twice (first a): 'b'");
}

static void test_option_at_the_end_needs_an_argument(void) {
    expect_error(read_list("// x\n  -v  # nothing follows\n"),
                 "files.f:2:3: error: syntax: option needs an argument: '-v'");
}

static void test_quoted_option_is_the_option(void) {
    touch("lib/p.v");
    expect_ok(read_list("\"-v\" \"lib/p.v\"\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"libfiles\": [{\"path\": \"lib/p.v\", \"language\": \"verilog\"}],",
        line_of("libfiles"));
}

/* --- plus options -------------------------------------------------------------------------- */

static void test_incdir_splits_at_plus(void) {
    touch("a/x.vh");
    touch("b/y.vh");
    expect_ok(read_list("+incdir+a++b+\n"));
    TEST_ASSERT_EQUAL_STRING("  \"incdirs\": [\"a\", \"b\"],", line_of("incdirs"));
}

static void test_incdir_missing_directory(void) {
    touch("a/x.vh");
    expect_error_in(read_list("+incdir+a+nope\n"),
                    "files.f:1:1: error: missing_file: directory not found: '%D/nope'");
}

static void test_define_splits_at_plus(void) {
    expect_ok(read_list("+define+USE+W=12+E=\n+define+TAG=\"AB\"\n"));
    TEST_ASSERT_EQUAL_STRING("  \"defines\": [{\"name\": \"USE\", \"value\": null}, "
                             "{\"name\": \"W\", \"value\": \"12\"}, "
                             "{\"name\": \"E\", \"value\": \"\"}, "
                             "{\"name\": \"TAG\", \"value\": \"\\\"AB\\\"\"}],",
                             line_of("defines"));
}

static void test_define_bad_name(void) {
    expect_error(read_list("+define+OK+1X=2\n"),
                 "files.f:1:1: error: syntax: bad macro name: '1X=2'");
}

static void test_libext_splits_at_plus(void) {
    expect_ok(read_list("+libext+.v+.sv\n"));
    TEST_ASSERT_EQUAL_STRING("  \"libext\": [\".v\", \".sv\"],", line_of("libext"));
}

static void test_libext_unknown_file_type(void) {
    expect_error(read_list("+libext+.v+.foo\n"),
                 "files.f:1:1: error: unknown_file_type: no language for extension: '.foo'");
}

static void test_plus_option_without_a_value(void) {
    expect_error(read_list("+define+\n"),
                 "files.f:1:1: error: syntax: option has no value: '+define+'");
    errors = 0;
    expect_error(read_list("+incdir++\n"),
                 "files.f:1:1: error: syntax: option has no value: '+incdir++'");
    errors = 0;
    expect_error(read_list("+libext\n"),
                 "files.f:1:1: error: syntax: option has no value: '+libext'");
}

static void test_unknown_plus_option(void) {
    expect_error(read_list("+foo+x\n"),
                 "files.f:1:1: error: unknown_option: unknown option: '+foo+x'");
    errors = 0;
    expect_error(read_list("+\n"), "files.f:1:1: error: unknown_option: unknown option: '+'");
}

static void test_unknown_dash_option(void) {
    expect_error(read_list("-q\n"), "files.f:1:1: error: unknown_option: unknown option: '-q'");
    errors = 0;
    expect_error(read_list("-\n"), "files.f:1:1: error: unknown_option: unknown option: '-'");
}

/* --- -sv ----------------------------------------------------------------------------------- */

static void test_sv_makes_later_verilog_systemverilog(void) {
    touch("a.v");
    touch("b.v");
    touch("c.vhd");
    touch("d.sv");
    expect_ok(read_list("a.v\n-sv\nb.v c.vhd d.sv\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"b.v\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"c.vhd\", \"language\": \"vhdl\", \"library\": \"work\"}, "
        "{\"path\": \"d.sv\", \"language\": \"systemverilog\", \"library\": \"work\"}],",
        line_of("files"));
}

/* --- $VAR ---------------------------------------------------------------------------------- */

static void test_var_after_a_comment_never_errors(void) {
    touch("a.v");
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_UNSET"));
    expect_ok(read_list("a.v # $ODIN3_T_UNSET\n// $ODIN3_T_UNSET $ bad\n"));
    TEST_ASSERT_EQUAL_STRING(k_a_file, line_of("files"));
}

static void test_var_in_an_argument_is_located_at_its_line(void) {
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_UNSET"));
    expect_error(read_list("-v\n  $ODIN3_T_UNSET/p.v\n"),
                 "files.f:2:3: error: undefined_variable: environment variable not set: "
                 "'ODIN3_T_UNSET'");
}

static void test_var_forms(void) {
    touch("src/a.v");
    touch("src/b.v");
    touch("src/c.v");
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_RTL", "src", 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T$X", "src", 1));
    expect_ok(read_list("$ODIN3_T_RTL/a.v\n${ODIN3_T_RTL}/b.v\n$ODIN3_T$X/c.v\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/c.v\", \"language\": \"verilog\", \"library\": \"work\"}],",
        line_of("files"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_RTL"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T$X"));
}

static void test_var_name_includes_dollar(void) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_A", "src", 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_A$B"));
    expect_error(read_list("$ODIN3_T_A$B/a.v\n"),
                 "files.f:1:1: error: undefined_variable: environment variable not set: "
                 "'ODIN3_T_A$B'");
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_A"));
}

static void test_stray_dollar_is_syntax(void) {
    expect_error(read_list("$\n"), "files.f:1:1: error: syntax: bad variable reference: '$'");
    errors = 0;
    expect_error(read_list("  a$\n"), "files.f:1:3: error: syntax: bad variable reference: 'a$'");
    errors = 0;
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_RTL", "src", 1));
    expect_error(read_list("${ODIN3_T_RTL/a.v\n"),
                 "files.f:1:1: error: syntax: bad variable reference: '${ODIN3_T_RTL/a.v'");
    errors = 0;
    expect_error(read_list("$1/a.v\n"), "files.f:1:1: error: syntax: bad variable reference: "
                                        "'$1/a.v'");
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_RTL"));
}

static void test_undefined_variable_before_a_stray_dollar(void) {
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_UNSET"));
    expect_error(read_list("$$ODIN3_T_UNSET\n"),
                 "files.f:1:1: error: undefined_variable: environment variable not set: "
                 "'ODIN3_T_UNSET'");
}

static void test_a_value_may_contain_dollar(void) {
    touch("x$y.v");
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_D", "$", 1));
    expect_ok(read_list("x${ODIN3_T_D}y.v\n"));
    TEST_ASSERT_EQUAL_STRING("  \"files\": [{\"path\": \"x$y.v\", \"language\": \"verilog\", "
                             "\"library\": \"work\"}],",
                             line_of("files"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_D"));
}

static void test_a_variable_may_name_an_option(void) {
    touch("lib/p.v");
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_OPT", "-v", 1));
    expect_ok(read_list("$ODIN3_T_OPT lib/p.v\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"libfiles\": [{\"path\": \"lib/p.v\", \"language\": \"verilog\"}],",
        line_of("libfiles"));
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_OPT", "", 1));
    errors = 0;
    expect_error(read_list("$ODIN3_T_OPT\n"),
                 "files.f:1:1: error: unknown_file_type: no language for file: ''");
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_OPT"));
}

/* --- nested lists -------------------------------------------------------------------------- */

static void test_sv_scoping_and_resolution_bases(void) {
    char dir[PATH_BUF];
    expect_ok(read_in(data_dir("nest", dir), "files.f"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/d.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/e.v\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/h.v\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/g.vhd\", \"language\": \"vhdl\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"lists/src/i.v\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/j.v\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/c.v\", \"language\": \"verilog\", \"library\": \"work\"}],",
        line_of("files"));
}

static void test_cycle_names_the_chain(void) {
    char dir[PATH_BUF];
    expect_error_in(read_in(data_dir("cycle", dir), "a.f"),
                    "c.f:2:1: error: f_cycle: file list cycle: '%D/b.f', '%D/c.f', '%D/b.f'\n"
                    "  included from b.f:1\n"
                    "  included from a.f:1");
}

static void test_a_list_naming_itself_is_a_cycle(void) {
    expect_error_in(read_list("-F ./files.f\n"),
                    "files.f:1:1: error: f_cycle: file list cycle: '%D/files.f', '%D/files.f'");
}

static void test_diamond_is_a_duplicate_listing(void) {
    char dir[PATH_BUF];
    expect_ok(read_in(data_dir("diamond", dir), "files.f"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"src/shared.v\", \"language\": \"verilog\", \"library\": "
        "\"work\"}, {\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/top.v\", \"language\": \"verilog\", \"library\": \"work\"}],",
        line_of("files"));
    TEST_ASSERT_EQUAL_STRING("  \"duplicates\": [\"src/shared.v\"]", line_of("duplicates"));
}

static void test_nested_list_must_exist_at_the_option(void) {
    expect_error_in(read_list("\n  -f lists/none.f\n"),
                    "files.f:2:3: error: missing_file: file not found: '%D/lists/none.f'");
}

static void test_error_in_a_nested_list_prints_the_chain(void) {
    char dir[PATH_BUF];
    expect_error(read_in(data_dir("nested_error", dir), "files.f"),
                 "lists/bad.f:1:1: error: unknown_option: unknown option: '-q'\n"
                 "  included from files.f:3");
}

static void test_F_argument_and_entries_resolve_against_the_list(void) {
    put("lists/inner.f", odin3_bytes_cstr("src/x.v\n-F deeper/d.f\n"));
    touch("lists/src/x.v");
    put("lists/deeper/d.f", odin3_bytes_cstr("y.v\n"));
    touch("lists/deeper/y.v");
    expect_ok(read_list("-F lists/inner.f\n"));
    TEST_ASSERT_EQUAL_STRING(
        "  \"files\": [{\"path\": \"lists/src/x.v\", \"language\": \"verilog\", \"library\": "
        "\"work\"}, {\"path\": \"lists/deeper/y.v\", \"language\": \"verilog\", \"library\": "
        "\"work\"}],",
        line_of("files"));
}

/* --- words --------------------------------------------------------------------------------- */

static void test_crlf_list(void) {
    touch("src/top.v");
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_T_RTL", "src", 1));
    expect_ok(read_list("// crlf\r\n-top top\r\n+libext+.v\r\n$ODIN3_T_RTL/top.v\r\n"));
    TEST_ASSERT_EQUAL_STRING("  \"top\": \"top\",", line_of("top"));
    TEST_ASSERT_EQUAL_STRING("  \"libext\": [\".v\"],", line_of("libext"));
    TEST_ASSERT_EQUAL_STRING("  \"files\": [{\"path\": \"src/top.v\", \"language\": \"verilog\", "
                             "\"library\": \"work\"}],",
                             line_of("files"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_T_RTL"));
}

static void test_word_errors_come_before_any_word_is_used(void) {
    expect_error(read_list("missing.v\n  ok.v \"unterminated\n"),
                 "files.f:2:8: error: syntax: unterminated quote");
}

static void test_quote_inside_a_bare_word_is_ordinary(void) {
    touch("a\"b.v");
    expect_ok(read_list("a\"b.v\n"));
    TEST_ASSERT_EQUAL_STRING("  \"files\": [{\"path\": \"a\\\"b.v\", \"language\": \"verilog\", "
                             "\"library\": \"work\"}],",
                             line_of("files"));
}

/* --- caps ---------------------------------------------------------------------------------- */

static void test_depth_cap(void) {
    put("one.f", odin3_bytes_cstr("-f two.f\n"));
    put("two.f", odin3_bytes_cstr("// deeper\n-f three.f\n"));
    touch("three.f");
    odin3_project_test_set_limits(proj, (odin3_proj_limits){3, ODIN3_PROJ_MAX_QIP_DEPTH,
                                                            ODIN3_PROJ_MAX_WORDS,
                                                            ODIN3_PROJ_MAX_XML_NODES});
    put("files.f", odin3_bytes_cstr("-f one.f\n"));
    expect_ok(read_in(tmp, "files.f")); /* three.f is nested 3 deep */
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    odin3_project_test_set_limits(proj, (odin3_proj_limits){2, ODIN3_PROJ_MAX_QIP_DEPTH,
                                                            ODIN3_PROJ_MAX_WORDS,
                                                            ODIN3_PROJ_MAX_XML_NODES});
    expect_error(read_in(tmp, "files.f"),
                 "two.f:2:1: error: syntax: file lists nested more than 2 deep "
                 "(ODIN3_PROJ_MAX_FLIST_DEPTH): 'three.f'\n"
                 "  included from one.f:1\n"
                 "  included from files.f:1");
}

static void test_word_cap(void) {
    touch("a.v");
    odin3_project_test_set_limits(proj, (odin3_proj_limits){ODIN3_PROJ_MAX_FLIST_DEPTH,
                                                            ODIN3_PROJ_MAX_QIP_DEPTH, 3,
                                                            ODIN3_PROJ_MAX_XML_NODES});
    put("files.f", odin3_bytes_cstr("a.v a.v # x y z\n\"a.v\" // q r\n"));
    expect_ok(read_in(tmp, "files.f")); /* exactly 3 words */
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    odin3_project_test_set_limits(proj, (odin3_proj_limits){ODIN3_PROJ_MAX_FLIST_DEPTH,
                                                            ODIN3_PROJ_MAX_QIP_DEPTH, 3,
                                                            ODIN3_PROJ_MAX_XML_NODES});
    expect_error(read_list("a.v\n\"a b\" c.v  d.v e.v\n"),
                 "files.f:2:12: error: syntax: file list has more than 3 words "
                 "(ODIN3_PROJ_MAX_WORDS)");
}

/* --- the corpus ---------------------------------------------------------------------------- */

/* One corpus case with an -f entry: NULL error = the record must equal expected.json's
 * "project"; a design-level negative (2C/2D) reads without error and has no "project". */
typedef struct corpus_case {
    const char *name;
    const char *entry;
    const char *error; /* "%D" = the case directory */
    bool design_error;
} corpus_case;

static const corpus_case k_corpus[] = {
    {"auto_top", "files.f", NULL, false},
    {"blif_netlist", "files.f", NULL, false},
    {"defines", "files.f", NULL, false},
    {"f_diamond", "files.f", NULL, false},
    {"f_env_crlf", "files.f", NULL, false},
    {"f_options", "files.f", NULL, false},
    {"f_vs_F", "files.f", NULL, false},
    {"include_dirs", "files.f", NULL, false},
    {"include_order", "files.f", NULL, false},
    {"lib_v_y", "files.f", NULL, false},
    {"mixed_verilog_top", "files.f", NULL, false},
    {"mixed_vhdl_top", "files.f", NULL, false},
    {"multi_dir", "files.f", NULL, false},
    {"nested_f", "files.f", NULL, false},
    {"relative_cwd", "proj/files.f", NULL, false},
    {"single_file", "files.f", NULL, false},
    {"sv_package", "files.f", NULL, false},
    {"vhdl_dep_order", "files.f", NULL, false},
    {"vhdl_split_units", "files.f", NULL, false},
    {"vhdl_work", "files.f", NULL, false},
    {"neg_ambiguous_top", "files.f", NULL, true},
    {"neg_dependency_cycle", "files.f", NULL, true},
    {"neg_duplicate_module", "files.f", NULL, true},
    {"neg_include_cycle", "files.f", NULL, true},
    {"neg_include_not_found", "files.f", NULL, true},
    {"neg_no_top", "files.f", NULL, true},
    {"neg_unknown_top", "files.f", NULL, true},
    {"neg_unresolved_module", "files.f", NULL, true},
    {"neg_f_cycle", "files.f",
     "lists/more.f:2:1: error: f_cycle: file list cycle: '%D/files.f', '%D/lists/more.f', "
     "'%D/files.f'\n  included from files.f:3",
     false},
    {"neg_missing_file", "files.f",
     "files.f:2:1: error: missing_file: file not found: '%D/src/missing.v'", false},
    {"neg_syntax", "files.f", "files.f:2:1: error: syntax: unterminated quote", false},
    {"neg_undefined_variable", "files.f",
     "files.f:2:1: error: undefined_variable: environment variable not set: 'NOPE'", false},
    {"neg_unknown_file_type", "files.f",
     "files.f:2:1: error: unknown_file_type: no language for file: 'src/notes.txt'", false},
    {"neg_unknown_option", "files.f",
     "files.f:2:1: error: unknown_option: unknown option: '-bogus'", false},
};

/* A case's expected.json, NUL-terminated. */
static void read_expected(const char *case_dir, odin3_strbuf *out) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/expected.json", case_dir);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(path, FIXTURE_MAX, &bytes));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){bytes.data, bytes.len}));
    odin3_file_bytes_free(&bytes);
}

/* The "project" object of a case's expected.json, re-indented as a top-level dump. */
static void corpus_project(const char *case_dir, odin3_strbuf *out) {
    odin3_strbuf json;
    odin3_strbuf_init(&json);
    read_expected(case_dir, &json);
    const char *open = strstr(json.data, "\n  \"project\": {\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(open, case_dir);
    const char *at = open + strlen("\n  \"project\": {\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("{\n")));
    while (strncmp(at, "  }", 3) != 0) {
        const char *end = strchr(at, '\n');
        TEST_ASSERT_NOT_NULL(end);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at + 2, (size_t)(end - at) - 1}));
        at = end + 1;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("}\n")));
    odin3_strbuf_free(&json);
}

/* json without the blanks and newlines outside strings (expected.json wraps long arrays). */
static void compact(const char *json, odin3_strbuf *out) {
    bool in_string = false;
    for (const char *at = json; *at != '\0'; at++) {
        bool blank = *at == ' ' || *at == '\n';
        if (in_string || !blank) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at, 1}));
        }
        if (in_string && *at == '\\') {
            at++;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at, 1}));
        } else if (*at == '"') {
            in_string = !in_string;
        }
    }
}

static bool has_project(const char *case_dir) {
    odin3_strbuf json;
    odin3_strbuf_init(&json);
    read_expected(case_dir, &json);
    bool found = strstr(json.data, "\n  \"project\": {\n") != NULL;
    odin3_strbuf_free(&json);
    return found;
}

static void corpus_one(const corpus_case *one) {
    char dir[PATH_BUF];
    (void)snprintf(dir, sizeof dir, "%s/%s", ODIN3_PROJECT_FIXTURES, one->name);
    odin3_strbuf norm;
    odin3_strbuf_init(&norm);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_path_normalize(odin3_bytes_cstr(dir), &norm));
    odin3_design_drop_project(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    captured[0] = '\0';
    errors = 0;
    odin3_status st = read_in(norm.data, one->entry);
    if (one->error != NULL) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(with_dir(one->error), captured, one->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, st, one->name);
    } else {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", captured, one->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, one->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(!one->design_error, has_project(norm.data), one->name);
        if (!one->design_error) {
            odin3_strbuf want;
            odin3_strbuf got;
            odin3_strbuf want_compact;
            odin3_strbuf_init(&want);
            odin3_strbuf_init(&got);
            odin3_strbuf_init(&want_compact);
            corpus_project(norm.data, &want);
            compact(want.data, &want_compact);
            compact(dump_rel(), &got);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(want_compact.data, got.data, one->name);
            odin3_strbuf_free(&want);
            odin3_strbuf_free(&got);
            odin3_strbuf_free(&want_compact);
        }
    }
    odin3_strbuf_free(&norm);
}

static bool in_corpus_table(const char *name) {
    for (size_t i = 0; i < sizeof k_corpus / sizeof k_corpus[0]; i++) {
        if (strcmp(k_corpus[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether the case directory name has a top-level files.f. */
static bool has_list(const char *name) {
    if (name[0] == '.') {
        return false;
    }
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(&path, "%s/%s/files.f", ODIN3_PROJECT_FIXTURES, name));
    struct stat st;
    bool found = stat(path.data, &st) == 0;
    odin3_strbuf_free(&path);
    return found;
}

/* Every case directory with a top-level files.f is in the table. */
static void test_corpus_table_is_complete(void) {
    DIR *cases = opendir(ODIN3_PROJECT_FIXTURES);
    TEST_ASSERT_NOT_NULL(cases);
    size_t seen = 0;
    for (struct dirent *ent = readdir(cases); ent != NULL; ent = readdir(cases)) {
        if (has_list(ent->d_name)) {
            TEST_ASSERT_TRUE_MESSAGE(in_corpus_table(ent->d_name), ent->d_name);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, closedir(cases));
    TEST_ASSERT_EQUAL_size_t(sizeof k_corpus / sizeof k_corpus[0] - 1, seen); /* + relative_cwd */
}

static void test_corpus_f_cases(void) {
    TEST_ASSERT_EQUAL_INT(0, unsetenv("NOPE"));
    TEST_ASSERT_EQUAL_INT(0, setenv("RTL", "src", 1)); /* f_env_crlf's env */
    for (size_t i = 0; i < sizeof k_corpus / sizeof k_corpus[0]; i++) {
        corpus_one(&k_corpus[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, unsetenv("RTL"));
}

/* --- out of memory ------------------------------------------------------------------------- */

static char oom_dir[PATH_BUF / 2];
static char oom_want[MSG_MAX];

/* One try on a fresh design with the fail_after-th allocation failing; on failure the status is
 * NO_MEMORY, nothing is logged, and the record is dropped. True when the read succeeded. */
static bool oom_try(long fail_after) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/files.f", oom_dir);
    odin3_proj_path entry = {id("files.f"), id(path), {0}};
    (void)snprintf(rel_dir, sizeof rel_dir, "%s", oom_dir);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_read_flist(proj, &entry);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_STRING(oom_want, line_of("files"));
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    odin3_design_drop_project(design);
    TEST_ASSERT_NULL(odin3_design_project(design));
    return false;
}

static void test_out_of_memory(void) {
    odin3_strbuf norm;
    odin3_strbuf_init(&norm);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_path_normalize(odin3_bytes_cstr(ODIN3_FLIST_DATA "/nest"), &norm));
    (void)snprintf(oom_dir, sizeof oom_dir, "%s", norm.data);
    odin3_strbuf_free(&norm);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_in(oom_dir, "files.f"));
    (void)snprintf(oom_want, sizeof oom_want, "%s", line_of("files"));
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 50); /* the sweep really hit many allocation points */
    TEST_ASSERT_EQUAL_INT(0, errors);
}

int main(void) {
    for (size_t i = 0; i < sizeof k_corpus_vars / sizeof k_corpus_vars[0]; i++) {
        const char *value = getenv(k_corpus_vars[i]);
        saved_vars[i] = value != NULL ? strdup(value) : NULL;
    }
    UNITY_BEGIN();
    RUN_TEST(test_bare_words_are_sources_in_work);
    RUN_TEST(test_bare_word_unknown_file_type);
    RUN_TEST(test_bare_word_missing_file);
    RUN_TEST(test_a_file_listed_twice_is_a_duplicate);
    RUN_TEST(test_v_adds_a_libfile);
    RUN_TEST(test_v_unknown_file_type_at_the_option);
    RUN_TEST(test_v_missing_file);
    RUN_TEST(test_y_adds_a_libdir);
    RUN_TEST(test_y_wants_a_directory);
    RUN_TEST(test_top_and_top_module);
    RUN_TEST(test_top_errors);
    RUN_TEST(test_option_at_the_end_needs_an_argument);
    RUN_TEST(test_quoted_option_is_the_option);
    RUN_TEST(test_incdir_splits_at_plus);
    RUN_TEST(test_incdir_missing_directory);
    RUN_TEST(test_define_splits_at_plus);
    RUN_TEST(test_define_bad_name);
    RUN_TEST(test_libext_splits_at_plus);
    RUN_TEST(test_libext_unknown_file_type);
    RUN_TEST(test_plus_option_without_a_value);
    RUN_TEST(test_unknown_plus_option);
    RUN_TEST(test_unknown_dash_option);
    RUN_TEST(test_sv_makes_later_verilog_systemverilog);
    RUN_TEST(test_var_after_a_comment_never_errors);
    RUN_TEST(test_var_in_an_argument_is_located_at_its_line);
    RUN_TEST(test_var_forms);
    RUN_TEST(test_var_name_includes_dollar);
    RUN_TEST(test_stray_dollar_is_syntax);
    RUN_TEST(test_undefined_variable_before_a_stray_dollar);
    RUN_TEST(test_a_value_may_contain_dollar);
    RUN_TEST(test_a_variable_may_name_an_option);
    RUN_TEST(test_sv_scoping_and_resolution_bases);
    RUN_TEST(test_cycle_names_the_chain);
    RUN_TEST(test_a_list_naming_itself_is_a_cycle);
    RUN_TEST(test_diamond_is_a_duplicate_listing);
    RUN_TEST(test_nested_list_must_exist_at_the_option);
    RUN_TEST(test_error_in_a_nested_list_prints_the_chain);
    RUN_TEST(test_F_argument_and_entries_resolve_against_the_list);
    RUN_TEST(test_crlf_list);
    RUN_TEST(test_word_errors_come_before_any_word_is_used);
    RUN_TEST(test_quote_inside_a_bare_word_is_ordinary);
    RUN_TEST(test_depth_cap);
    RUN_TEST(test_word_cap);
    RUN_TEST(test_corpus_table_is_complete);
    RUN_TEST(test_corpus_f_cases);
    RUN_TEST(test_out_of_memory);
    int failures = UNITY_END();
    for (size_t i = 0; i < sizeof k_corpus_vars / sizeof k_corpus_vars[0]; i++) {
        free(saved_vars[i]);
    }
    return failures;
}
