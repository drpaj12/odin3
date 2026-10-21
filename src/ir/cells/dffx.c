/*
 * dffx.c — gate-level flip-flops with an enable, an asynchronous reset or a synchronous reset: the
 * 68 types of dffx.h, Yosys 0.55 simcells.v spelling and meaning (5.3 follow-up, append-only IR-8).
 *
 * Ports in this order: C, [R], [E], D, Q (C first: ODIN3_CT_CLOCK_PIN0). R is the reset (or set)
 * pin, E the enable. One INIT parameter, as `$_DFF_P_`. Name letters (dffx.h): clock polarity,
 * reset polarity, reset value, enable polarity; P is active high.
 *   $_DFFE_cE_       at the clock edge, if E active: Q <= D
 *   $_DFF_crv_       R active: Q = v at once (asynchronous); else at the edge Q <= D
 *   $_DFFE_crve_     as $_DFF_crv_, the edge load only if E active
 *   $_SDFF_crv_      at the edge: R active -> v, else D
 *   $_SDFFE_crve_    at the edge: R active -> v, else if E active, D (reset over enable)
 *   $_SDFFCE_crve_   at the edge, if E active: R active -> v, else D (enable over reset)
 *
 * Simulation (sim/cell.h). State: the stored Q. An asynchronous reset is applied whenever the cell
 * is evaluated with R active: at each settle (COMB, which sees a primary input at once) and at
 * both clock edges (which see R as settled), and it beats the clock edge.
 */
#include "dffx.h"
#include "cells.h"

static const odin3_port_def k_dffx_e_ports[] = {
    ODIN3_PORT_BIT("C", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("E", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};
static const odin3_port_def k_dffx_a_ports[] = {
    ODIN3_PORT_BIT("C", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("R", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};
static const odin3_port_def k_dffx_ae_ports[] = {
    ODIN3_PORT_BIT("C", ODIN3_DIR_IN),  ODIN3_PORT_BIT("R", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("E", ODIN3_DIR_IN),  ODIN3_PORT_BIT("D", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("Q", ODIN3_DIR_OUT),
};

/* What a type does; see the table above. */
typedef enum dffx_kind {
    DX_KIND_E,   /* enable only */
    DX_KIND_A,   /* async reset */
    DX_KIND_AE,  /* async reset, enable */
    DX_KIND_S,   /* sync reset */
    DX_KIND_SE,  /* sync reset over enable */
    DX_KIND_SCE, /* enable over sync reset */
} dffx_kind;

typedef struct dffx_spec {
    dffx_kind kind;
    bool clk_pos;
    bool rst_pos;
    bool rst_val;
    bool en_pos;
} dffx_spec;

enum { SEQ_INIT, PORT_C = 0, PORT_R = 1, D_FROM_END = 2 };

#define DX_POL_P true
#define DX_POL_N false
#define DX_VAL_0 false
#define DX_VAL_1 true

static bool has_reset(const dffx_spec *spec) {
    return spec->kind != DX_KIND_E;
}
static bool is_async(const dffx_spec *spec) {
    return spec->kind == DX_KIND_A || spec->kind == DX_KIND_AE;
}
static bool has_enable(const dffx_spec *spec) {
    return spec->kind != DX_KIND_A && spec->kind != DX_KIND_S;
}

/* True when port is at its active level. */
static bool active(const odin3_sim_cell *cell, uint32_t port, bool positive) {
    return odin3_cells_sim_in(cell, port) == positive;
}

static bool enable_on(const odin3_sim_cell *cell, const dffx_spec *spec) {
    uint32_t port = PORT_R + (has_reset(spec) ? 1U : 0U);
    return !has_enable(spec) || active(cell, port, spec->en_pos);
}

static bool reset_on(const odin3_sim_cell *cell, const dffx_spec *spec) {
    return has_reset(spec) && active(cell, PORT_R, spec->rst_pos);
}

/* What Q takes at the active clock edge. */
static uint8_t next_state(const odin3_sim_cell *cell, const dffx_spec *spec) {
    bool data = odin3_cells_sim_in(cell, cell->n_ports - D_FROM_END);
    bool held = cell->state[0] != 0;
    bool reset = reset_on(cell, spec);
    bool next = held;
    if (spec->kind == DX_KIND_SCE) {
        next = !enable_on(cell, spec) ? held : (reset ? spec->rst_val : data);
    } else if (reset) {
        next = spec->rst_val;
    } else if (enable_on(cell, spec)) {
        next = data;
    }
    return (uint8_t)(next ? 1 : 0);
}

static void dffx_sim(const odin3_sim_cell *cell, const dffx_spec *spec) {
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = odin3_sim_init_bit(&cell->params[SEQ_INIT]);
        return;
    }
    if (is_async(spec) && reset_on(cell, spec)) {
        cell->state[0] = (uint8_t)(spec->rst_val ? 1 : 0);
    }
    odin3_sim_event active_edge = spec->clk_pos ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE;
    if (cell->event == active_edge) {
        cell->state[0] = next_state(cell, spec);
    } else if (cell->event == ODIN3_SIM_COMB) {
        odin3_cells_sim_out(cell, cell->state[0] != 0);
    }
}

#define DX_FLAGS (ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0)

/* One type: its spec, its hook and its definition, named `var`. */
#define DX_DEFINE(var, label, ports, ...)                                                          \
    static const dffx_spec spec_##var = {__VA_ARGS__};                                             \
    static void sim_##var(const odin3_sim_cell *cell) {                                            \
        dffx_sim(cell, &spec_##var);                                                               \
    }                                                                                              \
    const odin3_celltype_def odin3_cell_dffx_##var = {label,                                       \
                                                      ODIN3_GRAN_BIT,                              \
                                                      DX_FLAGS,                                    \
                                                      ports,                                       \
                                                      ODIN3_NELEM(ports),                          \
                                                      odin3_cells_init_params,                     \
                                                      1,                                           \
                                                      odin3_cells_init_verify,                     \
                                                      NULL,                                        \
                                                      sim_##var,                                   \
                                                      NULL}

#define DX_E(c, e)                                                                                 \
    DX_DEFINE(e_##c##e, "$_DFFE_" #c #e "_", k_dffx_e_ports, DX_KIND_E, DX_POL_##c, false, false,  \
              DX_POL_##e);
#define DX_A(c, r, v)                                                                              \
    DX_DEFINE(a_##c##r##v, "$_DFF_" #c #r #v "_", k_dffx_a_ports, DX_KIND_A, DX_POL_##c,           \
              DX_POL_##r, DX_VAL_##v, false);
#define DX_AE(c, r, v, e)                                                                          \
    DX_DEFINE(ae_##c##r##v##e, "$_DFFE_" #c #r #v #e "_", k_dffx_ae_ports, DX_KIND_AE, DX_POL_##c, \
              DX_POL_##r, DX_VAL_##v, DX_POL_##e);
#define DX_S(c, r, v)                                                                              \
    DX_DEFINE(s_##c##r##v, "$_SDFF_" #c #r #v "_", k_dffx_a_ports, DX_KIND_S, DX_POL_##c,          \
              DX_POL_##r, DX_VAL_##v, false);
#define DX_SE(c, r, v, e)                                                                          \
    DX_DEFINE(se_##c##r##v##e, "$_SDFFE_" #c #r #v #e "_", k_dffx_ae_ports, DX_KIND_SE,            \
              DX_POL_##c, DX_POL_##r, DX_VAL_##v, DX_POL_##e);
#define DX_SCE(c, r, v, e)                                                                         \
    DX_DEFINE(sce_##c##r##v##e, "$_SDFFCE_" #c #r #v #e "_", k_dffx_ae_ports, DX_KIND_SCE,         \
              DX_POL_##c, DX_POL_##r, DX_VAL_##v, DX_POL_##e);

ODIN3_DFFX_LIST(DX_A, DX_E, DX_AE, DX_S, DX_SE, DX_SCE)
