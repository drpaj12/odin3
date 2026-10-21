/*
 * test_read_json_cells.c — the JSON reader's cells (spec 5.3 M-5 to M-8, M-11, M-13, M-14, E-6 to
 * E-9, T-5, T-6): type resolution, parameters, connections, `$sop`, hierarchy, memories, second
 * drivers, limits and out of memory.
 */
#include "rj_harness.h"

#include "ir/prov.h"
#include "techlib/reader.h"
#include "util/str.h"

#include <time.h>

enum { BUF = 16384, BITS_OVER = 1048577, SOP_FUNCS = 256, SOP_INPUTS = 3, MINTERMS = 8 };

void setUp(void) {
    rj_setup();
}
void tearDown(void) {
    rj_teardown();
}

/* Appends text to buf at *at (the buffer is BUF bytes; a longer text fails the test). */
static void append(char *buf, size_t *at, const char *text) {
    size_t len = strlen(text);
    TEST_ASSERT_TRUE_MESSAGE(*at + len < BUF, "test JSON too long");
    memcpy(buf + *at, text, len + 1);
    *at += len;
}

/* {"modules": {"top": {"ports": {<ports>}, "cells": {<cells>}}}} */
static const char *top_json(const char *ports, const char *cells) {
    static char buf[BUF];
    size_t at = 0;
    buf[0] = '\0';
    append(buf, &at, "{\"modules\": {\"top\": {\"ports\": {");
    append(buf, &at, ports);
    append(buf, &at, "}, \"cells\": {");
    append(buf, &at, cells);
    append(buf, &at, "}}}}");
    return buf;
}

#define IN_PORT(name, bits) "\"" name "\": {\"direction\": \"input\", \"bits\": " bits "}"
#define OUT_PORT(name, bits) "\"" name "\": {\"direction\": \"output\", \"bits\": " bits "}"

/* The ports of every test that makes one 2-bit $and: a, b in, y out. */
#define AND_PORTS IN_PORT("a", "[2, 3]") ", " IN_PORT("b", "[4, 5]") ", " OUT_PORT("y", "[6, 7]")

static const char *and_cell(const char *params, const char *conns, const char *extra) {
    static char buf[BUF];
    size_t at = 0;
    buf[0] = '\0';
    append(buf, &at, "\"g\": {\"type\": \"$and\", \"parameters\": {");
    append(buf, &at, params);
    append(buf, &at, "}, ");
    append(buf, &at, extra);
    append(buf, &at, " \"connections\": {");
    append(buf, &at, conns);
    append(buf, &at, "}}");
    return buf;
}

static const char *const AND2_PARAMS = "\"A_WIDTH\": 2, \"B_WIDTH\": 2, \"Y_WIDTH\": 2";
static const char *const AND2_CONNS = "\"A\": [2, 3], \"B\": [4, 5], \"Y\": [6, 7]";

static odin3_node_id the_cell(odin3_module *mod) {
    return rj_node(mod, "g");
}

static int64_t int_param(odin3_module *mod, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(mod, node, index);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, val->kind);
    return val->i;
}

static uint32_t live_nodes(odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(mod); i++) {
        count += odin3_node_live(mod, (odin3_node_id){i}) ? 1U : 0U;
    }
    return count;
}

/* --- M-5: type resolution -------------------------------------------------------------------- */

static void test_unknown_type_is_an_error(void) {
    rj_expect_error(top_json(IN_PORT("a", "[2]"),
                             "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [2]}}"),
                    "modules.top.cells.u: cell 'u' has type 'mystery', which is neither a module "
                    "of the file nor a registered cell type (use --auto-blackbox to treat it as a "
                    "black box)");
    rj_expect_error(top_json("", "\"u\": {\"parameters\": {}}"), "the cell has no type string");
    rj_expect_error(top_json("", "\"u\": 3"), "the cell has no type string");
    rj_expect_error(top_json("", "\"u\": {\"type\": \"$port_in\", \"connections\": {}}"),
                    "'$port_in' is a port type, not a cell");
}

static void test_auto_blackbox(void) {
    const odin3_json_opts opts = {.auto_blackbox = true};
    const char *json =
        top_json(IN_PORT("a", "[2, 3]") ", " OUT_PORT("y", "[4]"),
                 "\"u\": {\"type\": \"mystery\", \"port_directions\": {\"A\": \"input\", "
                 "\"Y\": \"output\"}, \"connections\": {\"A\": [2, 3], \"Y\": [4], \"E\": [5]}}, "
                 "\"v\": {\"type\": \"mystery\", \"connections\": {\"A\": [2, 3], \"E\": [5]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &opts));
    rj_log_has("implicit black box 'mystery': inferred port directions");
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(rj_design, rj_str("mystery"), &type));
    const odin3_celltype_def *def = odin3_celltype_get(rj_design, type);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    TEST_ASSERT_EQUAL_UINT32(3, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("A", def->ports[0].name);
    TEST_ASSERT_EQUAL_UINT32(2, def->ports[0].width);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, def->ports[0].dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[1].dir);
    /* E: no port_directions; both cells have it on bit 5, nothing else does: nothing drives it */
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, def->ports[2].dir);
    rj_log_has("A=input, E=input, Y=output; E: its net bit 5 has no driver");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_celltype_instances(rj_design, type));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(rj_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
    /* a type is warned about once */
    rj_log_has("implicit black box");
    TEST_ASSERT_NULL(strstr(strstr(rj_log, "implicit black box") + 1, "implicit black box"));
}

static void test_auto_blackbox_conflicts(void) {
    const odin3_json_opts opts = {.auto_blackbox = true};
    rj_expect_failure((rj_failure){
        top_json("", "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [2, 3]}}, "
                     "\"v\": {\"type\": \"mystery\", \"connections\": {\"A\": [2]}}"),
        "modules.top.cells.v: port 'A' is 1 bits here and 2 bits in an earlier cell", &opts});
    rj_expect_failure((rj_failure){
        top_json("", "\"u\": {\"type\": \"mystery\", \"port_directions\": {\"A\": \"up\"}, "
                     "\"connections\": {\"A\": [2]}}"),
        "'up' is not input, output or inout", &opts});
    /* an unregistered `$` type is still a warning that says so */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          rj_read_opts(top_json("", "\"u\": {\"type\": \"$alu\", \"connections\": "
                                                    "{\"A\": [2]}}"),
                                       &opts));
    rj_log_has("'$alu' is a Yosys internal cell type this tool has no cell for");
    rj_log_has("not equivalent to its source");
}

static void expect_type_dirs(const char *name, const odin3_dir *want, uint32_t n) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(rj_design, rj_str(name), &type));
    const odin3_celltype_def *def = odin3_celltype_get(rj_design, type);
    TEST_ASSERT_EQUAL_UINT32(n, def->n_ports);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(want[i], def->ports[i].dir, def->ports[i].name);
    }
}

static const odin3_json_opts k_auto = {.auto_blackbox = true};

/* EL-11 (2026-10-21): without port_directions a port's direction comes from its bits: an output on
 * a bit nothing else drives and something reads, an input on a module input or a driven bit; one
 * warning lists them. The text is the one the Verilog front end prints for the same design. */
static void test_implicit_infers_directions(void) {
    const char *json =
        top_json(IN_PORT("a", "[2]") ", " OUT_PORT("y", "[4]"),
                 "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], \"Q\": [4]}}, "
                 "\"v\": {\"type\": \"other\", \"connections\": {\"D\": [2], \"Q\": [5]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir mystery[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT};
    const odin3_dir other[] = {ODIN3_DIR_IN, ODIN3_DIR_IN};
    expect_type_dirs("mystery", mystery, 2);
    expect_type_dirs("other", other, 2);
    rj_log_has("implicit black box 'mystery': inferred port directions: A=input, Q=output");
    rj_log_has("implicit black box 'other': inferred port directions: D=input, Q=input");
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(rj_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Parity with the Verilog front end: test_elab_stage1_inst.c elaborates the same design
 * (module top (input a, output y); wire w; foo u (.A(a), .Q(w)); assign y = w;) and expects this
 * very text. */
static void test_implicit_infers_text_parity(void) {
    const char *json = top_json(IN_PORT("a", "[2]") ", " OUT_PORT("y", "[3]"),
                                "\"u\": {\"type\": \"foo\", \"connections\": {\"A\": [2], "
                                "\"Q\": [3]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    rj_log_has("implicit black box 'foo': inferred port directions: A=input, Q=output");
}

/* A bit driven by a known cell's output makes the unknown port an input; a module inout bit keeps
 * it inout and the warning says so. */
static void test_implicit_infers_known_and_inout(void) {
    const char *json =
        top_json(IN_PORT("a", "[2]") ", " OUT_PORT(
                     "y", "[5]") ", "
                                 "\"io\": {\"direction\": \"inout\", \"bits\": [6]}",
                 "\"g\": {\"type\": \"$not\", \"parameters\": {\"A_WIDTH\": 1, \"A_SIGNED\": 0, "
                 "\"Y_WIDTH\": 1}, \"connections\": {\"A\": [2], \"Y\": [3]}}, "
                 "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [3], \"Q\": [5], "
                 "\"P\": [6]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir want[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT, ODIN3_DIR_INOUT};
    expect_type_dirs("mystery", want, 3);
    rj_log_has("A=input, P=inout, Q=output; P: its net has no known direction");
}

/* The directions do not depend on the order of the cells (Yosys writes them alphabetically, and a
 * consumer is often before its producer). Nothing known tells which of mystery.Q and other.D drives
 * bit 3, but mystery has no output and other has one, so mystery.Q is chosen as the output. */
static void check_chain(const char *first, const char *second) {
    char both[1024];
    (void)snprintf(both, sizeof both, "%s, %s", first, second);
    const char *json = top_json(IN_PORT("a", "[2]") ", " OUT_PORT("y", "[4]"), both);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir mystery[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT};
    const odin3_dir other[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT};
    expect_type_dirs("mystery", mystery, 2);
    expect_type_dirs("other", other, 2);
    rj_log_has("implicit black box 'mystery': inferred port directions: A=input, Q=output; Q: "
               "chosen as output: the type had none");
    rj_log_has("implicit black box 'other': inferred port directions: D=input, Q=output");
}

static void test_implicit_infers_order_independent(void) {
    const char *prod = "\"b_prod\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], "
                       "\"Q\": [3]}}";
    const char *cons = "\"a_cons\": {\"type\": \"other\", \"connections\": {\"D\": [3], "
                       "\"Q\": [4]}}";
    check_chain(cons, prod);
    const char *prod2 = "\"a_prod\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], "
                        "\"Q\": [3]}}";
    const char *cons2 = "\"b_cons\": {\"type\": \"other\", \"connections\": {\"D\": [3], "
                        "\"Q\": [4]}}";
    check_chain(prod2, cons2);
}

/* Two types with no output share bit 3: neither is chosen (no guess by order or name). */
static void test_implicit_tie_two_types(void) {
    const char *json =
        top_json(IN_PORT("a", "[2]"),
                 "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], \"Q\": [3]}}, "
                 "\"v\": {\"type\": \"other\", \"connections\": {\"D\": [3], \"E\": [2]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir want[] = {ODIN3_DIR_IN, ODIN3_DIR_IN};
    expect_type_dirs("mystery", want, 2);
    expect_type_dirs("other", want, 2);
    rj_log_has("implicit black box 'mystery': inferred port directions: A=input, Q=input; Q: its "
               "net bit 3 has no driver");
}

/* A type with no output and two candidate ports stays ambiguous. */
static void test_implicit_tie_two_ports(void) {
    const char *json = top_json(
        IN_PORT("a", "[2]") ", " OUT_PORT("y", "[5]"),
        "\"u\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], \"P\": [3], \"Q\": [4]}}, "
        "\"v\": {\"type\": \"other\", \"connections\": {\"D\": [3], \"E\": [4], \"Q\": [5]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir want[] = {ODIN3_DIR_IN, ODIN3_DIR_IN, ODIN3_DIR_IN};
    expect_type_dirs("mystery", want, 3);
    rj_log_has("implicit black box 'mystery': inferred port directions: A=input, P=input, "
               "Q=input; P: its net bit 3 has no driver; Q: its net bit 4 has no driver");
}

/* A floating input at one cell casts no vote; one connection with an input bit and an output bit,
 * or a module inout bit and an output bit, is an inout; a constant is a known driver. */
static void test_implicit_infers_floating_slices_constants(void) {
    const char *json =
        top_json(IN_PORT("a", "[2]") ", " OUT_PORT(
                     "y", "[3, 6, 7]") ", "
                                       "\"io\": {\"direction\": \"inout\", \"bits\": [5]}",
                 "\"u1\": {\"type\": \"foo\", \"connections\": {\"A\": [2], \"Q\": [3], "
                 "\"X\": [2, 6], \"P\": [5, 7], \"K\": [\"1\", \"0\"]}}, "
                 "\"u2\": {\"type\": \"foo\", \"connections\": {\"A\": [11], \"Q\": [12], "
                 "\"X\": [2, 13], \"P\": [5, 14], \"K\": [\"0\", \"1\"]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    const odin3_dir want[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT, ODIN3_DIR_INOUT, ODIN3_DIR_INOUT,
                              ODIN3_DIR_IN};
    expect_type_dirs("foo", want, 5);
    rj_log_has("implicit black box 'foo': inferred port directions: A=input, K=input, P=inout, "
               "Q=output, X=inout; P: its net has no known direction; X: an input at some "
               "instances, an output at others");
}

/* Two ports that are outputs by their own cells and share a bit are a conflict: the warning names
 * both, whatever the order (the net with two drivers is then an error of the reader). */
static void check_conflict(const char *last, const char *prior) {
    char cells[1024];
    (void)snprintf(cells, sizeof cells,
                   "\"a1\": {\"type\": \"foo\", \"connections\": {\"Q\": [3]}}, "
                   "\"a2\": {\"type\": \"bar\", \"connections\": {\"Z\": [4]}}, %s, %s",
                   last, prior);
    const char *json = top_json(OUT_PORT("y", "[3, 4, 5]"), cells);
    /* the reader then refuses the net with two drivers; the warnings come first */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rj_read_opts(json, &k_auto));
    rj_log_has("net is driven by 2 cell outputs");
    rj_log_has("implicit black box 'foo': inferred port directions: Q=output; Q: its net is also "
               "driven by 'bar.Z'");
    rj_log_has("implicit black box 'bar': inferred port directions: Z=output; Z: its net is also "
               "driven by 'foo.Q'");
}

static void test_implicit_infers_conflict_both_ports(void) {
    const char *foo = "\"b1\": {\"type\": \"foo\", \"connections\": {\"Q\": [5]}}";
    const char *bar = "\"b2\": {\"type\": \"bar\", \"connections\": {\"Z\": [5]}}";
    check_conflict(foo, bar);
    check_conflict(bar, foo);
}

/* Three outputs on one bit: each names the smallest other (type, port) and the port list is sorted
 * by name, whatever the order of the cells, of their connections, or of the JSON keys. */
static void check_conflict_three(const char *cells) {
    const char *json = top_json(OUT_PORT("y", "[3, 4, 5, 6]"), cells);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rj_read_opts(json, &k_auto));
    rj_log_has("implicit black box 'bar': inferred port directions: Z=output; Z: its net is also "
               "driven by 'baz.W'");
    rj_log_has("implicit black box 'baz': inferred port directions: W=output; W: its net is also "
               "driven by 'bar.Z'");
    rj_log_has("implicit black box 'foo': inferred port directions: P=input, Q=output; Q: its net "
               "is also driven by 'bar.Z'");
}

static void test_implicit_conflict_three_orders(void) {
    const char *foo1 = "\"a1\": {\"type\": \"foo\", \"connections\": {\"P\": [\"1\"], \"Q\": [3]}}";
    const char *foo2 = "\"a1\": {\"type\": \"foo\", \"connections\": {\"Q\": [3], \"P\": [\"1\"]}}";
    const char *bar = "\"a2\": {\"type\": \"bar\", \"connections\": {\"Z\": [4]}}";
    const char *baz = "\"a3\": {\"type\": \"baz\", \"connections\": {\"W\": [5]}}";
    const char *fs = "\"b1\": {\"type\": \"foo\", \"connections\": {\"Q\": [6], \"P\": [\"1\"]}}";
    const char *fs2 = "\"b1\": {\"type\": \"foo\", \"connections\": {\"P\": [\"1\"], \"Q\": [6]}}";
    const char *bs = "\"b2\": {\"type\": \"bar\", \"connections\": {\"Z\": [6]}}";
    const char *zs = "\"b3\": {\"type\": \"baz\", \"connections\": {\"W\": [6]}}";
    const char *order[3][6] = {
        {foo1, bar, baz, fs, bs, zs}, {zs, bs, fs2, baz, foo2, bar}, {bs, fs, zs, bar, baz, foo1}};
    for (size_t k = 0; k < 3; k++) {
        char cells[2048];
        (void)snprintf(cells, sizeof cells, "%s, %s, %s, %s, %s, %s", order[k][0], order[k][1],
                       order[k][2], order[k][3], order[k][4], order[k][5]);
        check_conflict_three(cells);
    }
}

/* A port that is an input to two bits nothing drives names the smallest label, whatever the order
 * of the cells and the connections. */
static void check_two_bits(const char *cells) {
    const char *json = top_json(IN_PORT("a", "[2]"), cells);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(json, &k_auto));
    rj_log_has("implicit black box 'mystery': inferred port directions: A=input, P=input; P: its "
               "net bit 3 has no driver");
}

static void test_implicit_undriven_two_bits_orders(void) {
    check_two_bits("\"u1\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], \"P\": [3]}}, "
                   "\"v1\": {\"type\": \"other\", \"connections\": {\"D\": [3], \"E\": [2]}}, "
                   "\"u2\": {\"type\": \"mystery\", \"connections\": {\"A\": [2], \"P\": [4]}}, "
                   "\"v2\": {\"type\": \"other\", \"connections\": {\"D\": [4], \"E\": [2]}}");
    check_two_bits("\"u2\": {\"type\": \"mystery\", \"connections\": {\"P\": [4], \"A\": [2]}}, "
                   "\"v2\": {\"type\": \"other\", \"connections\": {\"E\": [2], \"D\": [4]}}, "
                   "\"u1\": {\"type\": \"mystery\", \"connections\": {\"P\": [3], \"A\": [2]}}, "
                   "\"v1\": {\"type\": \"other\", \"connections\": {\"E\": [2], \"D\": [3]}}");
}

/* A type the IR refuses (a port with an empty name) is a located error naming the first cell. */
static void test_implicit_type_refused(void) {
    const odin3_json_opts opts = {.auto_blackbox = true};
    rj_expect_failure(
        (rj_failure){top_json("", "\"u\": {\"type\": \"mystery\", \"connections\": {\"\": [2]}}"),
                     "modules.top.cells.u: cannot make the implicit black box 'mystery'", &opts});
    rj_log_has("in.json:1:");
}

/* A name that would not fit its buffer is a located error, never a truncated key. */
static void test_long_names(void) {
    char name[400];
    memset(name, 'k', sizeof name - 1);
    name[sizeof name - 1] = '\0';
    char json[2048];
    (void)snprintf(json, sizeof json, "{\"modules\": {\"top\": {\"attributes\": {\"%s\": 1}}}}",
                   name);
    rj_expect_error(json, "is longer than");
    (void)snprintf(
        json, sizeof json,
        "{\"modules\": {\"top\": {\"memories\": {\"m\": {\"attributes\": {\"%s\": 1}}}}}}", name);
    rj_expect_error(json, "is longer than");
    (void)snprintf(json, sizeof json,
                   "{\"modules\": {\"top\": {\"memories\": {\"%s\": {\"width\": 1}}}}}", name);
    rj_expect_error(json, "the name of the memory attribute is too long");
    (void)snprintf(json, sizeof json,
                   "{\"modules\": {\"sub\": {}, \"top\": {\"cells\": {\"u\": {\"type\": \"sub\", "
                   "\"parameters\": {\"%s\": 1}, \"connections\": {}}}}}}",
                   name);
    rj_expect_error(json, "is longer than");
}

static void test_scopeinfo_is_dropped(void) {
    rj_expect_ok(top_json("",
                          "\"$scope\": {\"type\": \"$scopeinfo\", \"parameters\": {\"TYPE\": "
                          "\"module\"}, \"attributes\": {\"src\": \"x\"}, \"connections\": {}}"));
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(0, live_nodes(mod));
    rj_log_has("dropped 1 $scopeinfo cells");
}

/* A tech-library type loaded beforehand is a registered cell type with typed parameters. */
static void test_techlib_type(void) {
    rj_fresh();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_techlib_read(rj_design, ODIN3_LIB_DIR "/lpm.o3lib"));
    rj_log_clear();
    const char *json = top_json(
        IN_PORT("x", "[2, 3]") ", " IN_PORT("z", "[4, 5]") ", " OUT_PORT("s", "[6, 7]"),
        "\"g\": {\"type\": \"lpm_add_sub\", \"parameters\": {\"lpm_width\": "
        "\"00000000000000000000000000000010\", \"lpm_direction\": \"ADD\"}, \"connections\": "
        "{\"dataa\": [2, 3], \"datab\": [4, 5], \"result\": [6, 7]}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_read(rj_design, rj_put("lib.json", json), NULL));
    odin3_module *mod = rj_module("top");
    odin3_node_id node = the_cell(mod);
    odin3_celltype_id type = odin3_node_type(mod, node);
    const odin3_celltype_def *def = odin3_celltype_get(rj_design, type);
    TEST_ASSERT_EQUAL_STRING("lpm_add_sub", def->name);
    TEST_ASSERT_EQUAL_INT64(2, int_param(mod, node, 1));
}

/* --- M-6: parameters ------------------------------------------------------------------------- */

static void test_int_parameters(void) {
    static const char *const k_widths[] = {
        "\"A_WIDTH\": \"00000000000000000000000000000010\", \"B_WIDTH\": 2, \"Y_WIDTH\": \"10\"",
        "\"A_WIDTH\": \"0000000000000000000000000000000000000000000000000000000000000010\", "
        "\"B_WIDTH\": \"0010\", \"Y_WIDTH\": 2"};
    for (size_t i = 0; i < sizeof k_widths / sizeof k_widths[0]; i++) {
        rj_expect_ok(top_json(AND_PORTS, and_cell(k_widths[i], AND2_CONNS, "")));
        odin3_module *mod = rj_module("top");
        odin3_node_id node = the_cell(mod);
        TEST_ASSERT_EQUAL_INT64(2, int_param(mod, node, 2));
        TEST_ASSERT_EQUAL_INT64(2, int_param(mod, node, 3));
        TEST_ASSERT_EQUAL_INT64(2, int_param(mod, node, 4));
    }
}

/* A default for an omitted parameter: a 1-bit $and. */
static void test_default_parameters(void) {
    rj_expect_ok(top_json(IN_PORT("a", "[2]") ", " IN_PORT("b", "[3]") ", " OUT_PORT("y", "[4]"),
                          and_cell("", "\"A\": [2], \"B\": [3], \"Y\": [4]", "")));
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_INT64(1, int_param(mod, the_cell(mod), 2));
}

/* $mem: a negative INT, STRING with Yosys's blank, empty BITS. */
static void test_mem_parameters(void) {
    static const char *const k_mem_open =
        "\"g\": {\"type\": \"$mem\", \"parameters\": {\"MEMID\": \"%s\", "
        "\"SIZE\": \"00000000000000000000000000000100\", "
        "\"OFFSET\": \"11111111111111111111111111111110\", "
        "\"ABITS\": 2, \"WIDTH\": 8, \"RD_PORTS\": 0, \"WR_PORTS\": 0, "
        "\"RD_CLK_ENABLE\": \"\", \"RD_CLK_POLARITY\": \"\", \"RD_TRANSPARENT\": \"\", "
        "\"WR_CLK_ENABLE\": \"\", \"WR_CLK_POLARITY\": \"\", \"INIT\": \"\"}, "
        "\"connections\": {}}";
    static const struct {
        const char *given;
        const char *kept;
    } k_ids[] = {{"abc", "abc"}, {"10 ", "10"}, {"1x", "1x"}};
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; i++) {
        char cell[BUF];
        (void)snprintf(cell, sizeof cell, k_mem_open, k_ids[i].given);
        rj_expect_ok(top_json("", cell));
        odin3_module *mod = rj_module("top");
        odin3_node_id node = the_cell(mod);
        TEST_ASSERT_EQUAL_INT64(-2, int_param(mod, node, 2));
        const odin3_value *memid = odin3_node_param(mod, node, 0);
        TEST_ASSERT_EQUAL_STRING(k_ids[i].kept,
                                 odin3_strtab_get(odin3_design_strtab(rj_design), memid->str));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_node_param(mod, node, 12)->len);
    }
}

/* BITS with x and z: an $adff's reset value. */
static void test_bits_parameter(void) {
    rj_expect_ok(
        top_json(IN_PORT("c", "[2]") ", " IN_PORT("r", "[3]") ", " IN_PORT(
                     "d", "[4, 5]") ", " OUT_PORT("q", "[6, 7]"),
                 "\"g\": {\"type\": \"$adff\", \"parameters\": {\"WIDTH\": 2, \"CLK_POLARITY\": 1, "
                 "\"ARST_POLARITY\": 1, \"ARST_VALUE\": \"1x\"}, \"connections\": {\"CLK\": [2], "
                 "\"ARST\": [3], \"D\": [4, 5], \"Q\": [6, 7]}}"));
    odin3_module *mod = rj_module("top");
    const odin3_value *val = odin3_node_param(mod, the_cell(mod), 3);
    TEST_ASSERT_EQUAL_UINT32(2, val->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, val->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, val->bits[1]);
}

/* COVER rows joined by a newline: the generic $sop (no TABLE). */
static void test_cover_parameter(void) {
    rj_expect_ok(top_json(
        IN_PORT("a", "[2, 3]") ", " OUT_PORT("y", "[4]"),
        "\"g\": {\"type\": \"$sop\", \"parameters\": {\"WIDTH\": 2, \"COVER\": \"01 1\\n1- 1\"}, "
        "\"connections\": {\"A\": [2, 3], \"Y\": [4]}}"));
    odin3_module *mod = rj_module("top");
    const odin3_value *cover = odin3_node_param(mod, the_cell(mod), 1);
    TEST_ASSERT_EQUAL_UINT32(2, cover->cover_inputs);
    TEST_ASSERT_EQUAL_UINT32(6, cover->len);
    TEST_ASSERT_EQUAL_MEMORY("011"
                             "1-1",
                             cover->bits, 6);
}

static void test_parameter_errors(void) {
    rj_expect_error(top_json(AND_PORTS, and_cell("\"FOO\": 1", AND2_CONNS, "")),
                    "modules.top.cells.g.parameters.FOO: '$and' has no parameter 'FOO'");
    rj_expect_error(top_json(AND_PORTS, and_cell("\"A_WIDTH\": \"abc\"", AND2_CONNS, "")),
                    "modules.top.cells.g.parameters.A_WIDTH: cannot read 'abc' as an integer");
    rj_expect_error(top_json(AND_PORTS, and_cell("\"A_WIDTH\": \"\"", AND2_CONNS, "")),
                    "cannot read '' as an integer");
    rj_expect_error(top_json(AND_PORTS, and_cell("\"A_WIDTH\": [2]", AND2_CONNS, "")),
                    "a parameter value is a string or an integer");
    rj_expect_error(
        top_json("", "\"g\": {\"type\": \"$adff\", \"parameters\": {\"ARST_VALUE\": \"1q\"}, "
                     "\"connections\": {}}"),
        "cannot read '1q' as a bit vector");
    rj_expect_error(top_json("", "\"g\": {\"type\": \"$and\", \"parameters\": 3, "
                                 "\"connections\": {}}"),
                    "parameters is not an object");
    rj_expect_error(
        top_json("", "\"g\": {\"type\": \"$sop\", \"parameters\": {\"WIDTH\": 1, \"COVER\": "
                     "\"2 1\"}, \"connections\": {}}"),
        "cannot read '2 1' as a cover");
}

/* The type's verify hook failing is a located error on the cell. */
static void test_verify_failure(void) {
    rj_expect_error(
        top_json(IN_PORT("a", "[2, 3]") ", " OUT_PORT("y", "[4]"),
                 "\"g\": {\"type\": \"$lut\", \"parameters\": {\"WIDTH\": 2, \"LUT\": \"01x0\"}, "
                 "\"connections\": {\"A\": [2, 3], \"Y\": [4]}}"),
        "modules.top.cells.g: the cell of type '$lut' is refused");
    rj_log_has("$lut: LUT bit 1 is x; only 0 and 1 are allowed");
}

/* --- M-7: connections ------------------------------------------------------------------------ */

static void test_connection_errors(void) {
    rj_expect_error(top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"Q\": [2]", "")),
                    "modules.top.cells.g.connections.Q: '$and' has no port 'Q'");
    rj_expect_error(top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"A\": [2], \"B\": [4, 5]", "")),
                    "modules.top.cells.g.connections.A: 1 bits, but the port 'A' of '$and' is 2 "
                    "bits wide for these parameters");
    rj_expect_error(top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"A\": 3", "")),
                    "bits is not an array");
    rj_expect_error(
        top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"A\": [2, 3], \"Y\": [\"1\", 7]", "")),
        "a constant on the output port of a '$and'");
}

/* A width mismatch creates no node (no resize). */
static void test_mismatch_creates_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          rj_read(top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"A\": [2]", ""))));
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(odin3_module_port_count(mod), live_nodes(mod));
}

/* An open port stays open; x and z on an output are the open pin. */
static void test_open_ports(void) {
    rj_expect_ok(
        top_json(AND_PORTS, and_cell(AND2_PARAMS, "\"A\": [2, 3], \"Y\": [\"x\", \"z\"]", "")));
    odin3_module *mod = rj_module("top");
    odin3_node_id node = the_cell(mod);
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_pin_net(mod, odin3_node_port(mod, node, 1).first)));
    TEST_ASSERT_FALSE(odin3_net_valid(odin3_pin_net(mod, odin3_node_port(mod, node, 2).first)));
    TEST_ASSERT_TRUE(odin3_net_valid(odin3_pin_net(mod, odin3_node_port(mod, node, 0).first)));
}

/* The writer writes the net of a constant-table $lut as the constant: the output pin stays open. */
static void test_constant_lut_output(void) {
    rj_expect_ok(
        top_json(IN_PORT("a", "[2, 3]") ", " OUT_PORT("y", "[\"1\"]"),
                 "\"g\": {\"type\": \"$lut\", \"parameters\": {\"WIDTH\": 2, \"LUT\": \"1111\"}, "
                 "\"connections\": {\"A\": [2, 3], \"Y\": [\"1\"]}}"));
}

static void test_port_directions(void) {
    rj_expect_ok(top_json(AND_PORTS,
                          and_cell(AND2_PARAMS, AND2_CONNS,
                                   "\"port_directions\": {\"A\": \"input\", \"Y\": \"output\"},")));
    rj_expect_error(top_json(AND_PORTS, and_cell(AND2_PARAMS, AND2_CONNS,
                                                 "\"port_directions\": {\"A\": \"output\"},")),
                    "modules.top.cells.g.port_directions.A: direction 'output', but the port 'A' "
                    "of '$and' is an input");
    rj_expect_error(top_json(AND_PORTS, and_cell(AND2_PARAMS, AND2_CONNS,
                                                 "\"port_directions\": {\"Z\": "
                                                 "\"input\"},")),
                    "port_directions.Z: '$and' has no port 'Z'");
    rj_expect_error(
        top_json(AND_PORTS, and_cell(AND2_PARAMS, AND2_CONNS, "\"port_directions\": 3,")),
        "port_directions is not an object");
}

/* --- E-8: second drivers --------------------------------------------------------------------- */

static void test_second_driver(void) {
    rj_expect_error(top_json(IN_PORT("a", "[2]") ", " OUT_PORT("y", "[3]"),
                             "\"g\": {\"type\": \"$_NOT_\", \"connections\": {\"A\": [2], "
                             "\"Y\": [3]}}, "
                             "\"h\": {\"type\": \"$_NOT_\", \"connections\": {\"A\": [2], "
                             "\"Y\": [3]}}"),
                    "modules.top.cells.h.connections.Y[0]: net is driven by 2 cell outputs (one "
                    "allowed)");
    rj_expect_error(top_json(IN_PORT("a", "[2]"),
                             "\"g\": {\"type\": \"$_NOT_\", \"connections\": {\"A\": [2], "
                             "\"Y\": [2]}}"),
                    "net is driven by 2 cell outputs (one allowed)");
}

/* --- M-13, M-14: memories and hierarchy ------------------------------------------------------ */

static void test_memories(void) {
    rj_expect_ok(
        "{\"modules\": {\"top\": {\"memories\": {\"m\": {\"hide_name\": 0, \"width\": 8, "
        "\"size\": 16, \"start_offset\": 0, \"attributes\": {\"ram_style\": \"block\"}}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_objref self = {ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v};
    const odin3_value *width = odin3_attr_get(mod, self, rj_str("memory:m:width"));
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT64(8, width->i);
    TEST_ASSERT_NOT_NULL(odin3_attr_get(mod, self, rj_str("memory:m:size")));
    TEST_ASSERT_NOT_NULL(odin3_attr_get(mod, self, rj_str("memory:m:attribute:ram_style")));
    rj_expect_error("{\"modules\": {\"top\": {\"memories\": {\"m\": {\"width\": -1}}}}}",
                    "modules.top.memories.m.width: width is not a non-negative integer");
    rj_expect_error("{\"modules\": {\"top\": {\"memories\": 5}}}", "memories is not an object");
}

/* A $memrd of the registered shape reads. */
static void test_memrd_reads(void) {
    rj_expect_ok(
        top_json(IN_PORT("a", "[2, 3]") ", " OUT_PORT("d", "[4, 5, 6, 7]"),
                 "\"g\": {\"type\": \"$memrd\", \"parameters\": {\"MEMID\": \"m\", \"ABITS\": 2, "
                 "\"WIDTH\": 4, \"CLK_ENABLE\": 0, \"CLK_POLARITY\": 0, \"TRANSPARENT\": 0}, "
                 "\"connections\": {\"ADDR\": [2, 3], \"DATA\": [4, 5, 6, 7]}}"));
}

static void test_module_cycle(void) {
    rj_expect_error("{\"modules\": {\"a\": {\"cells\": {\"u\": {\"type\": \"b\", \"connections\": "
                    "{}}}}, \"b\": {\"cells\": {\"u\": {\"type\": \"a\", \"connections\": {}}}}}}",
                    "module 'a' instantiates itself through 'b'");
    rj_expect_error("{\"modules\": {\"a\": {\"cells\": {\"u\": {\"type\": \"a\", \"connections\": "
                    "{}}}}}}",
                    "module 'a' instantiates itself");
}

/* A parent before or after its child in the file both read (M-0). */
static void test_instance_order_free(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"cells\": {\"u\": {\"type\": \"sub\", "
                 "\"parameters\": {\"W\": \"00000000000000000000000000001000\"}, "
                 "\"connections\": {\"i\": [2]}}}}, \"sub\": {\"ports\": {\"i\": {\"direction\": "
                 "\"input\", \"bits\": [2]}}}}}");
    TEST_ASSERT_EQUAL_STRING("top", rj_top_name());
    odin3_module *mod = rj_module("top");
    odin3_node_id inst = rj_node(mod, "u");
    const odin3_value *val =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, inst.v}, rj_str("parameter:W"));
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, val->kind);
    TEST_ASSERT_EQUAL_UINT32(32, val->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, val->bits[3]);
}

/* --- M-11: init --------------------------------------------------------------------------- */

static const odin3_value *wire_attr(odin3_module *mod, const char *wire, const char *key) {
    return odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_WIRE, rj_wire(mod, wire).v}, rj_str(key));
}

/* init of a netname a Q drives is the latch's INIT; otherwise it stays an attribute. */
static void test_init_attribute(void) {
    rj_expect_ok(top_json(
        IN_PORT("c", "[2]") ", " IN_PORT("d", "[3]"),
        "\"l1\": {\"type\": \"$_DFF_P_\", \"connections\": {\"C\": [2], \"D\": [3], \"Q\": [4]}}, "
        "\"l2\": {\"type\": \"$_DFF_P_\", \"connections\": {\"C\": [2], \"D\": [3], \"Q\": [5]}}"));
    odin3_module *plain = rj_module("top");
    TEST_ASSERT_EQUAL_INT64(3, int_param(plain, rj_node(plain, "l1"), 0));
    const char *json =
        "{\"modules\": {\"top\": {\"ports\": {\"c\": {\"direction\": \"input\", \"bits\": [2]}, "
        "\"d\": {\"direction\": \"input\", \"bits\": [3]}}, \"cells\": {"
        "\"l1\": {\"type\": \"$_DFF_P_\", \"connections\": {\"C\": [2], \"D\": [3], \"Q\": [4]}}, "
        "\"l2\": {\"type\": \"$_DFF_P_\", \"connections\": {\"C\": [2], \"D\": [3], \"Q\": [5]}}},"
        "\"netnames\": {\"q1\": {\"bits\": [4], \"attributes\": {\"init\": \"1\"}}, "
        "\"q2\": {\"bits\": [5], \"attributes\": {\"init\": \"x\"}}, "
        "\"other\": {\"bits\": [9, 10], \"attributes\": {\"init\": \"10\"}}}}}}";
    rj_expect_ok(json);
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_INT64(1, int_param(mod, rj_node(mod, "l1"), 0));
    TEST_ASSERT_EQUAL_INT64(3, int_param(mod, rj_node(mod, "l2"), 0));
    TEST_ASSERT_NULL(wire_attr(mod, "q1", "init"));
    TEST_ASSERT_NULL(wire_attr(mod, "q2", "init"));
    TEST_ASSERT_NOT_NULL(wire_attr(mod, "other", "init"));
}

/* --- E-3: limits --------------------------------------------------------------------------- */

static void test_bits_limit(void) {
    size_t size = (size_t)BITS_OVER * 2 + 256;
    char *json = odin3_util_malloc(size);
    TEST_ASSERT_NOT_NULL(json);
    size_t at = (size_t)snprintf(json, size,
                                 "{\"modules\": {\"top\": {\"ports\": {\"a\": "
                                 "{\"direction\": \"input\", \"bits\": [");
    for (uint32_t i = 0; i < BITS_OVER; i++) {
        json[at++] = '2';
        json[at++] = ',';
    }
    at--; /* the last comma */
    (void)snprintf(json + at, size - at, "]}}}}}");
    rj_expect_error(json, "modules.top.ports.a.bits: 1048577 bits (1 to 1048576 are allowed)");
    odin3_util_free(json);
}

/* --- $sop: all 256 functions of three inputs ----------------------------------------------- */

static void sop_cell(uint32_t func, char *out, size_t size) {
    char table[BUF];
    size_t len = 0;
    uint32_t depth = 0;
    for (uint32_t row = MINTERMS; row > 0; row--) {
        uint32_t idx = row - 1;
        if (((func >> idx) & 1U) == 0) {
            continue;
        }
        depth++;
        for (uint32_t in = SOP_INPUTS; in > 0; in--) {
            const char *lit = ((idx >> (in - 1)) & 1U) != 0 ? "10" : "01";
            table[len++] = lit[0];
            table[len++] = lit[1];
        }
    }
    table[len] = '\0';
    char digits[33];
    for (uint32_t i = 0; i < 32; i++) {
        digits[i] = ((depth >> (31 - i)) & 1U) != 0 ? '1' : '0';
    }
    digits[32] = '\0';
    (void)snprintf(out, size,
                   "\"g\": {\"type\": \"$sop\", \"parameters\": {\"DEPTH\": \"%s\", \"TABLE\": "
                   "\"%s\", \"WIDTH\": \"00000000000000000000000000000011\"}, \"connections\": "
                   "{\"A\": [2, 3, 4], \"Y\": [5]}}",
                   digits, table);
}

static void expected_lut(uint32_t func, char *out) {
    for (uint32_t i = 0; i < MINTERMS; i++) {
        out[i] = ((func >> (7 - i)) & 1U) != 0 ? '1' : '0';
    }
    out[MINTERMS] = '\0';
}

static void test_sop_all_functions(void) {
    const char *ports = IN_PORT("a", "[2, 3, 4]") ", " OUT_PORT("y", "[5]");
    for (uint32_t func = 0; func < SOP_FUNCS; func++) {
        char cell[BUF];
        sop_cell(func, cell, sizeof cell);
        rj_expect_ok(top_json(ports, cell));
        const char *out = rj_path("sop.json");
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, out));
        char *text = rj_slurp(out);
        char want[BUF];
        char lut[16];
        expected_lut(func, lut);
        (void)snprintf(want, sizeof want, "\"LUT\": \"%s\"", lut);
        if (strstr(text, want) == NULL) {
            char msg[BUF * 2];
            (void)snprintf(msg, sizeof msg, "function %u: missing %s in\n%s", (unsigned)func, want,
                           text);
            TEST_FAIL_MESSAGE(msg);
        }
        odin3_util_free(text);
        (void)remove(out);
    }
}

/* Don't-care literals (00): the rows 1-0 and -11. */
static void test_sop_dont_care(void) {
    rj_expect_ok(top_json(
        IN_PORT("a", "[2, 3, 4]") ", " OUT_PORT("y", "[5]"),
        "\"g\": {\"type\": \"$sop\", \"parameters\": {\"DEPTH\": 2, \"TABLE\": "
        "\"101000010010\", \"WIDTH\": 3}, \"connections\": {\"A\": [2, 3, 4], \"Y\": [5]}}"));
    odin3_module *mod = rj_module("top");
    const odin3_value *cover = odin3_node_param(mod, the_cell(mod), 1);
    TEST_ASSERT_EQUAL_UINT32(8, cover->len);
    TEST_ASSERT_EQUAL_MEMORY("1-01"
                             "-111",
                             cover->bits, 8);
}

/* --- T-6: size, depth, out of memory -----------------------------------------------------------
 */

/* One cell with ports wider together than the reader's cap (2^22 bits). */
static void test_cell_width_limit(void) {
    rj_expect_error(
        top_json(
            "",
            "\"g\": {\"type\": \"$mem\", \"parameters\": {\"MEMID\": \"m\", \"ABITS\": 1048576, "
            "\"WIDTH\": 1048576, \"RD_PORTS\": 1, \"WR_PORTS\": 1, \"SIZE\": 2}, "
            "\"connections\": {}}"),
        "has no valid width for these parameters (or the cell is wider than the reader's limit)");
}

/* A 200,000-deep file is the depth error, in bounded stack (the parser is iterative). */
static void test_deep_file(void) {
    enum { DEEP = 200000 };
    char *json = odin3_util_malloc(DEEP + 1);
    TEST_ASSERT_NOT_NULL(json);
    memset(json, '[', DEEP);
    json[DEEP] = '\0';
    rj_expect_error(json, "JSON nested deeper than 32");
    odin3_util_free(json);
}

static const char *const FEATURE_JSON =
    "{\"creator\": \"t\", \"modules\": {"
    "\"sub\": {\"attributes\": {\"k\": \"v\"}, \"ports\": {\"i\": {\"direction\": \"input\", "
    "\"bits\": [2]}}, \"netnames\": {\"i\": {\"bits\": [2], \"attributes\": {\"src\": "
    "\"a.v:1\"}}}},"
    "\"bb\": {\"attributes\": {\"blackbox\": 1}, \"ports\": {\"p\": {\"direction\": \"input\", "
    "\"bits\": [2, 3]}}},"
    "\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", \"bits\": [2, 3]}, "
    "\"c\": {\"direction\": \"input\", \"bits\": [4]}, "
    "\"y\": {\"direction\": \"output\", \"bits\": [5, \"1\"]}},"
    "\"cells\": {"
    "\"g\": {\"type\": \"$and\", \"parameters\": {\"A_WIDTH\": 2, \"B_WIDTH\": 2, \"Y_WIDTH\": 2}, "
    "\"connections\": {\"A\": [2, 3], \"B\": [\"1\", \"x\"], \"Y\": [6, 7]}},"
    "\"l\": {\"type\": \"$_DFF_P_\", \"connections\": {\"C\": [4], \"D\": [6], \"Q\": [8]}},"
    "\"s\": {\"type\": \"$sop\", \"parameters\": {\"DEPTH\": 1, \"TABLE\": \"1001\", \"WIDTH\": "
    "2}, "
    "\"connections\": {\"A\": [6, 7], \"Y\": [5]}},"
    "\"u\": {\"type\": \"sub\", \"parameters\": {\"W\": 3}, \"connections\": {\"i\": [4]}},"
    "\"b\": {\"type\": \"bb\", \"connections\": {\"p\": [2, 3]}},"
    "\"m\": {\"type\": \"mystery\", \"connections\": {\"A\": [2]}},"
    "\"k\": {\"type\": \"$lut\", \"parameters\": {\"WIDTH\": 0, \"LUT\": \"1\"}, "
    "\"connections\": {\"A\": [], \"Y\": [9]}}},"
    "\"memories\": {\"mem\": {\"width\": 8, \"size\": 4}},"
    "\"netnames\": {\"q\": {\"bits\": [8], \"attributes\": {\"init\": \"1\"}}, "
    "\"w\": {\"bits\": [6, 7], \"attributes\": {\"keep\": 1}}}}}}";

/* Every allocation of a read can fail: out of memory, no leak (ASan checks), never a crash. */
static void test_oom_sweep(void) {
    enum { SWEEP_LIMIT = 20000 };
    const odin3_json_opts opts = {.auto_blackbox = true};
    const char *path = rj_put("oom.json", FEATURE_JSON);
    rj_fresh();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_read(rj_design, path, &opts));
    odin3_log_set_level(ODIN3_LOG_ERROR);
    long fail_at = 0;
    for (; fail_at < SWEEP_LIMIT; fail_at++) {
        rj_fresh();
        rj_log_clear();
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_json_read(rj_design, path, &opts);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, rj_log);
    }
    TEST_ASSERT_TRUE(fail_at > 50);
    TEST_ASSERT_TRUE(fail_at < SWEEP_LIMIT);
}

/* E-5: a chain of cells (default 1,000,000, $ODIN3_RJ_CHAIN) reads and passes check. */
static void test_cell_chain(void) {
    const char *env = getenv("ODIN3_RJ_CHAIN");
    const uint32_t count = env != NULL ? (uint32_t)strtoul(env, NULL, 10) : 1000000U;
    const char *path = rj_path("chain.json");
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fprintf(fp,
                  "{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", "
                  "\"bits\": [2]}, \"y\": {\"direction\": \"output\", \"bits\": [%u]}}, "
                  "\"cells\": {\n",
                  (unsigned)count + 2);
    for (uint32_t i = 0; i < count; i++) {
        (void)fprintf(fp,
                      "%s\"c%u\": {\"type\": \"$_NOT_\", \"connections\": {\"A\": [%u], "
                      "\"Y\": [%u]}}\n",
                      i > 0 ? "," : "", (unsigned)i, (unsigned)i + 2, (unsigned)i + 3);
    }
    (void)fprintf(fp, "}}}}\n");
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    rj_fresh();
    clock_t start = clock();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_read(rj_design, path, NULL));
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    char msg[RJ_PATH];
    (void)snprintf(msg, sizeof msg, "read %u cells in %.2f s", (unsigned)count, secs);
    TEST_MESSAGE(msg);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(rj_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* A module with the name of a cell type is refused (a `$` name is no module). */
static void test_module_named_like_a_type(void) {
    rj_expect_error("{\"modules\": {\"$and\": {}}}",
                    "modules.$and: module '$and' has the name of a "
                    "cell type");
}

/* Several black boxes and one module: the module is the top. */
static void test_top_among_blackboxes(void) {
    rj_expect_ok("{\"modules\": {\"b1\": {\"attributes\": {\"blackbox\": 1}}, "
                 "\"b2\": {\"attributes\": {\"blackbox\": 1}}, \"top\": {}, "
                 "\"b3\": {\"attributes\": {\"blackbox\": \"1\"}}}}");
    TEST_ASSERT_EQUAL_STRING("top", rj_top_name());
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(rj_design));
}

/* A chain of 50,000 modules, the last instantiating the first: found by the iterative search. */
static void test_long_module_chain(void) {
    enum { CHAIN = 50000 };
    const char *path = rj_path("modchain.json");
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fprintf(fp, "{\"modules\": {\n");
    for (uint32_t i = 0; i < CHAIN; i++) {
        (void)fprintf(fp,
                      "%s\"m%u\": {\"cells\": {\"u\": {\"type\": \"m%u\", \"connections\": {}}}}\n",
                      i > 0 ? "," : "", (unsigned)i, (unsigned)((i + 1) % CHAIN));
    }
    (void)fprintf(fp, "}}\n");
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    rj_fresh();
    rj_log_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_json_read(rj_design, path, NULL));
    rj_log_has("instantiates itself through");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_module_named_like_a_type);
    RUN_TEST(test_top_among_blackboxes);
    RUN_TEST(test_long_module_chain);
    RUN_TEST(test_cell_width_limit);
    RUN_TEST(test_deep_file);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_cell_chain);
    RUN_TEST(test_unknown_type_is_an_error);
    RUN_TEST(test_auto_blackbox);
    RUN_TEST(test_auto_blackbox_conflicts);
    RUN_TEST(test_implicit_infers_directions);
    RUN_TEST(test_implicit_infers_known_and_inout);
    RUN_TEST(test_implicit_infers_text_parity);
    RUN_TEST(test_implicit_infers_order_independent);
    RUN_TEST(test_implicit_tie_two_types);
    RUN_TEST(test_implicit_tie_two_ports);
    RUN_TEST(test_implicit_infers_floating_slices_constants);
    RUN_TEST(test_implicit_infers_conflict_both_ports);
    RUN_TEST(test_implicit_conflict_three_orders);
    RUN_TEST(test_implicit_undriven_two_bits_orders);
    RUN_TEST(test_implicit_type_refused);
    RUN_TEST(test_long_names);
    RUN_TEST(test_scopeinfo_is_dropped);
    RUN_TEST(test_techlib_type);
    RUN_TEST(test_int_parameters);
    RUN_TEST(test_default_parameters);
    RUN_TEST(test_mem_parameters);
    RUN_TEST(test_bits_parameter);
    RUN_TEST(test_cover_parameter);
    RUN_TEST(test_parameter_errors);
    RUN_TEST(test_verify_failure);
    RUN_TEST(test_connection_errors);
    RUN_TEST(test_mismatch_creates_nothing);
    RUN_TEST(test_open_ports);
    RUN_TEST(test_constant_lut_output);
    RUN_TEST(test_port_directions);
    RUN_TEST(test_second_driver);
    RUN_TEST(test_memories);
    RUN_TEST(test_memrd_reads);
    RUN_TEST(test_module_cycle);
    RUN_TEST(test_instance_order_free);
    RUN_TEST(test_init_attribute);
    RUN_TEST(test_bits_limit);
    RUN_TEST(test_sop_all_functions);
    RUN_TEST(test_sop_dont_care);
    return UNITY_END();
}
