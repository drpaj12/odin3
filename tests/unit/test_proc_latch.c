/*
 * test_proc_latch.c — proc's latch phase (2E spec §8, §10, §14, §15; PR-14, PR-16; plan Task 8):
 * the verified example and --nolatches (Review Focus 2), quickcheck through a 2D ?: mux, set_undef
 * and readers, grouping by wire and rule, every fix-up rule, crossed feedback (no latch, as Yosys),
 * the snapshot against the live view (internal risk §21 #4), init removal (W2), the walk's memo on
 * a diamond chain of 30 shared muxes and its (net, set_undef) key (Review Focus 5, internal risk
 * #6), a combinational cycle, the internal error on a constant output, and out of memory.
 *
 * Each test feeds the module's bodies (proc_build) through what the pass runs before the latch
 * phase: decode, clean, rmdead, prune and arst per process, then step B per process
 * (odin3_proc_begin_lowering, the node's deletion, the promotions recorded, odin3_proc_mux, M1).
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il, gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto dlatch <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-12). The cells are compared as a set with Yosys's
 * (its creation order is a hash pool's); 2E's own order, asserted here, is each process's hold
 * cells, EN $not and $dlatch per latch group, then the muxes the fix-up re-created in its
 * breadth-first order. latch_diamond_30 is checked against Yosys at depth 8 (Yosys walks it
 * without a memo, 2^30 visits at depth 30): 47 cells, as the formula 4 + 6 (n - 1) + 1 gives.
 * latch_snapshot_vs_live, latch_memo_set_undef and latch_const_candidate change the IR by hand
 * between the phases, which RTLIL cannot say; they have no Yosys run. A cell is listed as in
 * test_proc_mux.c: its type, parameters, ports' nets LSB first and full_case/parallel_case; a net
 * by its constant (0 1 x z), else the alphabetically first wire bit on it, else `c<k>`, the output
 * of listed cell k.
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
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
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
#include <string.h>

enum {
    LOG_LINES = 32,
    LOG_TEXT = 512,
    MAX_CELLS = 256,
    MAX_PROCS = 4,
    DESC_TEXT = 512,
    NAME_TEXT = 64,
    OOM_LIMIT = 20000,
    MAX_EXPANDS = 64,
    DIAMOND = 30,
    DIAMOND_FREE = 5, /* the selects s1 .. s5 the evaluator varies */
    DIAMOND_TEXT = 16384,
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

/* The walk's expansions, recorded by the on_expand hook. */
typedef struct expand_log {
    odin3_net_id net[MAX_EXPANDS];
    bool set_undef[MAX_EXPANDS];
    uint32_t count;
    const odin3_module *module; /* for watch: the pin whose net is recorded at each expansion */
    odin3_pin_id watch;
    odin3_net_id watched[MAX_EXPANDS];
} expand_log;

static expand_log expands;

static void on_expand(void *user, odin3_net_id net, bool set_undef) {
    expand_log *log = user;
    TEST_ASSERT_TRUE(log->count < MAX_EXPANDS);
    log->net[log->count] = net;
    log->set_undef[log->count] = set_undef;
    if (log->module != NULL && log->watch.v != 0) {
        log->watched[log->count] = odin3_pin_net(log->module, log->watch);
    }
    log->count++;
}

static void install_expand_hook(void) {
    memset(&expands, 0, sizeof expands);
    odin3_proc_test_hooks hooks = {NULL, NULL, on_expand, &expands};
    odin3_proc_test_set_hooks(&hooks);
}

void setUp(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
    install_expand_hook();
    odin3_proc_test_set_limits(NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_hooks(NULL);
    odin3_proc_test_set_limits(NULL);
    odin3_budget_test_set_limit(0);
    odin3_log_set_sink(NULL, NULL);
}

/* The lines logged at level, each prefixed as the default sink prints them. */
static size_t lines_at(odin3_log_level level, const char *prefix, char out[][LOG_TEXT],
                       size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (levels[i] != level || strncmp(lines[i], "check:", strlen("check:")) == 0) {
            continue;
        }
        int len = count < max ? snprintf(out[count], LOG_TEXT, "%s%s", prefix, lines[i]) : 1;
        TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
        count++;
    }
    return count;
}

/* --- fixture: build, the analysis phases and step B per process; the latch phase ---------- */

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_proc_mctx mctx;
    odin3_proc_stats stats;
    odin3_node_id cells[MAX_CELLS];
    uint32_t n_cells;
} fixture;

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

static odin3_pw *fx_pw(fixture *fx, uint32_t index) {
    return odin3_vec_at(&fx->mctx.pws, index);
}

/* Builds text and runs decode, clean, rmdead, prune and arst on each process of module m. */
static void fx_open(fixture *fx, const char *text) {
    memset(fx, 0, sizeof *fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx->res.design, odin3_bytes_cstr("proc"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx->res.design, name, &fx->ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->ctx, fx->res.module));
    fx->mctx.stats = &fx->stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vec_reserve(&fx->mctx.pws, MAX_PROCS));
    for (uint32_t i = 0; odin3_node_valid(pb_proc(&fx->res, "m", i)); i++) {
        TEST_ASSERT_TRUE(i < MAX_PROCS);
        odin3_pw *pw = odin3_vec_push(&fx->mctx.pws);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", i), pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(pw, 0, ODIN3_PW_CLEAN_WHOLE_TREE));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx->mctx, pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_prune(&fx->mctx, pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_arst(&fx->mctx, pw));
    }
}

static void fx_close(fixture *fx) {
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* Step B for every process: its operation, its node deleted, its promotions, mux, M1. */
static void fx_step_b(fixture *fx) {
    for (uint32_t i = 0; i < fx->mctx.pws.len; i++) {
        odin3_pw *pw = fx_pw(fx, i);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_begin_lowering(&fx->mctx, pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(fx->res.module, pw->node));
        pw->node = (odin3_node_id){0};
        for (size_t j = 0; j < pw->promoted.len; j++) {
            const odin3_pw_action *act = odin3_vec_cat(&pw->promoted, j);
            for (uint32_t k = 0; k < act->lhs.len; k++) {
                TEST_ASSERT_EQUAL_INT(
                    ODIN3_OK, odin3_proc_connect(&fx->mctx, odin3_pw_sig_bits(pw, act->lhs)[k],
                                                 odin3_pw_sig_bits(pw, act->rhs)[k]));
            }
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mux(&fx->mctx, pw));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx->mctx));
    }
    check_full(&fx->res);
    n_lines = 0;
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

/* The latch phase; the design checked and the cells listed. */
static odin3_status fx_latch(fixture *fx) {
    odin3_status st = odin3_proc_latches(&fx->mctx);
    check_full(&fx->res);
    fx_list(fx);
    return st;
}

/* --- naming nets and cells (as test_proc_mux.c) ------------------------------------------- */

static const char *str_of(const fixture *fx, uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(fx->res.design), id);
}

static uint32_t str_id(const fixture *fx, const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(fx->res.design), odin3_bytes_cstr(text), &str));
    return str;
}

static void wirebit_name(const fixture *fx, odin3_wirebit wb, char *out) {
    const char *name = str_of(fx, odin3_wire_name(fx->res.module, wb.wire));
    int len = odin3_wire_width(fx->res.module, wb.wire) > 1
                  ? snprintf(out, NAME_TEXT, "%s[%u]", name, wb.bit)
                  : snprintf(out, NAME_TEXT, "%s", name);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
}

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

static bool net_cell_name(const fixture *fx, odin3_net_id net, char *out) {
    for (uint32_t index = 0; index < fx->n_cells; index++) {
        if (cell_drives(fx, index, net, out)) {
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

static void put(char *buf, const char *text) {
    size_t used = strlen(buf);
    TEST_ASSERT_TRUE(used + strlen(text) < DESC_TEXT);
    memcpy(buf + used, text, strlen(text) + 1);
}

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

static void expect_cells(const fixture *fx, const char *const *want, uint32_t n) {
    char got[DESC_TEXT];
    for (uint32_t k = 0; k < fx->n_cells && k < n; k++) {
        describe(fx, k, got);
        TEST_ASSERT_EQUAL_STRING(want[k], got);
    }
    TEST_ASSERT_EQUAL_UINT32(n, fx->n_cells);
}

static odin3_wire_id wire_named(const fixture *fx, const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(fx->res.module, str_id(fx, name));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return wire;
}

static odin3_net_id wire_net(const fixture *fx, const char *name, uint32_t bit) {
    return odin3_wire_net(fx->res.module, wire_named(fx, name), bit);
}

static odin3_node_id node_named(const fixture *fx, const char *name) {
    odin3_node_id node = odin3_module_find_node(fx->res.module, str_id(fx, name));
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    return node;
}

/* The live node of type `type` that drives net (none when there is none). */
static odin3_node_id driver_of(const fixture *fx, odin3_net_id net) {
    odin3_pin_id pin = odin3_net_driver(fx->res.module, net);
    return pin.v != 0 ? odin3_pin_node(fx->res.module, pin) : (odin3_node_id){0};
}

/* The first pin of port `port` of node. */
static odin3_pin_id port_pin(const fixture *fx, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(fx->res.module, node, port);
    TEST_ASSERT_TRUE(pins.count > 0);
    return pins.first;
}

/* Moves pin to the module's x constant net, as a pass could have since the database was built. */
static void move_to_x(fixture *fx, odin3_pin_id pin) {
    odin3_net_id xnet = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_const_net(&fx->mctx, ODIN3_PW_SX, &xnet));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(fx->res.module, pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(fx->res.module, pin, xnet));
}

static uint32_t count_type(const fixture *fx, const char *type) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < fx->n_cells; k++) {
        const odin3_celltype_def *def =
            odin3_celltype_get(fx->res.design, odin3_node_type(fx->res.module, fx->cells[k]));
        count += strcmp(def->name, type) == 0 ? 1U : 0U;
    }
    return count;
}

#define N_OF(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* $mux and $pmux port indices. */
enum { PORT_A = 0, PORT_B = 1, PORT_S = 2, PORT_Y = 3 };

/* --- the verified example and --nolatches (Review Focus 2) -------------------------------- */

/* latch_verified_example.il: the default-mode body of `always @* if (en) y = a;`. */
static const char *const k_verified = "module \\m\n"
                                      "  wire input 1 \\en\n"
                                      "  wire input 2 \\a\n"
                                      "  wire output 3 \\y\n"
                                      "  wire \\t0\n"
                                      "  wire \\t1\n"
                                      "  process $p\n"
                                      "    assign \\t0 \\t1\n"
                                      "    switch \\en\n"
                                      "      case 1'1\n"
                                      "        assign \\t1 \\a\n"
                                      "      case\n"
                                      "        assign \\t1 \\y\n"
                                      "    end\n"
                                      "    sync always\n"
                                      "      update \\y \\t0\n"
                                      "  end\n"
                                      "end\n";

#define NOT1(a, y) "$not A_SIGNED=0 A_WIDTH=1 Y_WIDTH=1 A=[" a "] Y=[" y "]"
#define OR1(a, y) "$reduce_or A_SIGNED=0 A_WIDTH=1 Y_WIDTH=1 A=[" a "] Y=[" y "]"
#define AND1(a, b, y)                                                                              \
    "$and A_SIGNED=0 B_SIGNED=0 A_WIDTH=1 B_WIDTH=1 Y_WIDTH=1 A=[" a "] B=[" b "] Y=[" y "]"
#define DLATCH(w, en, d, q) "$dlatch WIDTH=" w " EN_POLARITY=1 EN=[" en "] D=[" d "] Q=[" q "]"

/* The verified example: $not(en), $reduce_or(1'1), their $and, its $reduce_or, the EN $not, the
 * $dlatch, then the mux the fix-up re-created (A := the valid B slice, the y input having moved
 * to x), with its old record and rmdead's full_case. */
static void test_latch_verified_example(void) {
    static const char *const k_cells[] = {NOT1("en", "c0"),
                                          OR1("1", "c1"),
                                          AND1("c0", "c1", "c2"),
                                          OR1("c2", "c3"),
                                          NOT1("c3", "c4"),
                                          DLATCH("1", "c4", "t0", "y"),
                                          "$mux WIDTH=1 A=[a] B=[a] S=[en] Y=[t0] full_case=1"};
    fixture fx;
    fx_open(&fx, k_verified);
    fx_step_b(&fx);
    odin3_prov_id mux_prov = odin3_node_prov(fx.res.module, driver_of(&fx, wire_net(&fx, "t0", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    for (uint32_t k = 0; k < 6; k++) { /* the latch step's cells: the process's lowering record */
        TEST_ASSERT_EQUAL_UINT32(fx_pw(&fx, 0)->lowered.v,
                                 odin3_node_prov(fx.res.module, fx.cells[k]).v);
    }
    TEST_ASSERT_EQUAL_UINT32(mux_prov.v, odin3_node_prov(fx.res.module, fx.cells[6]).v);
    char warns[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 2));
    TEST_ASSERT_EQUAL_STRING("odin3: warning: proc: module 'm': latch inferred for 'y': the always "
                             "block does not assign it on every path",
                             warns[0]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W1]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DLATCH]);
    TEST_ASSERT_EQUAL_UINT32(5, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_MUX]); /* not counted again */
    TEST_ASSERT_EQUAL_UINT32(0, ((const odin3_pw_sync *)odin3_vec_cat(&fx_pw(&fx, 0)->syncs, 0))
                                    ->actions.len); /* cleared for the ff phase */
    fx_close(&fx);
}

/* latch_nolatches_none.il: the --nolatches body (an unassigned path is x): quickcheck finds no
 * feedback, so y is merged with the mux's Y (its A on $_CONSTX_'s net) at M2, without a warning. */
static void test_latch_nolatches_none(void) {
    static const char *const k_cells[] = {"$mux WIDTH=1 A=[x] B=[a] S=[en] Y=[t0] full_case=1"};
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\en\n"
                 "  wire input 2 \\a\n"
                 "  wire output 3 \\y\n"
                 "  wire \\t0\n"
                 "  wire \\t1\n"
                 "  process $p\n"
                 "    assign \\t0 \\t1\n"
                 "    switch \\en\n"
                 "      case 1'1\n"
                 "        assign \\t1 \\a\n"
                 "      case\n"
                 "        assign \\t1 1'x\n"
                 "    end\n"
                 "    sync always\n"
                 "      update \\y \\t0\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "t0", 0).v, wire_net(&fx, "y", 0).v);
    char warns[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(0, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 2));
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.cells[ODIN3_PROC_K_DLATCH]);
    TEST_ASSERT_EQUAL_UINT32(0, expands.count); /* quickcheck false: no walk */
    fx_close(&fx);
}

/* --- quickcheck through a ?: mux, readers, groups ----------------------------------------- */

/* latch_through_ternary.il: a 2D ?: $mux outside the process (A = y, B = b, S = c) feeding the
 * rhs: quickcheck is true and the hold rule is "or" of (c, 0, TRUE); the latch step's cells take
 * the process's attributes, and the ?: mux is re-created with A := b and its own record. */
static const char *const k_ternary = "module \\m\n"
                                     "  wire input 1 \\b\n"
                                     "  wire input 2 \\c\n"
                                     "  wire output 3 \\y\n"
                                     "  wire \\w\n"
                                     "  cell $mux \\m0\n"
                                     "    parameter \\WIDTH 1\n"
                                     "    connect \\A \\y\n"
                                     "    connect \\B \\b\n"
                                     "    connect \\S \\c\n"
                                     "    connect \\Y \\w\n"
                                     "  end\n"
                                     "  attribute \\keep 1\n"
                                     "  process $p\n"
                                     "    sync always\n"
                                     "      update \\y \\w\n"
                                     "  end\n"
                                     "end\n";

/* Rule rule_id is the "or" of one child, (c, 0, TRUE). */
static void expect_or_of_inner(const fixture *fx, uint32_t rule_id) {
    const odin3_proc_rule *rule = odin3_proc_ldb_rule(&fx->mctx, rule_id);
    TEST_ASSERT_NOT_NULL(rule);
    TEST_ASSERT_EQUAL_HEX32(ODIN3_PW_S0, rule->signal);
    TEST_ASSERT_EQUAL_HEX32(ODIN3_PW_S0, rule->match);
    TEST_ASSERT_EQUAL_UINT32(1, rule->n_children);
    const odin3_proc_rule *inner =
        odin3_proc_ldb_rule(&fx->mctx, odin3_proc_ldb_child(&fx->mctx, rule, 0));
    TEST_ASSERT_NOT_NULL(inner);
    TEST_ASSERT_EQUAL_UINT32(wire_net(fx, "c", 0).v, inner->signal);
    TEST_ASSERT_EQUAL_HEX32(ODIN3_PW_S0, inner->match);
    TEST_ASSERT_EQUAL_UINT32(1, inner->n_children);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROC_RULE_TRUE, odin3_proc_ldb_child(&fx->mctx, inner, 0));
}

/* The only latch's rule: the "or" of one child, (c, 0, TRUE). */
static void expect_ternary_rule(const fixture *fx) {
    odin3_node_id latch = {0};
    uint32_t rule_id = 0;
    TEST_ASSERT_TRUE(odin3_proc_ldb_latch(&fx->mctx, 0, &latch, &rule_id));
    TEST_ASSERT_FALSE(odin3_proc_ldb_latch(&fx->mctx, 1, &latch, &rule_id));
    expect_or_of_inner(fx, rule_id);
}

static void test_latch_through_ternary(void) {
    static const char *const k_cells[] = {NOT1("c", "c0"),
                                          OR1("1", "c1"),
                                          AND1("c0", "c1", "c2"),
                                          OR1("c2", "c3"),
                                          NOT1("c3", "c4"),
                                          DLATCH("1", "c4", "w", "y"),
                                          "$mux WIDTH=1 A=[b] B=[b] S=[c] Y=[w]"};
    fixture fx;
    fx_open(&fx, k_ternary);
    fx_step_b(&fx);
    odin3_prov_id mux_prov = odin3_node_prov(fx.res.module, node_named(&fx, "m0"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_ldb_build(&fx.mctx));
    const odin3_pw_action *act = odin3_vec_cat(
        &((const odin3_pw_sync *)odin3_vec_cat(&fx_pw(&fx, 0)->syncs, 0))->actions, 0);
    odin3_proc_qc qc = {fx_pw(&fx, 0), act->rhs, act->lhs};
    TEST_ASSERT_TRUE(odin3_proc_quickcheck(&fx.mctx, &qc));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    expect_ternary_rule(&fx);
    uint32_t keep = str_id(&fx, "keep");
    for (uint32_t k = 0; k < 6; k++) { /* helpers, EN and $dlatch: the $proc node's attributes */
        TEST_ASSERT_NOT_NULL(
            odin3_attr_get(fx.res.module, (odin3_objref){ODIN3_OBJ_NODE, fx.cells[k].v}, keep));
    }
    TEST_ASSERT_NULL(odin3_attr_get(fx.res.module, (odin3_objref){ODIN3_OBJ_NODE, fx.cells[6].v},
                                    keep)); /* the re-created ?: mux keeps its own */
    TEST_ASSERT_EQUAL_UINT32(mux_prov.v, odin3_node_prov(fx.res.module, fx.cells[6]).v);
    fx_close(&fx);
}

/* latch_users_gt1.il: the mux's output is read by two $not cells, so set_undef is false at it and
 * below: the walk expands it once with set_undef false, its A pin stays on y and the fix-up leaves
 * it as it is (Yosys keeps `A = y` too). */
static void test_latch_users_gt1(void) {
    static const char *const k_cells[] = {
        NOT1("t0", "z1"), NOT1("t0", "z2"), "$mux WIDTH=1 A=[y] B=[a] S=[en] Y=[t0] full_case=1",
        NOT1("en", "c3"), OR1("1", "c4"),   AND1("c3", "c4", "c5"),
        OR1("c5", "c6"),  NOT1("c6", "c7"), DLATCH("1", "c7", "t0", "y")};
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\en\n"
                 "  wire input 2 \\a\n"
                 "  wire output 3 \\y\n"
                 "  wire output 4 \\z1\n"
                 "  wire output 5 \\z2\n"
                 "  wire \\t0\n"
                 "  wire \\t1\n"
                 "  cell $not \\n1\n"
                 "    parameter \\A_SIGNED 0\n"
                 "    parameter \\A_WIDTH 1\n"
                 "    parameter \\Y_WIDTH 1\n"
                 "    connect \\A \\t0\n"
                 "    connect \\Y \\z1\n"
                 "  end\n"
                 "  cell $not \\n2\n"
                 "    parameter \\A_SIGNED 0\n"
                 "    parameter \\A_WIDTH 1\n"
                 "    parameter \\Y_WIDTH 1\n"
                 "    connect \\A \\t0\n"
                 "    connect \\Y \\z2\n"
                 "  end\n"
                 "  process $p\n"
                 "    assign \\t0 \\t1\n"
                 "    switch \\en\n"
                 "      case 1'1\n"
                 "        assign \\t1 \\a\n"
                 "      case\n"
                 "        assign \\t1 \\y\n"
                 "    end\n"
                 "    sync always\n"
                 "      update \\y \\t0\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_UINT32(1, expands.count);
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "t0", 0).v, expands.net[0].v);
    TEST_ASSERT_FALSE(expands.set_undef[0]);
    fx_close(&fx);
}

/* latch_group_by_wire_rule.il: y[1:0] under en is one $dlatch WIDTH 2; v[0] (en: the same rule,
 * whose hold is reused, but another wire) and v[1] (f: another rule) are two more, in key order. */
static void test_latch_group_by_wire_rule(void) {
    static const char *const k_cells[] = {
        NOT1("en", "c0"),
        OR1("1", "c1"),
        AND1("c0", "c1", "c2"),
        OR1("c2", "c3"),
        NOT1("c3", "c4"),
        DLATCH("2", "c4", "t1[0] t1[1]", "y[0] y[1]"),
        NOT1("c3", "c6"),
        DLATCH("1", "c6", "u1[0]", "v[0]"),
        NOT1("f", "c8"),
        OR1("1", "c9"),
        AND1("c8", "c9", "c10"),
        OR1("c10", "c11"),
        NOT1("c11", "c12"),
        DLATCH("1", "c12", "u1[1]", "v[1]"),
        "$mux WIDTH=2 A=[a[0] a[1]] B=[a[0] a[1]] S=[en] Y=[t1[0] t1[1]]",
        "$mux WIDTH=1 A=[a[0]] B=[a[0]] S=[en] Y=[u1[0]]",
        "$mux WIDTH=1 A=[a[1]] B=[a[1]] S=[f] Y=[u1[1]]"};
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\en\n"
                 "  wire input 2 \\f\n"
                 "  wire width 2 input 3 \\a\n"
                 "  wire width 2 output 4 \\y\n"
                 "  wire width 2 output 5 \\v\n"
                 "  wire width 2 \\t1\n"
                 "  wire width 2 \\u1\n"
                 "  process $p\n"
                 "    assign \\t1 \\y\n"
                 "    assign \\u1 \\v\n"
                 "    switch \\en\n"
                 "      case 1'1\n"
                 "        assign \\t1 \\a\n"
                 "        assign \\u1 [0] \\a [0]\n"
                 "    end\n"
                 "    switch \\f\n"
                 "      case 1'1\n"
                 "        assign \\u1 [1] \\a [1]\n"
                 "    end\n"
                 "    sync always\n"
                 "      update \\y \\t1\n"
                 "      update \\v \\u1\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    char warns[4][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(3, lines_at(ODIN3_LOG_WARN, "", warns, 4));
    TEST_ASSERT_NOT_NULL(strstr(warns[0], "latch inferred for 'y':"));
    TEST_ASSERT_NOT_NULL(strstr(warns[1], "latch inferred for 'v[0]':"));
    TEST_ASSERT_NOT_NULL(strstr(warns[2], "latch inferred for 'v[1]':"));
    fx_close(&fx);
}

/* --- the fix-up ---------------------------------------------------------------------------- */

/* latch_fixup_rules.il: in the latch's cone, the process's mux loses its x A (A := p); $pmux pm
 * with B slices {x, b} and S {s0, s1} becomes $mux(A = r, B = b, S = s1); $mux rm whose only B
 * slice is z (undefined too, as Yosys's is_fully_undef) becomes B = A, S = 1'0; $pmux tm with A x
 * takes its last valid slice t2 as A. Each keeps its record; they are re-created in breadth-first
 * order from the $dlatch. */
static const char *const k_fixup = "module \\m\n"
                                   "  wire input 1 \\en\n"
                                   "  wire input 2 \\b\n"
                                   "  wire input 3 \\s0\n"
                                   "  wire input 4 \\s1\n"
                                   "  wire input 5 \\s2\n"
                                   "  wire input 6 \\s3\n"
                                   "  wire input 7 \\s4\n"
                                   "  wire input 8 \\t1\n"
                                   "  wire input 9 \\t2\n"
                                   "  wire output 10 \\y\n"
                                   "  wire \\p\n"
                                   "  wire \\r\n"
                                   "  wire \\tt\n"
                                   "  wire \\t0\n"
                                   "  wire \\u\n"
                                   "  cell $pmux \\pm\n"
                                   "    parameter \\WIDTH 1\n"
                                   "    parameter \\S_WIDTH 2\n"
                                   "    connect \\A \\r\n"
                                   "    connect \\B { \\b 1'x }\n"
                                   "    connect \\S { \\s1 \\s0 }\n"
                                   "    connect \\Y \\p\n"
                                   "  end\n"
                                   "  cell $mux \\rm\n"
                                   "    parameter \\WIDTH 1\n"
                                   "    connect \\A \\tt\n"
                                   "    connect \\B 1'z\n"
                                   "    connect \\S \\s2\n"
                                   "    connect \\Y \\r\n"
                                   "  end\n"
                                   "  cell $pmux \\tm\n"
                                   "    parameter \\WIDTH 1\n"
                                   "    parameter \\S_WIDTH 2\n"
                                   "    connect \\A 1'x\n"
                                   "    connect \\B { \\t2 \\t1 }\n"
                                   "    connect \\S { \\s4 \\s3 }\n"
                                   "    connect \\Y \\tt\n"
                                   "  end\n"
                                   "  process $p\n"
                                   "    assign \\t0 \\u\n"
                                   "    switch \\en\n"
                                   "      case 1'1\n"
                                   "        assign \\u \\p\n"
                                   "      case\n"
                                   "        assign \\u \\y\n"
                                   "    end\n"
                                   "    sync always\n"
                                   "      update \\y \\t0\n"
                                   "  end\n"
                                   "end\n";

static const char *const k_fixup_cells[] = {
    NOT1("en", "c0"),
    OR1("1", "c1"),
    AND1("c0", "c1", "c2"),
    OR1("c2", "c3"),
    NOT1("c3", "c4"),
    DLATCH("1", "c4", "t0", "y"),
    "$mux WIDTH=1 A=[p] B=[p] S=[en] Y=[t0] full_case=1",
    "$mux WIDTH=1 A=[r] B=[b] S=[s1] Y=[p]",
    "$mux WIDTH=1 A=[tt] B=[tt] S=[0] Y=[r]",
    "$pmux WIDTH=1 S_WIDTH=2 A=[t2] B=[t1 t2] S=[s3 s4] Y=[tt]"};

static void test_latch_fixup_rules(void) {
    fixture fx;
    fx_open(&fx, k_fixup);
    fx_step_b(&fx);
    odin3_prov_id provs[3] = {odin3_node_prov(fx.res.module, node_named(&fx, "pm")),
                              odin3_node_prov(fx.res.module, node_named(&fx, "rm")),
                              odin3_node_prov(fx.res.module, node_named(&fx, "tm"))};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_fixup_cells, N_OF(k_fixup_cells));
    for (uint32_t k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_UINT32(provs[k].v, odin3_node_prov(fx.res.module, fx.cells[7 + k]).v);
    }
    TEST_ASSERT_EQUAL_UINT32(4, expands.count); /* the mux, then pm, rm and tm, all set_undef */
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_MUX]); /* the process's only */
    fx_close(&fx);
}

/* latch_fixup_stops_at_ff.il: the fix-up walks only Yosys's internal combinational types, so a
 * $pmux with an x slice behind a $dff in the latch's cone is not rewritten (Yosys keeps it too);
 * the process's mux in front of the $dff is. */
static void test_latch_fixup_stops_at_ff(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\clk\n  wire input 2 \\en\n  wire input 3 \\b\n"
                 "  wire input 4 \\r1\n  wire input 5 \\s0\n  wire input 6 \\s1\n"
                 "  wire output 7 \\y\n  wire \\p\n  wire \\q\n  wire \\t0\n  wire \\u\n"
                 "  cell $pmux \\pm\n    parameter \\WIDTH 1\n    parameter \\S_WIDTH 2\n"
                 "    connect \\A \\r1\n    connect \\B { \\b 1'x }\n"
                 "    connect \\S { \\s1 \\s0 }\n    connect \\Y \\p\n  end\n"
                 "  cell $dff \\r\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
                 "    connect \\CLK \\clk\n    connect \\D \\p\n    connect \\Q \\q\n  end\n"
                 "  process $p\n    assign \\t0 \\u\n    switch \\en\n      case 1'1\n"
                 "        assign \\u \\q\n      case\n        assign \\u \\y\n    end\n"
                 "    sync always\n      update \\y \\t0\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    odin3_node_id pmux = node_named(&fx, "pm");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_TRUE(odin3_node_live(fx.res.module, pmux)); /* not re-created */
    TEST_ASSERT_EQUAL_UINT32(2, odin3_node_port(fx.res.module, pmux, PORT_S).count);
    odin3_node_id mux = driver_of(&fx, wire_net(&fx, "t0", 0)); /* re-created: A := q */
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "q", 0).v,
                             odin3_pin_net(fx.res.module, port_pin(&fx, mux, PORT_A)).v);
    fx_close(&fx);
}

/* latch_repeated_out.il: two ALWAYS actions update y (from ?: muxes w0 and w1, both with y on A):
 * as Yosys's dict, the last in (w1) is y's candidate, so only w1's mux is walked and fixed and
 * the $dlatch's D is w1. */
static void test_latch_repeated_out(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\e\n  wire input 2 \\f\n  wire input 3 \\a\n  wire input 4 \\b\n"
                 "  wire output 5 \\y\n  wire \\w0\n  wire \\w1\n"
                 "  cell $mux \\c0\n    parameter \\WIDTH 1\n    connect \\A \\y\n"
                 "    connect \\B \\a\n    connect \\S \\e\n    connect \\Y \\w0\n  end\n"
                 "  cell $mux \\c1\n    parameter \\WIDTH 1\n    connect \\A \\y\n"
                 "    connect \\B \\b\n    connect \\S \\f\n    connect \\Y \\w1\n  end\n"
                 "  process $p\n    sync always\n      update \\y \\w0\n      update \\y \\w1\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    odin3_node_id first = node_named(&fx, "c0");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(1, count_type(&fx, "$dlatch"));
    odin3_node_id latch = {0};
    uint32_t rule = 0;
    TEST_ASSERT_TRUE(odin3_proc_ldb_latch(&fx.mctx, 0, &latch, &rule));
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "w1", 0).v,
                             odin3_pin_net(fx.res.module, port_pin(&fx, latch, 1)).v);
    TEST_ASSERT_TRUE(odin3_node_live(fx.res.module, first)); /* c0 untouched: A still y */
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "y", 0).v,
                             odin3_pin_net(fx.res.module, port_pin(&fx, first, PORT_A)).v);
    fx_close(&fx);
}

/* --- the walk's rules (review I1: Yosys-matched probes t8rev/p/pa, pb, pc) ---------------- */

/* latch_incomplete_case.il (t8rev pa): `always @* case (s) 0: y = a; 1: y = b; endcase` with the
 * hold default, the everyday latch. The $pmux's A branch is nested in (S[k], 0) for every select
 * bit k, so the hold is "neither item" ($not of both $eq, the 00 item's outermost); the fix-up
 * gives the $pmux its last valid slice as A. */
static void test_latch_incomplete_case(void) {
    static const char *const k_cells[] = {
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[1 0] Y=[c0]",
        "$eq A_SIGNED=0 B_SIGNED=0 A_WIDTH=2 B_WIDTH=2 Y_WIDTH=1 A=[s[0] s[1]] B=[0 0] Y=[c1]",
        NOT1("c1", "c2"),
        NOT1("c0", "c3"),
        OR1("1", "c4"),
        AND1("c3", "c4", "c5"),
        OR1("c5", "c6"),
        AND1("c2", "c6", "c7"),
        OR1("c7", "c8"),
        NOT1("c8", "c9"),
        DLATCH("1", "c9", "t", "y"),
        "$pmux WIDTH=1 S_WIDTH=2 A=[a] B=[b a] S=[c0 c1] Y=[t]"};
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire width 2 input 1 \\s\n  wire input 2 \\a\n  wire input 3 \\b\n"
                 "  wire output 4 \\y\n  wire \\t\n"
                 "  process $p\n    assign \\t \\y\n    switch \\s\n      case 2'00\n"
                 "        assign \\t \\a\n      case 2'01\n        assign \\t \\b\n    end\n"
                 "    sync always\n      update \\y \\t\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    fx_close(&fx);
}

/* latch_memo_per_out.il (t8rev pb): outputs y and z share the ?: mux m = s ? z : y
 * (`if (en) begin y = m; z = m; end`). The memo starts afresh for each output, so m is expanded
 * once per output (set_undef false: two readers) and gives (s, 0) for y but (s, 1) for z; m keeps
 * its pins (no move under two readers), the process's muxes take A := m. */
static void test_latch_memo_per_out(void) {
    static const char *const k_cells[] = {
        "$mux WIDTH=1 A=[y] B=[z] S=[s] Y=[m]",
        NOT1("en", "c1"),
        OR1("1", "c2"),
        AND1("c1", "c2", "c3"),
        NOT1("s", "c4"),
        OR1("1", "c5"),
        AND1("c4", "c5", "c6"),
        OR1("c6", "c7"),
        OR1("c7", "c8"),
        AND1("en", "c8", "c9"),
        "$reduce_or A_SIGNED=0 A_WIDTH=2 Y_WIDTH=1 A=[c3 c9] Y=[c10]",
        NOT1("c10", "c11"),
        DLATCH("1", "c11", "ty", "y"),
        OR1("1", "c13"),
        AND1("s", "c13", "c14"),
        OR1("c14", "c15"),
        OR1("c15", "c16"),
        AND1("en", "c16", "c17"),
        "$reduce_or A_SIGNED=0 A_WIDTH=2 Y_WIDTH=1 A=[c3 c17] Y=[c18]",
        NOT1("c18", "c19"),
        DLATCH("1", "c19", "tz", "z"),
        "$mux WIDTH=1 A=[m] B=[m] S=[en] Y=[ty]",
        "$mux WIDTH=1 A=[m] B=[m] S=[en] Y=[tz]"};
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\s\n  wire input 2 \\en\n  wire output 3 \\y\n"
                 "  wire output 4 \\z\n  wire \\m\n  wire \\ty\n  wire \\tz\n"
                 "  cell $mux $cm\n    parameter \\WIDTH 1\n    connect \\A \\y\n"
                 "    connect \\B \\z\n    connect \\S \\s\n    connect \\Y \\m\n  end\n"
                 "  process $p\n    assign \\ty \\y\n    assign \\tz \\z\n    switch \\en\n"
                 "      case 1'1\n        assign \\ty \\m\n        assign \\tz \\m\n    end\n"
                 "    sync always\n      update \\y \\ty\n      update \\z \\tz\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_UINT32(4, expands.count); /* ty, m; tz, m: m once per output */
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "m", 0).v, expands.net[1].v);
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "m", 0).v, expands.net[3].v);
    TEST_ASSERT_FALSE(expands.set_undef[3]);
    fx_close(&fx);
}

/* latch_self_hold.il (t8rev pc): a hold default and nothing else (an `if` folded to false) leaves
 * `y <- y`: quickcheck compares only the muxes' sources with y, not the rhs itself, so there is no
 * latch and no W1, y stays undriven (Yosys: `connect \y \y`), and z is merged with a. */
static void test_latch_self_hold(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\a\n  wire output 2 \\y\n  wire output 3 \\z\n"
                 "  wire \\t\n  wire \\u\n"
                 "  process $p\n    assign \\t \\y\n    assign \\u \\a\n    sync always\n"
                 "      update \\y \\t\n      update \\z \\u\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(0, fx.n_cells);
    TEST_ASSERT_EQUAL_UINT32(0, expands.count);
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.warnings[ODIN3_PROC_W1]);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(fx.res.module, wire_net(&fx, "y", 0)));
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "a", 0).v, wire_net(&fx, "z", 0).v);
    fx_close(&fx);
}

/* --- crossed feedback, snapshot and live view (Review Focus 2, internal risk #4) ----------- */

/* latch_crossed_feedback.il: two blocks feeding each other (y1 = y2 and y2 = y1 through promoted
 * temporaries): quickcheck sees no mux, so neither is a latch, and M2 leaves y1 and y2 on one
 * undriven net, as Yosys gives (`connect \y2 \y1`, `connect \y1 \y2`). */
static void test_latch_crossed_feedback(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire output 1 \\y1\n"
                 "  wire output 2 \\y2\n"
                 "  wire \\t0a\n"
                 "  wire \\t0b\n"
                 "  process $p1\n"
                 "    assign \\t0a \\y2\n"
                 "    sync always\n"
                 "      update \\y1 \\t0a\n"
                 "  end\n"
                 "  process $p2\n"
                 "    assign \\t0b \\y1\n"
                 "    sync always\n"
                 "      update \\y2 \\t0b\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(0, fx.n_cells);
    odin3_net_id net = wire_net(&fx, "y1", 0);
    TEST_ASSERT_EQUAL_UINT32(net.v, wire_net(&fx, "y2", 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(fx.res.module, net));
    TEST_ASSERT_EQUAL_UINT32(0, fx.stats.warnings[ODIN3_PROC_W1]);
    fx_close(&fx);
}

/* latch_snapshot_vs_live: a 2D ?: mux M (A = y, B = a, S = en) feeds `always y = w`. */
static const char *const k_snapshot = "module \\m\n"
                                      "  wire input 1 \\en\n"
                                      "  wire input 2 \\a\n"
                                      "  wire output 3 \\y\n"
                                      "  wire \\w\n"
                                      "  cell $mux \\mx\n"
                                      "    parameter \\WIDTH 1\n"
                                      "    connect \\A \\y\n"
                                      "    connect \\B \\a\n"
                                      "    connect \\S \\en\n"
                                      "    connect \\Y \\w\n"
                                      "  end\n"
                                      "  process $p\n"
                                      "    sync always\n"
                                      "      update \\y \\w\n"
                                      "  end\n"
                                      "end\n";

static odin3_proc_qc first_action_qc(fixture *fx) {
    const odin3_pw_sync *sync = odin3_vec_cat(&fx_pw(fx, 0)->syncs, 0);
    const odin3_pw_action *act = odin3_vec_cat(&sync->actions, 0);
    odin3_proc_qc qc = {fx_pw(fx, 0), act->rhs, act->lhs};
    return qc;
}

/* Internal risk #4: after the database is built, M's A pin moves to x by hand. quickcheck still
 * sees y (it reads the snapshot) until the database is rebuilt; the walk reads the moved pin live
 * and finds no feedback, so y is merged with w and there is no latch. */
static void test_latch_snapshot_vs_live(void) {
    fixture fx;
    fx_open(&fx, k_snapshot);
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_ldb_build(&fx.mctx));
    move_to_x(&fx, port_pin(&fx, node_named(&fx, "mx"), PORT_A));
    odin3_proc_qc qc = first_action_qc(&fx);
    TEST_ASSERT_TRUE(odin3_proc_quickcheck(&fx.mctx, &qc));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_ldb_build(&fx.mctx));
    TEST_ASSERT_FALSE(odin3_proc_quickcheck(&fx.mctx, &qc));
    fx_close(&fx);

    fx_open(&fx, k_snapshot);
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_ldb_build(&fx.mctx));
    move_to_x(&fx, port_pin(&fx, node_named(&fx, "mx"), PORT_A));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx)); /* uses the database built above */
    TEST_ASSERT_EQUAL_UINT32(1, expands.count);     /* quickcheck was true: the walk ran */
    TEST_ASSERT_EQUAL_UINT32(0, count_type(&fx, "$dlatch"));
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "w", 0).v, wire_net(&fx, "y", 0).v);
    fx_close(&fx);
}

/* --- init (§10, Review Focus 2) ------------------------------------------------------------ */

/* latch_init_removed.il: y[1:0] (init 2'11) is a no-latch output: its init goes, with W2 once;
 * $tmp3 (no wire) has nothing to remove and gives no warning; the latch output yl keeps its init.
 * W2 comes from the earlier process, W1 from the later. */
static void test_latch_init_removed(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\en\n"
                 "  wire width 2 input 2 \\a\n"
                 "  attribute \\init 2'11\n"
                 "  wire width 2 output 3 \\y\n"
                 "  attribute \\init 1'0\n"
                 "  wire output 4 \\yl\n"
                 "  wire width 2 \\t0\n"
                 "  wire \\t1\n"
                 "  wire \\t2\n"
                 "  wire \\t3\n"
                 "  wire $tmp3\n"
                 "  process $p1\n"
                 "    assign \\t0 \\a\n"
                 "    sync always\n"
                 "      update \\y \\t0\n"
                 "  end\n"
                 "  process $p2\n"
                 "    assign \\t1 \\a [0]\n"
                 "    sync always\n"
                 "      update $tmp3 \\t1\n"
                 "  end\n"
                 "  process $p3\n"
                 "    assign \\t2 \\t3\n"
                 "    switch \\en\n"
                 "      case 1'1\n"
                 "        assign \\t3 \\a [1]\n"
                 "      case\n"
                 "        assign \\t3 \\yl\n"
                 "    end\n"
                 "    sync always\n"
                 "      update \\yl \\t2\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    uint32_t init = str_id(&fx, "init");
    TEST_ASSERT_NULL(odin3_attr_get(fx.res.module,
                                    (odin3_objref){ODIN3_OBJ_WIRE, wire_named(&fx, "y").v}, init));
    const odin3_value *kept = odin3_attr_get(
        fx.res.module, (odin3_objref){ODIN3_OBJ_WIRE, wire_named(&fx, "yl").v}, init);
    TEST_ASSERT_NOT_NULL(kept);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, kept->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, kept->bits[0]);
    char warns[3][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(2, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 3));
    TEST_ASSERT_EQUAL_STRING("odin3: warning: proc: module 'm': initial value of 'y' is ignored: "
                             "'y' is assigned in a combinational always block",
                             warns[0]);
    TEST_ASSERT_EQUAL_STRING("odin3: warning: proc: module 'm': latch inferred for 'yl': the "
                             "always block does not assign it on every path",
                             warns[1]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W2]);
    TEST_ASSERT_EQUAL_UINT32(1, count_type(&fx, "$dlatch"));
    fx_close(&fx);
}

/* --- the memo (Review Focus 5, internal risk #6) ------------------------------------------- */

/* The diamond: M1 = $mux(A = y, B = a, S = s1), Mk = $mux(A = B = M(k-1).Y, S = sk). */
static void diamond_text(odin3_strbuf *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(text, odin3_bytes_cstr("module \\m\n"
                                                                     "  wire input 1 \\a\n")));
    for (uint32_t k = 1; k <= DIAMOND; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(text, "  wire input %u \\s%u\n", k + 1, k));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text, "  wire \\m%u\n", k));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(text, "  wire output %u \\y\n", DIAMOND + 2));
    for (uint32_t k = 1; k <= DIAMOND; k++) {
        char in[NAME_TEXT];
        int len = k == 1 ? snprintf(in, NAME_TEXT, "\\y") : snprintf(in, NAME_TEXT, "\\m%u", k - 1);
        TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(text,
                                           "  cell $mux \\c%u\n    parameter \\WIDTH 1\n"
                                           "    connect \\A %s\n    connect \\B %s\n"
                                           "    connect \\S \\s%u\n    connect \\Y \\m%u\n  end\n",
                                           k, in, k == 1 ? "\\a" : in, k, k));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(text,
                                                         "  process $p\n    sync always\n"
                                                         "      update \\y \\m%u\n  end\nend\n",
                                                         DIAMOND));
}

/* The hold cone above the latch's EN: its $not, $and and $reduce_or cells and, while it is
 * evaluated, their values (-1 not known yet); the select nets s1 .. s30. */
typedef struct cone {
    odin3_node_id node[MAX_CELLS];
    int value[MAX_CELLS];
    uint32_t count;
    odin3_net_id sel_net[DIAMOND];
} cone;

static const char *type_of(const fixture *fx, odin3_node_id node) {
    return odin3_celltype_get(fx->res.design, odin3_node_type(fx->res.module, node))->name;
}

static bool is_hold_cell(const char *type) {
    return strcmp(type, "$not") == 0 || strcmp(type, "$and") == 0 ||
           strcmp(type, "$reduce_or") == 0;
}

static bool cone_has(const cone *cn, odin3_node_id node, uint32_t *at) {
    for (uint32_t i = 0; i < cn->count; i++) {
        if (cn->node[i].v == node.v) {
            *at = i;
            return true;
        }
    }
    return false;
}

/* Pushes the nets on the input pins of a hold cell ($and: A and B; else A). */
static void push_inputs(const fixture *fx, odin3_node_id node, odin3_vec *stack) {
    uint32_t n_ports = strcmp(type_of(fx, node), "$and") == 0 ? 2 : 1;
    for (uint32_t port = 0; port < n_ports; port++) {
        odin3_pinslice in = odin3_node_port(fx->res.module, node, port);
        for (uint32_t k = 0; k < in.count; k++) {
            odin3_net_id *slot = odin3_vec_push(stack);
            TEST_ASSERT_NOT_NULL(slot);
            *slot = odin3_pin_net(fx->res.module, (odin3_pin_id){in.first.v + k});
        }
    }
}

/* Collects the hold cells above net, each once, with an explicit stack. */
static void cone_collect(const fixture *fx, odin3_net_id net, cone *cn) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(odin3_net_id));
    *(odin3_net_id *)odin3_vec_push(&stack) = net;
    while (stack.len > 0) {
        odin3_net_id top = *(const odin3_net_id *)odin3_vec_cat(&stack, stack.len - 1);
        odin3_vec_pop(&stack);
        odin3_node_id node = driver_of(fx, top);
        uint32_t at = 0;
        if (!odin3_node_valid(node) || !is_hold_cell(type_of(fx, node)) ||
            cone_has(cn, node, &at)) {
            continue;
        }
        TEST_ASSERT_TRUE(cn->count < MAX_CELLS);
        cn->node[cn->count++] = node;
        push_inputs(fx, node, &stack);
    }
    odin3_vec_free(&stack);
}

/* A net's value: a constant, a select, or a cone cell's value (-1 when not known yet). */
static int net_value(const fixture *fx, const cone *cn, odin3_net_id net, const bool *sel) {
    odin3_const value = odin3_net_const_value(fx->res.module, net);
    if (value != ODIN3_CONST_NONE) {
        return value == ODIN3_CONST_1 ? 1 : 0;
    }
    for (uint32_t k = 0; k < DIAMOND; k++) {
        if (cn->sel_net[k].v == net.v) {
            return sel[k] ? 1 : 0;
        }
    }
    uint32_t at = 0;
    TEST_ASSERT_TRUE(cone_has(cn, driver_of(fx, net), &at));
    return cn->value[at];
}

/* Cone cell `at`'s value from its inputs, -1 while one of them is not known. */
static int cell_value(const fixture *fx, const cone *cn, uint32_t at, const bool *sel) {
    odin3_node_id node = cn->node[at];
    const char *type = type_of(fx, node);
    odin3_pinslice in = odin3_node_port(fx->res.module, node, 0);
    bool is_and = strcmp(type, "$and") == 0;
    int acc = is_and ? 1 : 0;
    uint32_t n_in = in.count + (is_and ? 1U : 0U);
    for (uint32_t k = 0; k < n_in; k++) {
        odin3_pin_id pin = k < in.count ? (odin3_pin_id){in.first.v + k}
                                        : odin3_node_port(fx->res.module, node, 1).first;
        int bit = net_value(fx, cn, odin3_pin_net(fx->res.module, pin), sel);
        if (bit < 0) {
            return -1;
        }
        acc = is_and ? (acc & bit) : (acc | bit);
    }
    return strcmp(type, "$not") == 0 ? 1 - acc : acc;
}

/* The value of net (the latch's EN) for the selects sel, evaluating the cone to a fixed point. */
static bool cone_eval(const fixture *fx, cone *cn, odin3_net_id net, const bool *sel) {
    for (uint32_t i = 0; i < cn->count; i++) {
        cn->value[i] = -1;
    }
    for (uint32_t pass = 0; pass < cn->count; pass++) {
        for (uint32_t i = 0; i < cn->count; i++) {
            cn->value[i] = cn->value[i] < 0 ? cell_value(fx, cn, i, sel) : cn->value[i];
        }
    }
    int value = net_value(fx, cn, net, sel);
    TEST_ASSERT_TRUE(value >= 0);
    return value == 1;
}

/* No (net, set_undef) was expanded twice. */
static void expect_expands_distinct(void) {
    for (uint32_t i = 0; i < expands.count; i++) {
        for (uint32_t j = i + 1; j < expands.count; j++) {
            TEST_ASSERT_FALSE(expands.net[i].v == expands.net[j].v &&
                              expands.set_undef[i] == expands.set_undef[j]);
        }
    }
}

/* EN = s1 for every assignment of s1 .. s5, the other selects 0. */
static void expect_en_is_s1(const fixture *fx, cone *cn, odin3_net_id enable) {
    for (uint32_t k = 0; k < DIAMOND; k++) {
        char name[NAME_TEXT];
        (void)snprintf(name, NAME_TEXT, "s%u", k + 1);
        cn->sel_net[k] = wire_net(fx, name, 0);
    }
    for (uint32_t bits = 0; bits < (1U << DIAMOND_FREE); bits++) {
        bool sel[DIAMOND] = {false};
        for (uint32_t k = 0; k < DIAMOND_FREE; k++) {
            sel[k] = ((bits >> k) & 1U) != 0;
        }
        TEST_ASSERT_EQUAL(sel[0], cone_eval(fx, cn, enable, sel));
    }
}

/* Review Focus 5: 30 shared muxes. The walk expands each (net, set_undef) at most once (M30's Y
 * with set_undef, the rest without: two readers each), the EN cone has 179 cells (4 for M1, 6 per
 * later stage, the EN $not; Yosys 0.55 gives the formula's 47 at depth 8), and EN = s1 for every
 * assignment of s1 .. s5 with the other selects 0. */
static void test_latch_diamond_30(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    diamond_text(&text);
    fixture fx;
    fx_open(&fx, text.data);
    odin3_strbuf_free(&text);
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(DIAMOND, expands.count);
    expect_expands_distinct();
    TEST_ASSERT_EQUAL_UINT32(1, count_type(&fx, "$dlatch"));
    odin3_node_id latch = {0};
    uint32_t rule = 0;
    TEST_ASSERT_TRUE(odin3_proc_ldb_latch(&fx.mctx, 0, &latch, &rule));
    odin3_net_id enable = odin3_pin_net(fx.res.module, port_pin(&fx, latch, 0));
    static cone cn;
    memset(&cn, 0, sizeof cn);
    cone_collect(&fx, enable, &cn);
    TEST_ASSERT_TRUE(cn.count <= 180);
    TEST_ASSERT_EQUAL_UINT32(179, cn.count);
    expect_en_is_s1(&fx, &cn, enable);
    fx_close(&fx);
}

/* latch_memo_set_undef: T = $mux(A = nn, B = mm, S = s) feeds `always y = w`; nn = N.Y has two
 * readers (T and a $not); mm = M.Y, M = $mux(A = y, B = a, S = u), has one. N's A is moved onto
 * mm after the database is built (the snapshot keeps mm's one reader): in a consistent snapshot a
 * net reached twice in one walk has two readers and so one key, and only a rewiring the snapshot
 * does not see reaches it under both. The walk reaches mm first through nn (set_undef false) and
 * then directly from T (true): two expansions, one per key, and only the second moves M's A pin
 * off y (the fix-up then re-creates M with A := a). */
static void test_latch_memo_set_undef(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\s\n  wire input 2 \\t\n  wire input 3 \\u\n"
                 "  wire input 4 \\a\n  wire input 5 \\b\n  wire input 6 \\c\n"
                 "  wire output 7 \\y\n  wire output 8 \\z\n"
                 "  wire \\w\n  wire \\nn\n  wire \\mm\n"
                 "  cell $mux \\tm\n    parameter \\WIDTH 1\n    connect \\A \\nn\n"
                 "    connect \\B \\mm\n    connect \\S \\s\n    connect \\Y \\w\n  end\n"
                 "  cell $mux \\nm\n    parameter \\WIDTH 1\n    connect \\A \\c\n"
                 "    connect \\B \\b\n    connect \\S \\t\n    connect \\Y \\nn\n  end\n"
                 "  cell $not \\x\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
                 "    parameter \\Y_WIDTH 1\n    connect \\A \\nn\n    connect \\Y \\z\n  end\n"
                 "  cell $mux \\mx\n    parameter \\WIDTH 1\n    connect \\A \\y\n"
                 "    connect \\B \\a\n    connect \\S \\u\n    connect \\Y \\mm\n  end\n"
                 "  process $p\n    sync always\n      update \\y \\w\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_ldb_build(&fx.mctx));
    odin3_pin_id nm_a = port_pin(&fx, node_named(&fx, "nm"), PORT_A);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(fx.res.module, nm_a));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(fx.res.module, nm_a, wire_net(&fx, "mm", 0)));
    expands.module = fx.res.module;
    expands.watch = port_pin(&fx, node_named(&fx, "mx"), PORT_A);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    odin3_net_id mm = wire_net(&fx, "mm", 0);
    odin3_net_id ynet = wire_net(&fx, "y", 0);
    TEST_ASSERT_EQUAL_UINT32(4, expands.count); /* w (true), nn (false), mm (false), mm (true) */
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "nn", 0).v, expands.net[1].v);
    TEST_ASSERT_FALSE(expands.set_undef[1]);
    TEST_ASSERT_EQUAL_UINT32(mm.v, expands.net[2].v);
    TEST_ASSERT_FALSE(expands.set_undef[2]);
    TEST_ASSERT_EQUAL_UINT32(mm.v, expands.net[3].v);
    TEST_ASSERT_TRUE(expands.set_undef[3]);
    TEST_ASSERT_EQUAL_UINT32(ynet.v, expands.watched[2].v); /* not moved before either one */
    TEST_ASSERT_EQUAL_UINT32(ynet.v, expands.watched[3].v);
    odin3_node_id remade = driver_of(&fx, mm); /* moved, then A := a by the fix-up */
    TEST_ASSERT_EQUAL_UINT32(wire_net(&fx, "a", 0).v,
                             odin3_pin_net(fx.res.module, port_pin(&fx, remade, PORT_A)).v);
    fx_close(&fx);
}

/* --- robustness ---------------------------------------------------------------------------- */

/* latch_mux_cycle: a mux whose B is its own Y (a combinational cycle 2D can elaborate from
 * `assign w = s ? b : w;`) in the cone: Yosys's recursion never ends there; 2E reads the cycle as
 * no feedback and ends, with the latch of the A branch. */
static void test_latch_mux_cycle(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\s\n  wire input 2 \\t\n  wire input 3 \\b\n"
                 "  wire output 4 \\y\n  wire \\w\n  wire \\v\n"
                 "  cell $mux \\c1\n    parameter \\WIDTH 1\n    connect \\A \\w\n"
                 "    connect \\B \\b\n    connect \\S \\s\n    connect \\Y \\w\n  end\n"
                 "  cell $mux \\c2\n    parameter \\WIDTH 1\n    connect \\A \\y\n"
                 "    connect \\B \\w\n    connect \\S \\t\n    connect \\Y \\v\n  end\n"
                 "  process $p\n    sync always\n      update \\y \\v\n  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(2, expands.count); /* v (true), w (false: two readers) */
    TEST_ASSERT_EQUAL_UINT32(1, count_type(&fx, "$dlatch"));
    odin3_node_id latch = {0};
    uint32_t rule_id = 0;
    TEST_ASSERT_TRUE(odin3_proc_ldb_latch(&fx.mctx, 0, &latch, &rule_id));
    const odin3_proc_rule *rule = odin3_proc_ldb_rule(&fx.mctx, rule_id);
    TEST_ASSERT_NOT_NULL(rule);
    TEST_ASSERT_EQUAL_UINT32(1, rule->n_children); /* the A branch only: the cycle is FALSE */
    fx_close(&fx);
}

/* latch_const_candidate: an ALWAYS output merged into $_CONST1_'s net through the net map before
 * the phase (2D never does it) is an internal error, with valid IR. */
static void test_latch_const_candidate(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n"
                 "  wire input 1 \\a\n"
                 "  wire output 2 \\y\n"
                 "  wire \\t0\n"
                 "  process $p\n"
                 "    assign \\t0 \\a\n"
                 "    sync always\n"
                 "      update \\y \\t0\n"
                 "  end\n"
                 "end\n");
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_proc_connect(&fx.mctx, wire_net(&fx, "y", 0).v, ODIN3_PW_S1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx.mctx));
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, fx_latch(&fx));
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_ERROR, "", errs, 2));
    TEST_ASSERT_EQUAL_STRING("proc: internal: latch candidate on a constant net", errs[0]);
    fx_close(&fx);
}

static void test_latch_invalid(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_latches(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_ldb_build(NULL));
    TEST_ASSERT_FALSE(odin3_proc_quickcheck(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, odin3_proc_ldb_bytes(NULL));
    odin3_proc_ldb_free(NULL);
    fixture fx;
    fx_open(&fx, k_verified);
    odin3_proc_qc qc = first_action_qc(&fx);
    TEST_ASSERT_FALSE(odin3_proc_quickcheck(&fx.mctx, &qc)); /* no database yet */
    TEST_ASSERT_NULL(odin3_proc_ldb_rule(&fx.mctx, ODIN3_PROC_RULE_FIRST));
    fx_close(&fx);
}

/* --- out of memory ------------------------------------------------------------------------- */

/* latch_oom.il: the fix-up module with two more processes, a no-latch output with an init (W2)
 * and a two-bit latch group. */
static const char *const k_oom = "module \\m\n"
                                 "  wire input 1 \\en\n"
                                 "  wire input 2 \\b\n"
                                 "  wire input 3 \\s0\n"
                                 "  wire input 4 \\s1\n"
                                 "  wire input 5 \\s2\n"
                                 "  wire width 2 input 6 \\d\n"
                                 "  wire output 7 \\y\n"
                                 "  attribute \\init 1'1\n"
                                 "  wire output 8 \\q\n"
                                 "  wire width 2 output 9 \\v\n"
                                 "  wire \\p\n"
                                 "  wire \\r\n"
                                 "  wire \\t0\n"
                                 "  wire \\u\n"
                                 "  wire \\t5\n"
                                 "  wire width 2 \\t6\n"
                                 "  cell $pmux \\pm\n"
                                 "    parameter \\WIDTH 1\n"
                                 "    parameter \\S_WIDTH 2\n"
                                 "    connect \\A \\r\n"
                                 "    connect \\B { \\b 1'x }\n"
                                 "    connect \\S { \\s1 \\s0 }\n"
                                 "    connect \\Y \\p\n"
                                 "  end\n"
                                 "  cell $mux \\rm\n"
                                 "    parameter \\WIDTH 1\n"
                                 "    connect \\A 1'x\n"
                                 "    connect \\B \\b\n"
                                 "    connect \\S \\s2\n"
                                 "    connect \\Y \\r\n"
                                 "  end\n"
                                 "  process $p1\n"
                                 "    assign \\t0 \\u\n"
                                 "    switch \\en\n"
                                 "      case 1'1\n"
                                 "        assign \\u \\p\n"
                                 "      case\n"
                                 "        assign \\u \\y\n"
                                 "    end\n"
                                 "    sync always\n"
                                 "      update \\y \\t0\n"
                                 "  end\n"
                                 "  process $p2\n"
                                 "    assign \\t5 \\b\n"
                                 "    sync always\n"
                                 "      update \\q \\t5\n"
                                 "  end\n"
                                 "  process $p3\n"
                                 "    assign \\t6 \\v\n"
                                 "    switch \\s0\n"
                                 "      case 1'1\n"
                                 "        assign \\t6 \\d\n"
                                 "    end\n"
                                 "    sync always\n"
                                 "      update \\v \\t6\n"
                                 "  end\n"
                                 "end\n";

static char oom_want[MAX_CELLS][DESC_TEXT];
static uint32_t oom_cells;

/*
 * Whether the injected failure fired: probes up to fail + 1 allocations, one of which fails when it
 * did not (fewer than fail + 1 allocations were made). Leaves the hook disabled.
 */
static bool injection_fired(long fail) {
    for (long i = 0; i <= fail; i++) {
        void *probe = odin3_util_malloc(1);
        if (probe == NULL) {
            return false;
        }
        odin3_util_free(probe);
    }
    odin3_util_set_alloc_fail_after(-1);
    return true;
}

/* One injection over the latch phase: NO_MEMORY or the whole result (a failure the phase absorbs,
 * such as a diagnostic's location, gives the whole result too), valid IR (ASan reports a leak at
 * exit). True once the phase makes no more allocations than fail. */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom);
    fx_step_b(&fx);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_proc_latches(&fx.mctx);
    bool fired = injection_fired(fail);
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
    }
    fx_close(&fx);
    return !fired;
}

static void test_latch_oom(void) {
    odin3_proc_test_set_hooks(NULL);
    fixture fx;
    fx_open(&fx, k_oom);
    fx_step_b(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_latch(&fx));
    TEST_ASSERT_EQUAL_UINT32(2, count_type(&fx, "$dlatch"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W2]);
    oom_cells = fx.n_cells;
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_latch_verified_example);
    RUN_TEST(test_latch_nolatches_none);
    RUN_TEST(test_latch_through_ternary);
    RUN_TEST(test_latch_users_gt1);
    RUN_TEST(test_latch_group_by_wire_rule);
    RUN_TEST(test_latch_fixup_rules);
    RUN_TEST(test_latch_fixup_stops_at_ff);
    RUN_TEST(test_latch_repeated_out);
    RUN_TEST(test_latch_incomplete_case);
    RUN_TEST(test_latch_memo_per_out);
    RUN_TEST(test_latch_self_hold);
    RUN_TEST(test_latch_crossed_feedback);
    RUN_TEST(test_latch_snapshot_vs_live);
    RUN_TEST(test_latch_init_removed);
    RUN_TEST(test_latch_diamond_30);
    RUN_TEST(test_latch_memo_set_undef);
    RUN_TEST(test_latch_mux_cycle);
    RUN_TEST(test_latch_const_candidate);
    RUN_TEST(test_latch_invalid);
    RUN_TEST(test_latch_oom);
    return UNITY_END();
}
