/*
 * test_edit_gaps.c — the edit gaps of docs/specs/2026-10-31-edit-gaps-design.md through
 * libodin3.so only: edit events (added, param, rewired, removed), their export in BLIF
 * (--edit-tags) and JSON (--odin3) and the round trips, the default outputs without them, and
 * initial parameters of add_node.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static odin3_design *design;
static uint32_t top;

static const char K_BLIF[] = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n";

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    FILE *file = fopen(ODIN3_TEST_TMP ".in.blif", "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fputs(K_BLIF, file);
    (void)fclose(file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "read_blif", ODIN3_TEST_TMP ".in.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

static char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = calloc(1, 1U << 16);
    TEST_ASSERT_NOT_NULL(text);
    (void)fread(text, 1, (1U << 16) - 1U, file);
    (void)fclose(file);
    return text;
}

static odin3_ref net_named(const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, top, name, &id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, id);
    return (odin3_ref){top, id};
}

static odin3_ref pin_of(odin3_ref node, uint32_t index) {
    odin3_span pins;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(design, node, &pins));
    TEST_ASSERT_TRUE(index < pins.count);
    return (odin3_ref){top, pins.first + index};
}

static odin3_ref add_sop(const char *name, const char *tag) {
    odin3_ref out;
    odin3_edit_new what = {sizeof what,  0, .name = name, .tag = tag, .cover = "11 1\n",
                           .n_inputs = 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_sop(design, top, &what, &out));
    return out;
}

static const char *events_of(uint32_t kind, odin3_ref ref) {
    const char *text = "unset";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_get_events(design, (odin3_obj){top, kind, ref.id}, &text));
    return text;
}

static void test_events_added_param_rewired_removed(void) {
    odin3_ref gate = add_sop("g", "trojan:t1");
    odin3_edit_new net_what = {sizeof net_what, 0, .name = "tq", .tag = "trojan:t1"};
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, &net_what, &net));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\n", events_of(ODIN3_OBJ_NODE, gate));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\n", events_of(ODIN3_OBJ_NET, net));
    TEST_ASSERT_NULL(events_of(ODIN3_OBJ_NET, net_named("a")));
    odin3_ref in0 = pin_of(gate, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, in0, net_named("a")));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\n",
                             events_of(ODIN3_OBJ_NODE, gate)); /* connect: none */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire(design, in0, net_named("b")));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\nrewired edit\n", events_of(ODIN3_OBJ_NODE, gate));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire(design, in0, net_named("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire_tagged(design, in0, net_named("a"), "t2"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire_tagged(design, in0, net_named("b"), "t2"));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\nrewired edit\nrewired t2\n",
                             events_of(ODIN3_OBJ_NODE, gate));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_disconnect_tagged(design, in0, "t3"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_disconnect_tagged(design, in0, "t4")); /* no-op */
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\nrewired edit\nrewired t2\nremoved t3\n",
                             events_of(ODIN3_OBJ_NODE, gate));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_disconnect(design, pin_of(gate, 1)));
    /* an unconnected pin: nothing moved, nothing recorded */
    TEST_ASSERT_NULL(strstr(events_of(ODIN3_OBJ_NODE, gate), "removed edit"));
}

static void test_clean_node_rewire_marks_it_and_bad_tags_are_refused(void) {
    uint32_t drv_pin = 0;
    uint32_t drv_node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver(design, net_named("y"), &drv_pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_get_node(design, (odin3_ref){top, drv_pin}, &drv_node));
    odin3_ref node = {top, drv_node};
    odin3_ref pin = pin_of(node, 0); /* the first input of the .names */
    TEST_ASSERT_NULL(events_of(ODIN3_OBJ_NODE, node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_rewire_tagged(design, pin, net_named("b"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_rewire_tagged(design, pin, net_named("b"), "x\ny"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_disconnect_tagged(design, pin, NULL));
    TEST_ASSERT_NULL(events_of(ODIN3_OBJ_NODE, node));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_rewire_tagged(design, pin, net_named("b"), "t9"));
    TEST_ASSERT_EQUAL_STRING("rewired t9\n", events_of(ODIN3_OBJ_NODE, node));
    odin3_edit_new what = {sizeof what, 0, .name = "bad", .tag = "x\ny"};
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, &what, &net));
}

static void test_set_cover_twin_keeps_events_and_adds_param(void) {
    odin3_ref gate = add_sop("g", "t1");
    odin3_ref twin;
    odin3_edit_new what = {sizeof what, 0, .tag = "t2", .cover = "00 1\n"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_set_cover(design, gate, &what, &twin));
    TEST_ASSERT_EQUAL_STRING("added t1\nparam t2\n", events_of(ODIN3_OBJ_NODE, twin));
}

static void test_get_tag_after_a_round_trip_falls_back_to_the_events(void) {
    odin3_ref gate = add_sop("tg", "trojan:t1");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_rewire_tagged(design, pin_of(gate, 0), net_named("b"), "t 2,%#"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "write_blif",
                                                          "--edit-tags " ODIN3_TEST_TMP ".t.blif"));
    char *text = slurp(ODIN3_TEST_TMP ".t.blif");
    TEST_ASSERT_NOT_NULL(strstr(text, ".attr odin3_edit added:trojan:t1,rewired:t%202%2C%25%23\n"));
    free(text);
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(again, "read_blif", ODIN3_TEST_TMP ".t.blif"));
    uint32_t atop = 0;
    uint32_t node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(again, &atop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(again, atop, "tg", &node));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node);
    const char *events = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_events(again, (odin3_obj){atop, ODIN3_OBJ_NODE, node}, &events));
    TEST_ASSERT_EQUAL_STRING("added trojan:t1\nrewired t 2,%#\n", events);
    const char *tag = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_get_tag(again, (odin3_obj){atop, ODIN3_OBJ_NODE, node}, &tag));
    TEST_ASSERT_EQUAL_STRING("trojan:t1", tag);
    /* a default write after the read does not print the events */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(again, "write_blif", ODIN3_TEST_TMP ".u.blif"));
    text = slurp(ODIN3_TEST_TMP ".u.blif");
    TEST_ASSERT_NULL(strstr(text, "odin3_edit"));
    free(text);
    odin3_design_destroy(again);
}

static void test_default_blif_and_json_do_not_change_with_events(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_blif", ODIN3_TEST_TMP ".a.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_json", ODIN3_TEST_TMP ".a.json"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "write_json",
                                                          "--odin3 " ODIN3_TEST_TMP ".ao.json"));
    odin3_ref gate = add_sop("g", "t1");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_disconnect(design, pin_of(gate, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_blif", ODIN3_TEST_TMP ".b.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_json", ODIN3_TEST_TMP ".b.json"));
    char *text = slurp(ODIN3_TEST_TMP ".b.blif");
    TEST_ASSERT_NULL(strstr(text, "odin3_edit"));
    free(text);
    text = slurp(ODIN3_TEST_TMP ".b.json");
    TEST_ASSERT_NULL(strstr(text, "odin3_edit"));
    free(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "write_json",
                                                          "--odin3 " ODIN3_TEST_TMP ".bo.json"));
    text = slurp(ODIN3_TEST_TMP ".bo.json");
    TEST_ASSERT_NOT_NULL(strstr(text, "\"odin3_edit\": \"added:t1,removed:edit\""));
    free(text);
}

static void test_json_odin3_round_trip_restores_cell_and_net_events(void) {
    odin3_ref gate = add_sop("jg", "trojan:j");
    odin3_edit_new net_what = {sizeof net_what, 0, .name = "jn", .tag = "trojan:j"};
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, &net_what, &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 2), net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_rewire_tagged(design, pin_of(gate, 0), net_named("b"), "r"));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_run_pass(design, "write_json", "--odin3 " ODIN3_TEST_TMP ".j.json"));
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(again, "read_json", ODIN3_TEST_TMP ".j.json"));
    uint32_t atop = 0;
    uint32_t node = 0;
    uint32_t anet = 0;
    odin3_span pins;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(again, &atop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_node(again, atop, "jg", &node));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node);
    /* the edit-made net has no name of its own (a netname is a wire): reach it by its driver */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_pins(again, (odin3_ref){atop, node}, &pins));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_get_net(again, (odin3_ref){atop, pins.first + 2U}, &anet));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, anet);
    const char *events = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_events(again, (odin3_obj){atop, ODIN3_OBJ_NODE, node}, &events));
    TEST_ASSERT_EQUAL_STRING("added trojan:j\nrewired r\n", events);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_edit_get_events(again, (odin3_obj){atop, ODIN3_OBJ_NET, anet}, &events));
    TEST_ASSERT_EQUAL_STRING("added trojan:j\n", events);
    odin3_design_destroy(again);
}

/*
 * The exports of a fixed edit sequence against the committed files in tests/golden/edit_tags
 * (ODIN3_EDIT_GOLDEN). ODIN3_UPDATE_GOLDEN=1 rewrites them.
 */
static void check_golden(const char *golden_name) {
    const char *produced =
        strstr(golden_name, ".json") != NULL ? ODIN3_TEST_TMP ".g.json" : ODIN3_TEST_TMP ".g.blif";
    char golden[512];
    (void)snprintf(golden, sizeof golden, "%s/%s", ODIN3_EDIT_GOLDEN, golden_name);
    char *got = slurp(produced);
    if (getenv("ODIN3_UPDATE_GOLDEN") != NULL) {
        FILE *file = fopen(golden, "wb");
        TEST_ASSERT_NOT_NULL(file);
        (void)fputs(got, file);
        (void)fclose(file);
    }
    char *want = slurp(golden);
    TEST_ASSERT_EQUAL_STRING(want, got);
    free(want);
    free(got);
}

static void test_golden_exports_of_a_fixed_edit_sequence(void) {
    odin3_ref gate = add_sop("trojan_and", "trojan:t1");
    odin3_edit_new net_what = {sizeof net_what, 0, .name = "trojan_q", .tag = "trojan:t1"};
    odin3_ref net;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, &net_what, &net));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 0), net_named("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 1), net_named("b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_connect(design, pin_of(gate, 2), net));
    uint32_t drv_pin = 0;
    uint32_t drv_node = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_driver(design, net_named("y"), &drv_pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_get_node(design, (odin3_ref){top, drv_pin}, &drv_node));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_edit_rewire_tagged(design, pin_of((odin3_ref){top, drv_node}, 1), net, "trojan:t1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "write_blif",
                                                          "--edit-tags " ODIN3_TEST_TMP ".g.blif"));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_run_pass(design, "write_json", "--odin3 " ODIN3_TEST_TMP ".g.json"));
    check_golden("edit_tags.blif");
    check_golden("edit_tags_odin3.json");
}

static void test_edit_tags_is_refused_for_the_vtr_dialect(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_design_run_pass(design, "write_blif", "--vtr --edit-tags " ODIN3_TEST_TMP ".v.blif"));
}

/* --- add_node with initial parameters (gap 3) ---------------------------------------------- */

static int64_t param_of(odin3_ref node, const char *name) {
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_count(design, node, &count));
    for (uint32_t i = 0; i < count; i++) {
        const char *pname = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_name(design, node, i, &pname));
        if (strcmp(pname, name) == 0) {
            int64_t value = -99;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_int(design, node, i, &value));
            return value;
        }
    }
    TEST_FAIL_MESSAGE("no such parameter");
    return 0;
}

static void test_add_node_takes_initial_params_without_a_twin(void) {
    odin3_ref plain;
    odin3_edit_new base = {sizeof base, 0, .name = "ff0", .tag = "t", .cell_type = "$_DFF_P_"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_node(design, top, &base, &plain));
    TEST_ASSERT_EQUAL_INT64(3, param_of(plain, "INIT"));
    const char *names[] = {"INIT"};
    const int64_t values[] = {0};
    odin3_edit_new what = {sizeof what,
                           0,
                           .name = "ff1",
                           .tag = "t",
                           .cell_type = "$_DFF_P_",
                           .param_names = names,
                           .param_values = values,
                           .n_params = 1};
    odin3_ref ff;
    uint32_t before = 0;
    uint32_t after = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_node(design, top, &what, &ff));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &after));
    TEST_ASSERT_EQUAL_UINT32(before + 1, after); /* one node, no twin */
    TEST_ASSERT_EQUAL_INT64(0, param_of(ff, "INIT"));
    TEST_ASSERT_EQUAL_STRING("added t\n", events_of(ODIN3_OBJ_NODE, ff));
}

static void test_add_node_bad_params_change_nothing(void) {
    const char *names[] = {"NOPE", "INIT"};
    const int64_t values[] = {1, 1};
    odin3_edit_new what = {sizeof what,
                           0,
                           .name = "ff2",
                           .tag = "t",
                           .cell_type = "$_DFF_P_",
                           .param_names = names,
                           .param_values = values,
                           .n_params = 1};
    odin3_ref ff;
    uint32_t before = 0;
    uint32_t after = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_node(design, top, &what, &ff));
    names[0] = "INIT"; /* repeated */
    what.n_params = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_node(design, top, &what, &ff));
    what.n_params = 9;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_node(design, top, &what, &ff));
    what.n_params = 1;
    what.param_names = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_node(design, top, &what, &ff));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &after));
    TEST_ASSERT_EQUAL_UINT32(before, after);
}

/* The struct as the first release had it (56 bytes): still accepted, new fields read as zero. */
typedef struct old_edit_new {
    uint32_t size, reserved;
    const char *name, *tag, *cell_type, *cover;
    int64_t value;
    uint32_t n_inputs, index;
} old_edit_new;

static void test_old_sized_struct_still_works(void) {
    old_edit_new old = {sizeof old, 0, .name = "ff3", .tag = "t", .cell_type = "$_DFF_P_"};
    TEST_ASSERT_EQUAL_UINT32(56, old.size);
    odin3_ref ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_edit_add_node(design, top, (const odin3_edit_new *)&old, &ff));
    TEST_ASSERT_EQUAL_INT64(3, param_of(ff, "INIT"));
}

/* --- fix round: the reserved key, NUL escapes, size versioning ----------------------------- */

static void test_user_attribute_named_odin3_edit_does_not_break_the_writers(void) {
    odin3_ref gate = add_sop("ua", "trojan:u");
    odin3_ref other = add_sop("ub", "trojan:v");
    odin3_obj obj = {top, ODIN3_OBJ_NODE, gate.id};
    /* a user attribute of the file name; and one under the reserved key itself (malformed) */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set_string(design, obj, "odin3_edit", "garbage"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_attr_set_string(design, (odin3_obj){top, ODIN3_OBJ_NODE, other.id},
                                                "odin3 edit", "garbage"));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_design_run_pass(design, "write_blif", "--edit-tags " ODIN3_TEST_TMP ".ua.blif"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "write_json",
                                                          "--odin3 " ODIN3_TEST_TMP ".ua.json"));
    char *text = slurp(ODIN3_TEST_TMP ".ua.blif");
    TEST_ASSERT_NOT_NULL(strstr(text, ".attr odin3_edit added:trojan:u\n"));
    free(text);
    text = slurp(ODIN3_TEST_TMP ".ua.json");
    TEST_ASSERT_NOT_NULL(strstr(text, "\"odin3_edit\": \"added:trojan:u\""));
    free(text);
    /* the user attribute named odin3_edit is still an ordinary attribute in a default write */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(design, "write_json", ODIN3_TEST_TMP ".ub.json"));
    text = slurp(ODIN3_TEST_TMP ".ub.json");
    TEST_ASSERT_NOT_NULL(strstr(text, "\"odin3_edit\": \"garbage\""));
    free(text);
}

static void test_nul_escape_in_an_edit_attr_is_read_as_a_plain_attribute(void) {
    FILE *file = fopen(ODIN3_TEST_TMP ".nul.blif", "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fputs(".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n"
                ".attr odin3_edit added:a%00b\n.end\n",
                file);
    (void)fclose(file);
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_run_pass(again, "read_blif", ODIN3_TEST_TMP ".nul.blif"));
    odin3_design_destroy(again);
}

static void test_tag_with_a_newline_is_refused_by_every_creation_call(void) {
    odin3_edit_new what = {
        sizeof what,  0, .name = "nl", .tag = "x\ny", .cell_type = "$_DFF_P_", .cover = "11 1\n",
        .n_inputs = 2};
    odin3_ref out;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_node(design, top, &what, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_sop(design, top, &what, &out));
    odin3_ref gate = add_sop("nlg", "ok");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_set_cover(design, gate, &what, &out));
}

/* A caller's struct that extends the current one (a later library's): accepted. */
typedef struct big_edit_new {
    odin3_edit_new base;
    uint64_t later_field;
} big_edit_new;

static void test_edit_new_sizes_are_exactly_v1_or_at_least_current(void) {
    const uint32_t v1 = 56;
    odin3_edit_new what = {sizeof what, 0, .name = "sz0", .tag = "t"};
    odin3_ref net;
    TEST_ASSERT_EQUAL_UINT32(v1, (uint32_t)offsetof(odin3_edit_new, param_names));
    for (uint32_t size = 0; size < sizeof what + 8U; size++) {
        what.size = size;
        what.name = NULL;
        odin3_status want = size == v1 || size >= sizeof what ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, odin3_edit_add_net(design, top, &what, &net),
                                      "size accepted or refused wrongly");
    }
    big_edit_new big = {{sizeof big, 0, .name = "sz1", .tag = "t"}, 7};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_edit_add_net(design, top, &big.base, &net));
}

static void test_n_params_is_refused_where_params_are_not_used(void) {
    const char *names[] = {"INIT"};
    const int64_t values[] = {0};
    odin3_edit_new what = {sizeof what,          0,
                           .name = "np",         .tag = "t",
                           .cover = "11 1\n",    .n_inputs = 2,
                           .param_names = names, .param_values = values,
                           .n_params = 1};
    odin3_ref out;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_net(design, top, &what, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_add_sop(design, top, &what, &out));
    odin3_ref gate = add_sop("npg", "ok");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_edit_set_cover(design, gate, &what, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_edit_set_param_int(design, gate, &what, &out));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_events_added_param_rewired_removed);
    RUN_TEST(test_clean_node_rewire_marks_it_and_bad_tags_are_refused);
    RUN_TEST(test_set_cover_twin_keeps_events_and_adds_param);
    RUN_TEST(test_get_tag_after_a_round_trip_falls_back_to_the_events);
    RUN_TEST(test_default_blif_and_json_do_not_change_with_events);
    RUN_TEST(test_json_odin3_round_trip_restores_cell_and_net_events);
    RUN_TEST(test_golden_exports_of_a_fixed_edit_sequence);
    RUN_TEST(test_edit_tags_is_refused_for_the_vtr_dialect);
    RUN_TEST(test_add_node_takes_initial_params_without_a_twin);
    RUN_TEST(test_add_node_bad_params_change_nothing);
    RUN_TEST(test_old_sized_struct_still_works);
    RUN_TEST(test_user_attribute_named_odin3_edit_does_not_break_the_writers);
    RUN_TEST(test_nul_escape_in_an_edit_attr_is_read_as_a_plain_attribute);
    RUN_TEST(test_tag_with_a_newline_is_refused_by_every_creation_call);
    RUN_TEST(test_edit_new_sizes_are_exactly_v1_or_at_least_current);
    RUN_TEST(test_n_params_is_refused_where_params_are_not_used);
    return UNITY_END();
}
