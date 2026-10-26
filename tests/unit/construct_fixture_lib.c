/*
 * construct_fixture_lib.c — the test-only odin3_procfix library's odin3_test_construct_design: a
 * design with a hand-filled construct table (the elaborator does not fill it before V1.2.0 Task
 * 4), so the Python binding reads a chain and a join through the ABI. Never installed.
 *
 * Constructs (rows): 1 module top, 2 always A, 3 always B, 4 statement a1 in A, 5 expression e1 in
 * a1, 6 statement b1 in B. Records: SOURCE e1, SOURCE b1, DERIVED coarsen {e1, b1} (a join of
 * a1 and b1). Module `top` has the join, its node n (a $_NOT_) the SOURCE e1, its net a rewrite.
 */
#include "ir/celltype.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ROWS = 6, LINE_STEP = 10 };

typedef struct fixture {
    odin3_design *design;
    odin3_pass_ctx ctx;
    uint32_t file;
} fixture;

static uint32_t intern(odin3_design *design, const char *text) {
    uint32_t str = 0;
    return odin3_design_intern(design, odin3_bytes_cstr(text), &str) == ODIN3_OK ? str : 0;
}

/* Rows 1-6: kind and parent; AST node n of row r is 10 * r. */
static odin3_status add_rows(fixture *fix) {
    static const struct {
        uint32_t kind;
        uint32_t parent;
    } rows[ROWS] = {{ODIN3_AST_MODULE, 0}, {ODIN3_AST_ALWAYS, 1},
                    {ODIN3_AST_ALWAYS, 1}, {ODIN3_AST_BLOCKING_ASSIGN, 2},
                    {ODIN3_AST_BINARY, 4}, {ODIN3_AST_BLOCKING_ASSIGN, 3}};
    for (uint32_t i = 0; i < ROWS; i++) {
        odin3_construct_row row = {0};
        row.run = fix->ctx.run.v;
        row.ast = LINE_STEP * (i + 1);
        row.parent = rows[i].parent;
        row.kind = (uint8_t)rows[i].kind;
        row.level = (uint8_t)odin3_construct_kind_level(rows[i].kind);
        row.file = fix->file;
        row.line = LINE_STEP * (i + 1);
        row.end_line = row.line;
        row.col = 1;
        row.end_col = 20;
        row.name = i == 0 ? intern(fix->design, "top") : 0;
        uint32_t index = 0;
        odin3_status st = odin3_construct_add(fix->design, &row, &index);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return odin3_construct_mark_run(fix->design, fix->ctx.run, ODIN3_CONSTRUCT_MODE_REFS);
}

static odin3_prov_id leaf(fixture *fix, uint32_t row) {
    odin3_srcloc loc = {fix->file, LINE_STEP * row, 1, LINE_STEP * row, 20, 0};
    odin3_prov_origin origin = {&loc, 1, LINE_STEP * row, 0};
    odin3_prov_id id = {0};
    return odin3_prov_source(&fix->ctx, &origin, &id) == ODIN3_OK ? id : (odin3_prov_id){0};
}

/* recs[0] names module top, recs[1] its node n. */
static odin3_status add_objects(fixture *fix, const odin3_prov_id recs[2]) {
    odin3_module_id mid = {0};
    odin3_status st = odin3_module_create(fix->design, intern(fix->design, "top"), recs[0], &mid);
    odin3_celltype_id type = {0};
    odin3_node_id node = {0};
    odin3_net_id net = {0};
    if (st == ODIN3_OK && !odin3_celltype_find(fix->design, intern(fix->design, "$_NOT_"), &type)) {
        st = ODIN3_ERR_INVALID_ARG;
    }
    odin3_module *module = st == ODIN3_OK ? odin3_module_get(fix->design, mid) : NULL;
    odin3_node_spec spec = {type, intern(fix->design, "n"), recs[1], NULL, 0};
    if (st == ODIN3_OK) {
        st = odin3_node_create(module, &spec, &node);
    }
    if (st == ODIN3_OK) {
        st = odin3_net_create(module, intern(fix->design, "w"), (odin3_prov_id){0}, &net);
    }
    return st;
}

static odin3_status build(fixture *fix) {
    odin3_status st =
        odin3_pass_run_begin(fix->design, intern(fix->design, "read_verilog"), &fix->ctx);
    fix->file = intern(fix->design, "t.v");
    if (st == ODIN3_OK) {
        st = add_rows(fix);
    }
    odin3_prov_id parents[2] = {leaf(fix, 5), leaf(fix, 6)};
    odin3_prov_id join = {0};
    const odin3_opclass cls = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_STMT, ODIN3_LEVEL_WORD, false, 0};
    if (st == ODIN3_OK) {
        odin3_prov_begin_op_class(&fix->ctx, &cls);
        st = odin3_prov_derive(&fix->ctx, (odin3_prov_list){parents, 2}, &join);
    }
    const odin3_prov_id named[2] = {join, parents[0]};
    return st == ODIN3_OK ? add_objects(fix, named) : st;
}

odin3_design *odin3_test_construct_design(void);

ODIN3_EXPORT odin3_design *odin3_test_construct_design(void) {
    fixture fix = {odin3_design_create(), {0}, 0};
    if (fix.design != NULL && build(&fix) != ODIN3_OK) {
        odin3_design_destroy(fix.design);
        fix.design = NULL;
    }
    return fix.design;
}
