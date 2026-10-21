/*
 * ff.c — the flip-flops of `lower` (3B spec §6.1, LW-7; plan Task 6; PHASE3 decision 19): the
 * eleven word FFs $dff $dffe $sdff $sdffe $sdffce $adff $adffe $aldff $aldffe $dffsr $dffsre become
 * one `$_DFF_P_` per bit (or `$_DFF_N_`) with the next-state logic in front of D. The registry has
 * no async-reset, enable or sync-reset bit cell, so an asynchronous set, reset or load can only be
 * made synchronous (decision 19 (a): Parmys's adff2dff / aldff2dff and Yosys's dffunmap).
 *
 * The simlib meaning of each type, which the next-state functions below reproduce (`$_MUX_(a, b, s)
 * = s ? b : a`; an active-low control swaps the mux arms, so no inverter node is made):
 *
 *   $dff     Q <= D                                       at the active edge
 *   $dffe    Q <= EN ? D : Q
 *   $sdff    Q <= SRST ? SRST_VALUE : D
 *   $sdffe   Q <= SRST ? SRST_VALUE : (EN ? D : Q)        reset over enable
 *   $sdffce  Q <= EN ? (SRST ? SRST_VALUE : D) : Q        enable over reset
 *   $adff    ARST active: Q = ARST_VALUE at once; else Q <= D
 *   $adffe   as $adff, the edge load only if EN
 *   $aldff   ALOAD active: Q = AD at once; else Q <= D
 *   $aldffe  as $aldff, the edge load only if EN
 *   $dffsr   per bit: CLR active: Q = 0; else SET active: Q = 1; else Q <= D
 *   $dffsre  as $dffsr, the edge load only if EN
 *
 * Made synchronous, the async families take the control into the next-state function, over the
 * enable (agent default of spec §6.3: `Q <= ARST ? VALUE : (EN ? D : Q)`, which is what the RTL
 * does). The hidden option --async-enable-first gives Parmys's shape, the enable over the reset.
 *
 * The 68 gate-level flip-flops of ir/cells/dffx.h ($_DFFE_*, $_DFF_<c><r><v>_, $_SDFF*; Yosys
 * 0.55 spelling, from read_json) are bit cells with the same meaning at width 1: they take the
 * shape of the matching word type ($_SDFFE_ as $sdffe, $_DFF_PN0_ as $adff, ...) and are lowered
 * the same way, so a gate-level netlist read from a file reaches write_blif --vtr (spec 5.3
 * follow-up). Their polarity is in the name, not in parameters.
 */
#include "passes/ffinit.h"
#include "passes/lower/lower_int.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* WIDTH and CLK_POLARITY are parameters 0 and 1 of every clocked family cell (ff.c of the IR). */
enum { FF_P_WIDTH = 0, FF_P_CLK_POLARITY = 1, FF_INIT_UNKNOWN = 3 };

/* What a cell does besides D, EN and the clock. */
typedef enum ff_ctl_kind { FF_CTL_NONE, FF_CTL_RESET, FF_CTL_LOAD, FF_CTL_SETCLR } ff_ctl_kind;

/* The shape of a type: its control, and the number of $_MUX_ it needs per bit. */
typedef struct ff_shape {
    const char *type;
    ff_ctl_kind ctl;
    const char *ctl_port;     /* "SRST", "ARST", "ALOAD" (NULL: none or SET/CLR) */
    const char *ctl_polarity; /* the polarity parameter of ctl_port */
    const char *value_name;   /* the *_VALUE parameter (FF_CTL_RESET) */
    const char *controls;     /* the --report text of an async cell, "SET,CLR" for two */
    bool has_en;
    bool async;
    bool en_outer; /* the enable is over the synchronous reset ($sdffce) */
    uint32_t muxes;
} ff_shape;

enum {
    FF_DFF,
    FF_DFFE,
    FF_SDFF,
    FF_SDFFE,
    FF_SDFFCE,
    FF_ADFF,
    FF_ADFFE,
    FF_ALDFF,
    FF_ALDFFE,
    FF_DFFSR,
    FF_DFFSRE,
    FF_COUNT
};

static const ff_shape k_shapes[FF_COUNT] = {
    {"$dff", FF_CTL_NONE, NULL, NULL, NULL, NULL, false, false, false, 0},
    {"$dffe", FF_CTL_NONE, NULL, NULL, NULL, NULL, true, false, false, 1},
    {"$sdff", FF_CTL_RESET, "SRST", "SRST_POLARITY", "SRST_VALUE", NULL, false, false, false, 1},
    {"$sdffe", FF_CTL_RESET, "SRST", "SRST_POLARITY", "SRST_VALUE", NULL, true, false, false, 2},
    {"$sdffce", FF_CTL_RESET, "SRST", "SRST_POLARITY", "SRST_VALUE", NULL, true, false, true, 2},
    {"$adff", FF_CTL_RESET, "ARST", "ARST_POLARITY", "ARST_VALUE", "ARST", false, true, false, 1},
    {"$adffe", FF_CTL_RESET, "ARST", "ARST_POLARITY", "ARST_VALUE", "ARST", true, true, false, 2},
    {"$aldff", FF_CTL_LOAD, "ALOAD", "ALOAD_POLARITY", NULL, "ALOAD", false, true, false, 1},
    {"$aldffe", FF_CTL_LOAD, "ALOAD", "ALOAD_POLARITY", NULL, "ALOAD", true, true, false, 2},
    {"$dffsr", FF_CTL_SETCLR, NULL, NULL, NULL, "SET,CLR", false, true, false, 2},
    {"$dffsre", FF_CTL_SETCLR, NULL, NULL, NULL, "SET,CLR", true, true, false, 3},
};

/* The cell being lowered, by role. */
typedef struct ff_view {
    const ff_shape *shape;
    uint32_t width;
    bool clk_high; /* CLK_POLARITY */
    bool en_outer; /* the enable is over the reset/load */
    odin3_net_id clk;
    lw_sel en;
    lw_sel ctl; /* ARST, SRST or ALOAD */
    bool set_high;
    bool clr_high;
    lw_span data;
    lw_span load; /* AD */
    lw_span set;
    lw_span clr;
    lw_span out; /* the cell's Q nets as they were */
    const odin3_value *value;
    int gate_init; /* INIT of a gate-level cell (0 or 1); -1: from the net's init attribute */
} ff_view;

/* The names of a control: its port and its polarity parameter. */
typedef struct ff_names {
    const char *port;
    const char *polarity;
} ff_names;

static lw_sel sel_of(const lw_cell *cell, const ff_names *names) {
    lw_span span = lw_cell_span(cell, names->port);
    return (lw_sel){span.n > 0 ? span.nets[0] : (odin3_net_id){0},
                    lw_cell_polarity(cell, names->polarity)};
}

static const ff_shape *shape_named(const char *type) {
    for (uint32_t i = 0; i < FF_COUNT; i++) {
        if (strcmp(k_shapes[i].type, type) == 0) {
            return &k_shapes[i];
        }
    }
    return NULL;
}

static void view_load(const lw_ctx *lw, const lw_cell *cell, const ff_shape *shape, ff_view *view) {
    memset(view, 0, sizeof *view);
    view->gate_init = -1;
    view->shape = shape;
    view->width = cell->port[cell->out_port].n;
    view->clk_high = cell->params[FF_P_CLK_POLARITY].i != 0;
    view->clk = lw_cell_span(cell, "CLK").nets[0];
    view->en = sel_of(cell, &(ff_names){shape->has_en ? "EN" : NULL, "EN_POLARITY"});
    view->ctl = sel_of(cell, &(ff_names){shape->ctl_port, shape->ctl_polarity});
    view->set_high = lw_cell_polarity(cell, "SET_POLARITY");
    view->clr_high = lw_cell_polarity(cell, "CLR_POLARITY");
    view->data = lw_cell_span(cell, "D");
    view->load = lw_cell_span(cell, "AD");
    view->set = lw_cell_span(cell, "SET");
    view->clr = lw_cell_span(cell, "CLR");
    view->out = cell->port[cell->out_port];
    view->value = lw_cell_param(cell, shape->value_name);
    view->en_outer = shape->has_en &&
                     (shape->en_outer ||
                      (lw->opts->async_enable_first && shape->async && shape->ctl != FF_CTL_NONE));
}

/* --- the gate-level types (dffx.h) --------------------------------------------------------- */

/* A gate-level type taken apart: the word shape it behaves as, and the letters of its name. */
typedef struct gate_ff {
    int shape;
    bool clk_high;
    bool ctl_high; /* the reset's level (R) */
    bool value;    /* the reset value */
    bool en_high;
} gate_ff;

typedef struct gate_family {
    const char *prefix;
    size_t letters; /* the letters after the prefix, without the closing `_` */
    int shape;
} gate_family;

static const gate_family k_gate_families[] = {
    {"$_DFFE_", 2, FF_DFFE}, {"$_DFFE_", 4, FF_ADFFE},  {"$_DFF_", 3, FF_ADFF},
    {"$_SDFF_", 3, FF_SDFF}, {"$_SDFFE_", 4, FF_SDFFE}, {"$_SDFFCE_", 4, FF_SDFFCE},
};

static bool letters_valid(const char *letters, size_t count, const gate_ff *gate) {
    if (strlen(letters) != count + 1 || letters[count] != '_') {
        return false;
    }
    bool has_reset = gate->shape != FF_DFFE;
    for (size_t i = 0; i < count; i++) {
        bool value_slot = has_reset && i == 2;
        const char *allowed = value_slot ? "01" : "PN";
        if (strchr(allowed, letters[i]) == NULL || letters[i] == '\0') {
            return false;
        }
    }
    return true;
}

/* True when name is one of the 68 types of dffx.h; *gate says which. */
static bool gate_parse(const char *name, gate_ff *gate) {
    for (size_t i = 0; i < sizeof k_gate_families / sizeof k_gate_families[0]; i++) {
        const gate_family *fam = &k_gate_families[i];
        size_t len = strlen(fam->prefix);
        *gate = (gate_ff){.shape = fam->shape};
        if (strncmp(name, fam->prefix, len) != 0 ||
            !letters_valid(name + len, fam->letters, gate)) {
            continue;
        }
        const char *at = name + len;
        gate->clk_high = at[0] == 'P';
        if (fam->shape == FF_DFFE) {
            gate->en_high = at[1] == 'P';
            return true;
        }
        gate->ctl_high = at[1] == 'P';
        gate->value = at[2] == '1';
        gate->en_high = fam->letters == 4 && at[3] == 'P';
        return true;
    }
    return false;
}

bool lw_ff_is_async(const char *type_name) {
    const ff_shape *shape = shape_named(type_name);
    gate_ff gate;
    if (gate_parse(type_name, &gate)) {
        return k_shapes[gate.shape].async;
    }
    return shape != NULL && shape->async;
}

bool lw_ff_gate_name(const char *type_name) {
    gate_ff gate;
    return gate_parse(type_name, &gate);
}

static const uint8_t k_gate_bit[2] = {ODIN3_BIT_0, ODIN3_BIT_1};
static const odin3_value k_gate_value[2] = {{ODIN3_VAL_BITS, 0, &k_gate_bit[0], 1, 0, 0},
                                            {ODIN3_VAL_BITS, 0, &k_gate_bit[1], 1, 0, 0}};

static lw_sel gate_sel(const lw_cell *cell, const char *port, bool high) {
    lw_span span = lw_cell_span(cell, port);
    return (lw_sel){span.n > 0 ? span.nets[0] : (odin3_net_id){0}, high};
}

static odin3_status view_load_gate(const lw_ctx *lw, const lw_cell *cell, const gate_ff *gate,
                                   ff_view *view) {
    memset(view, 0, sizeof *view);
    lw_span clk = lw_cell_span(cell, "C");
    lw_span data = lw_cell_span(cell, "D");
    if (clk.n == 0 || data.n == 0) {
        odin3_log(ODIN3_LOG_ERROR, "lower: gate-level FF '%s' has no %s pin", cell->def->name,
                  clk.n == 0 ? "C" : "D");
        return ODIN3_ERR_INVALID_ARG;
    }
    view->shape = &k_shapes[gate->shape];
    view->width = 1;
    view->clk_high = gate->clk_high;
    view->clk = clk.nets[0];
    view->en = gate_sel(cell, view->shape->has_en ? "E" : NULL, gate->en_high);
    view->ctl = gate_sel(cell, view->shape->ctl == FF_CTL_RESET ? "R" : NULL, gate->ctl_high);
    view->data = data;
    view->out = cell->port[cell->out_port];
    view->value = &k_gate_value[gate->value ? 1 : 0];
    view->en_outer = view->shape->en_outer ||
                     (view->shape->has_en && lw->opts->async_enable_first && view->shape->async &&
                      view->shape->ctl != FF_CTL_NONE);
    view->gate_init =
        cell->params[0].kind == ODIN3_VAL_INT && cell->params[0].i >= 0 && cell->params[0].i <= 1
            ? (int)cell->params[0].i
            : -1;
    return ODIN3_OK;
}

/* --- the next-state function (spec §6.1's table) ------------------------------------------------
 */

/* EN ? net : q. */
static odin3_status enable_step(lw_ctx *lw, const ff_view *view, odin3_net_id net,
                                odin3_net_id qnet, odin3_net_id *out) {
    if (!view->shape->has_en) {
        *out = net;
        return ODIN3_OK;
    }
    return lw_pick(lw, view->en, net, qnet, out);
}

/* The constant net of bit `bit` of a *_VALUE (x and z give the x constant). */
static odin3_status value_net(lw_ctx *lw, const ff_view *view, uint32_t bit, odin3_net_id *out) {
    odin3_bit val = (odin3_bit)view->value->bits[bit];
    return lw_const(lw, val == ODIN3_BIT_0 || val == ODIN3_BIT_1 ? val : ODIN3_BIT_X, out);
}

/* $dffsr: CLR[bit] ? 0 : (SET[bit] ? 1 : net). */
static odin3_status setclr_step(lw_ctx *lw, const ff_view *view, uint32_t bit, odin3_net_id net,
                                odin3_net_id *out) {
    odin3_net_id zero = {0};
    odin3_net_id one = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    st = st == ODIN3_OK ? lw_const(lw, ODIN3_BIT_1, &one) : st;
    lw_sel set = {view->set.nets[bit], view->set_high};
    lw_sel clr = {view->clr.nets[bit], view->clr_high};
    st = st == ODIN3_OK ? lw_pick(lw, set, one, net, &net) : st;
    return st == ODIN3_OK ? lw_pick(lw, clr, zero, net, out) : st;
}

/* The reset, load or set/clear step over `net`. */
static odin3_status control_step(lw_ctx *lw, const ff_view *view, uint32_t bit, odin3_net_id net,
                                 odin3_net_id *out) {
    odin3_net_id arm = {0};
    odin3_status st = ODIN3_OK;
    switch (view->shape->ctl) {
    case FF_CTL_RESET:
        st = value_net(lw, view, bit, &arm);
        return st == ODIN3_OK ? lw_pick(lw, view->ctl, arm, net, out) : st;
    case FF_CTL_LOAD:
        return lw_pick(lw, view->ctl, view->load.nets[bit], net, out);
    case FF_CTL_SETCLR:
        return setclr_step(lw, view, bit, net, out);
    case FF_CTL_NONE:
    default:
        break;
    }
    *out = net;
    return ODIN3_OK;
}

/* N[bit] of spec §6.1: what Q[bit] takes at the active edge; `q` is the storage node's own Q net
 * (the feedback arm of the enable). */
static odin3_status lw_next_state(lw_ctx *lw, const ff_view *view, uint32_t bit, odin3_net_id qnet,
                                  odin3_net_id *out) {
    odin3_net_id net = view->data.nets[bit];
    odin3_status st = ODIN3_OK;
    if (view->en_outer) {
        st = control_step(lw, view, bit, net, &net);
        return st == ODIN3_OK ? enable_step(lw, view, net, qnet, out) : st;
    }
    st = enable_step(lw, view, net, qnet, &net);
    return st == ODIN3_OK ? control_step(lw, view, bit, net, out) : st;
}

/* --- the storage node --------------------------------------------------------------------------
 */

/* One storage node (spec §6.1): `$_DFF_P_` or, with `negative`, `$_DFF_N_`, on the clock net `clk`,
 * D = `next`, Q = `q` (made by lw_net), INIT = `init` (0, 1 or x -> 3). */
typedef struct lw_storage_args {
    bool negative;
    odin3_net_id clk;
    odin3_net_id next;
    odin3_bit init;
} lw_storage_args;

static odin3_status lw_storage(lw_ctx *lw, const lw_storage_args *args, odin3_net_id qnet) {
    int64_t init = args->init == ODIN3_BIT_0 || args->init == ODIN3_BIT_1 ? (int64_t)args->init
                                                                          : FF_INIT_UNKNOWN;
    const odin3_value param = odin3_value_int(init);
    const odin3_net_id in[2] = {args->clk, args->next};
    const lw_gate_spec spec = {args->negative ? LW_G_DFF_N : LW_G_DFF_P, in, 2, &param};
    return lw_gate_on(lw, &spec, qnet);
}

/* --- the cell ----------------------------------------------------------------------------------
 */

static const char *module_text(const lw_ctx *lw) {
    return odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module));
}

/* The clock of the storage nodes: CLK itself, or (a falling edge, --neg-edge invert) one $_NOT_
 * of it shared by the bits; *negative says the nodes are $_DFF_N_ (--neg-edge keep). */
static odin3_status clock_for(lw_ctx *lw, const ff_view *view, odin3_net_id *clk, bool *negative) {
    *clk = view->clk;
    *negative = false;
    if (view->clk_high) {
        return ODIN3_OK;
    }
    if (lw->opts->keep_neg_edge) {
        *negative = true;
        return ODIN3_OK;
    }
    return lw_not(lw, view->clk, clk);
}

/* One bit: its Q net, the next-state logic, the storage node. */
static odin3_status build_bit(lw_ctx *lw, const ff_view *view, uint32_t bit,
                              const lw_storage_args *base, odin3_net_id *qnet) {
    odin3_status st = lw_net(lw, qnet);
    lw_storage_args args = *base;
    st = st == ODIN3_OK ? lw_next_state(lw, view, bit, *qnet, &args.next) : st;
    odin3_net_id old = bit < view->out.n ? view->out.nets[bit] : (odin3_net_id){0};
    args.init = odin3_net_valid(old) ? odin3_ffinit_get(lw->module, old) : ODIN3_BIT_X;
    if (view->gate_init >= 0) {
        args.init = view->gate_init == 1 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    return st == ODIN3_OK ? lw_storage(lw, &args, *qnet) : st;
}

/* The warning, the report entry and the totals of a finished cell. */
static odin3_status book(lw_ctx *lw, const lw_cell *cell, const ff_view *view) {
    char label[LW_LABEL_LEN];
    lw_node_label(lw, cell->node, label, sizeof label);
    lw_report_entry entry = {module_text(lw), label, cell->def->name, view->width, NULL};
    odin3_status st = ODIN3_OK;
    if (!view->clk_high && !lw->opts->keep_neg_edge) {
        odin3_log(ODIN3_LOG_WARN,
                  "lower: module '%s': falling-edge '%s' '%s' gets a clock inverter", entry.module,
                  entry.type, label);
        st = lw_report_add(lw, LW_REP_NEG_EDGE, &entry);
    }
    if (st == ODIN3_OK && view->shape->async) {
        entry.controls = view->shape->controls;
        st = lw_report_add(lw, LW_REP_ASYNC, &entry);
        lw->totals.async_bits += view->width;
        lw->totals.async_cells++;
    }
    return st;
}

static odin3_status build_view(lw_ctx *lw, const lw_cell *cell, ff_view view, lw_bits *out) {
    lw_storage_args base = {false, view.clk, view.clk, ODIN3_BIT_X};
    odin3_status st = clock_for(lw, &view, &base.clk, &base.negative);
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < view.width; bit++) {
        odin3_net_id qnet = {0};
        st = build_bit(lw, &view, bit, &base, &qnet);
        st = st == ODIN3_OK ? lw_bits_push(out, qnet) : st;
    }
    return st == ODIN3_OK ? book(lw, cell, &view) : st;
}

static odin3_status build_ff(lw_ctx *lw, const lw_cell *cell, const ff_shape *shape, lw_bits *out) {
    ff_view view;
    view_load(lw, cell, shape, &view);
    return build_view(lw, cell, view, out);
}

/* The exact gate count: per bit the muxes and the storage node, one $_NOT_ for a falling edge
 * (--neg-edge invert; counted also for keep, an upper bound). */
static uint64_t ff_estimate(const lw_cell *cell, const ff_shape *shape) {
    uint64_t width = cell->port[cell->out_port].n;
    uint64_t inverter = cell->params[FF_P_CLK_POLARITY].i == 0 ? 1U : 0U;
    return lw_sat_add(lw_sat_mul(width, (uint64_t)shape->muxes + 1U), inverter);
}

/* The 22 entry points of the dispatch table. */
#define FF_RULE(fn, kind)                                                                          \
    odin3_status lw_build_##fn(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {                    \
        return build_ff(lw, cell, &k_shapes[kind], out);                                           \
    }                                                                                              \
    uint64_t lw_est_##fn(const lw_cell *cell) {                                                    \
        return ff_estimate(cell, &k_shapes[kind]);                                                 \
    }

FF_RULE(dff, FF_DFF)
FF_RULE(dffe, FF_DFFE)
FF_RULE(sdff, FF_SDFF)
FF_RULE(sdffe, FF_SDFFE)
FF_RULE(sdffce, FF_SDFFCE)
FF_RULE(adff, FF_ADFF)
FF_RULE(adffe, FF_ADFFE)
FF_RULE(aldff, FF_ALDFF)
FF_RULE(aldffe, FF_ALDFFE)
FF_RULE(dffsr, FF_DFFSR)
FF_RULE(dffsre, FF_DFFSRE)

/* The entry points of the gate-level types: every one of dffx.h (rule found by lw_rule_find). */
odin3_status lw_build_gate_ff(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    gate_ff gate;
    ff_view view;
    if (!gate_parse(cell->def->name, &gate)) {
        odin3_log(ODIN3_LOG_ERROR, "lower: no lowering rule for cell type '%s'", cell->def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = view_load_gate(lw, cell, &gate, &view);
    if (st != ODIN3_OK) {
        return st;
    }
    return build_view(lw, cell, view, out);
}

uint64_t lw_est_gate_ff(const lw_cell *cell) {
    gate_ff gate;
    if (!gate_parse(cell->def->name, &gate)) {
        return 0;
    }
    return (uint64_t)k_shapes[gate.shape].muxes + 1U + (gate.clk_high ? 0U : 1U);
}

/* --- --async error -----------------------------------------------------------------------------
 */

bool lw_ff_async_refused(const lw_ctx *lw, odin3_node_id node, const char *type_name) {
    if (!lw->opts->async_is_error || !lw_ff_is_async(type_name)) {
        return false;
    }
    char label[LW_LABEL_LEN];
    lw_node_label(lw, node, label, sizeof label);
    odin3_log(ODIN3_LOG_ERROR, "lower: module '%s': asynchronous control on '%s' '%s'",
              module_text(lw), type_name, label);
    return true;
}
