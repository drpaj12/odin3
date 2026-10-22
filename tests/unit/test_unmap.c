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
#include "passes/unmap/kit.h"
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unmap_options);
    RUN_TEST(test_net_class);
    RUN_TEST(test_net_class_x);
    RUN_TEST(test_observed);
    RUN_TEST(test_types_find);
    RUN_TEST(test_types_find_no_lib);
    RUN_TEST(test_types_find_wrong_shape);
    RUN_TEST(test_const_net);
    return UNITY_END();
}
