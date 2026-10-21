/*
 * test_dffx_cells.c — the 68 gate-level flip-flops with a reset or an enable (dffx.h): the table
 * (names, ports, flags), and the simulate hook against a reference model written from Yosys 0.55
 * simcells.v truth tables, driven through hand-built views.
 */
#include "ir/celltype.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MAX_PINS = 5, NAME_LEN = 24, MAX_TYPES = 80, N_COMBOS = 8 };

void setUp(void) {
}
void tearDown(void) {
}

/* One type of the family, parsed from its name. */
typedef struct model {
    char family[8]; /* DFF, DFFE, SDFF, SDFFE, SDFFCE */
    bool clk_pos;
    bool has_reset;
    bool rst_pos;
    bool rst_val;
    bool has_en;
    bool en_pos;
    bool async;
    bool en_first; /* $_SDFFCE_: the enable is over the reset */
} model;

static bool pol(char letter) {
    return letter == 'P';
}

/* Parses `$_<family>_<letters>_`; the DFFE family is split by the number of letters. */
static model parse(const char *name) {
    model mod;
    memset(&mod, 0, sizeof mod);
    const char *cur = name + 2;
    size_t flen = strcspn(cur, "_");
    memcpy(mod.family, cur, flen);
    cur += flen + 1;
    mod.clk_pos = pol(cur[0]);
    size_t nlet = strlen(cur) - 1; /* without the closing _ */
    if (strcmp(mod.family, "DFFE") == 0 && nlet == 2) {
        mod.has_en = true;
        mod.en_pos = pol(cur[1]);
        return mod;
    }
    mod.has_reset = true;
    mod.rst_pos = pol(cur[1]);
    mod.rst_val = cur[2] == '1';
    mod.async = strcmp(mod.family, "DFF") == 0 || strcmp(mod.family, "DFFE") == 0;
    mod.has_en = nlet == 4;
    mod.en_pos = mod.has_en && pol(cur[3]);
    mod.en_first = strcmp(mod.family, "SDFFCE") == 0;
    return mod;
}

/* The reference: Q after the event `edge_on` (true: this type's active edge), from q0, with the
 * pins at (rst, en, data) as raw levels. */
static bool reference(const model *mod, bool q0, bool edge_on, const bool pins[3]) {
    bool rst_on = mod->has_reset && pins[0] == mod->rst_pos;
    bool en_on = !mod->has_en || pins[1] == mod->en_pos;
    if (mod->async && rst_on) {
        return mod->rst_val;
    }
    if (!edge_on) {
        return q0;
    }
    if (mod->en_first && !en_on) {
        return q0;
    }
    if (mod->en_first) {
        return rst_on ? mod->rst_val : pins[2];
    }
    if (rst_on) {
        return mod->rst_val;
    }
    return en_on ? pins[2] : q0;
}

/* A cell under test: one slot per port, in definition order. */
typedef struct harness {
    const odin3_celltype_def *def;
    uint8_t values[MAX_PINS];
    uint32_t idx[MAX_PINS];
    odin3_sim_span spans[MAX_PINS];
    uint8_t state[1];
    odin3_value params[1];
    odin3_sim_cell cell;
} harness;

static void setup(harness *hns, const odin3_celltype_def *def, int64_t init) {
    memset(hns, 0, sizeof *hns);
    hns->def = def;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(MAX_PINS, def->n_ports, def->name);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        hns->idx[i] = i;
        hns->spans[i] = (odin3_sim_span){&hns->idx[i], 1};
    }
    hns->params[0] = odin3_value_int(init);
    hns->cell = (odin3_sim_cell){hns->values, hns->spans,     def->n_ports, hns->params, hns->state,
                                 1,           ODIN3_SIM_COMB, NULL,         NULL,        0};
}

static void fire(harness *hns, odin3_sim_event event) {
    hns->cell.event = event;
    hns->def->simulate(&hns->cell);
}

static uint32_t port_of(const odin3_celltype_def *def, const char *port) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, port) == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* --- the table -------------------------------------------------------------------------------- */

static const char *const k_pol = "PN";

/* Every name of the family, made by nested loops (not by dffx.h's macros). */
static uint32_t expected_names(char names[][NAME_LEN]) {
    uint32_t count = 0;
    for (const char *clk = k_pol; *clk != '\0'; clk++) {
        for (const char *en = k_pol; *en != '\0'; en++) {
            (void)snprintf(names[count++], NAME_LEN, "$_DFFE_%c%c_", *clk, *en);
        }
        for (const char *rst = k_pol; *rst != '\0'; rst++) {
            for (const char *val = "01"; *val != '\0'; val++) {
                (void)snprintf(names[count++], NAME_LEN, "$_DFF_%c%c%c_", *clk, *rst, *val);
                (void)snprintf(names[count++], NAME_LEN, "$_SDFF_%c%c%c_", *clk, *rst, *val);
                for (const char *en = k_pol; *en != '\0'; en++) {
                    (void)snprintf(names[count++], NAME_LEN, "$_DFFE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                    (void)snprintf(names[count++], NAME_LEN, "$_SDFFE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                    (void)snprintf(names[count++], NAME_LEN, "$_SDFFCE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                }
            }
        }
    }
    return count;
}

static const odin3_celltype_def *find(const char *name) {
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        if (strcmp(odin3_builtin_celltypes[i]->name, name) == 0) {
            return odin3_builtin_celltypes[i];
        }
    }
    return NULL;
}

static void check_shape(const odin3_celltype_def *def, const char *name) {
    TEST_ASSERT_NOT_NULL_MESSAGE(def, name);
    TEST_ASSERT_EQUAL_MESSAGE(ODIN3_GRAN_BIT, def->gran, name);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0, def->flags, name);
    model mod = parse(name);
    uint32_t want_ports = 3 + (mod.has_reset ? 1U : 0U) + (mod.has_en ? 1U : 0U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_ports, def->n_ports, name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("C", def->ports[0].name, name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Q", def->ports[def->n_ports - 1].name, name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("D", def->ports[def->n_ports - 2].name, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(mod.has_reset ? 1U : UINT32_MAX, port_of(def, "R"), name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(mod.has_en ? 1U + (mod.has_reset ? 1U : 0U) : UINT32_MAX,
                                     port_of(def, "E"), name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, def->n_params, name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("INIT", def->params[0].name, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(def->simulate, name);
}

static void test_table(void) {
    static char names[MAX_TYPES][NAME_LEN];
    uint32_t count = expected_names(names);
    TEST_ASSERT_EQUAL_UINT32(68, count);
    for (uint32_t i = 0; i < count; i++) {
        check_shape(find(names[i]), names[i]);
        for (uint32_t j = 0; j < i; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(names[i], names[j]) != 0, names[i]);
        }
    }
}

/* Appended after $lut: the 75 earlier types did not move. */
static void test_appended(void) {
    TEST_ASSERT_EQUAL_UINT32(75 + 68, odin3_builtin_celltype_count);
    TEST_ASSERT_EQUAL_STRING("$lut", odin3_builtin_celltypes[74]->name);
}

/* --- the simulate hook -------------------------------------------------------------------------
 */

static void set_pin(harness *hns, const char *port, bool level) {
    uint32_t at = port_of(hns->def, port);
    if (at != UINT32_MAX) {
        hns->values[at] = (uint8_t)(level ? 1 : 0);
    }
}

/* One scenario: INIT q0, pins, one event, then COMB; Q must be the reference. */
static void run_case(const odin3_celltype_def *def, const model *mod, bool q0, const bool pins[3],
                     odin3_sim_event event) {
    harness hns;
    setup(&hns, def, q0 ? 1 : 0);
    fire(&hns, ODIN3_SIM_INIT);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(q0 ? 1 : 0, hns.state[0], def->name);
    set_pin(&hns, "R", pins[0]);
    set_pin(&hns, "E", pins[1]);
    set_pin(&hns, "D", pins[2]);
    fire(&hns, event);
    fire(&hns, ODIN3_SIM_COMB);
    odin3_sim_event active = mod->clk_pos ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE;
    bool want = reference(mod, q0, event == active, pins);
    char msg[96];
    (void)snprintf(msg, sizeof msg, "%s q0=%d R=%d E=%d D=%d event=%d", def->name, q0, pins[0],
                   pins[1], pins[2], (int)event);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(want ? 1 : 0, hns.values[def->n_ports - 1], msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(want ? 1 : 0, hns.state[0], msg);
}

static void test_truth(void) {
    static char names[MAX_TYPES][NAME_LEN];
    uint32_t count = expected_names(names);
    static const odin3_sim_event k_events[] = {ODIN3_SIM_POSEDGE, ODIN3_SIM_NEGEDGE,
                                               ODIN3_SIM_COMB};
    for (uint32_t i = 0; i < count; i++) {
        const odin3_celltype_def *def = find(names[i]);
        model mod = parse(names[i]);
        for (uint32_t combo = 0; combo < N_COMBOS * 2; combo++) {
            bool pins[3] = {(combo & 1U) != 0, (combo & 2U) != 0, (combo & 4U) != 0};
            for (uint32_t ev = 0; ev < 3; ev++) {
                run_case(def, &mod, (combo & 8U) != 0, pins, k_events[ev]);
            }
        }
    }
}

/* An asynchronous reset with no clock edge at all drives Q at once; a synchronous one does not. */
static void test_async_without_edge(void) {
    const odin3_celltype_def *async = find("$_DFF_PN1_");
    const odin3_celltype_def *sync = find("$_SDFF_PN1_");
    harness hns;
    setup(&hns, async, 0);
    fire(&hns, ODIN3_SIM_INIT);
    set_pin(&hns, "R", false); /* active low */
    fire(&hns, ODIN3_SIM_COMB);
    TEST_ASSERT_EQUAL_UINT8(1, hns.values[async->n_ports - 1]);
    setup(&hns, sync, 0);
    fire(&hns, ODIN3_SIM_INIT);
    set_pin(&hns, "R", false);
    fire(&hns, ODIN3_SIM_COMB);
    TEST_ASSERT_EQUAL_UINT8(0, hns.values[sync->n_ports - 1]);
}

/* The reset beats the clock edge, and the priorities of the three synchronous families differ. */
static void test_priorities(void) {
    /* $_SDFFE_PP1P_: reset over enable, so R active with E inactive still sets Q. */
    harness hns;
    setup(&hns, find("$_SDFFE_PP1P_"), 0);
    fire(&hns, ODIN3_SIM_INIT);
    set_pin(&hns, "R", true);
    set_pin(&hns, "E", false);
    set_pin(&hns, "D", false);
    fire(&hns, ODIN3_SIM_POSEDGE);
    TEST_ASSERT_EQUAL_UINT8(1, hns.state[0]);
    /* $_SDFFCE_PP1P_: enable over reset, so E inactive holds. */
    setup(&hns, find("$_SDFFCE_PP1P_"), 0);
    fire(&hns, ODIN3_SIM_INIT);
    set_pin(&hns, "R", true);
    set_pin(&hns, "E", false);
    set_pin(&hns, "D", true);
    fire(&hns, ODIN3_SIM_POSEDGE);
    TEST_ASSERT_EQUAL_UINT8(0, hns.state[0]);
    set_pin(&hns, "E", true);
    fire(&hns, ODIN3_SIM_POSEDGE);
    TEST_ASSERT_EQUAL_UINT8(1, hns.state[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table);
    RUN_TEST(test_appended);
    RUN_TEST(test_truth);
    RUN_TEST(test_async_without_edge);
    RUN_TEST(test_priorities);
    return UNITY_END();
}
