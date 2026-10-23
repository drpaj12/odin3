/*
 * test_read_slang.c — the SystemVerilog adapter seam (ABI v6, DESIGN §4.2) and the elaborated
 * AST it builds: packages, enums and structs folded; always_ff/comb/latch as ALWAYS subs;
 * interfaces flattened into "<instance>.<signal>" nets and "<port>.<signal>" ports; every node's
 * source range in the design's source manager (AST-2); IR provenance back to the .sv text.
 *
 * The test links libodin3 and the adapter's libodin3_slang.so (the one registered reader), so it
 * drives exactly what the CLI drives.
 */
#include "odin3/odin3.h"
#include "odin3_slang.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_SV_DATA
#error "ODIN3_SV_DATA must name tests/unit/data/slang"
#endif

#define D ODIN3_SV_DATA

enum { SV_NAME_MAX = 256, SV_PATH_MAX = 1024 };

static odin3_design *design;

void setUp(void) {
}

void tearDown(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
        design = NULL;
    }
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

/* Runs read_verilog on the files (space separated) with top and returns its status. */
static odin3_status read_sv(const char *files, const char *top) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top_name(top));
    odin3_status st = odin3_design_run_pass(design, "read_verilog", files);
    return st;
}

static uint32_t top_module(void) {
    uint32_t top = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, top);
    return top;
}

static const odin3_ast *elaborated_store(void) {
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, 1, ODIN3_AST_FORM_ELABORATED, &ast));
    TEST_ASSERT_NOT_NULL(ast);
    uint32_t run = 0;
    uint32_t form = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_run(ast, &run));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_form(ast, &form));
    TEST_ASSERT_EQUAL_UINT32(1, run);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_FORM_ELABORATED, form);
    return ast;
}

/* Counts the nodes of kind (and sub, when sub >= 0) in the elaborated store; checks every node's
 * range decodes and loc <= end within one buffer (AST-2's invariant). */
static uint32_t count_kind(const odin3_ast *ast, uint32_t kind, int sub) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_NOT_NULL(sm);
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_node_end(ast, &end));
    TEST_ASSERT_GREATER_THAN_UINT32(1, end);
    uint32_t found = 0;
    for (uint32_t node = 1; node < end; node++) {
        uint32_t k = 0;
        uint32_t s = 0;
        uint32_t loc = 0;
        uint32_t node_end = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_kind(ast, node, &k));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_sub(ast, node, &s));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_loc(ast, node, &loc));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_end(ast, node, &node_end));
        if (loc != 0) {
            odin3_srcpos pos = {0};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_position(sm, loc, &pos));
            TEST_ASSERT_NOT_EQUAL_UINT32(0, pos.buffer);
            TEST_ASSERT_GREATER_THAN_UINT32(0, pos.line);
        }
        if (node_end != 0) {
            odin3_srcpos pos = {0};
            odin3_srcpos epos = {0};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_position(sm, node_end, &epos));
            if (loc != 0 && odin3_srcman_get_position(sm, loc, &pos) == ODIN3_OK &&
                pos.buffer == epos.buffer) {
                TEST_ASSERT_LESS_OR_EQUAL_UINT32(node_end, loc);
            }
        }
        if (k == kind && (sub < 0 || s == (uint32_t)sub)) {
            found++;
        }
    }
    return found;
}

/* True when a DECLARATOR (a port's or a net's name) has this name. */
static bool has_declarator(const odin3_ast *ast, const char *name) {
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_node_end(ast, &end));
    for (uint32_t node = 1; node < end; node++) {
        uint32_t kind = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_kind(ast, node, &kind));
        if (kind != ODIN3_AST_DECLARATOR) {
            continue;
        }
        const char *text = NULL;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_name(ast, node, &text));
        if (strcmp(text, name) == 0) {
            return true;
        }
    }
    return false;
}

struct visit_count {
    int n;
    char first_file[SV_NAME_MAX];
    uint32_t first_line;
};

static void count_source(const odin3_source *src, void *user) {
    struct visit_count *vc = user;
    if (vc->n == 0 && src->file[0] != '\0') {
        (void)snprintf(vc->first_file, sizeof vc->first_file, "%s", src->file);
        vc->first_line = src->line;
    }
    vc->n++;
}

/* The design's provenance reaches the .sv text: some module or node visits a source whose file
 * ends in .sv with a nonzero line. */
static void expect_provenance(void) {
    uint32_t top = top_module();
    odin3_obj obj = {.module = top, .kind = ODIN3_OBJ_MODULE, .id = 0};
    struct visit_count vc = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_visit_sources(design, obj, count_source, &vc));
    if (vc.n == 0) {
        uint32_t end = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_end(design, top, &end));
        for (uint32_t node = 1; node < end && vc.n == 0; node++) {
            bool live = false;
            odin3_ref ref = {.module = top, .id = node};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_is_live(design, ref, &live));
            if (!live) {
                continue;
            }
            odin3_obj one = {.module = top, .kind = ODIN3_OBJ_NODE, .id = node};
            memset(&vc, 0, sizeof vc);
            (void)odin3_prov_visit_sources(design, one, count_source, &vc);
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, vc.n);
    TEST_ASSERT_GREATER_THAN_UINT32(0, vc.first_line);
    TEST_ASSERT_NOT_NULL(strstr(vc.first_file, ".sv"));
}

static void test_sv_package(void) {
    fresh_design();
    /* The golden project's package/alu/top, in compilation order. */
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files,
                   "%s/../../../golden/projects/sv_package/src/pkg/alu_pkg.sv "
                   "%s/../../../golden/projects/sv_package/src/alu.sv "
                   "%s/../../../golden/projects/sv_package/src/top.sv",
                   D, D, D);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv(files, "top"));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    uint32_t top = top_module();
    const char *name = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_name(design, top, &name));
    TEST_ASSERT_EQUAL_STRING("top", name);
    uint32_t ports = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_port_count(design, top, &ports));
    TEST_ASSERT_EQUAL_UINT32(5, ports);

    const odin3_ast *ast = elaborated_store();
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_UNIT, -1));
    TEST_ASSERT_EQUAL_UINT32(2, count_kind(ast, ODIN3_AST_MODULE, -1));
    TEST_ASSERT_EQUAL_UINT32(10, count_kind(ast, ODIN3_AST_PORT_DECL, -1));
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_FF));
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_CASE, -1));
    TEST_ASSERT_GREATER_THAN_UINT32(0, count_kind(ast, ODIN3_AST_NUMBER, -1));
    /* The package's wildcard import and its constants are folded: no dependency on package
     * names remains (no IDENT named OP_ADD or W). */
    TEST_ASSERT_TRUE(!has_declarator(ast, "OP_ADD"));
    /* One process, lowered to $proc, and nonempty IR. */
    uint32_t procs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_proc_count(design, top, &procs));
    uint32_t nodes = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_node_count(design, top, &nodes));
    TEST_ASSERT_GREATER_THAN_UINT32(0, nodes);
    expect_provenance();
}

static void test_sv_types(void) {
    fresh_design();
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_types.sv", D);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv(files, "dut"));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    const odin3_ast *ast = elaborated_store();
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_COMB));
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_FF));
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_LATCH));
    /* Packed-struct member accesses are bit selects and the memory is unpacked. */
    TEST_ASSERT_GREATER_THAN_UINT32(0, count_kind(ast, ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART));
    /* The latching memory write keeps a $proc (three processes: comb, ff, latch). */
    uint32_t top = top_module();
    uint32_t procs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_proc_count(design, top, &procs));
    TEST_ASSERT_EQUAL_UINT32(3, procs);
    expect_provenance();
}

static void test_sv_interface(void) {
    fresh_design();
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_iface.sv", D);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv(files, "top"));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    const odin3_ast *ast = elaborated_store();
    /* The interface port of leaf is flattened to "<port>.<signal>" ports, and the interface
     * instance in top to "<instance>.<signal>" nets (the declarations carry those names). */
    TEST_ASSERT_TRUE(has_declarator(ast, "s.data"));
    TEST_ASSERT_TRUE(has_declarator(ast, "s.valid"));
    TEST_ASSERT_TRUE(has_declarator(ast, "bus.data"));
    TEST_ASSERT_TRUE(has_declarator(ast, "bus.valid"));
    /* No generate survive elaboration (slang expanded them). */
    TEST_ASSERT_EQUAL_UINT32(0, count_kind(ast, ODIN3_AST_GEN_BLOCK, -1));
    TEST_ASSERT_EQUAL_UINT32(0, count_kind(ast, ODIN3_AST_GENERATE, -1));
    expect_provenance();
}

static void test_reader_is_final(void) {
    /* Registering a second reader is refused (the seam is process-wide and final). */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_sv_reader_register(odin3_read_slang));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_sv_reader_register(NULL));
}

static void test_read_slang_arguments(void) {
    const char *paths[1] = {"/nonexistent.sv"};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(NULL, 1, paths, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 0, paths, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 1, NULL, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 1, paths, 0, NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sv_reader_register(odin3_read_slang));
    RUN_TEST(test_reader_is_final);
    RUN_TEST(test_sv_package);
    RUN_TEST(test_sv_types);
    RUN_TEST(test_sv_interface);
    RUN_TEST(test_read_slang_arguments);
    return UNITY_END();
}
