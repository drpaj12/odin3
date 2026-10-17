/*
 * test_blif_writer.c — unit tests for the BLIF writer (read → write → read round trips), with the
 * plain dialect's constants (3E-3) and $lut as `.names` (3E-5).
 */
#include "backends/blif/vtr.h"
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

#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

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

/* A port declared [msb:lsb] (IR-7, 2D IR change C). */
typedef struct ranged {
    const char *name;
    odin3_dir dir;
    int32_t msb, lsb;
} ranged;

static odin3_wire_id ranged_port(odin3_module *module, ranged port) {
    odin3_port_spec spec = {.name = intern_in(odin3_module_design(module), port.name),
                            .dir = port.dir,
                            .has_range = true,
                            .msb = port.msb,
                            .lsb = port.lsb};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, NULL));
    return odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
}

static void expect_contains(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), text);
}

/* Writes the design, reads it back and writes that again: the two texts are the same. */
static const char *write_stable(char *text) {
    static char again[TEXT_MAX];
    write_ok(design, OUT_PATH);
    read_into(design2, OUT_PATH);
    write_ok(design2, OUT2_PATH);
    TEST_ASSERT_EQUAL_STRING(slurp(OUT_PATH, text), slurp(OUT2_PATH, again));
    return text;
}

/*
 * Port bits are named by their declared index, LSB first, as Yosys 0.55 write_blif names them:
 * [0:3] (ascending) lists a[3] … a[0], [5:2] (descending, offset) d[2] … d[5], and [-1:2]
 * (ascending through a negative bound) n[2] n[1] n[0] n[-1]. Read back (as scalar ports named
 * `a[3]` …) and written again, the text is unchanged.
 */
static void test_ranged_port_bits(void) {
    odin3_module *module = new_module(design, "top");
    odin3_wire_id in = ranged_port(module, (ranged){"a", ODIN3_DIR_IN, 0, 3});
    (void)ranged_port(module, (ranged){"d", ODIN3_DIR_IN, 5, 2});
    odin3_wire_id out = ranged_port(module, (ranged){"n", ODIN3_DIR_OUT, -1, 2});
    for (uint32_t k = 0; k < 4; k++) {
        new_buffer(module, odin3_wire_net(module, in, k), odin3_wire_net(module, out, k));
    }
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs a[3] a[2] a[1] a[0] d[2] d[3] d[4] d[5]\n"
                             ".outputs n[2] n[1] n[0] n[-1]\n"
                             ".names a[3] n[2]\n1 1\n.names a[2] n[1]\n1 1\n"
                             ".names a[1] n[0]\n1 1\n.names a[0] n[-1]\n1 1\n.end\n",
                             write_stable(text));
}

/* An instance's formals carry the child's declared port indices (`.subckt sub i[1]=x[0] …`). */
static void test_ranged_port_formals(void) {
    odin3_module *sub = new_module(design, "sub");
    odin3_wire_id sub_in = ranged_port(sub, (ranged){"i", ODIN3_DIR_IN, 0, 1});
    odin3_wire_id sub_out = ranged_port(sub, (ranged){"o", ODIN3_DIR_OUT, -1, 0});
    for (uint32_t k = 0; k < 2; k++) {
        new_buffer(sub, odin3_wire_net(sub, sub_in, k), odin3_wire_net(sub, sub_out, k));
    }
    odin3_module *top = new_module(design, "top");
    odin3_wire_id xin = new_port(top, "x", ODIN3_DIR_IN, 2);
    odin3_wire_id yout = new_port(top, "y", ODIN3_DIR_OUT, 2);
    odin3_net_id xs[2] = {odin3_wire_net(top, xin, 0), odin3_wire_net(top, xin, 1)};
    odin3_net_id ys[2] = {odin3_wire_net(top, yout, 0), odin3_wire_net(top, yout, 1)};
    const odin3_netvec ports[] = {{xs, 2}, {ys, 2}};
    odin3_node_spec spec = {.type = odin3_module_celltype(sub), .name = intern_in(design, "u")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, NULL));
    static char text[TEXT_MAX];
    const char *have = write_stable(text);
    expect_contains(have, ".subckt sub i[1]=x[0] i[0]=x[1] o[0]=y[0] o[-1]=y[1]\n");
    expect_contains(have, ".model sub\n.inputs i[1] i[0]\n.outputs o[0] o[-1]\n"
                          ".names i[1] o[0]\n1 1\n.names i[0] o[-1]\n1 1\n.end\n");
}

/* A top that is not module 1 is written first, with no leading blank line, and one blank line
 * separates it from the next model (final review M9: the separator was keyed on the module ID). */
static void test_top_first_model_spacing(void) {
    odin3_module *sub = new_module(design, "sub");
    odin3_wire_id sub_in = new_port(sub, "i", ODIN3_DIR_IN, 1);
    odin3_wire_id sub_out = new_port(sub, "o", ODIN3_DIR_OUT, 1);
    new_buffer(sub, odin3_wire_net(sub, sub_in, 0), odin3_wire_net(sub, sub_out, 0));
    odin3_module *top = new_module(design, "top");
    odin3_net_id x_net = odin3_wire_net(top, new_port(top, "x", ODIN3_DIR_IN, 1), 0);
    odin3_net_id y_net = odin3_wire_net(top, new_port(top, "y", ODIN3_DIR_OUT, 1), 0);
    const odin3_netvec ports[] = {{&x_net, 1}, {&y_net, 1}};
    odin3_node_spec spec = {.type = odin3_module_celltype(sub), .name = intern_in(design, "u")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(top, &spec, ports, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, (odin3_module_id){2}));
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(
        ".model top\n.inputs x\n.outputs y\n.subckt sub i=x o=y\n.cname u\n.end\n\n"
        ".model sub\n.inputs i\n.outputs o\n.names i o\n1 1\n.end\n",
        write_stable(text));
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
    static const odin3_port_def ports[] = {{"i", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                                           {"o", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false}};
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

/* A destination whose directory refuses the private temporary (/dev) is IO; the device stays. */
static void test_failed_write_is_io_error(void) {
    FILE *probe = fopen("/dev/full", "wb");
    if (probe == NULL) {
        TEST_IGNORE_MESSAGE("no /dev/full");
    }
    (void)fclose(probe);
    read_into(design, FIXTURES[1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_blif_write(design, "/dev/full"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "/dev/full: cannot open"), last_error);
    probe = fopen("/dev/full", "wb"); /* a device is never removed */
    TEST_ASSERT_NOT_NULL(probe);
    (void)fclose(probe);
}

static const char *const BASELINE = "previous contents\n";

static void put_baseline(const char *path) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(BASELINE), fwrite(BASELINE, 1, strlen(BASELINE), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* path still holds BASELINE, and no "<path>.XXXXXX" temporary is left in the directory. */
static void expect_baseline(const char *path) {
    static char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(BASELINE, slurp(path, text));
    char prefix[MSG_MAX];
    (void)snprintf(prefix, sizeof prefix, "%s.", path);
    DIR *dir = opendir(".");
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        TEST_ASSERT_NULL_MESSAGE(strstr(ent->d_name, prefix), ent->d_name);
    }
    (void)closedir(dir);
}

/* A write that fails part way (file size limit, EFBIG) is IO and leaves an existing file as it
 * was: the writer writes a private temporary and renames it only on success. */
static void test_failed_write_keeps_destination(void) {
    read_into(design, FIXTURES[0]);
    put_baseline(OUT_PATH);
    struct rlimit old;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    TEST_ASSERT_TRUE(prev != SIG_ERR);
    struct rlimit small = {256, old.rlim_max};
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &small));
    odin3_status st = odin3_blif_write(design, OUT_PATH);
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &old));
    TEST_ASSERT_TRUE(signal(SIGXFSZ, prev) != SIG_ERR);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "write failed"), last_error);
    expect_baseline(OUT_PATH);
}

static void test_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(NULL, OUT_PATH));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, NULL));
}

/* A refused write leaves an existing file as it was (the write is atomic). */
static void test_inout_port_is_refused(void) {
    odin3_module *module = new_module(design, "top");
    new_port(module, "io", ODIN3_DIR_INOUT, 1);
    put_baseline(IN_PATH);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, IN_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "inout"), last_error);
    expect_baseline(IN_PATH);
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
 * flags, widths from its parameters) and its instances with the declared spelling and order. */
static void test_declared_parametric_model_written_as_declared(void) {
    register_wpow_once();
    static const char text[] =
        ".model top\n.inputs a b\n.outputs y\n.subckt o3test_wpow B[0]=b A=a Y[1]=y\n.end\n\n"
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

/* The 2D word cells (IR change A, EL-31) take the generic path, as $add does: a .subckt when
 * the connected formals imply the parameters (no decomposition in the writer: that is `lower`'s
 * job, DESIGN D1), else the parameter refusal. */
static void test_2d_word_cells_as_subckt(void) {
    static const char text[] = ".model top\n.inputs a b c\n.outputs y z\n"
                               ".subckt $logic_and A[0]=a A[1]=b B[0]=c Y[0]=y\n"
                               ".subckt $shiftx A[0]=a A[1]=b B[0]=c Y[0]=z\n.end\n";
    write_input(text);
    round_trip(IN_PATH);
    static char out[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(text, slurp(OUT_PATH, out));
    /* both read as the registered word cells (not implicit black boxes), widths implied */
    odin3_module *top = module_of(design, 1);
    static const char *const types[2] = {"$logic_and", "$shiftx"};
    for (uint32_t i = 0; i < 2; i++) {
        odin3_node_id node = {odin3_module_port_count(top) + 1 + i};
        const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(top, node));
        TEST_ASSERT_EQUAL_STRING(types[i], def->name);
        TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_WORD, def->gran);
        static const int64_t want[5] = {0, 0, 2, 1, 1}; /* A_SIGNED B_SIGNED A_W B_W Y_W */
        for (uint32_t k = 0; k < 5; k++) {
            TEST_ASSERT_EQUAL_INT64(want[k], odin3_node_param(top, node, k)->i);
        }
    }
}

static void test_2d_signed_word_cell_is_refused(void) {
    odin3_module *module = new_module(design, "top");
    const odin3_value params[3] = {odin3_value_int(1), odin3_value_int(1), odin3_value_int(1)};
    odin3_node_spec spec = {.type = type_named(design, "$neg"), .params = params, .n_params = 3};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "cannot write a cell of '$neg'"), last_error);
}

/* The eleven word flip-flops and latches of 2E's IR change D (2E spec §13) have no BLIF form:
 * each, at its defaults (so a .subckt would be implied), is refused with the exact message and
 * the destination is not written. */
static void test_ir_change_d_cells_are_refused(void) {
    static const char *const types[] = {"$adffe",   "$sdffe",    "$sdffce", "$aldff",
                                        "$aldffe",  "$dffsr",    "$dffsre", "$dlatch",
                                        "$adlatch", "$dlatchsr", "$sr"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        odin3_design *des = odin3_design_create();
        TEST_ASSERT_NOT_NULL(des);
        odin3_module *module = new_module(des, "top");
        odin3_node_spec spec = {.type = type_named(des, types[i])};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, NULL));
        (void)remove(OUT_PATH);
        last_error[0] = '\0';
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, odin3_blif_write(des, OUT_PATH),
                                      types[i]);
        char want[MSG_MAX];
        (void)snprintf(want, sizeof want,
                       "%s: model 'top': word flip-flops and latches have no BLIF form; cannot "
                       "write a cell of '%s'",
                       OUT_PATH, types[i]);
        TEST_ASSERT_EQUAL_STRING(want, last_error);
        TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(0, remove(OUT_PATH), types[i]); /* never created */
        odin3_design_destroy(des);
    }
}

/* Writing design under every allocation failure gives NO_MEMORY, never a crash or a leak, and
 * a write that succeeds gives the reference text. */
static void oom_sweep(void) {
    static char want[TEXT_MAX];
    static char text[TEXT_MAX];
    last_error[0] = '\0';
    write_ok(design, OUT2_PATH);
    (void)slurp(OUT2_PATH, want);
    put_baseline(OUT_PATH);
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
            expect_baseline(OUT_PATH); /* a failed write keeps the old file */
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

/* --- constants as gnd/vcc/unconn (spec 3E §9.4, plan point (p)) ---------------------------- */

/* A cell of a 1-bit-port type: nets[i] on port i (none: unconnected). */
static odin3_node_id new_cell(odin3_module *module, const char *type, const odin3_net_id *nets,
                              uint32_t n_ports) {
    enum { MAX_PORTS = 3 };
    TEST_ASSERT_TRUE(n_ports <= MAX_PORTS);
    odin3_netvec ports[MAX_PORTS];
    for (uint32_t i = 0; i < n_ports; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    odin3_node_spec spec = {.type = type_named(odin3_module_design(module), type)};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return node;
}

/* A $_CONST*_ cell driving a new unnamed net; that net. */
static odin3_net_id new_const(odin3_module *module, const char *type) {
    odin3_net_id net = new_net(module, NULL);
    new_cell(module, type, &net, 1);
    return net;
}

static odin3_net_id port_net(odin3_module *module, const char *name, odin3_dir dir) {
    return odin3_wire_net(module, new_port(module, name, dir, 1), 0);
}

/* Module m: $_CONST0_, $_CONST1_ and $_CONSTX_ on three $_AND_ inputs, $_CONSTZ_ on output z. */
static void build_const_module(odin3_design *des) {
    odin3_module *module = new_module(des, "m");
    odin3_net_id in_a = port_net(module, "a", ODIN3_DIR_IN);
    odin3_net_id y1 = port_net(module, "y1", ODIN3_DIR_OUT);
    odin3_net_id y2 = port_net(module, "y2", ODIN3_DIR_OUT);
    odin3_net_id out_z = port_net(module, "z", ODIN3_DIR_OUT);
    odin3_net_id zero = new_const(module, "$_CONST0_");
    odin3_net_id one = new_const(module, "$_CONST1_");
    odin3_net_id x_net = new_const(module, "$_CONSTX_");
    new_cell(module, "$_AND_", (odin3_net_id[]){zero, one, y1}, 3);
    new_cell(module, "$_AND_", (odin3_net_id[]){x_net, in_a, y2}, 3);
    new_cell(module, "$_CONSTZ_", &out_z, 1);
}

static const char CONST_TEXT[] = ".model m\n.inputs a\n.outputs y1 y2 z\n"
                                 ".names gnd\n.names vcc\n1\n.names unconn\n"
                                 ".subckt $_AND_ A=gnd B=vcc Y=y1\n"
                                 ".subckt $_AND_ A=unconn B=a Y=y2\n"
                                 ".names unconn z\n1 1\n.end\n";

/* Writes design to OUT_PATH, reads it back into design2 and writes that to OUT2_PATH: both texts
 * are want. */
static void expect_written_twice(const char *want) {
    static char text[TEXT_MAX];
    write_ok(design, OUT_PATH);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
    read_into(design2, OUT_PATH);
    write_ok(design2, OUT2_PATH);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT2_PATH, text));
}

/* The number of live nodes of module whose type is name. */
static uint32_t count_type(odin3_design *des, const odin3_module *module, const char *name) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        count +=
            odin3_node_live(module, node) &&
                    strcmp(odin3_celltype_get(des, odin3_node_type(module, node))->name, name) == 0
                ? 1U
                : 0U;
    }
    return count;
}

/* The generators follow the ports, the gates read them, z gets a buffer from unconn, no
 * `.subckt $_CONST`; read back, the constants are zero-input $sops and the text is the same. */
static void test_plain_const_generators(void) {
    build_const_module(design);
    expect_written_twice(CONST_TEXT);
    odin3_module *back = module_of(design2, 1);
    static const char *const consts[] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, count_type(design2, back, consts[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(4, count_type(design2, back, "$sop")); /* 3 generators, z's buffer */
    static const char *const gens[] = {"gnd", "vcc", "unconn"};
    static const odin3_const values[] = {ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_0};
    for (uint32_t i = 0; i < 3; i++) {
        odin3_net_id net = odin3_module_find_net(back, find_in(design2, gens[i]));
        TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(net), gens[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(values[i], odin3_net_const_value(back, net), gens[i]);
    }
}

/* Each module writes only the generators it reads: m2, on $_CONST1_ alone, only vcc. */
static void test_plain_const_per_module(void) {
    build_const_module(design);
    odin3_module *m2 = new_module(design, "m2");
    odin3_net_id out_o = port_net(m2, "o", ODIN3_DIR_OUT);
    odin3_net_id one = new_const(m2, "$_CONST1_");
    new_cell(m2, "$_AND_", (odin3_net_id[]){one, one, out_o}, 3);
    new_const(m2, "$_CONST0_"); /* read by nothing: no gnd */
    static char want[TEXT_MAX];
    int len =
        snprintf(want, sizeof want, "%s\n%s", CONST_TEXT,
                 ".model m2\n.outputs o\n.names vcc\n1\n.subckt $_AND_ A=vcc B=vcc Y=o\n.end\n");
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    expect_written_twice(want);
}

/* A signal net named vcc in a module that reads vcc is written `$n<ID>`; one named gnd keeps its
 * name (the module reads no gnd). */
static void test_plain_const_name_fallback(void) {
    odin3_module *module = new_module(design, "top");
    odin3_net_id in_a = port_net(module, "a", ODIN3_DIR_IN);
    odin3_net_id out_y = port_net(module, "y", ODIN3_DIR_OUT);
    odin3_net_id out_w = port_net(module, "w", ODIN3_DIR_OUT);
    odin3_net_id vcc = new_net(module, "vcc");
    odin3_net_id gnd = new_net(module, "gnd");
    new_buffer(module, in_a, vcc);
    new_cell(module, "$_AND_", (odin3_net_id[]){vcc, new_const(module, "$_CONST1_"), gnd}, 3);
    new_buffer(module, gnd, out_y);
    new_cell(module, "$_OR_", (odin3_net_id[]){in_a, in_a, out_w}, 3);
    static char want[TEXT_MAX];
    int len = snprintf(want, sizeof want,
                       ".model top\n.inputs a\n.outputs y w\n.names vcc\n1\n"
                       ".names a $n%u\n1 1\n.subckt $_AND_ A=$n%u B=vcc Y=gnd\n.names gnd y\n1 1\n"
                       ".subckt $_OR_ A=a B=a Y=w\n.end\n",
                       (unsigned)vcc.v, (unsigned)vcc.v);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    static char text[TEXT_MAX];
    write_ok(design, OUT_PATH);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
}

/* A zero-input $sop on a net named gnd (value 0) is the gnd generator: no second `.names gnd`. */
static void test_plain_const_sop_is_generator(void) {
    write_input(".model top\n.outputs y\n.names gnd\n.subckt $_AND_ A=gnd B=k Y=y\n.end\n");
    read_into(design, IN_PATH);
    odin3_module *module = module_of(design, 1);
    odin3_net_id k_net = odin3_module_find_net(module, find_in(design, "k"));
    TEST_ASSERT_TRUE(odin3_net_valid(k_net));
    new_cell(module, "$_CONST0_", &k_net, 1);
    expect_written_twice(".model top\n.outputs y\n.names gnd\n"
                         ".subckt $_AND_ A=gnd B=gnd Y=y\n.end\n");
}

/* An output port named vcc on a $_CONST1_ net is the generator; an input port named unconn in a
 * module that reads unconn is refused, and nothing is written. */
static void test_plain_const_port_names(void) {
    odin3_module *module = new_module(design, "top");
    odin3_net_id vcc = port_net(module, "vcc", ODIN3_DIR_OUT);
    odin3_net_id out_y = port_net(module, "y", ODIN3_DIR_OUT);
    new_cell(module, "$_CONST1_", &vcc, 1);
    new_cell(module, "$_NOT_", (odin3_net_id[]){vcc, out_y}, 2);
    expect_written_twice(".model top\n.outputs vcc y\n.names vcc\n1\n"
                         ".subckt $_NOT_ A=vcc Y=y\n.end\n");
    odin3_module *other = new_module(design, "bad");
    odin3_net_id in = port_net(other, "unconn", ODIN3_DIR_IN);
    new_cell(other, "$_AND_", (odin3_net_id[]){in, new_const(other, "$_CONSTX_"), {0}}, 3);
    put_baseline(OUT_PATH);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    char want[MSG_MAX];
    int len = snprintf(want, sizeof want,
                       "%s: model 'bad': a port bit on another net has the name of the constant "
                       "generator 'unconn'",
                       OUT_PATH);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
    expect_baseline(OUT_PATH);
}

/* A zero-input $sop of value 1 (the row `1`) driving out. */
static void new_sop_one(odin3_module *module, odin3_net_id out) {
    static const uint8_t row[] = {'1'};
    odin3_value params[2] = {odin3_value_int(0), {.kind = ODIN3_VAL_COVER}};
    params[1].bits = row;
    params[1].len = 1;
    params[1].cover_inputs = 0;
    odin3_node_spec spec = {
        .type = type_named(odin3_module_design(module), "$sop"), .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{NULL, 0}, {&out, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, NULL));
}

/* Module top: output vcc driven by a constant-1 zero-input $sop (its net renamed to net_name, or
 * unnamed for NULL), and a $_CONST1_ read by $_NOT_ -> y. */
static void build_vcc_port_sop(const char *net_name) {
    odin3_module *module = new_module(design, "top");
    odin3_net_id vcc = port_net(module, "vcc", ODIN3_DIR_OUT);
    odin3_net_id out_y = port_net(module, "y", ODIN3_DIR_OUT);
    uint32_t name = net_name == NULL ? 0 : intern_in(design, net_name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_rename(module, vcc, name));
    new_sop_one(module, vcc);
    new_cell(module, "$_NOT_", (odin3_net_id[]){new_const(module, "$_CONST1_"), out_y}, 2);
}

static const char VCC_PORT_SOP_TEXT[] = ".model top\n.outputs vcc y\n.names vcc\n1\n"
                                        ".subckt $_NOT_ A=vcc Y=y\n.end\n";

/* Review I1: a constant-1 $sop written under the port bit name vcc is the vcc generator, so no
 * second `.names vcc` (one driver per name): its net unnamed (it takes the port bit's name) or
 * named vcc. */
static void test_plain_const_port_sop_is_generator(void) {
    build_vcc_port_sop(NULL);
    expect_written_twice(VCC_PORT_SOP_TEXT);
    odin3_design_destroy(design);
    odin3_design_destroy(design2);
    design = odin3_design_create();
    design2 = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_NOT_NULL(design2);
    build_vcc_port_sop("vcc");
    expect_written_twice(VCC_PORT_SOP_TEXT);
}

/* Review I1: the same $sop on a net named k would be written `.names k` with a buffer to vcc,
 * a second driver beside the generator: refused, nothing written. */
static void test_plain_const_port_named_sop_refused(void) {
    build_vcc_port_sop("k");
    put_baseline(OUT_PATH);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_write(design, OUT_PATH));
    char want[MSG_MAX];
    int len = snprintf(want, sizeof want,
                       "%s: model 'top': a port bit on another net has the name of the constant "
                       "generator 'vcc'",
                       OUT_PATH);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
    expect_baseline(OUT_PATH);
}

/* Review M1: gens_have is per module. m1's own `.names vcc` $sop is its generator; m2 reads
 * $_CONST1_ and must still write its own. */
static void test_plain_const_generator_found_per_module(void) {
    odin3_module *m1 = new_module(design, "m1");
    odin3_net_id out_y = port_net(m1, "y", ODIN3_DIR_OUT);
    odin3_net_id vcc = new_net(m1, "vcc");
    new_sop_one(m1, vcc);
    new_buffer(m1, vcc, out_y);
    odin3_module *m2 = new_module(design, "m2");
    odin3_net_id out_o = port_net(m2, "o", ODIN3_DIR_OUT);
    new_cell(m2, "$_CONST1_", &out_o, 1);
    expect_written_twice(".model m1\n.outputs y\n.names vcc\n1\n.names vcc y\n1 1\n.end\n\n"
                         ".model m2\n.outputs o\n.names vcc\n1\n.names vcc o\n1 1\n.end\n");
}

static void test_out_of_memory_sweep_constants(void) {
    build_const_module(design);
    oom_sweep();
}

/* --- $lut as `.names` with odin3_blif_isop's cover (3E-5, Task 9) ---------------------------- */

enum { LUT_TEST_MAX_K = 3 };

/* A $lut's WIDTH and table (bit i = LUT[i]). */
typedef struct lut_case {
    uint32_t width;
    uint64_t table;
} lut_case;

/* Module top: scalar inputs a, b, … (WIDTH of them), output y, and one $lut of the case on them
 * (A[j] = the j-th input) driving y; its node. */
static odin3_node_id build_lut_top(lut_case lc) {
    static const char *const NAMES[LUT_TEST_MAX_K] = {"a", "b", "c"};
    uint8_t bits[1U << LUT_TEST_MAX_K];
    TEST_ASSERT_TRUE(lc.width >= 1 && lc.width <= LUT_TEST_MAX_K);
    odin3_module *module = new_module(design, "top");
    odin3_net_id ins[LUT_TEST_MAX_K];
    for (uint32_t j = 0; j < lc.width; j++) {
        ins[j] = port_net(module, NAMES[j], ODIN3_DIR_IN);
    }
    odin3_net_id out = port_net(module, "y", ODIN3_DIR_OUT);
    for (uint32_t i = 0; i < 1U << lc.width; i++) {
        bits[i] = (uint8_t)(((lc.table >> i) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    odin3_value params[2] = {odin3_value_int(lc.width), {.kind = ODIN3_VAL_BITS}};
    params[1].bits = bits;
    params[1].len = 1U << lc.width;
    odin3_node_spec spec = {.type = type_named(design, "$lut"), .params = params, .n_params = 2};
    const odin3_netvec ports[] = {{ins, lc.width}, {&out, 1}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return node;
}

/* The read-back $sop on y (design2) computes the LUT's table (3D §13.3 item 1): the same k-input
 * table, or for a constant LUT a zero-input $sop of that value. */
static void expect_read_back_table(lut_case lc) {
    odin3_module *back = module_of(design2, 1);
    TEST_ASSERT_EQUAL_UINT32(1, count_type(design2, back, "$sop"));
    odin3_net_id y_net = odin3_module_find_net(back, find_in(design2, "y"));
    odin3_node_id sop = odin3_pin_node(back, odin3_net_driver(back, y_net));
    uint64_t mask = (1ULL << (1U << lc.width)) - 1U;
    if (lc.table == 0 || lc.table == mask) {
        TEST_ASSERT_EQUAL_UINT32(1, odin3_node_pins(back, sop).count);
        TEST_ASSERT_EQUAL_INT(lc.table == 0 ? ODIN3_CONST_0 : ODIN3_CONST_1,
                              odin3_net_const_value(back, y_net));
        return;
    }
    static uint64_t tt[ODIN3_BLIF_VTR_TT_WORDS];
    TEST_ASSERT_TRUE(odin3_blif_vtr_eval(back, sop, tt));
    TEST_ASSERT_EQUAL_HEX64(lc.table, tt[0]);
}

/* write, read_blif, write: want both times, and the read-back $sop is the LUT's table. */
static void expect_lut_plain(lut_case lc, const char *want) {
    (void)build_lut_top(lc);
    expect_written_twice(want);
    expect_read_back_table(lc);
    odin3_design_destroy(design);
    odin3_design_destroy(design2);
    design = odin3_design_create();
    design2 = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_NOT_NULL(design2);
}

/* Spec 3E §6 (both dialects), plan point (b): AND2 on a b, the majority's ISOP rows, OR2's
 * off-set row (the shorter), a 1-input inverter, and the constants as zero-input `.names`. */
static void test_lut_plain(void) {
    expect_lut_plain((lut_case){2, 0x8},
                     ".model top\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n");
    expect_lut_plain((lut_case){3, 0xE8},
                     ".model top\n.inputs a b c\n.outputs y\n.names a b c y\n-11 1\n1-1 1\n"
                     "11- 1\n.end\n");
    expect_lut_plain((lut_case){2, 0xE},
                     ".model top\n.inputs a b\n.outputs y\n.names a b y\n00 0\n.end\n");
    expect_lut_plain((lut_case){1, 0x1},
                     ".model top\n.inputs a\n.outputs y\n.names a y\n0 1\n.end\n");
    expect_lut_plain((lut_case){2, 0x0}, ".model top\n.inputs a b\n.outputs y\n.names y\n.end\n");
    expect_lut_plain((lut_case){3, 0xFF},
                     ".model top\n.inputs a b c\n.outputs y\n.names y\n1\n.end\n");
}

/* An unconnected A pin is written under its own `$p<pin ID>`, as a .names pin is (never
 * skipped, as a .subckt formal would be). */
static void test_lut_plain_unconnected_pin(void) {
    odin3_node_id lut = build_lut_top((lut_case){2, 0x8});
    odin3_module *module = module_of(design, 1);
    odin3_pin_id pin = {odin3_node_port(module, lut, 0).first.v + 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, pin));
    static char text[TEXT_MAX];
    char want[MSG_MAX];
    (void)snprintf(want, sizeof want,
                   ".model top\n.inputs a b\n.outputs y\n.names a $p%u y\n11 1\n.end\n",
                   (unsigned)pin.v);
    write_ok(design, OUT_PATH);
    TEST_ASSERT_EQUAL_STRING(want, slurp(OUT_PATH, text));
}

/* The plain write of a LUT under every allocation failure (the ISOP's arena and cover). */
static void test_out_of_memory_sweep_lut(void) {
    (void)build_lut_top((lut_case){3, 0xE8});
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
    RUN_TEST(test_ranged_port_bits);
    RUN_TEST(test_ranged_port_formals);
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
    RUN_TEST(test_failed_write_keeps_destination);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_inout_port_is_refused);
    RUN_TEST(test_non_default_parameters_are_refused);
    RUN_TEST(test_registered_type_as_subckt);
    RUN_TEST(test_declared_parametric_model_written_as_declared);
    RUN_TEST(test_inferred_parameters_written);
    RUN_TEST(test_unrecoverable_parameters_are_refused);
    RUN_TEST(test_2d_word_cells_as_subckt);
    RUN_TEST(test_2d_signed_word_cell_is_refused);
    RUN_TEST(test_ir_change_d_cells_are_refused);
    RUN_TEST(test_top_first_model_spacing);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_out_of_memory_sweep_generated_names);
    RUN_TEST(test_out_of_memory_sweep_inferred_parameters);
    RUN_TEST(test_plain_const_generators);
    RUN_TEST(test_plain_const_per_module);
    RUN_TEST(test_plain_const_name_fallback);
    RUN_TEST(test_plain_const_sop_is_generator);
    RUN_TEST(test_plain_const_port_names);
    RUN_TEST(test_plain_const_port_sop_is_generator);
    RUN_TEST(test_plain_const_port_named_sop_refused);
    RUN_TEST(test_plain_const_generator_found_per_module);
    RUN_TEST(test_lut_plain);
    RUN_TEST(test_lut_plain_unconnected_pin);
    RUN_TEST(test_out_of_memory_sweep_lut);
    RUN_TEST(test_out_of_memory_sweep_constants);
    return UNITY_END();
}
