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
#include "opclass_expect.h"
#include "passes/manager.h"
#include "passes/submod/submod.h"
#include "passes/submod/submod_int.h"
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
#define FOUR_GATES                                                                                 \
    ".model m\n.inputs a b c\n.outputs y\n"                                                        \
    ".names a b n1\n11 1\n.names n1 c n2\n11 1\n.names n2 a n3\n1- 1\n-1 1\n"                      \
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   intern(key), &value));
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
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "a"));   /* driven by a module input */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "b"));   /* read by both gates, one port */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "n3"));  /* driven outside the group */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "n1")); /* read outside */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "y"));  /* read by the module output */
    check_clean();
}

/* The one non-port node of module, none when there is not exactly one. */
static odin3_node_id only_cell(const odin3_module *module) {
    odin3_node_id found = {0};
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(module); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(module, node) && !is_port_node(module, node)) {
            found = node;
            count++;
        }
    }
    return count == 1 ? found : (odin3_node_id){0};
}

static void expect_derived(odin3_prov_id prov) {
    const odin3_prov_record *rec = odin3_prov_get(design, prov);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
}

/* The instance of sub in the top module: named sub_i, provenance DERIVED. */
static void expect_instance(const odin3_module *sub) {
    odin3_node_id inst = {0};
    for (uint32_t nid = 1; nid < odin3_module_node_end(top()); nid++) {
        if (odin3_node_live(top(), (odin3_node_id){nid}) &&
            odin3_node_type(top(), (odin3_node_id){nid}).v == odin3_module_celltype(sub).v) {
            inst = (odin3_node_id){nid};
        }
    }
    TEST_ASSERT_TRUE(odin3_node_valid(inst));
    expect_derived(odin3_node_prov(top(), inst));
    TEST_ASSERT_EQUAL_STRING(
        "sub_i", odin3_strtab_get(odin3_design_strtab(design), odin3_node_name(top(), inst)));
}

/* The tag attribute (7) came along; the group key did not. */
static void expect_attrs(const odin3_module *sub, odin3_node_id moved) {
    odin3_objref ref = {ODIN3_OBJ_NODE, moved.v};
    const odin3_value *tag = odin3_attr_get(sub, ref, intern("tag"));
    TEST_ASSERT_NOT_NULL(tag);
    TEST_ASSERT_EQUAL_INT64(7, tag->i);
    TEST_ASSERT_NULL(odin3_attr_get(sub, ref, intern("submod")));
}

static const odin3_opclass SUBMOD_CLASS = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                           false, 0};

static void test_submod_moves_prov_and_attrs(void) {
    read_blif(FOUR_GATES);
    odin3_node_id gate = driver_of(top(), "n2");
    odin3_prov_id prov = odin3_node_prov(top(), gate);
    odin3_value seven = odin3_value_int(7);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NODE, gate.v},
                                                   intern("tag"), &seven));
    mark_str(top(), gate, "submod", "sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    odin3_node_id moved = only_cell(sub);
    TEST_ASSERT_TRUE(odin3_node_valid(moved));
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(sub, moved).v);
    expect_attrs(sub, moved);
    expect_derived(odin3_module_prov(sub));
    expect_instance(sub);
    opx_all(design, "submod", &SUBMOD_CLASS);
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
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_submod_extract(design, (odin3_module_id){1},
                                                                      "submod", "none", NULL));
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
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_submod_extract(design, (odin3_module_id){1},
                                                                      "submod", "sub", NULL));
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
        "alpha",
        odin3_strtab_get(odin3_design_strtab(design),
                         odin3_module_name(odin3_module_get(design, (odin3_module_id){2}))));
    TEST_ASSERT_EQUAL_STRING(
        "zeta",
        odin3_strtab_get(odin3_design_strtab(design),
                         odin3_module_name(odin3_module_get(design, (odin3_module_id){3}))));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text); /* the key is gone: no groups */
    TEST_ASSERT_EQUAL_UINT32(4, odin3_design_module_end(design));
    check_clean();
}

static void test_submod_options(void) {
    read_blif(FOUR_GATES);
    mark_str(top(), driver_of(top(), "n2"), "grp", "g1");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("-bogus"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("-bus"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "-bus needs a value"), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-attr grp -name renamed"), log_text);
    TEST_ASSERT_NOT_NULL(module_named("renamed"));
    TEST_ASSERT_NULL(module_named("g1"));
    check_clean();
}

/* The width of the port called name of module, 0 when it has none. */
static uint32_t port_width(const odin3_module *module, const char *name) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(module));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return def->ports[i].width;
        }
    }
    return 0;
}

static void write_json_file(const char *text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.json", dir);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Reads a JSON design (top module `top`) from text. */
static void read_json(const char *text) {
    char args[PATH_BUF];
    write_json_file(text);
    (void)snprintf(args, sizeof args, "--top top %s/t.json", dir);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_json", odin3_bytes_cstr(args)), log_text);
}

/* top: a[4] in, y[4] out; g1 = ~a (w1 low bits of a) into the unnamed nets 10.., g2 = ~n into y. */
#define BUS_JSON(w1, a1, n1, mark1, mark2)                                                         \
    "{\"modules\": {\"top\": {\"ports\": {"                                                        \
    "\"a\": {\"direction\": \"input\", \"bits\": [2, 3, 4, 5]}, "                                  \
    "\"y\": {\"direction\": \"output\", \"bits\": [6, 7, 8, 9]}}, \"cells\": {"                    \
    "\"g1\": {\"type\": \"$not\", \"parameters\": {\"A_WIDTH\": " w1                               \
    ", \"A_SIGNED\": 0, \"Y_WIDTH\": " w1 "}, " mark1 "\"connections\": {\"A\": " a1               \
    ", \"Y\": " n1 "}}, "                                                                          \
    "\"g2\": {\"type\": \"$not\", \"parameters\": {\"A_WIDTH\": 4, \"A_SIGNED\": 0, "              \
    "\"Y_WIDTH\": 4}, " mark2 "\"connections\": {\"A\": [10, 11, 12, 13], "                        \
    "\"Y\": [6, 7, 8, 9]}}}}}}"

#define SUB_ATTR "\"attributes\": {\"submod\": \"sub\"}, "

static void test_submod_bus_in_and_out(void) {
    read_json(BUS_JSON("4", "[2, 3, 4, 5]", "[10, 11, 12, 13]", SUB_ATTR, ""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(5, port_count(sub)); /* the bus a and four scalar outputs */
    TEST_ASSERT_EQUAL_UINT32(4, port_width(sub, "a"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "a"));
    check_clean();
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    read_json(BUS_JSON("4", "[2, 3, 4, 5]", "[10, 11, 12, 13]", "", SUB_ATTR));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(5, port_count(sub)); /* four scalar inputs and the bus y */
    TEST_ASSERT_EQUAL_UINT32(4, port_width(sub, "y"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "y"));
    check_clean();
}

static void test_submod_bus_partial_stays_scalar(void) {
    read_json(BUS_JSON("2", "[2, 3]", "[10, 11]", SUB_ATTR, ""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(4, port_count(sub));
    TEST_ASSERT_EQUAL_UINT32(0, port_width(sub, "a"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "a[0]"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "a[1]"));
    check_clean();
}

/* A copy of node on its input nets whose outputs go to new nets, with provenance none. */
static odin3_node_id clone_fresh_noprov(odin3_module *module, odin3_node_id node) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
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
            odin3_pin_id pin = {slice.first.v + bit};
            if (odin3_pin_drives(module, pin)) {
                TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                      odin3_net_create(module, 0, (odin3_prov_id){0}, &nets[at]));
            } else {
                nets[at] = odin3_pin_net(module, pin);
            }
            at++;
        }
    }
    odin3_node_spec spec = {odin3_node_type(module, node), 0, {0}, params, def->n_params};
    odin3_node_id copy = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &copy));
    return copy;
}

static void test_submod_derived_prov_without_source_prov(void) {
    read_blif(FOUR_GATES);
    odin3_node_id copy = clone_fresh_noprov(top(), driver_of(top(), "n1"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_prov(top(), copy).v);
    mark_str(top(), copy, "submod", "sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    expect_derived(odin3_module_prov(sub));
    expect_instance(sub);
    check_clean();
}

static void test_submod_result_lists_instance_and_ports(void) {
    read_blif(FOUR_GATES);
    mark_driver("n2", "sub");
    mark_driver("n3", "sub");
    odin3_submod_result res;
    memset(&res, 0, sizeof res);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_submod_extract(design, (odin3_module_id){1}, "submod", "sub", &res),
        log_text);
    odin3_module *sub = odin3_module_get(design, res.module);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(4, res.n_ports);
    TEST_ASSERT_TRUE(odin3_node_live(top(), res.instance));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_celltype(sub).v, odin3_node_type(top(), res.instance).v);
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(sub));
    TEST_ASSERT_EQUAL_UINT32(res.n_ports, def->n_ports);
    for (uint32_t i = 0; i < res.n_ports; i++) {
        const odin3_submod_port *port = &res.ports[i];
        TEST_ASSERT_EQUAL_STRING(def->ports[i].name,
                                 odin3_strtab_get(odin3_design_strtab(design), port->name));
        TEST_ASSERT_EQUAL_INT((int)def->ports[i].dir, (int)port->dir);
        TEST_ASSERT_EQUAL_UINT32(def->ports[i].width, port->width);
        odin3_pinslice slice = odin3_node_port(top(), res.instance, i);
        TEST_ASSERT_EQUAL_UINT32(port->width, slice.count);
        for (uint32_t bit = 0; bit < port->width; bit++) {
            TEST_ASSERT_EQUAL_UINT32(port->nets[bit].v,
                                     odin3_pin_net(top(), (odin3_pin_id){slice.first.v + bit}).v);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(net_named(top(), "n3").v, res.ports[3].nets[0].v); /* the output */
    odin3_submod_result_free(&res);
    odin3_submod_result_free(&res);
    check_clean();
}

/* A one-bit wire called name on net (BLIF nets have no wires). */
static odin3_wire_id add_wire(const char *name, odin3_net_id net) {
    odin3_wire_spec spec = {intern(name), 0, 0, false, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(top(), &spec, &net, &wire));
    return wire;
}

static void test_submod_wire_attrs_reach_port_wires(void) {
    read_blif(FOUR_GATES);
    odin3_wire_id wire = add_wire("w1", net_named(top(), "n1"));
    odin3_value val = odin3_value_int(5);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                                                   intern("src"), &val));
    mark_driver("n2", "sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    odin3_wire_id port = odin3_module_find_wire(sub, intern("n1"));
    TEST_ASSERT_TRUE(odin3_wire_valid(port));
    const odin3_value *got =
        odin3_attr_get(sub, (odin3_objref){ODIN3_OBJ_WIRE, port.v}, intern("src"));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT64(5, got->i);
    check_clean();
}

static void test_submod_port_name_does_not_shadow_internal_net(void) {
    read_blif(FOUR_GATES);
    /* n1 loses its net name (its wire stays: the port is named n1); the internal net n2 takes it */
    odin3_net_id first = net_named(top(), "n1");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top(), first, 0));
    (void)add_wire("n1", first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(top(), net_named(top(), "n2"), intern("n1")));
    mark_driver("n1", "sub"); /* the gate of the old n2, now named n1 */
    mark_driver("n3", "sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_TRUE(odin3_wire_valid(odin3_module_find_wire(sub, intern("n1"))));
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_module_find_net(sub, intern("n1"))));
    check_clean();
}

static void test_submod_refusal_leaves_everything_unchanged(void) {
    read_blif(FOUR_GATES);
    mark_driver("n1", "alpha");
    mark_str(top(), odin3_module_port(top(), 0), "submod", "zeta"); /* a port node: refused */
    uint32_t nodes = odin3_module_node_end(top());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(""));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top()));
    TEST_ASSERT_TRUE(odin3_node_live(top(), driver_of(top(), "n1")));
    check_clean();
}

static void test_submod_same_name_in_two_modules_refused_up_front(void) {
    read_blif(".model top\n.inputs a b\n.outputs y z\n.names a b y\n11 1\n"
              ".subckt leaf a=a b=b y=z\n.end\n"
              ".model leaf\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n");
    uint32_t end = odin3_design_module_end(design);
    TEST_ASSERT_EQUAL_UINT32(3, end);
    for (uint32_t mid = 1; mid < end; mid++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        mark_str(module, driver_of(module, "y"), "submod", "same");
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "already in use"));
    TEST_ASSERT_EQUAL_UINT32(end, odin3_design_module_end(design));
    check_clean();
}

/* --- v1.0.5 plan Task 5: the NAME[BIT] attribute table --------------------------------------- */

#define ANDS4                                                                                      \
    ".model m\n.inputs a0 a1 a2 a3 b0 b1 b2 b3\n.outputs y0 y1 y2 y3\n"                            \
    ".names a0 b0 y0\n11 1\n.names a1 b1 y1\n11 1\n.names a2 b2 y2\n11 1\n"                        \
    ".names a3 b3 y3\n11 1\n.end\n"

enum { BUS_BITS = 4 };

static void test_submod_busattr_parse(void) {
    size_t len = 0;
    uint32_t bit = 0;
    TEST_ASSERT_TRUE(odin3_submod_bus_parse("h12[3]", &len, &bit));
    TEST_ASSERT_EQUAL_size_t(3, len);
    TEST_ASSERT_EQUAL_UINT32(3, bit);
    TEST_ASSERT_TRUE(odin3_submod_bus_parse("a[0]", &len, &bit));
    TEST_ASSERT_TRUE(odin3_submod_bus_parse("a[2147483647]", &len, &bit));
}

static void test_submod_busattr_parse_refused(void) {
    size_t len = 0;
    uint32_t bit = 0;
    const char *bad[] = {"h12",   "h12[]",         "[3]",     "$x[1]", "h[3]x",
                         "h[-1]", "h[2147483648]", "a[b][1]", "h[3",   "h[3]]"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(odin3_submod_bus_parse(bad[i], &len, &bit), bad[i]);
    }
}

/* Net `net` gets attribute loki_bus = text. */
typedef struct net_tag {
    const char *net;
    const char *text;
} net_tag;

static void tag_net(net_tag tag) {
    odin3_net_id id = net_named(top(), tag.net);
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(tag.text), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NET, id.v},
                                                   intern("loki_bus"), &value));
}

/* The four ANDs, the state of their nets by hand and y<i> tagged `y4[<i>]`. */
static void load_ands(odin3_vec *state) {
    read_blif(ANDS4);
    odin3_vec_init(state, sizeof(uint8_t));
    for (uint32_t nid = 0; nid < odin3_module_net_end(top()); nid++) {
        uint8_t *slot = odin3_vec_push(state);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = SM_NONE;
    }
    for (int i = 0; i < BUS_BITS; i++) {
        char name[PATH_BUF];
        char text[PATH_BUF];
        (void)snprintf(name, sizeof name, "y%d", i);
        *(uint8_t *)odin3_vec_at(state, net_named(top(), name).v) = SM_OUT;
        (void)snprintf(text, sizeof text, "y4[%d]", i);
        tag_net((net_tag){name, text});
        (void)snprintf(name, sizeof name, "a%d", i);
        *(uint8_t *)odin3_vec_at(state, net_named(top(), name).v) = SM_IN;
        (void)snprintf(name, sizeof name, "b%d", i);
        *(uint8_t *)odin3_vec_at(state, net_named(top(), name).v) = SM_IN;
    }
}

static void test_submod_busset_groups(void) {
    odin3_vec state;
    odin3_submod_busset set;
    load_ands(&state);
    odin3_submod_busset_init(&set);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_submod_busset_build(top(), &state, "loki_bus", "sub", &set), log_text);
    TEST_ASSERT_EQUAL_size_t(1, set.buses.len);
    const odin3_submod_bus *bus = set.buses.data;
    TEST_ASSERT_TRUE(bus->ok);
    TEST_ASSERT_EQUAL_UINT32(BUS_BITS, bus->width);
    TEST_ASSERT_EQUAL_UINT8(SM_OUT, bus->dir);
    const odin3_net_id *nets = set.nets.data;
    for (uint32_t i = 0; i < BUS_BITS; i++) {
        char name[PATH_BUF];
        (void)snprintf(name, sizeof name, "y%u", (unsigned)i);
        TEST_ASSERT_EQUAL_UINT32(net_named(top(), name).v, nets[bus->first + i].v);
        TEST_ASSERT_EQUAL_UINT32(1, *(uint32_t *)odin3_vec_at(&set.of_net, nets[bus->first + i].v));
    }
    TEST_ASSERT_EQUAL_UINT32(0, *(uint32_t *)odin3_vec_at(&set.of_net, net_named(top(), "a0").v));
    odin3_submod_busset_free(&set);
    odin3_vec_free(&state);
}

/* Two bits of the four inputs per bus: P, Q (from a), R, S (from b), all the same direction and
 * interned one after the other, so consecutive name IDs (one pair is 2m, 2m+1). */
static const char *const MULTI_TAGS[] = {"a0", "busP[0]", "a1", "busP[1]", "a2", "busQ[0]",
                                         "a3", "busQ[1]", "b0", "busR[0]", "b1", "busR[1]",
                                         "b2", "busS[0]", "b3", "busS[1]"};
enum { MULTI_PER_BUS = 4 };

static void retag_multi(void) {
    for (uint32_t i = 0; i < BUS_BITS; i++) {
        char name[PATH_BUF];
        (void)snprintf(name, sizeof name, "y%u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_attr_remove(top(), (odin3_objref){ODIN3_OBJ_NET, net_named(top(), name).v},
                              intern("loki_bus")));
    }
    for (size_t i = 0; i < sizeof MULTI_TAGS / sizeof MULTI_TAGS[0]; i += 2) {
        tag_net((net_tag){MULTI_TAGS[i], MULTI_TAGS[i + 1]});
    }
}

static void expect_multi_bus(const odin3_submod_busset *set, size_t idx) {
    const odin3_submod_bus *bus = (const odin3_submod_bus *)set->buses.data + idx;
    const odin3_net_id *nets = set->nets.data;
    TEST_ASSERT_TRUE(bus->ok);
    TEST_ASSERT_EQUAL_UINT32(2, bus->width);
    TEST_ASSERT_EQUAL_UINT8(SM_IN, bus->dir);
    TEST_ASSERT_TRUE(idx == 0 || bus[-1].name < bus->name);
    for (size_t k = 0; k < 2; k++) {
        const char *want = MULTI_TAGS[(idx * MULTI_PER_BUS) + (k * 2)];
        TEST_ASSERT_EQUAL_UINT32(net_named(top(), want).v, nets[bus->first + k].v);
    }
}

static void test_submod_busset_multi_bus(void) {
    odin3_vec state;
    odin3_submod_busset set;
    load_ands(&state);
    retag_multi();
    odin3_submod_busset_init(&set);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_submod_busset_build(top(), &state, "loki_bus", "sub", &set), log_text);
    TEST_ASSERT_EQUAL_size_t(BUS_BITS, set.buses.len);
    for (size_t i = 0; i < set.buses.len; i++) {
        expect_multi_bus(&set, i);
    }
    odin3_submod_busset_free(&set);
    odin3_vec_free(&state);
}

/* A net attribute wins over the attribute of its one-bit wire. */
static void test_submod_busset_net_beats_wire(void) {
    odin3_vec state;
    odin3_submod_busset set;
    load_ands(&state);
    odin3_net_id y0 = net_named(top(), "y0");
    odin3_wire_id wire = odin3_net_primary(top(), y0).wire;
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_wire_width(top(), wire));
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern("other[0]"), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                                                   intern("loki_bus"), &value));
    odin3_submod_busset_init(&set);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_submod_busset_build(top(), &state, "loki_bus", "sub", &set), log_text);
    TEST_ASSERT_EQUAL_size_t(1, set.buses.len);
    TEST_ASSERT_TRUE(((const odin3_submod_bus *)set.buses.data)->ok);
    TEST_ASSERT_EQUAL_UINT32(BUS_BITS, ((const odin3_submod_bus *)set.buses.data)->width);
    odin3_submod_busset_free(&set);
    odin3_vec_free(&state);
}

/* The one bus of the table with y2 untagged (or retagged): not ok, the warning names the cause. */
static void expect_fallback(bool untag) {
    odin3_vec state;
    odin3_submod_busset set;
    load_ands(&state);
    const char *why = untag ? "has no bit 2" : "bit 1 twice";
    if (untag) {
        odin3_net_id y2 = net_named(top(), "y2");
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_attr_remove(top(), (odin3_objref){ODIN3_OBJ_NET, y2.v}, intern("loki_bus")));
    } else {
        tag_net((net_tag){"y2", "y4[1]"});
    }
    odin3_submod_busset_init(&set);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_submod_busset_build(top(), &state, "loki_bus", "sub", &set), log_text);
    TEST_ASSERT_EQUAL_size_t(1, set.buses.len);
    TEST_ASSERT_FALSE(((const odin3_submod_bus *)set.buses.data)->ok);
    TEST_ASSERT_EQUAL_UINT32(0, *(uint32_t *)odin3_vec_at(&set.of_net, net_named(top(), "y0").v));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, why), log_text);
    odin3_submod_busset_free(&set);
    odin3_vec_free(&state);
}

static void test_submod_busset_gap(void) {
    expect_fallback(true);
}

static void test_submod_busset_duplicate(void) {
    expect_fallback(false);
}

static void test_submod_busset_malformed(void) {
    odin3_vec state;
    odin3_submod_busset set;
    load_ands(&state);
    tag_net((net_tag){"y0", "y4"});
    odin3_submod_busset_init(&set);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_submod_busset_build(top(), &state, "loki_bus", "sub", &set));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "is not NAME[BIT]"), log_text);
    odin3_submod_busset_free(&set);
    odin3_vec_free(&state);
}

/* --- v1.0.5 plan Task 6: submod -bus BUSKEY --------------------------------------------------- */

/* The four ANDs with every driver of y<i> in group `sub`, y<i> tagged `y4[<i>]` and, with
 * tag_a, a<i> tagged `a4[<i>]`. */
static void load_ands_group(bool tag_a) {
    read_blif(ANDS4);
    for (int i = 0; i < BUS_BITS; i++) {
        char name[PATH_BUF];
        char text[PATH_BUF];
        (void)snprintf(name, sizeof name, "y%d", i);
        mark_driver(name, "sub");
        (void)snprintf(text, sizeof text, "y4[%d]", i);
        tag_net((net_tag){name, text});
        if (tag_a) {
            (void)snprintf(name, sizeof name, "a%d", i);
            (void)snprintf(text, sizeof text, "a4[%d]", i);
            tag_net((net_tag){name, text});
        }
    }
}

/* The live instance of `sub` in the top module. */
static odin3_node_id instance_of(const odin3_module *sub) {
    for (uint32_t nid = 1; nid < odin3_module_node_end(top()); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(top(), node) &&
            odin3_node_type(top(), node).v == odin3_module_celltype(sub).v) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE("no instance");
    return (odin3_node_id){0};
}

/* The net on bit `bit` of port `port` of the one instance of `sub` in the top module. */
static odin3_net_id instance_net(const odin3_module *sub, const char *port, uint32_t bit) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(sub));
    odin3_node_id node = instance_of(sub);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, port) == 0) {
            odin3_pinslice slice = odin3_node_port(top(), node, i);
            TEST_ASSERT_TRUE(bit < slice.count);
            return odin3_pin_net(top(), (odin3_pin_id){slice.first.v + bit});
        }
    }
    TEST_FAIL_MESSAGE(port);
    return (odin3_net_id){0};
}

static void test_submod_busattr_vector_in_and_out(void) {
    load_ands_group(true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-bus loki_bus"), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(6, port_count(sub));
    TEST_ASSERT_EQUAL_UINT32(BUS_BITS, port_width(sub, "a4"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, port_dir(sub, "a4"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "b0"));
    TEST_ASSERT_EQUAL_UINT32(BUS_BITS, port_width(sub, "y4"));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, port_dir(sub, "y4"));
    for (uint32_t i = 0; i < BUS_BITS; i++) {
        char name[PATH_BUF];
        (void)snprintf(name, sizeof name, "y%u", (unsigned)i);
        TEST_ASSERT_EQUAL_UINT32(net_named(top(), name).v, instance_net(sub, "y4", i).v);
        (void)snprintf(name, sizeof name, "a%u", (unsigned)i);
        TEST_ASSERT_EQUAL_UINT32(net_named(top(), name).v, instance_net(sub, "a4", i).v);
    }
    check_clean();
}

static void test_submod_busattr_off_by_default(void) {
    load_ands_group(true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(""), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(12, port_count(sub));
    TEST_ASSERT_EQUAL_UINT32(0, port_width(sub, "y4"));
    check_clean();
}

static void test_submod_busattr_gap_stays_scalar(void) {
    load_ands_group(false);
    odin3_net_id y2 = net_named(top(), "y2");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_remove(top(), (odin3_objref){ODIN3_OBJ_NET, y2.v},
                                                      intern("loki_bus")));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-bus loki_bus"), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(0, port_width(sub, "y4"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "y0"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "y1"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "y3"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "bus 'y4' has no bit 2"), log_text);
    check_clean();
}

static void test_submod_busattr_mixed_dir_stays_scalar(void) {
    load_ands_group(false);
    tag_net((net_tag){"a0", "y4[4]"}); /* an input with the output bus's name */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-bus loki_bus"), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(12, port_count(sub));
    TEST_ASSERT_EQUAL_UINT32(0, port_width(sub, "y4"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "bus 'y4' is on both"), log_text);
    check_clean();
}

static void test_submod_busattr_malformed_refused(void) {
    load_ands_group(false);
    tag_net((net_tag){"y0", "y4"});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("-bus loki_bus"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "is not NAME[BIT]"), log_text);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_NULL(module_named("sub"));
    check_clean();
}

static void test_submod_busattr_beats_wire_bus(void) {
    read_json(BUS_JSON("4", "[2, 3, 4, 5]", "[10, 11, 12, 13]", SUB_ATTR, ""));
    odin3_wire_id wire = odin3_module_find_wire(top(), intern("a"));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    for (uint32_t bit = 0; bit < BUS_BITS; bit++) {
        char text[PATH_BUF];
        (void)snprintf(text, sizeof text, "q[%u]", (unsigned)(3U - bit)); /* reversed */
        odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(text), 0};
        odin3_net_id net = odin3_wire_net(top(), wire, bit);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NET, net.v},
                                                       intern("loki_bus"), &value));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-bus loki_bus"), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(BUS_BITS, port_width(sub, "q"));
    TEST_ASSERT_EQUAL_UINT32(0, port_width(sub, "a"));
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(top(), wire, 3).v, instance_net(sub, "q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(top(), wire, 0).v, instance_net(sub, "q", 3).v);
    check_clean();
}

static void test_submod_busattr_name_unique(void) {
    read_blif(".model m\n.inputs a0 a1 c0 c1 y4\n.outputs z\n.names a0 a1 n1\n11 1\n"
              ".names n1 c0 c1 y4 z\n1111 1\n.end\n");
    mark_driver("n1", "sub");
    mark_driver("z", "sub");
    tag_net((net_tag){"a0", "y4[0]"});
    tag_net((net_tag){"a1", "y4[1]"});
    tag_net((net_tag){"c0", "n1[0]"});
    tag_net((net_tag){"c1", "n1[1]"});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("-bus loki_bus"), log_text);
    odin3_module *sub = module_named("sub");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(2, port_width(sub, "y4"));
    TEST_ASSERT_EQUAL_UINT32(1, port_width(sub, "y4_1")); /* the scalar net named y4 */
    TEST_ASSERT_EQUAL_UINT32(2, port_width(sub, "n1"));
    bool renamed = false;
    for (uint32_t nid = 1; nid < odin3_module_net_end(sub); nid++) {
        uint32_t name = odin3_net_name(sub, (odin3_net_id){nid});
        renamed =
            renamed || (name != 0 && strncmp(odin3_strtab_get(odin3_design_strtab(design), name),
                                             "$submod$", strlen("$submod$")) == 0);
    }
    TEST_ASSERT_TRUE(renamed);
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
    RUN_TEST(test_submod_bus_in_and_out);
    RUN_TEST(test_submod_bus_partial_stays_scalar);
    RUN_TEST(test_submod_derived_prov_without_source_prov);
    RUN_TEST(test_submod_result_lists_instance_and_ports);
    RUN_TEST(test_submod_wire_attrs_reach_port_wires);
    RUN_TEST(test_submod_port_name_does_not_shadow_internal_net);
    RUN_TEST(test_submod_refusal_leaves_everything_unchanged);
    RUN_TEST(test_submod_same_name_in_two_modules_refused_up_front);
    RUN_TEST(test_submod_busattr_parse);
    RUN_TEST(test_submod_busattr_parse_refused);
    RUN_TEST(test_submod_busset_groups);
    RUN_TEST(test_submod_busset_multi_bus);
    RUN_TEST(test_submod_busset_net_beats_wire);
    RUN_TEST(test_submod_busset_gap);
    RUN_TEST(test_submod_busset_duplicate);
    RUN_TEST(test_submod_busset_malformed);
    RUN_TEST(test_submod_busattr_vector_in_and_out);
    RUN_TEST(test_submod_busattr_off_by_default);
    RUN_TEST(test_submod_busattr_gap_stays_scalar);
    RUN_TEST(test_submod_busattr_mixed_dir_stays_scalar);
    RUN_TEST(test_submod_busattr_malformed_refused);
    RUN_TEST(test_submod_busattr_beats_wire_bus);
    RUN_TEST(test_submod_busattr_name_unique);
    return UNITY_END();
}
