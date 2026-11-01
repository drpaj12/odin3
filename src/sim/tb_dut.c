/* tb_dut.c — binds a testbench instance to a module of the design simulated by odin3_sim (spec
 * sections 1 and 4). */
#include "ir/module.h"
#include "sim/sim.h"
#include "sim/tb_internal.h"
#include "util/log.h"
#include "util/str.h"

#include <stdlib.h>
#include <string.h>

enum { EDGE_CAP = 64 /* clock edges recorded between two NBA regions */ };

struct tb_dut {
    odin3_sim *sim;
    tb_inst *inst;
    int clock_conn;       /* index in inst->conns, -1 none */
    uint64_t clock_last;  /* clock value at the last observation (starts at 0) */
    bool edges[EDGE_CAP]; /* recorded edges in order: true posedge, false negedge */
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
        if (dut->clock_conn >= 0) {
            tb_error("testbench", line, "DUT with more than one clock input");
            return false;
        }
        dut->clock_conn = (int)(conn - dut->inst->conns);
    }
    return true;
}

odin3_status tb_dut_open(odin3_design *design, tb_module *tb, tb_inst *inst, tb_dut **out) {
    (void)tb;
    *out = NULL;
    odin3_module_id id = {0};
    if (!find_module(design, inst->module, &id)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "testbench:%u: no module `%s` in the design (read the DUT first)", inst->line,
                  inst->module);
        return ODIN3_ERR_INVALID_ARG;
    }
    tb_dut *dut = calloc(1, sizeof *dut);
    if (dut == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    dut->inst = inst;
    dut->clock_conn = -1;
    odin3_status st = odin3_sim_build(design, id, &dut->sim);
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
    if (dut != NULL) {
        odin3_sim_destroy(dut->sim);
        free(dut);
    }
}

static bool apply_inputs(tb_ev *ev, tb_dut *dut) {
    bool changed = false;
    for (uint32_t i = 0; i < dut->inst->nconns; i++) {
        tb_conn *conn = &dut->inst->conns[i];
        if (conn->is_out || (int)i == dut->clock_conn || conn->expr == NULL) {
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

void tb_dut_observe(tb_ev *ev, tb_dut *dut) {
    if (dut->clock_conn < 0 || dut->inst->conns[dut->clock_conn].expr == NULL) {
        return;
    }
    const tb_expr *clk = dut->inst->conns[dut->clock_conn].expr;
    uint64_t val = tb_eval(ev, clk, clk->width, false) & 1U;
    if (val == dut->clock_last) {
        return;
    }
    dut->clock_last = val;
    if (dut->nedges == EDGE_CAP) {
        ev->failed = true;
        ev->why = "more than 64 clock edges between two NBA regions";
        return;
    }
    dut->edges[dut->nedges++] = val != 0;
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

bool tb_dut_comb(tb_ev *ev, tb_dut *dut) {
    if (apply_inputs(ev, dut)) {
        (void)odin3_sim_settle_events(dut->sim);
    }
    return publish(ev, dut);
}

bool tb_dut_edges(tb_ev *ev, tb_dut *dut) {
    if (dut->nedges == 0) {
        return false;
    }
    for (uint32_t i = 0; i < dut->nedges; i++) {
        (void)odin3_sim_clock_edge(dut->sim, dut->edges[i] ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE);
    }
    dut->nedges = 0;
    (void)publish(ev, dut);
    return true;
}
