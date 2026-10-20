/*
 * test_fsm_data.c — the FSM table and the net index of the fsm pass (4.8 plan Task 3; spec section
 * 4): the Const comparator against Yosys's order (rtlil.cc:316-326), the table's copy, free and
 * out-of-memory behaviour, and the per-module index (drivers and users per net, port nets,
 * constants, unknown nodes).
 */
#include "ir/ids.h"
#include "ir/module.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static pb_result res;

void setUp(void) {
    memset(&res, 0, sizeof res);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    pb_free(&res);
}

static int cmp(const char *lhs, const char *rhs) {
    uint8_t lhs_bits[16];
    uint8_t rhs_bits[16];
    static const char k_chars[] = "01xz-m";
    for (size_t i = 0; lhs[i] != '\0'; i++) {
        lhs_bits[i] = (uint8_t)(strchr(k_chars, lhs[i]) - k_chars);
    }
    for (size_t i = 0; rhs[i] != '\0'; i++) {
        rhs_bits[i] = (uint8_t)(strchr(k_chars, rhs[i]) - k_chars);
    }
    return odin3_fsm_const_cmp(lhs_bits, (uint32_t)strlen(lhs), rhs_bits, (uint32_t)strlen(rhs));
}

typedef struct order_row {
    const char *lhs, *rhs;
    int sign; /* of cmp(lhs, rhs) */
} order_row;

/* Codes are written bit 0 first. */
static const order_row k_order[] = {
    {"0", "00", -1},   /* shorter first */
    {"111", "00", 1},  /* longer after, whatever the bits */
    {"01x", "01x", 0}, /* equal */
    {"00", "10", -1},  /* bit 0 decides first */
    {"01", "10", -1},  /* value 2 ("01", bit 0 = 0) sorts before value 1 */
    {"0", "1", -1},    {"1", "x", -1},  {"x", "z", -1}, {"z", "-", -1},
    {"-", "m", -1},    {"1-", "0m", 1}, {"m", "0", 1},
};

static void test_const_order(void) {
    for (size_t i = 0; i < sizeof k_order / sizeof k_order[0]; i++) {
        int got = cmp(k_order[i].lhs, k_order[i].rhs);
        int sign = (got > 0) - (got < 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_order[i].sign, sign, k_order[i].lhs);
    }
}

static void test_bit_chars(void) {
    static const char k_want[] = "01xz-m";
    for (int bit = 0; bit <= ODIN3_FSM_BIT_MARK; bit++) {
        TEST_ASSERT_EQUAL_CHAR(k_want[bit], odin3_fsm_bit_char((uint8_t)bit));
    }
    TEST_ASSERT_EQUAL_CHAR('?', odin3_fsm_bit_char(200));
    TEST_ASSERT_TRUE(odin3_fsm_bit_defined(ODIN3_FSM_BIT_1));
    TEST_ASSERT_FALSE(odin3_fsm_bit_defined(ODIN3_FSM_BIT_X));
}

/* A table of two 3-bit states and one transition (2 inputs, 1 output). */
static odin3_status fill(odin3_fsm_data *data) {
    odin3_status st = odin3_fsm_data_init(data);
    if (st != ODIN3_OK) {
        return st;
    }
    data->num_inputs = 2;
    data->num_outputs = 1;
    data->state_bits = 3;
    data->reset_state = 0;
    const uint8_t s0[3] = {0, 0, 0};
    const uint8_t s1[3] = {1, 0, 4};
    st = odin3_fsm_data_add_state(data, s0);
    if (st == ODIN3_OK) {
        st = odin3_fsm_data_add_state(data, s1);
    }
    odin3_fsm_trans tr = {0, 1, odin3_fsm_data_bits(data, 2), odin3_fsm_data_bits(data, 1)};
    if (st == ODIN3_OK && (tr.ctrl_in == NULL || tr.ctrl_out == NULL)) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        tr.ctrl_in[0] = ODIN3_FSM_BIT_1;
        tr.ctrl_in[1] = ODIN3_FSM_BIT_DC;
        tr.ctrl_out[0] = ODIN3_FSM_BIT_1;
        st = odin3_fsm_data_add_trans(data, &tr);
    }
    return st;
}

static void check_filled(const odin3_fsm_data *data) {
    TEST_ASSERT_EQUAL_UINT(2, data->states.len);
    TEST_ASSERT_EQUAL_UINT(1, data->trans.len);
    TEST_ASSERT_EQUAL_UINT8(4, odin3_fsm_data_state(data, 1)[2]);
    const odin3_fsm_trans *tr = odin3_fsm_data_trans(data, 0);
    TEST_ASSERT_EQUAL_INT(1, tr->state_out);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_FSM_BIT_DC, tr->ctrl_in[1]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_FSM_BIT_1, tr->ctrl_out[0]);
    TEST_ASSERT_EQUAL_INT(0, data->reset_state);
}

static void test_data_copy_and_free(void) {
    odin3_fsm_data src;
    odin3_fsm_data dst;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fill(&src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_copy(&src, &dst));
    odin3_fsm_data_free(&src);
    check_filled(&dst); /* the copy owns its arrays */
    odin3_fsm_data_free(&dst);
    odin3_fsm_data_free(&dst); /* safe twice */
    odin3_fsm_data_free(NULL);
}

/* One copy with the allocator failing after `after` successful allocations. */
static odin3_status copy_failing(long after) {
    odin3_fsm_data src;
    odin3_fsm_data dst;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fill(&src));
    odin3_util_set_alloc_fail_after(after);
    odin3_status st = odin3_fsm_data_copy(&src, &dst);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        check_filled(&dst);
    } else {
        TEST_ASSERT_NULL(dst.arena); /* a failed copy leaves nothing */
    }
    odin3_fsm_data_free(&dst);
    odin3_fsm_data_free(&src);
    return st;
}

/* Out of memory at every allocation of a copy: NO_MEMORY with nothing left, until it fits. */
static void test_data_copy_oom(void) {
    long failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 200; after++) {
        st = copy_failing(after);
        failures += st == ODIN3_OK ? 0 : 1;
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 4);
}

static void test_data_fill_oom(void) {
    for (long after = 0; after < 40; after++) {
        odin3_fsm_data data;
        odin3_util_set_alloc_fail_after(after);
        odin3_status st = fill(&data);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        odin3_fsm_data_free(&data);
    }
}

/* --- the index ----------------------------------------------------------------------------------
 */

static const char *const k_mod = "module \\sub\n"
                                 "  wire input 1 \\i\n"
                                 "  wire output 2 \\o\n"
                                 "end\n"
                                 "module \\m\n"
                                 "  wire input 1 \\clk\n"
                                 "  wire input 2 \\a\n"
                                 "  wire width 2 \\q\n"
                                 "  wire width 2 \\d\n"
                                 "  wire output 3 \\y\n"
                                 "  cell $dff \\f\n"
                                 "    parameter \\WIDTH 2\n"
                                 "    parameter \\CLK_POLARITY 1\n"
                                 "    connect \\CLK \\clk\n"
                                 "    connect \\D \\d\n"
                                 "    connect \\Q \\q\n"
                                 "  end\n"
                                 "  cell $eq \\e\n"
                                 "    parameter \\A_SIGNED 0\n"
                                 "    parameter \\B_SIGNED 0\n"
                                 "    parameter \\A_WIDTH 2\n"
                                 "    parameter \\B_WIDTH 2\n"
                                 "    parameter \\Y_WIDTH 1\n"
                                 "    connect \\A \\q\n"
                                 "    connect \\B 2'01\n"
                                 "    connect \\Y \\y\n"
                                 "  end\n"
                                 "  cell \\sub \\u\n"
                                 "    connect \\i \\a\n"
                                 "    connect \\o \\d [0]\n"
                                 "  end\n"
                                 "end\n";

static uint32_t wire_net(const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res.design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res.module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(res.module, wire, bit).v;
}

static bool has_ref(odin3_fsm_refs span, uint32_t flags, uint32_t port, const char *type_name) {
    for (uint32_t i = 0; i < span.count; i++) {
        const odin3_celltype_def *def = odin3_celltype_get(
            res.design, odin3_node_type(res.module, (odin3_node_id){span.refs[i].node}));
        if (span.refs[i].flags == flags && span.refs[i].port == port &&
            strcmp(def->name, type_name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_index_refs(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_mod, &res));
    odin3_fsm_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(res.design, res.module, &ix));
    uint32_t q_net = wire_net("q", 0);
    odin3_fsm_refs span = odin3_fsm_ix_refs(&ix, q_net);
    TEST_ASSERT_EQUAL_UINT(2, span.count);
    TEST_ASSERT_TRUE(has_ref(span, ODIN3_FSM_REF_OUT, ODIN3_FSM_P_DFF_Q, "$dff"));
    TEST_ASSERT_TRUE(has_ref(span, ODIN3_FSM_REF_IN, ODIN3_FSM_P_BIN_A, "$eq"));
    TEST_ASSERT_FALSE(odin3_fsm_ix_port_net(&ix, q_net));
    TEST_ASSERT_EQUAL_UINT32(q_net, odin3_fsm_ix_sig(&ix, q_net));
    odin3_fsm_index_free(&ix);
    odin3_fsm_index_free(&ix);
}

static void test_index_ports_and_unknown(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_mod, &res));
    odin3_fsm_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(res.design, res.module, &ix));
    uint32_t y_net = wire_net("y", 0);
    TEST_ASSERT_TRUE(odin3_fsm_ix_port_net(&ix, y_net));
    TEST_ASSERT_TRUE(
        has_ref(odin3_fsm_ix_refs(&ix, y_net), ODIN3_FSM_REF_OUT, ODIN3_FSM_P_BIN_Y, "$eq"));
    /* the port node's own pin is no reference */
    TEST_ASSERT_EQUAL_UINT(1, odin3_fsm_ix_refs(&ix, y_net).count);
    uint32_t a_net = wire_net("a", 0);
    odin3_fsm_refs span = odin3_fsm_ix_refs(&ix, a_net);
    TEST_ASSERT_EQUAL_UINT(1, span.count);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_FSM_REF_IN | ODIN3_FSM_REF_UNKNOWN, span.refs[0].flags);
    uint32_t d0 = wire_net("d", 0);
    span = odin3_fsm_ix_refs(&ix, d0);
    TEST_ASSERT_EQUAL_UINT(2, span.count);
    TEST_ASSERT_TRUE(has_ref(span, ODIN3_FSM_REF_IN, ODIN3_FSM_P_DFF_D, "$dff"));
    odin3_fsm_index_free(&ix);
}

static void test_index_constants(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_mod, &res));
    odin3_fsm_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(res.design, res.module, &ix));
    uint32_t q_net = wire_net("q", 0);
    odin3_fsm_refs span = odin3_fsm_ix_refs(&ix, q_net);
    odin3_node_id eq = {0};
    for (uint32_t i = 0; i < span.count; i++) {
        if (span.refs[i].flags == ODIN3_FSM_REF_IN) {
            eq.v = span.refs[i].node;
        }
    }
    odin3_pinslice b_port = odin3_node_port(res.module, eq, ODIN3_FSM_P_BIN_B);
    odin3_fsm_sig b0 = odin3_fsm_ix_pin_sig(&ix, b_port.first);
    odin3_fsm_sig b1 = odin3_fsm_ix_pin_sig(&ix, (odin3_pin_id){b_port.first.v + 1});
    TEST_ASSERT_TRUE(odin3_fsm_sig_is_const(b0) && odin3_fsm_sig_is_const(b1));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_FSM_BIT_1, odin3_fsm_sig_bit(b0));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_FSM_BIT_0, odin3_fsm_sig_bit(b1));
    TEST_ASSERT_TRUE(odin3_fsm_sig_defined(b0));
    /* a constant net has no references, and an unconnected pin reads x */
    TEST_ASSERT_EQUAL_UINT(0,
                           odin3_fsm_ix_refs(&ix, odin3_pin_net(res.module, b_port.first).v).count);
    TEST_ASSERT_EQUAL_UINT32(odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_X), odin3_fsm_ix_sig(&ix, 0));
    odin3_fsm_index_free(&ix);
}

static void test_index_whole_wire(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_mod, &res));
    odin3_fsm_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(res.design, res.module, &ix));
    /* the Q bus of the flip-flop as signal bits equals the wire's nets, in order */
    odin3_vec sigs;
    odin3_vec_init(&sigs, sizeof(odin3_fsm_sig));
    odin3_node_id dff = {0};
    odin3_fsm_refs span = odin3_fsm_ix_refs(&ix, wire_net("q", 0));
    for (uint32_t i = 0; i < span.count; i++) {
        if (span.refs[i].flags == ODIN3_FSM_REF_OUT) {
            dff.v = span.refs[i].node;
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ix_port_sigs(&ix, dff, ODIN3_FSM_P_DFF_Q, &sigs));
    TEST_ASSERT_EQUAL_UINT(2, sigs.len);
    TEST_ASSERT_EQUAL_UINT32(wire_net("q", 0), ((odin3_fsm_sig *)sigs.data)[0]);
    TEST_ASSERT_EQUAL_UINT32(wire_net("q", 1), ((odin3_fsm_sig *)sigs.data)[1]);
    odin3_vec_free(&sigs);
    odin3_fsm_index_free(&ix);
}

static void test_index_oom(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_mod, &res));
    long failures = 0;
    for (long after = 0; after < 40; after++) {
        odin3_fsm_index ix;
        odin3_util_set_alloc_fail_after(after);
        odin3_status st = odin3_fsm_index_build(res.design, res.module, &ix);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            odin3_fsm_index_free(&ix);
            break;
        }
        failures++;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(ix.off);
    }
    TEST_ASSERT_TRUE(failures >= 3);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_const_order);
    RUN_TEST(test_bit_chars);
    RUN_TEST(test_data_copy_and_free);
    RUN_TEST(test_data_copy_oom);
    RUN_TEST(test_data_fill_oom);
    RUN_TEST(test_index_refs);
    RUN_TEST(test_index_ports_and_unknown);
    RUN_TEST(test_index_constants);
    RUN_TEST(test_index_whole_wire);
    RUN_TEST(test_index_oom);
    return UNITY_END();
}
