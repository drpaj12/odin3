/* sim4.c — 4-state models of the cell types (sim4.h). */
#include "sim/sim4.h"

#include "ir/lut.h"
#include "ir/value.h"
#include "sim/word.h"
#include "sim/word4.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { CODE_0 = 0, CODE_1 = 1, CODE_X = 2, CODE_Z = 3 };

static bool unknown(uint8_t code) {
    return (code & 2U) != 0;
}

static uint8_t in4(const odin3_sim_cell *cell, uint32_t port) {
    return cell->values[cell->ports[port].idx[0]];
}

static void out4(const odin3_sim_cell *cell, uint8_t code) {
    cell->values[cell->ports[cell->n_ports - 1].idx[0]] = code;
}

/* --- gate functions on codes -------------------------------------------------------------------
 */

static uint8_t and4(uint8_t lhs, uint8_t rhs) {
    if ((!unknown(lhs) && lhs == 0) || (!unknown(rhs) && rhs == 0)) {
        return CODE_0;
    }
    return unknown(lhs) || unknown(rhs) ? (uint8_t)CODE_X : (uint8_t)CODE_1;
}

static uint8_t or4(uint8_t lhs, uint8_t rhs) {
    if ((!unknown(lhs) && lhs == 1) || (!unknown(rhs) && rhs == 1)) {
        return CODE_1;
    }
    return unknown(lhs) || unknown(rhs) ? (uint8_t)CODE_X : (uint8_t)CODE_0;
}

static uint8_t xor4(uint8_t lhs, uint8_t rhs) {
    return unknown(lhs) || unknown(rhs) ? (uint8_t)CODE_X : (uint8_t)(lhs ^ rhs);
}

static uint8_t not4(uint8_t code) {
    return unknown(code) ? (uint8_t)CODE_X : (uint8_t)(code ^ 1U);
}

/* a when equal and known in both, else x (the ?: merge). */
static uint8_t merge4(uint8_t lhs, uint8_t rhs) {
    return lhs == rhs && !unknown(lhs) ? lhs : (uint8_t)CODE_X;
}

enum { PORT_A, PORT_B, PORT_S };

static void buf4(const odin3_sim_cell *cell) {
    out4(cell, in4(cell, PORT_A)); /* a buffer passes z */
}
static void not_gate4(const odin3_sim_cell *cell) {
    out4(cell, not4(in4(cell, PORT_A)));
}
static void and_gate4(const odin3_sim_cell *cell) {
    out4(cell, and4(in4(cell, PORT_A), in4(cell, PORT_B)));
}
static void or_gate4(const odin3_sim_cell *cell) {
    out4(cell, or4(in4(cell, PORT_A), in4(cell, PORT_B)));
}
static void xor_gate4(const odin3_sim_cell *cell) {
    out4(cell, xor4(in4(cell, PORT_A), in4(cell, PORT_B)));
}
static void nand_gate4(const odin3_sim_cell *cell) {
    out4(cell, not4(and4(in4(cell, PORT_A), in4(cell, PORT_B))));
}
static void nor_gate4(const odin3_sim_cell *cell) {
    out4(cell, not4(or4(in4(cell, PORT_A), in4(cell, PORT_B))));
}
static void xnor_gate4(const odin3_sim_cell *cell) {
    out4(cell, not4(xor4(in4(cell, PORT_A), in4(cell, PORT_B))));
}
static void mux_gate4(const odin3_sim_cell *cell) {
    uint8_t sel = in4(cell, PORT_S);
    uint8_t lhs = in4(cell, PORT_A);
    uint8_t rhs = in4(cell, PORT_B);
    if (unknown(sel)) {
        out4(cell, (cell->flags & ODIN3_SIM_CELL_RTL_SEL) != 0 ? lhs : merge4(lhs, rhs));
        return;
    }
    out4(cell, sel == 1 ? rhs : lhs);
}
static void const0_4(const odin3_sim_cell *cell) {
    out4(cell, CODE_0);
}
static void const1_4(const odin3_sim_cell *cell) {
    out4(cell, CODE_1);
}
static void constx_4(const odin3_sim_cell *cell) {
    out4(cell, CODE_X);
}
static void constz_4(const odin3_sim_cell *cell) {
    out4(cell, CODE_Z);
}

/* --- storage -----------------------------------------------------------------------------------
 */

enum { SEQ_INIT = 0, D_FROM_END = 2 };

/* INIT 0 and 1 start the state at that value; 2 (don't care), 3 (unknown) and anything else at x.
 */
static uint8_t init_code(const odin3_value *init) {
    if (init->kind == ODIN3_VAL_INT && init->i == 0) {
        return CODE_0;
    }
    return init->kind == ODIN3_VAL_INT && init->i == 1 ? (uint8_t)CODE_1 : (uint8_t)CODE_X;
}

static uint8_t data4(const odin3_sim_cell *cell) {
    return in4(cell, cell->n_ports - D_FROM_END);
}

static void flop4(const odin3_sim_cell *cell, odin3_sim_event edge) {
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = init_code(&cell->params[SEQ_INIT]);
    } else if (cell->event == edge) {
        cell->state[0] = data4(cell);
    } else if (cell->event == ODIN3_SIM_COMB) {
        out4(cell, cell->state[0]);
    }
}

/* Transparent while E is active; E unknown: the state stays only when D equals it. */
static void latch4(const odin3_sim_cell *cell, uint8_t active) {
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = init_code(&cell->params[SEQ_INIT]);
    } else if (cell->event == ODIN3_SIM_COMB) {
        uint8_t enable = in4(cell, PORT_A);
        uint8_t data = data4(cell);
        if (unknown(enable)) {
            cell->state[0] = merge4(cell->state[0], data);
        } else if (enable == active) {
            cell->state[0] = data;
        }
        out4(cell, cell->state[0]);
    }
}

static void dff_p4(const odin3_sim_cell *cell) {
    flop4(cell, ODIN3_SIM_POSEDGE);
}
static void dff_n4(const odin3_sim_cell *cell) {
    flop4(cell, ODIN3_SIM_NEGEDGE);
}
static void dlatch_p4(const odin3_sim_cell *cell) {
    latch4(cell, 1);
}
static void dlatch_n4(const odin3_sim_cell *cell) {
    latch4(cell, 0);
}

/* The enable/reset flip-flops, from the name: $_DFFE_cE_, $_DFF_crv_, $_DFFE_crve_, $_SDFF_crv_,
 * $_SDFFE_crve_, $_SDFFCE_crve_ (c clock, r reset, e enable polarity P or N; v reset value). */
typedef enum dx_kind { DX_E, DX_A, DX_AE, DX_S, DX_SE, DX_SCE } dx_kind;

typedef struct dx_spec {
    dx_kind kind;
    bool clk_pos, rst_pos, rst_val, en_pos;
} dx_spec;

static bool has_reset(const dx_spec *spec) {
    return spec->kind != DX_E;
}
static bool has_enable(const dx_spec *spec) {
    return spec->kind != DX_A && spec->kind != DX_S;
}
static bool is_async(const dx_spec *spec) {
    return spec->kind == DX_A || spec->kind == DX_AE;
}

/* The code a pin must have to be active: unknown pins are handled by the caller. */
static uint8_t level_of(bool positive) {
    return positive ? CODE_1 : CODE_0;
}

/* What the state becomes at the edge for one assumption (r_on, e_on) of the control pins. */
static uint8_t next_for(const dx_spec *spec, bool r_on, bool e_on, uint8_t data, uint8_t state) {
    uint8_t rst = spec->rst_val ? CODE_1 : CODE_0;
    if (spec->kind == DX_SCE) {
        return !e_on ? state : (r_on ? rst : data);
    }
    if (r_on) {
        return rst;
    }
    return e_on ? data : state;
}

/* The merge of next_for over every assumption the unknown control pins allow. */
static uint8_t next4(const odin3_sim_cell *cell, const dx_spec *spec) {
    uint8_t data = data4(cell);
    uint8_t state = cell->state[0];
    uint8_t r_pin = has_reset(spec) ? in4(cell, 1) : level_of(!spec->rst_pos);
    uint8_t e_pin = has_enable(spec) ? in4(cell, has_reset(spec) ? 2 : 1) : level_of(spec->en_pos);
    bool r_known = !unknown(r_pin);
    bool e_known = !unknown(e_pin);
    uint8_t out = 0;
    bool first = true;
    for (uint32_t r_case = 0; r_case < 2; r_case++) {
        for (uint32_t e_case = 0; e_case < 2; e_case++) {
            bool r_on = r_case != 0;
            bool e_on = e_case != 0;
            bool r_ok = !has_reset(spec)
                            ? !r_on
                            : (r_known ? (r_pin == level_of(spec->rst_pos)) == r_on : true);
            bool e_ok = !has_enable(spec)
                            ? e_on
                            : (e_known ? (e_pin == level_of(spec->en_pos)) == e_on : true);
            if (!r_ok || !e_ok) {
                continue;
            }
            uint8_t cand = next_for(spec, r_on, e_on, data, state);
            out = first ? cand : merge4(out, cand);
            first = false;
        }
    }
    return out;
}

/* An asynchronous reset: active forces the value, unknown leaves the state only when it equals it.
 */
static void async_reset4(const odin3_sim_cell *cell, const dx_spec *spec) {
    uint8_t pin = in4(cell, 1);
    uint8_t rst = spec->rst_val ? CODE_1 : CODE_0;
    if (unknown(pin)) {
        cell->state[0] = merge4(cell->state[0], rst);
    } else if (pin == level_of(spec->rst_pos)) {
        cell->state[0] = rst;
    }
}

/* The spec rides in the view's type4 pointer as a small integer (no allocation, no shared state):
 * bits 0-2 the kind, 3 clk_pos, 4 rst_pos, 5 rst_val, 6 en_pos, plus 1 so it is never NULL. */
static const void *dx_pack(const dx_spec *spec) {
    uintptr_t bits = (uintptr_t)spec->kind | ((uintptr_t)spec->clk_pos << 3) |
                     ((uintptr_t)spec->rst_pos << 4) | ((uintptr_t)spec->rst_val << 5) |
                     ((uintptr_t)spec->en_pos << 6);
    return (const void *)(bits + 1U);
}

static dx_spec dx_unpack(const void *data) {
    uintptr_t bits = (uintptr_t)data - 1U;
    return (dx_spec){.kind = (dx_kind)(bits & 7U),
                     .clk_pos = ((bits >> 3) & 1U) != 0,
                     .rst_pos = ((bits >> 4) & 1U) != 0,
                     .rst_val = ((bits >> 5) & 1U) != 0,
                     .en_pos = ((bits >> 6) & 1U) != 0};
}

static void dffx4(const odin3_sim_cell *cell) {
    const dx_spec unpacked = dx_unpack(cell->type4);
    const dx_spec *spec = &unpacked;
    if (cell->event == ODIN3_SIM_INIT) {
        cell->state[0] = init_code(&cell->params[SEQ_INIT]);
        return;
    }
    if (is_async(spec)) {
        async_reset4(cell, spec);
    }
    odin3_sim_event edge = spec->clk_pos ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE;
    if (cell->event == edge) {
        cell->state[0] = next4(cell, spec);
    } else if (cell->event == ODIN3_SIM_COMB) {
        out4(cell, cell->state[0]);
    }
}

/* Parses the name of an enable/reset flip-flop; false for any other name. */
static bool dx_parse(const char *name, dx_spec *spec) {
    static const struct {
        const char *prefix;
        dx_kind
            kind2; /* kind when two letters follow the prefix (enable only), else the 3/4 form */
        dx_kind kind3;
        dx_kind kind4;
    } k_forms[] = {
        {"$_DFFE_", DX_E, DX_A, DX_AE},        {"$_DFF_", DX_A, DX_A, DX_A},
        {"$_SDFF_", DX_S, DX_S, DX_S},         {"$_SDFFE_", DX_SE, DX_SE, DX_SE},
        {"$_SDFFCE_", DX_SCE, DX_SCE, DX_SCE},
    };
    for (size_t i = 0; i < sizeof k_forms / sizeof k_forms[0]; i++) {
        size_t plen = strlen(k_forms[i].prefix);
        if (strncmp(name, k_forms[i].prefix, plen) != 0) {
            continue;
        }
        const char *letters = name + plen;
        size_t n = strlen(letters);
        if (n < 2 || letters[n - 1] != '_') {
            return false;
        }
        n--; /* letters before the closing underscore */
        *spec = (dx_spec){0};
        spec->clk_pos = letters[0] == 'P';
        if (i == 0 && n == 2) {
            spec->kind = DX_E;
            spec->en_pos = letters[1] == 'P';
            return true;
        }
        if (i == 0 && n == 4) {
            spec->kind = DX_AE;
        } else if (i == 1 && n == 3) {
            spec->kind = DX_A;
        } else if ((i == 2 && n == 3) || (i >= 3 && n == 4)) {
            spec->kind = k_forms[i].kind3;
        } else {
            return false;
        }
        spec->rst_pos = letters[1] == 'P';
        spec->rst_val = letters[2] == '1';
        spec->en_pos = n == 4 && letters[3] == 'P';
        return true;
    }
    return false;
}

/* --- $lut ---------------------------------------------------------------------------------------
 */

/* Y = LUT[i]; an unknown input bit tries both values, and Y is known only when all agree. */
static void lut4(const odin3_sim_cell *cell) {
    const odin3_sim_span *span = &cell->ports[ODIN3_LUT_PORT_A];
    const uint8_t *table = cell->params[ODIN3_LUT_P_LUT].bits;
    uint32_t base = 0;
    uint32_t free_bits[ODIN3_LUT_MAX_WIDTH];
    uint32_t n_free = 0;
    for (uint32_t j = 0; j < span->width; j++) {
        uint8_t code = cell->values[span->idx[j]];
        if (unknown(code)) {
            free_bits[n_free++] = j;
        } else {
            base |= (uint32_t)code << j;
        }
    }
    uint8_t first = table[base] == ODIN3_BIT_1 ? CODE_1 : CODE_0;
    for (uint32_t combo = 1; combo < (1U << n_free); combo++) {
        uint32_t idx = base;
        for (uint32_t k = 0; k < n_free; k++) {
            idx |= ((combo >> k) & 1U) << free_bits[k];
        }
        if ((table[idx] == ODIN3_BIT_1 ? CODE_1 : CODE_0) != first) {
            out4(cell, CODE_X);
            return;
        }
    }
    out4(cell, first);
}

/* --- word cells --------------------------------------------------------------------------------
 */

enum { P_A_SIGNED = 0, P_B_SIGNED = 1, W_PLANES = 4 };

static bool param_set(const odin3_sim_cell *cell, uint32_t param) {
    return cell->params[param].kind == ODIN3_VAL_INT && cell->params[param].i != 0;
}

/* Y = code in bit 0, 0 above (a comparison or reduction result at Y_WIDTH). */
static void flag4(const odin3_sim_cell *cell, uint32_t code) {
    const odin3_sim_span *y_span = &cell->ports[cell->n_ports - 1];
    for (uint32_t k = 0; k < y_span->width; k++) {
        cell->values[y_span->idx[k]] = k == 0 ? (uint8_t)code : (uint8_t)0;
    }
}

/* The two operands of $eq/$ne at width, extended like the 2-state cell (signed only when both). */
static uint32_t eq_code(const odin3_sim_cell *cell, bool case_eq) {
    uint32_t a_width = cell->ports[0].width;
    uint32_t b_width = cell->ports[1].width;
    uint32_t width = a_width > b_width ? a_width : b_width;
    uint32_t stride = odin3_word_limbs(width);
    uint64_t *planes = (uint64_t *)cell->scratch;
    bool is_signed = param_set(cell, P_A_SIGNED) && param_set(cell, P_B_SIGNED);
    odin3_w4 lhs = {planes, planes + stride};
    odin3_w4 rhs = {planes + 2U * stride, planes + 3U * stride};
    odin3_w4_load(lhs, width, &(odin3_word_port){cell->values, cell->ports[0], is_signed});
    odin3_w4_load(rhs, width, &(odin3_word_port){cell->values, cell->ports[1], is_signed});
    odin3_w4_bin op = {{NULL, NULL}, {lhs.v, lhs.u}, {rhs.v, rhs.u}, width};
    return case_eq ? odin3_w4_case_eq(&op) : odin3_w4_eq(&op);
}

static void eq4(const odin3_sim_cell *cell) {
    flag4(cell, eq_code(cell, (cell->flags & ODIN3_SIM_CELL_RTL_SEL) != 0));
}

static void ne4(const odin3_sim_cell *cell) {
    uint32_t code = eq_code(cell, (cell->flags & ODIN3_SIM_CELL_RTL_SEL) != 0);
    flag4(cell, code >= 2U ? code : code ^ 1U);
}

/* The truth of A as a condition (1 if any bit is 1, 0 if all are 0, else x). */
static uint32_t truth_of_a(const odin3_sim_cell *cell) {
    uint32_t width = cell->ports[0].width;
    uint32_t stride = odin3_word_limbs(width);
    uint64_t *planes = (uint64_t *)cell->scratch;
    odin3_w4 word = {planes, planes + stride};
    odin3_w4_load(word, width, &(odin3_word_port){cell->values, cell->ports[0], false});
    return odin3_w4_truth((odin3_w4c){word.v, word.u}, width);
}

static void reduce_or4(const odin3_sim_cell *cell) {
    flag4(cell, truth_of_a(cell));
}

static void logic_not4(const odin3_sim_cell *cell) {
    uint32_t code = truth_of_a(cell);
    flag4(cell, code >= 2U ? code : code ^ 1U);
}

/* $mux: Y = S ? B : A. An unknown S takes A when the mux comes from an RTL if/case, else merges. */
static void mux4(const odin3_sim_cell *cell) {
    const odin3_sim_span *y_span = &cell->ports[3];
    uint8_t sel = in4(cell, PORT_S);
    bool rtl = (cell->flags & ODIN3_SIM_CELL_RTL_SEL) != 0;
    for (uint32_t k = 0; k < y_span->width; k++) {
        uint8_t lhs = cell->values[cell->ports[PORT_A].idx[k]];
        uint8_t rhs = cell->values[cell->ports[PORT_B].idx[k]];
        uint8_t out = lhs;
        if (!unknown(sel)) {
            out = sel == 1 ? rhs : lhs;
        } else if (!rtl) {
            out = merge4(lhs, rhs);
        }
        cell->values[y_span->idx[k]] = out;
    }
}

/* $pmux: Y = A when no select bit is 1, else the OR of the selected B slices; an unknown select
 * bit merges the results with and without it (an RTL case ignores it: its selects are known). */
static void pmux4(const odin3_sim_cell *cell) {
    const odin3_sim_span *s_span = &cell->ports[2];
    const odin3_sim_span *y_span = &cell->ports[3];
    const odin3_sim_span *b_span = &cell->ports[PORT_B];
    bool rtl = (cell->flags & ODIN3_SIM_CELL_RTL_SEL) != 0;
    for (uint32_t k = 0; k < y_span->width; k++) {
        bool any = false;
        uint8_t value = 0;
        for (uint32_t i = 0; i < s_span->width; i++) {
            if (cell->values[s_span->idx[i]] == 1) {
                uint8_t slice = cell->values[b_span->idx[i * y_span->width + k]];
                value = any ? or4(value, slice) : slice;
                any = true;
            }
        }
        uint8_t out = any ? value : cell->values[cell->ports[PORT_A].idx[k]];
        for (uint32_t i = 0; i < s_span->width && !rtl; i++) {
            if (unknown(cell->values[s_span->idx[i]])) {
                uint8_t slice = cell->values[b_span->idx[i * y_span->width + k]];
                out = merge4(out, any ? or4(value, slice) : slice);
            }
        }
        cell->values[y_span->idx[k]] = out;
    }
}

/* --- the table ---------------------------------------------------------------------------------
 */

typedef struct hook_entry {
    const char *name;
    odin3_sim4_fn fn;
} hook_entry;

static const hook_entry k_hooks[] = {
    {"$_BUF_", buf4},
    {"$_NOT_", not_gate4},
    {"$_AND_", and_gate4},
    {"$_OR_", or_gate4},
    {"$_XOR_", xor_gate4},
    {"$_NAND_", nand_gate4},
    {"$_NOR_", nor_gate4},
    {"$_XNOR_", xnor_gate4},
    {"$_MUX_", mux_gate4},
    {"$_CONST0_", const0_4},
    {"$_CONST1_", const1_4},
    {"$_CONSTX_", constx_4},
    {"$_CONSTZ_", constz_4},
    {"$_DFF_P_", dff_p4},
    {"$_DFF_N_", dff_n4},
    {"$_FF_", dff_p4},
    {"$_DLATCH_P_", dlatch_p4},
    {"$_DLATCH_N_", dlatch_n4},
    {"$lut", lut4},
    {"$mux", mux4},
    {"$pmux", pmux4},
    {"$eq", eq4},
    {"$ne", ne4},
    {"$reduce_or", reduce_or4},
    {"$reduce_bool", reduce_or4},
    {"$logic_not", logic_not4},
};

odin3_sim4_fn odin3_sim4_lookup(const odin3_celltype_def *def, const void **data) {
    *data = NULL;
    for (size_t i = 0; i < sizeof k_hooks / sizeof k_hooks[0]; i++) {
        if (strcmp(def->name, k_hooks[i].name) == 0) {
            return k_hooks[i].fn;
        }
    }
    dx_spec spec;
    if (dx_parse(def->name, &spec)) {
        *data = dx_pack(&spec);
        return dffx4;
    }
    return NULL;
}

uint32_t odin3_sim4_scratch_bytes(const odin3_celltype_def *def, const odin3_sim_cell *sizing) {
    uint32_t width = 0;
    if (strcmp(def->name, "$eq") == 0 || strcmp(def->name, "$ne") == 0) {
        uint32_t a_width = sizing->ports[0].width;
        uint32_t b_width = sizing->ports[1].width;
        width = a_width > b_width ? a_width : b_width;
    } else if (strcmp(def->name, "$reduce_or") == 0 || strcmp(def->name, "$reduce_bool") == 0 ||
               strcmp(def->name, "$logic_not") == 0) {
        width = sizing->ports[0].width;
    }
    uint32_t bytes = 0;
    for (uint32_t i = 0; width > 0 && i < W_PLANES; i++) {
        if (!odin3_word_reserve(width, &bytes)) {
            return UINT32_MAX;
        }
    }
    return bytes;
}

void odin3_sim4_generic(const odin3_sim_cell *cell) {
    const odin3_celltype_def *def = cell->type4;
    bool any_unknown = false;
    for (uint32_t port = 0; port < cell->n_ports && !any_unknown; port++) {
        if (def->ports[port].dir != ODIN3_DIR_IN) {
            continue;
        }
        for (uint32_t k = 0; k < cell->ports[port].width; k++) {
            any_unknown = any_unknown || unknown(cell->values[cell->ports[port].idx[k]]);
        }
    }
    if (!any_unknown) {
        def->simulate(cell);
        return;
    }
    for (uint32_t port = 0; port < cell->n_ports; port++) {
        if (def->ports[port].dir == ODIN3_DIR_IN) {
            continue;
        }
        for (uint32_t k = 0; k < cell->ports[port].width; k++) {
            cell->values[cell->ports[port].idx[k]] = CODE_X;
        }
    }
}
