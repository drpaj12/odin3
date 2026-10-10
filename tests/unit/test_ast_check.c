/*
 * test_ast_check.c — unit tests for the AST structural check (spec §10): a valid parsed and a
 * valid elaborated module pass FULL, one negative per check line of rules 1–8 (built directly or
 * through the corruption hook), FAST's rule set, the log cap, misuse and OOM.
 */
#include "ast/ast.h"
#include "ast/ast_test.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "ir/check.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_CAP = 1 << 16,
    NEEDLE = 64,
    OOM_LIMIT = 64,
    FILE_BYTES = 40,
    MANY_ERRORS = 30,
    MANY_SYMBOLS = 12,
    BAD_DT = 6,    /* one past ODIN3_AST_DT_TIME */
    BAD_DRIVE = 6, /* one past ODIN3_AST_DRIVE_SUPPLY */
};

static odin3_design *design;
static odin3_ast *ast;
static char log_text[LOG_CAP];
static size_t log_len;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    const char *tag = "W ";
    if (level == ODIN3_LOG_ERROR) {
        tag = "E ";
    } else if (level == ODIN3_LOG_INFO) {
        tag = "I ";
    }
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%s%s\n", tag, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static void reset_log(void) {
    log_text[0] = '\0';
    log_len = 0;
}

static void new_store(odin3_ast_form form) {
    odin3_ast_destroy(ast);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, form, (odin3_passrun_id){1}, &ast));
}

void setUp(void) {
    odin3_log_set_sink(capture_sink, NULL);
    reset_log();
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    new_store(ODIN3_AST_FORM_PARSED);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- log assertions ------------------------------------------------------------------------ */

static bool logged_rule(int rule) {
    char needle[NEEDLE];
    (void)snprintf(needle, sizeof needle, "E ast check: rule %d:", rule);
    return strstr(log_text, needle) != NULL;
}

static void assert_ok(odin3_check_level level) {
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(ast, level));
    TEST_ASSERT_NULL(strstr(log_text, "E "));
}

/* FULL fails with rule `rule` (others may cascade). */
static void expect_rule(int rule) {
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_ast_check(ast, ODIN3_CHECK_FULL));
    if (!logged_rule(rule)) {
        TEST_FAIL_MESSAGE(log_text);
    }
}

/* FULL fails with rule `rule` and no other. */
static void expect_only(int rule) {
    expect_rule(rule);
    for (int other = 1; other <= ODIN3_AST_CHECK_RULES; other++) {
        if (other != rule && logged_rule(other)) {
            TEST_FAIL_MESSAGE(log_text);
        }
    }
}

static void expect_text(const char *needle) {
    if (strstr(log_text, needle) == NULL) {
        TEST_FAIL_MESSAGE(log_text);
    }
}

static void corrupt(odin3_ast_test_fault fault, uint32_t id, uint32_t arg) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_test_corrupt(ast, (odin3_ast_test_target){fault, id, arg}));
}

/* --- builders ------------------------------------------------------------------------------ */

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_id make(odin3_ast_spec spec, const odin3_ast_id *kids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_make(ast, &spec, kids, n, &id), log_text);
    return id;
}

static odin3_ast_id node(uint8_t kind, uint8_t sub, const odin3_ast_id *kids, uint32_t n) {
    return make((odin3_ast_spec){.kind = kind, .sub = sub}, kids, n);
}

static odin3_ast_id named(uint8_t kind, const char *name, const odin3_ast_id *kids, uint32_t n) {
    return make((odin3_ast_spec){.kind = kind, .name = intern(name)}, kids, n);
}

static odin3_ast_id ident(const char *name) {
    return named(ODIN3_AST_IDENT, name, NULL, 0);
}

static odin3_ast_id number(const char *size, const char *base, const char *digits) {
    odin3_number_text text = {odin3_bytes_cstr(size), odin3_bytes_cstr(base),
                              odin3_bytes_cstr(digits)};
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(text, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_number_parse(ast, (odin3_loc){0}, text, &spec.payload));
    return make(spec, NULL, 0);
}

static odin3_ast_id dec(const char *digits) {
    return number("", "", digits);
}

#define KIDS(...) ((const odin3_ast_id[]){__VA_ARGS__})
#define NKIDS(...) (uint32_t)(sizeof KIDS(__VA_ARGS__) / sizeof(odin3_ast_id))
#define NODE(kind, sub, ...) node(ODIN3_AST_##kind, (sub), KIDS(__VA_ARGS__), NKIDS(__VA_ARGS__))
#define NAMED(kind, name, ...)                                                                     \
    named(ODIN3_AST_##kind, (name), KIDS(__VA_ARGS__), NKIDS(__VA_ARGS__))

static const odin3_ast_id NIL = {0};

static odin3_ast_id declarator(const char *name, odin3_ast_id value) {
    return NAMED(DECLARATOR, name, value);
}

static odin3_ast_id net(const char *name) {
    return NODE(NET_DECL, ODIN3_AST_NET_DECL_WIRE, NIL, NIL, NIL, declarator(name, NIL));
}

static odin3_ast_id bit_select(const char *base, const char *index) {
    return NODE(SELECT, ODIN3_AST_SELECT_BIT, ident(base), dec(index), NIL);
}

static odin3_ast_id posedge(const char *clock) {
    odin3_ast_id edge = NODE(EVENT_EXPR, ODIN3_AST_EVENT_EXPR_POSEDGE, ident(clock));
    return NODE(EVENT_CONTROL, 0, NIL, edge);
}

static odin3_ast_id unit_of_two(odin3_ast_id first, odin3_ast_id second) {
    return NAMED(UNIT, "f.v", first, second);
}

static odin3_ast_id unit_of(odin3_ast_id module) {
    return NAMED(UNIT, "f.v", module);
}

/* A FILE buffer of FILE_BYTES bytes; its first loc. */
static odin3_loc file_loc(void) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_srcfile_spec spec = {.name = intern("f.v"), .len = FILE_BYTES};
    odin3_srcbuf_id buf = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &buf));
    return odin3_srcman_loc(sm, buf, 0);
}

static void comment_at(odin3_loc loc, const char *text) {
    odin3_ast_comment_spec spec = {.loc = loc.v,
                                   .kind = ODIN3_AST_COMMENT_LINE,
                                   .text_ptr = text,
                                   .text_len = (uint32_t)strlen(text)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast, &spec));
}

/* --- the valid parsed module --------------------------------------------------------------- */

/* The interesting IDs of the parsed fixture. */
typedef struct parsed {
    odin3_ast_id unit, module, case_stmt, case_item, binary, instance, number, display;
} parsed;

static odin3_ast_id parsed_ports(void) {
    odin3_ast_id msb = NODE(BINARY, ODIN3_AST_BINARY_SUB, ident("W"), dec("1"));
    odin3_ast_id range = NODE(RANGE, 0, msb, dec("0"));
    odin3_ast_spec in = {.kind = ODIN3_AST_PORT_DECL,
                         .sub = ODIN3_AST_PORT_DECL_INPUT,
                         .flags = ODIN3_AST_F_PORT_DECL_IN_HEADER};
    odin3_ast_id port_a = make(in, KIDS(range, declarator("a", NIL), declarator("clk", NIL)), 3);
    odin3_ast_spec out = in;
    out.sub = ODIN3_AST_PORT_DECL_OUTPUT;
    out.flags |= ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    odin3_ast_id port_y = make(out, KIDS(NIL, declarator("y", NIL)), 2);
    return NODE(LIST, 0, port_a, port_y);
}

static odin3_ast_id parsed_params(void) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_PARAM_DECL,
                           .sub = ODIN3_AST_PARAM_DECL_PARAMETER,
                           .flags = ODIN3_AST_F_PARAM_DECL_IN_HEADER};
    return NODE(LIST, 0, make(spec, KIDS(NIL, declarator("W", dec("8"))), 2));
}

static odin3_ast_id parsed_always(parsed *out) {
    odin3_ast_id nba = NODE(NONBLOCKING_ASSIGN, 0, ident("y"), NIL, ident("w"));
    out->number = number("4", "'d", "1");
    out->case_item = NODE(CASE_ITEM, 0, nba, out->number);
    odin3_ast_spec dflt = {.kind = ODIN3_AST_CASE_ITEM, .flags = ODIN3_AST_F_CASE_ITEM_DEFAULT};
    odin3_ast_id other = make(dflt, KIDS(node(ODIN3_AST_NULL_STMT, 0, NULL, 0)), 1);
    out->case_stmt = NODE(CASE, ODIN3_AST_CASE_CASE, ident("a"), out->case_item, other);
    odin3_ast_id attr = NAMED(ATTR, "full_case", NIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, out->case_stmt, &attr, 1));
    odin3_ast_id block = NODE(SEQ_BLOCK, 0, NIL, out->case_stmt);
    odin3_ast_id timing = NODE(TIMING_STMT, 0, posedge("clk"), block);
    return NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, timing);
}

static odin3_ast_id parsed_generate(void) {
    odin3_ast_id init = NODE(BLOCKING_ASSIGN, 0, ident("i"), NIL, dec("0"));
    odin3_ast_id cond = NODE(BINARY, ODIN3_AST_BINARY_LT, ident("i"), dec("2"));
    odin3_ast_id incr = NODE(BINARY, ODIN3_AST_BINARY_ADD, ident("i"), dec("1"));
    odin3_ast_id step = NODE(BLOCKING_ASSIGN, 0, ident("i"), NIL, incr);
    odin3_ast_id body = NAMED(GEN_BLOCK, "g", NIL, net("t"));
    odin3_ast_id loop = NODE(GEN_FOR, 0, init, cond, step, body);
    odin3_ast_id arm = NODE(CASE_ITEM, 0, NAMED(GEN_BLOCK, "h", NIL), dec("1"));
    odin3_ast_id gcase = NODE(GEN_CASE, 0, ident("W"), arm);
    return NODE(GENERATE, 0, loop, gcase);
}

static odin3_ast_id parsed_instance(parsed *out) {
    odin3_ast_id params = NODE(LIST, 0, NAMED(CONNECTION, "W", dec("4")));
    out->instance =
        NAMED(INSTANCE, "u0", NIL, NAMED(CONNECTION, "a", ident("a")), NAMED(CONNECTION, "y", NIL));
    return NAMED(INSTANTIATION, "sub", params, out->instance);
}

static odin3_ast_id parsed_initial(parsed *out) {
    odin3_ast_spec call = {.kind = ODIN3_AST_TASK_CALL,
                           .flags = ODIN3_AST_F_TASK_CALL_SYSTEM,
                           .name = intern("$display")};
    odin3_ast_id text = named(ODIN3_AST_STRING, "x", NULL, 0);
    out->display = make(call, KIDS(text, NIL, ident("a")), 3);
    return NODE(INITIAL, 0, out->display);
}

/* module m #(parameter W = 8) (input [W-1:0] a, clk, output reg y); … endmodule, with locs,
 * an attribute, comments in reverse order, and an abandoned subtree. */
static parsed build_parsed(void) {
    parsed out;
    odin3_loc at = file_loc();
    odin3_ast_id params = parsed_params();
    odin3_ast_id ports = parsed_ports();
    odin3_ast_id assign = NODE(NET_ASSIGN, 0, ident("w"),
                               out.binary = NODE(BINARY, ODIN3_AST_BINARY_AND, bit_select("a", "0"),
                                                 number("1", "'b", "1")));
    odin3_ast_id items[] = {net("w"),
                            NODE(CONT_ASSIGN, 0, NIL, NIL, assign),
                            parsed_always(&out),
                            parsed_instance(&out),
                            NODE(GENVAR_DECL, 0, declarator("i", NIL)),
                            parsed_generate(),
                            parsed_initial(&out)};
    odin3_ast_id kids[2 + sizeof items / sizeof items[0]] = {params, ports};
    memcpy(kids + 2, items, sizeof items);
    odin3_ast_spec mod = {
        .kind = ODIN3_AST_MODULE, .loc = at, .end = {at.v + FILE_BYTES}, .name = intern("m")};
    out.module = make(mod, kids, sizeof kids / sizeof kids[0]);
    (void)ident("abandoned"); /* a syntax error's leftover: counted, not an error */
    out.unit = unit_of(out.module);
    comment_at((odin3_loc){at.v + 9}, "// second");
    comment_at((odin3_loc){at.v + 2}, "// first");
    return out;
}

/* --- the valid elaborated module ----------------------------------------------------------- */

typedef struct elaborated {
    odin3_ast_id unit, module, gen;
    odin3_sym_id sym_w;
    uint32_t name_y;
} elaborated;

static odin3_ast_id elab_ports(void) {
    odin3_ast_spec in = {.kind = ODIN3_AST_PORT_DECL, .sub = ODIN3_AST_PORT_DECL_INPUT};
    odin3_ast_id range = NODE(RANGE, 0, dec("7"), dec("0"));
    odin3_ast_id port_a = make(in, KIDS(range, declarator("a", NIL)), 2);
    odin3_ast_id port_clk = make(in, KIDS(NIL, declarator("clk", NIL)), 2);
    odin3_ast_spec out = {.kind = ODIN3_AST_PORT_DECL,
                          .sub = ODIN3_AST_PORT_DECL_OUTPUT,
                          .flags = ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT};
    odin3_ast_id port_y = make(out, KIDS(NIL, declarator("y", NIL)), 2);
    return NODE(LIST, 0, port_a, port_clk, port_y);
}

static odin3_ast_id g3_w(void) {
    return NODE(HIER_NAME, 0, bit_select("g", "3"), ident("w"));
}

static odin3_ast_id elab_module(elaborated *out) {
    odin3_ast_id local =
        NODE(PARAM_DECL, ODIN3_AST_PARAM_DECL_LOCALPARAM, NIL, declarator("W", dec("8")));
    odin3_ast_id params = NODE(LIST, 0, NAMED(CONNECTION, "W", dec("8")));
    odin3_ast_id inst = NAMED(INSTANTIATION, "leaf$W=8", params,
                              NAMED(INSTANCE, "u0", NIL, NAMED(CONNECTION, "a", ident("a"))));
    out->gen = make((odin3_ast_spec){.kind = ODIN3_AST_GEN_BLOCK, .name = intern("g")},
                    KIDS(dec("3"), net("w")), 2);
    odin3_ast_id assign =
        NODE(CONT_ASSIGN, 0, NIL, NIL, NODE(NET_ASSIGN, 0, g3_w(), bit_select("a", "0")));
    odin3_ast_id nba = NODE(NONBLOCKING_ASSIGN, 0, ident("y"), NIL, g3_w());
    odin3_ast_id always =
        NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, NODE(TIMING_STMT, 0, posedge("clk"), nba));
    return NAMED(MODULE, "sub$W=8", NIL, elab_ports(), local, inst, out->gen, assign, always);
}

static void elab_symtab(elaborated *out) {
    odin3_symtab *st = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symtab_create(ast, &st));
    odin3_scope_id ids[4] = {{0}};
    odin3_scope_spec specs[4] = {
        {.kind = ODIN3_SCOPE_DESIGN},
        {.kind = ODIN3_SCOPE_LIBRARY, .name = intern("work")},
        {.kind = ODIN3_SCOPE_MODULE, .name = intern("sub$W=8"), .node = out->module},
        {.kind = ODIN3_SCOPE_GENERATE,
         .name = intern("g"),
         .index = 3,
         .has_index = true,
         .node = out->gen}};
    for (uint32_t i = 0; i < 4; i++) {
        specs[i].parent = i > 0 ? ids[i - 1] : (odin3_scope_id){0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_scope_new(st, &specs[i], &ids[i]));
    }
    odin3_symbol_spec net_y = {.name = intern("y"), .scope = ids[2], .kind = ODIN3_SYM_VAR};
    odin3_symbol_spec net_w = {.name = intern("w"), .scope = ids[3], .kind = ODIN3_SYM_NET};
    odin3_sym_id sym = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(st, &net_y, &sym));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(st, &net_w, &out->sym_w));
    out->name_y = net_y.name;
}

static elaborated build_elaborated(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elaborated out;
    memset(&out, 0, sizeof out);
    out.module = elab_module(&out);
    out.unit = unit_of(out.module);
    elab_symtab(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    return out;
}

static void elab_wrap_item(odin3_ast_id item);

static odin3_ast_id empty_list(void) {
    return node(ODIN3_AST_LIST, 0, NULL, 0);
}

static odin3_ast_id null_stmt(void) {
    return node(ODIN3_AST_NULL_STMT, 0, NULL, 0);
}

/* assign lhs = rhs; as a module item. */
static odin3_ast_id assign_item(odin3_ast_id lhs, odin3_ast_id rhs) {
    return NODE(CONT_ASSIGN, 0, NIL, NIL, NODE(NET_ASSIGN, 0, lhs, rhs));
}

/* assign w = rhs; inside UNIT[MODULE m] of a fresh ELABORATED store, then FULL fails rule 7
 * only. rhs is made by the caller after new_store. */
static void elab_assign_rejected(odin3_ast_id rhs) {
    elab_wrap_item(assign_item(ident("w"), rhs));
    expect_only(7);
}

/* An ELABORATED store holding UNIT[MODULE m[item]]; item is made by the caller first. */
static void elab_wrap(odin3_ast_id item) {
    unit_of(NAMED(MODULE, "m", NIL, NIL, item));
}

static void elab_wrap_item(odin3_ast_id item) {
    elab_wrap(item);
}

/* --- valid stores -------------------------------------------------------------------------- */

static void test_valid_parsed_module(void) {
    build_parsed();
    assert_ok(ODIN3_CHECK_FULL);
    expect_text("I ast check: rule 3: 1 unreachable non-root node");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    assert_ok(ODIN3_CHECK_FULL);
    assert_ok(ODIN3_CHECK_FAST);
}

static void test_valid_elaborated_module(void) {
    build_elaborated();
    assert_ok(ODIN3_CHECK_FULL);
    assert_ok(ODIN3_CHECK_FAST);
    TEST_ASSERT_NULL(strstr(log_text, "unreachable"));
}

static void test_empty_store(void) {
    assert_ok(ODIN3_CHECK_FULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    assert_ok(ODIN3_CHECK_FULL);
}

static void test_misuse(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_check(NULL, ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_check(ast, (odin3_check_level)7));
    odin3_ast_test_target bad = {ODIN3_AST_TEST_FAULT_END, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, bad));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_ast_test_corrupt(NULL, (odin3_ast_test_target){ODIN3_AST_TEST_HEIGHT, 0, 0}));
    /* preconditions: no node 1, no symbol table, no comments */
    for (uint32_t fault = ODIN3_AST_TEST_KIND; fault < ODIN3_AST_TEST_FAULT_END; fault++) {
        if (fault == ODIN3_AST_TEST_HEIGHT || fault == ODIN3_AST_TEST_FORM) {
            continue;
        }
        odin3_ast_test_target target = {(odin3_ast_test_fault)fault, 1, 0};
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, target));
    }
}

/* --- rule 1 -------------------------------------------------------------------------------- */

static void test_rule1_unknown_kind(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_KIND, fix.number.v, 0);
    expect_rule(1);
    expect_text("rule 1: node ");
}

static void test_rule1_sub_out_of_range(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_SUB, fix.binary.v, 0);
    expect_only(1);
    corrupt(ODIN3_AST_TEST_SUB, fix.module.v, 0); /* a kind without sub-kinds */
    expect_only(1);
}

static void test_rule1_undefined_flag(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_FLAG, fix.case_item.v, 0);
    expect_only(1);
}

static void test_rule1_flag_field_values(void) {
    odin3_ast_spec port = {.kind = ODIN3_AST_PORT_DECL,
                           .sub = ODIN3_AST_PORT_DECL_INPUT,
                           .flags = (ODIN3_AST_DT_TIME + 1) << ODIN3_AST_F_PORT_DECL_DT_SHIFT};
    make(port, KIDS(NIL, declarator("a", NIL)), 2);
    expect_only(1);
    new_store(ODIN3_AST_FORM_PARSED);
    port.flags = (ODIN3_AST_NET_DECL_COUNT + 1) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    make(port, KIDS(NIL, declarator("a", NIL)), 2);
    expect_only(1);
    new_store(ODIN3_AST_FORM_PARSED);
    odin3_ast_spec charge = {.kind = ODIN3_AST_STRENGTH,
                             .sub = ODIN3_AST_STRENGTH_CHARGE,
                             .flags = ODIN3_AST_CHARGE_SMALL << ODIN3_AST_F_STRENGTH_S1_SHIFT};
    make(charge, NULL, 0);
    expect_only(1);
}

static void test_rule1_payload_on_plain_kind(void) {
    parsed fix = build_parsed();
    odin3_ast_id leaf = ident("q");
    corrupt(ODIN3_AST_TEST_PAYLOAD_PLAIN, leaf.v, 0);
    expect_only(1);
    (void)fix;
}

static void test_rule1_payload_index(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_PAYLOAD_INDEX, fix.number.v, 0);
    expect_only(1);
}

static void test_rule1_undecodable_loc(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_LOC, fix.binary.v, 0);
    expect_only(1);
    assert_ok(ODIN3_CHECK_FAST); /* FAST does not decode */
}

static void test_rule1_loc_after_end(void) {
    odin3_loc at = file_loc();
    odin3_ast_spec spec = {
        .kind = ODIN3_AST_IDENT, .loc = {at.v + 1}, .end = {at.v + 5}, .name = intern("v")};
    odin3_ast_id leaf = make(spec, NULL, 0);
    assert_ok(ODIN3_CHECK_FULL);
    corrupt(ODIN3_AST_TEST_LOC_ORDER, leaf.v, 0);
    expect_only(1);
    assert_ok(ODIN3_CHECK_FAST);
}

/* --- rule 2 -------------------------------------------------------------------------------- */

static void test_rule2_wrong_slot_class(void) {
    NODE(UNARY, ODIN3_AST_UNARY_NOT, node(ODIN3_AST_NULL_STMT, 0, NULL, 0));
    expect_only(2);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_ast_check(ast, ODIN3_CHECK_FAST));
}

static void test_rule2_child_not_below_parent(void) {
    parsed fix = build_parsed();
    odin3_ast_id later = ident("later");
    corrupt(ODIN3_AST_TEST_CHILD_ABOVE, fix.binary.v, later.v);
    expect_rule(2);
    expect_text("not below");
}

static void test_rule2_count_and_mandatory(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_COUNT, fix.binary.v, 0);
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_MANDATORY, fix.binary.v, 0);
    expect_only(2);
}

static void test_rule2_tail_zero_outside_e0(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_TAIL_ZERO, fix.case_stmt.v, 0);
    expect_rule(2); /* the dropped item also lowers the tree: rule 5 */
    expect_text("0 tail entry");
}

static void test_rule2_tail_zero_needs_system(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NOT_SYSTEM, fix.display.v, 0); /* a user task call: E*, not E*0 */
    expect_only(2);
}

static void test_rule2_case_item_default_with_expressions(void) {
    odin3_ast_spec item = {.kind = ODIN3_AST_CASE_ITEM, .flags = ODIN3_AST_F_CASE_ITEM_DEFAULT};
    make(item, KIDS(node(ODIN3_AST_NULL_STMT, 0, NULL, 0), dec("1")), 2);
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(CASE_ITEM, 0, node(ODIN3_AST_NULL_STMT, 0, NULL, 0)); /* neither DEFAULT nor expressions */
    expect_only(2);
}

static void test_rule2_event_control_tail_and_star(void) {
    odin3_ast_spec star = {.kind = ODIN3_AST_EVENT_CONTROL,
                           .flags = ODIN3_AST_F_EVENT_CONTROL_STAR};
    make(star, KIDS(NIL, NODE(EVENT_EXPR, 0, ident("a"))), 2);
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(EVENT_CONTROL, 0, NIL); /* no STAR and no events */
    expect_only(2);
}

static void test_rule2_case_body_class(void) {
    odin3_ast_id item = NODE(CASE_ITEM, 0, NAMED(GEN_BLOCK, "g", NIL), dec("1"));
    NODE(CASE, ODIN3_AST_CASE_CASE, ident("s"), item); /* an I body under a procedural CASE */
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    item = NODE(CASE_ITEM, 0, NODE(SEQ_BLOCK, 0, NIL), dec("1"));
    NODE(GEN_CASE, 0, ident("s"), item); /* an S body under GEN_CASE */
    expect_only(2);
}

static void test_rule2_hier_part(void) {
    odin3_ast_id part = NODE(SELECT, ODIN3_AST_SELECT_PART, ident("g"), dec("1"), dec("0"));
    NODE(HIER_NAME, 0, part, ident("w"));
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    odin3_ast_id deep = NODE(SELECT, ODIN3_AST_SELECT_BIT, bit_select("g", "1"), dec("0"), NIL);
    NODE(HIER_NAME, 0, deep, ident("w"));
    expect_only(2);
}

static void test_rule2_list_contents(void) {
    unit_of(NAMED(MODULE, "m", NIL, NODE(LIST, 0, ident("p"))));
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NAMED(INSTANTIATION, "s", NODE(LIST, 0, dec("1")), NAMED(INSTANCE, "u", NIL));
    expect_only(2);
}

static void test_rule2_gen_block_index_in_parsed_form(void) {
    make((odin3_ast_spec){.kind = ODIN3_AST_GEN_BLOCK, .name = intern("g")}, KIDS(dec("3")), 1);
    expect_only(2);
}

/* --- rule 3 -------------------------------------------------------------------------------- */

static void test_rule3_node_in_two_spans(void) {
    odin3_ast_id shared = ident("s");
    NODE(UNARY, ODIN3_AST_UNARY_NOT, shared);
    NODE(UNARY, ODIN3_AST_UNARY_MINUS, shared);
    expect_only(3);
    assert_ok(ODIN3_CHECK_FAST);
    new_store(ODIN3_AST_FORM_PARSED);
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_CHILD_SHARED, fix.module.v, fix.binary.v);
    expect_rule(3);
}

static void test_rule3_attr_in_child_span(void) {
    NODE(LIST, 0, NAMED(ATTR, "keep", NIL));
    expect_only(3);
}

static void test_rule3_attr_attached_twice(void) {
    odin3_ast_id attr = NAMED(ATTR, "keep", NIL);
    odin3_ast_id first = ident("a");
    odin3_ast_id second = ident("b");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, first, &attr, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, second, &attr, 1));
    expect_only(3);
}

static void test_rule3_unit_in_span(void) {
    odin3_ast_id unit = named(ODIN3_AST_UNIT, "f.v", NULL, 0);
    NODE(LIST, 0, unit);
    expect_only(3);
}

/* --- rule 4 -------------------------------------------------------------------------------- */

static void test_rule4_instance_without_name(void) {
    odin3_ast_id inst = NODE(INSTANCE, 0, NIL);
    NAMED(INSTANTIATION, "sub", NIL, inst);
    expect_only(4);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(GATE_DECL, ODIN3_AST_GATE_DECL_AND, NIL, NIL, NODE(INSTANCE, 0, NIL));
    assert_ok(ODIN3_CHECK_FULL); /* a gate instance may be unnamed */
}

static void test_rule4_name_rules(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NAME_MISSING, fix.module.v, 0);
    expect_only(4);
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NAME_BAD, fix.instance.v, 0);
    expect_only(4);
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NAME_EXTRA, fix.binary.v, intern("x"));
    expect_only(4);
}

/* --- rule 5 -------------------------------------------------------------------------------- */

static void test_rule5_wrong_height(void) {
    build_parsed();
    corrupt(ODIN3_AST_TEST_HEIGHT, 0, 0);
    expect_only(5);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_ast_check(ast, ODIN3_CHECK_FAST));
}

/* --- rule 6 -------------------------------------------------------------------------------- */

static void test_rule6_unsorted_comments(void) {
    build_parsed();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    corrupt(ODIN3_AST_TEST_COMMENT_ORDER, 0, 0);
    expect_only(6);
    assert_ok(ODIN3_CHECK_FAST);
}

static void test_rule6_bad_attribute_span(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_ATTR_SPAN, fix.case_stmt.v, 0);
    expect_rule(6);
}

static void test_rule6_number_has_xz(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NUMBER_XZ, fix.number.v, 0);
    expect_only(6);
    assert_ok(ODIN3_CHECK_FAST);
}

/* --- rule 7 -------------------------------------------------------------------------------- */

static void test_rule7_defparam(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id target = NODE(HIER_NAME, 0, ident("u"), ident("W"));
    elab_wrap(NODE(DEFPARAM, 0, target, dec("4")));
    expect_only(7);
}

static void test_rule7_range_bound_ident(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id range = NODE(RANGE, 0, ident("W"), dec("0"));
    elab_wrap(NODE(NET_DECL, ODIN3_AST_NET_DECL_WIRE, NIL, range, NIL, declarator("w", NIL)));
    expect_only(7);
}

static void test_rule7_parameter_decl(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(PARAM_DECL, ODIN3_AST_PARAM_DECL_PARAMETER, NIL, declarator("W", dec("8"))));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(PARAM_DECL, ODIN3_AST_PARAM_DECL_LOCALPARAM, NIL, declarator("W", ident("V"))));
    expect_only(7);
}

static void test_rule7_hierarchical_call(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_spec call = {
        .kind = ODIN3_AST_CALL, .flags = ODIN3_AST_F_CALL_HIER, .name = intern("u.f")};
    odin3_ast_id value = make(call, NULL, 0);
    elab_wrap(NODE(CONT_ASSIGN, 0, NIL, NIL, NODE(NET_ASSIGN, 0, ident("w"), value)));
    expect_only(7);
}

static void test_rule7_shapes(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(GEN_BLOCK, 0, NIL)); /* an unnamed generate block */
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, node(ODIN3_AST_NULL_STMT, 0, NULL, 0)));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_spec in = {.kind = ODIN3_AST_PORT_DECL, .sub = ODIN3_AST_PORT_DECL_INPUT};
    odin3_ast_id two = make(in, KIDS(NIL, declarator("a", NIL), declarator("b", NIL)), 3);
    unit_of(NAMED(MODULE, "m", NIL, NODE(LIST, 0, two)));
    expect_only(7);
}

static void test_rule7_form_flip(void) {
    build_parsed();
    corrupt(ODIN3_AST_TEST_FORM, 0, 0);
    expect_only(7);
    assert_ok(ODIN3_CHECK_FAST);
}

/* --- rule 8 -------------------------------------------------------------------------------- */

static void test_rule8_symbol_scope(void) {
    elaborated elab = build_elaborated();
    corrupt(ODIN3_AST_TEST_SYMBOL_SCOPE, elab.sym_w.v, 0);
    expect_only(8);
    expect_text("rule 8: symbol ");
    assert_ok(ODIN3_CHECK_FAST);
}

static void test_rule8_map_disagrees(void) {
    elaborated elab = build_elaborated();
    corrupt(ODIN3_AST_TEST_SYMBOL_NAME, elab.sym_w.v, elab.name_y);
    expect_only(8);
}

/* --- fix round 1: one negative per remaining check line ------------------------------------ */

static void test_rule1_payload_children_and_end(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_PAYLOAD_CHILDREN, fix.number.v, 0);
    expect_only(1);
    expect_text("children on a payload kind");
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_END, fix.binary.v, 0);
    expect_only(1);
    expect_text("end does not decode");
    assert_ok(ODIN3_CHECK_FAST);
}

static void test_rule1_dt_fields_and_drive(void) {
    odin3_ast_spec param = {.kind = ODIN3_AST_PARAM_DECL,
                            .sub = ODIN3_AST_PARAM_DECL_PARAMETER,
                            .flags = BAD_DT << ODIN3_AST_F_PARAM_DECL_DT_SHIFT};
    make(param, KIDS(NIL, declarator("W", dec("1"))), 2);
    expect_only(1);
    new_store(ODIN3_AST_FORM_PARSED);
    odin3_ast_spec func = {.kind = ODIN3_AST_FUNCTION_DECL,
                           .flags = BAD_DT << ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT,
                           .name = intern("f")};
    make(func, KIDS(NIL, empty_list(), empty_list(), null_stmt()), 4);
    expect_only(1);
    uint16_t drives[2] = {BAD_DRIVE << ODIN3_AST_F_STRENGTH_S0_SHIFT,
                          BAD_DRIVE << ODIN3_AST_F_STRENGTH_S1_SHIFT};
    for (uint32_t i = 0; i < 2; i++) {
        new_store(ODIN3_AST_FORM_PARSED);
        make((odin3_ast_spec){.kind = ODIN3_AST_STRENGTH,
                              .sub = ODIN3_AST_STRENGTH_DRIVE,
                              .flags = drives[i]},
             NULL, 0);
        expect_only(1);
    }
}

static void test_rule2_tail_bounds(void) {
    NODE(UNARY, ODIN3_AST_UNARY_NOT, NODE(UNARY, ODIN3_AST_UNARY_NOT, ident("t"))); /* tallest */
    odin3_ast_id cat = NODE(CONCAT, 0, ident("a"));
    corrupt(ODIN3_AST_TEST_COUNT, cat.v, 0); /* CONCAT needs one entry */
    expect_only(2);
    expect_text("tail count out of bounds");
    odin3_ast_id two = NODE(CONCAT, 0, ident("a"), ident("b"));
    odin3_ast_test_target above_min = {ODIN3_AST_TEST_COUNT, two.v, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, above_min));
}

static void test_rule2_function_task_block_lists(void) {
    odin3_ast_id var = NODE(VAR_DECL, ODIN3_AST_VAR_DECL_REG, NIL, declarator("v", NIL));
    NAMED(FUNCTION_DECL, "f", NIL, NODE(LIST, 0, var), empty_list(), null_stmt());
    expect_only(2); /* a VAR_DECL among the ports */
    new_store(ODIN3_AST_FORM_PARSED);
    odin3_ast_id stmt = NODE(NET_ASSIGN, 0, ident("a"), ident("b"));
    NAMED(FUNCTION_DECL, "f", NIL, empty_list(), NODE(LIST, 0, stmt), null_stmt());
    expect_only(2); /* not a declaration among the locals */
    new_store(ODIN3_AST_FORM_PARSED);
    NAMED(TASK_DECL, "t", NODE(LIST, 0, ident("p")), empty_list(), null_stmt());
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NAMED(TASK_DECL, "t", empty_list(), NODE(LIST, 0, ident("p")), null_stmt());
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(SEQ_BLOCK, 0, NODE(LIST, 0, NODE(NET_ASSIGN, 0, ident("a"), ident("b"))));
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(PAR_BLOCK, 0, NODE(LIST, 0, ident("a")));
    expect_only(2);
}

static void test_rule2_span_outside_table(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_SPAN_OUT, fix.binary.v, 0);
    expect_rule(2);
    expect_text("span outside the child table");
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_ast_check(ast, ODIN3_CHECK_FAST));
}

static void test_rule2_select_third_slot(void) {
    NODE(SELECT, ODIN3_AST_SELECT_BIT, ident("a"), dec("1"), dec("0"));
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    NODE(SELECT, ODIN3_AST_SELECT_PART_PLUS, ident("a"), dec("1"), NIL);
    expect_only(2);
}

static void test_hook_preconditions(void) {
    parsed fix = build_parsed();
    odin3_ast_id first = odin3_ast_child(ast, fix.module, 0);
    odin3_ast_test_target same = {ODIN3_AST_TEST_CHILD_SHARED, fix.module.v, first.v};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, same));
    uint32_t attr = odin3_ast_attrs(ast, fix.case_stmt).ids[0].v; /* an ATTR is no corruption */
    odin3_ast_test_target not_attr = {ODIN3_AST_TEST_ATTR_ENTRY, fix.case_stmt.v, attr};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, not_attr));
    odin3_ast_test_target taken = {ODIN3_AST_TEST_ATTR_KEY, fix.case_stmt.v, fix.binary.v};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, taken));
    odin3_ast_test_target system = {ODIN3_AST_TEST_NOT_SYSTEM, fix.binary.v, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, system));
    assert_ok(ODIN3_CHECK_FULL); /* nothing changed */
}

static void test_rule5_depth_cap(void) {
    odin3_ast_id top = ident("x");
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        top = NODE(UNARY, ODIN3_AST_UNARY_MINUS, top);
    }
    assert_ok(ODIN3_CHECK_FAST);
    odin3_ast_id over = NODE(UNARY, ODIN3_AST_UNARY_MINUS, ident("y"));
    corrupt(ODIN3_AST_TEST_CHILD_SHARED, over.v, top.v); /* height MAX_DEPTH + 1 */
    expect_only(5);
    expect_text("height above ODIN3_AST_MAX_DEPTH");
}

static void test_rule6_number_payloads(void) {
    odin3_ast_test_fault faults[] = {ODIN3_AST_TEST_NUMBER_BIT, ODIN3_AST_TEST_NUMBER_EMPTY,
                                     ODIN3_AST_TEST_NUMBER_WIDE, ODIN3_AST_TEST_NUMBER_QUESTION};
    const char *texts[] = {"bit not 0 1 x z", "without bits", "wider than the cap",
                           "has_question without a z bit"};
    for (uint32_t i = 0; i < 4; i++) {
        new_store(ODIN3_AST_FORM_PARSED);
        parsed fix = build_parsed();
        corrupt(faults[i], fix.number.v, 0);
        expect_only(6);
        expect_text(texts[i]);
        assert_ok(ODIN3_CHECK_FAST);
    }
    odin3_ast_id with_z = number("4", "'b", "10z1");
    odin3_ast_test_target has_z = {ODIN3_AST_TEST_NUMBER_QUESTION, with_z.v, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_test_corrupt(ast, has_z));
}

static void test_rule6_attribute_entries(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_ATTR_ENTRY, fix.case_stmt.v, fix.binary.v);
    expect_only(6);
    expect_text("attribute entry is not an ATTR");
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_ATTR_EMPTY, fix.case_stmt.v, 0);
    expect_only(6);
    expect_text("attribute span outside the child table");
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_ATTR_KEY, fix.case_stmt.v, odin3_ast_node_end(ast).v);
    expect_only(6);
    expect_text("attributes on a missing node or an ATTR");
}

static void test_rule7_constants(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_assign_rejected(NODE(REPLICATE, 0, ident("n"), NODE(CONCAT, 0, ident("a"))));
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_assign_rejected(NODE(SELECT, ODIN3_AST_SELECT_PART, ident("a"), ident("m"), dec("0")));
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_assign_rejected(NODE(SELECT, ODIN3_AST_SELECT_PART, ident("a"), dec("3"), ident("l")));
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_assign_rejected(
        NODE(SELECT, ODIN3_AST_SELECT_PART_PLUS, ident("a"), ident("i"), ident("w")));
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id varying = NODE(SELECT, ODIN3_AST_SELECT_BIT, ident("g"), ident("i"), NIL);
    elab_assign_rejected(NODE(HIER_NAME, 0, varying, ident("w")));
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(INITIAL, 0, NODE(REPEAT, 0, ident("n"), null_stmt())));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id params = NODE(LIST, 0, NAMED(CONNECTION, "W", ident("V")));
    elab_wrap(NAMED(INSTANTIATION, "leaf", params, NAMED(INSTANCE, "u0", NIL)));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED); /* a part_plus start and a bit index may vary */
    odin3_ast_id plus = NODE(SELECT, ODIN3_AST_SELECT_PART_PLUS, ident("a"), ident("i"), dec("2"));
    odin3_ast_id bit = NODE(SELECT, ODIN3_AST_SELECT_BIT, ident("a"), ident("i"), NIL);
    elab_wrap(assign_item(ident("w"), NODE(CONCAT, 0, plus, bit)));
    assert_ok(ODIN3_CHECK_FULL);
}

static void test_rule7_absent_forms(void) {
    for (uint32_t sub = ODIN3_AST_VAR_DECL_TIME; sub < ODIN3_AST_VAR_DECL_COUNT; sub++) {
        new_store(ODIN3_AST_FORM_ELABORATED);
        elab_wrap(NODE(VAR_DECL, (uint8_t)sub, NIL, declarator("t", NIL)));
        expect_only(7);
    }
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(PARAM_DECL, ODIN3_AST_PARAM_DECL_SPECPARAM, NIL, declarator("S", dec("1"))));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_spec call = {
        .kind = ODIN3_AST_TASK_CALL, .flags = ODIN3_AST_F_TASK_CALL_HIER, .name = intern("u.t")};
    elab_wrap(NODE(INITIAL, 0, make(call, NULL, 0)));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    unit_of(NAMED(MODULE, "m", NIL, NODE(LIST, 0, NAMED(PORT_REF, "a", NIL))));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_spec strong = {.kind = ODIN3_AST_STRENGTH, .sub = ODIN3_AST_STRENGTH_DRIVE};
    elab_wrap(NODE(NET_DECL, ODIN3_AST_NET_DECL_WIRE, make(strong, NULL, 0), NIL, NIL,
                   declarator("w", NIL)));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(NET_DECL, ODIN3_AST_NET_DECL_WIRE, NIL, NIL, NODE(DELAY, 0, dec("1")),
                   declarator("w", NIL)));
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(EVENT_DECL, 0, declarator("e", NIL)));
    expect_only(7);
}

static void test_rule7_hierarchy_shapes(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, NODE(TIMING_STMT, 0, posedge("clk"), NIL)));
    expect_only(7); /* a TIMING_STMT without its statement */
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_spec in = {.kind = ODIN3_AST_PORT_DECL, .sub = ODIN3_AST_PORT_DECL_INPUT};
    elab_wrap(make(in, KIDS(NIL, declarator("a", NIL)), 2)); /* a non-ANSI port */
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id port = make(in, KIDS(NIL, declarator("a", NIL)), 2);
    elab_wrap(make((odin3_ast_spec){.kind = ODIN3_AST_GEN_BLOCK, .name = intern("g")},
                   KIDS(NIL, port), 2));
    expect_only(7);
}

static void test_rule7_unit_count(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    NAMED(MODULE, "m", NIL, NIL); /* no UNIT at all */
    expect_only(7);
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(net("a"));
    unit_of(NAMED(MODULE, "n", NIL, NIL));
    expect_only(7);
}

static void test_rule8_log_cap(void) {
    elaborated elab = build_elaborated();
    odin3_symtab *st = odin3_ast_symtab(ast);
    odin3_scope_id scope = odin3_symbol_get(st, elab.sym_w)->scope;
    char name[NEEDLE];
    for (uint32_t i = 0; i < MANY_SYMBOLS; i++) {
        (void)snprintf(name, sizeof name, "s%u", (unsigned)i);
        odin3_symbol_spec spec = {.name = intern(name), .scope = scope, .kind = ODIN3_SYM_NET};
        odin3_sym_id sym = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_symbol_new(st, &spec, &sym));
        corrupt(ODIN3_AST_TEST_SYMBOL_NAME, sym.v, elab.name_y);
    }
    expect_only(8);
    expect_text("more errors not shown");
}

/* A TEXT-payload node of kind (an opaque construct). */
static odin3_ast_id text_node(uint8_t kind, const char *name) {
    odin3_ast_spec spec = {.kind = kind, .name = name != NULL ? intern(name) : 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_text_new(ast, odin3_bytes_cstr("opaque"), &spec.payload));
    return make(spec, NULL, 0);
}

static odin3_ast_id gen_body(void) {
    return NAMED(GEN_BLOCK, "b", NIL);
}

/* A minimal well-formed module item of an absent kind (index which), for rule 7. */
static odin3_ast_id absent_item(uint32_t which) {
    odin3_ast_id init = NODE(BLOCKING_ASSIGN, 0, ident("i"), NIL, dec("0"));
    switch (which) {
    case 0:
        return node(ODIN3_AST_DIRECTIVE, ODIN3_AST_DIRECTIVE_RESETALL, NULL, 0);
    case 1:
        return text_node(ODIN3_AST_SPECIFY_BLOCK, NULL);
    case 2:
        return node(ODIN3_AST_GENERATE, 0, NULL, 0);
    case 3:
        return NODE(GEN_FOR, 0, init, ident("c"),
                    NODE(BLOCKING_ASSIGN, 0, ident("i"), NIL, dec("1")), gen_body());
    case 4:
        return NODE(GEN_IF, 0, ident("c"), gen_body(), NIL);
    case 5:
        return NODE(GEN_CASE, 0, ident("c"), NODE(CASE_ITEM, 0, gen_body(), dec("1")));
    case 6:
        return NODE(GENVAR_DECL, 0, declarator("i", NIL));
    case 7:
        return assign_item(ident("w"), NODE(MINTYPMAX, 0, dec("1"), dec("2"), dec("3")));
    case 8:
        return NODE(INITIAL, 0,
                    NODE(PROC_CONT_ASSIGN, ODIN3_AST_PROC_CONT_ASSIGN_FORCE, ident("a"), dec("1")));
    case 9:
        return NODE(INITIAL, 0, NODE(WAIT, 0, ident("c"), NIL));
    case 10:
        return NODE(INITIAL, 0, NODE(DISABLE, 0, NODE(HIER_NAME, 0, ident("b"))));
    case 11:
        return NODE(INITIAL, 0, NODE(EVENT_TRIGGER, 0, NODE(HIER_NAME, 0, ident("e"))));
    case 12:
        return NODE(INITIAL, 0, NODE(FOREVER, 0, null_stmt()));
    default:
        return NODE(INITIAL, 0, NODE(PAR_BLOCK, 0, NIL));
    }
}

enum { ABSENT_ITEMS = 14 };

static void test_rule7_every_absent_kind(void) {
    for (uint32_t k = 0; k < ABSENT_ITEMS; k++) {
        new_store(ODIN3_AST_FORM_ELABORATED);
        elab_wrap(absent_item(k));
        expect_only(7);
        expect_text("kind absent from the elaborated form");
    }
    uint8_t unit_kinds[2] = {ODIN3_AST_UDP_DECL, ODIN3_AST_CONFIG_DECL};
    for (uint32_t i = 0; i < 2; i++) {
        new_store(ODIN3_AST_FORM_ELABORATED);
        odin3_ast_id module = NAMED(MODULE, "m", NIL, NIL);
        unit_of_two(module, text_node(unit_kinds[i], "u"));
        expect_only(7);
    }
}

static void test_rule7_more_constants_and_always(void) {
    new_store(ODIN3_AST_FORM_ELABORATED);
    elab_wrap(NODE(NET_DECL, ODIN3_AST_NET_DECL_WIRE, NIL, NODE(RANGE, 0, dec("7"), ident("L")),
                   NIL, declarator("w", NIL)));
    expect_only(7); /* a RANGE lsb */
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id not_timing = NODE(IF, 0, posedge("clk"), null_stmt(), NIL);
    elab_wrap(NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, not_timing));
    expect_rule(7); /* an EVENT_CONTROL where IF wants an E is rule 2 too */
    expect_text("always is not over");
    new_store(ODIN3_AST_FORM_ELABORATED);
    odin3_ast_id delay = NODE(DELAY, 0, dec("1"));
    elab_wrap(NODE(ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, NODE(TIMING_STMT, 0, delay, null_stmt())));
    expect_rule(7); /* DELAY is absent too */
    expect_text("always is not over");
}

static void test_rule2_hier_number_and_param_list(void) {
    NODE(HIER_NAME, 0, dec("1"), ident("w"));
    expect_only(2);
    new_store(ODIN3_AST_FORM_PARSED);
    unit_of(NAMED(MODULE, "m", NODE(LIST, 0, net("n")), NIL));
    expect_only(2);
}

static void test_rule4_payload_and_corrupt_span_names(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NAME_EXTRA, fix.number.v, intern("x"));
    expect_only(4);
    new_store(ODIN3_AST_FORM_PARSED);
    odin3_ast_id str = named(ODIN3_AST_STRING, "s", NULL, 0);
    corrupt(ODIN3_AST_TEST_NAME_MISSING, str.v, 0);
    expect_only(4);
    new_store(ODIN3_AST_FORM_PARSED);
    fix = build_parsed();
    corrupt(ODIN3_AST_TEST_SPAN_OUT, fix.module.v, 0);
    corrupt(ODIN3_AST_TEST_NAME_MISSING, fix.module.v, 0);
    expect_rule(4);
}

/* --- FAST, the log cap, OOM ---------------------------------------------------------------- */

static void test_fast_skips_full_rules(void) {
    parsed fix = build_parsed();
    corrupt(ODIN3_AST_TEST_NUMBER_XZ, fix.number.v, 0);
    corrupt(ODIN3_AST_TEST_LOC, fix.binary.v, 0);
    corrupt(ODIN3_AST_TEST_FORM, 0, 0);
    odin3_ast_id shared = ident("s");
    NODE(UNARY, ODIN3_AST_UNARY_NOT, shared);
    NODE(UNARY, ODIN3_AST_UNARY_NOT, shared);
    assert_ok(ODIN3_CHECK_FAST);
}

static void test_log_cap(void) {
    for (uint32_t i = 0; i < MANY_ERRORS; i++) {
        NODE(UNARY, ODIN3_AST_UNARY_NOT, node(ODIN3_AST_NULL_STMT, 0, NULL, 0));
    }
    expect_only(2);
    expect_text("E ast check: rule 2: 20 more errors not shown");
}

/* FULL with the fail-th allocation failing; true once it succeeds. */
static bool check_with_failure(long fail) {
    reset_log();
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_ast_check(ast, ODIN3_CHECK_FULL);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(strstr(log_text, "E ast check"));
    }
    return st == ODIN3_OK;
}

static void test_oom(void) {
    build_parsed();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    long fail = 0;
    while (fail < OOM_LIMIT && !check_with_failure(fail)) {
        fail++;
    }
    TEST_ASSERT_TRUE(fail > 0 && fail < OOM_LIMIT);
}

static void run_first_half(void) {
    RUN_TEST(test_valid_parsed_module);
    RUN_TEST(test_valid_elaborated_module);
    RUN_TEST(test_empty_store);
    RUN_TEST(test_misuse);
    RUN_TEST(test_rule1_unknown_kind);
    RUN_TEST(test_rule1_sub_out_of_range);
    RUN_TEST(test_rule1_undefined_flag);
    RUN_TEST(test_rule1_flag_field_values);
    RUN_TEST(test_rule1_payload_on_plain_kind);
    RUN_TEST(test_rule1_payload_index);
    RUN_TEST(test_rule1_undecodable_loc);
    RUN_TEST(test_rule1_loc_after_end);
    RUN_TEST(test_rule2_wrong_slot_class);
    RUN_TEST(test_rule2_child_not_below_parent);
    RUN_TEST(test_rule2_count_and_mandatory);
    RUN_TEST(test_rule2_tail_zero_outside_e0);
    RUN_TEST(test_rule2_tail_zero_needs_system);
    RUN_TEST(test_rule2_case_item_default_with_expressions);
    RUN_TEST(test_rule2_event_control_tail_and_star);
    RUN_TEST(test_rule2_case_body_class);
    RUN_TEST(test_rule2_hier_part);
    RUN_TEST(test_rule2_list_contents);
    RUN_TEST(test_rule2_gen_block_index_in_parsed_form);
    RUN_TEST(test_rule3_node_in_two_spans);
    RUN_TEST(test_rule3_attr_in_child_span);
    RUN_TEST(test_rule3_attr_attached_twice);
    RUN_TEST(test_rule3_unit_in_span);
    RUN_TEST(test_rule4_instance_without_name);
    RUN_TEST(test_rule4_name_rules);
    RUN_TEST(test_rule5_wrong_height);
    RUN_TEST(test_rule6_unsorted_comments);
}

static void run_second_half(void) {
    RUN_TEST(test_rule6_bad_attribute_span);
    RUN_TEST(test_rule6_number_has_xz);
    RUN_TEST(test_rule7_defparam);
    RUN_TEST(test_rule7_range_bound_ident);
    RUN_TEST(test_rule7_parameter_decl);
    RUN_TEST(test_rule7_hierarchical_call);
    RUN_TEST(test_rule7_shapes);
    RUN_TEST(test_rule7_form_flip);
    RUN_TEST(test_rule8_symbol_scope);
    RUN_TEST(test_rule8_map_disagrees);
    RUN_TEST(test_rule1_payload_children_and_end);
    RUN_TEST(test_rule1_dt_fields_and_drive);
    RUN_TEST(test_rule2_tail_bounds);
    RUN_TEST(test_rule2_function_task_block_lists);
    RUN_TEST(test_rule2_span_outside_table);
    RUN_TEST(test_rule2_select_third_slot);
    RUN_TEST(test_hook_preconditions);
    RUN_TEST(test_rule5_depth_cap);
    RUN_TEST(test_rule6_number_payloads);
    RUN_TEST(test_rule6_attribute_entries);
    RUN_TEST(test_rule7_constants);
    RUN_TEST(test_rule7_absent_forms);
    RUN_TEST(test_rule7_hierarchy_shapes);
    RUN_TEST(test_rule7_unit_count);
    RUN_TEST(test_rule8_log_cap);
    RUN_TEST(test_rule7_every_absent_kind);
    RUN_TEST(test_rule7_more_constants_and_always);
    RUN_TEST(test_rule2_hier_number_and_param_list);
    RUN_TEST(test_rule4_payload_and_corrupt_span_names);
    RUN_TEST(test_fast_skips_full_rules);
    RUN_TEST(test_log_cap);
    RUN_TEST(test_oom);
}

int main(void) {
    UNITY_BEGIN();
    run_first_half();
    run_second_half();
    return UNITY_END();
}
