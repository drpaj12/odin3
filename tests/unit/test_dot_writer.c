/*
 * test_dot_writer.c — unit tests for the Graphviz dot writer: clusters, edges, the node budget,
 * focus by path, by file:line (provenance forward index) and by fan-in cone, dot acceptance.
 */
#include "backends/dot/writer.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    CHAIN = 5,
    LONG_CHAIN = 20000,
    SET_MAX = 4096,
    OOM_LIMIT = 4000,
    BUDGET_TEST = 2,
    MSG_MAX = 512,
    NAME_BUF = 32,
    PATH_BUF = 256,
    FILE_BUF = 512,
};

static odin3_design *design;
static odin3_module *module;
static odin3_pass_ctx ctx;
static char last_error[MSG_MAX];
static char out_dir[PATH_BUF];
static char out_path[FILE_BUF];

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_module *make_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    TEST_ASSERT_NOT_NULL(mod);
    return mod;
}

/* A private directory per run (under TMPDIR or /tmp), so concurrent test runs never share files. */
static void make_out_dir(void) {
    const char *base = getenv("TMPDIR");
    (void)snprintf(out_dir, sizeof out_dir, "%s/odin3_dot_XXXXXX", base != NULL ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(out_dir));
    (void)snprintf(out_path, sizeof out_path, "%s/out.dot", out_dir);
}

static void remove_out_dir(void) {
    (void)remove(out_path);
    (void)rmdir(out_dir);
}

void setUp(void) {
    last_error[0] = '\0';
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("reader"), &ctx));
    module = make_module("top");
    make_out_dir();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
    remove_out_dir();
}

/* --- IR builders --------------------------------------------------------------------------- */

static odin3_prov_id source_at(const char *file, uint32_t line, uint32_t col) {
    odin3_srcloc loc = {intern(file), line, col, line, col + 1};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static odin3_net_id net_named(odin3_module *mod, const char *name) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_net_create(mod, name != NULL ? intern(name) : 0, (odin3_prov_id){0}, &id));
    return id;
}

typedef struct gate_spec {
    const char *type;
    const char *name;
    odin3_prov_id prov;
} gate_spec;

/* A node of a gate type with nets a [, b], y in port order (b is ignored for unary gates). */
static odin3_node_id gate_in(odin3_module *mod, gate_spec gs, const odin3_net_id *nets) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(gs.type), &tid));
    odin3_node_spec spec = {tid, gs.name != NULL ? intern(gs.name) : 0, gs.prov, NULL, 0};
    uint32_t ports = odin3_celltype_get(design, tid)->n_ports;
    odin3_netvec vecs[3];
    for (uint32_t i = 0; i < ports; i++) {
        vecs[i].nets = &nets[i];
        vecs[i].count = 1;
    }
    odin3_node_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, vecs, &id));
    return id;
}

static odin3_node_id not_gate(odin3_net_id in, odin3_net_id out, const char *name) {
    odin3_net_id nets[2] = {in, out};
    gate_spec gs = {"$_NOT_", name, (odin3_prov_id){0}};
    return gate_in(module, gs, nets);
}

static void add_port(odin3_module *mod, const char *name, odin3_dir dir) {
    odin3_port_spec spec = {intern(name), dir, 1, true, (odin3_prov_id){0}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
}

static odin3_net_id port_net(odin3_module *mod, uint32_t index) {
    return odin3_wire_net(mod, odin3_module_port_wire(mod, index), 0);
}

/* top: ports i, o; instance u1 of sub (ports a, y); sub: ports a, y with one NOT gate. */
static void build_hier(void) {
    odin3_module *sub = make_module("sub");
    add_port(sub, "a", ODIN3_DIR_IN);
    add_port(sub, "y", ODIN3_DIR_OUT);
    odin3_net_id inner[2] = {port_net(sub, 0), port_net(sub, 1)};
    gate_spec ng = {"$_NOT_", "inv", (odin3_prov_id){0}};
    gate_in(sub, ng, inner);
    add_port(module, "i", ODIN3_DIR_IN);
    add_port(module, "o", ODIN3_DIR_OUT);
    odin3_net_id outer[2] = {port_net(module, 0), port_net(module, 1)};
    gate_spec inst = {"sub", "u1", (odin3_prov_id){0}};
    gate_in(module, inst, outer);
}

/* A chain of n NOT gates g0..g(n-1) over nets n0..nn (named). Returns net n0. */
static odin3_net_id build_chain(uint32_t n, odin3_net_id *last) {
    char name[NAME_BUF];
    (void)snprintf(name, sizeof name, "n0");
    odin3_net_id first = net_named(module, name);
    odin3_net_id prev = first;
    for (uint32_t i = 0; i < n; i++) {
        (void)snprintf(name, sizeof name, "n%u", i + 1);
        odin3_net_id next = net_named(module, name);
        char gname[NAME_BUF];
        (void)snprintf(gname, sizeof gname, "g%u", i);
        not_gate(prev, next, gname);
        prev = next;
    }
    *last = prev;
    return first;
}

/* --- output inspection --------------------------------------------------------------------- */

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));
    long size = ftell(fp);
    TEST_ASSERT_TRUE(size >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_SET));
    char *text = odin3_util_malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, (size_t)size, fp);
    TEST_ASSERT_EQUAL_UINT((size_t)size, got);
    text[size] = '\0';
    (void)fclose(fp);
    return text;
}

static size_t count_of(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* The (module, node) pairs declared in text as "m<M>n<N> [label". */
typedef struct nodeset {
    uint64_t keys[SET_MAX];
    size_t count;
} nodeset;

/* Parses "m<M>n<N> [label" at the start of a line (leading blanks allowed). */
static bool parse_decl(const char *line, uint64_t *key) {
    const char *at = line + strspn(line, " \t");
    char *end = NULL;
    if (*at != 'm') {
        return false;
    }
    unsigned long mod = strtoul(at + 1, &end, 10);
    if (*end != 'n') {
        return false;
    }
    unsigned long node = strtoul(end + 1, &end, 10);
    if (strncmp(end, " [label", 7) != 0) {
        return false;
    }
    *key = ((uint64_t)mod << 32) | node;
    return true;
}

static nodeset declared_nodes(const char *text) {
    nodeset set = {{0}, 0};
    uint64_t key = 0;
    for (const char *at = strchr(text, '\n'); at != NULL; at = strchr(at, '\n')) {
        at++;
        if (parse_decl(at, &key)) {
            TEST_ASSERT_TRUE(set.count < SET_MAX);
            set.keys[set.count++] = key;
        }
    }
    return set;
}

static bool has_node(const nodeset *set, uint32_t mod, uint32_t node) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->keys[i] == (((uint64_t)mod << 32) | node)) {
            return true;
        }
    }
    return false;
}

static void write_ok(const odin3_dot_opts *opts) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_dot_write(design, out_path, opts));
}

static void expect_in(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), needle);
}

static void expect_absent(const char *text, const char *needle) {
    TEST_ASSERT_NULL_MESSAGE(strstr(text, needle), needle);
}

/* Writes with the focus, checks the node count, frees the text. */
static char *render(odin3_dot_focus_kind kind, const char *focus) {
    odin3_dot_opts opts = {kind, focus, 0};
    write_ok(&opts);
    return slurp(out_path);
}

static void expect_nodes(odin3_dot_focus_kind kind, const char *focus, size_t want) {
    char *text = render(kind, focus);
    size_t got = declared_nodes(text).count;
    odin3_util_free(text);
    TEST_ASSERT_EQUAL_UINT(want, got);
}

/* --- tests --------------------------------------------------------------------------------- */

static void test_clusters_per_module(void) {
    build_hier();
    char *text = render(ODIN3_DOT_FOCUS_NONE, NULL);
    TEST_ASSERT_TRUE(strncmp(text, "digraph", 7) == 0);
    TEST_ASSERT_EQUAL_UINT(1, count_of(text, "subgraph cluster_1 {"));
    TEST_ASSERT_EQUAL_UINT(1, count_of(text, "subgraph cluster_2 {"));
    expect_in(text, "$_NOT_");
    expect_in(text, "inv");
    expect_in(text, "u1");
    odin3_util_free(text);
}

static void test_edges_driver_to_sink(void) {
    odin3_net_id last = {0};
    (void)build_chain(CHAIN, &last);
    write_ok(NULL);
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_UINT(CHAIN - 1, count_of(text, " -> "));
    TEST_ASSERT_NOT_NULL(strstr(text, "m1n1 -> m1n2"));
    TEST_ASSERT_NOT_NULL(strstr(text, "label=\"n1\""));
    odin3_util_free(text);
}

static void test_edges_per_pin_pair(void) {
    odin3_net_id first = net_named(module, "w");
    odin3_net_id out1 = net_named(module, NULL);
    odin3_net_id out2 = net_named(module, NULL);
    not_gate(net_named(module, "x"), first, "drv");
    not_gate(first, out1, "s1");
    not_gate(first, out2, "s2");
    write_ok(NULL);
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_UINT(2, count_of(text, " -> "));
    odin3_util_free(text);
}

static void test_deterministic(void) {
    build_hier();
    write_ok(NULL);
    char *one = slurp(out_path);
    write_ok(NULL);
    char *two = slurp(out_path);
    TEST_ASSERT_EQUAL_STRING(one, two);
    odin3_util_free(one);
    odin3_util_free(two);
}

static void test_label_escaping(void) {
    odin3_net_id mid = net_named(module, "a\"b\\c");
    not_gate(net_named(module, NULL), mid, "drv");
    not_gate(mid, net_named(module, NULL), "q\"uote\nline");
    write_ok(NULL);
    char *text = slurp(out_path);
    TEST_ASSERT_NOT_NULL(strstr(text, "q\\\"uote\\nline"));
    TEST_ASSERT_NOT_NULL(strstr(text, "a\\\"b\\\\c"));
    odin3_util_free(text);
}

static void test_budget_refusal_counts(void) {
    odin3_net_id last = {0};
    (void)build_chain(CHAIN, &last);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_NONE, NULL, BUDGET_TEST};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "5 nodes"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "max-nodes 2"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "--focus"));
    TEST_ASSERT_NOT_EQUAL_INT(0, access(out_path, F_OK));
    opts.max_nodes = CHAIN; /* exactly at the budget is fine */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_dot_write(design, out_path, &opts));
}

static void test_default_budget(void) {
    odin3_net_id last = {0};
    (void)build_chain(ODIN3_DOT_DEFAULT_MAX_NODES + 1, &last);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, NULL));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "2001 nodes"));
}

static void test_focus_lifts_budget(void) {
    odin3_net_id last = {0};
    (void)build_chain(CHAIN, &last);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_PATH, "top", BUDGET_TEST};
    write_ok(&opts);
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_UINT(CHAIN, declared_nodes(text).count);
    odin3_util_free(text);
}

static void test_focus_path(void) {
    build_hier();
    char *text = render(ODIN3_DOT_FOCUS_PATH, "top/u1");
    expect_in(text, "label=\"sub\"");
    expect_absent(text, "label=\"top\"");
    expect_in(text, "inv");
    expect_absent(text, "u1");
    odin3_util_free(text);
    text = render(ODIN3_DOT_FOCUS_PATH, "top");
    expect_in(text, "label=\"top\"");
    expect_in(text, "label=\"sub\""); /* the subtree under top */
    odin3_util_free(text);
    text = render(ODIN3_DOT_FOCUS_PATH, "sub");
    expect_absent(text, "label=\"top\"");
    odin3_util_free(text);
}

static void test_focus_path_invalid(void) {
    build_hier();
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_PATH, "nosuch", 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    opts.focus = "top/nosuch";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    opts.focus = "top/i"; /* names a node that is not a module instance */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    opts.focus = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
}

/* Every live node hit is drawn; returns how many there are. */
static size_t assert_hits_drawn(odin3_prov_hits hits, const nodeset *drawn) {
    size_t live = 0;
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        if (hit->obj.kind == ODIN3_OBJ_NODE && hit->live) {
            live++;
            TEST_ASSERT_TRUE(has_node(drawn, hit->module.v, hit->obj.id));
        }
    }
    return live;
}

/* Review Focus 4: --focus file:line selects exactly what the provenance forward index returns. */
static void test_focus_loc_matches_index(void) {
    odin3_prov_id at10 = source_at("x.v", 10, 1);
    odin3_prov_id at10b = source_at("x.v", 10, 5);
    odin3_prov_id at11 = source_at("x.v", 11, 1);
    odin3_prov_id other = source_at("y.v", 10, 1);
    odin3_pass_ctx lower = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("lower"), &lower));
    odin3_prov_begin_op(&lower);
    odin3_prov_id derived = {0};
    odin3_prov_list parents = {&at10, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&lower, parents, &derived));
    odin3_prov_id provs[] = {at10, derived, at11, at10b, at10, other, (odin3_prov_id){0}};
    odin3_node_id nodes[7];
    odin3_net_id prev = net_named(module, NULL);
    for (size_t i = 0; i < 7; i++) {
        odin3_net_id next = net_named(module, NULL);
        odin3_net_id nets[2] = {prev, next};
        gate_spec gs = {"$_NOT_", NULL, provs[i]};
        nodes[i] = gate_in(module, gs, nets);
        prev = next;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, nodes[4])); /* dead: not drawn */
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_srcloc loc = {intern("x.v"), 10, 0, 0, 0};
    odin3_prov_hits hits = odin3_prov_index_by_loc(ix, loc);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_LOC, "x.v:10", 0};
    write_ok(&opts);
    char *text = slurp(out_path);
    nodeset got = declared_nodes(text);
    size_t live_hits = assert_hits_drawn(hits, &got);
    TEST_ASSERT_EQUAL_UINT(3, live_hits); /* nodes 0, 1 (derived), 3 (col 5); 4 is dead */
    TEST_ASSERT_EQUAL_UINT(live_hits, got.count);
    odin3_prov_index_destroy(ix);
    odin3_util_free(text);
}

static void test_focus_loc_none_and_invalid(void) {
    odin3_net_id last = {0};
    (void)build_chain(2, &last);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_LOC, "nowhere.v:3", 0};
    write_ok(&opts); /* a valid location with no objects: an empty graph */
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_UINT(0, declared_nodes(text).count);
    odin3_util_free(text);
    const char *bad[] = {"x.v", "x.v:", ":3", "x.v:3q", "x.v:-1", ""};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        opts.focus = bad[i];
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    }
}

static void test_focus_cone(void) {
    odin3_net_id last = {0};
    (void)build_chain(CHAIN, &last);
    not_gate(net_named(module, "other_in"), net_named(module, "other_out"), "unrelated");
    char *text = render(ODIN3_DOT_FOCUS_CONE, "n3");
    nodeset got = declared_nodes(text);
    TEST_ASSERT_EQUAL_UINT(3, got.count); /* g0, g1, g2 drive n1, n2, n3 */
    TEST_ASSERT_TRUE(has_node(&got, 1, 1) && has_node(&got, 1, 2) && has_node(&got, 1, 3));
    TEST_ASSERT_EQUAL_UINT(2, count_of(text, " -> "));
    expect_absent(text, "unrelated");
    odin3_util_free(text);
    expect_nodes(ODIN3_DOT_FOCUS_CONE, "top:n3", 3); /* module-qualified */
    expect_nodes(ODIN3_DOT_FOCUS_CONE, "n0", 0);     /* a net nothing drives */
}

static void test_cone_through_reconvergence_and_loop(void) {
    odin3_net_id in_a = net_named(module, "a");
    odin3_net_id in_b = net_named(module, "b");
    odin3_net_id out = net_named(module, "y");
    odin3_net_id nets[3] = {in_a, in_b, out};
    gate_spec gs = {"$_AND_", "and", (odin3_prov_id){0}};
    gate_in(module, gs, nets);
    not_gate(out, in_a, "fb"); /* a combinational loop: y -> a -> y */
    expect_nodes(ODIN3_DOT_FOCUS_CONE, "y", 2);
}

/* The cone walk is iterative: a chain far deeper than any sane stack still works. */
static void test_cone_deep_chain(void) {
    odin3_net_id last = {0};
    (void)build_chain(LONG_CHAIN, &last);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_CONE, "n20000", 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_dot_write(design, out_path, &(odin3_dot_opts){0, NULL, 0}));
    write_ok(&opts);
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_UINT(LONG_CHAIN - 1, count_of(text, " -> "));
    odin3_util_free(text);
}

static void test_cone_invalid(void) {
    odin3_net_id last = {0};
    (void)build_chain(2, &last);
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_CONE, "nonet", 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    opts.focus = "nomod:n1";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
    opts.focus = "";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
}

static void test_invalid_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(NULL, out_path, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, NULL, NULL));
    odin3_dot_opts opts = {(odin3_dot_focus_kind)99, "x", 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_dot_write(design, out_path, &opts));
}

static void test_unwritable_path(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_dot_write(design, "/nonexistent-dir/out.dot", NULL));
}

static void test_empty_design(void) {
    write_ok(NULL);
    char *text = slurp(out_path);
    TEST_ASSERT_TRUE(strncmp(text, "digraph", 7) == 0);
    odin3_util_free(text);
}

static void test_odd_module_name_cluster_id(void) {
    odin3_module *odd = make_module("a.b c");
    add_port(odd, "p", ODIN3_DIR_IN);
    char *text = render(ODIN3_DOT_FOCUS_NONE, NULL);
    expect_in(text, "subgraph cluster_2 {");
    expect_in(text, "label=\"a.b c\"");
    odin3_util_free(text);
}

/* One write with the fail-th allocation failing; nothing is written unless it succeeds. */
static bool oom_try(const odin3_dot_opts *opts, long fail) {
    (void)remove(out_path);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_dot_write(design, out_path, opts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_TRUE((st == ODIN3_OK) == (access(out_path, F_OK) == 0));
    return st == ODIN3_OK;
}

/* Fails each allocation point in turn until one run succeeds. */
static void oom_sweep(const odin3_dot_opts *opts) {
    long fail = 0;
    while (fail < OOM_LIMIT && !oom_try(opts, fail)) {
        fail++;
    }
    TEST_ASSERT_TRUE(fail < OOM_LIMIT);
}

/* Old scheme: modules "m2" (ID 1) and "a.b" (ID 2) could both be cluster_m2. */
static void test_cluster_ids_unique(void) {
    odin3_module *plain = make_module("m3");
    odin3_module *odd = make_module("a.b");
    add_port(plain, "p", ODIN3_DIR_IN);
    add_port(odd, "p", ODIN3_DIR_IN);
    char *text = render(ODIN3_DOT_FOCUS_NONE, NULL);
    TEST_ASSERT_EQUAL_UINT(1, count_of(text, "subgraph cluster_2 {"));
    TEST_ASSERT_EQUAL_UINT(1, count_of(text, "subgraph cluster_3 {"));
    expect_in(text, "label=\"m3\"");
    expect_in(text, "label=\"a.b\"");
    odin3_util_free(text);
}

static void write_old_destination(void) {
    FILE *fp = fopen(out_path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_TRUE(fputs("old contents", fp) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

/* The run directory holds the destination only: the writer's temporary ("out.dot.XXXXXX") is gone.
 */
static void assert_no_temp_file(void) {
    DIR *dir = opendir(out_dir);
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        TEST_ASSERT_NULL(strstr(ent->d_name, "out.dot."));
    }
    (void)closedir(dir);
}

/* A failed write leaves an existing destination untouched and no temporary file. */
static void test_failure_keeps_destination(void) {
    build_hier();
    write_old_destination();
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_dot_write(design, out_path, NULL));
    odin3_util_set_alloc_fail_after(-1);
    char *text = slurp(out_path);
    TEST_ASSERT_EQUAL_STRING("old contents", text);
    odin3_util_free(text);
    assert_no_temp_file();
}

static void test_rename_failure_cleans_temp(void) {
    build_hier();
    TEST_ASSERT_EQUAL_INT(0, mkdir(out_path, 0700)); /* a directory cannot be renamed over */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_dot_write(design, out_path, NULL));
    assert_no_temp_file();
    TEST_ASSERT_EQUAL_INT(0, rmdir(out_path));
}

static void test_out_of_memory_sweep(void) {
    build_hier();
    oom_sweep(NULL);
}

static void test_focus_oom_sweep(void) {
    odin3_net_id last = {0};
    (void)build_chain(CHAIN, &last);
    gate_spec located = {"$_NOT_", NULL, source_at("x.v", 1, 1)};
    odin3_net_id nets[2] = {last, net_named(module, NULL)};
    gate_in(module, located, nets);
    odin3_dot_opts cone = {ODIN3_DOT_FOCUS_CONE, "top:n3", 0};
    oom_sweep(&cone);
    odin3_dot_opts loc = {ODIN3_DOT_FOCUS_LOC, "x.v:1", 0};
    oom_sweep(&loc);
    odin3_dot_opts path = {ODIN3_DOT_FOCUS_PATH, "top", 0};
    oom_sweep(&path);
}

extern char **environ;

/* True when dot -Tsvg exits 0 on the file. */
static bool dot_accepts(const char *path) {
    char *argv[] = {"dot", "-Tsvg", (char *)path, "-o", "/dev/null", NULL};
    pid_t pid = 0;
    int status = 0;
    if (posix_spawn(&pid, "/usr/bin/dot", NULL, NULL, argv, environ) != 0) {
        return false;
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void test_dot_accepts_output(void) {
    if (access("/usr/bin/dot", X_OK) != 0) {
        TEST_IGNORE_MESSAGE("dot is not installed");
    }
    build_hier();
    odin3_prov_id weird = source_at("x.v", 1, 1);
    odin3_net_id weird_net = net_named(module, "we\"ird\\net");
    gate_spec gs = {"$_NOT_", "na me", weird};
    odin3_net_id nets[2] = {weird_net, net_named(module, NULL)};
    gate_in(module, gs, nets);
    write_ok(NULL);
    TEST_ASSERT_TRUE(dot_accepts(out_path));
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_PATH, "top/u1", 0};
    write_ok(&opts);
    TEST_ASSERT_TRUE(dot_accepts(out_path));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clusters_per_module);
    RUN_TEST(test_edges_driver_to_sink);
    RUN_TEST(test_edges_per_pin_pair);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_label_escaping);
    RUN_TEST(test_budget_refusal_counts);
    RUN_TEST(test_default_budget);
    RUN_TEST(test_focus_lifts_budget);
    RUN_TEST(test_focus_path);
    RUN_TEST(test_focus_path_invalid);
    RUN_TEST(test_focus_loc_matches_index);
    RUN_TEST(test_focus_loc_none_and_invalid);
    RUN_TEST(test_focus_cone);
    RUN_TEST(test_cone_through_reconvergence_and_loop);
    RUN_TEST(test_cone_deep_chain);
    RUN_TEST(test_cone_invalid);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_unwritable_path);
    RUN_TEST(test_empty_design);
    RUN_TEST(test_odd_module_name_cluster_id);
    RUN_TEST(test_cluster_ids_unique);
    RUN_TEST(test_failure_keeps_destination);
    RUN_TEST(test_rename_failure_cleans_temp);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_focus_oom_sweep);
    RUN_TEST(test_dot_accepts_output);
    return UNITY_END();
}
