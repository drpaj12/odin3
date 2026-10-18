/*
 * test_lower_corpus.c — 3B Task 9 (spec 12.1, 12.2): out-of-memory at every allocation for each
 * rule family (the shifts, $pmux, the arithmetic and comparisons, $mul, $div and $mod, the bitwise
 * cells: the Task 4 and 5 builders that the Task 1 kit sweep does not run), and a module built in
 * reverse creation order. The reverse test writes lower_reverse_a.blif and lower_reverse_b.blif to
 * the working directory; CTest lower_reverse_netlist_compare compares them.
 */
#include "backends/blif/writer.h"
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MAX_STEPS = 40000, SMALL = 3, WIDE = 4 };

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_lower_test_set_limits(NULL);
}

/* One cell type with its widths: binary (5 parameters), unary (3) or $pmux (2). */
typedef struct oom_case {
    const char *type;
    enum { OOM_BINARY, OOM_UNARY, OOM_PMUX } shape;
    bool is_signed;
} oom_case;

static odin3_node_id build_case(lf *fix, const oom_case *cs, odin3_module **mod) {
    lf_binary spec = {cs->is_signed, cs->is_signed, WIDE, SMALL, WIDE};
    odin3_value params[5];
    lf_cell_spec cell = {cs->type, params, 5, "dut", {0}};
    if (cs->shape == OOM_UNARY) {
        lf_unary_params(&spec, params);
        cell.n_params = 3;
    } else if (cs->shape == OOM_PMUX) {
        params[0] = odin3_value_int(SMALL);
        params[1] = odin3_value_int(SMALL);
        cell.n_params = 2;
    } else {
        lf_binary_params(&spec, params);
    }
    return lf_one_cell(fix, &cell, mod);
}

/* One run with the `step`-th allocation failing: true when `lower` succeeded. Every run must leave
 * a design that passes check FULL, and a failure before the commit must leave it as it was. */
static bool oom_run(const oom_case *cs, long step) {
    lf fix;
    lf_begin(&fix);
    odin3_module *mod = NULL;
    odin3_node_id node = build_case(&fix, cs, &mod);
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_strbuf_init(&before);
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    odin3_util_set_alloc_fail_after(step);
    odin3_status st = odin3_pass_run(fix.design, "lower", odin3_bytes_cstr(""));
    odin3_util_set_alloc_fail_after(-1);
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_module(mod, opts), cs->type);
    bool done = st == ODIN3_OK;
    if (!done) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_NO_MEMORY, st, cs->type);
    }
    if (!done && odin3_node_live(mod, node)) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(before.data, after.data, cs->type);
    }
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    lf_end(&fix);
    return done;
}

/* Fails the n-th allocation of `lower` for n = 0, 1, ... until a run succeeds (spec 12.2). */
static void sweep_case(const oom_case *cs) {
    long step = 0;
    while (step < MAX_STEPS && !oom_run(cs, step)) {
        step++;
    }
    TEST_ASSERT_TRUE_MESSAGE(step < MAX_STEPS, cs->type);
    TEST_ASSERT_TRUE_MESSAGE(step > 3, "the sweep injected no failure");
}

static void test_oom_at_every_allocation_shifts_and_pmux(void) {
    static const oom_case k_cases[] = {{"$shl", OOM_BINARY, false},
                                       {"$shr", OOM_BINARY, false},
                                       {"$sshr", OOM_BINARY, true},
                                       {"$shiftx", OOM_BINARY, false},
                                       {"$pmux", OOM_PMUX, false}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        sweep_case(&k_cases[i]);
    }
}

static void test_oom_at_every_allocation_arithmetic_and_comparisons(void) {
    static const oom_case k_cases[] = {{"$add", OOM_BINARY, false}, {"$sub", OOM_BINARY, true},
                                       {"$neg", OOM_UNARY, true},   {"$lt", OOM_BINARY, true},
                                       {"$ge", OOM_BINARY, false},  {"$eq", OOM_BINARY, false}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        sweep_case(&k_cases[i]);
    }
}

static void test_oom_at_every_allocation_mul_div_mod(void) {
    static const oom_case k_cases[] = {{"$mul", OOM_BINARY, false},
                                       {"$mul", OOM_BINARY, true},
                                       {"$div", OOM_BINARY, false},
                                       {"$mod", OOM_BINARY, true}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        sweep_case(&k_cases[i]);
    }
}

static void test_oom_at_every_allocation_bitwise(void) {
    static const oom_case k_cases[] = {{"$and", OOM_BINARY, false},
                                       {"$xor", OOM_BINARY, true},
                                       {"$not", OOM_UNARY, false},
                                       {"$reduce_or", OOM_UNARY, false}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        sweep_case(&k_cases[i]);
    }
}

/* --- reverse creation order ----------------------------------------------------------------- */

static const char *const k_rev_types[] = {"$add", "$and", "$lt", "$shl"};
enum { REV_CELLS = 4 };

/* A module holding the four cells, created in forward or reverse order; each cell's ports are
 * prefixed by its type so the two orders name the same wires. */
static void build_four(lf *fix, bool reverse) {
    odin3_module *mod = lf_module(fix, "m");
    lf_binary spec = {false, false, WIDE, WIDE, WIDE};
    odin3_value params[5];
    lf_binary_params(&spec, params);
    for (uint32_t i = 0; i < REV_CELLS; i++) {
        uint32_t k = reverse ? REV_CELLS - 1U - i : i;
        lf_cell_spec cell = {k_rev_types[k], params, 5, k_rev_types[k], {0}};
        char prefix[16];
        TEST_ASSERT_TRUE(strlen(k_rev_types[k]) < sizeof prefix - 2U);
        memcpy(prefix, k_rev_types[k] + 1, strlen(k_rev_types[k]));
        strcat(prefix, "_");
        (void)lf_cell_prefixed(fix, mod, &cell, prefix);
    }
}

static void lower_and_write(bool reverse, const char *path) {
    lf fix;
    lf_begin(&fix);
    build_four(&fix, reverse);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(fix.design, "lower", odin3_bytes_cstr("")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(fix.design, path));
    lf_end(&fix);
}

static void test_a_module_built_in_reverse_order_lowers_to_the_same_netlist(void) {
    lower_and_write(false, "lower_reverse_a.blif");
    lower_and_write(true, "lower_reverse_b.blif");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oom_at_every_allocation_shifts_and_pmux);
    RUN_TEST(test_oom_at_every_allocation_arithmetic_and_comparisons);
    RUN_TEST(test_oom_at_every_allocation_mul_div_mod);
    RUN_TEST(test_oom_at_every_allocation_bitwise);
    RUN_TEST(test_a_module_built_in_reverse_order_lowers_to_the_same_netlist);
    return UNITY_END();
}
