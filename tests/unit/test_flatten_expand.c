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
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, OOM_LIMIT = 6000, NAME_BUF = 32 };

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
    return UNITY_END();
}
