/*
 * test_submod.c — the submod pass (Phase 7 spec 3.1, plan Task 5): the group moved into a new
 * module with the right ports, provenance and attributes, the refusals of S-5 (name clash, port
 * node, empty group) and Review Focus 1 (a boundary net with two drivers). Designs are small BLIF
 * texts in a mkdtemp directory under the build tree.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "passes/submod/submod.h"
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

#ifndef ODIN3_SM_ROOT
#error "ODIN3_SM_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, LOG_MAX = 16384, MAX_PINS = 16 };

/* a, b, c in; n1 = a&b; n2 = n1&c; n3 = n2|a; y = n3^b. */
#define FOUR_GATES                                                                      \
    ".model m\n.inputs a b c\n.outputs y\n"                                             \
    ".names a b n1\n11 1\n.names n1 c n2\n11 1\n.names n2 a n3\n1- 1\n-1 1\n"           \
    ".names n3 b y\n10 1\n01 1\n.end\n"

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
    (void)snprintf(dir, sizeof dir, "%s/sm.XXXXXX", ODIN3_SM_ROOT);
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

static odin3_status run(const char *args) {
    return odin3_pass_run(design, "submod", odin3_bytes_cstr(args));
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

static odin3_net_id net_named(const odin3_module *module, const char *name) {
    odin3_net_id net = odin3_module_find_net(module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), name);
    return net;
}

static odin3_node_id driver_of(const odin3_module *module, const char *name) {
    odin3_pin_id pin = odin3_net_driver(module, net_named(module, name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_pin_valid(pin), name);
    return odin3_pin_node(module, pin);
}

static void mark_str(odin3_module *module, odin3_node_id node, const char *key, const char *val) {
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(val), 0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern(key),
                                 &value));
}

static void mark_driver(const char *net, const char *group) {
    mark_str(top(), driver_of(top(), net), "submod", group);
}

static void check_clean(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(design, opts), log_text);
}

static bool is_port_node(const odin3_module *module, odin3_node_id node) {
    return odin3_celltype_get(design, odin3_node_type(module, node))->gran == ODIN3_GRAN_PORT;
}

/* Live nodes of module that are not port nodes, and how many of those are of celltype type. */
static uint32_t count_cells(const odin3_module *module, odin3_celltype_id only) {
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(module); nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(module, node) || is_port_node(module, node)) {
            continue;
        }
        count += only.v == 0 || odin3_node_type(module, node).v == only.v ? 1U : 0U;
    }
    return count;
}

static odin3_module *module_named(const char *name) {
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        if (strcmp(odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(module)),
                   name) == 0) {
            return module;
        }
    }
    return NULL;
}

/* The direction of the port called name of module, -1 when it has none. */
static int port_dir(const odin3_module *module, const char *name) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(module));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return (int)def->ports[i].dir;
        }
    }
    return -1;
}

static uint32_t port_count(const odin3_module *module) {
    return odin3_celltype_get(design, odin3_module_celltype(module))->n_ports;
}

/* --- tests --------------------------------------------------------------------------------- */

static void test_submod_two_gates(void) {
    read_blif(FOUR_GATES);
    mark_driver("n2", "sub");
    mark_driver("n3", "sub");
    uint32_t before = count_cells(top(), (odin3_celltype_id){0});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(2, count_cells(sub, (odin3_celltype_id){0}));
    TEST_ASSERT_EQUAL_UINT32(4, port_count(sub));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "n1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "c"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "a"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "n3"));
    TEST_ASSERT_EQUAL_INT(-1, port_dir(sub, "n2"));
    TEST_ASSERT_EQUAL_UINT32(1, count_cells(top(), odin3_module_celltype(sub)));
    TEST_ASSERT_EQUAL_UINT32(before - 2 + 1, count_cells(top(), (odin3_celltype_id){0}));
    check_clean();
}

static void test_submod_ports_from_boundary(void) {
    read_blif(FOUR_GATES);
    mark_driver("n1", "g");
    mark_driver("y", "g");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("g");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(5, port_count(sub));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "a")); /* driven by a module input */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "b")); /* read by both gates, one port */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "n3")); /* driven outside the group */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "n1")); /* read outside */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "y")); /* read by the module output */
    check_clean();
}

static void test_submod_moves_prov_and_attrs(void) {
    read_blif(FOUR_GATES);
    odin3_node_id gate = driver_of(top(), "n2");
    odin3_prov_id prov = odin3_node_prov(top(), gate);
    odin3_value seven = odin3_value_int(7);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NODE, gate.v}, intern("tag"),
                                 &seven));
    mark_str(top(), gate, "submod", "sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    odin3_node_id moved = {0};
    for (uint32_t nid = 1; nid < odin3_module_node_end(sub); nid++) {
        if (odin3_node_live(sub, (odin3_node_id){nid}) && !is_port_node(sub, (odin3_node_id){nid})) {
            moved = (odin3_node_id){nid};
        }
    }
    TEST_ASSERT_TRUE(odin3_node_valid(moved));
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(sub, moved).v);
    odin3_objref ref = {ODIN3_OBJ_NODE, moved.v};
    const odin3_value *tag = odin3_attr_get(sub, ref, intern("tag"));
    TEST_ASSERT_NOT_NULL(tag);
    TEST_ASSERT_EQUAL_INT64(7, tag->i);
    TEST_ASSERT_NULL(odin3_attr_get(sub, ref, intern("submod")));
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_module_prov(sub));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    for (uint32_t nid = 1; nid < odin3_module_node_end(top()); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(top(), node) &&
            odin3_node_type(top(), node).v == odin3_module_celltype(sub).v) {
            rec = odin3_prov_get(design, odin3_node_prov(top(), node));
            TEST_ASSERT_NOT_NULL(rec);
            TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
            TEST_ASSERT_EQUAL_STRING("sub_i",
                                     odin3_strtab_get(odin3_design_strtab(design),
                                                      odin3_node_name(top(), node)));
        }
    }
    check_clean();
}

static void test_submod_name_clash_refused(void) {
    read_blif(FOUR_GATES);
    mark_driver("n2", "m");
    uint32_t nodes = odin3_module_node_end(top());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "already in use"));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top()));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    check_clean();
}

static void test_submod_port_node_refused(void) {
    read_blif(FOUR_GATES);
    mark_str(top(), odin3_module_port(top(), 0), "submod", "sub");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "port node"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    check_clean();
}

static void test_submod_empty_group_refused(void) {
    read_blif(FOUR_GATES);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_submod_extract(design, (odin3_module_id){1}, "submod", "none",
                                               NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "is empty"));
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("-name x"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "is empty"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text); /* no groups, no error */
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    check_clean();
}

/* A copy of node on the same nets, so every output net of node gets a second driver. */
static void clone_node(odin3_module *module, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(module, node);
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    odin3_value params[MAX_PINS];
    odin3_net_id nets[MAX_PINS];
    odin3_netvec ports[MAX_PINS];
    TEST_ASSERT_TRUE(def->n_params <= MAX_PINS && def->n_ports <= MAX_PINS);
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = *odin3_node_param(module, node, i);
    }
    uint32_t at = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice slice = odin3_node_port(module, node, port);
        ports[port] = (odin3_netvec){nets + at, slice.count};
        for (uint32_t bit = 0; bit < slice.count; bit++) {
            nets[at++] = odin3_pin_net(module, (odin3_pin_id){slice.first.v + bit});
        }
    }
    odin3_node_spec spec = {type, 0, {0}, params, def->n_params};
    odin3_node_id copy = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &copy));
}

static void test_submod_multi_driver_refused(void) {
    read_blif(FOUR_GATES);
    odin3_node_id gate = driver_of(top(), "n3");
    clone_node(top(), gate);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_driver_count(top(), net_named(top(), "n3")));
    mark_str(top(), gate, "submod", "sub");
    uint32_t nodes = odin3_module_node_end(top());
    /* odin3_pass_run's own check refuses a two-driver net first, so call the step directly. */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_submod_extract(design, (odin3_module_id){1}, "submod", "sub",
                                               NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "n3"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "drivers"));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top()));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_TRUE(odin3_node_live(top(), gate));
}

static void test_submod_two_groups_sorted_by_value(void) {
    read_blif(FOUR_GATES);
    mark_driver("n1", "zeta");
    mark_driver("y", "alpha");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_design_module_end(design));
    TEST_ASSERT_EQUAL_STRING(
        "alpha", odin3_strtab_get(odin3_design_strtab(design),
                                  odin3_module_name(odin3_module_get(design, (odin3_module_id){2}))));
    TEST_ASSERT_EQUAL_STRING(
        "zeta", odin3_strtab_get(odin3_design_strtab(design),
                                 odin3_module_name(odin3_module_get(design, (odin3_module_id){3}))));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text); /* the key is gone: no groups */
    TEST_ASSERT_EQUAL_UINT32(4, odin3_design_module_end(design));
    check_clean();
}

static void test_submod_options(void) {
    read_blif(FOUR_GATES);
    mark_str(top(), driver_of(top(), "n2"), "grp", "g1");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("-bogus"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-attr grp -name renamed"), log_text);
    TEST_ASSERT_NOT_NULL(module_named("renamed"));
    TEST_ASSERT_NULL(module_named("g1"));
    check_clean();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_submod_two_gates);
    RUN_TEST(test_submod_ports_from_boundary);
    RUN_TEST(test_submod_moves_prov_and_attrs);
    RUN_TEST(test_submod_name_clash_refused);
    RUN_TEST(test_submod_port_node_refused);
    RUN_TEST(test_submod_empty_group_refused);
    RUN_TEST(test_submod_multi_driver_refused);
    RUN_TEST(test_submod_two_groups_sorted_by_value);
    RUN_TEST(test_submod_options);
    return UNITY_END();
}
