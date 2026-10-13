/*
 * test_elab_stage2_wires.c — 2D Task 17: stage 2's context, modules, ports and wires, provenance,
 * attributes, keep levels, caps and byte budget, the failure paths, the ABI entry
 * odin3_ast_elaborate and an OOM sweep (EL-2 steps 4–7, EL-23, EL-24, EL-45, EL-46, EL-47). The
 * whole read_verilog pass runs through the pass manager (elab_test.h's et_read); the IR is read
 * back through the module API and every located diagnostic is asserted as the whole line.
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 200000, OOM_SETTLE = 64, FILE_MAX = 1 << 16 };

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_elab_test_set_hooks(NULL);
    odin3_log_set_sink(NULL, NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* --- running and reading back --------------------------------------------------------------- */

/* t.v read through the whole pass at keep level keep. */
static void read_text(const char *text, odin3_keep_ast keep) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, keep, &rs);
}

/* The pass ran clean: OK, no error, and the IR passes check FULL. */
static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

/* Errors that are the locatable diagnostics: not the pass manager's own "failed" line. */
static uint32_t diag_errors(void) {
    return rs.errors - et_count_with(&rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

static void read_clean(const char *text, odin3_keep_ast keep) {
    read_text(text, keep);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    expect_ir_ok();
}

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(rs.design), id);
}

static uint32_t id_of(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(text), &id), text);
    return id;
}

/* The module named name; fails the test when there is none. */
static odin3_module *module_named(const char *name) {
    uint32_t id = id_of(name);
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        if (odin3_module_name(module) == id) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static bool has_module(const char *name) {
    uint32_t id = 0;
    if (!odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(name), &id)) {
        return false;
    }
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        if (odin3_module_name(odin3_module_get(rs.design, (odin3_module_id){index})) == id) {
            return true;
        }
    }
    return false;
}

/* The wire named name of module; fails the test when there is none. */
static odin3_wire_id wire_named(const odin3_module *module, const char *name) {
    uint32_t id = 0;
    odin3_wire_id wire = {0};
    if (odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(name), &id)) {
        wire = odin3_module_find_wire(module, id);
    }
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    return wire;
}

static bool has_wire(const odin3_module *module, const char *name) {
    uint32_t id = 0;
    return odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(name), &id) &&
           odin3_module_find_wire(module, id).v != 0;
}

/* The wire's range and sign. */
typedef struct wire_shape {
    int32_t msb, lsb;
    bool is_signed;
} wire_shape;

static void expect_wire(const odin3_module *module, const char *name, wire_shape want) {
    odin3_wire_id wire = wire_named(module, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want.msb, odin3_wire_msb(module, wire), name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want.lsb, odin3_wire_lsb(module, wire), name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want.is_signed, odin3_wire_signed(module, wire), name);
    uint32_t width =
        (uint32_t)(want.msb >= want.lsb ? want.msb - want.lsb : want.lsb - want.msb) + 1;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(width, odin3_wire_width(module, wire), name);
}

/* The held store of form (the run of the last read), NULL when none. */
static odin3_ast *held_store(odin3_ast_form form) {
    for (uint32_t i = 0; i < odin3_design_ast_count(rs.design); i++) {
        odin3_ast *ast = odin3_design_ast_at(rs.design, i);
        if (odin3_ast_form_of(ast) == form) {
            return ast;
        }
    }
    return NULL;
}

/* The first DECLARATOR of the store named name. */
static odin3_ast_id declarator_named(const odin3_ast *ast, const char *name) {
    for (uint32_t id = 1; id < odin3_ast_node_end(ast).v; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(ast, node) == ODIN3_AST_DECLARATOR &&
            strcmp(odin3_ast_name_str(ast, node), name) == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return (odin3_ast_id){0};
}

/* The NET and VAR symbols of the held elaborated store whose flags & mask equal want: each must
 * record top and the wire named like it. Returns how many there are. */
static uint32_t count_symbol_wires(const odin3_module *top, uint16_t mask, uint16_t want) {
    const odin3_symtab *table = odin3_ast_symtab(held_store(ODIN3_AST_FORM_ELABORATED));
    TEST_ASSERT_NOT_NULL(table);
    uint32_t seen = 0;
    for (uint32_t index = 1; index <= odin3_symtab_symbol_count(table); index++) {
        const odin3_symbol *sym = odin3_symbol_get(table, (odin3_sym_id){index});
        if ((sym->kind != ODIN3_SYM_NET && sym->kind != ODIN3_SYM_VAR) ||
            (sym->flags & mask) != want) {
            continue;
        }
        seen++;
        TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top).v, sym->ir_module.v);
        TEST_ASSERT_EQUAL_UINT32(wire_named(top, str_of(sym->name)).v, sym->ir_wire.v);
    }
    return seen;
}

/* The SOURCE record of a wire: its first location, ast ID and hierarchy path. */
typedef struct prov_want {
    const char *file;
    uint32_t line, col, end_line, end_col;
    const char *hier;
} prov_want;

static const odin3_prov_record *wire_prov(const odin3_module *module, const char *name) {
    const odin3_prov_record *rec =
        odin3_prov_get(rs.design, odin3_wire_prov(module, wire_named(module, name)));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, rec->kind);
    return rec;
}

static void expect_prov(const odin3_prov_record *rec, prov_want want) {
    TEST_ASSERT_TRUE(rec->n_locs >= 1);
    TEST_ASSERT_EQUAL_STRING(want.file, str_of(rec->locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(want.line, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(want.col, rec->locs[0].col);
    TEST_ASSERT_EQUAL_UINT32(want.end_line, rec->locs[0].end_line);
    TEST_ASSERT_EQUAL_UINT32(want.end_col, rec->locs[0].end_col);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, rec->locs[0].loc);
    TEST_ASSERT_EQUAL_STRING(want.hier, str_of(rec->hier));
}

/* --- wires (EL-23) --------------------------------------------------------------------------- */

/* The ranges, directions and signs of the four declaration forms; every symbol records its wire. */
static void test_wires(void) {
    read_clean("module top;\n  wire [0:3] w;\n  wire [-1:2] n;\n  reg signed [7:0] r;\n"
               "  integer i;\n  wire s;\nendmodule\n",
               ODIN3_KEEP_AST_ELABORATED);
    odin3_module *top = module_named("top");
    expect_wire(top, "w", (wire_shape){0, 3, false});
    expect_wire(top, "n", (wire_shape){-1, 2, false});
    expect_wire(top, "r", (wire_shape){7, 0, true});
    expect_wire(top, "i", (wire_shape){31, 0, true});
    expect_wire(top, "s", (wire_shape){0, 0, false});
    uint32_t seen = count_symbol_wires(top, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(5, seen);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_ast_count(rs.design));
}

/* Each wire has one net per bit, each with the wire bit as primary. */
static void test_wire_nets(void) {
    read_clean("module top;\n  wire [3:0] w;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    odin3_wire_id wire = wire_named(top, "w");
    for (uint32_t bit = 0; bit < 4; bit++) {
        odin3_net_id net = odin3_wire_net(top, wire, bit);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, net.v);
        TEST_ASSERT_EQUAL_UINT32(wire.v, odin3_net_primary(top, net).wire.v);
        TEST_ASSERT_EQUAL_UINT32(bit, odin3_net_primary(top, net).bit);
    }
}

/* --- scoped names (Review Focus 5) ----------------------------------------------------------- */

static void test_scoped_names(void) {
    read_clean("module top;\n  genvar i;\n"
               "  for (i = -1; i < 2; i = i + 1) begin : g\n    wire t;\n  end\n"
               "endmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    for (const char *const *name = (const char *const[]){"g[-1].t", "g[0].t", "g[1].t", NULL};
         *name != NULL; name++) {
        expect_wire(top, *name, (wire_shape){0, 0, false});
    }
    TEST_ASSERT_FALSE(has_wire(top, "t"));
}

/* Named and unnamed blocks of a process: their locals are module wires named by the block. */
static void test_block_names(void) {
    read_text("module top;\n  reg x;\n  always @(*) begin : blk\n    reg [3:0] count;\n"
              "    x = count[0];\n  end\n  always @(*) begin\n    reg tmp;\n    x = tmp;\n  end\n"
              "endmodule\n",
              ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    expect_wire(top, "blk.count", (wire_shape){3, 0, false});
    expect_wire(top, "$unnamed_block$1.tmp", (wire_shape){0, 0, false});
    expect_wire(top, "x", (wire_shape){0, 0, false});
    TEST_ASSERT_FALSE(has_wire(top, "count"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    expect_ir_ok();
}

/* Task 14's probe: genblk1.genblk1.x, a bare `if` consumes genblk2, the loop is genblk3[0].w. */
static void test_probe_names(void) {
    read_clean("module top;\n  localparam A = 0, B = 1;\n"
               "  if (A) begin wire x; end else if (B) begin wire x; end\n"
               "  if (B) begin wire z; end\n  genvar i;\n"
               "  for (i = 0; i < 1; i = i + 1) begin wire w; end\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    expect_wire(top, "genblk1.genblk1.x", (wire_shape){0, 0, false});
    expect_wire(top, "genblk2.z", (wire_shape){0, 0, false});
    expect_wire(top, "genblk3[0].w", (wire_shape){0, 0, false});
}

/* A function's locals are inlined where it is called: no wire of their own. */
static void test_function_locals_make_no_wire(void) {
    read_clean("module top;\n  function f;\n    input a;\n    reg b;\n    f = a;\n  endfunction\n"
               "  wire w;\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_FALSE(has_wire(top, "b"));
    TEST_ASSERT_FALSE(has_wire(top, "f.b"));
    TEST_ASSERT_TRUE(has_wire(top, "w"));
}

/* A memory is Task 20's: no wire (and no error). */
static void test_memory_makes_no_wire(void) {
    read_clean("module top;\n  reg [7:0] mem [0:3];\n  wire w;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_FALSE(has_wire(module_named("top"), "mem"));
    TEST_ASSERT_TRUE(has_wire(module_named("top"), "w"));
}

/* An implicit net is a wire too, and stage 1's marker is not copied onto it. */
static void test_implicit_net(void) {
    read_text("module top (input a);\n  sub u (.x(q));\nendmodule\n"
              "module sub (input x);\nendmodule\n",
              ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    odin3_wire_id implicit = wire_named(top, "q");
    TEST_ASSERT_NULL(
        odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_WIRE, implicit.v}, id_of("implicit")));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status,
                                  et_log(&rs)); /* the instance landed (Task 19) */
}

/* --- ports (EL-24, IR change C) -------------------------------------------------------------- */

/* One port record of the cell type. */
typedef struct port_want {
    const char *name;
    bool scalar, is_signed;
    uint32_t width;
    odin3_dir dir;
} port_want;

static void expect_port_def(const odin3_celltype_def *def, uint32_t index, port_want want) {
    const odin3_port_def *port = &def->ports[index];
    TEST_ASSERT_EQUAL_STRING(want.name, port->name);
    TEST_ASSERT_EQUAL_INT(want.scalar, port->scalar);
    TEST_ASSERT_EQUAL_INT(want.is_signed, port->is_signed);
    TEST_ASSERT_EQUAL_INT(want.dir, port->dir);
    if (want.width != 0) {
        TEST_ASSERT_EQUAL_UINT32(want.width, port->width);
    }
}

/* The cell type's port records of test_ports' module. */
static void expect_port_defs(const odin3_module *top) {
    const odin3_celltype_def *def = odin3_celltype_get(rs.design, odin3_module_celltype(top));
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_EQUAL_UINT32(4, def->n_ports);
    expect_port_def(def, 0, (port_want){"a", false, true, 8, ODIN3_DIR_IN});
    expect_port_def(def, 1, (port_want){"b", true, false, 1, ODIN3_DIR_IN});
    expect_port_def(def, 2, (port_want){"c", false, false, 4, ODIN3_DIR_OUT});
    expect_port_def(def, 3, (port_want){"d", true, false, 1, ODIN3_DIR_INOUT});
}

static void test_ports(void) {
    read_clean("module top (input signed [0:7] a, input b, output [3:0] c, inout d);\n"
               "endmodule\n",
               ODIN3_KEEP_AST_ELABORATED);
    odin3_module *top = module_named("top");
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(top));
    expect_wire(top, "a", (wire_shape){0, 7, true});
    expect_wire(top, "b", (wire_shape){0, 0, false});
    expect_wire(top, "c", (wire_shape){3, 0, false});
    expect_port_defs(top);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_wire_port_node(top, wire_named(top, "a")).v);
    TEST_ASSERT_EQUAL_UINT32(4, count_symbol_wires(top, ODIN3_SYM_F_PORT, ODIN3_SYM_F_PORT));
}

/* A one-bit range keeps its range and is not scalar (IR change C). */
static void test_port_one_bit_range(void) {
    read_clean("module top (input [0:0] a, input [5:5] b);\nendmodule\n", ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    const odin3_celltype_def *def = odin3_celltype_get(rs.design, odin3_module_celltype(top));
    TEST_ASSERT_FALSE(def->ports[0].scalar);
    expect_wire(top, "a", (wire_shape){0, 0, false});
    expect_wire(top, "b", (wire_shape){5, 5, false});
}

/* Non-ANSI ports, merged with their declarations in stage 1, are created in header order. */
static void test_non_ansi_ports(void) {
    read_clean(
        "module top (b, a);\n  output [1:0] b;\n  input signed a;\n  reg [1:0] b;\nendmodule\n",
        ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(top));
    TEST_ASSERT_EQUAL_UINT32(wire_named(top, "b").v, odin3_module_port_wire(top, 0).v);
    TEST_ASSERT_EQUAL_UINT32(wire_named(top, "a").v, odin3_module_port_wire(top, 1).v);
    expect_wire(top, "a", (wire_shape){0, 0, true});
}

/* The ports of every module exist before the first body (EL-2): the hook sees them final. */
typedef struct body_probe {
    odin3_design *design;
    uint32_t calls;
    uint32_t modules_at_first;
    uint32_t ports_at_first[4];
} body_probe;

static void probe_body(void *user, odin3_ast_id module) {
    body_probe *probe = user;
    (void)module;
    if (probe->calls++ == 0) {
        probe->modules_at_first = odin3_design_module_end(probe->design) - 1;
        for (uint32_t id = 1; id <= probe->modules_at_first && id <= 4; id++) {
            probe->ports_at_first[id - 1] =
                odin3_module_port_count(odin3_module_get(probe->design, (odin3_module_id){id}));
        }
    }
}

static void test_ports_before_bodies(void) {
    et_src src = {"t.v", "module sub #(parameter W = 1) (input [W-1:0] a, output [W-1:0] y);\n"
                         "endmodule\n"
                         "module top (input [7:0] i, output [7:0] o);\n"
                         "  sub #(.W(8)) u1 (.a(i), .y(o));\n"
                         "  sub #(.W(4)) u2 (.a(i[3:0]), .y());\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    body_probe probe = {0};
    odin3_elab_test_hooks hooks = {0, probe_body, &probe, 0, NULL};
    odin3_elab_test_set_hooks(&hooks);
    et_setup(&req, &rs);
    probe.design = rs.design;
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    TEST_ASSERT_EQUAL_UINT32(3, probe.calls);
    TEST_ASSERT_EQUAL_UINT32(3, probe.modules_at_first);
    for (uint32_t index = 0; index < 3; index++) {
        TEST_ASSERT_EQUAL_UINT32(2, probe.ports_at_first[index]);
    }
    expect_wire(module_named("top"), "i", (wire_shape){7, 0, false});
    expect_wire(module_named("sub$W=8"), "a", (wire_shape){7, 0, false});
    expect_wire(module_named("sub$W=4"), "y", (wire_shape){3, 0, false});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* --- provenance (EL-45) ---------------------------------------------------------------------- */

static void test_provenance(void) {
    read_clean("module top (input a);\n  wire [-1:2] w;\n"
               "  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n    wire t;\n  end\n"
               "endmodule\n",
               ODIN3_KEEP_AST_ELABORATED);
    odin3_module *top = module_named("top");
    const odin3_ast *ast = held_store(ODIN3_AST_FORM_ELABORATED);
    const odin3_prov_record *rec = wire_prov(top, "w");
    expect_prov(rec, (prov_want){"t.v", 2, 15, 2, 16, "top"});
    TEST_ASSERT_EQUAL_UINT32(declarator_named(ast, "w").v, rec->ast);
    TEST_ASSERT_EQUAL_UINT32(1, rec->run.v); /* the read_verilog run */
    rec = wire_prov(top, "g[1].t");
    expect_prov(rec, (prov_want){"t.v", 5, 10, 5, 11, "top/g[1]"});
    TEST_ASSERT_EQUAL_STRING("top/g[0]", str_of(wire_prov(top, "g[0].t")->hier));
    /* the port: its PORT_DECL; the module: its MODULE */
    rec = wire_prov(top, "a");
    expect_prov(rec, (prov_want){"t.v", 1, 13, 1, 20, "top"});
    rec = odin3_prov_get(rs.design, odin3_module_prov(top));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_STRING("top", str_of(rec->hier));
    TEST_ASSERT_EQUAL_UINT32(1, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(1, rec->locs[0].col);
}

static void test_provenance_specialization(void) {
    read_text(
        "module sub #(parameter W = 1) (output o);\n  if (W > 4) begin wire v; end\nendmodule\n"
        "module top;\n  sub #(.W(8)) u ();\nendmodule\n",
        ODIN3_KEEP_AST_NONE);
    odin3_module *sub = module_named("sub$W=8");
    expect_wire(sub, "genblk1.v", (wire_shape){0, 0, false});
    TEST_ASSERT_EQUAL_STRING("sub$W=8/genblk1", str_of(wire_prov(sub, "genblk1.v")->hier));
    TEST_ASSERT_EQUAL_STRING("sub$W=8", str_of(wire_prov(sub, "o")->hier));
}

/* Nested scopes join with '/' and loop indices. */
static void test_provenance_nested(void) {
    read_clean("module top;\n  genvar i;\n  for (i = 0; i < 2; i = i + 1) begin : g\n"
               "    if (i) begin : inner\n      wire t;\n    end\n  end\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_EQUAL_STRING("top/g[1]/inner", str_of(wire_prov(top, "g[1].inner.t")->hier));
}

/* --- attributes (2A §7) ---------------------------------------------------------------------- */

/* The attribute key of the wire named name; NULL when it has none. */
typedef struct attr_ref {
    const char *wire, *key;
} attr_ref;

static const odin3_value *wire_attr(const odin3_module *module, attr_ref ref) {
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire_named(module, ref.wire).v};
    return odin3_attr_get(module, obj, id_of(ref.key));
}

static void test_attributes(void) {
    read_clean("module top ((* keep *) input a);\n  (* keep *) wire w;\n"
               "  (* src = \"x\" *) wire s;\n  (* init = 4'b1010 *) wire [3:0] n;\n"
               "  (* a, b = 5 *) wire m;\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    const odin3_value *val = wire_attr(top, (attr_ref){"w", "keep"});
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, val->kind);
    TEST_ASSERT_EQUAL_INT64(1, val->i);
    val = wire_attr(top, (attr_ref){"s", "src"});
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, val->kind);
    TEST_ASSERT_EQUAL_STRING("x", str_of(val->str));
    val = wire_attr(top, (attr_ref){"n", "init"});
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, val->kind);
    TEST_ASSERT_EQUAL_UINT32(4, val->len);
    static const uint8_t k_1010[] = {0, 1, 0, 1}; /* LSB first */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_1010, val->bits, 4);
    TEST_ASSERT_EQUAL_INT64(1, wire_attr(top, (attr_ref){"m", "a"})->i);
    TEST_ASSERT_EQUAL_UINT32(32, wire_attr(top, (attr_ref){"m", "b"})->len);
}

/* A port: the attribute is on its wire and its port node. */
static void test_port_attributes(void) {
    read_clean("module top ((* keep *) input a, input b);\nendmodule\n", ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_NOT_NULL(wire_attr(top, (attr_ref){"a", "keep"}));
    TEST_ASSERT_NULL(wire_attr(top, (attr_ref){"b", "keep"}));
    odin3_node_id node = odin3_wire_port_node(top, wire_named(top, "a"));
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_NODE, node.v}, id_of("keep")));
}

/* Non-ANSI ports: the attributes of the port declaration and of the net or variable declaration
 * that merged into it land on the port's wire, and the port is created in header order. */
static void test_non_ansi_port_attributes(void) {
    read_clean("module top (a, b, c);\n  (* keep *) input a;\n  (* src = \"x\" *) output b;\n"
               "  (* hello *) wire b;\n  (* keep *) output [1:0] c;\n  (* hi *) wire [1:0] c;\n"
               "endmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_EQUAL_UINT32(3, odin3_module_port_count(top));
    expect_wire(top, "c", (wire_shape){1, 0, false});
    static const struct {
        attr_ref ref;
        bool present;
    } k_cases[] = {{{"a", "keep"}, true},  {{"b", "src"}, true}, {{"b", "hello"}, true},
                   {{"c", "keep"}, true},  {{"c", "hi"}, true},  {{"a", "hello"}, false},
                   {{"b", "keep"}, false}, {{"c", "src"}, false}};
    for (size_t index = 0; index < sizeof k_cases / sizeof k_cases[0]; index++) {
        const odin3_value *val = wire_attr(top, k_cases[index].ref);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_cases[index].present, val != NULL, k_cases[index].ref.key);
    }
    TEST_ASSERT_EQUAL_STRING("x", str_of(wire_attr(top, (attr_ref){"b", "src"})->str));
    TEST_ASSERT_EQUAL_INT64(1, wire_attr(top, (attr_ref){"b", "hello"})->i);
}

static void test_module_attributes(void) {
    read_clean("(* keep, fsm_encoding = \"onehot\" *)\nmodule top;\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    odin3_objref self = {ODIN3_OBJ_MODULE, odin3_module_id_of(top).v};
    TEST_ASSERT_EQUAL_INT64(1, odin3_attr_get(top, self, id_of("keep"))->i);
    TEST_ASSERT_EQUAL_STRING("onehot",
                             str_of(odin3_attr_get(top, self, id_of("fsm_encoding"))->str));
}

/* --- the top and the summary (EL-2 steps 5 and 7) --------------------------------------------- */

static int find_with_prefix(const char *prefix) {
    for (uint32_t i = 0; i < et_count(&rs); i++) {
        if (strncmp(et_text(&rs, i), prefix, strlen(prefix)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The summary line (an info line), failing the test when there is none. */
static const char *summary_line(void) {
    int at = find_with_prefix("read_verilog: elaborated top");
    TEST_ASSERT_TRUE_MESSAGE(at >= 0, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_INFO, et_level(&rs, (uint32_t)at));
    return et_text(&rs, (uint32_t)at);
}

static void expect_summary_shape(const char *line) {
    regex_t re;
    TEST_ASSERT_EQUAL_INT(0, regcomp(&re,
                                     "^read_verilog: elaborated top 'top': 1 modules \\(0 "
                                     "specializations\\), 0 processes, 0 cells, [0-9]+ nets, "
                                     "[0-9.]+ \\+ [0-9.]+ ms$",
                                     REG_EXTENDED | REG_NOSUB));
    int hit = regexec(&re, line, 0, NULL, 0);
    regfree(&re);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hit, line);
}

/* The N of "..., C cells, N nets, ...". */
static uint32_t summary_nets(const char *line) {
    static const char k_cells[] = "cells, ";
    const char *at = strstr(line, k_cells);
    TEST_ASSERT_NOT_NULL(at);
    return (uint32_t)strtoul(at + strlen(k_cells), NULL, 10);
}

static void test_top_and_summary(void) {
    read_clean("module top (input a, output [1:0] y);\n  wire [3:0] w;\nendmodule\n",
               ODIN3_KEEP_AST_NONE);
    odin3_module *top = module_named("top");
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top).v, odin3_design_top(rs.design).v);
    expect_summary_shape(summary_line());
    TEST_ASSERT_EQUAL_UINT32(1 + 2 + 4, summary_nets(summary_line())); /* a, y and w */
}

/* A specialization counts as one: top, sub (the default's name) and sub$W=8 are 3 modules (1). */
static void test_summary_specialization_count(void) {
    read_text("module sub #(parameter W = 1) (output o);\nendmodule\n"
              "module top;\n  sub #(.W(8)) u1 ();\n  sub u2 ();\nendmodule\n",
              ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_NOT_NULL(strstr(summary_line(), "3 modules (1 specializations), "));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(module_named("top")).v,
                             odin3_design_top(rs.design).v);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_design_module_end(rs.design) - 1);
    expect_ir_ok();
}

/* --- keep levels (review I10) ----------------------------------------------------------------- */

static void test_keep_none(void) {
    read_clean("module top;\n  wire w;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(rs.design));
}

static void slurp(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(out, 1, cap - 1, file);
    out[got] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* write_verilog_ast of the held store prints the elaborated UNIT. */
static void expect_printed_elaborated(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(rs.design, "write_verilog_ast", odin3_bytes_cstr("out.vast")));
    static char text[FILE_MAX];
    slurp("out.vast", text, sizeof text);
    TEST_ASSERT_EQUAL_INT(0, remove("out.vast"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "elaborated AST"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "module top"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "wire w"), text);
}

static void test_keep_elaborated(void) {
    read_clean("module top;\n  wire w;\nendmodule\n", ODIN3_KEEP_AST_ELABORATED);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_ast_count(rs.design));
    odin3_ast *elab = held_store(ODIN3_AST_FORM_ELABORATED);
    TEST_ASSERT_NOT_NULL(elab);
    TEST_ASSERT_NOT_NULL(odin3_ast_symtab(elab)); /* held with its symtab */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(elab, ODIN3_CHECK_FULL));
    TEST_ASSERT_NULL(held_store(ODIN3_AST_FORM_PARSED));
    expect_printed_elaborated();
}

static void test_keep_all(void) {
    read_clean("module top;\n  wire w;\nendmodule\n", ODIN3_KEEP_AST_ALL);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_ast_count(rs.design));
    TEST_ASSERT_NOT_NULL(held_store(ODIN3_AST_FORM_PARSED));
    TEST_ASSERT_NOT_NULL(held_store(ODIN3_AST_FORM_ELABORATED));
}

/* After a failed stage 2 the stores follow the keep level too. */
static void test_keep_after_failure(void) {
    read_text("module top;\n  reg r;\n  always @(*) r = 1;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_ast_count(rs.design));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_text("module top;\n  reg r;\n  always @(*) r = 1;\nendmodule\n",
              ODIN3_KEEP_AST_ELABORATED);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_ast_count(rs.design));
}

/* --- the second read (moved from Task 10; review R3) ------------------------------------------ */

static void test_second_read_duplicate_module(void) {
    static const et_src srcs[] = {{"first.v", "module m;\nendmodule\n"},
                                  {"second.v", "module m;\n  wire w;\nendmodule\n"}};
    et_req req = {srcs, 2, "read_verilog first.v", NULL, NULL};
    et_setup(&req, &rs);
    TEST_ASSERT_TRUE(has_module("m")); /* the first read made it */
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("second.v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "second.v:1:1: error: duplicate_module: module 'm' is already defined by an earlier "
              "read (run 1)");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_module_end(rs.design) - 1);
    expect_ir_ok();
}

/* A second read with another module name adds to the design; the top is the latest read's. */
static void test_second_read_other_module(void) {
    static const et_src srcs[] = {{"first.v", "module m;\nendmodule\n"},
                                  {"second.v", "module n;\n  wire w;\nendmodule\n"}};
    et_req req = {srcs, 2, "read_verilog first.v", NULL, NULL};
    et_setup(&req, &rs);
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("second.v"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(module_named("n")).v,
                             odin3_design_top(rs.design).v);
    expect_ir_ok();
}

/* --- failure paths ---------------------------------------------------------------------------- */

typedef struct body_count {
    uint32_t calls;
} body_count;

static void count_body(void *user, odin3_ast_id module) {
    (void)module;
    ((body_count *)user)->calls++;
}

/* A port the IR refuses (hook): the module is left with the ports before it, no body runs, the
 * top stays unset and the IR is valid (review M10). */
static void test_refused_port_stops_stage(void) {
    et_src src = {"t.v", "module sub (input a, output y);\nendmodule\n"
                         "module top (input i, output o);\n  sub u (.a(i), .y(o));\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    body_count count = {0};
    odin3_elab_test_hooks hooks = {0, count_body, &count, 3, NULL};
    odin3_elab_test_set_hooks(&hooks);
    et_setup(&req, &rs);
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:13: error: cannot create port 'a' of module 'sub'");
    TEST_ASSERT_EQUAL_UINT32(0, count.calls);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(module_named("top")));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_port_count(module_named("sub")));
    expect_ir_ok();
}

/* A pending kind: the located line once, the rest of the module emitted, PARSE (point (l)). */
static void test_pending_always(void) {
    read_text("module top;\n  reg r;\n  always @(*) r = 1;\n  wire w;\nendmodule\n",
              ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: elaboration is not available yet (2D): 'ALWAYS'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    odin3_module *top = module_named("top");
    TEST_ASSERT_TRUE(has_wire(top, "w"));
    TEST_ASSERT_TRUE(has_wire(top, "r"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    TEST_ASSERT_EQUAL_INT(-1, find_with_prefix("read_verilog: elaborated top"));
    expect_ir_ok();
}

/* A kind reported once however many times it occurs; other kinds each have their line. */
static void test_pending_once_per_kind(void) {
    read_text("module top;\n  reg a;\n  always @(*) a = 1;\n  always @(*) a = 0;\n"
              "  initial a = 0;\nendmodule\n",
              ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:3: error: elaboration is not available yet (2D): 'ALWAYS'");
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:5:3: error: elaboration is not available yet (2D): 'INITIAL'");
    TEST_ASSERT_EQUAL_UINT32(2, diag_errors());
}

/* A declaration's initialiser: a net's is a continuous assignment (Task 18), a variable's waits
 * for Task 24. */
static void test_pending_initialisers(void) {
    read_text("module top;\n  wire a = 1;\n  reg r = 0;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_INT(-1, et_find(&rs, ODIN3_LOG_ERROR,
                                      "t.v:2:8: error: elaboration is not available yet (2D): "
                                      "'NET_ASSIGN'"));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:7: error: elaboration is not available yet (2D): 'INITIAL'");
    TEST_ASSERT_EQUAL_UINT32(1, diag_errors());
    TEST_ASSERT_TRUE(has_wire(module_named("top"), "a"));
    TEST_ASSERT_TRUE(has_wire(module_named("top"), "r"));
}

/* A landed kind forced pending is reported at its walk and skipped (the table is consulted). */
static void test_forced_pending_declaration(void) {
    odin3_elab_test_pend(ODIN3_ELAB_STAGE2, ODIN3_AST_NET_DECL, true);
    read_text("module top;\n  wire a;\n  wire b;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    odin3_elab_test_pend(ODIN3_ELAB_STAGE2, ODIN3_AST_NET_DECL, false);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:2:3: error: elaboration is not available yet (2D): 'NET_DECL'");
    TEST_ASSERT_EQUAL_UINT32(1, diag_errors());
    TEST_ASSERT_FALSE(has_wire(module_named("top"), "a"));
}

/* --- caps (point (aa), EL-46) ----------------------------------------------------------------- */

static void lower_limits(void (*set)(odin3_elab_limits *)) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    set(&limits);
    odin3_elab_test_set_limits(&limits);
}

static void net_bits_64(odin3_elab_limits *limits) {
    limits->max_net_bits = 64;
}

static void test_net_bits_cap(void) {
    lower_limits(net_bits_64);
    read_text("module top;\n  wire [99:0] w;\n  wire after;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:15: error: elaboration needs more than 64 net bits");
    TEST_ASSERT_EQUAL_UINT32(1, diag_errors());
    TEST_ASSERT_FALSE(has_wire(module_named("top"), "w"));
    TEST_ASSERT_FALSE(has_wire(module_named("top"), "after")); /* fatal: nothing after it */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    expect_ir_ok();
}

/* The nets of ports count too, and exactly the cap is allowed. */
static void test_net_bits_ports_and_exact(void) {
    lower_limits(net_bits_64);
    read_clean("module top (input [31:0] a, output [31:0] y);\nendmodule\n", ODIN3_KEEP_AST_NONE);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    lower_limits(net_bits_64);
    read_text("module top (input [31:0] a, output [32:0] y);\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:1:43: error: elaboration needs more than 64 net bits");
}

static void bytes_64k(odin3_elab_limits *limits) {
    limits->max_bytes = 65536;
}

static void test_byte_budget(void) {
    lower_limits(bytes_64k);
    read_text("module top;\n  wire w;\nendmodule\n", ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    et_expect(&rs, ODIN3_LOG_ERROR,
              "elaboration needs more than 65536 bytes (the budget is 60% of physical memory)");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    expect_ir_ok();
}

/* --- the context's helpers (direct) ------------------------------------------------------------
 */

/* A stage-1 result with stage 2's context over it, bound, with the ports of every module made. */
static odin3_elab2 *context_over(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_elab2 *e2 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_create(&rs.ctx, rs.elab, &rs.opts, &e2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab_bind(rs.elab, &e2->bind));
    e2->st = odin3_ast_symtab(rs.elab);
    return e2;
}

static odin3_ast_id first_module(void) {
    return odin3_ast_child(rs.elab, odin3_ast_root(rs.elab, 0), 0);
}

static void test_context_types(void) {
    odin3_elab2 *e2 = context_over("module top;\nendmodule\n");
    static const struct {
        odin3_elab2_type type;
        const char *name;
    } k_cases[] = {{ODIN3_E2T_ADD, "$add"},
                   {ODIN3_E2T_SHIFTX, "$shiftx"},
                   {ODIN3_E2T_REDUCE_BOOL, "$reduce_bool"},
                   {ODIN3_E2T_MUX, "$mux"},
                   {ODIN3_E2T_MEM, "$mem"},
                   {ODIN3_E2T_PROC, "$proc"},
                   {ODIN3_E2T_CONST0, "$_CONST0_"},
                   {ODIN3_E2T_CONSTZ, "$_CONSTZ_"}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        odin3_celltype_id found = {0};
        TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(rs.design, id_of(k_cases[i].name), &found),
                                 k_cases[i].name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(found.v, odin3_elab2_type_id(e2, k_cases[i].type).v,
                                         k_cases[i].name);
    }
    for (unsigned type = 0; type < (unsigned)ODIN3_E2T_COUNT; type++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_elab2_type_id(e2, (odin3_elab2_type)type).v);
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab2_type_id(e2, ODIN3_E2T_COUNT).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab2_type_id(NULL, ODIN3_E2T_ADD).v);
    odin3_elab2_destroy(e2);
    odin3_elab2_destroy(NULL);
}

static void test_context_create_misuse(void) {
    et_src src = {"t.v", "module top;\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
    odin3_elab2 *e2 = (odin3_elab2 *)1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_create(NULL, rs.elab, NULL, &e2));
    TEST_ASSERT_NULL(e2);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_create(&rs.ctx, NULL, NULL, &e2));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_create(&rs.ctx, rs.parsed, NULL, &e2));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_create(&rs.ctx, rs.elab, NULL, NULL));
    et_expect(&rs, ODIN3_LOG_ERROR, "odin3_elab2_create: not an elaborated store of this design");
}

/* IR-4: the module's shared constant nets, made on first use. */
static odin3_net_id const_of(odin3_elab2 *e2, odin3_bit bit) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_const_net(e2, bit, &net));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, net.v);
    return net;
}

static void test_const_net(void) {
    odin3_elab2 *e2 = context_over("module top;\nendmodule\n");
    odin3_net_id none = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_const_net(e2, ODIN3_BIT_1, &none));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_ports(e2, first_module()));
    odin3_module *top = module_named("top");
    uint32_t nodes = odin3_module_node_end(top);
    odin3_net_id one = const_of(e2, ODIN3_BIT_1);
    TEST_ASSERT_EQUAL_UINT32(one.v, const_of(e2, ODIN3_BIT_1).v);
    TEST_ASSERT_EQUAL_UINT32(nodes + 1, odin3_module_node_end(top)); /* made once */
    static const odin3_bit k_bits[] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_Z};
    static const odin3_const k_values[] = {ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_X,
                                           ODIN3_CONST_Z};
    for (unsigned index = 0; index < 4; index++) {
        odin3_net_id net = const_of(e2, k_bits[index]);
        TEST_ASSERT_EQUAL_INT(k_values[index], odin3_net_const_value(top, net));
        for (unsigned other = 0; other < index; other++) {
            TEST_ASSERT_NOT_EQUAL_UINT32(const_of(e2, k_bits[other]).v, net.v);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_const_net(e2, (odin3_bit)4, &none));
    odin3_elab2_destroy(e2);
}

/* EL-45: locs = node, then the call when inlined; ast = node; hier = <spec>/<scope path>. */
static odin3_prov_id prov_of(odin3_elab2 *e2, odin3_elab2_prov_req req) {
    odin3_prov_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_prov(e2, &req, &out));
    return out;
}

static void test_prov_call(void) {
    odin3_elab2 *e2 = context_over("module top;\n  wire first;\n  wire second;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_ports(e2, first_module()));
    odin3_ast_id first = declarator_named(rs.elab, "first");
    odin3_ast_id second = declarator_named(rs.elab, "second");
    odin3_prov_id plain = prov_of(e2, (odin3_elab2_prov_req){first, {0}, 0});
    odin3_prov_id inlined = prov_of(e2, (odin3_elab2_prov_req){first, second, 0});
    odin3_prov_id other = prov_of(e2, (odin3_elab2_prov_req){second, {0}, 0});
    TEST_ASSERT_EQUAL_UINT32(inlined.v, prov_of(e2, (odin3_elab2_prov_req){first, second, 0}).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(plain.v, inlined.v); /* two call sites differ */
    TEST_ASSERT_NOT_EQUAL_UINT32(inlined.v, other.v);
    const odin3_prov_record *rec = odin3_prov_get(rs.design, inlined);
    TEST_ASSERT_EQUAL_UINT32(2, rec->n_locs);
    TEST_ASSERT_EQUAL_UINT32(2, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(3, rec->locs[1].line);
    TEST_ASSERT_EQUAL_UINT32(first.v, rec->ast);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_prov_get(rs.design, plain)->n_locs);
    TEST_ASSERT_EQUAL_UINT32(rs.ctx.run.v, rec->run.v);
    odin3_elab2_destroy(e2);
}

/* The error cap, the stop line once, and warn once per (loc, format). */
static void errors_3(odin3_elab_limits *limits) {
    limits->max_errors = 3;
}

static void test_context_diagnostics(void) {
    lower_limits(errors_3);
    odin3_elab2 *e2 = context_over("module top;\n  wire a;\n  wire b;\nendmodule\n");
    odin3_loc at = odin3_ast_loc(rs.elab, declarator_named(rs.elab, "a"));
    for (int i = 0; i < 5; i++) {
        odin3_elab2_error(e2, at, "boom %d", i);
    }
    TEST_ASSERT_TRUE(odin3_elab2_stopped(e2));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:8: error: boom 2");
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:8: error: too many errors; elaboration stopped after 3");
    TEST_ASSERT_EQUAL_INT(-1, et_find(&rs, ODIN3_LOG_ERROR, "t.v:2:8: error: boom 3"));
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR, "too many errors"));
    odin3_elab2_destroy(e2);
}

static void test_context_warn_once(void) {
    odin3_elab2 *e2 = context_over("module top;\n  wire a;\n  wire b;\nendmodule\n");
    odin3_loc first = odin3_ast_loc(rs.elab, declarator_named(rs.elab, "a"));
    odin3_loc second = odin3_ast_loc(rs.elab, declarator_named(rs.elab, "b"));
    odin3_elab2_warn(e2, first, "careful %d", 1);
    odin3_elab2_warn(e2, first, "careful %d", 2); /* the same loc and format: once */
    odin3_elab2_warn(e2, second, "careful %d", 3);
    odin3_elab2_warn(e2, first, "other");
    TEST_ASSERT_EQUAL_UINT32(3, rs.warnings);
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:2:8: warning: careful 1");
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:3:8: warning: careful 3");
    et_expect(&rs, ODIN3_LOG_WARN, "t.v:2:8: warning: other");
    TEST_ASSERT_FALSE(odin3_elab2_stopped(e2));
    TEST_ASSERT_EQUAL_UINT32(0, e2->errors); /* a warning is not an error */
    odin3_elab2_destroy(e2);
}

/* The budget and the object counter: the check runs every 2^16 objects. */
static void test_context_account(void) {
    odin3_elab2 *e2 = context_over("module top;\nendmodule\n");
    odin3_elab2_counts few = {1, 1, 1, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_account(e2, &few));
    TEST_ASSERT_EQUAL_UINT64(48 + 28 + 48 + 64, e2->est_bytes);
    TEST_ASSERT_EQUAL_UINT64(4, e2->objects);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_budget(e2));
    odin3_elab_limits tight = *odin3_elab_limits_get();
    tight.max_bytes = 1;
    odin3_elab_test_set_limits(&tight);
    e2->limits = odin3_elab_limits_get();
    odin3_elab2_counts mid = {1, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_account(e2, &mid)); /* far below 2^16 objects */
    odin3_elab2_counts many = {1U << 16, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_elab2_account(e2, &many));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "elaboration needs more than 1 bytes (the budget is 60% of physical memory)");
    TEST_ASSERT_TRUE(odin3_elab2_stopped(e2));
    odin3_elab2_destroy(e2);
}

/* --- determinism -------------------------------------------------------------------------------
 */

static const char k_rich[] =
    "module top (input signed [0:7] a, input b, output [3:0] c);\n  wire [-1:2] n;\n"
    "  genvar i;\n  for (i = 0; i < 3; i = i + 1) begin : g\n    wire t;\n  end\n"
    "  if (1) begin wire z; end\n  reg signed [7:0] r;\n  (* keep *) integer k;\nendmodule\n";

/* write_rtlil of the rich module as text. */
static void rtlil_of_rich(char *out, size_t cap) {
    read_clean(k_rich, ODIN3_KEEP_AST_NONE);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr("rich.il")));
    slurp("rich.il", out, cap);
    TEST_ASSERT_EQUAL_INT(0, remove("rich.il"));
}

static void test_deterministic_rtlil(void) {
    static char first[FILE_MAX];
    static char second[FILE_MAX];
    rtlil_of_rich(first, sizeof first);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    rtlil_of_rich(second, sizeof second);
    TEST_ASSERT_NOT_NULL(strstr(first, "wire width 4 upto offset -1 \\n"));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* --- the ABI entry odin3_ast_elaborate ---------------------------------------------------------
 */

/* A node of the hand-built store, through the ABI builders only (the slang seam). */
static uint32_t node_of(odin3_ast *ast, odin3_ast_spec spec, const uint32_t *kids, uint32_t count) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, kids, count, &id));
    return id;
}

static uint32_t name_id(odin3_ast *ast, const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_intern(ast, text, &id));
    return id;
}

/* The hand-built store: a store, the location of its FILE buffer's first byte, its run. */
typedef struct abi_store {
    odin3_ast *ast;
    uint32_t run;
    uint32_t buf;
    uint32_t base; /* loc of byte 0 of adapter.sv */
} abi_store;

/* The loc of byte offset of the store's buffer. */
static uint32_t loc_at(const abi_store *store, uint32_t offset) {
    odin3_srcman *sm = NULL;
    uint32_t loc = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(rs.design, &sm));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_loc(sm, store->buf, offset, &loc));
    return loc;
}

/* A 32-bit signed decimal NUMBER (point (bb)) of the bits 0..255 value, at offset. */
typedef struct int_req {
    uint32_t offset;
    unsigned value;
} int_req;

static uint32_t int_node(const abi_store *store, int_req req) {
    char bits[33];
    for (unsigned bit = 0; bit < 32; bit++) {
        bits[bit] = (req.value >> (31U - bit)) & 1U ? '1' : '0';
    }
    bits[32] = '\0';
    uint32_t loc = loc_at(store, req.offset);
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER,
                           .sub = ODIN3_AST_NUMBER_DEC,
                           .flags = ODIN3_AST_F_NUMBER_SIGNED,
                           .loc = loc,
                           .end = loc + 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_number_new_str(store->ast, loc, 32, bits, &spec.payload));
    return node_of(store->ast, spec, NULL, 0);
}

/* A DECLARATOR named text at offset (no initialiser). */
static uint32_t declarator_node(const abi_store *store, const char *text, uint32_t offset) {
    uint32_t loc = loc_at(store, offset);
    uint32_t no_value[1] = {0};
    odin3_ast_spec spec = {.kind = ODIN3_AST_DECLARATOR,
                           .loc = loc,
                           .end = loc + 1,
                           .name = name_id(store->ast, text)};
    return node_of(store->ast, spec, no_value, 1);
}

/* `input a` at offsets 14 .. 21 (the DECLARATOR a at 20). */
static uint32_t port_node(const abi_store *store) {
    uint32_t kids[2] = {0, declarator_node(store, "a", 20)};
    odin3_ast_spec spec = {.kind = ODIN3_AST_PORT_DECL,
                           .sub = ODIN3_AST_PORT_DECL_INPUT,
                           .flags = ODIN3_AST_F_PORT_DECL_IN_HEADER |
                                    (1U << ODIN3_AST_F_PORT_DECL_NET_SHIFT),
                           .loc = loc_at(store, 14),
                           .end = loc_at(store, 21)};
    return node_of(store->ast, spec, kids, 2);
}

/* `wire [3:0] <name>;` at offsets 30 .. 44 (the DECLARATOR at 41). */
static uint32_t net_node(const abi_store *store, const char *name) {
    uint32_t bounds[2] = {int_node(store, (int_req){36, 3}), int_node(store, (int_req){38, 0})};
    odin3_ast_spec range_spec = {
        .kind = ODIN3_AST_RANGE, .loc = loc_at(store, 35), .end = loc_at(store, 40)};
    uint32_t kids[4] = {0, node_of(store->ast, range_spec, bounds, 2), 0,
                        declarator_node(store, name, 41)};
    odin3_ast_spec spec = {.kind = ODIN3_AST_NET_DECL,
                           .sub = ODIN3_AST_NET_DECL_WIRE,
                           .loc = loc_at(store, 30),
                           .end = loc_at(store, 44)};
    return node_of(store->ast, spec, kids, 4);
}

/* A new elaborated store held by rs.design under a pass run of its own, over a FILE buffer
 * adapter.sv. */
static abi_store open_store(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(rs.design, et_intern(&rs, "adapter"), &ctx));
    abi_store out = {NULL, ctx.run.v, 0, 0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_new_ast(rs.design, ODIN3_AST_FORM_ELABORATED, out.run, &out.ast));
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(rs.design, &sm));
    odin3_srcfile_spec file = {.name = name_id(out.ast, "adapter.sv"), .len = 200};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_file(sm, &file, &out.buf));
    out.base = loc_at(&out, 0);
    return out;
}

enum { MODULE_ITEMS = 4 };

/* `module <name> (input a); wire [3:0] w; <extra items> endmodule`; the extras follow the net. */
static uint32_t module_node(const abi_store *store, const char *name, const uint32_t *extra,
                            uint32_t count) {
    uint32_t list_kids[1] = {port_node(store)};
    uint32_t list = node_of(store->ast, (odin3_ast_spec){.kind = ODIN3_AST_LIST}, list_kids, 1);
    uint32_t kids[3 + MODULE_ITEMS] = {0, list, net_node(store, "w")};
    TEST_ASSERT_TRUE(count <= MODULE_ITEMS);
    for (uint32_t index = 0; index < count; index++) {
        kids[3 + index] = extra[index];
    }
    odin3_ast_spec named = {.kind = ODIN3_AST_MODULE,
                            .loc = store->base,
                            .end = store->base + 60,
                            .name = name_id(store->ast, name)};
    return node_of(store->ast, named, kids, 3 + count);
}

/* The UNIT named unit_name over the modules, then the seal. */
static void seal_store(const abi_store *store, const char *unit_name, const uint32_t *modules,
                       uint32_t count) {
    odin3_ast_spec unit = {.kind = ODIN3_AST_UNIT,
                           .loc = store->base,
                           .end = store->base + 60,
                           .name = name_id(store->ast, unit_name)};
    (void)node_of(store->ast, unit, modules, count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(store->ast));
}

/* The store `module <name> (input a); wire [3:0] w; endmodule` on the FILE buffer adapter.sv,
 * held by rs.design with a pass run of its own, sealed. */
static abi_store build_store(const char *module_name) {
    abi_store out = open_store();
    uint32_t module = module_node(&out, module_name, NULL, 0);
    seal_store(&out, module_name, &module, 1);
    return out;
}

/* A richer store: `top` (input a; wire [3:0] w; a generate block g[1] holding `(* keep *) wire
 * [3:0] t;`) and `sub` (input a; wire [3:0] w), so stage 2 meets a scope, an attribute and two
 * modules. */
static abi_store build_rich_store(void) {
    abi_store out = open_store();
    uint32_t inner = net_node(&out, "t");
    uint32_t keep = node_of(out.ast,
                            (odin3_ast_spec){.kind = ODIN3_AST_ATTR,
                                             .loc = loc_at(&out, 25),
                                             .end = loc_at(&out, 29),
                                             .name = name_id(out.ast, "keep")},
                            (uint32_t[1]){0}, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_attach(out.ast, inner, &keep, 1));
    uint32_t gen_kids[2] = {int_node(&out, (int_req){26, 1}), inner};
    uint32_t gen = node_of(out.ast,
                           (odin3_ast_spec){.kind = ODIN3_AST_GEN_BLOCK,
                                            .loc = loc_at(&out, 24),
                                            .end = loc_at(&out, 46),
                                            .name = name_id(out.ast, "g")},
                           gen_kids, 2);
    uint32_t modules[2] = {module_node(&out, "top", &gen, 1), module_node(&out, "sub", NULL, 0)};
    seal_store(&out, "top", modules, 2);
    return out;
}

static void abi_setup(void) {
    et_req req = {NULL, 0, NULL, NULL, NULL};
    et_setup(&req, &rs);
}

static void test_abi_elaborate(void) {
    abi_setup();
    abi_store store = build_store("top");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_elaborate(rs.design, store.ast, NULL),
                                  et_log(&rs));
    odin3_module *top = module_named("top");
    expect_wire(top, "a", (wire_shape){0, 0, false});
    expect_wire(top, "w", (wire_shape){3, 0, false});
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top).v, odin3_design_top(rs.design).v);
    TEST_ASSERT_NOT_NULL(odin3_ast_symtab(store.ast)); /* the store kept its symbol table */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_ast_count(rs.design)); /* still held */
    const odin3_prov_record *rec = wire_prov(top, "w");
    TEST_ASSERT_EQUAL_UINT32(store.run, rec->run.v);
    TEST_ASSERT_EQUAL_STRING("adapter.sv", str_of(rec->locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(1, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(42, rec->locs[0].col);
    TEST_ASSERT_EQUAL_STRING("top", str_of(rec->hier));
    TEST_ASSERT_NOT_EQUAL_UINT32(
        0, find_with_prefix("read_verilog: elaborated top 'top': 1 modules") + 1);
    expect_ir_ok();
    /* a second call: the store has its symbol table now */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, store.ast, NULL));
}

/* The richer store: both modules, the generate block's scoped wire with its attribute. */
static void test_abi_rich_store(void) {
    abi_setup();
    abi_store store = build_rich_store();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_elaborate(rs.design, store.ast, NULL),
                                  et_log(&rs));
    odin3_module *top = module_named("top");
    expect_wire(top, "g[1].t", (wire_shape){3, 0, false});
    TEST_ASSERT_NOT_NULL(wire_attr(top, (attr_ref){"g[1].t", "keep"}));
    TEST_ASSERT_EQUAL_STRING("top/g[1]", str_of(wire_prov(top, "g[1].t")->hier));
    expect_wire(module_named("sub"), "w", (wire_shape){3, 0, false});
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top).v, odin3_design_top(rs.design).v);
    expect_ir_ok();
}

static void test_abi_options_and_misuse(void) {
    abi_setup();
    abi_store store = build_store("top");
    odin3_elab_options bad = {0};
    bad.reserved[3] = 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, store.ast, &bad));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(NULL, store.ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_module_end(rs.design) - 1);
    odin3_elab_options ok = {.nolatches = true, .nomem2reg = true, .top = 7, .auto_blackbox = true};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_elaborate(rs.design, store.ast, &ok),
                                  et_log(&rs));
    TEST_ASSERT_TRUE(has_module("top"));
}

static void test_abi_parsed_store(void) {
    abi_setup();
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(rs.design, et_intern(&rs, "adapter"), &ctx));
    odin3_ast *parsed = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_new_ast(rs.design, ODIN3_AST_FORM_PARSED, ctx.run.v, &parsed));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, parsed, NULL));
    et_expect(&rs, ODIN3_LOG_ERROR, "odin3_ast_elaborate: the store is not elaborated");
}

/* A store the design does not hold, or of a run the design does not know, is refused. */
static void test_abi_foreign_store(void) {
    abi_setup();
    odin3_ast *loose = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(rs.design, ODIN3_AST_FORM_ELABORATED,
                                                     (odin3_passrun_id){1}, &loose));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, loose, NULL));
    odin3_ast_destroy(loose);
    odin3_ast *unknown_run = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_new_ast(rs.design, ODIN3_AST_FORM_ELABORATED, 9, &unknown_run));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, unknown_run, NULL));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "odin3_ast_elaborate: the store's run is not a run of the design");
}

/* An adapter's store is untrusted: rule 7 runs in every build, a violation is INVALID_ARG. */
static void test_abi_breaks_form(void) {
    abi_setup();
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(rs.design, et_intern(&rs, "adapter"), &ctx));
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_new_ast(rs.design, ODIN3_AST_FORM_ELABORATED, ctx.run.v, &ast));
    uint32_t unit =
        node_of(ast, (odin3_ast_spec){.kind = ODIN3_AST_UNIT, .name = name_id(ast, "t")}, NULL, 0);
    (void)unit;
    uint32_t second =
        node_of(ast, (odin3_ast_spec){.kind = ODIN3_AST_UNIT, .name = name_id(ast, "u")}, NULL, 0);
    (void)second;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_elaborate(rs.design, ast, NULL));
    et_expect(&rs, ODIN3_LOG_ERROR, "odin3_ast_elaborate: the store breaks the elaborated form");
}

/* A MODULE named like a cell type of the design: a located error, nothing made. */
static void test_abi_module_named_like_cell_type(void) {
    abi_setup();
    abi_store store = build_store("$and");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_ast_elaborate(rs.design, store.ast, NULL));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "adapter.sv:1:1: error: module '$and' has the name of a cell type of the design; "
              "rename the module");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    expect_ir_ok();
}

/* --- out of memory -----------------------------------------------------------------------------
 */

/* What the quiet sink saw of a run (it allocates nothing, so an injected failure is not its). */
static bool g_saw_missing_file;

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    g_saw_missing_file |= strstr(msg, "missing_file:") != NULL;
}

static const char k_three[] =
    "module sub #(parameter W = 1) (input [W-1:0] a, output [W-1:0] y);\n"
    "  wire [W-1:0] mid;\n  (* keep *) wire flag;\nendmodule\n"
    "module top (input [7:0] i, output [7:0] o);\n  sub #(.W(8)) u1 (.a(i), .y(o));\n"
    "  sub #(.W(4)) u2 (.a(i[3:0]), .y());\nendmodule\n";

/* A read that ran to its own end: the three modules with their ports. */
static bool read_complete(void) {
    if (odin3_design_module_end(rs.design) != 4) {
        return false;
    }
    for (uint32_t id = 1; id <= 3; id++) {
        if (odin3_module_port_count(odin3_module_get(rs.design, (odin3_module_id){id})) != 2) {
            return false;
        }
    }
    return true;
}

/* A fresh design under the quiet sink (no allocation of the sink's own). */
static void fresh_quiet_design(void) {
    odin3_design_destroy(rs.design);
    rs.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(rs.design);
    odin3_log_set_sink(quiet_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
}

static void expect_ir_ok_quiet(void) {
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(rs.design, full));
}

/* One run of the whole read with the limit-th allocation failing. */
static odin3_status read_failing_at(long limit) {
    fresh_quiet_design();
    g_saw_missing_file = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    odin3_util_set_alloc_fail_after(-1);
    /* an injected allocation failure is NO_MEMORY, never a user error and never a missing file */
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_FALSE(g_saw_missing_file);
    expect_ir_ok_quiet();
    return st;
}

/* Every allocation of the whole read fails in turn (stage 2's among them): the status is NO_MEMORY,
 * the IR made so far passes check FULL, nothing leaks (ASan's leak check ends the process), until
 * the read completes (OOM_SETTLE runs in a row). */
static void test_oom_sweep(void) {
    et_src src = {"t.v", k_three};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    uint32_t failures = 0;
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = read_failing_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK && read_complete() ? settled + 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    TEST_ASSERT_GREATER_THAN_UINT32(100, failures);
}

/* One run of the ABI entry on a fresh store with the limit-th allocation failing. */
static odin3_status elaborate_failing_at(long limit) {
    fresh_quiet_design();
    abi_store store = build_rich_store();
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_ast_elaborate(rs.design, store.ast, NULL);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_ERR_NO_MEMORY || st == ODIN3_OK);
    expect_ir_ok_quiet();
    return st;
}

/* The same sweep through the ABI entry, whose store the design holds. */
static void test_oom_abi(void) {
    abi_setup();
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    uint32_t failures = 0;
    for (long limit = 0; limit < OOM_LIMIT && st == ODIN3_ERR_NO_MEMORY; limit++) {
        st = elaborate_failing_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_UINT32(20, failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wires);
    RUN_TEST(test_wire_nets);
    RUN_TEST(test_scoped_names);
    RUN_TEST(test_block_names);
    RUN_TEST(test_probe_names);
    RUN_TEST(test_function_locals_make_no_wire);
    RUN_TEST(test_memory_makes_no_wire);
    RUN_TEST(test_implicit_net);
    RUN_TEST(test_ports);
    RUN_TEST(test_port_one_bit_range);
    RUN_TEST(test_non_ansi_ports);
    RUN_TEST(test_ports_before_bodies);
    RUN_TEST(test_provenance);
    RUN_TEST(test_provenance_specialization);
    RUN_TEST(test_provenance_nested);
    RUN_TEST(test_attributes);
    RUN_TEST(test_port_attributes);
    RUN_TEST(test_non_ansi_port_attributes);
    RUN_TEST(test_module_attributes);
    RUN_TEST(test_top_and_summary);
    RUN_TEST(test_summary_specialization_count);
    RUN_TEST(test_keep_none);
    RUN_TEST(test_keep_elaborated);
    RUN_TEST(test_keep_all);
    RUN_TEST(test_keep_after_failure);
    RUN_TEST(test_second_read_duplicate_module);
    RUN_TEST(test_second_read_other_module);
    RUN_TEST(test_refused_port_stops_stage);
    RUN_TEST(test_pending_always);
    RUN_TEST(test_pending_once_per_kind);
    RUN_TEST(test_pending_initialisers);
    RUN_TEST(test_forced_pending_declaration);
    RUN_TEST(test_net_bits_cap);
    RUN_TEST(test_net_bits_ports_and_exact);
    RUN_TEST(test_byte_budget);
    RUN_TEST(test_context_types);
    RUN_TEST(test_context_create_misuse);
    RUN_TEST(test_const_net);
    RUN_TEST(test_prov_call);
    RUN_TEST(test_context_diagnostics);
    RUN_TEST(test_context_warn_once);
    RUN_TEST(test_context_account);
    RUN_TEST(test_deterministic_rtlil);
    RUN_TEST(test_abi_elaborate);
    RUN_TEST(test_abi_rich_store);
    RUN_TEST(test_abi_options_and_misuse);
    RUN_TEST(test_abi_parsed_store);
    RUN_TEST(test_abi_foreign_store);
    RUN_TEST(test_abi_breaks_form);
    RUN_TEST(test_abi_module_named_like_cell_type);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_oom_abi);
    return UNITY_END();
}
