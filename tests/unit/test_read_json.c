/*
 * test_read_json.c — unit tests for the JSON reader (spec 5.3 T-5, T-7): modules, ports, wires,
 * nets, attributes, top choice, black boxes, provenance, cells and every located error.
 */
#include "rj_harness.h"

#include "ir/prov.h"
#include "passes/manager.h"

enum { LINE_OF_KEY = 3, COL_OF_KEY = 5 };

void setUp(void) {
    rj_setup();
}
void tearDown(void) {
    rj_teardown();
}

/* A module with two ports and no cells, to put one more member into. */
#define TOP_OPEN                                                                                   \
    "{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", \"bits\": [2]}, "
#define TOP_END "}}}"

static const odin3_value *attr_of(odin3_module *mod, const char *key) {
    return odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v},
                          rj_str(key));
}

static uint32_t live_wires(odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < odin3_module_wire_end(mod); i++) {
        count += odin3_wire_live(mod, (odin3_wire_id){i}) ? 1U : 0U;
    }
    return count;
}

typedef struct wire_shape {
    const char *name;
    int32_t msb, lsb;
    bool is_signed;
    bool is_port;
} wire_shape;

static void assert_shape(odin3_module *mod, const wire_shape *want) {
    odin3_wire_id wire = rj_wire(mod, want->name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want->msb, odin3_wire_msb(mod, wire), want->name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(want->lsb, odin3_wire_lsb(mod, wire), want->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want->is_signed, odin3_wire_signed(mod, wire), want->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want->is_port, odin3_node_valid(odin3_wire_port_node(mod, wire)),
                                  want->name);
}

/* M-2: ports in file order, their ranges and signs; a netname of a port adds no wire. */
static void test_ports_and_wires(void) {
    rj_expect_ok(
        "{\"modules\": {\"top\": {\"ports\": {"
        "\"a\": {\"direction\": \"input\", \"bits\": [2]},"
        "\"y\": {\"direction\": \"output\", \"bits\": [3]},"
        "\"v\": {\"direction\": \"inout\", \"bits\": [4, 5, 6], \"offset\": 4},"
        "\"u\": {\"direction\": \"input\", \"bits\": [7, 8], \"offset\": 1, \"upto\": 1, "
        "\"signed\": 1}},"
        "\"netnames\": {\"a\": {\"bits\": [2]}, \"y\": {\"bits\": [3]}, \"n\": {\"bits\": [9, 10]}}"
        "}}}");
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(4, odin3_module_port_count(mod));
    TEST_ASSERT_EQUAL_UINT32(5, live_wires(mod));
    const wire_shape shapes[] = {
        {"v", 6, 4, false, true}, {"u", 1, 2, true, true}, {"n", 1, 0, false, false}};
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        assert_shape(mod, &shapes[i]);
    }
}

/* M-3: nets are created by ascending bit number (after the nets of the ports), constants last
 * in order of first use. */
static void test_net_order(void) {
    rj_expect_ok(
        "{\"modules\": {\"top\": {\"ports\": {\"p\": {\"direction\": \"input\", \"bits\": [20]}},"
        "\"netnames\": {\"c\": {\"bits\": [7]}, \"a\": {\"bits\": [3]}, \"b\": {\"bits\": [5]},"
        "\"k\": {\"bits\": [\"1\", \"0\"]}}}}}");
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_wire_net(mod, rj_wire(mod, "p"), 0).v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_wire_net(mod, rj_wire(mod, "a"), 0).v);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_wire_net(mod, rj_wire(mod, "b"), 0).v);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_wire_net(mod, rj_wire(mod, "c"), 0).v);
    odin3_wire_id k = rj_wire(mod, "k");
    TEST_ASSERT_EQUAL_UINT32(5, odin3_wire_net(mod, k, 0).v);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_wire_net(mod, k, 1).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(mod, odin3_wire_net(mod, k, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(mod, odin3_wire_net(mod, k, 1)));
}

/* M-3: a label is not an index; a huge one costs the same as a small one. */
static void test_huge_label(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"netnames\": {\"a\": {\"bits\": [4294967295]}}}}}");
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_wire_net(mod, rj_wire(mod, "a"), 0).v);
}

/* M-3: "0", "1", "x", "z" are constant nets, one per value. */
static void test_constants_bind(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"ports\": {\"y\": {\"direction\": \"output\", "
                 "\"bits\": [2, 3, 4, 5]}},"
                 "\"cells\": {\"g\": {\"type\": \"$_AND_\", \"connections\": {\"A\": [\"1\"], "
                 "\"B\": [\"x\"], \"Y\": [2]}},"
                 "\"h\": {\"type\": \"$_AND_\", \"connections\": {\"A\": [\"1\"], \"B\": [\"z\"], "
                 "\"Y\": [3]}}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_node_id first = rj_node(mod, "g");
    odin3_node_id second = rj_node(mod, "h");
    odin3_net_id a_first = odin3_pin_net(mod, odin3_node_port(mod, first, 0).first);
    odin3_net_id a_second = odin3_pin_net(mod, odin3_node_port(mod, second, 0).first);
    TEST_ASSERT_EQUAL_UINT32(a_first.v, a_second.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(mod, a_first));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_CONST_X,
        odin3_net_const_value(mod, odin3_pin_net(mod, odin3_node_port(mod, first, 1).first)));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_CONST_Z,
        odin3_net_const_value(mod, odin3_pin_net(mod, odin3_node_port(mod, second, 1).first)));
}

/* M-3: a constant on an output port bit takes that port's net and is driven by the constant. */
static void test_constant_output_port(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"ports\": {\"y\": {\"direction\": \"output\", "
                 "\"bits\": [\"0\", \"1\", \"0\"]}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_wire_id outw = rj_wire(mod, "y");
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(mod, outw, 0).v, odin3_wire_net(mod, outw, 2).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(mod, odin3_wire_net(mod, outw, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(mod, odin3_wire_net(mod, outw, 1)));
}

/* M-4: two netnames over one bit are one net; the first is its primary wire. */
static void test_alias_netnames(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"netnames\": {\"n1\": {\"bits\": [5]}, "
                 "\"n2\": {\"bits\": [5]}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_net_id net = odin3_wire_net(mod, rj_wire(mod, "n1"), 0);
    TEST_ASSERT_EQUAL_UINT32(net.v, odin3_wire_net(mod, rj_wire(mod, "n2"), 0).v);
    TEST_ASSERT_EQUAL_UINT32(rj_wire(mod, "n1").v, odin3_net_primary(mod, net).wire.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_alias_count(mod, net));
}

/* M-2: a netname with a port's name adds its attributes to the port wire and no second wire. */
static void test_netname_of_port(void) {
    rj_expect_ok(TOP_OPEN "\"y\": {\"direction\": \"output\", \"bits\": [3]}}, "
                          "\"netnames\": {\"a\": {\"bits\": [2], \"attributes\": {\"keep\": 1}}, "
                          "\"y\": {\"bits\": [3]}}" TOP_END);
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(2, live_wires(mod));
    odin3_wire_id wire = rj_wire(mod, "a");
    const odin3_value *val =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, rj_str("keep"));
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, val->kind);
    TEST_ASSERT_EQUAL_INT64(1, val->i);
}

static void test_netname_port_mismatch(void) {
    rj_expect_error(TOP_OPEN "\"y\": {\"direction\": \"output\", \"bits\": [3]}}, "
                             "\"netnames\": {\"a\": {\"bits\": [3]}}" TOP_END,
                    "bit 0 of netname 'a' differs from the port 'a'");
    rj_expect_error(TOP_OPEN "\"y\": {\"direction\": \"output\", \"bits\": [3]}}, "
                             "\"netnames\": {\"a\": {\"bits\": [2, 3]}}" TOP_END,
                    "netname 'a' has 2 bits but the port 'a' has 1");
}

/* M-4: a one-bit netname is a one-bit wire. */
static void test_one_bit_netname(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"netnames\": {\"$n7\": {\"hide_name\": 1, "
                 "\"bits\": [6]}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_wire_id wire = rj_wire(mod, "$n7");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_wire_width(mod, wire));
    TEST_ASSERT_EQUAL_INT32(0, odin3_wire_msb(mod, wire));
}

typedef struct typed_case {
    const char *key;
    const char *json;
    odin3_value_kind kind;
    int64_t num;
    const char *text; /* digits MSB first for BITS, the string for STRING */
} typed_case;

static void assert_typed(odin3_module *mod, const typed_case *item) {
    const odin3_value *val = attr_of(mod, item->key);
    TEST_ASSERT_NOT_NULL_MESSAGE(val, item->key);
    TEST_ASSERT_EQUAL_INT_MESSAGE(item->kind, val->kind, item->key);
    if (item->kind == ODIN3_VAL_INT) {
        TEST_ASSERT_EQUAL_INT64_MESSAGE(item->num, val->i, item->key);
    } else if (item->kind == ODIN3_VAL_STRING) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            item->text, odin3_strtab_get(odin3_design_strtab(rj_design), val->str), item->key);
    } else {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)strlen(item->text), val->len, item->key);
        for (uint32_t i = 0; i < val->len; i++) {
            char want = item->text[val->len - 1 - i];
            char got = "01xz"[val->bits[i]];
            TEST_ASSERT_EQUAL_CHAR_MESSAGE(want, got, item->key);
        }
    }
}

/* J-10: attributes are typed as Yosys types them (M-11: integers are INT). */
static void test_attribute_typing(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"attributes\": {\"s1\": \"0101\", \"s2\": \"abc\", "
                 "\"s3\": \"01 \", \"s4\": \"x \", \"s5\": 5, \"s6\": -1, \"s7\": \"\", "
                 "\"s8\": \"0 1 \", \"s9\": \"1x0z\"}}}}");
    odin3_module *mod = rj_module("top");
    const typed_case cases[] = {
        {"s1", NULL, ODIN3_VAL_BITS, 0, "0101"}, {"s2", NULL, ODIN3_VAL_STRING, 0, "abc"},
        {"s3", NULL, ODIN3_VAL_STRING, 0, "01"}, {"s4", NULL, ODIN3_VAL_STRING, 0, "x"},
        {"s5", NULL, ODIN3_VAL_INT, 5, NULL},    {"s6", NULL, ODIN3_VAL_INT, -1, NULL},
        {"s7", NULL, ODIN3_VAL_BITS, 0, ""},     {"s8", NULL, ODIN3_VAL_STRING, 0, "0 1"},
        {"s9", NULL, ODIN3_VAL_BITS, 0, "1x0z"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        assert_typed(mod, &cases[i]);
    }
}

static void test_attribute_errors(void) {
    rj_expect_error("{\"modules\": {\"top\": {\"attributes\": {\"k\": [1]}}}}",
                    "modules.top.attributes.k: an attribute value is a string or an integer");
    rj_expect_error("{\"modules\": {\"top\": {\"attributes\": 3}}}", "attributes is not an object");
}

static const char *const TWO_TOPS =
    "{\"modules\": {\"a\": {\"attributes\": {\"top\": \"00000000000000000000000000000000\"}}, "
    "\"b\": {\"attributes\": {\"top\": \"00000000000000000000000000000001\"}}}}";

/* M-1: the top attribute, --top, the unique uninstantiated module, a tie. */
static void test_top_choice(void) {
    rj_expect_ok(TWO_TOPS);
    TEST_ASSERT_EQUAL_STRING("b", rj_top_name());
    const odin3_json_opts named = {.top = "a"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(TWO_TOPS, &named));
    TEST_ASSERT_EQUAL_STRING("a", rj_top_name());
    rj_expect_ok("{\"modules\": {\"sub\": {}, \"top\": {\"cells\": {\"u\": {\"type\": \"sub\", "
                 "\"connections\": {}}}}}}");
    TEST_ASSERT_EQUAL_STRING("top", rj_top_name());
    rj_expect_error("{\"modules\": {\"a\": {}, \"b\": {}, \"c\": {}}}",
                    "cannot choose a top module: no unique uninstantiated module: a, b, c "
                    "(use --top)");
    const odin3_json_opts missing = {.top = "zz"};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rj_read_opts(TWO_TOPS, &missing));
    rj_log_has("--top: no module named 'zz'");
    rj_expect_error("{\"modules\": {\"a\": {\"attributes\": {\"top\": 1}}, "
                    "\"b\": {\"attributes\": {\"top\": 1}}}}",
                    "several modules have the top attribute: a, b");
}

/* M-1: a black-box module is a black-box cell type, not an IR module and never the top. */
static void test_blackbox_module(void) {
    rj_expect_ok(
        "{\"modules\": {\"bb\": {\"attributes\": {\"blackbox\": "
        "\"00000000000000000000000000000001\"},"
        "\"ports\": {\"i\": {\"direction\": \"input\", \"bits\": [2, 3]}, "
        "\"o\": {\"direction\": \"output\", \"bits\": [4]}}},"
        "\"top\": {\"ports\": {\"x\": {\"direction\": \"input\", \"bits\": [2, 3]}, "
        "\"y\": {\"direction\": \"output\", \"bits\": [4]}},"
        "\"cells\": {\"u\": {\"type\": \"bb\", \"connections\": {\"i\": [2, 3], \"o\": [4]}}}}}}");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(rj_design));
    TEST_ASSERT_EQUAL_STRING("top", rj_top_name());
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(rj_design, rj_str("bb"), &type));
    const odin3_celltype_def *def = odin3_celltype_get(rj_design, type);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    TEST_ASSERT_EQUAL_UINT32(2, def->n_ports);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(rj_design));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(rj_design, type));
}

/* M-10: IMPORTED records at the key's line and column. */
static void test_provenance(void) {
    rj_expect_ok("{\n\"modules\": {\n    \"top\": {\"ports\": {}, \"netnames\": {\n"
                 "    \"w\": {\"bits\": [2]}}}}}");
    odin3_module *mod = rj_module("top");
    const odin3_prov_record *mrec = odin3_prov_get(rj_design, odin3_module_prov(mod));
    TEST_ASSERT_NOT_NULL(mrec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, mrec->kind);
    TEST_ASSERT_EQUAL_UINT32(LINE_OF_KEY, mrec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(COL_OF_KEY, mrec->locs[0].col);
    const odin3_prov_record *wrec =
        odin3_prov_get(rj_design, odin3_wire_prov(mod, rj_wire(mod, "w")));
    TEST_ASSERT_EQUAL_UINT32(4, wrec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(5, wrec->locs[0].col);
    TEST_ASSERT_EQUAL_STRING(rj_path("in.json"),
                             odin3_strtab_get(odin3_design_strtab(rj_design), wrec->locs[0].file));
}

/* J-8, J-9, T-7: unknown and odin3_ members are skipped and counted; version 2 is an error. */
static void test_unknown_and_ext_members(void) {
    rj_expect_ok("{\"odin3_version\": 1, \"extra\": {\"deep\": [1, 2]}, \"modules\": {\"top\": "
                 "{\"odin3_graph\": {}, \"mystery\": 1, \"ports\": {}}}}");
    rj_log_has("skipped 2 members the schema does not list (first 'extra'), 2 odin3_ members");
    rj_expect_error("{\"odin3_version\": 2, \"modules\": {\"top\": {}}}",
                    "odin3_version 2: written by a newer Odin III");
}

static void test_creator_in_run_info(void) {
    rj_expect_ok("{\"creator\": \"Yosys 0.55 (git sha1 x)\", \"modules\": {\"top\": {}}}");
    uint32_t info =
        odin3_passrun_info(rj_design, (odin3_passrun_id){odin3_passrun_end(rj_design) - 1});
    TEST_ASSERT_NOT_EQUAL_UINT32(0, info);
    const char *text = odin3_strtab_get(odin3_design_strtab(rj_design), info);
    TEST_ASSERT_EQUAL_STRING("creator=\"Yosys 0.55 (git sha1 x)\"", text);
}

/* E-4, E-10, E-3: located structural errors. */
static void test_structural_errors(void) {
    rj_expect_error("[1]", "the root is not an object");
    rj_expect_error("{\"modules\": {}}", "no modules");
    rj_expect_error("{}", "no modules");
    rj_expect_error("{\"modules\": 3}", "modules is not an object");
    rj_expect_error("{\"modules\": {\"top\": 3}}", "modules.top: the module is not an object");
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": 3}}}", "ports is not an object");
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": {\"a\": {\"bits\": [2]}}}}}",
                    "modules.top.ports.a: the port has no direction");
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"sideways\", "
                    "\"bits\": [2]}}}}}",
                    "direction 'sideways' is not input, output or inout");
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", "
                    "\"bits\": []}}}}}",
                    "0 bits (1 to 1048576 are allowed)");
    rj_expect_error("{\"modules\": {\"top\": {\"netnames\": {\"a\": {\"bits\": [2, \"q\"]}}}}}",
                    "modules.top.netnames.a.bits[1]: a bit is a non-negative integer or one of");
    rj_expect_error("{\"modules\": {\"top\": {\"netnames\": {\"a\": {\"bits\": [-3]}}}}}",
                    "modules.top.netnames.a.bits[0]");
    rj_expect_error("{\"modules\": {\"top\": {\"netnames\": {\"a\": 3}}}}",
                    "the netname is not an object");
}

static void test_port_bit_errors(void) {
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", "
                    "\"bits\": [\"0\"]}}}}}",
                    "an input or inout port bit cannot be a constant");
    rj_expect_error("{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", "
                    "\"bits\": [2]}, \"b\": {\"direction\": \"input\", \"bits\": [2]}}}}}",
                    "a net is driven by more than one input port bit");
    rj_expect_error(
        "{\"modules\": {\"top\": {\"ports\": {\"a\": {\"direction\": \"input\", "
        "\"bits\": [2], \"offset\": 2147483647, \"upto\": 0}, "
        "\"b\": {\"direction\": \"input\", \"bits\": [3, 4], \"offset\": 2147483647}}}}}",
        "offset 2147483647 is out of range");
}

/* An input port bit may be shared with an output port: the output port reads the input net. */
static void test_shared_port_bit(void) {
    rj_expect_ok(TOP_OPEN "\"y\": {\"direction\": \"output\", \"bits\": [2]}}, "
                          "\"netnames\": {\"a\": {\"bits\": [2]}, \"y\": {\"bits\": [2]}}" TOP_END);
    odin3_module *mod = rj_module("top");
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(mod, rj_wire(mod, "a"), 0).v,
                             odin3_wire_net(mod, rj_wire(mod, "y"), 0).v);
}

/* The parser's located error: path:line:col: message. */
static void test_syntax_error_is_located(void) {
    rj_expect_error("{\n  \"modules\": {\"top\": {,}}\n}", "in.json:2:");
}

static void test_missing_file(void) {
    rj_fresh();
    rj_log_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_json_read(rj_design, rj_path("absent.json"), NULL));
    rj_log_has("cannot read");
}

static void test_design_must_be_empty(void) {
    rj_expect_ok("{\"modules\": {\"top\": {}}}");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_json_read(rj_design, rj_path("in.json"), NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_read(NULL, rj_path("in.json"), NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_read(rj_design, NULL, NULL));
}

/* J-1: the pass through the pass manager: options, the global --top, the creator run record. */
static void test_pass(void) {
    const char *path =
        rj_put("pass.json", "{\"creator\": \"c\", \"modules\": {\"a\": {}, \"b\": {}}}");
    char args[RJ_PATH + 32];
    rj_fresh();
    (void)snprintf(args, sizeof args, "--top b \"%s\"", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr(args)));
    TEST_ASSERT_EQUAL_STRING("b", rj_top_name());
    rj_fresh();
    odin3_pass_options options = odin3_pass_get_options();
    options.top = "a";
    odin3_pass_set_options(options);
    (void)snprintf(args, sizeof args, "\"%s\"", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr(args)));
    TEST_ASSERT_EQUAL_STRING("a", rj_top_name());
    options.top = NULL;
    odin3_pass_set_options(options);
    rj_fresh();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr("--bogus x")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr("--top")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr("")));
    (void)snprintf(args, sizeof args, "\"%s\" \"%s\"", path, path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr(args)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ports_and_wires);
    RUN_TEST(test_net_order);
    RUN_TEST(test_huge_label);
    RUN_TEST(test_constants_bind);
    RUN_TEST(test_constant_output_port);
    RUN_TEST(test_alias_netnames);
    RUN_TEST(test_netname_of_port);
    RUN_TEST(test_netname_port_mismatch);
    RUN_TEST(test_one_bit_netname);
    RUN_TEST(test_attribute_typing);
    RUN_TEST(test_attribute_errors);
    RUN_TEST(test_top_choice);
    RUN_TEST(test_blackbox_module);
    RUN_TEST(test_provenance);
    RUN_TEST(test_unknown_and_ext_members);
    RUN_TEST(test_creator_in_run_info);
    RUN_TEST(test_structural_errors);
    RUN_TEST(test_port_bit_errors);
    RUN_TEST(test_shared_port_bit);
    RUN_TEST(test_syntax_error_is_located);
    RUN_TEST(test_missing_file);
    RUN_TEST(test_design_must_be_empty);
    RUN_TEST(test_pass);
    return UNITY_END();
}
