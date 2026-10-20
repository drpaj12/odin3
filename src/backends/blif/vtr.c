/* vtr.c — the VTR dialect's preflight (spec 3E VB-11) and what a clean write logs, resolved
 * options (VB-4), legal names (VB-10), and the `.names` covers and their normalisation (VB-6). */
#include "backends/blif/vtr.h"

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "techlib/reader.h"
#include "util/log.h"
#include "util/u64map.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VTR_PREFIX ODIN3_BLIF_VTR_PREFIX

enum {
    LEGAL_FIRST = 0x21, /* VB-10 rule 1: the printable bytes, blank excluded */
    LEGAL_LAST = 0x7E,
};

/* The printable bytes VB-10 rule 1 still escapes: VPR's ID_SET excludes the first four (R1; `#`
 * for ABC), and `%` is the escape. */
static const char ESCAPED[] = "=\"#\\%";

/* VB-10 rule 3's whole names, and the prefix of VPR's outpad blocks. */
static const char *const RESERVED[] = {"re", "fe", "ah", "al",  "as",  "NIL",   "0",
                                       "1",  "2",  "3",  "gnd", "vcc", "unconn"};
static const char OUTPAD_PREFIX[] = "out:";

/* Truncates out back to len, undoing a failed sequence of appends (the documented "unchanged"). */
static void truncate_to(odin3_strbuf *out, size_t len) {
    if (out->data != NULL) {
        out->len = len;
        out->data[len] = '\0';
    }
}

static bool byte_legal(unsigned char byte) {
    return byte >= LEGAL_FIRST && byte <= LEGAL_LAST &&
           memchr(ESCAPED, byte, sizeof ESCAPED - 1) == NULL;
}

bool odin3_blif_vtr_reserved(odin3_bytes name) {
    size_t prefix = sizeof OUTPAD_PREFIX - 1;
    if (name.len >= prefix && memcmp(name.ptr, OUTPAD_PREFIX, prefix) == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++) {
        if (name.len == strlen(RESERVED[i]) && memcmp(name.ptr, RESERVED[i], name.len) == 0) {
            return true;
        }
    }
    return false;
}

odin3_status odin3_blif_vtr_legal_bytes(odin3_bytes name, odin3_strbuf *out) {
    size_t start = out->len;
    const unsigned char *bytes = name.ptr;
    odin3_status st = ODIN3_OK;
    size_t run = 0; /* start of the pending run of kept bytes */
    for (size_t i = 0; st == ODIN3_OK && i < name.len; i++) {
        if (byte_legal(bytes[i]) && !(i == 0 && bytes[i] == '.')) {
            continue;
        }
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, i - run});
        if (st == ODIN3_OK) {
            st = odin3_strbuf_appendf(out, "%%%02X", (unsigned)bytes[i]);
        }
        run = i + 1;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, name.len - run});
    }
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out) {
    size_t start = out->len;
    odin3_status st =
        odin3_blif_vtr_reserved(name) ? odin3_strbuf_append(out, odin3_bytes_cstr("%")) : ODIN3_OK;
    st = st == ODIN3_OK ? odin3_blif_vtr_legal_bytes(name, out) : st;
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

/* True when byte `at` of text is the `^` of a `_^_`. */
static bool clock_caret(const unsigned char *text, size_t len, size_t at) {
    return text[at] == '^' && at > 0 && at + 1 < len && text[at - 1] == '_' && text[at + 1] == '_';
}

odin3_status odin3_blif_vtr_clock_name(odin3_bytes legal, odin3_strbuf *out) {
    if (legal.len == 0) {
        return ODIN3_OK; /* an empty name escapes to nothing; `legal.ptr` may be NULL */
    }
    size_t start = out->len;
    const unsigned char *bytes = legal.ptr;
    odin3_status st = ODIN3_OK;
    size_t run = 0;
    for (size_t i = 0; st == ODIN3_OK && i < legal.len; i++) {
        if (bytes[i] != '|' && !clock_caret(bytes, legal.len, i)) {
            continue;
        }
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, i - run});
        if (st == ODIN3_OK) {
            st = odin3_strbuf_appendf(out, "%%%02X", (unsigned)bytes[i]);
        }
        run = i + 1;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, legal.len - run});
    }
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts) {
    return opts->lut_k == 0 ? ODIN3_BLIF_VTR_DEFAULT_K : opts->lut_k;
}

/* The options' own checks, for a direct caller of the preflight: through odin3_blif_write_with
 * they never fire, since its check_opts refuses the same options first. */
static odin3_status check_opts(const odin3_blif_write_opts *opts) {
    uint32_t k = odin3_blif_vtr_k(opts);
    if (opts->dialect != ODIN3_BLIF_DIALECT_VTR) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: opts are not the VTR dialect");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (k < ODIN3_BLIF_VTR_MIN_K || k > ODIN3_BLIF_VTR_MAX_K) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: lut_k %u is outside %u..%u",
                  (unsigned)k, ODIN3_BLIF_VTR_MIN_K, ODIN3_BLIF_VTR_MAX_K);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (opts->name_style != ODIN3_BLIF_NAMES_SHORT &&
        opts->name_style != ODIN3_BLIF_NAMES_PROVENANCE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: name_style %d is unknown",
                  (int)opts->name_style);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

enum { SOP_COVER_PARAM = 1 }; /* $sop parameters WIDTH, COVER (ir/cells/sop.c) */

/* --- source classes (spec 3E §9.1) -------------------------------------------------------- */

/* The registry names the preflight and the classes tell apart. */
static const char SOP_NAME[] = "$sop";
static const char LUT_NAME[] = "$lut";
static const char UNCONN_NAME[] = "unconn";

/* True when name is one of the n names of list. */
static bool name_in(const char *name, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const CONST_CELLS[] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};

/* The name of a module's net (its own name; strtab text), "" when it has none. */
static const char *own_net_name(const odin3_module *module, odin3_net_id net) {
    uint32_t name = odin3_net_name(module, net);
    return name == 0 ? ""
                     : odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), name);
}

odin3_blif_src_class odin3_blif_vtr_net_class(const odin3_module *module, odin3_net_id net) {
    odin3_pin_id driver = odin3_net_valid(net) ? odin3_net_driver(module, net) : (odin3_pin_id){0};
    if (!odin3_pin_valid(driver)) {
        return ODIN3_BLIF_SRC_UNDRIVEN;
    }
    odin3_node_id node = odin3_pin_node(module, driver);
    const char *type =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name;
    bool sop = strcmp(type, SOP_NAME) == 0;
    if (!sop && !name_in(type, CONST_CELLS, sizeof CONST_CELLS / sizeof CONST_CELLS[0])) {
        return ODIN3_BLIF_SRC_SIGNAL; /* only the driving cell's own function counts */
    }
    switch (odin3_net_const_value(module, net)) {
    case ODIN3_CONST_X:
    case ODIN3_CONST_Z:
        return ODIN3_BLIF_SRC_X;
    case ODIN3_CONST_0:
        return sop && odin3_node_param(module, node, SOP_COVER_PARAM)->len == 0 &&
                       strcmp(own_net_name(module, net), UNCONN_NAME) == 0
                   ? ODIN3_BLIF_SRC_U
                   : ODIN3_BLIF_SRC_0;
    case ODIN3_CONST_1:
        return ODIN3_BLIF_SRC_1;
    default:
        return ODIN3_BLIF_SRC_SIGNAL;
    }
}

/* --- .names cells: covers, truth tables, §6's normalisation ------------------------------- */

/* §6's gate covers, rows without blanks (inputs + 1 bytes each). */
typedef struct vtr_gate_rows {
    const char *name;
    const char *rows;
    uint32_t inputs;
} vtr_gate_rows;

static const vtr_gate_rows GATE_ROWS[] = {
    {"$_BUF_", "11", 1},    {"$_NOT_", "01", 1},      {"$_AND_", "111", 2},
    {"$_OR_", "1-1-11", 2}, {"$_XOR_", "101011", 2},  {"$_NAND_", "0-1-01", 2},
    {"$_NOR_", "001", 2},   {"$_XNOR_", "111001", 2}, {"$_MUX_", "1-01-111", 3},
};

enum {
    TT_WORD_BITS = 64,
    TT_WORD_SHIFT = 6, /* log2(TT_WORD_BITS) */
    BUF_TABLE = 2,     /* a 1-input table f(0) = 0, f(1) = 1: bits 0b10 */
    ONE_INPUT_MASK = 3,
};

bool odin3_blif_vtr_rows_of(const odin3_module *module, odin3_node_id node,
                            odin3_blif_vtr_rows *rows) {
    const char *type =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name;
    if (strcmp(type, SOP_NAME) == 0) {
        const odin3_value *cover = odin3_node_param(module, node, SOP_COVER_PARAM);
        *rows = (odin3_blif_vtr_rows){cover->bits, cover->len, cover->cover_inputs};
        return true;
    }
    for (size_t i = 0; i < sizeof GATE_ROWS / sizeof GATE_ROWS[0]; i++) {
        if (strcmp(type, GATE_ROWS[i].name) == 0) {
            *rows = (odin3_blif_vtr_rows){(const uint8_t *)GATE_ROWS[i].rows,
                                          (uint32_t)strlen(GATE_ROWS[i].rows), GATE_ROWS[i].inputs};
            return true;
        }
    }
    return false;
}

/* Words of a truth table of `inputs` inputs. */
static uint32_t tt_words(uint32_t inputs) {
    uint64_t bits = (uint64_t)1 << inputs;
    return bits <= TT_WORD_BITS ? 1U : (uint32_t)(bits >> TT_WORD_SHIFT);
}

/* The mask of the valid bits of each word of a table of `inputs` inputs. */
static uint64_t tt_mask(uint32_t inputs) {
    return inputs >= TT_WORD_SHIFT ? UINT64_MAX : ((uint64_t)1 << ((uint64_t)1 << inputs)) - 1U;
}

/* Sets in tt the minterms the row (`inputs` plane characters) covers: value with each subset of
 * the don't-care inputs, in increasing order, rather than a scan of all 2^k minterms per row (3E-1
 * final review M9). */
static void tt_add_row(const uint8_t *row, uint32_t inputs, uint64_t *tt) {
    uint32_t care = 0;
    uint32_t value = 0;
    for (uint32_t j = 0; j < inputs; j++) {
        care |= row[j] != '-' ? 1U << j : 0U;
        value |= row[j] == '1' ? 1U << j : 0U;
    }
    uint32_t dont = ~care & ((1U << inputs) - 1U);
    uint32_t sub = 0;
    do {
        uint32_t minterm = value | sub;
        tt[minterm >> TT_WORD_SHIFT] |= (uint64_t)1 << (minterm & (TT_WORD_BITS - 1U));
        sub = (sub - dont) & dont; /* the next subset; 0 again after the last */
    } while (sub != 0);
}

/* tt = the function of rows (k <= 16), as odin3_blif_vtr_eval states. */
static void tt_of_rows(const odin3_blif_vtr_rows *rows, uint64_t *tt) {
    uint32_t k = rows->inputs;
    uint32_t step = k + 1U;
    bool on = false;
    for (uint32_t at = k; at < rows->len; at += step) {
        on = on || rows->bits[at] == '1';
    }
    memset(tt, 0, sizeof *tt * tt_words(k));
    for (uint32_t at = 0; at + k < rows->len; at += step) {
        if (rows->bits[at + k] == (on ? '1' : '0')) {
            tt_add_row(rows->bits + at, k, tt);
        }
    }
    for (uint32_t word = 0; !on && rows->len > 0 && word < tt_words(k); word++) {
        tt[word] = ~tt[word] & tt_mask(k); /* only off-set rows: the complement */
    }
}

/* True and params = node's WIDTH and LUT when node is a $lut of a valid WIDTH (3D AB-2). */
static bool lut_params(const odin3_module *module, odin3_node_id node, odin3_value *params) {
    const char *type =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name;
    if (strcmp(type, LUT_NAME) != 0) {
        return false;
    }
    params[ODIN3_LUT_P_WIDTH] = *odin3_node_param(module, node, ODIN3_LUT_P_WIDTH);
    params[ODIN3_LUT_P_LUT] = *odin3_node_param(module, node, ODIN3_LUT_P_LUT);
    const odin3_value *width = &params[ODIN3_LUT_P_WIDTH];
    return width->kind == ODIN3_VAL_INT && width->i >= 1 && width->i <= ODIN3_LUT_MAX_WIDTH;
}

/* True and *inputs = the inputs of a `.names` cell (odin3_blif_vtr_rows_of) or of a $lut. */
static bool names_inputs(const odin3_module *module, odin3_node_id node, uint32_t *inputs) {
    odin3_blif_vtr_rows rows;
    odin3_value params[2];
    if (odin3_blif_vtr_rows_of(module, node, &rows)) {
        *inputs = rows.inputs;
        return true;
    }
    if (lut_params(module, node, params)) {
        *inputs = (uint32_t)params[ODIN3_LUT_P_WIDTH].i;
        return true;
    }
    return false;
}

bool odin3_blif_vtr_eval(const odin3_module *module, odin3_node_id node, uint64_t *tt) {
    odin3_blif_vtr_rows rows;
    odin3_value params[2];
    if (lut_params(module, node, params)) { /* the mask is the table (3E §18, M9) */
        return odin3_lut_table(params, tt, odin3_lut_words((uint32_t)params[ODIN3_LUT_P_WIDTH].i));
    }
    if (!odin3_blif_vtr_rows_of(module, node, &rows) || rows.inputs > ODIN3_BLIF_VTR_MAX_K) {
        return false;
    }
    tt_of_rows(&rows, tt);
    return true;
}

odin3_blif_vtr_fn odin3_blif_vtr_function(const odin3_module *module, odin3_node_id node) {
    uint64_t tt[ODIN3_BLIF_VTR_TT_WORDS];
    uint32_t k = 0;
    if (!names_inputs(module, node, &k) || !odin3_blif_vtr_eval(module, node, tt)) {
        return ODIN3_BLIF_VTR_FN_NONE;
    }
    bool zero = true;
    bool one = true;
    for (uint32_t word = 0; word < tt_words(k); word++) {
        zero = zero && tt[word] == 0;
        one = one && tt[word] == tt_mask(k);
    }
    if (zero || one) {
        return zero ? ODIN3_BLIF_VTR_FN_CONST0 : ODIN3_BLIF_VTR_FN_CONST1;
    }
    if (k == 1) {
        return (tt[0] & ONE_INPUT_MASK) == BUF_TABLE ? ODIN3_BLIF_VTR_FN_BUF
                                                     : ODIN3_BLIF_VTR_FN_INV;
    }
    return ODIN3_BLIF_VTR_FN_COVER;
}

bool odin3_blif_vtr_const_cell(const odin3_module *module, odin3_node_id node) {
    const char *type =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name;
    return name_in(type, CONST_CELLS, sizeof CONST_CELLS / sizeof CONST_CELLS[0]);
}

/* --- the walk ------------------------------------------------------------------------------ */

/* The cells the preflight tells apart by registry name (everything else is VK_OTHER). */
typedef enum vtr_kind {
    VK_OTHER,
    VK_PORT_IN,
    VK_PORT_OUT,
    VK_PORT_INOUT,
    VK_CONST,
    VK_SOP,
    VK_GATE,
    VK_LUT,
    VK_DFF_P,
    VK_DFF_N,
    VK_DLATCH,
    VK_FF
} vtr_kind;

typedef struct vtr_named_kind {
    const char *name;
    vtr_kind kind;
} vtr_named_kind;

static const vtr_named_kind KINDS[] = {
    {"$port_in", VK_PORT_IN}, {"$port_out", VK_PORT_OUT}, {"$port_inout", VK_PORT_INOUT},
    {"$_CONST0_", VK_CONST},  {"$_CONST1_", VK_CONST},    {"$_CONSTX_", VK_CONST},
    {"$_CONSTZ_", VK_CONST},  {"$sop", VK_SOP},           {"$_BUF_", VK_GATE},
    {"$_NOT_", VK_GATE},      {"$_AND_", VK_GATE},        {"$_OR_", VK_GATE},
    {"$_XOR_", VK_GATE},      {"$_NAND_", VK_GATE},       {"$_NOR_", VK_GATE},
    {"$_XNOR_", VK_GATE},     {"$_MUX_", VK_GATE},        {"$_DFF_P_", VK_DFF_P},
    {"$_DFF_N_", VK_DFF_N},   {"$_DLATCH_P_", VK_DLATCH}, {"$_DLATCH_N_", VK_DLATCH},
    {"$_FF_", VK_FF},         {"$lut", VK_LUT},
};

static vtr_kind kind_of(const char *name) {
    for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        if (strcmp(name, KINDS[i].name) == 0) {
            return KINDS[i].kind;
        }
    }
    return VK_OTHER;
}

/* Plan 3E point (e): E1b by name. The word flip-flops with an asynchronous input (2E IR change
 * D's included, so the rule holds before and after it) and the gate-level prefixes. */
static const char *const ASYNC_WORD_FFS[] = {"$adff",   "$adffe", "$aldff",
                                             "$aldffe", "$dffsr", "$dffsre"};
static const char *const ASYNC_PREFIXES[] = {"$_DFFSR_", "$_DFFSRE_", "$_SR_", "$_ALDFF_",
                                             "$_ALDFFE_"};

/* True when name is prefix, then one byte of each of the n sets, then `_` and the end. */
static bool match_sets(const char *name, const char *prefix, const char *const *sets, size_t n) {
    size_t len = strlen(prefix);
    if (strncmp(name, prefix, len) != 0 || strlen(name) != len + n + 1 || name[len + n] != '_') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (name[len + i] == '\0' || strchr(sets[i], name[len + i]) == NULL) {
            return false;
        }
    }
    return true;
}

static bool async_ff(const char *name) {
    static const char *const dff_sets[] = {"NP", "NP", "01"};        /* $_DFF_[NP][NP][01]_ */
    static const char *const dffe_sets[] = {"NP", "NP", "01", "NP"}; /* $_DFFE_[NP][NP][01][NP]_ */
    for (size_t i = 0; i < sizeof ASYNC_PREFIXES / sizeof ASYNC_PREFIXES[0]; i++) {
        if (strncmp(name, ASYNC_PREFIXES[i], strlen(ASYNC_PREFIXES[i])) == 0) {
            return true;
        }
    }
    return name_in(name, ASYNC_WORD_FFS, sizeof ASYNC_WORD_FFS / sizeof ASYNC_WORD_FFS[0]) ||
           match_sets(name, "$_DFF_", dff_sets, sizeof dff_sets / sizeof dff_sets[0]) ||
           match_sets(name, "$_DFFE_", dffe_sets, sizeof dffe_sets / sizeof dffe_sets[0]);
}

/* §11's texts after `<loc>: <type> '<cell>': `. */
static const char E1B_TEXT[] =
    "asynchronous set/reset flip-flop; VPR has none (lower makes it synchronous, decision 19)";
static const char E1_TEXT[] = "falling-edge flip-flop; VPR accepts only rising-edge latches "
                              "(lower inverts the clock)";
static const char E2_TEXT[] = "level-sensitive latch; VPR accepts only rising-edge latches";
static const char E3_TEXT[] = "flip-flop without a clock; VPR requires a clock on every latch";
static const char E4_TEXT[] = "tri-state cell; VPR has no tri-state";
static const char E5_TEXT[] = "not a netlist cell (run lower)";
static const char E8_TEXT[] = "cover mixes on-set and off-set rows";

/* `, …` after the second driver of E12 (plan 3E point (h)). */
static const char MORE_DRIVERS[] = ", \xE2\x80\xA6";

enum {
    LATCH_C = 0, /* $_DFF_P_ ports C D Q */
    LATCH_D = 1,
    SHOWN_DRIVERS = 2,     /* E12 names at most two drivers */
    DRIVER_KEY_SHIFT = 32, /* next_driver's key: node ID << 32 | pin ID */
};

/* The walk over the top: its state, the text being built, the plan and the report it fills. */
typedef struct vtr_walk {
    const odin3_design *design;
    const odin3_module *top;
    const odin3_strtab *tab;
    uint32_t k;
    odin3_blif_vtr_plan *plan;
    odin3_blif_vtr_report *report;
    odin3_strbuf head; /* `<loc>: <type> '<cell>'`, or another line's start */
    odin3_strbuf text; /* E11's list, E7's text */
} vtr_walk;

/* Counts one refusal; true when it is logged (the cap of ODIN3_BLIF_VTR_MAX_ERRORS not passed). */
static bool refusal_logged(vtr_walk *walk) {
    return ++walk->report->errors <= ODIN3_BLIF_VTR_MAX_ERRORS;
}

static void first_source(void *user, odin3_prov_id leaf) {
    odin3_prov_id *first = user;
    if (first->v == 0) {
        *first = leaf;
    }
}

/* Appends `<loc>` (plan 3E point (i)): `<file as given>:<line>` of the first SOURCE or IMPORTED
 * record odin3_prov_sources yields for prov, else the top's name. */
static odin3_status append_loc(vtr_walk *walk, odin3_prov_id prov) {
    const odin3_prov_record *rec = odin3_prov_get(walk->design, prov);
    if (rec != NULL && rec->kind == ODIN3_PROV_DERIVED) {
        odin3_prov_id first = {0};
        odin3_status st = odin3_prov_sources(walk->design, prov, first_source, &first);
        if (st != ODIN3_OK) {
            return st;
        }
        rec = odin3_prov_get(walk->design, first);
    }
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        return odin3_strbuf_appendf(&walk->head, "%s:%u",
                                    odin3_strtab_get(walk->tab, rec->locs[0].file),
                                    (unsigned)rec->locs[0].line);
    }
    return odin3_strbuf_append(
        &walk->head, odin3_bytes_cstr(odin3_strtab_get(walk->tab, odin3_module_name(walk->top))));
}

/* Appends `<cell>`: the node's name, else `$c<ID>`. */
static odin3_status append_cell(vtr_walk *walk, odin3_node_id node) {
    uint32_t name = odin3_node_name(walk->top, node);
    return name != 0 ? odin3_strbuf_append(&walk->head,
                                           odin3_bytes_cstr(odin3_strtab_get(walk->tab, name)))
                     : odin3_strbuf_appendf(&walk->head, "$c%u", (unsigned)node.v);
}

static const char *type_name(const vtr_walk *walk, odin3_node_id node) {
    return odin3_celltype_get(walk->design, odin3_node_type(walk->top, node))->name;
}

/* walk->head = `<loc>: <type> '<cell>'`. */
static odin3_status cell_head(vtr_walk *walk, odin3_node_id node) {
    odin3_strbuf_clear(&walk->head);
    odin3_status st = append_loc(walk, odin3_node_prov(walk->top, node));
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&walk->head, ": %s '", type_name(walk, node)) : st;
    st = st == ODIN3_OK ? append_cell(walk, node) : st;
    return st == ODIN3_OK ? odin3_strbuf_append(&walk->head, odin3_bytes_cstr("'")) : st;
}

/* Logs `<loc>: <type> '<cell>': <text>`. */
static odin3_status log_cell(vtr_walk *walk, odin3_node_id node, const char *text) {
    odin3_status st = cell_head(walk, node);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, VTR_PREFIX "%s: %s", walk->head.data, text);
    }
    return st;
}

/* One refusal of node with text (logged below the cap). */
static odin3_status refuse_cell(vtr_walk *walk, odin3_node_id node, const char *text) {
    return refusal_logged(walk) ? log_cell(walk, node, text) : ODIN3_OK;
}

/* The class of the first pin of port `port` of node. */
static odin3_blif_src_class pin_class(const vtr_walk *walk, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(walk->top, node, port);
    return odin3_blif_vtr_net_class(walk->top, odin3_pin_net(walk->top, pins.first));
}

static bool is_x(odin3_blif_src_class cls) {
    return cls == ODIN3_BLIF_SRC_X || cls == ODIN3_BLIF_SRC_UNDRIVEN;
}

/* A clock net VPR cannot use: x, undriven or `unconn` (class U). A constant clock is accepted (a
 * warning); an `unconn` one is read by VPR but aborts its full flow (`clk_pin` assertion, 3E-4). */
static bool is_open_clock(odin3_blif_src_class cls) {
    return is_x(cls) || cls == ODIN3_BLIF_SRC_U;
}

/* Counts a logic input (a .names input or a .latch D) of class cls (§9.2). */
static void count_logic_pin(vtr_walk *walk, odin3_blif_src_class cls) {
    if (is_x(cls)) {
        walk->report->x_to_gnd++;
    } else if (cls == ODIN3_BLIF_SRC_U) {
        walk->report->unconn_logic++;
    }
}

static odin3_status refuse_clock(vtr_walk *walk, odin3_node_id node, uint32_t port);

/* E3 for a C that is unconnected, undriven or x; E3b for a C on `unconn`; else one .latch. */
static odin3_status check_latch(vtr_walk *walk, odin3_node_id node) {
    odin3_blif_src_class clock = pin_class(walk, node, LATCH_C);
    if (is_x(clock)) {
        return refuse_cell(walk, node, E3_TEXT);
    }
    if (is_open_clock(clock)) {
        return refuse_clock(walk, node, LATCH_C);
    }
    walk->report->latches++;
    count_logic_pin(walk, pin_class(walk, node, LATCH_D));
    return ODIN3_OK;
}

/* True when the cover has rows with output `0` and rows with output `1`. */
static bool cover_mixed(const odin3_value *cover) {
    bool on = false;
    bool off = false;
    for (uint32_t at = cover->cover_inputs; at < cover->len; at += cover->cover_inputs + 1) {
        on = on || cover->bits[at] == '1';
        off = off || cover->bits[at] == '0';
    }
    return on && off;
}

/* E7 (its stored inputs, plan 3E point (g)), then E8 for a $sop; else one .names (a $lut's from
 * odin3_blif_isop, 3E-5). */
static odin3_status check_names(vtr_walk *walk, odin3_node_id node, vtr_kind kind) {
    odin3_pinslice pins = odin3_node_pins(walk->top, node);
    uint32_t inputs = 0;
    for (uint32_t k = 0; k < pins.count; k++) {
        if (odin3_pin_dir(walk->top, (odin3_pin_id){pins.first.v + k}) == ODIN3_DIR_IN) {
            inputs++;
        }
    }
    if (inputs > walk->k) {
        if (!refusal_logged(walk)) {
            return ODIN3_OK;
        }
        odin3_strbuf_clear(&walk->text);
        odin3_status st =
            odin3_strbuf_appendf(&walk->text, "%u inputs exceed K=%u (run abc, or pass --lut-k)",
                                 (unsigned)inputs, (unsigned)walk->k);
        return st == ODIN3_OK ? log_cell(walk, node, walk->text.data) : st;
    }
    if (kind == VK_SOP && cover_mixed(odin3_node_param(walk->top, node, SOP_COVER_PARAM))) {
        return refuse_cell(walk, node, E8_TEXT);
    }
    walk->report->names++;
    odin3_blif_vtr_fn fn = odin3_blif_vtr_function(walk->top, node);
    if (fn == ODIN3_BLIF_VTR_FN_CONST0 || fn == ODIN3_BLIF_VTR_FN_CONST1) {
        return ODIN3_OK; /* written as a zero-input generator: its inputs are dropped (§6) */
    }
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (odin3_pin_dir(walk->top, pin) == ODIN3_DIR_IN) {
            count_logic_pin(walk,
                            odin3_blif_vtr_net_class(walk->top, odin3_pin_net(walk->top, pin)));
        }
    }
    return ODIN3_OK;
}

/* The name of the top's port whose node is node ("" when none). */
static const char *port_name_of(const vtr_walk *walk, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(walk->design, odin3_module_celltype(walk->top));
    for (uint32_t i = 0; i < odin3_module_port_count(walk->top); i++) {
        if (odin3_module_port(walk->top, i).v == node.v) {
            return def->ports[i].name;
        }
    }
    return "";
}

/* E9: `<module>: port '<p>': inout port; VPR has no bidirectional pins`. */
static odin3_status refuse_inout(vtr_walk *walk, odin3_node_id node) {
    if (refusal_logged(walk)) {
        odin3_log(
            ODIN3_LOG_ERROR, VTR_PREFIX "%s: port '%s': inout port; VPR has no bidirectional pins",
            odin3_strtab_get(walk->tab, odin3_module_name(walk->top)), port_name_of(walk, node));
    }
    return ODIN3_OK;
}

/* Output-port bits: X and UNDRIVEN are written from gnd (§9.2). */
static void count_port_out(vtr_walk *walk, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(walk->top, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(walk->top, (odin3_pin_id){pins.first.v + k});
        if (is_x(odin3_blif_vtr_net_class(walk->top, net))) {
            walk->report->x_to_gnd++;
        }
    }
}

/* E11: `… parameters <list> cannot be recovered from BLIF formals` (plan 3E point (w)); *refused
 * when it applies. */
static odin3_status check_params(vtr_walk *walk, odin3_node_id node, bool *refused) {
    bool ok = true;
    odin3_strbuf_clear(&walk->text);
    odin3_status st = odin3_blif_params_check(walk->design, walk->top, node, &walk->text, &ok);
    if (st == ODIN3_ERR_INVALID_ARG) { /* the type cannot infer them (the IR logged why) */
        odin3_strbuf_clear(&walk->text);
        st = odin3_strbuf_append(&walk->text, odin3_bytes_cstr("of the type"));
        ok = false;
    }
    *refused = st == ODIN3_OK && !ok;
    if (!*refused || !refusal_logged(walk)) {
        return st;
    }
    st = cell_head(walk, node);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR,
                  VTR_PREFIX "%s: parameters %s cannot be recovered from BLIF formals",
                  walk->head.data, walk->text.data);
    }
    return st;
}

/* True when a pin of port `port` of node has class X or UNDRIVEN (also U when `clock`); *count adds
 * each such pin. */
static bool port_has_x(const vtr_walk *walk, odin3_node_id node, uint32_t port, bool clock,
                       uint32_t *count) {
    odin3_pinslice pins = odin3_node_port(walk->top, node, port);
    bool any = false;
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(walk->top, (odin3_pin_id){pins.first.v + k});
        odin3_blif_src_class cls = odin3_blif_vtr_net_class(walk->top, net);
        if (clock ? is_open_clock(cls) : is_x(cls)) {
            any = true;
            (*count)++;
        }
    }
    return any;
}

/* E3b for port `port` of node (logged below the cap). */
static odin3_status refuse_clock(vtr_walk *walk, odin3_node_id node, uint32_t port) {
    if (!refusal_logged(walk)) {
        return ODIN3_OK;
    }
    const odin3_celltype_def *def =
        odin3_celltype_get(walk->design, odin3_node_type(walk->top, node));
    odin3_strbuf_clear(&walk->text);
    odin3_status st = odin3_strbuf_appendf(&walk->text, "clock input '%s' is unconnected or x",
                                           def->ports[port].name);
    return st == ODIN3_OK ? log_cell(walk, node, walk->text.data) : st;
}

/* E3b for a clock input (§8.2) that is unconnected, undriven, x or `unconn` (3E-4: VPR's full flow
 * aborts on an `unconn` clock); *refused when it applies. */
static odin3_status check_clock(vtr_walk *walk, odin3_node_id node, uint32_t port, bool *refused) {
    uint32_t bad = 0;
    if (!port_has_x(walk, node, port, true, &bad)) {
        return ODIN3_OK;
    }
    *refused = true;
    return refuse_clock(walk, node, port);
}

/* A hard cell of a loaded library: E11, then E3b per clock input; else one .subckt, whose x,
 * undriven and unconnected non-clock inputs are written as `unconn`. */
static odin3_status check_hard(vtr_walk *walk, odin3_node_id node, const odin3_techlib_cell *lib) {
    const odin3_celltype_def *def =
        odin3_celltype_get(walk->design, odin3_node_type(walk->top, node));
    assert(def->n_ports == lib->n_ports); /* the library registers its ports in order (1G) */
    bool refused = false;
    odin3_status st = check_params(walk, node, &refused);
    uint32_t x_inputs = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports && i < lib->n_ports; i++) {
        if (def->ports[i].dir != ODIN3_DIR_IN) {
            continue;
        }
        if (lib->ports[i].clock) {
            st = check_clock(walk, node, i, &refused);
        } else {
            (void)port_has_x(walk, node, i, false, &x_inputs);
        }
    }
    if (st == ODIN3_OK && !refused) {
        walk->report->subckts++;
        walk->report->x_to_unconn += x_inputs;
    }
    return st;
}

/* E10: `<loc>: instance '<cell>' of module '<m>': hierarchy not flattened …`. */
static odin3_status refuse_instance(vtr_walk *walk, odin3_node_id node) {
    if (!refusal_logged(walk)) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(&walk->head);
    odin3_status st = append_loc(walk, odin3_node_prov(walk->top, node));
    st = st == ODIN3_OK ? odin3_strbuf_append(&walk->head, odin3_bytes_cstr(": instance '")) : st;
    st = st == ODIN3_OK ? append_cell(walk, node) : st;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR,
                  VTR_PREFIX "%s' of module '%s': hierarchy not flattened (run flatten); VPR reads "
                             "one model",
                  walk->head.data, type_name(walk, node));
    }
    return st;
}

/* A cell that is no built-in netlist cell: E4, E10, a hard cell, E6 or E5 (point (f)). */
static odin3_status check_other(vtr_walk *walk, odin3_node_id node, const odin3_celltype_def *def) {
    const odin3_techlib_cell *lib =
        odin3_celltype_lib(walk->design, odin3_node_type(walk->top, node));
    if ((def->flags & ODIN3_CT_TRISTATE) != 0) {
        return refuse_cell(walk, node, E4_TEXT);
    }
    if (def->gran == ODIN3_GRAN_MODULE) {
        return refuse_instance(walk, node);
    }
    /* A library black box is an arch model with no function (4.2: the generic blocks `read_arch`
     * registers); it is written as a `.subckt` like a hard cell. */
    if (odin3_techlib_is_subckt(lib)) {
        return check_hard(walk, node, lib);
    }
    if (lib != NULL || def->gran == ODIN3_GRAN_HARD || def->gran == ODIN3_GRAN_BLACKBOX) {
        if (!refusal_logged(walk)) {
            return ODIN3_OK;
        }
        odin3_strbuf_clear(&walk->text);
        odin3_status st = odin3_strbuf_appendf(
            &walk->text,
            "'%s' is not a hard cell of a loaded tech library (read_techlib lib/vtr.o3lib)",
            def->name);
        return st == ODIN3_OK ? log_cell(walk, node, walk->text.data) : st;
    }
    return refuse_cell(walk, node, E5_TEXT);
}

/* One node of the top, with at most one refusal of plan 3E point (f)'s precedence (and E3b per
 * clock input of a hard cell). */
static odin3_status check_node(vtr_walk *walk, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(walk->design, odin3_node_type(walk->top, node));
    vtr_kind kind = kind_of(def->name);
    if (kind == VK_PORT_OUT) {
        count_port_out(walk, node);
        return ODIN3_OK;
    }
    if (kind == VK_PORT_INOUT) {
        return refuse_inout(walk, node);
    }
    if (kind == VK_PORT_IN || kind == VK_CONST) {
        return ODIN3_OK;
    }
    if (async_ff(def->name)) {
        return refuse_cell(walk, node, E1B_TEXT);
    }
    switch (kind) {
    case VK_DFF_N:
        return refuse_cell(walk, node, E1_TEXT);
    case VK_DLATCH:
        return refuse_cell(walk, node, E2_TEXT);
    case VK_FF:
        return refuse_cell(walk, node, E3_TEXT);
    case VK_DFF_P:
        return check_latch(walk, node);
    case VK_SOP:
    case VK_GATE:
    case VK_LUT:
        return check_names(walk, node, kind);
    default:
        return check_other(walk, node, def);
    }
}

/* --- nets: E12 and the classes ------------------------------------------------------------- */

/* Appends a net's name for E12: its own, else its primary wire bit's (`w[i]`, `w` for a 1-bit
 * wire), else `$n<ID>`. */
static odin3_status append_net(vtr_walk *walk, odin3_net_id net) {
    uint32_t name = odin3_net_name(walk->top, net);
    odin3_wirebit bit = odin3_net_primary(walk->top, net);
    if (name != 0) {
        return odin3_strbuf_append(&walk->head,
                                   odin3_bytes_cstr(odin3_strtab_get(walk->tab, name)));
    }
    if (!odin3_wire_valid(bit.wire) || odin3_wire_name(walk->top, bit.wire) == 0) {
        return odin3_strbuf_appendf(&walk->head, "$n%u", (unsigned)net.v);
    }
    const char *wire = odin3_strtab_get(walk->tab, odin3_wire_name(walk->top, bit.wire));
    return odin3_wire_width(walk->top, bit.wire) == 1
               ? odin3_strbuf_append(&walk->head, odin3_bytes_cstr(wire))
               : odin3_strbuf_appendf(&walk->head, "%s[%d]", wire,
                                      (int)odin3_wire_index(walk->top, bit.wire, bit.bit));
}

/* Appends a driver's name for E12: a $port_in's port, else the cell (`<cell>`). */
static odin3_status append_driver(vtr_walk *walk, odin3_node_id node) {
    if (odin3_celltype_get(walk->design, odin3_node_type(walk->top, node))->gran ==
        ODIN3_GRAN_PORT) {
        return odin3_strbuf_append(&walk->head, odin3_bytes_cstr(port_name_of(walk, node)));
    }
    return append_cell(walk, node);
}

/* True for an OUT pin. E12 counts only those: an INOUT pin (an implicit black box's, an inout
 * port's) is on a node refused by E6 or E9 already. */
static bool drives_out(const vtr_walk *walk, odin3_pin_id pin) {
    return odin3_pin_dir(walk->top, pin) == ODIN3_DIR_OUT;
}

/* The number of OUT pins on net. */
static uint32_t out_drivers(const vtr_walk *walk, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(walk->top, net);
    uint32_t count = 0;
    for (uint32_t i = 0; i < pins.count; i++) {
        if (drives_out(walk, pins.pins[i])) {
            count++;
        }
    }
    return count;
}

/* The lowest key node ID << 32 | pin ID above `above` of an OUT pin on net (0 for none): the
 * driver pins in (node ID, pin ID) order, so a cell with two pins on net is listed twice. */
static uint64_t next_driver(const vtr_walk *walk, odin3_net_id net, uint64_t above) {
    odin3_pinlist pins = odin3_net_pins(walk->top, net);
    uint64_t best = 0;
    for (uint32_t i = 0; i < pins.count; i++) {
        uint64_t key = (uint64_t)odin3_pin_node(walk->top, pins.pins[i]).v << DRIVER_KEY_SHIFT |
                       pins.pins[i].v;
        if (drives_out(walk, pins.pins[i]) && key > above && (best == 0 || key < best)) {
            best = key;
        }
    }
    return best;
}

/* Appends E12's first SHOWN_DRIVERS driver names, each in `'…'`, joined by `, `. */
static odin3_status append_drivers(vtr_walk *walk, odin3_net_id net) {
    odin3_status st = ODIN3_OK;
    uint64_t key = next_driver(walk, net, 0); /* the net has > 1 OUT pins */
    for (uint32_t i = 0; st == ODIN3_OK && key != 0 && i < SHOWN_DRIVERS; i++) {
        st = odin3_strbuf_append(&walk->head, odin3_bytes_cstr(i == 0 ? "'" : ", '"));
        st = st == ODIN3_OK
                 ? append_driver(walk, (odin3_node_id){(uint32_t)(key >> DRIVER_KEY_SHIFT)})
                 : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(&walk->head, odin3_bytes_cstr("'")) : st;
        key = next_driver(walk, net, key);
    }
    return st;
}

/* E12: `<module>: net '<net>' has <N> drivers ('<a>', '<b>'[, …])` (plan 3E point (h)). */
static odin3_status refuse_drivers(vtr_walk *walk, odin3_net_id net, uint32_t drivers) {
    if (!refusal_logged(walk)) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(&walk->head);
    odin3_status st = odin3_strbuf_appendf(
        &walk->head, "%s: net '", odin3_strtab_get(walk->tab, odin3_module_name(walk->top)));
    st = st == ODIN3_OK ? append_net(walk, net) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&walk->head, "' has %u drivers (", (unsigned)drivers)
                        : st;
    st = st == ODIN3_OK ? append_drivers(walk, net) : st;
    if (st == ODIN3_OK && drivers > SHOWN_DRIVERS) {
        st = odin3_strbuf_append(&walk->head, odin3_bytes_cstr(MORE_DRIVERS));
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, VTR_PREFIX "%s)", walk->head.data);
    }
    return st;
}

/* One net: its class (kept when not SIGNAL), then E12. */
static odin3_status check_net(vtr_walk *walk, odin3_net_id net) {
    odin3_blif_src_class cls = odin3_blif_vtr_net_class(walk->top, net);
    odin3_status st = cls == ODIN3_BLIF_SRC_SIGNAL
                          ? ODIN3_OK
                          : odin3_u64map_put(walk->plan->classes, (odin3_kv){net.v, (uint64_t)cls});
    uint32_t drivers = odin3_net_driver_count(walk->top, net) > 1 ? out_drivers(walk, net) : 0;
    return st == ODIN3_OK && drivers > 1 ? refuse_drivers(walk, net, drivers) : st;
}

/* The nodes in ID order, then the nets (spec 3E §11). */
static odin3_status walk_top(vtr_walk *walk) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(walk->top); i++) {
        if (odin3_node_live(walk->top, (odin3_node_id){i})) {
            st = check_node(walk, (odin3_node_id){i});
        }
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_net_end(walk->top); i++) {
        if (odin3_net_live(walk->top, (odin3_net_id){i})) {
            st = check_net(walk, (odin3_net_id){i});
        }
    }
    return st;
}

/* --- what a clean write logs (plan 3E point (k)) ------------------------------------------- */

static const char NEVER_CLOCKED[] = "; VPR will never clock it";

/* The class a clock warning reads for the first pin of port `port` of node: §9.1's, except that a
 * net driven by a cell §6 writes as a zero-input generator (a $sop or gate with inputs whose
 * function is constant) counts as that constant too (§7 item 3: "a constant $sop"). */
static odin3_blif_src_class clock_class(const vtr_walk *walk, odin3_node_id node, uint32_t port) {
    odin3_blif_src_class cls = pin_class(walk, node, port);
    odin3_net_id net = odin3_pin_net(walk->top, odin3_node_port(walk->top, node, port).first);
    if (cls != ODIN3_BLIF_SRC_SIGNAL) {
        return cls;
    }
    odin3_pin_id driver = odin3_net_driver(walk->top, net); /* SIGNAL: a non-constant driver */
    odin3_blif_vtr_fn fn = odin3_blif_vtr_function(walk->top, odin3_pin_node(walk->top, driver));
    if (fn == ODIN3_BLIF_VTR_FN_CONST0 || fn == ODIN3_BLIF_VTR_FN_CONST1) {
        return fn == ODIN3_BLIF_VTR_FN_CONST1 ? ODIN3_BLIF_SRC_1 : ODIN3_BLIF_SRC_0;
    }
    return cls;
}

/* The warning for a clock pin (a $_DFF_P_'s C, or bit 0 of a hard cell's clock input `port`,
 * NULL for the latch) of class cls: a constant (§7 item 3). An `unconn` clock is E3b. */
static odin3_status warn_clock(vtr_walk *walk, odin3_node_id node, const char *port,
                               odin3_blif_src_class cls) {
    if (cls != ODIN3_BLIF_SRC_0 && cls != ODIN3_BLIF_SRC_1) {
        return ODIN3_OK;
    }
    odin3_status st = cell_head(walk, node);
    if (st != ODIN3_OK) {
        return st;
    }
    char value = cls == ODIN3_BLIF_SRC_1 ? '1' : '0';
    if (port == NULL) {
        odin3_log(ODIN3_LOG_WARN, VTR_PREFIX "%s: clock is the constant %c%s", walk->head.data,
                  value, NEVER_CLOCKED);
    } else {
        odin3_log(ODIN3_LOG_WARN, VTR_PREFIX "%s: clock input '%s' is the constant %c%s",
                  walk->head.data, port, value, NEVER_CLOCKED);
    }
    return ODIN3_OK;
}

/* The clock warnings of one node: a $_DFF_P_'s C, each clock input of a hard cell. */
static odin3_status warn_node(vtr_walk *walk, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(walk->top, node);
    const odin3_celltype_def *def = odin3_celltype_get(walk->design, type);
    if (kind_of(def->name) == VK_DFF_P) {
        return warn_clock(walk, node, NULL, clock_class(walk, node, LATCH_C));
    }
    const odin3_techlib_cell *lib = odin3_celltype_lib(walk->design, type);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0;
         odin3_techlib_is_subckt(lib) && st == ODIN3_OK && i < lib->n_ports && i < def->n_ports;
         i++) {
        if (lib->ports[i].clock && def->ports[i].dir == ODIN3_DIR_IN) {
            st = warn_clock(walk, node, def->ports[i].name, clock_class(walk, node, i));
        }
    }
    return st;
}

/* The hard types the summary line counts, in its order (spec 3E §11). */
static const char *const SUMMARY_TYPES[] = {"adder", "multiply", "single_port_ram",
                                            "dual_port_ram"};
enum { SUMMARY_TYPE_COUNT = sizeof SUMMARY_TYPES / sizeof SUMMARY_TYPES[0] };

/* counts[j] = the live hard cells of the top of type SUMMARY_TYPES[j]. */
static void count_summary_types(const vtr_walk *walk, uint32_t *counts) {
    for (uint32_t i = 1; i < odin3_module_node_end(walk->top); i++) {
        odin3_node_id node = {i};
        odin3_celltype_id type = odin3_node_type(walk->top, node);
        const odin3_techlib_cell *lib =
            odin3_node_live(walk->top, node) ? odin3_celltype_lib(walk->design, type) : NULL;
        const char *name = odin3_celltype_get(walk->design, type)->name;
        for (uint32_t j = 0;
             lib != NULL && lib->kind == ODIN3_TECHLIB_HARD && j < SUMMARY_TYPE_COUNT; j++) {
            counts[j] += strcmp(name, SUMMARY_TYPES[j]) == 0 ? 1U : 0U;
        }
    }
}

/* walk->text = ` (adder A, …)` over the live hard cells of the top (types with 0 left out), or
 * nothing when every count is 0. */
static odin3_status summary_types(vtr_walk *walk) {
    uint32_t counts[SUMMARY_TYPE_COUNT] = {0};
    count_summary_types(walk, counts);
    odin3_strbuf_clear(&walk->text);
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < SUMMARY_TYPE_COUNT; j++) {
        if (counts[j] > 0) {
            st = odin3_strbuf_appendf(&walk->text, "%s%s %u", walk->text.len == 0 ? " (" : ", ",
                                      SUMMARY_TYPES[j], (unsigned)counts[j]);
        }
    }
    return st == ODIN3_OK && walk->text.len > 0
               ? odin3_strbuf_append(&walk->text, odin3_bytes_cstr(")"))
               : st;
}

/* plan->summary = the summary info line (spec 3E §11), logged once the file is written. */
static odin3_status build_summary(vtr_walk *walk) {
    const odin3_blif_vtr_report *report = walk->report;
    odin3_status st = summary_types(walk);
    odin3_strbuf_clear(&walk->plan->summary);
    if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(
            &walk->plan->summary,
            VTR_PREFIX "'%s': %u .names, %u .latch, %u .subckt%s, K=%u; x bits: %u to gnd, %u "
                       "open (unconn)",
            odin3_strtab_get(walk->tab, odin3_module_name(walk->top)), (unsigned)report->names,
            (unsigned)report->latches, (unsigned)report->subckts,
            walk->text.len > 0 ? walk->text.data : "", (unsigned)walk->k,
            (unsigned)report->x_to_gnd, (unsigned)report->x_to_unconn);
    }
    return st;
}

/* What a write with no refusal logs (vtr.h, odin3_blif_vtr_plan_build). */
static odin3_status log_notes(vtr_walk *walk) {
    const odin3_blif_vtr_report *report = walk->report;
    const char *top = odin3_strtab_get(walk->tab, odin3_module_name(walk->top));
    if (report->modules_skipped > 0) {
        odin3_log(ODIN3_LOG_INFO, VTR_PREFIX "%u modules besides the top '%s' not written",
                  (unsigned)report->modules_skipped, top);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(walk->top); i++) {
        if (odin3_node_live(walk->top, (odin3_node_id){i})) {
            st = warn_node(walk, (odin3_node_id){i});
        }
    }
    if (st == ODIN3_OK && report->unconn_logic > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  VTR_PREFIX "net 'unconn' feeds %u .names/.latch pins; VPR leaves them open "
                             "(run abc first)",
                  (unsigned)report->unconn_logic);
    }
    return st == ODIN3_OK ? build_summary(walk) : st;
}

/* --- the plan ------------------------------------------------------------------------------ */

/* VB-10 last: its warnings and info only when nothing was refused, its refusal below the cap. */
static odin3_status name_top(const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan,
                             odin3_blif_vtr_report *report) {
    odin3_blif_names_req req = {plan->design, opts, report->errors == 0,
                                report->errors < ODIN3_BLIF_VTR_MAX_ERRORS};
    odin3_status st = odin3_blif_vtr_name_top(&req, &plan->names, &report->renamed);
    if (st == ODIN3_ERR_INVALID_ARG) {
        report->errors++;
        st = ODIN3_OK;
    }
    return st;
}

/* The walk over the top (the plan's design, top and K are set): the nodes, the nets, the names,
 * then, when nothing was refused, what a clean write logs (log_notes). */
static odin3_status walk_plan(const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan,
                              odin3_blif_vtr_report *report) {
    vtr_walk walk = {.design = plan->design,
                     .top = plan->top,
                     .tab = odin3_design_strtab(plan->design),
                     .k = plan->k,
                     .plan = plan,
                     .report = report};
    odin3_strbuf_init(&walk.head);
    odin3_strbuf_init(&walk.text);
    plan->classes = odin3_u64map_create(0);
    odin3_status st = plan->classes != NULL ? walk_top(&walk) : ODIN3_ERR_NO_MEMORY;
    bool log_oom = true;
    if (st == ODIN3_OK) {
        st = name_top(opts, plan, report);
        log_oom = false; /* naming logs its own */
    }
    if (st == ODIN3_OK && report->errors == 0) {
        st = log_notes(&walk);
        log_oom = true;
    }
    odin3_strbuf_free(&walk.head);
    odin3_strbuf_free(&walk.text);
    if (st == ODIN3_ERR_NO_MEMORY && log_oom) {
        odin3_log(ODIN3_LOG_ERROR, VTR_PREFIX "out of memory");
    }
    return st;
}

odin3_status odin3_blif_vtr_plan_build(const odin3_design *design,
                                       const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan,
                                       odin3_blif_vtr_report *report) {
    if (design == NULL || opts == NULL || plan == NULL || report == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: NULL design, opts or report");
        return ODIN3_ERR_INVALID_ARG;
    }
    *report = (odin3_blif_vtr_report){0};
    *plan = (odin3_blif_vtr_plan){.design = design};
    odin3_status st = check_opts(opts);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_module_id top = odin3_design_top(design);
    if (!odin3_module_valid(top)) { /* E13, checked first and alone (plan 3E point (f)) */
        odin3_log(ODIN3_LOG_ERROR,
                  VTR_PREFIX "no top module (read_blif sets one; or run hierarchy)");
        report->errors = 1;
        return ODIN3_ERR_INVALID_ARG;
    }
    /* Module IDs run from 1 to odin3_design_module_end - 1, every one live: the IR has no way to
     * remove a module (no odin3_module_live exists), so all but the top are skipped (§5). */
    report->modules_skipped = odin3_design_module_end(design) - 2;
    plan->top = odin3_module_get((odin3_design *)design, top);
    plan->k = odin3_blif_vtr_k(opts);
    st = walk_plan(opts, plan, report);
    if (st == ODIN3_OK && report->errors > ODIN3_BLIF_VTR_MAX_ERRORS) {
        odin3_log(ODIN3_LOG_ERROR, VTR_PREFIX "%u more errors not shown",
                  (unsigned)(report->errors - ODIN3_BLIF_VTR_MAX_ERRORS));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    return report->errors == 0 ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

void odin3_blif_vtr_plan_free(odin3_blif_vtr_plan *plan) {
    if (plan == NULL) {
        return;
    }
    odin3_u64map_destroy(plan->classes);
    odin3_blif_names_free(plan->names);
    odin3_strbuf_free(&plan->summary);
    *plan = (odin3_blif_vtr_plan){0};
}

odin3_blif_src_class odin3_blif_vtr_plan_class(const odin3_blif_vtr_plan *plan, odin3_net_id net) {
    uint64_t cls = ODIN3_BLIF_SRC_SIGNAL;
    if (!odin3_net_valid(net)) {
        return ODIN3_BLIF_SRC_UNDRIVEN;
    }
    if (plan->classes != NULL) {
        (void)odin3_u64map_get(plan->classes, net.v, &cls);
    }
    return (odin3_blif_src_class)cls;
}

odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report) {
    odin3_blif_vtr_plan plan = {0};
    odin3_status st = odin3_blif_vtr_plan_build(design, opts, &plan, report);
    if (st == ODIN3_OK) {
        odin3_blif_vtr_log_summary(&plan);
    }
    odin3_blif_vtr_plan_free(&plan);
    return st;
}

void odin3_blif_vtr_log_summary(const odin3_blif_vtr_plan *plan) {
    if (plan->summary.len > 0) {
        odin3_log(ODIN3_LOG_INFO, "%s", plan->summary.data);
    }
}
