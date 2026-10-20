/*
 * test_fsm_driver.c — the fsm driver (4.8 plan Task 10; Yosys 0.55 fsm.cc:80-163): the stage
 * order seen in the log, the options, a design that still holds `$proc` cells, determinism of the
 * output and idempotence of a second run.
 */
#include "backends/rtlil/writer.h"
#include "fsm_test.h"
#include "passes/fsm/fsm.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository root"
#endif

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    ft_close();
}

static void read_micro(const char *name) {
    char path[FT_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/fsm/%s.v", ODIN3_SOURCE_DIR, name);
    ft_read_proc_file(path);
}

static void run_fsm(const char *args) {
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("fsm", args), ft_log());
}

/* The index of the first message that starts with prefix, or -1. */
static int first_index(const char *prefix) {
    for (uint32_t i = 0; i < ft.n_msgs; i++) {
        if (strncmp(ft.msgs[i].text, prefix, strlen(prefix)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The design as RTLIL text; the caller frees it. */
static char *rtlil_text(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_rtlil_text(ft.design, &buf));
    char *copy = strdup(buf.data);
    TEST_ASSERT_NOT_NULL(copy);
    odin3_strbuf_free(&buf);
    return copy;
}

/* --- stage order ---------------------------------------------------------------------------- */

static void test_stages_run_in_yosys_order(void) {
    read_micro("moore_arst");
    run_fsm("");
    int found = first_index("fsm: found state register");
    int recode = first_index("fsm: recoding");
    int info = first_index("fsm: information on FSM");
    int mapped = first_index("fsm: mapped");
    TEST_ASSERT_TRUE_MESSAGE(found >= 0 && recode > found, ft_log());
    TEST_ASSERT_TRUE_MESSAGE(info > recode && mapped > info, ft_log());
}

static void test_nomap_stops_before_the_mapping(void) {
    read_micro("moore_arst");
    run_fsm("-nomap");
    TEST_ASSERT_TRUE(first_index("fsm: information on FSM") >= 0);
    TEST_ASSERT_EQUAL_INT(-1, first_index("fsm: mapped"));
}

static void test_norecode_skips_the_recoding(void) {
    read_micro("moore_arst");
    run_fsm("-norecode");
    TEST_ASSERT_EQUAL_INT(-1, first_index("fsm: recoding"));
    TEST_ASSERT_TRUE(first_index("fsm: mapped") >= 0);
}

/* --- options -------------------------------------------------------------------------------- */

static void test_nodetect_uses_only_the_user_attribute(void) {
    read_micro("moore_arst");
    run_fsm("-nodetect");
    TEST_ASSERT_EQUAL_INT(-1, first_index("fsm: found state register"));
    TEST_ASSERT_EQUAL_INT(-1, first_index("fsm: mapped"));
    read_micro("enc_user");
    run_fsm("-nodetect");
    TEST_ASSERT_TRUE_MESSAGE(first_index("fsm: mapped") >= 0, ft_log());
}

static void test_detect_marks_are_reused_when_detect_ran_first(void) {
    read_micro("moore_arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", ""));
    run_fsm("-nodetect");
    TEST_ASSERT_TRUE_MESSAGE(first_index("fsm: mapped") >= 0, ft_log());
}

static void test_bad_options_are_errors(void) {
    read_micro("moore_arst");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-expand"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-fullexpand"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-export"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-encfile x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm", "-nothing"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, ft_pass("fsm_detect", "-nothing"));
}

/* --- a design with $proc cells ---------------------------------------------------------------- */

static void test_a_design_with_proc_cells_is_refused(void) {
    ft_open();
    ft_write_source("module m(input clk, input d, output reg q);\n"
                    "  always @(posedge clk) q <= d;\nendmodule\n");
    char args[FT_PATH + 32];
    (void)snprintf(args, sizeof args, "--nolatches %s", ft.file);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ft_pass("read_verilog", args));
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, ft_pass("fsm", ""));
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, ft_pass("fsm_detect", ""));
}

/* --- determinism and idempotence -------------------------------------------------------------- */

static void test_two_runs_give_identical_rtlil(void) {
    read_micro("two_fsm");
    run_fsm("");
    char *first = rtlil_text();
    read_micro("two_fsm");
    run_fsm("");
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

static void test_a_second_run_finds_nothing_new(void) {
    read_micro("two_fsm");
    run_fsm("");
    char *first = rtlil_text();
    run_fsm("");
    char *second = rtlil_text();
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stages_run_in_yosys_order);
    RUN_TEST(test_nomap_stops_before_the_mapping);
    RUN_TEST(test_norecode_skips_the_recoding);
    RUN_TEST(test_nodetect_uses_only_the_user_attribute);
    RUN_TEST(test_detect_marks_are_reused_when_detect_ran_first);
    RUN_TEST(test_bad_options_are_errors);
    RUN_TEST(test_a_design_with_proc_cells_is_refused);
    RUN_TEST(test_two_runs_give_identical_rtlil);
    RUN_TEST(test_a_second_run_finds_nothing_new);
    return UNITY_END();
}
