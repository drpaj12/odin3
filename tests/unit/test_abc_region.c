/*
 * test_abc_region.c — what ABC sees of a module and a clock domain (3D plan Task 15; spec §5,
 * AB-10 to AB-12): roles of every registry type and of the rule edges, constants per reader pin,
 * the bypass of hard pins on unconn, CIs, COs and their order, clock domains, loops, the cut-point
 * counts, a long chain, and an out-of-memory sweep.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "ir/cells/cells.h"
#include "ir/celltype.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "passes/manager.h"
#include "sim/cell.h"
#include "util/alloc.h"

#include <inttypes.h>
#include <time.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { CHAIN_GATES = 100000, OOM_LIMIT = 6000 };

static odin3_design *design;
static odin3_module *mod;
static odin3_pass_ctx ctx;
static odin3_abc_run *run;
static odin3_vec domains;
static odin3_abc_region region;

/* --- two global test types ---------------------------------------------------------------- */

static void t_sim(const odin3_sim_cell *cell) {
    cell->values[cell->ports[cell->n_ports - 1].idx[0]] = 0;
}

static const odin3_port_def k_t1_ports[] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false}};
static const odin3_param_def k_t_params[] = {
    {"P", ODIN3_VAL_INT, {ODIN3_VAL_INT, 0, NULL, 0, 0, 0}}};
static const odin3_port_def k_t7_ports[] = {{"A0", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A1", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A2", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A3", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A4", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A5", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"A6", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                            {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false}};
static const odin3_celltype_def k_param_gate = {
    "$t_param", ODIN3_GRAN_BIT, 0, k_t1_ports, 2, k_t_params, 1, NULL, NULL, t_sim, NULL};
static const odin3_celltype_def k_wide_gate = {
    "$t_wide7", ODIN3_GRAN_BIT, 0, k_t7_ports, 8, NULL, 0, NULL, NULL, t_sim, NULL};

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    mod = NULL;
    run = NULL;
    region = (odin3_abc_region){0};
    domains = (odin3_vec){0};
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_run_limits(NULL);
    odin3_abc_region_free(&region);
    odin3_abc_domains_free(&domains);
    odin3_abc_run_destroy(run);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, name), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
}

static void start(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, afx_intern(design, "abc"), &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_run_start(&ctx, odin3_bytes_cstr(""), &run));
    run->module = mod;
}

/* Domains of the module, then the region of domain index `which`. */
static void build(uint32_t which) {
    odin3_abc_region_free(&region);
    odin3_abc_domains_free(&domains);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_TRUE(which < domains.len);
    run->domain = (const odin3_abc_domain *)domains.data + which;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_region_build(run, run->domain, &region));
    run->region = &region;
}

static uint32_t role_of(odin3_node_id node) {
    return ((const uint8_t *)region.role.data)[node.v];
}

static uint32_t at32(const odin3_vec *vec, size_t i) {
    return ((const uint32_t *)vec->data)[i];
}

static bool contains(const odin3_vec *vec, uint32_t value) {
    for (size_t i = 0; i < vec->len; i++) {
        if (at32(vec, i) == value) {
            return true;
        }
    }
    return false;
}

static void expect_vec(const odin3_vec *vec, const uint32_t *want, size_t n) {
    TEST_ASSERT_EQUAL_UINT32(n, (uint32_t)vec->len);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], at32(vec, i));
    }
}

/* --- roles ---------------------------------------------------------------------------------- */

static bool name_in(const char *name, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Every registry type: the role follows §5.1's rule. */
static void test_every_registry_type(void) {
    static const char *const k_soft[] = {"$lut",   "$_BUF_",  "$_NOT_", "$_AND_",  "$_OR_",
                                         "$_XOR_", "$_NAND_", "$_NOR_", "$_XNOR_", "$_MUX_"};
    static const char *const k_const[] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_",
                                          "$sop"};
    new_module("m");
    odin3_node_id nodes[256];
    uint32_t count = 0;
    for (uint32_t i = 0; i < odin3_builtin_celltype_count && count < 256; i++) {
        const odin3_celltype_def *def = odin3_builtin_celltypes[i];
        if (def->gran == ODIN3_GRAN_PORT || (def->flags & ODIN3_CT_PROCESS) != 0) {
            continue;
        }
        odin3_node_spec spec = {afx_type(design, def->name), 0, (odin3_prov_id){0}, NULL,
                                def->n_params};
        if (odin3_node_create(mod, &spec, &nodes[count]) == ODIN3_OK) {
            count++;
        }
    }
    TEST_ASSERT_GREATER_THAN_UINT32(40, count);
    start();
    build(0);
    for (uint32_t i = 0; i < count; i++) {
        const char *name = odin3_celltype_get(design, odin3_node_type(mod, nodes[i]))->name;
        uint32_t want = ODIN3_ABC_ROLE_CUT;
        if (name_in(name, k_soft, sizeof k_soft / sizeof k_soft[0])) {
            want = ODIN3_ABC_ROLE_SOFT;
        } else if (name_in(name, k_const, sizeof k_const / sizeof k_const[0])) {
            want = ODIN3_ABC_ROLE_CONST;
        } else if (strcmp(name, "$_DFF_P_") == 0 && run->domain->kind == ODIN3_ABC_DOM_EDGE &&
                   !run->domain->negedge) {
            want = ODIN3_ABC_ROLE_REG;
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, role_of(nodes[i]), name);
    }
}

/* Rule edges: a parameterised bit type, a 7-input gate, a keep cell, a bus driver. */
static void test_rule_edges(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&k_param_gate));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&k_wide_gate));
    odin3_design_destroy(design);
    design = odin3_design_create();
    new_module("m");
    odin3_net_id a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id y1 = afx_net(design, mod, "y1");
    odin3_net_id y2 = afx_net(design, mod, "y2");
    odin3_net_id y3 = afx_net(design, mod, "y3");
    odin3_net_id bus = afx_net(design, mod, "bus");
    odin3_net_id soft_out = afx_net(design, mod, "so");
    odin3_net_id n1[2] = {a, y1};
    odin3_node_id param = afx_cell(design, mod, "$t_param", "p", n1, 2);
    odin3_net_id n7[8] = {a, b, a, b, a, b, a, y2};
    odin3_node_id wide = afx_cell(design, mod, "$t_wide7", "w", n7, 8);
    odin3_net_id nk[3] = {a, b, y3};
    odin3_node_id keep = afx_cell(design, mod, "$_AND_", "k", nk, 3);
    afx_keep_node(design, mod, keep);
    odin3_net_id nb1[2] = {a, bus};
    odin3_node_id buf1 = afx_cell(design, mod, "$_BUF_", "b1", nb1, 2);
    odin3_net_id nb2[2] = {b, bus};
    odin3_node_id buf2 = afx_cell(design, mod, "$_BUF_", "b2", nb2, 2);
    odin3_net_id ns[2] = {bus, soft_out};
    odin3_node_id reader = afx_cell(design, mod, "$_NOT_", "r", ns, 2);
    start();
    build(0);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CUT, role_of(param));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CUT, role_of(wide));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CUT, role_of(keep));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CUT, role_of(buf1));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CUT, role_of(buf2));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_SOFT, role_of(reader));
    TEST_ASSERT_TRUE(contains(&region.cis, bus.v)); /* the bus is a CI to its soft reader */
    TEST_ASSERT_EQUAL_UINT32(5, region.cut_counts[ODIN3_ABC_CUT_OTHER] +
                                    region.cut_counts[ODIN3_ABC_CUT_TRISTATE]);
}

/* --- constants ------------------------------------------------------------------------------ */

/* x, z and unconn read as 0 per pin (C2); 1 reads as 1; a counted INFO input. */
static void test_constants_per_pin(void) {
    new_module("m");
    odin3_net_id a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id y1 = afx_port(design, mod, "y1", ODIN3_DIR_OUT);
    odin3_net_id y2 = afx_port(design, mod, "y2", ODIN3_DIR_OUT);
    odin3_net_id y3 = afx_port(design, mod, "y3", ODIN3_DIR_OUT);
    odin3_net_id cx = afx_net(design, mod, "cx");
    odin3_net_id c1 = afx_net(design, mod, "c1");
    odin3_net_id unconn = afx_net(design, mod, "unconn");
    odin3_net_id cxn[1] = {cx};
    odin3_net_id c1n[1] = {c1};
    (void)afx_cell(design, mod, "$_CONSTX_", "gx", cxn, 1);
    (void)afx_cell(design, mod, "$_CONST1_", "g1", c1n, 1);
    (void)afx_const_sop(design, mod, unconn);
    odin3_net_id n1[3] = {a, cx, y1};
    odin3_net_id n2[3] = {a, unconn, y2};
    odin3_net_id n3[3] = {a, c1, y3};
    (void)afx_cell(design, mod, "$_AND_", "u1", n1, 3);
    (void)afx_cell(design, mod, "$_AND_", "u2", n2, 3);
    (void)afx_cell(design, mod, "$_AND_", "u3", n3, 3);
    start();
    build(0);
    TEST_ASSERT_EQUAL_INT(0, odin3_abc_pin_literal(run, cx));
    TEST_ASSERT_EQUAL_INT(0, odin3_abc_pin_literal(run, unconn));
    TEST_ASSERT_EQUAL_INT(1, odin3_abc_pin_literal(run, c1));
    TEST_ASSERT_EQUAL_INT(-1, odin3_abc_pin_literal(run, a));
    TEST_ASSERT_TRUE(odin3_abc_net_undefined(mod, unconn));
    TEST_ASSERT_FALSE(odin3_abc_net_undefined(mod, cx));
    TEST_ASSERT_EQUAL_UINT32(2, region.zero_pins); /* cx and unconn; c1 is a real 1 */
    TEST_ASSERT_FALSE(contains(&region.cis, cx.v));
    TEST_ASSERT_FALSE(contains(&region.cis, unconn.v));
    TEST_ASSERT_FALSE(contains(&region.cis, c1.v));
}

/* Hard-block pins on unconn and on a constant x stay outside every run (Review Focus 1). */
static void test_bypass_unconn_hard_pin(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
    new_module("m");
    odin3_net_id clk = afx_port(design, mod, "clk", ODIN3_DIR_IN);
    odin3_net_id a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id o = afx_port(design, mod, "o", ODIN3_DIR_OUT);
    odin3_net_id unconn = afx_net(design, mod, "unconn");
    odin3_net_id cx = afx_net(design, mod, "cx");
    odin3_net_id cxn[1] = {cx};
    odin3_node_id gen_x = afx_cell(design, mod, "$_CONSTX_", "gx", cxn, 1);
    odin3_node_id gen_u = afx_const_sop(design, mod, unconn);
    odin3_net_id addr[15];
    for (uint32_t i = 0; i < 15; i++) {
        addr[i] = i == 5 ? unconn : (i == 6 ? cx : a);
    }
    odin3_net_id data[1] = {a};
    odin3_net_id we[1] = {a};
    odin3_net_id out[1] = {o};
    odin3_net_id clk_v[1] = {clk};
    odin3_netvec ports[5] = {{clk_v, 1}, {data, 1}, {addr, 15}, {we, 1}, {out, 1}};
    odin3_node_spec spec = {afx_type(design, "single_port_ram"), 0, (odin3_prov_id){0}, NULL, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL));
    afx_check(design);
    start();
    build(0);
    TEST_ASSERT_FALSE(contains(&region.cos, unconn.v));
    TEST_ASSERT_FALSE(contains(&region.cos, cx.v));
    TEST_ASSERT_FALSE(contains(&region.cis, unconn.v));
    TEST_ASSERT_FALSE(contains(&region.cis, cx.v));
    TEST_ASSERT_FALSE(contains(&region.soft, gen_x.v));
    TEST_ASSERT_FALSE(contains(&region.soft, gen_u.v));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CONST, role_of(gen_x));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_CONST, role_of(gen_u));
    TEST_ASSERT_EQUAL_UINT32(1, region.cut_counts[ODIN3_ABC_CUT_HARD]);
}

/* --- CIs and COs ----------------------------------------------------------------------------- */

static void test_cis_and_cos(void) {
    new_module("m");
    odin3_net_id a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id w1 = afx_net(design, mod, "w1");
    odin3_net_id dead = afx_net(design, mod, "dead");
    odin3_net_id kept = afx_net(design, mod, "kept");
    odin3_net_id free_net = afx_net(design, mod, "free");
    odin3_net_id g1[3] = {a, b, w1};
    odin3_net_id g2[2] = {w1, y};
    odin3_net_id g3[3] = {a, b, dead};
    odin3_net_id g4[3] = {a, b, kept};
    odin3_net_id g5[3] = {kept, free_net, afx_net(design, mod, "z")};
    (void)afx_cell(design, mod, "$_AND_", "g1", g1, 3);
    (void)afx_cell(design, mod, "$_NOT_", "g2", g2, 2);
    (void)afx_cell(design, mod, "$_OR_", "g3", g3, 3);
    (void)afx_cell(design, mod, "$_XOR_", "g4", g4, 3);
    (void)afx_cell(design, mod, "$_AND_", "g5", g5, 3);
    odin3_value one = odin3_value_int(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NET, kept.v},
                                                   afx_intern(design, "keep"), &one));
    start();
    build(0);
    uint32_t cis[3] = {a.v, b.v, free_net.v};
    expect_vec(&region.cis, cis, 3);
    uint32_t cos[2] = {y.v, kept.v};
    expect_vec(&region.cos, cos, 2);
    TEST_ASSERT_FALSE(contains(&region.cos, dead.v)); /* nothing reads it */
    TEST_ASSERT_FALSE(contains(&region.cos, w1.v));   /* only a soft cell reads it */
    TEST_ASSERT_EQUAL_UINT32(1, at32(&region.ci_of, a.v));
    TEST_ASSERT_EQUAL_UINT32(0, at32(&region.ci_of, w1.v));
    TEST_ASSERT_EQUAL_UINT32(1, at32(&region.co_of, y.v));
    TEST_ASSERT_EQUAL_UINT32(2, at32(&region.co_of, kept.v));
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)region.soft.len);
}

/* A derived clock keeps its net: a soft net read by a flip-flop's C is a CO. */
static void test_derived_clock_is_a_co(void) {
    new_module("m");
    odin3_net_id clk = afx_port(design, mod, "clk", ODIN3_DIR_IN);
    odin3_net_id en = afx_port(design, mod, "en", ODIN3_DIR_IN);
    odin3_net_id d = afx_port(design, mod, "d", ODIN3_DIR_IN);
    odin3_net_id q = afx_port(design, mod, "q", ODIN3_DIR_OUT);
    odin3_net_id clk2 = afx_net(design, mod, "clk2");
    odin3_net_id g[3] = {clk, en, clk2};
    (void)afx_cell(design, mod, "$_AND_", "g", g, 3);
    odin3_value init = odin3_value_int(0);
    odin3_net_id f[3] = {clk2, d, q};
    (void)afx_cell_p(design, mod, "$_DFF_P_", &init, f, 3);
    start();
    build(0);
    TEST_ASSERT_TRUE(contains(&region.cos, clk2.v));
    TEST_ASSERT_TRUE(contains(&region.cos, q.v)); /* the Q net is read by a port */
    TEST_ASSERT_FALSE(contains(&region.cis, clk2.v));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)region.regs.len);
    TEST_ASSERT_EQUAL_UINT32(d.v, at32(&region.reg_d, 0));
    TEST_ASSERT_EQUAL_UINT32(1, at32(&region.q_of, q.v));
}

/* --- domains -------------------------------------------------------------------------------- */

static void build_four_domains(void) {
    new_module("m");
    odin3_net_id clk_a = afx_port(design, mod, "clk_a", ODIN3_DIR_IN);
    odin3_net_id clk_b = afx_port(design, mod, "clk_b", ODIN3_DIR_IN);
    odin3_net_id d = afx_port(design, mod, "d", ODIN3_DIR_IN);
    odin3_net_id q[4];
    q[0] = afx_port(design, mod, "q0", ODIN3_DIR_OUT);
    q[1] = afx_port(design, mod, "q1", ODIN3_DIR_OUT);
    q[2] = afx_port(design, mod, "q2", ODIN3_DIR_OUT);
    q[3] = afx_port(design, mod, "q3", ODIN3_DIR_OUT);
    odin3_value init = odin3_value_int(0);
    odin3_net_id f0[3] = {clk_b, d, q[0]};
    odin3_net_id f1[3] = {clk_a, d, q[1]};
    odin3_net_id f2[3] = {clk_a, d, q[2]};
    odin3_net_id f3[2] = {d, q[3]};
    (void)afx_cell_p(design, mod, "$_DFF_P_", &init, f0, 3);
    (void)afx_cell_p(design, mod, "$_DFF_P_", &init, f1, 3);
    (void)afx_cell_p(design, mod, "$_DFF_N_", &init, f2, 3);
    (void)afx_cell_p(design, mod, "$_FF_", &init, f3, 2);
}

static void test_domains_order_and_cap(void) {
    build_four_domains();
    start();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_EQUAL_UINT64(4, domains.len);
    const char *want[4] = {"clock 'clk_a' posedge", "clock 'clk_a' negedge",
                           "clock 'clk_b' posedge", "global clock"};
    for (size_t i = 0; i < 4; i++) {
        char text[160];
        const odin3_abc_domain *dom = (const odin3_abc_domain *)domains.data + i;
        odin3_abc_domain_text(mod, dom, text, sizeof text);
        TEST_ASSERT_EQUAL_STRING(want[i], text);
        TEST_ASSERT_EQUAL_UINT64(1, dom->ffs.len);
    }
    odin3_abc_domains_free(&domains);
    domains = (odin3_vec){0};
    odin3_abc_run_limits lim = {2, 0, 0, 0};
    odin3_abc_test_set_run_limits(&lim);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_EQUAL_STRING("abc: module 'm' has 4 clock domains; at most 2",
                             afx_only(ODIN3_LOG_ERROR, "abc:"));
}

/* A run sees its domain's flip-flops as registers and the other domains' as cut cells. */
static void test_other_domains_are_cut(void) {
    build_four_domains();
    start();
    build(0);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)region.regs.len);
    TEST_ASSERT_EQUAL_UINT32(0, region.cut_counts[ODIN3_ABC_CUT_OTHER]);
    uint32_t reg = at32(&region.regs, 0);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_ROLE_REG, role_of((odin3_node_id){reg}));
    build(3);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)region.regs.len);
}

static void test_module_without_flip_flops_has_one_domain(void) {
    new_module("m");
    (void)afx_port(design, mod, "a", ODIN3_DIR_IN);
    start();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_EQUAL_UINT64(1, domains.len);
    char text[160];
    odin3_abc_domain_text(mod, domains.data, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("no flip-flops", text);
}

/* --- loops ---------------------------------------------------------------------------------- */

static void test_loop_is_cut(void) {
    new_module("m");
    odin3_net_id n1 = afx_net(design, mod, "n1");
    odin3_net_id n2 = afx_net(design, mod, "n2");
    odin3_net_id g1[2] = {n2, n1};
    odin3_net_id g2[2] = {n1, n2};
    odin3_node_id first = afx_cell(design, mod, "$_NOT_", "u1", g1, 2);
    odin3_node_id second = afx_cell(design, mod, "$_NOT_", "u2", g2, 2);
    start();
    build(0);
    char want[160];
    (void)snprintf(want, sizeof want,
                   "abc: module 'm': combinational loop through $_NOT_ 'u1'; its output n1 is a "
                   "cut point");
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_WARN, want));
    TEST_ASSERT_TRUE(contains(&region.cis, n1.v));
    TEST_ASSERT_TRUE(contains(&region.cos, n1.v));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)region.soft.len);
    TEST_ASSERT_EQUAL_UINT32(second.v, at32(&region.soft, 0)); /* u2 reads the cut n1 first */
    TEST_ASSERT_EQUAL_UINT32(first.v, at32(&region.soft, 1));
}

/* --- cut-point counts ------------------------------------------------------------------------- */

static void test_cut_counts(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
    char path[AFX_PATH];
    afx_path(path, "cuts.blif");
    afx_write(path, ".model m\n.inputs a b clk\n.outputs y\n"
                    ".subckt multiply a[0]=a b[0]=b out[0]=m0\n"
                    ".subckt sub x=a y=s0\n"
                    ".latch a l0 ah clk 0\n"
                    ".names a b k0\n11 1\n"
                    ".names m0 s0 l0 k0 y\n1111 1\n.end\n"
                    ".model sub\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    mod = afx_top(design);
    odin3_node_id kept = {0};
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        const odin3_celltype_def *def =
            odin3_celltype_get(design, odin3_node_type(mod, (odin3_node_id){id}));
        if (strcmp(def->name, "$sop") == 0 &&
            odin3_node_pins(mod, (odin3_node_id){id}).count == 3) {
            kept = (odin3_node_id){id}; /* the 2-input AND, driving k0 */
        }
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, kept.v);
    afx_keep_node(design, mod, kept);
    start();
    build(0);
    TEST_ASSERT_EQUAL_UINT32(1, region.cut_counts[ODIN3_ABC_CUT_HARD]);
    TEST_ASSERT_EQUAL_UINT32(1, region.cut_counts[ODIN3_ABC_CUT_INSTANCE]);
    TEST_ASSERT_EQUAL_UINT32(1, region.cut_counts[ODIN3_ABC_CUT_LATCH]);
    TEST_ASSERT_EQUAL_UINT32(0, region.cut_counts[ODIN3_ABC_CUT_TRISTATE]);
    TEST_ASSERT_EQUAL_UINT32(1, region.cut_counts[ODIN3_ABC_CUT_OTHER]);
}

/* --- scale and memory ------------------------------------------------------------------------ */

/* A long chain classifies and orders with no recursion (Release: under 2 s). */
static void test_long_chain(void) {
    new_module("m");
    odin3_net_id prev = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    for (uint32_t i = 0; i < CHAIN_GATES; i++) {
        odin3_net_id next = i + 1 == CHAIN_GATES ? y : afx_net(design, mod, NULL);
        odin3_net_id g[2] = {prev, next};
        (void)afx_cell(design, mod, "$_NOT_", NULL, g, 2);
        prev = next;
    }
    start();
    clock_t begin = clock();
    build(0);
    double secs = (double)(clock() - begin) / CLOCKS_PER_SEC;
    TEST_ASSERT_EQUAL_UINT32(CHAIN_GATES, (uint32_t)region.soft.len);
    for (uint32_t i = 1; i < CHAIN_GATES; i++) {
        TEST_ASSERT_TRUE(at32(&region.soft, i - 1) < at32(&region.soft, i));
    }
#ifdef NDEBUG
    TEST_ASSERT_TRUE_MESSAGE(secs < 2.0, "region_build of 100,000 gates took 2 s or more");
#else
    (void)secs;
#endif
}

static void test_region_oom_sweep(void) {
    new_module("m");
    odin3_net_id a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id clk = afx_port(design, mod, "clk", ODIN3_DIR_IN);
    odin3_net_id q = afx_port(design, mod, "q", ODIN3_DIR_OUT);
    odin3_net_id w = afx_net(design, mod, "w");
    odin3_net_id g[2] = {a, w};
    (void)afx_cell(design, mod, "$_NOT_", "g", g, 2);
    odin3_value init = odin3_value_int(0);
    odin3_net_id f[3] = {clk, w, q};
    (void)afx_cell_p(design, mod, "$_DFF_P_", &init, f, 3);
    start();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    uint32_t failures = 0;
    for (long at = 0; at < OOM_LIMIT; at++) {
        odin3_abc_region out;
        odin3_util_set_alloc_fail_after(at);
        odin3_status st = odin3_abc_region_build(run, domains.data, &out);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            odin3_abc_region_free(&out);
            break;
        }
        failures++;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(out.soft.data);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
    afx_check(design);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_registry_type);
    RUN_TEST(test_rule_edges);
    RUN_TEST(test_constants_per_pin);
    RUN_TEST(test_bypass_unconn_hard_pin);
    RUN_TEST(test_cis_and_cos);
    RUN_TEST(test_derived_clock_is_a_co);
    RUN_TEST(test_domains_order_and_cap);
    RUN_TEST(test_other_domains_are_cut);
    RUN_TEST(test_module_without_flip_flops_has_one_domain);
    RUN_TEST(test_loop_is_cut);
    RUN_TEST(test_cut_counts);
    RUN_TEST(test_long_chain);
    RUN_TEST(test_region_oom_sweep);
    return UNITY_END();
}
