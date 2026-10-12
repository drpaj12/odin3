/*
 * test_ffinit.c — initial values on the `init` attribute (2E spec §10; 2F spec §9.4; Yosys
 * kernel/ffinit.h set_init and remove_init): the target bit, the other memberships cleared, an
 * attribute left all x removed, defined_on, and out of memory.
 * Yosys re-derivation: none (no proc phase runs here).
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "passes/ffinit.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { BITS_MAX = 16, OOM_LIMIT = 200 };

static odin3_design *design;
static odin3_module *module;
static size_t n_errors;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    n_errors += level == ODIN3_LOG_ERROR ? 1 : 0;
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("m"), (odin3_prov_id){0}, &id));
    module = odin3_module_get(design, id);
    n_errors = 0;
    odin3_log_set_sink(count_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

/* A wire [width-1:0] on fresh nets (nets NULL) or on the given nets. */
static odin3_wire_id wire_of(const char *name, uint32_t width, const odin3_net_id *nets) {
    odin3_wire_spec spec = {intern(name), (int32_t)width - 1, 0, false, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, nets, &wire));
    return wire;
}

static odin3_net_id bit_net(odin3_wire_id wire, uint32_t bit) {
    return odin3_wire_net(module, wire, bit);
}

static odin3_objref wire_ref(odin3_wire_id wire) {
    return (odin3_objref){ODIN3_OBJ_WIRE, wire.v};
}

/* Sets wire's `init` from text, MSB first ("1x01"). */
static void set_init(odin3_wire_id wire, const char *text) {
    static const char k_chars[] = "01xz";
    uint8_t bits[BITS_MAX];
    size_t len = strlen(text);
    TEST_ASSERT_TRUE(len <= BITS_MAX);
    for (size_t i = 0; i < len; i++) {
        bits[i] = (uint8_t)(strchr(k_chars, text[len - 1 - i]) - k_chars);
    }
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = bits, .len = (uint32_t)len};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, wire_ref(wire), intern("init"), &val));
}

/* The `init` value as text, MSB first. */
static void init_text(const odin3_value *val, char *got) {
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, val->kind);
    TEST_ASSERT_TRUE(val->len <= BITS_MAX);
    for (uint32_t i = 0; i < val->len; i++) {
        got[val->len - 1 - i] = "01xz"[val->bits[i]];
    }
    got[val->len] = '\0';
}

/* Asserts wire's `init` reads text, MSB first; NULL text: no `init` at all. */
static void expect_init(odin3_wire_id wire, const char *text) {
    const odin3_value *val = odin3_attr_get(module, wire_ref(wire), intern("init"));
    if (text == NULL) {
        TEST_ASSERT_NULL(val);
        return;
    }
    TEST_ASSERT_NOT_NULL(val);
    char got[BITS_MAX + 1] = {0};
    init_text(val, got);
    TEST_ASSERT_EQUAL_STRING(text, got);
}

/* n = bit 1 of w (its primary), also both bits of v (aliases), v's init 2'b01: v is left all x. */
static void test_set_primary(void) {
    odin3_wire_id wire_w = wire_of("w", 4, NULL);
    odin3_net_id net_n = bit_net(wire_w, 1);
    odin3_net_id both[2] = {net_n, net_n};
    odin3_wire_id wire_v = wire_of("v", 2, both);
    set_init(wire_v, "01");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, net_n, ODIN3_BIT_1));
    expect_init(wire_w, "xx1x");
    expect_init(wire_v, NULL);
    check_full();
}

/* Clearing one bit of another membership keeps that wire's other defined bits. */
static void test_set_clears_other_memberships(void) {
    odin3_wire_id wire_w = wire_of("w", 4, NULL);
    odin3_net_id net_n = bit_net(wire_w, 2);
    odin3_net_id other = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &other));
    odin3_net_id nets[2] = {net_n, other};
    odin3_wire_id wire_v = wire_of("v", 2, nets);
    set_init(wire_v, "01");
    set_init(wire_w, "1xxx");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, net_n, ODIN3_BIT_0));
    expect_init(wire_w, "10xx");
    expect_init(wire_v, "0x");
    /* x on the target: written, and an attribute left all x is removed */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, bit_net(wire_w, 3), ODIN3_BIT_X));
    expect_init(wire_w, "x0xx");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, net_n, ODIN3_BIT_X));
    expect_init(wire_w, NULL);
    check_full();
}

/* An unnamed net whose only membership is the alias u[2] (its primary wire was deleted). */
static void test_set_alias_only(void) {
    odin3_net_id net_x = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &net_x));
    odin3_wire_id wire_t = wire_of("t", 1, &net_x);
    odin3_wire_id wire_f = wire_of("f", 4, NULL);
    odin3_net_id nets[4] = {bit_net(wire_f, 0), bit_net(wire_f, 1), net_x, bit_net(wire_f, 3)};
    odin3_wire_id wire_u = wire_of("u", 4, nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_delete(module, wire_t));
    TEST_ASSERT_FALSE(odin3_wire_valid(odin3_net_primary(module, net_x).wire));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, net_x, ODIN3_BIT_1));
    expect_init(wire_u, "x1xx");
    expect_init(wire_f, NULL);
    check_full();
}

/* A net with no wire membership ($tmp): nothing to write anywhere. */
static void test_set_no_wire(void) {
    odin3_wire_id wire_w = wire_of("w", 2, NULL);
    odin3_net_id tmp = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &tmp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(module, tmp, ODIN3_BIT_1));
    expect_init(wire_w, NULL);
    odin3_wire_id where = {1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, tmp, &where));
    TEST_ASSERT_FALSE(odin3_wire_valid(where));
    TEST_ASSERT_EQUAL_size_t(0, n_errors);
}

/* w's init 4'b1x01, bit by bit; defined_on names w only for a defined bit. */
static void test_remove(void) {
    odin3_wire_id wire_w = wire_of("w", 4, NULL);
    set_init(wire_w, "1x01");
    odin3_wire_id where = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, bit_net(wire_w, 0), &where));
    expect_init(wire_w, "1x0x");
    TEST_ASSERT_EQUAL_UINT32(wire_w.v, where.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, bit_net(wire_w, 3), &where));
    TEST_ASSERT_EQUAL_UINT32(wire_w.v, where.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, bit_net(wire_w, 2), &where));
    TEST_ASSERT_FALSE(odin3_wire_valid(where)); /* an x bit */
    expect_init(wire_w, "xx0x");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, bit_net(wire_w, 1), NULL));
    TEST_ASSERT_NULL(odin3_attr_get(module, wire_ref(wire_w), intern("init")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, bit_net(wire_w, 1), &where));
    TEST_ASSERT_FALSE(odin3_wire_valid(where));
    check_full();
}

/* Every membership is cleared; defined_on is the first, in primary-then-alias order, that was
 * defined. */
static void test_remove_every_membership(void) {
    odin3_wire_id wire_w = wire_of("w", 1, NULL);
    odin3_net_id net_n = bit_net(wire_w, 0);
    odin3_wire_id wire_v = wire_of("v", 1, &net_n);
    odin3_wire_id wire_z = wire_of("z", 1, &net_n);
    set_init(wire_v, "1");
    set_init(wire_z, "0");
    odin3_wire_id where = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_remove(module, net_n, &where));
    TEST_ASSERT_EQUAL_UINT32(wire_v.v, where.v);
    expect_init(wire_w, NULL);
    expect_init(wire_v, NULL);
    expect_init(wire_z, NULL);
}

/* Misuse is INVALID_ARG (logged). */
static void test_rejects(void) {
    odin3_wire_id wire_w = wire_of("w", 1, NULL);
    odin3_net_id net_n = bit_net(wire_w, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ffinit_set(NULL, net_n, ODIN3_BIT_1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ffinit_set(module, (odin3_net_id){99}, ODIN3_BIT_1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ffinit_set(module, net_n, (odin3_bit)7));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ffinit_remove(module, (odin3_net_id){99}, NULL));
    TEST_ASSERT_EQUAL_size_t(4, n_errors);
}

/* Out of memory at every allocation of a set: NO_MEMORY or OK, valid IR, no leak (ASan). */
static void test_set_oom(void) {
    bool done = false;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        tearDown();
        setUp();
        odin3_wire_id wire_w = wire_of("w", 4, NULL);
        odin3_net_id net_n = bit_net(wire_w, 1);
        odin3_net_id both[2] = {net_n, net_n};
        odin3_wire_id wire_v = wire_of("v", 2, both);
        set_init(wire_v, "01");
        odin3_util_set_alloc_fail_after(fail);
        odin3_status st = odin3_ffinit_set(module, net_n, ODIN3_BIT_1);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        check_full();
        done = st == ODIN3_OK;
        if (done) {
            expect_init(wire_w, "xx1x");
            expect_init(wire_v, NULL);
        }
    }
    TEST_ASSERT_TRUE(done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_primary);
    RUN_TEST(test_set_clears_other_memberships);
    RUN_TEST(test_set_alias_only);
    RUN_TEST(test_set_no_wire);
    RUN_TEST(test_remove);
    RUN_TEST(test_remove_every_membership);
    RUN_TEST(test_rejects);
    RUN_TEST(test_set_oom);
    return UNITY_END();
}
