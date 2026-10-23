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

/* One SystemVerilog read: the files (space separated) and the top. */
struct sv_case {
    const char *files;
    const char *top;
};

/* Captures the log so a failing read's message can be checked. */
struct log_capture {
    char text[4096];
    size_t len;
};

static void capture_log(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    struct log_capture *cap = user;
    size_t n = strlen(msg);
    if (cap->len + n + 2 >= sizeof cap->text) {
        return;
    }
    memcpy(cap->text + cap->len, msg, n);
    cap->len += n;
    cap->text[cap->len++] = '\n';
    cap->text[cap->len] = '\0';
}

/* A read that must fail with want_status, whose log text must contain needle (a located
 * message: file:line and the diagnostic's text). */
static void expect_read_error(const char *files, odin3_status want_status, const char *needle) {
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top_name(NULL));
    struct log_capture cap = {0};
    odin3_log_set_sink(capture_log, &cap);
    odin3_status st = odin3_design_run_pass(design, "read_verilog", files);
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(want_status, st);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.text, needle), cap.text);
}

/* Runs read_verilog for the case and returns its status. */
static odin3_status read_sv(struct sv_case one) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_top_name(one.top));
    return odin3_design_run_pass(design, "read_verilog", one.files);
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

/* AST-2's range invariant of one node: a nonzero loc and end decode, and loc <= end when both
 * lie in one buffer. */
static void check_node_range(const odin3_ast *ast, odin3_srcman *sm, uint32_t node) {
    uint32_t loc = 0;
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_loc(ast, node, &loc));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_end(ast, node, &end));
    odin3_srcpos pos = {0};
    odin3_srcpos epos = {0};
    if (loc != 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_position(sm, loc, &pos));
        TEST_ASSERT_NOT_EQUAL_UINT32(0, pos.buffer);
        TEST_ASSERT_GREATER_THAN_UINT32(0, pos.line);
    }
    if (end != 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_position(sm, end, &epos));
    }
    if (loc != 0 && end != 0 && pos.buffer == epos.buffer) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(end, loc);
    }
}

/* Counts the nodes of kind (and sub, when sub >= 0) in the elaborated store, checking every
 * node's source range on the way (AST-2). */
static uint32_t count_kind(const odin3_ast *ast, uint32_t kind, int want_sub) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_NOT_NULL(sm);
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_node_end(ast, &end));
    TEST_ASSERT_GREATER_THAN_UINT32(1, end);
    uint32_t found = 0;
    for (uint32_t node = 1; node < end; node++) {
        uint32_t k = 0;
        uint32_t sub = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_kind(ast, node, &k));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_sub(ast, node, &sub));
        check_node_range(ast, sm, node);
        if (k == kind && (want_sub < 0 || sub == (uint32_t)want_sub)) {
            found++;
        }
    }
    return found;
}

/* True when some NET_DECL (var: false) or VAR_DECL (var: true) carries SIGNED. */
static bool has_signed_decl(const odin3_ast *ast, bool var) {
    uint32_t end = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_node_end(ast, &end));
    for (uint32_t node = 1; node < end; node++) {
        uint32_t kind = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_kind(ast, node, &kind));
        if (kind != (var ? ODIN3_AST_VAR_DECL : ODIN3_AST_NET_DECL)) {
            continue;
        }
        uint32_t flags = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_flags(ast, node, &flags));
        if ((flags & ODIN3_AST_F_NET_DECL_SIGNED) != 0) {
            return true;
        }
    }
    return false;
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv((struct sv_case){files, "top"}));
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv((struct sv_case){files, "dut"}));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    const odin3_ast *ast = elaborated_store();
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_COMB));
    TEST_ASSERT_EQUAL_UINT32(2, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_FF));
    TEST_ASSERT_EQUAL_UINT32(1, count_kind(ast, ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS_LATCH));
    /* Packed-struct member accesses are bit selects and the memory is unpacked. */
    TEST_ASSERT_GREATER_THAN_UINT32(0, count_kind(ast, ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART));
    /* The latching memory write keeps a $proc (four processes: comb, two always_ff, latch). */
    uint32_t top = top_module();
    uint32_t procs = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_get_proc_count(design, top, &procs));
    TEST_ASSERT_EQUAL_UINT32(4, procs);
    expect_provenance();
}

/* Each name is one DECLARATOR of the store. */
static void expect_declarators(const odin3_ast *ast, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(has_declarator(ast, names[i]));
    }
}

/* The interface port of leaf is flattened to "<port>.<signal>" ports, and the interface
 * instance in top to "<instance>.<signal>" nets (the declarations carry those names). */
static void expect_interface_names(const odin3_ast *ast) {
    static const char *const k_names[] = {"s.data", "s.valid", "bus.data", "bus.valid"};
    expect_declarators(ast, k_names, sizeof k_names / sizeof k_names[0]);
}

static void test_sv_interface(void) {
    fresh_design();
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_iface.sv", D);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv((struct sv_case){files, "top"}));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_module_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    const odin3_ast *ast = elaborated_store();
    expect_interface_names(ast);
    /* No generate survives elaboration (slang expanded it). */
    TEST_ASSERT_EQUAL_UINT32(0, count_kind(ast, ODIN3_AST_GEN_BLOCK, -1));
    TEST_ASSERT_EQUAL_UINT32(0, count_kind(ast, ODIN3_AST_GENERATE, -1));
    expect_provenance();
}

static void test_sv_signed(void) {
    fresh_design();
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_signed.sv", D);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_sv((struct sv_case){files, "signed_cmp"}));
    const odin3_ast *ast = elaborated_store();
    /* The module's own `wire signed` and `logic signed` keep SIGNED (not only the ports). */
    TEST_ASSERT_TRUE(has_signed_decl(ast, false));
    TEST_ASSERT_TRUE(has_signed_decl(ast, true));
}

/* A syntax error reaches the log as located text (file:line and the message), not as a bare
 * ODIN3_ERR_PARSE. */
static void test_sv_diagnostics(void) {
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_bad_syntax.sv", D);
    expect_read_error(files, ODIN3_ERR_PARSE, "sv_bad_syntax.sv:");
    expect_read_error(files, ODIN3_ERR_PARSE, "expected expression");
}

/* An explicit cast has no Odin AST node: the adapter refuses it with a located error rather
 * than emitting the untruncated operand. */
static void test_sv_refused_cast(void) {
    char files[SV_PATH_MAX];
    (void)snprintf(files, sizeof files, "%s/sv_bad_cast.sv", D);
    expect_read_error(files, ODIN3_ERR_PARSE, "sv_bad_cast.sv:");
    expect_read_error(files, ODIN3_ERR_PARSE, "explicit 4-bit cast is not supported");
}

static void test_sv_missing_file(void) {
    expect_read_error(D "/no_such_file.sv", ODIN3_ERR_IO, "cannot read");
}

/* The constructs the docs list as refused are refused, each with its own located message. */
static void test_sv_refused_constructs(void) {
    expect_read_error(D "/sv_refused_unique.sv", ODIN3_ERR_PARSE, "'unique case' is not supported");
    expect_read_error(D "/sv_refused_priority.sv", ODIN3_ERR_PARSE,
                      "'priority if' is not supported");
    expect_read_error(D "/sv_refused_timing.sv", ODIN3_ERR_PARSE,
                      "intra-assignment timing control is not supported");
    expect_read_error(D "/sv_refused_strength.sv", ODIN3_ERR_PARSE,
                      "drive strength on a continuous assignment is not supported");
    expect_read_error(D "/sv_refused_ref.sv", ODIN3_ERR_PARSE, "ref port 'x' is not supported");
}

static void test_reader_is_final(void) {
    /* Registering a second reader is refused (the seam is process-wide and final). */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_sv_reader_register(odin3_read_slang));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_sv_reader_register(NULL));
}

static void test_read_slang_arguments(void) {
    /* A live design, so each call reaches its own check instead of the NULL-design one. */
    fresh_design();
    const char *paths[1] = {"/nonexistent.sv"};
    const char *empty[1] = {""};
    odin3_elab_options auto_bb = {.auto_blackbox = true};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(NULL, 1, paths, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 0, paths, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 1, NULL, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 1, paths, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_read_slang(design, 1, empty, 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_read_slang(design, 1, paths, 1, NULL, &auto_bb));
}

int main(void) {
    UNITY_BEGIN();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sv_reader_register(odin3_read_slang));
    RUN_TEST(test_reader_is_final);
    RUN_TEST(test_sv_package);
    RUN_TEST(test_sv_types);
    RUN_TEST(test_sv_interface);
    RUN_TEST(test_sv_signed);
    RUN_TEST(test_sv_diagnostics);
    RUN_TEST(test_sv_refused_cast);
    RUN_TEST(test_sv_missing_file);
    RUN_TEST(test_sv_refused_constructs);
    RUN_TEST(test_read_slang_arguments);
    return UNITY_END();
}
