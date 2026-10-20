/*
 * test_levelize.c — the levelize pass (Phase 6 spec 6.4, V-1 to V-8): hand counts, the Q4 rule for
 * memories and black boxes, loop cuts, rerun identity, the --key guard, word-view refusal and the
 * 100000-cell chain bound. Designs are small BLIF files in a mkdtemp directory under the build
 * tree. V-7 (d), the JSON round trip of the levels, needs write_json --odin3 (lane 6.3) and runs
 * after both lanes merge; here the plain attribute is checked in the write_json text.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifndef ODIN3_LV_ROOT
#error "ODIN3_LV_ROOT must name the build tree"
#endif
#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum {
    PATH_BUF = 512,
    DIR_BUF = 256,
    LOG_MAX = 16384,
    CHAIN_LEN = 100000,
    CHAIN_MAX_SECONDS = 10,
    CHAIN_MAX_KB = 256 * 1024,
    TEXT_MAX = 64,
    JSON_MAX = 65536
};

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

static void path_of(const char *name, char *out) {
    (void)snprintf(out, PATH_BUF, "%s/%s", dir, name);
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/lv.XXXXXX", ODIN3_LV_ROOT);
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
            path_of(entry->d_name, path);
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

static bool log_has(const char *text) {
    return strstr(log_text, text) != NULL;
}

static odin3_status run(const char *pass, const char *args) {
    return odin3_pass_run(design, pass, odin3_bytes_cstr(args));
}

/* Reads a BLIF text into the fresh design. */
static void read_blif(const char *text) {
    char path[PATH_BUF];
    path_of("t.blif", path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_blif", path), log_text);
}

static void read_vtr_lib(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_techlib", ODIN3_LIB_DIR "/vtr.o3lib"),
                                  log_text);
}

static void read_gates_lib(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, run("read_techlib", ODIN3_LIB_DIR "/generic_gates.o3lib"), log_text);
}

static odin3_module *top(void) {
    return odin3_module_get(design, (odin3_module_id){1});
}

static bool key_id(const char *key, uint32_t *id) {
    return odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(key), id);
}

/* The INT attribute `key` of an object, -1 when it has none. */
static int64_t attr_int(const odin3_module *module, odin3_objref obj, const char *key) {
    uint32_t id = 0;
    if (!key_id(key, &id)) {
        return -1;
    }
    const odin3_value *val = odin3_attr_get(module, obj, id);
    return val != NULL && val->kind == ODIN3_VAL_INT ? val->i : -1;
}

static odin3_net_id net_named(const odin3_module *module, const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(key_id(name, &id), name);
    odin3_net_id net = odin3_module_find_net(module, id);
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), name);
    return net;
}

static odin3_objref net_ref(const odin3_module *module, const char *name) {
    return (odin3_objref){ODIN3_OBJ_NET, net_named(module, name).v};
}

static int64_t net_lvl(const char *name) {
    return attr_int(top(), net_ref(top(), name), "odin3_level");
}

/* The node driving the net called name. */
static odin3_node_id driver_of(const odin3_module *module, const char *name) {
    odin3_pin_id pin = odin3_net_driver(module, net_named(module, name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_pin_valid(pin), name);
    return odin3_pin_node(module, pin);
}

static int64_t cell_attr(const char *net_name, bool cut) {
    odin3_node_id node = driver_of(top(), net_name);
    return attr_int(top(), (odin3_objref){ODIN3_OBJ_NODE, node.v},
                    cut ? "odin3_level_cut" : "odin3_level");
}

static int64_t cell_lvl(const char *net_name) {
    return cell_attr(net_name, false);
}

static int64_t cell_cut(const char *net_name) {
    return cell_attr(net_name, true);
}

static void levelize_ok(const char *args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("levelize", args), log_text);
}

/* --- V-7 (a): hand counts ------------------------------------------------------------------- */

static void test_chain_of_three(void) {
    read_blif(".model m\n.inputs a\n.outputs n3\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n.names n2 n3\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("a"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("n1"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n2"));
    TEST_ASSERT_EQUAL_INT64(3, net_lvl("n3"));
    TEST_ASSERT_EQUAL_INT64(1, cell_lvl("n1"));
    TEST_ASSERT_EQUAL_INT64(3, cell_lvl("n3"));
    TEST_ASSERT_EQUAL_INT64(-1, cell_cut("n3"));
    TEST_ASSERT_TRUE(log_has("levelize: 1 modules, 3 cells, max level 3, 0 loops cut"));
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

static void test_reconvergent_takes_max(void) {
    read_blif(".model m\n.inputs a b\n.outputs y\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n.names n2 n3\n1 1\n"
              ".names a n3 y\n11 1\n.names b z\n1 1\n.names z y2\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(4, net_lvl("y"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("z"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("y2"));
}

static void test_register_resets_level(void) {
    read_blif(".model m\n.inputs a clk\n.outputs n4\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n.latch n2 q re clk 0\n"
              ".names q n3\n1 1\n.names n3 n4\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n2"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("q"));
    TEST_ASSERT_EQUAL_INT64(0, cell_lvl("q"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("n3"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n4"));
}

static void test_constant_and_deeper_cone(void) {
    read_blif(".model m\n.inputs a\n.outputs y w\n"
              ".names k1\n1\n.names k1 a y\n11 1\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n.names a n2 w\n11 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("k1"));
    TEST_ASSERT_EQUAL_INT64(0, cell_lvl("k1"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("y"));
    TEST_ASSERT_EQUAL_INT64(3, net_lvl("w"));
}

static size_t count_of(const char *needle) {
    size_t count = 0;
    for (const char *at = strstr(log_text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* --- V-7 (b): loops ------------------------------------------------------------------------- */

static void expect_one_located_warning(void) {
    TEST_ASSERT_TRUE(log_has("t.blif:"));
    TEST_ASSERT_EQUAL_size_t(1, count_of("combinational loop through"));
}

static void test_loop_of_three_warns_once(void) {
    read_blif(".model m\n.inputs a\n.outputs d\n"
              ".names a x3 x1\n11 1\n.names x1 x2\n1 1\n.names x2 x3\n1 1\n"
              ".names x2 d\n1 1\n.end\n");
    odin3_node_id first = driver_of(top(), "x1");
    char want[TEXT_MAX * 2];
    (void)snprintf(want, sizeof want,
                   "levelize: module 'm': combinational loop through $sop '$c%u' (",
                   (unsigned)first.v);
    levelize_ok("");
    TEST_ASSERT_TRUE_MESSAGE(log_has(want), log_text);
    expect_one_located_warning();
    TEST_ASSERT_EQUAL_INT64(1, cell_cut("x1"));
    TEST_ASSERT_EQUAL_INT64(0, cell_lvl("x1"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("x1"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("x2"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("x3"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("d"));
    TEST_ASSERT_EQUAL_INT64(-1, cell_cut("x2"));
    TEST_ASSERT_TRUE(log_has("levelize: 1 modules, 4 cells, max level 2, 1 loops cut"));
}

/* A cell that is only downstream of a loop is not on it, even with a lower ID. */
static void test_cut_is_on_the_cycle_not_downstream(void) {
    read_blif(".model m\n.inputs a\n.outputs d\n"
              ".names x2 d\n1 1\n"
              ".names a x3 x1\n11 1\n.names x1 x2\n1 1\n.names x2 x3\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(-1, cell_cut("d"));
    TEST_ASSERT_EQUAL_INT64(1, cell_cut("x1"));
}

static void test_self_loop(void) {
    read_blif(".model m\n.inputs a\n.outputs x\n.names a x x\n11 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_TRUE(log_has("combinational loop through $sop"));
    TEST_ASSERT_EQUAL_INT64(1, cell_cut("x"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("x"));
}

static void test_two_disjoint_loops_warn_twice(void) {
    read_blif(".model m\n.inputs a b\n.outputs p q\n"
              ".names a q1 p1\n11 1\n.names p1 q1\n1 1\n.names p1 p\n1 1\n"
              ".names b r1 s1\n11 1\n.names s1 r1\n1 1\n.names s1 q\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_size_t(2, count_of("combinational loop through"));
    TEST_ASSERT_TRUE(log_has("2 loops cut"));
}

/* --- V-7 (c): rerun ------------------------------------------------------------------------- */

static odin3_status snap_visit(void *ctx, uint32_t key, const odin3_value *val) {
    odin3_strbuf *out = ctx;
    return odin3_strbuf_appendf(out, " %u=%lld", (unsigned)key, (long long)val->i);
}

/* Every attribute of every live cell and net of the design, in ID order. */
static void attr_snapshot(odin3_strbuf *out) {
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
            (void)odin3_strbuf_appendf(out, "\nn%u", (unsigned)id);
            (void)odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, id}, snap_visit, out);
        }
        for (uint32_t id = 1; id < odin3_module_net_end(module); id++) {
            (void)odin3_strbuf_appendf(out, "\nw%u", (unsigned)id);
            (void)odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NET, id}, snap_visit, out);
        }
    }
}

/* Every node (type, name, provenance, pin nets) and net (name, provenance, pins), no attributes. */
static void structure_snapshot(odin3_strbuf *out) {
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
            odin3_node_id node = {id};
            (void)odin3_strbuf_appendf(
                out, "\nn%u t%u s%u p%u:", (unsigned)id, (unsigned)odin3_node_type(module, node).v,
                (unsigned)odin3_node_name(module, node), (unsigned)odin3_node_prov(module, node).v);
            odin3_pinslice pins = odin3_node_pins(module, node);
            for (uint32_t i = 0; i < pins.count; i++) {
                odin3_pin_id pin = {pins.first.v + i};
                (void)odin3_strbuf_appendf(out, " %u", (unsigned)odin3_pin_net(module, pin).v);
            }
        }
        for (uint32_t id = 1; id < odin3_module_net_end(module); id++) {
            odin3_net_id net = {id};
            (void)odin3_strbuf_appendf(out, "\nw%u s%u p%u", (unsigned)id,
                                       (unsigned)odin3_net_name(module, net),
                                       (unsigned)odin3_net_prov(module, net).v);
        }
    }
}

static void test_rerun_identical_and_structure_untouched(void) {
    read_blif(".model m\n.inputs a clk\n.outputs d q\n"
              ".names a x3 x1\n11 1\n.names x1 x2\n1 1\n.names x2 x3\n1 1\n"
              ".names x2 d\n1 1\n.latch d q re clk 0\n.end\n");
    odin3_strbuf before;
    odin3_strbuf mid;
    odin3_strbuf once;
    odin3_strbuf twice;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&mid);
    odin3_strbuf_init(&once);
    odin3_strbuf_init(&twice);
    odin3_strbuf_init(&after);
    structure_snapshot(&before);
    levelize_ok("");
    attr_snapshot(&once);
    structure_snapshot(&mid);
    levelize_ok("");
    attr_snapshot(&twice);
    structure_snapshot(&after);
    TEST_ASSERT_TRUE(once.len > 0);
    TEST_ASSERT_EQUAL_STRING(once.data, twice.data);
    TEST_ASSERT_EQUAL_STRING(before.data, mid.data);
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&mid);
    odin3_strbuf_free(&once);
    odin3_strbuf_free(&twice);
    odin3_strbuf_free(&after);
}

/* --- V-7 (e): word view and processes refused ------------------------------------------------ */

static void test_word_cell_refused(void) {
    odin3_module_id mid = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("m"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, name, (odin3_prov_id){0}, &mid));
    odin3_module *module = odin3_module_get(design, mid);
    odin3_celltype_id type = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("$dff"), &name));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, name, &type));
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", ""));
    TEST_ASSERT_TRUE_MESSAGE(
        log_has("levelize: module 'm' has the word-level cell '$dff'; run proc and lower first"),
        log_text);
    TEST_ASSERT_EQUAL_INT64(
        -1, attr_int(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, "odin3_level"));
}

static void test_process_refused(void) {
    (void)proc_fixture_design(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", ""));
    TEST_ASSERT_TRUE_MESSAGE(log_has("has processes; run proc first"), log_text);
}

/* --- V-7 (f): the key guard ----------------------------------------------------------------- */

static void test_reserved_key_refused(void) {
    read_blif(".model m\n.inputs a\n.outputs n1\n.names a n1\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", "--key odin3_x"));
    TEST_ASSERT_TRUE(log_has("--key"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", "--key"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", "--bogus"));
    TEST_ASSERT_EQUAL_INT64(-1, net_lvl("n1"));
    levelize_ok("--key odin3_level");
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("n1"));
}

static void test_custom_key_and_cut_key(void) {
    read_blif(".model m\n.inputs a\n.outputs x\n.names a x x\n11 1\n.end\n");
    levelize_ok("--key depth");
    TEST_ASSERT_EQUAL_INT64(0, attr_int(top(), net_ref(top(), "x"), "depth"));
    TEST_ASSERT_EQUAL_INT64(
        1, attr_int(top(), (odin3_objref){ODIN3_OBJ_NODE, driver_of(top(), "x").v}, "depth_cut"));
    TEST_ASSERT_EQUAL_INT64(-1, net_lvl("x"));
    levelize_ok("--key depth");
}

static void test_user_attribute_not_overwritten(void) {
    read_blif(".model m\n.inputs a\n.outputs n1\n.names a n1\n1 1\n.end\n");
    uint32_t key = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("mine"), &key));
    odin3_net_id net = net_named(top(), "n1");
    odin3_value text = {.kind = ODIN3_VAL_STRING, .str = key};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NET, net.v}, key, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", "--key mine"));
    TEST_ASSERT_TRUE(log_has("levelize:"));
    TEST_ASSERT_EQUAL_INT64(-1, attr_int(top(), net_ref(top(), "n1"), "mine"));
    TEST_ASSERT_EQUAL_INT64(-1, net_lvl("a"));
    /* the default key on a cell, user-written as a string */
    uint32_t lvl = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(design, odin3_bytes_cstr("odin3_level"), &lvl));
    odin3_node_id node = driver_of(top(), "n1");
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_attr_set(top(), (odin3_objref){ODIN3_OBJ_NODE, node.v}, lvl, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("levelize", ""));
}

/* --- V-7 (g): sequential cells start paths --------------------------------------------------- */

static void test_flops_and_latches_are_sources(void) {
    read_blif(".model m\n.inputs a clk\n.outputs y\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n"
              ".latch n2 p re clk 0\n.latch n2 n fe clk 0\n.latch n2 l ah clk 0\n"
              ".latch n2 f\n"
              ".names p n l f y\n1111 1\n.end\n");
    levelize_ok("");
    const char *const sources[] = {"p", "n", "l", "f"};
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT64_MESSAGE(0, net_lvl(sources[i]), sources[i]);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(0, cell_lvl(sources[i]), sources[i]);
    }
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n2"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("y"));
}

/* A library cell with a seq statement (a gate-granularity flip-flop) is a source too. */
static void test_library_seq_cell_is_a_source(void) {
    read_gates_lib();
    read_blif(".model m\n.inputs a c\n.outputs y\n"
              ".names a n1\n1 1\n.subckt DFFP D=n1 C=c Q=q\n.names q y\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("n1"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("q"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("y"));
}

/* --- V-7 (h) to (j): memories and black boxes (Q4) -------------------------------------------- */

static const odin3_plugin_port k_bb_ports[] = {{"a", ODIN3_DIR_IN, 1, NULL, true},
                                               {"y", ODIN3_DIR_OUT, 1, NULL, true}};
static const odin3_plugin_port k_bbseq_ports[] = {{"clk", ODIN3_DIR_IN, 1, NULL, true},
                                                  {"d", ODIN3_DIR_IN, 1, NULL, true},
                                                  {"q", ODIN3_DIR_OUT, 1, NULL, true}};

static void register_plugin_types(void) {
    odin3_plugin_celltype bb = {.name = "lv_bb",
                                .gran = ODIN3_GRAN_BLACKBOX,
                                .flags = 0,
                                .ports = k_bb_ports,
                                .n_ports = 2};
    odin3_plugin_celltype seq = {.name = "lv_bbseq",
                                 .gran = ODIN3_GRAN_BLACKBOX,
                                 .flags = ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0,
                                 .ports = k_bbseq_ports,
                                 .n_ports = 3};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&bb));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register(&seq));
}

static void test_ram_between_two_chains(void) {
    read_vtr_lib();
    read_blif(".model m\n.inputs a clk we\n.outputs m2\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n"
              ".subckt single_port_ram clk=clk data[0]=n2 addr[0]=a we=we out[0]=r\n"
              ".names r m1\n1 1\n.names m1 m2\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n2"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("r"));
    TEST_ASSERT_EQUAL_INT64(0, cell_lvl("r"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("m1"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("m2"));
}

static void test_blackbox_without_latch_is_combinational(void) {
    read_blif(".model m\n.inputs a\n.outputs z\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n"
              ".subckt lv_bb a=n2 y=b\n.names b z\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(3, net_lvl("b"));
    TEST_ASSERT_EQUAL_INT64(4, net_lvl("z"));
}

static void test_blackbox_with_register_follows_memory_rule(void) {
    read_blif(".model m\n.inputs a clk\n.outputs z\n"
              ".names a n1\n1 1\n.names n1 n2\n1 1\n"
              ".subckt lv_bbseq clk=clk d=n2 q=b\n.names b z\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("n2"));
    TEST_ASSERT_EQUAL_INT64(0, net_lvl("b"));
    TEST_ASSERT_EQUAL_INT64(0, cell_lvl("b"));
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("z"));
}

/* --- V-3: an instance of a design module is one combinational cell ---------------------------- */

static void test_instance_is_one_combinational_cell(void) {
    read_blif(".model top\n.inputs a\n.outputs n2\n"
              ".names a n1\n1 1\n.subckt sub i=n1 o=s1\n.names s1 n2\n1 1\n.end\n"
              ".model sub\n.inputs i\n.outputs o\n.names i t\n1 1\n.names t o\n1 1\n.end\n");
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT64(1, net_lvl("n1"));
    TEST_ASSERT_EQUAL_INT64(2, net_lvl("s1"));
    TEST_ASSERT_EQUAL_INT64(3, net_lvl("n2"));
    const odin3_module *sub = odin3_module_get(design, (odin3_module_id){2});
    TEST_ASSERT_EQUAL_INT64(2, attr_int(sub, net_ref(sub, "o"), "odin3_level"));
    TEST_ASSERT_TRUE(log_has("levelize: 2 modules"));
    TEST_ASSERT_TRUE(log_has("instance"));
}

/* --- script path: the attribute reaches write_json ----------------------------------------------
 */

static void test_levels_reach_write_json(void) {
    read_blif(".model m\n.inputs a\n.outputs n2\n.names a n1\n1 1\n.names n1 n2\n1 1\n.end\n");
    char path[PATH_BUF];
    path_of("out.json", path);
    levelize_ok("");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", path));
    char text[JSON_MAX] = {0};
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(text, 1, sizeof text - 1, file);
    TEST_ASSERT_TRUE(got > 0);
    (void)fclose(file);
    TEST_ASSERT_NOT_NULL(strstr(text, "\"odin3_level\""));
}

/* --- V-6: cost -------------------------------------------------------------------------------- */

static void build_chain(odin3_module *module, odin3_net_id *first, odin3_net_id *last) {
    odin3_celltype_id type = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("$_NOT_"), &name));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, name, &type));
    odin3_net_id prev = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &prev));
    *first = prev;
    for (uint32_t i = 0; i < CHAIN_LEN; i++) {
        odin3_net_id next = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &next));
        odin3_netvec ports[2] = {{&prev, 1}, {&next, 1}};
        odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, NULL, 0};
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
        prev = next;
    }
    *last = prev;
}

static void test_chain_of_100000(void) {
    odin3_module_id mid = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("chain"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, name, (odin3_prov_id){0}, &mid));
    odin3_module *module = odin3_module_get(design, mid);
    odin3_net_id first = {0};
    odin3_net_id last = {0};
    build_chain(module, &first, &last);
    struct timespec t0;
    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    levelize_ok("");
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    TEST_ASSERT_TRUE((t1.tv_sec - t0.tv_sec) < CHAIN_MAX_SECONDS);
    TEST_ASSERT_EQUAL_INT64(
        0, attr_int(module, (odin3_objref){ODIN3_OBJ_NET, first.v}, "odin3_level"));
    TEST_ASSERT_EQUAL_INT64(CHAIN_LEN,
                            attr_int(module, (odin3_objref){ODIN3_OBJ_NET, last.v}, "odin3_level"));
    for (uint32_t id = 1; id <= CHAIN_LEN; id++) {
        TEST_ASSERT_EQUAL_INT64(
            id, attr_int(module, (odin3_objref){ODIN3_OBJ_NODE, id}, "odin3_level"));
    }
    TEST_ASSERT_TRUE(log_has("levelize: 1 modules, 100000 cells, max level 100000, 0 loops cut"));
    struct rusage usage;
    TEST_ASSERT_EQUAL_INT(0, getrusage(RUSAGE_SELF, &usage));
    TEST_ASSERT_TRUE(usage.ru_maxrss < CHAIN_MAX_KB);
}

int main(void) {
    UNITY_BEGIN();
    register_plugin_types();
    RUN_TEST(test_chain_of_100000);
    RUN_TEST(test_chain_of_three);
    RUN_TEST(test_reconvergent_takes_max);
    RUN_TEST(test_register_resets_level);
    RUN_TEST(test_constant_and_deeper_cone);
    RUN_TEST(test_loop_of_three_warns_once);
    RUN_TEST(test_cut_is_on_the_cycle_not_downstream);
    RUN_TEST(test_self_loop);
    RUN_TEST(test_two_disjoint_loops_warn_twice);
    RUN_TEST(test_rerun_identical_and_structure_untouched);
    RUN_TEST(test_word_cell_refused);
    RUN_TEST(test_process_refused);
    RUN_TEST(test_reserved_key_refused);
    RUN_TEST(test_custom_key_and_cut_key);
    RUN_TEST(test_user_attribute_not_overwritten);
    RUN_TEST(test_flops_and_latches_are_sources);
    RUN_TEST(test_library_seq_cell_is_a_source);
    RUN_TEST(test_ram_between_two_chains);
    RUN_TEST(test_blackbox_without_latch_is_combinational);
    RUN_TEST(test_blackbox_with_register_follows_memory_rule);
    RUN_TEST(test_instance_is_one_combinational_cell);
    RUN_TEST(test_levels_reach_write_json);
    return UNITY_END();
}
