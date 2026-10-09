/*
 * test_blif_reader.c — unit tests for the BLIF reader (models, ports, black boxes, bodies).
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
static const char *const BODY = ODIN3_BLIF_FIXTURES "/hand_body.blif";
static char last_error[MSG_MAX];
static uint32_t error_count; /* error lines logged since setUp */
static odin3_design *design;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
        error_count++;
    }
}

void setUp(void) {
    last_error[0] = '\0';
    error_count = 0;
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_blif_test_set_between_passes(NULL, NULL);
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

/* Reads PATH holding text and expects a parse error logged at `line` that contains `what`. */
static void expect_parse_error(const char *text, uint32_t line, const char *what) {
    write_str(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(design, PATH));
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want, "%s:%u: ", PATH, (unsigned)line);
    TEST_ASSERT_EQUAL_STRING_LEN(want, last_error, strlen(want));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, what), last_error);
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

typedef struct loc_want {
    const char *file;
    uint32_t line, col, end_col;
} loc_want;

static void expect_file_loc(const odin3_prov_record *rec, loc_want want) {
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->n_locs);
    TEST_ASSERT_EQUAL_STRING(want.file, str_of(rec->locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(want.line, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(want.col, rec->locs[0].col);
    TEST_ASSERT_EQUAL_UINT32(want.line, rec->locs[0].end_line);
    TEST_ASSERT_EQUAL_UINT32(want.end_col, rec->locs[0].end_col);
    TEST_ASSERT_EQUAL_STRING("read_blif", str_of(odin3_passrun_name(design, rec->run)));
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
                       3, "conflicts with the registered cell type");
    TEST_ASSERT_EQUAL_UINT32(1, error_count); /* one located line, no unlocated IR log */
}

static void test_blackbox_repeats_a_port_name(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs a[0] a[1] a\n.outputs y\n.blackbox\n"
                       ".end\n",
                       3, "repeats port name 'a'");
    TEST_ASSERT_EQUAL_UINT32(1, error_count);
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
    expect_parse_error(".model a\n.end\n\n.model b\n.end\n.model a\n.end\n", 6,
                       "duplicate model 'a'");
}

static void test_duplicate_black_box_model(void) {
    expect_parse_error(".model a\n.end\n.model a\n.blackbox\n.end\n", 3, "duplicate model");
}

static void test_missing_end_before_next_model(void) {
    expect_parse_error(".model a\n.inputs x\n.model b\n.end\n", 1, "has no .end");
}

static void test_missing_end_at_eof(void) {
    expect_parse_error(".model a\n.end\n# c\n.model b\n.inputs x\n", 4, "has no .end");
}

static void test_port_declared_twice(void) {
    expect_parse_error(".model a\n.inputs x y\n.outputs z\n.inputs w \\\n x\n.end\n", 4,
                       "declared twice");
}

static void test_output_declared_twice_on_one_line(void) {
    expect_parse_error(".model a\n.outputs q[0] q[0]\n.end\n", 2, "declared twice");
}

static void test_black_box_port_both_input_and_output(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs x\n.outputs x\n.blackbox\n.end\n", 5,
                       "both an input and an output");
}

static void test_directive_outside_a_model(void) {
    expect_parse_error("# header\n.inputs x\n.model a\n.end\n", 2, "outside a .model");
}

static void test_end_outside_a_model(void) {
    expect_parse_error(".model a\n.end\n.end\n", 3, "outside a .model");
}

static void test_end_and_blackbox_take_no_arguments(void) {
    expect_parse_error(".model a\n.end a\n", 2, "takes no arguments");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model a\n.blackbox yes\n.end\n", 2, "takes no arguments");
}

static void test_clock_in_a_black_box(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs c\n.clock c\n.blackbox\n.end\n", 5,
                       ".clock in a black box");
}

static void test_model_needs_one_name(void) {
    expect_parse_error(".model\n.end\n", 1, "exactly one name");
}

static void test_model_named_like_a_builtin_cell(void) {
    expect_parse_error(".model top\n.end\n.model $sop\n.end\n", 3, "name of a cell type");
}

static void test_black_box_with_a_body(void) {
    expect_parse_error(".model top\n.end\n.model bb\n.inputs a\n.outputs y\n.blackbox\n"
                       ".names a y\n1 1\n.end\n",
                       7, "has a body");
}

static void test_unknown_directive(void) {
    expect_parse_error(".model top\n.inputs a\n.outputs y\n.gate and2 A=a Y=y\n.end\n", 4,
                       "unknown directive '.gate'");
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

/* --- pass 2: bodies ------------------------------------------------------------------------ */

static void expect_check_clean(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Cell `index` of module (0 = the first non-port node, in ID order = file order). */
static odin3_node_id cell_at(odin3_module *module, uint32_t index) {
    odin3_node_id node = {odin3_module_port_count(module) + 1 + index};
    TEST_ASSERT_TRUE(node.v < odin3_module_node_end(module));
    TEST_ASSERT_TRUE(odin3_node_live(module, node));
    return node;
}

static const char *type_of(odin3_module *module, odin3_node_id node) {
    return odin3_celltype_get(design, odin3_node_type(module, node))->name;
}

/* A pin of a node: port index and bit. */
typedef struct pin_at {
    uint32_t port, bit;
} pin_at;

/* Name of the net on a pin of node, "" when unconnected. */
static const char *pin_net_at(odin3_module *module, odin3_node_id node, pin_at at) {
    odin3_pinslice pins = odin3_node_port(module, node, at.port);
    TEST_ASSERT_TRUE(at.bit < pins.count);
    odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){pins.first.v + at.bit});
    return odin3_net_valid(net) ? str_of(odin3_net_name(module, net)) : "";
}

static void expect_cover(odin3_module *module, odin3_node_id node, uint32_t width,
                         const char *rows) {
    TEST_ASSERT_EQUAL_STRING("$sop", type_of(module, node));
    TEST_ASSERT_EQUAL_INT64(width, odin3_node_param(module, node, 0)->i);
    const odin3_value *cover = odin3_node_param(module, node, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_COVER, cover->kind);
    TEST_ASSERT_EQUAL_UINT32(width, cover->cover_inputs);
    TEST_ASSERT_EQUAL_UINT32(strlen(rows), cover->len);
    if (cover->len > 0) {
        TEST_ASSERT_EQUAL_MEMORY(rows, cover->bits, cover->len);
    }
}

static void test_names_become_sop_with_rows_as_written(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    odin3_node_id and2 = cell_at(top, 0);
    expect_cover(top, and2, 2, "111");
    TEST_ASSERT_EQUAL_STRING("a", pin_net_at(top, and2, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("b", pin_net_at(top, and2, (pin_at){0, 1}));
    TEST_ASSERT_EQUAL_STRING("n1", pin_net_at(top, and2, (pin_at){1, 0}));
    odin3_node_id inv = cell_at(top, 1);
    expect_cover(top, inv, 1, "01");
    TEST_ASSERT_EQUAL_STRING("y", pin_net_at(top, inv, (pin_at){1, 0})); /* the output port's net */
    expect_cover(top, cell_at(top, 2), 0, "");                           /* .names k0: constant 0 */
    expect_cover(top, cell_at(top, 3), 0, "1"); /* .names k1 / 1: constant 1 */
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1,
                          odin3_net_const_value(top, odin3_module_find_net(top, intern("k1"))));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0,
                          odin3_net_const_value(top, odin3_module_find_net(top, intern("k0"))));
    odin3_module *sub = module_at(2);
    expect_cover(sub, cell_at(sub, 0), 2, "1-1-11");
}

typedef struct latch_want {
    const char *type;
    int64_t init;
    const char *ctrl, *in, *out;
} latch_want;

static void expect_latch(odin3_module *module, uint32_t index, latch_want want) {
    odin3_node_id node = cell_at(module, index);
    TEST_ASSERT_EQUAL_STRING(want.type, type_of(module, node));
    TEST_ASSERT_EQUAL_INT64(want.init, odin3_node_param(module, node, 0)->i);
    uint32_t data = want.ctrl != NULL ? 1 : 0; /* C/E comes first when the latch has one */
    if (want.ctrl != NULL) {
        TEST_ASSERT_EQUAL_STRING(want.ctrl, pin_net_at(module, node, (pin_at){0, 0}));
    }
    TEST_ASSERT_EQUAL_STRING(want.in, pin_net_at(module, node, (pin_at){data, 0}));
    TEST_ASSERT_EQUAL_STRING(want.out, pin_net_at(module, node, (pin_at){data + 1, 0}));
}

static void test_latch_forms(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    expect_latch(top, 4, (latch_want){"$_DFF_P_", 2, "clk", "n1", "q[0]"});
    expect_latch(top, 5, (latch_want){"$_DFF_N_", 3, "clk", "y", "q[1]"}); /* default init 3 */
    expect_latch(top, 6, (latch_want){"$_DLATCH_P_", 0, "clk", "a", "l"});
    expect_latch(top, 7, (latch_want){"$_DLATCH_N_", 3, "clk", "b", "m1"});
    expect_latch(top, 8, (latch_want){"$_FF_", 3, NULL, "m1", "m2"});
    expect_latch(top, 9, (latch_want){"$_FF_", 1, NULL, "m2", "m3"});
}

/* Review Focus 1: .subckt names `sub`, a model defined later in the file. */
static void test_subckt_of_a_later_model_by_bit_formals(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    odin3_node_id inst = cell_at(top, 10);
    TEST_ASSERT_EQUAL_UINT32(odin3_module_celltype(module_at(2)).v, odin3_node_type(top, inst).v);
    TEST_ASSERT_EQUAL_STRING("b", pin_net_at(top, inst, (pin_at){0, 0})); /* i[0]=b */
    TEST_ASSERT_EQUAL_STRING("a", pin_net_at(top, inst, (pin_at){0, 1})); /* i[1]=a */
    TEST_ASSERT_EQUAL_STRING("s1", pin_net_at(top, inst, (pin_at){1, 0}));
    TEST_ASSERT_EQUAL_STRING("u_sub", str_of(odin3_node_name(top, inst)));
    TEST_ASSERT_EQUAL_UINT32(inst.v, odin3_module_find_node(top, intern("u_sub")).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, odin3_node_type(top, inst)));
}

static void test_subckt_of_black_box_leaves_unlisted_formals_open(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    odin3_node_id inst = cell_at(top, 11);
    TEST_ASSERT_EQUAL_STRING("bb", type_of(top, inst));
    TEST_ASSERT_EQUAL_STRING("n1", pin_net_at(top, inst, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("", pin_net_at(top, inst, (pin_at){1, 0})); /* z unlisted */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_name(top, inst));
    TEST_ASSERT_EQUAL_UINT32(12 + 6 + 1, odin3_module_node_end(top));
}

static const char *node_attr(odin3_module *module, odin3_node_id node, const char *key) {
    const odin3_value *val =
        odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern(key));
    if (val == NULL) {
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, val->kind);
    return str_of(val->str);
}

static void test_attr_value_blank_runs_collapse(void) {
    write_str(".model top\n.names a\n.attr src \"a   b\"\t c\n.end\n");
    read_ok(PATH);
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_STRING("\"a b\" c",
                             node_attr(top, cell_at(top, 0), ODIN3_BLIF_ATTR_PREFIX "src"));
}

static void test_attr_and_param_on_the_previous_cell(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    odin3_node_id inst = cell_at(top, 10);
    TEST_ASSERT_EQUAL_STRING("\"top.v:4\"", node_attr(top, inst, ODIN3_BLIF_ATTR_PREFIX "src"));
    TEST_ASSERT_EQUAL_STRING("01 01", node_attr(top, inst, ODIN3_BLIF_PARAM_PREFIX "P"));
    TEST_ASSERT_EQUAL_STRING(ODIN3_BLIF_ATTR_PREFIX "src " ODIN3_BLIF_PARAM_PREFIX "P",
                             node_attr(top, inst, ODIN3_BLIF_ATTR_EXTRAS));
    TEST_ASSERT_NULL(node_attr(top, cell_at(top, 11), ODIN3_BLIF_ATTR_EXTRAS));
}

static void test_body_provenance(void) {
    read_ok(BODY);
    odin3_module *top = module_at(1);
    const odin3_prov_record *and2 = odin3_prov_get(design, odin3_node_prov(top, cell_at(top, 0)));
    expect_file_loc(and2, (loc_want){BODY, 5, 1, 4});
    odin3_net_id n1 = odin3_module_find_net(top, intern("n1"));
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, n1)), (loc_want){BODY, 5, 4, 4});
    odin3_net_id s1 = odin3_module_find_net(top, intern("s1")); /* o=s1: token 5 of line 18 */
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, s1)), (loc_want){BODY, 18, 5, 5});
    const odin3_prov_record *dff = odin3_prov_get(design, odin3_node_prov(top, cell_at(top, 4)));
    expect_file_loc(dff, (loc_want){BODY, 12, 1, 6});
    TEST_ASSERT_NOT_EQUAL_UINT32(odin3_node_prov(top, cell_at(top, 0)).v,
                                 odin3_net_prov(top, n1).v);
    /* nets first named on the directive right after a .names get their own line */
    odin3_net_id k0 = odin3_module_find_net(top, intern("k0"));
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, k0)), (loc_want){BODY, 9, 2, 2});
    odin3_net_id k1 = odin3_module_find_net(top, intern("k1"));
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, k1)), (loc_want){BODY, 10, 2, 2});
    odin3_net_id m1 = odin3_module_find_net(top, intern("m1")); /* line 15, after a .latch */
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, m1)), (loc_want){BODY, 15, 3, 3});
    /* port nets were made in pass 1 and keep their records */
    odin3_net_id net_a = odin3_module_find_net(top, intern("a"));
    expect_file_loc(odin3_prov_get(design, odin3_net_prov(top, net_a)), (loc_want){BODY, 3, 2, 2});
}

static void test_check_clean_after_every_fixture(void) {
    static const char *const files[] = {
        ODIN3_BLIF_FIXTURES "/hand_ports.blif",
        ODIN3_BLIF_FIXTURES "/hand_body.blif",
        ODIN3_BLIF_FIXTURES "/ff.odin.blif",
        ODIN3_BLIF_FIXTURES "/ff.parmys.blif",
        ODIN3_BLIF_FIXTURES "/adder_hard_block.parmys.blif",
        ODIN3_BLIF_FIXTURES "/ansiportlist_2.parmys.blif",
        ODIN3_BLIF_FIXTURES "/pow.parmys.blif",
        ODIN3_BLIF_FIXTURES "/dffsre.parmys.blif",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        odin3_design_destroy(design);
        design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        read_ok(files[i]);
        expect_check_clean();
    }
}

/* A scalar port named like a bit (`a[1]` of `a[0] b a[1]`) is matched by its exact name. */
static void test_subckt_formal_matches_scalar_bit_named_port(void) {
    write_str(".model top\n.inputs x\n.subckt m a[1]=x b=x\n.end\n"
              ".model m\n.inputs a[0] b a[1]\n.end\n");
    read_ok(PATH);
    odin3_module *top = module_at(1);
    odin3_node_id inst = cell_at(top, 0);
    TEST_ASSERT_EQUAL_STRING("", pin_net_at(top, inst, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("x", pin_net_at(top, inst, (pin_at){1, 0}));
    TEST_ASSERT_EQUAL_STRING("x", pin_net_at(top, inst, (pin_at){2, 0}));
    expect_check_clean();
}

/* Multiple drivers are read as written; check reports them (rule 4). */
static void test_multiple_drivers_are_read(void) {
    write_str(".model top\n.inputs a b\n.outputs y\n.names a y\n1 1\n.names b y\n1 1\n.end\n");
    read_ok(PATH);
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(2,
                             odin3_net_driver_count(top, odin3_module_find_net(top, intern("y"))));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_CHECK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* --- pass 2 errors ------------------------------------------------------------------------- */

static void test_cover_row_width_mismatch(void) {
    expect_parse_error(".model top\n.inputs a b\n.names a b y\n11 1\n1 1\n.end\n", 5,
                       "cover row does not fit");
}

static void test_cover_row_bad_characters(void) {
    expect_parse_error(".model top\n.inputs a\n.names a y\n1 x\n.end\n", 4,
                       "cover row does not fit");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.inputs a\n.names a y\n2 1\n.end\n", 4,
                       "cover row does not fit");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.names y\n1 1\n.end\n", 3, "cover row does not fit");
}

/* BLIF forbids mixing on-set (output 1) and off-set (output 0) rows in one cover. */
static void test_cover_mixing_on_and_off_set(void) {
    expect_parse_error(".model top\n.inputs a b\n.names a b y\n11 1\n00 0\n.end\n", 5,
                       "mixes on-set and off-set");
}

static void test_cover_row_outside_names(void) {
    expect_parse_error(".model top\n.inputs a\n.latch a q\n1 1\n.end\n", 4, "outside .names");
}

static void test_names_needs_an_output(void) {
    expect_parse_error(".model top\n.names\n.end\n", 2, ".names needs an output");
}

static void test_latch_async_type_rejected(void) {
    expect_parse_error(".model top\n.inputs d c\n.latch d q as c 0\n.end\n", 3, "'as'");
}

static void test_latch_unknown_type(void) {
    expect_parse_error(".model top\n.inputs d c\n.latch d q rise c 0\n.end\n", 3,
                       "unknown latch type 'rise'");
}

static void test_latch_bad_init(void) {
    expect_parse_error(".model top\n.inputs d c\n.latch d q re c 4\n.end\n", 3, "init");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.inputs d\n.latch d q 01\n.end\n", 3, "init");
}

static void test_latch_token_count(void) {
    expect_parse_error(".model top\n.inputs d\n.latch d\n.end\n", 3, ".latch takes");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.inputs d c\n.latch d q re c 1 x\n.end\n", 3, ".latch takes");
}

/* A Yosys-like parametric type: A A_WIDTH, B B_WIDTH, Y Y_WIDTH (all default 1). */
static const odin3_param_def POW_PARAMS[] = {
    {"A_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}},
    {"B_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}},
    {"Y_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}}};
static const odin3_port_def POW_PORTS[] = {
    {.name = "A", .dir = ODIN3_DIR_IN, .width_param = "A_WIDTH"},
    {.name = "B", .dir = ODIN3_DIR_IN, .width_param = "B_WIDTH"},
    {.name = "Y", .dir = ODIN3_DIR_OUT, .width_param = "Y_WIDTH"},
};
static const odin3_celltype_def POW_DEF = {.name = "o3test_pow",
                                           .gran = ODIN3_GRAN_HARD,
                                           .ports = POW_PORTS,
                                           .n_ports = 3,
                                           .params = POW_PARAMS,
                                           .n_params = 3};

static void register_pow_once(void) {
    static bool registered;
    if (!registered) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&POW_DEF));
        registered = true;
    }
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

static void expect_params(odin3_module *module, odin3_node_id node, const int64_t *want,
                          uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT64(want[i], odin3_node_param(module, node, i)->i);
    }
}

/* Review Focus 5: a registered parametric type with no .model takes its parameters from the
 * largest bit index of each port's formals (per instance); unnamed ports keep the default. */
static void test_subckt_of_registered_parametric_type_infers_params(void) {
    register_pow_once();
    write_str(".model top\n.inputs a b\n.outputs y\n"
              ".subckt o3test_pow A[1]=a Y[7]=y\n"
              ".subckt o3test_pow A[0]=a B[2]=b Y[0]=y2\n.end\n");
    read_ok(PATH);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(design));
    odin3_module *top = module_at(1);
    odin3_node_id first = cell_at(top, 0);
    TEST_ASSERT_EQUAL_STRING("o3test_pow", type_of(top, first));
    expect_params(top, first, (const int64_t[]){2, 1, 8}, 3);
    TEST_ASSERT_EQUAL_STRING("a", pin_net_at(top, first, (pin_at){0, 1}));
    TEST_ASSERT_EQUAL_STRING("", pin_net_at(top, first, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("y", pin_net_at(top, first, (pin_at){2, 7}));
    expect_params(top, cell_at(top, 1), (const int64_t[]){1, 3, 1}, 3);
    expect_check_clean();
}

/* A scalar formal never names a port sized by a parameter (no .model says it is scalar). */
static void test_subckt_of_registered_parametric_type_needs_bit_formals(void) {
    register_pow_once();
    expect_parse_error(".model top\n.inputs a\n.subckt o3test_pow A=a\n.end\n", 3, "no port 'A'");
}

/* An inferred width is capped (2^20 bits): a one-line .subckt cannot force a huge allocation. */
static void test_inferred_width_is_capped(void) {
    register_pow_once();
    expect_parse_error(".model top\n.inputs a\n.subckt o3test_pow A[1048576]=a\n.end\n", 3,
                       "formal 'A[1048576]' implies a port width above 1048576 bits");
    register_pow_once();
    expect_parse_error(".model top\n.inputs a\n.subckt o3test_pow Y[999999999]=a\n.end\n", 3,
                       "above 1048576 bits");
}

/* The cap covers constant widths too: a registered type with a huge constant port is refused on
 * its first undeclared .subckt, before any pin is made. */
static void test_constant_width_is_capped(void) {
    static const odin3_port_def ports[] = {
        {.name = "a", .dir = ODIN3_DIR_IN, .width = 4294967294U},
        {.name = "Y", .dir = ODIN3_DIR_OUT, .scalar = true, .width = 1}};
    const odin3_celltype_def def = {
        .name = "o3test_big", .gran = ODIN3_GRAN_HARD, .ports = ports, .n_ports = 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, NULL));
    expect_parse_error(".model top\n.inputs x\n.outputs y\n.subckt o3test_big a[0]=x Y=y\n.end\n",
                       4, "gives port 'a' 4294967294 bits here, above the 1048576");
}

/* The total width of an undeclared instance is capped too (2^22 bits): many ports each under the
 * per-port cap must not add up to gigabytes. */
static void test_total_width_is_capped(void) {
    static odin3_port_def ports[5];
    for (uint32_t i = 0; i < 5; i++) {
        static const char *const names[] = {"a", "b", "c", "d", "e"};
        ports[i] = (odin3_port_def){.name = names[i], .dir = ODIN3_DIR_IN, .width = 1U << 20};
    }
    const odin3_celltype_def def = {
        .name = "o3test_wide", .gran = ODIN3_GRAN_HARD, .ports = ports, .n_ports = 5};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, NULL));
    expect_parse_error(".model top\n.inputs x\n.subckt o3test_wide a[0]=x\n.end\n", 3,
                       "'o3test_wide' gives its ports 5242880 bits in total here, above the "
                       "4194304");
}

/* A module of the file is sized by its own definition: no cap applies to its instances. */
static void test_module_instance_not_capped(void) {
    static char text[1U << 24];
    int at = snprintf(text, sizeof text,
                      ".model top\n.inputs x\n.subckt m w[1048576]=x\n.end\n"
                      ".model m\n.inputs");
    for (uint32_t k = 0; k <= (1U << 20); k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " w[%u]", (unsigned)k);
    }
    (void)snprintf(text + at, sizeof text - (size_t)at, "\n.end\n");
    write_str(text);
    read_ok(PATH);
}

/* Review Focus 1: a declared parametric model in another port order and other scalar flags; its
 * instances get the declared parameters and use the declared spelling of each port. */
static void test_declared_parametric_model(void) {
    register_pow_once();
    write_str(".model top\n.inputs a b\n.outputs y\n"
              ".subckt o3test_pow A=a B[0]=b Y[1]=y\n.end\n"
              ".model o3test_pow\n.inputs B[0] B[1] B[2] A\n.outputs Y[0] Y[1]\n.blackbox\n.end\n");
    read_ok(PATH);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    const odin3_value *params = odin3_design_declared_model_params(design, 0);
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL_INT64(1, params[0].i);
    TEST_ASSERT_EQUAL_INT64(3, params[1].i);
    TEST_ASSERT_EQUAL_INT64(2, params[2].i);
    const odin3_celltype_def *decl = odin3_design_declared_model_decl(design, 0);
    TEST_ASSERT_EQUAL_STRING("B", decl->ports[0].name);
    TEST_ASSERT_TRUE(decl->ports[1].scalar);
    odin3_module *top = module_at(1);
    odin3_node_id node = cell_at(top, 0);
    expect_params(top, node, (const int64_t[]){1, 3, 2}, 3);
    TEST_ASSERT_EQUAL_STRING("a", pin_net_at(top, node, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("y", pin_net_at(top, node, (pin_at){2, 1}));
    expect_check_clean();
}

/* Instances of a declared model are sized by the declaration, not by their formals. */
static void test_declared_parametric_model_bounds_formals(void) {
    register_pow_once();
    expect_parse_error(".model top\n.inputs a\n.subckt o3test_pow B[3]=a\n.end\n"
                       ".model o3test_pow\n.inputs B[0] B[1] B[2] A\n.outputs Y\n.blackbox\n.end\n",
                       3, "no port 'B[3]'");
}

/* Review Focus 2: declared widths the registered type cannot have: a located error with why. */
static void test_declared_parametric_model_contradiction(void) {
    register_pow_once();
    expect_parse_error(".model top\n.end\n.model o3test_pow\n.inputs A B\n.outputs Y Z\n"
                       ".blackbox\n.end\n",
                       3,
                       "conflicts with the registered cell type of that name: the cell type has no "
                       "port 'Z'");
    TEST_ASSERT_EQUAL_UINT32(1, error_count);
}

/* Odin II spells a width-1 port `cin[0]`: the declaration says so, and instances follow it. */
static void test_declared_bracketed_scalar_port(void) {
    register_hard_once();
    write_str(".model top\n.inputs x y\n.outputs s\n"
              ".subckt o3test_hard_adder a[1]=x cin[0]=y s[0]=s\n.end\n"
              ".model o3test_hard_adder\n.inputs cin[0] a[0] a[1]\n.outputs s[0]\n"
              ".blackbox\n.end\n");
    read_ok(PATH);
    odin3_module *top = module_at(1);
    odin3_node_id node = cell_at(top, 0);
    TEST_ASSERT_EQUAL_STRING("y", pin_net_at(top, node, (pin_at){1, 0}));
    TEST_ASSERT_FALSE(odin3_design_declared_model_decl(design, 0)->ports[0].scalar);
    expect_check_clean();
}

/* A model neither in the file nor registered (Yosys's `$pow`) is an implicit black box. */
static void test_subckt_of_an_undeclared_model_is_an_implicit_black_box(void) {
    write_str(".model top\n.inputs a b\n.outputs y\n.subckt $pow A[0]=a B[0]=b Y[0]=y\n"
              ".subckt $pow A[0]=b Z=y2\n.end\n");
    read_ok(PATH);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(design));
    odin3_module *top = module_at(1);
    odin3_node_id first = cell_at(top, 0);
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(top, first));
    TEST_ASSERT_EQUAL_STRING("$pow", def->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    static const char *const formals[] = {"A[0]", "B[0]", "Y[0]", "Z"};
    TEST_ASSERT_EQUAL_UINT32(4, def->n_ports);
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_STRING(formals[i], def->ports[i].name);
        TEST_ASSERT_EQUAL_INT(ODIN3_DIR_INOUT, def->ports[i].dir);
        TEST_ASSERT_TRUE(def->ports[i].scalar);
        TEST_ASSERT_EQUAL_UINT32(1, def->ports[i].width);
    }
    TEST_ASSERT_EQUAL_STRING("y", pin_net_at(top, first, (pin_at){2, 0}));
    TEST_ASSERT_EQUAL_STRING("", pin_net_at(top, first, (pin_at){3, 0}));
    odin3_node_id second = cell_at(top, 1);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_type(top, first).v, odin3_node_type(top, second).v);
    TEST_ASSERT_EQUAL_STRING("b", pin_net_at(top, second, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("", pin_net_at(top, second, (pin_at){1, 0}));
    TEST_ASSERT_EQUAL_STRING("y2", pin_net_at(top, second, (pin_at){3, 0}));
    expect_check_clean();
}

static void test_implicit_black_box_malformed_connection(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt nope x\n.end\n", 3, "formal=actual");
}

static void test_subckt_unknown_formal(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt m q=a\n.end\n"
                       ".model m\n.inputs p[0] p[1]\n.end\n",
                       3, "no port 'q'");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.inputs a\n.subckt m p[2]=a\n.end\n"
                       ".model m\n.inputs p[0] p[1]\n.end\n",
                       3, "no port 'p[2]'");
}

static void test_subckt_wide_port_by_name(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt m p=a\n.end\n"
                       ".model m\n.inputs p[0] p[1]\n.end\n",
                       3, "no port 'p'");
}

/* A bare formal binds only a scalar port: `o` does not name the width-1 vector `o[0]`. */
static void test_subckt_bare_formal_of_width1_vector(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt m o=a\n.end\n"
                       ".model m\n.outputs o[0]\n.end\n",
                       3, "no port 'o'");
}

static void test_subckt_formal_twice(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt m p[1]=a p[1]=a\n.end\n"
                       ".model m\n.inputs p[0] p[1]\n.end\n",
                       3, "connected twice");
}

static void test_subckt_malformed_connection(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt bb x\n.end\n"
                       ".model bb\n.inputs x\n.blackbox\n.end\n",
                       3, "formal=actual");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.inputs a\n.subckt bb x=\n.end\n"
                       ".model bb\n.inputs x\n.blackbox\n.end\n",
                       3, "formal=actual");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.subckt\n.end\n", 2, ".subckt needs a model");
}

static void test_subckt_of_itself(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt top a=a\n.end\n", 3, "instantiates itself");
}

static void test_subckt_of_a_port_type(void) {
    expect_parse_error(".model top\n.inputs a\n.subckt $port_in Y=a\n.end\n", 3,
                       "cannot be instantiated");
}

static void test_cname_errors(void) {
    expect_parse_error(".model top\n.cname u1\n.end\n", 2, "no previous cell");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.names a\n.cname\n.end\n", 3, ".cname takes one name");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.names a\n.cname u\n.names b\n.cname u\n.end\n", 5,
                       "duplicate cell name 'u'");
}

static void test_attr_and_param_errors(void) {
    expect_parse_error(".model top\n.attr k v\n.end\n", 2, "no previous cell");
    odin3_design_destroy(design);
    design = odin3_design_create();
    expect_parse_error(".model top\n.names a\n.param k\n.end\n", 3, "a key and a value");
}

/* The previous cell does not carry over to the next model. */
static void test_cname_does_not_cross_models(void) {
    expect_parse_error(".model top\n.names a\n.end\n.model m\n.cname u\n.end\n", 5,
                       "no previous cell");
}

static void test_first_model_must_be_a_module(void) {
    expect_parse_error(".model bb\n.inputs a\n.blackbox\n.end\n.model top\n.end\n", 1,
                       "the first model must be a module");
}

/* --- the file changing between the passes ------------------------------------------------ */

static void rewrite_hook(void *user) {
    write_str((const char *)user);
}

static void expect_changed(const char *second, uint32_t line) {
    write_str(".model top\n.inputs a\n.end\n.model m\n.end\n");
    odin3_blif_test_set_between_passes(rewrite_hook, (void *)second);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(design, PATH));
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want, "%s:%u: file changed during read", PATH, (unsigned)line);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
    odin3_blif_test_set_between_passes(NULL, NULL);
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

static void test_file_changed_between_passes(void) {
    expect_changed(".model top\n.end\n.model m\n.end\n.model extra\n.end\n", 5); /* more */
    expect_changed(".model top\n.end\n.model n\n.end\n", 3);                     /* renamed */
    expect_changed(".model top\n.names a\n.end\n", 3);                           /* fewer */
    expect_changed(".names a\n.model top\n.end\n", 1); /* body before any .model */
}

/* Out of memory before any line is read is logged without a line number. */
static void test_out_of_memory_before_reading(void) {
    write_str(".model top\n.end\n");
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_blif_read(design, PATH));
    odin3_util_set_alloc_fail_after(-1);
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want, "%s: out of memory", PATH);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
}

/* --- copied goldens ------------------------------------------------------------------------ */

static void test_golden_ff(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/ff.odin.blif");
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_STRING("dff", str_of(odin3_module_name(top)));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(top));
    expect_port(top, 0, (port_want){"dff^clk", ODIN3_DIR_IN, 1, true});
    expect_port(top, 3, (port_want){"dff^q", ODIN3_DIR_OUT, 1, true});
    expect_cover(top, cell_at(top, 0), 0, "");  /* gnd */
    expect_cover(top, cell_at(top, 2), 0, "1"); /* vcc */
    expect_cover(top, cell_at(top, 3), 4, "1-1-1-1-11");
    expect_latch(top, 4, (latch_want){"$_DFF_P_", 3, "dff^clk", "dff^nMUX~0^MUX_2~3", "dff^q_FF"});
    TEST_ASSERT_EQUAL_UINT32(4 + 8 + 1, odin3_module_node_end(top));
}

static void test_golden_ff_parmys(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/ff.parmys.blif");
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(top));
    expect_latch(top, 3,
                 (latch_want){"$_DFF_P_", 2, "clk", "$auto$rtlil.cc:3203:MuxGate$139", "q"});
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
    odin3_module *top = module_at(1);
    odin3_node_id first = cell_at(top, 3); /* a=vcc b=gnd cin=gnd cout=… sumout=… */
    TEST_ASSERT_EQUAL_STRING("adder", type_of(top, first));
    TEST_ASSERT_EQUAL_STRING("vcc", pin_net_at(top, first, (pin_at){0, 0}));
    TEST_ASSERT_EQUAL_STRING("gnd", pin_net_at(top, first, (pin_at){1, 0}));
    TEST_ASSERT_EQUAL_STRING("$add~1^ADD~0-0[0]", pin_net_at(top, first, (pin_at){4, 0}));
    TEST_ASSERT_EQUAL_UINT32(8 + 3 + 8 + 1, odin3_module_node_end(top));
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

/* Undeclared Yosys cells: $_DFFSR_PPP_ with formals C D Q R S in first-use order. */
static void test_golden_dffsre_implicit_cell(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/dffsre.parmys.blif");
    odin3_module *top = module_at(1);
    odin3_node_id ff = cell_at(top, 3);
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(top, ff));
    TEST_ASSERT_EQUAL_STRING("$_DFFSR_PPP_", def->name);
    TEST_ASSERT_EQUAL_UINT32(5, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("S", def->ports[4].name);
    TEST_ASSERT_EQUAL_STRING("CLR~1", pin_net_at(top, ff, (pin_at){3, 0}));
    expect_check_clean();
}

/* Odin II wrote two drivers on one net: read as written, check reports rule 4. */
static void test_golden_multiple_drivers(void) {
    read_ok(ODIN3_BLIF_FIXTURES "/elsif_both_defined.odin.blif");
    odin3_module *top = module_at(1);
    TEST_ASSERT_EQUAL_UINT32(
        2, odin3_net_driver_count(top, odin3_module_find_net(top, intern("simple_op^out"))));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_CHECK,
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Every allocation failure gives NO_MEMORY (or IO when the lexer itself cannot be opened). */
static void oom_sweep(const char *path) {
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        odin3_design_destroy(design);
        design = odin3_design_create();
        TEST_ASSERT_NOT_NULL(design);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_blif_read(design, path);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            done = true;
        } else if (st != ODIN3_ERR_NO_MEMORY) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
        }
    }
    TEST_ASSERT_TRUE(done);
}

static void test_out_of_memory_sweep(void) {
    oom_sweep(HAND);
    oom_sweep(BODY);
    oom_sweep(ODIN3_BLIF_FIXTURES "/dffsre.parmys.blif");
}

/* The same through the parametric paths: a declared parametric model, then inferred parameters. */
static void test_out_of_memory_sweep_parametric(void) {
    register_pow_once();
    write_str(".model top\n.inputs a b\n.outputs y\n.subckt o3test_pow A=a B[0]=b Y[1]=y\n.end\n"
              ".model o3test_pow\n.inputs B[0] B[1] B[2] A\n.outputs Y[0] Y[1]\n.blackbox\n.end\n");
    oom_sweep(PATH);
    write_str(".model top\n.inputs a\n.outputs y\n.subckt o3test_pow A[1]=a Y[7]=y\n.end\n");
    oom_sweep(PATH);
}

static void run_pass1_tests(void) {
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
    RUN_TEST(test_blackbox_repeats_a_port_name);
    RUN_TEST(test_subckt_of_registered_parametric_type_infers_params);
    RUN_TEST(test_subckt_of_registered_parametric_type_needs_bit_formals);
    RUN_TEST(test_inferred_width_is_capped);
    RUN_TEST(test_constant_width_is_capped);
    RUN_TEST(test_total_width_is_capped);
    RUN_TEST(test_module_instance_not_capped);
    RUN_TEST(test_declared_parametric_model);
    RUN_TEST(test_declared_parametric_model_bounds_formals);
    RUN_TEST(test_declared_parametric_model_contradiction);
    RUN_TEST(test_declared_bracketed_scalar_port);
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
}

static void run_pass2_and_golden_tests(void) {
    RUN_TEST(test_names_become_sop_with_rows_as_written);
    RUN_TEST(test_latch_forms);
    RUN_TEST(test_subckt_of_a_later_model_by_bit_formals);
    RUN_TEST(test_subckt_of_black_box_leaves_unlisted_formals_open);
    RUN_TEST(test_attr_and_param_on_the_previous_cell);
    RUN_TEST(test_body_provenance);
    RUN_TEST(test_check_clean_after_every_fixture);
    RUN_TEST(test_subckt_formal_matches_scalar_bit_named_port);
    RUN_TEST(test_multiple_drivers_are_read);
    RUN_TEST(test_cover_row_width_mismatch);
    RUN_TEST(test_cover_row_bad_characters);
    RUN_TEST(test_cover_row_outside_names);
    RUN_TEST(test_names_needs_an_output);
    RUN_TEST(test_latch_async_type_rejected);
    RUN_TEST(test_latch_unknown_type);
    RUN_TEST(test_latch_bad_init);
    RUN_TEST(test_latch_token_count);
    RUN_TEST(test_subckt_of_an_undeclared_model_is_an_implicit_black_box);
    RUN_TEST(test_implicit_black_box_malformed_connection);
    RUN_TEST(test_subckt_unknown_formal);
    RUN_TEST(test_subckt_wide_port_by_name);
    RUN_TEST(test_subckt_bare_formal_of_width1_vector);
    RUN_TEST(test_cover_mixing_on_and_off_set);
    RUN_TEST(test_attr_value_blank_runs_collapse);
    RUN_TEST(test_file_changed_between_passes);
    RUN_TEST(test_out_of_memory_before_reading);
    RUN_TEST(test_subckt_formal_twice);
    RUN_TEST(test_subckt_malformed_connection);
    RUN_TEST(test_subckt_of_itself);
    RUN_TEST(test_subckt_of_a_port_type);
    RUN_TEST(test_cname_errors);
    RUN_TEST(test_attr_and_param_errors);
    RUN_TEST(test_cname_does_not_cross_models);
    RUN_TEST(test_first_model_must_be_a_module);
    RUN_TEST(test_missing_file_is_io_error);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_golden_ff);
    RUN_TEST(test_golden_ff_parmys);
    RUN_TEST(test_golden_adder_hard_block);
    RUN_TEST(test_golden_multi_model_with_black_boxes);
    RUN_TEST(test_golden_dffsre_implicit_cell);
    RUN_TEST(test_golden_multiple_drivers);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_out_of_memory_sweep_parametric);
}

int main(void) {
    UNITY_BEGIN();
    run_pass1_tests();
    run_pass2_and_golden_tests();
    return UNITY_END();
}
