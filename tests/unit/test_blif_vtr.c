/*
 * test_blif_vtr.c — unit tests for write_blif --vtr (spec 3E §16.1): options, arguments, the API's
 * own refusals, E13 and the process refusal (3E Task 1); VB-10 names (3E Task 2); the preflight's
 * refusals E1-E12, its cap, the source classes and the report (3E Task 3).
 */
#include "backends/blif/vtr.h"
#include "backends/blif/writer.h"
#include "blif_vtr_test.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/str.h"

#include <dirent.h>
#include <stdarg.h>
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

/* The expected line, formatted (fails the test when it does not fit). */
static char want_line[LINE_BUF * 2];

static const char *line_of(const char *fmt, ...) ODIN3_PRINTF(1, 2);
static const char *line_of(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(want_line, sizeof want_line, fmt, args);
    va_end(args);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want_line);
    return want_line;
}

static odin3_module *top_of(void) {
    return odin3_module_get(tc.design, odin3_design_top(tc.design));
}

/* The ID of the first live node of the top whose type is named type. */
static uint32_t node_of(const char *type) {
    odin3_module *top = top_of();
    for (uint32_t i = 1; i < odin3_module_node_end(top); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(top, node) &&
            strcmp(odin3_celltype_get(tc.design, odin3_node_type(top, node))->name, type) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(type);
    return 0;
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

/* buf's bytes as odin3_bytes. */
static odin3_bytes buf_of(const odin3_strbuf *buf) {
    return (odin3_bytes){buf->data, buf->len};
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
/* tests/golden/blif/<stem>.blif read through bvt_read (after lib/vtr.o3lib when vtr_lib). */
static void read_golden_with(const char *stem, bool vtr_lib) {
    char golden[PATH_BUF];
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_TRUE(snprintf(golden, sizeof golden, "%s/%s.blif", ODIN3_BLIF_FIXTURES, stem) <
                     PATH_BUF);
    slurp_into(golden, &text);
    read_ok(text_of(&text), vtr_lib);
    odin3_strbuf_free(&text);
}

static void read_golden(const char *stem) {
    read_golden_with(stem, false);
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

/* ansiportlist_2's declared black boxes adder and multiply, read after lib/vtr.o3lib (else E6):
 * their instances stay (Task 5 adds the stubs), their declared models are not written. */
static void test_vtr_omits_declared_blackboxes(void) {
    read_golden_with("ansiportlist_2.parmys", true);
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

/* --- VB-10 names (3E Task 2) ----------------------------------------------------------------- */

/* odin3_blif_vtr_legal_name(in) appended to a buffer holding "<": the result after the "<". */
static void expect_legal(odin3_bytes in, const char *want) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("<")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_legal_name(in, &buf));
    TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE("<", buf.data, 1, want);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(want) + 1, buf.len, want);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf.data + 1, want);
    odin3_strbuf_free(&buf);
}

/* A legal-name case of the brief's table. */
typedef struct legal_case {
    const char *in;
    const char *want;
} legal_case;

static void test_legal_name_table(void) {
    static const legal_case cases[] = {
        {"a#b", "a%23b"},
        {"a b", "a%20b"},
        {"x=y", "x%3Dy"},
        {"q\"", "q%22"},
        {"a\\b", "a%5Cb"},
        {"50%", "50%25"},
        {"\xC3\xA9", "%C3%A9"},
        {"\t", "%09"},
        {".end", "%2Eend"},
        {"re", "%re"},
        {"0", "%0"},
        {"NIL", "%NIL"},
        {"out:x", "%out:x"},
        {"unconn", "%unconn"},
        {"$auto$rtlil.cc:3203:MuxGate$1084", "$auto$rtlil.cc:3203:MuxGate$1084"},
        {"single_port_ram^MEM~0-7^out~0", "single_port_ram^MEM~0-7^out~0"},
        {"memory_controller_address~26", "memory_controller_address~26"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        expect_legal(odin3_bytes_cstr(cases[i].in), cases[i].want);
    }
    /* the rest of rule 3's list, a `.` and an `out:` that are not leading or whole, NUL and DEL */
    static const char *const reserved[] = {"fe", "ah", "al", "as", "1", "2", "3", "gnd", "vcc"};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        char want[LINE_BUF];
        TEST_ASSERT_TRUE(snprintf(want, sizeof want, "%%%s", reserved[i]) < LINE_BUF);
        expect_legal(odin3_bytes_cstr(reserved[i]), want);
    }
    expect_legal(odin3_bytes_cstr("a.b"), "a.b");
    expect_legal(odin3_bytes_cstr("xout:"), "xout:");
    expect_legal(odin3_bytes_cstr("Re"), "Re");
    expect_legal(odin3_bytes_cstr("nil"), "nil");
    expect_legal(odin3_bytes_cstr("out"), "out");
    expect_legal((odin3_bytes){"a\0b\x7F", 4}, "a%00b%7F");
    expect_legal((odin3_bytes){"", 0}, "");
    expect_legal(odin3_bytes_cstr("!~"), "!~");
}

/* `fe` and the byte 0xFE: lower-case letters are the reserved prefix, upper-case hex a byte. */
static void test_legal_name_injective_fe(void) {
    expect_legal(odin3_bytes_cstr("fe"), "%fe");
    expect_legal(odin3_bytes_cstr("\xFE"), "%FE");
    expect_legal(odin3_bytes_cstr("%fe"), "%25fe");
    expect_legal(odin3_bytes_cstr("%FE"), "%25FE");
}

/* Every name of up to INJ_LEN bytes over an alphabet of the bytes the rules turn on, with its legal
 * form and its clock form: all forms of different names differ, and every form is VPR-legal. */
enum { INJ_ALPHABET = 16, INJ_LEN = 3, INJ_NAMES = 1 + 16 + 16 * 16 + 16 * 16 * 16, FORM_MAX = 32 };

static const char INJ_BYTES[INJ_ALPHABET] = {'%', '2', '5', 'f', 'e', 'F', 'E', '.',
                                             'r', ' ', '|', '^', '_', '0', 'N', '\xFE'};

typedef struct inj_form {
    char text[FORM_MAX];
    uint32_t name;
} inj_form;

static int inj_cmp(const void *left, const void *right) {
    return strcmp(((const inj_form *)left)->text, ((const inj_form *)right)->text);
}

/* Name number index: its length, then its bytes in base INJ_ALPHABET. */
static size_t inj_name(uint32_t index, char *name) {
    size_t len = 0;
    uint32_t span = 1;
    while (index >= span) {
        index -= span;
        span *= INJ_ALPHABET;
        len++;
    }
    for (size_t i = 0; i < len; i++, index /= INJ_ALPHABET) {
        name[i] = INJ_BYTES[index % INJ_ALPHABET];
    }
    return len;
}

/* True when text (len bytes) is VPR-legal: printable, none of `="#\`, no leading `.`. */
static bool vpr_legal(const char *text, size_t len) {
    bool legal = len == 0 || text[0] != '.';
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)text[i];
        legal = legal && byte >= 0x21 && byte <= 0x7E && strchr("=\"#\\", byte) == NULL;
    }
    return legal;
}

/* The form in buf into form, checked VPR-legal. */
static void inj_store(const odin3_strbuf *buf, inj_form *form, uint32_t name) {
    const char *text = text_of(buf);
    TEST_ASSERT_TRUE(buf->len < FORM_MAX);
    TEST_ASSERT_TRUE_MESSAGE(vpr_legal(text, buf->len), text);
    memcpy(form->text, text, buf->len + 1);
    form->name = name;
}

static void test_legal_name_injective_generated(void) {
    static inj_form forms[2 * INJ_NAMES];
    odin3_strbuf legal;
    odin3_strbuf clock;
    odin3_strbuf_init(&legal);
    odin3_strbuf_init(&clock);
    for (uint32_t i = 0; i < INJ_NAMES; i++) {
        char name[INJ_LEN];
        size_t len = inj_name(i, name);
        odin3_strbuf_clear(&legal);
        odin3_strbuf_clear(&clock);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_blif_vtr_legal_name((odin3_bytes){name, len}, &legal));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_clock_name(buf_of(&legal), &clock));
        inj_store(&legal, &forms[(size_t)2 * i], i);
        inj_store(&clock, &forms[(size_t)2 * i + 1], i);
    }
    qsort(forms, (size_t)2 * INJ_NAMES, sizeof forms[0], inj_cmp);
    for (uint32_t i = 1; i < 2 * INJ_NAMES; i++) {
        bool same = strcmp(forms[i - 1].text, forms[i].text) == 0;
        TEST_ASSERT_TRUE_MESSAGE(!same || forms[i - 1].name == forms[i].name, forms[i].text);
    }
    odin3_strbuf_free(&legal);
    odin3_strbuf_free(&clock);
}

/* Rule 3's list as odin3_blif_vtr_reserved has it: whole names, and the `out:` prefix. */
static void test_reserved_names(void) {
    static const char *const yes[] = {"re", "fe", "ah",  "al",  "as",     "NIL",  "0",    "1",
                                      "2",  "3",  "gnd", "vcc", "unconn", "out:", "out:x"};
    static const char *const no[] = {"", "vcc1", "ou", "out", "Out:x", "4", "nil", "xre"};
    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(odin3_blif_vtr_reserved(odin3_bytes_cstr(yes[i])), yes[i]);
    }
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(odin3_blif_vtr_reserved(odin3_bytes_cstr(no[i])), no[i]);
    }
}

/* odin3_blif_vtr_clock_name(in), alone in a fresh buffer. */
static void expect_clock(const char *in, const char *want) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_clock_name(odin3_bytes_cstr(in), &buf));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf.len > 0 ? buf.data : "", in);
    odin3_strbuf_free(&buf);
}

/* Two `.latch` clocks and two signals with the same characters: only the clocks change. */
static const char *const CLOCK_NAMES = ".model top\n"
                                       ".inputs c d e\n"
                                       ".outputs q r y z\n"
                                       ".names c a|b\n"
                                       "1 1\n"
                                       ".names e x_^_y\n"
                                       "1 1\n"
                                       ".latch d q re a|b 2\n"
                                       ".latch d r re x_^_y 2\n"
                                       ".names d p|q\n"
                                       "1 1\n"
                                       ".names p|q y\n"
                                       "1 1\n"
                                       ".names d m_^_n\n"
                                       "1 1\n"
                                       ".names m_^_n z\n"
                                       "1 1\n"
                                       ".end\n";

static void test_clock_name_rule(void) {
    expect_clock("a|b", "a%7Cb");
    expect_clock("x_^_y", "x_%5E_y");
    expect_clock("_^_^_", "_%5E_%5E_");
    expect_clock("^_^", "^_^");
    expect_clock("a^_b_^c", "a^_b_^c");
    expect_clock("||", "%7C%7C");
    expect_clock("", "");
    read_ok(CLOCK_NAMES, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    char want[LINE_BUF * 2];
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, want);
    TEST_ASSERT_TRUE(strlen(want) + strlen(".model top\n") < sizeof want);
    (void)strncat(want, ".model top\n", sizeof want - strlen(want) - 1);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data, want), out.data);
    const char *body = out.data + strlen(want);
    TEST_ASSERT_EQUAL_STRING(".inputs c d e\n"
                             ".outputs q r y z\n"
                             ".names c a%7Cb\n"
                             "1 1\n"
                             ".names e x_%5E_y\n"
                             "1 1\n"
                             ".latch d q re a%7Cb 2\n"
                             ".latch d r re x_%5E_y 2\n"
                             ".names d p|q\n"
                             "1 1\n"
                             ".names p|q y\n"
                             "1 1\n"
                             ".names d m_^_n\n"
                             "1 1\n"
                             ".names m_^_n z\n"
                             "1 1\n"
                             ".end\n",
                             body);
    /* rule 8 is VB-10's too: the two clocks are counted (no port changed, so no warning) */
    TEST_ASSERT_EQUAL_UINT32(0, tc.warnings);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 2 names legalised (first: 'a|b' -> 'a%7Cb')\n"),
        tc.log.data);
}

/* --- hand IR ------------------------------------------------------------------------------- */

static uint32_t intern(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(tc.design, odin3_bytes_cstr(name), &id));
    return id;
}

/* tc's design replaced by one empty module `top`, the top. */
static odin3_module *hand_top(void) {
    read_ok(TWO_MODELS, false); /* the directory and the sink */
    empty_design();
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(tc.design, intern("top"), (odin3_prov_id){0}, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(tc.design, id));
    return odin3_module_get(tc.design, id);
}

/* A port of width bits declared [msb:lsb] (has_range) or [width-1:0]; its wire. */
typedef struct hand_port_spec {
    const char *name;
    odin3_dir dir;
    uint32_t width;
    bool has_range;
    int32_t msb, lsb;
} hand_port_spec;

static odin3_wire_id hand_port(odin3_module *module, hand_port_spec want) {
    odin3_port_spec spec = {.name = intern(want.name),
                            .dir = want.dir,
                            .width = want.has_range ? 0 : want.width,
                            .scalar = !want.has_range && want.width == 1,
                            .has_range = want.has_range,
                            .msb = want.msb,
                            .lsb = want.lsb};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, NULL));
    return odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
}

/* The net of a scalar port. */
static odin3_net_id hand_scalar(odin3_module *module, const char *name, odin3_dir dir) {
    return odin3_wire_net(module, hand_port(module, (hand_port_spec){name, dir, 1, false, 0, 0}),
                          0);
}

static odin3_net_id hand_net(odin3_module *module, const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(module, intern(name), (odin3_prov_id){0}, &net));
    return net;
}

/* A buffer `$sop` from in to `to`; its node. */
static odin3_node_id hand_buffer(odin3_module *module, odin3_net_id in, odin3_net_id to) {
    static const uint8_t rows[] = {'1', '1'};
    odin3_value params[2] = {odin3_value_int(1), {.kind = ODIN3_VAL_COVER}};
    params[1].bits = rows;
    params[1].len = sizeof rows;
    params[1].cover_inputs = 1;
    odin3_celltype_id sop = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern("$sop"), &sop));
    odin3_node_spec spec = {.type = sop, .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{&in, 1}, {&to, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return node;
}

/* The written file after the header and `.model top`, which it must start with. */
static const char *vtr_body(void) {
    char want[LINE_BUF * 2];
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, want);
    TEST_ASSERT_TRUE(strlen(want) + strlen(".model top\n") < sizeof want);
    (void)strncat(want, ".model top\n", sizeof want - strlen(want) - 1);
    TEST_ASSERT_TRUE_MESSAGE(out.len > 0 && starts_with(out.data, want), text_of(&out));
    return out.data + strlen(want);
}

/* `--vtr out.blif` succeeds; the body after `.model top`. */
static const char *write_vtr(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    return vtr_body();
}

/* Nets `a#b` and `a%23b` stay distinct (rule 1 escapes `%`), so neither falls back. */
static void test_escaped_names_distinct(void) {
    odin3_module *top = hand_top();
    odin3_net_id in = hand_scalar(top, "i", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_net_id first = hand_net(top, "a#b");
    odin3_net_id second = hand_net(top, "a%23b");
    hand_buffer(top, in, first);
    hand_buffer(top, first, second);
    hand_buffer(top, second, y_net);
    TEST_ASSERT_EQUAL_STRING(".inputs i\n.outputs y\n.names i a%23b\n1 1\n"
                             ".names a%23b a%2523b\n1 1\n.names a%2523b y\n1 1\n.end\n",
                             write_vtr());
}

/* A net named `p q` and an unnamed net on a wire `p q`: both legalise to `p%20q`, and the second
 * by net ID falls back to `$n<ID>` (VB-10 rule 4: legalisation before uniqueness). */
static void collide(bool named_first) {
    odin3_module *top = hand_top();
    odin3_net_id in = hand_scalar(top, "i", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_net_id first = {0};
    odin3_net_id second = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top, named_first ? intern("p q") : 0,
                                                     (odin3_prov_id){0}, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top, named_first ? 0 : intern("p q"),
                                                     (odin3_prov_id){0}, &second));
    odin3_wire_spec wire = {.name = intern("p q")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_wire_create(top, &wire, named_first ? &second : &first, NULL));
    hand_buffer(top, in, first);
    hand_buffer(top, first, second);
    hand_buffer(top, second, y_net);
    char want[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want,
                              ".inputs i\n.outputs y\n.names i p%%20q\n1 1\n"
                              ".names p%%20q $n%u\n1 1\n.names $n%u y\n1 1\n.end\n",
                              (unsigned)second.v, (unsigned)second.v) < LINE_BUF);
    TEST_ASSERT_EQUAL_STRING(want, write_vtr());
    bvt_free(&tc);
}

static void test_collision_falls_back(void) {
    collide(true);
    collide(false);
}

/* `input [8:1] a`: bit k is a[1 + k] in both dialects (VB-10 rule 5; the plain writer has used the
 * declared index since 2D-1), and an ascending [1:8] lists a[8] … a[1]. */
static void declared_design(void) {
    odin3_module *top = hand_top();
    odin3_wire_id a_wire = hand_port(top, (hand_port_spec){"a", ODIN3_DIR_IN, 0, true, 8, 1});
    odin3_wire_id b_wire = hand_port(top, (hand_port_spec){"b", ODIN3_DIR_IN, 0, true, 1, 8});
    odin3_wire_id y_wire = hand_port(top, (hand_port_spec){"y", ODIN3_DIR_OUT, 16, false, 0, 0});
    for (uint32_t k = 0; k < 8; k++) {
        hand_buffer(top, odin3_wire_net(top, a_wire, k), odin3_wire_net(top, y_wire, k));
        hand_buffer(top, odin3_wire_net(top, b_wire, k), odin3_wire_net(top, y_wire, k + 8));
    }
}

static const char *const DECLARED_PORTS =
    ".inputs a[1] a[2] a[3] a[4] a[5] a[6] a[7] a[8] b[8] b[7] b[6] b[5] b[4] b[3] b[2] b[1]\n";

static void test_declared_index(void) {
    declared_design();
    TEST_ASSERT_TRUE_MESSAGE(starts_with(write_vtr(), DECLARED_PORTS), text_of(&out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".names a[1] y[0]\n1 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".names b[1] y[15]\n1 1\n"));
}

/* The plain dialect names the same bits alike (2D-1's declared index, pinned in test_blif_writer's
 * test_ranged_port_bits), not a[0] … a[7]. */
static void test_declared_index_plain(void) {
    declared_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "out.blif", &out), tc.log.data);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data, ".model top\n"), out.data);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(out.data + strlen(".model top\n"), DECLARED_PORTS),
                             out.data);
}

/* A port `re` is written `%re`, with one warning; a 3-bit port `a b` gets one warning for its three
 * bits (next test). */
static void test_port_rename_warning(void) {
    read_ok(".model top\n.inputs re\n.outputs y\n.names re y\n1 1\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".inputs %re\n.outputs y\n.names %re y\n1 1\n.end\n", write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, count_of(tc.log.data, "warn: write_blif: --vtr: port"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data, "warn: write_blif: --vtr: port 're' "
                                                     "written as '%re' (VPR cannot read 're')\n"),
                                 tc.log.data);
}

static void test_port_rename_once_per_port(void) {
    odin3_module *top = hand_top();
    odin3_wire_id ab = hand_port(top, (hand_port_spec){"a b", ODIN3_DIR_IN, 3, false, 0, 0});
    odin3_wire_id y_wire = hand_port(top, (hand_port_spec){"y", ODIN3_DIR_OUT, 3, false, 0, 0});
    for (uint32_t k = 0; k < 3; k++) {
        hand_buffer(top, odin3_wire_net(top, ab, k), odin3_wire_net(top, y_wire, k));
    }
    TEST_ASSERT_TRUE_MESSAGE(
        starts_with(write_vtr(), ".inputs a%20b[0] a%20b[1] a%20b[2]\n.outputs y[0] y[1] y[2]\n"),
        text_of(&out));
    TEST_ASSERT_EQUAL_UINT32(1, count_of(tc.log.data, "warn: write_blif: --vtr: port"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data,
                                        "warn: write_blif: --vtr: port 'a b' "
                                        "written as 'a%20b' (VPR cannot read 'a b')\n"),
                                 tc.log.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 3 names legalised (first: 'a b[0]' -> 'a%20b[0]')\n"),
        tc.log.data);
}

/* 1C's refusal of two ports of one BLIF name on different nets (`a[1]` twice) comes from the
 * preflight's naming run in the VTR dialect: counted as an error, and no file. */
static void test_port_collision_refused(void) {
    odin3_module *top = hand_top();
    odin3_wire_id a_wire = hand_port(top, (hand_port_spec){"a", ODIN3_DIR_IN, 2, false, 0, 0});
    odin3_net_id a1 = hand_scalar(top, "a[1]", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_net_id z_net = hand_scalar(top, "z", ODIN3_DIR_OUT);
    hand_buffer(top, odin3_wire_net(top, a_wire, 1), y_net);
    hand_buffer(top, a1, z_net);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_TRUE_MESSAGE(only_error_is("write_blif: --vtr: model 'top': two ports on different "
                                           "nets have the BLIF name 'a[1]'"),
                             tc.log.data);
    TEST_ASSERT_FALSE(bvt_file_exists(&tc, "out.blif"));
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(1, report.errors);
}

/* A `single_port_ram` (1G hard cell) clock gets rule 8. */
static const char *const RAM_CLOCK =
    ".model top\n"
    ".inputs e d a w\n"
    ".outputs o\n"
    ".names e x|y\n"
    "1 1\n"
    ".subckt single_port_ram clk=x|y data[0]=d addr[0]=a we=w out[0]=o\n"
    ".end\n";

static void test_hard_cell_clock(void) {
    read_ok(RAM_CLOCK, true);
    const char *body = write_vtr();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, ".names e x%7Cy\n1 1\n"), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, " clk=x%7Cy"), body);
    TEST_ASSERT_NULL_MESSAGE(strstr(body, "x|y"), body);
}

/* Without the library the same cell is an implicit black box with no known clock, which the VTR
 * dialect refuses (E6, 3E Task 3) before any name is written. */
static void test_blackbox_clock_unknown(void) {
    read_ok(RAM_CLOCK, false);
    expect_refused(
        (refusal){"--vtr out.blif", line_of("write_blif: --vtr: t.blif:6: single_port_ram '$c%u': "
                                            "'single_port_ram' is not a hard cell of a loaded tech "
                                            "library (read_techlib lib/vtr.o3lib)",
                                            node_of("single_port_ram"))});
}

/* A clock that is a port bit: the port is written in its rule-8 form everywhere, and the warning
 * says why (VPR itself reads `c|k`). */
static void test_clock_port_bit(void) {
    read_ok(".model top\n.inputs c|k d\n.outputs q\n.latch d q re c|k 2\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".inputs c%7Ck d\n.outputs q\n.latch d q re c%7Ck 2\n.end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, count_of(tc.log.data, "warn: write_blif: --vtr:"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data, "warn: write_blif: --vtr: clock port 'c|k' "
                                                     "written as 'c%7Ck' (VTR's latch clock "
                                                     "domains cannot hold '|' or '_^_')\n"),
                                 tc.log.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 1 names legalised (first: 'c|k' -> 'c%7Ck')\n"),
        tc.log.data);
}

/* An output port `re` whose net is named `y`: the port bit is renamed (and buffered) while the net
 * keeps its name, and it counts in N. */
static void test_port_renamed_net_named(void) {
    odin3_module *top = hand_top();
    odin3_net_id in = hand_scalar(top, "i", ODIN3_DIR_IN);
    odin3_net_id re_net = hand_scalar(top, "re", ODIN3_DIR_OUT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top, re_net, intern("y")));
    hand_buffer(top, in, re_net);
    TEST_ASSERT_EQUAL_STRING(".inputs i\n.outputs %re\n.names i y\n1 1\n.names y %re\n1 1\n.end\n",
                             write_vtr());
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data, "info: write_blif: --vtr: 1 names legalised (first: 're' -> '%re')\n"),
        tc.log.data);
}

/* Port bits are named before nets, but the info line's first is the lowest net ID: an internal
 * net `a b` created before the port `c d`. */
static void test_first_is_lowest_net(void) {
    odin3_module *top = hand_top();
    odin3_net_id ab = hand_net(top, "a b");
    odin3_net_id cd = hand_scalar(top, "c d", ODIN3_DIR_IN);
    odin3_net_id o_net = hand_scalar(top, "o", ODIN3_DIR_OUT);
    TEST_ASSERT_TRUE(ab.v < cd.v);
    hand_buffer(top, cd, ab);
    hand_buffer(top, ab, o_net);
    TEST_ASSERT_EQUAL_STRING(
        ".inputs c%20d\n.outputs o\n.names c%20d a%20b\n1 1\n.names a%20b o\n1 1\n.end\n",
        write_vtr());
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 2 names legalised (first: 'a b' -> 'a%20b')\n"),
        tc.log.data);
}

/* Three illegal net names in a chain i -> `a b` -> `c=d` -> `.e` -> o. */
static void three_illegal(void) {
    odin3_module *top = hand_top();
    odin3_net_id in = hand_scalar(top, "i", ODIN3_DIR_IN);
    odin3_net_id o_net = hand_scalar(top, "o", ODIN3_DIR_OUT);
    odin3_net_id ab = hand_net(top, "a b");
    odin3_net_id cd = hand_net(top, "c=d");
    odin3_net_id dot_e = hand_net(top, ".e");
    hand_buffer(top, in, ab);
    hand_buffer(top, ab, cd);
    hand_buffer(top, cd, dot_e);
    hand_buffer(top, dot_e, o_net);
}

static void test_info_line(void) {
    three_illegal();
    TEST_ASSERT_EQUAL_STRING(".inputs i\n.outputs o\n.names i a%20b\n1 1\n.names a%20b c%3Dd\n"
                             "1 1\n.names c%3Dd %2Ee\n1 1\n.names %2Ee o\n1 1\n.end\n",
                             write_vtr());
    TEST_ASSERT_TRUE_MESSAGE(
        starts_with(tc.log.data,
                    "info: write_blif: --vtr: 3 names legalised (first: 'a b' -> 'a%20b')\n"),
        tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(1, count_of(tc.log.data, "legalised"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(tc.log.data, "write_blif: --vtr: port"));
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(3, report.renamed);
    TEST_ASSERT_EQUAL_UINT32(0, report.errors);
    /* the plain dialect writes the names as they are, and says nothing */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "out.blif", &out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".names c=d .e\n"));
    TEST_ASSERT_NULL(strstr(tc.log.data, "legalised"));
}

/* A generator keeps its reserved name (rule 3's exception, VB-9 §9.3): Parmys's `.names gnd` and
 * `.names vcc` / `1`; a signal named `vcc` and a constant-1 cover named `gnd` are renamed. */
static void test_generator_keeps_name(void) {
    static const char *const parmys = ".inputs a\n.outputs y\n.names gnd\n.names vcc\n1\n"
                                      ".names unconn\n.names gnd vcc unconn a y\n0101 1\n.end\n";
    char text[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text, ".model top\n%s", parmys) < LINE_BUF);
    read_ok(text, false);
    TEST_ASSERT_EQUAL_STRING(parmys, write_vtr());
    TEST_ASSERT_NULL_MESSAGE(strstr(tc.log.data, "legalised"), tc.log.data);
    bvt_free(&tc);
    read_ok(".model top\n.inputs a\n.outputs y\n.names a vcc\n1 1\n.names gnd\n1\n"
            ".names vcc gnd y\n11 1\n.end\n",
            false);
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names a %vcc\n1 1\n.names %gnd\n1\n"
                             ".names %vcc %gnd y\n11 1\n.end\n",
                             write_vtr());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data, "info: write_blif: --vtr: 2 names legalised"),
                                 tc.log.data);
}

/* --- the preflight: refusals E1-E12, the cap, classes, the report (3E Task 3) ---------------- */

/* `write_blif --vtr out.blif` refused with exactly want (after `error: `), no out.blif. */
static void expect_vtr_refused(const char *want) {
    expect_refused((refusal){"--vtr out.blif", want});
}

/* odin3_blif_vtr_preflight (default K) is INVALID_ARG and logs exactly `error: <want>`. */
static void expect_preflight_refused(const char *want) {
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    char line[LINE_BUF * 2];
    TEST_ASSERT_TRUE(snprintf(line, sizeof line, "error: %s\n", want) < (int)sizeof line);
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_STRING(line, text_of(&tc.log));
    TEST_ASSERT_EQUAL_UINT32(1, report.errors);
}

/* A pin of a node: bit `bit` of port `port`. */
typedef struct pin_at {
    uint32_t port, bit;
} pin_at;

/* Moves pin `bit` of port `port` of node onto a new net driven by a new cell of type const_type
 * (`$_CONSTX_`, …). */
static void pin_to_const(odin3_node_id node, pin_at at, const char *const_type) {
    odin3_module *top = top_of();
    odin3_pin_id pin = {odin3_node_port(top, node, at.port).first.v + at.bit};
    odin3_net_id net = {0};
    odin3_celltype_id type = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top, pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top, 0, (odin3_prov_id){0}, &net));
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern(const_type), &type));
    odin3_node_spec spec = {.type = type};
    const odin3_netvec ports[] = {{&net, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top, pin, net));
}

/* A new unconnected node of the named type with params (NULL: the defaults) in hand IR. */
static odin3_node_id hand_cell(odin3_module *top, const char *type_name, const odin3_value *params,
                               uint32_t n_params) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(tc.design, intern(type_name), &type), type_name);
    odin3_node_spec spec = {.type = type, .params = params, .n_params = n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(top, &spec, &node));
    return node;
}

/* A latch on line 5, with the latch line given. */
static void read_latch_line(const char *latch) {
    char text[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              ".model top\n.inputs d c\n.outputs q\n# line 4\n%s\n.end\n",
                              latch) < LINE_BUF);
    read_ok(text, false);
}

static void test_e1_falling_edge(void) {
    read_latch_line(".latch d q fe c 0");
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:5: $_DFF_N_ '$c%u': falling-edge "
                               "flip-flop; VPR accepts only rising-edge latches (lower inverts "
                               "the clock)",
                               node_of("$_DFF_N_")));
    bvt_free(&tc);
    read_latch_line(".latch d q fe c 0\n.cname ff1"); /* a named cell: its name */
    expect_vtr_refused("write_blif: --vtr: t.blif:5: $_DFF_N_ 'ff1': falling-edge flip-flop; VPR "
                       "accepts only rising-edge latches (lower inverts the clock)");
}

static const char *const E1B = "asynchronous set/reset flip-flop; VPR has none (lower makes it "
                               "synchronous, decision 19)";

static void test_e1b_async(void) {
    read_ok(".model top\n.inputs c d r s\n.outputs q\n# line 4\n"
            ".subckt $_DFFSR_PPP_ C=c D=d Q=q R=r S=s\n.end\n",
            false);
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:5: $_DFFSR_PPP_ '$c%u': %s",
                               node_of("$_DFFSR_PPP_"), E1B));
    bvt_free(&tc);
    odin3_module *top = hand_top();
    odin3_node_id adff = hand_cell(top, "$adff", NULL, 0);
    expect_vtr_refused(line_of("write_blif: --vtr: top: $adff '$c%u': %s", adff.v, E1B));
}

/* Plan 3E point (e)'s names: E1b wins over E6 for each; `$_DFF_PN2_` and `$_DFFE_PP0_` are no
 * async flip-flops (E6). */
static void test_e1b_names(void) {
    read_ok(".model top\n.inputs c d r\n.outputs q\n"
            ".subckt $_DFF_PN0_ C=c D=d Q=q R=r\n"
            ".subckt $_DFFE_PP1N_ C=c D=d Q=q R=r\n"
            ".subckt $_SR_PN_ S=c R=r Q=q\n"
            ".subckt $_ALDFF_PP_ C=c D=d Q=q\n"
            ".subckt $_ALDFFE_PPP_ C=c D=d Q=q\n"
            ".subckt $_DFFSRE_PPPP_ C=c D=d Q=q\n"
            ".subckt $_DFF_PN2_ C=c D=d Q=q\n"
            ".subckt $_DFFE_PP0_ C=c D=d Q=q\n"
            ".end\n",
            false);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(8, report.errors);
    TEST_ASSERT_EQUAL_UINT32(6, count_of(tc.log.data, E1B));
    TEST_ASSERT_EQUAL_UINT32(2, count_of(tc.log.data, "is not a hard cell"));
    TEST_ASSERT_NOT_NULL(strstr(tc.log.data, "t.blif:10: $_DFF_PN2_ '"));
    TEST_ASSERT_NOT_NULL(strstr(tc.log.data, "t.blif:11: $_DFFE_PP0_ '"));
}

static void test_e2_level_latch(void) {
    read_latch_line(".latch d q ah c 0");
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:5: $_DLATCH_P_ '$c%u': level-sensitive "
                               "latch; VPR accepts only rising-edge latches",
                               node_of("$_DLATCH_P_")));
}

static const char *const E3 = "flip-flop without a clock; VPR requires a clock on every latch";

/* A hand $_DFF_P_ with D and Q connected and C open; its node. */
static odin3_node_id hand_dff_open_clock(odin3_module *top) {
    odin3_net_id d_net = hand_scalar(top, "d", ODIN3_DIR_IN);
    odin3_net_id q_net = hand_scalar(top, "q", ODIN3_DIR_OUT);
    odin3_node_id dff = hand_cell(top, "$_DFF_P_", NULL, 0);
    odin3_pinslice pins = odin3_node_pins(top, dff);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_connect(top, (odin3_pin_id){pins.first.v + 1}, d_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_connect(top, (odin3_pin_id){pins.first.v + 2}, q_net));
    return dff;
}

static void test_e3_no_clock(void) {
    read_latch_line(".latch d q 0");
    expect_vtr_refused(
        line_of("write_blif: --vtr: t.blif:5: $_FF_ '$c%u': %s", node_of("$_FF_"), E3));
    bvt_free(&tc);
    odin3_module *top = hand_top();
    odin3_node_id dff = hand_dff_open_clock(top);
    expect_vtr_refused(line_of("write_blif: --vtr: top: $_DFF_P_ '$c%u': %s", dff.v, E3));
    pin_to_const(dff, (pin_at){0, 0}, "$_CONSTX_");
    expect_vtr_refused(line_of("write_blif: --vtr: top: $_DFF_P_ '$c%u': %s", dff.v, E3));
    odin3_pin_id clock = odin3_node_port(top, dff, 0).first; /* then C on an undriven net */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top, clock));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top, clock, hand_net(top, "floating")));
    expect_preflight_refused(line_of("write_blif: --vtr: top: $_DFF_P_ '$c%u': %s", dff.v, E3));
}

/* A single_port_ram with ADDR_WIDTH 4 and DATA_WIDTH 1, every pin connected, fed by .names. */
static const char *const RAM4 = ".model top\n"
                                ".inputs c d a0 a1 a2 a3 w\n"
                                ".outputs o y\n"
                                ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 addr[1]=a1 "
                                "addr[2]=a2 addr[3]=a3 we=w out[0]=o\n"
                                ".names a0 a1 y\n"
                                "11 1\n"
                                ".end\n";

static void test_e3b_ram_clock_x(void) {
    read_ok(RAM4, true);
    uint32_t ram = node_of("single_port_ram");
    pin_to_const((odin3_node_id){ram}, (pin_at){0, 0}, "$_CONSTX_");
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:4: single_port_ram '$c%u': clock input "
                               "'clk' is unconnected or x",
                               ram));
}

/* E3b with `clk` unconnected (an open pin is as bad as x). */
static void test_e3b_ram_clock_open(void) {
    read_ok(RAM4, true);
    uint32_t ram = node_of("single_port_ram");
    odin3_pin_id clock = odin3_node_port(top_of(), (odin3_node_id){ram}, 0).first;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top_of(), clock));
    expect_preflight_refused(line_of("write_blif: --vtr: t.blif:4: single_port_ram '$c%u': clock "
                                     "input 'clk' is unconnected or x",
                                     ram));
}

static void test_e4_tristate(void) {
    read_ok(".model top\n.inputs a e\n.outputs y\n.subckt $tribuf A=a EN=e Y=y\n.end\n", false);
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:4: $tribuf '$c%u': tri-state cell; VPR "
                               "has no tri-state",
                               node_of("$tribuf")));
}

static void test_e5_word_cell(void) {
    read_ok(".model top\n.inputs a b\n.outputs y\n.subckt $add A[0]=a B[0]=b Y[0]=y\n.end\n",
            false);
    expect_vtr_refused(
        line_of("write_blif: --vtr: t.blif:4: $add '$c%u': not a netlist cell (run lower)",
                node_of("$add")));
}

static const char *const E6 = "is not a hard cell of a loaded tech library (read_techlib "
                              "lib/vtr.o3lib)";

static void test_e6_not_hard(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.subckt foo a=a\n.end\n", false);
    expect_vtr_refused(
        line_of("write_blif: --vtr: t.blif:6: foo '$c%u': 'foo' %s", node_of("foo"), E6));
    bvt_free(&tc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        bvt_read_lib(&tc, (bvt_source){".model top\n.inputs a b\n.outputs y\n"
                                       ".subckt AND2 A=a B=b Y=y\n.end\n",
                                       "generic_gates.o3lib"}),
        tc.log.data);
    expect_vtr_refused(
        line_of("write_blif: --vtr: t.blif:4: AND2 '$c%u': 'AND2' %s", node_of("AND2"), E6));
    bvt_free(&tc);
    read_ok(".model top\n.inputs a\n.outputs y\n.subckt bb i=a o=y\n.end\n\n"
            ".model bb\n.inputs i\n.outputs o\n.blackbox\n.end\n",
            false);
    expect_vtr_refused(
        line_of("write_blif: --vtr: t.blif:4: bb '$c%u': 'bb' %s", node_of("bb"), E6));
}

/* A 7-input .names on line 4 with the cover row given. */
static void read_names7(const char *row) {
    char text[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              ".model top\n.inputs a b c d e f g\n.outputs y\n"
                              ".names a b c d e f g y\n%s\n.end\n",
                              row) < LINE_BUF);
    read_ok(text, false);
}

static void test_e7_too_many_inputs(void) {
    read_names7("1111111 1");
    const char *want = line_of("write_blif: --vtr: t.blif:4: $sop '$c%u': 7 inputs exceed K=6 "
                               "(run abc, or pass --lut-k)",
                               node_of("$sop"));
    expect_vtr_refused(want);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr --lut-k 7 out.blif", &out),
                                  tc.log.data);
    bvt_free(&tc);
    read_names7("------- 1"); /* a constant function is still refused (plan 3E point (g)) */
    expect_vtr_refused(want);
    bvt_free(&tc);
    read_ok(".model top\n.inputs a b s\n.outputs y\n.subckt $_MUX_ A=a B=b S=s Y=y\n.end\n", false);
    expect_refused((refusal){"--vtr --lut-k 2 out.blif",
                             line_of("write_blif: --vtr: t.blif:4: $_MUX_ '$c%u': 3 inputs exceed "
                                     "K=2 (run abc, or pass --lut-k)",
                                     node_of("$_MUX_"))});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr --lut-k 3 out.blif", &out),
                                  tc.log.data);
}

/* A hand $sop of `inputs` inputs (all on in) driving y with the cover rows (inputs + 1 bytes
 * each, no blanks); its node. */
static odin3_node_id hand_sop(odin3_module *top, uint32_t inputs, const char *rows) {
    odin3_net_id in = hand_scalar(top, "a", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_net_id ins[7] = {in, in, in, in, in, in, in};
    TEST_ASSERT_TRUE(inputs <= 7);
    odin3_value params[2] = {odin3_value_int(inputs), {.kind = ODIN3_VAL_COVER}};
    params[1].bits = (const uint8_t *)rows;
    params[1].len = (uint32_t)strlen(rows);
    params[1].cover_inputs = inputs;
    odin3_celltype_id sop = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern("$sop"), &sop));
    odin3_node_spec spec = {.type = sop, .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{ins, inputs}, {&y_net, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, &node));
    return node;
}

/* E8 in hand IR (the BLIF reader refuses such a cover itself). */
static void test_e8_mixed_cover(void) {
    odin3_node_id sop = hand_sop(hand_top(), 1, "1100");
    expect_vtr_refused(line_of("write_blif: --vtr: top: $sop '$c%u': cover mixes on-set and "
                               "off-set rows",
                               sop.v));
    bvt_free(&tc);
    sop = hand_sop(hand_top(), 7, "1111111100000000"); /* E7 comes first: one refusal per node */
    expect_vtr_refused(line_of("write_blif: --vtr: top: $sop '$c%u': 7 inputs exceed K=6 (run "
                               "abc, or pass --lut-k)",
                               sop.v));
}

static void test_e9_inout(void) {
    odin3_module *top = hand_top();
    (void)hand_port(top, (hand_port_spec){"p", ODIN3_DIR_INOUT, 1, false, 0, 0});
    expect_vtr_refused("write_blif: --vtr: top: port 'p': inout port; VPR has no bidirectional "
                       "pins");
}

static void test_e10_hierarchy(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.subckt sub x=a z=y\n.end\n\n"
            ".model sub\n.inputs x\n.outputs z\n.names x z\n1 1\n.end\n",
            false);
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:4: instance '$c%u' of module 'sub': "
                               "hierarchy not flattened (run flatten); VPR reads one model",
                               node_of("sub")));
}

/* A top and the declared model multiply (a[0..35], b[0..35], out[0..71]), after lib/vtr.o3lib;
 * then a hand node of multiply with A_WIDTH a_width and B_WIDTH b_width; its node. */
static odin3_node_id multiply_node(int64_t a_width, int64_t b_width) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(".model top\n.inputs i\n.outputs "
                                                              "o\n.names i o\n1 1\n.end\n\n"
                                                              ".model multiply\n.inputs")));
    for (int i = 0; i < 72; i++) { /* a[0] … a[35], then b[0] … b[35] */
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(&text, " %c[%d]", i < 36 ? 'a' : 'b', i % 36));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("\n.outputs")));
    for (int i = 0; i < 72; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&text, " out[%d]", i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&text, odin3_bytes_cstr("\n.blackbox\n.end\n")));
    read_ok(text.data, true);
    odin3_strbuf_free(&text);
    odin3_value params[2] = {odin3_value_int(a_width), odin3_value_int(b_width)};
    return hand_cell(top_of(), "multiply", params, 2);
}

static void test_e11_params(void) {
    odin3_node_id mul = multiply_node(8, 36);
    expect_vtr_refused(line_of("write_blif: --vtr: top: multiply '$c%u': parameters 'A_WIDTH' "
                               "cannot be recovered from BLIF formals",
                               mul.v));
    bvt_free(&tc);
    mul = multiply_node(8, 9);
    expect_vtr_refused(line_of("write_blif: --vtr: top: multiply '$c%u': parameters 'A_WIDTH', "
                               "'B_WIDTH' cannot be recovered from BLIF formals",
                               mul.v));
}

/* E12 (the pass's Debug check refuses a net with two drivers first, so the preflight is called):
 * drivers in node ID order, at most two, then `, …`; a $port_in by its port. */
static void test_e12_drivers(void) {
    odin3_module *top = hand_top();
    odin3_net_id a_net = hand_scalar(top, "a", ODIN3_DIR_IN);
    odin3_net_id b_net = hand_scalar(top, "b", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_net_id n_net = hand_net(top, "n");
    odin3_node_id first = hand_buffer(top, a_net, n_net);
    odin3_node_id second = hand_buffer(top, b_net, n_net);
    (void)hand_buffer(top, n_net, y_net);
    expect_preflight_refused(line_of("write_blif: --vtr: top: net 'n' has 2 drivers ('$c%u', "
                                     "'$c%u')",
                                     first.v, second.v));
    (void)hand_buffer(top, a_net, n_net);
    expect_preflight_refused(line_of("write_blif: --vtr: top: net 'n' has 3 drivers ('$c%u', "
                                     "'$c%u', \xE2\x80\xA6)",
                                     first.v, second.v));
    bvt_free(&tc);
    top = hand_top();
    a_net = hand_scalar(top, "a", ODIN3_DIR_IN);
    b_net = hand_scalar(top, "b", ODIN3_DIR_IN);
    second = hand_buffer(top, b_net, a_net);
    expect_preflight_refused(
        line_of("write_blif: --vtr: top: net 'a' has 2 drivers ('a', '$c%u')", second.v));
}

/* One cell driving the net twice (review I1): listed twice, no crash. Read with odin3_blif_read
 * (the read_blif pass's Debug check refuses the net) and refused by the API write, the path with
 * no check before it. */
static void test_e12_one_cell_twice(void) {
    char path[PATH_BUF];
    read_ok(TWO_MODELS, false); /* the directory and the sink */
    empty_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(tc.design, "read_techlib",
                                                   odin3_bytes_cstr(ODIN3_LIB_DIR "/vtr.o3lib")));
    path_in_dir("twice.blif", path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(".model top\n.inputs x y z\n.outputs n\n"
                           ".subckt adder a=x b=y cin=z cout=n sumout=n\n.end\n",
                           file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(tc.design, path), tc.log.data);
    uint32_t adder = node_of("adder");
    const char *want =
        line_of("write_blif: --vtr: top: net 'n' has 2 drivers ('$c%u', '$c%u')", adder, adder);
    expect_preflight_refused(want);
    expect_api_refused(&(odin3_blif_write_opts){ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0},
                       want);
}

/* 21 `fe` latches: 20 E1 lines, then the count line; report.errors is exact. */
static void test_error_cap(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&text, odin3_bytes_cstr(".model top\n.inputs d c\n"
                                                                      ".outputs")));
    for (int i = 0; i < 21; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&text, " q%d", i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("\n")));
    for (int i = 0; i < 21; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&text, ".latch d q%d fe c 0\n", i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr(".end\n")));
    read_ok(text.data, false);
    odin3_strbuf_free(&text);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(21, report.errors);
    TEST_ASSERT_EQUAL_UINT32(21, count_of(tc.log.data, "error: "));
    TEST_ASSERT_EQUAL_UINT32(20, count_of(tc.log.data, "falling-edge flip-flop"));
    TEST_ASSERT_TRUE_MESSAGE(
        ends_with(tc.log.data, "\nerror: write_blif: --vtr: 1 more errors not shown\n"),
        tc.log.data);
    TEST_ASSERT_NOT_NULL(strstr(tc.log.data, "t.blif:23: $_DFF_N_"));
    TEST_ASSERT_NULL(strstr(tc.log.data, "t.blif:24: $_DFF_N_"));
}

/* The net of the top named name. */
static odin3_net_id net_named(const char *name) {
    odin3_net_id net = odin3_module_find_net(top_of(), intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), name);
    return net;
}

/* A new net of the top driven by a new cell of the constant type const_type. */
static odin3_net_id const_net(const char *const_type) {
    odin3_module *top = top_of();
    odin3_net_id net = {0};
    odin3_celltype_id type = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top, 0, (odin3_prov_id){0}, &net));
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern(const_type), &type));
    odin3_node_spec spec = {.type = type};
    const odin3_netvec ports[] = {{&net, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, NULL));
    return net;
}

/* Twenty refusals, then naming's own (two ports of one BLIF name): its line is past the cap, so
 * it is counted only (review M2). */
static void test_naming_refusal_past_cap(void) {
    odin3_module *top = hand_top();
    (void)hand_port(top, (hand_port_spec){"a", ODIN3_DIR_IN, 2, false, 0, 0});
    (void)hand_scalar(top, "a[1]", ODIN3_DIR_IN);
    for (int i = 0; i < 20; i++) {
        (void)hand_cell(top, "$_DFF_N_", NULL, 0);
    }
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(21, report.errors);
    TEST_ASSERT_EQUAL_UINT32(21, count_of(tc.log.data, "error: "));
    TEST_ASSERT_EQUAL_UINT32(20, count_of(tc.log.data, "falling-edge flip-flop"));
    TEST_ASSERT_NULL_MESSAGE(strstr(tc.log.data, "BLIF name"), tc.log.data);
    TEST_ASSERT_TRUE_MESSAGE(
        ends_with(tc.log.data, "\nerror: write_blif: --vtr: 1 more errors not shown\n"),
        tc.log.data);
}

/* A refused write logs its refusal alone: no VB-10 port warning or info line (review M3). */
static void test_refused_logs_no_renames(void) {
    read_ok(".model top\n.inputs re c\n.outputs q\n.latch re q fe c 0\n.end\n", false);
    expect_vtr_refused(line_of("write_blif: --vtr: t.blif:4: $_DFF_N_ '$c%u': falling-edge "
                               "flip-flop; VPR accepts only rising-edge latches (lower inverts "
                               "the clock)",
                               node_of("$_DFF_N_")));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, tc.warnings, tc.log.data);
    TEST_ASSERT_NULL_MESSAGE(strstr(tc.log.data, "legalised"), tc.log.data);
}

/* `.names unconn` with the row `0` is a constant 0, not Parmys's zero-row `unconn` (review M1). */
static void test_unconn_with_row_is_zero(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names unconn\n0\n.names unconn a y\n11 1\n"
            ".end\n",
            false);
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_0,
                          odin3_blif_vtr_net_class(top_of(), net_named("unconn")));
}

/* Point (i) for a DERIVED record: the first leaf of the backward walk gives `<loc>`. */
static void test_loc_derived(void) {
    odin3_module *top = hand_top();
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(tc.design, intern("probe"), &ctx));
    odin3_srcloc loc = {.file = intern("src/x.v"), .line = 7, .col = 1, .end_line = 7};
    odin3_prov_origin origin = {.locs = &loc, .n_locs = 1};
    odin3_prov_id leaf = {0};
    odin3_prov_id derived = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &leaf));
    odin3_prov_begin_op(&ctx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){&leaf, 1}, &derived));
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern("$_DFF_N_"), &type));
    odin3_node_spec spec = {.type = type, .prov = derived};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(top, &spec, &node));
    expect_preflight_refused(line_of("write_blif: --vtr: src/x.v:7: $_DFF_N_ '$c%u': "
                                     "falling-edge flip-flop; VPR accepts only rising-edge latches "
                                     "(lower inverts the clock)",
                                     node.v));
}

/* Review Focus 2: §9.1's classes by the driving cell only; x is gnd on a LUT input and open on a
 * hard-block input, never the reverse. */
static void test_classes_by_pin_kind(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names gnd\n.names vcc\n1\n.names unconn\n"
            ".names vcc b\n1 1\n.names c\n1\n.names e\n.names gnd vcc unconn b c e y\n"
            "011110 1\n.end\n",
            false);
    odin3_module *top = top_of();
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_0, odin3_blif_vtr_net_class(top, net_named("gnd")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_1, odin3_blif_vtr_net_class(top, net_named("vcc")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_U, odin3_blif_vtr_net_class(top, net_named("unconn")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_SIGNAL, odin3_blif_vtr_net_class(top, net_named("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_1, odin3_blif_vtr_net_class(top, net_named("c")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_0, odin3_blif_vtr_net_class(top, net_named("e")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_SIGNAL, odin3_blif_vtr_net_class(top, net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_X, odin3_blif_vtr_net_class(top, const_net("$_CONSTX_")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_X, odin3_blif_vtr_net_class(top, const_net("$_CONSTZ_")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_0, odin3_blif_vtr_net_class(top, const_net("$_CONST0_")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_1, odin3_blif_vtr_net_class(top, const_net("$_CONST1_")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_UNDRIVEN,
                          odin3_blif_vtr_net_class(top, hand_net(top, "w")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_UNDRIVEN,
                          odin3_blif_vtr_net_class(top, (odin3_net_id){0}));
    /* the plan keeps the classes that are not SIGNAL */
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    odin3_blif_vtr_plan plan = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_plan_build(tc.design, &opts, &plan, &report));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_U, odin3_blif_vtr_plan_class(&plan, net_named("unconn")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_SIGNAL, odin3_blif_vtr_plan_class(&plan, net_named("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_UNDRIVEN,
                          odin3_blif_vtr_plan_class(&plan, net_named("w")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BLIF_SRC_UNDRIVEN,
                          odin3_blif_vtr_plan_class(&plan, (odin3_net_id){0}));
    TEST_ASSERT_NOT_NULL(plan.names);
    odin3_blif_vtr_plan_free(&plan);
    TEST_ASSERT_NULL(plan.names);
    TEST_ASSERT_EQUAL_UINT32(1, report.unconn_logic); /* the .names input on `unconn` */
    TEST_ASSERT_EQUAL_UINT32(0, report.x_to_gnd);
    /* x on a .names input -> gnd; x on a RAM address bit -> unconn */
    bvt_free(&tc);
    read_ok(RAM4, true);
    pin_to_const((odin3_node_id){node_of("$sop")}, (pin_at){0, 1}, "$_CONSTX_");
    pin_to_const((odin3_node_id){node_of("single_port_ram")}, (pin_at){2, 3}, "$_CONSTX_");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(1, report.x_to_gnd);
    TEST_ASSERT_EQUAL_UINT32(1, report.x_to_unconn);
    TEST_ASSERT_EQUAL_UINT32(0, report.unconn_logic);
    TEST_ASSERT_EQUAL_UINT32(1, report.names);
    TEST_ASSERT_EQUAL_UINT32(1, report.subckts);
}

/* Review Focus 5: an E1 refusal leaves an existing out.blif as it was. */
static void test_refusal_keeps_old_file(void) {
    char path[PATH_BUF];
    read_latch_line(".latch d q fe c 0");
    path_in_dir("out.blif", path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("old\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_EQUAL_STRING("old\n", text_of(&out));
}

/* The number of lines of buf that start with word. */
static uint32_t lines_starting(const odin3_strbuf *buf, const char *word) {
    uint32_t count = 0;
    for (const char *at = text_of(buf); at != NULL && *at != '\0';) {
        count += starts_with(at, word) ? 1U : 0U;
        at = strchr(at, '\n');
        at = at != NULL ? at + 1 : NULL;
    }
    return count;
}

/* The adder golden (Task 5's fixture), read after lib/vtr.o3lib: no refusal, and the report's
 * counts are the written file's lines. */
static void test_clean_report(void) {
    read_golden_with("adder_hard_block.parmys", true);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {.errors = 9};
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    TEST_ASSERT_EQUAL_UINT32(0, report.errors);
    TEST_ASSERT_EQUAL_UINT32(0, count_of(text_of(&tc.log), "error"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(3, report.names);
    TEST_ASSERT_EQUAL_UINT32(0, report.latches);
    TEST_ASSERT_EQUAL_UINT32(8, report.subckts);
    TEST_ASSERT_EQUAL_UINT32(lines_starting(&out, ".names "), report.names);
    TEST_ASSERT_EQUAL_UINT32(lines_starting(&out, ".latch "), report.latches);
    TEST_ASSERT_EQUAL_UINT32(lines_starting(&out, ".subckt "), report.subckts);
    TEST_ASSERT_EQUAL_UINT32(0, report.x_to_gnd + report.x_to_unconn + report.unconn_logic);
}

/* odin3_blif_vtr_preflight under an allocation failure at every step: NO_MEMORY until it gives
 * want (leaks are LeakSanitizer's). */
static void preflight_oom_sweep(odin3_status want) {
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report clean = {0};
    odin3_strbuf clean_log;
    odin3_strbuf_init(&clean_log);
    odin3_strbuf_clear(&tc.log);
    TEST_ASSERT_EQUAL_INT(want, odin3_blif_vtr_preflight(tc.design, &opts, &clean));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&clean_log, buf_of(&tc.log)));
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        odin3_blif_vtr_report report = {0};
        odin3_strbuf_clear(&tc.log);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_blif_vtr_preflight(tc.design, &opts, &report);
        odin3_util_set_alloc_fail_after(-1);
        if (st != want) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
        done = st == want;
        if (done) { /* the end state is an unfailed run's: no failure swallowed or refused */
            TEST_ASSERT_EQUAL_MEMORY(&clean, &report, sizeof report);
            TEST_ASSERT_EQUAL_STRING(text_of(&clean_log), text_of(&tc.log));
        }
    }
    odin3_strbuf_free(&clean_log);
    TEST_ASSERT_TRUE(done);
}

static void test_out_of_memory_preflight(void) {
    read_golden_with("adder_hard_block.parmys", true);
    preflight_oom_sweep(ODIN3_OK);
    bvt_free(&tc);
    (void)multiply_node(8, 9); /* E11's list */
    odin3_module *top = top_of();
    odin3_net_id n_net = net_named("o");
    (void)hand_buffer(top, net_named("i"), n_net); /* E12 */
    (void)hand_cell(top, "$_DFF_N_", NULL, 0);     /* E1 */
    preflight_oom_sweep(ODIN3_ERR_INVALID_ARG);
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

/* odin3_blif_vtr_legal_name: NO_MEMORY leaves the buffer as it was, until it succeeds. */
static void test_out_of_memory_legal_name(void) {
    static const char name[] = "out:a b#c\xFE.d_^_e|f";
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        odin3_strbuf buf;
        odin3_strbuf_init(&buf);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&buf, odin3_bytes_cstr("keep")));
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_blif_vtr_legal_name(odin3_bytes_cstr(name), &buf);
        odin3_status clock =
            st == ODIN3_OK ? odin3_blif_vtr_clock_name(odin3_bytes_cstr("|d_^_e|"), &buf) : st;
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_STRING("keep", buf.data);
        } else if (clock != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, clock);
            TEST_ASSERT_EQUAL_STRING("keep%out:a%20b%23c%FE.d_^_e|f", buf.data);
        } else {
            TEST_ASSERT_EQUAL_STRING("keep%out:a%20b%23c%FE.d_^_e|f%7Cd_%5E_e%7C", buf.data);
            done = true;
        }
        odin3_strbuf_free(&buf);
    }
    TEST_ASSERT_TRUE(done);
}

/* A --vtr write with renames (the info line, the preflight's naming run, the writer's): NO_MEMORY
 * with no file until it succeeds, then the same bytes as without the sweep. */
static void test_out_of_memory_sweep_renames(void) {
    char path[PATH_BUF];
    three_illegal();
    path_in_dir("api.blif", path);
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        done = api_oom_step(fail_at, path, &opts);
    }
    TEST_ASSERT_TRUE(done);
    odin3_strbuf api;
    odin3_strbuf_init(&api);
    slurp_into(path, &api);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_EQUAL_STRING(text_of(&out), text_of(&api));
    odin3_strbuf_free(&api);
}

/* Tasks 1 and 2: options, arguments, the API, E13, names. */
static void run_tasks_1_2(void) {
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
    RUN_TEST(test_legal_name_table);
    RUN_TEST(test_legal_name_injective_fe);
    RUN_TEST(test_reserved_names);
    RUN_TEST(test_legal_name_injective_generated);
    RUN_TEST(test_clock_name_rule);
    RUN_TEST(test_escaped_names_distinct);
    RUN_TEST(test_collision_falls_back);
    RUN_TEST(test_declared_index);
    RUN_TEST(test_declared_index_plain);
    RUN_TEST(test_port_rename_warning);
    RUN_TEST(test_port_rename_once_per_port);
    RUN_TEST(test_port_collision_refused);
    RUN_TEST(test_hard_cell_clock);
    RUN_TEST(test_blackbox_clock_unknown);
    RUN_TEST(test_clock_port_bit);
    RUN_TEST(test_port_renamed_net_named);
    RUN_TEST(test_first_is_lowest_net);
    RUN_TEST(test_info_line);
    RUN_TEST(test_generator_keeps_name);
}

/* Task 3 (the preflight), then help, processes and out of memory. */
static void run_task_3_and_more(void) {
    RUN_TEST(test_e1_falling_edge);
    RUN_TEST(test_e1b_async);
    RUN_TEST(test_e1b_names);
    RUN_TEST(test_e2_level_latch);
    RUN_TEST(test_e3_no_clock);
    RUN_TEST(test_e3b_ram_clock_x);
    RUN_TEST(test_e3b_ram_clock_open);
    RUN_TEST(test_e4_tristate);
    RUN_TEST(test_e5_word_cell);
    RUN_TEST(test_e6_not_hard);
    RUN_TEST(test_e7_too_many_inputs);
    RUN_TEST(test_e8_mixed_cover);
    RUN_TEST(test_e9_inout);
    RUN_TEST(test_e10_hierarchy);
    RUN_TEST(test_e11_params);
    RUN_TEST(test_e12_drivers);
    RUN_TEST(test_e12_one_cell_twice);
    RUN_TEST(test_error_cap);
    RUN_TEST(test_naming_refusal_past_cap);
    RUN_TEST(test_refused_logs_no_renames);
    RUN_TEST(test_unconn_with_row_is_zero);
    RUN_TEST(test_loc_derived);
    RUN_TEST(test_classes_by_pin_kind);
    RUN_TEST(test_refusal_keeps_old_file);
    RUN_TEST(test_clean_report);
    RUN_TEST(test_out_of_memory_preflight);
    RUN_TEST(test_help_string);
    RUN_TEST(test_processes_refused_first);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_out_of_memory_sweep_pass);
    RUN_TEST(test_out_of_memory_legal_name);
    RUN_TEST(test_out_of_memory_sweep_renames);
}

int main(void) {
    UNITY_BEGIN();
    run_tasks_1_2();
    run_task_3_and_more();
    return UNITY_END();
}
