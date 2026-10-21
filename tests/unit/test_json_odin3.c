/*
 * test_json_odin3.c — write_json --odin3 and its read_json handlers (spec 6.3 E-1 to E-3, E-8;
 * V-7 d): default bytes unchanged, key order and values, the level written once, A/B/C round trip,
 * the version policy, and the levels of a levelized BLIF surviving the trip.
 */
#include "rj_harness.h"

#include "ir/celltype.h"
#include "ir/prov.h"
#include "passes/manager.h"

enum { PATH_BUF = 512, LEVEL_MAX = 64, NAME_BUF = 32 };

static odin3_pass_ctx run_ctx;

static uint32_t intern(const char *name) {
    return rj_str(name);
}

static odin3_prov_id prov_at(uint32_t line) {
    odin3_srcloc loc = {intern("t.v"), line, 1, line, 1, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run_ctx, &origin, &id));
    return id;
}

void setUp(void) {
    rj_setup();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(rj_design, intern("builder"), &run_ctx));
}
void tearDown(void) {
    rj_teardown();
}

static void set_int_attr(odin3_module *mod, odin3_objref obj, const char *key, int64_t num) {
    odin3_value val = odin3_value_int(num);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, obj, intern(key), &val));
}

/* top: inputs a, b; output y; an AND `g1` (a, b -> n1) and a NOT `g2` (n1 -> y). g1 carries
 * odin3_level 1 and odin3_fsm_state 2, g2 odin3_level 2, the net n1 odin3_level 1. */
static void build_design(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(rj_design, intern("top"), prov_at(1), &mid));
    odin3_module *mod = odin3_module_get(rj_design, mid);
    odin3_net_id nets[4];
    const struct {
        const char *name;
        odin3_dir dir;
    } k_ports[3] = {{"a", ODIN3_DIR_IN}, {"b", ODIN3_DIR_IN}, {"y", ODIN3_DIR_OUT}};
    for (uint32_t i = 0; i < 3; i++) {
        odin3_port_spec spec = {
            intern(k_ports[i].name), k_ports[i].dir, 1, true, prov_at(2), false, 0, 0, false};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
        nets[i] = odin3_wire_net(mod, odin3_module_port_wire(mod, i), 0);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, intern("n1"), prov_at(3), &nets[3]));
    odin3_celltype_id and_type = {0};
    odin3_celltype_id not_type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(rj_design, intern("$_AND_"), &and_type));
    TEST_ASSERT_TRUE(odin3_celltype_find(rj_design, intern("$_NOT_"), &not_type));
    odin3_netvec and_ports[3] = {{&nets[0], 1}, {&nets[1], 1}, {&nets[3], 1}};
    odin3_netvec not_ports[2] = {{&nets[3], 1}, {&nets[2], 1}};
    odin3_node_spec and_spec = {and_type, intern("g1"), prov_at(4), NULL, 0};
    odin3_node_spec not_spec = {not_type, intern("g2"), prov_at(5), NULL, 0};
    odin3_node_id g1 = {0};
    odin3_node_id g2 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &and_spec, and_ports, &g1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &not_spec, not_ports, &g2));
    set_int_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, g1.v}, "odin3_level", 1);
    set_int_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, g1.v}, "odin3_fsm_state", 2);
    set_int_attr(mod, (odin3_objref){ODIN3_OBJ_NODE, g2.v}, "odin3_level", 2);
    set_int_attr(mod, (odin3_objref){ODIN3_OBJ_NET, nets[3].v}, "odin3_level", 1);
}

static char *write_text(const char *name, const odin3_json_wopts *opts) {
    const char *path = rj_path(name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_write_opts(rj_design, path, opts));
    return rj_slurp(path);
}

static const odin3_json_wopts k_odin3 = {true};
static const odin3_json_wopts k_plain = {false};

static void assert_has(const char *text, const char *needle) {
    if (strstr(text, needle) == NULL) {
        char msg[1024];
        (void)snprintf(msg, sizeof msg, "missing: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

static void assert_lacks(const char *text, const char *needle) {
    if (strstr(text, needle) != NULL) {
        char msg[1024];
        (void)snprintf(msg, sizeof msg, "unexpected: %s", needle);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* --- E-1: without the option nothing changes ------------------------------------------------- */

static void test_default_is_byte_identical(void) {
    build_design();
    char *plain = write_text("p.json", NULL);
    char *off = write_text("o.json", &k_plain);
    TEST_ASSERT_EQUAL_STRING(plain, off);
    assert_lacks(plain, "odin3_version");
    assert_lacks(plain, "odin3_prov");
    /* a levelized design keeps the level under attributes, as a bit string, as today (E-1) */
    assert_has(
        plain,
        "\"attributes\": {\n            \"odin3_level\": \"00000000000000000000000000000001\","
        "\n            \"odin3_fsm_state\": \"00000000000000000000000000000010\",");
    assert_has(plain, "\"bits\": [ 5 ],\n          \"attributes\": {\n            \"odin3_level\"");
    odin3_util_free(plain);
    odin3_util_free(off);
}

/* --- E-2: keys, order, the level once -------------------------------------------------------- */

static void test_keys_and_order(void) {
    build_design();
    char *text = write_text("a.json", &k_odin3);
    assert_has(text, "\"creator\": \"Odin III\",\n  \"odin3_version\": 1,\n  \"modules\": {");
    /* the cell g1: level then prov are its last members, after connections */
    assert_has(text, "\"Y\": [ 5 ]\n"
                     "          },\n"
                     "          \"odin3_level\": 1,\n"
                     "          \"odin3_prov\": {\n"
                     "            \"kind\": \"SOURCE\",\n"
                     "            \"hier\": \"\",\n"
                     "            \"hdl\": [ \"t.v:4.1-4.1\" ]\n"
                     "          }\n"
                     "        },");
    /* the level is not repeated under attributes; other odin3_ attributes stay */
    assert_lacks(text, "\"odin3_level\": \"0000");
    assert_has(text, "\"odin3_fsm_state\": \"00000000000000000000000000000010\"");
    /* the net n1 and the module carry prov; the module's is its last member */
    assert_has(text,
               "\"n1\": {\n          \"hide_name\": 0,\n          \"bits\": [ 5 ],\n"
               "          \"attributes\": {\n            \"src\": \"t.v:3.1-3.1\"\n          },\n"
               "          \"odin3_level\": 1,\n          \"odin3_prov\": {");
    assert_has(text, "\"hdl\": [ \"t.v:1.1-1.1\" ]\n      }\n    }\n  }\n}\n");
    odin3_util_free(text);
}

/* --- E-3: the round trip ---------------------------------------------------------------------- */

/* The text without its odin3_prov objects (each is the last member of its object). */
static char *drop_prov(const char *text) {
    char *out = odin3_util_malloc(strlen(text) + 1);
    TEST_ASSERT_NOT_NULL(out);
    size_t len = 0;
    const char *at = text;
    for (const char *hit = strstr(at, ",\n"); hit != NULL; hit = strstr(at, ",\n")) {
        const char *next = hit + 2;
        next += strspn(next, " ");
        if (strncmp(next, "\"odin3_prov\": {", strlen("\"odin3_prov\": {")) == 0) {
            memcpy(out + len, at, (size_t)(hit - at));
            len += (size_t)(hit - at);
            at = strchr(next, '}') + 1;
        } else {
            memcpy(out + len, at, (size_t)(next - at));
            len += (size_t)(next - at);
            at = next;
        }
    }
    (void)strcpy(out + len, at);
    return out;
}

static void read_back(const char *name) {
    rj_fresh();
    rj_log_clear();
    odin3_status st = odin3_json_read(rj_design, rj_path(name), NULL);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(rj_log);
    }
}

static void test_round_trip_abc(void) {
    build_design();
    char *first = write_text("a.json", &k_odin3);
    read_back("a.json");
    char *second = write_text("b.json", &k_odin3);
    read_back("b.json");
    char *third = write_text("c.json", &k_odin3);
    char *a_np = drop_prov(first);
    char *b_np = drop_prov(second);
    TEST_ASSERT_EQUAL_STRING(a_np, b_np);
    TEST_ASSERT_EQUAL_STRING(second, third);
    assert_has(second, "\"odin3_level\": 2,");
    odin3_util_free(first);
    odin3_util_free(second);
    odin3_util_free(third);
    odin3_util_free(a_np);
    odin3_util_free(b_np);
}

static void test_reader_sets_level_attribute_and_counts_prov(void) {
    build_design();
    odin3_util_free(write_text("a.json", &k_odin3));
    read_back("a.json");
    assert_has(rj_log, "validated 7 odin3_prov objects");
    /* only odin3_version is counted as skipped: odin3_level and odin3_prov are consumed */
    assert_has(rj_log, "skipped 0 members the schema does not list, 1 odin3_ members");
    odin3_module *mod = odin3_module_get(rj_design, (odin3_module_id){1});
    odin3_node_id g2 = odin3_module_find_node(mod, intern("g2"));
    TEST_ASSERT_TRUE(odin3_node_valid(g2));
    const odin3_value *level =
        odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, g2.v}, intern("odin3_level"));
    TEST_ASSERT_NOT_NULL(level);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, level->kind);
    TEST_ASSERT_EQUAL_INT64(2, level->i);
}

/* --- E-3 / E-8: the version and malformed keys ------------------------------------------------ */

static const char k_min_head[] = "{\"creator\": \"t\", \"odin3_version\": ";
static const char k_min_tail[] =
    ", \"modules\": {\"m\": {\"attributes\": {}, \"ports\": {}, \"cells\": {}, \"netnames\": "
    "{}}}}\n";

static odin3_status read_version(const char *version) {
    char text[512];
    (void)snprintf(text, sizeof text, "%s%s%s", k_min_head, version, k_min_tail);
    return rj_read(text);
}

static void test_version_policy(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_version("1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_version("2"));
    TEST_ASSERT_NOT_NULL(strstr(rj_log, "odin3_version 2"));
}

/* J-9: odin3_version is exactly the integer 1 */
static void test_version_must_be_integer_one(void) {
    const char *const bad[] = {"true", "false", "null", "\"1\"", "[1]", "{}", "0", "-1", "3"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (read_version(bad[i]) != ODIN3_ERR_PARSE) {
            char msg[64];
            (void)snprintf(msg, sizeof msg, "accepted odin3_version %s", bad[i]);
            TEST_FAIL_MESSAGE(msg);
        }
        rj_log_has("odin3_version");
    }
}

static void test_unknown_odin3_member_is_counted(void) {
    rj_expect_ok("{\"modules\": {\"m\": {\"attributes\": {}, \"ports\": {}, \"cells\": {}, "
                 "\"netnames\": {}, \"odin3_future\": 1}}}\n");
    TEST_ASSERT_NOT_NULL(strstr(rj_log, "1 odin3_ members"));
}

/* Finding 4: a malformed odin3_prov or odin3_level is a located error. */
static void expect_module_member_error(const char *member) {
    char json[1024];
    (void)snprintf(json, sizeof json,
                   "{\"modules\": {\"m\": {\"attributes\": {}, \"ports\": {}, \"cells\": {}, "
                   "\"netnames\": {\"n\": {\"bits\": [ 2 ], %s}}}}}\n",
                   member);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rj_read(json));
    rj_log_has(strstr(member, "odin3_level") != NULL ? "odin3_level" : "odin3_prov");
    rj_log_has("in.json:1:");
}

static void test_bad_prov_is_an_error(void) {
    expect_module_member_error(
        "\"odin3_prov\": {\"kind\": \"BOGUS\", \"hier\": \"\", \"hdl\": []}");
    expect_module_member_error("\"odin3_prov\": 3");
    expect_module_member_error("\"odin3_prov\": {\"kind\": \"SOURCE\", \"hier\": 1, \"hdl\": []}");
    expect_module_member_error("\"odin3_prov\": {\"kind\": \"SOURCE\", \"hier\": \"\"}");
    expect_module_member_error(
        "\"odin3_prov\": {\"kind\": \"SOURCE\", \"hier\": \"\", \"hdl\": [1]}");
    expect_module_member_error("\"odin3_prov\": {\"kind\": \"SOURCE\", \"hier\": \"\", \"hdl\": "
                               "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}");
}

static void test_good_prov_with_eight_hdl_is_read(void) {
    rj_expect_ok("{\"modules\": {\"m\": {\"attributes\": {}, \"ports\": {}, \"cells\": {}, "
                 "\"netnames\": {}, \"odin3_prov\": {\"kind\": \"IMPORTED\", \"hier\": \"x\", "
                 "\"hdl\": [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\"]}}}}\n");
    TEST_ASSERT_NOT_NULL(strstr(rj_log, "validated 1 odin3_prov"));
}

static void test_bad_level_is_an_error(void) {
    expect_module_member_error("\"odin3_level\": \"2\"");
    expect_module_member_error("\"odin3_level\": true");
    expect_module_member_error("\"odin3_level\": -1");
    expect_module_member_error("\"odin3_level\": 2147483648");
}

static void test_level_bounds_are_accepted(void) {
    rj_expect_ok("{\"modules\": {\"m\": {\"attributes\": {}, \"ports\": {}, \"cells\": {}, "
                 "\"netnames\": {\"n\": {\"bits\": [ 2 ], \"odin3_level\": 2147483647}, "
                 "\"z\": {\"bits\": [ 3 ], \"odin3_level\": 0}}}}}\n");
}

/* Finding 3 and E-2 placement: the level is a key of cells and netnames only; a module keeps its
 * level attribute under attributes. Level then prov are the last two members, never in attributes.
 */
static void test_level_key_placement(void) {
    build_design();
    odin3_module *mod = odin3_module_get(rj_design, (odin3_module_id){1});
    set_int_attr(mod, (odin3_objref){ODIN3_OBJ_MODULE, 1}, "odin3_level", 5);
    char *text = write_text("m.json", &k_odin3);
    assert_has(text, "\"odin3_level\": \"00000000000000000000000000000101\"");
    int keys = 0;
    for (const char *at = strstr(text, "\"odin3_level\": "); at != NULL;
         at = strstr(at + 1, "\"odin3_level\": ")) {
        if (at[strlen("\"odin3_level\": ")] == '"') {
            continue; /* the module's bit string under attributes */
        }
        keys++;
        const char *after = strstr(at, ",\n");
        TEST_ASSERT_NOT_NULL(after);
        after += 2;
        after += strspn(after, " ");
        TEST_ASSERT_EQUAL_INT(0, strncmp(after, "\"odin3_prov\": {", strlen("\"odin3_prov\": {")));
    }
    TEST_ASSERT_EQUAL_INT(3, keys); /* g1, g2, n1 */
    odin3_util_free(text);
}

/* --- V-7 (d): levels of a levelized BLIF survive write_json --odin3, read_json --------------- */

static int levels_of(odin3_module *mod, int64_t *out) {
    int count = 0;
    uint32_t key = intern("odin3_level");
    const uint32_t end = odin3_module_node_end(mod);
    for (uint32_t i = 1; i < end && count < LEVEL_MAX; i++) {
        const odin3_value *val = odin3_node_live(mod, (odin3_node_id){i})
                                     ? odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, i}, key)
                                     : NULL;
        if (val != NULL) {
            out[count++] = val->i;
        }
    }
    return count;
}

static void test_levels_survive_levelize_write_read(void) {
    const char *blif = rj_put("t.blif", ".model m\n.inputs a b\n.outputs n3\n.names a b n1\n11 1\n"
                                        ".names n1 b n2\n10 1\n.names n2 n3\n0 1\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "read_blif", odin3_bytes_cstr(blif)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(rj_design, "levelize", odin3_bytes_cstr("")));
    char args[PATH_BUF];
    (void)snprintf(args, sizeof args, "%.400s --odin3", rj_path("l.json"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rj_design, "write_json", odin3_bytes_cstr(args)));
    int64_t before[LEVEL_MAX];
    int64_t after[LEVEL_MAX];
    int n_before = levels_of(odin3_module_get(rj_design, (odin3_module_id){1}), before);
    TEST_ASSERT_EQUAL_INT(3, n_before);
    read_back("l.json");
    int n_after = levels_of(odin3_module_get(rj_design, (odin3_module_id){1}), after);
    TEST_ASSERT_EQUAL_INT(n_before, n_after);
    TEST_ASSERT_EQUAL_INT64_ARRAY(before, after, n_before);
    TEST_ASSERT_EQUAL_INT64(3, after[n_after - 1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_is_byte_identical);
    RUN_TEST(test_keys_and_order);
    RUN_TEST(test_round_trip_abc);
    RUN_TEST(test_reader_sets_level_attribute_and_counts_prov);
    RUN_TEST(test_version_policy);
    RUN_TEST(test_version_must_be_integer_one);
    RUN_TEST(test_unknown_odin3_member_is_counted);
    RUN_TEST(test_bad_prov_is_an_error);
    RUN_TEST(test_good_prov_with_eight_hdl_is_read);
    RUN_TEST(test_bad_level_is_an_error);
    RUN_TEST(test_level_bounds_are_accepted);
    RUN_TEST(test_level_key_placement);
    RUN_TEST(test_levels_survive_levelize_write_read);
    return UNITY_END();
}
