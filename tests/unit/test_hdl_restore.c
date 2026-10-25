/*
 * test_hdl_restore.c — unit tests for the reader's record of restored HDL locations (spec 5.4
 * D-1, D-7), with the dump_locs pass that lists them.
 */
#include "backends/common/hdl_loc.h"
#include "frontends/hdl_restore.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MANY = 12, TEXT_MAX = 4096, PATH_MAX_LEN = 128 };

/* The class of the operations these tests begin (IR-12a; rule 13 needs one per derive). */
static const odin3_opclass test_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                         false, 0};

static odin3_design *design;
static odin3_pass_ctx run;

static uint32_t intern(const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &str));
    return str;
}

void setUp(void) {
    odin3_log_set_level(ODIN3_LOG_ERROR);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("read_x"), &run));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* The reader's own record: IMPORTED at line 4 of a netlist. */
static odin3_prov_id netlist_prov(void) {
    odin3_srcloc loc = {intern("d.blif"), 4, 0, 0, 0, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&run, &origin, &id));
    return id;
}

static odin3_hdl_loc hdl_loc(const char *file, uint32_t line) {
    return (odin3_hdl_loc){odin3_bytes_cstr(file), line, 1, line, 9};
}

static odin3_hdl_restored restore(const odin3_hdl_restore_req *req) {
    odin3_hdl_restored out = {{0}, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_restore(&run, req, &out));
    return out;
}

/* D-1: DERIVED{parents = [SOURCE, base]}, the HDL first. */
static void test_record_shape(void) {
    odin3_prov_id base = netlist_prov();
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){base, &loc, 1, intern("top/u1")});
    const odin3_prov_record *rec = odin3_prov_get(design, got.prov);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT(2, rec->parents.count);
    const odin3_prov_record *first = odin3_prov_get(design, rec->parents.ids[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, first->kind);
    TEST_ASSERT_EQUAL_UINT(base.v, rec->parents.ids[1].v);
    TEST_ASSERT_EQUAL_UINT(1, first->n_locs);
    TEST_ASSERT_EQUAL_UINT(7, first->locs[0].line);
    TEST_ASSERT_EQUAL_UINT(9, first->locs[0].end_col);
    TEST_ASSERT_EQUAL_UINT(0, first->ast);
    TEST_ASSERT_EQUAL_UINT(0, first->locs[0].loc);
    TEST_ASSERT_FALSE(got.capped);
}

/* D-1: hier is on both records. */
static void test_hier_on_both(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    uint32_t hier = intern("top/u1");
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), &loc, 1, hier});
    const odin3_prov_record *rec = odin3_prov_get(design, got.prov);
    TEST_ASSERT_EQUAL_UINT(hier, rec->hier);
    TEST_ASSERT_EQUAL_UINT(hier, odin3_prov_get(design, rec->parents.ids[0])->hier);
}

/* No netlist record (base 0): still one DERIVED record, whose only parent is the SOURCE. */
static void test_base_zero_has_source_parent(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){{0}, &loc, 1, 0});
    const odin3_prov_record *rec = odin3_prov_get(design, got.prov);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT(1, rec->parents.count);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, odin3_prov_get(design, rec->parents.ids[0])->kind);
}

/* Hash-consing: equal restore calls are one record; another base or location is another. */
static void test_equal_calls_one_record(void) {
    odin3_prov_id base = netlist_prov();
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_hdl_loc other = hdl_loc("a.v", 8);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restore_req req = {base, &loc, 1, 0};
    odin3_hdl_restored first = restore(&req);
    odin3_hdl_restored second = restore(&req);
    TEST_ASSERT_EQUAL_UINT(first.prov.v, second.prov.v);
    odin3_hdl_restore_req other_req = {base, &other, 1, 0};
    TEST_ASSERT_NOT_EQUAL_UINT(first.prov.v, restore(&other_req).prov.v);
}

/* The record names the object by its HDL leaf; odin3_hdl_locs returns what was restored. */
static void test_hdl_view_of_restored(void) {
    odin3_hdl_loc locs[2] = {hdl_loc("a.v", 3), hdl_loc("b.v", 5)};
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), locs, 2, 0});
    odin3_hdl_ctx *ctx = odin3_hdl_ctx_create(design, "test");
    TEST_ASSERT_NOT_NULL(ctx);
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, got.prov, &out));
    TEST_ASSERT_EQUAL_UINT(2, out.n);
    TEST_ASSERT_EQUAL_UINT(3, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT(5, out.locs[1].line);
    TEST_ASSERT_EQUAL_UINT(1, out.locs[1].col);
    TEST_ASSERT_EQUAL_UINT(9, out.locs[1].end_col);
    const odin3_prov_record *named = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_first_record(ctx, got.prov, &named));
    TEST_ASSERT_NOT_NULL(named);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, named->kind);
    TEST_ASSERT_EQUAL_STRING("a.v",
                             odin3_strtab_get(odin3_design_strtab(design), named->locs[0].file));
    odin3_hdl_ctx_destroy(ctx);
}

/* D-7: more than 8 locations keep the first 8 and set the flag. */
static void test_cap(void) {
    odin3_hdl_loc locs[MANY];
    for (uint32_t i = 0; i < MANY; i++) {
        locs[i] = hdl_loc("a.v", i + 1);
    }
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), locs, MANY, 0});
    TEST_ASSERT_TRUE(got.capped);
    const odin3_prov_record *rec = odin3_prov_get(design, got.prov);
    const odin3_prov_record *source = odin3_prov_get(design, rec->parents.ids[0]);
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, source->n_locs);
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, source->locs[ODIN3_HDL_LOC_MAX - 1].line);
}

static unsigned errors_seen;

static void error_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_seen++;
    }
}

/* No operation begun, no locations, a bad base, a NULL argument: INVALID_ARG, logged. */
static void test_invalid_arguments(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_hdl_restored out = {{0}, false};
    odin3_hdl_restore_req req = {{0}, &loc, 1, 0};
    errors_seen = 0;
    odin3_log_set_sink(error_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(&run, &req, &out)); /* op 0 */
    odin3_prov_begin_op_class(&run, &test_class);
    req.n = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(&run, &req, &out));
    req.n = 1;
    req.base = (odin3_prov_id){99999};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(&run, &req, &out));
    req.base = (odin3_prov_id){0};
    req.hier_str = 99999;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(&run, &req, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(NULL, &req, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hdl_restore(&run, NULL, &out));
    TEST_ASSERT_TRUE(errors_seen >= 4);
}

/* A design whose objects carry such records passes the full check, and the forward index finds
 * the object from the HDL file and line. */
static void test_check_and_forward_index(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), &loc, 1, 0});
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("top"), got.prov, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern("n"), got.prov, &net));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    odin3_prov_index *index = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(index);
    odin3_srcloc where = {intern("a.v"), 7, 0, 0, 0, 0};
    odin3_prov_hits hits = odin3_prov_index_by_loc(index, where);
    bool found_net = false;
    for (uint32_t i = 0; i < hits.count; i++) {
        found_net =
            found_net || (hits.hits[i].obj.kind == ODIN3_OBJ_NET && hits.hits[i].obj.id == net.v);
    }
    TEST_ASSERT_TRUE(found_net);
    odin3_srcloc elsewhere = {intern("d.blif"), 4, 0, 0, 0, 0};
    TEST_ASSERT_TRUE(odin3_prov_index_by_loc(index, elsewhere).count >
                     0); /* the netlist line stays reachable */
    odin3_prov_index_destroy(index);
}

/* --- the memoised walk (review M2) --------------------------------------------------------------
 */

enum { CHAIN = 3000, OBJECTS = 2000 };

/* A SOURCE of file:line in ctx's run. */
static odin3_prov_id source_at(odin3_pass_ctx *ctx, const char *file, uint32_t line) {
    odin3_srcloc loc = {intern(file), line, 1, line, 2, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_source(ctx, &(odin3_prov_origin){&loc, 1, 0, 0}, &id));
    return id;
}

/* A DERIVED record over leaf and, when there is one, tail. */
static odin3_prov_id derived_over(odin3_pass_ctx *ctx, odin3_prov_id leaf, odin3_prov_id tail) {
    odin3_prov_id parents[2] = {leaf, tail};
    odin3_prov_id id = {0};
    odin3_prov_begin_op_class(ctx, &test_class);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_derive(ctx, (odin3_prov_list){parents, tail.v != 0 ? 2U : 1U}, &id));
    return id;
}

static void expect_chain_object(odin3_hdl_ctx *hdl, odin3_prov_id object, uint32_t own_line) {
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(hdl, object, &out));
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, out.n);
    TEST_ASSERT_TRUE(out.capped);
    TEST_ASSERT_EQUAL_UINT(own_line, out.locs[0].line); /* own leaf first, then the chain tail */
    TEST_ASSERT_EQUAL_UINT(CHAIN, out.locs[1].line);
    TEST_ASSERT_EQUAL_UINT(CHAIN - (ODIN3_HDL_LOC_MAX - 2), out.locs[ODIN3_HDL_LOC_MAX - 1].line);
}

/* A DERIVED chain of CHAIN records (each over the one before and a SOURCE of its own line), then
 * OBJECTS records each over the chain's tail and a SOURCE of its own: all queried through one
 * context. Each record is walked once, so the context resolves about CHAIN + 3 * OBJECTS records,
 * not OBJECTS * CHAIN. */
static void test_shared_deep_chain_is_walked_once(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("deep"), &ctx));
    odin3_prov_id tail = {0};
    for (uint32_t i = 0; i < CHAIN; i++) {
        tail = derived_over(&ctx, source_at(&ctx, "c.v", i + 1), tail);
    }
    odin3_hdl_ctx *hdl = odin3_hdl_ctx_create(design, "test");
    TEST_ASSERT_NOT_NULL(hdl);
    for (uint32_t i = 0; i < OBJECTS; i++) {
        expect_chain_object(hdl, derived_over(&ctx, source_at(&ctx, "o.v", i + 1), tail), i + 1);
    }
    TEST_ASSERT_TRUE(odin3_hdl_ctx_records(hdl) <= (size_t)CHAIN * 2 + (size_t)OBJECTS * 3);
    odin3_hdl_ctx_destroy(hdl);
}

/* The memoised result equals the plain definition on a small DAG with a shared diamond. */
static void test_memo_equals_walk_on_diamond(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("dia"), &ctx));
    odin3_prov_id leaf[3];
    for (uint32_t i = 0; i < 3; i++) {
        odin3_srcloc loc = {intern("d.v"), i + 1, 1, i + 1, 2, 0};
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_prov_source(&ctx, &(odin3_prov_origin){&loc, 1, 0, 0}, &leaf[i]));
    }
    odin3_prov_id left = {0};
    odin3_prov_id right = {0};
    odin3_prov_id top = {0};
    odin3_prov_id lp[2] = {leaf[0], leaf[1]};
    odin3_prov_id rp[2] = {leaf[2], leaf[1]};
    odin3_prov_begin_op_class(&ctx, &test_class);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){lp, 2}, &left));
    odin3_prov_begin_op_class(&ctx, &test_class);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){rp, 2}, &right));
    odin3_prov_id tp[2] = {left, right};
    odin3_prov_begin_op_class(&ctx, &test_class);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&ctx, (odin3_prov_list){tp, 2}, &top));
    odin3_hdl_ctx *hdl = odin3_hdl_ctx_create(design, "test");
    TEST_ASSERT_NOT_NULL(hdl);
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(hdl, top, &out));
    TEST_ASSERT_EQUAL_UINT(3, out.n);
    TEST_ASSERT_EQUAL_UINT(1, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT(2, out.locs[1].line);
    TEST_ASSERT_EQUAL_UINT(3, out.locs[2].line);
    TEST_ASSERT_FALSE(out.capped);
    odin3_hdl_ctx_destroy(hdl);
}

/* Review L1: a location with line 0 is never printed (the parser would reject `file:0`). */
static void test_line_zero_is_skipped(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("zero"), &ctx));
    odin3_srcloc locs[2] = {{intern("a.v"), 0, 0, 0, 0, 0}, {intern("b.v"), 5, 0, 0, 0, 0}};
    odin3_prov_id only_zero = {0};
    odin3_prov_id mixed = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_prov_source(&ctx, &(odin3_prov_origin){&locs[0], 1, 0, 0}, &only_zero));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_source(&ctx, &(odin3_prov_origin){locs, 2, 0, 0}, &mixed));
    odin3_hdl_ctx *hdl = odin3_hdl_ctx_create(design, "test");
    TEST_ASSERT_NOT_NULL(hdl);
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(hdl, only_zero, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(hdl, mixed, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.n); /* the first location has no line: the leaf does not count */
    odin3_hdl_ctx_destroy(hdl);
}

/* --- dump_locs ------------------------------------------------------------------------------- */

static char *read_whole(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = odin3_util_calloc(TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    size_t len = fread(text, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    (void)fclose(file);
    return text;
}

/* A design with a located module and net, an unlocated net, and the dump_locs output of it. */
static char *dump_of_located_design(unsigned *bare_id) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), &loc, 1, 0});
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("top"), got.prov, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id located = {0};
    odin3_net_id bare = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern("n"), got.prov, &located));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, netlist_prov(), &bare));
    *bare_id = (unsigned)bare.v;
    char dir[] = "/tmp/odin3_dumplocs_XXXXXX";
    char path[PATH_MAX_LEN];
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/locs.txt", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(path)));
    char *text = read_whole(path);
    (void)remove(path);
    (void)rmdir(dir);
    return text;
}

static void expect_line(const char *text, const char *want) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, want), text);
}

/* The pass lists module, wire, net and node lines, SRC empty for an object with no HDL location,
 * and a restored object shows what was restored (never the netlist line). */
static void test_dump_locs_lines(void) {
    unsigned bare = 0;
    char *text = dump_of_located_design(&bare);
    char want_bare[64];
    (void)snprintf(want_bare, sizeof want_bare, "top|net|$n%u|\n", bare);
    expect_line(text, "top|module|top|a.v:7.1-7.9\n");
    expect_line(text, "top|net|n|a.v:7.1-7.9\n");
    expect_line(text, want_bare);
    TEST_ASSERT_NULL(strstr(text, "d.blif"));
    odin3_util_free(text);
}

/* Review L4: a `|` or backslash in a name is escaped, so the fields stay unambiguous. */
static void test_dump_locs_escapes_names(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), &loc, 1, 0});
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("t"), got.prov, &mid));
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(odin3_module_get(design, mid),
                                                     intern("x|y\\z"), got.prov, &net));
    char dir[] = "/tmp/odin3_dumplocs_XXXXXX";
    char path[PATH_MAX_LEN];
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/locs.txt", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(path)));
    char *text = read_whole(path);
    expect_line(text, "t|net|x\\|y\\\\z|a.v:7.1-7.9\n");
    odin3_util_free(text);
    (void)remove(path);
    (void)rmdir(dir);
}

static void test_dump_locs_arguments(void) {
    errors_seen = 0;
    odin3_log_set_sink(error_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(design, "dump_locs", odin3_bytes_cstr("a b")));
    TEST_ASSERT_TRUE(errors_seen > 0);
}

/* A node of the registered type, or of a port-less local type, of that name. */
static void add_typed_node(odin3_module *mod, const char *type_name, const char *node_name,
                           odin3_prov_id prov) {
    odin3_celltype_def def = {type_name, ODIN3_GRAN_HARD, 0, NULL, 0, NULL, 0, NULL, NULL, NULL,
                              NULL};
    odin3_celltype_id type = {0};
    odin3_node_id node = {0};
    if (!odin3_celltype_find(design, intern(type_name), &type)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &type));
    }
    odin3_node_spec spec = {type, intern(node_name), prov, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, &node));
}

/* A module "t" with located nodes of several types. */
static void build_typed_design(void) {
    odin3_hdl_loc loc = hdl_loc("a.v", 7);
    odin3_prov_begin_op_class(&run, &test_class);
    odin3_hdl_restored got = restore(&(odin3_hdl_restore_req){netlist_prov(), &loc, 1, 0});
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("t"), got.prov, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, intern("n"), got.prov, &(odin3_net_id){0}));
    add_typed_node(mod, "$lut", "u1", got.prov);
    add_typed_node(mod, "$_DFF_P_", "u2", got.prov);
    add_typed_node(mod, "lpm_add_sub", "u3", netlist_prov());
    add_typed_node(mod, "a:b|c\\d", "u4", got.prov);
    add_typed_node(mod, "x\ny", "u5", got.prov);
}

/* The dump_locs output of the typed design: args_prefix, then a temp file path, then suffix. */
static char *dump_typed_design_ex(const char *args_prefix, const char *suffix) {
    build_typed_design();
    char dir[] = "/tmp/odin3_dumplocs_XXXXXX";
    char path[PATH_MAX_LEN];
    char args[PATH_MAX_LEN + 16];
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/locs.txt", dir);
    (void)snprintf(args, sizeof args, "%s%s%s", args_prefix, path, suffix);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(args)));
    char *text = read_whole(path);
    (void)remove(path);
    (void)rmdir(dir);
    return text;
}

static char *dump_typed_design(const char *args_prefix) {
    return dump_typed_design_ex(args_prefix, "");
}

/* Without --types the kind field is the bare `node` (byte-identical output). */
static void test_dump_locs_default_untyped(void) {
    char *text = dump_typed_design("");
    expect_line(text, "t|node|u1|a.v:7.1-7.9\n");
    expect_line(text, "t|node|u3|\n");
    TEST_ASSERT_NULL(strstr(text, "node:"));
    TEST_ASSERT_NULL(strstr(text, "$lut"));
    odin3_util_free(text);
}

/* With --types a node line is `module|node:TYPE|name|SRC`, TYPE escaped; other lines unchanged. */
static void test_dump_locs_types(void) {
    char *text = dump_typed_design("--types ");
    expect_line(text, "t|node:$lut|u1|a.v:7.1-7.9\n");
    expect_line(text, "t|node:$_DFF_P_|u2|a.v:7.1-7.9\n");
    expect_line(text, "t|node:lpm_add_sub|u3|\n");
    expect_line(text, "t|node:a:b\\|c\\\\d|u4|a.v:7.1-7.9\n");
    expect_line(text, "t|node:x\\ny|u5|a.v:7.1-7.9\n");
    expect_line(text, "t|net|n|a.v:7.1-7.9\n");
    expect_line(text, "t|module|t|a.v:7.1-7.9\n");
    odin3_util_free(text);
}

/* A reader tells the formats apart by the kind field alone: plain text is `|node|`. */
static void test_dump_locs_kind_field_plain(void) {
    char *plain = dump_typed_design("");
    TEST_ASSERT_NOT_NULL(strstr(plain, "|node|"));
    TEST_ASSERT_NULL(strstr(plain, "|node:"));
    odin3_util_free(plain);
}

/* Typed text has no bare `|node|`; the first `:` of the kind field splits kind from type. */
static void test_dump_locs_kind_field_typed(void) {
    char *typed = dump_typed_design("--types ");
    TEST_ASSERT_NULL(strstr(typed, "|node|"));
    TEST_ASSERT_NOT_NULL(strstr(typed, "|node:a:b"));
    odin3_util_free(typed);
}

static void test_dump_locs_unknown_option(void) {
    errors_seen = 0;
    odin3_log_set_sink(error_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(design, "dump_locs", odin3_bytes_cstr("--bogus x")));
    TEST_ASSERT_TRUE(errors_seen > 0);
}

/* Runs dump_locs with args and expects INVALID_ARG plus a logged error. */
static void expect_invalid_args(const char *args) {
    errors_seen = 0;
    odin3_log_set_sink(error_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(args)));
    TEST_ASSERT_TRUE(errors_seen > 0);
}

static void test_dump_locs_types_after_path(void) {
    char *text = dump_typed_design_ex("", " --types");
    expect_line(text, "t|node:$lut|u1|a.v:7.1-7.9\n");
    TEST_ASSERT_NULL(strstr(text, "|node|"));
    odin3_util_free(text);
}

static void test_dump_locs_types_twice(void) {
    build_typed_design();
    expect_invalid_args("--types --types /tmp/odin3_dumplocs_never");
}

static void test_dump_locs_second_path(void) {
    build_typed_design();
    expect_invalid_args("--types /tmp/odin3_dumplocs_a /tmp/odin3_dumplocs_b");
}

static void test_dump_locs_empty_path(void) {
    build_typed_design();
    expect_invalid_args("\"\"");
    expect_invalid_args("--types \"\"");
}

/* A literal `--` ends the options: a path starting with `--` is usable. */
static void test_dump_locs_double_dash(void) {
    char dir[] = "/tmp/odin3_dumplocs_XXXXXX";
    char path[PATH_MAX_LEN];
    char args[PATH_MAX_LEN + 16];
    build_typed_design();
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/--locs.txt", dir);
    (void)snprintf(args, sizeof args, "--types -- %s", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "dump_locs", odin3_bytes_cstr(args)));
    char *text = read_whole(path);
    expect_line(text, "t|node:$lut|u1|a.v:7.1-7.9\n");
    odin3_util_free(text);
    (void)remove(path);
    (void)rmdir(dir);
    expect_invalid_args("-- -- x");
}

/* Runs `dump_locs --types` with stdout redirected to the file at path; the pass status. */
static odin3_status types_to_stdout_file(const char *path) {
    (void)fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    TEST_ASSERT_TRUE(saved >= 0);
    TEST_ASSERT_NOT_NULL(freopen(path, "w", stdout));
    odin3_status st = odin3_pass_run(design, "dump_locs", odin3_bytes_cstr("--types"));
    (void)fflush(stdout);
    TEST_ASSERT_TRUE(dup2(saved, STDOUT_FILENO) >= 0);
    (void)close(saved);
    clearerr(stdout);
    return st;
}

/* --types with no path writes the typed text to stdout. */
static void test_dump_locs_types_stdout(void) {
    char dir[] = "/tmp/odin3_dumplocs_XXXXXX";
    char path[PATH_MAX_LEN];
    build_typed_design();
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/out.txt", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, types_to_stdout_file(path));
    char *text = read_whole(path);
    expect_line(text, "t|node:$lut|u1|a.v:7.1-7.9\n");
    expect_line(text, "t|node:x\\ny|u5|a.v:7.1-7.9\n");
    odin3_util_free(text);
    (void)remove(path);
    (void)rmdir(dir);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_record_shape);
    RUN_TEST(test_hier_on_both);
    RUN_TEST(test_base_zero_has_source_parent);
    RUN_TEST(test_shared_deep_chain_is_walked_once);
    RUN_TEST(test_memo_equals_walk_on_diamond);
    RUN_TEST(test_line_zero_is_skipped);
    RUN_TEST(test_equal_calls_one_record);
    RUN_TEST(test_hdl_view_of_restored);
    RUN_TEST(test_cap);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_check_and_forward_index);
    RUN_TEST(test_dump_locs_lines);
    RUN_TEST(test_dump_locs_escapes_names);
    RUN_TEST(test_dump_locs_arguments);
    RUN_TEST(test_dump_locs_default_untyped);
    RUN_TEST(test_dump_locs_types);
    RUN_TEST(test_dump_locs_kind_field_plain);
    RUN_TEST(test_dump_locs_kind_field_typed);
    RUN_TEST(test_dump_locs_unknown_option);
    RUN_TEST(test_dump_locs_types_after_path);
    RUN_TEST(test_dump_locs_types_twice);
    RUN_TEST(test_dump_locs_second_path);
    RUN_TEST(test_dump_locs_empty_path);
    RUN_TEST(test_dump_locs_double_dash);
    RUN_TEST(test_dump_locs_types_stdout);
    return UNITY_END();
}
