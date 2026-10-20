/*
 * test_fsm_extract.c — fsm_extract (4.8 plan Task 6; spec X-1 to X-6; Yosys 0.55
 * fsm_extract.cc): the states, the reset state, the control inputs and outputs, the transition
 * table of the 3-state machine of spec section 12 (its shape is Yosys's logged table), the
 * failures, the evaluation cap, and that the IR is unchanged.
 */
#include "backends/rtlil/writer.h"
#include "fsm_test.h"
#include "passes/fsm/fsm.h"
#include "util/alloc.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository root"
#endif

/* One module opened for the stages, with its extracted FSMs. */
typedef struct run {
    odin3_fsm_mod fm;
    odin3_fsm_opts opts;
    odin3_vec insts;
} run;

static run rn;

static void run_close(void) {
    odin3_fsm_inst *insts = rn.insts.data;
    for (size_t i = 0; i < rn.insts.len; i++) {
        odin3_fsm_inst_free(&insts[i]);
    }
    odin3_vec_free(&rn.insts);
    odin3_fsm_mod_close(&rn.fm);
    memset(&rn, 0, sizeof rn);
}

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    run_close();
    ft_close();
}

/* Detects and extracts module `name` of the current design. */
static void extract(const char *name) {
    run_close();
    odin3_vec_init(&rn.insts, sizeof(odin3_fsm_inst));
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_fsm_mod_open(&rn.fm, ft.design, ft_module(name), &rn.opts));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_fsm_detect_module(&rn.fm), ft_log());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_fsm_extract_module(&rn.fm, &rn.insts), ft_log());
}

static void read_micro(const char *name) {
    char path[FT_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/fsm/%s.v", ODIN3_SOURCE_DIR, name);
    ft_read_proc_file(path);
}

static const odin3_fsm_inst *only(void) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, rn.insts.len, ft_log());
    return rn.insts.data;
}

static const char *code_text(const uint8_t *bits, int32_t count) {
    static char text[64];
    for (int32_t i = 0; i < count; i++) {
        text[i] = odin3_fsm_bit_char(bits[count - 1 - i]);
    }
    text[count] = '\0';
    return text;
}

/* --- the machine of spec section 12 ------------------------------------------------------------
 */

static void test_three_state_machine(void) {
    read_micro("moore_arst");
    extract("moore_arst");
    const odin3_fsm_inst *inst = only();
    const odin3_fsm_data *data = &inst->data;
    TEST_ASSERT_EQUAL_INT(2, data->state_bits);
    TEST_ASSERT_EQUAL_UINT(3, data->states.len);
    /* Yosys's Const order: bit 0 decides first, so 2'10 comes before 2'01 */
    TEST_ASSERT_EQUAL_STRING("00", code_text(odin3_fsm_data_state(data, 0), 2));
    TEST_ASSERT_EQUAL_STRING("10", code_text(odin3_fsm_data_state(data, 1), 2));
    TEST_ASSERT_EQUAL_STRING("01", code_text(odin3_fsm_data_state(data, 2), 2));
    TEST_ASSERT_EQUAL_INT(0, data->reset_state); /* from the async reset */
    TEST_ASSERT_EQUAL_INT(1, data->num_inputs);
    TEST_ASSERT_EQUAL_INT(6, data->num_outputs); /* o, the compares and the 2-bit next state */
    TEST_ASSERT_EQUAL_UINT(4, data->trans.len);
}

/* The transitions: (state_in, go, state_out) as Yosys logged them. */
static void test_three_state_transitions(void) {
    read_micro("moore_arst");
    extract("moore_arst");
    const odin3_fsm_data *data = &only()->data;
    static const struct {
        int32_t in, out;
        uint8_t go;
    } k_want[] = {{0, 0, ODIN3_FSM_BIT_0},
                  {0, 2, ODIN3_FSM_BIT_1},
                  {1, 0, ODIN3_FSM_BIT_DC},
                  {2, 1, ODIN3_FSM_BIT_DC}};
    for (size_t i = 0; i < data->trans.len; i++) {
        const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        TEST_ASSERT_EQUAL_INT(k_want[i].in, tr->state_in);
        TEST_ASSERT_EQUAL_INT(k_want[i].out, tr->state_out);
        TEST_ASSERT_EQUAL_UINT8(k_want[i].go, tr->ctrl_in[0]);
    }
}

static void test_reset_state_guessed_from_the_mux_tree(void) {
    read_micro("sync_rst");
    extract("sync_rst");
    const odin3_fsm_data *data = &only()->data;
    TEST_ASSERT_EQUAL_INT(0, data->reset_state);
    TEST_ASSERT_EQUAL_STRING("00", code_text(odin3_fsm_data_state(data, 0), 2));
    TEST_ASSERT_EQUAL_INT(2, data->num_inputs); /* rst and go */
}

static void test_no_reset_state(void) {
    read_micro("no_reset");
    extract("no_reset");
    TEST_ASSERT_EQUAL_INT(-1, only()->data.reset_state);
}

static void test_one_hot_pmux_selects_exclude_each_other(void) {
    /* the compares of a case are exclusive: a transition needs no more than the one it tests */
    read_micro("ctrl_out_case");
    extract("ctrl_out_case");
    const odin3_fsm_data *data = &only()->data;
    for (size_t i = 0; i < data->trans.len; i++) {
        TEST_ASSERT_EQUAL_INT(1, data->num_inputs); /* only go; the compares are outputs */
    }
    TEST_ASSERT_EQUAL_UINT(5, data->trans.len);
}

static void test_undefined_next_state_is_ignored(void) {
    read_micro("x_next");
    extract("x_next");
    const odin3_fsm_data *data = &only()->data;
    TEST_ASSERT_EQUAL_UINT(3, data->trans.len); /* state 2 -> x is dropped */
}

static void test_selfloop_state_is_a_state(void) {
    read_micro("selfloop_state");
    extract("selfloop_state");
    TEST_ASSERT_EQUAL_UINT(4, only()->data.states.len);
}

/* True when the signal bits of vec are in Yosys's SigBit order, none twice. */
static bool sig_sorted(const odin3_vec *vec) {
    const odin3_fsm_sig *bits = vec->data;
    for (size_t i = 1; i < vec->len; i++) {
        if (odin3_fsm_sig_cmp(bits[i - 1], bits[i]) >= 0) {
            return false;
        }
    }
    return true;
}

/* True when no bit of lhs is in rhs. */
static bool sig_disjoint(const odin3_vec *lhs, const odin3_vec *rhs) {
    const odin3_fsm_sig *bits = lhs->data;
    for (size_t i = 0; i < lhs->len; i++) {
        if (odin3_fsm_sigvec_has(rhs, bits[i])) {
            return false;
        }
    }
    return true;
}

static void test_ctrl_inputs_are_sorted_and_not_outputs(void) {
    read_micro("sync_rst");
    extract("sync_rst");
    const odin3_fsm_inst *inst = only();
    TEST_ASSERT_TRUE(sig_sorted(&inst->ctrl_in));
    TEST_ASSERT_TRUE(sig_sorted(&inst->ctrl_out));
    TEST_ASSERT_TRUE(sig_disjoint(&inst->ctrl_in, &inst->ctrl_out));
}

static void test_register_facts_negedge_async(void) {
    read_micro("negedge_clk");
    extract("negedge_clk");
    const odin3_fsm_inst *inst = only();
    TEST_ASSERT_FALSE(inst->clk_pol);
    TEST_ASSERT_TRUE(inst->arst_pol);
    TEST_ASSERT_FALSE(odin3_fsm_sig_is_const(inst->arst));
    TEST_ASSERT_EQUAL_STRING("auto", inst->encoding);
}

static void test_register_facts_sync_reset(void) {
    read_micro("sync_rst");
    extract("sync_rst");
    TEST_ASSERT_TRUE(odin3_fsm_sig_is_const(only()->arst)); /* no async reset: a constant 0 */
}

static void test_user_encoding_is_carried(void) {
    read_micro("enc_user");
    extract("enc_user");
    TEST_ASSERT_EQUAL_STRING("user", only()->encoding);
}

/* True when the node IDs of vec ascend. */
static bool nodes_ascend(const odin3_vec *vec) {
    const uint32_t *nodes = vec->data;
    for (size_t i = 1; i < vec->len; i++) {
        if (nodes[i - 1] >= nodes[i]) {
            return false;
        }
    }
    return true;
}

static void test_provenance_inputs(void) {
    read_micro("moore_arst");
    extract("moore_arst");
    const odin3_fsm_inst *inst = only();
    TEST_ASSERT_TRUE(inst->dff_node != 0);
    TEST_ASSERT_TRUE(inst->mux_cells.len >= 1 && nodes_ascend(&inst->mux_cells));
    TEST_ASSERT_TRUE(inst->out_drivers.len >= 1 && nodes_ascend(&inst->out_drivers));
}

/* --- failures ----------------------------------------------------------------------------------
 */

static void test_open_tree_fails(void) {
    ft_read_proc("module t(input clk, input rst, input go, input [1:0] nxt, output o);\n"
                 "  (* fsm_encoding = \"auto\" *) reg [1:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0; else if (go) st <= nxt; else st <= 1;\n"
                 "  assign o = st == 1;\nendmodule\n");
    extract("t");
    TEST_ASSERT_EQUAL_UINT(0, rn.insts.len);
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_INFO,
               "fsm: extraction of 't.st' failed: state selection tree is not closed"),
        ft_log());
    TEST_ASSERT_EQUAL_STRING("auto", ft_wire_attr((ft_ref){"t", "st"}, "fsm_encoding"));
}

static void test_one_state_fails(void) {
    ft_read_proc("module t(input clk, input rst, output o);\n"
                 "  (* fsm_encoding = \"auto\" *) reg [1:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0; else st <= 0;\n"
                 "  assign o = st == 1;\nendmodule\n");
    extract("t");
    TEST_ASSERT_EQUAL_UINT(0, rn.insts.len);
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_INFO,
               "fsm: extraction of 't.st' failed: at least two states are required"),
        ft_log());
}

static void test_wire_without_register_fails(void) {
    ft_read_proc("module t(input a, input [1:0] b, output [1:0] y);\n"
                 "  (* fsm_encoding = \"auto\" *) wire [1:0] w = a ? b : 2'd1;\n"
                 "  assign y = w;\nendmodule\n");
    extract("t");
    TEST_ASSERT_EQUAL_UINT(0, rn.insts.len);
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_INFO,
               "fsm: extraction of 't.w' failed: incomplete input selection tree root"),
        ft_log());
}

static void test_cap_skips_the_fsm(void) {
    read_micro("moore_arst");
    rn.opts.max_evals = 3;
    run_close();
    odin3_vec_init(&rn.insts, sizeof(odin3_fsm_inst));
    rn.opts.max_evals = 3;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_fsm_mod_open(&rn.fm, ft.design, ft_module("moore_arst"), &rn.opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_detect_module(&rn.fm));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_extract_module(&rn.fm, &rn.insts));
    TEST_ASSERT_EQUAL_UINT(0, rn.insts.len);
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_WARN, "fsm: module 'moore_arst': state register 'st': giving up after "
                               "3 evaluations; not recoded"),
        ft_log());
    TEST_ASSERT_EQUAL_STRING("auto", ft_wire_attr((ft_ref){"moore_arst", "st"}, "fsm_encoding"));
}

/* --- the IR is not changed ---------------------------------------------------------------------
 */

static void test_ir_is_unchanged(void) {
    read_micro("mealy_arst");
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &before));
    extract("mealy_arst");
    only();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

static void test_deterministic(void) {
    read_micro("mealy_arst");
    extract("mealy_arst");
    odin3_fsm_data first;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_copy(&only()->data, &first));
    extract("mealy_arst");
    const odin3_fsm_data *second = &only()->data;
    TEST_ASSERT_EQUAL_UINT(first.trans.len, second->trans.len);
    for (size_t i = 0; i < first.trans.len; i++) {
        const odin3_fsm_trans *lhs = odin3_fsm_data_trans(&first, i);
        const odin3_fsm_trans *rhs = odin3_fsm_data_trans(second, i);
        TEST_ASSERT_EQUAL_INT(lhs->state_in, rhs->state_in);
        TEST_ASSERT_EQUAL_INT(lhs->state_out, rhs->state_out);
        TEST_ASSERT_EQUAL_MEMORY(lhs->ctrl_in, rhs->ctrl_in, (size_t)first.num_inputs);
        TEST_ASSERT_EQUAL_MEMORY(lhs->ctrl_out, rhs->ctrl_out, (size_t)first.num_outputs);
    }
    odin3_fsm_data_free(&first);
}

static void test_extract_oom(void) {
    read_micro("moore_arst");
    long failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 20000; after++) {
        run_close();
        odin3_vec_init(&rn.insts, sizeof(odin3_fsm_inst));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_fsm_mod_open(&rn.fm, ft.design, ft_module("moore_arst"), &rn.opts));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_detect_module(&rn.fm));
        odin3_util_set_alloc_fail_after(after);
        st = odin3_fsm_extract_module(&rn.fm, &rn.insts);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        failures += st == ODIN3_OK ? 0 : 1;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 20);
    only();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_three_state_machine);
    RUN_TEST(test_three_state_transitions);
    RUN_TEST(test_reset_state_guessed_from_the_mux_tree);
    RUN_TEST(test_no_reset_state);
    RUN_TEST(test_one_hot_pmux_selects_exclude_each_other);
    RUN_TEST(test_undefined_next_state_is_ignored);
    RUN_TEST(test_selfloop_state_is_a_state);
    RUN_TEST(test_ctrl_inputs_are_sorted_and_not_outputs);
    RUN_TEST(test_register_facts_negedge_async);
    RUN_TEST(test_register_facts_sync_reset);
    RUN_TEST(test_user_encoding_is_carried);
    RUN_TEST(test_provenance_inputs);
    RUN_TEST(test_open_tree_fails);
    RUN_TEST(test_one_state_fails);
    RUN_TEST(test_wire_without_register_fails);
    RUN_TEST(test_cap_skips_the_fsm);
    RUN_TEST(test_ir_is_unchanged);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_extract_oom);
    return UNITY_END();
}
