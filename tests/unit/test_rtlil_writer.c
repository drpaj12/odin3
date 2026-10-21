/*
 * test_rtlil_writer.c — write_rtlil (2D spec §13, EL-48; plan Task 5): the exact text of the plan's
 * fixture, wires and sigspecs, every sync type, $mem parameters, nets that are no wire's bit
 * (review R4), unconnected pins, instances and names, refusals, the atomic write, determinism and
 * out of memory, the design's top, and one cell of every registered `$` type the writer does not
 * refuse (review M5). Each design is also written to ODIN3_RTLIL_OUT/<name>.il for the Yosys 0.55
 * round trip (CTest rtlil_yosys_roundtrip).
 */
#include "backends/rtlil/writer.h"
#include "ffcells_fixture.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_RTLIL_OUT
#error "ODIN3_RTLIL_OUT must name the directory the .il fixtures are written to"
#endif

enum { LOG_CAP = 4096, TEXT_MAX = 1 << 16, PATH_MAX_LEN = 512, OOM_LIMIT = 4000, ARENA = 1024 };
/* EVERY_CELL_MIN: the `$` types 2D-1 (46) and 2E-1 (the eleven flip-flops and latches of IR change
 * D) register that test_every_cell writes (review probe A). */
enum { CELL_PORTS_MAX = 16, CELL_PINS_MAX = 64, NAME_MAX_LEN = 64, EVERY_CELL_MIN = 57 };

static odin3_design *design;
static odin3_prov_id prov;
static odin3_strbuf text;
static char last_error[LOG_CAP];
static char file_buf[TEXT_MAX];

static void error_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

/* A SOURCE record at <file>:1 in a fresh pass run "test". */
static odin3_prov_id source_prov(const char *file_name) {
    odin3_pass_ctx run = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("test"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, name, &run));
    uint32_t file = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(design, odin3_bytes_cstr(file_name), &file));
    odin3_srcloc loc = {file, 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run, &origin, &out));
    return out;
}

void setUp(void) {
    last_error[0] = '\0';
    odin3_log_set_sink(error_sink, NULL);
    odin3_strbuf_init(&text);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    prov = source_prov("t.v");
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_strbuf_free(&text);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- building ------------------------------------------------------------------------------ */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &id));
    return odin3_module_get(design, id);
}

static odin3_celltype_id type_named(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

/* A port [msb:lsb] (sign is_signed); returns its wire. */
static odin3_wire_id port(odin3_module *mod, const char *name, odin3_dir dir, int32_t msb,
                          int32_t lsb) {
    odin3_port_spec spec = {intern(name), dir, 0, false, prov, true, msb, lsb, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    return odin3_module_port_wire(mod, odin3_module_port_count(mod) - 1);
}

static odin3_wire_id signed_port(odin3_module *mod, const char *name, odin3_dir dir, int32_t msb,
                                 int32_t lsb) {
    odin3_port_spec spec = {intern(name), dir, 0, false, prov, true, msb, lsb, true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    return odin3_module_port_wire(mod, odin3_module_port_count(mod) - 1);
}

/* A wire [msb:lsb] on fresh nets (nets NULL) or on the given nets. */
static odin3_wire_id wire_on(odin3_module *mod, const char *name, int32_t msb, int32_t lsb,
                             const odin3_net_id *nets) {
    odin3_wire_spec spec = {intern(name), msb, lsb, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, nets, &wire));
    return wire;
}

static odin3_net_id bit_of(odin3_module *mod, odin3_wire_id wire, uint32_t bit) {
    return odin3_wire_net(mod, wire, bit);
}

/* The net driven by a new constant node of type tname (one per value, IR-4). */
static odin3_net_id const_net(odin3_module *mod, const char *tname) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, prov, &net));
    odin3_node_spec spec = {type_named(tname), 0, prov, NULL, 0};
    odin3_netvec ports[1] = {{&net, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL));
    return net;
}

/* The four shared constant nets. */
typedef struct consts {
    odin3_net_id c0, c1, cx, cz;
} consts;

static consts four_consts(odin3_module *mod) {
    consts out = {const_net(mod, "$_CONST0_"), const_net(mod, "$_CONST1_"),
                  const_net(mod, "$_CONSTX_"), const_net(mod, "$_CONSTZ_")};
    return out;
}

/* A node of type tname named name (NULL: unnamed) with the given parameters and ports. */
typedef struct cell_req {
    const char *type;
    const char *name;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
} cell_req;

static odin3_node_id cell(odin3_module *mod, cell_req req) {
    odin3_node_spec spec = {type_named(req.type), req.name != NULL ? intern(req.name) : 0, prov,
                            req.params, req.n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, req.ports, &node));
    return node;
}

static odin3_value str_value(const char *str) {
    odin3_value val = {.kind = ODIN3_VAL_STRING, .str = intern(str)};
    return val;
}

static odin3_value bits_value(const uint8_t *bits, uint32_t len) {
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = bits, .len = len};
    return val;
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

/* --- output -------------------------------------------------------------------------------- */

/* The design's text (checked FULL first); the buffer is reset on each call. */
static const char *rtlil(void) {
    check_full();
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_rtlil_text(design, &text), last_error);
    return text.data;
}

static const char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(file_buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file_buf[len] = '\0';
    return file_buf;
}

/* Writes the design to ODIN3_RTLIL_OUT/<name>.il (the Yosys round trip's input) and checks the
 * file holds exactly odin3_rtlil_text's text. */
static void save_fixture(const char *name) {
    char path[PATH_MAX_LEN];
    (void)snprintf(path, sizeof path, "%s/%s.il", ODIN3_RTLIL_OUT, name);
    const char *expect = rtlil();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_rtlil_write(design, path), last_error);
    TEST_ASSERT_EQUAL_STRING(expect, slurp(path));
}

static void expect_line(const char *line) {
    char want[LOG_CAP];
    int len = snprintf(want, sizeof want, "\n%s\n", line);
    TEST_ASSERT_TRUE_MESSAGE(len > 0 && (size_t)len < sizeof want, line);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text.data, want), line);
}

/* How many times needle occurs in the text. */
static int count_lines(const char *needle) {
    int count = 0;
    for (const char *at = strstr(text.data, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* --- the plan's fixture -------------------------------------------------------------------- */

/*
 * Module m: input [3:0] a (port 1), output signed [0:1] y (port 2), wire b [3:0] merged onto a,
 * the four shared constants, and last an unnamed $add (A = a, B = 4'b0011, Y_WIDTH 2 on y).
 * Returns the $add's ID.
 */
static uint32_t build_fixture(void) {
    odin3_module *mod = new_module("m");
    odin3_wire_id a_wire = port(mod, "a", ODIN3_DIR_IN, 3, 0);
    odin3_wire_id y_wire = signed_port(mod, "y", ODIN3_DIR_OUT, 0, 1);
    odin3_wire_id b_wire = wire_on(mod, "b", 3, 0, NULL);
    odin3_net_id a_nets[4];
    odin3_net_id y_nets[2];
    for (uint32_t k = 0; k < 4; k++) {
        a_nets[k] = bit_of(mod, a_wire, k);
        odin3_net_pair pair = {a_nets[k], bit_of(mod, b_wire, k)};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(mod, pair));
    }
    y_nets[0] = bit_of(mod, y_wire, 0);
    y_nets[1] = bit_of(mod, y_wire, 1);
    consts cs = four_consts(mod);
    odin3_net_id b_nets[4] = {cs.c1, cs.c1, cs.c0, cs.c0};
    odin3_value params[5] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(4),
                             odin3_value_int(4), odin3_value_int(2)};
    odin3_netvec ports[3] = {{a_nets, 4}, {b_nets, 4}, {y_nets, 2}};
    return cell(mod, (cell_req){"$add", NULL, params, 5, ports}).v;
}

static void test_fixture_text(void) {
    uint32_t k = build_fixture();
    char want[TEXT_MAX];
    (void)snprintf(want, sizeof want,
                   "module \\m\n"
                   "  wire width 4 input 1 \\a\n"
                   "  wire width 2 upto output 2 signed \\y\n"
                   "  wire width 4 \\b\n"
                   "  cell $add $c%u\n"
                   "    parameter \\A_SIGNED 0\n"
                   "    parameter \\B_SIGNED 0\n"
                   "    parameter \\A_WIDTH 4\n"
                   "    parameter \\B_WIDTH 4\n"
                   "    parameter \\Y_WIDTH 2\n"
                   "    connect \\A \\a\n"
                   "    connect \\B 4'0011\n"
                   "    connect \\Y \\y\n"
                   "  end\n"
                   "  connect \\b \\a\n"
                   "end\n",
                   k);
    TEST_ASSERT_EQUAL_STRING(want, rtlil());
    save_fixture("fixture");
}

/* Two writes, and the text and the file, give identical bytes. */
static void test_deterministic(void) {
    (void)build_fixture();
    char first[TEXT_MAX];
    (void)snprintf(first, sizeof first, "%s", rtlil());
    TEST_ASSERT_EQUAL_STRING(first, rtlil());
    char path[PATH_MAX_LEN];
    (void)snprintf(path, sizeof path, "%s/determinism.il", ODIN3_RTLIL_OUT);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_write(design, path));
    TEST_ASSERT_EQUAL_STRING(first, slurp(path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_write(design, path));
    TEST_ASSERT_EQUAL_STRING(first, slurp(path));
}

/* --- wires and sigspecs -------------------------------------------------------------------- */

static void test_wires_and_sigspecs(void) {
    odin3_module *mod = new_module("s");
    odin3_wire_id a_wire = port(mod, "a", ODIN3_DIR_IN, 3, 0);
    (void)wire_on(mod, "w", 8, 1, NULL);
    (void)wire_on(mod, "dn", -2, 1, NULL);
    consts cs = four_consts(mod);
    odin3_net_id v_nets[4] = {cs.c1, cs.cx, bit_of(mod, a_wire, 2), bit_of(mod, a_wire, 3)};
    (void)wire_on(mod, "v", 3, 0, v_nets);
    (void)rtlil();
    expect_line("  wire width 8 offset 1 \\w");
    expect_line("  wire width 4 upto offset -2 \\dn");
    expect_line("  wire width 4 \\v");
    expect_line("  connect \\v { \\a [3:2] 2'x1 }");
    save_fixture("sigspecs");
}

/* A wire bit folded into a constant net is connected to the constant (2F §20.1, M11). */
static void test_constant_alias(void) {
    odin3_module *mod = new_module("c");
    odin3_wire_id y_wire = port(mod, "y", ODIN3_DIR_OUT, 1, 0);
    consts cs = four_consts(mod);
    odin3_net_pair pair = {cs.c0, bit_of(mod, y_wire, 1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(mod, pair));
    odin3_net_id t_nets[1] = {cs.c1};
    (void)wire_on(mod, "t", 0, 0, t_nets);
    (void)rtlil();
    expect_line("  connect \\y [1] 1'0");
    expect_line("  connect \\t 1'1");
    TEST_ASSERT_NULL(strstr(text.data, "$_CONST"));
    save_fixture("constants");
}

/* Module and wire attributes come before their lines. */
static void test_attributes(void) {
    odin3_module *mod = new_module("top");
    odin3_wire_id x_wire = port(mod, "x", ODIN3_DIR_IN, 0, 0);
    odin3_value one = odin3_value_int(1);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, 1}, intern("top"), &one));
    static const uint8_t k_zero[1] = {0};
    odin3_value init = bits_value(k_zero, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_WIRE, x_wire.v},
                                                   intern("init"), &init));
    static const char *const k_want = "attribute \\top 1\n"
                                      "module \\top\n"
                                      "  attribute \\init 1'0\n"
                                      "  wire input 1 \\x\n"
                                      "end\n";
    TEST_ASSERT_EQUAL_STRING(k_want, rtlil());
    /* As the design's top it keeps its own top attribute, not a second one (review M4). */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, (odin3_module_id){1}));
    TEST_ASSERT_EQUAL_STRING(k_want, rtlil());
}

/* --- processes ----------------------------------------------------------------------------- */

/*
 * One process with every sync type over ports c, d (inputs) and q0..q4 (outputs): posedge c
 * updates q0, q2, q3; negedge c q1; low c q2 <- 0; high c q3 <- 1; always q4. I = [c, d, 0, 1],
 * O = [q0..q4]. Returns the node.
 */
static odin3_node_id build_sync_process(odin3_module *mod) {
    odin3_net_id in[4] = {bit_of(mod, port(mod, "c", ODIN3_DIR_IN, 0, 0), 0),
                          bit_of(mod, port(mod, "d", ODIN3_DIR_IN, 0, 0), 0)};
    odin3_net_id out[5];
    static const char *const k_q[5] = {"q0", "q1", "q2", "q3", "q4"};
    for (uint32_t k = 0; k < 5; k++) {
        out[k] = bit_of(mod, port(mod, k_q[k], ODIN3_DIR_OUT, 0, 0), 0);
    }
    consts cs = four_consts(mod);
    in[2] = cs.c0;
    in[3] = cs.c1;
    const odin3_pbit pbits[] = {ODIN3_PBIT_OUT | 0, ODIN3_PBIT_IN | 1,  ODIN3_PBIT_OUT | 2,
                                ODIN3_PBIT_IN | 1,  ODIN3_PBIT_OUT | 3, ODIN3_PBIT_IN | 1,
                                ODIN3_PBIT_OUT | 1, ODIN3_PBIT_IN | 1,  ODIN3_PBIT_OUT | 2,
                                ODIN3_PBIT_IN | 2,  ODIN3_PBIT_OUT | 3, ODIN3_PBIT_IN | 3,
                                ODIN3_PBIT_OUT | 4, ODIN3_PBIT_IN | 1};
    odin3_proc_action actions[7];
    for (uint32_t k = 0; k < 7; k++) {
        actions[k] = (odin3_proc_action){{2 * k, 1}, {2 * k + 1, 1}, {0}};
    }
    const odin3_proc_case root = {0, 0, 0, 0, 0, 0, {0}};
    const odin3_proc_sync syncs[5] = {{ODIN3_SYNC_POSEDGE, ODIN3_PBIT_IN | 0, 0, 3},
                                      {ODIN3_SYNC_NEGEDGE, ODIN3_PBIT_IN | 0, 3, 1},
                                      {ODIN3_SYNC_LEVEL0, ODIN3_PBIT_IN | 0, 4, 1},
                                      {ODIN3_SYNC_LEVEL1, ODIN3_PBIT_IN | 0, 5, 1},
                                      {ODIN3_SYNC_ALWAYS, 0, 6, 1}};
    odin3_proc_body body = {pbits, 14, NULL, 0, actions, 7, &root, 1, NULL, 0, syncs, 5};
    odin3_arena *arena = odin3_arena_create(ARENA);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[3] = {odin3_value_int(4), odin3_value_int(5), {0}};
    odin3_status st = odin3_proc_body_pack(arena, &body, &params[ODIN3_PROC_P_BODY]);
    odin3_netvec ports[2] = {{in, 4}, {out, 5}};
    odin3_node_spec spec = {type_named("$proc"), 0, prov, params, 3};
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(mod, &spec, ports, &node);
    }
    odin3_arena_destroy(arena);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return node;
}

static void test_every_sync_type(void) {
    odin3_module *mod = new_module("p");
    odin3_node_id node = build_sync_process(mod);
    odin3_value src = str_value("t.v:3");
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern("src"), &src));
    char want[TEXT_MAX];
    (void)snprintf(want, sizeof want,
                   "\n  attribute \\src \"t.v:3\"\n"
                   "  process $proc$t.v:1$%u\n"
                   "    sync posedge \\c\n"
                   "      update \\q0 \\d\n"
                   "      update \\q2 \\d\n"
                   "      update \\q3 \\d\n"
                   "    sync negedge \\c\n"
                   "      update \\q1 \\d\n"
                   "    sync low \\c\n"
                   "      update \\q2 1'0\n"
                   "    sync high \\c\n"
                   "      update \\q3 1'1\n"
                   "    sync always\n"
                   "      update \\q4 \\d\n"
                   "  end\n"
                   "end\n",
                   node.v);
    const char *got = rtlil();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(got, want), got);
    TEST_ASSERT_NULL(strstr(got, "cell $proc"));
    save_fixture("syncs");
}

/* Processes come after the cells. */
static void test_process_after_cells(void) {
    odin3_module *mod = new_module("p");
    (void)build_sync_process(mod);
    odin3_wire_id n_wire = wire_on(mod, "n", 0, 0, NULL);
    odin3_net_id d_net = bit_of(mod, odin3_module_port_wire(mod, 1), 0);
    odin3_net_id n_net = bit_of(mod, n_wire, 0);
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(1)};
    odin3_netvec ports[2] = {{&d_net, 1}, {&n_net, 1}};
    (void)cell(mod, (cell_req){"$not", "inv", params, 3, ports});
    const char *got = rtlil();
    const char *at_cell = strstr(got, "  cell $not \\inv\n");
    const char *at_proc = strstr(got, "  process ");
    TEST_ASSERT_NOT_NULL(at_cell);
    TEST_ASSERT_NOT_NULL(at_proc);
    TEST_ASSERT_TRUE(at_cell < at_proc);
}

/* A process name never holds whitespace: a file name with a blank falls back to $proc$<ID>. */
static void test_process_name_with_blank_path(void) {
    odin3_module *mod = new_module("p");
    prov = source_prov("my dir/t.v");
    odin3_node_id node = build_sync_process(mod);
    (void)rtlil();
    char line[LOG_CAP];
    (void)snprintf(line, sizeof line, "  process $proc$%u", node.v);
    expect_line(line);
    TEST_ASSERT_NULL(strstr(text.data, "my dir"));
}

/*
 * A process with a full_case/parallel_case switch on s[1:0], a "2'-0" pattern, a default case and
 * a temporary on a net of its own ($n<ID>): root t <- b; case 2'-0: t <- a; sync always y <- t.
 * I = [s0, s1, a, b, 0], O = [y, t].
 */
static void test_switch_process(void) {
    odin3_module *mod = new_module("sw");
    odin3_wire_id s_wire = port(mod, "s", ODIN3_DIR_IN, 1, 0);
    odin3_net_id a_net = bit_of(mod, port(mod, "a", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id b_net = bit_of(mod, port(mod, "b", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id y_net = bit_of(mod, port(mod, "y", ODIN3_DIR_OUT, 0, 0), 0);
    odin3_net_id t_net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, prov, &t_net));
    consts cs = four_consts(mod);
    odin3_net_id in[5] = {bit_of(mod, s_wire, 0), bit_of(mod, s_wire, 1), a_net, b_net, cs.c0};
    odin3_net_id out[2] = {y_net, t_net};
    const odin3_pbit pbits[] = {ODIN3_PBIT_OUT | 1, ODIN3_PBIT_IN | 3, ODIN3_PBIT_IN | 0,
                                ODIN3_PBIT_IN | 1,  ODIN3_PBIT_IN | 4, ODIN3_PBIT_ANY,
                                ODIN3_PBIT_OUT | 1, ODIN3_PBIT_IN | 2, ODIN3_PBIT_OUT | 0,
                                ODIN3_PBIT_OUT | 1};
    const odin3_pspan compares[1] = {{4, 2}};
    const odin3_proc_action actions[3] = {
        {{0, 1}, {1, 1}, {0}}, {{6, 1}, {7, 1}, {0}}, {{8, 1}, {9, 1}, {0}}};
    const odin3_proc_case cases[3] = {
        {0, 0, 0, 1, 0, 1, {0}}, {0, 1, 1, 1, 1, 0, {0}}, {1, 0, 2, 0, 1, 0, {0}}};
    const odin3_proc_switch sw = {
        {2, 2}, 1, 2, ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE, {0}};
    const odin3_proc_sync sync = {ODIN3_SYNC_ALWAYS, 0, 2, 1};
    odin3_proc_body body = {pbits, 10, compares, 1, actions, 3, cases, 3, &sw, 1, &sync, 1};
    odin3_arena *arena = odin3_arena_create(ARENA);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_value params[3] = {odin3_value_int(5), odin3_value_int(2), {0}};
    odin3_status st = odin3_proc_body_pack(arena, &body, &params[ODIN3_PROC_P_BODY]);
    odin3_netvec ports[2] = {{in, 5}, {out, 2}};
    odin3_node_spec spec = {type_named("$proc"), 0, prov, params, 3};
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(mod, &spec, ports, &node);
    }
    odin3_arena_destroy(arena);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    char want[TEXT_MAX];
    (void)snprintf(want, sizeof want,
                   "  wire $n%u\n"
                   "  process $proc$t.v:1$%u\n"
                   "    assign $n%u \\b\n"
                   "    attribute \\full_case 1\n"
                   "    attribute \\parallel_case 1\n"
                   "    switch \\s\n"
                   "      case 2'-0\n"
                   "        assign $n%u \\a\n"
                   "      case\n"
                   "    end\n"
                   "    sync always\n"
                   "      update \\y $n%u\n"
                   "  end\n"
                   "end\n",
                   t_net.v, node.v, t_net.v, t_net.v, t_net.v);
    const char *got = rtlil();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(got, want), got);
    save_fixture("switch");
}

/* --- $mem ---------------------------------------------------------------------------------- */

static const uint8_t k_bit0[1] = {ODIN3_BIT_0};
static const uint8_t k_bit1[1] = {ODIN3_BIT_1};

/* A $mem: 2 words of 8 bits from index 1, one read port and wr write ports. */
static void mem_params(odin3_value *params, const char *memid, uint32_t wr, const uint8_t *init) {
    params[0] = str_value(memid);
    params[1] = odin3_value_int(2);
    params[2] = odin3_value_int(1);
    params[3] = odin3_value_int(1);
    params[4] = odin3_value_int(8);
    params[5] = odin3_value_int(1);
    params[6] = odin3_value_int(wr);
    params[7] = bits_value(k_bit0, 1);
    params[8] = bits_value(k_bit1, 1);
    params[9] = bits_value(k_bit0, 1);
    params[10] = bits_value(k_bit1, wr);
    params[11] = bits_value(k_bit1, wr);
    params[12] = bits_value(init, 16);
}

/* Read port nets of a $mem: clk, en, addr, data. */
typedef struct mem_nets {
    odin3_net_id clk, en, addr, data[8], wen[8], waddr, wdata[8];
} mem_nets;

static void test_mem_parameters(void) {
    odin3_module *mod = new_module("mm");
    mem_nets nets;
    nets.clk = bit_of(mod, port(mod, "clk", ODIN3_DIR_IN, 0, 0), 0);
    nets.addr = bit_of(mod, port(mod, "ra", ODIN3_DIR_IN, 0, 0), 0);
    nets.waddr = bit_of(mod, port(mod, "wa", ODIN3_DIR_IN, 0, 0), 0);
    odin3_wire_id wd = port(mod, "wd", ODIN3_DIR_IN, 7, 0);
    odin3_wire_id rd = port(mod, "rd", ODIN3_DIR_OUT, 7, 0);
    odin3_wire_id rom = port(mod, "rom", ODIN3_DIR_OUT, 7, 0);
    consts cs = four_consts(mod);
    nets.en = cs.c1;
    for (uint32_t k = 0; k < 8; k++) {
        nets.data[k] = bit_of(mod, rd, k);
        nets.wen[k] = cs.c1;
        nets.wdata[k] = bit_of(mod, wd, k);
    }
    uint8_t xs[16];
    memset(xs, ODIN3_BIT_X, sizeof xs);
    odin3_value params[13];
    mem_params(params, "m", 1, xs);
    odin3_netvec ports[8] = {{&nets.clk, 1}, {&nets.en, 1}, {&nets.addr, 1},  {nets.data, 8},
                             {&nets.clk, 1}, {nets.wen, 8}, {&nets.waddr, 1}, {nets.wdata, 8}};
    (void)cell(mod, (cell_req){"$mem", NULL, params, 13, ports});
    uint8_t zeros[16] = {0};
    mem_params(params, "$r", 0, zeros);
    odin3_net_id rom_data[8];
    for (uint32_t k = 0; k < 8; k++) {
        rom_data[k] = bit_of(mod, rom, k);
    }
    odin3_netvec rom_ports[8] = {{&cs.cx, 1}, {&nets.en, 1}, {&nets.addr, 1}, {rom_data, 8},
                                 {&cs.c0, 0}, {&cs.c0, 0},   {&cs.c0, 0},     {&cs.c0, 0}};
    (void)cell(mod, (cell_req){"$mem", NULL, params, 13, rom_ports});
    (void)rtlil();
    expect_line("    parameter \\MEMID \"\\\\m\"");
    expect_line("    parameter \\OFFSET 1");
    expect_line("    parameter \\INIT 16'xxxxxxxxxxxxxxxx");
    /* the ROM: MEMID already an RTLIL ID; 0-bit write-port flags as Yosys's 1 bit */
    expect_line("    parameter \\MEMID \"$r\"");
    expect_line("    parameter \\WR_PORTS 0\n"
                "    parameter \\RD_CLK_ENABLE 1'0\n"
                "    parameter \\RD_CLK_POLARITY 1'1\n"
                "    parameter \\RD_TRANSPARENT 1'0\n"
                "    parameter \\WR_CLK_ENABLE 1'0\n"
                "    parameter \\WR_CLK_POLARITY 1'0\n"
                "    parameter \\INIT 16'0000000000000000");
    expect_line("    connect \\WR_CLK { }");
    save_fixture("mem");
}

/* --- nets that are no wire's bit (review R4, Task 3 review N7) ----------------------------- */

static void test_nets_without_wires(void) {
    odin3_module *mod = new_module("n");
    odin3_net_id a_net = bit_of(mod, port(mod, "a", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id y_net = bit_of(mod, port(mod, "y", ODIN3_DIR_OUT, 0, 0), 0);
    odin3_net_id mid = {0};
    odin3_net_id anon = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern("mid"), prov, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, prov, &anon));
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(1)};
    odin3_netvec p1[2] = {{&a_net, 1}, {&mid, 1}};
    odin3_netvec p2[2] = {{&mid, 1}, {&anon, 1}};
    odin3_netvec p3[2] = {{&anon, 1}, {&y_net, 1}};
    (void)cell(mod, (cell_req){"$not", "n1", params, 3, p1});
    (void)cell(mod, (cell_req){"$not", "n2", params, 3, p2});
    (void)cell(mod, (cell_req){"$not", "n3", params, 3, p3});
    (void)rtlil();
    char line[LOG_CAP];
    expect_line("  wire \\mid");
    (void)snprintf(line, sizeof line, "  wire $n%u", anon.v);
    expect_line(line);
    expect_line("    connect \\Y \\mid");
    (void)snprintf(line, sizeof line, "    connect \\A $n%u", anon.v);
    expect_line(line);
    save_fixture("nets");
}

/* --- unconnected pins ---------------------------------------------------------------------- */

static void test_unconnected_pins(void) {
    odin3_module *mod = new_module("u");
    odin3_net_id a_net = bit_of(mod, port(mod, "a", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id y_net = bit_of(mod, port(mod, "y", ODIN3_DIR_OUT, 0, 0), 0);
    consts cs = four_consts(mod);
    odin3_net_id none = {0};
    odin3_net_id an[3] = {cs.c1, a_net, none};
    odin3_net_id bn[2] = {none, none};
    odin3_net_id yn[2] = {y_net, none};
    odin3_value params[5] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(3),
                             odin3_value_int(2), odin3_value_int(2)};
    odin3_netvec ports[3] = {{an, 3}, {bn, 2}, {yn, 2}};
    odin3_node_id node = cell(mod, (cell_req){"$add", "add", params, 5, ports});
    uint32_t first = odin3_node_pins(mod, node).first.v; /* A0 A1 A2 B0 B1 Y0 Y1 */
    (void)rtlil();
    char line[LOG_CAP];
    (void)snprintf(line, sizeof line, "  wire $p%u\n  wire width 2 $p%u\n  wire $p%u", first + 2,
                   first + 3, first + 6);
    expect_line(line);
    (void)snprintf(line, sizeof line, "    connect \\A { $p%u \\a 1'1 }", first + 2);
    expect_line(line);
    (void)snprintf(line, sizeof line, "    connect \\B $p%u", first + 3);
    expect_line(line);
    (void)snprintf(line, sizeof line, "    connect \\Y { $p%u \\y }", first + 6);
    expect_line(line);
    save_fixture("pins");
}

/* --- instances and names ------------------------------------------------------------------- */

static void test_instances_and_names(void) {
    odin3_module *sub = new_module("sub");
    (void)port(sub, "i", ODIN3_DIR_IN, 1, 0);
    odin3_module *top = new_module("top");
    odin3_wire_id x_wire = port(top, "x", ODIN3_DIR_IN, 1, 0);
    odin3_net_id x_nets[2] = {bit_of(top, x_wire, 0), bit_of(top, x_wire, 1)};
    odin3_netvec ports[1] = {{x_nets, 2}};
    (void)cell(top, (cell_req){"sub", "u1", NULL, 0, ports});
    odin3_node_id clash = cell(top, (cell_req){"sub", "x", NULL, 0, ports});
    (void)port(top, "io", ODIN3_DIR_INOUT, 0, 0);
    (void)rtlil();
    expect_line("  wire width 2 input 1 \\x\n  wire inout 2 \\io");
    TEST_ASSERT_EQUAL_STRING_LEN("module \\sub\n  wire width 2 input 1 \\i\nend\nmodule \\top\n",
                                 text.data,
                                 strlen("module \\sub\n  wire width 2 input 1 \\i\n"
                                        "end\nmodule \\top\n"));
    expect_line("  cell \\sub \\u1\n    connect \\i \\x\n  end");
    char line[LOG_CAP];
    (void)snprintf(line, sizeof line, "  cell \\sub $c%u", clash.v); /* \x is the wire's */
    expect_line(line);
    save_fixture("instances");
}

/* A module named like an RTLIL ID ($m, \q): its instances' type is spelled as its header. */
static void test_module_names_like_ids(void) {
    odin3_module *dollar = new_module("$m");
    (void)port(dollar, "i", ODIN3_DIR_IN, 0, 0);
    odin3_module *slash = new_module("\\q");
    (void)port(slash, "i", ODIN3_DIR_IN, 0, 0);
    odin3_module *top = new_module("top");
    odin3_net_id x_net = bit_of(top, port(top, "x", ODIN3_DIR_IN, 0, 0), 0);
    odin3_netvec ports[1] = {{&x_net, 1}};
    (void)cell(top, (cell_req){"$m", "u1", NULL, 0, ports});
    (void)cell(top, (cell_req){"\\q", "u2", NULL, 0, ports});
    (void)rtlil();
    TEST_ASSERT_EQUAL_STRING_LEN("module \\$m\n", text.data, strlen("module \\$m\n"));
    expect_line("module \\\\q");
    expect_line("  cell \\$m \\u1");
    expect_line("  cell \\\\q \\u2");
    save_fixture("modnames");
}

/* --- black-box stubs (2D spec §13 point 7, review I1; 2E Task 3) ------------------------- */

static const odin3_port_def k_bbp_ports[] = {
    {"i", ODIN3_DIR_IN, false, 0, "W", NULL, NULL, false},
    {"o", ODIN3_DIR_OUT, false, 0, "W", NULL, NULL, false},
};
static const odin3_port_def k_fix_ports[] = {
    {"i", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
    {"o", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false},
    {"io", ODIN3_DIR_INOUT, true, 1, NULL, NULL, NULL, false},
};
static const odin3_port_def k_hm_ports[] = {
    {"A", ODIN3_DIR_IN, false, 3, NULL, NULL, NULL, false},
    {"Y", ODIN3_DIR_OUT, false, 0, NULL, NULL, NULL, false}, /* zero-width */
};
static const uint8_t k_mode_default[2] = {1, 0}; /* 2'01 */

/*
 * Module top instantiates bbp (black box, ports i/o of width W, default 1) at W = 8 and W = 4,
 * fix (a declared model, fixed ports with an inout) and hm (hard, constant widths, parameters of
 * every kind); unused declares a model nobody instantiates.
 */
static void build_blackboxes(void) {
    odin3_param_def bbp_params[1] = {{"W", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}}};
    odin3_celltype_def bbp = {
        "bbp", ODIN3_GRAN_BLACKBOX, 0, k_bbp_ports, 2, bbp_params, 1, NULL, NULL, NULL, NULL};
    odin3_param_def hm_params[3] = {
        {"W", ODIN3_VAL_INT, {ODIN3_VAL_INT, 2, NULL, 0, 0, 0}},
        {"MODE", ODIN3_VAL_BITS, {ODIN3_VAL_BITS, 0, k_mode_default, 2, 0, 0}},
        {"TAG", ODIN3_VAL_STRING, {ODIN3_VAL_STRING, 0, NULL, 0, intern("t"), 0}},
    };
    odin3_celltype_def hm = {
        "hm", ODIN3_GRAN_HARD, 0, k_hm_ports, 2, hm_params, 3, NULL, NULL, NULL, NULL};
    odin3_celltype_def fix = {
        "fix", ODIN3_GRAN_BLACKBOX, 0, k_fix_ports, 3, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_def unused = {
        "unused", ODIN3_GRAN_BLACKBOX, 0, k_fix_ports, 1, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &bbp, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &hm, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &unused, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &fix, &id));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &fix, &id));
    odin3_module *top = new_module("top");
    odin3_wire_id wire_a = port(top, "a", ODIN3_DIR_IN, 7, 0);
    odin3_wire_id wire_b = port(top, "b", ODIN3_DIR_IN, 3, 0);
    odin3_wire_id wire_y = port(top, "y", ODIN3_DIR_OUT, 7, 0);
    odin3_wire_id wire_z = port(top, "z", ODIN3_DIR_OUT, 3, 0);
    odin3_net_id net_c = bit_of(top, port(top, "c", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id net_w = bit_of(top, port(top, "w", ODIN3_DIR_OUT, 0, 0), 0);
    odin3_net_id net_q = bit_of(top, port(top, "q", ODIN3_DIR_INOUT, 0, 0), 0);
    odin3_net_id nets[4][8];
    const odin3_wire_id wires[4] = {wire_a, wire_y, wire_b, wire_z};
    for (uint32_t k = 0; k < 4; k++) {
        for (uint32_t j = 0; j < odin3_wire_width(top, wires[k]); j++) {
            nets[k][j] = bit_of(top, wires[k], j);
        }
    }
    odin3_value w8 = odin3_value_int(8);
    odin3_value w4 = odin3_value_int(4);
    odin3_netvec u1[2] = {{nets[0], 8}, {nets[1], 8}};
    odin3_netvec u2[2] = {{nets[2], 4}, {nets[3], 4}};
    odin3_netvec u3[3] = {{&net_c, 1}, {&net_w, 1}, {&net_q, 1}};
    odin3_value hp[3] = {odin3_value_int(5), bits_value(k_mode_default, 2), str_value("x")};
    odin3_netvec u4[2] = {{nets[2], 3}, {&net_c, 0}};
    (void)cell(top, (cell_req){"bbp", "u1", &w8, 1, u1});
    (void)cell(top, (cell_req){"bbp", "u2", &w4, 1, u2});
    (void)cell(top, (cell_req){"fix", "u3", NULL, 0, u3});
    (void)cell(top, (cell_req){"hm", "u4", hp, 3, u4});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, odin3_module_id_of(top)));
}

/* One stub per black-box or hard type, after the modules, as Yosys's read_verilog makes them. */
static void test_blackbox_stubs(void) {
    build_blackboxes();
    (void)rtlil();
    expect_line(
        "  cell \\bbp \\u1\n    parameter \\W 8\n    connect \\i \\a\n    connect \\o \\y\n  end");
    expect_line(
        "  cell \\bbp \\u2\n    parameter \\W 4\n    connect \\i \\b\n    connect \\o \\z\n  end");
    const char *stubs = "end\n"
                        "attribute \\blackbox 1\n"
                        "module \\unused\n"
                        "  wire input 1 \\i\n"
                        "end\n"
                        "attribute \\blackbox 1\n"
                        "module \\fix\n"
                        "  wire input 1 \\i\n"
                        "  wire output 2 \\o\n"
                        "  wire inout 3 \\io\n"
                        "end\n"
                        "attribute \\blackbox 1\n"
                        "attribute \\dynports 1\n"
                        "module \\bbp\n"
                        "  parameter \\W 1\n"
                        "  wire input 1 \\i\n"
                        "  wire output 2 \\o\n"
                        "end\n"
                        "attribute \\blackbox 1\n"
                        "module \\hm\n"
                        "  parameter \\W 2\n"
                        "  parameter \\MODE 2'01\n"
                        "  parameter \\TAG \"t\"\n"
                        "  wire width 3 input 1 \\A\n"
                        "  wire width 0 output 2 \\Y\n"
                        "end\n";
    size_t len = strlen(stubs);
    TEST_ASSERT_TRUE(text.len > len);
    TEST_ASSERT_EQUAL_STRING(stubs, text.data + text.len - len);
    TEST_ASSERT_EQUAL_INT(1, count_lines("module \\fix\n"));
    save_fixture("bbox"); /* CTest rtlil_bbox_hierarchy: Yosys 0.55 hierarchy -check */
}

/* A design without black boxes gets no stub; out of memory while writing stubs is NO_MEMORY with
 * the text unchanged. */
static void test_blackbox_stubs_oom(void) {
    (void)build_fixture();
    TEST_ASSERT_NULL(strstr(rtlil(), "blackbox"));
    build_blackboxes();
    char want[TEXT_MAX];
    int len = snprintf(want, sizeof want, "keep%s", rtlil());
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long tries = 0; st == ODIN3_ERR_NO_MEMORY && tries < OOM_LIMIT; tries++) {
        odin3_strbuf out;
        odin3_strbuf_init(&out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&out, odin3_bytes_cstr("keep")));
        odin3_util_set_alloc_fail_after(tries);
        st = odin3_rtlil_text(design, &out);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_STRING(st == ODIN3_OK ? want : "keep", out.data);
        odin3_strbuf_free(&out);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

/* --- the design's top (review M4) --------------------------------------------------------- */

/* The top gets "attribute \top 1" before its module line, as Yosys's write_rtlil writes it. */
static void test_top_attribute(void) {
    odin3_module *sub = new_module("sub");
    (void)port(sub, "i", ODIN3_DIR_IN, 0, 0);
    odin3_module *top = new_module("t");
    odin3_net_id x_net = bit_of(top, port(top, "x", ODIN3_DIR_IN, 0, 0), 0);
    odin3_netvec ports[1] = {{&x_net, 1}};
    (void)cell(top, (cell_req){"sub", "u", NULL, 0, ports});
    TEST_ASSERT_NULL(strstr(rtlil(), "attribute \\top"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, (odin3_module_id){2}));
    (void)rtlil();
    TEST_ASSERT_EQUAL_STRING_LEN("module \\sub\n", text.data, strlen("module \\sub\n"));
    expect_line("end\nattribute \\top 1\nmodule \\t");
    save_fixture("top");
}

/* --- every cell type (review M5) ----------------------------------------------------------- */

/* The types written as cells that the writer refuses as Odin III defines them (writer.c's
 * k_no_rtlil, less $sop, which has a const_value hook), and $mem and $proc, whose fixtures are
 * test_mem_parameters and the process tests. */
static const char *const k_not_every[] = {"$_DFF_P_",    "$_DFF_N_", "$_DLATCH_P_",
                                          "$_DLATCH_N_", "$_FF_",    "$memrd",
                                          "$memwr",      "$mem",     "$proc"};

/* True for a built-in `$` type the every-cell fixture writes as one cell. */
static bool in_every_cell(const odin3_celltype_def *def) {
    if (def->name[0] != '$' || def->gran == ODIN3_GRAN_PORT || def->const_value != NULL) {
        return false; /* ports, and constants and $sop (written as constants or refused) */
    }
    for (size_t i = 0; i < sizeof k_not_every / sizeof k_not_every[0]; i++) {
        if (strcmp(def->name, k_not_every[i]) == 0) {
            return false;
        }
    }
    /* The gate-level flip-flops of ir/cells/dffx.h carry an INIT, like $_DFF_P_: no RTLIL form. */
    return strncmp(def->name, "$_DFF", strlen("$_DFF")) != 0 &&
           strncmp(def->name, "$_SDFF", strlen("$_SDFF")) != 0;
}

/* def's defaults with every INT *WIDTH* parameter 1, into params. */
static void every_params(const odin3_celltype_def *def, odin3_value *params) {
    for (uint32_t idx = 0; idx < def->n_params; idx++) {
        params[idx] = def->params[idx].dflt;
        if (params[idx].kind == ODIN3_VAL_INT && strstr(def->params[idx].name, "WIDTH") != NULL) {
            params[idx] = odin3_value_int(1);
        }
    }
}

/* Each port of a node of type with params on fresh unnamed nets from nets (CELL_PINS_MAX). */
static void every_ports(odin3_module *mod, odin3_celltype_id type, const odin3_value *params,
                        odin3_netvec *ports, odin3_net_id *nets) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    uint32_t used = 0;
    for (uint32_t idx = 0; idx < def->n_ports; idx++) {
        uint32_t width = odin3_celltype_port_width(design, type, params, idx);
        TEST_ASSERT_TRUE_MESSAGE(used + width <= CELL_PINS_MAX, def->name);
        ports[idx] = (odin3_netvec){&nets[used], width};
        for (uint32_t bit = 0; bit < width; bit++) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, prov, &nets[used++]));
        }
    }
}

/* One node of def's type named c_<type without $>: its defaults, every INT *WIDTH* parameter 1,
 * each pin on a fresh unnamed net. */
static void add_every_cell(odin3_module *mod, const odin3_celltype_def *def) {
    TEST_ASSERT_TRUE_MESSAGE(def->n_params <= CELL_PORTS_MAX && def->n_ports <= CELL_PORTS_MAX,
                             def->name);
    odin3_celltype_id type = type_named(def->name);
    odin3_value params[CELL_PORTS_MAX];
    every_params(def, params);
    odin3_net_id nets[CELL_PINS_MAX];
    odin3_netvec ports[CELL_PORTS_MAX];
    every_ports(mod, type, params, ports, nets);
    char name[NAME_MAX_LEN];
    (void)snprintf(name, sizeof name, "c_%s", def->name + 1);
    odin3_node_spec spec = {type, intern(name), prov, params, def->n_params};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL),
                                  def->name);
}

/* Every registered built-in `$` type that write_rtlil writes as a cell, one cell each, so the
 * Yosys 0.55 round trip checks each definition against Yosys's internal cell checker and grows
 * with the registry (2E-1's flip-flops, 3D's $lut). */
static void test_every_cell(void) {
    odin3_module *mod = new_module("cells");
    uint32_t made = 0;
    for (uint32_t idx = 0; idx < odin3_builtin_celltype_count; idx++) {
        if (in_every_cell(odin3_builtin_celltypes[idx])) {
            add_every_cell(mod, odin3_builtin_celltypes[idx]);
            made++;
        }
    }
    (void)rtlil();
    for (uint32_t idx = 0; idx < odin3_builtin_celltype_count; idx++) {
        const odin3_celltype_def *def = odin3_builtin_celltypes[idx];
        char line[LOG_CAP];
        (void)snprintf(line, sizeof line, "  cell %s \\c_%s", def->name, def->name + 1);
        if (in_every_cell(def)) {
            expect_line(line);
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(EVERY_CELL_MIN, made);
    save_fixture("every_cell");
}

/* --- the eleven flip-flops and latches (2E IR change D, §13) -------------------------------- */

/* $aldff (WIDTH 1) is written generically: simlib's parameters and ports, in order. */
static void test_rtlil_aldff(void) {
    odin3_module *mod = new_module("m");
    odin3_net_id clk = bit_of(mod, port(mod, "clk", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id aload = bit_of(mod, port(mod, "aload", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id ad = bit_of(mod, port(mod, "ad", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id dat = bit_of(mod, port(mod, "d", ODIN3_DIR_IN, 0, 0), 0);
    odin3_net_id out = bit_of(mod, port(mod, "q", ODIN3_DIR_OUT, 0, 0), 0);
    odin3_value params[3] = {odin3_value_int(1), odin3_value_int(1), odin3_value_int(1)};
    odin3_netvec ports[5] = {{&clk, 1}, {&aload, 1}, {&ad, 1}, {&dat, 1}, {&out, 1}};
    uint32_t k = cell(mod, (cell_req){"$aldff", NULL, params, 3, ports}).v;
    char want[LOG_CAP];
    (void)snprintf(want, sizeof want,
                   "  cell $aldff $c%u\n"
                   "    parameter \\WIDTH 1\n"
                   "    parameter \\CLK_POLARITY 1\n"
                   "    parameter \\ALOAD_POLARITY 1\n"
                   "    connect \\CLK \\clk\n"
                   "    connect \\ALOAD \\aload\n"
                   "    connect \\AD \\ad\n"
                   "    connect \\D \\d\n"
                   "    connect \\Q \\q\n"
                   "  end\n",
                   k);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rtlil(), want), text.data);
}

/* Module ffcells (all eleven, WIDTH 2) to ffcells.il, the input of CTest ffcells_yosys_read. */
static void test_ffcells_fixture(void) {
    (void)ffcells_build(design);
    (void)rtlil();
    for (uint32_t i = 0; i < FFCELLS_COUNT; i++) {
        char line[LOG_CAP];
        (void)snprintf(line, sizeof line, "  cell %s \\c_%s", ffcells_types[i],
                       ffcells_types[i] + 1);
        expect_line(line);
    }
    expect_line("    parameter \\ARST_VALUE 2'01");
    expect_line("    parameter \\CLR_POLARITY 0");
    save_fixture("ffcells");
}

/* --- refusals and the atomic write --------------------------------------------------------- */

/* A node of type tname named u (params NULL: the defaults) in module r must be refused with
 * "write_rtlil: module 'r': cell 'u' of type '<tname>' <why>", and out stays unchanged. */
static void expect_cell_refused(const char *tname, const odin3_value *params, uint32_t n_params,
                                const char *why) {
    odin3_module *mod = new_module("r");
    odin3_node_spec spec = {type_named(tname), intern("u"), prov, params, n_params};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, NULL));
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("keep")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(design, &text));
    TEST_ASSERT_EQUAL_STRING("keep", text.data);
    char want[LOG_CAP];
    (void)snprintf(want, sizeof want, "write_rtlil: module 'r': cell 'u' of type '%s' %s", tname,
                   why);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
}

static void test_refuses_dff_p(void) {
    expect_cell_refused("$_DFF_P_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_dff_n(void) {
    expect_cell_refused("$_DFF_N_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_dlatch_p(void) {
    expect_cell_refused("$_DLATCH_P_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_dlatch_n(void) {
    expect_cell_refused("$_DLATCH_N_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_ff(void) {
    expect_cell_refused("$_FF_", NULL, 0, "has no RTLIL form");
}

/* The 68 gate-level flip-flops of ir/cells/dffx.h: one of each family. */
static void test_refuses_dffx_sync_reset(void) {
    expect_cell_refused("$_SDFF_PP0_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_dffx_enable(void) {
    expect_cell_refused("$_DFFE_PN_", NULL, 0, "has no RTLIL form");
}

static void test_refuses_dffx_async_reset(void) {
    expect_cell_refused("$_DFF_PN1_", NULL, 0, "has no RTLIL form");
}

/* A $sop that is not a constant (one input, cover "1 1"). */
static void test_refuses_sop(void) {
    static const uint8_t k_cover[2] = {'1', '1'};
    odin3_value params[2] = {
        odin3_value_int(1),
        {.kind = ODIN3_VAL_COVER, .bits = k_cover, .len = 2, .cover_inputs = 1}};
    expect_cell_refused("$sop", params, 2, "has no RTLIL form");
}

static void test_refuses_memrd(void) {
    expect_cell_refused("$memrd", NULL, 0, "has no RTLIL form");
}

static void test_refuses_memwr(void) {
    expect_cell_refused("$memwr", NULL, 0, "has no RTLIL form");
}

/* The registry's default $mem has an empty MEMID. */
static void test_refuses_mem_with_empty_memid(void) {
    expect_cell_refused("$mem", NULL, 0, "has an empty MEMID");
}

/* A refused unnamed node is named $c<ID>, as it would be written. */
static void test_refusal_names_unnamed_cell(void) {
    odin3_module *mod = new_module("r");
    odin3_node_spec spec = {type_named("$memrd"), 0, prov, NULL, 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, &node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(design, &text));
    char want[LOG_CAP];
    (void)snprintf(want, sizeof want,
                   "write_rtlil: module 'r': cell '$c%u' of type '$memrd' has no RTLIL form",
                   node.v);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
}

static void test_unwritable_name_is_refused(void) {
    odin3_module *mod = new_module("bad");
    (void)wire_on(mod, "a b", 0, 0, NULL);
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("keep")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(design, &text));
    TEST_ASSERT_EQUAL_STRING("keep", text.data);
    TEST_ASSERT_EQUAL_STRING("write_rtlil: module 'bad': wire 'a b' cannot be written in RTLIL",
                             last_error);
}

static void test_net_named_like_wire_is_refused(void) {
    odin3_module *mod = new_module("dup");
    (void)wire_on(mod, "w", 0, 0, NULL);
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern("w"), prov, &net));
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(design, &text));
    TEST_ASSERT_EQUAL_STRING("write_rtlil: module 'dup': net 'w' has the name of a wire",
                             last_error);
}

static void test_null_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(NULL, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_text(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_write(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_write(NULL, "x.il"));
}

static const char *const BASELINE = "previous contents\n";

/* The destination still holds BASELINE and no "<name>." temporary is left beside it. */
static void expect_baseline(const char *path) {
    TEST_ASSERT_EQUAL_STRING(BASELINE, slurp(path));
    DIR *dir = opendir(ODIN3_RTLIL_OUT);
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        TEST_ASSERT_NULL_MESSAGE(strstr(ent->d_name, "kept.txt."), ent->d_name);
    }
    (void)closedir(dir);
}

static void test_failed_write_keeps_destination(void) {
    char path[PATH_MAX_LEN];
    (void)snprintf(path, sizeof path, "%s/kept.txt", ODIN3_RTLIL_OUT);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(BASELINE), fwrite(BASELINE, 1, strlen(BASELINE), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    odin3_module *mod = new_module("bad");
    (void)wire_on(mod, "a\tb", 0, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_rtlil_write(design, path));
    expect_baseline(path);
    odin3_util_set_alloc_fail_after(0);
    odin3_status st = odin3_rtlil_write(design, path);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, st);
    expect_baseline(path);
}

static void test_unopenable_path_is_io(void) {
    (void)build_fixture();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_rtlil_write(design, "/nonexistent-dir/out.il"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "/nonexistent-dir/out.il: cannot open"));
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Every allocation failure is NO_MEMORY with out unchanged; enough memory gives the text. */
static void test_out_of_memory(void) {
    odin3_module *mod = new_module("p");
    (void)build_sync_process(mod);
    (void)build_fixture();
    char want[TEXT_MAX];
    (void)snprintf(want, sizeof want, "keep%s", rtlil());
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long tries = 0;
    for (; st == ODIN3_ERR_NO_MEMORY && tries < OOM_LIMIT; tries++) {
        odin3_strbuf out;
        odin3_strbuf_init(&out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&out, odin3_bytes_cstr("keep")));
        odin3_util_set_alloc_fail_after(tries);
        st = odin3_rtlil_text(design, &out);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_EQUAL_STRING(st == ODIN3_OK ? want : "keep", out.data);
        odin3_strbuf_free(&out);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_INT32(1, (int32_t)tries);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fixture_text);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_wires_and_sigspecs);
    RUN_TEST(test_constant_alias);
    RUN_TEST(test_attributes);
    RUN_TEST(test_every_sync_type);
    RUN_TEST(test_process_after_cells);
    RUN_TEST(test_process_name_with_blank_path);
    RUN_TEST(test_switch_process);
    RUN_TEST(test_mem_parameters);
    RUN_TEST(test_nets_without_wires);
    RUN_TEST(test_unconnected_pins);
    RUN_TEST(test_instances_and_names);
    RUN_TEST(test_module_names_like_ids);
    RUN_TEST(test_top_attribute);
    RUN_TEST(test_every_cell);
    RUN_TEST(test_rtlil_aldff);
    RUN_TEST(test_ffcells_fixture);
    RUN_TEST(test_refuses_dff_p);
    RUN_TEST(test_refuses_dff_n);
    RUN_TEST(test_refuses_dlatch_p);
    RUN_TEST(test_refuses_dlatch_n);
    RUN_TEST(test_refuses_ff);
    RUN_TEST(test_refuses_dffx_sync_reset);
    RUN_TEST(test_refuses_dffx_enable);
    RUN_TEST(test_refuses_dffx_async_reset);
    RUN_TEST(test_refuses_sop);
    RUN_TEST(test_refuses_memrd);
    RUN_TEST(test_refuses_memwr);
    RUN_TEST(test_refuses_mem_with_empty_memid);
    RUN_TEST(test_refusal_names_unnamed_cell);
    RUN_TEST(test_unwritable_name_is_refused);
    RUN_TEST(test_net_named_like_wire_is_refused);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_failed_write_keeps_destination);
    RUN_TEST(test_unopenable_path_is_io);
    RUN_TEST(test_out_of_memory);
    RUN_TEST(test_blackbox_stubs);
    RUN_TEST(test_blackbox_stubs_oom);
    return UNITY_END();
}
