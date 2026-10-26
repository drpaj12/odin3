/*
 * test_prov_label.c — the ABI v7 record readers on hand-built graphs (V1.2.0 3.1, 3.3): class
 * labels (dir, levels), construct, span-only, joins (distinct anchors, inherited blends, the
 * instance rule, inlined calls, the cap, bulk), the sort order of J, chain and object readers.
 */
#include "backends/common/prov_export.h"
#include "ir/celltype.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static odin3_design *design;
static odin3_pass_ctx ctx;

static const odin3_opclass REWRITE = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                      0};
static const odin3_opclass REFINE = {ODIN3_DIR_REFINE, ODIN3_LEVEL_STMT, ODIN3_LEVEL_WORD, false,
                                     0};
static const odin3_opclass COARSEN = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, false,
                                      0};
static const odin3_opclass BULK = {ODIN3_DIR_COARSEN, ODIN3_LEVEL_GATE, ODIN3_LEVEL_LUT, true, 0};
static const odin3_opclass CONTEXT1 = {ODIN3_DIR_CONTEXT, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD, false,
                                       1};

/* Row numbers of the shared table (build_table). */
enum {
    R_MOD = 1,
    R_ALWAYS_A,
    R_ALWAYS_B,
    R_A1, /* statement in A; 2 expressions below it */
    R_E1,
    R_E2,
    R_B1, /* statement in B */
    R_IF, /* statement in A; then-arm; statement in the arm */
    R_THEN,
    R_IN_ARM,
    R_INST1, /* two instances in the module */
    R_INST2,
    R_FIRST_FREE
};

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

typedef struct row_args {
    uint32_t kind;
    uint32_t part;
    uint32_t ast;
    uint32_t parent;
} row_args;

/* ADD_ROW(kind, part, ast, parent): appends a row; level from the kind map; line = ast. */
#define ADD_ROW(...) add_row((row_args){__VA_ARGS__})

static uint32_t add_row(row_args args) {
    odin3_construct_row row = {0};
    row.run = ctx.run.v;
    row.ast = args.ast;
    row.parent = args.parent;
    row.part = (uint8_t)args.part;
    row.kind = (uint8_t)args.kind;
    row.level = (uint8_t)odin3_construct_kind_level(args.kind);
    row.file = intern("a.v");
    row.line = args.ast;
    row.end_line = args.ast;
    row.end_col = 5;
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &index));
    return index;
}

/* The shared table; AST node n of row R_x is n = 10 * R_x, so a record's ast says its row. */
static void build_table(void) {
    TEST_ASSERT_EQUAL_UINT32(R_MOD, ADD_ROW(ODIN3_AST_MODULE, 0, 10, 0));
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_A, ADD_ROW(ODIN3_AST_ALWAYS, 0, 20, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_B, ADD_ROW(ODIN3_AST_ALWAYS, 0, 30, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_A1, ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 0, 40, R_ALWAYS_A));
    TEST_ASSERT_EQUAL_UINT32(R_E1, ADD_ROW(ODIN3_AST_BINARY, 0, 50, R_A1));
    TEST_ASSERT_EQUAL_UINT32(R_E2, ADD_ROW(ODIN3_AST_BINARY, 0, 60, R_A1));
    TEST_ASSERT_EQUAL_UINT32(R_B1, ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 0, 70, R_ALWAYS_B));
    TEST_ASSERT_EQUAL_UINT32(R_IF, ADD_ROW(ODIN3_AST_IF, 0, 80, R_ALWAYS_A));
    TEST_ASSERT_EQUAL_UINT32(R_THEN,
                             ADD_ROW(ODIN3_CONSTRUCT_IF_THEN, ODIN3_CONSTRUCT_PART_THEN, 80, R_IF));
    TEST_ASSERT_EQUAL_UINT32(R_IN_ARM, ADD_ROW(ODIN3_AST_NONBLOCKING_ASSIGN, 0, 100, R_THEN));
    TEST_ASSERT_EQUAL_UINT32(R_INST1, ADD_ROW(ODIN3_AST_INSTANCE, 0, 110, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_INST2, ADD_ROW(ODIN3_AST_INSTANCE, 0, 120, R_MOD));
}

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_verilog"), &ctx));
    build_table();
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

/* SOURCE record of the construct row `row` of the shared table (its ast is 10 * row, but for
 * the rows past R_FIRST_FREE, which tests make with ast = 10 * row too). */
static odin3_prov_id leaf_of(uint32_t row) {
    odin3_srcloc loc = {intern("a.v"), row, 1, row, 5, 0};
    odin3_prov_origin origin = {&loc, 1, 10 * row, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static odin3_prov_id derive(const odin3_opclass *cls, const odin3_prov_id *parents, uint32_t n) {
    odin3_prov_id out = {0};
    odin3_prov_begin_op_class(&ctx, cls);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){parents, n}, &out));
    return out;
}

static odin3_prov_info info_of(odin3_prov_id record) {
    odin3_prov_info info;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_get_info(design, record.v, &info));
    return info;
}

typedef struct collected {
    uint32_t ids[64];
    uint32_t count;
} collected;

static void collect(uint32_t id, void *user) {
    collected *out = user;
    TEST_ASSERT_TRUE(out->count < 64);
    out->ids[out->count++] = id;
}

/* --- origin records -------------------------------------------------------------------------- */

static void test_source_record(void) {
    odin3_prov_info info = info_of(leaf_of(R_A1));
    TEST_ASSERT_EQUAL_UINT32(0, info.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_ORIGIN, info.dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_NONE, info.from_level);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_NONE, info.to_level);
    TEST_ASSERT_EQUAL_UINT32(R_A1, info.construct);
    TEST_ASSERT_EQUAL_UINT32(0, info.flags);
    TEST_ASSERT_EQUAL_UINT32(0, info.n_parents);
    TEST_ASSERT_EQUAL_UINT32(0, info.n_context);
    TEST_ASSERT_EQUAL_UINT32(0, info.n_joined);
    TEST_ASSERT_EQUAL_STRING("", info.hier);
    TEST_ASSERT_EQUAL_UINT32(ctx.run.v, info.run);
}

static void test_leaf_without_construct_is_span_only(void) {
    odin3_srcloc loc = {intern("a.v"), 3, 1, 3, 5, 0};
    odin3_prov_origin none = {&loc, 1, 0, 0};
    odin3_prov_id src = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &none, &src));
    odin3_prov_info info = info_of(src);
    TEST_ASSERT_EQUAL_UINT32(0, info.construct);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_SPAN_ONLY, info.flags);
    odin3_prov_id imp = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&ctx, &none, &imp));
    TEST_ASSERT_EQUAL_UINT32(1, info_of(imp).kind);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_SPAN_ONLY, info_of(imp).flags);
}

static void test_record_out_of_range(void) {
    odin3_prov_info info;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_get_info(design, 0, &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_get_info(design, odin3_prov_end(design), &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_get_info(NULL, 1, &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_get_info(design, 1, NULL));
}

/* --- class labels ---------------------------------------------------------------------------- */

static void test_rewrite_and_refine_labels(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_info rewrite = info_of(derive(&REWRITE, &src, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_REWRITE, rewrite.dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_WORD, rewrite.from_level);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_WORD, rewrite.to_level);
    TEST_ASSERT_EQUAL_UINT32(2, rewrite.kind);
    TEST_ASSERT_EQUAL_UINT32(1, rewrite.n_parents);
    TEST_ASSERT_EQUAL_UINT32(0, rewrite.flags);
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    odin3_prov_info refine = info_of(derive(&REFINE, two, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_REFINE, refine.dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_STMT, refine.from_level);
    TEST_ASSERT_EQUAL_UINT32(0, refine.flags & ODIN3_PROV_F_JOIN); /* the proc mux is no join */
    TEST_ASSERT_EQUAL_UINT32(0, refine.n_joined);
}

static void test_hier_is_a_string_of_the_design(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_id out = {0};
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_derive_hier(&ctx, (odin3_prov_list){&src, 1}, intern("top/u1"), &out));
    TEST_ASSERT_EQUAL_STRING("top/u1", info_of(out).hier);
}

/* --- joins ----------------------------------------------------------------------------------- */

static void test_two_expressions_of_one_statement_are_no_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_E1), leaf_of(R_E2)};
    odin3_prov_info info = info_of(derive(&COARSEN, two, 2));
    TEST_ASSERT_EQUAL_UINT32(1, info.n_joined); /* one anchor: the statement */
    TEST_ASSERT_EQUAL_UINT32(0, info.flags & ODIN3_PROV_F_JOIN);
    collected rows = {{0}, 0};
    odin3_prov_id id = derive(&COARSEN, two, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_joined(design, id.v, collect, &rows));
    TEST_ASSERT_EQUAL_UINT32(1, rows.count);
    TEST_ASSERT_EQUAL_UINT32(R_A1, rows.ids[0]);
}

static void test_two_blocks_are_a_join_in_key_order(void) {
    odin3_prov_id two[2] = {leaf_of(R_B1), leaf_of(R_A1)}; /* stored B first */
    odin3_prov_id id = derive(&COARSEN, two, 2);
    odin3_prov_info info = info_of(id);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, info.flags);
    TEST_ASSERT_EQUAL_UINT32(2, info.n_joined);
    collected rows = {{0}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_joined(design, id.v, collect, &rows));
    TEST_ASSERT_EQUAL_UINT32(2, rows.count);
    TEST_ASSERT_EQUAL_UINT32(R_A1, rows.ids[0]); /* module, always A < always B by span */
    TEST_ASSERT_EQUAL_UINT32(R_B1, rows.ids[1]);
}

static void test_an_if_and_its_arm_are_no_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_IF), leaf_of(R_IN_ARM)};
    odin3_prov_info info = info_of(derive(&COARSEN, two, 2));
    TEST_ASSERT_EQUAL_UINT32(1, info.n_joined); /* the arm statement; the IF is its ancestor */
    TEST_ASSERT_EQUAL_UINT32(0, info.flags & ODIN3_PROV_F_JOIN);
    collected rows = {{0}, 0};
    odin3_prov_id id = derive(&COARSEN, two, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_joined(design, id.v, collect, &rows));
    TEST_ASSERT_EQUAL_UINT32(R_IN_ARM, rows.ids[0]);
}

static void test_a_blend_inherited_from_one_parent_is_no_new_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    odin3_prov_id blend = derive(&COARSEN, two, 2);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, info_of(blend).flags);
    odin3_prov_id again[2] = {blend, leaf_of(R_E1)}; /* e1 is below a1: same J */
    odin3_prov_info same = info_of(derive(&COARSEN, again, 2));
    TEST_ASSERT_EQUAL_UINT32(2, same.n_joined);
    TEST_ASSERT_EQUAL_UINT32(0, same.flags & ODIN3_PROV_F_JOIN);
    odin3_prov_id wider[2] = {blend, leaf_of(R_IF)};
    odin3_prov_info grown = info_of(derive(&COARSEN, wider, 2));
    TEST_ASSERT_EQUAL_UINT32(3, grown.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, grown.flags & ODIN3_PROV_F_JOIN);
}

static void test_span_only_leaves_never_join(void) {
    odin3_srcloc loc = {intern("a.v"), 3, 1, 3, 5, 0};
    odin3_prov_origin none = {&loc, 1, 0, 0};
    odin3_prov_id imp = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&ctx, &none, &imp));
    odin3_prov_id two[2] = {leaf_of(R_A1), imp};
    odin3_prov_info info = info_of(derive(&COARSEN, two, 2));
    TEST_ASSERT_EQUAL_UINT32(1, info.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_SPAN_ONLY, info.flags);
}

static void test_cap_and_bulk_flags(void) {
    enum { WIDE = 20 };
    odin3_prov_id parents[WIDE];
    for (uint32_t i = 0; i < WIDE; i++) {
        uint32_t row = ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 0, 10 * (R_FIRST_FREE + i), R_ALWAYS_B);
        parents[i] = leaf_of(row);
    }
    odin3_prov_id id = derive(&BULK, parents, WIDE);
    odin3_prov_info info = info_of(id);
    TEST_ASSERT_EQUAL_UINT32(WIDE, info.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN | ODIN3_PROV_F_LOSSY_CAP | ODIN3_PROV_F_LOSSY_BULK,
                             info.flags);
    collected rows = {{0}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_joined(design, id.v, collect, &rows));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_JOIN_CAP, rows.count);
    TEST_ASSERT_EQUAL_UINT32(R_FIRST_FREE, rows.ids[0]);
    /* a small bulk run is still bulk */
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN | ODIN3_PROV_F_LOSSY_BULK,
                             info_of(derive(&BULK, two, 2)).flags);
}

/* --- instances ------------------------------------------------------------------------------- */

static void test_context_records_and_the_instance_join(void) {
    odin3_prov_id orig = leaf_of(R_A1);
    odin3_prov_id pair1[2] = {orig, leaf_of(R_INST1)};
    odin3_prov_id pair2[2] = {orig, leaf_of(R_INST2)};
    odin3_prov_id flat1 = {0};
    odin3_prov_id flat2 = {0};
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){pair1, 2}, &flat1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){pair2, 2}, &flat2));
    odin3_prov_info ctx_info = info_of(flat1);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_CONTEXT, ctx_info.dir);
    TEST_ASSERT_EQUAL_UINT32(1, ctx_info.n_context);
    TEST_ASSERT_EQUAL_UINT32(2, ctx_info.n_parents);
    TEST_ASSERT_EQUAL_UINT32(0, ctx_info.flags);
    odin3_prov_id both[2] = {flat1, flat2};
    odin3_prov_info merged = info_of(derive(&COARSEN, both, 2));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN_INSTANCE, merged.flags);
    TEST_ASSERT_EQUAL_UINT32(1, merged.n_joined); /* the instances never enter J */
    odin3_prov_id one[1] = {flat1};
    TEST_ASSERT_EQUAL_UINT32(0, info_of(derive(&COARSEN, one, 1)).flags);
}

static void test_instance_alone_has_no_data_leaf(void) {
    odin3_prov_id only_inst[1] = {leaf_of(R_INST1)};
    odin3_prov_id flat = {0};
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive(&ctx, (odin3_prov_list){only_inst, 1}, &flat));
    odin3_prov_info info = info_of(flat);
    TEST_ASSERT_EQUAL_UINT32(1, info.n_context);
    TEST_ASSERT_EQUAL_UINT32(0, info.flags); /* no data leaf, so no span-only leaf either */
    odin3_prov_id merged[2] = {flat, leaf_of(R_B1)};
    odin3_prov_info coarse = info_of(derive(&COARSEN, merged, 2));
    TEST_ASSERT_EQUAL_UINT32(1, coarse.n_joined);
}

/* --- inlined calls --------------------------------------------------------------------------- */

static void test_a_body_called_from_two_blocks_is_a_join(void) {
    uint32_t site_a = ADD_ROW(ODIN3_CONSTRUCT_CALL_SITE, 0, 900, R_A1);
    uint32_t site_b = ADD_ROW(ODIN3_CONSTRUCT_CALL_SITE, 0, 910, R_B1);
    uint32_t body = ADD_ROW(ODIN3_AST_FUNCTION_DECL, 0, 920, R_MOD);
    uint32_t stmt = ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 0, 930, body);
    odin3_construct_row copy_fn = *odin3_construct_get(design, body);
    copy_fn.next = 0;
    odin3_construct_row copy_st = *odin3_construct_get(design, stmt);
    copy_st.next = 0;
    uint32_t sites[2] = {site_a, site_b};
    odin3_prov_id leaves[2];
    for (uint32_t i = 0; i < 2; i++) {
        odin3_construct_row fn = copy_fn;
        fn.parent = sites[i];
        fn.call = sites[i];
        uint32_t fn_row = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &fn, &fn_row));
        odin3_construct_row st = copy_st;
        st.parent = fn_row;
        st.call = sites[i];
        uint32_t st_row = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &st, &st_row));
        const odin3_construct_row *site = odin3_construct_get(design, sites[i]);
        odin3_srcloc locs[2] = {
            {intern("a.v"), 930, 1, 930, 5, 0},
            {site->file, site->line, site->col, site->end_line, site->end_col, 0}};
        odin3_prov_origin origin = {locs, 2, 930, 0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &leaves[i]));
        TEST_ASSERT_EQUAL_UINT32(st_row, info_of(leaves[i]).construct);
    }
    TEST_ASSERT_TRUE(info_of(leaves[0]).construct != info_of(leaves[1]).construct);
    odin3_prov_info joined = info_of(derive(&COARSEN, leaves, 2));
    TEST_ASSERT_EQUAL_UINT32(2, joined.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, joined.flags);
}

/* --- parents, chain, objects ----------------------------------------------------------------- */

static void test_visit_parents_in_stored_order(void) {
    odin3_prov_id two[2] = {leaf_of(R_B1), leaf_of(R_A1)};
    odin3_prov_id id = derive(&COARSEN, two, 2);
    collected got = {{0}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_parents(design, id.v, collect, &got));
    TEST_ASSERT_EQUAL_UINT32(2, got.count);
    TEST_ASSERT_EQUAL_UINT32(two[0].v, got.ids[0]);
    TEST_ASSERT_EQUAL_UINT32(two[1].v, got.ids[1]);
    got.count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_parents(design, two[0].v, collect, &got));
    TEST_ASSERT_EQUAL_UINT32(0, got.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_parents(design, 0, collect, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_parents(design, id.v, NULL, &got));
}

typedef struct chain_log {
    uint32_t index[16];
    uint32_t leaf[16];
    odin3_level level[16];
    uint32_t count;
} chain_log;

static void on_construct(const odin3_construct *found, uint32_t leaf, void *user) {
    chain_log *log = user;
    TEST_ASSERT_TRUE(log->count < 16);
    log->index[log->count] = found->index;
    log->leaf[log->count] = leaf;
    log->level[log->count] = found->level;
    log->count++;
}

static odin3_module *make_module(odin3_prov_id prov, odin3_module_id *id) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("top"), prov, id));
    return odin3_module_get(design, *id);
}

static void test_obj_prov_and_chain(void) {
    odin3_prov_id two[2] = {leaf_of(R_E1), leaf_of(R_B1)};
    odin3_prov_id id = derive(&COARSEN, two, 2);
    odin3_module_id mid = {0};
    odin3_module *module = make_module(id, &mid);
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(module, intern("n"), (odin3_prov_id){0}, &net));
    uint32_t record = 99;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_obj_get_prov(design, (odin3_obj){mid.v, ODIN3_OBJ_MODULE, 0}, &record));
    TEST_ASSERT_EQUAL_UINT32(id.v, record);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_obj_get_prov(design, (odin3_obj){mid.v, ODIN3_OBJ_NET, net.v}, &record));
    TEST_ASSERT_EQUAL_UINT32(0, record); /* no provenance */
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_obj_get_prov(design, (odin3_obj){mid.v, ODIN3_OBJ_NET, 999}, &record));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_obj_get_prov(design, (odin3_obj){7, ODIN3_OBJ_NET, 1}, &record));
    chain_log log = {{0}, {0}, {0}, 0};
    odin3_obj module_obj = {mid.v, ODIN3_OBJ_MODULE, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_chain(design, module_obj, on_construct, &log));
    /* leaf e1: e1, a1, always A, module; leaf b1: b1, always B (module already reported) */
    TEST_ASSERT_EQUAL_UINT32(6, log.count);
    TEST_ASSERT_EQUAL_UINT32(R_E1, log.index[0]);
    TEST_ASSERT_EQUAL_UINT32(R_A1, log.index[1]);
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_A, log.index[2]);
    TEST_ASSERT_EQUAL_UINT32(R_MOD, log.index[3]);
    TEST_ASSERT_EQUAL_UINT32(R_B1, log.index[4]);
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_B, log.index[5]);
    TEST_ASSERT_EQUAL_UINT32(two[0].v, log.leaf[3]);
    TEST_ASSERT_EQUAL_UINT32(two[1].v, log.leaf[4]);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_EXPR, log.level[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_MODULE, log.level[3]);
    log.count = 0;
    odin3_obj net_obj = {mid.v, ODIN3_OBJ_NET, net.v};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_chain(design, net_obj, on_construct, &log));
    TEST_ASSERT_EQUAL_UINT32(0, log.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_chain(design, module_obj, NULL, &log));
}

static void test_get_construct_copies_a_row(void) {
    odin3_construct row;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_construct(design, R_THEN, &row));
    TEST_ASSERT_EQUAL_UINT32(R_THEN, row.index);
    TEST_ASSERT_EQUAL_UINT32(R_IF, row.parent);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_CONSTRUCT_IF_THEN, row.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_BRANCH, row.level);
    TEST_ASSERT_EQUAL_STRING("a.v", row.file);
    TEST_ASSERT_EQUAL_STRING("", row.name);
    TEST_ASSERT_EQUAL_UINT32(80, row.line);
    TEST_ASSERT_EQUAL_UINT32(R_FIRST_FREE, odin3_design_construct_count(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_construct(design, 0, &row));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_construct(design, R_FIRST_FREE, &row));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_construct(design, 1, NULL));
}

/* --- NULL arguments, dead and compacted objects ---------------------------------------------- */

static void noop_u32(uint32_t id, void *user) {
    (void)id;
    (void)user;
}

static void noop_construct(const odin3_construct *found, uint32_t leaf, void *user) {
    (void)found;
    (void)leaf;
    (void)user;
}

/* Each of the seven readers refuses a NULL design, out pointer and visit; outputs stay put. */
static void test_null_arguments_on_every_reader(void) {
    odin3_prov_id two[2] = {leaf_of(R_E1), leaf_of(R_B1)};
    odin3_prov_id id = derive(&COARSEN, two, 2);
    odin3_module_id mid = {0};
    (void)make_module(id, &mid);
    odin3_obj obj = {mid.v, ODIN3_OBJ_MODULE, 0};
    odin3_construct row;
    memset(&row, 0x5a, sizeof row);
    odin3_prov_info info;
    memset(&info, 0x5a, sizeof info);
    uint32_t record = 77;
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_construct_count(NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_construct(NULL, 1, &row));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_construct(design, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_obj_get_prov(NULL, obj, &record));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_obj_get_prov(design, obj, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_get_info(NULL, id.v, &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_get_info(design, id.v, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_parents(NULL, id.v, noop_u32, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_parents(design, id.v, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_joined(NULL, id.v, noop_u32, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_visit_joined(design, id.v, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_chain(NULL, obj, noop_construct, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_visit_chain(design, obj, NULL, NULL));
    odin3_construct untouched;
    memset(&untouched, 0x5a, sizeof untouched);
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &row, sizeof row);
    odin3_prov_info untouched_info;
    memset(&untouched_info, 0x5a, sizeof untouched_info);
    TEST_ASSERT_EQUAL_MEMORY(&untouched_info, &info, sizeof info);
    TEST_ASSERT_EQUAL_UINT32(77, record);
}

static void test_dead_and_compacted_objects(void) {
    odin3_prov_id src_a = leaf_of(R_A1);
    odin3_prov_id src_b = leaf_of(R_B1);
    odin3_module_id mid = {0};
    odin3_module *module = make_module((odin3_prov_id){0}, &mid);
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_NOT_"), &type));
    odin3_node_id first = {0};
    odin3_node_id second = {0};
    odin3_node_spec spec = {type, intern("n1"), src_a, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &first));
    spec = (odin3_node_spec){type, intern("n2"), src_b, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &second));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, first));
    /* a dead node still answers, until compact */
    odin3_obj dead = {mid.v, ODIN3_OBJ_NODE, first.v};
    uint32_t record = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_obj_get_prov(design, dead, &record));
    TEST_ASSERT_EQUAL_UINT32(src_a.v, record);
    chain_log log = {{0}, {0}, {0}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_chain(design, dead, on_construct, &log));
    TEST_ASSERT_EQUAL_UINT32(R_A1, log.index[0]);
    TEST_ASSERT_EQUAL_UINT32(R_MOD, log.index[log.count - 1U]);
    odin3_compact_map map = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    odin3_compact_map_free(&map);
    /* the live node is now ID 1; the old ID 2 is past the store */
    odin3_obj moved = {mid.v, ODIN3_OBJ_NODE, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_obj_get_prov(design, moved, &record));
    TEST_ASSERT_EQUAL_UINT32(src_b.v, record);
    odin3_obj gone = {mid.v, ODIN3_OBJ_NODE, second.v};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_obj_get_prov(design, gone, &record));
    log.count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_visit_chain(design, gone, on_construct, &log));
    TEST_ASSERT_EQUAL_UINT32(0, log.count);
    /* records and rows survive compact */
    TEST_ASSERT_EQUAL_UINT32(R_B1, info_of(src_b).construct);
    TEST_ASSERT_EQUAL_UINT32(R_FIRST_FREE, odin3_design_construct_count(design));
}

/* --- identical chains, the cache, wide and deep graphs -------------------------------------- */

/* A SOURCE record of AST node `ast`, at line `line` (hash-consing keeps equal ones apart). */
static odin3_prov_id leaf_at(uint32_t ast, uint32_t line) {
    odin3_srcloc loc = {intern("a.v"), line, 1, line, 5, 0};
    odin3_prov_origin origin = {&loc, 1, ast, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static void test_rows_of_one_chain_are_one_construct(void) {
    /* two rows (two AST nodes) with the same file, span, kind and parent: one construct */
    odin3_construct_row row = {0};
    row.run = ctx.run.v;
    row.parent = R_ALWAYS_B;
    row.kind = ODIN3_AST_BLOCKING_ASSIGN;
    row.level = (uint8_t)odin3_construct_kind_level(ODIN3_AST_BLOCKING_ASSIGN);
    row.file = intern("a.v");
    row.line = 900;
    row.end_line = 900;
    row.end_col = 5;
    uint32_t twin[2] = {0, 0};
    for (uint32_t i = 0; i < 2; i++) {
        row.ast = 9000 + i;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_construct_add(design, &row, &twin[i]));
    }
    odin3_prov_id pair[2] = {leaf_at(9000, 1), leaf_at(9001, 2)};
    odin3_prov_info same = info_of(derive(&COARSEN, pair, 2));
    TEST_ASSERT_EQUAL_UINT32(1, same.n_joined);
    TEST_ASSERT_EQUAL_UINT32(0, same.flags);
    /* a record over the twins and an other statement: a parent over the twins alone has J of
     * one, so the join is new and J holds the twins once */
    odin3_prov_id with_other[3] = {pair[0], pair[1], leaf_of(R_B1)};
    odin3_prov_info wider = info_of(derive(&COARSEN, with_other, 3));
    TEST_ASSERT_EQUAL_UINT32(2, wider.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, wider.flags);
    /* the same set through a parent that holds the other twin is no new join */
    odin3_prov_id other_twin[2] = {leaf_at(9001, 3), leaf_of(R_B1)};
    odin3_prov_id base = derive(&COARSEN, other_twin, 2);
    odin3_prov_id again[2] = {base, pair[0]};
    odin3_prov_info inherited = info_of(derive(&COARSEN, again, 2));
    TEST_ASSERT_EQUAL_UINT32(2, inherited.n_joined);
    TEST_ASSERT_EQUAL_UINT32(0, inherited.flags & ODIN3_PROV_F_JOIN);
}

static void test_labels_follow_the_table_as_it_grows(void) {
    odin3_prov_id late = leaf_at(7777, 4); /* no row yet: span-only */
    odin3_prov_id two[2] = {late, leaf_of(R_A1)};
    odin3_prov_id merge = derive(&COARSEN, two, 2);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_SPAN_ONLY, info_of(late).flags);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_SPAN_ONLY, info_of(merge).flags);
    TEST_ASSERT_EQUAL_UINT32(1, info_of(merge).n_joined);
    uint32_t row = ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 0, 7777, R_ALWAYS_B);
    TEST_ASSERT_EQUAL_UINT32(row, info_of(late).construct);
    TEST_ASSERT_EQUAL_UINT32(0, info_of(late).flags);
    odin3_prov_info grown = info_of(merge);
    TEST_ASSERT_EQUAL_UINT32(0, grown.flags & ODIN3_PROV_F_SPAN_ONLY);
    TEST_ASSERT_EQUAL_UINT32(2, grown.n_joined);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_F_JOIN, grown.flags & ODIN3_PROV_F_JOIN);
    /* a clone starts with no cache and agrees */
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    odin3_prov_info cloned;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_get_info(copy, merge.v, &cloned));
    TEST_ASSERT_EQUAL_UINT32(grown.flags, cloned.flags);
    TEST_ASSERT_EQUAL_UINT32(grown.n_joined, cloned.n_joined);
    odin3_design_destroy(copy);
}

/* Reads every record of the graph, newest first, and returns the clock ticks it took. */
static clock_t read_all_records(uint32_t first, uint32_t last) {
    clock_t start = clock();
    for (uint32_t record = last; record >= first; record--) {
        odin3_prov_info info;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_get_info(design, record, &info));
    }
    return clock() - start;
}

static void test_a_deep_graph_is_read_in_linear_work(void) {
    enum { DEPTH = 30000, SECONDS = 15 };
    uint32_t ast = 10 * R_E1; /* every leaf anchors at the statement a1 */
    odin3_prov_id head = leaf_at(ast, 1);
    uint32_t first = head.v;
    for (uint32_t i = 0; i < DEPTH; i++) {
        odin3_prov_id pair[2] = {head, leaf_at(ast, 2U + i)};
        head = derive(&COARSEN, pair, 2);
    }
    clock_t used = read_all_records(first, head.v);
    TEST_ASSERT_TRUE_MESSAGE(used < (clock_t)SECONDS * CLOCKS_PER_SEC,
                             "deep graph read too slowly");
    odin3_prov_info top = info_of(head);
    TEST_ASSERT_EQUAL_UINT32(1, top.n_joined);
    TEST_ASSERT_EQUAL_UINT32(0, top.flags);
}

static void test_a_wide_graph_is_read_in_linear_work(void) {
    enum { WIDTH = 20000, SECONDS = 15 };
    uint32_t ast = 10 * R_E2;
    odin3_prov_id *parents = calloc(WIDTH, sizeof *parents);
    TEST_ASSERT_NOT_NULL(parents);
    for (uint32_t i = 0; i < WIDTH; i++) {
        parents[i] = leaf_at(ast, 1U + i);
    }
    odin3_prov_id mid[WIDTH / 1000];
    for (uint32_t k = 0; k < WIDTH / 1000; k++) {
        mid[k] = derive(&COARSEN, parents + (k * 1000U), 1000);
    }
    odin3_prov_id top = derive(&COARSEN, mid, WIDTH / 1000);
    clock_t used = read_all_records(parents[0].v, top.v);
    free(parents);
    TEST_ASSERT_TRUE_MESSAGE(used < (clock_t)SECONDS * CLOCKS_PER_SEC,
                             "wide graph read too slowly");
    TEST_ASSERT_EQUAL_UINT32(1, info_of(top).n_joined);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_source_record);
    RUN_TEST(test_leaf_without_construct_is_span_only);
    RUN_TEST(test_record_out_of_range);
    RUN_TEST(test_rewrite_and_refine_labels);
    RUN_TEST(test_hier_is_a_string_of_the_design);
    RUN_TEST(test_two_expressions_of_one_statement_are_no_join);
    RUN_TEST(test_two_blocks_are_a_join_in_key_order);
    RUN_TEST(test_an_if_and_its_arm_are_no_join);
    RUN_TEST(test_a_blend_inherited_from_one_parent_is_no_new_join);
    RUN_TEST(test_span_only_leaves_never_join);
    RUN_TEST(test_cap_and_bulk_flags);
    RUN_TEST(test_context_records_and_the_instance_join);
    RUN_TEST(test_instance_alone_has_no_data_leaf);
    RUN_TEST(test_a_body_called_from_two_blocks_is_a_join);
    RUN_TEST(test_visit_parents_in_stored_order);
    RUN_TEST(test_obj_prov_and_chain);
    RUN_TEST(test_get_construct_copies_a_row);
    RUN_TEST(test_null_arguments_on_every_reader);
    RUN_TEST(test_dead_and_compacted_objects);
    RUN_TEST(test_rows_of_one_chain_are_one_construct);
    RUN_TEST(test_labels_follow_the_table_as_it_grows);
    RUN_TEST(test_a_deep_graph_is_read_in_linear_work);
    RUN_TEST(test_a_wide_graph_is_read_in_linear_work);
    return UNITY_END();
}
