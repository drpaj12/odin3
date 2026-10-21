/*
 * test_lower_gate_ff.c — `lower` on the 68 gate-level flip-flops with a reset or an enable
 * (ir/cells/dffx.h; 5.3 follow-up). Every type is built alone in a module and lowered; the lowered
 * module has no such cell and, for the synchronous types, follows the original cell in the 1E
 * simulator (its simulate hook is the Yosys 0.55 semantics) on seeded random inputs. An
 * asynchronous reset is made synchronous (PHASE3 decision 19): one warning, `--async error`
 * refuses it, and the lowered module differs only in reset timing.
 */
#include "ir/check.h"
#include "lower_fixture.h"
#include "passes/lower/lower_int.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { NAME_LEN = 24, MAX_TYPES = 80, CYCLES = 200, LOG_TEXT = 4096 };

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
}
void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static const char *const k_pol = "PN";

/* Every name of the family (the same nested loops as test_dffx_cells.c). */
static uint32_t family_names(char names[][NAME_LEN]) {
    uint32_t count = 0;
    for (const char *clk = k_pol; *clk != '\0'; clk++) {
        for (const char *en = k_pol; *en != '\0'; en++) {
            (void)snprintf(names[count++], NAME_LEN, "$_DFFE_%c%c_", *clk, *en);
        }
        for (const char *rst = k_pol; *rst != '\0'; rst++) {
            for (const char *val = "01"; *val != '\0'; val++) {
                (void)snprintf(names[count++], NAME_LEN, "$_DFF_%c%c%c_", *clk, *rst, *val);
                (void)snprintf(names[count++], NAME_LEN, "$_SDFF_%c%c%c_", *clk, *rst, *val);
                for (const char *en = k_pol; *en != '\0'; en++) {
                    (void)snprintf(names[count++], NAME_LEN, "$_DFFE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                    (void)snprintf(names[count++], NAME_LEN, "$_SDFFE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                    (void)snprintf(names[count++], NAME_LEN, "$_SDFFCE_%c%c%c%c_", *clk, *rst, *val,
                                   *en);
                }
            }
        }
    }
    return count;
}

static bool is_async(const char *name) {
    /* $_DFF_<c><r><v>_ and $_DFFE_<c><r><v><e>_ (3 or 4 letters); $_DFFE_<c><e>_ has 2. */
    if (strncmp(name, "$_DFFE_", 7) == 0) {
        return strlen(name) == 7 + 4 + 1;
    }
    return strncmp(name, "$_DFF_", 6) == 0;
}

static bool negative_clock(const char *name) {
    const char *at = strchr(name + 2, '_') + 1;
    return at[0] == 'N';
}

/* A one-cell design: INIT given. */
typedef struct built {
    lf fix;
    odin3_module *mod;
} built;

static void build(built *out, const char *type, int64_t init) {
    lf_begin(&out->fix);
    odin3_value param = odin3_value_int(init);
    lf_cell_spec spec = {type, &param, 1, NULL, {0}};
    (void)lf_one_cell(&out->fix, &spec, &out->mod);
}

static odin3_status lower(built *dd, const char *type, const char *extra) {
    char args[64];
    (void)snprintf(args, sizeof args, "%s%s", negative_clock(type) ? "--neg-edge keep " : "",
                   extra);
    return odin3_pass_run(dd->fix.design, "lower", odin3_bytes_cstr(args));
}

static bool read_q(odin3_sim *sim) {
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        if (strcmp(odin3_sim_output_name(sim, port), "Q") == 0) {
            bool val = false;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_output(sim, (odin3_sim_bit){port, 0}, &val));
            return val;
        }
    }
    TEST_FAIL_MESSAGE("no output Q");
    return false;
}

/* The two designs on the same seeded random inputs; the index of the first cycle whose Q differs
 * plus one, 0 when all agree. */
static uint32_t first_difference(built *orig, built *low, uint64_t seed) {
    odin3_sim *sa = NULL;
    odin3_sim *sb = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build(orig->fix.design, odin3_module_id_of(orig->mod), &sa));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build(low->fix.design, odin3_module_id_of(low->mod), &sb));
    odin3_prng pa;
    odin3_prng pb;
    odin3_prng_seed(&pa, seed);
    odin3_prng_seed(&pb, seed);
    uint32_t bad = read_q(sa) == read_q(sb) ? 0 : 1;
    for (uint32_t cyc = 0; bad == 0 && cyc < CYCLES; cyc++) {
        odin3_sim_drive_random(sa, &pa);
        odin3_sim_drive_random(sb, &pb);
        odin3_sim_cycle(sa);
        odin3_sim_cycle(sb);
        bad = read_q(sa) == read_q(sb) ? 0 : cyc + 2;
    }
    odin3_sim_destroy(sa);
    odin3_sim_destroy(sb);
    return bad;
}

static void check_view(odin3_module *mod, const char *type) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_module(mod, opts), type);
}

static void test_every_type_is_lowered(void) {
    static char names[MAX_TYPES][NAME_LEN];
    uint32_t count = family_names(names);
    TEST_ASSERT_EQUAL_UINT32(68, count);
    for (uint32_t i = 0; i < count; i++) {
        for (int64_t init = 0; init <= 1; init++) {
            built orig;
            built low;
            build(&orig, names[i], init);
            build(&low, names[i], init);
            TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, lower(&low, names[i], ""), names[i]);
            check_view(low.mod, names[i]);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lf_count_type(&low.fix, low.mod, names[i]),
                                             names[i]);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                1,
                lf_count_type(&low.fix, low.mod,
                              negative_clock(names[i]) ? "$_DFF_N_" : "$_DFF_P_"),
                names[i]);
            if (!is_async(names[i])) {
                char msg[64];
                uint32_t bad = first_difference(&orig, &low, 1000U + i);
                (void)snprintf(msg, sizeof msg, "%s init %d: first difference at cycle %u",
                               names[i], (int)init, (unsigned)bad);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, bad, msg);
            }
            lf_end(&orig.fix);
            lf_end(&low.fix);
        }
    }
}

static void test_async_is_made_synchronous_with_one_warning(void) {
    built low;
    build(&low, "$_DFF_PN1_", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&low, "$_DFF_PN1_", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "asynchronous reset/set/load made synchronous on 1 "));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&low.fix, low.mod, "$_MUX_"));
    lf_end(&low.fix);
}

static void test_sync_types_warn_nothing(void) {
    built low;
    build(&low, "$_SDFFE_PP1P_", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&low, "$_SDFFE_PP1P_", ""));
    TEST_ASSERT_NULL(strstr(log_text, "asynchronous"));
    TEST_ASSERT_EQUAL_UINT32(2, lf_count_type(&low.fix, low.mod, "$_MUX_"));
    lf_end(&low.fix);
}

static void test_async_error_refuses_the_gate_cell(void) {
    built low;
    build(&low, "$_DFFE_PP0P_", 0);
    odin3_status st = lower(&low, "$_DFFE_PP0P_", "--async error");
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "asynchronous control on '$_DFFE_PP0P_'"));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&low.fix, low.mod, "$_DFFE_PP0P_"));
    lf_end(&low.fix);
}

static void test_neg_edge_default_inverts_the_clock(void) {
    built low;
    build(&low, "$_SDFF_NP0_", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(low.fix.design, "lower", odin3_bytes_cstr("")));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&low.fix, low.mod, "$_NOT_"));
    TEST_ASSERT_EQUAL_UINT32(1, lf_count_type(&low.fix, low.mod, "$_DFF_P_"));
    lf_end(&low.fix);
}

static void test_enable_first_option_changes_async_gate_types(void) {
    const char *type = "$_DFFE_PP1P_";
    built plain;
    built first;
    build(&plain, type, 0);
    build(&first, type, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&plain, type, ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&first, type, "--async-enable-first"));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, first_difference(&plain, &first, 77U));
    lf_end(&plain.fix);
    lf_end(&first.fix);
}

static void test_enable_first_option_leaves_sync_gate_types(void) {
    const char *type = "$_SDFFE_PP1P_";
    built plain;
    built first;
    build(&plain, type, 0);
    build(&first, type, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&plain, type, ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lower(&first, type, "--async-enable-first"));
    TEST_ASSERT_EQUAL_UINT32(0, first_difference(&plain, &first, 78U));
    lf_end(&plain.fix);
    lf_end(&first.fix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_type_is_lowered);
    RUN_TEST(test_async_is_made_synchronous_with_one_warning);
    RUN_TEST(test_sync_types_warn_nothing);
    RUN_TEST(test_async_error_refuses_the_gate_cell);
    RUN_TEST(test_neg_edge_default_inverts_the_clock);
    RUN_TEST(test_enable_first_option_changes_async_gate_types);
    RUN_TEST(test_enable_first_option_leaves_sync_gate_types);
    return UNITY_END();
}
