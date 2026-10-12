/*
 * test_lut_cell.c — the $lut cell (3D spec §2, AB-2; docs/IR.md "LUT cells"): its registry entry
 * (ruling I5), verify, const_value, simulate, the table helpers of ir/lut.h, the engine word layout
 * (I10), and its JSON and RTLIL forms. The module of json_form and rtlil_form is also written to
 * ODIN3_LUT_OUT/lut.json and lut.il, the inputs of CTests lut_json_yosys and lut_rtlil_yosys.
 */
#include "backends/blif/writer.h"
#include "backends/dot/writer.h"
#include "backends/json/writer.h"
#include "backends/rtlil/writer.h"
#include "backends/verilog/writer.h"
#include "ir/cells/cells.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_LINES = 8,
    LOG_TEXT = 256,
    PATH_MAX_LEN = 512,
    TEXT_MAX = 16384,
    MAX_TABLE = 1 << ODIN3_LUT_MAX_WIDTH,
    RANDOM_TABLES = 1000,
    RANDOM_INPUTS = 64,
    EXHAUSTIVE_MAX = 2, /* every table for WIDTH 1..2 */
    ALL_INPUTS_MAX = 6, /* every input for WIDTH up to 6 */
    ARENA_CHUNK = 64,   /* smaller than a wide table: the table takes a fresh chunk */
    OOM_LIMIT = 1000,
    SEED = 0x3D1
};

static odin3_design *design;
static uint32_t log_count;
static char log_lines[LOG_LINES][LOG_TEXT];
static char file_buf[TEXT_MAX];
static uint8_t table[MAX_TABLE];

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    if (log_count < LOG_LINES) {
        int len = snprintf(log_lines[log_count], LOG_TEXT, "%s", msg);
        TEST_ASSERT_TRUE(len >= 0 && len < LOG_TEXT);
    }
    log_count++;
}

void setUp(void) {
    log_count = 0;
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- building ------------------------------------------------------------------------------ */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id lut_type(void) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$lut"), &id));
    return id;
}

static odin3_module *new_module(const char *name, odin3_module_id *mid) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, mid));
    return odin3_module_get(design, *mid);
}

/* A module port of width bits; its nets, LSB first, into nets. */
static void add_port(odin3_module *mod, const char *name, odin3_dir dir, uint32_t width,
                     odin3_net_id *nets) {
    odin3_port_spec spec = {intern(name),
                            dir,
                            width,
                            width == 1 && dir == ODIN3_DIR_OUT,
                            (odin3_prov_id){0},
                            false,
                            0,
                            0,
                            false};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        nets[i] = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + i});
    }
}

static odin3_value bits_value(const uint8_t *bits, uint32_t len) {
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = bits, .len = len};
    return val;
}

/* LUT bytes (LSB first) from a string written MSB first, as Verilog's N'b... */
static uint32_t lut_from_msb(const char *msb_first, uint8_t *bits) {
    uint32_t len = (uint32_t)strlen(msb_first);
    for (uint32_t i = 0; i < len; i++) {
        bits[i] = (uint8_t)(msb_first[len - 1 - i] == '1' ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return len;
}

/* Module m: input a[width-1:0], output y, one $lut of the given table (MSB-first text). */
static odin3_module_id build_lut_module(uint32_t width, const char *msb_first) {
    odin3_module_id mid = {0};
    odin3_module *mod = new_module("m", &mid);
    odin3_net_id a_nets[ODIN3_LUT_MAX_WIDTH];
    odin3_net_id y_net = {0};
    add_port(mod, "a", ODIN3_DIR_IN, width, a_nets);
    add_port(mod, "y", ODIN3_DIR_OUT, 1, &y_net);
    uint32_t len = lut_from_msb(msb_first, table);
    odin3_value params[2] = {odin3_value_int(width), bits_value(table, len)};
    odin3_netvec ports[2] = {{a_nets, width}, {&y_net, 1}};
    odin3_node_spec spec = {lut_type(), intern("u"), (odin3_prov_id){0}, params, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL));
    return mid;
}

static void check_full_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

/* --- registry (ruling I5) ------------------------------------------------------------------- */

static void test_registry_lookup(void) {
    uint32_t index = odin3_lut_builtin_index();
    TEST_ASSERT_LESS_THAN_UINT32(odin3_builtin_celltype_count, index);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_lut, odin3_builtin_celltypes[index]);
    uint32_t occurrences = 0;
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        occurrences += odin3_builtin_celltypes[i] == &odin3_cell_lut ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT32(1, occurrences);
    odin3_celltype_id id = lut_type();
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_lut, odin3_celltype_get(design, id));
    /* a design's slot 0 is reserved, so built-in index b is cell-type ID b + 1 */
    TEST_ASSERT_EQUAL_UINT32(index + 1, id.v);
    TEST_ASSERT_EQUAL_STRING("$lut", odin3_cell_lut.name);
    TEST_ASSERT_EQUAL(ODIN3_GRAN_BIT, odin3_cell_lut.gran);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_cell_lut.flags);
    TEST_ASSERT_NULL(odin3_cell_lut.sim_scratch_bytes);
}

/* --- defaults ------------------------------------------------------------------------------ */

static bool sim_y(odin3_sim *sim, bool a_val) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_set_input(sim, (odin3_sim_bit){0, 0}, a_val));
    odin3_sim_cycle(sim);
    bool out = !a_val;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){0, 0}, &out));
    return out;
}

static void test_defaults_identity(void) {
    const odin3_celltype_def *def = &odin3_cell_lut;
    TEST_ASSERT_EQUAL_UINT32(2, def->n_params);
    TEST_ASSERT_EQUAL_STRING("WIDTH", def->params[ODIN3_LUT_P_WIDTH].name);
    TEST_ASSERT_EQUAL_STRING("LUT", def->params[ODIN3_LUT_P_LUT].name);
    TEST_ASSERT_EQUAL_STRING("A", def->ports[ODIN3_LUT_PORT_A].name);
    TEST_ASSERT_EQUAL_STRING("Y", def->ports[ODIN3_LUT_PORT_Y].name);
    odin3_module_id mid = {0};
    odin3_module *mod = new_module("m", &mid);
    odin3_net_id a_net = {0};
    odin3_net_id y_net = {0};
    add_port(mod, "a", ODIN3_DIR_IN, 1, &a_net);
    add_port(mod, "y", ODIN3_DIR_OUT, 1, &y_net);
    odin3_node_spec spec = {lut_type(), 0, (odin3_prov_id){0}, NULL, 2};
    odin3_netvec ports[2] = {{&a_net, 1}, {&y_net, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    const odin3_value *width = odin3_node_param(mod, node, ODIN3_LUT_P_WIDTH);
    const odin3_value *lut = odin3_node_param(mod, node, ODIN3_LUT_P_LUT);
    TEST_ASSERT_EQUAL(ODIN3_VAL_INT, width->kind);
    TEST_ASSERT_EQUAL_INT64(1, width->i);
    TEST_ASSERT_EQUAL(ODIN3_VAL_BITS, lut->kind);
    TEST_ASSERT_EQUAL_UINT32(2, lut->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, lut->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, lut->bits[1]);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_node_pins(mod, node).count);
    check_full_ok();
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_build(design, mid, &sim));
    TEST_ASSERT_FALSE(sim_y(sim, false));
    TEST_ASSERT_TRUE(sim_y(sim, true));
    TEST_ASSERT_FALSE(sim_y(sim, false));
    odin3_sim_destroy(sim);
}

/* Y of the built simulator for the input index `input` (a[j] = bit j of it). */
static bool sim_y_wide(odin3_sim *sim, uint32_t input) {
    uint32_t width = odin3_sim_input_width(sim, 0);
    for (uint32_t j = 0; j < width; j++) {
        odin3_status st = odin3_sim_set_input(sim, (odin3_sim_bit){0, j}, ((input >> j) & 1U) != 0);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    }
    odin3_sim_cycle(sim);
    bool out = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){0, 0}, &out));
    return out;
}

/* A WIDTH-6 $lut through odin3_sim_build: every input gives LUT[i] (review M4). */
static void test_sim_build_width6(void) {
    char msb_first[65];
    for (uint32_t i = 0; i < 64; i++) {
        msb_first[63 - i] = (char)((i * 7U + i / 5U) % 3U == 0 ? '1' : '0'); /* LUT[i] */
    }
    msb_first[64] = '\0';
    odin3_module_id mid = build_lut_module(6, msb_first);
    check_full_ok();
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_build(design, mid, &sim));
    TEST_ASSERT_EQUAL_UINT32(6, odin3_sim_input_width(sim, 0));
    for (uint32_t input = 0; input < 64; input++) {
        bool want = msb_first[63 - input] == '1';
        TEST_ASSERT_EQUAL_MESSAGE(want, sim_y_wide(sim, input), "Y != LUT[i]");
    }
    odin3_sim_destroy(sim);
}

/* --- verify -------------------------------------------------------------------------------- */

/* A $lut with params is refused: INVALID_ARG, no node, and exactly the two lines expected. */
static void expect_refused(const odin3_value *params, const char *first_line) {
    odin3_module *mod = odin3_module_get(design, (odin3_module_id){1});
    odin3_node_spec spec = {lut_type(), 0, (odin3_prov_id){0}, params, 2};
    uint32_t before = odin3_module_node_end(mod);
    odin3_node_id node = {0};
    log_count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(mod, &spec, &node));
    TEST_ASSERT_EQUAL_UINT32(0, node.v);
    TEST_ASSERT_EQUAL_UINT32(before, odin3_module_node_end(mod));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, log_count, first_line);
    TEST_ASSERT_EQUAL_STRING(first_line, log_lines[0]);
    TEST_ASSERT_EQUAL_STRING("node_create: '$lut' rejects these parameters", log_lines[1]);
}

static void test_verify_refusals(void) {
    odin3_module_id mid = {0};
    (void)new_module("m", &mid);
    TEST_ASSERT_EQUAL_UINT32(1, mid.v);
    static const uint8_t k_two[2] = {ODIN3_BIT_0, ODIN3_BIT_1};
    static const uint8_t k_three[3] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_0};
    static const uint8_t k_x[4] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_0};
    static const uint8_t k_z[4] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_Z, ODIN3_BIT_0};
    static const uint8_t k_z_top[4] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_Z};
    odin3_value params[2] = {odin3_value_int(0), bits_value(k_two, 2)};
    expect_refused(params, "cell parameter WIDTH must be an int in 1..16");
    params[0] = odin3_value_int(ODIN3_LUT_MAX_WIDTH + 1);
    expect_refused(params, "cell parameter WIDTH must be an int in 1..16");
    params[0] = odin3_value_int(2);
    params[1] = bits_value(k_three, 3);
    expect_refused(params, "cell parameter LUT must be 4 bits");
    params[1] = bits_value(k_x, 4);
    expect_refused(params, "$lut: LUT bit 2 is x; only 0 and 1 are allowed");
    params[1] = bits_value(k_z, 4);
    expect_refused(params, "$lut: LUT bit 2 is z; only 0 and 1 are allowed");
    params[1] = bits_value(k_z_top, 4); /* the last bit is checked too (review M1) */
    expect_refused(params, "$lut: LUT bit 3 is z; only 0 and 1 are allowed");
    check_full_ok();
}

/* --- const_value --------------------------------------------------------------------------- */

static void test_const_value(void) {
    uint8_t bits[8];
    odin3_value params[2] = {odin3_value_int(3), bits_value(bits, 8)};
    memset(bits, ODIN3_BIT_0, sizeof bits);
    TEST_ASSERT_EQUAL(ODIN3_CONST_0, odin3_cell_lut.const_value(params));
    memset(bits, ODIN3_BIT_1, sizeof bits);
    TEST_ASSERT_EQUAL(ODIN3_CONST_1, odin3_cell_lut.const_value(params));
    for (uint32_t one = 0; one < sizeof bits; one++) {
        memset(bits, ODIN3_BIT_0, sizeof bits);
        bits[one] = ODIN3_BIT_1;
        TEST_ASSERT_EQUAL(ODIN3_CONST_NONE, odin3_cell_lut.const_value(params));
    }
    /* through the IR: the net a constant $lut drives carries the constant (IR-4) */
    odin3_module_id mid = build_lut_module(3, "11111111");
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_node_id node = odin3_module_find_node(mod, intern("u"));
    odin3_net_id y_net = odin3_pin_net(mod, odin3_node_port(mod, node, ODIN3_LUT_PORT_Y).first);
    TEST_ASSERT_TRUE(odin3_net_valid(y_net));
    TEST_ASSERT_EQUAL(ODIN3_CONST_1, odin3_net_const_value(mod, y_net));
}

/* --- simulate ------------------------------------------------------------------------------ */

/* One $lut cell laid out for its simulate hook: A bits at values[0..width), Y at values[width]. */
typedef struct harness {
    uint8_t values[ODIN3_LUT_MAX_WIDTH + 1];
    uint32_t idx[ODIN3_LUT_MAX_WIDTH + 1];
    odin3_sim_span spans[2];
    odin3_value params[2];
    odin3_sim_cell cell;
} harness;

static void harness_init(harness *hns, uint32_t width) {
    memset(hns, 0, sizeof *hns);
    for (uint32_t i = 0; i <= width; i++) {
        hns->idx[i] = i;
    }
    hns->spans[ODIN3_LUT_PORT_A] = (odin3_sim_span){hns->idx, width};
    hns->spans[ODIN3_LUT_PORT_Y] = (odin3_sim_span){&hns->idx[width], 1};
    hns->params[ODIN3_LUT_P_WIDTH] = odin3_value_int(width);
    hns->params[ODIN3_LUT_P_LUT] = bits_value(table, 1U << width);
    hns->cell = (odin3_sim_cell){hns->values, hns->spans,     2,    hns->params, NULL,
                                 0,           ODIN3_SIM_COMB, NULL, NULL,        0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cell_lut.verify(hns->params));
}

/* Y for input index in (A[j] = bit j), the output preset to the opposite of the expected. */
static void expect_y(harness *hns, uint32_t input) {
    uint32_t width = hns->spans[ODIN3_LUT_PORT_A].width;
    for (uint32_t j = 0; j < width; j++) {
        hns->values[j] = (uint8_t)((input >> j) & 1U);
    }
    uint8_t want = table[input];
    hns->values[width] = (uint8_t)(want == 0 ? 1 : 0);
    odin3_cell_lut.simulate(&hns->cell);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, hns->values[width], "Y != LUT[i]");
}

static void random_table(odin3_prng *prng, uint32_t len) {
    for (uint32_t i = 0; i < len; i += 64) {
        uint64_t draw = odin3_prng_next(prng);
        for (uint32_t k = 0; k < 64 && i + k < len; k++) {
            table[i + k] = (uint8_t)((draw >> k) & 1U);
        }
    }
}

/* Table number tab of the harness's WIDTH: LUT[i] = bit i of tab. */
static void numbered_table(const harness *hns, uint32_t tab) {
    for (uint32_t i = 0; i < hns->params[ODIN3_LUT_P_LUT].len; i++) {
        table[i] = (uint8_t)((tab >> i) & 1U);
    }
}

/* Every input for WIDTH up to ALL_INPUTS_MAX, else RANDOM_INPUTS random ones. */
static void check_inputs(harness *hns, odin3_prng *prng, uint32_t len) {
    bool every = hns->spans[ODIN3_LUT_PORT_A].width <= ALL_INPUTS_MAX;
    uint32_t count = every ? len : RANDOM_INPUTS;
    for (uint32_t k = 0; k < count; k++) {
        expect_y(hns, every ? k : (uint32_t)odin3_prng_next(prng) & (len - 1));
    }
}

static void simulate_width(odin3_prng *prng, uint32_t width) {
    harness hns;
    harness_init(&hns, width);
    uint32_t len = 1U << width;
    bool exhaustive = width <= EXHAUSTIVE_MAX;
    uint32_t tables = exhaustive ? 1U << len : RANDOM_TABLES;
    for (uint32_t tab = 0; tab < tables; tab++) {
        if (exhaustive) {
            numbered_table(&hns, tab);
        } else {
            random_table(prng, len);
        }
        check_inputs(&hns, prng, len);
    }
}

static void test_simulate_tables(void) {
    odin3_prng prng;
    odin3_prng_seed(&prng, SEED);
    for (uint32_t width = 1; width <= ODIN3_LUT_MAX_WIDTH; width++) {
        simulate_width(&prng, width);
    }
}

/* Y for A[0] = a0, A[1] = a1 of a WIDTH-2 $lut whose table is msb_first. */
static uint8_t y_of(const char *msb_first, uint8_t a0, uint8_t a1) {
    harness hns;
    (void)lut_from_msb(msb_first, table);
    harness_init(&hns, 2);
    hns.values[0] = a0;
    hns.values[1] = a1;
    odin3_cell_lut.simulate(&hns.cell);
    return hns.values[2];
}

static void test_bit_order(void) {
    TEST_ASSERT_EQUAL_UINT8(0, y_of("1000", 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, y_of("1000", 1, 0));
    TEST_ASSERT_EQUAL_UINT8(0, y_of("1000", 0, 1));
    TEST_ASSERT_EQUAL_UINT8(1, y_of("1000", 1, 1));
    TEST_ASSERT_EQUAL_UINT8(0, y_of("0010", 0, 0));
    TEST_ASSERT_EQUAL_UINT8(1, y_of("0010", 1, 0)); /* A[0] is the LSB of the index */
    TEST_ASSERT_EQUAL_UINT8(0, y_of("0010", 0, 1));
    TEST_ASSERT_EQUAL_UINT8(0, y_of("0010", 1, 1));
}

/* --- table helpers ------------------------------------------------------------------------- */

static void test_table_helpers(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_lut_words(1));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_lut_words(6));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_lut_words(7));
    TEST_ASSERT_EQUAL_UINT32(1024, odin3_lut_words(16));
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    const uint64_t words[2] = {0x8000000000000001ULL, 0x1ULL};
    odin3_value params[2];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_params(arena, 7, words, params));
    TEST_ASSERT_EQUAL_INT64(7, params[ODIN3_LUT_P_WIDTH].i);
    TEST_ASSERT_EQUAL_UINT32(128, params[ODIN3_LUT_P_LUT].len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cell_lut.verify(params));
    for (uint32_t i = 0; i < params[ODIN3_LUT_P_LUT].len; i++) {
        uint8_t want = i == 0 || i == 63 || i == 64 ? ODIN3_BIT_1 : ODIN3_BIT_0;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, params[ODIN3_LUT_P_LUT].bits[i], "LUT bit");
    }
    uint64_t back[3] = {~0ULL, ~0ULL, ~0ULL};
    TEST_ASSERT_EQUAL(false, odin3_lut_table(params, back, 1)); /* below odin3_lut_words(7) */
    TEST_ASSERT_EQUAL(true, odin3_lut_table(params, back, 3));
    TEST_ASSERT_EQUAL_HEX64(words[0], back[0]);
    TEST_ASSERT_EQUAL_HEX64(words[1], back[1]);
    TEST_ASSERT_EQUAL_HEX64(0, back[2]);
    odin3_arena_destroy(arena);
}

/* Bits from 2^width up are ignored by odin3_lut_params and read back as zero. */
static void test_table_upper_bits(void) {
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[2];
    const uint64_t high[1] = {0xF0ULL | 0x5ULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_params(arena, 2, high, params));
    uint64_t back[2] = {~0ULL, ~0ULL};
    TEST_ASSERT_EQUAL(true, odin3_lut_table(params, back, 2));
    TEST_ASSERT_EQUAL_HEX64(0x5ULL, back[0]);
    TEST_ASSERT_EQUAL_HEX64(0, back[1]);
    odin3_arena_destroy(arena);
}

/* odin3_lut_params refuses width 0, width 17 and NULL arguments, leaving params alone. */
static void test_table_params_refusals(void) {
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    const uint64_t words[2] = {0x1ULL, 0x1ULL};
    odin3_value untouched[2] = {odin3_value_int(-1), odin3_value_int(-1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_lut_params(arena, 17, words, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_lut_params(arena, 0, words, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_lut_params(NULL, 1, words, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_lut_params(arena, 1, NULL, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_lut_params(arena, 1, words, NULL));
    TEST_ASSERT_EQUAL_INT64(-1, untouched[0].i);
    TEST_ASSERT_EQUAL_INT64(-1, untouched[1].i);
    TEST_ASSERT_EQUAL_UINT32(0, log_count); /* the helpers never log */
    odin3_arena_destroy(arena);
}

/* odin3_lut_table refuses what verify refuses, and leaves words alone. */
static void test_table_refusals(void) {
    static const uint8_t k_x[4] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_0};
    static const uint8_t k_two[2] = {ODIN3_BIT_0, ODIN3_BIT_1};
    uint64_t back[1] = {0x77ULL};
    odin3_value params[2] = {odin3_value_int(2), bits_value(k_x, 4)};
    TEST_ASSERT_EQUAL(false, odin3_lut_table(params, back, 1));
    params[1] = bits_value(k_two, 2);
    TEST_ASSERT_EQUAL(false, odin3_lut_table(params, back, 1)); /* 2 bits for WIDTH 2 */
    params[0] = odin3_value_int(0);
    TEST_ASSERT_EQUAL(false, odin3_lut_table(params, back, 1));
    params[0] = odin3_value_int(1);
    TEST_ASSERT_EQUAL(false, odin3_lut_table(NULL, back, 1));
    TEST_ASSERT_EQUAL(false, odin3_lut_table(params, NULL, 1));
    TEST_ASSERT_EQUAL_HEX64(0x77ULL, back[0]);
    TEST_ASSERT_EQUAL(true, odin3_lut_table(params, back, 1));
    TEST_ASSERT_EQUAL_HEX64(0x2ULL, back[0]);
    TEST_ASSERT_EQUAL_UINT32(0, log_count);
}

/* --- engine word layout (I10) -------------------------------------------------------------- */

static void test_engine_word_layout(void) {
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[2];
    const uint32_t six[2] = {0xFFFFFFA5U, 0xFFFFFFFFU}; /* lut_size 6: W = 2 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_from_engine(arena, 3, 6, six, params));
    TEST_ASSERT_EQUAL_INT64(3, params[ODIN3_LUT_P_WIDTH].i);
    uint8_t want[16];
    TEST_ASSERT_EQUAL_UINT32(8, lut_from_msb("10100101", want)); /* bits 8 and up ignored */
    TEST_ASSERT_EQUAL_UINT32(8, params[ODIN3_LUT_P_LUT].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, params[ODIN3_LUT_P_LUT].bits, 8);
    const uint32_t four[1] = {0x0000F00FU}; /* lut_size 4: W = 1 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_from_engine(arena, 4, 4, four, params));
    TEST_ASSERT_EQUAL_UINT32(16, params[ODIN3_LUT_P_LUT].len);
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t bit = i <= 3 || i >= 12 ? ODIN3_BIT_1 : ODIN3_BIT_0;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(bit, params[ODIN3_LUT_P_LUT].bits[i], "engine bit");
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cell_lut.verify(params));
    /* k = lut_size = 6 reads both 32-bit words: word 1 holds LUT[32..63] */
    const uint32_t two_words[2] = {0x00000001U, 0x80000002U};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_from_engine(arena, 6, 6, two_words, params));
    TEST_ASSERT_EQUAL_UINT32(64, params[ODIN3_LUT_P_LUT].len);
    for (uint32_t i = 0; i < 64; i++) {
        uint8_t bit = i == 0 || i == 33 || i == 63 ? ODIN3_BIT_1 : ODIN3_BIT_0;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(bit, params[ODIN3_LUT_P_LUT].bits[i], "two-word bit");
    }
    odin3_value untouched[2] = {odin3_value_int(-1), odin3_value_int(-1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_lut_from_engine(arena, 7, 6, six, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_lut_from_engine(arena, 0, 6, six, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_lut_from_engine(arena, 3, 17, six, untouched));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_lut_from_engine(arena, 3, 6, NULL, untouched));
    TEST_ASSERT_EQUAL_INT64(-1, untouched[0].i);
    odin3_arena_destroy(arena);
}

/* odin3_lut_from_engine(k, lut_size, truth) has LUT bytes equal to want (MSB-first text). */
/* One engine case: n_inputs, lut_size, the single truth word and the LUT expected (MSB first). */
typedef struct engine_case {
    uint32_t n_inputs;
    uint32_t lut_size;
    uint32_t word;
    const char *want;
} engine_case;

static void expect_engine(const engine_case *ec) {
    uint32_t n_inputs = ec->n_inputs;
    uint32_t lut_size = ec->lut_size;
    uint32_t word = ec->word;
    const char *want = ec->want;
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    const uint32_t truth[1] = {word};
    odin3_value params[2];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_lut_from_engine(arena, n_inputs, lut_size, truth, params));
    TEST_ASSERT_EQUAL_INT64(n_inputs, params[ODIN3_LUT_P_WIDTH].i);
    uint8_t bits[32];
    uint32_t len = lut_from_msb(want, bits);
    TEST_ASSERT_EQUAL_UINT32(len, params[ODIN3_LUT_P_LUT].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(bits, params[ODIN3_LUT_P_LUT].bits, len, want);
    odin3_arena_destroy(arena);
}

/* The MiniLUT shapes of lut_size <= 5 (one 32-bit word, ABC replicating the table in it). */
static void test_engine_small_sizes(void) {
    static const engine_case k_cases[] = {
        {1, 2, 0x55555555U, "01"},                               /* ABC's inverter LUT: NOT */
        {5, 5, 0x80000001U, "10000000000000000000000000000001"}, /* bits 0 and 31 */
        {3, 5, 0xA5A5A5A5U, "10100101"},                         /* only the first 2^3 bits */
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        expect_engine(&k_cases[i]);
    }
}

/* --- text forms ---------------------------------------------------------------------------- */

static const char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(file_buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file_buf[len] = '\0';
    return file_buf;
}

static void out_path(char *path, const char *name) {
    int len = snprintf(path, PATH_MAX_LEN, "%s/%s", ODIN3_LUT_OUT, name);
    TEST_ASSERT_TRUE(len > 0 && len < PATH_MAX_LEN);
}

static void expect_in(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), needle);
}

static void test_json_form(void) {
    (void)build_lut_module(2, "0110");
    check_full_ok();
    char path[PATH_MAX_LEN];
    out_path(path, "lut.json");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(design, path));
    const char *text = slurp(path);
    expect_in(text, "\"type\": \"$lut\"");
    expect_in(text, "\"LUT\": \"0110\"");
    expect_in(text, "\"WIDTH\": \"00000000000000000000000000000010\"");
    expect_in(text, "\"A\": \"input\"");
    expect_in(text, "\"Y\": \"output\"");
}

/* An asymmetric table shows the MSB-first order: LUT[3] = 1 is the first character. */
static void test_json_form_msb_first(void) {
    (void)build_lut_module(2, "1000");
    char path[PATH_MAX_LEN];
    out_path(path, "lut_msb.json");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(design, path));
    expect_in(slurp(path), "\"LUT\": \"1000\"");
}

static void rtlil_of(odin3_strbuf *text) {
    odin3_strbuf_init(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(design, text));
    TEST_ASSERT_NOT_NULL(text->data);
}

static void test_rtlil_form(void) {
    (void)build_lut_module(2, "0110");
    check_full_ok();
    odin3_strbuf text;
    rtlil_of(&text);
    expect_in(text.data, "  cell $lut \\u\n");
    expect_in(text.data, "\n    parameter \\WIDTH 2\n");
    expect_in(text.data, "\n    parameter \\LUT 4'0110\n");
    char path[PATH_MAX_LEN];
    out_path(path, "lut.il");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_write(design, path));
    TEST_ASSERT_EQUAL_STRING(text.data, slurp(path));
    odin3_strbuf_free(&text);
}

static void test_rtlil_form_msb_first(void) {
    (void)build_lut_module(2, "1000");
    odin3_strbuf text;
    rtlil_of(&text);
    expect_in(text.data, "\n    parameter \\LUT 4'1000\n");
    odin3_strbuf_free(&text);
}

/* --- interim BLIF, Verilog and DOT forms (until 3E-5 and 3D Task 9; docs/IR.md) ----------- */

/* write_blif of a one-$lut module: INVALID_ARG with the refusal line, or OK and the .subckt. */
/* One BLIF case: the $lut's WIDTH and table (MSB first), and the .subckt expected (NULL: refused).
 */
typedef struct blif_case {
    uint32_t width;
    const char *msb_first;
    const char *subckt;
} blif_case;

static void expect_blif(const blif_case *bc) {
    const char *msb_first = bc->msb_first;
    const char *subckt = bc->subckt;
    (void)build_lut_module(bc->width, msb_first);
    char path[PATH_MAX_LEN];
    out_path(path, "interim.blif");
    log_count = 0;
    odin3_status st = odin3_blif_write(design, path);
    if (subckt == NULL) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, st, msb_first);
        TEST_ASSERT_EQUAL_UINT32(1, log_count);
        expect_in(log_lines[0], ": model 'm': parameters neither declared nor implied by the "
                                "connected formals have no BLIF form; cannot write a cell of "
                                "'$lut'");
    } else {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, msb_first);
        expect_in(slurp(path), subckt);
    }
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

/* write_blif refuses every $lut but the WIDTH-1 identity, which is a .subckt (review I1). */
static void test_interim_blif(void) {
    static const blif_case k_cases[] = {
        {2, "0110", NULL},
        {2, "1000", NULL},
        {1, "01", NULL},
        {1, "10", "\n.subckt $lut A[0]=a[0] Y=y\n"},
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        expect_blif(&k_cases[i]);
    }
}

/* write_verilog writes a generic instance, LUT MSB first; write_dot a box labelled $lut. */
static void test_interim_verilog_dot(void) {
    (void)build_lut_module(2, "1000");
    char path[PATH_MAX_LEN];
    out_path(path, "interim.v");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, path, NULL));
    expect_in(slurp(path),
              "  \\$lut #(.WIDTH(2), .LUT(4'b1000)) u (\n    .A(a),\n    .Y(y)\n  );\n");
    out_path(path, "interim.dot");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_dot_write(design, path, NULL));
    expect_in(slurp(path), "[label=\"$lut\\nu\"];");
}

/* --- out of memory ------------------------------------------------------------------------- */

static uint64_t oom_words[1024];

/* One odin3_lut_params of a 16-input table with allocation fail_at failing; its status. */
static odin3_status try_lut_params(long fail_at) {
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[2] = {odin3_value_int(-1), odin3_value_int(-1)};
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_lut_params(arena, ODIN3_LUT_MAX_WIDTH, oom_words, params);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_INT64(-1, params[0].i);
    } else {
        static uint64_t back[1024];
        TEST_ASSERT_EQUAL(true, odin3_lut_table(params, back, 1024));
        TEST_ASSERT_EQUAL_HEX64_ARRAY(oom_words, back, 1024);
    }
    odin3_arena_destroy(arena);
    return st;
}

static void test_oom_lut_params(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        oom_words[i] = 0x0123456789ABCDEFULL * (i + 1);
    }
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = try_lut_params(fail_at);
        failures += st != ODIN3_OK ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
}

/* One odin3_lut_from_engine of a 16-input table (W = 2^11) with allocation fail_at failing. */
static odin3_status try_from_engine(long fail_at, const uint32_t *truth) {
    odin3_arena *arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[2] = {odin3_value_int(-1), odin3_value_int(-1)};
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st =
        odin3_lut_from_engine(arena, ODIN3_LUT_MAX_WIDTH, ODIN3_LUT_MAX_WIDTH, truth, params);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_INT64(-1, params[0].i);
    } else {
        static uint64_t back[1024];
        TEST_ASSERT_EQUAL(true, odin3_lut_table(params, back, 1024));
        for (size_t i = 0; i < 1024; i++) {
            uint64_t want = (uint64_t)truth[2 * i] | ((uint64_t)truth[(2 * i) + 1] << 32);
            TEST_ASSERT_EQUAL_HEX64(want, back[i]);
        }
    }
    odin3_arena_destroy(arena);
    return st;
}

static void test_oom_from_engine(void) {
    static uint32_t truth[2048];
    for (uint32_t i = 0; i < 2048; i++) {
        truth[i] = 0x9E3779B9U * (i + 1);
    }
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = try_from_engine(fail_at, truth);
        failures += st != ODIN3_OK ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
}

/* One attempt at a connected 16-input $lut in a fresh design; its status. */
static odin3_status try_wide_node(long fail_at) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid = {0};
    odin3_module *mod = new_module("m", &mid);
    odin3_net_id a_nets[ODIN3_LUT_MAX_WIDTH];
    odin3_net_id y_net = {0};
    add_port(mod, "a", ODIN3_DIR_IN, ODIN3_LUT_MAX_WIDTH, a_nets);
    add_port(mod, "y", ODIN3_DIR_OUT, 1, &y_net);
    for (uint32_t i = 0; i < MAX_TABLE; i++) {
        table[i] = (uint8_t)(i % 3 == 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    odin3_value params[2] = {odin3_value_int(ODIN3_LUT_MAX_WIDTH), bits_value(table, MAX_TABLE)};
    odin3_netvec ports[2] = {{a_nets, ODIN3_LUT_MAX_WIDTH}, {&y_net, 1}};
    odin3_node_spec spec = {lut_type(), 0, (odin3_prov_id){0}, params, 2};
    uint32_t nodes = odin3_module_node_end(mod);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_node_create_connected(mod, &spec, ports, NULL);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(mod));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, lut_type()));
        TEST_ASSERT_EQUAL_UINT32(1, odin3_net_pins(mod, y_net).count); /* the port pin only */
    }
    check_full_ok();
    return st;
}

static void test_oom_wide_node(void) {
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = try_wide_node(fail_at);
        failures += st != ODIN3_OK ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, lut_type()));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_registry_lookup);
    RUN_TEST(test_defaults_identity);
    RUN_TEST(test_sim_build_width6);
    RUN_TEST(test_verify_refusals);
    RUN_TEST(test_const_value);
    RUN_TEST(test_simulate_tables);
    RUN_TEST(test_bit_order);
    RUN_TEST(test_table_helpers);
    RUN_TEST(test_table_upper_bits);
    RUN_TEST(test_table_params_refusals);
    RUN_TEST(test_table_refusals);
    RUN_TEST(test_engine_word_layout);
    RUN_TEST(test_engine_small_sizes);
    RUN_TEST(test_json_form);
    RUN_TEST(test_json_form_msb_first);
    RUN_TEST(test_rtlil_form);
    RUN_TEST(test_rtlil_form_msb_first);
    RUN_TEST(test_interim_blif);
    RUN_TEST(test_interim_verilog_dot);
    RUN_TEST(test_oom_lut_params);
    RUN_TEST(test_oom_from_engine);
    RUN_TEST(test_oom_wide_node);
    return UNITY_END();
}
