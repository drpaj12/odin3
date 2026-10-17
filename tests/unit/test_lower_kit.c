/*
 * test_lower_kit.c — 3B Task 1: the builders' toolkit (spec §3, §4, §12.2): extension, the
 * both-signed rule and its warning, the pairing tree against the closed form, lw_connect, the
 * constants, the caps and the budget, and the transaction (rollback after a failure at each
 * allocation, out of memory at every allocation of every helper).
 */
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, MAX_INPUTS = 48, TREE_MAX = 40, OOM_LIMIT = 400, N_INPUT_NETS = 8 };

static lf fix;
static lw_ctx ctx;
static odin3_module *mod;
static odin3_node_id host; /* a $and cell: the "current cell" of the transaction */
static odin3_net_id inputs[MAX_INPUTS];
static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

static odin3_net_id new_net(const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, name != NULL ? lf_str(&fix, name) : 0,
                                                     (odin3_prov_id){0}, &net));
    return net;
}

static void open_cell(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_begin(&ctx, host));
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    lf_begin(&fix);
    lf_binary spec = {false, false, 1, 1, 1};
    odin3_value params[5];
    lf_binary_params(&spec, params);
    lf_cell_spec cs = {"$and", params, 5, "host", {0}};
    host = lf_one_cell(&fix, &cs, &mod);
    for (uint32_t i = 0; i < MAX_INPUTS; i++) {
        inputs[i] = new_net(NULL);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_ctx_init(&ctx, &fix.ctx, fix.design, NULL));
    lw_ctx_enter(&ctx, mod);
    open_cell();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_budget_test_set_limit(0);
    odin3_lower_test_set_limits(NULL);
    lw_ctx_free(&ctx);
    lf_end(&fix);
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers over the IR -------------------------------------------------------------------- */

static const char *type_of(odin3_node_id node) {
    return odin3_celltype_get(fix.design, odin3_node_type(mod, node))->name;
}

/* The gate driving net, none when the net has no driver. */
static odin3_node_id driver_of(odin3_net_id net) {
    odin3_pin_id pin = odin3_net_driver(mod, net);
    return odin3_pin_valid(pin) ? odin3_pin_node(mod, pin) : (odin3_node_id){0};
}

/* The two input nets (ports 0 and 1) of a gate. */
typedef struct gate_in {
    odin3_net_id first;
    odin3_net_id second;
} gate_in;

/* Input nets (ports 0 and 1) of the gate driving net. */
static gate_in gate_inputs(odin3_net_id net) {
    odin3_node_id node = driver_of(net);
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    odin3_pinslice pins = odin3_node_pins(mod, node);
    gate_in found = {odin3_pin_net(mod, pins.first),
                     odin3_pin_net(mod, (odin3_pin_id){pins.first.v + 1})};
    return found;
}

static bool is_gate_driven(odin3_net_id net) {
    odin3_node_id node = driver_of(net);
    return odin3_node_valid(node) && strncmp(type_of(node), "$_", 2) == 0 &&
           strncmp(type_of(node), "$_CONST", 7) != 0;
}

/* The depth of the gate tree below net: levels of gates until only non-gate nets are left. */
static uint32_t depth_of(odin3_net_id root) {
    odin3_net_id frontier[MAX_INPUTS] = {root};
    uint32_t count = 1;
    uint32_t depth = 0;
    for (;;) {
        odin3_net_id next[MAX_INPUTS];
        uint32_t next_count = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (is_gate_driven(frontier[i])) {
                gate_in in = gate_inputs(frontier[i]);
                next[next_count++] = in.first;
                next[next_count++] = in.second;
            }
        }
        if (next_count == 0) {
            return depth;
        }
        depth++;
        memcpy(frontier, next, next_count * sizeof next[0]);
        count = next_count;
    }
}

static uint32_t ceil_log2(uint32_t n) {
    uint32_t bits = 0;
    while (((uint32_t)1 << bits) < n) {
        bits++;
    }
    return bits;
}

static uint32_t live_nets(void) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_net_end(mod); id++) {
        count += odin3_net_live(mod, (odin3_net_id){id}) ? 1U : 0U;
    }
    return count;
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(mod, opts));
}

/* --- extension ------------------------------------------------------------------------------ */

static void extend_case(uint32_t width, bool is_signed, uint32_t n_in, lw_bits *out) {
    lw_span in = {inputs, n_in};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_extend(&ctx, in, width, is_signed, out));
    TEST_ASSERT_EQUAL_UINT32(width, lw_bits_len(out));
}

static void test_extend_unsigned_pads_with_the_constant_zero(void) {
    lw_bits out;
    lw_bits_init(&out);
    uint32_t before = lf_live_nodes(mod);
    extend_case(5, false, 3, &out);
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(inputs[i].v, lw_bits_at(&out, i).v);
    }
    odin3_net_id zero = lw_bits_at(&out, 3);
    TEST_ASSERT_EQUAL_UINT32(zero.v, lw_bits_at(&out, 4).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(mod, zero));
    TEST_ASSERT_EQUAL_UINT32(before + 1, lf_live_nodes(mod)); /* one $_CONST0_ for both bits */
    lw_bits_free(&out);
}

static void test_extend_signed_repeats_the_top_bit(void) {
    lw_bits out;
    lw_bits_init(&out);
    uint32_t before = lf_live_nodes(mod);
    extend_case(5, true, 3, &out);
    TEST_ASSERT_EQUAL_UINT32(inputs[2].v, lw_bits_at(&out, 3).v);
    TEST_ASSERT_EQUAL_UINT32(inputs[2].v, lw_bits_at(&out, 4).v);
    TEST_ASSERT_EQUAL_UINT32(before, lf_live_nodes(mod)); /* no constant needed */
    lw_bits_free(&out);
}

static void test_extend_truncates_and_keeps_equal_widths(void) {
    lw_bits out;
    lw_bits_init(&out);
    extend_case(2, false, 3, &out);
    TEST_ASSERT_EQUAL_UINT32(inputs[1].v, lw_bits_at(&out, 1).v);
    extend_case(3, true, 3, &out);
    TEST_ASSERT_EQUAL_UINT32(inputs[2].v, lw_bits_at(&out, 2).v);
    extend_case(1, true, 1, &out);
    TEST_ASSERT_EQUAL_UINT32(inputs[0].v, lw_bits_at(&out, 0).v);
    lw_bits_free(&out);
}

/* --- the both-signed rule ------------------------------------------------------------------- */

/* A cell of `type` in a fresh module, entered and opened; returns its lw_cell. */
static lw_cell mixed_cell(const char *type, const lf_binary *spec, bool unary, const char *name) {
    odin3_module *other = lf_module(&fix, name);
    odin3_value params[5];
    if (unary) {
        lf_unary_params(spec, params);
    } else {
        lf_binary_params(spec, params);
    }
    lf_cell_spec cs = {type, params, unary ? 3U : 5U, "c", {0}};
    odin3_node_id node = lf_cell(&fix, other, &cs);
    lw_ctx_enter(&ctx, other);
    mod = other;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_begin(&ctx, node));
    lw_cell cell;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_load_cell(&ctx, node, &cell));
    return cell;
}

static void test_both_signed_needs_both_flags_and_warns_on_a_mixed_cell(void) {
    static const struct {
        bool a_signed, b_signed, want, warns;
    } k_cases[] = {{false, false, false, false},
                   {true, true, true, false},
                   {true, false, false, true},
                   {false, true, false, true}};
    for (uint32_t i = 0; i < 4; i++) {
        char name[16];
        (void)snprintf(name, sizeof name, "mix%u", i);
        log_text[0] = '\0';
        lf_binary spec = {k_cases[i].a_signed, k_cases[i].b_signed, 4, 4, 4};
        lw_cell cell = mixed_cell("$add", &spec, false, name);
        TEST_ASSERT_EQUAL_INT(k_cases[i].want, lw_both_signed(&ctx, &cell));
        TEST_ASSERT_EQUAL_INT(k_cases[i].warns,
                              strstr(log_text, "has A_SIGNED != B_SIGNED") != NULL);
    }
    TEST_ASSERT_EQUAL_UINT64(2, ctx.totals.mixed_sign);
}

static void test_both_signed_warning_text_is_pinned(void) {
    lf_binary spec = {true, false, 4, 4, 4};
    lw_cell cell = mixed_cell("$and", &spec, false, "mixed");
    (void)lw_both_signed(&ctx, &cell);
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "lower: module 'mixed': cell '$and' 'c' has A_SIGNED != B_SIGNED: treated as "
                  "unsigned (both-signed rule, Yosys simlib; PHASE2 #25)"));
}

static void test_a_unary_cell_is_signed_by_a_signed_alone(void) {
    lf_binary spec = {true, false, 4, 0, 4};
    lw_cell cell = mixed_cell("$not", &spec, true, "u1");
    TEST_ASSERT_TRUE(lw_both_signed(&ctx, &cell));
    spec.a_signed = false;
    lw_cell cell2 = mixed_cell("$neg", &spec, true, "u2");
    TEST_ASSERT_FALSE(lw_both_signed(&ctx, &cell2));
    TEST_ASSERT_EQUAL_UINT64(0, ctx.totals.mixed_sign);
}

/* --- the pairing tree ------------------------------------------------------------------------ */

static void test_tree_nodes_and_depth_match_the_closed_form_for_1_to_40_inputs(void) {
    for (uint32_t size = 1; size <= TREE_MAX; size++) {
        open_cell();
        uint32_t before = lf_live_nodes(mod);
        odin3_net_id root = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_tree(&ctx, LW_G_OR, (lw_span){inputs, size}, &root));
        TEST_ASSERT_EQUAL_UINT32(size - 1, lf_live_nodes(mod) - before);
        TEST_ASSERT_EQUAL_UINT64(size - 1, ctx.gates);
        TEST_ASSERT_EQUAL_UINT32(ceil_log2(size), depth_of(root));
        if (size == 1) {
            TEST_ASSERT_EQUAL_UINT32(inputs[0].v, root.v);
        }
        lw_rollback(&ctx);
    }
    check_full();
}

static void test_tree_of_five_carries_the_odd_bit_to_the_root(void) {
    odin3_net_id root = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_tree(&ctx, LW_G_AND, (lw_span){inputs, 5}, &root));
    gate_in top = gate_inputs(root);
    TEST_ASSERT_EQUAL_UINT32(inputs[4].v, top.second.v);
    gate_in level1 = gate_inputs(top.first);
    gate_in pair0 = gate_inputs(level1.first);
    TEST_ASSERT_EQUAL_UINT32(inputs[0].v, pair0.first.v);
    TEST_ASSERT_EQUAL_UINT32(inputs[1].v, pair0.second.v);
    gate_in pair1 = gate_inputs(level1.second);
    TEST_ASSERT_EQUAL_UINT32(inputs[2].v, pair1.first.v);
    TEST_ASSERT_EQUAL_UINT32(inputs[3].v, pair1.second.v);
    TEST_ASSERT_EQUAL_STRING("$_AND_", type_of(driver_of(root)));
}

static void test_tree_of_seven_pairs_the_carried_bit_at_the_next_level(void) {
    odin3_net_id root = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_tree(&ctx, LW_G_XOR, (lw_span){inputs, 7}, &root));
    gate_in top = gate_inputs(root);
    gate_in right = gate_inputs(top.second);
    TEST_ASSERT_EQUAL_UINT32(inputs[6].v, right.second.v);
    gate_in pair = gate_inputs(right.first);
    TEST_ASSERT_EQUAL_UINT32(inputs[4].v, pair.first.v);
    TEST_ASSERT_EQUAL_UINT32(inputs[5].v, pair.second.v);
}

/* --- lw_connect ----------------------------------------------------------------------------- */

static void test_connect_keeps_the_dropped_net_name_and_wire_bit_as_aliases(void) {
    odin3_net_id keep = new_net(NULL);
    odin3_net_id drop = new_net("dropped");
    uint32_t name = odin3_net_name(mod, drop);
    odin3_wire_id wire = {0};
    odin3_wire_spec wspec = {lf_str(&fix, "w"), 0, 0, false, {0}};
    odin3_net_id vec[1] = {drop};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &wspec, vec, &wire));
    odin3_wirebit primary = odin3_net_primary(mod, drop);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, keep, drop));
    TEST_ASSERT_FALSE(odin3_net_live(mod, drop));
    TEST_ASSERT_EQUAL_UINT32(wire.v, primary.wire.v);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_wire_net(mod, primary.wire, primary.bit).v);
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_module_find_net(mod, name).v);
    TEST_ASSERT_TRUE(odin3_net_alias_count(mod, keep) >= 1);
    check_full();
}

static void test_connect_moves_the_sinks_and_keeps_a_port_net_driven(void) {
    odin3_net_id keep = new_net(NULL);
    odin3_net_id port_net = lf_wnet(&fix, mod, "Y", 0); /* the $port_out reads it, host drives it */
    uint32_t pins = odin3_net_pins(mod, port_net).count;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, keep, port_net));
    TEST_ASSERT_EQUAL_UINT32(pins, odin3_net_pins(mod, keep).count);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, keep));
}

static void test_connect_into_the_canonical_constant_net(void) {
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &zero));
    odin3_net_id drop = new_net("sink");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, zero, drop));
    TEST_ASSERT_EQUAL_UINT32(zero.v, odin3_module_find_net(mod, lf_str(&fix, "sink")).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(mod, zero));
}

static void test_connect_with_keep_equal_to_drop_or_a_missing_net_changes_nothing(void) {
    odin3_net_id net = new_net("same");
    uint32_t before = live_nets();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, net, net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, net, (odin3_net_id){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_connect(&ctx, (odin3_net_id){0}, net));
    TEST_ASSERT_EQUAL_UINT32(before, live_nets());
    TEST_ASSERT_TRUE(odin3_net_live(mod, net));
}

/* --- constants, caps, budget ---------------------------------------------------------------- */

static void test_a_constant_is_made_once_and_reused(void) {
    odin3_net_id zero = {0};
    odin3_net_id again = {0};
    odin3_net_id one = {0};
    odin3_net_id any = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &again));
    TEST_ASSERT_EQUAL_UINT32(zero.v, again.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_1, &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_X, &any));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(mod, one));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X, odin3_net_const_value(mod, any));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_CONST0_"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, lw_const(&ctx, ODIN3_BIT_Z, &any));
}

static void test_an_existing_module_constant_is_found_on_entry(void) {
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &zero));
    lw_end(&ctx);
    lw_ctx_enter(&ctx, mod); /* the scan finds the node made above */
    open_cell();
    odin3_net_id found = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &found));
    TEST_ASSERT_EQUAL_UINT32(zero.v, found.v);
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_CONST0_"));
}

static void test_the_per_cell_cap_stops_a_runaway_builder(void) {
    ctx.cap_cell = 3;
    odin3_net_id root = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          lw_tree(&ctx, LW_G_OR, (lw_span){inputs, 10}, &root));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "would create more than 3 nodes (cap 3)"));
    lw_rollback(&ctx);
    check_full();
}

static void test_the_budget_is_checked_every_so_many_created_nodes(void) {
    ctx.next_budget = ctx.total_created + 2;
    odin3_budget_test_set_limit(1);
    odin3_net_id root = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, lw_tree(&ctx, LW_G_OR, (lw_span){inputs, 10}, &root));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower needs more than 1 bytes"));
    lw_rollback(&ctx);
    check_full();
}

static void test_live_node_count_follows_creation_and_rollback(void) {
    odin3_net_id root = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_tree(&ctx, LW_G_OR, (lw_span){inputs, 9}, &root));
    TEST_ASSERT_EQUAL_UINT64(lf_live_nodes(mod), ctx.live_nodes);
    lw_rollback(&ctx);
    TEST_ASSERT_EQUAL_UINT64(lf_live_nodes(mod), ctx.live_nodes);
}

/* --- rollback and out of memory at every allocation ----------------------------------------- */

static odin3_status op_not(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_not(lw, inputs[0], &out_net);
}
static odin3_status op_and(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_and(lw, inputs[0], inputs[1], &out_net);
}
static odin3_status op_or(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_or(lw, inputs[0], inputs[1], &out_net);
}
static odin3_status op_xor(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_xor(lw, inputs[0], inputs[1], &out_net);
}
static odin3_status op_xnor(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_xnor(lw, inputs[0], inputs[1], &out_net);
}
static odin3_status op_mux(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_mux(lw, inputs[0], inputs[1], inputs[2], &out_net);
}
static odin3_status op_net(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_net(lw, &out_net);
}
static odin3_status op_const0(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_const(lw, ODIN3_BIT_0, &out_net);
}
static odin3_status op_constx(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_const(lw, ODIN3_BIT_X, &out_net);
}
static odin3_status op_extend(lw_ctx *lw) {
    lw_bits out;
    lw_bits_init(&out);
    odin3_status st = lw_extend(lw, (lw_span){inputs, 3}, 9, false, &out);
    lw_bits_free(&out);
    return st;
}
static odin3_status op_tree(lw_ctx *lw) {
    odin3_net_id out_net = {0};
    return lw_tree(lw, LW_G_XOR, (lw_span){inputs, 7}, &out_net);
}

typedef odin3_status (*op_fn)(lw_ctx *lw);

/* Runs op with a failure injected at allocation 0, 1, 2 …: every failure is NO_MEMORY and, after
 * lw_rollback, leaves the module as it was; the first success ends the sweep. */
static void sweep_oom(op_fn op) {
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &before));
    uint32_t nets = live_nets();
    uint32_t nodes = lf_live_nodes(mod);
    uint32_t failures = 0;
    bool done = false;
    for (long step = 0; !done && step < OOM_LIMIT; step++) {
        lw_ctx_free(&ctx); /* empty vectors: the first push of each allocates */
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_ctx_init(&ctx, &fix.ctx, fix.design, NULL));
        lw_ctx_enter(&ctx, mod);
        open_cell();
        odin3_util_set_alloc_fail_after(step);
        odin3_status st = op(&ctx);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            done = true;
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            failures++;
        }
        lw_rollback(&ctx);
        odin3_strbuf after;
        odin3_strbuf_init(&after);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_rtlil(&fix, &after));
        TEST_ASSERT_EQUAL_STRING(before.data, after.data);
        odin3_strbuf_free(&after);
        TEST_ASSERT_EQUAL_UINT32(nets, live_nets());
        TEST_ASSERT_EQUAL_UINT32(nodes, lf_live_nodes(mod));
        TEST_ASSERT_EQUAL_UINT64(nodes, ctx.live_nodes);
        check_full();
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(failures > 0);
    odin3_strbuf_free(&before);
}

static void test_oom_at_every_allocation_of_each_gate_helper(void) {
    static const op_fn k_ops[] = {op_not, op_and, op_or, op_xor, op_xnor, op_mux, op_net};
    for (size_t i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++) {
        sweep_oom(k_ops[i]);
    }
}

static void test_oom_at_every_allocation_of_constants_extension_and_trees(void) {
    static const op_fn k_ops[] = {op_const0, op_constx, op_extend, op_tree};
    for (size_t i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++) {
        sweep_oom(k_ops[i]);
    }
}

static void test_rollback_after_a_constant_makes_the_next_call_create_it_again(void) {
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &zero));
    lw_rollback(&ctx);
    TEST_ASSERT_EQUAL_UINT32(0, lf_count_type(&fix, mod, "$_CONST0_"));
    open_cell();
    odin3_net_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &again));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&fix, mod, "$_CONST0_"));
    lw_end(&ctx);
    open_cell();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_const(&ctx, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_UINT32(again.v, zero.v); /* committed constants stay */
}

/* A keep attribute on the word cell is copied onto every node the builder makes (spec §9). */
static void test_keep_is_copied_onto_created_nodes(void) {
    odin3_value one = odin3_value_int(1);
    uint32_t key = lf_str(&fix, "keep");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NODE, host.v}, key, &one));
    lw_ctx_free(&ctx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_ctx_init(&ctx, &fix.ctx, fix.design, NULL));
    lw_ctx_enter(&ctx, mod);
    open_cell();
    odin3_net_id out_net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_and(&ctx, inputs[0], inputs[1], &out_net));
    const odin3_value *val =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, driver_of(out_net).v}, key);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT64(1, val->i);
}

/*
 * Plan Task 1 step 3: can odin3_net_merge fail? It reserves room in the keep net's pin array and in
 * the alias table before it moves anything (src/ir/net.c), so a failure changes nothing, and the
 * first alias of a module allocates the alias table: yes, it can fail. The spec §12.2 contract is
 * therefore the second one: a failure while committing a cell can leave it partly connected (its
 * outputs undriven, check rule 4 W), never invalid.
 */
static void test_net_merge_can_fail_and_a_failure_changes_nothing(void) {
    uint32_t failures = 0;
    for (long step = 0; step < 40; step++) {
        lf f2;
        lf_begin(&f2);
        odin3_module *m2 = NULL;
        lf_binary spec = {false, false, 1, 1, 1};
        odin3_value params[5];
        lf_binary_params(&spec, params);
        lf_cell_spec cs = {"$and", params, 5, "dut", {0}};
        (void)lf_one_cell(&f2, &cs, &m2);
        odin3_net_id keep = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(m2, 0, (odin3_prov_id){0}, &keep));
        odin3_net_id drop = lf_wnet(&f2, m2, "Y", 0); /* a port net: a name, a wire bit, a sink */
        uint32_t keep_pins = odin3_net_pins(m2, keep).count;
        uint32_t drop_pins = odin3_net_pins(m2, drop).count;
        odin3_util_set_alloc_fail_after(step);
        odin3_status st = odin3_net_merge(m2, (odin3_net_pair){keep, drop});
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            failures++;
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_TRUE(odin3_net_live(m2, drop));
            TEST_ASSERT_EQUAL_UINT32(keep_pins, odin3_net_pins(m2, keep).count);
            TEST_ASSERT_EQUAL_UINT32(drop_pins, odin3_net_pins(m2, drop).count);
            odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(m2, opts));
        }
        lf_end(&f2);
    }
    TEST_ASSERT_TRUE_MESSAGE(failures > 0, "odin3_net_merge never failed: spec 12.2 can promise "
                                           "full atomicity");
}

static void test_bits_vector_basics(void) {
    lw_bits bits;
    lw_bits_init(&bits);
    TEST_ASSERT_EQUAL_UINT32(0, lw_bits_len(&bits));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lw_bits_push(&bits, inputs[3]));
    TEST_ASSERT_EQUAL_UINT32(1, lw_bits_len(&bits));
    TEST_ASSERT_EQUAL_UINT32(inputs[3].v, lw_bits_at(&bits, 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, lw_bits_span(&bits).n);
    lw_bits_clear(&bits);
    TEST_ASSERT_EQUAL_UINT32(0, lw_bits_len(&bits));
    lw_bits_free(&bits);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_extend_unsigned_pads_with_the_constant_zero);
    RUN_TEST(test_extend_signed_repeats_the_top_bit);
    RUN_TEST(test_extend_truncates_and_keeps_equal_widths);
    RUN_TEST(test_both_signed_needs_both_flags_and_warns_on_a_mixed_cell);
    RUN_TEST(test_both_signed_warning_text_is_pinned);
    RUN_TEST(test_a_unary_cell_is_signed_by_a_signed_alone);
    RUN_TEST(test_tree_nodes_and_depth_match_the_closed_form_for_1_to_40_inputs);
    RUN_TEST(test_tree_of_five_carries_the_odd_bit_to_the_root);
    RUN_TEST(test_tree_of_seven_pairs_the_carried_bit_at_the_next_level);
    RUN_TEST(test_connect_keeps_the_dropped_net_name_and_wire_bit_as_aliases);
    RUN_TEST(test_connect_moves_the_sinks_and_keeps_a_port_net_driven);
    RUN_TEST(test_connect_into_the_canonical_constant_net);
    RUN_TEST(test_connect_with_keep_equal_to_drop_or_a_missing_net_changes_nothing);
    RUN_TEST(test_a_constant_is_made_once_and_reused);
    RUN_TEST(test_an_existing_module_constant_is_found_on_entry);
    RUN_TEST(test_the_per_cell_cap_stops_a_runaway_builder);
    RUN_TEST(test_the_budget_is_checked_every_so_many_created_nodes);
    RUN_TEST(test_live_node_count_follows_creation_and_rollback);
    RUN_TEST(test_oom_at_every_allocation_of_each_gate_helper);
    RUN_TEST(test_oom_at_every_allocation_of_constants_extension_and_trees);
    RUN_TEST(test_rollback_after_a_constant_makes_the_next_call_create_it_again);
    RUN_TEST(test_keep_is_copied_onto_created_nodes);
    RUN_TEST(test_net_merge_can_fail_and_a_failure_changes_nothing);
    RUN_TEST(test_bits_vector_basics);
    return UNITY_END();
}
