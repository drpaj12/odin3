/*
 * test_dfflibmap.c — the `dfflibmap` pass (gatemap spec 6, G-6; plan Tasks 8 and 9): the state
 * cell choice (passes/dfflibmap/choose.c) on lib/generic_gates.o3lib, lib/cmos6.o3lib and inline
 * libraries, and the pass (dfflibmap.c) on designs built from RTLIL text (proc_build.h).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "passes/builtin.h"
#include "passes/dfflibmap/dfflibmap.h"
#include "proc_build.h"
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
static pb_result g_res;
static char g_msg[MSG_MAX];
static char g_warn[MSG_MAX];
static char g_info[MSG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
    } else if (level == ODIN3_LOG_WARN) {
        (void)snprintf(g_warn, sizeof g_warn, "%s", msg);
    } else if (level == ODIN3_LOG_INFO && strncmp(msg, "dfflibmap:", strlen("dfflibmap:")) == 0) {
        (void)snprintf(g_info, sizeof g_info, "%s", msg);
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    g_warn[0] = '\0';
    g_info[0] = '\0';
    g_res = (pb_result){0};
    odin3_log_set_sink(sink, NULL);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
    g_cells = (odin3_libcells){0};
}

void tearDown(void) {
    pb_free(&g_res);
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

/* --- Task 9: the pass ---------------------------------------------------------------------- */

/* The two shipped libraries the pass tests read. */
typedef enum test_lib { LIB_GENERIC, LIB_CMOS6 } test_lib;

/* The design of rtlil (one module, `top`) with the library read first. */
static void build(const char *rtlil, test_lib lib) {
    char path[MSG_MAX];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_LIB_DIR,
                   lib == LIB_CMOS6 ? "cmos6.o3lib" : "generic_gates.o3lib");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pb_build_lib((pb_source){rtlil, path}, &g_res), g_msg);
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(g_res.design, opts));
}

/* Runs the pass body with args, with check FULL before and after it. */
static odin3_status run_pass(const char *args) {
    check_full();
    odin3_status st = odin3_pass_dfflibmap.run(&g_res.ctx, g_res.design, odin3_bytes_cstr(args),
                                               odin3_pass_dfflibmap.user);
    check_full();
    return st;
}

static const char *node_type_name(odin3_node_id node) {
    const odin3_module *mod = g_res.module;
    return odin3_celltype_get(g_res.design, odin3_node_type(mod, node))->name;
}

static uint32_t count_type(const char *type) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(g_res.module); id++) {
        odin3_node_id node = {id};
        bool hit = odin3_node_live(g_res.module, node) && strcmp(node_type_name(node), type) == 0;
        found += hit ? 1U : 0U;
    }
    return found;
}

static uint32_t count_live(void) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(g_res.module); id++) {
        found += odin3_node_live(g_res.module, (odin3_node_id){id}) ? 1U : 0U;
    }
    return found;
}

/* The nth live node (from 0) of the type named type. */
static odin3_node_id nth_of(const char *type, uint32_t nth) {
    for (uint32_t id = 1; id < odin3_module_node_end(g_res.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(g_res.module, node) && strcmp(node_type_name(node), type) == 0 &&
            nth-- == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(type);
    return (odin3_node_id){0};
}

static odin3_node_id node_named(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(g_res.design), odin3_bytes_cstr(name), &str));
    odin3_node_id node = odin3_module_find_node(g_res.module, str);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    return node;
}

/* The net on (1-bit) port `port` of node. */
static odin3_net_id port_net(odin3_node_id node, const char *port) {
    const odin3_celltype_def *def =
        odin3_celltype_get(g_res.design, odin3_node_type(g_res.module, node));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, port) == 0) {
            return odin3_pin_net(g_res.module, odin3_node_port(g_res.module, node, i).first);
        }
    }
    TEST_FAIL_MESSAGE(port);
    return (odin3_net_id){0};
}

static odin3_net_id wire_net(const char *name) {
    return port_net(nth_of("$_DFF_P_", 0), name);
}

#define TOP_PORTS                                                                                  \
    "module \\top\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\e\n"                 \
    "  wire output 4 \\q\n  wire output 5 \\r\n"

static void test_pass_maps_and_keeps_name_and_prov(void) {
    build(TOP_PORTS "  cell $_DFF_P_ \\q_reg\n    parameter \\INIT 3\n    connect \\C \\clk\n"
                    "    connect \\D \\d\n    connect \\Q \\q\n  end\n"
                    "  connect \\r \\e\nend\n",
          LIB_GENERIC);
    odin3_node_id old = node_named("q_reg");
    odin3_prov_id prov = odin3_node_prov(g_res.module, old);
    odin3_net_id clk = wire_net("C");
    odin3_net_id data = wire_net("D");
    odin3_net_id qnet = wire_net("Q");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_pass("generic_gates"), g_msg);
    odin3_node_id node = node_named("q_reg");
    TEST_ASSERT_EQUAL_STRING("DFFP", node_type_name(node));
    TEST_ASSERT_FALSE(odin3_node_live(g_res.module, old));
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(g_res.module, node).v);
    TEST_ASSERT_EQUAL_UINT32(clk.v, port_net(node, "C").v);
    TEST_ASSERT_EQUAL_UINT32(data.v, port_net(node, "D").v);
    TEST_ASSERT_EQUAL_UINT32(qnet.v, port_net(node, "Q").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_DFF_P_"));
    TEST_ASSERT_EQUAL_STRING("dfflibmap: library 'generic_gates': 1 cells mapped (DFFP x1), 0 "
                             "clock inverters",
                             g_info);
    TEST_ASSERT_EQUAL_STRING("", g_warn);
}

static void test_pass_shares_clock_inverter(void) {
    build(TOP_PORTS "  cell $_DFF_N_ \\a\n    parameter \\INIT 3\n    connect \\C \\clk\n"
                    "    connect \\D \\d\n    connect \\Q \\q\n  end\n"
                    "  cell $_DFF_N_ \\b\n    parameter \\INIT 2\n    connect \\C \\clk\n"
                    "    connect \\D \\e\n    connect \\Q \\r\n  end\nend\n",
          LIB_CMOS6);
    odin3_net_id clk = port_net(node_named("a"), "C");
    odin3_prov_id prov = odin3_node_prov(g_res.module, node_named("a"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_pass("cmos6"), g_msg);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("INV"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("DFF"));
    odin3_node_id inv = nth_of("INV", 0);
    TEST_ASSERT_EQUAL_UINT32(clk.v, port_net(inv, "A").v);
    odin3_net_id inverted = port_net(inv, "Y");
    TEST_ASSERT_EQUAL_UINT32(inverted.v, port_net(node_named("a"), "C").v);
    TEST_ASSERT_EQUAL_UINT32(inverted.v, port_net(node_named("b"), "C").v);
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(g_res.module, inv).v);
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_net_prov(g_res.module, inverted).v);
    TEST_ASSERT_EQUAL_STRING("dfflibmap: library 'cmos6': 2 cells mapped (DFF x2), 1 clock "
                             "inverters",
                             g_info);
}

static void test_pass_no_match_changes_nothing(void) {
    build(TOP_PORTS "  cell $_DFF_P_ \\f\n    parameter \\INIT 3\n    connect \\C \\clk\n"
                    "    connect \\D \\d\n    connect \\Q \\q\n  end\n"
                    "  cell $_DLATCH_P_ \\l\n    parameter \\INIT 3\n    connect \\E \\clk\n"
                    "    connect \\D \\e\n    connect \\Q \\r\n  end\nend\n",
          LIB_CMOS6);
    uint32_t live = count_live();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("cmos6"));
    TEST_ASSERT_EQUAL_UINT32(live, count_live());
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_DFF_P_"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("DFF"));
    TEST_ASSERT_EQUAL_STRING("dfflibmap: module 'top': no cell of library 'cmos6' implements "
                             "$_DLATCH_P_ 'l' (high, INIT 3)",
                             g_msg);
}

static void test_pass_refuses_other_state_type(void) {
    build(TOP_PORTS
          "  cell $dff \\w\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
          "    connect \\CLK \\clk\n    connect \\D \\d\n    connect \\Q \\q\n  end\nend\n",
          LIB_GENERIC);
    uint32_t live = count_live();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("generic_gates"));
    TEST_ASSERT_EQUAL_UINT32(live, count_live());
    TEST_ASSERT_EQUAL_STRING("dfflibmap: module 'top': cell 'w' of type $dff is a flip-flop or "
                             "latch dfflibmap cannot map (only $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and "
                             "$_DLATCH_N_)",
                             g_msg);
}

static void test_pass_refuses_global_clock_ff(void) {
    build(TOP_PORTS "  cell $_FF_ \\g\n    parameter \\INIT 0\n    connect \\D \\d\n"
                    "    connect \\Q \\q\n  end\nend\n",
          LIB_GENERIC);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("generic_gates"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "cell 'g' of type $_FF_ is a flip-flop or latch"));
}

static void test_pass_library_not_loaded(void) {
    build(TOP_PORTS "end\n", LIB_GENERIC);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("cmos6"));
    TEST_ASSERT_EQUAL_STRING(
        "dfflibmap: library 'cmos6' is not loaded; read it with read_techlib first", g_msg);
}

static void test_pass_init_lost_warns(void) {
    build(TOP_PORTS "  cell $_DFF_P_ \\one\n    parameter \\INIT 1\n    connect \\C \\clk\n"
                    "    connect \\D \\d\n    connect \\Q \\q\n  end\n"
                    "  cell $_DLATCH_N_ \\lat\n    parameter \\INIT 0\n    connect \\E \\clk\n"
                    "    connect \\D \\e\n    connect \\Q \\r\n  end\nend\n",
          LIB_GENERIC);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_pass("generic_gates"), g_msg);
    TEST_ASSERT_EQUAL_STRING("DFFP", node_type_name(node_named("one")));
    TEST_ASSERT_EQUAL_STRING("DLATCHN", node_type_name(node_named("lat")));
    TEST_ASSERT_EQUAL_STRING("dfflibmap: 2 flip-flops or latches with INIT 0 or 1 became cells "
                             "whose init is x; their initial value is lost",
                             g_warn);
    TEST_ASSERT_EQUAL_STRING("dfflibmap: library 'generic_gates': 2 cells mapped (DFFP x1, "
                             "DLATCHN x1), 0 clock inverters",
                             g_info);
}

static void test_pass_args(void) {
    build(TOP_PORTS "end\n", LIB_GENERIC);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass(""));
    TEST_ASSERT_EQUAL_STRING("dfflibmap: expects one library name", g_msg);
    g_msg[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass("generic_gates cmos6"));
    TEST_ASSERT_EQUAL_STRING("dfflibmap: expects one library name", g_msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("generic_gates"));
    /* No cell to map: no list. */
    TEST_ASSERT_EQUAL_STRING(
        "dfflibmap: library 'generic_gates': 0 cells mapped, 0 clock inverters", g_info);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_choose_posedge_generic);
    RUN_TEST(test_choose_area_tie_break);
    RUN_TEST(test_choose_init_rules);
    RUN_TEST(test_choose_negedge_fallback_inverts);
    RUN_TEST(test_choose_no_inverter_no_fallback);
    RUN_TEST(test_choose_no_latch_in_cmos6);
    RUN_TEST(test_pass_maps_and_keeps_name_and_prov);
    RUN_TEST(test_pass_shares_clock_inverter);
    RUN_TEST(test_pass_no_match_changes_nothing);
    RUN_TEST(test_pass_refuses_other_state_type);
    RUN_TEST(test_pass_refuses_global_clock_ff);
    RUN_TEST(test_pass_library_not_loaded);
    RUN_TEST(test_pass_init_lost_warns);
    RUN_TEST(test_pass_args);
    return UNITY_END();
}
