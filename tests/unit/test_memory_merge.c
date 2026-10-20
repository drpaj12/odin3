/*
 * test_memory_merge.c — the `memory_merge` pass (4.2 spec G-1, G-4, G-5, T-7; plan Task 5). The
 * `$mem` is built as 2D EL-39 emits it (asynchronous read, raw address, RD_EN 1) with the
 * flip-flops R-8 merges around it, from flat RTLIL (pb_build); the pass runs through the pass
 * manager (check FULL before and after). Value-preserving cases compare hardmap_ramsim.c traces of
 * the IR before and after the pass.
 */
#include "hardmap_fixture.h"
#include "hardmap_ramsim.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/partial_map/mem.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 8192,
    TEXT_SIZE = 4096,
    SIM_CYCLES = 1500,
    OOM_LIMIT = 4000,
    ABITS = 4,
    WIDTH = 3
};

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* --- the design ---------------------------------------------------------------------------- */

typedef enum wr_mode { WR_NONE, WR_CLK, WR_CLK2 } wr_mode;

/* One RTLIL text: a 16 x 3 `$mem`, one read port, the read-side cells and the write side. */
typedef struct variant {
    const char *wires;   /* extra wire declarations */
    const char *ff;      /* the flip-flop cell */
    const char *rd_addr; /* the net on RD_ADDR */
    const char *rd_data; /* the net on RD_DATA */
    wr_mode wr;          /* the write port */
} variant;

static const char k_ff_data[] =
    "  cell $dff \\ff\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 1\n"
    "    connect \\CLK \\clk\n    connect \\D \\rd\n    connect \\Q \\q\n  end\n";
static const char k_ff_dffe_one[] =
    "  cell $dffe \\ff\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 1\n"
    "    parameter \\EN_POLARITY 1\n    connect \\EN 1'1\n    connect \\CLK \\clk\n"
    "    connect \\D \\rd\n    connect \\Q \\q\n  end\n";
static const char k_ff_dffe_var[] =
    "  cell $dffe \\ff\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 1\n"
    "    parameter \\EN_POLARITY 1\n    connect \\EN \\ren\n    connect \\CLK \\clk\n"
    "    connect \\D \\rd\n    connect \\Q \\q\n  end\n";
static const char k_ff_dffe_low[] =
    "  cell $dffe \\ff\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 1\n"
    "    parameter \\EN_POLARITY 0\n    connect \\EN \\ren\n    connect \\CLK \\clk\n"
    "    connect \\D \\rd\n    connect \\Q \\q\n  end\n";
static const char k_ff_falling[] =
    "  cell $dff \\ff\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 0\n"
    "    connect \\CLK \\clk\n    connect \\D \\rd\n    connect \\Q \\q\n  end\n";
static const char k_ff_addr[] =
    "  cell $dff \\ff\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"
    "    connect \\CLK \\clk\n    connect \\D \\ra\n    connect \\Q \\rq\n  end\n";
static const char k_wire_rd[] = "  wire width 3 \\rd\n";
static const char k_wire_rq[] = "  wire width 4 \\rq\n";

static const variant k_data = {k_wire_rd, k_ff_data, "\\ra", "\\rd", WR_CLK};
static const variant k_dffe_one = {k_wire_rd, k_ff_dffe_one, "\\ra", "\\rd", WR_CLK};
static const variant k_dffe_var = {k_wire_rd, k_ff_dffe_var, "\\ra", "\\rd", WR_CLK};
static const variant k_dffe_low = {k_wire_rd, k_ff_dffe_low, "\\ra", "\\rd", WR_CLK};
static const variant k_falling = {k_wire_rd, k_ff_falling, "\\ra", "\\rd", WR_CLK};
static const variant k_addr_rom = {k_wire_rq, k_ff_addr, "\\rq", "\\q", WR_NONE};
static const variant k_addr_clk2 = {k_wire_rq, k_ff_addr, "\\rq", "\\q", WR_CLK2};
static const variant k_addr_wr = {k_wire_rq, k_ff_addr, "\\rq", "\\q", WR_CLK};
static const variant k_async = {"", "", "\\ra", "\\q", WR_CLK};

static void mem_text(char *out, size_t size, const variant *var, const char *q_attr) {
    const char *wr_clk = var->wr == WR_CLK2 ? "\\clk2" : "\\clk";
    unsigned n_wr = var->wr == WR_NONE ? 0U : 1U;
    int len = snprintf(
        out, size,
        "module \\m\n  wire width 1 input 1 \\clk\n  wire width 1 input 2 \\clk2\n"
        "  wire width 4 input 3 \\ra\n  wire width 4 input 4 \\wa\n  wire width 3 input 5 \\wd\n"
        "  wire width 1 input 6 \\we\n  wire width 1 input 7 \\ren\n%s  wire width 3 output 8 \\q\n"
        "%s"
        "  cell $mem \\u\n    parameter \\MEMID \"\\\\mem\"\n    parameter \\SIZE 16\n"
        "    parameter \\OFFSET 0\n    parameter \\ABITS 4\n    parameter \\WIDTH 3\n"
        "    parameter \\RD_PORTS 1\n    parameter \\WR_PORTS %u\n"
        "    parameter \\RD_CLK_ENABLE 1'0\n    parameter \\RD_CLK_POLARITY 1'1\n"
        "    parameter \\RD_TRANSPARENT 1'0\n    parameter \\WR_CLK_ENABLE %u'%s\n"
        "    parameter \\WR_CLK_POLARITY %u'%s\n    parameter \\INIT 0'\n"
        "    connect \\RD_CLK 1'x\n    connect \\RD_EN 1'1\n    connect \\RD_ADDR %s\n"
        "    connect \\RD_DATA %s\n    connect \\WR_CLK %s\n    connect \\WR_EN %s\n"
        "    connect \\WR_ADDR %s\n    connect \\WR_DATA %s\n  end\n%send\n",
        q_attr, var->wires, n_wr, n_wr, n_wr != 0 ? "1" : "", n_wr, n_wr != 0 ? "1" : "",
        var->rd_addr, var->rd_data, n_wr != 0 ? wr_clk : "{ }",
        n_wr != 0 ? "{ \\we \\we \\we }" : "{ }", n_wr != 0 ? "\\wa" : "{ }",
        n_wr != 0 ? "\\wd" : "{ }", var->ff);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < size);
}

static void build(const variant *var, pb_result *res) {
    char text[TEXT_SIZE];
    mem_text(text, sizeof text, var, "");
    hm_build_ok(text, res);
}

static odin3_status run_merge(pb_result *res) {
    return odin3_pass_run(res->design, "memory_merge", odin3_bytes_cstr(""));
}

static void merge_ok(pb_result *res) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_merge(res));
}

/* The live `$mem` of the module and one bit of one of its bit parameters. */
static odin3_node_id mem_node(const pb_result *res) {
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res->module, "$mem"));
    return hm_nth(res->module, "$mem", 0);
}

static bool param_bit(const pb_result *res, uint32_t param) {
    const odin3_value *prm = odin3_node_param(res->module, mem_node(res), 0);
    return prm[param].bits[0] == ODIN3_BIT_1;
}

static bool contains(const char *text) {
    return strstr(log_text, text) != NULL;
}

/* --- the merges ---------------------------------------------------------------------------- */

/* The read port is synchronous on the rising edge and not transparent. */
static void expect_sync_port(const pb_result *res) {
    TEST_ASSERT_TRUE(param_bit(res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_TRUE(param_bit(res, MP_RD_CLK_POLARITY));
    TEST_ASSERT_FALSE(param_bit(res, MP_RD_TRANSPARENT));
}

/* common/mem.v's shape: a registered read of a word array, a write on the same clock. */
static void test_data_merge(void) {
    pb_result res;
    build(&k_data, &res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dff"));
    expect_sync_port(&res);
    TEST_ASSERT_TRUE(contains("memory_merge: 1 read ports merged (1 data, 0 address), 0 left "
                              "asynchronous"));
    pb_free(&res);
}

static void test_name_and_provenance(void) {
    pb_result res;
    build(&k_data, &res);
    uint32_t name = odin3_node_name(res.module, hm_nth(res.module, "$mem", 0));
    odin3_prov_id before = odin3_node_prov(res.module, hm_nth(res.module, "$mem", 0));
    merge_ok(&res);
    odin3_node_id mem = mem_node(&res);
    TEST_ASSERT_EQUAL_UINT32(name, odin3_node_name(res.module, mem));
    TEST_ASSERT_TRUE(name != 0);
    TEST_ASSERT_TRUE(odin3_node_prov(res.module, mem).v != before.v);
    pb_free(&res);
}

static void test_dffe_const_one(void) {
    pb_result res;
    build(&k_dffe_one, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dffe"));
    TEST_ASSERT_TRUE(param_bit(&res, MP_RD_CLK_ENABLE));
    pb_free(&res);
}

/* A variable enable becomes RD_EN of the port. */
static void test_dffe_variable_enable(void) {
    pb_result res;
    build(&k_dffe_var, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dffe"));
    odin3_node_id mem = mem_node(&res);
    odin3_pinslice en = odin3_node_port(res.module, mem, MEM_P_RD_EN);
    TEST_ASSERT_EQUAL_UINT32(1, en.count);
    odin3_net_id net = odin3_pin_net(res.module, en.first);
    TEST_ASSERT_FALSE(odin3_net_const_value(res.module, net) == ODIN3_CONST_1);
    pb_free(&res);
}

/* An active-low enable would need an inverter: the pass leaves the cell and its flip-flop. */
static void test_active_low_enable_left(void) {
    pb_result res;
    build(&k_dffe_low, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dffe"));
    TEST_ASSERT_FALSE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_TRUE(contains("0 read ports merged (0 data, 0 address), 1 left asynchronous"));
    pb_free(&res);
}

static void test_falling_edge_merges(void) {
    pb_result res;
    build(&k_falling, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dff"));
    TEST_ASSERT_TRUE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_FALSE(param_bit(&res, MP_RD_CLK_POLARITY));
    pb_free(&res);
}

static void test_addr_merge(void) {
    pb_result res;
    build(&k_addr_clk2, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dff"));
    TEST_ASSERT_TRUE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_TRUE(contains("1 read ports merged (0 data, 1 address), 0 left asynchronous"));
    pb_free(&res);
}

/* D12: a write port on the read clock forbids the address merge. */
static void test_addr_merge_with_write_stays(void) {
    pb_result res;
    build(&k_addr_wr, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    TEST_ASSERT_FALSE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_TRUE(contains("0 read ports merged (0 data, 0 address), 1 left asynchronous"));
    pb_free(&res);
}

static void test_async_without_register(void) {
    pb_result res;
    build(&k_async, &res);
    merge_ok(&res);
    TEST_ASSERT_FALSE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_TRUE(contains("0 read ports merged (0 data, 0 address), 1 left asynchronous"));
    pb_free(&res);
}

/* A flip-flop with an initial value is not merged (the RAM's read register has none). */
static void test_init_value_left(void) {
    pb_result res;
    char text[TEXT_SIZE];
    mem_text(text, sizeof text, &k_data, "  attribute \\init 3'101\n");
    hm_build_ok(text, &res);
    merge_ok(&res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    TEST_ASSERT_FALSE(param_bit(&res, MP_RD_CLK_ENABLE));
    pb_free(&res);
}

static void test_twice_and_no_arguments(void) {
    pb_result res;
    build(&k_data, &res);
    merge_ok(&res);
    log_text[0] = '\0';
    merge_ok(&res);
    TEST_ASSERT_TRUE(contains("0 read ports merged (0 data, 0 address), 0 left asynchronous"));
    TEST_ASSERT_TRUE(param_bit(&res, MP_RD_CLK_ENABLE));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(res.design, "memory_merge", odin3_bytes_cstr("-x")));
    TEST_ASSERT_TRUE(contains("memory_merge: takes no arguments"));
    pb_free(&res);
}

/* A design with no `$mem` at all is an empty run. */
static void test_no_mem(void) {
    pb_result res;
    hm_build_ok(
        "module \\m\n  wire width 1 input 1 \\a\n  wire width 1 output 2 \\y\n"
        "  cell $not \\n\n    parameter \\A_WIDTH 1\n    parameter \\Y_WIDTH 1\n"
        "    parameter \\A_SIGNED 0\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n",
        &res);
    merge_ok(&res);
    TEST_ASSERT_TRUE(contains("0 read ports merged (0 data, 0 address), 0 left asynchronous"));
    pb_free(&res);
}

/* --- simulation: the merge keeps the read behaviour ---------------------------------------- */

static odin3_net_id wnet(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(res->module, wire, bit);
}

typedef struct sim_nets {
    odin3_net_id in[ABITS * 2 + WIDTH + 2];
    odin3_net_id out[WIDTH];
    uint32_t n_in;
} sim_nets;

static void add_wire(const pb_result *res, const char *name, uint32_t width, sim_nets *sn) {
    for (uint32_t bit = 0; bit < width; bit++) {
        sn->in[sn->n_in++] = wnet(res, name, bit);
    }
}

static void trace(pb_result *res, bool writes, bool preload, odin3_strbuf *out) {
    sim_nets sn;
    sn.n_in = 0;
    add_wire(res, "ra", ABITS, &sn);
    if (writes) {
        add_wire(res, "wa", ABITS, &sn);
        add_wire(res, "wd", WIDTH, &sn);
        add_wire(res, "we", 1, &sn);
    }
    add_wire(res, "ren", 1, &sn);
    for (uint32_t bit = 0; bit < WIDTH; bit++) {
        sn.out[bit] = wnet(res, "q", bit);
    }
    rs_trace(res->module, &(rs_io){sn.in, sn.n_in, sn.out, WIDTH},
             (rs_run){SIM_CYCLES, 4242, preload}, out);
}

/* Builds the variant twice, merges the second, and compares the traces. */
static bool sim_equal(const variant *var, bool preload) {
    pb_result before;
    pb_result after;
    odin3_strbuf trace_before;
    odin3_strbuf trace_after;
    odin3_strbuf_init(&trace_before);
    odin3_strbuf_init(&trace_after);
    build(var, &before);
    build(var, &after);
    merge_ok(&after);
    TEST_ASSERT_TRUE(param_bit(&after, MP_RD_CLK_ENABLE));
    trace(&before, var->wr != WR_NONE, preload, &trace_before);
    trace(&after, var->wr != WR_NONE, preload, &trace_after);
    uint32_t ones = 0;
    for (size_t i = 0; i < trace_before.len; i++) {
        ones += trace_before.data[i] == '1' ? 1U : 0U;
    }
    TEST_ASSERT_TRUE_MESSAGE(ones > 50, "the trace must read back data");
    bool same = trace_before.len == trace_after.len &&
                memcmp(trace_before.data, trace_after.data, trace_before.len) == 0;
    odin3_strbuf_free(&trace_before);
    odin3_strbuf_free(&trace_after);
    pb_free(&before);
    pb_free(&after);
    return same;
}

static void test_sim_data_merge(void) {
    TEST_ASSERT_TRUE(sim_equal(&k_data, false));
    TEST_ASSERT_TRUE(sim_equal(&k_dffe_one, false));
}

static void test_sim_dffe_variable(void) {
    TEST_ASSERT_TRUE(sim_equal(&k_dffe_var, false));
}

static void test_sim_addr_merge_rom(void) {
    TEST_ASSERT_TRUE(sim_equal(&k_addr_rom, true));
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Every allocation failure leaves a valid IR, and the pass succeeds once memory is back. */
static void test_oom_sweep(void) {
    uint32_t failures = 0;
    for (int32_t limit = 0; limit < OOM_LIMIT; limit++) {
        pb_result res;
        build(&k_data, &res);
        odin3_util_set_alloc_fail_after(limit);
        odin3_status st = odin3_pass_run(res.design, "memory_merge", odin3_bytes_cstr(""));
        odin3_util_set_alloc_fail_after(-1);
        odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
            pb_free(&res);
            break;
        }
        failures++;
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
        pb_free(&res);
    }
    TEST_ASSERT_TRUE(failures > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_data_merge);
    RUN_TEST(test_name_and_provenance);
    RUN_TEST(test_dffe_const_one);
    RUN_TEST(test_dffe_variable_enable);
    RUN_TEST(test_active_low_enable_left);
    RUN_TEST(test_falling_edge_merges);
    RUN_TEST(test_addr_merge);
    RUN_TEST(test_addr_merge_with_write_stays);
    RUN_TEST(test_async_without_register);
    RUN_TEST(test_init_value_left);
    RUN_TEST(test_twice_and_no_arguments);
    RUN_TEST(test_no_mem);
    RUN_TEST(test_sim_data_merge);
    RUN_TEST(test_sim_dffe_variable);
    RUN_TEST(test_sim_addr_merge_rom);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
