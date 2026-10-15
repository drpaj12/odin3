/*
 * test_abc_splice.c — the splice (3D plan Task 17; spec §6.5, §5.4, AB-13, AB-18): phase B's table
 * (every row, both columns), the new flip-flops, folding, the init-1 restore, names, cleanup and
 * the failures. Every case builds a small module through the IR API, runs the abc pass on the
 * fake engine, and checks the structure, `odin3_check_design` FULL and (where the module can be
 * simulated) that 1E simulation, 256 vectors on each of three seeds, is equal before and after.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "abc_sim.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/exchange.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/abc/splice.h"
#include "passes/manager.h"
#include "util/alloc.h"

static odin3_design *design;
static odin3_module *mod;
static asim_trace before;
static bool traced;

enum { OOM_LIMIT = 4000 };

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    odin3_pass_set_options((odin3_pass_options){0});
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    before = (asim_trace){0};
    traced = false;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_run_limits(NULL);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    asim_free(&before);
    odin3_design_destroy(design);
}

/* --- building ------------------------------------------------------------------------------ */

static odin3_net_id input(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_IN);
}

static odin3_net_id output(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_OUT);
}

static odin3_net_id wire_net(const char *name) {
    return afx_net(design, mod, name);
}

static odin3_node_id cell1(const char *type, odin3_net_id a, odin3_net_id y) {
    odin3_net_id nets[2] = {a, y};
    return AFX_CELL(design, mod, type, NULL, nets, 2);
}

static odin3_node_id cell2(const char *type, odin3_net_id a, odin3_net_id b, odin3_net_id y) {
    odin3_net_id nets[3] = {a, b, y};
    return AFX_CELL(design, mod, type, NULL, nets, 3);
}

/* A global-clock flip-flop with the given INIT and name. */
static odin3_node_id ff_global(const char *name, int64_t init, odin3_net_id dnet,
                               odin3_net_id qnet) {
    odin3_value params[1] = {odin3_value_int(init)};
    odin3_net_id nets[2] = {dnet, qnet};
    return afx_make(design, mod, &(afx_cell_spec){"$_FF_", name, params, nets, 2});
}

static odin3_node_id ff_edge(const char *name, int64_t init, odin3_net_id clk, odin3_net_id dnet,
                             odin3_net_id qnet) {
    odin3_value params[1] = {odin3_value_int(init)};
    odin3_net_id nets[3] = {clk, dnet, qnet};
    return afx_make(design, mod, &(afx_cell_spec){"$_DFF_P_", name, params, nets, 3});
}

/* A level latch: a cut cell that reads dnet, so dnet is an ordinary CO. */
static void latch_reader(odin3_net_id enable, odin3_net_id dnet, odin3_net_id qnet) {
    odin3_net_id nets[3] = {enable, dnet, qnet};
    (void)AFX_CELL(design, mod, "$_DLATCH_P_", NULL, nets, 3);
}

static odin3_net_id const_net(const char *type, const char *name) {
    odin3_net_id net = wire_net(name);
    (void)AFX_CELL(design, mod, type, NULL, &net, 1);
    return net;
}

/* --- running and reading ------------------------------------------------------------------- */

static void trace_before(void) {
    asim_record(design, &before);
    traced = true;
}

static odin3_status run_abc(void) {
    return odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6"));
}

/* The pass succeeded, the IR is valid and (when traced) simulation is as before. */
static void expect_clean(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc(), afx_errors());
    afx_check(design);
    if (traced) {
        asim_expect_equal(design, &before);
    }
}

static const char *type_name(odin3_node_id node) {
    return odin3_celltype_get(design, odin3_node_type(mod, node))->name;
}

static uint32_t count_type(const char *name) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        found += odin3_node_live(mod, node) && strcmp(type_name(node), name) == 0 ? 1U : 0U;
    }
    return found;
}

static odin3_node_id first_of(const char *name) {
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && strcmp(type_name(node), name) == 0) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static odin3_node_id driver_node(odin3_net_id net) {
    TEST_ASSERT_TRUE(odin3_net_live(mod, net));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, net));
    return odin3_pin_node(mod, odin3_net_driver(mod, net));
}

static const char *driver_type(odin3_net_id net) {
    return type_name(driver_node(net));
}

/* The net on pin 0 of port `port` of node. */
static odin3_net_id port_net(odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    TEST_ASSERT_EQUAL_UINT32(1, pins.count);
    return odin3_pin_net(mod, pins.first);
}

/* The net of the module port wire called name (a port's net follows merges). */
static odin3_net_id port_wire_net(const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(mod, afx_intern(design, name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(mod, wire, 0);
}

static odin3_net_id named_net(const char *name) {
    odin3_net_id net = odin3_module_find_net(mod, afx_intern(design, name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), name);
    return net;
}

static int64_t init_of(odin3_node_id ff) {
    return odin3_node_param(mod, ff, 0)->i;
}

static uint32_t lut_width(odin3_node_id lut) {
    return (uint32_t)odin3_node_param(mod, lut, ODIN3_LUT_P_WIDTH)->i;
}

static bool lut_bit(odin3_node_id lut, uint32_t idx) {
    return odin3_node_param(mod, lut, ODIN3_LUT_P_LUT)->bits[idx] == ODIN3_BIT_1;
}

/* --- phase B: both columns of the table ---------------------------------------------------- */

static void test_fresh_net(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id y = output("y");
    (void)cell2("$_AND_", a, b, y);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_STRING("$lut", driver_type(y));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_AND_"));
    odin3_node_id lut = driver_node(y);
    TEST_ASSERT_EQUAL_UINT32(2, lut_width(lut));
    TEST_ASSERT_TRUE(lut_bit(lut, 3));
    TEST_ASSERT_FALSE(lut_bit(lut, 1));
    TEST_ASSERT_EQUAL_UINT32(y.v, port_wire_net("y").v);
}

static void test_shared_driver(void) {
    odin3_net_id a = input("a");
    odin3_net_id en = input("en");
    odin3_net_id q1 = wire_net("q1");
    odin3_net_id q2 = wire_net("q2");
    (void)ff_global("f1", 0, a, q1);
    (void)ff_global("f2", 0, a, q2);
    latch_reader(en, q1, output("o1"));
    latch_reader(en, q2, output("o2"));
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_FF_"));
    TEST_ASSERT_EQUAL_UINT32(named_net("q1").v, named_net("q2").v);
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_UINT32(named_net("q1").v, port_net(ff, 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
}

static void test_special_port_pair_buffered(void) {
    odin3_net_id a = input("a");
    odin3_net_id o1 = output("o1");
    odin3_net_id o2 = output("o2");
    (void)ff_global("f1", 0, a, o1);
    (void)ff_global("f2", 0, a, o2);
    trace_before();
    expect_clean();
    TEST_ASSERT_TRUE(o1.v != o2.v);
    TEST_ASSERT_EQUAL_UINT32(o1.v, port_wire_net("o1").v);
    TEST_ASSERT_EQUAL_UINT32(o2.v, port_wire_net("o2").v);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_FF_"));
    TEST_ASSERT_EQUAL_STRING("$_FF_", driver_type(o1));
    TEST_ASSERT_EQUAL_STRING("$lut", driver_type(o2));
    odin3_node_id buf = driver_node(o2);
    TEST_ASSERT_EQUAL_UINT32(1, lut_width(buf));
    TEST_ASSERT_FALSE(lut_bit(buf, 0));
    TEST_ASSERT_TRUE(lut_bit(buf, 1));
    TEST_ASSERT_EQUAL_UINT32(o1.v, port_net(buf, ODIN3_LUT_PORT_A).v);
}

static void test_ci_ordinary(void) {
    odin3_net_id a = input("a");
    odin3_net_id en = input("en");
    odin3_net_id n = wire_net("n");
    (void)cell2("$_AND_", a, a, n);
    latch_reader(en, n, output("o"));
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(a.v, named_net("n").v);
    TEST_ASSERT_EQUAL_UINT32(a.v, port_net(first_of("$_DLATCH_P_"), 1).v);
}

static void test_ci_special(void) {
    odin3_net_id a = input("a");
    odin3_net_id y = output("y");
    (void)cell2("$_AND_", a, a, y);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(y.v, port_wire_net("y").v);
    odin3_node_id buf = driver_node(y);
    TEST_ASSERT_EQUAL_UINT32(1, lut_width(buf));
    TEST_ASSERT_EQUAL_UINT32(a.v, port_net(buf, ODIN3_LUT_PORT_A).v);
}

static void test_const_ordinary(void) {
    odin3_net_id a = input("a");
    odin3_net_id en = input("en");
    odin3_net_id one = const_net("$_CONST1_", "one");
    odin3_net_id n = wire_net("n");
    (void)cell2("$_OR_", a, one, n);
    latch_reader(en, n, output("o"));
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST1_"));
    TEST_ASSERT_EQUAL_UINT32(one.v, named_net("n").v);
    TEST_ASSERT_EQUAL_UINT32(one.v, port_net(first_of("$_DLATCH_P_"), 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
}

/* No constant generator in the module: a, !a and OR give constant 1, so a new one is made. */
static void test_const_ordinary_makes_generator(void) {
    odin3_net_id a = input("a");
    odin3_net_id en = input("en");
    odin3_net_id na = wire_net("na");
    odin3_net_id n = wire_net("n");
    (void)cell1("$_NOT_", a, na);
    (void)cell2("$_OR_", a, na, n);
    latch_reader(en, n, output("o"));
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST1_"));
    TEST_ASSERT_EQUAL_STRING("$_CONST1_", driver_type(named_net("n")));
}

static void test_const_port(void) {
    odin3_net_id a = input("a");
    odin3_net_id na = wire_net("na");
    odin3_net_id y = output("y");
    (void)cell1("$_NOT_", a, na);
    (void)cell2("$_AND_", a, na, y);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST0_"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_STRING("$_CONST0_", driver_type(port_wire_net("y")));
}

static void test_constant_to_clock_buffered(void) {
    odin3_net_id clk = input("clk");
    odin3_net_id a = input("a");
    odin3_net_id nclk = wire_net("nclk");
    odin3_net_id clk2 = wire_net("clk2");
    odin3_net_id y = output("y");
    (void)cell1("$_NOT_", clk, nclk);
    (void)cell2("$_AND_", clk, nclk, clk2);
    (void)ff_edge("r", 0, clk2, a, y);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_abc());
    afx_check(design);
    TEST_ASSERT_TRUE(odin3_net_live(mod, clk2));
    TEST_ASSERT_EQUAL_STRING("$lut", driver_type(clk2));
    odin3_node_id buf = driver_node(clk2);
    TEST_ASSERT_EQUAL_UINT32(1, lut_width(buf));
    TEST_ASSERT_EQUAL_STRING("$_CONST0_", driver_type(port_net(buf, ODIN3_LUT_PORT_A)));
    odin3_node_id kept = odin3_module_find_node(mod, afx_intern(design, "r"));
    TEST_ASSERT_TRUE(odin3_node_valid(kept));
    TEST_ASSERT_EQUAL_UINT32(clk2.v, port_net(kept, 0).v);
}

static void test_derived_clock_buffered(void) {
    odin3_net_id clk = input("clk");
    odin3_net_id en = input("en");
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id clk2 = wire_net("clk2");
    (void)cell2("$_AND_", clk, en, clk2);
    (void)ff_edge("r1", 0, clk, a, output("q1"));
    (void)ff_edge("r2", 0, clk2, b, output("q2"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_abc());
    afx_check(design);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_abc_fake_open_count());
    TEST_ASSERT_TRUE(odin3_net_live(mod, clk2));
    TEST_ASSERT_EQUAL_STRING("$lut", driver_type(clk2));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$_DFF_P_"));
    odin3_node_id r2 = odin3_module_find_node(mod, afx_intern(design, "r2"));
    TEST_ASSERT_TRUE(odin3_node_valid(r2));
    TEST_ASSERT_EQUAL_UINT32(clk2.v, port_net(r2, 0).v);
}

/* --- flip-flops ---------------------------------------------------------------------------- */

static void expect_new_flop(int64_t init, int64_t want) {
    odin3_net_id clk = input("clk");
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id n = wire_net("n");
    odin3_net_id qn = wire_net("qn");
    (void)cell2("$_AND_", a, b, n);
    (void)ff_edge("r0", init, clk, n, qn);
    (void)cell1("$_NOT_", qn, output("y"));
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_DFF_P_"));
    odin3_node_id ff = first_of("$_DFF_P_");
    TEST_ASSERT_EQUAL_INT64(want, init_of(ff));
    TEST_ASSERT_EQUAL_UINT32(afx_intern(design, "r0"), odin3_node_name(mod, ff));
    TEST_ASSERT_EQUAL_UINT32(clk.v, port_net(ff, 0).v);
    TEST_ASSERT_EQUAL_UINT32(named_net("qn").v, port_net(ff, 2).v);
    TEST_ASSERT_EQUAL_STRING("$lut", driver_type(port_net(ff, 1)));
}

static void test_flop_init_zero(void) {
    expect_new_flop(0, 0);
}

static void test_flop_init_two(void) {
    expect_new_flop(2, 2);
}

static void test_flop_init_three_becomes_two(void) {
    expect_new_flop(3, 2);
}

/* --- folding ------------------------------------------------------------------------------- */

static void test_folding_const_lut(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id y = output("y");
    (void)cell2("$_AND_", a, b, y);
    odin3_abc_fake_extra("const 0");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_abc());
    afx_check(design);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST0_"));
    TEST_ASSERT_EQUAL_STRING("$_CONST0_", driver_type(port_wire_net("y")));
}

/* --- the init-1 restore -------------------------------------------------------------------- */

static void test_restore_flip_reader(void) {
    odin3_net_id x = input("x");
    odin3_net_id a = input("a");
    odin3_net_id q = wire_net("q");
    odin3_net_id y = output("y");
    (void)ff_global("r", 1, x, q);
    (void)cell2("$_AND_", q, a, y);
    trace_before();
    expect_clean();
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_INT64(1, init_of(ff));
    TEST_ASSERT_EQUAL_UINT32(x.v, port_net(ff, 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(2, lut_width(driver_node(y)));
}

static void test_restore_inverter_to_merge(void) {
    odin3_net_id x = input("x");
    odin3_net_id y = output("y");
    (void)ff_global("r", 1, x, y);
    trace_before();
    expect_clean();
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_INT64(1, init_of(ff));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(y.v, port_net(ff, 1).v);
    TEST_ASSERT_EQUAL_UINT32(x.v, port_net(ff, 0).v);
}

static void test_restore_direct_reader_not(void) {
    odin3_net_id x = input("x");
    odin3_net_id q = wire_net("q");
    odin3_net_id y = output("y");
    (void)ff_global("r", 1, x, q);
    (void)cell1("$_NOT_", q, y);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    odin3_node_id not_lut = driver_node(y);
    TEST_ASSERT_EQUAL_UINT32(1, lut_width(not_lut));
    TEST_ASSERT_TRUE(lut_bit(not_lut, 0));
    TEST_ASSERT_FALSE(lut_bit(not_lut, 1));
    TEST_ASSERT_EQUAL_INT64(1, init_of(first_of("$_FF_")));
}

static void test_restore_shared_d_not(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id n = output("n");
    odin3_net_id m = wire_net("m");
    (void)cell2("$_AND_", a, b, n);
    (void)cell1("$_NOT_", n, m);
    (void)ff_global("r", 1, m, output("z"));
    trace_before();
    expect_clean();
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_INT64(1, init_of(ff));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$lut"));
    odin3_node_id not_lut = driver_node(port_net(ff, 0));
    TEST_ASSERT_EQUAL_UINT32(1, lut_width(not_lut));
    TEST_ASSERT_TRUE(lut_bit(not_lut, 0));
    TEST_ASSERT_EQUAL_UINT32(n.v, port_net(not_lut, ODIN3_LUT_PORT_A).v);
}

static void test_restore_d_only(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id d = wire_net("d");
    (void)cell2("$_AND_", a, b, d);
    (void)ff_global("r", 1, d, output("z"));
    trace_before();
    expect_clean();
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    odin3_node_id src = driver_node(port_net(ff, 0));
    TEST_ASSERT_EQUAL_UINT32(2, lut_width(src));
    TEST_ASSERT_TRUE(lut_bit(src, 3));
    TEST_ASSERT_FALSE(lut_bit(src, 0));
}

static void test_restore_const_d(void) {
    odin3_net_id zero = const_net("$_CONST0_", "zero");
    (void)ff_global("r", 1, zero, output("z"));
    trace_before();
    expect_clean();
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(zero.v, port_net(ff, 0).v);
    TEST_ASSERT_EQUAL_INT64(1, init_of(ff));
}

/* Two INIT 1 registers on one D that scorr merges: one flip-flop, INIT 1, both ports driven. */
static void test_restore_merged_pair(void) {
    odin3_net_id x = input("x");
    odin3_net_id z1 = output("z1");
    odin3_net_id z2 = output("z2");
    (void)ff_global("r1", 1, x, z1);
    (void)ff_global("r2", 1, x, z2);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_FF_"));
    TEST_ASSERT_EQUAL_INT64(1, init_of(first_of("$_FF_")));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(z1.v, port_wire_net("z1").v);
    TEST_ASSERT_EQUAL_UINT32(z2.v, port_wire_net("z2").v);
}

/* --- names and cleanup --------------------------------------------------------------------- */

static void test_names_survive(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id y = output("y");
    odin3_net_id q = wire_net("q");
    odin3_net_id q_alias = wire_net("q_old");
    (void)cell2("$_AND_", a, b, q);
    (void)ff_global("r", 0, q, q_alias);
    (void)cell1("$_BUF_", q_alias, y);
    trace_before();
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(y.v, port_wire_net("y").v);
    odin3_node_id ff = first_of("$_FF_");
    TEST_ASSERT_EQUAL_UINT32(afx_intern(design, "r"), odin3_node_name(mod, ff));
}

static void test_cleanup_nets_and_wires(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id c = input("c");
    odin3_net_id mid = wire_net("mid");
    odin3_net_id y = output("y");
    (void)cell2("$_AND_", a, b, mid);
    (void)cell2("$_AND_", mid, c, y);
    odin3_wire_spec spec = {afx_intern(design, "w_mid"), 0, 0, false, (odin3_prov_id){0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, &mid, &wire));
    trace_before();
    expect_clean();
    TEST_ASSERT_FALSE(odin3_wire_valid(odin3_module_find_wire(mod, afx_intern(design, "w_mid"))));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$lut"));
    for (uint32_t id = 1; id < odin3_module_net_end(mod); id++) {
        odin3_net_id net = {id};
        if (odin3_net_live(mod, net)) {
            TEST_ASSERT_TRUE_MESSAGE(odin3_net_pins(mod, net).count > 0 ||
                                         odin3_wire_valid(odin3_net_primary(mod, net).wire),
                                     "a pinless net was left behind");
        }
    }
}

/* --- failures ------------------------------------------------------------------------------ */

/* Everything the run needs up to the splice, for the direct calls below. */
typedef struct staged {
    odin3_pass_ctx ctx;
    odin3_abc_run *run;
    odin3_vec domains;
    odin3_abc_region region;
    odin3_abc_result result;
} staged;

static void build_oom_module(void) {
    odin3_net_id a = input("a");
    odin3_net_id b = input("b");
    odin3_net_id n = output("n");
    odin3_net_id m = wire_net("m");
    (void)cell2("$_AND_", a, b, n);
    (void)cell1("$_NOT_", n, m);
    (void)ff_global("r", 1, m, output("z"));
    (void)ff_global("r2", 0, a, output("z2"));
}

static void stage(staged *st) {
    *st = (staged){0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(design, afx_intern(design, "abc"), &st->ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_abc_run_start(&st->ctx, odin3_bytes_cstr("--lut 6"), &st->run));
    st->run->module = mod;
    st->run->module_id = odin3_design_top(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(st->run, mod, &st->domains));
    st->run->domain = st->domains.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_abc_region_build(st->run, st->run->domain, &st->region));
    st->run->region = &st->region;
    odin3_abc_result_init(&st->result);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_exchange_blif(st->run, &st->result));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&st->result));
}

static void unstage(staged *st) {
    odin3_abc_result_free(&st->result);
    odin3_abc_region_free(&st->region);
    odin3_abc_domains_free(&st->domains);
    odin3_abc_run_destroy(st->run);
}

/* One splice with the n-th allocation failing; returns its status. */
static odin3_status splice_failing_at(long nth, char **text_before) {
    staged st;
    build_oom_module();
    stage(&st);
    *text_before = afx_blif_text(design);
    odin3_abc_splice_stats stats;
    odin3_util_set_alloc_fail_after(nth);
    odin3_status res = odin3_abc_splice(st.run, &st.region, &st.result, &stats);
    odin3_util_set_alloc_fail_after(-1);
    unstage(&st);
    return res;
}

static void new_module(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
}

/*
 * The sweep: every allocation of one splice fails in turn. An out-of-memory splice leaves a valid
 * IR that simulates as before; one that failed at its first allocation (phase A) leaves write_blif
 * byte-identical.
 */
static void test_oom_sweep(void) {
    build_oom_module();
    trace_before();
    long ok_after = -1;
    for (long nth = 0; nth < OOM_LIMIT && ok_after < 0; nth++) {
        new_module();
        char *text = NULL;
        odin3_status res = splice_failing_at(nth, &text);
        afx_check(design);
        if (res == ODIN3_OK) {
            ok_after = nth;
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, res, "an allocation failure");
        }
        asim_expect_equal(design, &before);
        if (nth == 0) {
            char *now = afx_blif_text(design);
            TEST_ASSERT_EQUAL_STRING(text, now);
            free(now);
        }
        free(text);
    }
    TEST_ASSERT_TRUE_MESSAGE(ok_after > 0, "the sweep never reached a success");
}

static void test_invalid_arguments(void) {
    odin3_abc_result res;
    odin3_abc_result_init(&res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_splice(NULL, NULL, &res, NULL));
    odin3_abc_result_free(&res);
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_canonical_const(NULL, 0, &net));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_net);
    RUN_TEST(test_shared_driver);
    RUN_TEST(test_special_port_pair_buffered);
    RUN_TEST(test_ci_ordinary);
    RUN_TEST(test_ci_special);
    RUN_TEST(test_const_ordinary);
    RUN_TEST(test_const_ordinary_makes_generator);
    RUN_TEST(test_const_port);
    RUN_TEST(test_constant_to_clock_buffered);
    RUN_TEST(test_derived_clock_buffered);
    RUN_TEST(test_flop_init_zero);
    RUN_TEST(test_flop_init_two);
    RUN_TEST(test_flop_init_three_becomes_two);
    RUN_TEST(test_folding_const_lut);
    RUN_TEST(test_restore_flip_reader);
    RUN_TEST(test_restore_inverter_to_merge);
    RUN_TEST(test_restore_direct_reader_not);
    RUN_TEST(test_restore_shared_d_not);
    RUN_TEST(test_restore_d_only);
    RUN_TEST(test_restore_const_d);
    RUN_TEST(test_restore_merged_pair);
    RUN_TEST(test_names_survive);
    RUN_TEST(test_cleanup_nets_and_wires);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_invalid_arguments);
    return UNITY_END();
}
