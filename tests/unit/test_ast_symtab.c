/*
 * test_ast_symtab.c — unit tests for the symbol table: library/module/generate scopes, indexed
 * and escaped names, outward lookup and shadowing, duplicates, assignment lists, value copies,
 * remap after compact, check rule 8, and OOM.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    NODES = 8,
    OOM_LIMIT = 64,
    PAGE = 4096,
    FILL_NAME = 100000,
    MAP_FILLS = 40,
    BIG_BITS = 70000,
};

static odin3_design *design;
static odin3_ast *ast;
static odin3_symtab *st;
static odin3_ast_id nodes[NODES + 1]; /* nodes[1..NODES] are IDENTs */
static size_t errors_logged;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
}

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

/* A fresh AST with NODES IDENT nodes and no symbol table. */
static void fresh_store(void) {
    odin3_ast_destroy(ast);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    for (uint32_t i = 1; i <= NODES; i++) {
        odin3_ast_spec spec = {.kind = ODIN3_AST_IDENT, .name = intern("x")};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, NULL, 0, &nodes[i]));
    }
    st = NULL;
}

/* fresh_store plus a symbol table. */
static void fresh_ast(void) {
    fresh_store();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symtab_create(ast, &st));
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_EQUAL_PTR(NULL, design);
    ast = NULL;
    fresh_ast();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    st = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_scope_id scope(odin3_scope_spec spec) {
    odin3_scope_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(st, &spec, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id.v);
    return id;
}

static odin3_scope_id named(uint8_t kind, odin3_scope_id parent, uint32_t name) {
    return scope((odin3_scope_spec){.kind = kind, .name = name, .parent = parent});
}

static odin3_scope_id indexed(odin3_scope_id parent, uint32_t name, int32_t index) {
    return scope((odin3_scope_spec){.kind = ODIN3_SCOPE_GENERATE,
                                    .name = name,
                                    .index = index,
                                    .has_index = true,
                                    .parent = parent});
}

static odin3_status try_scope(odin3_scope_spec spec) {
    odin3_scope_id id = {0};
    odin3_status status = odin3_scope_new(st, &spec, &id);
    if (status != ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(0, id.v);
    }
    return status;
}

static odin3_sym_id sym(odin3_scope_id in, uint8_t kind, uint32_t name) {
    odin3_symbol_spec spec = {.name = name, .scope = in, .kind = kind, .decl = nodes[1]};
    odin3_sym_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(st, &spec, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id.v);
    return id;
}

static odin3_status try_sym(odin3_symbol_spec spec) {
    odin3_sym_id id = {0};
    odin3_status status = odin3_symbol_new(st, &spec, &id);
    if (status != ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(0, id.v);
    }
    return status;
}

static odin3_scope_id child(odin3_scope_id parent, uint32_t name, int64_t index) {
    return odin3_scope_child(st, parent, name, index);
}

/* DESIGN -> LIBRARY "work" -> MODULE "top". */
typedef struct tree {
    odin3_scope_id design, lib, top;
} tree;

static tree base_tree(void) {
    tree tr;
    tr.design = named(ODIN3_SCOPE_DESIGN, (odin3_scope_id){0}, 0);
    tr.lib = named(ODIN3_SCOPE_LIBRARY, tr.design, intern("work"));
    tr.top = named(ODIN3_SCOPE_MODULE, tr.lib, intern("top"));
    return tr;
}

static void assert_check_ok(void) {
    size_t before = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symtab_check(st, ast));
    TEST_ASSERT_EQUAL_size_t(before, errors_logged);
}

/* --- lifetime ------------------------------------------------------------------------------ */

static void test_create_once(void) {
    TEST_ASSERT_EQUAL_PTR(st, odin3_ast_symtab(ast));
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_ast_symtab(NULL));
    odin3_symtab *other = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symtab_create(ast, &other));
    TEST_ASSERT_EQUAL_PTR(NULL, other);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symtab_create(NULL, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symtab_create(ast, NULL));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
    TEST_ASSERT_EQUAL_PTR(st, odin3_ast_symtab(ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_scope_count(st));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_symbol_count(st));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_scope_count(NULL));
    assert_check_ok();
}

/* --- scopes -------------------------------------------------------------------------------- */

static void test_library_scopes(void) {
    tree tr = base_tree();
    uint32_t top = intern("top");
    odin3_scope_id lib2 = named(ODIN3_SCOPE_LIBRARY, tr.design, intern("lib2"));
    odin3_scope_id top2 = named(ODIN3_SCOPE_MODULE, lib2, top);
    TEST_ASSERT_NOT_EQUAL_UINT32(tr.top.v, top2.v);
    TEST_ASSERT_EQUAL_UINT32(tr.lib.v, child(tr.design, intern("work"), ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(tr.top.v, child(tr.lib, top, ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(top2.v, child(lib2, top, ODIN3_SCOPE_NO_INDEX).v);
    /* the same module name in two libraries: two MODULE symbols, one per library scope */
    odin3_sym_id m1 = sym(tr.lib, ODIN3_SYM_MODULE, top);
    odin3_sym_id m2 = sym(lib2, ODIN3_SYM_MODULE, top);
    TEST_ASSERT_NOT_EQUAL_UINT32(m1.v, m2.v);
    TEST_ASSERT_EQUAL_UINT32(m1.v, odin3_symtab_lookup(st, tr.top, top).v);
    TEST_ASSERT_EQUAL_UINT32(m2.v, odin3_symtab_lookup(st, top2, top).v);
    /* child lists in creation order */
    TEST_ASSERT_EQUAL_UINT32(tr.lib.v, odin3_scope_first_child(st, tr.design).v);
    TEST_ASSERT_EQUAL_UINT32(lib2.v, odin3_scope_next_sibling(st, tr.lib).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_next_sibling(st, lib2).v);
    const odin3_scope *rec = odin3_scope_get(st, top2);
    TEST_ASSERT_NOT_EQUAL_PTR(NULL, rec);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SCOPE_MODULE, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(lib2.v, rec->parent.v);
    TEST_ASSERT_EQUAL_UINT32(top, rec->name);
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_scope_get(st, (odin3_scope_id){0}));
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_scope_get(st, (odin3_scope_id){top2.v + 1}));
    TEST_ASSERT_EQUAL_UINT32(5, odin3_symtab_scope_count(st));
    assert_check_ok();
}

static void test_scope_shape_misuse(void) {
    odin3_scope_id none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          try_scope((odin3_scope_spec){.kind = ODIN3_SCOPE_LIBRARY}));
    tree tr = base_tree();
    odin3_scope_spec bad[] = {
        {.kind = 0, .parent = tr.top},
        {.kind = ODIN3_SCOPE_KIND_END, .parent = tr.top},
        {.kind = ODIN3_SCOPE_DESIGN, .parent = none},      /* a second DESIGN */
        {.kind = ODIN3_SCOPE_DESIGN, .parent = tr.design}, /* DESIGN with a parent */
        {.kind = ODIN3_SCOPE_LIBRARY, .name = 7, .parent = tr.top},
        {.kind = ODIN3_SCOPE_MODULE, .name = 7, .parent = tr.design},
        {.kind = ODIN3_SCOPE_BLOCK, .parent = tr.lib},
        {.kind = ODIN3_SCOPE_BLOCK, .parent = tr.design},
        {.kind = ODIN3_SCOPE_BLOCK, .parent = {99}}, /* no such parent */
        {.kind = ODIN3_SCOPE_BLOCK, .parent = none}, /* no parent */
        {.kind = ODIN3_SCOPE_MODULE, .name = 8, .parent = tr.lib, .has_index = true},
        {.kind = ODIN3_SCOPE_BLOCK, .parent = tr.top, .index = 3}, /* index without has_index */
        {.kind = ODIN3_SCOPE_BLOCK, .parent = tr.top, .node = {NODES + 1}},
        {.kind = ODIN3_SCOPE_MODULE, .name = intern("top"), .parent = tr.lib}, /* duplicate */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, try_scope(bad[i]), "bad spec");
    }
    odin3_scope_spec ok = {.kind = ODIN3_SCOPE_BLOCK, .parent = tr.top};
    odin3_scope_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_new(NULL, &ok, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_new(st, NULL, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_new(st, &ok, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_symtab_scope_count(st));
    TEST_ASSERT_EQUAL_size_t(sizeof bad / sizeof bad[0] + 4, errors_logged);
    /* unnamed scopes never clash; FUNCTION/TASK nest in blocks */
    odin3_scope_id b1 = scope(ok);
    odin3_scope_id b2 = scope(ok);
    TEST_ASSERT_NOT_EQUAL_UINT32(b1.v, b2.v);
    named(ODIN3_SCOPE_FUNCTION, b1, intern("f"));
    named(ODIN3_SCOPE_TASK, tr.top, intern("t"));
    assert_check_ok();
}

static void test_generate_indices(void) {
    tree tr = base_tree();
    uint32_t gname = intern("g");
    uint32_t bname = intern("b");
    /* created out of order: the chain is kept in index order */
    odin3_scope_id g3 = indexed(tr.top, gname, 3);
    odin3_scope_id g1 = indexed(tr.top, gname, 1);
    odin3_scope_id g0 = indexed(tr.top, gname, 0);
    odin3_scope_id g2 = indexed(tr.top, gname, 2);
    odin3_scope_id gm = indexed(tr.top, gname, -1);
    odin3_scope_id blk = named(ODIN3_SCOPE_BLOCK, tr.top, bname);
    odin3_scope_id esc = named(ODIN3_SCOPE_BLOCK, tr.top, intern("g[1]")); /* escaped \g[1] */
    TEST_ASSERT_EQUAL_UINT32(g2.v, child(tr.top, gname, 2).v);
    TEST_ASSERT_EQUAL_UINT32(g0.v, child(tr.top, gname, 0).v);
    TEST_ASSERT_EQUAL_UINT32(g3.v, child(tr.top, gname, 3).v);
    TEST_ASSERT_EQUAL_UINT32(gm.v, child(tr.top, gname, -1).v);
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, gname, 4).v);
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, gname, ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, gname, INT64_MAX).v);
    TEST_ASSERT_EQUAL_UINT32(blk.v, child(tr.top, bname, ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, bname, 0).v);
    TEST_ASSERT_EQUAL_UINT32(esc.v, child(tr.top, intern("g[1]"), ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(g1.v, child(tr.top, gname, 1).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(esc.v, g1.v);
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, intern("g[1]"), 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, child((odin3_scope_id){0}, gname, 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_child(NULL, tr.top, gname, 1).v);
    /* the chain: -1, 0, 1, 2, 3 */
    odin3_scope_id order[] = {gm, g0, g1, g2, g3};
    odin3_scope_id at = gm;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(order[i].v, at.v);
        TEST_ASSERT_EQUAL_INT32((int32_t)i - 1, odin3_scope_get(st, at)->index);
        at = odin3_scope_get(st, at)->same_name_next;
    }
    TEST_ASSERT_EQUAL_UINT32(0, at.v);
    /* a duplicate index clashes; a plain block gname then sits first in the chain */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          try_scope((odin3_scope_spec){.kind = ODIN3_SCOPE_GENERATE,
                                                       .name = gname,
                                                       .index = 2,
                                                       .has_index = true,
                                                       .parent = tr.top}));
    odin3_scope_id plain = named(ODIN3_SCOPE_BLOCK, tr.top, gname);
    TEST_ASSERT_EQUAL_UINT32(plain.v, child(tr.top, gname, ODIN3_SCOPE_NO_INDEX).v);
    TEST_ASSERT_EQUAL_UINT32(gm.v, odin3_scope_get(st, plain)->same_name_next.v);
    TEST_ASSERT_EQUAL_UINT32(g2.v, child(tr.top, gname, 2).v);
    assert_check_ok();
}

static void test_set_name(void) {
    tree tr = base_tree();
    uint32_t gen = intern("genblk1");
    odin3_scope_id anon = scope((odin3_scope_spec){
        .kind = ODIN3_SCOPE_GENERATE, .index = 4, .has_index = true, .parent = tr.top});
    odin3_scope_id other = scope((odin3_scope_spec){
        .kind = ODIN3_SCOPE_GENERATE, .index = 4, .has_index = true, .parent = tr.top});
    TEST_ASSERT_EQUAL_UINT32(0, child(tr.top, 0, 4).v); /* unnamed scopes are not in the map */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_set_name(st, anon, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_set_name(st, anon, gen));
    TEST_ASSERT_EQUAL_UINT32(anon.v, child(tr.top, gen, 4).v);
    TEST_ASSERT_EQUAL_UINT32(gen, odin3_scope_get(st, anon)->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_set_name(st, anon, intern("y")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_set_name(st, other, gen)); /* clash */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_get(st, other)->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_scope_set_name(st, (odin3_scope_id){99}, gen));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_scope_set_name(NULL, anon, gen));
    TEST_ASSERT_EQUAL_size_t(5, errors_logged);
    assert_check_ok();
}

/* --- symbols ------------------------------------------------------------------------------- */

static void test_lookup_outward_and_shadowing(void) {
    tree tr = base_tree();
    uint32_t aname = intern("a");
    uint32_t bname = intern("b");
    odin3_sym_id top_a = sym(tr.top, ODIN3_SYM_NET, aname);
    odin3_sym_id top_b = sym(tr.top, ODIN3_SYM_VAR, bname);
    odin3_scope_id blk = named(ODIN3_SCOPE_BLOCK, tr.top, intern("blk"));
    odin3_scope_id inner = named(ODIN3_SCOPE_BLOCK, blk, 0);
    odin3_sym_id blk_a = sym(blk, ODIN3_SYM_VAR, aname);
    TEST_ASSERT_EQUAL_UINT32(blk_a.v, odin3_symtab_lookup(st, inner, aname).v); /* shadows top.a */
    TEST_ASSERT_EQUAL_UINT32(top_a.v, odin3_symtab_lookup(st, tr.top, aname).v);
    TEST_ASSERT_EQUAL_UINT32(top_b.v, odin3_symtab_lookup(st, inner, bname).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup_local(st, inner, bname).v);
    TEST_ASSERT_EQUAL_UINT32(top_b.v, odin3_symtab_lookup_local(st, tr.top, bname).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup(st, inner, intern("c")).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup(st, (odin3_scope_id){0}, aname).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup(st, (odin3_scope_id){99}, aname).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup(NULL, inner, aname).v);
    /* per-scope symbol lists in ID order */
    TEST_ASSERT_EQUAL_UINT32(top_a.v, odin3_scope_first_symbol(st, tr.top).v);
    TEST_ASSERT_EQUAL_UINT32(top_b.v, odin3_scope_next_symbol(st, top_a).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_next_symbol(st, top_b).v);
    TEST_ASSERT_EQUAL_UINT32(blk_a.v, odin3_scope_first_symbol(st, blk).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_first_symbol(st, inner).v);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_symtab_symbol_count(st));
    assert_check_ok();
}

static void test_symbol_misuse(void) {
    tree tr = base_tree();
    uint32_t aname = intern("a");
    sym(tr.top, ODIN3_SYM_NET, aname);
    odin3_symbol_spec ok = {.name = intern("b"), .scope = tr.top, .kind = ODIN3_SYM_NET};
    odin3_symbol_spec bad[] = {ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok};
    bad[0].name = aname; /* duplicate (scope, name) */
    bad[1].name = 0;
    bad[2].kind = 0;
    bad[3].kind = ODIN3_SYM_KIND_END;
    bad[4].flags = ODIN3_SYM_F_ASSIGNED_BLOCKING;
    bad[5].flags = ODIN3_SYM_F_MASK + 1;
    bad[6].dir = ODIN3_AST_PORT_DECL_COUNT + 1;
    bad[7].msb = 3; /* without has_packed */
    bad[8].decl = (odin3_ast_id){NODES + 1};
    bad[9].declarator = (odin3_ast_id){NODES + 1};
    bad[10].scope = (odin3_scope_id){99};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, try_sym(bad[i]), "bad spec");
    }
    odin3_sym_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_new(NULL, &ok, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_new(st, NULL, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_new(st, &ok, NULL));
    TEST_ASSERT_EQUAL_size_t(sizeof bad / sizeof bad[0] + 3, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_symtab_symbol_count(st));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, try_sym(ok));
    assert_check_ok();
}

static void test_symbol_fields(void) {
    tree tr = base_tree();
    odin3_symbol_spec spec = {.name = intern("p"),
                              .scope = tr.top,
                              .kind = ODIN3_SYM_NET,
                              .sub = 2,
                              .flags = ODIN3_SYM_F_PORT | ODIN3_SYM_F_SIGNED,
                              .dir = 1 + ODIN3_AST_PORT_DECL_OUTPUT,
                              .decl = nodes[2],
                              .declarator = nodes[3],
                              .msb = 0,
                              .lsb = 7,
                              .has_packed = true};
    odin3_sym_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(st, &spec, &id));
    const odin3_dim dims[] = {{0, 15}, {3, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_set_unpacked(st, id, dims, 2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_add_flags(st, id, ODIN3_SYM_F_OVERRIDDEN));
    const odin3_symbol *rec = odin3_symbol_get(st, id);
    TEST_ASSERT_EQUAL_UINT32(spec.name, rec->name);
    TEST_ASSERT_EQUAL_UINT32(tr.top.v, rec->scope.v);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_NET, rec->kind);
    TEST_ASSERT_EQUAL_UINT8(2, rec->sub);
    TEST_ASSERT_EQUAL_UINT8(1 + ODIN3_AST_PORT_DECL_OUTPUT, rec->dir);
    TEST_ASSERT_EQUAL_HEX16(ODIN3_SYM_F_PORT | ODIN3_SYM_F_SIGNED | ODIN3_SYM_F_OVERRIDDEN,
                            rec->flags);
    TEST_ASSERT_EQUAL_UINT32(nodes[2].v, rec->decl.v);
    TEST_ASSERT_EQUAL_UINT32(nodes[3].v, rec->declarator.v);
    TEST_ASSERT_EQUAL_INT(1, rec->has_packed);
    TEST_ASSERT_EQUAL_INT32(0, rec->packed.msb); /* ascending [0:7] preserved */
    TEST_ASSERT_EQUAL_INT32(7, rec->packed.lsb);
    odin3_dim_span span = odin3_symbol_unpacked(st, id);
    TEST_ASSERT_EQUAL_UINT32(2, span.n);
    TEST_ASSERT_EQUAL_INT32(15, span.dims[0].lsb);
    TEST_ASSERT_EQUAL_INT32(3, span.dims[1].msb);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_unpacked(st, (odin3_sym_id){99}).n);
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_symbol_get(st, (odin3_sym_id){0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_symbol_add_flags(st, id, ODIN3_SYM_F_ASSIGNED_CONTINUOUS));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_set_unpacked(st, id, NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_symbol_set_unpacked(st, (odin3_sym_id){99}, dims, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_set_unpacked(st, id, NULL, 0)); /* clears */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_unpacked(st, id).n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_symbol_set_ir(st, id, (odin3_module_id){4}, (odin3_wire_id){9}));
    TEST_ASSERT_EQUAL_UINT32(4, rec->ir_module.v);
    TEST_ASSERT_EQUAL_UINT32(9, rec->ir_wire.v);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_symbol_set_ir(st, (odin3_sym_id){0}, (odin3_module_id){4}, (odin3_wire_id){9}));
    assert_check_ok();
}

static void test_assign_lists(void) {
    tree tr = base_tree();
    odin3_sym_id var = sym(tr.top, ODIN3_SYM_VAR, intern("v"));
    odin3_sym_id wsym = sym(tr.top, ODIN3_SYM_NET, intern("w"));
    TEST_ASSERT_EQUAL_HEX16(0, odin3_symbol_get(st, var)->flags);
    const odin3_assign_kind kinds[] = {ODIN3_ASSIGN_NONBLOCKING, ODIN3_ASSIGN_BLOCKING,
                                       ODIN3_ASSIGN_CONTINUOUS, ODIN3_ASSIGN_BLOCKING};
    const uint32_t at[] = {5, 2, 7, 3}; /* source order, not node order */
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_add_assign(st, var, nodes[at[i]], kinds[i]));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_symbol_add_assign(st, wsym, nodes[1], ODIN3_ASSIGN_CONTINUOUS));
    odin3_assign_id it = odin3_symbol_first_assign(st, var);
    for (size_t i = 0; i < 4; i++) {
        const odin3_sym_assign *rec = odin3_assign_get(st, it);
        TEST_ASSERT_NOT_EQUAL_PTR(NULL, rec);
        TEST_ASSERT_EQUAL_UINT32(nodes[at[i]].v, rec->node.v);
        TEST_ASSERT_EQUAL_UINT8(kinds[i], rec->kind);
        it = odin3_symbol_next_assign(st, it);
    }
    TEST_ASSERT_EQUAL_UINT32(0, it.v);
    TEST_ASSERT_EQUAL_HEX16(ODIN3_SYM_F_ASSIGNED_MASK, odin3_symbol_get(st, var)->flags);
    TEST_ASSERT_EQUAL_HEX16(ODIN3_SYM_F_ASSIGNED_CONTINUOUS, odin3_symbol_get(st, wsym)->flags);
    TEST_ASSERT_EQUAL_UINT32(odin3_symbol_get(st, wsym)->first_assign.v,
                             odin3_symbol_get(st, wsym)->last_assign.v);
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_assign_get(st, (odin3_assign_id){0}));
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_assign_get(st, (odin3_assign_id){99}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_first_assign(st, (odin3_sym_id){99}).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_add_assign(st, var, (odin3_ast_id){0},
                                                                         ODIN3_ASSIGN_BLOCKING));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_symbol_add_assign(st, var, (odin3_ast_id){NODES + 1}, ODIN3_ASSIGN_BLOCKING));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_symbol_add_assign(st, var, nodes[1], (odin3_assign_kind)0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_symbol_add_assign(st, var, nodes[1], ODIN3_ASSIGN_KIND_END));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_symbol_add_assign(st, (odin3_sym_id){99}, nodes[1], ODIN3_ASSIGN_BLOCKING));
    assert_check_ok();
}

static void test_set_value_copies(void) {
    tree tr = base_tree();
    odin3_sym_id param = sym(tr.top, ODIN3_SYM_PARAM, intern("P"));
    uint8_t bits[] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X, ODIN3_BIT_Z};
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = bits, .len = 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_set_value(st, param, &val, true));
    bits[0] = ODIN3_BIT_Z; /* the caller's buffer changes; the stored copy does not */
    const odin3_symbol *rec = odin3_symbol_get(st, param);
    TEST_ASSERT_EQUAL_INT(1, rec->has_value);
    TEST_ASSERT_EQUAL_INT(1, rec->value_signed);
    TEST_ASSERT_NOT_EQUAL_PTR(bits, rec->value.bits);
    TEST_ASSERT_EQUAL_UINT32(4, rec->value.len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, rec->value.bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_Z, rec->value.bits[3]);
    odin3_value bad = {.kind = ODIN3_VAL_BITS, .bits = NULL, .len = 3};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_set_value(st, param, &bad, false));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symbol_set_value(st, param, NULL, false));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_symbol_set_value(st, (odin3_sym_id){99}, &val, false));
    TEST_ASSERT_EQUAL_INT(1, rec->value_signed); /* unchanged by the failures */
    odin3_value num = odin3_value_int(-5);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_set_value(st, param, &num, false));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, rec->value.kind);
    TEST_ASSERT_EQUAL_INT64(-5, rec->value.i);
    TEST_ASSERT_EQUAL_INT(0, rec->value_signed);
    assert_check_ok();
}

/* --- remap and check ----------------------------------------------------------------------- */

static odin3_sym_id with_ir(odin3_scope_id in, const char *name, uint32_t module, uint32_t wire) {
    odin3_sym_id id = sym(in, ODIN3_SYM_NET, intern(name));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_symbol_set_ir(st, id, (odin3_module_id){module}, (odin3_wire_id){wire}));
    return id;
}

static void test_remap(void) {
    tree tr = base_tree();
    odin3_sym_id live = with_ir(tr.top, "live", 1, 5);
    odin3_sym_id dead = with_ir(tr.top, "dead", 1, 7);
    odin3_sym_id other = with_ir(tr.top, "other", 2, 5);
    odin3_sym_id unset = with_ir(tr.top, "unset", 1, 0);
    odin3_sym_id beyond = with_ir(tr.top, "beyond", 1, 20);
    uint32_t wire[10] = {0, 1, 2, 0, 0, 3, 4, 0, 5, 6}; /* old 5 -> 3, old 7 dead */
    odin3_compact_map map = {.wire = wire, .n_wire = 10};
    odin3_symtab_remap(st, (odin3_module_id){1}, &map);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_symbol_get(st, live)->ir_wire.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_get(st, dead)->ir_wire.v);
    TEST_ASSERT_EQUAL_UINT32(5, odin3_symbol_get(st, other)->ir_wire.v); /* other module */
    TEST_ASSERT_EQUAL_UINT32(2, odin3_symbol_get(st, other)->ir_module.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_get(st, unset)->ir_wire.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_get(st, beyond)->ir_wire.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_symbol_get(st, live)->ir_module.v);
    odin3_symtab_remap(st, (odin3_module_id){1}, NULL); /* no-op */
    odin3_symtab_remap(NULL, (odin3_module_id){1}, &map);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_symbol_get(st, live)->ir_wire.v);
    assert_check_ok();
}

/* Everything the earlier tests build, in one table. */
static void build_everything(void) {
    tree tr = base_tree();
    odin3_scope_id lib2 = named(ODIN3_SCOPE_LIBRARY, tr.design, intern("lib2"));
    named(ODIN3_SCOPE_MODULE, lib2, intern("top"));
    uint32_t gname = intern("g");
    for (int32_t i = 3; i >= 0; i--) {
        odin3_scope_id gen = indexed(tr.top, gname, i);
        sym(gen, ODIN3_SYM_VAR, intern("v"));
    }
    named(ODIN3_SCOPE_BLOCK, tr.top, intern("g[1]"));
    odin3_sym_id asym = sym(tr.top, ODIN3_SYM_NET, intern("a"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_symbol_add_assign(st, asym, nodes[4], ODIN3_ASSIGN_CONTINUOUS));
    const odin3_dim dims[] = {{0, 3}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_set_unpacked(st, asym, dims, 1));
    odin3_scope_id blk = scope(
        (odin3_scope_spec){.kind = ODIN3_SCOPE_BLOCK, .parent = tr.top, .node = nodes[NODES]});
    sym(blk, ODIN3_SYM_VAR, intern("a"));
}

static void test_check(void) {
    build_everything();
    assert_check_ok();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symtab_check(NULL, ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_symtab_check(st, NULL));
    /* against a smaller store, the AST node IDs are out of range */
    odin3_ast *small = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){2}, &small));
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_symtab_check(st, small));
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(
        3, errors_logged); /* decls, an assignment node, a scope node */
    odin3_ast_destroy(small);
}

/* --- OOM ----------------------------------------------------------------------------------- */

static tree oom_tree;
static long armed; /* the allocation the sweep makes fail */

/* Arms the failure injection; each op calls it just before the call under test. */
static void arm(void) {
    odin3_util_set_alloc_fail_after(armed);
}

/* A fresh AST and table: the base tree plus fill named blocks and fill symbols in top. */
static void prepare(uint32_t fill) {
    fresh_ast();
    oom_tree = base_tree();
    for (uint32_t i = 0; i < fill; i++) {
        named(ODIN3_SCOPE_BLOCK, oom_tree.top, FILL_NAME + i);
        sym(oom_tree.top, ODIN3_SYM_NET, FILL_NAME + i);
    }
}

/* The first fill symbol has no dims and no value (the list/value ops change only it). */
static void assert_fill_untouched(void) {
    odin3_sym_id first = odin3_symtab_lookup_local(st, oom_tree.top, FILL_NAME);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_symbol_unpacked(st, first).n);
    const odin3_symbol *rec = odin3_symbol_get(st, first);
    TEST_ASSERT_EQUAL_INT(0, rec != NULL && rec->has_value);
}

typedef odin3_status (*oom_op)(void);

static odin3_status op_scope(void) {
    odin3_scope_id id = {0};
    odin3_scope_spec spec = {.kind = ODIN3_SCOPE_GENERATE,
                             .name = FILL_NAME - 1,
                             .index = 1,
                             .has_index = true,
                             .parent = oom_tree.top};
    arm();
    return odin3_scope_new(st, &spec, &id);
}

static odin3_status op_symbol(void) {
    odin3_sym_id id = {0};
    odin3_symbol_spec spec = {.name = FILL_NAME - 1, .scope = oom_tree.top, .kind = ODIN3_SYM_VAR};
    arm();
    return odin3_symbol_new(st, &spec, &id);
}

static odin3_status op_set_name(void) {
    odin3_scope_id id = {0};
    odin3_scope_spec spec = {.kind = ODIN3_SCOPE_BLOCK, .parent = oom_tree.top};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(st, &spec, &id));
    arm();
    return odin3_scope_set_name(st, id, FILL_NAME - 1);
}

/*
 * Runs op on a fresh table of fill entries with the k-th allocation failing, for k = 0, 1, …
 * until it succeeds; each failure must be NO_MEMORY with the counts unchanged and check clean.
 * Returns the number of failures.
 */
static long oom_sweep(oom_op op, uint32_t fill) {
    for (long k = 0; k < OOM_LIMIT; k++) {
        prepare(fill);
        uint32_t scopes = odin3_symtab_scope_count(st);
        uint32_t syms = odin3_symtab_symbol_count(st);
        armed = k;
        odin3_status status = op();
        odin3_util_set_alloc_fail_after(-1);
        if (status == ODIN3_OK) {
            return k;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, status);
        TEST_ASSERT_EQUAL_UINT32(syms, odin3_symtab_symbol_count(st));
        TEST_ASSERT_EQUAL_UINT32(scopes + (op == op_set_name), odin3_symtab_scope_count(st));
        TEST_ASSERT_EQUAL_UINT32(0, child(oom_tree.top, FILL_NAME - 1, 1).v);
        TEST_ASSERT_EQUAL_UINT32(0, child(oom_tree.top, FILL_NAME - 1, ODIN3_SCOPE_NO_INDEX).v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_symtab_lookup_local(st, oom_tree.top, FILL_NAME - 1).v);
        assert_fill_untouched();
        assert_check_ok();
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT");
    return -1;
}

/* Sums the failures over fills 0..MAP_FILLS: some fill puts the next insert at a map growth. */
static long oom_map_sweep(oom_op op) {
    long failures = 0;
    for (uint32_t fill = 0; fill < MAP_FILLS; fill++) {
        failures += oom_sweep(op, fill);
    }
    return failures;
}

static void test_oom_scope_and_symbol(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_map_sweep(op_scope));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_map_sweep(op_symbol));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_map_sweep(op_set_name));
    /* the next scope and symbol need a new page (slot 0 + 3 base scopes + fill) */
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_sweep(op_scope, PAGE - 4));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_sweep(op_symbol, PAGE - 1));
}

static odin3_status op_assign(void) {
    odin3_sym_id first = odin3_symtab_lookup_local(st, oom_tree.top, FILL_NAME);
    odin3_status status = ODIN3_OK;
    arm();
    for (uint32_t i = 0; i < NODES + 1 && status == ODIN3_OK; i++) {
        status = odin3_symbol_add_assign(st, first, nodes[1 + i % NODES], ODIN3_ASSIGN_BLOCKING);
    }
    return status;
}

static odin3_status op_unpacked(void) {
    odin3_sym_id first = odin3_symtab_lookup_local(st, oom_tree.top, FILL_NAME);
    const odin3_dim dims[] = {{0, 1}, {2, 3}};
    arm();
    return odin3_symbol_set_unpacked(st, first, dims, 2);
}

static odin3_status op_value(void) {
    static uint8_t big[BIG_BITS]; /* larger than an arena chunk: its own allocation */
    odin3_sym_id first = odin3_symtab_lookup_local(st, oom_tree.top, FILL_NAME);
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = big, .len = BIG_BITS};
    arm();
    return odin3_symbol_set_value(st, first, &val, true);
}

static void test_oom_lists_and_values(void) {
    /* the first assignment allocates the table; the ninth grows it */
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(2, oom_sweep(op_assign, 1));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_sweep(op_unpacked, 1));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(1, oom_sweep(op_value, 1));
}

/* One symtab_create on a fresh store with allocation fail_at failing. */
static odin3_status try_create(long fail_at) {
    fresh_store();
    odin3_symtab *made = NULL;
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status status = odin3_symtab_create(ast, &made);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_PTR(made, odin3_ast_symtab(ast)); /* NULL on failure */
    return status;
}

static void test_oom_create(void) {
    long k = 0;
    odin3_status status = ODIN3_ERR_NO_MEMORY;
    for (; k < OOM_LIMIT && status != ODIN3_OK; k++) {
        status = try_create(k);
        TEST_ASSERT_EQUAL_INT(status == ODIN3_OK ? ODIN3_OK : ODIN3_ERR_NO_MEMORY, status);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, status);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(6, k); /* struct, two page vecs, two maps, arena */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_once);
    RUN_TEST(test_library_scopes);
    RUN_TEST(test_scope_shape_misuse);
    RUN_TEST(test_generate_indices);
    RUN_TEST(test_set_name);
    RUN_TEST(test_lookup_outward_and_shadowing);
    RUN_TEST(test_symbol_misuse);
    RUN_TEST(test_symbol_fields);
    RUN_TEST(test_assign_lists);
    RUN_TEST(test_set_value_copies);
    RUN_TEST(test_remap);
    RUN_TEST(test_check);
    RUN_TEST(test_oom_scope_and_symbol);
    RUN_TEST(test_oom_lists_and_values);
    RUN_TEST(test_oom_create);
    return UNITY_END();
}
