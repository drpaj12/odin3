/*
 * test_techlib_libs.c — the shipped libraries (lib/vtr.o3lib, lib/generic_gates.o3lib) and the
 * goldens' black boxes resolved against lib/vtr.o3lib by the BLIF reader and writer (IR-7b).
 */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ODIN3_LIB_DIR) || !defined(ODIN3_TECHLIB_FIXTURES) || !defined(ODIN3_BLIF_FIXTURES)
#error "ODIN3_LIB_DIR, ODIN3_TECHLIB_FIXTURES and ODIN3_BLIF_FIXTURES must be defined"
#endif

enum { MSG_MAX = 1024, PATH_LEN = 512, TEXT_MAX = 1 << 16, MAX_FIXTURES = 256 };

static const char *const VTR = ODIN3_LIB_DIR "/vtr.o3lib";
static const char *const GATES = ODIN3_LIB_DIR "/generic_gates.o3lib";
static const char *const IN_PATH = "odin3_libs_test_in.blif";
static const char *const OUT_PATH = "odin3_libs_test_out.blif";

static odin3_design *g_design;
static char g_msg[MSG_MAX];
static unsigned g_errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
        g_errors++;
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    g_errors = 0;
    odin3_log_set_sink(sink, NULL);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
    (void)remove(IN_PATH);
    (void)remove(OUT_PATH);
}

static void fresh_design(void) {
    odin3_design_destroy(g_design);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
}

static void load(const char *path) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(g_design, path), g_msg);
}

static odin3_celltype_id type_of(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(name), &str)) {
        (void)odin3_celltype_find(g_design, str, &id);
    }
    return id;
}

static const odin3_celltype_def *def_of(const char *name) {
    const odin3_celltype_def *def = odin3_celltype_get(g_design, type_of(name));
    TEST_ASSERT_NOT_NULL_MESSAGE(def, name);
    return def;
}

static void write_input(const char *text) {
    FILE *file = fopen(IN_PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static const char *slurp(const char *path, char *buf) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t len = fread(buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[len] = '\0';
    return buf;
}

static void expect_check_clean(void) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_check_design(g_design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE}));
}

/* Port `index` of def: name and direction. */
static void expect_port(const odin3_celltype_def *def, uint32_t index, const char *name,
                        odin3_dir dir) {
    TEST_ASSERT_TRUE(index < def->n_ports);
    TEST_ASSERT_EQUAL_STRING(name, def->ports[index].name);
    TEST_ASSERT_EQUAL_INT(dir, def->ports[index].dir);
}

/* --- the libraries ------------------------------------------------------------------------- */

static void test_generic_gates(void) {
    load(GATES);
    static const char *const cells[] = {
        "BUF",  "INV",   "AND2", "AND3", "AND4", "OR2",     "OR3",     "OR4",  "NAND2", "NOR2",
        "XOR2", "XNOR2", "MUX2", "DFFP", "DFFN", "DLATCHP", "DLATCHN", "TIE0", "TIE1"};
    for (size_t i = 0; i < sizeof cells / sizeof cells[0]; i++) {
        const odin3_celltype_def *def = def_of(cells[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_GRAN_BIT, def->gran, cells[i]);
        const odin3_techlib_cell *lib = odin3_techlib_cell_get(g_design, type_of(cells[i]));
        TEST_ASSERT_NOT_NULL(lib);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, lib->n_fns + lib->n_seqs, cells[i]);
        for (uint32_t port = 0; port < def->n_ports; port++) {
            TEST_ASSERT_EQUAL_UINT32(
                1, odin3_celltype_port_width(g_design, type_of(cells[i]), NULL, port));
        }
    }
    expect_port(def_of("MUX2"), 2, "S", ODIN3_DIR_IN);
    TEST_ASSERT_EQUAL_UINT32(1, def_of("TIE1")->n_ports);
    const odin3_techlib_cell *dffn = odin3_techlib_cell_get(g_design, type_of("DFFN"));
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_NEGEDGE, dffn->seqs[0].trigger);
    TEST_ASSERT_TRUE(dffn->ports[1].clock);
    const odin3_techlib_cell *latch = odin3_techlib_cell_get(g_design, type_of("DLATCHN"));
    TEST_ASSERT_EQUAL_INT(ODIN3_TECHLIB_LOW, latch->seqs[0].trigger);
}

/* A cell's expected ports: names in order, the first n_in inputs and the rest outputs. */
typedef struct ports_want {
    const char *cell;
    const char *const *names;
    uint32_t n_ports;
    uint32_t n_in;
} ports_want;

static void expect_ports(ports_want want) {
    const odin3_celltype_def *def = def_of(want.cell);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, def->gran);
    TEST_ASSERT_EQUAL_UINT32(want.n_ports, def->n_ports);
    for (uint32_t i = 0; i < want.n_ports; i++) {
        expect_port(def, i, want.names[i], i < want.n_in ? ODIN3_DIR_IN : ODIN3_DIR_OUT);
    }
}

/* vtr.o3lib: Odin II's port names and order. */
static void test_vtr_ports(void) {
    load(VTR);
    static const char *const adder[] = {"a", "b", "cin", "cout", "sumout"};
    expect_ports((ports_want){"adder", adder, 5, 3});
    static const char *const mul[] = {"a", "b", "out"};
    expect_ports((ports_want){"multiply", mul, 3, 2});
    static const char *const ram[] = {"clk", "data", "addr", "we", "out"};
    expect_ports((ports_want){"single_port_ram", ram, 5, 4});
    static const char *const dpram[] = {"clk", "data2", "data1", "addr2", "addr1",
                                        "we2", "we1",   "out2",  "out1"};
    expect_ports((ports_want){"dual_port_ram", dpram, 9, 7});
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(def_of("adder")->ports[i].scalar);
    }
}

/* vtr.o3lib: the parameter defaults are the widths every golden uses. */
static void test_vtr_parameters(void) {
    load(VTR);
    const odin3_celltype_def *mul = def_of("multiply");
    TEST_ASSERT_EQUAL_UINT32(2, mul->n_params);
    TEST_ASSERT_EQUAL_STRING("A_WIDTH", mul->params[0].name);
    TEST_ASSERT_EQUAL_INT64(36, mul->params[0].dflt.i);
    TEST_ASSERT_EQUAL_INT64(36, mul->params[1].dflt.i);
    const odin3_value dflt[2] = {mul->params[0].dflt, mul->params[1].dflt};
    TEST_ASSERT_EQUAL_UINT32(72, odin3_celltype_port_width(g_design, type_of("multiply"), dflt, 2));
    TEST_ASSERT_EQUAL_STRING("ADDR_WIDTH", def_of("dual_port_ram")->params[0].name);
    TEST_ASSERT_EQUAL_INT64(15, def_of("dual_port_ram")->params[0].dflt.i);
    TEST_ASSERT_EQUAL_INT64(1, def_of("dual_port_ram")->params[1].dflt.i);
    TEST_ASSERT_EQUAL_INT64(15, def_of("single_port_ram")->params[0].dflt.i);
    TEST_ASSERT_NOT_NULL(odin3_techlib_cell_get(g_design, type_of("dual_port_ram"))->memory);
}

/* The name of port `port` of cell `cell`. */
static const char *port_name(const char *cell, uint32_t port) {
    return def_of(cell)->ports[port].name;
}

/* Read i drives the i-th memory-driven output (positional rule): dual_port_ram's reads are listed
 * in its output order, out2 then out1, so each read's address is the one of its output. */
static void test_vtr_reads_pair_with_outputs(void) {
    load(VTR);
    const odin3_techlib_memory *mem =
        odin3_techlib_cell_get(g_design, type_of("dual_port_ram"))->memory;
    TEST_ASSERT_EQUAL_UINT32(2, mem->n_outs);
    uint32_t reads = 0;
    for (uint32_t i = 0; i < mem->n_mports; i++) {
        const odin3_techlib_memport *mport = &mem->mports[i];
        if (mport->write) {
            continue;
        }
        TEST_ASSERT_TRUE(reads < mem->n_outs);
        const char *out = port_name("dual_port_ram", mem->outs[reads]);
        const char *addr = port_name("dual_port_ram", mport->ports[1]);
        TEST_ASSERT_EQUAL_STRING(out + strlen("out"), addr + strlen("addr"));
        reads++;
    }
    TEST_ASSERT_EQUAL_UINT32(mem->n_outs, reads);
}

/* --- the goldens' black boxes -------------------------------------------------------------- */

/* The parameters every golden declaration implies, by model. */
static const int64_t *want_params(const char *model) {
    static const int64_t mul[] = {36, 36};
    static const int64_t ram[] = {15, 1};
    if (strcmp(model, "multiply") == 0) {
        return mul;
    }
    return strcmp(model, "adder") == 0 ? NULL : ram;
}

/* Reads one fixture with vtr.o3lib loaded: its model resolves to the library cell, with the
 * golden widths as parameters on the declaration and the instance; the design is check-clean and
 * writes back to text that declares the same parameters. */
static void resolve_fixture(const char *path) {
    fresh_design();
    load(VTR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(g_design, path), g_msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, odin3_design_declared_model_count(g_design), path);
    odin3_celltype_id type = odin3_design_declared_model(g_design, 0);
    const odin3_celltype_def *def = odin3_celltype_get(g_design, type);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_GRAN_HARD, def->gran, path);
    TEST_ASSERT_NOT_NULL_MESSAGE(odin3_techlib_cell_get(g_design, type), path);
    const int64_t *want = want_params(def->name);
    const odin3_value *params = odin3_design_declared_model_params(g_design, 0);
    odin3_module *top = odin3_module_get(g_design, (odin3_module_id){1});
    odin3_node_id node = {odin3_module_port_count(top) + 1};
    TEST_ASSERT_EQUAL_UINT32(type.v, odin3_node_type(top, node).v);
    for (uint32_t i = 0; want != NULL && i < def->n_params; i++) {
        TEST_ASSERT_EQUAL_INT64_MESSAGE(want[i], params[i].i, path);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(want[i], odin3_node_param(top, node, i)->i, path);
    }
    expect_check_clean();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(g_design, OUT_PATH), g_msg);
}

static int compare_names(const void *lhs, const void *rhs) {
    return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

static bool is_blif(const char *name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".blif") == 0;
}

/* The fixtures of tests/golden/techlib, sorted, into sorted[]; returns how many. */
static size_t list_fixtures(const char **sorted) {
    static char names[MAX_FIXTURES][PATH_LEN];
    DIR *dir = opendir(ODIN3_TECHLIB_FIXTURES);
    TEST_ASSERT_NOT_NULL(dir);
    size_t count = 0;
    for (struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        if (is_blif(ent->d_name)) {
            TEST_ASSERT_TRUE(count < MAX_FIXTURES);
            (void)snprintf(names[count], PATH_LEN, "%s/%s", ODIN3_TECHLIB_FIXTURES, ent->d_name);
            sorted[count] = names[count];
            count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, closedir(dir));
    qsort((void *)sorted, count, sizeof sorted[0], compare_names);
    return count;
}

/* Every distinct golden `.model … .blackbox` stanza (tools/golden-blackboxes) resolves. */
static void test_every_golden_stanza_resolves(void) {
    static const char *sorted[MAX_FIXTURES];
    size_t count = list_fixtures(sorted);
    static const char *const kinds[] = {"/adder.", "/multiply.", "/single_port_ram.",
                                        "/dual_port_ram."};
    unsigned models = 0;
    for (size_t i = 0; i < count; i++) {
        resolve_fixture(sorted[i]);
        for (unsigned k = 0; k < 4; k++) {
            models |= strstr(sorted[i], kinds[k]) != NULL ? 1U << k : 0U;
        }
    }
    TEST_ASSERT_EQUAL_UINT(0xF, models); /* each of the four hard blocks is covered */
    TEST_ASSERT_EQUAL_size_t(49, count); /* the distinct stanzas of the 2026-10-09 goldens */
}

/* Review Focus 1: `.model multiply` declared with 18-bit `a`, 9-bit `b` and 27-bit `out`
 * resolves to the library cell with A_WIDTH=18, B_WIDTH=9; the instance gets them; the writer
 * prints the same formals and the same declaration. */
static void test_multiply_parameters_from_declaration(void) {
    load(VTR);
    static char text[TEXT_MAX];
    static char out[TEXT_MAX];
    int at = snprintf(text, sizeof text,
                      ".model top\n.inputs x y\n.outputs z\n"
                      ".subckt multiply b[8]=y a[17]=x out[26]=z\n.end\n\n"
                      ".model multiply\n.inputs");
    for (int k = 0; k < 9; k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " b[%d]", k);
    }
    for (int k = 0; k < 18; k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " a[%d]", k);
    }
    at += snprintf(text + at, sizeof text - (size_t)at, "\n.outputs");
    for (int k = 0; k < 27; k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " out[%d]", k);
    }
    (void)snprintf(text + at, sizeof text - (size_t)at, "\n.blackbox\n.end\n");
    write_input(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(g_design, IN_PATH), g_msg);
    const odin3_value *params = odin3_design_declared_model_params(g_design, 0);
    TEST_ASSERT_EQUAL_INT64(18, params[0].i);
    TEST_ASSERT_EQUAL_INT64(9, params[1].i);
    odin3_module *top = odin3_module_get(g_design, (odin3_module_id){1});
    odin3_node_id node = {odin3_module_port_count(top) + 1};
    TEST_ASSERT_EQUAL_INT64(18, odin3_node_param(top, node, 0)->i);
    TEST_ASSERT_EQUAL_INT64(9, odin3_node_param(top, node, 1)->i);
    TEST_ASSERT_EQUAL_UINT32(27, odin3_node_port(top, node, 2).count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(g_design, OUT_PATH), g_msg);
    /* the writer wraps lines at 100 columns: compare with continuations joined */
    const char *got = slurp(OUT_PATH, out);
    static char joined[TEXT_MAX];
    size_t len = 0;
    for (const char *ch = got; *ch != '\0'; ch++) {
        if (ch[0] == ' ' && ch[1] == '\\' && ch[2] == '\n' && ch[3] == ' ' && ch[4] == ' ') {
            ch += 4;
        }
        joined[len++] = *ch;
    }
    joined[len] = '\0';
    TEST_ASSERT_EQUAL_STRING(text, joined);
}

/* Review Focus 2: a declaration whose widths contradict the library's expressions (out 70 bits for
 * 36x36) is a located parse error naming the port. */
static void test_multiply_contradicting_width(void) {
    load(VTR);
    static char text[TEXT_MAX];
    int at = snprintf(text, sizeof text, ".model top\n.end\n.model multiply\n.inputs");
    for (int k = 0; k < 72; k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " %c[%d]", k < 36 ? 'a' : 'b', k % 36);
    }
    at += snprintf(text + at, sizeof text - (size_t)at, "\n.outputs");
    for (int k = 0; k < 70; k++) {
        at += snprintf(text + at, sizeof text - (size_t)at, " out[%d]", k);
    }
    (void)snprintf(text + at, sizeof text - (size_t)at, "\n.blackbox\n.end\n");
    write_input(text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(g_design, IN_PATH));
    TEST_ASSERT_EQUAL_STRING("odin3_libs_test_in.blif:3: black box 'multiply' conflicts with the "
                             "registered cell type of that name: port 'out' has 70 bits, the cell "
                             "type gives it 72",
                             g_msg);
    TEST_ASSERT_EQUAL_UINT(1, g_errors);
}

/* Review Focus 5: with vtr.o3lib loaded, `$pow` (not in the library) stays an implicit black
 * box, and `multiply` used without a `.model` takes its parameters from its formals. */
static void test_implicit_and_inferred_with_library(void) {
    load(VTR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_blif_read(g_design, ODIN3_BLIF_FIXTURES "/pow.parmys.blif"), g_msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def_of("$pow")->gran);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(g_design));
    fresh_design();
    load(VTR);
    write_input(".model top\n.inputs x y\n.outputs z\n"
                ".subckt multiply a[3]=x b[1]=y out[5]=z\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(g_design, IN_PATH), g_msg);
    odin3_module *top = odin3_module_get(g_design, (odin3_module_id){1});
    odin3_node_id node = {odin3_module_port_count(top) + 1};
    TEST_ASSERT_EQUAL_INT64(4, odin3_node_param(top, node, 0)->i);
    TEST_ASSERT_EQUAL_INT64(2, odin3_node_param(top, node, 1)->i);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_node_port(top, node, 2).count);
    expect_check_clean();
    static char out[TEXT_MAX];
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(g_design, OUT_PATH), g_msg);
    TEST_ASSERT_EQUAL_STRING(".model top\n.inputs x y\n.outputs z\n"
                             ".subckt multiply a[3]=x b[1]=y out[5]=z\n.end\n",
                             slurp(OUT_PATH, out));
    fresh_design();
    load(VTR);
    write_input(".model top\n.inputs x\n.subckt multiply a[3]=x b[1]=x out[6]=x\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(g_design, IN_PATH));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_msg, "no port 'out[6]'"), g_msg);
}

/* Inferred parameters whose width expression exceeds the cap (out = 2^20 + 2^20 bits) are a
 * located parse error before any port is sized. */
static void test_inferred_expression_width_is_capped(void) {
    load(VTR);
    write_input(".model top\n.inputs x\n.subckt multiply a[1048575]=x b[1048575]=x\n.end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_blif_read(g_design, IN_PATH));
    TEST_ASSERT_EQUAL_STRING("odin3_libs_test_in.blif:3: the parameters inferred for 'multiply' "
                             "give port 'out' 2097152 bits, above 1048576",
                             g_msg);
}

static void test_missing_library_is_io_error(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_techlib_read(g_design, ODIN3_LIB_DIR "/none.o3lib"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_generic_gates);
    RUN_TEST(test_vtr_ports);
    RUN_TEST(test_vtr_parameters);
    RUN_TEST(test_vtr_reads_pair_with_outputs);
    RUN_TEST(test_every_golden_stanza_resolves);
    RUN_TEST(test_multiply_parameters_from_declaration);
    RUN_TEST(test_multiply_contradicting_width);
    RUN_TEST(test_implicit_and_inferred_with_library);
    RUN_TEST(test_inferred_expression_width_is_capped);
    RUN_TEST(test_missing_library_is_io_error);
    return UNITY_END();
}
