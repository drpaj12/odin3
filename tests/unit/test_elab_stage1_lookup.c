/*
 * test_elab_stage1_lookup.c — 2D Task 10: stage 1's context and entry (EL-2, EL-3), module
 * lookup (EL-6), top selection (EL-7), the pending kinds of plan point (l), the ownership of the
 * parsed store (review I10), odin3_ast_get_library_units_from and an OOM sweep. Sources are parsed
 * by 2C (elab_test.h); every diagnostic is asserted as the whole message through a capturing sink.
 *
 * Definitions are located at their keyword (the MODULE/UDP_DECL/CONFIG_DECL node's loc): the
 * parsed store records no location for a definition's name (2C spec §4.2 step 4), and the
 * handover cases Q7.err and S2.err pin the keyword.
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/verilog/elab.h"
#include "frontends/verilog/elab1.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

#define PENDING "elaboration is not available yet (2D)"

enum { OOM_LIMIT = 200000, OOM_SETTLE = 64, SCRIPT_BUF = 1024 };

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_NET_DECL, false);
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_MODULE, false);
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_UDP_DECL, false);
    odin3_elab_test_set_hooks(NULL);
    odin3_elab_test_set_limits(NULL);
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = NULL;
    odin3_pass_set_options(opts);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* --- helpers ------------------------------------------------------------------------------- */

static void set_cli_top(const char *top) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = top;
    odin3_pass_set_options(opts);
}

/* A stage-1 run over files with no pre-script or project. */
static void run_files(const et_src *srcs, uint32_t n, const odin3_elab_opts *opts) {
    et_req req = {srcs, n, NULL, NULL, opts};
    et_stage1(&req, &rs);
}

static void run_one(const char *path, const char *text) {
    et_src src = {path, text};
    run_files(&src, 1, NULL);
}

/* A fresh stage-1 context over the result's parsed store with its definitions collected. */
static odin3_elab1 *context(void) {
    odin3_elab1 *e1 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_create(&rs.ctx, rs.parsed, &rs.opts, &e1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_collect_defs(e1));
    return e1;
}

static uint32_t intern(const char *text) {
    return et_intern(&rs, text);
}

/* What name instantiated in library resolves to, with a fresh context. */
static odin3_elab_res resolve(const char *name, const char *library, odin3_elab_def *def) {
    odin3_elab1 *e1 = context();
    odin3_elab_res res = odin3_elab1_resolve(e1, intern(name), intern(library));
    if (def != NULL && (res.kind == ODIN3_RES_MODULE || res.kind == ODIN3_RES_UDP)) {
        *def = *odin3_elab1_def(e1, res.def);
    }
    odin3_elab1_destroy(e1);
    return res;
}

static const char *str(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(rs.design), id);
}

/* --- lookup order (EL-6) --------------------------------------------------------------------- */

static const et_src k_two_libs[] = {
    {"t.v", "module t;\n  sub u ();\nendmodule\nmodule sub;\nendmodule\n"},
    {"l2.v", "module sub;\nendmodule\n"},
};

/* sub in work and lib2: an instance in a work module takes work.sub, no info line. */
static void test_lookup_own_library_first(void) {
    et_src proj = {"p.o3proj", "file verilog t.v\nfile verilog l2.v library lib2\n"};
    et_req req = {k_two_libs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_INFO, "found in libraries"));
    odin3_elab_def def;
    odin3_elab_res res = resolve("sub", "work", &def);
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, res.kind);
    TEST_ASSERT_EQUAL_STRING("work", str(def.library));
    TEST_ASSERT_FALSE(def.library_unit);
    res = resolve("sub", "lib2", &def);
    TEST_ASSERT_EQUAL_STRING("lib2", str(def.library));
}

/* sub only in lib2 and lib3: the first in store order, with the info line once. */
static void test_lookup_several_libraries(void) {
    static const et_src srcs[] = {
        {"t.v", "module t;\n  sub u1 ();\n  sub u2 ();\nendmodule\n"},
        {"l2.v", "module sub;\nendmodule\n"},
        {"l3.v", "module sub;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nfile verilog l2.v library lib2\n"
                               "file verilog l3.v library lib3\n"};
    et_req req = {srcs, 3, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_INFO,
              "module 'sub' found in libraries 'lib2', 'lib3'; using 'lib2.sub'");
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_INFO, "found in libraries"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    odin3_elab_def def;
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("sub", "work", &def).kind);
    TEST_ASSERT_EQUAL_STRING("lib2", str(def.library));
}

/* A -v library-unit module is taken only when no design unit defines the name. */
static void test_lookup_library_unit_last(void) {
    static const et_src srcs[] = {
        {"t.v", "module t;\n  sub u ();\n  other o ();\nendmodule\nmodule sub;\nendmodule\n"},
        {"cells.v", "module sub;\nendmodule\nmodule other;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nlibfile verilog cells.v\n"};
    et_req req = {srcs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    odin3_elab_def def;
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("sub", "work", &def).kind);
    TEST_ASSERT_FALSE(def.library_unit);
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("other", "work", &def).kind);
    TEST_ASSERT_TRUE(def.library_unit);
    TEST_ASSERT_EQUAL_STRING("work", str(def.library));
}

/* One other library defines it: no info line (it names several libraries only). */
static void test_lookup_single_other_library(void) {
    static const et_src srcs[] = {
        {"t.v", "module t;\n  sub u ();\nendmodule\n"},
        {"l2.v", "module sub;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nfile verilog l2.v library lib2\n"};
    et_req req = {srcs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_INFO, "found in libraries"));
    odin3_elab_def def;
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("sub", "work", &def).kind);
    TEST_ASSERT_EQUAL_STRING("lib2", str(def.library));
}

/* Every instantiation of a body is resolved, each unresolved one reported at itself. */
static void test_lookup_every_instance(void) {
    run_one("t.v", "module t;\n  foo u1 ();\n  bar u2 ();\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:2:3: error: unresolved_module: module 'foo' is not defined");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: unresolved_module: module 'bar' is not defined");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, et_real_errors(&rs), et_log(&rs));
}

/* `(* blackbox *)` is true as Yosys reads it: no value or a 1 bit; `= 0` is an ordinary module. */
static void test_blackbox_attribute_value(void) {
    run_one("b.v", "(* blackbox = 0 *)\nmodule b0;\nendmodule\n(* blackbox = 1 *)\nmodule b1;\n"
                   "endmodule\n(* blackbox *)\nmodule b2;\nendmodule\n");
    odin3_elab_def def;
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("b0", "work", &def).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DEF_MODULE, def.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("b1", "work", &def).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DEF_BLACKBOX, def.kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("b2", "work", &def).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DEF_BLACKBOX, def.kind);
    /* b0 is the only candidate: black boxes never are (EL-7). */
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.b0' chosen: the only module no other module instantiates");
}

/* Q7 at the UDP_DECL, once, with or without an instance; the instance resolves to it silently. */
static void test_udp_rejected(void) {
    static const char udp[] = "primitive p (y, a);\n  output y;\n  input a;\n"
                              "  table\n    0 : 1;\n    1 : 0;\n  endtable\nendprimitive\n";
    run_one("p.v", udp);
    et_expect(&rs, ODIN3_LOG_ERROR, "p.v:1:1: error: user-defined primitive 'p' is not supported");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
    et_free(&rs);
    char text[SCRIPT_BUF];
    int got = snprintf(text, sizeof text,
                       "%smodule t (input a, output y);\n  p g (y, a);\nendmodule\n", udp);
    TEST_ASSERT_TRUE(got > 0 && got < SCRIPT_BUF);
    run_one("p.v", text);
    et_expect(&rs, ODIN3_LOG_ERROR, "p.v:1:1: error: user-defined primitive 'p' is not supported");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_real_errors(&rs), et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_UDP, resolve("p", "work", NULL).kind);
}

static void test_config_rejected(void) {
    run_one("c.v", "module t;\nendmodule\nconfig c;\n  design work.t;\nendconfig\n");
    et_expect(&rs, ODIN3_LOG_ERROR, "c.v:3:1: error: configuration 'c' is not supported");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

/* A library unit's UDP: Q7 once an instance resolves to it, never for one nothing instantiates. */
static void test_library_udp_rejected_when_used(void) {
    static const et_src srcs[] = {
        {"t.v", "module t (input a, output y);\n  p g1 (y, a);\n  p g2 (y, a);\nendmodule\n"},
        {"cells.v", "primitive q (y, a);\n  output y;\n  input a;\n  table\n    0 : 1;\n"
                    "  endtable\nendprimitive\nprimitive p (y, a);\n  output y;\n  input a;\n"
                    "  table\n    0 : 1;\n  endtable\nendprimitive\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nlibfile verilog cells.v\n"};
    et_req req = {srcs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "cells.v:8:1: error: user-defined primitive 'p' is not supported");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_real_errors(&rs), et_log(&rs));
}

/* A forced-pending UDP_DECL gives its pending line instead of Q7 (point (l)). */
static void test_pending_forced_udp(void) {
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_UDP_DECL, true);
    run_one("p.v", "primitive p (y, a);\n  output y;\n  input a;\n  table\n    0 : 1;\n"
                   "  endtable\nendprimitive\nmodule t;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR, "p.v:1:1: error: " PENDING ": 'UDP_DECL'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
}

/* --- cell types, unresolved and implicit black boxes ---------------------------------------- */

static void test_celltype_resolves(void) {
    et_src src = {"t.v", "module t (input c);\n  single_port_ram #(.ADDR_WIDTH(10)) r (.clk(c));\n"
                         "endmodule\n"};
    et_req req = {&src, 1, "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib", NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    odin3_elab_res res = resolve("single_port_ram", "work", NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_CELLTYPE, res.kind);
    odin3_celltype_id want;
    TEST_ASSERT_TRUE(odin3_celltype_find(rs.design, intern("single_port_ram"), &want));
    TEST_ASSERT_EQUAL_UINT32(want.v, res.type.v);
}

/* A `$` name never resolves to a built-in cell type. */
static void test_dollar_name_unresolved(void) {
    run_one("t.v",
            "module t (input a, output y);\n  wire w;\n  \\$add u (.A(a), .Y(y));\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: unresolved_module: module '$add' is not defined");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_real_errors(&rs), et_log(&rs));
}

static void test_undefined_unresolved(void) {
    run_one("t.v", "module t (input a, output y);\n  foo u (.a(a), .y(y));\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:2:3: error: unresolved_module: module 'foo' is not defined");
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_NONE, resolve("foo", "work", NULL).kind);
}

static void test_auto_blackbox(void) {
    odin3_elab_opts opts = {.auto_blackbox = true};
    et_src src = {"t.v", "module t (input a, output y);\n  foo u1 (.a(a), .y(y));\n"
                         "  foo u2 (.a(a), .y(y));\nendmodule\n"};
    run_files(&src, 1, &opts);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
    et_expect(&rs, ODIN3_LOG_INFO,
              "module 'foo' is not defined; it becomes an implicit black box (--auto-blackbox)");
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_INFO, "implicit black box"));
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_IMPLICIT_BLACKBOX, resolve("foo", "work", NULL).kind);
}

/* --- names taken by an earlier read (EL-6) --------------------------------------------------- */

static void test_taken_hard_cell(void) {
    et_src src = {"a.v", "module adder;\nendmodule\n"};
    et_req req = {&src, 1, "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib", NULL, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "a.v:1:1: error: module 'adder' has the name of cell type 'adder' registered by an "
              "earlier read (hard cell); rename the module");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
}

/* A taken module is not elaborated: its body is not walked (nothing depends on a failed item). */
static void test_taken_body_not_walked(void) {
    et_src src = {"a.v", "module adder;\n  foo u ();\nendmodule\n"};
    et_req req = {&src, 1, "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib", NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_ERROR, "unresolved_module"));
}

static const char k_blif[] = ".model top\n.inputs a\n.outputs y\n.subckt bb a=a y=y\n.end\n\n"
                             ".model bb\n.inputs a\n.outputs y\n.blackbox\n.end\n";

static void test_taken_blackbox(void) {
    static const et_src srcs[] = {{"m.blif", k_blif}, {"b.v", "module bb;\nendmodule\n"}};
    et_req req = {srcs, 2, "read_blif m.blif", NULL, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "b.v:1:1: error: module 'bb' has the name of cell type 'bb' registered by an "
              "earlier read (black box); rename the module");
}

/* A module of an earlier read: duplicate_module naming that read's run (EL-6, F27). */
static void test_taken_module_of_earlier_read(void) {
    static const et_src srcs[] = {{"m.blif", k_blif}, {"t.v", "module top;\nendmodule\n"}};
    et_req req = {srcs, 2, "read_blif m.blif", NULL, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:1:1: error: duplicate_module: module 'top' is already defined by an earlier "
              "read (run 1)");
}

/* A `(* blackbox *)` module reuses a registered type (IR-7b): no taken-name error. */
static void test_blackbox_module_reuses_cell(void) {
    et_src src = {"t.v", "module t;\n  adder u ();\nendmodule\n"
                         "(* blackbox *)\nmodule adder (input a, b, cin, output cout, sumout);\n"
                         "endmodule\n"};
    et_req req = {&src, 1, "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib", NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_count_with(&rs, ODIN3_LOG_ERROR, "has the name"),
                                     et_log(&rs));
    odin3_elab_def def;
    TEST_ASSERT_EQUAL_INT(ODIN3_RES_MODULE, resolve("adder", "work", &def).kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_DEF_BLACKBOX, def.kind);
}

/* --- the top (EL-7) -------------------------------------------------------------------------- */

static void test_top_single_candidate(void) {
    run_one("t.v", "module t;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.t' chosen: the only module no other module instantiates");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, rs.errors);
    TEST_ASSERT_NOT_NULL(rs.elab);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_FORM_ELABORATED, odin3_ast_form_of(rs.elab));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
}

static void corpus_case(const char *name) {
    char script[SCRIPT_BUF];
    int got = snprintf(script, sizeof script, "read_project %s/%s/project.o3proj",
                       ODIN3_PROJECT_FIXTURES, name);
    TEST_ASSERT_TRUE(got > 0 && got < SCRIPT_BUF);
    et_req req = {NULL, 0, script, NULL, NULL};
    et_stage1(&req, &rs);
}

static void test_top_ambiguous(void) {
    corpus_case("neg_ambiguous_top");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "ambiguous_top: no top given and several modules are not instantiated: "
              "'work.alpha', 'work.beta'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_free(&rs);
    static const et_src srcs[] = {{"a.v", "module a;\nendmodule\n"},
                                  {"b.v", "module b;\nendmodule\n"}};
    run_files(srcs, 2, NULL);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "ambiguous_top: no top given and several modules are not instantiated: 'work.a', "
              "'work.b'");
}

static void test_top_none(void) {
    corpus_case("neg_no_top");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "no_top: no top given and every module is instantiated by another");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_free(&rs);
    run_one("e.v", "// no module\n");
    et_expect(&rs, ODIN3_LOG_ERROR, "no_top: no top given and no module is defined");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
}

static void test_top_unknown(void) {
    set_cli_top("nope");
    run_one("t.v", "module t;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_ERROR, "unknown_top: 'nope' is not a defined module");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
}

static void test_top_unknown_project(void) {
    et_src src = {"t.v", "module t;\nendmodule\n"};
    et_src proj = {"p.o3proj", "file verilog t.v\ntop nope\n"};
    et_req req = {&src, 1, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "p.o3proj:2:1: error: unknown_top: 'nope' is not a defined module");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
}

/* --top wins over the project's top. */
static void test_top_cli_beats_project(void) {
    set_cli_top("t");
    et_src src = {"t.v", "module t;\nendmodule\n"};
    et_src proj = {"p.o3proj", "file verilog t.v\ntop nope\n"};
    et_req req = {&src, 1, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("t", str(rs.opts.top));
    TEST_ASSERT_EQUAL_UINT32(0, rs.opts.top_at.v);
}

static const et_src k_sub_two_libs[] = {
    {"l2.v", "module sub;\nendmodule\n"},
    {"l3.v", "module sub;\nendmodule\n"},
};

static void sub_two_libs(const char *top) {
    set_cli_top(top);
    et_src proj = {"p.o3proj", "file verilog l2.v library lib2\nfile verilog l3.v library lib3\n"};
    et_req req = {k_sub_two_libs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
}

static void test_top_several_libraries(void) {
    sub_two_libs("sub");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "ambiguous_top: 'sub' is defined in several libraries: 'lib2.sub', 'lib3.sub'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_free(&rs);
    sub_two_libs("lib3.sub");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    /* The top is lib3's: the elaborated MODULE copies its range, in l3.v. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(UNIT:sub @l3.v:1:1"), rs.dump.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(MODULE:sub @l3.v:1:1"), rs.dump.data);
    TEST_ASSERT_NULL(strstr(rs.dump.data, "l2.v"));
}

static void test_top_unreachable_info(void) {
    set_cli_top("t");
    run_one("t.v", "module u1;\nendmodule\nmodule t;\nendmodule\nmodule u2;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_INFO, "not instantiated under the top, not elaborated: 'u1', 'u2'");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
}

/* An instantiation in a generate branch that is never selected still counts (divergence 20). */
static void test_top_generate_branch_counts(void) {
    run_one("t.v", "module t;\n  generate\n    if (0) begin : g\n      sub u ();\n    end\n"
                   "  endgenerate\nendmodule\nmodule sub;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.t' chosen: the only module no other module instantiates");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
}

/* An instantiation in each generate form counts: a loop body, a case body, a plain region. */
static void expect_t_only_candidate(const char *text) {
    run_one("t.v", text);
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.t' chosen: the only module no other module instantiates");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
}

static void test_top_generate_for_counts(void) {
    expect_t_only_candidate("module t;\n  genvar i;\n  generate\n"
                            "    for (i = 0; i < 2; i = i + 1) begin : g\n      sub u ();\n"
                            "    end\n  endgenerate\nendmodule\nmodule sub;\nendmodule\n");
}

static void test_top_generate_case_counts(void) {
    expect_t_only_candidate("module t;\n  generate\n    case (1)\n      0: sub u ();\n"
                            "      default: ;\n    endcase\n  endgenerate\nendmodule\n"
                            "module sub;\nendmodule\n");
}

static void test_top_generate_region_counts(void) {
    expect_t_only_candidate("module t;\n  generate\n    sub u ();\n  endgenerate\nendmodule\n"
                            "module sub;\nendmodule\n");
}

/* Library units are never top candidates nor named as not elaborated (DESIGN §4.0). */
static void test_top_library_units_excluded(void) {
    static const et_src srcs[] = {
        {"t.v", "module t;\n  sub u ();\nendmodule\n"},
        {"cells.v", "module sub;\nendmodule\nmodule unused;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nlibfile verilog cells.v\n"};
    et_req req = {srcs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.t' chosen: the only module no other module instantiates");
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_INFO, "not elaborated"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_real_errors(&rs), et_log(&rs));
}

/* A black box is no candidate, but what its body instantiates counts (EL-7 counts every MODULE);
 * given as the top it is an error (EL-4: a black box is never a MODULE). */
static void test_top_blackbox_excluded(void) {
    static const char text[] = "(* blackbox *)\nmodule bb;\n  x u ();\nendmodule\n"
                               "module x;\nendmodule\nmodule t;\nendmodule\n";
    run_one("t.v", text);
    et_expect(&rs, ODIN3_LOG_INFO,
              "top module 'work.t' chosen: the only module no other module instantiates");
    et_expect(&rs, ODIN3_LOG_INFO, "not instantiated under the top, not elaborated: 'bb', 'x'");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    et_free(&rs);
    set_cli_top("bb");
    run_one("t.v", text);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "unknown_top: 'bb' is a black box ((* blackbox *)), not a module to elaborate");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
}

/* A given top takes the design unit over a library unit of the same name. */
static void test_top_given_prefers_design_unit(void) {
    set_cli_top("sub");
    static const et_src srcs[] = {
        {"t.v", "module sub;\nendmodule\nmodule t;\n  other o ();\nendmodule\n"},
        {"cells.v", "module sub;\nendmodule\nmodule other;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nlibfile verilog cells.v\n"};
    et_req req = {srcs, 2, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(MODULE:sub @t.v:1:1"), rs.dump.data);
}

/* --- pending kinds (point (l), ruling C3) ---------------------------------------------------- */

static void test_pending_forced_kind(void) {
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_NET_DECL, true);
    run_one("t.v", "module t;\n  wire a;\n  wire b;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:3: error: " PENDING ": 'NET_DECL'");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab); /* destroyed: no check ever sees an unhandled kind */
}

/* Before Task 15 an always block is pending. */
static void test_pending_always(void) {
    run_one("t.v", "module t;\n  reg r;\n  always r = 1;\nendmodule\n"); /* T6 (Task 16) */
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: always block without an event control is not supported");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
}

/* A kind stage 1 handles, forced pending: its line replaces it (the table is consulted). */
static void test_pending_forced_module(void) {
    odin3_elab_test_pend(ODIN3_ELAB_STAGE1, ODIN3_AST_MODULE, true);
    run_one("t.v", "module t;\n  wire a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:1: error: " PENDING ": 'MODULE'");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

/* A design unit's directive and a reached module's attribute are pending until they land. */
static void test_pending_directive_and_attribute(void) {
    run_one("d.v", "`timescale 1ns/1ps\n(* keep *)\nmodule t;\nendmodule\n");
    et_expect(&rs, ODIN3_LOG_WARN, "d.v:1:1: warning: `timescale is ignored"); /* L18 (Task 16) */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
}

/* What odin3_elab_pending answers for (stage, kind or feature). */
typedef struct pend_case {
    uint8_t stage;
    uint32_t kind;
    bool pending;
} pend_case;

static const pend_case k_pend_cases[] = {
    {ODIN3_ELAB_STAGE1, ODIN3_AST_MODULE, false},
    {ODIN3_ELAB_STAGE1, ODIN3_AST_UNIT, false},
    {ODIN3_ELAB_STAGE1, ODIN3_AST_UDP_DECL, false},
    {ODIN3_ELAB_STAGE1, ODIN3_AST_CONFIG_DECL, false},
    {ODIN3_ELAB_STAGE1, ODIN3_AST_INSTANTIATION, false}, /* landed by Task 11 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_GATE_DECL, false},     /* landed by Task 12 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_GEN_FOR, false},       /* landed by Task 14 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_HIER_NAME, false},     /* landed by Task 14 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_ALWAYS, false},        /* landed by Task 15 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_ATTR, false},
    {ODIN3_ELAB_STAGE1, ODIN3_AST_DELAY, false},     /* landed by Task 16 */
    {ODIN3_ELAB_STAGE1, ODIN3_AST_DIRECTIVE, false}, /* landed by Task 16 */
    {ODIN3_ELAB_STAGE1, ODIN3_PEND_COMB_CALL, false},
    {ODIN3_ELAB_STAGE2, ODIN3_PEND_MEM_WRITE, true},
    {ODIN3_ELAB_STAGE2, ODIN3_AST_MODULE, true},
    {ODIN3_ELAB_STAGE2, ODIN3_AST_DEFPARAM, false}, /* never in an elaborated store */
    {0, ODIN3_AST_LIST, false},
    {ODIN3_ELAB_STAGE2 + 1, ODIN3_AST_LIST, false},
    {ODIN3_ELAB_STAGE1, ODIN3_PEND_END, false},
};

static void test_pending_tables(void) {
    for (size_t i = 0; i < sizeof k_pend_cases / sizeof k_pend_cases[0]; i++) {
        const pend_case *pc = &k_pend_cases[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(pc->pending, odin3_elab_pending(pc->stage, pc->kind),
                                      odin3_ast_kind_name(pc->kind));
    }
    odin3_elab_test_pend(ODIN3_ELAB_STAGE2, ODIN3_AST_DEFPARAM, true);
    TEST_ASSERT_EQUAL_INT(1, odin3_elab_pending(ODIN3_ELAB_STAGE2, ODIN3_AST_DEFPARAM));
    odin3_elab_test_pend(ODIN3_ELAB_STAGE2, ODIN3_AST_DEFPARAM, false);
    TEST_ASSERT_EQUAL_INT(0, odin3_elab_pending(ODIN3_ELAB_STAGE2, ODIN3_AST_DEFPARAM));
}

/* The report: once per kind, features by their text, unlocated without a loc. */
static void test_pending_report(void) {
    run_one("t.v", "module t;\nendmodule\n");
    bool seen[ODIN3_PEND_END] = {false};
    uint32_t before = rs.errors;
    odin3_elab_pend_report(rs.design, (odin3_loc){0}, ODIN3_PEND_COMB_CALL, seen);
    odin3_elab_pend_report(rs.design, (odin3_loc){0}, ODIN3_PEND_COMB_CALL, seen);
    odin3_elab_pend_report(rs.design, (odin3_loc){0}, ODIN3_PEND_REGS_VAR_READ, seen);
    odin3_elab_pend_report(rs.design, (odin3_loc){0}, ODIN3_PEND_END, seen);
    odin3_elab_pend_report(rs.design, (odin3_loc){0}, ODIN3_PEND_MEM_WRITE, NULL);
    TEST_ASSERT_EQUAL_UINT32(before + 2, rs.errors);
    et_expect(&rs, ODIN3_LOG_ERROR, PENDING ": 'continuous-context call'");
    et_expect(&rs, ODIN3_LOG_ERROR, PENDING ": 'variable index into a register array'");
}

/* --- diagnostics helpers --------------------------------------------------------------------- */

static void test_warn_once_and_cap(void) {
    run_one("t.v", "module t;\nendmodule\n");
    odin3_elab1 *e1 = context();
    odin3_ast_id module = odin3_ast_child(rs.parsed, odin3_ast_root(rs.parsed, 0), 0);
    odin3_elab1_warn_once(e1, module, "w %d", 1);
    odin3_elab1_warn_once(e1, module, "w %d", 2);
    odin3_elab1_warn_once(e1, module, "v");
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:1:1: warning: w 1");
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:1:1: warning: v");
    TEST_ASSERT_EQUAL_UINT32(2, rs.warnings);
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_errors = 2;
    odin3_elab_test_set_limits(&limits);
    odin3_elab1_destroy(e1);
    e1 = context();
    odin3_loc at = odin3_ast_loc(rs.parsed, module);
    for (int i = 0; i < 4; i++) {
        odin3_elab1_error(e1, at, "e%d", i);
    }
    TEST_ASSERT_TRUE(odin3_elab1_stopped(e1));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_elab1_errors(e1));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:1: error: e1");
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:1: error: too many errors; elaboration stopped after 2");
    TEST_ASSERT_EQUAL_INT(-1, et_find(&rs, ODIN3_LOG_ERROR, "t.v:1:1: error: e3"));
    odin3_elab1_destroy(e1);
}

/* --- ownership of the parsed store (review I10) ---------------------------------------------- */

typedef struct body_probe {
    odin3_design *design;
    uint32_t calls;
    bool parsed_held;
} body_probe;

static void probe_body(void *user, odin3_ast_id module) {
    body_probe *probe = user;
    (void)module;
    probe->calls++;
    odin3_ast *found = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_find_ast(probe->design, (odin3_passrun_id){1},
                                                          ODIN3_AST_FORM_PARSED, &found));
    probe->parsed_held = found != NULL;
}

/* read_verilog e.v through the pass manager, the probe on stage 2's start. */
static odin3_status read_through_pass(odin3_keep_ast keep, body_probe *probe) {
    et_src src = {"e.v", "module e;\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, keep));
    *probe = (body_probe){rs.design, 0, true};
    odin3_elab_test_hooks hooks = {0, probe_body, probe};
    odin3_elab_test_set_hooks(&hooks);
    return odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("e.v"));
}

static void test_ownership_keep_none(void) {
    body_probe probe;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_through_pass(ODIN3_KEEP_AST_NONE, &probe));
    TEST_ASSERT_EQUAL_UINT32(1, probe.calls);
    TEST_ASSERT_FALSE(probe.parsed_held); /* dropped before stage 2 */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(rs.design));
    odin3_design_drop_ast(rs.design, (odin3_passrun_id){1}, ODIN3_AST_FORM_PARSED);
    odin3_design_drop_ast(rs.design, (odin3_passrun_id){1}, ODIN3_AST_FORM_PARSED);
}

static void test_ownership_keep_all(void) {
    body_probe probe;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_through_pass(ODIN3_KEEP_AST_ALL, &probe));
    TEST_ASSERT_TRUE(probe.parsed_held);
    odin3_ast *found = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_find_ast(rs.design, (odin3_passrun_id){1},
                                                          ODIN3_AST_FORM_PARSED, &found));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_find_ast(rs.design, (odin3_passrun_id){1},
                                                          ODIN3_AST_FORM_ELABORATED, &found));
    TEST_ASSERT_NOT_NULL(found);
    odin3_design_drop_ast(rs.design, (odin3_passrun_id){1}, ODIN3_AST_FORM_PARSED);
    odin3_design_drop_ast(rs.design, (odin3_passrun_id){1}, ODIN3_AST_FORM_PARSED);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_ast_count(rs.design)); /* ALL keeps both */
}

/* --keep-ast (ELABORATED) keeps the elaborated store only. */
static void test_ownership_keep_elaborated(void) {
    body_probe probe;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_through_pass(ODIN3_KEEP_AST_ELABORATED, &probe));
    TEST_ASSERT_FALSE(probe.parsed_held);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_ast_count(rs.design));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_FORM_ELABORATED,
                          odin3_ast_form_of(odin3_design_ast_at(rs.design, 0)));
}

/* --- ABI ------------------------------------------------------------------------------------- */

static void test_abi_library_units_from(void) {
    static const et_src srcs[] = {
        {"a.v", "module a;\n  c u ();\nendmodule\n"},
        {"b.v", "module b;\nendmodule\n"},
        {"lib.v", "module c;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog a.v\nfile verilog b.v\nlibfile verilog lib.v\n"};
    et_req req = {srcs, 3, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    uint32_t index = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_library_units_from(rs.parsed, &index));
    TEST_ASSERT_EQUAL_UINT32(2, index);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_library_units_from(NULL, &index));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_library_units_from(rs.parsed, NULL));
    et_free(&rs);
    run_one("t.v", "module t;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_library_units_from(rs.parsed, &index));
    TEST_ASSERT_EQUAL_UINT32(0, index);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_library_units_from(rs.elab, &index));
    TEST_ASSERT_EQUAL_UINT32(0, index);
}

/* --- the entry (EL-2, EL-3) ------------------------------------------------------------------ */

static const et_src k_t_empty = {"t.v", "module t;\nendmodule\n"};

/* read_verilog args through the pass manager, src written in a fresh directory. */
static odin3_status pass_on(const et_src *src, const char *args) {
    et_req req = {src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    return odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr(args));
}

static void test_entry_nolatches(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pass_on(&k_t_empty, "--nolatches t.v"));
    const odin3_elab_opts *seen = odin3_elab_test_last_opts();
    TEST_ASSERT_TRUE(seen->nolatches);
    TEST_ASSERT_FALSE(seen->nomem2reg);
    TEST_ASSERT_FALSE(seen->auto_blackbox);
}

static void test_entry_other_flags(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pass_on(&k_t_empty, "--auto-blackbox --nomem2reg t.v"));
    const odin3_elab_opts *seen = odin3_elab_test_last_opts();
    TEST_ASSERT_FALSE(seen->nolatches);
    TEST_ASSERT_TRUE(seen->nomem2reg);
    TEST_ASSERT_TRUE(seen->auto_blackbox);
    TEST_ASSERT_EQUAL_UINT32(0, seen->top);
}

/* EL-2 step 1: --top fills the top read_verilog leaves 0. */
static void test_entry_top_option(void) {
    set_cli_top("t");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pass_on(&k_t_empty, "t.v"));
    TEST_ASSERT_EQUAL_STRING("t", str(odin3_elab_test_last_opts()->top));
}

static void test_entry_unknown_option(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass_on(&k_t_empty, "--bogus t.v"));
    et_expect(&rs, ODIN3_LOG_ERROR, "read_verilog: unknown option '--bogus'");
}

/* A clean stage 1 reaches stage 2's stub: the unlocated text and PARSE (until Task 17). */
static void test_entry_reaches_stub(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pass_on(&k_t_empty, "t.v"));
    et_expect(&rs, ODIN3_LOG_ERROR, PENDING);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_count_with(&rs, ODIN3_LOG_ERROR, PENDING), et_log(&rs));
}

/* The elaborated store of a clean stage 1 passes check FULL; its shape. */
static void test_entry_store_checks(void) {
    set_cli_top("t");
    static const et_src srcs[] = {{"t.v", "module t ();\nendmodule\n"},
                                  {"u.v", "(* keep *)\nmodule u;\nendmodule\n"}};
    run_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    odin3_ast_id unit = odin3_ast_root(rs.elab, 0);
    TEST_ASSERT_EQUAL_STRING("t", odin3_ast_name_str(rs.elab, unit));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(rs.elab, unit));
    odin3_ast_id module = odin3_ast_child(rs.elab, unit, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_MODULE, odin3_ast_kind_of(rs.elab, module));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(rs.elab, module, 0).v); /* no params LIST */
    odin3_ast_id ports = odin3_ast_child(rs.elab, module, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_LIST, odin3_ast_kind_of(rs.elab, ports));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(rs.elab, ports));
}

static void test_misuse(void) {
    run_one("t.v", "module t;\nendmodule\n");
    odin3_ast *out = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_stage1(NULL, rs.parsed, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_stage1(&rs.ctx, NULL, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_stage1(&rs.ctx, rs.parsed, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_stage1(&rs.ctx, rs.elab, NULL, &out));
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab_run(NULL, rs.design, rs.parsed, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab1_options(NULL, NULL, &rs.opts));
    odin3_elab1 *e1 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab1_create(NULL, rs.parsed, NULL, &e1));
    TEST_ASSERT_NULL(e1);
    odin3_elab1_destroy(NULL);
}

/* --- OOM ------------------------------------------------------------------------------------- */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

/* What every step of a sweep must keep: the clean run's status and the parsed node count. */
typedef struct oom_want {
    odin3_status status;
    uint32_t nodes;
} oom_want;

/* One step of the sweep: stage 1 with allocation limit limit; NO_MEMORY with nothing left, or
 * the clean status (a failure the logger absorbed, or past the last allocation); the parsed
 * store's node count unchanged. */
static odin3_status oom_step(const oom_want *want, long limit) {
    odin3_ast *elab = NULL;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_elab_stage1(&rs.ctx, rs.parsed, &rs.opts, &elab);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == want->status || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_TRUE(st == ODIN3_OK ? elab != NULL : elab == NULL);
    TEST_ASSERT_EQUAL_UINT32(want->nodes, odin3_ast_node_end(rs.parsed).v);
    odin3_ast_destroy(elab);
    return st;
}

/* Every allocation of stage 1 fails in turn, until OOM_SETTLE steps in a row reach want; the
 * parsed store's dump is unchanged and it passes check FULL. Returns the NO_MEMORY steps. */
static int oom_sweep(odin3_status want) {
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    odin3_ast_id unit = odin3_ast_root(rs.parsed, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(rs.parsed, unit, &before));
    oom_want want_step = {want, odin3_ast_node_end(rs.parsed).v};
    odin3_log_set_sink(quiet_sink, NULL);
    int failed = 0;
    int settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        bool oom = oom_step(&want_step, limit) == ODIN3_ERR_NO_MEMORY;
        failed += oom ? 1 : 0;
        settled = oom ? 0 : settled + 1;
    }
    TEST_ASSERT_EQUAL_INT(OOM_SETTLE, settled);
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(rs.parsed, unit, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.parsed, ODIN3_CHECK_FULL));
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    return failed;
}

static void test_oom(void) {
    /* Three modules with hierarchy (Task 11: specialized and written cleanly). */
    run_one("t.v", "module t;\n  m u ();\nendmodule\nmodule m;\n  l v ();\nendmodule\n"
                   "module l;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rs.status);
    TEST_ASSERT_TRUE(oom_sweep(ODIN3_OK) > 10);
    et_free(&rs);
    /* Three clean modules, a given top: the store is written, sealed and handed out. */
    set_cli_top("t");
    run_one("t.v", "module t;\nendmodule\nmodule m;\nendmodule\nmodule l;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rs.status);
    TEST_ASSERT_TRUE(oom_sweep(ODIN3_OK) > 10);
}

/* The library paths (review I1): a given top in several libraries, then an instance resolved
 * across libraries with its info line. */
static void test_oom_libraries(void) {
    sub_two_libs("sub");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_TRUE(oom_sweep(ODIN3_ERR_PARSE) > 10);
    et_free(&rs);
    set_cli_top(NULL);
    static const et_src srcs[] = {
        {"t.v", "module t;\n  sub u ();\nendmodule\n"},
        {"l2.v", "module sub;\nendmodule\n"},
        {"l3.v", "module sub;\nendmodule\n"},
    };
    et_src proj = {"p.o3proj", "file verilog t.v\nfile verilog l2.v library lib2\n"
                               "file verilog l3.v library lib3\n"};
    et_req req = {srcs, 3, "read_project p.o3proj", &proj, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rs.status); /* Task 11: the instance is written */
    TEST_ASSERT_TRUE(oom_sweep(ODIN3_OK) > 10);
}

/* --- Task 10 review carries (n2, n3) ------------------------------------------------------ */

/* n2: a kind that is neither pending nor handled is a 2D bug (the I3 path): its line and CHECK. */
static void test_unhandled_kind(void) {
    run_one("t.v", "module t;\nendmodule\n");
    odin3_elab1 *e1 = context();
    odin3_ast_id module = odin3_ast_child(rs.parsed, odin3_ast_root(rs.parsed, 0), 0);
    TEST_ASSERT_FALSE(odin3_elab_pending(ODIN3_ELAB_STAGE1, ODIN3_AST_MODULE));
    odin3_elab1_not_yet(e1, module);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:1:1: error: stage 1: no elaboration of 'MODULE' (a 2D bug)");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_elab1_status(e1));
    odin3_elab1_destroy(e1);
}

/* n2: a black box's body is never walked: its instance of an undefined module is not reported;
 * the black box is a ports-only MODULE (Task 12 fix round, EL-4 v4.8). */
static void test_blackbox_body_unwalked(void) {
    run_one("t.v", "module t;\n  bb x ();\nendmodule\n(* blackbox *)\nmodule bb;\n  foo y ();\n"
                   "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, et_count_with(&rs, ODIN3_LOG_ERROR, "unresolved_module"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(INSTANTIATION:bb @t.v:2:3"), rs.dump.data);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rs.dump.data, "(MODULE:bb"), rs.dump.data);
    TEST_ASSERT_NULL(strstr(rs.dump.data, "(INSTANTIATION:foo"));
}

/* Arms an allocation failure once, as stage 2 starts (the on_body_start hook). */
typedef struct arm_at {
    long after;
    bool armed;
} arm_at;

static void arm_failure(void *user, odin3_ast_id module) {
    arm_at *arm = user;
    (void)module;
    if (!arm->armed) {
        arm->armed = true;
        odin3_util_set_alloc_fail_after(arm->after);
    }
}

/* A design under --keep-ast all holding 7 stores (a --parse-only read, then three reads), so the
 * next read's elaborated store is the 9th: handing it over grows the design's store list. */
static void seven_stores(void) {
    et_src src = {"e.v", "module e;\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    odin3_log_set_sink(quiet_sink, NULL); /* the sink must not take the failure */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, ODIN3_KEEP_AST_ALL));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("--parse-only e.v")));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                              odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("e.v")));
    }
    TEST_ASSERT_EQUAL_UINT32(7, odin3_design_ast_count(rs.design));
}

/* n3 (m6): out of memory while the elaborated store is handed to the design wins over stage 2's
 * PARSE. Each allocation after stage 2 starts fails in turn until the store is held; while it is
 * not, the read must say NO_MEMORY. */
static void test_oom_hand_over(void) {
    int missed = 0;
    bool held = false;
    for (long after = 0; after < OOM_SETTLE && !held; after++) {
        seven_stores();
        arm_at arm = {after, false};
        odin3_elab_test_hooks hooks = {0, arm_failure, &arm};
        odin3_elab_test_set_hooks(&hooks);
        odin3_status st = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("e.v"));
        odin3_util_set_alloc_fail_after(-1);
        odin3_elab_test_set_hooks(NULL);
        held = odin3_design_ast_count(rs.design) == 9;
        if (!held) {
            TEST_ASSERT_EQUAL_UINT32(8, odin3_design_ast_count(rs.design));
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            missed++;
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st); /* the stage-2 stub */
        }
        et_free(&rs);
    }
    TEST_ASSERT_TRUE(held);
    TEST_ASSERT_TRUE(missed > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lookup_own_library_first);
    RUN_TEST(test_lookup_several_libraries);
    RUN_TEST(test_lookup_library_unit_last);
    RUN_TEST(test_lookup_single_other_library);
    RUN_TEST(test_lookup_every_instance);
    RUN_TEST(test_blackbox_attribute_value);
    RUN_TEST(test_udp_rejected);
    RUN_TEST(test_config_rejected);
    RUN_TEST(test_library_udp_rejected_when_used);
    RUN_TEST(test_pending_forced_udp);
    RUN_TEST(test_celltype_resolves);
    RUN_TEST(test_dollar_name_unresolved);
    RUN_TEST(test_undefined_unresolved);
    RUN_TEST(test_auto_blackbox);
    RUN_TEST(test_taken_hard_cell);
    RUN_TEST(test_taken_body_not_walked);
    RUN_TEST(test_taken_blackbox);
    RUN_TEST(test_taken_module_of_earlier_read);
    RUN_TEST(test_blackbox_module_reuses_cell);
    RUN_TEST(test_top_single_candidate);
    RUN_TEST(test_top_ambiguous);
    RUN_TEST(test_top_none);
    RUN_TEST(test_top_unknown);
    RUN_TEST(test_top_unknown_project);
    RUN_TEST(test_top_cli_beats_project);
    RUN_TEST(test_top_several_libraries);
    RUN_TEST(test_top_unreachable_info);
    RUN_TEST(test_top_generate_branch_counts);
    RUN_TEST(test_top_generate_for_counts);
    RUN_TEST(test_top_generate_case_counts);
    RUN_TEST(test_top_generate_region_counts);
    RUN_TEST(test_top_library_units_excluded);
    RUN_TEST(test_top_blackbox_excluded);
    RUN_TEST(test_top_given_prefers_design_unit);
    RUN_TEST(test_pending_forced_kind);
    RUN_TEST(test_pending_always);
    RUN_TEST(test_pending_forced_module);
    RUN_TEST(test_pending_directive_and_attribute);
    RUN_TEST(test_pending_tables);
    RUN_TEST(test_pending_report);
    RUN_TEST(test_warn_once_and_cap);
    RUN_TEST(test_ownership_keep_none);
    RUN_TEST(test_ownership_keep_all);
    RUN_TEST(test_ownership_keep_elaborated);
    RUN_TEST(test_abi_library_units_from);
    RUN_TEST(test_entry_nolatches);
    RUN_TEST(test_entry_other_flags);
    RUN_TEST(test_entry_top_option);
    RUN_TEST(test_entry_unknown_option);
    RUN_TEST(test_entry_reaches_stub);
    RUN_TEST(test_entry_store_checks);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom);
    RUN_TEST(test_oom_libraries);
    RUN_TEST(test_unhandled_kind);
    RUN_TEST(test_blackbox_body_unwalked);
    RUN_TEST(test_oom_hand_over);
    return UNITY_END();
}
