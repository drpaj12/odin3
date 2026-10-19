/*
 * test_abc_unconn.c — pins on Parmys's undefined net `unconn` (3D plan Task 19; spec §5.2,
 * 3E §14.1 items 2 and 4, Review Focus 1), on the fake engine and the BLIF exchange: a hard
 * block's address bits left on `unconn` stay there with the same nets and their generators, a soft
 * path from `unconn` ends on the constant 0 like VTR's, and no `.names` or `.latch` pin of the
 * written BLIF is on `unconn` afterwards.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/region.h"
#include "passes/manager.h"
#include "util/alloc.h"

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository"
#endif

enum { ADDR_BITS = 15, FIRST_OPEN = 5, X_BIT = 4, BUFFERED_BIT = 3 };

static odin3_design *design;
static odin3_module *mod;
static const char *exchange_env; /* ODIN3_ABC_EXCHANGE for the cases being run: "aig" or NULL */

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    if (exchange_env != NULL) {
        TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", exchange_env, 1));
    } else {
        TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    }
    design = odin3_design_create();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    (void)unsetenv("ODIN3_ABC_EXE");
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* ram_unconn.blif, with a $_CONSTX_ generator on the net xaddr; mod is its top. */
static odin3_node_id load_ram(void) {
    char path[AFX_JOIN];
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
    (void)snprintf(path, sizeof path, "%s/tests/unit/data/abc/ram_unconn.blif", ODIN3_SOURCE_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    mod = afx_top(design);
    odin3_net_id xaddr = odin3_module_find_net(mod, afx_intern(design, "xaddr"));
    TEST_ASSERT_TRUE(odin3_net_valid(xaddr));
    odin3_node_id gen = AFX_CELL(design, mod, "$_CONSTX_", NULL, &xaddr, 1);
    afx_check(design);
    return gen;
}

static odin3_node_id ram_node(void) {
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        const odin3_celltype_def *def =
            odin3_celltype_get(design, odin3_node_type(mod, (odin3_node_id){id}));
        if (strcmp(def->name, "single_port_ram") == 0) {
            return (odin3_node_id){id};
        }
    }
    TEST_FAIL_MESSAGE("no single_port_ram");
    return (odin3_node_id){0};
}

static odin3_net_id addr_net(odin3_node_id ram, uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(mod, ram, 2); /* clk, data, addr, we, out */
    TEST_ASSERT_EQUAL_UINT32(ADDR_BITS, pins.count);
    return odin3_pin_net(mod, (odin3_pin_id){pins.first.v + bit});
}

/* How many live nodes of the module have the cell type. */
static uint32_t count_cells(const char *type) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        found +=
            odin3_node_live(mod, node) &&
                    strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, type) == 0
                ? 1U
                : 0U;
    }
    return found;
}

/* The addr[5..14] bits stay on the one `unconn` net (named so, driven by its zero-row $sop) and
 * addr[4] on its $_CONSTX_ net, whose generator is alive: the pass renumbers nets when it
 * compacts, so the nets are recognised by what they are, not by their IDs. */
static void test_hard_block_pins_stay_open(void) {
    odin3_node_id gen = load_ram();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")), afx_errors());
    afx_check(design);
    odin3_node_id ram = ram_node();
    odin3_net_id first = addr_net(ram, FIRST_OPEN);
    TEST_ASSERT_TRUE(odin3_abc_net_undefined(mod, first));
    for (uint32_t bit = FIRST_OPEN; bit < ADDR_BITS; bit++) {
        TEST_ASSERT_EQUAL_UINT32(first.v, addr_net(ram, bit).v);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X, odin3_net_const_value(mod, addr_net(ram, X_BIT)));
    TEST_ASSERT_EQUAL_UINT32(1, count_cells("$_CONSTX_"));
    (void)gen;
}

/* A soft path from `unconn` into a RAM pin ends on the canonical constant 0 (like VTR). */
static void test_buffer_of_unconn_ends_on_gnd(void) {
    (void)load_ram();
    odin3_net_id gnd = odin3_module_find_net(mod, afx_intern(design, "gnd"));
    TEST_ASSERT_TRUE(odin3_net_valid(gnd));
    TEST_ASSERT_NOT_EQUAL_UINT32(gnd.v, addr_net(ram_node(), BUFFERED_BIT).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")));
    afx_check(design);
    gnd = odin3_module_find_net(mod, afx_intern(design, "gnd"));
    TEST_ASSERT_TRUE(odin3_net_valid(gnd));
    TEST_ASSERT_EQUAL_UINT32(gnd.v, addr_net(ram_node(), BUFFERED_BIT).v);
}

/* No pin of a `.names` or `.latch` line of the written text is on `unconn`; the generator's own
 * one-name `.names unconn` line has no pin besides its output. */
static void expect_no_unconn_word(char *line) {
    if (strncmp(line, ".names", 6) != 0 && strncmp(line, ".latch", 6) != 0) {
        return;
    }
    if (strcmp(line, ".names unconn") == 0) {
        return;
    }
    char shown[AFX_LINE];
    (void)snprintf(shown, sizeof shown, "%s", line);
    for (char *word = strtok(line, " "); word != NULL; word = strtok(NULL, " ")) {
        TEST_ASSERT_TRUE_MESSAGE(strcmp(word, "unconn") != 0, shown);
    }
}

static void expect_no_unconn_pin(const char *text) {
    const char *line = text;
    while (*line != '\0') {
        size_t len = strcspn(line, "\n");
        char copy[AFX_LINE];
        TEST_ASSERT_TRUE(len < sizeof copy);
        memcpy(copy, line, len);
        copy[len] = '\0';
        expect_no_unconn_word(copy);
        line += len + (line[len] == '\n' ? 1 : 0);
    }
}

static void test_no_written_pin_on_unconn(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id out_q = afx_port(design, mod, "q", ODIN3_DIR_OUT);
    odin3_net_id out_z = afx_port(design, mod, "z", ODIN3_DIR_OUT);
    odin3_net_id unconn = afx_net(design, mod, "unconn");
    (void)afx_const_sop(design, mod, unconn);
    odin3_net_id gate[3] = {in_a, unconn, out_y};
    (void)AFX_CELL(design, mod, "$_OR_", NULL, gate, 3);
    odin3_net_id flop[2] = {unconn, out_q};
    (void)AFX_CELL(design, mod, "$_FF_", NULL, flop, 2);
    odin3_net_id buffer[2] = {unconn, out_z};
    (void)AFX_CELL(design, mod, "$_BUF_", NULL, buffer, 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")), afx_errors());
    afx_check(design);
    char *text = afx_blif_text(design);
    expect_no_unconn_pin(text);
    free(text);
}

/* The same fixtures with every run in the odin3-abc subprocess (3D-5a hand-over): the real ABC
 * answers, the splice is the same. Needs ODIN3_ABC_EXE_PATH (the build's odin3-abc). */
static void use_child(void) {
#ifdef ODIN3_ABC_EXE_PATH
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", ODIN3_ABC_EXE_PATH, 1));
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_CHILD);
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

static void test_hard_block_pins_stay_open_child(void) {
    use_child();
    test_hard_block_pins_stay_open();
}

static void test_buffer_of_unconn_ends_on_gnd_child(void) {
    use_child();
    test_buffer_of_unconn_ends_on_gnd();
}

static void test_no_written_pin_on_unconn_child(void) {
    use_child();
    test_no_written_pin_on_unconn();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hard_block_pins_stay_open);
    RUN_TEST(test_buffer_of_unconn_ends_on_gnd);
    RUN_TEST(test_no_written_pin_on_unconn);
    RUN_TEST(test_hard_block_pins_stay_open_child);
    RUN_TEST(test_buffer_of_unconn_ends_on_gnd_child);
    RUN_TEST(test_no_written_pin_on_unconn_child);
    exchange_env = "aig"; /* the same three on the in-memory exchange (3D-7, fake engine) */
    RUN_TEST(test_hard_block_pins_stay_open);
    RUN_TEST(test_buffer_of_unconn_ends_on_gnd);
    RUN_TEST(test_no_written_pin_on_unconn);
    return UNITY_END();
}
