/*
 * fuzz_abc_splice.c — the abc pass's splice, fuzzed (3D spec §13.7; plan Task 19), a libFuzzer
 * target built with -DODIN3_FUZZ=ON under clang, ASan and UBSan. It runs the whole pass on the
 * fake engine (tests/unit/abc_fake_engine.c); ABC is never linked.
 *
 * Input: byte 0 is K (2 + byte % 5) and bit 7 asks for an allocation failure at the index given
 * by bytes 1 and 2; byte 3 is the fake's choices (bit 0 `scorr` is always on in the default
 * script; bits 1 to 4 pick `const <i>` for the extra command, bit 5 selects value 1, bit 6 turns
 * it on); the rest of the bytes build a hand-made module: four inputs, a clock, an `unconn`
 * generator, a constant generator, then up to 32 items of three bytes each: a gate (AND, OR, XOR,
 * NOT, BUF) over earlier nets, a flip-flop (INIT 0 to 3, a global or a clocked one, optionally on
 * a derived clock `clk & a0`), or an output port on an earlier net. Every cell carries a SOURCE
 * record.
 *
 * Invariants (a violation is __builtin_trap(), which libFuzzer reports; tests/ only):
 *  - the status is OK, or, with an injected failure, NO_MEMORY, IO or ENGINE (the fake reads the
 *    exchange file with 1C's reader);
 *  - the FULL check passes, whatever the status;
 *  - a module that simulates (no derived clock, no loop) gives the same 64 vectors of 1E
 *    simulation as before, on success and on failure;
 *  - after a success every `$lut` and every flip-flop of the design has a record that reaches a
 *    SOURCE;
 *  - no leak, no sanitizer report, no hang.
 */
#include "../unit/abc_fake_engine.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/abc/abc_test.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum {
    HEADER_BYTES = 4,
    ITEM_BYTES = 3,
    MAX_ITEMS = 32,
    MAX_NETS = 64,
    K_BASE = 2,
    K_SPAN = 5,
    FAIL_BIT = 0x80,
    CONST_ON = 0x40,
    CONST_VALUE = 0x20,
    CONST_INDEX_SHIFT = 1,
    CONST_INDEX_MASK = 0xF,
    ITEM_KINDS = 8,
    INPUTS = 4,
    SIM_CYCLES = 64,
    SIM_SEEDS = 2,
    TEXT_CAP = 64
};

static void die(const char *what) {
    (void)fprintf(stderr, "fuzz_abc_splice: %s\n", what);
    __builtin_trap();
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    odin3_log_mute(true);
    return 0;
}

/* --- building ------------------------------------------------------------------------------- */

typedef struct build {
    odin3_design *design;
    odin3_module *mod;
    odin3_pass_ctx reader;
    odin3_net_id nets[MAX_NETS]; /* usable sources, in creation order */
    uint32_t n_nets;
    uint32_t outputs;
    uint32_t cells;
    bool derived;
} build;

static uint32_t intern(build *bd, const char *text) {
    uint32_t str = 0;
    if (odin3_design_intern(bd->design, odin3_bytes_cstr(text), &str) != ODIN3_OK) {
        die("intern");
    }
    return str;
}

static odin3_prov_id fresh_source(build *bd) {
    odin3_srcloc loc = {intern(bd, "f.v"), ++bd->cells, 1, bd->cells, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    if (odin3_prov_source(&bd->reader, &origin, &id) != ODIN3_OK) {
        die("source");
    }
    return id;
}

static odin3_net_id add_port(build *bd, const char *name, odin3_dir dir) {
    odin3_port_spec spec = {intern(bd, name), dir, 1, true, (odin3_prov_id){0}, false, 0, 0, false};
    odin3_node_id node = {0};
    if (odin3_module_add_port(bd->mod, &spec, &node) != ODIN3_OK) {
        die("add_port");
    }
    return odin3_pin_net(bd->mod, odin3_node_pins(bd->mod, node).first);
}

static void push_net(build *bd, odin3_net_id net) {
    if (bd->n_nets < MAX_NETS) {
        bd->nets[bd->n_nets++] = net;
    }
}

static odin3_net_id new_net(build *bd) {
    odin3_net_id net = {0};
    if (odin3_net_create(bd->mod, 0, (odin3_prov_id){0}, &net) != ODIN3_OK) {
        die("net_create");
    }
    return net;
}

static odin3_node_id make_cell(build *bd, const char *type, const odin3_net_id *nets,
                               uint32_t count, const odin3_value *init) {
    odin3_netvec ports[3];
    for (uint32_t i = 0; i < count; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    uint32_t name = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(bd->design), odin3_bytes_cstr(type), &name) ||
        !odin3_celltype_find(bd->design, name, &id)) {
        die("cell type");
    }
    odin3_node_spec spec = {id, 0, fresh_source(bd), init, init != NULL ? 1U : 0U};
    odin3_node_id node = {0};
    if (odin3_node_create_connected(bd->mod, &spec, ports, &node) != ODIN3_OK) {
        die("create");
    }
    return node;
}

static odin3_net_id pick(const build *bd, uint8_t byte) {
    return bd->nets[byte % bd->n_nets];
}

/* The output net of an item: an output port when the item says so, else a fresh net. */
static odin3_net_id sink(build *bd, uint8_t flags) {
    if ((flags & 1U) != 0 && bd->outputs < MAX_ITEMS) {
        char name[TEXT_CAP];
        (void)snprintf(name, sizeof name, "o%u", (unsigned)bd->outputs++);
        return add_port(bd, name, ODIN3_DIR_OUT);
    }
    return new_net(bd);
}

static void add_gate(build *bd, const uint8_t *item) {
    static const char *const k_gates[] = {"$_AND_", "$_OR_", "$_XOR_", "$_NOT_", "$_BUF_"};
    static const uint32_t k_inputs[] = {2, 2, 2, 1, 1};
    uint32_t kind = (item[0] >> 4U) % (sizeof k_gates / sizeof k_gates[0]);
    odin3_net_id nets[3];
    nets[0] = pick(bd, item[1]);
    nets[1] = k_inputs[kind] == 2 ? pick(bd, item[2]) : sink(bd, item[0]);
    if (k_inputs[kind] == 2) {
        nets[2] = sink(bd, item[0]);
    }
    uint32_t count = k_inputs[kind] + 1U;
    (void)make_cell(bd, k_gates[kind], nets, count, NULL);
    push_net(bd, nets[count - 1U]);
}

static void add_flop(build *bd, const uint8_t *item) {
    odin3_value init = odin3_value_int(item[0] & 3U);
    odin3_net_id qnet = sink(bd, item[0]);
    if ((item[0] & 4U) != 0) { /* a global-clock flip-flop */
        odin3_net_id nets[2] = {pick(bd, item[1]), qnet};
        (void)make_cell(bd, "$_FF_", nets, 2, &init);
    } else {
        odin3_net_id clk = bd->nets[INPUTS]; /* the clock input */
        if ((item[0] & 8U) != 0 && bd->n_nets > INPUTS + 1U) {
            clk = bd->nets[INPUTS + 1U]; /* the derived clock clk & a0 */
            bd->derived = true;
        }
        odin3_net_id nets[3] = {clk, pick(bd, item[1]), qnet};
        (void)make_cell(bd, "$_DFF_P_", nets, 3, &init);
    }
    push_net(bd, qnet);
}

static void add_output(build *bd, const uint8_t *item) {
    if (bd->outputs >= MAX_ITEMS) {
        return;
    }
    char name[TEXT_CAP];
    (void)snprintf(name, sizeof name, "o%u", (unsigned)bd->outputs++);
    odin3_net_id port = add_port(bd, name, ODIN3_DIR_OUT);
    odin3_net_id nets[2] = {pick(bd, item[1]), port};
    (void)make_cell(bd, "$_BUF_", nets, 2, NULL);
}

static void add_item(build *bd, const uint8_t *item) {
    switch (item[0] & 3U) {
    case 0:
    case 1:
        add_gate(bd, item);
        break;
    case 2:
        add_flop(bd, item);
        break;
    default:
        add_output(bd, item);
        break;
    }
}

/* The fixed part: inputs, the clock, the derived clock, `unconn` and a constant generator. */
static void add_base(build *bd) {
    for (uint32_t i = 0; i < INPUTS; i++) {
        char name[TEXT_CAP];
        (void)snprintf(name, sizeof name, "a%u", (unsigned)i);
        push_net(bd, add_port(bd, name, ODIN3_DIR_IN));
    }
    push_net(bd, add_port(bd, "clk", ODIN3_DIR_IN));
    odin3_net_id derived = new_net(bd);
    odin3_net_id gate[3] = {bd->nets[INPUTS], bd->nets[0], derived};
    (void)make_cell(bd, "$_AND_", gate, 3, NULL);
    push_net(bd, derived);
    uint32_t unconn_name = intern(bd, "unconn");
    odin3_net_id unconn = {0};
    if (odin3_net_create(bd->mod, unconn_name, (odin3_prov_id){0}, &unconn) != ODIN3_OK) {
        die("unconn");
    }
    odin3_netvec ports[2] = {{&unconn, 0}, {&unconn, 1}};
    uint32_t sop = 0;
    odin3_celltype_id sop_type = {0};
    if (!odin3_strtab_find(odin3_design_strtab(bd->design), odin3_bytes_cstr("$sop"), &sop) ||
        !odin3_celltype_find(bd->design, sop, &sop_type)) {
        die("sop");
    }
    odin3_node_spec spec = {sop_type, 0, fresh_source(bd), NULL, 2};
    if (odin3_node_create_connected(bd->mod, &spec, ports, NULL) != ODIN3_OK) {
        die("unconn generator");
    }
    push_net(bd, unconn);
    odin3_net_id one = new_net(bd);
    (void)make_cell(bd, "$_CONST1_", &one, 1, NULL);
    push_net(bd, one);
}

static bool build_design(build *bd, const uint8_t *data, size_t size) {
    bd->design = odin3_design_create();
    if (bd->design == NULL) {
        die("design");
    }
    uint32_t pass = intern(bd, "fuzz");
    if (odin3_pass_run_begin(bd->design, pass, &bd->reader) != ODIN3_OK) {
        die("run begin");
    }
    odin3_module_id mid = {0};
    if (odin3_module_create(bd->design, intern(bd, "m"), (odin3_prov_id){0}, &mid) != ODIN3_OK) {
        die("module");
    }
    bd->mod = odin3_module_get(bd->design, mid);
    add_base(bd);
    size_t items = (size - HEADER_BYTES) / ITEM_BYTES;
    for (size_t i = 0; i < items && i < MAX_ITEMS; i++) {
        add_item(bd, data + HEADER_BYTES + i * ITEM_BYTES);
    }
    return bd->derived;
}

/* --- simulation ----------------------------------------------------------------------------- */

typedef struct trace {
    uint8_t *bits;
    size_t len;
} trace;

static bool record(odin3_design *design, trace *out) {
    odin3_sim *sim = NULL;
    if (odin3_sim_build(design, odin3_design_top(design), &sim) != ODIN3_OK) {
        return false;
    }
    uint32_t width = 0;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        width += odin3_sim_output_width(sim, port);
    }
    out->len = (size_t)width * SIM_CYCLES * SIM_SEEDS;
    out->bits = calloc(out->len == 0 ? 1 : out->len, 1);
    if (out->bits == NULL) {
        die("trace");
    }
    size_t at = 0;
    for (uint32_t seed = 1; seed <= SIM_SEEDS; seed++) {
        odin3_prng prng;
        odin3_prng_seed(&prng, seed);
        for (uint32_t cycle = 0; cycle < SIM_CYCLES; cycle++) {
            odin3_sim_drive_random(sim, &prng);
            odin3_sim_cycle(sim);
            for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
                for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
                    bool value = false;
                    (void)odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &value);
                    out->bits[at++] = value ? 1U : 0U;
                }
            }
        }
    }
    odin3_sim_destroy(sim);
    return true;
}

/* --- after the pass ------------------------------------------------------------------------- */

static void count_leaf(void *user, odin3_prov_id leaf) {
    (void)leaf;
    (*(uint32_t *)user)++;
}

/* Every `$lut` and flip-flop has a record that reaches a SOURCE. */
static void check_records(const build *bd) {
    const odin3_module *mod = bd->mod;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node)) {
            continue;
        }
        const odin3_celltype_def *def = odin3_celltype_get(bd->design, odin3_node_type(mod, node));
        if (strcmp(def->name, "$lut") != 0 && strcmp(def->name, "$_FF_") != 0 &&
            strcmp(def->name, "$_DFF_P_") != 0) {
            continue;
        }
        uint32_t leaves = 0;
        odin3_prov_id prov = odin3_node_prov(mod, node);
        if (prov.v == 0 || odin3_prov_sources(bd->design, prov, count_leaf, &leaves) != ODIN3_OK ||
            leaves == 0) {
            die("a new object has no record that reaches a SOURCE");
        }
    }
}

static void check_valid(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    if (odin3_check_design(design, opts) != ODIN3_OK) {
        die("the FULL check failed");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < HEADER_BYTES) {
        return 0;
    }
    build bd = {0};
    bool derived = build_design(&bd, data, size);
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    if ((data[3] & CONST_ON) != 0) {
        char extra[TEXT_CAP];
        (void)snprintf(extra, sizeof extra, "const %u %u",
                       (unsigned)((data[3] >> CONST_INDEX_SHIFT) & CONST_INDEX_MASK),
                       (data[3] & CONST_VALUE) != 0 ? 1U : 0U);
        odin3_abc_fake_extra(extra);
    }
    trace before = {0};
    bool simulates = !derived && record(bd.design, &before);
    char args[TEXT_CAP];
    (void)snprintf(args, sizeof args, "--lut %u", (unsigned)(K_BASE + data[0] % K_SPAN));
    bool inject = (data[0] & FAIL_BIT) != 0;
    if (inject) {
        odin3_util_set_alloc_fail_after((long)(data[1] | (data[2] << 8U)));
    }
    odin3_status st = odin3_pass_run(bd.design, "abc", odin3_bytes_cstr(args));
    odin3_util_set_alloc_fail_after(-1);
    bool tolerated =
        inject && (st == ODIN3_ERR_NO_MEMORY || st == ODIN3_ERR_IO || st == ODIN3_ERR_ENGINE);
    if (st != ODIN3_OK && !tolerated) {
        die("an unexpected status");
    }
    check_valid(bd.design);
    if (simulates && (data[3] & CONST_ON) == 0) { /* `const` changes the function on purpose */
        trace after = {0};
        if (!record(bd.design, &after) || after.len != before.len ||
            memcmp(after.bits, before.bits, before.len) != 0) {
            die("simulation differs after the pass");
        }
        free(after.bits);
    }
    if (st == ODIN3_OK) {
        check_records(&bd);
    }
    free(before.bits);
    odin3_abc_test_set_engine(NULL);
    odin3_design_destroy(bd.design);
    return 0;
}
