/* lut.c — $sop (BLIF cover) as Yosys $lut (up to 6 inputs) or $sop with DEPTH/TABLE beyond. */
#include "backends/json/jw.h"

enum { LUT_MAX_INPUTS = 6 };

/* The rows of a cover whose output matches the polarity of its first row (BLIF: all rows of a
 * .names agree; an OFF-set cover lists the inputs for which the output is 0). */
typedef struct cover_view {
    const uint8_t *rows;
    uint32_t inputs;
    uint32_t n_rows;
    uint8_t polarity; /* '1': ON-set, '0': OFF-set */
} cover_view;

static cover_view view_of(const odin3_value *width, const odin3_value *cover) {
    cover_view view = {cover->bits, (uint32_t)width->i, 0, '1'};
    if (cover->len > 0) {
        view.n_rows = cover->len / (view.inputs + 1);
        view.polarity = cover->bits[view.inputs];
    }
    return view;
}

static const uint8_t *row_at(const cover_view *view, uint32_t row) {
    return view->rows + (size_t)row * (view->inputs + 1);
}

static bool row_counts(const cover_view *view, uint32_t row) {
    return row_at(view, row)[view->inputs] == view->polarity;
}

static bool row_matches(const cover_view *view, const uint8_t *cells, uint64_t idx) {
    for (uint32_t j = 0; j < view->inputs; j++) {
        bool bit = ((idx >> j) & 1U) != 0;
        if ((cells[j] == '1' && !bit) || (cells[j] == '0' && bit)) {
            return false;
        }
    }
    return true;
}

/* The function's value for input vector idx (input j is bit j). */
static bool eval(const cover_view *view, uint64_t idx) {
    bool hit = false;
    for (uint32_t row = 0; row < view->n_rows && !hit; row++) {
        hit = row_counts(view, row) && row_matches(view, row_at(view, row), idx);
    }
    return view->polarity == '0' ? !hit : hit;
}

static void write_lut(jw *out, const cover_view *view) {
    const uint64_t size = UINT64_C(1) << view->inputs;
    jw_char(out, '"');
    for (uint64_t idx = size; idx > 0; idx--) {
        jw_char(out, eval(view, idx - 1) ? '1' : '0');
    }
    jw_char(out, '"');
}

static void write_lut_cell(jw *out, jw_list *cells, odin3_node_id node, const cover_view *view) {
    jw_cell_ref ref = {node, "", "$lut"};
    jw_cell cell;
    jw_cell_begin(out, cells, &ref, &cell);
    jw_key(out, &cell.params, "LUT");
    write_lut(out, view);
    jw_param_int(out, &cell.params, "WIDTH", view->inputs);
    jw_cell_attrs(out, &cell, node);
    jw_dir(out, &cell, "A", ODIN3_DIR_IN);
    jw_dir(out, &cell, "Y", ODIN3_DIR_OUT);
    jw_cell_conns(out, &cell);
    jw_key(out, &cell.conns, "A");
    jw_pins(out, odin3_node_port(out->module, node, 0));
    jw_key(out, &cell.conns, "Y");
    jw_pins(out, odin3_node_port(out->module, node, 1));
    jw_cell_end(out, &cell);
}

/* One literal as Yosys $sop's two table bits (bit 1 then bit 0): '1' -> 10, '0' -> 01. */
static void write_literal(jw *out, uint8_t lit) {
    if (lit == '1') {
        jw_raw(out, "10");
    } else {
        jw_raw(out, lit == '0' ? "01" : "00");
    }
}

/* TABLE, most significant bit first: last row first, highest input first. */
static void write_table(jw *out, const cover_view *view) {
    jw_char(out, '"');
    for (uint32_t row = view->n_rows; row > 0; row--) {
        if (!row_counts(view, row - 1)) {
            continue;
        }
        for (uint32_t j = view->inputs; j > 0; j--) {
            write_literal(out, row_at(view, row - 1)[j - 1]);
        }
    }
    jw_char(out, '"');
}

static uint32_t depth_of(const cover_view *view) {
    uint32_t depth = 0;
    for (uint32_t row = 0; row < view->n_rows; row++) {
        depth += row_counts(view, row) ? 1U : 0U;
    }
    return depth;
}

/* Y is the node's output, or (OFF-set) the helper bit that a $not inverts into it. */
static void write_sop_cell(jw *out, jw_list *cells, odin3_node_id node, const cover_view *view) {
    const bool invert = view->polarity == '0';
    jw_cell_ref ref = {node, invert ? "$sop" : "", "$sop"};
    jw_cell cell;
    jw_cell_begin(out, cells, &ref, &cell);
    jw_param_int(out, &cell.params, "DEPTH", depth_of(view));
    jw_key(out, &cell.params, "TABLE");
    write_table(out, view);
    jw_param_int(out, &cell.params, "WIDTH", view->inputs);
    jw_cell_attrs(out, &cell, node);
    jw_dir(out, &cell, "A", ODIN3_DIR_IN);
    jw_dir(out, &cell, "Y", ODIN3_DIR_OUT);
    jw_cell_conns(out, &cell);
    jw_key(out, &cell.conns, "A");
    jw_pins(out, odin3_node_port(out->module, node, 0));
    jw_key(out, &cell.conns, "Y");
    if (invert) {
        jw_raw(out, "[ ");
        jw_bit(out, out->extra_bit);
        jw_raw(out, " ]");
    } else {
        jw_pins(out, odin3_node_port(out->module, node, 1));
    }
    jw_cell_end(out, &cell);
}

static void write_not_cell(jw *out, jw_list *cells, odin3_node_id node) {
    jw_cell_ref ref = {node, "", "$not"};
    jw_cell cell;
    jw_cell_begin(out, cells, &ref, &cell);
    jw_param_int(out, &cell.params, "A_SIGNED", 0);
    jw_param_int(out, &cell.params, "A_WIDTH", 1);
    jw_param_int(out, &cell.params, "Y_WIDTH", 1);
    jw_cell_attrs(out, &cell, node);
    jw_dir(out, &cell, "A", ODIN3_DIR_IN);
    jw_dir(out, &cell, "Y", ODIN3_DIR_OUT);
    jw_cell_conns(out, &cell);
    jw_key(out, &cell.conns, "A");
    jw_raw(out, "[ ");
    jw_bit(out, out->extra_bit);
    jw_raw(out, " ]");
    jw_key(out, &cell.conns, "Y");
    jw_pins(out, odin3_node_port(out->module, node, 1));
    jw_cell_end(out, &cell);
    out->extra_bit++;
}

void jw_sop_cell(jw *out, jw_list *list, odin3_node_id node) {
    const odin3_value *width = odin3_node_param(out->module, node, 0);
    const odin3_value *cover = odin3_node_param(out->module, node, 1);
    if (width == NULL || cover == NULL) {
        return;
    }
    cover_view view = view_of(width, cover);
    if (view.inputs <= LUT_MAX_INPUTS) {
        write_lut_cell(out, list, node, &view);
        return;
    }
    write_sop_cell(out, list, node, &view);
    if (view.polarity == '0') {
        write_not_cell(out, list, node);
    }
}
