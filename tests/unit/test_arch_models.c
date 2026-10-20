/*
 * test_arch_models.c — the `<models>` reader of the architecture importer (4.1 Task 2).
 */
#include "arch_test.h"

static odin3_arch *g_arch;

void setUp(void) {
    arch_test_reset();
    g_arch = odin3_arch_create();
    NOT_NULL(g_arch);
}

void tearDown(void) {
    odin3_arch_destroy(g_arch);
    arch_test_done();
}

static void parse_models_ok(void) {
    EQ_INT_MSG(ODIN3_OK, arch_test_parse_file(g_arch, "models_ok.xml"), g_msg);
    EQ_U32(2, odin3_arch_model_count(g_arch));
}

static void test_reference_multiply(void) {
    parse_models_ok();
    const odin3_arch_model *mul = odin3_arch_model_at(g_arch, 0);
    EQ_STR("multiply", mul->name);
    EQ_U32(3, mul->n_ports);
    EQ_STR("a", mul->ports[0].name);
    IS_FALSE(mul->ports[0].is_out);
    EQ_U32(1, mul->ports[0].n_sinks);
    EQ_STR("out", mul->ports[0].sinks[0]);
    IS_TRUE(mul->ports[2].is_out);
    EQ_U32(4, mul->line);
}

static void test_reference_ram(void) {
    parse_models_ok();
    const odin3_arch_model *ram = odin3_arch_model_find(g_arch, "single_port_ram");
    NOT_NULL(ram);
    EQ_U32(4, ram->n_ports);
    EQ_STR("clk", ram->ports[0].clock);
    IS_TRUE(ram->ports[1].is_clock);
    IS_TRUE(ram->ports[2].non_clock_global);
    EQ_STR("clk", ram->ports[3].clock);
    IS_TRUE(ram->ports[3].is_out);
    IS_NULL(odin3_arch_model_find(g_arch, "nope"));
}

static void test_no_models(void) {
    EQ_INT_MSG(ODIN3_OK, arch_test_parse_file(g_arch, "no_models.xml"), g_msg);
    EQ_U32(0, odin3_arch_model_count(g_arch));
}

static void test_errors(void) {
    EQ_INT(ODIN3_ERR_PARSE, arch_test_parse_file(g_arch, "bad_root.xml"));
    EQ_STR("bad_root.xml:1: expected the root element 'architecture'", g_msg);
}

typedef struct err_case {
    const char *file;
    const char *msg;
} err_case;

static void expect_error(const err_case *want) {
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    arch_test_reset();
    EQ_INT(ODIN3_ERR_PARSE, arch_test_parse_file(g_arch, want->file));
    EQ_STR(want->msg, g_msg);
    EQ_U32(1, g_errors);
}

static void test_each_error(void) {
    static const err_case k_cases[] = {
        {"models_dup.xml", "models_dup.xml:4: duplicate model 'm'"},
        {"models_sink.xml",
         "models_sink.xml:3: combinational sink 'nope' is not an output of model 'm'"},
        {"unsupported_child.xml",
         "unsupported_child.xml:4: unsupported element 'xi:include' in <models>"}};
    for (size_t idx = 0; idx < sizeof k_cases / sizeof k_cases[0]; idx++) {
        expect_error(&k_cases[idx]);
    }
}

static void expect_text_error(const err_case *want) {
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    arch_test_reset();
    EQ_INT(ODIN3_ERR_PARSE, arch_test_parse(g_arch, "t.xml", want->file));
    EQ_STR(want->msg, g_msg);
}

static void test_inline_errors(void) {
    static const err_case k_cases[] = {
        {"<architecture><models><model name=\"m\"><input_ports>"
         "<port name=\"a\"/><port name=\"a\"/></input_ports></model></models>"
         "</architecture>",
         "t.xml:1: duplicate port 'a' in model 'm'"},
        {"<architecture><models><model name=\"m\"><input_ports>"
         "<port name=\"a\" is_clock=\"1\" is_non_clock_global=\"1\"/></input_ports>"
         "</model></models></architecture>",
         "t.xml:1: port 'a' cannot be both a clock and a non-clock global"},
        {"<architecture><models><model><input_ports/></model></models></architecture>",
         "t.xml:1: <model> needs a 'name' attribute"},
        {"<architecture><models><model name=\"m\"><output_ports>"
         "<port name=\"o\" combinational_sink_ports=\"o\"/></output_ports></model>"
         "</models></architecture>",
         "t.xml:1: output port 'o' of model 'm' cannot have combinational_sink_ports"},
        {"<architecture><models><model name=\"m\"><input_ports>"
         "<port name=\"a\" is_clock=\"maybe\"/></input_ports></model></models>"
         "</architecture>",
         "t.xml:1: attribute 'is_clock' must be 1, 0, true or false, not 'maybe'"},
        {"<architecture><models><model name=\"m\"><junk/></model></models>"
         "</architecture>",
         "t.xml:1: unsupported element 'junk' in <model>"},
    };
    for (size_t idx = 0; idx < sizeof k_cases / sizeof k_cases[0]; idx++) {
        expect_text_error(&k_cases[idx]);
    }
}

static void test_xml_syntax_and_bom(void) {
    arch_test_reset();
    EQ_INT(ODIN3_ERR_PARSE, arch_test_parse(g_arch, "s.xml", "<architecture>"));
    IS_TRUE(strncmp(g_msg, "s.xml:1: ", 9) == 0);
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    EQ_INT_MSG(
        ODIN3_OK,
        arch_test_parse(g_arch, "b.xml",
                        "\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?>\n<architecture/>\n"),
        g_msg);
}

static void test_oom_sweep(void) {
    for (long limit = 0; limit < 400; limit++) {
        odin3_arch *arch = odin3_arch_create();
        NOT_NULL(arch);
        char *text = arch_test_load("models_ok.xml");
        odin3_util_set_alloc_fail_after(limit);
        odin3_status st = arch_test_parse(arch, "m.xml", text);
        odin3_util_set_alloc_fail_after(-1);
        free(text);
        IS_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        odin3_arch_destroy(arch);
        if (st == ODIN3_OK) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("the import never succeeded within 400 allocations");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reference_multiply);
    RUN_TEST(test_reference_ram);
    RUN_TEST(test_no_models);
    RUN_TEST(test_errors);
    RUN_TEST(test_each_error);
    RUN_TEST(test_inline_errors);
    RUN_TEST(test_xml_syntax_and_bom);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
