/*
 * test_proc_emit.c — proc's shared helpers (2E spec §7, §14, §15, §16; plan Task 3, review I1/I2):
 * the cell helper (inputs through the net map, constants on the shared constant nets, fresh and
 * given outputs, attributes, kinds, the budget every budget_pins pins, refusals, out of memory),
 * odin3_proc_type, odin3_proc_prov and odin3_proc_begin_lowering, odin3_proc_warn and
 * odin3_proc_name_bits.
 *
 * Yosys re-derivation: none (no proc phase runs here; the cells are built by hand).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/budget.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_LINES = 16, LOG_TEXT = 512, OOM_LIMIT = 400, NAME_TEXT = 64 };

/* The module m: a process (`keep` on its node, located at t.v:3.1) assigning q from a. */
static const char *const k_text = "module \\m\n"
                                  "  wire input 1 \\clk\n"
                                  "  wire input 2 \\a\n"
                                  "  wire width 2 input 3 \\s\n"
                                  "  wire output 4 \\q\n"
                                  "  wire width 8 \\w8\n"
                                  "  wire \\c\n"
                                  "  attribute \\keep 1\n"
                                  "  attribute \\src \"t.v:3.1\"\n"
                                  "  process $p\n"
                                  "    assign \\q \\a\n"
                                  "    sync always\n"
                                  "  end\n"
                                  "end\n";

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        levels[n_lines] = level;
        (void)snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
    }
    n_lines++;
}

/* A built design, a pass run named proc, its module context with stats and the decoded process. */
typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_proc_mctx mctx;
    odin3_proc_stats stats;
    odin3_pw *pw;
    odin3_module *module;
} fixture;

static fixture fx;

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void open_fixture(void) {
    memset(&fx, 0, sizeof fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_text, &fx.res));
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("proc"), &fx.ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx.mctx, &fx.ctx, fx.module));
    fx.mctx.stats = &fx.stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vec_reserve(&fx.mctx.pws, 1));
    fx.pw = odin3_vec_push(&fx.mctx.pws);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx.mctx, pb_proc(&fx.res, "m", 0), fx.pw));
}

static void close_fixture(void) {
    odin3_proc_mctx_free(&fx.mctx);
    pb_free(&fx.res);
}

void setUp(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
    open_fixture();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_limits(NULL);
    odin3_budget_test_set_limit(0);
    close_fixture();
    odin3_log_set_sink(NULL, NULL);
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

static odin3_wire_id wire_named(const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return wire;
}

static odin3_pw_bit wire_bit(const char *name, uint32_t bit) {
    return odin3_wire_net(fx.module, wire_named(name), bit).v;
}

/* A run of pw->bits holding bits[0 .. n). */
static odin3_pw_sig sig_of(const odin3_pw_bit *bits, uint32_t n) {
    odin3_pw_sig sig = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_new(fx.pw, bits, n, &sig));
    return sig;
}

static odin3_pw_bit sig_bit(odin3_pw_sig sig, uint32_t pos) {
    TEST_ASSERT_TRUE(pos < sig.len);
    return odin3_pw_sig_bits(fx.pw, sig)[pos];
}

/* The node driving net (its first driver's node). */
static odin3_node_id driver_of(odin3_pw_bit net) {
    odin3_pin_id pin = odin3_net_driver(fx.module, (odin3_net_id){net});
    TEST_ASSERT_TRUE(odin3_pin_valid(pin));
    return odin3_pin_node(fx.module, pin);
}

/* The net on bit 0 of port `port` of node. */
static odin3_net_id port_net(odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(fx.module, node, port);
    TEST_ASSERT_TRUE(pins.count > 0);
    return odin3_pin_net(fx.module, pins.first);
}

static const odin3_value *node_attr(odin3_node_id node, const char *key) {
    return odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern(key));
}

/* node carries attribute key, an INT of value. */
static void expect_int_attr(odin3_node_id node, const char *key, int64_t value) {
    const odin3_value *val = node_attr(node, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(val, key);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, val->kind);
    TEST_ASSERT_EQUAL_INT64(value, val->i);
}

/* --- the cell helper ------------------------------------------------------------------------- */

static const odin3_value k_unary[3] = {{ODIN3_VAL_INT, 0, NULL, 0, 0, 0},
                                       {ODIN3_VAL_INT, 1, NULL, 0, 0, 0},
                                       {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}};

/* A $not of in on fresh nets with prov: the request. */
static odin3_proc_cell not_cell(const odin3_pw_sig *in, odin3_prov_id prov) {
    odin3_proc_cell cell = {
        odin3_proc_type(&fx.mctx, "$not"), k_unary, 3, fx.pw, in, 1, NULL, NULL, 0, prov};
    return cell;
}

/* A constant input sits on the shared constant net; the fresh output, appended to pw->bits, takes
 * the cell's record. */
static void test_add_cell_const_input_fresh_output(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    odin3_pw_bit one = ODIN3_PW_S1;
    odin3_pw_sig in = sig_of(&one, 1);
    odin3_proc_cell cell = not_cell(&in, fx.pw->lowered);
    uint32_t bits_before = (uint32_t)fx.pw->bits.len;
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    TEST_ASSERT_EQUAL_UINT32(bits_before, out.first);
    TEST_ASSERT_EQUAL_UINT32(1, out.len);
    odin3_pw_bit y_net = sig_bit(out, 0);
    TEST_ASSERT_TRUE(odin3_net_live(fx.module, (odin3_net_id){y_net}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_name(fx.module, (odin3_net_id){y_net}));
    TEST_ASSERT_EQUAL_UINT32(fx.pw->lowered.v, odin3_net_prov(fx.module, (odin3_net_id){y_net}).v);
    odin3_node_id node = driver_of(y_net);
    TEST_ASSERT_EQUAL_UINT32(odin3_proc_type(&fx.mctx, "$not").v,
                             odin3_node_type(fx.module, node).v);
    TEST_ASSERT_EQUAL_UINT32(fx.pw->lowered.v, odin3_node_prov(fx.module, node).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(fx.module, port_net(node, 0)));
    TEST_ASSERT_NULL(node_attr(node, "keep")); /* attrs_from NULL */
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_MUX]);
    check_full();
}

/* A $mux onto the process's O net q (step B deleted the node): inputs resolved through the net
 * map, the working form's attributes and the switch flags copied, counted as a $mux. */
static void test_add_cell_given_output_attrs(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(fx.module, fx.pw->node));
    odin3_net_id tmp = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &tmp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_connect(&fx.mctx, tmp.v, wire_bit("a", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx.mctx)); /* tmp -> a */
    const odin3_pw_bit in_bits[3] = {tmp.v, ODIN3_PW_S0, wire_bit("s", 0)};
    odin3_pw_sig ins[3] = {sig_of(&in_bits[0], 1), sig_of(&in_bits[1], 1), sig_of(&in_bits[2], 1)};
    odin3_pw_bit q_net = wire_bit("q", 0);
    odin3_pw_sig q_sig = sig_of(&q_net, 1);
    odin3_value width = odin3_value_int(1);
    odin3_proc_cell cell = {odin3_proc_type(&fx.mctx, "$mux"),
                            &width,
                            1,
                            fx.pw,
                            ins,
                            3,
                            &q_sig,
                            fx.pw,
                            ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE,
                            fx.pw->lowered};
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    TEST_ASSERT_EQUAL_HEX32(q_net, sig_bit(out, 0));
    odin3_node_id mux = driver_of(q_net);
    TEST_ASSERT_EQUAL_UINT32(wire_bit("a", 0), port_net(mux, 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(fx.module, port_net(mux, 1)));
    TEST_ASSERT_EQUAL_UINT32(wire_bit("s", 0), port_net(mux, 2).v);
    TEST_ASSERT_EQUAL_UINT32(q_net, port_net(mux, 3).v);
    expect_int_attr(mux, "keep", 1);
    expect_int_attr(mux, "full_case", 1);
    expect_int_attr(mux, "parallel_case", 1);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_MUX]);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    check_full();
}

/* Each type proc makes counts as its kind (§17, point (j)); a $dff gets no switch attribute. */
static void test_add_cell_kinds(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    const odin3_pw_bit bits[2] = {wire_bit("clk", 0), wire_bit("a", 0)};
    odin3_pw_sig ins[2] = {sig_of(&bits[0], 1), sig_of(&bits[1], 1)};
    const odin3_value dff_params[2] = {odin3_value_int(1), odin3_value_int(1)};
    odin3_proc_cell dff = {odin3_proc_type(&fx.mctx, "$dff"),
                           dff_params,
                           2,
                           fx.pw,
                           ins,
                           2,
                           NULL,
                           fx.pw,
                           0,
                           fx.pw->lowered};
    odin3_pw_sig dff_out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_add_cell(&fx.mctx, &dff, &dff_out));
    odin3_pw_sig out = {0, 0};
    const odin3_value eq_params[5] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(1),
                                      odin3_value_int(1), odin3_value_int(1)};
    odin3_proc_cell eq = {odin3_proc_type(&fx.mctx, "$eq"),
                          eq_params,
                          5,
                          fx.pw,
                          ins,
                          2,
                          NULL,
                          NULL,
                          0,
                          fx.pw->lowered};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_add_cell(&fx.mctx, &eq, &out));
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_EQ]);
    uint32_t total = 0;
    for (uint32_t k = 0; k < ODIN3_PROC_N_KINDS; k++) {
        total += fx.stats.cells[k];
    }
    TEST_ASSERT_EQUAL_UINT32(2, total);
    odin3_node_id dff_node = driver_of(sig_bit(dff_out, 0));
    TEST_ASSERT_EQUAL_UINT32(odin3_proc_type(&fx.mctx, "$dff").v,
                             odin3_node_type(fx.module, dff_node).v);
    expect_int_attr(dff_node, "keep", 1);
    TEST_ASSERT_NULL(node_attr(dff_node, "full_case"));
    check_full();
}

/* A width mismatch, a missing input, a constant output and wrong parameters make nothing. */
static void test_add_cell_rejects(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    const odin3_pw_bit two[2] = {wire_bit("s", 0), wire_bit("s", 1)};
    odin3_pw_sig wide = sig_of(two, 2);
    odin3_proc_cell cell = not_cell(&wide, fx.pw->lowered);
    uint32_t nets = odin3_module_net_end(fx.module);
    uint32_t nodes = odin3_module_node_end(fx.module);
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    TEST_ASSERT_EQUAL_STRING("proc: add_cell: $not: input 0 does not have 1 bits", lines[0]);
    cell.n_inputs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    odin3_pw_sig narrow = sig_of(two, 1);
    odin3_pw_bit zero = ODIN3_PW_S0;
    odin3_pw_sig const_out = sig_of(&zero, 1);
    cell = not_cell(&narrow, fx.pw->lowered);
    cell.output = &const_out;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    cell = not_cell(&narrow, fx.pw->lowered);
    cell.n_params = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_add_cell(&fx.mctx, &cell, &out));
    TEST_ASSERT_EQUAL_UINT32(nets, odin3_module_net_end(fx.module));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(fx.module));
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    check_full();
}

/* A budget check every budget_pins pins, before the cell is made: E6 at the process (§16). */
static void test_add_cell_budget(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.budget_pins = 3;
    odin3_proc_test_set_limits(&limits);
    odin3_budget_test_set_limit(1);
    odin3_pw_bit a_net = wire_bit("a", 0);
    odin3_pw_sig in = sig_of(&a_net, 1);
    odin3_proc_cell cell = not_cell(&in, fx.pw->lowered);
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_add_cell(&fx.mctx, &cell, &out)); /* 2 pins */
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    uint32_t nodes = odin3_module_node_end(fx.module);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_proc_add_cell(&fx.mctx, &cell, &out)); /* 4 */
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(fx.module));
    TEST_ASSERT_EQUAL_size_t(1, n_lines);
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_ERROR, levels[0]);
    TEST_ASSERT_EQUAL_STRING("t.v:3:1: error: proc needs more than 1 bytes (the design's budget "
                             "is 60% of physical memory)",
                             lines[0]);
    TEST_ASSERT_TRUE(fx.mctx.stopped);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.errors);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    check_full();
}

/* The live nodes of type. */
static uint32_t live_of_type(odin3_celltype_id type) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(fx.module); i++) {
        odin3_node_id node = {i};
        count += odin3_node_live(fx.module, node) && odin3_node_type(fx.module, node).v == type.v;
    }
    return count;
}

/* True when some net from ID first on is live, pinless, unnamed and wireless (a stranded net). */
static bool stranded_from(uint32_t first) {
    for (uint32_t i = first; i < odin3_module_net_end(fx.module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(fx.module, net) && odin3_net_pins(fx.module, net).count == 0 &&
            odin3_net_name(fx.module, net) == 0 &&
            !odin3_wire_valid(odin3_net_primary(fx.module, net).wire)) {
            return true;
        }
    }
    return false;
}

/* One injection over a $mux with a constant input and a fresh output: NO_MEMORY leaves no
 * stranded fresh net and no $mux; OK makes the $mux. */
static bool oom_step(long fail) {
    close_fixture();
    open_fixture();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    const odin3_pw_bit in_bits[3] = {wire_bit("a", 0), ODIN3_PW_S1, wire_bit("s", 1)};
    odin3_pw_sig ins[3] = {sig_of(&in_bits[0], 1), sig_of(&in_bits[1], 1), sig_of(&in_bits[2], 1)};
    odin3_value width = odin3_value_int(1);
    odin3_proc_cell cell = {odin3_proc_type(&fx.mctx, "$mux"),
                            &width,
                            1,
                            fx.pw,
                            ins,
                            3,
                            NULL,
                            fx.pw,
                            ODIN3_SWITCH_FULL_CASE,
                            fx.pw->lowered};
    uint32_t nets = odin3_module_net_end(fx.module);
    odin3_pw_sig out = {0, 0};
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_proc_add_cell(&fx.mctx, &cell, &out);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_FALSE(stranded_from(nets));
    /* a $mux exists exactly when it was counted (only an attribute can fail after it is made) */
    uint32_t muxes = live_of_type(odin3_proc_type(&fx.mctx, "$mux"));
    TEST_ASSERT_EQUAL_UINT32(muxes, fx.stats.cells[ODIN3_PROC_K_MUX]);
    TEST_ASSERT_TRUE(st != ODIN3_OK || muxes == 1);
    check_full();
    return st == ODIN3_OK;
}

static void test_add_cell_oom(void) {
    bool done = false;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
}

/* --- types and provenance ------------------------------------------------------------------- */

static void test_proc_type(void) {
    static const char *const names[] = {"$dff",  "$adff", "$aldff", "$dffsr", "$dlatch",   "$mux",
                                        "$pmux", "$eq",   "$not",   "$and",   "$reduce_or"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        odin3_celltype_id want = {0};
        TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern(names[i]), &want));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.v, odin3_proc_type(&fx.mctx, names[i]).v, names[i]);
    }
    odin3_celltype_id xor_type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$xor"), &xor_type));
    TEST_ASSERT_EQUAL_UINT32(xor_type.v, odin3_proc_type(&fx.mctx, "$xor").v); /* not cached */
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    TEST_ASSERT_FALSE(odin3_celltype_valid(odin3_proc_type(&fx.mctx, "$nope")));
    TEST_ASSERT_EQUAL_STRING("proc: cell type '$nope' is not registered", lines[0]);
}

/* The parents of record rec, which must be DERIVED in the fixture's run and current operation. */
static void expect_parents(odin3_prov_id rec, const odin3_prov_id *want, uint32_t n) {
    const odin3_prov_record *got = odin3_prov_get(fx.res.design, rec);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, got->kind);
    TEST_ASSERT_EQUAL_UINT32(fx.ctx.run.v, got->run.v);
    TEST_ASSERT_EQUAL_UINT32(fx.ctx.op, got->op);
    TEST_ASSERT_EQUAL_UINT32(n, got->parents.count);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i].v, got->parents.ids[i].v);
    }
}

/* begin_lowering: a new operation and lowered = derive([pw->prov]); proc_prov: a 0 parent, or
 * none, stands for pw->prov. */
static void test_prov(void) {
    TEST_ASSERT_TRUE(fx.pw->prov.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw));
    TEST_ASSERT_EQUAL_UINT32(1, fx.ctx.op);
    expect_parents(fx.pw->lowered, &fx.pw->prov, 1);
    odin3_prov_id other = odin3_module_prov(fx.module);
    TEST_ASSERT_TRUE(other.v != 0 && other.v != fx.pw->prov.v);
    odin3_prov_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_prov(&fx.mctx, fx.pw, NULL, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(fx.pw->lowered.v, out.v); /* hash-consed: the same record */
    const odin3_prov_id given[2] = {{0}, other};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_prov(&fx.mctx, fx.pw, given, 2, &out));
    const odin3_prov_id want[2] = {fx.pw->prov, other};
    expect_parents(out, want, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, fx.pw)); /* next op */
    TEST_ASSERT_EQUAL_UINT32(2, fx.ctx.op);
    TEST_ASSERT_TRUE(fx.pw->lowered.v != out.v);
}

/* Review I2: a working form without a record derives from its other parents, or gives 0. */
static void test_prov_without_record(void) {
    odin3_pw bare = *fx.pw; /* shares the vectors; only prov and lowered are touched */
    bare.prov = (odin3_prov_id){0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx.mctx, &bare));
    TEST_ASSERT_EQUAL_UINT32(0, bare.lowered.v);
    odin3_prov_id out = {1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_prov(&fx.mctx, &bare, NULL, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(0, out.v);
    const odin3_prov_id zeros[2] = {{0}, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_prov(&fx.mctx, &bare, zeros, 2, &out));
    TEST_ASSERT_EQUAL_UINT32(0, out.v);
    odin3_prov_id other = odin3_module_prov(fx.module);
    const odin3_prov_id given[2] = {{0}, other};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_prov(&fx.mctx, &bare, given, 2, &out));
    expect_parents(out, &other, 1);
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
}

/* --- diagnostics ------------------------------------------------------------------------------ */

/* odin3_proc_warn counts warnings[kind] (not errors) and is located as an error is. */
static void test_warn(void) {
    odin3_proc_warn(&fx.mctx, ODIN3_PROC_W3, fx.pw->prov, "'%s' takes a non-constant value", "q");
    odin3_proc_warn(&fx.mctx, ODIN3_PROC_W5B, (odin3_prov_id){0}, "case analysis");
    TEST_ASSERT_EQUAL_size_t(2, n_lines);
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_WARN, levels[0]);
    TEST_ASSERT_EQUAL_STRING("t.v:3:1: warning: 'q' takes a non-constant value", lines[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_WARN, levels[1]);
    TEST_ASSERT_EQUAL_STRING("proc: module 'm': case analysis", lines[1]);
    for (uint32_t k = 0; k < ODIN3_PROC_N_WARNINGS; k++) {
        uint32_t want = k == ODIN3_PROC_W3 || k == ODIN3_PROC_W5B ? 1 : 0;
        TEST_ASSERT_EQUAL_UINT32(want, fx.stats.warnings[k]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.errors);
    TEST_ASSERT_EQUAL_UINT32(0, fx.mctx.errors);
    fx.mctx.stats = NULL; /* outside a run: logged, not counted */
    odin3_proc_warn(&fx.mctx, ODIN3_PROC_W1, (odin3_prov_id){0}, "latch");
    TEST_ASSERT_EQUAL_size_t(3, n_lines);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.warnings[ODIN3_PROC_W1]);
}

/* §15's name of n bits, through odin3_proc_name_bits. */
static void expect_name(const odin3_pw_bit *bits, uint32_t n, const char *want) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_name_bits(&fx.mctx, bits, n, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data != NULL ? out.data : "");
    odin3_strbuf_free(&out);
}

/* A whole wire, a part in declared indices (also of an ascending wire), a single bit, the first
 * bit of a scattered group, and a one-bit wire. */
static void test_name_bits_wires(void) {
    odin3_pw_bit w8[8];
    for (uint32_t k = 0; k < 8; k++) {
        w8[k] = wire_bit("w8", k);
    }
    expect_name(w8, 8, "w8");
    expect_name(w8 + 4, 4, "w8[7:4]");
    expect_name(w8 + 3, 1, "w8[3]");
    const odin3_pw_bit scattered[2] = {w8[3], w8[5]};
    expect_name(scattered, 2, "w8[3]");
    const odin3_pw_bit reversed[2] = {w8[5], w8[4]};
    expect_name(reversed, 2, "w8[5]");
    odin3_pw_bit c_net = wire_bit("c", 0);
    expect_name(&c_net, 1, "c");
    odin3_wire_spec spec = {intern("u"), 0, 3, false, (odin3_prov_id){0}}; /* [0:3] */
    odin3_wire_id upto = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(fx.module, &spec, NULL, &upto));
    const odin3_pw_bit low_two[2] = {odin3_wire_net(fx.module, upto, 0).v,
                                     odin3_wire_net(fx.module, upto, 1).v};
    expect_name(low_two, 2, "u[2:3]");
    expect_name(low_two, 0, "");
}

/* Nets that are no named wire's primary bit, constants, and a net merged away (net map). */
static void test_name_bits_nets(void) {
    odin3_net_id named = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(fx.module, intern("nn"), (odin3_prov_id){0}, &named));
    expect_name(&named.v, 1, "nn");
    odin3_net_id bare = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &bare));
    char want[NAME_TEXT];
    int len = snprintf(want, sizeof want, "$n%u", bare.v);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    expect_name(&bare.v, 1, want);
    /* an alias-only net: its primary wire deleted, the alias t2[1] remains */
    odin3_wire_spec one = {intern("t1"), 0, 0, false, (odin3_prov_id){0}};
    odin3_wire_spec two = {intern("t2"), 1, 0, false, (odin3_prov_id){0}};
    odin3_wire_id t1 = {0};
    odin3_wire_id t2 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(fx.module, &one, &bare, &t1));
    const odin3_net_id pair[2] = {named, bare};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(fx.module, &two, pair, &t2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(fx.module, t1));
    expect_name(&bare.v, 1, "t2[1]");
    const odin3_pw_bit consts[4] = {ODIN3_PW_S1, ODIN3_PW_SX, ODIN3_PW_S0, ODIN3_PW_S1};
    expect_name(consts, 1, "1'1");
    expect_name(consts + 1, 1, "1'x");
    expect_name(consts + 2, 2, "1'0");
    odin3_net_id gone = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &gone));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_connect(&fx.mctx, gone.v, wire_bit("c", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx.mctx));
    expect_name(&gone.v, 1, "c");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_cell_const_input_fresh_output);
    RUN_TEST(test_add_cell_given_output_attrs);
    RUN_TEST(test_add_cell_kinds);
    RUN_TEST(test_add_cell_rejects);
    RUN_TEST(test_add_cell_budget);
    RUN_TEST(test_add_cell_oom);
    RUN_TEST(test_proc_type);
    RUN_TEST(test_prov);
    RUN_TEST(test_prov_without_record);
    RUN_TEST(test_warn);
    RUN_TEST(test_name_bits_wires);
    RUN_TEST(test_name_bits_nets);
    return UNITY_END();
}
