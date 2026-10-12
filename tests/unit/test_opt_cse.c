/*
 * test_opt_cse.c — opt_merge (2F spec OP-6, §6, §20.5; plan Task 8): the key and its
 * normalisations, constants and x as values, FF init and -keepdc, -nomux, `keep`, cascades, hash
 * collisions, the classes that never merge, Yosys 0.55's own cases, provenance and out of memory.
 *
 * Yosys parity (0.55, the VTR build; `yosys -p "read_rtlil X.il; opt_merge [-keepdc]; write_rtlil"`
 * recorded in each tests/unit/data/opt/merge/X.expect): the same cells merge. Yosys walks its cell
 * dictionary in reverse creation order and so keeps the later of two equal cells; spec §6 keeps the
 * earlier, so the survivor's name and the Q a surviving init sits on differ (the keep_* cases
 * agree), and merge_yosys_cases compares cell types, init digits and, for keep_*, names.
 * Yosys compares the parameters exactly: `$reduce_or` over {a, a, b} and over {b, a} differ in
 * A_WIDTH and so never merge (the brief's example), unlike {a, a, b} against {b, a, b}.
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
#include "passes/opt/opt_test.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_MAX_LEN = 256, LINES_MAX = 64, OOM_LIMIT = 6000, CASE_BUF = 8192, MAX_CELLS = 32 };

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

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_module *module;
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
    odin3_opt_merge_test_set_key_hash(NULL);
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
    odin3_status st = pb_build(text, &fx.res);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        printf("build: %s\n", lines[i].text);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    odin3_status st = odin3_check_design(fx.res.design, opts);
    for (uint32_t i = 0; st != ODIN3_OK && i < n_lines; i++) {
        if (lines[i].level == ODIN3_LOG_ERROR) {
            printf("check: %s\n", lines[i].text);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

static void run_merge(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_merge_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

static bool alive(const char *name) {
    odin3_node_id node = node_named(name);
    return odin3_node_valid(node) && odin3_node_live(fx.module, node);
}

static odin3_net_id wnet(const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, 0);
}

/* The live nodes whose type is called type. */
static uint32_t count_type(const char *type) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node)) {
            const odin3_celltype_def *def =
                odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
            hits += strcmp(def->name, type) == 0 ? 1U : 0U;
        }
    }
    return hits;
}

static void expect_removed(uint32_t want) {
    char text[LINE_MAX_LEN];
    int len = snprintf(text, sizeof text, "opt_merge: removed %u cells", (unsigned)want);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof text);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_INFO, text));
    TEST_ASSERT_EQUAL_UINT32(want, fx.counts.merge_removed);
}

/* Two single-gate cells on a, b: type, then each cell's A and B (or A alone for a unary one). */
#define PAIR_TEXT(type, props1, conn1, props2, conn2)                                              \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"                    \
    "  wire output 4 \\z\n"                                                                        \
    "  cell " type " \\c1\n" props1 conn1 "    connect \\Y \\y\n  end\n"                           \
    "  cell " type " \\c2\n" props2 conn2 "    connect \\Y \\z\n  end\nend\n"

/* Whether the two cells of PAIR_TEXT were merged: their outputs are one net. */
static bool merged_pair(void) {
    return wnet("y").v == wnet("z").v;
}

static void run_pair(const char *text, bool want_merged) {
    build(text);
    run_merge();
    TEST_ASSERT_EQUAL_INT(want_merged ? 1 : 0, merged_pair() ? 1 : 0);
    expect_removed(want_merged ? 1U : 0U);
}

#define AB "    connect \\A \\a\n    connect \\B \\b\n"
#define BA "    connect \\A \\b\n    connect \\B \\a\n"

/* --- commutative, not commutative ------------------------------------------------------------ */

static void commutative(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"
          "  wire output 4 \\z\n  wire $tmp1\n"
          "  cell $and \\c5\n" AB "    connect \\Y \\y\n  end\n"
          "  cell $not \\c6\n    connect \\A \\a\n    connect \\Y $tmp1\n  end\n"
          "  cell $and \\c7\n" BA "    connect \\Y \\z\n  end\nend\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, node_named("c5"), 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, node_named("c7"), 0));
    run_merge();
    TEST_ASSERT_TRUE(merged_pair());
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$and")); /* the survivor, rebuilt */
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, (odin3_node_id){7}));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, (odin3_node_id){5}));
    expect_removed(1);
    TEST_ASSERT_EQUAL_UINT32(
        1, count_lines(ODIN3_LOG_DEBUG, "opt_merge: module 'm': '$c7' is identical to '$c5'"));
}

static void test_cse_commutative(void) {
    commutative();
}

static void not_commutative(void) {
    run_pair(PAIR_TEXT("$sub", "", AB, "", BA), false);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$sub"));
}

static void test_cse_not_commutative(void) {
    not_commutative();
}

/* --- sets and multisets ---------------------------------------------------------------------- */

#define RED_TEXT(type, wa, wb, sa, sb)                                                             \
    PAIR_TEXT(type, "    parameter \\A_WIDTH " wa "\n", "    connect \\A { " sa " }\n",            \
              "    parameter \\A_WIDTH " wb "\n", "    connect \\A { " sb " }\n")

static void test_cse_reduce_sets(void) {
    run_pair(RED_TEXT("$reduce_or", "3", "3", "\\a \\a \\b", "\\b \\a \\b"), true);
    tearDown();
    setUp();
    /* the A_WIDTH parameters differ: Yosys compares parameters exactly, so no merge */
    run_pair(RED_TEXT("$reduce_or", "3", "2", "\\a \\a \\b", "\\b \\a"), false);
    tearDown();
    setUp();
    run_pair(RED_TEXT("$reduce_xor", "3", "3", "\\a \\a \\b", "\\a \\b \\a"), true);
    tearDown();
    setUp();
    run_pair(RED_TEXT("$reduce_xor", "3", "3", "\\a \\a \\b", "\\a \\b \\b"), false);
}

/* --- $pmux ------------------------------------------------------------------------------------ */

#define PMUX_TEXT(bb, ss)                                                                          \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\c\n"                     \
    "  wire input 4 \\s0\n  wire input 5 \\s1\n  wire output 6 \\y\n  wire output 7 \\z\n"         \
    "  cell $pmux \\c1\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"                          \
    "    connect \\B { \\c \\b }\n    connect \\S { \\s1 \\s0 }\n    connect \\Y \\y\n"            \
    "  end\n  cell $pmux \\c2\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"                   \
    "    connect \\B { " bb " }\n    connect \\S { " ss " }\n    connect \\Y \\z\n  end\nend\n"

static void test_cse_pmux_pairs(void) {
    run_pair(PMUX_TEXT("\\b \\c", "\\s0 \\s1"), true); /* the pairs swapped */
    tearDown();
    setUp();
    run_pair(PMUX_TEXT("\\b \\c", "\\s1 \\s0"), false); /* B swapped, S not */
}

/* --- parameters ------------------------------------------------------------------------------- */

static void test_cse_params(void) {
    build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire width 4 output 3 \\y\n"
          "  wire width 5 output 4 \\z\n"
          "  cell $add \\c1\n    parameter \\Y_WIDTH 4\n" AB "    connect \\Y \\y\n  end\n"
          "  cell $add \\c2\n    parameter \\Y_WIDTH 5\n" AB "    connect \\Y \\z\n  end\nend\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$add"));
    expect_removed(0);
    tearDown();
    setUp();
    run_pair(
        PAIR_TEXT("$add", "    parameter \\A_SIGNED 0\n", AB, "    parameter \\A_SIGNED 1\n", AB),
        false);
}

/* --- constants -------------------------------------------------------------------------------- */

/* A constant $sop (WIDTH 0, one row "1") on the net of wire k2. */
static void add_const_sop(const char *wire) {
    static const uint8_t row[] = {'1'};
    const odin3_value params[2] = {odin3_value_int(0),
                                   {ODIN3_VAL_COVER, 0, row, (uint32_t)sizeof row, 0, 0}};
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$sop"), &type));
    odin3_net_id net = wnet(wire);
    odin3_netvec ports[2] = {{NULL, 0}, {&net, 1}};
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, params, 2};
    odin3_node_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(fx.module, &spec, ports, &out));
}

static void test_cse_const_operands(void) {
    build("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  wire output 3 \\z\n"
          "  wire \\k1\n  wire \\k2\n"
          "  cell $_CONST1_ \\one\n    connect \\Y \\k1\n  end\n"
          "  cell $and \\c1\n    connect \\A \\a\n    connect \\B \\k1\n    connect \\Y \\y\n"
          "  end\n"
          "  cell $and \\c2\n    connect \\A \\a\n    connect \\B \\k2\n    connect \\Y \\z\n"
          "  end\nend\n");
    add_const_sop("k2");
    run_merge();
    TEST_ASSERT_TRUE(merged_pair());
    expect_removed(1);
}

static void test_cse_x_is_a_value(void) {
    run_pair(PAIR_TEXT("$and", "", "    connect \\A \\a\n    connect \\B 1'x\n", "",
                       "    connect \\A \\a\n    connect \\B 1'x\n"),
             true);
    tearDown();
    setUp();
    run_pair(PAIR_TEXT("$and", "", "    connect \\A \\a\n    connect \\B 1'x\n", "",
                       "    connect \\A \\a\n    connect \\B 1'0\n"),
             false);
}

/* --- FFs ---------------------------------------------------------------------------------------
 */

#define FF_TEXT(init1, init2)                                                                      \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n" init1 "  wire output 3 \\y\n" init2   \
    "  wire output 4 \\z\n"                                                                        \
    "  cell $dff \\c1\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\d\n    connect \\Q \\y\n  end\n"                   \
    "  cell $dff \\c2\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\d\n    connect \\Q \\z\n  end\nend\n"

#define INIT1 "  attribute \\init 1'1\n"
#define INIT0 "  attribute \\init 1'0\n"

static void test_cse_ff_init(void) {
    run_pair(FF_TEXT(INIT1, INIT0), false);
    tearDown();
    setUp();
    run_pair(FF_TEXT("", ""), true);
    tearDown();
    setUp();
    fx.opts.keepdc = true;
    run_pair(FF_TEXT("", ""), false);
    tearDown();
    setUp();
    fx.opts.keepdc = true; /* fully defined inits merge under keepdc, and the init survives */
    run_pair(FF_TEXT(INIT1, INIT1), true);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("y")));
    tearDown();
    setUp();
    /* x against 1: the inits differ, so no merge */
    run_pair(FF_TEXT("", INIT1), false);
}

/* --- -nomux, keep ----------------------------------------------------------------------------- */

#define MUX_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\s\n"                     \
    "  wire output 4 \\y\n  wire output 5 \\z\n"                                                   \
    "  cell $mux \\c1\n    connect \\A \\a\n    connect \\B \\b\n    connect \\S \\s\n"            \
    "    connect \\Y \\y\n  end\n"                                                                 \
    "  cell $mux \\c2\n    connect \\A \\a\n    connect \\B \\b\n    connect \\S \\s\n"            \
    "    connect \\Y \\z\n  end\nend\n"

static void test_cse_nomux(void) {
    run_pair(MUX_TEXT, true);
    tearDown();
    setUp();
    fx.opts.nomux = true;
    run_pair(MUX_TEXT, false);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$mux"));
}

#define KEEP_TEXT(attr1, attr2)                                                                    \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"                    \
    "  wire output 4 \\z\n" attr1 "  cell $and \\c1\n" AB "    connect \\Y \\y\n  end\n" attr2     \
    "  cell $and \\c2\n" AB "    connect \\Y \\z\n  end\nend\n"

#define KEEP "  attribute \\keep 1\n"

static bool has_keep(const char *name) {
    odin3_objref ref = {ODIN3_OBJ_NODE, node_named(name).v};
    return odin3_attr_get(fx.module, ref, intern("keep")) != NULL;
}

static void expect_alive(const char *name, bool want) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(want ? 1 : 0, alive(name) ? 1 : 0, name);
}

static void test_cse_keep_swap(void) {
    build(KEEP_TEXT("", KEEP));
    run_merge();
    TEST_ASSERT_TRUE(merged_pair());
    expect_alive("c2", true); /* only the later one has keep: it survives */
    expect_alive("c1", false);
    TEST_ASSERT_TRUE(has_keep("c2")); /* the rebuild keeps it */
}

static void test_cse_keep_first(void) {
    build(KEEP_TEXT(KEEP, ""));
    run_merge();
    TEST_ASSERT_TRUE(alive("c1"));
    TEST_ASSERT_FALSE(alive("c2"));
}

static void test_cse_keep_both(void) {
    build(KEEP_TEXT(KEEP, KEEP));
    run_merge();
    TEST_ASSERT_TRUE(alive("c1") && alive("c2"));
    TEST_ASSERT_FALSE(merged_pair());
    expect_removed(0);
}

/* --- cascade, collisions -----------------------------------------------------------------------
 */

#define AND2(name, out)                                                                            \
    "  cell $and \\" name "\n    connect \\A \\a\n    connect \\B \\b\n    connect \\Y " out       \
    "\n  end\n"
#define NOT1(name, in, out)                                                                        \
    "  cell $not \\" name "\n    connect \\A " in "\n    connect \\Y " out "\n  end\n"
#define CASCADE_HEAD                                                                               \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"                    \
    "  wire output 4 \\z\n  wire \\t1\n  wire \\t2\n"

static void cascade(void) {
    build(CASCADE_HEAD AND2("a1", "\\t1") AND2("a2", "\\t2") NOT1("n1", "\\t1", "\\y")
              NOT1("n2", "\\t2", "\\z") "end\n");
    run_merge();
    TEST_ASSERT_TRUE(merged_pair());
    expect_removed(2);
    tearDown();
    setUp();
    /* the readers first in ID order: the second sweep finds them */
    build(CASCADE_HEAD NOT1("n1", "\\t1", "\\y") NOT1("n2", "\\t2", "\\z") AND2("a1", "\\t1")
              AND2("a2", "\\t2") "end\n");
    run_merge();
    TEST_ASSERT_TRUE(merged_pair());
    expect_removed(2);
}

static void test_cse_cascade(void) {
    cascade();
}

static uint64_t const_hash(const void *key, size_t len) {
    (void)key;
    (void)len;
    return 0;
}

static void test_cse_collision(void) {
    odin3_opt_merge_test_set_key_hash(const_hash);
    commutative();
    tearDown();
    setUp();
    odin3_opt_merge_test_set_key_hash(const_hash);
    not_commutative();
    tearDown();
    setUp();
    odin3_opt_merge_test_set_key_hash(const_hash);
    cascade();
}

/* --- what never merges -------------------------------------------------------------------------
 */

static void test_cse_never(void) {
    build("module \\sub\n  wire input 1 \\a\n  wire output 2 \\y\n"
          "  cell $_NOT_ \\n\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n"
          "module \\m\n  wire input 1 \\a\n  wire input 2 \\en\n  wire output 3 \\y1\n"
          "  wire output 4 \\y2\n  wire output 5 \\y3\n  wire output 6 \\y4\n  wire output 7 \\k1\n"
          "  wire output 8 \\k2\n  wire output 9 \\p1\n  wire output 10 \\p2\n"
          "  cell $tribuf \\t1\n    connect \\A \\a\n    connect \\EN \\en\n    connect \\Y \\y1\n "
          " end\n"
          "  cell $tribuf \\t2\n    connect \\A \\a\n    connect \\EN \\en\n    connect \\Y \\y2\n "
          " end\n"
          "  cell $mem \\m1\n  end\n  cell $mem \\m2\n  end\n"
          "  cell \\sub \\u1\n    connect \\a \\a\n    connect \\y \\y3\n  end\n"
          "  cell \\sub \\u2\n    connect \\a \\a\n    connect \\y \\y4\n  end\n"
          "  cell $_CONST1_ \\one\n    connect \\Y \\k1\n  end\n"
          "  cell $_CONST1_ \\uno\n    connect \\Y \\k2\n  end\n"
          "  process $p1\n    sync always\n      update \\p1 \\a\n  end\n"
          "  process $p2\n    sync always\n      update \\p2 \\a\n  end\nend\n");
    run_merge();
    expect_removed(0);
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$mem"));
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$proc"));
    TEST_ASSERT_TRUE(alive("u1") && alive("u2"));
}

/* --- Yosys 0.55's own cases --------------------------------------------------------------------
 */

static const char *const k_cases[] = {
    "basic_add_same",    "basic_add_diff",      "basic_xor_unary", "basic_rxor_sorted",
    "basic_ror_unified", "basic_rxor_multiset", "basic_gt_same",   "basic_gt_diff",
    "init_o_p",          "init_split_bits",     "init_one_side",   "init_keepdc_def",
    "init_keepdc_undef", "keep_first",          "keep_second",     "keep_both",
};

/* Reads DATA/<name><ext> into a NUL-terminated buffer (CASE_BUF bytes). */
static void read_case(const char *name, const char *ext, char *buf) {
    char path[LINE_MAX_LEN];
    int len = snprintf(path, sizeof path, "%s/%s%s", ODIN3_MERGE_DATA, name, ext);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, CASE_BUF - 1U, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < CASE_BUF - 1U, path);
    buf[got] = '\0';
}

static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp((const char *)lhs, (const char *)rhs);
}

enum { FIELD_LEN = 160, ITEM_LEN = 2 * FIELD_LEN + 2 };

typedef struct summary {
    char cells[MAX_CELLS][ITEM_LEN]; /* "type" (and " name" for keep_ cases), sorted */
    uint32_t n_cells;
    char inits[ITEM_LEN]; /* every init digit of every wire, sorted */
} summary;

static int cmp_char(const void *lhs, const void *rhs) {
    return *(const char *)lhs - *(const char *)rhs;
}

static void sort_chars(char *text) {
    qsort(text, strlen(text), 1, cmp_char);
}

static void store_cell(summary *out, const char *const fields[2], bool with_names) {
    const char *type = fields[0];
    const char *name = fields[1];
    TEST_ASSERT_TRUE(out->n_cells < MAX_CELLS);
    const char *shown = name[0] == '\\' ? name + 1 : name;
    (void)snprintf(out->cells[out->n_cells++], ITEM_LEN, "%s%s%s", type, with_names ? " " : "",
                   with_names ? shown : "");
}

static void store_init(summary *out, const char *value) {
    const char *digits = strchr(value, '\'');
    TEST_ASSERT_NOT_NULL(digits);
    TEST_ASSERT_TRUE(strlen(out->inits) + strlen(digits) < ITEM_LEN);
    (void)strcat(out->inits, digits + 1);
}

/* One .expect line: "cell TYPE NAME" or "init NAME N'bits". */
static void parse_line(const char *line, bool with_names, summary *out) {
    char type[FIELD_LEN];
    char name[FIELD_LEN];
    char value[FIELD_LEN];
    if (sscanf(line, "cell %159s %159s", type, name) == 2) {
        const char *const fields[2] = {type, name};
        store_cell(out, fields, with_names);
    } else if (sscanf(line, "init %159s %159s", name, value) == 2) {
        store_init(out, value);
    }
}

/* The .expect file: lines as above and a "# flags: ..." header. */
static void parse_expect(char *text, bool with_names, summary *out, bool *keepdc) {
    memset(out, 0, sizeof *out);
    *keepdc = strstr(text, "# flags: -keepdc") != NULL;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        parse_line(line, with_names, out);
    }
    qsort(out->cells, out->n_cells, ITEM_LEN, cmp_str);
    sort_chars(out->inits);
}

static void add_cells(bool with_names, summary *out) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        if (!odin3_node_live(fx.module, node) || def->gran == ODIN3_GRAN_PORT ||
            strncmp(def->name, "$_CONST", 7) == 0) {
            continue;
        }
        uint32_t name = odin3_node_name(fx.module, node);
        TEST_ASSERT_TRUE(out->n_cells < MAX_CELLS);
        const char *text = odin3_strtab_get(odin3_design_strtab(fx.res.design), name);
        (void)snprintf(out->cells[out->n_cells++], ITEM_LEN, "%s%s%s", def->name,
                       with_names ? " " : "", with_names ? text : "");
    }
}

static void add_inits(summary *out) {
    for (uint32_t id = 1; id < odin3_module_wire_end(fx.module); id++) {
        odin3_objref ref = {ODIN3_OBJ_WIRE, id};
        const odin3_value *init = odin3_attr_get(fx.module, ref, intern("init"));
        for (uint32_t bit = 0; init != NULL && bit < init->len; bit++) {
            size_t at = strlen(out->inits);
            TEST_ASSERT_TRUE(at + 1U < ITEM_LEN);
            out->inits[at] = "01xz"[init->bits[bit] & 3U];
        }
    }
}

static void summarize(bool with_names, summary *out) {
    memset(out, 0, sizeof *out);
    add_cells(with_names, out);
    qsort(out->cells, out->n_cells, ITEM_LEN, cmp_str);
    add_inits(out);
    sort_chars(out->inits);
}

static void run_yosys_case(const char *name) {
    static char text[CASE_BUF];
    static char expect[CASE_BUF];
    summary want;
    summary got;
    bool keepdc = false;
    read_case(name, ".il", text);
    read_case(name, ".expect", expect);
    bool with_names = strncmp(name, "keep_", 5) == 0;
    parse_expect(expect, with_names, &want, &keepdc);
    pb_free(&fx.res);
    build(text);
    fx.opts.keepdc = keepdc;
    run_merge();
    summarize(with_names, &got);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.n_cells, got.n_cells, name);
    for (uint32_t i = 0; i < want.n_cells; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.cells[i], got.cells[i], name);
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.inits, got.inits, name);
}

static void test_merge_yosys_cases(void) {
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        run_yosys_case(k_cases[i]);
        tearDown();
        setUp();
    }
}

/* --- provenance (point (p), §20.5) --------------------------------------------------------------
 */

#define PROV_TEXT                                                                                  \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire output 3 \\y\n"                    \
    "  wire output 4 \\z\n"                                                                        \
    "  attribute \\src \"t.v:3.1\"\n  cell $and \\c1\n" AB "    connect \\Y \\y\n  end\n"          \
    "  attribute \\src \"t.v:7.1\"\n  cell $and \\c2\n" BA "    connect \\Y \\z\n  end\nend\n"

/* True when hits holds a node of id with the given liveness. */
static bool has_hit(odin3_prov_hits hits, uint32_t id, bool live) {
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        if (hit->obj.kind == ODIN3_OBJ_NODE && hit->obj.id == id && hit->live == live) {
            return true;
        }
    }
    return false;
}

/* The survivor's record is DERIVED from [first, second]. */
static void expect_derived(odin3_node_id surv, odin3_prov_id first, odin3_prov_id second) {
    const odin3_prov_record *rec = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, surv));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(first.v, rec->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(second.v, rec->parents.ids[1].v);
}

/* The dropped node's line reaches the live survivor and the dead duplicate (§20.5). */
static void expect_index(odin3_node_id surv, odin3_node_id dropped) {
    odin3_prov_index *ix = odin3_prov_index_build(fx.res.design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_srcloc loc = {intern("t.v"), 7, 1, 0, 0, 0};
    odin3_prov_hits hits = odin3_prov_index_by_loc(ix, loc);
    bool live_surv = has_hit(hits, surv.v, true);
    bool dead_dup = has_hit(hits, dropped.v, false);
    odin3_prov_index_destroy(ix);
    TEST_ASSERT_TRUE(live_surv);
    TEST_ASSERT_TRUE(dead_dup);
}

static void test_cse_provenance(void) {
    build(PROV_TEXT);
    odin3_node_id first = node_named("c1");
    odin3_node_id second = node_named("c2");
    odin3_prov_id rec1 = odin3_node_prov(fx.module, first);
    odin3_prov_id rec2 = odin3_node_prov(fx.module, second);
    TEST_ASSERT_TRUE(rec1.v != 0 && rec2.v != 0 && rec1.v != rec2.v);
    run_merge();
    odin3_node_id surv = node_named("c1");
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, surv));
    TEST_ASSERT_TRUE(surv.v != first.v); /* rebuilt: a new node */
    expect_derived(surv, rec1, rec2);
    expect_index(surv, second);
}

/* --- out of memory ------------------------------------------------------------------------------
 */

#define OOM_TEXT                                                                                   \
    "module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n  wire input 3 \\clk\n"                   \
    "  wire input 4 \\s0\n  wire input 5 \\s1\n  wire output 6 \\y\n  wire output 7 \\z\n"         \
    "  wire output 8 \\q1\n  wire output 9 \\q2\n  wire output 10 \\p1\n  wire output 11 \\p2\n"   \
    "  cell $and \\c1\n" AB "    connect \\Y \\y\n  end\n"                                         \
    "  cell $and \\c2\n" BA "    connect \\Y \\z\n  end\n"                                         \
    "  cell $dff \\f1\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\a\n    connect \\Q \\q1\n  end\n"                  \
    "  cell $dff \\f2\n    parameter \\CLK_POLARITY 1\n    parameter \\WIDTH 1\n"                  \
    "    connect \\CLK \\clk\n    connect \\D \\a\n    connect \\Q \\q2\n  end\n"                  \
    "  cell $pmux \\p1c\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"                         \
    "    connect \\B { \\b \\a }\n    connect \\S { \\s1 \\s0 }\n    connect \\Y \\p1\n  end\n"    \
    "  cell $pmux \\p2c\n    parameter \\S_WIDTH 2\n    connect \\A \\a\n"                         \
    "    connect \\B { \\a \\b }\n    connect \\S { \\s0 \\s1 }\n    connect \\Y \\p2\n  "         \
    "end\nend\n"

static void oom_run_once(long fail, bool *done) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_merge_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    *done = st == ODIN3_OK;
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
}

static void test_cse_oom(void) {
    bool done = false;
    long failures = 0;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        oom_run_once(fail, &done);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(failures > 5);
    TEST_ASSERT_EQUAL_UINT32(3, fx.counts.merge_removed); /* and, dff, pmux */
    printf("opt_merge OOM: %ld failing allocation points\n", failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cse_commutative);
    RUN_TEST(test_cse_not_commutative);
    RUN_TEST(test_cse_reduce_sets);
    RUN_TEST(test_cse_pmux_pairs);
    RUN_TEST(test_cse_params);
    RUN_TEST(test_cse_const_operands);
    RUN_TEST(test_cse_x_is_a_value);
    RUN_TEST(test_cse_ff_init);
    RUN_TEST(test_cse_nomux);
    RUN_TEST(test_cse_keep_swap);
    RUN_TEST(test_cse_keep_first);
    RUN_TEST(test_cse_keep_both);
    RUN_TEST(test_cse_cascade);
    RUN_TEST(test_cse_collision);
    RUN_TEST(test_cse_never);
    RUN_TEST(test_merge_yosys_cases);
    RUN_TEST(test_cse_provenance);
    RUN_TEST(test_cse_oom);
    return UNITY_END();
}
