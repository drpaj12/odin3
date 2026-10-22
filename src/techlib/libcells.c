/*
 * libcells.c — the gate, state and skipped cells of one .o3lib library (gatemap plan Task 3).
 * The rules are in libcells.h.
 */
#include "techlib/libcells.h"

#include "ir/celltype.h"
#include "sim/gate_truth.h"
#include "techlib/expr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    STATE_PORTS = 3,   /* a state cell's clock, data and output */
    TABLE_BUF = 0x2,   /* the 1-input table of the identity */
    TABLE_INV = 0x1,   /* ... and of the complement */
    INIT_X = -1,       /* odin3_libstate.init of an x init */
    SKIP_PORT_NONE = 0 /* odin3_libskip.port of a reason without a port */
};

/* The smallest-area choice of one role (buf, inv, tie0, tie1) so far. */
typedef struct pick {
    odin3_celltype_id type;
    uint32_t area;
} pick;

typedef struct collect_ctx {
    const odin3_design *design;
    odin3_libcells *out;
    pick buf, inv, tie0, tie1;
} collect_ctx;

/* The port is one scalar bit of constant width, not inout. */
static bool port_is_bit(const odin3_port_def *port) {
    return port->width == 1 && port->width_fn == NULL && port->width_expr == NULL &&
           port->width_param == NULL && port->dir != ODIN3_DIR_INOUT;
}

bool odin3_libcells_area(const odin3_design *design, uint32_t area_str, double *out) {
    if (area_str == 0) {
        return false;
    }
    const char *text = odin3_strtab_get(odin3_design_strtab(design), area_str);
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/* Offers type (area area) for slot: it wins on a smaller numeric area, or when slot is empty or
 * has no numeric area and it has one. Ties keep the earlier cell. */
static void offer(const odin3_design *design, pick *slot, odin3_celltype_id type, uint32_t area) {
    if (slot->type.v == 0) {
        *slot = (pick){type, area};
        return;
    }
    double have = 0.0;
    double cand = 0.0;
    if (!odin3_libcells_area(design, area, &cand)) {
        return;
    }
    if (!odin3_libcells_area(design, slot->area, &have) || cand < have) {
        *slot = (pick){type, area};
    }
}

static bool is_alpha_(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

/* name matches [A-Za-z_][A-Za-z0-9_]* and is not CONST0 or CONST1. */
static bool is_genlib_name(const char *name) {
    if (!is_alpha_(name[0]) || strcmp(name, "CONST0") == 0 || strcmp(name, "CONST1") == 0) {
        return false;
    }
    for (const char *ch = name + 1; *ch != '\0'; ch++) {
        if (!is_alpha_(*ch) && !(*ch >= '0' && *ch <= '9')) {
            return false;
        }
    }
    return true;
}

/* The first input j of gate its table does not depend on, or n_inputs when it depends on all. */
static uint32_t first_ignored_input(const odin3_libgate *gate) {
    for (uint32_t j = 0; j < gate->n_inputs; j++) {
        bool depends = false;
        for (uint32_t row = 0; row < (1U << gate->n_inputs) && !depends; row++) {
            if ((row & (1U << j)) == 0) {
                depends = ((gate->table >> row) & 1U) != ((gate->table >> (row | (1U << j))) & 1U);
            }
        }
        if (!depends) {
            return j;
        }
    }
    return gate->n_inputs;
}

static odin3_status push_skip(odin3_libcells *out, odin3_celltype_id type, uint8_t why,
                              uint32_t port) {
    odin3_libskip *slot = odin3_vec_push(&out->skips);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_libskip){type, why, port};
    return ODIN3_OK;
}

/* A seq cell: a state cell (and a STATE skip) or a SHAPE skip. */
static odin3_status add_seq_cell(collect_ctx *ctx, odin3_celltype_id type) {
    odin3_libstate state;
    if (!odin3_libstate_of(ctx->design, type, &state)) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_SHAPE, SKIP_PORT_NONE);
    }
    odin3_libstate *slot = odin3_vec_push(&ctx->out->states);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = state;
    return push_skip(ctx->out, type, ODIN3_LIBCELL_STATE, SKIP_PORT_NONE);
}

/* The gate of def with table: its input ports in port order and its output. */
static odin3_libgate gate_of(const odin3_celltype_def *def, const odin3_techlib_cell *lib,
                             odin3_celltype_id type, uint64_t table) {
    odin3_libgate gate = {type, 0, table, {0}, 0, lib->area, lib->delay};
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (def->ports[port].dir == ODIN3_DIR_IN) {
            gate.in_port[gate.n_inputs++] = port;
        } else {
            gate.out_port = port;
        }
    }
    return gate;
}

/* The first port of def that is not a genlib name, or n_ports. */
static uint32_t first_bad_name(const odin3_celltype_def *def) {
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (!is_genlib_name(def->ports[port].name)) {
            return port;
        }
    }
    return def->n_ports;
}

/* A gate cell after the CONSTANT test: a gate, or a SHAPE, NO_SUPPORT or NAME skip. */
static odin3_status add_gate_cell(collect_ctx *ctx, odin3_celltype_id type,
                                  const odin3_celltype_def *def, const odin3_techlib_cell *lib) {
    uint64_t table = 0;
    odin3_status st = lib->n_fns == 1 && lib->memory == NULL ? odin3_gate_truth(def, lib, &table)
                                                             : ODIN3_ERR_INVALID_ARG;
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    if (st != ODIN3_OK) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_SHAPE, SKIP_PORT_NONE);
    }
    odin3_libgate gate = gate_of(def, lib, type, table);
    uint32_t ignored = first_ignored_input(&gate);
    if (ignored < gate.n_inputs) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_NO_SUPPORT, gate.in_port[ignored]);
    }
    if (!is_genlib_name(def->name)) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_CELL_NAME, SKIP_PORT_NONE);
    }
    uint32_t bad = first_bad_name(def);
    if (bad < def->n_ports) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_NAME, bad);
    }
    odin3_libgate *slot = odin3_vec_push(&ctx->out->gates);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = gate;
    if (gate.n_inputs == 1 && table == TABLE_BUF) {
        offer(ctx->design, &ctx->buf, type, lib->area);
    } else if (gate.n_inputs == 1 && table == TABLE_INV) {
        offer(ctx->design, &ctx->inv, type, lib->area);
    }
    return ODIN3_OK;
}

/* One cell of the library, by the rules of libcells.h. */
static odin3_status add_cell(collect_ctx *ctx, odin3_celltype_id type,
                             const odin3_celltype_def *def, const odin3_techlib_cell *lib) {
    if (lib->kind != ODIN3_TECHLIB_GATE) {
        return push_skip(ctx->out, type, ODIN3_LIBCELL_NOT_GATE, SKIP_PORT_NONE);
    }
    if (lib->n_seqs > 0) {
        return add_seq_cell(ctx, type);
    }
    int value = 0;
    odin3_status st = odin3_gate_const_value(def, lib, &value);
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    if (st == ODIN3_OK) {
        offer(ctx->design, value == 0 ? &ctx->tie0 : &ctx->tie1, type, lib->area);
        return push_skip(ctx->out, type, ODIN3_LIBCELL_CONSTANT, SKIP_PORT_NONE);
    }
    return add_gate_cell(ctx, type, def, lib);
}

static void libcells_init(odin3_libcells *cells) {
    *cells = (odin3_libcells){0};
    odin3_vec_init(&cells->gates, sizeof(odin3_libgate));
    odin3_vec_init(&cells->states, sizeof(odin3_libstate));
    odin3_vec_init(&cells->skips, sizeof(odin3_libskip));
}

odin3_status odin3_libcells_collect(const odin3_design *design, const char *library,
                                    odin3_libcells *out) {
    libcells_init(out);
    uint32_t name = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(library), &name)) {
        return ODIN3_OK;
    }
    collect_ctx ctx = {design, out, {{0}, 0}, {{0}, 0}, {{0}, 0}, {{0}, 0}};
    for (uint32_t id = 1;; id++) {
        odin3_celltype_id type = {id};
        const odin3_celltype_def *def = odin3_celltype_get(design, type);
        if (def == NULL) {
            break;
        }
        const odin3_techlib_cell *lib = odin3_techlib_cell_get(design, type);
        if (lib == NULL || lib->library != name) {
            continue;
        }
        out->n_cells++;
        odin3_status st = add_cell(&ctx, type, def, lib);
        if (st != ODIN3_OK) {
            odin3_libcells_free(out);
            return st;
        }
    }
    out->library = out->n_cells > 0 ? name : 0;
    out->buf = ctx.buf.type;
    out->inv = ctx.inv.type;
    out->tie0 = ctx.tie0.type;
    out->tie1 = ctx.tie1.type;
    return ODIN3_OK;
}

void odin3_libcells_free(odin3_libcells *cells) {
    odin3_vec_free(&cells->gates);
    odin3_vec_free(&cells->states);
    odin3_vec_free(&cells->skips);
    cells->library = 0;
    cells->n_cells = 0;
    cells->buf = cells->inv = cells->tie0 = cells->tie1 = (odin3_celltype_id){0};
}

/* The init of seq: 0 or 1, INIT_X for x; false for any other value. */
static bool init_of(const odin3_techlib_seq *seq, int *init) {
    if (seq->init == NULL) {
        *init = INIT_X;
        return true;
    }
    int64_t value = 0;
    odin3_expr_error err;
    if (odin3_expr_eval_int_quiet(seq->init, NULL, &value, &err) != ODIN3_OK ||
        (value != 0 && value != 1)) {
        return false;
    }
    *init = (int)value;
    return true;
}

/* The data input of a three-port seq cell: the input other than the clock, named by the seq's
 * data identifier; def->n_ports when there is none. */
static uint32_t data_port_of(const odin3_design *design, const odin3_celltype_def *def,
                             const odin3_techlib_seq *seq) {
    if (seq->data->kind != ODIN3_EXPR_IDENT) {
        return def->n_ports;
    }
    const char *name = odin3_strtab_get(odin3_design_strtab(design), seq->data->ident);
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (port != seq->clock && def->ports[port].dir == ODIN3_DIR_IN && name != NULL &&
            strcmp(def->ports[port].name, name) == 0) {
            return port;
        }
    }
    return def->n_ports;
}

/* def has exactly three 1-bit ports: two inputs (one the seq's clock) and the seq's output. */
static bool state_shape(const odin3_celltype_def *def, const odin3_techlib_seq *seq) {
    if (def->n_params != 0 || def->n_ports != STATE_PORTS || seq->clock >= STATE_PORTS ||
        seq->port >= STATE_PORTS || def->ports[seq->clock].dir != ODIN3_DIR_IN ||
        def->ports[seq->port].dir != ODIN3_DIR_OUT) {
        return false;
    }
    uint32_t inputs = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (!port_is_bit(&def->ports[port])) {
            return false;
        }
        inputs += def->ports[port].dir == ODIN3_DIR_IN ? 1U : 0U;
    }
    return inputs == STATE_PORTS - 1;
}

bool odin3_libstate_of(const odin3_design *design, odin3_celltype_id type, odin3_libstate *out) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    const odin3_techlib_cell *lib = odin3_techlib_cell_get(design, type);
    if (def == NULL || lib == NULL || lib->kind != ODIN3_TECHLIB_GATE || lib->n_seqs != 1 ||
        lib->n_fns != 0 || lib->memory != NULL || !state_shape(def, &lib->seqs[0])) {
        return false;
    }
    const odin3_techlib_seq *seq = &lib->seqs[0];
    uint32_t data = data_port_of(design, def, seq);
    int init = INIT_X;
    if (data == def->n_ports || !init_of(seq, &init)) {
        return false;
    }
    *out = (odin3_libstate){type, seq->trigger, init, seq->clock, data, seq->port, lib->area};
    return true;
}

const char *odin3_libcell_skip_text(uint8_t why) {
    switch (why) {
    case ODIN3_LIBCELL_NOT_GATE:
        return "not a gate cell";
    case ODIN3_LIBCELL_STATE:
        return "a flip-flop or latch";
    case ODIN3_LIBCELL_CONSTANT:
        return "a constant cell (used for constants)";
    case ODIN3_LIBCELL_SHAPE:
        return "not one output and 1 to 6 one-bit inputs without parameters";
    case ODIN3_LIBCELL_NO_SUPPORT:
        return "its function ignores input '%s'";
    case ODIN3_LIBCELL_NAME:
        return "port '%s' is not a genlib name";
    case ODIN3_LIBCELL_CELL_NAME:
        return "its name is not a genlib name";
    default:
        return "";
    }
}
