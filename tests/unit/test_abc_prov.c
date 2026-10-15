/*
 * test_abc_prov.c — provenance across ABC, the LUT depth and the run information (3D plan Task 18;
 * spec §8, AB-25): the records of the new objects on cone anchors, shared LUTs, the forward and
 * backward walks, the coarse form over the edge budget, dead logic, depth, the run information
 * and the per-run INFO lines, and an out-of-memory sweep over the records.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "abc_stage.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/prov.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/sha256.h"

#include <regex.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { MAX_LEAVES = 16, SWEEP_LIMIT = 4000 };

static odin3_design *design;
static odin3_module *mod;
static odin3_pass_ctx reader_ctx;
static odin3_arena *lut_arena;

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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, mid));
    mod = odin3_module_get(design, mid);
    lut_arena = odin3_arena_create(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(design, afx_intern(design, "read"), &reader_ctx));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_run_limits(NULL);
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    odin3_arena_destroy(lut_arena);
}

/* --- building: cells that carry a SOURCE record --------------------------------------------- */

static odin3_net_id input(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_IN);
}

static odin3_net_id output(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_OUT);
}

static odin3_net_id wire_net(const char *name) {
    return afx_net(design, mod, name);
}

/* A SOURCE record at line `line` of t.v. */
static odin3_prov_id source_at(uint32_t line) {
    odin3_srcloc loc = {afx_intern(design, "t.v"), line, 1, line, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&reader_ctx, &origin, &id));
    return id;
}

/* A cell of `type` on `nets` (port order) whose provenance is the SOURCE record of `line`. */
static odin3_node_id cell_at(const char *type, uint32_t line, const odin3_net_id *nets,
                             uint32_t count) {
    odin3_netvec ports[AFX_MAX_PORTS];
    for (uint32_t i = 0; i < count; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    odin3_celltype_id id = afx_type(design, type);
    odin3_node_spec spec = {id, 0, source_at(line), NULL, odin3_celltype_get(design, id)->n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

static odin3_node_id and_at(uint32_t line, odin3_net_id in_a, odin3_net_id in_b,
                            odin3_net_id out_y) {
    odin3_net_id nets[3] = {in_a, in_b, out_y};
    return cell_at("$_AND_", line, nets, 3);
}

static odin3_node_id buf_at(uint32_t line, odin3_net_id in_a, odin3_net_id out_y) {
    odin3_net_id nets[2] = {in_a, out_y};
    return cell_at("$_BUF_", line, nets, 2);
}

static odin3_node_id ff_at(uint32_t line, odin3_net_id clk, odin3_net_id dnet, odin3_net_id qnet) {
    odin3_net_id nets[3] = {clk, dnet, qnet};
    return cell_at("$_DFF_P_", line, nets, 3);
}

static odin3_status run_abc(const char *args) {
    return odin3_pass_run(design, "abc", odin3_bytes_cstr(args));
}

/* --- reading -------------------------------------------------------------------------------- */

static odin3_node_id driver_of(odin3_net_id net) {
    TEST_ASSERT_TRUE(odin3_net_live(mod, net));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, net));
    return odin3_pin_node(mod, odin3_net_driver(mod, net));
}

static odin3_net_id port_net_of(const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(mod, afx_intern(design, name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(mod, wire, 0);
}

static odin3_net_id input_net(odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    return odin3_pin_net(mod, pins.first);
}

static const odin3_prov_record *record_of(odin3_node_id node) {
    odin3_prov_id prov = odin3_node_prov(mod, node);
    TEST_ASSERT_TRUE(prov.v != 0);
    const odin3_prov_record *rec = odin3_prov_get(design, prov);
    TEST_ASSERT_NOT_NULL(rec);
    return rec;
}

/* The parents of a node's record must be exactly want[0..count). */
static void expect_parents(odin3_node_id node, const odin3_prov_id *want, uint32_t count) {
    const odin3_prov_record *rec = record_of(node);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(count, rec->parents.count);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i].v, rec->parents.ids[i].v);
    }
}

typedef struct leaves {
    odin3_prov_id ids[MAX_LEAVES];
    uint32_t count;
} leaves;

static void collect_leaf(void *user, odin3_prov_id leaf) {
    leaves *out = user;
    if (out->count < MAX_LEAVES) {
        out->ids[out->count++] = leaf;
    }
}

static void expect_leaves(odin3_node_id node, const odin3_prov_id *want, uint32_t count) {
    leaves out = {{{0}}, 0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_sources(design, odin3_node_prov(mod, node), collect_leaf, &out));
    TEST_ASSERT_EQUAL_UINT32(count, out.count);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i].v, out.ids[i].v);
    }
}

static uint32_t count_live(const char *type) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        found +=
            odin3_node_live(mod, node) &&
                    strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, type) == 0
                ? 1U
                : 0U;
    }
    return found;
}

/* --- cone mode ------------------------------------------------------------------------------ */

/* y = (a & b) & c: both LUTs sit in y's region, whose old region is both gates, in ID order. */
static void test_cone_records_in_node_order(void) {
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id in_c = input("c");
    odin3_net_id mid = wire_net("mid");
    odin3_net_id out_y = output("y");
    odin3_prov_id s_in = odin3_node_prov(mod, and_at(10, in_a, in_b, mid));
    odin3_prov_id s_top = odin3_node_prov(mod, and_at(11, mid, in_c, out_y));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6 --prov cone"), afx_errors());
    afx_check(design);
    odin3_node_id top = driver_of(port_net_of("y"));
    odin3_node_id inner = driver_of(input_net(top, ODIN3_LUT_PORT_A));
    const odin3_prov_id want[2] = {s_in, s_top};
    expect_parents(top, want, 2);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(mod, top).v, odin3_node_prov(mod, inner).v);
    expect_leaves(top, want, 2);
    expect_leaves(inner, want, 2);
}

/* A register's D cone has the old D cone's records as parents (C3); the new flip-flop's record
 * has the old flip-flop's. */
static void test_register_input_and_flop(void) {
    odin3_net_id clk = input("clk");
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id dat_d = wire_net("d");
    odin3_net_id out_q = output("q");
    odin3_prov_id s_and = odin3_node_prov(mod, and_at(20, in_a, in_b, dat_d));
    odin3_prov_id s_ff = odin3_node_prov(mod, ff_at(21, clk, dat_d, out_q));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    afx_check(design);
    odin3_node_id flop = driver_of(port_net_of("q"));
    odin3_node_id src = driver_of(input_net(flop, 1));
    expect_parents(src, &s_and, 1);
    expect_parents(flop, &s_ff, 1);
    expect_leaves(flop, &s_ff, 1);
    /* forward: the old gate's record reaches the new LUT */
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_prov_hits hits = odin3_prov_index_by_record(ix, s_and);
    bool found = false;
    for (uint32_t i = 0; i < hits.count; i++) {
        found = found || (hits.hits[i].live && hits.hits[i].obj.kind == ODIN3_OBJ_NODE &&
                          hits.hits[i].obj.id == src.v);
    }
    odin3_prov_index_destroy(ix);
    TEST_ASSERT_TRUE_MESSAGE(found, "the children of the old cell's record lack the new LUT");
}

/* p = a & b feeds y1 = p & c and y2 = p & d: p's LUT is in two regions and carries a record whose
 * parents are the two outputs' records, in CO order; the first leaf is the lowest-ID old cell. */
static void build_shared(odin3_prov_id *s_p, odin3_prov_id *s_1, odin3_prov_id *s_2) {
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id in_c = input("c");
    odin3_net_id dat_d = input("d");
    odin3_net_id net_p = wire_net("p");
    odin3_net_id y1 = output("y1");
    odin3_net_id y2 = output("y2");
    *s_p = odin3_node_prov(mod, and_at(30, in_a, in_b, net_p));
    *s_1 = odin3_node_prov(mod, and_at(31, net_p, in_c, y1));
    *s_2 = odin3_node_prov(mod, and_at(32, net_p, dat_d, y2));
}

static void test_shared_lut_has_an_extra_record(void) {
    odin3_prov_id s_p;
    odin3_prov_id s_1;
    odin3_prov_id s_2;
    build_shared(&s_p, &s_1, &s_2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    afx_check(design);
    odin3_node_id l1 = driver_of(port_net_of("y1"));
    odin3_node_id l2 = driver_of(port_net_of("y2"));
    odin3_node_id shared = driver_of(input_net(l1, ODIN3_LUT_PORT_A));
    TEST_ASSERT_EQUAL_UINT32(shared.v, driver_of(input_net(l2, ODIN3_LUT_PORT_A)).v);
    const odin3_prov_id own1[2] = {s_p, s_1};
    expect_parents(l1, own1, 2);
    const odin3_prov_id both[2] = {odin3_node_prov(mod, l1), odin3_node_prov(mod, l2)};
    expect_parents(shared, both, 2);
    /* the first leaf of the backward walk is the lowest-ID old cell's SOURCE */
    leaves out = {{{0}}, 0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_sources(design, odin3_node_prov(mod, shared), collect_leaf, &out));
    TEST_ASSERT_TRUE(out.count >= 1);
    TEST_ASSERT_EQUAL_UINT32(s_p.v, out.ids[0].v);
}

/* A budget of one parent edge: one record for the run, every new object carries it, exact WARN. */
static void test_coarse_mode(void) {
    odin3_prov_id s_p;
    odin3_prov_id s_1;
    odin3_prov_id s_2;
    build_shared(&s_p, &s_1, &s_2);
    odin3_abc_test_set_run_limits(&(odin3_abc_run_limits){0, 0, 0, 1});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    afx_check(design);
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_WARN,
                             "abc: module 'm', no flip-flops: provenance needs 6 parent edges "
                             "(budget 1); the run's LUTs share one record"));
    odin3_node_id l1 = driver_of(port_net_of("y1"));
    odin3_node_id l2 = driver_of(port_net_of("y2"));
    odin3_node_id shared = driver_of(input_net(l1, ODIN3_LUT_PORT_A));
    const odin3_prov_id all[3] = {s_p, s_1, s_2};
    expect_parents(l1, all, 3);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(mod, l1).v, odin3_node_prov(mod, l2).v);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(mod, l1).v, odin3_node_prov(mod, shared).v);
}

/* An old soft cell that reaches no output and no register input dies without children. */
static void test_dead_logic_has_no_children(void) {
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id out_y = output("y");
    odin3_net_id dead = wire_net("dead");
    odin3_prov_id s_dead = odin3_node_prov(mod, and_at(40, in_a, in_b, dead));
    (void)buf_at(41, in_a, out_y);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    afx_check(design);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_prov_hits hits = odin3_prov_index_by_record(ix, s_dead);
    uint32_t live = 0;
    for (uint32_t i = 0; i < hits.count; i++) {
        live += hits.hits[i].live ? 1U : 0U;
    }
    odin3_prov_index_destroy(ix);
    TEST_ASSERT_EQUAL_UINT32(0, live);
}

/* --- depth --------------------------------------------------------------------------------- */

static odin3_node_id lut_cell(odin3_net_id in, odin3_net_id out) {
    static const uint64_t k_buffer = 0x2;
    odin3_value params[2];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_lut_params(lut_arena, 1, &k_buffer, params));
    odin3_net_id nets[2] = {in, out};
    odin3_netvec ports[2] = {{&nets[0], 1}, {&nets[1], 1}};
    odin3_node_spec spec = {{odin3_lut_builtin_index()}, 0, (odin3_prov_id){0}, params, 2};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

static void test_depth_chain_and_cut(void) {
    uint32_t depth = 99;
    odin3_net_id in = input("in");
    odin3_net_id n1 = wire_net("n1");
    odin3_net_id n2 = wire_net("n2");
    odin3_net_id n3 = wire_net("n3");
    odin3_net_id n4 = wire_net("n4");
    odin3_net_id n5 = wire_net("n5");
    odin3_net_id out = output("out");
    (void)lut_cell(in, n1);
    (void)lut_cell(n1, n2);
    (void)lut_cell(n2, n3);
    odin3_net_id latch_nets[3] = {in, n3, n4};
    (void)AFX_CELL(design, mod, "$_DLATCH_P_", NULL, latch_nets, 3);
    (void)lut_cell(n4, n5);
    (void)lut_cell(n5, out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_depth(design, &depth));
    TEST_ASSERT_EQUAL_UINT32(3, depth); /* the latch cuts the path: 3 before it, 2 after */
}

static void test_depth_without_top(void) {
    odin3_design *empty = odin3_design_create();
    uint32_t depth = 99;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_depth(empty, &depth));
    TEST_ASSERT_EQUAL_UINT32(0, depth);
    odin3_design_destroy(empty);
}

/* The text of the last INFO message captured. */
static const char *last_info_message(void) {
    uint32_t shown = afx_messages.count < AFX_LINES ? afx_messages.count : AFX_LINES;
    const char *found = "";
    for (uint32_t i = 0; i < shown; i++) {
        found = afx_messages.level[i] == ODIN3_LOG_INFO ? afx_messages.text[i] : found;
    }
    return found;
}

static const char *run_info_text(void) {
    uint32_t info = odin3_passrun_info(design, (odin3_passrun_id){odin3_passrun_end(design) - 1U});
    return info == 0 ? "" : odin3_strtab_get(odin3_design_strtab(design), info);
}

/* port -> LUT -> LUT -> LUT -> FF is depth 3, and the last INFO line says so. */
static void test_final_depth_line(void) {
    odin3_net_id clk = input("clk");
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id in_c = input("c");
    odin3_net_id dat_d = input("d");
    odin3_net_id n1 = wire_net("n1");
    odin3_net_id n2 = wire_net("n2");
    odin3_net_id n3 = wire_net("n3");
    (void)and_at(50, in_a, in_b, n1);
    (void)and_at(51, n1, in_c, n2);
    (void)and_at(52, n2, dat_d, n3);
    (void)ff_at(53, clk, n3, output("q"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    TEST_ASSERT_EQUAL_STRING("abc: final: depth 3", last_info_message());
    TEST_ASSERT_NOT_NULL(strstr(run_info_text(), " depth=3"));
}

/* --- run information ----------------------------------------------------------------------- */

static void sha256_of_file(const char *path, char *hex) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    static char text[1 << 16];
    size_t len = fread(text, 1, sizeof text, file);
    (void)fclose(file);
    odin3_sha256 sha;
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, text, len);
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_sha256_final(&sha, digest);
    odin3_sha256_hex(digest, hex);
}

static const char *last_info(void) {
    return odin3_strtab_get(
        odin3_design_strtab(design),
        odin3_passrun_info(design, (odin3_passrun_id){odin3_passrun_end(design) - 1U}));
}

static void test_run_information_default_script(void) {
    odin3_abc_engine engine = *odin3_abc_fake_engine();
    engine.abc_commit = "a3001b72edc5de22442e942165487fddf150e3d0";
    odin3_abc_test_set_engine(&engine);
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    (void)and_at(60, in_a, in_b, output("y"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    char hex[ODIN3_SHA256_DIGEST_BYTES * 2 + 1];
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/abc/vtr.abc", ODIN3_LIB_DIR);
    sha256_of_file(path, hex);
    char want[400];
    (void)snprintf(want, sizeof want,
                   "script=builtin:lib/abc/vtr.abc sha256=%s abc=a3001b72edc5de22442e942165487fddf1"
                   "50e3d0 K=6 exchange=blif depth=1",
                   hex);
    TEST_ASSERT_EQUAL_STRING(want, last_info());
}

static void test_run_information_quotes_a_path_with_blanks(void) {
    char dir[AFX_PATH];
    char script[AFX_JOIN];
    afx_path(dir, "my dir");
    afx_mkdir(dir);
    (void)snprintf(script, sizeof script, "%s/s.abc", dir);
    AFX_WRITE(script, "strash\nif -K {K}\n");
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_LINKED);
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    (void)and_at(61, in_a, in_b, output("y"));
    char args[3 * AFX_PATH];
    (void)snprintf(args, sizeof args, "--lut 4 --script \"%s\"", script);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc(args), afx_errors());
    char want[AFX_JOIN + 16];
    (void)snprintf(want, sizeof want, "script=\"%s\" ", script);
    TEST_ASSERT_EQUAL_INT(0, strncmp(last_info(), want, strlen(want)));
    TEST_ASSERT_NOT_NULL(strstr(last_info(), " K=4 exchange=blif depth=1"));
}

/* Without a top module there is nothing to map, and the depth is 0. */
static void test_run_information_without_top(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), afx_errors());
    TEST_ASSERT_NOT_NULL(strstr(last_info(), " K=6 exchange=blif depth=0"));
}

/* --- the per-run INFO line ------------------------------------------------------------------ */

static void test_run_info_line(void) {
    odin3_net_id clk = input("clk");
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id in_c = input("c");
    odin3_net_id n1 = wire_net("n1");
    odin3_net_id n2 = wire_net("n2");
    odin3_net_id n3 = wire_net("n3");
    odin3_net_id n4 = wire_net("n4");
    odin3_net_id n5 = wire_net("n5");
    (void)and_at(70, in_a, in_b, n1);
    (void)and_at(71, n1, in_c, n2);
    (void)and_at(72, in_a, in_c, n3);
    (void)buf_at(73, n2, n4);
    (void)buf_at(74, n3, n5);
    (void)ff_at(75, clk, n4, output("q1"));
    (void)ff_at(76, clk, n5, output("q2"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 2"), afx_errors());
    regex_t re;
    TEST_ASSERT_EQUAL_INT(
        0, regcomp(&re,
                   "^abc: module 'm', clock 'clk' posedge: 5 cells and 2 flip-flops -> 3 LUTs "
                   "\\(at most 2 inputs, depth 2\\) and 2 flip-flops, [0-9.]+ ms in ABC$",
                   REG_EXTENDED | REG_NOSUB));
    uint32_t matched = 0;
    for (uint32_t i = 0; i < afx_messages.count && i < AFX_LINES; i++) {
        matched += afx_messages.level[i] == ODIN3_LOG_INFO &&
                           regexec(&re, afx_messages.text[i], 0, NULL, 0) == 0
                       ? 1U
                       : 0U;
    }
    regfree(&re);
    TEST_ASSERT_EQUAL_UINT32(1, matched);
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO, "abc: module 'm', clock 'clk' posedge: no name map "
                                             "(BLIF exchange); provenance uses cut cones"));
    TEST_ASSERT_EQUAL_UINT32(3, count_live("$lut"));
}

/* --- out of memory -------------------------------------------------------------------------- */

static void test_oom_sweep_over_records(void) {
    odin3_prov_id s_p;
    odin3_prov_id s_1;
    odin3_prov_id s_2;
    build_shared(&s_p, &s_1, &s_2);
    long ok_after = -1;
    for (long nth = 0; nth < SWEEP_LIMIT && ok_after < 0; nth++) {
        afx_staged st;
        afx_stage(design, mod, &st);
        odin3_vec anchors;
        odin3_vec obj_prov;
        odin3_vec_init(&anchors, sizeof(odin3_abc_anchor));
        odin3_vec_init(&obj_prov, sizeof(uint32_t));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_anchors(st.run, &st.result, &anchors));
        odin3_util_set_alloc_fail_after(nth);
        odin3_status res = odin3_abc_records(st.run, &st.result, &anchors, &obj_prov);
        odin3_util_set_alloc_fail_after(-1);
        if (res == ODIN3_OK) {
            ok_after = nth;
            TEST_ASSERT_EQUAL_UINT32(2 + st.result.n_cis + st.result.n_regs_out + st.result.n_luts,
                                     obj_prov.len);
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, res);
        }
        afx_check(design);
        odin3_vec_free(&anchors);
        odin3_vec_free(&obj_prov);
        afx_unstage(&st);
    }
    TEST_ASSERT_TRUE_MESSAGE(ok_after > 0, "the sweep never reached a success");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cone_records_in_node_order);
    RUN_TEST(test_register_input_and_flop);
    RUN_TEST(test_shared_lut_has_an_extra_record);
    RUN_TEST(test_coarse_mode);
    RUN_TEST(test_dead_logic_has_no_children);
    RUN_TEST(test_depth_chain_and_cut);
    RUN_TEST(test_depth_without_top);
    RUN_TEST(test_final_depth_line);
    RUN_TEST(test_run_information_default_script);
    RUN_TEST(test_run_information_quotes_a_path_with_blanks);
    RUN_TEST(test_run_information_without_top);
    RUN_TEST(test_run_info_line);
    RUN_TEST(test_oom_sweep_over_records);
    return UNITY_END();
}
