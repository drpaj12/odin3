/*
 * test_abc_gates.c — abc --gates after start-up (gatemap plan Tasks 11 and 12; spec 4.3, 4.4, G-2,
 * G-7): the cell result read from hand-built odin3_abc_cells (each library gate tagged; an unknown
 * gate, a wrong input count, a wrong function and an untagged object refused), the session order on
 * the fake engine, and the splice of library gates (types, buffers, TIE cells, the init-1 cut,
 * provenance).
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/cells.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/abc/splice_int.h"
#include "passes/manager.h"
#include "util/alloc.h"

#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { N = 0x7FFFFFFF, N2 = 0x7FFFFFFE };

static odin3_design *design;
static odin3_module *mod;
static odin3_pass_ctx ctx;
static odin3_abc_run *run;
static odin3_vec domains;
static odin3_abc_region region;
static odin3_abc_result result;
static odin3_arena *arena;

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    odin3_pass_set_options((odin3_pass_options){0});
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    mod = NULL;
    run = NULL;
    region = (odin3_abc_region){0};
    domains = (odin3_vec){0};
    odin3_abc_result_init(&result);
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_result_free(&result);
    odin3_arena_destroy(arena);
    odin3_abc_region_free(&region);
    odin3_abc_domains_free(&domains);
    odin3_abc_run_destroy(run);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void read_generic(void) {
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/generic_gates.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
}

/* Module m: inputs a b, output y = a & b. */
static void build_and(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id in_b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id gate[3] = {in_a, in_b, out_y};
    (void)AFX_CELL(design, mod, "$_AND_", "g", gate, 3);
    afx_check(design);
}

/* A run of abc --gates generic_gates on module m, its first domain's region built. */
static void start_region_of(odin3_module *module) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, afx_intern(design, "abc"), &ctx));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_abc_run_start(&ctx, odin3_bytes_cstr("--gates generic_gates"), &run),
        afx_errors());
    run->module = module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, module, &domains));
    TEST_ASSERT_TRUE(domains.len > 0);
    run->domain = (const odin3_abc_domain *)domains.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_region_build(run, run->domain, &region));
    run->region = &region;
}

/* start_region_of(mod) for build_and's module: two CIs, one CO. */
static void start_region(void) {
    start_region_of(mod);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)region.cis.len);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)region.cos.len);
}

/* The index of the library gate named name in the run's lib.gates. */
static uint32_t gate_index(const char *name) {
    for (size_t at = 0; at < run->lib.gates.len; at++) {
        const odin3_libgate *gate = odin3_vec_cat(&run->lib.gates, at);
        if (strcmp(odin3_celltype_get(design, gate->type)->name, name) == 0) {
            return (uint32_t)at;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return UINT32_MAX;
}

/*
 * A hand-built cell network over a b: object 4 is gate g4 over object 2 (a), object 5 gate g5 over
 * objects 4 and 3 (b), object 6 the PO of 5. names and tags as given.
 */
typedef struct hand_cells {
    int32_t fanins[14];
    uint32_t truths[7];
    int32_t gate[7];
    const char *names[2];
    odin3_abc_cells cells;
} hand_cells;

static void hand_init(hand_cells *hc, uint32_t truth4, uint32_t truth5) {
    const int32_t fanins[14] = {N, N, N, N, N, N, N, N, 2, N, 4, 3, 5, N2};
    memcpy(hc->fanins, fanins, sizeof fanins);
    memset(hc->truths, 0, sizeof hc->truths);
    hc->truths[4] = truth4;
    hc->truths[5] = truth5;
    const int32_t gate[7] = {-1, -1, -1, -1, 1, 0, -1};
    memcpy(hc->gate, gate, sizeof gate);
    hc->names[0] = "AND2";
    hc->names[1] = "INV";
    hc->cells = (odin3_abc_cells){{hc->fanins, hc->truths, NULL, 7, 0, 2}, hc->gate, hc->names, 2};
}

/* from_cells fails with ENGINE and logs exactly the one malformed-network error why. */
static void expect_malformed(const hand_cells *hc, const char *why) {
    char line[256];
    (void)snprintf(line, sizeof line, "abc: ABC returned a malformed network: %s", why);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_result_from_cells(run, &hc->cells, arena, &result));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(ODIN3_LOG_ERROR, "abc:"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_ERROR, line), afx_errors());
}

/* --- Task 11: the cell result ---------------------------------------------------------------- */

static void test_from_cells_tags_gates(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_abc_result_from_cells(run, &hc.cells, arena, &result), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(2, result.n_luts);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.gate.len);
    TEST_ASSERT_EQUAL_UINT32(gate_index("INV"), ((const uint32_t *)result.gate.data)[0]);
    TEST_ASSERT_EQUAL_UINT32(gate_index("AND2"), ((const uint32_t *)result.gate.data)[1]);
}

static void test_from_cells_unknown_gate(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.names[0] = "NAND3";
    expect_malformed(&hc, "gate 'NAND3' is not a cell of library 'generic_gates'");
}

static void test_from_cells_wrong_arity(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.gate[5] = 1; /* "INV" on the 2-input object */
    expect_malformed(&hc, "gate 'INV' has 2 inputs; library cell 'INV' has 1");
}

static void test_from_cells_wrong_function(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0xE);
    expect_malformed(&hc, "gate 'AND2' does not compute library cell 'AND2''s function");
}

static void test_from_cells_untagged_object(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.gate[5] = -1;
    expect_malformed(&hc, "a gate object without a gate name");
    odin3_abc_result_free(&result);
    hand_init(&hc, 0x1, 0x8);
    hc.gate[4] = 2; /* past n_names */
    expect_malformed(&hc, "a gate object without a gate name");
}

/* from_cells with the allocator failing after limit allocations, on a fresh arena. */
static odin3_status cells_with_limit(const hand_cells *hc, int limit) {
    odin3_abc_result_free(&result);
    odin3_arena *scratch = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(scratch);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_abc_result_from_cells(run, &hc->cells, scratch, &result);
    odin3_util_set_alloc_fail_after(-1);
    odin3_arena_destroy(scratch);
    return st;
}

static void test_from_cells_out_of_memory(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (int limit = 0; limit < 64 && st != ODIN3_OK; limit++) {
        st = cells_with_limit(&hc, limit);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.gate.len);
}

static void test_gates_session_order(void) {
    read_generic();
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--gates generic_gates")),
        afx_errors());
    TEST_ASSERT_EQUAL_STRING("&put\nstrash\nifraig -v\nscorr -v\ndc2 -v\ndch -f\nmap -a\n"
                             "print_stats\n",
                             odin3_abc_fake_last_commands());
    TEST_ASSERT_TRUE(odin3_abc_fake_genlib()[0] != '\0');
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: module 'm', no flip-flops: no name map (--gates); "
                             "provenance uses cut cones"));
}

/* --- Task 12: the splice --------------------------------------------------------------------- */

/* Reads BLIF text (written to design.blif first) into the design; mod is its top module. */
static void read_blif_text(const char *text) {
    char path[AFX_PATH];
    afx_path(path, "design.blif");
    AFX_WRITE(path, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(design, path), afx_errors());
    mod = afx_top(design);
    TEST_ASSERT_NOT_NULL(mod);
}

/* Reads an .o3lib text (written to inline.o3lib first). */
static void read_inline_library(const char *text) {
    char path[AFX_PATH];
    afx_path(path, "inline.o3lib");
    AFX_WRITE(path, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(path)));
}

static odin3_status run_gates(const char *library) {
    char args[64];
    (void)snprintf(args, sizeof args, "--gates %s", library);
    afx_log_reset();
    return odin3_pass_run(design, "abc", odin3_bytes_cstr(args));
}

/* Live nodes of mod whose type is called name. */
static uint32_t count_type(const char *name) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, name) == 0) {
            count++;
        }
    }
    return count;
}

/* Live nodes of mod that are not ports. */
static uint32_t count_cells(void) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT) {
            count++;
        }
    }
    return count;
}

static const char k_two_outputs[] = ".model m\n"
                                    ".inputs a b c\n"
                                    ".outputs y z\n"
                                    ".names a b n1\n11 1\n"
                                    ".names n1 c y\n1- 1\n-1 1\n"
                                    ".names a b z\n01 1\n"
                                    ".end\n";

static void test_gates_splice_types(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    uint32_t and2 = count_type("AND2");
    uint32_t inv = count_type("INV");
    TEST_ASSERT_TRUE(and2 > 0);
    TEST_ASSERT_TRUE(inv > 0);
    TEST_ASSERT_EQUAL_UINT32(and2 + inv, count_cells());
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$lut"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$sop"));
    afx_check(design);
    char line[160];
    (void)snprintf(line, sizeof line,
                   "abc: module 'm', no flip-flops: 3 cells and 0 flip-flops -> %u gates and 0 "
                   "flip-flops, ",
                   (unsigned)(and2 + inv));
    TEST_ASSERT_NOT_NULL(afx_only(ODIN3_LOG_INFO, line));
}

static void test_gates_buffer_without_buf_cell(void) {
    read_inline_library(
        "library nobuf\n"
        "cell INV gate area 1\n  in A 1 ; out Y 1\n  fn Y = ~A\nend\n"
        "cell AND2 gate area 1\n  in A 1 ; in B 1 ; out Y 1\n  fn Y = A & B\nend\n");
    read_blif_text(".model m\n.inputs a b\n.outputs y1 y2\n"
                   ".names a b y1\n11 1\n.names y1 y2\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("nobuf"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_BUF_"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("AND2"));
    TEST_ASSERT_EQUAL_UINT32(2, count_cells());
    afx_check(design);
}

static void test_gates_buffer_uses_buf_cell(void) {
    read_generic();
    read_blif_text(".model m\n.inputs a b\n.outputs y1 y2\n"
                   ".names a b y1\n11 1\n.names y1 y2\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1, count_type("BUF"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_BUF_"));
    afx_check(design);
}

/* A splice of the run's region with nothing in it but what sp_canon makes. */
static void canon_splice(splice *sp) {
    *sp = (splice){0};
    sp->run = run;
    sp->mod = mod;
    sp->region = &region;
    sp->res = &result;
    odin3_vec_init(&sp->created, sizeof(sp_created));
}

static void test_gates_const_uses_tie(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    start_region_of(mod);
    splice sp;
    canon_splice(&sp);
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, sp_canon(&sp, 1, &net));
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, net));
    odin3_node_id tie = odin3_pin_node(mod, odin3_net_driver(mod, net));
    TEST_ASSERT_EQUAL_STRING("TIE1", odin3_celltype_get(design, odin3_node_type(mod, tie))->name);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_node_prov(mod, tie).v);
    odin3_net_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, sp_canon(&sp, 1, &again));
    TEST_ASSERT_EQUAL_UINT32(net.v, again.v);
    odin3_vec_free(&sp.created);
    canon_splice(&sp); /* a later splice finds the TIE1 net instead of making one */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, sp_canon(&sp, 1, &again));
    TEST_ASSERT_EQUAL_UINT32(net.v, again.v);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)sp.created.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, sp_canon(&sp, 0, &again));
    TEST_ASSERT_EQUAL_STRING(
        "TIE0", odin3_celltype_get(
                    design, odin3_node_type(mod, odin3_pin_node(mod, odin3_net_driver(mod, again))))
                    ->name);
    odin3_vec_free(&sp.created);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("TIE1"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("TIE0"));
}

static const char k_init_flop[] = ".model m\n"
                                  ".inputs a b clk\n"
                                  ".outputs q\n"
                                  ".names a b d\n11 1\n"
                                  ".latch d q re clk %d\n"
                                  ".end\n";

/* The one $_DFF_P_ of mod. */
static odin3_node_id the_flop(void) {
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            strcmp(odin3_celltype_get(design, odin3_node_type(mod, node))->name, "$_DFF_P_") == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE("no $_DFF_P_");
    return (odin3_node_id){0};
}

/* The flop's node IDs change with the pass's compaction; its record tells the old node from a
 * rebuilt one (the splice gives a new flip-flop a DERIVED record). */
static void test_gates_init1_flop_is_cut(void) {
    char text[sizeof k_init_flop + 8];
    read_generic();
    (void)snprintf(text, sizeof text, k_init_flop, 1);
    read_blif_text(text);
    odin3_prov_id before = odin3_node_prov(mod, the_flop());
    TEST_ASSERT_NOT_EQUAL_UINT32(0, before.v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    odin3_node_id after = the_flop();
    TEST_ASSERT_EQUAL_UINT32(before.v, odin3_node_prov(mod, after).v);
    TEST_ASSERT_EQUAL_INT64(1, odin3_node_param(mod, after, 0)->i);
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: module 'm': cut points: 0 hard or black-box cells, 0 instances, "
                             "0 level latches, 0 tri-state or bus cells, 1 other cells"));
    TEST_ASSERT_EQUAL_UINT32(1, count_type("AND2"));
    afx_check(design);
    /* INIT 0: a register ABC sees, rebuilt with a DERIVED record. */
    odin3_design_destroy(design);
    design = odin3_design_create();
    read_generic();
    (void)snprintf(text, sizeof text, k_init_flop, 0);
    read_blif_text(text);
    before = odin3_node_prov(mod, the_flop());
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    odin3_prov_id rebuilt = odin3_node_prov(mod, the_flop());
    TEST_ASSERT_NOT_EQUAL_UINT32(before.v, rebuilt.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, odin3_prov_get(design, rebuilt)->kind);
    TEST_ASSERT_EQUAL_UINT32(0, afx_count(ODIN3_LOG_INFO, "abc: module 'm': cut points:"));
}

/* A library-cell node's record: DERIVED, with parents. */
static void expect_derived(odin3_node_id node) {
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_node_prov(mod, node));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_TRUE(rec->parents.count > 0);
}

static void test_gates_prov_every_gate(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        bool library_cell = odin3_node_live(mod, node) &&
                            odin3_celltype_lib(design, odin3_node_type(mod, node)) != NULL;
        if (library_cell) {
            expect_derived(node);
            seen++;
        }
    }
    TEST_ASSERT_TRUE(seen > 0);
}

/* --- review fixes: pass-level --gates cases ------------------------------------------------ */

/* The net of port index of mod (declaration order: the BLIF's inputs, then its outputs). */
static odin3_net_id port_net(uint32_t index) {
    odin3_node_id port = odin3_module_port(mod, index);
    return odin3_pin_net(mod, odin3_node_pins(mod, port).first);
}

/* The type name of the one driver of net ("" when it has none or several). */
static const char *driver_type(odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_driver_count(mod, net) != 1) {
        return "";
    }
    odin3_node_id node = odin3_pin_node(mod, odin3_net_driver(mod, net));
    return odin3_celltype_get(design, odin3_node_type(mod, node))->name;
}

/* The one driver of net. */
static odin3_node_id driver_node(odin3_net_id net) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_driver_count(mod, net));
    return odin3_pin_node(mod, odin3_net_driver(mod, net));
}

/* The type names of the drivers of the input pins of node, in pin order, joined by blanks. */
static const char *input_drivers(odin3_node_id node) {
    static char text[128];
    text[0] = '\0';
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_reads(mod, pin)) {
            size_t used = strlen(text);
            (void)snprintf(text + used, sizeof text - used, "%s%s", used > 0 ? " " : "",
                           driver_type(odin3_pin_net(mod, pin)));
        }
    }
    return text;
}

/* y = a & ~a: the AIG makes y the constant 0, so ABC's PO reads object 0. */
static const char k_const_output[] = ".model m\n.inputs a\n.outputs y\n"
                                     ".names a n\n0 1\n.names a n y\n11 1\n.end\n";

static void read_cmos6(void) {
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/cmos6.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
}

static void test_gates_const_output_tie(void) {
    read_generic();
    read_blif_text(k_const_output);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_STRING("TIE0", driver_type(port_net(1)));
    TEST_ASSERT_EQUAL_UINT32(1, count_cells());
    afx_check(design);
}

/* cmos6 has no TIE cell: the constant is a $_CONST0_ cell (written as a literal). */
static void test_gates_const_output_cmos6(void) {
    read_cmos6();
    read_blif_text(k_const_output);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("cmos6"), afx_errors());
    TEST_ASSERT_EQUAL_STRING("$_CONST0_", driver_type(port_net(1)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_node_prov(mod, driver_node(port_net(1))).v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_net_prov(mod, port_net(1)).v);
    TEST_ASSERT_EQUAL_UINT32(1, count_cells());
    afx_check(design);
}

/* y = a: ABC's PO reads the PI; y carries a through the library's BUF. */
static void test_gates_input_to_output(void) {
    read_generic();
    read_blif_text(".model m\n.inputs a b\n.outputs y z\n"
                   ".names a y\n1 1\n.names a b z\n11 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    odin3_net_id out_y = port_net(2);
    if (out_y.v != port_net(0).v) {
        TEST_ASSERT_EQUAL_STRING("BUF", driver_type(out_y));
        TEST_ASSERT_EQUAL_STRING("$port_in", input_drivers(driver_node(out_y)));
    }
    TEST_ASSERT_EQUAL_STRING("AND2", driver_type(port_net(3)));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$sop"));
    afx_check(design);
}

/* y = q & a with q a register ABC keeps: the AND2 reads the new flip-flop's Q. */
static void test_gates_reads_register(void) {
    read_generic();
    read_blif_text(".model m\n.inputs a b clk\n.outputs y\n"
                   ".names a b d\n11 1\n.latch d q re clk 0\n.names q a y\n11 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$_DFF_P_"));
    TEST_ASSERT_EQUAL_STRING("AND2", driver_type(port_net(3)));
    const char *ins = input_drivers(driver_node(port_net(3)));
    TEST_ASSERT_TRUE_MESSAGE(strstr(ins, "$_DFF_P_") != NULL, ins);
    afx_check(design);
}

/* A register with D = 0 and INIT 0 is the constant 0 after scorr, so ABC's AND2 for y = q & a
 * reads object 0: the splice gives it the TIE0 net (uses_constant over the gate fanins). */
static void test_gates_gate_reads_constant(void) {
    read_generic();
    read_blif_text(".model m\n.inputs a clk\n.outputs y\n"
                   ".names zero\n.latch zero q re clk 0\n.names q a y\n11 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$_DFF_P_"));
    TEST_ASSERT_EQUAL_STRING("AND2", driver_type(port_net(2)));
    const char *ins = input_drivers(driver_node(port_net(2)));
    TEST_ASSERT_TRUE_MESSAGE(strstr(ins, "TIE0") != NULL, ins);
    afx_check(design);
}

/* ABC names a gate the library does not have: a malformed network, the IR as it was. */
static void test_gates_malformed_leaves_ir(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    char *before = afx_blif_text(design);
    odin3_abc_fake_gate_names((odin3_abc_fake_gates){"NOPE2", NULL});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_gates("generic_gates"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1, afx_count(ODIN3_LOG_ERROR, "abc: ABC returned a malformed network: "), afx_errors());
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    afx_check(design);
}

/* The depth counts library gates: the fake maps y = (a & b) | c to AND2, INV, AND2, INV (4 levels,
 * the OR as an AND of inverted inputs). */
static void test_gates_depth_counts_gates(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_INFO, "abc: final: depth 4"),
                             afx_only(ODIN3_LOG_INFO, "abc: final: depth"));
    const char *info = odin3_strtab_get(
        odin3_design_strtab(design),
        odin3_passrun_info(design, (odin3_passrun_id){odin3_passrun_end(design) - 1U}));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(info, " depth=4"), info);
}

/* A library TIE cell is a constant, not a cut point: y = a & t with t from TIE1 is y = a. */
static void test_gates_tie_cell_is_constant(void) {
    read_generic();
    read_blif_text(".model m\n.inputs a\n.outputs y\n"
                   ".subckt TIE1 Y=t\n.names a t y\n11 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_gates("generic_gates"), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(0, afx_count(ODIN3_LOG_INFO, "abc: module 'm': cut points:"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("AND2"));
    TEST_ASSERT_EQUAL_UINT32(0, count_type("$sop"));
    afx_check(design);
}

/* Live nets and nodes of mod. */
static uint32_t live_objects(void) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        count += odin3_node_live(mod, (odin3_node_id){id}) ? 1U : 0U;
    }
    for (uint32_t id = 1; id < odin3_module_net_end(mod); id++) {
        count += odin3_net_live(mod, (odin3_net_id){id}) ? 1U : 0U;
    }
    return count;
}

/* sp_canon(1) out of memory at every allocation after the coarse record (made first, so the
 * sweep reaches the new net, node and log): the rollback leaves nothing behind. Ends with one cell
 * of type made. */
static void canon_out_of_memory(const char *made) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (int limit = 0; st == ODIN3_ERR_NO_MEMORY && limit < 64; limit++) {
        splice sp;
        canon_splice(&sp);
        odin3_prov_id coarse = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, sp_coarse(&sp, &coarse));
        uint32_t before = live_objects();
        odin3_net_id net = {0};
        odin3_util_set_alloc_fail_after(limit);
        st = sp_canon(&sp, 1, &net);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            sp_rollback(&sp);
            TEST_ASSERT_EQUAL_UINT32(before, live_objects());
        }
        odin3_vec_free(&sp.created);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(1, count_type(made));
}

/* make_tie (the library's TIE1). */
static void test_gates_make_tie_out_of_memory(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    start_region_of(mod);
    canon_out_of_memory("TIE1");
}

/* No TIE1 in the library: the $_CONST1_ cell, with the coarse record. */
static void test_gates_const_cell_out_of_memory(void) {
    read_generic();
    read_blif_text(k_two_outputs);
    start_region_of(mod);
    run->lib.tie1 = (odin3_celltype_id){0};
    canon_out_of_memory("$_CONST1_");
    odin3_node_id node = {0};
    for (uint32_t id = 1; id < odin3_module_node_end(mod) && node.v == 0; id++) {
        bool is_const =
            odin3_node_live(mod, (odin3_node_id){id}) &&
            strcmp(odin3_celltype_get(design, odin3_node_type(mod, (odin3_node_id){id}))->name,
                   "$_CONST1_") == 0;
        node.v = is_const ? id : 0;
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, odin3_node_prov(mod, node).v);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_from_cells_tags_gates);
    RUN_TEST(test_from_cells_unknown_gate);
    RUN_TEST(test_from_cells_wrong_arity);
    RUN_TEST(test_from_cells_wrong_function);
    RUN_TEST(test_from_cells_untagged_object);
    RUN_TEST(test_from_cells_out_of_memory);
    RUN_TEST(test_gates_session_order);
    RUN_TEST(test_gates_splice_types);
    RUN_TEST(test_gates_buffer_without_buf_cell);
    RUN_TEST(test_gates_buffer_uses_buf_cell);
    RUN_TEST(test_gates_const_uses_tie);
    RUN_TEST(test_gates_init1_flop_is_cut);
    RUN_TEST(test_gates_prov_every_gate);
    RUN_TEST(test_gates_const_output_tie);
    RUN_TEST(test_gates_const_output_cmos6);
    RUN_TEST(test_gates_input_to_output);
    RUN_TEST(test_gates_reads_register);
    RUN_TEST(test_gates_gate_reads_constant);
    RUN_TEST(test_gates_malformed_leaves_ir);
    RUN_TEST(test_gates_depth_counts_gates);
    RUN_TEST(test_gates_tie_cell_is_constant);
    RUN_TEST(test_gates_make_tie_out_of_memory);
    RUN_TEST(test_gates_const_cell_out_of_memory);
    return UNITY_END();
}
