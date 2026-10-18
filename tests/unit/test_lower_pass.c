/*
 * test_lower_pass.c — 3B Task 1: the pass as a whole (spec §2, §3.1, §10, §11, §13): option
 * parsing, the REFUSE refusal, an empty design, a read_blif netlist left byte for byte, the errors
 * of the types that have no bit form, the caps and the completeness of the rule table against the
 * registry.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/module.h"
#include "lower_fixture.h"
#include "odin3/odin3.h"
#include "passes/lower/lower.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "proc_fixture.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_BLIF_DIR
#error "ODIN3_BLIF_DIR must name tests/golden/blif"
#endif

enum { LOG_TEXT = 8192 };

static lf fix;
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
    lf_begin(&fix);
}

void tearDown(void) {
    odin3_lower_test_set_limits(NULL);
    lf_end(&fix);
    odin3_log_set_sink(NULL, NULL);
}

static odin3_status run_pass(const char *args) {
    return odin3_pass_run(fix.design, "lower", odin3_bytes_cstr(args));
}

/* --- options -------------------------------------------------------------------------------- */

static odin3_status parse(const char *args, odin3_strbuf *report, odin3_lower_opts *opts) {
    return odin3_lower_parse_args(odin3_bytes_cstr(args), report, opts);
}

static void test_options_default_to_invert_sync_and_no_report(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("", &report, &opts));
    TEST_ASSERT_FALSE(opts.keep_neg_edge);
    TEST_ASSERT_FALSE(opts.async_is_error);
    TEST_ASSERT_EQUAL_UINT64(0, opts.max_mem_bits);
    TEST_ASSERT_NULL(opts.report_path);
    odin3_strbuf_free(&report);
}

static void test_every_option_is_parsed(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--neg-edge keep --async error --report out.json "
                                          "--max-mem-bits 4096",
                                          &report, &opts));
    TEST_ASSERT_TRUE(opts.keep_neg_edge);
    TEST_ASSERT_TRUE(opts.async_is_error);
    TEST_ASSERT_EQUAL_UINT64(4096, opts.max_mem_bits);
    TEST_ASSERT_EQUAL_STRING("out.json", opts.report_path);
    odin3_strbuf_free(&report);
}

static void test_the_snapshot_directory_is_parsed_and_bounded(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("", &report, &opts));
    TEST_ASSERT_EQUAL_STRING("", opts.snapshot_dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--lower-snapshot /tmp/snap --report r.json", &report, &opts));
    TEST_ASSERT_EQUAL_STRING("/tmp/snap", opts.snapshot_dir);
    TEST_ASSERT_EQUAL_STRING("r.json", opts.report_path);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--lower-snapshot", &report, &opts));
    char longer[ODIN3_LOWER_SNAPSHOT_PATH + 64];
    memset(longer, 'd', sizeof longer);
    memcpy(longer, "--lower-snapshot ", 17);
    longer[sizeof longer - 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse(longer, &report, &opts));
    odin3_strbuf_free(&report);
}

static void test_the_other_spellings_restore_the_defaults(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--neg-edge invert --async sync", &report, &opts));
    TEST_ASSERT_FALSE(opts.keep_neg_edge);
    TEST_ASSERT_FALSE(opts.async_is_error);
    TEST_ASSERT_NULL(opts.report_path);
    odin3_strbuf_free(&report);
}

static void test_a_quoted_report_path_may_hold_blanks(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("--report \"a b.json\"", &report, &opts));
    TEST_ASSERT_EQUAL_STRING("a b.json", opts.report_path);
    odin3_strbuf_free(&report);
}

static void test_an_unknown_argument_is_invalid_arg_with_the_1d_text(void) {
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("--frobnicate", &report, &opts));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: unknown argument '--frobnicate'"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, parse("stray", &report, &opts));
    odin3_strbuf_free(&report);
}

static void test_bad_option_values_are_invalid_arg(void) {
    static const char *const k_bad[] = {
        "--neg-edge",        "--neg-edge sideways", "--async",          "--async maybe",
        "--report",          "--max-mem-bits",      "--max-mem-bits 0", "--max-mem-bits x1",
        "--max-mem-bits -3", "--max-mem-bits 12ab",
    };
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, parse(k_bad[i], &report, &opts),
                                      k_bad[i]);
    }
    odin3_strbuf_free(&report);
}

/* --- refusal, empty, untouched -------------------------------------------------------------- */

static void test_a_module_with_a_process_is_refused_by_the_manager(void) {
    (void)proc_fixture_design(fix.design);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_pass(""));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "pass 'lower': module 'm' has processes; run proc first"));
}

static void test_an_empty_design_has_nothing_to_lower(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass(""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: nothing to lower"));
}

static void test_a_module_of_bit_cells_is_left_alone(void) {
    odin3_module *mod = lf_module(&fix, "m");
    lf_cell_spec gate = {"$_AND_", NULL, 0, "g", {0}};
    (void)lf_cell(&fix, mod, &gate);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass(""));
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: nothing to lower"));
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

static void read_blif_unchanged(const char *file) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_BLIF_DIR, file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(fix.design, "read_blif", odin3_bytes_cstr(path)), file);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_pass(""), file);
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(before.data, after.data, file);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lower: nothing to lower"));
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

static void test_read_blif_netlists_pass_through_byte_for_byte(void) {
    read_blif_unchanged("hand_body.blif");
}

static void test_read_blif_netlists_with_latches_and_ports_pass_through(void) {
    read_blif_unchanged("ff.odin.blif");
}

static void test_read_blif_netlist_with_hard_cells_passes_through(void) {
    read_blif_unchanged("adder_hard_block.parmys.blif");
}

static void test_read_blif_netlist_dffsre_passes_through(void) {
    read_blif_unchanged("dffsre.parmys.blif");
}

/* --- types with no bit form ----------------------------------------------------------------- */

/* A cell of `type` named `name` (NULL: unnamed) whose lowering must fail with `text`. */
typedef struct error_case {
    const char *type;
    const char *name;
    const char *text;
} error_case;

static void expect_error(const error_case *ec) {
    lf_cell_spec cs = {ec->type, NULL, 0, ec->name, {0}};
    odin3_module *mod = lf_module(&fix, "m");
    (void)lf_cell(&fix, mod, &cs);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &before));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, run_pass(""), ec->type);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, ec->text), log_text);
    odin3_strbuf after;
    odin3_strbuf_init(&after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lf_signature(&fix, &after));
    TEST_ASSERT_EQUAL_STRING(before.data, after.data); /* the design is intact */
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

static void test_pow_has_no_bit_level_form(void) {
    error_case ec = {"$pow", "p",
                     "lower: module 'm': '$pow' 'p' has no bit-level form (run opt first; Yosys "
                     "leaves it unmapped)"};
    expect_error(&ec);
}

static void test_tribuf_has_no_bit_level_cell(void) {
    error_case ec = {"$tribuf", "t", "lower: '$tribuf' 't' has no bit-level cell in the registry"};
    expect_error(&ec);
}

static void test_standalone_memrd_and_memwr_are_errors(void) {
    error_case ec = {"$memrd", "r", "lower: standalone $memrd/$memwr 'r' (use $mem)"};
    expect_error(&ec);
}

static void test_standalone_memwr_is_an_error(void) {
    error_case ec = {"$memwr", "w", "lower: standalone $memrd/$memwr 'w' (use $mem)"};
    expect_error(&ec);
}

static void test_an_unnamed_cell_is_named_by_its_id_in_the_error(void) {
    error_case ec = {"$pow", NULL, "'$pow' '$c"};
    expect_error(&ec);
}

static void test_a_word_type_without_a_rule_names_the_type_and_the_cell(void) {
    odin3_port_def ports[2] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
                               {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL, false}};
    odin3_celltype_def def = {
        "$plugin_word", ODIN3_GRAN_WORD, 0, ports, 2, NULL, 0, NULL, NULL, NULL, NULL};
    odin3_celltype_id tid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(fix.design, &def, &tid));
    error_case ec = {"$plugin_word", "pw",
                     "lower: no lowering rule for cell type '$plugin_word' ('pw')"};
    expect_error(&ec);
}

/* --- --report ------------------------------------------------------------------------------- */

static size_t read_text(const char *path, char *buf, size_t size) {
    FILE *file = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(buf, 1, size - 1, file);
    (void)fclose(file);
    buf[got] = '\0';
    return got;
}

static void test_report_file_is_written_with_the_four_lists(void) {
    (void)lf_module(&fix, "m");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_pass("--report test_lower_pass.report.json"));
    char text[512];
    TEST_ASSERT_TRUE(read_text("test_lower_pass.report.json", text, sizeof text) > 0);
    static const char *const k_lists[] = {"\"async_to_sync\": []", "\"neg_edge\": []",
                                          "\"mixed_sign\": []", "\"mem\": []"};
    for (size_t i = 0; i < sizeof k_lists / sizeof k_lists[0]; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, k_lists[i]), k_lists[i]);
    }
}

/* --- the rule table against the registry ---------------------------------------------------- */

/* Exactly one of: a rule, listed as having none. */
static void check_one_home(const odin3_celltype_def *def) {
    bool has_rule = lw_rule_find(def->name) != NULL;
    bool listed = lw_no_rule_listed(def->name);
    char msg[96];
    (void)snprintf(msg, sizeof msg, "%s: rule %d, no_rule %d", def->name, has_rule ? 1 : 0,
                   listed ? 1 : 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (has_rule ? 1 : 0) + (listed ? 1 : 0), msg);
}

/* Checks one registry entry; returns whether it is a word type. */
static bool check_registry_entry(const odin3_celltype_def *def) {
    if (def->gran != ODIN3_GRAN_WORD) {
        TEST_ASSERT_NULL_MESSAGE(lw_rule_find(def->name), def->name);
        return false;
    }
    check_one_home(def);
    return true;
}

static void test_dispatch_covers_every_word_type_of_the_registry(void) {
    uint32_t words = 0;
    const odin3_celltype_def *def = NULL;
    for (uint32_t id = 1; (def = odin3_celltype_get(fix.design, (odin3_celltype_id){id})) != NULL;
         id++) {
        words += check_registry_entry(def) ? 1U : 0U;
    }
    TEST_ASSERT_TRUE(words > 40);
}

static void check_is_word_type(const char *name) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(fix.design, lf_str(&fix, name), &tid), name);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_WORD, odin3_celltype_get(fix.design, tid)->gran);
}

static void check_rule_entry(const lw_rule *rule) {
    check_is_word_type(rule->type_name);
    TEST_ASSERT_NOT_NULL(rule->build);
    TEST_ASSERT_NOT_NULL(rule->estimate);
}

static void test_every_rule_and_listed_name_is_a_word_type(void) {
    for (uint32_t i = 0; i < lw_rule_count(); i++) {
        check_rule_entry(lw_rule_at(i));
    }
    static const char *const k_listed[] = {"$proc", "$tribuf", "$pow", "$memrd", "$memwr"};
    for (size_t i = 0; i < sizeof k_listed / sizeof k_listed[0]; i++) {
        TEST_ASSERT_TRUE(lw_no_rule_listed(k_listed[i]));
        check_is_word_type(k_listed[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_options_default_to_invert_sync_and_no_report);
    RUN_TEST(test_every_option_is_parsed);
    RUN_TEST(test_the_other_spellings_restore_the_defaults);
    RUN_TEST(test_the_snapshot_directory_is_parsed_and_bounded);
    RUN_TEST(test_a_quoted_report_path_may_hold_blanks);
    RUN_TEST(test_an_unknown_argument_is_invalid_arg_with_the_1d_text);
    RUN_TEST(test_bad_option_values_are_invalid_arg);
    RUN_TEST(test_a_module_with_a_process_is_refused_by_the_manager);
    RUN_TEST(test_an_empty_design_has_nothing_to_lower);
    RUN_TEST(test_a_module_of_bit_cells_is_left_alone);
    RUN_TEST(test_read_blif_netlists_pass_through_byte_for_byte);
    RUN_TEST(test_read_blif_netlists_with_latches_and_ports_pass_through);
    RUN_TEST(test_read_blif_netlist_with_hard_cells_passes_through);
    RUN_TEST(test_read_blif_netlist_dffsre_passes_through);
    RUN_TEST(test_pow_has_no_bit_level_form);
    RUN_TEST(test_tribuf_has_no_bit_level_cell);
    RUN_TEST(test_standalone_memrd_and_memwr_are_errors);
    RUN_TEST(test_standalone_memwr_is_an_error);
    RUN_TEST(test_an_unnamed_cell_is_named_by_its_id_in_the_error);
    RUN_TEST(test_a_word_type_without_a_rule_names_the_type_and_the_cell);
    RUN_TEST(test_report_file_is_written_with_the_four_lists);
    RUN_TEST(test_dispatch_covers_every_word_type_of_the_registry);
    RUN_TEST(test_every_rule_and_listed_name_is_a_word_type);
    return UNITY_END();
}
