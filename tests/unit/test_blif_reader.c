/*
 * test_blif_reader.c — unit tests for the BLIF reader (pass 1: models, ports, black boxes).
 */
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif

enum { MSG_MAX = 512, NAME_MAX_LEN = 64, OOM_SWEEP = 20000 };

static const char *const PATH = "odin3_reader_test.blif";
static const char *const HAND = ODIN3_BLIF_FIXTURES "/hand_ports.blif";
static char last_error[MSG_MAX];
static odin3_design *design;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

void setUp(void) {
    last_error[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    (void)remove(PATH);
}

static void write_str(const char *data) {
    FILE *file = fopen(PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(data), fwrite(data, 1, strlen(data), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(design), id);
}

static uint32_t intern(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &id));
    return id;
}

static odin3_module *module_at(uint32_t id) {
    odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
    TEST_ASSERT_NOT_NULL(module);
    return module;
}

static void read_ok(const char *path) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    TEST_ASSERT_EQUAL_STRING("", last_error);
}

/* Reads PATH holding text and expects a parse error logged at `line`. */
static void expect_parse_error(const char *text, uint32_t line) {
    write_str(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(design, PATH));
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want, "%s:%u: ", PATH, (unsigned)line);
    TEST_ASSERT_EQUAL_STRING_LEN(want, last_error, strlen(want));
}

typedef struct port_want {
    const char *name;
    odin3_dir dir;
    uint32_t width;
    bool scalar;
} port_want;

/* Port `index` of module: cell-type port, wire name and per-bit net names. */
static void expect_port(odin3_module *module, uint32_t index, port_want want) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_module_celltype(module));
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_TRUE(index < def->n_ports);
    const odin3_port_def *port = &def->ports[index];
    TEST_ASSERT_EQUAL_STRING(want.name, port->name);
    TEST_ASSERT_EQUAL_INT(want.dir, port->dir);
    TEST_ASSERT_EQUAL_UINT32(want.width, port->width);
    TEST_ASSERT_EQUAL(want.scalar, port->scalar);
    odin3_wire_id wire = odin3_module_port_wire(module, index);
    TEST_ASSERT_EQUAL_STRING(want.name, str_of(odin3_wire_name(module, wire)));
    TEST_ASSERT_EQUAL_UINT32(want.width, odin3_wire_width(module, wire));
    for (uint32_t k = 0; k < want.width; k++) {
        char bit[NAME_MAX_LEN];
        if (want.scalar) {
            (void)snprintf(bit, sizeof bit, "%s", want.name);
        } else {
            (void)snprintf(bit, sizeof bit, "%s[%u]", want.name, (unsigned)k);
        }
        odin3_net_id net = odin3_wire_net(module, wire, k);
        TEST_ASSERT_EQUAL_STRING(bit, str_of(odin3_net_name(module, net)));
        TEST_ASSERT_EQUAL_UINT32(net.v, odin3_module_find_net(module, intern(bit)).v);
    }
}

static const odin3_prov_record *wire_record(odin3_module *module, uint32_t port) {
    odin3_wire_id wire = odin3_module_port_wire(module, port);
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_wire_prov(module, wire));
    TEST_ASSERT_NOT_NULL(rec);
    return rec;
}

static void expect_loc(const odin3_prov_record *rec, uint32_t line, uint32_t col,
                       uint32_t end_col) {
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->n_locs);
    TEST_ASSERT_EQUAL_STRING(HAND, str_of(rec->locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(line, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(col, rec->locs[0].col);
    TEST_ASSERT_EQUAL_UINT32(line, rec->locs[0].end_line);
    TEST_ASSERT_EQUAL_UINT32(end_col, rec->locs[0].end_col);
    TEST_ASSERT_EQUAL_STRING("read_blif", str_of(odin3_passrun_name(design, rec->run)));
}

/* --- pass 1 on the hand-written fixture -------------------------------------------------- */

static void test_modules_in_file_order_first_is_top(void) {
    read_ok(HAND);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_design_module_end(design)); /* top, sub; bb is no module */
    TEST_ASSERT_EQUAL_STRING("top", str_of(odin3_module_name(module_at(1))));
    TEST_ASSERT_EQUAL_STRING("sub", str_of(odin3_module_name(module_at(2))));
    expect_loc(odin3_prov_get(design, odin3_module_prov(module_at(1))), 2, 2, 2);
}

/* Review Focus 2: a[0] b a[1] stay three scalar ports, in order. */
static void test_nonconsecutive_bits_stay_scalar_in_order(void) {
    read_ok(HAND);
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(11, odin3_module_port_count(top));
    expect_port(top, 0, (port_want){"a[0]", ODIN3_DIR_IN, 1, true});
    expect_port(top, 1, (port_want){"b", ODIN3_DIR_IN, 1, true});
    expect_port(top, 2, (port_want){"a[1]", ODIN3_DIR_IN, 1, true});
    /* d[1] d[2] do not start at 0; e[1] e[0] are out of order */
    expect_port(top, 7, (port_want){"d[1]", ODIN3_DIR_OUT, 1, true});
    expect_port(top, 8, (port_want){"d[2]", ODIN3_DIR_OUT, 1, true});
    expect_port(top, 9, (port_want){"e[1]", ODIN3_DIR_OUT, 1, true});
    expect_port(top, 10, (port_want){"e[0]", ODIN3_DIR_OUT, 1, true});
}

static void test_consecutive_bits_group_into_vectors(void) {
    read_ok(HAND);
    odin3_module *top = module_at(1);
    expect_port(top, 3, (port_want){"v", ODIN3_DIR_IN, 3, false});
    expect_port(top, 4, (port_want){"clk", ODIN3_DIR_IN, 1, true});
    expect_port(top, 5, (port_want){"y", ODIN3_DIR_OUT, 2, false});
    expect_port(top, 6, (port_want){"z", ODIN3_DIR_OUT, 1, true});
    odin3_module *sub = module_at(2); /* p[0] \ p[1]: one logical line, so one directive */
    TEST_ASSERT_EQUAL_UINT32(3, odin3_module_port_count(sub));
    expect_port(sub, 0, (port_want){"p", ODIN3_DIR_IN, 2, false});
    expect_port(sub, 1, (port_want){"q", ODIN3_DIR_IN, 1, true});
    expect_port(sub, 2, (port_want){"r", ODIN3_DIR_OUT, 1, true});
}

static void test_bits_in_separate_directives_stay_scalar(void) {
    write_str(".model m\n.inputs a[0]\n.inputs a[1] b[0]x\n.outputs c[01] c[1]\n.end\n");
    read_ok(PATH);
    odin3_module *mod = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(5, odin3_module_port_count(mod));
    expect_port(mod, 0, (port_want){"a[0]", ODIN3_DIR_IN, 1, true}); /* a[1] is elsewhere */
    expect_port(mod, 1, (port_want){"a[1]", ODIN3_DIR_IN, 1, true});
    expect_port(mod, 2, (port_want){"b[0]x", ODIN3_DIR_IN, 1, true});
    expect_port(mod, 3, (port_want){"c[01]", ODIN3_DIR_OUT, 1, true});
    expect_port(mod, 4, (port_want){"c[1]", ODIN3_DIR_OUT, 1, true});
}

static void test_port_provenance_points_at_its_token(void) {
    read_ok(HAND);
    odin3_module *top = module_at(1);
    expect_loc(wire_record(top, 1), 3, 3, 3);          /* b: token 2 of line 3 */
    expect_loc(wire_record(top, 3), 3, 5, 7);          /* v[0] v[1] v[2]: tokens 4..6 */
    expect_loc(wire_record(top, 5), 4, 2, 3);          /* y[0] y[1] */
    expect_loc(wire_record(module_at(2), 0), 9, 2, 3); /* p[0] \ p[1]: logical line 9 */
    odin3_wire_id wire = odin3_module_port_wire(top, 3);
    odin3_prov_id prov = odin3_wire_prov(top, wire);
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_net_prov(top, odin3_wire_net(top, wire, 2)).v);
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(top, odin3_module_port(top, 3)).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(prov.v, odin3_wire_prov(top, odin3_module_port_wire(top, 1)).v);
}

static void test_clock_names_are_a_module_attribute(void) {
    read_ok(HAND);
    odin3_module *top = module_at(1);
    const odin3_value *clock =
        odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_MODULE, 1}, intern(ODIN3_BLIF_ATTR_CLOCK));
    TEST_ASSERT_NOT_NULL(clock);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, clock->kind);
    TEST_ASSERT_EQUAL_STRING("clk", str_of(clock->str));
    TEST_ASSERT_NULL(odin3_attr_get(module_at(2), (odin3_objref){ODIN3_OBJ_MODULE, 2},
                                    intern(ODIN3_BLIF_ATTR_CLOCK)));
}

static void test_clock_lists_join_in_order(void) {
    write_str(".model m\n.inputs c1 c2\n.clock c1\n.clock c2 c3\n.end\n");
    read_ok(PATH);
    const odin3_value *clock = odin3_attr_get(module_at(1), (odin3_objref){ODIN3_OBJ_MODULE, 1},
                                              intern(ODIN3_BLIF_ATTR_CLOCK));
    TEST_ASSERT_NOT_NULL(clock);
    TEST_ASSERT_EQUAL_STRING("c1 c2 c3", str_of(clock->str));
}

static const odin3_celltype_def *declared_bb(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    odin3_celltype_id bb = odin3_design_declared_model(design, 0);
    odin3_celltype_id found = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("bb"), &found));
    TEST_ASSERT_EQUAL_UINT32(found.v, bb.v);
    const odin3_celltype_def *def = odin3_celltype_get(design, bb);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    TEST_ASSERT_EQUAL_UINT32(3, def->n_ports);
    return def;
}

static void test_blackbox_declared_with_grouped_formals(void) {
    read_ok(HAND);
    const odin3_celltype_def *def = declared_bb();
    TEST_ASSERT_EQUAL_STRING("x", def->ports[0].name);
    TEST_ASSERT_EQUAL_UINT32(3, def->ports[0].width);
    TEST_ASSERT_FALSE(def->ports[0].scalar);
    TEST_ASSERT_EQUAL_STRING("s", def->ports[1].name);
    TEST_ASSERT_TRUE(def->ports[1].scalar);
    /* o[0] is the only bit of o in its list: a width-1 vector */
    TEST_ASSERT_EQUAL_STRING("o", def->ports[2].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[2].dir);
    TEST_ASSERT_EQUAL_UINT32(1, def->ports[2].width);
    TEST_ASSERT_FALSE(def->ports[2].scalar);
}

/* A process-global "hard" type the black-box declaration below must reuse (IR-7b). */
static const odin3_port_def HARD_PORTS[] = {
    {.name = "a", .dir = ODIN3_DIR_IN, .scalar = false, .width = 2},
    {.name = "cin", .dir = ODIN3_DIR_IN, .scalar = true, .width = 1},
    {.name = "s", .dir = ODIN3_DIR_OUT, .scalar = true, .width = 1},
};
static const odin3_celltype_def HARD_DEF = {
    .name = "o3test_hard_adder", .gran = ODIN3_GRAN_HARD, .ports = HARD_PORTS, .n_ports = 3};

static void register_hard_once(void) {
    static bool registered;
    if (!registered) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&HARD_DEF));
        registered = true;
    }
    odin3_design_destroy(design); /* designs see only definitions registered before them */
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

static void test_blackbox_reuses_compatible_registered_type(void) {
    register_hard_once();
    odin3_celltype_id hard = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("o3test_hard_adder"), &hard));
    write_str(".model top\n.end\n.model o3test_hard_adder\n.inputs a[0] a[1] cin\n"
              ".outputs s\n.blackbox\n.end\n");
    read_ok(PATH);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    TEST_ASSERT_EQUAL_UINT32(hard.v, odin3_design_declared_model(design, 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, odin3_celltype_get(design, hard)->gran);
}

static void test_blackbox_incompatible_with_registered_type(void) {
    register_hard_once();
    expect_parse_error(".model top\n.end\n.model o3test_hard_adder\n.inputs a[0] cin\n"
                       ".outputs s\n.blackbox\n.end\n",
                       3);
}

/* A net that is both a primary input and a primary output is one net shared by two ports. */
static void test_input_and_output_of_the_same_name_share_a_net(void) {
    write_str(".model m\n.inputs x\n.outputs x y\n.end\n");
    read_ok(PATH);
    odin3_module *mod = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_module_port_count(mod));
    expect_port(mod, 0, (port_want){"x", ODIN3_DIR_IN, 1, true});
    odin3_wire_id out = odin3_module_port_wire(mod, 1);
    TEST_ASSERT_EQUAL_STRING("x$blif_port", str_of(odin3_wire_name(mod, out)));
    const odin3_value *name = odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_WIRE, out.v},
                                             intern(ODIN3_BLIF_ATTR_PORT_NAME));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("x", str_of(name->str));
    odin3_net_id net = odin3_wire_net(mod, odin3_module_port_wire(mod, 0), 0);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_wire_net(mod, out, 0).v);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_module_find_net(mod, intern("x")).v);
    expect_port(mod, 2, (port_want){"y", ODIN3_DIR_OUT, 1, true});
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

static void test_mangled_name_skips_taken_names(void) {
    write_str(".model m\n.inputs v[0] v[1] v$blif_port\n.outputs v[0] v[1]\n.end\n");
    read_ok(PATH);
    odin3_module *mod = module_at(1);
    odin3_wire_id out = odin3_module_port_wire(mod, 2);
    TEST_ASSERT_EQUAL_STRING("v$blif_port2", str_of(odin3_wire_name(mod, out)));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_wire_width(mod, out));
    odin3_wire_id in = odin3_module_port_wire(mod, 0);
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(mod, in, 1).v, odin3_wire_net(mod, out, 1).v);
}

/* --- structural errors --------------------------------------------------------------------- */

static void test_duplicate_model(void) {
    expect_parse_error(".model a\n.end\n\n.model b\n.end\n.model a\n.end\n", 6);
}

static void test_duplicate_black_box_model(void) {
    expect_parse_error(".model a\n.end\n.model a\n.blackbox\n.end\n", 3);
}

static void test_missing_end_before_next_model(void) {
    expect_parse_error(".model a\n.inputs x\n.model b\n.end\n", 1);
}

static void test_missing_end_at_eof(void) {
    expect_parse_error(".model a\n.end\n# c\n.model b\n.inputs x\n", 4);
}

static void test_port_declared_twice(void) {
    expect_parse_error(".model a\n.inputs x y\n.outputs z\n.inputs w \\\n x\n.end\n", 4);
}

static void test_output_declared_twice_on_one_line(void) {
    expect_parse_error(".model a\n.outputs q[0] q[0]\n.end\n", 2);
}

static void test_black_box_port_both_input_and_output(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs x\n.outputs x\n.blackbox\n.end\n", 5);
}

static void test_directive_outside_a_model(void) {
    expect_parse_error("# header\n.inputs x\n.model a\n.end\n", 2);
}

static void test_end_outside_a_model(void) {
    expect_parse_error(".model a\n.end\n.end\n", 3);
}

static void test_end_and_blackbox_take_no_arguments(void) {
    expect_parse_error(".model a\n.end a\n", 2);
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model a\n.blackbox yes\n.end\n", 2);
}

static void test_clock_in_a_black_box(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs c\n.clock c\n.blackbox\n.end\n", 5);
}

static void test_model_needs_one_name(void) {
    expect_parse_error(".model\n.end\n", 1);
}

static void test_model_named_like_a_builtin_cell(void) {
    expect_parse_error(".model top\n.end\n.model $sop\n.end\n", 3);
}

static void test_black_box_with_a_body(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs a\n.outputs y\n.blackbox\n"
                       ".names a y\n1 1\n.end\n",
                       7);
}

static void test_unknown_directive(void) {
    expect_parse_error(".model top\n.inputs a\n.outputs y\n.gate and2 A=a Y=y\n.end\n", 4);
}

static void test_missing_file_is_io_error(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_blif_read(design, "no/such/file.blif"));
}

static void test_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_read(NULL, HAND));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_read(design, NULL));
    last_error[0] = '\0';
    read_ok(HAND);
    last_error[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_read(design, HAND)); /* not fresh */
}

/* --- copied goldens ------------------------------------------------------------------------ */

static void test_golden_ff(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/ff.odin.blif");
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_STRING("dff", str_of(odin3_module_name(top)));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(top));
    expect_port(top, 0, (port_want){"dff^clk", ODIN3_DIR_IN, 1, true});
    expect_port(top, 3, (port_want){"dff^q", ODIN3_DIR_OUT, 1, true});
}

static void test_golden_ff_parmys(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/ff.parmys.blif");
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(module_at(1)));
}

static void test_golden_adder_hard_block(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/adder_hard_block.parmys.blif");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_EQUAL_UINT32(8, odin3_module_port_count(module_at(1)));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    const odin3_celltype_def *adder =
        odin3_celltype_get(design, odin3_design_declared_model(design, 0));
    TEST_ASSERT_EQUAL_STRING("adder", adder->name);
    TEST_ASSERT_EQUAL_UINT32(5, adder->n_ports);
    TEST_ASSERT_EQUAL_STRING("cin", adder->ports[1].name);
}

static void test_golden_multi_model_with_black_boxes(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/ansiportlist_2.parmys.blif");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(design));
    TEST_ASSERT_EQUAL_UINT32(8 + 60, odin3_module_port_count(module_at(1)));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_declared_model_count(design));
    const odin3_celltype_def *mul =
        odin3_celltype_get(design, odin3_design_declared_model(design, 1));
    TEST_ASSERT_EQUAL_STRING("multiply", mul->name);
    TEST_ASSERT_EQUAL_UINT32(3, mul->n_ports);
    TEST_ASSERT_EQUAL_STRING("b", mul->ports[0].name);
    TEST_ASSERT_EQUAL_UINT32(36, mul->ports[0].width);
    TEST_ASSERT_EQUAL_STRING("out", mul->ports[2].name);
    TEST_ASSERT_EQUAL_UINT32(72, mul->ports[2].width);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Every allocation failure gives NO_MEMORY (or IO when the lexer itself cannot be opened). */
static void test_out_of_memory_sweep(void) {
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        odin3_design_destroy(design);
        design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_blif_read(design, HAND);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            done = true;
        } else if (st != ODIN3_ERR_NO_MEMORY) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
        }
    }
    TEST_ASSERT_TRUE(done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_modules_in_file_order_first_is_top);
    RUN_TEST(test_nonconsecutive_bits_stay_scalar_in_order);
    RUN_TEST(test_consecutive_bits_group_into_vectors);
    RUN_TEST(test_bits_in_separate_directives_stay_scalar);
    RUN_TEST(test_port_provenance_points_at_its_token);
    RUN_TEST(test_clock_names_are_a_module_attribute);
    RUN_TEST(test_clock_lists_join_in_order);
    RUN_TEST(test_blackbox_declared_with_grouped_formals);
    RUN_TEST(test_blackbox_reuses_compatible_registered_type);
    RUN_TEST(test_blackbox_incompatible_with_registered_type);
    RUN_TEST(test_input_and_output_of_the_same_name_share_a_net);
    RUN_TEST(test_mangled_name_skips_taken_names);
    RUN_TEST(test_duplicate_model);
    RUN_TEST(test_duplicate_black_box_model);
    RUN_TEST(test_missing_end_before_next_model);
    RUN_TEST(test_missing_end_at_eof);
    RUN_TEST(test_port_declared_twice);
    RUN_TEST(test_output_declared_twice_on_one_line);
    RUN_TEST(test_black_box_port_both_input_and_output);
    RUN_TEST(test_directive_outside_a_model);
    RUN_TEST(test_end_outside_a_model);
    RUN_TEST(test_end_and_blackbox_take_no_arguments);
    RUN_TEST(test_clock_in_a_black_box);
    RUN_TEST(test_model_needs_one_name);
    RUN_TEST(test_model_named_like_a_builtin_cell);
    RUN_TEST(test_black_box_with_a_body);
    RUN_TEST(test_unknown_directive);
    RUN_TEST(test_missing_file_is_io_error);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_golden_ff);
    RUN_TEST(test_golden_ff_parmys);
    RUN_TEST(test_golden_adder_hard_block);
    RUN_TEST(test_golden_multi_model_with_black_boxes);
    RUN_TEST(test_out_of_memory_sweep);
    return UNITY_END();
}
