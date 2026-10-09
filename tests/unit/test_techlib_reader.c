/*
 * test_techlib_reader.c — unit tests for the .o3lib reader and the width-expression rule.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "techlib/expr.h"
#include "techlib/reader.h"
#include "techlib/width.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MSG_MAX = 1024, PATH_MAX_LEN = 64, DEEP_TERMS = 80, WIDE_PORTS = 17, NAME_MAX_LEN = 32 };

static odin3_design *g_design;
static char g_msg[MSG_MAX]; /* last error logged since setUp (located context comes last) */
static unsigned g_errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
        g_errors++;
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    g_errors = 0;
    odin3_log_set_sink(sink, NULL);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
}

static odin3_status read_into(odin3_design *design, const char *text) {
    const odin3_techlib_text src = {"t.o3lib", odin3_bytes_cstr(text)};
    return odin3_techlib_read_text(design, &src);
}

static void read_ok(const char *text) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_into(g_design, text), g_msg);
    TEST_ASSERT_EQUAL_UINT(0, g_errors);
}

static odin3_celltype_id type_of(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(name), &str)) {
        (void)odin3_celltype_find(g_design, str, &id);
    }
    return id;
}

static const odin3_celltype_def *def_of(const char *name) {
    const odin3_celltype_def *def = odin3_celltype_get(g_design, type_of(name));
    TEST_ASSERT_NOT_NULL_MESSAGE(def, name);
    return def;
}

static const odin3_techlib_cell *lib_of(const char *name) {
    const odin3_techlib_cell *lib = odin3_techlib_cell_get(g_design, type_of(name));
    TEST_ASSERT_NOT_NULL_MESSAGE(lib, name);
    return lib;
}

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(g_design), id);
}

static uint32_t width_of(const char *cell, uint32_t port, const odin3_value *params) {
    uint32_t width = 0;
    const odin3_port_query query = {type_of(cell), params, port};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_celltype_port_width_checked(g_design, &query, &width), g_msg);
    return width;
}

/* --- constructs ---------------------------------------------------------------------------- */

static const char k_and2[] = "library generic\n"
                             "cell AND2 gate area 1\n"
                             "  in A 1 ; in B 1 ; out Y 1\n"
                             "  fn Y = A & B\n"
                             "end\n";

static void test_gate_cell(void) {
    read_ok(k_and2);
    const odin3_celltype_def *def = def_of("AND2");
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BIT, def->gran);
    TEST_ASSERT_EQUAL_UINT32(3, def->n_ports);
    TEST_ASSERT_EQUAL_UINT32(0, def->n_params);
    static const char *const names[] = {"A", "B", "Y"};
    static const odin3_dir dirs[] = {ODIN3_DIR_IN, ODIN3_DIR_IN, ODIN3_DIR_OUT};
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], def->ports[i].name);
        TEST_ASSERT_EQUAL_INT(dirs[i], def->ports[i].dir);
        TEST_ASSERT_TRUE(def->ports[i].scalar);
        TEST_ASSERT_NULL(def->ports[i].width_expr);
        TEST_ASSERT_EQUAL_UINT32(1, width_of("AND2", i, NULL));
    }
}

static void test_gate_cell_library_data(void) {
    read_ok(k_and2);
    const odin3_techlib_cell *lib = lib_of("AND2");
    TEST_ASSERT_EQUAL_STRING("generic", str_of(lib->library));
    TEST_ASSERT_EQUAL_STRING("t.o3lib", lib->file);
    TEST_ASSERT_EQUAL_UINT32(2, lib->line);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_GATE, lib->kind);
    TEST_ASSERT_EQUAL_STRING("1", str_of(lib->area));
    TEST_ASSERT_EQUAL_UINT32(0, lib->delay);
    TEST_ASSERT_EQUAL_UINT32(3, lib->n_ports);
}

static void test_gate_cell_function(void) {
    read_ok(k_and2);
    const odin3_techlib_cell *lib = lib_of("AND2");
    TEST_ASSERT_EQUAL_UINT32(1, lib->n_fns);
    TEST_ASSERT_EQUAL_UINT32(2, lib->fns[0].port);
    TEST_ASSERT_EQUAL_UINT32(4, lib->fns[0].line);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BINARY, lib->fns[0].expr->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_AND, lib->fns[0].expr->op);
    TEST_ASSERT_EQUAL_STRING("A", str_of(lib->fns[0].expr->a->ident));
    TEST_ASSERT_EQUAL_UINT32(0, lib->n_seqs);
    TEST_ASSERT_NULL(lib->memory);
}

static void test_comments_blank_lines_and_separators(void) {
    read_ok("# a library\r\n"
            "\n"
            "library L   # trailing comment\r\n"
            "  ;;  \n"
            "cell INV gate ; in A 1 ; out Y 1 ; fn Y = ~A ; end  # all on one line\n"
            "\t# indented comment\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_UNARY, lib_of("INV")->fns[0].expr->kind);
}

static void test_kinds_map_to_granularity(void) {
    read_ok("library L\n"
            "cell adder hard\n"
            "  in a 1 ; in b 1 ; in cin 1 ; out cout 1 ; out sumout 1\n"
            "  fn sumout = a ^ b ^ cin\n"
            "  fn cout = (a & b) | (a & cin) | (b & cin)\n"
            "end\n"
            "cell opaque blackbox delay 2.5\n"
            "  in w 3 ; out y 4 ; inout z 2\n"
            "end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, def_of("adder")->gran);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_HARD, lib_of("adder")->kind);
    TEST_ASSERT_EQUAL_UINT32(2, lib_of("adder")->n_fns);
    TEST_ASSERT_EQUAL_UINT32(4, lib_of("adder")->fns[0].port);
    TEST_ASSERT_EQUAL_UINT32(3, lib_of("adder")->fns[1].port);
    const odin3_celltype_def *opaque = def_of("opaque");
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, opaque->gran);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_INOUT, opaque->ports[2].dir);
    TEST_ASSERT_FALSE(opaque->ports[0].scalar);
    TEST_ASSERT_EQUAL_UINT32(4, width_of("opaque", 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_BLACKBOX, lib_of("opaque")->kind);
    TEST_ASSERT_EQUAL_STRING("2.5", str_of(lib_of("opaque")->delay));
    TEST_ASSERT_EQUAL_UINT32(0, lib_of("opaque")->n_fns);
}

static const char k_multiply[] = "library vtr\n"
                                 "cell multiply hard\n"
                                 "  param A_WIDTH int 36\n"
                                 "  param B_WIDTH int 36\n"
                                 "  in  a A_WIDTH signed\n"
                                 "  in  b B_WIDTH\n"
                                 "  out out A_WIDTH + B_WIDTH\n"
                                 "  fn  out = a * b\n"
                                 "end\n";

static void test_parametric_width(void) {
    read_ok(k_multiply);
    const odin3_celltype_def *def = def_of("multiply");
    TEST_ASSERT_EQUAL_UINT32(2, def->n_params);
    TEST_ASSERT_EQUAL_STRING("A_WIDTH", def->params[0].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, def->params[0].kind);
    TEST_ASSERT_EQUAL_INT64(36, def->params[1].dflt.i);
    TEST_ASSERT_EQUAL_STRING("A_WIDTH", def->ports[0].width_param);
    TEST_ASSERT_NULL(def->ports[2].width_param);
    TEST_ASSERT_NOT_NULL(def->ports[2].width_expr);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_BINARY, odin3_width_expr_tree(def->ports[2].width_expr)->kind);
}

static void test_parametric_width_evaluates(void) {
    read_ok(k_multiply);
    odin3_value params[2] = {odin3_value_int(36), odin3_value_int(36)};
    TEST_ASSERT_EQUAL_UINT32(72, width_of("multiply", 2, params));
    params[0] = odin3_value_int(8);
    params[1] = odin3_value_int(4);
    TEST_ASSERT_EQUAL_UINT32(12, width_of("multiply", 2, params));
    TEST_ASSERT_EQUAL_UINT32(8, width_of("multiply", 0, params));
    TEST_ASSERT_TRUE(lib_of("multiply")->ports[0].is_signed);
    TEST_ASSERT_FALSE(lib_of("multiply")->ports[1].is_signed);
}

/* Width evaluation failures are reported to the caller (who adds the location). */
/* One failing width query of multiply's port out: one error, located at the port and type. */
static void expect_width_error(const odin3_value *params, const char *reason) {
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want, "port_width: port 'out' of cell type 'multiply': %s", reason);
    unsigned before = g_errors;
    uint32_t width = 7;
    const odin3_port_query query = {type_of("multiply"), params, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_port_width_checked(g_design, &query, &width));
    TEST_ASSERT_EQUAL_UINT32(7, width);
    TEST_ASSERT_EQUAL_UINT(before + 1, g_errors);
    TEST_ASSERT_EQUAL_STRING(want, g_msg);
}

/* Width evaluation failures are reported once, with the port and cell type. */
static void test_parametric_width_rejects_bad_params(void) {
    read_ok(k_multiply);
    odin3_value params[2] = {odin3_value_int(-40), odin3_value_int(4)};
    expect_width_error(params, "width -36 is outside 0..4294967295");
    params[0] = odin3_value_int(INT64_MAX);
    expect_width_error(params, "integer overflow");
    params[0] = odin3_value_int(UINT32_MAX);
    expect_width_error(params, "width 4294967299 is outside 0..4294967295");
    params[0].kind = ODIN3_VAL_STRING;
    expect_width_error(params, "parameter 'A_WIDTH' is not an int");
}

/* A width without identifiers is folded to a constant, so an IR-7b declaration matches it. */
static void test_constant_width_expression_folds(void) {
    read_ok(
        "library L\ncell cst hard ; in a 2 * 2 ; in b (8 >> 3) ; out y 1 ; fn y = a[0] ; end\n");
    const odin3_celltype_def *def = def_of("cst");
    TEST_ASSERT_NULL(def->ports[0].width_expr);
    TEST_ASSERT_EQUAL_UINT32(4, def->ports[0].width);
    TEST_ASSERT_TRUE(def->ports[1].scalar);
    const odin3_port_def ports[3] = {{"a", ODIN3_DIR_IN, false, 4, NULL, NULL, NULL},
                                     {"b", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                     {"y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};
    const odin3_celltype_def decl = {"cst", ODIN3_GRAN_BLACKBOX, 0, ports, 3, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(g_design, &decl, &id));
    TEST_ASSERT_EQUAL_UINT32(type_of("cst").v, id.v);
}

/* The check hook names the identifier that does not resolve against a definition. */
static void test_width_check_names_identifier(void) {
    read_ok(k_multiply);
    const odin3_celltype_def *mul = def_of("multiply");
    const odin3_param_def params[2] = {{"X", ODIN3_VAL_INT, odin3_value_int(1)},
                                       {"Y", ODIN3_VAL_INT, odin3_value_int(1)}};
    const odin3_celltype_def def = {"mult2", ODIN3_GRAN_HARD, 0, &mul->ports[2], 1, params, 2, NULL,
                                    NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(g_design, &def, NULL));
    TEST_ASSERT_EQUAL_STRING("add_local: cell type 'mult2': width expression names 'A_WIDTH', "
                             "which is not an int parameter of the type",
                             g_msg);
}

static void test_three_cells_register_three_types(void) {
    read_ok("library mixed\n"
            "cell add_w hard ; param W int 8\n"
            "  in a W ; in b W ; out s W + 1\n"
            "  fn s = a + b\n"
            "end\n"
            "cell shl hard\n"
            "  param N int 4 ; param S int 2\n"
            "  in a N ; out y N << S ; fn y = a << S\n"
            "end\n"
            "cell pick hard ; param W int 3 ; param K int 2\n"
            "  in a W * K ; in sel K > 1 ? 2 : 1 ; out y W ; fn y = a[2:0]\n"
            "end\n");
    TEST_ASSERT_TRUE(odin3_celltype_valid(type_of("add_w")));
    TEST_ASSERT_TRUE(odin3_celltype_valid(type_of("shl")));
    TEST_ASSERT_TRUE(odin3_celltype_valid(type_of("pick")));
    const odin3_value w16[1] = {odin3_value_int(16)};
    TEST_ASSERT_EQUAL_UINT32(16, width_of("add_w", 0, w16));
    TEST_ASSERT_EQUAL_UINT32(17, width_of("add_w", 2, w16));
    const odin3_value shl[2] = {odin3_value_int(3), odin3_value_int(4)};
    TEST_ASSERT_EQUAL_UINT32(48, width_of("shl", 1, shl));
    const odin3_value pick[2] = {odin3_value_int(5), odin3_value_int(1)};
    TEST_ASSERT_EQUAL_UINT32(5, width_of("pick", 0, pick));
    TEST_ASSERT_EQUAL_UINT32(1, width_of("pick", 1, pick));
    TEST_ASSERT_EQUAL_UINT32(5, width_of("pick", 2, pick));
}

static void assert_mods(const odin3_techlib_port *mods, bool is_signed, bool clock) {
    TEST_ASSERT_EQUAL_INT(is_signed, mods->is_signed);
    TEST_ASSERT_EQUAL_INT(clock, mods->clock);
}

static void test_clock_and_signed_modifiers(void) {
    read_ok("library L\n"
            "cell DFFP gate area 4\n"
            "  in D 1 ; in C 1 clock ; out Q 1\n"
            "  seq Q <= D @ posedge C init x\n"
            "end\n"
            "cell M hard ; in a 4 clock signed ; in b 4 signed clock ; out y 4 signed\n"
            "  fn y = a + b\n"
            "end\n");
    const odin3_techlib_port *dff = lib_of("DFFP")->ports;
    assert_mods(&dff[0], false, false);
    assert_mods(&dff[1], false, true);
    const odin3_techlib_port *mods = lib_of("M")->ports;
    assert_mods(&mods[0], true, true);
    assert_mods(&mods[1], true, true);
    assert_mods(&mods[2], true, false);
}

static void test_seq_statements(void) {
    read_ok("library L\n"
            "cell DFFP gate ; in D 1 ; in C 1 clock ; out Q 1\n"
            "  seq Q <= D @ posedge C init x\n"
            "end\n"
            "cell DFFN gate ; in D 1 ; in C 1 clock ; out Q 1 ; seq Q<=~D@negedge C init 1 ; end\n"
            "cell DLATCHP gate ; in D 1 ; in E 1 clock ; out Q 1 ; seq Q <= D @ high E ; end\n"
            "cell DLATCHN gate ; in D 1 ; in E 1 clock ; out Q 1 ; seq Q <= D @ low E init 1'b0\n"
            "end\n");
    const odin3_techlib_cell *dffp = lib_of("DFFP");
    TEST_ASSERT_EQUAL_UINT32(0, dffp->n_fns);
    TEST_ASSERT_EQUAL_UINT32(1, dffp->n_seqs);
    TEST_ASSERT_EQUAL_UINT32(2, dffp->seqs[0].port);
    TEST_ASSERT_EQUAL_UINT32(1, dffp->seqs[0].clock);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_POSEDGE, dffp->seqs[0].trigger);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_IDENT, dffp->seqs[0].data->kind);
    TEST_ASSERT_NULL(dffp->seqs[0].init);
    TEST_ASSERT_EQUAL_UINT32(3, dffp->seqs[0].line);
    const odin3_techlib_seq *dffn = &lib_of("DFFN")->seqs[0];
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_NEGEDGE, dffn->trigger);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_UNARY, dffn->data->kind);
    TEST_ASSERT_NOT_NULL(dffn->init);
    TEST_ASSERT_EQUAL_INT64(1, dffn->init->ival);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_HIGH, lib_of("DLATCHP")->seqs[0].trigger);
    TEST_ASSERT_NULL(lib_of("DLATCHP")->seqs[0].init);
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_LOW, lib_of("DLATCHN")->seqs[0].trigger);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_SIZED, lib_of("DLATCHN")->seqs[0].init->kind);
}

static const char k_rams[] =
    "library vtr\n"
    "cell single_port_ram hard\n"
    "  param ADDR_WIDTH int 15 ; param DATA_WIDTH int 1\n"
    "  in addr ADDR_WIDTH ; in data DATA_WIDTH ; in we 1 ; in clk 1 clock\n"
    "  out out DATA_WIDTH\n"
    "  memory words 2 ** ADDR_WIDTH ; width DATA_WIDTH ; write sync clk we ; read sync clk\n"
    "end\n"
    "cell dual_port_ram hard\n"
    "  param ADDR_WIDTH int 15 ; param DATA_WIDTH int 1\n"
    "  in addr1 ADDR_WIDTH ; in addr2 ADDR_WIDTH ; in data1 DATA_WIDTH ; in data2 DATA_WIDTH\n"
    "  in we1 1 ; in we2 1 ; in clk 1 clock\n"
    "  out out1 DATA_WIDTH ; out out2 DATA_WIDTH\n"
    "  memory words 2 ** ADDR_WIDTH\n"
    "  width DATA_WIDTH\n"
    "  write sync clk we1 ; write sync clk we2\n"
    "  read sync clk ; read async\n"
    "end\n";

static void test_memory_statements(void) {
    read_ok(k_rams);
    const odin3_techlib_cell *sp = lib_of("single_port_ram");
    TEST_ASSERT_NOT_NULL(sp->memory);
    const odin3_techlib_memory *mem = sp->memory;
    TEST_ASSERT_EQUAL_UINT32(6, mem->line);
    TEST_ASSERT_EQUAL_INT(ODIN3_OP_POW, mem->words->op);
    TEST_ASSERT_EQUAL_INT(ODIN3_EXPR_IDENT, mem->width->kind);
    TEST_ASSERT_EQUAL_UINT32(2, mem->n_mports);
    TEST_ASSERT_TRUE(mem->mports[0].write);
    TEST_ASSERT_TRUE(mem->mports[0].sync);
    TEST_ASSERT_EQUAL_UINT32(2, mem->mports[0].n_ports);
    TEST_ASSERT_EQUAL_UINT32(3, mem->mports[0].ports[0]);
    TEST_ASSERT_EQUAL_UINT32(2, mem->mports[0].ports[1]);
}

static void test_memory_read_port_and_outputs(void) {
    read_ok(k_rams);
    const odin3_techlib_memory *mem = lib_of("single_port_ram")->memory;
    TEST_ASSERT_FALSE(mem->mports[1].write);
    TEST_ASSERT_EQUAL_UINT32(1, mem->mports[1].n_ports);
    TEST_ASSERT_EQUAL_UINT32(1, mem->n_outs);
    TEST_ASSERT_EQUAL_UINT32(4, mem->outs[0]);
}

static void test_memory_ports_and_widths(void) {
    read_ok(k_rams);
    const odin3_value params[2] = {odin3_value_int(10), odin3_value_int(32)};
    TEST_ASSERT_EQUAL_UINT32(10, width_of("single_port_ram", 0, params));
    TEST_ASSERT_EQUAL_UINT32(32, width_of("single_port_ram", 4, params));
    const odin3_techlib_memory *dp = lib_of("dual_port_ram")->memory;
    TEST_ASSERT_EQUAL_UINT32(4, dp->n_mports);
    TEST_ASSERT_FALSE(dp->mports[3].sync);
    TEST_ASSERT_EQUAL_UINT32(0, dp->mports[3].n_ports);
    TEST_ASSERT_EQUAL_UINT32(2, dp->n_outs);
    TEST_ASSERT_EQUAL_UINT32(7, dp->outs[0]);
    TEST_ASSERT_EQUAL_UINT32(8, dp->outs[1]);
}

/* fn and seq may drive some outputs; the memory drives the rest. */
static void test_memory_drives_remaining_outputs(void) {
    read_ok("library L\n"
            "cell rom hard ; in a 2 ; in clk 1 clock ; out q 8 ; out hit 1 ; out r 1\n"
            "  fn hit = a == 0 ; seq r <= a[0] @ posedge clk\n"
            "  memory words 4 ; width 8 ; read sync clk\n"
            "end\n");
    const odin3_techlib_cell *rom = lib_of("rom");
    TEST_ASSERT_EQUAL_UINT32(1, rom->memory->n_outs);
    TEST_ASSERT_EQUAL_UINT32(2, rom->memory->outs[0]);
}

static void test_fn_expressions_name_ports_and_params(void) {
    read_ok("library L\n"
            "cell sh hard ; param W int 8 ; param S int 3\n"
            "  in a W ; out y W ; out lo 4 ; out cat 2 * W\n"
            "  fn y = a >> S ; fn lo = a[3:0] ; fn cat = {a, {W{1'b0}}}\n"
            "end\n");
    TEST_ASSERT_EQUAL_UINT32(3, lib_of("sh")->n_fns);
    const odin3_value params[2] = {odin3_value_int(5), odin3_value_int(1)};
    TEST_ASSERT_EQUAL_UINT32(10, width_of("sh", 3, params));
}

static void test_failed_read_registers_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_into(g_design, "library L\n"
                                                               "cell OK1 gate\n"
                                                               "  in A 1 ; out Y 1\n"
                                                               "  fn Y = A\n"
                                                               "end\n"
                                                               "cell BAD gate\n"
                                                               "  out Y 1\n"
                                                               "end\n"));
    TEST_ASSERT_FALSE(odin3_celltype_valid(type_of("OK1")));
    TEST_ASSERT_FALSE(odin3_celltype_valid(type_of("BAD")));
}

static void test_cell_get_on_other_types(void) {
    TEST_ASSERT_NULL(odin3_techlib_cell_get(g_design, type_of("$port_in")));
    TEST_ASSERT_NULL(odin3_techlib_cell_get(g_design, (odin3_celltype_id){0}));
}

/* --- files --------------------------------------------------------------------------------- */

static void test_read_file(void) {
    char path[PATH_MAX_LEN] = "/tmp/odin3_techlib_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    FILE *file = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(k_and2), fwrite(k_and2, 1, strlen(k_and2), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    odin3_status st = odin3_techlib_read(g_design, path);
    const odin3_techlib_cell *lib = odin3_techlib_cell_get(g_design, type_of("AND2"));
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, g_msg);
    TEST_ASSERT_NOT_NULL(lib);
    TEST_ASSERT_EQUAL_STRING(path, lib->file);
}

static void test_read_missing_file(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_techlib_read(g_design, "/nonexistent/x.o3lib"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "/nonexistent/x.o3lib"));
}

/* --- errors -------------------------------------------------------------------------------- */

typedef struct bad_case {
    const char *text;
    const char *where; /* expected "file:line[:col]:" prefix */
    const char *what;  /* expected fragment of the message */
} bad_case;

#define CELL "library L\ncell C gate\n"

static const bad_case k_bad[] = {
    /* Review Focus 4: an output with no driver outside a blackbox */
    {CELL "  in A 1 ; out Y 1\nend\n", "t.o3lib:3:", "no fn, seq or memory"},
    {"library L\ncell H hard ; out Y 4 ; end\n", "t.o3lib:2:", "no fn, seq or memory"},
    {CELL "  out Y 1 ; fn Y = 1 ; fn Y = 0\nend\n", "t.o3lib:3:", "already driven"},
    {CELL "  in C 1 clock ; out Y 1 ; fn Y = 1 ; seq Y <= 0 @ posedge C\nend\n",
     "t.o3lib:3:", "already driven"},
    {"library L\ncell B blackbox ; out Y 1 ; fn Y = 1 ; end\n", "t.o3lib:2:", "blackbox"},
    {"library L\ncell B blackbox ; in c 1 ; out Y 1 ; seq Y <= 1 @ posedge c ; end\n",
     "t.o3lib:2:", "blackbox"},
    {"library L\ncell B blackbox ; out Y 1 ; memory words 2 ; end\n", "t.o3lib:2:", "blackbox"},
    {CELL "  in A 1 ; out Y 1 ; fn A = 1\nend\n", "t.o3lib:3:", "not an output"},
    {CELL "  out Y 1 ; fn Z = 1\nend\n", "t.o3lib:3:", "not an output"},
    {CELL "  out Y 1 ; fn Y 1\nend\n", "t.o3lib:3:", "'='"},
    {CELL "  out Y 1 ; fn Y =\nend\n", "t.o3lib:3:", "expression"},
    /* duplicate names */
    {CELL "  out Y 1 ; fn Y = 1\nend\ncell C gate\nend\n", "t.o3lib:5:", "duplicate cell"},
    {"library L\ncell $port_in gate\nend\n", "t.o3lib:2:", "already"},
    {CELL "  in A 1 ; in A 2\nend\n", "t.o3lib:3:", "duplicate"},
    {CELL "  param W int 1 ; param W int 2\nend\n", "t.o3lib:3:", "duplicate"},
    {CELL "  param W int 1 ; in W 2\nend\n", "t.o3lib:3:", "duplicate"},
    {CELL "  in W 2 ; param W int 1\nend\n", "t.o3lib:3:", "duplicate"},
    /* unknown identifiers (located at the identifier's column) */
    {CELL "  param A int 1\n  out y A + B\nend\n", "t.o3lib:4:13:", "'B'"},
    {CELL "  in p 1 ; out y p\nend\n", "t.o3lib:3:18:", "'p'"},
    {CELL "  out y Q\n  param Q int 2\nend\n", "t.o3lib:3:9:", "'Q'"},
    {CELL "  in a 1 ; out y 1\n  fn y = a & b\nend\n", "t.o3lib:4:14:", "'b'"},
    {CELL "  in a 1 ; out y 1 ; in c 1 clock\n  seq y <= q @ posedge c\nend\n",
     "t.o3lib:4:12:", "'q'"},
    {CELL "  in c 1 clock ; out y 1\n  seq y <= 1 @ posedge c init c\nend\n",
     "t.o3lib:4:31:", "'c'"},
    {CELL "  in a 1 ; out y 1 ; memory words N\nend\n", "t.o3lib:3:35:", "'N'"},
    /* library statement */
    {"cell C gate\nend\n", "t.o3lib:1:", "library"},
    {"# nothing\n", "t.o3lib:", "library"},
    {"library L\nlibrary M\n", "t.o3lib:2:", "library"},
    {"library\n", "t.o3lib:1:", "name"},
    {"library L extra\n", "t.o3lib:1:", "extra"},
    /* statement structure */
    {"library L\nfoo bar\n", "t.o3lib:2:", "unknown statement 'foo'"},
    {CELL "  in A 1\n", "t.o3lib:2:", "end"},
    {"library L\nend\n", "t.o3lib:2:", "outside a cell"},
    {"library L\nin A 1\n", "t.o3lib:2:", "outside a cell"},
    {CELL "cell D gate\n", "t.o3lib:3:", "inside cell"},
    {CELL "end extra\n", "t.o3lib:3:", "extra"},
    {"library L\ncell C\nend\n", "t.o3lib:2:", "gate, hard or blackbox"},
    {"library L\ncell C wire\nend\n", "t.o3lib:2:", "gate, hard or blackbox"},
    {"library L\ncell\n", "t.o3lib:2:", "name"},
    {"library L\ncell C gate area\nend\n", "t.o3lib:2:", "number"},
    {"library L\ncell C gate area 1x\nend\n", "t.o3lib:2:", "number"},
    {"library L\ncell C gate area .5\nend\n", "t.o3lib:2:", "number"},
    {"library L\ncell C gate area 1 area 2\nend\n", "t.o3lib:2:", "twice"},
    {"library L\ncell C gate size 1\nend\n", "t.o3lib:2:", "size"},
    /* parameters */
    {CELL "  param W bits 1\nend\n", "t.o3lib:3:", "int"},
    {CELL "  param W int\nend\n", "t.o3lib:3:", "expression"},
    {CELL "  param W int X + 1\nend\n", "t.o3lib:3:", "constant"},
    {CELL "  param W int 1 +\nend\n", "t.o3lib:3:", "expected"},
    {CELL "  param 9W int 1\nend\n", "t.o3lib:3:", "name"},
    /* ports */
    {CELL "  in A\nend\n", "t.o3lib:3:", "width"},
    {CELL "  in A signed\nend\n", "t.o3lib:3:", "width"},
    {CELL "  in A 0\nend\n", "t.o3lib:3:", "width"},
    {CELL "  param W int 0 ; in A W\nend\n", "t.o3lib:3:", "width"},
    {CELL "  param W int 2 ; in A W - 3\nend\n", "t.o3lib:3:", "width"},
    {CELL "  in A {2, 3}\nend\n", "t.o3lib:3:", "width"},
    {CELL "  in A (1\nend\n", "t.o3lib:3:", "("},
    {CELL "  out Y 1 clock ; fn Y = 1\nend\n", "t.o3lib:3:", "clock"},
    {CELL "  in A 1 clock clock\nend\n", "t.o3lib:3:", "twice"},
    {CELL "  in 1 1\nend\n", "t.o3lib:3:", "name"},
    /* seq */
    {CELL "  in c 1 clock ; out y 1 ; seq y = 1 @ posedge c\nend\n", "t.o3lib:3:", "'<='"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= 1 posedge c\nend\n", "t.o3lib:3:", "'@'"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= 1 @ rising c\nend\n", "t.o3lib:3:", "posedge"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= 1 @ posedge\nend\n", "t.o3lib:3:", "clock"},
    {CELL "  out c 1 ; out y 1 ; fn c = 0 ; seq y <= 1 @ posedge c\nend\n",
     "t.o3lib:3:", "not an input"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= 1 @ posedge c init\nend\n",
     "t.o3lib:3:", "expression"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= 1 @ posedge c reset 0\nend\n", "t.o3lib:3:", "init"},
    {CELL "  in c 1 clock ; out y 1 ; seq y <= @ posedge c\nend\n", "t.o3lib:3:", "expression"},
    /* memory */
    {CELL "  out y 1 ; width 4\nend\n", "t.o3lib:3:", "memory"},
    {CELL "  out y 1 ; read sync c\nend\n", "t.o3lib:3:", "memory"},
    {CELL "  out y 1 ; write sync c\nend\n", "t.o3lib:3:", "memory"},
    {CELL "  out y 1 ; memory words 2 ; memory words 4\nend\n", "t.o3lib:3:", "one memory"},
    {CELL "  out y 1 ; memory size 2\nend\n", "t.o3lib:3:", "words"},
    {CELL "  out y 1 ; memory words 2 ; width 1 ; width 2\nend\n", "t.o3lib:3:", "twice"},
    {CELL "  out y 1 ; memory words 2 ; read async\nend\n", "t.o3lib:3:", "width"},
    {CELL "  out y 1 ; memory words 2 ; width 1\nend\n", "t.o3lib:3:", "read"},
    {CELL "  in c 1 ; out y 1 ; memory words 2 ; width 1 ; read sometimes c\nend\n",
     "t.o3lib:3:", "sync or async"},
    {CELL "  in c 1 ; out y 1 ; memory words 2 ; width 1 ; read sync\nend\n",
     "t.o3lib:3:", "clock"},
    {CELL "  in c 1 ; out y 1 ; memory words 2 ; width 1 ; read sync d\nend\n",
     "t.o3lib:3:", "'d'"},
    {CELL "  in c 1 ; out y 1 ; memory words 2 ; width 1 ; read sync y\nend\n",
     "t.o3lib:3:", "not an input"},
    {CELL "  in c 1 ; out y 1 ; fn y = c ; memory words 2 ; width 1 ; read async\nend\n",
     "t.o3lib:3:", "drives no output"},
    {CELL "  in c 1 ; out y 1 ; memory words 0 ; width 1 ; read async\nend\n",
     "t.o3lib:3:", "words"},
    /* reserved names */
    {CELL "  param signed int 1\nend\n", "t.o3lib:3:9:", "reserved"},
    {CELL "  in clock 1\nend\n", "t.o3lib:3:6:", "reserved"},
    {CELL "  out x 1 ; fn x = 0\nend\n", "t.o3lib:3:7:", "reserved"},
    /* clocks must be declared with `clock` */
    {CELL "  in c 1 ; out y 1 ; seq y <= 1 @ posedge c\nend\n",
     "t.o3lib:3:43:", "not a clock input"},
    {CELL "  in e 1 ; out y 1 ; seq y <= 1 @ high e\nend\n", "t.o3lib:3:40:", "not a clock input"},
    {CELL "  in c 1 ; out y 1 ; memory words 2 ; width 1 ; read sync c\nend\n",
     "t.o3lib:3:59:", "not a clock input"},
    {CELL "  in c 1 clock ; in w 1 ; out y 1 ; memory words 2 ; width 1 ; write sync w c\n"
          "  read async\nend\n",
     "t.o3lib:3:75:", "not a clock input"},
    /* evaluation failures carry their reason in the located message */
    {CELL "  in A 8'bx\nend\n", "t.o3lib:3:8:",
     "width does not evaluate with the default "
     "parameters: sized literal contains x or z bits"},
    {CELL "  param W int 4 / 0\nend\n",
     "t.o3lib:3:17:", "the default of 'W' must be a constant integer: division by zero"},
};

static void run_bad(size_t idx, const bad_case *bad) {
    char label[2 * MSG_MAX];
    setUp();
    odin3_status st = read_into(g_design, bad->text);
    (void)snprintf(label, sizeof label, "case %zu: got \"%s\"", idx, g_msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, st, label);
    TEST_ASSERT_TRUE_MESSAGE(strncmp(g_msg, bad->where, strlen(bad->where)) == 0, label);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_msg, bad->what), label);
    TEST_ASSERT_FALSE_MESSAGE(odin3_celltype_valid(type_of("C")), label);
    tearDown();
}

static void test_malformed_libraries(void) {
    tearDown();
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        run_bad(i, &k_bad[i]);
    }
    setUp();
}

static void test_nul_byte(void) {
    static const char text[] = "library L\ncell C gate\0\nend\n";
    const odin3_techlib_text src = {"t.o3lib", {text, sizeof text - 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_techlib_read_text(g_design, &src));
    TEST_ASSERT_EQUAL_STRING("t.o3lib:2:12: NUL byte in the library", g_msg);
}

/* The same library read twice: the second read finds every cell already registered. */
static void test_second_read_rejects_existing_cells(void) {
    read_ok(k_and2);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_into(g_design, k_and2));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "t.o3lib:2:"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "AND2"));
}

/* --- width expression compile, walk and OOM ------------------------------------------------ */

static void test_collect_idents_order(void) {
    odin3_strtab *tab = odin3_design_strtab(g_design);
    odin3_arena *arena = odin3_celltype_arena(g_design);
    const odin3_expr_parser parser = {arena, tab, "w", 1};
    const odin3_expr *expr = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_expr_parse(&parser, odin3_bytes_cstr("a ? {b, c[d:1]} : e + a"), &expr));
    odin3_vec nodes;
    odin3_vec_init(&nodes, sizeof(const odin3_expr *));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_expr_collect_idents(expr, &nodes));
    static const char *const want[] = {"a", "b", "c", "d", "e", "a"};
    TEST_ASSERT_EQUAL_size_t(6, nodes.len);
    for (size_t i = 0; i < nodes.len; i++) {
        const odin3_expr *const *node = (const odin3_expr *const *)odin3_vec_cat(&nodes, i);
        TEST_ASSERT_EQUAL_STRING(want[i], odin3_strtab_get(tab, (*node)->ident));
    }
    odin3_vec_free(&nodes);
}

static const char k_full[] =
    "library oom # every construct once\n"
    "cell AND2 gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A & B ; end\n"
    "cell DFFP gate ; in D 1 ; in C 1 clock ; out Q 1 ; seq Q <= D @ posedge C init 1'b0 ; end\n"
    "cell multiply hard\n"
    "  param A_WIDTH int 36 ; param B_WIDTH int 36\n"
    "  in a A_WIDTH signed ; in b B_WIDTH ; out out A_WIDTH + B_WIDTH ; fn out = a * b\n"
    "end\n"
    "cell ram hard ; param AW int 4 ; param DW int 8\n"
    "  in addr AW ; in data DW ; in we 1 ; in clk 1 clock ; out out DW\n"
    "  memory words 2 ** AW ; width DW ; write sync clk we ; read sync clk\n"
    "end\n"
    "cell bb blackbox delay 3 ; in w 2 ; out y 2 ; end\n";

/* Checks a design that read k_full, then destroys it. */
static void check_full(odin3_design *design) {
    odin3_design *keep = g_design;
    g_design = design;
    const odin3_value params[2] = {odin3_value_int(3), odin3_value_int(5)};
    TEST_ASSERT_EQUAL_UINT32(8, width_of("multiply", 2, params));
    TEST_ASSERT_NOT_NULL(lib_of("ram")->memory);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def_of("bb")->gran);
    g_design = keep;
    odin3_design_destroy(design);
}

static void test_read_oom_sweep(void) {
    long tries = 0;
    for (;; tries++) {
        odin3_design *design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = read_into(design, k_full);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            check_full(design);
            break;
        }
        odin3_design_destroy(design);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, g_msg);
    }
    TEST_ASSERT_TRUE(tries > 20);
}

static void test_width_compile_oom_sweep(void) {
    odin3_strtab *tab = odin3_design_strtab(g_design);
    odin3_arena *arena = odin3_celltype_arena(g_design);
    const odin3_expr_parser parser = {arena, tab, "w", 1};
    const odin3_expr *expr = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_expr_parse(&parser, odin3_bytes_cstr("(A + B) * A"), &expr));
    uint32_t names[2] = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, odin3_bytes_cstr("B"), &names[0]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strtab_intern(tab, odin3_bytes_cstr("A"), &names[1]));
    const odin3_width_source src = {arena, tab, expr, names, 2};
    const odin3_width_expr *wexpr = NULL;
    long tries = 0;
    for (;; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = odin3_width_expr_compile(&src, &wexpr);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(wexpr);
    }
    TEST_ASSERT_TRUE(tries > 0);
    TEST_ASSERT_EQUAL_PTR(expr, odin3_width_expr_tree(wexpr));
}

/* --- nodes of reader-registered parametric cells ------------------------------------------- */

/*
 * multiply; shift and neg, whose output widths can overflow or go negative; deep, whose width
 * expression is too deep to evaluate without allocating; wide, with more ports than node.c plans
 * for inline.
 */
static void read_node_lib(void) {
    odin3_strbuf lib;
    odin3_strbuf_init(&lib);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&lib, "%s", k_multiply));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&lib, "cell shift hard ; param N int 4 ; param S int 2 ; in a N\n"
                                   "  out y N << S ; fn y = a ; end\n"
                                   "cell neg hard ; param N int 4 ; param S int 2 ; in a N\n"
                                   "  out y N - S ; fn y = a ; end\n"
                                   "cell deep hard ; param W int 2 ; in a W"));
    for (int i = 0; i < DEEP_TERMS; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&lib, " + 0"));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&lib, " ; out y 1 ; fn y = a[0] ; end\n"
                                                               "cell wide hard ; param W int 2\n"));
    for (int i = 0; i < WIDE_PORTS; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&lib, "  in i%d W + %d\n", i, i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(&lib, "  out y W * 2 ; fn y = i0 ; end\n"));
    read_ok(lib.data);
    odin3_strbuf_free(&lib);
}

static odin3_module *new_module(const char *name) {
    uint32_t str = 0;
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(g_design, odin3_bytes_cstr(name), &str));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(g_design, str, (odin3_prov_id){0}, &id));
    return odin3_module_get(g_design, id);
}

static odin3_status make_node(odin3_module *mod, const char *type, const odin3_value *params,
                              odin3_node_id *out) {
    const odin3_celltype_def *def = def_of(type);
    const odin3_node_spec spec = {type_of(type), 0, {0}, params, def->n_params};
    return odin3_node_create(mod, &spec, out);
}

static odin3_status check_full_module(odin3_module *mod) {
    return odin3_check_module(mod, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE});
}

static void test_node_create_parametric(void) {
    read_node_lib();
    odin3_module *mod = new_module("top");
    const odin3_value mul[2] = {odin3_value_int(3), odin3_value_int(5)};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, make_node(mod, "multiply", mul, &node));
    TEST_ASSERT_EQUAL_UINT32(16, odin3_node_pins(mod, node).count);
    TEST_ASSERT_EQUAL_UINT32(8, odin3_node_port(mod, node, 2).count);
    const odin3_value two[1] = {odin3_value_int(2)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, make_node(mod, "deep", two, &node));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_node_pins(mod, node).count);
    const odin3_value three[1] = {odin3_value_int(3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, make_node(mod, "wide", three, &node));
    TEST_ASSERT_EQUAL_UINT32(WIDE_PORTS * 3 + (WIDE_PORTS - 1) * WIDE_PORTS / 2 + 6,
                             odin3_node_pins(mod, node).count);
    TEST_ASSERT_EQUAL_UINT32(3 + WIDE_PORTS - 1, odin3_node_port(mod, node, WIDE_PORTS - 1).count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full_module(mod));
}

/* One rejected create: INVALID_ARG, the reason located at port and type, the module unchanged. */
static void expect_node_rejected(odin3_module *mod, const char *type, const odin3_value *params,
                                 const char *message) {
    uint32_t nodes = odin3_module_node_end(mod);
    uint32_t pins = odin3_module_pin_end(mod);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, make_node(mod, type, params, NULL));
    TEST_ASSERT_EQUAL_STRING(message, g_msg);
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(mod));
    TEST_ASSERT_EQUAL_UINT32(pins, odin3_module_pin_end(mod));
}

static void test_node_create_rejects_bad_widths(void) {
    read_node_lib();
    odin3_module *mod = new_module("top");
    const odin3_value overflow[2] = {odin3_value_int(4), odin3_value_int(62)};
    expect_node_rejected(mod, "shift", overflow,
                         "port_width: port 'y' of cell type 'shift': integer overflow");
    const odin3_value negative[2] = {odin3_value_int(2), odin3_value_int(5)};
    expect_node_rejected(mod, "neg", negative,
                         "port_width: port 'y' of cell type 'neg': width -3 is outside "
                         "0..4294967295");
    const odin3_value huge[2] = {odin3_value_int(UINT32_MAX), odin3_value_int(4)};
    expect_node_rejected(mod, "multiply", huge,
                         "port_width: port 'out' of cell type 'multiply': width 4294967299 is "
                         "outside 0..4294967295");
}

static void test_node_create_connected_parametric(void) {
    read_node_lib();
    odin3_module *mod = new_module("top");
    odin3_net_id nets[16];
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, (odin3_prov_id){0}, &nets[i]));
    }
    const odin3_value params[2] = {odin3_value_int(3), odin3_value_int(5)};
    const odin3_node_spec spec = {type_of("multiply"), 0, {0}, params, 2};
    odin3_netvec ports[3] = {{nets, 3}, {nets + 3, 5}, {nets + 8, 7}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_node_create_connected(mod, &spec, ports, NULL));
    TEST_ASSERT_EQUAL_STRING("node_create_connected: port out has width 8, got 7 nets", g_msg);
    ports[2].count = 8;
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    TEST_ASSERT_EQUAL_UINT32(16, odin3_node_pins(mod, node).count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full_module(mod));
}

/* Node creation under every allocation failure, a fresh module per try; returns the failures. */
static long sweep_node_create(const char *type, const odin3_value *params, uint32_t pin_count) {
    long tries = 0;
    for (;; tries++) {
        char name[NAME_MAX_LEN];
        (void)snprintf(name, sizeof name, "%s_%ld", type, tries);
        odin3_module *mod = new_module(name);
        odin3_node_id node = {0};
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = make_node(mod, type, params, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_UINT32(pin_count, odin3_node_pins(mod, node).count);
            TEST_ASSERT_EQUAL_UINT32(pin_count + 1, odin3_module_pin_end(mod));
            break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, g_msg);
        TEST_ASSERT_EQUAL_UINT32(1, odin3_module_node_end(mod));
        TEST_ASSERT_EQUAL_UINT32(1, odin3_module_pin_end(mod));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_full_module(mod));
    }
    return tries;
}

static void test_node_create_oom_sweep(void) {
    read_node_lib();
    const odin3_value two[1] = {odin3_value_int(2)};
    /* deep: its evaluation stacks spill to the heap; wide: its widths do not fit the plan */
    TEST_ASSERT_TRUE(sweep_node_create("deep", two, 3) >= 2);
    TEST_ASSERT_TRUE(sweep_node_create(
                         "wide", two, WIDE_PORTS * 2 + (WIDE_PORTS - 1) * WIDE_PORTS / 2 + 4) >= 1);
    const odin3_value mul[2] = {odin3_value_int(3), odin3_value_int(5)};
    (void)sweep_node_create("multiply", mul, 16); /* a fresh module has room: may never fail */
}

/* A width expression that runs out of memory inside check is NO_MEMORY, never a violation. */
static void test_check_oom_is_not_a_violation(void) {
    read_node_lib();
    odin3_module *mod = new_module("top");
    const odin3_value two[1] = {odin3_value_int(2)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, make_node(mod, "deep", two, NULL));
    long tries = 0;
    for (;; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = check_full_module(mod);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, g_msg);
    }
    TEST_ASSERT_TRUE(tries > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gate_cell);
    RUN_TEST(test_gate_cell_library_data);
    RUN_TEST(test_gate_cell_function);
    RUN_TEST(test_comments_blank_lines_and_separators);
    RUN_TEST(test_kinds_map_to_granularity);
    RUN_TEST(test_parametric_width);
    RUN_TEST(test_parametric_width_evaluates);
    RUN_TEST(test_parametric_width_rejects_bad_params);
    RUN_TEST(test_constant_width_expression_folds);
    RUN_TEST(test_width_check_names_identifier);
    RUN_TEST(test_three_cells_register_three_types);
    RUN_TEST(test_clock_and_signed_modifiers);
    RUN_TEST(test_seq_statements);
    RUN_TEST(test_memory_statements);
    RUN_TEST(test_memory_read_port_and_outputs);
    RUN_TEST(test_memory_ports_and_widths);
    RUN_TEST(test_memory_drives_remaining_outputs);
    RUN_TEST(test_fn_expressions_name_ports_and_params);
    RUN_TEST(test_failed_read_registers_nothing);
    RUN_TEST(test_cell_get_on_other_types);
    RUN_TEST(test_read_file);
    RUN_TEST(test_read_missing_file);
    RUN_TEST(test_malformed_libraries);
    RUN_TEST(test_nul_byte);
    RUN_TEST(test_second_read_rejects_existing_cells);
    RUN_TEST(test_collect_idents_order);
    RUN_TEST(test_read_oom_sweep);
    RUN_TEST(test_width_compile_oom_sweep);
    RUN_TEST(test_node_create_parametric);
    RUN_TEST(test_node_create_rejects_bad_widths);
    RUN_TEST(test_node_create_connected_parametric);
    RUN_TEST(test_node_create_oom_sweep);
    RUN_TEST(test_check_oom_is_not_a_violation);
    return UNITY_END();
}
