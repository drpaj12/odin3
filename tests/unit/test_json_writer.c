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

#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

enum { OUT_BUF = 64, MAX_COVER = 64, SEVEN = 7, CELL_TEXT = 4096 };

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
    odin3_srcloc loc = {intern("t.v"), line, 1, line, 1, 0};
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
    odin3_port_spec spec = {
        intern(args.name), args.dir, args.width, args.width == 1, prov_at(2), false, 0, 0, false};
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

static void add_named_gate(odin3_module *mod, const char *name, const odin3_net_id nets[3]) {
    odin3_node_spec spec = {type_id("$_AND_"), intern(name), prov_at(4), NULL, 0};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    add_node(mod, &spec, ports);
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
    assert_has(text, "\"src\": \"t.v:4.1-4.1\"");
    assert_has(text, "\"src\": \"t.v:3.1-3.1\"");
    odin3_util_free(text);
    (void)remove("t5.json");
}

/* --- 5.4a: the src text (spec 5.4 W-2 to W-6) ------------------------------------------------- */

/* Writes the design to a file of a private temporary directory, reads it, removes both. */
static char *write_json_scratch(void) {
    char dir[] = "/tmp/odin3_jsrc_XXXXXX";
    char path[sizeof dir + 16];
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof path, "%s/o.json", dir);
    char *text = write_json(path);
    (void)remove(path);
    (void)rmdir(dir);
    return text;
}

/* A SOURCE (or IMPORTED) record at file:line.col-end_line.end_col. */
static odin3_prov_id leaf_at(const char *file, odin3_srcloc where, bool imported) {
    odin3_srcloc loc = where;
    loc.file = intern(file);
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    odin3_status st = imported ? odin3_prov_imported(&reader, &origin, &id)
                               : odin3_prov_source(&reader, &origin, &id);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return id;
}

static odin3_prov_id derived_of(const odin3_prov_id *parents, uint32_t count) {
    odin3_prov_id id = {0};
    odin3_prov_begin_op(&reader);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive(&reader, (odin3_prov_list){parents, count}, &id));
    return id;
}

/* top with one named AND gate whose provenance is prov (its nets and ports carry t.v lines). */
static void gate_with_prov(odin3_prov_id prov) {
    odin3_module *mod = new_module("top");
    odin3_net_id nets[3] = {new_net(mod, "a"), new_net(mod, "b"), new_net(mod, "y")};
    odin3_node_spec spec = {type_id("$_AND_"), intern("g"), prov, NULL, 0};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    add_node(mod, &spec, ports);
}

static size_t count_of(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* W-4: Yosys's form, with the end span. */
static void test_src_end_span(void) {
    gate_with_prov(leaf_at("a.v", (odin3_srcloc){0, 3, 1, 4, 10, 0}, false));
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"a.v:3.1-4.10\"");
    odin3_util_free(text);
}

/* W-4: a record with a column but no end is `file:L.C`; with neither it is `file:L`. */
static void test_src_short_forms(void) {
    odin3_prov_id parents[2] = {leaf_at("a.v", (odin3_srcloc){0, 3, 7, 0, 0, 0}, false),
                                leaf_at("b.v", (odin3_srcloc){0, 9, 0, 0, 0, 0}, false)};
    gate_with_prov(derived_of(parents, 2));
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"a.v:3.7|b.v:9\"");
    odin3_util_free(text);
}

/* T-6, W-2: a design that only has IMPORTED leaves writes no src at all. */
static void test_src_imported_only_writes_none(void) {
    odin3_prov_id imp = leaf_at("d.blif", (odin3_srcloc){0, 5, 0, 0, 0, 0}, true);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("top"), imp, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    const char *const names[3] = {"a", "b", "y"};
    odin3_net_id nets[3] = {{0}, {0}, {0}};
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern(names[i]), imp, &nets[i]));
    }
    odin3_node_spec spec = {type_id("$_AND_"), intern("g"), imp, NULL, 0};
    odin3_netvec ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[2], 1}};
    add_node(mod, &spec, ports);
    char *text = write_json_scratch();
    assert_lacks(text, "\"src\"");
    assert_lacks(text, "d.blif");
    odin3_util_free(text);
}

/* W-2: an IMPORTED parent ahead of a SOURCE one prints the SOURCE only. */
static void test_src_imported_parent_skipped(void) {
    odin3_prov_id parents[2] = {leaf_at("d.blif", (odin3_srcloc){0, 5, 0, 0, 0, 0}, true),
                                leaf_at("a.v", (odin3_srcloc){0, 8, 1, 8, 4, 0}, false)};
    gate_with_prov(derived_of(parents, 2));
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"a.v:8.1-8.4\"");
    assert_lacks(text, "d.blif");
    odin3_util_free(text);
}

/* W-3: a DERIVED record of three SOURCE parents prints three locations in parent order, deduped. */
static void test_src_three_parents(void) {
    odin3_prov_id parents[4] = {leaf_at("c.v", (odin3_srcloc){0, 3, 1, 3, 5, 0}, false),
                                leaf_at("a.v", (odin3_srcloc){0, 1, 1, 1, 5, 0}, false),
                                leaf_at("b.v", (odin3_srcloc){0, 2, 1, 2, 5, 0}, false),
                                leaf_at("a.v", (odin3_srcloc){0, 1, 1, 1, 5, 0}, false)};
    gate_with_prov(derived_of(parents, 4));
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"c.v:3.1-3.5|a.v:1.1-1.5|b.v:2.1-2.5\"");
    odin3_util_free(text);
}

/* W-3, X-3: at most 8 locations; the first 8 are kept. */
static void test_src_cap_of_eight(void) {
    odin3_prov_id parents[10];
    for (uint32_t i = 0; i < 10; i++) {
        parents[i] = leaf_at("a.v", (odin3_srcloc){0, i + 1, 1, i + 1, 2, 0}, false);
    }
    gate_with_prov(derived_of(parents, 10));
    char *text = write_json_scratch();
    assert_has(text, "a.v:8.1-8.2\"");
    assert_lacks(text, "a.v:9.1-9.2");
    assert_has(text, "|a.v:7.1-7.2|a.v:8.1-8.2\"");
    odin3_util_free(text);
}

static unsigned warned_files;
static char warned_text[CELL_TEXT];

static void warn_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_WARN) {
        warned_files++;
        (void)snprintf(warned_text, sizeof warned_text, "%s", msg);
    }
}

/* W-5, X-4: a file name with a byte JSON src cannot carry is skipped, one WARNING per file. */
static void test_src_unwritable_file_skipped(void) {
    odin3_prov_id parents[3] = {leaf_at("x|y.v", (odin3_srcloc){0, 1, 1, 1, 2, 0}, false),
                                leaf_at("ok.v", (odin3_srcloc){0, 2, 1, 2, 2, 0}, false),
                                leaf_at("x|y.v", (odin3_srcloc){0, 3, 1, 3, 2, 0}, false)};
    gate_with_prov(derived_of(parents, 3));
    warned_files = 0;
    odin3_log_set_level(ODIN3_LOG_WARN);
    odin3_log_set_sink(warn_sink, NULL);
    char *text = write_json_scratch();
    odin3_log_set_sink(NULL, NULL);
    odin3_log_set_level(ODIN3_LOG_ERROR);
    assert_has(text, "\"src\": \"ok.v:2.1-2.2\"");
    assert_lacks(text, "x|y.v");
    TEST_ASSERT_EQUAL_UINT(1, warned_files);
    TEST_ASSERT_NOT_NULL(strstr(warned_text, "write_json: source file name 'x|y.v'"));
    odin3_util_free(text);
}

/* A file name with a quote or backslash is escaped by JSON itself. */
static void test_src_file_with_quote_is_escaped(void) {
    gate_with_prov(leaf_at("a\"b\\c.v", (odin3_srcloc){0, 4, 1, 4, 3, 0}, false));
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"a\\\"b\\\\c.v:4.1-4.3\"");
    odin3_util_free(text);
}

/* W-6: a module carries its src (new), in its attributes before the user's. */
static void test_src_on_module(void) {
    odin3_prov_id prov = leaf_at("m.v", (odin3_srcloc){0, 2, 1, 9, 10, 0}, false);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("top"), prov, &mid));
    char *text = write_json_scratch();
    assert_has(text, "\"attributes\": {\n        \"src\": \"m.v:2.1-9.10\"\n      }");
    TEST_ASSERT_EQUAL_UINT(1, count_of(text, "m.v:2.1-9.10"));
    odin3_util_free(text);
}

/* W-6: wires and named nets carry src in their netnames. */
static void test_src_on_wires_and_nets(void) {
    odin3_module *mod = new_module("top");
    (void)add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    (void)new_net(mod, "n");
    char *text = write_json_scratch();
    assert_has(text, "\"src\": \"t.v:2.1-2.1\"");
    assert_has(text, "\"src\": \"t.v:3.1-3.1\"");
    odin3_util_free(text);
}

static void set_attr(odin3_module *mod, odin3_objref obj, const char *key, odin3_value val) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, obj, intern(key), &val));
}

static odin3_value str_value(const char *text) {
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0};
    val.str = intern(text);
    return val;
}

/* Every attribute is written (odin3_attr_foreach order) on cells, netnames and modules; BLIF
 * reader keys are mapped back to Yosys names; a user src replaces the provenance one. */
static void test_user_attributes(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id net_n = new_net(mod, "n");
    add_named_gate(mod, "g", (odin3_net_id[3]){net_a, net_n, net_y});
    odin3_objref cell = {ODIN3_OBJ_NODE, odin3_module_find_node(mod, intern("g")).v};
    set_attr(mod, cell, "keep", odin3_value_int(1));
    set_attr(mod, cell, "blif.attr:src", str_value("\"top.v:7.3-7.9\""));
    set_attr(mod, cell, "blif.attr:mask", str_value("0110"));
    set_attr(mod, cell, "blif.param:WIDTH", str_value("\"0101\""));
    set_attr(mod, cell, "blif_extras", str_value("blif.attr:src blif.attr:mask blif.param:WIDTH"));
    set_attr(mod, (odin3_objref){ODIN3_OBJ_NET, net_n.v}, "note", str_value("hi"));
    set_attr(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, "top",
             odin3_value_int(1));
    char *text = write_json("t13.json");
    assert_has(text, "\"parameters\": {\n            \"WIDTH\": \"0101 \"\n          }");
    assert_has(text, "\"keep\": \"00000000000000000000000000000001\",\n"
                     "            \"src\": \"top.v:7.3-7.9\",\n"
                     "            \"mask\": \"0110\"\n");
    assert_lacks(text, "blif_extras");
    assert_lacks(text, "\"src\": \"t.v:4.1-4.1\""); /* the user's src wins */
    assert_has(text, "\"note\": \"hi\"");
    assert_has(text, "\"attributes\": {\n        \"top\": \"00000000000000000000000000000001\",\n"
                     "        \"src\": \"t.v:1.1-1.1\"\n");
    odin3_util_free(text);
    (void)remove("t13.json");
}

static const odin3_port_def BB_PORTS[] = {
    {"d", ODIN3_DIR_IN, false, 3, NULL, NULL, NULL, false},
    {"q", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false},
};

/* A declared black-box model is a Yosys blackbox module: its ports with fresh bits, no cells. */
static void test_declared_blackbox_module(void) {
    odin3_celltype_def def = {"bb", ODIN3_GRAN_BLACKBOX, 0, BB_PORTS, 2, NULL, 0, NULL, NULL, NULL,
                              NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &id));
    (void)new_module("top");
    char *text = write_json("t14.json");
    assert_has(text, "\"bb\": {\n"
                     "      \"attributes\": {\n"
                     "        \"blackbox\": \"00000000000000000000000000000001\"\n"
                     "      },\n"
                     "      \"ports\": {\n"
                     "        \"d\": {\n"
                     "          \"direction\": \"input\",\n"
                     "          \"bits\": [ 2, 3, 4 ]\n"
                     "        },\n"
                     "        \"q\": {\n"
                     "          \"direction\": \"output\",\n"
                     "          \"bits\": [ 5 ]\n");
    odin3_util_free(text);
    (void)remove("t14.json");
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

/* A cell's JSON object to find: the whole text and the cell's key. */
typedef struct cell_query {
    const char *text;
    const char *key;
} cell_query;

/* The cell object of query.key in query.text: from `"key": {` to the end of its "connections"
 * member, copied into buf. */
static const char *cell_object(cell_query query, char *buf, size_t size) {
    const char *key = query.key;
    const char *text = query.text;
    char head[OUT_BUF];
    (void)snprintf(head, sizeof head, "\"%s\": {", key);
    const char *start = strstr(text, head);
    TEST_ASSERT_NOT_NULL_MESSAGE(start, key);
    const char *conn = strstr(start, "\"connections\"");
    TEST_ASSERT_NOT_NULL_MESSAGE(conn, key);
    const char *end = strchr(conn, '}');
    TEST_ASSERT_NOT_NULL_MESSAGE(end, key);
    size_t len = (size_t)(end - start);
    TEST_ASSERT_LESS_THAN_size_t(size, len);
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* The 2D word cells (IR change A, EL-31) are generic cells under their Yosys names, $shiftx and
 * $pow included; each cell's own object has its type, parameters and connections. */
static void test_2d_word_cells(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_x = add_port(mod, (port_args){"x", ODIN3_DIR_IN, 1});
    odin3_net_id net_s = add_port(mod, (port_args){"s", ODIN3_DIR_IN, 1});
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    odin3_net_id net_z = add_port(mod, (port_args){"z", ODIN3_DIR_OUT, 1});
    odin3_net_id net_n = add_port(mod, (port_args){"n", ODIN3_DIR_OUT, 1});
    odin3_net_id two[2] = {net_x, net_s};
    odin3_value bin[5] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(2),
                          odin3_value_int(1), odin3_value_int(1)};
    odin3_node_spec shiftx = {type_id("$shiftx"), intern("sx"), prov_at(9), bin, 5};
    odin3_netvec sx_ports[3] = {{two, 2}, {&net_s, 1}, {&net_y, 1}};
    add_node(mod, &shiftx, sx_ports);
    odin3_node_spec pow = {type_id("$pow"), intern("pw"), prov_at(9), bin, 5};
    odin3_netvec pw_ports[3] = {{two, 2}, {&net_s, 1}, {&net_z, 1}};
    add_node(mod, &pow, pw_ports);
    odin3_value un[3] = {odin3_value_int(1), odin3_value_int(2), odin3_value_int(1)};
    odin3_node_spec lnot = {type_id("$logic_not"), intern("ln"), prov_at(9), un, 3};
    odin3_netvec ln_ports[2] = {{two, 2}, {&net_n, 1}};
    add_node(mod, &lnot, ln_ports);
    char *text = write_json("t2d.json");
    static char cell[CELL_TEXT];
    static const char *const keys[2] = {"sx", "pw"};
    static const char *const types[2] = {"\"type\": \"$shiftx\"", "\"type\": \"$pow\""};
    static const char *const outs[2] = {"\"Y\": [ 4 ]", "\"Y\": [ 5 ]"};
    for (uint32_t i = 0; i < 2; i++) {
        (void)cell_object((cell_query){text, keys[i]}, cell, sizeof cell);
        assert_has(cell, types[i]);
        assert_has(cell, "\"A_SIGNED\": \"00000000000000000000000000000000\"");
        assert_has(cell, "\"B_SIGNED\": \"00000000000000000000000000000001\"");
        assert_has(cell, "\"A_WIDTH\": \"00000000000000000000000000000010\"");
        assert_has(cell, "\"B_WIDTH\": \"00000000000000000000000000000001\"");
        assert_has(cell, "\"Y_WIDTH\": \"00000000000000000000000000000001\"");
        assert_has(cell, "\"A\": [ 2, 3 ]");
        assert_has(cell, "\"B\": [ 3 ]");
        assert_has(cell, outs[i]);
    }
    (void)cell_object((cell_query){text, "ln"}, cell, sizeof cell);
    assert_has(cell, "\"type\": \"$logic_not\"");
    assert_has(cell, "\"A_SIGNED\": \"00000000000000000000000000000001\"");
    assert_lacks(cell, "B_SIGNED");
    assert_has(cell, "\"Y\": [ 6 ]");
    odin3_util_free(text);
    (void)remove("t2d.json");
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

/*
 * Ranged ports (IR-7, 2D IR change C) are written as Yosys writes them: bits LSB first, [0:3]
 * signed is upto and signed, [5:2] has offset 2, [-1:2] has offset -1 and is upto.
 */
static void test_ranged_ports(void) {
    odin3_module *mod = new_module("top");
    static const struct {
        const char *name;
        odin3_dir dir;
        int32_t msb, lsb;
        bool is_signed;
    } k_ports[] = {{"a", ODIN3_DIR_IN, 0, 3, true},
                   {"d", ODIN3_DIR_IN, 5, 2, false},
                   {"n", ODIN3_DIR_OUT, -1, 2, false}};
    for (size_t i = 0; i < sizeof k_ports / sizeof k_ports[0]; i++) {
        odin3_port_spec spec = {.name = intern(k_ports[i].name),
                                .dir = k_ports[i].dir,
                                .prov = prov_at(2),
                                .has_range = true,
                                .msb = k_ports[i].msb,
                                .lsb = k_ports[i].lsb,
                                .is_signed = k_ports[i].is_signed};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    }
    char *text = write_json("t7r.json");
    assert_has(text, "\"a\": {\n          \"direction\": \"input\",\n"
                     "          \"bits\": [ 2, 3, 4, 5 ],\n"
                     "          \"upto\": 1,\n          \"signed\": 1\n");
    assert_has(text, "\"d\": {\n          \"direction\": \"input\",\n"
                     "          \"bits\": [ 6, 7, 8, 9 ],\n          \"offset\": 2\n");
    assert_has(text, "\"n\": {\n          \"direction\": \"output\",\n"
                     "          \"bits\": [ 10, 11, 12, 13 ],\n"
                     "          \"offset\": -1,\n          \"upto\": 1\n");
    odin3_util_free(text);
    (void)remove("t7r.json");
}

/* A user name equal to a generated key wins; the generated one gets a $u suffix. */
static void test_cell_key_collision(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id nets[6];
    for (int i = 0; i < 6; i++) {
        nets[i] = new_net(mod, NULL);
    }
    add_named_gate(mod, "$c2", nets);  /* node 1 is literally named $c2 */
    add_gate(mod, "$_AND_", nets + 3); /* node 2 is unnamed: generated key $c2 */
    char *text = write_json("t12.json");
    TEST_ASSERT_EQUAL_UINT64(1, count_of(text, "\"$c2\": {"));
    assert_has(text, "\"$c2$u1\": {");
    odin3_util_free(text);
    (void)remove("t12.json");
}

static void test_netname_key_collision(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id clk = new_net(mod, "clk");
    odin3_net_id din = new_net(mod, "d");
    odin3_net_id qnet = new_net(mod, NULL); /* net 3: generated netname $n3 for its init */
    add_latch(mod, (odin3_net_id[3]){clk, din, qnet}, 1);
    odin3_wire_spec ws = {intern("$n3"), 0, 0, false, prov_at(3)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &ws, NULL, NULL));
    char *text = write_json("t13.json");
    TEST_ASSERT_EQUAL_UINT64(1, count_of(text, "\"$n3\": {"));
    assert_has(text, "\"$n3$u1\": {");
    odin3_util_free(text);
    (void)remove("t13.json");
}

static const char *find_yosys(void);
static int run_program(const char *const command[2]);

/* IR-14: Yosys has one namespace per module, so a cell named like a net (BLIF `.cname foo` on
 * the cell driving `foo`) is bumped; the netname keeps the user's name. */
static void test_cell_named_like_net(void) {
    odin3_module *mod = new_module("top");
    odin3_net_id net_a = add_port(mod, (port_args){"a", ODIN3_DIR_IN, 1});
    odin3_net_id net_b = add_port(mod, (port_args){"b", ODIN3_DIR_IN, 1});
    odin3_net_id foo = new_net(mod, "foo");
    odin3_net_id net_y = add_port(mod, (port_args){"y", ODIN3_DIR_OUT, 1});
    add_named_gate(mod, "foo", (odin3_net_id[3]){net_a, net_b, foo});
    add_named_gate(mod, "a", (odin3_net_id[3]){foo, net_b, net_y}); /* named like a port */
    char *text = write_json("t15.json");
    assert_has(text, "\"foo$u1\": {\n          \"hide_name\": 0,\n          \"type\": \"$_AND_\"");
    assert_has(text, "\"a$u1\": {\n          \"hide_name\": 0,\n          \"type\": \"$_AND_\"");
    assert_has(text, "\"foo\": {\n          \"hide_name\": 0,\n          \"bits\"");
    TEST_ASSERT_EQUAL_UINT64(1, count_of(text, "\"foo\": {"));
    odin3_util_free(text);
    const char *yosys = find_yosys();
    if (yosys != NULL) {
        FILE *script = fopen("t15.ys", "wb");
        TEST_ASSERT_NOT_NULL(script);
        (void)fprintf(script, "read_json t15.json\nhierarchy -check\n");
        (void)fclose(script);
        TEST_ASSERT_EQUAL_INT(0, run_program((const char *const[2]){yosys, "t15.ys"}));
        (void)remove("t15.ys");
    }
    (void)remove("t15.json");
}

/* A 7-input OFF-set $sop over fresh nets, named name (NULL: unnamed). */
static void add_wide_offset_sop(odin3_module *mod, const char *name) {
    odin3_net_id ins[SEVEN];
    for (uint32_t i = 0; i < SEVEN; i++) {
        ins[i] = new_net(mod, NULL);
    }
    odin3_net_id out = new_net(mod, NULL);
    static const char k_cover[] = "1-0---10";
    odin3_value params[2] = {
        odin3_value_int(SEVEN),
        {ODIN3_VAL_COVER, 0, (const uint8_t *)k_cover, (uint32_t)strlen(k_cover), 0, SEVEN}};
    odin3_node_spec spec = {type_id("$sop"), name != NULL ? intern(name) : 0, prov_at(6), params,
                            2};
    odin3_netvec ports[2] = {{ins, SEVEN}, {&out, 1}};
    add_node(mod, &spec, ports);
}

/* Two generated keys that coincide: a cell the user named $c<N+1> and the unnamed node N+1, both
 * OFF-set wide covers, both want "$c<N+1>$sop". The second written is bumped. */
static void test_generated_keys_coincide(void) {
    odin3_module *mod = new_module("top");
    char name[OUT_BUF];
    (void)snprintf(name, sizeof name, "$c%u", (unsigned)odin3_module_node_end(mod) + 1);
    add_wide_offset_sop(mod, name);
    add_wide_offset_sop(mod, NULL);
    char *text = write_json("t16.json");
    char key[OUT_BUF * 2];
    (void)snprintf(key, sizeof key, "\"%s$sop\": {", name);
    TEST_ASSERT_EQUAL_UINT64(1, count_of(text, key));
    (void)snprintf(key, sizeof key, "\"%s$sop$u1\": {", name);
    TEST_ASSERT_EQUAL_UINT64(1, count_of(text, key));
    odin3_util_free(text);
    (void)remove("t16.json");
}

/* An empty cover is constant 0 whatever its width: a $lut with no inputs, never DEPTH 0. */
static void test_empty_cover_is_const0(void) {
    char *text = sop_json(SEVEN, "");
    assert_has(text, "\"type\": \"$lut\"");
    assert_has(text, "\"LUT\": \"0\"");
    assert_has(text, "\"WIDTH\": \"00000000000000000000000000000000\"");
    assert_has(text, "\"A\": [  ]");
    assert_lacks(text, "DEPTH");
    odin3_util_free(text);
}

/* Invalid UTF-8 in a name is replaced so the JSON stays valid. */
static void test_invalid_utf8_name(void) {
    odin3_module *mod = new_module("top");
    (void)new_net(mod, "bad\xff\xc3(name");
    (void)new_net(mod, "caf\xc3\xa9");
    odin3_net_id clk = new_net(mod, NULL);
    add_latch(mod, (odin3_net_id[3]){clk, clk, new_net(mod, "bad\xff\xc3(name2")}, 0);
    char *text = write_json("t14.json");
    assert_has(text, "\"bad\\ufffd\\ufffd(name2\"");
    odin3_util_free(text);
    (void)remove("t14.json");
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

static char json_error[512];

static void json_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(json_error, sizeof json_error, "%s", msg);
    }
}

/* A write that fails mid-design (RLIMIT_FSIZE, SIGXFSZ ignored) logs the failing write's own
 * reason (EFBIG), and the destination is not created. */
static void test_write_failure_reason(void) {
    odin3_module *mod = new_module("top");
    for (int i = 0; i < 200; i++) {
        odin3_net_id nets[3] = {new_net(mod, NULL), new_net(mod, NULL), new_net(mod, NULL)};
        add_gate(mod, "$_AND_", nets);
    }
    json_error[0] = '\0';
    odin3_log_set_sink(json_sink, NULL);
    struct rlimit old;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = {1024, old.rlim_max};
    TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_FSIZE, &small));
    odin3_status status = odin3_json_write(design, "t17.json");
    (void)setrlimit(RLIMIT_FSIZE, &old);
    (void)signal(SIGXFSZ, prev);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, status);
    TEST_ASSERT_NOT_NULL(strstr(json_error, "t17.json: write failed: File too large"));
    TEST_ASSERT_NOT_EQUAL_INT(0, access("t17.json", F_OK));
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

static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        (void)fclose(fp);
    }
    return fp != NULL;
}

/* One write with the fail_at-th allocation failing; true when it succeeded. A failed write must
 * leave the existing destination (baseline) untouched and no temporary file behind. */
static bool write_failing_at(const char *baseline, long fail_at) {
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status status = odin3_json_write(design, "t9.json");
    odin3_util_set_alloc_fail_after(-1);
    if (status == ODIN3_OK) {
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, status);
    char *left = slurp("t9.json");
    TEST_ASSERT_EQUAL_STRING(baseline, left);
    odin3_util_free(left);
    TEST_ASSERT_FALSE(file_exists("t9.json.tmp"));
    return false;
}

/* Every allocation point fails once: NO_MEMORY, destination and temp as expected, then OK. */
static void test_oom_sweep(void) {
    odin3_module *mod = new_module("top");
    add_sop(mod, SEVEN,
            "1111111"
            "0");
    char *baseline = write_json("t9.json");
    enum { SWEEP_LIMIT = 1000 };
    long fail_at = 0;
    while (fail_at < SWEEP_LIMIT && !write_failing_at(baseline, fail_at)) {
        fail_at++;
    }
    TEST_ASSERT_TRUE(fail_at > 0);
    TEST_ASSERT_TRUE(fail_at < SWEEP_LIMIT);
    char *again = slurp("t9.json");
    TEST_ASSERT_EQUAL_STRING(baseline, again);
    odin3_util_free(baseline);
    odin3_util_free(again);
    (void)remove("t9.json");
}

/* Runs `program -q -s script` without a shell; the exit status, or -1 if it cannot be started. */
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

/* $ODIN3_YOSYS (CMake sets it from find_program), else `yosys` on PATH; NULL when it does not run.
 */
static const char *find_yosys(void) {
    const char *env = getenv("ODIN3_YOSYS");
    const char *yosys = env != NULL && env[0] != '\0' ? env : "yosys";
    FILE *probe = fopen("probe.ys", "wb");
    if (probe == NULL) {
        return NULL;
    }
    (void)fclose(probe);
    int rc = run_program((const char *const[2]){yosys, "probe.ys"});
    (void)remove("probe.ys");
    return rc == 0 ? yosys : NULL;
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
    odin3_objref top = {ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v};
    set_attr(mod, top, "keep", odin3_value_int(1));
    set_attr(mod, top, "blif.attr:note", str_value("\"0101\""));
    set_attr(mod, (odin3_objref){ODIN3_OBJ_NET, net_b.v}, "blif.attr:mask", str_value("10"));
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
    {2, ""},
    {SEVEN, ""},
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
    RUN_TEST(test_src_end_span);
    RUN_TEST(test_src_short_forms);
    RUN_TEST(test_src_imported_only_writes_none);
    RUN_TEST(test_src_imported_parent_skipped);
    RUN_TEST(test_src_three_parents);
    RUN_TEST(test_src_cap_of_eight);
    RUN_TEST(test_src_unwritable_file_skipped);
    RUN_TEST(test_src_file_with_quote_is_escaped);
    RUN_TEST(test_src_on_module);
    RUN_TEST(test_src_on_wires_and_nets);
    RUN_TEST(test_user_attributes);
    RUN_TEST(test_declared_blackbox_module);
    RUN_TEST(test_instance_and_parameters);
    RUN_TEST(test_parameters_binary);
    RUN_TEST(test_2d_word_cells);
    RUN_TEST(test_wire_range_attributes);
    RUN_TEST(test_ranged_ports);
    RUN_TEST(test_cell_key_collision);
    RUN_TEST(test_netname_key_collision);
    RUN_TEST(test_cell_named_like_net);
    RUN_TEST(test_generated_keys_coincide);
    RUN_TEST(test_empty_cover_is_const0);
    RUN_TEST(test_invalid_utf8_name);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_io_failure);
    RUN_TEST(test_write_failure_reason);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_yosys_reads_output);
    RUN_TEST(test_yosys_sop_equivalence);
    return UNITY_END();
}
