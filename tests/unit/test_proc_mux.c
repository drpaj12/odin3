/*
 * test_proc_mux.c — proc's mux phase and M1 (2E spec §6, §7, §14; PR-12, PR-13; plan Task 7):
 * each row of §6's table, the groups of an overlapping casez (Review Focus 3), parallel_case, the
 * full_case mask with nested switches, items assigning the default's value, the open-mux quirks
 * (internal risk §21 #1), $eq per item with x bits, non-constant items, snippet splitting, the
 * pattern cap of the grouping pools (W5b, checked equivalent by 1E simulation), the all-'-' item
 * appended in a parallel_case group (§20 #17), provenance, M1's order and drivers, and out of
 * memory.
 *
 * Each test feeds a body (proc_build) to the phase as step B does: odin3_proc_begin_lowering, the
 * node's deletion, the promotions recorded, odin3_proc_mux, then M1 (odin3_proc_apply_merges).
 * Before that, the fixture runs clean, rmdead, prune and arst on it, as yosys-phase --upto mux
 * does before proc_mux (proc_init has nothing to do); the tests marked "proc_mux alone" feed the
 * decoded body straight to the phase.
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il (several per test where named),
 * gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto mux <test>.il
 * or, for "proc_mux alone", with --passes "proc_mux" (Yosys 0.55, git sha1 60f126cd0; run
 * 2026-10-12). The cells are compared as a set with Yosys's (its creation order is a hash pool's,
 * §6); 2E's own order, asserted here, is the snippets' creation order and, per switch, a mux's
 * node made when no case can append to it any more. mux_parallel_all_dash has no Yosys result
 * (Yosys 0.55 asserts `ctrl_sig.size() == 1`); its values are checked by simulation against
 * Verilog's first-match order. A cell is listed as its type, its parameters, its ports' nets LSB
 * first and its full_case/parallel_case attributes; a net is named by its constant (0 1 x z), else
 * by the alphabetically first wire bit on it (`w` or `w[i]`), else as `c<k>` (`c<k>[i]`), the
 * output of listed cell k.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/budget.h"
#include "passes/proc/ppool.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_LINES = 32,
    LOG_TEXT = 512,
    MAX_CELLS = 24,
    DESC_TEXT = 512,
    NAME_TEXT = 64,
    OOM_LIMIT = 20000,
    CAP_BITS = 8,
    SIM_VECTORS = 64,
    SIM_SEED = 7,
};

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        levels[n_lines] = level;
        int len = snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
        TEST_ASSERT_TRUE(len >= 0 && len < LOG_TEXT);
    }
    n_lines++;
}

void setUp(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
    odin3_proc_test_set_hooks(NULL);
    odin3_proc_test_set_limits(NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_limits(NULL);
    odin3_budget_test_set_limit(0);
    odin3_log_set_sink(NULL, NULL);
}

/* Line `line` was logged at level and is not the IR check's. */
static bool line_is(size_t line, odin3_log_level level) {
    return levels[line] == level && strncmp(lines[line], "check:", strlen("check:")) != 0;
}

/* The lines logged at level, but the IR check's, each prefixed as the default sink prints them. */
static size_t lines_at(odin3_log_level level, const char *prefix, char out[][LOG_TEXT],
                       size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (!line_is(i, level)) {
            continue;
        }
        int len = count < max ? snprintf(out[count], LOG_TEXT, "%s%s", prefix, lines[i]) : 1;
        TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
        count++;
    }
    return count;
}

/* --- fixture: build, decode, the analysis phases; step B; the cells ------------------------ */

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_proc_mctx mctx;
    odin3_proc_stats stats;
    odin3_pw *pw;
    odin3_node_id cells[MAX_CELLS];
    uint32_t n_cells;
} fixture;

/* Whether the fixture runs clean, rmdead, prune and arst before the mux phase (--upto mux), or
 * feeds the decoded body straight to it (--passes "proc_mux"). */
typedef enum pre { PRE_UPTO_ARST, PRE_NONE } pre;

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

static void fx_open(fixture *fx, const char *text, pre phases) {
    memset(fx, 0, sizeof *fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx->res.design, odin3_bytes_cstr("proc"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx->res.design, name, &fx->ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->ctx, fx->res.module));
    fx->mctx.stats = &fx->stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vec_reserve(&fx->mctx.pws, 1));
    fx->pw = odin3_vec_push(&fx->mctx.pws);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", 0), fx->pw));
    if (phases == PRE_UPTO_ARST) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(fx->pw, 0, ODIN3_PW_CLEAN_WHOLE_TREE));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx->mctx, fx->pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_prune(&fx->mctx, fx->pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_arst(&fx->mctx, fx->pw));
    }
    n_lines = 0;
}

static void fx_close(fixture *fx) {
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* Step B up to the phase: the process's operation, its node deleted, its promotions recorded. */
static void fx_delete(fixture *fx) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx->mctx, fx->pw));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(fx->res.module, fx->pw->node));
    fx->pw->node = (odin3_node_id){0};
    for (size_t i = 0; i < fx->pw->promoted.len; i++) {
        const odin3_pw_action *act = odin3_vec_cat(&fx->pw->promoted, i);
        for (uint32_t k = 0; k < act->lhs.len; k++) {
            TEST_ASSERT_EQUAL_INT(
                ODIN3_OK, odin3_proc_connect(&fx->mctx, odin3_pw_sig_bits(fx->pw, act->lhs)[k],
                                             odin3_pw_sig_bits(fx->pw, act->rhs)[k]));
        }
    }
}

static bool is_listed(const odin3_celltype_def *def) {
    return def != NULL && strncmp(def->name, "$_CONST", strlen("$_CONST")) != 0 &&
           strncmp(def->name, "$port_", strlen("$port_")) != 0;
}

/* The module's live cells in node-ID order (creation order), without constants and ports. */
static void fx_list(fixture *fx) {
    const odin3_module *module = fx->res.module;
    fx->n_cells = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) &&
            is_listed(odin3_celltype_get(fx->res.design, odin3_node_type(module, node)))) {
            TEST_ASSERT_TRUE(fx->n_cells < MAX_CELLS);
            fx->cells[fx->n_cells++] = node;
        }
    }
}

/* The mux phase and M1; the cells listed and the design checked. */
static odin3_status fx_mux(fixture *fx) {
    odin3_status st = odin3_proc_mux(&fx->mctx, fx->pw);
    st = st == ODIN3_OK ? odin3_proc_apply_merges(&fx->mctx) : st;
    check_full(&fx->res);
    fx_list(fx);
    return st;
}

static void fx_lower(fixture *fx) {
    fx_delete(fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_mux(fx));
}

/* --- naming nets and cells ------------------------------------------------------------------ */

static const char *str_of(const fixture *fx, uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(fx->res.design), id);
}

/* `w` or `w[i]` for bit `bit` of wire wire. */
static void wirebit_name(const fixture *fx, odin3_wirebit wb, char *out) {
    const char *name = str_of(fx, odin3_wire_name(fx->res.module, wb.wire));
    int len = odin3_wire_width(fx->res.module, wb.wire) > 1
                  ? snprintf(out, NAME_TEXT, "%s[%u]", name, wb.bit)
                  : snprintf(out, NAME_TEXT, "%s", name);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
}

/* The alphabetically first wire bit on net into out; false when the net has none. */
static bool net_wire_name(const fixture *fx, odin3_net_id net, char *out) {
    const odin3_module *module = fx->res.module;
    char cand[NAME_TEXT];
    bool found = false;
    odin3_wirebit primary = odin3_net_primary(module, net);
    if (odin3_wire_valid(primary.wire)) {
        wirebit_name(fx, primary, out);
        found = true;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire)) {
            wirebit_name(fx, alias.wb, cand);
            if (!found || strcmp(cand, out) < 0) {
                memcpy(out, cand, NAME_TEXT);
                found = true;
            }
        }
    }
    return found;
}

/* `c<index>` or `c<index>[bit]` when listed cell index drives net; false otherwise. */
static bool cell_drives(const fixture *fx, uint32_t index, odin3_net_id net, char *out) {
    const odin3_module *module = fx->res.module;
    odin3_pinslice pins = odin3_node_pins(module, fx->cells[index]);
    for (uint32_t pos = 0; pos < pins.count; pos++) {
        odin3_pin_id pin = {pins.first.v + pos};
        if (!odin3_pin_drives(module, pin) || odin3_pin_net(module, pin).v != net.v) {
            continue;
        }
        uint32_t width =
            odin3_node_port(module, fx->cells[index], odin3_pin_port(module, pin)).count;
        int len = width > 1 ? snprintf(out, NAME_TEXT, "c%u[%u]", index, odin3_pin_bit(module, pin))
                            : snprintf(out, NAME_TEXT, "c%u", index);
        TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
        return true;
    }
    return false;
}

/* The name of net as the output of a listed cell; false when no listed cell drives it. */
static bool net_cell_name(const fixture *fx, odin3_net_id net, char *out) {
    for (uint32_t k = 0; k < fx->n_cells; k++) {
        if (cell_drives(fx, k, net, out)) {
            return true;
        }
    }
    return false;
}

static void net_name(const fixture *fx, odin3_net_id net, char *out) {
    static const char *const k_consts[] = {"?", "0", "1", "x", "z"};
    odin3_const value = odin3_net_const_value(fx->res.module, net);
    if (value != ODIN3_CONST_NONE) {
        (void)snprintf(out, NAME_TEXT, "%s", k_consts[value]);
        return;
    }
    if (!net_wire_name(fx, net, out) && !net_cell_name(fx, net, out)) {
        (void)snprintf(out, NAME_TEXT, "n%u", net.v);
    }
}

/* Appends text to buf (DESC_TEXT bytes, NUL-terminated). */
static void put(char *buf, const char *text) {
    size_t used = strlen(buf);
    TEST_ASSERT_TRUE(used + strlen(text) < DESC_TEXT);
    memcpy(buf + used, text, strlen(text) + 1);
}

/* An attribute of the cell's node, rendered " name=<int>" when set. */
static void put_attr(const fixture *fx, odin3_node_id node, const char *key, char *buf) {
    uint32_t str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(fx->res.design), odin3_bytes_cstr(key), &str)) {
        return;
    }
    const odin3_value *value =
        odin3_attr_get(fx->res.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, str);
    if (value != NULL) {
        char text[NAME_TEXT];
        (void)snprintf(text, NAME_TEXT, " %s=%lld", key, (long long)value->i);
        put(buf, text);
    }
}

/* Listed cell index: type, parameters, ports (nets LSB first), full_case/parallel_case. */
static void describe(const fixture *fx, uint32_t index, char *buf) {
    const odin3_module *module = fx->res.module;
    odin3_node_id node = fx->cells[index];
    const odin3_celltype_def *def =
        odin3_celltype_get(fx->res.design, odin3_node_type(module, node));
    char text[NAME_TEXT];
    buf[0] = '\0';
    put(buf, def->name);
    for (uint32_t i = 0; i < def->n_params; i++) {
        (void)snprintf(text, NAME_TEXT, " %s=%lld", def->params[i].name,
                       (long long)odin3_node_param(module, node, i)->i);
        put(buf, text);
    }
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice pins = odin3_node_port(module, node, port);
        (void)snprintf(text, NAME_TEXT, " %s=[", def->ports[port].name);
        put(buf, text);
        for (uint32_t bit = 0; bit < pins.count; bit++) {
            net_name(fx, odin3_pin_net(module, (odin3_pin_id){pins.first.v + bit}), text);
            put(buf, bit > 0 ? " " : "");
            put(buf, text);
        }
        put(buf, "]");
    }
    put_attr(fx, node, "full_case", buf);
    put_attr(fx, node, "parallel_case", buf);
}

/* The listed cells must read want[0 .. n), in order. */
static void expect_cells(const fixture *fx, const char *const *want, uint32_t n) {
    char got[DESC_TEXT];
    for (uint32_t k = 0; k < fx->n_cells && k < n; k++) {
        describe(fx, k, got);
        TEST_ASSERT_EQUAL_STRING(want[k], got);
    }
    TEST_ASSERT_EQUAL_UINT32(n, fx->n_cells);
}

/* The net of bit `bit` of the wire named name. */
static odin3_net_id wire_net(const fixture *fx, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(fx->res.design), odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(fx->res.module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(fx->res.module, wire, bit);
}

/* The net wire name (bit bit) is on must be called want. */
static void expect_net(const fixture *fx, const char *name, uint32_t bit, const char *want) {
    char got[NAME_TEXT];
    net_name(fx, wire_net(fx, name, bit), got);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

/* A test's text, its phases before mux, and the cells it must give. */
typedef struct mux_case {
    const char *text;
    pre phases;
    const char *const *cells;
    uint32_t n_cells;
} mux_case;

/* Lowers the text's process and checks its cells; the fixture stays open for more checks. */
static void mux_run(fixture *fx, mux_case test) {
    fx_open(fx, test.text, test.phases);
    fx_lower(fx);
    expect_cells(fx, test.cells, test.n_cells);
}

#define N_OF(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* --- §6's table and the grouping (Review Focus 3) -------------------------------------------- */

/* mux_if_else.il: `if (s) t1 = a; else t1 = b;` with root t0 <- t1, which prune promotes: one $mux,
 * no $eq (a 1-bit signal against 1 is the bit itself), and t0, t1 and the mux's Y one net. */
static const char *const k_if_else = "module \\m\n"
                                     "  wire input 1 \\s\n"
                                     "  wire input 2 \\a\n"
                                     "  wire input 3 \\b\n"
                                     "  wire output 4 \\y\n"
                                     "  wire \\t0\n"
                                     "  wire \\t1\n"
                                     "  process $p\n"
                                     "    assign \\t0 \\t1\n"
                                     "    switch \\s\n"
                                     "      case 1'1\n"
                                     "        assign \\t1 \\a\n"
                                     "      case\n"
                                     "        assign \\t1 \\b\n"
                                     "    end\n"
                                     "    sync always\n"
                                     "      update \\y \\t0\n"
                                     "  end\n"
                                     "end\n";

static void test_mux_if_else(void) {
    static const char *const k_cells[] = {"$mux WIDTH=1 A=[b] B=[a] S=[s] Y=[t0] full_case=1"};
    fixture fx;
    mux_run(&fx, (mux_case){k_if_else, PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "t0", 0).v, wire_net(&fx, "t1", 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_MUX]);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_EQ]);
    fx_close(&fx);
}

/* mux_if_clocked.il: a clocked `if (en) q <= d;`: the root default t0 = q is the mux's A. */
static void test_mux_if_clocked(void) {
    static const char *const k_cells[] = {"$mux WIDTH=1 A=[q] B=[d] S=[en] Y=[t0]"};
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire input 1 \\clk\n"
                            "  wire input 2 \\en\n"
                            "  wire input 3 \\d\n"
                            "  wire output 4 \\q\n"
                            "  wire \\t0\n"
                            "  process $p\n"
                            "    assign \\t0 \\q\n"
                            "    switch \\en\n"
                            "      case 1'1\n"
                            "        assign \\t0 \\d\n"
                            "    end\n"
                            "    sync posedge \\clk\n"
                            "      update \\q \\t0\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_case_pmux.il: a case with distinct constant items (simple parallel): one $eq per value, a
 * $reduce_or for the two-value item, one $pmux whose A is the default. */
#define CASE_PMUX_BODY(src_sw, src0, src1, src2)                                                   \
    "module \\m\n"                                                                                 \
    "  wire width 2 input 1 \\sel\n"                                                               \
    "  wire input 2 \\a\n"                                                                         \
    "  wire input 3 \\b\n"                                                                         \
    "  wire input 4 \\c\n"                                                                         \
    "  wire output 5 \\y\n"                                                                        \
    "  wire \\t1\n"                                                                                \
    "  process $p\n" src_sw "    switch \\sel\n" src0 "      case 2'00\n"                          \
    "        assign \\t1 \\a\n" src1 "      case 2'01, 2'10\n"                                     \
    "        assign \\t1 \\b\n" src2 "      case\n"                                                \
    "        assign \\t1 \\c\n"                                                                    \
    "    end\n"                                                                                    \
    "    sync always\n"                                                                            \
    "      update \\y \\t1\n"                                                                      \
    "  end\n"                                                                                      \
    "end\n"

#define EQ2(b0, b1, y)                                                                             \
    "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[sel[0] sel[1]] B=[" b0 " " b1      \
    "] Y=[" y "] full_case=1"

static const char *const k_case_pmux_cells[] = {
    EQ2("1", "0", "c0"),
    EQ2("0", "1", "c1"),
    "$reduce_or A_SIGNED=0 A_WIDTH=2 Y_WIDTH=1 A=[c0 c1] Y=[c2] full_case=1",
    EQ2("0", "0", "c3"),
    "$pmux WIDTH=1 S_WIDTH=2 A=[c] B=[b a] S=[c2 c3] Y=[t1] full_case=1",
};

static void test_mux_case_pmux(void) {
    fixture fx;
    mux_run(&fx, (mux_case){CASE_PMUX_BODY("", "", "", ""), PRE_UPTO_ARST, k_case_pmux_cells,
                            N_OF(k_case_pmux_cells)});
    TEST_ASSERT_EQUAL_UINT32(3, fx.stats.cells[ODIN3_PROC_K_EQ]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_PMUX]);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_MUX]);
    fx_close(&fx);
}

/* mux_casez_chain.il and mux_parallel_case.il: casez with overlapping items 2'1- and 2'-1 is a
 * priority chain of two $mux (no $eq: each kept compare is one bit against 1); with parallel_case
 * it is one $pmux (Review Focus 3). */
#define CASEZ_BODY(flag)                                                                           \
    "module \\m\n"                                                                                 \
    "  wire width 2 input 1 \\s\n"                                                                 \
    "  wire input 2 \\a\n"                                                                         \
    "  wire input 3 \\b\n"                                                                         \
    "  wire input 4 \\c\n"                                                                         \
    "  wire output 5 \\y\n"                                                                        \
    "  wire \\t1\n"                                                                                \
    "  process $p\n" flag "    switch \\s\n"                                                       \
    "      case 2'1-\n"                                                                            \
    "        assign \\t1 \\a\n"                                                                    \
    "      case 2'-1\n"                                                                            \
    "        assign \\t1 \\b\n"                                                                    \
    "      case\n"                                                                                 \
    "        assign \\t1 \\c\n"                                                                    \
    "    end\n"                                                                                    \
    "    sync always\n"                                                                            \
    "      update \\y \\t1\n"                                                                      \
    "  end\n"                                                                                      \
    "end\n"

static void test_mux_casez_chain(void) {
    static const char *const k_cells[] = {
        "$mux WIDTH=1 A=[c] B=[b] S=[s[0]] Y=[c0] full_case=1",
        "$mux WIDTH=1 A=[c0] B=[a] S=[s[1]] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){CASEZ_BODY(""), PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_EQ]);
    fx_close(&fx);
}

static void test_mux_parallel_case(void) {
    static const char *const k_cells[] = {
        "$pmux WIDTH=1 S_WIDTH=2 A=[c] B=[b a] S=[s[0] s[1]] Y=[t1] full_case=1 parallel_case=1"};
    fixture fx;
    mux_run(&fx, (mux_case){CASEZ_BODY("    attribute \\parallel_case 1\n"), PRE_UPTO_ARST, k_cells,
                            N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_full_case_mask.il: a full_case switch whose every case assigns t1 masks the root's t1 = q:
 * the $pmux's A is x. */
static void test_mux_full_case_mask(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=1 B_WIDTH=1 Y_WIDTH=1 A=[s] B=[0] Y=[c0] full_case=1",
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[b a] S=[s c0] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\b\n"
                            "  wire input 4 \\q\n"
                            "  wire output 5 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    assign \\t1 \\q\n"
                            "    attribute \\full_case 1\n"
                            "    switch \\s\n"
                            "      case 1'0\n"
                            "        assign \\t1 \\a\n"
                            "      case 1'1\n"
                            "        assign \\t1 \\b\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/*
 * mux_full_nested.il and mux_full_nested_partial.il (§21 #1, the mask with nested switches): the
 * outer full_case switch's case 1'1 assigns t1 only through a nested full_case switch, which
 * counts (both muxes get A = x); a nested switch without full_case does not (A = q in both).
 * mux_full_nested_under_partial.il: after prune the mask matters only below a switch without
 * full_case, whose root default q prune keeps: the full_case switch s inside its case still gets A
 * = x, through its nested full_case switch r.
 */
#define NESTED_BODY(inner_flag, inner_cases)                                                       \
    "module \\m\n"                                                                                 \
    "  wire input 1 \\s\n"                                                                         \
    "  wire input 2 \\r\n"                                                                         \
    "  wire input 3 \\a\n"                                                                         \
    "  wire input 4 \\b\n"                                                                         \
    "  wire input 5 \\c\n"                                                                         \
    "  wire input 6 \\q\n"                                                                         \
    "  wire output 7 \\y\n"                                                                        \
    "  wire \\t1\n"                                                                                \
    "  process $p\n"                                                                               \
    "    assign \\t1 \\q\n"                                                                        \
    "    attribute \\full_case 1\n"                                                                \
    "    switch \\s\n"                                                                             \
    "      case 1'0\n"                                                                             \
    "        assign \\t1 \\a\n"                                                                    \
    "      case 1'1\n" inner_flag "        switch \\r\n" inner_cases "        end\n"               \
    "    end\n"                                                                                    \
    "    sync always\n"                                                                            \
    "      update \\y \\t1\n"                                                                      \
    "  end\n"                                                                                      \
    "end\n"

#define EQ1(sig, y)                                                                                \
    "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=1 B_WIDTH=1 Y_WIDTH=1 A=[" sig "] B=[0] Y=[" y              \
    "] full_case=1"

static void test_mux_full_case_nested(void) {
    static const char *const k_full[] = {
        EQ1("r", "c0"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[c b] S=[r c0] Y=[c1] full_case=1",
        EQ1("s", "c2"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[c1 a] S=[s c2] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){NESTED_BODY("        attribute \\full_case 1\n",
                                        "          case 1'0\n            assign \\t1 \\b\n"
                                        "          case 1'1\n            assign \\t1 \\c\n"),
                            PRE_UPTO_ARST, k_full, N_OF(k_full)});
    fx_close(&fx);
}

static void test_mux_full_case_nested_partial(void) {
    static const char *const k_partial[] = {
        "$mux WIDTH=1 A=[q] B=[b] S=[r] Y=[c0]",
        EQ1("s", "c1"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[q] B=[c0 a] S=[s c1] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){NESTED_BODY("", "          case 1'1\n            assign \\t1 \\b\n"),
                            PRE_UPTO_ARST, k_partial, N_OF(k_partial)});
    fx_close(&fx);
}

static void test_mux_full_case_under_partial(void) {
    fixture fx;
    static const char *const k_under[] = {
        EQ1("r", "c0"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[c b] S=[r c0] Y=[c1] full_case=1",
        EQ1("s", "c2"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[c1 a] S=[s c2] Y=[c3] full_case=1",
        "$mux WIDTH=1 A=[q] B=[c3] S=[e] Y=[t1]",
    };
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire input 1 \\e\n"
                            "  wire input 2 \\s\n"
                            "  wire input 3 \\r\n"
                            "  wire input 4 \\a\n"
                            "  wire input 5 \\b\n"
                            "  wire input 6 \\c\n"
                            "  wire input 7 \\q\n"
                            "  wire output 8 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    assign \\t1 \\q\n"
                            "    switch \\e\n"
                            "      case 1'1\n"
                            "        attribute \\full_case 1\n"
                            "        switch \\s\n"
                            "          case 1'0\n"
                            "            assign \\t1 \\a\n"
                            "          case 1'1\n"
                            "            attribute \\full_case 1\n"
                            "            switch \\r\n"
                            "              case 1'0\n"
                            "                assign \\t1 \\b\n"
                            "              case 1'1\n"
                            "                assign \\t1 \\c\n"
                            "            end\n"
                            "        end\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_under, N_OF(k_under)});
    fx_close(&fx);
}

/* mux_items_eq_default.il (Review Focus 3, the `value == A` skip): item 2'00 assigns the default's
 * value, so it builds no $eq and no stage. */
static void test_mux_items_eq_default(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c0] "
        "full_case=1",
        "$mux WIDTH=1 A=[c] B=[a] S=[c0] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\c\n"
                            "  wire output 4 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'00\n"
                            "        assign \\t1 \\c\n"
                            "      case 2'01\n"
                            "        assign \\t1 \\a\n"
                            "      case\n"
                            "        assign \\t1 \\c\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/*
 * mux_open_across_boundary.il, proc_mux alone (internal risk §21 #1; with rmdead the 2'-- item
 * would remove 2'10): groups {00, 01}, {--}, {10}. 10 opens M; -- has an empty select, so a
 * becomes the result with no cell and M ends (its Y has no sinks; §20 #18: Yosys keeps M open and
 * appends 00 to it, losing b for s = 2'b00); 01's value a equals the result (no cell); 00 opens a
 * new mux on a. y1 = (s == 2'b00) ? b : a, Verilog's value.
 */
static void test_mux_open_across_boundary(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[0 1] Y=[c0]",
        "$mux WIDTH=1 A=[x] B=[c] S=[c0] Y=[c1]",
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[0 0] Y=[c2]",
        "$mux WIDTH=1 A=[a] B=[b] S=[c2] Y=[y1]",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\b\n"
                            "  wire input 4 \\c\n"
                            "  wire output 5 \\y\n"
                            "  wire \\y1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'00\n"
                            "        assign \\y1 \\b\n"
                            "      case 2'01\n"
                            "        assign \\y1 \\a\n"
                            "      case 2'--\n"
                            "        assign \\y1 \\a\n"
                            "      case 2'10\n"
                            "        assign \\y1 \\c\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\y1\n"
                            "  end\n"
                            "end\n",
                            PRE_NONE, k_cells, N_OF(k_cells)});
    odin3_net_id dead_y =
        odin3_pin_net(fx.res.module, odin3_node_port(fx.res.module, fx.cells[1], 3).first);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(fx.res.module, dead_y).count);
    fx_close(&fx);
}

/*
 * mux_open_boundary_nonconst.il (the same shape after rmdead, which keeps the non-constant item k
 * after 2'--: `casez (s) 2'b00: y = x0; 2'b01: y = a; 2'b??: y = a; k: y = b;`): k opens M; --
 * makes a the result and M ends (dead); 01's value is a (no cell); 00 opens a new mux on a, so t1 =
 * (s == 2'b00) ? x0 : a, Verilog's value. Yosys 0.55 appends 00 to M and gives t1 = a, wrong for s
 * = 2'b00 (§20 #18; `sat`, Yosys's `eval` and Icarus, 2E Task 7 review).
 */
static void test_mux_open_boundary_nonconst(void) {
    static const char *const k_cells[] = {
        ("$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[k[0] k[1]] "
         "Y=[c0] full_case=1"),
        "$mux WIDTH=1 A=[x] B=[b] S=[c0] Y=[c1] full_case=1",
        ("$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[0 0] Y=[c2] "
         "full_case=1"),
        "$mux WIDTH=1 A=[a] B=[x0] S=[c2] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire width 2 input 2 \\k\n"
                            "  wire input 3 \\x0\n"
                            "  wire input 4 \\a\n"
                            "  wire input 5 \\b\n"
                            "  wire output 6 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'00\n"
                            "        assign \\t1 \\x0\n"
                            "      case 2'01\n"
                            "        assign \\t1 \\a\n"
                            "      case 2'--\n"
                            "        assign \\t1 \\a\n"
                            "      case \\k\n"
                            "        assign \\t1 \\b\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    odin3_net_id dead_y =
        odin3_pin_net(fx.res.module, odin3_node_port(fx.res.module, fx.cells[1], 3).first);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(fx.res.module, dead_y).count);
    fx_close(&fx);
}

/*
 * mux_case_leaves_snippet.il (2E Task 7 review I1): `if (s) z = a; else begin y = b; z = b; end`
 * with y = c before it. Case 1'1 leaves y alone, so its value for y is the switch's initial value
 * c, not the running result b of the case after it: y = $mux(A = b, B = c, S = s).
 */
static void test_mux_case_leaves_snippet(void) {
    static const char *const k_cells[] = {
        "$mux WIDTH=1 A=[b] B=[c] S=[s] Y=[y] full_case=1",
        "$mux WIDTH=1 A=[b] B=[a] S=[s] Y=[z] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\b\n"
                            "  wire input 4 \\c\n"
                            "  wire output 5 \\y\n"
                            "  wire output 6 \\z\n"
                            "  process $p\n"
                            "    assign \\y \\c\n"
                            "    switch \\s\n"
                            "      case 1'1\n"
                            "        assign \\z \\a\n"
                            "      case\n"
                            "        assign \\y \\b\n"
                            "        assign \\z \\b\n"
                            "    end\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_dead_eq.il, proc_mux alone: `2'01, 2'--`: the $eq of 2'01 is made before 2'-- turns out
 * empty, so it stays with no sinks; no mux, and t1 is a. */
static void test_mux_dead_eq(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c0]"};
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire output 3 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'01, 2'--\n"
                            "        assign \\t1 \\a\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_NONE, k_cells, N_OF(k_cells)});
    expect_net(&fx, "t1", 0, "a");
    odin3_net_id eq_y =
        odin3_pin_net(fx.res.module, odin3_node_port(fx.res.module, fx.cells[0], 2).first);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(fx.res.module, eq_y).count);
    fx_close(&fx);
}

/* mux_x_compare.il: an x compare bit is not '-': it stays, as a pin on the x constant net. */
static void test_mux_x_compare(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 x] Y=[c0] "
        "full_case=1",
        "$mux WIDTH=1 A=[b] B=[a] S=[c0] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\b\n"
                            "  wire output 4 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'x1\n"
                            "        assign \\t1 \\a\n"
                            "      case\n"
                            "        assign \\t1 \\b\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_nonconst_item.il (2D C8): a non-constant item starts a group and puts the next case in a new
 * one, so each item is its own stage, its select a $eq of the constant signal and the item. */
static void test_mux_nonconst_item(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=1 B_WIDTH=1 Y_WIDTH=1 A=[1] B=[b] Y=[c0] full_case=1",
        "$mux WIDTH=4 A=[0 0 0 0] B=[0 1 0 0] S=[c0] Y=[c1[0] c1[1] c1[2] c1[3]] full_case=1",
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=1 B_WIDTH=1 Y_WIDTH=1 A=[1] B=[a] Y=[c2] full_case=1",
        ("$mux WIDTH=4 A=[c1[0] c1[1] c1[2] c1[3]] B=[1 0 0 0] S=[c2] Y=[t1[0] t1[1] t1[2] t1[3]] "
         "full_case=1"),
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire input 1 \\a\n"
                            "  wire input 2 \\b\n"
                            "  wire width 4 output 3 \\y\n"
                            "  wire width 4 \\t1\n"
                            "  process $p\n"
                            "    switch 1'1\n"
                            "      case \\a\n"
                            "        assign \\t1 4'0001\n"
                            "      case \\b\n"
                            "        assign \\t1 4'0010\n"
                            "      case\n"
                            "        assign \\t1 4'0000\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_nonconst_then_const.il: a non-constant item puts the next case in a new group even when
 * the pool (which took nothing) holds its value: a chain of two $mux, not a $pmux. */
static void test_mux_nonconst_then_const(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c0] "
        "full_case=1",
        "$mux WIDTH=1 A=[c] B=[b] S=[c0] Y=[c1] full_case=1",
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[k[0] k[1]] "
        "Y=[c2] "
        "full_case=1",
        "$mux WIDTH=1 A=[c1] B=[a] S=[c2] Y=[t1] full_case=1",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire width 2 input 2 \\k\n"
                            "  wire input 3 \\a\n"
                            "  wire input 4 \\b\n"
                            "  wire input 5 \\c\n"
                            "  wire output 6 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case \\k\n"
                            "        assign \\t1 \\a\n"
                            "      case 2'01\n"
                            "        assign \\t1 \\b\n"
                            "      case\n"
                            "        assign \\t1 \\c\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* mux_repeated_value.il, proc_mux alone (rmdead would remove the repeat): a value listed twice
 * makes the switch not simple parallel, so the pool puts the second 2'01 in a new group: a chain
 * of two $mux, not one $pmux. */
static void test_mux_repeated_value(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c0]",
        "$mux WIDTH=1 A=[c] B=[b] S=[c0] Y=[c1]",
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c2]",
        "$mux WIDTH=1 A=[c1] B=[a] S=[c2] Y=[t1]",
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire input 2 \\a\n"
                            "  wire input 3 \\b\n"
                            "  wire input 4 \\c\n"
                            "  wire output 5 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    switch \\s\n"
                            "      case 2'01\n"
                            "        assign \\t1 \\a\n"
                            "      case 2'01\n"
                            "        assign \\t1 \\b\n"
                            "      case\n"
                            "        assign \\t1 \\c\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_NONE, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* --- snippets --------------------------------------------------------------------------------- */

/* The nets of the wires named by names, one bit each (bit 0), as stored bits. */
static odin3_pw_bit bit_of(const fixture *fx, const char *name) {
    return wire_net(fx, name, 0).v;
}

/*
 * mux_snips_partition.il: root `{c b a} <- {i2 i1 i0}` and `d <- i0`, then in a case `{a c} <-
 * {j1 j0}` (bits c, a), `d <- j0` and `{a c} <- {i1 i2}`. Inserting {c a} splits {a b c} at c
 * ({a b}, {c}) and then {a b} at a ({a}, {b}); d and {a c} change nothing more. Snippets in
 * creation order: d, c, a, b. The switch cache has d, c and a; each case's last action assigning
 * a snippet gives its rhs slice.
 */
static void test_mux_snips_partition(void) {
    fixture fx;
    fx_open(&fx,
            "module \\m\n"
            "  wire input 1 \\e\n  wire input 2 \\i0\n  wire input 3 \\i1\n  wire input 4 \\i2\n"
            "  wire input 5 \\j0\n  wire input 6 \\j1\n  wire output 7 \\y\n"
            "  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n"
            "  process $p\n"
            "    assign { \\c \\b \\a } { \\i2 \\i1 \\i0 }\n"
            "    assign \\d \\i0\n"
            "    switch \\e\n"
            "      case 1'1\n"
            "        assign { \\a \\c } { \\j1 \\j0 }\n"
            "        assign \\d \\j0\n"
            "        assign { \\a \\c } { \\i1 \\i2 }\n"
            "    end\n"
            "    sync always\n"
            "      update \\y \\d\n"
            "  end\n"
            "end\n",
            PRE_NONE);
    odin3_proc_snips *snips = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_snips_build(&fx.mctx, fx.pw, &snips));
    static const char *const k_order[] = {"d", "c", "a", "b"};
    TEST_ASSERT_EQUAL_UINT32(N_OF(k_order), odin3_proc_snips_count(snips));
    for (uint32_t k = 0; k < N_OF(k_order); k++) {
        odin3_pw_sig sig = odin3_proc_snip(snips, k);
        TEST_ASSERT_EQUAL_UINT32(1, sig.len);
        TEST_ASSERT_EQUAL_UINT32(bit_of(&fx, k_order[k]), odin3_pw_sig_bits(fx.pw, sig)[0]);
    }
    const odin3_pw_case *root = odin3_vec_cat(&fx.pw->cases, 0);
    uint32_t sw = *(const uint32_t *)odin3_vec_cat(&root->switches, 0);
    uint32_t inner = *(const uint32_t *)odin3_vec_cat(
        &((const odin3_pw_switch *)odin3_vec_cat(&fx.pw->switches, sw))->cases, 0);
    static const bool k_touched[] = {true, true, true, false};
    static const char *const k_root_rhs[] = {"i0", "i2", "i0", "i1"};
    static const char *const k_case_rhs[] = {"j0", "i2", "i1", NULL};
    for (uint32_t k = 0; k < N_OF(k_order); k++) {
        odin3_pw_sig rhs = {0, 0};
        TEST_ASSERT_EQUAL(k_touched[k], odin3_proc_snips_touches(snips, sw, k));
        TEST_ASSERT_TRUE(odin3_proc_snips_assigned(snips, 0, k, &rhs));
        TEST_ASSERT_EQUAL_UINT32(bit_of(&fx, k_root_rhs[k]), odin3_pw_sig_bits(fx.pw, rhs)[0]);
        TEST_ASSERT_EQUAL(k_case_rhs[k] != NULL, odin3_proc_snips_assigned(snips, inner, k, &rhs));
        if (k_case_rhs[k] != NULL) {
            TEST_ASSERT_EQUAL_UINT32(bit_of(&fx, k_case_rhs[k]), odin3_pw_sig_bits(fx.pw, rhs)[0]);
        }
    }
    odin3_proc_snips_free(snips);
    fx_close(&fx);
}

/* mux_snippets_split.il, proc_mux alone (with prune, `t[3:2] <- p[3:2]` would be promoted first):
 * `t <- p` at the root and `t[1:0] <- r` in a case give snippets t[1:0] (the split's middle piece,
 * made first) and t[3:2], each with its own tree: a $mux for t[1:0], a plain connection for
 * t[3:2]. */
static void test_mux_snippets_split(void) {
    static const char *const k_cells[] = {
        "$mux WIDTH=2 A=[p[0] p[1]] B=[r[0] r[1]] S=[e] Y=[t[0] t[1]]"};
    static const char *const k_text = "module \\m\n"
                                      "  wire input 1 \\e\n"
                                      "  wire width 4 input 2 \\p\n"
                                      "  wire width 2 input 3 \\r\n"
                                      "  wire width 4 output 4 \\y\n"
                                      "  wire width 4 \\t\n"
                                      "  process $p\n"
                                      "    assign \\t \\p\n"
                                      "    switch \\e\n"
                                      "      case 1'1\n"
                                      "        assign \\t [1:0] \\r\n"
                                      "    end\n"
                                      "    sync always\n"
                                      "      update \\y \\t\n"
                                      "  end\n"
                                      "end\n";
    fixture fx;
    fx_open(&fx, k_text, PRE_NONE);
    odin3_proc_snips *snips = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_snips_build(&fx.mctx, fx.pw, &snips));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_proc_snips_count(snips));
    for (uint32_t k = 0; k < 2; k++) {
        odin3_pw_sig sig = odin3_proc_snip(snips, k);
        TEST_ASSERT_EQUAL_UINT32(2, sig.len);
        for (uint32_t i = 0; i < 2; i++) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "t", 2 * k + i).v,
                                     odin3_pw_sig_bits(fx.pw, sig)[i]);
        }
    }
    odin3_proc_snips_free(snips);
    fx_close(&fx);
    mux_run(&fx, (mux_case){k_text, PRE_NONE, k_cells, N_OF(k_cells)});
    expect_net(&fx, "t", 2, "p[2]");
    expect_net(&fx, "t", 3, "p[3]");
    fx_close(&fx);
}

/* --- the grouping cap (W5b) ------------------------------------------------------------------ */

/*
 * mux_pattern_cap_w5b.il: a 4-bit casez whose items group as {1--1, 01--}, {1-0-, 001-, 0001}
 * without the cap. Under a cap of 8 pattern bits every take passes the cap, so each item ends its
 * group: more stages, the same values (1E simulation, 64 random vectors), and W5b once.
 */
static const char *const k_cap_text = "module \\m\n"
                                      "  wire width 4 input 1 \\s\n"
                                      "  wire width 2 input 2 \\a\n"
                                      "  wire width 2 input 3 \\b\n"
                                      "  wire width 2 input 4 \\c\n"
                                      "  wire width 2 input 5 \\d\n"
                                      "  wire width 2 input 6 \\e\n"
                                      "  wire width 2 input 7 \\f\n"
                                      "  wire width 2 output 8 \\y\n"
                                      "  process $p\n"
                                      "    assign \\y \\f\n"
                                      "    switch \\s\n"
                                      "      case 4'1--1\n"
                                      "        assign \\y \\a\n"
                                      "      case 4'01--\n"
                                      "        assign \\y \\b\n"
                                      "      case 4'1-0-\n"
                                      "        assign \\y \\c\n"
                                      "      case 4'001-\n"
                                      "        assign \\y \\d\n"
                                      "      case 4'0001\n"
                                      "        assign \\y \\e\n"
                                      "    end\n"
                                      "  end\n"
                                      "end\n";

/* SIM_VECTORS vectors: out[v] is output port 0 after vector v, in[v] the input bits in port then
 * bit order. */
typedef struct sim_trace {
    uint64_t out[SIM_VECTORS];
    uint64_t in[SIM_VECTORS];
} sim_trace;

/* Output port 0 of sim as an integer. */
static uint64_t output_word(const odin3_sim *sim) {
    uint64_t word = 0;
    for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, 0); bit++) {
        bool value = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){0, bit}, &value));
        word |= (uint64_t)value << bit;
    }
    return word;
}

/* Every input bit of sim, in port then bit order, as an integer. */
static uint64_t input_word(const odin3_sim *sim) {
    uint64_t word = 0;
    uint32_t at = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim, port); bit++, at++) {
            bool value = false;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_input(sim, (odin3_sim_bit){port, bit}, &value));
            word |= (uint64_t)value << at;
        }
    }
    return word;
}

/* Simulates the fixture's module for SIM_VECTORS random vectors (seed SIM_SEED) into *trace. */
static void simulate(fixture *fx, sim_trace *trace) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_sim_build(fx->res.design, odin3_module_id_of(fx->res.module), &sim));
    odin3_prng prng;
    odin3_prng_seed(&prng, SIM_SEED);
    for (uint32_t vec = 0; vec < SIM_VECTORS; vec++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        trace->out[vec] = output_word(sim);
        trace->in[vec] = input_word(sim);
    }
    odin3_sim_destroy(sim);
}

static void test_mux_pattern_cap_w5b(void) {
    static sim_trace want;
    static sim_trace got;
    fixture fx;
    fx_open(&fx, k_cap_text, PRE_UPTO_ARST);
    fx_lower(&fx);
    uint32_t uncapped = fx.n_cells;
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.warnings[ODIN3_PROC_W5B]);
    TEST_ASSERT_EQUAL_UINT32(2, fx.stats.cells[ODIN3_PROC_K_PMUX]);
    simulate(&fx, &want);
    fx_close(&fx);
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_pattern_bits = CAP_BITS;
    odin3_proc_test_set_limits(&limits);
    fx_open(&fx, k_cap_text, PRE_UPTO_ARST);
    fx_lower(&fx);
    char warns[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 2));
    TEST_ASSERT_EQUAL_STRING("odin3: warning: proc: module 'm': case analysis needs more than 8 "
                             "pattern bits; the remaining items of this case statement are not "
                             "grouped into a $pmux",
                             warns[0]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W5B]);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_PMUX]);
    TEST_ASSERT_TRUE(fx.n_cells > uncapped);
    simulate(&fx, &got);
    TEST_ASSERT_EQUAL_UINT64_ARRAY(want.in, got.in, SIM_VECTORS);
    TEST_ASSERT_EQUAL_UINT64_ARRAY(want.out, got.out, SIM_VECTORS);
    fx_close(&fx);
    /* With a record on the switch, W5b is located there (§15: at the switch's record). */
    char text[LOG_TEXT * 2];
    const char *needle = "    switch \\s\n";
    const char *at = strstr(k_cap_text, needle);
    TEST_ASSERT_NOT_NULL(at);
    int len = snprintf(text, sizeof text, "%.*s    attribute \\src \"t.v:9.5\"\n%s",
                       (int)(at - k_cap_text), k_cap_text, at);
    TEST_ASSERT_TRUE(len > 0 && len < (int)sizeof text);
    fx_open(&fx, text, PRE_UPTO_ARST);
    fx_lower(&fx);
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 2));
    TEST_ASSERT_EQUAL_STRING("odin3: warning: t.v:9:5: warning: case analysis needs more than 8 "
                             "pattern bits; the remaining items of this case statement are not "
                             "grouped into a $pmux",
                             warns[0]);
    fx_close(&fx);
}

/* --- the all-'-' item in a parallel_case group (§6, §20 #17) and a default appended ---------- */

/* `(* parallel_case *) casez (s) [2'b00: y = c;] 2'b??: y = a; k: y = b; endcase`: rmdead keeps
 * the non-constant item k after the item matching every value (it removes only constant items), so
 * the reverse walk opens a mux for k and the 2'-- item reaches the append with an empty select,
 * where Yosys 0.55 asserts (`ctrl_sig.size() == 1`, proc_mux.cc:264; both texts, --upto mux). */
#define ALL_DASH_BODY(first)                                                                       \
    "module \\m\n"                                                                                 \
    "  wire width 2 input 1 \\s\n"                                                                 \
    "  wire width 2 input 2 \\k\n"                                                                 \
    "  wire input 3 \\a\n"                                                                         \
    "  wire input 4 \\b\n"                                                                         \
    "  wire input 5 \\c\n"                                                                         \
    "  wire output 6 \\y\n"                                                                        \
    "  process $p\n"                                                                               \
    "    assign \\y 1'0\n"                                                                         \
    "    attribute \\parallel_case 1\n"                                                            \
    "    switch \\s\n" first "      case 2'--\n"                                                   \
    "        assign \\y \\a\n"                                                                     \
    "      case \\k\n"                                                                             \
    "        assign \\y \\b\n"                                                                     \
    "    end\n"                                                                                    \
    "  end\n"                                                                                      \
    "end\n"

#define PAR_FLAGS " full_case=1 parallel_case=1"

/* Input bits in port then bit order: s 0-1, k 2-3, a 4, b 5, c 6. */
enum { IN_S = 0, IN_A = 4, IN_C = 6, S_MASK = 3 };

/*
 * mux_parallel_all_dash.il and mux_parallel_all_dash_first.il: 2E lowers the empty-select append
 * as gen_mux lowers an empty select (the case's value becomes the result, no cell) and ends the
 * open mux (k's $mux, then dead), so the 2'00 item before it opens a new mux on that result:
 * Verilog's first-match values, y = (s == 2'b00) ? c : a, and y = a without the 2'00 item.
 */
static void test_mux_parallel_all_dash(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[k[0] k[1]] "
        "Y=[c0]" PAR_FLAGS,
        "$mux WIDTH=1 A=[x] B=[b] S=[c0] Y=[c1]" PAR_FLAGS,
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[0 0] "
        "Y=[c2]" PAR_FLAGS,
        "$mux WIDTH=1 A=[a] B=[c] S=[c2] Y=[y]" PAR_FLAGS,
    };
    static sim_trace trace;
    fixture fx;
    mux_run(&fx, (mux_case){ALL_DASH_BODY("      case 2'00\n        assign \\y \\c\n"),
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    odin3_net_id dead =
        odin3_pin_net(fx.res.module, odin3_node_port(fx.res.module, fx.cells[1], 3).first);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(fx.res.module, dead).count);
    simulate(&fx, &trace);
    for (uint32_t vec = 0; vec < SIM_VECTORS; vec++) {
        uint64_t in = trace.in[vec];
        uint64_t want = ((in >> IN_S) & S_MASK) == 0 ? (in >> IN_C) & 1 : (in >> IN_A) & 1;
        TEST_ASSERT_EQUAL_UINT64(want, trace.out[vec]);
    }
    fx_close(&fx);
    mux_run(&fx, (mux_case){ALL_DASH_BODY(""), PRE_UPTO_ARST, k_cells, 2});
    expect_net(&fx, "y", 0, "a");
    fx_close(&fx);
}

/* par_default_mid.il: a default before a non-constant item of a parallel_case switch is appended
 * to k's mux with no compare at all: its select is 0 (Yosys builds a $reduce_or of zero bits,
 * which the IR does not have; the values are the same: the slice is never selected). */
static void test_mux_default_appended(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[k[0] k[1]] "
        "Y=[c0]" PAR_FLAGS,
        "$pmux WIDTH=1 S_WIDTH=2 A=[x] B=[b a] S=[c0 0] Y=[t1]" PAR_FLAGS,
    };
    fixture fx;
    mux_run(&fx, (mux_case){"module \\m\n"
                            "  wire width 2 input 1 \\s\n"
                            "  wire width 2 input 2 \\k\n"
                            "  wire input 3 \\a\n"
                            "  wire input 4 \\b\n"
                            "  wire output 5 \\y\n"
                            "  wire \\t1\n"
                            "  process $p\n"
                            "    attribute \\parallel_case 1\n"
                            "    switch \\s\n"
                            "      case\n"
                            "        assign \\t1 \\a\n"
                            "      case \\k\n"
                            "        assign \\t1 \\b\n"
                            "    end\n"
                            "    sync always\n"
                            "      update \\y \\t1\n"
                            "  end\n"
                            "end\n",
                            PRE_UPTO_ARST, k_cells, N_OF(k_cells)});
    fx_close(&fx);
}

/* --- provenance and M1 ----------------------------------------------------------------------- */

/* mux_provenance.il: mux_case_pmux.il with src attributes on the switch and its cases: the $pmux,
 * its $eqs and its $reduce_or share one DERIVED record whose parents are [switch, case 0, case 1,
 * case 2], in case order (§14); the nets they create take it too. */
static void test_mux_provenance(void) {
    fixture fx;
    mux_run(&fx, (mux_case){CASE_PMUX_BODY("    attribute \\src \"t.v:5.3\"\n",
                                           "      attribute \\src \"t.v:6.5\"\n",
                                           "      attribute \\src \"t.v:7.5\"\n",
                                           "      attribute \\src \"t.v:8.5\"\n"),
                            PRE_UPTO_ARST, k_case_pmux_cells, N_OF(k_case_pmux_cells)});
    const odin3_pw_case *root = odin3_vec_cat(&fx.pw->cases, 0);
    const odin3_pw_switch *sw =
        odin3_vec_cat(&fx.pw->switches, *(const uint32_t *)odin3_vec_cat(&root->switches, 0));
    odin3_prov_id parents[4] = {sw->prov};
    for (uint32_t i = 0; i < 3; i++) {
        parents[i + 1] = ((const odin3_pw_case *)odin3_vec_cat(
                              &fx.pw->cases, *(const uint32_t *)odin3_vec_cat(&sw->cases, i)))
                             ->prov;
    }
    odin3_prov_id id = odin3_node_prov(fx.res.module, fx.cells[4]);
    const odin3_prov_record *rec = odin3_prov_get(fx.res.design, id);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(4, rec->parents.count);
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(parents[i].v != 0 && parents[i].v != fx.pw->prov.v);
        TEST_ASSERT_EQUAL_UINT32(parents[i].v, rec->parents.ids[i].v);
    }
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(id.v, odin3_node_prov(fx.res.module, fx.cells[k]).v);
    }
    odin3_net_id eq_y =
        odin3_pin_net(fx.res.module, odin3_node_port(fx.res.module, fx.cells[0], 2).first);
    TEST_ASSERT_EQUAL_UINT32(id.v, odin3_net_prov(fx.res.module, eq_y).v);
    fx_close(&fx);
}

/* mux_m1.il: the promotions (t2 <- a, then t0 <- t1: prune's reverse order) are recorded before
 * the snippet's connection t1 <- the $mux's Y, and after M1 every net the process assigned has
 * exactly one driver. */
static void test_mux_m1(void) {
    fixture fx;
    fx_open(&fx,
            "module \\m\n"
            "  wire input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n"
            "  wire output 4 \\y\n  wire output 5 \\z\n"
            "  wire \\t0\n  wire \\t1\n  wire \\t2\n"
            "  process $p\n"
            "    assign \\t0 \\t1\n"
            "    assign \\t2 \\a\n"
            "    switch \\s\n"
            "      case 1'1\n        assign \\t1 \\a\n"
            "      case\n        assign \\t1 \\b\n"
            "    end\n"
            "    sync always\n      update \\y \\t0\n      update \\z \\t2\n"
            "  end\n"
            "end\n",
            PRE_UPTO_ARST);
    TEST_ASSERT_EQUAL_size_t(2, fx.pw->promoted.len);
    fx_delete(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mux(&fx.mctx, fx.pw));
    TEST_ASSERT_EQUAL_size_t(3, fx.mctx.pending.len);
    static const char *const k_lhs[] = {"t2", "t0", "t1"};
    static const char *const k_rhs[] = {"a", "t1", NULL};
    for (uint32_t i = 0; i < 3; i++) {
        const odin3_proc_conn *conn = odin3_vec_cat(&fx.mctx.pending, i);
        TEST_ASSERT_EQUAL_UINT32(bit_of(&fx, k_lhs[i]), conn->lhs);
        if (k_rhs[i] != NULL) {
            TEST_ASSERT_EQUAL_UINT32(bit_of(&fx, k_rhs[i]), conn->rhs);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx.mctx));
    check_full(&fx.res);
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(1,
                                 odin3_net_driver_count(fx.res.module, wire_net(&fx, k_lhs[i], 0)));
    }
    fx_list(&fx);
    char got[DESC_TEXT];
    describe(&fx, 0, got);
    TEST_ASSERT_EQUAL_STRING("$mux WIDTH=1 A=[b] B=[a] S=[s] Y=[t0] full_case=1", got);
    fx_close(&fx);
}

/* --- stops: E6, out of memory ----------------------------------------------------------------- */

/* Every net made since net_end has pins: a mux still open when the phase stopped left none. */
static void expect_no_new_pinless(const fixture *fx, uint32_t net_end) {
    for (uint32_t i = net_end; i < odin3_module_net_end(fx->res.module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(fx->res.module, net)) {
            TEST_ASSERT_TRUE(odin3_net_pins(fx->res.module, net).count > 0);
        }
    }
}

/* The byte budget (E6) stops the phase at the first mux's node, while that mux is open (casez
 * chain: its select needs no cell): ODIN3_ERR_PARSE, valid IR, its Y nets deleted. */
static void test_mux_budget_e6(void) {
    fixture fx;
    fx_open(&fx, CASEZ_BODY(""), PRE_UPTO_ARST);
    fx_delete(&fx);
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.budget_pins = 1;
    odin3_proc_test_set_limits(&limits);
    odin3_budget_test_set_limit(1);
    uint32_t net_end = odin3_module_net_end(fx.res.module);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_proc_mux(&fx.mctx, fx.pw));
    odin3_budget_test_set_limit(0);
    check_full(&fx.res);
    fx_list(&fx);
    TEST_ASSERT_EQUAL_UINT32(0, fx.n_cells);
    expect_no_new_pinless(&fx, net_end);
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_ERROR, "", errs, 2));
    TEST_ASSERT_NOT_NULL(strstr(errs[0], "proc needs more than 1 bytes"));
    fx_close(&fx);
}

/* mux_oom.il: snippet splits, a $pmux with a $reduce_or, a nested full_case switch, an x compare,
 * and an all-'-' item appended in a parallel_case group. */
static const char *const k_oom_text = "module \\m\n"
                                      "  wire width 2 input 1 \\s\n"
                                      "  wire width 2 input 2 \\k\n"
                                      "  wire input 3 \\r\n"
                                      "  wire width 4 input 4 \\p\n"
                                      "  wire width 2 input 5 \\a\n"
                                      "  wire width 2 input 6 \\b\n"
                                      "  wire width 4 output 7 \\y\n"
                                      "  wire output 8 \\v\n"
                                      "  wire width 4 \\t\n"
                                      "  wire \\u\n"
                                      "  process $p\n"
                                      "    assign \\t \\p\n"
                                      "    assign \\u \\r\n"
                                      "    switch \\s\n"
                                      "      case 2'00, 2'11\n"
                                      "        assign \\t [1:0] \\a\n"
                                      "      case 2'01\n"
                                      "        attribute \\full_case 1\n"
                                      "        switch \\r\n"
                                      "          case 1'0\n"
                                      "            assign \\t [3:1] { \\b \\r }\n"
                                      "          case 1'1\n"
                                      "            assign \\t [3:1] { \\a \\r }\n"
                                      "        end\n"
                                      "      case 2'x0\n"
                                      "        assign \\u \\p [3]\n"
                                      "    end\n"
                                      "    attribute \\parallel_case 1\n"
                                      "    switch \\s\n"
                                      "      case 2'--\n"
                                      "        assign \\u \\a [0]\n"
                                      "      case \\k\n"
                                      "        assign \\u \\b [0]\n"
                                      "    end\n"
                                      "    sync always\n"
                                      "      update \\y \\t\n"
                                      "      update \\v \\u\n"
                                      "  end\n"
                                      "end\n";

static char oom_want[MAX_CELLS][DESC_TEXT];
static uint32_t oom_cells;

/* One injection over the mux phase and M1: NO_MEMORY or the whole result, valid IR, no mux net
 * left pinless (ASan reports a leak at exit). True once it succeeds. */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom_text, PRE_UPTO_ARST);
    fx_delete(&fx);
    uint32_t net_end = odin3_module_net_end(fx.res.module);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_proc_mux(&fx.mctx, fx.pw);
    st = st == ODIN3_OK ? odin3_proc_apply_merges(&fx.mctx) : st;
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    check_full(&fx.res);
    fx_list(&fx);
    if (st == ODIN3_OK) {
        char got[DESC_TEXT];
        TEST_ASSERT_EQUAL_UINT32(oom_cells, fx.n_cells);
        for (uint32_t k = 0; k < oom_cells; k++) {
            describe(&fx, k, got);
            TEST_ASSERT_EQUAL_STRING(oom_want[k], got);
        }
    } else {
        expect_no_new_pinless(&fx, net_end);
    }
    fx_close(&fx);
    return st == ODIN3_OK;
}

static void test_mux_oom(void) {
    fixture fx;
    fx_open(&fx, k_oom_text, PRE_UPTO_ARST);
    fx_lower(&fx);
    oom_cells = fx.n_cells;
    TEST_ASSERT_TRUE(oom_cells >= 6);
    for (uint32_t k = 0; k < oom_cells; k++) {
        describe(&fx, k, oom_want[k]);
    }
    fx_close(&fx);
    bool done = false;
    long fail = 0;
    for (; fail < OOM_LIMIT && !done; fail++) {
        done = oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(fail > 1); /* at least one injection failed */
}

static void test_mux_invalid(void) {
    fixture fx;
    fx_open(&fx, k_if_else, PRE_NONE);
    odin3_proc_snips *snips = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_mux(NULL, fx.pw));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_mux(&fx.mctx, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_snips_build(&fx.mctx, NULL, &snips));
    TEST_ASSERT_NULL(snips);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_proc_snips_count(NULL));
    odin3_proc_snips_free(NULL);
    fx_close(&fx);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mux_if_else);
    RUN_TEST(test_mux_if_clocked);
    RUN_TEST(test_mux_case_pmux);
    RUN_TEST(test_mux_casez_chain);
    RUN_TEST(test_mux_parallel_case);
    RUN_TEST(test_mux_full_case_mask);
    RUN_TEST(test_mux_full_case_nested);
    RUN_TEST(test_mux_full_case_nested_partial);
    RUN_TEST(test_mux_full_case_under_partial);
    RUN_TEST(test_mux_items_eq_default);
    RUN_TEST(test_mux_open_across_boundary);
    RUN_TEST(test_mux_open_boundary_nonconst);
    RUN_TEST(test_mux_case_leaves_snippet);
    RUN_TEST(test_mux_dead_eq);
    RUN_TEST(test_mux_x_compare);
    RUN_TEST(test_mux_nonconst_item);
    RUN_TEST(test_mux_nonconst_then_const);
    RUN_TEST(test_mux_repeated_value);
    RUN_TEST(test_mux_snips_partition);
    RUN_TEST(test_mux_snippets_split);
    RUN_TEST(test_mux_pattern_cap_w5b);
    RUN_TEST(test_mux_parallel_all_dash);
    RUN_TEST(test_mux_default_appended);
    RUN_TEST(test_mux_provenance);
    RUN_TEST(test_mux_m1);
    RUN_TEST(test_mux_budget_e6);
    RUN_TEST(test_mux_oom);
    RUN_TEST(test_mux_invalid);
    return UNITY_END();
}
