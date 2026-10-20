/*
 * test_fsm_recode.c — fsm_recode (4.8 plan Task 8; spec R-1 to R-5; Yosys 0.55 fsm_recode.cc):
 * the auto rule at the 31/32/33 boundary, the -encoding option, none and user, an unknown
 * string, the numbering of one-hot and binary codes with and without a reset state, the packed
 * binary case and the 6-to-5-bit case of mcml, and the log.
 */
#include "fsm_table.h"
#include "fsm_test.h"
#include "util/alloc.h"

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    ft_close();
}

/* A module just to name the wire in messages. */
static void open_design(void) {
    ft_read_proc("module m(input clk, input rst, input go, output o);\n"
                 "  reg [1:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0; else if (go) st <= st + 1;\n"
                 "  assign o = st == 3;\nendmodule\n");
}

/* The wire `st` of m, for the instance's name in log lines. */
static odin3_wire_id st_wire(void) {
    odin3_module *module = ft_module("m");
    odin3_wire_id wire = odin3_module_find_wire(module, ft_intern("st"));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return wire;
}

/* The size of a table: code width, number of states, reset state. */
typedef struct tbl_shape {
    int32_t bits;
    uint32_t count;
    int32_t reset;
} tbl_shape;

/* An instance with `count` states of `bits`-bit binary codes. */
static void table(odin3_fsm_inst *inst, tbl_shape ts) {
    int32_t bits = ts.bits;
    make(inst, (shape){bits, 0, 0, ts.reset}, NULL, 0);
    for (uint32_t i = 0; i < ts.count; i++) {
        uint8_t code[40];
        for (int32_t k = 0; k < bits; k++) {
            code[k] = (uint8_t)(((i * 7U + 3U) >> k) & 1U); /* some codes, not the indices */
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_data_add_state(&inst->data, code));
    }
    inst->wire = st_wire();
}

typedef struct recode_case {
    const char *attr;   /* the fsm_encoding string */
    const char *option; /* -encoding, NULL for none */
    long fail_after;    /* allocations that succeed before one fails, -1 for none */
} recode_case;

/* Recodes the instance and returns the status; fm carries the option. */
static odin3_status recode(odin3_fsm_inst *inst, recode_case rc) {
    odin3_fsm_mod fm;
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.encoding = rc.option;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_mod_open(&fm, ft.design, ft_module("m"), &opts));
    (void)snprintf(inst->encoding, sizeof inst->encoding, "%s", rc.attr);
    ft.n_msgs = 0;
    odin3_util_set_alloc_fail_after(rc.fail_after);
    odin3_status st = odin3_fsm_recode(&fm, inst);
    odin3_util_set_alloc_fail_after(-1);
    odin3_fsm_mod_close(&fm);
    return st;
}

static const char *code_text(const odin3_fsm_inst *inst, size_t idx) {
    static char text[64];
    int32_t bits = inst->data.state_bits;
    for (int32_t k = 0; k < bits; k++) {
        text[k] = odin3_fsm_bit_char(odin3_fsm_data_state(&inst->data, idx)[k]);
    }
    text[bits] = '\0';
    return text;
}

/* --- the auto rule ---------------------------------------------------------------------------- */

static void check_auto(uint32_t count, const char *want_encoding, int32_t want_bits) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){6, count, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"auto", NULL, -1}));
    TEST_ASSERT_EQUAL_STRING(want_encoding, inst.final_encoding);
    TEST_ASSERT_EQUAL_INT(want_bits, inst.data.state_bits);
    done(&inst);
}

static void test_auto_two_states_is_one_hot(void) {
    check_auto(2, "one-hot", 2);
}

static void test_auto_fifteen_states_is_one_hot(void) {
    check_auto(15, "one-hot", 15);
}

static void test_auto_thirty_one_states_is_one_hot(void) {
    check_auto(31, "one-hot", 31);
}

static void test_auto_thirty_two_states_is_binary(void) {
    check_auto(32, "binary", 5); /* the mcml case: 6 bits become 5 */
}

static void test_auto_thirty_three_states_is_binary(void) {
    check_auto(33, "binary", 6);
}

static void test_the_log_names_the_choice(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){3, 3, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"auto", NULL, -1}));
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_INFO, "fsm: 'm.st': mapping auto encoding to 'one-hot'"));
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_INFO, "fsm: recoding 'm.st' with one-hot encoding (3 states, 3 bits)"),
        ft_log());
    done(&inst);
}

/* --- -encoding -------------------------------------------------------------------------------- */

static void test_option_one_hot_forces_auto_at_40_states(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){6, 40, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"auto", "one-hot", -1}));
    TEST_ASSERT_EQUAL_INT(40, inst.data.state_bits);
    done(&inst);
}

static void test_option_binary_forces_auto_at_5_states(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){5, 5, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"auto", "binary", -1}));
    TEST_ASSERT_EQUAL_INT(3, inst.data.state_bits);
    done(&inst);
}

static void test_option_does_not_override_an_explicit_attribute(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){6, 40, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"binary", "one-hot", -1}));
    TEST_ASSERT_EQUAL_STRING("binary", inst.final_encoding);
    TEST_ASSERT_EQUAL_INT(6, inst.data.state_bits); /* 40 states in 6 bits: already packed */
    done(&inst);
}

/* --- none, user, unknown ---------------------------------------------------------------------- */

static void check_untouched(const char *attr) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){4, 5, 0});
    char before[16][8];
    for (size_t i = 0; i < 5; i++) {
        (void)snprintf(before[i], sizeof before[i], "%s", code_text(&inst, i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){attr, "one-hot", -1}));
    TEST_ASSERT_EQUAL_INT(4, inst.data.state_bits);
    for (size_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_STRING(before[i], code_text(&inst, i));
    }
    TEST_ASSERT_EQUAL_STRING(attr, inst.final_encoding);
    done(&inst);
}

static void test_none_is_untouched(void) {
    check_untouched("none");
}

static void test_user_is_untouched(void) {
    check_untouched("user");
}

static void test_unknown_encoding_uses_auto(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){4, 5, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"gray", NULL, -1}));
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_INFO, "fsm: 'm.st': unknown encoding 'gray', using auto"), ft_log());
    TEST_ASSERT_EQUAL_STRING("one-hot", inst.final_encoding);
    TEST_ASSERT_EQUAL_INT(5, inst.data.state_bits);
    done(&inst);
}

/* --- the numbering ---------------------------------------------------------------------------- */

static void test_one_hot_with_a_reset_state(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){3, 4, 2}); /* state 2 resets */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"one-hot", NULL, -1}));
    /* the reset state is bit 0, the others take bits 1, 2, 3 in table order; bit 0 first */
    TEST_ASSERT_EQUAL_STRING("-1--", code_text(&inst, 0));
    TEST_ASSERT_EQUAL_STRING("--1-", code_text(&inst, 1));
    TEST_ASSERT_EQUAL_STRING("1---", code_text(&inst, 2));
    TEST_ASSERT_EQUAL_STRING("---1", code_text(&inst, 3));
    done(&inst);
}

static void test_one_hot_without_a_reset_state(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){3, 3, -1});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"one-hot", NULL, -1}));
    TEST_ASSERT_EQUAL_STRING("1--", code_text(&inst, 0));
    TEST_ASSERT_EQUAL_STRING("-1-", code_text(&inst, 1));
    TEST_ASSERT_EQUAL_STRING("--1", code_text(&inst, 2));
    done(&inst);
}

static void test_binary_with_a_reset_state(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){5, 5, 3});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"binary", NULL, -1}));
    TEST_ASSERT_EQUAL_INT(3, inst.data.state_bits);
    /* indices: state 3 -> 0; 0 -> 1; 1 -> 2; 2 -> 3; 4 -> 4; written bit 0 first */
    TEST_ASSERT_EQUAL_STRING("100", code_text(&inst, 0));
    TEST_ASSERT_EQUAL_STRING("010", code_text(&inst, 1));
    TEST_ASSERT_EQUAL_STRING("110", code_text(&inst, 2));
    TEST_ASSERT_EQUAL_STRING("000", code_text(&inst, 3));
    TEST_ASSERT_EQUAL_STRING("001", code_text(&inst, 4));
    done(&inst);
}

static void test_binary_without_a_reset_state(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){4, 3, -1});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"binary", NULL, -1}));
    TEST_ASSERT_EQUAL_STRING("00", code_text(&inst, 0));
    TEST_ASSERT_EQUAL_STRING("10", code_text(&inst, 1));
    TEST_ASSERT_EQUAL_STRING("01", code_text(&inst, 2));
    done(&inst);
}

static void test_already_packed_binary_keeps_the_codes(void) {
    open_design();
    odin3_fsm_inst inst;
    table(&inst, (tbl_shape){5, 32, 0}); /* 32 states in 5 bits */
    char first[16];
    (void)snprintf(first, sizeof first, "%s", code_text(&inst, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, recode(&inst, (recode_case){"binary", NULL, -1}));
    TEST_ASSERT_EQUAL_STRING(first, code_text(&inst, 1));
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_INFO,
                            "fsm: 'm.st': existing encoding is already a packed binary encoding"));
    done(&inst);
}

static void test_ceil_log2(void) {
    static const int32_t k_in[] = {0, 1, 2, 3, 4, 5, 31, 32, 33, 1000};
    static const int32_t k_out[] = {0, 0, 1, 2, 2, 3, 5, 5, 6, 10};
    for (size_t i = 0; i < sizeof k_in / sizeof k_in[0]; i++) {
        TEST_ASSERT_EQUAL_INT(k_out[i], odin3_fsm_ceil_log2(k_in[i]));
    }
}

static void test_recode_oom(void) {
    open_design();
    long failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 100; after++) {
        odin3_fsm_inst inst;
        table(&inst, (tbl_shape){9, 300, 0}); /* 90,000 code bytes: the arena needs a new chunk */
        st = recode(&inst, (recode_case){"one-hot", NULL, after});
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(9, inst.data.state_bits); /* unrecoded, and valid */
            failures++;
        }
        done(&inst);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_auto_two_states_is_one_hot);
    RUN_TEST(test_auto_fifteen_states_is_one_hot);
    RUN_TEST(test_auto_thirty_one_states_is_one_hot);
    RUN_TEST(test_auto_thirty_two_states_is_binary);
    RUN_TEST(test_auto_thirty_three_states_is_binary);
    RUN_TEST(test_the_log_names_the_choice);
    RUN_TEST(test_option_one_hot_forces_auto_at_40_states);
    RUN_TEST(test_option_binary_forces_auto_at_5_states);
    RUN_TEST(test_option_does_not_override_an_explicit_attribute);
    RUN_TEST(test_none_is_untouched);
    RUN_TEST(test_user_is_untouched);
    RUN_TEST(test_unknown_encoding_uses_auto);
    RUN_TEST(test_one_hot_with_a_reset_state);
    RUN_TEST(test_one_hot_without_a_reset_state);
    RUN_TEST(test_binary_with_a_reset_state);
    RUN_TEST(test_binary_without_a_reset_state);
    RUN_TEST(test_already_packed_binary_keeps_the_codes);
    RUN_TEST(test_ceil_log2);
    RUN_TEST(test_recode_oom);
    return UNITY_END();
}
