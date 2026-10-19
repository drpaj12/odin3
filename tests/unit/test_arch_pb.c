/*
 * test_arch_pb.c — the pb_type walk of the architecture importer (4.1 Task 3).
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

static void parse_ok(void) {
    EQ_INT_MSG(ODIN3_OK, arch_test_parse_file(g_arch, "pb_ok.xml"), g_msg);
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
    EQ_U32(8, odin3_arch_leaf_count(g_arch));
    const odin3_arch_leaf *first = odin3_arch_leaf_at(g_arch, 0);
    EQ_INT(ODIN3_ARCH_LEAF_NAMES, first->kind);
    EQ_U32(3, first->n_levels);
    EQ_STR("clb", first->levels[0].pb);
    IS_NULL(first->levels[0].in_mode);
    EQ_STR("fle", first->levels[1].pb);
    EQ_U32(10, first->levels[1].num_pb);
    EQ_STR("lut5", first->levels[2].pb);
    EQ_STR("m5", first->levels[2].in_mode);
    EQ_STR("lut", first->cls);
    EQ_U32(2, first->n_pins);
    EQ_STR("lut_in", first->pins[0].port_class);
    EQ_INT(ODIN3_ARCH_LEAF_LATCH, leaf_named("ff")->kind);
    const odin3_arch_leaf *m9 = leaf_named("m9");
    EQ_INT(ODIN3_ARCH_LEAF_SUBCKT, m9->kind);
    EQ_STR("multiply", m9->model);
    EQ_U32(3, m9->n_levels);
    EQ_STR("mult_36", m9->levels[0].pb);
    EQ_STR("half", m9->levels[1].pb);
    EQ_STR("two", m9->levels[1].in_mode);
    EQ_U32(2, m9->levels[1].num_pb);
    EQ_STR("one", leaf_named("m36")->levels[1].in_mode);
    IS_NULL(leaf_named("sp9")->levels[1].in_mode);
}

static void test_k_and_widths(void) {
    parse_ok();
    EQ_U32(6, odin3_arch_lut_k(g_arch));
    EQ_U32(5, odin3_arch_lut_k_min(g_arch));
    IS_TRUE(odin3_arch_has_adder(g_arch));
    const odin3_arch_model *mul = odin3_arch_model_find(g_arch, "multiply");
    EQ_U32(36, mul->ports[0].max_width);
    EQ_U32(9, mul->ports[0].min_width);
    EQ_U32(72, mul->ports[2].max_width);
    const odin3_arch_model *ram = odin3_arch_model_find(g_arch, "single_port_ram");
    EQ_STR("addr", ram->ports[1].name);
    EQ_U32(15, ram->ports[1].max_width);
    EQ_U32(9, ram->ports[1].min_width);
    EQ_U32(64, ram->ports[2].max_width);
    EQ_U32(1, ram->ports[2].min_width);
    EQ_U32(1, ram->ports[3].max_width); /* clk */
    uint32_t idx = 0;
    IS_TRUE(odin3_arch_next_leaf_of(g_arch, "single_port_ram", 0, &idx));
    EQ_STR("sp9", odin3_arch_leaf_at(g_arch, idx)->levels[1].pb);
    IS_TRUE(odin3_arch_next_leaf_of(g_arch, "single_port_ram", idx + 1, &idx));
    EQ_STR("sp15", odin3_arch_leaf_at(g_arch, idx)->levels[1].pb);
}

static void test_interconnect_and_directlist(void) {
    parse_ok();
    EQ_U32(4, odin3_arch_ic_count(g_arch));
    const odin3_arch_ic *d1 = odin3_arch_ic_at(g_arch, 0);
    EQ_STR("direct", d1->kind);
    EQ_STR("d1", d1->name);
    EQ_STR("fle", d1->pb);
    EQ_STR("m5", d1->in_mode);
    EQ_STR("fle.in", d1->input);
    const odin3_arch_ic *chain = odin3_arch_ic_at(g_arch, 1);
    EQ_U32(1, chain->n_packs);
    EQ_STR("chain", chain->packs[0].name);
    EQ_STR("add.cin", chain->packs[0].out_port);
    EQ_STR("mux", odin3_arch_ic_at(g_arch, 2)->kind);
    const odin3_arch_ic *cross = odin3_arch_ic_at(g_arch, 3);
    EQ_STR("complete", cross->kind);
    EQ_STR("clb", cross->pb);
    IS_NULL(cross->in_mode);
    EQ_U32(1, odin3_arch_direct_count(g_arch));
    const odin3_arch_direct *dir = odin3_arch_direct_at(g_arch, 0);
    EQ_STR("adder_carry", dir->name);
    EQ_STR("clb.cout", dir->from_pin);
    EQ_INT(-1, dir->y_offset);
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
}

static void test_leaf_errors(void) {
    static const err_case k_cases[] = {
        {"pb_unknown_model.xml",
         "pb_unknown_model.xml:8: pb_type 'leaf' uses model 'nope', which is not in "
         "<models>"},
        {"pb_port_dir.xml",
         "pb_port_dir.xml:8: pb_type 'leaf' port 'a' is an output but model 'm' declares "
         "an input"},
        {"pb_bad_blif.xml", "pb_bad_blif.xml:8: unsupported blif_model '.gate'"},
    };
    for (size_t idx = 0; idx < sizeof k_cases / sizeof k_cases[0]; idx++) {
        expect_error(&k_cases[idx]);
    }
}

static void test_no_block_list(void) {
    EQ_INT(ODIN3_OK, arch_test_parse(g_arch, "n.xml", "<architecture/>"));
    EQ_U32(0, odin3_arch_leaf_count(g_arch));
    EQ_U32(0, odin3_arch_lut_k(g_arch));
    IS_FALSE(odin3_arch_has_adder(g_arch));
}

enum { DEEP = 10000 };

static void test_deep_nesting_has_no_recursion(void) {
    static const char open[] = "<pb_type name=\"p\">";
    static const char close[] = "</pb_type>";
    static const char leaf[] = "<pb_type name=\"l\" blif_model=\".names\"><input name=\"in\" "
                               "num_pins=\"4\"/></pb_type>";
    size_t cap = DEEP * (sizeof open + sizeof close) + 256;
    char *text = malloc(cap);
    NOT_NULL(text);
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
    EQ_INT_MSG(ODIN3_OK, st, g_msg);
    EQ_U32(1, odin3_arch_leaf_count(g_arch));
    EQ_U32(DEEP + 1, odin3_arch_leaf_at(g_arch, 0)->n_levels);
    EQ_U32(4, odin3_arch_lut_k(g_arch));
}

static void test_oom_sweep(void) {
    for (long limit = 0; limit < 4000; limit++) {
        odin3_arch *arch = odin3_arch_create();
        char *text = arch_test_load("pb_ok.xml");
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
