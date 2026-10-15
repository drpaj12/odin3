/*
 * test_flatten_graph.c — 3A Task 2: the pre-flight graph (cell type -> module map, Kahn order,
 * cycles, flat sizes, roots), the keep_hierarchy rule, the caps and the budget, the options and the
 * pass registration (spec §3.3, §7, §11, §12; plan Task 2).
 */
#include "flatten_fixture.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/flatten/flatten.h"
#include "passes/flatten/flatten_test.h"
#include "passes/manager.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_BLIF_DIR
#error "ODIN3_BLIF_DIR must name tests/golden/blif"
#endif

enum { LOG_TEXT = 8192, OOM_LIMIT = 4000, SAT_LEVELS = 70, MANY_UNUSED = 10, NAME_BUF = 16 };

static ff fix;
static char log_text[LOG_TEXT];

/* "<prefix><i>" in buf (the sizes here never truncate). */
static void numbered(char *buf, size_t size, const char *prefix, uint32_t idx) {
    (void)snprintf(buf, size, "%s%u", prefix, idx);
}

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    ff_begin(&fix);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_flatten_test_set_limits(NULL);
    odin3_budget_test_set_limit(0);
    odin3_log_set_sink(NULL, NULL);
    ff_end(&fix);
}

/* --- builders ------------------------------------------------------------------------------ */

/* leaf: a -> NOT -> y. */
static odin3_module *make_leaf(const char *name) {
    odin3_module *mod = ff_module(&fix, name);
    (void)ff_port(&fix, mod, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mod, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id nets[2] = {ff_wnet(&fix, mod, "a", 0), ff_wnet(&fix, mod, "y", 0)};
    (void)ff_gate(&fix, mod, (ff_cell){"$_NOT_", "g"}, nets);
    return mod;
}

/* A module with ports a, y and `fan` instances of child, each with a fresh output net. */
static odin3_module *make_level(const char *name, odin3_module *child, uint32_t fan) {
    odin3_module *mod = ff_module(&fix, name);
    (void)ff_port(&fix, mod, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mod, "y", ODIN3_DIR_OUT, 1);
    for (uint32_t i = 0; i < fan; i++) {
        char inst[NAME_BUF];
        numbered(inst, sizeof inst, "u", i);
        odin3_net_id nets[2] = {ff_wnet(&fix, mod, "a", 0), ff_net(&fix, mod, NULL)};
        (void)ff_inst(&fix, (ff_pair){mod, child}, inst, nets);
    }
    return mod;
}

static odin3_module *make_empty_with(const char *name, odin3_module *const *children,
                                     uint32_t count) {
    odin3_module *mod = ff_module(&fix, name);
    for (uint32_t i = 0; i < count; i++) {
        char inst[NAME_BUF];
        numbered(inst, sizeof inst, "u", i);
        (void)ff_inst(&fix, (ff_pair){mod, children[i]}, inst, NULL);
    }
    return mod;
}

static void set_top(odin3_module *mod) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(fix.design, odin3_module_id_of(mod)));
}

static odin3_flatten_graph *build(odin3_module *top) {
    odin3_flatten_graph *graph = NULL;
    odin3_status st = odin3_flatten_graph_build(fix.design, odin3_module_id_of(top), &graph);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL(graph);
    return graph;
}

static uint32_t position(const odin3_flatten_graph *graph, odin3_module *mod) {
    uint32_t n_order = 0;
    const uint32_t *order = odin3_flatten_graph_order(graph, &n_order);
    for (uint32_t i = 0; i < n_order; i++) {
        if (order[i] == odin3_module_id_of(mod).v) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* --- tests --------------------------------------------------------------------------------- */

static void test_graph_map_types(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(fix.design, "read_blif",
                                         odin3_bytes_cstr(ODIN3_BLIF_DIR "/hand_body.blif")));
    odin3_module *top = odin3_module_get(fix.design, odin3_design_top(fix.design));
    odin3_flatten_graph *graph = build(top);
    odin3_module *sub = odin3_module_get(fix.design, (odin3_module_id){2});
    TEST_ASSERT_EQUAL_UINT32(2, odin3_flatten_graph_module_of(graph, odin3_module_celltype(sub)));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_flatten_graph_module_of(graph, odin3_module_celltype(top)));
    /* the bb stub is a black box, the $_AND_ a bit cell: neither is an instance */
    odin3_flatten_totals tot = odin3_flatten_graph_totals(graph);
    TEST_ASSERT_EQUAL_UINT64(1, tot.instances);
    TEST_ASSERT_EQUAL_UINT32(2, tot.reach);
    TEST_ASSERT_EQUAL_UINT32(1, tot.roots);
    odin3_flatten_graph_free(graph);
}

static void test_graph_kahn_order(void) {
    odin3_module *dd = ff_module(&fix, "d");
    odin3_module *bb = make_empty_with("b", &dd, 1);
    odin3_module *cc = make_empty_with("c", &dd, 1);
    odin3_module *kids[2] = {bb, cc};
    odin3_module *aa = make_empty_with("a", kids, 2);
    odin3_flatten_graph *graph = build(aa);
    TEST_ASSERT_EQUAL_UINT32(0, position(graph, aa));
    TEST_ASSERT_TRUE(position(graph, bb) < position(graph, dd));
    TEST_ASSERT_TRUE(position(graph, cc) < position(graph, dd));
    TEST_ASSERT_EQUAL_UINT64(4, odin3_flatten_graph_totals(graph).instances); /* b, c, d twice */
    odin3_flatten_graph_free(graph);
}

static void test_graph_cycle_a_b_a(void) {
    odin3_module *aa = ff_module(&fix, "a");
    odin3_module *bb = make_empty_with("b", &aa, 1);
    (void)ff_inst(&fix, (ff_pair){aa, bb}, "ub", NULL);
    odin3_flatten_graph *graph = NULL;
    uint32_t nodes = odin3_module_node_end(aa);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_flatten_graph_build(fix.design, odin3_module_id_of(aa), &graph));
    TEST_ASSERT_NULL(graph);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: recursive instantiation: a -> b -> a"));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(aa));
}

static void test_graph_self_cycle(void) {
    odin3_module *aa = ff_module(&fix, "a");
    (void)ff_inst(&fix, (ff_pair){aa, aa}, "me", NULL);
    odin3_flatten_graph *graph = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_flatten_graph_build(fix.design, odin3_module_id_of(aa), &graph));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: recursive instantiation: a -> a"));
}

static void test_graph_cycle_via_kept(void) {
    odin3_module *aa = ff_module(&fix, "a");
    odin3_module *bb = make_empty_with("b", &aa, 1);
    odin3_node_id inst = ff_inst(&fix, (ff_pair){aa, bb}, "ub", NULL);
    ff_attr_int(&fix, aa, (odin3_objref){ODIN3_OBJ_NODE, inst.v}, "keep_hierarchy", 1);
    odin3_flatten_graph *graph = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_flatten_graph_build(fix.design, odin3_module_id_of(aa), &graph));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "recursive instantiation: a -> b -> a"));
}

/* 3 levels, fan-out 3: leaf (1 gate), mid (3 leaves), top (3 mids). */
static odin3_module *make_tree(void) {
    odin3_module *leaf = make_leaf("leaf");
    odin3_module *mid = make_level("mid", leaf, 3);
    odin3_module *top = make_level("top", mid, 3);
    set_top(top);
    return top;
}

static void test_graph_sizes(void) {
    odin3_module *top = make_tree();
    odin3_flatten_graph *graph = build(top);
    odin3_flatten_size size = odin3_flatten_graph_size(graph, odin3_module_id_of(top).v);
    TEST_ASSERT_EQUAL_UINT64(9, size.nodes);
    TEST_ASSERT_EQUAL_UINT64(18, size.pins);
    TEST_ASSERT_EQUAL_UINT64(38, size.nets);
    TEST_ASSERT_EQUAL_UINT64(26, size.wires);
    TEST_ASSERT_EQUAL_UINT64(12, size.instances);
    odin3_flatten_totals tot = odin3_flatten_graph_totals(graph);
    TEST_ASSERT_EQUAL_UINT64(12, tot.instances);
    TEST_ASSERT_EQUAL_UINT64(0, tot.kept);
    TEST_ASSERT_EQUAL_UINT32(3, tot.reach);
    TEST_ASSERT_EQUAL_UINT32(2, tot.unused);
    TEST_ASSERT_TRUE(odin3_flatten_graph_estimate(graph) > 0);
    odin3_flatten_graph_free(graph);
}

static void test_graph_sizes_saturate(void) {
    odin3_module *prev = ff_module(&fix, "m0");
    odin3_net_id gate_nets[2] = {ff_net(&fix, prev, NULL), ff_net(&fix, prev, NULL)};
    (void)ff_gate(&fix, prev, (ff_cell){"$_NOT_", "g"}, gate_nets);
    for (uint32_t i = 1; i <= SAT_LEVELS; i++) {
        char name[NAME_BUF];
        numbered(name, sizeof name, "m", i);
        odin3_module *kids[2] = {prev, prev};
        prev = make_empty_with(name, kids, 2);
    }
    odin3_flatten_graph *graph = build(prev);
    odin3_flatten_size size = odin3_flatten_graph_size(graph, odin3_module_id_of(prev).v);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, size.nodes);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, size.instances);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, odin3_flatten_graph_estimate(graph));
    odin3_flatten_graph_free(graph);
}

static void test_caps_trip(void) {
    odin3_module *top = make_tree();
    odin3_flatten_limits tiny = {2, 0, 0, 0, 0};
    odin3_flatten_test_set_limits(&tiny);
    uint32_t nodes = odin3_module_node_end(top);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_flatten_run(&fix.ctx, fix.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: the flat design would have 9 nodes (cap 2)"));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top));
    TEST_ASSERT_EQUAL_UINT32(5, ff_live_nodes(top)); /* 3 instances + 2 port nodes: untouched */
}

static void test_budget_trip(void) {
    odin3_module *top = make_tree();
    uint32_t nodes = odin3_module_node_end(top);
    odin3_budget_test_set_limit(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_flatten_run(&fix.ctx, fix.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: flatten needs more than 1 bytes"));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top));
}

/* --- keep_hierarchy ------------------------------------------------------------------------- */

static bool kept_with(const odin3_value *val, bool on_module) {
    static uint32_t serial;
    char name[NAME_BUF];
    numbered(name, sizeof name, "c", serial);
    odin3_module *chd = make_leaf(name);
    numbered(name, sizeof name, "p", serial++);
    odin3_module *par = ff_module(&fix, name);
    odin3_node_id inst = ff_inst(&fix, (ff_pair){par, chd}, "u", (odin3_net_id[2]){{0}, {0}});
    uint32_t key = ff_str(&fix, "keep_hierarchy");
    odin3_objref obj = on_module ? (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(chd).v}
                                 : (odin3_objref){ODIN3_OBJ_NODE, inst.v};
    if (val != NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(on_module ? chd : par, obj, key, val));
    }
    return odin3_flatten_is_kept(par, inst, chd);
}

typedef struct kept_case {
    odin3_value value;
    bool kept;
} kept_case;

static void test_kept_rule_values(void) {
    static const uint8_t zero[1] = {ODIN3_BIT_0};
    static const uint8_t one[1] = {ODIN3_BIT_1};
    /* string 0 is the empty string; "x" is interned first */
    const kept_case cases[] = {{{ODIN3_VAL_INT, 0, NULL, 0, 0, 0}, false},
                               {{ODIN3_VAL_INT, 1, NULL, 0, 0, 0}, true},
                               {{ODIN3_VAL_BITS, 0, zero, 1, 0, 0}, false},
                               {{ODIN3_VAL_BITS, 0, one, 1, 0, 0}, true},
                               {{ODIN3_VAL_STRING, 0, NULL, 0, 0, 0}, false},
                               {{ODIN3_VAL_STRING, 0, NULL, 0, ff_str(&fix, "x"), 0}, true}};
    for (int on_module = 0; on_module <= 1; on_module++) {
        TEST_ASSERT_FALSE(kept_with(NULL, on_module != 0));
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            TEST_ASSERT_EQUAL_INT(cases[i].kept ? 1 : 0,
                                  kept_with(&cases[i].value, on_module != 0) ? 1 : 0);
        }
    }
}

static void test_roots_include_kept(void) {
    odin3_module *leaf = make_leaf("leaf");
    odin3_module *kmod = make_level("kmod", leaf, 1);
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_inst(&fix, (ff_pair){top, kmod}, "k", (odin3_net_id[2]){{0}, {0}});
    ff_attr_int(&fix, kmod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(kmod).v},
                "keep_hierarchy", 1);
    set_top(top);
    odin3_flatten_graph *graph = build(top);
    uint32_t n_roots = 0;
    const uint32_t *roots = odin3_flatten_graph_roots(graph, &n_roots);
    TEST_ASSERT_EQUAL_UINT32(2, n_roots);
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(kmod).v, roots[0]); /* module-ID order */
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top).v, roots[1]);
    odin3_flatten_totals tot = odin3_flatten_graph_totals(graph);
    TEST_ASSERT_EQUAL_UINT64(1, tot.instances); /* leaf inside kmod */
    TEST_ASSERT_EQUAL_UINT64(1, tot.kept);
    odin3_flatten_size top_size = odin3_flatten_graph_size(graph, odin3_module_id_of(top).v);
    TEST_ASSERT_EQUAL_UINT64(1, top_size.nodes); /* the kept instance itself */
    odin3_flatten_graph_free(graph);
}

static void test_kept_on_top_ignored(void) {
    odin3_module *leaf = make_leaf("leaf");
    odin3_module *top = make_level("top", leaf, 1);
    ff_attr_int(&fix, top, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(top).v},
                "keep_hierarchy", 1);
    odin3_flatten_graph *graph = build(top);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_flatten_graph_totals(graph).roots);
    TEST_ASSERT_EQUAL_UINT64(1, odin3_flatten_graph_totals(graph).instances);
    odin3_flatten_graph_free(graph);
}

static void test_unused_modules_listed(void) {
    odin3_module *kids[MANY_UNUSED];
    for (uint32_t i = 0; i < MANY_UNUSED; i++) {
        char name[NAME_BUF];
        numbered(name, sizeof name, "c", i);
        kids[i] = ff_module(&fix, name);
    }
    odin3_module *top = make_empty_with("top", kids, MANY_UNUSED);
    odin3_flatten_graph *graph = build(top);
    odin3_flatten_graph_log_unused(graph);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "10 modules are no longer instantiated: c0, c1, c2, c3, "
                                          "c4, c5, c6, c7, ..."));
    TEST_ASSERT_NULL(strstr(log_text, "c8"));
    odin3_flatten_graph_free(graph);
}

/* --- options, registration ------------------------------------------------------------------ */

static odin3_status parse(const char *text, odin3_flatten_opts *opts, odin3_strbuf *sep) {
    return odin3_flatten_parse_args(odin3_bytes_cstr(text), sep, opts);
}

static void test_opts_parse(void) {
    odin3_strbuf sep;
    odin3_strbuf_init(&sep);
    odin3_flatten_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("", &opts, &sep));
    TEST_ASSERT_NULL(opts.separator);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--separator \"::\"", &opts, &sep));
    TEST_ASSERT_EQUAL_STRING("::", opts.separator);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--separator _", &opts, &sep));
    TEST_ASSERT_EQUAL_STRING("_", opts.separator);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--separator \"\"", &opts, &sep));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--separator \"a b\"", &opts, &sep));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--separator", &opts, &sep));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--separator '#'", &opts, &sep));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--bogus", &opts, &sep));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: unknown option '--bogus'"));
    odin3_strbuf_free(&sep);
}

static void test_no_top(void) {
    (void)make_leaf("leaf");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_flatten_run(&fix.ctx, fix.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: the design has no top module (run hierarchy)"));
}

static void test_run_counts_instances(void) {
    (void)make_tree();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_flatten_run(&fix.ctx, fix.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: 12 instances to expand"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "2 modules are no longer instantiated: leaf, mid"));
}

static void test_refuse_processes(void) {
    pb_result res;
    const char *text = "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                       "  wire output 3 \\q\n  process $p\n    sync posedge \\clk\n"
                       "      update \\q \\d\n  end\nend\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(res.design, (odin3_module_id){1}));
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, odin3_pass_run(res.design, "flatten", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has processes"));
    pb_free(&res);
}

/* One build with the allocation failing after `fail_at`; true when it was injected. */
static bool build_fails_at(odin3_module *top, long fail_at, odin3_flatten_graph **graph) {
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_flatten_graph_build(fix.design, odin3_module_id_of(top), graph);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_NULL(*graph);
    return true;
}

static void test_graph_oom_sweep(void) {
    odin3_module *top = make_tree();
    odin3_flatten_graph *graph = NULL;
    long fail_at = 0;
    while (fail_at < OOM_LIMIT && build_fails_at(top, fail_at, &graph)) {
        fail_at++;
    }
    TEST_ASSERT_TRUE(fail_at > 0);
    TEST_ASSERT_NOT_NULL(graph);
    odin3_flatten_graph_free(graph);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_graph_map_types);
    RUN_TEST(test_graph_kahn_order);
    RUN_TEST(test_graph_cycle_a_b_a);
    RUN_TEST(test_graph_self_cycle);
    RUN_TEST(test_graph_cycle_via_kept);
    RUN_TEST(test_graph_sizes);
    RUN_TEST(test_graph_sizes_saturate);
    RUN_TEST(test_caps_trip);
    RUN_TEST(test_budget_trip);
    RUN_TEST(test_kept_rule_values);
    RUN_TEST(test_roots_include_kept);
    RUN_TEST(test_kept_on_top_ignored);
    RUN_TEST(test_unused_modules_listed);
    RUN_TEST(test_opts_parse);
    RUN_TEST(test_no_top);
    RUN_TEST(test_run_counts_instances);
    RUN_TEST(test_refuse_processes);
    RUN_TEST(test_graph_oom_sweep);
    return UNITY_END();
}
