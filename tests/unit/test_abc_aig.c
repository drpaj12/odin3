/*
 * test_abc_aig.c — the AIG builder and the in-memory protocol (3D plan Task 20; spec §6.3, AB-16,
 * AB-22): the layout of t1 with both INITs, the hashing rules, the balanced trees, an AIG
 * evaluator against 1E simulation, the PI names the fake engine returns, the cap, the session
 * commands and the OOM sweeps of aig_build and result_from_luts.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "abc_sim.h"
#include "abc_stage.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/aig.h"
#include "passes/abc/aligned.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/manager.h"
#include "sim/sim.h"
#include "util/alloc.h"

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository"
#endif

enum { NULL_LIT = 0x7FFFFFFF, MAX_VALUES = 4096, MAX_PIS = 64, OOM_LIMIT = 4000 };

static odin3_design *design;
static odin3_module *mod;

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
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_run_limits(NULL);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* --- building ------------------------------------------------------------------------------ */

static void new_module(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_top(design, mid));
    mod = odin3_module_get(design, mid);
}

static odin3_net_id input(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_IN);
}

static odin3_net_id output(const char *name) {
    return afx_port(design, mod, name, ODIN3_DIR_OUT);
}

static odin3_node_id gate2(const char *type, odin3_net_id src0, odin3_net_id src1,
                           odin3_net_id dst) {
    odin3_net_id nets[3] = {src0, src1, dst};
    return AFX_CELL(design, mod, type, NULL, nets, 3);
}

static odin3_net_id const_net(const char *name, bool one) {
    odin3_net_id net = afx_net(design, mod, name);
    (void)AFX_CELL(design, mod, one ? "$_CONST1_" : "$_CONST0_", NULL, &net, 1);
    return net;
}

/* Loads BLIF text as the design (written to the test directory first). */
typedef struct blif_text {
    const char *name;
    const char *text;
} blif_text;

static void load_text(blif_text file) {
    char path[AFX_PATH];
    afx_path(path, file.name);
    afx_write_bytes(path, odin3_bytes_cstr(file.text));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    mod = afx_top(design);
}

/* t1.blif with the register's INIT. */
static void load_t1(int init) {
    char text[AFX_TEXT];
    char path[AFX_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/unit/data/abc/t1.blif", ODIN3_SOURCE_DIR);
    (void)snprintf(text, sizeof text, "%s", afx_slurp(path));
    char *tail = strstr(text, "clk 0");
    TEST_ASSERT_NOT_NULL(tail);
    tail[4] = (char)('0' + init);
    load_text((blif_text){"t1_init.blif", text});
}

static afx_staged staged;
static odin3_abc_aigb aig;

/* The region of the design's top, and its AIG in `aig` (free it with free_all). */
static odin3_status build_aig(void) {
    afx_stage_region(design, mod, &staged);
    return odin3_abc_aig_build(staged.run, &staged.region, &aig);
}

static void free_all(void) {
    odin3_abc_aig_free(&aig);
    afx_unstage(&staged);
    staged = (afx_staged){0};
}

static int32_t obj_at(uint32_t idx, uint32_t slot) {
    return ((const int32_t *)aig.objs.data)[2U * idx + slot];
}

static uint32_t n_objs(void) {
    return (uint32_t)(aig.objs.len / 2U);
}

/* --- layout -------------------------------------------------------------------------------- */

/* t1: PIs a, b and the register output after object 0, one AND (2, 4), the PO of y and the
 * register input. */
static void expect_t1_layout(int32_t q_lit, int32_t d_lit) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(7, n_objs());
    TEST_ASSERT_EQUAL_UINT32(1, aig.n_regs);
    TEST_ASSERT_EQUAL_UINT32(3, aig.n_pis);
    TEST_ASSERT_EQUAL_UINT32(2, aig.n_pos);
    for (uint32_t obj = 0; obj < 4; obj++) { /* object 0, then the three PIs */
        TEST_ASSERT_EQUAL_INT(NULL_LIT, obj_at(obj, 0));
        TEST_ASSERT_EQUAL_INT(NULL_LIT, obj_at(obj, 1));
    }
    TEST_ASSERT_EQUAL_INT(2, obj_at(4, 0));
    TEST_ASSERT_EQUAL_INT(4, obj_at(4, 1));
    TEST_ASSERT_EQUAL_INT(q_lit, obj_at(5, 0));
    TEST_ASSERT_EQUAL_INT(NULL_LIT, obj_at(5, 1));
    TEST_ASSERT_EQUAL_INT(d_lit, obj_at(6, 0));
    TEST_ASSERT_EQUAL_INT(NULL_LIT, obj_at(6, 1));
    odin3_abc_aig view = odin3_abc_aig_view(&aig);
    TEST_ASSERT_EQUAL_UINT32(7, view.n_objs);
    TEST_ASSERT_EQUAL_UINT32(1, view.n_regs);
    TEST_ASSERT_TRUE(view.objs == aig.objs.data);
    free_all();
}

static void test_layout_init0(void) {
    load_t1(0);
    expect_t1_layout(6, 8);
}

/* INIT 1: the register output read by the PO and the register input are complemented (AB-13). */
static void test_layout_init1(void) {
    load_t1(1);
    expect_t1_layout(7, 9);
}

/* --- hashing and trees --------------------------------------------------------------------- */

static void test_hashing(void) {
    new_module();
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id zero = const_net("k0", false);
    odin3_net_id one = const_net("k1", true);
    odin3_net_id not_a = afx_net(design, mod, "not_a");
    odin3_net_id nets_not[2] = {in_a, not_a};
    (void)AFX_CELL(design, mod, "$_NOT_", NULL, nets_not, 2);
    odin3_net_id zout[6];
    const char *names[6] = {"z0", "z1", "z2", "z3", "z4", "z5"};
    for (uint32_t i = 0; i < 6; i++) {
        zout[i] = output(names[i]);
    }
    (void)gate2("$_AND_", in_a, zero, zout[0]);
    (void)gate2("$_AND_", in_a, one, zout[1]);
    (void)gate2("$_AND_", in_a, in_a, zout[2]);
    (void)gate2("$_AND_", in_a, not_a, zout[3]);
    odin3_node_id first = gate2("$_AND_", in_a, in_b, zout[4]);
    odin3_node_id second = gate2("$_AND_", in_b, in_a, zout[5]);
    TEST_ASSERT_TRUE(first.v < second.v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    TEST_ASSERT_EQUAL_INT(0, odin3_abc_aig_lit(&aig, zout[0]));
    TEST_ASSERT_EQUAL_INT(2, odin3_abc_aig_lit(&aig, zout[1]));
    TEST_ASSERT_EQUAL_INT(2, odin3_abc_aig_lit(&aig, zout[2]));
    TEST_ASSERT_EQUAL_INT(0, odin3_abc_aig_lit(&aig, zout[3]));
    int32_t shared = odin3_abc_aig_lit(&aig, zout[4]);
    TEST_ASSERT_EQUAL_INT(6, shared); /* the only AND: object 3 after object 0 and the PIs a, b */
    TEST_ASSERT_EQUAL_INT(shared, odin3_abc_aig_lit(&aig, zout[5]));
    TEST_ASSERT_EQUAL_INT(-1, odin3_abc_aig_lit(&aig, in_a));
    TEST_ASSERT_EQUAL_UINT32(4U + 6U, n_objs()); /* object 0, 2 PIs, 1 AND, 6 POs */
    TEST_ASSERT_EQUAL_UINT32(first.v, ((const uint32_t *)aig.owner.data)[3]);
    TEST_ASSERT_TRUE(odin3_net_valid(odin3_abc_aig_net_of_var(&aig, 3)));
    free_all();
}

/* A 4-literal cube: ANDs (l0, l1) and (l2, l3), then their AND. */
static void test_balanced_cube(void) {
    load_text((blif_text){
        "cube4.blif", ".model c4\n.inputs a b c d\n.outputs y\n.names a b c d y\n1111 1\n.end\n"});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1U + 4U + 3U + 1U, n_objs());
    TEST_ASSERT_EQUAL_INT(2, obj_at(5, 0));
    TEST_ASSERT_EQUAL_INT(4, obj_at(5, 1));
    TEST_ASSERT_EQUAL_INT(6, obj_at(6, 0));
    TEST_ASSERT_EQUAL_INT(8, obj_at(6, 1));
    TEST_ASSERT_EQUAL_INT(10, obj_at(7, 0));
    TEST_ASSERT_EQUAL_INT(12, obj_at(7, 1));
    TEST_ASSERT_EQUAL_INT(14, obj_at(8, 0));
    free_all();
}

/* An off-set cover complements the output; an OR is an AND of complements. */
static void test_off_set_and_or(void) {
    load_text((blif_text){"offor.blif",
                          ".model oo\n.inputs a b\n.outputs y n\n.names a b y\n1- 1\n-1 1\n"
                          ".names a b n\n11 0\n.end\n"});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    /* y = a | b = ~(~a & ~b): the AND (3, 5) with an output complement; n = ~(a & b). */
    TEST_ASSERT_EQUAL_INT(3, obj_at(3, 0));
    TEST_ASSERT_EQUAL_INT(5, obj_at(3, 1));
    TEST_ASSERT_EQUAL_INT(2, obj_at(4, 0));
    TEST_ASSERT_EQUAL_INT(4, obj_at(4, 1));
    odin3_net_id or_net = odin3_module_find_net(mod, afx_intern(design, "y"));
    odin3_net_id nand_net = odin3_module_find_net(mod, afx_intern(design, "n"));
    TEST_ASSERT_EQUAL_INT(7, odin3_abc_aig_lit(&aig, or_net));
    TEST_ASSERT_EQUAL_INT(9, odin3_abc_aig_lit(&aig, nand_net));
    free_all();
}

/* --- equal to the IR ----------------------------------------------------------------------- */

typedef struct evaluator {
    uint8_t value[MAX_VALUES];
    uint8_t pis[MAX_PIS];
} evaluator;

static bool lit_value(const evaluator *ev, int32_t lit) {
    return (ev->value[(uint32_t)lit >> 1] ^ ((uint32_t)lit & 1U)) != 0;
}

/* Evaluates every object for the PI values in ev->pis. */
static void evaluate(evaluator *ev) {
    uint32_t pi = 0;
    ev->value[0] = 0;
    for (uint32_t obj = 1; obj < n_objs(); obj++) {
        int32_t lit0 = obj_at(obj, 0);
        int32_t lit1 = obj_at(obj, 1);
        if (lit0 == NULL_LIT) {
            ev->value[obj] = ev->pis[pi++];
        } else if (lit1 == NULL_LIT) {
            ev->value[obj] = lit_value(ev, lit0) ? 1U : 0U;
        } else {
            ev->value[obj] = lit_value(ev, lit0) && lit_value(ev, lit1) ? 1U : 0U;
        }
    }
}

static const char *net_text(odin3_net_id net) {
    return odin3_strtab_get(odin3_design_strtab(design), odin3_net_name(mod, net));
}

static int find_port(const char *name, bool is_input, const odin3_sim *sim) {
    uint32_t count = is_input ? odin3_sim_input_count(sim) : odin3_sim_output_count(sim);
    for (uint32_t port = 0; port < count; port++) {
        const char *have =
            is_input ? odin3_sim_input_name(sim, port) : odin3_sim_output_name(sim, port);
        if (strcmp(have, name) == 0) {
            return (int)port;
        }
    }
    return -1;
}

/* Copies the simulation's input bits of the region's CIs into the evaluator's PI values. */
static void read_cis(const odin3_sim *sim, evaluator *ev) {
    const odin3_vec *cis = &staged.region.cis;
    for (uint32_t ci = 0; ci < cis->len; ci++) {
        int port =
            find_port(net_text((odin3_net_id){((const uint32_t *)cis->data)[ci]}), true, sim);
        bool value = false;
        TEST_ASSERT_TRUE_MESSAGE(port >= 0, "a CI that is not an input port");
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_sim_get_input(sim, (odin3_sim_bit){(uint32_t)port, 0}, &value));
        ev->pis[ci] = value ? 1U : 0U;
    }
}

/* One clock cycle of the AIG: the registers take their next state, then the outputs settle. */
static void step_aig(evaluator *ev) {
    uint32_t n_cis = (uint32_t)staged.region.cis.len;
    uint32_t n_cos = aig.n_pos - aig.n_regs;
    uint32_t first_po = n_objs() - aig.n_pos;
    evaluate(ev);
    for (uint32_t reg = 0; reg < aig.n_regs; reg++) {
        ev->pis[n_cis + reg] = ev->value[first_po + n_cos + reg];
    }
    evaluate(ev);
}

/* The outputs that are output ports, against the simulation; how many were compared. */
static uint32_t compare_cos(const odin3_sim *sim, const evaluator *ev) {
    uint32_t compared = 0;
    uint32_t n_cos = aig.n_pos - aig.n_regs;
    uint32_t first_po = n_objs() - aig.n_pos;
    for (uint32_t co = 0; co < n_cos; co++) {
        int port = find_port(
            net_text((odin3_net_id){((const uint32_t *)staged.region.cos.data)[co]}), false, sim);
        bool want = false;
        if (port < 0) {
            continue;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_sim_get_output(sim, (odin3_sim_bit){(uint32_t)port, 0}, &want));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want ? 1U : 0U, ev->value[first_po + co],
                                        "the AIG and the simulation differ");
        compared++;
    }
    return compared;
}

/* One seed: the AIG, a sequential evaluation with the registers at their sent init 0, against
 * the simulation of the module. Returns how many output values were compared. */
static uint32_t compare_seed(uint32_t seed) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_build(design, odin3_design_top(design), &sim));
    evaluator ev;
    memset(&ev, 0, sizeof ev);
    odin3_prng prng;
    odin3_prng_seed(&prng, seed);
    uint32_t compared = 0;
    for (uint32_t cycle = 0; cycle < ASIM_CYCLES; cycle++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        read_cis(sim, &ev);
        step_aig(&ev);
        compared += compare_cos(sim, &ev);
    }
    odin3_sim_destroy(sim);
    return compared;
}

static void expect_equal_to_sim(const char *name) {
    char path[AFX_PATH];
    (void)snprintf(path, sizeof path, "%s/tests/unit/data/abc/%s.blif", ODIN3_SOURCE_DIR, name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    mod = afx_top(design);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    uint32_t compared = 0;
    for (uint32_t seed = 1; seed <= ASIM_SEEDS; seed++) {
        compared += compare_seed(seed);
    }
    TEST_ASSERT_TRUE_MESSAGE(compared > 0, name);
    free_all();
}

static void test_equal_t1(void) {
    expect_equal_to_sim("t1");
}

static void test_equal_toggle_init1(void) {
    expect_equal_to_sim("toggle_init1");
}

static void test_equal_scorr_merge_init1(void) {
    expect_equal_to_sim("scorr_merge_init1");
}

static void test_equal_two_ports(void) {
    expect_equal_to_sim("two_ports");
}

static void test_equal_keep_net(void) {
    expect_equal_to_sim("keep_net");
}

/* --- the engine ---------------------------------------------------------------------------- */

/* The fake engine's get_luts names every register PI with the index it was loaded with. */
static void test_pi_names(void) {
    load_text((blif_text){"two_regs.blif", ".model tr\n.inputs clk a b\n.outputs q1 q2\n"
                                           ".latch a q1 re clk 0\n.latch b q2 re clk 0\n.end\n"});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    const odin3_abc_engine *eng = odin3_abc_fake_engine();
    odin3_abc_fail fail = {0};
    odin3_abc_aig view = odin3_abc_aig_view(&aig);
    odin3_abc_luts luts = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->open(eng->self, NULL, NULL, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->load_aig(eng->self, &view, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->get_luts(eng->self, &luts, &fail));
    TEST_ASSERT_EQUAL_UINT32(2, luts.n_regs);
    TEST_ASSERT_NOT_NULL(luts.ci_from);
    for (int32_t pi = 0; pi < (int32_t)aig.n_pis; pi++) {
        TEST_ASSERT_EQUAL_INT32(pi, luts.ci_from[pi]);
    }
    eng->close(eng->self);
    free_all();
}

/* The session's commands: open, load_aig, &put, the script, `&get -m -n`, get_luts, close. */
static void test_session_commands(void) {
    load_t1(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1, odin3_abc_fake_open_count());
    const char *cmds = odin3_abc_fake_last_commands();
    TEST_ASSERT_EQUAL_INT(0,
                          strncmp(cmds, "read ", 5)); /* the BLIF exchange is still the default */
}

static void test_session_commands_aig(void) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", "aig", 1));
    load_t1(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")), afx_errors());
    afx_check(design);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_abc_fake_open_count());
    const char *cmds = odin3_abc_fake_last_commands();
    TEST_ASSERT_EQUAL_INT(0, strncmp(cmds, "&put\n", 5));
    TEST_ASSERT_NULL(strstr(cmds, "write_hie"));
    TEST_ASSERT_NOT_NULL(strstr(cmds, "&get -m -n\n"));
    TEST_ASSERT_EQUAL_UINT32(1, afx_count(ODIN3_LOG_INFO, "abc: script "));
    TEST_ASSERT_NOT_NULL(strstr(afx_only(ODIN3_LOG_INFO, "abc: script "), "exchange aig"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
}

/* A failing internal command: `abc: internal command '<cmd>' failed`, nothing changed. */
static void test_internal_command_failure(void) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", "aig", 1));
    load_t1(0);
    char *before = afx_blif_text(design);
    odin3_abc_fake_fail_on("&put", (odin3_abc_fail){ODIN3_ABC_FAIL_COMMAND, 1, ""});
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_ERROR, "abc: internal command '&put' failed"));
    afx_check(design);
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
}

/* --- the aligned MiniLUT ------------------------------------------------------------------- */

/* Three input registers: k = 0 on a, k = 1 and k = 2 both on b. scorr merges 1 and 2 into one
 * result register named after k = 1, so the result has two registers and the aligned MiniLUT has
 * three register outputs, with the register input of k = 2 on the constant 0. */
static void test_aligned_after_scorr_merge(void) {
    load_text((blif_text){"merge3.blif", ".model m3\n.inputs clk a b\n.outputs q0 q1 q2\n"
                                         ".latch a q0 re clk 0\n.latch b q1 re clk 0\n"
                                         ".latch b q2 re clk 0\n.end\n"});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    const odin3_abc_engine *eng = odin3_abc_fake_engine();
    odin3_abc_fail fail = {0};
    odin3_abc_aig view = odin3_abc_aig_view(&aig);
    odin3_abc_luts luts = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->open(eng->self, NULL, NULL, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->load_aig(eng->self, &view, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->exec(eng->self, "scorr", &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->get_luts(eng->self, &luts, &fail));
    TEST_ASSERT_EQUAL_UINT32(2, luts.n_regs);
    odin3_arena *arena = odin3_arena_create(0);
    odin3_abc_luts aligned = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_aligned_build(staged.run, &luts, arena, &aligned));
    TEST_ASSERT_EQUAL_UINT32(3, aligned.n_regs);
    TEST_ASSERT_EQUAL_UINT32(2, aligned.lut_size);
    /* objects: 0, 1, PIs a b + three registers, no LUT, POs q0 q1 q2 + three register inputs */
    TEST_ASSERT_EQUAL_UINT32(2U + 2U + 3U + 0U + 3U + 3U, aligned.n_objs);
    const uint32_t first_po = 2U + 2U + 3U;
    const uint32_t reg_pos = first_po + 3U; /* the register inputs of k = 0, 1, 2 */
    const size_t width = aligned.lut_size;
    TEST_ASSERT_EQUAL_INT(2, aligned.fanins[reg_pos * width]);        /* a: PI 0 */
    TEST_ASSERT_EQUAL_INT(3, aligned.fanins[(reg_pos + 1U) * width]); /* b: PI 1 */
    TEST_ASSERT_EQUAL_INT(0, aligned.fanins[(reg_pos + 2U) * width]); /* constant 0 */
    for (uint32_t po = first_po; po < aligned.n_objs; po++) {
        TEST_ASSERT_EQUAL_INT(0x7FFFFFFE, aligned.fanins[(po * width) + 1U]);
    }
    odin3_arena_destroy(arena);
    eng->close(eng->self);
    free_all();
}

/* --- the register correspondence ----------------------------------------------------------- */

/* A hand-made MiniLUT of one real input and one register whose PI names ci_from[1]; the result
 * is checked against expect (1 input, 1 output, 1 register, no K). */
static odin3_status from_luts_with_name(int32_t name, bool has_names) {
    static const int32_t fanins[6 * 2] = {
        0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, /* constants */
        0x7FFFFFFF, 0x7FFFFFFF,                         /* PI: the input */
        0x7FFFFFFF, 0x7FFFFFFF,                         /* PI: the register */
        2,          0x7FFFFFFE,                         /* PO y: reads the input */
        3,          0x7FFFFFFE};                        /* PO: the register's D reads it */
    static const uint32_t truths[6 * 1] = {0, UINT32_MAX, 0, 0, 0, 0};
    int32_t names[2] = {0, name};
    odin3_abc_luts luts = {fanins, truths, has_names ? names : NULL, 6, 1, 2};
    odin3_abc_result_expect expect = {1, 1, 1, 0};
    odin3_arena *arena = odin3_arena_create(0);
    odin3_abc_result res;
    odin3_status st = odin3_abc_result_from_luts(&luts, &expect, arena, &res);
    odin3_abc_result_free(&res);
    odin3_arena_destroy(arena);
    return st;
}

static void test_result_names_checked(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, from_luts_with_name(1, true));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, from_luts_with_name(0, true)); /* a real input's name */
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_ERROR,
                             "abc: ABC returned a malformed network: register 0 carries no input "
                             "register's name"));
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, from_luts_with_name(-1, true));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, from_luts_with_name(1, false)); /* no names at all */
    TEST_ASSERT_EQUAL_UINT32(
        2, afx_count(ODIN3_LOG_ERROR, "abc: ABC returned a malformed network: register 0"));
}

/* --- every CO and register input against the IR, cell by cell ---------------------------- */

/* The IR's own values for one random vector: rd[net] is what a reader of the net sees (a CI, a
 * register's value, a constant, else the driver's output), drv[net] what the soft driver computes
 * (they differ on a loop-cut net). Independent of the builder's trees: each cell is evaluated
 * straight from its table or its rows. */
typedef struct irvals {
    uint8_t rd[MAX_VALUES];
    uint8_t drv[MAX_VALUES];
    uint8_t reg[MAX_PIS]; /* each register's IR value Q */
} irvals;

static bool ir_read(const irvals *iv, odin3_net_id net) {
    int lit = odin3_abc_pin_literal(staged.run, net);
    if (!odin3_net_valid(net) || lit == 0) {
        return false;
    }
    return lit == 1 || iv->rd[net.v] != 0;
}

/* The inputs of node in port order as bits of an index (input j is bit j; at most 64). */
static uint64_t ir_inputs(const irvals *iv, odin3_node_id node, uint32_t *count) {
    uint64_t index = 0;
    *count = 0;
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_dir(mod, pin) == ODIN3_DIR_IN) {
            index |= (uint64_t)(ir_read(iv, odin3_pin_net(mod, pin)) ? 1U : 0U) << (*count)++;
        }
    }
    return index;
}

static bool ir_sop(const odin3_value *cover, uint64_t index) {
    uint32_t inputs = cover->cover_inputs;
    size_t stride = (size_t)inputs + 1U;
    bool any = false;
    bool off_set = false;
    for (size_t off = 0; off + stride <= cover->len; off += stride) {
        bool match = true;
        for (uint32_t col = 0; col < inputs; col++) {
            char want = (char)cover->bits[off + col];
            bool bit = ((index >> col) & 1U) != 0;
            match = match && (want == '-' || (want == '1') == bit);
        }
        any = any || match;
        off_set = cover->bits[off + inputs] == '0';
    }
    return off_set ? !any : any;
}

static bool ir_cell(const irvals *iv, odin3_node_id node) {
    uint32_t counted = 0;
    uint64_t index = ir_inputs(iv, node, &counted);
    odin3_celltype_id type = odin3_node_type(mod, node);
    uint64_t table = 0;
    uint32_t gate_inputs = 0;
    if (odin3_abc_gate_table(staged.run, type, &table, &gate_inputs) == ODIN3_OK) {
        return ((table >> index) & 1U) != 0;
    }
    if (strcmp(odin3_celltype_get(design, type)->name, "$lut") == 0) {
        uint64_t words[ODIN3_LUT_MAX_WORDS];
        odin3_value params[2] = {*odin3_node_param(mod, node, ODIN3_LUT_P_WIDTH),
                                 *odin3_node_param(mod, node, ODIN3_LUT_P_LUT)};
        TEST_ASSERT_TRUE(odin3_lut_table(params, words, ODIN3_LUT_MAX_WORDS));
        return ((words[index / 64U] >> (index % 64U)) & 1U) != 0;
    }
    return ir_sop(odin3_node_param(mod, node, 1), index);
}

static odin3_net_id out_net(odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

static bool reg_inverted(uint32_t reg) {
    odin3_node_id node = {((const uint32_t *)staged.region.regs.data)[reg]};
    const odin3_value *init = odin3_node_param(mod, node, 0);
    return init != NULL && init->kind == ODIN3_VAL_INT && init->i == 1;
}

/* Random CIs and register values (rd), a register's Q net reads its value. */
static void ir_seed(irvals *iv, odin3_prng *prng) {
    const odin3_abc_region *region = &staged.region;
    memset(iv, 0, sizeof *iv);
    for (size_t ci = 0; ci < region->cis.len; ci++) {
        iv->rd[((const uint32_t *)region->cis.data)[ci]] = odin3_prng_bit(prng) ? 1U : 0U;
    }
    for (uint32_t net = 1; net < odin3_module_net_end(mod); net++) {
        uint32_t reg = ((const uint32_t *)region->q_of.data)[net];
        if (reg != 0) {
            iv->reg[reg - 1U] = odin3_prng_bit(prng) ? 1U : 0U;
            iv->rd[net] = iv->reg[reg - 1U];
        }
    }
}

/* One random vector: the seed values, then the soft cells in region order. */
static void ir_vector(irvals *iv, odin3_prng *prng) {
    const odin3_abc_region *region = &staged.region;
    ir_seed(iv, prng);
    for (size_t at = 0; at < region->soft.len; at++) {
        odin3_node_id node = {((const uint32_t *)region->soft.data)[at]};
        odin3_net_id net = out_net(node);
        uint8_t value = ir_cell(iv, node) ? 1U : 0U;
        if (odin3_net_valid(net)) {
            iv->drv[net.v] = value;
            iv->rd[net.v] =
                ((const uint32_t *)region->ci_of.data)[net.v] == 0 ? value : iv->rd[net.v];
        }
    }
}

/* The AIG on the same vector: PIs are the CIs, then each register's Q xor its inversion. */
static void aig_vector(const irvals *iv, evaluator *ev) {
    const odin3_abc_region *region = &staged.region;
    for (size_t ci = 0; ci < region->cis.len; ci++) {
        ev->pis[ci] = iv->rd[((const uint32_t *)region->cis.data)[ci]];
    }
    for (uint32_t reg = 0; reg < aig.n_regs; reg++) {
        ev->pis[region->cis.len + reg] = iv->reg[reg] ^ (reg_inverted(reg) ? 1U : 0U);
    }
    evaluate(ev);
}

/* Every real CO and every register input of the AIG against the IR on this vector. */
static void expect_pos_equal(const irvals *iv, const evaluator *ev) {
    const odin3_abc_region *region = &staged.region;
    uint32_t first_po = n_objs() - aig.n_pos;
    for (size_t co = 0; co < region->cos.len; co++) {
        uint32_t net = ((const uint32_t *)region->cos.data)[co];
        uint32_t reg = ((const uint32_t *)region->q_of.data)[net];
        uint8_t want = reg != 0 ? iv->reg[reg - 1U] : iv->drv[net];
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, ev->value[first_po + co], "a CO differs");
    }
    for (uint32_t reg = 0; reg < aig.n_regs; reg++) {
        odin3_net_id dnet = {((const uint32_t *)region->reg_d.data)[reg]};
        uint8_t want = (ir_read(iv, dnet) ? 1U : 0U) ^ (reg_inverted(reg) ? 1U : 0U);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, ev->value[first_po + region->cos.len + reg],
                                        "a register input differs");
    }
}

static void expect_comb_equal(const char *name) {
    char path[AFX_PATH];
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
    (void)snprintf(path, sizeof path, "%s/tests/unit/data/abc/%s.blif", ODIN3_SOURCE_DIR, name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, path));
    mod = afx_top(design);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, build_aig(), afx_errors());
    static irvals iv;
    static evaluator ev;
    for (uint32_t seed = 1; seed <= ASIM_SEEDS; seed++) {
        odin3_prng prng;
        odin3_prng_seed(&prng, seed);
        for (uint32_t vec = 0; vec < ASIM_CYCLES; vec++) {
            memset(&ev, 0, sizeof ev);
            ir_vector(&iv, &prng);
            aig_vector(&iv, &ev);
            expect_pos_equal(&iv, &ev);
        }
    }
    free_all();
}

/* All eight data designs, including the three the simulator cannot run (a derived clock, a
 * combinational loop with its cut net, a hard block): CIs and register values are random, the IR
 * is evaluated cell by cell. */
static void test_comb_equal_t1(void) {
    expect_comb_equal("t1");
}

static void test_comb_equal_toggle_init1(void) {
    expect_comb_equal("toggle_init1");
}

static void test_comb_equal_scorr_merge_init1(void) {
    expect_comb_equal("scorr_merge_init1");
}

static void test_comb_equal_two_ports(void) {
    expect_comb_equal("two_ports");
}

static void test_comb_equal_keep_net(void) {
    expect_comb_equal("keep_net");
}

static void test_comb_equal_derived_clock(void) {
    expect_comb_equal("derived_clock");
}

static void test_comb_equal_loop(void) {
    expect_comb_equal("loop");
}

static void test_comb_equal_ram_unconn(void) {
    expect_comb_equal("ram_unconn");
}

/* --- cover and size checks ----------------------------------------------------------------- */

/* A $sop whose rows disagree on the output polarity is refused, not built as a wrong function. */
static void test_mixed_polarity_cover_refused(void) {
    new_module();
    odin3_net_id in_a = input("a");
    odin3_net_id in_b = input("b");
    odin3_net_id out_y = output("y");
    odin3_value params[2] = {odin3_value_int(2), {.kind = ODIN3_VAL_COVER}};
    static const uint8_t rows[6] = {'1', '-', '1', '-', '1', '0'};
    params[1].bits = rows;
    params[1].len = sizeof rows;
    params[1].cover_inputs = 2;
    odin3_net_id nets[3] = {in_a, in_b, out_y};
    odin3_netvec ports[2] = {{nets, 2}, {&nets[2], 1}};
    odin3_node_spec spec = {afx_type(design, "$sop"), 0, (odin3_prov_id){0}, params, 2};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    odin3_status st = build_aig();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, st, afx_errors());
    TEST_ASSERT_EQUAL_UINT32(1, afx_count(ODIN3_LOG_ERROR, "abc: module 'm': a cover has a row"));
    free_all();
}

/* A MiniLUT with lut_size 1 is malformed for the import and for the aligned build. */
static void test_lut_size_checked(void) {
    static const int32_t fanins[4] = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
    static const uint32_t truths[4] = {0, UINT32_MAX, 0, 0};
    odin3_abc_luts luts = {fanins, truths, NULL, 4, 0, 1};
    odin3_abc_result_expect expect = {0, 0, 0, 0};
    odin3_arena *arena = odin3_arena_create(0);
    odin3_abc_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_result_from_luts(&luts, &expect, arena, &res));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_ERROR,
                             "abc: ABC returned a malformed network: a LUT size outside 2 to 16"));
    odin3_abc_result_free(&res);
    load_t1(0);
    afx_stage_region(design, mod, &staged);
    odin3_abc_luts aligned = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_abc_aligned_build(staged.run, &luts, arena, &aligned));
    odin3_arena_destroy(arena);
    afx_unstage(&staged);
    staged = (afx_staged){0};
}

/* --- the cap ------------------------------------------------------------------------------- */

static void test_cap(void) {
    load_text((blif_text){
        "cube5.blif",
        ".model c5\n.inputs a b c d e\n.outputs y\n.names a b c d e y\n11111 1\n.end\n"});
    odin3_abc_run_limits limits = {0};
    limits.aig_objs = 10;
    odin3_abc_test_set_run_limits(&limits);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, build_aig());
    TEST_ASSERT_TRUE_MESSAGE(
        afx_has(ODIN3_LOG_ERROR,
                "abc: module 'c5', no flip-flops: the AIG needs more than 10 objects"),
        afx_errors());
    free_all();
}

/* --- out of memory ------------------------------------------------------------------------- */

/* Every allocation of aig_build fails in turn: NO_MEMORY, and a success after the last. */
static void test_oom_aig_build(void) {
    load_t1(0);
    afx_stage_region(design, mod, &staged);
    long ok_after = -1;
    for (long nth = 0; nth < OOM_LIMIT && ok_after < 0; nth++) {
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_abc_aig_build(staged.run, &staged.region, &aig);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            ok_after = nth;
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
        odin3_abc_aig_free(&aig);
    }
    TEST_ASSERT_TRUE_MESSAGE(ok_after > 0, "the sweep never reached a success");
    free_all();
}

/* Every allocation of result_from_luts fails in turn, on the fake engine's t1 result. */
static void test_oom_result_from_luts(void) {
    load_t1(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_aig());
    const odin3_abc_engine *eng = odin3_abc_fake_engine();
    odin3_abc_fail fail = {0};
    odin3_abc_aig view = odin3_abc_aig_view(&aig);
    odin3_abc_luts luts = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->open(eng->self, NULL, NULL, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->load_aig(eng->self, &view, &fail));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, eng->get_luts(eng->self, &luts, &fail));
    odin3_abc_result_expect expect = {2, 1, 1, 6};
    long ok_after = -1;
    for (long nth = 0; nth < OOM_LIMIT && ok_after < 0; nth++) {
        odin3_arena *arena = odin3_arena_create(0);
        TEST_ASSERT_NOT_NULL(arena);
        odin3_abc_result res;
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = odin3_abc_result_from_luts(&luts, &expect, arena, &res);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            ok_after = nth;
            TEST_ASSERT_EQUAL_UINT32(1, res.n_luts);
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
        odin3_abc_result_free(&res);
        odin3_arena_destroy(arena);
    }
    TEST_ASSERT_TRUE_MESSAGE(ok_after > 0, "the sweep never reached a success");
    eng->close(eng->self);
    free_all();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_layout_init0);
    RUN_TEST(test_layout_init1);
    RUN_TEST(test_hashing);
    RUN_TEST(test_balanced_cube);
    RUN_TEST(test_off_set_and_or);
    RUN_TEST(test_equal_t1);
    RUN_TEST(test_equal_toggle_init1);
    RUN_TEST(test_equal_scorr_merge_init1);
    RUN_TEST(test_equal_two_ports);
    RUN_TEST(test_equal_keep_net);
    RUN_TEST(test_comb_equal_t1);
    RUN_TEST(test_comb_equal_toggle_init1);
    RUN_TEST(test_comb_equal_scorr_merge_init1);
    RUN_TEST(test_comb_equal_two_ports);
    RUN_TEST(test_comb_equal_keep_net);
    RUN_TEST(test_comb_equal_derived_clock);
    RUN_TEST(test_comb_equal_loop);
    RUN_TEST(test_comb_equal_ram_unconn);
    RUN_TEST(test_mixed_polarity_cover_refused);
    RUN_TEST(test_lut_size_checked);
    RUN_TEST(test_pi_names);
    RUN_TEST(test_aligned_after_scorr_merge);
    RUN_TEST(test_result_names_checked);
    RUN_TEST(test_session_commands);
    RUN_TEST(test_session_commands_aig);
    RUN_TEST(test_internal_command_failure);
    RUN_TEST(test_cap);
    RUN_TEST(test_oom_aig_build);
    RUN_TEST(test_oom_result_from_luts);
    return UNITY_END();
}
