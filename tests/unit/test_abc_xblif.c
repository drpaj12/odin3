/*
 * test_abc_xblif.c — the BLIF exchange's files (3D plan Task 16; spec §6.2, AB-13, AB-15): the
 * input BLIF of a region byte for byte, the command file, and the read-back with every §11
 * malformed-network text. Fixture t1: a, b, clk inputs; n = a & b; a flip-flop q (C = clk, D = n)
 * driving the output y.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "ir/lut.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/prov.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/abc/xblif.h"
#include "passes/manager.h"
#include "util/alloc.h"

#include <inttypes.h>

enum { OOM_LIMIT = 4000 };

static odin3_design *design;
static odin3_module *mod;
static odin3_pass_ctx ctx;
static odin3_abc_run *run;
static odin3_vec domains;
static odin3_abc_region region;
static odin3_net_id n_net;
static char dir_arg[AFX_PATH + 32];

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    run = NULL;
    region = (odin3_abc_region){0};
    domains = (odin3_vec){0};
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_region_free(&region);
    odin3_abc_domains_free(&domains);
    odin3_abc_run_destroy(run);
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* t1 with the flip-flop's INIT. */
static void build_t1(int64_t init) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id in_b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id clk = afx_port(design, mod, "clk", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    n_net = afx_net(design, mod, "n");
    odin3_net_id and_nets[3] = {in_a, in_b, n_net};
    (void)AFX_CELL(design, mod, "$_AND_", "g", and_nets, 3);
    odin3_value params[1] = {odin3_value_int(init)};
    odin3_net_id dff_nets[3] = {clk, n_net, out_y};
    (void)AFX_CELL_P(design, mod, "$_DFF_P_", params, dff_nets, 3);
    afx_check(design);
}

/* Starts a run with extra arguments and builds the region of t1's only domain. */
static void start_region(const char *extra) {
    char dir[AFX_PATH];
    afx_path(dir, "xb");
    afx_mkdir(dir);
    (void)snprintf(dir_arg, sizeof dir_arg, "--via-blif %s %s", dir, extra);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, afx_intern(design, "abc"), &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_start(&ctx, odin3_bytes_cstr(dir_arg), &run));
    run->module = mod;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_EQUAL_UINT64(1, domains.len);
    run->domain = domains.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_region_build(run, run->domain, &region));
    run->region = &region;
}

static void expect_in_blif(const char *want) {
    char path[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_write(run, &region, path));
    TEST_ASSERT_EQUAL_STRING(want, afx_slurp(path));
}

static void test_input_init1(void) {
    build_t1(1);
    start_region("--lut 6");
    char want[1024];
    (void)snprintf(want, sizeof want,
                   ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0\n"
                   ".names o3ci0 o3ci1 o3n%" PRIu32 "\n11 1\n"
                   ".names o3n%" PRIu32 " o3d0\n0 1\n.latch o3d0 o3q0 0\n.names o3q0 o3r0\n0 1\n"
                   ".names o3r0 o3co0\n1 1\n.end\n",
                   n_net.v, n_net.v);
    expect_in_blif(want);
}

/* INIT 0 has no inverters; the CO buffer reads o3q0. INIT 2 and 3 are sent at 0 as well. */
static void test_input_init0_2_3(void) {
    for (int64_t init = 0; init <= 3; init += (init == 0 ? 2 : 1)) {
        odin3_design_destroy(design);
        design = odin3_design_create();
        odin3_abc_run_destroy(run);
        run = NULL;
        odin3_abc_region_free(&region);
        odin3_abc_domains_free(&domains);
        build_t1(init);
        start_region("--lut 6");
        char want[1024];
        (void)snprintf(want, sizeof want,
                       ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0\n"
                       ".names o3ci0 o3ci1 o3n%" PRIu32 "\n11 1\n"
                       ".latch o3n%" PRIu32 " o3q0 0\n.names o3q0 o3co0\n1 1\n.end\n",
                       n_net.v, n_net.v);
        expect_in_blif(want);
    }
}

static void test_command_file(void) {
    build_t1(0);
    start_region("--lut 6");
    char in[AFX_PATH];
    char out[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, in, sizeof in));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_OUT, out, sizeof out));
    odin3_strbuf cmds;
    odin3_strbuf_init(&cmds);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_abc_xblif_commands(run, &(odin3_abc_paths){in, out}, &cmds));
    char want[2048];
    (void)snprintf(want, sizeof want,
                   "read \"%s\"\nstrash\nifraig -v\nscorr -v\ndc2 -v\ndch -f\nif -K 6 -v\nmfs2 -v\n"
                   "print_stats\nwrite_hie \"%s\" \"%s\"\n",
                   in, in, out);
    TEST_ASSERT_EQUAL_STRING(want, cmds.data);
    odin3_strbuf_free(&cmds);
}

/* Two writes of t1 give identical bytes. */
static void test_write_is_deterministic(void) {
    build_t1(1);
    start_region("--lut 6");
    char path[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_write(run, &region, path));
    char *first = strdup(afx_slurp(path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_write(run, &region, path));
    TEST_ASSERT_EQUAL_STRING(first, afx_slurp(path));
    free(first);
}

/* One two-input gate with both inputs on the same net a, driving the output y. */
static void build_dup(const char *type) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id nets[3] = {in_a, in_a, out_y};
    (void)AFX_CELL(design, mod, type, "g", nets, 3);
    afx_check(design);
}

/* The one `.names` line and rows the input BLIF has for the gate: header, then rows. */
static void expect_dup_cover(const char *type, const char *want_names_and_rows) {
    build_dup(type);
    start_region("--lut 6");
    char path[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_write(run, &region, path));
    const char *text = afx_slurp(path);
    const char *at = strstr(text, ".names");
    TEST_ASSERT_NOT_NULL(at);
    const char *end = strstr(text, ".end");
    TEST_ASSERT_NOT_NULL(end);
    char got[512];
    (void)snprintf(got, sizeof got, "%.*s", (int)(end - at), at);
    TEST_ASSERT_EQUAL_STRING(want_names_and_rows, got);
}

/* A == B: the columns merge. XOR is constant 0 (an empty cover would be rejected by strash). */
static void test_dup_fanin_xor_is_constant_0(void) {
    expect_dup_cover("$_XOR_", ".names o3co0\n");
}

/* A 2-input $lut (A | ~B, one OFF-set row 01 or 10) on one net twice: the table is constant 1. */
static void test_dup_fanin_lut_or_not_is_constant_1(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    static const uint64_t k_table = 0xB;
    odin3_value params[2];
    odin3_arena *arena = odin3_arena_create(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_params(arena, 2, &k_table, params));
    odin3_net_id a_nets[2] = {in_a, in_a};
    odin3_netvec ports[2] = {{a_nets, 2}, {&out_y, 1}};
    odin3_node_spec spec = {odin3_abc_lut_type(design), 0, (odin3_prov_id){0}, params, 2};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    odin3_arena_destroy(arena);
    afx_check(design);
    start_region("--lut 6");
    char path[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_write(run, &region, path));
    TEST_ASSERT_NOT_NULL(strstr(afx_slurp(path), ".names o3co0\n1\n.end"));
}

static void test_dup_fanin_xnor_keeps_both_rows(void) {
    expect_dup_cover("$_XNOR_", ".names o3ci0 o3co0\n0 1\n1 1\n");
}

static void test_dup_fanin_and_is_a_buffer(void) {
    expect_dup_cover("$_AND_", ".names o3ci0 o3co0\n1 1\n");
}

static void test_dup_fanin_nand_survivor_row(void) {
    expect_dup_cover("$_NAND_", ".names o3ci0 o3co0\n1 0\n");
}

/* --- read-back ----------------------------------------------------------------------------- */

/* One crafted output and the one error it must give. */
typedef struct malformed_case {
    const char *file;
    const char *text;
    const char *why;
} malformed_case;

/* Reads a crafted output and expects exactly the one error line. */
static void expect_malformed(const malformed_case *test) {
    char path[AFX_PATH];
    afx_path(path, test->file);
    AFX_WRITE(path, test->text);
    odin3_abc_result res;
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_ENGINE, odin3_abc_xblif_read(run, path, &res),
                                  test->why);
    char want[AFX_LINE];
    (void)snprintf(want, sizeof want, "abc: ABC returned a malformed network: %s", test->why);
    TEST_ASSERT_EQUAL_STRING(want, afx_only(ODIN3_LOG_ERROR, "abc:"));
    odin3_abc_result_free(&res);
}

#define OUT_HEAD ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0\n"
#define MANY_INPUTS "o3ci0 o3ci1 o3ci0 o3ci1 o3ci0 o3ci1 o3ci0 o3ci1 o3ci0 o3ci1 o3ci0 o3ci1 o3ci0"

static void test_readback_errors(void) {
    static const malformed_case k_cases[] = {
        {"r1.blif",
         ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co9\n.names o3ci0 o3co9\n1 1\n.end\n",
         "an unknown output 'o3co9'"},
        {"r2.blif", OUT_HEAD ".subckt foo a=o3ci0 y=o3co0\n.end\n", "a .subckt"},
        {"r3.blif", OUT_HEAD ".latch o3ci0 o3q0 1\n.names o3q0 o3co0\n1 1\n.end\n",
         "a latch with init 1 (all were sent at 0)"},
        {"r4.blif",
         OUT_HEAD ".names " MANY_INPUTS
                  " o3ci1 o3ci0 o3ci1 o3ci0 o3co0\n11111111111111111 1\n.end\n",
         "a LUT with 17 inputs (at most 16)"},
        {"r5.blif", OUT_HEAD ".names o3ci0 o3ci1 o3ci0 o3ci1 o3ci0 o3co0\n11111 1\n.end\n",
         "a LUT with 5 inputs exceeds --lut 4"},
        {"r6.blif",
         OUT_HEAD ".latch o3ci0 o3q0 0\n.latch o3ci1 o3q1 0\n.names o3q0 o3co0\n1 1\n.end\n",
         "2 registers, more than the 1 it was given"},
        {"r7.blif", OUT_HEAD ".latch o3ci0 foo 0\n.names foo o3co0\n1 1\n.end\n",
         "register 0 carries no input register's name"},
        {"r8.blif",
         ".model o3abc\n.inputs o3ci0 o3ci1 o3ci2\n.outputs o3co0\n.names o3ci0 o3co0\n1 1\n.end\n",
         "3 primary inputs, expected 2"},
        {"r9.blif",
         ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0 o3co1\n.names o3ci0 o3co0\n1 1\n"
         ".names o3ci1 o3co1\n1 1\n.end\n",
         "2 outputs, expected 1"},
    };
    build_t1(0);
    start_region("--lut 4");
    for (size_t idx = 0; idx < sizeof k_cases / sizeof k_cases[0]; idx++) {
        expect_malformed(&k_cases[idx]);
    }
}

/* A missing output file is the carry-over of 3D-5a: a diagnosed error, never a success. */
static void test_readback_missing_file(void) {
    build_t1(0);
    start_region("--lut 4");
    char path[AFX_PATH];
    afx_path(path, "does-not-exist.blif");
    odin3_abc_result res;
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_xblif_read(run, path, &res));
    TEST_ASSERT_EQUAL_STRING("abc: ABC returned a malformed network: no output network",
                             afx_only(ODIN3_LOG_ERROR, "abc:"));
}

/* A one-input `1 1` .names is a connection: the CO is driven by the AND LUT's object itself. */
static void test_readback_buffer_is_a_connection(void) {
    build_t1(0);
    start_region("--lut 6");
    char path[AFX_PATH];
    afx_path(path, "ok.blif");
    AFX_WRITE(path, ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0\n.names o3ci0 o3ci1 w\n11 "
                    "1\n.names w d\n1 1\n.latch d o3q0 0\n.names o3q0 o3co0\n1 1\n.end\n");
    odin3_abc_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_xblif_read(run, path, &res));
    TEST_ASSERT_EQUAL_UINT32(2, res.n_cis);
    TEST_ASSERT_EQUAL_UINT32(1, res.n_regs_out);
    TEST_ASSERT_EQUAL_UINT32(1, res.n_luts);
    TEST_ASSERT_EQUAL_UINT32(1, res.n_cos);
    uint32_t reg_obj = 2 + res.n_cis;
    uint32_t lut_obj = odin3_abc_result_first_lut(&res);
    const uint32_t *drivers = res.co_driver.data;
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)res.co_driver.len);
    TEST_ASSERT_EQUAL_UINT32(reg_obj, drivers[0]); /* y = the register */
    TEST_ASSERT_EQUAL_UINT32(lut_obj, drivers[1]); /* the register's input = the AND */
    TEST_ASSERT_EQUAL_UINT32(0, ((const uint32_t *)res.reg_from.data)[0]);
    const uint64_t *truth = res.truth.data;
    TEST_ASSERT_EQUAL_HEX64(0x8, truth[0]); /* AND of two inputs */
    odin3_abc_result_free(&res);
}

/* One write and one read-back with allocation `at` failing; true when both worked. */
static bool oom_round(long at, const odin3_abc_paths *files) {
    odin3_abc_result res;
    odin3_util_set_alloc_fail_after(at);
    odin3_status st_w = odin3_abc_xblif_write(run, &region, files->in);
    odin3_status st_r = odin3_abc_xblif_read(run, files->out, &res);
    odin3_util_set_alloc_fail_after(-1);
    /* the reader's lexer reports a failed buffer allocation as IO */
    bool write_ok = st_w == ODIN3_OK || st_w == ODIN3_ERR_NO_MEMORY || st_w == ODIN3_ERR_IO;
    bool read_ok = st_r == ODIN3_OK || st_r == ODIN3_ERR_NO_MEMORY || st_r == ODIN3_ERR_IO;
    TEST_ASSERT_TRUE_MESSAGE(write_ok && read_ok, "an injected failure gave another status");
    if (st_r == ODIN3_OK) {
        odin3_abc_result_free(&res);
    }
    return st_w == ODIN3_OK && st_r == ODIN3_OK;
}

/* An OOM sweep over the write and the read-back: NO_MEMORY, no leak, a fresh try works. */
static void test_oom_sweep(void) {
    build_t1(1);
    start_region("--lut 6");
    char in[AFX_PATH];
    char out[AFX_PATH];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, in, sizeof in));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_path(run, ODIN3_ABC_FILE_OUT, out, sizeof out));
    AFX_WRITE(out, ".model o3abc\n.inputs o3ci0 o3ci1\n.outputs o3co0\n.names o3ci0 o3ci1 w\n11 "
                   "1\n.latch w o3q0 0\n.names o3q0 o3co0\n1 1\n.end\n");
    uint32_t failures = 0;
    long at = 0;
    while (at < OOM_LIMIT && !oom_round(at, &(odin3_abc_paths){in, out})) {
        failures++;
        at++;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
    TEST_ASSERT_TRUE(at < OOM_LIMIT);
    afx_check(design);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_input_init1);
    RUN_TEST(test_input_init0_2_3);
    RUN_TEST(test_command_file);
    RUN_TEST(test_write_is_deterministic);
    RUN_TEST(test_readback_errors);
    RUN_TEST(test_readback_missing_file);
    RUN_TEST(test_readback_buffer_is_a_connection);
    RUN_TEST(test_dup_fanin_xor_is_constant_0);
    RUN_TEST(test_dup_fanin_lut_or_not_is_constant_1);
    RUN_TEST(test_dup_fanin_xnor_keeps_both_rows);
    RUN_TEST(test_dup_fanin_and_is_a_buffer);
    RUN_TEST(test_dup_fanin_nand_survivor_row);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
