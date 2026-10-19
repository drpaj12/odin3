/*
 * test_arch_models.c — the `<models>` reader of the architecture importer (4.1 Task 2).
 */
#include "arch_test.h"

static odin3_arch *g_arch;

void setUp(void) {
    arch_test_reset();
    g_arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(g_arch);
}

void tearDown(void) {
    odin3_arch_destroy(g_arch);
    arch_test_done();
}

static void test_reference_models(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, arch_test_parse_file(g_arch, "models_ok.xml"), g_msg);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_arch_model_count(g_arch));
    const odin3_arch_model *mul = odin3_arch_model_at(g_arch, 0);
    TEST_ASSERT_EQUAL_STRING("multiply", mul->name);
    TEST_ASSERT_EQUAL_UINT32(3, mul->n_ports);
    TEST_ASSERT_EQUAL_STRING("a", mul->ports[0].name);
    TEST_ASSERT_FALSE(mul->ports[0].is_out);
    TEST_ASSERT_EQUAL_UINT32(1, mul->ports[0].n_sinks);
    TEST_ASSERT_EQUAL_STRING("out", mul->ports[0].sinks[0]);
    TEST_ASSERT_TRUE(mul->ports[2].is_out);
    TEST_ASSERT_EQUAL_UINT32(4, mul->line);
    const odin3_arch_model *ram = odin3_arch_model_find(g_arch, "single_port_ram");
    TEST_ASSERT_NOT_NULL(ram);
    TEST_ASSERT_EQUAL_UINT32(4, ram->n_ports);
    TEST_ASSERT_EQUAL_STRING("clk", ram->ports[0].clock);
    TEST_ASSERT_TRUE(ram->ports[1].is_clock);
    TEST_ASSERT_TRUE(ram->ports[2].non_clock_global);
    TEST_ASSERT_EQUAL_STRING("clk", ram->ports[3].clock);
    TEST_ASSERT_TRUE(ram->ports[3].is_out);
    TEST_ASSERT_NULL(odin3_arch_model_find(g_arch, "nope"));
}

static void test_no_models(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, arch_test_parse_file(g_arch, "no_models.xml"), g_msg);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_model_count(g_arch));
}

static void test_errors(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, arch_test_parse_file(g_arch, "bad_root.xml"));
    TEST_ASSERT_EQUAL_STRING("bad_root.xml:1: expected the root element 'architecture'", g_msg);
}

static void expect_error(const char *file, const char *msg) {
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, arch_test_parse_file(g_arch, file));
    TEST_ASSERT_EQUAL_STRING(msg, g_msg);
    TEST_ASSERT_EQUAL_UINT(1, g_errors);
}

static void test_each_error(void) {
    expect_error("models_dup.xml", "models_dup.xml:4: duplicate model 'm'");
    expect_error("models_sink.xml",
                 "models_sink.xml:3: combinational sink 'nope' is not an output of model 'm'");
    expect_error("unsupported_child.xml",
                 "unsupported_child.xml:4: unsupported element 'xi:include' in <models>");
}

static void expect_text_error(const char *text, const char *msg) {
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, arch_test_parse(g_arch, "t.xml", text));
    TEST_ASSERT_EQUAL_STRING(msg, g_msg);
}

static void test_inline_errors(void) {
    expect_text_error("<architecture><models><model name=\"m\"><input_ports>"
                      "<port name=\"a\"/><port name=\"a\"/></input_ports></model></models>"
                      "</architecture>",
                      "t.xml:1: duplicate port 'a' in model 'm'");
    expect_text_error("<architecture><models><model name=\"m\"><input_ports>"
                      "<port name=\"a\" is_clock=\"1\" is_non_clock_global=\"1\"/></input_ports>"
                      "</model></models></architecture>",
                      "t.xml:1: port 'a' cannot be both a clock and a non-clock global");
    expect_text_error("<architecture><models><model><input_ports/></model></models></architecture>",
                      "t.xml:1: <model> needs a 'name' attribute");
    expect_text_error("<architecture><models><model name=\"m\"><output_ports>"
                      "<port name=\"o\" combinational_sink_ports=\"o\"/></output_ports></model>"
                      "</models></architecture>",
                      "t.xml:1: output port 'o' of model 'm' cannot have combinational_sink_ports");
    expect_text_error("<architecture><models><model name=\"m\"><input_ports>"
                      "<port name=\"a\" is_clock=\"maybe\"/></input_ports></model></models>"
                      "</architecture>",
                      "t.xml:1: attribute 'is_clock' must be 1, 0, true or false, not 'maybe'");
    expect_text_error("<architecture><models><model name=\"m\"><junk/></model></models>"
                      "</architecture>",
                      "t.xml:1: unsupported element 'junk' in <model>");
}

static void test_xml_syntax_and_bom(void) {
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, arch_test_parse(g_arch, "s.xml", "<architecture>"));
    TEST_ASSERT_TRUE(strncmp(g_msg, "s.xml:1: ", 9) == 0);
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        arch_test_parse(g_arch, "b.xml",
                        "\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?>\n<architecture/>\n"),
        g_msg);
}

static void test_oom_sweep(void) {
    for (long n = 0; n < 400; n++) {
        odin3_arch *arch = odin3_arch_create();
        TEST_ASSERT_NOT_NULL(arch);
        char *text = arch_test_load("models_ok.xml");
        odin3_util_set_alloc_fail_after(n);
        odin3_status st = arch_test_parse(arch, "m.xml", text);
        odin3_util_set_alloc_fail_after(-1);
        free(text);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        odin3_arch_destroy(arch);
        if (st == ODIN3_OK) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("the import never succeeded within 400 allocations");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reference_models);
    RUN_TEST(test_no_models);
    RUN_TEST(test_errors);
    RUN_TEST(test_each_error);
    RUN_TEST(test_inline_errors);
    RUN_TEST(test_xml_syntax_and_bom);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
