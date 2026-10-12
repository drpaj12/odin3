/*
 * test_elab_bind.c — 2D Task 13: the binder (spec §2.5, EL-5) and the EL-4 store-shape rows that
 * 2A's check rule 7 does not cover (§2.4), on hand-built elaborated stores (elab_test_tree.h's eb_*
 * builders, the slang seam): the scopes and symbols of a module with ports, nets, a variable, an
 * unsized localparam, a generate loop, a named block, a function and a task; outward and
 * hierarchical resolution; the assignment lists; every binder error and EL-4 violation as its whole
 * line; black boxes; a 10,000-deep generate nesting; misuse; the error cap; out of memory.
 */
#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "elab_test_tree.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    MAX_LINES = 64,
    LINE_BYTES = 512,
    NEST = 10000,      /* nested GEN_BLOCKs (the AST height cap is 32768) */
    OOM_LIMIT = 20000, /* allocation points swept at most */
    OOM_SETTLE = 64,   /* clean runs in a row that end the sweep */
    OOM_MIN_FAILURES = 20,
    LOW_CAP = 3, /* a lowered error cap */
    ALL_ONES = 32,
    WIDE = 40000, /* generate-loop iterations of the timing test */
};

static const double WIDE_BUDGET_S = 1.0; /* the wide loop's CPU budget in Release */

static odin3_design *design;
static et_tree tree;
static odin3_elab_binding bnd;
static char lines[MAX_LINES][LINE_BYTES];
static odin3_log_level levels[MAX_LINES];
static uint32_t n_lines;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines >= MAX_LINES) {
        return;
    }
    size_t len = strlen(msg);
    len = len < LINE_BYTES - 1 ? len : LINE_BYTES - 1;
    memcpy(lines[n_lines], msg, len);
    lines[n_lines][len] = '\0';
    levels[n_lines] = level;
    n_lines++;
}

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    eb_init(&tree, design, "b.v");
    bnd = (odin3_elab_binding){0};
    n_lines = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_elab_binding_free(&bnd);
    et_free(&tree);
    odin3_design_destroy(design);
}

/* --- the log --------------------------------------------------------------------------------- */

static const char *all_lines(void) {
    static char buf[MAX_LINES * LINE_BYTES];
    size_t at = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < n_lines; i++) {
        size_t len = strlen(lines[i]);
        if (at + len + 2 > sizeof buf) {
            break;
        }
        memcpy(buf + at, lines[i], len);
        buf[at + len] = '\n';
        at += len + 1;
        buf[at] = '\0';
    }
    return buf;
}

static uint32_t error_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        count += levels[i] == ODIN3_LOG_ERROR ? 1 : 0;
    }
    return count;
}

/* The error line was logged. */
static void expect_line(const char *line) {
    for (uint32_t i = 0; i < n_lines; i++) {
        if (levels[i] == ODIN3_LOG_ERROR && strcmp(lines[i], line) == 0) {
            return;
        }
    }
    TEST_FAIL_MESSAGE(all_lines());
}

/* --- building ------------------------------------------------------------------------------ */

static odin3_ast_id ident(const char *name) {
    return et_ident(&tree, name);
}

static odin3_ast_id no_range(void) {
    return (odin3_ast_id){0};
}

/* [msb:0] of 32-bit NUMBERs. */
static odin3_ast_id vec(int32_t msb) {
    return eb_range(&tree, (eb_bounds){eb_int(&tree, msb), eb_int(&tree, 0)});
}

static odin3_ast_id wire(const char *name, odin3_ast_id range) {
    eb_decl decl = {.sub = ODIN3_AST_NET_DECL_WIRE, .range = range, .name = name};
    return eb_net(&tree, &decl);
}

static odin3_ast_id reg(const char *name, odin3_ast_id range) {
    eb_decl decl = {.sub = ODIN3_AST_VAR_DECL_REG, .range = range, .name = name};
    return eb_var(&tree, &decl);
}

/* A wire port of direction dir. */
static odin3_ast_id port(const char *name, uint8_t dir, odin3_ast_id range) {
    eb_decl decl = {.sub = dir, .flags = eb_wire_port(), .range = range, .name = name};
    return eb_port(&tree, &decl);
}

static odin3_ast_id cont(odin3_ast_id lhs, odin3_ast_id rhs) {
    return eb_assign(&tree, ODIN3_AST_CONT_ASSIGN, lhs, rhs);
}

static odin3_ast_id blocking(odin3_ast_id lhs, odin3_ast_id rhs) {
    return eb_assign(&tree, ODIN3_AST_BLOCKING_ASSIGN, lhs, rhs);
}

/* The NET_ASSIGN of a CONT_ASSIGN. */
static odin3_ast_id net_assign_of(odin3_ast_id cont_assign) {
    return odin3_ast_child(tree.ast, cont_assign, 2);
}

static odin3_ast_id null_stmt(void) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_NULL_STMT};
    return et_node(&tree, spec, NULL, 0);
}

/* MODULE name (ports LIST or 0) with items. */
static odin3_ast_id module_of(const char *name, odin3_ast_id ports, const odin3_ast_id *items,
                              uint32_t n) {
    eb_mod mod = {name, {0}, ports, items, n};
    return eb_module(&tree, &mod);
}

/* A store of one module t (no ports) with items. */
static odin3_ast_id top_of(const odin3_ast_id *items, uint32_t n) {
    odin3_ast_id module = module_of("t", (odin3_ast_id){0}, items, n);
    (void)eb_unit(&tree, &module, 1);
    return module;
}

static odin3_status bind(void) {
    return odin3_elab_bind(tree.ast, &bnd);
}

/* --- reading the binding ------------------------------------------------------------------- */

static odin3_symtab *symtab(void) {
    odin3_symtab *st = odin3_ast_symtab(tree.ast);
    TEST_ASSERT_NOT_NULL(st);
    return st;
}

static uint32_t name_id(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &id), name);
    return id;
}

/* The symbol named name in scope itself (it must exist). */
static const odin3_symbol *local(odin3_scope_id scope, const char *name) {
    odin3_sym_id sym = odin3_symtab_lookup_local(symtab(), scope, name_id(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, sym.v, name);
    return odin3_symbol_get(symtab(), sym);
}

static odin3_sym_id local_id(odin3_scope_id scope, const char *name) {
    return odin3_symtab_lookup_local(symtab(), scope, name_id(name));
}

static const odin3_symbol *resolved(odin3_ast_id node) {
    odin3_sym_id sym = odin3_elab_sym_of(&bnd, node);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, sym.v,
                                         odin3_ast_kind_name(odin3_ast_kind_of(tree.ast, node)));
    return odin3_symbol_get(symtab(), sym);
}

/* The scope has kind, name and (has_index) index. */
typedef struct scope_want {
    uint8_t kind;
    const char *name;
    bool has_index;
    int32_t index;
} scope_want;

static void expect_scope(odin3_scope_id scope, scope_want want) {
    const odin3_scope *rec = odin3_scope_get(symtab(), scope);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT8(want.kind, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(name_id(want.name), rec->name);
    TEST_ASSERT_EQUAL(want.has_index, rec->has_index);
    TEST_ASSERT_EQUAL_INT32(want.index, rec->index);
}

/* The bind failed with status after exactly the error line; nothing is bound. */
static void expect_failure(odin3_status status, const char *line) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(status, bind(), all_lines());
    expect_line(line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, error_count(), all_lines());
    TEST_ASSERT_NULL(bnd.sym_of);
    TEST_ASSERT_NULL(bnd.scope_of);
}

/* --- the fixture: module t of the brief ---------------------------------------------------- */

/* The nodes the tests read. */
typedef struct fixture {
    odin3_ast_id module;
    odin3_ast_id w_in_g0;   /* the IDENT w read inside g[0] */
    odin3_ast_id late_use;  /* the IDENT late, used before its declaration */
    odin3_ast_id hier_g1;   /* g[1].tt */
    odin3_ast_id hier_blk;  /* blk.count */
    odin3_ast_id assign_w;  /* assign w = a: its NET_ASSIGN */
    odin3_ast_id nb_r;      /* r <= P */
    odin3_ast_id in_f;      /* r = x inside f */
    odin3_ast_id call_f;    /* f(a[0]) */
    odin3_ast_id call_k;    /* k; in blk */
    odin3_ast_id h1;        /* wire h1 = g[1].tt: its DECLARATOR */
    odin3_ast_id up_g0;     /* blk.count read inside g[0] (found outward) */
    odin3_ast_id scopes[4]; /* the GEN_BLOCK g[0], the SEQ_BLOCK blk, f's and k's decls */
} fixture;

/* g[0]: begin wire tt; assign tt = w[0]; wire up = blk.count; end and
 * g[1]: begin wire tt; end. */
static void build_loop(fixture *fx, odin3_ast_id *items) {
    et_line(&tree, 7);
    fx->w_in_g0 = ident("w");
    odin3_ast_id sel =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, fx->w_in_g0, eb_int(&tree, 0), {0}});
    const odin3_ast_id blk_count[] = {ident("blk"), ident("count")};
    fx->up_g0 = eb_hier(&tree, blk_count, 2);
    eb_decl up = {.sub = ODIN3_AST_NET_DECL_WIRE, .name = "up", .value = fx->up_g0};
    const odin3_ast_id g0_items[] = {wire("tt", (odin3_ast_id){0}), cont(ident("tt"), sel),
                                     eb_net(&tree, &up)};
    eb_block g0 = {"g", eb_int(&tree, 0), g0_items, 3};
    items[0] = eb_gen_block(&tree, &g0);
    fx->scopes[0] = items[0];
    et_line(&tree, 8);
    const odin3_ast_id g1_items[] = {wire("tt", (odin3_ast_id){0})};
    eb_block g1 = {"g", eb_int(&tree, 1), g1_items, 1};
    items[1] = eb_gen_block(&tree, &g1);
}

/* initial begin : blk reg count; count = 0; k; end, function f, task k. */
static void build_subroutines(fixture *fx, odin3_ast_id *items) {
    et_line(&tree, 9);
    const odin3_ast_id locals[] = {reg("count", (odin3_ast_id){0})};
    fx->call_k = eb_task_call(&tree, "k", NULL, 0);
    const odin3_ast_id stmts[] = {blocking(ident("count"), eb_int(&tree, 0)), fx->call_k};
    eb_block blk = {"blk", eb_list(&tree, locals, 1), stmts, 2};
    fx->scopes[1] = eb_named_block(&tree, &blk);
    items[0] = eb_initial(&tree, fx->scopes[1]);
    et_line(&tree, 10);
    eb_decl input = {.sub = ODIN3_AST_PORT_DECL_INPUT, .name = "x"};
    const odin3_ast_id ports[] = {eb_port(&tree, &input)};
    fx->in_f = blocking(ident("r"), ident("x"));
    const odin3_ast_id body[] = {blocking(ident("f"), ident("x")), fx->in_f};
    eb_subr fn = {.name = "f", .ports = eb_list(&tree, ports, 1), .body = eb_begin(&tree, body, 2)};
    items[1] = eb_function(&tree, &fn);
    et_line(&tree, 11);
    eb_subr task = {.name = "k"};
    items[2] = eb_task(&tree, &task);
    fx->scopes[2] = items[1];
    fx->scopes[3] = items[2];
}

/* assign w = a; always @(posedge a[0]) r <= P; wire h1 = g[1].tt; wire h2 = blk.count;
 * wire c = f(a[0]). */
static void build_uses(fixture *fx, odin3_ast_id *items) {
    et_line(&tree, 12);
    items[0] = cont(ident("w"), ident("a"));
    fx->assign_w = net_assign_of(items[0]);
    et_line(&tree, 13);
    odin3_ast_id clock =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, ident("a"), eb_int(&tree, 0), {0}});
    const odin3_ast_id events[] = {eb_event(&tree, ODIN3_AST_EVENT_EXPR_POSEDGE, clock)};
    fx->nb_r = eb_assign(&tree, ODIN3_AST_NONBLOCKING_ASSIGN, ident("r"), ident("P"));
    items[1] = eb_always(&tree, events, 1, fx->nb_r);
    et_line(&tree, 14);
    const odin3_ast_id g1tt[] = {eb_index(&tree, "g", 1), ident("tt")};
    fx->hier_g1 = eb_hier(&tree, g1tt, 2);
    eb_decl h1 = {.sub = ODIN3_AST_NET_DECL_WIRE, .name = "h1", .value = fx->hier_g1};
    items[2] = eb_net(&tree, &h1);
    fx->h1 = odin3_ast_child(tree.ast, items[2], 3);
    et_line(&tree, 15);
    const odin3_ast_id blk_count[] = {ident("blk"), ident("count")};
    fx->hier_blk = eb_hier(&tree, blk_count, 2);
    eb_decl h2 = {.sub = ODIN3_AST_NET_DECL_WIRE, .name = "h2", .value = fx->hier_blk};
    items[3] = eb_net(&tree, &h2);
    et_line(&tree, 16);
    const odin3_ast_id arg[] = {
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_BIT, ident("a"), eb_int(&tree, 0), {0}})};
    fx->call_f = et_call(&tree, "f", arg, 1);
    eb_decl c_decl = {.sub = ODIN3_AST_NET_DECL_WIRE, .name = "c", .value = fx->call_f};
    items[4] = eb_net(&tree, &c_decl);
}

/* module t (input [3:0] a, output y) of the brief, with its declarations and uses. */
static void build_main(fixture *fx) {
    odin3_ast_id items[16];
    const odin3_ast_id ports[] = {port("a", ODIN3_AST_PORT_DECL_INPUT, vec(3)),
                                  port("y", ODIN3_AST_PORT_DECL_OUTPUT, (odin3_ast_id){0})};
    et_line(&tree, 2);
    items[0] = wire("w", vec(3));
    et_line(&tree, 3);
    eb_decl reg_r = {.sub = ODIN3_AST_VAR_DECL_REG,
                     .flags = ODIN3_AST_F_VAR_DECL_SIGNED,
                     .range = vec(7),
                     .name = "r"};
    items[1] = eb_var(&tree, &reg_r);
    et_line(&tree, 4);
    items[2] = eb_localparam(&tree, "P", et_num(&tree, "'hFFFFFFFF"));
    et_line(&tree, 5);
    fx->late_use = ident("late");
    items[3] = cont(ident("y"), fx->late_use);
    et_line(&tree, 6);
    items[4] = wire("late", (odin3_ast_id){0});
    build_loop(fx, items + 5);
    build_subroutines(fx, items + 7);
    build_uses(fx, items + 10);
    fx->module = module_of("t", eb_list(&tree, ports, 2), items, 15);
    (void)eb_unit(&tree, &fx->module, 1);
}

static odin3_scope_id bind_main(fixture *fx) {
    build_main(fx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    TEST_ASSERT_EQUAL_UINT32(0, error_count());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL)); /* rule 8 */
    odin3_scope_id scope = odin3_elab_scope_of(&bnd, fx->module);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, scope.v);
    return scope;
}

/* --- symbols, scopes, resolution, assignments ---------------------------------------------- */

/* `wire [3:0] w` is a NET with packed {3,0}; `reg signed [7:0] r` a VAR with SIGNED. */
static void test_symbols_objects(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    const odin3_symbol *wire_w = local(mod, "w");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_NET, wire_w->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_NET_DECL_WIRE, wire_w->sub);
    TEST_ASSERT_TRUE(wire_w->has_packed);
    TEST_ASSERT_EQUAL_INT32(3, wire_w->packed.msb);
    TEST_ASSERT_EQUAL_INT32(0, wire_w->packed.lsb);
    const odin3_symbol *reg_r = local(mod, "r");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_VAR, reg_r->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_VAR_DECL_REG, reg_r->sub);
    TEST_ASSERT_TRUE((reg_r->flags & ODIN3_SYM_F_SIGNED) != 0);
    TEST_ASSERT_EQUAL_INT32(7, reg_r->packed.msb);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_DECLARATOR, odin3_ast_kind_of(tree.ast, wire_w->declarator));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NET_DECL, odin3_ast_kind_of(tree.ast, wire_w->decl));
}

/* `localparam P` (an unsized NUMBER of 32 ones) is a LOCALPARAM with the 32-bit all-ones value;
 * its declarator's NUMBER keeps payload width 0. */
static void test_symbols_localparam(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    const odin3_symbol *param = local(mod, "P");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_LOCALPARAM, param->kind);
    TEST_ASSERT_TRUE(param->has_value);
    TEST_ASSERT_FALSE(param->value_signed);
    TEST_ASSERT_EQUAL_UINT32(ALL_ONES, param->value.len);
    uint32_t ones = 0;
    for (uint32_t i = 0; i < param->value.len; i++) {
        ones += param->value.bits[i] == ODIN3_BIT_1 ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(ALL_ONES, ones);
    odin3_ast_id value = odin3_ast_child(tree.ast, param->declarator, 0);
    const odin3_ast_number *num = odin3_ast_number_of(tree.ast, value);
    TEST_ASSERT_NOT_NULL(num);
    TEST_ASSERT_EQUAL_UINT32(0, num->width); /* unsized: stage 2 extends it by its top bit */
}

/* Ports (NET, PORT, the direction), blocks, the function and the task. */
static void test_symbols_ports_and_scopes(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    const odin3_symbol *in_a = local(mod, "a");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_NET, in_a->kind);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_PORT, in_a->flags);
    TEST_ASSERT_EQUAL_UINT8(1 + ODIN3_AST_PORT_DECL_INPUT, in_a->dir);
    TEST_ASSERT_EQUAL_INT32(3, in_a->packed.msb);
    TEST_ASSERT_EQUAL_UINT8(1 + ODIN3_AST_PORT_DECL_OUTPUT, local(mod, "y")->dir);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_BLOCK, local(mod, "g")->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_BLOCK, local(mod, "blk")->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_FUNCTION, local(mod, "f")->kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_TASK, local(mod, "k")->kind);
}

/* DESIGN -> LIBRARY work -> MODULE t (with t's MODULE symbol in the LIBRARY). */
static void test_scopes_top(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    odin3_symtab *st = symtab();
    odin3_scope_id top = {1};
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SCOPE_DESIGN, odin3_scope_get(st, top)->kind);
    odin3_scope_id lib = odin3_scope_first_child(st, top);
    expect_scope(lib, (scope_want){ODIN3_SCOPE_LIBRARY, "work", false, 0});
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_next_sibling(st, lib).v);
    TEST_ASSERT_EQUAL_UINT32(mod.v, odin3_scope_first_child(st, lib).v);
    expect_scope(mod, (scope_want){ODIN3_SCOPE_MODULE, "t", false, 0});
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_MODULE, local(lib, "t")->kind);
    TEST_ASSERT_EQUAL_UINT32(local_id(lib, "t").v, odin3_elab_sym_of(&bnd, fx.module).v);
}

/* MODULE t -> GENERATE g (0), GENERATE g (1, on the same-name chain), BLOCK blk, FUNCTION f,
 * TASK k, in that order. */
static void test_scopes_module(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    odin3_symtab *st = symtab();
    odin3_scope_id g0 = odin3_scope_first_child(st, mod);
    expect_scope(g0, (scope_want){ODIN3_SCOPE_GENERATE, "g", true, 0});
    odin3_scope_id g1 = odin3_scope_next_sibling(st, g0);
    expect_scope(g1, (scope_want){ODIN3_SCOPE_GENERATE, "g", true, 1});
    TEST_ASSERT_EQUAL_UINT32(g1.v, odin3_scope_get(st, g0)->same_name_next.v);
    TEST_ASSERT_EQUAL_UINT32(g0.v, odin3_scope_child(st, mod, name_id("g"), 0).v);
    TEST_ASSERT_EQUAL_UINT32(g1.v, odin3_scope_child(st, mod, name_id("g"), 1).v);
    odin3_scope_id blk = odin3_scope_next_sibling(st, g1);
    expect_scope(blk, (scope_want){ODIN3_SCOPE_BLOCK, "blk", false, 0});
    odin3_scope_id fn = odin3_scope_next_sibling(st, blk);
    expect_scope(fn, (scope_want){ODIN3_SCOPE_FUNCTION, "f", false, 0});
    odin3_scope_id task = odin3_scope_next_sibling(st, fn);
    expect_scope(task, (scope_want){ODIN3_SCOPE_TASK, "k", false, 0});
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_next_sibling(st, task).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, local_id(g0, "tt").v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, local_id(blk, "count").v);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_VAR, local(fn, "x")->kind); /* a function port */
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_PORT, local(fn, "x")->flags);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_VAR, local(fn, "f")->kind); /* its result */
}

static void test_resolution(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    odin3_symtab *st = symtab();
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "w").v, odin3_elab_sym_of(&bnd, fx.w_in_g0).v);
    odin3_scope_id g1 = odin3_scope_child(st, mod, name_id("g"), 1);
    TEST_ASSERT_EQUAL_UINT32(local_id(g1, "tt").v, odin3_elab_sym_of(&bnd, fx.hier_g1).v);
    odin3_scope_id g0 = odin3_scope_child(st, mod, name_id("g"), 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(local_id(g0, "tt").v, odin3_elab_sym_of(&bnd, fx.hier_g1).v);
    odin3_scope_id blk = odin3_scope_child(st, mod, name_id("blk"), ODIN3_SCOPE_NO_INDEX);
    TEST_ASSERT_EQUAL_UINT32(local_id(blk, "count").v, odin3_elab_sym_of(&bnd, fx.hier_blk).v);
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "late").v, odin3_elab_sym_of(&bnd, fx.late_use).v);
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "f").v, odin3_elab_sym_of(&bnd, fx.call_f).v);
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "k").v, odin3_elab_sym_of(&bnd, fx.call_k).v);
    odin3_scope_id fn = odin3_scope_child(st, mod, name_id("f"), ODIN3_SCOPE_NO_INDEX);
    odin3_ast_id x_in_f = odin3_ast_child(tree.ast, fx.in_f, 2);
    TEST_ASSERT_EQUAL_UINT32(local_id(fn, "x").v, odin3_elab_sym_of(&bnd, x_in_f).v);
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "h1").v, odin3_elab_sym_of(&bnd, fx.h1).v);
    TEST_ASSERT_EQUAL_UINT32(local_id(blk, "count").v, odin3_elab_sym_of(&bnd, fx.up_g0).v);
}

/* scope_of for each scope-opening kind (MODULE in bind_main). */
static void test_scope_of(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    odin3_symtab *st = symtab();
    TEST_ASSERT_EQUAL_UINT32(odin3_scope_child(st, mod, name_id("g"), 0).v,
                             odin3_elab_scope_of(&bnd, fx.scopes[0]).v);
    static const char *const names[] = {"blk", "f", "k"};
    for (uint32_t i = 0; i < 3; i++) {
        odin3_scope_id want = odin3_scope_child(st, mod, name_id(names[i]), ODIN3_SCOPE_NO_INDEX);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, want.v);
        TEST_ASSERT_EQUAL_UINT32(want.v, odin3_elab_scope_of(&bnd, fx.scopes[1 + i]).v);
        TEST_ASSERT_EQUAL_UINT32(local_id(mod, names[i]).v,
                                 odin3_elab_sym_of(&bnd, fx.scopes[1 + i]).v);
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_scope_of(&bnd, fx.w_in_g0).v);
}

/* The symbol's assignments: count of them, the first by node of kind. */
typedef struct assigns_want {
    uint32_t count;
    odin3_ast_id node;
    odin3_assign_kind kind;
} assigns_want;

static void expect_assigns(const odin3_symbol *sym, assigns_want want) {
    uint32_t count = 0;
    for (odin3_assign_id at = sym->first_assign; at.v != 0;
         at = odin3_symbol_next_assign(symtab(), at)) {
        count++;
    }
    TEST_ASSERT_EQUAL_UINT32(want.count, count);
    const odin3_sym_assign *first = odin3_assign_get(symtab(), sym->first_assign);
    TEST_ASSERT_EQUAL_UINT32(want.node.v, first != NULL ? first->node.v : 0);
    TEST_ASSERT_EQUAL_UINT8(want.count > 0 ? want.kind : 0, first != NULL ? first->kind : 0);
}

static void test_assignments(void) {
    fixture fx;
    odin3_scope_id mod = bind_main(&fx);
    const odin3_symbol *wire_w = local(mod, "w");
    expect_assigns(wire_w, (assigns_want){1, fx.assign_w, ODIN3_ASSIGN_CONTINUOUS});
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_ASSIGNED_CONTINUOUS, wire_w->flags);
    const odin3_symbol *reg_r = local(mod, "r"); /* not r = x inside f */
    expect_assigns(reg_r, (assigns_want){1, fx.nb_r, ODIN3_ASSIGN_NONBLOCKING});
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_SIGNED | ODIN3_SYM_F_ASSIGNED_NONBLOCKING, reg_r->flags);
    /* a net declaration assignment */
    expect_assigns(local(mod, "h1"), (assigns_want){1, fx.h1, ODIN3_ASSIGN_CONTINUOUS});
    odin3_scope_id blk = odin3_scope_child(symtab(), mod, name_id("blk"), ODIN3_SCOPE_NO_INDEX);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_ASSIGNED_BLOCKING, local(blk, "count")->flags);
    odin3_scope_id fn = odin3_scope_child(symtab(), mod, name_id("f"), ODIN3_SCOPE_NO_INDEX);
    expect_assigns(local(fn, "f"), (assigns_want){0, {0}, ODIN3_ASSIGN_BLOCKING});
    expect_assigns(local(mod, "a"), (assigns_want){0, {0}, ODIN3_ASSIGN_BLOCKING});
}

/* {p1, p2} = …: both assigned; {q, q} = …: one record. */
static void test_concat_lvalues(void) {
    const odin3_ast_id pair[] = {ident("p1"), ident("p2")};
    odin3_ast_id cont_pair = cont(et_concat(&tree, pair, 2), eb_int(&tree, 0));
    const odin3_ast_id twice[] = {ident("q"), ident("q")};
    odin3_ast_id same = blocking(et_concat(&tree, twice, 2), eb_int(&tree, 0));
    const odin3_ast_id items[] = {wire("p1", no_range()), wire("p2", no_range()),
                                  reg("q", no_range()), cont_pair, eb_always(&tree, NULL, 0, same)};
    odin3_ast_id module = top_of(items, 5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    odin3_scope_id mod = odin3_elab_scope_of(&bnd, module);
    odin3_ast_id net_assign = net_assign_of(cont_pair);
    expect_assigns(local(mod, "p1"), (assigns_want){1, net_assign, ODIN3_ASSIGN_CONTINUOUS});
    expect_assigns(local(mod, "p2"), (assigns_want){1, net_assign, ODIN3_ASSIGN_CONTINUOUS});
    expect_assigns(local(mod, "q"), (assigns_want){1, same, ODIN3_ASSIGN_BLOCKING});
}

/* `output reg q`, `integer i`, `reg [7:0] mem [0:3]`, and `function signed [3:0] f` whose body
 * is `f = f(x)`: the result VAR for f, the FUNCTION for the call. */
typedef struct forms {
    odin3_ast_id module;
    odin3_ast_id result; /* the IDENT f assigned in f */
    odin3_ast_id call;   /* f(x) in f */
} forms;

static void build_forms(forms *out) {
    eb_decl out_q = {.sub = ODIN3_AST_PORT_DECL_OUTPUT,
                     .flags = (uint16_t)(ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT),
                     .name = "q"};
    const odin3_ast_id ports[] = {eb_port(&tree, &out_q)};
    eb_decl integer = {.sub = ODIN3_AST_VAR_DECL_INTEGER, .name = "i"};
    const odin3_ast_id dims[] = {eb_range(&tree, (eb_bounds){eb_int(&tree, 0), eb_int(&tree, 3)})};
    eb_decl mem = {
        .sub = ODIN3_AST_VAR_DECL_REG, .range = vec(7), .name = "mem", .dims = dims, .n_dims = 1};
    const odin3_ast_id args[] = {ident("x")};
    out->call = et_call(&tree, "f", args, 1);
    out->result = ident("f");
    eb_decl input = {.sub = ODIN3_AST_PORT_DECL_INPUT, .name = "x"};
    const odin3_ast_id fn_ports[] = {eb_port(&tree, &input)};
    eb_subr fn = {.name = "f",
                  .flags = ODIN3_AST_F_FUNCTION_DECL_SIGNED,
                  .range = vec(3),
                  .ports = eb_list(&tree, fn_ports, 1),
                  .body = blocking(out->result, out->call)};
    const odin3_ast_id items[] = {eb_var(&tree, &integer), eb_var(&tree, &mem),
                                  eb_function(&tree, &fn),
                                  eb_localparam(&tree, "N", eb_int(&tree, -2)),
                                  eb_localparam(&tree, "S", et_string(&tree, "ab"))};
    out->module = module_of("t", eb_list(&tree, ports, 1), items, 5);
    (void)eb_unit(&tree, &out->module, 1);
}

static void test_symbol_forms(void) {
    forms fx;
    build_forms(&fx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    odin3_scope_id mod = odin3_elab_scope_of(&bnd, fx.module);
    const odin3_symbol *out_q = local(mod, "q");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_VAR, out_q->kind);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_PORT, out_q->flags);
    const odin3_symbol *integer = local(mod, "i");
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_VAR_DECL_INTEGER, integer->sub);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_SIGNED, integer->flags);
    TEST_ASSERT_EQUAL_INT32(31, integer->packed.msb);
    odin3_dim_span span = odin3_symbol_unpacked(symtab(), local_id(mod, "mem"));
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_INT32(0, span.dims[0].msb);
    TEST_ASSERT_EQUAL_INT32(3, span.dims[0].lsb);
    odin3_scope_id fn = odin3_scope_child(symtab(), mod, name_id("f"), ODIN3_SCOPE_NO_INDEX);
    const odin3_symbol *result = resolved(fx.result);
    TEST_ASSERT_EQUAL_UINT32(local_id(fn, "f").v, odin3_elab_sym_of(&bnd, fx.result).v);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_SIGNED, result->flags);
    TEST_ASSERT_EQUAL_INT32(3, result->packed.msb);
    const odin3_symbol *neg = local(mod, "N");
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_SIGNED, neg->flags);
    TEST_ASSERT_TRUE(neg->value_signed);
    TEST_ASSERT_EQUAL_UINT32(32, neg->value.len);
    const odin3_symbol *text = local(mod, "S"); /* "ab": 16 bits, 'a' the high byte */
    TEST_ASSERT_EQUAL_UINT32(16, text->value.len);
    TEST_ASSERT_FALSE(text->value_signed);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, text->value.bits[14]); /* 'a' = 0x61 */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, text->value.bits[15]);
    /* the call is the function, not its result */
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "f").v, odin3_elab_sym_of(&bnd, fx.call).v);
}

/* A black box's scope holds its ports only; its INSTANTIATION resolves to its MODULE symbol; a
 * cell type's to nothing. */
static void test_blackbox(void) {
    const odin3_ast_id params[] = {eb_localparam(&tree, "W", eb_int(&tree, 3))};
    const odin3_ast_id bb_ports[] = {port("a", ODIN3_AST_PORT_DECL_INPUT, vec(2))};
    const odin3_ast_id body[] = {wire("inner", no_range())}; /* none in the EL-4 form: unbound */
    eb_mod bb_mod = {"bb", eb_list(&tree, params, 1), eb_list(&tree, bb_ports, 1), body, 1};
    odin3_ast_id box = eb_module(&tree, &bb_mod);
    eb_attr(&tree, box, "blackbox");
    const odin3_ast_id given[] = {eb_conn(&tree, "W", eb_int(&tree, 3))};
    const odin3_ast_id conns[] = {eb_conn(&tree, "a", ident("x"))};
    eb_inst inst_u = {"bb", "u", eb_list(&tree, given, 1), conns, 1};
    eb_inst inst_v = {"cell", "v", {0}, NULL, 0};
    const odin3_ast_id items[] = {wire("x", vec(2)), eb_instance(&tree, &inst_u),
                                  eb_instance(&tree, &inst_v)};
    const odin3_ast_id modules[] = {module_of("t", (odin3_ast_id){0}, items, 3), box};
    (void)eb_unit(&tree, modules, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    odin3_scope_id box_scope = odin3_elab_scope_of(&bnd, box);
    odin3_sym_id only = odin3_scope_first_symbol(symtab(), box_scope);
    TEST_ASSERT_EQUAL_UINT32(local_id(box_scope, "a").v, only.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_scope_next_symbol(symtab(), only).v); /* nor W, nor inner */
    const odin3_symbol *target = resolved(items[1]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_MODULE, target->kind);
    TEST_ASSERT_EQUAL_UINT32(box.v, target->decl.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_sym_of(&bnd, items[2]).v); /* a registered cell type */
    odin3_scope_id mod = odin3_elab_scope_of(&bnd, modules[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYM_INSTANCE, local(mod, "u")->kind);
}

/* --- user errors (§2.5) -------------------------------------------------------------------- */

static void test_undeclared(void) {
    et_line(&tree, 4);
    odin3_ast_id out_y = wire("y", no_range());
    et_line(&tree, 5);
    odin3_ast_id lhs = ident("y");
    et_col(&tree, 12);
    odin3_ast_id rhs = ident("x");
    const odin3_ast_id items[] = {out_y, cont(lhs, rhs)};
    (void)top_of(items, 2);
    expect_failure(ODIN3_ERR_PARSE, "b.v:5:12: error: 'x' is not declared");
}

/* wire w; (the keyword at column 3, the name at 13) then reg w; at 3:11. */
static void test_duplicate(void) {
    et_line(&tree, 2);
    et_col(&tree, 13);
    eb_decl name = {.name = "w"};
    const odin3_ast_id kids[] = {{0}, {0}, {0}, eb_declarator(&tree, &name)};
    et_col(&tree, 3);
    odin3_ast_id first =
        eb_node(&tree, (eb_shape){ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_WIRE, 0, NULL}, kids, 4);
    et_line(&tree, 3);
    et_col(&tree, 11);
    const odin3_ast_id items[] = {first, reg("w", no_range())};
    (void)top_of(items, 2);
    expect_failure(ODIN3_ERR_PARSE, "b.v:3:11: error: 'w' is already declared (first at b.v:2:13)");
}

/* Two generate blocks of one name and index, and a named block named like a wire: M11 at the
 * block. */
static void test_duplicate_blocks(void) {
    et_line(&tree, 2);
    odin3_ast_id first = wire("n", no_range());
    eb_block g0 = {"g", eb_int(&tree, 0), NULL, 0};
    eb_block again = {"g", eb_int(&tree, 0), NULL, 0};
    et_line(&tree, 3);
    odin3_ast_id gen = eb_gen_block(&tree, &g0);
    et_line(&tree, 4);
    odin3_ast_id gen2 = eb_gen_block(&tree, &again);
    et_line(&tree, 5);
    eb_block named = {"n", {0}, NULL, 0};
    const odin3_ast_id items[] = {first, gen, gen2,
                                  eb_initial(&tree, eb_named_block(&tree, &named))};
    (void)top_of(items, 4);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    expect_line("b.v:4:1: error: 'g' is already declared (first at b.v:3:1)");
    expect_line("b.v:5:1: error: 'n' is already declared (first at b.v:2:1)");
    TEST_ASSERT_EQUAL_UINT32(2, error_count());
}

/* Two MODULEs of one name, in two libraries: M11 at the second (names are unique in a UNIT, point
 * (q); stage 1 never writes this, an adapter may). */
static void test_duplicate_modules(void) {
    et_line(&tree, 2);
    odin3_ast_id first = module_of("m", no_range(), NULL, 0);
    odin3_srcfile_spec spec = {.name = eb_intern(&tree, "lib.v"),
                               .library = eb_intern(&tree, "mylib"),
                               .len = ET_LINE_BYTES};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(tree.sm, &spec, &buf));
    tree.at = odin3_srcman_loc(tree.sm, buf, 4);
    const odin3_ast_id modules[] = {module_of("t", no_range(), NULL, 0), first,
                                    module_of("m", no_range(), NULL, 0)};
    (void)eb_unit(&tree, modules, 3);
    expect_failure(ODIN3_ERR_PARSE, "lib.v:1:5: error: 'm' is already declared (first at b.v:2:1)");
}

/* A module name is no name of an expression: the MODULE symbols of a LIBRARY are not found from
 * a module's scope. */
static void test_module_name_not_value(void) {
    et_line(&tree, 4);
    const odin3_ast_id items[] = {wire("y", no_range()), cont(ident("y"), ident("t"))};
    (void)top_of(items, 2);
    expect_failure(ODIN3_ERR_PARSE, "b.v:4:1: error: 't' is not declared");
}

/* An instance, function or task name is no value (Yosys: "Failed to detect width for identifier");
 * a generate block's name binds to its BLOCK symbol (Task 18's carry: Yosys makes it an implicit
 * net). Review m1. */
static void test_not_a_value(void) {
    eb_inst inst_u = {"cell", "u", {0}, NULL, 0};
    eb_subr fn = {.name = "f"};
    eb_subr task = {.name = "k"};
    eb_block gen = {"g", {0}, NULL, 0};
    const odin3_ast_id g_parts[] = {ident("g"), ident("u2")};
    eb_block inner = {"h", {0}, NULL, 0};
    et_line(&tree, 3);
    odin3_ast_id use_u = ident("u");
    et_line(&tree, 4);
    odin3_ast_id use_f = ident("f");
    et_line(&tree, 5);
    odin3_ast_id use_k = ident("k");
    et_line(&tree, 6);
    odin3_ast_id use_hier = eb_hier(&tree, g_parts, 2);
    odin3_ast_id use_g = ident("g");
    eb_inst inst_u2 = {"cell", "u2", {0}, NULL, 0};
    const odin3_ast_id in_g[] = {eb_instance(&tree, &inst_u2), eb_gen_block(&tree, &inner)};
    gen.items = in_g;
    gen.n = 2;
    const odin3_ast_id items[] = {wire("y", no_range()),      eb_instance(&tree, &inst_u),
                                  eb_function(&tree, &fn),    eb_task(&tree, &task),
                                  eb_gen_block(&tree, &gen),  cont(ident("y"), use_u),
                                  cont(ident("y"), use_f),    cont(ident("y"), use_k),
                                  cont(ident("y"), use_hier), cont(ident("y"), use_g)};
    odin3_ast_id module = top_of(items, 10);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    expect_line("b.v:3:1: error: 'u' is not a net, variable or parameter");
    expect_line("b.v:4:1: error: 'f' is not a net, variable or parameter");
    expect_line("b.v:5:1: error: 'k' is not a net, variable or parameter");
    expect_line("b.v:6:1: error: 'g.u2' is not a net, variable or parameter");
    TEST_ASSERT_EQUAL_UINT32(4, error_count());
    (void)module;
}

/* A MODULE whose FILE buffer belongs to library mylib is in a LIBRARY scope of that name; an
 * INSTANTIATION in work still resolves to it. */
static void test_library_scopes(void) {
    odin3_srcfile_spec spec = {.name = eb_intern(&tree, "lib.v"),
                               .library = eb_intern(&tree, "mylib"),
                               .len = ET_LINE_BYTES};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(tree.sm, &spec, &buf));
    odin3_loc here = tree.at;
    tree.at = odin3_srcman_loc(tree.sm, buf, 0);
    odin3_ast_id child = module_of("m", no_range(), NULL, 0);
    tree.at = here;
    eb_inst inst = {"m", "u", {0}, NULL, 0};
    const odin3_ast_id items[] = {eb_instance(&tree, &inst)};
    const odin3_ast_id modules[] = {module_of("t", no_range(), items, 1), child};
    (void)eb_unit(&tree, modules, 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    odin3_symtab *st = symtab();
    odin3_scope_id work = odin3_scope_first_child(st, (odin3_scope_id){1});
    expect_scope(work, (scope_want){ODIN3_SCOPE_LIBRARY, "work", false, 0});
    odin3_scope_id mylib = odin3_scope_next_sibling(st, work);
    expect_scope(mylib, (scope_want){ODIN3_SCOPE_LIBRARY, "mylib", false, 0});
    TEST_ASSERT_EQUAL_UINT32(mylib.v,
                             odin3_scope_get(st, odin3_elab_scope_of(&bnd, child))->parent.v);
    TEST_ASSERT_EQUAL_UINT32(local_id(mylib, "m").v, odin3_elab_sym_of(&bnd, items[0]).v);
}

static void test_hierarchical_through_instance(void) {
    eb_inst inst_u = {"cell", "u", {0}, NULL, 0};
    et_line(&tree, 3);
    const odin3_ast_id parts[] = {ident("u"), ident("w")};
    et_col(&tree, 14);
    odin3_ast_id hier = eb_hier(&tree, parts, 2);
    const odin3_ast_id items[] = {wire("y", no_range()), eb_instance(&tree, &inst_u),
                                  cont(ident("y"), hier)};
    (void)top_of(items, 3);
    expect_failure(ODIN3_ERR_PARSE,
                   "b.v:3:14: error: hierarchical reference 'u.w' is not supported");
}

/* A hierarchical call (rule 7's row too): E21 at the CALL. */
static void test_hierarchical_call(void) {
    et_line(&tree, 4);
    odin3_ast_spec spec = {
        .kind = ODIN3_AST_CALL, .flags = ODIN3_AST_F_CALL_HIER, .name = eb_intern(&tree, "u.f")};
    odin3_ast_id call = et_node(&tree, spec, NULL, 0);
    const odin3_ast_id items[] = {wire("y", no_range()), cont(ident("y"), call)};
    (void)top_of(items, 2);
    expect_failure(ODIN3_ERR_PARSE,
                   "b.v:4:1: error: hierarchical reference 'u.f' is not supported");
}

/* g[5].tt where g has iterations 0 and 1; f.x into a function: E21. */
static void test_hier_missing(void) {
    eb_block g0 = {"g", eb_int(&tree, 0), NULL, 0};
    eb_block g1 = {"g", eb_int(&tree, 1), NULL, 0};
    et_line(&tree, 4);
    const odin3_ast_id parts[] = {eb_index(&tree, "g", 5), ident("tt")};
    odin3_ast_id hier = eb_hier(&tree, parts, 2);
    et_line(&tree, 5);
    const odin3_ast_id into_f[] = {ident("f"), ident("x")};
    odin3_ast_id hier_f = eb_hier(&tree, into_f, 2);
    eb_subr fn = {.name = "f"};
    const odin3_ast_id items[] = {wire("y", no_range()),    eb_gen_block(&tree, &g0),
                                  eb_gen_block(&tree, &g1), eb_function(&tree, &fn),
                                  cont(ident("y"), hier),   cont(ident("y"), hier_f)};
    (void)top_of(items, 6);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    expect_line(
        "b.v:4:1: error: 'g[5].tt' does not exist (generate block 'g[5]' was not elaborated)");
    expect_line("b.v:5:1: error: hierarchical reference 'f.x' is not supported");
    TEST_ASSERT_EQUAL_UINT32(2, error_count());
}

/* A HIER_NAME ends at a declaration: g[1].tt[0] (an indexed last component) is E21. */
static void test_hier_indexed_end(void) {
    const odin3_ast_id tt[] = {wire("tt", vec(1))};
    eb_block g1 = {"g", eb_int(&tree, 1), tt, 1};
    et_line(&tree, 6);
    const odin3_ast_id parts[] = {eb_index(&tree, "g", 1), eb_index(&tree, "tt", 0)};
    odin3_ast_id hier = eb_hier(&tree, parts, 2);
    const odin3_ast_id items[] = {wire("y", no_range()), eb_gen_block(&tree, &g1),
                                  cont(ident("y"), hier)};
    (void)top_of(items, 3);
    expect_failure(ODIN3_ERR_PARSE,
                   "b.v:6:1: error: hierarchical reference 'g[1].tt[0]' is not supported");
}

static void test_not_function_or_task(void) {
    eb_subr task = {.name = "k"};
    eb_subr fn = {.name = "f"};
    et_line(&tree, 6);
    odin3_ast_id call = et_call(&tree, "k", NULL, 0);
    et_line(&tree, 7);
    odin3_ast_id task_call = eb_task_call(&tree, "f", NULL, 0);
    et_line(&tree, 8);
    odin3_ast_id nothing = et_call(&tree, "nothing", NULL, 0);
    const odin3_ast_id items[] = {wire("y", no_range()),        eb_task(&tree, &task),
                                  eb_function(&tree, &fn),      cont(ident("y"), call),
                                  eb_initial(&tree, task_call), cont(ident("y"), nothing)};
    (void)top_of(items, 6);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    expect_line("b.v:6:1: error: 'k' is not a function");
    expect_line("b.v:7:1: error: 'f' is not a task");
    expect_line("b.v:8:1: error: 'nothing' is not declared");
    TEST_ASSERT_EQUAL_UINT32(3, error_count());
}

static void test_width(void) {
    et_line(&tree, 2);
    et_col(&tree, 9);
    eb_bounds bounds = {et_num(&tree, "2147483648"), eb_int(&tree, 0)};
    et_col(&tree, 8);
    const odin3_ast_id items[] = {wire("big", eb_range(&tree, bounds))};
    (void)top_of(items, 1);
    expect_failure(ODIN3_ERR_PARSE,
                   "b.v:2:9: error: width 2147483649 exceeds the limit of 1048576 bits");
}

/* An unpacked dimension has no width cap (memories are EL-27's), but its bounds are int32. */
static void test_unpacked_bounds(void) {
    eb_bounds words = {eb_int(&tree, 0), eb_int(&tree, 1 << 21)};
    const odin3_ast_id deep[] = {eb_range(&tree, words)};
    eb_decl mem = {.sub = ODIN3_AST_VAR_DECL_REG, .name = "mem", .dims = deep, .n_dims = 1};
    et_line(&tree, 3);
    et_col(&tree, 14);
    eb_bounds wide = {eb_int(&tree, 0), et_num(&tree, "36'sh800000000")}; /* -2^35 */
    et_col(&tree, 1);
    const odin3_ast_id far[] = {eb_range(&tree, wide)};
    eb_decl far_mem = {.sub = ODIN3_AST_VAR_DECL_REG, .name = "far", .dims = far, .n_dims = 1};
    /* the int32 edges themselves are bounds (review m3) */
    const odin3_ast_id edges[] = {
        eb_range(&tree, (eb_bounds){eb_int(&tree, INT32_MIN), eb_int(&tree, INT32_MAX)})};
    eb_decl edge_mem = {.sub = ODIN3_AST_VAR_DECL_REG, .name = "edge", .dims = edges, .n_dims = 1};
    const odin3_ast_id items[] = {eb_var(&tree, &mem), eb_var(&tree, &far_mem),
                                  eb_var(&tree, &edge_mem)};
    (void)top_of(items, 3);
    expect_failure(ODIN3_ERR_PARSE,
                   "b.v:3:14: error: width 34359738369 exceeds the limit of 1048576 bits");
    odin3_scope_id lib = odin3_scope_first_child(symtab(), (odin3_scope_id){1});
    odin3_scope_id mod = odin3_scope_child(symtab(), lib, name_id("t"), ODIN3_SCOPE_NO_INDEX);
    odin3_dim_span span = odin3_symbol_unpacked(symtab(), local_id(mod, "mem"));
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_INT32(1 << 21, span.dims[0].lsb); /* bound, though the binding failed */
    span = odin3_symbol_unpacked(symtab(), local_id(mod, "edge"));
    TEST_ASSERT_EQUAL_UINT32(1, span.n);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, span.dims[0].msb);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, span.dims[0].lsb);
}

/* A packed range wider than the cap (at the RANGE) and an x bound. */
static void test_width_and_xz(void) {
    et_line(&tree, 2);
    odin3_ast_id wide = eb_range(&tree, (eb_bounds){eb_int(&tree, 1 << 20), eb_int(&tree, 0)});
    et_line(&tree, 3);
    odin3_ast_id unknown = eb_range(&tree, (eb_bounds){et_num(&tree, "4'bx"), eb_int(&tree, 0)});
    const odin3_ast_id items[] = {wire("wide", wide), wire("unknown", unknown)};
    (void)top_of(items, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    expect_line("b.v:2:1: error: width 1048577 exceeds the limit of 1048576 bits");
    expect_line("b.v:3:1: error: range bound has x or z bits");
    TEST_ASSERT_EQUAL_UINT32(2, error_count());
}

static void test_error_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_errors = LOW_CAP;
    odin3_elab_test_set_limits(&limits);
    odin3_ast_id items[6];
    items[0] = wire("y", no_range());
    for (uint32_t i = 1; i < 6; i++) {
        et_line(&tree, 1 + i);
        items[i] = cont(ident("y"), ident("x"));
    }
    (void)top_of(items, 6);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, bind());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(LOW_CAP + 1, error_count(), all_lines());
    expect_line("b.v:4:1: error: 'x' is not declared");
    expect_line("b.v:5:1: error: too many errors; elaboration stopped after 3");
}

static void test_misuse(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_bind(NULL, &bnd));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_bind(tree.ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, bind()); /* no UNIT yet */
    expect_line("odin3_elab_bind: the elaborated form holds exactly one UNIT");
    et_tree parsed;
    et_init(&parsed, design, "p.v");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_bind(parsed.ast, &bnd));
    expect_line("odin3_elab_bind: the store is not ELABORATED");
    et_free(&parsed);
    const odin3_ast_id items[] = {wire("y", no_range())};
    (void)top_of(items, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bind());
    odin3_elab_binding second = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_bind(tree.ast, &second));
    expect_line("odin3_elab_bind: the store has a symbol table already");
    TEST_ASSERT_NULL(second.sym_of);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_scope_of(NULL, (odin3_ast_id){1}).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_sym_of(&bnd, (odin3_ast_id){UINT32_MAX}).v);
    odin3_elab_binding_free(NULL);
}

/* --- EL-4 violations (the rows rule 7 does not cover) -------------------------------------- */

/* Rule 7 accepts the store; the bind is INVALID_ARG after exactly the line, with no IR made. */
static void expect_shape(const char *line) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL));
    n_lines = 0;
    expect_failure(ODIN3_ERR_INVALID_ARG, line);
    uint32_t modules = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &modules));
    TEST_ASSERT_EQUAL_UINT32(0, modules);
}

static odin3_ast_id while_stmt(void) {
    const odin3_ast_id kids[] = {eb_int(&tree, 0), null_stmt()};
    return eb_node(&tree, (eb_shape){ODIN3_AST_WHILE, 0, 0, NULL}, kids, 2);
}

static void test_shape_while(void) {
    eb_subr fn = {.name = "f", .body = while_stmt()}; /* allowed in a function */
    et_line(&tree, 3);
    et_col(&tree, 11);
    odin3_ast_id loop = while_stmt();
    const odin3_ast_id items[] = {eb_function(&tree, &fn), eb_initial(&tree, loop)};
    (void)top_of(items, 2);
    expect_shape("b.v:3:11: error: elaborated form: 'WHILE' outside a function");
}

static void test_shape_system_function(void) {
    const odin3_ast_id arg[] = {ident("y")};
    odin3_ast_id allowed = et_call(&tree, "$signed", arg, 1);
    et_line(&tree, 4);
    et_col(&tree, 15);
    odin3_ast_id random = et_call(&tree, "$random", NULL, 0);
    const odin3_ast_id items[] = {wire("y", no_range()), cont(ident("y"), allowed),
                                  cont(ident("y"), random)};
    (void)top_of(items, 3);
    expect_shape("b.v:4:15: error: elaborated form: system function '$random' (only $signed and "
                 "$unsigned are allowed)");
}

static void test_shape_system_task(void) {
    odin3_ast_id allowed = eb_initial(&tree, eb_task_call(&tree, "$readmemb", NULL, 0));
    et_line(&tree, 5);
    odin3_ast_id outside = eb_task_call(&tree, "$readmemh", NULL, 0);
    const odin3_ast_id items[] = {allowed, eb_always(&tree, NULL, 0, outside)};
    (void)top_of(items, 2);
    expect_shape(
        "b.v:5:1: error: elaborated form: system task '$readmemh' outside an initial block");
}

static void test_shape_other_system_task(void) {
    et_line(&tree, 6);
    const odin3_ast_id items[] = {eb_initial(&tree, eb_task_call(&tree, "$display", NULL, 0))};
    (void)top_of(items, 1);
    expect_shape("b.v:6:1: error: elaborated form: system task '$display' (only $readmemh and "
                 "$readmemb are allowed)");
}

static void test_shape_mixed_events(void) {
    const odin3_ast_id events[] = {eb_event(&tree, ODIN3_AST_EVENT_EXPR_POSEDGE, ident("c")),
                                   eb_event(&tree, ODIN3_AST_EVENT_EXPR_NONE, ident("d"))};
    et_line(&tree, 7);
    et_col(&tree, 10);
    odin3_ast_id control = eb_control(&tree, events, 2);
    et_col(&tree, 1);
    odin3_ast_id body = eb_assign(&tree, ODIN3_AST_NONBLOCKING_ASSIGN, ident("r"), ident("d"));
    const odin3_ast_id timing[] = {eb_timing(&tree, control, body)};
    odin3_ast_id always =
        eb_node(&tree, (eb_shape){ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, 0, NULL}, timing, 1);
    const odin3_ast_id items[] = {wire("c", no_range()), wire("d", no_range()),
                                  reg("r", no_range()), always};
    (void)top_of(items, 4);
    expect_shape("b.v:7:10: error: elaborated form: edge and level events in one event list");
}

static void test_shape_timing_outside_always(void) {
    const odin3_ast_id events[] = {eb_event(&tree, ODIN3_AST_EVENT_EXPR_POSEDGE, ident("c"))};
    odin3_ast_id body = blocking(ident("r"), ident("c"));
    et_line(&tree, 8);
    odin3_ast_id timing = eb_timing(&tree, eb_control(&tree, events, 1), body);
    const odin3_ast_id items[] = {wire("c", no_range()), reg("r", no_range()),
                                  eb_initial(&tree, timing)};
    (void)top_of(items, 3);
    expect_shape("b.v:8:1: error: elaborated form: 'TIMING_STMT' outside an always");
}

static void test_shape_intra_assignment(void) {
    const odin3_ast_id events[] = {eb_event(&tree, ODIN3_AST_EVENT_EXPR_POSEDGE, ident("c"))};
    et_line(&tree, 9);
    et_col(&tree, 9);
    odin3_ast_id control = eb_control(&tree, events, 1);
    const odin3_ast_id kids[] = {ident("r"), control, ident("c")};
    odin3_ast_id assign =
        eb_node(&tree, (eb_shape){ODIN3_AST_BLOCKING_ASSIGN, 0, 0, NULL}, kids, 3);
    const odin3_ast_id items[] = {wire("c", no_range()), reg("r", no_range()),
                                  eb_always(&tree, NULL, 0, assign)};
    (void)top_of(items, 3);
    expect_shape("b.v:9:9: error: elaborated form: intra-assignment control");
}

static void test_shape_count(void) {
    const odin3_ast_id part[] = {ident("y")};
    et_line(&tree, 10);
    et_col(&tree, 13);
    odin3_ast_id count = eb_int(&tree, -1);
    odin3_ast_id rep = et_replicate(&tree, count, et_concat(&tree, part, 1));
    const odin3_ast_id items[] = {wire("y", no_range()), cont(ident("y"), rep)};
    (void)top_of(items, 2);
    expect_shape("b.v:10:13: error: elaborated form: a count that is not a NUMBER of 0 or more");
}

static void test_shape_select_bound(void) {
    et_line(&tree, 11);
    et_col(&tree, 13);
    odin3_ast_id text = et_string(&tree, "s");
    et_col(&tree, 1);
    odin3_ast_id sel =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, ident("y"), text, eb_int(&tree, 0)});
    const odin3_ast_id items[] = {wire("y", vec(1)), cont(ident("y"), sel)};
    (void)top_of(items, 2);
    expect_shape("b.v:11:13: error: elaborated form: a select bound or width that is not a NUMBER");
}

/* An indexed part select's base may be any expression; its width is a NUMBER. */
static void test_shape_select_width(void) {
    et_line(&tree, 12);
    et_col(&tree, 17);
    odin3_ast_id width = et_real(&tree, 2.0);
    et_col(&tree, 1);
    odin3_ast_id sel =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART_PLUS, ident("y"), ident("y"), width});
    const odin3_ast_id items[] = {wire("y", vec(1)), cont(ident("y"), sel)};
    (void)top_of(items, 2);
    expect_shape("b.v:12:17: error: elaborated form: a select bound or width that is not a NUMBER");
}

/* An instance-array slice of any expression (v4.8: read at its self-determined width) is a valid
 * SELECT: its object need not be a name. */
static void test_select_of_expression(void) {
    odin3_ast_id sum = et_binary(&tree, ODIN3_AST_BINARY_XOR, ident("p"), ident("r"));
    odin3_ast_id slice =
        et_select(&tree, (et_sel){ODIN3_AST_SELECT_PART, sum, eb_int(&tree, 1), eb_int(&tree, 0)});
    const odin3_ast_id items[] = {wire("p", vec(3)), wire("r", vec(3)), wire("y", vec(1)),
                                  cont(ident("y"), slice)};
    (void)top_of(items, 4);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
}

static void test_shape_instance_ports(void) {
    const odin3_ast_id m_ports[] = {port("p", ODIN3_AST_PORT_DECL_INPUT, no_range()),
                                    port("q", ODIN3_AST_PORT_DECL_INPUT, no_range())};
    odin3_ast_id child = module_of("m", eb_list(&tree, m_ports, 2), NULL, 0);
    const odin3_ast_id conns[] = {eb_conn(&tree, "q", ident("x")), eb_conn(&tree, "p", ident("x"))};
    et_line(&tree, 12);
    eb_inst inst_u = {"m", "u", {0}, conns, 2};
    const odin3_ast_id items[] = {wire("x", no_range()), eb_instance(&tree, &inst_u)};
    const odin3_ast_id modules[] = {module_of("t", no_range(), items, 2), child};
    (void)eb_unit(&tree, modules, 2);
    expect_shape("b.v:12:1: error: elaborated form: instance 'u' does not connect every port of "
                 "'m' once, in port order");
}

/* One connection short: the count rule. */
static void test_shape_instance_count(void) {
    const odin3_ast_id m_ports[] = {port("p", ODIN3_AST_PORT_DECL_INPUT, no_range()),
                                    port("q", ODIN3_AST_PORT_DECL_INPUT, no_range())};
    odin3_ast_id child = module_of("m", eb_list(&tree, m_ports, 2), NULL, 0);
    const odin3_ast_id conns[] = {eb_conn(&tree, "p", ident("x"))};
    et_line(&tree, 13);
    eb_inst inst_v = {"m", "v", {0}, conns, 1};
    const odin3_ast_id items[] = {wire("x", no_range()), eb_instance(&tree, &inst_v)};
    const odin3_ast_id modules[] = {module_of("t", no_range(), items, 2), child};
    (void)eb_unit(&tree, modules, 2);
    expect_shape("b.v:13:1: error: elaborated form: instance 'v' does not connect every port of "
                 "'m' once, in port order");
}

/* One connection too many: the count rule. */
static void test_shape_instance_extra(void) {
    const odin3_ast_id m_ports[] = {port("p", ODIN3_AST_PORT_DECL_INPUT, no_range())};
    odin3_ast_id child = module_of("m", eb_list(&tree, m_ports, 1), NULL, 0);
    const odin3_ast_id conns[] = {eb_conn(&tree, "p", ident("x")), eb_conn(&tree, "q", ident("x"))};
    et_line(&tree, 14);
    eb_inst inst_w = {"m", "w", {0}, conns, 2};
    const odin3_ast_id items[] = {wire("x", no_range()), eb_instance(&tree, &inst_w)};
    const odin3_ast_id modules[] = {module_of("t", no_range(), items, 2), child};
    (void)eb_unit(&tree, modules, 2);
    expect_shape("b.v:14:1: error: elaborated form: instance 'w' does not connect every port of "
                 "'m' once, in port order");
}

/* The four EL-4 rows carried from Task 13's first report (review m2). */
static void test_shape_instance_range(void) {
    odin3_ast_id range = vec(1);
    et_line(&tree, 15);
    const odin3_ast_id kids[] = {range};
    odin3_ast_id inst = eb_node(&tree, (eb_shape){ODIN3_AST_INSTANCE, 0, 0, "u"}, kids, 1);
    const odin3_ast_id holder[] = {{0}, inst};
    const odin3_ast_id items[] = {
        eb_node(&tree, (eb_shape){ODIN3_AST_INSTANTIATION, 0, 0, "cell"}, holder, 2)};
    (void)top_of(items, 1);
    expect_shape("b.v:15:1: error: elaborated form: instance 'u' with a range");
}

static void test_shape_net_kind(void) {
    et_line(&tree, 16);
    eb_decl tri = {.sub = ODIN3_AST_NET_DECL_TRI1, .name = "w"};
    const odin3_ast_id items[] = {eb_net(&tree, &tri)};
    (void)top_of(items, 1);
    expect_shape("b.v:16:1: error: elaborated form: net 'w' of kind 'tri1'");
}

static void test_shape_port_trireg(void) {
    et_line(&tree, 17);
    eb_decl charge = {
        .sub = ODIN3_AST_PORT_DECL_INPUT,
        .flags = (uint16_t)((1U + ODIN3_AST_NET_DECL_TRIREG) << ODIN3_AST_F_PORT_DECL_NET_SHIFT),
        .name = "p"};
    const odin3_ast_id ports[] = {eb_port(&tree, &charge)};
    odin3_ast_id module = module_of("t", eb_list(&tree, ports, 1), NULL, 0);
    (void)eb_unit(&tree, &module, 1);
    expect_shape("b.v:17:1: error: elaborated form: net 'p' of kind 'trireg'");
}

static void test_shape_switch_gate(void) {
    const odin3_ast_id terms[] = {{0},
                                  eb_conn(&tree, NULL, ident("y")),
                                  eb_conn(&tree, NULL, ident("y")),
                                  eb_conn(&tree, NULL, ident("y"))};
    odin3_ast_id inst = eb_node(&tree, (eb_shape){ODIN3_AST_INSTANCE, 0, 0, NULL}, terms, 4);
    et_line(&tree, 18);
    const odin3_ast_id gate_kids[] = {{0}, {0}, inst};
    odin3_ast_id gate = eb_node(
        &tree, (eb_shape){ODIN3_AST_GATE_DECL, ODIN3_AST_GATE_DECL_NMOS, 0, NULL}, gate_kids, 3);
    const odin3_ast_id items[] = {wire("y", no_range()), gate};
    (void)top_of(items, 2);
    expect_shape("b.v:18:1: error: elaborated form: switch-level or pull gate 'nmos'");
}

static void test_shape_vectored(void) {
    et_line(&tree, 19);
    eb_decl vectored = {.sub = ODIN3_AST_NET_DECL_WIRE,
                        .flags = ODIN3_AST_F_NET_DECL_VECTORED,
                        .range = vec(3),
                        .name = "w"};
    const odin3_ast_id items[] = {eb_net(&tree, &vectored)};
    (void)top_of(items, 1);
    expect_shape("b.v:19:1: error: elaborated form: 'vectored' on a net declaration");
}

static void test_shape_scalared(void) {
    et_line(&tree, 20);
    eb_decl scalared = {.sub = ODIN3_AST_NET_DECL_WIRE,
                        .flags = ODIN3_AST_F_NET_DECL_SCALARED,
                        .range = vec(3),
                        .name = "w"};
    const odin3_ast_id items[] = {eb_net(&tree, &scalared)};
    (void)top_of(items, 1);
    expect_shape("b.v:20:1: error: elaborated form: 'scalared' on a net declaration");
}

static void test_shape_port_net_kind(void) {
    et_line(&tree, 1);
    et_col(&tree, 17);
    eb_decl bare = {.sub = ODIN3_AST_PORT_DECL_INPUT, .name = "p"};
    const odin3_ast_id ports[] = {eb_port(&tree, &bare)};
    odin3_ast_id module = module_of("t", eb_list(&tree, ports, 1), NULL, 0);
    (void)eb_unit(&tree, &module, 1);
    expect_shape("b.v:1:17: error: elaborated form: port 'p' without a net kind");
}

static void test_shape_parameter(void) {
    et_line(&tree, 13);
    et_col(&tree, 14);
    const odin3_ast_id items[] = {eb_localparam(&tree, "P", et_real(&tree, 1.5))};
    (void)top_of(items, 1);
    expect_shape("b.v:13:14: error: elaborated form: parameter 'P' is not a localparam with a "
                 "NUMBER or STRING value");
}

static void test_shape_unnamed_block(void) {
    const odin3_ast_id locals[] = {reg("b", no_range())};
    et_line(&tree, 14);
    et_col(&tree, 11);
    eb_block blk = {NULL, eb_list(&tree, locals, 1), NULL, 0};
    const odin3_ast_id items[] = {eb_initial(&tree, eb_named_block(&tree, &blk))};
    (void)top_of(items, 1);
    expect_shape("b.v:14:11: error: elaborated form: unnamed block 'b' with declarations");
}

static void test_shape_range_bound(void) {
    et_line(&tree, 15);
    et_col(&tree, 7);
    odin3_ast_id text = et_string(&tree, "s");
    const odin3_ast_id items[] = {wire("w", eb_range(&tree, (eb_bounds){text, eb_int(&tree, 0)}))};
    (void)top_of(items, 1);
    expect_shape("b.v:15:7: error: elaborated form: a range bound that is not a NUMBER");
}

static void test_shape_generate_index(void) {
    et_line(&tree, 16);
    et_col(&tree, 3);
    eb_block gen = {"g", et_num(&tree, "2'bx1"), NULL, 0};
    et_col(&tree, 1);
    const odin3_ast_id items[] = {eb_gen_block(&tree, &gen)};
    (void)top_of(items, 1);
    expect_shape("b.v:16:3: error: elaborated form: a generate index that is not a 32-bit NUMBER");
}

/* --- scale and out of memory --------------------------------------------------------------- */

/* NEST GEN_BLOCKs, each in the one before; the innermost reads the module's wire top_w. */
static void test_deep_nesting(void) {
    odin3_ast_id use = ident("top_w");
    const odin3_ast_id leaf[] = {wire("leaf", no_range()), cont(ident("leaf"), use)};
    eb_block inner = {"g", {0}, leaf, 2};
    odin3_ast_id block = eb_gen_block(&tree, &inner);
    for (uint32_t i = 1; i < NEST; i++) {
        eb_block outer = {"g", {0}, &block, 1};
        block = eb_gen_block(&tree, &outer);
    }
    const odin3_ast_id items[] = {wire("top_w", no_range()), block};
    odin3_ast_id module = top_of(items, 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    odin3_scope_id mod = odin3_elab_scope_of(&bnd, module);
    TEST_ASSERT_EQUAL_UINT32(local_id(mod, "top_w").v, odin3_elab_sym_of(&bnd, use).v);
    TEST_ASSERT_EQUAL_UINT32(3 + NEST, odin3_symtab_scope_count(symtab()));
}

/* A store to sweep: its builder and the status of a bind that meets no failure. */
typedef struct oom_case {
    void (*build)(void);
    odin3_status done;
} oom_case;

/* Iteration i of the wide loop: g[i]: begin wire c; assign c = g[i-1].c; end (no assignment for
 * i = 0); *use gets the HIER_NAME. */
static odin3_ast_id wide_iteration(int32_t index, odin3_ast_id *use) {
    odin3_ast_id body[2] = {wire("c", no_range()), {0}};
    uint32_t count = 1;
    if (index > 0) {
        const odin3_ast_id parts[] = {eb_index(&tree, "g", index - 1), ident("c")};
        *use = eb_hier(&tree, parts, 2);
        body[1] = cont(ident("c"), *use);
        count = 2;
    }
    eb_block blk = {"g", eb_int(&tree, index), body, count};
    return eb_gen_block(&tree, &blk);
}

/* A generate loop of WIDE iterations, each but the first reading its predecessor (g[i-1].c, the
 * carry-chain idiom), binds in linear time: inserts past a chain's tail and position lookups are
 * O(1) (review I1; 40,000 took 4.65 s in Release before). Release (NDEBUG) bounds the CPU time. */
static void test_wide_loop(void) {
    static odin3_ast_id items[2 + WIDE]; /* params and ports slots stay 0 */
    odin3_ast_id last_use = {0};
    for (int32_t i = 0; i < WIDE; i++) {
        items[2 + i] = wide_iteration(i, &last_use);
    }
    odin3_ast_id module = eb_node(&tree, (eb_shape){ODIN3_AST_MODULE, 0, 0, "t"}, items, 2 + WIDE);
    (void)eb_unit(&tree, &module, 1);
    clock_t start = clock();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, bind(), all_lines());
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    odin3_scope_id mod = odin3_elab_scope_of(&bnd, module);
    odin3_scope_id before = odin3_scope_child(symtab(), mod, name_id("g"), WIDE - 2);
    TEST_ASSERT_EQUAL_UINT32(local_id(before, "c").v, odin3_elab_sym_of(&bnd, last_use).v);
    char note[LINE_BYTES];
    int got = snprintf(note, sizeof note, "bind of %d iterations: %.3f s CPU", WIDE, seconds);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof note);
    TEST_MESSAGE(note);
#ifdef NDEBUG
    TEST_ASSERT_TRUE_MESSAGE(seconds < WIDE_BUDGET_S, note);
#else
    (void)WIDE_BUDGET_S; /* sanitizer builds are not timed */
#endif
}

/* One bind of a fresh store with the allocation after `limit` ones failing: true when it gives
 * done; a failure is NO_MEMORY with nothing bound, the partial symbol table staying with the store
 * (it passes rule 8 and is freed with it). */
static bool oom_step(const oom_case *at, long limit) {
    tearDown();
    setUp();
    at->build();
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = bind();
    odin3_util_set_alloc_fail_after(-1);
    if (st != at->done) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(bnd.sym_of);
        TEST_ASSERT_NULL(bnd.scope_of);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(tree.ast, ODIN3_CHECK_FULL));
    }
    return st == at->done;
}

/* Every allocation fails in turn until OOM_SETTLE runs in a row give done (LSan: no leak). */
static void oom_sweep(const oom_case *at) {
    uint32_t settled = 0;
    uint32_t failed = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        bool ok = oom_step(at, limit);
        settled = ok ? settled + 1 : 0;
        failed += ok ? 0 : 1;
    }
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    TEST_ASSERT_TRUE(failed > OOM_MIN_FAILURES);
}

static void build_main_plain(void) {
    fixture fx;
    build_main(&fx);
}

/* Errors and violations on the failure paths: a duplicate, an undeclared name, E21, an EL-4 row. */
static void build_errors(void) {
    eb_inst inst_u = {"cell", "u", {0}, NULL, 0};
    const odin3_ast_id parts[] = {ident("u"), ident("w")};
    const odin3_ast_id items[] = {
        wire("y", no_range()),        reg("y", no_range()),
        eb_instance(&tree, &inst_u),  cont(ident("y"), eb_hier(&tree, parts, 2)),
        cont(ident("y"), ident("x")), eb_initial(&tree, while_stmt())};
    (void)top_of(items, 6);
}

static void test_oom(void) {
    const oom_case clean = {build_main_plain, ODIN3_OK};
    oom_sweep(&clean);
    const oom_case errors = {build_errors, ODIN3_ERR_INVALID_ARG};
    oom_sweep(&errors);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_symbols_objects);
    RUN_TEST(test_symbols_localparam);
    RUN_TEST(test_symbols_ports_and_scopes);
    RUN_TEST(test_scopes_top);
    RUN_TEST(test_scopes_module);
    RUN_TEST(test_resolution);
    RUN_TEST(test_scope_of);
    RUN_TEST(test_assignments);
    RUN_TEST(test_concat_lvalues);
    RUN_TEST(test_symbol_forms);
    RUN_TEST(test_blackbox);
    RUN_TEST(test_undeclared);
    RUN_TEST(test_duplicate);
    RUN_TEST(test_duplicate_blocks);
    RUN_TEST(test_duplicate_modules);
    RUN_TEST(test_module_name_not_value);
    RUN_TEST(test_not_a_value);
    RUN_TEST(test_library_scopes);
    RUN_TEST(test_hierarchical_through_instance);
    RUN_TEST(test_hierarchical_call);
    RUN_TEST(test_hier_missing);
    RUN_TEST(test_hier_indexed_end);
    RUN_TEST(test_not_function_or_task);
    RUN_TEST(test_width);
    RUN_TEST(test_unpacked_bounds);
    RUN_TEST(test_width_and_xz);
    RUN_TEST(test_error_cap);
    RUN_TEST(test_misuse);
    RUN_TEST(test_shape_while);
    RUN_TEST(test_shape_system_function);
    RUN_TEST(test_shape_system_task);
    RUN_TEST(test_shape_other_system_task);
    RUN_TEST(test_shape_mixed_events);
    RUN_TEST(test_shape_timing_outside_always);
    RUN_TEST(test_shape_intra_assignment);
    RUN_TEST(test_shape_count);
    RUN_TEST(test_shape_select_bound);
    RUN_TEST(test_shape_select_width);
    RUN_TEST(test_select_of_expression);
    RUN_TEST(test_shape_instance_ports);
    RUN_TEST(test_shape_instance_count);
    RUN_TEST(test_shape_instance_extra);
    RUN_TEST(test_shape_instance_range);
    RUN_TEST(test_shape_net_kind);
    RUN_TEST(test_shape_port_trireg);
    RUN_TEST(test_shape_switch_gate);
    RUN_TEST(test_shape_vectored);
    RUN_TEST(test_shape_scalared);
    RUN_TEST(test_shape_port_net_kind);
    RUN_TEST(test_shape_parameter);
    RUN_TEST(test_shape_unnamed_block);
    RUN_TEST(test_shape_range_bound);
    RUN_TEST(test_shape_generate_index);
    RUN_TEST(test_deep_nesting);
    RUN_TEST(test_wide_loop);
    RUN_TEST(test_oom);
    return UNITY_END();
}
