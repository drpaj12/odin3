/*
 * test_blif_writer.c — unit tests for the BLIF writer (read → write → read round trips).
 */
#include "backends/blif/writer.h"
#include "frontends/blif/attrs.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_BLIF_FIXTURES
#error "ODIN3_BLIF_FIXTURES must name tests/golden/blif"
#endif

enum { MSG_MAX = 512, TEXT_MAX = 1 << 16, NAME_MAX_LEN = 64, OOM_SWEEP = 20000, WRAP = 100 };

static const char *const IN_PATH = "odin3_writer_test_in.blif";
static const char *const OUT_PATH = "odin3_writer_test_out.blif";
static const char *const OUT2_PATH = "odin3_writer_test_out2.blif";

static const char *const FIXTURES[] = {
    ODIN3_BLIF_FIXTURES "/adder_hard_block.parmys.blif",
    ODIN3_BLIF_FIXTURES "/ansiportlist_2.parmys.blif",
    ODIN3_BLIF_FIXTURES "/dffsre.parmys.blif",
    ODIN3_BLIF_FIXTURES "/elsif_both_defined.odin.blif",
    ODIN3_BLIF_FIXTURES "/ff.odin.blif",
    ODIN3_BLIF_FIXTURES "/ff.parmys.blif",
    ODIN3_BLIF_FIXTURES "/hand_body.blif",
    ODIN3_BLIF_FIXTURES "/hand_ports.blif",
    ODIN3_BLIF_FIXTURES "/pow.parmys.blif",
};
enum { N_FIXTURES = sizeof FIXTURES / sizeof FIXTURES[0] };

static char last_error[MSG_MAX];
static odin3_design *design;  /* the design read from the input */
static odin3_design *design2; /* the design read back from the writer's output */

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
    design2 = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_NOT_NULL(design2);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    odin3_design_destroy(design2);
    design = NULL;
    design2 = NULL;
    (void)remove(IN_PATH);
    (void)remove(OUT_PATH);
    (void)remove(OUT2_PATH);
}

/* --- helpers ------------------------------------------------------------------------------- */

/* Writes data to IN_PATH. */
static void write_input(const char *data) {
    FILE *file = fopen(IN_PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(data), fwrite(data, 1, strlen(data), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The whole file at path, NUL-terminated, in a static buffer of TEXT_MAX bytes. */
static const char *slurp(const char *path, char *buf) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[len] = '\0';
    return buf;
}

static const char *str_in(const odin3_design *des, uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(des), id);
}

static uint32_t intern_in(odin3_design *des, const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(des, odin3_bytes_cstr(name), &id));
    return id;
}

/* The ID of name in des, 0 when it was never interned (so nothing can be named by it). */
static uint32_t find_in(const odin3_design *des, const char *name) {
    uint32_t id = 0;
    return odin3_strtab_find(odin3_design_strtab(des), odin3_bytes_cstr(name), &id) ? id : 0;
}

static odin3_module *module_of(odin3_design *des, uint32_t id) {
    odin3_module *module = odin3_module_get(des, (odin3_module_id){id});
    TEST_ASSERT_NOT_NULL(module);
    return module;
}

static void read_into(odin3_design *des, const char *path) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(des, path), last_error);
    TEST_ASSERT_EQUAL_STRING("", last_error);
}

static void write_ok(const odin3_design *des, const char *path) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(des, path), last_error);
    TEST_ASSERT_EQUAL_STRING("", last_error);
}

/* --- design comparison --------------------------------------------------------------------- */

/* A pair of objects, one per design, compared by name and content. */
typedef struct side {
    odin3_design *des;
    odin3_module *module;
} side;

static void expect_same_value(const side *one, const side *two, const odin3_value *lhs,
                              const odin3_value *rhs) {
    TEST_ASSERT_NOT_NULL(lhs);
    TEST_ASSERT_NOT_NULL(rhs);
    TEST_ASSERT_EQUAL_INT(lhs->kind, rhs->kind);
    if (lhs->kind == ODIN3_VAL_STRING) {
        TEST_ASSERT_EQUAL_STRING(str_in(one->des, lhs->str), str_in(two->des, rhs->str));
        return;
    }
    TEST_ASSERT_EQUAL_INT64(lhs->i, rhs->i);
    TEST_ASSERT_EQUAL_UINT32(lhs->len, rhs->len);
    TEST_ASSERT_EQUAL_UINT32(lhs->cover_inputs, rhs->cover_inputs);
    if (lhs->len > 0) {
        TEST_ASSERT_EQUAL_MEMORY(lhs->bits, rhs->bits, lhs->len);
    }
}

/* Attribute key (a string) of obj in both designs: both absent or both equal. */
static void expect_same_attr(const side *one, const side *two, odin3_objref obj, const char *key) {
    uint32_t key1 = find_in(one->des, key);
    uint32_t key2 = find_in(two->des, key);
    const odin3_value *lhs = key1 == 0 ? NULL : odin3_attr_get(one->module, obj, key1);
    const odin3_value *rhs = key2 == 0 ? NULL : odin3_attr_get(two->module, obj, key2);
    TEST_ASSERT_EQUAL_MESSAGE(lhs == NULL, rhs == NULL, key);
    if (lhs != NULL) {
        expect_same_value(one, two, lhs, rhs);
    }
}

/* Cell-type ports: names, directions, widths and bracket forms. */
static void expect_same_ports(const odin3_celltype_def *lhs, const odin3_celltype_def *rhs) {
    TEST_ASSERT_EQUAL_STRING(lhs->name, rhs->name);
    TEST_ASSERT_EQUAL_UINT32(lhs->n_ports, rhs->n_ports);
    for (uint32_t i = 0; i < lhs->n_ports; i++) {
        TEST_ASSERT_EQUAL_STRING(lhs->ports[i].name, rhs->ports[i].name);
        TEST_ASSERT_EQUAL_INT(lhs->ports[i].dir, rhs->ports[i].dir);
        TEST_ASSERT_EQUAL_UINT32(lhs->ports[i].width, rhs->ports[i].width);
        TEST_ASSERT_EQUAL(lhs->ports[i].scalar, rhs->ports[i].scalar);
    }
}

static const char *net_text(const side *one, odin3_net_id net) {
    return odin3_net_valid(net) ? str_in(one->des, odin3_net_name(one->module, net)) : "";
}

/* Node by node in ID order: type, name, parameters, pin nets (by name) and BLIF attributes. */
static void expect_same_node(const side *one, const side *two, odin3_node_id node) {
    odin3_celltype_id type1 = odin3_node_type(one->module, node);
    odin3_celltype_id type2 = odin3_node_type(two->module, node);
    const odin3_celltype_def *def = odin3_celltype_get(one->des, type1);
    TEST_ASSERT_EQUAL_STRING(def->name, odin3_celltype_get(two->des, type2)->name);
    TEST_ASSERT_EQUAL_STRING(str_in(one->des, odin3_node_name(one->module, node)),
                             str_in(two->des, odin3_node_name(two->module, node)));
    for (uint32_t i = 0; i < def->n_params; i++) {
        expect_same_value(one, two, odin3_node_param(one->module, node, i),
                          odin3_node_param(two->module, node, i));
    }
    odin3_pinslice pins1 = odin3_node_pins(one->module, node);
    odin3_pinslice pins2 = odin3_node_pins(two->module, node);
    TEST_ASSERT_EQUAL_UINT32(pins1.count, pins2.count);
    for (uint32_t k = 0; k < pins1.count; k++) {
        odin3_net_id net1 = odin3_pin_net(one->module, (odin3_pin_id){pins1.first.v + k});
        odin3_net_id net2 = odin3_pin_net(two->module, (odin3_pin_id){pins2.first.v + k});
        TEST_ASSERT_EQUAL_STRING(net_text(one, net1), net_text(two, net2));
    }
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    expect_same_attr(one, two, obj, ODIN3_BLIF_ATTR_EXTRAS);
    uint32_t key = find_in(one->des, ODIN3_BLIF_ATTR_EXTRAS);
    const odin3_value *extras = key == 0 ? NULL : odin3_attr_get(one->module, obj, key);
    if (extras == NULL) {
        return;
    }
    char list[MSG_MAX];
    (void)snprintf(list, sizeof list, "%s", str_in(one->des, extras->str));
    for (char *save = NULL, *word = strtok_r(list, " ", &save); word != NULL;
         word = strtok_r(NULL, " ", &save)) {
        expect_same_attr(one, two, obj, word);
    }
}

static uint32_t live_nets(const odin3_module *module) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        count += odin3_net_live(module, (odin3_net_id){i}) ? 1U : 0U;
    }
    return count;
}

/* Every live net of one has a name that names a live net of two; the live counts agree. */
static void expect_same_nets(const side *one, const side *two) {
    TEST_ASSERT_EQUAL_UINT32(live_nets(one->module), live_nets(two->module));
    for (uint32_t i = 1; i < odin3_module_net_end(one->module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(one->module, net)) {
            const char *name = net_text(one, net);
            uint32_t str = find_in(two->des, name);
            TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(odin3_module_find_net(two->module, str)),
                                     name);
        }
    }
}

static void expect_same_module(const side *one, const side *two) {
    TEST_ASSERT_EQUAL_STRING(str_in(one->des, odin3_module_name(one->module)),
                             str_in(two->des, odin3_module_name(two->module)));
    expect_same_ports(odin3_celltype_get(one->des, odin3_module_celltype(one->module)),
                      odin3_celltype_get(two->des, odin3_module_celltype(two->module)));
    uint32_t ports = odin3_module_port_count(one->module);
    for (uint32_t i = 0; i < ports; i++) {
        odin3_wire_id wire = odin3_module_port_wire(one->module, i);
        TEST_ASSERT_EQUAL_UINT32(wire.v, odin3_module_port_wire(two->module, i).v);
        expect_same_attr(one, two, (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                         ODIN3_BLIF_ATTR_PORT_NAME);
    }
    expect_same_attr(one, two, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(one->module).v},
                     ODIN3_BLIF_ATTR_CLOCK);
    TEST_ASSERT_EQUAL_UINT32(odin3_module_node_end(one->module),
                             odin3_module_node_end(two->module));
    for (uint32_t i = 1; i < odin3_module_node_end(one->module); i++) {
        TEST_ASSERT_TRUE(odin3_node_live(two->module, (odin3_node_id){i}));
        expect_same_node(one, two, (odin3_node_id){i});
    }
    expect_same_nets(one, two);
}

/* design and design2 agree on modules, ports, cells, nets and declared models. */
static void expect_same_designs(void) {
    TEST_ASSERT_EQUAL_UINT32(odin3_design_module_end(design), odin3_design_module_end(design2));
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        side one = {design, module_of(design, i)};
        side two = {design2, module_of(design2, i)};
        expect_same_module(&one, &two);
    }
    uint32_t models = odin3_design_declared_model_count(design);
    TEST_ASSERT_EQUAL_UINT32(models, odin3_design_declared_model_count(design2));
    for (uint32_t i = 0; i < models; i++) {
        expect_same_ports(odin3_celltype_get(design, odin3_design_declared_model(design, i)),
                          odin3_celltype_get(design2, odin3_design_declared_model(design2, i)));
    }
}

/* Reads path, writes it, reads the output back, compares, and checks that writing the second
 * design gives the same bytes again. */
static void round_trip(const char *path) {
    static char text1[TEXT_MAX];
    static char text2[TEXT_MAX];
    read_into(design, path);
    write_ok(design, OUT_PATH);
    read_into(design2, OUT_PATH);
    expect_same_designs();
    write_ok(design2, OUT2_PATH);
    TEST_ASSERT_EQUAL_STRING(slurp(OUT_PATH, text1), slurp(OUT2_PATH, text2));
}

/* The ports of design2's top module have exactly these names (NULL-terminated list). */
static void expect_port_names(const char *first, ...) {
    odin3_module *module = module_of(design2, 1);
    const odin3_celltype_def *def = odin3_celltype_get(design2, odin3_module_celltype(module));
    va_list args;
    va_start(args, first);
    uint32_t count = 0;
    for (const char *name = first; name != NULL; name = va_arg(args, const char *)) {
        TEST_ASSERT_TRUE(count < def->n_ports);
        TEST_ASSERT_EQUAL_STRING(name, def->ports[count].name);
        count++;
    }
    va_end(args);
    TEST_ASSERT_EQUAL_UINT32(count, def->n_ports);
}

/* --- tests --------------------------------------------------------------------------------- */

static void test_round_trip_every_fixture(void) {
    for (uint32_t i = 0; i < N_FIXTURES; i++) {
        odin3_design_destroy(design);
        odin3_design_destroy(design2);
        design = odin3_design_create();
        design2 = odin3_design_create();
        round_trip(FIXTURES[i]);
    }
}

/* A small file with every construct, and its exact written text. */
static const char *const EVERY_INPUT = "# comment\n"
                                       ".model top\n"
                                       ".inputs a b clk v[0] v[1]\n"
                                       ".outputs y q[0] q[1]\n"
                                       ".clock clk\n"
                                       ".names a b n1\n"
                                       "11 1\n"
                                       "0- 1\n"
                                       ".latch n1 q[0] re clk 2\n"
                                       ".latch y q[1] fe clk\n"
                                       ".latch a l1 ah clk 0\n"
                                       ".latch b l2 al clk\n"
                                       ".latch l1 l3\n"
                                       ".latch l2 l4 1\n"
                                       ".subckt sub i[1]=a o=s1\n"
                                       ".cname u_sub\n"
                                       ".attr src \"top.v:3\"\n"
                                       ".param P 01 01\n"
                                       ".subckt bb x[1]=v[1] x[0]=v[0] z=y\n"
                                       ".end\n"
                                       ".model sub\n"
                                       ".inputs i[0] i[1]\n"
                                       ".outputs o\n"
                                       ".names i[0] i[1] o\n"
                                       "1- 1\n"
                                       ".end\n"
                                       ".model bb\n"
                                       ".inputs x[0] x[1]\n"
                                       ".outputs z\n"
                                       ".blackbox\n"
                                       ".end\n";
static const char *const EVERY_OUTPUT = ".model top\n"
                                        ".inputs a b clk v[0] v[1]\n"
                                        ".outputs y q[0] q[1]\n"
                                        ".clock clk\n"
                                        ".names a b n1\n"
                                        "11 1\n"
                                        "0- 1\n"
                                        ".latch n1 q[0] re clk 2\n"
                                        ".latch y q[1] fe clk 3\n"
                                        ".latch a l1 ah clk 0\n"
                                        ".latch b l2 al clk 3\n"
                                        ".latch l1 l3 3\n"
                                        ".latch l2 l4 1\n"
                                        ".subckt sub i[1]=a o=s1\n"
                                        ".cname u_sub\n"
                                        ".attr src \"top.v:3\"\n"
                                        ".param P 01 01\n"
                                        ".subckt bb x[0]=v[0] x[1]=v[1] z=y\n"
                                        ".end\n"
                                        "\n"
                                        ".model sub\n"
                                        ".inputs i[0] i[1]\n"
                                        ".outputs o\n"
                                        ".names i[0] i[1] o\n"
                                        "1- 1\n"
                                        ".end\n"
                                        "\n"
                                        ".model bb\n"
                                        ".inputs x[0] x[1]\n"
                                        ".outputs z\n"
                                        ".blackbox\n"
                                        ".end\n";

static void test_written_text(void) {
    write_input(EVERY_INPUT);
    read_into(design, IN_PATH);
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(EVERY_OUTPUT, slurp(OUT_PATH, text));
}

/* Review Focus 3: zero-input .names, constant 0 (empty cover) and constant 1 (row `1`). */
static void test_zero_input_names(void) {
    write_input(".model top\n.outputs k0 k1\n.names k0\n.names k1\n1\n.end\n");
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.outputs k0 k1\n.names k0\n.names k1\n1\n.end\n",
                             slurp(OUT_PATH, text));
}

/* Review Focus 5: a name in both .inputs and .outputs is written in both lists. */
static void test_name_in_inputs_and_outputs(void) {
    write_input(".model top\n.inputs x v[0] v[1]\n.outputs x v[0] v[1] y\n"
                ".names x y\n1 1\n.end\n");
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs x v[0] v[1]\n.outputs x v[0] v[1] y\n"
                             ".names x y\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    odin3_module *module = module_of(design2, 1);
    TEST_ASSERT_EQUAL_UINT32(5, odin3_module_port_count(module)); /* x v x v y */
    odin3_wire_id out_x = odin3_module_port_wire(module, 2);
    TEST_ASSERT_EQUAL_STRING("x$blif_port", str_in(design2, odin3_wire_name(module, out_x)));
    const odin3_value *blif = odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_WIRE, out_x.v},
                                             find_in(design2, ODIN3_BLIF_ATTR_PORT_NAME));
    TEST_ASSERT_NOT_NULL(blif);
    TEST_ASSERT_EQUAL_STRING("x", str_in(design2, blif->str));
}

/* A .subckt of a model whose port is in both lists: the formal is the BLIF name, once. */
static void test_subckt_of_model_with_in_and_out_port(void) {
    write_input(".model top\n.inputs a\n.outputs y\n.subckt sub x=a\n"
                ".names a y\n1 1\n.end\n"
                ".model sub\n.inputs x\n.outputs x\n.end\n");
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.outputs y\n.subckt sub x=a\n"
                             ".names a y\n1 1\n.end\n"
                             "\n.model sub\n.inputs x\n.outputs x\n.end\n",
                             slurp(OUT_PATH, text));
}

/* Ports in port order: runs of one direction share a line; .inputs may follow .outputs. */
static void test_port_direction_runs(void) {
    write_input(".model top\n.inputs a\n.outputs y\n.inputs b c\n.outputs z\n"
                ".names a b c y\n111 1\n.names y z\n1 1\n.end\n");
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.outputs y\n.inputs b c\n.outputs z\n"
                             ".names a b c y\n111 1\n.names y z\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
}

/* Appends ` a_long_input_name[k]` for k = 0 .. 39 to input at len; returns the new length. */
static size_t long_names(char *input, size_t len) {
    for (uint32_t k = 0; k < 40; k++) {
        len += (size_t)snprintf(input + len, TEXT_MAX - len, " a_long_input_name[%u]", k);
    }
    return len;
}

/* Lines of a text: how many end in a continuation, and the longest (without its newline). */
typedef struct line_stats {
    uint32_t wraps;
    size_t longest;
} line_stats;

static line_stats count_lines(const char *text) {
    line_stats stats = {0, 0};
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        end = end != NULL ? end : line + strlen(line);
        size_t len = (size_t)(end - line);
        stats.longest = len > stats.longest ? len : stats.longest;
        stats.wraps += len > 0 && end[-1] == '\\' ? 1U : 0U;
        line = *end != '\0' ? end + 1 : end;
    }
    return stats;
}

/* Lines longer than 100 columns wrap with ` \` between tokens; reading back regroups. */
static void test_long_lines_wrap(void) {
    static char input[TEXT_MAX];
    size_t len = (size_t)snprintf(input, sizeof input, ".model top\n.inputs");
    len = long_names(input, len);
    len += (size_t)snprintf(input + len, sizeof input - len, "\n.outputs y\n.names");
    len = long_names(input, len);
    (void)snprintf(input + len, sizeof input - len, " y\n%040d 1\n.end\n", 0);
    write_input(input);
    round_trip(IN_PATH);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_port_count(module_of(design2, 1)));
    static char text[TEXT_MAX];
    const char *out = slurp(OUT_PATH, text);
    TEST_ASSERT_NOT_NULL(strstr(out, " \\\n  a_long_input_name["));
    line_stats stats = count_lines(out);
    TEST_ASSERT_TRUE(stats.longest <= WRAP);
    TEST_ASSERT_TRUE(stats.wraps >= 2 * 8); /* 40 names of ~22 columns: 8+ wraps a line */
}

/* A token longer than the wrap column stays whole on its own continuation line. */
static void test_long_token_stays_whole(void) {
    static char name[2 * WRAP + 1];
    memset(name, 'n', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    static char input[TEXT_MAX];
    (void)snprintf(input, sizeof input, ".model top\n.inputs a %s\n.outputs y\n.end\n", name);
    write_input(input);
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    static char want[TEXT_MAX];
    (void)snprintf(want, sizeof want, ".model top\n.inputs a \\\n  %s\n.outputs y\n.end\n", name);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
}

/* --- generated names (designs built through the IR API) ------------------------------------ */

static odin3_celltype_id type_named(odin3_design *des, const char *name) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(des, intern_in(des, name), &type));
    return type;
}

static odin3_module *new_module(odin3_design *des, const char *name) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(des, intern_in(des, name), (odin3_prov_id){0}, &id));
    return module_of(des, id.v);
}

static odin3_wire_id new_port(odin3_module *module, const char *name, odin3_dir dir,
                              uint32_t width) {
    odin3_port_spec spec = {.name = intern_in(odin3_module_design(module), name),
                            .dir = dir,
                            .width = width,
                            .scalar = width == 1};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, &node));
    return odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
}

static odin3_net_id new_net(odin3_module *module, const char *name) {
    odin3_net_id net = {0};
    uint32_t str = name == NULL ? 0 : intern_in(odin3_module_design(module), name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, str, (odin3_prov_id){0}, &net));
    return net;
}

/* A buffer `.names in out` / `1 1` (in or out may be none: an unconnected pin). */
static odin3_node_id new_buffer(odin3_module *module, odin3_net_id in, odin3_net_id out) {
    odin3_design *des = odin3_module_design(module);
    static const uint8_t rows[] = {'1', '1'};
    odin3_value params[2] = {odin3_value_int(1), {.kind = ODIN3_VAL_COVER}};
    params[1].bits = rows;
    params[1].len = 2;
    params[1].cover_inputs = 1;
    odin3_node_spec spec = {.type = type_named(des, "$sop"), .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{&in, 1}, {&out, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return node;
}

/* Unnamed nets: a port net takes its wire bit's name, an internal net `$n<ID>`. */
static void test_generated_names(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 2);
    odin3_wire_id out = new_port(module, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id a0 = odin3_wire_net(module, in, 0);
    odin3_net_id a1 = odin3_wire_net(module, in, 1);
    odin3_net_id y_net = odin3_wire_net(module, out, 0);
    odin3_net_id mid = new_net(module, NULL);
    new_buffer(module, a0, mid);
    new_buffer(module, mid, y_net);
    odin3_node_id dangling = new_buffer(module, a1, (odin3_net_id){0});
    odin3_pin_id open_y = {odin3_node_port(module, dangling, 1).first.v};
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    static char want[TEXT_MAX];
    (void)snprintf(want, sizeof want,
                   ".model top\n.inputs a[0] a[1]\n.outputs y\n"
                   ".names a[0] $n%u\n1 1\n.names $n%u y\n1 1\n.names a[1] $p%u\n1 1\n.end\n",
                   (unsigned)mid.v, (unsigned)mid.v, (unsigned)open_y.v);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    TEST_ASSERT_EQUAL_UINT32(live_nets(module) + 1, live_nets(module_of(design2, 1)));
}

/* A generated name never repeats another net's name: the candidate moves on to `$n<ID>$k`. */
static void test_generated_names_are_unique(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id out = new_port(module, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id y_net = odin3_wire_net(module, out, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, y_net, 0)); /* its wire bit is `y` */
    odin3_net_id mid = new_net(module, NULL);                            /* ID 2: `$n2` is taken */
    TEST_ASSERT_EQUAL_UINT32(2, mid.v);
    odin3_net_id taken = new_net(module, "$n2");
    odin3_net_id taken1 = new_net(module, "$n2$1");
    new_buffer(module, taken, mid);
    new_buffer(module, mid, taken1);
    new_buffer(module, taken1, y_net);
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.outputs y\n"
                             ".names $n2 $n2$2\n1 1\n.names $n2$2 $n2$1\n1 1\n"
                             ".names $n2$1 y\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    static char again[TEXT_MAX];
    write_ok(design, OUT2_PATH);
    TEST_ASSERT_EQUAL_STRING(text, slurp(OUT2_PATH, again)); /* stable */
    read_into(design2, OUT_PATH);
    TEST_ASSERT_EQUAL_UINT32(live_nets(module), live_nets(module_of(design2, 1)));
}

/* A wire bit name that another net already has falls back to `$n<ID>`. */
static void test_wire_bit_name_taken(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 1);
    odin3_net_id a_net = odin3_wire_net(module, in, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, a_net, 0));
    odin3_net_id other = new_net(module, "a");
    new_buffer(module, a_net, other);
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    /* the port keeps its name; the other net `a` (ID 2) is written as `$n2` */
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.names a $n2\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    expect_port_names("a", NULL);
}

/* A port net with its own name keeps it; a buffer joins it to the port bit. */
static void test_port_net_with_another_name(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 1);
    odin3_wire_id out = new_port(module, "y", ODIN3_DIR_OUT, 1);
    odin3_net_id y_net = odin3_wire_net(module, out, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, y_net, intern_in(design, "inner")));
    new_buffer(module, odin3_wire_net(module, in, 0), y_net);
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.outputs y\n.names a inner\n1 1\n"
                             ".names inner y\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    expect_port_names("a", "y", NULL);
}

/* Two outputs on one net: the net takes the first port bit name, the second gets a buffer. */
static void test_two_outputs_share_a_net(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 1);
    odin3_wire_id out_y = new_port(module, "y", ODIN3_DIR_OUT, 1);
    odin3_wire_id out_z = new_port(module, "z", ODIN3_DIR_OUT, 1);
    odin3_net_id y_net = odin3_wire_net(module, out_y, 0);
    odin3_net_pair pair = {y_net, odin3_wire_net(module, out_z, 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, pair));
    new_buffer(module, odin3_wire_net(module, in, 0), y_net);
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.outputs y z\n.names a y\n1 1\n"
                             ".names y z\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    expect_port_names("a", "y", "z", NULL);
}

/* An input wired straight to an output (one net): the output gets a buffer from the input. */
static void test_input_feeds_output(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 1);
    odin3_wire_id out = new_port(module, "y", ODIN3_DIR_OUT, 1);
    odin3_net_pair pair = {odin3_wire_net(module, in, 0), odin3_wire_net(module, out, 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, pair));
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n",
                             slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    expect_port_names("a", "y", NULL);
}

/* A design listing one black box twice writes its model once. */
static void test_declared_black_box_written_once(void) {
    new_module(design, "top");
    static const odin3_port_def ports[] = {{"i", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                           {"o", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};
    odin3_celltype_def def = {
        .name = "bb", .gran = ODIN3_GRAN_BLACKBOX, .ports = ports, .n_ports = 2};
    odin3_celltype_id first = {0};
    odin3_celltype_id second = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &second));
    TEST_ASSERT_EQUAL_UINT32(first.v, second.v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_declared_model_count(design));
    write_ok(design, OUT_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.end\n\n.model bb\n.inputs i\n.outputs o\n"
                             ".blackbox\n.end\n",
                             slurp(OUT_PATH, text));
}

static void set_node_attr(odin3_module *module, odin3_node_id node, const char *key,
                          odin3_value value) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   intern_in(design, key), &value));
}

/* blif_extras keys whose attribute is missing, not a string, or has no .attr/.param prefix are
 * skipped with a warning each. */
static void test_extras_without_a_string_attribute(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 1);
    odin3_node_id node = new_buffer(module, odin3_wire_net(module, in, 0), new_net(module, "b"));
    odin3_value text = {.kind = ODIN3_VAL_STRING, .str = intern_in(design, "\"t.v:1\"")};
    odin3_value list = {.kind = ODIN3_VAL_STRING,
                        .str =
                            intern_in(design, "blif.attr:src blif.attr:gone blif.param:P other")};
    set_node_attr(module, node, "blif.attr:src", text);
    set_node_attr(module, node, "blif.param:P", odin3_value_int(1));
    set_node_attr(module, node, ODIN3_BLIF_ATTR_EXTRAS, list);
    odin3_log_reset_counts();
    write_ok(design, OUT_PATH);
    TEST_ASSERT_EQUAL_size_t(3, odin3_log_count(ODIN3_LOG_WARN));
    static char text_out[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a\n.names a b\n1 1\n.attr src \"t.v:1\"\n.end\n",
                             slurp(OUT_PATH, text_out));
}

/* The first live cell of type `name` in module, or none. */
static odin3_node_id find_cell(odin3_module *module, const char *name) {
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) &&
            strcmp(odin3_celltype_get(design, odin3_node_type(module, node))->name, name) == 0) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

/* A .subckt of a model whose port is in both lists, with both pins connected to different nets:
 * BLIF can name the formal only once, so the write is refused (and the file removed). */
static void test_subckt_both_pins_of_one_blif_name(void) {
    write_input(".model top\n.inputs a\n.outputs y\n.subckt sub x=a\n.names a y\n1 1\n.end\n"
                ".model sub\n.inputs x\n.outputs x\n.end\n");
    read_into(design, IN_PATH);
    odin3_module *module = module_of(design, 1);
    odin3_node_id cell = find_cell(module, "sub");
    TEST_ASSERT_TRUE(odin3_node_valid(cell));
    odin3_pin_id out = odin3_node_port(module, cell, 1).first;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(module, out, new_net(module, "c")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "both ports"), last_error);
    TEST_ASSERT_NULL(fopen(OUT_PATH, "rb"));
}

/* --- errors -------------------------------------------------------------------------------- */

static void test_unwritable_path_is_io_error(void) {
    read_into(design, FIXTURES[0]);
    const char *path = "odin3_no_such_dir/out.blif";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_blif_write(design, path));
    TEST_ASSERT_EQUAL_STRING_LEN(path, last_error, strlen(path));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "cannot open"));
}

/* A write that fails (no space left on /dev/full) is IO with a located message. */
static void test_failed_write_is_io_error(void) {
    FILE *probe = fopen("/dev/full", "wb");
    if (probe == NULL) {
        TEST_IGNORE_MESSAGE("no /dev/full");
    }
    (void)fclose(probe);
    read_into(design, FIXTURES[1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_blif_write(design, "/dev/full"));
    TEST_ASSERT_EQUAL_STRING_LEN("/dev/full:", last_error, strlen("/dev/full:"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "write failed"));
    probe = fopen("/dev/full", "wb"); /* a device is never removed */
    TEST_ASSERT_NOT_NULL(probe);
    (void)fclose(probe);
}

static void test_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(NULL, OUT_PATH));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, NULL));
}

/* A refused write removes the file it had started, even one that existed before. */
static void test_inout_port_is_refused(void) {
    odin3_module *module = new_module(design, "top");
    new_port(module, "io", ODIN3_DIR_INOUT, 1);
    write_input("old contents\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, IN_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "inout"), last_error);
    FILE *file = fopen(IN_PATH, "rb");
    TEST_ASSERT_NULL(file);
}

/* A registered type with non-default parameters has no BLIF form. */
static void test_non_default_parameters_are_refused(void) {
    odin3_module *module = new_module(design, "top");
    odin3_celltype_id mux = type_named(design, "$mux");
    const odin3_celltype_def *def = odin3_celltype_get(design, mux);
    TEST_ASSERT_EQUAL_UINT32(1, def->n_params);
    odin3_value width = odin3_value_int(def->params[0].dflt.i + 1);
    odin3_node_spec spec = {.type = mux, .params = &width, .n_params = 1};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "$mux"), last_error);
}

/* A registered type with default parameters is written as .subckt and read back as that type. */
static void test_registered_type_as_subckt(void) {
    write_input(".model top\n.inputs a b\n.outputs y\n"
                ".subckt $_AND_ A=a B=b Y=y\n.subckt $_NOT_ A=a\n.end\n");
    round_trip(IN_PATH);
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a b\n.outputs y\n"
                             ".subckt $_AND_ A=a B=b Y=y\n.subckt $_NOT_ A=a\n.end\n",
                             slurp(OUT_PATH, text));
}

/* A Yosys-like parametric type: A A_WIDTH, B B_WIDTH, Y Y_WIDTH (all default 1). */
static const odin3_param_def WPOW_PARAMS[] = {
    {"A_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}},
    {"B_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}},
    {"Y_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}}};
static const odin3_port_def WPOW_PORTS[] = {
    {.name = "A", .dir = ODIN3_DIR_IN, .width_param = "A_WIDTH"},
    {.name = "B", .dir = ODIN3_DIR_IN, .width_param = "B_WIDTH"},
    {.name = "Y", .dir = ODIN3_DIR_OUT, .width_param = "Y_WIDTH"},
};
static const odin3_celltype_def WPOW_DEF = {.name = "o3test_wpow",
                                            .gran = ODIN3_GRAN_HARD,
                                            .ports = WPOW_PORTS,
                                            .n_ports = 3,
                                            .params = WPOW_PARAMS,
                                            .n_params = 3};

/* Registers o3test_wpow once and gives both designs a fresh start that sees it. */
static void register_wpow_once(void) {
    static bool registered;
    if (!registered) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&WPOW_DEF));
        registered = true;
    }
    odin3_design_destroy(design);
    odin3_design_destroy(design2);
    design = odin3_design_create();
    design2 = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_NOT_NULL(design2);
}

/* Review Focus 1: a declared parametric model is written as declared (port order, scalar
 * flags, widths from its parameters) and its instances with the declared spelling. */
static void test_declared_parametric_model_written_as_declared(void) {
    register_wpow_once();
    static const char text[] =
        ".model top\n.inputs a b\n.outputs y\n.subckt o3test_wpow A=a B[0]=b Y[1]=y\n.end\n\n"
        ".model o3test_wpow\n.inputs B[0] B[1] B[2] A\n.outputs Y[0] Y[1]\n.blackbox\n.end\n";
    write_input(text);
    round_trip(IN_PATH);
    static char out[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(text, slurp(OUT_PATH, out));
}

/* An undeclared parametric instance is written when its formals imply its parameters. */
static void test_inferred_parameters_written(void) {
    register_wpow_once();
    static const char text[] = ".model top\n.inputs a\n.outputs y\n"
                               ".subckt o3test_wpow A[1]=a Y[7]=y\n.end\n";
    write_input(text);
    round_trip(IN_PATH);
    static char out[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(text, slurp(OUT_PATH, out));
}

/* Parameters the connected formals do not imply cannot be written. */
static void test_unrecoverable_parameters_are_refused(void) {
    register_wpow_once();
    odin3_module *module = new_module(design, "top");
    const odin3_value params[3] = {odin3_value_int(4), odin3_value_int(1), odin3_value_int(1)};
    odin3_node_spec spec = {
        .type = type_named(design, "o3test_wpow"), .params = params, .n_params = 3};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "o3test_wpow"), last_error);
}

/* Writing design under every allocation failure gives NO_MEMORY, never a crash or a leak, and
 * a write that succeeds gives the reference text. */
static void oom_sweep(void) {
    static char want[TEXT_MAX];
    static char text[TEXT_MAX];
    last_error[0] = '\0';
    write_ok(design, OUT2_PATH);
    (void)slurp(OUT2_PATH, want);
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_SWEEP && !done; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_blif_write(design, OUT_PATH);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            done = true;
            TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
    }
    TEST_ASSERT_TRUE(done);
}

static void test_out_of_memory_sweep(void) {
    read_into(design, ODIN3_BLIF_FIXTURES "/hand_body.blif");
    oom_sweep();
}

/* The same with generated names (wire bits, `$n`, `$p`) and a vector black box. */
static void test_out_of_memory_sweep_generated_names(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = new_port(module, "a", ODIN3_DIR_IN, 2);
    odin3_net_id mid = new_net(module, NULL);
    new_buffer(module, odin3_wire_net(module, in, 0), mid);
    new_buffer(module, mid, (odin3_net_id){0});
    read_into(design2, ODIN3_BLIF_FIXTURES "/hand_ports.blif");
    oom_sweep();
    odin3_design *swap = design;
    design = design2;
    design2 = swap;
    oom_sweep();
}

/* The same with a .subckt whose parameters the writer infers from its connected formals. */
static void test_out_of_memory_sweep_inferred_parameters(void) {
    register_wpow_once();
    write_input(".model top\n.inputs a\n.outputs y\n.subckt o3test_wpow A[1]=a Y[7]=y\n.end\n");
    read_into(design, IN_PATH);
    oom_sweep();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_every_fixture);
    RUN_TEST(test_written_text);
    RUN_TEST(test_zero_input_names);
    RUN_TEST(test_name_in_inputs_and_outputs);
    RUN_TEST(test_subckt_of_model_with_in_and_out_port);
    RUN_TEST(test_port_direction_runs);
    RUN_TEST(test_long_lines_wrap);
    RUN_TEST(test_long_token_stays_whole);
    RUN_TEST(test_generated_names);
    RUN_TEST(test_generated_names_are_unique);
    RUN_TEST(test_wire_bit_name_taken);
    RUN_TEST(test_port_net_with_another_name);
    RUN_TEST(test_two_outputs_share_a_net);
    RUN_TEST(test_input_feeds_output);
    RUN_TEST(test_declared_black_box_written_once);
    RUN_TEST(test_extras_without_a_string_attribute);
    RUN_TEST(test_subckt_both_pins_of_one_blif_name);
    RUN_TEST(test_unwritable_path_is_io_error);
    RUN_TEST(test_failed_write_is_io_error);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_inout_port_is_refused);
    RUN_TEST(test_non_default_parameters_are_refused);
    RUN_TEST(test_registered_type_as_subckt);
    RUN_TEST(test_declared_parametric_model_written_as_declared);
    RUN_TEST(test_inferred_parameters_written);
    RUN_TEST(test_unrecoverable_parameters_are_refused);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_out_of_memory_sweep_generated_names);
    RUN_TEST(test_out_of_memory_sweep_inferred_parameters);
    return UNITY_END();
}
