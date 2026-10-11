/*
 * test_proc_pw.c — the proc working form (2E spec §3, §4, §17; plan Task 2): decode of every
 * position kind, decode then encode as the identity, the signal operations against hand-computed
 * results, the bit key, encode after an edit, and proc_build (pb_*) against hand-packed bodies.
 *
 * The bodies are 2D Task 3's P1 and P2 (test_ir_proc.c). Yosys re-derivation: none of these tests
 * runs a proc phase, so no yosys-phase command applies.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_LINES = 16,
    LOG_TEXT = 256,
    OOM_LIMIT = 2000,
    ARENA_CHUNK = 1024,
    PBITS_MAX = 16,
    ARR_MAX = 8,
    SAVE_WORDS = 128,
    KEY_SHIFT = 32,
};

#define IN ODIN3_PBIT_IN
#define OUT ODIN3_PBIT_OUT
#define ANY ODIN3_PBIT_ANY

static odin3_design *design;
static odin3_module *module;
static odin3_arena *arena;
static odin3_prov_id prov;
static odin3_pass_ctx run;
static odin3_proc_mctx mctx;
static odin3_celltype_id proc_type;

static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && n_lines < LOG_LINES) {
        (void)snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
    }
    n_lines += level == ODIN3_LOG_ERROR ? 1 : 0;
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id type_named(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, str, &id));
    return id;
}

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("test"), &run));
    odin3_srcloc loc = {intern("t.v"), 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run, &origin, &prov));
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern("m"), prov, &id));
    module = odin3_module_get(design, id);
    arena = odin3_arena_create(ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    proc_type = type_named("$proc");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&mctx, &run, module));
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_mctx_free(&mctx);
    odin3_arena_destroy(arena);
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
}

static void check_full_ok_in(odin3_design *des) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(des, opts));
}

static odin3_wire_id new_wire(const char *name, uint32_t width) {
    odin3_wire_spec spec = {intern(name), (int32_t)width - 1, 0, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &wire));
    return wire;
}

static odin3_net_id wire1(const char *name) {
    return odin3_wire_net(module, new_wire(name, 1), 0);
}

static odin3_net_id const_net(const char *type_name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, &net));
    odin3_node_spec spec = {type_named(type_name), 0, prov, NULL, 0};
    odin3_netvec port = {&net, 1};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, &port, &node));
    return net;
}

/* Makes a new module named name the one the tests (and mctx) work on. */
static void use_module(const char *name) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &id));
    module = odin3_module_get(design, id);
    odin3_proc_mctx_free(&mctx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&mctx, &run, module));
}

/* --- P1 and P2 (2D Task 3's fixtures) ------------------------------------------------------ */

typedef struct body_buf {
    odin3_pbit pbits[PBITS_MAX];
    odin3_pspan compares[ARR_MAX];
    odin3_proc_action actions[ARR_MAX];
    odin3_proc_case cases[ARR_MAX];
    odin3_proc_switch switches[ARR_MAX];
    odin3_proc_sync syncs[ARR_MAX];
    odin3_proc_body body;
} body_buf;

typedef struct body_counts {
    uint32_t pbits, compares, actions, cases, switches, syncs;
} body_counts;

static void link_body(body_buf *buf, body_counts counts) {
    buf->body = (odin3_proc_body){buf->pbits,    counts.pbits,    buf->compares, counts.compares,
                                  buf->actions,  counts.actions,  buf->cases,    counts.cases,
                                  buf->switches, counts.switches, buf->syncs,    counts.syncs};
}

/* P1: I = [clk, d], O = [q, t0]; root t0 <- q, t0 <- d; sync posedge clk: q <- t0. */
static void p1_body(body_buf *buf) {
    memset(buf, 0, sizeof *buf);
    const odin3_pbit pbits[] = {OUT | 1, OUT | 0, OUT | 1, IN | 1, OUT | 0, OUT | 1};
    memcpy(buf->pbits, pbits, sizeof pbits);
    buf->actions[0] = (odin3_proc_action){{0, 1}, {1, 1}, {0}};
    buf->actions[1] = (odin3_proc_action){{2, 1}, {3, 1}, {0}};
    buf->actions[2] = (odin3_proc_action){{4, 1}, {5, 1}, {0}};
    buf->cases[0] = (odin3_proc_case){0, 0, 0, 2, 0, 0, {0}};
    buf->syncs[0] = (odin3_proc_sync){ODIN3_SYNC_POSEDGE, IN | 0, 2, 1};
    link_body(buf, (body_counts){6, 0, 3, 1, 0, 1});
}

/*
 * P2: P1 plus a full_case switch on s (I pin 2) with case 1'1 (I pin 3, on the $_CONST1_ net):
 * t0 <- d, and a default case; the sync action is q <- t0.
 */
static void p2_body(body_buf *buf) {
    memset(buf, 0, sizeof *buf);
    const odin3_pbit pbits[] = {OUT | 1, OUT | 0, OUT | 1, IN | 1,  OUT | 0,
                                OUT | 1, IN | 2,  IN | 3,  OUT | 1, IN | 1};
    memcpy(buf->pbits, pbits, sizeof pbits);
    buf->compares[0] = (odin3_pspan){7, 1};
    buf->actions[0] = (odin3_proc_action){{0, 1}, {1, 1}, {0}};
    buf->actions[1] = (odin3_proc_action){{2, 1}, {3, 1}, {0}};
    buf->actions[2] = (odin3_proc_action){{8, 1}, {9, 1}, {0}};
    buf->actions[3] = (odin3_proc_action){{4, 1}, {5, 1}, {0}};
    buf->cases[0] = (odin3_proc_case){0, 0, 0, 2, 0, 1, {0}};
    buf->cases[1] = (odin3_proc_case){0, 1, 2, 1, 1, 0, {0}};
    buf->cases[2] = (odin3_proc_case){1, 0, 3, 0, 1, 0, {0}};
    buf->switches[0] = (odin3_proc_switch){{6, 1}, 1, 2, ODIN3_SWITCH_FULL_CASE, {0}};
    buf->syncs[0] = (odin3_proc_sync){ODIN3_SYNC_POSEDGE, IN | 0, 3, 1};
    link_body(buf, (body_counts){10, 1, 4, 3, 1, 1});
}

typedef struct p_nets {
    odin3_net_id clk, d, q, t0, s, c1;
} p_nets;

static p_nets p_wires(bool p2) {
    p_nets nets = {wire1("clk"), wire1("d"), wire1("q"), wire1("t0"), {0}, {0}};
    if (p2) {
        nets.s = wire1("s");
        nets.c1 = const_net("$_CONST1_");
    }
    return nets;
}

/* P1 (or P2) on fresh wires; *nets gets them. */
static odin3_node_id p_node(const body_buf *buf, bool p2, p_nets *nets) {
    *nets = p_wires(p2);
    odin3_net_id in[4] = {nets->clk, nets->d, nets->s, nets->c1};
    odin3_net_id outs[2] = {nets->q, nets->t0};
    uint32_t n_in = p2 ? 4 : 2;
    odin3_value params[3] = {odin3_value_int(n_in), odin3_value_int(2), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_proc_body_pack(arena, &buf->body, &params[ODIN3_PROC_P_BODY]));
    odin3_node_spec spec = {proc_type, 0, prov, params, 3};
    odin3_netvec ports[2] = {{in, n_in}, {outs, 2}};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    return node;
}

static odin3_node_id p1_node(p_nets *nets) {
    body_buf buf;
    p1_body(&buf);
    return p_node(&buf, false, nets);
}

static odin3_node_id p2_node(p_nets *nets) {
    body_buf buf;
    p2_body(&buf);
    return p_node(&buf, true, nets);
}

/* --- working-form access ------------------------------------------------------------------- */

static const odin3_pw_case *pw_case(const odin3_pw *pw, uint32_t idx) {
    TEST_ASSERT_TRUE(idx < pw->cases.len);
    return odin3_vec_cat(&pw->cases, idx);
}

static const odin3_pw_action *case_action(const odin3_pw_case *cs, uint32_t idx) {
    TEST_ASSERT_TRUE(idx < cs->actions.len);
    return odin3_vec_cat(&cs->actions, idx);
}

static const odin3_pw_sync *pw_sync(const odin3_pw *pw, uint32_t idx) {
    TEST_ASSERT_TRUE(idx < pw->syncs.len);
    return odin3_vec_cat(&pw->syncs, idx);
}

/* sig holds exactly the n bits of want. */
static void expect_sig(const odin3_pw *pw, odin3_pw_sig sig, const odin3_pw_bit *want, uint32_t n) {
    TEST_ASSERT_EQUAL_UINT32(n, sig.len);
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_HEX32(want[i], bits[i]);
    }
}

static void expect_bit(const odin3_pw *pw, odin3_pw_sig sig, odin3_pw_bit want) {
    expect_sig(pw, sig, &want, 1);
}

/* The action is lhs <- rhs, one bit each. */
static void expect_action(const odin3_pw *pw, const odin3_pw_action *act, odin3_net_id lhs,
                          odin3_pw_bit rhs) {
    expect_bit(pw, act->lhs, lhs.v);
    expect_bit(pw, act->rhs, rhs);
}

static void decode_ok(const odin3_proc_mctx *ctx, odin3_node_id node, odin3_pw *pw) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(ctx, node, pw));
}

/* --- decode -------------------------------------------------------------------------------- */

static void test_decode_p1(void) {
    p_nets nets;
    odin3_node_id node = p1_node(&nets);
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    TEST_ASSERT_EQUAL_UINT32(node.v, pw.node.v);
    TEST_ASSERT_EQUAL_UINT32(prov.v, pw.prov.v);
    TEST_ASSERT_EQUAL_UINT32(0, pw.lowered.v);
    TEST_ASSERT_EQUAL_UINT32(1, pw.cases.len);
    TEST_ASSERT_EQUAL_UINT32(0, pw.switches.len);
    const odin3_pw_case *root = pw_case(&pw, 0);
    TEST_ASSERT_EQUAL_UINT32(2, root->actions.len);
    TEST_ASSERT_EQUAL_UINT32(0, root->compares.len);
    TEST_ASSERT_EQUAL_UINT32(0, root->switches.len);
    expect_action(&pw, case_action(root, 0), nets.t0, nets.q.v);
    expect_action(&pw, case_action(root, 1), nets.t0, nets.d.v);
    TEST_ASSERT_EQUAL_UINT32(1, pw.syncs.len);
    const odin3_pw_sync *sync = pw_sync(&pw, 0);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_SYNC_POSEDGE, sync->type);
    TEST_ASSERT_EQUAL_UINT32(nets.clk.v, sync->signal);
    TEST_ASSERT_EQUAL_UINT32(1, sync->actions.len);
    expect_action(&pw, odin3_vec_cat(&sync->actions, 0), nets.q, nets.t0.v);
    const odin3_net_id want[] = {nets.clk, nets.d, nets.q, nets.t0};
    TEST_ASSERT_EQUAL_UINT32(4, pw.in_nets.len);
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i].v, ((const odin3_net_id *)pw.in_nets.data)[i].v);
    }
    TEST_ASSERT_EQUAL_UINT32(0, pw.attrs.len);
    TEST_ASSERT_EQUAL_UINT32(0, pw.promoted.len);
    odin3_pw_free(&pw);
}

/* P2's compare (I pin 3, on the $_CONST1_ net) decodes to S1; an ANY compare to SA. */
static void test_decode_constants_compare(void) {
    p_nets nets;
    odin3_pw pw;
    decode_ok(&mctx, p2_node(&nets), &pw);
    const odin3_pw_case *one = pw_case(&pw, 1);
    TEST_ASSERT_EQUAL_UINT32(1, one->compares.len);
    expect_bit(&pw, *(const odin3_pw_sig *)odin3_vec_cat(&one->compares, 0), ODIN3_PW_S1);
    TEST_ASSERT_EQUAL_UINT16(ODIN3_SWITCH_FULL_CASE,
                             ((const odin3_pw_switch *)odin3_vec_cat(&pw.switches, 0))->flags);
    odin3_pw_free(&pw);

    use_module("m2");
    body_buf buf;
    p2_body(&buf);
    buf.pbits[7] = ANY;
    odin3_node_id node = p_node(&buf, true, &nets);
    decode_ok(&mctx, node, &pw);
    expect_bit(&pw, *(const odin3_pw_sig *)odin3_vec_cat(&pw_case(&pw, 1)->compares, 0),
               ODIN3_PW_SA);
    odin3_pw_free(&pw);
}

/* Builds text with pb_build and decodes the first process of module m into *pw. */
static void pb_decode(const char *text, pb_result *res, odin3_pw *pw) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, res));
    check_full_ok_in(res->design);
    odin3_proc_mctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&ctx, &res->ctx, res->module));
    decode_ok(&ctx, pb_proc(res, "m", 0), pw);
    odin3_proc_mctx_free(&ctx);
}

/* A wire merged into $_CONST1_'s net reads as S1; so does a sync on that net. */
static void test_decode_constants_merged(void) {
    pb_result res;
    odin3_pw pw;
    pb_decode("module \\m\n  wire \\k\n  wire \\y\n  wire \\t\n  process $p\n"
              "    assign \\t \\k\n    sync always\n      update \\y \\t\n  end\n"
              "  connect \\k 1'1\nend\n",
              &res, &pw);
    expect_bit(&pw, case_action(pw_case(&pw, 0), 0)->rhs, ODIN3_PW_S1);
    odin3_pw_free(&pw);
    pb_free(&res);

    pb_decode("module \\m\n  wire \\d\n  wire \\q\n  process $p\n    sync posedge 1'1\n"
              "      update \\q \\d\n  end\nend\n",
              &res, &pw);
    TEST_ASSERT_EQUAL_HEX32(ODIN3_PW_S1, pw_sync(&pw, 0)->signal);
    odin3_pw_free(&pw);
    pb_free(&res);
}

/* The $_CONSTX_ and $_CONSTZ_ nets read as SX and SZ. */
static void test_decode_constants_xz(void) {
    pb_result res;
    odin3_pw pw;
    pb_decode("module \\m\n  wire width 2 \\t\n  wire width 2 \\y\n  process $p\n"
              "    assign \\t 2'xz\n    sync always\n      update \\y \\t\n  end\nend\n",
              &res, &pw);
    const odin3_pw_bit want[] = {ODIN3_PW_SZ, ODIN3_PW_SX};
    expect_sig(&pw, case_action(pw_case(&pw, 0), 0)->rhs, want, 2);
    odin3_pw_free(&pw);
    pb_free(&res);
}

static void test_decode_attrs(void) {
    p_nets nets;
    odin3_node_id node = p1_node(&nets);
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    odin3_value one = odin3_value_int(1);
    odin3_value src = {ODIN3_VAL_STRING, 0, NULL, 0, intern("t.v:3"), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern("keep"), &one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern("src"), &src));
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    TEST_ASSERT_EQUAL_UINT32(2, pw.attrs.len);
    const odin3_pw_attr *attrs = pw.attrs.data;
    TEST_ASSERT_EQUAL_UINT32(intern("keep"), attrs[0].key);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, attrs[0].value->kind);
    TEST_ASSERT_EQUAL_INT64(1, attrs[0].value->i);
    TEST_ASSERT_EQUAL_UINT32(intern("src"), attrs[1].key);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, attrs[1].value->kind);
    TEST_ASSERT_EQUAL_UINT32(intern("t.v:3"), attrs[1].value->str);
    odin3_pw_free(&pw);
}

/* A node that is not a live $proc, and a position on an unconnected pin, are refused. */
static void test_decode_refusals(void) {
    p_nets nets;
    odin3_node_id node = p1_node(&nets);
    odin3_pw pw;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pw_decode(&mctx, (odin3_node_id){node.v + 1}, &pw));
    TEST_ASSERT_EQUAL_UINT32(0, pw.cases.len);
    odin3_pinslice in = odin3_node_port(module, node, ODIN3_PROC_PORT_I);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(module, (odin3_pin_id){in.first.v + 1}));
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_decode(&mctx, node, &pw));
    TEST_ASSERT_EQUAL_STRING("proc: decode: node 1: position IN|1 is on an unconnected pin",
                             lines[0]);
    TEST_ASSERT_EQUAL_UINT32(0, pw.bits.len);
}

/* --- encode -------------------------------------------------------------------------------- */

typedef struct saved_body {
    uint64_t store[SAVE_WORDS];
    uint32_t len;
} saved_body;

static void save_body(odin3_node_id node, saved_body *out) {
    const odin3_value *val = odin3_node_param(module, node, ODIN3_PROC_P_BODY);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof out->store, val->len);
    memcpy(out->store, val->bits, val->len);
    out->len = val->len;
}

static bool body_is(odin3_node_id node, const saved_body *saved) {
    odin3_value want = {ODIN3_VAL_PROC, 0, (const uint8_t *)saved->store, saved->len, 0, 0};
    return odin3_value_equal(odin3_node_param(module, node, ODIN3_PROC_P_BODY), &want);
}

/* The nets on the pins of port `port` of node, in pin order, into nets (at most 8). */
static uint32_t port_nets(odin3_node_id node, uint32_t port, odin3_net_id *nets) {
    odin3_pinslice pins = odin3_node_port(module, node, port);
    TEST_ASSERT_TRUE(pins.count <= ARR_MAX);
    for (uint32_t i = 0; i < pins.count; i++) {
        nets[i] = odin3_pin_net(module, (odin3_pin_id){pins.first.v + i});
    }
    return pins.count;
}

/* The nets of a node's I and O pins (taken before encode deletes it). */
typedef struct pin_nets {
    odin3_net_id nets[2][ARR_MAX];
    uint32_t count[2];
} pin_nets;

static pin_nets pins_of(odin3_node_id node) {
    pin_nets pins;
    for (uint32_t port = 0; port < 2; port++) {
        pins.count[port] = port_nets(node, port, pins.nets[port]);
    }
    return pins;
}

static void expect_same_pins(const pin_nets *was, odin3_node_id new_node) {
    pin_nets now = pins_of(new_node);
    for (uint32_t port = 0; port < 2; port++) {
        TEST_ASSERT_EQUAL_UINT32(was->count[port], now.count[port]);
        for (uint32_t i = 0; i < was->count[port]; i++) {
            TEST_ASSERT_EQUAL_UINT32(was->nets[port][i].v, now.nets[port][i].v);
        }
    }
}

/* Decode then encode: a new node with the same BODY bytes and pins; the old one is dead. */
static void roundtrip(odin3_node_id node) {
    saved_body saved;
    save_body(node, &saved);
    pin_nets was = pins_of(node);
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&mctx, &pw));
    TEST_ASSERT_NOT_EQUAL_UINT32(node.v, pw.node.v);
    TEST_ASSERT_TRUE(odin3_node_live(module, pw.node));
    TEST_ASSERT_FALSE(odin3_node_live(module, node));
    TEST_ASSERT_TRUE(body_is(pw.node, &saved));
    expect_same_pins(&was, pw.node);
    TEST_ASSERT_EQUAL_UINT32(prov.v, odin3_node_prov(module, pw.node).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(0, mctx.touched.len);
    odin3_pw_free(&pw);
    check_full_ok_in(design);
}

static void test_roundtrip_identity_p1(void) {
    p_nets nets;
    roundtrip(p1_node(&nets));
}

static void test_roundtrip_identity_p2(void) {
    p_nets nets;
    roundtrip(p2_node(&nets));
}

/* Encode copies the attributes onto the new node. */
static void test_encode_keeps_attrs(void) {
    p_nets nets;
    odin3_node_id node = p1_node(&nets);
    odin3_value one = odin3_value_int(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                                   intern("keep"), &one));
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&mctx, &pw));
    const odin3_value *val =
        odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_NODE, pw.node.v}, intern("keep"));
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT64(1, val->i);
    odin3_pw_free(&pw);
}

/* pb_print of node, as a C string the caller frees with odin3_strbuf_free. */
static odin3_strbuf print_of(const odin3_module *mod, odin3_node_id node) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(mod, node, &text));
    return text;
}

/* Text without [from, the end of the first line after from that is exactly end_line). */
static void cut_block(char *text, const char *from, const char *end_line) {
    char *start = strstr(text, from);
    TEST_ASSERT_NOT_NULL(start);
    char *stop = strstr(start, end_line);
    TEST_ASSERT_NOT_NULL(stop);
    stop += strlen(end_line);
    memmove(start, stop, strlen(stop) + 1);
}

static void test_encode_after_edit(void) {
    p_nets nets;
    odin3_node_id node = p2_node(&nets);
    odin3_strbuf before = print_of(module, node);
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    odin3_vec_clear(&((odin3_pw_case *)odin3_vec_at(&pw.cases, 0))->switches);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&mctx, &pw));
    odin3_proc_body body;
    TEST_ASSERT_TRUE(
        odin3_proc_body_view(odin3_node_param(module, pw.node, ODIN3_PROC_P_BODY), &body));
    odin3_proc_widths widths = {2, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_verify(&body, widths, NULL));
    odin3_strbuf after = print_of(module, pw.node);
    cut_block(before.data, "  attribute \\full_case 1\n", "\n  end\n");
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
    odin3_pw_free(&pw);
    check_full_ok_in(design);
}

/* --- signal operations --------------------------------------------------------------------- */

typedef struct sig_nets {
    odin3_pw_bit a, b, c, p, q, r;
} sig_nets;

static sig_nets sig_wires(void) {
    odin3_net_id net_c = wire1("c"); /* created first: sort_unify orders by wire ID */
    odin3_net_id net_a = wire1("a");
    odin3_net_id net_b = wire1("b");
    sig_nets nets = {net_a.v, net_b.v, net_c.v, wire1("p").v, wire1("q").v, wire1("r").v};
    return nets;
}

static void pw_empty(odin3_pw *pw) {
    memset(pw, 0, sizeof *pw);
    odin3_vec_init(&pw->bits, sizeof(odin3_pw_bit));
}

static odin3_pw_sig sig_of(odin3_pw *pw, const odin3_pw_bit *bits, uint32_t n) {
    odin3_pw_sig sig = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_new(pw, bits, n, &sig));
    return sig;
}

#define SIG(pw, ...)                                                                               \
    sig_of(pw, (const odin3_pw_bit[]){__VA_ARGS__},                                                \
           (uint32_t)(sizeof((const odin3_pw_bit[]){__VA_ARGS__}) / sizeof(odin3_pw_bit)))

static void test_sig_replace_remove2_extract(void) {
    sig_nets sn = sig_wires();
    odin3_pw pw;
    pw_empty(&pw);
    odin3_pw_sig abc = SIG(&pw, sn.a, sn.b, sn.c);
    odin3_pw_sig pqr = SIG(&pw, sn.p, sn.q, sn.r);
    odin3_pw_replace req = {abc, SIG(&pw, sn.b), SIG(&pw, ODIN3_PW_S1), abc};
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_replace(&mctx, &pw, &req, &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){sn.a, ODIN3_PW_S1, sn.c}, 3);
    odin3_pw_pair pair = {{0, 0}, {0, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_remove2(&mctx, &pw, (odin3_pw_pair){abc, pqr},
                                                         SIG(&pw, sn.a, sn.c), &pair));
    expect_bit(&pw, pair.a, sn.b);
    expect_bit(&pw, pair.b, sn.q);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_extract(&mctx, &pw, (odin3_pw_pair){abc, pqr},
                                                         SIG(&pw, sn.c, sn.a), &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){sn.p, sn.r}, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_extract(&mctx, &pw, (odin3_pw_pair){abc, {0, 0}},
                                                         SIG(&pw, sn.c, sn.a), &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){sn.a, sn.c}, 2);
    TEST_ASSERT_EQUAL_UINT32(0, mctx.touched.len); /* the index is reset after each */
    odin3_pw_free(&pw);
}

/* Constants in the pattern match nothing; constants in sig are never removed; widths checked. */
static void test_sig_constants_and_widths(void) {
    sig_nets sn = sig_wires();
    odin3_pw pw;
    pw_empty(&pw);
    odin3_pw_sig sig = SIG(&pw, ODIN3_PW_S0, sn.a);
    odin3_pw_replace req = {sig, SIG(&pw, ODIN3_PW_S0, sn.a), SIG(&pw, sn.p, sn.q), sig};
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_replace(&mctx, &pw, &req, &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){ODIN3_PW_S0, sn.q}, 2);
    odin3_pw_pair pair = {{0, 0}, {0, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_remove2(&mctx, &pw, (odin3_pw_pair){sig, {0, 0}},
                                                         SIG(&pw, ODIN3_PW_S0, sn.a), &pair));
    expect_sig(&pw, pair.a, (const odin3_pw_bit[]){ODIN3_PW_S0}, 1);
    TEST_ASSERT_EQUAL_UINT32(0, pair.b.len);
    req.other = SIG(&pw, sn.a);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_sig_replace(&mctx, &pw, &req, &out));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        odin3_pw_sig_extract(&mctx, &pw, (odin3_pw_pair){sig, SIG(&pw, sn.p)}, sig, &out));
    odin3_pw_free(&pw);
}

typedef bool (*sig_pred)(const odin3_pw *pw, odin3_pw_sig sig);

/* One predicate on a two-bit signal and its expected answer. */
typedef struct pred_case {
    sig_pred pred;
    odin3_pw_bit bits[2];
    bool want;
} pred_case;

static void test_sig_predicates(void) {
    sig_nets sn = sig_wires();
    const pred_case cases[] = {
        {odin3_pw_sig_is_fully_def, {ODIN3_PW_S0, ODIN3_PW_S1}, true},
        {odin3_pw_sig_is_fully_def, {ODIN3_PW_S0, ODIN3_PW_SX}, false},
        {odin3_pw_sig_is_fully_undef, {ODIN3_PW_SX, ODIN3_PW_SZ}, true},
        {odin3_pw_sig_is_fully_undef, {ODIN3_PW_SX, ODIN3_PW_S0}, false},
        {odin3_pw_sig_as_bool, {ODIN3_PW_S0, ODIN3_PW_S1}, true},
        {odin3_pw_sig_as_bool, {ODIN3_PW_S0, ODIN3_PW_SX}, false},
        {odin3_pw_sig_is_fully_const, {ODIN3_PW_S0, ODIN3_PW_SA}, true},
        {odin3_pw_sig_is_fully_const, {ODIN3_PW_S0, sn.a}, false},
    };
    odin3_pw pw;
    pw_empty(&pw);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        odin3_pw_sig sig = sig_of(&pw, cases[i].bits, 2);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].want, cases[i].pred(&pw, sig), "predicate case");
    }
    odin3_pw_free(&pw);
}

static void test_sig_equal(void) {
    sig_nets sn = sig_wires();
    odin3_pw pw;
    pw_empty(&pw);
    TEST_ASSERT_TRUE(
        odin3_pw_sig_equal(&pw, SIG(&pw, sn.a, ODIN3_PW_S1), SIG(&pw, sn.a, ODIN3_PW_S1)));
    TEST_ASSERT_FALSE(
        odin3_pw_sig_equal(&pw, SIG(&pw, sn.a, ODIN3_PW_S1), SIG(&pw, sn.a, ODIN3_PW_SX)));
    TEST_ASSERT_FALSE(odin3_pw_sig_equal(&pw, SIG(&pw, sn.a), SIG(&pw, sn.a, sn.a)));
    odin3_pw_free(&pw);
}

static void test_sig_sort_unify(void) {
    sig_nets sn = sig_wires();
    odin3_pw pw;
    pw_empty(&pw);
    odin3_pw_sig out = {0, 0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pw_sig_sort_unify(&mctx, &pw, SIG(&pw, sn.a, sn.b, sn.a, sn.c), &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){sn.c, sn.a, sn.b}, 3);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_pw_sig_sort_unify(&mctx, &pw,
                                SIG(&pw, sn.b, ODIN3_PW_SX, ODIN3_PW_S1, sn.b, ODIN3_PW_S1), &out));
    expect_sig(&pw, out, (const odin3_pw_bit[]){ODIN3_PW_S1, ODIN3_PW_SX, sn.b}, 3);
    TEST_ASSERT_EQUAL_UINT32(0, mctx.touched.len);
    odin3_pw_free(&pw);
}

/* sig_new may copy from pw->bits itself, even when the append moves them. */
static void test_sig_new_from_own_bits(void) {
    sig_nets sn = sig_wires();
    odin3_pw pw;
    pw_empty(&pw);
    odin3_pw_sig sig = SIG(&pw, sn.a, sn.b, sn.c, sn.p, sn.q, sn.r, sn.a, sn.b);
    for (uint32_t i = 0; i < 6; i++) { /* 8, 16, ... 512 bits: the vec grows past its capacity */
        sig = sig_of(&pw, odin3_pw_sig_bits(&pw, sig), sig.len);
        odin3_pw_sig twice = {0, 0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_pw_sig_new(&pw, odin3_pw_sig_bits(&pw, sig), sig.len, &twice));
        sig = (odin3_pw_sig){sig.first, sig.len * 2};
        TEST_ASSERT_EQUAL_UINT32(sig.first + sig.len / 2, twice.first);
    }
    const odin3_pw_bit *bits = odin3_pw_sig_bits(&pw, sig);
    for (uint32_t i = 0; i < sig.len; i++) {
        const odin3_pw_bit want[] = {sn.a, sn.b, sn.c, sn.p, sn.q, sn.r, sn.a, sn.b};
        TEST_ASSERT_EQUAL_HEX32(want[i % 8], bits[i]);
    }
    odin3_pw_free(&pw);
}

/* --- the bit key --------------------------------------------------------------------------- */

static void test_bit_key(void) {
    odin3_net_id w1 = wire1("w1");
    (void)wire1("w2");
    odin3_net_id w3_2 = odin3_wire_net(module, new_wire("w3", 4), 2);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_primary(module, w1).wire.v);
    TEST_ASSERT_EQUAL_UINT64((UINT64_C(3) << KEY_SHIFT) | 2, odin3_proc_bit_key(module, w3_2.v));
    TEST_ASSERT_EQUAL_UINT64(1, odin3_proc_bit_key(module, ODIN3_PW_S1));
    TEST_ASSERT_TRUE(odin3_proc_bit_key(module, ODIN3_PW_SM) < odin3_proc_bit_key(module, w1.v));
}

/* After a merge, the key of the dropped net's wire bit is the kept net's. */
static void test_bit_key_merged(void) {
    odin3_net_id a0 = wire1("a");
    odin3_wire_id wire_b = new_wire("b", 1);
    odin3_net_id b0 = odin3_wire_net(module, wire_b, 0);
    uint64_t key_a = odin3_proc_bit_key(module, a0.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){a0, b0}));
    TEST_ASSERT_EQUAL_UINT32(a0.v, odin3_wire_net(module, wire_b, 0).v);
    TEST_ASSERT_EQUAL_UINT64(key_a,
                             odin3_proc_bit_key(module, odin3_wire_net(module, wire_b, 0).v));
}

/* A net with no wire: UINT32_MAX, then its ID. */
static void test_bit_key_unnamed(void) {
    odin3_net_id net = {0};
    while (net.v < 17) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, &net));
    }
    TEST_ASSERT_EQUAL_UINT32(17, net.v);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)UINT32_MAX << KEY_SHIFT) | 17,
                             odin3_proc_bit_key(module, net.v));
    TEST_ASSERT_TRUE(odin3_proc_bit_key(module, ODIN3_PW_S1) < odin3_proc_bit_key(module, net.v));
}

/* --- constant nets ------------------------------------------------------------------------- */

/* The first live constant node's net is found, else one is made; the answer is kept. */
static void test_const_net(void) {
    odin3_net_id made = {0};
    uint32_t nodes = odin3_module_node_end(module);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_const_net(&mctx, ODIN3_PW_SX, &made));
    TEST_ASSERT_EQUAL_UINT32(nodes + 1, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X, odin3_net_const_value(module, made));
    odin3_net_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_const_net(&mctx, ODIN3_PW_SX, &again));
    TEST_ASSERT_EQUAL_UINT32(made.v, again.v);
    odin3_net_id one = const_net("$_CONST1_");
    (void)const_net("$_CONST1_");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_const_net(&mctx, ODIN3_PW_S1, &again));
    TEST_ASSERT_EQUAL_UINT32(one.v, again.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_const_net(&mctx, ODIN3_PW_SA, &again));
    check_full_ok_in(design);
}

/* --- proc_build ---------------------------------------------------------------------------- */

static const char *const k_p1_text = "module \\m\n"
                                     "  wire \\clk\n"
                                     "  wire \\d\n"
                                     "  wire \\q\n"
                                     "  wire \\t0\n"
                                     "  process $proc$t.v:1$1\n"
                                     "    assign \\t0 \\q\n"
                                     "    assign \\t0 \\d\n"
                                     "    sync posedge \\clk\n"
                                     "      update \\q \\t0\n"
                                     "  end\n"
                                     "end\n";

/* The node's BODY equals the built node's. */
static void expect_same_body(odin3_node_id node, const pb_result *res, odin3_node_id built) {
    TEST_ASSERT_TRUE(odin3_node_valid(built));
    TEST_ASSERT_TRUE(odin3_value_equal(odin3_node_param(module, node, ODIN3_PROC_P_BODY),
                                       odin3_node_param(res->module, built, ODIN3_PROC_P_BODY)));
}

static void test_pb_matches_pack(void) {
    p_nets nets;
    odin3_node_id node = p1_node(&nets);
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_p1_text, &res));
    check_full_ok_in(res.design);
    odin3_node_id built = pb_proc(&res, "m", 0);
    TEST_ASSERT_FALSE(odin3_node_valid(pb_proc(&res, "m", 1)));
    expect_same_body(node, &res, built);
    odin3_strbuf want = print_of(module, node);
    odin3_strbuf got = print_of(res.module, built);
    TEST_ASSERT_EQUAL_STRING(want.data, got.data);
    TEST_ASSERT_NULL(strstr(got.data, "process"));
    odin3_strbuf_free(&want);
    odin3_strbuf_free(&got);
    pb_free(&res);
}

static const char *const k_grammar_text = "module \\sub\n"
                                          "  wire input 1 \\a\n"
                                          "  wire output 2 \\y\n"
                                          "  cell $not \\n1\n"
                                          "    parameter \\A_SIGNED 0\n"
                                          "    parameter \\A_WIDTH 1\n"
                                          "    parameter \\Y_WIDTH 1\n"
                                          "    connect \\A \\a\n"
                                          "    connect \\Y \\y\n"
                                          "  end\n"
                                          "end\n"
                                          "attribute \\top 1\n"
                                          "module \\top\n"
                                          "  attribute \\keep 1\n"
                                          "  wire \\x\n"
                                          "  attribute \\init 2'1x\n"
                                          "  wire width 2 \\w\n"
                                          "  wire \\z\n"
                                          "  attribute \\keep 1\n"
                                          "  cell \\sub \\u1\n"
                                          "    connect \\a \\x\n"
                                          "    connect \\y \\z\n"
                                          "  end\n"
                                          "  cell $mem \\m1\n"
                                          "    parameter \\MEMID \"\\\\m\"\n"
                                          "  end\n"
                                          "  attribute \\fake_error 1\n"
                                          "  attribute \\src \"t.v:4.3\"\n"
                                          "  process $p\n"
                                          "    attribute \\src \"t.v:5.7\"\n"
                                          "    assign { \\w [1] \\w [0] } { \\x 1'0 }\n"
                                          "    sync always\n"
                                          "  end\n"
                                          "end\n";

static odin3_node_id node_named(const odin3_module *mod, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(odin3_module_design(mod)),
                                       odin3_bytes_cstr(name), &str));
    odin3_node_id node = odin3_module_find_node(mod, str);
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    return node;
}

static const odin3_value *attr_of(const odin3_module *mod, odin3_objref obj, const char *key) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(odin3_module_design(mod)),
                                       odin3_bytes_cstr(key), &str));
    return odin3_attr_get(mod, obj, str);
}

static odin3_wire_id wire_named(const odin3_module *mod, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(odin3_module_design(mod)),
                                       odin3_bytes_cstr(name), &str));
    return odin3_module_find_wire(mod, str);
}

/* obj has attribute key, an INT of value 1. */
static void expect_int_attr(const odin3_module *mod, odin3_objref obj, const char *key) {
    const odin3_value *val = attr_of(mod, obj, key);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, val->kind);
    TEST_ASSERT_EQUAL_INT64(1, val->i);
}

/* The instance u1 of sub in top, its pins on x and z. */
static void expect_grammar_instance(const pb_result *res, odin3_module *top) {
    const odin3_module *sub = pb_module(res, "sub");
    odin3_node_id u1 = node_named(top, "u1");
    TEST_ASSERT_EQUAL_UINT32(odin3_module_celltype(sub).v, odin3_node_type(top, u1).v);
    odin3_wire_id wire_x = wire_named(top, "x");
    odin3_wire_id wire_z = wire_named(top, "z");
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(top, wire_x, 0).v,
                             odin3_pin_net(top, odin3_node_port(top, u1, 0).first).v);
    odin3_pinslice y_pins = odin3_node_port(top, u1, 1);
    TEST_ASSERT_EQUAL_UINT32(1, y_pins.count);
    TEST_ASSERT_EQUAL_UINT32(odin3_wire_net(top, wire_z, 0).v, odin3_pin_net(top, y_pins.first).v);
}

/* keep on a wire and a cell, init on a wire, a STRING parameter. */
static void expect_grammar_attrs(const pb_result *res, odin3_module *top) {
    expect_int_attr(top, (odin3_objref){ODIN3_OBJ_WIRE, wire_named(top, "x").v}, "keep");
    expect_int_attr(top, (odin3_objref){ODIN3_OBJ_NODE, node_named(top, "u1").v}, "keep");
    expect_int_attr(top, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(top).v}, "top");
    const odin3_value *init =
        attr_of(top, (odin3_objref){ODIN3_OBJ_WIRE, wire_named(top, "w").v}, "init");
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, init->kind);
    TEST_ASSERT_EQUAL_UINT32(2, init->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_X, init->bits[0]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, init->bits[1]);
    const odin3_value *memid = odin3_node_param(top, node_named(top, "m1"), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, memid->kind);
    TEST_ASSERT_EQUAL_STRING("\\m", odin3_strtab_get(odin3_design_strtab(res->design), memid->str));
}

/* A source position: line and column. */
typedef struct src_at {
    uint32_t line, col;
} src_at;

/* Record id is a SOURCE record at t.v, at, with no raw location. */
static void expect_src(const pb_result *res, odin3_prov_id id, src_at at) {
    const odin3_prov_record *rec = odin3_prov_get(res->design, id);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->n_locs);
    TEST_ASSERT_EQUAL_STRING("t.v",
                             odin3_strtab_get(odin3_design_strtab(res->design), rec->locs[0].file));
    TEST_ASSERT_EQUAL_UINT32(at.line, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(at.col, rec->locs[0].col);
    TEST_ASSERT_EQUAL_UINT32(0, rec->locs[0].loc);
}

/* The process: its attribute on the $proc node, its \src records, its body. */
static void expect_grammar_process(const pb_result *res, odin3_module *top) {
    odin3_node_id proc = pb_proc(res, "top", 0);
    expect_int_attr(top, (odin3_objref){ODIN3_OBJ_NODE, proc.v}, "fake_error");
    TEST_ASSERT_NULL(attr_of(top, (odin3_objref){ODIN3_OBJ_NODE, proc.v}, "src"));
    expect_src(res, odin3_node_prov(top, proc), (src_at){4, 3});
    odin3_proc_body body;
    TEST_ASSERT_TRUE(odin3_proc_body_view(odin3_node_param(top, proc, ODIN3_PROC_P_BODY), &body));
    TEST_ASSERT_EQUAL_UINT32(1, body.n_actions);
    expect_src(res, body.actions[0].prov, (src_at){5, 7});
    odin3_strbuf text = print_of(top, proc);
    TEST_ASSERT_EQUAL_STRING("attribute \\fake_error 1\n"
                             "  assign \\w { \\x 1'0 }\n"
                             "  sync always\n"
                             "end\n",
                             text.data);
    odin3_strbuf_free(&text);
}

static void test_pb_grammar(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_grammar_text, &res));
    check_full_ok_in(res.design);
    odin3_module *top = pb_module(&res, "top");
    TEST_ASSERT_NOT_NULL(pb_module(&res, "sub"));
    TEST_ASSERT_EQUAL_PTR(top, res.module);
    TEST_ASSERT_NULL(pb_module(&res, "none"));
    expect_grammar_instance(&res, top);
    expect_grammar_attrs(&res, top);
    expect_grammar_process(&res, top);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &text));
    TEST_ASSERT_NOT_NULL(strstr(text.data, "module \\top\n"));
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* Switches, cases with several compares, flags, $tmp nets and every sync kind. */
static void test_pb_switches(void) {
    const char *text = "module \\m\n"
                       "  wire width 2 \\s\n"
                       "  wire \\a\n"
                       "  wire \\y\n"
                       "  wire \\r\n"
                       "  wire $tmp1\n"
                       "  process $p\n"
                       "    assign $tmp1 \\a\n"
                       "    attribute \\full_case 1\n"
                       "    attribute \\parallel_case 1\n"
                       "    switch \\s\n"
                       "      case 2'01 , 2'1-\n"
                       "        assign $tmp1 1'1\n"
                       "        switch \\a\n"
                       "          case 1'0\n"
                       "        end\n"
                       "      case\n"
                       "        assign $tmp1 \\s [0]\n"
                       "    end\n"
                       "    sync high \\r\n"
                       "      update \\y $tmp1\n"
                       "    sync negedge \\a\n"
                       "    sync low \\r\n"
                       "  end\n"
                       "end\n";
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
    check_full_ok_in(res.design);
    odin3_strbuf got = print_of(res.module, pb_proc(&res, "m", 0));
    char want[1024];
    uint32_t tmp = 0;
    for (uint32_t i = 1; i < odin3_module_net_end(res.module); i++) {
        if (odin3_proc_sig_is_net(res.module, (odin3_net_id){i})) {
            tmp = i;
        }
    }
    (void)snprintf(want, sizeof want,
                   "  assign $n%u \\a\n"
                   "  attribute \\full_case 1\n"
                   "  attribute \\parallel_case 1\n"
                   "  switch \\s\n"
                   "    case 2'01 , 2'1-\n"
                   "      assign $n%u 1'1\n"
                   "      switch \\a\n"
                   "        case 1'0\n"
                   "      end\n"
                   "    case\n"
                   "      assign $n%u \\s [0]\n"
                   "  end\n"
                   "  sync high \\r\n"
                   "    update \\y $n%u\n"
                   "  sync negedge \\a\n"
                   "  sync low \\r\n"
                   "end\n",
                   tmp, tmp, tmp, tmp);
    TEST_ASSERT_EQUAL_STRING(want, got.data);
    odin3_strbuf_free(&got);
    pb_free(&res);
}

/*
 * Yosys's own output reads back: its header comment, autoidx and the blank after a default case.
 * The text is `yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto prune` of P2's RTLIL (0.55):
 * proc_prune drops the overwritten `assign \t0 \q`.
 */
static void test_pb_reads_yosys_output(void) {
    const char *text = "# Generated by Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0 -fPIC -O3)\n"
                       "autoidx 1\n"
                       "module \\m\n"
                       "  wire \\clk\n"
                       "  wire \\d\n"
                       "  wire \\q\n"
                       "  wire \\s\n"
                       "  wire \\t0\n"
                       "  process $p\n"
                       "    assign \\t0 \\d\n"
                       "    attribute \\full_case 1\n"
                       "    switch \\s\n"
                       "      case 1'1\n"
                       "        assign \\t0 \\d\n"
                       "      case \n"
                       "    end\n"
                       "    sync posedge \\clk\n"
                       "      update \\q \\t0\n"
                       "  end\n"
                       "end\n";
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
    check_full_ok_in(res.design);
    odin3_strbuf got = print_of(res.module, pb_proc(&res, "m", 0));
    TEST_ASSERT_EQUAL_STRING("  assign \\t0 \\d\n"
                             "  attribute \\full_case 1\n"
                             "  switch \\s\n"
                             "    case 1'1\n"
                             "      assign \\t0 \\d\n"
                             "    case\n"
                             "  end\n"
                             "  sync posedge \\clk\n"
                             "    update \\q \\t0\n"
                             "end\n",
                             got.data);
    odin3_strbuf_free(&got);
    pb_free(&res);
}

/* Text outside the subset fails with its line, and leaves the result empty. */
static void test_pb_errors(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pb_build("module \\m\n  wire \\a\n  frob\nend\n", &res));
    TEST_ASSERT_EQUAL_STRING("pb_build: line 3: unknown line 'frob'", lines[0]);
    TEST_ASSERT_NULL(res.design);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          pb_build("module \\m\n  wire \\a\n  connect \\a \\b\nend\n", &res));
    TEST_ASSERT_EQUAL_STRING("pb_build: line 3: unknown wire '\\b'", lines[0]);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, pb_build("module \\m\n  wire \\a\n", &res));
    TEST_ASSERT_EQUAL_STRING("pb_build: line 2: the text ends inside a block", lines[0]);
}

/* An I pin and an O pin on one net (after a merge): both decode to it; encode makes it O only. */
static void test_decode_io_same_net(void) {
    const char *text = "module \\m\n  wire \\a\n  wire \\y\n  wire \\t\n  process $p\n"
                       "    assign \\t \\a\n    sync always\n      update \\y \\t\n  end\n"
                       "  connect \\a \\y\nend\n";
    pb_result res;
    odin3_pw pw;
    pb_decode(text, &res, &pw);
    odin3_module *mod = res.module;
    odin3_net_id net_a = odin3_wire_net(mod, wire_named(mod, "a"), 0);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, odin3_wire_net(mod, wire_named(mod, "y"), 0).v);
    expect_bit(&pw, case_action(pw_case(&pw, 0), 0)->rhs, net_a.v);
    expect_bit(&pw, ((const odin3_pw_action *)odin3_vec_cat(&pw_sync(&pw, 0)->actions, 0))->lhs,
               net_a.v);
    odin3_proc_mctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&ctx, &res.ctx, mod));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&ctx, &pw));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_node_port(mod, pw.node, ODIN3_PROC_PORT_I).count);
    odin3_pinslice outs = odin3_node_port(mod, pw.node, ODIN3_PROC_PORT_O);
    TEST_ASSERT_EQUAL_UINT32(2, outs.count);
    TEST_ASSERT_EQUAL_UINT32(net_a.v, odin3_pin_net(mod, outs.first).v);
    odin3_proc_body body;
    TEST_ASSERT_TRUE(
        odin3_proc_body_view(odin3_node_param(mod, pw.node, ODIN3_PROC_P_BODY), &body));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_verify(&body, (odin3_proc_widths){0, 2}, NULL));
    TEST_ASSERT_EQUAL_HEX32(OUT | 0, body.pbits[body.actions[0].rhs.first]);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(mod));
    check_full_ok_in(res.design);
    odin3_proc_mctx_free(&ctx);
    odin3_pw_free(&pw);
    pb_free(&res);
}

/* --- out of memory ------------------------------------------------------------------------- */

static void test_oom_decode(void) {
    p_nets nets;
    odin3_node_id node = p2_node(&nets);
    bool done = false;
    for (long k = 0; k < OOM_LIMIT && !done; k++) {
        odin3_pw pw;
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_pw_decode(&mctx, node, &pw);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_ERR_NO_MEMORY) {
            TEST_ASSERT_EQUAL_UINT32(0, pw.cases.len);
            TEST_ASSERT_EQUAL_UINT32(0, pw.bits.len);
            check_full_ok_in(design);
            continue;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
        odin3_pw_free(&pw);
        done = true;
    }
    TEST_ASSERT_TRUE(done);
}

/* One step of the encode sweep: true once encode succeeded (no allocation failed). */
static bool oom_encode_step(long fail_after, odin3_node_id node, const saved_body *saved) {
    odin3_pw pw;
    decode_ok(&mctx, node, &pw);
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_pw_encode(&mctx, &pw);
    odin3_util_set_alloc_fail_after(-1);
    odin3_node_id now = pw.node;
    odin3_pw_free(&pw);
    if (st == ODIN3_OK) {
        TEST_ASSERT_TRUE(body_is(now, saved));
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_UINT32(node.v, now.v);
    TEST_ASSERT_TRUE(odin3_node_live(module, node));
    TEST_ASSERT_TRUE(body_is(node, saved));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(0, mctx.touched.len);
    check_full_ok_in(design);
    return false;
}

static void test_oom_encode(void) {
    p_nets nets;
    odin3_node_id node = p2_node(&nets);
    saved_body saved;
    save_body(node, &saved);
    bool done = false;
    for (long k = 0; k < OOM_LIMIT && !done; k++) {
        done = oom_encode_step(k, node, &saved);
    }
    TEST_ASSERT_TRUE(done);
    check_full_ok_in(design);
}

/* One step of the pb_build sweep: true once it succeeded. */
static bool oom_pb_step(long fail_after) {
    pb_result res;
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = pb_build(k_grammar_text, &res);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        check_full_ok_in(res.design);
    } else {
        TEST_ASSERT_NULL(res.design);
    }
    pb_free(&res);
    return st == ODIN3_OK;
}

static void test_oom_pb_build(void) {
    bool done = false;
    for (long k = 0; k < OOM_LIMIT && !done; k++) {
        done = oom_pb_step(k);
    }
    TEST_ASSERT_TRUE(done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_p1);
    RUN_TEST(test_decode_constants_compare);
    RUN_TEST(test_decode_constants_merged);
    RUN_TEST(test_decode_constants_xz);
    RUN_TEST(test_decode_attrs);
    RUN_TEST(test_decode_refusals);
    RUN_TEST(test_roundtrip_identity_p1);
    RUN_TEST(test_roundtrip_identity_p2);
    RUN_TEST(test_encode_keeps_attrs);
    RUN_TEST(test_encode_after_edit);
    RUN_TEST(test_sig_replace_remove2_extract);
    RUN_TEST(test_sig_constants_and_widths);
    RUN_TEST(test_sig_predicates);
    RUN_TEST(test_sig_equal);
    RUN_TEST(test_sig_sort_unify);
    RUN_TEST(test_sig_new_from_own_bits);
    RUN_TEST(test_bit_key);
    RUN_TEST(test_bit_key_merged);
    RUN_TEST(test_bit_key_unnamed);
    RUN_TEST(test_const_net);
    RUN_TEST(test_pb_matches_pack);
    RUN_TEST(test_pb_grammar);
    RUN_TEST(test_pb_switches);
    RUN_TEST(test_pb_reads_yosys_output);
    RUN_TEST(test_pb_errors);
    RUN_TEST(test_decode_io_same_net);
    RUN_TEST(test_oom_decode);
    RUN_TEST(test_oom_encode);
    RUN_TEST(test_oom_pb_build);
    return UNITY_END();
}
