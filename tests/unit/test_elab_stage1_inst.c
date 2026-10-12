/*
 * test_elab_stage1_inst.c — 2D Task 12: instances in stage 1 (EL-10, EL-11, EL-8's cell-type
 * targets): connections resolved to ports (ordered, named, unnamed positions, the output rules),
 * instance and gate arrays with their sliced connections, `(* blackbox *)` modules and implicit
 * black boxes, the parameters of cell-type instances, and the instance cap. Sources are parsed by
 * 2C (elab_test.h); every located diagnostic is asserted as the whole line, and the shapes are
 * read from the elaborated store (or, where a later task's kind stops the store, from stage 1's
 * own tables through elab1.h).
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/verilog/elab1.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

#define TECHLIB "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib"

/* The module of the connection cases (the brief's m(x, y, z), ANSI: Task 15 lands non-ANSI). */
#define M_XYZ "module m (input x, input y, input z);\nendmodule\n"

enum { OOM_LIMIT = 400000, OOM_SETTLE = 64, TEXT_BUF = 256 };

static et_result rs;
static odin3_arena *values;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_elab_test_set_limits(NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
    odin3_arena_destroy(values);
}

/* --- running ------------------------------------------------------------------------------- */

static void run_req(const char *text, const odin3_elab_opts *opts, const char *pre) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, pre, NULL, opts};
    et_stage1(&req, &rs);
}

static void run_one(const char *text) {
    run_req(text, NULL, NULL);
}

/* A run that must succeed: OK, no error, the store passes check FULL and binds (EL-5). */
static void expect_clean(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    et_expect_binds(&rs);
}

static void run_clean(const char *text) {
    run_one(text);
    expect_clean();
}

/* The line is the only error, and stage 1 failed. */
static void expect_only_error(const char *line) {
    et_expect(&rs, ODIN3_LOG_ERROR, line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

/* --- reading the elaborated store ---------------------------------------------------------- */

static const char *name_of(odin3_ast_id node) {
    return odin3_ast_name_str(rs.elab, node);
}

/* The first node of kind named name in the store, else 0. */
static odin3_ast_id find(odin3_ast_kind kind, const char *name) {
    TEST_ASSERT_NOT_NULL_MESSAGE(rs.elab, et_log(&rs));
    uint32_t end = odin3_ast_node_end(rs.elab).v;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(rs.elab, node) == kind && strcmp(name_of(node), name) == 0) {
            return node;
        }
    }
    return (odin3_ast_id){0};
}

static odin3_ast_id instance(const char *name) {
    odin3_ast_id node = find(ODIN3_AST_INSTANCE, name);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, name);
    return node;
}

/* Connection pos of an INSTANCE (after its array slot). */
static odin3_ast_id conn_at(odin3_ast_id inst, uint32_t pos) {
    TEST_ASSERT_TRUE(pos + 1 < odin3_ast_nchild(rs.elab, inst));
    return odin3_ast_child(rs.elab, inst, pos + 1);
}

static uint32_t conn_count(odin3_ast_id inst) {
    return odin3_ast_nchild(rs.elab, inst) - 1;
}

/* A NUMBER's value as an integer (it must be known and fit). */
static int64_t number_int(odin3_ast_id node) {
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NUMBER, odin3_ast_kind_of(rs.elab, node));
    odin3_ast_literal lit;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(rs.elab, node, values, &lit));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(values, &lit, &val));
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &out));
    return out;
}

/* A leaf of the elaborated store as text: IDENT name, SELECT name[a] / name[a:b] over a name, a
 * NUMBER's value. */
static void spell_leaf(odin3_ast_id expr, char *buf, size_t len) {
    odin3_ast_kind kind = odin3_ast_kind_of(rs.elab, expr);
    int got = -1;
    if (kind == ODIN3_AST_IDENT) {
        got = snprintf(buf, len, "%s", name_of(expr));
    } else if (kind == ODIN3_AST_NUMBER) {
        got = snprintf(buf, len, "%lld", (long long)number_int(expr));
    } else if (kind == ODIN3_AST_SELECT) {
        odin3_ast_id obj = odin3_ast_child(rs.elab, expr, 0);
        odin3_ast_id lsb = odin3_ast_child(rs.elab, expr, 2);
        long long msb = (long long)number_int(odin3_ast_child(rs.elab, expr, 1));
        got = lsb.v == 0 ? snprintf(buf, len, "%s[%lld]", name_of(obj), msb)
                         : snprintf(buf, len, "%s[%lld:%lld]", name_of(obj), msb,
                                    (long long)number_int(lsb));
    }
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && (size_t)got < len, odin3_ast_kind_name(kind));
}

/* An expression as text: a leaf, or {…} of leaves. */
static void spell(odin3_ast_id expr, char *buf, size_t len) {
    if (odin3_ast_kind_of(rs.elab, expr) != ODIN3_AST_CONCAT) {
        spell_leaf(expr, buf, len);
        return;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < odin3_ast_nchild(rs.elab, expr); i++) {
        char part[TEXT_BUF];
        spell_leaf(odin3_ast_child(rs.elab, expr, i), part, sizeof part);
        int one = snprintf(buf + at, len - at, "%s%s", i > 0 ? ", " : "{", part);
        TEST_ASSERT_TRUE(one > 0 && (size_t)one < len - at);
        at += (size_t)one;
    }
    TEST_ASSERT_TRUE(at + 2 <= len);
    memcpy(buf + at, "}", 2);
}

/* Connection pos of inst is named port and spells want ("" for no expression). */
static void expect_conn(odin3_ast_id inst, uint32_t pos, const char *port, const char *want) {
    odin3_ast_id conn = conn_at(inst, pos);
    TEST_ASSERT_EQUAL_STRING(port, name_of(conn));
    odin3_ast_id expr = odin3_ast_child(rs.elab, conn, 0);
    char text[TEXT_BUF] = "";
    if (expr.v != 0) {
        spell(expr, text, sizeof text);
    }
    TEST_ASSERT_EQUAL_STRING(want, text);
}

/* The UNIT's MODULE names, comma separated. */
static const char *module_names(void) {
    static char buf[TEXT_BUF];
    size_t len = 0;
    buf[0] = '\0';
    odin3_ast_span mods = odin3_ast_children(rs.elab, odin3_ast_root(rs.elab, 0));
    for (uint32_t i = 0; i < mods.n; i++) {
        int got =
            snprintf(buf + len, sizeof buf - len, "%s%s", i > 0 ? "," : "", name_of(mods.ids[i]));
        TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof buf - len);
        len += (size_t)got;
    }
    return buf;
}

/* The params LIST entry named param of the single_port_ram INSTANTIATION: its integer value. */
static int64_t param_value(const char *param) {
    odin3_ast_id inst = find(ODIN3_AST_INSTANTIATION, "single_port_ram");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, inst.v);
    odin3_ast_id list = odin3_ast_child(rs.elab, inst, 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, list.v);
    for (uint32_t i = 0; i < odin3_ast_nchild(rs.elab, list); i++) {
        odin3_ast_id conn = odin3_ast_child(rs.elab, list, i);
        if (strcmp(name_of(conn), param) == 0) {
            return number_int(odin3_ast_child(rs.elab, conn, 0));
        }
    }
    TEST_FAIL_MESSAGE(param);
    return 0;
}

/* The black-box MODULE named name (EL-4 v4.8): the `blackbox` ATTR without a value, n_ports ports,
 * no items. */
static odin3_ast_id expect_blackbox(const char *name, uint32_t n_ports) {
    odin3_ast_id module = find(ODIN3_AST_MODULE, name);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, module.v, name);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, module));
    odin3_ast_span attrs = odin3_ast_attrs(rs.elab, module);
    TEST_ASSERT_EQUAL_UINT32(1, attrs.n);
    TEST_ASSERT_EQUAL_STRING("blackbox", name_of(attrs.ids[0]));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(rs.elab, attrs.ids[0], 0).v);
    TEST_ASSERT_EQUAL_UINT32(n_ports,
                             odin3_ast_nchild(rs.elab, odin3_ast_child(rs.elab, module, 1)));
    return module;
}

/* Port pos of module as "name dir [msb:lsb]" (no range when scalar); a wire, IN_HEADER. */
static void expect_port(odin3_ast_id module, uint32_t pos, const char *want) {
    odin3_ast_id port = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, module, 1), pos);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_PORT_DECL, odin3_ast_kind_of(rs.elab, port));
    uint16_t flags = odin3_ast_flags(rs.elab, port);
    TEST_ASSERT_TRUE((flags & ODIN3_AST_F_PORT_DECL_IN_HEADER) != 0);
    TEST_ASSERT_EQUAL_UINT32(1U + ODIN3_AST_NET_DECL_WIRE,
                             (flags & ODIN3_AST_F_PORT_DECL_NET_MASK) >>
                                 ODIN3_AST_F_PORT_DECL_NET_SHIFT);
    char text[TEXT_BUF];
    odin3_ast_id range = odin3_ast_child(rs.elab, port, 0);
    const char *decl = name_of(odin3_ast_child(rs.elab, port, 1));
    const char *dir = odin3_ast_sub_name(rs.elab, port);
    int got = range.v == 0 ? snprintf(text, sizeof text, "%s %s", decl, dir)
                           : snprintf(text, sizeof text, "%s %s [%lld:%lld]", decl, dir,
                                      (long long)number_int(odin3_ast_child(rs.elab, range, 0)),
                                      (long long)number_int(odin3_ast_child(rs.elab, range, 1)));
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof text);
    TEST_ASSERT_EQUAL_STRING(want, text);
}

/* --- connections (EL-10) ------------------------------------------------------------------- */

static void test_ordered_with_empty(void) {
    run_clean(M_XYZ "module top; wire a, c;\n  m u (a, , c);\nendmodule\n");
    odin3_ast_id inst = instance("u");
    TEST_ASSERT_EQUAL_UINT32(3, conn_count(inst));
    expect_conn(inst, 0, "x", "a");
    expect_conn(inst, 1, "y", "");
    expect_conn(inst, 2, "z", "c");
}

static void test_named_in_port_order(void) {
    run_clean(M_XYZ "module top; wire a, c;\n  m u (.z(c), .x(a));\nendmodule\n");
    odin3_ast_id inst = instance("u");
    TEST_ASSERT_EQUAL_UINT32(3, conn_count(inst));
    expect_conn(inst, 0, "x", "a");
    expect_conn(inst, 1, "y", "");
    expect_conn(inst, 2, "z", "c");
}

static void test_named_empty(void) {
    run_clean(M_XYZ "module top; wire a;\n  m u (.x(a), .y());\nendmodule\n");
    expect_conn(instance("u"), 1, "y", "");
}

static void test_no_port(void) {
    run_one(M_XYZ "module top; wire a;\n  m u (.q(a));\nendmodule\n");
    expect_only_error("t.v:4:8: error: module 'm' has no port 'q'");
}

static void test_connected_twice(void) {
    run_one(M_XYZ "module top; wire a, b;\n  m u (.x(a), .x(b));\nendmodule\n");
    expect_only_error("t.v:4:15: error: port 'x' of instance 'u' is connected twice");
}

static void test_too_many_connections(void) {
    run_one(M_XYZ "module top; wire a, b, c, d;\n  m u (a, b, c, d);\nendmodule\n");
    expect_only_error("t.v:4:5: error: instance 'u' of 'm' has 4 connections but 'm' has 3 ports");
}

/* I2, at the INSTANCE. */
static void test_mixed_connections(void) {
    run_one(M_XYZ "module top; wire a, b;\n  m u (a, .y(b));\nendmodule\n");
    expect_only_error("t.v:4:5: error: instance 'u' mixes ordered and named connections");
}

/* --- unnamed header positions ---------------------------------------------------------------- */

/* The INSTANTIATION of module top holding the instance named name, in the parsed store. */
static odin3_elab_inst parsed_instance(odin3_elab1 *e1, const char *module, const char *name) {
    odin3_elab_inst inst = {.spec = UINT32_MAX};
    for (uint32_t def = 0; def < odin3_elab1_def_count(e1); def++) {
        const odin3_elab_def *rec = odin3_elab1_def(e1, def);
        if (strcmp(odin3_elab1_str(e1, rec->name), module) != 0) {
            continue;
        }
        odin3_ast_span items = odin3_ast_children(rs.parsed, rec->node);
        for (uint32_t i = 2; i < items.n; i++) {
            if (odin3_ast_kind_of(rs.parsed, items.ids[i]) != ODIN3_AST_INSTANTIATION) {
                continue;
            }
            odin3_ast_id kid = odin3_ast_child(rs.parsed, items.ids[i], 1);
            if (strcmp(odin3_ast_name_str(rs.parsed, kid), name) == 0) {
                inst.instantiation = items.ids[i];
                inst.instance = kid;
                inst.target =
                    odin3_elab1_resolve(e1, odin3_ast_name(rs.parsed, items.ids[i]), rec->library);
            }
        }
    }
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, inst.instance.v, name);
    return inst;
}

/* `module n(a, b, )` (the one empty position 2C accepts, a trailing comma; `(a, , b)` is a
 * syntax error as in Yosys 0.55): an ordered connection to it warns and is dropped. The
 * header is non-ANSI (Task 15 lands it, so the store stops at its pending kinds): the warning
 * comes from the run, the resolution from odin3_elab1_connect itself. */
static void test_unnamed_position(void) {
    run_one("module n (a, b, );\n  input a;\n  input b;\nendmodule\n"
            "module top;\n  wire x, y, z;\n  n u (x, z, y);\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_WARN,
              "t.v:7:14: warning: connection 3 of instance 'u' goes to an unnamed port position "
              "and is ignored");
    TEST_ASSERT_EQUAL_UINT32(1, rs.warnings);
    odin3_elab1 *e1 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_create(&rs.ctx, rs.parsed, &rs.opts, &e1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_collect_defs(e1));
    odin3_elab_inst inst = parsed_instance(e1, "top", "u");
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, inst.target.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_connect(e1, &inst));
    TEST_ASSERT_EQUAL_size_t(2, e1->conns.len);
    const odin3_elab1_conn *first = odin3_vec_cat(&e1->conns, 0);
    const odin3_elab1_conn *second = odin3_vec_cat(&e1->conns, 1);
    TEST_ASSERT_EQUAL_STRING("a", odin3_elab1_str(e1, first->name));
    TEST_ASSERT_EQUAL_STRING("x", odin3_ast_name_str(rs.parsed, first->expr));
    TEST_ASSERT_EQUAL_STRING("b", odin3_elab1_str(e1, second->name));
    TEST_ASSERT_EQUAL_STRING("z", odin3_ast_name_str(rs.parsed, second->expr));
    odin3_elab1_destroy(e1);
}

/* M5: `u ()` on a port-less module. */
static void test_portless_instance(void) {
    run_clean("module e;\nendmodule\nmodule top;\n  e u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, conn_count(instance("u")));
}

/* --- instance arrays (I6, Q5) ------------------------------------------------------------- */

#define M2 "module m2 (input [1:0] d, output [1:0] q);\nendmodule\n"

static void test_array_slices(void) {
    run_clean(M2 "module top;\n  wire [7:0] bus8; wire [1:0] q2;\n"
                 "  m2 u[3:0] (.d(bus8), .q(q2));\nendmodule\n");
    odin3_ast_id holder = find(ODIN3_AST_INSTANTIATION, "m2");
    TEST_ASSERT_EQUAL_UINT32(1 + 4, odin3_ast_nchild(rs.elab, holder));
    static const char *const names[] = {"u[3]", "u[2]", "u[1]", "u[0]"};
    static const char *const slices[] = {"bus8[7:6]", "bus8[5:4]", "bus8[3:2]", "bus8[1:0]"};
    for (uint32_t k = 0; k < 4; k++) {
        odin3_ast_id inst = odin3_ast_child(rs.elab, holder, k + 1);
        TEST_ASSERT_EQUAL_STRING(names[k], name_of(inst));
        expect_conn(inst, 0, "d", slices[k]);
        expect_conn(inst, 1, "q", "q2");
    }
}

static void test_array_width_error(void) {
    run_one(M2 "module top;\n  wire [2:0] w3; wire [1:0] q2;\n"
               "  m2 u[3:0] (.d(w3), .q(q2));\nendmodule\n");
    expect_only_error(
        "t.v:5:14: error: connection of 'd' to instance array 'u' is 3 bits; expected 2 or 8");
}

/* Gate arrays slice with P = 1 (Q5). */
static void test_gate_array(void) {
    run_clean("module top;\n  wire [1:0] y2, a2, b2;\n  and g[1:0] (y2, a2, b2);\nendmodule\n");
    odin3_ast_id gate = find(ODIN3_AST_GATE_DECL, "");
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_GATE_DECL_AND, odin3_ast_sub(rs.elab, gate));
    odin3_ast_id first = odin3_ast_child(rs.elab, gate, 2);
    TEST_ASSERT_EQUAL_STRING("g[1]", name_of(first));
    expect_conn(first, 0, "", "y2[1]");
    expect_conn(first, 1, "", "a2[1]");
    expect_conn(first, 2, "", "b2[1]");
    expect_conn(odin3_ast_child(rs.elab, gate, 3), 0, "", "y2[0]");
}

/* An ascending array gives its right element the low bits (Yosys, IEEE): u[0] the high ones. */
static void test_ascending_array(void) {
    run_clean(M2 "module top;\n  wire [0:3] bus; wire [7:0] bus8; wire [1:0] q2;\n"
                 "  m2 u[0:1] (.d(bus), .q(q2));\n  m2 ub[0:3] (.d(bus8), .q(q2));\nendmodule\n");
    expect_conn(instance("u[0]"), 0, "d", "bus[2:3]"); /* the lowest index the low bits */
    expect_conn(instance("u[1]"), 0, "d", "bus[0:1]");
    expect_conn(instance("ub[0]"), 0, "d", "bus8[1:0]");
    expect_conn(instance("ub[3]"), 0, "d", "bus8[7:6]");
    odin3_ast_id holder = find(ODIN3_AST_INSTANTIATION, "m2");
    TEST_ASSERT_EQUAL_STRING("u[0]", name_of(odin3_ast_child(rs.elab, holder, 1))); /* left first */
}

/* Slices of a part select, a concatenation, a constant, and (a SELECT of its own value, bit 0 its
 * LSB) any other expression, a variable +: included, without a diagnostic. */
static void test_array_slice_forms(void) {
    run_clean(M2 "module top;\n  wire [15:0] w; wire [1:0] a, b; wire [1:0] q2;\n"
                 "  m2 u[1:0] (.d(w[11:8]), .q(q2));\n  m2 v[1:0] (.d({a, b}), .q(q2));\n"
                 "  m2 c[1:0] (.d(4'b1001), .q(q2));\n  wire [3:0] i, p, r;\n"
                 "  m2 x[1:0] (.d(p ^ r), .q(q2));\n  m2 y[1:0] (.d(w[i +: 4]), .q(q2));\n"
                 "endmodule\n");
    expect_conn(instance("x[1]"), 0, "d", "[3:2]");
    odin3_ast_id sel = odin3_ast_child(rs.elab, conn_at(instance("x[0]"), 0), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY,
                          odin3_ast_kind_of(rs.elab, odin3_ast_child(rs.elab, sel, 0)));
    sel = odin3_ast_child(rs.elab, conn_at(instance("y[0]"), 0), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_SELECT,
                          odin3_ast_kind_of(rs.elab, odin3_ast_child(rs.elab, sel, 0)));
    expect_conn(instance("y[0]"), 0, "d", "[1:0]");
    expect_conn(instance("u[1]"), 0, "d", "w[11:10]");
    expect_conn(instance("u[0]"), 0, "d", "w[9:8]");
    expect_conn(instance("v[1]"), 0, "d", "a");
    expect_conn(instance("v[0]"), 0, "d", "b");
    expect_conn(instance("c[1]"), 0, "d", "2");
    expect_conn(instance("c[0]"), 0, "d", "1");
}

/* Connection 0 of the INSTANCE named inst is a call of fn, or (msb >= 0) the slice [msb:lsb] of it.
 */
typedef struct call_conn {
    const char *inst;
    const char *fn;
    int64_t msb;
    int64_t lsb;
} call_conn;

static void expect_call_conn(const call_conn *want) {
    odin3_ast_id expr = odin3_ast_child(rs.elab, conn_at(instance(want->inst), 0), 0);
    if (want->msb >= 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_AST_SELECT, odin3_ast_kind_of(rs.elab, expr));
        TEST_ASSERT_EQUAL_INT64(want->msb, number_int(odin3_ast_child(rs.elab, expr, 1)));
        TEST_ASSERT_EQUAL_INT64(want->lsb, number_int(odin3_ast_child(rs.elab, expr, 2)));
        expr = odin3_ast_child(rs.elab, expr, 0);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_CALL, odin3_ast_kind_of(rs.elab, expr));
    TEST_ASSERT_EQUAL_STRING(want->fn, name_of(expr));
}

#define EXPECT_CALL_CONN(inst, fn, msb, lsb)                                                       \
    expect_call_conn(&(const call_conn){(inst), (fn), (msb), (lsb)})

/* I-2: a user function call in an array connection is typed at the function's declared result
 * width (Yosys 0.55, §17 #17): [3:0] here, `integer` 32, none 1 bit. */
static void test_array_function_call(void) {
    run_clean(M2 "module top;\n  wire [3:0] x; wire [1:0] q2;\n"
                 "  function [1:0] inv; input [1:0] v; inv = ~v; endfunction\n"
                 "  m2 u[1:0] (.d({inv(x[3:2]), inv(x[1:0])}), .q(q2));\nendmodule\n");
    EXPECT_CALL_CONN("u[1]", "inv", -1, 0); /* each element takes one call whole */
    EXPECT_CALL_CONN("u[0]", "inv", -1, 0);
    et_free(&rs);
    run_clean(M2 "module top;\n  wire [3:0] x; wire [1:0] q2;\n"
                 "  function [3:0] inv; input [3:0] v; inv = ~v; endfunction\n"
                 "  m2 u[1:0] (.d(inv(x)), .q(q2));\nendmodule\n");
    EXPECT_CALL_CONN("u[1]", "inv", 3, 2); /* the 4-bit result split in two */
    EXPECT_CALL_CONN("u[0]", "inv", 1, 0);
    et_free(&rs);
    run_clean("module m32 (input [31:0] d);\nendmodule\n"
              "module top;\n  wire a, b;\n"
              "  function integer fi; input v; fi = v; endfunction\n"
              "  function one; input v; one = v; endfunction\n"
              "  m32 u[1:0] (.d({fi(a), fi(b)}));\n"
              "  m32 w[31:0] (.d({32{one(a)}}));\nendmodule\n");
}

/* --- outputs (I3) ----------------------------------------------------------------------------- */

#define M_OUT "module mo (input a, output [3:0] q);\nendmodule\n"

static void test_output_not_a_net(void) {
    run_one(M_OUT "module top; wire a; wire [3:0] b;\n  mo u (.a(a), .q(a + b));\nendmodule\n");
    expect_only_error(
        "t.v:4:16: error: output port 'q' of instance 'u' must be connected to a net");
}

static void test_output_constant(void) {
    run_one(M_OUT "module top; wire a;\n  mo u (.a(a), .q(4'd3));\nendmodule\n");
    expect_only_error(
        "t.v:4:16: error: output port 'q' of instance 'u' is connected to a constant");
}

/* I3 on an inout, and a variable-index select is no net lvalue (review m3). */
static void test_output_inout_and_index(void) {
    run_one("module mi (inout [3:0] p);\nendmodule\nmodule top;\n  mi u (.p(4'd3));\n"
            "endmodule\n");
    expect_only_error("t.v:4:9: error: inout port 'p' of instance 'u' is connected to a constant");
    et_free(&rs);
    run_one(M_OUT "module top; wire a; wire [7:0] w; wire [2:0] i;\n  mo u (.a(a), .q(w[i]));\n"
                  "endmodule\n");
    expect_only_error(
        "t.v:4:16: error: output port 'q' of instance 'u' must be connected to a net");
}

/* Nets, constant selects and their concatenations are net lvalues. */
static void test_output_lvalues(void) {
    run_clean(M_OUT "module top; wire a; wire [7:0] w; wire [1:0] p;\n"
                    "  mo u (.a(a), .q(w[5:2]));\n  mo v (.a(a), .q({p, w[1], w[0]}));\n"
                    "endmodule\n");
}

/* --- black boxes (EL-11) ------------------------------------------------------------------- */

static void test_blackbox_module(void) {
    run_one("(* blackbox *) module bb (input [3:0] a, output y);\n  assign y = &a;\nendmodule\n"
            "module top; wire [3:0] x; wire z;\n  bb u (.y(z), .a(x));\nendmodule\n");
    expect_clean();
    et_expect(&rs, ODIN3_LOG_INFO, "t.v:1:16: info: body of black box 'bb' is ignored");
    TEST_ASSERT_EQUAL_STRING("top,bb", module_names());
    odin3_ast_id module = expect_blackbox("bb", 2);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(rs.elab, module, 0).v); /* no parameters */
    expect_port(module, 0, "a input [3:0]");
    expect_port(module, 1, "y output");
    odin3_ast_id inst = instance("u");
    expect_conn(inst, 0, "a", "x"); /* by port, in port order (EL-4 v4.8) */
    expect_conn(inst, 1, "y", "z");
}

/* The INSTANTIATION named n-th (from 0) in node order; 0 when there are fewer. */
static odin3_ast_id nth_instantiation(const char *name, uint32_t n) {
    uint32_t end = odin3_ast_node_end(rs.elab).v;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(rs.elab, node) == ODIN3_AST_INSTANTIATION &&
            strcmp(name_of(node), name) == 0 && n-- == 0) {
            return node;
        }
    }
    return (odin3_ast_id){0};
}

/* A parameterized `(* blackbox *)` definition keeps its name, as Yosys 0.55's module and cells
 * do (PR 2D-3 final review I2, EL-9/EL-11 v5.0): one ports-only MODULE with the defaults' ports
 * and values, written even when no instance uses the defaults; each INSTANTIATION names it and
 * carries its final values; connections are checked against the instance's own widths. */
static void test_blackbox_specializations(void) {
    run_one("(* blackbox *) module pb #(parameter W = 4) (input [W-1:0] a);\nendmodule\n"
            "module top; wire [1:0] x2; wire [2:0] x3; wire [7:0] x8;\n"
            "  pb #(.W(2)) u1 (.a(x2));\n  pb #(.W(3)) u2 (.a(x3));\n"
            "  pb #(.W(2)) u3[3:0] (.a(x8));\nendmodule\n");
    expect_clean();
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_INFO, "black box"));
    TEST_ASSERT_EQUAL_UINT32(0, find(ODIN3_AST_INSTANTIATION, "pb$W=2").v);
    TEST_ASSERT_EQUAL_STRING("top,pb", module_names());
    odin3_ast_id module = expect_blackbox("pb", 1);
    expect_port(module, 0, "a input [3:0]");
    odin3_ast_id params = odin3_ast_child(rs.elab, module, 0);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(rs.elab, params));
    odin3_ast_id decl = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, params, 0), 1);
    TEST_ASSERT_EQUAL_STRING("W", name_of(decl));
    TEST_ASSERT_EQUAL_INT64(4, number_int(odin3_ast_child(rs.elab, decl, 0)));
    static const int64_t widths[] = {2, 3, 2};
    for (uint32_t i = 0; i < 3; i++) {
        odin3_ast_id holder = nth_instantiation("pb", i);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, holder.v);
        odin3_ast_id value = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, holder, 0), 0);
        TEST_ASSERT_EQUAL_STRING("W", name_of(value)); /* the final values, as for a module */
        TEST_ASSERT_EQUAL_INT64(widths[i], number_int(odin3_ast_child(rs.elab, value, 0)));
    }
    TEST_ASSERT_EQUAL_UINT32(0, nth_instantiation("pb", 3).v);
    expect_conn(instance("u3[0]"), 0, "a", "x8[1:0]");
}

/* An instance's connection width is checked against its own specialization's ports. */
static void test_blackbox_specialization_widths(void) {
    run_one("(* blackbox *) module pb #(parameter W = 4) (input [W-1:0] a);\nendmodule\n"
            "module top; wire [2:0] x3;\n  pb #(.W(2)) u[1:0] (.a(x3));\nendmodule\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0,
                                 et_count_with(&rs, ODIN3_LOG_ERROR, "is 3 bits; expected 2 or 4"));
}

/* --- cell types (EL-8's cell-type targets, EL-11) ------------------------------------------ */

#define RAM_TOP(params, extra)                                                                     \
    "module top; wire c;\n  single_port_ram " params " ram (.clk(c));\n" extra "endmodule\n"

/* A parameterized source `(* blackbox *)` of a registered type's name keeps that name whatever
 * its parameters, so stage 2 meets the registered type (IR-7b reuse; review I2). */
static void test_blackbox_param_keeps_celltype_name(void) {
    run_req("(* blackbox *) module single_port_ram #(parameter ADDR_WIDTH = 1) (input clk);\n"
            "endmodule\n" RAM_TOP("#(.ADDR_WIDTH(10))", ""),
            NULL, TECHLIB);
    expect_clean();
    TEST_ASSERT_EQUAL_STRING("top,single_port_ram", module_names());
    TEST_ASSERT_EQUAL_INT64(10, param_value("ADDR_WIDTH"));
}

static void test_celltype_named(void) {
    run_req(RAM_TOP("#(.ADDR_WIDTH(10))", ""), NULL, TECHLIB);
    expect_clean();
    TEST_ASSERT_EQUAL_INT64(10, param_value("ADDR_WIDTH"));
}

static void test_celltype_no_parameter(void) {
    run_req(RAM_TOP("#(.FOO(1))", ""), NULL, TECHLIB);
    expect_only_error("t.v:2:21: error: cell type 'single_port_ram' has no parameter 'FOO'");
}

static void test_celltype_defparam(void) {
    run_req(RAM_TOP("#(.ADDR_WIDTH(10))", "  defparam ram.DATA_WIDTH = 8;\n"), NULL, TECHLIB);
    expect_clean();
    TEST_ASSERT_EQUAL_INT64(10, param_value("ADDR_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(8, param_value("DATA_WIDTH"));
}

static void test_celltype_project_wins(void) {
    et_src srcs[] = {{"t.v", RAM_TOP("#(.ADDR_WIDTH(10))", "")}};
    et_src proj = {"p.o3proj", "file verilog t.v\ntop top\nparam top.ram.ADDR_WIDTH 12\n"};
    et_req req = {srcs, 1, TECHLIB "; read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    expect_clean();
    TEST_ASSERT_EQUAL_INT64(12, param_value("ADDR_WIDTH"));
}

static void test_celltype_not_integer(void) {
    run_req(RAM_TOP("#(.ADDR_WIDTH(4'bx010))", ""), NULL, TECHLIB);
    expect_only_error("t.v:2:21: error: value of parameter 'ADDR_WIDTH' of cell type "
                      "'single_port_ram' is not an integer");
}

/* The params LIST is in the type's parameter order whatever the source's (review m3). */
static void test_celltype_param_order(void) {
    run_req(RAM_TOP("#(.DATA_WIDTH(8), .ADDR_WIDTH(4))", ""), NULL, TECHLIB);
    expect_clean();
    odin3_ast_id list =
        odin3_ast_child(rs.elab, find(ODIN3_AST_INSTANTIATION, "single_port_ram"), 0);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, list));
    TEST_ASSERT_EQUAL_STRING("ADDR_WIDTH", name_of(odin3_ast_child(rs.elab, list, 0)));
    TEST_ASSERT_EQUAL_STRING("DATA_WIDTH", name_of(odin3_ast_child(rs.elab, list, 1)));
}

/* Ordered values map to the type's parameters in declaration order, and are written named. */
static void test_celltype_ordered(void) {
    run_req(RAM_TOP("#(9, 4)", ""), NULL, TECHLIB);
    expect_clean();
    TEST_ASSERT_EQUAL_INT64(9, param_value("ADDR_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(4, param_value("DATA_WIDTH"));
}

static void test_celltype_errors(void) {
    run_req(RAM_TOP("#(1, 2, 3)", ""), NULL, TECHLIB);
    expect_only_error("t.v:2:27: error: too many parameter values for cell type 'single_port_ram' "
                      "(3 given, 2 parameters)");
    et_free(&rs);
    run_req("module top; wire c;\n  single_port_ram ram (.clk(c), .q(c));\nendmodule\n", NULL,
            TECHLIB);
    expect_only_error("t.v:2:33: error: cell type 'single_port_ram' has no port 'q'");
    et_free(&rs);
    run_req(RAM_TOP("", "  defparam ram.x.W = 1;\n"), NULL, TECHLIB);
    expect_only_error("t.v:3:12: error: defparam target 'ram.x.W' does not name a parameter of an "
                      "instance below this module");
    et_free(&rs);
    run_req("module top; wire c; wire [3:0] o;\n"
            "  single_port_ram #(.DATA_WIDTH(4)) ram (.clk(c), .out(4'd0));\nendmodule\n",
            NULL, TECHLIB);
    expect_only_error(
        "t.v:2:51: error: output port 'out' of instance 'ram' is connected to a constant");
}

/* --- implicit black boxes (--auto-blackbox) ------------------------------------------------ */

static const odin3_elab_opts k_auto = {.auto_blackbox = true};

static void test_implicit_union(void) {
    run_req("module top; wire x, y, x2, z;\n  foo u1 (.a(x), .b(y));\n"
            "  foo u2 (.a(x2), .c(z));\nendmodule\n",
            &k_auto, NULL);
    expect_clean();
    TEST_ASSERT_EQUAL_STRING("top,foo", module_names());
    odin3_ast_id module = expect_blackbox("foo", 3);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(rs.elab, module, 0).v);
    expect_port(module, 0, "a inout");
    expect_port(module, 1, "b inout");
    expect_port(module, 2, "c inout");
    odin3_ast_id one = instance("u1");
    expect_conn(one, 0, "a", "x"); /* one per port of the union, in its order */
    expect_conn(one, 1, "b", "y");
    expect_conn(one, 2, "c", "");
    expect_conn(instance("u2"), 1, "b", "");
    expect_conn(instance("u2"), 2, "c", "z");
}

/* An implicit black box's port takes its first connection's width; a port no connection gives
 * a width to is 1 bit; the union covers instances in other modules too. */
static void test_implicit_widths(void) {
    run_req("module sub; wire [5:0] s;\n  foo f (.d(s));\nendmodule\n"
            "module top; wire [3:0] x;\n  foo u (.a(x), .e());\n  sub v ();\nendmodule\n",
            &k_auto, NULL);
    expect_clean();
    TEST_ASSERT_EQUAL_STRING("top,sub,foo", module_names());
    odin3_ast_id module = expect_blackbox("foo", 3);
    expect_port(module, 0, "d inout [5:0]"); /* the union in source order */
    expect_port(module, 1, "a inout [3:0]");
    expect_port(module, 2, "e inout");
    expect_conn(instance("u"), 0, "d", "");
}

/* An array instance's first connection gives an implicit port one element's width when the
 * element count divides it (else the whole width, broadcast); a later scalar instance then
 * connects that width (PR 2D-3 final review m3). */
static void test_implicit_width_from_array(void) {
    run_req("module top; wire [7:0] bus8; wire [1:0] x2; wire clk;\n"
            "  foo u[3:0] (.a(bus8), .c(clk));\n  foo v (.a(x2), .c(clk));\nendmodule\n",
            &k_auto, NULL);
    expect_clean();
    odin3_ast_id module = expect_blackbox("foo", 2);
    expect_port(module, 0, "a inout [1:0]");
    expect_port(module, 1, "c inout");
}

/* A black box's body adds no port to an implicit black box's union (Task 12 review n3, Q10). */
static void test_blackbox_body_not_in_union(void) {
    run_req("(* blackbox *) module bb (input a);\n  foo f (.zz(a));\nendmodule\n"
            "module top; wire x;\n  bb b (.a(x));\n  foo u (.p(x));\nendmodule\n",
            &k_auto, NULL);
    expect_clean();
    TEST_ASSERT_EQUAL_STRING("top,bb,foo", module_names());
    expect_port(expect_blackbox("foo", 1), 0, "p inout");
}

static bool port_signed(odin3_ast_id module, uint32_t pos) {
    odin3_ast_id port = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, module, 1), pos);
    return (odin3_ast_flags(rs.elab, port) & ODIN3_AST_F_PORT_DECL_SIGNED) != 0;
}

/* The symbol the binder made for port of the MODULE named module (in library work). */
static const odin3_symbol *bound_port(const char *module, const char *port) {
    const odin3_symtab *st = odin3_ast_symtab(rs.elab);
    odin3_scope_id lib = odin3_scope_first_child(st, (odin3_scope_id){1});
    odin3_scope_id box = odin3_scope_child(st, lib, et_intern(&rs, module), ODIN3_SCOPE_NO_INDEX);
    const odin3_symbol *sym =
        odin3_symbol_get(st, odin3_symtab_lookup_local(st, box, et_intern(&rs, port)));
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, port);
    return sym;
}

/* A black-box port keeps its SIGNED flag, in its PORT_DECL and in the symbol the binder makes
 * (Task 12 review n3, Q11; divergence #23's stage-2 side). */
static void test_blackbox_port_signed(void) {
    run_one("(* blackbox *) module bb (input signed [3:0] a, inout signed io, output y);\n"
            "endmodule\nmodule top; wire [3:0] x; wire z, w;\n  bb u (.a(x), .io(z), .y(w));\n"
            "endmodule\n");
    expect_clean();
    odin3_ast_id module = expect_blackbox("bb", 3);
    expect_port(module, 1, "io inout");
    TEST_ASSERT_TRUE(port_signed(module, 0));
    TEST_ASSERT_TRUE(port_signed(module, 1));
    TEST_ASSERT_FALSE(port_signed(module, 2));
    const odin3_symbol *io = bound_port("bb", "io");
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SYM_F_PORT | ODIN3_SYM_F_SIGNED, io->flags);
    TEST_ASSERT_EQUAL_UINT8(1 + ODIN3_AST_PORT_DECL_INOUT, io->dir);
}

static void test_implicit_needs_named(void) {
    run_req("module top; wire x;\n  foo u3 (x);\nendmodule\n", &k_auto, NULL);
    expect_only_error("t.v:2:7: error: implicit black box 'foo' needs named connections");
}

static void test_implicit_width_mismatch(void) {
    run_req("module top; wire [3:0] x4; wire [7:0] x8;\n  foo u1 (.a(x4));\n"
            "  foo u2 (.a(x8));\nendmodule\n",
            &k_auto, NULL);
    expect_only_error("t.v:3:11: error: implicit black box 'foo': port 'a' is 4 bits at its first "
                      "instance; this connection is 8 bits");
}

/* --- the instance cap (point (aa)) --------------------------------------------------------- */

static void test_instance_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_instances = 4;
    odin3_elab_test_set_limits(&limits);
    run_one("module m (input a);\nendmodule\nmodule top;\n  wire [7:0] w;\n"
            "  m u[7:0] (.a(w));\n  nope later ();\nendmodule\n");
    expect_only_error("t.v:5:3: error: elaboration makes more than 4 instances");
    et_free(&rs);
    run_clean("module m (input a);\nendmodule\nmodule top;\n  wire [3:0] w;\n"
              "  m u[3:0] (.a(w));\nendmodule\n"); /* exactly the cap */
}

/* A cell type's port sized by a parameter the instance does not give is inferred in stage 2: an
 * array connection then goes whole (no width error); a given one slices. */
static void test_celltype_array(void) {
    run_req("module top; wire c; wire [15:0] d16; wire [9:0] a10;\n"
            "  single_port_ram #(.ADDR_WIDTH(5)) r[1:0] (.clk(c), .data(d16), .addr(a10));\n"
            "endmodule\n",
            NULL, TECHLIB);
    expect_clean();
    expect_conn(instance("r[1]"), 1, "data", "d16");
    expect_conn(instance("r[1]"), 2, "addr", "a10[9:5]");
    expect_conn(instance("r[0]"), 2, "addr", "a10[4:0]");
    et_free(&rs);
    run_req("module top; wire [7:0] a8, b8; wire [15:0] o16;\n"
            "  multiply #(.A_WIDTH(4), .B_WIDTH(4)) m[1:0] (.a(a8), .b(b8), .out(o16));\n"
            "  multiply #(.A_WIDTH(4)) n[1:0] (.a(a8), .b(b8), .out(o16));\nendmodule\n",
            NULL, TECHLIB);
    expect_clean();
    expect_conn(instance("m[1]"), 2, "out", "o16[15:8]"); /* A_WIDTH + B_WIDTH: all given */
    expect_conn(instance("n[1]"), 2, "out", "o16");       /* B_WIDTH is inferred: whole */
}

/* --- out of memory ------------------------------------------------------------------------- */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

/* One step of the sweep: the allocation limit and the parsed store's node count to keep. */
typedef struct oom_at {
    long limit;
    uint32_t nodes;
} oom_at;

static odin3_status oom_step(const oom_at *at) {
    odin3_ast *elab = NULL;
    odin3_util_set_alloc_fail_after(at->limit);
    odin3_status st = odin3_elab_stage1(&rs.ctx, rs.parsed, &rs.opts, &elab);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_TRUE(st == ODIN3_OK ? elab != NULL : elab == NULL);
    TEST_ASSERT_EQUAL_UINT32(at->nodes, odin3_ast_node_end(rs.parsed).v);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(elab, ODIN3_CHECK_FULL));
    }
    odin3_ast_destroy(elab);
    return st;
}

/* Ordered connections, arrays (module, gate, sliced concatenation), a black box, a cell type with
 * a defparam and an implicit black box: every allocation fails in turn until OOM_SETTLE runs in a
 * row succeed. */
static void test_oom(void) {
    run_req(M2 "(* blackbox *) module bb #(parameter W = 1) (input [W-1:0] a);\n"
               "  assign q = a;\nendmodule\n"
               "module top; wire c; wire [7:0] bus; wire [1:0] q2, y2, a2; wire [3:0] p, r;\n"
               "  m2 u[3:0] (bus, q2);\n  m2 v[1:0] (.d({p[1:0], r[3:2]}), .q(q2));\n"
               "  and g[1:0] (y2, a2, 2'b01);\n  bb #(4) b (p);\n"
               "  single_port_ram #(10) ram (.clk(c));\n  defparam ram.DATA_WIDTH = 8;\n"
               "  foo f (.a(p), .b(c));\nendmodule\n",
            &k_auto, TECHLIB);
    expect_clean();
    uint32_t nodes = odin3_ast_node_end(rs.parsed).v;
    odin3_log_set_sink(quiet_sink, NULL);
    int failed = 0;
    int settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        oom_at at = {limit, nodes};
        bool oom = oom_step(&at) == ODIN3_ERR_NO_MEMORY;
        failed += oom ? 1 : 0;
        settled = oom ? 0 : settled + 1;
    }
    TEST_ASSERT_EQUAL_INT(OOM_SETTLE, settled);
    TEST_ASSERT_TRUE(failed > 100);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.parsed, ODIN3_CHECK_FULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ordered_with_empty);
    RUN_TEST(test_named_in_port_order);
    RUN_TEST(test_named_empty);
    RUN_TEST(test_no_port);
    RUN_TEST(test_connected_twice);
    RUN_TEST(test_too_many_connections);
    RUN_TEST(test_mixed_connections);
    RUN_TEST(test_unnamed_position);
    RUN_TEST(test_portless_instance);
    RUN_TEST(test_array_slices);
    RUN_TEST(test_array_width_error);
    RUN_TEST(test_gate_array);
    RUN_TEST(test_ascending_array);
    RUN_TEST(test_array_slice_forms);
    RUN_TEST(test_array_function_call);
    RUN_TEST(test_output_not_a_net);
    RUN_TEST(test_output_constant);
    RUN_TEST(test_output_inout_and_index);
    RUN_TEST(test_output_lvalues);
    RUN_TEST(test_blackbox_module);
    RUN_TEST(test_blackbox_specializations);
    RUN_TEST(test_blackbox_specialization_widths);
    RUN_TEST(test_blackbox_param_keeps_celltype_name);
    RUN_TEST(test_celltype_named);
    RUN_TEST(test_celltype_no_parameter);
    RUN_TEST(test_celltype_defparam);
    RUN_TEST(test_celltype_project_wins);
    RUN_TEST(test_celltype_not_integer);
    RUN_TEST(test_celltype_param_order);
    RUN_TEST(test_celltype_ordered);
    RUN_TEST(test_celltype_errors);
    RUN_TEST(test_celltype_array);
    RUN_TEST(test_implicit_union);
    RUN_TEST(test_implicit_widths);
    RUN_TEST(test_implicit_width_from_array);
    RUN_TEST(test_blackbox_body_not_in_union);
    RUN_TEST(test_blackbox_port_signed);
    RUN_TEST(test_implicit_needs_named);
    RUN_TEST(test_implicit_width_mismatch);
    RUN_TEST(test_instance_cap);
    RUN_TEST(test_oom);
    return UNITY_END();
}
