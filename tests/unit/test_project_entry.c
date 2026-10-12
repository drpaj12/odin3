/*
 * test_project_entry.c — unit tests for read_project's core (PRJ-16, odin3_project_read): format
 * by suffix and by request, a relative entry, a missing entry, .qip and .xml entries, --revision
 * misuse, a second read, a failed read dropping the record, --top replacing the project top with
 * its info line, the ignored and duplicates info lines, two corpus cases, and an OOM sweep.
 */
#include "frontends/project/project.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_O3PROJ_DATA
#error "ODIN3_O3PROJ_DATA must name tests/unit/data/project/o3proj"
#endif
#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

#define D ODIN3_O3PROJ_DATA

enum { PATH_BUF = 1024, MSG_MAX = 4096, INFO_MAX = 4, OOM_LIMIT = 4000, FIXTURE_MAX = 1 << 16 };

static odin3_design *design;
static char captured[MSG_MAX];
static int errors;
static char infos[INFO_MAX][MSG_MAX];
static int ninfos;
static char cwd[PATH_BUF];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    } else if (level == ODIN3_LOG_INFO && ninfos < INFO_MAX) {
        (void)snprintf(infos[ninfos++], MSG_MAX, "%s", msg);
    }
}

static void clear_logs(void) {
    errors = 0;
    captured[0] = '\0';
    ninfos = 0;
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    clear_logs();
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    design = NULL;
    fresh_design();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_status read_req(const odin3_project_request *req) {
    clear_logs();
    return odin3_project_read(design, req);
}

static odin3_status read_entry(const char *entry) {
    odin3_project_request req = {entry, 0, NULL, NULL, NULL};
    return read_req(&req);
}

static odin3_project *project(void) {
    odin3_project *proj = odin3_design_project(design);
    TEST_ASSERT_NOT_NULL(proj);
    return proj;
}

static const char *str(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
}

/* The failed read logged exactly message once, and dropped the record. */
static void expect_failure(odin3_status st, odin3_status want, const char *message) {
    TEST_ASSERT_EQUAL_INT(want, st);
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(message, captured);
    TEST_ASSERT_NULL(odin3_design_project(design));
}

/* --- format -------------------------------------------------------------------------------- */

static void test_format_by_suffix(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/all.o3proj"));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_O3PROJ, project()->format);
    TEST_ASSERT_TRUE(project()->finished);
    TEST_ASSERT_EQUAL_STRING(D "/all.o3proj", str(project()->entry.given));
    TEST_ASSERT_EQUAL_STRING(D "/all.o3proj", str(project()->entry.resolved));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/q/top.qsf"));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_QUARTUS, project()->format);
    TEST_ASSERT_EQUAL_STRING("top", str(project()->top));
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/q/../q/proj.qpf.f"));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_FLIST, project()->format);
    TEST_ASSERT_EQUAL_STRING(D "/q/proj.qpf.f", str(project()->entry.resolved));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)project()->files.len);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(ODIN3_PROJECT_FIXTURES "/single_file/odin2.xml"));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_ODIN2, project()->format);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)project()->files.len);
    TEST_ASSERT_TRUE(project()->output.present);
    TEST_ASSERT_EQUAL_STRING(ODIN3_PROJECT_FIXTURES "/single_file/out/counter.blif",
                             str(project()->output.path));
    TEST_ASSERT_EQUAL_STRING("blif", str(project()->output.format));
}

/* --format overrides the suffix: a list named .o3proj. */
static void test_format_by_request(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_entry(D "/list.o3proj"));
    TEST_ASSERT_EQUAL_STRING(D "/list.o3proj:1:1: error: unknown_key: unknown key: 'src/a.v'",
                             captured);
    fresh_design();
    odin3_project_request req = {D "/list.o3proj", ODIN3_PROJ_FORMAT_FLIST, NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_PROJ_FORMAT_FLIST, project()->format);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)project()->files.len);
}

/* A relative entry resolves against the working directory; it stays as given. */
static void test_relative_entry(void) {
    TEST_ASSERT_EQUAL_INT(0, chdir(D "/q"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry("./../all.o3proj"));
    TEST_ASSERT_EQUAL_STRING("./../all.o3proj", str(project()->entry.given));
    TEST_ASSERT_EQUAL_STRING(D "/all.o3proj", str(project()->entry.resolved));
    TEST_ASSERT_EQUAL_STRING("./../all.o3proj: listed more than once, read once: " D "/src/a.v",
                             infos[0]);
}

static void test_entry_errors(void) {
    expect_failure(read_entry("nope.o3proj"), ODIN3_ERR_PARSE,
                   "missing_file: project not found: 'nope.o3proj'");
    expect_failure(read_entry(D "/q"), ODIN3_ERR_PARSE,
                   "missing_file: project not found: '" D "/q'");
    expect_failure(read_entry(D "/q/ip.qip"), ODIN3_ERR_PARSE,
                   "unknown_file_type: a .qip is not a project entry: '" D "/q/ip.qip'");
    /* .xml is an Odin II config (2B-4), as is any entry with format ODIN2 */
    expect_failure(read_entry(D "/k6.xml"), ODIN3_ERR_PARSE,
                   D "/k6.xml:1:1: error: syntax: root must be <config>: 'architecture'");
    odin3_project_request req = {D "/all.o3proj", ODIN3_PROJ_FORMAT_ODIN2, NULL, NULL, NULL};
    expect_failure(read_req(&req), ODIN3_ERR_PARSE,
                   D "/all.o3proj:1:1: error: syntax: text before the root element");
    /* a .qip with --format qsf is read as a .qsf */
    req = (odin3_project_request){D "/q/ip.qip", ODIN3_PROJ_FORMAT_QUARTUS, NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_EQUAL_STRING("ip", str(project()->top));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_project_read(design, NULL));
    /* a format that is neither 0 nor an odin3_proj_format: misuse, no record touched */
    fresh_design();
    req = (odin3_project_request){D "/all.o3proj", ODIN3_PROJ_FORMAT_ODIN2 + 1, NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, read_req(&req));
    TEST_ASSERT_NULL(odin3_design_project(design));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --revision needs a Quartus entry; it chooses among a .qpf's revisions ("" is one). */
static void test_revision(void) {
    odin3_project_request req = {D "/all.o3proj", 0, "fast", NULL, NULL};
    expect_failure(read_req(&req), ODIN3_ERR_PARSE,
                   "syntax: --revision needs a .qpf or .qsf entry");
    req = (odin3_project_request){D "/q/proj.qpf", 0, "small", NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_EQUAL_STRING("small", str(project()->top));
    TEST_ASSERT_TRUE(project()->has_revision);
    TEST_ASSERT_EQUAL_STRING("small", str(project()->revision));
    fresh_design();
    req = (odin3_project_request){D "/q/proj.qpf", 0, "", NULL, NULL};
    expect_failure(read_req(&req), ODIN3_ERR_PARSE, "unknown_revision: no such revision: ''");
    expect_failure(read_entry(D "/q/proj.qpf"), ODIN3_ERR_PARSE,
                   D "/q/proj.qpf:1:1: error: ambiguous_revision: several revisions; choose one "
                     "with --revision: 'fast', 'small'");
}

/* --- the record's life --------------------------------------------------------------------- */

static void test_second_read(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/q/top.qsf"));
    odin3_project *first = project();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, read_entry(D "/all.o3proj"));
    TEST_ASSERT_EQUAL_STRING("project already read from " D "/q/top.qsf", captured);
    TEST_ASSERT_TRUE(first == project()); /* the first record stays */
    TEST_ASSERT_EQUAL_STRING("top", str(project()->top));
}

/* A failed read drops the record; the next read starts clean. */
static void test_failure_drops_record(void) {
    expect_failure(read_entry(D "/import_twice.o3proj"), ODIN3_ERR_PARSE,
                   "q/top.qsf:1:1: error: syntax: top given twice (first other): 'top'\n"
                   "  included from " D "/import_twice.o3proj:2");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/import_param.o3proj"));
    TEST_ASSERT_EQUAL_STRING("top", str(project()->top));
}

/* --- --top and the info lines -------------------------------------------------------------- */

static void test_top_replaces(void) {
    odin3_project_request req = {D "/all.o3proj", 0, NULL, "other", NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_EQUAL_STRING("other", str(project()->top));
    TEST_ASSERT_EQUAL_UINT32(0, project()->top_at.v);
    TEST_ASSERT_EQUAL_INT(2, ninfos); /* after finish, before the duplicates line */
    TEST_ASSERT_EQUAL_STRING(D "/all.o3proj: --top other replaces the project top mylib.top",
                             infos[0]);
    TEST_ASSERT_EQUAL_STRING(D "/all.o3proj: listed more than once, read once: " D "/src/a.v",
                             infos[1]);
    fresh_design();
    /* the same top: no info line, the declaring line kept */
    req.top = "mylib.top";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_TRUE(project()->top_at.v != 0);
    TEST_ASSERT_EQUAL_INT(1, ninfos);
    fresh_design();
    /* no project top: --top sets it, no info line */
    req = (odin3_project_request){D "/list.o3proj", ODIN3_PROJ_FORMAT_FLIST, NULL, "lib.t", NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_req(&req));
    TEST_ASSERT_EQUAL_STRING("lib.t", str(project()->top));
    TEST_ASSERT_EQUAL_INT(0, ninfos);
    fresh_design();
    req = (odin3_project_request){D "/all.o3proj", 0, NULL, "a b", NULL};
    expect_failure(read_req(&req), ODIN3_ERR_PARSE, "syntax: bad top name: 'a b'");
}

static void test_info_lines(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/q/top.qsf"));
    TEST_ASSERT_EQUAL_INT(1, ninfos);
    TEST_ASSERT_EQUAL_STRING(D "/q/top.qsf: ignored: PIN_ASSIGN", infos[0]);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(D "/q/dups.qsf"));
    TEST_ASSERT_EQUAL_INT(2, ninfos);
    TEST_ASSERT_EQUAL_STRING(D "/q/dups.qsf: ignored: -hdl_version, LOCATION", infos[0]);
    TEST_ASSERT_EQUAL_STRING(
        D "/q/dups.qsf: listed more than once, read once: " D "/lib/l.v, " D "/src/a.v", infos[1]);
}

/* --- corpus -------------------------------------------------------------------------------- */

/* The "project" object of a case's expected.json, as the dump spells it. */
static void corpus_project(const char *case_dir, odin3_strbuf *out) {
    char path[PATH_BUF + sizeof "/expected.json"]; /* case_dir is a PATH_BUF buffer */
    (void)snprintf(path, sizeof path, "%s/expected.json", case_dir);
    odin3_file_bytes bytes;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(path, FIXTURE_MAX, &bytes));
    odin3_strbuf json;
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
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at + 2, (size_t)(end - at) - 1}));
        at = end + 1;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("}\n")));
    odin3_strbuf_free(&json);
}

/* A corpus case and the entry file to read from it. */
typedef struct corpus_entry {
    const char *name, *entry;
} corpus_entry;

/* Reads the case's entry and compares the dump (case paths relative) with its expected record. */
static void expect_corpus(corpus_entry which) {
    const char *name = which.name;
    const char *entry = which.entry;
    fresh_design();
    char case_dir[PATH_BUF];
    (void)snprintf(case_dir, sizeof case_dir, "%s/%s", ODIN3_PROJECT_FIXTURES, name);
    TEST_ASSERT_EQUAL_INT(0, chdir(case_dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_entry(entry));
    odin3_strbuf got;
    odin3_strbuf want;
    odin3_strbuf_init(&got);
    odin3_strbuf_init(&want);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(project(), &got));
    corpus_project(case_dir, &want);
    /* the dump's paths are absolute and normalized: strip "<case dir>/" */
    char prefix[PATH_BUF];
    TEST_ASSERT_NOT_NULL(getcwd(prefix, sizeof prefix - 1));
    strcat(prefix, "/");
    odin3_strbuf rel;
    odin3_strbuf_init(&rel);
    const char *at = got.data;
    for (const char *hit = strstr(at, prefix); hit != NULL; hit = strstr(at, prefix)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_append(&rel, (odin3_bytes){at, (size_t)(hit - at)}));
        at = hit + strlen(prefix);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&rel, odin3_bytes_cstr(at)));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.data, rel.data, name);
    odin3_strbuf_free(&rel);
    odin3_strbuf_free(&got);
    odin3_strbuf_free(&want);
}

static void test_corpus_cases(void) {
    expect_corpus((corpus_entry){"single_file", "project.o3proj"});
    expect_corpus((corpus_entry){"single_file", "files.f"});
    expect_corpus((corpus_entry){"lib_v_y", "project.o3proj"});
    expect_corpus((corpus_entry){"lib_v_y", "files.f"});
    expect_corpus((corpus_entry){"param_string", "project.o3proj"});
    expect_corpus((corpus_entry){"param_string", "top.qpf"});
}

/* --- out of memory ------------------------------------------------------------------------- */

static bool oom_try(const odin3_project_request *req, long fail_after) {
    fresh_design();
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_project_read(design, req);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(odin3_design_project(design));
    }
    return st == ODIN3_OK;
}

/* Reads req with the k-th allocation failing, k = 0, 1, … until it succeeds. */
static void sweep(const odin3_project_request *req) {
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(req, k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 20);
}

static void test_out_of_memory(void) {
    static const odin3_project_request reqs[] = {
        {D "/all.o3proj", 0, NULL, "other", NULL},
        {D "/q/proj.qpf", 0, "fast", NULL, NULL},
        {D "/q/dups.qsf", 0, NULL, NULL, NULL},
        {ODIN3_PROJECT_FIXTURES "/odin2_arch/odin2.xml", 0, NULL, NULL, NULL},
        {D "/abc_script.o3proj", 0, NULL, NULL, D "/flow.o3"}, /* any extension */
    };
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        sweep(&reqs[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_by_suffix);
    RUN_TEST(test_format_by_request);
    RUN_TEST(test_relative_entry);
    RUN_TEST(test_entry_errors);
    RUN_TEST(test_revision);
    RUN_TEST(test_second_read);
    RUN_TEST(test_failure_drops_record);
    RUN_TEST(test_top_replaces);
    RUN_TEST(test_info_lines);
    RUN_TEST(test_corpus_cases);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
