/*
 * test_unmap.c — the unmap pass (v1.0.4, spec docs/specs/2026-10-21-unmap-design.md, plan Tasks 1
 * to 16): hard cells back to word-level cells. Designs are small BLIF texts or Verilog files in a
 * mkdtemp directory under the build tree; the VTR library is lib/vtr.o3lib.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "passes/unmap/chain.h"
#include "passes/unmap/kit.h"
#include "passes/unmap/operand.h"
#include "passes/unmap/ram_depth.h"
#include "passes/unmap/unmap.h"
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

#ifndef ODIN3_UM_ROOT
#error "ODIN3_UM_ROOT must name the build tree"
#endif
#ifndef ODIN3_UM_SRC
#error "ODIN3_UM_SRC must name the source tree"
#endif
#ifndef ODIN3_UM_LIB
#error "ODIN3_UM_LIB must name lib/vtr.o3lib"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, LOG_MAX = 32768 };

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
    (void)snprintf(dir, sizeof dir, "%s/um.XXXXXX", ODIN3_UM_ROOT);
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

static void write_text(const char *name, odin3_bytes text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(text.len, fwrite(text.ptr, 1, text.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The path of a file of the source tree (the tests run from the build directory). */
static const char *src_path(const char *relative) {
    static char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_UM_SRC, relative);
    return path;
}

static void load_lib(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(ODIN3_UM_LIB)), log_text);
}

static void read_blif(const char *text) {
    write_text("t.blif", odin3_bytes_cstr(text));
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.blif", dir);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_blif", odin3_bytes_cstr(path)), log_text);
}

static odin3_status unmap(const char *args) {
    return odin3_pass_run(design, "unmap", odin3_bytes_cstr(args));
}

static void check_clean(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(design, opts), log_text);
}

static odin3_module *top(void) {
    return odin3_module_get(design, (odin3_module_id){1});
}

static uint32_t intern(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &id));
    return id;
}

static odin3_net_id net_named(const char *name) {
    odin3_net_id net = odin3_module_find_net(top(), intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), name);
    return net;
}

static odin3_celltype_id type_named(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

static void ok(odin3_status status) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, status);
}

/* The live nodes of the top module with type `name`, in node-ID order; returns the count. */
static uint32_t nodes_of(const char *name, odin3_node_id *out, uint32_t cap) {
    odin3_celltype_id type = type_named(name);
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(top()); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(top(), node) && odin3_node_type(top(), node).v == type.v) {
            if (out != NULL && count < cap) {
                out[count] = node;
            }
            count++;
        }
    }
    return count;
}

static um_ctx make_ctx(const um_opts *opts) {
    um_ctx uc;
    memset(&uc, 0, sizeof uc);
    uc.design = design;
    uc.module = top();
    uc.opts = opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_types_find(design, &uc.types));
    return uc;
}

static void test_unmap_options(void) {
    TEST_ASSERT_NOT_NULL(odin3_pass_find(odin3_bytes_cstr("unmap")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, unmap(""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, unmap("--no-regroup"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, unmap("--bogus"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "unmap: unknown argument '--bogus' (known: --no-regroup)"));
}

/* --- Task 2: net classes, types, constants ------------------------------------------------- */

#define CLASS_BLIF                                                                                 \
    ".model m\n.inputs x\n.outputs co dang\n.names gnd\n.names vcc\n1\n.names unconn\n"            \
    ".names x dang\n1 1\n"                                                                         \
    ".subckt adder a=x b=gnd cin=unconn cout=co\n.end\n"

static void test_net_class(void) {
    load_lib();
    read_blif(CLASS_BLIF);
    odin3_module *mod = top();
    TEST_ASSERT_EQUAL_INT(UM_GND, um_net_class(mod, net_named("gnd")));
    TEST_ASSERT_EQUAL_INT(UM_VCC, um_net_class(mod, net_named("vcc")));
    TEST_ASSERT_EQUAL_INT(UM_PAD, um_net_class(mod, net_named("unconn")));
    TEST_ASSERT_EQUAL_INT(UM_SIG, um_net_class(mod, net_named("x")));
    TEST_ASSERT_EQUAL_INT(UM_NONE, um_net_class(mod, (odin3_net_id){0}));
    odin3_node_id adder[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("adder", adder, 1));
    TEST_ASSERT_FALSE(odin3_net_valid(um_port_net(mod, adder[0], (um_pos){4, 0})));
    TEST_ASSERT_EQUAL_UINT32(net_named("x").v, um_port_net(mod, adder[0], (um_pos){0, 0}).v);
    TEST_ASSERT_FALSE(odin3_net_valid(um_port_net(mod, adder[0], (um_pos){0, 1})));
}

static void test_net_class_x(void) {
    read_blif(".model m\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top(), 0, odin3_module_prov(top()), &net));
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {type_named("$_CONSTX_"), 0, odin3_module_prov(top()), NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, &port, &node));
    TEST_ASSERT_EQUAL_INT(UM_PAD, um_net_class(top(), net));
}

static void test_observed(void) {
    read_blif(CLASS_BLIF);
    TEST_ASSERT_TRUE(um_observed(top(), net_named("co")));
    TEST_ASSERT_TRUE(um_observed(top(), net_named("dang")));
    TEST_ASSERT_FALSE(um_observed(top(), net_named("gnd")));
}

static void test_types_find(void) {
    um_types types;
    load_lib();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_types_find(design, &types));
    TEST_ASSERT_TRUE(types.adder.v != 0 && types.multiply.v != 0 && types.sp_ram.v != 0 &&
                     types.dp_ram.v != 0);
    TEST_ASSERT_TRUE(types.add.v != 0 && types.mem.v != 0 && types.konst1.v != 0);
}

static void test_types_find_no_lib(void) {
    um_types types;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_types_find(design, &types));
    TEST_ASSERT_EQUAL_UINT32(0, types.adder.v);
    TEST_ASSERT_EQUAL_UINT32(0, types.multiply.v);
    TEST_ASSERT_TRUE(types.mul.v != 0);
}

static void test_types_find_wrong_shape(void) {
    read_blif(".model adder\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    um_types types;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, um_types_find(design, &types));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "unmap: cell type 'adder' does not have the expected ports"));
}

static void test_const_net(void) {
    read_blif(".model m\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n");
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_net_id first = {0};
    odin3_net_id second = {0};
    odin3_net_id one = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_const_net(&uc, false, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_const_net(&uc, false, &second));
    TEST_ASSERT_EQUAL_UINT32(first.v, second.v);
    TEST_ASSERT_EQUAL_INT(UM_GND, um_net_class(top(), first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_const_net(&uc, true, &one));
    TEST_ASSERT_EQUAL_INT(UM_VCC, um_net_class(top(), one));
    TEST_ASSERT_NOT_EQUAL(first.v, one.v);
    /* a constant node that is already there is found, also by a fresh context */
    um_ctx again = make_ctx(&opts);
    odin3_net_id found = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_const_net(&again, true, &found));
    TEST_ASSERT_EQUAL_UINT32(one.v, found.v);
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$_CONST1_", NULL, 0));
    check_clean();
}

/* --- Task 3: the replacement transaction -------------------------------------------------- */

#define TWO_ADDERS                                                                                 \
    ".model m\n.inputs a b c d\n.outputs s1 s2\n"                                                  \
    ".subckt adder a=a b=b cin=c cout=k1 sumout=s1\n"                                              \
    ".subckt adder a=c b=d cin=a cout=k2 sumout=s2\n.end\n"

static const odin3_value k_add1[5] = {{ODIN3_VAL_INT, 0, NULL, 0, 0, 0},
                                      {ODIN3_VAL_INT, 0, NULL, 0, 0, 0},
                                      {ODIN3_VAL_INT, 1, NULL, 0, 0, 0},
                                      {ODIN3_VAL_INT, 1, NULL, 0, 0, 0},
                                      {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}};

static void begin_run(um_ctx *uc, odin3_pass_ctx *pctx) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("unmap"), pctx));
    uc->pass = pctx;
}

static void set_str_attr(odin3_node_id node, const char *key, const char *val) {
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(val), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   intern(key), &value));
}

static const odin3_value *attr_of(odin3_node_id node, const char *key) {
    return odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern(key));
}

/* A $add on the nets a, b (inputs) and s1 (output); the group is the two adders. */
static odin3_node_id replace_two(odin3_node_id adders[2]) {
    odin3_net_id nets[3] = {net_named("a"), net_named("b"), net_named("s1")};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_pass_ctx pctx;
    begin_run(&uc, &pctx);
    um_group group;
    um_group_init(&group);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_group_add(&group, adders[1]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_group_add(&group, adders[0]));
    um_new req = {uc.types.add, k_add1, 5, ports, adders[0], "chain", 0, 0};
    odin3_node_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_replace(&uc, &group, &req, &out));
    um_group_free(&group);
    return out;
}

static void test_replace_provenance_and_attrs(void) {
    load_lib();
    read_blif(TWO_ADDERS);
    odin3_node_id adders[2];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", adders, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(top(), adders[0], intern("addr0")));
    set_str_attr(adders[0], "foo", "bar");
    set_str_attr(adders[0], "hardmap", "adder:head");
    odin3_prov_id p0 = odin3_node_prov(top(), adders[0]);
    odin3_prov_id p1 = odin3_node_prov(top(), adders[1]);
    odin3_node_id add = replace_two(adders);
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("adder", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(add.v, odin3_module_find_node(top(), intern("addr0")).v);
    TEST_ASSERT_NOT_NULL(attr_of(add, "foo"));
    TEST_ASSERT_NULL(attr_of(add, "hardmap"));
    TEST_ASSERT_EQUAL_UINT32(intern("chain"), attr_of(add, "unmap")->str);
    TEST_ASSERT_EQUAL_INT64(2, attr_of(add, "unmap_pieces")->i);
    odin3_prov_id prov = odin3_node_prov(top(), add);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, odin3_prov_get(design, prov)->kind);
    odin3_prov_list parents = odin3_prov_parents(design, prov);
    TEST_ASSERT_EQUAL_UINT32(2, parents.count);
    TEST_ASSERT_TRUE(p0.v != p1.v && parents.ids[0].v == p0.v && parents.ids[1].v == p1.v);
    check_clean();
}

static void test_replace_no_prov(void) {
    load_lib();
    read_blif(".model m\n.inputs a b\n.outputs s1\n.names a b s1\n11 1\n.end\n");
    odin3_node_id adders[2];
    odin3_net_id open_pin = {0};
    odin3_netvec ports[5] = {
        {&open_pin, 1}, {&open_pin, 1}, {&open_pin, 1}, {&open_pin, 1}, {&open_pin, 1}};
    odin3_node_spec spec = {type_named("adder"), 0, {0}, NULL, 0};
    for (size_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_node_create_connected(top(), &spec, ports, &adders[i]));
    }
    odin3_node_id add = replace_two(adders);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_prov(top(), add).v);
}

static void test_replace_failure_keeps_group(void) {
    load_lib();
    read_blif(TWO_ADDERS);
    odin3_node_id adders[2];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", adders, 2));
    odin3_net_id nets[2] = {net_named("a"), net_named("b")};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {nets, 2}}; /* Y has 1 bit */
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_pass_ctx pctx;
    begin_run(&uc, &pctx);
    um_group group;
    um_group_init(&group);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_group_add(&group, adders[0]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_group_add(&group, adders[1]));
    um_new req = {uc.types.add, k_add1, 5, ports, adders[0], "chain", 0, 0};
    odin3_node_id out = {0};
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, um_replace(&uc, &group, &req, &out));
    um_group_free(&group);
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", NULL, 0));
    check_clean();
}

/* --- Task 4: carry chain discovery -------------------------------------------------------- */

#define CHAIN_HEAD                                                                                 \
    ".model m\n.inputs x0 x1 x2 y0 y1 y2 z\n.outputs s0 s1 s2 t\n.names gnd\n.names vcc\n1\n"
#define HEAD_CELL ".subckt adder a=vcc b=gnd cin=gnd cout=c0\n"
#define STAGE0 ".subckt adder a=x0 b=y0 cin=c0 cout=c1 sumout=s0\n"
#define STAGE1 ".subckt adder a=x1 b=y1 cin=c1 cout=c2 sumout=s1\n"
#define STAGE2 ".subckt adder a=x2 b=y2 cin=c2 cout=c3 sumout=s2\n"

static const um_walk *walk_at(const odin3_vec *walks, size_t index) {
    TEST_ASSERT_TRUE(index < walks->len);
    return (const um_walk *)walks->data + index;
}

static void expect_cells(const um_walk *walk, const odin3_node_id *want, size_t count) {
    TEST_ASSERT_EQUAL_size_t(count, walk->cells.len);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i].v, ((const odin3_node_id *)walk->cells.data)[i].v);
    }
}

static void test_walks_simple(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0 STAGE1 STAGE2 ".end\n");
    odin3_node_id cells[4];
    TEST_ASSERT_EQUAL_UINT32(4, nodes_of("adder", cells, 4));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(1, walks.len);
    expect_cells(walk_at(&walks, 0), cells, 4);
    TEST_ASSERT_NULL(walk_at(&walks, 0)->decline);
    um_walks_free(&walks);
}

static void test_walks_order_independent(void) {
    load_lib();
    read_blif(CHAIN_HEAD STAGE2 STAGE1 STAGE0 HEAD_CELL ".end\n");
    odin3_node_id cells[4]; /* file order: s2 s1 s0 head */
    TEST_ASSERT_EQUAL_UINT32(4, nodes_of("adder", cells, 4));
    odin3_node_id want[4] = {cells[3], cells[2], cells[1], cells[0]};
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(1, walks.len);
    expect_cells(walk_at(&walks, 0), want, 4);
    um_walks_free(&walks);
}

static void test_walks_branch(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0
              ".subckt adder a=x1 b=y1 cin=c1 cout=c2 sumout=s1\n"
              ".subckt adder a=x2 b=y2 cin=c1 cout=c3 sumout=s2\n.end\n");
    odin3_node_id cells[4];
    TEST_ASSERT_EQUAL_UINT32(4, nodes_of("adder", cells, 4));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(3, walks.len);
    expect_cells(walk_at(&walks, 0), cells, 2); /* head, stage 0 (the branching cell) */
    expect_cells(walk_at(&walks, 1), cells + 2, 1);
    expect_cells(walk_at(&walks, 2), cells + 3, 1);
    um_walks_free(&walks);
}

static void test_walks_tap(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0 STAGE1 ".names c1 t\n1 1\n.end\n");
    odin3_node_id cells[3];
    TEST_ASSERT_EQUAL_UINT32(3, nodes_of("adder", cells, 3));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(2, walks.len);
    expect_cells(walk_at(&walks, 0), cells, 2);
    expect_cells(walk_at(&walks, 1), cells + 2, 1);
    um_walks_free(&walks);
}

static void test_walks_cycle(void) {
    load_lib();
    read_blif(CHAIN_HEAD ".subckt adder a=x0 b=y0 cin=c3 cout=c1 sumout=s0\n"
                         ".subckt adder a=x1 b=y1 cin=c1 cout=c2 sumout=s1\n"
                         ".subckt adder a=x2 b=y2 cin=c2 cout=c3 sumout=s2\n.end\n");
    odin3_node_id cells[3];
    TEST_ASSERT_EQUAL_UINT32(3, nodes_of("adder", cells, 3));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(1, walks.len);
    expect_cells(walk_at(&walks, 0), cells, 3);
    TEST_ASSERT_EQUAL_STRING("cycle", walk_at(&walks, 0)->decline);
    um_walks_free(&walks);
}

static void test_walks_two_chains(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0
              ".subckt adder a=x1 b=y1 cin=gnd cout=d1 sumout=s1\n"
              ".subckt adder a=x2 b=y2 cin=d1 cout=d2 sumout=s2\n.end\n");
    odin3_node_id cells[4];
    TEST_ASSERT_EQUAL_UINT32(4, nodes_of("adder", cells, 4));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    odin3_vec walks;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_chain_walks(&uc, &walks));
    TEST_ASSERT_EQUAL_size_t(2, walks.len);
    expect_cells(walk_at(&walks, 0), cells, 2);
    expect_cells(walk_at(&walks, 1), cells + 2, 2);
    um_walks_free(&walks);
}

/* --- Task 5: chains to $add and $sub ------------------------------------------------------ */

static void run_script(const char *text) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        odin3_pass_run_script(design, odin3_bytes_cstr(text),
                              (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND}),
        log_text);
}

/* Reads Verilog `src` with the VTR library and runs proc, opt and hardmap. */
static void flow_file(const char *path) {
    char text[PATH_BUF * 2];
    (void)snprintf(text, sizeof text,
                   "read_techlib %s\nread_verilog --vtr-compat %s\nproc\nopt\nhardmap\n",
                   ODIN3_UM_LIB, path);
    run_script(text);
}

static void flow(const char *src) {
    char path[PATH_BUF];
    write_text("t.v", odin3_bytes_cstr(src));
    (void)snprintf(path, sizeof path, "%s/t.v", dir);
    flow_file(path);
}

static void expect_params(odin3_node_id node, const int64_t want[5]) {
    int64_t got[5];
    for (uint32_t i = 0; i < 5; i++) {
        got[i] = odin3_node_param(top(), node, i)->i;
    }
    TEST_ASSERT_EQUAL_INT64_ARRAY(want, got, 5);
}

static int64_t attr_int(odin3_node_id node, const char *key) {
    const odin3_value *value = attr_of(node, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(value, key);
    return value->i;
}

#define MOD_ADD                                                                                    \
    "module t(input [7:0] a, input [5:0] b, output [8:0] y); assign y = a + b; endmodule\n"

static void test_unmap_add_roundtrip(void) {
    flow(MOD_ADD);
    run_script("unmap\nopt_clean");
    odin3_node_id add[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 2));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("adder", NULL, 0));
    expect_params(add[0], (const int64_t[5]){0, 0, 8, 6, 9});
    TEST_ASSERT_NULL(attr_of(add[0], "hardmap"));
    TEST_ASSERT_EQUAL_UINT32(intern("chain"), attr_of(add[0], "unmap")->str);
    TEST_ASSERT_EQUAL_INT64(10, attr_int(add[0], "unmap_pieces"));
    check_clean();
}

static void test_unmap_sub_roundtrip(void) {
    flow("module t(input [7:0] a, input [5:0] b, output [8:0] y); assign y = a - b; endmodule\n");
    run_script("unmap\nopt_clean");
    odin3_node_id sub[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$sub", sub, 2));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$not", NULL, 0));
    expect_params(sub[0], (const int64_t[5]){0, 0, 8, 6, 9});
    check_clean();
}

static void test_unmap_add_signed(void) {
    flow("module t(input signed [7:0] a, input signed [3:0] b, output signed [8:0] y);\n"
         "assign y = a + b; endmodule\n");
    run_script("unmap\nopt_clean");
    odin3_node_id add[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 2));
    expect_params(add[0], (const int64_t[5]){1, 1, 8, 4, 9});
    check_clean();
}

/* Replaces the $sop that drives net `out` by a 1-bit $not of net `in`. */
static void make_not(const char *in, const char *out) {
    odin3_net_id out_net = net_named(out);
    odin3_node_id sop = odin3_pin_node(top(), odin3_net_driver(top(), out_net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(top(), sop));
    odin3_net_id in_net = net_named(in);
    odin3_netvec ports[2] = {{&in_net, 1}, {&out_net, 1}};
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(1)};
    odin3_node_spec spec = {type_named("$not"), 0, odin3_module_prov(top()), params, 3};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, ports, &node));
}

#define SUB_HEAD                                                                                   \
    ".model m\n.inputs x0 x1 y0 y1\n.outputs s0 s1 t\n.names gnd\n.names vcc\n1\n"                 \
    ".names unconn\n.names y0 n0\n0 1\n.names y1 n1\n0 1\n"                                        \
    ".subckt adder a=vcc b=vcc cin=unconn cout=c0\n"                                               \
    ".subckt adder a=x0 b=n0 cin=c0 cout=c1 sumout=s0\n"                                           \
    ".subckt adder a=x1 b=n1 cin=c1 cout=c2 sumout=s1\n"

static void test_unmap_not_shared(void) {
    load_lib();
    read_blif(SUB_HEAD ".names n0 t\n1 1\n.end\n");
    make_not("y0", "n0");
    make_not("y1", "n1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id sub[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$sub", sub, 1));
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$not", NULL, 0)); /* n0 also feeds a LUT: it stays */
    check_clean();
}

static void test_unmap_sub_absorbs_not(void) {
    load_lib();
    read_blif(SUB_HEAD ".end\n");
    make_not("y0", "n0");
    make_not("y1", "n1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$sub", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$not", NULL, 0));
    check_clean();
}

static void test_unmap_cout_tap_end(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0 STAGE1 STAGE2 ".names c3 t\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id add[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 2));
    expect_params(add[0], (const int64_t[5]){0, 0, 3, 3, 4});
    odin3_pinslice ypins = odin3_node_port(top(), add[0], 2);
    TEST_ASSERT_EQUAL_UINT32(4, ypins.count);
    TEST_ASSERT_EQUAL_UINT32(net_named("c3").v,
                             odin3_pin_net(top(), (odin3_pin_id){ypins.first.v + 3}).v);
    check_clean();
}

static void test_unmap_cout_tap_middle(void) {
    load_lib();
    read_blif(CHAIN_HEAD HEAD_CELL STAGE0 STAGE1 STAGE2 ".names c1 t\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id add[2];
    odin3_node_id rest[3];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 2));
    expect_params(add[0], (const int64_t[5]){0, 0, 1, 1, 2});
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", rest, 3));
    TEST_ASSERT_EQUAL_UINT32(intern("adder:cin_carry"), attr_of(rest[0], "unmap_note")->str);
    check_clean();
}

static void test_unmap_parmys_blif(void) {
    load_lib();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        odin3_pass_run(
            design, "read_blif",
            odin3_bytes_cstr(src_path("tests/golden/blif-vtr/adder_hard_block.parmys.vtr.blif"))),
        log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id add[3];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$add", add, 3));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("adder", NULL, 0));
    expect_params(add[0], (const int64_t[5]){0, 0, 3, 1, 3});
    expect_params(add[1], (const int64_t[5]){0, 0, 2, 2, 3});
    check_clean();
}

static void test_unmap_declines(void) {
    load_lib();
    read_blif(CHAIN_HEAD ".names unconn\n.subckt adder a=x0 b=y0 cin=z cout=c1 sumout=s0\n"
                         ".subckt adder a=vcc b=gnd cin=gnd cout=c9\n"
                         ".subckt adder a=x1 b=y1 cin=unconn cout=c2 sumout=s1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id cells[3];
    TEST_ASSERT_EQUAL_UINT32(3, nodes_of("adder", cells, 3));
    TEST_ASSERT_EQUAL_UINT32(intern("adder:cin_signal"), attr_of(cells[0], "unmap_note")->str);
    TEST_ASSERT_EQUAL_UINT32(intern("adder:lone_head"), attr_of(cells[1], "unmap_note")->str);
    TEST_ASSERT_EQUAL_UINT32(intern("adder:cin_x"), attr_of(cells[2], "unmap_note")->str);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "adder at"));
    check_clean();
}

/* S-7: a multiply output on a carry-in is a signal, not a recovered carry: the note is the same on
 * the second run, when the multiply is a `$mul` that carries `unmap`. */
static void test_unmap_cin_from_multiply_twice(void) {
    load_lib();
    read_blif(CHAIN_HEAD ".subckt multiply a[0]=x2 b[0]=y2 out[0]=m0 out[1]=m1\n"
                         ".subckt adder a=x0 b=y0 cin=m0 cout=c1 sumout=s0\n"
                         ".subckt adder a=x1 b=y1 cin=c1 cout=c2 sumout=s1\n"
                         ".names m1 s2\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id cells[2];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", cells, 2));
    TEST_ASSERT_EQUAL_UINT32(intern("adder:cin_signal"), attr_of(cells[0], "unmap_note")->str);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("adder", cells, 2));
    TEST_ASSERT_EQUAL_UINT32(intern("adder:cin_signal"), attr_of(cells[0], "unmap_note")->str);
}

/* --- S-7 and the decline reasons ---------------------------------------------------------- */

/* A text of the design that a second run must not change: every live node's ID, type, notes and
 * the nets of its pins. */
static char *census(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    um_ctx uc = {.design = design, .module = top()};
    for (uint32_t nid = 1; nid < odin3_module_node_end(top()); nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(top(), node)) {
            continue;
        }
        const char *note = um_attr_text(&uc, node, "unmap_note");
        const char *cand = um_attr_text(&uc, node, "unmap_cand");
        ok(odin3_strbuf_appendf(&buf, "%u t%u %s %s:", nid, odin3_node_type(top(), node).v,
                                note != NULL ? note : "-", cand != NULL ? cand : "-"));
        odin3_pinslice pins = odin3_node_pins(top(), node);
        for (uint32_t i = 0; i < pins.count; i++) {
            ok(odin3_strbuf_appendf(&buf, " %u",
                                    odin3_pin_net(top(), (odin3_pin_id){pins.first.v + i}).v));
        }
        ok(odin3_strbuf_appendf(&buf, "\n"));
    }
    char *text = strdup(buf.data);
    odin3_strbuf_free(&buf);
    TEST_ASSERT_NOT_NULL(text);
    return text;
}

/* S-7: a second run of unmap on the result of the first changes nothing and keeps every note. */
static void expect_second_run_same(const char *blif) {
    load_lib();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_blif", odin3_bytes_cstr(src_path(blif))), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    char *first = census();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    char *second = census();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
    check_clean();
}

static void test_unmap_second_run_tap(void) {
    expect_second_run_same("tests/unmap/blif/tap.blif");
}

static void test_unmap_second_run_branch(void) {
    expect_second_run_same("tests/unmap/blif/branch.blif");
}

static void test_unmap_second_run_ram_pair(void) {
    expect_second_run_same("tests/unmap/blif/ram_pair_equal_ctl.blif");
}

/* One adder cell the pass declines: the note on the cell, the same after a second run. */
typedef struct adder_case {
    const char *blif;
    const char *note;
} adder_case;

static void expect_adder_note(adder_case test) {
    load_lib();
    read_blif(test.blif);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id cell[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("adder", cell, 1));
    TEST_ASSERT_EQUAL_UINT32(intern(test.note), attr_of(cell[0], "unmap_note")->str);
    char *first = census();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    char *second = census();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
    check_clean();
}

#define ADDER_MODEL "\n.model adder\n.inputs a b cin\n.outputs sumout cout\n.blackbox\n.end\n"

/* C-4: a subtraction whose last carry-out is read. */
static void test_unmap_decline_cout_sub(void) {
    expect_adder_note((adder_case){CHAIN_HEAD ".subckt adder a=x0 b=y0 cin=vcc cout=c1 sumout=s0\n"
                                              ".names c1 t\n1 1\n.end\n" ADDER_MODEL,
                                   "adder:cout_sub"});
}

/* C-5: a subtraction whose b is not an inverted operand. */
static void test_unmap_decline_carry_in_one(void) {
    expect_adder_note((adder_case){
        CHAIN_HEAD ".subckt adder a=x0 b=y0 cin=vcc cout=c1 sumout=s0\n.end\n" ADDER_MODEL,
        "adder:carry_in_one"});
}

/* A multiply cell with WIDTH 0 has no operand bits: kept as it is, with the note. */
static void test_unmap_decline_mul_width(void) {
    load_lib();
    read_blif(CHAIN_HEAD ".end\n");
    odin3_net_id none = {0};
    odin3_netvec ports[3] = {{&none, 0}, {&none, 0}, {&none, 0}};
    odin3_value width = odin3_value_int(0);
    odin3_node_spec spec = {type_named("multiply"), 0, odin3_module_prov(top()), &width, 1};
    odin3_node_id made = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, ports, &made));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("multiply", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(intern("mul:width"), attr_of(made, "unmap_note")->str);
    char *first = census();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    char *second = census();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

/* R-2: a RAM with 32 address bits is not converted. */
static void test_unmap_decline_ram_abits(void) {
    load_lib();
    read_blif(CHAIN_HEAD
              ".subckt single_port_ram clk=x0 we=x1 addr[31]=x2 data[0]=y0 out[0]=s2\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id ram[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("single_port_ram", ram, 1));
    TEST_ASSERT_EQUAL_UINT32(intern("ram:abits"), attr_of(ram[0], "unmap_note")->str);
}

/* C-1: a ring of adders: the note is on the cells of the ring. */
static void test_unmap_decline_cycle(void) {
    load_lib();
    read_blif(CHAIN_HEAD ".subckt adder a=x0 b=y0 cin=c3 cout=c1 sumout=s0\n"
                         ".subckt adder a=x1 b=y1 cin=c1 cout=c2 sumout=s1\n"
                         ".subckt adder a=x2 b=y2 cin=c2 cout=c3 sumout=s2\n.end\n" ADDER_MODEL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id cells[3];
    TEST_ASSERT_EQUAL_UINT32(3, nodes_of("adder", cells, 3));
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(intern("adder:cycle"), attr_of(cells[i], "unmap_note")->str);
    }
    char *first = census();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    char *second = census();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

/* R-2: two cells of one group with 32 address bits: both get the note (they stay as they are). */
static void test_unmap_decline_ram_abits_group(void) {
    enum { WIDE = 32 };
    odin3_strbuf blif;
    odin3_strbuf_init(&blif);
    ok(odin3_strbuf_appendf(&blif, ".model m\n.inputs c w d0 d1"));
    for (int i = 0; i < WIDE; i++) {
        ok(odin3_strbuf_appendf(&blif, " a%d", i));
    }
    ok(odin3_strbuf_appendf(&blif, "\n.outputs\n.end\n"));
    load_lib();
    read_blif(blif.data);
    odin3_strbuf_free(&blif);
    odin3_prov_id prov = odin3_module_prov(top());
    odin3_net_id addr[WIDE];
    for (int i = 0; i < WIDE; i++) {
        char name[16];
        (void)snprintf(name, sizeof name, "a%d", i);
        addr[i] = net_named(name);
    }
    odin3_node_id cells[2];
    const char *data[2] = {"d0", "d1"};
    for (int i = 0; i < 2; i++) {
        odin3_net_id clk = net_named("c");
        odin3_net_id we = net_named("w");
        odin3_net_id din = net_named(data[i]);
        odin3_net_id dout = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top(), 0, prov, &dout));
        odin3_netvec ports[5] = {{&clk, 1}, {&din, 1}, {addr, WIDE}, {&we, 1}, {&dout, 1}};
        odin3_value params[2] = {odin3_value_int(WIDE), odin3_value_int(1)};
        odin3_node_spec spec = {type_named("single_port_ram"), 0, prov, params, 2};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_node_create_connected(top(), &spec, ports, &cells[i]));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("single_port_ram", NULL, 0));
    for (int i = 0; i < 2; i++) {
        const odin3_value *note = attr_of(cells[i], "unmap_note");
        TEST_ASSERT_NOT_NULL(note);
        TEST_ASSERT_EQUAL_UINT32(intern("ram:abits"), note->str);
    }
}

static void test_trim_and_run(void) {
    load_lib();
    read_blif(CLASS_BLIF);
    odin3_net_id nets[4] = {net_named("x"), net_named("x"), net_named("gnd"), net_named("unconn")};
    um_vec vec = {nets, 4};
    TEST_ASSERT_EQUAL_UINT32(2, um_trim_tail(top(), &vec, false));
    TEST_ASSERT_EQUAL_UINT32(2, vec.len);
    TEST_ASSERT_EQUAL_UINT32(1, um_top_run(top(), &vec)); /* both bits are net x */
    vec.len = 1;
    TEST_ASSERT_EQUAL_UINT32(1, um_top_run(top(), &vec));
    TEST_ASSERT_EQUAL_UINT32(0, um_trim_tail(top(), &vec, true));
    nets[0] = net_named("gnd");
    vec.len = 3;
    TEST_ASSERT_EQUAL_UINT32(1, um_trim_tail(top(), &vec, true));
    TEST_ASSERT_EQUAL_UINT32(2, vec.len);
}

/* --- Task 6: multiply to $mul ------------------------------------------------------------- */

static odin3_node_id only_mul(void) {
    odin3_node_id mul[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$mul", mul, 2));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("multiply", NULL, 0));
    return mul[0];
}

static void test_unmap_mul_8x7(void) {
    flow("module t(input [7:0] a, input [6:0] b, output [14:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    odin3_node_id mul = only_mul();
    expect_params(mul, (const int64_t[5]){0, 0, 8, 7, 15});
    TEST_ASSERT_EQUAL_INT64(36, attr_int(mul, "unmap_w"));
    TEST_ASSERT_EQUAL_UINT32(intern("mul"), attr_of(mul, "unmap")->str);
    check_clean();
}

static void test_unmap_mul_signed(void) {
    flow("module t(input signed [7:0] a, input signed [7:0] b, output signed [15:0] p);\n"
         "assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){1, 1, 8, 8, 16});
    check_clean();
}

static void test_unmap_mul_trunc(void) {
    flow("module t(input [7:0] a, input [7:0] b, output [7:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){0, 0, 8, 8, 8});
    check_clean();
}

static void test_unmap_mul_dead(void) {
    load_lib();
    read_blif(".model m\n.inputs x\n.outputs y\n.names unconn\n.names x y\n1 1\n"
              ".subckt multiply a[0]=unconn b[0]=unconn\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    expect_params(only_mul(), (const int64_t[5]){0, 0, 1, 1, 2});
    check_clean();
}

static void test_unmap_mul_blif(void) {
    load_lib();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        odin3_pass_run(
            design, "read_blif",
            odin3_bytes_cstr(src_path("tests/golden/blif-vtr/multiply_narrow.vtr.blif"))),
        log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    expect_params(only_mul(), (const int64_t[5]){0, 0, 8, 8, 16});
    check_clean();
}

/* --- Task 7: multiplier regroup, pattern P1 ------------------------------------------------ */

typedef struct p1_shape {
    int width; /* the multiply WIDTH */
    int cut;   /* s: A bits of the low product */
    int high;  /* h: A bits of the high product */
    int keep;  /* q: B bits of both */
    bool tap;  /* a LUT also reads a net of the low product's upper half */
} p1_shape;

/* One multiply cell: A bits x[first ..] (len of them), B bits y[0 .. keep), the rest of the pins
 * pad, and len + keep product bits on the nets l<j> (low) or u<j> (high). */
static void put_mul(odin3_strbuf *buf, const p1_shape *shape, int first, int len, bool low) {
    ok(odin3_strbuf_appendf(buf, ".subckt multiply"));
    for (int i = 0; i < shape->width; i++) {
        if (i < len) {
            ok(odin3_strbuf_appendf(buf, " a[%d]=x%d", i, first + i));
        } else {
            ok(odin3_strbuf_appendf(buf, " a[%d]=unconn", i));
        }
        if (i < shape->keep) {
            ok(odin3_strbuf_appendf(buf, " b[%d]=y%d", i, i));
        } else {
            ok(odin3_strbuf_appendf(buf, " b[%d]=unconn", i));
        }
    }
    for (int i = 0; i < len + shape->keep; i++) {
        if (low) {
            ok(odin3_strbuf_appendf(buf, " out[%d]=l%d", i, i));
        } else {
            ok(odin3_strbuf_appendf(buf, " out[%d]=u%d", i, i));
        }
    }
    ok(odin3_strbuf_appendf(buf, "\n"));
}

static void put_header(odin3_strbuf *buf, const p1_shape *shape) {
    ok(odin3_strbuf_appendf(buf, ".model m\n.inputs"));
    for (int i = 0; i < shape->cut + shape->high; i++) {
        ok(odin3_strbuf_appendf(buf, " x%d", i));
    }
    for (int i = 0; i < shape->keep; i++) {
        ok(odin3_strbuf_appendf(buf, " y%d", i));
    }
    ok(odin3_strbuf_appendf(buf, shape->tap ? "\n.outputs t" : "\n.outputs"));
    for (int i = 0; i < shape->cut + shape->high + shape->keep; i++) {
        ok(odin3_strbuf_appendf(buf, " p%d", i));
    }
    ok(odin3_strbuf_appendf(buf, "\n.names gnd\n.names vcc\n1\n.names unconn\n"));
}

/* The adder chain add0 = l[cut ..) + u[0 ..) without a top stage, and the output buffers. */
static void put_chain(odin3_strbuf *buf, const p1_shape *shape) {
    int stages = shape->high + shape->keep;
    ok(odin3_strbuf_appendf(buf, ".subckt adder a=vcc b=gnd cin=gnd cout=cr0\n"));
    for (int i = 0; i < stages; i++) {
        if (i < shape->keep) {
            ok(odin3_strbuf_appendf(buf, ".subckt adder a=l%d", shape->cut + i));
        } else {
            ok(odin3_strbuf_appendf(buf, ".subckt adder a=gnd"));
        }
        ok(odin3_strbuf_appendf(buf, " b=u%d cin=cr%d", i, i));
        ok(odin3_strbuf_appendf(buf, " cout=cr%d sumout=z%d\n", i + 1, i));
    }
    for (int i = 0; i < shape->cut; i++) {
        ok(odin3_strbuf_appendf(buf, ".names l%d p%d\n1 1\n", i, i));
    }
    for (int i = 0; i < stages; i++) {
        ok(odin3_strbuf_appendf(buf, ".names z%d p%d\n1 1\n", i, shape->cut + i));
    }
}

/* The hard cells of a one-side (A) split of an (cut + high) x keep multiplier, as VTR maps it. */
static void read_p1_blif(const p1_shape *shape) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    put_header(&buf, shape);
    put_mul(&buf, shape, 0, shape->cut, true);
    put_mul(&buf, shape, shape->cut, shape->high, false);
    put_chain(&buf, shape);
    if (shape->tap) {
        ok(odin3_strbuf_appendf(&buf, ".names l%d t\n1 1\n", shape->cut + 1));
    }
    ok(odin3_strbuf_appendf(&buf, ".end\n"));
    read_blif(buf.data);
    odin3_strbuf_free(&buf);
}

static void test_unmap_mul_split_a(void) {
    flow("module t(input [39:0] a, input [7:0] b, output [47:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    odin3_node_id mul = only_mul();
    expect_params(mul, (const int64_t[5]){0, 0, 40, 8, 48});
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(intern("mul.p1"), attr_of(mul, "unmap")->str);
    TEST_ASSERT_EQUAL_INT64(36, attr_int(mul, "unmap_w"));
    check_clean();
}

static void test_unmap_mul_split_b(void) {
    flow("module t(input [7:0] a, input [39:0] b, output [47:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){0, 0, 8, 40, 48});
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    check_clean();
}

static void test_unmap_mul_no_regroup(void) {
    flow("module t(input [39:0] a, input [7:0] b, output [47:0] p); assign p = a * b; endmodule\n");
    run_script("unmap --no-regroup\nopt_clean");
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$mul", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", NULL, 0));
    check_clean();
}

static void test_unmap_mul_blif_p1(void) {
    load_lib();
    read_p1_blif(&(p1_shape){8, 8, 3, 4, false});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    expect_params(only_mul(), (const int64_t[5]){0, 0, 11, 4, 15});
    check_clean();
}

static void test_unmap_mul_split_other_w(void) {
    load_lib();
    read_p1_blif(&(p1_shape){8, 5, 6, 3, false});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$mul", NULL, 0));
    odin3_node_id add[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 1));
    TEST_ASSERT_NULL(attr_of(add[0], "unmap_note"));
    check_clean();
}

/* --- Task 8: multiplier regroup, pattern P2 and nesting ------------------------------------ */

static void test_unmap_mul_split_ab(void) {
    flow(
        "module t(input [39:0] a, input [39:0] b, output [79:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    odin3_node_id mul = only_mul();
    expect_params(mul, (const int64_t[5]){0, 0, 40, 40, 80});
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(intern("mul.p2"), attr_of(mul, "unmap")->str);
    check_clean();
}

static void test_unmap_mul_split_nested(void) {
    flow("module t(input [79:0] a, input [79:0] b, output [159:0] p); assign p = a * b; "
         "endmodule\n");
    run_script("unmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){0, 0, 80, 80, 160});
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    check_clean();
}

static void test_unmap_mul_split_80x40(void) {
    flow("module t(input [79:0] a, input [39:0] b, output [119:0] p); assign p = a * b; "
         "endmodule\n");
    run_script("unmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){0, 0, 80, 40, 120});
    check_clean();
}

/* M-7: the pieces above the low product are dead (the result has 32 bits), and opt_clean removes
 * them before unmap sees them: what is left is one W x W multiplier. (A dead piece still reads
 * its inputs, so unmap on the uncleaned design cannot tell: it keeps them as they are.) */
static void test_unmap_mul_low_only(void) {
    flow(
        "module t(input [39:0] a, input [39:0] b, output [31:0] p); assign p = a * b; endmodule\n");
    run_script("opt_clean\nunmap\nopt_clean");
    expect_params(only_mul(), (const int64_t[5]){0, 0, 36, 36, 32});
    check_clean();
}

static void test_unmap_mul_shared_product(void) {
    load_lib();
    read_p1_blif(&(p1_shape){8, 8, 3, 4, true});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$mul", NULL, 0));
    odin3_node_id add[1];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", add, 1));
    TEST_ASSERT_EQUAL_UINT32(intern("mul:shared"), attr_of(add[0], "unmap_note")->str);
    check_clean();
}

/* A P2 shape: W = 3, a 2 x 2 high product, cross products 3 x 2 and 2 x 3. */
static void put_p2_mul(odin3_strbuf *buf, const char *const *pins, int out_len, const char *out) {
    enum { P2_WIDTH = 3 };
    ok(odin3_strbuf_appendf(buf, ".subckt multiply"));
    for (int i = 0; i < P2_WIDTH; i++) {
        ok(odin3_strbuf_appendf(buf, " a[%d]=%s b[%d]=%s", i, pins[i], i, pins[P2_WIDTH + i]));
    }
    for (int i = 0; i < out_len; i++) {
        ok(odin3_strbuf_appendf(buf, " out[%d]=%s%d", i, out, i));
    }
    ok(odin3_strbuf_appendf(buf, "\n"));
}

/* An adder chain of `stages` stages: sum bit i is `<sum>i`; the operands are name arrays. */
typedef struct p2_operands {
    const char *const *op_a;
    const char *const *op_b;
} p2_operands;

static void put_p2_chain(odin3_strbuf *buf, p2_operands ops, int stages, const char *sum) {
    ok(odin3_strbuf_appendf(buf, ".subckt adder a=vcc b=gnd cin=gnd cout=%sc0\n", sum));
    for (int i = 0; i < stages; i++) {
        ok(odin3_strbuf_appendf(buf, ".subckt adder a=%s b=%s cin=%sc%d cout=%sc%d sumout=%s%d\n",
                                ops.op_a[i], ops.op_b[i], sum, i, sum, i + 1, sum, i));
    }
}

/* cross_read: how many of the 5 bits of the first cross product the adder R0 reads (4: the top
 * bit of its operand is tied to ground and the product pin is not read). */
static void read_p2_blif(int cross_read) {
    static const char *const m00[] = {"x0", "x1", "x2", "y0", "y1", "y2"};
    static const char *const m11[] = {"x3", "x4", "unconn", "y3", "y4", "unconn"};
    static const char *const c1[] = {"x0", "x1", "x2", "y3", "y4", "unconn"};
    static const char *const c2[] = {"x3", "x4", "unconn", "y0", "y1", "y2"};
    static const char *const r0a[] = {"c0", "c1", "c2", "c3", "c4", "gnd"};
    static const char *const r0a_cut[] = {"c0", "c1", "c2", "c3", "gnd", "gnd"};
    static const char *const r0b[] = {"d0", "d1", "d2", "d3", "d4", "gnd"};
    static const char *const r1a[] = {"z0", "z1", "z2", "z3", "z4", "z5", "gnd"};
    static const char *const r1b[] = {"l3", "l4", "l5", "u0", "u1", "u2", "u3"};
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    ok(odin3_strbuf_appendf(&buf, ".model m\n.inputs x0 x1 x2 x3 x4 y0 y1 y2 y3 y4\n.outputs"));
    for (int i = 0; i < 10; i++) {
        ok(odin3_strbuf_appendf(&buf, " p%d", i));
    }
    ok(odin3_strbuf_appendf(&buf, "\n.names gnd\n.names vcc\n1\n.names unconn\n"));
    put_p2_mul(&buf, m00, 6, "l");
    put_p2_mul(&buf, m11, 4, "u");
    put_p2_mul(&buf, c1, cross_read, "c");
    put_p2_mul(&buf, c2, 5, "d");
    put_p2_chain(&buf, (p2_operands){cross_read == 5 ? r0a : r0a_cut, r0b}, 6, "z");
    put_p2_chain(&buf, (p2_operands){r1a, r1b}, 7, "s");
    for (int i = 0; i < 3; i++) {
        ok(odin3_strbuf_appendf(&buf, ".names l%d p%d\n1 1\n", i, i));
    }
    for (int i = 0; i < 7; i++) {
        ok(odin3_strbuf_appendf(&buf, ".names s%d p%d\n1 1\n", i, 3 + i));
    }
    ok(odin3_strbuf_appendf(&buf, ".end\n"));
    read_blif(buf.data);
    odin3_strbuf_free(&buf);
}

/* The generator makes a real P2 shape: all six cells merge into one 5 x 5 multiplier. */
static void test_unmap_mul_p2_blif(void) {
    load_lib();
    read_p2_blif(5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$add", NULL, 0));
    expect_params(only_mul(), (const int64_t[5]){0, 0, 5, 5, 10});
    check_clean();
}

/* An upper bit of a cross product that the adder does not read: not the full product, no merge. */
static void test_unmap_mul_p2_cross_narrow(void) {
    load_lib();
    read_p2_blif(4);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(4, nodes_of("$mul", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$add", NULL, 0));
    check_clean();
}

/* --- Task 9: RAM cells to $mem ------------------------------------------------------------- */

enum {
    MEM_SIZE = 1,
    MEM_ABITS = 3,
    MEM_WIDTH,
    MEM_RD_PORTS,
    MEM_WR_PORTS,
    MEM_RD_CLK_ENABLE,
    MEM_RD_CLK_POLARITY,
    MEM_RD_TRANSPARENT,
    MEM_WR_CLK_ENABLE,
    MEM_INIT = 12
};
enum { PORT_RD_EN = 1, PORT_WR_EN = 5, MEM_SLICES_MAX = 32 };

static odin3_node_id only_mem(void) {
    odin3_node_id mem[2];
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$mem", mem, 2));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("single_port_ram", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("dual_port_ram", NULL, 0));
    return mem[0];
}

/* The first of `count` $mem cells (the slices partial_map made: one data bit each). */
static odin3_node_id first_mem(uint32_t count) {
    odin3_node_id mem[MEM_SLICES_MAX];
    TEST_ASSERT_EQUAL_UINT32(count, nodes_of("$mem", mem, MEM_SLICES_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("single_port_ram", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("dual_port_ram", NULL, 0));
    return mem[0];
}

static int64_t mem_param(odin3_node_id mem, uint32_t index) {
    return odin3_node_param(top(), mem, index)->i;
}

/* ABITS, WIDTH, RD_PORTS and WR_PORTS of a $mem. */
static void expect_shape(odin3_node_id mem, const int64_t want[4]) {
    int64_t got[4] = {mem_param(mem, MEM_ABITS), mem_param(mem, MEM_WIDTH),
                      mem_param(mem, MEM_RD_PORTS), mem_param(mem, MEM_WR_PORTS)};
    TEST_ASSERT_EQUAL_INT64_ARRAY(want, got, 4);
    TEST_ASSERT_EQUAL_INT64(want[0] < 62 ? (int64_t)1 << want[0] : 0, mem_param(mem, MEM_SIZE));
}

static void test_unmap_spram(void) {
    flow_file(src_path("tests/micro/verilog/common/spram.v"));
    run_script("unmap --no-regroup\nopt_clean");
    odin3_node_id mem = first_mem(16); /* 16 one-bit slices; Task 10 merges them */
    expect_shape(mem, (const int64_t[4]){8, 1, 1, 1});
    const odin3_value *clk_en = odin3_node_param(top(), mem, MEM_RD_CLK_ENABLE);
    TEST_ASSERT_EQUAL_UINT32(1, clk_en->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, clk_en->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, odin3_node_param(top(), mem, MEM_RD_TRANSPARENT)->bits[0]);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_param(top(), mem, MEM_INIT)->len);
    TEST_ASSERT_EQUAL_INT(UM_VCC,
                          um_net_class(top(), um_port_net(top(), mem, (um_pos){PORT_RD_EN, 0})));
    TEST_ASSERT_EQUAL_INT(UM_SIG,
                          um_net_class(top(), um_port_net(top(), mem, (um_pos){PORT_WR_EN, 0})));
    check_clean();
}

static void test_unmap_dpram(void) {
    flow_file(src_path("tests/micro/verilog/common/dpram.v"));
    run_script("unmap --no-regroup\nopt_clean");
    expect_shape(first_mem(16), (const int64_t[4]){8, 1, 2, 2});
    check_clean();
}

static void test_unmap_rom(void) {
    flow_file(src_path("tests/micro/verilog/common/rom.v"));
    run_script("unmap --no-regroup\nopt_clean");
    odin3_node_id mem = first_mem(8);
    TEST_ASSERT_EQUAL_INT64(0, mem_param(mem, MEM_WR_PORTS));
    TEST_ASSERT_EQUAL_INT64(1, mem_param(mem, MEM_RD_PORTS));
    check_clean();
}

static void test_unmap_ram_dead(void) {
    load_lib();
    read_blif(".model m\n.inputs x\n.outputs y\n.names gnd\n.names x y\n1 1\n"
              ".subckt single_port_ram clk=x data[0]=x addr[0]=x we=gnd\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    expect_shape(only_mem(), (const int64_t[4]){1, 1, 1, 0});
    check_clean();
}

static void test_unmap_ram_blif_pad(void) {
    load_lib();
    read_blif(".model m\n.inputs a0 a1 a2 a3 a4 a5 d w c\n.outputs o\n.names unconn\n"
              ".subckt single_port_ram clk=c data[0]=d we=w out[0]=o addr[0]=a0 addr[1]=a1 "
              "addr[2]=a2 addr[3]=a3 addr[4]=a4 addr[5]=a5 addr[6]=unconn addr[7]=unconn "
              "addr[8]=unconn addr[9]=unconn addr[10]=unconn addr[11]=unconn addr[12]=unconn "
              "addr[13]=unconn addr[14]=unconn\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    expect_shape(only_mem(), (const int64_t[4]){6, 1, 1, 1});
    check_clean();
}

/* --- Task 10: RAM width and replica merge by origin ---------------------------------------- */

enum { PORT_WR_DATA = 7, RAM_BITS = 8 };

static void test_unmap_ram_width_merge(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    odin3_node_id slices[RAM_BITS];
    TEST_ASSERT_EQUAL_UINT32(RAM_BITS, nodes_of("dual_port_ram", slices, RAM_BITS));
    odin3_net_id data[RAM_BITS];
    for (uint32_t i = 0; i < RAM_BITS; i++) {
        data[i] = um_port_net(top(), slices[i], (um_pos){2, 0}); /* data1 */
    }
    odin3_prov_id origin = odin3_node_prov(top(), slices[0]);
    run_script("unmap\nopt_clean");
    odin3_node_id mem = first_mem(1);
    expect_shape(mem, (const int64_t[4]){6, 8, 1, 1});
    TEST_ASSERT_EQUAL_UINT32(intern("ram.width"), attr_of(mem, "unmap")->str);
    TEST_ASSERT_EQUAL_INT64(RAM_BITS, attr_int(mem, "unmap_pieces"));
    TEST_ASSERT_TRUE(origin.v != 0);
    TEST_ASSERT_EQUAL_INT64(origin.v, attr_int(mem, "unmap_origin"));
    for (uint32_t i = 0; i < RAM_BITS; i++) {
        TEST_ASSERT_EQUAL_UINT32(data[i].v, um_port_net(top(), mem, (um_pos){PORT_WR_DATA, i}).v);
    }
    check_clean();
}

/* partial_map's role numbers agree with node-ID order (the tests below run unmap on it). */
static void test_unmap_ram_bit_order(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    odin3_node_id slices[RAM_BITS];
    TEST_ASSERT_EQUAL_UINT32(RAM_BITS, nodes_of("dual_port_ram", slices, RAM_BITS));
    for (int i = 0; i < RAM_BITS; i++) {
        char *end = NULL;
        const char *role =
            um_attr_text(&(um_ctx){.design = design, .module = top()}, slices[i], "hardmap");
        TEST_ASSERT_NOT_NULL(role);
        const char *tag = strstr(role, ":b");
        TEST_ASSERT_NOT_NULL(tag);
        TEST_ASSERT_EQUAL_INT64(i, strtol(tag + 2, &end, 10));
    }
}

/* The nets the slices of ram_w8 write (port data1) and read (port out2), in node-ID order. */
enum { HARD_DATA1 = 2, HARD_OUT2 = 7, MEM_RD_DATA_PORT = 3, MEM_WR_DATA_PORT = 7 };

typedef struct slice_nets {
    odin3_node_id slice[RAM_BITS];
    odin3_net_id wdata[RAM_BITS];
    odin3_net_id rdata[RAM_BITS];
} slice_nets;

static void read_slices(slice_nets *sn) {
    TEST_ASSERT_EQUAL_UINT32(RAM_BITS, nodes_of("dual_port_ram", sn->slice, RAM_BITS));
    for (int i = 0; i < RAM_BITS; i++) {
        sn->wdata[i] = um_port_net(top(), sn->slice[i], (um_pos){HARD_DATA1, 0});
        sn->rdata[i] = um_port_net(top(), sn->slice[i], (um_pos){HARD_OUT2, 0});
        TEST_ASSERT_TRUE(odin3_net_valid(sn->wdata[i]) && odin3_net_valid(sn->rdata[i]));
    }
}

/* Gives slice `node` the role `b<bit>` partial_map writes. */
static void set_role(odin3_node_id node, int bit) {
    char text[32];
    (void)snprintf(text, sizeof text, "dual_port_ram:b%d", bit);
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(text), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   intern("hardmap"), &value));
}

/* The merged $mem takes slice `order[k]` as data bit k. */
static void expect_merged_order(const slice_nets *sn, const int order[RAM_BITS]) {
    odin3_node_id mem = first_mem(1);
    expect_shape(mem, (const int64_t[4]){6, RAM_BITS, 1, 1});
    for (int k = 0; k < RAM_BITS; k++) {
        TEST_ASSERT_EQUAL_UINT32(
            sn->wdata[order[k]].v,
            um_port_net(top(), mem, (um_pos){MEM_WR_DATA_PORT, (uint32_t)k}).v);
        TEST_ASSERT_EQUAL_UINT32(
            sn->rdata[order[k]].v,
            um_port_net(top(), mem, (um_pos){MEM_RD_DATA_PORT, (uint32_t)k}).v);
    }
}

static void test_unmap_ram_merged_bit_order(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    slice_nets sn;
    read_slices(&sn);
    run_script("unmap");
    expect_merged_order(&sn, (const int[RAM_BITS]){0, 1, 2, 3, 4, 5, 6, 7});
}

/* R-6: the role numbers decide when every slice has one, whatever the node-ID order is. */
static void test_unmap_ram_role_order(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    slice_nets sn;
    read_slices(&sn);
    for (int i = 0; i < RAM_BITS; i++) {
        set_role(sn.slice[i], RAM_BITS - 1 - i);
    }
    run_script("unmap");
    expect_merged_order(&sn, (const int[RAM_BITS]){7, 6, 5, 4, 3, 2, 1, 0});
}

/* One slice without a role: node-ID order for all of them. */
static void test_unmap_ram_role_order_partial(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    slice_nets sn;
    read_slices(&sn);
    for (int i = 0; i < RAM_BITS; i++) {
        set_role(sn.slice[i], RAM_BITS - 1 - i);
    }
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_attr_remove(top(), (odin3_objref){ODIN3_OBJ_NODE, sn.slice[3].v}, intern("hardmap")));
    run_script("unmap");
    expect_merged_order(&sn, (const int[RAM_BITS]){0, 1, 2, 3, 4, 5, 6, 7});
}

static void test_unmap_ram_replicas(void) {
    flow_file(src_path("tests/unmap/ram_2r1w.v"));
    run_script("unmap\nopt_clean");
    odin3_node_id mem = first_mem(1);
    expect_shape(mem, (const int64_t[4]){4, 4, 2, 1});
    TEST_ASSERT_EQUAL_UINT32(intern("ram.replica"), attr_of(mem, "unmap")->str);
    check_clean();
}

static void test_unmap_ram_blif_candidates(void) {
    load_lib();
    read_blif(".model m\n.inputs c w a0 d0 d1\n.outputs o0 o1\n"
              ".subckt single_port_ram clk=c we=w addr[0]=a0 data[0]=d0 out[0]=o0\n"
              ".subckt single_port_ram clk=c we=w addr[0]=a0 data[0]=d1 out[0]=o1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    odin3_node_id mem[3];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$mem", mem, 3));
    TEST_ASSERT_EQUAL_UINT32(intern("ram:0"), attr_of(mem[0], "unmap_cand")->str);
    TEST_ASSERT_EQUAL_UINT32(intern("ram:0"), attr_of(mem[1], "unmap_cand")->str);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "unmap: 2 RAM cells with equal control nets and "
                                          "different origin kept apart (candidate set ram:0)"));
    check_clean();
}

static void test_unmap_ram_no_regroup(void) {
    flow_file(src_path("tests/unmap/ram_w8.v"));
    run_script("unmap --no-regroup\nopt_clean");
    TEST_ASSERT_EQUAL_UINT32(RAM_BITS, nodes_of("$mem", NULL, 0));
}

/* --- Task 11: log line, idempotence, robustness -------------------------------------------- */

enum { RTLIL_CAP = 262144 };

/* The text of the file `name` in the test directory. */
static char *slurp(const char *name) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = calloc(RTLIL_CAP, 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, RTLIL_CAP - 1, file);
    TEST_ASSERT_TRUE(got > 0 && got < RTLIL_CAP - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    return text;
}

static void write_rtlil_to(const char *name) {
    char text[PATH_BUF];
    (void)snprintf(text, sizeof text, "write_rtlil %s/%s", dir, name);
    run_script(text);
}

static void test_unmap_info_line(void) {
    flow(MOD_ADD);
    run_script("unmap");
    TEST_ASSERT_NOT_NULL(strstr(log_text,
                                "unmap: 10 adder cells to 1 $add + 0 $sub, 0 multiply to 0 "
                                "$mul (0 regrouped), 0 RAM cells to 0 $mem, 0 groups left "
                                "(see unmap_note)"));
}

static void test_unmap_info_line_regroup(void) {
    flow(
        "module t(input [39:0] a, input [39:0] b, output [79:0] p); assign p = a * b; endmodule\n");
    run_script("unmap");
    TEST_ASSERT_NOT_NULL(strstr(log_text, "4 multiply to 1 $mul (3 regrouped)"));
}

static void test_unmap_idempotent(void) {
    flow(
        "module t(input [39:0] a, input [39:0] b, output [79:0] p); assign p = a * b; endmodule\n");
    run_script("unmap\nopt_clean");
    write_rtlil_to("first.il");
    log_text[0] = '\0';
    run_script("unmap");
    write_rtlil_to("second.il");
    TEST_ASSERT_NULL(strstr(log_text, "adder cells to"));
    char *first = slurp("first.il");
    char *second = slurp("second.il");
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

static void test_unmap_no_lib(void) {
    write_text("t.v", odin3_bytes_cstr(MOD_ADD));
    char text[PATH_BUF];
    (void)snprintf(text, sizeof text, "read_verilog %s/t.v\nproc\nopt\nunmap", dir);
    run_script(text);
    TEST_ASSERT_NULL(strstr(log_text, "adder cells to"));
    TEST_ASSERT_EQUAL_UINT32(1, nodes_of("$add", NULL, 0));
}

static uint32_t count_in_all(const char *type_name) {
    odin3_celltype_id type = type_named(type_name);
    uint32_t count = 0;
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        odin3_module *mod = odin3_module_get(design, (odin3_module_id){mid});
        for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
            odin3_node_id node = {nid};
            count += odin3_node_live(mod, node) && odin3_node_type(mod, node).v == type.v ? 1 : 0;
        }
    }
    return count;
}

#define MODEL_CHAIN(name)                                                                          \
    ".model " name "\n.inputs x y\n.outputs s\n.names gnd\n.names vcc\n1\n"                        \
    ".subckt adder a=vcc b=gnd cin=gnd cout=c0\n"                                                  \
    ".subckt adder a=x b=y cin=c0 cout=c1 sumout=s\n.end\n"

static void test_unmap_two_modules(void) {
    load_lib();
    read_blif(MODEL_CHAIN("one") MODEL_CHAIN("two"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, unmap(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, count_in_all("$add"));
    TEST_ASSERT_EQUAL_UINT32(0, count_in_all("adder"));
    check_clean();
}

static void test_unmap_determinism(void) {
    for (int round = 0; round < 2; round++) {
        odin3_design_destroy(design);
        design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        flow("module t(input [39:0] a, input [7:0] b, output [47:0] p); assign p = a * b; "
             "endmodule\n");
        run_script("unmap\nopt_clean");
        write_rtlil_to(round == 0 ? "one.il" : "two.il");
    }
    char *one = slurp("one.il");
    char *two = slurp("two.il");
    TEST_ASSERT_EQUAL_STRING(one, two);
    free(one);
    free(two);
}

/* --- Task 15: depth recovery, the write enable tree ---------------------------------------- */

enum { DEEP_LEAVES = 4, DEEP_LEVELS = 2 };

/* The four leaves of ram_deep.v after unmap --no-regroup, as a vec of node IDs. */
static void deep_leaves(odin3_vec *leaves) {
    flow_file(src_path("tests/unmap/ram_deep.v"));
    run_script("unmap --no-regroup");
    odin3_node_id nodes[DEEP_LEAVES];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", nodes, DEEP_LEAVES));
    odin3_vec_init(leaves, sizeof(odin3_node_id));
    for (uint32_t i = 0; i < DEEP_LEAVES; i++) {
        odin3_node_id *slot = odin3_vec_push(leaves);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = nodes[i];
    }
}

static odin3_net_id port_bit(const char *wire_name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(top(), intern(wire_name));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(top(), wire, bit);
}

/* Some live `$not` reads the net. */
static bool reads_not(odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(top(), net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        odin3_node_id node = odin3_pin_node(top(), sinks.pins[i]);
        if (odin3_node_type(top(), node).v == type_named("$not").v) {
            return true;
        }
    }
    return false;
}

static void check_tree(const um_write_tree *tree) {
    const uint32_t *got = tree->paths.data;
    uint32_t seen = 0;
    for (size_t i = 0; i < tree->paths.len; i++) {
        seen |= 1U << got[i];
    }
    TEST_ASSERT_EQUAL_UINT32(0xF, seen); /* the four paths 0..3, one per leaf */
}

static void check_tree_nets(const um_write_tree *tree) {
    const odin3_net_id *nets = tree->ad.data;
    TEST_ASSERT_TRUE(odin3_net_valid(nets[0]) && odin3_net_valid(nets[1]));
    TEST_ASSERT_TRUE(nets[0].v != nets[1].v && nets[0].v != tree->we0.v);
    TEST_ASSERT_TRUE(reads_not(nets[0]) && reads_not(nets[1]));
}

/* R-8 step 1 on the leaves: whether the structure matched (the status is always OK here). */
static bool write_tree_matches(const um_ctx *uc, const odin3_vec *leaves, um_write_tree *tree) {
    bool matched = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, um_depth_write_tree(uc, leaves, DEEP_LEVELS, tree, &matched));
    return matched;
}

static void test_depth_write_tree(void) {
    odin3_vec leaves;
    deep_leaves(&leaves);
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    um_write_tree tree;
    TEST_ASSERT_TRUE(write_tree_matches(&uc, &leaves, &tree));
    check_tree(&tree);
    check_tree_nets(&tree);
    um_write_tree_free(&tree);
    odin3_vec_free(&leaves);
}

/* Moves the input `port` of the `$and` that is the first link of the leaf's enable chain onto
 * `net`. */
static void rewire_leaf_and(odin3_node_id leaf, uint32_t port, odin3_net_id net) {
    odin3_net_id enable = um_port_net(top(), leaf, (um_pos){5, 0});
    odin3_node_id gate = odin3_pin_node(top(), odin3_net_driver(top(), enable));
    odin3_pinslice pins = odin3_node_port(top(), gate, port);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top(), pins.first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top(), pins.first, net));
}

static void test_depth_write_mismatch(void) {
    odin3_vec leaves;
    deep_leaves(&leaves);
    const odin3_node_id *nodes = leaves.data;
    rewire_leaf_and(nodes[0], 1, port_bit("addr", 5)); /* another address net at a level */
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    um_write_tree tree;
    TEST_ASSERT_FALSE(write_tree_matches(&uc, &leaves, &tree));
    um_write_tree_free(&tree);
    odin3_vec_free(&leaves);
}

static void test_depth_write_duplicate_path(void) {
    odin3_vec leaves;
    deep_leaves(&leaves);
    const odin3_node_id *nodes = leaves.data;
    /* leaf 1 gets leaf 0's write enable: two leaves have one path */
    odin3_net_id enable = um_port_net(top(), nodes[0], (um_pos){5, 0});
    odin3_pinslice pins = odin3_node_port(top(), nodes[1], 5);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top(), pins.first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top(), pins.first, enable));
    um_opts opts = {true};
    um_ctx uc = make_ctx(&opts);
    um_write_tree tree;
    TEST_ASSERT_FALSE(write_tree_matches(&uc, &leaves, &tree));
    um_write_tree_free(&tree);
    odin3_vec_free(&leaves);
}

/* --- Task 16: depth recovery, read side and merge ------------------------------------------ */

static void flow_deep(void) {
    flow_file(src_path("tests/unmap/ram_deep.v"));
}

static void test_unmap_depth_roundtrip(void) {
    flow_deep();
    run_script("unmap\nopt_clean");
    odin3_node_id mem = first_mem(1);
    expect_shape(mem, (const int64_t[4]){17, 1, 1, 1});
    TEST_ASSERT_EQUAL_UINT32(intern("ram.depth"), attr_of(mem, "unmap")->str);
    /* the three `$mux` cells proc made for the write (RTL logic) stay; the glue is gone */
    TEST_ASSERT_EQUAL_UINT32(3, nodes_of("$mux", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$dff", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$and", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$not", NULL, 0));
    check_clean();
}

/* A user `$not` reads the output of a glue `$not` of the write side. */
static odin3_node_id add_reader_of_glue_not(void) {
    odin3_node_id glue[8];
    uint32_t count = nodes_of("$not", glue, 8);
    TEST_ASSERT_TRUE(count > 0);
    odin3_net_id shared = um_port_net(top(), glue[0], (um_pos){1, 0});
    odin3_net_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top(), 0, odin3_module_prov(top()), &out));
    odin3_netvec ports[2] = {{&shared, 1}, {&out, 1}};
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(1)};
    odin3_node_spec spec = {type_named("$not"), 0, odin3_module_prov(top()), params, 3};
    odin3_node_id user = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, ports, &user));
    return glue[0];
}

static void test_unmap_depth_shared_not(void) {
    flow_deep();
    odin3_node_id kept = add_reader_of_glue_not();
    run_script("unmap");
    first_mem(1);
    TEST_ASSERT_TRUE(odin3_node_live(top(), kept)); /* somebody else reads it: it stays */
    check_clean();
}

/* The first `$mux` of the output tree: the glue partial_map built carries `hardmap`. */
static odin3_node_id first_glue_mux(void) {
    odin3_node_id mux[16];
    uint32_t count = nodes_of("$mux", mux, 16);
    for (uint32_t i = 0; i < count && i < 16; i++) {
        if (attr_of(mux[i], "hardmap") != NULL) {
            return mux[i];
        }
    }
    TEST_FAIL_MESSAGE("no glue mux");
    return (odin3_node_id){0};
}

/* Swaps the data inputs of the first glue `$mux`. */
static void swap_mux_inputs(void) {
    odin3_node_id mux = first_glue_mux();
    odin3_pinslice pin_a = odin3_node_port(top(), mux, 0);
    odin3_pinslice pin_b = odin3_node_port(top(), mux, 1);
    odin3_net_id net_a = odin3_pin_net(top(), pin_a.first);
    odin3_net_id net_b = odin3_pin_net(top(), pin_b.first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top(), pin_a.first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top(), pin_b.first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top(), pin_a.first, net_b));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top(), pin_b.first, net_a));
}

static void test_unmap_depth_mismatch_note(void) {
    flow_deep();
    swap_mux_inputs();
    run_script("unmap");
    odin3_node_id mems[4];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", mems, DEEP_LEAVES));
    for (uint32_t i = 0; i < DEEP_LEAVES; i++) {
        TEST_ASSERT_EQUAL_UINT32(intern("ram:depth"), attr_of(mems[i], "unmap_note")->str);
    }
    TEST_ASSERT_NOT_NULL(strstr(log_text, "not recovered: depth"));
}

static void test_unmap_depth_lowered(void) {
    flow_deep();
    run_script("lower\nunmap");
    odin3_node_id mems[4];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", mems, DEEP_LEAVES));
    TEST_ASSERT_NULL(attr_of(mems[0], "unmap_note"));
    TEST_ASSERT_NULL(strstr(log_text, "not recovered"));
}

/* The micros of the small depth-2 architecture (tests/unmap/arch_d2.xml): 4-bit addresses split
 * into four leaves per data bit. */
static void flow_small(const char *micro) {
    char text[PATH_BUF * 3];
    (void)snprintf(text, sizeof text,
                   "read_arch %s\nread_verilog --vtr-compat %s\nproc\nopt\nhardmap\n",
                   src_path("tests/unmap/arch_d2.xml"), micro);
    run_script(text);
}

static const char *micro_path(const char *name) {
    static char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/tests/unmap/%s", ODIN3_UM_SRC, name);
    return path;
}

static void test_unmap_depth_multibit(void) {
    flow_small(micro_path("ram_deep_w2.v"));
    run_script("unmap\nopt_clean");
    odin3_node_id mem = first_mem(1);
    expect_shape(mem, (const int64_t[4]){4, 2, 1, 1});
    TEST_ASSERT_EQUAL_UINT32(intern("ram.depth"), attr_of(mem, "unmap")->str);
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$and", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$not", NULL, 0));
    check_clean();
}

/* Two read ports: the replicas of the write side keep two `$mem` cells, each depth-merged. */
static void test_unmap_depth_two_reads(void) {
    flow_small(micro_path("ram_deep_2r.v"));
    run_script("unmap\nopt_clean");
    odin3_node_id mems[2];
    TEST_ASSERT_EQUAL_UINT32(2, nodes_of("$mem", mems, 2));
    for (uint32_t i = 0; i < 2; i++) {
        expect_shape(mems[i], (const int64_t[4]){4, 2, 1, 1});
        TEST_ASSERT_EQUAL_UINT32(intern("ram.depth"), attr_of(mems[i], "unmap")->str);
    }
    TEST_ASSERT_EQUAL_UINT32(0, nodes_of("$and", NULL, 0));
    check_clean();
}

/* The two-bit memory refused once (a swapped mux), put right, then run again: the control of the
 * tests that damage one more thing between the runs. */
static void refuse_then_restore(void) {
    flow_small(micro_path("ram_deep_w2.v"));
    swap_mux_inputs();
    run_script("unmap");
    odin3_node_id leaves[4];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", leaves, DEEP_LEAVES));
    TEST_ASSERT_EQUAL_UINT32(intern("ram:depth"), attr_of(leaves[0], "unmap_note")->str);
    swap_mux_inputs();
}

static void test_unmap_depth_second_run(void) {
    refuse_then_restore();
    run_script("unmap");
    first_mem(1);
    check_clean();
}

/* R-8: the write enable of every data bit of every leaf is the same net. */
static void test_unmap_depth_bit_enables_differ(void) {
    refuse_then_restore();
    odin3_node_id leaves[4];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", leaves, DEEP_LEAVES));
    odin3_pinslice enables = odin3_node_port(top(), leaves[2], PORT_WR_EN);
    TEST_ASSERT_EQUAL_UINT32(2, enables.count);
    odin3_pin_id second = {enables.first.v + 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(top(), second));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(top(), second, port_bit("we", 0)));
    run_script("unmap");
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", leaves, DEEP_LEAVES));
    TEST_ASSERT_EQUAL_UINT32(intern("ram:depth"), attr_of(leaves[0], "unmap_note")->str);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "not recovered: depth"));
}

/* The registered select of a leaf level is replaced by a `$dff` with the given polarity and
 * width; the select net stays bit `at` of its Q. */
typedef struct select_dff {
    int64_t polarity;
    uint32_t width;
    uint32_t at;
} select_dff;

static void replace_select_dff(select_dff want) {
    odin3_node_id dffs[8];
    uint32_t count = nodes_of("$dff", dffs, 8);
    odin3_node_id old = {0};
    for (uint32_t i = 0; i < count && i < 8 && old.v == 0; i++) {
        old = attr_of(dffs[i], "hardmap") != NULL ? dffs[i] : old;
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, old.v);
    odin3_net_id clk = um_port_net(top(), old, (um_pos){0, 0});
    odin3_net_id in = um_port_net(top(), old, (um_pos){1, 0});
    odin3_net_id out = um_port_net(top(), old, (um_pos){2, 0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(top(), old));
    odin3_net_id data_in[2] = {in, in};
    odin3_net_id data_out[2] = {{0}, {0}};
    for (uint32_t i = 0; i < want.width; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_net_create(top(), 0, odin3_module_prov(top()), &data_out[i]));
    }
    data_out[want.at] = out;
    odin3_netvec ports[3] = {{&clk, 1}, {data_in, want.width}, {data_out, want.width}};
    odin3_value params[2] = {odin3_value_int(want.width), odin3_value_int(want.polarity)};
    odin3_node_spec spec = {type_named("$dff"), 0, odin3_module_prov(top()), params, 2};
    odin3_node_id made = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top(), &spec, ports, &made));
}

static void expect_depth_refused(void) {
    run_script("unmap");
    odin3_node_id leaves[DEEP_LEAVES];
    TEST_ASSERT_EQUAL_UINT32(DEEP_LEAVES, nodes_of("$mem", leaves, DEEP_LEAVES));
    TEST_ASSERT_EQUAL_UINT32(intern("ram:depth"), attr_of(leaves[0], "unmap_note")->str);
}

static void test_unmap_depth_select_dff_ok(void) {
    flow_deep();
    replace_select_dff((select_dff){1, 1, 0});
    run_script("unmap");
    first_mem(1);
}

static void test_unmap_depth_select_negedge(void) {
    flow_deep();
    replace_select_dff((select_dff){0, 1, 0});
    expect_depth_refused();
}

static void test_unmap_depth_select_wide_dff(void) {
    flow_deep();
    replace_select_dff((select_dff){1, 2, 0});
    expect_depth_refused();
}

static void test_unmap_depth_select_on_upper_bit(void) {
    flow_deep();
    replace_select_dff((select_dff){1, 2, 1});
    expect_depth_refused();
}

static void run_kit_tests(void) {
    RUN_TEST(test_unmap_options);
    RUN_TEST(test_net_class);
    RUN_TEST(test_net_class_x);
    RUN_TEST(test_observed);
    RUN_TEST(test_types_find);
    RUN_TEST(test_types_find_no_lib);
    RUN_TEST(test_types_find_wrong_shape);
    RUN_TEST(test_const_net);
    RUN_TEST(test_replace_provenance_and_attrs);
    RUN_TEST(test_replace_no_prov);
    RUN_TEST(test_replace_failure_keeps_group);
}

static void run_chain_tests(void) {
    RUN_TEST(test_walks_simple);
    RUN_TEST(test_walks_order_independent);
    RUN_TEST(test_walks_branch);
    RUN_TEST(test_walks_tap);
    RUN_TEST(test_walks_cycle);
    RUN_TEST(test_walks_two_chains);
    RUN_TEST(test_unmap_add_roundtrip);
    RUN_TEST(test_unmap_sub_roundtrip);
    RUN_TEST(test_unmap_add_signed);
    RUN_TEST(test_unmap_not_shared);
    RUN_TEST(test_unmap_sub_absorbs_not);
    RUN_TEST(test_unmap_cout_tap_end);
    RUN_TEST(test_unmap_cout_tap_middle);
    RUN_TEST(test_unmap_parmys_blif);
    RUN_TEST(test_unmap_declines);
    RUN_TEST(test_trim_and_run);
    RUN_TEST(test_unmap_cin_from_multiply_twice);
    RUN_TEST(test_unmap_second_run_tap);
    RUN_TEST(test_unmap_second_run_branch);
    RUN_TEST(test_unmap_second_run_ram_pair);
    RUN_TEST(test_unmap_decline_cout_sub);
    RUN_TEST(test_unmap_decline_carry_in_one);
    RUN_TEST(test_unmap_decline_mul_width);
    RUN_TEST(test_unmap_decline_ram_abits);
    RUN_TEST(test_unmap_decline_cycle);
    RUN_TEST(test_unmap_decline_ram_abits_group);
}

static void run_mul_tests(void) {
    RUN_TEST(test_unmap_mul_8x7);
    RUN_TEST(test_unmap_mul_signed);
    RUN_TEST(test_unmap_mul_trunc);
    RUN_TEST(test_unmap_mul_dead);
    RUN_TEST(test_unmap_mul_blif);
    RUN_TEST(test_unmap_mul_split_a);
    RUN_TEST(test_unmap_mul_split_b);
    RUN_TEST(test_unmap_mul_no_regroup);
    RUN_TEST(test_unmap_mul_blif_p1);
    RUN_TEST(test_unmap_mul_split_other_w);
    RUN_TEST(test_unmap_mul_split_ab);
    RUN_TEST(test_unmap_mul_split_nested);
    RUN_TEST(test_unmap_mul_split_80x40);
    RUN_TEST(test_unmap_mul_low_only);
    RUN_TEST(test_unmap_mul_shared_product);
    RUN_TEST(test_unmap_mul_p2_blif);
    RUN_TEST(test_unmap_mul_p2_cross_narrow);
}

static void run_ram_tests(void) {
    RUN_TEST(test_unmap_spram);
    RUN_TEST(test_unmap_dpram);
    RUN_TEST(test_unmap_rom);
    RUN_TEST(test_unmap_ram_dead);
    RUN_TEST(test_unmap_ram_blif_pad);
    RUN_TEST(test_unmap_ram_width_merge);
    RUN_TEST(test_unmap_ram_bit_order);
    RUN_TEST(test_unmap_ram_merged_bit_order);
    RUN_TEST(test_unmap_ram_role_order);
    RUN_TEST(test_unmap_ram_role_order_partial);
    RUN_TEST(test_unmap_ram_replicas);
    RUN_TEST(test_unmap_ram_blif_candidates);
    RUN_TEST(test_unmap_ram_no_regroup);
}

static void run_misc_tests(void) {
    RUN_TEST(test_depth_write_tree);
    RUN_TEST(test_depth_write_mismatch);
    RUN_TEST(test_depth_write_duplicate_path);
    RUN_TEST(test_unmap_depth_roundtrip);
    RUN_TEST(test_unmap_depth_shared_not);
    RUN_TEST(test_unmap_depth_mismatch_note);
    RUN_TEST(test_unmap_depth_lowered);
    RUN_TEST(test_unmap_depth_multibit);
    RUN_TEST(test_unmap_depth_two_reads);
    RUN_TEST(test_unmap_depth_second_run);
    RUN_TEST(test_unmap_depth_bit_enables_differ);
    RUN_TEST(test_unmap_depth_select_dff_ok);
    RUN_TEST(test_unmap_depth_select_negedge);
    RUN_TEST(test_unmap_depth_select_wide_dff);
    RUN_TEST(test_unmap_depth_select_on_upper_bit);
    RUN_TEST(test_unmap_info_line);
    RUN_TEST(test_unmap_info_line_regroup);
    RUN_TEST(test_unmap_idempotent);
    RUN_TEST(test_unmap_no_lib);
    RUN_TEST(test_unmap_two_modules);
    RUN_TEST(test_unmap_determinism);
}

int main(void) {
    UNITY_BEGIN();
    run_kit_tests();
    run_chain_tests();
    run_mul_tests();
    run_ram_tests();
    run_misc_tests();
    return UNITY_END();
}
