/*
 * test_project_quartus.c — unit tests for the Quartus reader (PRJ-12..14) over the trees in
 * tests/unit/data/project/quartus: every command and assignment row with its record, every error
 * kind with its exact message and location, the §10.1 Tcl quirks through a .qsf, the .qip idiom
 * in both forms, revision selection, anchored scopes, the qip depth cap through the test hook,
 * and an OOM sweep with the record dropped. Each tree also matches the oracle (scratch harness,
 * task-4 report).
 */
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "frontends/project/project_test.h"
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
#include <unistd.h>

#ifndef ODIN3_QUARTUS_DATA
#error "ODIN3_QUARTUS_DATA must name tests/unit/data/project/quartus"
#endif

enum { PATH_BUF = 1024, MSG_MAX = 4096, OUT_MAX = 8192, OOM_LIMIT = 2000 };

static odin3_design *design;
static odin3_project *proj;
static char captured[MSG_MAX];
static int errors;
static char out[OUT_MAX];
static char tmp[PATH_BUF / 2];
static char tmp_file[PATH_BUF];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    errors = 0;
    tmp[0] = '\0';
    tmp_file[0] = '\0';
    design = NULL;
    fresh_design();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    if (tmp_file[0] != '\0') {
        TEST_ASSERT_EQUAL_INT(0, unlink(tmp_file));
    }
    if (tmp[0] != '\0') {
        TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    }
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

/* Copies text into out with every occurrence of from replaced by to. */
static const char *replace(const char *text, const char *from, const char *to) {
    size_t len = 0;
    size_t from_len = strlen(from);
    while (*text != '\0' && len + strlen(to) + 1 < sizeof out) {
        if (strncmp(text, from, from_len) == 0) {
            len += (size_t)snprintf(out + len, sizeof out - len, "%s", to);
            text += from_len;
        } else {
            out[len++] = *text++;
        }
    }
    out[len] = '\0';
    return out;
}

/* Reads the entry rel (given as rel, resolved under the data tree) with a wanted revision. */
static odin3_status read_req(const char *rel, const odin3_quartus_req *req) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_QUARTUS_DATA, rel);
    odin3_proj_path file = {id(rel), id(path), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_quartus(proj, &file, req);
}

static const odin3_quartus_req k_none = {{NULL, 0}, false, {0}};

static odin3_status read_entry(const char *rel) {
    return read_req(rel, &k_none);
}

/* The record dumped after finish, data-tree paths relative ("src/a.v"). */
static const char *dump(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &text));
    replace(text.data, ODIN3_QUARTUS_DATA "/", "");
    odin3_strbuf_free(&text);
    return out;
}

/* The dump's line for key, without the indent and the trailing comma. */
static const char *field(const char *key) {
    static char line[OUT_MAX];
    char head[PATH_BUF];
    (void)snprintf(head, sizeof head, "\n  \"%s\": ", key);
    const char *at = strstr(dump(), head);
    TEST_ASSERT_NOT_NULL(at);
    at += strlen(head);
    size_t len = strcspn(at, "\n");
    len -= len > 0 && at[len - 1] == ',';
    (void)snprintf(line, sizeof line, "%.*s", (int)len, at);
    return line;
}

/* Reading rel (wanted revision, or NULL) fails with message; "$D" stands for the data tree. */
static void expect_error_req(const char *rel, const odin3_quartus_req *req, const char *message) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_req(rel, req));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(message, replace(captured, ODIN3_QUARTUS_DATA, "$D"));
}

static void expect_error(const char *rel, const char *message) {
    expect_error_req(rel, &k_none, message);
}

/* --- records ------------------------------------------------------------------------------- */

static void test_design_files_and_ignored(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("basic/top.qpf"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.sv\", \"language\": \"systemverilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/c.vhd\", \"language\": \"vhdl\", \"library\": \"mylib\"}, "
        "{\"path\": \"src/d.vqm\", \"language\": \"vqm\", \"library\": \"work\"}, "
        "{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"other\"}]",
        field("files"));
    TEST_ASSERT_EQUAL_STRING("[\"src/a.v\"]", field("duplicates"));
    TEST_ASSERT_EQUAL_STRING("[\"-hdl_version\", \"IO_STANDARD\", \"LOCATION\", \"SDC_FILE\", "
                             "\"VERILOG_INCLUDE_FILE\", \"post_message\"]",
                             field("ignored"));
    TEST_ASSERT_EQUAL_STRING("\"top\"", field("top"));
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_TRUE(proj->top_at.v != 0); /* TOP_LEVEL_ENTITY's location */
}

static void test_directories_defines_device(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("dirs/dirs.qsf"));
    TEST_ASSERT_EQUAL_STRING("[\"inc\", \"lib\"]", field("incdirs"));
    TEST_ASSERT_EQUAL_STRING("[\"inc\", \"lib\", \"lib\"]", field("libdirs"));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"X\", \"value\": \"\"}, {\"name\": \"Y\", \"value\": "
                             "\"\\\"a b\\\"\"}, {\"name\": \"Z\", \"value\": null}]",
                             field("defines"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"kind\": \"device\", \"family\": \"Cyclone V\", \"device\": \"5CSEMA5F31C7\"}]",
        field("arch"));
    /* no TOP_LEVEL_ENTITY: the revision (the .qsf's stem) at the file as a whole */
    TEST_ASSERT_EQUAL_STRING("\"dirs\"", field("top"));
    TEST_ASSERT_EQUAL_UINT32(0, proj->top_at.v);
    TEST_ASSERT_EQUAL_UINT32(id("dirs/dirs.qsf"), proj->top_file.given);
}

/* The DEVICE entry is created at the first FAMILY/DEVICE, after the arch entries before it. */
static void test_device_entry_in_load_order(void) {
    odin3_proj_arch o3 = {.kind = ODIN3_PROJ_ARCH_O3LIB};
    o3.path = (odin3_proj_path){id("t.o3lib"), id("/p/t.o3lib"), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_add_arch(proj, &o3));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("dirs/family.qsf"));
    TEST_ASSERT_EQUAL_STRING("[{\"kind\": \"o3lib\", \"path\": \"/p/t.o3lib\"}, "
                             "{\"kind\": \"device\", \"family\": \"\", \"device\": null}]",
                             field("arch"));
}

static void test_mapping_rules(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("mapping/mapping.qsf"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"sub\", \"subject\": \"$mem\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"top.u_a.c\", \"subject\": \"$mul\", \"action\": "
        "\"soft\", \"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"e.a.c\", \"subject\": \"$mul\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"top.x\", \"subject\": \"$mem\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"top\", \"subject\": \"$mem\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"top..g\", \"subject\": \"$mul\", \"action\": \"soft\", "
        "\"cells\": []}]",
        field("rules"));
    /* defaults (ON, AUTO) make no rule; any other value is ignored, AUTO for AUTO_* too */
    TEST_ASSERT_EQUAL_STRING("[\"AUTO_DSP_RECOGNITION\", \"RAMSTYLE\"]", field("ignored"));
    TEST_ASSERT_EQUAL_STRING("\"lib.top\"", field("top"));
}

static void test_parameters(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("params/params.qsf"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"scope\": \"top\", \"name\": \"W\", \"value\": \"8\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"N\", \"value\": \"-1\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"S\", \"value\": \"hi there\", \"type\": \"string\"}, "
        "{\"scope\": \"top\", \"name\": \"T\", \"value\": \"abc\", \"type\": \"string\"}, "
        "{\"scope\": \"top\", \"name\": \"H\", \"value\": \"8'hFF\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"E\", \"value\": \"4\", \"type\": \"number\"}, "
        "{\"scope\": \"top.u1.u2\", \"name\": \"I\", \"value\": \"3\", \"type\": \"number\"}, "
        "{\"scope\": \"top.u\", \"name\": \"J\", \"value\": \"5\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"K\", \"value\": \"6\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"Q\", \"value\": \"q\", \"type\": \"string\"}, "
        "{\"scope\": \"top\", \"name\": \"U\", \"value\": \"1_0\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"V\", \"value\": \"'sb1x\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"X\", \"value\": \"-2_\", \"type\": \"number\"}]",
        field("params"));
}

/* QIP_FILE: a plain path against the project directory, the idiom (quoted and bare) against the
 * .qip; the .qip's commands run where it is named. */
static void test_qip_files(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("qip/top.qsf"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"qip/ip/a_ip.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"qip/ip/sub/b_ip.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}]",
        field("files"));
    TEST_ASSERT_EQUAL_STRING("[\"MISC_FILE\"]", field("ignored"));
}

static void test_revision_selection(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_req("revisions/proj.qpf", &(odin3_quartus_req){odin3_bytes_cstr("fast"), true, {0}}));
    TEST_ASSERT_EQUAL_STRING("\"fast\"", field("top"));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_req("revisions/proj.qpf", &(odin3_quartus_req){odin3_bytes_cstr("small"), true, {0}}));
    TEST_ASSERT_EQUAL_STRING("\"small\"", field("top"));
    fresh_design();
    /* no PROJECT_REVISION: the .qpf's stem */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("stem/stem.qpf"));
    TEST_ASSERT_EQUAL_STRING("\"stem\"", field("top"));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_req("stem/stem.qpf", &(odin3_quartus_req){odin3_bytes_cstr("stem"), true, {0}}));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        read_req("stem/stem.qsf", &(odin3_quartus_req){odin3_bytes_cstr("stem"), true, {0}}));
}

static void test_revision_errors(void) {
    expect_error("revisions/proj.qpf", "revisions/proj.qpf:4:1: error: ambiguous_revision: several "
                                       "revisions; choose one with --revision: 'small', 'fast'");
    expect_error_req("revisions/proj.qpf",
                     &(odin3_quartus_req){odin3_bytes_cstr("nope"), true, {0}},
                     "odin3: error: unknown_revision: no such revision: 'nope'");
    expect_error_req("revisions/proj.qpf", &(odin3_quartus_req){odin3_bytes_cstr(""), true, {0}},
                     "odin3: error: unknown_revision: no such revision: ''");
    expect_error_req("stem/stem.qsf", &(odin3_quartus_req){odin3_bytes_cstr("other"), true, {0}},
                     "odin3: error: unknown_revision: no such revision: 'other'");
    expect_error(
        "missing_qsf/top.qpf",
        "missing_qsf/top.qpf: error: missing_file: file not found: '$D/missing_qsf/gone.qsf'");
    expect_error("bad_qpf/top.qpf",
                 "bad_qpf/top.qpf:2:1: error: syntax: expected NAME = \"value\"");
}

/* `import … revision r`: the error is located at the import word, and the files read print it in
 * their include chain. */
static void test_revision_at_import_word(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_srcfile_spec spec = {.name = id("p.o3proj"), .resolved = id("/abs/p.o3proj"), .len = 30};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, buf, 10));
    odin3_quartus_req req = {odin3_bytes_cstr("nope"), true, odin3_srcman_loc(sm, buf, 10)};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_req("revisions/proj.qpf", &req));
    TEST_ASSERT_EQUAL_STRING("p.o3proj:2:1: error: unknown_revision: no such revision: 'nope'",
                             captured);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    spec = (odin3_srcfile_spec){.name = id("p.o3proj"), .resolved = id("/abs/p.o3proj"), .len = 30};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    req = (odin3_quartus_req){{NULL, 0}, false, odin3_srcman_loc(sm, buf, 3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_req("errors/option.qsf", &req));
    TEST_ASSERT_EQUAL_STRING("errors/option.qsf:1:1: error: unknown_option: option not allowed "
                             "here: '-to'\n  included from p.o3proj:1",
                             captured);
}

static void test_top_rules(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("top/last.qsf"));
    TEST_ASSERT_EQUAL_STRING("\"b\"", field("top")); /* the last one wins */
    fresh_design();
    /* TOP_LEVEL_ENTITY "" falls back to the revision, still located at the assignment */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("top/empty.qsf"));
    TEST_ASSERT_EQUAL_STRING("\"empty\"", field("top"));
    TEST_ASSERT_TRUE(proj->top_at.v != 0);
    fresh_design();
    expect_error("top/bad.qsf", "top/bad.qsf:1:1: error: syntax: bad top name: 'a b'");
    fresh_design();
    /* a .qsf default top that is not an identifier: a file-level error */
    expect_error("bad_top/my-proj.qsf",
                 "bad_top/my-proj.qsf: error: syntax: bad top name: 'my-proj'");
    fresh_design();
    /* an .o3proj top already set (import): the Quartus top is a second one */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_project_set_top(proj, odin3_bytes_cstr("x"), (odin3_loc){0}));
    expect_error("top/last.qsf", "top/last.qsf:2:1: error: syntax: top given twice (first x): 'b'");
}

/* --- errors -------------------------------------------------------------------------------- */

static void test_command_errors(void) {
    static const char *const cases[][2] = {
        {"errors/option.qsf",
         "errors/option.qsf:1:1: error: unknown_option: option not allowed here: '-to'"},
        {"errors/flag.qsf",
         "errors/flag.qsf:1:1: error: unknown_option: option not allowed here: '-disable'"},
        {"errors/single_option.qsf",
         "errors/single_option.qsf:1:1: error: unknown_option: option not allowed here: '-entity'"},
        {"errors/rule_option.qsf",
         "errors/rule_option.qsf:1:1: error: unknown_option: option not allowed here: '-library'"},
        {"errors/ignored_option.qsf", "errors/ignored_option.qsf:1:1: error: unknown_option: "
                                      "option not allowed here: '-bogus'"},
        {"errors/needs_value.qsf",
         "errors/needs_value.qsf:1:1: error: syntax: option needs a value: '-library'"},
        {"errors/two_values.qsf",
         "errors/two_values.qsf:1:1: error: syntax: expected one value, got 2"},
        {"errors/no_value.qsf",
         "errors/no_value.qsf:1:1: error: syntax: expected one value, got 0"},
        {"errors/no_name.qsf",
         "errors/no_name.qsf:1:1: error: syntax: an assignment needs one -name"},
        {"errors/two_names.qsf",
         "errors/two_names.qsf:1:1: error: syntax: an assignment needs one -name"},
        {"errors/param_name.qsf",
         "errors/param_name.qsf:1:1: error: syntax: set_parameter needs -name"},
        {"params/entity.qsf",
         "params/entity.qsf:2:1: error: syntax: set_parameter -entity must name the top: 'other'"},
        {"errors/macro.qsf", "errors/macro.qsf:1:1: error: syntax: bad macro name: '1X=2'"},
        {"errors/missing.qsf", "errors/missing.qsf:1:1: error: missing_file: file not found: "
                               "'$D/errors/nope.v'"},
        {"errors/missing_dir.qsf", "errors/missing_dir.qsf:1:1: error: missing_file: directory "
                                   "not found: '$D/errors/nope'"},
        {"errors/missing_qip.qsf", "errors/missing_qip.qsf:1:1: error: missing_file: file not "
                                   "found: '$D/errors/nope.qip'"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh_design();
        expect_error(cases[i][0], cases[i][1]);
    }
}

/* Word errors through a .qsf (§10.1): the whole file is tokenized first, so line 9's word error
 * wins over line 2's unknown option; an unterminated brace is at the EOF line; an escape before a
 * newline at the next line. */
static void test_tcl_errors_through_qsf(void) {
    expect_error("word_first/wf.qsf",
                 "word_first/wf.qsf:9:43: error: syntax: $ in a bare word is not supported");
    fresh_design();
    expect_error("errors/brace.qsf", "errors/brace.qsf:2:1: error: syntax: unterminated brace");
    fresh_design();
    expect_error("errors/escape.qsf",
                 "errors/escape.qsf:2:1: error: syntax: bad escape (only \\\" and \\\\)");
    fresh_design();
    expect_error("errors/bracket.qsf",
                 "errors/bracket.qsf:1:31: error: syntax: command substitution [...] is not "
                 "supported (only the .qip idiom [file join $::quartus(qip_path) \"<path>\"])");
}

static void test_qip_cycle_and_cap(void) {
    expect_error("qip_cycle/top.qsf",
                 "$D/qip_cycle/ip/b.qip:2:1: error: qip_cycle: QIP_FILE cycle: "
                 "'$D/qip_cycle/ip/a.qip', '$D/qip_cycle/ip/b.qip', '$D/qip_cycle/ip/a.qip'\n"
                 "  included from ip/a.qip:1\n"
                 "  included from qip_cycle/top.qsf:1");
    fresh_design();
    odin3_project_test_set_limits(
        proj, (odin3_proj_limits){ODIN3_PROJ_MAX_FLIST_DEPTH, 1, ODIN3_PROJ_MAX_WORDS});
    expect_error("qip/top.qsf", "ip/a.qip:2:1: error: syntax: nested .qip files exceed the depth "
                                "limit of 1: '$D/qip/ip/sub/b.qip'\n"
                                "  included from qip/top.qsf:2");
    fresh_design();
    odin3_project_test_set_limits(
        proj, (odin3_proj_limits){ODIN3_PROJ_MAX_FLIST_DEPTH, 2, ODIN3_PROJ_MAX_WORDS});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("qip/top.qsf"));
}

/* --- line endings and blanks (temporary files) --------------------------------------------- */

typedef struct tmp_spec {
    const char *name;
    const char *content;
    size_t len; /* 0: strlen(content) */
} tmp_spec;

static const char *write_tmp(const tmp_spec *spec) {
    const char *name = spec->name;
    const char *content = spec->content;
    size_t len = spec->len != 0 ? spec->len : strlen(content);
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_quartus_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    (void)snprintf(tmp_file, sizeof tmp_file, "%s/%s", tmp, name);
    FILE *fp = fopen(tmp_file, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(content, 1, len, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    return tmp_file;
}

static odin3_status read_tmp(const char *given) {
    odin3_proj_path file = {id(given), id(tmp_file), {0}};
    odin3_quartus_req req = {{NULL, 0}, false, {0}};
    return odin3_read_quartus(proj, &file, &req);
}

static void test_crlf_qsf(void) {
    tmp_spec spec = {"crlf.qsf",
                     "set_global_assignment -name FAMILY \\\r\n  {Cyclone\r\nV}\r\n"
                     "set_global_assignment -name TOP_LEVEL_ENTITY t;post\r\n",
                     0};
    write_tmp(&spec);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tmp("crlf.qsf"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"kind\": \"device\", \"family\": \"Cyclone\\nV\", \"device\": null}]", field("arch"));
    TEST_ASSERT_EQUAL_STRING("[\"post\"]", field("ignored"));
}

/* .qpf lines: Python whitespace (here a no-break space) around the name, '=' and value. */
static void test_qpf_blanks(void) {
    tmp_spec spec = {"p.qpf", "\xc2\xa0 \t# x\n\v\n PROJECT_REVISION\t=\xc2\xa0\"a b\" \t\r\n", 0};
    write_tmp(&spec);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_tmp("p.qpf"));
    /* the revision is chosen; its .qsf "a b.qsf" does not exist beside the .qpf */
    TEST_ASSERT_NOT_NULL(strstr(captured, "p.qpf: error: missing_file: file not found: '"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "/a b.qsf'"));
}

/* A NUL byte in a PROJECT_REVISION value: located `syntax` at its line (no strtab string holds
 * NUL, PRJ-4; a declared divergence — the oracle keeps the byte). */
static void test_qpf_nul_revision(void) {
    static const char text[] = "PROJECT_REVISION = \"a\0b\"\n";
    tmp_spec spec = {"n.qpf", text, sizeof text - 1};
    write_tmp(&spec);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_tmp("n.qpf"));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING("n.qpf:1:1: error: syntax: NUL byte in revision: 'a\\0b'", captured);
}

/* --- out of memory ------------------------------------------------------------------------- */

static bool oom_try(const char *entry, long fail_after) {
    fresh_design();
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_QUARTUS_DATA, entry);
    odin3_proj_path file = {id(entry), id(path), {0}};
    odin3_quartus_req req = {{NULL, 0}, false, {0}};
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_read_quartus(proj, &file, &req);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        odin3_design_drop_project(design);
    }
    return st == ODIN3_OK;
}

/* Reads entry with the k-th allocation failing, k = 0, 1, … until it succeeds. */
static void sweep(const char *entry) {
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(entry, k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 10);
}

static void test_out_of_memory(void) {
    static const char *const entries[] = {"basic/top.qpf", "qip/top.qsf", "mapping/mapping.qsf",
                                          "params/params.qsf", "dirs/dirs.qsf"};
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        sweep(entries[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_design_files_and_ignored);
    RUN_TEST(test_directories_defines_device);
    RUN_TEST(test_device_entry_in_load_order);
    RUN_TEST(test_mapping_rules);
    RUN_TEST(test_parameters);
    RUN_TEST(test_qip_files);
    RUN_TEST(test_revision_selection);
    RUN_TEST(test_revision_errors);
    RUN_TEST(test_revision_at_import_word);
    RUN_TEST(test_top_rules);
    RUN_TEST(test_command_errors);
    RUN_TEST(test_tcl_errors_through_qsf);
    RUN_TEST(test_qip_cycle_and_cap);
    RUN_TEST(test_crlf_qsf);
    RUN_TEST(test_qpf_blanks);
    RUN_TEST(test_qpf_nul_revision);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
