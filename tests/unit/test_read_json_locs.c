/*
 * test_read_json_locs.c — read_json restores `src` (spec 5.4 D-1, D-2, L-6, L-8, D-7; T-2 JSON
 * legs, T-4): five fields and the record shape, ports, multi-location texts, malformed texts,
 * --keep-src-attr, the write/read/write fixpoint, Yosys's text printed back, the cell count.
 */
#include "rj_harness.h"

#include "backends/common/hdl_loc.h"
#include "ir/prov.h"
#include "passes/manager.h"

void setUp(void) {
    rj_setup();
}
void tearDown(void) {
    rj_teardown();
}

enum { COL_B = 10 };

/* One module with a port a and y, a cell g, a one-bit netname n, a two-bit netname w. */
#define ALL_LEVELS                                                                                 \
    "{\"modules\": {\"top\": {\"attributes\": {\"src\": \"m.v:1.1-9.10\"},"                        \
    "\"ports\": {\"a\": {\"direction\": \"input\", \"bits\": [2]},"                                \
    "\"y\": {\"direction\": \"output\", \"bits\": [3]}},"                                          \
    "\"cells\": {\"g\": {\"type\": \"$_AND_\", \"attributes\": {\"src\": \"m.v:5.3-5.20\"},"       \
    "\"connections\": {\"A\": [2], \"B\": [\"1\"], \"Y\": [3]}}},"                                 \
    "\"netnames\": {\"a\": {\"bits\": [2], \"attributes\": {\"src\": \"m.v:2.9-2.10\"}},"          \
    "\"y\": {\"bits\": [3]},"                                                                      \
    "\"n\": {\"bits\": [4], \"attributes\": {\"src\": \"m.v:6.5-6.6\"}},"                          \
    "\"w\": {\"bits\": [5, 6], \"attributes\": {\"src\": \"m.v:7.1-7.2\"}}}}}}"

typedef struct want_loc {
    const char *file;
    uint32_t line, col, end_line, end_col;
} want_loc;

/* The HDL locations of prov, through the writers' helper. */
static odin3_hdl_locs_out locs_of(odin3_prov_id prov) {
    odin3_hdl_ctx *ctx = odin3_hdl_ctx_create(rj_design, "test");
    TEST_ASSERT_NOT_NULL(ctx);
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, prov, &out));
    odin3_hdl_ctx_destroy(ctx);
    return out;
}

static void assert_one_loc(odin3_prov_id prov, want_loc want) {
    odin3_hdl_locs_out out = locs_of(prov);
    TEST_ASSERT_EQUAL_UINT32(1, out.n);
    TEST_ASSERT_EQUAL_STRING(want.file,
                             odin3_strtab_get(odin3_design_strtab(rj_design), out.locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(want.line, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(want.col, out.locs[0].col);
    TEST_ASSERT_EQUAL_UINT32(want.end_line, out.locs[0].end_line);
    TEST_ASSERT_EQUAL_UINT32(want.end_col, out.locs[0].end_col);
}

/* D-1: DERIVED{parents [SOURCE, IMPORTED at the JSON value]}. */
static void assert_shape(odin3_prov_id prov) {
    const odin3_prov_record *rec = odin3_prov_get(rj_design, prov);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count);
    const odin3_prov_record *source = odin3_prov_get(rj_design, rec->parents.ids[0]);
    const odin3_prov_record *netlist = odin3_prov_get(rj_design, rec->parents.ids[1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, source->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED, netlist->kind);
    TEST_ASSERT_EQUAL_STRING(rj_path("in.json"), odin3_strtab_get(odin3_design_strtab(rj_design),
                                                                  netlist->locs[0].file));
}

static bool has_attr(odin3_module *mod, odin3_objref obj, const char *key) {
    return odin3_attr_get(mod, obj, rj_str(key)) != NULL;
}

/* D-2: module, cell, netname, one-bit netname and port restore all five fields. */
static void test_levels_restore(void) {
    rj_expect_ok(ALL_LEVELS);
    odin3_module *mod = rj_module("top");
    assert_one_loc(odin3_module_prov(mod), (want_loc){"m.v", 1, 1, 9, COL_B});
    assert_shape(odin3_module_prov(mod));
    odin3_node_id cell = rj_node(mod, "g");
    assert_one_loc(odin3_node_prov(mod, cell), (want_loc){"m.v", 5, 3, 5, 20});
    assert_shape(odin3_node_prov(mod, cell));
    odin3_wire_id wide = rj_wire(mod, "w");
    assert_one_loc(odin3_wire_prov(mod, wide), (want_loc){"m.v", 7, 1, 7, 2});
    odin3_wire_id bit = rj_wire(mod, "n");
    assert_one_loc(odin3_wire_prov(mod, bit), (want_loc){"m.v", 6, 5, 6, 6});
    assert_one_loc(odin3_net_prov(mod, odin3_wire_net(mod, bit, 0)), (want_loc){"m.v", 6, 5, 6, 6});
    /* a port takes its netname's location; a port without src in its netname has none */
    odin3_wire_id port = rj_wire(mod, "a");
    assert_one_loc(odin3_wire_prov(mod, port), (want_loc){"m.v", 2, 9, 2, COL_B});
    assert_shape(odin3_wire_prov(mod, port));
    TEST_ASSERT_EQUAL_UINT32(0, locs_of(odin3_wire_prov(mod, rj_wire(mod, "y"))).n);
    /* X-1: the nets of a wider wire keep the netlist record only */
    TEST_ASSERT_EQUAL_UINT32(0, locs_of(odin3_net_prov(mod, odin3_wire_net(mod, wide, 0))).n);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_PROV_IMPORTED,
        odin3_prov_get(rj_design, odin3_net_prov(mod, odin3_wire_net(mod, wide, 0)))->kind);
}

/* L-8: a restored src is not kept as an attribute. */
static void test_attribute_dropped(void) {
    rj_expect_ok(ALL_LEVELS);
    odin3_module *mod = rj_module("top");
    const odin3_objref objs[] = {{ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v},
                                 {ODIN3_OBJ_NODE, rj_node(mod, "g").v},
                                 {ODIN3_OBJ_WIRE, rj_wire(mod, "w").v},
                                 {ODIN3_OBJ_WIRE, rj_wire(mod, "a").v}};
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) {
        TEST_ASSERT_FALSE(has_attr(mod, objs[i], "src"));
    }
}

/* D-1: one operation makes every record of the read. */
static void test_one_operation(void) {
    rj_expect_ok(ALL_LEVELS);
    odin3_module *mod = rj_module("top");
    const odin3_prov_record *first = odin3_prov_get(rj_design, odin3_module_prov(mod));
    const odin3_prov_record *second =
        odin3_prov_get(rj_design, odin3_node_prov(mod, rj_node(mod, "g")));
    TEST_ASSERT_TRUE(first->op != 0);
    TEST_ASSERT_EQUAL_UINT32(first->op, second->op);
}

/* L-1: a multi-location text restores two locations. */
static void test_multi_location(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"cells\": {\"g\": {\"type\": \"$_NOT_\","
                 "\"attributes\": {\"src\": \"a.v:3.1-4.10|b.v:5.2-5.3\"},"
                 "\"connections\": {\"A\": [2], \"Y\": [3]}}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_hdl_locs_out out = locs_of(odin3_node_prov(mod, rj_node(mod, "g")));
    TEST_ASSERT_EQUAL_UINT32(2, out.n);
    TEST_ASSERT_EQUAL_UINT32(3, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(5, out.locs[1].line);
    TEST_ASSERT_EQUAL_STRING("b.v",
                             odin3_strtab_get(odin3_design_strtab(rj_design), out.locs[1].file));
}

static uint32_t count_in_log(const char *needle) {
    uint32_t count = 0;
    for (const char *at = strstr(rj_log, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

/* L-6, D-7: a malformed src stays an attribute; one WARNING per file with the count. */
static void test_malformed_stays_attribute(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"attributes\": {\"src\": \"no location\"},"
                 "\"ports\": {\"a\": {\"direction\": \"input\", \"bits\": [2]}},"
                 "\"cells\": {\"g\": {\"type\": \"$_NOT_\", \"attributes\": {\"src\": \"xy\"},"
                 "\"connections\": {\"A\": [2], \"Y\": [3]}}},"
                 "\"netnames\": {\"a\": {\"bits\": [2], \"attributes\": {\"src\": \"a.v:0\"}}}}}}");
    odin3_module *mod = rj_module("top");
    odin3_node_id cell = rj_node(mod, "g");
    const odin3_value *text =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, cell.v}, rj_str("src"));
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING("xy", odin3_strtab_get(odin3_design_strtab(rj_design), text->str));
    TEST_ASSERT_TRUE(
        has_attr(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, "src"));
    TEST_ASSERT_TRUE(has_attr(mod, (odin3_objref){ODIN3_OBJ_WIRE, rj_wire(mod, "a").v}, "src"));
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED,
                          odin3_prov_get(rj_design, odin3_node_prov(mod, cell))->kind);
    TEST_ASSERT_EQUAL_UINT32(1, count_in_log("are not location lists"));
    rj_log_has("in.json:1:");
    rj_log_has("3 `src` attribute(s)");
    rj_log_has("first: 'no location'");
}

/* A module whose attribute src is the JSON string text (already escaped for JSON). */
static void read_module_src(const char *text) {
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"modules\": {\"top\": {\"attributes\": {\"src\": \"%s\"}}}}", text);
    rj_expect_ok(json);
}

/* L-6: the text in the WARNING is untrusted: bytes outside 0x20..0x7e print as \xNN. */
static void test_bad_text_escaped(void) {
    read_module_src("caf\\u00e9\\u0001\\u007f!");
    rj_log_has("first: 'caf\\xc3\\xa9\\x01\\x7f!'");
}

/* L-6: the cut at 60 bytes does not fall inside a UTF-8 sequence. */
static void test_bad_text_cut_on_boundary(void) {
    char text[128];
    (void)snprintf(text, sizeof text, "%059d\\u00e9tail", 0);
    read_module_src(text);
    (void)snprintf(text, sizeof text, "first: '%059d'", 0);
    rj_log_has(text);
    TEST_ASSERT_NULL(strstr(rj_log, "\\xc3"));
}

/* L-8: --keep-src-attr keeps the string and makes no record. */
static void test_keep_src_attr(void) {
    odin3_json_opts opts = {.keep_src_attr = true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, rj_read_opts(ALL_LEVELS, &opts));
    odin3_module *mod = rj_module("top");
    odin3_node_id cell = rj_node(mod, "g");
    const odin3_value *text =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, cell.v}, rj_str("src"));
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING("m.v:5.3-5.20",
                             odin3_strtab_get(odin3_design_strtab(rj_design), text->str));
    for (uint32_t i = 1; i < odin3_prov_end(rj_design); i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_PROV_IMPORTED,
                              odin3_prov_get(rj_design, (odin3_prov_id){i})->kind);
    }
    TEST_ASSERT_EQUAL_UINT32(0, count_in_log("location lists"));
}

/* T-2, T-4: Yosys's text is printed back as it was; write, read, write is a fixpoint. */
static void test_write_read_write(void) {
    rj_expect_ok(ALL_LEVELS);
    const char *f1 = rj_path("f1.json");
    const char *f2 = rj_path("f2.json");
    const char *f3 = rj_path("f3.json");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f1));
    char *one = rj_slurp(f1);
    const char *const texts[] = {"\"src\": \"m.v:1.1-9.10\"", "\"src\": \"m.v:5.3-5.20\"",
                                 "\"src\": \"m.v:2.9-2.10\"", "\"src\": \"m.v:6.5-6.6\"",
                                 "\"src\": \"m.v:7.1-7.2\""};
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(one, texts[i]), texts[i]);
    }
    odin3_util_free(one);
    rj_fresh();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_read(rj_design, f1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f2));
    rj_fresh();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_read(rj_design, f2, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write(rj_design, f3));
    char *two = rj_slurp(f2);
    char *three = rj_slurp(f3);
    TEST_ASSERT_EQUAL_STRING(two, three);
    TEST_ASSERT_NOT_NULL(strstr(two, "\"src\": \"m.v:5.3-5.20\""));
    TEST_ASSERT_NULL(strstr(two, "in.json"));
    odin3_util_free(two);
    odin3_util_free(three);
}

/* T-4: cells with an HDL location after the read equal cells with `src` in the file. */
static void test_cell_count(void) {
    rj_expect_ok("{\"modules\": {\"top\": {\"cells\": {"
                 "\"c1\": {\"type\": \"$_NOT_\", \"attributes\": {\"src\": \"a.v:1.1-1.5\"},"
                 "\"connections\": {\"A\": [2], \"Y\": [3]}},"
                 "\"c2\": {\"type\": \"$_NOT_\", \"connections\": {\"A\": [3], \"Y\": [4]}},"
                 "\"c3\": {\"type\": \"$_NOT_\", \"attributes\": {\"src\": \"a.v:2.1-2.5\"},"
                 "\"connections\": {\"A\": [4], \"Y\": [5]}},"
                 "\"c4\": {\"type\": \"$_NOT_\", \"attributes\": {\"src\": \"a.v:3.1-3.5\"},"
                 "\"connections\": {\"A\": [5], \"Y\": [6]}}}}}}");
    odin3_module *mod = rj_module("top");
    uint32_t located = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(mod); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(mod, node) && locs_of(odin3_node_prov(mod, node)).n > 0) {
            located++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3, located);
}

/* The pass: read_json restores by default and `--keep-src-attr` keeps the attribute (L-8). */
static void test_pass_option(void) {
    const char *path = rj_put("pass.json", ALL_LEVELS);
    char args[(2 * RJ_PATH) + 32];
    const int keep = RJ_PATH - 1; /* rj_path never returns more; tells gcc the bound */
    rj_fresh();
    (void)snprintf(args, sizeof args, "\"%.*s\"", keep, path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr(args)));
    odin3_module *mod = rj_module("top");
    assert_one_loc(odin3_node_prov(mod, rj_node(mod, "g")), (want_loc){"m.v", 5, 3, 5, 20});
    rj_fresh();
    (void)snprintf(args, sizeof args, "--keep-src-attr \"%.*s\"", keep, path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "read_json", odin3_bytes_cstr(args)));
    mod = rj_module("top");
    TEST_ASSERT_TRUE(has_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, rj_node(mod, "g").v}, "src"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_levels_restore);
    RUN_TEST(test_attribute_dropped);
    RUN_TEST(test_one_operation);
    RUN_TEST(test_multi_location);
    RUN_TEST(test_malformed_stays_attribute);
    RUN_TEST(test_bad_text_escaped);
    RUN_TEST(test_bad_text_cut_on_boundary);
    RUN_TEST(test_keep_src_attr);
    RUN_TEST(test_write_read_write);
    RUN_TEST(test_cell_count);
    RUN_TEST(test_pass_option);
    return UNITY_END();
}
