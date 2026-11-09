/*
 * act_model.c — builds the activity model of one module (spec section 3): every live cell becomes a
 * function cell (gates, $lut, $sop, the `adder` hard cell), a storage cell (next-state function
 * with its own state as an input), or an opaque cell whose outputs act as primary inputs.
 */
#include "ir/celltype.h"
#include "ir/lut.h"
#include "ir/value.h"
#include "passes/activity/act_int.h"
#include "sim/gate_truth.h"
#include "util/alloc.h"
#include "util/log.h"

#include <string.h>

enum {
    SOP_PARAM_COVER = 1,
    MAX_FUNC_WORDS = 4,
    BIT_WORD = 64,
    LUT_PARAMS = 2,
    FIRST_CELLS = 64,
    TABLE_MAX = 1 << ACT_MAX_INPUTS,
    NO_Q = 0xFF,
    STORE_INS = 4,
    LUT_MAX_IN = 8,
    NAME_BUF = 32,
    MIN_FF_NAME = 4,
    RESET_LETTERS = 3,
    RESET_EN_LETTERS = 4,
    ADDER_ROWS = 8,
    ADDER_INS = 3
};

static const double k_half = 0.5;

/* Scratch of one build: the function being assembled. */
typedef struct act_build {
    act_model *md;
    const act_opts *opts;
    uint8_t *tab; /* TABLE_MAX bytes */
    uint8_t *aux; /* TABLE_MAX bytes */
} act_build;

typedef struct act_fn {
    uint32_t ins[ACT_MAX_INPUTS + 1];
    uint32_t n_in;
    uint32_t out;
    bool is_ff;
} act_fn;

static bool type_is(const odin3_celltype_def *def, const char *name) {
    return strcmp(def->name, name) == 0;
}

/* --- tables ----------------------------------------------------------------------------------- */

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

static bool table_sop(const act_model *md, odin3_node_id node, uint32_t width, uint8_t *tab) {
    const odin3_value *cover = odin3_node_param(md->mod, node, SOP_PARAM_COVER);
    if (cover == NULL || cover->cover_inputs != width) {
        return false;
    }
    for (uint64_t mint = 0; mint < (1ULL << width); mint++) {
        tab[mint] = sop_eval(cover, mint);
    }
    return true;
}

static bool table_lut(const act_model *md, odin3_node_id node, uint32_t width, uint8_t *tab) {
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
    for (uint32_t mint = 0; mint < (1U << width); mint++) {
        tab[mint] = (uint8_t)(words[mint / BIT_WORD] >> (mint % BIT_WORD) & 1U);
    }
    return true;
}

static bool table_gate(const act_model *md, odin3_celltype_id type, const odin3_celltype_def *def,
                       uint32_t width, uint8_t *tab) {
    uint64_t bits = 0;
    const odin3_design *design = odin3_module_design(md->mod);
    if (!odin3_gate_truth_eligible(def) || odin3_gate_truth_inputs(def) != width ||
        odin3_gate_truth(def, odin3_celltype_lib(design, type), &bits) != ODIN3_OK) {
        return false;
    }
    for (uint32_t mint = 0; mint < (1U << width); mint++) {
        tab[mint] = (uint8_t)(bits >> mint & 1U);
    }
    return true;
}

/* --- normal form: distinct inputs that matter ------------------------------------------------- */

/* Merges pins on one net (XOR(a, a) shows its constant) into aux, then swaps back into tab. */
static void collapse_duplicates(act_build *bd, act_fn *fn) {
    uint32_t map[ACT_MAX_INPUTS];
    uint32_t uniq[ACT_MAX_INPUTS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < fn->n_in; i++) {
        uint32_t j = 0;
        while (j < count && uniq[j] != fn->ins[i]) {
            j++;
        }
        uniq[j] = fn->ins[i];
        map[i] = j;
        count += j == count ? 1U : 0U;
    }
    if (count == fn->n_in) {
        return;
    }
    for (uint32_t row = 0; row < (1U << count); row++) {
        uint32_t mint = 0;
        for (uint32_t i = 0; i < fn->n_in; i++) {
            mint |= ((row >> map[i]) & 1U) << i;
        }
        bd->aux[row] = bd->tab[mint];
    }
    memcpy(bd->tab, bd->aux, (size_t)1U << count);
    memcpy(fn->ins, uniq, count * sizeof uniq[0]);
    fn->n_in = count;
}

static bool depends_on(const act_build *bd, const act_fn *fn, uint32_t idx) {
    uint32_t bit = 1U << idx;
    for (uint32_t mint = 0; mint < (1U << fn->n_in); mint++) {
        if ((mint & bit) == 0 && bd->tab[mint] != bd->tab[mint | bit]) {
            return true;
        }
    }
    return false;
}

/* Removes input idx (the function ignores it): the rows with that bit 0 survive. */
static void drop_input(act_build *bd, act_fn *fn, uint32_t idx) {
    uint32_t low = (1U << idx) - 1U;
    for (uint32_t row = 0; row < (1U << (fn->n_in - 1)); row++) {
        uint32_t mint = (row & low) | ((row & ~low) << 1);
        bd->aux[row] = bd->tab[mint];
    }
    memcpy(bd->tab, bd->aux, (size_t)1U << (fn->n_in - 1));
    for (uint32_t i = idx; i + 1 < fn->n_in; i++) {
        fn->ins[i] = fn->ins[i + 1];
    }
    fn->n_in--;
}

static void drop_unused(act_build *bd, act_fn *fn) {
    for (uint32_t i = fn->n_in; i > 0; i--) {
        if (!depends_on(bd, fn, i - 1)) {
            drop_input(bd, fn, i - 1);
        }
    }
}

static odin3_status push_cell(act_build *bd, const act_fn *fn) {
    act_model *md = bd->md;
    if (md->n_cells == md->cap_cells) {
        size_t cap = md->cap_cells == 0 ? FIRST_CELLS : md->cap_cells * 2;
        act_cell *grown = odin3_util_realloc(md->cells, cap * sizeof *grown);
        if (grown == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        md->cells = grown;
        md->cap_cells = cap;
    }
    act_cell cell = {.out = fn->out,
                     .in_off = (uint32_t)md->in_pool.len,
                     .tab_off = (uint32_t)md->tab_pool.len,
                     .n_in = (uint8_t)fn->n_in,
                     .is_ff = fn->is_ff ? 1U : 0U,
                     .q_index = NO_Q};
    for (uint32_t i = 0; i < fn->n_in; i++) {
        uint32_t *slot = odin3_vec_push(&md->in_pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = fn->ins[i];
        cell.q_index = (uint8_t)(fn->is_ff && fn->ins[i] == fn->out ? i : cell.q_index);
    }
    for (size_t i = 0; i < ((size_t)1U << fn->n_in); i++) {
        uint8_t *slot = odin3_vec_push(&md->tab_pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = bd->tab[i];
    }
    md->cells[md->n_cells++] = cell;
    return ODIN3_OK;
}

static odin3_status add_fn(act_build *bd, act_fn *fn) {
    collapse_duplicates(bd, fn);
    drop_unused(bd, fn);
    return push_cell(bd, fn);
}

/* --- pins ------------------------------------------------------------------------------------- */

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

/* Gathers the input nets of node (pin order) and its single output; false: wide or not 1 output. */
static bool gather(const act_model *md, odin3_node_id node, act_fn *fn) {
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    uint32_t n_out = 0;
    memset(fn, 0, sizeof *fn);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(md->mod, pin);
        if (odin3_pin_drives(md->mod, pin)) {
            fn->out = net.v;
            n_out++;
        } else if (fn->n_in < ACT_MAX_INPUTS) {
            fn->ins[fn->n_in++] = net.v;
        } else {
            return false;
        }
    }
    return n_out == 1;
}

/* --- storage ---------------------------------------------------------------------------------- */

typedef struct ff_spec {
    bool latch, has_r, has_e, r_pos, r_val, e_pos, e_first;
} ff_spec;

/* The name of a storage type split at the first underscore: family and polarity letters. */
typedef struct ff_name {
    char fam[NAME_BUF];
    char let[NAME_BUF];
} ff_name;

static bool pol(char ch, bool *val) {
    *val = ch == 'P';
    return ch == 'P' || ch == 'N';
}

/* Reset polarity and value letters of a type with a reset. */
static bool reset_letters(const char *let, ff_spec *sp) {
    sp->has_r = true;
    sp->r_val = let[1] == '1';
    return pol(let[0], &sp->r_pos) && (let[1] == '0' || let[1] == '1');
}

/* The enable polarity letter. */
static bool enable_letter(char ch, ff_spec *sp) {
    sp->has_e = true;
    return pol(ch, &sp->e_pos);
}

static bool known_family(const char *fam) {
    static const char *const names[] = {"DFF", "DFFE", "SDFF", "SDFFE", "SDFFCE"};
    for (size_t kk = 0; kk < sizeof names / sizeof names[0]; kk++) {
        if (strcmp(fam, names[kk]) == 0) {
            return true;
        }
    }
    return false;
}

/* The two families that have no clock letter: FF and DLATCH; *handled tells which matched. */
static bool unclocked_family(const ff_name *nm, ff_spec *sp, bool *handled) {
    size_t cnt = strlen(nm->let);
    *handled = true;
    if (strcmp(nm->fam, "FF") == 0) {
        return cnt == 0U;
    }
    if (strcmp(nm->fam, "DLATCH") == 0) {
        sp->latch = true;
        return cnt == 1U && enable_letter(nm->let[0], sp);
    }
    *handled = false;
    return false;
}

/* Letters after the family: clock polarity, [reset polarity, reset value], [enable polarity]. */
static bool spec_letters(const ff_name *nm, ff_spec *sp) {
    bool handled = false;
    bool ok = unclocked_family(nm, sp, &handled);
    if (handled) {
        return ok;
    }
    size_t cnt = strlen(nm->let);
    size_t flen = strlen(nm->fam);
    bool enable = flen > 0 && nm->fam[flen - 1U] == 'E';
    bool unused = false;
    sp->e_first = strcmp(nm->fam, "SDFFCE") == 0;
    bool reset = nm->fam[0] == 'S' || cnt == (enable ? RESET_EN_LETTERS : RESET_LETTERS);
    size_t want = 1U + (reset ? 2U : 0U) + (enable ? 1U : 0U);
    if (!known_family(nm->fam) || cnt != want || !pol(nm->let[0], &unused)) {
        return false;
    }
    size_t pos = 1;
    if (reset && !reset_letters(nm->let + pos, sp)) {
        return false;
    }
    pos += reset ? 2U : 0U;
    return !enable || enable_letter(nm->let[pos], sp);
}

static bool parse_ff_name(const char *name, ff_spec *sp) {
    ff_name nm;
    size_t len = strlen(name);
    memset(sp, 0, sizeof *sp);
    memset(&nm, 0, sizeof nm);
    if (len < MIN_FF_NAME || len >= NAME_BUF || name[0] != '$' || name[1] != '_' ||
        name[len - 1U] != '_') {
        return false;
    }
    size_t body = len - 3U;
    const char *sep = memchr(name + 2, '_', body);
    size_t flen = sep != NULL ? (size_t)(sep - (name + 2)) : body;
    memcpy(nm.fam, name + 2, flen);
    if (sep != NULL) {
        memcpy(nm.let, sep + 1, body - flen - 1U);
    }
    return spec_letters(&nm, sp);
}

/* g(d, r, e, q) of the spec, with active levels applied. */
static uint8_t ff_next(const ff_spec *sp, bool dat, bool rst, bool ena, bool qq) {
    bool r_act = sp->has_r && rst == sp->r_pos;
    bool e_act = !sp->has_e || ena == sp->e_pos;
    if (sp->e_first) {
        if (!e_act) {
            return qq;
        }
        return r_act ? sp->r_val : dat;
    }
    if (r_act) {
        return sp->r_val;
    }
    return e_act ? dat : qq;
}

/* Fills the inputs [D, R?, E?, Q] and the table of a recognised storage cell. */
static bool ff_function(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                        const ff_spec *sp, act_fn *fn) {
    odin3_pinslice pins = odin3_node_pins(bd->md->mod, node);
    uint32_t net[STORE_INS] = {0};
    memset(fn, 0, sizeof *fn);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        const char *port = def->ports[odin3_pin_port(bd->md->mod, pin)].name;
        uint32_t nid = odin3_pin_net(bd->md->mod, pin).v;
        if (odin3_pin_drives(bd->md->mod, pin)) {
            fn->out = nid;
        } else if (strcmp(port, "D") == 0) {
            net[0] = nid;
        } else if (strcmp(port, "R") == 0) {
            net[1] = nid;
        } else if (strcmp(port, "E") == 0) {
            net[2] = nid;
        }
    }
    if (fn->out == 0 || net[0] == 0 || (sp->has_r && net[1] == 0) || (sp->has_e && net[2] == 0)) {
        return false;
    }
    fn->ins[0] = net[0];
    fn->ins[1] = net[1];
    fn->ins[2] = net[2];
    fn->ins[3] = fn->out;
    fn->n_in = STORE_INS;
    fn->is_ff = true;
    for (uint32_t mint = 0; mint < (1U << STORE_INS); mint++) {
        bd->tab[mint] = ff_next(sp, mint & 1U, (mint >> 1 & 1U) != 0, (mint >> 2 & 1U) != 0,
                                (mint >> 3 & 1U) != 0);
    }
    return true;
}

/* A flip-flop with no other control than D and a clock, whatever its name. */
static bool plain_function(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                           act_fn *fn) {
    odin3_pinslice pins = odin3_node_pins(bd->md->mod, node);
    memset(fn, 0, sizeof *fn);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        const char *port = def->ports[odin3_pin_port(bd->md->mod, pin)].name;
        uint32_t nid = odin3_pin_net(bd->md->mod, pin).v;
        if (odin3_pin_drives(bd->md->mod, pin)) {
            fn->out = nid;
        } else if (strcmp(port, "D") == 0) {
            fn->ins[0] = nid;
        } else if ((def->flags & ODIN3_CT_CLOCK_PIN0) == 0 ||
                   odin3_pin_port(bd->md->mod, pin) != 0) {
            return false;
        }
    }
    fn->n_in = 1;
    fn->is_ff = true;
    bd->tab[0] = 0;
    bd->tab[1] = 1;
    return fn->out != 0 && fn->ins[0] != 0;
}

static void mark_clock(act_model *md, odin3_node_id node, const odin3_celltype_def *def) {
    if ((def->flags & ODIN3_CT_CLOCK_PIN0) == 0 || (def->flags & ODIN3_CT_SEQ_EDGE) == 0) {
        return;
    }
    odin3_pinslice pins = odin3_node_port(md->mod, node, 0);
    if (pins.count == 1) {
        odin3_net_id net = odin3_pin_net(md->mod, pins.first);
        if (odin3_net_valid(net)) {
            md->clock[net.v] = 1;
        }
    }
}

static odin3_status add_storage(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                                bool *done) {
    ff_spec sp;
    act_fn fn;
    *done = false;
    mark_clock(bd->md, node, def);
    if ((parse_ff_name(def->name, &sp) && ff_function(bd, node, def, &sp, &fn)) ||
        plain_function(bd, node, def, &fn)) {
        *done = true;
        bd->md->latches += sp.latch ? 1U : 0U;
        return add_fn(bd, &fn);
    }
    return ODIN3_OK;
}

/* --- combinational cells ---------------------------------------------------------------------- */

static bool func_table(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                       const act_fn *fn) {
    const act_model *md = bd->md;
    if (type_is(def, "$sop")) {
        return table_sop(md, node, fn->n_in, bd->tab);
    }
    if (type_is(def, "$lut")) {
        return fn->n_in <= LUT_MAX_IN && table_lut(md, node, fn->n_in, bd->tab);
    }
    return table_gate(md, odin3_node_type(md->mod, node), def, fn->n_in, bd->tab);
}

static bool is_function_type(const odin3_celltype_def *def) {
    return def->gran == ODIN3_GRAN_BIT &&
           (type_is(def, "$sop") || type_is(def, "$lut") ||
            (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) == 0);
}

static odin3_status add_function(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                                 bool *done) {
    act_fn fn;
    *done = false;
    if (!gather(bd->md, node, &fn)) {
        bd->md->wide_cells += fn.n_in >= ACT_MAX_INPUTS ? 1U : 0U;
        return ODIN3_OK;
    }
    if (fn.n_in > bd->opts->exact) {
        bd->md->wide_cells++;
        return ODIN3_OK;
    }
    if (!func_table(bd, node, def, &fn)) {
        return ODIN3_OK;
    }
    *done = true;
    return fn.out == 0 ? ODIN3_OK : add_fn(bd, &fn);
}

/* The pins of the hard `adder`: a, b, cin, then cout and sumout. */
typedef struct adder_pins {
    uint32_t ins[ADDER_INS];
    uint32_t cout, sum;
} adder_pins;

static bool adder_gather(const act_model *md, odin3_node_id node, const odin3_celltype_def *def,
                         adder_pins *ap) {
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    memset(ap, 0, sizeof *ap);
    for (uint32_t kk = 0; kk < pins.count; kk++) {
        odin3_pin_id pin = {pins.first.v + kk};
        const char *port = def->ports[odin3_pin_port(md->mod, pin)].name;
        uint32_t nid = odin3_pin_net(md->mod, pin).v;
        ap->ins[0] = strcmp(port, "a") == 0 ? nid : ap->ins[0];
        ap->ins[1] = strcmp(port, "b") == 0 ? nid : ap->ins[1];
        ap->ins[2] = strcmp(port, "cin") == 0 ? nid : ap->ins[2];
        ap->cout = strcmp(port, "cout") == 0 ? nid : ap->cout;
        ap->sum = strcmp(port, "sumout") == 0 ? nid : ap->sum;
    }
    return ap->ins[0] != 0 && ap->ins[1] != 0 && ap->ins[2] != 0;
}

/* The hard `adder` (a, b, cin -> cout, sumout) as two function cells: sum is the parity of the
 * three inputs, carry their majority. */
static odin3_status add_adder(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                              bool *done) {
    adder_pins ap;
    *done = adder_gather(bd->md, node, def, &ap);
    if (!*done) {
        return ODIN3_OK;
    }
    const uint32_t outs[2] = {ap.sum, ap.cout};
    odin3_status st = ODIN3_OK;
    for (uint32_t kk = 0; st == ODIN3_OK && kk < 2U; kk++) {
        act_fn fn = {.ins = {ap.ins[0], ap.ins[1], ap.ins[2]}, .n_in = ADDER_INS, .out = outs[kk]};
        for (uint32_t mint = 0; mint < ADDER_ROWS; mint++) {
            uint32_t sum = (mint & 1U) + (mint >> 1 & 1U) + (mint >> 2 & 1U);
            bd->tab[mint] = (uint8_t)(kk == 0 ? (sum & 1U) : (sum >> 1));
        }
        st = outs[kk] != 0 ? add_fn(bd, &fn) : ODIN3_OK;
    }
    return st;
}

/* --- opaque cells ----------------------------------------------------------------------------- */

static odin3_status add_opaque(act_build *bd, odin3_node_id node, const odin3_celltype_def *def,
                               odin3_celltype_id type) {
    act_model *md = bd->md;
    odin3_pinslice pins = odin3_node_pins(md->mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(md->mod, pin);
        if (odin3_net_valid(net) && odin3_pin_drives(md->mod, pin)) {
            md->kind[net.v] = ACT_OPAQUE;
        }
    }
    md->opaque_cells++;
    md->instances += def->gran == ODIN3_GRAN_MODULE ? 1U : 0U;
    uint32_t *seen = odin3_vec_push(&md->opaque_types);
    if (seen == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *seen = type.v;
    return ODIN3_OK;
}

/* True when --opaque-type named this cell type. */
static bool forced_opaque(const act_build *bd, const odin3_celltype_def *def) {
    const char *list = bd->opts->opaque_types.data;
    size_t len = strlen(def->name);
    while (list != NULL && *list != '\0') {
        const char *eol = strchr(list, '\n');
        size_t cur = eol != NULL ? (size_t)(eol - list) : strlen(list);
        if (cur == len && strncmp(list, def->name, len) == 0) {
            return true;
        }
        list = eol != NULL ? eol + 1 : NULL;
    }
    return false;
}

static odin3_status add_node(act_build *bd, odin3_node_id node) {
    const act_model *md = bd->md;
    if (!odin3_node_live(md->mod, node)) {
        return ODIN3_OK;
    }
    odin3_celltype_id type = odin3_node_type(md->mod, node);
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(md->mod), type);
    if (def == NULL || def->gran == ODIN3_GRAN_PORT || node_is_constant(md->mod, node)) {
        return ODIN3_OK;
    }
    bool done = false;
    odin3_status st = ODIN3_OK;
    if (forced_opaque(bd, def)) {
        return add_opaque(bd, node, def, type);
    }
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        st = add_storage(bd, node, def, &done);
    } else if (type_is(def, "adder")) {
        st = add_adder(bd, node, def, &done);
    } else if (is_function_type(def)) {
        st = add_function(bd, node, def, &done);
    }
    return done || st != ODIN3_OK ? st : add_opaque(bd, node, def, type);
}

/* --- nets ------------------------------------------------------------------------------------- */

static odin3_status alloc_nets(act_model *md) {
    size_t cnt = (size_t)md->n_nets + 1;
    md->kind = odin3_util_calloc(cnt);
    md->clock = odin3_util_calloc(cnt);
    md->prob = odin3_util_calloc(cnt * sizeof(double));
    md->dens = odin3_util_calloc(cnt * sizeof(double));
    return md->kind != NULL && md->clock != NULL && md->prob != NULL && md->dens != NULL
               ? ODIN3_OK
               : ODIN3_ERR_NO_MEMORY;
}

/* P1 of a constant: x and z are an unknown bit, 0.5. */
static double const_prob(odin3_const cv) {
    if (cv == ODIN3_CONST_1) {
        return 1.0;
    }
    return cv == ODIN3_CONST_0 ? 0.0 : k_half;
}

/* Marks which nets have any pin; constants and primary inputs get their kind and statistics. */
static void classify_net(act_model *md, uint32_t id, const act_opts *opts) {
    odin3_net_id net = {id};
    if (!odin3_net_live(md->mod, net)) {
        return;
    }
    odin3_const cv = odin3_net_const_value(md->mod, net);
    if (cv == ODIN3_CONST_NONE) {
        return;
    }
    md->kind[id] = ACT_CONST;
    md->prob[id] = const_prob(cv);
    md->dens[id] = 0.0;
    (void)opts;
}

static void mark_pins(act_model *md, const act_opts *opts) {
    const odin3_module *mod = md->mod;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
        odin3_pinslice pins = odin3_node_pins(mod, node);
        for (uint32_t i = 0; def != NULL && i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            odin3_net_id net = odin3_pin_net(mod, pin);
            if (!odin3_net_valid(net) || md->kind[net.v] == ACT_CONST) {
                continue;
            }
            bool primary = def->gran == ODIN3_GRAN_PORT && odin3_pin_drives(mod, pin);
            if (primary) {
                md->kind[net.v] = ACT_PI;
                md->prob[net.v] = opts->pi_prob;
                md->dens[net.v] = opts->pi_density;
            } else if (md->kind[net.v] == ACT_UNUSED) {
                md->kind[net.v] = ACT_UNDRIVEN;
            }
        }
    }
}

static void mark_drivers(act_model *md) {
    for (size_t k = 0; k < md->n_cells; k++) {
        const act_cell *cell = &md->cells[k];
        if (md->kind[cell->out] != ACT_PI && md->kind[cell->out] != ACT_CONST) {
            md->kind[cell->out] = cell->is_ff ? ACT_FF : ACT_COMB;
        }
    }
}

enum { BYTE_BITS = 8, BYTE_MASK = 0xFF };
static const double k_clock_density = 2.0;

/* The reference clock: the one named by --clock, else the first primary-input clock. */
static void pick_ref_clock(act_model *md, const act_opts *opts) {
    const odin3_design *design = odin3_module_design(md->mod);
    for (uint32_t id = 1; id < md->n_nets; id++) {
        if (md->kind[id] != ACT_CLOCK) {
            continue;
        }
        const char *name = odin3_strtab_get(odin3_design_strtab(design),
                                            odin3_net_name(md->mod, (odin3_net_id){id}));
        if (opts->clock.len > 0 && name != NULL && strcmp(name, opts->clock.data) == 0) {
            md->ref_clock = id;
            return;
        }
        md->ref_clock = md->ref_clock == 0 ? id : md->ref_clock;
    }
}

/* Statistics of the opaque outputs; the primary inputs that drive clock pins become clocks. */
static void finish_nets(act_model *md, const act_opts *opts) {
    for (uint32_t id = 1; id < md->n_nets; id++) {
        if (md->kind[id] == ACT_OPAQUE) {
            md->prob[id] = opts->opaque_prob;
            md->dens[id] = opts->opaque_density;
        } else if (md->kind[id] == ACT_UNDRIVEN) {
            md->undriven++;
        } else if (md->kind[id] == ACT_PI && md->clock[id] != 0) {
            md->kind[id] = ACT_CLOCK;
            md->prob[id] = k_half;
            md->dens[id] = k_clock_density;
        }
    }
    pick_ref_clock(md, opts);
}

static uint64_t mix(uint64_t hash, uint64_t word) {
    for (int byte = 0; byte < BYTE_BITS; byte++) {
        hash ^= (word >> (BYTE_BITS * byte)) & BYTE_MASK;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Fingerprint of what the values are computed from: nets, cells, inputs and tables. */
static uint64_t model_hash(const act_model *md) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t id = 1; id < md->n_nets; id++) {
        hash = mix(hash, md->kind[id]);
    }
    for (size_t k = 0; k < md->n_cells; k++) {
        const act_cell *cell = &md->cells[k];
        hash = mix(mix(hash, cell->out), (uint64_t)cell->n_in | (uint64_t)cell->is_ff << BYTE_BITS);
    }
    for (size_t k = 0; k < md->in_pool.len; k++) {
        hash = mix(hash, *(const uint32_t *)odin3_vec_cat(&md->in_pool, k));
    }
    for (size_t k = 0; k < md->tab_pool.len; k++) {
        hash = mix(hash, *(const uint8_t *)odin3_vec_cat(&md->tab_pool, k));
    }
    return hash;
}

const uint32_t *act_cell_ins(const act_model *md, const act_cell *cell) {
    return cell->n_in == 0 ? NULL : odin3_vec_cat(&md->in_pool, cell->in_off);
}

odin3_status act_model_build(act_model *md, odin3_module *mod, const act_opts *opts) {
    memset(md, 0, sizeof *md);
    md->mod = mod;
    md->n_nets = odin3_module_net_end(mod);
    odin3_vec_init(&md->in_pool, sizeof(uint32_t));
    odin3_vec_init(&md->tab_pool, sizeof(uint8_t));
    odin3_vec_init(&md->opaque_types, sizeof(uint32_t));
    act_build bd = {.md = md, .opts = opts};
    bd.tab = odin3_util_calloc(TABLE_MAX);
    bd.aux = odin3_util_calloc(TABLE_MAX);
    odin3_status st = bd.tab != NULL && bd.aux != NULL ? alloc_nets(md) : ODIN3_ERR_NO_MEMORY;
    for (uint32_t id = 1; st == ODIN3_OK && id < md->n_nets; id++) {
        classify_net(md, id, opts);
    }
    if (st == ODIN3_OK) {
        mark_pins(md, opts);
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
        st = add_node(&bd, (odin3_node_id){id});
    }
    if (st == ODIN3_OK) {
        mark_drivers(md);
        finish_nets(md, opts);
        md->hash = model_hash(md);
    }
    odin3_util_free(bd.tab);
    odin3_util_free(bd.aux);
    return st;
}

void act_model_free(act_model *md) {
    odin3_util_free(md->kind);
    odin3_util_free(md->clock);
    odin3_util_free(md->prob);
    odin3_util_free(md->dens);
    odin3_util_free(md->cells);
    odin3_vec_free(&md->in_pool);
    odin3_vec_free(&md->tab_pool);
    odin3_vec_free(&md->opaque_types);
    memset(md, 0, sizeof *md);
}
