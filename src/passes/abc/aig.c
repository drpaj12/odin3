/*
 * aig.c — the AIG builder of the in-memory exchange (3D spec §6.3, AB-16; plan Task 20).
 */
#include "passes/abc/aig.h"

#include "backends/blif/isop.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "passes/abc/abc.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    DOMAIN_TEXT_BYTES = 160,
    LIT_BITS = 32,
    LIT_FLIP = 1,       /* the complement bit of a literal */
    LIT_FALSE = 0,      /* constant 0 */
    LIT_TRUE = 1,       /* constant 1 */
    SPACED_ROW_TAIL = 3 /* isop rows: planes, a blank, the output character, a newline */
};

/* The state of one build. */
typedef struct ab {
    odin3_abc_run *run;
    const odin3_abc_region *region;
    const odin3_module *module;
    odin3_abc_aigb *out;
    odin3_vec inverted; /* uint8_t per register: INIT 1 (AB-13) */
    odin3_vec ins;      /* int32_t: the input literals of the cell being built */
    odin3_vec cube;     /* int32_t: the literals of one cube */
    odin3_vec cubes;    /* int32_t: the cube literals of one cover */
    uint32_t cap;       /* the object cap */
    uint32_t owner;     /* the node ID of the cell being built */
} ab;

/* Two literals, in call order. */
typedef struct lit_pair {
    int32_t first;
    int32_t second;
} lit_pair;

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

static int32_t *lits_of(const odin3_vec *vec) {
    return vec->data;
}

static uint32_t obj_count(const ab *bld) {
    return (uint32_t)(bld->out->objs.len / 2U);
}

/* --- objects ------------------------------------------------------------------------------- */

static const char *module_text(const odin3_abc_run *run) {
    return odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module));
}

static odin3_status over_cap(const ab *bld) {
    char domain[DOMAIN_TEXT_BYTES];
    odin3_abc_domain_text(bld->run->module, bld->run->domain, domain, sizeof domain);
    odin3_log(ODIN3_LOG_ERROR, "abc: module '%s', %s: the AIG needs more than %u objects",
              module_text(bld->run), domain, (unsigned)bld->cap);
    return ODIN3_ERR_PARSE;
}

/* Appends object (pair) owned by bld->owner; *lit gets its literal. PARSE (logged) over the cap. */
static odin3_status push_obj(ab *bld, lit_pair pair, int32_t *lit) {
    if (obj_count(bld) >= bld->cap) {
        return over_cap(bld);
    }
    odin3_abc_aigb *aig = bld->out;
    odin3_status st = odin3_vec_reserve(&aig->objs, aig->objs.len + 2U);
    st = st == ODIN3_OK ? odin3_vec_reserve(&aig->owner, aig->owner.len + 1U) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    int32_t *slot = odin3_vec_push(&aig->objs);
    int32_t *slot1 = odin3_vec_push(&aig->objs);
    uint32_t *own = odin3_vec_push(&aig->owner);
    slot[0] = pair.first;
    slot1[0] = pair.second;
    *own = bld->owner;
    *lit = ODIN3_ABC_AIG_LIT(obj_count(bld) - 1U);
    return ODIN3_OK;
}

/* One AND of two literals with the trivial cases (a&0, a&1, a&a, a&~a) and the hash. */
static odin3_status and_lit(ab *bld, lit_pair in, int32_t *out) {
    int32_t lo = in.first < in.second ? in.first : in.second;
    int32_t hi = in.first < in.second ? in.second : in.first;
    if (lo == LIT_FALSE) {
        *out = LIT_FALSE;
        return ODIN3_OK;
    }
    if (lo == LIT_TRUE || lo == hi) {
        *out = hi; /* a&1 = a, a&a = a */
        return ODIN3_OK;
    }
    if (hi == (lo ^ LIT_FLIP)) {
        *out = LIT_FALSE;
        return ODIN3_OK;
    }
    uint64_t key = ((uint64_t)(uint32_t)lo << LIT_BITS) | (uint32_t)hi;
    uint64_t found = 0;
    if (odin3_u64map_get(bld->out->hash, key, &found)) {
        *out = (int32_t)found;
        return ODIN3_OK;
    }
    odin3_status st = push_obj(bld, (lit_pair){lo, hi}, out);
    return st == ODIN3_OK
               ? odin3_u64map_put(bld->out->hash, (odin3_kv){key, (uint64_t)(uint32_t)*out})
               : st;
}

/* Pairwise AND of lits[0..count) in place, left to right, level by level (Mini_AigAndMulti). An
 * empty list is the constant 1. */
static odin3_status and_balanced(ab *bld, int32_t *lits, uint32_t count, int32_t *out) {
    if (count == 0) {
        *out = LIT_TRUE;
        return ODIN3_OK;
    }
    while (count > 1) {
        uint32_t used = 0;
        for (uint32_t i = 0; i + 1U < count; i += 2U) {
            odin3_status st = and_lit(bld, (lit_pair){lits[i], lits[i + 1U]}, &lits[used++]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        if ((count & 1U) != 0) {
            lits[used++] = lits[count - 1U];
        }
        count = used;
    }
    *out = lits[0];
    return ODIN3_OK;
}

static odin3_status push_lit(odin3_vec *vec, int32_t lit) {
    int32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = lit;
    return ODIN3_OK;
}

/* --- covers -------------------------------------------------------------------------------- */

/* Cover rows as one view: row i starts at data + i * stride with one plane character per input;
 * the output character follows the planes (after one blank when spaced). */
typedef struct rows_view {
    const char *data;
    size_t stride;
    uint32_t count;
    bool spaced;
} rows_view;

/* The literal of one cube: the AND of its literals in input order. */
static odin3_status cube_lit(ab *bld, const char *row, int32_t *out) {
    odin3_vec_clear(&bld->cube);
    uint32_t inputs = (uint32_t)bld->ins.len;
    for (uint32_t col = 0; col < inputs; col++) {
        if (row[col] == '-') {
            continue;
        }
        int32_t lit = lits_of(&bld->ins)[col] ^ (row[col] == '1' ? 0 : LIT_FLIP);
        odin3_status st = push_lit(&bld->cube, lit);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return and_balanced(bld, lits_of(&bld->cube), (uint32_t)bld->cube.len, out);
}

/* A row is valid when its planes are 0, 1 or - and its output character is 0 or 1 and equals the
 * first row's (BLIF has one polarity per cover); logs and says INVALID_ARG otherwise. */
static odin3_status check_row(const ab *bld, const char *row, const rows_view *rows) {
    uint32_t inputs = (uint32_t)bld->ins.len;
    char out_char = row[inputs + (rows->spaced ? 1U : 0U)];
    const char *first = rows->data;
    bool ok = (out_char == '0' || out_char == '1') &&
              out_char == first[inputs + (rows->spaced ? 1U : 0U)];
    for (uint32_t col = 0; ok && col < inputs; col++) {
        ok = row[col] == '0' || row[col] == '1' || row[col] == '-';
    }
    if (!ok) {
        odin3_log(ODIN3_LOG_ERROR,
                  "abc: module '%s': a cover has a row with a plane or output character other than "
                  "0, 1 or -, or with another output polarity than the first row",
                  module_text(bld->run));
    }
    return ok ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

/* The literal of a cover over the input literals in bld->ins: a balanced OR of its cubes in row
 * order (an AND of complements), complemented for an off-set cover. */
static odin3_status cover_lit(ab *bld, const rows_view *rows, int32_t *out) {
    uint32_t inputs = (uint32_t)bld->ins.len;
    odin3_vec_clear(&bld->cubes);
    bool off_set = false;
    for (uint32_t num = 0; num < rows->count; num++) {
        const char *row = rows->data + (size_t)num * rows->stride;
        int32_t cube = 0;
        odin3_status st = check_row(bld, row, rows);
        st = st == ODIN3_OK ? cube_lit(bld, row, &cube) : st;
        st = st == ODIN3_OK ? push_lit(&bld->cubes, cube ^ LIT_FLIP) : st;
        if (st != ODIN3_OK) {
            return st;
        }
        off_set = row[inputs + (rows->spaced ? 1U : 0U)] == '0';
    }
    int32_t not_on = 0; /* the AND of the complemented cubes: the complement of the on-set */
    odin3_status st = and_balanced(bld, lits_of(&bld->cubes), (uint32_t)bld->cubes.len, &not_on);
    *out = off_set ? not_on : not_on ^ LIT_FLIP; /* an empty cover is the constant 0 */
    return st;
}

/* A table of bld->ins.len inputs as its irredundant cover (odin3_blif_isop), then cover_lit. */
static odin3_status table_lit(ab *bld, const uint64_t *words, int32_t *out) {
    uint32_t inputs = (uint32_t)bld->ins.len;
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t n_rows = 0;
    int constant = -1;
    odin3_status st = odin3_blif_isop(words, inputs, &cover, &n_rows, &constant);
    if (st == ODIN3_OK && constant >= 0) {
        *out = constant == 1 ? LIT_TRUE : LIT_FALSE;
    } else if (st == ODIN3_OK) {
        rows_view rows = {cover.data, (size_t)inputs + SPACED_ROW_TAIL, n_rows, true};
        st = cover_lit(bld, &rows, out);
    }
    odin3_strbuf_free(&cover);
    return st;
}

static odin3_status sop_lit(ab *bld, odin3_node_id node, int32_t *out) {
    const odin3_value *cover = odin3_node_param(bld->module, node, 1);
    size_t stride = bld->ins.len + 1U;
    rows_view rows = {(const char *)cover->bits, stride, (uint32_t)(cover->len / stride), false};
    return cover_lit(bld, &rows, out);
}

static odin3_status lut_lit(ab *bld, odin3_node_id node, int32_t *out) {
    uint64_t words[ODIN3_LUT_MAX_WORDS];
    odin3_value params[2] = {*odin3_node_param(bld->module, node, ODIN3_LUT_P_WIDTH),
                             *odin3_node_param(bld->module, node, ODIN3_LUT_P_LUT)};
    if (!odin3_lut_table(params, words, ODIN3_LUT_MAX_WORDS)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return table_lit(bld, words, out);
}

/* --- literals of nets ---------------------------------------------------------------------- */

/* The literal a reader of net sees: a constant, a CI, a register's value, or a soft output. */
static int32_t read_lit(const ab *bld, odin3_net_id net) {
    int lit = odin3_abc_pin_literal(bld->run, net);
    if (!odin3_net_valid(net) || lit == 0) {
        return LIT_FALSE;
    }
    if (lit == 1) {
        return LIT_TRUE;
    }
    uint32_t ci = u32_at(&bld->region->ci_of, net.v);
    uint32_t reg = u32_at(&bld->region->q_of, net.v);
    if (ci != 0) {
        return ODIN3_ABC_AIG_LIT(ci); /* object 1 + (ci - 1) */
    }
    if (reg != 0) {
        uint32_t obj = 1U + (uint32_t)bld->region->cis.len + (reg - 1U);
        return ODIN3_ABC_AIG_LIT(obj) ^ (int32_t)((const uint8_t *)bld->inverted.data)[reg - 1U];
    }
    uint64_t found = 0;
    return odin3_u64map_get(bld->out->net_lit, net.v, &found) ? (int32_t)found : LIT_FALSE;
}

/* The literal of a CO: a register's value, else the soft output the net carries. */
static int32_t co_lit(const ab *bld, odin3_net_id net) {
    uint32_t reg = u32_at(&bld->region->q_of, net.v);
    if (reg != 0) {
        uint32_t obj = 1U + (uint32_t)bld->region->cis.len + (reg - 1U);
        return ODIN3_ABC_AIG_LIT(obj) ^ (int32_t)((const uint8_t *)bld->inverted.data)[reg - 1U];
    }
    uint64_t found = 0;
    return odin3_u64map_get(bld->out->net_lit, net.v, &found) ? (int32_t)found : LIT_FALSE;
}

/* Records the literal of a soft output net, and the lowest-ID net of its variable. */
static odin3_status note_net(ab *bld, odin3_net_id net, int32_t lit) {
    odin3_status st =
        odin3_u64map_put(bld->out->net_lit, (odin3_kv){net.v, (uint64_t)(uint32_t)lit});
    uint32_t var = (uint32_t)lit >> 1;
    uint64_t known = 0;
    if (st != ODIN3_OK || var == 0) {
        return st;
    }
    if (odin3_u64map_get(bld->out->var_net, var, &known) && known <= net.v) {
        return ODIN3_OK;
    }
    return odin3_u64map_put(bld->out->var_net, (odin3_kv){var, net.v});
}

/* --- cells --------------------------------------------------------------------------------- */

/* The literals of the input pins of node, in port order, into bld->ins. */
static odin3_status collect_ins(ab *bld, odin3_node_id node) {
    odin3_vec_clear(&bld->ins);
    odin3_pinslice pins = odin3_node_pins(bld->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_dir(bld->module, pin) != ODIN3_DIR_IN) {
            continue;
        }
        odin3_status st = push_lit(&bld->ins, read_lit(bld, odin3_pin_net(bld->module, pin)));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_net_id output_net(const ab *bld, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(bld->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(bld->module, pin)) {
            return odin3_pin_net(bld->module, pin);
        }
    }
    return (odin3_net_id){0};
}

/* The literal of the soft cell node's output. */
static odin3_status cell_lit(ab *bld, odin3_node_id node, int32_t *out) {
    odin3_celltype_id type = odin3_node_type(bld->module, node);
    uint64_t table = 0;
    uint32_t inputs = 0;
    odin3_status gate = odin3_abc_gate_table(bld->run, type, &table, &inputs);
    if (gate == ODIN3_OK) {
        return table_lit(bld, &table, out);
    }
    if (gate == ODIN3_ERR_NO_MEMORY) {
        return gate;
    }
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(bld->module), type);
    return strcmp(def->name, "$lut") == 0 ? lut_lit(bld, node, out) : sop_lit(bld, node, out);
}

static odin3_status build_cell(ab *bld, odin3_node_id node) {
    bld->owner = node.v;
    odin3_status st = collect_ins(bld, node);
    int32_t lit = 0;
    st = st == ODIN3_OK ? cell_lit(bld, node, &lit) : st;
    odin3_net_id net = output_net(bld, node);
    return st == ODIN3_OK && odin3_net_valid(net) ? note_net(bld, net, lit) : st;
}

/* --- the file ------------------------------------------------------------------------------ */

static odin3_status init_state(ab *bld, odin3_abc_run *run, const odin3_abc_region *region,
                               odin3_abc_aigb *out) {
    *out = (odin3_abc_aigb){0};
    odin3_vec_init(&out->objs, sizeof(int32_t));
    odin3_vec_init(&out->owner, sizeof(uint32_t));
    out->hash = odin3_u64map_create(0);
    out->net_lit = odin3_u64map_create(0);
    out->var_net = odin3_u64map_create(0);
    *bld = (ab){run, region, run->module, out, {0}, {0}, {0}, {0}, 0, 0};
    odin3_vec_init(&bld->inverted, 1);
    odin3_vec_init(&bld->ins, sizeof(int32_t));
    odin3_vec_init(&bld->cube, sizeof(int32_t));
    odin3_vec_init(&bld->cubes, sizeof(int32_t));
    bld->cap = odin3_abc_run_limits_get()->aig_objs;
    return out->hash != NULL && out->net_lit != NULL && out->var_net != NULL ? ODIN3_OK
                                                                             : ODIN3_ERR_NO_MEMORY;
}

static void free_state(ab *bld) {
    odin3_vec_free(&bld->inverted);
    odin3_vec_free(&bld->ins);
    odin3_vec_free(&bld->cube);
    odin3_vec_free(&bld->cubes);
}

/* One uint8_t per register: 1 for INIT 1 (AB-13). */
static odin3_status mark_inverted(ab *bld) {
    for (size_t j = 0; j < bld->region->regs.len; j++) {
        odin3_node_id reg = {u32_at(&bld->region->regs, j)};
        const odin3_value *init = odin3_node_param(bld->module, reg, 0);
        uint8_t *slot = odin3_vec_push(&bld->inverted);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = init != NULL && init->kind == ODIN3_VAL_INT && init->i == 1 ? 1U : 0U;
    }
    return ODIN3_OK;
}

/* Object 0, then one PI per CI and one per register output. */
static odin3_status build_inputs(ab *bld) {
    const odin3_abc_region *region = bld->region;
    uint32_t pis = (uint32_t)(region->cis.len + region->regs.len);
    int32_t lit = 0;
    odin3_status st = push_obj(bld, (lit_pair){ODIN3_ABC_AIG_NULL, ODIN3_ABC_AIG_NULL}, &lit);
    for (uint32_t pi = 0; st == ODIN3_OK && pi < pis; pi++) {
        st = push_obj(bld, (lit_pair){ODIN3_ABC_AIG_NULL, ODIN3_ABC_AIG_NULL}, &lit);
    }
    bld->out->n_pis = pis;
    bld->out->n_regs = (uint32_t)region->regs.len;
    return st;
}

/* The POs: the COs, then the register inputs (complemented when the register is inverted). */
static odin3_status build_outputs(ab *bld) {
    const odin3_abc_region *region = bld->region;
    odin3_status st = ODIN3_OK;
    int32_t lit = 0;
    bld->owner = 0;
    for (size_t at = 0; st == ODIN3_OK && at < region->cos.len; at++) {
        int32_t drv = co_lit(bld, (odin3_net_id){u32_at(&region->cos, at)});
        st = push_obj(bld, (lit_pair){drv, ODIN3_ABC_AIG_NULL}, &lit);
    }
    for (size_t reg = 0; st == ODIN3_OK && reg < region->regs.len; reg++) {
        int32_t drv = read_lit(bld, (odin3_net_id){u32_at(&region->reg_d, reg)}) ^
                      (int32_t)((const uint8_t *)bld->inverted.data)[reg];
        st = push_obj(bld, (lit_pair){drv, ODIN3_ABC_AIG_NULL}, &lit);
    }
    bld->out->n_pos = (uint32_t)(region->cos.len + region->regs.len);
    return st;
}

odin3_status odin3_abc_aig_build(odin3_abc_run *run, const odin3_abc_region *region,
                                 odin3_abc_aigb *out) {
    ab bld;
    odin3_status st = init_state(&bld, run, region, out);
    st = st == ODIN3_OK ? mark_inverted(&bld) : st;
    st = st == ODIN3_OK ? build_inputs(&bld) : st;
    for (size_t at = 0; st == ODIN3_OK && at < region->soft.len; at++) {
        st = build_cell(&bld, (odin3_node_id){u32_at(&region->soft, at)});
    }
    st = st == ODIN3_OK ? build_outputs(&bld) : st;
    free_state(&bld);
    return st;
}

odin3_abc_aig odin3_abc_aig_view(const odin3_abc_aigb *aig) {
    return (odin3_abc_aig){aig->objs.data, (uint32_t)(aig->objs.len / 2U), aig->n_regs};
}

int32_t odin3_abc_aig_lit(const odin3_abc_aigb *aig, odin3_net_id net) {
    uint64_t found = 0;
    if (aig->net_lit == NULL || !odin3_u64map_get(aig->net_lit, net.v, &found)) {
        return -1;
    }
    return (int32_t)found;
}

odin3_net_id odin3_abc_aig_net_of_var(const odin3_abc_aigb *aig, uint32_t var) {
    uint64_t found = 0;
    if (aig->var_net == NULL || !odin3_u64map_get(aig->var_net, var, &found)) {
        return (odin3_net_id){0};
    }
    return (odin3_net_id){(uint32_t)found};
}

void odin3_abc_aig_free(odin3_abc_aigb *aig) {
    if (aig == NULL) {
        return;
    }
    odin3_vec_free(&aig->objs);
    odin3_vec_free(&aig->owner);
    odin3_u64map_destroy(aig->hash);
    odin3_u64map_destroy(aig->net_lit);
    odin3_u64map_destroy(aig->var_net);
    *aig = (odin3_abc_aigb){0};
    odin3_vec_init(&aig->objs, sizeof(int32_t));
    odin3_vec_init(&aig->owner, sizeof(uint32_t));
}
