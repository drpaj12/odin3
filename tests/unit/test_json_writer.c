/*
 * test_json_writer.c — unit tests for odin3_json_write: schema shape per construct, bit numbering,
 * $sop -> $lut / $sop, latch INIT, determinism, I/O failure, and net_a Yosys read_json round trip.
 */
#include "backends/json/writer.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { OUT_BUF = 64, MAX_COVER = 64, SEVEN = 7 };

extern char **environ;

static odin3_design *design;
static odin3_pass_ctx reader;

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(name), &id));
    return id;
}

static odin3_prov_id prov_at(uint32_t line) {
    odin3_srcloc loc = {intern("t.v"), line, 1, line, 1};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&reader, &origin, &id));
    return id;
}

void setUp(void) {
    odin3_log_set_level(ODIN3_LOG_ERROR);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("reader"), &reader));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov_at(1), &mid));
    return odin3_module_get(design, mid);
}

typedef struct port_args {
    const char *name;
    odin3_dir dir;
    uint32_t width;
} port_args;

/* Adds net_a port; returns the net of its bit 0. */
static odin3_net_id add_port(odin3_module *mod, port_args args) {
    odin3_port_spec spec = {intern(args.name), args.dir, args.width, args.width == 1, prov_at(2)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    uint32_t idx = odin3_module_port_count(mod) - 1;
    return odin3_wire_net(mod, odin3_module_port_wire(mod, idx), 0);
}

static odin3_net_id new_net(odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, name ? intern(name) : 0, prov_at(3), &net));
    return net;
}

static void add_node(odin3_module *mod, const odin3_node_spec *spec, const odin3_netvec *ports) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, spec, ports, NULL));
}

static void add_gate(odin3_module *mod, const char *type, const odin3_net_id nets[3]) {
    odin3_node_spec spec = {type_id(type), 0, prov_at(4), NULL, 0};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    add_node(mod, &spec, ports);
}

static void add_const(odin3_module *mod, const char *type, odin3_net_id net) {
    odin3_node_spec spec = {type_id(type), 0, prov_at(5), NULL, 0};
    odin3_netvec ports[1] = {{&net, 1}};
    add_node(mod, &spec, ports);
}

/* A $sop over `inputs` nets (one fresh port each) with the given cover text; output port "y". */
static void add_sop(odin3_module *mod, uint32_t width, const char *cover) {
    odin3_net_id ins[MAX_COVER];
    for (uint32_t i = 0; i < width; i++) {
        char name[OUT_BUF];
        (void)snprintf(name, sizeof name, "i%u", (unsigned)i);
        ins[i] = add_port(mod, (port_args){name, ODIN3_DIR_IN, 1});
    }
    odin3_net_id out = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_value params[2] = {
        odin3_value_int(width),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)cover, (uint32_t)strlen(cover), 0, width}};
    odin3_node_spec spec = {type_id("$sop"), intern("g"), prov_at(6), params, 2};
    odin3_netvec ports[2] = {{ins, width}, {&out, 1}};
    add_node(mod, &spec, ports);
}

/* --- reading the output back ----------------------------------------------------------------- */

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    char *text = odin3_util_malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_UINT64((size_t)size, fread(text, 1, (size_t)size, fp));
    text[size] = '\0';
    (void)fclose(fp);
    return text;
}

static char *write_json(const char *file) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(design, file));
    return slurp(file);
}

static void assert_has(const char *text, const char *needle) {
    if (strstr(text, needle) == NULL) {
        char msg[512];
        (void)snprintf(msg, sizeof msg, "missing: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

static void assert_lacks(const char *text, const char *needle) {
    if (strstr(text, needle) != NULL) {
        char msg[512];
        (void)snprintf(msg, sizeof msg, "unexpected: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* --- tests ----------------------------------------------------------------------------------- */

/* Review focus 2: ports and cells agree on bits; every net is one bit >= 2, in net ID order. */
static void test_bits_ports_cells_netnames(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_b = add_port(mod, (port_args){"b", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    (void)add_port(mod, (port_args){"v", ODIN3_DIR_INOUT, 3});
    add_gate(mod, "$_AND_", (odin3_net_id[3]){net_a, net_b, net_y});
    char *text = write_json("t1.json");
    assert_has(text, "\"creator\"");
    assert_has(text, "\"top\": {");
    assert_has(text, "\"a\": {\n          \"direction\": \"input\",\n          \"bits\": [ 2 ]");
    assert_has(text, "\"b\": {\n          \"direction\": \"input\",\n          \"bits\": [ 3 ]");
    assert_has(text, "\"direction\": \"output\",\n          \"bits\": [ 4 ]");
    assert_has(text, "\"direction\": \"inout\",\n          \"bits\": [ 5, 6, 7 ]");
    assert_has(text, "\"type\": \"$_AND_\"");
    assert_has(text, "\"A\": [ 2 ]");
    assert_has(text, "\"B\": [ 3 ]");
    assert_has(text, "\"Y\": [ 4 ]");
    assert_has(text, "\"port_directions\"");
    assert_has(text, "\"netnames\"");
    assert_has(text, "\"v\": {\n          \"hide_name\": 0,\n          \"bits\": [ 5, 6, 7 ]");
    assert_lacks(text, "$port_in");
    odin3_util_free(text);
    (void)remove("t1.json");
}

/* Constants are "0"/"1" and their driver cells are not written. */
static void test_constants(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id zero = add_port(mod, (port_args){"z", ODIN3_DIR_OUT, 1});
    odin3_net_id one = new_net(mod, "one");
    add_const(mod, "$_CONST1_", one);
    add_const(mod, "$_CONST0_", zero);
    add_gate(mod, "$_AND_", (odin3_net_id[3]){net_a, one, net_y});
    char *text = write_json("t2.json");
    assert_has(text, "\"B\": [ \"1\" ]");
    assert_has(text, "\"direction\": \"output\",\n          \"bits\": [ \"0\" ]");
    assert_has(text, "\"one\": {\n          \"hide_name\": 0,\n          \"bits\": [ \"1\" ]");
    assert_lacks(text, "$_CONST");
    odin3_util_free(text);
    (void)remove("t2.json");
}

static char *sop_json(uint32_t width, const char *cover) {
    odin3_module *mod = new_module("top");
    add_sop(mod, width, cover);
    char *text = write_json("t3.json");
    (void)remove("t3.json");
    return text;
}

/* Review focus 3: ON-set and OFF-set covers become exact $lut truth tables. */
static void test_sop_lut(void) {
    char *text = sop_json(2, "111");
    assert_has(text, "\"type\": \"$lut\"");
    assert_has(text, "\"LUT\": \"1000\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000010\"");
    assert_has(text, "\"A\": [ 2, 3 ]");
    assert_has(text, "\"Y\": [ 4 ]");
    odin3_util_free(text);
}

static void test_sop_lut_offset(void) {
    char *text = sop_json(2, "110"); /* NAND: OFF-set row 11 */
    assert_has(text, "\"LUT\": \"0111\"");
    odin3_util_free(text);
}

static void test_sop_lut_dash_and_order(void) {
    /* y = a & !b | c   (inputs a b c = bits 0 1 2); rows "10-" and "--1" */
    char *text = sop_json(3, "10-1"
                             "--11");
    /* index = a + 2b + 4c; ones at: 1 (a), 4 5 6 7 (c): 11110010 MSB first */
    assert_has(text, "\"LUT\": \"11110010\"");
    odin3_util_free(text);
}

static void test_sop_lut_six_and_empty(void) {
    char *text = sop_json(6, "1111110");
    assert_has(text, "\"type\": \"$lut\"");
    char expect[OUT_BUF * 2];
    (void)snprintf(expect, sizeof expect, "\"LUT\": \"0%.63s\"",
                   "111111111111111111111111111111111111111111111111111111111111111111");
    assert_has(text, expect);
    odin3_util_free(text);
}

static void test_sop_wide_on_set(void) {
    char *text = sop_json(SEVEN, "1-0---11"
                                 "0------1");
    assert_has(text, "\"type\": \"$sop\"");
    assert_has(text, "\"DEPTH\": \"00000000000000000000000000000010\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000111\"");
    assert_has(text, "\"TABLE\": \"0000000000000110000000010010\"");
    assert_lacks(text, "$not");
    odin3_util_free(text);
}

static void test_sop_wide_off_set(void) {
    char *text = sop_json(SEVEN, "11111110");
    assert_has(text, "\"type\": \"$sop\"");
    assert_has(text, "\"type\": \"$not\"");
    assert_has(text, "\"TABLE\": \"10101010101010\"");
    odin3_util_free(text);
}

/* Review focus 5: INIT 0/1 are defined, INIT 2/3 are net_x. */
static void add_latch(odin3_module *mod, const odin3_net_id nets[3], int64_t init) {
    odin3_value param = odin3_value_int(init);
    odin3_node_spec spec = {type_id("$_DFF_P_"), 0, prov_at(7), &param, 1};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    add_node(mod, &spec, ports);
}

static void test_latch_init(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    for (int64_t init = 0; init <= 3; init++) {
        char name[OUT_BUF];
        (void)snprintf(name, sizeof name, "q%d", (int)init);
        add_latch(mod, (odin3_net_id[3]){clk, din, new_net(mod, name)}, init);
    }
    char *text = write_json("t4.json");
    assert_has(text, "\"type\": \"$_DFF_P_\"");
    assert_has(text, "\"q0\": {\n          \"hide_name\": 0,\n          \"bits\": [ 4 ],\n"
                     "          \"attributes\": {\n            \"init\": \"0\"");
    assert_has(text, "\"q1\": {\n          \"hide_name\": 0,\n          \"bits\": [ 5 ],\n"
                     "          \"attributes\": {\n            \"init\": \"1\"");
    assert_has(text, "\"q2\": {\n          \"hide_name\": 0,\n          \"bits\": [ 6 ],\n"
                     "          \"attributes\": {\n            \"init\": \"x\"");
    assert_has(text, "\"q3\": {\n          \"hide_name\": 0,\n          \"bits\": [ 7 ],\n"
                     "          \"attributes\": {\n            \"init\": \"x\"");
    assert_lacks(text, "\"INIT\"");
    odin3_util_free(text);
    (void)remove("t4.json");
}

/* A latch Q on an unnamed net still carries its init on net_a generated netname. */
static void test_latch_init_unnamed_q(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    odin3_net_id net_q = new_net(mod, NULL);
    odin3_value param = odin3_value_int(1);
    odin3_node_spec spec = {type_id("$_DFF_N_"), 0, prov_at(7), &param, 1};
    odin3_netvec ports[3] = {{&clk, 1}, {&din, 1}, {&net_q, 1}};
    add_node(mod, &spec, ports);
    char *text = write_json("t4b.json");
    assert_has(text, "\"$n3\": {");
    assert_has(text, "\"init\": \"1\"");
    odin3_util_free(text);
    (void)remove("t4b.json");
}

static void test_src_attribute_and_names(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = new_net(mod, "a");
    odin3_net_id net_b = new_net(mod, "b");
    odin3_net_id net_y = new_net(mod, "y");
    add_gate(mod, "$_AND_", (odin3_net_id[3]){net_a, net_b, net_y});
    char *text = write_json("t5.json");
    assert_has(text, "\"$c1\": {\n          \"hide_name\": 1,");
    assert_has(text, "\"src\": \"t.v:4.1\"");
    assert_has(text, "\"src\": \"t.v:3.1\"");
    odin3_util_free(text);
    (void)remove("t5.json");
}

static void test_instance_and_parameters(void) {
    odin3_module *sub = new_module("sub");
    (void)add_port(sub, (port_args){"i", ODIN3_DIR_IN, 2});
    (void)add_port(sub, (port_args){"o", ODIN3_DIR_OUT, 1});
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = add_port(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id in2[2] = {net_x, net_x};
    odin3_node_spec spec = {odin3_module_celltype(sub), intern("u1"), prov_at(8), NULL, 0};
    odin3_netvec ports[2] = {{in2, 2}, {&net_y, 1}};
    add_node(mod, &spec, ports);
    char *text = write_json("t6.json");
    assert_has(text, "\"type\": \"sub\"");
    assert_has(text, "\"u1\": {");
    assert_has(text, "\"i\": \"input\"");
    assert_has(text, "\"i\": [ 2, 2 ]");
    assert_has(text, "\"o\": [ 3 ]");
    odin3_util_free(text);
    (void)remove("t6.json");
}

/* Parameters are binary strings (Yosys style). */
static void test_parameters_binary(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = add_port(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_s = add_port(mod, (port_args){"s", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 2});
    odin3_net_id y1 = odin3_wire_net(mod, odin3_module_port_wire(mod, 2), 1);
    odin3_net_id lo[2] = {net_x, net_x};
    odin3_net_id out[2] = {net_y, y1};
    odin3_value width = odin3_value_int(2);
    odin3_node_spec spec = {type_id("$mux"), intern("m"), prov_at(9), &width, 1};
    odin3_netvec ports[4] = {{lo, 2}, {lo, 2}, {&net_s, 1}, {out, 2}};
    add_node(mod, &spec, ports);
    char *text = write_json("t6b.json");
    assert_has(text, "\"type\": \"$mux\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000010\"");
    assert_has(text, "\"S\": [ 3 ]");
    assert_has(text, "\"Y\": [ 4, 5 ]");
    odin3_util_free(text);
    (void)remove("t6b.json");
}

/* A wire [5:2] has offset 2; [0:3] is upto; signedness is written. */
static void test_wire_range_attributes(void) {
    odin3_module *mod = new_module("top");
    odin3_wire_spec down = {intern("w"), 5, 2, true, prov_at(3)};
    odin3_wire_spec up = {intern("u"), 0, 3, false, prov_at(3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &down, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &up, NULL, NULL));
    char *text = write_json("t7.json");
    assert_has(text, "\"w\": {\n          \"hide_name\": 0,\n          \"bits\": [ 2, 3, 4, 5 ],\n"
                     "          \"offset\": 2,\n          \"signed\": 1,");
    assert_has(text, "\"u\": {\n          \"hide_name\": 0,\n          \"bits\": [ 6, 7, 8, 9 ],\n"
                     "          \"upto\": 1,");
    odin3_util_free(text);
    (void)remove("t7.json");
}

static void test_deterministic(void) {
    odin3_module *mod = new_module("top");
    add_sop(mod, 3, "10-1");
    char *first = write_json("t8a.json");
    char *second = write_json("t8b.json");
    TEST_ASSERT_EQUAL_STRING(first, second);
    odin3_util_free(first);
    odin3_util_free(second);
    (void)remove("t8a.json");
    (void)remove("t8b.json");
}

static void test_io_failure(void) {
    (void)new_module("top");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_json_write(design, "/nonexistent-dir/x/out.json"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_write(NULL, "x.json"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_write(design, NULL));
    FILE *probe = fopen("/dev/full", "wb");
    if (probe != NULL) {
        (void)fclose(probe);
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_json_write(design, "/dev/full"));
    }
}

/* Every allocation point fails once: NO_MEMORY with no file left behind, then OK and identical. */
static void test_oom_sweep(void) {
    odin3_module *mod = new_module("top");
    add_sop(mod, SEVEN,
            "1111111"
            "0");
    char *baseline = write_json("t9.json");
    enum { SWEEP_LIMIT = 1000 };
    bool finished = false;
    uint32_t failures = 0;
    for (long fail_at = 0; fail_at < SWEEP_LIMIT && !finished; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status status = odin3_json_write(design, "t9.json");
        odin3_util_set_alloc_fail_after(-1);
        if (status == ODIN3_OK) {
            finished = true;
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, status);
            TEST_ASSERT_NULL(fopen("t9.json", "rb"));
            failures++;
        }
    }
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(failures > 0);
    char *again = slurp("t9.json");
    TEST_ASSERT_EQUAL_STRING(baseline, again);
    odin3_util_free(baseline);
    odin3_util_free(again);
    (void)remove("t9.json");
}

/* Runs `program -net_q -net_s script` without net_a shell; the exit status, or -1 if it cannot be
 * started. */
static int run_program(const char *const command[2]) {
    enum { ARG_BUF = 512 };
    char prog_arg[ARG_BUF];
    char script_arg[ARG_BUF];
    (void)snprintf(prog_arg, sizeof prog_arg, "%s", command[0]);
    (void)snprintf(script_arg, sizeof script_arg, "%s", command[1]);
    char flag_q[] = "-q";
    char flag_s[] = "-s";
    char *argv[] = {prog_arg, flag_q, flag_s, script_arg, NULL};
    pid_t pid = 0;
    int status = 0;
    if (posix_spawnp(&pid, command[0], NULL, NULL, argv, environ) != 0 ||
        waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* Yosys on PATH, else the workspace build; NULL when neither runs. */
static const char *find_yosys(void) {
    static char path[512];
    FILE *probe = fopen("probe.ys", "wb");
    if (probe == NULL) {
        return NULL;
    }
    (void)fclose(probe);
    const char *home = getenv("HOME");
    (void)snprintf(path, sizeof path, "%s/odin3-ws/external/yosys/build/yosys",
                   home != NULL ? home : "");
    const char *found = NULL;
    if (run_program((const char *const[2]){"yosys", "probe.ys"}) == 0) {
        found = "yosys";
    } else if (run_program((const char *const[2]){path, "probe.ys"}) == 0) {
        found = path;
    }
    (void)remove("probe.ys");
    return found;
}

/* Yosys must accept what we write (when it is installed). */
static void test_yosys_reads_output(void) {
    const char *yosys = find_yosys();
    if (yosys == NULL) {
        TEST_IGNORE_MESSAGE("yosys not found");
    }
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_b = add_port(mod, (port_args){"b", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id zero = add_port(mod, (port_args){"z", ODIN3_DIR_OUT, 1});
    odin3_net_id one = new_net(mod, "one");
    add_const(mod, "$_CONST1_", one);
    add_const(mod, "$_CONST0_", zero);
    add_gate(mod, "$_AND_", (odin3_net_id[3]){net_a, one, net_y});
    add_latch(mod, (odin3_net_id[3]){net_a, net_b, new_net(mod, "q")}, 1);
    odin3_util_free(write_json("t10.json"));
    FILE *script = fopen("t10.ys", "wb");
    TEST_ASSERT_NOT_NULL(script);
    (void)fprintf(script, "read_json t10.json\nhierarchy -check\n");
    (void)fclose(script);
    int rc = run_program((const char *const[2]){yosys, "t10.ys"});
    (void)remove("t10.json");
    (void)remove("t10.ys");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

typedef struct sop_case {
    uint32_t width;
    const char *cover; /* rows of width input chars then the output char, concatenated */
} sop_case;

static const sop_case k_cases[] = {
    {2, "111"},
    {2, "110"},
    {3, "10-1--11"},
    {SEVEN, "1-0---11"
            "0------1"},
    {SEVEN, "1-0---10"
            "0------0"},
    {6, "1-0--10"
        "-1----0"},
};
enum { N_CASES = sizeof k_cases / sizeof k_cases[0] };

/* The reference: the same cover as net_a BLIF .names, model g<k>. */
static void write_reference(FILE *fp, uint32_t index) {
    const sop_case *cs = &k_cases[index];
    (void)fprintf(fp, ".model g%u\n.inputs", (unsigned)index);
    for (uint32_t i = 0; i < cs->width; i++) {
        (void)fprintf(fp, " i%u", (unsigned)i);
    }
    (void)fprintf(fp, "\n.outputs y\n.names");
    for (uint32_t i = 0; i < cs->width; i++) {
        (void)fprintf(fp, " i%u", (unsigned)i);
    }
    (void)fprintf(fp, " y\n");
    size_t row = cs->width + 1;
    for (size_t off = 0; off < strlen(cs->cover); off += row) {
        (void)fprintf(fp, "%.*s %c\n", (int)cs->width, cs->cover + off, cs->cover[off + cs->width]);
    }
    (void)fprintf(fp, ".end\n");
}

static void ref_name(char *buf, size_t size, uint32_t index) {
    (void)snprintf(buf, size, "t11_g%u.blif", (unsigned)index);
}

/* Review focus 3, semantically: Yosys proves each written $lut/$sop equal to the BLIF cover. */
static void test_yosys_sop_equivalence(void) {
    const char *yosys = find_yosys();
    if (yosys == NULL) {
        TEST_IGNORE_MESSAGE("yosys not found");
    }
    for (uint32_t index = 0; index < N_CASES; index++) {
        char name[OUT_BUF];
        (void)snprintf(name, sizeof name, "m%u", (unsigned)index);
        add_sop(new_module(name), k_cases[index].width, k_cases[index].cover);
    }
    odin3_util_free(write_json("t11.json"));
    FILE *script = fopen("t11.ys", "wb");
    TEST_ASSERT_NOT_NULL(script);
    (void)fprintf(script, "read_json t11.json\n");
    for (uint32_t index = 0; index < N_CASES; index++) {
        char name[OUT_BUF];
        ref_name(name, sizeof name, index);
        FILE *ref = fopen(name, "wb");
        TEST_ASSERT_NOT_NULL(ref);
        write_reference(ref, index);
        (void)fclose(ref);
        (void)fprintf(script, "read_blif %s\n", name);
    }
    for (uint32_t index = 0; index < N_CASES; index++) {
        (void)fprintf(script,
                      "miter -equiv -flatten g%u m%u mt%u\nsat -verify -prove trigger 0 mt%u\n",
                      (unsigned)index, (unsigned)index, (unsigned)index, (unsigned)index);
    }
    (void)fclose(script);
    int rc = run_program((const char *const[2]){yosys, "t11.ys"});
    (void)remove("t11.json");
    (void)remove("t11.ys");
    for (uint32_t index = 0; index < N_CASES; index++) {
        char name[OUT_BUF];
        ref_name(name, sizeof name, index);
        (void)remove(name);
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bits_ports_cells_netnames);
    RUN_TEST(test_constants);
    RUN_TEST(test_sop_lut);
    RUN_TEST(test_sop_lut_offset);
    RUN_TEST(test_sop_lut_dash_and_order);
    RUN_TEST(test_sop_lut_six_and_empty);
    RUN_TEST(test_sop_wide_on_set);
    RUN_TEST(test_sop_wide_off_set);
    RUN_TEST(test_latch_init);
    RUN_TEST(test_latch_init_unnamed_q);
    RUN_TEST(test_src_attribute_and_names);
    RUN_TEST(test_instance_and_parameters);
    RUN_TEST(test_parameters_binary);
    RUN_TEST(test_wire_range_attributes);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_io_failure);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_yosys_reads_output);
    RUN_TEST(test_yosys_sop_equivalence);
    return UNITY_END();
}
