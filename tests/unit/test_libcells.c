/*
 * test_libcells.c — the library cell view (techlib/libcells.h) and the gate truth helper
 * (sim/gate_truth.h) on lib/generic_gates.o3lib, lib/cmos6.o3lib and inline libraries (gatemap
 * plan Task 3).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "sim/gate_truth.h"
#include "techlib/libcells.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must be defined"
#endif

enum { MSG_MAX = 1024 };

static odin3_design *g_design;
static odin3_libcells g_cells;
static char g_msg[MSG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
    g_cells = (odin3_libcells){0};
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_libcells_free(&g_cells);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
}

static void load_file(const char *name) {
    char path[MSG_MAX];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_LIB_DIR, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(g_design, path), g_msg);
}

static void load_text(const char *text) {
    odin3_techlib_text src = {"inline.o3lib", odin3_bytes_cstr(text)};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read_text(g_design, &src), g_msg);
}

static void collect(const char *library) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_libcells_collect(g_design, library, &g_cells));
}

static const char *type_name(odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(g_design, type);
    return def == NULL ? "(none)" : def->name;
}

static const odin3_libgate *gate_at(size_t idx) {
    return odin3_vec_cat(&g_cells.gates, idx);
}

static const odin3_libstate *state_at(size_t idx) {
    return odin3_vec_cat(&g_cells.states, idx);
}

static const odin3_libskip *skip_at(size_t idx) {
    return odin3_vec_cat(&g_cells.skips, idx);
}

static void expect_skip(size_t idx, const char *cell, uint8_t why) {
    TEST_ASSERT_TRUE(idx < g_cells.skips.len);
    TEST_ASSERT_EQUAL_STRING(cell, type_name(skip_at(idx)->type));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(why, skip_at(idx)->why, cell);
}

/* --- the shipped libraries ----------------------------------------------------------------- */

static void test_collect_generic_gates(void) {
    load_file("generic_gates.o3lib");
    collect("generic_gates");
    static const char *const names[] = {"BUF", "INV",   "AND2", "AND3", "AND4",  "OR2", "OR3",
                                        "OR4", "NAND2", "NOR2", "XOR2", "XNOR2", "MUX2"};
    static const uint64_t tables[] = {0x2,    0x1, 0x8, 0x80, 0x8000, 0xE, 0xFE,
                                      0xFFFE, 0x7, 0x1, 0x6,  0x9,    0xCA};
    TEST_ASSERT_EQUAL_UINT32(19, g_cells.n_cells);
    TEST_ASSERT_EQUAL_size_t(13, g_cells.gates.len);
    for (size_t i = 0; i < 13; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], type_name(gate_at(i)->type));
        TEST_ASSERT_EQUAL_HEX64_MESSAGE(tables[i], gate_at(i)->table, names[i]);
    }
    const odin3_libgate *mux = gate_at(12);
    TEST_ASSERT_EQUAL_UINT32(3, mux->n_inputs);
    TEST_ASSERT_EQUAL_UINT32(2, mux->in_port[2]);
    TEST_ASSERT_EQUAL_UINT32(3, mux->out_port);
    TEST_ASSERT_EQUAL_STRING("2", odin3_strtab_get(odin3_design_strtab(g_design), mux->area));
    TEST_ASSERT_EQUAL_UINT32(0, mux->delay);
    static const char *const state_names[] = {"DFFP", "DFFN", "DLATCHP", "DLATCHN"};
    static const odin3_techlib_trigger triggers[] = {ODIN3_TECHLIB_POSEDGE, ODIN3_TECHLIB_NEGEDGE,
                                                     ODIN3_TECHLIB_HIGH, ODIN3_TECHLIB_LOW};
    TEST_ASSERT_EQUAL_size_t(4, g_cells.states.len);
    for (size_t i = 0; i < 4; i++) {
        const odin3_libstate *state = state_at(i);
        TEST_ASSERT_EQUAL_STRING(state_names[i], type_name(state->type));
        TEST_ASSERT_EQUAL_INT(triggers[i], state->trigger);
        TEST_ASSERT_EQUAL_INT(-1, state->init);
        TEST_ASSERT_EQUAL_UINT32(0, state->data_port);
        TEST_ASSERT_EQUAL_UINT32(1, state->clock_port);
        TEST_ASSERT_EQUAL_UINT32(2, state->q_port);
        expect_skip(i, state_names[i], ODIN3_LIBCELL_STATE);
    }
    TEST_ASSERT_EQUAL_size_t(6, g_cells.skips.len);
    expect_skip(4, "TIE0", ODIN3_LIBCELL_CONSTANT);
    expect_skip(5, "TIE1", ODIN3_LIBCELL_CONSTANT);
    TEST_ASSERT_EQUAL_STRING("BUF", type_name(g_cells.buf));
    TEST_ASSERT_EQUAL_STRING("INV", type_name(g_cells.inv));
    TEST_ASSERT_EQUAL_STRING("TIE0", type_name(g_cells.tie0));
    TEST_ASSERT_EQUAL_STRING("TIE1", type_name(g_cells.tie1));
    TEST_ASSERT_EQUAL_STRING("generic_gates",
                             odin3_strtab_get(odin3_design_strtab(g_design), g_cells.library));
}

static void test_collect_cmos6(void) {
    load_file("cmos6.o3lib");
    collect("cmos6");
    static const char *const names[] = {"BUF", "INV", "NAND2", "NOR2"};
    static const uint64_t tables[] = {0x2, 0x1, 0x7, 0x1};
    TEST_ASSERT_EQUAL_UINT32(6, g_cells.n_cells);
    TEST_ASSERT_EQUAL_size_t(4, g_cells.gates.len);
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], type_name(gate_at(i)->type));
        TEST_ASSERT_EQUAL_HEX64(tables[i], gate_at(i)->table);
    }
    TEST_ASSERT_EQUAL_size_t(1, g_cells.states.len);
    const odin3_libstate *dff = state_at(0);
    TEST_ASSERT_EQUAL_STRING("DFF", type_name(dff->type));
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_POSEDGE, dff->trigger);
    TEST_ASSERT_EQUAL_INT(-1, dff->init);
    TEST_ASSERT_EQUAL_UINT32(0, dff->clock_port);
    TEST_ASSERT_EQUAL_UINT32(1, dff->data_port);
    TEST_ASSERT_EQUAL_UINT32(2, dff->q_port);
    TEST_ASSERT_EQUAL_size_t(2, g_cells.skips.len);
    expect_skip(0, "DFF", ODIN3_LIBCELL_STATE);
    expect_skip(1, "DFFSR", ODIN3_LIBCELL_NOT_GATE);
    TEST_ASSERT_EQUAL_STRING("BUF", type_name(g_cells.buf));
    TEST_ASSERT_EQUAL_STRING("INV", type_name(g_cells.inv));
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.tie0.v);
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.tie1.v);
}

static void test_collect_unknown_library(void) {
    load_file("cmos6.o3lib");
    collect("no_such_library");
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.n_cells);
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.library);
    TEST_ASSERT_EQUAL_size_t(0, g_cells.gates.len);
    TEST_ASSERT_EQUAL_size_t(0, g_cells.skips.len);
    /* A name in the strtab that no cell has as its library ("BUF" is a cell name). */
    odin3_libcells_free(&g_cells);
    collect("BUF");
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.n_cells);
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.library);
}

/* Review Focus 1: a cell whose fn ignores an input is never a gate. */
static void test_genlib_skips_non_support_input(void) {
    load_text("library t\ncell G gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(0, g_cells.gates.len);
    TEST_ASSERT_EQUAL_size_t(1, g_cells.skips.len);
    expect_skip(0, "G", ODIN3_LIBCELL_NO_SUPPORT);
    TEST_ASSERT_EQUAL_UINT32(1, skip_at(0)->port);
    char text[MSG_MAX];
    (void)snprintf(text, sizeof text, odin3_libcell_skip_text(skip_at(0)->why), "B");
    TEST_ASSERT_EQUAL_STRING("its function ignores input 'B'", text);
}

static void test_genlib_skips_reserved_port_name(void) {
    load_text("library t\n"
              "cell G1 gate area 1 ; in CONST0 1 ; in B 1 ; out Y 1 ; fn Y = CONST0 & B ; end\n"
              "cell G2 gate area 1 ; in A$1 1 ; out Y 1 ; fn Y = ~A$1 ; end\n"
              "cell G3 gate area 1 ; in A 1 ; out CONST1 1 ; fn CONST1 = ~A ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(0, g_cells.gates.len);
    TEST_ASSERT_EQUAL_size_t(3, g_cells.skips.len);
    expect_skip(0, "G1", ODIN3_LIBCELL_NAME);
    TEST_ASSERT_EQUAL_UINT32(0, skip_at(0)->port);
    expect_skip(1, "G2", ODIN3_LIBCELL_NAME);
    TEST_ASSERT_EQUAL_UINT32(0, skip_at(1)->port);
    expect_skip(2, "G3", ODIN3_LIBCELL_NAME);
    TEST_ASSERT_EQUAL_UINT32(1, skip_at(2)->port);
    char text[MSG_MAX];
    (void)snprintf(text, sizeof text, odin3_libcell_skip_text(ODIN3_LIBCELL_NAME), "A$1");
    TEST_ASSERT_EQUAL_STRING("port 'A$1' is not a genlib name", text);
}

/* gm-gates follow-ups (a) and (b): a cell whose own name is not a genlib name, the genlib's
 * reserved constant gate names included, is skipped, so the adapter never sees it. */
static void test_genlib_skips_bad_cell_name(void) {
    load_text("library t\n"
              "cell AND2.X1 gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A & B ; end\n"
              "cell nand2-x1 gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = ~(A & B) ; end\n"
              "cell 2NAND gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = ~(A & B) ; end\n"
              "cell $odin3_const0 gate area 1 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n"
              "cell $odin3_const1 gate area 1 ; in A 1 ; out Y 1 ; fn Y = A ; end\n"
              "cell CONST0 gate area 1 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n"
              "cell INV_X1 gate area 1 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(1, g_cells.gates.len);
    TEST_ASSERT_EQUAL_size_t(6, g_cells.skips.len);
    expect_skip(0, "AND2.X1", ODIN3_LIBCELL_CELL_NAME);
    expect_skip(1, "nand2-x1", ODIN3_LIBCELL_CELL_NAME);
    expect_skip(2, "2NAND", ODIN3_LIBCELL_CELL_NAME);
    expect_skip(3, "$odin3_const0", ODIN3_LIBCELL_CELL_NAME);
    expect_skip(4, "$odin3_const1", ODIN3_LIBCELL_CELL_NAME);
    expect_skip(5, "CONST0", ODIN3_LIBCELL_CELL_NAME);
    TEST_ASSERT_TRUE(odin3_celltype_valid(g_cells.inv));
    TEST_ASSERT_FALSE(odin3_celltype_valid(g_cells.buf));
}

static void test_buf_smallest_area_then_order(void) {
    load_text("library t\n"
              "cell B0 gate ; in A 1 ; out Y 1 ; fn Y = A ; end\n"
              "cell B3 gate area 3 ; in A 1 ; out Y 1 ; fn Y = A ; end\n"
              "cell B2 gate area 2 ; in A 1 ; out Y 1 ; fn Y = A ; end\n"
              "cell I1 gate area 1.5 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n"
              "cell I2 gate area 1.5 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(5, g_cells.gates.len);
    TEST_ASSERT_EQUAL_STRING("B2", type_name(g_cells.buf));
    TEST_ASSERT_EQUAL_STRING("I1", type_name(g_cells.inv));
    double area = 0.0;
    TEST_ASSERT_TRUE(odin3_libcells_area(g_design, gate_at(3)->area, &area));
    TEST_ASSERT_TRUE(area > 1.25 && area < 1.75); /* 1.5 */
    TEST_ASSERT_FALSE(odin3_libcells_area(g_design, gate_at(0)->area, &area));
}

/* A cell without area is chosen only when no cell of its role has one. */
static void test_tie_without_area(void) {
    load_text("library t\n"
              "cell T0 gate ; out Y 1 ; fn Y = 1'b0 ; end\n"
              "cell T1 gate ; out Y 1 ; fn Y = 1'b1 ; end\n"
              "cell T1B gate area 2 ; out Y 1 ; fn Y = 1'b1 ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_STRING("T0", type_name(g_cells.tie0));
    TEST_ASSERT_EQUAL_STRING("T1B", type_name(g_cells.tie1));
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.buf.v);
}

static void test_libstate_init_constant(void) {
    load_text("library t\n"
              "cell F1 gate area 4 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init 1'b1\nend\n"
              "cell F0 gate area 4 ; in D 1 ; in C 1 clock ; out Q 1\n"
              "  seq Q <= D @ negedge C init 0\nend\n"
              "cell F2 gate area 4 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init 2\nend\n"
              "cell FN gate area 4 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= ~D @ posedge C init x\nend\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(2, g_cells.states.len);
    TEST_ASSERT_EQUAL_STRING("F1", type_name(state_at(0)->type));
    TEST_ASSERT_EQUAL_INT(1, state_at(0)->init);
    TEST_ASSERT_EQUAL_STRING("F0", type_name(state_at(1)->type));
    TEST_ASSERT_EQUAL_INT(0, state_at(1)->init);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_NEGEDGE, state_at(1)->trigger);
    expect_skip(0, "F1", ODIN3_LIBCELL_STATE);
    expect_skip(1, "F0", ODIN3_LIBCELL_STATE);
    expect_skip(2, "F2", ODIN3_LIBCELL_SHAPE);
    expect_skip(3, "FN", ODIN3_LIBCELL_SHAPE);
    odin3_libstate state;
    odin3_celltype_id f2 = skip_at(2)->type;
    TEST_ASSERT_FALSE(odin3_libstate_of(g_design, f2, &state));
}

static void test_shape_skips(void) {
    load_text("library t\n"
              "cell W gate area 1 ; in A 2 ; out Y 1 ; fn Y = A[0] & A[1] ; end\n"
              "cell T gate area 1 ; in A 1 ; out Y 1 ; out Z 1 ; fn Y = A ; fn Z = ~A ; end\n"
              "cell H hard area 1 ; in A 1 ; out Y 1 ; fn Y = A ; end\n");
    collect("t");
    TEST_ASSERT_EQUAL_size_t(0, g_cells.gates.len);
    expect_skip(0, "W", ODIN3_LIBCELL_SHAPE);
    expect_skip(1, "T", ODIN3_LIBCELL_SHAPE);
    expect_skip(2, "H", ODIN3_LIBCELL_NOT_GATE);
}

static void test_collect_out_of_memory(void) {
    load_file("generic_gates.o3lib");
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                          odin3_libcells_collect(g_design, "generic_gates", &g_cells));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT32(0, g_cells.n_cells);
    TEST_ASSERT_EQUAL_size_t(0, g_cells.gates.len);
}

/* collect with the n-th allocation failing, for n = 0.. until it succeeds: every failure is
 * NO_MEMORY and leaves nothing behind (ASan checks the leaks). */
static void test_collect_oom_sweep(void) {
    enum { SWEEP_MAX = 10000 };
    load_file("cmos6.o3lib");
    long fail = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (; fail < SWEEP_MAX && st != ODIN3_OK; fail++) {
        odin3_libcells_free(&g_cells);
        odin3_util_set_alloc_fail_after(fail);
        st = odin3_libcells_collect(g_design, "cmos6", &g_cells);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_UINT32(0, g_cells.n_cells);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(fail > 1);
    TEST_ASSERT_EQUAL_size_t(4, g_cells.gates.len);
}

static void test_skip_texts(void) {
    TEST_ASSERT_EQUAL_STRING("not a gate cell", odin3_libcell_skip_text(ODIN3_LIBCELL_NOT_GATE));
    TEST_ASSERT_EQUAL_STRING("a flip-flop or latch", odin3_libcell_skip_text(ODIN3_LIBCELL_STATE));
    TEST_ASSERT_EQUAL_STRING("a constant cell (used for constants)",
                             odin3_libcell_skip_text(ODIN3_LIBCELL_CONSTANT));
    TEST_ASSERT_EQUAL_STRING("not one output and 1 to 6 one-bit inputs without parameters",
                             odin3_libcell_skip_text(ODIN3_LIBCELL_SHAPE));
    TEST_ASSERT_EQUAL_STRING("its name is not a genlib name",
                             odin3_libcell_skip_text(ODIN3_LIBCELL_CELL_NAME));
    TEST_ASSERT_EQUAL_STRING("", odin3_libcell_skip_text(0));
}

static odin3_celltype_id find_type(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(name), &str));
    TEST_ASSERT_TRUE(odin3_celltype_find(g_design, str, &type));
    return type;
}

/* The helper on a built-in gate (no type data). */
static void test_gate_truth_builtin(void) {
    const odin3_celltype_def *def = odin3_celltype_get(g_design, find_type("$_AND_"));
    TEST_ASSERT_TRUE(odin3_gate_truth_eligible(def));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_gate_truth_inputs(def));
    uint64_t table = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_gate_truth(def, NULL, &table));
    TEST_ASSERT_EQUAL_HEX64(0x8, table);
    int value = -1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_gate_const_value(def, NULL, &value));
    TEST_ASSERT_EQUAL_INT(-1, value);
}

/* ... and on library cells (fnsim's hook: type data and scratch). */
static void test_gate_truth_library(void) {
    load_file("generic_gates.o3lib");
    odin3_celltype_id tie1 = find_type("TIE1");
    int value = -1;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_gate_const_value(odin3_celltype_get(g_design, tie1),
                                                 odin3_celltype_lib(g_design, tie1), &value));
    TEST_ASSERT_EQUAL_INT(1, value);
    odin3_celltype_id xor2 = find_type("XOR2");
    uint64_t table = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_gate_truth(odin3_celltype_get(g_design, xor2),
                                                     odin3_celltype_lib(g_design, xor2), &table));
    TEST_ASSERT_EQUAL_HEX64(0x6, table);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_collect_generic_gates);
    RUN_TEST(test_collect_cmos6);
    RUN_TEST(test_collect_unknown_library);
    RUN_TEST(test_genlib_skips_non_support_input);
    RUN_TEST(test_genlib_skips_reserved_port_name);
    RUN_TEST(test_genlib_skips_bad_cell_name);
    RUN_TEST(test_buf_smallest_area_then_order);
    RUN_TEST(test_tie_without_area);
    RUN_TEST(test_libstate_init_constant);
    RUN_TEST(test_shape_skips);
    RUN_TEST(test_collect_out_of_memory);
    RUN_TEST(test_collect_oom_sweep);
    RUN_TEST(test_skip_texts);
    RUN_TEST(test_gate_truth_builtin);
    RUN_TEST(test_gate_truth_library);
    return UNITY_END();
}
