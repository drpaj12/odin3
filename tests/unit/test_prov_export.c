/*
 * test_prov_export.c — the version 2 name map text on hand-built provenance DAGs (V1.2.0 3.2, 3.3):
 * refine, rewrite and coarsen records, the join rule, the instance join by context chain, the proc
 * mux, bulk and the cap, the numbering, the AST layer modes, escaping and determinism.
 */
#include "backends/common/hash_name.h"
#include "backends/common/hdl_loc.h"
#include "backends/common/name_map.h"
#include "backends/common/prov_export.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum {
    R_MOD = 1,
    R_ALWAYS_A,
    R_ALWAYS_B,
    R_A1, /* statement in A; 2 expressions below it */
    R_E1,
    R_E2,
    R_B1, /* statement in B */
    R_INST1,
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
    uint32_t ast;
    uint32_t parent;
} row_args;

#define ADD_ROW(...) add_row((row_args){__VA_ARGS__})

static uint32_t add_row(row_args args) {
    odin3_construct_row row = {0};
    row.run = ctx.run.v;
    row.ast = args.ast;
    row.parent = args.parent;
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

static void build_table(void) {
    TEST_ASSERT_EQUAL_UINT32(R_MOD, ADD_ROW(ODIN3_AST_MODULE, 10, 0));
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_A, ADD_ROW(ODIN3_AST_ALWAYS, 20, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_ALWAYS_B, ADD_ROW(ODIN3_AST_ALWAYS, 30, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_A1, ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 40, R_ALWAYS_A));
    TEST_ASSERT_EQUAL_UINT32(R_E1, ADD_ROW(ODIN3_AST_BINARY, 50, R_A1));
    TEST_ASSERT_EQUAL_UINT32(R_E2, ADD_ROW(ODIN3_AST_BINARY, 60, R_A1));
    TEST_ASSERT_EQUAL_UINT32(R_B1, ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 70, R_ALWAYS_B));
    TEST_ASSERT_EQUAL_UINT32(R_INST1, ADD_ROW(ODIN3_AST_INSTANCE, 80, R_MOD));
    TEST_ASSERT_EQUAL_UINT32(R_INST2, ADD_ROW(ODIN3_AST_INSTANCE, 90, R_MOD));
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

/* --- writing and reading back ---------------------------------------------------------------- */

/* One renamed object named by the test: module m, a net whose short name is the real hash of its
 * full name `m/<name>@a.v:1` (the parse checks it); the texts live in a ring of buffers. */
static odin3_prov_obj object(const char *name, odin3_prov_id record) {
    static char fulls[16][64];
    static odin3_hash_name hashes[16];
    static unsigned at = 0;
    unsigned slot = at++ % 16;
    (void)snprintf(fulls[slot], sizeof fulls[slot], "m/%s@a.v:1", name);
    odin3_hash_name_make(odin3_bytes_cstr(fulls[slot]), 10, &hashes[slot]);
    return (odin3_prov_obj){.entry = {.module = odin3_bytes_cstr("m"),
                                      .kind = ODIN3_NAME_NET,
                                      .short_name = odin3_bytes_cstr(hashes[slot].text),
                                      .full = odin3_bytes_cstr(fulls[slot]),
                                      .hier = odin3_bytes_cstr("m"),
                                      .file = odin3_bytes_cstr("a.v"),
                                      .line = 1},
                            .record = record.v};
}

static odin3_prov_obj kept(const char *name, odin3_prov_id record) {
    odin3_prov_obj obj = object(name, record);
    obj.kept = true;
    obj.entry.full = (odin3_bytes){NULL, 0};
    obj.entry.hier = (odin3_bytes){NULL, 0};
    obj.entry.file = (odin3_bytes){NULL, 0};
    obj.entry.line = 0;
    return obj;
}

/* The objects to write, their number and the AST layer. */
typedef struct objlist {
    const odin3_prov_obj *objs;
    size_t count;
    odin3_construct_mode ast;
} objlist;

static odin3_status write_map(const objlist *list, odin3_strbuf *out) {
    const odin3_prov_map_req req = {design, 10, list->ast};
    return odin3_prov_map_text(&req, list->objs, list->count, out);
}

typedef struct parsed {
    odin3_strbuf text;
    odin3_name_map map;
} parsed;

/* Writes the map of objs and parses it back; the parse must succeed and the check be clean. */
static void roundtrip(parsed *out, const odin3_prov_obj *objs, size_t n, odin3_construct_mode ast) {
    odin3_strbuf_init(&out->text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, write_map(&(objlist){objs, n, ast}, &out->text));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&out->map));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_name_map_parse(&out->map, (odin3_bytes){out->text.data, out->text.len},
                                       "test.names"));
    TEST_ASSERT_EQUAL_UINT32(0, out->map.ignored);
    odin3_name_check_v2 check = odin3_name_map_check_v2(&out->map);
    TEST_ASSERT_TRUE_MESSAGE(odin3_name_check_v2_report(&check, "roundtrip"), out->text.data);
}

static void parsed_free(parsed *res) {
    odin3_name_map_free(&res->map);
    odin3_strbuf_free(&res->text);
}

/* The n-th (1-based) line of a type, NULL when there is none. */
static const odin3_nm_line *nth(const parsed *res, char type, uint32_t n) {
    for (size_t i = 0; i < res->map.lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&res->map.lines, i);
        if (line->type == type && --n == 0) {
            return line;
        }
    }
    return NULL;
}

static uint32_t count_of(const parsed *res, char type) {
    uint32_t count = 0;
    for (size_t i = 0; i < res->map.lines.len; i++) {
        count += ((const odin3_nm_line *)odin3_vec_cat(&res->map.lines, i))->type == type ? 1U : 0U;
    }
    return count;
}

/* Field f of a line as a NUL-terminated string (a small ring of buffers). */
__attribute__((noinline)) static const char *fld(const odin3_nm_line *line, uint32_t idx) {
    static char ring[8][256];
    static unsigned at = 0;
    char *buf = ring[at++ % 8];
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_TRUE(idx < line->nfields);
    size_t len = line->f[idx].len < 255 ? line->f[idx].len : 255;
    if (len > 0) {
        memcpy(buf, line->f[idx].ptr, len);
    }
    buf[len] = '\0';
    return buf;
}

/* The number at the start of text. */
static uint32_t num_of(const char *text) {
    return (uint32_t)strtoul(text, NULL, 10);
}

/* True when the comma list field contains the word. */
static bool has_flag(const odin3_nm_line *line, const char *word) {
    const char *list = fld(line, 8);
    size_t len = strlen(word);
    for (const char *at = list; *at != '\0';) {
        const char *end = strchr(at, ',');
        size_t part = end != NULL ? (size_t)(end - at) : strlen(at);
        if (part == len && strncmp(at, word, len) == 0) {
            return true;
        }
        at = end != NULL ? end + 1 : at + part;
    }
    return false;
}

/* The R line whose record is the n-th numbered: the number of the record of `rec`. */
static const odin3_nm_line *r_of_object(const parsed *res, uint32_t object_index) {
    const odin3_nm_line *obj = nth(res, 'O', object_index);
    TEST_ASSERT_NOT_NULL(obj);
    const char *recs = fld(obj, 7);
    TEST_ASSERT_EQUAL_CHAR('r', recs[0]);
    return nth(res, 'R', num_of(recs + 1));
}

/* Writes one object over `record` and returns its R line in a parsed map. */
static void one_object(parsed *res, odin3_prov_id record, odin3_construct_mode ast) {
    odin3_prov_obj obj = object("one", record);
    roundtrip(res, &obj, 1, ast);
}

/* --- classes ---------------------------------------------------------------------------------- */

static void test_origin_and_rewrite_and_refine_lines(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_id rewrite = derive(&REWRITE, &src, 1);
    parsed res;
    one_object(&res, rewrite, ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_EQUAL_UINT32(2, count_of(&res, 'R'));
    const odin3_nm_line *leaf = nth(&res, 'R', 1);
    const odin3_nm_line *derived = nth(&res, 'R', 2);
    TEST_ASSERT_EQUAL_STRING("r1", fld(leaf, 0));
    TEST_ASSERT_EQUAL_STRING("S", fld(leaf, 1));
    TEST_ASSERT_EQUAL_STRING("stmt", fld(leaf, 2));
    TEST_ASSERT_EQUAL_STRING("origin", fld(leaf, 3));
    TEST_ASSERT_EQUAL_STRING("f1:4.1-4.5", fld(leaf, 6));
    TEST_ASSERT_EQUAL_STRING("c3", fld(leaf, 7));
    TEST_ASSERT_EQUAL_STRING("D", fld(derived, 1));
    TEST_ASSERT_EQUAL_STRING("word", fld(derived, 2));
    TEST_ASSERT_EQUAL_STRING("rewrite:word", fld(derived, 3));
    TEST_ASSERT_EQUAL_STRING("r1", fld(derived, 5));
    TEST_ASSERT_EQUAL_STRING("", fld(derived, 7));
    TEST_ASSERT_FALSE(has_flag(derived, "join"));
    TEST_ASSERT_EQUAL_STRING("t1.1", fld(leaf, 4));
    TEST_ASSERT_EQUAL_STRING("t1.2", fld(derived, 4));
    parsed_free(&res);
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    one_object(&res, derive(&REFINE, two, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *refine = r_of_object(&res, 1);
    TEST_ASSERT_EQUAL_STRING("refine:stmt", fld(refine, 3));
    TEST_ASSERT_FALSE(has_flag(refine, "join")); /* the proc mux is no join */
    TEST_ASSERT_EQUAL_STRING("", fld(refine, 7));
    parsed_free(&res);
}

static void test_coarsen_with_distinct_anchors_is_a_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_B1), leaf_of(R_A1)};
    parsed res;
    one_object(&res, derive(&COARSEN, two, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *join = r_of_object(&res, 1);
    TEST_ASSERT_EQUAL_STRING("coarsen:gate", fld(join, 3));
    TEST_ASSERT_EQUAL_STRING("lut", fld(join, 2));
    TEST_ASSERT_TRUE(has_flag(join, "join"));
    /* the parents are numbered first, in stored order: b1 then a1 */
    TEST_ASSERT_EQUAL_STRING("r1,r2", fld(join, 5));
    /* J lists the two statements in key order: a1 before b1, whatever the parent order */
    const char *cons = fld(join, 7);
    TEST_ASSERT_EQUAL_CHAR('c', cons[0]);
    const odin3_nm_line *first = nth(&res, 'C', num_of(cons + 1));
    const char *comma = strchr(cons, ',');
    TEST_ASSERT_NOT_NULL(comma);
    const odin3_nm_line *second = nth(&res, 'C', num_of(comma + 2));
    TEST_ASSERT_EQUAL_STRING("40.0-40.5", fld(first, 5));
    TEST_ASSERT_EQUAL_STRING("70.0-70.5", fld(second, 5));
    parsed_free(&res);
}

static void test_coarsen_without_distinct_anchors_is_not_a_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_E1), leaf_of(R_E2)}; /* two expressions of one statement */
    parsed res;
    one_object(&res, derive(&COARSEN, two, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *line = r_of_object(&res, 1);
    TEST_ASSERT_EQUAL_STRING("coarsen:gate", fld(line, 3));
    TEST_ASSERT_FALSE(has_flag(line, "join"));
    TEST_ASSERT_EQUAL_STRING("", fld(line, 7));
    parsed_free(&res);
}

static void test_inherited_blend_is_not_a_new_join(void) {
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    odin3_prov_id blend = derive(&COARSEN, two, 2);
    odin3_prov_id again[2] = {blend, leaf_of(R_E1)};
    parsed res;
    one_object(&res, derive(&COARSEN, again, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *line = r_of_object(&res, 1);
    TEST_ASSERT_FALSE(has_flag(line, "join"));
    parsed_free(&res);
}

/* --- inlined calls and instances ------------------------------------------------------------- */

static void test_function_body_called_from_two_blocks_is_a_join(void) {
    uint32_t site_a = ADD_ROW(ODIN3_AST_CALL, 900, R_A1);
    uint32_t site_b = ADD_ROW(ODIN3_AST_CALL, 910, R_B1);
    uint32_t body = ADD_ROW(ODIN3_AST_FUNCTION_DECL, 920, R_MOD);
    uint32_t stmt = ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 930, body);
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
    }
    parsed res;
    one_object(&res, derive(&COARSEN, leaves, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *join = r_of_object(&res, 1);
    TEST_ASSERT_TRUE(has_flag(join, "join"));
    const odin3_nm_line *call_leaf = nth(&res, 'R', 1);
    TEST_ASSERT_EQUAL_STRING("S", fld(call_leaf, 1));
    TEST_ASSERT_TRUE(strstr(fld(call_leaf, 8), "call=c") != NULL); /* a call-site record */
    parsed_free(&res);
}

/* True when a context record (dir `context:`) carries ctx=1. */
static bool context_record_has_ctx(const parsed *res) {
    for (uint32_t k = 1; k <= count_of(res, 'R'); k++) {
        const odin3_nm_line *line = nth(res, 'R', k);
        if (strncmp(fld(line, 3), "context:", 8) == 0) {
            return has_flag(line, "ctx=1");
        }
    }
    return false;
}

static void test_instance_join_by_context_chain(void) {
    odin3_prov_id orig = leaf_of(R_A1);
    odin3_prov_id pair1[2] = {orig, leaf_of(R_INST1)};
    odin3_prov_id pair2[2] = {orig, leaf_of(R_INST2)};
    odin3_prov_id flat1 = {0};
    odin3_prov_id flat2 = {0};
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){pair1, 2}, &flat1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){pair2, 2}, &flat2));
    odin3_prov_id both[2] = {flat1, flat2};
    parsed res;
    one_object(&res, derive(&COARSEN, both, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *merged = r_of_object(&res, 1);
    TEST_ASSERT_TRUE(has_flag(merged, "join=instance"));
    TEST_ASSERT_FALSE(has_flag(merged, "join")); /* one anchor: the instances never enter J */
    TEST_ASSERT_TRUE(context_record_has_ctx(&res));
    parsed_free(&res);
}

static odin3_prov_id wide_bulk_record(void);

/* --- instance joins over several levels (memoised per record) -------------------------------- */

static odin3_prov_id context_of(odin3_prov_id orig, uint32_t inst_row) {
    odin3_prov_id pair[2] = {orig, leaf_of(inst_row)};
    odin3_prov_id out = {0};
    odin3_prov_begin_op_class(&ctx, &CONTEXT1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){pair, 2}, &out));
    return out;
}

static void test_instance_join_found_through_nested_coarsen_records(void) {
    odin3_prov_id orig = leaf_of(R_A1);
    odin3_prov_id both[2] = {context_of(orig, R_INST1), context_of(orig, R_INST2)};
    odin3_prov_id inner = derive(&COARSEN, both, 2);
    odin3_prov_id outer_in[2] = {inner, leaf_of(R_B1)};
    odin3_prov_id outer = derive(&COARSEN, outer_in, 2);
    odin3_prov_id top_in[2] = {outer, leaf_of(R_E1)};
    parsed res;
    one_object(&res, derive(&COARSEN, top_in, 2), ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_TRUE(has_flag(r_of_object(&res, 1), "join=instance")); /* the top record */
    for (uint32_t k = 1; k <= count_of(&res, 'R'); k++) {
        const odin3_nm_line *line = nth(&res, 'R', k);
        if (strncmp(fld(line, 3), "coarsen:", 8) == 0) {
            TEST_ASSERT_TRUE(has_flag(line, "join=instance")); /* inner, outer and top alike */
        }
    }
    parsed_free(&res);
}

static void test_one_context_chain_twice_is_no_instance_join(void) {
    odin3_prov_id orig = leaf_of(R_A1);
    odin3_prov_id both[2] = {context_of(orig, R_INST1), context_of(orig, R_INST1)};
    parsed res;
    one_object(&res, derive(&COARSEN, both, 2), ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_FALSE(has_flag(r_of_object(&res, 1), "join=instance"));
    parsed_free(&res);
}

static void test_instance_join_not_found_when_chains_hold_different_anchors(void) {
    odin3_prov_id left = context_of(leaf_of(R_A1), R_INST1);
    odin3_prov_id right = context_of(leaf_of(R_B1), R_INST2);
    odin3_prov_id both[2] = {left, right};
    parsed res;
    one_object(&res, derive(&COARSEN, both, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *line = r_of_object(&res, 1);
    TEST_ASSERT_FALSE(has_flag(line, "join=instance"));
    TEST_ASSERT_TRUE(has_flag(line, "join")); /* two statements */
    parsed_free(&res);
}

/* --- refusals and counts ---------------------------------------------------------------------- */

static unsigned g_errors;
static unsigned g_warnings;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    g_errors += level == ODIN3_LOG_ERROR ? 1U : 0U;
    g_warnings += level == ODIN3_LOG_WARN ? 1U : 0U;
}

static void test_derived_record_without_a_class_is_refused_with_a_message(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_id classless = {0};
    odin3_prov_begin_op(&ctx); /* an operation with no class */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive(&ctx, (odin3_prov_list){&src, 1}, &classless));
    odin3_opclass cls;
    TEST_ASSERT_FALSE(odin3_prov_class_of(design, classless, &cls));
    odin3_prov_obj obj = object("nocls", classless);
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    g_errors = 0;
    odin3_log_set_sink(count_sink, NULL);
    odin3_status st = write_map(&(objlist){&obj, 1, ODIN3_CONSTRUCT_MODE_REFS}, &out);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st);
    TEST_ASSERT_TRUE(g_errors >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)out.len); /* no text, so no map that fails its check */
    odin3_strbuf_free(&out);
}

static void test_dropped_kept_names_are_counted_in_a_warning(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_obj dupes[3] = {kept("same", src), kept("same", src), kept("same", src)};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    g_warnings = 0;
    odin3_log_set_sink(count_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          write_map(&(objlist){dupes, 3, ODIN3_CONSTRUCT_MODE_REFS}, &out));
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, g_warnings); /* one warning for both drops */
    odin3_strbuf_free(&out);
}

/* A SOURCE record with n distinct locations in file a.v. */
static odin3_prov_id many_locs(uint32_t n) {
    odin3_srcloc locs[12];
    for (uint32_t i = 0; i < n; i++) {
        locs[i] = (odin3_srcloc){intern("a.v"), 100 + i, 1, 100 + i, 5, 0};
    }
    odin3_prov_origin origin = {locs, n, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &id));
    return id;
}

static uint32_t items_of(const char *list) {
    uint32_t items = list[0] == '\0' ? 0U : 1U;
    for (const char *at = list; *at != '\0'; at++) {
        items += *at == '|' ? 1U : 0U;
    }
    return items;
}

static void test_capped_is_written_only_when_the_cap_bites(void) {
    parsed res;
    one_object(&res, many_locs(ODIN3_HDL_LOC_MAX), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *at_cap = r_of_object(&res, 1);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_HDL_LOC_MAX, items_of(fld(at_cap, 6)));
    TEST_ASSERT_FALSE(has_flag(at_cap, "capped"));
    parsed_free(&res);
    one_object(&res, many_locs(ODIN3_HDL_LOC_MAX + 3U), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *over = r_of_object(&res, 1);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_HDL_LOC_MAX, items_of(fld(over, 6)));
    TEST_ASSERT_TRUE(has_flag(over, "capped"));
    parsed_free(&res);
}

/* --- every written map passes its own check, and a failing check is reported ------------------ */

static void test_written_maps_pass_the_check_and_a_bad_one_is_reported(void) {
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    odin3_prov_obj objs[3] = {object("a", derive(&COARSEN, two, 2)),
                              object("b", wide_bulk_record()), kept("c", leaf_of(R_E1))};
    for (int ast = ODIN3_CONSTRUCT_MODE_OFF; ast <= ODIN3_CONSTRUCT_MODE_FULL; ast++) {
        parsed res;
        roundtrip(&res, objs, 3, (odin3_construct_mode)ast); /* parse + check inside */
        odin3_name_check_v2 check = odin3_name_map_check_v2(&res.map);
        TEST_ASSERT_TRUE(odin3_name_check_v2_report(&check, "test"));
        TEST_ASSERT_EQUAL_UINT32(0, check.ignored);
        parsed_free(&res);
    }
    /* a map with a reference past the last record fails, and the report says so */
    const char *bad = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=none\n"
                      "R\tr1\tD\tword\trewrite:word\tt1.1\tr9\t\t\t\n"
                      "T\tt1\tp\t\t\n";
    odin3_name_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_parse(&map, odin3_bytes_cstr(bad), "bad.names"));
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    g_errors = 0;
    odin3_log_set_sink(count_sink, NULL);
    TEST_ASSERT_FALSE(odin3_name_check_v2_report(&check, "test"));
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_TRUE(g_errors >= 1);
    odin3_name_map_free(&map);
}

/* --- bulk and the cap ------------------------------------------------------------------------- */

/* How many constructs the cons list names. */
static uint32_t list_members(const char *list) {
    uint32_t members = list[0] == '\0' ? 0U : 1U;
    for (const char *at = list; *at != '\0'; at++) {
        members += *at == ',' ? 1U : 0U;
    }
    return members;
}

enum { WIDE = 20 };

/* The record of a bulk operation over WIDE statements, each with a construct of its own. */
static odin3_prov_id wide_bulk_record(void) {
    odin3_prov_id parents[WIDE];
    for (uint32_t i = 0; i < WIDE; i++) {
        uint32_t row = ADD_ROW(ODIN3_AST_BLOCKING_ASSIGN, 10 * (R_FIRST_FREE + i), R_ALWAYS_B);
        parents[i] = leaf_of(row);
    }
    return derive(&BULK, parents, WIDE);
}

static void test_bulk_over_the_cap_has_the_flags(void) {
    parsed res;
    one_object(&res, wide_bulk_record(), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *line = r_of_object(&res, 1);
    static const char *const WANT[] = {"join", "lossy=bulk", "lossy=cap", "n=20", "bulk"};
    bool all = true;
    for (size_t k = 0; k < sizeof WANT / sizeof WANT[0]; k++) {
        all = all && has_flag(line, WANT[k]);
    }
    TEST_ASSERT_TRUE_MESSAGE(all, fld(line, 8));
    parsed_free(&res);
}

static void test_bulk_over_the_cap_lists_sixteen(void) {
    parsed res;
    one_object(&res, wide_bulk_record(), ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROV_JOIN_CAP, list_members(fld(r_of_object(&res, 1), 7)));
    parsed_free(&res);
}

static void test_small_bulk_run_has_no_cap(void) {
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    parsed res;
    one_object(&res, derive(&BULK, two, 2), ODIN3_CONSTRUCT_MODE_REFS);
    const odin3_nm_line *line = r_of_object(&res, 1);
    TEST_ASSERT_TRUE(has_flag(line, "lossy=bulk"));
    TEST_ASSERT_FALSE(has_flag(line, "lossy=cap"));
    TEST_ASSERT_TRUE(has_flag(line, "n=2"));
    parsed_free(&res);
}

/* --- numbering, determinism, modes ------------------------------------------------------------ */

static void test_numbering_parents_first_and_constructs_after_chains(void) {
    odin3_prov_id src = leaf_of(R_E1);
    odin3_prov_id mid = derive(&REWRITE, &src, 1);
    odin3_prov_id top = derive(&REWRITE, &mid, 1);
    parsed res;
    one_object(&res, top, ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_EQUAL_UINT32(3, count_of(&res, 'R'));
    TEST_ASSERT_EQUAL_STRING("", fld(nth(&res, 'R', 1), 5));
    TEST_ASSERT_EQUAL_STRING("r1", fld(nth(&res, 'R', 2), 5));
    TEST_ASSERT_EQUAL_STRING("r2", fld(nth(&res, 'R', 3), 5));
    /* e1's chain: module, always A, a1, e1 -- the module first */
    TEST_ASSERT_EQUAL_UINT32(4, count_of(&res, 'C'));
    TEST_ASSERT_EQUAL_STRING("MODULE", fld(nth(&res, 'C', 1), 1));
    TEST_ASSERT_EQUAL_STRING("", fld(nth(&res, 'C', 1), 3));
    TEST_ASSERT_EQUAL_STRING("c1", fld(nth(&res, 'C', 2), 3));
    TEST_ASSERT_EQUAL_STRING("c3", fld(nth(&res, 'C', 4), 3));
    TEST_ASSERT_EQUAL_STRING("BINARY", fld(nth(&res, 'C', 4), 1));
    TEST_ASSERT_EQUAL_STRING("c4", fld(nth(&res, 'R', 1), 7));
    parsed_free(&res);
}

static void test_equal_designs_and_any_object_order_give_equal_bytes(void) {
    odin3_prov_id two[2] = {leaf_of(R_A1), leaf_of(R_B1)};
    odin3_prov_obj objs[3] = {object("bbb", derive(&COARSEN, two, 2)), object("aaa", leaf_of(R_E1)),
                              kept("zeta", leaf_of(R_B1))};
    odin3_prov_obj reversed[3] = {objs[2], objs[1], objs[0]};
    odin3_strbuf first;
    odin3_strbuf second;
    odin3_strbuf_init(&first);
    odin3_strbuf_init(&second);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          write_map(&(objlist){objs, 3, ODIN3_CONSTRUCT_MODE_REFS}, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          write_map(&(objlist){reversed, 3, ODIN3_CONSTRUCT_MODE_REFS}, &second));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)first.len, (uint32_t)second.len);
    TEST_ASSERT_EQUAL_MEMORY(first.data, second.data, first.len);
    /* O lines (sorted by short name) before the N line */
    parsed res;
    roundtrip(&res, objs, 3, ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_EQUAL_CHAR('O', ((const odin3_nm_line *)odin3_vec_cat(&res.map.lines, 0))->type);
    TEST_ASSERT_EQUAL_CHAR('O', ((const odin3_nm_line *)odin3_vec_cat(&res.map.lines, 1))->type);
    TEST_ASSERT_EQUAL_CHAR('N', ((const odin3_nm_line *)odin3_vec_cat(&res.map.lines, 2))->type);
    TEST_ASSERT_TRUE(strcmp(fld(nth(&res, 'O', 1), 2), fld(nth(&res, 'O', 2), 2)) < 0);
    parsed_free(&res);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

static void test_modes_none_constructs_full(void) {
    odin3_prov_id src = leaf_of(R_E1);
    odin3_prov_obj obj = object("one", src);
    parsed res;
    roundtrip(&res, &obj, 1, ODIN3_CONSTRUCT_MODE_OFF);
    TEST_ASSERT_EQUAL_UINT32(0, count_of(&res, 'C'));
    TEST_ASSERT_EQUAL_UINT32(0, count_of(&res, 'F') - 1U); /* only the file of the origin */
    TEST_ASSERT_EQUAL_STRING("", fld(nth(&res, 'R', 1), 7));
    TEST_ASSERT_TRUE(strstr(res.text.data, "ast=none") != NULL);
    parsed_free(&res);
    roundtrip(&res, &obj, 1, ODIN3_CONSTRUCT_MODE_CONSTRUCTS);
    TEST_ASSERT_TRUE(strstr(res.text.data, "ast=constructs") != NULL);
    /* the expression is resolved to its statement: module, always A, a1 */
    TEST_ASSERT_EQUAL_UINT32(3, count_of(&res, 'C'));
    TEST_ASSERT_EQUAL_STRING("stmt", fld(nth(&res, 'R', 1), 2));
    TEST_ASSERT_EQUAL_STRING("c3", fld(nth(&res, 'R', 1), 7));
    parsed_free(&res);
    roundtrip(&res, &obj, 1, ODIN3_CONSTRUCT_MODE_FULL);
    TEST_ASSERT_TRUE(strstr(res.text.data, "ast=full") != NULL);
    TEST_ASSERT_EQUAL_UINT32(R_FIRST_FREE - 1U, count_of(&res, 'C')); /* every row of the table */
    parsed_free(&res);
}

/* --- escaping, malformed input, errors -------------------------------------------------------- */

static void test_escaping_of_list_separators_and_hier(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_id out = {0};
    odin3_prov_begin_op_class(&ctx, &REWRITE);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive_hier(&ctx, (odin3_prov_list){&src, 1},
                                                           intern("top/g[a,b|c\"]"), &out));
    odin3_prov_obj obj = object("one", out);
    obj.entry.module = odin3_bytes_cstr("mo,d|ule");
    parsed res;
    roundtrip(&res, &obj, 1, ODIN3_CONSTRUCT_MODE_REFS);
    TEST_ASSERT_NULL(strstr(res.text.data, "mo,d"));
    TEST_ASSERT_NOT_NULL(strstr(res.text.data, "mo%2Cd%7Cule"));
    TEST_ASSERT_NOT_NULL(strstr(res.text.data, "hier=top/g[a%2Cb%7Cc\"]"));
    const odin3_name_entry *entry = odin3_vec_cat(&res.map.entries, 0);
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)entry->module.len);
    TEST_ASSERT_EQUAL_MEMORY("mo,d|ule", entry->module.ptr, 8);
    parsed_free(&res);
}

static void test_duplicate_objects_and_bad_records(void) {
    odin3_prov_id src = leaf_of(R_A1);
    odin3_prov_obj dup[2] = {object("same", src), object("same", src)};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          write_map(&(objlist){dup, 2, ODIN3_CONSTRUCT_MODE_REFS}, &out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)out.len);
    odin3_prov_obj dupes[2] = {kept("same", src), kept("same", leaf_of(R_B1))};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          write_map(&(objlist){dupes, 2, ODIN3_CONSTRUCT_MODE_REFS}, &out));
    uint32_t lines = 0;
    for (const char *at = out.data; (at = strstr(at, "\nN\t")) != NULL; at++) {
        lines++;
    }
    TEST_ASSERT_EQUAL_UINT32(1, lines); /* the second N line with the key is dropped */
    odin3_strbuf_clear(&out);
    odin3_prov_obj bad = object("bad", (odin3_prov_id){odin3_prov_end(design) + 5});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          write_map(&(objlist){&bad, 1, ODIN3_CONSTRUCT_MODE_REFS}, &out));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)out.len);
    odin3_prov_obj none = object("none", (odin3_prov_id){0}); /* an object with no record */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          write_map(&(objlist){&none, 1, ODIN3_CONSTRUCT_MODE_REFS}, &out));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "\n"));
    odin3_strbuf_free(&out);
}

static void test_resolve_mode_is_at_most_what_the_read_kept(void) {
    odin3_construct_mode got = ODIN3_CONSTRUCT_MODE_FULL;
    /* nothing kept: only the bare option works, with ast=none */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_map_resolve(design, ODIN3_PROV_MAP_ANY, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_OFF, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_map_resolve(design, ODIN3_PROV_MAP_REFS, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_map_resolve(design, ODIN3_PROV_MAP_NONE, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, ctx.run, ODIN3_CONSTRUCT_MODE_REFS));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_map_resolve(design, ODIN3_PROV_MAP_ANY, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_map_resolve(design, ODIN3_PROV_MAP_CONSTRUCTS, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_CONSTRUCTS, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_map_resolve(design, ODIN3_PROV_MAP_FULL, "t", &got));
}

static void test_resolve_full_kept_allows_all_and_clips_the_bare_option(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_construct_mark_run(design, ctx.run, ODIN3_CONSTRUCT_MODE_FULL));
    odin3_construct_mode got = ODIN3_CONSTRUCT_MODE_OFF;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_map_resolve(design, ODIN3_PROV_MAP_FULL, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_FULL, got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_map_resolve(design, ODIN3_PROV_MAP_ANY, "t", &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS, got); /* bare is refs, never more */
}

static void test_option_words_are_the_three_modes(void) {
    odin3_prov_map_mode mode = ODIN3_PROV_MAP_NONE;
    TEST_ASSERT_TRUE(odin3_prov_map_word(odin3_bytes_cstr("refs"), &mode));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_MAP_REFS, mode);
    TEST_ASSERT_TRUE(odin3_prov_map_word(odin3_bytes_cstr("constructs"), &mode));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_MAP_CONSTRUCTS, mode);
    TEST_ASSERT_TRUE(odin3_prov_map_word(odin3_bytes_cstr("full"), &mode));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_MAP_FULL, mode);
}

static void test_other_option_words_are_not_modes(void) {
    odin3_prov_map_mode mode = ODIN3_PROV_MAP_NONE;
    TEST_ASSERT_FALSE(odin3_prov_map_word(odin3_bytes_cstr("refsx"), &mode));
    TEST_ASSERT_FALSE(odin3_prov_map_word(odin3_bytes_cstr("--name-map"), &mode));
    TEST_ASSERT_FALSE(odin3_prov_map_word((odin3_bytes){NULL, 0}, &mode));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_MAP_NONE, mode);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_origin_and_rewrite_and_refine_lines);
    RUN_TEST(test_coarsen_with_distinct_anchors_is_a_join);
    RUN_TEST(test_coarsen_without_distinct_anchors_is_not_a_join);
    RUN_TEST(test_inherited_blend_is_not_a_new_join);
    RUN_TEST(test_function_body_called_from_two_blocks_is_a_join);
    RUN_TEST(test_instance_join_by_context_chain);
    RUN_TEST(test_instance_join_found_through_nested_coarsen_records);
    RUN_TEST(test_one_context_chain_twice_is_no_instance_join);
    RUN_TEST(test_instance_join_not_found_when_chains_hold_different_anchors);
    RUN_TEST(test_derived_record_without_a_class_is_refused_with_a_message);
    RUN_TEST(test_dropped_kept_names_are_counted_in_a_warning);
    RUN_TEST(test_capped_is_written_only_when_the_cap_bites);
    RUN_TEST(test_written_maps_pass_the_check_and_a_bad_one_is_reported);
    RUN_TEST(test_bulk_over_the_cap_has_the_flags);
    RUN_TEST(test_bulk_over_the_cap_lists_sixteen);
    RUN_TEST(test_small_bulk_run_has_no_cap);
    RUN_TEST(test_numbering_parents_first_and_constructs_after_chains);
    RUN_TEST(test_equal_designs_and_any_object_order_give_equal_bytes);
    RUN_TEST(test_modes_none_constructs_full);
    RUN_TEST(test_escaping_of_list_separators_and_hier);
    RUN_TEST(test_duplicate_objects_and_bad_records);
    RUN_TEST(test_resolve_mode_is_at_most_what_the_read_kept);
    RUN_TEST(test_resolve_full_kept_allows_all_and_clips_the_bare_option);
    RUN_TEST(test_option_words_are_the_three_modes);
    RUN_TEST(test_other_option_words_are_not_modes);
    return UNITY_END();
}
