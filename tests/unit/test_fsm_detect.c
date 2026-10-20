/*
 * test_fsm_detect.c — fsm_detect (4.8 plan Task 5; spec D-1 to D-7; Yosys 0.55 fsm_detect.cc):
 * the rule tests on the FSM micros (tests/fsm), the mux-tree check on hand-built modules (a
 * loop, a leaf on a port, a driver that is not a mux), and a table-driven run over every micro of
 * tests/fsm/detect_marks.txt, whose expected marks are Yosys 0.55's (tools/fsm-check/fsm-facts).
 */
#include "fsm_test.h"
#include "ir/check.h"
#include "passes/fsm/fsm.h"
#include "proc_build.h"
#include "util/alloc.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository root"
#endif

enum { LINE = 4096, MAX_NAMES = 64 };

void setUp(void) {
    memset(&ft, 0, sizeof ft);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    ft_close();
}

static void read_micro(const char *rel) {
    char path[FT_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/fsm/%s.v", ODIN3_SOURCE_DIR, rel);
    ft_read_proc_file(path);
}

static void detect(const char *args) {
    ft.n_msgs = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, ft_pass("fsm_detect", args), ft_log());
}

static void expect_marked(ft_ref wire) {
    const char *enc = ft_wire_attr(wire, "fsm_encoding");
    TEST_ASSERT_NOT_NULL_MESSAGE(enc, wire.name);
    TEST_ASSERT_EQUAL_STRING("auto", enc);
}

static void expect_unmarked(ft_ref wire) {
    TEST_ASSERT_NULL_MESSAGE(ft_wire_attr(wire, "fsm_encoding"), wire.name);
}

/* --- D-2: skips ------------------------------------------------------------------------------ */

static void test_none_is_skipped_silently(void) {
    read_micro("enc_none");
    detect("");
    TEST_ASSERT_EQUAL_STRING("none", ft_wire_attr((ft_ref){"enc_none", "st"}, "fsm_encoding"));
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_INFO, "fsm:"));
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_WARN, "fsm:"));
}

static void test_one_bit_is_skipped(void) {
    read_micro("one_bit");
    detect("");
    expect_unmarked((ft_ref){"one_bit", "st"});
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_INFO, "fsm:"));
}

static void test_one_bit_attribute_is_removed_with_a_warning(void) {
    read_micro("one_bit_attr");
    TEST_ASSERT_NOT_NULL(ft_wire_attr((ft_ref){"one_bit_attr", "st"}, "fsm_encoding"));
    detect("");
    expect_unmarked((ft_ref){"one_bit_attr", "st"});
    TEST_ASSERT_TRUE(ft_has(
        ODIN3_LOG_WARN, "fsm: removing fsm_encoding attribute from 1-bit net 'one_bit_attr.st'"));
}

static void test_no_matching_q_is_not_marked(void) {
    /* a register whose Q is only part of the wire, and a wire that is a concatenation */
    ft_read_proc("module t(input clk, input rst, input go, output o);\n"
                 "  reg [1:0] a; reg b;\n  wire [2:0] both = {a, b};\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) begin a <= 0; b <= 0; end\n"
                 "    else begin if (go) a <= a + 1; b <= ~b; end\n"
                 "  assign o = both[2];\nendmodule\n");
    detect("");
    expect_unmarked((ft_ref){"t", "both"});
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_INFO, "fsm: found"));
}

static void test_dffe_is_not_a_state_register(void) {
    read_micro("enable_reg");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          ft_pass("opt", "")); /* opt makes the $dffe fsm_detect ignores */
    detect("");
    expect_unmarked((ft_ref){"enable_reg", "st"});
}

/* --- D-3: the mux tree ----------------------------------------------------------------------- */

static void test_closed_tree_is_marked(void) {
    read_micro("moore_arst");
    detect("");
    expect_marked((ft_ref){"moore_arst", "st"});
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_INFO, "fsm: found state register 'moore_arst.st'"));
}

static void test_counter_is_not_a_tree(void) {
    read_micro("counter_not_fsm");
    detect("");
    expect_unmarked((ft_ref){"counter_not_fsm", "st"});
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_INFO, "fsm:"));
}

static void test_port_leaf_is_not_a_tree(void) {
    ft_read_proc("module t(input clk, input rst, input go, input [1:0] nxt, output o);\n"
                 "  reg [1:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0; else if (go) st <= nxt; else st <= 1;\n"
                 "  assign o = st == 1;\nendmodule\n");
    detect("");
    expect_unmarked((ft_ref){"t", "st"});
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_INFO, "fsm:"));
}

static const char *const k_loop =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\s\n  wire width 2 \\q\n  wire width 2 \\d\n"
    "  wire width 2 \\e\n  wire output 3 \\y\n"
    "  cell $dff \\f\n    parameter \\WIDTH 2\n    parameter \\CLK_POLARITY 1\n"
    "    connect \\CLK \\clk\n    connect \\D \\d\n    connect \\Q \\q\n  end\n"
    "  cell $mux \\m1\n    parameter \\WIDTH 2\n    connect \\A \\q\n    connect \\B \\e\n"
    "    connect \\S \\s\n    connect \\Y \\d\n  end\n"
    "  cell $mux \\m2\n    parameter \\WIDTH 2\n    connect \\A \\d\n    connect \\B 2'01\n"
    "    connect \\S \\s\n    connect \\Y \\e\n  end\n"
    "  cell $logic_not \\n\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 2\n"
    "    parameter \\Y_WIDTH 1\n    connect \\A \\q\n    connect \\Y \\y\n  end\nend\n";

static void test_logic_loop_warns(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_loop, &res));
    ft_open();
    odin3_design_destroy(ft.design);
    ft.design = res.design;
    res.design = NULL;
    detect("");
    expect_unmarked((ft_ref){"m", "q"});
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_WARN, "fsm: module 'm': logic loop in mux tree at 'd'"), ft_log());
}

/* --- D-4: the users -------------------------------------------------------------------------- */

static void test_compare_users_are_good(void) {
    read_micro("ctrl_out_case");
    detect("");
    expect_marked((ft_ref){"ctrl_out_case", "st"});
}

static void test_add_reader_is_not_good(void) {
    ft_read_proc("module t(input clk, input rst, input go, output [1:0] o);\n"
                 "  reg [1:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0;\n"
                 "    else case (st) 0: if (go) st <= 1; 1: st <= 2; 2: st <= 0; endcase\n"
                 "  assign o = st + {1'b0, go};\nendmodule\n");
    detect("");
    expect_unmarked((ft_ref){"t", "st"});
    TEST_ASSERT_TRUE(
        ft_has(ODIN3_LOG_INFO, "fsm: not marking 't.st': users do not benefit from recoding"));
}

/* --- D-5: self-resetting --------------------------------------------------------------------- */

static void test_self_reset(void) {
    read_micro("self_reset");
    detect("");
    expect_unmarked((ft_ref){"self_reset", "st"});
    TEST_ASSERT_TRUE(
        ft_has(ODIN3_LOG_INFO, "fsm: not marking 'self_reset.st': seems self-resetting"));
}

static void test_ignore_self_reset(void) {
    read_micro("self_reset");
    detect("-ignore-self-reset");
    expect_marked((ft_ref){"self_reset", "st"});
}

/* --- D-6: the decision ---------------------------------------------------------------------- */

static void test_user_attribute_with_no_complaint(void) {
    read_micro("enc_one_hot");
    detect("");
    TEST_ASSERT_EQUAL_STRING("one-hot",
                             ft_wire_attr((ft_ref){"enc_one_hot", "st"}, "fsm_encoding"));
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_INFO,
                            "fsm: state register 'enc_one_hot.st' already has an fsm_encoding "
                            "attribute"));
    TEST_ASSERT_EQUAL_UINT32(0, ft_count(ODIN3_LOG_WARN, "fsm:"));
}

static void test_user_attribute_with_init_warns(void) {
    ft_read_proc("module t(input clk, input rst, input go, output o);\n"
                 "  (* fsm_encoding = \"one-hot\" *) reg [1:0] st = 0;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0;\n"
                 "    else case (st) 0: if (go) st <= 1; 1: st <= 2; 2: st <= 0; endcase\n"
                 "  assign o = st == 2;\nendmodule\n");
    detect("");
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_WARN,
               "fsm: module 't': user-specified fsm_encoding on 'st': initialization value on "
               "FSM state register is ignored; possible simulation-synthesis mismatch"),
        ft_log());
}

static void test_user_attribute_on_a_counter_warns(void) {
    ft_read_proc("module t(input clk, input rst, output o);\n"
                 "  (* fsm_encoding = \"binary\" *) reg [2:0] st;\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0; else st <= st + 1;\n"
                 "  assign o = st == 7;\nendmodule\n");
    detect("");
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_WARN,
               "fsm: module 't': user-specified fsm_encoding on 'st': doesn't look like a "
               "proper FSM; possible simulation-synthesis mismatch"),
        ft_log());
    TEST_ASSERT_EQUAL_UINT32(1, ft_count(ODIN3_LOG_WARN, "fsm:"));
}

static void test_user_attribute_on_a_port_warns(void) {
    ft_read_proc("module t(input clk, input rst, input go, (* fsm_encoding = \"one-hot\" *) "
                 "output reg [1:0] st);\n"
                 "  always @(posedge clk or posedge rst)\n"
                 "    if (rst) st <= 0;\n"
                 "    else case (st) 0: if (go) st <= 1; 1: st <= 2; 2: st <= 0; endcase\n"
                 "endmodule\n");
    detect("");
    TEST_ASSERT_TRUE_MESSAGE(
        ft_has(ODIN3_LOG_WARN,
               "fsm: module 't': user-specified fsm_encoding on 'st': forcing FSM recoding on "
               "module port might result in larger circuit"),
        ft_log());
    TEST_ASSERT_EQUAL_UINT32(2, ft_count(ODIN3_LOG_WARN, "fsm:")); /* and the users */
    TEST_ASSERT_EQUAL_STRING("one-hot", ft_wire_attr((ft_ref){"t", "st"}, "fsm_encoding"));
}

/* --- D-7: exclusions ------------------------------------------------------------------------- */

static void test_init_blocks(void) {
    read_micro("init_reg");
    detect("");
    expect_unmarked((ft_ref){"init_reg", "st"});
    TEST_ASSERT_TRUE(
        ft_has(ODIN3_LOG_INFO, "fsm: not marking 'init_reg.st': has an initialization value"));
}

static void test_port_blocks(void) {
    read_micro("port_out");
    detect("");
    expect_unmarked((ft_ref){"port_out", "st"});
    TEST_ASSERT_TRUE(
        ft_has(ODIN3_LOG_INFO, "fsm: not marking 'port_out.st': connected to a module port"));
    TEST_ASSERT_TRUE(ft_has(ODIN3_LOG_INFO,
                            "fsm: not marking 'port_out.st': users do not benefit from recoding"));
}

static void test_keep_does_not_block(void) {
    read_micro("keep_reg");
    detect("");
    expect_marked((ft_ref){"keep_reg", "st"});
}

static void test_two_fsms_in_one_module(void) {
    read_micro("two_fsm");
    detect("");
    expect_marked((ft_ref){"two_fsm", "a"});
    expect_marked((ft_ref){"two_fsm", "b"});
}

/* --- the table -------------------------------------------------------------------------------- */

/* The names the table does not compare (tests/fsm/diff_expect.txt, stage detect). */
static char skipped[MAX_NAMES][LINE];
static uint32_t n_skipped;

/* The name of a diff_expect.txt line whose stage is detect; false for any other line. */
static bool detect_line_name(const char *line, char *name) {
    char stage[32];
    return line[0] != '#' && sscanf(line, "%4095s %31s", name, stage) == 2 &&
           strcmp(stage, "detect") == 0;
}

static void load_skipped(void) {
    n_skipped = 0;
    FILE *file = fopen(ODIN3_SOURCE_DIR "/tests/fsm/diff_expect.txt", "r");
    TEST_ASSERT_NOT_NULL(file);
    char line[LINE];
    char name[LINE];
    while (fgets(line, sizeof line, file) != NULL) {
        if (detect_line_name(line, name)) {
            TEST_ASSERT_TRUE(n_skipped < MAX_NAMES);
            (void)snprintf(skipped[n_skipped++], LINE, "%s", name);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* "tests/micro/verilog/syntax/x.v" -> "micro/syntax/x", "tests/fsm/y.v" -> "fsm/y". */
static void name_of(const char *path, char *out, size_t size) {
    const char *rest = path + strlen("tests/");
    if (strncmp(rest, "micro/verilog/", 14) == 0) {
        rest = rest - 0;
        (void)snprintf(out, size, "micro/%s", rest + 14);
    } else {
        (void)snprintf(out, size, "%s", rest);
    }
    size_t len = strlen(out);
    if (len > 2 && strcmp(out + len - 2, ".v") == 0) {
        out[len - 2] = '\0';
    }
}

static bool is_skipped(const char *name) {
    for (uint32_t i = 0; i < n_skipped; i++) {
        if (strcmp(skipped[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* True when the source reads the VTR RAM models, which the micro build here does not supply. */
static bool needs_vtr_models(const char *path) {
    char full[FT_PATH];
    (void)snprintf(full, sizeof full, "%s/%s", ODIN3_SOURCE_DIR, path);
    FILE *file = fopen(full, "r");
    TEST_ASSERT_NOT_NULL(file);
    char line[LINE];
    bool found = false;
    while (!found && fgets(line, sizeof line, file) != NULL) {
        found = strstr(line, "single_port_ram") != NULL || strstr(line, "dual_port_ram") != NULL;
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    return found;
}

static const struct {
    const char *text, *code;
} k_reasons[] = {{"connected to a module port", "port"},
                 {"users do not benefit from recoding", "users"},
                 {"has an initialization value", "init"},
                 {"seems self-resetting", "selfreset"}};

/* Appends item to a comma-separated list. */
static void list_add(char *list, size_t size, const char *item) {
    (void)snprintf(list + strlen(list), size - strlen(list), "%s%s", list[0] != '\0' ? "," : "",
                   item);
}

/* Notes one "not marking" line: wire:reason for the reason it names. */
static void note_rejected(const char *text, const ft_ref *wire_ref, char *rejected, size_t size) {
    for (size_t k = 0; k < sizeof k_reasons / sizeof k_reasons[0]; k++) {
        if (strstr(text, k_reasons[k].text) != NULL) {
            char item[300];
            (void)snprintf(item, sizeof item, "%s:%s", wire_ref->name, k_reasons[k].code);
            list_add(rejected, size, item);
        }
    }
}

/* The marked wires and the rejected ones with their reasons, read off the log. */
static void summarize(char *marked, char *rejected, size_t size) {
    marked[0] = rejected[0] = '\0';
    for (uint32_t i = 0; i < ft.n_msgs; i++) {
        const char *text = ft.msgs[i].text;
        char wire[256];
        if (sscanf(text, "fsm: found state register '%255[^']'", wire) == 1) {
            list_add(marked, size, wire);
        } else if (sscanf(text, "fsm: not marking '%255[^']'", wire) == 1) {
            const ft_ref wire_ref = {NULL, wire};
            note_rejected(text, &wire_ref, rejected, size);
        }
    }
}

/* Merges "w:a,w:b" into "w:a+b" the way the table spells it. */
static void merge_reasons(const char *in, char *out, size_t size) {
    out[0] = '\0';
    char last[300] = "";
    for (const char *at = in; *at != '\0';) {
        char item[300];
        size_t len = strcspn(at, ",");
        (void)snprintf(item, sizeof item, "%.*s", (int)len, at);
        at += len + (at[len] == ',' ? 1 : 0);
        char *colon = strchr(item, ':');
        *colon = '\0';
        if (strcmp(last, item) == 0) {
            (void)snprintf(out + strlen(out), size - strlen(out), "+%s", colon + 1);
        } else {
            (void)snprintf(out + strlen(out), size - strlen(out), "%s%s:%s",
                           out[0] != '\0' ? "," : "", item, colon + 1);
            (void)snprintf(last, sizeof last, "%s", item);
        }
    }
}

static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp(lhs, rhs);
}

/* Sorts a comma-separated list in place ("-" and "" stay). */
static void sort_list(char *list) {
    char items[32][256];
    uint32_t count = 0;
    for (char *at = list; *at != '\0' && count < 32;) {
        size_t len = strcspn(at, ",");
        (void)snprintf(items[count++], sizeof items[0], "%.*s", (int)len, at);
        at += len + (at[len] == ',' ? 1 : 0);
    }
    qsort(items, count, sizeof items[0], cmp_str);
    list[0] = '\0';
    for (uint32_t i = 0; i < count; i++) {
        (void)snprintf(list + strlen(list), LINE - strlen(list), "%s%s", i > 0 ? "," : "",
                       items[i]);
    }
}

/* One row of detect_marks.txt: PATH MARKED REJECTED. True when the log agrees. */
static bool table_row(const char *path, const char *want_marked, const char *want_rejected) {
    char full[FT_PATH];
    (void)snprintf(full, sizeof full, "%s/%s", ODIN3_SOURCE_DIR, path);
    ft_read_proc_file(full);
    detect("");
    char marked[LINE];
    char rejected[LINE];
    char merged[LINE];
    summarize(marked, rejected, sizeof marked);
    merge_reasons(rejected, merged, sizeof merged);
    char want_marked_sorted[LINE];
    char want_rejected_sorted[LINE];
    (void)snprintf(want_marked_sorted, sizeof want_marked_sorted, "%s", want_marked);
    (void)snprintf(want_rejected_sorted, sizeof want_rejected_sorted, "%s", want_rejected);
    sort_list(marked);
    sort_list(merged);
    sort_list(want_marked_sorted);
    sort_list(want_rejected_sorted);
    bool same = strcmp(want_marked_sorted, marked[0] != '\0' ? marked : "-") == 0 &&
                strcmp(want_rejected_sorted, merged[0] != '\0' ? merged : "-") == 0;
    if (!same) {
        printf("MISMATCH %s: marked %s want %s; rejected %s want %s\n", path, marked, want_marked,
               merged, want_rejected);
    }
    ft_close();
    return same;
}

/* One line of detect_marks.txt: 1 when it was compared and agreed, 0 when skipped, -1 on a
 * mismatch. */
static int table_line(const char *line) {
    char path[LINE];
    char marked[LINE];
    char rejected[LINE];
    char name[LINE];
    if (line[0] == '#' || sscanf(line, "%4095s %4095s %4095s", path, marked, rejected) != 3) {
        return 0;
    }
    name_of(path, name, sizeof name);
    if (is_skipped(name) || needs_vtr_models(path)) {
        return 0;
    }
    return table_row(path, marked, rejected) ? 1 : -1;
}

static void test_table_against_yosys(void) {
    load_skipped();
    FILE *file = fopen(ODIN3_SOURCE_DIR "/tests/fsm/detect_marks.txt", "r");
    TEST_ASSERT_NOT_NULL(file);
    char line[LINE];
    uint32_t rows = 0;
    uint32_t wrong = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        int verdict = table_line(line);
        rows += verdict != 0 ? 1U : 0U;
        wrong += verdict < 0 ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_UINT32(0, wrong);
    TEST_ASSERT_TRUE_MESSAGE(rows >= 40, "the table has its micros");
}

/* --- out of memory ---------------------------------------------------------------------------- */

static void test_detect_oom(void) {
    read_micro("moore_arst");
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    long failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long after = 0; st != ODIN3_OK && after < 4000; after++) {
        odin3_util_set_alloc_fail_after(after);
        st = odin3_fsm_detect_run(ft.design, &opts);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        failures += st == ODIN3_OK ? 0 : 1;
        odin3_check_opts check = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(ft.design, check));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(failures >= 10);
    expect_marked((ft_ref){"moore_arst", "st"});
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_none_is_skipped_silently);
    RUN_TEST(test_one_bit_is_skipped);
    RUN_TEST(test_one_bit_attribute_is_removed_with_a_warning);
    RUN_TEST(test_no_matching_q_is_not_marked);
    RUN_TEST(test_dffe_is_not_a_state_register);
    RUN_TEST(test_closed_tree_is_marked);
    RUN_TEST(test_counter_is_not_a_tree);
    RUN_TEST(test_port_leaf_is_not_a_tree);
    RUN_TEST(test_logic_loop_warns);
    RUN_TEST(test_compare_users_are_good);
    RUN_TEST(test_add_reader_is_not_good);
    RUN_TEST(test_self_reset);
    RUN_TEST(test_ignore_self_reset);
    RUN_TEST(test_user_attribute_with_no_complaint);
    RUN_TEST(test_user_attribute_with_init_warns);
    RUN_TEST(test_user_attribute_on_a_counter_warns);
    RUN_TEST(test_user_attribute_on_a_port_warns);
    RUN_TEST(test_init_blocks);
    RUN_TEST(test_port_blocks);
    RUN_TEST(test_keep_does_not_block);
    RUN_TEST(test_two_fsms_in_one_module);
    RUN_TEST(test_table_against_yosys);
    RUN_TEST(test_detect_oom);
    return UNITY_END();
}
