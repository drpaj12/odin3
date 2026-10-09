/*
 * test_verilog_writer.c — unit tests for the structural Verilog writer (IR built through the API).
 */
#include "backends/verilog/writer.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum {
    TEXT_MAX = 1 << 16,
    MSG_MAX = 512,
    NAME_BUF = 64,
    DIR_BUF = 128,
    PATH_BUF = 256,
    SCRIPT_BUF = 1024,
    OOM_LIMIT = 20000,
    TOOL_MISSING = -1
};

/* A private directory per run (under TMPDIR or /tmp) and the files the tests use in it. */
static char out_dir[DIR_BUF];
static char out_path[PATH_BUF];  /* the writer's output */
static char out2_path[PATH_BUF]; /* a second output, or a tool's output file */
static char aux_path[PATH_BUF];  /* a tool's stdout */
static char tb_path[PATH_BUF];   /* a testbench */
static char vvp_path[PATH_BUF];  /* a compiled simulation */
static char *const RUN_FILES[] = {out_path, out2_path, aux_path, tb_path, vvp_path};

static odin3_design *design;
static odin3_module *module; /* the module being built */
static char text[TEXT_MAX];  /* the writer's output */
static char aux[TEXT_MAX];   /* a tool's output */
static char last_error[MSG_MAX];
static size_t errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

static void dir_file(char *buf, const char *name) {
    (void)snprintf(buf, PATH_BUF, "%s/%s", out_dir, name);
}

static void make_out_dir(void) {
    const char *base = getenv("TMPDIR");
    (void)snprintf(out_dir, sizeof out_dir, "%s/odin3_vwriter_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(out_dir));
    dir_file(out_path, "out.v");
    dir_file(out2_path, "out2.v");
    dir_file(aux_path, "aux.txt");
    dir_file(tb_path, "tb.v");
    dir_file(vvp_path, "sim.vvp");
}

/* Removes every file a test may leave, then the directory (which must then be empty: the
 * writer leaves no temporary behind). */
static void remove_out_dir(void) {
    for (size_t i = 0; i < sizeof RUN_FILES / sizeof RUN_FILES[0]; i++) {
        (void)remove(RUN_FILES[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(out_dir));
}

void setUp(void) {
    last_error[0] = '\0';
    errors = 0;
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    module = NULL;
    make_out_dir();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    remove_out_dir();
}

/* --- external tools ------------------------------------------------------------------------ */

/* Runs argv (PATH search) with stdout to stdout_path (NULL: /dev/null); its exit status, or
 * TOOL_MISSING when argv[0] is not found. */
static int run_tool(char *const argv[], const char *stdout_path) {
    posix_spawn_file_actions_t actions;
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
    TEST_ASSERT_EQUAL_INT(
        0, posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                            stdout_path != NULL ? stdout_path : "/dev/null",
                                            O_WRONLY | O_CREAT | O_TRUNC, 0644));
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (rc == ENOENT) {
        return TOOL_MISSING;
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Yosys: $ODIN3_YOSYS when set, else `yosys` on PATH. */
static char *yosys_path(void) {
    char *env = getenv("ODIN3_YOSYS");
    return env != NULL && env[0] != '\0' ? env : "yosys";
}

/* iverilog compiles path as Verilog-2005 (true), or iverilog is missing (false, noted). */
static bool iverilog_compiles(const char *path) {
    char *argv[] = {"iverilog", "-g2005", "-o", "/dev/null", (char *)path, NULL};
    int rc = run_tool(argv, NULL);
    if (rc == TOOL_MISSING) {
        TEST_MESSAGE("iverilog not found: compile check skipped");
        return false;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "iverilog rejected the writer's output");
    return true;
}

/* Runs yosys with script, stdout to aux_path; ignores the test when yosys is missing. */
static void yosys_runs(const char *script) {
    char *argv[] = {yosys_path(), "-q", "-p", (char *)script, NULL};
    int rc = run_tool(argv, aux_path);
    if (rc == TOOL_MISSING) {
        TEST_IGNORE_MESSAGE("yosys not found (PATH or ODIN3_YOSYS): Yosys check skipped");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "yosys failed on the writer's output");
}

/* --- files --------------------------------------------------------------------------------- */

static const char *slurp(const char *path, char *buf) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[len] = '\0';
    return buf;
}

/* Writes the testbench text to tb_path. */
static void put_testbench(const char *data) {
    FILE *file = fopen(tb_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(data), fwrite(data, 1, strlen(data), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

/* Writes the design to out_path and loads it into text, without the iverilog check. */
static const char *write_text(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_verilog_write(design, out_path, NULL),
                                  last_error);
    TEST_ASSERT_EQUAL_STRING("", last_error);
    return slurp(out_path, text);
}

/* write_text, then iverilog must compile the output (when installed). */
static const char *write_ok(void) {
    (void)write_text();
    (void)iverilog_compiles(out_path);
    return text;
}

static void expect_in(const char *hay, const char *needle) {
    if (strstr(hay, needle) == NULL) {
        (void)fprintf(stderr, "--- output ---\n%s--- end ---\n", hay);
        TEST_FAIL_MESSAGE(needle);
    }
}

static void expect_has(const char *needle) {
    expect_in(text, needle);
}

static void expect_not(const char *needle) {
    if (strstr(text, needle) != NULL) {
        (void)fprintf(stderr, "--- output ---\n%s--- end ---\n", text);
        TEST_FAIL_MESSAGE(needle);
    }
}

static size_t count_of(const char *hay, const char *needle) {
    size_t count = 0;
    for (const char *at = strstr(hay, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* --- building IR --------------------------------------------------------------------------- */

static uint32_t intern(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &id));
    return id;
}

static odin3_celltype_id type_of(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

static odin3_module *new_module_prov(const char *name, odin3_prov_id prov) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &id));
    module = odin3_module_get(design, id);
    TEST_ASSERT_NOT_NULL(module);
    return module;
}

static odin3_module *new_module(const char *name) {
    return new_module_prov(name, (odin3_prov_id){0});
}

typedef struct port_args {
    const char *name;
    odin3_dir dir;
    uint32_t width;
    bool scalar;
} port_args;

static odin3_wire_id add_port_prov(port_args args, odin3_prov_id prov) {
    odin3_port_spec spec = {intern(args.name), args.dir, args.width, args.scalar, prov};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(module, &spec, &node));
    return odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
}

static odin3_wire_id in_vec(const char *name, uint32_t width) {
    return add_port_prov((port_args){name, ODIN3_DIR_IN, width, false}, (odin3_prov_id){0});
}
static odin3_wire_id out_vec(const char *name, uint32_t width) {
    return add_port_prov((port_args){name, ODIN3_DIR_OUT, width, false}, (odin3_prov_id){0});
}
static odin3_net_id in_bit(const char *name) {
    odin3_wire_id wire =
        add_port_prov((port_args){name, ODIN3_DIR_IN, 1, true}, (odin3_prov_id){0});
    return odin3_wire_net(module, wire, 0);
}
static odin3_net_id out_bit(const char *name) {
    odin3_wire_id wire =
        add_port_prov((port_args){name, ODIN3_DIR_OUT, 1, true}, (odin3_prov_id){0});
    return odin3_wire_net(module, wire, 0);
}

static odin3_net_id bit_of(odin3_wire_id wire, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(module, wire, bit);
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    return net;
}

static odin3_net_id new_net_prov(const char *name, odin3_prov_id prov) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(module, name != NULL ? intern(name) : 0, prov, &net));
    return net;
}

static odin3_net_id new_net(const char *name) {
    return new_net_prov(name, (odin3_prov_id){0});
}

/* The nets of a wire, LSB first, into nets (capacity NAME_BUF). */
static odin3_netvec wire_nets(odin3_wire_id wire, odin3_net_id *nets) {
    uint32_t width = odin3_wire_width(module, wire);
    TEST_ASSERT_TRUE(width <= NAME_BUF);
    for (uint32_t i = 0; i < width; i++) {
        nets[i] = bit_of(wire, i);
    }
    return (odin3_netvec){nets, width};
}

typedef struct cell_args {
    const char *type;
    const char *name;
    const odin3_value *params;
    uint32_t n_params;
    odin3_prov_id prov;
} cell_args;

static odin3_node_id add_cell_args(cell_args args, const odin3_netvec *ports) {
    odin3_node_spec spec = {type_of(args.type), args.name != NULL ? intern(args.name) : 0,
                            args.prov, args.params, args.n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node), last_error);
    return node;
}

static odin3_node_id add_cell(const char *type, const odin3_value *params, uint32_t n_params,
                              const odin3_netvec *ports) {
    return add_cell_args((cell_args){type, NULL, params, n_params, {0}}, ports);
}

#define NV1(net) ((odin3_netvec){(odin3_net_id[]){net}, 1})

/* The input and output net of a one-input gate. */
typedef struct io_pair {
    odin3_net_id in, out;
} io_pair;

static odin3_node_id gate1(const char *type, io_pair io) {
    odin3_netvec ports[] = {NV1(io.in), NV1(io.out)};
    return add_cell(type, NULL, 0, ports);
}

static odin3_node_id gate2(const char *type, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id out) {
    odin3_netvec ports[] = {NV1(lhs), NV1(rhs), NV1(out)};
    return add_cell(type, NULL, 0, ports);
}

/* A $sop over ins (width nets) with cover rows packed as width input chars + output char. */
static odin3_node_id sop(odin3_netvec ins, const char *rows, odin3_net_id out) {
    odin3_value params[2] = {
        odin3_value_int(ins.count),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)rows, (uint32_t)strlen(rows), 0, ins.count}};
    odin3_netvec ports[] = {ins, NV1(out)};
    return add_cell("$sop", params, 2, ports);
}

/* A bit-level storage cell: ctrl (none for $_FF_), d, q, INIT. */
typedef struct storage_args {
    const char *type;
    odin3_net_id ctrl, data, q;
    int64_t init;
} storage_args;

static odin3_node_id storage(storage_args args) {
    odin3_value params[1] = {odin3_value_int(args.init)};
    if (!odin3_net_valid(args.ctrl)) {
        odin3_netvec ports[] = {NV1(args.data), NV1(args.q)};
        return add_cell(args.type, params, 1, ports);
    }
    odin3_netvec ports[] = {NV1(args.ctrl), NV1(args.data), NV1(args.q)};
    return add_cell(args.type, params, 1, ports);
}

/* Word binary cell params A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH from the port vectors. */
static odin3_node_id binary(const char *type, bool is_signed, const odin3_netvec *ports) {
    odin3_value params[5] = {odin3_value_int(is_signed), odin3_value_int(is_signed),
                             odin3_value_int(ports[0].count), odin3_value_int(ports[1].count),
                             odin3_value_int(ports[2].count)};
    return add_cell(type, params, 5, ports);
}

static odin3_node_id unary(const char *type, const odin3_netvec *ports) {
    odin3_value params[3] = {odin3_value_int(0), odin3_value_int(ports[0].count),
                             odin3_value_int(ports[1].count)};
    return add_cell(type, params, 3, ports);
}

/* "\$c<ID> " — the generated Verilog name of a node. */
static const char *cname(odin3_node_id node, char *buf) {
    (void)snprintf(buf, NAME_BUF, "\\$c%u ", node.v);
    return buf;
}

/* Formats into buf (capacity MSG_MAX) and checks the output has it. */
static void expect_fmt(char *buf, const char *fmt, const char *arg) {
    (void)snprintf(buf, MSG_MAX, fmt, arg);
    expect_has(buf);
}

/* --- identifiers (Review Focus 1) ---------------------------------------------------------- */

static odin3_verilog_ident kind_of(const char *name) {
    return odin3_verilog_ident_kind(odin3_bytes_cstr(name));
}

static void test_ident_kinds(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_SIMPLE, kind_of("a"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_SIMPLE, kind_of("_x9"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_SIMPLE, kind_of("a$b"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_SIMPLE, kind_of("Wire"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("$add~5^ADD~5-1[0]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("n~19"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("1abc"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("a.b"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("a[0]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("\\x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("wire"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("module"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("logic"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("always_ff"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_ESCAPED, kind_of("xnor"));
}

static void test_ident_unwritable(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of(""));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("a b"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("a\tb"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("a\nb"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("caf\xc3\xa9"));
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("\x7f"));
}

static void test_keywords(void) {
    TEST_ASSERT_EQUAL_INT(1, odin3_verilog_is_keyword(odin3_bytes_cstr("endmodule")));
    TEST_ASSERT_EQUAL_INT(1, odin3_verilog_is_keyword(odin3_bytes_cstr("supply0")));
    TEST_ASSERT_EQUAL_INT(0, odin3_verilog_is_keyword(odin3_bytes_cstr("Module")));
    TEST_ASSERT_EQUAL_INT(0, odin3_verilog_is_keyword(odin3_bytes_cstr("")));
}

static void test_ident_append(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_verilog_append_ident(&buf, odin3_bytes_cstr("$add~5^ADD~5-1[0]")));
    TEST_ASSERT_EQUAL_STRING("\\$add~5^ADD~5-1[0] ", buf.data);
    odin3_strbuf_clear(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_append_ident(&buf, odin3_bytes_cstr("abc")));
    TEST_ASSERT_EQUAL_STRING("abc", buf.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_append_ident(&buf, odin3_bytes_cstr("a b")));
    TEST_ASSERT_EQUAL_STRING("abc", buf.data);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_NO_MEMORY,
        odin3_verilog_append_ident(
            &buf, odin3_bytes_cstr("$a_rather_long_name_that_must_grow_the_buffer")));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_STRING("abc", buf.data);
    odin3_strbuf_free(&buf);
}

/* esc: a[1:0], \b.c -> nets `$add~5^ADD~5-1[0]` = a0 & ~a1, `n~19` = ~(that & b.c) -> y;
 * wire `top/u1.v`[1:0] = ~a, w = its bit 1 & bit 0. */
static void build_escapes(void) {
    new_module("esc");
    odin3_wire_id net_a = in_vec("a", 2);
    odin3_net_id bc = in_bit("b.c");
    odin3_net_id net_y = out_bit("y");
    odin3_net_id add5 = new_net("$add~5^ADD~5-1[0]");
    odin3_net_id n19 = new_net("n~19");
    odin3_net_id a_nets[2];
    (void)sop(wire_nets(net_a, a_nets),
              "10"
              "1",
              add5);
    (void)sop((odin3_netvec){(odin3_net_id[]){add5, bc}, 2},
              "11"
              "0",
              n19);
    (void)gate1("$_BUF_", (io_pair){n19, net_y});
    /* An escaped vector wire read bit by bit: \top/u1.v [1] & \top/u1.v [0] -> w. */
    odin3_net_id net_w = out_bit("w");
    odin3_wire_spec spec = {intern("top/u1.v"), 1, 0, false, {0}};
    odin3_wire_id vec = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &vec));
    odin3_net_id v_nets[2];
    odin3_netvec inv[] = {wire_nets(net_a, a_nets), wire_nets(vec, v_nets)};
    (void)unary("$not", inv);
    (void)gate2("$_AND_", v_nets[1], v_nets[0], net_w);
}

static void test_escaped_names(void) {
    build_escapes();
    (void)write_ok();
    expect_has("module esc (\n  input [1:0] a,\n  input \\b.c ,\n  output y,\n  output w\n);\n");
    expect_has("  wire [1:0] \\top/u1.v ;\n");
    expect_has("  assign \\top/u1.v = ~a;\n");
    expect_has("  assign w = \\top/u1.v [1] & \\top/u1.v [0];\n");
    expect_has("  wire \\$add~5^ADD~5-1[0] ;\n");
    expect_has("  wire \\n~19 ;\n");
    expect_has("  assign \\$add~5^ADD~5-1[0] = a[0] & ~a[1];\n");
    expect_has("  assign \\n~19 = ~(\\$add~5^ADD~5-1[0] & \\b.c );\n");
    expect_has("  assign y = \\n~19 ;\n");
    expect_has("endmodule\n");
}

/* Review Focus 1: the escaped names survive Yosys read_verilog (and its write_verilog). */
static void test_escaped_names_yosys_round_trip(void) {
    build_escapes();
    (void)write_ok();
    char script[SCRIPT_BUF];
    (void)snprintf(script, sizeof script, "read_verilog %s; write_verilog -noattr %s", out_path,
                   out2_path);
    yosys_runs(script);
    const char *back = slurp(out2_path, aux);
    expect_in(back, "\\$add~5^ADD~5-1[0] ");
    expect_in(back, "\\n~19 ");
    expect_in(back, "\\b.c ");
    expect_in(back, "\\top/u1.v ");
}

/* --- bit-level gates ----------------------------------------------------------------------- */

static void test_gates(void) {
    new_module("gates");
    odin3_net_id net_a = in_bit("a");
    odin3_net_id net_b = in_bit("b");
    odin3_net_id net_s = in_bit("s");
    static const char *const kinds[] = {"$_AND_",  "$_OR_",  "$_XOR_",
                                        "$_NAND_", "$_NOR_", "$_XNOR_"};
    for (uint32_t i = 0; i < 6; i++) {
        char name[NAME_BUF];
        (void)snprintf(name, sizeof name, "y%u", i);
        (void)gate2(kinds[i], net_a, net_b, out_bit(name));
    }
    (void)gate1("$_BUF_", (io_pair){net_a, out_bit("buf")});
    (void)gate1("$_NOT_", (io_pair){net_a, out_bit("inv")});
    odin3_netvec mux[] = {NV1(net_a), NV1(net_b), NV1(net_s), NV1(out_bit("mx"))};
    (void)add_cell("$_MUX_", NULL, 0, mux);
    (void)write_ok();
    expect_has("  assign y0 = a & b;\n");
    expect_has("  assign y1 = a | b;\n");
    expect_has("  assign y2 = a ^ b;\n");
    expect_has("  assign y3 = ~(a & b);\n");
    expect_has("  assign y4 = ~(a | b);\n");
    expect_has("  assign y5 = ~(a ^ b);\n");
    expect_has("  assign \\buf = a;\n"); /* buf is a gate keyword */
    expect_has("  assign inv = ~a;\n");
    expect_has("  assign mx = s ? b : a;\n");
}

/* --- $sop (Review Focus 3) ----------------------------------------------------------------- */

/* The ON-set rows of the sop3 fixture: a0 & ~a2, ~a0 & a1 & a2. */
#define SOP3_ON                                                                                    \
    "1-0"                                                                                          \
    "1"                                                                                            \
    "011"                                                                                          \
    "1"
#define SOP3_OFF                                                                                   \
    "1-0"                                                                                          \
    "0"                                                                                            \
    "011"                                                                                          \
    "0"

/* sop3: a[2:0] -> on (ON-set), off (same rows as OFF-set), k1/k0/kn (zero-input covers "1", "0",
 * no rows), e3 (three inputs, no rows). */
static void build_sops(void) {
    new_module("sop3");
    odin3_wire_id net_a = in_vec("a", 3);
    odin3_net_id on = out_bit("on");
    odin3_net_id off = out_bit("off");
    odin3_net_id k1 = out_bit("k1");
    odin3_net_id k0 = out_bit("k0");
    odin3_net_id kn = out_bit("kn");
    odin3_net_id e3 = out_bit("e3");
    odin3_net_id a_nets[3];
    odin3_netvec ins = wire_nets(net_a, a_nets);
    (void)sop(ins, SOP3_ON, on);
    (void)sop(ins, SOP3_OFF, off);
    (void)sop((odin3_netvec){NULL, 0}, "1", k1);
    (void)sop((odin3_netvec){NULL, 0}, "0", k0);
    (void)sop((odin3_netvec){NULL, 0}, "", kn);
    (void)sop(ins, "", e3);
}

static void test_sop_text(void) {
    build_sops();
    (void)write_ok();
    expect_has("  assign on = (a[0] & ~a[2]) | (~a[0] & a[1] & a[2]);\n");
    expect_has("  assign off = ~((a[0] & ~a[2]) | (~a[0] & a[1] & a[2]));\n");
    expect_has("  assign k1 = 1'b1;\n");
    expect_has("  assign k0 = 1'b0;\n");
    expect_has("  assign kn = 1'b0;\n");
    expect_has("  assign e3 = 1'b0;\n");
}

/* Value of the sop3 ON-set cover at input value in (bit k = a[k]). */
static int sop3_on(unsigned in) {
    unsigned a0 = in & 1U;
    unsigned a1 = (in >> 1) & 1U;
    unsigned a2 = (in >> 2) & 1U;
    return (int)((a0 & (a2 ^ 1U)) | ((a0 ^ 1U) & a1 & a2));
}

static const char *const SOP3_TB =
    "module tb;\n"
    "  reg [2:0] a;\n"
    "  wire on, off, k1, k0, kn, e3;\n"
    "  integer i;\n"
    "  sop3 dut(.a(a), .on(on), .off(off), .k1(k1), .k0(k0), .kn(kn), .e3(e3));\n"
    "  initial begin\n"
    "    for (i = 0; i < 8; i = i + 1) begin\n"
    "      a = i;\n"
    "      #1 $display(\"%0d %b%b%b%b%b%b\", i, on, off, k1, k0, kn, e3);\n"
    "    end\n"
    "  end\n"
    "endmodule\n";

/* Review Focus 3: simulating the written covers gives the BLIF semantics (OFF-set complemented). */
static void test_sop_simulates(void) {
    build_sops();
    (void)write_ok();
    put_testbench(SOP3_TB);
    char *compile[] = {"iverilog",      "-g2005",         "-o", (char *)vvp_path,
                       (char *)tb_path, (char *)out_path, NULL};
    int rc = run_tool(compile, NULL);
    if (rc == TOOL_MISSING) {
        TEST_IGNORE_MESSAGE("iverilog not found: simulation skipped");
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    char *sim[] = {"vvp", "-n", (char *)vvp_path, NULL};
    TEST_ASSERT_EQUAL_INT(0, run_tool(sim, aux_path));
    char expected[MSG_MAX] = "";
    size_t len = 0;
    for (unsigned i = 0; i < 8; i++) {
        int on = sop3_on(i);
        len += (size_t)snprintf(expected + len, sizeof expected - len, "%u %d%d1000\n", i, on, !on);
    }
    TEST_ASSERT_EQUAL_STRING(expected, slurp(aux_path, aux));
}

static void test_sop_mixed_cover_refused(void) {
    new_module("mixed");
    odin3_wire_id net_a = in_vec("a", 2);
    odin3_net_id net_y = out_bit("y");
    odin3_net_id a_nets[2];
    (void)sop(wire_nets(net_a, a_nets),
              "10"
              "1"
              "01"
              "0",
              net_y);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, out_path, NULL));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "mixes"));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "'$c"));
    TEST_ASSERT_FALSE(file_exists(out_path));
}

/* --- storage cells (Review Focus 5) -------------------------------------------------------- */

typedef struct regs_fixture {
    odin3_node_id dff_n, dlatch_p, dlatch_n, ff;
} regs_fixture;

/* regs: clk en d -> q0 (via net r0, $_DFF_P_ INIT 0), q1 ($_DFF_N_ INIT 1), q2 ($_DLATCH_P_
 * INIT 2), q3 ($_DLATCH_N_ INIT 3), q4 ($_FF_ INIT 1), q5 (via net r5, $_DFF_P_ INIT 3). */
static regs_fixture build_regs(void) {
    new_module("regs");
    odin3_net_id clk = in_bit("clk");
    odin3_net_id en = in_bit("en");
    odin3_net_id net_d = in_bit("d");
    odin3_net_id net_q[6];
    for (uint32_t i = 0; i < 6; i++) {
        char name[NAME_BUF];
        (void)snprintf(name, sizeof name, "q%u", i);
        net_q[i] = out_bit(name);
    }
    odin3_net_id r0 = new_net("r0");
    odin3_net_id r5 = new_net("r5");
    regs_fixture fix;
    (void)storage((storage_args){"$_DFF_P_", clk, net_d, r0, 0});
    (void)gate1("$_BUF_", (io_pair){r0, net_q[0]});
    fix.dff_n = storage((storage_args){"$_DFF_N_", clk, net_d, net_q[1], 1});
    fix.dlatch_p = storage((storage_args){"$_DLATCH_P_", en, net_d, net_q[2], 2});
    fix.dlatch_n = storage((storage_args){"$_DLATCH_N_", en, net_d, net_q[3], 3});
    fix.ff = storage((storage_args){"$_FF_", {0}, net_d, net_q[4], 1});
    (void)storage((storage_args){"$_DFF_P_", clk, net_d, r5, 3});
    (void)gate1("$_BUF_", (io_pair){r5, net_q[5]});
    return fix;
}

static void test_storage_text(void) {
    regs_fixture fix = build_regs();
    (void)write_ok();
    char name[NAME_BUF];
    char line[MSG_MAX];
    expect_has("  reg r0;\n");
    expect_has("  always @(posedge clk) r0 <= d;\n  initial r0 = 1'b0;\n");
    expect_fmt(line, "  reg %s;\n", cname(fix.dff_n, name));
    expect_fmt(line, "  always @(negedge clk) %s<= d;\n", name);
    expect_fmt(line, "  initial %s= 1'b1;\n", name);
    expect_fmt(line, "  assign q1 = %s;\n", name);
    expect_fmt(line, "  always @* if (en) %s<= d;\n", cname(fix.dlatch_p, name));
    expect_fmt(line, "  assign q2 = %s;\n", name);
    expect_fmt(line, "  always @* if (!en) %s<= d;\n", cname(fix.dlatch_n, name));
    expect_has("  (* gclk *) wire \\$gclk ;\n");
    expect_fmt(line, "  always @(posedge \\$gclk ) %s<= d;\n", cname(fix.ff, name));
    expect_fmt(line, "  initial %s= 1'b1;\n", name);
    expect_has("  reg r5;\n");
    expect_has("  always @(posedge clk) r5 <= d;\n");
}

/* Review Focus 5: INIT 2 and 3 give no initial value; 0 and 1 do. */
static void test_storage_init_undefined(void) {
    regs_fixture fix = build_regs();
    (void)write_ok();
    char name[NAME_BUF];
    char line[MSG_MAX];
    (void)snprintf(line, sizeof line, "initial %s", cname(fix.dlatch_p, name));
    expect_not(line);
    (void)snprintf(line, sizeof line, "initial %s", cname(fix.dlatch_n, name));
    expect_not(line);
    expect_not("initial r5");
    TEST_ASSERT_EQUAL_size_t(3, count_of(text, "initial"));
}

/* True when blif has a `.latch d <q> <init>` line (no type, no clock: Yosys's $_FF_). */
static bool has_global_latch(const char *blif, char init) {
    for (const char *at = strstr(blif, ".latch d "); at != NULL; at = strstr(at + 1, ".latch d ")) {
        const char *end = strchr(at, '\n');
        size_t blanks = 0;
        for (const char *chr = at; chr < end; chr++) {
            blanks += *chr == ' ';
        }
        if (blanks == 3 && end[-1] == init && end[-2] == ' ') {
            return true;
        }
    }
    return false;
}

/* Yosys maps the written storage back to the BLIF latches with the same types and inits. */
static void test_storage_yosys_latches(void) {
    (void)build_regs();
    (void)write_ok();
    char script[SCRIPT_BUF];
    (void)snprintf(script, sizeof script,
                   "read_verilog %s; proc; techmap; opt_clean; write_blif %s", out_path, out2_path);
    yosys_runs(script);
    const char *blif = slurp(out2_path, aux);
    expect_in(blif, ".latch d r0 re clk 0\n");
    expect_in(blif, " fe clk 1\n");
    expect_in(blif, " ah en 2\n");
    expect_in(blif, " al en 2\n");
    expect_in(blif, ".latch d r5 re clk 2\n");
    TEST_ASSERT_EQUAL_size_t(6, count_of(blif, ".latch d "));
    TEST_ASSERT_TRUE_MESSAGE(has_global_latch(blif, '1'), "no `.latch d <q> 1` for $_FF_");
}

/* --- vectors, concatenations and instances ------------------------------------------------- */

typedef struct inst_fixture {
    odin3_node_id u2, u3;
    odin3_pin_id dangling; /* u3's y[0] */
} inst_fixture;

/* sub: a[3:0] s -> y[1:0] = a[1:0] & a[3:2]. */
static odin3_celltype_id build_sub(void) {
    new_module("sub");
    odin3_wire_id net_a = in_vec("a", 4);
    (void)in_bit("s");
    odin3_wire_id net_y = out_vec("y", 2);
    odin3_netvec ports[] = {{(odin3_net_id[]){bit_of(net_a, 0), bit_of(net_a, 1)}, 2},
                            {(odin3_net_id[]){bit_of(net_a, 2), bit_of(net_a, 3)}, 2},
                            {(odin3_net_id[]){bit_of(net_y, 0), bit_of(net_y, 1)}, 2}};
    (void)binary("$and", false, ports);
    return odin3_module_celltype(module);
}

/* top: i[3:0] -> o[1:0], t; wire x[3:0] = ~i; three instances of sub. */
static inst_fixture build_instances(void) {
    odin3_celltype_id sub = build_sub();
    new_module("top");
    odin3_wire_id i = in_vec("i", 4);
    odin3_wire_id net_o = out_vec("o", 2);
    odin3_net_id net_t = out_bit("t");
    odin3_wire_spec xs = {intern("x"), 3, 0, false, {0}};
    odin3_wire_id wire_x = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &xs, NULL, &wire_x));
    odin3_net_id i_nets[4];
    odin3_net_id x_nets[4];
    odin3_net_id o_nets[2];
    odin3_netvec inv[] = {wire_nets(i, i_nets), wire_nets(wire_x, x_nets)};
    (void)unary("$not", inv);
    odin3_net_id zero = new_net(NULL);
    odin3_netvec cports[] = {NV1(zero)};
    (void)add_cell("$_CONST0_", NULL, 0, cports);
    odin3_node_spec spec = {sub, intern("u1"), {0}, NULL, 0};
    odin3_netvec u1[] = {{(odin3_net_id[]){zero, x_nets[0], i_nets[2], x_nets[1]}, 4},
                         NV1(i_nets[3]),
                         wire_nets(net_o, o_nets)};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, u1, &node));
    inst_fixture fix;
    spec.name = 0;
    odin3_netvec u2[] = {{(odin3_net_id[]){i_nets[0], i_nets[1], x_nets[2], x_nets[3]}, 4},
                         NV1((odin3_net_id){0}),
                         {(odin3_net_id[]){{0}, {0}}, 2}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, u2, &fix.u2));
    odin3_netvec u3[] = {
        wire_nets(wire_x, x_nets), NV1(i_nets[0]), {(odin3_net_id[]){{0}, net_t}, 2}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, u3, &fix.u3));
    fix.dangling = odin3_node_port(module, fix.u3, 2).first;
    return fix;
}

static void test_vectors_msb_first(void) {
    inst_fixture fix = build_instances();
    (void)write_ok();
    char name[NAME_BUF];
    char line[MSG_MAX];
    expect_has("module sub (\n  input [3:0] a,\n  input s,\n  output [1:0] y\n);\n");
    expect_has("  assign y = a[1:0] & a[3:2];\n");
    expect_has("  wire [3:0] x;\n");
    expect_has("  assign x = ~i;\n");
    expect_has("  sub u1 (\n    .a({x[1], i[2], x[0], 1'b0}),\n    .s(i[3]),\n    .y(o)\n  );\n");
    expect_fmt(line, "  sub %s(\n    .a({x[3:2], i[1:0]}),\n    .s(),\n    .y()\n  );\n",
               cname(fix.u2, name));
    (void)snprintf(name, sizeof name, "\\$p%u ", fix.dangling.v);
    expect_fmt(line, "  wire %s;\n", name);
    expect_fmt(line, "    .y({t, %s})\n", name);
    expect_has("    .a(x),\n    .s(i[0]),\n");
    expect_not("CONST");
}

/* --- word cells ---------------------------------------------------------------------------- */

typedef struct words_fixture {
    odin3_node_id dff, adff, sdff, dffe;
} words_fixture;

typedef struct words_ins {
    odin3_net_id a[4], b[4], sel[2];
    odin3_net_id s, clk, en, rst;
} words_ins;

static odin3_netvec slice(const odin3_net_id *nets, uint32_t count) {
    return (odin3_netvec){nets, count};
}

/* An output port of width bits; its nets into nets. */
static odin3_netvec out_nets(const char *name, uint32_t width, odin3_net_id *nets) {
    return wire_nets(out_vec(name, width), nets);
}

static void build_word_logic(const words_ins *in) {
    odin3_net_id sum[4];
    odin3_net_id dif[4];
    odin3_net_id shr[4];
    odin3_net_id inv[4];
    odin3_net_id mx[4];
    odin3_net_id tb[4];
    odin3_net_id pm[2];
    odin3_netvec add[] = {slice(in->a, 4), slice(in->b, 4), out_nets("sum", 4, sum)};
    (void)binary("$add", true, add);
    odin3_netvec sub[] = {slice(in->a, 4), slice(in->b, 4), out_nets("dif", 4, dif)};
    (void)binary("$sub", false, sub);
    odin3_netvec sshr[] = {slice(in->a, 4), slice(in->b, 2), out_nets("shr", 4, shr)};
    odin3_value sp[5] = {odin3_value_int(1), odin3_value_int(0), odin3_value_int(4),
                         odin3_value_int(2), odin3_value_int(4)};
    (void)add_cell("$sshr", sp, 5, sshr);
    odin3_netvec eq[] = {slice(in->a, 4), slice(in->b, 4), NV1(out_bit("eq"))};
    (void)binary("$eq", false, eq);
    odin3_netvec red[] = {slice(in->a, 4), NV1(out_bit("red"))};
    (void)unary("$reduce_or", red);
    odin3_netvec neg[] = {slice(in->a, 4), out_nets("inv", 4, inv)};
    (void)unary("$not", neg);
    odin3_value w4[1] = {odin3_value_int(4)};
    odin3_netvec mux[] = {slice(in->a, 4), slice(in->b, 4), NV1(in->s), out_nets("mx", 4, mx)};
    (void)add_cell("$mux", w4, 1, mux);
    odin3_value pw[2] = {odin3_value_int(2), odin3_value_int(2)};
    odin3_netvec pmux[] = {slice(in->a, 2), slice(in->b, 4), slice(in->sel, 2),
                           out_nets("pm", 2, pm)};
    (void)add_cell("$pmux", pw, 2, pmux);
    odin3_netvec tri[] = {slice(in->a, 4), NV1(in->en), out_nets("tb", 4, tb)};
    (void)add_cell("$tribuf", w4, 1, tri);
}

static const uint8_t RESET_01[2] = {1, 0}; /* LSB first: 2'b01 */
static const uint8_t RESET_10[2] = {0, 1}; /* 2'b10 */

static words_fixture build_word_regs(const words_ins *in) {
    words_fixture fix;
    odin3_net_id q1[4];
    odin3_net_id q2[2];
    odin3_net_id q3[2];
    odin3_net_id q4[2];
    odin3_value dp[2] = {odin3_value_int(4), odin3_value_int(1)};
    odin3_netvec dff[] = {NV1(in->clk), slice(in->a, 4), out_nets("q1", 4, q1)};
    fix.dff = add_cell("$dff", dp, 2, dff);
    odin3_value ap[4] = {odin3_value_int(2),
                         odin3_value_int(0),
                         odin3_value_int(0),
                         {ODIN3_VAL_BITS, 0, RESET_01, 2, 0, 0}};
    odin3_netvec adff[] = {NV1(in->clk), NV1(in->rst), slice(in->a, 2), out_nets("q2", 2, q2)};
    fix.adff = add_cell("$adff", ap, 4, adff);
    odin3_value sp[4] = {odin3_value_int(2),
                         odin3_value_int(1),
                         odin3_value_int(1),
                         {ODIN3_VAL_BITS, 0, RESET_10, 2, 0, 0}};
    odin3_netvec sdff[] = {NV1(in->clk), NV1(in->rst), slice(in->b, 2), out_nets("q3", 2, q3)};
    fix.sdff = add_cell("$sdff", sp, 4, sdff);
    odin3_value ep[3] = {odin3_value_int(2), odin3_value_int(1), odin3_value_int(0)};
    odin3_netvec dffe[] = {NV1(in->clk), NV1(in->en), slice(in->a + 2, 2), out_nets("q4", 2, q4)};
    fix.dffe = add_cell("$dffe", ep, 3, dffe);
    return fix;
}

/* A signed wire sw = ~b read whole by an unsigned $add (must not be read as signed). */
static void build_signed_wire(const words_ins *in) {
    odin3_wire_spec spec = {intern("sw"), 3, 0, true, {0}};
    odin3_wire_id sw = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &sw));
    odin3_net_id sw_nets[4];
    odin3_net_id sum2[4];
    odin3_netvec inv[] = {slice(in->b, 4), wire_nets(sw, sw_nets)};
    (void)unary("$not", inv);
    odin3_netvec add[] = {slice(sw_nets, 4), slice(in->a, 4), out_nets("sum2", 4, sum2)};
    (void)binary("$add", false, add);
    odin3_net_id one = new_net("one");
    odin3_netvec cports[] = {NV1(one)};
    (void)add_cell("$_CONST1_", NULL, 0, cports);
}

static words_fixture build_words(void) {
    new_module("words");
    words_ins in;
    odin3_wire_id net_a = in_vec("a", 4);
    odin3_wire_id net_b = in_vec("b", 4);
    odin3_wire_id sel = in_vec("sel", 2);
    (void)wire_nets(net_a, in.a);
    (void)wire_nets(net_b, in.b);
    (void)wire_nets(sel, in.sel);
    in.s = in_bit("s");
    in.clk = in_bit("clk");
    in.en = in_bit("en");
    in.rst = in_bit("rst");
    build_word_logic(&in);
    build_signed_wire(&in);
    return build_word_regs(&in);
}

static void test_word_logic(void) {
    (void)build_words();
    (void)write_ok();
    expect_has("  assign sum = $signed(a) + $signed(b);\n");
    expect_has("  assign dif = a - b;\n");
    expect_has("  assign shr = $signed(a) >>> b[1:0];\n");
    expect_has("  assign eq = a == b;\n");
    expect_has("  assign red = |a;\n");
    expect_has("  assign inv = ~a;\n");
    expect_has("  assign mx = s ? b : a;\n");
    expect_has(
        "  assign pm = |sel ? (({2{sel[0]}} & b[1:0]) | ({2{sel[1]}} & b[3:2])) : a[1:0];\n");
    expect_has("  assign tb = en ? a : 4'bzzzz;\n");
    expect_has("  wire signed [3:0] sw;\n");
    expect_has("  assign sum2 = sw[3:0] + a;\n");
    expect_has("  wire one;\n");
    expect_has("  assign one = 1'b1;\n");
}

static void test_word_regs(void) {
    words_fixture fix = build_words();
    (void)write_ok();
    char name[NAME_BUF];
    char line[MSG_MAX];
    expect_fmt(line, "  reg [3:0] %s;\n", cname(fix.dff, name));
    expect_fmt(line, "  always @(posedge clk) %s<= a;\n", name);
    expect_fmt(line, "  assign q1 = %s;\n", name);
    (void)cname(fix.adff, name);
    (void)snprintf(line, sizeof line,
                   "  always @(negedge clk, negedge rst) if (!rst) %s<= 2'b01; else %s<= a[1:0];\n",
                   name, name);
    expect_has(line);
    (void)cname(fix.sdff, name);
    (void)snprintf(line, sizeof line,
                   "  always @(posedge clk) if (rst) %s<= 2'b10; else %s<= b[1:0];\n", name, name);
    expect_has(line);
    expect_fmt(line, "  always @(posedge clk) if (!en) %s<= a[3:2];\n", cname(fix.dffe, name));
    expect_not("initial");
}

/* Yosys reads the word-level output (operators, processes) without complaint. */
static void test_word_yosys_reads(void) {
    (void)build_words();
    (void)write_ok();
    char script[SCRIPT_BUF];
    (void)snprintf(script, sizeof script, "read_verilog %s; proc; check -assert", out_path);
    yosys_runs(script);
}

/* --- black boxes and hard cells ------------------------------------------------------------ */

static const odin3_port_def ADDER_PORTS[] = {
    {"a", ODIN3_DIR_IN, true, 1, NULL, NULL},       {"b", ODIN3_DIR_IN, true, 1, NULL, NULL},
    {"cin", ODIN3_DIR_IN, true, 1, NULL, NULL},     {"cout", ODIN3_DIR_OUT, true, 1, NULL, NULL},
    {"sumout", ODIN3_DIR_OUT, true, 1, NULL, NULL},
};
static const odin3_port_def BBV_PORTS[] = {
    {"d", ODIN3_DIR_IN, false, 4, NULL, NULL},
    {"q", ODIN3_DIR_OUT, true, 1, NULL, NULL},
};
static const odin3_port_def HMUL_PORTS[] = {
    {"A", ODIN3_DIR_IN, false, 0, "W", NULL},
    {"Y", ODIN3_DIR_OUT, false, 0, "W", NULL},
};
static const uint8_t MODE_DEFAULT[2] = {1, 0}; /* 2'b01 */

static odin3_celltype_id declare(const odin3_celltype_def *def) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, def, &id));
    return id;
}

/* A HARD type hmul: parameters W (INT 2), MODE (BITS 2'b01), TAG (STRING ""); A, Y of width W. */
static odin3_celltype_id add_hmul(void) {
    odin3_param_def params[3] = {
        {"W", ODIN3_VAL_INT, {ODIN3_VAL_INT, 2, NULL, 0, 0, 0}},
        {"MODE", ODIN3_VAL_BITS, {ODIN3_VAL_BITS, 0, MODE_DEFAULT, 2, 0, 0}},
        {"TAG", ODIN3_VAL_STRING, {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0}},
    };
    odin3_celltype_def def = {"hmul", ODIN3_GRAN_HARD, 0, HMUL_PORTS, 2, params, 3, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    return id;
}

static odin3_node_id build_blackboxes(void) {
    odin3_celltype_def adder = {"adder", ODIN3_GRAN_BLACKBOX, 0, ADDER_PORTS, 5, NULL, 0, NULL,
                                NULL};
    odin3_celltype_def bbv = {"bbv", ODIN3_GRAN_BLACKBOX, 0, BBV_PORTS, 2, NULL, 0, NULL, NULL};
    (void)declare(&adder);
    (void)declare(&bbv);
    (void)declare(&adder);
    (void)add_hmul();
    new_module("bbtop");
    odin3_wire_id i = in_vec("i", 4);
    odin3_net_id net_o = out_bit("o");
    odin3_wire_id wire_m = out_vec("m", 3);
    odin3_net_id i_nets[4];
    odin3_net_id m_nets[3];
    (void)wire_nets(i, i_nets);
    odin3_netvec add[] = {NV1(i_nets[0]), NV1(i_nets[1]), NV1(i_nets[2]), NV1((odin3_net_id){0}),
                          NV1(net_o)};
    (void)add_cell_args((cell_args){"adder", "fa", NULL, 0, {0}}, add);
    odin3_netvec bbvp[] = {slice(i_nets, 4), NV1((odin3_net_id){0})};
    (void)add_cell_args((cell_args){"bbv", "bb1", NULL, 0, {0}}, bbvp);
    uint32_t tag = intern("x\"y");
    odin3_value hp[3] = {odin3_value_int(3),
                         {ODIN3_VAL_BITS, 0, (const uint8_t[]){0, 1}, 2, 0, 0},
                         {ODIN3_VAL_STRING, 0, NULL, 0, tag, 0}};
    odin3_netvec mul[] = {slice(i_nets, 3), wire_nets(wire_m, m_nets)};
    return add_cell("hmul", hp, 3, mul);
}

static void test_blackbox_stubs(void) {
    odin3_node_id hmul = build_blackboxes();
    (void)write_ok();
    char name[NAME_BUF];
    char line[MSG_MAX];
    expect_has("  adder fa (\n    .a(i[0]),\n    .b(i[1]),\n    .cin(i[2]),\n    .cout(),\n"
               "    .sumout(o)\n  );\n");
    expect_has("  bbv bb1 (\n    .d(i),\n    .q()\n  );\n");
    expect_fmt(line, "  hmul #(.W(3), .MODE(2'b10), .TAG(\"x\\\"y\")) %s(\n    .A(i[2:0]),\n",
               cname(hmul, name));
    const char *adder = "(* blackbox *)\nmodule adder (\n  input a,\n  input b,\n  input cin,\n"
                        "  output cout,\n  output sumout\n);\nendmodule\n";
    const char *bbv = "(* blackbox *)\nmodule bbv (\n  input [3:0] d,\n  output q\n);\nendmodule\n";
    const char *stub = "(* blackbox *)\nmodule hmul #(\n  parameter W = 2,\n  parameter MODE = "
                       "2'b01,\n  parameter TAG = \"\"\n) (\n  input [W-1:0] A,\n  output [W-1:0] "
                       "Y\n);\nendmodule\n";
    expect_has(adder);
    expect_has(bbv);
    expect_has(stub);
    TEST_ASSERT_EQUAL_size_t(1, count_of(text, "module adder"));
    TEST_ASSERT_TRUE(strstr(text, adder) < strstr(text, bbv));
    TEST_ASSERT_TRUE(strstr(text, bbv) < strstr(text, stub));
    TEST_ASSERT_TRUE(strstr(text, "module bbtop") < strstr(text, adder));
}

/* $memrd and other built-ins without a Verilog form are generic instances with every parameter
 * (Yosys reads them as its internal cells; Icarus has no definition, so no compile check). */
static void test_generic_builtin_instance(void) {
    new_module("mem");
    odin3_net_id clk = in_bit("clk");
    odin3_wire_id addr = in_vec("addr", 2);
    odin3_wire_id data = out_vec("data", 4);
    odin3_net_id addr_nets[2];
    odin3_net_id data_nets[4];
    odin3_value params[6] = {{ODIN3_VAL_STRING, 0, NULL, 0, intern("\\ram"), 0},
                             odin3_value_int(2),
                             odin3_value_int(4),
                             odin3_value_int(0),
                             odin3_value_int(0),
                             odin3_value_int(0)};
    odin3_netvec ports[] = {NV1(clk), NV1((odin3_net_id){0}), wire_nets(addr, addr_nets),
                            wire_nets(data, data_nets)};
    odin3_node_id node = add_cell("$memrd", params, 6, ports);
    (void)write_text();
    char name[NAME_BUF];
    char line[MSG_MAX];
    expect_fmt(line,
               "  \\$memrd #(.MEMID(\"\\\\ram\"), .ABITS(2), .WIDTH(4), .CLK_ENABLE(0), "
               ".CLK_POLARITY(0), .TRANSPARENT(0)) %s(\n    .CLK(clk),\n    .EN(),\n"
               "    .ADDR(addr),\n    .DATA(data)\n  );\n",
               cname(node, name));
    expect_not("(* blackbox *)");
}

/* --- aliases, feed-throughs and names ------------------------------------------------------ */

/* ft: y is a (one net), z = ~a, z2 on z's net, wire t aliases a. */
static void build_feedthrough(void) {
    new_module("ft");
    odin3_net_id net_a = in_bit("a");
    odin3_net_id net_y = out_bit("y");
    odin3_net_id net_z = out_bit("z");
    odin3_net_id z2 = out_bit("z2");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){net_a, net_y}));
    (void)gate1("$_NOT_", (io_pair){net_a, net_z});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){net_z, z2}));
    odin3_wire_spec spec = {intern("t"), 0, 0, false, {0}};
    odin3_wire_id net_t = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, &net_a, &net_t));
}

static void test_aliases_and_feedthrough(void) {
    build_feedthrough();
    (void)write_ok();
    expect_has("  wire t;\n");
    expect_has("  assign y = a;\n");
    expect_has("  assign z2 = z;\n");
    expect_has("  assign t = a;\n");
    expect_has("  assign z = ~a;\n");
}

static void test_two_input_ports_on_one_net_refused(void) {
    new_module("bad");
    odin3_net_id net_a = in_bit("a");
    odin3_net_id net_b = in_bit("b");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){net_a, net_b}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, out_path, NULL));
    TEST_ASSERT_FALSE(file_exists(out_path));
}

static void test_unwritable_port_refused(void) {
    new_module("bad");
    (void)in_bit("a b");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, out_path, NULL));
    TEST_ASSERT_FALSE(file_exists(out_path));
}

typedef struct names_fixture {
    odin3_net_id unnamed, spaced;
    odin3_node_id reg_x;
} names_fixture;

/* nm: a -> chain of inverters over nets: unnamed, "$n<unnamed>", "wire", "a b", "x"; then a
 * $_DFF_P_ named x (its reg name collides with net x) drives y. */
static names_fixture build_names(void) {
    new_module("nm");
    odin3_net_id net_a = in_bit("a");
    odin3_net_id net_y = out_bit("y");
    names_fixture fix;
    fix.unnamed = new_net(NULL);
    char taken[NAME_BUF];
    (void)snprintf(taken, sizeof taken, "$n%u", fix.unnamed.v);
    odin3_net_id n2 = new_net(taken);
    odin3_net_id n3 = new_net("wire");
    fix.spaced = new_net("a b");
    odin3_net_id n5 = new_net("x");
    (void)gate1("$_NOT_", (io_pair){net_a, fix.unnamed});
    (void)gate1("$_NOT_", (io_pair){fix.unnamed, n2});
    (void)gate1("$_NOT_", (io_pair){n2, n3});
    (void)gate1("$_NOT_", (io_pair){n3, fix.spaced});
    (void)gate1("$_NOT_", (io_pair){fix.spaced, n5});
    odin3_value init[1] = {odin3_value_int(3)};
    odin3_netvec ports[] = {NV1(net_a), NV1(n5), NV1(net_y)};
    fix.reg_x = add_cell_args((cell_args){"$_DFF_P_", "x", init, 1, {0}}, ports);
    return fix;
}

static void test_names_unique_across_kinds(void) {
    names_fixture fix = build_names();
    (void)write_ok();
    char line[MSG_MAX];
    (void)snprintf(line, sizeof line, "  assign \\$n%u$1 = ~a;\n", fix.unnamed.v);
    expect_has(line);
    (void)snprintf(line, sizeof line, "  assign \\$n%u = ~\\$n%u$1 ;\n", fix.unnamed.v,
                   fix.unnamed.v);
    expect_has(line);
    (void)snprintf(line, sizeof line, "  assign \\wire = ~\\$n%u ;\n", fix.unnamed.v);
    expect_has(line);
    (void)snprintf(line, sizeof line, "  assign \\$n%u = ~\\wire ;\n", fix.spaced.v);
    expect_has(line);
    (void)snprintf(line, sizeof line, "  assign x = ~\\$n%u ;\n", fix.spaced.v);
    expect_has(line);
    char name[NAME_BUF];
    expect_fmt(line, "  always @(posedge a) %s<= x;\n", cname(fix.reg_x, name));
    expect_has("  wire x;\n");
}

/* --- provenance ---------------------------------------------------------------------------- */

static void test_provenance_comments(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("blif"), &ctx));
    odin3_srcloc loc7 = {intern("in.blif"), 7, 1, 7, 1};
    odin3_srcloc loc9 = {intern("in.blif"), 9, 1, 9, 1};
    odin3_prov_id p7 = {0};
    odin3_prov_id p9 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_imported(&ctx, &(odin3_prov_origin){&loc7, 1, 0, 0}, &p7));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_imported(&ctx, &(odin3_prov_origin){&loc9, 1, 0, 0}, &p9));
    odin3_pass_ctx opt;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("opt"), &opt));
    odin3_prov_begin_op(&opt);
    odin3_prov_id derived = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(&opt, (odin3_prov_list){&p9, 1}, &derived));
    new_module_prov("pv", p7);
    odin3_wire_id net_a = add_port_prov((port_args){"a", ODIN3_DIR_IN, 1, true}, p9);
    odin3_net_id net_y = out_bit("y");
    odin3_net_id wire_m = new_net_prov("m", p7);
    odin3_netvec first[] = {NV1(bit_of(net_a, 0)), NV1(wire_m)};
    (void)add_cell_args((cell_args){"$_NOT_", NULL, NULL, 0, p7}, first);
    odin3_netvec second[] = {NV1(wire_m), NV1(net_y)};
    (void)add_cell_args((cell_args){"$_NOT_", NULL, NULL, 0, derived}, second);
    (void)write_ok();
    expect_has("module pv (  // in.blif:7\n  input a,  // in.blif:9\n  output y\n);\n");
    expect_has("  wire m;  // in.blif:7\n");
    expect_has("  assign m = ~a;  // in.blif:7\n");
    expect_has("  assign y = ~m;  // in.blif:9\n");
}

/* --- whole-file properties and failures ---------------------------------------------------- */

/* Every fixture in one design (the stubs of build_blackboxes come last). */
static void build_all(void) {
    build_escapes();
    build_sops();
    (void)build_regs();
    (void)build_instances();
    (void)build_words();
    build_feedthrough();
    (void)build_names();
    (void)build_blackboxes();
}

static void test_all_fixtures_compile_and_deterministic(void) {
    build_all();
    (void)write_ok();
    static char first[TEXT_MAX];
    memcpy(first, text, sizeof first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, out2_path, NULL));
    TEST_ASSERT_EQUAL_STRING(first, slurp(out2_path, text));
}

static void test_portless_module(void) {
    new_module("empty");
    (void)write_ok();
    TEST_ASSERT_EQUAL_STRING("module empty;\nendmodule\n", text);
}

static void test_empty_design(void) {
    (void)write_text();
    TEST_ASSERT_EQUAL_STRING("", text);
}

static void test_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(NULL, out_path, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(2, errors);
}

static void test_io_errors(void) {
    build_escapes();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO,
                          odin3_verilog_write(design, "/nonexistent-dir/out.v", NULL));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "/nonexistent-dir/out.v: cannot open"));
}

/* A destination whose directory refuses the temporary file: IO, and the device is left alone. */
static void test_write_failure(void) {
    if (!file_exists("/dev/full")) {
        TEST_IGNORE_MESSAGE("/dev/full not available");
    }
    build_escapes();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_verilog_write(design, "/dev/full", NULL));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "/dev/full: cannot open"));
    TEST_ASSERT_TRUE(file_exists("/dev/full"));
}

static const char *const BASELINE = "previous contents\n";

/* The destination still holds BASELINE and no temporary file is left. */
static void expect_baseline_kept(void) {
    TEST_ASSERT_EQUAL_STRING(BASELINE, slurp(out_path, aux));
    DIR *dir = opendir(out_dir);
    TEST_ASSERT_NOT_NULL(dir);
    for (const struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        TEST_ASSERT_NULL(strstr(ent->d_name, "out.v.")); /* the writer's "out.v.XXXXXX" */
    }
    (void)closedir(dir);
}

static void put_baseline(void) {
    FILE *file = fopen(out_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(BASELINE), fwrite(BASELINE, 1, strlen(BASELINE), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* A refusal found halfway through leaves the existing destination as it was. */
static void test_refusal_keeps_destination(void) {
    build_escapes();
    new_module("mixed");
    odin3_wire_id net_a = in_vec("a", 2);
    odin3_net_id a_nets[2];
    (void)sop(wire_nets(net_a, a_nets),
              "10"
              "1"
              "01"
              "0",
              out_bit("y"));
    put_baseline();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, out_path, NULL));
    expect_baseline_kept();
}

/* A real write error (file size limit, EFBIG) is a located IO failure; the destination stays. */
static void test_write_error_keeps_destination(void) {
    build_all();
    put_baseline();
    struct rlimit old;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    TEST_ASSERT_TRUE(prev != SIG_ERR);
    struct rlimit small = {256, old.rlim_max};
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &small));
    odin3_status st = odin3_verilog_write(design, out_path, NULL);
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &old));
    TEST_ASSERT_TRUE(signal(SIGXFSZ, prev) != SIG_ERR);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
    TEST_ASSERT_NOT_NULL(strstr(last_error, "write failed"));
    expect_baseline_kept();
}

/* One write with the allocation after the first `allowed` failing; a failure must be NO_MEMORY
 * and leave the destination as it was. */
static odin3_status write_failing_after(long allowed) {
    odin3_util_set_alloc_fail_after(allowed);
    odin3_status st = odin3_verilog_write(design, out_path, NULL);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        expect_baseline_kept();
    }
    return st;
}

/* Every allocation failure is NO_MEMORY and keeps the old file; enough allocations succeed. */
static void test_out_of_memory_sweep(void) {
    build_all();
    put_baseline();
    long tries = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (; st != ODIN3_OK && tries < OOM_LIMIT; tries++) {
        st = write_failing_after(tries);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(tries > 10);
    static char first[TEXT_MAX];
    memcpy(first, slurp(out_path, text), sizeof first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, out2_path, NULL));
    TEST_ASSERT_EQUAL_STRING(first, slurp(out2_path, text));
}

/* --- fix round 1 --------------------------------------------------------------------------- */

static const odin3_port_def HS_PORTS[] = {
    {"A", ODIN3_DIR_IN, true, 1, NULL, NULL},
    {"Z", ODIN3_DIR_IN, false, 0, NULL, NULL}, /* zero-width */
};

/* String parameters are byte-exact: no blank is ever dropped inside a literal or a comment. */
static void test_string_blanks_exact(void) {
    odin3_param_def params[1] = {
        {"TAG", ODIN3_VAL_STRING, {ODIN3_VAL_STRING, 0, NULL, 0, intern("dflt  two  spaces"), 0}},
    };
    odin3_celltype_def def = {"hs", ODIN3_GRAN_HARD, 0, HS_PORTS, 2, params, 1, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("r"), &ctx));
    odin3_srcloc loc = {intern("s.v"), 3, 1, 3, 1};
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_source(&ctx, &(odin3_prov_origin){&loc, 1, 0, 0}, &prov));
    new_module("strs");
    odin3_net_id net_a = in_bit("a");
    (void)add_port_prov((port_args){"q.r", ODIN3_DIR_OUT, 1, true}, prov);
    odin3_value tag = {ODIN3_VAL_STRING, 0, NULL, 0, intern(" x  y   z "), 0};
    odin3_netvec ports[] = {NV1(net_a), {NULL, 0}};
    (void)add_cell_args((cell_args){"hs", "u", &tag, 1, {0}}, ports);
    (void)write_ok();
    expect_has("  hs #(.TAG(\" x  y   z \")) u (\n    .A(a),\n    .Z()\n  );\n");
    expect_has("  parameter TAG = \"dflt  two  spaces\"\n");
    expect_has("  output \\q.r   // s.v:3\n");
}

/* A zero-width black-box port is declared without a range (never [-1:0]). */
static void test_zero_width_stub_port(void) {
    odin3_celltype_def def = {"zw", ODIN3_GRAN_BLACKBOX, 0, HS_PORTS, 2, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &id));
    (void)write_ok();
    expect_has("(* blackbox *)\nmodule zw (\n  input A,\n  input Z\n);\nendmodule\n");
    expect_not("[-1:0]");
}

/* A backtick (Icarus expands macros inside escaped names) is unwritable: generated name for a
 * net, refusal for a port. */
static void test_backtick_names(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_VERILOG_UNWRITABLE, kind_of("m`define"));
    new_module("bt");
    odin3_net_id net_a = in_bit("a");
    odin3_net_id tick = new_net("m`x");
    (void)gate1("$_NOT_", (io_pair){net_a, tick});
    (void)gate1("$_NOT_", (io_pair){tick, out_bit("y")});
    (void)write_ok();
    char line[MSG_MAX];
    (void)snprintf(line, sizeof line, "  assign \\$n%u = ~a;\n", tick.v);
    expect_has(line);
    expect_not("`");
    new_module("bt2");
    (void)in_bit("p`q");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, out_path, NULL));
    TEST_ASSERT_NOT_NULL(strstr(last_error, "cannot be written"));
}

/* A net named like a bit of a declared vector (`a[0]` beside port a[1:0]) gets a generated name:
 * Yosys write_blif would flatten both to the same BLIF name. */
static void test_bit_select_lookalike_renamed(void) {
    new_module("lk");
    odin3_wire_id net_a = in_vec("a", 2);
    odin3_net_id look = new_net("a[0]");
    odin3_net_id other = new_net("b[0]");
    (void)gate1("$_NOT_", (io_pair){bit_of(net_a, 1), look});
    (void)gate2("$_AND_", look, bit_of(net_a, 0), other);
    (void)gate1("$_BUF_", (io_pair){other, out_bit("y")});
    (void)write_ok();
    char line[MSG_MAX];
    (void)snprintf(line, sizeof line, "  assign \\$n%u = ~a[1];\n", look.v);
    expect_has(line);
    (void)snprintf(line, sizeof line, "  assign \\b[0] = \\$n%u & a[0];\n", look.v);
    expect_has(line);
}

static const char *const PMUX_TB = "module tb;\n"
                                   "  reg [1:0] a;\n"
                                   "  reg [3:0] b;\n"
                                   "  reg [1:0] s;\n"
                                   "  wire [1:0] y;\n"
                                   "  pmx dut(.a(a), .b(b), .s(s), .y(y));\n"
                                   "  initial begin\n"
                                   "    a = 2'b11; b = 4'b1001; s = 2'b00;\n"
                                   "    #1 $display(\"%b\", y);\n"
                                   "    a = 2'b00; s = 2'b01;\n"
                                   "    #1 $display(\"%b\", y);\n"
                                   "    s = 2'b10;\n"
                                   "    #1 $display(\"%b\", y);\n"
                                   "    s = 2'b11;\n"
                                   "    #1 $display(\"%b\", y);\n"
                                   "  end\n"
                                   "endmodule\n";

/* $pmux: no select bit -> A; one -> its B slice; several -> the OR of their slices (Yosys
 * gate-level semantics, as the 1E simulator). */
static void test_pmux_simulates(void) {
    new_module("pmx");
    odin3_net_id a_nets[2];
    odin3_net_id b_nets[4];
    odin3_net_id s_nets[2];
    odin3_net_id y_nets[2];
    odin3_netvec ports[] = {wire_nets(in_vec("a", 2), a_nets), wire_nets(in_vec("b", 4), b_nets),
                            wire_nets(in_vec("s", 2), s_nets), wire_nets(out_vec("y", 2), y_nets)};
    odin3_value params[2] = {odin3_value_int(2), odin3_value_int(2)};
    (void)add_cell("$pmux", params, 2, ports);
    (void)write_ok();
    put_testbench(PMUX_TB);
    char *compile[] = {"iverilog", "-g2005", "-o", vvp_path, tb_path, out_path, NULL};
    int rc = run_tool(compile, NULL);
    if (rc == TOOL_MISSING) {
        TEST_IGNORE_MESSAGE("iverilog not found: simulation skipped");
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    char *sim[] = {"vvp", "-n", vvp_path, NULL};
    TEST_ASSERT_EQUAL_INT(0, run_tool(sim, aux_path));
    TEST_ASSERT_EQUAL_STRING("11\n01\n10\n11\n", slurp(aux_path, aux));
}

/* --- name styles ---------------------------------------------------------------------------- */

typedef struct prov_fixture {
    odin3_net_id unnamed;
} prov_fixture;

/* pn: net m (prov top/u1 @ in.v:7), an unnamed net (same prov), a net n without prov, an
 * instance u2 of sub2 (prov in.v:9, no hierarchy). */
static prov_fixture build_prov_names(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("fe"), &ctx));
    odin3_srcloc loc7 = {intern("in.v"), 7, 1, 7, 1};
    odin3_srcloc loc9 = {intern("in.v"), 9, 1, 9, 1};
    odin3_prov_id p7 = {0};
    odin3_prov_id p9 = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_prov_source(&ctx, &(odin3_prov_origin){&loc7, 1, 0, intern("top/u1")}, &p7));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_source(&ctx, &(odin3_prov_origin){&loc9, 1, 0, 0}, &p9));
    new_module("sub2");
    odin3_net_id sub_in = in_bit("i");
    (void)gate1("$_BUF_", (io_pair){sub_in, out_bit("o")});
    odin3_celltype_id sub = odin3_module_celltype(module);
    new_module("pn");
    odin3_net_id net_a = in_bit("a");
    prov_fixture fix;
    odin3_net_id named = new_net_prov("m", p7);
    fix.unnamed = new_net_prov(NULL, p7);
    odin3_net_id plain = new_net("n");
    (void)gate1("$_NOT_", (io_pair){net_a, named});
    (void)gate1("$_NOT_", (io_pair){named, fix.unnamed});
    (void)gate1("$_NOT_", (io_pair){fix.unnamed, plain});
    odin3_node_spec spec = {sub, intern("u2"), p9, NULL, 0};
    odin3_netvec ports[] = {NV1(plain), NV1(out_bit("y"))};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return fix;
}

static void test_name_style_short(void) {
    prov_fixture fix = build_prov_names();
    odin3_verilog_opts opts = {ODIN3_VERILOG_NAMES_SHORT};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, out_path, &opts));
    (void)slurp(out_path, text);
    (void)iverilog_compiles(out_path);
    char line[MSG_MAX];
    expect_has("  wire m;  // in.v:7\n");
    (void)snprintf(line, sizeof line, "  wire \\$n%u ;  // in.v:7\n", fix.unnamed.v);
    expect_has(line);
    expect_has("  wire n;\n");
    expect_has("  sub2 u2 (  // in.v:9\n");
}

/* PROVENANCE: hier/name@file:line for objects with a location; ports and the rest unchanged. */
static void test_name_style_provenance(void) {
    prov_fixture fix = build_prov_names();
    odin3_verilog_opts opts = {ODIN3_VERILOG_NAMES_PROVENANCE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, out_path, &opts));
    (void)slurp(out_path, text);
    (void)iverilog_compiles(out_path);
    char line[MSG_MAX];
    expect_has("  wire \\top/u1/m@in.v:7 ;  // in.v:7\n");
    (void)snprintf(line, sizeof line, "  wire \\top/u1/$n%u@in.v:7 ;  // in.v:7\n", fix.unnamed.v);
    expect_has(line);
    expect_has("  wire n;\n");
    expect_has("  sub2 \\u2@in.v:9 (  // in.v:9\n");
    expect_has("  assign \\top/u1/m@in.v:7 = ~a;\n");
    expect_has("module pn (\n  input a,\n  output y\n);\n");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ident_kinds);
    RUN_TEST(test_ident_unwritable);
    RUN_TEST(test_keywords);
    RUN_TEST(test_ident_append);
    RUN_TEST(test_escaped_names);
    RUN_TEST(test_escaped_names_yosys_round_trip);
    RUN_TEST(test_gates);
    RUN_TEST(test_sop_text);
    RUN_TEST(test_sop_simulates);
    RUN_TEST(test_sop_mixed_cover_refused);
    RUN_TEST(test_storage_text);
    RUN_TEST(test_storage_init_undefined);
    RUN_TEST(test_storage_yosys_latches);
    RUN_TEST(test_vectors_msb_first);
    RUN_TEST(test_word_logic);
    RUN_TEST(test_word_regs);
    RUN_TEST(test_word_yosys_reads);
    RUN_TEST(test_blackbox_stubs);
    RUN_TEST(test_generic_builtin_instance);
    RUN_TEST(test_aliases_and_feedthrough);
    RUN_TEST(test_two_input_ports_on_one_net_refused);
    RUN_TEST(test_unwritable_port_refused);
    RUN_TEST(test_names_unique_across_kinds);
    RUN_TEST(test_provenance_comments);
    RUN_TEST(test_all_fixtures_compile_and_deterministic);
    RUN_TEST(test_portless_module);
    RUN_TEST(test_empty_design);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_io_errors);
    RUN_TEST(test_write_failure);
    RUN_TEST(test_refusal_keeps_destination);
    RUN_TEST(test_write_error_keeps_destination);
    RUN_TEST(test_out_of_memory_sweep);
    RUN_TEST(test_string_blanks_exact);
    RUN_TEST(test_zero_width_stub_port);
    RUN_TEST(test_backtick_names);
    RUN_TEST(test_bit_select_lookalike_renamed);
    RUN_TEST(test_pmux_simulates);
    RUN_TEST(test_name_style_short);
    RUN_TEST(test_name_style_provenance);
    return UNITY_END();
}
