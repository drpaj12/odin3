/*
 * test_ir_prov.c — unit tests for provenance lineage: pass runs, operations, hash-consing,
 * backward and forward navigation, tombstones (IR-6, IR-12, IR-13).
 */
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ADDS = 100,
    PIECES = 5,
    LEAVES_MAX = 16,
    NAME_BUF = 32,
    BIG = 100000,
    DEEP = 100000,
    OOM_LIMIT = 10000,
    PROV_PAGE = 1 << 10, /* records per page of the store (prov.c) */
    INDEX_GROWS_AT = 14, /* idindex: 16 slots at 85% load hold 13; the 14th entry grows it */
    /* chain scenario: index, sweep buffer, carried rows and hits, child rows and children,
     * leaf locations, keys, key rows, key leaves, marks, queue, result */
    INDEX_BUILD_ALLOCS = 13,
    SAVE_MAX = 1024,
    WIDE = 1000,
    LINEAR_BYTES = 64, /* index bytes per (record + object slot) */
};

static odin3_design *design;
static odin3_module *module;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("top"), (odin3_prov_id){0}, &mid));
    module = odin3_module_get(design, mid);
    TEST_ASSERT_NOT_NULL(module);
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = NULL;
    fresh_design();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_pass_ctx run_named(const char *name) {
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern(name), &ctx));
    return ctx;
}

static odin3_srcloc loc_at(const char *file, uint32_t line, uint32_t col) {
    odin3_srcloc loc = {intern(file), line, col, line, col + 3};
    return loc;
}

static odin3_prov_id source_at(odin3_pass_ctx *ctx, odin3_srcloc loc, uint32_t hier) {
    odin3_prov_origin origin = {&loc, 1, 0, hier};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(ctx, &origin, &id));
    TEST_ASSERT_TRUE(odin3_prov_valid(id));
    return id;
}

static odin3_prov_id derive_n(odin3_pass_ctx *ctx, const odin3_prov_id *parents, uint32_t n) {
    odin3_prov_id id = {0};
    odin3_prov_list list = {parents, n};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(ctx, list, &id));
    TEST_ASSERT_TRUE(odin3_prov_valid(id));
    return id;
}

static odin3_prov_id derive1(odin3_pass_ctx *ctx, odin3_prov_id parent) {
    return derive_n(ctx, &parent, 1);
}

static odin3_prov_id derive2(odin3_pass_ctx *ctx, odin3_prov_id first, odin3_prov_id second) {
    odin3_prov_id parents[2] = {first, second};
    return derive_n(ctx, parents, 2);
}

static odin3_status derive_status(odin3_pass_ctx *ctx, const odin3_prov_id *parents, uint32_t n) {
    odin3_prov_id id = {0};
    odin3_prov_list list = {parents, n};
    return odin3_prov_derive(ctx, list, &id);
}

static odin3_node_id node_with(const char *type, odin3_prov_id prov) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(type), &tid));
    odin3_node_spec spec = {tid, 0, prov, NULL, 0};
    odin3_node_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &id));
    return id;
}

static odin3_net_id net_with(odin3_prov_id prov) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, &id));
    return id;
}

static void kill_node(odin3_node_id node) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, node));
}

typedef struct leaves {
    odin3_prov_id ids[LEAVES_MAX];
    uint32_t count;
} leaves;

static void collect(void *user, odin3_prov_id leaf) {
    leaves *out = user;
    TEST_ASSERT_TRUE(out->count < LEAVES_MAX);
    out->ids[out->count++] = leaf;
}

static leaves sources_of(odin3_prov_id id) {
    leaves out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_sources(design, id, collect, &out));
    return out;
}

/* The hit for (kind, id) of the module `module`, or NULL. */
static const odin3_prov_hit *find_hit(odin3_prov_hits hits, odin3_objkind kind, uint32_t id) {
    const odin3_prov_hit *found = NULL;
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        if (hit->module.v == odin3_module_id_of(module).v && hit->obj.kind == kind &&
            hit->obj.id == id && hit->tombstone == 0) {
            TEST_ASSERT_NULL(found); /* each object appears once */
            found = hit;
        }
    }
    return found;
}

static void assert_node_hit(odin3_prov_hits hits, odin3_node_id node, bool live) {
    const odin3_prov_hit *hit = find_hit(hits, ODIN3_OBJ_NODE, node.v);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_EQUAL(live, hit->live);
    TEST_ASSERT_EQUAL(live, odin3_node_live(module, node));
}

static uint32_t count_live(odin3_prov_hits hits, bool live) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < hits.count; i++) {
        count += hits.hits[i].live == live ? 1U : 0U;
    }
    return count;
}

/* A copy of a query result (a view lasts only until the next query on the index). */
static odin3_prov_hit saved_hits[SAVE_MAX];

static odin3_prov_hits save_hits(odin3_prov_hits hits) {
    TEST_ASSERT_TRUE(hits.count <= SAVE_MAX);
    if (hits.count > 0) {
        memcpy(saved_hits, hits.hits, sizeof saved_hits[0] * hits.count);
    }
    odin3_prov_hits copy = {saved_hits, hits.count};
    return copy;
}

static void assert_same_hits(odin3_prov_hits want, odin3_prov_hits got) {
    TEST_ASSERT_EQUAL_UINT32(want.count, got.count);
    for (uint32_t i = 0; i < want.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(want.hits[i].module.v, got.hits[i].module.v);
        TEST_ASSERT_EQUAL_INT(want.hits[i].obj.kind, got.hits[i].obj.kind);
        TEST_ASSERT_EQUAL_UINT32(want.hits[i].obj.id, got.hits[i].obj.id);
        TEST_ASSERT_EQUAL(want.hits[i].live, got.hits[i].live);
        TEST_ASSERT_EQUAL_UINT32(want.hits[i].tombstone, got.hits[i].tombstone);
    }
}

/* --- pass runs ----------------------------------------------------------------------------- */

static void test_pass_runs(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_passrun_end(design));
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_pass_ctx lower = run_named("lower");
    TEST_ASSERT_EQUAL_PTR(design, rd.design);
    TEST_ASSERT_EQUAL_UINT32(1, rd.run.v);
    TEST_ASSERT_EQUAL_UINT32(2, lower.run.v);
    TEST_ASSERT_EQUAL_UINT32(0, rd.op);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_UINT32(intern("read_verilog"), odin3_passrun_name(design, rd.run));
    TEST_ASSERT_EQUAL_UINT32(intern("lower"), odin3_passrun_name(design, lower.run));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_name(design, (odin3_passrun_id){0}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_name(design, (odin3_passrun_id){3}));
    odin3_prov_begin_op(&lower);
    odin3_prov_begin_op(&lower);
    TEST_ASSERT_EQUAL_UINT32(2, lower.op);
}

static void test_pass_run_invalid(void) {
    odin3_pass_ctx ctx = {0};
    uint32_t past = (uint32_t)odin3_strtab_count(odin3_design_strtab(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_begin(design, 0, &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_begin(design, past, &ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_begin(design, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_begin(NULL, 1, &ctx));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_passrun_end(design));
    TEST_ASSERT_NULL(ctx.design);
    TEST_ASSERT_EQUAL_size_t(4, errors_logged);
}

/* --- SOURCE / IMPORTED ---------------------------------------------------------------------- */

/* Two records that must not be consed together. */
static void assert_differs(odin3_prov_id one, odin3_prov_id other) {
    TEST_ASSERT_NOT_EQUAL(one.v, other.v);
}

static void test_source_hash_consing(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_srcloc loc = loc_at("a.v", 42, 5);
    odin3_prov_id first = source_at(&rd, loc, intern("top/gen[0]"));
    odin3_prov_begin_op(&rd);
    TEST_ASSERT_EQUAL_UINT32(first.v, source_at(&rd, loc, intern("top/gen[0]")).v); /* op ignored */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_get(design, first)->op); /* the first call's */
    assert_differs(first, source_at(&rd, loc, intern("top/gen[1]")));
    assert_differs(first, source_at(&rd, loc_at("a.v", 43, 5), intern("top/gen[0]")));
    assert_differs(first, source_at(&rd, loc_at("a.v", 42, 6), intern("top/gen[0]")));
    assert_differs(first, source_at(&rd, loc_at("b.v", 42, 5), intern("top/gen[0]")));
    odin3_pass_ctx again = run_named("read_verilog");
    assert_differs(first, source_at(&again, loc, intern("top/gen[0]"))); /* run */
    odin3_prov_origin origin = {&loc, 1, 0, intern("top/gen[0]")};
    odin3_prov_id imported = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&rd, &origin, &imported));
    assert_differs(first, imported); /* kind */
    origin.ast = 7;
    odin3_prov_id with_ast = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&rd, &origin, &with_ast));
    assert_differs(first, with_ast); /* ast */
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

static void test_source_record_fields(void) {
    odin3_pass_ctx rd = run_named("read_blif");
    odin3_srcloc locs[2] = {loc_at("n.blif", 10, 1), loc_at("n.blif", 12, 1)};
    odin3_prov_origin origin = {locs, 2, 0, intern("top")};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_imported(&rd, &origin, &id));
    locs[0].line = 99; /* the record holds a copy */
    const odin3_prov_record *rec = odin3_prov_get(design, id);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(rd.run.v, rec->run.v);
    TEST_ASSERT_EQUAL_UINT32(2, rec->n_locs);
    TEST_ASSERT_EQUAL_UINT32(10, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(12, rec->locs[1].line);
    TEST_ASSERT_EQUAL_UINT32(intern("n.blif"), rec->locs[1].file);
    TEST_ASSERT_EQUAL_UINT32(intern("top"), rec->hier);
    TEST_ASSERT_EQUAL_UINT32(0, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_parents(design, id).count);
    odin3_prov_origin none = {NULL, 0, 0, 0}; /* no location at all is allowed */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&rd, &none, &id));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_get(design, id)->n_locs);
}

static void test_source_invalid(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    uint32_t past = (uint32_t)odin3_strtab_count(odin3_design_strtab(design));
    odin3_srcloc bad_file = {past, 1, 1, 1, 1};
    odin3_prov_origin origins[3] = {{NULL, 1, 0, 0}, {&bad_file, 1, 0, 0}, {NULL, 0, 0, past}};
    odin3_prov_id id = {0};
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_source(&rd, &origins[i], &id));
    }
    odin3_prov_origin fine = {NULL, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_source(&rd, NULL, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_source(NULL, &fine, &id));
    odin3_pass_ctx stale = {design, {7}, 0}; /* no such run */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_imported(&stale, &fine, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_prov_end(design));
    TEST_ASSERT_EQUAL_size_t(6, errors_logged);
}

/* --- DERIVED ------------------------------------------------------------------------------- */

static void test_derive_invalid(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("a.v", 1, 1), 0);
    odin3_pass_ctx lower = run_named("lower");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, derive_status(&lower, &src, 1)); /* op 0 */
    odin3_prov_begin_op(&lower);
    odin3_prov_id missing[3] = {{0}, {2}, {UINT32_MAX}}; /* none, next (future), far */
    for (uint32_t i = 0; i < 3; i++) {
        odin3_prov_id pair[2] = {src, missing[i]};
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, derive_status(&lower, pair, 2));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, derive_status(&lower, &src, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, derive_status(&lower, NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, derive_status(NULL, &src, 1));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_prov_end(design)); /* only src */
    TEST_ASSERT_EQUAL_size_t(7, errors_logged);
}

static void test_derive_identity(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("a.v", 1, 1), 0);
    odin3_prov_id other = source_at(&rd, loc_at("a.v", 2, 1), 0);
    odin3_pass_ctx lower = run_named("lower");
    odin3_prov_begin_op(&lower);
    odin3_prov_id piece = derive1(&lower, src);
    TEST_ASSERT_EQUAL_UINT32(piece.v, derive1(&lower, src).v); /* same operation */
    const odin3_prov_record *rec = odin3_prov_get(design, piece);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(lower.run.v, rec->run.v);
    TEST_ASSERT_EQUAL_UINT32(1, rec->op);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(src.v, rec->parents.ids[0].v);
    odin3_prov_id both = derive2(&lower, src, other);
    TEST_ASSERT_NOT_EQUAL(both.v, derive2(&lower, other, src).v); /* parent order counts */
    odin3_prov_id repeated[3] = {src, other, src};
    TEST_ASSERT_EQUAL_UINT32(both.v, derive_n(&lower, repeated, 3).v); /* kept once */
    odin3_prov_begin_op(&lower);
    TEST_ASSERT_NOT_EQUAL(piece.v, derive1(&lower, src).v); /* next operation */
    odin3_pass_ctx again = run_named("lower");
    again.op = 1;
    TEST_ASSERT_NOT_EQUAL(piece.v, derive1(&again, src).v); /* same op number, other run */
}

/* --- Review Focus 3: 100 $adds from one line, each lowered, two gates clumped ---------------- */

typedef struct focus_scenario {
    odin3_srcloc loc;
    odin3_prov_id source;
    odin3_prov_id op_rec[ADDS];
    odin3_node_id adds[ADDS];
    odin3_node_id gates[ADDS][PIECES];
    odin3_prov_id clump_rec;
    odin3_node_id clump;
} focus_scenario;

static void focus_lower(focus_scenario *sc) {
    odin3_pass_ctx lower = run_named("lower");
    for (uint32_t i = 0; i < ADDS; i++) {
        odin3_prov_begin_op(&lower);
        sc->op_rec[i] = derive1(&lower, sc->source);
        for (uint32_t k = 0; k < PIECES; k++) {
            TEST_ASSERT_EQUAL_UINT32(sc->op_rec[i].v, derive1(&lower, sc->source).v);
            sc->gates[i][k] = node_with("$_AND_", sc->op_rec[i]);
        }
        kill_node(sc->adds[i]);
    }
}

static void focus_build(focus_scenario *sc) {
    odin3_pass_ctx rd = run_named("read_verilog");
    sc->loc = loc_at("adders.v", 42, 9);
    sc->source = source_at(&rd, sc->loc, intern("top/gen"));
    for (uint32_t i = 0; i < ADDS; i++) {
        sc->adds[i] = node_with("$add", sc->source);
    }
    focus_lower(sc);
    odin3_pass_ctx clump = run_named("clump");
    odin3_prov_begin_op(&clump);
    sc->clump_rec = derive2(&clump, sc->op_rec[3], sc->op_rec[7]);
    sc->clump = node_with("$_XOR_", sc->clump_rec);
    kill_node(sc->gates[3][0]);
    kill_node(sc->gates[7][0]);
}

static void focus_check_hits(const focus_scenario *sc, odin3_prov_hits hits) {
    TEST_ASSERT_EQUAL_UINT32(ADDS + ADDS * PIECES + 1, hits.count);
    TEST_ASSERT_EQUAL_UINT32(ADDS + 2, count_live(hits, false));
    for (uint32_t i = 0; i < ADDS; i++) {
        assert_node_hit(hits, sc->adds[i], false);
        for (uint32_t k = 0; k < PIECES; k++) {
            assert_node_hit(hits, sc->gates[i][k], !(k == 0 && (i == 3 || i == 7)));
        }
    }
    assert_node_hit(hits, sc->clump, true);
}

static void test_review_focus_3(void) {
    focus_scenario sc;
    memset(&sc, 0, sizeof sc);
    focus_build(&sc);
    for (uint32_t i = 1; i < ADDS; i++) { /* 100 distinct operation records, in order */
        TEST_ASSERT_EQUAL_UINT32(sc.op_rec[0].v + i, sc.op_rec[i].v);
    }
    const odin3_prov_record *rec = odin3_prov_get(design, sc.clump_rec);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(sc.op_rec[3].v, rec->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(sc.op_rec[7].v, rec->parents.ids[1].v);
    leaves found = sources_of(sc.clump_rec);
    TEST_ASSERT_EQUAL_UINT32(1, found.count); /* S once, though reached twice */
    TEST_ASSERT_EQUAL_UINT32(sc.source.v, found.ids[0].v);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_prov_hits by_loc = save_hits(odin3_prov_index_by_loc(ix, sc.loc));
    focus_check_hits(&sc, by_loc);
    assert_same_hits(by_loc, odin3_prov_index_by_record(ix, sc.source));
    odin3_prov_hits op3 = odin3_prov_index_by_record(ix, sc.op_rec[3]);
    TEST_ASSERT_EQUAL_UINT32(PIECES + 1, op3.count);
    assert_node_hit(op3, sc.gates[3][0], false);
    assert_node_hit(op3, sc.clump, true);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_prov_index_by_record(ix, sc.clump_rec).count);
    odin3_prov_index_destroy(ix);
}

/* --- decompose then clump, with nets, wires and a tombstone --------------------------------- */

typedef struct chain_scenario {
    odin3_srcloc loc;
    odin3_prov_id source, first_op, second_op, clump_rec;
    odin3_node_id add, add2, gates[2][PIECES], clump;
    odin3_net_id net;
    odin3_wire_id wire;
    uint32_t tomb;
} chain_scenario;

/* Lowers add into PIECES gates, a net and a 2-bit wire (with its 2 nets), all in one operation. */
static odin3_prov_id chain_lower(chain_scenario *sc, odin3_pass_ctx *lower, uint32_t which) {
    odin3_prov_begin_op(lower);
    odin3_prov_id rec = derive1(lower, sc->source);
    for (uint32_t k = 0; k < PIECES; k++) {
        sc->gates[which][k] = node_with("$_AND_", derive1(lower, sc->source));
    }
    if (which == 0) {
        sc->net = net_with(rec);
        odin3_wire_spec spec = {intern("w_lowered"), 1, 0, false, rec};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &sc->wire));
    }
    kill_node(which == 0 ? sc->add : sc->add2);
    return rec;
}

static void chain_build(chain_scenario *sc) {
    odin3_pass_ctx rd = run_named("read_verilog");
    sc->loc = loc_at("alu.v", 7, 3);
    sc->source = source_at(&rd, sc->loc, intern("top/alu"));
    sc->add = node_with("$add", sc->source);
    sc->add2 = node_with("$add", sc->source);
    odin3_pass_ctx lower = run_named("lower");
    sc->first_op = chain_lower(sc, &lower, 0);
    sc->second_op = chain_lower(sc, &lower, 1);
    odin3_pass_ctx clump = run_named("abc");
    odin3_prov_begin_op(&clump);
    sc->clump_rec = derive2(&clump, sc->first_op, sc->second_op);
    sc->clump = node_with("$_XOR_", sc->clump_rec);
    kill_node(sc->gates[0][1]);
    kill_node(sc->gates[1][2]);
    /* compact (Task 7) would free the dead add's slot and leave this behind */
    odin3_tombstone tomb = {odin3_module_id_of(module), ODIN3_OBJ_NODE,
                            odin3_node_type(module, sc->add), 0, sc->source};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_tombstone_add(design, &tomb));
    sc->tomb = odin3_tombstone_end(design) - 1;
}

static void chain_check_backward(const chain_scenario *sc) {
    leaves found = sources_of(sc->clump_rec);
    TEST_ASSERT_EQUAL_UINT32(1, found.count);
    TEST_ASSERT_EQUAL_UINT32(sc->source.v, found.ids[0].v);
    const odin3_prov_record *src = odin3_prov_get(design, found.ids[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, src->kind);
    TEST_ASSERT_EQUAL_UINT32(7, src->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(intern("alu.v"), src->locs[0].file);
    odin3_prov_list up = odin3_prov_parents(design, sc->clump_rec);
    TEST_ASSERT_EQUAL_UINT32(2, up.count);
    TEST_ASSERT_EQUAL_UINT32(sc->first_op.v, up.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(sc->second_op.v, up.ids[1].v);
    TEST_ASSERT_EQUAL_UINT32(sc->source.v, odin3_prov_parents(design, sc->first_op).ids[0].v);
    /* every step names its run: abc <- lower <- read_verilog */
    const odin3_prov_record *step = odin3_prov_get(design, sc->clump_rec);
    TEST_ASSERT_EQUAL_UINT32(intern("abc"), odin3_passrun_name(design, step->run));
    step = odin3_prov_get(design, step->parents.ids[0]);
    TEST_ASSERT_EQUAL_UINT32(intern("lower"), odin3_passrun_name(design, step->run));
    TEST_ASSERT_EQUAL_UINT32(intern("read_verilog"), odin3_passrun_name(design, src->run));
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(module, sc->gates[0][1]).v,
                             odin3_pin_prov(module, odin3_node_pins(module, sc->gates[0][1]).first)
                                 .v); /* pins inherit their node's prov */
}

/* Tombstone hits, each checked to be tombstone `tomb`: dead, kind node, no object ID. */
static uint32_t count_tombstones(odin3_prov_hits hits, uint32_t tomb) {
    uint32_t tombs = 0;
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        if (hit->tombstone == 0) {
            continue;
        }
        tombs++;
        TEST_ASSERT_EQUAL_UINT32(tomb, hit->tombstone);
        TEST_ASSERT_EQUAL_UINT32(0, hit->obj.id);
        TEST_ASSERT_EQUAL_INT(ODIN3_OBJ_NODE, hit->obj.kind);
        TEST_ASSERT_EQUAL(false, hit->live);
    }
    return tombs;
}

static void chain_check_forward(const chain_scenario *sc, odin3_prov_hits hits) {
    /* 2 adds + 10 gates + clump + net + wire + the wire's 2 nets + tombstone */
    TEST_ASSERT_EQUAL_UINT32(2 + 2 * PIECES + 1 + 1 + 1 + 2 + 1, hits.count);
    TEST_ASSERT_EQUAL_UINT32(5, count_live(hits, false)); /* 2 adds, 2 gates, tombstone */
    assert_node_hit(hits, sc->add, false);
    assert_node_hit(hits, sc->add2, false);
    for (uint32_t k = 0; k < PIECES; k++) {
        assert_node_hit(hits, sc->gates[0][k], k != 1);
        assert_node_hit(hits, sc->gates[1][k], k != 2);
    }
    assert_node_hit(hits, sc->clump, true);
    TEST_ASSERT_NOT_NULL(find_hit(hits, ODIN3_OBJ_NET, sc->net.v));
    TEST_ASSERT_NOT_NULL(find_hit(hits, ODIN3_OBJ_WIRE, sc->wire.v));
    TEST_ASSERT_NOT_NULL(find_hit(hits, ODIN3_OBJ_NET, odin3_wire_net(module, sc->wire, 1).v));
    TEST_ASSERT_EQUAL_UINT32(1, count_tombstones(hits, sc->tomb));
}

static void test_decompose_then_clump(void) {
    chain_scenario sc;
    memset(&sc, 0, sizeof sc);
    chain_build(&sc);
    chain_check_backward(&sc);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_srcloc other_col = sc.loc;
    other_col.col = 40; /* file + line match; column is ignored */
    odin3_prov_hits hits = save_hits(odin3_prov_index_by_loc(ix, other_col));
    chain_check_forward(&sc, hits);
    assert_same_hits(hits, odin3_prov_index_by_record(ix, sc.source));
    odin3_prov_hits first = odin3_prov_index_by_record(ix, sc.first_op);
    TEST_ASSERT_EQUAL_UINT32(PIECES + 1 + 1 + 2 + 1, first.count); /* gates, net, wire, clump */
    assert_node_hit(first, sc.gates[0][1], false);
    assert_node_hit(first, sc.clump, true);
    odin3_prov_hits back = odin3_prov_index_by_record(ix, sc.clump_rec);
    TEST_ASSERT_EQUAL_UINT32(1, back.count);
    assert_node_hit(back, sc.clump, true);
    odin3_srcloc other_line = sc.loc;
    other_line.line = 8;
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_index_by_loc(ix, other_line).count);
    odin3_srcloc other_file = sc.loc;
    other_file.file = intern("other.v");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_index_by_loc(ix, other_file).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_index_by_record(ix, (odin3_prov_id){0}).count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_index_by_record(ix, (odin3_prov_id){9999}).count);
    odin3_prov_index_destroy(ix);
}

/* --- several leaves, diamonds, deep chains --------------------------------------------------- */

static void test_sources_order_and_diamond(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id line1 = source_at(&rd, loc_at("m.v", 1, 1), 0);
    odin3_prov_id line2 = source_at(&rd, loc_at("m.v", 2, 1), 0);
    odin3_pass_ctx opt = run_named("opt");
    odin3_prov_begin_op(&opt);
    odin3_prov_id left = derive2(&opt, line2, line1);
    odin3_prov_begin_op(&opt);
    odin3_prov_id right = derive1(&opt, line1);
    odin3_prov_begin_op(&opt);
    odin3_prov_id top = derive2(&opt, right, left); /* diamond on line1 */
    leaves found = sources_of(top);
    TEST_ASSERT_EQUAL_UINT32(2, found.count);
    TEST_ASSERT_EQUAL_UINT32(line1.v, found.ids[0].v); /* depth first, parents in order */
    TEST_ASSERT_EQUAL_UINT32(line2.v, found.ids[1].v);
    found = sources_of(line1); /* line1 leaf is its own source */
    TEST_ASSERT_EQUAL_UINT32(1, found.count);
    TEST_ASSERT_EQUAL_UINT32(line1.v, found.ids[0].v);
    leaves none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_sources(design, (odin3_prov_id){0}, collect, &none));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_prov_sources(design, (odin3_prov_id){99}, collect, &none));
    TEST_ASSERT_EQUAL_UINT32(0, none.count);
    odin3_node_id node = node_with("$_OR_", top);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_prov_hits hits = odin3_prov_index_by_loc(ix, loc_at("m.v", 2, 1));
    TEST_ASSERT_EQUAL_UINT32(1, hits.count);
    assert_node_hit(hits, node, true);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_prov_index_by_loc(ix, loc_at("m.v", 1, 1)).count);
    odin3_prov_index_destroy(ix);
}

/* A visit callback that itself navigates (the walk is over before callbacks run). */
static void nested_visit(void *user, odin3_prov_id leaf) {
    uint32_t *calls = user;
    leaves inner = sources_of(leaf);
    TEST_ASSERT_EQUAL_UINT32(1, inner.count);
    TEST_ASSERT_EQUAL_UINT32(leaf.v, inner.ids[0].v);
    (*calls)++;
}

static odin3_prov_id chain_of(odin3_pass_ctx *ctx, odin3_prov_id root, uint32_t depth) {
    odin3_prov_id cur = root;
    for (uint32_t i = 0; i < depth; i++) {
        odin3_prov_begin_op(ctx);
        cur = derive1(ctx, cur);
    }
    return cur;
}

static void test_deep_chains(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("deep.v", 3, 1), 0);
    odin3_pass_ctx pass = run_named("deepen");
    odin3_prov_id ten = chain_of(&pass, src, 10);
    leaves found = sources_of(ten);
    TEST_ASSERT_EQUAL_UINT32(1, found.count);
    TEST_ASSERT_EQUAL_UINT32(src.v, found.ids[0].v);
    uint32_t calls = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_sources(design, ten, nested_visit, &calls));
    TEST_ASSERT_EQUAL_UINT32(1, calls);
    odin3_prov_id deep = chain_of(&pass, ten, DEEP); /* recursion would overflow the stack */
    found = sources_of(deep);
    TEST_ASSERT_EQUAL_UINT32(1, found.count);
    TEST_ASSERT_EQUAL_UINT32(src.v, found.ids[0].v);
    odin3_node_id node = node_with("$_NOT_", deep);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    assert_node_hit(odin3_prov_index_by_loc(ix, loc_at("deep.v", 3, 1)), node, true);
    assert_node_hit(odin3_prov_index_by_record(ix, ten), node, true);
    odin3_prov_index_destroy(ix);
}

/* --- hash-consing at scale ------------------------------------------------------------------ */

static void test_hash_consing_100k(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_srcloc loc = loc_at("big.v", 0, 1);
    uint32_t first = odin3_prov_end(design);
    for (uint32_t i = 0; i < BIG; i++) {
        loc.line = i;
        TEST_ASSERT_EQUAL_UINT32(first + i, source_at(&rd, loc, 0).v);
    }
    for (uint32_t i = 0; i < BIG; i++) { /* identical records dedupe */
        loc.line = i;
        TEST_ASSERT_EQUAL_UINT32(first + i, source_at(&rd, loc, 0).v);
    }
    TEST_ASSERT_EQUAL_UINT32(first + BIG, odin3_prov_end(design));
    odin3_pass_ctx lower = run_named("lower");
    odin3_prov_id parent = {first};
    odin3_prov_begin_op(&lower);
    odin3_prov_id shared = derive1(&lower, parent);
    for (uint32_t i = 0; i < BIG; i++) { /* one operation: one record */
        TEST_ASSERT_EQUAL_UINT32(shared.v, derive1(&lower, parent).v);
    }
    uint32_t base = odin3_prov_end(design);
    for (uint32_t i = 0; i < BIG; i++) { /* distinct operations never merge */
        odin3_prov_begin_op(&lower);
        TEST_ASSERT_EQUAL_UINT32(base + i, derive1(&lower, parent).v);
    }
    TEST_ASSERT_EQUAL_UINT32(base + BIG, odin3_prov_end(design));
}

/* --- tombstones and lookups ----------------------------------------------------------------- */

static void test_tombstones(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_tombstone_end(design));
    odin3_module_id mid = odin3_module_id_of(module);
    odin3_tombstone net = {mid, ODIN3_OBJ_NET, {0}, intern("old_net"), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_tombstone_add(design, &net));
    const odin3_tombstone *got = odin3_tombstone_get(design, 1);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OBJ_NET, got->kind);
    TEST_ASSERT_EQUAL_UINT32(intern("old_net"), got->name);
    TEST_ASSERT_NULL(odin3_tombstone_get(design, 0));
    TEST_ASSERT_NULL(odin3_tombstone_get(design, 2));
}

static void test_tombstone_invalid(void) {
    odin3_module_id mid = odin3_module_id_of(module);
    odin3_celltype_id and_type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_AND_"), &and_type));
    odin3_tombstone gate = {mid, ODIN3_OBJ_NODE, and_type, 0, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_tombstone_add(design, &gate));
    odin3_tombstone bad[7] = {
        {{0}, ODIN3_OBJ_NODE, and_type, 0, {0}},         /* no module */
        {{mid.v + 1}, ODIN3_OBJ_NODE, and_type, 0, {0}}, /* unknown module */
        {mid, ODIN3_OBJ_MODULE, {0}, 0, {0}},            /* modules are not deleted */
        {mid, ODIN3_OBJ_WIRE, {0}, 0, {1}},              /* no such record */
        {mid, ODIN3_OBJ_NODE, {0}, 0, {0}},              /* a node without a type */
        {mid, ODIN3_OBJ_NODE, {UINT32_MAX}, 0, {0}},     /* an unknown type */
        {mid, ODIN3_OBJ_NET, and_type, 0, {0}},          /* a net with a type */
    };
    for (uint32_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_tombstone_add(design, &bad[i]));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_tombstone_add(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_tombstone_add(NULL, &gate));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_tombstone_end(design));
    TEST_ASSERT_EQUAL_size_t(9, errors_logged);
}

static void test_lookups_out_of_range(void) {
    TEST_ASSERT_NULL(odin3_prov_get(design, (odin3_prov_id){0}));
    TEST_ASSERT_NULL(odin3_prov_get(design, (odin3_prov_id){1}));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_prov_end(design));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_parents(design, (odin3_prov_id){5}).count);
    odin3_prov_index *ix = odin3_prov_index_build(design); /* no records, no objects with prov */
    TEST_ASSERT_NOT_NULL(ix);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_prov_index_by_loc(ix, (odin3_srcloc){0}).count);
    odin3_prov_index_destroy(ix);
    odin3_prov_index_destroy(NULL);
}

static void test_null_arguments(void) {
    TEST_ASSERT_NULL(odin3_prov_index_build(NULL));
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("x.v", 1, 1), 0);
    leaves none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_sources(design, src, NULL, &none));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_prov_sources(NULL, src, collect, &none));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
}

static void test_module_prov_indexed(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("sub.v", 1, 1), 0);
    odin3_module_id sub = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("sub"), src, &sub));
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    odin3_prov_hits hits = odin3_prov_index_by_record(ix, src);
    TEST_ASSERT_EQUAL_UINT32(1, hits.count);
    TEST_ASSERT_EQUAL_UINT32(sub.v, hits.hits[0].module.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OBJ_MODULE, hits.hits[0].obj.kind);
    TEST_ASSERT_EQUAL_UINT32(sub.v, hits.hits[0].obj.id);
    TEST_ASSERT_TRUE(hits.hits[0].live);
    odin3_prov_index_destroy(ix);
}

/* --- forward index at scale: linear build ------------------------------------------------- */

/* Index memory is linear: under LINEAR_BYTES per record plus object slot. */
static void assert_index_linear(const odin3_prov_index *ix) {
    size_t slots = (size_t)odin3_module_node_end(module) + odin3_module_net_end(module) +
                   odin3_module_wire_end(module) + odin3_tombstone_end(design);
    size_t bound = LINEAR_BYTES * (odin3_prov_end(design) + slots);
    TEST_ASSERT_TRUE(odin3_prov_index_bytes(ix) < bound);
}

/* 100k operations deep, every step leaving a dead object behind (retained churn). */
static void test_index_dead_chain(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_srcloc loc = loc_at("churn.v", 5, 1);
    odin3_prov_id src = source_at(&rd, loc, 0);
    odin3_pass_ctx pass = run_named("churn");
    odin3_prov_id cur = src;
    for (uint32_t i = 0; i < DEEP; i++) {
        odin3_prov_begin_op(&pass);
        cur = derive1(&pass, cur);
        kill_node(node_with("$_NOT_", cur));
    }
    odin3_node_id last = node_with("$_NOT_", cur);
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    assert_index_linear(ix);
    odin3_prov_hits hits = odin3_prov_index_by_record(ix, src);
    TEST_ASSERT_EQUAL_UINT32(DEEP + 1, hits.count);
    TEST_ASSERT_EQUAL_UINT32(1, count_live(hits, true));
    assert_node_hit(hits, last, true);
    TEST_ASSERT_EQUAL_UINT32(DEEP + 1, odin3_prov_index_by_loc(ix, loc).count);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_prov_index_by_record(ix, cur).count); /* last dead + last */
    odin3_prov_index_destroy(ix);
}

/* 1000 sources clumped into one object, which is then decomposed into 1000 pieces. */
static void test_index_clump_then_decompose(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id srcs[WIDE];
    for (uint32_t i = 0; i < WIDE; i++) {
        srcs[i] = source_at(&rd, loc_at("wide.v", i + 1, 1), 0);
        kill_node(node_with("$_BUF_", srcs[i]));
    }
    odin3_pass_ctx clump = run_named("clump");
    odin3_prov_begin_op(&clump);
    odin3_prov_id whole = derive_n(&clump, srcs, WIDE);
    kill_node(node_with("$_AND_", whole));
    odin3_pass_ctx lower = run_named("lower");
    odin3_prov_begin_op(&lower);
    for (uint32_t i = 0; i < WIDE; i++) {
        (void)node_with("$_OR_", derive1(&lower, whole));
    }
    odin3_prov_index *ix = odin3_prov_index_build(design);
    TEST_ASSERT_NOT_NULL(ix);
    assert_index_linear(ix);
    odin3_prov_hits hits = odin3_prov_index_by_record(ix, srcs[0]);
    TEST_ASSERT_EQUAL_UINT32(1 + 1 + WIDE, hits.count);
    TEST_ASSERT_EQUAL_UINT32(2, count_live(hits, false));
    TEST_ASSERT_EQUAL_UINT32(1 + 1 + WIDE,
                             odin3_prov_index_by_loc(ix, loc_at("wide.v", 500, 9)).count);
    TEST_ASSERT_EQUAL_UINT32(WIDE + 1, odin3_prov_index_by_record(ix, whole).count);
    odin3_prov_index_destroy(ix);
}

/* --- out of memory -------------------------------------------------------------------------- */

static void exhaust_prov_arena(size_t need) {
    odin3_arena *arena = design->prov->arena;
    while (odin3_arena_bytes_reserved(arena) - odin3_arena_bytes_used(arena) >= need) {
        TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, 1));
    }
}

/* Sweeps fail points over one record creation; returns the number of injected failures. */
static uint32_t record_oom_sweep(odin3_pass_ctx *ctx, odin3_prov_list parents, odin3_srcloc loc) {
    uint32_t end = odin3_prov_end(design);
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    odin3_prov_id id = {0};
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        odin3_prov_origin origin = {&loc, 1, 0, 0};
        odin3_util_set_alloc_fail_after(fail_at);
        st = parents.count > 0 ? odin3_prov_derive(ctx, parents, &id)
                               : odin3_prov_source(ctx, &origin, &id);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_UINT32(end, odin3_prov_end(design));
            failures++;
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(end, id.v);
    TEST_ASSERT_EQUAL_UINT32(end + 1, odin3_prov_end(design));
    return failures;
}

/* Fills the record store to a page boundary with source records; returns the last location. */
static odin3_srcloc fill_record_page(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_srcloc loc = loc_at("oom.v", 0, 1);
    while (odin3_prov_end(design) < PROV_PAGE) {
        loc.line++;
        (void)source_at(&rd, loc, 0);
    }
    return loc;
}

/* The next derive needs a new record page and a new arena chunk. */
static void test_derive_oom_sweep(void) {
    odin3_srcloc loc = fill_record_page();
    odin3_pass_ctx lower = run_named("lower");
    odin3_prov_begin_op(&lower);
    exhaust_prov_arena(sizeof(odin3_prov_id));
    odin3_prov_id parent = {1};
    odin3_prov_list one = {&parent, 1};
    TEST_ASSERT_EQUAL_UINT32(2, record_oom_sweep(&lower, one, loc));
    TEST_ASSERT_EQUAL_UINT32(1,
                             odin3_prov_get(design, (odin3_prov_id){PROV_PAGE})->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(PROV_PAGE, derive1(&lower, (odin3_prov_id){1}).v); /* consed */
}

/*
 * Two parents (one repeated): the de-duplication scratch, the visit marks, a new record page and
 * a new arena chunk. Kept allocations would shift later fail points, so each try starts fresh.
 */
static void test_derive_parents_oom_sweep(void) {
    odin3_prov_id parents[3] = {{2}, {1}, {2}};
    odin3_prov_list list = {parents, 3};
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    odin3_prov_id id = {0};
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        fresh_design();
        (void)fill_record_page();
        odin3_pass_ctx lower = run_named("lower");
        odin3_prov_begin_op(&lower);
        exhaust_prov_arena(2 * sizeof(odin3_prov_id));
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_prov_derive(&lower, list, &id);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_UINT32(PROV_PAGE, odin3_prov_end(design));
            failures++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4, failures);
    TEST_ASSERT_EQUAL_UINT32(PROV_PAGE, id.v);
    const odin3_prov_record *rec = odin3_prov_get(design, id);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.ids[1].v);
}

/* The next source record grows the hash-cons index and needs a new arena chunk. */
static void test_source_oom_sweep(void) {
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_srcloc loc = loc_at("oom.v", 0, 1);
    while (odin3_prov_end(design) < INDEX_GROWS_AT) {
        loc.line++;
        (void)source_at(&rd, loc, 0);
    }
    exhaust_prov_arena(sizeof(odin3_srcloc));
    loc.line++;
    odin3_prov_list none = {NULL, 0};
    TEST_ASSERT_EQUAL_UINT32(2, record_oom_sweep(&rd, none, loc));
    for (uint32_t line = 1; line <= loc.line; line++) { /* every record still found */
        odin3_srcloc probe = loc;
        probe.line = line;
        TEST_ASSERT_EQUAL_UINT32(line, source_at(&rd, probe, 0).v);
    }
}

static void test_run_and_tombstone_oom(void) {
    uint32_t failures = 0;
    uint32_t name = intern("p");
    odin3_tombstone tomb = {odin3_module_id_of(module), ODIN3_OBJ_NET, {0}, 0, {0}};
    for (uint32_t i = 0; i < 2 * 8; i++) { /* vecs of 8 then 16 (slot 0 reserved) grow twice */
        odin3_status tst = ODIN3_ERR_NO_MEMORY;
        for (long fail_at = 0; fail_at < OOM_LIMIT && tst != ODIN3_OK; fail_at++) {
            odin3_pass_ctx ctx = {0};
            uint32_t runs = odin3_passrun_end(design);
            uint32_t tombs = odin3_tombstone_end(design);
            odin3_util_set_alloc_fail_after(fail_at);
            odin3_status st = odin3_pass_run_begin(design, name, &ctx);
            tst = st == ODIN3_OK ? odin3_tombstone_add(design, &tomb) : st;
            odin3_util_set_alloc_fail_after(-1);
            if (tst == ODIN3_OK) {
                continue;
            }
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, tst);
            TEST_ASSERT_EQUAL_UINT32(st == ODIN3_OK ? runs + 1 : runs, odin3_passrun_end(design));
            TEST_ASSERT_EQUAL_UINT32(tombs, odin3_tombstone_end(design));
            failures++;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, tst);
    }
    TEST_ASSERT_EQUAL_UINT32(4, failures); /* two growths each */
    TEST_ASSERT_EQUAL_UINT32(2 * 8 + 1, odin3_tombstone_end(design));
}

/* A fresh design with a 100-deep chain over one source; returns the chain's last record. */
static odin3_prov_id fresh_chain(void) {
    fresh_design();
    odin3_pass_ctx rd = run_named("read_verilog");
    odin3_prov_id src = source_at(&rd, loc_at("s.v", 1, 1), 0);
    odin3_pass_ctx pass = run_named("deepen");
    return chain_of(&pass, src, 100);
}

/* Each try starts from a fresh design, so try n fails the n-th allocation of a full walk. */
static void test_sources_oom_sweep(void) {
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    leaves found = {0};
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        odin3_prov_id deep = fresh_chain();
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_prov_sources(design, deep, collect, &found);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_UINT32(0, found.count); /* no call before success */
            failures++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, failures); /* marks, worklist, leaf list */
    TEST_ASSERT_EQUAL_UINT32(1, found.count);
}

/* Fresh design per try, so every allocation point of a full build fails once. */
static void test_index_oom_sweep(void) {
    chain_scenario sc;
    odin3_prov_index *ix = NULL;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT && ix == NULL; fail_at++) {
        fresh_design();
        memset(&sc, 0, sizeof sc);
        chain_build(&sc);
        odin3_util_set_alloc_fail_after(fail_at);
        ix = odin3_prov_index_build(design);
        odin3_util_set_alloc_fail_after(-1);
        failures += ix == NULL ? 1U : 0U;
    }
    TEST_ASSERT_NOT_NULL(ix);
    /* every allocation of the build fails once; ASan's leak check covers each cleanup */
    TEST_ASSERT_EQUAL_UINT32(INDEX_BUILD_ALLOCS, failures);
    chain_check_forward(&sc, odin3_prov_index_by_loc(ix, sc.loc));
    odin3_prov_index_destroy(ix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pass_runs);
    RUN_TEST(test_pass_run_invalid);
    RUN_TEST(test_source_hash_consing);
    RUN_TEST(test_source_record_fields);
    RUN_TEST(test_source_invalid);
    RUN_TEST(test_derive_invalid);
    RUN_TEST(test_derive_identity);
    RUN_TEST(test_review_focus_3);
    RUN_TEST(test_decompose_then_clump);
    RUN_TEST(test_sources_order_and_diamond);
    RUN_TEST(test_deep_chains);
    RUN_TEST(test_hash_consing_100k);
    RUN_TEST(test_tombstones);
    RUN_TEST(test_tombstone_invalid);
    RUN_TEST(test_lookups_out_of_range);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_module_prov_indexed);
    RUN_TEST(test_index_dead_chain);
    RUN_TEST(test_index_clump_then_decompose);
    RUN_TEST(test_derive_oom_sweep);
    RUN_TEST(test_derive_parents_oom_sweep);
    RUN_TEST(test_source_oom_sweep);
    RUN_TEST(test_run_and_tombstone_oom);
    RUN_TEST(test_sources_oom_sweep);
    RUN_TEST(test_index_oom_sweep);
    return UNITY_END();
}
