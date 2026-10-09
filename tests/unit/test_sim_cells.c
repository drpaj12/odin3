/*
 * test_sim_cells.c — simulate hooks of the bit-level cell types, driven through hand-built views.
 */
#include "ir/celltype.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { MAX_PORTS = 4, MAX_BITS = 8, N_INIT = 4, N_COMBOS = 8 };

/* A cell under test: every pin bit has its own slot in values, ports in definition order. */
typedef struct harness {
    const odin3_celltype_def *def;
    uint8_t values[MAX_BITS];
    uint32_t idx[MAX_BITS];
    odin3_sim_span spans[MAX_PORTS];
    uint8_t state[1];
    uint32_t n_bits;
    odin3_sim_cell cell;
} harness;

void setUp(void) {
}
void tearDown(void) {
}

static const odin3_celltype_def *def_named(const char *name) {
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        if (strcmp(odin3_builtin_celltypes[i]->name, name) == 0) {
            return odin3_builtin_celltypes[i];
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static bool is_sequential(const odin3_celltype_def *def) {
    return (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0;
}

/* Lays out the cell named name; a vector port (only $sop's A here) is vec_width bits wide. */
static void setup(harness *hns, const char *name, const odin3_value *params, uint32_t vec_width) {
    memset(hns, 0, sizeof *hns);
    hns->def = def_named(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(hns->def->simulate, name);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_PORTS, hns->def->n_ports);
    for (uint32_t i = 0; i < hns->def->n_ports; i++) {
        uint32_t width = hns->def->ports[i].scalar ? 1 : vec_width;
        hns->spans[i] = (odin3_sim_span){&hns->idx[hns->n_bits], width};
        for (uint32_t j = 0; j < width; j++) {
            hns->idx[hns->n_bits] = hns->n_bits;
            hns->n_bits++;
        }
    }
    bool seq = is_sequential(hns->def);
    hns->cell = (odin3_sim_cell){
        hns->values, hns->spans,    hns->def->n_ports, params, seq ? hns->state : NULL,
        seq ? 1 : 0, ODIN3_SIM_COMB};
}

/* Sets the input bits from bit k of combo (all bits before the single output bit). */
static void drive(harness *hns, uint32_t combo) {
    for (uint32_t k = 0; k + 1 < hns->n_bits; k++) {
        hns->values[k] = (uint8_t)((combo >> k) & 1U);
    }
}

/* The single output bit (last port). */
static uint8_t output(const harness *hns) {
    return hns->values[hns->n_bits - 1];
}

static void preset_output(harness *hns, uint8_t val) {
    hns->values[hns->n_bits - 1] = val;
}

static void fire(harness *hns, odin3_sim_event event) {
    hns->cell.event = event;
    hns->def->simulate(&hns->cell);
}

/*
 * Evaluates the combinational cell on input combos 0..n_combos-1, each with the output preset to
 * the opposite of want[combo] so that the hook must write it.
 */
static void expect_comb(harness *hns, const uint8_t *want, uint32_t n_combos) {
    for (uint32_t combo = 0; combo < n_combos; combo++) {
        drive(hns, combo);
        preset_output(hns, (uint8_t)(want[combo] == 0 ? 1 : 0));
        fire(hns, ODIN3_SIM_COMB);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[combo], output(hns), hns->def->name);
    }
}

/* --- gates ----------------------------------------------------------------------------------- */

typedef struct gate_case {
    const char *name;
    uint32_t n_combos;
    uint8_t want[N_COMBOS]; /* by input combo: A is bit 0, B bit 1, S bit 2 */
} gate_case;

static void test_gate_truth_tables(void) {
    static const gate_case k_cases[] = {
        {"$_BUF_", 2, {0, 1}},
        {"$_NOT_", 2, {1, 0}},
        {"$_AND_", 4, {0, 0, 0, 1}},
        {"$_OR_", 4, {0, 1, 1, 1}},
        {"$_XOR_", 4, {0, 1, 1, 0}},
        {"$_NAND_", 4, {1, 1, 1, 0}},
        {"$_NOR_", 4, {1, 0, 0, 0}},
        {"$_XNOR_", 4, {1, 0, 0, 1}},
        /* Y = S ? B : A */
        {"$_MUX_", 8, {0, 1, 0, 1, 0, 0, 1, 1}},
    };
    for (uint32_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        harness hns;
        setup(&hns, k_cases[i].name, NULL, 1);
        TEST_ASSERT_FALSE(is_sequential(hns.def));
        expect_comb(&hns, k_cases[i].want, k_cases[i].n_combos);
    }
}

static void test_constants(void) {
    static const struct {
        const char *name;
        uint8_t want;
    } k_cases[] = {{"$_CONST0_", 0}, {"$_CONST1_", 1}, {"$_CONSTX_", 0}, {"$_CONSTZ_", 0}};
    for (uint32_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        harness hns;
        setup(&hns, k_cases[i].name, NULL, 1);
        TEST_ASSERT_FALSE(is_sequential(hns.def));
        expect_comb(&hns, &k_cases[i].want, 1);
    }
}

/* --- $sop ------------------------------------------------------------------------------------ */

/* $sop over rows (each width input chars then the output char) on every input combo. */
static void expect_sop(const char *rows, uint32_t width, const uint8_t *want) {
    uint32_t len = (uint32_t)strlen(rows);
    const odin3_value params[2] = {
        odin3_value_int(width),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)rows, len, 0, width},
    };
    TEST_ASSERT_EQUAL(ODIN3_OK, def_named("$sop")->verify(params));
    harness hns;
    setup(&hns, "$sop", params, width);
    expect_comb(&hns, want, 1U << width);
}

static void test_sop_on_set(void) {
    /* Y = A0&A1 | !A0 */
    static const uint8_t k_want[] = {1, 0, 1, 1};
    expect_sop("111"
               "0-1",
               2, k_want);
}

static void test_sop_off_set(void) {
    /* OFF-set: Y = !(A0&A1 | !A0&!A1) = A0 ^ A1 */
    static const uint8_t k_want[] = {0, 1, 1, 0};
    expect_sop("110"
               "000",
               2, k_want);
    /* A single OFF row with a don't care: Y = !(!A1) = A1 */
    static const uint8_t k_want_dc[] = {0, 0, 1, 1};
    expect_sop("-00", 2, k_want_dc);
}

static void test_sop_dont_care(void) {
    /* Y = A1 over three inputs; A0 and A2 are don't cares */
    static const uint8_t k_want[] = {0, 0, 1, 1, 0, 0, 1, 1};
    expect_sop("-1-1", 3, k_want);
    /* An all-don't-care row is a tautology */
    static const uint8_t k_ones[] = {1, 1, 1, 1};
    expect_sop("--1", 2, k_ones);
    static const uint8_t k_zeros[] = {0, 0, 0, 0};
    expect_sop("--0", 2, k_zeros);
}

static void test_sop_empty_and_zero_input(void) {
    static const uint8_t k_zero[] = {0, 0, 0, 0};
    static const uint8_t k_one[] = {1};
    expect_sop("", 2, k_zero); /* no rows: constant 0 */
    expect_sop("", 0, k_zero);
    expect_sop("1", 0, k_one);
    expect_sop("0", 0, k_zero);
}

static void test_sop_mixed_rows(void) {
    /* Not legal BLIF: ON rows decide, OFF rows are ignored when any ON row exists */
    static const uint8_t k_want[] = {0, 0, 0, 1};
    expect_sop("111"
               "000",
               2, k_want);
    /* Zero inputs, as const_value: 1 when any row says 1 */
    static const uint8_t k_one[] = {1};
    expect_sop("01", 0, k_one);
}

/* --- latches and flip-flops ------------------------------------------------------------------ */

typedef struct seq_case {
    const char *name;
    uint32_t flags; /* the expected simulation flags */
} seq_case;

static const seq_case k_seq[] = {
    {"$_DFF_P_", ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0},
    {"$_DFF_N_", ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0},
    {"$_FF_", ODIN3_CT_SEQ_EDGE},
    {"$_DLATCH_P_", ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0},
    {"$_DLATCH_N_", ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0},
};

enum { SIM_FLAGS = ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0 };

static void test_sequential_flags(void) {
    for (uint32_t i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(k_seq[i].flags, def_named(k_seq[i].name)->flags & SIM_FLAGS,
                                        k_seq[i].name);
    }
    static const char *const k_comb[] = {"$_BUF_",    "$_MUX_",    "$_CONST0_", "$_CONST1_",
                                         "$_CONSTX_", "$_CONSTZ_", "$sop"};
    for (uint32_t i = 0; i < sizeof k_comb / sizeof k_comb[0]; i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(0, def_named(k_comb[i])->flags & SIM_FLAGS, k_comb[i]);
    }
}

static void test_init_mapping(void) {
    static const uint8_t k_want[N_INIT] = {0, 1, 0, 0};
    for (uint32_t i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        for (int64_t init = 0; init < N_INIT; init++) {
            const odin3_value params[1] = {odin3_value_int(init)};
            TEST_ASSERT_EQUAL_UINT8(k_want[init], odin3_sim_init_bit(&params[0]));
            harness hns;
            setup(&hns, k_seq[i].name, params, 1);
            hns.state[0] = (uint8_t)(k_want[init] == 0 ? 1 : 0); /* the hook must write it */
            preset_output(&hns, 1);
            fire(&hns, ODIN3_SIM_INIT);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(k_want[init], hns.state[0], k_seq[i].name);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, output(&hns), "INIT writes no output");
        }
    }
    const odin3_value str = {ODIN3_VAL_STRING, 1, NULL, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8(0, odin3_sim_init_bit(&str));
}

/* Port order: C D Q for the flip-flops, D Q for $_FF_. */
static void set_d(harness *hns, uint8_t val) {
    hns->values[hns->n_bits - 2] = val;
}

/* An edge-triggered cell: state follows D only on its edge; COMB copies state to Q. */
static void check_edge(const char *name, odin3_sim_event edge, odin3_sim_event other) {
    const odin3_value params[1] = {odin3_value_int(0)};
    harness hns;
    setup(&hns, name, params, 1);
    fire(&hns, ODIN3_SIM_INIT);
    set_d(&hns, 1);
    preset_output(&hns, 1);
    fire(&hns, ODIN3_SIM_COMB);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, output(&hns), "COMB: Q = state, not D");
    TEST_ASSERT_EQUAL_UINT8(0, hns.state[0]);
    fire(&hns, other);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, hns.state[0], "the other edge leaves the state");
    fire(&hns, edge);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hns.state[0], "its edge samples D");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, output(&hns), "an edge writes no output");
    set_d(&hns, 0);
    fire(&hns, ODIN3_SIM_COMB);
    TEST_ASSERT_EQUAL_UINT8(1, output(&hns));
    TEST_ASSERT_EQUAL_UINT8(1, hns.state[0]);
    fire(&hns, edge);
    fire(&hns, ODIN3_SIM_COMB);
    TEST_ASSERT_EQUAL_UINT8(0, output(&hns));
}

static void test_flip_flop_edges(void) {
    check_edge("$_DFF_P_", ODIN3_SIM_POSEDGE, ODIN3_SIM_NEGEDGE);
    check_edge("$_DFF_N_", ODIN3_SIM_NEGEDGE, ODIN3_SIM_POSEDGE);
    check_edge("$_FF_", ODIN3_SIM_POSEDGE, ODIN3_SIM_NEGEDGE);
}

/* Inputs of a latch, port order E D Q. */
typedef struct latch_in {
    uint8_t enable;
    uint8_t data;
} latch_in;

static void latch_step(harness *hns, latch_in inputs) {
    hns->values[0] = inputs.enable;
    hns->values[1] = inputs.data;
    fire(hns, ODIN3_SIM_COMB);
}

static void check_latch(const char *name, uint8_t active) {
    uint8_t inactive = (uint8_t)(active == 0 ? 1 : 0);
    const odin3_value params[1] = {odin3_value_int(1)};
    harness hns;
    setup(&hns, name, params, 1);
    fire(&hns, ODIN3_SIM_INIT);
    latch_step(&hns, (latch_in){inactive, 0});
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, output(&hns), "disabled: Q holds INIT");
    latch_step(&hns, (latch_in){active, 0});
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, output(&hns), "transparent: Q = D");
    latch_step(&hns, (latch_in){active, 1});
    TEST_ASSERT_EQUAL_UINT8(1, output(&hns));
    TEST_ASSERT_EQUAL_UINT8(1, hns.state[0]);
    latch_step(&hns, (latch_in){inactive, 0});
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, output(&hns), "disabled: Q holds the last D");
    TEST_ASSERT_EQUAL_UINT8(1, hns.state[0]);
    fire(&hns, ODIN3_SIM_POSEDGE);
    fire(&hns, ODIN3_SIM_NEGEDGE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hns.state[0], "edges leave a latch alone");
    latch_step(&hns, (latch_in){active, 0});
    TEST_ASSERT_EQUAL_UINT8(0, output(&hns));
    TEST_ASSERT_EQUAL_UINT8(0, hns.state[0]);
}

static void test_latch_transparency(void) {
    check_latch("$_DLATCH_P_", 1);
    check_latch("$_DLATCH_N_", 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gate_truth_tables);
    RUN_TEST(test_constants);
    RUN_TEST(test_sop_on_set);
    RUN_TEST(test_sop_off_set);
    RUN_TEST(test_sop_dont_care);
    RUN_TEST(test_sop_empty_and_zero_input);
    RUN_TEST(test_sop_mixed_rows);
    RUN_TEST(test_sequential_flags);
    RUN_TEST(test_init_mapping);
    RUN_TEST(test_flip_flop_edges);
    RUN_TEST(test_latch_transparency);
    return UNITY_END();
}
