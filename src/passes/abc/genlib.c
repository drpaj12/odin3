/*
 * genlib.c — the genlib text of an .o3lib library (gatemap spec 3; plan Task 4). Format in
 * genlib.h.
 */
#include "passes/abc/genlib.h"

#include "backends/blif/isop.h"
#include "ir/celltype.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/str.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    TABLE_INV = 0x1,  /* 1 input: the complement */
    TABLE_AND2 = 0x8, /* 2 inputs: A & B */
    TABLE_NAND2 = 0x7,
    TABLE_OR2 = 0xE,
    TABLE_NOR2 = 0x1,
    TWO_INPUTS = 2
};

static const char *str_of(const odin3_design *design, uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
}

static const odin3_libgate *gate_at(const odin3_libcells *cells, size_t idx) {
    return odin3_vec_cat(&cells->gates, idx);
}

/* ABC can build an AIG's AND and NOT from the library: an inverter and a 2-input AND, NAND, OR
 * or NOR. */
static bool has_and_basis(const odin3_libcells *cells) {
    bool inv = false;
    bool two = false;
    for (size_t i = 0; i < cells->gates.len; i++) {
        const odin3_libgate *gate = gate_at(cells, i);
        inv = inv || (gate->n_inputs == 1 && gate->table == TABLE_INV);
        two = two || (gate->n_inputs == TWO_INPUTS &&
                      (gate->table == TABLE_AND2 || gate->table == TABLE_NAND2 ||
                       gate->table == TABLE_OR2 || gate->table == TABLE_NOR2));
    }
    return inv && two;
}

odin3_status odin3_abc_genlib_check(const odin3_design *design, const odin3_libcells *cells,
                                    const char *library) {
    if (cells->n_cells == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "abc: --gates: library '%s' is not loaded; read it with read_techlib first",
                  library);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < cells->gates.len; i++) {
        const odin3_libgate *gate = gate_at(cells, i);
        double area = 0.0;
        if (!odin3_libcells_area(design, gate->area, &area)) {
            odin3_log(ODIN3_LOG_ERROR, "abc: --gates: library '%s': cell '%s' has no numeric area",
                      library, odin3_celltype_get(design, gate->type)->name);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    if (!has_and_basis(cells)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "abc: --gates: library '%s' has no inverter and 2-input AND, NAND, OR or NOR "
                  "gate; ABC cannot map to it",
                  library);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* One ISOP row (n plane characters) as a product: `name`, `!name`, joined by `*`. */
static odin3_status append_product(const odin3_celltype_def *def, const odin3_libgate *gate,
                                   const char *row, odin3_strbuf *out) {
    bool first = true;
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; j < gate->n_inputs && st == ODIN3_OK; j++) {
        if (row[j] == '-') {
            continue;
        }
        st = odin3_strbuf_appendf(out, "%s%s%s", first ? "" : "*", row[j] == '0' ? "!" : "",
                                  def->ports[gate->in_port[j]].name);
        first = false;
    }
    /* A row of `-` only would be the constant 1; the NO_SUPPORT check (a gate must depend on every
     * input) rejects such a cell before this point. */
    assert(!first || st != ODIN3_OK);
    return st;
}

/* The formula of gate: its on-set ISOP rows as products joined by `+`. */
static odin3_status append_formula(const odin3_celltype_def *def, const odin3_libgate *gate,
                                   odin3_strbuf *out) {
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t rows = 0;
    odin3_status st = odin3_blif_isop_set(&gate->table, gate->n_inputs, false, &cover, &rows);
    /* No on-set rows would be the constant 0; the NO_SUPPORT check rejects such a cell earlier. */
    assert(st != ODIN3_OK || rows > 0);
    const char *row = cover.data;
    for (uint32_t k = 0; k < rows && st == ODIN3_OK; k++) {
        st = k == 0 ? ODIN3_OK : odin3_strbuf_append(out, odin3_bytes_cstr("+"));
        if (st == ODIN3_OK) {
            st = append_product(def, gate, row, out);
        }
        const char *end = strchr(row, '\n');
        row = end == NULL ? row : end + 1;
    }
    odin3_strbuf_free(&cover);
    return st;
}

/* One gate: its GATE line and its PIN lines. */
static odin3_status append_gate(const odin3_design *design, const odin3_libgate *gate,
                                odin3_strbuf *out) {
    const odin3_celltype_def *def = odin3_celltype_get(design, gate->type);
    const char *area = gate->area == 0 ? "0" : str_of(design, gate->area);
    const char *delay = gate->delay == 0 ? "1" : str_of(design, gate->delay);
    odin3_status st = odin3_strbuf_appendf(out, "GATE %s %s %s=", def->name, area,
                                           def->ports[gate->out_port].name);
    if (st == ODIN3_OK) {
        st = append_formula(def, gate, out);
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr(";\n"));
    }
    for (uint32_t j = 0; j < gate->n_inputs && st == ODIN3_OK; j++) {
        st = odin3_strbuf_appendf(out, "PIN %s UNKNOWN 1 999 %s 0 %s 0\n",
                                  def->ports[gate->in_port[j]].name, delay, delay);
    }
    return st;
}

odin3_status odin3_abc_genlib_text(const odin3_design *design, const odin3_libcells *cells,
                                   odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < cells->gates.len && st == ODIN3_OK; i++) {
        st = append_gate(design, gate_at(cells, i), out);
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(
            out, odin3_bytes_cstr("GATE " ODIN3_ABC_GENLIB_CONST0 " 0 Y=CONST0;\n"
                                  "GATE " ODIN3_ABC_GENLIB_CONST1 " 0 Y=CONST1;\n"));
    }
    return st;
}

void odin3_abc_genlib_sha256(const odin3_strbuf *text, char hex[ODIN3_SHA256_HEX_CHARS]) {
    odin3_sha256 ctx;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_sha256_init(&ctx);
    odin3_sha256_update(&ctx, text->data, text->len);
    odin3_sha256_final(&ctx, digest);
    odin3_sha256_hex(digest, hex);
}
