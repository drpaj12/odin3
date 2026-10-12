/*
 * test_blif_vtr.c — unit tests for write_blif --vtr (spec 3E §16.1): options, arguments, the API's
 * own refusals, E13 and the process refusal (3E Task 1).
 */
#include "backends/blif/vtr.h"
#include "backends/blif/writer.h"
#include "blif_vtr_test.h"
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif
#ifndef ODIN3_BVT_DATA
#error "ODIN3_BVT_DATA must name tests/unit/data/blif_vtr"
#endif

enum { PATH_BUF = 512, OOM_SWEEP = 20000, LINE_BUF = 256 };

/* Two models: the top and a module the top does not instantiate. */
static const char *const TWO_MODELS = ".model top\n"
                                      ".inputs a b\n"
                                      ".outputs y\n"
                                      ".names a b y\n"
                                      "11 1\n"
                                      ".end\n"
                                      "\n"
                                      ".model other\n"
                                      ".inputs x\n"
                                      ".outputs z\n"
                                      ".names x z\n"
                                      "0 1\n"
                                      ".end\n";

static bvt tc;
static odin3_strbuf out;

void setUp(void) {
    tc = (bvt){0};
    odin3_strbuf_init(&out);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_strbuf_free(&out);
    bvt_free(&tc);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void read_ok(const char *text, bool vtr_lib) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_read(&tc, text, vtr_lib), tc.log.data);
}

static void path_in_dir(const char *name, char *path) {
    (void)snprintf(path, PATH_BUF, "%s/%s", tc.dir, name);
}

/* The pass manager's own line after a pass that failed with INVALID_ARG. */
static const char *const PASS_FAILED = "error: pass write_blif: failed: ODIN3_ERR_INVALID_ARG";

/* True when the log's first line is `error: <want>` and every other error line is PASS_FAILED
 * (the manager's; its timing line is info). */
static bool only_error_is(const char *want) {
    char line[LINE_BUF];
    (void)snprintf(line, sizeof line, "error: %s\n", want);
    const char *text = tc.log.len > 0 ? tc.log.data : "";
    if (strncmp(text, line, strlen(line)) != 0) {
        return false;
    }
    for (const char *at = text + strlen(line); *at != '\0';) {
        const char *end = strchr(at, '\n');
        size_t len = end != NULL ? (size_t)(end - at) : strlen(at);
        if (strncmp(at, "error: ", strlen("error: ")) == 0 &&
            (len != strlen(PASS_FAILED) || strncmp(at, PASS_FAILED, len) != 0)) {
            return false;
        }
        at += end != NULL ? len + 1 : len;
    }
    return true;
}

/* A refused write_blif: its arguments and the one error line it logs. */
typedef struct refusal {
    const char *args;
    const char *line;
} refusal;

/* write_blif <want.args> is INVALID_ARG with exactly the error line want.line (then at most the
 * pass manager's failure line), and writes no out.blif. */
static void expect_refused(refusal want) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, bvt_write(&tc, want.args, &out),
                                  want.args);
    TEST_ASSERT_TRUE_MESSAGE(only_error_is(want.line), tc.log.data);
    TEST_ASSERT_FALSE_MESSAGE(bvt_file_exists(&tc, "out.blif"), want.args);
}

/* The API write to <dir>/name with opts is INVALID_ARG with exactly the error line want. */
static void expect_api_refused(const odin3_blif_write_opts *opts, const char *want) {
    char path[PATH_BUF];
    char line[LINE_BUF];
    path_in_dir("api.blif", path);
    (void)snprintf(line, sizeof line, "error: %s\n", want);
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write_with(tc.design, path, opts));
    TEST_ASSERT_EQUAL_STRING(line, tc.log.data);
    TEST_ASSERT_FALSE(bvt_file_exists(&tc, "api.blif"));
}

/* Replaces tc's design (read for its directory and sink) with an empty one. */
static void empty_design(void) {
    odin3_design_destroy(tc.design);
    tc.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(tc.design);
}

/* The header line the VTR dialect writes for K=lut_k, short names, and the blank line after it,
 * in a buffer of LINE_BUF bytes. */
static void header_of(uint32_t lut_k, char *buf) {
    (void)snprintf(buf, LINE_BUF, "# Odin III %s write_blif --vtr (K=%u, names short)\n\n",
                   odin3_version_string(), (unsigned)lut_k);
}

static bool starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* --- arguments (plan 3E point (l)) ---------------------------------------------------------- */

static void test_unknown_argument(void) {
    read_ok(TWO_MODELS, false);
    expect_refused((refusal){"--bogus out.blif", "write_blif: unknown argument '--bogus'"});
}

static void test_path_count(void) {
    read_ok(TWO_MODELS, false);
    expect_refused((refusal){"--vtr", "write_blif: expects one path"});
    expect_refused((refusal){"--vtr a.blif b.blif", "write_blif: expects one path"});
    TEST_ASSERT_FALSE(bvt_file_exists(&tc, "a.blif"));
    TEST_ASSERT_FALSE(bvt_file_exists(&tc, "b.blif"));
    expect_refused((refusal){"", "write_blif: expects one path"});
}

static void test_lut_k_values(void) {
    static const char *const bad[] = {"1", "17", "six", "6x", "-6", "0"};
    read_ok(TWO_MODELS, false);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char args[LINE_BUF];
        char want[LINE_BUF];
        (void)snprintf(args, sizeof args, "--vtr --lut-k %s out.blif", bad[i]);
        (void)snprintf(want, sizeof want,
                       "write_blif: --lut-k expects an integer in 2..16, got '%s'", bad[i]);
        expect_refused((refusal){args, want});
    }
    expect_refused((refusal){"--vtr out.blif --lut-k",
                             "write_blif: --lut-k expects an integer in 2..16, "
                             "got ''"});
}

static void test_name_style_values(void) {
    read_ok(TWO_MODELS, false);
    expect_refused((refusal){"--vtr --name-style long out.blif",
                             "write_blif: --name-style expects short or provenance, got 'long'"});
    expect_refused((refusal){"--vtr out.blif --name-style",
                             "write_blif: --name-style expects short or provenance, got ''"});
}

static void test_options_need_vtr(void) {
    read_ok(TWO_MODELS, false);
    expect_refused(
        (refusal){"--lut-k 6 out.blif", "write_blif: --lut-k and --name-style need --vtr"});
    expect_refused((refusal){"out.blif --name-style short",
                             "write_blif: --lut-k and --name-style need --vtr"});
}

static void test_option_given_twice(void) {
    read_ok(TWO_MODELS, false);
    expect_refused((refusal){"--vtr --vtr out.blif", "write_blif: option '--vtr' given twice"});
    expect_refused((refusal){"--vtr --lut-k 4 --lut-k 5 out.blif",
                             "write_blif: option '--lut-k' given twice"});
    expect_refused((refusal){"--vtr --name-style short out.blif --name-style short",
                             "write_blif: option '--name-style' given twice"});
}

/* Only `--` words are options, so a path starting with one `-` is a path as before 3E; after a
 * lone `--` every word is a path. */
static void test_dash_paths(void) {
    read_ok(TWO_MODELS, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "-weird.blif", &out), tc.log.data);
    TEST_ASSERT_TRUE(bvt_file_exists(&tc, "-weird.blif"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr -- --odd.blif", &out),
                                  tc.log.data);
    TEST_ASSERT_TRUE(bvt_file_exists(&tc, "--odd.blif"));
    expect_refused((refusal){"-- out.blif --vtr", "write_blif: expects one path"});
    expect_refused((refusal){"--vtr --", "write_blif: expects one path"});
}

/* Point (m): provenance names parse, and the writer refuses them until 3E-6. */
static void test_name_style_provenance_not_yet(void) {
    read_ok(TWO_MODELS, false);
    expect_refused(
        (refusal){"--vtr --name-style provenance out.blif",
                  "write_blif: --vtr: --name-style provenance is not available yet (3E-6)"});
}

/* The path and the options in any order; the header carries the K. */
static void test_order_free(void) {
    char want[LINE_BUF];
    read_ok(TWO_MODELS, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "out.blif --lut-k 5 --vtr", &out),
                                  tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, tc.errors);
    TEST_ASSERT_EQUAL_UINT32(0, tc.warnings);
    header_of(5, want);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data, want), out.data);
}

/* Without --lut-k the K is 6; --name-style short is accepted; only the top is written. */
static void test_vtr_defaults_and_top_alone(void) {
    char want[LINE_BUF];
    read_ok(TWO_MODELS, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, bvt_write(&tc, "--vtr --name-style short out.blif", &out), tc.log.data);
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, want);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data, want), out.data);
    TEST_ASSERT_TRUE(starts_with(out.data + strlen(want), ".model top\n"));
    TEST_ASSERT_NULL(strstr(out.data, ".model other"));
}

/* Without --lut-k and --name-style: K=6, short names. */
static void test_vtr_plain_options(void) {
    char want[LINE_BUF];
    read_ok(TWO_MODELS, false);
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, want);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data, want), out.data);
}

/* An argument refusal leaves an existing file at the path as it was. */
static void test_argument_refusal_keeps_old_file(void) {
    read_ok(TWO_MODELS, false);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "out.blif", &out));
    char *before = odin3_util_malloc(out.len + 1);
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, out.data, out.len + 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, bvt_write(&tc, "--vtr --lut-k 99 out.blif", &out));
    TEST_ASSERT_EQUAL_STRING(before, out.data);
    odin3_util_free(before);
}

/* --- the plain dialect is unchanged ---------------------------------------------------------- */

/* buf's text, "" when it holds none. */
static const char *text_of(const odin3_strbuf *buf) {
    return buf->len > 0 ? buf->data : "";
}

/* The whole file at path into buf, empty for a NULL path (Unity fails when it cannot be read). */
static void slurp_into(const char *path, odin3_strbuf *buf) {
    odin3_strbuf_clear(buf);
    if (path == NULL) {
        return; /* nothing was written */
    }
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char chunk[LINE_BUF];
    for (size_t got = fread(chunk, 1, sizeof chunk, file); got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(buf, (odin3_bytes){chunk, got}));
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The one golden of tests/golden/blif that fails check (Odin II's multi-driver net), so the pass
 * (which Debug checks first) is not compared on it. */
static const char *const UNCHECKED = "elsif_both_defined.odin.blif";

static bool ends_with(const char *text, const char *suffix) {
    size_t len = strlen(text);
    return len >= strlen(suffix) && strcmp(text + len - strlen(suffix), suffix) == 0;
}

/* For one golden (read without the pass manager, as test_blif_writer does): `write_blif out.blif`
 * writes what odin3_blif_write_with(NULL) writes, in status and bytes (the pass wiring; the bytes
 * themselves are pinned by test_plain_bytes_pinned). Only UNCHECKED fails check and is skipped. */
static void plain_same(const char *golden) {
    odin3_strbuf api;
    odin3_strbuf_init(&api);
    read_ok(TWO_MODELS, false); /* the directory and the sink */
    empty_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(tc.design, golden), golden);
    char path[PATH_BUF];
    path_in_dir("api.blif", path);
    odin3_status with = odin3_blif_write_with(tc.design, path, NULL);
    slurp_into(with == ODIN3_OK ? path : NULL, &api);
    bool checked = odin3_check_design(tc.design, (odin3_check_opts){ODIN3_CHECK_FULL,
                                                                    ODIN3_VIEW_NONE}) == ODIN3_OK;
    TEST_ASSERT_TRUE_MESSAGE(checked != ends_with(golden, UNCHECKED), golden);
    if (checked) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(with, bvt_write(&tc, "out.blif", &out), golden);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(text_of(&api), text_of(&out), golden);
    }
    odin3_strbuf_free(&api);
    bvt_free(&tc);
}

static void test_plain_pass_matches_api(void) {
    DIR *dir = opendir(ODIN3_BLIF_FIXTURES);
    TEST_ASSERT_NOT_NULL(dir);
    uint32_t files = 0;
    for (struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        char path[PATH_BUF];
        (void)snprintf(path, sizeof path, "%s/%s", ODIN3_BLIF_FIXTURES, entry->d_name);
        if (ends_with(entry->d_name, ".blif")) {
            files++;
            plain_same(path);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, closedir(dir));
    TEST_ASSERT_TRUE(files > 1);
}

/*
 * The plain bytes as the base build (11b5ab4, before 3E) wrote them: tests/unit/data/blif_vtr/plain
 * /<stem>.blif is that build's `read_blif tests/golden/blif/<stem>.blif; write_blif <file>`. Both
 * the API with NULL opts and the pass must still write them. 3E-3 (Task 8) changes the plain
 * constants on purpose and updates these files in the same commit.
 */
static const char *const PINNED[] = {"adder_hard_block.parmys", "ansiportlist_2.parmys", "ff.odin",
                                     "hand_body", "hand_ports"};

static void pinned_same(const char *stem) {
    char golden[PATH_BUF];
    char pinned[PATH_BUF];
    char path[PATH_BUF];
    odin3_strbuf want;
    odin3_strbuf api;
    odin3_strbuf_init(&want);
    odin3_strbuf_init(&api);
    (void)snprintf(golden, sizeof golden, "%s/%s.blif", ODIN3_BLIF_FIXTURES, stem);
    (void)snprintf(pinned, sizeof pinned, "%s/plain/%s.blif", ODIN3_BVT_DATA, stem);
    slurp_into(pinned, &want);
    read_ok(TWO_MODELS, false); /* the directory and the sink */
    empty_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(tc.design, golden), stem);
    path_in_dir("api.blif", path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write_with(tc.design, path, NULL), stem);
    slurp_into(path, &api);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(text_of(&want), text_of(&api), stem);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "out.blif", &out), stem);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(text_of(&want), text_of(&out), stem);
    odin3_strbuf_free(&want);
    odin3_strbuf_free(&api);
    bvt_free(&tc);
}

static void test_plain_bytes_pinned(void) {
    for (size_t i = 0; i < sizeof PINNED / sizeof PINNED[0]; i++) {
        pinned_same(PINNED[i]);
    }
}

/* --- what the VTR dialect leaves out at Task 1 ----------------------------------------------- */

/* Reads tests/golden/blif/<stem>.blif into tc through bvt_read. */
static void read_golden(const char *stem) {
    char golden[PATH_BUF];
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    (void)snprintf(golden, sizeof golden, "%s/%s.blif", ODIN3_BLIF_FIXTURES, stem);
    slurp_into(golden, &text);
    read_ok(text_of(&text), false);
    odin3_strbuf_free(&text);
}

static uint32_t count_of(const char *text, const char *needle) {
    uint32_t count = 0;
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* hand_ports has a `.clock` line and the models sub and bb (a black box): plain writes them all,
 * --vtr writes the top alone, without `.clock`. */
static void test_vtr_omits_clock_and_models(void) {
    read_golden("hand_ports");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "out.blif", &out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n.clock clk\n"));
    TEST_ASSERT_EQUAL_UINT32(3, count_of(out.data, ".model "));
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".blackbox"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_NULL(strstr(out.data, ".clock"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".model "));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n.model top\n"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(out.data, ".blackbox"));
}

/* ansiportlist_2's declared black boxes adder and multiply: their instances stay (Tasks 3-5 make
 * them hard cells and stubs), their declared models are not written. */
static void test_vtr_omits_declared_blackboxes(void) {
    read_golden("ansiportlist_2.parmys");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".model "));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(out.data, ".blackbox"));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".subckt adder "));
}

/* --- the API's own refusals (plan 3E point (n)) ---------------------------------------------- */

static void test_api_plain_rejects_vtr_options(void) {
    read_ok(TWO_MODELS, false);
    static const char *const want =
        "odin3_blif_write_with: lut_k and name_style apply to the VTR dialect only";
    expect_api_refused(
        &(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_PLAIN, ODIN3_BLIF_NAMES_PROVENANCE, 0}, want);
    expect_api_refused(
        &(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_PLAIN, ODIN3_BLIF_NAMES_SHORT, 6}, want);
}

static void test_api_lut_k_range(void) {
    read_ok(TWO_MODELS, false);
    expect_api_refused(&(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 1},
                       "odin3_blif_write_with: lut_k 1 is outside 2..16");
    expect_api_refused(&(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 17},
                       "odin3_blif_write_with: lut_k 17 is outside 2..16");
    expect_api_refused(
        &(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_PROVENANCE, 6},
        "write_blif: --vtr: --name-style provenance is not available yet (3E-6)");
}

/* NULL opts is odin3_blif_write, byte for byte. */
static void test_api_null_opts_is_plain(void) {
    char with[PATH_BUF];
    char plain[PATH_BUF];
    odin3_strbuf other;
    odin3_strbuf_init(&other);
    read_ok(TWO_MODELS, false);
    path_in_dir("with.blif", with);
    path_in_dir("plain.blif", plain);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write_with(tc.design, with, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(tc.design, plain));
    slurp_into(with, &out);
    slurp_into(plain, &other);
    TEST_ASSERT_EQUAL_STRING(other.data, out.data);
    TEST_ASSERT_TRUE(starts_with(out.data, ".model top\n"));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".model other\n"));
    odin3_strbuf_free(&other);
}

/* The VTR dialect through the API: the same bytes as the pass. */
static void test_api_vtr_matches_pass(void) {
    char path[PATH_BUF];
    odin3_strbuf api;
    odin3_strbuf_init(&api);
    read_ok(TWO_MODELS, false);
    path_in_dir("api.blif", path);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write_with(tc.design, path, &opts));
    slurp_into(path, &api);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "--vtr --lut-k 4 out.blif", &out));
    TEST_ASSERT_EQUAL_STRING(api.data, out.data);
    odin3_strbuf_free(&api);
}

/* --- the preflight at Task 1: E13 and the options -------------------------------------------- */

static void test_e13_no_top(void) {
    read_ok(TWO_MODELS, false);
    empty_design();
    odin3_module_id id = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(tc.design, odin3_bytes_cstr("lonely"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(tc.design, name, (odin3_prov_id){0}, &id));
    expect_refused(
        (refusal){"--vtr out.blif",
                  "write_blif: --vtr: no top module (read_blif sets one; or run hierarchy)"});
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {.errors = 7};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(1, report.errors);
}

/* An E13 refusal (the writer's preflight, not the parser) leaves an existing file as it was. */
static void test_e13_keeps_old_file(void) {
    char path[PATH_BUF];
    read_ok(TWO_MODELS, false);
    path_in_dir("out.blif", path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("keep\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    empty_design();
    odin3_module_id id = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(tc.design, odin3_bytes_cstr("lonely"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(tc.design, name, (odin3_prov_id){0}, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_TRUE_MESSAGE(
        only_error_is("write_blif: --vtr: no top module (read_blif sets one; or run hierarchy)"),
        tc.log.data);
    TEST_ASSERT_EQUAL_STRING("keep\n", out.data);
}

static void test_preflight_report_and_k(void) {
    read_ok(TWO_MODELS, false);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_BLIF_VTR_DEFAULT_K, odin3_blif_vtr_k(&opts));
    odin3_blif_vtr_report report = {.errors = 7, .renamed = 3};
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_STRING("", tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, report.errors);
    TEST_ASSERT_EQUAL_UINT32(0, report.renamed);
    TEST_ASSERT_EQUAL_UINT32(1, report.modules_skipped);
    opts.lut_k = 16;
    TEST_ASSERT_EQUAL_UINT32(16, odin3_blif_vtr_k(&opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    opts.lut_k = 17;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_STRING("error: odin3_blif_vtr_preflight: lut_k 17 is outside 2..16\n",
                             tc.log.data);
    opts.lut_k = 0;
    opts.dialect = ODIN3_BLIF_DIALECT_PLAIN;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_vtr_preflight(NULL, &opts, &report));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, NULL, &report));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_vtr_preflight(tc.design, &opts, NULL));
}

/* --- help and processes ---------------------------------------------------------------------- */

static void test_help_string(void) {
    const odin3_pass_def *def = NULL;
    for (uint32_t i = 0; i < odin3_pass_count(); i++) {
        if (strcmp(odin3_pass_at(i)->name, "write_blif") == 0) {
            def = odin3_pass_at(i);
        }
    }
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_EQUAL_STRING("write_blif [--vtr] [--lut-k K] [--name-style short|provenance] "
                             "<file>: write the design as BLIF (the top first); --vtr writes the "
                             "top alone in the dialect VPR reads",
                             def->help);
}

/* 2D's P1 (no top): the process refusal comes first, and no preflight line (E13) follows. */
static void test_processes_refused_first(void) {
    read_ok(TWO_MODELS, false);
    empty_design();
    (void)proc_fixture_design(tc.design);
    expect_refused(
        (refusal){"--vtr out.blif", "pass 'write_blif': module 'm' has processes; run proc first"});
    expect_api_refused(&(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 1},
                       "module 'm' has processes; run proc first");
}

/* --- out of memory --------------------------------------------------------------------------- */

/* One step of the API sweep: true when the write succeeded; a failure is NO_MEMORY, no file. */
static bool api_oom_step(long fail_at, const char *path, const odin3_blif_write_opts *opts) {
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_blif_write_with(tc.design, path, opts);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_FALSE(bvt_file_exists(&tc, "api.blif"));
    }
    return st == ODIN3_OK;
}

/* odin3_blif_write_with in the VTR dialect: NO_MEMORY with no file, until it succeeds. */
static void test_out_of_memory_sweep(void) {
    char path[PATH_BUF];
    read_ok(TWO_MODELS, false);
    path_in_dir("api.blif", path);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 3};
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        done = api_oom_step(fail_at, path, &opts);
    }
    TEST_ASSERT_TRUE(done);
}

/* One step of the pass sweep: true when the pass succeeded (and wrote oom.blif, then removed). */
static bool pass_oom_step(long fail_at, const char *args) {
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_pass_run(tc.design, "write_blif", odin3_bytes_cstr(args));
    odin3_util_set_alloc_fail_after(-1);
    char path[PATH_BUF];
    path_in_dir("oom.blif", path);
    bool written = remove(path) == 0;
    if (st != ODIN3_OK) {
        /* the manager's check after the pass may fail once the file is written */
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    }
    TEST_ASSERT_TRUE(st != ODIN3_OK || written);
    return st == ODIN3_OK;
}

/* The pass with options (the parser's path buffer, then the writer): NO_MEMORY until it succeeds.
 * The no-file property is the API sweep's: here the pass manager's own check after the pass can
 * run out of memory once the file is written. */
static void test_out_of_memory_sweep_pass(void) {
    char path[PATH_BUF];
    char args[PATH_BUF + LINE_BUF];
    read_ok(TWO_MODELS, false);
    path_in_dir("oom.blif", path);
    (void)snprintf(args, sizeof args, "--vtr --lut-k 4 %s", path);
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        done = pass_oom_step(fail_at, args);
    }
    TEST_ASSERT_TRUE(done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_argument);
    RUN_TEST(test_path_count);
    RUN_TEST(test_lut_k_values);
    RUN_TEST(test_name_style_values);
    RUN_TEST(test_options_need_vtr);
    RUN_TEST(test_option_given_twice);
    RUN_TEST(test_dash_paths);
    RUN_TEST(test_name_style_provenance_not_yet);
    RUN_TEST(test_order_free);
    RUN_TEST(test_vtr_defaults_and_top_alone);
    RUN_TEST(test_vtr_plain_options);
    RUN_TEST(test_argument_refusal_keeps_old_file);
    RUN_TEST(test_plain_pass_matches_api);
    RUN_TEST(test_plain_bytes_pinned);
    RUN_TEST(test_vtr_omits_clock_and_models);
    RUN_TEST(test_vtr_omits_declared_blackboxes);
    RUN_TEST(test_api_plain_rejects_vtr_options);
    RUN_TEST(test_api_lut_k_range);
    RUN_TEST(test_api_null_opts_is_plain);
    RUN_TEST(test_api_vtr_matches_pass);
    RUN_TEST(test_e13_no_top);
    RUN_TEST(test_e13_keeps_old_file);
    RUN_TEST(test_preflight_report_and_k);
    RUN_TEST(test_help_string);
    RUN_TEST(test_processes_refused_first);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_out_of_memory_sweep_pass);
    return UNITY_END();
}
