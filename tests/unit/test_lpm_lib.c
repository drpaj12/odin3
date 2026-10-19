/*
 * test_lpm_lib.c — lib/lpm.o3lib (5.1 L-1..L-4, W-3): the four LPM cells, their ports and
 * parameters as spec section 3 lists them, and LPM-structured Verilog read back as registered
 * cells.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/value.h"
#include "lpmmap_fixture.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

#if !defined(ODIN3_LIB_DIR) || !defined(ODIN3_LPM_DIR)
#error "ODIN3_LIB_DIR and ODIN3_LPM_DIR must be defined"
#endif

enum { MSG_MAX = 1024, BIG_WIDTH = 8 };

static const char *const LPM_LIB = ODIN3_LIB_DIR "/lpm.o3lib";
static const char *const READ_V = ODIN3_LPM_DIR "/read_instances.v";

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(g_design, LPM_LIB), g_msg);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
}

static const odin3_celltype_def *def_of(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(name), &str));
    TEST_ASSERT_TRUE(odin3_celltype_find(g_design, str, &id));
    return odin3_celltype_get(g_design, id);
}

static const odin3_port_def *port_of(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return &def->ports[i];
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static const odin3_param_def *param_def(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return &def->params[i];
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static const char *dflt_str(const odin3_celltype_def *def, const char *name) {
    const odin3_param_def *param = param_def(def, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_VAL_STRING, param->kind, name);
    return odin3_strtab_get(odin3_design_strtab(g_design), param->dflt.str);
}

static void expect_ports(const char *cell, const char *const *names, const odin3_dir *dirs,
                         uint32_t count) {
    const odin3_celltype_def *def = def_of(cell);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, def->n_ports, cell);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(names[i], def->ports[i].name, cell);
        TEST_ASSERT_EQUAL_INT_MESSAGE(dirs[i], def->ports[i].dir, names[i]);
    }
}

static void test_lib_registers_four_blackbox_types(void) {
    static const char *const cells[] = {"lpm_add_sub", "lpm_mult", "lpm_compare", "altsyncram"};
    for (size_t i = 0; i < sizeof cells / sizeof cells[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_GRAN_BLACKBOX, def_of(cells[i])->gran, cells[i]);
    }
    /* No other type: the generic and vtr cells are not registered by this library. */
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    TEST_ASSERT_FALSE(
        odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr("lpm_ff"), &str) &&
        odin3_celltype_find(g_design, str, &id));
}

static void test_lib_port_names_and_directions(void) {
    static const char *const add[] = {"result", "cout",  "overflow", "add_sub", "cin",
                                      "dataa",  "datab", "clock",    "clken",   "aclr"};
    static const odin3_dir add_dir[] = {ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_IN,
                                        ODIN3_DIR_IN,  ODIN3_DIR_IN,  ODIN3_DIR_IN,  ODIN3_DIR_IN,
                                        ODIN3_DIR_IN,  ODIN3_DIR_IN};
    expect_ports("lpm_add_sub", add, add_dir, 10);
    static const char *const mul[] = {"result", "dataa", "datab", "sum", "clock", "clken", "aclr"};
    static const odin3_dir mul_dir[] = {ODIN3_DIR_OUT, ODIN3_DIR_IN, ODIN3_DIR_IN, ODIN3_DIR_IN,
                                        ODIN3_DIR_IN,  ODIN3_DIR_IN, ODIN3_DIR_IN};
    expect_ports("lpm_mult", mul, mul_dir, 7);
    static const char *const cmp[] = {"alb",   "aeb",   "agb",   "aleb",  "aneb", "ageb",
                                      "dataa", "datab", "clock", "clken", "aclr"};
    static const odin3_dir cmp_dir[] = {ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_OUT,
                                        ODIN3_DIR_OUT, ODIN3_DIR_OUT, ODIN3_DIR_IN,  ODIN3_DIR_IN,
                                        ODIN3_DIR_IN,  ODIN3_DIR_IN,  ODIN3_DIR_IN};
    expect_ports("lpm_compare", cmp, cmp_dir, 11);
    static const char *const ram[] = {"clock0",    "clock1",         "address_a", "data_a",
                                      "rden_a",    "wren_a",         "q_a",       "addressstall_a",
                                      "address_b", "data_b",         "rden_b",    "wren_b",
                                      "q_b",       "addressstall_b", "clocken0",  "clocken1",
                                      "clocken2",  "clocken3",       "aclr0",     "aclr1"};
    odin3_dir ram_dir[20];
    for (size_t i = 0; i < 20; i++) {
        ram_dir[i] = (strcmp(ram[i], "q_a") == 0 || strcmp(ram[i], "q_b") == 0) ? ODIN3_DIR_OUT
                                                                                : ODIN3_DIR_IN;
    }
    expect_ports("altsyncram", ram, ram_dir, 20);
}

static void test_lib_parametric_widths(void) {
    const odin3_celltype_def *add = def_of("lpm_add_sub");
    TEST_ASSERT_EQUAL_STRING("lpm_width", port_of(add, "dataa")->width_param);
    TEST_ASSERT_EQUAL_STRING("lpm_width", port_of(add, "result")->width_param);
    TEST_ASSERT_NULL(port_of(add, "cin")->width_param);
    const odin3_celltype_def *mul = def_of("lpm_mult");
    TEST_ASSERT_EQUAL_STRING("lpm_widtha", port_of(mul, "dataa")->width_param);
    TEST_ASSERT_EQUAL_STRING("lpm_widthb", port_of(mul, "datab")->width_param);
    TEST_ASSERT_EQUAL_STRING("lpm_widths", port_of(mul, "sum")->width_param);
    TEST_ASSERT_EQUAL_STRING("lpm_widthp", port_of(mul, "result")->width_param);
    const odin3_celltype_def *cmp = def_of("lpm_compare");
    TEST_ASSERT_EQUAL_STRING("lpm_width", port_of(cmp, "datab")->width_param);
    const odin3_celltype_def *ram = def_of("altsyncram");
    TEST_ASSERT_EQUAL_STRING("widthad_a", port_of(ram, "address_a")->width_param);
    TEST_ASSERT_EQUAL_STRING("width_a", port_of(ram, "data_a")->width_param);
    TEST_ASSERT_EQUAL_STRING("width_a", port_of(ram, "q_a")->width_param);
    TEST_ASSERT_EQUAL_STRING("widthad_b", port_of(ram, "address_b")->width_param);
    TEST_ASSERT_EQUAL_STRING("width_b", port_of(ram, "data_b")->width_param);
    TEST_ASSERT_EQUAL_STRING("width_b", port_of(ram, "q_b")->width_param);
}

static void test_lib_param_kinds_and_defaults(void) {
    const odin3_celltype_def *add = def_of("lpm_add_sub");
    TEST_ASSERT_EQUAL_STRING("lpm_add_sub", dflt_str(add, "lpm_type"));
    TEST_ASSERT_EQUAL_INT64(1, param_def(add, "lpm_width")->dflt.i);
    TEST_ASSERT_EQUAL_STRING("UNUSED", dflt_str(add, "lpm_direction"));
    TEST_ASSERT_EQUAL_STRING("SIGNED", dflt_str(add, "lpm_representation"));
    TEST_ASSERT_EQUAL_INT64(0, param_def(add, "lpm_pipeline")->dflt.i);
    TEST_ASSERT_EQUAL_STRING("UNUSED", dflt_str(add, "lpm_hint"));
    const odin3_celltype_def *mul = def_of("lpm_mult");
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", dflt_str(mul, "lpm_representation"));
    TEST_ASSERT_EQUAL_INT64(1, param_def(mul, "lpm_widths")->dflt.i);
    const odin3_celltype_def *cmp = def_of("lpm_compare");
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", dflt_str(cmp, "lpm_representation"));
    TEST_ASSERT_EQUAL_STRING("lpm_compare", dflt_str(cmp, "lpm_type"));
    const odin3_celltype_def *ram = def_of("altsyncram");
    TEST_ASSERT_EQUAL_STRING("Cyclone V", dflt_str(ram, "intended_device_family"));
    TEST_ASSERT_EQUAL_STRING("SINGLE_PORT", dflt_str(ram, "operation_mode"));
    TEST_ASSERT_EQUAL_STRING("NEW_DATA_NO_NBE_READ",
                             dflt_str(ram, "read_during_write_mode_port_a"));
    TEST_ASSERT_EQUAL_STRING("DONT_CARE", dflt_str(ram, "read_during_write_mode_mixed_ports"));
    TEST_ASSERT_EQUAL_STRING("UNREGISTERED", dflt_str(ram, "outdata_reg_b"));
    TEST_ASSERT_EQUAL_INT64(1, param_def(ram, "width_byteena_a")->dflt.i);
}

/* True when the port `port` of cell type `cell` carries the clock flag. */
typedef struct portref {
    const char *cell;
    const char *port;
} portref;

static bool port_is_clock(portref ref) {
    const char *cell = ref.cell;
    const char *port = ref.port;
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(cell), &str));
    TEST_ASSERT_TRUE(odin3_celltype_find(g_design, str, &id));
    const odin3_techlib_cell *lib = odin3_techlib_cell_get(g_design, id);
    TEST_ASSERT_NOT_NULL(lib);
    const odin3_celltype_def *def = def_of(cell);
    for (uint32_t idx = 0; idx < def->n_ports; idx++) {
        if (strcmp(def->ports[idx].name, port) == 0) {
            return lib->ports[idx].clock;
        }
    }
    TEST_FAIL_MESSAGE(port);
    return false;
}

static void test_lib_clock_flags(void) {
    static const struct {
        const char *cell;
        const char *port;
    } clocks[] = {{"lpm_add_sub", "clock"},
                  {"lpm_mult", "clock"},
                  {"lpm_compare", "clock"},
                  {"altsyncram", "clock0"},
                  {"altsyncram", "clock1"}};
    for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(port_is_clock((portref){clocks[i].cell, clocks[i].port}),
                                 clocks[i].port);
    }
    TEST_ASSERT_FALSE(port_is_clock((portref){"lpm_add_sub", "clken"}));
}

static odin3_module *module_with(const char *type) {
    for (uint32_t i = 1; i < odin3_design_module_end(g_design); i++) {
        odin3_module *mod = odin3_module_get(g_design, (odin3_module_id){i});
        if (mod != NULL && lm_count(mod, type) > 0) {
            return mod;
        }
    }
    return NULL;
}

static void test_read_verilog_resolves_instances(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(g_design, "read_verilog", READ_V),
                                  g_msg);
    odin3_module *top = module_with("lpm_add_sub");
    TEST_ASSERT_NOT_NULL(top);
    static const char *const types[] = {"lpm_add_sub", "lpm_compare", "lpm_mult", "altsyncram"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, lm_count(top, types[i]), types[i]);
    }
    odin3_node_id add = lm_nth(top, "lpm_add_sub", 0);
    TEST_ASSERT_EQUAL_INT64(BIG_WIDTH, lm_param_int(top, add, "lpm_width"));
    TEST_ASSERT_EQUAL_STRING("SUB", lm_param_str(top, add, "lpm_direction"));
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", lm_param_str(top, add, "lpm_representation"));
    TEST_ASSERT_EQUAL_UINT32(BIG_WIDTH, lm_port_width(top, add, "dataa"));
    odin3_node_id mul = lm_nth(top, "lpm_mult", 0);
    TEST_ASSERT_EQUAL_INT64(16, lm_param_int(top, mul, "lpm_widthp"));
    TEST_ASSERT_EQUAL_UINT32(16, lm_port_width(top, mul, "result"));
    odin3_node_id ram = lm_nth(top, "altsyncram", 0);
    TEST_ASSERT_EQUAL_STRING("Stratix IV", lm_param_str(top, ram, "intended_device_family"));
    TEST_ASSERT_EQUAL_STRING("SINGLE_PORT", lm_param_str(top, ram, "operation_mode"));
    TEST_ASSERT_EQUAL_UINT32(4, lm_port_width(top, ram, "address_a"));
    TEST_ASSERT_EQUAL_UINT32(BIG_WIDTH, lm_port_width(top, ram, "q_a"));
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(g_design, opts));
}

static void test_explicit_instances_survive_opt(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(g_design, "read_verilog", READ_V),
                                  g_msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(g_design, "proc", ""), g_msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_design_run_pass(g_design, "opt", ""), g_msg);
    odin3_module *top = module_with("lpm_add_sub");
    TEST_ASSERT_NOT_NULL(top);
    static const char *const types[] = {"lpm_add_sub", "lpm_compare", "lpm_mult", "altsyncram"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, lm_count(top, types[i]), types[i]);
    }
}

static void test_read_without_lib_fails(void) {
    odin3_design *bare = odin3_design_create();
    TEST_ASSERT_NOT_NULL(bare);
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(bare, "read_verilog", READ_V));
    TEST_ASSERT_TRUE(g_errors > 0);
    odin3_design_destroy(bare);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lib_registers_four_blackbox_types);
    RUN_TEST(test_lib_port_names_and_directions);
    RUN_TEST(test_lib_parametric_widths);
    RUN_TEST(test_lib_param_kinds_and_defaults);
    RUN_TEST(test_lib_clock_flags);
    RUN_TEST(test_read_verilog_resolves_instances);
    RUN_TEST(test_explicit_instances_survive_opt);
    RUN_TEST(test_read_without_lib_fails);
    return UNITY_END();
}
