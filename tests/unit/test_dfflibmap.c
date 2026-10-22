/*
 * test_dfflibmap.c — the `dfflibmap` pass (gatemap spec 6, G-6; plan Tasks 8 and 9): the state
 * cell choice (passes/dfflibmap/choose.c) on lib/generic_gates.o3lib, lib/cmos6.o3lib and inline
 * libraries.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "passes/dfflibmap/dfflibmap.h"
#include "techlib/libcells.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
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

/* The name of the state cell a choice picked. */
static const char *chosen(const odin3_dlm_choice *choice) {
    TEST_ASSERT_TRUE(choice->state < g_cells.states.len);
    const odin3_libstate *state = odin3_vec_cat(&g_cells.states, choice->state);
    return odin3_celltype_get(g_design, state->type)->name;
}

/* odin3_dlm_choose on kind and init picks the cell named name, inverting the clock or not. */
static void expect_choice(odin3_dlm_kind kind, int64_t init, const char *name, bool invert) {
    odin3_dlm_choice choice = {UINT32_MAX, !invert};
    TEST_ASSERT_TRUE_MESSAGE(odin3_dlm_choose(g_design, &g_cells, kind, init, &choice), name);
    TEST_ASSERT_EQUAL_STRING(name, chosen(&choice));
    TEST_ASSERT_EQUAL(invert, choice.invert_clock);
}

/* odin3_dlm_choose on kind and init finds nothing and leaves its output alone. */
static void expect_none(odin3_dlm_kind kind, int64_t init) {
    odin3_dlm_choice choice = {UINT32_MAX, false};
    TEST_ASSERT_FALSE(odin3_dlm_choose(g_design, &g_cells, kind, init, &choice));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, choice.state);
}

/* --- Task 8: choosing a state cell --------------------------------------------------------- */

static void test_choose_posedge_generic(void) {
    load_file("generic_gates.o3lib");
    collect("generic_gates");
    expect_choice(ODIN3_DLM_DFF_P, 3, "DFFP", false);
    expect_choice(ODIN3_DLM_DFF_N, 0, "DFFN", false);
    expect_choice(ODIN3_DLM_DLATCH_P, 1, "DLATCHP", false);
    expect_choice(ODIN3_DLM_DLATCH_N, 2, "DLATCHN", false);
}

static void test_choose_area_tie_break(void) {
    load_text("library t\n"
              "cell F0 gate ; in C 1 clock ; in D 1 ; out Q 1 ; seq Q <= D @ posedge C ; end\n"
              "cell F5 gate area 5 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init x\nend\n"
              "cell F3 gate area 3 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init x\nend\n"
              "cell L4 gate area 4 ; in E 1 clock ; in D 1 ; out Q 1 ; seq Q <= D @ high E ; end\n"
              "cell L4B gate area 4 ; in E 1 clock ; in D 1 ; out Q 1 ; seq Q <= D @ high E ; end\n"
              "cell N0 gate ; in C 1 clock ; in D 1 ; out Q 1 ; seq Q <= D @ negedge C ; end\n");
    collect("t");
    expect_choice(ODIN3_DLM_DFF_P, 3, "F3", false);
    expect_choice(ODIN3_DLM_DLATCH_P, 3, "L4", false);
    /* The only negedge cell has no area: still chosen. */
    expect_choice(ODIN3_DLM_DFF_N, 3, "N0", false);
}

static void test_choose_init_rules(void) {
    load_text("library t\n"
              "cell Z gate area 1 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init 0\nend\n"
              "cell X gate area 9 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ negedge C init x\nend\n");
    collect("t");
    expect_none(ODIN3_DLM_DFF_P, 1);
    expect_choice(ODIN3_DLM_DFF_P, 2, "Z", false);
    expect_choice(ODIN3_DLM_DFF_P, 0, "Z", false);
    expect_choice(ODIN3_DLM_DFF_P, 3, "Z", false);
    /* Init x takes any INIT. */
    expect_choice(ODIN3_DLM_DFF_N, 1, "X", false);
    expect_none(ODIN3_DLM_KINDS, 3);
}

static void test_choose_negedge_fallback_inverts(void) {
    load_file("cmos6.o3lib");
    collect("cmos6");
    expect_choice(ODIN3_DLM_DFF_N, 3, "DFF", true);
    expect_choice(ODIN3_DLM_DFF_P, 3, "DFF", false);
}

static void test_choose_no_inverter_no_fallback(void) {
    load_text("library t\n"
              "cell F gate area 4 ; in C 1 clock ; in D 1 ; out Q 1\n"
              "  seq Q <= D @ posedge C init x\nend\n"
              "cell B gate area 1 ; in A 1 ; out Y 1 ; fn Y = A ; end\n"
              "cell L gate area 4 ; in E 1 clock ; in D 1 ; out Q 1 ; seq Q <= D @ low E ; end\n");
    collect("t");
    expect_none(ODIN3_DLM_DFF_N, 3);
    expect_choice(ODIN3_DLM_DFF_P, 3, "F", false);
    /* Latches never fall back to the other level. */
    expect_none(ODIN3_DLM_DLATCH_P, 3);
}

static void test_choose_no_latch_in_cmos6(void) {
    load_file("cmos6.o3lib");
    collect("cmos6");
    expect_none(ODIN3_DLM_DLATCH_P, 3);
    expect_none(ODIN3_DLM_DLATCH_N, 3);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_choose_posedge_generic);
    RUN_TEST(test_choose_area_tie_break);
    RUN_TEST(test_choose_init_rules);
    RUN_TEST(test_choose_negedge_fallback_inverts);
    RUN_TEST(test_choose_no_inverter_no_fallback);
    RUN_TEST(test_choose_no_latch_in_cmos6);
    return UNITY_END();
}
