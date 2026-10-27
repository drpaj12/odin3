/*
 * test_abc_objective.c — the objective of an abc script and of the objective-only Q line of the
 * name map (V1.2.0 3.6, Task 10). The rule (src/passes/abc/objective.c): a `# objective: <word>`
 * comment line is a stated goal (basis r); otherwise the last mapping command (`if`, `&if`, `map`,
 * `&nf`) maps for `area` when its `a` flags are odd in number and for `depth` when even (basis i);
 * a script with no mapping command states none. The Q line takes the pass name for any other pass.
 */
#include "backends/common/prov_extra.h"
#include "passes/abc/abc.h"
#include "passes/abc/script.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { K_LUT = 6, LINE_CAP = 512 };

void setUp(void) {
    odin3_log_set_sink(NULL, NULL);
}

void tearDown(void) {
}

/* Checks `text` with the BLIF exchange and K = 6 (gates: abc --gates) and returns its objective. */
static void objective_of(const char *text, bool gates, char out[ODIN3_ABC_OBJECTIVE_BYTES],
                         bool *stated) {
    const odin3_abc_script_src src = {"t.abc", NULL, odin3_bytes_cstr(text)};
    const odin3_abc_script_mode mode = {gates ? 0U : (uint32_t)K_LUT, ODIN3_ABC_EXCHANGE_BLIF,
                                        gates};
    odin3_abc_script script;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_script_parse_mode(&src, &mode, &script));
    memcpy(out, script.objective, ODIN3_ABC_OBJECTIVE_BYTES);
    *stated = script.objective_stated;
    odin3_abc_script_free(&script);
}

static void expect(const char *text, bool gates, const char *want, bool want_stated) {
    char got[ODIN3_ABC_OBJECTIVE_BYTES];
    bool stated = true;
    objective_of(text, gates, got, &stated);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want_stated, stated, text);
}

static void test_the_mapper_without_a_flag_maps_for_depth(void) {
    expect("strash\nif -K {K} -v\nmfs2 -v\n", false, "depth", false);
    expect("strash\ndch -f\nif -K 6\n", false, "depth", false);
}

static void test_the_a_flag_maps_for_area_and_toggles(void) {
    expect("strash\nif -K 6 -a\n", false, "area", false);
    expect("strash\nif -a -K 6 -a\n", false, "depth", false);
    expect("strash\nif -ak -K 6\n", false, "area", false);
    expect("strash\nif -K 6 -a -v\nlutpack\n", false, "area", false);
}

static void test_the_last_mapping_command_decides(void) {
    expect("strash\nif -K 6 -a\nif -K 6\n", false, "depth", false);
    expect("strash\nif -K 6\nif -K 6 -a\n", false, "area", false);
}

static void test_the_gate_mappers(void) {
    expect("strash\nmap -a\n", true, "area", false);
    expect("strash\nmap\n", true, "depth", false);
    expect("&get -n\n&st\n&nf -a\n", true, "area", false);
}

static void test_the_and_space_mapper(void) {
    expect("strash\n&get\n&if -K 6\n", false, "depth", false);
    expect("strash\n&get\n&if -K 6 -a\n", false, "area", false);
}

static void test_a_value_letter_takes_the_rest_of_its_cluster(void) {
    /* `-Ka` would give K the value "a" (getopt); the checker refuses it, so test the rule alone */
    const odin3_abc_cmd cmd = {"if -Ka -K 6", 1, NULL};
    char got[ODIN3_ABC_OBJECTIVE_BYTES];
    bool stated = true;
    odin3_abc_objective(NULL, 0, &cmd, 1, got, &stated);
    TEST_ASSERT_EQUAL_STRING("depth", got);
}

static void test_a_stated_goal_is_read_as_written(void) {
    expect("# objective: area\nstrash\nif -K 6\n", false, "area", true);
    expect("   #objective:   min-depth\nstrash\nif -K 6 -a\n", false, "min-depth", true);
    expect("strash\n# note\n# objective: delay\nif -K 6\n", false, "delay", true);
}

static void test_a_comment_that_is_not_a_stated_goal_is_ignored(void) {
    expect("strash\nif -K 6 # objective: area\n", false, "depth", false);
    expect("# Objective: area\nstrash\nif -K 6\n", false, "depth", false);
    expect("# objective:\nstrash\nif -K 6\n", false, "depth", false);
    expect("# objective: Area\nstrash\nif -K 6 -a\n", false, "area", false); /* word is empty */
}

static void test_a_script_with_no_mapping_command_states_none(void) {
    const odin3_abc_cmd cmds[] = {{"strash", 1, NULL}, {"dc2 -v", 2, NULL}};
    char got[ODIN3_ABC_OBJECTIVE_BYTES] = "x";
    bool stated = true;
    odin3_abc_objective(NULL, 0, cmds, 2, got, &stated);
    TEST_ASSERT_EQUAL_STRING("", got);
    TEST_ASSERT_FALSE(stated);
}

static void test_the_embedded_default_scripts(void) {
    const odin3_abc_script_src src = {ODIN3_ABC_DEFAULT_SCRIPT_NAME,
                                      NULL,
                                      {odin3_abc_default_script, odin3_abc_default_script_len}};
    odin3_abc_script script;
    const odin3_abc_script_mode mode = {K_LUT, ODIN3_ABC_EXCHANGE_BLIF, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_script_parse_mode(&src, &mode, &script));
    TEST_ASSERT_EQUAL_STRING("depth", script.objective);
    TEST_ASSERT_FALSE(script.objective_stated);
    odin3_abc_script_free(&script);
    const odin3_abc_script_src gates = {
        ODIN3_ABC_GATES_SCRIPT_NAME, NULL, {odin3_abc_gates_script, odin3_abc_gates_script_len}};
    const odin3_abc_script_mode gmode = {0, ODIN3_ABC_EXCHANGE_BLIF, true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_script_parse_mode(&gates, &gmode, &script));
    TEST_ASSERT_EQUAL_STRING("area", script.objective);
    odin3_abc_script_free(&script);
}

/* --- the objective-only Q line ---------------------------------------------------------------- */

static void q_line(const char *pass, const char *info, char *out, size_t cap) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_xline_emit_fresh_q(7, odin3_bytes_cstr(pass),
                                                             odin3_bytes_cstr(info), &buf));
    TEST_ASSERT_TRUE(buf.len < cap);
    memcpy(out, buf.data, buf.len + 1U);
    odin3_strbuf_free(&buf);
}

static void test_q_for_a_stated_objective_is_recorded(void) {
    char line[LINE_CAP];
    q_line("abc", "script=a.abc K=6 depth=1 objective=area objective_src=stated", line,
           sizeof line);
    TEST_ASSERT_EQUAL_STRING("Q\tr7\t\tarea\t\t\t\t\t\t\t\twhy=-,objective=r,gain=-,pcrit=-,rho=-,"
                             "slack=-,near=-,npaths=-\t-\n",
                             line);
}

static void test_q_for_a_script_derived_objective_is_inferred(void) {
    char line[LINE_CAP];
    q_line("abc", "script=a.abc K=6 depth=1 objective=depth objective_src=rule", line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Q\tr7\t\tdepth\t\t\t\t\t\t\t\twhy=-,objective=i,gain=-,pcrit=-,rho=-,"
                             "slack=-,near=-,npaths=-\t-\n",
                             line);
}

static void test_q_for_abc_with_no_objective_is_empty(void) {
    char line[LINE_CAP];
    q_line("abc", "script=a.abc K=6 depth=1", line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Q\tr7\t\t\t\t\t\t\t\t\t\twhy=-,objective=-,gain=-,pcrit=-,rho=-,"
                             "slack=-,near=-,npaths=-\t-\n",
                             line);
}

static void test_q_ignores_an_objective_word_inside_a_quoted_value(void) {
    char line[LINE_CAP];
    q_line("abc", "script=\"my objective=area dir/a.abc\" K=6", line, sizeof line);
    TEST_ASSERT_NOT_NULL(strstr(line, "objective=-,"));
}

static void test_q_for_another_pass_is_the_pass_name(void) {
    char line[LINE_CAP];
    q_line("opt", "objective=area", line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Q\tr7\t\topt\t\t\t\t\t\t\t\twhy=-,objective=r,gain=-,pcrit=-,rho=-,"
                             "slack=-,near=-,npaths=-\t-\n",
                             line);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_mapper_without_a_flag_maps_for_depth);
    RUN_TEST(test_the_a_flag_maps_for_area_and_toggles);
    RUN_TEST(test_the_last_mapping_command_decides);
    RUN_TEST(test_the_gate_mappers);
    RUN_TEST(test_the_and_space_mapper);
    RUN_TEST(test_a_value_letter_takes_the_rest_of_its_cluster);
    RUN_TEST(test_a_stated_goal_is_read_as_written);
    RUN_TEST(test_a_comment_that_is_not_a_stated_goal_is_ignored);
    RUN_TEST(test_a_script_with_no_mapping_command_states_none);
    RUN_TEST(test_the_embedded_default_scripts);
    RUN_TEST(test_q_for_a_stated_objective_is_recorded);
    RUN_TEST(test_q_for_a_script_derived_objective_is_inferred);
    RUN_TEST(test_q_for_abc_with_no_objective_is_empty);
    RUN_TEST(test_q_ignores_an_objective_word_inside_a_quoted_value);
    RUN_TEST(test_q_for_another_pass_is_the_pass_name);
    return UNITY_END();
}
