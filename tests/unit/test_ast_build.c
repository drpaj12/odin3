/*
 * test_ast_build.c — unit tests for the AST store: the slot table, every shape rule of §5, the
 * pending stack, AST-5 ordering, sealing, the depth and child caps, payloads, OOM, and the
 * design's per-run AST list with its keep levels.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/ast_test.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    CAPTURE = 4096,
    OOM_LIMIT = 64,
    HOOK_CHILDREN = 6,
    MAX_KIDS = 16,
    RANDOM_NODES = 10000,
    PAGE_NODES = 4096,
};

static odin3_design *design;
static odin3_ast *ast;
static char captured[CAPTURE];
static size_t errors_logged;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
    strncpy(captured, msg, sizeof captured - 1);
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    captured[0] = '\0';
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_id make(odin3_ast_spec spec, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, ids, n, &id));
    TEST_ASSERT_NOT_EQUAL(0, id.v);
    return id;
}

static odin3_ast_id ident(const char *name) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_IDENT, .name = intern(name)};
    return make(spec, NULL, 0);
}

static odin3_ast_spec spec_of(uint8_t kind, uint8_t sub) {
    return (odin3_ast_spec){.kind = kind, .sub = sub};
}

static uint32_t number_payload(void) {
    static const int dummy = 0; /* number.c (Task 3) owns the record; nothing reads it here */
    odin3_ast_payload_rec rec = {.kind = ODIN3_AST_PAYLOAD_NUMBER, .u.number = &dummy};
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_payload_push(ast, rec, &payload));
    return payload;
}

/* A spec for kind with its name and payload filled as its row requires. */
static odin3_ast_spec valid_spec(uint8_t kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    odin3_ast_spec spec = spec_of(kind, 0);
    if (info->name_rule == ODIN3_AST_NAME_REQ) {
        spec.name = intern("n");
    }
    if (info->payload == ODIN3_AST_PAYLOAD_NUMBER) {
        spec.payload = number_payload();
    } else if (info->payload == ODIN3_AST_PAYLOAD_REAL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 1.5, &spec.payload));
    } else if (info->payload == ODIN3_AST_PAYLOAD_TEXT) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_ast_text_new(ast, odin3_bytes_cstr("specify"), &spec.payload));
    }
    return spec;
}

/* Everything a failed builder call must leave as it was. */
typedef struct counts {
    uint32_t end, children, mark, payloads, units, max_height;
} counts;

static counts counts_now(void) {
    return (counts){odin3_ast_node_end(ast).v, (uint32_t)ast->children.len,
                    odin3_ast_mark(ast),       (uint32_t)ast->payloads.len,
                    odin3_ast_root_count(ast), odin3_ast_max_height(ast)};
}

static void assert_counts(counts want) {
    counts got = counts_now();
    TEST_ASSERT_EQUAL_UINT32(want.end, got.end);
    TEST_ASSERT_EQUAL_UINT32(want.children, got.children);
    TEST_ASSERT_EQUAL_UINT32(want.mark, got.mark);
    TEST_ASSERT_EQUAL_UINT32(want.payloads, got.payloads);
    TEST_ASSERT_EQUAL_UINT32(want.units, got.units);
    TEST_ASSERT_EQUAL_UINT32(want.max_height, got.max_height);
}

/* make must fail with want, log one error line, and change nothing. */
static void assert_rejected(odin3_status want, odin3_ast_spec spec, const odin3_ast_id *ids,
                            uint32_t n) {
    counts before = counts_now();
    size_t errors = errors_logged;
    odin3_ast_id id = {77};
    TEST_ASSERT_EQUAL_INT(want, odin3_ast_make(ast, &spec, ids, n, &id));
    TEST_ASSERT_EQUAL_UINT32(77, id.v);
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
    assert_counts(before);
}

/* A FILE f.v holding "assign y = a;\n" and the loc of its 1:1. */
static odin3_loc file_loc(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_srcfile_spec spec = {.name = intern("f.v"), .len = 14};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    return odin3_srcman_loc(sm, buf, 0);
}

/* --- the table ----------------------------------------------------------------------------- */

/* Spec §4.3 rows transcribed independently: name, slots, tail min, tail max (-1 none, -2 *). */
typedef struct row_want {
    const char *name;
    int slots, tail_min, tail_max;
} row_want;

static const row_want rows[] = {
    {"UNIT", 0, 0, -2},
    {"DIRECTIVE", 0, 0, -1},
    {"MODULE", 2, 0, -2},
    {"UDP_DECL", 0, 0, -1},
    {"CONFIG_DECL", 0, 0, -1},
    {"LIST", 0, 0, -2},
    {"PORT_REF", 1, 0, -1},
    {"PORT_DECL", 1, 1, -2},
    {"DECLARATOR", 1, 0, -2},
    {"RANGE", 2, 0, -1},
    {"NET_DECL", 3, 1, -2},
    {"VAR_DECL", 1, 1, -2},
    {"PARAM_DECL", 1, 1, -2},
    {"DEFPARAM", 2, 0, -1},
    {"GENVAR_DECL", 0, 1, -2},
    {"EVENT_DECL", 0, 1, -2},
    {"STRENGTH", 0, 0, -1},
    {"DELAY", 0, 1, 3},
    {"CONT_ASSIGN", 2, 1, -2},
    {"NET_ASSIGN", 2, 0, -1},
    {"ALWAYS", 1, 0, -1},
    {"INITIAL", 1, 0, -1},
    {"INSTANTIATION", 1, 1, -2},
    {"INSTANCE", 1, 0, -2},
    {"CONNECTION", 1, 0, -1},
    {"GATE_DECL", 2, 1, -2},
    {"FUNCTION_DECL", 4, 0, -1},
    {"TASK_DECL", 3, 0, -1},
    {"SPECIFY_BLOCK", 0, 0, -1},
    {"ATTR", 1, 0, -1},
    {"GENERATE", 0, 0, -2},
    {"GEN_FOR", 4, 0, -1},
    {"GEN_IF", 3, 0, -1},
    {"GEN_CASE", 1, 1, -2},
    {"GEN_BLOCK", 1, 0, -2},
    {"SEQ_BLOCK", 1, 0, -2},
    {"PAR_BLOCK", 1, 0, -2},
    {"BLOCKING_ASSIGN", 3, 0, -1},
    {"NONBLOCKING_ASSIGN", 3, 0, -1},
    {"PROC_CONT_ASSIGN", 2, 0, -1},
    {"IF", 3, 0, -1},
    {"CASE", 1, 1, -2},
    {"CASE_ITEM", 1, 0, -2},
    {"FOR", 4, 0, -1},
    {"WHILE", 2, 0, -1},
    {"REPEAT", 2, 0, -1},
    {"FOREVER", 1, 0, -1},
    {"TIMING_STMT", 2, 0, -1},
    {"WAIT", 2, 0, -1},
    {"DISABLE", 1, 0, -1},
    {"EVENT_TRIGGER", 1, 0, -1},
    {"TASK_CALL", 0, 0, -2},
    {"NULL_STMT", 0, 0, -1},
    {"EVENT_CONTROL", 1, 0, -2},
    {"EVENT_EXPR", 1, 0, -1},
    {"NUMBER", 0, 0, -1},
    {"REAL", 0, 0, -1},
    {"STRING", 0, 0, -1},
    {"IDENT", 0, 0, -1},
    {"HIER_NAME", 0, 1, -2},
    {"SELECT", 3, 0, -1},
    {"CONCAT", 0, 1, -2},
    {"REPLICATE", 2, 0, -1},
    {"UNARY", 1, 0, -1},
    {"BINARY", 2, 0, -1},
    {"TERNARY", 3, 0, -1},
    {"CALL", 0, 0, -2},
    {"MINTYPMAX", 3, 0, -1},
};

static void check_row(uint32_t kind) {
    const row_want *want = &rows[kind - 1];
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    TEST_ASSERT_NOT_NULL(info);
    TEST_ASSERT_EQUAL_UINT32(kind, info->kind);
    TEST_ASSERT_EQUAL_STRING(want->name, odin3_ast_kind_name(kind));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)want->slots, odin3_ast_slot_count(kind));
    TEST_ASSERT_EQUAL(want->tail_max != -1, odin3_ast_has_tail(kind));
    if (want->tail_max == -1) {
        return;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)want->tail_min, info->tail_min);
    uint32_t max = want->tail_max == -2 ? UINT32_MAX : (uint32_t)want->tail_max;
    TEST_ASSERT_EQUAL_UINT32(max, info->tail_max);
}

static void test_table_matches_spec(void) {
    TEST_ASSERT_EQUAL_UINT32(68, sizeof rows / sizeof rows[0]);
    TEST_ASSERT_EQUAL_UINT32(69, ODIN3_AST_KIND_COUNT);
    for (uint32_t k = 1; k < ODIN3_AST_KIND_COUNT; k++) {
        check_row(k);
    }
    TEST_ASSERT_EQUAL_STRING("NONE", odin3_ast_kind_name(0));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_kind_name(69));
    TEST_ASSERT_NULL(odin3_ast_kind_info_of(0));
    TEST_ASSERT_NULL(odin3_ast_kind_info_of(69));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_slot_count(69));
    TEST_ASSERT_FALSE(odin3_ast_has_tail(69));
}

/* The spelling of sub of a row, "" past its count (the table's own data). */
static const char *sub_text(const odin3_ast_kind_info *info, uint32_t sub) {
    return sub < info->sub_count ? info->sub_names[sub] : "";
}

static void test_sub_names_and_counts(void) {
    TEST_ASSERT_EQUAL_UINT32(10, ODIN3_AST_DIRECTIVE_COUNT);
    TEST_ASSERT_EQUAL_UINT32(12, ODIN3_AST_NET_DECL_COUNT);
    TEST_ASSERT_EQUAL_UINT32(26, ODIN3_AST_GATE_DECL_COUNT);
    TEST_ASSERT_EQUAL_UINT32(10, ODIN3_AST_UNARY_COUNT);
    TEST_ASSERT_EQUAL_UINT32(24, ODIN3_AST_BINARY_COUNT);
    TEST_ASSERT_EQUAL_UINT32(24, odin3_ast_kind_info_of(ODIN3_AST_BINARY)->sub_count);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_kind_info_of(ODIN3_AST_IDENT)->sub_count);
    TEST_ASSERT_EQUAL_STRING(
        "~^", sub_text(odin3_ast_kind_info_of(ODIN3_AST_UNARY), ODIN3_AST_UNARY_XNOR));
    TEST_ASSERT_EQUAL_STRING(
        ">>>", sub_text(odin3_ast_kind_info_of(ODIN3_AST_BINARY), ODIN3_AST_BINARY_ASHR));
    TEST_ASSERT_EQUAL_STRING("pulldown", sub_text(odin3_ast_kind_info_of(ODIN3_AST_GATE_DECL),
                                                  ODIN3_AST_GATE_DECL_PULLDOWN));
    TEST_ASSERT_EQUAL_STRING("pragma", sub_text(odin3_ast_kind_info_of(ODIN3_AST_DIRECTIVE),
                                                ODIN3_AST_DIRECTIVE_PRAGMA));
    /* the widest flag row (PORT_DECL) uses 9 bits */
    TEST_ASSERT_EQUAL_HEX16(0x1FF, ODIN3_AST_FM_PORT_DECL);
}

static void test_sub_name_of_node(void) {
    odin3_ast_id leaf = ident("x");
    odin3_ast_spec spec = {.kind = ODIN3_AST_EVENT_EXPR, .sub = ODIN3_AST_EVENT_EXPR_POSEDGE};
    odin3_ast_id edge = make(spec, &leaf, 1);
    TEST_ASSERT_EQUAL_STRING("posedge", odin3_ast_sub_name(ast, edge));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_sub_name(ast, leaf));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_sub_name(ast, (odin3_ast_id){0}));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_sub_name(NULL, edge));
}

typedef struct class_case {
    odin3_ast_class cls;
    uint32_t kind;
    bool want;
} class_case;

static const class_case class_cases[] = {
    {ODIN3_AST_CLS_E, ODIN3_AST_MINTYPMAX, true},
    {ODIN3_AST_CLS_S, ODIN3_AST_NULL_STMT, true},
    {ODIN3_AST_CLS_I, ODIN3_AST_NULL_STMT, true},
    {ODIN3_AST_CLS_S, ODIN3_AST_CASE_ITEM, false},
    {ODIN3_AST_CLS_D, ODIN3_AST_NET_DECL, true},
    {ODIN3_AST_CLS_I, ODIN3_AST_NET_DECL, true},
    {ODIN3_AST_CLS_D, ODIN3_AST_PORT_DECL, false},
    {ODIN3_AST_CLS_I, ODIN3_AST_PORT_DECL, true},
    {ODIN3_AST_CLS_U, ODIN3_AST_DIRECTIVE, true},
    {ODIN3_AST_CLS_I, ODIN3_AST_DIRECTIVE, true},
    {ODIN3_AST_CLS_U, ODIN3_AST_GENERATE, false},
    {ODIN3_AST_CLS_R, ODIN3_AST_RANGE, true},
    {ODIN3_AST_CLS_L, ODIN3_AST_LIST, true},
    {ODIN3_AST_CLS_C, ODIN3_AST_CONNECTION, true},
    {ODIN3_AST_CLS_X, ODIN3_AST_ATTR, true},
    {ODIN3_AST_CLS_TIMING, ODIN3_AST_EVENT_CONTROL, true},
    {ODIN3_AST_CLS_TIMING, ODIN3_AST_DELAY, true},
    {ODIN3_AST_CLS_HIER_PART, ODIN3_AST_SELECT, true},
    {ODIN3_AST_CLS_HIER_PART, ODIN3_AST_NUMBER, false},
    {ODIN3_AST_CLS_BODY, ODIN3_AST_GEN_IF, true},
    {ODIN3_AST_CLS_BODY, ODIN3_AST_IF, true},
    {ODIN3_AST_CLS_CASE_ITEM, ODIN3_AST_CASE_ITEM, true},
    {ODIN3_AST_CLS_X, 0, false},
    {ODIN3_AST_CLS_X, 69, false},
    {ODIN3_AST_CLS_NONE, ODIN3_AST_IDENT, false},
};

static void test_classes(void) {
    for (size_t i = 0; i < sizeof class_cases / sizeof class_cases[0]; i++) {
        const class_case *cc = &class_cases[i];
        TEST_ASSERT_EQUAL(cc->want, odin3_ast_class_has(cc->cls, odin3_ast_kind_info_of(cc->kind)));
    }
}

/* E and S are disjoint; D lies inside I. */
static void test_class_partition(void) {
    for (uint32_t k = 1; k < ODIN3_AST_KIND_COUNT; k++) {
        const odin3_ast_kind_info *info = odin3_ast_kind_info_of(k);
        bool expr = odin3_ast_class_has(ODIN3_AST_CLS_E, info);
        bool stmt = odin3_ast_class_has(ODIN3_AST_CLS_S, info);
        bool decl = odin3_ast_class_has(ODIN3_AST_CLS_D, info);
        TEST_ASSERT_FALSE(expr && stmt);
        TEST_ASSERT_TRUE(!decl || odin3_ast_class_has(ODIN3_AST_CLS_I, info));
    }
}

/* --- every kind at its minimum and maximum child counts ------------------------------------ */

/* count leaf children (slots first, then the tail). */
static void fill_kids(odin3_ast_id *kids, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        kids[i] = ident("leaf");
    }
}

/* Clears every optional slot (a minimal node keeps optional slots at 0). */
static void clear_optional(const odin3_ast_kind_info *info, odin3_ast_id *kids) {
    for (uint32_t i = 0; i < info->nslots; i++) {
        if ((info->slots[i] & ODIN3_AST_SLOT_OPT) != 0) {
            kids[i] = (odin3_ast_id){0};
        }
    }
}

static void check_built(odin3_ast_id id, uint8_t kind, const odin3_ast_id *kids, uint32_t count) {
    TEST_ASSERT_EQUAL_INT(kind, odin3_ast_kind_of(ast, id));
    TEST_ASSERT_EQUAL_UINT32(count, odin3_ast_nchild(ast, id));
    odin3_ast_span span = odin3_ast_children(ast, id);
    TEST_ASSERT_EQUAL_UINT32(count, span.n);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(kids[i].v, span.ids[i].v);
        TEST_ASSERT_EQUAL_UINT32(kids[i].v, odin3_ast_child(ast, id, i).v);
        TEST_ASSERT_TRUE(kids[i].v < id.v); /* AST-5 */
    }
}

/* The largest legal child count: the slots, plus the tail bound or the hooked child cap. */
static uint32_t max_kids(const odin3_ast_kind_info *info) {
    if (info->tail == ODIN3_AST_CLS_NONE) {
        return info->nslots;
    }
    return info->tail_max == UINT32_MAX ? HOOK_CHILDREN : info->nslots + info->tail_max;
}

static void check_kind_bounds(uint8_t kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    bool tail = info->tail != ODIN3_AST_CLS_NONE;
    uint32_t low = info->nslots + (tail ? info->tail_min : 0);
    uint32_t high = max_kids(info);
    odin3_ast_id kids[MAX_KIDS];
    fill_kids(kids, low);
    clear_optional(info, kids);
    check_built(make(valid_spec(kind), kids, low), kind, kids, low);
    fill_kids(kids, high);
    check_built(make(valid_spec(kind), kids, high), kind, kids, high);
    /* one more is out of bounds: INVALID_ARG past the table, PARSE past the hooked cap */
    fill_kids(kids, high + 1);
    bool capped = tail && info->tail_max == UINT32_MAX;
    assert_rejected(capped ? ODIN3_ERR_PARSE : ODIN3_ERR_INVALID_ARG, valid_spec(kind), kids,
                    high + 1);
}

static void test_every_kind_min_and_max(void) {
    odin3_ast_test_set_limits(
        ast, (odin3_ast_limits){HOOK_CHILDREN, ODIN3_AST_MAX_NODES, ODIN3_AST_MAX_DEPTH});
    for (uint32_t kind = 1; kind < ODIN3_AST_KIND_COUNT; kind++) {
        check_kind_bounds((uint8_t)kind);
    }
}

static void test_list_and_delay_bounds(void) {
    odin3_ast_id kids[4] = {ident("a"), ident("b"), ident("c"), ident("d")};
    make(spec_of(ODIN3_AST_LIST, 0), NULL, 0);
    make(spec_of(ODIN3_AST_LIST, 0), kids, 4);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_DELAY, 0), NULL, 0);
    make(spec_of(ODIN3_AST_DELAY, 0), kids, 1);
    make(spec_of(ODIN3_AST_DELAY, 0), kids, 3);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_DELAY, 0), kids, 4);
}

/* --- shape rules (§5): INVALID_ARG, a logged line, nothing changed ------------------------- */

static void test_shape_rules(void) {
    odin3_ast_id aa = ident("a");
    odin3_ast_id bb = ident("b");
    odin3_ast_id ab[2] = {aa, bb};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(69, 0), NULL, 0);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(0, 0), NULL, 0);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_BINARY, 24), ab, 2);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_LIST, 1), NULL, 0);
    odin3_ast_spec module = {.kind = ODIN3_AST_MODULE, .flags = 1U << 1, .name = intern("m")};
    odin3_ast_id none2[2] = {{0}, {0}};
    assert_rejected(ODIN3_ERR_INVALID_ARG, module, none2, 2);
    module.flags = ODIN3_AST_F_MODULE_MACROMODULE;
    make(module, none2, 2);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_BINARY, 0), ab, 1);
    odin3_ast_id three[3] = {aa, bb, aa};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_BINARY, 0), three, 3);
    odin3_ast_id four[4] = {aa, bb, aa, bb};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_DELAY, 0), four, 4);
    odin3_ast_id future[2] = {aa, odin3_ast_node_end(ast)};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_BINARY, 0), future, 2);
    odin3_ast_id zero_lhs[2] = {{0}, bb};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_BINARY, 0), zero_lhs, 2);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_IDENT, 0), NULL, 0);
    odin3_ast_spec named = {.kind = ODIN3_AST_BINARY, .name = intern("x")};
    assert_rejected(ODIN3_ERR_INVALID_ARG, named, ab, 2);
    uint32_t count = (uint32_t)odin3_strtab_count(odin3_design_strtab(design));
    odin3_ast_spec bad_name = {.kind = ODIN3_AST_IDENT, .name = count};
    assert_rejected(ODIN3_ERR_INVALID_ARG, bad_name, NULL, 0);
    uint32_t real = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 2.0, &real));
    odin3_ast_spec paid = {.kind = ODIN3_AST_IDENT, .name = intern("p"), .payload = real};
    assert_rejected(ODIN3_ERR_INVALID_ARG, paid, NULL, 0);
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_NUMBER, 0), NULL, 0);
    odin3_ast_spec wrong_type = {.kind = ODIN3_AST_NUMBER, .payload = real};
    assert_rejected(ODIN3_ERR_INVALID_ARG, wrong_type, NULL, 0);
    odin3_ast_spec past = {.kind = ODIN3_AST_REAL, .payload = real + 1};
    assert_rejected(ODIN3_ERR_INVALID_ARG, past, NULL, 0);
    odin3_ast_spec reserved = {.kind = ODIN3_AST_IDENT, .name = intern("r"), .reserved = {0, 1}};
    assert_rejected(ODIN3_ERR_INVALID_ARG, reserved, NULL, 0);
    TEST_ASSERT_NOT_NULL(strstr(captured, "odin3_ast_make: reserved fields must be zero"));
}

static void test_tail_zero_entries(void) {
    odin3_ast_id aa = ident("a");
    odin3_ast_id gap[3] = {aa, {0}, aa};
    assert_rejected(ODIN3_ERR_INVALID_ARG, spec_of(ODIN3_AST_CONCAT, 0), gap, 3);
    odin3_ast_spec call = {.kind = ODIN3_AST_CALL, .name = intern("f")};
    assert_rejected(ODIN3_ERR_INVALID_ARG, call, gap, 3);
    call.flags = ODIN3_AST_F_CALL_SYSTEM;
    call.name = intern("$display");
    check_built(make(call, gap, 3), ODIN3_AST_CALL, gap, 3);
    odin3_ast_spec task = {.kind = ODIN3_AST_TASK_CALL,
                           .flags = ODIN3_AST_F_TASK_CALL_SYSTEM,
                           .name = intern("$display")};
    check_built(make(task, gap, 3), ODIN3_AST_TASK_CALL, gap, 3);
    /* an optional slot may be 0; its index never moves */
    odin3_ast_id sel[3] = {aa, aa, {0}};
    odin3_ast_id id = make(spec_of(ODIN3_AST_SELECT, ODIN3_AST_SELECT_BIT), sel, 3);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(ast, id, 2).v);
}

static void test_null_arguments(void) {
    odin3_ast_spec spec = spec_of(ODIN3_AST_NULL_STMT, 0);
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make(NULL, &spec, NULL, 0, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make(ast, NULL, NULL, 0, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make(ast, &spec, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make(ast, &spec, NULL, 1, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make_marked(ast, NULL, 0, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_push(NULL, (odin3_ast_id){0}));
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_real_new(ast, 1.0, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_text_new(ast, (odin3_bytes){NULL, 3}, &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_intern(NULL, odin3_bytes_cstr("x"), &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_finish(NULL));
    odin3_ast *other = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_create(NULL, ODIN3_AST_FORM_PARSED,
                                                                  (odin3_passrun_id){1}, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_create(design, (odin3_ast_form)3,
                                                                  (odin3_passrun_id){1}, &other));
    TEST_ASSERT_NULL(other);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_node_end(ast).v);
}

/* --- pending stack --------------------------------------------------------------------------- */

static void test_nested_marks(void) {
    odin3_ast_id aa = ident("a");
    odin3_ast_id bb = ident("b");
    odin3_ast_id cc_id = ident("c");
    uint32_t outer = odin3_ast_mark(ast);
    TEST_ASSERT_EQUAL_UINT32(0, outer);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, aa));
    uint32_t inner = odin3_ast_mark(ast);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, bb));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, cc_id));
    odin3_ast_id cat = {0};
    odin3_ast_spec concat = spec_of(ODIN3_AST_CONCAT, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make_marked(ast, &concat, inner, &cat));
    TEST_ASSERT_EQUAL_UINT32(inner, odin3_ast_mark(ast));
    odin3_ast_id bc[2] = {bb, cc_id};
    check_built(cat, ODIN3_AST_CONCAT, bc, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, cat));
    odin3_ast_id top = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make_marked(ast, &concat, outer, &top));
    odin3_ast_id a_cat[2] = {aa, cat};
    check_built(top, ODIN3_AST_CONCAT, a_cat, 2);
    TEST_ASSERT_EQUAL_UINT32(outer, odin3_ast_mark(ast));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_ast_max_height(ast));
}

static void test_unwind_and_marked_errors(void) {
    odin3_ast_id aa = ident("a");
    uint32_t mark = odin3_ast_mark(ast);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, aa));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, (odin3_ast_id){0}));
    odin3_ast_spec binary = spec_of(ODIN3_AST_BINARY, 0);
    odin3_ast_id id = {0};
    counts before = counts_now();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make_marked(ast, &binary, mark, &id));
    assert_counts(before); /* the 0 rhs is rejected; the stack is kept */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make_marked(ast, &binary, 3, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_push(ast, odin3_ast_node_end(ast)));
    assert_counts(before);
    odin3_ast_unwind(ast, mark + 5); /* above the top: no-op */
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_mark(ast));
    odin3_ast_unwind(ast, mark);
    TEST_ASSERT_EQUAL_UINT32(mark, odin3_ast_mark(ast));
    /* an empty mark builds a node with no tail */
    odin3_ast_spec list = spec_of(ODIN3_AST_LIST, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make_marked(ast, &list, mark, &id));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(ast, id));
}

/* A view of the child table passed back to make is copied correctly even when it grows. */
static void test_make_from_own_children(void) {
    odin3_ast_id kids[8];
    for (uint32_t i = 0; i < 8; i++) {
        kids[i] = ident("k");
    }
    odin3_ast_id list = make(spec_of(ODIN3_AST_LIST, 0), kids, 8); /* fills the table to 8 */
    TEST_ASSERT_EQUAL_size_t(ast->children.cap, ast->children.len);
    odin3_ast_span view = odin3_ast_children(ast, list);
    odin3_ast_id copy = make(spec_of(ODIN3_AST_LIST, 0), view.ids, view.n);
    check_built(copy, ODIN3_AST_LIST, kids, 8);
}

/* --- AST-5 over a random build -----------------------------------------------------------------
 */

static uint32_t rng_state = 12345;

static uint32_t rng(uint32_t bound) {
    rng_state = rng_state * 1664525U + 1013904223U;
    return (rng_state >> 8) % bound;
}

static odin3_ast_id random_node(uint32_t made) {
    odin3_ast_id kids[3];
    uint32_t pick = made < 8 ? 0 : rng(4);
    for (uint32_t i = 0; i < 3; i++) {
        kids[i] = (odin3_ast_id){1 + rng(made > 0 ? made : 1)};
    }
    switch (pick) {
    case 1:
        return make(spec_of(ODIN3_AST_UNARY, (uint8_t)rng(ODIN3_AST_UNARY_COUNT)), kids, 1);
    case 2:
        return make(spec_of(ODIN3_AST_BINARY, (uint8_t)rng(ODIN3_AST_BINARY_COUNT)), kids, 2);
    case 3:
        return make(spec_of(ODIN3_AST_CONCAT, 0), kids, 1 + rng(3));
    default:
        return ident("x");
    }
}

static void test_ast5_random_build(void) {
    for (uint32_t made = 0; made < RANDOM_NODES; made++) {
        TEST_ASSERT_EQUAL_UINT32(made + 1, random_node(made).v);
    }
    uint32_t end = odin3_ast_node_end(ast).v;
    TEST_ASSERT_EQUAL_UINT32(RANDOM_NODES + 1, end);
    uint32_t max_height = 0;
    for (uint32_t node = 1; node < end; node++) {
        odin3_ast_span span = odin3_ast_children(ast, (odin3_ast_id){node});
        for (uint32_t i = 0; i < span.n; i++) {
            TEST_ASSERT_TRUE(span.ids[i].v != 0 && span.ids[i].v < node);
        }
        uint32_t height = ((const uint16_t *)ast->heights.data)[node];
        max_height = height > max_height ? height : max_height;
    }
    TEST_ASSERT_EQUAL_UINT32(max_height, odin3_ast_max_height(ast));
}

/* --- finish, accessors, payloads ------------------------------------------------------------ */

static void test_finish_seals(void) {
    odin3_ast_id aa = ident("a");
    odin3_ast_id unit = make(valid_spec(ODIN3_AST_UNIT), NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    odin3_ast_spec spec = spec_of(ODIN3_AST_NULL_STMT, 0);
    odin3_ast_id id = {0};
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make(ast, &spec, NULL, 0, &id));
    TEST_ASSERT_NOT_NULL(strstr(captured, "odin3_ast_make: the store is finished"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_push(ast, aa));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_make_marked(ast, &spec, 0, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_real_new(ast, 1.0, &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_text_new(ast, odin3_bytes_cstr("t"), &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(ast));
    odin3_ast_unwind(ast, 0);
    TEST_ASSERT_EQUAL_STRING("a", odin3_ast_name_str(ast, aa));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_max_height(ast));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_root_count(ast));
    TEST_ASSERT_EQUAL_UINT32(unit.v, odin3_ast_root(ast, 0).v);
    TEST_ASSERT_NOT_EQUAL(0, intern("still")); /* intern works after finish */
}

static void test_accessors(void) {
    odin3_loc at = file_loc();
    odin3_ast_spec yspec = {
        .kind = ODIN3_AST_IDENT, .loc = at, .end = {at.v + 1}, .name = intern("y")};
    odin3_ast_id lhs = make(yspec, NULL, 0);
    odin3_ast_id rhs = ident("z");
    odin3_ast_id kids[2] = {lhs, rhs};
    odin3_ast_id bin = make(spec_of(ODIN3_AST_BINARY, ODIN3_AST_BINARY_ADD), kids, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY, odin3_ast_kind_of(ast, bin));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_BINARY_ADD, odin3_ast_sub(ast, bin));
    TEST_ASSERT_EQUAL_UINT32(at.v, odin3_ast_loc(ast, lhs).v);
    TEST_ASSERT_EQUAL_UINT32(at.v + 1, odin3_ast_end(ast, lhs).v);
    TEST_ASSERT_EQUAL_UINT32(yspec.name, odin3_ast_name(ast, lhs));
    TEST_ASSERT_EQUAL_STRING("y", odin3_ast_name_str(ast, lhs));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_name_str(ast, bin));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_payload(ast, bin));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_FORM_PARSED, odin3_ast_form_of(ast));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_run(ast).v);
    TEST_ASSERT_TRUE(odin3_ast_design(ast) == design);
    TEST_ASSERT_TRUE(odin3_ast_bytes_reserved(ast) > 0);
}

static void test_payload_accessors(void) {
    uint32_t real = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 0.25, &real));
    odin3_ast_spec rspec = {.kind = ODIN3_AST_REAL, .payload = real};
    odin3_ast_id real_node = make(rspec, NULL, 0);
    uint32_t text = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_text_new(ast, odin3_bytes_cstr("specify\nendspecify"), &text));
    odin3_ast_spec tspec = {.kind = ODIN3_AST_SPECIFY_BLOCK, .payload = text};
    odin3_ast_id text_node = make(tspec, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(real, odin3_ast_payload(ast, real_node));
    TEST_ASSERT_EQUAL_UINT32(text, odin3_ast_payload(ast, text_node));
    TEST_ASSERT_TRUE(odin3_ast_real(ast, real_node) == 0.25);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(ast, real_node));
    odin3_bytes bytes = odin3_ast_text(ast, text_node);
    TEST_ASSERT_EQUAL_size_t(18, bytes.len);
    TEST_ASSERT_EQUAL_MEMORY("specify\nendspecify", bytes.ptr, 18);
    TEST_ASSERT_TRUE(odin3_ast_real(ast, text_node) == 0.0);
    TEST_ASSERT_NULL(odin3_ast_text(ast, real_node).ptr);
}

static void test_out_of_range_ids(void) {
    odin3_ast_id aa = ident("a");
    odin3_ast_id bad[3] = {{0}, {aa.v + 1}, {UINT32_MAX}};
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_AST_NONE, odin3_ast_kind_of(ast, bad[i]));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_sub(ast, bad[i]));
        TEST_ASSERT_EQUAL_UINT16(0, odin3_ast_flags(ast, bad[i]));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_loc(ast, bad[i]).v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_end(ast, bad[i]).v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_name(ast, bad[i]));
        TEST_ASSERT_EQUAL_STRING("", odin3_ast_name_str(ast, bad[i]));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(ast, bad[i]));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(ast, bad[i], 0).v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_children(ast, bad[i]).n);
        TEST_ASSERT_NULL(odin3_ast_children(ast, bad[i]).ids);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_payload(ast, bad[i]));
        TEST_ASSERT_TRUE(odin3_ast_real(ast, bad[i]) == 0.0);
        TEST_ASSERT_EQUAL_size_t(0, odin3_ast_text(ast, bad[i]).len);
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(ast, aa, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root(ast, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root_count(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NONE, odin3_ast_kind_of(NULL, aa));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_node_end(NULL).v);
}

static void test_null_store_getters(void) {
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root(NULL, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_max_height(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_run(NULL).v);
    TEST_ASSERT_NULL(odin3_ast_design(NULL));
}

static void test_units_are_roots(void) {
    odin3_ast_id first = make(valid_spec(ODIN3_AST_UNIT), NULL, 0);
    odin3_ast_id module_id = make(valid_spec(ODIN3_AST_MODULE), (odin3_ast_id[2]){{0}, {0}}, 2);
    odin3_ast_id second = make(valid_spec(ODIN3_AST_UNIT), &module_id, 1);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_root_count(ast));
    TEST_ASSERT_EQUAL_UINT32(first.v, odin3_ast_root(ast, 0).v);
    TEST_ASSERT_EQUAL_UINT32(second.v, odin3_ast_root(ast, 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root(ast, 2).v);
}

/* --- caps ------------------------------------------------------------------------------------ */

static void test_depth_cap(void) {
    odin3_loc at = file_loc();
    odin3_ast_spec unary = {.kind = ODIN3_AST_UNARY, .sub = ODIN3_AST_UNARY_MINUS, .loc = at};
    odin3_ast_id top = ident("x");
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &unary, &top, 1, &top));
    }
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH, odin3_ast_max_height(ast));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH + 1, odin3_ast_node_end(ast).v);
    assert_rejected(ODIN3_ERR_PARSE, unary, &top, 1);
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: nesting deeper than 32768", captured);
    /* a shallower node beside the chain still builds */
    odin3_ast_id leaf = ident("y");
    make(unary, &leaf, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH, odin3_ast_max_height(ast));
}

static void test_depth_hook(void) {
    odin3_ast_test_set_limits(ast,
                              (odin3_ast_limits){ODIN3_AST_MAX_CHILDREN, ODIN3_AST_MAX_NODES, 3});
    odin3_ast_id top = ident("x");
    top = make(spec_of(ODIN3_AST_UNARY, 0), &top, 1);
    top = make(spec_of(ODIN3_AST_UNARY, 0), &top, 1);
    assert_rejected(ODIN3_ERR_PARSE, spec_of(ODIN3_AST_UNARY, 0), &top, 1);
    TEST_ASSERT_NOT_NULL(strstr(captured, "nesting deeper than 3"));
    odin3_ast_test_set_limits(ast, (odin3_ast_limits){1, 1, UINT32_MAX});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH, ast->max_depth); /* clamped */
}

static void test_children_cap(void) {
    odin3_loc at = file_loc();
    odin3_ast_test_set_limits(
        ast, (odin3_ast_limits){HOOK_CHILDREN, ODIN3_AST_MAX_NODES, ODIN3_AST_MAX_DEPTH});
    odin3_ast_id kids[HOOK_CHILDREN + 1];
    for (uint32_t i = 0; i <= HOOK_CHILDREN; i++) {
        kids[i] = ident("k");
    }
    odin3_ast_spec concat = {.kind = ODIN3_AST_CONCAT, .loc = at};
    make(concat, kids, HOOK_CHILDREN);
    assert_rejected(ODIN3_ERR_PARSE, concat, kids, HOOK_CHILDREN + 1);
    TEST_ASSERT_EQUAL_STRING("f.v:1:1: error: more than 6 children", captured);
    uint32_t mark = odin3_ast_mark(ast);
    for (uint32_t i = 0; i <= HOOK_CHILDREN; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, kids[i]));
    }
    odin3_ast_id id = {0};
    counts before = counts_now();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ast_make_marked(ast, &concat, mark, &id));
    assert_counts(before);
}

static void test_node_cap(void) {
    odin3_ast_test_set_limits(ast,
                              (odin3_ast_limits){ODIN3_AST_MAX_CHILDREN, 2, ODIN3_AST_MAX_DEPTH});
    ident("a");
    ident("b");
    counts before = counts_now();
    odin3_ast_spec spec = spec_of(ODIN3_AST_NULL_STMT, 0);
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_ast_make(ast, &spec, NULL, 0, &id));
    assert_counts(before);
}

static void test_text_cap(void) {
    size_t len = (size_t)ODIN3_AST_MAX_TEXT_BYTES + 1;
    char *big = odin3_util_calloc(len);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'a', len);
    uint32_t payload = 0;
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_ast_text_new(ast, (odin3_bytes){big, len}, &payload));
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
    TEST_ASSERT_NOT_NULL(strstr(captured, "error: opaque text longer than 1048576 bytes"));
    TEST_ASSERT_EQUAL_size_t(0, ast->payloads.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_text_new(ast, (odin3_bytes){big, len - 1}, &payload));
    TEST_ASSERT_EQUAL_UINT32(1, payload);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_text_new(ast, (odin3_bytes){NULL, 0}, &payload));
    TEST_ASSERT_EQUAL_UINT32(2, payload);
    odin3_util_free(big);
}

/* --- OOM ------------------------------------------------------------------------------------- */

typedef odin3_status (*oom_step)(void);

/* Fills the node page, height vec and child table to capacity: the next make must grow all. */
static void fill_to_page(void) {
    TEST_ASSERT_EQUAL_size_t(0, ast->children.len);
    while (odin3_ast_node_end(ast).v < PAGE_NODES - 1) {
        ident("leaf");
    }
    uint32_t mark = odin3_ast_mark(ast);
    for (uint32_t i = 0; i < PAGE_NODES / 2; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, (odin3_ast_id){PAGE_NODES / 4 + i}));
    }
    odin3_ast_id id = {0};
    odin3_ast_spec list = spec_of(ODIN3_AST_LIST, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make_marked(ast, &list, mark, &id));
    TEST_ASSERT_EQUAL_UINT32(PAGE_NODES, odin3_ast_node_end(ast).v);
    TEST_ASSERT_EQUAL_size_t(ast->children.cap, ast->children.len);
    TEST_ASSERT_EQUAL_size_t(ast->heights.cap, ast->heights.len);
}

static void prep_marked(void) {
    fill_to_page();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, (odin3_ast_id){PAGE_NODES / 4}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, (odin3_ast_id){PAGE_NODES / 2}));
}

static odin3_ast_spec unit_spec(void) {
    return (odin3_ast_spec){.kind = ODIN3_AST_UNIT, .name = intern("u.v")};
}

/* UNITs until the unit list is full, then a full page. */
static void prep_unit(void) {
    odin3_ast_spec spec = unit_spec();
    odin3_ast_id id = {0};
    while (ast->units.len < ast->units.cap || ast->units.cap == 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &id));
    }
    fill_to_page();
}

static void prep_pending(void) {
    odin3_ast_id leaf = ident("a");
    while (ast->pending.len < ast->pending.cap || ast->pending.cap == 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_push(ast, leaf));
    }
}

static void prep_payloads(void) {
    uint32_t payload = 0;
    while (ast->payloads.len < ast->payloads.cap || ast->payloads.cap == 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 1.0, &payload));
    }
}

static odin3_status step_make(void) {
    odin3_ast_spec spec = spec_of(ODIN3_AST_BINARY, 0);
    odin3_ast_id kids[2] = {{1}, {2}};
    odin3_ast_id id = {0};
    return odin3_ast_make(ast, &spec, kids, 2, &id);
}

static odin3_status step_make_marked(void) {
    odin3_ast_spec spec = spec_of(ODIN3_AST_CONCAT, 0);
    odin3_ast_id id = {0};
    return odin3_ast_make_marked(ast, &spec, odin3_ast_mark(ast) - 2, &id);
}

static odin3_status step_unit(void) {
    odin3_ast_spec spec = unit_spec();
    odin3_ast_id id = {0};
    return odin3_ast_make(ast, &spec, NULL, 0, &id);
}

static odin3_status step_push(void) {
    return odin3_ast_push(ast, (odin3_ast_id){1});
}

static odin3_status step_real(void) {
    uint32_t payload = 0;
    return odin3_ast_real_new(ast, 2.0, &payload);
}

static odin3_status step_text(void) {
    static const char big[1U << 17] = {0}; /* larger than an arena chunk: its own allocation */
    uint32_t payload = 0;
    return odin3_ast_text_new(ast, (odin3_bytes){big, sizeof big}, &payload);
}

/*
 * Runs step at every failure point, each on a fresh store prepared without failures: every
 * failure is NO_MEMORY with the counts unchanged; returns the number of failure points.
 */
static long oom_sweep(oom_step step, void (*prepare)(void)) {
    for (long k = 0; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        prepare();
        counts before = counts_now();
        size_t arena = odin3_arena_bytes_used(ast->arena);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = step();
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            return k;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        assert_counts(before);
        TEST_ASSERT_EQUAL_size_t(arena, odin3_arena_bytes_used(ast->arena));
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT");
    return 0;
}

static void test_oom_builders(void) {
    /* node page, child table, height vec */
    TEST_ASSERT_EQUAL_INT(3, oom_sweep(step_make, fill_to_page));
    TEST_ASSERT_EQUAL_INT(3, oom_sweep(step_make_marked, prep_marked));
    /* node page, height vec, unit list (no children) */
    TEST_ASSERT_EQUAL_INT(3, oom_sweep(step_unit, prep_unit));
    TEST_ASSERT_EQUAL_INT(1, oom_sweep(step_push, prep_pending));
    TEST_ASSERT_EQUAL_INT(1, oom_sweep(step_real, prep_payloads));
    TEST_ASSERT_EQUAL_INT(2, oom_sweep(step_text, prep_payloads)); /* payload table, arena chunk */
}

static void test_oom_create_and_intern(void) {
    long points = 0;
    for (; points < OOM_LIMIT; points++) {
        odin3_ast *made = NULL;
        odin3_util_set_alloc_fail_after(points);
        odin3_status st =
            odin3_ast_create(design, ODIN3_AST_FORM_ELABORATED, (odin3_passrun_id){2}, &made);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            odin3_ast_destroy(made);
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(made);
    }
    TEST_ASSERT_TRUE(points >= 4); /* struct, page table/page, arena, heights */
    size_t before = odin3_strtab_count(odin3_design_strtab(design));
    uint32_t name = 0;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY,
                          odin3_ast_intern(ast, odin3_bytes_cstr("fresh_name"), &name));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_size_t(before, odin3_strtab_count(odin3_design_strtab(design)));
}

/* --- the design's AST list ---------------------------------------------------------------------
 */

static odin3_ast *new_store(odin3_ast_form form, uint32_t run) {
    odin3_ast *made = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, form, (odin3_passrun_id){run}, &made));
    return made;
}

static odin3_ast *held(odin3_ast_form form, uint32_t run) {
    odin3_ast *found = (odin3_ast *)&found; /* poisoned: get must overwrite it */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, (odin3_passrun_id){run}, form, &found));
    return found;
}

static void test_design_list(void) {
    odin3_ast *parsed = new_store(ODIN3_AST_FORM_PARSED, 5);
    odin3_ast *elab = new_store(ODIN3_AST_FORM_ELABORATED, 5);
    odin3_ast *dup = new_store(ODIN3_AST_FORM_PARSED, 5);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, parsed));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, elab));
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_ast(design, dup));
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
    odin3_ast_destroy(dup); /* still the caller's */
    TEST_ASSERT_TRUE(held(ODIN3_AST_FORM_PARSED, 5) == parsed);
    TEST_ASSERT_TRUE(held(ODIN3_AST_FORM_ELABORATED, 5) == elab);
    /* held stores are destroyed with the design (ASan reports a leak otherwise) */
}

static void test_design_get_missing(void) {
    TEST_ASSERT_NULL(held(ODIN3_AST_FORM_PARSED, 5));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_ast(design, new_store(ODIN3_AST_FORM_PARSED, 5)));
    TEST_ASSERT_NULL(held(ODIN3_AST_FORM_PARSED, 6));
    TEST_ASSERT_NULL(held(ODIN3_AST_FORM_ELABORATED, 5));
}

static void test_design_list_misuse(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_ast(design, NULL));
    odin3_ast *found = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_design_get_ast(NULL, (odin3_passrun_id){5}, ODIN3_AST_FORM_PARSED, &found));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_ast(design, (odin3_passrun_id){5},
                                                                      ODIN3_AST_FORM_PARSED, NULL));
    odin3_design *other = odin3_design_create();
    odin3_ast *foreign = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(other, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){9}, &foreign));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_set_ast(design, foreign));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(other, foreign));
    odin3_design_destroy(other); /* destroys foreign */
}

static void test_drop_keep_levels(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, odin3_design_keep_ast(design));
    static const odin3_keep_ast levels[3] = {ODIN3_KEEP_AST_NONE, ODIN3_KEEP_AST_ELABORATED,
                                             ODIN3_KEEP_AST_ALL};
    static const bool keeps_parsed[3] = {false, false, true};
    static const bool keeps_elab[3] = {false, true, true};
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t run = 10 + i;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_design_set_ast(design, new_store(ODIN3_AST_FORM_PARSED, run)));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_design_set_ast(design, new_store(ODIN3_AST_FORM_ELABORATED, run)));
        odin3_design_set_keep_ast(design, levels[i]);
        TEST_ASSERT_EQUAL_INT(levels[i], odin3_design_keep_ast(design));
        odin3_design_drop_ast(design, (odin3_passrun_id){run}, ODIN3_AST_FORM_PARSED);
        odin3_design_drop_ast(design, (odin3_passrun_id){run}, ODIN3_AST_FORM_ELABORATED);
        TEST_ASSERT_EQUAL(keeps_parsed[i], held(ODIN3_AST_FORM_PARSED, run) != NULL);
        TEST_ASSERT_EQUAL(keeps_elab[i], held(ODIN3_AST_FORM_ELABORATED, run) != NULL);
        odin3_design_drop_ast(design, (odin3_passrun_id){run + 100}, ODIN3_AST_FORM_PARSED);
    }
    /* a store dropped under NONE frees its slot: the same run and form can be set again */
    odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_ast(design, new_store(ODIN3_AST_FORM_PARSED, 10)));
    TEST_ASSERT_NOT_NULL(held(ODIN3_AST_FORM_ELABORATED, 12)); /* earlier entries survive */
}

static void test_design_list_oom(void) {
    odin3_ast *store = new_store(ODIN3_AST_FORM_PARSED, 20);
    odin3_util_set_alloc_fail_after(0); /* the first set allocates the list */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_design_set_ast(design, store));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NULL(held(ODIN3_AST_FORM_PARSED, 20)); /* not taken: still the caller's */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, store));
    TEST_ASSERT_TRUE(held(ODIN3_AST_FORM_PARSED, 20) == store);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_matches_spec);
    RUN_TEST(test_sub_names_and_counts);
    RUN_TEST(test_classes);
    RUN_TEST(test_class_partition);
    RUN_TEST(test_sub_name_of_node);
    RUN_TEST(test_every_kind_min_and_max);
    RUN_TEST(test_list_and_delay_bounds);
    RUN_TEST(test_shape_rules);
    RUN_TEST(test_tail_zero_entries);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_nested_marks);
    RUN_TEST(test_unwind_and_marked_errors);
    RUN_TEST(test_make_from_own_children);
    RUN_TEST(test_ast5_random_build);
    RUN_TEST(test_finish_seals);
    RUN_TEST(test_accessors);
    RUN_TEST(test_payload_accessors);
    RUN_TEST(test_out_of_range_ids);
    RUN_TEST(test_null_store_getters);
    RUN_TEST(test_units_are_roots);
    RUN_TEST(test_depth_cap);
    RUN_TEST(test_depth_hook);
    RUN_TEST(test_children_cap);
    RUN_TEST(test_node_cap);
    RUN_TEST(test_text_cap);
    RUN_TEST(test_oom_builders);
    RUN_TEST(test_oom_create_and_intern);
    RUN_TEST(test_design_list);
    RUN_TEST(test_design_get_missing);
    RUN_TEST(test_design_list_misuse);
    RUN_TEST(test_drop_keep_levels);
    RUN_TEST(test_design_list_oom);
    return UNITY_END();
}
