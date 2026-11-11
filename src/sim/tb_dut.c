/* tb_dut.c — binds a testbench instance to a module of the design simulated by odin3_sim (spec
 * sections 1 and 4). */
#include "ir/module.h"
#include "sim/sim.h"
#include "sim/tb_internal.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EDGE_CAP = 64,  /* clock edges recorded between two NBA regions */
    MAX_CLOCKS = 16 /* clock inputs of one DUT instance */
};

/* One clock input of the DUT: the connection that drives it, and the code of its bit 0 at the last
 * observation (starts x in 4-state mode, as the testbench reg; 0 in 2-state mode). */
typedef struct dut_clock {
    int conn; /* index in inst->conns */
    uint32_t last;
} dut_clock;

/* One recorded edge: which clock (index in clocks), and whether it rose. */
typedef struct dut_edge {
    uint8_t clock;
    bool rise;
} dut_edge;

struct tb_dut {
    odin3_sim *sim;
    tb_inst *inst;
    bool four_state;
    tbv *last;  /* per connection: the value last applied (input) or published (output) */
    bool *seen; /* per connection: last holds a value */
    dut_clock clocks[MAX_CLOCKS];
    uint32_t nclocks;
    dut_edge edges[EDGE_CAP]; /* recorded edges in order */
    uint32_t nedges;
};

static bool find_module(odin3_design *design, const char *name, odin3_module_id *out) {
    for (uint32_t id = 1; id < odin3_design_module_end(design); id++) {
        odin3_module_id mid = {id};
        const odin3_module *mod = odin3_module_get(design, mid);
        if (mod != NULL &&
            strcmp(odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(mod)), name) ==
                0) {
            *out = mid;
            return true;
        }
    }
    return false;
}

bool tb_design_has_module(odin3_design *design, const char *name) {
    odin3_module_id id;
    return design != NULL && find_module(design, name, &id);
}

typedef struct param_walk {
    odin3_design *design;
    uint32_t want, seen;
    bool found;
    tb_param_def *out;
} param_walk;

/* The value as tb bits: BITS as they are, INT as 64 bits, STRING 8 bits per character (first
 * character in the high byte). */
static void param_bits(odin3_design *design, const odin3_value *value, tb_param_def *out) {
    uint32_t width = 0;
    if (value->kind == ODIN3_VAL_BITS) {
        width = value->len > (uint32_t)TB_MAX_WIDTH ? (uint32_t)TB_MAX_WIDTH : value->len;
        tbv_zero(&out->val, width);
        for (uint32_t i = 0; i < width; i++) {
            tbv code;
            tbv_set_code(value->bits[i], &code, 1);
            tb_field_put((tb_planes){out->val.v, out->val.u, width}, (tb_field){i, 1}, &code);
        }
    } else if (value->kind == ODIN3_VAL_STRING) {
        const char *text = odin3_strtab_get(odin3_design_strtab(design), value->str);
        size_t len = text != NULL ? strlen(text) : 0;
        len = len > (size_t)TB_MAX_WIDTH / 8U ? (size_t)TB_MAX_WIDTH / 8U : len;
        width = len == 0 ? 8U : (uint32_t)(len * 8U);
        tbv_zero(&out->val, width);
        for (size_t i = 0; i < len; i++) {
            tbv byte;
            tbv_set_u64((uint8_t)text[i], &byte, 8);
            tb_field_put((tb_planes){out->val.v, out->val.u, width},
                         (tb_field){(uint32_t)((len - 1U - i) * 8U), 8}, &byte);
        }
    } else {
        width = 64;
        tbv_set_u64((uint64_t)value->i, &out->val, width);
    }
    out->width = width;
}

static odin3_status param_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    param_walk *pw = ctx;
    const char *key = odin3_strtab_get(odin3_design_strtab(pw->design), key_str);
    size_t pre = strlen(ODIN3_ATTR_PARAM_DEFAULT_PREFIX);
    if (pw->found || key == NULL || strncmp(key, ODIN3_ATTR_PARAM_DEFAULT_PREFIX, pre) != 0 ||
        pw->seen++ != pw->want) {
        return ODIN3_OK;
    }
    (void)snprintf(pw->out->name, sizeof pw->out->name, "%s", key + pre);
    param_bits(pw->design, value, pw->out);
    pw->found = true;
    return ODIN3_OK;
}

bool tb_dut_param_at(odin3_design *design, const char *module, uint32_t index, tb_param_def *out) {
    odin3_module_id id = {0};
    if (design == NULL || !find_module(design, module, &id)) {
        return false;
    }
    param_walk pw = {design, index, 0, false, out};
    (void)odin3_attr_foreach(odin3_module_get(design, id), (odin3_objref){ODIN3_OBJ_MODULE, id.v},
                             param_visit, &pw);
    return pw.found;
}

static bool port_lookup(const odin3_sim *sim, const char *name, tb_conn *conn) {
    for (uint32_t i = 0; i < odin3_sim_input_count(sim); i++) {
        if (strcmp(odin3_sim_input_name(sim, i), name) == 0) {
            conn->sim_port = i;
            conn->is_out = false;
            conn->width = odin3_sim_input_width(sim, i);
            return true;
        }
    }
    for (uint32_t i = 0; i < odin3_sim_output_count(sim); i++) {
        if (strcmp(odin3_sim_output_name(sim, i), name) == 0) {
            conn->sim_port = i;
            conn->is_out = true;
            conn->width = odin3_sim_output_width(sim, i);
            return true;
        }
    }
    return false;
}

/* Registers conn as a clock of the DUT when its port is one. False (logged) for a clock bit in a
 * port wider than one bit (the other bits would never be driven), and for too many clocks. */
static bool bind_clock(tb_dut *dut, tb_conn *conn, uint32_t line) {
    const char *mod = dut->inst->module;
    char where[TB_WHERE_MAX];
    for (uint32_t bit_no = 0; conn->width > 1 && bit_no < conn->width; bit_no++) {
        odin3_sim_bit high = {conn->sim_port, bit_no};
        if (odin3_sim_input_is_clock(dut->sim, high)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s: module `%s` port `%s` is %u bits wide and bit %u is a clock; a "
                      "clock must be a one-bit port (split the port)",
                      tb_where("testbench", line, where, sizeof where), mod, conn->port,
                      conn->width, bit_no);
            return false;
        }
    }
    odin3_sim_bit bit = {conn->sim_port, 0};
    if (odin3_sim_input_is_clock(dut->sim, bit)) {
        if (dut->nclocks == MAX_CLOCKS) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s: module `%s` has more than %d clock inputs; `%s` is one too many",
                      tb_where("testbench", line, where, sizeof where), mod, MAX_CLOCKS,
                      conn->port);
            return false;
        }
        uint32_t start = dut->four_state ? (uint32_t)ODIN3_W4_X : (uint32_t)ODIN3_W4_0;
        dut->clocks[dut->nclocks++] = (dut_clock){(int)(conn - dut->inst->conns), start};
    }
    return true;
}

static bool bind_conn(tb_dut *dut, tb_conn *conn, uint32_t line) {
    const char *mod = dut->inst->module;
    char where[TB_WHERE_MAX];
    if (conn->port[0] == '\0') {
        tb_error("testbench", line, "ordered port connections (use .port(expr))");
        return false;
    }
    if (!port_lookup(dut->sim, conn->port, conn)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: module `%s` has no input or output port `%s`",
                  tb_where("testbench", line, where, sizeof where), mod, conn->port);
        return false;
    }
    if (conn->width > TB_MAX_WIDTH) {
        tb_error("testbench", line, "DUT port wider than 1024 bits");
        return false;
    }
    if (conn->is_out && conn->expr != NULL && conn->expr->kind != E_VAR) {
        tb_error("testbench", line, "output port connected to anything but a whole wire or reg");
        return false;
    }
    if (!conn->is_out && !bind_clock(dut, conn, line)) {
        return false;
    }
    return true;
}

/* With several clock inputs a cell that has no clock pin (a global-clock `$_FF_`) belongs to none
 * of them, so no edge would ever reach it: refused, naming the DUT and the cell. */
static bool check_global_clock(const tb_dut *dut) {
    char where[TB_WHERE_MAX];
    const char *first = "";
    uint32_t count = odin3_sim_global_clock_cells(dut->sim, &first);
    if (dut->nclocks < 2 || count == 0) {
        return true;
    }
    odin3_log(ODIN3_LOG_ERROR,
              "%s: DUT `%s` (module `%s`) has %u clock inputs and %u cell(s) with no "
              "clock pin (first: %s, a global clock, e.g. a BLIF `.latch` with no control); it "
              "would never be clocked. Give the latch a control (`.latch in out re clk 0`) or "
              "use one clock input",
              tb_where("testbench", dut->inst->line, where, sizeof where), dut->inst->name,
              dut->inst->module, dut->nclocks, count, first);
    return false;
}

odin3_status tb_dut_open(odin3_design *design, const tb_module *tb, tb_inst *inst, tb_dut **out) {
    *out = NULL;
    tb_dut *dut = calloc(1, sizeof *dut);
    if (dut == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    dut->inst = inst;
    dut->four_state = tb->four_state;
    dut->last = calloc((size_t)inst->nconns + 1U, sizeof *dut->last);
    dut->seen = calloc((size_t)inst->nconns + 1U, sizeof *dut->seen);
    if (dut->last == NULL || dut->seen == NULL) {
        tb_dut_close(dut);
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = tb_inst_sim(design, inst, tb->four_state);
    dut->sim = inst->sim;
    if (st == ODIN3_OK) {
        odin3_sim_reset(dut->sim); /* built at parse time, or kept from an earlier run */
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < inst->nconns; i++) {
        st = bind_conn(dut, &inst->conns[i], inst->line) ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
    }
    st = st != ODIN3_OK || check_global_clock(dut) ? st : ODIN3_ERR_INVALID_ARG;
    if (st != ODIN3_OK) {
        tb_dut_close(dut);
        return st;
    }
    *out = dut;
    return ODIN3_OK;
}

void tb_dut_close(tb_dut *dut) {
    if (dut != NULL) {
        free(dut->last);
        free(dut->seen);
    }
    free(dut);
}

odin3_status tb_inst_sim(odin3_design *design, tb_inst *inst, bool four_state) {
    if (inst->sim != NULL) {
        return ODIN3_OK;
    }
    char where[TB_WHERE_MAX];
    odin3_module_id id = {0};
    if (design == NULL || !find_module(design, inst->module, &id)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: no module `%s` in the design (read the DUT first)",
                  tb_where("testbench", inst->line, where, sizeof where), inst->module);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_sim_options opts = {0};
    opts.four_state = four_state;
    return odin3_sim_build_opts(design, id, &opts, &inst->sim);
}

odin3_status tb_inst_find(tb_module *tb, tb_inst *inst, const char *path, uint32_t *id) {
    odin3_status st = tb_inst_sim(tb->design, inst, tb->four_state);
    if (st == ODIN3_OK && inst->sigs == NULL) {
        st = odin3_sim_signals_create(inst->sim, &inst->sigs);
    }
    *id = 0;
    return st != ODIN3_OK ? st : odin3_sim_signals_find(inst->sigs, odin3_bytes_cstr(path), id);
}

odin3_status tb_inst_signal(tb_module *tb, tb_inst *inst, const char *path, tb_src_loc loc,
                            tb_hier_sig *out) {
    char where[TB_WHERE_MAX];
    uint32_t id = 0;
    odin3_status st = tb_inst_find(tb, inst, path, &id);
    const odin3_sim_signal *sig =
        st == ODIN3_OK && id != 0 ? odin3_sim_signals_at(inst->sigs, id) : NULL;
    if (sig == NULL) {
        if (st == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s: DUT `%s` (module `%s`) has no signal `%s` (a memory word has a name "
                      "only after `lower --keep-mem`, as `mem[3]`; `opt` removes the name of a "
                      "memory nothing reads)",
                      tb_where(loc.file, loc.line, where, sizeof where), inst->name, inst->module,
                      path);
        }
        return st == ODIN3_OK ? ODIN3_ERR_INVALID_ARG : st;
    }
    if (sig->width > TB_MAX_WIDTH) {
        odin3_log(ODIN3_LOG_ERROR, "%s: signal `%s` of DUT `%s` is wider than 1024 bits",
                  tb_where(loc.file, loc.line, where, sizeof where), path, inst->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (tb_hier_sig){id, sig->width};
    return ODIN3_OK;
}

enum { ARRAY_MAX = 1U << 20, ARRAY_GAP = 64, ARRAY_FIRST_CAP = 64, HIER_WORD_PATH = 280 };

/* Word `index` of the array: its signal ID, 0 when the DUT has none (a word `opt` folded to a
 * constant); false on an error (out of memory, a width past the limit or different from the
 * others). */
static bool array_word(const tb_array_ctx *ac, uint32_t index, uint32_t *id) {
    char word[HIER_WORD_PATH];
    (void)snprintf(word, sizeof word, "%s[%u]", ac->path, index);
    *id = 0;
    if (tb_inst_find(ac->tb, ac->inst, word, id) != ODIN3_OK) {
        return false;
    }
    const odin3_sim_signal *sig = *id != 0 ? odin3_sim_signals_at(ac->inst->sigs, *id) : NULL;
    if (sig == NULL) {
        *id = 0;
        return true;
    }
    if (sig->width > TB_MAX_WIDTH || (ac->out->width != 0 && sig->width != ac->out->width)) {
        *id = 0;
        return false;
    }
    ac->out->width = sig->width;
    return true;
}

/* Room for word `index` in out->ids (zero-filled), *cap words now; false on out of memory. */
static bool array_room(tb_hier_arr *out, uint32_t *cap, uint32_t index) {
    if (index < *cap) {
        return true;
    }
    uint32_t want = *cap == 0 ? (uint32_t)ARRAY_FIRST_CAP : *cap;
    while (want <= index) {
        want *= 2U;
    }
    uint32_t *grown = calloc(want, sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (out->ids != NULL) {
        memcpy(grown, out->ids, (size_t)*cap * sizeof *grown);
        free(out->ids);
    }
    out->ids = grown;
    *cap = want;
    return true;
}

odin3_status tb_inst_array(tb_module *tb, tb_inst *inst, const char *path, tb_hier_arr *out) {
    *out = (tb_hier_arr){NULL, 0, 0};
    tb_array_ctx ac = {tb, inst, path, out};
    uint32_t cap = 0;
    uint32_t misses = 0;
    bool more = true;
    for (uint32_t index = 0; more && index < ARRAY_MAX && misses < ARRAY_GAP; index++) {
        uint32_t id = 0;
        more = array_word(&ac, index, &id);
        misses = id == 0 ? misses + 1U : 0U;
        if (more && id != 0) {
            if (!array_room(out, &cap, index)) {
                free(out->ids);
                *out = (tb_hier_arr){NULL, 0, 0};
                return ODIN3_ERR_NO_MEMORY;
            }
            out->ids[index] = id;
            out->n = index + 1U;
        }
    }
    return ODIN3_OK;
}

bool tb_inst_poke(tb_inst *inst, uint32_t id, const tbv *val, tb_poke_err err) {
    const odin3_sim_signal *sig = odin3_sim_signals_at(inst->sigs, id);
    uint8_t codes[TB_MAX_WIDTH];
    const char *why = NULL;
    if (sig == NULL || sig->width > TB_MAX_WIDTH) {
        return false;
    }
    bool four = odin3_sim_is_four_state(inst->sim);
    for (uint32_t k = 0; k < sig->width; k++) {
        uint32_t code = tbv_code(val, k);
        codes[k] = (uint8_t)(four ? code : (uint32_t)(code == 1U)); /* 2-state: x and z read 0 */
    }
    if (odin3_sim_signals_poke(inst->sigs, id, codes, sig->width, &why) == ODIN3_OK) {
        inst->poked = true;
        return true;
    }
    (void)snprintf(err.text, err.cap,
                   "cannot set `%s` of the DUT: %s (the memory must be kept by `lower --keep-mem`; "
                   "`opt` may have removed it)",
                   odin3_sim_signals_text(inst->sigs, sig->name),
                   why != NULL ? why : "poke failed");
    return false;
}

void tb_sig_peek(const odin3_sim_signals *sigs, uint32_t id, tbv *out) {
    const odin3_sim_signal *sig = odin3_sim_signals_at(sigs, id);
    uint8_t bits[TB_MAX_WIDTH]; /* only the first width bytes are read, after the read fills them */
    uint32_t width = sig != NULL ? sig->width : 1U;
    tbv_zero(out, width);
    if (sig == NULL || odin3_sim_signals_read(sigs, id, bits, sig->width) != ODIN3_OK) {
        return;
    }
    for (uint32_t k = 0; k < width; k++) {
        odin3_w4_set_bit((odin3_w4){out->v, out->u}, (odin3_w4_bit_at){k, bits[k] & 3U});
    }
}

void tb_inst_peek(const tb_inst *inst, uint32_t id, tbv *out) {
    tb_sig_peek(inst->sigs, id, out);
}

void tb_inst_release(tb_inst *inst) {
    odin3_sim_signals_destroy(inst->sigs);
    odin3_sim_destroy(inst->sim);
    inst->sigs = NULL;
    inst->sim = NULL;
}

static bool is_clock_conn(const tb_dut *dut, uint32_t conn) {
    for (uint32_t k = 0; k < dut->nclocks; k++) {
        if (dut->clocks[k].conn == (int)conn) {
            return true;
        }
    }
    return false;
}

static bool apply_inputs(tb_ev *ev, tb_dut *dut) {
    bool changed = false;
    for (uint32_t i = 0; i < dut->inst->nconns; i++) {
        const tb_conn *conn = &dut->inst->conns[i];
        if (conn->is_out || is_clock_conn(dut, i) || conn->expr == NULL) {
            continue;
        }
        tbv val;
        tb_eval_assign(ev, conn->expr, conn->width, &val);
        if (dut->seen[i] && tbv_same(&val, &dut->last[i], conn->width)) {
            continue;
        }
        for (uint32_t bit_no = 0; bit_no < conn->width; bit_no++) {
            odin3_sim_bit bit = {conn->sim_port, bit_no};
            uint32_t code = tbv_code(&val, bit_no);
            /* 2-state: an x or z bit reads 0, as the testbench's old 2-state values did */
            (void)odin3_sim_set_input4(dut->sim, bit,
                                       (uint8_t)(dut->four_state ? code : code == 1U));
        }
        tbv_copy(&dut->last[i], &val, conn->width);
        dut->seen[i] = true;
        changed = true;
    }
    return changed;
}

static void observe_clock(tb_ev *ev, tb_dut *dut, uint32_t which) {
    dut_clock *clock = &dut->clocks[which];
    const tb_expr *clk = dut->inst->conns[clock->conn].expr;
    if (clk == NULL) {
        return;
    }
    tbv val;
    tb_eval(ev, clk, clk->width, false, &val);
    uint32_t code = tbv_code(&val, 0);
    if (!dut->four_state) {
        code = code == ODIN3_W4_1 ? (uint32_t)ODIN3_W4_1 : (uint32_t)ODIN3_W4_0;
    }
    uint32_t before = clock->last;
    if (code == before) {
        return;
    }
    clock->last = code;
    /* the 4-state edge rule: toward 1 is a posedge (0 to x, x to 1, ...), toward 0 a negedge; x to
     * z and z to x are neither */
    bool rise = before == ODIN3_W4_0 || code == ODIN3_W4_1;
    bool fall = before == ODIN3_W4_1 || code == ODIN3_W4_0;
    if (!rise && !fall) {
        return;
    }
    if (dut->nedges == EDGE_CAP) {
        ev->failed = true;
        ev->why = "more than 64 clock edges between two NBA regions";
        return;
    }
    dut->edges[dut->nedges++] = (dut_edge){(uint8_t)which, rise};
}

void tb_dut_observe(tb_ev *ev, tb_dut *dut) {
    for (uint32_t k = 0; k < dut->nclocks; k++) {
        observe_clock(ev, dut, k);
    }
}

static bool publish(tb_ev *ev, tb_dut *dut) {
    bool changed = false;
    for (uint32_t i = 0; i < dut->inst->nconns; i++) {
        const tb_conn *conn = &dut->inst->conns[i];
        if (!conn->is_out || conn->expr == NULL) {
            continue;
        }
        tbv val;
        tbv_zero(&val, conn->width);
        for (uint32_t bit_no = 0; bit_no < conn->width; bit_no++) {
            uint8_t code = 0;
            odin3_sim_bit at = {conn->sim_port, bit_no};
            (void)odin3_sim_get_output4(dut->sim, at, &code);
            odin3_w4_set_bit((odin3_w4){val.v, val.u}, (odin3_w4_bit_at){bit_no, code});
        }
        if (dut->seen[i] && tbv_same(&val, &dut->last[i], conn->width)) {
            continue;
        }
        tbv_copy(&dut->last[i], &val, conn->width);
        dut->seen[i] = true;
        changed = true;
        if (conn->drv != 0) {
            (void)tb_drive(ev, conn->drv, &val);
        } else {
            tb_resize rs = {conn->width, conn->expr->width, false, false};
            tbv_extend(&val, &rs);
            tb_write(ev, conn->expr, &val);
        }
    }
    return changed;
}

/* Fails the run when the DUT's simulator reported an error (a derived clock that never settles). */
static void dut_status(tb_ev *ev, odin3_status st) {
    if (st != ODIN3_OK) {
        ev->failed = true;
        ev->why = "a derived clock of the DUT does not settle (a register clocks itself)";
    }
}

bool tb_dut_comb(tb_ev *ev, tb_dut *dut) {
    bool poked = dut->inst->poked;
    dut->inst->poked = false;
    if (apply_inputs(ev, dut) || poked) {
        dut_status(ev, odin3_sim_settle_events(dut->sim));
    }
    return publish(ev, dut);
}

/* Replays edges[0..count) of one time step. A DUT with one clock input connected sends each edge
 * to every clock of the design, as the cycle simulator does. With several, the edges of different
 * clocks act together (the cells sample the inputs as the active region left them, whichever
 * clock they hang on); a second edge of the same clock starts a new group. */
static odin3_status replay_edges(tb_dut *dut) {
    odin3_sim_clock_event group[MAX_CLOCKS];
    uint32_t used = 0;
    uint32_t mask = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < dut->nedges; i++) {
        dut_edge edge = dut->edges[i];
        odin3_sim_event event = edge.rise ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE;
        if (dut->nclocks == 1) {
            st = odin3_sim_clock_edge(dut->sim, event);
            continue;
        }
        if ((mask & (1U << edge.clock)) != 0) {
            st = odin3_sim_clock_edges(dut->sim, group, used);
            used = 0;
            mask = 0;
        }
        const tb_conn *conn = &dut->inst->conns[dut->clocks[edge.clock].conn];
        group[used++] = (odin3_sim_clock_event){{conn->sim_port, 0}, event};
        mask |= 1U << edge.clock;
    }
    if (st == ODIN3_OK && used > 0) {
        st = odin3_sim_clock_edges(dut->sim, group, used);
    }
    return st;
}

bool tb_dut_edges(tb_ev *ev, tb_dut *dut) {
    if (dut->nedges == 0) {
        return false;
    }
    dut_status(ev, replay_edges(dut));
    dut->nedges = 0;
    (void)publish(ev, dut);
    return true;
}
