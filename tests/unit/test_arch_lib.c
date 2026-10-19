/*
 * test_arch_lib.c — the library generated from the models (4.1 Task 4).
 */
#include "arch_test.h"
#include "arch_test_cmp.h"
#include "techlib/reader.h"

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

#define VTR_MODELS                                                                            \
    "<architecture><models>"                                                                  \
    "<model name=\"adder\"><input_ports><port name=\"a\"/><port name=\"b\"/>"                 \
    "<port name=\"cin\"/></input_ports><output_ports><port name=\"cout\"/>"                   \
    "<port name=\"sumout\"/></output_ports></model>"                                          \
    "<model name=\"multiply\"><input_ports><port name=\"a\"/><port name=\"b\"/></input_ports>" \
    "<output_ports><port name=\"out\"/></output_ports></model>"                               \
    "<model name=\"single_port_ram\"><input_ports><port name=\"we\" clock=\"clk\"/>"          \
    "<port name=\"addr\" clock=\"clk\"/><port name=\"data\" clock=\"clk\"/>"                  \
    "<port name=\"clk\" is_clock=\"1\"/></input_ports><output_ports>"                         \
    "<port name=\"out\" clock=\"clk\"/></output_ports></model>"                               \
    "<model name=\"dual_port_ram\"><input_ports><port name=\"we1\"/><port name=\"we2\"/>"     \
    "<port name=\"addr1\"/><port name=\"addr2\"/><port name=\"data1\"/><port name=\"data2\"/>" \
    "<port name=\"clk\" is_clock=\"1\"/></input_ports><output_ports><port name=\"out1\"/>"    \
    "<port name=\"out2\"/></output_ports></model></models></architecture>"

static char *normalized(const char *text) {
    size_t len = strlen(text);
    char *out = calloc(len + 1, 1);
    TEST_ASSERT_NOT_NULL(out);
    size_t at = 0;
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t n = end != NULL ? (size_t)(end - line) : strlen(line);
        char tmp[256];
        TEST_ASSERT_TRUE(n < sizeof tmp);
        memcpy(tmp, line, n);
        tmp[n] = '\0';
        char *hash = strchr(tmp, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        size_t m = strlen(tmp);
        while (m > 0 && (tmp[m - 1] == ' ' || tmp[m - 1] == '\t' || tmp[m - 1] == '\r')) {
            tmp[--m] = '\0';
        }
        if (m > 0 && strncmp(tmp, "library ", 8) != 0) {
            at += (size_t)sprintf(out + at, "%s\n", tmp);
        }
        line = end != NULL ? end + 1 : line + n;
    }
    return out;
}

static char *read_all(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *buf = calloc(1u << 16, 1);
    TEST_ASSERT_NOT_NULL(buf);
    (void)fread(buf, 1, (1u << 16) - 1u, fp);
    (void)fclose(fp);
    return buf;
}

static void make(const char *xml) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, arch_test_parse(g_arch, "x/arch one.xml", xml), g_msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_make_library(g_arch));
}

static void test_templates_equal_vtr_o3lib_text(void) {
    make(VTR_MODELS);
    char *file = read_all(ODIN3_LIB_DIR "/vtr.o3lib");
    char *want = normalized(file);
    char *got = normalized(odin3_arch_library_text(g_arch));
    TEST_ASSERT_EQUAL_STRING(want, got);
    free(file);
    free(want);
    free(got);
    TEST_ASSERT_EQUAL_UINT32(4, g_arch->n_registered);
    TEST_ASSERT_EQUAL_STRING("arch_one", g_arch->library);
    TEST_ASSERT_NOT_NULL(strstr(odin3_arch_library_text(g_arch), "library arch_one\n"));
    TEST_ASSERT_EQUAL_UINT(0, g_warnings);
}

static void test_templates_equal_vtr_o3lib_types(void) {
    make(VTR_MODELS);
    odin3_design *da = odin3_design_create();
    odin3_design *db = odin3_design_create();
    const odin3_techlib_text src = {"gen", odin3_bytes_cstr(odin3_arch_library_text(g_arch))};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read_text(da, &src), g_msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(db, ODIN3_LIB_DIR "/vtr.o3lib"),
                                  g_msg);
    static const char *const k_names[] = {"adder", "multiply", "single_port_ram", "dual_port_ram"};
    for (size_t i = 0; i < 4; i++) {
        arch_test_same_type(da, db, k_names[i]);
    }
    odin3_design_destroy(da);
    odin3_design_destroy(db);
}

static void test_template_mismatch_is_black_box(void) {
    make("<architecture><models><model name=\"multiply\"><input_ports><port name=\"a\"/>"
         "<port name=\"c\"/></input_ports><output_ports><port name=\"out\"/></output_ports>"
         "</model></models></architecture>");
    TEST_ASSERT_EQUAL_UINT(1, g_warnings);
    TEST_ASSERT_EQUAL_STRING("x/arch one.xml:1: model 'multiply' differs from the VTR primitive "
                             "(port 'c' is not a port of the primitive); imported as a black box",
                             g_warn);
    TEST_ASSERT_NOT_NULL(strstr(odin3_arch_library_text(g_arch), "cell multiply blackbox\n"));
    TEST_ASSERT_NULL(strstr(odin3_arch_library_text(g_arch), "param WIDTH"));
}

static void test_generic_widths_and_clock(void) {
    make("<architecture><models><model name=\"pcie\"><input_ports><port name=\"a\"/>"
         "<port name=\"c\" is_clock=\"1\"/><port name=\"u\"/></input_ports>"
         "<output_ports><port name=\"o\"/></output_ports></model></models>"
         "<complexblocklist><pb_type name=\"b\">"
         "<pb_type name=\"p1\" blif_model=\".subckt pcie\"><input name=\"a\" num_pins=\"3\"/>"
         "<clock name=\"c\"/><output name=\"o\" num_pins=\"70\"/></pb_type>"
         "<pb_type name=\"p2\" blif_model=\".subckt pcie\"><input name=\"a\" num_pins=\"8\"/>"
         "<clock name=\"c\"/><output name=\"o\" num_pins=\"2\"/></pb_type>"
         "</pb_type></complexblocklist></architecture>");
    const char *text = odin3_arch_library_text(g_arch);
    TEST_ASSERT_NOT_NULL(strstr(text, "cell pcie blackbox\n  in a 8\n  in c 1 clock\n  in u 1\n"
                                      "  out o 70\nend\n"));
    TEST_ASSERT_EQUAL_STRING("x/arch one.xml:1: model 'pcie' port 'u' is not used by any pb_type; "
                             "width 1",
                             g_info);
}

static void test_non_identifier_names_are_skipped(void) {
    make("<architecture><models>"
         "<model name=\"ok\"><input_ports><port name=\"a\"/></input_ports></model>"
         "<model name=\"ram.opmode{rom}\"><input_ports><port name=\"a\"/></input_ports></model>"
         "<model name=\"xp\"><input_ports><port name=\"x\"/></input_ports></model>"
         "</models></architecture>");
    TEST_ASSERT_EQUAL_UINT32(3, g_arch->n_models);
    TEST_ASSERT_EQUAL_UINT32(1, g_arch->n_registered);
    TEST_ASSERT_TRUE(g_arch->models[0].registered);
    TEST_ASSERT_FALSE(g_arch->models[1].registered);
    TEST_ASSERT_FALSE(g_arch->models[2].registered);
    TEST_ASSERT_EQUAL_STRING("x/arch one.xml: 2 models have names that are not .o3lib identifiers "
                             "and are not registered (first: 'ram.opmode{rom}')",
                             g_warn);
    TEST_ASSERT_NULL(strstr(odin3_arch_library_text(g_arch), "ram.opmode"));
}

static void test_no_models_makes_empty_library(void) {
    make("<architecture/>");
    odin3_design *design = odin3_design_create();
    const odin3_techlib_text src = {"gen", odin3_bytes_cstr(odin3_arch_library_text(g_arch))};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read_text(design, &src), g_msg);
    odin3_design_destroy(design);
}

static void test_library_oom_sweep(void) {
    for (long n = 0; n < 200; n++) {
        odin3_arch *arch = odin3_arch_create();
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, arch_test_parse(arch, "m.xml", VTR_MODELS));
        odin3_util_set_alloc_fail_after(n);
        odin3_status st = odin3_arch_make_library(arch);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        odin3_arch_destroy(arch);
        if (st == ODIN3_OK) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("never succeeded");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_templates_equal_vtr_o3lib_text);
    RUN_TEST(test_templates_equal_vtr_o3lib_types);
    RUN_TEST(test_template_mismatch_is_black_box);
    RUN_TEST(test_generic_widths_and_clock);
    RUN_TEST(test_non_identifier_names_are_skipped);
    RUN_TEST(test_no_models_makes_empty_library);
    RUN_TEST(test_library_oom_sweep);
    return UNITY_END();
}
