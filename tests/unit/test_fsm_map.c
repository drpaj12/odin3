/*
 * test_fsm_map.c — fsm_map and the provenance and attributes of a mapped FSM (4.8 plan Task 9;
 * spec M-1 to M-6, P-1 to P-3; Yosys 0.55 fsm_map.cc): the cells made for one-hot and binary
 * machines with and without an asynchronous reset, a one-state table, the state wire's name, the
 * old drivers of the control outputs, the DERIVED record and its parents, the attributes, a `keep`
 * register that survives opt_clean, the IR check after the pass, and out of memory.
 */
#include "backends/rtlil/writer.h"
#include "fsm_test.h"
#include "ir/check.h"
#include "ir/prov.h"
#include "passes/fsm/fsm.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository root"
#endif

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    ft_close();
}

static void read_micro(const char *name) {
    char path[FT_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/fsm/%s.v", ODIN3_SOURCE_DIR, name);
    ft_read_proc_file(path);
}

static void run_fsm(const char *args) {
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("fsm", args), ft_log());
}

/* The live nodes of module `m` of a cell type, in ID order. */
static uint32_t nodes_of(odin3_module *module, const char *type, odin3_node_id *out, uint32_t cap) {
    odin3_celltype_id id = {0};
    uint32_t count = 0;
    if (!odin3_celltype_find(ft.design, ft_intern(type), &id)) {
        return 0;
    }
    for (uint32_t node = 1; node < odin3_module_node_end(module); node++) {
        if (odin3_node_live(module, (odin3_node_id){node}) &&
            odin3_node_type(module, (odin3_node_id){node}).v == id.v && count < cap) {
            out[count++] = (odin3_node_id){node};
        }
    }
    return count;
}

enum { CAP = 64 };

/* The new state register: the one whose Q is the wire `st`. */
static odin3_node_id register_of(odin3_module *module, const char *type) {
    odin3_node_id nodes[CAP];
    uint32_t count = nodes_of(module, type, nodes, CAP);
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern("st"));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    odin3_net_id q0 = odin3_wire_net(module, wire, 0);
    for (uint32_t i = 0; i < count; i++) {
        odin3_pinslice qpins =
            odin3_node_port(module, nodes[i], strcmp(type, "$adff") == 0 ? 3U : 2U);
        if (qpins.count > 0 && odin3_pin_net(module, qpins.first).v == q0.v) {
            return nodes[i];
        }
    }
    TEST_FAIL_MESSAGE("no register drives the new state wire");
    return (odin3_node_id){0};
}

static int64_t param_int(odin3_module *module, odin3_node_id node, uint32_t index) {
    return odin3_node_param(module, node, index)->i;
}

/* --- one-hot -----------------------------------------------------------------------------------
 */

static void test_one_hot_with_an_async_reset(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_node_id reg = register_of(module, "$adff");
    TEST_ASSERT_EQUAL_INT64(3, param_int(module, reg, 0)); /* WIDTH: three states, three bits */
    TEST_ASSERT_EQUAL_INT64(1, param_int(module, reg, 1)); /* CLK_POLARITY */
    TEST_ASSERT_EQUAL_INT64(1, param_int(module, reg, 2)); /* ARST_POLARITY */
    const odin3_value *rst = odin3_node_param(module, reg, 3);
    TEST_ASSERT_EQUAL_UINT32(3, rst->len);
    TEST_ASSERT_EQUAL_UINT8(1, rst->bits[0]); /* the reset state is bit 0 */
    TEST_ASSERT_EQUAL_UINT8(0, rst->bits[1]);
    TEST_ASSERT_EQUAL_UINT8(0, rst->bits[2]);
    TEST_ASSERT_NOT_NULL(ft_wire_attr((ft_ref){"moore_arst", "st"}, "odin3_fsm_encoding"));
    TEST_ASSERT_TRUE_MESSAGE(ft_has(ODIN3_LOG_INFO, "fsm: mapped 'moore_arst.st' to 9 cells"),
                             ft_log());
}

static void test_the_state_wire_keeps_the_name_and_the_old_one_moves(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_wire_id fresh = odin3_module_find_wire(module, ft_intern("st"));
    odin3_wire_id old = odin3_module_find_wire(module, ft_intern("$fsm$oldstate\\st"));
    TEST_ASSERT_TRUE(odin3_wire_valid(fresh) && odin3_wire_valid(old));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_wire_width(module, fresh));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_wire_width(module, old));
    TEST_ASSERT_NULL(ft_wire_attr((ft_ref){"moore_arst", "$fsm$oldstate\\st"}, "fsm_encoding"));
}

static void test_onehot_attribute_and_fsm_attributes(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern("st"));
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    TEST_ASSERT_EQUAL_INT64(1, odin3_attr_get(module, obj, ft_intern("onehot"))->i);
    TEST_ASSERT_EQUAL_INT64(3, odin3_attr_get(module, obj, ft_intern("odin3_fsm_states"))->i);
    TEST_ASSERT_EQUAL_INT64(2, odin3_attr_get(module, obj, ft_intern("odin3_fsm_orig_bits"))->i);
    TEST_ASSERT_EQUAL_STRING("one-hot",
                             ft_wire_attr((ft_ref){"moore_arst", "st"}, "odin3_fsm_encoding"));
    TEST_ASSERT_EQUAL_STRING("st",
                             ft_wire_attr((ft_ref){"moore_arst", "st"}, "odin3_fsm_orig_name"));
}

static void test_register_node_carries_the_attributes(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_node_id reg = register_of(module, "$adff");
    odin3_objref obj = {ODIN3_OBJ_NODE, reg.v};
    TEST_ASSERT_EQUAL_INT64(3, odin3_attr_get(module, obj, ft_intern("odin3_fsm_states"))->i);
    TEST_ASSERT_NOT_NULL(odin3_attr_get(module, obj, ft_intern("odin3_fsm_encoding")));
}

/* --- reset styles ------------------------------------------------------------------------------
 */

static void test_sync_reset_makes_a_plain_dff(void) {
    read_micro("sync_rst");
    run_fsm("");
    odin3_module *module = ft_module("sync_rst");
    odin3_node_id reg = register_of(module, "$dff");
    TEST_ASSERT_EQUAL_INT64(3, param_int(module, reg, 0));
    TEST_ASSERT_EQUAL_UINT32(2,
                             odin3_celltype_get(ft.design, odin3_node_type(module, reg))->n_params);
}

static void test_active_low_reset_polarity_is_copied(void) {
    read_micro("arst_low");
    run_fsm("");
    odin3_module *module = ft_module("arst_low");
    odin3_node_id reg = register_of(module, "$adff");
    TEST_ASSERT_EQUAL_INT64(0, param_int(module, reg, 2));
}

static void test_negedge_polarity_is_copied(void) {
    read_micro("negedge_clk");
    run_fsm("");
    odin3_module *module = ft_module("negedge_clk");
    TEST_ASSERT_EQUAL_INT64(0, param_int(module, register_of(module, "$adff"), 1));
}

/* --- binary ------------------------------------------------------------------------------------
 */

static void test_binary_makes_decoders_and_a_pmux(void) {
    read_micro("enc_binary");
    run_fsm("");
    odin3_module *module = ft_module("enc_binary");
    odin3_node_id reg = register_of(module, "$adff");
    TEST_ASSERT_EQUAL_INT64(2, param_int(module, reg, 0)); /* 4 states: 2 bits */
    odin3_node_id found[CAP];
    TEST_ASSERT_TRUE(nodes_of(module, "$pmux", found, CAP) >= 1);
    TEST_ASSERT_NULL(odin3_attr_get(
        module, (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(module, ft_intern("st")).v},
        ft_intern("onehot")));
    TEST_ASSERT_EQUAL_STRING("binary",
                             ft_wire_attr((ft_ref){"enc_binary", "st"}, "odin3_fsm_encoding"));
}

static void test_user_encoding_keeps_its_codes_but_is_mapped(void) {
    read_micro("enc_user");
    run_fsm("");
    odin3_module *module = ft_module("enc_user");
    TEST_ASSERT_EQUAL_INT64(2, param_int(module, register_of(module, "$adff"), 0));
    TEST_ASSERT_EQUAL_STRING("user",
                             ft_wire_attr((ft_ref){"enc_user", "st"}, "odin3_fsm_encoding"));
}

/* --- rejected registers are left alone ---------------------------------------------------------
 */

static void test_a_rejected_register_gets_no_attributes(void) {
    read_micro("self_reset");
    run_fsm("");
    TEST_ASSERT_NULL(ft_wire_attr((ft_ref){"self_reset", "st"}, "odin3_fsm_states"));
    TEST_ASSERT_EQUAL_UINT32(0, ft_count_nodes((ft_ref){"self_reset", "$fsm_none"}));
    odin3_module *module = ft_module("self_reset");
    TEST_ASSERT_EQUAL_UINT32(
        2, odin3_wire_width(module, odin3_module_find_wire(module, ft_intern("st"))));
}

static void test_nomap_leaves_the_ir_unchanged(void) {
    read_micro("mealy_arst");
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &before));
    run_fsm("-nomap");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

/* --- the control outputs -----------------------------------------------------------------------
 */

static void test_the_output_port_has_one_driver_the_new_logic(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern("o"));
    odin3_net_id net = odin3_wire_net(module, wire, 0);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(module, net));
    odin3_pin_id pin = odin3_net_driver(module, net);
    const odin3_celltype_def *def =
        odin3_celltype_get(ft.design, odin3_node_type(module, odin3_pin_node(module, pin)));
    TEST_ASSERT_EQUAL_STRING("$reduce_or", def->name);
}

/* --- provenance --------------------------------------------------------------------------------
 */

static void test_derived_record_has_the_parents_of_the_old_logic(void) {
    read_micro("moore_arst");
    odin3_module *module = ft_module("moore_arst");
    odin3_node_id old = {0};
    odin3_node_id found[CAP];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of(module, "$adff", found, CAP));
    old = found[0];
    odin3_prov_id old_prov = odin3_node_prov(module, old);
    run_fsm("");
    module = ft_module("moore_arst");
    odin3_node_id reg = register_of(module, "$adff");
    odin3_prov_id prov = odin3_node_prov(module, reg);
    const odin3_prov_record *rec = odin3_prov_get(ft.design, prov);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_TRUE(rec->parents.count >= 2);
    TEST_ASSERT_EQUAL_UINT32(old_prov.v, rec->parents.ids[0].v); /* the old register first */
}

static void test_everything_new_shares_one_record(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_prov_id prov = odin3_node_prov(module, register_of(module, "$adff"));
    odin3_node_id found[CAP];
    uint32_t count = nodes_of(module, "$reduce_or", found, CAP);
    TEST_ASSERT_TRUE(count >= 1);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(module, found[i]).v);
    }
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern("st"));
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_wire_prov(module, wire).v);
}

static void test_the_new_register_reaches_the_source(void) {
    read_micro("moore_arst");
    run_fsm("");
    odin3_module *module = ft_module("moore_arst");
    odin3_prov_id prov = odin3_node_prov(module, register_of(module, "$adff"));
    TEST_ASSERT_TRUE(prov.v != 0);
    odin3_prov_list parents = odin3_prov_parents(ft.design, prov);
    TEST_ASSERT_TRUE(parents.count >= 1);
}

/* --- keep, and the old register -------------------------------------------------------------- */

static uint32_t registers_of_width(odin3_module *module, int64_t width) {
    odin3_node_id found[CAP];
    uint32_t count = nodes_of(module, "$adff", found, CAP);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < count; i++) {
        hits += param_int(module, found[i], 0) == width ? 1U : 0U;
    }
    return hits;
}

static void test_the_old_register_goes_with_opt_clean(void) {
    read_micro("moore_arst");
    run_fsm("");
    TEST_ASSERT_EQUAL_UINT32(1, registers_of_width(ft_module("moore_arst"), 2)); /* still there */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("opt_clean", ""));
    TEST_ASSERT_EQUAL_UINT32(0, registers_of_width(ft_module("moore_arst"), 2));
    TEST_ASSERT_EQUAL_UINT32(1, registers_of_width(ft_module("moore_arst"), 3));
}

static void test_a_keep_register_survives_opt_clean(void) {
    read_micro("keep_reg");
    run_fsm("");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("opt_clean", ""));
    TEST_ASSERT_EQUAL_UINT32(1, registers_of_width(ft_module("keep_reg"), 2));
}

/* --- encoding option ---------------------------------------------------------------------------
 */

static void test_encoding_option_forces_binary(void) {
    read_micro("moore_arst");
    run_fsm("-encoding binary");
    odin3_module *module = ft_module("moore_arst");
    TEST_ASSERT_EQUAL_INT64(2, param_int(module, register_of(module, "$adff"), 0));
}

static void test_unsupported_options_are_errors(void) {
    read_micro("moore_arst");
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-expand"));
    TEST_ASSERT_TRUE_MESSAGE(ft_has(ODIN3_LOG_ERROR, "fsm: option '-expand' is not supported"),
                             ft_log());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-bogus"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-encoding gray"));
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_ERROR, "fsm: unsupported encoding 'gray'"));
}

/* --- a one-state table --------------------------------------------------------------------------
 */

static void test_one_state_table_has_a_constant_next_state(void) {
    read_micro("moore_arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", ""));
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_fsm_mod fm;
    odin3_vec insts;
    odin3_vec_init(&insts, sizeof(odin3_fsm_inst));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_fsm_mod_open(&fm, ft.design, ft_module("moore_arst"), &opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_extract_module(&fm, &insts));
    odin3_fsm_inst *inst = insts.data;
    /* keep the reset state and its self-loop only */
    odin3_kit_truncate(&inst->data.states, 1);
    inst->data.trans.len = 1;
    TEST_ASSERT_EQUAL_INT(0, odin3_fsm_data_trans(&inst->data, 0)->state_out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_recode(&fm, inst));
    TEST_ASSERT_EQUAL_INT(1, inst->data.state_bits);
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(ft.design, ft_intern("fsm"), &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_map(&fm, &ctx, inst));
    odin3_module *module = ft_module("moore_arst");
    odin3_node_id reg = register_of(module, "$adff");
    odin3_pinslice d_pin = odin3_node_port(module, reg, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1,
                          odin3_net_const_value(module, odin3_pin_net(module, d_pin.first)));
    odin3_check_opts check = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(ft.design, check));
    odin3_fsm_inst_free(inst);
    odin3_vec_free(&insts);
    odin3_fsm_mod_close(&fm);
}

/* --- out of memory -----------------------------------------------------------------------------
 */

/* The IR text now. */
static char *rtlil_now(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &text));
    return text.data;
}

/* One fsm run with the allocator failing after `after` allocations; true when it failed with
 * the IR as it was before (`original`). The IR passes the full check either way. */
static bool run_failing(odin3_pass_ctx *ctx, const char *original, long after, odin3_status *st) {
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.nodetect = true;
    odin3_util_set_alloc_fail_after(after);
    *st = odin3_fsm_run(ctx, ft.design, &opts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(*st == ODIN3_OK || *st == ODIN3_ERR_NO_MEMORY);
    odin3_check_opts check = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(ft.design, check));
    if (*st == ODIN3_OK) {
        return false;
    }
    char *now = rtlil_now();
    bool same = strcmp(now, original) == 0;
    free(now);
    return same;
}

static void test_map_oom_leaves_a_valid_ir(void) {
    read_micro("moore_arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", "")); /* the marks stay across failures */
    char *original = rtlil_now();
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(ft.design, ft_intern("fsm"), &ctx));
    long failures = 0;
    long unchanged = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 6000; after++) {
        unchanged += run_failing(&ctx, original, after, &st) ? 1 : 0;
        failures += st == ODIN3_OK ? 0 : 1;
    }
    free(original);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 50);
    TEST_ASSERT_TRUE_MESSAGE(unchanged >= failures / 2, "most failures leave the IR as it was");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_one_hot_with_an_async_reset);
    RUN_TEST(test_the_state_wire_keeps_the_name_and_the_old_one_moves);
    RUN_TEST(test_onehot_attribute_and_fsm_attributes);
    RUN_TEST(test_register_node_carries_the_attributes);
    RUN_TEST(test_sync_reset_makes_a_plain_dff);
    RUN_TEST(test_active_low_reset_polarity_is_copied);
    RUN_TEST(test_negedge_polarity_is_copied);
    RUN_TEST(test_binary_makes_decoders_and_a_pmux);
    RUN_TEST(test_user_encoding_keeps_its_codes_but_is_mapped);
    RUN_TEST(test_a_rejected_register_gets_no_attributes);
    RUN_TEST(test_nomap_leaves_the_ir_unchanged);
    RUN_TEST(test_the_output_port_has_one_driver_the_new_logic);
    RUN_TEST(test_derived_record_has_the_parents_of_the_old_logic);
    RUN_TEST(test_everything_new_shares_one_record);
    RUN_TEST(test_the_new_register_reaches_the_source);
    RUN_TEST(test_the_old_register_goes_with_opt_clean);
    RUN_TEST(test_a_keep_register_survives_opt_clean);
    RUN_TEST(test_encoding_option_forces_binary);
    RUN_TEST(test_unsupported_options_are_errors);
    RUN_TEST(test_one_state_table_has_a_constant_next_state);
    RUN_TEST(test_map_oom_leaves_a_valid_ir);
    return UNITY_END();
}
