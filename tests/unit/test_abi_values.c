/*
 * test_abi_values.c — ABI parameter text for the value kinds BLIF never produces (BITS, STRING),
 * net alias names and the out-of-memory paths; builds the IR internally (links odin3_core).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdint.h>
#include <string.h>

enum { OOM_SWEEP = 64 };

static odin3_design *design;
static odin3_module *module;
static uint32_t module_id;

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

static uint32_t intern(const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &str));
    return str;
}

static odin3_celltype_id find_type(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern(name), &id));
    return id;
}

void setUp(void) {
    odin3_log_set_sink(quiet_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("m"), (odin3_prov_id){0}, &id));
    module = odin3_module_get(design, id);
    module_id = id.v;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* An $adff of width 4 whose ARST_VALUE is, LSB first, 1 0 x z. */
static odin3_ref make_adff(void) {
    static const uint8_t bits[] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X, ODIN3_BIT_Z};
    odin3_value params[4] = {odin3_value_int(4),
                             odin3_value_int(1),
                             odin3_value_int(0),
                             {ODIN3_VAL_BITS, 0, bits, 4, 0, 0}};
    odin3_node_spec spec = {find_type("$adff"), 0, {0}, params, 4};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    return (odin3_ref){module_id, node.v};
}

static const char *text_of(odin3_ref node, uint32_t index) {
    const char *text = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_text(design, node, index, &text));
    return text;
}

static void test_bits_text_msb_first(void) {
    odin3_ref adff = make_adff();
    TEST_ASSERT_EQUAL_STRING("zx01", text_of(adff, 3));
    TEST_ASSERT_EQUAL_STRING("0", text_of(adff, 2));
    odin3_value_kind kind = ODIN3_VAL_INT;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_param_kind(design, adff, 3, &kind));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, kind);
    odin3_span pins = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_get_port_pins(design, adff, 3, &pins));
    TEST_ASSERT_EQUAL_UINT32(4, pins.count); /* Q follows WIDTH */
}

static void test_string_and_empty_bits_text(void) {
    odin3_value params[13];
    const odin3_celltype_def *def = odin3_celltype_get(design, find_type("$mem"));
    TEST_ASSERT_EQUAL_UINT32(13, def->n_params);
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
    }
    params[0].str = intern("ram0");
    odin3_node_spec spec = {find_type("$mem"), intern("u_mem"), {0}, params, def->n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    odin3_ref mem = {module_id, node.v};
    TEST_ASSERT_EQUAL_STRING("ram0", text_of(mem, 0));
    TEST_ASSERT_EQUAL_STRING("", text_of(mem, 12)); /* INIT: no bits */
    TEST_ASSERT_EQUAL_STRING("0", text_of(mem, 7));
    int64_t value = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_get_param_int(design, mem, 0, &value));
}

static void test_negative_int_text(void) {
    odin3_value params[1] = {odin3_value_int(-3)};
    /* $_FF_ INIT must be 0..3, so use a design-local black box with one INT parameter */
    static const odin3_param_def k_params[] = {
        {"P", ODIN3_VAL_INT, {ODIN3_VAL_INT, 0, NULL, 0, 0, 0}}};
    odin3_celltype_def def = {"bbp", ODIN3_GRAN_BLACKBOX, 0, NULL, 0, k_params, 1, NULL, NULL};
    odin3_celltype_id type = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &type));
    odin3_node_spec spec = {type, 0, {0}, params, 1};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &node));
    TEST_ASSERT_EQUAL_STRING("-3", text_of((odin3_ref){module_id, node.v}, 0));
}

static void test_alias_names(void) {
    odin3_wire_spec wspec = {intern("w"), 3, 2, false, {0}}; /* [3:2]: bit 0 is index 2 */
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &wspec, NULL, &wire));
    odin3_net_id keep = {0};
    odin3_net_id drop = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(module, intern("keep"), (odin3_prov_id){0}, &keep));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(module, intern("drop"), (odin3_prov_id){0}, &drop));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
    odin3_net_id bit1 = odin3_wire_net(module, wire, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, bit1}));
    odin3_ref net = {module_id, keep.v};
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_alias_count(design, net, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count); /* drop's name, then bit1's primary (w, 1) */
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_alias_name(design, net, 0, &name));
    TEST_ASSERT_EQUAL_STRING("drop", name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_get_alias_name(design, net, 1, &name));
    TEST_ASSERT_EQUAL_STRING("w[3]", name);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_net_get_alias_name(design, net, 2, &name));
    uint32_t found = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_lookup_net(design, module_id, "drop", &found));
    TEST_ASSERT_EQUAL_UINT32(keep.v, found);
    bool live = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_is_live(design, (odin3_ref){module_id, drop.v}, &live));
    TEST_ASSERT_FALSE(live);
    uint32_t nets = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_net_count(design, module_id, &nets));
    TEST_ASSERT_EQUAL_UINT32(2, nets); /* keep and w's bit 0 net */
}

/* The computed text is interned: on out of memory the call fails and its output is unchanged. */
static void test_param_text_out_of_memory(void) {
    odin3_ref adff = make_adff();
    bool failed = false;
    for (long fail_after = 0; fail_after < OOM_SWEEP; fail_after++) {
        const char *text = "keep";
        odin3_util_set_alloc_fail_after(fail_after);
        odin3_status st = odin3_node_get_param_text(design, adff, 3, &text);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_STRING("zx01", text);
            break;
        }
        failed = true;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_STRING("keep", text);
    }
    TEST_ASSERT_TRUE(failed);
}

static void test_attr_set_out_of_memory(void) {
    odin3_obj obj = {module_id, ODIN3_OBJ_MODULE, 0};
    bool failed = false;
    for (long fail_after = 0; fail_after < OOM_SWEEP; fail_after++) {
        odin3_util_set_alloc_fail_after(fail_after);
        odin3_status st = odin3_attr_set_string(design, obj, "fresh key", "fresh value");
        odin3_util_set_alloc_fail_after(-1);
        const char *value = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_get_string(design, obj, "fresh key", &value));
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_STRING("fresh value", value);
            break;
        }
        failed = true;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(value); /* the IR is unchanged */
    }
    TEST_ASSERT_TRUE(failed);
}

static void test_attr_not_a_string(void) {
    odin3_value one = odin3_value_int(1);
    odin3_objref obj = {ODIN3_OBJ_MODULE, module_id};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern("num"), &one));
    const char *value = "keep";
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_attr_get_string(design, (odin3_obj){module_id, ODIN3_OBJ_MODULE, 0}, "num", &value));
    TEST_ASSERT_EQUAL_STRING("keep", value);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bits_text_msb_first);
    RUN_TEST(test_string_and_empty_bits_text);
    RUN_TEST(test_negative_int_text);
    RUN_TEST(test_alias_names);
    RUN_TEST(test_param_text_out_of_memory);
    RUN_TEST(test_attr_set_out_of_memory);
    RUN_TEST(test_attr_not_a_string);
    return UNITY_END();
}
