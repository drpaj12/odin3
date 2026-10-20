/*
 * test_pm_generic_vtr.c — 4.2 Task 8: the generic blocks of real architectures. For each of the
 * two sweep archs (COFFE 22nm, k4_N4_90nm: both have no generic model, so the phase is a no-op
 * there), one with generic models (timing/k4_frac_N4_tileable_fracff_ckbuf_40nm.xml: io,
 * frac_lut4, dff, dffr, dffrn, ckbuf) and zeroasic/z1000: a design with one instance of every
 * registered generic model, written from the record's ports and widths into a mkdtemp file, binds
 * and maps with no error and no warning. Skips (exit 77) without $VTR_ROOT.
 */
#include "frontends/arch/arch.h"
#include "ir/design.h"
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

enum { LOG_TEXT = 16384, PATH_CAP = 1024, DIR_CAP = 64, SKIP_CODE = 77 };

static char log_text[LOG_TEXT];
static char g_dir[DIR_CAP];
static odin3_design *g_design;
static unsigned g_errors, g_warnings;

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    g_errors += level == ODIN3_LOG_ERROR ? 1U : 0U;
    g_warnings += level == ODIN3_LOG_WARN ? 1U : 0U;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    g_errors = g_warnings = 0;
    odin3_log_set_sink(text_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
    (void)snprintf(g_dir, sizeof g_dir, "%s/odin3-pm-generic-vtr-XXXXXX",
                   getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
}

void tearDown(void) {
    odin3_design_destroy(g_design);
    odin3_log_set_sink(NULL, NULL);
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/gen.v", g_dir);
    (void)unlink(path);
    (void)rmdir(g_dir);
}

static bool is_generic(const odin3_arch_model *model) {
    return model->registered && !model->is_template;
}

static uint32_t width_of(const odin3_arch_mport *port) {
    return port->max_width != 0 ? port->max_width : 1U; /* arch_lib.c: an unused port is 1 bit */
}

/* One instance of the model on its own top-level ports. */
static void add_instance(odin3_strbuf *ports, odin3_strbuf *body, const odin3_arch_model *model,
                         uint32_t index) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(body, "  %s u%u (", model->name, index));
    for (uint32_t i = 0; i < model->n_ports; i++) {
        const odin3_arch_mport *port = &model->ports[i];
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(ports, "    %s [%u:0] %c%u_%s,\n",
                                           port->is_out ? "output" : "input", width_of(port) - 1U,
                                           port->is_out ? 'o' : 'i', index, port->name));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(body, "%s.%s(%c%u_%s)", i != 0 ? ", " : "", port->name,
                                           port->is_out ? 'o' : 'i', index, port->name));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(body, ");\n"));
}

/* Writes the design of one instance per generic model into g_dir/gen.v; returns their number. */
static uint32_t write_design(const odin3_arch *arch, char *path, size_t cap) {
    odin3_strbuf ports;
    odin3_strbuf body;
    odin3_strbuf_init(&ports);
    odin3_strbuf_init(&body);
    uint32_t generic = 0;
    for (uint32_t i = 0; i < odin3_arch_model_count(arch); i++) {
        const odin3_arch_model *model = odin3_arch_model_at(arch, i);
        if (is_generic(model)) {
            add_instance(&ports, &body, model, generic++);
        }
    }
    (void)snprintf(path, cap, "%s/gen.v", g_dir);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fprintf(fp, "module gen_top (\n%s    input unused_in,\n    output unused_out\n);\n%s",
                  ports.data != NULL ? ports.data : "", body.data != NULL ? body.data : "");
    (void)fprintf(fp, "  assign unused_out = unused_in;\nendmodule\n");
    (void)fclose(fp);
    odin3_strbuf_free(&ports);
    odin3_strbuf_free(&body);
    return generic;
}

/* The check of one arch: every registered generic model binds, and partial_map maps. */
static void check_arch(const char *rel, uint32_t want_generic) {
    const char *root = getenv("VTR_ROOT");
    if (root == NULL) {
        TEST_IGNORE_MESSAGE("VTR_ROOT is not set");
        return;
    }
    char xml[PATH_CAP];
    (void)snprintf(xml, sizeof xml, "%s/vtr_flow/arch/%s", root, rel);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(g_design, "read_arch", odin3_bytes_cstr(xml)), log_text);
    char path[PATH_CAP];
    uint32_t generic = write_design(odin3_design_arch(g_design), path, sizeof path);
    TEST_ASSERT_EQUAL_UINT32(want_generic, generic);
    g_warnings = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(g_design, "read_verilog", odin3_bytes_cstr(path)), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(g_design, "proc", (odin3_bytes){NULL, 0}), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(g_design, "partial_map", (odin3_bytes){NULL, 0}), log_text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, g_errors, log_text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, g_warnings, log_text);
    char want[96];
    (void)snprintf(want, sizeof want, "%u instances of %u generic blocks bound", generic, generic);
    TEST_ASSERT_TRUE_MESSAGE(generic == 0 || strstr(log_text, want) != NULL, log_text);
}

static void test_coffe_sweep_arch_has_no_generic_model(void) {
    check_arch("COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml", 0);
}

static void test_k4_n4_sweep_arch_has_no_generic_model(void) {
    check_arch("timing/k4_N4_90nm.xml", 0);
}

static void test_k4_frac_fracff_blocks_bind(void) {
    check_arch("timing/k4_frac_N4_tileable_fracff_ckbuf_40nm.xml", 6);
}

static void test_z1000_blocks_bind(void) {
    check_arch("zeroasic/z1000/z1000.xml", 4);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_coffe_sweep_arch_has_no_generic_model);
    RUN_TEST(test_k4_n4_sweep_arch_has_no_generic_model);
    RUN_TEST(test_k4_frac_fracff_blocks_bind);
    RUN_TEST(test_z1000_blocks_bind);
    int failures = UNITY_END();
    if (failures != 0) {
        return 1;
    }
    return getenv("VTR_ROOT") == NULL ? SKIP_CODE : 0;
}
