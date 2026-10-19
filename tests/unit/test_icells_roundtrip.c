/*
 * test_icells_roundtrip.c — 5.2 T-3: every built-in cell type the Verilog writer writes as a module
 * instance reads back, through `read_verilog -icells`, as a cell of the same type with the same
 * parameters and the same pin counts per port. One cell of every eligible registered type (all
 * but modules, ports, processes, hard and black-box types) is built at the type's default
 * parameters, written, and read into a second design. A type that the writer does not write as an
 * instance (an assign or an always block) must be in k_not_instance below; a type that is written
 * as an instance must not be: the test fails on either mismatch, so a new type is covered or
 * named here.
 */
#include "backends/verilog/writer.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 512, TEXT_MAX = 1 << 20, NAME_MAX_LEN = 64 };

/* The types backends/verilog/writer.c writes as assigns, always blocks or literals (BUILTINS). */
static const char *const k_not_instance[] = {
    "$_CONST0_",    "$_CONST1_",    "$_CONSTX_",   "$_CONSTZ_",  "$_BUF_",      "$_NOT_",
    "$_AND_",       "$_OR_",        "$_XOR_",      "$_NAND_",    "$_NOR_",      "$_XNOR_",
    "$_MUX_",       "$sop",         "$lut",        "$_DFF_P_",   "$_DFF_N_",    "$_DLATCH_P_",
    "$_DLATCH_N_",  "$_FF_",        "$add",        "$sub",       "$mul",        "$div",
    "$mod",         "$and",         "$or",         "$xor",       "$shl",        "$shr",
    "$sshr",        "$eq",          "$ne",         "$lt",        "$le",         "$gt",
    "$ge",          "$not",         "$reduce_and", "$reduce_or", "$reduce_xor", "$neg",
    "$reduce_xnor", "$reduce_bool", "$logic_not",  "$xnor",      "$logic_and",  "$logic_or",
    "$mux",         "$pmux",        "$tribuf",     "$dff",       "$dffe",       "$adff",
    "$sdff",
};

static char g_dir[PATH_BUF - 16];
static char g_path[PATH_BUF];
static char g_text[TEXT_MAX];
static char g_errors[1 << 12];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && strlen(g_errors) + strlen(msg) + 2 < sizeof g_errors) {
        strcat(g_errors, msg);
        strcat(g_errors, "\n");
    }
}

void setUp(void) {
    g_errors[0] = '\0';
    const char *base = getenv("TMPDIR");
    (void)snprintf(g_dir, sizeof g_dir, "%s/odin3_icells_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
    (void)snprintf(g_path, sizeof g_path, "%s/rt.v", g_dir);
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    (void)remove(g_path);
    (void)rmdir(g_dir);
}

static bool listed(const char *name) {
    for (size_t i = 0; i < sizeof k_not_instance / sizeof k_not_instance[0]; i++) {
        if (strcmp(k_not_instance[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool eligible(const odin3_celltype_def *def) {
    return def->gran != ODIN3_GRAN_MODULE && def->gran != ODIN3_GRAN_PORT &&
           def->gran != ODIN3_GRAN_HARD && def->gran != ODIN3_GRAN_BLACKBOX &&
           (def->flags & ODIN3_CT_PROCESS) == 0;
}

static uint32_t intern(odin3_design *design, const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &id));
    return id;
}

/* One cell of def at its default parameters, every port on fresh nets; false when the registry
 * refuses the defaults (reported by the caller with the type name). */
static bool add_default_cell(odin3_design *design, odin3_module *mod, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    odin3_value params[32];
    TEST_ASSERT_TRUE(def->n_params <= sizeof params / sizeof params[0]);
    for (uint32_t k = 0; k < def->n_params; k++) {
        params[k] = def->params[k].dflt;
    }
    odin3_netvec *vecs = calloc(def->n_ports + 1U, sizeof *vecs);
    TEST_ASSERT_NOT_NULL(vecs);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        uint32_t width = odin3_celltype_port_width(design, type, params, i);
        odin3_net_id *nets = calloc(width + 1U, sizeof *nets);
        TEST_ASSERT_NOT_NULL(nets);
        for (uint32_t b = 0; b < width; b++) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_net_create(mod, 0, odin3_module_prov(mod), &nets[b]));
        }
        vecs[i] = (odin3_netvec){nets, width};
    }
    odin3_node_spec spec = {.type = type, .params = params, .n_params = def->n_params};
    odin3_node_id node;
    odin3_status st = odin3_node_create_connected(mod, &spec, vecs, &node);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        free((void *)vecs[i].nets);
    }
    free(vecs);
    return st == ODIN3_OK;
}

static bool value_equal(const odin3_design *da, const odin3_value *a, const odin3_design *db,
                        const odin3_value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case ODIN3_VAL_INT:
        return a->i == b->i;
    case ODIN3_VAL_STRING:
        return strcmp(odin3_strtab_get(odin3_design_strtab(da), a->str),
                      odin3_strtab_get(odin3_design_strtab(db), b->str)) == 0;
    case ODIN3_VAL_BITS:
        return a->len == b->len && (a->len == 0 || memcmp(a->bits, b->bits, a->len) == 0);
    default:
        return false;
    }
}

static const odin3_module *first_module(odin3_design *design) {
    return odin3_module_get(design, (odin3_module_id){1});
}

/* The first live node of type `name` in mod. */
static odin3_node_id find_node(const odin3_design *design, const odin3_module *mod,
                               const char *name) {
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
        if (odin3_node_live(mod, node) && def != NULL && strcmp(def->name, name) == 0) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static void slurp_output(void) {
    FILE *file = fopen(g_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(g_text, 1, sizeof g_text - 1U, file);
    g_text[got] = '\0';
    (void)fclose(file);
}

/* True when the text has an instance line `  \<name> ` . */
static bool written_as_instance(const char *name) {
    char key[NAME_MAX_LEN + 8];
    (void)snprintf(key, sizeof key, "\n  \\%s ", name);
    return strstr(g_text, key) != NULL;
}

static void test_every_instance_type_reads_back(void) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, intern(design, "top"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    uint32_t built = 0;
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        odin3_celltype_id type;
        TEST_ASSERT_TRUE(
            odin3_celltype_find(design, intern(design, odin3_builtin_celltypes[i]->name), &type));
        if (!eligible(odin3_builtin_celltypes[i])) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(add_default_cell(design, mod, type),
                                 odin3_builtin_celltypes[i]->name);
        built++;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(20, built);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_verilog_write(design, g_path, NULL), g_errors);
    slurp_output();

    odin3_design *back = odin3_design_create();
    TEST_ASSERT_NOT_NULL(back);
    char args[PATH_BUF + 16];
    (void)snprintf(args, sizeof args, "-icells %s", g_path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(back, "read_verilog", odin3_bytes_cstr(args)), g_errors);
    const odin3_module *after = first_module(back);
    uint32_t covered = 0;
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        const odin3_celltype_def *def = odin3_builtin_celltypes[i];
        if (!eligible(def)) {
            continue;
        }
        bool as_instance = written_as_instance(def->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(!listed(def->name), as_instance,
                                      def->name); /* the list and the writer agree */
        if (!as_instance) {
            continue;
        }
        odin3_node_id was = find_node(design, mod, def->name);
        odin3_node_id now = find_node(back, after, def->name);
        TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, now.v, def->name);
        for (uint32_t k = 0; k < def->n_params; k++) {
            TEST_ASSERT_TRUE_MESSAGE(value_equal(design, odin3_node_param(mod, was, k), back,
                                                 odin3_node_param(after, now, k)),
                                     def->params[k].name);
        }
        for (uint32_t p = 0; p < def->n_ports; p++) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(odin3_node_port(mod, was, p).count,
                                             odin3_node_port(after, now, p).count,
                                             def->ports[p].name);
        }
        covered++;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(15, covered);
    (void)printf("icells round trip: %u instance types read back of %u built\n", (unsigned)covered,
                 (unsigned)built);
    odin3_design_destroy(back);
    odin3_design_destroy(design);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_instance_type_reads_back);
    return UNITY_END();
}
