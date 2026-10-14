/*
 * test_flatten_expand.c — 3A Task 3: the expansion (joins, names, attributes, constants, MEMID,
 * errors, provenance, determinism, out of memory; spec §3-§9, §12, §14.1; plan Task 3).
 */
#include "flatten_fixture.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/flatten/flatten.h"
#include "passes/flatten/flatten_test.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, OOM_LIMIT = 6000, NAME_BUF = 32, PAD_NETS = 7 };

static ff fix;
static char log_text[LOG_TEXT];

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
    (void)odin3_log_set_level(ODIN3_LOG_DEBUG);
    ff_begin(&fix);
}

void tearDown(void) {
    odin3_flatten_test_set_limits(NULL);
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    (void)odin3_log_set_level(ODIN3_LOG_INFO);
    ff_end(&fix);
}

/* --- builders ------------------------------------------------------------------------------ */

static uint32_t str(const char *text) {
    return ff_str(&fix, text);
}

/* sub: ports a (in), y (out); wire t; NOT n1: a -> t; NOT n2: t -> y. */
static odin3_module *make_sub(const char *name, odin3_prov_id gate_prov) {
    odin3_module *mod = ff_module(&fix, name);
    (void)ff_port(&fix, mod, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mod, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id net_t = ff_wire(&fix, mod, "t", 1);
    odin3_net_id first[2] = {ff_wnet(&fix, mod, "a", 0), net_t};
    odin3_net_id second[2] = {net_t, ff_wnet(&fix, mod, "y", 0)};
    fix.prov = gate_prov;
    (void)ff_gate(&fix, mod, (ff_cell){"$_NOT_", "n1"}, first);
    fix.prov = gate_prov;
    (void)ff_gate(&fix, mod, (ff_cell){"$_NOT_", "n2"}, second);
    return mod;
}

/* top: ports x (in), z (out). */
static odin3_module *make_top(void) {
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 1);
    return top;
}

static odin3_node_id inst_xz(odin3_module *top, odin3_module *sub, const char *name) {
    odin3_net_id nets[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", 0)};
    return ff_inst(&fix, (ff_pair){top, sub}, name, nets);
}

static void set_top(odin3_module *top) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(fix.design, odin3_module_id_of(top)));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fix.design, opts));
}

static odin3_status flatten_top(odin3_module *top) {
    set_top(top);
    return odin3_flatten_run(&fix.ctx, fix.design, NULL);
}

static void flatten_ok(odin3_module *top) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, flatten_top(top), log_text);
    check_full();
}

static bool has_node(const odin3_module *mod, const char *name) {
    return odin3_node_valid(odin3_module_find_node(mod, str(name)));
}

static bool has_wire(const odin3_module *mod, const char *name) {
    return odin3_wire_valid(odin3_module_find_wire(mod, str(name)));
}

static uint32_t count_type(const odin3_module *mod, const char *type) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix.design, str(type), &tid));
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        count += odin3_node_live(mod, (odin3_node_id){nid}) &&
                         odin3_node_type(mod, (odin3_node_id){nid}).v == tid.v
                     ? 1
                     : 0;
    }
    return count;
}

static uint32_t count_instances(const odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(fix.design, odin3_node_type(mod, node))->gran == ODIN3_GRAN_MODULE) {
            count++;
        }
    }
    return count;
}

/* The alias list of net as "name:bit" strings joined by ',' ("" when none). */
static void aliases_of(const odin3_module *mod, odin3_net_id net, char *out, size_t size) {
    uint32_t cursor = 0;
    odin3_net_alias alias;
    out[0] = '\0';
    while (odin3_net_alias_next(mod, net, &cursor, &alias)) {
        if (alias.wb.wire.v == 0) {
            continue;
        }
        const char *name =
            odin3_strtab_get(odin3_design_strtab(fix.design), odin3_wire_name(mod, alias.wb.wire));
        size_t used = strlen(out);
        (void)snprintf(out + used, size - used, "%s%s:%u", used == 0 ? "" : ",", name,
                       alias.wb.bit);
    }
}

/* --- tests --------------------------------------------------------------------------------- */

typedef struct name_lists {
    const char *const *nodes;
    const char *const *wires;
} name_lists;

static void assert_names(const odin3_module *mod, name_lists lists) {
    for (size_t i = 0; lists.nodes[i] != NULL; i++) {
        TEST_ASSERT_TRUE_MESSAGE(has_node(mod, lists.nodes[i]), lists.nodes[i]);
    }
    for (size_t i = 0; lists.wires[i] != NULL; i++) {
        TEST_ASSERT_TRUE_MESSAGE(has_wire(mod, lists.wires[i]), lists.wires[i]);
    }
}

static void test_flat_simple(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(0, count_instances(top));
    TEST_ASSERT_EQUAL_UINT32(2, count_type(top, "$_NOT_"));
    assert_names(top, (name_lists){(const char *const[]){"u1.n1", "u1.n2", NULL},
                                   (const char *const[]){"u1.a", "u1.t", NULL}});
    TEST_ASSERT_EQUAL_UINT32(ff_wnet(&fix, top, "x", 0).v,
                             odin3_wire_net(top, odin3_module_find_wire(top, str("u1.a")), 0).v);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: 1 instances to expand"));
}

static void test_flat_alias_order(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    char list[NAME_BUF * 2];
    aliases_of(top, ff_wnet(&fix, top, "x", 0), list, sizeof list);
    TEST_ASSERT_EQUAL_STRING("u1.a:0", list);
    aliases_of(top, ff_wnet(&fix, top, "z", 0), list, sizeof list);
    TEST_ASSERT_EQUAL_STRING("u1.y:0", list);
}

static void test_flat_nested_names(void) {
    odin3_module *leaf = make_sub("leaf", (odin3_prov_id){0});
    odin3_module *mid = ff_module(&fix, "mid");
    (void)ff_port(&fix, mid, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mid, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id nets[2] = {ff_wnet(&fix, mid, "a", 0), ff_wnet(&fix, mid, "y", 0)};
    (void)ff_inst(&fix, (ff_pair){mid, leaf}, "b", nets);
    odin3_module *top = make_top();
    (void)inst_xz(top, mid, "a");
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(0, count_instances(top));
    assert_names(top, (name_lists){(const char *const[]){"a.b.n1", NULL},
                                   (const char *const[]){"a.b.t", "a.a", NULL}});
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: 2 instances to expand"));
}

static void test_flat_two_instances(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 2);
    for (uint32_t i = 0; i < 2; i++) {
        odin3_net_id nets[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        (void)ff_inst(&fix, (ff_pair){top, sub}, i == 0 ? "u1" : "u2", nets);
    }
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(4, count_type(top, "$_NOT_"));
    TEST_ASSERT_TRUE(has_node(top, "u2.n2"));
    TEST_ASSERT_NOT_EQUAL(odin3_wire_net(top, odin3_module_find_wire(top, str("u1.t")), 0).v,
                          odin3_wire_net(top, odin3_module_find_wire(top, str("u2.t")), 0).v);
}

static void test_flat_unconnected(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)ff_inst(&fix, (ff_pair){top, sub}, "u1", (odin3_net_id[2]){{0}, {0}});
    flatten_ok(top);
    odin3_net_id in_net = odin3_wire_net(top, odin3_module_find_wire(top, str("u1.a")), 0);
    odin3_net_id out_net = odin3_wire_net(top, odin3_module_find_wire(top, str("u1.y")), 0);
    TEST_ASSERT_TRUE(odin3_net_valid(in_net));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(top, in_net));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(top, out_net).count);
}

static void test_flat_constants(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)ff_port(&fix, sub, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, sub, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id nets[3] = {ff_wnet(&fix, sub, "a", 0), ff_const(&fix, sub, 1),
                            ff_wnet(&fix, sub, "y", 0)};
    (void)ff_gate(&fix, sub, (ff_cell){"$_AND_", "g"}, nets);
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 3);
    for (uint32_t i = 0; i < 3; i++) {
        char name[NAME_BUF];
        (void)snprintf(name, sizeof name, "u%u", i);
        odin3_net_id io[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        (void)ff_inst(&fix, (ff_pair){top, sub}, name, io);
    }
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(3, count_type(top, "$_AND_"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type(top, "$_CONST1_"));
}

static void test_flat_multidriver_error(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    (void)inst_xz(top, sub, "u2");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has 2 drivers after joining port 'y'"));
}

static void test_flat_const_output_error(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)ff_port(&fix, sub, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id one = ff_const(&fix, sub, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_merge(sub, (odin3_net_pair){ff_wnet(&fix, sub, "y", 0), one}));
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 1);
    odin3_net_id zz = ff_wnet(&fix, top, "z", 0);
    (void)ff_inst(&fix, (ff_pair){top, sub}, "u1", &zz);
    odin3_net_id gate_nets[2] = {ff_net(&fix, top, NULL), zz};
    (void)ff_gate(&fix, top, (ff_cell){"$_NOT_", "other"}, gate_nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "which is driven by a constant"));
}

/* The child's net IDs (1..3) cannot coincide with the root's z (padding nets come first): the
 * join check must map the child net to the root net (review C1). */
static void test_flat_multidriver_ids_differ(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = ff_module(&fix, "top");
    for (int i = 0; i < PAD_NETS; i++) {
        (void)ff_net(&fix, top, NULL);
    }
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 1);
    TEST_ASSERT_TRUE(ff_wnet(&fix, top, "z", 0).v > (uint32_t)PAD_NETS);
    (void)inst_xz(top, sub, "u1");
    (void)inst_xz(top, sub, "u2");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has 2 drivers after joining port 'y'"));
}

static void test_flat_const_output_ids_differ(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)ff_port(&fix, sub, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id one = ff_const(&fix, sub, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_merge(sub, (odin3_net_pair){ff_wnet(&fix, sub, "y", 0), one}));
    odin3_module *top = ff_module(&fix, "top");
    for (int i = 0; i < PAD_NETS; i++) {
        (void)ff_net(&fix, top, NULL);
    }
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 1);
    odin3_net_id zz = ff_wnet(&fix, top, "z", 0);
    (void)ff_inst(&fix, (ff_pair){top, sub}, "u1", &zz);
    odin3_net_id gate_nets[2] = {ff_net(&fix, top, NULL), zz};
    (void)ff_gate(&fix, top, (ff_cell){"$_NOT_", "other"}, gate_nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "which is driven by a constant"));
}

static void test_flat_feedthrough(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)ff_port(&fix, sub, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, sub, "y", ODIN3_DIR_OUT, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_merge(sub, (odin3_net_pair){ff_wnet(&fix, sub, "a", 0),
                                                                ff_wnet(&fix, sub, "y", 0)}));
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(ff_wnet(&fix, top, "x", 0).v, ff_wnet(&fix, top, "z", 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, count_instances(top));
    char list[NAME_BUF * 3];
    aliases_of(top, ff_wnet(&fix, top, "x", 0), list, sizeof list);
    TEST_ASSERT_NOT_NULL(strstr(list, "u1.a:0"));
    TEST_ASSERT_NOT_NULL(strstr(list, "u1.y:0"));
}

static void test_flat_keep_hierarchy_inst(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    odin3_node_id kept = inst_xz(top, sub, "k");
    ff_attr_int(&fix, top, (odin3_objref){ODIN3_OBJ_NODE, kept.v}, "keep_hierarchy", 1);
    (void)ff_inst(&fix, (ff_pair){top, sub}, "e", (odin3_net_id[2]){{0}, {0}});
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(1, count_instances(top));
    TEST_ASSERT_TRUE(has_node(top, "k"));
    TEST_ASSERT_TRUE(has_node(top, "e.n1"));
    /* the kept instance's module is a root: sub itself has nothing to expand and is unchanged */
    TEST_ASSERT_EQUAL_UINT32(2, count_type(sub, "$_NOT_"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: keeping 'k'"));
}

static void test_flat_keep_hierarchy_module(void) {
    odin3_module *leaf = make_sub("leaf", (odin3_prov_id){0});
    odin3_module *kmod = ff_module(&fix, "kmod");
    (void)ff_port(&fix, kmod, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, kmod, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id io[2] = {ff_wnet(&fix, kmod, "a", 0), ff_wnet(&fix, kmod, "y", 0)};
    (void)ff_inst(&fix, (ff_pair){kmod, leaf}, "b", io);
    ff_attr_int(&fix, kmod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(kmod).v},
                "keep_hierarchy", 1);
    odin3_module *top = make_top();
    (void)inst_xz(top, kmod, "k");
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(1, count_instances(top));
    TEST_ASSERT_EQUAL_UINT32(0,
                             count_instances(kmod)); /* a kept module's own instances: in place */
    TEST_ASSERT_TRUE(has_node(kmod, "b.n1"));
}

static void test_flat_attrs(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_node_id n1 = odin3_module_find_node(sub, str("n1"));
    ff_attr_int(&fix, sub, (odin3_objref){ODIN3_OBJ_NODE, n1.v}, "keep", 1);
    ff_attr_int(&fix, sub, (odin3_objref){ODIN3_OBJ_NODE, n1.v}, "init", 5);
    ff_attr_int(&fix, sub, (odin3_objref){ODIN3_OBJ_WIRE, odin3_module_find_wire(sub, str("a")).v},
                "blif_name", 1);
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    odin3_node_id copy = odin3_module_find_node(top, str("u1.n1"));
    const odin3_value *keep =
        odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_NODE, copy.v}, str("keep"));
    TEST_ASSERT_NOT_NULL(keep);
    TEST_ASSERT_EQUAL_INT64(1, keep->i);
    TEST_ASSERT_NOT_NULL(odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_NODE, copy.v}, str("init")));
    odin3_wire_id wire = odin3_module_find_wire(top, str("u1.a"));
    TEST_ASSERT_NULL(odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, str("blif_name")));
}

static void test_flat_names_unique(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)ff_wire(&fix, top, "u1.t", 1); /* an escaped identifier that clashes with u1.t */
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    TEST_ASSERT_TRUE(has_wire(top, "u1.t"));
    TEST_ASSERT_TRUE(has_wire(top, "u1.t_1"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: renamed 'u1.t_1' (name taken)"));
}

static void test_flat_unnamed_instance(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    odin3_node_id inst = inst_xz(top, sub, NULL);
    char want[NAME_BUF];
    (void)snprintf(want, sizeof want, "$c%u.n1", inst.v);
    flatten_ok(top);
    TEST_ASSERT_TRUE(has_node(top, want));
}

/* --- provenance ----------------------------------------------------------------------------- */

static const char *hier_text(odin3_prov_id id) {
    return odin3_strtab_get(odin3_design_strtab(fix.design), odin3_prov_get(fix.design, id)->hier);
}

static void test_flat_prov(void) {
    odin3_prov_id gate_src = ff_source(&fix, "sub.v", 3, "sub/gen[3]");
    odin3_prov_id inst_src = ff_source(&fix, "top.v", 7, "top");
    odin3_module *sub = make_sub("sub", gate_src);
    odin3_module *top = make_top();
    fix.prov = inst_src;
    (void)inst_xz(top, sub, "u1");
    fix.prov = inst_src;
    (void)inst_xz(top, sub, "u2");
    odin3_net_id extra = ff_net(&fix, top, NULL);
    (void)extra;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top)); /* u1 and u2 both drive z */
    odin3_node_id copy = odin3_module_find_node(top, str("u1.n1"));
    TEST_ASSERT_TRUE(odin3_node_valid(copy));
    odin3_prov_id rec = odin3_node_prov(top, copy);
    const odin3_prov_record *rr = odin3_prov_get(fix.design, rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rr->kind);
    TEST_ASSERT_EQUAL_STRING("top/u1/gen[3]", hier_text(rec));
    TEST_ASSERT_EQUAL_UINT32(2, rr->parents.count);
    TEST_ASSERT_EQUAL_UINT32(gate_src.v, rr->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(inst_src.v, rr->parents.ids[1].v);
}

static void test_flat_prov_distinct_instances(void) {
    odin3_prov_id gate_src = ff_source(&fix, "sub.v", 3, "sub");
    odin3_prov_id inst_src = ff_source(&fix, "top.v", 7, "top");
    odin3_module *sub = make_sub("sub", gate_src);
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 2);
    for (uint32_t i = 0; i < 2; i++) {
        odin3_net_id io[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        fix.prov = inst_src;
        (void)ff_inst(&fix, (ff_pair){top, sub}, i == 0 ? "u1" : "u2", io);
    }
    flatten_ok(top);
    odin3_prov_id r1 = odin3_node_prov(top, odin3_module_find_node(top, str("u1.n1")));
    odin3_prov_id r2 = odin3_node_prov(top, odin3_module_find_node(top, str("u2.n1")));
    TEST_ASSERT_NOT_EQUAL(r1.v, r2.v);
    TEST_ASSERT_EQUAL_STRING("top/u1", hier_text(r1));
    TEST_ASSERT_EQUAL_STRING("top/u2", hier_text(r2));
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(top, odin3_module_find_node(top, str("u1.n2"))).v,
                             r1.v); /* hash-consed: same parents, op and path */
}

static void test_flat_prov_nested(void) {
    odin3_prov_id leaf_src = ff_source(&fix, "leaf.v", 2, "leaf");
    odin3_prov_id b_src = ff_source(&fix, "mid.v", 5, "mid");
    odin3_prov_id a_src = ff_source(&fix, "top.v", 9, "top");
    odin3_module *leaf = make_sub("leaf", leaf_src);
    odin3_module *mid = ff_module(&fix, "mid");
    (void)ff_port(&fix, mid, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mid, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id io[2] = {ff_wnet(&fix, mid, "a", 0), ff_wnet(&fix, mid, "y", 0)};
    fix.prov = b_src;
    (void)ff_inst(&fix, (ff_pair){mid, leaf}, "b", io);
    odin3_module *top = make_top();
    fix.prov = a_src;
    (void)inst_xz(top, mid, "a");
    flatten_ok(top);
    odin3_prov_id rec = odin3_node_prov(top, odin3_module_find_node(top, str("a.b.n1")));
    const odin3_prov_record *rr = odin3_prov_get(fix.design, rec);
    TEST_ASSERT_EQUAL_STRING("top/a/b", hier_text(rec));
    TEST_ASSERT_EQUAL_UINT32(leaf_src.v, rr->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(b_src.v, rr->parents.ids[1].v);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, odin3_prov_get(fix.design, rr->parents.ids[1])->kind);
}

/* --- determinism, out of memory --------------------------------------------------------------- */

static void test_flat_determinism(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 2);
    for (uint32_t i = 0; i < 2; i++) {
        odin3_net_id io[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        (void)ff_inst(&fix, (ff_pair){top, sub}, i == 0 ? "u1" : "u2", io);
    }
    flatten_ok(top);
    char first[NAME_BUF * 4];
    first[0] = '\0';
    for (uint32_t nid = 1; nid < odin3_module_node_end(top); nid++) {
        size_t used = strlen(first);
        (void)snprintf(first + used, sizeof first - used, "%u:%u,", nid,
                       odin3_node_name(top, (odin3_node_id){nid}));
    }
    /* the second design, built identically, gives the same node/name table */
    ff_end(&fix);
    ff_begin(&fix);
    sub = make_sub("sub", (odin3_prov_id){0});
    top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 2);
    for (uint32_t i = 0; i < 2; i++) {
        odin3_net_id io[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        (void)ff_inst(&fix, (ff_pair){top, sub}, i == 0 ? "u1" : "u2", io);
    }
    flatten_ok(top);
    char second[NAME_BUF * 4];
    second[0] = '\0';
    for (uint32_t nid = 1; nid < odin3_module_node_end(top); nid++) {
        size_t used = strlen(second);
        (void)snprintf(second + used, sizeof second - used, "%u:%u,", nid,
                       odin3_node_name(top, (odin3_node_id){nid}));
    }
    TEST_ASSERT_EQUAL_STRING(first, second);
}

static void build_simple(odin3_module **top) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    *top = make_top();
    (void)inst_xz(*top, sub, "u1");
    set_top(*top);
}

static void test_flat_oom(void) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        ff_end(&fix);
        ff_begin(&fix);
        odin3_module *top = NULL;
        build_simple(&top);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_flatten_run(&fix.ctx, fix.design, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            check_full();
            failures++;
        }
    }
    TEST_ASSERT_TRUE(failures > 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

/* --- the rest of spec 14.1: buses, memories, parameters, names, hand-written BLIF ---------------
 */

/* The first index of the parameter called name in def; asserts it exists. */
static uint32_t param_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

enum { MAX_WORD_NETS = 64, MAX_WORD_PORTS = 16 };

typedef struct word_spec {
    const char *type;
    const char *name;
    const char *memid; /* NULL: the type has no MEMID */
    const char *const *int_names;
    const int64_t *int_values;
    uint32_t n_ints;
} word_spec;

/* The parameters of the spec: defaults, then its INTs and MEMID. */
static void word_params(const odin3_celltype_def *def, const word_spec *spec, odin3_value *params) {
    TEST_ASSERT_TRUE(def->n_params <= MAX_WORD_PORTS * 2);
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
    }
    for (uint32_t i = 0; i < spec->n_ints; i++) {
        params[param_index(def, spec->int_names[i])] = odin3_value_int(spec->int_values[i]);
    }
    if (spec->memid != NULL) {
        odin3_value *memid = &params[param_index(def, "MEMID")];
        memid->kind = ODIN3_VAL_STRING;
        memid->str = str(spec->memid);
    }
}

typedef struct word_ports {
    odin3_net_id nets[MAX_WORD_NETS];
    odin3_netvec ports[MAX_WORD_PORTS];
} word_ports;

/* Every pin of the type, with the given parameters, on a fresh unnamed net of mod. */
static void word_nets(odin3_module *mod, odin3_celltype_id tid, const odin3_value *params,
                      word_ports *out) {
    const odin3_celltype_def *def = odin3_celltype_get(fix.design, tid);
    uint32_t used = 0;
    TEST_ASSERT_TRUE(def->n_ports <= MAX_WORD_PORTS);
    for (uint32_t port = 0; port < def->n_ports; port++) {
        uint32_t width = odin3_celltype_port_width(fix.design, tid, params, port);
        TEST_ASSERT_TRUE(used + width <= MAX_WORD_NETS);
        for (uint32_t bit = 0; bit < width; bit++) {
            out->nets[used + bit] = ff_net(&fix, mod, NULL);
        }
        out->ports[port] = (odin3_netvec){&out->nets[used], width};
        used += width;
    }
}

/* A word-level node of a built-in type in mod with the given INT parameters (the rest defaults)
 * and every pin on a fresh unnamed net. */
static odin3_node_id make_word(odin3_module *mod, const word_spec *spec) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix.design, str(spec->type), &tid));
    const odin3_celltype_def *def = odin3_celltype_get(fix.design, tid);
    odin3_value params[MAX_WORD_PORTS * 2];
    word_params(def, spec, params);
    word_ports conn;
    word_nets(mod, tid, params, &conn);
    odin3_node_spec node = {tid, spec->name != NULL ? str(spec->name) : 0, fix.prov, params,
                            def->n_params};
    fix.prov = (odin3_prov_id){0};
    odin3_node_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &node, conn.ports, &out));
    return out;
}

/* The MEMID text of the $mem node `name` of mod. */
static const char *memid_of(const odin3_module *mod, const char *name) {
    odin3_node_id node = odin3_module_find_node(mod, str(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    const odin3_celltype_def *def = odin3_celltype_get(fix.design, odin3_node_type(mod, node));
    uint32_t at = param_index(def, "MEMID");
    return odin3_strtab_get(odin3_design_strtab(fix.design), odin3_node_param(mod, node, at)->str);
}

/* The two-sided bus: sub drives `io` (inout) through a $tribuf; top drives the same bus through
 * its own $tribuf and through the instance. */
static void test_flat_bus(void) {
    odin3_module *sub = ff_module(&fix, "drv");
    (void)ff_port(&fix, sub, "d", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, sub, "en", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, sub, "io", ODIN3_DIR_INOUT, 1);
    odin3_net_id sub_nets[3] = {ff_wnet(&fix, sub, "d", 0), ff_wnet(&fix, sub, "en", 0),
                                ff_wnet(&fix, sub, "io", 0)};
    (void)ff_gate(&fix, sub, (ff_cell){"$tribuf", "t"}, sub_nets);
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "d", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "en", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "bus", ODIN3_DIR_INOUT, 1);
    odin3_net_id top_nets[3] = {ff_wnet(&fix, top, "d", 0), ff_wnet(&fix, top, "en", 0),
                                ff_wnet(&fix, top, "bus", 0)};
    (void)ff_gate(&fix, top, (ff_cell){"$tribuf", "own"}, top_nets);
    (void)ff_inst(&fix, (ff_pair){top, sub}, "u1", top_nets);
    (void)ff_inst(&fix, (ff_pair){top, sub}, "u2", top_nets);
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(0, count_instances(top));
    TEST_ASSERT_EQUAL_UINT32(3, count_type(top, "$tribuf"));
    TEST_ASSERT_TRUE(has_node(top, "u1.t"));
    TEST_ASSERT_TRUE(has_node(top, "u2.t"));
    TEST_ASSERT_EQUAL_UINT32(ff_wnet(&fix, top, "bus", 0).v,
                             odin3_wire_net(top, odin3_module_find_wire(top, str("u2.io")), 0).v);
    /* three tristate drivers and the inout port node's own pin */
    TEST_ASSERT_EQUAL_UINT32(4, odin3_net_driver_count(top, ff_wnet(&fix, top, "bus", 0)));
}

/* A $mem in `sub`, instantiated twice; the parent already holds a memory whose MEMID is u1.mem. */
static void test_flat_mem_memid(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)make_word(sub, &(word_spec){"$mem", "mem", "mem", NULL, NULL, 0});
    odin3_module *top = ff_module(&fix, "top");
    (void)make_word(top, &(word_spec){"$mem", "own", "u1.mem", NULL, NULL, 0});
    (void)ff_inst_bus(&fix, (ff_pair){top, sub}, "u1", NULL);
    (void)ff_inst_bus(&fix, (ff_pair){top, sub}, "u2", NULL);
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(3, count_type(top, "$mem"));
    TEST_ASSERT_EQUAL_STRING("u1.mem", memid_of(top, "own"));
    TEST_ASSERT_EQUAL_STRING("u1.mem_1", memid_of(top, "u1.mem"));
    TEST_ASSERT_EQUAL_STRING("u2.mem", memid_of(top, "u2.mem"));
    TEST_ASSERT_EQUAL_STRING("mem", memid_of(sub, "mem")); /* the child is untouched */
}

/* A $mem, a $memrd and a $memwr that share MEMID "mem" in `sub`: after flatten the three cells of
 * each instance still share one MEMID, and two instances get two (review I2). */
static void test_flat_memid_shared_by_cells(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    (void)make_word(sub, &(word_spec){"$mem", "m", "mem", NULL, NULL, 0});
    (void)make_word(sub, &(word_spec){"$memrd", "rd", "mem", NULL, NULL, 0});
    (void)make_word(sub, &(word_spec){"$memwr", "wr", "mem", NULL, NULL, 0});
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_inst_bus(&fix, (ff_pair){top, sub}, "u1", NULL);
    (void)ff_inst_bus(&fix, (ff_pair){top, sub}, "u2", NULL);
    flatten_ok(top);
    TEST_ASSERT_EQUAL_STRING("u1.mem", memid_of(top, "u1.m"));
    TEST_ASSERT_EQUAL_STRING("u1.mem", memid_of(top, "u1.rd"));
    TEST_ASSERT_EQUAL_STRING("u1.mem", memid_of(top, "u1.wr"));
    TEST_ASSERT_EQUAL_STRING("u2.mem", memid_of(top, "u2.m"));
    TEST_ASSERT_EQUAL_STRING("u2.mem", memid_of(top, "u2.rd"));
    TEST_ASSERT_EQUAL_STRING("u2.mem", memid_of(top, "u2.wr"));
    TEST_ASSERT_EQUAL_STRING("mem", memid_of(sub, "rd")); /* the child is untouched */
}

/* The widths and signedness of a word cell reach the copy unchanged. */
static void test_flat_params_copied(void) {
    odin3_module *sub = ff_module(&fix, "sub");
    const char *const names[] = {"A_WIDTH", "B_WIDTH", "Y_WIDTH", "A_SIGNED"};
    const int64_t values[] = {3, 2, 4, 1};
    (void)make_word(sub, &(word_spec){"$add", "add", NULL, names, values, 4});
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_inst_bus(&fix, (ff_pair){top, sub}, "u1", NULL);
    flatten_ok(top);
    odin3_node_id orig = odin3_module_find_node(sub, str("add"));
    odin3_node_id copy = odin3_module_find_node(top, str("u1.add"));
    TEST_ASSERT_TRUE(odin3_node_valid(copy));
    const odin3_celltype_def *def = odin3_celltype_get(fix.design, odin3_node_type(top, copy));
    for (uint32_t i = 0; i < def->n_params; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            odin3_value_equal(odin3_node_param(sub, orig, i), odin3_node_param(top, copy, i)),
            def->params[i].name);
    }
    TEST_ASSERT_EQUAL_INT64(4, odin3_node_param(top, copy, param_index(def, "Y_WIDTH"))->i);
}

/* A BLIF-style net name (a net with a name and no wire) becomes `u1.n~19`, a bare net name. */
static void test_flat_net_names(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_net_id bare = ff_net(&fix, sub, "n~19");
    odin3_net_id gate_nets[2] = {ff_wnet(&fix, sub, "a", 0), bare};
    (void)ff_gate(&fix, sub, (ff_cell){"$_NOT_", "n3"}, gate_nets);
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    odin3_net_id net = odin3_module_find_net(top, str("u1.n~19"));
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(top, net));
}

/* Generate-block names keep their brackets and nest under the instance path (`a.g[1].u2`). */
static void test_flat_generate_names(void) {
    odin3_module *leaf = ff_module(&fix, "leaf");
    (void)ff_port(&fix, leaf, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, leaf, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id inner = ff_wire(&fix, leaf, "g[1].t", 1);
    odin3_net_id first[2] = {ff_wnet(&fix, leaf, "a", 0), inner};
    odin3_net_id second[2] = {inner, ff_wnet(&fix, leaf, "y", 0)};
    (void)ff_gate(&fix, leaf, (ff_cell){"$_NOT_", "g[1].u2"}, first);
    (void)ff_gate(&fix, leaf, (ff_cell){"$_NOT_", "g[2].u3"}, second);
    odin3_module *mid = ff_module(&fix, "mid");
    (void)ff_port(&fix, mid, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mid, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id io[2] = {ff_wnet(&fix, mid, "a", 0), ff_wnet(&fix, mid, "y", 0)};
    (void)ff_inst(&fix, (ff_pair){mid, leaf}, "h[0].l", io);
    odin3_module *top = make_top();
    (void)inst_xz(top, mid, "m");
    flatten_ok(top);
    assert_names(top,
                 (name_lists){(const char *const[]){"m.h[0].l.g[1].u2", "m.h[0].l.g[2].u3", NULL},
                              (const char *const[]){"m.h[0].l.g[1].t", NULL}});
}

/* Hand-written BLIF (hand_body): `sub` is expanded, `bb` stays a black-box instance. */
static void read_blif_ok(const char *file) {
    char path[256];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_BLIF_DIR, file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(fix.design, "read_blif", odin3_bytes_cstr(path)));
}

static void test_flat_hand_body(void) {
    read_blif_ok("hand_body.blif");
    odin3_module *top = odin3_module_get(fix.design, odin3_design_top(fix.design));
    TEST_ASSERT_TRUE(has_node(top, "u_sub"));
    TEST_ASSERT_EQUAL_UINT32(1, count_instances(top)); /* sub; the bb stub is a black box */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_flatten_run(&fix.ctx, fix.design, NULL),
                                  log_text);
    check_full();
    TEST_ASSERT_FALSE(has_node(top, "u_sub"));
    uint32_t boxes = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(top); nid++) {
        odin3_node_id node = {nid};
        boxes += odin3_node_live(top, node) &&
                         odin3_celltype_get(fix.design, odin3_node_type(top, node))->gran ==
                             ODIN3_GRAN_BLACKBOX
                     ? 1
                     : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(1, boxes);
}

/* hand_ports has no instance: nothing is expanded, nothing changes. */
static void test_flat_hand_ports(void) {
    read_blif_ok("hand_ports.blif");
    odin3_module *top = odin3_module_get(fix.design, odin3_design_top(fix.design));
    uint32_t nodes = odin3_module_node_end(top);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_flatten_run(&fix.ctx, fix.design, NULL));
    check_full();
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: 0 instances expanded (0 kept)"));
}

/* Recursion through odin3_flatten_run: the text names the cycle, the design is untouched. */
static void test_flat_recursion(void) {
    odin3_module *aa = ff_module(&fix, "a");
    odin3_module *bb = ff_module(&fix, "b");
    (void)ff_inst_bus(&fix, (ff_pair){aa, bb}, "ub", NULL);
    (void)ff_inst_bus(&fix, (ff_pair){bb, aa}, "ua", NULL);
    uint32_t nodes_a = odin3_module_node_end(aa);
    uint32_t nodes_b = odin3_module_node_end(bb);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(aa));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "flatten: recursive instantiation: a -> b -> a"));
    TEST_ASSERT_EQUAL_UINT32(nodes_a, odin3_module_node_end(aa));
    TEST_ASSERT_EQUAL_UINT32(nodes_b, odin3_module_node_end(bb));
}

/* A cap hit through the run: the pre-flight error, the module untouched. */
static void test_flat_caps(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    odin3_flatten_limits tiny = {1, 0, 0, 0, 0};
    odin3_flatten_test_set_limits(&tiny);
    uint32_t live = ff_live_nodes(top);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, flatten_top(top));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "(cap 1)"));
    TEST_ASSERT_EQUAL_UINT32(live, ff_live_nodes(top));
    TEST_ASSERT_EQUAL_UINT32(1, count_instances(top));
}

/* Sets attribute keep_hierarchy of the instance to a STRING (the empty string when text is 0). */
static void keep_string(odin3_module *mod, odin3_node_id inst, uint32_t text) {
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, text, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NODE, inst.v},
                                                   str("keep_hierarchy"), &val));
}

/* Four instances of sub in top: keep_hierarchy INT 1, STRING "yes", INT 0, STRING "". */
static void add_four_instances(odin3_module *sub, odin3_module *top) {
    const char *names[4] = {"k_int", "k_str", "e_int", "e_str"};
    odin3_node_id inst[4];
    for (uint32_t i = 0; i < 4; i++) {
        odin3_net_id io[2] = {ff_wnet(&fix, top, "x", 0), ff_wnet(&fix, top, "z", i)};
        inst[i] = ff_inst(&fix, (ff_pair){top, sub}, names[i], io);
    }
    ff_attr_int(&fix, top, (odin3_objref){ODIN3_OBJ_NODE, inst[0].v}, "keep_hierarchy", 1);
    ff_attr_int(&fix, top, (odin3_objref){ODIN3_OBJ_NODE, inst[2].v}, "keep_hierarchy", 0);
    keep_string(top, inst[1], str("yes"));
    keep_string(top, inst[3], 0);
}

/* The keep_hierarchy value kinds at expansion time: INT 1 and a non-empty STRING keep the
 * instance, INT 0 and the empty string expand it. */
static void test_flat_keep_hierarchy_value_kinds(void) {
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, 4);
    add_four_instances(sub, top);
    flatten_ok(top);
    TEST_ASSERT_EQUAL_UINT32(2, count_instances(top));
    assert_names(top,
                 (name_lists){(const char *const[]){"k_int", "k_str", "e_int.n1", "e_str.n1", NULL},
                              (const char *const[]){"e_int.t", NULL}});
}

/* --- the pre-flight estimate (plan Task 5 calibration) --------------------------------------- */

enum { FAN = 6, EST_LEAVES = FAN * FAN };

/* mid: a -> y[FAN], FAN leaves; top: x -> z[FAN*FAN], FAN mids. 36 leaf instances, 42 in all. */
static odin3_module *make_wide_tree(void) {
    odin3_module *leaf = make_sub("leaf", (odin3_prov_id){0});
    odin3_module *mid = ff_module(&fix, "mid");
    (void)ff_port(&fix, mid, "a", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, mid, "y", ODIN3_DIR_OUT, FAN);
    for (uint32_t i = 0; i < FAN; i++) {
        odin3_net_id io[2] = {ff_wnet(&fix, mid, "a", 0), ff_wnet(&fix, mid, "y", i)};
        char name[NAME_BUF];
        (void)snprintf(name, sizeof name, "l%u", i);
        (void)ff_inst(&fix, (ff_pair){mid, leaf}, name, io);
    }
    odin3_module *top = ff_module(&fix, "top");
    (void)ff_port(&fix, top, "x", ODIN3_DIR_IN, 1);
    (void)ff_port(&fix, top, "z", ODIN3_DIR_OUT, EST_LEAVES);
    for (uint32_t i = 0; i < FAN; i++) {
        odin3_net_id x_net = ff_wnet(&fix, top, "x", 0);
        odin3_net_id z_nets[FAN];
        for (uint32_t bit = 0; bit < FAN; bit++) {
            z_nets[bit] = ff_wnet(&fix, top, "z", i * FAN + bit);
        }
        odin3_netvec ports[2] = {{&x_net, 1}, {z_nets, FAN}};
        char name[NAME_BUF];
        (void)snprintf(name, sizeof name, "m%u", i);
        (void)ff_inst_bus(&fix, (ff_pair){top, mid}, name, ports);
    }
    return top;
}

/* The estimate is never below what the run reserves (spec §12: it rounds up). */
static void test_estimate_not_below_actual(void) {
    odin3_module *top = make_wide_tree();
    set_top(top);
    odin3_flatten_graph *graph = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_flatten_graph_build(fix.design, odin3_module_id_of(top), &graph));
    uint64_t estimate = odin3_flatten_graph_estimate(graph);
    odin3_flatten_graph_free(graph);
    uint64_t before = odin3_design_bytes_reserved(fix.design);
    flatten_ok(top);
    uint64_t grew = odin3_design_bytes_reserved(fix.design) - before;
    TEST_ASSERT_EQUAL_UINT32(0, count_instances(top));
    TEST_ASSERT_EQUAL_UINT32(EST_LEAVES * 2, count_type(top, "$_NOT_"));
    printf("flatten estimate %llu bytes, reserved %llu\n", (unsigned long long)estimate,
           (unsigned long long)grew);
    TEST_ASSERT_TRUE(estimate >= grew);
}

/* The sorted names of the live nodes and wires of mod and the alias lists of its port nets. */
static void name_set(const odin3_module *mod, char *out, size_t size) {
    const odin3_strtab *tab = odin3_design_strtab(fix.design);
    char parts[32][NAME_BUF];
    uint32_t n_parts = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod) && n_parts < 32; nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) && odin3_node_name(mod, node) != 0) {
            (void)snprintf(parts[n_parts++], NAME_BUF, "n:%s:%s",
                           odin3_strtab_get(tab, odin3_node_name(mod, node)),
                           odin3_celltype_get(fix.design, odin3_node_type(mod, node))->name);
        }
    }
    for (uint32_t wid = 1; wid < odin3_module_wire_end(mod) && n_parts < 32; wid++) {
        if (odin3_wire_live(mod, (odin3_wire_id){wid})) {
            (void)snprintf(parts[n_parts++], NAME_BUF, "w:%s",
                           odin3_strtab_get(tab, odin3_wire_name(mod, (odin3_wire_id){wid})));
        }
    }
    for (uint32_t i = 0; i < n_parts; i++) { /* insertion sort: tiny */
        for (uint32_t j = i; j > 0 && strcmp(parts[j - 1], parts[j]) > 0; j--) {
            char swap[NAME_BUF];
            memcpy(swap, parts[j], NAME_BUF);
            memcpy(parts[j], parts[j - 1], NAME_BUF);
            memcpy(parts[j - 1], swap, NAME_BUF);
        }
    }
    out[0] = '\0';
    for (uint32_t i = 0; i < n_parts; i++) {
        size_t used = strlen(out);
        (void)snprintf(out + used, size - used, "%s,", parts[i]);
    }
    char list[NAME_BUF * 2];
    aliases_of(mod, ff_wnet(&fix, (odin3_module *)mod, "x", 0), list, sizeof list);
    size_t used = strlen(out);
    (void)snprintf(out + used, size - used, "x=%s;", list);
}

/* Modules created in the reverse order give the same names, cell types and aliases. */
static void test_flat_determinism_reversed(void) {
    char forward[LOG_TEXT / 2];
    char reversed[LOG_TEXT / 2];
    odin3_module *sub = make_sub("sub", (odin3_prov_id){0});
    odin3_module *top = make_top();
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    name_set(top, forward, sizeof forward);
    ff_end(&fix);
    ff_begin(&fix);
    top = make_top();
    sub = make_sub("sub", (odin3_prov_id){0});
    (void)inst_xz(top, sub, "u1");
    flatten_ok(top);
    name_set(top, reversed, sizeof reversed);
    TEST_ASSERT_EQUAL_STRING(forward, reversed);
    TEST_ASSERT_NOT_NULL(strstr(forward, "w:u1.t,"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flat_simple);
    RUN_TEST(test_flat_alias_order);
    RUN_TEST(test_flat_nested_names);
    RUN_TEST(test_flat_two_instances);
    RUN_TEST(test_flat_unconnected);
    RUN_TEST(test_flat_constants);
    RUN_TEST(test_flat_multidriver_error);
    RUN_TEST(test_flat_const_output_error);
    RUN_TEST(test_flat_multidriver_ids_differ);
    RUN_TEST(test_flat_const_output_ids_differ);
    RUN_TEST(test_flat_feedthrough);
    RUN_TEST(test_flat_keep_hierarchy_inst);
    RUN_TEST(test_flat_keep_hierarchy_module);
    RUN_TEST(test_flat_attrs);
    RUN_TEST(test_flat_names_unique);
    RUN_TEST(test_flat_unnamed_instance);
    RUN_TEST(test_flat_prov);
    RUN_TEST(test_flat_prov_distinct_instances);
    RUN_TEST(test_flat_prov_nested);
    RUN_TEST(test_flat_determinism);
    RUN_TEST(test_flat_oom);
    RUN_TEST(test_flat_bus);
    RUN_TEST(test_flat_mem_memid);
    RUN_TEST(test_flat_memid_shared_by_cells);
    RUN_TEST(test_flat_params_copied);
    RUN_TEST(test_flat_net_names);
    RUN_TEST(test_flat_generate_names);
    RUN_TEST(test_flat_hand_body);
    RUN_TEST(test_flat_hand_ports);
    RUN_TEST(test_flat_recursion);
    RUN_TEST(test_flat_caps);
    RUN_TEST(test_flat_keep_hierarchy_value_kinds);
    RUN_TEST(test_flat_determinism_reversed);
    RUN_TEST(test_estimate_not_below_actual);
    return UNITY_END();
}
