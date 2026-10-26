/*
 * test_ast_abi.c — the AST and source-manager ABI (ABI v4): the §3.1 macro example and a small
 * module built and read back through odin3.h only; links only libodin3.so, like a plugin.
 */
#include "odin3/odin3.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_CAP = 1 << 14, RUN = 7 };

static odin3_design *design;
static odin3_srcman *sm;
static odin3_ast *ast;
static char log_text[LOG_CAP];
static size_t log_len;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%d %s\n", (int)level, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

static void reset_log(void) {
    log_text[0] = '\0';
    log_len = 0;
}

void setUp(void) {
    reset_log();
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_new_ast(design, ODIN3_AST_FORM_PARSED, RUN, &ast));
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers ------------------------------------------------------------------------------ */

static uint32_t intern(const char *text) {
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_intern(ast, text, &name));
    return name;
}

/* A FILE buffer holding text, its lines recorded, as a source reader would add it. */
static uint32_t add_file(const char *name, const char *text) {
    odin3_srcfile_spec spec = {.name = intern(name), .len = (uint32_t)strlen(text)};
    uint32_t buf = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_file(sm, &spec, &buf));
    for (uint32_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_line(sm, buf, i + 1));
        }
    }
    return buf;
}

static uint32_t in(uint32_t buf, uint32_t offset) {
    uint32_t loc = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_loc(sm, buf, offset, &loc));
    return loc;
}

/* A 1-based line:col in a FILE buffer. */
typedef struct text_pos {
    uint32_t buffer, line, col;
} text_pos;

/* The loc of pos.line:pos.col in text, the bytes of FILE buffer pos.buffer. */
static uint32_t at(const char *text, text_pos pos) {
    uint32_t offset = 0;
    for (uint32_t seen = 1; seen < pos.line; offset++) {
        seen += text[offset] == '\n' ? 1U : 0U;
    }
    return in(pos.buffer, offset + pos.col - 1);
}

static uint32_t expand(odin3_expansion_spec spec) {
    uint32_t buf = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_expansion(sm, &spec, &buf));
    return buf;
}

static const char *format(uint32_t loc) {
    const char *text = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_format(sm, loc, &text));
    return text;
}

static uint32_t file_loc(uint32_t loc) {
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_file_loc(sm, loc, &out));
    return out;
}

static uint32_t exp_loc(uint32_t loc) {
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_expansion_loc(sm, loc, &out));
    return out;
}

static uint32_t spelling(uint32_t loc) {
    uint32_t out = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_spelling(sm, loc, &out));
    return out;
}

/* --- version ------------------------------------------------------------------------------- */

static void test_abi_version_is_7(void) {
    TEST_ASSERT_EQUAL_UINT32(7, odin3_abi_version()); /* v1.2.0, provenance chain (ABI 7) */
    TEST_ASSERT_EQUAL_INT(69, ODIN3_AST_KIND_COUNT);
    TEST_ASSERT_EQUAL_INT(1, ODIN3_AST_UNIT);
    TEST_ASSERT_EQUAL_INT(68, ODIN3_AST_MINTYPMAX);
    TEST_ASSERT_EQUAL_INT(24, ODIN3_AST_BINARY_COUNT);
    TEST_ASSERT_EQUAL_INT(32768, ODIN3_AST_MAX_DEPTH);
}

/* --- the §3.1 example through the ABI ----------------------------------------------------- */

static const char F_V[] = "`define INNER(p) (p + 1)\n"
                          "`define OUTER(q) `INNER(q * 2)\n"
                          "assign y = `OUTER(xx);\n";

typedef struct fv {
    uint32_t file, e1, a1, e2, a2, a3;
} fv;

static fv build_fv(void) {
    fv ex;
    ex.file = add_file("f.v", F_V);
    uint32_t file = ex.file;
    ex.e1 = expand((odin3_expansion_spec){.kind = ODIN3_SRCBUF_EXPANSION,
                                          .name = intern("OUTER"),
                                          .parent = at(F_V, (text_pos){file, 3, 12}),
                                          .parent_end = at(F_V, (text_pos){file, 3, 22}),
                                          .def = at(F_V, (text_pos){file, 2, 18}),
                                          .len = 13});
    ex.a1 = expand((odin3_expansion_spec){.kind = ODIN3_SRCBUF_MACRO_ARG,
                                          .name = intern("OUTER"),
                                          .parent = in(ex.e1, 7),
                                          .parent_end = in(ex.e1, 8),
                                          .def = at(F_V, (text_pos){file, 3, 19}),
                                          .len = 2});
    ex.e2 = expand((odin3_expansion_spec){.kind = ODIN3_SRCBUF_EXPANSION,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e1, 0),
                                          .parent_end = in(ex.e1, 13),
                                          .def = at(F_V, (text_pos){file, 1, 18}),
                                          .len = 7});
    ex.a2 = expand((odin3_expansion_spec){.kind = ODIN3_SRCBUF_MACRO_ARG,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e2, 1),
                                          .parent_end = in(ex.e2, 2),
                                          .def = in(ex.a1, 0),
                                          .len = 2});
    ex.a3 = expand((odin3_expansion_spec){.kind = ODIN3_SRCBUF_MACRO_ARG,
                                          .name = intern("INNER"),
                                          .parent = in(ex.e2, 1),
                                          .parent_end = in(ex.e2, 2),
                                          .def = in(ex.e1, 8),
                                          .len = 4});
    return ex;
}

static void test_srcman_example_decodes(void) {
    fv ex = build_fv();
    uint32_t file = ex.file;
    TEST_ASSERT_EQUAL_UINT32(1, ex.file);
    TEST_ASSERT_EQUAL_UINT32(6, ex.a3); /* buffer IDs in creation order */
    TEST_ASSERT_EQUAL_UINT32(1, in(file, 0));
    TEST_ASSERT_EQUAL_UINT32(sizeof F_V + 1, in(ex.e1, 0)); /* the file reserves len + 1 */
    uint32_t xx = in(ex.a2, 0);
    uint32_t star = in(ex.a3, 1);
    uint32_t paren = in(ex.e2, 0);
    TEST_ASSERT_EQUAL_UINT32(in(ex.a1, 0), spelling(xx));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 3, 19}), spelling(spelling(xx)));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 2, 27}), spelling(spelling(star)));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 1, 18}), spelling(paren));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 3, 12}), exp_loc(xx));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 3, 12}), exp_loc(star));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 3, 19}), file_loc(xx));
    TEST_ASSERT_EQUAL_UINT32(at(F_V, (text_pos){file, 3, 12}), file_loc(star));
    TEST_ASSERT_EQUAL_UINT32(0, spelling(0));
    TEST_ASSERT_EQUAL_UINT32(0, file_loc(0));
    odin3_srcpos pos = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_position(sm, xx, &pos));
    TEST_ASSERT_EQUAL_UINT32(file, pos.buffer);
    TEST_ASSERT_EQUAL_UINT32(3, pos.line);
    TEST_ASSERT_EQUAL_UINT32(19, pos.col);
    TEST_ASSERT_EQUAL_STRING("f.v:3:19", format(xx));
    TEST_ASSERT_EQUAL_STRING("f.v:3:12", format(star));
    TEST_ASSERT_EQUAL_STRING("<unknown>:0:0", format(0));
    TEST_ASSERT_FALSE(log_has(" 0 ")); /* no error logged */
}

static void test_srcman_segments_and_bad_arguments(void) {
    fv ex = build_fv();
    odin3_segment seg = {.stream = ex.file, .out_offset = 0, .loc = in(ex.file, 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_segment(sm, &seg));
    seg = (odin3_segment){.stream = ex.file, .out_offset = 11, .loc = in(ex.e2, 0)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_segment(sm, &seg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_segment(sm, &seg));
    seg.stream = ex.e1; /* not a project FILE buffer */
    seg.out_offset = 20;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_segment(sm, &seg));
    uint32_t past = in(ex.a3, 4) + 1;
    uint32_t out = 99;
    const char *text = "unchanged";
    odin3_srcpos pos = {.line = 99};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_file_loc(sm, past, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_expansion_loc(sm, past, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_spelling(sm, past, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_format(sm, past, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_position(sm, 0, &pos));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_position(sm, past, &pos));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_loc(sm, ex.a3 + 1, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_loc(sm, ex.e1, 14, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_file_loc(NULL, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_file_loc(sm, 1, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_get_format(NULL, 1, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_line(sm, ex.e1, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_line(NULL, ex.file, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_file(sm, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_srcman_build_expansion(sm, NULL, &out));
    TEST_ASSERT_EQUAL_UINT32(99, out);
    TEST_ASSERT_EQUAL_STRING("unchanged", text);
    TEST_ASSERT_EQUAL_UINT32(99, pos.line);
    TEST_ASSERT_TRUE(log_has("odin3_srcman_get_file_loc: invalid argument"));
    TEST_ASSERT_TRUE(log_has("odin3_srcman_get_position: invalid argument"));
    TEST_ASSERT_TRUE(log_has("odin3_srcman_get_format: invalid argument"));
}

/* --- a small module through the ABI --------------------------------------------------------- */

static const char M_V[] = "module m(input [3:0] a, output y); // c\n"
                          "  (* keep *) assign y = &a;\n"
                          "  localparam real r = 1.5;\n"
                          "  specify endspecify\n"
                          "endmodule\n";

static const char SPECIFY[] = "specify endspecify";

/* The module's node IDs, in creation order (IDs are dense from 1). */
enum {
    N_MSB = 1,
    N_LSB,
    N_RANGE,
    N_DECL_A,
    N_PORT_A,
    N_DECL_Y,
    N_PORT_Y,
    N_PORTS,
    N_Y,
    N_A,
    N_AND,
    N_NET_ASSIGN,
    N_ASSIGN,
    N_ATTR,
    N_REAL,
    N_DECL_R,
    N_PARAM,
    N_SPECIFY,
    N_MODULE,
    N_UNIT,
    N_END
};

typedef struct mod {
    uint32_t file;
} mod;

static uint32_t node(odin3_ast_spec spec, const uint32_t *ids, uint32_t n) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, ids, n, &id));
    return id;
}

static uint32_t number(uint32_t loc, const char *bits) {
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, loc, 0, bits, &payload));
    return node((odin3_ast_spec){.kind = ODIN3_AST_NUMBER,
                                 .sub = ODIN3_AST_NUMBER_DEC,
                                 .flags = ODIN3_AST_F_NUMBER_SIGNED,
                                 .loc = loc,
                                 .end = loc + 1,
                                 .payload = payload},
                NULL, 0);
}

static uint32_t ident(uint32_t loc, const char *name) {
    return node(
        (odin3_ast_spec){.kind = ODIN3_AST_IDENT, .loc = loc, .end = loc + 1, .name = intern(name)},
        NULL, 0);
}

/* `input [3:0] a, output y` as a LIST built on the pending stack. */
static void build_ports(uint32_t file) {
    uint32_t range[] = {number(at(M_V, (text_pos){file, 1, 17}), "11"),
                        number(at(M_V, (text_pos){file, 1, 19}), "0")};
    node((odin3_ast_spec){.kind = ODIN3_AST_RANGE, .loc = at(M_V, (text_pos){file, 1, 16})}, range,
         2);
    uint32_t none[] = {0};
    node((odin3_ast_spec){.kind = ODIN3_AST_DECLARATOR,
                          .loc = at(M_V, (text_pos){file, 1, 22}),
                          .name = intern("a")},
         none, 1);
    uint32_t port_a[] = {N_RANGE, N_DECL_A};
    node((odin3_ast_spec){.kind = ODIN3_AST_PORT_DECL,
                          .sub = ODIN3_AST_PORT_DECL_INPUT,
                          .flags = ODIN3_AST_F_PORT_DECL_IN_HEADER,
                          .loc = at(M_V, (text_pos){file, 1, 10}),
                          .end = at(M_V, (text_pos){file, 1, 23})},
         port_a, 2);
    node((odin3_ast_spec){.kind = ODIN3_AST_DECLARATOR,
                          .loc = at(M_V, (text_pos){file, 1, 32}),
                          .name = intern("y")},
         none, 1);
    uint32_t port_y[] = {0, N_DECL_Y};
    node((odin3_ast_spec){.kind = ODIN3_AST_PORT_DECL,
                          .sub = ODIN3_AST_PORT_DECL_OUTPUT,
                          .flags = ODIN3_AST_F_PORT_DECL_IN_HEADER,
                          .loc = at(M_V, (text_pos){file, 1, 25})},
         port_y, 2);
    uint32_t mark = 99;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_mark(ast, &mark));
    TEST_ASSERT_EQUAL_UINT32(0, mark);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_push(ast, N_PORT_A));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_push(ast, N_PORT_Y));
    uint32_t list = 0;
    odin3_ast_spec spec = {.kind = ODIN3_AST_LIST, .loc = at(M_V, (text_pos){file, 1, 9})};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_marked(ast, &spec, mark, &list));
    TEST_ASSERT_EQUAL_UINT32(N_PORTS, list);
}

/* `(* keep *) assign y = &a;` */
static void build_assign(uint32_t file) {
    ident(at(M_V, (text_pos){file, 2, 21}), "y");
    ident(at(M_V, (text_pos){file, 2, 26}), "a");
    uint32_t operand[] = {N_A};
    node((odin3_ast_spec){.kind = ODIN3_AST_UNARY,
                          .sub = ODIN3_AST_UNARY_AND,
                          .loc = at(M_V, (text_pos){file, 2, 25}),
                          .end = at(M_V, (text_pos){file, 2, 27})},
         operand, 1);
    uint32_t sides[] = {N_Y, N_AND};
    node((odin3_ast_spec){.kind = ODIN3_AST_NET_ASSIGN, .loc = at(M_V, (text_pos){file, 2, 21})},
         sides, 2);
    uint32_t assign[] = {0, 0, N_NET_ASSIGN};
    node((odin3_ast_spec){.kind = ODIN3_AST_CONT_ASSIGN,
                          .loc = at(M_V, (text_pos){file, 2, 14}),
                          .end = at(M_V, (text_pos){file, 2, 28})},
         assign, 3);
    uint32_t none[] = {0};
    node((odin3_ast_spec){.kind = ODIN3_AST_ATTR,
                          .loc = at(M_V, (text_pos){file, 2, 6}),
                          .name = intern("keep")},
         none, 1);
    uint32_t attrs[] = {N_ATTR};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_attach(ast, N_ASSIGN, attrs, 1));
}

/* `localparam real r = 1.5;` and `specify endspecify` */
static void build_items(uint32_t file) {
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_real(ast, 1.5, &payload));
    node((odin3_ast_spec){.kind = ODIN3_AST_REAL,
                          .loc = at(M_V, (text_pos){file, 3, 23}),
                          .payload = payload},
         NULL, 0);
    uint32_t init[] = {N_REAL};
    node((odin3_ast_spec){.kind = ODIN3_AST_DECLARATOR,
                          .loc = at(M_V, (text_pos){file, 3, 19}),
                          .name = intern("r")},
         init, 1);
    uint32_t param[] = {0, N_DECL_R};
    node((odin3_ast_spec){.kind = ODIN3_AST_PARAM_DECL,
                          .sub = ODIN3_AST_PARAM_DECL_LOCALPARAM,
                          .flags = ODIN3_AST_DT_REAL << ODIN3_AST_F_PARAM_DECL_DT_SHIFT,
                          .loc = at(M_V, (text_pos){file, 3, 3})},
         param, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_build_text(ast, SPECIFY, (uint32_t)strlen(SPECIFY), &payload));
    node((odin3_ast_spec){.kind = ODIN3_AST_SPECIFY_BLOCK,
                          .loc = at(M_V, (text_pos){file, 4, 3}),
                          .payload = payload},
         NULL, 0);
}

static mod build_module(void) {
    mod built = {add_file("m.v", M_V)};
    uint32_t file = built.file;
    build_ports(file);
    build_assign(file);
    build_items(file);
    uint32_t kids[] = {0, N_PORTS, N_ASSIGN, N_PARAM, N_SPECIFY};
    node((odin3_ast_spec){.kind = ODIN3_AST_MODULE,
                          .loc = at(M_V, (text_pos){file, 1, 1}),
                          .end = at(M_V, (text_pos){file, 5, 10}),
                          .name = intern("m")},
         kids, 5);
    uint32_t unit[] = {N_MODULE};
    node((odin3_ast_spec){.kind = ODIN3_AST_UNIT, .loc = in(file, 0), .name = intern("m.v")}, unit,
         1);
    const char *note = "// c";
    odin3_ast_comment_spec comment = {.loc = at(M_V, (text_pos){file, 1, 36}),
                                      .end = at(M_V, (text_pos){file, 1, 40}),
                                      .kind = ODIN3_AST_COMMENT_LINE,
                                      .text_ptr = note,
                                      .text_len = 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_comment(ast, &comment));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    return built;
}

static uint32_t get_u32(odin3_status (*fn)(const odin3_ast *, uint32_t, uint32_t *), uint32_t id) {
    uint32_t out = 0xFFFFFFFFU;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fn(ast, id, &out));
    return out;
}

static const char *get_str(odin3_status (*fn)(const odin3_ast *, uint32_t, const char **),
                           uint32_t id) {
    const char *out = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fn(ast, id, &out));
    return out;
}

static uint32_t child(uint32_t id, uint32_t index) {
    uint32_t out = 0xFFFFFFFFU;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_child(ast, id, index, &out));
    return out;
}

static void test_module_store_fields(void) {
    build_module();
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_node_end(ast, &value));
    TEST_ASSERT_EQUAL_UINT32(N_END, value);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_form(ast, &value));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_FORM_PARSED, value);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_run(ast, &value));
    TEST_ASSERT_EQUAL_UINT32(RUN, value);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_root_count(ast, &value));
    TEST_ASSERT_EQUAL_UINT32(1, value);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_root_at(ast, 0, &value));
    TEST_ASSERT_EQUAL_UINT32(N_UNIT, value);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_root_at(ast, 1, &value));
    TEST_ASSERT_EQUAL_UINT32(N_UNIT, value);
}

static void test_module_node_fields(void) {
    mod built = build_module();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MODULE, get_u32(odin3_ast_get_kind, N_MODULE));
    TEST_ASSERT_EQUAL_STRING("MODULE", get_str(odin3_ast_get_kind_name, N_MODULE));
    TEST_ASSERT_EQUAL_STRING("m", get_str(odin3_ast_get_name, N_MODULE));
    TEST_ASSERT_EQUAL_STRING("", get_str(odin3_ast_get_name, N_PORTS));
    TEST_ASSERT_EQUAL_STRING("", get_str(odin3_ast_get_sub_name, N_MODULE));
    TEST_ASSERT_EQUAL_UINT32(at(M_V, (text_pos){built.file, 1, 1}),
                             get_u32(odin3_ast_get_loc, N_MODULE));
    TEST_ASSERT_EQUAL_UINT32(at(M_V, (text_pos){built.file, 5, 10}),
                             get_u32(odin3_ast_get_end, N_MODULE));
    TEST_ASSERT_EQUAL_UINT32(0, get_u32(odin3_ast_get_end, N_PORT_Y));
    TEST_ASSERT_EQUAL_UINT32(5, get_u32(odin3_ast_get_nchild, N_MODULE));
    uint32_t want[] = {0, N_PORTS, N_ASSIGN, N_PARAM, N_SPECIFY};
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], child(N_MODULE, i));
    }
    TEST_ASSERT_EQUAL_UINT32(N_MODULE, child(N_UNIT, 0));
    TEST_ASSERT_EQUAL_UINT32(N_PORT_A, child(N_PORTS, 0));
    TEST_ASSERT_EQUAL_UINT32(N_PORT_Y, child(N_PORTS, 1));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_PORT_DECL_INPUT, get_u32(odin3_ast_get_sub, N_PORT_A));
    TEST_ASSERT_EQUAL_STRING("input", get_str(odin3_ast_get_sub_name, N_PORT_A));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_F_PORT_DECL_IN_HEADER,
                             get_u32(odin3_ast_get_flags, N_PORT_A));
    TEST_ASSERT_EQUAL_STRING("&", get_str(odin3_ast_get_sub_name, N_AND));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DT_REAL << ODIN3_AST_F_PARAM_DECL_DT_SHIFT,
                             get_u32(odin3_ast_get_flags, N_PARAM));
    TEST_ASSERT_EQUAL_STRING("m.v:2:25", format(get_u32(odin3_ast_get_loc, N_AND)));
    TEST_ASSERT_EQUAL_STRING("m.v:1:1", format(get_u32(odin3_ast_get_loc, N_UNIT)));
    TEST_ASSERT_EQUAL_STRING("m.v", get_str(odin3_ast_get_name, N_UNIT));
}

static void test_module_payloads_and_attributes(void) {
    build_module();
    TEST_ASSERT_EQUAL_UINT32(0, get_u32(odin3_ast_get_nchild, N_MSB));
    const char *bits = NULL;
    uint32_t width = 99;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_number_str(ast, N_MSB, &bits, &width));
    TEST_ASSERT_EQUAL_STRING("11", bits);
    TEST_ASSERT_EQUAL_UINT32(0, width);
    double real = 0.0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_real(ast, N_REAL, &real));
    TEST_ASSERT_TRUE(real == 1.5);
    const char *text = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_text(ast, N_SPECIFY, &text, &len));
    TEST_ASSERT_EQUAL_UINT32(strlen(SPECIFY), len);
    TEST_ASSERT_EQUAL_MEMORY(SPECIFY, text, len);
    TEST_ASSERT_EQUAL_UINT32(1, get_u32(odin3_ast_get_attr_count, N_ASSIGN));
    TEST_ASSERT_EQUAL_UINT32(0, get_u32(odin3_ast_get_attr_count, N_MODULE));
    uint32_t attr = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_attr_at(ast, N_ASSIGN, 0, &attr));
    TEST_ASSERT_EQUAL_UINT32(N_ATTR, attr);
    TEST_ASSERT_EQUAL_STRING("keep", get_str(odin3_ast_get_name, attr));
    /* wrong payload kinds */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_get_number_str(ast, N_REAL, &bits, &width));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_real(ast, N_MSB, &real));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_text(ast, N_REAL, &text, &len));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_attr_at(ast, N_ASSIGN, 1, &attr));
    TEST_ASSERT_EQUAL_STRING("11", bits);
    TEST_ASSERT_TRUE(real == 1.5);
    TEST_ASSERT_EQUAL_UINT32(N_ATTR, attr);
}

static void test_number_new_str_round_trip(void) {
    uint32_t loc = add_file("n.v", "4'b10x?\n");
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, in(loc, 0), 4, "10x?", &payload));
    uint32_t id = node((odin3_ast_spec){.kind = ODIN3_AST_NUMBER,
                                        .sub = ODIN3_AST_NUMBER_BIN,
                                        .loc = in(loc, 0),
                                        .payload = payload},
                       NULL, 0);
    const char *bits = NULL;
    uint32_t width = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_number_str(ast, id, &bits, &width));
    TEST_ASSERT_EQUAL_STRING("10x?", bits);
    TEST_ASSERT_EQUAL_UINT32(4, width);
    /* one has_question flag per literal: every z then reads back as ? */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, in(loc, 0), 0, "z?1", &payload));
    id = node(
        (odin3_ast_spec){.kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_BIN, .payload = payload},
        NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_number_str(ast, id, &bits, &width));
    TEST_ASSERT_EQUAL_STRING("??1", bits);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, in(loc, 0), 0, "1z", &payload));
    id = node(
        (odin3_ast_spec){.kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_BIN, .payload = payload},
        NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_get_number_str(ast, id, &bits, &width));
    TEST_ASSERT_EQUAL_STRING("1z", bits);
    payload = 77;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_new_str(ast, in(loc, 0), 0, "", &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_new_str(ast, in(loc, 0), 0, "1X", &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_number_new_str(ast, in(loc, 0), 0, NULL, &payload));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_ast_number_new_str(ast, in(loc, 0),
                                                   (uint32_t)ODIN3_AST_MAX_NUMBER_BITS + 1, "1",
                                                   &payload));
    TEST_ASSERT_EQUAL_UINT32(77, payload);
    TEST_ASSERT_TRUE(log_has("n.v:1:1: error: literal wider than"));
}

/* --- builder misuse ------------------------------------------------------------------------- */

static void test_builder_misuse(void) {
    uint32_t out = 99;
    odin3_ast_spec spec = {.kind = ODIN3_AST_IDENT}; /* IDENT requires a name */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, &spec, NULL, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(NULL, &spec, NULL, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, NULL, NULL, 0, &out));
    spec.name = intern("x");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, &spec, NULL, 0, NULL));
    uint32_t kids[] = {1};
    spec.kind = ODIN3_AST_LIST;
    spec.name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, &spec, NULL, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, &spec, kids, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_push(ast, 1)); /* not made */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_marked(ast, &spec, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_mark(ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_unwind(NULL, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_attach(ast, 1, NULL, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_text(ast, NULL, 3, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_real(ast, 1.0, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_intern(ast, NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_comment(ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_finish(NULL));
    TEST_ASSERT_EQUAL_UINT32(99, out);
    /* unwind: pushes go, the store's nodes stay */
    spec = (odin3_ast_spec){.kind = ODIN3_AST_IDENT, .name = intern("x")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, NULL, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_push(ast, out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_push(ast, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_unwind(ast, 0));
    uint32_t mark = 99;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_mark(ast, &mark));
    TEST_ASSERT_EQUAL_UINT32(0, mark);
    /* sealed */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_node(ast, &spec, NULL, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_push(ast, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_build_real(ast, 1.0, &out));
    TEST_ASSERT_EQUAL_UINT32(1, out);
}

/* --- readers: NULL handle or output, node 0 or past the end --------------------------------- */

typedef odin3_status (*u32_reader)(const odin3_ast *, uint32_t, uint32_t *);
typedef odin3_status (*str_reader)(const odin3_ast *, uint32_t, const char **);

typedef struct u32_case {
    u32_reader fn;
    const char *name;
} u32_case;

typedef struct str_case {
    str_reader fn;
    const char *name;
} str_case;

static void assert_u32_rejects(u32_case rc) {
    uint32_t out = 42;
    char want[128];
    (void)snprintf(want, sizeof want, "%s: invalid argument", rc.name);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(NULL, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, N_END, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, 1, NULL));
    TEST_ASSERT_EQUAL_UINT32(42, out);
    TEST_ASSERT_TRUE_MESSAGE(log_has(want), rc.name);
}

static void assert_str_rejects(str_case rc) {
    const char *out = "unchanged";
    char want[128];
    (void)snprintf(want, sizeof want, "%s: invalid argument", rc.name);
    reset_log();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(NULL, 1, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, N_END, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, rc.fn(ast, 1, NULL));
    TEST_ASSERT_EQUAL_STRING("unchanged", out);
    TEST_ASSERT_TRUE_MESSAGE(log_has(want), rc.name);
}

static void test_readers_reject_bad_arguments(void) {
    build_module();
    static const u32_case u32_cases[] = {
        {odin3_ast_get_kind, "odin3_ast_get_kind"},
        {odin3_ast_get_sub, "odin3_ast_get_sub"},
        {odin3_ast_get_flags, "odin3_ast_get_flags"},
        {odin3_ast_get_loc, "odin3_ast_get_loc"},
        {odin3_ast_get_end, "odin3_ast_get_end"},
        {odin3_ast_get_nchild, "odin3_ast_get_nchild"},
        {odin3_ast_get_attr_count, "odin3_ast_get_attr_count"},
    };
    static const str_case str_cases[] = {
        {odin3_ast_get_name, "odin3_ast_get_name"},
        {odin3_ast_get_kind_name, "odin3_ast_get_kind_name"},
        {odin3_ast_get_sub_name, "odin3_ast_get_sub_name"},
    };
    for (size_t i = 0; i < sizeof u32_cases / sizeof u32_cases[0]; i++) {
        assert_u32_rejects(u32_cases[i]);
    }
    for (size_t i = 0; i < sizeof str_cases / sizeof str_cases[0]; i++) {
        assert_str_rejects(str_cases[i]);
    }
    uint32_t out = 42;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_child(ast, N_MODULE, 5, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_child(ast, 0, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_child(ast, N_END, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_attr_at(ast, 0, 0, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_node_end(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_form(ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_run(NULL, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_root_count(ast, NULL));
    TEST_ASSERT_EQUAL_UINT32(42, out);
    double real = 2.0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_real(ast, N_END, &real));
    TEST_ASSERT_TRUE(real == 2.0);
    const char *text = "unchanged";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_text(ast, 0, &text, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_get_number_str(ast, 0, &text, &out));
    TEST_ASSERT_EQUAL_STRING("unchanged", text);
}

/* --- design handles and keep levels --------------------------------------------------------- */

static void test_design_holds_stores(void) {
    odin3_ast *elab = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, RUN, &elab));
    odin3_ast *other = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, RUN, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_new_ast(design, 3, RUN, &other));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_new_ast(design, ODIN3_AST_FORM_PARSED, 0, &other));
    TEST_ASSERT_NULL(other);
    odin3_ast *found = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, RUN, ODIN3_AST_FORM_ELABORATED, &found));
    TEST_ASSERT_EQUAL_PTR(elab, found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, RUN + 1, ODIN3_AST_FORM_PARSED, &found));
    TEST_ASSERT_NULL(found);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_ast(design, RUN, 0, &found));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_ast(NULL, RUN, ODIN3_AST_FORM_PARSED, &found));
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_ast_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_ast_at(design, 0, &found));
    TEST_ASSERT_EQUAL_PTR(ast, found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_ast_at(design, 1, &found));
    TEST_ASSERT_EQUAL_PTR(elab, found);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_ast_at(design, 2, &found));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_ast_count(design, NULL));
    odin3_srcman *again = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &again));
    TEST_ASSERT_EQUAL_PTR(sm, again);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_srcman(NULL, &again));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_srcman(design, NULL));
}

static void test_keep_levels(void) {
    odin3_keep_ast level = ODIN3_KEEP_AST_ALL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, level);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ALL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ALL, level);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_set_keep_ast(design, (odin3_keep_ast)3));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_set_keep_ast(NULL, ODIN3_KEEP_AST_NONE));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_keep_ast(design, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ALL, level);
    TEST_ASSERT_TRUE(log_has("odin3_design_set_keep_ast: invalid argument"));
}

/* A script naming a pass that wants the AST raises the keep level before anything runs. */
static void test_script_with_ast_pass_raises_keep_level(void) {
    odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
    /* the pass fails (no such directory), but the level was raised before the script ran */
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_IO, odin3_design_run_script(design, "write_verilog_ast /nonexistent/x.v", src));
    odin3_keep_ast level = ODIN3_KEEP_AST_NONE;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ELABORATED, level);
    /* raised, never lowered */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ALL));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_IO, odin3_design_run_script(design, "write_verilog_ast /nonexistent/x.v", src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_ALL, level);
    /* a script without such a pass leaves it alone */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_NONE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_script(design, "check", src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_keep_ast(design, &level));
    TEST_ASSERT_EQUAL_INT(ODIN3_KEEP_AST_NONE, level);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_abi_version_is_7);
    RUN_TEST(test_srcman_example_decodes);
    RUN_TEST(test_srcman_segments_and_bad_arguments);
    RUN_TEST(test_module_store_fields);
    RUN_TEST(test_module_node_fields);
    RUN_TEST(test_module_payloads_and_attributes);
    RUN_TEST(test_number_new_str_round_trip);
    RUN_TEST(test_builder_misuse);
    RUN_TEST(test_readers_reject_bad_arguments);
    RUN_TEST(test_design_holds_stores);
    RUN_TEST(test_keep_levels);
    RUN_TEST(test_script_with_ast_pass_raises_keep_level);
    return UNITY_END();
}
