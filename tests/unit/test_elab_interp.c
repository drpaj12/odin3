/*
 * test_elab_interp.c — 2D constant evaluation (spec §4.1, §4.6, §4.7; EL-12, EL-17, EL-18): the
 * evaluator, the constant-function interpreter and the system functions, on sources parsed by
 * 2C (vt_parse) with a test host that binds the module's parameters (evaluated lazily, through
 * the evaluator itself), signals, memories, functions and tasks. Every located diagnostic is
 * asserted as a whole line through a capturing log sink.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "verilog_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    MSG = 512,
    MAX_PARAMS = 2048, /* the 2000-link parameter chain */
    MAX_WRITES = 16,
    OOM_LIMIT = 100000,
    DEEP = 400, /* recursion through a replication count: two depth units per level */
};

static vt_result res;
static odin3_ceval *ev;
static odin3_arena *values; /* results and the host's parameter values */
static odin3_strbuf logged; /* every line logged by the evaluator, each followed by '\n' */
static char last[MSG];      /* the last line */
static int errors_logged;
static int warnings_logged;
static const char *source; /* the text parsed (for column lookups) */

/* --- the capturing sink -------------------------------------------------------------------- */

static void capture(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
    warnings_logged += level == ODIN3_LOG_WARN;
    (void)snprintf(last, sizeof last, "%s", msg);
    (void)odin3_strbuf_appendf(&logged, "%s\n", msg);
}

/* --- walking the parsed module ------------------------------------------------------------- */

static odin3_ast_id module_node(void) {
    odin3_ast_span units = odin3_ast_children(res.ast, res.unit);
    for (uint32_t k = 0; k < units.n; k++) {
        if (odin3_ast_kind_of(res.ast, units.ids[k]) == ODIN3_AST_MODULE) {
            return units.ids[k];
        }
    }
    TEST_FAIL_MESSAGE("no module");
    return (odin3_ast_id){0};
}

/* The first index of a declaration's DECLARATOR tail. */
static uint32_t declarators_from(odin3_ast_id decl) {
    switch (odin3_ast_kind_of(res.ast, decl)) {
    case ODIN3_AST_NET_DECL:
        return 3;
    case ODIN3_AST_GENVAR_DECL:
        return 0;
    default:
        return 1; /* PORT_DECL, VAR_DECL, PARAM_DECL: one range slot */
    }
}

typedef struct found {
    odin3_ast_id decl;       /* the declaration node (or FUNCTION_DECL/TASK_DECL) */
    odin3_ast_id declarator; /* 0 for a function or task */
} found;

static bool match_in(odin3_ast_id decl, const char *name, found *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(res.ast, decl);
    if (kind == ODIN3_AST_FUNCTION_DECL || kind == ODIN3_AST_TASK_DECL) {
        if (strcmp(odin3_ast_name_str(res.ast, decl), name) == 0) {
            *out = (found){decl, {0}};
            return true;
        }
        return false;
    }
    if (kind != ODIN3_AST_PORT_DECL && kind != ODIN3_AST_NET_DECL && kind != ODIN3_AST_VAR_DECL &&
        kind != ODIN3_AST_PARAM_DECL) {
        return false;
    }
    for (uint32_t k = declarators_from(decl); k < odin3_ast_nchild(res.ast, decl); k++) {
        odin3_ast_id dcl = odin3_ast_child(res.ast, decl, k);
        if (strcmp(odin3_ast_name_str(res.ast, dcl), name) == 0) {
            *out = (found){decl, dcl};
            return true;
        }
    }
    return false;
}

/* The module item, header port or header parameter declaring name. */
static bool find_name(const char *name, found *out) {
    odin3_ast_id mod = module_node();
    for (uint32_t slot = 0; slot < 2; slot++) {
        odin3_ast_id list = odin3_ast_child(res.ast, mod, slot);
        for (uint32_t k = 0; list.v != 0 && k < odin3_ast_nchild(res.ast, list); k++) {
            if (match_in(odin3_ast_child(res.ast, list, k), name, out)) {
                return true;
            }
        }
    }
    for (uint32_t k = 2; k < odin3_ast_nchild(res.ast, mod); k++) {
        if (match_in(odin3_ast_child(res.ast, mod, k), name, out)) {
            return true;
        }
    }
    return false;
}

/* A NUMBER node's value as an integer (test sources use literal bounds). */
static int64_t number(odin3_ast_id node) {
    odin3_ast_literal lit = {{0}, false, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(res.ast, node, values, &lit));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(values, &lit, &val));
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &out));
    return out;
}

/* --- the test host ------------------------------------------------------------------------- */

typedef struct param_entry {
    uint32_t decl; /* DECLARATOR */
    odin3_csym sym;
} param_entry;

static param_entry params[MAX_PARAMS];
static uint32_t n_params;

/* A host bug to inject into lookup's answers (final review, finding 4). */
enum { CORRUPT_NONE, CORRUPT_FLAGS, CORRUPT_RANGE };
static int corrupt;

/* The declared type and range of a declaration (literal bounds; integer: [31:0] signed). */
static void declared(odin3_ast_id decl, odin3_csym *sym) {
    odin3_ast_kind kind = odin3_ast_kind_of(res.ast, decl);
    bool integer =
        kind == ODIN3_AST_VAR_DECL && odin3_ast_sub(res.ast, decl) == ODIN3_AST_VAR_DECL_INTEGER;
    uint32_t range_slot = kind == ODIN3_AST_NET_DECL ? 1 : 0;
    odin3_ast_id range = kind == ODIN3_AST_GENVAR_DECL ? (odin3_ast_id){0}
                                                       : odin3_ast_child(res.ast, decl, range_slot);
    sym->msb = integer ? 31 : 0;
    sym->lsb = 0;
    if (range.v != 0) {
        sym->msb = number(odin3_ast_child(res.ast, range, 0));
        sym->lsb = number(odin3_ast_child(res.ast, range, 1));
    }
    int64_t span = sym->msb >= sym->lsb ? sym->msb - sym->lsb : sym->lsb - sym->msb;
    sym->type.width = (uint32_t)span + 1;
    sym->type.is_signed = integer || (odin3_ast_flags(res.ast, decl) & 1U) != 0;
}

static odin3_status param_value(found where, odin3_csym *out) {
    for (uint32_t k = 0; k < n_params; k++) {
        if (params[k].decl == where.declarator.v) {
            *out = params[k].sym;
            return ODIN3_OK;
        }
    }
    char what[MSG];
    (void)snprintf(what, sizeof what, "parameter '%s'",
                   odin3_ast_name_str(res.ast, where.declarator));
    odin3_ceval_req req = {odin3_ast_child(res.ast, where.declarator, 0), {0, false, false}, what};
    odin3_cval val = {0};
    odin3_status st = odin3_ceval_expr(ev, &req, values, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_csym sym = {.kind = ODIN3_CSYM_CONST, .value = val, .decl = where.declarator};
    sym.type = (odin3_etype){val.width, (val.flags & ODIN3_CVAL_SIGNED) != 0,
                             (val.flags & ODIN3_CVAL_UNSIZED) != 0};
    sym.msb = (int64_t)val.width - 1;
    sym.lsb = 0;
    if (odin3_ast_child(res.ast, where.decl, 0).v != 0) { /* a ranged localparam (same width) */
        odin3_csym range = {0};
        declared(where.decl, &range);
        TEST_ASSERT_EQUAL_UINT32(range.type.width, val.width);
        sym.msb = range.msb;
        sym.lsb = range.lsb;
    }
    sym.name = odin3_ast_name(res.ast, where.declarator);
    TEST_ASSERT_TRUE(n_params < MAX_PARAMS);
    params[n_params++] = (param_entry){where.declarator.v, sym};
    *out = sym;
    return ODIN3_OK;
}

static odin3_status lookup_name(odin3_ast_id ref, const char *name, odin3_csym *out) {
    found where = {{0}, {0}};
    if (!find_name(name, &where)) {
        return ODIN3_OK; /* NONE */
    }
    odin3_ast_kind kind = odin3_ast_kind_of(res.ast, where.decl);
    if (kind == ODIN3_AST_PARAM_DECL) {
        return param_value(where, out);
    }
    if (kind == ODIN3_AST_FUNCTION_DECL || kind == ODIN3_AST_TASK_DECL) {
        out->kind = kind == ODIN3_AST_FUNCTION_DECL ? ODIN3_CSYM_FUNCTION : ODIN3_CSYM_TASK;
        out->decl = where.decl;
        out->name = odin3_ast_name(res.ast, where.decl);
        return ODIN3_OK;
    }
    if (odin3_ast_nchild(res.ast, where.declarator) > 1) { /* a memory */
        odin3_diag(res.design, ODIN3_LOG_ERROR, odin3_ast_loc(res.ast, ref),
                   "'%s' is a memory; select a word", name);
        return ODIN3_ERR_PARSE;
    }
    out->kind = ODIN3_CSYM_SIGNAL;
    declared(where.decl, out);
    out->decl = where.declarator;
    out->name = odin3_ast_name(res.ast, where.declarator);
    return ODIN3_OK;
}

/* A SELECT: a memory word when its chain is as deep as the memory's unpacked dimensions. */
static odin3_status lookup_word(odin3_ast_id sel, odin3_csym *out) {
    uint32_t depth = 0;
    odin3_ast_id obj = sel;
    while (odin3_ast_kind_of(res.ast, obj) == ODIN3_AST_SELECT) {
        obj = odin3_ast_child(res.ast, obj, 0);
        depth++;
    }
    found where = {{0}, {0}};
    if (odin3_ast_kind_of(res.ast, obj) != ODIN3_AST_IDENT ||
        !find_name(odin3_ast_name_str(res.ast, obj), &where) || where.declarator.v == 0 ||
        odin3_ast_nchild(res.ast, where.declarator) - 1 != depth) {
        return ODIN3_OK;
    }
    out->kind = ODIN3_CSYM_SIGNAL;
    declared(where.decl, out);
    out->decl = where.declarator;
    out->name = odin3_ast_name(res.ast, where.declarator);
    return ODIN3_OK;
}

static odin3_status host_lookup(void *user, odin3_ast_id ref, odin3_csym *out) {
    (void)user;
    if (odin3_ast_kind_of(res.ast, ref) == ODIN3_AST_SELECT) {
        return lookup_word(ref, out);
    }
    odin3_status st = lookup_name(ref, odin3_ast_name_str(res.ast, ref), out);
    if (corrupt == CORRUPT_FLAGS && out->kind == ODIN3_CSYM_CONST) {
        out->value.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED; /* the type still says unsized */
    }
    if (corrupt == CORRUPT_RANGE && out->kind == ODIN3_CSYM_SIGNAL) {
        out->type.width = 16; /* the range still says [7:0] */
    }
    return st;
}

static odin3_status host_callee(void *user, odin3_ast_id call, odin3_csym *out) {
    (void)user;
    found where = {{0}, {0}};
    if (find_name(odin3_ast_name_str(res.ast, call), &where) && where.declarator.v == 0) {
        bool fn = odin3_ast_kind_of(res.ast, where.decl) == ODIN3_AST_FUNCTION_DECL;
        out->kind = fn ? ODIN3_CSYM_FUNCTION : ODIN3_CSYM_TASK;
        out->decl = where.decl;
        out->name = odin3_ast_name(res.ast, where.decl);
    }
    return ODIN3_OK;
}

static void host_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    (void)user;
    odin3_diag(res.design, level, loc, "%s", msg);
}

static const odin3_ceval_host HOST = {host_lookup, host_callee, host_report, NULL};

/* --- fixtures ------------------------------------------------------------------------------ */

static void parse(const char *text) {
    source = text;
    vt_parse("t.v", text, &res);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, res.status, res.log.data);
    odin3_log_set_sink(capture, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_create(res.ast, &HOST, &ev));
}

void setUp(void) {
    memset(&res, 0, sizeof res);
    ev = NULL;
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
    odin3_strbuf_init(&logged);
    last[0] = '\0';
    errors_logged = 0;
    warnings_logged = 0;
    n_params = 0;
    corrupt = CORRUPT_NONE;
    source = NULL;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_ceval_destroy(ev);
    ev = NULL;
    if (res.design != NULL) {
        vt_free(&res);
    }
    odin3_arena_destroy(values);
    odin3_strbuf_free(&logged);
}

/* The value expression of parameter name. */
static odin3_ast_id param_expr(const char *name) {
    found where = {{0}, {0}};
    TEST_ASSERT_TRUE_MESSAGE(find_name(name, &where), name);
    return odin3_ast_child(res.ast, where.declarator, 0);
}

static odin3_status eval_param(const char *name, odin3_cval *out) {
    char what[MSG];
    (void)snprintf(what, sizeof what, "parameter '%s'", name);
    odin3_ceval_req req = {param_expr(name), {0, false, false}, what};
    return odin3_ceval_expr(ev, &req, values, out);
}

static int64_t param_int(const char *name) {
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, eval_param(name, &val), logged.data);
    int64_t out = 0;
    TEST_ASSERT_TRUE_MESSAGE(odin3_cval_to_i64(&val, &out), name);
    return out;
}

/* The value printed (EL-9 spelling): "8'b0000x01x", "6", … */
static const char *printed(const odin3_cval *val) {
    static char buf[MSG];
    odin3_strbuf sb;
    odin3_strbuf_init(&sb);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_print(val, &sb));
    (void)snprintf(buf, sizeof buf, "%s", sb.data);
    odin3_strbuf_free(&sb);
    return buf;
}

/* "t.v:L:C" of the n-th (from 1) occurrence of needle in the source. */
static const char *at(const char *needle, int nth) {
    static char buf[MSG];
    const char *pos = source;
    for (int k = 0; k < nth; k++) {
        pos = strstr(k == 0 ? pos : pos + 1, needle);
        TEST_ASSERT_NOT_NULL_MESSAGE(pos, needle);
    }
    int line = 1;
    const char *line_start = source;
    for (const char *cur = source; cur < pos; cur++) {
        if (*cur == '\n') {
            line++;
            line_start = cur + 1;
        }
    }
    (void)snprintf(buf, sizeof buf, "t.v:%d:%d", line, (int)(pos - line_start) + 1);
    return buf;
}

static void expect_error(const char *where, const char *msg) {
    char want[MSG];
    (void)snprintf(want, sizeof want, "%s: error: %s", where, msg);
    TEST_ASSERT_EQUAL_STRING(want, last);
    TEST_ASSERT_EQUAL_INT(1, errors_logged);
}

/* Caps to lower (0 keeps the default). */
typedef struct caps {
    uint32_t steps, loops, depth;
} caps;

static void lower(caps wish) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_eval_steps = wish.steps != 0 ? wish.steps : lim.max_eval_steps;
    lim.max_loop_iterations = wish.loops != 0 ? wish.loops : lim.max_loop_iterations;
    lim.max_call_depth = wish.depth != 0 ? wish.depth : lim.max_call_depth;
    odin3_elab_test_set_limits(&lim);
}

/* --- the brief's functions ----------------------------------------------------------------- */

static const char *const CLOG2_SRC =
    "module t;\n"
    "  function integer clog2(input integer v); integer i; begin clog2 = 0; for (i = v - 1; "
    "i > 0; i = i >> 1) clog2 = clog2 + 1; end endfunction\n"
    "  localparam C1 = clog2(1);\n"
    "  localparam C2 = clog2(2);\n"
    "  localparam C3 = clog2(3);\n"
    "  localparam C1024 = clog2(1024);\n"
    "  localparam C1025 = clog2(1025);\n"
    "  localparam W = C1025 + 1;\n"
    "endmodule\n";

static void test_clog2_loop(void) {
    parse(CLOG2_SRC);
    TEST_ASSERT_EQUAL_INT64(0, param_int("C1"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("C2"));
    TEST_ASSERT_EQUAL_INT64(2, param_int("C3"));
    TEST_ASSERT_EQUAL_INT64(10, param_int("C1024"));
    TEST_ASSERT_EQUAL_INT64(11, param_int("C1025"));
    /* W reads C1025 through the host, which evaluates it first (a nested evaluation) */
    TEST_ASSERT_EQUAL_INT64(12, param_int("W"));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("C1025", &val));
    TEST_ASSERT_EQUAL_UINT32(32, val.width); /* an integer function: 32 bits, signed */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, val.flags);
    TEST_ASSERT_EQUAL_INT(0, errors_logged + warnings_logged);
}

static void test_while(void) {
    parse("module t;\n"
          "  function integer f(input integer n); integer k; begin k = 0; while (k < n) "
          "k = k + 2; f = k; end endfunction\n"
          "  localparam F5 = f(5);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(6, param_int("F5"));
}

static void test_casez(void) {
    parse("module t;\n"
          "  function [1:0] f(input [3:0] v); casez (v) 4'b1???: f = 3; 4'b01??: f = 2; "
          "default: f = 0; endcase endfunction\n"
          "  localparam A = f(4'b0110);\n"
          "  localparam B = f(4'b1000);\n"
          "  localparam C = f(4'b0001);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(2, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(3, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("C"));
}

static const char *const FACT_SRC = "module t;\n"
                                    "  function automatic integer fact(input integer n);\n"
                                    "    fact = (n <= 1) ? 1 :\n"
                                    "                  n * fact(n - 1);\n"
                                    "  endfunction\n"
                                    "  localparam F10 = fact(10);\n"
                                    "  localparam F9 = fact(9);\n"
                                    "  localparam F8 = fact(8);\n"
                                    "endmodule\n";

static void test_recursion(void) {
    parse(FACT_SRC);
    TEST_ASSERT_EQUAL_INT64(3628800, param_int("F10"));
    lower((caps){.depth = 8});
    TEST_ASSERT_EQUAL_INT64(40320, param_int("F8")); /* exactly 8 frames */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("F9", &val));
    expect_error("t.v:4:23", "constant function recursion deeper than 8 in 'fact'");
}

static void test_steps_cap(void) {
    parse("module t;\n"
          "  function integer f(input integer n); integer k; begin k = 0; while (1) k = k + 1; "
          "f = k; end endfunction\n"
          "  localparam P = f(1);\n"
          "endmodule\n");
    lower((caps){.steps = 1000});
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    expect_error(at("f(1)", 1), "constant evaluation of 'f' exceeds 1000 steps");
}

static void test_loop_cap(void) {
    parse("module t;\n"
          "  function integer f(input integer n); integer k; begin f = 0; "
          "for (k = 0; k < n; k = k + 1) f = f + k; end endfunction\n"
          "  localparam P = f(100);\n"
          "  localparam Q = f(16);\n"
          "  localparam R = f(17);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(4950, param_int("P"));
    lower((caps){.loops = 16});
    TEST_ASSERT_EQUAL_INT64(120, param_int("Q")); /* exactly 16 iterations */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("R", &val));
    expect_error(at("for", 1), "loop exceeds 16 iterations");
}

/* --- non-constant functions ---------------------------------------------------------------- */

/* A source whose P calls a function f that is not constant: where, and why. */
typedef struct nonconst_case {
    const char *text;
    const char *needle; /* where the error is located */
    const char *reason;
} nonconst_case;

static void expect_not_constant(const nonconst_case *kase) {
    parse(kase->text);
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    char msg[MSG];
    (void)snprintf(msg, sizeof msg, "function 'f' is not a constant function: %s", kase->reason);
    expect_error(at(kase->needle, 1), msg);
}

static void test_reads_module_variable(void) {
    expect_not_constant(
        &(nonconst_case){"module m(input [7:0] x);\n"
                         "  function integer f(input integer n); f = n + x; endfunction\n"
                         "  localparam P = f(1);\n"
                         "endmodule\n",
                         "x;", "it reads module variable 'x'"});
}

static void test_calls_task(void) {
    expect_not_constant(
        &(nonconst_case){"module m;\n"
                         "  task t; begin end endtask\n"
                         "  function integer f(input integer n); begin t; f = n; end endfunction\n"
                         "  localparam P = f(1);\n"
                         "endmodule\n",
                         "t; f", "it calls task 't'"});
}

static void test_calls_system_task(void) {
    expect_not_constant(&(nonconst_case){
        "module m;\n"
        "  function integer f(input integer n); begin $finish; f = n; end endfunction\n"
        "  localparam P = f(1);\n"
        "endmodule\n",
        "$finish", "it calls system task '$finish'"});
}

static void test_assigns_non_local(void) {
    expect_not_constant(
        &(nonconst_case){"module m;\n"
                         "  reg [7:0] r;\n"
                         "  function integer f(input integer n); begin r = n; f = n; end "
                         "endfunction\n"
                         "  localparam P = f(1);\n"
                         "endmodule\n",
                         "r = n", "it assigns 'r', which is not local"});
}

static void test_display_ignored(void) {
    parse("module m;\n"
          "  function integer f(input integer n); begin $display(\"n = %d\", n); f = n + 1; end "
          "endfunction\n"
          "  localparam P = f(1);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(2, param_int("P"));
    TEST_ASSERT_EQUAL_size_t(0, logged.len);
}

static void test_result_never_assigned(void) {
    parse("module m;\n"
          "  function [7:0] f(input integer n); if (n == 5) f = 1; endfunction\n"
          "  localparam P = f(3);\n"
          "  localparam Q = f(5);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("P", &val));
    TEST_ASSERT_EQUAL_STRING("8'bxxxxxxxx", printed(&val));
    TEST_ASSERT_EQUAL_INT64(1, param_int("Q"));
}

static void test_declared_after_use(void) {
    parse("module m;\n"
          "  localparam P = g(3);\n"
          "  function integer g(input integer n); g = n * 2; endfunction\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(6, param_int("P"));
}

/* --- system functions ---------------------------------------------------------------------- */

static void test_clog2_system(void) {
    parse("module m;\n"
          "  localparam A = $clog2(0);\n"
          "  localparam B = $clog2(1);\n"
          "  localparam C = $clog2(2);\n"
          "  localparam D = $clog2(3);\n"
          "  localparam E = $clog2(32'h8000_0000);\n"
          "  localparam F = $clog2(33'h1_0000_0001);\n"
          "  localparam G = $clog2(4'b1x00);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(0, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("C"));
    TEST_ASSERT_EQUAL_INT64(2, param_int("D"));
    TEST_ASSERT_EQUAL_INT64(31, param_int("E"));
    TEST_ASSERT_EQUAL_INT64(33, param_int("F")); /* Yosys 0.55 */
    TEST_ASSERT_EQUAL_INT64(0, param_int("G"));  /* x/z bits: 0, as Yosys 0.55 */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("E", &val));
    TEST_ASSERT_EQUAL_UINT32(32, val.width);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, val.flags);
}

static void test_clog2_not_constant(void) {
    parse("module m(input [7:0] w);\n"
          "  localparam P = $clog2(w);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    expect_error(at("w)", 2), "$clog2 argument is not constant (it reads 'w')");
}

static void test_other_system_function(void) {
    parse("module m;\n"
          "  localparam P = $random;\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    expect_error(at("$random", 1), "system function '$random' is not supported");
}

static void test_signed_unsigned(void) {
    parse("module m;\n"
          "  localparam A = $signed(4'b1100);\n"
          "  localparam [7:0] B = $signed(4'b1100) + 8'd0;\n"
          "  localparam C = $unsigned(-4'sd4) + 8'd0;\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("A", &val));
    TEST_ASSERT_EQUAL_STRING("4'sb1100", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("B", &val));
    TEST_ASSERT_EQUAL_STRING("8'b00001100", printed(&val)); /* unsigned context: zero extension */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("C", &val));
    TEST_ASSERT_EQUAL_STRING("8'b00001100", printed(&val));
}

/* --- try and exec -------------------------------------------------------------------------- */

static void test_try_not_constant(void) {
    parse("module m(input [7:0] a);\n"
          "  localparam P = a + 1;\n"
          "  localparam Q = 3 + 1;\n"
          "  function integer f(input integer n); f = n + a; endfunction\n"
          "  localparam R = f(2);\n"
          "endmodule\n");
    odin3_ceval_req req = {param_expr("P"), {0, false, false}, "parameter 'P'"};
    odin3_cval val = {0};
    bool is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_try(ev, &req, values, &val, &is_const));
    TEST_ASSERT_FALSE(is_const);
    req.expr = param_expr("R");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_try(ev, &req, values, &val, &is_const));
    TEST_ASSERT_FALSE(is_const);
    TEST_ASSERT_EQUAL_size_t(0, logged.len);
    req.expr = param_expr("Q");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_try(ev, &req, values, &val, &is_const));
    TEST_ASSERT_TRUE(is_const);
    TEST_ASSERT_EQUAL_STRING("4", printed(&val));
}

typedef struct write_rec {
    odin3_ast_id lvalue;
    int64_t index;
    bool has_index;
    odin3_cval value; /* a copy in `values` */
} write_rec;

static write_rec writes[MAX_WRITES];
static uint32_t n_writes;
static odin3_cval stored; /* what the test store holds for every read */
static bool stored_known;

static odin3_status store_read(void *user, odin3_ast_id ref, odin3_cval *out, bool *known) {
    (void)user;
    (void)ref;
    *out = stored;
    *known = stored_known;
    return ODIN3_OK;
}

static odin3_status store_write(void *user, odin3_ast_id lvalue, const odin3_cval *value) {
    (void)user;
    TEST_ASSERT_TRUE(n_writes < MAX_WRITES);
    write_rec *rec = &writes[n_writes++];
    rec->lvalue = lvalue;
    rec->has_index = odin3_ceval_word_index(ev, 0, &rec->index);
    odin3_status st = odin3_cval_copy(values, value, &rec->value); /* may fail: the OOM sweep */
    if (st != ODIN3_OK) {
        n_writes--;
        return st;
    }
    stored = rec->value; /* a later read sees it */
    return ODIN3_OK;
}

static const odin3_ceval_store STORE = {store_read, store_write, NULL};

static odin3_ast_id initial_stmt(int nth) {
    odin3_ast_id mod = module_node();
    int seen = 0;
    for (uint32_t k = 2; k < odin3_ast_nchild(res.ast, mod); k++) {
        odin3_ast_id item = odin3_ast_child(res.ast, mod, k);
        if (odin3_ast_kind_of(res.ast, item) == ODIN3_AST_INITIAL && ++seen == nth) {
            return odin3_ast_child(res.ast, item, 0);
        }
    }
    TEST_FAIL_MESSAGE("no initial");
    return (odin3_ast_id){0};
}

static const char *const EXEC_SRC = "module m(input [7:0] a);\n"
                                    "  reg [7:0] m [0:3];\n"
                                    "  reg [7:0] r;\n"
                                    "  initial m[2] = 8'h12;\n"
                                    "  initial begin r = 8'h0F; r[7:4] = 4'hA; end\n"
                                    "  initial r = a;\n"
                                    "endmodule\n";

static void exec_setup(void) {
    parse(EXEC_SRC);
    n_writes = 0;
    stored_known = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_cval_new(values, &(odin3_cval_spec){8, 0, ODIN3_BIT_X}, &stored));
}

static void test_exec_store(void) {
    exec_setup();
    bool is_const = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(1), &STORE, &is_const), logged.data);
    TEST_ASSERT_TRUE(is_const);
    TEST_ASSERT_EQUAL_UINT32(1, n_writes);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_SELECT, odin3_ast_kind_of(res.ast, writes[0].lvalue));
    TEST_ASSERT_TRUE(writes[0].has_index);
    TEST_ASSERT_EQUAL_INT64(2, writes[0].index);
    TEST_ASSERT_EQUAL_STRING("8'b00010010", printed(&writes[0].value));
}

/* A part select reads the variable, merges and writes it whole. */
static void test_exec_part_select(void) {
    exec_setup();
    bool is_const = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(2), &STORE, &is_const));
    TEST_ASSERT_TRUE(is_const);
    TEST_ASSERT_EQUAL_UINT32(2, n_writes);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_IDENT, odin3_ast_kind_of(res.ast, writes[1].lvalue));
    TEST_ASSERT_FALSE(writes[1].has_index);
    TEST_ASSERT_EQUAL_STRING("8'b10101111", printed(&writes[1].value));
}

/* A read the store does not know, or no store at all: not constant, nothing logged. */
static void test_exec_not_constant(void) {
    exec_setup();
    stored_known = false;
    bool is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(3), &STORE, &is_const));
    TEST_ASSERT_FALSE(is_const);
    TEST_ASSERT_EQUAL_UINT32(0, n_writes);
    is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(1), NULL, &is_const));
    TEST_ASSERT_FALSE(is_const);
    TEST_ASSERT_EQUAL_size_t(0, logged.len);
}

/* --- rulings: zero width, '**', callback nesting, strings, call arguments ------------------ */

/* The Task 8 ruling: {0{…}} is an empty value; Yosys 0.55 (probe p3.v) for each result. */
static void test_zero_width(void) {
    parse("module m;\n"
          "  localparam Z = {0{1'b1}};\n"
          "  localparam A = {0{1'b1}} + 8'd3;\n"
          "  localparam B = &{0{1'b1}};\n"
          "  localparam C = {8'hAB, {0{1'b1}}};\n"
          "  localparam D = |{0{1'b1}};\n"
          "  localparam E = !{0{1'b1}};\n"
          "  localparam F = 4'd5 << {0{1'b1}};\n"
          "  localparam G = ^{0{1'b1}};\n"
          "  function [3:0] f(input [3:0] v); f = v + 1; endfunction\n"
          "  localparam H = f({0{1'b1}});\n"
          "endmodule\n");
    odin3_cval val = {.width = 77};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("Z", &val));
    TEST_ASSERT_EQUAL_UINT32(0, val.width);
    TEST_ASSERT_NULL(val.aval);
    TEST_ASSERT_EQUAL_INT64(3, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(0xAB, param_int("C"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("D"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("E"));
    TEST_ASSERT_EQUAL_INT64(5, param_int("F"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("G"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("H")); /* an empty argument reads 0 */
    odin3_ceval_req req = {param_expr("Z"), {16, true, false}, "parameter 'Z'"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_expr(ev, &req, values, &val));
    TEST_ASSERT_EQUAL_STRING("16'b0000000000000000", printed(&val)); /* in a context: zeros */
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

/* The Task 7 concern: a constant ** is O(w^3); its work (steps x words^2) is bounded first. */
static void test_pow_bound(void) {
    parse("module m;\n"
          "  localparam A = 200'd3 ** 16'hFFFF;\n"
          "  localparam B = 200'd2 ** 16'hFFFF;\n"
          "  localparam C = 8'd3 ** 200;\n"
          "  localparam D = 65'd3 ** 7;\n"
          "  localparam E = A[63:0];\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("A", &val));
    const odin3_cval_part low = {&val, 0, 64};
    odin3_cval part = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_select(values, &low, &part));
    TEST_ASSERT_EQUAL_UINT64(10350089560744438443ULL, part.aval[0]); /* 3^65535 mod 2^200 */
    TEST_ASSERT_EQUAL_INT64(0, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(161, param_int("C")); /* 3^200 mod 256 */
    TEST_ASSERT_EQUAL_INT64(2187, param_int("D"));
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_eval_work = 200; /* 200 bits: 4 words, 16 products a step */
    odin3_elab_test_set_limits(&lim);
    TEST_ASSERT_EQUAL_INT64(0, param_int("B")); /* even base: 9 steps (0 after 2^8 > 200) */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val)); /* odd base: 16 steps */
    expect_error(at("200'd3", 1),
                 "constant '**' of 200 bits exceeds the limit of 200 word operations");
}

/* The Task 8 ruling: a call inside a replication count is evaluated from the typer's callback,
 * nested in C; each nesting counts against the call-depth cap with the frames. */
static const char *const NEST_SRC =
    "module m;\n"
    "  function automatic integer d(input integer n);\n"
    "    d = n + {(d(n - 1) >= 0 ? 1 : 1){1'b0}};\n"
    "  endfunction\n"
    "  function integer g(input integer n); g = n + {h(n){1'b0}}; endfunction\n"
    "  function integer h(input integer n); h = 2; endfunction\n"
    "  localparam D = d(3);\n"
    "  localparam G = g(5);\n"
    "endmodule\n";

static void test_call_nesting(void) {
    parse(NEST_SRC);
    TEST_ASSERT_EQUAL_INT64(5, param_int("G")); /* one nested level: fine */
    lower((caps){.depth = 2}); /* g's frame + the count's nesting + h's frame = 3 */
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("G", &out));
    expect_error(at("h(n)", 1), "constant function recursion deeper than 2 in 'h'");
    errors_logged = 0;
    lower((caps){.depth = 8});
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("D", &val));
    expect_error(at("d(n - 1)", 1), "constant function recursion deeper than 8 in 'd'");
}

static void test_call_nesting_default_cap(void) {
    parse(NEST_SRC); /* 512 C levels (a frame and a nesting each), under ASan: no overflow */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("D", &val));
    expect_error(at("d(n - 1)", 1), "constant function recursion deeper than 1024 in 'd'");
}

static void test_strings(void) {
    parse("module m;\n"
          "  localparam E = \"\";\n"
          "  localparam S = \"AB\";\n"
          "  localparam C = {\"a\", \"bc\"};\n"
          "  localparam T = 1'b1 ? \"xy\" : \"abc\";\n"
          "  localparam U = \"ab\" == 16'h6162;\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("E", &val)); /* 2C's EMPTY STRING: 8'h00 */
    TEST_ASSERT_EQUAL_UINT32(8, val.width);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_STRING, val.flags);
    TEST_ASSERT_EQUAL_STRING("\"\"", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("S", &val));
    TEST_ASSERT_EQUAL_STRING("\"AB\"", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("C", &val)); /* Yosys: a string concatenation */
    TEST_ASSERT_EQUAL_STRING("\"abc\"", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("T", &val)); /* zero-extended to 24 bits */
    TEST_ASSERT_EQUAL_UINT32(24, val.width);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_STRING, val.flags);
    TEST_ASSERT_EQUAL_INT64(1, param_int("U"));
}

/* Task 8's handoff: an argument is typed in the context of its input's width. Yosys 0.55's
 * eval_const_function evaluates it self-determined (probe p2.v: 0), a divergence. */
static void test_argument_context(void) {
    parse("module m;\n"
          "  function [8:0] h(input [8:0] a); h = a; endfunction\n"
          "  function [3:0] k(input [3:0] a); k = a; endfunction\n"
          "  localparam A = h(8'hFF + 8'h01);\n"
          "  localparam B = k(8'hF7);\n"
          "  localparam C = h(-4'sd1);\n"
          "  function [15:0] h16(input [15:0] a); h16 = a; endfunction\n"
          "  function [7:0] g(input [7:0] b); g = h16(b + 8'd1) >> 8; endfunction\n"
          "  function integer two(input integer n); two = n; endfunction\n"
          "  localparam D = g(8'hFF);\n"
          "  localparam E = {two(2){8'hAB}} + 64'd0;\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(256, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(7, param_int("B"));     /* truncated to the input */
    TEST_ASSERT_EQUAL_INT64(0x1FF, param_int("C")); /* a signed argument sign-extends */
    /* final review, finding 8: the 16-bit context reaches a call inside a function (b + 1 is
     * 9'h100), and a replication count from a call inside a 64-bit context */
    TEST_ASSERT_EQUAL_INT64(1, param_int("D"));
    TEST_ASSERT_EQUAL_INT64(0xABAB, param_int("E"));
}

/* Yosys 0.55 (probe p4.v) for each value. */
static void test_function_bodies(void) {
    parse("module m;\n"
          "  function [7:0] pw(input [7:0] a); reg [7:0] t; begin t = a; t[2] = 1'b0; "
          "t[7:4] = 4'h9; pw = t; end endfunction\n"
          "  function [7:0] rp(input integer n); begin rp = 0; repeat (n) rp = rp + 3; end "
          "endfunction\n"
          "  function integer inner(input integer n); inner = n * 10; endfunction\n"
          "  function integer outer(input integer n); outer = inner(n) + inner(n + 1); "
          "endfunction\n"
          "  function [7:0] blk(input integer n); integer k; begin blk = 0; "
          "for (k = 0; k < n; k = k + 1) begin : b reg [7:0] acc; if (k == 0) acc = 1; "
          "blk = blk + (acc === 8'bx ? 8'd100 : acc); end end endfunction\n"
          "  function [7:0] lp(input integer n); localparam [3:0] L = 8'hF5; lp = L + n; "
          "endfunction\n"
          "  function signed [7:0] sg(input signed [3:0] x); sg = x; endfunction\n"
          "  function [7:0] cc(input [7:0] a); reg [3:0] hi, lo; begin {hi, lo} = a; "
          "cc = {lo, hi}; end endfunction\n"
          "  function [7:0] pm(input [7:0] a); reg [7:0] t; begin t = 0; t[1 +: 3] = a; "
          "t[7 -: 2] = 2'b10; pm = t; end endfunction\n"
          "  localparam A = pw(8'h07);\n"
          "  localparam B = rp(4);\n"
          "  localparam C = outer(2);\n"
          "  localparam D = blk(3);\n"
          "  localparam E = lp(1);\n"
          "  localparam F = sg(4'b1000);\n"
          "  localparam G = rp(-1);\n"
          "  localparam H = cc(8'h3C);\n"
          "  localparam I = pm(8'h05);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(0x93, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(12, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(50, param_int("C"));
    TEST_ASSERT_EQUAL_INT64(201, param_int("D")); /* the block's reg is x at each entry */
    TEST_ASSERT_EQUAL_INT64(6, param_int("E"));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("F", &val));
    TEST_ASSERT_EQUAL_STRING("8'sb11111000", printed(&val));
    TEST_ASSERT_EQUAL_INT64(0, param_int("G"));
    TEST_ASSERT_EQUAL_INT64(0xC3, param_int("H"));
    TEST_ASSERT_EQUAL_INT64(0x8A, param_int("I"));
}

/* EL-36's matching (the brief's casez and casex), which Yosys 0.55's constant functions miss:
 * a wildcard item never matches there (probes p1.v, p2.v). x conditions read false. */
static void test_case_and_conditions(void) {
    parse("module m;\n"
          "  function [1:0] fx(input [3:0] v); casex (v) 4'b1xxx: fx = 3; 4'b01xx: fx = 2; "
          "default: fx = 1; endcase endfunction\n"
          "  function [1:0] fp(input [3:0] v); case (v) 4'b1x00: fp = 3; 4'b0110: fp = 2; "
          "default: fp = 1; endcase endfunction\n"
          "  function [1:0] fm(input [3:0] v); case (v) 1, 2: fm = 3; default: fm = 0; endcase "
          "endfunction\n"
          "  function [3:0] ifx(input [3:0] c); if (c) ifx = 1; else ifx = 2; endfunction\n"
          "  function [3:0] wx(input [3:0] c); begin wx = 0; while (c) begin wx = 5; c = 0; end "
          "end endfunction\n"
          "  localparam A = fx(4'b0110);\n"
          "  localparam B = fp(4'b1x00);\n"
          "  localparam C = fp(4'b0110);\n"
          "  localparam D = fm(2);\n"
          "  localparam E = ifx(4'b00x0);\n"
          "  localparam F = wx(4'b00x0);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(2, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(1, param_int("B")); /* plain case: x never matches (Yosys ==) */
    TEST_ASSERT_EQUAL_INT64(2, param_int("C"));
    TEST_ASSERT_EQUAL_INT64(3, param_int("D")); /* several expressions in one item (C4) */
    TEST_ASSERT_EQUAL_INT64(2, param_int("E"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("F"));
}

/* Yosys 0.55 (probe p3.v): selects of a ranged localparam, unsized extension. */
static void test_selects_and_unsized(void) {
    parse("module m;\n"
          "  localparam [7:4] Q = 4'hA;\n"
          "  localparam H = Q[5];\n"
          "  localparam I = Q[7:6];\n"
          "  localparam J = Q[4 +: 2];\n"
          "  localparam K = Q[3];\n"
          "  localparam R = Q[4:5];\n"
          "  localparam P = 'hFFFFFFFF;\n"
          "  localparam L = P + 64'd0;\n"
          "  localparam N = $signed('hFFFFFFFF) + 64'd0;\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, param_int("H"));
    TEST_ASSERT_EQUAL_INT64(2, param_int("I"));
    TEST_ASSERT_EQUAL_INT64(2, param_int("J"));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("K", &val)); /* out of range: x */
    TEST_ASSERT_EQUAL_STRING("1'bx", printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("L", &val));
    TEST_ASSERT_EQUAL_STRING("64'b1111111111111111111111111111111111111111111111111111111111111111",
                             printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("N", &val));
    TEST_ASSERT_EQUAL_STRING("64'b1111111111111111111111111111111111111111111111111111111111111111",
                             printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("P", &val)); /* still unsized (EL-16) */
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_UNSIZED, val.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("R", &val));
    expect_error(at("Q[4:5]", 1),
                 "part-select [4:5] of 'Q' is reversed relative to its declaration [7:4]");
}

/* Yosys 0.55 (probe p5.v): ?: and $signed/$unsigned rebuild a sized constant (mkconst_bits),
 * after extending an unsized operand by its top bit. */
static void test_unsized_through_operators(void) {
    parse("module m;\n"
          "  localparam A = 1'b1 ? 'hFFFFFFFF : 'h0;\n"
          "  localparam B = $unsigned('hFFFFFFFF);\n"
          "  localparam C = $signed('hFFFFFFFF);\n"
          "  localparam E = $unsigned('hFFFFFFFF) + 64'd0;\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("A", &val));
    TEST_ASSERT_EQUAL_UINT8(0, val.flags); /* sized: zero-extends into 64 bits */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("B", &val));
    TEST_ASSERT_EQUAL_UINT8(0, val.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("C", &val));
    TEST_ASSERT_EQUAL_UINT8(ODIN3_CVAL_SIGNED, val.flags);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("E", &val));
    TEST_ASSERT_EQUAL_STRING("64'b1111111111111111111111111111111111111111111111111111111111111111",
                             printed(&val));
}

static void test_call_errors(void) {
    parse("module m;\n"
          "  task t; begin end endtask\n"
          "  function integer f(input integer n); f = n; endfunction\n"
          "  localparam A = f(1, 2);\n"
          "  localparam B = nope(1);\n"
          "  localparam C = t(1);\n"
          "  localparam D = q + 1;\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    expect_error(at("f(1, 2)", 1), "function 'f' has 1 inputs; 2 arguments given");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("B", &val));
    expect_error(at("nope", 1), "'nope' is not declared");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("C", &val));
    expect_error(at("t(1)", 1), "'t' is not a function");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("D", &val));
    expect_error(at("q + 1", 1), "'q' is not declared");
}

static void test_function_statement_errors(void) {
    parse("module m;\n"
          "  function integer f(input integer n); begin f = 0; forever f = 1; end endfunction\n"
          "  localparam A = f(1);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    expect_error(at("forever", 1), "'forever' is not supported");
}

static void test_steps_without_function(void) {
    parse("module m;\n"
          "  localparam A = 1 + 2 + 3 + 4 + 5;\n"
          "endmodule\n");
    lower((caps){.steps = 4});
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    char want[MSG];
    TEST_ASSERT_TRUE(strstr(last, ": error: constant evaluation exceeds 4 steps") != NULL);
    (void)want;
    TEST_ASSERT_EQUAL_INT(1, errors_logged);
}

/* try: caps and errors still report; only non-constant input is silent. */
static void test_try_reports_errors(void) {
    parse("module m;\n"
          "  function integer f(input integer n); integer k; begin k = 0; while (1) k = k + 1; "
          "f = k; end endfunction\n"
          "  localparam A = f(1);\n"
          "  localparam B = $random;\n"
          "endmodule\n");
    lower((caps){.steps = 100});
    odin3_ceval_req req = {param_expr("A"), {0, false, false}, "parameter 'A'"};
    odin3_cval val = {0};
    bool is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ceval_try(ev, &req, values, &val, &is_const));
    TEST_ASSERT_FALSE(is_const);
    expect_error(at("f(1)", 1), "constant evaluation of 'f' exceeds 100 steps");
    errors_logged = 0;
    req.expr = param_expr("B");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ceval_try(ev, &req, values, &val, &is_const));
    expect_error(at("$random", 1), "system function '$random' is not supported");
}

/* A recursive call types its body's nodes in its own frame: with a width that depends on a
 * local ({n{1'b1}}), the caller's expression must be typed again when the call returns.
 * Yosys 0.55 (probes p6.v, p7.v): 63, 1023, 7 and 7. */
static void test_retype_after_recursion(void) {
    parse("module m;\n"
          "  function automatic integer r(input integer n);\n"
          "    r = (n <= 0) ? 0 : {r(n - 1), {n{1'b1}}};\n"
          "  endfunction\n"
          "  function automatic integer u(input integer n);\n"
          "    u = (n >= 3) ? 0 : {u(n + 1), {n{1'b1}}};\n"
          "  endfunction\n"
          "  localparam A = r(3);\n"
          "  localparam B = r(4);\n"
          "  localparam C = u(1);\n"
          "  localparam D = u(0);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(63, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(1023, param_int("B"));
    TEST_ASSERT_EQUAL_INT64(7, param_int("C")); /* re-typed after the deeper frame's {3{…}} */
    TEST_ASSERT_EQUAL_INT64(7, param_int("D")); /* likewise (probe p7.v) */
}

/* §9.1's <what> of the typer's constants, and their x/z error. */
static void test_constant_positions(void) {
    parse("module m(input [7:0] a);\n"
          "  localparam [7:0] X = 8'hF0;\n"
          "  localparam P = {a{1'b1}};\n"
          "  localparam Q = X[a:0];\n"
          "  localparam R = {4'bx{1'b1}};\n"
          "  localparam S = X[7:4];\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    expect_error(at("a{", 1), "replication count is not constant (it reads 'a')");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("Q", &val));
    expect_error(at("a:0", 1), "range bound is not constant (it reads 'a')");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("R", &val));
    expect_error(at("4'bx", 1), "replication count has x or z bits");
    TEST_ASSERT_EQUAL_INT64(0xF, param_int("S"));
}

/* K12 (stage 1 rejects them first; the interpreter reports the same texts if it meets them). */
static void test_function_k12(void) {
    parse("module m;\n"
          "  reg r;\n"
          "  function integer f(input integer n); begin f = n; f <= n; end endfunction\n"
          "  function integer g(input integer n); begin #1 g = n; end endfunction\n"
          "  localparam A = f(1);\n"
          "  localparam B = g(1);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    expect_error(at("f <= n", 1), "nonblocking assignment in function 'f' is not supported");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("B", &val));
    expect_error(at("#1", 1), "timing control in function 'g' is not supported");
}

/* Local declarations: integer, time, a memory (not constant, as Yosys 0.55), real. */
static void test_local_declarations(void) {
    parse("module m;\n"
          "  function [63:0] tm(input integer n); time t; begin t = 0; t = t - 1; tm = t; end "
          "endfunction\n"
          "  function integer mm(input integer n); reg [7:0] mem [0:3]; mm = n; endfunction\n"
          "  function integer rl(input integer n); real x; rl = n; endfunction\n"
          "  function integer ig(input integer n); integer k; begin k = -2; ig = k >>> 1; end "
          "endfunction\n"
          "  localparam A = tm(0);\n"
          "  localparam B = mm(1);\n"
          "  localparam C = rl(1);\n"
          "  localparam D = ig(0);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("A", &val)); /* time: 64 bits, unsigned */
    TEST_ASSERT_EQUAL_STRING("64'b1111111111111111111111111111111111111111111111111111111111111111",
                             printed(&val));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("B", &val));
    expect_error(at("mem [", 1), "function 'mm' is not a constant function: it declares memory "
                                 "'mem'");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("C", &val));
    expect_error(at("real", 1), "variable type 'real' is not supported");
    TEST_ASSERT_EQUAL_INT64(-1, param_int("D")); /* integer: signed */
}

/* --- a store keyed by name and word index, for exec ---------------------------------------- */

typedef struct cell {
    uint32_t name;
    int64_t index;
    odin3_cval value;
} cell;

static cell cells[MAX_WRITES];
static uint32_t n_cells;

static odin3_ast_id ref_base(odin3_ast_id ref) {
    while (odin3_ast_kind_of(res.ast, ref) == ODIN3_AST_SELECT) {
        ref = odin3_ast_child(res.ast, ref, 0);
    }
    return ref;
}

static cell *find_cell(odin3_ast_id ref, bool add) {
    uint32_t name = odin3_ast_name(res.ast, ref_base(ref));
    int64_t index = -1;
    (void)odin3_ceval_word_index(ev, 0, &index);
    for (uint32_t k = 0; k < n_cells; k++) {
        if (cells[k].name == name && cells[k].index == index) {
            return &cells[k];
        }
    }
    if (!add) {
        return NULL;
    }
    TEST_ASSERT_TRUE(n_cells < MAX_WRITES);
    cells[n_cells] = (cell){name, index, {0}};
    return &cells[n_cells++];
}

static odin3_status map_read(void *user, odin3_ast_id ref, odin3_cval *out, bool *known) {
    (void)user;
    const char *name = odin3_ast_name_str(res.ast, ref_base(ref));
    *known = strcmp(name, "a") != 0; /* the input is not constant */
    const cell *found_cell = find_cell(ref, false);
    if (found_cell != NULL) {
        *out = found_cell->value;
        return ODIN3_OK;
    }
    odin3_csym sym = {0};
    odin3_status st = host_lookup(NULL, ref, &sym);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return odin3_cval_new(values, &(odin3_cval_spec){sym.type.width, 0, ODIN3_BIT_X}, out);
}

static odin3_status map_write(void *user, odin3_ast_id lvalue, const odin3_cval *value) {
    (void)user;
    cell *slot = find_cell(lvalue, true);
    return odin3_cval_copy(values, value, &slot->value);
}

static const odin3_ceval_store MAP = {map_read, map_write, NULL};

static const char *cell_value(const char *name, int64_t index) {
    for (uint32_t k = 0; k < n_cells; k++) {
        if (strcmp(odin3_ast_name_str(res.ast, (odin3_ast_id){0}), "") == 0 &&
            cells[k].index == index &&
            strcmp(odin3_strtab_get(odin3_design_strtab(res.design), cells[k].name), name) == 0) {
            return printed(&cells[k].value);
        }
    }
    return "(none)";
}

static const char *const LOOPS_SRC =
    "module m(input [7:0] a);\n"
    "  reg [7:0] mem [1:4];\n"
    "  integer i;\n"
    "  reg [7:0] r;\n"
    "  function [7:0] sq(input [7:0] v); sq = v * v; endfunction\n"
    "  initial begin mem[1] = 8'h12; for (i = 2; i <= 4; i = i + 1) mem[i] = sq(i); end\n"
    "  initial begin r = mem[3]; r[0] <= 1'b0; $display(\"r\"); end\n"
    "  initial mem[4'bx] = 8'hFF;\n"
    "  initial begin r = 8'h01; r = a; end\n"
    "endmodule\n";

static void exec_ok(int nth) {
    bool is_const = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(nth), &MAP, &is_const), logged.data);
    TEST_ASSERT_TRUE(is_const);
}

static void test_exec_loops(void) {
    parse(LOOPS_SRC);
    n_cells = 0;
    exec_ok(1);
    TEST_ASSERT_EQUAL_STRING("8'b00010010", cell_value("mem", 1));
    TEST_ASSERT_EQUAL_STRING("8'b00001001", cell_value("mem", 3));
    TEST_ASSERT_EQUAL_STRING("8'b00010000", cell_value("mem", 4));
    TEST_ASSERT_EQUAL_STRING("5", cell_value("i", -1)); /* the loop variable is the module's */
    exec_ok(2);
    TEST_ASSERT_EQUAL_STRING("8'b00001000", cell_value("r", -1)); /* 9 with bit 0 cleared */
    TEST_ASSERT_EQUAL_size_t(0, logged.len);
}

static void test_exec_x_index_and_input(void) {
    parse(LOOPS_SRC);
    n_cells = 0;
    exec_ok(3);
    TEST_ASSERT_EQUAL_UINT32(0, n_cells); /* an x word index: the write is dropped */
    bool is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(4), &MAP, &is_const));
    TEST_ASSERT_FALSE(is_const); /* reads the input: the store drops the call's writes */
    TEST_ASSERT_EQUAL_size_t(0, logged.len);
}

static void test_misuse(void) {
    parse("module m;\n"
          "  localparam A = 1;\n"
          "  initial begin end\n"
          "endmodule\n");
    TEST_ASSERT_NOT_NULL(odin3_ceval_types(ev));
    TEST_ASSERT_NULL(odin3_ceval_types(NULL));
    odin3_ceval *other = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ceval_create(res.ast, NULL, &other));
    odin3_ceval_host bad = HOST;
    bad.report = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ceval_create(res.ast, &bad, &other));
    odin3_ceval_req req = {param_expr("A"), {0, false, false}, NULL};
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ceval_call(ev, &req, values, &val));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ceval_expr(ev, &req, NULL, &val));
    req.expr = module_node();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ceval_expr(ev, &req, values, &val));
    odin3_ceval_destroy(NULL);
}

static void test_misuse_exec(void) {
    parse("module m;\n"
          "  localparam A = 1;\n"
          "  initial begin end\n"
          "endmodule\n");
    bool is_const = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ceval_exec(ev, param_expr("A"), NULL, &is_const));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(1), NULL, &is_const));
    TEST_ASSERT_TRUE(is_const);
    int64_t idx = 0;
    TEST_ASSERT_FALSE(odin3_ceval_word_index(ev, 0, &idx)); /* outside a store call */
}

/* --- out of memory ------------------------------------------------------------------------- */

typedef odin3_status (*oom_step)(void);

/* Fails the n-th allocation of step for n = 0, 1, … until it succeeds: every failure is
 * NO_MEMORY (no leak: LeakSanitizer); returns how many failures were injected. */
static long oom_sweep(oom_step step) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fail = 0;
    for (; fail < OOM_LIMIT && st != ODIN3_OK; fail++) {
        odin3_util_set_alloc_fail_after(fail);
        st = step();
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return fail - 1;
}

static odin3_cval oom_val;

static odin3_status oom_expr(void) {
    odin3_ceval_req req = {param_expr("F8"), {0, false, false}, "parameter 'F8'"};
    return odin3_ceval_expr(ev, &req, values, &oom_val);
}

static odin3_status oom_call(void) {
    odin3_ceval_req req = {param_expr("F8"), {0, false, false}, "parameter 'F8'"};
    return odin3_ceval_call(ev, &req, values, &oom_val);
}

static odin3_status oom_exec(void) {
    bool is_const = false;
    n_writes = 0;
    odin3_status st = odin3_ceval_exec(ev, initial_stmt(1), &STORE, &is_const);
    TEST_ASSERT_TRUE(st != ODIN3_OK || is_const);
    return st;
}

static void expect_fact8(void) {
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&oom_val, &out));
    TEST_ASSERT_EQUAL_INT64(40320, out);
}

/* --- fix round 1 ---------------------------------------------------------------------------- */

/* Finding 1: a recursive call made from a width expression (a replication count) while the
 * caller's statement is still being typed. Yosys 0.55 (probe p8.v): 12, 28672, 28672, 13 and
 * 245761; the levels' own typers keep the caller's entries. */
static void test_recursive_widths(void) {
    parse("module m;\n"
          "  function automatic integer d(input integer n);\n"
          "    if (n == 0) d = 1; else d = {{n{1'b1}}, {d(n - 1){1'b0}}};\n"
          "  endfunction\n"
          "  function automatic integer e(input integer n);\n"
          "    e = {{(n + 1){1'b1}}, {((n > 0) ? e(n - 1) : 1){1'b0}}};\n"
          "  endfunction\n"
          "  function automatic integer g(input integer n);\n"
          "    if (n == 0) g = 1; else g = {{(n * 2){1'b1}}, {g(n - 1){1'b0}}, 1'b1};\n"
          "  endfunction\n"
          "  localparam D2 = d(2);\n"
          "  localparam D3 = d(3);\n"
          "  localparam E2 = e(2);\n"
          "  localparam G1 = g(1);\n"
          "  localparam G2 = g(2);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(12, param_int("D2"));
    TEST_ASSERT_EQUAL_INT64(28672, param_int("D3"));
    TEST_ASSERT_EQUAL_INT64(28672, param_int("E2"));
    TEST_ASSERT_EQUAL_INT64(13, param_int("G1"));
    TEST_ASSERT_EQUAL_INT64(245761, param_int("G2"));
}

static void lower_bytes(uint64_t bytes) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_bytes = bytes;
    odin3_elab_test_set_limits(&lim);
}

/* Finding 2: a named block's localparam reuses its storage at each entry. 4096 entries of a
 * 65536-bit localparam (16 KiB a copy, 64 MiB if each copy stayed) fit an 8 MiB budget. */
static void test_block_localparam_reuse(void) {
    parse("module m;\n"
          "  function integer f(input integer n); integer i; begin f = 0;\n"
          "    for (i = 0; i < n; i = i + 1) begin : b localparam [65535:0] Q = 1; "
          "f = f + Q[0]; end\n"
          "  end endfunction\n"
          "  localparam P = f(4096);\n"
          "endmodule\n");
    lower_bytes((uint64_t)8 << 20);
    TEST_ASSERT_EQUAL_INT64(4096, param_int("P"));
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

/* Temporaries are renewed between statements: 4096 iterations on 2048-bit values make about
 * 6 MiB of them, which a 4 MiB budget refuses unless they are renewed. */
static void test_long_loop_temporaries(void) {
    parse("module m;\n"
          "  function [2047:0] f(input integer n); integer k; begin f = 0; "
          "for (k = 0; k < n; k = k + 1) f = (f + 3) ^ {2048{1'b0}}; end endfunction\n"
          "  localparam A = f(4096);\n"
          "  localparam B = A[31:0];\n"
          "endmodule\n");
    lower_bytes((uint64_t)4 << 20);
    TEST_ASSERT_EQUAL_INT64(3 * 4096, param_int("B"));
}

/* Finding 5: the evaluator's memory against the byte budget (the hook lowers it). */
static void test_byte_budget(void) {
    parse("module m;\n"
          "  function automatic integer r(input integer n); reg [1048575:0] big; begin "
          "big = 0; r = (n <= 0) ? 0 : r(n - 1) + 1; end endfunction\n"
          "  localparam P = r(64);\n"
          "endmodule\n");
    lower_bytes((uint64_t)4 << 20); /* 256 KiB a frame */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    TEST_ASSERT_TRUE_MESSAGE(strstr(last, ": error: elaboration needs more than 4194304 bytes "
                                          "(the budget is 60% of physical memory)") != NULL,
                             last);
    TEST_ASSERT_EQUAL_INT(1, errors_logged);
}

/* Finding 4: * / % ** charge their word products to a per-evaluation work budget; a ** that
 * const_pow resolves without multiplying is never refused. */
static void test_work_budget(void) {
    parse("module m;\n"
          "  function [255:0] f(input integer n); integer k; begin f = 3; "
          "for (k = 0; k < n; k = k + 1) f = f * 3; end endfunction\n"
          "  localparam A = f(10);\n"
          "  localparam B = f(1000);\n"
          "  localparam C = 200'd5 ** -8'sd1;\n"
          "  localparam D = 200'd0 ** 16'hFFFF;\n"
          "endmodule\n");
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_eval_work = 1000; /* 256 bits: 16 products a multiply */
    odin3_elab_test_set_limits(&lim);
    TEST_ASSERT_EQUAL_INT64(177147, param_int("A")); /* 3^11, 10 multiplies */
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("B", &val));
    expect_error(at("f(1000)", 1), "constant evaluation of 'f' exceeds 1000 word operations");
    lim.max_eval_work = 16; /* one 256-bit multiply; the cheap ** below cost none */
    odin3_elab_test_set_limits(&lim);
    TEST_ASSERT_EQUAL_INT64(0, param_int("C")); /* a negative exponent: no multiply */
    TEST_ASSERT_EQUAL_INT64(0, param_int("D")); /* a zero base: no multiply */
}

/* Finding 3: a return range calling its own function nests in C through the typer alone (no
 * frame): 1024 levels at the default cap, about 2.5 KiB each under ASan. */
static void test_return_range_chain(void) {
    parse("module m;\n"
          "  function [f(1):0] f(input integer n); f = n; endfunction\n"
          "  localparam P = f(1);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P", &val));
    expect_error(at("[f(1):0]", 1), "constant function recursion deeper than 1024 in 'f'");
}

/* Finding 6: IEEE 1364-2005 §9.5.1: z (casez) or x and z (casex) on either side match. */
static void test_casez_both_sides(void) {
    parse("module m;\n"
          "  function f(input [3:0] v); casez (v) 4'b0110: f = 1; default: f = 0; endcase "
          "endfunction\n"
          "  function fx(input [3:0] v); casex (v) 4'b0110: fx = 1; default: fx = 0; endcase "
          "endfunction\n"
          "  localparam A = f(4'bz110);\n"
          "  localparam B = f(4'bx110);\n"
          "  localparam C = fx(4'bx110);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(1, param_int("A"));
    TEST_ASSERT_EQUAL_INT64(0, param_int("B")); /* casez: x is no wildcard */
    TEST_ASSERT_EQUAL_INT64(1, param_int("C"));
}

/* Finding 8: the $display family by exact name ($writememh is not one of them). */
static void test_display_names(void) {
    parse("module m;\n"
          "  reg [7:0] mem [0:3];\n"
          "  function integer f(input integer n); begin $writeh(n); $fdisplayb(1, n); f = n; end "
          "endfunction\n"
          "  function integer g(input integer n); begin $writememh(\"x.hex\", mem); g = n; end "
          "endfunction\n"
          "  localparam A = f(3);\n"
          "  localparam B = g(3);\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT64(3, param_int("A"));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("B", &val));
    expect_error(at("$writememh", 1),
                 "function 'g' is not a constant function: it calls system task '$writememh'");
}

/* Finding 9: a built-in with another argument count than 1. */
static void test_builtin_arity(void) {
    parse("module m;\n"
          "  localparam A = $clog2(1, 2);\n"
          "endmodule\n");
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    expect_error(at("$clog2", 1), "system function '$clog2' has 1 input; 2 arguments given");
}

/* Finding 7: an OOM sweep through the nested runs: a return range, a count, a part-select
 * lvalue and a case, each calling a function from the typer's callbacks (the review's source). */
static odin3_status oom_nested(void) {
    odin3_ceval_req req = {param_expr("G"), {0, false, false}, "parameter 'G'"};
    return odin3_ceval_expr(ev, &req, values, &oom_val);
}

static void test_oom_nested(void) {
    parse("module m;\n"
          "  localparam W = 3;\n"
          "  function [W:0] h(input integer n); h = n + 1; endfunction\n"
          "  function [h(2):0] g(input integer n); reg [7:0] t; begin t = 0; "
          "t[h(1) +: 2] = 2'b11; case (n) 1: g = {h(n){1'b1}}; default: g = t; endcase end "
          "endfunction\n"
          "  localparam G = g(5) + g(1);\n"
          "endmodule\n");
    TEST_ASSERT_TRUE(oom_sweep(oom_nested) > 100);
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&oom_val, &out));
    TEST_ASSERT_EQUAL_INT64(12 + 3, out); /* t = 8'b00001100; {2{1'b1}} */
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

static void test_oom(void) {
    parse(FACT_SRC);
    TEST_ASSERT_TRUE(oom_sweep(oom_expr) > 20); /* the sweep did inject failures */
    expect_fact8();
    TEST_ASSERT_TRUE(oom_sweep(oom_call) > 20);
    expect_fact8();
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

static void test_oom_exec(void) {
    parse("module m;\n"
          "  reg [7:0] r;\n"
          "  function integer f(input integer n); f = n * 3; endfunction\n"
          "  initial begin : b integer i; for (i = 0; i < 4; i = i + 1) r = f(i); end\n"
          "endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_cval_new(values, &(odin3_cval_spec){8, 0, ODIN3_BIT_X}, &stored));
    stored_known = true;
    TEST_ASSERT_TRUE(oom_sweep(oom_exec) > 20);
    TEST_ASSERT_EQUAL_UINT32(4, n_writes);
    TEST_ASSERT_EQUAL_STRING("8'b00001001", printed(&writes[3].value));
}

/* --- final review ---------------------------------------------------------------------------- */

/* `module m; localparam P0 = P1 + 1; … localparam P<links> = 0; endmodule`, in `values`. */
static const char *chain_source(int links) {
    odin3_strbuf sb;
    odin3_strbuf_init(&sb);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&sb, odin3_bytes_cstr("module m;\n")));
    for (int k = 0; k < links; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(&sb, "  localparam P%d = P%d + 1;\n", k, k + 1));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(&sb, "  localparam P%d = 0;\nendmodule\n", links));
    char *text = odin3_arena_strndup(values, sb.data, sb.len);
    TEST_ASSERT_NOT_NULL(text);
    odin3_strbuf_free(&sb);
    return text;
}

/* Finding 1: each link of a lazily evaluated parameter chain re-enters the evaluator from the
 * host's lookup (about 4 KiB of C stack under ASan); a chain within the call-depth cap works. */
static void test_parameter_chain(void) {
    parse(chain_source(1000));
    TEST_ASSERT_EQUAL_INT64(1000, param_int("P0"));
    TEST_ASSERT_EQUAL_INT(0, errors_logged);
}

/* Finding 1: the re-entries count against the call-depth cap, so a 2000-link chain (a stack
 * overflow under ASan before) is a located error at the 1025th nested evaluation. */
static void test_parameter_chain_cap(void) {
    parse(chain_source(2000));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("P0", &val));
    expect_error(at("P1026 + 1", 1),
                 "constant evaluation of parameter 'P1025' nested deeper than 1024");
}

/* Finding 4: a host answer whose type disagrees with its value or range is a host bug. */
static void test_host_symbol_checks(void) {
    parse("module m;\n"
          "  wire [7:0] w;\n"
          "  localparam P = 'hFFFFFFFF;\n"
          "  localparam A = P + 64'd0;\n"
          "  localparam B = w[3:0];\n"
          "endmodule\n");
    odin3_cval val = {0};
    corrupt = CORRUPT_FLAGS;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, eval_param("A", &val));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last, "host lookup gave a value whose width or flags "
                                              "differ from its type"),
                                 last);
    corrupt = CORRUPT_RANGE;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, eval_param("B", &val));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(last, "host lookup gave a range of another width than its type"), last);
    corrupt = CORRUPT_NONE;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eval_param("A", &val));
    TEST_ASSERT_EQUAL_STRING("64'b1111111111111111111111111111111111111111111111111111111111111111",
                             printed(&val));
}

/* Finding 5: a statement-free expression is held to the byte budget as its values are made,
 * not only every 4096 steps: 255 additions at 65536 bits (511 steps, about 12 MiB of
 * temporaries) under a 4 MiB budget. */
static void test_byte_budget_wide_expression(void) {
    odin3_strbuf sb;
    odin3_strbuf_init(&sb);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_append(&sb, odin3_bytes_cstr("module m;\n"
                                                            "  localparam W = {65536{1'b1}};\n"
                                                            "  localparam A = W")));
    for (int k = 0; k < 255; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&sb, odin3_bytes_cstr(" + W")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&sb, odin3_bytes_cstr(";\nendmodule\n")));
    char *text = odin3_arena_strndup(values, sb.data, sb.len);
    odin3_strbuf_free(&sb);
    TEST_ASSERT_NOT_NULL(text);
    parse(text);
    lower_bytes((uint64_t)4 << 20);
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, eval_param("A", &val));
    TEST_ASSERT_TRUE_MESSAGE(strstr(last, ": error: elaboration needs more than 4194304 bytes "
                                          "(the budget is 60% of physical memory)") != NULL,
                             last);
    TEST_ASSERT_EQUAL_INT(1, errors_logged);
}

/* Finding 6: an lvalue atom with more than 8 index expressions (a 7-dimensional word with a
 * part select) is refused, not written at a position read from the wrong stack slots. */
static void test_lvalue_index_cap(void) {
    parse("module m;\n"
          "  reg [7:0] m [0:1][0:1][0:1][0:1][0:1][0:1][0:1];\n"
          "  initial m[0][0][0][0][0][0][0][3:0] = 4'd5;\n"
          "  initial m[0][0][0][0][0][0][1][7] = 1'b1;\n"
          "endmodule\n");
    n_writes = 0;
    stored_known = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_cval_new(values, &(odin3_cval_spec){8, 0, ODIN3_BIT_0}, &stored));
    bool is_const = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_ceval_exec(ev, initial_stmt(1), &STORE, &is_const));
    TEST_ASSERT_FALSE(is_const);
    TEST_ASSERT_EQUAL_UINT32(0, n_writes);
    expect_error(at("m[0][0][0][0][0][0][0][3:0]", 1),
                 "assignment to a select with more than 8 indices is not supported");
    errors_logged = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_ceval_exec(ev, initial_stmt(2), &STORE, &is_const), logged.data);
    TEST_ASSERT_TRUE(is_const); /* 8 indices: the most an atom takes */
    TEST_ASSERT_EQUAL_UINT32(1, n_writes);
    TEST_ASSERT_EQUAL_STRING("8'b10000000", printed(&writes[0].value));
}

static void run_final_review(void) {
    RUN_TEST(test_parameter_chain);
    RUN_TEST(test_parameter_chain_cap);
    RUN_TEST(test_host_symbol_checks);
    RUN_TEST(test_byte_budget_wide_expression);
    RUN_TEST(test_lvalue_index_cap);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clog2_loop);
    RUN_TEST(test_while);
    RUN_TEST(test_casez);
    RUN_TEST(test_recursion);
    RUN_TEST(test_steps_cap);
    RUN_TEST(test_loop_cap);
    RUN_TEST(test_reads_module_variable);
    RUN_TEST(test_calls_task);
    RUN_TEST(test_calls_system_task);
    RUN_TEST(test_assigns_non_local);
    RUN_TEST(test_display_ignored);
    RUN_TEST(test_result_never_assigned);
    RUN_TEST(test_declared_after_use);
    RUN_TEST(test_clog2_system);
    RUN_TEST(test_clog2_not_constant);
    RUN_TEST(test_other_system_function);
    RUN_TEST(test_signed_unsigned);
    RUN_TEST(test_try_not_constant);
    RUN_TEST(test_exec_store);
    RUN_TEST(test_exec_part_select);
    RUN_TEST(test_exec_not_constant);
    RUN_TEST(test_zero_width);
    RUN_TEST(test_pow_bound);
    RUN_TEST(test_call_nesting);
    RUN_TEST(test_call_nesting_default_cap);
    RUN_TEST(test_strings);
    RUN_TEST(test_argument_context);
    RUN_TEST(test_function_bodies);
    RUN_TEST(test_case_and_conditions);
    RUN_TEST(test_selects_and_unsized);
    RUN_TEST(test_unsized_through_operators);
    RUN_TEST(test_call_errors);
    RUN_TEST(test_retype_after_recursion);
    RUN_TEST(test_constant_positions);
    RUN_TEST(test_function_k12);
    RUN_TEST(test_local_declarations);
    RUN_TEST(test_long_loop_temporaries);
    RUN_TEST(test_function_statement_errors);
    RUN_TEST(test_steps_without_function);
    RUN_TEST(test_try_reports_errors);
    RUN_TEST(test_exec_loops);
    RUN_TEST(test_exec_x_index_and_input);
    RUN_TEST(test_misuse);
    RUN_TEST(test_misuse_exec);
    RUN_TEST(test_recursive_widths);
    RUN_TEST(test_block_localparam_reuse);
    RUN_TEST(test_byte_budget);
    RUN_TEST(test_work_budget);
    RUN_TEST(test_return_range_chain);
    RUN_TEST(test_casez_both_sides);
    RUN_TEST(test_display_names);
    RUN_TEST(test_builtin_arity);
    RUN_TEST(test_oom_nested);
    RUN_TEST(test_oom);
    RUN_TEST(test_oom_exec);
    run_final_review();
    return UNITY_END();
}
