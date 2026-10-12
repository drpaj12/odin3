/*
 * test_proc_ff.c — proc's flip-flop phase (2E spec §9, §14, §15; PR-15; plan Task 9): groups by
 * chunk (find_any_lvalue), $dff with both clock edges, $adff through every reset form (Review
 * Focus 1), the collapse of equal resets, the self-reset mux, $dffsr (W4, with the name list cut
 * after three), $aldff (W3), a self-clocked and a constant-clocked register, attributes and
 * records, the internal errors step A rules out, and out of memory.
 *
 * Each test feeds the module's bodies (proc_build) through what the pass runs before the
 * flip-flop phase: decode, clean, rmdead, prune, arst and the clock pre-check per process, then
 * step B per process (odin3_proc_begin_lowering, the node's deletion, the promotions recorded,
 * odin3_proc_mux, M1) and the latch phase (with M2).
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il, gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto dff <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-12). Yosys's cell names ($procdff$N and its autoidx)
 * give its creation order, which matches 2E's within a process; ff_groups_by_chunk's processes come
 * in node-ID order in 2E (spec §20 #8), so 2E makes q's register last where Yosys makes it first.
 * The tests ff_internal_errors and ff_invalid skip phases on purpose and have no Yosys run. A cell
 * is listed as in test_proc_latch.c (type, parameters, ports' nets LSB first, a BITS parameter as
 * N'bits MSB first, and the attributes keep, full_case and parallel_case); a net by its constant
 * (0 1 x z), else the alphabetically first wire bit on it, else `c<k>`, the output of listed cell
 * k. Only the cells the flip-flop phase makes are listed.
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
    MAX_CELLS = 64,
    MAX_PROCS = 8,
    DESC_TEXT = 512,
    NAME_TEXT = 64,
    OOM_LIMIT = 20000,
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

/* Exactly one warning, want (as the default sink prints it). */
static void expect_one_warning(const char *want) {
    char warns[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_WARN, "odin3: warning: ", warns, 2));
    TEST_ASSERT_EQUAL_STRING(want, warns[0]);
}

/* --- fixture: step A, step B, the latch phase; then the flip-flop phase --------------------- */

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_proc_mctx mctx;
    odin3_proc_stats stats;
    odin3_node_id cells[MAX_CELLS];
    uint32_t n_cells;
    uint32_t mark; /* the node end before the flip-flop phase: its cells are listed */
} fixture;

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

static odin3_pw *fx_pw(fixture *fx, uint32_t index) {
    return odin3_vec_at(&fx->mctx.pws, index);
}

/* Builds text and runs step A on each process of module m (the clock pre-check when clocks). */
static void fx_open_with(fixture *fx, const char *text, bool clocks) {
    memset(fx, 0, sizeof *fx);
    n_lines = 0;
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
        if (clocks) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_check_memwr(&fx->mctx, pw));
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_check_clocks(&fx->mctx, pw));
        }
    }
    char none[1][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(0, lines_at(ODIN3_LOG_WARN, "", none, 1)); /* check's lines aside */
    TEST_ASSERT_EQUAL_size_t(0, lines_at(ODIN3_LOG_ERROR, "", none, 1));
}

static void fx_open(fixture *fx, const char *text) {
    fx_open_with(fx, text, true);
}

static void fx_close(fixture *fx) {
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* Step B for every process (its operation, its node deleted, its promotions, mux, M1). */
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
}

/* Step B and the latch phase; the design checked, the log emptied, the mark set. */
static void fx_lower(fixture *fx) {
    fx_step_b(fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_latches(&fx->mctx));
    check_full(&fx->res);
    n_lines = 0;
    fx->mark = odin3_module_node_end(fx->res.module);
}

static bool is_listed(const odin3_celltype_def *def) {
    return def != NULL && strncmp(def->name, "$_CONST", strlen("$_CONST")) != 0 &&
           strncmp(def->name, "$port_", strlen("$port_")) != 0;
}

/* The live cells made since the mark, in node-ID order (creation order), without constants. */
static void fx_list(fixture *fx) {
    const odin3_module *module = fx->res.module;
    fx->n_cells = 0;
    for (uint32_t i = fx->mark; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) &&
            is_listed(odin3_celltype_get(fx->res.design, odin3_node_type(module, node)))) {
            TEST_ASSERT_TRUE(fx->n_cells < MAX_CELLS);
            fx->cells[fx->n_cells++] = node;
        }
    }
}

/* The flip-flop phase on every process in order, stopping at the first failure; the design
 * checked and the phase's cells listed. */
static odin3_status fx_ff(fixture *fx) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < fx->mctx.pws.len; i++) {
        st = odin3_proc_ffs(&fx->mctx, fx_pw(fx, i));
    }
    check_full(&fx->res);
    fx_list(fx);
    return st;
}

/* --- naming nets and cells (as test_proc_latch.c) ----------------------------------------- */

static const char *str_of(const fixture *fx, uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(fx->res.design), id);
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
    int len = 1;
    if (value != ODIN3_CONST_NONE) {
        len = snprintf(out, NAME_TEXT, "%s", k_consts[value]);
    } else if (!net_wire_name(fx, net, out) && !net_cell_name(fx, net, out)) {
        len = snprintf(out, NAME_TEXT, "n%u", net.v);
    }
    TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
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
        int len = snprintf(text, NAME_TEXT, " %s=%lld", key, (long long)value->i);
        TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
        put(buf, text);
    }
}

/* One parameter: an INT as its number, BITS as N'bits, MSB first. */
static void put_param(const odin3_value *value, char *buf) {
    static const char k_bit_chars[] = "01xz";
    char text[NAME_TEXT];
    if (value->kind != ODIN3_VAL_BITS) {
        int len = snprintf(text, NAME_TEXT, "%lld", (long long)value->i);
        TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
        put(buf, text);
        return;
    }
    int len = snprintf(text, NAME_TEXT, "%u'", value->len);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT && value->len < NAME_TEXT - (uint32_t)len);
    for (uint32_t i = 0; i < value->len; i++) {
        text[len + (int)i] = k_bit_chars[value->bits[value->len - 1 - i] & 3U];
    }
    text[len + (int)value->len] = '\0';
    put(buf, text);
}

/* " NAME=" before a parameter or port. */
static void put_key(const char *name, const char *tail, char *buf) {
    char text[NAME_TEXT];
    int len = snprintf(text, NAME_TEXT, " %s=%s", name, tail);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_TEXT);
    put(buf, text);
}

/* Each port's nets, LSB first. */
static void put_ports(const fixture *fx, odin3_node_id node, const odin3_celltype_def *def,
                      char *buf) {
    const odin3_module *module = fx->res.module;
    char text[NAME_TEXT];
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice pins = odin3_node_port(module, node, port);
        put_key(def->ports[port].name, "[", buf);
        for (uint32_t bit = 0; bit < pins.count; bit++) {
            net_name(fx, odin3_pin_net(module, (odin3_pin_id){pins.first.v + bit}), text);
            put(buf, bit > 0 ? " " : "");
            put(buf, text);
        }
        put(buf, "]");
    }
}

static void describe(const fixture *fx, uint32_t index, char *buf) {
    const odin3_module *module = fx->res.module;
    odin3_node_id node = fx->cells[index];
    const odin3_celltype_def *def =
        odin3_celltype_get(fx->res.design, odin3_node_type(module, node));
    buf[0] = '\0';
    put(buf, def->name);
    for (uint32_t i = 0; i < def->n_params; i++) {
        put_key(def->params[i].name, "", buf);
        put_param(odin3_node_param(module, node, i), buf);
    }
    put_ports(fx, node, def, buf);
    put_attr(fx, node, "keep", buf);
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
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(fx->res.design), odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(fx->res.module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return wire;
}

#define N_OF(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* Runs the whole of step A, step B, the latch phase and the flip-flop phase on text, which must
 * succeed with exactly the cells want (and no warning unless the caller checks one). */
static void ff_run(fixture *fx, const char *text, const char *const *want, uint32_t n) {
    fx_open(fx, text);
    fx_lower(fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_ff(fx));
    expect_cells(fx, want, n);
    for (uint32_t i = 0; i < fx->mctx.pws.len; i++) { /* every sync action emptied */
        const odin3_pw *pw = fx_pw(fx, i);
        for (size_t j = 0; j < pw->syncs.len; j++) {
            const odin3_pw_sync *sync = odin3_vec_cat(&pw->syncs, j);
            for (size_t k = 0; k < sync->actions.len; k++) {
                TEST_ASSERT_EQUAL_UINT32(
                    0, ((const odin3_pw_action *)odin3_vec_cat(&sync->actions, k))->lhs.len);
            }
        }
    }
}

#define NOT1(a, y) "$not A_SIGNED=0 A_WIDTH=1 Y_WIDTH=1 A=[" a "] Y=[" y "]"
#define OR1(a, y) "$reduce_or A_SIGNED=0 A_WIDTH=1 Y_WIDTH=1 A=[" a "] Y=[" y "]"
#define MUX1(a, b, s, y) "$mux WIDTH=1 A=[" a "] B=[" b "] S=[" s "] Y=[" y "]"
#define DFF(pol, clk, d, q) "$dff WIDTH=1 CLK_POLARITY=" pol " CLK=[" clk "] D=[" d "] Q=[" q "]"
#define ADFF(arst, d, q)                                                                           \
    "$adff WIDTH=1 CLK_POLARITY=1 ARST_POLARITY=1 ARST_VALUE=1'0 CLK=[clk] ARST=[" arst "] D=[" d  \
    "] Q=[" q "]"
#define DFFSR(set, clr)                                                                            \
    "$dffsr WIDTH=1 CLK_POLARITY=1 SET_POLARITY=1 CLR_POLARITY=1 CLK=[clk] SET=[" set              \
    "] CLR=[" clr "] D=[d] Q=[q]"
#define UNARY_PARAMS                                                                               \
    "    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n    parameter \\Y_WIDTH 1\n"

/* --- $dff and the clock edge --------------------------------------------------------------- */

/* ff_dff.il: `@(posedge clk) q <= d;` (2D's body: t0 <- d, promoted by prune). */
#define K_DFF_TEXT(edge, attrs)                                                                    \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n  wire "           \
    "\\t0\n" attrs "  process $p\n    assign \\t0 \\d\n    sync " edge                             \
    " \\clk\n      update \\q \\t0\n"                                                              \
    "  end\nend\n"

static void test_ff_dff(void) {
    static const char *const k_cells[] = {DFF("1", "clk", "d", "q")};
    fixture fx;
    ff_run(&fx, K_DFF_TEXT("posedge", ""), k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DFF]);
    fx_close(&fx);
}

/* ff_negedge.il: `@(negedge clk)` gives CLK_POLARITY 0. */
static void test_ff_negedge(void) {
    static const char *const k_cells[] = {DFF("0", "clk", "d", "q")};
    fixture fx;
    ff_run(&fx, K_DFF_TEXT("negedge", ""), k_cells, N_OF(k_cells));
    fx_close(&fx);
}

/* --- $adff (Review Focus 1) ---------------------------------------------------------------- */

#define RST_SWITCH_V(sig, v)                                                                       \
    "    switch \\" sig "\n"                                                                       \
    "      case 1'1\n        assign \\t0 " v "\n"                                                  \
    "      case\n        assign \\t0 \\d\n"                                                        \
    "    end\n"
#define RST_SWITCH(sig) RST_SWITCH_V(sig, "1'0")

/* ff_adff.il: arst_posedge's body, `@(posedge clk or posedge rst) if (rst) q <= 0; else q <= d;`:
 * arst leaves `sync high rst` with q <- 1'0, so the one reset gets its $reduce_or (A_WIDTH 1) and
 * the $adff takes its Y with ARST_POLARITY 1. */
#define K_ADFF_TEXT_V(v)                                                                           \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"                 \
    "  wire output 4 \\q\n  wire \\t0\n  process $p\n" RST_SWITCH_V(                               \
        "rst", v) "    sync posedge \\clk\n      update \\q \\t0\n"                                \
                  "    sync posedge \\rst\n      update \\q \\t0\n  end\nend\n"
#define K_ADFF_TEXT K_ADFF_TEXT_V("1'0")

static void test_ff_adff(void) {
    static const char *const k_cells[] = {OR1("rst", "c0"), ADFF("c0", "d", "q")};
    fixture fx;
    ff_run(&fx, K_ADFF_TEXT, k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_ADFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_HELPER]);
    fx_close(&fx);
}

/* ff_adff_x.il: a reset to 1'x is a constant (not made of nets), so $adff with ARST_VALUE 1'x and
 * no W3, as Yosys's is_fully_const. */
static void test_ff_adff_x_value(void) {
    static const char *const k_cells[] = {
        OR1("rst", "c0"), "$adff WIDTH=1 CLK_POLARITY=1 ARST_POLARITY=1 ARST_VALUE=1'x CLK=[clk] "
                          "ARST=[c0] D=[d] Q=[q]"};
    fixture fx;
    ff_run(&fx, K_ADFF_TEXT_V("1'x"), k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_close(&fx);
}

/* ff_adff_rst_n.il: `@(posedge clk or negedge rst_n) if (!rst_n) q <= 0; else q <= d;`: arst
 * leaves `sync low rst_n`, whose trigger is $not(rst_n), ORed by $reduce_or: ARST_POLARITY 1. */
static void test_ff_adff_rst_n(void) {
    static const char *const k_cells[] = {NOT1("rst_n", "c0"), OR1("c0", "c1"),
                                          ADFF("c1", "d", "q")};
    fixture fx;
    ff_run(
        &fx,
        "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst_n\n  wire input 3 \\d\n"
        "  wire output 4 \\q\n  wire \\t0\n  wire \\e\n"
        "  cell $logic_not $c1\n" UNARY_PARAMS "    connect \\A \\rst_n\n    connect \\Y \\e\n"
        "  end\n  process $p\n" RST_SWITCH("e") "    sync posedge \\clk\n      update \\q \\t0\n"
                                                "    sync negedge \\rst_n\n      update \\q \\t0\n"
                                                "  end\nend\n",
        k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_close(&fx);
}

/* Two resets r1 (posedge) and r2 (negedge) in arst_two_resets's shape, the second's value v. */
#define K_TWO_RESETS(v)                                                                            \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r1\n  wire input 3 \\r2\n"                 \
    "  wire input 4 \\d\n  wire output 5 \\q\n  wire \\t0\n  wire \\nr2\n"                         \
    "  cell $logic_not $c1\n" UNARY_PARAMS "    connect \\A \\r2\n    connect \\Y \\nr2\n  end\n"  \
    "  process $p\n"                                                                               \
    "    switch \\r1\n"                                                                            \
    "      case 1'1\n        assign \\t0 1'0\n"                                                    \
    "      case\n"                                                                                 \
    "        switch \\nr2\n"                                                                       \
    "          case 1'1\n            assign \\t0 " v "\n"                                          \
    "          case\n            assign \\t0 \\d\n"                                                \
    "        end\n"                                                                                \
    "    end\n"                                                                                    \
    "    sync posedge \\clk\n      update \\q \\t0\n"                                              \
    "    sync posedge \\r1\n      update \\q \\t0\n"                                               \
    "    sync negedge \\r2\n      update \\q \\t0\n  end\nend\n"

/* ff_collapse_two_equal.il: `if (r1) q <= 0; else if (!r2) q <= 0; else q <= d;`: the two rules
 * have one value, so one $reduce_or of {r1, $not(r2)} (A_WIDTH 2) and one $adff. */
static void test_ff_collapse_two_equal(void) {
    static const char *const k_cells[] = {
        NOT1("r2", "c0"), "$reduce_or A_SIGNED=0 A_WIDTH=2 Y_WIDTH=1 A=[r1 c0] Y=[c1]",
        ADFF("c1", "d", "q")};
    fixture fx;
    ff_run(&fx, K_TWO_RESETS("1'0"), k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_close(&fx);
}

/* ff_self_reset.il: arst_self_value's body. q gets its $reduce_or and $adff; u, which the reset
 * does not touch (u <- u), gets its own $reduce_or, the mux $mux(A = e, B = u, S = that Y) as its
 * D, and a $dff (Review Focus 1). */
static void test_ff_self_reset(void) {
    static const char *const k_cells[] = {OR1("rst", "c0"), ADFF("c0", "d", "q"), OR1("rst", "c2"),
                                          MUX1("e", "u", "c2", "c3"), DFF("1", "clk", "c3", "u")};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
           "  wire input 4 \\e\n  wire output 5 \\q\n  wire output 6 \\u\n  wire \\t0\n"
           "  wire \\u0\n  process $p\n"
           "    assign \\u0 \\u\n"
           "    switch \\rst\n"
           "      case 1'1\n        assign \\t0 1'0\n"
           "      case\n        assign \\t0 \\d\n        assign \\u0 \\e\n"
           "    end\n"
           "    sync posedge \\clk\n      update \\q \\t0\n      update \\u \\u0\n"
           "    sync posedge \\rst\n      update \\q \\t0\n      update \\u \\u0\n  end\nend\n",
           k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_ADFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_MUX]);
    fx_close(&fx);
}

/* --- $dffsr (W4) and $aldff (W3) ----------------------------------------------------------- */

/* ff_dffsr.il: arst_two_resets, r1 -> 1'0 and r2 -> 1'1: from the last rule to the first, r2's
 * $not trigger, set = $mux(0, 1, it), $not(1'1), clr = $mux(0, that, it), then r1's set and clr
 * stages, and the $dffsr. */
static void test_ff_dffsr(void) {
    static const char *const k_cells[] = {NOT1("r2", "c0"),
                                          MUX1("0", "1", "c0", "c1"),
                                          NOT1("1", "c2"),
                                          MUX1("0", "c2", "c0", "c3"),
                                          MUX1("c1", "0", "r1", "c4"),
                                          NOT1("0", "c5"),
                                          MUX1("c3", "c5", "r1", "c6"),
                                          DFFSR("c4", "c6")};
    fixture fx;
    ff_run(&fx, K_TWO_RESETS("1'1"), k_cells, N_OF(k_cells));
    expect_one_warning("odin3: warning: proc: module 'm': 'q' has asynchronous resets with "
                       "different values ('r1', 'r2'); it is built as a set/reset register "
                       "($dffsr)");
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W4]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DFFSR]);
    fx_close(&fx);
}

/* ff_dffsr_four_resets.il: `if (r1) q <= 0; else if (r2) q <= 1; else if (r3) q <= 0; else if
 * (r4) q <= 1; else q <= d;` with five events: four LEVEL1 rules, chained from r4 to r1; W4 names
 * the first three, then `…`. */
static void test_ff_dffsr_four_resets(void) {
    static const char *const k_cells[] = {
        MUX1("0", "1", "r4", "c0"),  NOT1("1", "c1"),  MUX1("0", "c1", "r4", "c2"),
        MUX1("c0", "0", "r3", "c3"), NOT1("0", "c4"),  MUX1("c2", "c4", "r3", "c5"),
        MUX1("c3", "1", "r2", "c6"), NOT1("1", "c7"),  MUX1("c5", "c7", "r2", "c8"),
        MUX1("c6", "0", "r1", "c9"), NOT1("0", "c10"), MUX1("c8", "c10", "r1", "c11"),
        DFFSR("c9", "c11")};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r1\n  wire input 3 \\r2\n"
           "  wire input 4 \\r3\n  wire input 5 \\r4\n  wire input 6 \\d\n  wire output 7 \\q\n"
           "  wire \\t0\n  process $p\n"
           "    switch \\r1\n      case 1'1\n        assign \\t0 1'0\n      case\n"
           "        switch \\r2\n          case 1'1\n            assign \\t0 1'1\n"
           "          case\n"
           "            switch \\r3\n              case 1'1\n                assign \\t0 1'0\n"
           "              case\n"
           "                switch \\r4\n                  case 1'1\n"
           "                    assign \\t0 1'1\n                  case\n"
           "                    assign \\t0 \\d\n                end\n"
           "            end\n        end\n    end\n"
           "    sync posedge \\clk\n      update \\q \\t0\n"
           "    sync posedge \\r1\n      update \\q \\t0\n"
           "    sync posedge \\r2\n      update \\q \\t0\n"
           "    sync posedge \\r3\n      update \\q \\t0\n"
           "    sync posedge \\r4\n      update \\q \\t0\n  end\nend\n",
           k_cells, N_OF(k_cells));
    expect_one_warning("odin3: warning: proc: module 'm': 'q' has asynchronous resets with "
                       "different values ('r1', 'r2', 'r3', …); it is built as a set/reset "
                       "register ($dffsr)");
    fx_close(&fx);
}

/* ff_aldff.il: `@(posedge clk or posedge rst) if (rst) q <= a;` (2D's default-mode body: the root
 * default t0 <- q): the reset value is a, not constant, so $aldff with ALOAD the $reduce_or's Y,
 * AD = a and D = q (the root default), and W3. */
static void test_ff_aldff(void) {
    static const char *const k_cells[] = {
        OR1("rst", "c0"), "$aldff WIDTH=1 CLK_POLARITY=1 ALOAD_POLARITY=1 CLK=[clk] ALOAD=[c0] "
                          "AD=[a] D=[q] Q=[q]"};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\a\n"
           "  wire output 4 \\q\n  wire \\t0\n  process $p\n    assign \\t0 \\q\n"
           "    switch \\rst\n      case 1'1\n        assign \\t0 \\a\n    end\n"
           "    sync posedge \\clk\n      update \\q \\t0\n"
           "    sync posedge \\rst\n      update \\q \\t0\n  end\nend\n",
           k_cells, N_OF(k_cells));
    expect_one_warning("odin3: warning: proc: module 'm': 'q' takes a non-constant value while "
                       "asynchronous reset 'rst' is active; it is built as an asynchronous-load "
                       "register ($aldff)");
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W3]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_ALDFF]);
    fx_close(&fx);
}

/* --- groups, clocks, attributes ------------------------------------------------------------ */

#define DFF8                                                                                       \
    "$dff WIDTH=8 CLK_POLARITY=1 CLK=[clk] D=[dq[0] dq[1] dq[2] dq[3] dq[4] dq[5] dq[6] dq[7]] "   \
    "Q=[q[0] q[1] q[2] q[3] q[4] q[5] q[6] q[7]]"

/* ff_groups_by_chunk.il: {a b} <- {t0a t0b} as two chunk actions (2D v4.2) gives two registers;
 * one 8-bit action on q gives one $dff of WIDTH 8 whose Q is all of q. 2E's processes come in
 * node-ID order (Yosys makes q's first). */
static void test_ff_groups_by_chunk(void) {
    static const char *const k_cells[] = {DFF("1", "clk", "d0", "a"), DFF("1", "clk", "d1", "b"),
                                          DFF8};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d0\n  wire input 3 \\d1\n"
           "  wire width 8 input 4 \\dq\n  wire output 5 \\a\n  wire output 6 \\b\n"
           "  wire width 8 output 7 \\q\n  wire \\t0a\n  wire \\t0b\n  wire width 8 \\t0q\n"
           "  process $p1\n    assign \\t0a \\d0\n    assign \\t0b \\d1\n"
           "    sync posedge \\clk\n      update \\a \\t0a\n      update \\b \\t0b\n  end\n"
           "  process $p2\n    assign \\t0q \\dq\n"
           "    sync posedge \\clk\n      update \\q \\t0q\n  end\nend\n",
           k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_UINT32(3, fx.stats.cells[ODIN3_PROC_K_DFF]);
    fx_close(&fx);
}

/* ff_group_narrowed.il, a hand-built body arst leaves alone (no root switch): the group starts from
 * the last sync's first lhs, all of q (Yosys's find_any_lvalue: its `break` leaves only the inner
 * loop), and is narrowed to q[0] by the level sync, which assigns only that bit; so q[0] is an
 * $adff of its own and q[1], which only the edge assigns, a $dff. */
static void test_ff_group_narrowed(void) {
    static const char *const k_cells[] = {OR1("r", "c0"), ADFF("c0", "d[0]", "q[0]"),
                                          DFF("1", "clk", "d[1]", "q[1]")};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r\n  wire width 2 input 3 \\d\n"
           "  wire width 2 output 4 \\q\n  process $p\n    sync high \\r\n"
           "      update \\q [0] 1'0\n    sync posedge \\clk\n      update \\q \\d\n  end\nend\n",
           k_cells, N_OF(k_cells));
    fx_close(&fx);
}

/* ff_edge_chunks.il: the last sync's lhs is all of q, so the group is q, and the edge sync's two
 * chunk actions fill insig bit by bit: one $adff of WIDTH 2 with D = d and ARST_VALUE 2'01. */
static void test_ff_edge_chunks(void) {
    static const char *const k_cells[] = {
        OR1("r", "c0"), "$adff WIDTH=2 CLK_POLARITY=1 ARST_POLARITY=1 ARST_VALUE=2'01 CLK=[clk] "
                        "ARST=[c0] D=[d[0] d[1]] Q=[q[0] q[1]]"};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r\n  wire width 2 input 3 \\d\n"
           "  wire width 2 output 4 \\q\n  process $p\n    sync posedge \\clk\n"
           "      update \\q [0] \\d [0]\n      update \\q [1] \\d [1]\n"
           "    sync high \\r\n      update \\q 2'01\n  end\nend\n",
           k_cells, N_OF(k_cells));
    fx_close(&fx);
}

/* ff_self_clocked.il: `always @(posedge a) a <= ~a;` (EL-33 re-review N1): CLK and Q on a's net. */
static void test_ff_self_clocked(void) {
    static const char *const k_cells[] = {DFF("1", "a", "na", "a")};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire output 1 \\a\n  wire \\na\n  wire \\t0\n"
           "  cell $not $c1\n" UNARY_PARAMS "    connect \\A \\a\n    connect \\Y \\na\n  end\n"
           "  process $p\n    assign \\t0 \\na\n    sync posedge \\a\n      update \\a \\t0\n"
           "  end\nend\n",
           k_cells, N_OF(k_cells));
    fx_close(&fx);
}

/* ff_const_clock.il: `always @(posedge 1'b1)`: CLK on $_CONST1_'s net, as in Yosys. */
static void test_ff_const_clock(void) {
    static const char *const k_cells[] = {DFF("1", "1", "d", "q")};
    fixture fx;
    ff_run(&fx,
           "module \\m\n  wire input 1 \\d\n  wire output 2 \\q\n  wire \\t0\n  process $p\n"
           "    assign \\t0 \\d\n    sync posedge 1'1\n      update \\q \\t0\n  end\nend\n",
           k_cells, N_OF(k_cells));
    const odin3_module *module = fx.res.module;
    odin3_net_id clk = odin3_pin_net(module, odin3_node_port(module, fx.cells[0], 0).first);
    odin3_node_id driver = odin3_pin_node(module, odin3_net_driver(module, clk));
    TEST_ASSERT_EQUAL_STRING(
        "$_CONST1_", odin3_celltype_get(fx.res.design, odin3_node_type(module, driver))->name);
    fx_close(&fx);
}

/* ff_attrs.il: `(* keep *)` on the process: the $dff has keep (the $proc node's attributes); its
 * record is the process's lowering record, DERIVED with parents [the process's record]. */
static void test_ff_attrs(void) {
    static const char *const k_cells[] = {DFF("1", "clk", "d", "q") " keep=1"};
    fixture fx;
    ff_run(&fx, K_DFF_TEXT("posedge", "  attribute \\keep 1\n"), k_cells, N_OF(k_cells));
    const odin3_pw *pw = fx_pw(&fx, 0);
    odin3_prov_id id = odin3_node_prov(fx.res.module, fx.cells[0]);
    TEST_ASSERT_EQUAL_UINT32(pw->lowered.v, id.v);
    const odin3_prov_record *rec = odin3_prov_get(fx.res.design, id);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_TRUE(pw->prov.v != 0);
    TEST_ASSERT_EQUAL_UINT32(pw->prov.v, rec->parents.ids[0].v);
    fx_close(&fx);
}

/* The helper cells take the lowering record too, and only the register the attributes (Yosys's
 * Module::Not, ReduceOr and Mux add none; proc_dff copies proc->attributes to the register). */
static void test_ff_helper_records(void) {
    fixture fx;
    fx_open(&fx,
            "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
            "  wire output 4 \\q\n  wire \\t0\n  attribute \\keep 1\n  process $p\n" RST_SWITCH(
                "rst") "    sync posedge \\clk\n      update \\q \\t0\n"
                       "    sync posedge \\rst\n      update \\q \\t0\n  end\nend\n");
    fx_lower(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_ff(&fx));
    static const char *const k_cells[] = {OR1("rst", "c0"), ADFF("c0", "d", "q") " keep=1"};
    expect_cells(&fx, k_cells, N_OF(k_cells));
    TEST_ASSERT_EQUAL_UINT32(fx_pw(&fx, 0)->lowered.v,
                             odin3_node_prov(fx.res.module, fx.cells[0]).v);
    fx_close(&fx);
}

/* The live view (§4): a merge made after step A (here by hand, as M1 or M2 could) joins the two
 * resets' values w1 and w2, so the rules are equal and collapse into one $reduce_or trigger and one
 * $aldff (W3 names the first reset). Yosys compares these values as written (§20 #1), so this test
 * has no Yosys run; read as written they would differ and give a $dffsr. */
static void test_ff_live_view(void) {
    static const char *const k_cells[] = {
        "$reduce_or A_SIGNED=0 A_WIDTH=2 Y_WIDTH=1 A=[r1 r2] Y=[c0]",
        "$aldff WIDTH=1 CLK_POLARITY=1 ALOAD_POLARITY=1 CLK=[clk] ALOAD=[c0] AD=[w1] D=[d] Q=[q]"};
    fixture fx;
    fx_open(&fx,
            "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r1\n  wire input 3 \\r2\n"
            "  wire input 4 \\d\n  wire \\w1\n  wire \\w2\n  wire output 5 \\q\n  process $p\n"
            "    sync high \\r1\n      update \\q \\w1\n    sync high \\r2\n      update \\q \\w2\n"
            "    sync posedge \\clk\n      update \\q \\d\n  end\nend\n");
    fx_lower(&fx);
    odin3_net_id w1 = odin3_wire_net(fx.res.module, wire_named(&fx, "w1"), 0);
    odin3_net_id w2 = odin3_wire_net(fx.res.module, wire_named(&fx, "w2"), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_connect(&fx.mctx, w2.v, w1.v));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_apply_merges(&fx.mctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_ff(&fx));
    expect_cells(&fx, k_cells, N_OF(k_cells));
    expect_one_warning("odin3: warning: proc: module 'm': 'q' takes a non-constant value while "
                       "asynchronous reset 'r1' is active; it is built as an asynchronous-load "
                       "register ($aldff)");
    fx_close(&fx);
}

/* --- what step A and the latch phase rule out ----------------------------------------------- */

/* A body that skipped the clock pre-check (E5's and E1b's shapes) or the latch phase (an ALWAYS
 * action left): each is an internal error, never a wrong register. */
static void expect_internal(const char *text, bool latch, const char *want) {
    fixture fx;
    fx_open_with(&fx, text, false);
    fx_step_b(&fx);
    if (latch) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_latches(&fx.mctx));
    }
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_proc_ffs(&fx.mctx, fx_pw(&fx, 0)));
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, lines_at(ODIN3_LOG_ERROR, "", errs, 2));
    TEST_ASSERT_EQUAL_STRING(want, errs[0]);
    check_full(&fx.res);
    fx_close(&fx);
}

static void test_ff_internal_errors(void) {
    expect_internal("module \\m\n  wire input 1 \\en\n  wire input 2 \\d\n  wire output 3 \\q\n"
                    "  process $p\n    sync high \\en\n      update \\q \\d\n  end\nend\n",
                    true, "proc: internal: register without a clock edge");
    expect_internal("module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n"
                    "  process $p\n    sync posedge \\clk\n      update \\q \\d\n"
                    "    sync negedge \\clk\n      update \\q \\d\n  end\nend\n",
                    true, "proc: internal: register with two clock edges");
    expect_internal("module \\m\n  wire input 1 \\a\n  wire output 2 \\y\n  process $p\n"
                    "    sync always\n      update \\y \\a\n  end\nend\n",
                    false, "proc: internal: always sync left for the flip-flop phase");
}

static void test_ff_invalid(void) {
    fixture fx;
    fx_open(&fx, K_DFF_TEXT("posedge", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_ffs(NULL, fx_pw(&fx, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_ffs(&fx.mctx, NULL));
    fx_close(&fx);
}

/* --- out of memory ------------------------------------------------------------------------- */

/* ff_oom.il: one module with every register kind: q ($adff) and u (self reset) in one process,
 * s ($dffsr with W4), l ($aldff with W3) and w (8-bit $dff) in others. */
static const char *const k_oom =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
    "  wire input 4 \\e\n  wire input 5 \\r2\n  wire input 6 \\a\n  wire width 8 input 7 \\dw\n"
    "  wire output 8 \\q\n  wire output 9 \\u\n  wire output 10 \\s\n  wire output 11 \\l\n"
    "  wire width 8 output 12 \\w\n  wire \\t0\n  wire \\u0\n  wire \\s0\n  wire \\l0\n"
    "  wire width 8 \\w0\n  wire \\nr2\n"
    "  cell $logic_not $c1\n" UNARY_PARAMS "    connect \\A \\r2\n    connect \\Y \\nr2\n  end\n"
    "  process $p1\n    assign \\u0 \\u\n    switch \\rst\n"
    "      case 1'1\n        assign \\t0 1'0\n"
    "      case\n        assign \\t0 \\d\n        assign \\u0 \\e\n    end\n"
    "    sync posedge \\clk\n      update \\q \\t0\n      update \\u \\u0\n"
    "    sync posedge \\rst\n      update \\q \\t0\n      update \\u \\u0\n  end\n"
    "  process $p2\n    switch \\rst\n      case 1'1\n        assign \\s0 1'0\n      case\n"
    "        switch \\nr2\n          case 1'1\n            assign \\s0 1'1\n"
    "          case\n            assign \\s0 \\d\n        end\n    end\n"
    "    sync posedge \\clk\n      update \\s \\s0\n"
    "    sync posedge \\rst\n      update \\s \\s0\n"
    "    sync negedge \\r2\n      update \\s \\s0\n  end\n"
    "  process $p3\n    assign \\l0 \\l\n    switch \\rst\n      case 1'1\n"
    "        assign \\l0 \\a\n    end\n"
    "    sync posedge \\clk\n      update \\l \\l0\n"
    "    sync posedge \\rst\n      update \\l \\l0\n  end\n"
    "  process $p4\n    assign \\w0 \\dw\n    sync negedge \\clk\n      update \\w \\w0\n  end\n"
    "end\n";

static char oom_want[MAX_CELLS][DESC_TEXT];
static uint32_t oom_cells;

/*
 * Whether the injected failure fired: probes up to fail + 1 allocations, one of which fails when it
 * did not (fewer than fail + 1 allocations were made). Leaves the hook disabled. A sweep that ends
 * only when the failure did not fire is not cut short by a failure the code absorbs.
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

/* One injection over the flip-flop phase: NO_MEMORY, or the whole result (a failure the phase
 * absorbs gives the whole result too), valid IR (ASan reports a leak at exit). True once the
 * phase makes no more allocations than fail. */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom);
    fx_lower(&fx);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < fx.mctx.pws.len; i++) {
        st = odin3_proc_ffs(&fx.mctx, fx_pw(&fx, i));
    }
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

static void test_ff_oom(void) {
    fixture fx;
    fx_open(&fx, k_oom);
    fx_lower(&fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_ff(&fx));
    TEST_ASSERT_EQUAL_UINT32(2, fx.stats.cells[ODIN3_PROC_K_DFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_ADFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_ALDFF]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.cells[ODIN3_PROC_K_DFFSR]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W3]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.warnings[ODIN3_PROC_W4]);
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
    TEST_ASSERT_TRUE(fail > 1); /* the phase allocates: the sweep ran */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ff_dff);
    RUN_TEST(test_ff_negedge);
    RUN_TEST(test_ff_adff);
    RUN_TEST(test_ff_adff_x_value);
    RUN_TEST(test_ff_adff_rst_n);
    RUN_TEST(test_ff_collapse_two_equal);
    RUN_TEST(test_ff_self_reset);
    RUN_TEST(test_ff_dffsr);
    RUN_TEST(test_ff_dffsr_four_resets);
    RUN_TEST(test_ff_aldff);
    RUN_TEST(test_ff_groups_by_chunk);
    RUN_TEST(test_ff_group_narrowed);
    RUN_TEST(test_ff_edge_chunks);
    RUN_TEST(test_ff_self_clocked);
    RUN_TEST(test_ff_const_clock);
    RUN_TEST(test_ff_attrs);
    RUN_TEST(test_ff_helper_records);
    RUN_TEST(test_ff_live_view);
    RUN_TEST(test_ff_internal_errors);
    RUN_TEST(test_ff_invalid);
    RUN_TEST(test_ff_oom);
    return UNITY_END();
}
