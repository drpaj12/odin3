/*
 * sc_model.c — builds the scoap model of one module (spec section 3): every live cell becomes a
 * function cell (gates, $lut, $sop), a storage cell, or an opaque cell; the module ports, opaque
 * cells and constants fix the boundary values.
 */
#include "ir/celltype.h"
#include "ir/lut.h"
#include "ir/value.h"
#include "passes/scoap/sc_int.h"
#include "sim/gate_truth.h"
#include "util/alloc.h"

#include <string.h>

enum { SOP_PARAM_COVER = 1, MAX_FUNC_WORDS = 4, BIT_WORD = 64, LUT_PARAMS = 2, FIRST_CELLS = 64 };

typedef struct sc_pins {
    uint32_t in[SC_MAX_TABLE]; /* input nets in pin order */
    uint32_t n_in;
    uint32_t out;
    uint32_t n_out;
} sc_pins;

/* A constant driver: at least one pin, none reading, every pin on a constant net. */
static bool node_is_constant(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    if (pins.count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(mod, pin);
        if (odin3_pin_reads(mod, pin) || !odin3_net_valid(net) ||
            odin3_net_const_value(mod, net) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

static void lower(int64_t *slot, int64_t value) {
    *slot = value < *slot ? value : *slot;
}

/* The net acts as a primary input: controllable at cost 1, no frames. */
static void make_source(sc_model *md, uint32_t net) {
    if (net == 0 || md->fixed[net] != 0) {
        return;
    }
    for (int val = 0; val < 2; val++) {
        lower(&md->cc[val][net], 1);
        lower(&md->sc[val][net], 0);
    }
}

/* The net acts as a primary output: observable at cost 0. */
static void make_sink(sc_model *md, uint32_t net) {
    if (net != 0) {
        lower(&md->co[net], 0);
        lower(&md->so[net], 0);
    }
}

static void boundary_pins(sc_model *md, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(md->mod, pin);
        if (!odin3_net_valid(net)) {
            continue;
        }
        if (odin3_pin_drives(md->mod, pin)) {
            make_source(md, net.v);
        }
        if (odin3_pin_reads(md->mod, pin)) {
            make_sink(md, net.v);
        }
    }
}

static odin3_status alloc_nets(sc_model *md) {
    size_t cnt = (size_t)md->n_nets + 1;
    int64_t **arrays[] = {&md->cc[0], &md->cc[1], &md->sc[0], &md->sc[1], &md->co, &md->so};
    for (size_t k = 0; k < sizeof arrays / sizeof arrays[0]; k++) {
        *arrays[k] = odin3_util_calloc(cnt * sizeof(int64_t));
        if (*arrays[k] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (size_t i = 0; i < cnt; i++) {
            (*arrays[k])[i] = SCOAP_INF;
        }
    }
    md->live = odin3_util_calloc(cnt);
    md->fixed = odin3_util_calloc(cnt);
    return md->live != NULL && md->fixed != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Constant nets keep their value: the held value costs 1, the other is unreachable. */
static void init_net(sc_model *md, uint32_t id) {
    odin3_net_id net = {id};
    if (!odin3_net_live(md->mod, net)) {
        return;
    }
    md->live[id] = 1;
    odin3_const cv = odin3_net_const_value(md->mod, net);
    if (cv == ODIN3_CONST_NONE) {
        return;
    }
    md->fixed[id] = 1;
    for (int val = 0; val < 2; val++) {
        bool held = cv == ODIN3_CONST_X || cv == ODIN3_CONST_Z || (int)(cv == ODIN3_CONST_1) == val;
        md->cc[val][id] = held ? 1 : SCOAP_INF;
        md->sc[val][id] = held ? 0 : SCOAP_INF;
    }
}

/* --- cells ------------------------------------------------------------------------------------ */

static bool type_is(const odin3_celltype_def *def, const char *name) {
    return strcmp(def->name, name) == 0;
}

static void gather_pins(const sc_model *md, odin3_node_id node, sc_pins *pp) {
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    memset(pp, 0, sizeof *pp);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(md->mod, pin);
        if (odin3_pin_drives(md->mod, pin)) {
            pp->out = net.v;
            pp->n_out++;
        } else if (pp->n_in < SC_MAX_TABLE) {
            pp->in[pp->n_in++] = net.v;
        } else {
            pp->n_in = SC_MAX_TABLE + 1;
            break;
        }
    }
}

/* $sop semantics of src/ir/cells/sop.c: ON rows give 1, OFF rows give 0 on a hit, none: 0. */
static uint8_t sop_eval(const odin3_value *cover, uint64_t mint) {
    uint32_t width = cover->cover_inputs;
    uint64_t row_len = (uint64_t)width + 1;
    bool has_on = false;
    bool off_hit = false;
    for (uint64_t off = 0; off + row_len <= cover->len; off += row_len) {
        const uint8_t *row = cover->bits + off;
        bool hit = true;
        for (uint32_t j = 0; j < width; j++) {
            hit = hit && (row[j] == '-' || (row[j] == '1') == ((mint >> j & 1U) != 0));
        }
        if (row[width] == '1') {
            if (hit) {
                return 1;
            }
            has_on = true;
        } else {
            off_hit = off_hit || hit;
        }
    }
    return (!has_on && cover->len > 0 && !off_hit) ? 1 : 0;
}

static bool table_sop(const sc_model *md, odin3_node_id node, uint32_t width, sc_table *tab) {
    const odin3_value *cover = odin3_node_param(md->mod, node, SOP_PARAM_COVER);
    if (cover == NULL || cover->cover_inputs != width) {
        return false;
    }
    tab->n = width;
    for (uint32_t mint = 0; mint < (1U << width); mint++) {
        tab->f[mint] = sop_eval(cover, mint);
    }
    return true;
}

static bool table_lut(const sc_model *md, odin3_node_id node, uint32_t width, sc_table *tab) {
    odin3_value params[LUT_PARAMS];
    uint64_t words[MAX_FUNC_WORDS] = {0};
    for (uint32_t k = 0; k < LUT_PARAMS; k++) {
        const odin3_value *val = odin3_node_param(md->mod, node, k);
        if (val == NULL) {
            return false;
        }
        params[k] = *val;
    }
    if (!odin3_lut_table(params, words, MAX_FUNC_WORDS)) {
        return false;
    }
    tab->n = width;
    for (uint32_t mint = 0; mint < (1U << width); mint++) {
        tab->f[mint] = (uint8_t)(words[mint / BIT_WORD] >> (mint % BIT_WORD) & 1U);
    }
    return true;
}

static bool table_gate(const sc_model *md, odin3_celltype_id type, const odin3_celltype_def *def,
                       sc_table *tab) {
    uint64_t bits = 0;
    const odin3_design *design = odin3_module_design(md->mod);
    if (!odin3_gate_truth_eligible(def) ||
        odin3_gate_truth(def, odin3_celltype_lib(design, type), &bits) != ODIN3_OK) {
        return false;
    }
    tab->n = odin3_gate_truth_inputs(def);
    for (uint32_t mint = 0; mint < (1U << tab->n); mint++) {
        tab->f[mint] = (uint8_t)(bits >> mint & 1U);
    }
    return true;
}

static odin3_status push_cell(sc_model *md, const sc_cell *cell) {
    if (md->n_cells == md->cap_cells) {
        size_t cap = md->cap_cells == 0 ? FIRST_CELLS : md->cap_cells * 2;
        sc_cell *grown = odin3_util_realloc(md->cells, cap * sizeof *grown);
        if (grown == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        md->cells = grown;
        md->cap_cells = cap;
    }
    md->cells[md->n_cells++] = *cell;
    md->stats.cells[cell->kind]++;
    return ODIN3_OK;
}

static odin3_status push_inputs(sc_model *md, sc_cell *cell, const uint32_t *nets, uint32_t cnt) {
    cell->in_off = (uint32_t)md->in_pool.len;
    cell->n_in = cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t *slot = odin3_vec_push(&md->in_pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = nets[i];
    }
    return ODIN3_OK;
}

/* A wide $sop keeps its rows; the value they give is the one its cubes control. */
static odin3_status keep_cover(sc_model *md, sc_cell *cell, odin3_node_id node) {
    const odin3_value *cover = odin3_node_param(md->mod, node, SOP_PARAM_COVER);
    cell->cover_val = -2;
    if (cover == NULL || cover->cover_inputs != cell->n_in) {
        return ODIN3_OK;
    }
    cell->cover_val = cover->len == 0 ? -1 : 0;
    uint32_t row_len = cell->n_in + 1;
    for (uint32_t off = 0; off + row_len <= cover->len; off += row_len) {
        if (cover->bits[off + cell->n_in] == '1') {
            cell->cover_val = 1;
        }
    }
    cell->cover_off = (uint32_t)md->cover_pool.len;
    cell->cover_len = cover->len;
    for (uint32_t i = 0; i < cover->len; i++) {
        uint8_t *slot = odin3_vec_push(&md->cover_pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = cover->bits[i];
    }
    return ODIN3_OK;
}

static bool func_table(const sc_model *md, odin3_node_id node, const sc_pins *pp, sc_table *tab) {
    odin3_celltype_id type = odin3_node_type(md->mod, node);
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(md->mod), type);
    if (pp->n_in > SC_MAX_EXACT) {
        return false;
    }
    if (type_is(def, "$sop")) {
        return table_sop(md, node, pp->n_in, tab);
    }
    if (type_is(def, "$lut")) {
        return table_lut(md, node, pp->n_in, tab);
    }
    return table_gate(md, type, def, tab) && tab->n == pp->n_in;
}

/* A one-output combinational cell with a function: exact up to SC_MAX_EXACT inputs. */
static bool is_function_type(const odin3_celltype_def *def) {
    return def->gran == ODIN3_GRAN_BIT &&
           (type_is(def, "$sop") || type_is(def, "$lut") ||
            (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) == 0);
}

/* Pins on the same net are one input: the table is rebuilt over the distinct nets, so a constant
 * function (XOR(a, a)) shows its constant and a shared net's cost counts once. */
static void collapse_duplicates(sc_pins *pp, sc_table *tab) {
    uint32_t map[SC_MAX_EXACT];
    uint32_t uniq[SC_MAX_EXACT];
    uint32_t count = 0;
    for (uint32_t i = 0; i < pp->n_in; i++) {
        uint32_t j = 0;
        while (j < count && uniq[j] != pp->in[i]) {
            j++;
        }
        uniq[j] = pp->in[i];
        map[i] = j;
        count += j == count ? 1U : 0U;
    }
    if (count == pp->n_in) {
        return;
    }
    sc_table out = {.n = count};
    for (uint32_t row = 0; row < (1U << count); row++) {
        uint32_t mint = 0;
        for (uint32_t i = 0; i < pp->n_in; i++) {
            mint |= ((row >> map[i]) & 1U) << i;
        }
        out.f[row] = tab->f[mint];
    }
    *tab = out;
    memcpy(pp->in, uniq, count * sizeof uniq[0]);
    pp->n_in = count;
}

static odin3_status add_function(sc_model *md, odin3_node_id node, const sc_pins *pp, bool *done) {
    sc_cell cell = {.node = node.v, .out = pp->out, .kind = SC_FUNC, .cover_val = -2};
    sc_table tab;
    *done = true;
    odin3_status st = ODIN3_OK;
    if (func_table(md, node, pp, &tab)) {
        sc_pins uniq = *pp;
        collapse_duplicates(&uniq, &tab);
        st = push_inputs(md, &cell, uniq.in, uniq.n_in);
        st = st == ODIN3_OK ? sc_fn_compile(md, &cell, &tab) : st;
    } else {
        const odin3_celltype_def *def =
            odin3_celltype_get(odin3_module_design(md->mod), odin3_node_type(md->mod, node));
        bool wide = pp->n_in > SC_MAX_EXACT && pp->n_in <= SC_MAX_TABLE &&
                    (type_is(def, "$sop") || type_is(def, "$lut"));
        *done = wide;
        cell.kind = SC_WIDE;
        st = wide ? push_inputs(md, &cell, pp->in, pp->n_in) : st;
        st = wide && type_is(def, "$sop") && st == ODIN3_OK ? keep_cover(md, &cell, node) : st;
    }
    return *done && st == ODIN3_OK ? push_cell(md, &cell) : st;
}

/* The storage cell's nets: D, clock (0 none), other inputs; Q is the output. */
static bool storage_nets(const sc_model *md, odin3_node_id node, const odin3_celltype_def *def,
                         sc_pins *pp) {
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    uint32_t d_net = 0;
    uint32_t clk = 0;
    bool have_d = false;
    memset(pp, 0, sizeof *pp);
    pp->n_in = 2; /* D, C first */
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        uint32_t net = odin3_pin_net(md->mod, pin).v;
        uint32_t port = odin3_pin_port(md->mod, pin);
        if (odin3_pin_drives(md->mod, pin)) {
            pp->out = net;
            pp->n_out++;
        } else if (strcmp(def->ports[port].name, "D") == 0) {
            d_net = net;
            have_d = true;
        } else if ((def->flags & ODIN3_CT_CLOCK_PIN0) != 0 && port == 0) {
            clk = net;
        } else if (pp->n_in < SC_MAX_TABLE) {
            pp->in[pp->n_in++] = net;
        }
    }
    pp->in[0] = d_net;
    pp->in[1] = clk;
    return have_d && pp->n_out == 1;
}

static odin3_status add_other(sc_model *md, odin3_node_id node, const odin3_celltype_def *def,
                              odin3_celltype_id type) {
    bool storage = (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0;
    sc_cell cell = {.node = node.v, .kind = SC_OPAQUE, .cover_val = -2};
    sc_pins pp;
    odin3_status st = ODIN3_OK;
    if (storage && md->seq && storage_nets(md, node, def, &pp)) {
        cell.kind = SC_FF;
        cell.out = pp.out;
        st = push_inputs(md, &cell, pp.in, pp.n_in);
    } else {
        cell.kind = storage ? SC_SCAN : SC_OPAQUE;
        boundary_pins(md, node);
        if (!storage) {
            uint32_t *seen = odin3_vec_push(&md->opaque_types);
            st = seen != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
            if (seen != NULL) {
                *seen = type.v;
            }
        }
    }
    return st == ODIN3_OK ? push_cell(md, &cell) : st;
}

static odin3_status add_node(sc_model *md, odin3_node_id node) {
    if (!odin3_node_live(md->mod, node)) {
        return ODIN3_OK;
    }
    odin3_celltype_id type = odin3_node_type(md->mod, node);
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(md->mod), type);
    if (def == NULL) {
        return ODIN3_OK;
    }
    if (def->gran == ODIN3_GRAN_PORT) {
        boundary_pins(md, node);
        return ODIN3_OK;
    }
    if (node_is_constant(md->mod, node)) {
        return ODIN3_OK;
    }
    sc_pins pp;
    bool done = false;
    odin3_status st = ODIN3_OK;
    if (is_function_type(def)) {
        gather_pins(md, node, &pp);
        if (pp.n_out == 1) {
            st = add_function(md, node, &pp, &done);
        }
    }
    return done || st != ODIN3_OK ? st : add_other(md, node, def, type);
}

enum { BYTE_BITS = 8, BYTE_MASK = 0xFF, OUT_SHIFT = 8, COUNT_SHIFT = 40 };

/* FNV-1a over the bytes of a 64-bit word. */
static uint64_t mix(uint64_t hash, uint64_t word) {
    for (int byte = 0; byte < BYTE_BITS; byte++) {
        hash ^= (word >> (BYTE_BITS * byte)) & BYTE_MASK;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_nets(const sc_model *md, uint64_t hash) {
    for (uint32_t id = 1; id < md->n_nets; id++) {
        hash = mix(hash, (uint64_t)md->live[id] | (uint64_t)md->fixed[id] << 1);
        hash = mix(hash, (uint64_t)md->cc[0][id]);
        hash = mix(hash, (uint64_t)md->cc[1][id]);
        hash = mix(hash, (uint64_t)md->co[id]);
    }
    return hash;
}

static uint64_t hash_cells(const sc_model *md, uint64_t hash) {
    for (size_t k = 0; k < md->n_cells; k++) {
        const sc_cell *cell = &md->cells[k];
        uint64_t shape = (uint64_t)cell->kind | (uint64_t)cell->out << OUT_SHIFT |
                         (uint64_t)cell->n_in << COUNT_SHIFT;
        hash = mix(mix(hash, shape), (uint64_t)cell->node);
    }
    for (size_t k = 0; k < md->in_pool.len; k++) {
        hash = mix(hash, *(const uint32_t *)odin3_vec_cat(&md->in_pool, k));
    }
    return hash;
}

static uint64_t hash_functions(const sc_model *md, uint64_t hash) {
    const odin3_vec *cube_pools[] = {&md->prime_pool, &md->sens_pool};
    for (size_t pool = 0; pool < 2; pool++) {
        for (size_t k = 0; k < cube_pools[pool]->len; k++) {
            const sc_cube *cube = odin3_vec_cat(cube_pools[pool], k);
            hash = mix(hash, (uint64_t)cube->care | (uint64_t)cube->val << BYTE_BITS);
        }
    }
    for (size_t k = 0; k < md->cover_pool.len; k++) {
        hash = mix(hash, *(const uint8_t *)odin3_vec_cat(&md->cover_pool, k));
    }
    return hash;
}

/* Fingerprint of what the values are computed from: nets, boundary values, cells, functions. */
static uint64_t model_hash(const sc_model *md) {
    return hash_functions(md, hash_cells(md, hash_nets(md, UINT64_C(14695981039346656037))));
}

odin3_status sc_model_build(sc_model *md, odin3_module *mod, bool seq) {
    memset(md, 0, sizeof *md);
    md->mod = mod;
    md->seq = seq;
    md->n_nets = odin3_module_net_end(mod);
    odin3_vec_init(&md->in_pool, sizeof(uint32_t));
    odin3_vec_init(&md->prime_pool, sizeof(sc_cube));
    odin3_vec_init(&md->sens_ref, sizeof(uint32_t));
    odin3_vec_init(&md->sens_pool, sizeof(sc_cube));
    odin3_vec_init(&md->cover_pool, sizeof(uint8_t));
    odin3_vec_init(&md->opaque_types, sizeof(uint32_t));
    odin3_status st = alloc_nets(md);
    for (uint32_t id = 1; st == ODIN3_OK && id < md->n_nets; id++) {
        init_net(md, id);
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
        st = add_node(md, (odin3_node_id){id});
    }
    md->hash = model_hash(md);
    return st;
}

void sc_model_free(sc_model *md) {
    int64_t *arrays[] = {md->cc[0], md->cc[1], md->sc[0], md->sc[1], md->co, md->so};
    for (size_t k = 0; k < sizeof arrays / sizeof arrays[0]; k++) {
        odin3_util_free(arrays[k]);
    }
    odin3_util_free(md->live);
    odin3_util_free(md->fixed);
    odin3_util_free(md->cells);
    odin3_vec_free(&md->in_pool);
    odin3_vec_free(&md->prime_pool);
    odin3_vec_free(&md->sens_ref);
    odin3_vec_free(&md->sens_pool);
    odin3_vec_free(&md->cover_pool);
    odin3_vec_free(&md->opaque_types);
    memset(md, 0, sizeof *md);
}
