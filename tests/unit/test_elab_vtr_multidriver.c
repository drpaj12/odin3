/*
 * test_elab_vtr_multidriver.c — the VTR-compat ruling on several tri-state drivers of one internal
 * net (Peter, 2026-10-14; PHASE2 decision 27; 2D spec 6.7, 17 #79): with `read_verilog
 * --vtr-compat` the net is wired to the one driver Yosys 0.55 / Parmys keeps (the enables are
 * ignored), with a warning that names the net, the driver kept, the drivers dropped and the IEEE
 * reading; without the option the drivers are one $pmux as before (`tribuf -logic`). Every
 * source goes through the whole read_verilog pass (proc_test.h).
 */
#include "elab_test.h"
#include "proc_test.h"

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef ODIN3_ELAB_TMP
#error "ODIN3_ELAB_TMP must name the scratch directory"
#endif

void setUp(void) {
    memset(&pt_rs, 0, sizeof pt_rs);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    if (pt_rs.design != NULL || pt_rs.dir[0] != '\0') {
        et_free(&pt_rs);
    }
}

static const char k_vtr[] = "--vtr-compat t.v";
static const char k_plain[] = "t.v";

/* The rule, as the warning spells it. */
#define RULE                                                                                       \
    "(the input Yosys keeps as the net's representative: one other logic reads, else the lowest "  \
    "name), ignores every enable and drops "
#define IEEE                                                                                       \
    " (IEEE resolution would be a bus selected by the enables: the data of the driver that is "    \
    "enabled, undefined for several)"

static void read_ok_with(const char *args, const char *text) {
    pass_src src = {args, text};
    pt_read_with(&src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pt_rs.status, et_log(&pt_rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pt_diag_errors(), et_log(&pt_rs));
    pt_expect_ir_ok();
}

static bool same_net(const char *wire_a, uint32_t bit_a, const char *wire_b, uint32_t bit_b) {
    return pt_wnet(wire_a, bit_a).v == pt_wnet(wire_b, bit_b).v;
}

/* No tri-state cell and no bus is left. */
static void expect_no_bus(void) {
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$pmux"));
}

#define MICRO_HEAD                                                                                 \
    "module t(busout, bus0, bus1, bus2, bus3, enable, s);\n"                                       \
    "output [3:0] busout;\ninput [3:0] bus0, bus1, bus2, bus3;\ninput enable;\ninput [1:0] s;\n"   \
    "wire [3:0] data;\n"
#define MICRO_TAIL "assign busout = enable ? data : 4'hf;\nendmodule\n"

/* The micro syntax/multi_assignment: data is bus0, the other three inputs and s are unused. */
static void test_vtr_micro(void) {
    read_ok_with(k_vtr, MICRO_HEAD "assign data = (s == 0) ? bus0 : 4'hz;\n"
                                   "assign data = (s == 1) ? bus1 : 4'hz;\n"
                                   "assign data = (s == 2) ? bus2 : 4'hz;\n"
                                   "assign data = (s == 3) ? bus3 : 4'hz;\n" MICRO_TAIL);
    pt_expect_warning((at_loc){7, 8},
                      "net 'data' has 4 drivers; --vtr-compat keeps only the data of 'bus0' as "
                      "Parmys does " RULE "'bus1', 'bus2', 'bus3'" IEEE);
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    expect_no_bus();
    for (uint32_t bit = 0; bit < 4; bit++) {
        TEST_ASSERT_TRUE(same_net("data", bit, "bus0", bit));
        TEST_ASSERT_FALSE(same_net("data", bit, "bus1", bit));
    }
}

/* Without the option the same source is the IEEE bus (one $pmux, four selects) and the old warning
 * only. */
static void test_plain_micro_is_a_bus(void) {
    read_ok_with(k_plain, MICRO_HEAD "assign data = (s == 0) ? bus0 : 4'hz;\n"
                                     "assign data = (s == 1) ? bus1 : 4'hz;\n"
                                     "assign data = (s == 2) ? bus2 : 4'hz;\n"
                                     "assign data = (s == 3) ? bus3 : 4'hz;\n" MICRO_TAIL);
    pt_expect_warning((at_loc){7, 8},
                      "tri-state drivers on internal net 'data' converted to logic (no driver "
                      "enabled: x; several enabled: undefined)");
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$pmux"));
    TEST_ASSERT_FALSE(same_net("data", 0, "bus0", 0));
}

/* The order of the assigns does not matter: the lowest name wins, whichever comes first. */
static void test_vtr_order_does_not_matter(void) {
    read_ok_with(k_vtr, MICRO_HEAD "assign data = (s == 3) ? bus3 : 4'hz;\n"
                                   "assign data = (s == 1) ? bus1 : 4'hz;\n"
                                   "assign data = (s == 0) ? bus0 : 4'hz;\n" MICRO_TAIL);
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'data' has 3 drivers; --vtr-compat keeps "
                                                 "only the data of 'bus0'"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("drops 'bus3', 'bus1' (IEEE"));
    expect_no_bus();
    TEST_ASSERT_TRUE(same_net("data", 2, "bus0", 2));
}

/* By name, not by position in the port list: zz is declared first and loses to aa. */
static void test_vtr_lowest_name_wins(void) {
    read_ok_with(k_vtr, "module t(zz, aa, e0, e1, o);\ninput zz, aa, e0, e1;\noutput o;\nwire d;\n"
                        "assign d = e0 ? zz : 1'bz;\nassign d = e1 ? aa : 1'bz;\nassign o = ~d;\n"
                        "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("keeps only the data of 'aa'"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("drops 'zz' (IEEE"));
    TEST_ASSERT_TRUE(same_net("d", 0, "aa", 0));
    TEST_ASSERT_FALSE(same_net("d", 0, "zz", 0));
    expect_no_bus();
}

/* An input another cell reads is kept ahead of a lower name (opt_clean's connected signals). */
static void test_vtr_input_read_by_logic_wins(void) {
    read_ok_with(k_vtr, "module t(aa, bb, cc, e0, e1, e2, o, p);\ninput aa, bb, cc, e0, e1, e2;\n"
                        "output o, p;\nwire d;\nassign d = e0 ? aa : 1'bz;\n"
                        "assign d = e1 ? bb : 1'bz;\nassign d = e2 ? cc : 1'bz;\n"
                        "assign o = ~d;\nassign p = ~bb;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'd' has 3 drivers; --vtr-compat keeps only "
                                                 "the data of 'bb'"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("drops 'aa', 'cc' (IEEE"));
    TEST_ASSERT_TRUE(same_net("d", 0, "bb", 0));
    expect_no_bus();
}

/* Two bits of one input: the later bit of the equal wire is kept (x[1] of x[0] and x[1]). */
static void test_vtr_same_wire_highest_bit(void) {
    read_ok_with(k_vtr, "module t(x, e0, e1, o);\ninput [1:0] x;\ninput e0, e1;\noutput o;\n"
                        "wire d;\nassign d = e1 ? x[1] : 1'bz;\nassign d = e0 ? x[0] : 1'bz;\n"
                        "assign o = ~d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'd' has 2 drivers; --vtr-compat keeps only "
                                                 "the data of 'x' as Parmys does"));
    TEST_ASSERT_TRUE(same_net("d", 0, "x", 1));
    TEST_ASSERT_FALSE(same_net("d", 0, "x", 0));
    expect_no_bus();
}

/* Every bit picks for itself: b[1] is read by logic, so bit 1 follows b and bit 0 the lower name a.
 */
static void test_vtr_per_bit(void) {
    read_ok_with(k_vtr, "module t(a, b, e0, e1, o, p);\ninput [1:0] a, b;\ninput e0, e1;\n"
                        "output [1:0] o;\noutput p;\nwire [1:0] d;\n"
                        "assign d = e0 ? a : 2'bz;\nassign d = e1 ? b : 2'bz;\n"
                        "assign o = ~d;\nassign p = ~b[1];\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'd' has 2 drivers; --vtr-compat keeps only "
                                                 "the data of 'a', 'b'"));
    TEST_ASSERT_TRUE(same_net("d", 0, "a", 0));
    TEST_ASSERT_TRUE(same_net("d", 1, "b", 1));
    expect_no_bus();
}

/* Two drivers with the same data: one winner, nothing dropped. */
static void test_vtr_same_data_twice(void) {
    read_ok_with(k_vtr, "module t(a, e0, e1, o);\ninput a, e0, e1;\noutput o;\nwire d;\n"
                        "assign d = e0 ? a : 1'bz;\nassign d = e1 ? a : 1'bz;\nassign o = ~d;\n"
                        "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("keeps only the data of 'a' as Parmys does"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("and drops none (IEEE"));
    TEST_ASSERT_TRUE(same_net("d", 0, "a", 0));
    expect_no_bus();
}

/* A driver that is a cell output is not an input: Parmys leaves several drivers on the net, which
 * the IR cannot hold, so the IEEE bus stays under the option too, with the old warning. */
static void test_vtr_cell_data_keeps_the_bus(void) {
    read_ok_with(k_vtr, "module t(a, b, e0, e1, o);\ninput a, b, e0, e1;\noutput o;\nwire d;\n"
                        "assign d = e0 ? (a & b) : 1'bz;\nassign d = e1 ? b : 1'bz;\n"
                        "assign o = ~d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("converted to logic"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$pmux"));
}

/* One driver is the same data whichever way: the bus (a $pmux of one select) as before. */
static void test_vtr_single_driver_unchanged(void) {
    read_ok_with(k_vtr, "module t(a, e0, o);\ninput a, e0;\noutput o;\nwire d;\n"
                        "assign d = e0 ? a : 1'bz;\nassign o = ~d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("converted to logic"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$pmux"));
}

/* Two drivers on one top-level output keep their $tribuf cells under the option: decision 27 is for
 * internal nets and decision 28 for a lone driver. */
static void test_vtr_output_port_keeps_tribuf(void) {
    read_ok_with(k_vtr, "module t(a, b, e0, e1, o);\ninput a, b, e0, e1;\noutput o;\n"
                        "assign o = e0 ? a : 1'bz;\nassign o = e1 ? b : 1'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$tribuf"));
}

/* Probe h1: Parmys flattens first, so below the top its choice ranks the parent's wires. The rule
 * is for the top module only; a submodule keeps the IEEE bus and a warning says Parmys may differ.
 */
static void test_vtr_submodule_keeps_the_bus(void) {
    read_ok_with(k_vtr, "module sub(bus0, bus1, e0, e1, o);\ninput bus0, bus1, e0, e1;\n"
                        "output o;\nwire d;\nassign d = e0 ? bus0 : 1'bz;\n"
                        "assign d = e1 ? bus1 : 1'bz;\nassign o = ~d;\nendmodule\n"
                        "module t(zz, aa, e0, e1, o);\ninput zz, aa, e0, e1;\noutput o;\n"
                        "sub u(.bus0(zz), .bus1(aa), .e0(e0), .e1(e1), .o(o));\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'd' is multiply driven"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("Parmys's result may differ"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("converted to logic"));
}

/* Probe t1: two candidates (bb, cc) that other logic reads. Parmys shorts the loser to the winner
 * everywhere; we do not model that, so the rule declines: IEEE bus and a warning. */
static void test_vtr_second_read_candidate_declines(void) {
    read_ok_with(k_vtr, "module t(aa, bb, cc, e0, e1, e2, o, p, r);\n"
                        "input aa, bb, cc, e0, e1, e2;\noutput o, p, r;\nwire d;\n"
                        "assign d = e0 ? aa : 1'bz;\nassign d = e1 ? bb : 1'bz;\n"
                        "assign d = e2 ? cc : 1'bz;\nassign o = ~d;\nassign p = bb & e0;\n"
                        "assign r = cc | e1;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("net 'd' is multiply driven"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("Parmys's result may differ"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$pmux"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$tribuf"));
}

/* Probe s1: input aa is a candidate of two groups; Parmys puts both groups in one class. Both
 * groups decline (the first is not converted before the second is seen). */
static void test_vtr_shared_candidate_declines(void) {
    read_ok_with(k_vtr, "module t(aa, bb, cc, e0, e1, e2, e3, o, p, q);\n"
                        "input aa, bb, cc, e0, e1, e2, e3;\noutput o, p, q;\nwire d1, d2;\n"
                        "assign d1 = e0 ? aa : 1'bz;\nassign d1 = e1 ? bb : 1'bz;\n"
                        "assign d2 = e2 ? aa : 1'bz;\nassign d2 = e3 ? cc : 1'bz;\n"
                        "assign o = ~d1;\nassign p = ~d2;\nassign q = ~bb;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat keeps"));
    TEST_ASSERT_EQUAL_UINT32(2, pt_warnings_with("is multiply driven"));
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$pmux"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_count_of("$tribuf"));
}

/* Decision 28: a lone tri-state driver of a top-level output loses its enable under the option,
 * as Parmys does (`opt_expr -mux_undef`: `q = en ? d : 'bz` is `q = d`, `en` unused). */
#define OUT_WARN                                                                                   \
    "tri-state driver of output port 'q': --vtr-compat drops the enable and wires the output to "  \
    "the data as Parmys does (without the option the output is z while the enable is off)"

static void test_vtr_output_drops_enable(void) {
    read_ok_with(k_vtr, "module t(d, en, q);\ninput [3:0] d;\ninput en;\noutput [3:0] q;\n"
                        "assign q = en ? d : 4'bz;\nendmodule\n");
    pt_expect_warning((at_loc){5, 8}, OUT_WARN);
    TEST_ASSERT_EQUAL_UINT32(1, pt_soft_warnings());
    expect_no_bus();
    for (uint32_t bit = 0; bit < 4; bit++) {
        TEST_ASSERT_TRUE(same_net("q", bit, "d", bit));
    }
    TEST_ASSERT_FALSE(same_net("q", 0, "en", 0));
}

/* Without the option the output stays a $tribuf and nothing is said. */
static void test_plain_output_keeps_tribuf(void) {
    read_ok_with(k_plain, "module t(d, en, q);\ninput [3:0] d;\ninput en;\noutput [3:0] q;\n"
                          "assign q = en ? d : 4'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat"));
    TEST_ASSERT_EQUAL_UINT32(0, pt_soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$tribuf"));
    TEST_ASSERT_FALSE(same_net("q", 0, "d", 0));
}

/* bufif1, notif1 and the swapped branches are the same cell. The data of a notif is inverted, so
 * the output is wired to the inverter, not to the input. */
static void test_vtr_output_gates_and_swapped(void) {
    read_ok_with(k_vtr, "module t(a, b, c, e, p, q, r);\ninput a, b, c, e;\noutput p, q, r;\n"
                        "bufif1 (p, a, e);\nnotif1 (q, b, e);\nassign r = e ? 1'bz : c;\n"
                        "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(3, pt_warnings_with("--vtr-compat drops the enable"));
    expect_no_bus();
    TEST_ASSERT_TRUE(same_net("p", 0, "a", 0));
    TEST_ASSERT_FALSE(same_net("q", 0, "b", 0));
    TEST_ASSERT_EQUAL_UINT32(2, pt_count_of("$not")); /* b, and the enable of the swapped r */
}

/* The port is named once for a bus and the bits that are dangling are skipped; a partial driver
 * of a bus keeps working. */
static void test_vtr_output_partial_bus(void) {
    read_ok_with(k_vtr, "module t(d, en, q);\ninput [1:0] d;\ninput en;\noutput [3:0] q;\n"
                        "assign q[1:0] = en ? d : 2'bz;\nassign q[3:2] = d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, pt_warnings_with("output port 'q': --vtr-compat drops"));
    expect_no_bus();
    TEST_ASSERT_TRUE(same_net("q", 1, "d", 1));
    TEST_ASSERT_TRUE(same_net("q", 3, "d", 1));
}

/* A constant 0 enable stays a $tribuf: the output is z for good. */
static void test_vtr_output_constant_enable_declines(void) {
    read_ok_with(k_vtr, "module t(d, q);\ninput d;\noutput q;\nassign q = 1'b0 ? d : 1'bz;\n"
                        "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat drops the enable"));
}

/* An inout port keeps its $tribuf: the ruling is for outputs. */
static void test_vtr_inout_keeps_tribuf(void) {
    read_ok_with(k_vtr, "module t(d, en, io);\ninput d, en;\ninout io;\n"
                        "assign io = en ? d : 1'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat drops the enable"));
    TEST_ASSERT_EQUAL_UINT32(1, pt_count_of("$tribuf"));
}

/* Below the top the output may reach a top-level port, where z must stay: the existing error. */
static void test_vtr_submodule_output_still_errors(void) {
    pass_src src = {k_vtr, "module sub(d, en, q);\ninput d, en;\noutput q;\n"
                           "assign q = en ? d : 1'bz;\nendmodule\n"
                           "module t(d, en, q);\ninput d, en;\noutput q;\n"
                           "sub u(.d(d), .en(en), .q(q));\nendmodule\n"};
    pt_read_with(&src);
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, pt_rs.status);
    TEST_ASSERT_EQUAL_UINT32(0, pt_warnings_with("--vtr-compat drops the enable"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_vtr_micro);
    RUN_TEST(test_plain_micro_is_a_bus);
    RUN_TEST(test_vtr_order_does_not_matter);
    RUN_TEST(test_vtr_lowest_name_wins);
    RUN_TEST(test_vtr_input_read_by_logic_wins);
    RUN_TEST(test_vtr_same_wire_highest_bit);
    RUN_TEST(test_vtr_per_bit);
    RUN_TEST(test_vtr_same_data_twice);
    RUN_TEST(test_vtr_cell_data_keeps_the_bus);
    RUN_TEST(test_vtr_single_driver_unchanged);
    RUN_TEST(test_vtr_output_port_keeps_tribuf);
    RUN_TEST(test_vtr_submodule_keeps_the_bus);
    RUN_TEST(test_vtr_second_read_candidate_declines);
    RUN_TEST(test_vtr_shared_candidate_declines);
    RUN_TEST(test_vtr_output_drops_enable);
    RUN_TEST(test_plain_output_keeps_tribuf);
    RUN_TEST(test_vtr_output_gates_and_swapped);
    RUN_TEST(test_vtr_output_partial_bus);
    RUN_TEST(test_vtr_output_constant_enable_declines);
    RUN_TEST(test_vtr_inout_keeps_tribuf);
    RUN_TEST(test_vtr_submodule_output_still_errors);
    return UNITY_END();
}
