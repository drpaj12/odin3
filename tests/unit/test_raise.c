/*
 * test_raise.c — the raise pass (Phase 7 spec 3.4, plan Task 8): a group proven equivalent by
 * cone_check is replaced by one cell of the proven type, its provenance is a DERIVED record over
 * the union of the removed nodes' sources, and a group without a proof, with a stale proof (a node
 * added, a binding edited) or proven against another cell is refused with the design unchanged.
 * The proofs use the linked ABC when the build has it. Designs are small BLIF texts in a mkdtemp
 * directory under the build tree.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ODIN3_HAVE_REAL_ABC
#include "odin3_abc.h"
#endif

#ifndef ODIN3_RS_ROOT
#error "ODIN3_RS_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, LOG_MAX = 16384, PAIR_BYTES = 16, MAX_LEAVES = 256 };

/* 4-bit ripple adder: a half adder at bit 0, full adders above; the last carry is unused. */
#define RIPPLE                                                                                     \
    ".model m\n.inputs a0 a1 a2 a3 b0 b1 b2 b3\n.outputs s0 s1 s2 s3\n"                            \
    ".names a0 b0 s0\n10 1\n01 1\n.names a0 b0 c1\n11 1\n"                                         \
    ".names a1 b1 c1 s1\n001 1\n010 1\n100 1\n111 1\n.names a1 b1 c1 c2\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a2 b2 c2 s2\n001 1\n010 1\n100 1\n111 1\n.names a2 b2 c2 c3\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a3 b3 c3 s3\n001 1\n010 1\n100 1\n111 1\n.end\n"

#define ADD4 "-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=4"

static char dir[DIR_BUF];
static odin3_design *design;
static char log_text[LOG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level <= ODIN3_LOG_INFO) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/cc.XXXXXX", ODIN3_RS_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    log_text[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    DIR *handle = opendir(dir);
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL;
         entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            (void)snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
            (void)unlink(path);
        }
    }
    if (handle != NULL) {
        (void)closedir(handle);
    }
    (void)rmdir(dir);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static void read_blif(const char *text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.blif", dir);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_blif", odin3_bytes_cstr(path)), log_text);
}

static odin3_module *top(void) {
    return odin3_module_get(design, (odin3_module_id){1});
}

static uint32_t intern(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &id));
    return id;
}

static void set_str(odin3_objref obj, const char *key, const char *val) {
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(val), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), obj, intern(key), &value));
}

static const char *text_attr(const char *key) {
    odin3_module_id id = odin3_module_id_of(top());
    const odin3_value *val =
        odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_MODULE, id.v}, intern(key));
    return val != NULL && val->kind == ODIN3_VAL_STRING
               ? odin3_strtab_get(odin3_design_strtab(design), val->str)
               : NULL;
}

/* Binds the net named in pair "net=PORT[BIT]". */
static void bind(const char *pair) {
    char net[PATH_BUF];
    const char *eq = strchr(pair, '=');
    TEST_ASSERT_NOT_NULL(eq);
    (void)snprintf(net, sizeof net, "%.*s", (int)(eq - pair), pair);
    odin3_net_id id = odin3_module_find_net(top(), intern(net));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(id), net);
    set_str((odin3_objref){ODIN3_OBJ_NET, id.v}, "odin3:bind", eq + 1);
}

/* Every live node that is not a port node joins group grp=add4; ports are never in a group. */
static void mark_all(void) {
    odin3_module *mod = top();
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT) {
            set_str((odin3_objref){ODIN3_OBJ_NODE, nid}, "grp", "add4");
        }
    }
}

/* a_i to A[i], b_i to B[i], s_i to Y[i]; swap_a01 exchanges the bindings of a0 and a1. */
static void bind_adder(bool swap_a01) {
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, swap_a01 && i < 2 ? 1 - i : i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "b%d=B[%d]", i, i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "s%d=Y[%d]", i, i);
        bind(pair);
    }
}

static void load_adder(bool swap_a01) {
    read_blif(RIPPLE);
    mark_all();
    bind_adder(swap_a01);
}

static odin3_status run(const char *args) {
    return odin3_pass_run(design, "cone_check", odin3_bytes_cstr(args));
}

static void check_clean(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(design, opts), log_text);
}

static uint32_t live_count(void) {
    odin3_module *mod = top();
    uint32_t live = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        live +=
            odin3_node_live(mod, node) &&
                    odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT
                ? 1U
                : 0U;
    }
    return live;
}

static odin3_node_id only_logic_node(void) {
    odin3_module *mod = top();
    odin3_node_id found = {0};
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT) {
            TEST_ASSERT_EQUAL_UINT32(0, found.v);
            found = node;
        }
    }
    TEST_ASSERT_TRUE(found.v != 0);
    return found;
}

/* A fresh empty design (the test builds several adders). */
static void design_reset(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    log_text[0] = '\0';
}

static odin3_status run_raise(const char *args) {
    return odin3_pass_run(design, "raise", odin3_bytes_cstr(args));
}

static void use_real_abc(void) {
#ifdef ODIN3_HAVE_REAL_ABC
    static bool registered;
    if (!registered) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_adapter_init());
        registered = true;
    }
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

/* Loads the adder, proves it against $add and checks the verdict. */
static void prove_adder(void) {
    use_real_abc();
    load_adder(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
}

/* The text a refusal must log. */
typedef struct needle {
    const char *text;
} needle;

#define REFUSED(args, text) expect_refused(args, (needle){text})

static void expect_refused(const char *args, needle must_log) {
    uint32_t live = live_count();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(args), log_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, must_log.text), log_text);
    TEST_ASSERT_EQUAL_UINT32(live, live_count());
    check_clean();
}

/* --- leaves of provenance ----------------------------------------------------------------------
 */

typedef struct leaves {
    uint32_t ids[MAX_LEAVES];
    size_t count;
} leaves;

/* Adds leaf to the set, which stays sorted; a leaf already in it is not added again. */
static void add_leaf(void *user, odin3_prov_id leaf) {
    leaves *set = user;
    size_t at = 0;
    while (at < set->count && set->ids[at] < leaf.v) {
        at++;
    }
    if (at < set->count && set->ids[at] == leaf.v) {
        return;
    }
    TEST_ASSERT_TRUE(set->count < MAX_LEAVES);
    memmove(&set->ids[at + 1], &set->ids[at], (set->count - at) * sizeof set->ids[0]);
    set->ids[at] = leaf.v;
    set->count++;
}

/* The sources of every live non-port node, as a sorted set. */
static leaves all_sources(void) {
    leaves set = {{0}, 0};
    odin3_module *mod = top();
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT &&
            odin3_node_prov(mod, node).v != 0) {
            TEST_ASSERT_EQUAL_INT(
                ODIN3_OK, odin3_prov_sources(design, odin3_node_prov(mod, node), add_leaf, &set));
        }
    }
    return set;
}

/* --- the cases of plan Task 8 step 1 -----------------------------------------------------------
 */

static void test_raise_replaces_proven_group(void) {
    prove_adder();
    TEST_ASSERT_TRUE(live_count() > 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(ADD4), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, live_count());
    odin3_module *mod = top();
    odin3_node_id node = only_logic_node();
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
    TEST_ASSERT_EQUAL_STRING("$add", def->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, odin3_node_param(mod, node, 0)->kind);
    /* every pin of the new cell sits on a bound net: a_i on A[i], b_i on B[i], s_i on Y[i]. */
    odin3_pinslice pins = odin3_node_pins(mod, node);
    TEST_ASSERT_EQUAL_UINT32(12, pins.count);
    for (uint32_t k = 0; k < pins.count; k++) {
        TEST_ASSERT_TRUE(odin3_net_valid(odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k})));
    }
    check_clean();
    /* a second raise finds no group: the first one consumed it. */
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, run_raise(ADD4));
}

/* A one-bit wire named wname on the net named nname. */
static void add_wire(const char *wname, const char *nname) {
    odin3_net_id net = odin3_module_find_net(top(), intern(nname));
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    odin3_wire_spec spec = {intern(wname), 0, 0, false, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(top(), &spec, &net, &wire));
}

/* The wire of an internal net goes with the net: nothing dangles, and the boundary wires stay. */
static void test_raise_drops_internal_wires(void) {
    prove_adder();
    add_wire("w_c1", "c1");
    add_wire("w_c3", "c3");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(ADD4), log_text);
    TEST_ASSERT_FALSE(odin3_wire_live(top(), odin3_module_find_wire(top(), intern("w_c1"))));
    TEST_ASSERT_FALSE(odin3_wire_live(top(), odin3_module_find_wire(top(), intern("w_c3"))));
    check_clean();
}

/* Puts one logic node of module 2 in group grp=add4, which has no proof there. */
static void mark_second_module(void) {
    odin3_module *second = odin3_module_get(design, (odin3_module_id){2});
    TEST_ASSERT_NOT_NULL(second);
    odin3_node_id found = {0};
    for (uint32_t nid = 1; nid < odin3_module_node_end(second); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(second, node) &&
            odin3_celltype_get(design, odin3_node_type(second, node))->gran != ODIN3_GRAN_PORT) {
            found = node;
        }
    }
    TEST_ASSERT_TRUE(found.v != 0);
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern("add4"), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(second, (odin3_objref){ODIN3_OBJ_NODE, found.v},
                                                   intern("grp"), &value));
}

/* A later module refused: an earlier module that holds a valid proof is left unchanged too. */
static void test_raise_all_or_nothing_across_modules(void) {
    use_real_abc();
    read_blif(RIPPLE ".model m2\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    mark_all();
    bind_adder(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    mark_second_module();
    uint32_t live = live_count();
    TEST_ASSERT_TRUE(live > 1);
    REFUSED(ADD4, "no cone_check result");
    TEST_ASSERT_EQUAL_UINT32(live, live_count());
}

static void test_raise_provenance_union(void) {
    prove_adder();
    leaves before = all_sources();
    TEST_ASSERT_TRUE(before.count > 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(ADD4), log_text);
    odin3_node_id node = only_logic_node();
    odin3_prov_id prov = odin3_node_prov(top(), node);
    TEST_ASSERT_TRUE(prov.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, odin3_prov_get(design, prov)->kind);
    leaves after = all_sources();
    TEST_ASSERT_EQUAL_size_t(before.count, after.count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(before.ids, after.ids, before.count);
}

static void test_raise_refuses_unproven(void) {
    use_real_abc();
    load_adder(false);
    REFUSED(ADD4, "cone_check");
    /* a proof that differs does not count either */
    design_reset();
    load_adder(true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    TEST_ASSERT_EQUAL_STRING("differs", text_attr("odin3:cone_check"));
    REFUSED(ADD4, "not equivalent");
    /* nor a proof under a 0 s limit */
    design_reset();
    load_adder(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4 " -timeout 0"), log_text);
    TEST_ASSERT_EQUAL_STRING("unproven", text_attr("odin3:cone_check"));
    REFUSED(ADD4, "not equivalent");
}

static void test_raise_refuses_stale_proof(void) {
    prove_adder();
    /* a node joins the group after the proof */
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_CONST1_"), &type));
    odin3_node_id node = {0};
    odin3_node_spec spec = {type, 0, {0}, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(top(), &spec, &node));
    set_str((odin3_objref){ODIN3_OBJ_NODE, node.v}, "grp", "add4");
    REFUSED(ADD4, "changed since");
}

static void test_raise_refuses_edited_binding(void) {
    prove_adder();
    bind("a0=A[1]");
    bind("a1=A[0]");
    REFUSED(ADD4, "changed since");
}

/* A node outside the group reads an internal net after the proof: raise refuses, unchanged. */
static void test_raise_refuses_new_outside_reader(void) {
    prove_adder();
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_NOT_"), &type));
    odin3_net_id c1 = odin3_module_find_net(top(), intern("c1"));
    TEST_ASSERT_TRUE(odin3_net_valid(c1));
    odin3_net_id none = {0};
    odin3_netvec ports[2] = {{&c1, 1}, {&none, 1}};
    odin3_node_spec spec = {type, 0, {0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, ports, &node));
    REFUSED(ADD4, "raise:");
}

static void test_raise_refuses_other_cell(void) {
    prove_adder();
    REFUSED("-attr grp=add4 -cell $sub -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=4",
            "proved");
    REFUSED("-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=5",
            "proved");
}

static void test_raise_cell_text_ignores_param_order(void) {
    prove_adder();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        run_raise("-param Y_WIDTH=4 -cell $add -param B_WIDTH=4 -attr grp=add4 -param A_WIDTH=4"),
        log_text);
    TEST_ASSERT_EQUAL_UINT32(1, live_count());
}

static void test_raise_usage_errors(void) {
    prove_adder();
    REFUSED("-cell $add", "-attr");
    REFUSED("-attr grp=add4", "-cell");
    REFUSED("-attr grp=nothing -cell $add", "empty");
    REFUSED(ADD4 " -bogus", "-bogus");
}

/* --- the cases of v1.0.5 plan Task 3: -const ------------------------------------------------- */

/* s = a + 1 mod 16 as half adders: s0 = !a0, s1 = a1 ^ a0, c2 = a1 & a0, ... */
#define INC4_BLIF                                                                                  \
    ".model m\n.inputs a0 a1 a2 a3\n.outputs s0 s1 s2 s3\n.names a0 s0\n0 1\n"                     \
    ".names a1 a0 s1\n01 1\n10 1\n.names a1 a0 c2\n11 1\n"                                         \
    ".names a2 c2 s2\n01 1\n10 1\n.names a2 c2 c3\n11 1\n"                                         \
    ".names a3 c3 s3\n01 1\n10 1\n.end\n"

#define INC_ARGS                                                                                   \
    "-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=1 -param Y_WIDTH=4 -const B=1"

#define EQ_ARGS "-attr grp=add4 -cell $eq -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=1"

static void bind_inc4(void) {
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "s%d=Y[%d]", i, i);
        bind(pair);
    }
}

/* The live nodes of the cell type `name` in module id (1 is the top). */
static uint32_t count_type_in(uint32_t id, const char *name) {
    odin3_module *mod = odin3_module_get(design, (odin3_module_id){id});
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        count +=
            odin3_node_live(mod, node) &&
                    strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, name) == 0
                ? 1U
                : 0U;
    }
    return count;
}

static uint32_t count_type(const char *name) {
    return count_type_in(1, name);
}

/* The first live node of the cell type `name` in the top module. */
static odin3_node_id find_type(const char *name) {
    odin3_module *mod = top();
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, name) == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return (odin3_node_id){0};
}

static void prove_inc4(void) {
    use_real_abc();
    read_blif(INC4_BLIF);
    mark_all();
    bind_inc4();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(INC_ARGS), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
}

/* The constant value of the net on bit `bit` of port B (port 1) of node. */
static odin3_const b_bit_const(odin3_node_id node, uint32_t bit) {
    odin3_module *mod = top();
    odin3_pinslice slice = odin3_node_port(mod, node, 1);
    TEST_ASSERT_TRUE(bit < slice.count);
    return odin3_net_const_value(mod, odin3_pin_net(mod, (odin3_pin_id){slice.first.v + bit}));
}

static void test_raise_const_increment(void) {
    prove_inc4();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(INC_ARGS), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST1_"));
    TEST_ASSERT_EQUAL_UINT32(2, live_count());
    odin3_node_id add = find_type("$add");
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, b_bit_const(add, 0));
    check_clean();
}

static void test_raise_const_reuses_shared_constant(void) {
    use_real_abc();
    read_blif(INC4_BLIF);
    mark_all();
    bind_inc4();
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_CONST1_"), &type));
    odin3_net_id net = {0};
    odin3_node_id made = {0};
    odin3_node_spec spec = {type, 0, {0}, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top(), 0, (odin3_prov_id){0}, &net));
    odin3_netvec ports = {&net, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, &ports, &made));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(INC_ARGS), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(INC_ARGS), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_CONST1_"));
    odin3_node_id add = find_type("$add");
    odin3_pinslice slice = odin3_node_port(top(), add, 1);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_pin_net(top(), slice.first).v);
    check_clean();
}

static void test_raise_const_refuses_other_constant(void) {
    prove_inc4();
    REFUSED("-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=1 -param Y_WIDTH=4 "
            "-const B=0b1",
            "cone_check proved group");
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_CONST1_"));
}

static void test_raise_const_all_or_nothing(void) {
    use_real_abc();
    read_blif(INC4_BLIF ".model m2\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    mark_all();
    bind_inc4();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(INC_ARGS), log_text);
    mark_second_module();
    REFUSED(INC_ARGS, "no cone_check result");
    TEST_ASSERT_EQUAL_UINT32(0, count_type_in(1, "$_CONST1_"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type_in(1, "$_CONST0_"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type_in(2, "$_CONST1_"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type_in(2, "$_CONST0_"));
}

/* y = a == 13, read through a group-internal constant: .names one / 1 is a $sop in the group. */
static void test_raise_const_ignores_group_constant(void) {
    use_real_abc();
    read_blif(".model m\n.inputs a0 a1 a2 a3\n.outputs y\n.names one\n1\n"
              ".names a0 a1 a2 a3 one y\n10111 1\n.end\n");
    mark_all();
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, i);
        bind(pair);
    }
    bind("y=Y[0]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(EQ_ARGS " -const B=13"), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_raise(EQ_ARGS " -const B=13"), log_text);
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$sop"));
    odin3_node_id eq = find_type("$eq");
    const odin3_const want[4] = {ODIN3_CONST_1, ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_1};
    for (uint32_t bit = 0; bit < 4; bit++) {
        TEST_ASSERT_EQUAL_INT(want[bit], b_bit_const(eq, bit));
    }
    check_clean();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_raise_replaces_proven_group);
    RUN_TEST(test_raise_drops_internal_wires);
    RUN_TEST(test_raise_all_or_nothing_across_modules);
    RUN_TEST(test_raise_provenance_union);
    RUN_TEST(test_raise_refuses_unproven);
    RUN_TEST(test_raise_refuses_stale_proof);
    RUN_TEST(test_raise_refuses_edited_binding);
    RUN_TEST(test_raise_refuses_new_outside_reader);
    RUN_TEST(test_raise_refuses_other_cell);
    RUN_TEST(test_raise_cell_text_ignores_param_order);
    RUN_TEST(test_raise_usage_errors);
    RUN_TEST(test_raise_const_increment);
    RUN_TEST(test_raise_const_reuses_shared_constant);
    RUN_TEST(test_raise_const_refuses_other_constant);
    RUN_TEST(test_raise_const_all_or_nothing);
    RUN_TEST(test_raise_const_ignores_group_constant);
    return UNITY_END();
}
