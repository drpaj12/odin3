/*
 * xblif.c — the BLIF exchange's input and command files (3D spec §6.2, AB-15, AB-13, AB-22; plan
 * Task 16). The read-back is in xblif_read.c.
 */
#include "passes/abc/xblif.h"

#include "backends/blif/isop.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "passes/builtin.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    NAME_BYTES = 24, /* "o3co4294967295" and a NUL */
    WRAP_COLUMN = 100,
    ROW_TAIL_BYTES = 3, /* a blank, the output character, a newline */
    LINE_SLACK = 64     /* the fixed text around the names of a one-line directive */
};

typedef struct xname {
    char text[NAME_BYTES];
} xname;

/* One file being written. */
typedef struct wctx {
    odin3_abc_run *run;
    const odin3_abc_region *region;
    const odin3_module *module;
    odin3_strbuf body;
    size_t col; /* the current output column of body */
    bool c0_used, c1_used;
    odin3_vec names;    /* xname: the input names of the cell being written */
    odin3_vec inverted; /* uint8_t per register: INIT 1 (AB-13) */
} wctx;

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

/* --- names --------------------------------------------------------------------------------- */

static void write_name(const wctx *wc, odin3_net_id net, xname *out) {
    uint32_t co = odin3_net_valid(net) ? u32_at(&wc->region->co_of, net.v) : 0;
    if (co != 0) {
        (void)snprintf(out->text, NAME_BYTES, "o3co%" PRIu32, co - 1U);
    } else {
        (void)snprintf(out->text, NAME_BYTES, "o3n%" PRIu32, net.v);
    }
}

/* The name a reader of net uses (§6.2's table): constants first, then CI, register, other. */
static void read_name(wctx *wc, odin3_net_id net, xname *out) {
    int lit = odin3_abc_pin_literal(wc->run, net);
    if (!odin3_net_valid(net) || lit == 0) {
        wc->c0_used = true;
        (void)snprintf(out->text, NAME_BYTES, "o3c0");
        return;
    }
    if (lit == 1) {
        wc->c1_used = true;
        (void)snprintf(out->text, NAME_BYTES, "o3c1");
        return;
    }
    uint32_t ci = u32_at(&wc->region->ci_of, net.v);
    uint32_t reg = u32_at(&wc->region->q_of, net.v);
    if (ci != 0) {
        (void)snprintf(out->text, NAME_BYTES, "o3ci%" PRIu32, ci - 1U);
    } else if (reg != 0) {
        bool inv = ((const uint8_t *)wc->inverted.data)[reg - 1U] != 0;
        (void)snprintf(out->text, NAME_BYTES, "o3%c%" PRIu32, inv ? 'r' : 'q', reg - 1U);
    } else {
        write_name(wc, net, out);
    }
}

/* --- output with wrapping ------------------------------------------------------------------ */

static odin3_status put_text(wctx *wc, const char *text) {
    size_t len = strlen(text);
    odin3_status st = odin3_strbuf_append(&wc->body, (odin3_bytes){text, len});
    const char *nl = strrchr(text, '\n');
    wc->col = nl == NULL ? wc->col + len : (size_t)(text + len - nl - 1);
    return st;
}

/* One blank-separated word of a directive line, continuing the line when it is long. */
static odin3_status put_word(wctx *wc, const char *word) {
    odin3_status st = ODIN3_OK;
    if (wc->col > WRAP_COLUMN) {
        st = put_text(wc, " \\\n");
    }
    st = st == ODIN3_OK ? put_text(wc, " ") : st;
    return st == ODIN3_OK ? put_text(wc, word) : st;
}

/* --- cells --------------------------------------------------------------------------------- */

static odin3_status collect_inputs(wctx *wc, odin3_node_id node) {
    odin3_vec_clear(&wc->names);
    odin3_pinslice pins = odin3_node_pins(wc->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_dir(wc->module, pin) != ODIN3_DIR_IN) {
            continue;
        }
        xname *slot = odin3_vec_push(&wc->names);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        read_name(wc, odin3_pin_net(wc->module, pin), slot);
    }
    return ODIN3_OK;
}

/* Cover rows as one view: row i starts at data + i * stride with k plane characters; the output
 * character follows the plane (after one blank when spaced). */
typedef struct rows_view {
    const char *data;
    size_t stride;
    uint32_t count;
    bool spaced;
} rows_view;

/* The first column with the same name as column col (col itself when there is none). */
static uint32_t first_same(const xname *names, uint32_t col) {
    for (uint32_t twin = 0; twin < col; twin++) {
        if (strcmp(names[twin].text, names[col].text) == 0) {
            return twin;
        }
    }
    return col;
}

static bool has_twins(const xname *names, uint32_t count) {
    for (uint32_t col = 1; col < count; col++) {
        if (first_same(names, col) != col) {
            return true;
        }
    }
    return false;
}

/* One row with duplicate input names merged (a column agrees with its first twin or the row is
 * dropped); false when the row is contradictory. plane gets one character per column, '-' in the
 * columns that a twin took over. */
static bool merge_row(const xname *names, uint32_t count, const char *row, char *plane) {
    memcpy(plane, row, count);
    for (uint32_t col = 0; col < count; col++) {
        uint32_t twin = first_same(names, col);
        if (twin == col || row[col] == '-') {
            continue;
        }
        if (plane[twin] != '-' && plane[twin] != row[col]) {
            return false;
        }
        plane[twin] = row[col];
        plane[col] = '-';
    }
    return true;
}

/* A row's text: the plane characters of the unique columns, a blank, the output, a newline. */
static odin3_status put_row(wctx *wc, const char *plane, bool twins, char out_char) {
    const xname *names = wc->names.data;
    uint32_t count = (uint32_t)wc->names.len;
    odin3_status st = ODIN3_OK;
    for (uint32_t col = 0; st == ODIN3_OK && col < count; col++) {
        if (!twins || first_same(names, col) == col) {
            st = odin3_strbuf_append(&wc->body, (odin3_bytes){&plane[col], 1});
        }
    }
    char tail[ROW_TAIL_BYTES];
    size_t used = 0;
    if (count > 0) {
        tail[used++] = ' ';
    }
    tail[used++] = out_char;
    tail[used++] = '\n';
    return st == ODIN3_OK ? odin3_strbuf_append(&wc->body, (odin3_bytes){tail, used}) : st;
}

static odin3_status put_rows(wctx *wc, const rows_view *rows, odin3_strbuf *plane) {
    uint32_t count = (uint32_t)wc->names.len;
    bool twins = has_twins(wc->names.data, count);
    odin3_status st = ODIN3_OK;
    for (uint32_t num = 0; st == ODIN3_OK && num < rows->count; num++) {
        const char *row = rows->data + (size_t)num * rows->stride;
        char out_char = row[count + (rows->spaced ? 1U : 0U)];
        if (!twins) {
            st = put_row(wc, row, false, out_char);
        } else if (merge_row(wc->names.data, count, row, plane->data)) {
            st = put_row(wc, plane->data, true, out_char);
        }
    }
    wc->col = 0;
    return st;
}

/* `.names <unique inputs> <out>` and the rows. */
static odin3_status put_names(wctx *wc, const xname *out, const rows_view *rows) {
    uint32_t count = (uint32_t)wc->names.len;
    const xname *names = wc->names.data;
    odin3_status st = put_text(wc, ".names");
    for (uint32_t col = 0; st == ODIN3_OK && col < count; col++) {
        st = first_same(names, col) == col ? put_word(wc, names[col].text) : st;
    }
    st = st == ODIN3_OK ? put_word(wc, out->text) : st;
    st = st == ODIN3_OK ? put_text(wc, "\n") : st;
    odin3_strbuf plane;
    odin3_strbuf_init(&plane);
    st = st == ODIN3_OK ? odin3_strbuf_reserve(&plane, (size_t)count + 1U) : st;
    if (st == ODIN3_OK) {
        st = put_rows(wc, rows, &plane);
    }
    odin3_strbuf_free(&plane);
    return st;
}

/* A $sop: its cover as written. */
static odin3_status put_sop(wctx *wc, odin3_node_id node, const xname *out) {
    const odin3_value *cover = odin3_node_param(wc->module, node, 1);
    uint32_t k = (uint32_t)wc->names.len;
    size_t stride = (size_t)k + 1U;
    rows_view rows = {(const char *)cover->bits, stride, (uint32_t)(cover->len / stride), false};
    return put_names(wc, out, &rows);
}

/* A table of k inputs (words) as its irredundant cover (§6.2: AB-3). */
static odin3_status put_table(wctx *wc, const uint64_t *words, const xname *out) {
    uint32_t k = (uint32_t)wc->names.len;
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t n_rows = 0;
    int constant = -1;
    odin3_status st = odin3_blif_isop(words, k, &cover, &n_rows, &constant);
    if (st == ODIN3_OK && constant >= 0) {
        rows_view rows = {"1", 1, constant == 1 ? 1U : 0U, false};
        wc->names.len = 0; /* a constant has no inputs */
        st = put_names(wc, out, &rows);
    } else if (st == ODIN3_OK) {
        rows_view rows = {cover.data, (size_t)k + 3U, n_rows, true};
        st = put_names(wc, out, &rows);
    }
    odin3_strbuf_free(&cover);
    return st;
}

static odin3_status put_lut(wctx *wc, odin3_node_id node, const xname *out) {
    uint64_t words[ODIN3_LUT_MAX_WORDS];
    odin3_value params[2] = {*odin3_node_param(wc->module, node, ODIN3_LUT_P_WIDTH),
                             *odin3_node_param(wc->module, node, ODIN3_LUT_P_LUT)};
    if (!odin3_lut_table(params, words, ODIN3_LUT_MAX_WORDS)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return put_table(wc, words, out);
}

static odin3_status put_soft(wctx *wc, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wc->module, node);
    uint64_t table = 0;
    uint32_t inputs = 0;
    odin3_status st = collect_inputs(wc, node);
    xname out;
    odin3_pinslice pins = odin3_node_pins(wc->module, node);
    odin3_net_id net = {0};
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(wc->module, pin)) {
            net = odin3_pin_net(wc->module, pin);
        }
    }
    if (odin3_net_valid(net)) {
        write_name(wc, net, &out);
    } else {
        (void)snprintf(out.text, NAME_BYTES, "o3u%" PRIu32, node.v);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_status gate = odin3_abc_gate_table(wc->run, type, &table, &inputs);
    if (gate == ODIN3_OK) {
        return put_table(wc, &table, &out);
    }
    if (gate == ODIN3_ERR_NO_MEMORY) {
        return gate;
    }
    const odin3_celltype_def *def = odin3_celltype_get(odin3_module_design(wc->module), type);
    return strcmp(def->name, "$lut") == 0 ? put_lut(wc, node, &out) : put_sop(wc, node, &out);
}

/* --- registers ----------------------------------------------------------------------------- */

static odin3_status put_register(wctx *wc, uint32_t reg) {
    bool inv = ((const uint8_t *)wc->inverted.data)[reg] != 0;
    odin3_net_id dnet = {u32_at(&wc->region->reg_d, reg)};
    xname dname;
    read_name(wc, dnet, &dname);
    char line[3 * NAME_BYTES + LINE_SLACK];
    odin3_status st = ODIN3_OK;
    if (inv) {
        (void)snprintf(line, sizeof line, ".names %s o3d%" PRIu32 "\n0 1\n", dname.text, reg);
        st = put_text(wc, line);
        (void)snprintf(dname.text, NAME_BYTES, "o3d%" PRIu32, reg);
    }
    (void)snprintf(line, sizeof line, ".latch %s o3q%" PRIu32 " 0\n", dname.text, reg);
    st = st == ODIN3_OK ? put_text(wc, line) : st;
    if (inv && st == ODIN3_OK) {
        (void)snprintf(line, sizeof line, ".names o3q%" PRIu32 " o3r%" PRIu32 "\n0 1\n", reg, reg);
        st = put_text(wc, line);
    }
    return st;
}

/* A CO that is a register's value is a buffer from its IR value (o3q<j>, or o3r<j> inverted). */
static odin3_status put_co_buffers(wctx *wc) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < wc->region->cos.len; i++) {
        odin3_net_id net = {u32_at(&wc->region->cos, i)};
        uint32_t reg = u32_at(&wc->region->q_of, net.v);
        if (reg == 0) {
            continue;
        }
        bool inv = ((const uint8_t *)wc->inverted.data)[reg - 1U] != 0;
        char line[3 * NAME_BYTES + LINE_SLACK];
        (void)snprintf(line, sizeof line, ".names o3%c%" PRIu32 " o3co%u\n1 1\n", inv ? 'r' : 'q',
                       reg - 1U, (unsigned)i);
        st = put_text(wc, line);
    }
    return st;
}

static odin3_status init_inverted(wctx *wc) {
    odin3_vec_init(&wc->inverted, 1);
    for (size_t j = 0; j < wc->region->regs.len; j++) {
        odin3_node_id reg = {u32_at(&wc->region->regs, j)};
        const odin3_value *init = odin3_node_param(wc->module, reg, 0);
        uint8_t *slot = odin3_vec_push(&wc->inverted);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = init != NULL && init->kind == ODIN3_VAL_INT && init->i == 1 ? 1U : 0U;
    }
    return ODIN3_OK;
}

/* --- the file ------------------------------------------------------------------------------ */

/* One directive's names (` o3ci<i>` or ` o3co<i>`), wrapped when the line is long. */
static odin3_status put_port_names(odin3_strbuf *head, const char *prefix, size_t count) {
    odin3_status st = ODIN3_OK;
    size_t col = 0;
    for (size_t idx = 0; st == ODIN3_OK && idx < count; idx++) {
        char word[NAME_BYTES];
        (void)snprintf(word, sizeof word, " %s%u", prefix, (unsigned)idx);
        if (col > WRAP_COLUMN) {
            st = odin3_strbuf_append(head, odin3_bytes_cstr(" \\\n"));
            col = 0;
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(head, odin3_bytes_cstr(word)) : st;
        col += strlen(word);
    }
    return st;
}

static odin3_status put_header(const wctx *wc, odin3_strbuf *head) {
    odin3_status st = odin3_strbuf_append(head, odin3_bytes_cstr(".model o3abc\n.inputs"));
    st = st == ODIN3_OK ? put_port_names(head, "o3ci", wc->region->cis.len) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(head, odin3_bytes_cstr("\n.outputs")) : st;
    st = st == ODIN3_OK ? put_port_names(head, "o3co", wc->region->cos.len) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(head, odin3_bytes_cstr("\n")) : st;
    if (st == ODIN3_OK && wc->c0_used) {
        st = odin3_strbuf_append(head, odin3_bytes_cstr(".names o3c0\n"));
    }
    if (st == ODIN3_OK && wc->c1_used) {
        st = odin3_strbuf_append(head, odin3_bytes_cstr(".names o3c1\n1\n"));
    }
    return st;
}

static odin3_status write_body(wctx *wc) {
    odin3_status st = init_inverted(wc);
    for (size_t idx = 0; st == ODIN3_OK && idx < wc->region->soft.len; idx++) {
        st = put_soft(wc, (odin3_node_id){u32_at(&wc->region->soft, idx)});
    }
    for (uint32_t reg = 0; st == ODIN3_OK && reg < wc->region->regs.len; reg++) {
        st = put_register(wc, reg);
    }
    st = st == ODIN3_OK ? put_co_buffers(wc) : st;
    return st == ODIN3_OK ? put_text(wc, ".end\n") : st;
}

odin3_status odin3_abc_xblif_write(odin3_abc_run *run, const odin3_abc_region *region,
                                   const char *path) {
    wctx wc = {run, region, run->module, {0}, 0, false, false, {0}, {0}};
    odin3_strbuf_init(&wc.body);
    odin3_vec_init(&wc.names, sizeof(xname));
    odin3_vec_init(&wc.inverted, 1);
    odin3_strbuf head;
    odin3_strbuf_init(&head);
    odin3_status st = write_body(&wc);
    st = st == ODIN3_OK ? put_header(&wc, &head) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(&head, (odin3_bytes){wc.body.data, wc.body.len}) : st;
    st = st == ODIN3_OK ? odin3_pass_write_text(path, &head) : st;
    odin3_strbuf_free(&head);
    odin3_strbuf_free(&wc.body);
    odin3_vec_free(&wc.names);
    odin3_vec_free(&wc.inverted);
    return st;
}

/* --- commands ------------------------------------------------------------------------------ */

uint32_t odin3_abc_xblif_command_count(const odin3_abc_run *run) {
    return 2U + run->script.n_cmds + (run->script.final_space == ODIN3_ABC_SPACE_GIA ? 1U : 0U);
}

odin3_status odin3_abc_xblif_command(const odin3_abc_run *run, uint32_t index,
                                     const odin3_abc_paths *paths, odin3_strbuf *out,
                                     const odin3_abc_cmd **src) {
    *src = NULL;
    uint32_t n_cmds = run->script.n_cmds;
    if (index == 0) {
        return odin3_strbuf_appendf(out, "read \"%s\"", paths->in);
    }
    if (index <= n_cmds) {
        *src = odin3_abc_script_cmd(&run->script, index - 1U);
        return odin3_strbuf_append(out, odin3_bytes_cstr((*src)->line));
    }
    if (run->script.final_space == ODIN3_ABC_SPACE_GIA && index == n_cmds + 1U) {
        return odin3_strbuf_append(out, odin3_bytes_cstr("&put"));
    }
    return odin3_strbuf_appendf(out, "write_hie \"%s\" \"%s\"", paths->in, paths->out);
}

odin3_status odin3_abc_xblif_commands(const odin3_abc_run *run, const odin3_abc_paths *paths,
                                      odin3_strbuf *out) {
    const odin3_abc_cmd *src = NULL;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_abc_xblif_command_count(run); i++) {
        st = odin3_abc_xblif_command(run, i, paths, out, &src);
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n")) : st;
    }
    return st;
}
