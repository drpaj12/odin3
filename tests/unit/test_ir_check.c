/*
 * test_ir_check.c — unit tests for check (IR §9): a valid module passes, each corruption hook trips
 * its rule, FAST runs rules 1–5 and 11 only, bus and view semantics, the log cap, OOM.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_test.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_CAP = 1 << 16,
    NEEDLE_BUF = 64,
    MANY = 50,
    UNDRIVEN = 12, /* more rule-4 warnings than the per-severity display cap (10) */
    OOM_LIMIT = 100,
};

static odin3_design *design;
static odin3_module *module;
static odin3_prov_id prov;
static odin3_pass_ctx run_ctx; /* the test's pass run (setUp) */
static char log_text[LOG_CAP];
static size_t log_len;
static size_t errors_logged;
static size_t warnings_logged;

/* The fixture's objects (built by setUp through the API). */
static odin3_net_id net_a, net_b, net_y, mid, t1;
static odin3_node_id g_and, g_not, g_buf;
static odin3_wire_id wire_w;

static const char *level_tag(odin3_log_level level) {
    switch (level) {
    case ODIN3_LOG_ERROR:
        return "E ";
    case ODIN3_LOG_WARN:
        return "W ";
    default:
        return "I ";
    }
}

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR ? 1 : 0;
    warnings_logged += level == ODIN3_LOG_WARN ? 1 : 0;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%s%s\n", level_tag(level), msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static void reset_log(void) {
    log_text[0] = '\0';
    log_len = 0;
    errors_logged = 0;
    warnings_logged = 0;
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

/* True when the log holds "<sev> check: <mod>: rule <rule>:". sev is "E" or "W". */
static bool logged_rule(const char *sev, const char *mod, int rule) {
    char needle[NEEDLE_BUF];
    (void)snprintf(needle, sizeof needle, "%s check: %s: rule %d:", sev, mod, rule);
    return log_has(needle);
}

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

static odin3_check_opts opts(odin3_check_level level, odin3_view view) {
    return (odin3_check_opts){level, view};
}

static odin3_status full(odin3_module *mod) {
    return odin3_check_module(mod, opts(ODIN3_CHECK_FULL, ODIN3_VIEW_NONE));
}

static odin3_status fast(odin3_module *mod) {
    return odin3_check_module(mod, opts(ODIN3_CHECK_FAST, ODIN3_VIEW_NONE));
}

static odin3_module *new_module(const char *name) {
    odin3_module_id mid_id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &mid_id));
    return odin3_module_get(design, mid_id);
}

static odin3_net_id add_port(odin3_module *mod, const char *name, odin3_dir dir) {
    odin3_port_spec spec = {intern(name), dir, 1, true, prov};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    uint32_t index = odin3_module_port_count(mod) - 1;
    return odin3_wire_net(mod, odin3_module_port_wire(mod, index), 0);
}

static odin3_net_id new_net(odin3_module *mod, const char *name) {
    odin3_net_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_net_create(mod, name != NULL ? intern(name) : 0, prov, &id));
    return id;
}

/* A node of a parameterless (or default-parameter) type; nets one per pin, none = open. */
static odin3_node_id gate(odin3_module *mod, const char *type, const char *name,
                          const odin3_net_id *nets) {
    odin3_node_spec spec = {type_id(type), name != NULL ? intern(name) : 0, prov, NULL, 0};
    odin3_node_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, &id));
    odin3_pinslice pins = odin3_node_pins(mod, id);
    for (uint32_t k = 0; k < pins.count; k++) {
        if (odin3_net_valid(nets[k])) {
            TEST_ASSERT_EQUAL_INT(
                ODIN3_OK, odin3_pin_connect(mod, (odin3_pin_id){pins.first.v + k}, nets[k]));
        }
    }
    return id;
}

static odin3_pin_id pin_of(odin3_node_id node, uint32_t port) {
    return odin3_node_port(module, node, port).first;
}

/*
 * top: ports a, b (in) and y (out); g_and = a & b -> mid; g_not = ~mid -> y; g_buf = mid -> t1;
 * wire w = {t1, mid}; net t2 merged into t1 (its name becomes an alias of t1).
 */
static void build_top(void) {
    module = new_module("top");
    net_a = add_port(module, "a", ODIN3_DIR_IN);
    net_b = add_port(module, "b", ODIN3_DIR_IN);
    net_y = add_port(module, "y", ODIN3_DIR_OUT);
    mid = new_net(module, "mid");
    t1 = new_net(module, "t1");
    odin3_net_id t2 = new_net(module, "t2");
    g_and = gate(module, "$_AND_", "g_and", (odin3_net_id[]){net_a, net_b, mid});
    g_not = gate(module, "$_NOT_", "g_not", (odin3_net_id[]){mid, net_y});
    g_buf = gate(module, "$_BUF_", "g_buf", (odin3_net_id[]){mid, t1});
    odin3_wire_spec wspec = {intern("w"), 1, 0, false, prov};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_wire_create(module, &wspec, (odin3_net_id[]){mid, t1}, &wire_w));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){t1, t2}));
}

void setUp(void) {
    reset_log();
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    run_ctx = (odin3_pass_ctx){0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("test"), &run_ctx));
    odin3_srcloc loc = {intern("t.v"), 1, 1, 1, 2};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run_ctx, &origin, &prov));
    build_top();
    reset_log();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    module = NULL;
}

static void corrupt(odin3_ir_test_fault fault, uint32_t id) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ir_test_corrupt(module, (odin3_ir_test_target){fault, id}));
}

/* Applies the fault; FULL must fail with the rule; FAST fails with it iff fast_rule. */
static void expect_fault(odin3_ir_test_target target, int rule, bool fast_rule) {
    corrupt(target.fault, target.id);
    reset_log();
    TEST_ASSERT_EQUAL_INT(fast_rule ? ODIN3_ERR_CHECK : ODIN3_OK, fast(module));
    TEST_ASSERT_EQUAL_MESSAGE(fast_rule, logged_rule("E", "top", rule), log_text);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, full(module));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "top", rule), log_text);
}

/* --- valid IR ------------------------------------------------------------------------------ */

static void test_valid_module_passes_full_and_fast(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fast(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts(ODIN3_CHECK_FULL, 0)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, warnings_logged, log_text);
}

static void test_merged_alias_name_is_accepted(void) {
    TEST_ASSERT_EQUAL_UINT32(t1.v, odin3_module_find_net(module, intern("t2")).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_FALSE_MESSAGE(log_has("rule 6"), log_text);
}

static void test_netlist_view_passes(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_check_module(module, opts(ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
}

static void test_invalid_options_logged_once(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_check_module(module, opts(2, 0)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, errors_logged, log_text);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_check_design(design, opts(0, 3)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, errors_logged, log_text);
}

static void test_null_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_check_module(NULL, opts(0, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_check_design(NULL, opts(0, 0)));
}

/* --- one fault per rule ---------------------------------------------------------------------- */

static void test_fault_pin_bad_port(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PIN_BAD_PORT, pin_of(g_and, 0).v}, 1, true);
}

static void test_fault_pin_not_in_net(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PIN_NOT_IN_NET, pin_of(g_not, 0).v}, 2, true);
}

static void test_fault_net_extra_pin(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_NET_EXTRA_PIN, pin_of(g_buf, 0).v}, 2, true);
}

static void test_fault_partition(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PARTITION, mid.v}, 3, true);
}

static void test_fault_driver_count(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_DRIVER_COUNT, mid.v}, 3, true);
}

static void test_fault_multi_driver(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_MULTI_DRIVER, mid.v}, 4, true);
}

static void test_fault_pin_count(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PIN_COUNT, g_and.v}, 5, true);
}

static void test_fault_dup_name_passes_fast(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_DUP_NAME, g_not.v}, 6, false);
}

static void test_fault_bad_prov(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_BAD_PROV, g_and.v}, 7, false);
}

static void test_fault_wire_backref(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_WIRE_BACKREF, wire_w.v}, 8, false);
}

static void test_fault_port_list(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PORT_LIST, 1}, 9, false);
}

/* Rule 9: port pin k and port wire bit k must hold the same net (a and y, both directions). */
static void test_fault_port_wire_mismatch(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PORT_WIRE_MISMATCH, 0}, 9, false);
    TEST_ASSERT_TRUE_MESSAGE(log_has("port 0 (a)"), log_text);
}

static void test_fault_port_wire_mismatch_out(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_PORT_WIRE_MISMATCH, 2}, 9, false);
    TEST_ASSERT_TRUE_MESSAGE(log_has("port 2 (y)"), log_text);
}

static void test_fault_dead_node_live_pin(void) {
    expect_fault((odin3_ir_test_target){ODIN3_IR_TEST_DEAD_NODE_LIVE_PIN, g_buf.v}, 11, true);
}

/* A word-level module: $not a -> y, a $_CONST0_ on its own net (legal in every view). */
static void test_fault_view(void) {
    odin3_module *word = new_module("word");
    odin3_net_id in = add_port(word, "a", ODIN3_DIR_IN);
    odin3_net_id out = add_port(word, "y", ODIN3_DIR_OUT);
    odin3_node_id inv = gate(word, "$not", "inv", (odin3_net_id[]){in, out});
    (void)gate(word, "$_CONST0_", "zero", (odin3_net_id[]){new_net(word, "k")});
    odin3_check_opts rtlil = opts(ODIN3_CHECK_FULL, ODIN3_VIEW_RTLIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(word, rtlil));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged + warnings_logged, log_text);

    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ir_test_corrupt(word, (odin3_ir_test_target){ODIN3_IR_TEST_VIEW, inv.v}));
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(word, opts(ODIN3_CHECK_FAST, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_module(word, opts(ODIN3_CHECK_FULL, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_check_module(word, rtlil));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "word", 10), log_text);
    TEST_ASSERT_FALSE_MESSAGE(log_has("zero"), log_text); /* the constant is exempt */
}

static void test_corrupt_rejects_bad_targets(void) {
    odin3_ir_test_target bad[] = {
        {ODIN3_IR_TEST_PIN_BAD_PORT, 9999},
        {ODIN3_IR_TEST_PARTITION, t1.v}, /* a driver, no sink */
        {ODIN3_IR_TEST_PORT_LIST, 3},
        {ODIN3_IR_TEST_VIEW, g_and.v},
        {ODIN3_IR_TEST_WIRE_BACKREF, 9999},
        {ODIN3_IR_TEST_FAULT_COUNT, 1},
        {ODIN3_IR_TEST_PORT_WIRE_MISMATCH, 3},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ir_test_corrupt(module, bad[i]));
    }
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
}

/* --- rule 4: drivers and buses --------------------------------------------------------------- */

static void test_undriven_and_empty_nets_warn_only(void) {
    odin3_net_id undriven = new_net(module, "u");
    (void)gate(module, "$_NOT_", "reader", (odin3_net_id[]){undriven, new_net(module, "u_out")});
    (void)new_net(module, "empty");
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("W", "top", 4), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("(u)"), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("(empty)"), log_text);
}

static void add_tribuf(odin3_net_id bus) {
    (void)gate(module, "$tribuf", NULL, (odin3_net_id[]){net_a, net_b, bus});
}

static void test_two_tribufs_are_a_bus(void) {
    odin3_net_id bus = new_net(module, "bus");
    add_tribuf(bus);
    add_tribuf(bus);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged + warnings_logged, log_text);
}

static void test_bus_with_one_ordinary_driver_warns(void) {
    odin3_net_id bus = new_net(module, "bus");
    add_tribuf(bus);
    add_tribuf(bus);
    (void)gate(module, "$_CONST0_", NULL, (odin3_net_id[]){bus});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("W", "top", 4), log_text);
}

static void test_two_constants_on_one_net_fail(void) {
    odin3_net_id clash = new_net(module, "clash");
    (void)gate(module, "$_CONST0_", NULL, (odin3_net_id[]){clash});
    (void)gate(module, "$_CONST1_", NULL, (odin3_net_id[]){clash});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, fast(module));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "top", 4), log_text);
}

/* --- rules 7, 10, 11 through the API ---------------------------------------------------------- */

static void test_rtlil_view_rejects_bit_gates(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK,
                          odin3_check_module(module, opts(ODIN3_CHECK_FULL, ODIN3_VIEW_RTLIL)));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "top", 10), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("$_AND_"), log_text);
}

static void test_view_is_not_a_fast_rule(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_check_module(module, opts(ODIN3_CHECK_FAST, ODIN3_VIEW_RTLIL)));
}

static void test_unconnected_port_out_pin_fails(void) {
    odin3_node_id port_y = odin3_module_port(module, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pin_disconnect(module, odin3_node_pins(module, port_y).first));
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, fast(module));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "top", 11), log_text);
}

static void test_dangling_pin_warns(void) {
    (void)gate(module, "$_NOT_", "loose", (odin3_net_id[]){{0}, {0}});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fast(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("W", "top", 11), log_text);
}

static void test_prov_zero_is_one_warning(void) {
    odin3_node_spec spec = {type_id("$_NOT_"), 0, {0}, NULL, 0};
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, NULL));
    }
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("W", "top", 7), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("3 live objects"), log_text);
}

/* --- log cap, design check, OOM --------------------------------------------------------------- */

static void test_many_violations_are_summed(void) {
    for (int i = 0; i < MANY; i++) {
        (void)gate(module, "$_BUF_", NULL, (odin3_net_id[]){{0}, {0}});
    }
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fast(module));
    TEST_ASSERT_TRUE_MESSAGE(log_has("W check: top: rule 11: 90 more warnings not shown"),
                             log_text);
    TEST_ASSERT_TRUE(warnings_logged < MANY);
}

static void test_design_check_names_the_bad_module(void) {
    odin3_module *other = new_module("other");
    odin3_net_id clash = new_net(other, NULL);
    (void)gate(other, "$_CONST0_", NULL, (odin3_net_id[]){clash});
    (void)gate(other, "$_CONST1_", NULL, (odin3_net_id[]){clash});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_check_design(design, opts(ODIN3_CHECK_FULL, 0)));
    TEST_ASSERT_TRUE_MESSAGE(logged_rule("E", "other", 4), log_text);
    TEST_ASSERT_FALSE_MESSAGE(log_has("check: top:"), log_text);
}

/* FULL with allocation fail_at failing; a failed run logs no violation. */
static odin3_status full_failing_at(int fail_at) {
    reset_log();
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = full(module);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || !log_has("rule"), log_text);
    return st;
}

static void test_full_check_out_of_memory(void) {
    int fail_at = 0;
    while (fail_at < OOM_LIMIT && full_failing_at(fail_at) == ODIN3_ERR_NO_MEMORY) {
        fail_at++;
    }
    TEST_ASSERT_TRUE(fail_at > 0); /* FULL allocates */
    TEST_ASSERT_TRUE(fail_at < OOM_LIMIT);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fast(module)); /* FAST allocates nothing */
}

/* Warnings past the display cap must not hide an error of the same rule. */
static void test_warnings_never_hide_an_error(void) {
    for (int i = 0; i < UNDRIVEN; i++) {
        (void)gate(module, "$_NOT_", NULL,
                   (odin3_net_id[]){new_net(module, NULL), new_net(module, NULL)});
    }
    odin3_net_id clash = new_net(module, "clash");
    (void)gate(module, "$_CONST0_", NULL, (odin3_net_id[]){clash});
    (void)gate(module, "$_CONST1_", NULL, (odin3_net_id[]){clash});
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, fast(module));
    TEST_ASSERT_TRUE_MESSAGE(log_has("W check: top: rule 4: 2 more warnings not shown"), log_text);
    TEST_ASSERT_TRUE_MESSAGE(log_has("(clash) has 2 drivers"), log_text);
}

/* --- valid after every API path -------------------------------------------------------------- */

/* A sub-module with ports a (in) and y (out), y = ~a; instanced in top. */
static void add_instance(void) {
    odin3_module *leaf = new_module("leaf");
    odin3_net_id in = add_port(leaf, "a", ODIN3_DIR_IN);
    odin3_net_id out = add_port(leaf, "y", ODIN3_DIR_OUT);
    (void)gate(leaf, "$_NOT_", "inv", (odin3_net_id[]){in, out});
    odin3_net_id inst_out = new_net(module, "inst_out");
    odin3_netvec ports[] = {{&net_a, 1}, {&inst_out, 1}};
    odin3_node_spec spec = {odin3_module_celltype(leaf), intern("u1"), prov, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, NULL));
    (void)gate(module, "$_BUF_", "sink_u1", (odin3_net_id[]){inst_out, new_net(module, NULL)});
}

/* Merge of named nets that carry wire primaries and aliases. */
static void merge_rich_nets(void) {
    odin3_net_id keep = new_net(module, "keep");
    odin3_net_id drop = new_net(module, "drop");
    odin3_wire_id wk = {0};
    odin3_wire_id wd = {0};
    odin3_wire_spec ks = {intern("wk"), 0, 0, false, prov};
    odin3_wire_spec ds = {intern("wd"), 1, 0, false, prov};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &ks, &keep, &wk));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_wire_create(module, &ds, (odin3_net_id[]){drop, drop}, &wd));
    (void)gate(module, "$_BUF_", "drv_drop", (odin3_net_id[]){net_a, drop});
    (void)gate(module, "$_NOT_", "use_keep", (odin3_net_id[]){keep, new_net(module, NULL)});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){keep, drop}));
    TEST_ASSERT_EQUAL_UINT32(keep.v, odin3_module_find_net(module, intern("drop")).v);
}

/* wire_add_alias: a move to a net with a primary (alias), then back to a net without one. */
static void move_wire_bits(void) {
    odin3_wire_id w2 = {0};
    odin3_wire_spec spec = {intern("w2"), 1, 0, false, prov};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &w2));
    odin3_net_id old0 = odin3_wire_net(module, w2, 0);
    odin3_net_id old1 = odin3_wire_net(module, w2, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){w2, 0}, mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, old0)); /* no pins, no membership */
    odin3_net_id fresh = new_net(module, "fresh");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){w2, 0}, fresh));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_add_alias(module, (odin3_wirebit){w2, 1}, t1));
    (void)gate(module, "$_BUF_", "drv_fresh", (odin3_net_id[]){net_b, fresh});
    (void)gate(module, "$_BUF_", "drv_old1", (odin3_net_id[]){net_b, old1});
}

/* node_replace with DERIVED provenance; delete then reuse a name; net delete and re-create. */
static void replace_and_reuse(void) {
    odin3_prov_begin_op(&run_ctx);
    odin3_prov_id derived = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_prov_derive(&run_ctx, (odin3_prov_list){&prov, 1}, &derived));
    odin3_node_spec spec = {type_id("$_BUF_"), intern("g_buf2"), derived, NULL, 0};
    odin3_node_id buf2 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &buf2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_replace(module, (odin3_node_pair){g_buf, buf2}));
    odin3_net_id tmp_out = new_net(module, NULL);
    odin3_node_id tmp = gate(module, "$_NOT_", "tmp", (odin3_net_id[]){mid, tmp_out});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, tmp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, tmp_out)); /* pinless now */
    (void)gate(module, "$_NOT_", "tmp", (odin3_net_id[]){mid, new_net(module, NULL)});
    (void)gate(module, "$_NOT_", "g_buf",
               (odin3_net_id[]){mid, new_net(module, NULL)}); /* the replaced name */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_delete(module, new_net(module, "gone")));
    (void)gate(module, "$_BUF_", "use_gone", (odin3_net_id[]){mid, new_net(module, "gone")});
}

/* An inout port driven by a $tribuf: a bus of the port pin and the tristate output. */
static void inout_bus(void) {
    odin3_net_id io = add_port(module, "io", ODIN3_DIR_INOUT);
    odin3_net_id ports_nets[] = {net_a, net_b, io};
    odin3_netvec ports[] = {{&ports_nets[0], 1}, {&ports_nets[1], 1}, {&ports_nets[2], 1}};
    odin3_node_spec spec = {type_id("$tribuf"), intern("tb"), prov, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, NULL));
}

static void test_valid_after_every_api_path(void) {
    inout_bus(); /* ports first: instancing top's type would refuse add_port */
    add_instance();
    merge_rich_nets();
    move_wire_bits();
    replace_and_reuse();
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts(ODIN3_CHECK_FULL, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, full(module));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, errors_logged, log_text);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, warnings_logged, log_text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_module_passes_full_and_fast);
    RUN_TEST(test_merged_alias_name_is_accepted);
    RUN_TEST(test_netlist_view_passes);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_fault_pin_bad_port);
    RUN_TEST(test_fault_pin_not_in_net);
    RUN_TEST(test_fault_net_extra_pin);
    RUN_TEST(test_fault_partition);
    RUN_TEST(test_fault_driver_count);
    RUN_TEST(test_fault_multi_driver);
    RUN_TEST(test_fault_pin_count);
    RUN_TEST(test_fault_dup_name_passes_fast);
    RUN_TEST(test_fault_bad_prov);
    RUN_TEST(test_fault_wire_backref);
    RUN_TEST(test_fault_port_list);
    RUN_TEST(test_fault_port_wire_mismatch);
    RUN_TEST(test_fault_port_wire_mismatch_out);
    RUN_TEST(test_fault_dead_node_live_pin);
    RUN_TEST(test_fault_view);
    RUN_TEST(test_corrupt_rejects_bad_targets);
    RUN_TEST(test_undriven_and_empty_nets_warn_only);
    RUN_TEST(test_two_tribufs_are_a_bus);
    RUN_TEST(test_bus_with_one_ordinary_driver_warns);
    RUN_TEST(test_two_constants_on_one_net_fail);
    RUN_TEST(test_rtlil_view_rejects_bit_gates);
    RUN_TEST(test_view_is_not_a_fast_rule);
    RUN_TEST(test_unconnected_port_out_pin_fails);
    RUN_TEST(test_dangling_pin_warns);
    RUN_TEST(test_prov_zero_is_one_warning);
    RUN_TEST(test_many_violations_are_summed);
    RUN_TEST(test_design_check_names_the_bad_module);
    RUN_TEST(test_full_check_out_of_memory);
    RUN_TEST(test_invalid_options_logged_once);
    RUN_TEST(test_warnings_never_hide_an_error);
    RUN_TEST(test_valid_after_every_api_path);
    return UNITY_END();
}
