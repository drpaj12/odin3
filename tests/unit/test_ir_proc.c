/*
 * test_ir_proc.c — the $proc cell (2D IR change B: EL-33, spec §8.1; docs/IR.md IR-19): the type,
 * its body (pack, view, the allocation-free verify), the process counter, what the IR operations
 * do with a $proc node (driver and sink queries, rule 4, merge, compact, replace), the RTLIL
 * spelling of signals and process blocks, the registration rule and out of memory.
 */
#include "backends/json/writer.h"
#include "backends/verilog/writer.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LOG_LINES = 16,
    LOG_TEXT = 256,
    OOM_LIMIT = 1000,
    MAX_PINS = 16,
    PBITS_MAX = 16,
    ARR_MAX = 8,
    PROC_INDEX = 62,       /* appended after the nine word cells (plan point (f)) */
    BUILTIN_COUNT = 74,    /* the eleven 2E flip-flops and latches follow $proc (IR change D) */
    TEST_ARENA_CHUNK = 64, /* smaller than a packed body: pack must take a fresh chunk */
    MISALIGN = 4,
    SAVE_WORDS = 128, /* a saved body: up to 1 KiB */
    FILL_WIDTH = 480, /* a 1920-byte net vector: most of a 2048-byte module arena chunk */
    /*
     * A $proc made when the module's stores sit at page boundaries allocates four times: an
     * arena chunk for its parameters, a node page, a pin page (all before the commit) and a
     * pin-pool chunk for its first connection (after it). This pins util's growth policy too: if
     * that changes, re-derive the four.
     */
    CREATE_ALLOCS = 4,
};

#define IN ODIN3_PBIT_IN
#define OUT ODIN3_PBIT_OUT
#define ANY ODIN3_PBIT_ANY

static odin3_design *design;
static odin3_module *module;
static odin3_arena *arena;
static odin3_prov_id prov;
static odin3_celltype_id proc_type;

static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

/* Keeps every error line (the first LOG_LINES of them). */
static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level != ODIN3_LOG_ERROR) {
        return;
    }
    if (n_lines < LOG_LINES) {
        (void)snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
    }
    n_lines++;
}

static void lines_clear(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
}

static bool line_has(const char *needle) {
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (strstr(lines[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_module *new_module(const char *name) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, intern(name), prov, &id));
    return odin3_module_get(design, id);
}

static odin3_celltype_id type_named(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str)) {
        (void)odin3_celltype_find(design, str, &id);
    }
    return id;
}

/* A fresh design with a SOURCE record (no rule 7 warnings) and module m. */
static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_pass_ctx run = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("test"), &run));
    odin3_srcloc loc = {intern("t.v"), 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run, &origin, &prov));
    module = new_module("m");
    proc_type = type_named("$proc");
    TEST_ASSERT_NOT_EQUAL(0, proc_type.v);
}

void setUp(void) {
    lines_clear();
    odin3_log_set_sink(line_sink, NULL);
    design = NULL;
    arena = odin3_arena_create(TEST_ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    fresh_design();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    odin3_arena_destroy(arena);
    design = NULL;
    module = NULL;
    arena = NULL;
}

/* --- fixtures ------------------------------------------------------------------------------ */

/* A 1-bit wire named name in module mod; returns its net. */
static odin3_net_id wire_in(odin3_module *mod, const char *name) {
    odin3_wire_spec spec = {intern(name), 0, 0, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, NULL, &wire));
    return odin3_wire_net(mod, wire, 0);
}

static odin3_net_id wire1(const char *name) {
    return wire_in(module, name);
}

/* A net driven by a constant node of type tname (one per value, IR-4). */
static odin3_net_id const_net(const char *tname) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, &net));
    odin3_node_spec spec = {type_named(tname), 0, prov, NULL, 0};
    odin3_netvec ports[1] = {{&net, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, NULL));
    return net;
}

/* A body under construction: the arrays and the view of them. */
typedef struct body_buf {
    odin3_pbit pbits[PBITS_MAX];
    odin3_pspan compares[ARR_MAX];
    odin3_proc_action actions[ARR_MAX];
    odin3_proc_case cases[ARR_MAX];
    odin3_proc_switch switches[ARR_MAX];
    odin3_proc_sync syncs[ARR_MAX];
    odin3_proc_body body;
} body_buf;

/* Points the view at the arrays, with the given counts. */
typedef struct body_counts {
    uint32_t pbits, compares, actions, cases, switches, syncs;
} body_counts;

static void link_body(body_buf *buf, body_counts counts) {
    buf->body = (odin3_proc_body){buf->pbits,    counts.pbits,    buf->compares, counts.compares,
                                  buf->actions,  counts.actions,  buf->cases,    counts.cases,
                                  buf->switches, counts.switches, buf->syncs,    counts.syncs};
}

/*
 * P1: always @(posedge clk) q <= d; with I = [clk, d], O = [q, t0]: a0 t0 <- q, a1 t0 <- d (the
 * root case), a2 q <- t0 (sync posedge clk).
 */
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
 * a2 t0 <- d, and a default case; the sync action is a3 q <- t0.
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

/* The pins of a $proc to create: the nets of I and of O. */
typedef struct proc_pins {
    odin3_net_id in[MAX_PINS];
    uint32_t n_in;
    odin3_net_id out[MAX_PINS];
    uint32_t n_out;
} proc_pins;

/* The nets of P1 (and P2: s and the $_CONST1_ net). */
typedef struct p_nets {
    odin3_net_id clk, d, q, t0, s, c1;
} p_nets;

static p_nets p1_nets(void) {
    p_nets nets = {wire1("clk"), wire1("d"), wire1("q"), wire1("t0"), {0}, {0}};
    return nets;
}

static proc_pins p1_pins(const p_nets *nets) {
    proc_pins pins = {{nets->clk, nets->d}, 2, {nets->q, nets->t0}, 2};
    return pins;
}

static p_nets p2_nets(void) {
    p_nets nets = p1_nets();
    nets.s = wire1("s");
    nets.c1 = const_net("$_CONST1_");
    return nets;
}

static proc_pins p2_pins(const p_nets *nets) {
    proc_pins pins = {{nets->clk, nets->d, nets->s, nets->c1}, 4, {nets->q, nets->t0}, 2};
    return pins;
}

/* The parameters of a $proc with pins and the packed body. */
static void proc_params(const proc_pins *pins, const odin3_proc_body *body, odin3_value *params) {
    params[ODIN3_PROC_P_I_WIDTH] = odin3_value_int(pins->n_in);
    params[ODIN3_PROC_P_O_WIDTH] = odin3_value_int(pins->n_out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, body, &params[ODIN3_PROC_P_BODY]));
}

static odin3_status create_with(odin3_module *mod, const proc_pins *pins, const odin3_value *params,
                                odin3_node_id *out) {
    odin3_node_spec spec = {proc_type, 0, prov, params, 3};
    odin3_netvec ports[2] = {{pins->in, pins->n_in}, {pins->out, pins->n_out}};
    return odin3_node_create_connected(mod, &spec, ports, out);
}

static odin3_status create_proc(const proc_pins *pins, const odin3_proc_body *body,
                                odin3_node_id *out) {
    odin3_value params[3];
    proc_params(pins, body, params);
    return create_with(module, pins, params, out);
}

static odin3_node_id p1_node(const p_nets *nets) {
    body_buf buf;
    p1_body(&buf);
    proc_pins pins = p1_pins(nets);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(&pins, &buf.body, &node));
    return node;
}

/* Pin `bit` of port ODIN3_PROC_PORT_I or ODIN3_PROC_PORT_O of a $proc node. */
typedef struct port_bit {
    uint32_t port;
    uint32_t bit;
} port_bit;

static odin3_pin_id port_pin(odin3_node_id node, port_bit at) {
    odin3_pinslice slice = odin3_node_port(module, node, at.port);
    TEST_ASSERT_TRUE(at.bit < slice.count);
    return (odin3_pin_id){slice.first.v + at.bit};
}

static odin3_pin_id in_pin(odin3_node_id node, uint32_t bit) {
    return port_pin(node, (port_bit){ODIN3_PROC_PORT_I, bit});
}

static odin3_pin_id out_pin(odin3_node_id node, uint32_t bit) {
    return port_pin(node, (port_bit){ODIN3_PROC_PORT_O, bit});
}

static bool sinks_include(odin3_net_id net, odin3_pin_id pin) {
    odin3_pinlist sinks = odin3_net_sinks(module, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        if (sinks.pins[i].v == pin.v) {
            return true;
        }
    }
    return false;
}

static void check_full_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

static const odin3_value *body_of(odin3_node_id node) {
    return odin3_node_param(module, node, ODIN3_PROC_P_BODY);
}

/* A copy of a BODY's bytes (P1/P2 blobs are a few hundred bytes). */
typedef struct saved_body {
    uint64_t store[SAVE_WORDS]; /* 8-byte aligned */
    uint32_t len;
} saved_body;

static void save_body(const odin3_value *val, saved_body *out) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof out->store, val->len);
    memcpy(out->store, val->bits, val->len);
    out->len = val->len;
}

static odin3_value saved_value(const saved_body *saved) {
    odin3_value val = {ODIN3_VAL_PROC, 0, (const uint8_t *)saved->store, saved->len, 0, 0};
    return val;
}

/* The text odin3_proc_print gives for node contains needle. */
static void expect_print_has(odin3_node_id node, const char *needle) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_print(module, node, 0, &text));
    TEST_ASSERT_NOT_NULL(strstr(text.data, needle));
    odin3_strbuf_free(&text);
}

/* P1 on fresh wires: its pins and its packed parameters. */
static void p1_params(proc_pins *pins, odin3_value *params) {
    p_nets nets = p1_nets();
    *pins = p1_pins(&nets);
    body_buf buf;
    p1_body(&buf);
    proc_params(pins, &buf.body, params);
}

/* --- the type ------------------------------------------------------------------------------ */

static void test_type_is_registered(void) {
    TEST_ASSERT_EQUAL_UINT32(BUILTIN_COUNT, odin3_builtin_celltype_count);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_proc, odin3_builtin_celltypes[PROC_INDEX]);
    const odin3_celltype_def *def = odin3_celltype_get(design, proc_type);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_proc, def);
    TEST_ASSERT_EQUAL_STRING("$proc", def->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_WORD, def->gran);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_CT_PROCESS, def->flags);
    TEST_ASSERT_EQUAL_UINT32(32, ODIN3_CT_PROCESS);
    TEST_ASSERT_EQUAL_INT(4, ODIN3_VAL_PROC);
    TEST_ASSERT_NOT_NULL(def->verify);
    TEST_ASSERT_TRUE(def->const_value == NULL && def->simulate == NULL &&
                     def->sim_scratch_bytes == NULL);
}

static void test_type_ports_and_params(void) {
    const odin3_celltype_def *def = &odin3_cell_proc;
    TEST_ASSERT_EQUAL_UINT32(2, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("I", def->ports[ODIN3_PROC_PORT_I].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN, def->ports[ODIN3_PROC_PORT_I].dir);
    TEST_ASSERT_EQUAL_STRING("I_WIDTH", def->ports[ODIN3_PROC_PORT_I].width_param);
    TEST_ASSERT_FALSE(def->ports[ODIN3_PROC_PORT_I].scalar);
    TEST_ASSERT_EQUAL_STRING("O", def->ports[ODIN3_PROC_PORT_O].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[ODIN3_PROC_PORT_O].dir);
    TEST_ASSERT_EQUAL_STRING("O_WIDTH", def->ports[ODIN3_PROC_PORT_O].width_param);
    TEST_ASSERT_EQUAL_UINT32(3, def->n_params);
    TEST_ASSERT_EQUAL_STRING("I_WIDTH", def->params[ODIN3_PROC_P_I_WIDTH].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, def->params[ODIN3_PROC_P_I_WIDTH].kind);
    TEST_ASSERT_EQUAL_INT64(0, def->params[ODIN3_PROC_P_I_WIDTH].dflt.i);
    TEST_ASSERT_EQUAL_STRING("O_WIDTH", def->params[ODIN3_PROC_P_O_WIDTH].name);
    TEST_ASSERT_EQUAL_INT64(1, def->params[ODIN3_PROC_P_O_WIDTH].dflt.i);
    TEST_ASSERT_EQUAL_STRING("BODY", def->params[ODIN3_PROC_P_BODY].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_PROC, def->params[ODIN3_PROC_P_BODY].kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_PROC, def->params[ODIN3_PROC_P_BODY].dflt.kind);
    TEST_ASSERT_EQUAL_UINT32(0, def->params[ODIN3_PROC_P_BODY].dflt.len);
}

/* The empty default BODY is rejected, so a default $proc node cannot exist. */
static void test_default_node_cannot_exist(void) {
    uint32_t nodes = odin3_module_node_end(module);
    odin3_node_spec spec = {proc_type, 0, prov, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_node_create(module, &spec, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, n_lines);
    TEST_ASSERT_EQUAL_STRING("$proc: BODY: not a process body", lines[0]);
    TEST_ASSERT_EQUAL_STRING("node_create: '$proc' rejects these parameters", lines[1]);
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(module));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, proc_type));
}

/* I_WIDTH below 0 or O_WIDTH below 1 is refused before the body is looked at. */
static void test_width_params_checked(void) {
    p_nets nets = p1_nets();
    proc_pins pins = p1_pins(&nets);
    body_buf buf;
    p1_body(&buf);
    odin3_value params[3];
    proc_params(&pins, &buf.body, params);
    const odin3_celltype_def *def = odin3_celltype_get(design, proc_type);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, def->verify(params));
    params[ODIN3_PROC_P_O_WIDTH] = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, def->verify(params));
    params[ODIN3_PROC_P_O_WIDTH] = odin3_value_int(2);
    params[ODIN3_PROC_P_I_WIDTH] = odin3_value_int(-1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, def->verify(params));
    TEST_ASSERT_TRUE(line_has("O_WIDTH"));
    TEST_ASSERT_TRUE(line_has("I_WIDTH"));
}

/* --- create and query ---------------------------------------------------------------------- */

static void test_create_and_query(void) {
    p_nets nets = p1_nets();
    odin3_node_id node = p1_node(&nets);
    TEST_ASSERT_TRUE(odin3_node_live(module, node));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_node_pins(module, node).count);
    TEST_ASSERT_EQUAL_UINT32(out_pin(node, 0).v, odin3_net_driver(module, nets.q).v);
    TEST_ASSERT_EQUAL_UINT32(out_pin(node, 1).v, odin3_net_driver(module, nets.t0).v);
    TEST_ASSERT_TRUE(sinks_include(nets.d, in_pin(node, 1)) &&
                     sinks_include(nets.clk, in_pin(node, 0)));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_net_driver_count(module, nets.d));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_TRUE(odin3_design_has_processes(design));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(module).v,
                             odin3_design_first_proc_module(design).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, proc_type));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_PROC, body_of(node)->kind);
    check_full_ok();
}

/* The first module in creation order that has a process. */
static void test_first_proc_module(void) {
    TEST_ASSERT_FALSE(odin3_design_has_processes(design));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_first_proc_module(design).v);
    odin3_module *first = module;
    odin3_module *second = new_module("m2");
    odin3_module *third = new_module("m3");
    module = third;
    p_nets nets3 = p1_nets();
    (void)p1_node(&nets3);
    module = second;
    p_nets nets2 = p1_nets();
    (void)p1_node(&nets2);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(first));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(second));
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(second).v,
                             odin3_design_first_proc_module(design).v);
    TEST_ASSERT_TRUE(odin3_design_has_processes(design));
}

/* --- malformed bodies ---------------------------------------------------------------------- */

typedef void (*body_fault)(body_buf *buf, proc_pins *pins, const p_nets *nets);

typedef struct malformed {
    const char *what;
    bool p2; /* start from P2, else P1 */
    body_fault fault;
    const char *why;
} malformed;

static void fault_in_range(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[3] = IN | 2;
}
static void fault_lhs_in(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->actions[1].lhs = (odin3_pspan){3, 1};
}
static void fault_lhs_twice(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[6] = OUT | 0;
    buf->pbits[7] = OUT | 0;
    buf->body.n_pbits = 8;
    buf->actions[0] = (odin3_proc_action){{6, 2}, {0, 2}, {0}};
}
static void fault_widths(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->actions[0].rhs = (odin3_pspan){0, 2};
}
static void fault_any_rhs(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[3] = ANY;
}
static void fault_unassigned(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)buf, (void)nets;
    pins->out[pins->n_out++] = wire1("x");
}
static void fault_root_compares(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->compares[0] = (odin3_pspan){3, 1};
    buf->body.n_compares = 1;
    buf->cases[0].n_compares = 1;
}
static void fault_compare_width(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->compares[0] = (odin3_pspan){6, 2};
}
static void fault_action_reused(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->cases[1].first_action = 0;
}
static void fault_action_skipped(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->cases[0] = (odin3_proc_case){0, 0, 1, 1, 0, 0, {0}};
}
static void fault_cases_start(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->switches[0].first_case = 2;
}
/* Case 2 (of switch 0) owns switch 1, whose only case is case 2 itself. */
static void fault_cycle(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->switches[0].n_cases = 1;
    buf->switches[1] = (odin3_proc_switch){{6, 1}, 2, 1, 0, {0}};
    buf->cases[2].first_switch = 1;
    buf->cases[2].n_switches = 1;
    buf->body.n_switches = 2;
}
static void fault_always_signal(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->syncs[0] = (odin3_proc_sync){ODIN3_SYNC_ALWAYS, IN | 1, 2, 1};
}
static void fault_sync_any(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->syncs[0].signal = ANY;
}

/* Task 3 review M3: a zero-width action says nothing (t0 is still assigned by action 1). */
static void fault_empty_action(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->actions[0].lhs.len = 0;
    buf->actions[0].rhs.len = 0;
}

static const malformed k_malformed[] = {
    {"in range", false, fault_in_range, "IN position 2 is out of range (I_WIDTH 2)"},
    {"lhs IN", false, fault_lhs_in, "action 1: lhs holds a non-OUT position"},
    {"lhs twice", false, fault_lhs_twice, "action 0: OUT position 0 twice in one lhs"},
    {"widths", false, fault_widths, "action 0: lhs and rhs widths differ (1, 2)"},
    {"ANY rhs", false, fault_any_rhs, "ANY outside a compare"},
    {"unassigned", false, fault_unassigned, "OUT position 2 is never assigned"},
    {"root compares", false, fault_root_compares, "root case has compares"},
    {"compare width", true, fault_compare_width,
     "switch 0: case 1 compare 0 has width 2; the signal has 1"},
    {"action reused", true, fault_action_reused, "case 1: actions start at 0, expected 2"},
    {"action skipped", false, fault_action_skipped, "case 0: actions start at 1, expected 0"},
    {"cases start", true, fault_cases_start, "switch 0: cases start at 2, expected 1"},
    {"cycle", true, fault_cycle, "switch 1: case 2 is not after its parent case 2"},
    {"ALWAYS signal", false, fault_always_signal, "sync 0: an ALWAYS sync has a signal"},
    {"sync ANY", false, fault_sync_any, "sync 0: the signal must be one IN or OUT position"},
    {"empty action", false, fault_empty_action, "action 0: empty"},
};

/* The node and pin store ends of the module (nothing created between two readings). */
typedef struct store_ends {
    uint32_t nodes;
    uint32_t pins;
} store_ends;

static store_ends ends_now(void) {
    store_ends ends = {odin3_module_node_end(module), odin3_module_pin_end(module)};
    return ends;
}

/* Asserts that nothing was created and the sink saw exactly the two refusal lines. */
static void expect_refused(const char *why, store_ends before, const char *what) {
    uint32_t nodes = before.nodes;
    uint32_t pins = before.pins;
    char want[LOG_TEXT];
    (void)snprintf(want, sizeof want, "$proc: BODY: %s", why);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, n_lines, what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, lines[0], what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("node_create: '$proc' rejects these parameters", lines[1],
                                     what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(nodes, odin3_module_node_end(module), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(pins, odin3_module_pin_end(module), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, odin3_module_proc_count(module), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, odin3_celltype_instances(design, proc_type), what);
}

static void run_malformed(const malformed *mal) {
    fresh_design();
    p_nets nets = p2_nets();
    body_buf buf;
    proc_pins pins;
    if (mal->p2) {
        p2_body(&buf);
        pins = p2_pins(&nets);
    } else {
        p1_body(&buf);
        pins = p1_pins(&nets);
    }
    mal->fault(&buf, &pins, &nets);
    store_ends before = ends_now();
    lines_clear();
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, create_proc(&pins, &buf.body, &node),
                                  mal->what);
    expect_refused(mal->why, before, mal->what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, odin3_net_pins(module, nets.d).count, mal->what);
    /* The same verdict straight from verify. */
    odin3_proc_why why = {""};
    odin3_proc_widths widths = {pins.n_in, pins.n_out};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_verify(&buf.body, widths, &why));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(mal->why, why.text, mal->what);
}

static void test_malformed_bodies(void) {
    for (size_t i = 0; i < sizeof k_malformed / sizeof k_malformed[0]; i++) {
        run_malformed(&k_malformed[i]);
    }
}

/* The fixtures themselves are well formed (so each fault above is the only one). */
static void test_fixtures_verify(void) {
    body_buf buf;
    odin3_proc_why why = {""};
    p1_body(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_proc_body_verify(&buf.body, (odin3_proc_widths){2, 2}, &why));
    p2_body(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_proc_body_verify(&buf.body, (odin3_proc_widths){4, 2}, &why));
    TEST_ASSERT_EQUAL_STRING("", why.text);
}

/* A blob 4 bytes short goes through node_create: not a process body, nothing created. */
static void test_short_blob(void) {
    p_nets nets = p1_nets();
    proc_pins pins = p1_pins(&nets);
    body_buf buf;
    p1_body(&buf);
    odin3_value params[3];
    proc_params(&pins, &buf.body, params);
    params[ODIN3_PROC_P_BODY].len -= MISALIGN;
    store_ends before = ends_now();
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, create_with(module, &pins, params, NULL));
    expect_refused("not a process body", before, "short");
}

/* The same bytes 4 bytes into an 8-aligned buffer are not a body (no misaligned record reads). */
static void test_misaligned_body(void) {
    proc_pins pins;
    odin3_value params[3];
    p1_params(&pins, params);
    static uint64_t store[SAVE_WORDS + 1];
    uint8_t *mem = (uint8_t *)store + MISALIGN;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof store - MISALIGN, params[ODIN3_PROC_P_BODY].len);
    memcpy(mem, params[ODIN3_PROC_P_BODY].bits, params[ODIN3_PROC_P_BODY].len);
    params[ODIN3_PROC_P_BODY].bits = mem;
    odin3_proc_body view;
    TEST_ASSERT_FALSE(odin3_proc_body_view(&params[ODIN3_PROC_P_BODY], &view));
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cell_proc.verify(params));
    TEST_ASSERT_EQUAL_UINT32(1, n_lines);
    TEST_ASSERT_EQUAL_STRING("$proc: BODY: not a process body", lines[0]);
}

/* A BITS value: the hook says it is no body; node_create refuses the kind before the hook. */
static void test_bits_body(void) {
    proc_pins pins;
    odin3_value params[3];
    p1_params(&pins, params);
    const uint8_t bit[1] = {ODIN3_BIT_1};
    params[ODIN3_PROC_P_BODY] = (odin3_value){ODIN3_VAL_BITS, 0, bit, 1, 0, 0};
    odin3_proc_body view;
    TEST_ASSERT_FALSE(odin3_proc_body_view(&params[ODIN3_PROC_P_BODY], &view));
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_cell_proc.verify(params));
    TEST_ASSERT_EQUAL_STRING("$proc: BODY: not a process body", lines[0]);
    lines_clear();
    store_ends before = ends_now();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, create_with(module, &pins, params, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, n_lines);
    TEST_ASSERT_EQUAL_STRING("node_create: '$proc' parameter BODY has the wrong kind or a "
                             "malformed value",
                             lines[0]);
    TEST_ASSERT_EQUAL_UINT32(before.nodes, odin3_module_node_end(module));
}

/* The view reads a packed body in place; trailing bytes are not a body. */
static void test_view_reads_packed(void) {
    proc_pins pins;
    odin3_value params[3];
    p1_params(&pins, params);
    const odin3_value packed = params[ODIN3_PROC_P_BODY];
    odin3_proc_body view;
    TEST_ASSERT_TRUE(odin3_proc_body_view(&packed, &view));
    TEST_ASSERT_EQUAL_UINT32(6, view.n_pbits);
    TEST_ASSERT_EQUAL_UINT32(0, view.n_compares);
    TEST_ASSERT_EQUAL_UINT32(3, view.n_actions);
    TEST_ASSERT_EQUAL_UINT32(1, view.n_cases);
    TEST_ASSERT_EQUAL_UINT32(0, view.n_switches);
    TEST_ASSERT_EQUAL_UINT32(1, view.n_syncs);
    TEST_ASSERT_EQUAL_UINT32(IN | 1, view.pbits[3]);
    TEST_ASSERT_EQUAL_UINT32(5, view.actions[2].rhs.first);
    TEST_ASSERT_EQUAL_UINT32(2, view.cases[0].n_actions);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SYNC_POSEDGE, view.syncs[0].type);
    TEST_ASSERT_EQUAL_UINT32(2, view.syncs[0].first_action);
    odin3_value longer = packed;
    longer.len += 8;
    TEST_ASSERT_FALSE(odin3_proc_body_view(&longer, &view));
}

/* Each further rule verify knows, with its text (straight from verify). */
typedef struct more_case {
    const char *why;
    bool p2;
    body_fault fault;
} more_case;

static void more_out_range(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[0] = OUT | 2;
}
static void more_bad_tag(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[0] = (3U << 30) | 1U;
}
static void more_any_index(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[3] = ANY | 1U;
}
static void more_span_out(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->actions[2].rhs = (odin3_pspan){5, 2};
}
static void more_no_root(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->body.n_cases = 0;
}
static void more_missing_array(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->body.actions = NULL;
}
static void more_switch_any(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->pbits[6] = ANY;
}
static void more_switch_flags(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->switches[0].flags = 4;
}
static void more_sync_type(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->syncs[0].type = 0;
}
static void more_sync_range(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->syncs[0].signal = IN | 5;
}
static void more_unused_action(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->actions[3] = buf->actions[2];
    buf->body.n_actions = 4;
}
static void more_unused_compare(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->compares[1] = (odin3_pspan){7, 1};
    buf->body.n_compares = 2;
}
static void more_unused_case(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->switches[0].n_cases = 1;
}
static void more_switch_overrun(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->cases[0].n_switches = 2;
}
static void more_sync_overrun(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->syncs[0].n_actions = 2;
}
static void more_compares_start(body_buf *buf, proc_pins *pins, const p_nets *nets) {
    (void)pins, (void)nets;
    buf->cases[2].first_compare = 0;
}

static const more_case k_more[] = {
    {"OUT position 2 is out of range (O_WIDTH 2)", false, more_out_range},
    {"pbit 0 is neither IN, OUT nor ANY (0xc0000001)", false, more_bad_tag},
    {"pbit 3 is neither IN, OUT nor ANY (0x80000001)", false, more_any_index},
    {"action 2: rhs [5, 7) is outside the 6 positions", false, more_span_out},
    {"no root case", false, more_no_root},
    {"an array with entries is missing", false, more_missing_array},
    {"ANY outside a compare", true, more_switch_any},
    {"switch 0: unknown flags 0x4", true, more_switch_flags},
    {"sync 0: unknown type 0", false, more_sync_type},
    {"IN position 5 is out of range (I_WIDTH 2)", false, more_sync_range},
    {"only 3 of the 4 actions are used", false, more_unused_action},
    {"only 1 of the 2 compares are used", true, more_unused_compare},
    {"only 2 of the 3 cases are used", true, more_unused_case},
    {"case 0: switches [0, 2) run past the 1 switches", true, more_switch_overrun},
    {"sync 0: actions [2, 4) run past the 3 actions", false, more_sync_overrun},
    {"case 2: compares start at 0, expected 1", true, more_compares_start},
};

static void test_more_rules(void) {
    p_nets nets = p2_nets();
    for (size_t i = 0; i < sizeof k_more / sizeof k_more[0]; i++) {
        body_buf buf;
        proc_pins pins;
        if (k_more[i].p2) {
            p2_body(&buf);
            pins = p2_pins(&nets);
        } else {
            p1_body(&buf);
            pins = p1_pins(&nets);
        }
        k_more[i].fault(&buf, &pins, &nets);
        odin3_proc_why why = {""};
        odin3_proc_widths widths = {pins.n_in, pins.n_out};
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_ERR_INVALID_ARG, odin3_proc_body_verify(&buf.body, widths, &why), k_more[i].why);
        TEST_ASSERT_EQUAL_STRING(k_more[i].why, why.text);
    }
}

/* --- verify never allocates (review I3) ---------------------------------------------------- */

static void test_verify_allocates_nothing(void) {
    p_nets nets = p2_nets();
    proc_pins pins = p2_pins(&nets);
    body_buf buf;
    p2_body(&buf);
    odin3_value params[3];
    proc_params(&pins, &buf.body, params);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_with(module, &pins, params, &node));
    odin3_proc_why why = {""};
    odin3_proc_widths widths = {4, 2};
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_verify(&buf.body, widths, &why));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cell_proc.verify(params));
    p1_body(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_proc_body_verify(&buf.body, (odin3_proc_widths){2, 2}, &why));
    buf.pbits[3] = ANY;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_proc_body_verify(&buf.body, (odin3_proc_widths){2, 2}, &why));
    /* Check rule 5 runs verify: no false check error under a failing allocator (FAST: no marks). */
    odin3_check_opts fast = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, fast));
    /* The injected failure is still armed: nothing above allocated. */
    void *probe = odin3_util_malloc(1);
    TEST_ASSERT_NULL(probe);
    odin3_util_set_alloc_fail_after(-1);
}

/*
 * A process wider than verify's bitmap window (8192 positions): one action assigns every OUT
 * position from IN 0. The "twice" and "assigned" checks still find what lies past the first window.
 */
static void test_wide_body(void) {
    enum { WIDE = 8192 + 100, LATE = 8250, TWIN = 8200 };
    odin3_pbit *pbits = malloc(sizeof *pbits * 2 * WIDE);
    TEST_ASSERT_NOT_NULL(pbits);
    for (uint32_t k = 0; k < WIDE; k++) {
        pbits[k] = OUT | k;
        pbits[WIDE + k] = IN | 0;
    }
    odin3_proc_action act = {{0, WIDE}, {WIDE, WIDE}, {0}};
    odin3_proc_case root = {0, 0, 0, 1, 0, 0, {0}};
    odin3_proc_body body = {pbits, 2 * WIDE, NULL, 0, &act, 1, &root, 1, NULL, 0, NULL, 0};
    odin3_proc_widths widths = {1, WIDE};
    odin3_proc_why why = {""};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_verify(&body, widths, &why));
    pbits[LATE] = OUT | TWIN; /* TWIN twice, LATE never: both past the first window */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_verify(&body, widths, &why));
    TEST_ASSERT_EQUAL_STRING("action 0: OUT position 8200 twice in one lhs", why.text);
    pbits[LATE] = OUT | LATE;
    pbits[WIDE - 1] = OUT | LATE; /* the last position repeats LATE: the last one is never set */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_verify(&body, widths, &why));
    TEST_ASSERT_EQUAL_STRING("action 0: OUT position 8250 twice in one lhs", why.text);
    act.lhs.len = WIDE - 1; /* drop the repeat: position WIDE - 1 is assigned by nobody */
    act.rhs.len = WIDE - 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_verify(&body, widths, &why));
    TEST_ASSERT_EQUAL_STRING("OUT position 8291 is never assigned", why.text);
    free(pbits);
}

/* Point (h): always @(posedge a) a <= ~a; the sync is on OUT position 0. */
static void test_sync_on_out_position(void) {
    odin3_net_id net_a = wire1("a");
    odin3_net_id net_na = wire1("na");
    odin3_net_id net_t0 = wire1("t0");
    odin3_node_spec not_spec = {type_named("$not"), 0, prov, NULL, 0};
    odin3_netvec not_ports[2] = {{&net_a, 1}, {&net_na, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_create_connected(module, &not_spec, not_ports, NULL));
    body_buf buf;
    p1_body(&buf);
    buf.pbits[3] = IN | 0;
    buf.syncs[0].signal = OUT | 0;
    proc_pins pins = {{net_na}, 1, {net_a, net_t0}, 2};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(&pins, &buf.body, &node));
    TEST_ASSERT_EQUAL_UINT32(out_pin(node, 0).v, odin3_net_driver(module, net_a).v);
    check_full_ok();
    for (uint32_t type = ODIN3_SYNC_NEGEDGE; type <= ODIN3_SYNC_LEVEL1; type++) {
        buf.syncs[0].type = (uint8_t)type;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_proc_body_verify(&buf.body, (odin3_proc_widths){1, 2}, NULL));
    }
}

/* --- IR operations on a $proc node --------------------------------------------------------- */

static void test_merge_leaves_body_alone(void) {
    p_nets nets = p1_nets();
    odin3_node_id node = p1_node(&nets);
    odin3_net_id d2 = wire1("d2");
    const uint8_t *bits = body_of(node)->bits;
    saved_body before;
    save_body(body_of(node), &before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){d2, nets.d}));
    TEST_ASSERT_EQUAL_UINT32(d2.v, odin3_pin_net(module, in_pin(node, 1)).v);
    TEST_ASSERT_TRUE(sinks_include(d2, in_pin(node, 1)));
    TEST_ASSERT_EQUAL_PTR(bits, body_of(node)->bits);
    TEST_ASSERT_EQUAL_UINT32(before.len, body_of(node)->len);
    TEST_ASSERT_EQUAL_MEMORY(before.store, body_of(node)->bits, before.len);
    check_full_ok();
}

/* A second driver of q (an $add) is rule 4's error, as for any cell. */
static void test_rule4_second_driver(void) {
    p_nets nets = p1_nets();
    (void)p1_node(&nets);
    odin3_node_spec add_spec = {type_named("$add"), 0, prov, NULL, 0};
    odin3_netvec add_ports[3] = {{&nets.d, 1}, {&nets.clk, 1}, {&nets.q, 1}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_create_connected(module, &add_spec, add_ports, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_net_driver_count(module, nets.q));
    lines_clear();
    odin3_check_opts fast = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_check_design(design, fast));
    TEST_ASSERT_TRUE(line_has("rule 4"));
}

/* The tombstone compact wrote for a dead $proc: its type, its BODY and its pin net names. */
static void expect_proc_tombstone(uint32_t id, const saved_body *body) {
    const odin3_tombstone *tomb = odin3_tombstone_get(design, id);
    TEST_ASSERT_NOT_NULL(tomb);
    TEST_ASSERT_EQUAL_INT(ODIN3_OBJ_NODE, tomb->kind);
    TEST_ASSERT_EQUAL_UINT32(proc_type.v, tomb->type.v);
    TEST_ASSERT_EQUAL_UINT32(3, tomb->n_params);
    TEST_ASSERT_EQUAL_UINT32(4, tomb->n_pins);
    odin3_value want = saved_value(body);
    TEST_ASSERT_TRUE(odin3_value_equal(&want, &tomb->params[ODIN3_PROC_P_BODY]));
    odin3_proc_body view;
    TEST_ASSERT_TRUE(odin3_proc_body_view(&tomb->params[ODIN3_PROC_P_BODY], &view));
}

static void test_compact_live_and_dead(void) {
    p_nets nets = p1_nets();
    odin3_node_id dead = p1_node(&nets);
    odin3_node_id live = p1_node(&nets);
    saved_body dead_body;
    save_body(body_of(dead), &dead_body);
    saved_body live_body;
    save_body(body_of(live), &live_body);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, dead));
    uint32_t tombs = odin3_tombstone_end(design);
    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    odin3_node_id now = {map.node[live.v]};
    odin3_compact_map_free(&map);
    TEST_ASSERT_EQUAL_UINT32(1, now.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(live_body.len, body_of(now)->len);
    TEST_ASSERT_EQUAL_MEMORY(live_body.store, body_of(now)->bits, live_body.len);
    TEST_ASSERT_EQUAL_UINT32(tombs + 1, odin3_tombstone_end(design));
    expect_proc_tombstone(tombs, &dead_body);
    check_full_ok();
    expect_print_has(now, "    update \\q \\t0\n"); /* the printer sees the renumbered pins */
}

static void test_counter(void) {
    p_nets nets = p1_nets();
    odin3_node_id first = p1_node(&nets);
    odin3_node_id second = p1_node(&nets);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, first));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    /* A replacement made unconnected, then put in second's place. */
    body_buf buf;
    p1_body(&buf);
    proc_pins pins = p1_pins(&nets);
    odin3_value params[3];
    proc_params(&pins, &buf.body, params);
    odin3_node_spec spec = {proc_type, 0, prov, params, 3};
    odin3_node_id third = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, &third));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_replace(module, (odin3_node_pair){second, third}));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(out_pin(third, 0).v, odin3_net_driver(module, nets.q).v);
    odin3_compact_map map;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_compact(module, &map));
    odin3_node_id now = {map.node[third.v]};
    odin3_compact_map_free(&map);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_TRUE(odin3_design_has_processes(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, now));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(module));
    TEST_ASSERT_FALSE(odin3_design_has_processes(design));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_first_proc_module(design).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, proc_type));
    check_full_ok();
}

/* A $add never counts. */
static void test_counter_ignores_other_cells(void) {
    p_nets nets = p1_nets();
    odin3_node_spec add_spec = {type_named("$add"), 0, prov, NULL, 0};
    odin3_netvec add_ports[3] = {{&nets.d, 1}, {&nets.clk, 1}, {&nets.q, 1}};
    odin3_node_id add = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_node_create_connected(module, &add_spec, add_ports, &add));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, add));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(module));
}

/* --- the RTLIL spelling -------------------------------------------------------------------- */

static void expect_print(odin3_node_id node, uint32_t indent, const char *want) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_print(module, node, indent, &text));
    TEST_ASSERT_EQUAL_STRING(want, text.data);
    odin3_strbuf_free(&text);
}

static void test_print_p1(void) {
    p_nets nets = p1_nets();
    odin3_node_id node = {0};
    body_buf buf;
    p1_body(&buf);
    proc_pins pins = p1_pins(&nets);
    odin3_value params[3];
    proc_params(&pins, &buf.body, params);
    odin3_node_spec spec = {proc_type, 0, (odin3_prov_id){0}, params, 3}; /* no SOURCE record */
    odin3_netvec ports[2] = {{pins.in, 2}, {pins.out, 2}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(module, &spec, ports, &node));
    TEST_ASSERT_EQUAL_UINT32(1, node.v);
    expect_print(node, 0,
                 "process $proc$1\n"
                 "  assign \\t0 \\q\n"
                 "  assign \\t0 \\d\n"
                 "  sync posedge \\clk\n"
                 "    update \\q \\t0\n"
                 "end\n");
}

/* With a SOURCE record the name carries its file and line (point (r)); indent shifts each line. */
static void test_print_name_and_indent(void) {
    p_nets nets = p1_nets();
    odin3_node_id node = p1_node(&nets); /* prov: t.v line 1 */
    expect_print(node, 2,
                 "  process $proc$t.v:1$1\n"
                 "    assign \\t0 \\q\n"
                 "    assign \\t0 \\d\n"
                 "    sync posedge \\clk\n"
                 "      update \\q \\t0\n"
                 "  end\n");
}

static void test_print_p2(void) {
    p_nets nets = p2_nets(); /* the $_CONST1_ node is node 1 */
    body_buf buf;
    p2_body(&buf);
    proc_pins pins = p2_pins(&nets);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(&pins, &buf.body, &node));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_print(module, node, 0, &text));
    const char *const order[] = {"attribute \\full_case 1\n", "switch \\s\n", "case 1'1\n",
                                 "assign \\t0 \\d\n",         "case\n",       "end\n"};
    const char *at = strstr(text.data, "assign \\t0 \\d\n") + 1; /* after the root's own */
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        const char *next = strstr(at, order[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(next, order[i]);
        at = next + strlen(order[i]);
    }
    TEST_ASSERT_EQUAL_STRING("process $proc$t.v:1$2\n"
                             "  assign \\t0 \\q\n"
                             "  assign \\t0 \\d\n"
                             "  attribute \\full_case 1\n"
                             "  switch \\s\n"
                             "    case 1'1\n"
                             "      assign \\t0 \\d\n"
                             "    case\n"
                             "  end\n"
                             "  sync posedge \\clk\n"
                             "    update \\q \\t0\n"
                             "end\n",
                             text.data);
    odin3_strbuf_free(&text);
    check_full_ok();
}

/* Node attributes come first; every sync type; parallel_case; a nested switch; compare lists. */
static void test_print_attributes_syncs_nesting(void) {
    p_nets nets = p2_nets();
    body_buf buf;
    p2_body(&buf);
    buf.switches[0].flags = ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE;
    /* Case 1 compares against 1'1 or s itself; case 2 owns switch 1 on s with one case. */
    buf.compares[1] = (odin3_pspan){6, 1};
    buf.cases[1].n_compares = 2;
    buf.cases[2] = (odin3_proc_case){2, 0, 3, 0, 1, 1, {0}};
    buf.switches[1] = (odin3_proc_switch){{6, 1}, 3, 1, 0, {0}};
    buf.cases[3] = (odin3_proc_case){2, 1, 3, 0, 2, 0, {0}};
    buf.compares[2] = (odin3_pspan){7, 1};
    buf.syncs[1] = (odin3_proc_sync){ODIN3_SYNC_NEGEDGE, IN | 0, 4, 0};
    buf.syncs[2] = (odin3_proc_sync){ODIN3_SYNC_LEVEL0, IN | 2, 4, 0};
    buf.syncs[3] = (odin3_proc_sync){ODIN3_SYNC_LEVEL1, OUT | 0, 4, 0};
    buf.syncs[4] = (odin3_proc_sync){ODIN3_SYNC_ALWAYS, 0, 4, 0};
    link_body(&buf, (body_counts){10, 3, 4, 4, 2, 5});
    proc_pins pins = p2_pins(&nets);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(&pins, &buf.body, &node));
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    odin3_value src = {ODIN3_VAL_STRING, 0, NULL, 0, intern("t.v:3"), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern("src"), &src));
    odin3_value one = odin3_value_int(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(module, obj, intern("keep"), &one));
    expect_print(node, 0,
                 "attribute \\src \"t.v:3\"\n"
                 "attribute \\keep 1\n"
                 "process $proc$t.v:1$2\n"
                 "  assign \\t0 \\q\n"
                 "  assign \\t0 \\d\n"
                 "  attribute \\full_case 1\n"
                 "  attribute \\parallel_case 1\n"
                 "  switch \\s\n"
                 "    case 1'1 , \\s\n"
                 "      assign \\t0 \\d\n"
                 "    case\n"
                 "      switch \\s\n"
                 "        case 1'1\n"
                 "      end\n"
                 "  end\n"
                 "  sync posedge \\clk\n"
                 "    update \\q \\t0\n"
                 "  sync negedge \\clk\n"
                 "  sync low \\s\n"
                 "  sync high \\q\n"
                 "  sync always\n"
                 "end\n");
}

/* An 8-bit wire named name: its nets in bit order (vector position k = bit k). */
static void wire8(const char *name, odin3_net_id *nets) {
    odin3_wire_spec spec = {intern(name), 7, 0, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &wire));
    for (uint32_t k = 0; k < 8; k++) {
        nets[k] = odin3_wire_net(module, wire, k);
    }
}

/*
 * Task 3 review M5: multi-bit (chunk) actions, as EL-35/36 v4.2 emit them. 8-bit q, t0, d;
 * I = [clk, d[0..7]], O = [q[0..7], t0[0..7]]; t0 <- q, t0[3:0] <- d[3:0]; posedge clk: q <- t0.
 */
static void test_print_multibit_chunks(void) {
    proc_pins pins = {{wire1("clk")}, 9, {{0}}, 16};
    wire8("d", &pins.in[1]);
    wire8("q", &pins.out[0]);
    wire8("t0", &pins.out[8]);
    odin3_pbit pbits[40];
    for (uint32_t k = 0; k < 8; k++) {
        pbits[k] = OUT | (8 + k);      /* a0 lhs: t0 */
        pbits[8 + k] = OUT | k;        /* a0 rhs: q */
        pbits[24 + k] = OUT | k;       /* a2 lhs: q */
        pbits[32 + k] = OUT | (8 + k); /* a2 rhs: t0 */
    }
    for (uint32_t k = 0; k < 4; k++) {
        pbits[16 + k] = OUT | (8 + k); /* a1 lhs: t0[3:0] */
        pbits[20 + k] = IN | (1 + k);  /* a1 rhs: d[3:0] */
    }
    const odin3_proc_action actions[] = {
        {{0, 8}, {8, 8}, {0}}, {{16, 4}, {20, 4}, {0}}, {{24, 8}, {32, 8}, {0}}};
    const odin3_proc_case root = {0, 0, 0, 2, 0, 0, {0}};
    const odin3_proc_sync sync = {ODIN3_SYNC_POSEDGE, IN | 0, 2, 1};
    odin3_proc_body body = {pbits, 40, NULL, 0, actions, 3, &root, 1, NULL, 0, &sync, 1};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(&pins, &body, &node));
    expect_print(node, 0,
                 "process $proc$t.v:1$1\n"
                 "  assign \\t0 \\q\n"
                 "  assign \\t0 [3:0] \\d [3:0]\n"
                 "  sync posedge \\clk\n"
                 "    update \\q \\t0\n"
                 "end\n");
    check_full_ok();
}

/* P1 on pins (one of them on no net) fails to print, logging that position; out is unchanged. */
static void expect_unconnected(const proc_pins *pins, const char *position) {
    body_buf buf;
    p1_body(&buf);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, create_proc(pins, &buf.body, &node));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_strbuf_append(&text, odin3_bytes_cstr("kept"));
    lines_clear();
    if (st == ODIN3_OK) {
        st = odin3_proc_print(module, node, 0, &text);
    }
    bool kept = text.data != NULL && strcmp(text.data, "kept") == 0;
    odin3_strbuf_free(&text); /* before any assertion, so a failure reports instead of leaking */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st);
    TEST_ASSERT_TRUE_MESSAGE(kept, "out changed on failure");
    TEST_ASSERT_EQUAL_UINT32(1, n_lines);
    char want[LOG_TEXT];
    (void)snprintf(want, sizeof want, "proc_print: node %u: position %s is on an unconnected pin",
                   node.v, position);
    TEST_ASSERT_EQUAL_STRING(want, lines[0]);
}

/*
 * Task 3 review M1: a position on an unconnected pin is not the wildcard '-'. P1 with I pin 0
 * (the clock) on no net is a valid node (rule 11 only warns), but it cannot be printed.
 */
static void test_print_unconnected_pin(void) {
    p_nets nets = p1_nets();
    proc_pins pins = p1_pins(&nets);
    pins.in[0] = (odin3_net_id){0};
    expect_unconnected(&pins, "IN|0");
}

/*
 * Task 4 review minor 5: OUT positions are checked too, here reached through an action span: P1
 * with O pin 1 (t0) on no net fails at the first root action's lhs.
 */
static void test_print_unconnected_out_pin(void) {
    p_nets nets = p1_nets();
    proc_pins pins = p1_pins(&nets);
    pins.out[1] = (odin3_net_id){0};
    expect_unconnected(&pins, "OUT|1");
}

/* The printer refuses a node that is not a live process; out stays as it was. */
static void test_print_refuses(void) {
    p_nets nets = p1_nets();
    odin3_node_id node = p1_node(&nets);
    odin3_node_spec add_spec = {type_named("$add"), 0, prov, NULL, 0};
    odin3_node_id add = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &add_spec, &add));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("keep")));
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_print(module, add, 0, &text));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_print(module, node, 0, &text));
    TEST_ASSERT_EQUAL_UINT32(2, n_lines);
    TEST_ASSERT_EQUAL_STRING("keep", text.data);
    odin3_strbuf_free(&text);
}

static void expect_sigspec(const odin3_net_id *nets, uint32_t n, const char *want) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_sigspec(module, nets, n, &text));
    TEST_ASSERT_EQUAL_STRING(want, text.data != NULL ? text.data : "");
    odin3_strbuf_free(&text);
}

static void test_sigspec(void) {
    odin3_wire_spec spec = {intern("a"), 3, 0, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &wire));
    odin3_net_id bits[4];
    for (uint32_t k = 0; k < 4; k++) {
        bits[k] = odin3_wire_net(module, wire, k);
    }
    expect_sigspec(bits, 2, "\\a [1:0]");
    expect_sigspec(bits, 4, "\\a");
    expect_sigspec(&bits[2], 1, "\\a [2]");
    odin3_net_id c1 = const_net("$_CONST1_");
    odin3_net_id cx = const_net("$_CONSTX_");
    odin3_net_id mixed[3] = {c1, {0}, cx};
    expect_sigspec(mixed, 3, "3'x-1");
    /* Chunks MSB first in braces; non-consecutive bits split; other nets by name or $n<ID>. */
    odin3_net_id named = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, intern("n1"), prov, &named));
    odin3_net_id unnamed = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, prov, &unnamed));
    odin3_net_id concat[4] = {cx, c1, bits[2], bits[3]};
    expect_sigspec(concat, 4, "{ \\a [3:2] 2'1x }");
    odin3_net_id split[2] = {bits[0], bits[2]};
    expect_sigspec(split, 2, "{ \\a [2] \\a [0] }");
    odin3_net_id others[3] = {unnamed, named, bits[1]};
    char want[LOG_TEXT];
    (void)snprintf(want, sizeof want, "{ \\a [1] \\n1 $n%u }", (unsigned)unnamed.v);
    expect_sigspec(others, 3, want);
    odin3_net_id down[2] = {bits[1], bits[0]};
    expect_sigspec(down, 2, "{ \\a [0] \\a [1] }");
    expect_sigspec(NULL, 0, "{ }");
    odin3_net_id t1 = wire1("t1");
    expect_sigspec(&t1, 1, "\\t1");
}

/* The sigspec of a wire bit merged onto a constant net is the constant (as write_rtlil). */
static void test_sigspec_constant_wins(void) {
    odin3_net_id wired = wire1("w");
    odin3_net_id c0 = const_net("$_CONST0_");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(module, (odin3_net_pair){wired, c0}));
    TEST_ASSERT_NOT_EQUAL(0, odin3_net_primary(module, wired).wire.v);
    expect_sigspec(&wired, 1, "1'0");
}

static void test_value_text(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    const uint8_t bits[4] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X, ODIN3_BIT_Z};
    odin3_value vals[5] = {
        odin3_value_int(-7),
        odin3_value_int(INT64_C(1) << 40),
        {ODIN3_VAL_BITS, 0, bits, 4, 0, 0},
        {ODIN3_VAL_STRING, 0, NULL, 0, intern("a\"b\\c\nd\te\x01"), 0},
        {ODIN3_VAL_COVER, 0, (const uint8_t *)"1-1", 3, 2, 0},
    };
    char wide[LOG_TEXT] = "64'";
    for (size_t k = 0; k < 64; k++) { /* MSB first: bit 40 is character 3 + 23 */
        wide[3 + k] = k == 63 - 40 ? '1' : '0';
    }
    const char *const want[5] = {
        "-7", wide, "4'zx01", "\"a\\\"b\\\\c\\nd\\te\\001\"", "\"1-1\"",
    };
    for (size_t i = 0; i < 5; i++) {
        odin3_strbuf_clear(&text);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_value_text(design, &vals[i], &text));
        TEST_ASSERT_EQUAL_STRING(want[i], text.data);
    }
    p_nets nets = p1_nets();
    odin3_node_id node = p1_node(&nets);
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_proc_value_text(design, body_of(node), &text));
    TEST_ASSERT_EQUAL_UINT64(0, text.len);
    odin3_strbuf_free(&text);
}

/* --- values and attributes ----------------------------------------------------------------- */

static void test_value_kind_valid(void) {
    body_buf buf;
    p1_body(&buf);
    odin3_value packed;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, &buf.body, &packed));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_PROC, packed.kind);
    TEST_ASSERT_EQUAL_UINT32(0, packed.len % 8);
    TEST_ASSERT_TRUE(odin3_value_valid(&packed));
    odin3_value empty = {ODIN3_VAL_PROC, 0, NULL, 0, 0, 0};
    TEST_ASSERT_TRUE(odin3_value_valid(&empty));
    odin3_value broken = {ODIN3_VAL_PROC, 0, NULL, 8, 0, 0};
    odin3_value beyond = {(odin3_value_kind)(ODIN3_VAL_PROC + 1), 0, NULL, 0, 0, 0};
    TEST_ASSERT_FALSE(odin3_value_valid(&broken) || odin3_value_valid(&beyond));
}

/* Copies are equal bytes; one changed field, or another length, is not equal. */
static void test_value_kind_bytes(void) {
    body_buf buf;
    p1_body(&buf);
    odin3_value packed;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, &buf.body, &packed));
    odin3_value copy;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &packed, &copy));
    TEST_ASSERT_TRUE(packed.bits != copy.bits && odin3_value_equal(&packed, &copy));
    odin3_value again;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, &buf.body, &again));
    TEST_ASSERT_TRUE(odin3_value_equal(&packed, &again)); /* padding is zero */
    buf.actions[0].prov.v = 9;
    odin3_value changed;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, &buf.body, &changed));
    odin3_value empty = {ODIN3_VAL_PROC, 0, NULL, 0, 0, 0};
    TEST_ASSERT_FALSE(odin3_value_equal(&packed, &changed) || odin3_value_equal(&packed, &empty));
}

/* Attributes never hold a body (only $proc's BODY does). */
static void test_attr_refuses_body(void) {
    body_buf buf;
    p1_body(&buf);
    odin3_value packed;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_body_pack(arena, &buf.body, &packed));
    p_nets nets = p1_nets();
    odin3_objref obj = {ODIN3_OBJ_NET, nets.q.v};
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_attr_set(module, obj, intern("b"), &packed));
    TEST_ASSERT_EQUAL_UINT32(1, n_lines);
    TEST_ASSERT_NULL(odin3_attr_get(module, obj, intern("b")));
}

/* Pack refuses what it cannot write. */
static void test_pack_refuses(void) {
    body_buf buf;
    p1_body(&buf);
    odin3_value out = odin3_value_int(5);
    buf.body.syncs = NULL;
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_pack(arena, &buf.body, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_pack(arena, NULL, &out));
    p1_body(&buf);
    buf.body.n_pbits = UINT32_MAX; /* 16 GiB of positions: more than len can say */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_proc_body_pack(arena, &buf.body, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, out.kind);
    TEST_ASSERT_EQUAL_UINT32(3, n_lines);
}

/* --- registration (point (g), review I14) -------------------------------------------------- */

static const odin3_port_def k_one_port[] = {
    {"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL, false},
};
static const odin3_param_def k_proc_param[] = {
    {"B", ODIN3_VAL_PROC, {ODIN3_VAL_PROC, 0, NULL, 0, 0, 0}},
};

static void test_registration_rule(void) {
    const char *const tail = "only the built-in $proc may have a PROC parameter or "
                             "ODIN3_CT_PROCESS";
    char want[LOG_TEXT];
    odin3_celltype_def flagged = {
        "proc_flag", ODIN3_GRAN_WORD, ODIN3_CT_PROCESS, k_one_port, 1, NULL, 0, NULL, NULL, NULL,
        NULL};
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&flagged));
    (void)snprintf(want, sizeof want, "register_global: cell type 'proc_flag': %s", tail);
    TEST_ASSERT_EQUAL_STRING(want, lines[0]);
    odin3_celltype_def param = {
        "proc_param", ODIN3_GRAN_WORD, 0, k_one_port, 1, k_proc_param, 1, NULL, NULL, NULL, NULL};
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&param));
    (void)snprintf(want, sizeof want, "register_global: cell type 'proc_param': %s", tail);
    TEST_ASSERT_EQUAL_STRING(want, lines[0]);
    lines_clear();
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &param, &id));
    (void)snprintf(want, sizeof want, "add_local: cell type 'proc_param': %s", tail);
    TEST_ASSERT_EQUAL_STRING(want, lines[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_declare_blackbox(design, &flagged, &id));
    /* The rule is identity, not shape: a copy of $proc's definition under another name fails. */
    odin3_celltype_def copy = odin3_cell_proc;
    copy.name = "proc_copy";
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &copy, &id));
    (void)snprintf(want, sizeof want, "add_local: cell type 'proc_copy': %s", tail);
    TEST_ASSERT_EQUAL_STRING(want, lines[0]);
    TEST_ASSERT_EQUAL_UINT32(0, type_named("proc_param").v);
    TEST_ASSERT_EQUAL_UINT32(0, type_named("proc_copy").v);
    /* A kind past PROC is out of range for every definition. */
    const odin3_param_def beyond[] = {
        {"B", (odin3_value_kind)(ODIN3_VAL_PROC + 1), {ODIN3_VAL_INT, 0, NULL, 0, 0, 0}}};
    odin3_celltype_def wild = {
        "wild", ODIN3_GRAN_WORD, 0, k_one_port, 1, beyond, 1, NULL, NULL, NULL, NULL};
    lines_clear();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &wild, &id));
    TEST_ASSERT_TRUE(line_has("parameter kind invalid"));
}

/* Through the ABI a plugin can declare neither (src/api keeps its COVER bound and flag mask). */
static void test_abi_registration_refuses(void) {
    const odin3_plugin_port port = {"A", ODIN3_DIR_IN, 1, NULL, true};
    const odin3_plugin_param param = {"B", ODIN3_VAL_PROC, 0};
    odin3_plugin_celltype flagged = {
        "abi_proc_flag",         ODIN3_GRAN_WORD, ODIN3_CT_PROCESS, &port, NULL, 1, 0, NULL, NULL,
        {NULL, NULL, NULL, NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&flagged));
    odin3_plugin_celltype kinded = {
        "abi_proc_param",        ODIN3_GRAN_WORD, 0, &port, &param, 1, 1, NULL, NULL,
        {NULL, NULL, NULL, NULL}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register(&kinded));
}

/* --- writers ------------------------------------------------------------------------------- */

/* Until the writers refuse processes at their entry (2D Task 4), BODY itself has no form. */
static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        (void)fclose(fp);
    }
    return fp != NULL;
}

static void test_writers_have_no_body_form(void) {
    static const char *const k_json = "test_ir_proc.json";
    static const char *const k_verilog = "test_ir_proc.v";
    (void)remove(k_json); /* no leftovers from an earlier run */
    (void)remove(k_verilog);
    p_nets nets = p1_nets();
    (void)p1_node(&nets);
    lines_clear();
    /* Task 4: each writer refuses at its entry (2D EL-33), before the BODY arms are reached */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_write(design, k_json));
    TEST_ASSERT_TRUE(line_has("module 'm' has processes; run proc first"));
    lines_clear();
    odin3_verilog_opts opts;
    memset(&opts, 0, sizeof opts);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_verilog_write(design, k_verilog, &opts));
    TEST_ASSERT_TRUE(line_has("module 'm' has processes; run proc first"));
    TEST_ASSERT_FALSE(file_exists(k_json) || file_exists(k_verilog)); /* atomic: nothing left */
}

/* --- out of memory ------------------------------------------------------------------------- */

/* A pack into a fresh arena that fails after fail_at allocations; out is unchanged on failure. */
static odin3_status pack_attempt(const odin3_proc_body *body, long fail_at) {
    odin3_arena *fresh = odin3_arena_create(TEST_ARENA_CHUNK);
    TEST_ASSERT_NOT_NULL(fresh);
    odin3_value out = odin3_value_int(7);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_proc_body_pack(fresh, body, &out);
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_body view;
    bool fine = st == ODIN3_OK ? odin3_proc_body_view(&out, &view) : out.kind == ODIN3_VAL_INT;
    odin3_arena_destroy(fresh);
    TEST_ASSERT_TRUE(fine);
    return st;
}

static void test_oom_pack(void) {
    body_buf buf;
    p2_body(&buf);
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        st = pack_attempt(&buf.body, fail_at);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            failures++;
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(1, failures); /* the blob's own chunk */
}

/*
 * Fills the node and pin stores to a page boundary (IR-18), most of the module arena's first chunk
 * (a wide wire's net vector) and the pin pool's chunk, so the next node needs a new node page, a
 * new pin page, a new arena chunk for its parameters and, once committed, a new pin-pool chunk
 * for its first connection (whose failure undoes the committed node).
 */
static void fill_stores(void) {
    odin3_wire_spec wide = {intern("wide"), FILL_WIDTH - 1, 0, false, prov};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &wide, NULL, NULL));
    uint32_t page = UINT32_C(1) << ODIN3_MODULE_PAGE_SHIFT;
    odin3_node_spec spec = {type_named("$_CONST0_"), 0, prov, NULL, 0};
    while (odin3_module_node_end(module) < page) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(module, &spec, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(page, odin3_module_pin_end(module));
    /* Use up the pin pool's chunk: the first pin connected then needs a new one (no pin is yet). */
    odin3_arena *pool = module->pinpool.arena;
    const size_t align = _Alignof(max_align_t);
    const size_t step =
        (2 * sizeof(odin3_pin_id) + align - 1) / align * align; /* a class-0 block */
    while (odin3_arena_bytes_used(pool) + step <= odin3_arena_bytes_reserved(pool)) {
        TEST_ASSERT_NOT_NULL(odin3_pinpool_alloc(&module->pinpool, 0));
    }
}

/*
 * Every allocation create_connected makes may fail: the result is NO_MEMORY (never INVALID_ARG:
 * verify allocates nothing, review I3) with nothing created, the counter unchanged and the IR
 * valid. Each attempt runs on a fresh design whose stores sit at page boundaries.
 */
static void test_oom_create_connected(void) {
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        fresh_design();
        fill_stores();
        p_nets nets = p1_nets();
        body_buf buf;
        p1_body(&buf);
        proc_pins pins = p1_pins(&nets);
        odin3_value params[3];
        proc_params(&pins, &buf.body, params);
        store_ends before = ends_now();
        odin3_node_id node = {0};
        odin3_util_set_alloc_fail_after(fail_at);
        st = create_with(module, &pins, params, &node);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        failures++;
        TEST_ASSERT_EQUAL_UINT32(before.nodes, odin3_module_node_end(module));
        TEST_ASSERT_EQUAL_UINT32(before.pins, odin3_module_pin_end(module));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(module));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, proc_type));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, nets.d).count);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_pins(module, nets.q).count);
        check_full_ok();
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(CREATE_ALLOCS, failures);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    check_full_ok();
}

/* compact of a live and a dead $proc under out of memory: NO_MEMORY with nothing changed. */
static void test_oom_compact(void) {
    p_nets nets = p1_nets();
    odin3_node_id dead = p1_node(&nets);
    (void)p1_node(&nets);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(module, dead));
    uint32_t failures = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (long fail_at = 0; fail_at < OOM_LIMIT && st != ODIN3_OK; fail_at++) {
        uint32_t tombs = odin3_tombstone_end(design);
        odin3_util_set_alloc_fail_after(fail_at);
        st = odin3_module_compact(module, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_UINT32(tombs, odin3_tombstone_end(design));
            TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
            TEST_ASSERT_FALSE(odin3_node_live(module, dead));
            failures++;
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(module));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_module_node_end(module));
    check_full_ok();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_type_is_registered);
    RUN_TEST(test_type_ports_and_params);
    RUN_TEST(test_default_node_cannot_exist);
    RUN_TEST(test_width_params_checked);
    RUN_TEST(test_create_and_query);
    RUN_TEST(test_first_proc_module);
    RUN_TEST(test_fixtures_verify);
    RUN_TEST(test_malformed_bodies);
    RUN_TEST(test_short_blob);
    RUN_TEST(test_misaligned_body);
    RUN_TEST(test_bits_body);
    RUN_TEST(test_view_reads_packed);
    RUN_TEST(test_more_rules);
    RUN_TEST(test_verify_allocates_nothing);
    RUN_TEST(test_wide_body);
    RUN_TEST(test_sync_on_out_position);
    RUN_TEST(test_merge_leaves_body_alone);
    RUN_TEST(test_rule4_second_driver);
    RUN_TEST(test_compact_live_and_dead);
    RUN_TEST(test_counter);
    RUN_TEST(test_counter_ignores_other_cells);
    RUN_TEST(test_print_p1);
    RUN_TEST(test_print_name_and_indent);
    RUN_TEST(test_print_p2);
    RUN_TEST(test_print_attributes_syncs_nesting);
    RUN_TEST(test_print_refuses);
    RUN_TEST(test_print_multibit_chunks);
    RUN_TEST(test_print_unconnected_pin);
    RUN_TEST(test_print_unconnected_out_pin);
    RUN_TEST(test_sigspec);
    RUN_TEST(test_sigspec_constant_wins);
    RUN_TEST(test_value_text);
    RUN_TEST(test_value_kind_valid);
    RUN_TEST(test_value_kind_bytes);
    RUN_TEST(test_attr_refuses_body);
    RUN_TEST(test_pack_refuses);
    RUN_TEST(test_registration_rule);
    RUN_TEST(test_abi_registration_refuses);
    RUN_TEST(test_writers_have_no_body_form);
    RUN_TEST(test_oom_pack);
    RUN_TEST(test_oom_create_connected);
    RUN_TEST(test_oom_compact);
    return UNITY_END();
}
