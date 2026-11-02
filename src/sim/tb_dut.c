/* tb_dut.c — binds a testbench instance to a module of the design simulated by odin3_sim (spec
 * sections 1 and 4). */
#include "ir/module.h"
#include "sim/sim.h"
#include "sim/tb_internal.h"
#include "util/log.h"
#include "util/str.h"

#include <stdlib.h>
#include <string.h>

enum {
    EDGE_CAP = 64,  /* clock edges recorded between two NBA regions */
    MAX_CLOCKS = 16 /* clock inputs of one DUT instance */
};

/* One clock input of the DUT: the connection that drives it, and its value at the last
 * observation (starts at 0). */
typedef struct dut_clock {
    int conn; /* index in inst->conns */
    uint64_t last;
} dut_clock;

/* One recorded edge: which clock (index in clocks), and whether it rose. */
typedef struct dut_edge {
    uint8_t clock;
    bool rise;
} dut_edge;

struct tb_dut {
    odin3_sim *sim;
    tb_inst *inst;
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

static bool bind_conn(tb_dut *dut, tb_conn *conn, uint32_t line) {
    const char *mod = dut->inst->module;
    if (conn->port[0] == '\0') {
        tb_error("testbench", line, "ordered port connections (use .port(expr))");
        return false;
    }
    if (!port_lookup(dut->sim, conn->port, conn)) {
        odin3_log(ODIN3_LOG_ERROR, "testbench:%u: module `%s` has no input or output port `%s`",
                  line, mod, conn->port);
        return false;
    }
    if (conn->width > TB_MAX_WIDTH) {
        tb_error("testbench", line, "DUT port wider than 64 bits");
        return false;
    }
    if (conn->is_out && conn->expr != NULL && conn->expr->kind != E_VAR) {
        tb_error("testbench", line, "output port connected to anything but a whole wire or reg");
        return false;
    }
    odin3_sim_bit bit = {conn->sim_port, 0};
    if (!conn->is_out && odin3_sim_input_is_clock(dut->sim, bit)) {
        if (dut->nclocks == MAX_CLOCKS) {
            odin3_log(
                ODIN3_LOG_ERROR,
                "testbench:%u: module `%s` has more than %d clock inputs; `%s` is one too many",
                line, mod, MAX_CLOCKS, conn->port);
            return false;
        }
        dut->clocks[dut->nclocks++] = (dut_clock){(int)(conn - dut->inst->conns), 0};
    }
    return true;
}

odin3_status tb_dut_open(odin3_design *design, tb_module *tb, tb_inst *inst, tb_dut **out) {
    (void)tb;
    *out = NULL;
    tb_dut *dut = calloc(1, sizeof *dut);
    if (dut == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    dut->inst = inst;
    odin3_status st = tb_inst_sim(design, inst);
    dut->sim = inst->sim;
    if (st == ODIN3_OK) {
        odin3_sim_reset(dut->sim); /* built at parse time, or kept from an earlier run */
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < inst->nconns; i++) {
        st = bind_conn(dut, &inst->conns[i], inst->line) ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
    }
    if (st != ODIN3_OK) {
        tb_dut_close(dut);
        return st;
    }
    *out = dut;
    return ODIN3_OK;
}

void tb_dut_close(tb_dut *dut) {
    free(dut);
}

odin3_status tb_inst_sim(odin3_design *design, tb_inst *inst) {
    if (inst->sim != NULL) {
        return ODIN3_OK;
    }
    odin3_module_id id = {0};
    if (design == NULL || !find_module(design, inst->module, &id)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "testbench:%u: no module `%s` in the design (read the DUT first)", inst->line,
                  inst->module);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_sim_build(design, id, &inst->sim);
}

odin3_status tb_inst_signal(odin3_design *design, tb_inst *inst, const char *path,
                            tb_hier_sig *out) {
    odin3_status st = tb_inst_sim(design, inst);
    if (st == ODIN3_OK && inst->sigs == NULL) {
        st = odin3_sim_signals_create(inst->sim, &inst->sigs);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t id = 0;
    st = odin3_sim_signals_find(inst->sigs, odin3_bytes_cstr(path), &id);
    const odin3_sim_signal *sig =
        st == ODIN3_OK && id != 0 ? odin3_sim_signals_at(inst->sigs, id) : NULL;
    if (sig == NULL) {
        if (st == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "testbench:%u: DUT `%s` (module `%s`) has no signal `%s`",
                      inst->line, inst->name, inst->module, path);
        }
        return st == ODIN3_OK ? ODIN3_ERR_INVALID_ARG : st;
    }
    if (sig->width > TB_MAX_WIDTH) {
        odin3_log(ODIN3_LOG_ERROR, "testbench:%u: signal `%s` of DUT `%s` is wider than 64 bits",
                  inst->line, path, inst->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (tb_hier_sig){id, sig->width};
    return ODIN3_OK;
}

uint64_t tb_inst_peek(const tb_inst *inst, uint32_t id) {
    const odin3_sim_signal *sig = odin3_sim_signals_at(inst->sigs, id);
    uint8_t bits[TB_MAX_WIDTH] = {0};
    uint64_t val = 0;
    if (sig == NULL || odin3_sim_signals_read(inst->sigs, id, bits, sig->width) != ODIN3_OK) {
        return 0;
    }
    for (uint32_t k = 0; k < sig->width; k++) {
        val |= (uint64_t)(bits[k] & 1U) << k;
    }
    return val;
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
        tb_conn *conn = &dut->inst->conns[i];
        if (conn->is_out || is_clock_conn(dut, i) || conn->expr == NULL) {
            continue;
        }
        uint64_t val = tb_eval_assign(ev, conn->expr, conn->width);
        if (conn->seen && val == conn->last) {
            continue;
        }
        for (uint32_t bit_no = 0; bit_no < conn->width; bit_no++) {
            odin3_sim_bit bit = {conn->sim_port, bit_no};
            (void)odin3_sim_set_input(dut->sim, bit, ((val >> bit_no) & 1U) != 0);
        }
        conn->last = val;
        conn->seen = true;
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
    uint64_t val = tb_eval(ev, clk, clk->width, false) & 1U;
    if (val == clock->last) {
        return;
    }
    clock->last = val;
    if (dut->nedges == EDGE_CAP) {
        ev->failed = true;
        ev->why = "more than 64 clock edges between two NBA regions";
        return;
    }
    dut->edges[dut->nedges++] = (dut_edge){(uint8_t)which, val != 0};
}

void tb_dut_observe(tb_ev *ev, tb_dut *dut) {
    for (uint32_t k = 0; k < dut->nclocks; k++) {
        observe_clock(ev, dut, k);
    }
}

static bool publish(tb_ev *ev, tb_dut *dut) {
    bool changed = false;
    for (uint32_t i = 0; i < dut->inst->nconns; i++) {
        tb_conn *conn = &dut->inst->conns[i];
        if (!conn->is_out || conn->expr == NULL) {
            continue;
        }
        uint64_t val = 0;
        for (uint32_t bit_no = 0; bit_no < conn->width; bit_no++) {
            bool bit = false;
            odin3_sim_bit at = {conn->sim_port, bit_no};
            (void)odin3_sim_get_output(dut->sim, at, &bit);
            val |= bit ? (uint64_t)1 << bit_no : 0;
        }
        if (!conn->seen || val != conn->last) {
            tb_write(ev, conn->expr, val);
            conn->last = val;
            conn->seen = true;
            changed = true;
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
    if (apply_inputs(ev, dut)) {
        dut_status(ev, odin3_sim_settle_events(dut->sim));
    }
    return publish(ev, dut);
}

/* One replayed edge. A DUT with one clock input connected sends the edge to every clock of the
 * design, as the cycle simulator does; with several, each edge goes to its own clock only. */
static odin3_status replay_edge(tb_dut *dut, dut_edge edge) {
    odin3_sim_event event = edge.rise ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE;
    if (dut->nclocks == 1) {
        return odin3_sim_clock_edge(dut->sim, event);
    }
    const tb_conn *conn = &dut->inst->conns[dut->clocks[edge.clock].conn];
    return odin3_sim_clock_edge_input(dut->sim, (odin3_sim_bit){conn->sim_port, 0}, event);
}

bool tb_dut_edges(tb_ev *ev, tb_dut *dut) {
    if (dut->nedges == 0) {
        return false;
    }
    for (uint32_t i = 0; i < dut->nedges; i++) {
        dut_status(ev, replay_edge(dut, dut->edges[i]));
    }
    dut->nedges = 0;
    (void)publish(ev, dut);
    return true;
}
