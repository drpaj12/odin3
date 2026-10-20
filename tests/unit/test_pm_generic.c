/*
 * test_pm_generic.c — 4.2 generic hard blocks (spec B-1 to B-7; plan Task 7): the bind check and
 * report of `partial_map` over tests/unit/data/arch/generic_blocks.xml and the micros of
 * tests/micro_pm. Each test runs the pass script on a fresh design and reads the log it printed.
 */
#include "frontends/arch/arch.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
#endif

enum { LOG_TEXT = 16384, PATH_CAP = 600, DIR_CAP = 64, BLIF_CAP = 65536 };

#define ARCH_XML ODIN3_SOURCE_DIR "/tests/unit/data/arch/generic_blocks.xml"
#define MICRO(name) ODIN3_SOURCE_DIR "/tests/micro_pm/" name ".v"

static char log_text[LOG_TEXT];
static char g_dir[DIR_CAP];
static odin3_design *g_design;

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    const char *tag = "I ";
    if (level == ODIN3_LOG_ERROR) {
        tag = "E ";
    } else if (level == ODIN3_LOG_WARN) {
        tag = "W ";
    }
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 4 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s%s\n", tag, msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
    (void)snprintf(g_dir, sizeof g_dir, "%s/odin3-pm-generic-XXXXXX",
                   getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
}

static void remove_file(const char *name) {
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/%s", g_dir, name);
    (void)unlink(path);
}

void tearDown(void) {
    odin3_design_destroy(g_design);
    odin3_log_set_sink(NULL, NULL);
    remove_file("out.blif");
    (void)rmdir(g_dir);
}

static odin3_status pass(odin3_design *design, const char *name, const char *args) {
    return odin3_pass_run(design, name, odin3_bytes_cstr(args));
}

/* read_arch, read_verilog (with extra options) of the micro, proc: ready for partial_map. */
static odin3_status load(const char *micro, const char *vopts) {
    char args[PATH_CAP];
    odin3_status st = pass(g_design, "read_arch", ARCH_XML);
    (void)snprintf(args, sizeof args, "%s %s", vopts, micro);
    st = st == ODIN3_OK ? pass(g_design, "read_verilog", args) : st;
    return st == ODIN3_OK ? pass(g_design, "proc", "") : st;
}

static bool has(const char *text) {
    return strstr(log_text, text) != NULL;
}

/* How many instances of the cell type named name the design's modules hold. */
static uint32_t instances_of(const char *name) {
    odin3_celltype_id type = {0};
    uint32_t str = 0;
    if (odin3_design_intern(g_design, odin3_bytes_cstr(name), &str) != ODIN3_OK ||
        !odin3_celltype_find(g_design, str, &type)) {
        return 0;
    }
    return odin3_celltype_instances(g_design, type);
}

/* write_blif --vtr of the design into g_dir/out.blif; returns its text (a static buffer). */
static const char *write_vtr(odin3_design *design) {
    static char blif[BLIF_CAP];
    char path[PATH_CAP];
    char args[PATH_CAP + 16];
    (void)snprintf(path, sizeof path, "%s/out.blif", g_dir);
    (void)snprintf(args, sizeof args, "--vtr %s", path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(design, "write_blif", args));
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    size_t got = fread(blif, 1, sizeof blif - 1, fp);
    blif[got] = '\0';
    (void)fclose(fp);
    return blif;
}

static void assert_has(const char *text, const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, needle), needle);
}

/* (a) a binding design maps and writes a .subckt with every formal bit; the INFO line counts. */
static void test_binding_design_maps_and_writes_every_formal_bit(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_ok"), ""));
    log_text[0] = '\0'; /* read_arch warns about the model that is not registered */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text,
                                "I partial_map: 3 instances of 3 generic blocks bound (mult_8 1, "
                                "dpram_128x8 1, pcie 1)"));
    TEST_ASSERT_FALSE(has("W "));
    TEST_ASSERT_FALSE(has("E "));
    const char *blif = write_vtr(g_design);
    assert_has(blif, ".subckt mult_8 ");
    assert_has(blif, "a[7]=");
    assert_has(blif, "out[15]=");
    assert_has(blif, ".subckt dpram_128x8 ");
    assert_has(blif, "waddr[6]=");
    assert_has(blif, ".subckt pcie ");
    assert_has(blif, "tx[3]=");
}

/* IR-7b: the generic block survives write_blif --vtr and read_blif with the arch record. */
static void test_round_trip_through_blif(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_ok"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    (void)write_vtr(g_design);
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/out.blif", g_dir);
    odin3_design *again = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(again, "read_arch", ARCH_XML));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(again, "read_blif", path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(again, "partial_map", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "3 instances of 3 generic blocks bound"));
    odin3_design_destroy(again);
}

/* (b) B-3: an unknown port is the located elaboration error. */
static void test_unknown_port_is_the_located_elaboration_error(void) {
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, load(MICRO("generic_unknown_port"), ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "generic_unknown_port.v:"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "error: cell type 'mult_8' has no port 'foo'"));
}

/* (c) B-5: an unconnected input is tied to gnd with the warning. */
static void test_missing_input_is_tied_to_gnd(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_missing_input"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    assert_has(log_text, "partial_map: instance 'u_mul' of 'mult_8': input port 'b' is not "
                         "connected or is x; tied to gnd");
    assert_has(log_text, "1 instances of 1 generic blocks bound (mult_8 1)");
    const char *blif = write_vtr(g_design);
    assert_has(blif, "b[0]=gnd");
    assert_has(blif, "b[7]=gnd");
}

/* (d) B-4: a wider connection is truncated with the elaboration warning (existing). */
static void test_wider_connection_is_truncated_with_a_warning(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_wide"), ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "port 'a' of instance 'u_mul' (module 'mult_8') is 8 "
                                          "bits; the connection of 10 bits is truncated"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_FALSE(has("tied to gnd"));
}

/* (e) B-6: an unknown black box with an arch record is a located error; the IR is unchanged. */
static void test_unknown_block_with_a_record_is_an_error(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_unknown_block"), "--auto-blackbox"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, pass(g_design, "partial_map", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "generic_unknown_block.v:"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "error: partial_map: instance 'u_foo' of 'foo': 'foo' is not a model of "
                         "architecture 'generic_blocks' (VPR would reject the netlist)"));
    TEST_ASSERT_EQUAL_UINT32(1, instances_of("foo"));
}

/* (f) the same with --allow-unknown-blocks: a warning, exit 0. */
static void test_allow_unknown_blocks_is_a_warning(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_unknown_block"), "--auto-blackbox"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", "--allow-unknown-blocks"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "warning: partial_map: instance 'u_foo' of 'foo': 'foo' is not a model "
                         "of architecture 'generic_blocks' (VPR would reject the netlist)"));
    TEST_ASSERT_FALSE(has("E "));
}

/* (g) no arch record: nothing is checked. */
static void test_without_a_record_nothing_is_checked(void) {
    char args[PATH_CAP];
    (void)snprintf(args, sizeof args, "--auto-blackbox %s", MICRO("generic_unknown_block"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_verilog", args));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "proc", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_FALSE(has("not a model of architecture"));
    TEST_ASSERT_FALSE(has("generic blocks bound"));
}

/* (h) a model with a non-identifier name is not registered; its instance is the existing
 * unresolved-module error. */
static void test_non_identifier_model_is_not_registered(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_arch", ARCH_XML));
    const odin3_arch *arch = odin3_design_arch(g_design);
    const odin3_arch_model *odd = odin3_arch_model_find(arch, "odd-name");
    TEST_ASSERT_TRUE(odd != NULL && !odd->registered && arch->n_registered == 3);
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, pass(g_design, "read_verilog", MICRO("generic_odd_name")));
    assert_has(log_text, "E ");
}

/* Review M2: an unconnected clock is not tied to gnd; the writer's E3b refuses it. */
static void test_unconnected_clock_is_left_for_the_writer(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_no_clock"), ""));
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_FALSE(has("tied to gnd"));
    char args[PATH_CAP];
    (void)snprintf(args, sizeof args, "--vtr %s/out.blif", g_dir);
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, pass(g_design, "write_blif", args));
    assert_has(log_text, "clock input 'clk' is unconnected or x");
}

/* Review M5: a port with only some bits x is not "not connected". */
static void test_partial_x_is_left_alone(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_partial_x"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_FALSE(has("tied to gnd"));
}

/* Review M1: a whole port of explicit x is tied to gnd and the warning says so. */
static void test_explicit_x_warning_says_x(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_explicit_x"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    assert_has(log_text, "input port 'b' is not connected or is x; tied to gnd");
}

/* Review H2: a library black box (lpm) is not an unknown block, with an arch loaded. */
static void test_lpm_cell_is_not_an_unknown_block(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_arch", ARCH_XML));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_techlib", ODIN3_LIB_DIR "/lpm.o3lib"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_verilog", MICRO("generic_lpm")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "proc", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    TEST_ASSERT_FALSE(has("not a model of architecture"));
    TEST_ASSERT_FALSE(has("E "));
    TEST_ASSERT_FALSE(has("generic blocks bound"));
}

/* Review H1 (3E E6): an lpm cell is still refused by write_blif --vtr, with an arch that does not
 * have it and with none. */
static void test_lpm_cell_is_still_refused_by_the_vtr_writer(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_arch", ARCH_XML));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_techlib", ODIN3_LIB_DIR "/lpm.o3lib"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "read_verilog", MICRO("generic_lpm")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "proc", ""));
    char args[PATH_CAP];
    (void)snprintf(args, sizeof args, "--vtr %s/out.blif", g_dir);
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, pass(g_design, "write_blif", args));
    assert_has(log_text, "'lpm_compare' is not a hard cell of a loaded tech library");
    odin3_design *bare = odin3_design_create();
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(bare, "read_techlib", ODIN3_LIB_DIR "/lpm.o3lib"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(bare, "read_verilog", MICRO("generic_lpm")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(bare, "proc", ""));
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, pass(bare, "write_blif", args));
    assert_has(log_text, "'lpm_compare' is not a hard cell of a loaded tech library");
    odin3_design_destroy(bare);
}

/* Review M4: the `.subckt` count and the typed list agree for generic blocks. */
static void test_summary_counts_generic_blocks(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, load(MICRO("generic_ok"), ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pass(g_design, "partial_map", ""));
    log_text[0] = '\0';
    (void)write_vtr(g_design);
    assert_has(log_text, "3 .subckt (other 3)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_binding_design_maps_and_writes_every_formal_bit);
    RUN_TEST(test_round_trip_through_blif);
    RUN_TEST(test_unknown_port_is_the_located_elaboration_error);
    RUN_TEST(test_missing_input_is_tied_to_gnd);
    RUN_TEST(test_wider_connection_is_truncated_with_a_warning);
    RUN_TEST(test_unknown_block_with_a_record_is_an_error);
    RUN_TEST(test_allow_unknown_blocks_is_a_warning);
    RUN_TEST(test_without_a_record_nothing_is_checked);
    RUN_TEST(test_non_identifier_model_is_not_registered);
    RUN_TEST(test_unconnected_clock_is_left_for_the_writer);
    RUN_TEST(test_partial_x_is_left_alone);
    RUN_TEST(test_explicit_x_warning_says_x);
    RUN_TEST(test_lpm_cell_is_not_an_unknown_block);
    RUN_TEST(test_lpm_cell_is_still_refused_by_the_vtr_writer);
    RUN_TEST(test_summary_counts_generic_blocks);
    return UNITY_END();
}
