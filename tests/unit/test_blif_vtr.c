/*
 * test_blif_vtr.c — unit tests for write_blif --vtr (spec 3E §16.1): options, arguments, the API's
 * own refusals, E13 and the process refusal (3E Task 1); VB-10 names (3E Task 2); the preflight's
 * refusals E1-E12, its cap, the source classes and the report (3E Task 3); emission: the header,
 * generators, cells, latches and port buffers (3E Task 4); hard blocks: the sorted .subckt
 * formals, clock pins and the stubs, and Task 4's review minors (3E Task 5); $lut through
 * odin3_blif_isop's cover, E7 and the constant and 1-input normalisation (3E-5, Task 9).
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
#include "techlib/reader.h"
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
 * their instances stay, and their declared models come back as the stubs (§8.3), as declared and
 * sorted by name, after the top's .end. */
static void test_vtr_omits_declared_blackboxes(void) {
    read_golden_with("ansiportlist_2.parmys", true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(3, count_of(out.data, ".model "));
    TEST_ASSERT_EQUAL_UINT32(2, count_of(out.data, ".blackbox"));
    TEST_ASSERT_NOT_NULL(strstr(out.data, ".subckt adder "));
    const char *adder =
        strstr(out.data, "\n.end\n\n.model adder\n.inputs cin b a\n"
                         ".outputs sumout cout\n.blackbox\n.end\n\n.model multiply\n"
                         ".inputs b[0] b[1] ");
    TEST_ASSERT_NOT_NULL_MESSAGE(adder, out.data);
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
    TEST_ASSERT_EQUAL_STRING(
        "info: write_blif: --vtr: 1 modules besides the top 'top' not written\n"
        "info: write_blif: --vtr: 'top': 1 .names, 0 .latch, 0 .subckt, K=6; "
        "x bits: 0 to gnd, 0 open (unconn)\n",
        tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, report.errors);
    TEST_ASSERT_EQUAL_UINT32(0, report.renamed);
    TEST_ASSERT_EQUAL_UINT32(1, report.modules_skipped);
    opts.lut_k = 16;
    TEST_ASSERT_EQUAL_UINT32(16, odin3_blif_vtr_k(&opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    odin3_strbuf_clear(&tc.log);
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
    odin3_status st = bvt_write(&tc, "--vtr out.blif", &out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, text_of(&tc.log));
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
                                                     "written as '%re' (not a legal name "
                                                     "for VTR's BLIF readers)\n"),
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
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data, "warn: write_blif: --vtr: port 'a b' "
                                                     "written as 'a%20b' (not a legal name for "
                                                     "VTR's BLIF readers)\n"),
                                 tc.log.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 3 names legalised (first: 'a b[0]' -> 'a%20b[0]')\n"),
        tc.log.data);
}

/* A top's name, how the VTR dialect writes it, and how many `names legalised` lines that logs. */
typedef struct model_case {
    const char *name;
    const char *written;
    uint32_t renames;
} model_case;

/* A top named want.name (hand IR: one buffer a -> y) is written `.model <want.written>`. */
static void expect_model_written(model_case want) {
    read_ok(TWO_MODELS, false); /* the directory and the sink */
    empty_design();
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(tc.design, intern(want.name), (odin3_prov_id){0}, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(tc.design, id));
    odin3_module *top = odin3_module_get(tc.design, id);
    odin3_net_id in = hand_scalar(top, "a", ODIN3_DIR_IN); /* ports in this order */
    hand_buffer(top, in, hand_scalar(top, "y", ODIN3_DIR_OUT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out),
                                  text_of(&tc.log));
    char header[LINE_BUF];
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, header);
    line_of("%s.model %s\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n", header, want.written);
    TEST_ASSERT_EQUAL_STRING(want_line, out.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.renames, count_of(tc.log.data, "names legalised"),
                                     tc.log.data);
    bvt_free(&tc);
}

/* The top's `.model` name takes VB-10 rules 1 and 2 (a blank, a `\`, a leading `.`) but never
 * rule 3, since a model name is no net (`NIL` stays); the rename is counted and, being the
 * file's first name, is the info line's `first` (3E-1 final review M3). */
static void test_model_name_legalised(void) {
    expect_model_written((model_case){"a b", "a%20b", 1});
    expect_model_written((model_case){".end", "%2Eend", 1});
    expect_model_written((model_case){"x\\y", "x%5Cy", 1});
    expect_model_written((model_case){"NIL", "NIL", 0});
    read_ok(TWO_MODELS, false);
    empty_design();
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(tc.design, intern("p q"), (odin3_prov_id){0}, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(tc.design, id));
    odin3_module *top = odin3_module_get(tc.design, id);
    odin3_net_id in = hand_scalar(top, "a b", ODIN3_DIR_IN); /* ports in this order */
    hand_buffer(top, in, hand_scalar(top, "y", ODIN3_DIR_OUT));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(tc.log.data,
               "info: write_blif: --vtr: 2 names legalised (first: 'p q' -> 'p%20q')\n"),
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
    /* %gnd is constant 1 (class 1): its reader reads the generator vcc (§9.2) */
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names vcc\n1\n.names a %vcc\n1 1\n"
                             ".names %gnd\n1\n.names %vcc vcc y\n11 1\n.end\n",
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

/* --- emission: header, generators, cells, latches, port buffers (3E Task 4) ------------------ */

/* One cell of each Yosys gate type (spec 3E §6's table). */
static const char *const GATES = ".model top\n"
                                 ".inputs a b s\n"
                                 ".outputs y0 y1 y2 y3 y4 y5 y6 y7 y8\n"
                                 ".subckt $_BUF_ A=a Y=y0\n"
                                 ".subckt $_NOT_ A=a Y=y1\n"
                                 ".subckt $_AND_ A=a B=b Y=y2\n"
                                 ".subckt $_OR_ A=a B=b Y=y3\n"
                                 ".subckt $_XOR_ A=a B=b Y=y4\n"
                                 ".subckt $_NAND_ A=a B=b Y=y5\n"
                                 ".subckt $_NOR_ A=a B=b Y=y6\n"
                                 ".subckt $_XNOR_ A=a B=b Y=y7\n"
                                 ".subckt $_MUX_ A=a B=b S=s Y=y8\n"
                                 ".end\n";

/* GATES written with --vtr, after the header and `.model top`. */
static const char *const GATES_BODY = ".inputs a b s\n"
                                      ".outputs y0 y1 y2 y3 y4 y5 y6 y7 y8\n"
                                      ".names a y0\n1 1\n"
                                      ".names a y1\n0 1\n"
                                      ".names a b y2\n11 1\n"
                                      ".names a b y3\n1- 1\n-1 1\n"
                                      ".names a b y4\n10 1\n01 1\n"
                                      ".names a b y5\n0- 1\n-0 1\n"
                                      ".names a b y6\n00 1\n"
                                      ".names a b y7\n11 1\n00 1\n"
                                      ".names a b s y8\n1-0 1\n-11 1\n"
                                      ".end\n";

/* The whole file: the header, a blank line, `.model top`, then the body. */
static void test_gate_covers(void) {
    char want[LINE_BUF * 4];
    read_ok(GATES, false);
    header_of(ODIN3_BLIF_VTR_DEFAULT_K, want);
    TEST_ASSERT_TRUE(strlen(want) + strlen(".model top\n") + strlen(GATES_BODY) < sizeof want);
    (void)strncat(want, ".model top\n", sizeof want - strlen(want) - 1);
    (void)strncat(want, GATES_BODY, sizeof want - strlen(want) - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_EQUAL_STRING(want, text_of(&out));
}

static void test_latch_init_map(void) {
    read_ok(".model top\n.inputs clk d0 d1 d2 d3\n.outputs q0 q1 q2 q3\n"
            ".latch d0 q0 re clk 0\n.latch d1 q1 re clk 1\n.latch d2 q2 re clk 2\n"
            ".latch d3 q3 re clk 3\n.end\n",
            false);
    TEST_ASSERT_EQUAL_STRING(".inputs clk d0 d1 d2 d3\n.outputs q0 q1 q2 q3\n"
                             ".latch d0 q0 re clk 0\n.latch d1 q1 re clk 1\n"
                             ".latch d2 q2 re clk 2\n.latch d3 q3 re clk 2\n.end\n",
                             write_vtr());
}

/* The VTR report of tc's design (preflight, default K), which must pass. */
static odin3_blif_vtr_report vtr_report(void) {
    odin3_blif_write_opts opts = {ODIN3_BLIF_DIALECT_VTR, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_blif_vtr_report report = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_vtr_preflight(tc.design, &opts, &report));
    return report;
}

static void test_latch_x_d(void) {
    read_ok(".model top\n.inputs d clk\n.outputs q\n.latch d q re clk 2\n.end\n", false);
    pin_to_const((odin3_node_id){node_of("$_DFF_P_")}, (pin_at){1, 0}, "$_CONSTX_");
    TEST_ASSERT_EQUAL_STRING(".inputs d clk\n.outputs q\n.names gnd\n.latch gnd q re clk 2\n.end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().x_to_gnd);
}

static void test_latch_const_clock(void) {
    odin3_module *top = hand_top();
    odin3_node_id dff = hand_dff_open_clock(top);
    pin_to_const(dff, (pin_at){0, 0}, "$_CONST1_");
    TEST_ASSERT_EQUAL_STRING(".inputs d\n.outputs q\n.names vcc\n1\n.latch d q re vcc 2\n.end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: top: $_DFF_P_ '$c%u': clock is "
                                      "the constant 1; VPR will never clock it\n",
                                      dff.v)),
        tc.log.data);
    bvt_free(&tc); /* a Parmys gnd (constant 0) and unconn (point (j)) as clocks */
    read_ok(".model top\n.inputs d\n.outputs q p\n.names gnd\n.names unconn\n"
            ".latch d q re gnd 2\n.latch d p re unconn 2\n.end\n",
            false);
    TEST_ASSERT_EQUAL_STRING(".inputs d\n.outputs q p\n.names gnd\n.names unconn\n"
                             ".latch d q re gnd 2\n.latch d p re unconn 2\n.end\n",
                             write_vtr());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tc.log.data, "warn: write_blif: --vtr: t.blif:6: $_DFF_P_ "
                                                     "'$c"),
                                 tc.log.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1,
                                     count_of(tc.log.data, "': clock is the constant 0; VPR "
                                                           "will never clock it\n"),
                                     tc.log.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1, count_of(tc.log.data, "': clock is 'unconn'; VPR will never clock it\n"), tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, count_of(tc.log.data, "feeds")); /* a clock is no logic pin */
}

/* Pin `port` (bit 0) of node onto net. */
static void pin_onto(odin3_node_id node, uint32_t port, odin3_net_id net) {
    odin3_module *top = top_of();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_connect(top, odin3_node_port(top, node, port).first, net));
}

/* An unconnected Q is written under its own `$p<pin ID>` (§7 item 5); an unconnected .names input
 * reads gnd (x_to_gnd), never a dangling name. */
static void test_unconnected_pins(void) {
    odin3_module *top = hand_top();
    odin3_net_id c_net = hand_scalar(top, "c", ODIN3_DIR_IN);
    odin3_net_id d_net = hand_scalar(top, "d", ODIN3_DIR_IN);
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    odin3_node_id dff = hand_cell(top, "$_DFF_P_", NULL, 0);
    pin_onto(dff, 0, c_net);
    pin_onto(dff, 1, d_net);
    odin3_node_id gate = hand_cell(top, "$_AND_", NULL, 0);
    pin_onto(gate, 0, d_net);
    pin_onto(gate, 2, y_net);
    TEST_ASSERT_EQUAL_STRING(line_of(".inputs c d\n.outputs y\n.names gnd\n.latch d $p%u re c 2\n"
                                     ".names d gnd y\n11 1\n.end\n",
                                     odin3_node_port(top, dff, 2).first.v),
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().x_to_gnd);
}

/* Point (j) on a hard cell's clock input: a constant (written gnd) and Parmys's unconn. */
static void test_hard_clock_warnings(void) {
    read_ok(RAM4, true);
    uint32_t ram = node_of("single_port_ram");
    pin_to_const((odin3_node_id){ram}, (pin_at){0, 0}, "$_CONST0_");
    write_vtr();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, " clk=gnd "), out.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: t.blif:4: single_port_ram '$c%u': "
                                      "clock input 'clk' is the constant 0; VPR will never clock "
                                      "it\n",
                                      ram)),
        tc.log.data);
    bvt_free(&tc);
    read_ok(".model top\n.inputs d a w\n.outputs o\n.names unconn\n"
            ".subckt single_port_ram clk=unconn data[0]=d addr[0]=a we=w out[0]=o\n.end\n",
            true);
    write_vtr();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, " clk=unconn "), out.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: t.blif:5: single_port_ram '$c%u': "
                                      "clock is 'unconn'; VPR will never clock it\n",
                                      node_of("single_port_ram"))),
        tc.log.data);
}

/* One 1-input .names on a with the rows given (blank-separated lines) written as want. */
static void expect_one_input(const char *rows, const char *want) {
    char text[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              ".model top\n.inputs a\n.outputs y\n.names a y\n"
                              "%s.end\n",
                              rows) < LINE_BUF);
    read_ok(text, false);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, write_vtr(), rows);
    bvt_free(&tc);
}

static void test_one_input_cover_normalised(void) {
    expect_one_input("", ".inputs a\n.outputs y\n.names y\n.end\n");
    expect_one_input("1 1\n", ".inputs a\n.outputs y\n.names a y\n1 1\n.end\n");
    expect_one_input("1 1\n1 1\n", ".inputs a\n.outputs y\n.names a y\n1 1\n.end\n");
    expect_one_input("0 1\n1 1\n0 1\n", ".inputs a\n.outputs y\n.names y\n1\n.end\n");
    expect_one_input("0 0\n", ".inputs a\n.outputs y\n.names a y\n1 1\n.end\n"); /* off-set */
    expect_one_input("1 0\n", ".inputs a\n.outputs y\n.names a y\n0 1\n.end\n");
    expect_one_input("- 0\n", ".inputs a\n.outputs y\n.names y\n.end\n");
}

static void test_constant_cover_with_inputs(void) {
    read_ok(".model top\n.inputs a b\n.outputs y\n.names a b y\n-- 1\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".inputs a b\n.outputs y\n.names y\n1\n.end\n", write_vtr());
    /* its dropped inputs are not counted: x on one of them is no x bit to gnd */
    pin_to_const((odin3_node_id){node_of("$sop")}, (pin_at){0, 0}, "$_CONSTX_");
    TEST_ASSERT_EQUAL_STRING(".inputs a b\n.outputs y\n.names y\n1\n.end\n", write_vtr());
    TEST_ASSERT_EQUAL_UINT32(0, vtr_report().x_to_gnd);
    bvt_free(&tc);
    read_ok(".model top\n.inputs a b\n.outputs y\n.names a b y\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".inputs a b\n.outputs y\n.names y\n.end\n", write_vtr());
}

/* The lines of text that are `.names y` or a .names line ending in ` y`. */
static uint32_t names_driving_y(const char *text) {
    uint32_t count = 0;
    for (const char *at = text; *at != '\0';) {
        const char *end = strchr(at, '\n');
        size_t len = end != NULL ? (size_t)(end - at) : strlen(at);
        bool names = starts_with(at, ".names ");
        count += names && len >= 2 && at[len - 2] == ' ' && at[len - 1] == 'y' ? 1U : 0U;
        at += end != NULL ? len + 1 : len;
    }
    return count;
}

/* Review Focus 4. */
static void test_const_sop_on_output_one_driver(void) {
    read_ok(".model top\n.outputs y\n.names y\n1\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".outputs y\n.names y\n1\n.end\n", write_vtr());
    TEST_ASSERT_NULL(strstr(out.data, "vcc"));
    TEST_ASSERT_EQUAL_UINT32(1, names_driving_y(out.data));
}

/* A constant cell whose net has another name than the output-port bit: the generator again under
 * the bit's name, no buffer (one driver per net, §9.2). */
static void test_const_on_output_named_net(void) {
    odin3_module *top = hand_top();
    odin3_node_id sop = hand_sop(top, 0, "1");
    odin3_net_id y_net = odin3_pin_net(top, odin3_node_port(top, sop, 1).first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top, y_net, intern("n")));
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names n\n1\n.names y\n1\n.end\n",
                             write_vtr());
    bvt_free(&tc);
    top = hand_top();
    sop = hand_sop(top, 2, "0-01-0"); /* a normalised constant 0 cell */
    y_net = odin3_pin_net(top, odin3_node_port(top, sop, 1).first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top, y_net, intern("n")));
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names n\n.names y\n.end\n", write_vtr());
}

/* A new cell of the constant type const_type driving net (a net with no driver). */
static void drive_const(odin3_net_id net, const char *const_type) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern(const_type), &type));
    odin3_node_spec spec = {.type = type};
    const odin3_netvec ports[] = {{&net, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top_of(), &spec, ports, NULL));
}

/* $_CONST1_ on output y, which the .names reads too. */
static void test_const_cell_on_output_buffered(void) {
    read_ok(".model top\n.inputs a\n.outputs y z\n.names a y z\n11 1\n.end\n", false);
    drive_const(net_named("y"), "$_CONST1_");
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y z\n.names vcc\n1\n.names a vcc z\n11 1\n"
                             ".names vcc y\n1 1\n.end\n",
                             write_vtr());
}

static void test_x_output(void) {
    read_ok(".model top\n.outputs y\n.end\n", false);
    TEST_ASSERT_EQUAL_STRING(".outputs y\n.names gnd\n.names gnd y\n1 1\n.end\n", write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().x_to_gnd);
    bvt_free(&tc); /* $_CONSTX_ on an output-port bit, and one undriven */
    odin3_module *top = hand_top();
    drive_const(hand_scalar(top, "y", ODIN3_DIR_OUT), "$_CONSTX_");
    (void)hand_scalar(top, "z", ODIN3_DIR_OUT);
    TEST_ASSERT_EQUAL_STRING(".outputs y z\n.names gnd\n.names gnd y\n1 1\n.names gnd z\n1 1\n"
                             ".end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32(2, vtr_report().x_to_gnd);
}

/* Parmys's unconn on a .names input and on a RAM address bit. */
static void test_unconn_logic_warns(void) {
    read_ok(".model top\n.inputs c d a0 a1 a3 w\n.outputs o y\n.names unconn\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 addr[1]=a1 addr[2]=unconn "
            "addr[3]=a3 we=w out[0]=o\n.names unconn a1 y\n11 1\n.end\n",
            true);
    write_vtr();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, " addr[2]=unconn "), out.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, "\n.names unconn a1 y\n11 1\n"), out.data);
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".names unconn\n"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1,
                                     count_of(tc.log.data, "warn: write_blif: --vtr: net 'unconn' "
                                                           "feeds 1 .names/.latch pins; VPR "
                                                           "leaves them open (run abc first)\n"),
                                     tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(1, tc.warnings);
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().unconn_logic);
}

static void test_generator_is_sop(void) {
    static const char *const body = ".inputs a\n.outputs y\n.names gnd\n.names a gnd y\n10 1\n"
                                    ".end\n";
    char text[LINE_BUF];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text, ".model top\n%s", body) < LINE_BUF);
    read_ok(text, false);
    TEST_ASSERT_EQUAL_STRING(body, write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".names gnd\n"));
    bvt_free(&tc); /* a signal named gnd, and the generator gnd for an undriven output */
    read_ok(".model top\n.inputs a\n.outputs y z\n.names a gnd\n1 1\n.names gnd y\n1 1\n.end\n",
            false);
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y z\n.names gnd\n.names a %gnd\n1 1\n"
                             ".names %gnd y\n1 1\n.names gnd z\n1 1\n.end\n",
                             write_vtr());
}

/* §5 item 4's order gnd, vcc, unconn, each once; a .subckt input reads x as unconn. */
static void test_generators_in_order(void) {
    read_ok(".model top\n.inputs x y z\n.outputs s c t\n"
            ".subckt adder a=x b=y cin=z cout=c sumout=s\n.names x y z t\n111 1\n.end\n",
            true);
    odin3_node_id adder = {node_of("adder")};
    pin_to_const(adder, (pin_at){2, 0}, "$_CONSTX_");
    pin_to_const(adder, (pin_at){1, 0}, "$_CONST1_");
    pin_to_const(adder, (pin_at){0, 0}, "$_CONST0_");
    pin_to_const((odin3_node_id){node_of("$sop")}, (pin_at){0, 2}, "$_CONST1_");
    TEST_ASSERT_EQUAL_STRING(".inputs x y z\n.outputs s c t\n.names gnd\n.names vcc\n1\n"
                             ".names unconn\n"
                             ".subckt adder a=gnd b=vcc cin=unconn cout=c sumout=s\n"
                             ".names x y vcc t\n111 1\n.end\n\n.model adder\n.inputs a b cin\n"
                             ".outputs cout sumout\n.blackbox\n.end\n",
                             write_vtr());
    odin3_blif_vtr_report report = vtr_report();
    TEST_ASSERT_EQUAL_UINT32(1, report.x_to_unconn);
    TEST_ASSERT_EQUAL_UINT32(0, report.x_to_gnd);
}

static void test_header_exact(void) {
    char first[LINE_BUF];
    read_ok(GATES, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    TEST_ASSERT_TRUE(snprintf(first, sizeof first,
                              "# Odin III %s write_blif --vtr (K=6, names short)\n\n.model top\n",
                              odin3_version_string()) < LINE_BUF);
    TEST_ASSERT_TRUE_MESSAGE(starts_with(text_of(&out), first), text_of(&out));
}

/* 3D's run information (AB-25 §8.1), as abc records it; the second run's differs in its depth. */
static const char *const ABC_RUN_1 = "script=a.abc sha256="
                                     "0123456789abcdef0123456789abcdef"
                                     "0123456789abcdef0123456789abcdef abc="
                                     "0123456789abcdef0123456789abcdef01234567 K=6 exchange=blif "
                                     "depth=3";
static const char *const ABC_RUN_2 = "script=a.abc sha256="
                                     "0123456789abcdef0123456789abcdef"
                                     "0123456789abcdef0123456789abcdef abc="
                                     "0123456789abcdef0123456789abcdef01234567 K=6 exchange=blif "
                                     "depth=4";

/* Opens a run named name directly (no pass named abc runs) and records info (NULL: none). */
static void passrun_with_info(const char *name, const char *info) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin_args(tc.design, intern(name), 0, &ctx));
    if (info != NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_passrun_set_info(&ctx, intern(info)));
    }
}

/* Plan 3D point (g): one `# abc:` line per run named abc with information, in run order, between
 * the `# Odin III` line and the blank line; other runs, and abc runs without information, none. */
static void test_vtr_header_abc_runs(void) {
    read_ok(GATES, false);
    passrun_with_info("abc", ABC_RUN_1);
    passrun_with_info("abc", NULL);
    passrun_with_info("other", "K=5");
    passrun_with_info("abc", ABC_RUN_2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr out.blif", &out), tc.log.data);
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&want,
                             "# Odin III %s write_blif --vtr (K=6, names short)\n# abc: %s\n"
                             "# abc: %s\n\n.model top\n",
                             odin3_version_string(), ABC_RUN_1, ABC_RUN_2));
    TEST_ASSERT_TRUE_MESSAGE(starts_with(text_of(&out), want.data), text_of(&out));
    odin3_strbuf_free(&want);
}

static void test_summary_line(void) {
    read_ok(".model top\n.inputs a b c d e w\n.outputs y1 y2 q s1 s2 s3 o\n"
            ".subckt $_AND_ A=a B=b Y=y1\n.subckt $_AND_ A=b B=a Y=y2\n.latch d q re c 2\n"
            ".subckt adder a=a b=b cin=d cout=k1 sumout=s1\n"
            ".subckt adder a=a b=b cin=k1 cout=k2 sumout=s2\n"
            ".subckt adder a=a b=b cin=k2 cout=k3 sumout=s3\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=e we=w out[0]=o\n.end\n",
            true);
    pin_to_const((odin3_node_id){node_of("single_port_ram")}, (pin_at){2, 0}, "$_CONSTX_");
    write_vtr();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, "info: write_blif: --vtr: 'top': 2 .names, 1 .latch, 4 .subckt "
                              "(adder 3, single_port_ram 1), K=6; x bits: 0 to gnd, 1 open "
                              "(unconn)\n"),
        tc.log.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, " addr=unconn "), out.data); /* 1 bit (§8.2) */
    bvt_free(&tc);
    read_ok(GATES, false);
    write_vtr();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1,
                                     count_of(tc.log.data, "info: write_blif: --vtr: 'top': 9 "
                                                           ".names, 0 .latch, 0 .subckt, K=6; x "
                                                           "bits: 0 to gnd, 0 open (unconn)\n"),
                                     tc.log.data);
}

static void test_other_modules_info(void) {
    read_ok(TWO_MODELS, false);
    write_vtr();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data,
                 "info: write_blif: --vtr: 1 modules besides the top 'top' not written\n"),
        tc.log.data);
    TEST_ASSERT_EQUAL_UINT32(1, count_of(out.data, ".model"));
}

static void test_deterministic(void) {
    odin3_strbuf first;
    odin3_strbuf_init(&first);
    read_ok(GATES, false);
    write_vtr();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&first, buf_of(&out)));
    write_vtr();
    TEST_ASSERT_EQUAL_STRING(text_of(&first), text_of(&out));
    odin3_strbuf_free(&first);
}

/* Controller ruling (Task 3 review, I2): --vtr writes no .cname/.attr/.param (VPR reads them in
 * extended BLIF only, R3); plain still does. */
static void test_vtr_no_extended_directives(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.cname n1\n.attr src \"x.v:3\"\n"
            ".param P 1\n.end\n",
            false);
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names a y\n1 1\n.end\n", write_vtr());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bvt_write(&tc, "out.blif", &out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n.cname n1\n.attr src \"x.v:3\"\n.param P 1\n"));
    bvt_free(&tc);
    read_golden("hand_cname");
    write_vtr();
    TEST_ASSERT_EQUAL_UINT32(0, count_of(out.data, ".cname"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(out.data, ".attr"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(out.data, ".param"));
}

/* A full --vtr write of GATES under an allocation failure at every step: NO_MEMORY with no file,
 * until it succeeds with the pass's bytes. */
static void test_out_of_memory_sweep_gates(void) {
    char path[PATH_BUF];
    read_ok(GATES, false);
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

/* --- Task 5: hard blocks, formals, clock pins and stubs (spec 3E §8) -------------------------- */

static odin3_strbuf flat;

/* Appends piece to text. */
static void put(odin3_strbuf *text, const char *piece) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(text, odin3_bytes_cstr(piece)));
}

/* text with its ` \` line continuations joined (the logical lines), in flat. */
static const char *unwrapped(const char *text) {
    static const char mark[] = " \\\n  ";
    odin3_strbuf_clear(&flat);
    for (const char *at = text; *at != '\0';) {
        const char *cont = strstr(at, mark);
        size_t len = cont != NULL ? (size_t)(cont - at) : strlen(at);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&flat, (odin3_bytes){at, len}));
        if (cont != NULL) {
            put(&flat, " ");
        }
        at += cont != NULL ? len + strlen(mark) : len;
    }
    return text_of(&flat);
}

/* The ID of the n-th (from 0) live node of the top whose type is named type. */
static uint32_t nth_node(const char *type, uint32_t n) {
    odin3_module *top = top_of();
    for (uint32_t i = 1; i < odin3_module_node_end(top); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(top, node) &&
            strcmp(odin3_celltype_get(tc.design, odin3_node_type(top, node))->name, type) == 0 &&
            n-- == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(type);
    return 0;
}

/* Appends ` <port>[0]=<net> … <port>[width-1]=<net>` to text. */
static void formals_on(odin3_strbuf *text, const char *port, uint32_t width, const char *net) {
    for (uint32_t k = 0; k < width; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(text, " %s[%u]=%s", port, (unsigned)k, net));
    }
}

/* Appends ` <word>[0] … <word>[width-1]` to text. */
static void bits_of(odin3_strbuf *text, const char *word, uint32_t width) {
    for (uint32_t k = 0; k < width; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text, " %s[%u]", word, (unsigned)k));
    }
}

/* A single_port_ram (15 address bits, 2 data bits) read from a file whose formals are in library
 * order (clk data addr we out) is written addr[0] … addr[14] clk data[0] data[1] out[0] out[1]
 * we (§8.2), and the undeclared cell's stub keeps the library order (§8.3). */
static void test_formals_sorted(void) {
    odin3_strbuf text;
    odin3_strbuf want;
    odin3_strbuf_init(&text);
    odin3_strbuf_init(&want);
    put(&text, ".model top\n.inputs c d a w\n"
               ".outputs o0 o1\n"
               ".subckt single_port_ram clk=c "
               "data[0]=d data[1]=d");
    formals_on(&text, "addr", 15, "a");
    put(&text, " we=w out[0]=o0 out[1]=o1\n"
               ".end\n");
    read_ok(text.data, true);
    put(&want, ".inputs c d a w\n.outputs o0 o1\n"
               ".subckt single_port_ram");
    formals_on(&want, "addr", 15, "a");
    put(&want, " clk=c data[0]=d data[1]=d out[0]=o0 "
               "out[1]=o1 we=w\n.end\n\n"
               ".model single_port_ram\n.inputs clk "
               "data[0] data[1]");
    bits_of(&want, "addr", 15);
    put(&want, " we\n.outputs out[0] out[1]\n"
               ".blackbox\n.end\n");
    TEST_ASSERT_EQUAL_STRING(want.data, unwrapped(write_vtr()));
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&want);
}

/* A 1-bit data slice: `data=` and `out=` (Parmys's scalars), in the formals and in the stub. */
static void test_ram_one_bit_slice_scalar(void) {
    read_ok(".model top\n.inputs c d a0 a1 w\n.outputs o\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 addr[1]=a1 we=w out[0]=o\n.end\n",
            true);
    TEST_ASSERT_EQUAL_STRING(".inputs c d a0 a1 w\n.outputs o\n"
                             ".subckt single_port_ram addr[0]=a0 addr[1]=a1 clk=c data=d out=o "
                             "we=w\n.end\n\n.model single_port_ram\n.inputs clk data addr[0] "
                             "addr[1] we\n.outputs out\n.blackbox\n.end\n",
                             unwrapped(write_vtr()));
}

/* An adder with cout unconnected: `cout=$p<pin ID>`, so every output bit is written and the
 * block keeps a name of its own (R12); two such adders get distinct names. */
static void test_subckt_unconnected_outputs_named(void) {
    read_ok(".model top\n.inputs x y z\n.outputs s1 s2\n"
            ".subckt adder a=x b=y cin=z sumout=s1\n.subckt adder a=x b=y cin=z sumout=s2\n.end\n",
            true);
    odin3_module *top = top_of();
    uint32_t cout1 = odin3_node_port(top, (odin3_node_id){nth_node("adder", 0)}, 3).first.v;
    uint32_t cout2 = odin3_node_port(top, (odin3_node_id){nth_node("adder", 1)}, 3).first.v;
    TEST_ASSERT_NOT_EQUAL(cout1, cout2);
    TEST_ASSERT_EQUAL_STRING(line_of(".inputs x y z\n.outputs s1 s2\n"
                                     ".subckt adder a=x b=y cin=z cout=$p%u sumout=s1\n"
                                     ".subckt adder a=x b=y cin=z cout=$p%u sumout=s2\n.end\n\n"
                                     ".model adder\n.inputs a b cin\n.outputs cout sumout\n"
                                     ".blackbox\n.end\n",
                                     cout1, cout2),
                             write_vtr());
}

/* A hand-built RAM (library loaded, so <loc> is the top) whose clk is on $_CONST0_: written
 * `clk=gnd`, with the hard-cell clock warning. */
static void test_ram_clock_constant(void) {
    odin3_module *top = hand_top();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_techlib_read(tc.design, ODIN3_LIB_DIR "/vtr.o3lib"));
    odin3_net_id d_net = hand_scalar(top, "d", ODIN3_DIR_IN);
    odin3_net_id a_net = hand_scalar(top, "a", ODIN3_DIR_IN);
    odin3_net_id w_net = hand_scalar(top, "w", ODIN3_DIR_IN);
    odin3_net_id o_net = hand_scalar(top, "o", ODIN3_DIR_OUT);
    odin3_value params[2] = {odin3_value_int(1), odin3_value_int(1)}; /* ADDR_WIDTH DATA_WIDTH */
    odin3_node_id ram = hand_cell(top, "single_port_ram", params, 2);
    pin_onto(ram, 0, const_net("$_CONST0_"));
    pin_onto(ram, 1, d_net);
    pin_onto(ram, 2, a_net);
    pin_onto(ram, 3, w_net);
    pin_onto(ram, 4, o_net);
    TEST_ASSERT_EQUAL_STRING(".inputs d a w\n.outputs o\n.names gnd\n"
                             ".subckt single_port_ram addr=a clk=gnd data=d out=o we=w\n.end\n\n"
                             ".model single_port_ram\n.inputs clk data addr we\n.outputs out\n"
                             ".blackbox\n.end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: top: single_port_ram '$c%u': clock "
                                      "input 'clk' is the constant 0; VPR will never clock it\n",
                                      ram.v)),
        tc.log.data);
}

/* Appends `.subckt multiply` with a[0..a-1] and b[0..b-1] on i and out[0] on out_net. */
static void multiply_line(odin3_strbuf *text, uint32_t a_width, uint32_t b_width,
                          const char *out_net) {
    put(text, ".subckt multiply");
    formals_on(text, "a", a_width, "i");
    formals_on(text, "b", b_width, "i");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text, " out[0]=%s\n", out_net));
}

/* Two multiply nodes with a widths 8 and 12 (b 8 and 4): the stub has each port at its widest,
 * a[0] … a[11], b[0] … b[7], out[0] … out[15], in library order (a b out). */
static void test_stub_width_max(void) {
    odin3_strbuf text;
    odin3_strbuf want;
    odin3_strbuf_init(&text);
    odin3_strbuf_init(&want);
    put(&text, ".model top\n.inputs i\n"
               ".outputs p q\n");
    multiply_line(&text, 8, 8, "p");
    multiply_line(&text, 12, 4, "q");
    put(&text, ".end\n");
    read_ok(text.data, true);
    const char *body = unwrapped(write_vtr());
    put(&want, "\n.end\n\n.model multiply\n"
               ".inputs");
    bits_of(&want, "a", 12);
    bits_of(&want, "b", 8);
    put(&want, "\n.outputs");
    bits_of(&want, "out", 16);
    put(&want, "\n.blackbox\n.end\n");
    TEST_ASSERT_TRUE_MESSAGE(ends_with(body, want.data), body);
    TEST_ASSERT_EQUAL_UINT32(1, count_of(body, ".blackbox"));
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&want);
}

/* A declared multiply with ports in the order b a out: its stub keeps that order (the formals are
 * still sorted). */
static void test_stub_declared_order_kept(void) {
    read_ok(".model top\n.inputs i\n.outputs o p q r\n"
            ".subckt multiply a[0]=i a[1]=i b[0]=i b[1]=i out[0]=o out[1]=p out[2]=q out[3]=r\n"
            ".end\n\n.model multiply\n.inputs b[0] b[1] a[0] a[1]\n"
            ".outputs out[0] out[1] out[2] out[3]\n.blackbox\n.end\n",
            true);
    TEST_ASSERT_EQUAL_STRING(".inputs i\n.outputs o p q r\n"
                             ".subckt multiply a[0]=i a[1]=i b[0]=i b[1]=i out[0]=o out[1]=p "
                             "out[2]=q out[3]=r\n.end\n\n.model multiply\n"
                             ".inputs b[0] b[1] a[0] a[1]\n.outputs out[0] out[1] out[2] out[3]\n"
                             ".blackbox\n.end\n",
                             unwrapped(write_vtr()));
}

/* A declared adder with no live node gets no stub (VPR checks every stub against the arch). */
static void test_stub_unused_dropped(void) {
    read_ok(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n\n"
            ".model adder\n.inputs a b cin\n.outputs cout sumout\n.blackbox\n.end\n",
            true);
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names a y\n1 1\n.end\n", write_vtr());
}

/* Task 4 review M1 (§7 item 3): a clock whose driver §6 writes as a constant generator (a cover
 * with inputs whose function is constant) warns as a constant clock does. */
static void test_clock_constant_cover_warns(void) {
    read_ok(".model top\n.inputs a d e w\n.outputs q o\n.names a k1\n- 1\n.names a k0\n"
            ".latch d q re k1 2\n"
            ".subckt single_port_ram clk=k0 data[0]=d addr[0]=e we=w out[0]=o\n.end\n",
            true);
    const char *body = write_vtr();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, ".names k1\n1\n.names k0\n.latch d q re k1 2\n"),
                                 body);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: t.blif:7: $_DFF_P_ '$c%u': clock "
                                      "is the constant 1; VPR will never clock it\n",
                                      node_of("$_DFF_P_"))),
        tc.log.data);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1,
        count_of(tc.log.data, line_of("warn: write_blif: --vtr: t.blif:8: single_port_ram '$c%u': "
                                      "clock input 'clk' is the constant 0; VPR will never clock "
                                      "it\n",
                                      node_of("single_port_ram"))),
        tc.log.data);
}

/* Task 4 review M2: the summary line is logged only once the file is written. */
static void test_summary_after_write(void) {
    read_ok(GATES, false);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, bvt_write(&tc, "--vtr no-such-dir/out.blif", &out));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, count_of(tc.log.data, "'top': 9 .names"), tc.log.data);
    write_vtr();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_of(tc.log.data, "'top': 9 .names"), tc.log.data);
}

/* Task 4 review M5: a constant 8-input cover (a truth table of 4 words) under --lut-k 8. */
static void test_constant_cover_k8(void) {
    read_ok(".model top\n.inputs a\n.outputs y z\n.names a a a a a a a a y\n-------- 1\n"
            ".names a a a a a a a a z\n1------- 1\n0------- 1\n.end\n",
            false);
    uint64_t tt[ODIN3_BLIF_VTR_TT_WORDS];
    TEST_ASSERT_TRUE(odin3_blif_vtr_eval(top_of(), (odin3_node_id){nth_node("$sop", 1)}, tt));
    for (uint32_t word = 0; word < 4; word++) {
        TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, tt[word]);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr --lut-k 8 out.blif", &out),
                                  tc.log.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out.data, "\n.names y\n1\n.names z\n1\n.end\n"), out.data);
}

/* Task 4 review M6: an output-port bit on Parmys's `unconn` net itself is buffered from unconn
 * (VPR: open), not written as a constant 0; a gate's unconnected output is its `$p<pin ID>`. */
static void test_output_on_unconn_and_open_gate_output(void) {
    odin3_module *top = hand_top();
    odin3_node_id sop = hand_sop(top, 0, "");
    odin3_net_id y_net = odin3_pin_net(top, odin3_node_port(top, sop, 1).first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top, y_net, intern("unconn")));
    TEST_ASSERT_EQUAL_STRING(".inputs a\n.outputs y\n.names unconn\n.names unconn y\n1 1\n.end\n",
                             write_vtr());
    bvt_free(&tc);
    top = hand_top();
    odin3_net_id a_net = hand_scalar(top, "a", ODIN3_DIR_IN);
    odin3_node_id gate = hand_cell(top, "$_AND_", NULL, 0);
    pin_onto(gate, 0, a_net);
    pin_onto(gate, 1, a_net);
    TEST_ASSERT_EQUAL_STRING(
        line_of(".inputs a\n.names a a $p%u\n11 1\n.end\n", odin3_node_port(top, gate, 2).first.v),
        write_vtr());
}

/* The OOM sweep over a write with stubs: a declared adder (as declared) and an undeclared RAM
 * (write_lib_stub's scratch). */
static void test_out_of_memory_sweep_stubs(void) {
    char path[PATH_BUF];
    read_ok(".model top\n.inputs c d a w x y\n.outputs o s\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a we=w out[0]=o\n"
            ".subckt adder a=x b=y sumout=s\n.end\n\n"
            ".model adder\n.inputs cin b a\n.outputs sumout cout\n.blackbox\n.end\n",
            true);
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
    const char *body = write_vtr();
    TEST_ASSERT_EQUAL_STRING(text_of(&out), text_of(&api));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "\n.end\n\n.model adder\n.inputs cin b a\n"
                                              ".outputs sumout cout\n.blackbox\n.end\n\n"
                                              ".model single_port_ram\n.inputs clk data addr we\n"
                                              ".outputs out\n.blackbox\n.end\n"),
                                 body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "a=x b=y cin=unconn cout=$p"), body);
    odin3_strbuf_free(&api);
}

/* Review I1 (spec 3E §8.2 erratum): an undeclared type used at widths 1 and 2 writes `p[k]` on
 * every instance, as its stub does, so our own reader reads the output back. */
static void test_mixed_width_formals_reread(void) {
    read_ok(".model top\n.inputs c d a0 a1 w\n.outputs o1 o2 o3\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 we=w out[0]=o1\n"
            ".subckt single_port_ram clk=c data[0]=d data[1]=d addr[0]=a0 addr[1]=a1 we=w "
            "out[0]=o2 out[1]=o3\n.end\n",
            true);
    const char *body = unwrapped(write_vtr());
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(body, ".subckt single_port_ram addr[0]=a0 clk=c data[0]=d out[0]=o1 we=w\n"
                     ".subckt single_port_ram addr[0]=a0 addr[1]=a1 clk=c data[0]=d data[1]=d "
                     "out[0]=o2 out[1]=o3 we=w\n.end\n\n.model single_port_ram\n"
                     ".inputs clk data[0] data[1] addr[0] addr[1] we\n.outputs out[0] out[1]\n"),
        body);
    odin3_strbuf written;
    odin3_strbuf_init(&written);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&written, buf_of(&out)));
    bvt_free(&tc);
    read_ok(written.data, true); /* IR-7b: the instances match the stub */
    odin3_strbuf_free(&written);
    write_vtr();
}

/* Review M3 (agent default 1): a declared 1-bit vector port stays `p[0]`, as declared, and the
 * output reads back to the same bytes. */
static void test_declared_one_bit_vector_kept(void) {
    static const char *const body =
        ".inputs c d a w\n.outputs o\n"
        ".subckt single_port_ram addr[0]=a clk=c data[0]=d out[0]=o we=w\n.end\n\n"
        ".model single_port_ram\n.inputs clk we addr[0] data[0]\n.outputs out[0]\n.blackbox\n"
        ".end\n";
    char text[LINE_BUF * 2];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text, ".model top\n%s", body) < (int)sizeof text);
    read_ok(text, true);
    TEST_ASSERT_EQUAL_STRING(body, write_vtr());
}

/* Review M7: a deleted hard node neither gets a stub nor widens one. */
static void test_stub_dead_nodes(void) {
    read_ok(".model top\n.inputs c d a0 a1 w x\n.outputs o1 o2 s\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 we=w out[0]=o1\n"
            ".subckt single_port_ram clk=c data[0]=d addr[0]=a0 addr[1]=a1 we=w out[0]=o2\n"
            ".subckt adder a=x b=x cin=x sumout=s\n.end\n",
            true);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_delete(top_of(), (odin3_node_id){nth_node("adder", 0)}));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_node_delete(top_of(), (odin3_node_id){nth_node("single_port_ram", 1)}));
    const char *body = write_vtr();
    TEST_ASSERT_NULL_MESSAGE(strstr(body, ".model adder"), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(body, ".subckt single_port_ram addr=a0 clk=c data=d out=o1 we=w\n"), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "\n.model single_port_ram\n.inputs clk data addr "
                                              "we\n.outputs out\n.blackbox\n.end\n"),
                                 body);
}

/* 2E-1's eleven IR change D cells (plain write_blif refuses them in write_cell): under --vtr the
 * preflight refuses each first, the asynchronous flip-flops with E1b (plan point (e)), the others
 * as word cells with E5. */
static void test_ir_change_d_cells_refused(void) {
    static const char *const async_ffs[] = {"$adffe", "$aldff", "$aldffe", "$dffsr", "$dffsre"};
    static const char *const word_cells[] = {"$sdffe",   "$sdffce",   "$dlatch",
                                             "$adlatch", "$dlatchsr", "$sr"};
    for (size_t i = 0; i < sizeof async_ffs / sizeof async_ffs[0]; i++) {
        odin3_node_id node = hand_cell(hand_top(), async_ffs[i], NULL, 0);
        expect_preflight_refused(
            line_of("write_blif: --vtr: top: %s '$c%u': %s", async_ffs[i], node.v, E1B));
        bvt_free(&tc);
    }
    for (size_t i = 0; i < sizeof word_cells / sizeof word_cells[0]; i++) {
        odin3_node_id node = hand_cell(hand_top(), word_cells[i], NULL, 0);
        expect_preflight_refused(line_of("write_blif: --vtr: top: %s '$c%u': not a netlist cell "
                                         "(run lower)",
                                         word_cells[i], node.v));
        bvt_free(&tc);
    }
}

/* --- $lut (3E-5, plan 3E point (b): the --vtr row and its normalisation) ------------------- */

enum { LUT_TEST_MAX_K = 7 };

/* A $lut's WIDTH and table: bit i of `table` is LUT[i] (i taken mod 64 above 6 inputs). */
typedef struct lut_case {
    uint32_t width;
    uint64_t table;
} lut_case;

/* tc's design: a hand top with scalar inputs a, b, … (WIDTH of them) and output y, and one $lut
 * of the case on them (A[j] = the j-th input) driving y. Its node. */
static odin3_node_id hand_lut(lut_case lc) {
    static const char *const NAMES[LUT_TEST_MAX_K] = {"a", "b", "c", "d", "e", "f", "g"};
    static uint8_t bits[1U << LUT_TEST_MAX_K];
    TEST_ASSERT_TRUE(lc.width >= 1 && lc.width <= LUT_TEST_MAX_K);
    odin3_module *top = hand_top();
    odin3_net_id ins[LUT_TEST_MAX_K];
    for (uint32_t j = 0; j < lc.width; j++) {
        ins[j] = hand_scalar(top, NAMES[j], ODIN3_DIR_IN);
    }
    odin3_net_id y_net = hand_scalar(top, "y", ODIN3_DIR_OUT);
    for (uint32_t i = 0; i < 1U << lc.width; i++) {
        bits[i] = (uint8_t)(((lc.table >> (i % 64U)) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    odin3_value params[2] = {odin3_value_int(lc.width), {.kind = ODIN3_VAL_BITS}};
    params[1].bits = bits;
    params[1].len = 1U << lc.width;
    odin3_celltype_id lut = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(tc.design, intern("$lut"), &lut));
    odin3_node_spec spec = {.type = lut, .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{ins, lc.width}, {&y_net, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, &node));
    return node;
}

/* hand_lut(lc) written with --vtr is want (after `.model top`); it counts as one .names. */
static void expect_lut_vtr(lut_case lc, const char *want) {
    (void)hand_lut(lc);
    TEST_ASSERT_EQUAL_STRING(want, write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().names);
    bvt_free(&tc);
}

/* The plain writer's cells (test_blif_writer's lut_plain) give the same rows under --vtr: the
 * shorter ISOP, the off-set's when it has fewer rows. */
static void test_lut_vtr_rows(void) {
    expect_lut_vtr((lut_case){2, 0x8}, ".inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n");
    expect_lut_vtr((lut_case){3, 0xE8},
                   ".inputs a b c\n.outputs y\n.names a b c y\n-11 1\n1-1 1\n11- 1\n.end\n");
    expect_lut_vtr((lut_case){2, 0xE}, ".inputs a b\n.outputs y\n.names a b y\n00 0\n.end\n");
    expect_lut_vtr((lut_case){2, 0x0}, ".inputs a b\n.outputs y\n.names y\n.end\n");
}

/* A 1-input LUT has one row (§6 normalisation 2). */
static void test_lut_vtr_one_input(void) {
    expect_lut_vtr((lut_case){1, 0x2}, ".inputs a\n.outputs y\n.names a y\n1 1\n.end\n");
    expect_lut_vtr((lut_case){1, 0x1}, ".inputs a\n.outputs y\n.names a y\n0 1\n.end\n");
}

/* A constant LUT is the zero-input generator under its output (§6 normalisation 1), for the
 * width classes 1 (part of a word), 3 and 6 (a whole word); its dropped inputs read no x. */
static void test_lut_vtr_constant_widths(void) {
    static const uint32_t widths[] = {1, 3, 6};
    static const char *const inputs[] = {".inputs a\n", ".inputs a b c\n", ".inputs a b c d e f\n"};
    char want[LINE_BUF];
    for (size_t i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        TEST_ASSERT_TRUE(snprintf(want, sizeof want, "%s.outputs y\n.names y\n1\n.end\n",
                                  inputs[i]) < (int)sizeof want);
        expect_lut_vtr((lut_case){widths[i], UINT64_MAX}, want);
        TEST_ASSERT_TRUE(snprintf(want, sizeof want, "%s.outputs y\n.names y\n.end\n", inputs[i]) <
                         (int)sizeof want);
        expect_lut_vtr((lut_case){widths[i], 0}, want);
    }
    odin3_node_id lut = hand_lut((lut_case){3, UINT64_MAX});
    pin_to_const(lut, (pin_at){0, 1}, "$_CONSTX_");
    TEST_ASSERT_EQUAL_STRING(".inputs a b c\n.outputs y\n.names y\n1\n.end\n", write_vtr());
    TEST_ASSERT_EQUAL_UINT32(0, vtr_report().x_to_gnd);
}

/* E7 counts a LUT's stored inputs (point (g)): WIDTH 7 under K = 6 is refused, a constant one
 * too; --lut-k 7 writes it. An x input of a LUT reads gnd (§9.2). */
static void test_lut_vtr_e7_and_x(void) {
    odin3_node_id lut = hand_lut((lut_case){7, 0x8});
    const char *want = line_of("write_blif: --vtr: top: $lut '$c%u': 7 inputs exceed K=6 (run "
                               "abc, or pass --lut-k)",
                               lut.v);
    expect_refused((refusal){"--vtr --lut-k 6 out.blif", want});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bvt_write(&tc, "--vtr --lut-k 7 out.blif", &out),
                                  tc.log.data);
    bvt_free(&tc);
    lut = hand_lut((lut_case){7, 0});
    expect_refused((refusal){"--vtr --lut-k 6 out.blif",
                             line_of("write_blif: --vtr: top: $lut '$c%u': 7 inputs exceed K=6 "
                                     "(run abc, or pass --lut-k)",
                                     lut.v)});
    bvt_free(&tc);
    lut = hand_lut((lut_case){2, 0x8});
    pin_to_const(lut, (pin_at){0, 1}, "$_CONSTX_");
    TEST_ASSERT_EQUAL_STRING(".inputs a b\n.outputs y\n.names gnd\n.names a gnd y\n11 1\n.end\n",
                             write_vtr());
    TEST_ASSERT_EQUAL_UINT32(1, vtr_report().x_to_gnd);
}

/* A --vtr write of a LUT (the ISOP's arena and cover): NO_MEMORY with no file until it succeeds,
 * then the same bytes as without the sweep. */
static void test_out_of_memory_sweep_lut(void) {
    char path[PATH_BUF];
    (void)hand_lut((lut_case){3, 0xE8});
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

/* 3E-5: $lut. */
static void run_task_9(void) {
    RUN_TEST(test_lut_vtr_rows);
    RUN_TEST(test_lut_vtr_one_input);
    RUN_TEST(test_lut_vtr_constant_widths);
    RUN_TEST(test_lut_vtr_e7_and_x);
    RUN_TEST(test_out_of_memory_sweep_lut);
}

/* Task 5: hard blocks. */
static void run_task_5(void) {
    RUN_TEST(test_formals_sorted);
    RUN_TEST(test_ram_one_bit_slice_scalar);
    RUN_TEST(test_subckt_unconnected_outputs_named);
    RUN_TEST(test_ram_clock_constant);
    RUN_TEST(test_stub_width_max);
    RUN_TEST(test_stub_declared_order_kept);
    RUN_TEST(test_stub_unused_dropped);
    RUN_TEST(test_clock_constant_cover_warns);
    RUN_TEST(test_summary_after_write);
    RUN_TEST(test_constant_cover_k8);
    RUN_TEST(test_output_on_unconn_and_open_gate_output);
    RUN_TEST(test_out_of_memory_sweep_stubs);
    RUN_TEST(test_mixed_width_formals_reread);
    RUN_TEST(test_declared_one_bit_vector_kept);
    RUN_TEST(test_stub_dead_nodes);
    RUN_TEST(test_ir_change_d_cells_refused);
    odin3_strbuf_free(&flat);
}

/* Task 4: emission. */
static void run_task_4(void) {
    RUN_TEST(test_gate_covers);
    RUN_TEST(test_latch_init_map);
    RUN_TEST(test_latch_x_d);
    RUN_TEST(test_latch_const_clock);
    RUN_TEST(test_unconnected_pins);
    RUN_TEST(test_hard_clock_warnings);
    RUN_TEST(test_one_input_cover_normalised);
    RUN_TEST(test_constant_cover_with_inputs);
    RUN_TEST(test_const_sop_on_output_one_driver);
    RUN_TEST(test_const_on_output_named_net);
    RUN_TEST(test_const_cell_on_output_buffered);
    RUN_TEST(test_x_output);
    RUN_TEST(test_unconn_logic_warns);
    RUN_TEST(test_generator_is_sop);
    RUN_TEST(test_generators_in_order);
    RUN_TEST(test_header_exact);
    RUN_TEST(test_vtr_header_abc_runs);
    RUN_TEST(test_summary_line);
    RUN_TEST(test_other_modules_info);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_vtr_no_extended_directives);
    RUN_TEST(test_out_of_memory_sweep_gates);
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
    RUN_TEST(test_model_name_legalised);
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
    run_task_4();
    run_task_5();
    run_task_9();
    return UNITY_END();
}
