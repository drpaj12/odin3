/*
 * test_fsm_opt.c — fsm_opt and the unused-output query (4.8 plan Task 7; spec O-0 to O-5; Yosys
 * 0.55 fsm_opt.cc): each stage on a hand-made table with the result worked out from the Yosys
 * source, the stage order, and the query on modules read from Verilog (an output read only by
 * the dead old logic is unused; by a live cell, a port or `keep` logic it is used).
 *
 * Tables are written as text: a state is its code (bit 0 first), a transition is
 * "in>out ctrl_in|ctrl_out" with the bits 0 1 x - in the same order.
 */
#include "fsm_test.h"
#include "passes/fsm/fsm.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository root"
#endif

enum { TABLE_TEXT = 4096, FIRST_NET = 10 };

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    ft_close();
}

/* --- hand-made tables --------------------------------------------------------------------------
 */

static uint8_t bit_of(char ch) {
    static const char k_chars[] = "01xz-m";
    return (uint8_t)(strchr(k_chars, ch) - k_chars);
}

/* The shape of a table: its sizes and reset state. */
typedef struct shape {
    int32_t state_bits, n_in, n_out, reset;
} shape;

/* An instance with the states of `states` (n of them) and control nets FIRST_NET.. */
static void make(odin3_fsm_inst *inst, shape sh, const char *const *states, uint32_t count) {
    memset(inst, 0, sizeof *inst);
    odin3_vec_init(&inst->ctrl_in, sizeof(odin3_fsm_sig));
    odin3_vec_init(&inst->ctrl_out, sizeof(odin3_fsm_sig));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_init(&inst->data));
    inst->data.state_bits = sh.state_bits;
    inst->data.num_inputs = sh.n_in;
    inst->data.num_outputs = sh.n_out;
    inst->data.reset_state = sh.reset;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t code[16];
        for (int32_t k = 0; k < sh.state_bits; k++) {
            code[k] = bit_of(states[i][k]);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_add_state(&inst->data, code));
    }
    for (int32_t i = 0; i < sh.n_in; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_kit_push_u32(&inst->ctrl_in, FIRST_NET + (uint32_t)i));
    }
    for (int32_t i = 0; i < sh.n_out; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_kit_push_u32(&inst->ctrl_out, FIRST_NET + 100U + (uint32_t)i));
    }
}

/* One transition of the table. */
typedef struct tr_text {
    int32_t state_in, state_out;
    const char *ctrl_in, *ctrl_out;
} tr_text;

static void add(odin3_fsm_inst *inst, tr_text tt) {
    odin3_fsm_data *data = &inst->data;
    odin3_fsm_trans tr = {tt.state_in, tt.state_out,
                          odin3_fsm_data_bits(data, (size_t)data->num_inputs),
                          odin3_fsm_data_bits(data, (size_t)data->num_outputs)};
    for (int32_t i = 0; i < data->num_inputs; i++) {
        tr.ctrl_in[i] = bit_of(tt.ctrl_in[i]);
    }
    for (int32_t i = 0; i < data->num_outputs; i++) {
        tr.ctrl_out[i] = bit_of(tt.ctrl_out[i]);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_add_trans(data, &tr));
}

/* The table as text: "reset=R states=a,b trans: 0>1 01|10; ...". */
static const char *dump(const odin3_fsm_inst *inst) {
    static char buf[TABLE_TEXT];
    const odin3_fsm_data *data = &inst->data;
    size_t len =
        (size_t)snprintf(buf, sizeof buf, "reset=%d in=%d out=%d states=", data->reset_state,
                         data->num_inputs, data->num_outputs);
    for (size_t i = 0; i < data->states.len; i++) {
        for (int32_t k = 0; k < data->state_bits; k++) {
            buf[len++] = odin3_fsm_bit_char(odin3_fsm_data_state(data, i)[k]);
        }
        buf[len++] = i + 1 < data->states.len ? ',' : ' ';
    }
    for (size_t i = 0; i < data->trans.len; i++) {
        const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, i);
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%d>%d ", tr->state_in, tr->state_out);
        for (int32_t k = 0; k < data->num_inputs; k++) {
            buf[len++] = odin3_fsm_bit_char(tr->ctrl_in[k]);
        }
        buf[len++] = '|';
        for (int32_t k = 0; k < data->num_outputs; k++) {
            buf[len++] = odin3_fsm_bit_char(tr->ctrl_out[k]);
        }
        buf[len++] = ';';
    }
    buf[len] = '\0';
    return buf;
}

static void done(odin3_fsm_inst *inst) {
    odin3_vec_free(&inst->ctrl_in);
    odin3_vec_free(&inst->ctrl_out);
    odin3_fsm_data_free(&inst->data);
}

/* --- O-0 ----------------------------------------------------------------------------------------
 */

static const char *const k_four[] = {"00", "10", "01", "11"};

static void test_unreachable_states_go_to_a_fixpoint(void) {
    /* A -> B, B -> A, C -> D, D -> A: C has no way in, and with it gone D has none either */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 1, 0, 0}, k_four, 4);
    add(&inst, (tr_text){0, 1, "0", ""});
    add(&inst, (tr_text){1, 0, "-", ""});
    add(&inst, (tr_text){2, 3, "-", ""});
    add(&inst, (tr_text){3, 0, "-", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_unreachable(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=0 states=00,10 0>1 0|;1>0 -|;", dump(&inst));
    done(&inst);
}

static void test_unreachable_renumbers_and_moves_the_reset(void) {
    /* the reset state is B (1): A has no incoming transition and goes; B and C stay */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 1, 0, 1}, k_four, 3);
    add(&inst, (tr_text){0, 1, "-", ""});
    add(&inst, (tr_text){1, 2, "-", ""});
    add(&inst, (tr_text){2, 1, "-", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_unreachable(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=0 states=10,01 0>1 -|;1>0 -|;", dump(&inst));
    done(&inst);
}

static void test_unreachable_without_a_reset_state(void) {
    /* no reset state: every state that is no transition's target goes, as in Yosys */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 0, 0, -1}, k_four, 3);
    add(&inst, (tr_text){0, 1, "", ""});
    add(&inst, (tr_text){1, 2, "", ""});
    add(&inst, (tr_text){2, 1, "", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_unreachable(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=-1 in=0 out=0 states=10,01 0>1 |;1>0 |;", dump(&inst));
    done(&inst);
}

static void test_a_cycle_of_unreachable_states_stays(void) {
    /* Yosys's test is "has an incoming transition": two states that feed each other stay */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 0, 0, 0}, k_four, 3);
    add(&inst, (tr_text){0, 0, "", ""});
    add(&inst, (tr_text){1, 2, "", ""});
    add(&inst, (tr_text){2, 1, "", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_unreachable(&inst));
    TEST_ASSERT_EQUAL_UINT(3, inst.data.states.len);
    done(&inst);
}

/* --- O-1 ----------------------------------------------------------------------------------------
 */

static void test_unused_outputs_are_removed(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 0, 3, 0}, k_four, 2);
    add(&inst, (tr_text){0, 1, "", "101"});
    add(&inst, (tr_text){1, 0, "", "010"});
    const uint8_t unused[3] = {0, 1, 0};
    odin3_fsm_opt_unused_outputs(&inst, unused);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=0 out=2 states=00,10 0>1 |11;1>0 |00;", dump(&inst));
    TEST_ASSERT_EQUAL_UINT(2, inst.ctrl_out.len);
    TEST_ASSERT_EQUAL_UINT32(FIRST_NET + 100U, ((odin3_fsm_sig *)inst.ctrl_out.data)[0]);
    TEST_ASSERT_EQUAL_UINT32(FIRST_NET + 102U, ((odin3_fsm_sig *)inst.ctrl_out.data)[1]);
    done(&inst);
}

static void test_adjacent_unused_outputs(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 0, 4, 0}, k_four, 1);
    add(&inst, (tr_text){0, 0, "", "1011"});
    const uint8_t unused[4] = {0, 1, 1, 0};
    odin3_fsm_opt_unused_outputs(&inst, unused);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=0 out=2 states=00 0>0 |11;", dump(&inst));
    done(&inst);
}

static void test_no_unused_flags_changes_nothing(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 0, 2, 0}, k_four, 1);
    add(&inst, (tr_text){0, 0, "", "10"});
    odin3_fsm_opt_unused_outputs(&inst, NULL);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=0 out=2 states=00 0>0 |10;", dump(&inst));
    done(&inst);
}

/* --- O-2 ----------------------------------------------------------------------------------------
 */

static void test_aliased_inputs_merge_their_patterns(void) {
    /* inputs 0 and 2 are one net: a pattern that disagrees goes, a don't care takes the other */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 3, 0, 0}, k_four, 2);
    ((odin3_fsm_sig *)inst.ctrl_in.data)[2] = ((odin3_fsm_sig *)inst.ctrl_in.data)[0];
    add(&inst, (tr_text){0, 1, "1x1", ""}); /* agrees: kept, input 2 removed */
    add(&inst, (tr_text){0, 0, "10-", ""}); /* j is a don't care: takes i */
    add(&inst, (tr_text){1, 0, "01-", ""});
    add(&inst, (tr_text){1, 1, "-00", ""}); /* i is a don't care: takes j */
    add(&inst, (tr_text){1, 0, "010", ""}); /* disagrees (i = 0, j = 0 agrees!) */
    odin3_fsm_opt_alias_inputs(&inst);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=2 out=0 states=00,10 0>1 1x|;0>0 10|;1>0 01|;1>1 00|;"
                             "1>0 01|;",
                             dump(&inst));
    TEST_ASSERT_EQUAL_UINT(2, inst.ctrl_in.len);
    done(&inst);
}

static void test_alias_conflict_drops_the_transition(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 2, 0, 0}, k_four, 2);
    ((odin3_fsm_sig *)inst.ctrl_in.data)[1] = ((odin3_fsm_sig *)inst.ctrl_in.data)[0];
    add(&inst, (tr_text){0, 1, "10", ""});
    add(&inst, (tr_text){1, 0, "11", ""});
    odin3_fsm_opt_alias_inputs(&inst);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=0 states=00,10 1>0 1|;", dump(&inst));
    done(&inst);
}

/* --- O-3 ----------------------------------------------------------------------------------------
 */

static void test_feedback_input_is_removed(void) {
    /* input 1 is also output 0: a pattern must be a don't care or equal the output */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 2, 2, 0}, k_four, 2);
    ((odin3_fsm_sig *)inst.ctrl_in.data)[1] = ((odin3_fsm_sig *)inst.ctrl_out.data)[0];
    add(&inst, (tr_text){0, 1, "01", "10"}); /* in = 1, out = 1: kept */
    add(&inst, (tr_text){0, 0, "00", "10"}); /* in = 0, out = 1: dropped */
    add(&inst, (tr_text){1, 0, "1-", "01"}); /* don't care: kept */
    odin3_fsm_opt_feedback_inputs(&inst);
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=2 states=00,10 0>1 0|10;1>0 1|01;", dump(&inst));
    done(&inst);
}

/* --- O-4 ----------------------------------------------------------------------------------------
 */

static void test_dont_care_merges_to_a_fixpoint(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 2, 1, 0}, k_four, 2);
    add(&inst, (tr_text){0, 1, "00", "1"});
    add(&inst, (tr_text){0, 1, "10", "1"});
    add(&inst, (tr_text){0, 1, "01", "1"});
    add(&inst, (tr_text){0, 1, "11", "1"});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_dont_care(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=2 out=1 states=00,10 0>1 --|1;", dump(&inst));
    done(&inst);
}

static void test_dont_care_groups_by_state_and_output(void) {
    /* different ctrl_out or state_out: no merge; the table comes back in group order */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 1, 1, 0}, k_four, 2);
    add(&inst, (tr_text){1, 0, "0", "1"});
    add(&inst, (tr_text){0, 1, "1", "0"});
    add(&inst, (tr_text){0, 1, "0", "1"});
    add(&inst, (tr_text){0, 0, "1", "0"});
    add(&inst, (tr_text){0, 0, "0", "0"});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_dont_care(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=1 states=00,10 0>0 -|0;0>1 1|0;0>1 0|1;1>0 0|1;",
                             dump(&inst));
    done(&inst);
}

static void test_dont_care_leaves_patterns_that_do_not_pair(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 2, 0, 0}, k_four, 2);
    add(&inst, (tr_text){0, 1, "00", ""});
    add(&inst, (tr_text){0, 1, "11", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_dont_care(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=2 out=0 states=00,10 0>1 00|;0>1 11|;", dump(&inst));
    done(&inst);
}

static void test_dont_care_drops_repeated_patterns(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 1, 0, 0}, k_four, 2);
    add(&inst, (tr_text){0, 1, "0", ""});
    add(&inst, (tr_text){0, 1, "0", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_dont_care(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=0 states=00,10 0>1 0|;", dump(&inst));
    done(&inst);
}

/* --- O-5 ----------------------------------------------------------------------------------------
 */

static void test_constant_and_unused_inputs(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 3, 0, 0}, k_four, 2);
    odin3_fsm_sig *ctrl = inst.ctrl_in.data;
    ctrl[1] = odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_1); /* input 1 is the constant 1 */
    add(&inst, (tr_text){0, 1, "01-", ""});          /* matches the constant; input 2 is unused */
    add(&inst, (tr_text){0, 0, "00-", ""});          /* contradicts it: dropped */
    add(&inst, (tr_text){1, 0, "1x-", ""});          /* x is not a requirement */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_const_inputs(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=1 out=0 states=00,10 0>1 0|;1>0 1|;", dump(&inst));
    TEST_ASSERT_EQUAL_UINT32(FIRST_NET + 0U, ((odin3_fsm_sig *)inst.ctrl_in.data)[0]);
    done(&inst);
}

static void test_all_inputs_unused(void) {
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 2, 0, 0}, k_four, 2);
    add(&inst, (tr_text){0, 1, "--", ""});
    add(&inst, (tr_text){1, 0, "--", ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt_const_inputs(&inst));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=0 out=0 states=00,10 0>1 |;1>0 |;", dump(&inst));
    done(&inst);
}

/* --- the stages in order ------------------------------------------------------------------------
 */

static void test_stages_run_in_yosys_order(void) {
    /* an unreachable state, an unused output and a mergeable pair, all in one table */
    odin3_fsm_inst inst;
    make(&inst, (shape){2, 1, 2, 0}, k_four, 3);
    add(&inst, (tr_text){0, 1, "0", "11"});
    add(&inst, (tr_text){0, 1, "1", "10"}); /* with output 1 gone this merges with the line above */
    add(&inst, (tr_text){1, 0, "-", "00"});
    add(&inst, (tr_text){2, 0, "-", "00"}); /* state 2 has no way in */
    const uint8_t unused[2] = {0, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt(&inst, unused));
    TEST_ASSERT_EQUAL_STRING("reset=0 in=0 out=1 states=00,10 0>1 |1;1>0 |0;", dump(&inst));
    done(&inst);
}

static void test_opt_oom(void) {
    long failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 500; after++) {
        odin3_fsm_inst inst;
        make(&inst, (shape){2, 2, 1, 0}, k_four, 2);
        add(&inst, (tr_text){0, 1, "00", "1"});
        add(&inst, (tr_text){0, 1, "10", "1"});
        odin3_util_set_alloc_fail_after(after);
        st = odin3_fsm_opt(&inst, NULL);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        failures += st == ODIN3_OK ? 0 : 1;
        done(&inst);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 1);
}

/* --- the unused-output query --------------------------------------------------------------------
 */

typedef struct modrun {
    odin3_fsm_mod fm;
    odin3_fsm_opts opts;
    odin3_vec insts;
} modrun;

static modrun mr;

static void mr_close(void) {
    odin3_fsm_inst *insts = mr.insts.data;
    for (size_t i = 0; i < mr.insts.len; i++) {
        odin3_fsm_inst_free(&insts[i]);
    }
    odin3_vec_free(&mr.insts);
    odin3_fsm_mod_close(&mr.fm);
    memset(&mr, 0, sizeof mr);
}

/* Reads a micro, detects and extracts, and runs the first opt, then the second with the query. */
static const odin3_fsm_inst *optimized(const char *micro) {
    mr_close();
    char path[FT_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/fsm/%s.v", ODIN3_SOURCE_DIR, micro);
    ft_read_proc_file(path);
    odin3_vec_init(&mr.insts, sizeof(odin3_fsm_inst));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_fsm_mod_open(&mr.fm, ft.design, ft_module(micro), &mr.opts));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_detect_module(&mr.fm));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_extract_module(&mr.fm, &mr.insts));
    TEST_ASSERT_EQUAL_UINT(1, mr.insts.len);
    odin3_fsm_inst *inst = mr.insts.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt(inst, NULL));
    uint8_t *used = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_used_nets(&mr.fm, &mr.insts, &used));
    odin3_vec flags;
    odin3_vec_init(&flags, sizeof(uint8_t));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_unused_flags(inst, used, &flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_opt(inst, flags.data));
    odin3_vec_free(&flags);
    odin3_util_free(used);
    return inst;
}

static void test_dead_old_logic_outputs_are_unused(void) {
    /* Yosys 0.55 keeps one output of this machine, the port o (verified with fsm -nomap) */
    const odin3_fsm_inst *inst = optimized("moore_arst");
    TEST_ASSERT_EQUAL_INT(1, inst->data.num_outputs);
}

static void test_live_old_register_keeps_the_next_state_outputs(void) {
    /* a reader of Q that is no compare keeps the old register alive: o and the 2 next-state bits */
    const odin3_fsm_inst *inst = optimized("live_old_reg");
    TEST_ASSERT_EQUAL_INT(3, inst->data.num_outputs);
}

static void test_keep_register_keeps_the_next_state_outputs(void) {
    const odin3_fsm_inst *inst = optimized("keep_reg");
    TEST_ASSERT_EQUAL_INT(3, inst->data.num_outputs); /* Yosys: 3 for the keep micro */
}

static void test_outputs_read_by_live_logic_are_used(void) {
    const odin3_fsm_inst *inst = optimized("ctrl_out_case");
    TEST_ASSERT_EQUAL_INT(
        4, inst->data.num_outputs); /* Yosys 0.55: four, the compares the ports use */
}

static void test_query_on_a_module_without_fsms(void) {
    mr_close();
    ft_read_proc("module t(input a, output y);\n  assign y = ~a;\nendmodule\n");
    odin3_vec_init(&mr.insts, sizeof(odin3_fsm_inst));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_fsm_mod_open(&mr.fm, ft.design, ft_module("t"), &mr.opts));
    uint8_t *used = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_used_nets(&mr.fm, &mr.insts, &used));
    TEST_ASSERT_NOT_NULL(used);
    odin3_util_free(used);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unreachable_states_go_to_a_fixpoint);
    RUN_TEST(test_unreachable_renumbers_and_moves_the_reset);
    RUN_TEST(test_unreachable_without_a_reset_state);
    RUN_TEST(test_a_cycle_of_unreachable_states_stays);
    RUN_TEST(test_unused_outputs_are_removed);
    RUN_TEST(test_adjacent_unused_outputs);
    RUN_TEST(test_no_unused_flags_changes_nothing);
    RUN_TEST(test_aliased_inputs_merge_their_patterns);
    RUN_TEST(test_alias_conflict_drops_the_transition);
    RUN_TEST(test_feedback_input_is_removed);
    RUN_TEST(test_dont_care_merges_to_a_fixpoint);
    RUN_TEST(test_dont_care_groups_by_state_and_output);
    RUN_TEST(test_dont_care_leaves_patterns_that_do_not_pair);
    RUN_TEST(test_dont_care_drops_repeated_patterns);
    RUN_TEST(test_constant_and_unused_inputs);
    RUN_TEST(test_all_inputs_unused);
    RUN_TEST(test_stages_run_in_yosys_order);
    RUN_TEST(test_opt_oom);
    RUN_TEST(test_dead_old_logic_outputs_are_unused);
    RUN_TEST(test_live_old_register_keeps_the_next_state_outputs);
    RUN_TEST(test_keep_register_keeps_the_next_state_outputs);
    RUN_TEST(test_outputs_read_by_live_logic_are_used);
    RUN_TEST(test_query_on_a_module_without_fsms);
    mr_close();
    return UNITY_END();
}
