/*
 * test_abc_engine_abi.c — the ABC engine ABI (3D spec §4.3, AB-7; plan Task 4, point (b)):
 * odin3_abc_engine_register's checks and texts, the registered copy, "final", and the test
 * override. Registration is process-final, so the cases run in main's order: every refusal comes
 * before the one valid registration.
 */
#include "odin3/odin3.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/engine.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

enum { LOG_CAP = 512, LINE_CAP = 256 };

static const char *const COMMIT = "a3001b72edc5de22442e942165487fddf150e3d0";

static char last_line[LOG_CAP];
static size_t line_count;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t len = strlen(msg);
    if (len >= LOG_CAP) {
        len = LOG_CAP - 1;
    }
    memcpy(last_line, msg, len);
    last_line[len] = '\0';
    line_count++;
}

void setUp(void) {
    last_line[0] = '\0';
    line_count = 0;
    odin3_log_set_sink(capture_sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static odin3_status fake_open(void *self, odin3_abc_text_fn text, void *user,
                              odin3_abc_fail *fail) {
    (void)self;
    (void)text;
    (void)user;
    (void)fail;
    return ODIN3_OK;
}

static odin3_status fake_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail) {
    (void)self;
    (void)aig;
    (void)fail;
    return ODIN3_OK;
}

static odin3_status fake_exec(void *self, const char *command, odin3_abc_fail *fail) {
    (void)self;
    (void)command;
    (void)fail;
    return ODIN3_OK;
}

static odin3_status fake_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    (void)self;
    (void)luts;
    (void)fail;
    return ODIN3_OK;
}

static odin3_status fake_name_map(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                                  odin3_abc_fail *fail) {
    (void)self;
    (void)aligned;
    (void)lits;
    (void)fail;
    return ODIN3_OK;
}

static void fake_close(void *self) {
    (void)self;
}

/* A valid table named name. */
static odin3_abc_engine valid_engine(const char *name) {
    odin3_abc_engine engine = {
        .version = ODIN3_ABC_ENGINE_VERSION,
        .name = name,
        .abc_commit = COMMIT,
        .self = NULL,
        .open = fake_open,
        .load_aig = fake_load_aig,
        .exec = fake_exec,
        .get_luts = fake_get_luts,
        .name_map = fake_name_map,
        .close = fake_close,
        .reserved = {NULL, NULL, NULL, NULL},
    };
    return engine;
}

/* Registering engine fails with INVALID_ARG, logs exactly the one line want, and stores nothing. */
static void expect_refused(const odin3_abc_engine *engine, const char *want) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_engine_register(engine));
    TEST_ASSERT_EQUAL_size_t(1, line_count);
    TEST_ASSERT_EQUAL_STRING(want, last_line);
    TEST_ASSERT_NULL(odin3_abc_engine_get());
    last_line[0] = '\0';
    line_count = 0;
}

static void test_null_engine(void) {
    TEST_ASSERT_NULL(odin3_abc_engine_get());
    expect_refused(NULL, "odin3_abc_engine_register: invalid argument engine");
}

static void test_other_version(void) {
    odin3_abc_engine engine = valid_engine("fake");
    engine.version = 2;
    expect_refused(&engine, "odin3_abc_engine_register: engine version 2, expected 1");
}

/* Agent default: a NULL name is refused as a NULL engine (the later texts name the engine). */
static void test_null_name(void) {
    odin3_abc_engine engine = valid_engine(NULL);
    expect_refused(&engine, "odin3_abc_engine_register: invalid argument engine");
}

enum { FUNCTION_COUNT = 6, RESERVED_COUNT = 4 };

/* A valid table named "fake" with function fn (0 .. 5, table order) NULL. */
static odin3_abc_engine without_function(size_t fn) {
    odin3_abc_engine engine = valid_engine("fake");
    switch (fn) {
    case 0:
        engine.open = NULL;
        break;
    case 1:
        engine.load_aig = NULL;
        break;
    case 2:
        engine.exec = NULL;
        break;
    case 3:
        engine.get_luts = NULL;
        break;
    case 4:
        engine.name_map = NULL;
        break;
    default:
        engine.close = NULL;
        break;
    }
    return engine;
}

static void test_null_exec(void) {
    odin3_abc_engine engine = valid_engine("fake");
    engine.exec = NULL;
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake' has no 'exec' function");
    /* Table order: open is named before exec. */
    engine.open = NULL;
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake' has no 'open' function");
}

static void test_each_null_function(void) {
    static const char *const names[FUNCTION_COUNT] = {"open",     "load_aig", "exec",
                                                      "get_luts", "name_map", "close"};
    for (size_t fn = 0; fn < FUNCTION_COUNT; fn++) {
        char want[LINE_CAP];
        int len =
            snprintf(want, sizeof want,
                     "odin3_abc_engine_register: engine 'fake' has no '%s' function", names[fn]);
        TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
        odin3_abc_engine engine = without_function(fn);
        expect_refused(&engine, want);
    }
}

static void test_reserved_set(void) {
    static int marker;
    odin3_abc_engine engine = valid_engine("fake");
    engine.reserved[2] = &marker;
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake': reserved[2] must be NULL");
}

static void test_each_reserved_set(void) {
    static int marker;
    for (size_t slot = 0; slot < RESERVED_COUNT; slot++) {
        char want[LINE_CAP];
        int len =
            snprintf(want, sizeof want,
                     "odin3_abc_engine_register: engine 'fake': reserved[%zu] must be NULL", slot);
        TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
        odin3_abc_engine engine = valid_engine("fake");
        engine.reserved[slot] = &marker;
        expect_refused(&engine, want);
    }
}

/* Several faults at once: functions, then reserved, then abc_commit, one line each time. */
static void test_group_order(void) {
    static int marker;
    odin3_abc_engine engine = valid_engine("fake");
    engine.get_luts = NULL;
    engine.reserved[0] = &marker;
    engine.abc_commit = "BAD";
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake' has no 'get_luts' function");
    engine.get_luts = fake_get_luts;
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake': reserved[0] must be NULL");
    engine.reserved[0] = NULL;
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake': abc_commit 'BAD' is not 40 "
                            "lower-case hex digits");
    /* The version comes before everything, the name before the functions. */
    engine.version = 2;
    engine.exec = NULL;
    expect_refused(&engine, "odin3_abc_engine_register: engine version 2, expected 1");
    engine.version = ODIN3_ABC_ENGINE_VERSION;
    engine.name = NULL;
    expect_refused(&engine, "odin3_abc_engine_register: invalid argument engine");
}

static void expect_bad_commit(const char *commit) {
    char want[LINE_CAP];
    int len = snprintf(want, sizeof want,
                       "odin3_abc_engine_register: engine 'fake': abc_commit '%s' is not 40 "
                       "lower-case hex digits",
                       commit);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    odin3_abc_engine engine = valid_engine("fake");
    engine.abc_commit = commit;
    expect_refused(&engine, want);
}

static void test_bad_commit(void) {
    expect_bad_commit("A3001B72EDC5DE22442E942165487FDDF150E3D0");  /* upper case */
    expect_bad_commit("a3001b72edc5de22442e942165487fddf150e3d");   /* 39 digits */
    expect_bad_commit("a3001b72edc5de22442e942165487fddf150e3d00"); /* 41 digits */
    expect_bad_commit("g3001b72edc5de22442e942165487fddf150e3d0");  /* not hex */
    odin3_abc_engine engine = valid_engine("fake");
    engine.abc_commit = NULL; /* agent default: shown as '' */
    expect_refused(&engine, "odin3_abc_engine_register: engine 'fake': abc_commit '' is not 40 "
                            "lower-case hex digits");
}

static void test_register_then_final(void) {
    odin3_abc_engine engine = valid_engine("fake");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_engine_register(&engine));
    TEST_ASSERT_EQUAL_size_t(0, line_count);
    engine.name = "changed"; /* the registry holds a copy */
    const odin3_abc_engine *got = odin3_abc_engine_get();
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING("fake", got->name);
    TEST_ASSERT_EQUAL_STRING(COMMIT, got->abc_commit);
    TEST_ASSERT_TRUE(got->exec == fake_exec);

    odin3_abc_engine second = valid_engine("second");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_engine_register(&second));
    TEST_ASSERT_EQUAL_size_t(1, line_count);
    TEST_ASSERT_EQUAL_STRING("odin3_abc_engine_register: abc engine already registered (fake)",
                             last_line);
    TEST_ASSERT_EQUAL_STRING("fake", odin3_abc_engine_get()->name);
}

static void test_override(void) {
    odin3_abc_engine dbl = valid_engine("double");
    odin3_abc_test_set_engine(&dbl);
    dbl.name = "changed"; /* the override is a copy too */
    TEST_ASSERT_EQUAL_STRING("double", odin3_abc_engine_get()->name);
    odin3_abc_test_set_engine(NULL);
    TEST_ASSERT_EQUAL_STRING("fake", odin3_abc_engine_get()->name);
    TEST_ASSERT_EQUAL_size_t(0, line_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_null_engine);
    RUN_TEST(test_other_version);
    RUN_TEST(test_null_name);
    RUN_TEST(test_null_exec);
    RUN_TEST(test_each_null_function);
    RUN_TEST(test_reserved_set);
    RUN_TEST(test_each_reserved_set);
    RUN_TEST(test_bad_commit);
    RUN_TEST(test_group_order);
    RUN_TEST(test_register_then_final);
    RUN_TEST(test_override);
    return UNITY_END();
}
