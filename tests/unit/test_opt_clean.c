/*
 * test_opt_clean.c — opt_clean (2F spec OP-10, §10; plan Task 3): the buffer wiring, liveness
 * (ports, `keep` wires and nodes, instances of `keep` modules, black boxes, $mem and
 * $memrd/$memwr), the deletion order and the DEBUG lines, a dead FF's init, wires (public aliases
 * and constants without `-purge`, `-purge`), the silent init normalisation, modules with processes,
 * constants (carry M-d) and out of memory.
 *
 * Yosys parity (0.55, the VTR build; probes `yosys -p "read_rtlil X.il; opt_clean [-purge];
 * write_rtlil"`, PR #80 review): the init rules (opt_clean.cc rmunused_module_signals: an init on a
 * constant-driven net goes whatever its value, a duplicate init survives only on one alias, both
 * silently), the public-wire rule (without -purge an aliased or constant public wire stays; with
 * it only the primary name of a net with pins), the instance removal and the `$_BUF_` wiring are
 * Yosys's; the net counts are 2F §10's (Odin III names are wires over nets; IR-2, §17 #3).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LINE_MAX_LEN = 256, LINES_MAX = 64, OOM_LIMIT = 4000 };

typedef struct log_line {
    odin3_log_level level;
    char text[LINE_MAX_LEN];
} log_line;

static log_line lines[LINES_MAX];
static uint32_t n_lines;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LINES_MAX) {
        size_t len = strlen(msg);
        len = len < LINE_MAX_LEN ? len : LINE_MAX_LEN - 1;
        memcpy(lines[n_lines].text, msg, len);
        lines[n_lines].text[len] = '\0';
        lines[n_lines].level = level;
        n_lines++;
    }
}

static uint32_t count_lines(odin3_log_level level, const char *text) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level && strcmp(lines[i].text, text) == 0 ? 1U : 0U;
    }
    return hits;
}

static uint32_t count_level(odin3_log_level level) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level ? 1U : 0U;
    }
    return hits;
}

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_module *module;
    bool open;
} fixture;

static fixture fx;

void setUp(void) {
    memset(&fx, 0, sizeof fx);
    n_lines = 0;
    odin3_log_set_sink(sink, NULL);
    odin3_log_set_level(ODIN3_LOG_DEBUG);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
    odin3_log_set_level(ODIN3_LOG_INFO);
    odin3_log_set_sink(NULL, NULL);
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void build(const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

static void run_clean(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

static odin3_wire_id wire_named(const char *name) {
    return odin3_module_find_wire(fx.module, intern(name));
}

static odin3_net_id wnet(const char *name) {
    odin3_wire_id wire = wire_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, 0);
}

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

/* The net on port `port` (bit 0) of the node named name. */
static odin3_net_id port_net(const char *name, uint32_t port) {
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), name);
    return odin3_pin_net(fx.module, odin3_node_port(fx.module, node, port).first);
}

static bool alive(const char *name) {
    odin3_node_id node = node_named(name);
    return odin3_node_valid(node) && odin3_node_live(fx.module, node);
}

/* --- buffers -----------------------------------------------------------------------------------
 */

#define BUF_TEXT(attr)                                                                             \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\t\n" attr                       \
    "  cell $_BUF_ \\b\n    connect \\A \\a\n    connect \\Y \\t\n  end\n"                         \
    "  cell $_NOT_ \\n\n    connect \\A \\t\n    connect \\Y \\y\n  end\nend\n"

static void test_clean_buf(void) {
    build(BUF_TEXT(""));
    run_clean();
    TEST_ASSERT_FALSE(alive("b"));
    TEST_ASSERT_TRUE(alive("n"));
    TEST_ASSERT_EQUAL_UINT32(wnet("a").v, port_net("n", 0).v); /* the buffer is wiring */
    TEST_ASSERT_EQUAL_UINT32(wnet("a").v, wnet("t").v);        /* t is a name of a's net */
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_cells);        /* Yosys does not count it */
    TEST_ASSERT_TRUE(fx.counts.clean_changes > 0);             /* but it is did_something */
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO,
                       "opt_clean: removed 0 unused cells, 0 unused nets and 0 unused wires"));
}

static void test_clean_buf_keep(void) {
    build(BUF_TEXT("  attribute \\keep 1\n"));
    run_clean();
    TEST_ASSERT_TRUE(alive("b"));
    TEST_ASSERT_TRUE(wnet("a").v != wnet("t").v);
    TEST_ASSERT_EQUAL_UINT32(wnet("t").v, port_net("n", 0).v);
}

/* --- liveness ----------------------------------------------------------------------------------
 */

static void test_clean_liveness(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n  wire $tmp1\n"
          "  cell $or \\d\n    connect \\A \\a\n    connect \\B \\b\n    connect \\Y $tmp1\n  end\n"
          "  cell $and \\g\n    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\n"
          "end\n");
    odin3_node_id dead = node_named("d");
    TEST_ASSERT_EQUAL_UINT32(4, dead.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, dead, 0)); /* shown as $c4 */
    odin3_net_id ynet = odin3_pin_net(fx.module, odin3_node_port(fx.module, dead, 2).first);
    run_clean();
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, dead));
    TEST_ASSERT_FALSE(odin3_net_live(fx.module, ynet));
    TEST_ASSERT_TRUE(alive("g"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO,
                       "opt_clean: removed 1 unused cells, 1 unused nets and 0 unused wires"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_clean: module 'm': removing unused $or '$c4'"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_cells);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_nets);
}

/* Ports: a $port_inout's net is a root, so the logic driving it stays. */
static void test_clean_inout_root(void) {
    build("module \\m\n  wire input 1 \\a\n  wire inout 2 \\io\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\io\n  end\nend\n");
    run_clean();
    TEST_ASSERT_TRUE(alive("n"));
}

/* --- instances ---------------------------------------------------------------------------------
 */

#define SUB_TEXT                                                                                   \
    "module \\sub\n  wire input 1 \\a\n  wire output 2 \\y\n"                                      \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"

#define TOP_TEXT(attr)                                                                             \
    "module \\top\n  wire input 1 \\a\n  wire output 2 \\y\n  wire $tmp1\n"                        \
    "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\n" attr                    \
    "  cell \\sub \\u1\n    connect \\a \\a\n    connect \\y $tmp1\n  end\nend\n"

static void test_clean_unused_instance_removed(void) {
    build(SUB_TEXT TOP_TEXT(""));
    run_clean();
    TEST_ASSERT_FALSE(alive("u1"));
    TEST_ASSERT_TRUE(alive("g"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_cells);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_clean: module 'top': removing unused sub 'u1'"));
}

static void test_clean_keep_instance_kept(void) {
    build(SUB_TEXT TOP_TEXT("  attribute \\keep 1\n"));
    run_clean();
    TEST_ASSERT_TRUE(alive("u1"));
}

#define KEEP_CHAIN_TEXT                                                                            \
    "module \\lone\n  wire input 1 \\a\n  wire output 2 \\y\n"                                     \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"                    \
    "module \\leaf\n  wire input 1 \\a\n  wire output 2 \\y\n  attribute \\keep 1\n  wire \\w\n"   \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"                         \
    "  cell $_NOT_ \\k\n    connect \\A \\a\n    connect \\Y \\w\n  end\nend\n"                    \
    "module \\mid\n  wire input 1 \\a\n  wire output 2 \\y\n"                                      \
    "  cell \\leaf \\u\n    connect \\a \\a\n    connect \\y \\y\n  end\nend\n"                    \
    "attribute \\keep 1\nmodule \\kmod\n  wire input 1 \\a\n  wire output 2 \\y\n"                 \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"                    \
    "module \\top\n  wire input 1 \\a\n  wire output 2 \\y\n  wire $tmp1\n  wire $tmp2\n"          \
    "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"                         \
    "  cell \\mid \\um\n    connect \\a \\a\n    connect \\y $tmp1\n  end\n"                       \
    "  cell \\lone \\ul\n    connect \\a \\a\n    connect \\y $tmp2\n  end\nend\n"

/* The flags of module IDs 1.. as a text of 0 and 1. */
static void flags_text(const odin3_vec *keep, char *out) {
    const bool *flags = keep->data;
    for (size_t id = 1; id < keep->len; id++) {
        out[id - 1] = flags[id] ? '1' : '0';
    }
    out[keep->len - 1] = '\0';
}

static void test_clean_keep_inside_child(void) {
    build(KEEP_CHAIN_TEXT);
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(bool));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_keep_modules(fx.res.design, &keep));
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)keep.len); /* IDs 1..5 and the unused 0 */
    char text[8];
    flags_text(&keep, text);
    /* lone no; leaf a keep wire; mid an instance of leaf; kmod has keep; top two levels up */
    TEST_ASSERT_EQUAL_STRING("01111", text);
    odin3_vec_free(&keep);
    run_clean();
    TEST_ASSERT_TRUE(alive("um"));  /* mid is a keep module: its instance stays */
    TEST_ASSERT_FALSE(alive("ul")); /* lone has no keep: removed */
}

static void test_clean_keep_misuse(void) {
    build(KEEP_CHAIN_TEXT);
    odin3_vec bad;
    odin3_vec_init(&bad, sizeof(uint32_t)); /* not a bool vector */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_keep_modules(fx.res.design, &bad));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_keep_modules(NULL, &bad));
    odin3_vec_free(&bad);
}

/* --- black boxes and memories -------------------------------------------------------------- */

static void test_clean_blackbox(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    const odin3_port_def ports[2] = {
        {"I", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
        {"O", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false},
    };
    odin3_celltype_def def;
    memset(&def, 0, sizeof def);
    def.name = "bb";
    def.gran = ODIN3_GRAN_BLACKBOX;
    def.ports = ports;
    def.n_ports = 2;
    odin3_celltype_id type = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(fx.res.design, &def, &type));
    const char *names[2] = {"bbdead", "bbkeep"};
    for (uint32_t i = 0; i < 2; i++) {
        odin3_net_id out = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(fx.module, 0, (odin3_prov_id){0}, &out));
        odin3_net_id in = wnet("a");
        const odin3_netvec vecs[2] = {{&in, 1}, {&out, 1}};
        odin3_node_spec spec = {type, intern(names[i]), (odin3_prov_id){0}, NULL, 0};
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(fx.module, &spec, vecs, &node));
        if (i == 1) {
            odin3_value one;
            memset(&one, 0, sizeof one);
            one.kind = ODIN3_VAL_INT;
            one.i = 1;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_attr_set(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                 intern("keep"), &one));
        }
    }
    run_clean();
    TEST_ASSERT_FALSE(alive("bbdead"));
    TEST_ASSERT_TRUE(alive("bbkeep"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_cells);
}

#define MEM_PORTS(rdata)                                                                           \
    "    connect \\RD_CLK \\c\n    connect \\RD_EN \\c\n    connect \\RD_ADDR \\a\n"               \
    "    connect \\RD_DATA " rdata "\n    connect \\WR_CLK \\c\n    connect \\WR_EN \\c\n"         \
    "    connect \\WR_ADDR \\a\n    connect \\WR_DATA \\a\n  end\n"

static void test_clean_mem(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\c\n  wire output 3 \\y\n"
          "  wire $tmp1\n  cell $mem \\live\n" MEM_PORTS("\\y") "  cell $mem \\dead\n" MEM_PORTS(
              "$tmp1") "end\n");
    run_clean();
    TEST_ASSERT_TRUE(alive("live")); /* a live read port keeps the memory (and its write ports) */
    TEST_ASSERT_FALSE(alive("dead"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_cells);
}

/* Yosys mem2cells: a $memwr stays while a live $memrd reads its memory, and only then. */
static void test_clean_memrd_memwr(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\c\n  wire output 3 \\y\n"
          "  cell $memrd \\r\n    parameter \\MEMID \"mem1\"\n    connect \\CLK \\c\n"
          "    connect \\EN \\c\n    connect \\ADDR \\a\n    connect \\DATA \\y\n  end\n"
          "  cell $memwr \\w1\n    parameter \\MEMID \"mem1\"\n    connect \\CLK \\c\n"
          "    connect \\EN \\c\n    connect \\ADDR \\a\n    connect \\DATA \\a\n  end\n"
          "  cell $memwr \\w2\n    parameter \\MEMID \"mem2\"\n    connect \\CLK \\c\n"
          "    connect \\EN \\c\n    connect \\ADDR \\a\n    connect \\DATA \\a\n  end\nend\n");
    run_clean();
    TEST_ASSERT_TRUE(alive("r"));
    TEST_ASSERT_TRUE(alive("w1"));
    TEST_ASSERT_FALSE(alive("w2"));
}

/* --- flip-flops and init --------------------------------------------------------------------- */

static void test_clean_ff_init(void) {
    build("module \\m\n  wire input 1 \\c\n  wire input 2 \\d\n  wire output 3 \\y\n"
          "  attribute \\init 1'1\n  wire \\q\n  attribute \\init 1'1\n  wire \\q2\n"
          "  cell $_DFF_P_ \\f\n    connect \\C \\c\n    connect \\D \\d\n    connect \\Q \\q\n  "
          "end\n"
          "  cell $_DFF_P_ \\f2\n    connect \\C \\c\n    connect \\D \\d\n    connect \\Q \\q2\n"
          "  end\n  cell $_NOT_ \\n\n    connect \\A \\q2\n    connect \\Y \\y\n  end\nend\n");
    odin3_net_id qnet = wnet("q");
    odin3_net_id q2net = wnet("q2");
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, qnet));
    run_clean();
    TEST_ASSERT_FALSE(alive("f")); /* dead: removed, with its Q net's init */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, qnet));
    TEST_ASSERT_FALSE(odin3_wire_live(fx.module, wire_named("q"))); /* no init keeps it any more */
    TEST_ASSERT_TRUE(alive("f2"));                                  /* live: its init stays */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, q2net));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_cells);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires);
}

/* An init on a constant-driven net goes, silently, whatever its value; an init on a driven net
 * stays. */
static void test_clean_init_conflict(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire $tmp1\n"
          "  attribute \\init 1'1\n  wire \\w\n  attribute \\init 1'0\n  wire \\u\n  connect \\w "
          "1'0\n"
          "  cell $_NOT_ \\n1\n    connect \\A \\w\n    connect \\Y $tmp1\n  end\n"
          "  cell $_NOT_ \\n2\n    connect \\A \\a\n    connect \\Y \\u\n  end\n"
          "  cell $_AND_ \\n3\n    connect \\A $tmp1\n    connect \\B \\u\n    connect \\Y \\y\n"
          "  end\nend\n");
    run_clean();
    TEST_ASSERT_EQUAL_UINT32(0, count_level(ODIN3_LOG_WARN)); /* Yosys prints no conflict */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wnet("w")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_ffinit_get(fx.module, wnet("u"))); /* kept, silently */
    TEST_ASSERT_TRUE(odin3_wire_live(fx.module, wire_named("w"))); /* it has pins: stays */
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_changes);          /* removing it is no change */
}

/* --- wires ------------------------------------------------------------------------------------ */

#define WIRES_TEXT                                                                                 \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\dead\n"                         \
    "  attribute \\keep 1\n  wire \\kp\n  attribute \\init 1'1\n  wire \\iw\n  wire \\w\n"         \
    "  wire $x\n"                                                                                  \
    "  cell $_NOT_ \\n1\n    connect \\A \\a\n    connect \\Y \\w\n  end\n"                        \
    "  cell $_NOT_ \\n2\n    connect \\A \\w\n    connect \\Y $x\n  end\n"                         \
    "  cell $_NOT_ \\n3\n    connect \\A $x\n    connect \\Y \\y\n  end\nend\n"

typedef struct wire_expect {
    const char *name;
    bool live;
} wire_expect;

static void expect_wires(const wire_expect *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bool live = odin3_wire_live(fx.module, wire_named(list[i].name));
        TEST_ASSERT_EQUAL_INT_MESSAGE(list[i].live, live, list[i].name);
    }
}

static void test_clean_wires(void) {
    build(WIRES_TEXT);
    odin3_net_id dead_net = wnet("dead");
    run_clean();
    TEST_ASSERT_FALSE(odin3_net_live(fx.module, dead_net)); /* the deleted wire's net goes too */
    const wire_expect first[] = {{"dead", false},           /* no pins: deleted */
                                 {"a", true},               /* a port */
                                 {"kp", true},              /* keep */
                                 {"iw", true},              /* a defined init */
                                 {"w", true},               /* used */
                                 {"$x", true}};             /* used, and a $-name: kept (§17 #3) */
    expect_wires(first, sizeof first / sizeof first[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("iw"))); /* init stays */
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_nets);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_cells);
    run_clean(); /* nothing changes: the init is Yosys's to keep */
    expect_wires(first, sizeof first / sizeof first[0]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires);
}

static void test_clean_purge(void) {
    fx.opts.purge = true;
    build(WIRES_TEXT);
    run_clean();
    const wire_expect list[] = {{"w", true}, /* used, and its net's primary name: stays */
                                {"$x", true}, {"a", true},    {"kp", true},
                                {"iw", true}, {"dead", false}};
    expect_wires(list, sizeof list / sizeof list[0]);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_pins(fx.module, port_net("n1", 1)).count);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires);
}

#define ALIAS_TEXT                                                                                 \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\v\n  wire \\w\n"                \
    "  connect \\v \\w\n"                                                                          \
    "  cell $_NOT_ \\n1\n    connect \\A \\a\n    connect \\Y \\w\n  end\n"                        \
    "  cell $_NOT_ \\n2\n    connect \\A \\v\n    connect \\Y \\y\n  end\nend\n"

/* Without -purge both names of a net with pins stay; with it, only the non-primary alias goes. */
static void test_clean_purge_alias(void) {
    build(ALIAS_TEXT);
    odin3_net_id net = wnet("w");
    odin3_wire_id primary = odin3_net_primary(fx.module, net).wire;
    TEST_ASSERT_EQUAL_UINT32(net.v, wnet("v").v);
    run_clean();
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_wires);
    fx.opts.purge = true;
    run_clean();
    TEST_ASSERT_TRUE(odin3_wire_live(fx.module, primary)); /* the primary name stays */
    TEST_ASSERT_TRUE(odin3_net_live(fx.module, net));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires); /* and the other name went */
}

#define PINLESS_ALIAS_TEXT                                                                         \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\p\n"                            \
    "  attribute \\keep 1\n  wire \\k\n  connect \\p \\k\n"                                        \
    "  cell $_NOT_ \\n1\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"

/* Expects p and k live or not, and the pinless net alive. */
static void expect_pinless(bool p_live, odin3_net_id net) {
    const wire_expect list[] = {{"p", p_live}, {"k", true}};
    expect_wires(list, sizeof list / sizeof list[0]);
    TEST_ASSERT_TRUE(odin3_net_live(fx.module, net));
}

/* A pinless aliased net: without -purge the public alias p stays (Yosys s1 != s2) beside the
 * `keep` wire k; with -purge p goes and the net survives through k (clean_nets' alias guard). */
static void test_clean_net_alias_survives(void) {
    build(PINLESS_ALIAS_TEXT);
    odin3_net_id net = wnet("k");
    TEST_ASSERT_EQUAL_UINT32(net.v, wnet("p").v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(fx.module, net).count);
    run_clean();
    expect_pinless(true, net);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_wires);
    fx.opts.purge = true;
    run_clean();
    expect_pinless(false, net);
    TEST_ASSERT_EQUAL_UINT32(net.v, wnet("k").v);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.clean_wires);
}

#define UNUSED_ALIAS_TEXT                                                                          \
    "module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire \\c\n  wire \\u1\n"               \
    "  wire \\u2\n  wire $d1\n  wire $d2\n  connect \\c 1'0\n  connect \\u2 \\u1\n"                \
    "  connect $d2 $d1\n"                                                                          \
    "  cell $_NOT_ \\n1\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"

/* Fixtures A and B: without -purge a public wire that is a constant (A) or an alias (B) stays
 * though nothing reads it; the `$` twins of B go (no public name). With -purge all of them go. */
static void test_clean_public_alias_and_const_kept(void) {
    build(UNUSED_ALIAS_TEXT);
    run_clean();
    const wire_expect list[] = {
        {"c", true}, {"u1", true}, {"u2", true}, {"$d1", false}, {"$d2", false}};
    expect_wires(list, sizeof list / sizeof list[0]);
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.clean_wires);
    run_clean(); /* a second run changes nothing */
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.clean_wires);
}

static void test_clean_public_alias_and_const_purge(void) {
    fx.opts.purge = true;
    build(UNUSED_ALIAS_TEXT);
    run_clean();
    const wire_expect list[] = {
        {"c", false}, {"u1", false}, {"u2", false}, {"$d1", false}, {"$d2", false}};
    expect_wires(list, sizeof list / sizeof list[0]);
    TEST_ASSERT_EQUAL_UINT32(5, fx.counts.clean_wires);
}

/* Multi-bit -purge: v[0] only aliases w[0] but v[1] is the primary name of a net with a reader, so
 * v stays; of the fully aliased pair s and t, the primary name stays and the other goes. */
static void test_clean_purge_multibit(void) {
    fx.opts.purge = true;
    build("module \\m\n  wire width 2 input 1 \\a\n  wire width 2 output 2 \\y\n"
          "  wire width 2 \\w\n  wire width 2 \\v\n  wire width 2 \\s\n  wire width 2 \\t\n"
          "  wire width 2 \\x\n  connect \\v [0] \\w [0]\n  connect \\t \\s\n"
          "  cell $not \\g\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\a\n    connect \\Y \\w\n  end\n"
          "  cell $not \\r\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\v\n    connect \\Y \\x\n  end\n"
          "  cell $not \\r2\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\x\n    connect \\Y \\s\n  end\n"
          "  cell $not \\r3\n    parameter \\A_WIDTH 2\n    parameter \\Y_WIDTH 2\n"
          "    connect \\A \\t\n    connect \\Y \\y\n  end\nend\n");
    run_clean();
    const wire_expect list[] = {{"v", true}, {"x", true}};
    expect_wires(list, sizeof list / sizeof list[0]);
    TEST_ASSERT_EQUAL_INT(1, (int)odin3_wire_live(fx.module, wire_named("s")) +
                                 (int)odin3_wire_live(fx.module, wire_named("t")));
}

/* A `keep` wire is a liveness root: the logic driving it stays though nothing reads it. */
static void test_clean_keep_wire_root(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  attribute \\keep 1\n  wire \\k\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\k\n  end\n"
          "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    run_clean();
    TEST_ASSERT_TRUE(alive("n"));
    TEST_ASSERT_TRUE(alive("g"));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_cells);
    TEST_ASSERT_TRUE(odin3_wire_live(fx.module, wire_named("k")));
}

/* Instance `name` of module `child_id` inside module `parent_id` (no connections). */
typedef struct module_link {
    uint32_t parent, child; /* module IDs */
} module_link;

static void add_instance(module_link link, const char *name) {
    odin3_module *parent = odin3_module_get(fx.res.design, (odin3_module_id){link.parent});
    odin3_module *child = odin3_module_get(fx.res.design, (odin3_module_id){link.child});
    odin3_node_spec spec = {odin3_module_celltype(child), intern(name), {0}, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(parent, &spec, &node));
}

/* Keep spreads through a chain whose parents have the lower module IDs: top (1) > mid (2) > leaf
 * (3, a `keep` wire). The instances are added after the build (the text cannot refer forward). */
static void test_clean_keep_chain_forward(void) {
    build("module \\top\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"
          "module \\mid\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"
          "module \\leaf\n  wire input 1 \\a\n  wire output 2 \\y\n  attribute \\keep 1\n"
          "  wire \\w\n  cell $_NOT_ \\k\n    connect \\A \\a\n    connect \\Y \\w\n  end\n"
          "  cell $_NOT_ \\g\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    add_instance((module_link){2, 3}, "ul");
    add_instance((module_link){1, 2}, "um");
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(bool));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_keep_modules(fx.res.design, &keep));
    char text[8];
    flags_text(&keep, text);
    TEST_ASSERT_EQUAL_STRING("111", text);
    odin3_vec_free(&keep);
    run_clean();
    odin3_module *top = odin3_module_get(fx.res.design, (odin3_module_id){1});
    odin3_node_id um = odin3_module_find_node(top, intern("um"));
    TEST_ASSERT_TRUE(odin3_node_valid(um) && odin3_node_live(top, um)); /* top's instance stays */
}

/* A used non-FF net keeps its init, silently; a constant-driven net loses it whatever the value. */
static void test_clean_init_kept(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  attribute \\init 1'1\n  wire \\u\n  attribute \\init 1'1\n  wire \\c\n"
          "  connect \\c 1'1\n"
          "  cell $_NOT_ \\n1\n    connect \\A \\a\n    connect \\Y \\u\n  end\n"
          "  cell $_AND_ \\n2\n    connect \\A \\u\n    connect \\B \\c\n    connect \\Y \\y\n"
          "  end\nend\n");
    run_clean();
    TEST_ASSERT_EQUAL_UINT32(0, count_level(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("u")));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wnet("c")));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_changes);
}

/* Fixture E: init 0 on a net driven by 1'0 (the same value) goes too. */
static void test_clean_init_const_same_value(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  attribute \\init 1'0\n  wire \\c\n  connect \\c 1'0\n"
          "  cell $_AND_ \\n2\n    connect \\A \\a\n    connect \\B \\c\n    connect \\Y \\y\n"
          "  end\nend\n");
    run_clean();
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wnet("c")));
    TEST_ASSERT_EQUAL_UINT32(0, count_level(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_changes);
}

/* Counts the wire's defined init bits (BITS attribute `init`), 0 when it has none. */
static uint32_t wire_init_bits(const char *name) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(fx.res.design), odin3_bytes_cstr("init"), &key)) {
        return 0;
    }
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire_named(name).v};
    const odin3_value *val = odin3_attr_get(fx.module, obj, key);
    uint32_t bits = 0;
    for (uint32_t k = 0; val != NULL && val->kind == ODIN3_VAL_BITS && k < val->len; k++) {
        bits += val->bits[k] == ODIN3_BIT_0 || val->bits[k] == ODIN3_BIT_1 ? 1U : 0U;
    }
    return bits;
}

/* Fixture D: two names of one live FF's Q net both carry init 1; one membership keeps it. */
static void test_clean_init_alias_duplicate(void) {
    build("module \\m\n  wire input 1 \\c\n  wire input 2 \\d\n  wire output 3 \\y\n"
          "  attribute \\init 1'1\n  wire \\q\n  attribute \\init 1'1\n  wire \\q2\n"
          "  connect \\q2 \\q\n"
          "  cell $_DFF_P_ \\f\n    connect \\C \\c\n    connect \\D \\d\n    connect \\Q \\q\n  "
          "end\n"
          "  cell $_NOT_ \\n\n    connect \\A \\q2\n    connect \\Y \\y\n  end\nend\n");
    TEST_ASSERT_EQUAL_UINT32(wnet("q").v, wnet("q2").v);
    TEST_ASSERT_EQUAL_UINT32(2, wire_init_bits("q") + wire_init_bits("q2"));
    run_clean();
    TEST_ASSERT_EQUAL_UINT32(1, wire_init_bits("q") + wire_init_bits("q2"));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q")));
    TEST_ASSERT_EQUAL_UINT32(0, count_level(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_changes);
}

/* A dead non-FF node's driven net keeps its init (only a dead FF's Q init goes with it). */
static void test_clean_dead_nonff_keeps_init(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  attribute \\init 1'1\n  wire \\t\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\n"
          "  cell $_NOT_ \\dead\n    connect \\A \\a\n    connect \\Y \\t\n  end\nend\n");
    odin3_net_id net = wnet("t");
    run_clean();
    TEST_ASSERT_FALSE(alive("dead"));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, net));
    TEST_ASSERT_TRUE(odin3_wire_live(fx.module, wire_named("t"))); /* the init keeps the wire */
}

/* --- processes and constants -------------------------------------------------------------------
 */

static void test_clean_skips_processes(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\z\n  wire $tmp1\n"
          "  cell $or \\d\n    connect \\A \\a\n    connect \\B \\a\n    connect \\Y $tmp1\n  end\n"
          "  process $p\n    sync always\n      update \\z \\a\n  end\nend\n");
    run_clean();
    TEST_ASSERT_TRUE(alive("d"));
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_INFO, "opt_clean: module 'm' has processes; skipped"));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_cells);
}

/* Carry M-d: a constant node const_net made, and one canon left unconnected, are not cells. */
static void test_clean_constants_silent(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n");
    odin3_opt_ctx oc;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_ctx_init(&oc, &fx.ctx, fx.module, &fx.opts));
    oc.counts = &fx.counts;
    odin3_net_id c0 = {0};
    odin3_net_id c1 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&oc, ODIN3_BIT_0, &c0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&oc, ODIN3_BIT_1, &c1));
    odin3_node_id driver = odin3_pin_node(fx.module, odin3_net_driver(fx.module, c1));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_pin_disconnect(fx.module, odin3_net_driver(fx.module, c1))); /* Y unconnected */
    odin3_opt_ctx_free(&oc);
    run_clean();
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, driver));
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_cells);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.clean_changes);
    TEST_ASSERT_EQUAL_UINT32(0, count_level(ODIN3_LOG_DEBUG)); /* no "removing unused" line */
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.clean_nets); /* both constants' nets, left without pins */
}

static void test_clean_misuse(void) {
    build(BUF_TEXT(""));
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_clean_run(NULL, fx.res.design, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_clean_run(&fx.ctx, NULL, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_clean_run(&fx.ctx, fx.res.design, NULL, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, NULL));
}

/* --- out of memory -----------------------------------------------------------------------------
 */

#define OOM_TEXT                                                                                   \
    "module \\sub\n  wire input 1 \\a\n  wire output 2 \\y\n"                                      \
    "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"                    \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\c\n  wire output 3 \\y\n  wire \\t\n"        \
    "  wire \\dead\n  wire $tmp1\n  wire $tmp2\n  attribute \\init 1'1\n  wire \\q\n"              \
    "  cell $_BUF_ \\b\n    connect \\A \\a\n    connect \\Y \\t\n  end\n"                         \
    "  cell $_NOT_ \\n\n    connect \\A \\t\n    connect \\Y \\y\n  end\n"                         \
    "  cell $_DFF_P_ \\f\n    connect \\C \\c\n    connect \\D \\a\n    connect \\Q \\q\n  end\n"  \
    "  cell $or \\o\n    connect \\A \\a\n    connect \\B \\a\n    connect \\Y $tmp1\n  end\n"     \
    "  cell \\sub \\u\n    connect \\a \\a\n    connect \\y $tmp2\n  end\nend\n"

static void oom_run_once(long fail, bool *done) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full(); /* the IR stays valid whichever point failed */
}

static long oom_sweep(void) {
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    return failures;
}

static void test_clean_oom(void) {
    long failures = oom_sweep();
    TEST_ASSERT_TRUE(failures > 5);
    const char *gone[] = {"b", "o", "u", "f"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(alive(gone[i]), gone[i]);
    }
    printf("opt_clean OOM: %ld failing allocation points\n", failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_buf);
    RUN_TEST(test_clean_buf_keep);
    RUN_TEST(test_clean_liveness);
    RUN_TEST(test_clean_inout_root);
    RUN_TEST(test_clean_unused_instance_removed);
    RUN_TEST(test_clean_keep_instance_kept);
    RUN_TEST(test_clean_keep_inside_child);
    RUN_TEST(test_clean_keep_misuse);
    RUN_TEST(test_clean_blackbox);
    RUN_TEST(test_clean_mem);
    RUN_TEST(test_clean_memrd_memwr);
    RUN_TEST(test_clean_ff_init);
    RUN_TEST(test_clean_init_conflict);
    RUN_TEST(test_clean_wires);
    RUN_TEST(test_clean_purge);
    RUN_TEST(test_clean_purge_alias);
    RUN_TEST(test_clean_net_alias_survives);
    RUN_TEST(test_clean_init_kept);
    RUN_TEST(test_clean_init_const_same_value);
    RUN_TEST(test_clean_init_alias_duplicate);
    RUN_TEST(test_clean_dead_nonff_keeps_init);
    RUN_TEST(test_clean_public_alias_and_const_kept);
    RUN_TEST(test_clean_public_alias_and_const_purge);
    RUN_TEST(test_clean_purge_multibit);
    RUN_TEST(test_clean_keep_wire_root);
    RUN_TEST(test_clean_keep_chain_forward);
    RUN_TEST(test_clean_skips_processes);
    RUN_TEST(test_clean_constants_silent);
    RUN_TEST(test_clean_misuse);
    RUN_TEST(test_clean_oom);
    return UNITY_END();
}
