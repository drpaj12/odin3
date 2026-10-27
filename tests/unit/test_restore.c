/*
 * test_restore.c — unit tests for the restore of a version 2 name map (V1.2.0 4.5, Task 9): the
 * pass runs, records, classes, construct rows, extra lines and the mutating slot, the binding of
 * objects, a map that fails restores nothing, and the design copy. Run under ASan.
 */
#include "ast/kinds.h"
#include "ir/check.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/prov.h"
#include "ir/restore.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEXT_BUF = 8192, MSG_LEN = 256, MSG_MAX = 16 };

static const char HEADER[] = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n";
/* A tiny design: two statements of one always block under a module, a join of them, a bulk
 * coarsen over that, a context record, and one A, one Q and one K line. */
static const char BODY[] = "O\tm\tnet\th42xiy3yiju\ta@t.v:1\t\tt.v\t1\tr1\tgate\n"
                           "N\tm\tnet\tp\tr2\tgate\n"
                           "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:3.1-3.9\tc3\t\n"
                           "R\tr2\tS\tstmt\torigin\tt1.1\t\tf1:4.1-4.9\tc4\thier=top/u\n"
                           "R\tr3\tD\tgate\trewrite:stmt\tt2.1\tr1\t\t\t\n"
                           "R\tr4\tD\tgate\tcoarsen:stmt\tt2.1\tr1,r2\t\tc3,c4\tjoin\n"
                           "R\tr5\tD\tlut\tcoarsen:gate\tt2.2\tr3,r4\t\t\tbulk\n"
                           "R\tr6\tD\tgate\tcontext:module\tt2.3\tr1,r2\t\t\tctx=1\n"
                           "C\tc1\tMODULE\tmodule\t\tf1\t1.1-9.10\ttop\n"
                           "C\tc2\tALWAYS\tblock\tc1\tf1\t2.1-8.4\t\n"
                           "C\tc3\tNONBLOCKING_ASSIGN\tstmt\tc2\tf1\t3.1-3.9\t\n"
                           "C\tc4\tNONBLOCKING_ASSIGN\tstmt\tc2\tf1\t4.1-4.9\t\n"
                           "F\tf1\tt.v\n"
                           "T\tt1\tread_verilog\t\t\n"
                           "T\tt2\topt\t-full\tpasses=2\n"
                           "A\tatom1\tr4,r5\t-\n"
                           "Q\tr4\t\topt\t\t\t\t\t\t\t\tb\t-\n"
                           "K\tr4\tm\t\t\t\tb\tt2\n";

static odin3_design *design;
static odin3_name_map map;
static char warnings[MSG_MAX][MSG_LEN];
static unsigned warn_count;
static unsigned error_count;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_WARN && warn_count < MSG_MAX) {
        (void)snprintf(warnings[warn_count++], MSG_LEN, "%s", msg);
    } else if (level == ODIN3_LOG_ERROR) {
        error_count++;
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

void setUp(void) {
    warn_count = 0;
    error_count = 0;
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_blif"), &ctx));
    memset(&map, 0, sizeof map);
}

void tearDown(void) {
    odin3_name_map_free(&map);
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static void parse(const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&map));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_parse(&map, odin3_bytes_cstr(text), "t.names"));
}

static odin3_restore *restore_body(const char *header, const char *body) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", header, body);
    parse(text);
    odin3_restore *restore = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_restore(design, &map, "t.names", &restore));
    TEST_ASSERT_NOT_NULL(restore);
    return restore;
}

/* The record IDs the six R lines of BODY became: the first restored record is r1. */
static odin3_prov_id rec(uint32_t first, uint32_t n) {
    return (odin3_prov_id){first + n - 1U};
}

static const char *text_of_str(uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

static void test_restores_the_pass_runs_flagged_restored(void) {
    uint32_t runs_before = odin3_passrun_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    TEST_ASSERT_EQUAL_UINT32(runs_before + 2U, odin3_passrun_end(design));
    odin3_passrun_id t1 = {runs_before};
    odin3_passrun_id t2 = {runs_before + 1U};
    TEST_ASSERT_FALSE(odin3_passrun_restored(design, (odin3_passrun_id){1}));
    TEST_ASSERT_TRUE(odin3_passrun_restored(design, t1));
    TEST_ASSERT_TRUE(odin3_passrun_restored(design, t2));
    TEST_ASSERT_EQUAL_STRING("opt", text_of_str(odin3_passrun_name(design, t2)));
    TEST_ASSERT_EQUAL_STRING("-full", text_of_str(odin3_passrun_args(design, t2)));
    TEST_ASSERT_EQUAL_STRING("passes=2", text_of_str(odin3_passrun_info(design, t2)));
    odin3_restore_free(restore);
}

static void test_restores_the_records_with_their_origins_and_parents(void) {
    uint32_t runs_before = odin3_passrun_end(design);
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    TEST_ASSERT_EQUAL_UINT32(first + 6U, odin3_prov_end(design));
    const odin3_prov_record *r1 = odin3_prov_get(design, rec(first, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, r1->kind);
    TEST_ASSERT_EQUAL_UINT32(runs_before, r1->run.v);
    TEST_ASSERT_EQUAL_UINT32(1, r1->n_locs);
    TEST_ASSERT_EQUAL_UINT32(3, r1->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(9, r1->locs[0].end_col);
    TEST_ASSERT_EQUAL_UINT32(3, r1->ast); /* the remap: ast is the C line number */
    const odin3_prov_record *r2 = odin3_prov_get(design, rec(first, 2));
    TEST_ASSERT_EQUAL_STRING("top/u", text_of_str(r2->hier));
    const odin3_prov_record *r4 = odin3_prov_get(design, rec(first, 4));
    TEST_ASSERT_EQUAL_UINT32(2, r4->parents.count);
    TEST_ASSERT_EQUAL_UINT32(rec(first, 2).v, r4->parents.ids[1].v);
    TEST_ASSERT_EQUAL_UINT32(1, r4->op);
    odin3_restore_free(restore);
}

static void test_restores_the_classes_as_defaults_and_overrides(void) {
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    odin3_opclass cls;
    TEST_ASSERT_TRUE(odin3_prov_class_of(design, rec(first, 3), &cls));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_REWRITE, cls.dir);
    TEST_ASSERT_TRUE(odin3_prov_class_of(design, rec(first, 4), &cls)); /* an override */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_COARSEN, cls.dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_STMT, cls.from_level);
    TEST_ASSERT_EQUAL_INT(ODIN3_LEVEL_GATE, cls.to_level);
    TEST_ASSERT_FALSE(cls.bulk);
    odin3_restore_free(restore);
}

static void test_restores_the_bulk_flag_and_the_context_tail(void) {
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    odin3_opclass cls;
    TEST_ASSERT_TRUE(odin3_prov_class_of(design, rec(first, 5), &cls));
    TEST_ASSERT_TRUE(cls.bulk);
    TEST_ASSERT_TRUE(odin3_prov_class_of(design, rec(first, 6), &cls));
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_CONTEXT, cls.dir);
    TEST_ASSERT_EQUAL_UINT8(1, cls.context_tail);
    odin3_restore_free(restore);
}

static void test_construct_rows_are_remapped_per_run(void) {
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    uint32_t t1 = odin3_passrun_end(design) - 2U;
    uint32_t row = odin3_construct_of_record(design, rec(first, 1));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, row);
    const odin3_construct_row *stmt = odin3_construct_get(design, row);
    TEST_ASSERT_EQUAL_UINT32(t1, stmt->run);
    TEST_ASSERT_EQUAL_UINT32(3, stmt->ast);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_NONBLOCKING_ASSIGN, stmt->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_LEVEL_STMT, stmt->level);
    const odin3_construct_row *always = odin3_construct_get(design, stmt->parent);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_ALWAYS, always->kind);
    const odin3_construct_row *module = odin3_construct_get(design, always->parent);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_MODULE, module->kind);
    TEST_ASSERT_EQUAL_UINT32(0, module->parent);
    TEST_ASSERT_EQUAL_STRING("top", odin3_strtab_get(odin3_design_strtab(design), module->name));
    TEST_ASSERT_EQUAL_STRING("t.v", odin3_strtab_get(odin3_design_strtab(design), stmt->file));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS,
                          odin3_construct_run_mode(design, (odin3_passrun_id){t1}));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    odin3_restore_free(restore);
}

static void test_extra_lines_names_and_the_slot(void) {
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    const odin3_extra_table *extra = odin3_design_extra(design);
    TEST_ASSERT_NOT_NULL(extra);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_extra_count(extra));
    char want[64];
    (void)snprintf(want, sizeof want, "A\tatom1\tr%u,r%u\t-", (unsigned)rec(first, 4).v,
                   (unsigned)rec(first, 5).v);
    odin3_bytes line = odin3_extra_line(extra, 0);
    TEST_ASSERT_EQUAL_UINT(strlen(want), line.len);
    TEST_ASSERT_EQUAL_MEMORY(want, line.ptr, line.len);
    uint32_t t2 = odin3_passrun_end(design) - 1U;
    line = odin3_extra_line(extra, 2); /* the K line's stale t2 is the design's run */
    (void)snprintf(want, sizeof want, "t%u", (unsigned)t2);
    TEST_ASSERT_EQUAL_MEMORY(want, (const char *)line.ptr + line.len - strlen(want), strlen(want));
    /* the slot: restored runs and readers and writers never set it; the first other pass does */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_extra_mutating(extra));
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("write_blif"), &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_json"), &ctx));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_extra_mutating(extra));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("opt"), &ctx));
    TEST_ASSERT_EQUAL_UINT32(ctx.run.v, odin3_extra_mutating(extra));
    odin3_pass_ctx later;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("flatten"), &later));
    TEST_ASSERT_EQUAL_UINT32(ctx.run.v, odin3_extra_mutating(extra)); /* the first one is kept */
    odin3_design *copy = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_clone(design, &copy));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_extra_count(odin3_design_extra(copy)));
    TEST_ASSERT_EQUAL_UINT32(ctx.run.v, odin3_extra_mutating(odin3_design_extra(copy)));
    TEST_ASSERT_TRUE(odin3_passrun_restored(copy, (odin3_passrun_id){t2}));
    odin3_design_destroy(copy);
    odin3_restore_free(restore);
}

static void test_a_design_with_no_restore_has_no_table_and_no_slot(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_NULL(odin3_design_extra(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("opt"), &ctx));
    TEST_ASSERT_NULL(odin3_design_extra(design));
    TEST_ASSERT_FALSE(odin3_passrun_restored(design, ctx.run));
}

static void test_binds_objects_by_entry_and_by_name(void) {
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, BODY);
    const odin3_name_entry *entry = odin3_name_map_find(&map, odin3_bytes_cstr("m"), ODIN3_NAME_NET,
                                                        odin3_bytes_cstr("h42xiy3yiju"));
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT32(rec(first, 1).v, odin3_restore_entry_record(restore, entry));
    TEST_ASSERT_EQUAL_UINT32(rec(first, 2).v,
                             odin3_restore_named_record(restore, odin3_bytes_cstr("m"),
                                                        ODIN3_NAME_NET, odin3_bytes_cstr("p")));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_restore_named_record(restore, odin3_bytes_cstr("m"),
                                                           ODIN3_NAME_NODE, odin3_bytes_cstr("p")));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_restore_named_record(restore, odin3_bytes_cstr("m"),
                                                           ODIN3_NAME_NET, odin3_bytes_cstr("q")));
    odin3_restore_free(restore);
}

static void test_a_recs_list_of_several_ids_warns_and_binds_the_first(void) {
    char body[TEXT_BUF];
    (void)snprintf(body, sizeof body, "N\tm\tnet\tp\tr2,r1\tgate\n%s", strchr(BODY, 'R'));
    uint32_t first = odin3_prov_end(design);
    odin3_restore *restore = restore_body(HEADER, body);
    TEST_ASSERT_EQUAL_UINT32(rec(first, 2).v,
                             odin3_restore_named_record(restore, odin3_bytes_cstr("m"),
                                                        ODIN3_NAME_NET, odin3_bytes_cstr("p")));
    TEST_ASSERT_TRUE(warn_count >= 1);
    TEST_ASSERT_NOT_NULL(strstr(warnings[0], "t.names:2: name map restore: recs lists several"));
    odin3_restore_free(restore);
}

/* Each plant makes the plan fail: the design is as the reader made it (no run, record, row or
 * extra table), with a located error. */
typedef struct plant {
    const char *body;
    const char *needle;
} plant;

static void expect_refused(plant plant_in) {
    const char *body = plant_in.body;
    const char *needle = plant_in.needle;
    uint32_t runs = odin3_passrun_end(design);
    uint32_t recs = odin3_prov_end(design);
    uint32_t rows = odin3_construct_end(design);
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", HEADER, body);
    parse(text);
    odin3_restore *restore = NULL;
    error_count = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE,
                                  odin3_design_restore(design, &map, "t.names", &restore), needle);
    TEST_ASSERT_NULL(restore);
    TEST_ASSERT_TRUE_MESSAGE(error_count > 0, needle);
    TEST_ASSERT_EQUAL_UINT32(runs, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_UINT32(recs, odin3_prov_end(design));
    TEST_ASSERT_EQUAL_UINT32(rows, odin3_construct_end(design));
    TEST_ASSERT_NULL(odin3_design_extra(design));
    odin3_name_map_free(&map);
    memset(&map, 0, sizeof map);
}

static void test_a_map_that_fails_restores_nothing(void) {
    static const char TAIL[] = "F\tf1\tt.v\nT\tt1\tread_verilog\t\t\n";
    char body[TEXT_BUF];
    /* a parent numbered after its child (names --check's bad_order) */
    (void)snprintf(body, sizeof body,
                   "R\tr1\tD\tgate\trewrite:stmt\tt1.1\tr2\t\t\t\nR\tr2\tS\tstmt\torigin\tt1."
                   "1\t\tf1:1.1-1.2\t\t\n%s",
                   TAIL);
    expect_refused((plant){body, "bad_order"});
    /* a D record with no dir (a version 1 era tool) */
    (void)snprintf(
        body, sizeof body,
        "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\t\t\nR\tr2\tD\tgate\t\tt1.2\tr1\t\t\t\n%s",
        TAIL);
    expect_refused((plant){body, "no dir"});
    /* a class the table rejects: bulk on a rewrite */
    (void)snprintf(body, sizeof body,
                   "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\t\t\nR\tr2\tD\tgate\trewrite:"
                   "stmt\tt1.2\tr1\t\t\tbulk\n%s",
                   TAIL);
    expect_refused((plant){body, "bulk on a rewrite"});
    /* a context tail longer than the parents */
    (void)snprintf(body, sizeof body,
                   "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\t\t\nR\tr2\tD\tgate\tcontext:"
                   "stmt\tt1.2\tr1\t\t\tctx=2\n%s",
                   TAIL);
    expect_refused((plant){body, "ctx longer than parents"});
    /* a SOURCE with a construct that has no C line; a C line of an unknown kind or bad span */
    (void)snprintf(body, sizeof body, "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\tc1\t\n%s", TAIL);
    expect_refused((plant){body, "no C line"});
    (void)snprintf(
        body, sizeof body,
        "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\tc1\t\nC\tc1\tNOPE\tstmt\t\tf1\t1.1-1.2\t\n%s",
        TAIL);
    expect_refused((plant){body, "unknown kind"});
    (void)snprintf(body, sizeof body,
                   "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\tc1\t\nC\tc1\tNONBLOCKING_"
                   "ASSIGN\tstmt\t\tf1\t1.1-oops\t\n%s",
                   TAIL);
    expect_refused((plant){body, "bad span"});
    /* a kind that does not match its level */
    (void)snprintf(body, sizeof body,
                   "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1-1.2\tc1\t\nC\tc1\tNONBLOCKING_"
                   "ASSIGN\tmodule\t\tf1\t1.1-1.2\t\n%s",
                   TAIL);
    expect_refused((plant){body, "kind and level disagree"});
    /* a step whose run is not a T line; an origin with a bad span */
    (void)snprintf(body, sizeof body, "R\tr1\tS\tstmt\torigin\tt9.1\t\tf1:1.1-1.2\t\t\n%s", TAIL);
    expect_refused((plant){body, "no run"});
    (void)snprintf(body, sizeof body, "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:1.1\t\t\n%s", TAIL);
    expect_refused((plant){body, "origin without a span"});
}

static void test_ast_none_with_c_lines_is_refused(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "# odin3-names 2 hash=sha256 width=10 prov=1 ast=none\n%s",
                   BODY);
    parse(text);
    odin3_restore *restore = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_design_restore(design, &map, "t.names", &restore));
    TEST_ASSERT_NULL(odin3_design_extra(design));
}

static void test_a_version_1_map_is_refused(void) {
    parse("# odin3-names 1 hash=sha256 width=10\n");
    odin3_restore *restore = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_restore(design, &map, "t.names", &restore));
    TEST_ASSERT_NULL(odin3_design_extra(design));
}

static void test_a_second_restore_appends(void) {
    odin3_restore *one = restore_body(HEADER, BODY);
    odin3_name_map_free(&map);
    memset(&map, 0, sizeof map);
    odin3_restore *two = restore_body(HEADER, BODY);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_extra_count(odin3_design_extra(design)));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    odin3_restore_free(two);
    odin3_restore_free(one);
}

static void test_out_of_memory_returns_no_memory_without_a_crash(void) {
    char text[TEXT_BUF];
    (void)snprintf(text, sizeof text, "%s%s", HEADER, BODY);
    parse(text);
    for (int after = 0; after < 400; after += 7) {
        odin3_restore *restore = NULL;
        odin3_util_set_alloc_fail_after(after);
        odin3_status status = odin3_design_restore(design, &map, "t.names", &restore);
        odin3_util_set_alloc_fail_after(-1);
        if (status == ODIN3_OK) {
            odin3_restore_free(restore);
            break;
        }
        TEST_ASSERT_TRUE(status == ODIN3_ERR_NO_MEMORY || status == ODIN3_ERR_PARSE);
        odin3_design_destroy(design);
        design = odin3_design_create();
        odin3_pass_ctx ctx = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_blif"), &ctx));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_restores_the_pass_runs_flagged_restored);
    RUN_TEST(test_restores_the_records_with_their_origins_and_parents);
    RUN_TEST(test_restores_the_classes_as_defaults_and_overrides);
    RUN_TEST(test_restores_the_bulk_flag_and_the_context_tail);
    RUN_TEST(test_construct_rows_are_remapped_per_run);
    RUN_TEST(test_extra_lines_names_and_the_slot);
    RUN_TEST(test_a_design_with_no_restore_has_no_table_and_no_slot);
    RUN_TEST(test_binds_objects_by_entry_and_by_name);
    RUN_TEST(test_a_recs_list_of_several_ids_warns_and_binds_the_first);
    RUN_TEST(test_a_map_that_fails_restores_nothing);
    RUN_TEST(test_ast_none_with_c_lines_is_refused);
    RUN_TEST(test_a_version_1_map_is_refused);
    RUN_TEST(test_a_second_restore_appends);
    RUN_TEST(test_out_of_memory_returns_no_memory_without_a_crash);
    return UNITY_END();
}
