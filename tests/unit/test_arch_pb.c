/*
 * test_arch_pb.c — the pb_type walk of the architecture importer (4.1 Task 3).
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

static void parse_ok(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, arch_test_parse_file(g_arch, "pb_ok.xml"), g_msg);
}

static const odin3_arch_leaf *leaf_named(const char *pb) {
    for (uint32_t i = 0; i < odin3_arch_leaf_count(g_arch); i++) {
        const odin3_arch_leaf *leaf = odin3_arch_leaf_at(g_arch, i);
        if (strcmp(leaf->levels[leaf->n_levels - 1].pb, pb) == 0) {
            return leaf;
        }
    }
    TEST_FAIL_MESSAGE(pb);
    return NULL;
}

static void test_leaves_and_paths(void) {
    parse_ok();
    TEST_ASSERT_EQUAL_UINT32(8, odin3_arch_leaf_count(g_arch));
    const odin3_arch_leaf *first = odin3_arch_leaf_at(g_arch, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ARCH_LEAF_NAMES, first->kind);
    TEST_ASSERT_EQUAL_UINT32(3, first->n_levels);
    TEST_ASSERT_EQUAL_STRING("clb", first->levels[0].pb);
    TEST_ASSERT_NULL(first->levels[0].in_mode);
    TEST_ASSERT_EQUAL_STRING("fle", first->levels[1].pb);
    TEST_ASSERT_EQUAL_UINT32(10, first->levels[1].num_pb);
    TEST_ASSERT_EQUAL_STRING("lut5", first->levels[2].pb);
    TEST_ASSERT_EQUAL_STRING("m5", first->levels[2].in_mode);
    TEST_ASSERT_EQUAL_STRING("lut", first->cls);
    TEST_ASSERT_EQUAL_UINT32(2, first->n_pins);
    TEST_ASSERT_EQUAL_STRING("lut_in", first->pins[0].port_class);
    TEST_ASSERT_EQUAL_INT(ODIN3_ARCH_LEAF_LATCH, leaf_named("ff")->kind);
    const odin3_arch_leaf *m9 = leaf_named("m9");
    TEST_ASSERT_EQUAL_INT(ODIN3_ARCH_LEAF_SUBCKT, m9->kind);
    TEST_ASSERT_EQUAL_STRING("multiply", m9->model);
    TEST_ASSERT_EQUAL_UINT32(3, m9->n_levels);
    TEST_ASSERT_EQUAL_STRING("mult_36", m9->levels[0].pb);
    TEST_ASSERT_EQUAL_STRING("half", m9->levels[1].pb);
    TEST_ASSERT_EQUAL_STRING("two", m9->levels[1].in_mode);
    TEST_ASSERT_EQUAL_UINT32(2, m9->levels[1].num_pb);
    TEST_ASSERT_EQUAL_STRING("one", leaf_named("m36")->levels[1].in_mode);
    TEST_ASSERT_NULL(leaf_named("sp9")->levels[1].in_mode);
}

static void test_k_and_widths(void) {
    parse_ok();
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(g_arch));
    TEST_ASSERT_EQUAL_UINT32(5, odin3_arch_lut_k_min(g_arch));
    TEST_ASSERT_TRUE(odin3_arch_has_adder(g_arch));
    const odin3_arch_model *mul = odin3_arch_model_find(g_arch, "multiply");
    TEST_ASSERT_EQUAL_UINT32(36, mul->ports[0].max_width);
    TEST_ASSERT_EQUAL_UINT32(9, mul->ports[0].min_width);
    TEST_ASSERT_EQUAL_UINT32(72, mul->ports[2].max_width);
    const odin3_arch_model *ram = odin3_arch_model_find(g_arch, "single_port_ram");
    TEST_ASSERT_EQUAL_STRING("addr", ram->ports[1].name);
    TEST_ASSERT_EQUAL_UINT32(15, ram->ports[1].max_width);
    TEST_ASSERT_EQUAL_UINT32(9, ram->ports[1].min_width);
    TEST_ASSERT_EQUAL_UINT32(64, ram->ports[2].max_width);
    TEST_ASSERT_EQUAL_UINT32(1, ram->ports[2].min_width);
    TEST_ASSERT_EQUAL_UINT32(1, ram->ports[3].max_width); /* clk */
    uint32_t idx = 0;
    TEST_ASSERT_TRUE(odin3_arch_next_leaf_of(g_arch, "single_port_ram", 0, &idx));
    TEST_ASSERT_EQUAL_STRING("sp9", odin3_arch_leaf_at(g_arch, idx)->levels[1].pb);
    TEST_ASSERT_TRUE(odin3_arch_next_leaf_of(g_arch, "single_port_ram", idx + 1, &idx));
    TEST_ASSERT_EQUAL_STRING("sp15", odin3_arch_leaf_at(g_arch, idx)->levels[1].pb);
}

static void test_interconnect_and_directlist(void) {
    parse_ok();
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_ic_count(g_arch));
    const odin3_arch_ic *d1 = odin3_arch_ic_at(g_arch, 0);
    TEST_ASSERT_EQUAL_STRING("direct", d1->kind);
    TEST_ASSERT_EQUAL_STRING("d1", d1->name);
    TEST_ASSERT_EQUAL_STRING("fle", d1->pb);
    TEST_ASSERT_EQUAL_STRING("m5", d1->in_mode);
    TEST_ASSERT_EQUAL_STRING("fle.in", d1->input);
    const odin3_arch_ic *chain = odin3_arch_ic_at(g_arch, 1);
    TEST_ASSERT_EQUAL_UINT32(1, chain->n_packs);
    TEST_ASSERT_EQUAL_STRING("chain", chain->packs[0].name);
    TEST_ASSERT_EQUAL_STRING("add.cin", chain->packs[0].out_port);
    TEST_ASSERT_EQUAL_STRING("mux", odin3_arch_ic_at(g_arch, 2)->kind);
    const odin3_arch_ic *cross = odin3_arch_ic_at(g_arch, 3);
    TEST_ASSERT_EQUAL_STRING("complete", cross->kind);
    TEST_ASSERT_EQUAL_STRING("clb", cross->pb);
    TEST_ASSERT_NULL(cross->in_mode);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_arch_direct_count(g_arch));
    const odin3_arch_direct *dir = odin3_arch_direct_at(g_arch, 0);
    TEST_ASSERT_EQUAL_STRING("adder_carry", dir->name);
    TEST_ASSERT_EQUAL_STRING("clb.cout", dir->from_pin);
    TEST_ASSERT_EQUAL_INT32(-1, dir->y_offset);
}

static void expect_error(const char *file, const char *msg) {
    odin3_arch_destroy(g_arch);
    g_arch = odin3_arch_create();
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, arch_test_parse_file(g_arch, file));
    TEST_ASSERT_EQUAL_STRING(msg, g_msg);
}

static void test_leaf_errors(void) {
    expect_error("pb_unknown_model.xml",
                 "pb_unknown_model.xml:8: pb_type 'leaf' uses model 'nope', which is not in "
                 "<models>");
    expect_error("pb_unknown_port.xml",
                 "pb_unknown_port.xml:8: pb_type 'leaf' port 'zz' is not a port of model 'm'");
    expect_error("pb_port_dir.xml",
                 "pb_port_dir.xml:8: pb_type 'leaf' port 'a' is an output but model 'm' declares "
                 "an input");
    expect_error("pb_clock.xml",
                 "pb_clock.xml:8: pb_type 'leaf' port 'clk' is not a clock but model 'm' "
                 "declares it one");
    expect_error("pb_bad_blif.xml", "pb_bad_blif.xml:8: unsupported blif_model '.gate'");
}

static void test_no_block_list(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, arch_test_parse(g_arch, "n.xml", "<architecture/>"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_leaf_count(g_arch));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_lut_k(g_arch));
    TEST_ASSERT_FALSE(odin3_arch_has_adder(g_arch));
}

enum { DEEP = 10000 };

static void test_deep_nesting_has_no_recursion(void) {
    static const char open[] = "<pb_type name=\"p\">";
    static const char close[] = "</pb_type>";
    static const char leaf[] = "<pb_type name=\"l\" blif_model=\".names\"><input name=\"in\" "
                               "num_pins=\"4\"/></pb_type>";
    size_t cap = DEEP * (sizeof open + sizeof close) + 256;
    char *text = malloc(cap);
    TEST_ASSERT_NOT_NULL(text);
    char *at = text + sprintf(text, "<architecture><complexblocklist>");
    for (int i = 0; i < DEEP; i++) {
        at += sprintf(at, "%s", open);
    }
    at += sprintf(at, "%s", leaf);
    for (int i = 0; i < DEEP; i++) {
        at += sprintf(at, "%s", close);
    }
    (void)sprintf(at, "</complexblocklist></architecture>");
    odin3_status st = arch_test_parse(g_arch, "deep.xml", text);
    free(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, g_msg);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_arch_leaf_count(g_arch));
    TEST_ASSERT_EQUAL_UINT32(DEEP + 1, odin3_arch_leaf_at(g_arch, 0)->n_levels);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_lut_k(g_arch));
}

static void test_oom_sweep(void) {
    for (long n = 0; n < 4000; n++) {
        odin3_arch *arch = odin3_arch_create();
        char *text = arch_test_load("pb_ok.xml");
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
    TEST_FAIL_MESSAGE("the import never succeeded");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_leaves_and_paths);
    RUN_TEST(test_k_and_widths);
    RUN_TEST(test_interconnect_and_directlist);
    RUN_TEST(test_leaf_errors);
    RUN_TEST(test_no_block_list);
    RUN_TEST(test_deep_nesting_has_no_recursion);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
