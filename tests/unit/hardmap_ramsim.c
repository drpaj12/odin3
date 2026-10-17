/* hardmap_ramsim.c — see hardmap_ramsim.h. */
#include "hardmap_ramsim.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "sim/prng.h"
#include "unity.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum rs_kind {
    K_NONE,
    K_C0,
    K_C1,
    K_CX,
    K_AND,
    K_NOT,
    K_MUX,
    K_DFF,
    K_IO,
    K_SP,
    K_DP
} rs_kind;

enum { MAX_DATA = 4, MAX_ADDR = 20, DP_PORTS = 2 };

/* The pin ports of a RAM, per read/write port (same tables as ram.c's layouts). */
typedef struct rs_pins {
    uint32_t n_ports;
    uint32_t data[DP_PORTS];
    uint32_t addr[DP_PORTS];
    uint32_t we[DP_PORTS];
    uint32_t out[DP_PORTS];
} rs_pins;

static const rs_pins k_sp_pins = {1, {1, 0}, {2, 0}, {3, 0}, {4, 0}};
static const rs_pins k_dp_pins = {2, {2, 1}, {4, 3}, {6, 5}, {8, 7}};

typedef struct rs_ram {
    uint8_t *mem; /* words * data bits */
    uint32_t data_bits;
    uint32_t addr_bits;
    uint8_t reg[DP_PORTS][MAX_DATA]; /* the registered read of each port */
} rs_ram;

typedef struct rs {
    const odin3_module *mod;
    uint32_t n_nodes;
    uint8_t *val;  /* by net */
    uint8_t *ff;   /* by node: a `$dff`'s state */
    rs_kind *kind; /* by node */
    rs_ram *ram;   /* by node */
} rs;

static rs_kind kind_of(const odin3_module *mod, odin3_node_id node) {
    static const struct {
        const char *name;
        rs_kind kind;
    } k_names[] = {{"$_CONST0_", K_C0}, {"$_CONST1_", K_C1},       {"$_CONSTX_", K_CX},
                   {"$and", K_AND},     {"$not", K_NOT},           {"$mux", K_MUX},
                   {"$dff", K_DFF},     {"single_port_ram", K_SP}, {"dual_port_ram", K_DP}};
    if (!odin3_node_live(mod, node)) {
        return K_NONE;
    }
    const char *name =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node))->name;
    if (strncmp(name, "$port_", 6) == 0) {
        return K_IO; /* the module's ports: the test sets and reads their nets */
    }
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strcmp(name, k_names[i].name) == 0) {
            return k_names[i].kind;
        }
    }
    TEST_ASSERT_MESSAGE(false, name); /* a cell the model does not know */
    return K_NONE;
}

static const rs_pins *pins_of(rs_kind kind) {
    return kind == K_SP ? &k_sp_pins : &k_dp_pins;
}

/* A pin of a node: pin port (the cell type's port index) and bit. */
typedef struct rs_pin {
    uint32_t port;
    uint32_t bit;
} rs_pin;

static odin3_net_id pin_net(const rs *sim, odin3_node_id node, rs_pin pin) {
    odin3_pinslice pins = odin3_node_port(sim->mod, node, pin.port);
    TEST_ASSERT_TRUE(pin.bit < pins.count);
    return odin3_pin_net(sim->mod, (odin3_pin_id){pins.first.v + pin.bit});
}

static uint8_t pin_val(const rs *sim, odin3_node_id node, rs_pin pin) {
    odin3_net_id net = pin_net(sim, node, pin);
    return odin3_net_valid(net) ? sim->val[net.v] : 0;
}

/* Sets the net of a pin; true when its value changed. */
static bool pin_set(rs *sim, odin3_node_id node, rs_pin pin, uint8_t value) {
    odin3_net_id net = pin_net(sim, node, pin);
    if (!odin3_net_valid(net) || sim->val[net.v] == value) {
        return false;
    }
    sim->val[net.v] = value;
    return true;
}

/* The unsigned value of the first `bits` bits of pin port `port`. */
typedef struct rs_word {
    uint32_t port;
    uint32_t bits;
} rs_word;

static uint32_t word_at(const rs *sim, odin3_node_id node, rs_word word) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < word.bits; i++) {
        value |= (uint32_t)pin_val(sim, node, (rs_pin){word.port, i}) << i;
    }
    return value;
}

/* The outputs of a RAM: each port's registered read. */
static bool ram_outputs(rs *sim, odin3_node_id node) {
    const rs_pins *pp = pins_of(sim->kind[node.v]);
    const rs_ram *ram = &sim->ram[node.v];
    bool changed = false;
    for (uint32_t port_no = 0; port_no < pp->n_ports; port_no++) {
        for (uint32_t bit = 0; bit < ram->data_bits; bit++) {
            changed |= pin_set(sim, node, (rs_pin){pp->out[port_no], bit}, ram->reg[port_no][bit]);
        }
    }
    return changed;
}

/* The combinational and registered outputs of one node; true when a net changed. */
static bool eval_node(rs *sim, odin3_node_id node) {
    switch (sim->kind[node.v]) {
    case K_C0:
    case K_CX:
        return pin_set(sim, node, (rs_pin){0, 0}, 0);
    case K_C1:
        return pin_set(sim, node, (rs_pin){0, 0}, 1);
    case K_AND:
        return pin_set(sim, node, (rs_pin){2, 0},
                       pin_val(sim, node, (rs_pin){0, 0}) & pin_val(sim, node, (rs_pin){1, 0}));
    case K_NOT:
        return pin_set(sim, node, (rs_pin){1, 0}, pin_val(sim, node, (rs_pin){0, 0}) ^ 1U);
    case K_MUX:
        return pin_set(sim, node, (rs_pin){3, 0},
                       pin_val(sim, node, (rs_pin){2, 0}) != 0
                           ? pin_val(sim, node, (rs_pin){1, 0})
                           : pin_val(sim, node, (rs_pin){0, 0}));
    case K_DFF:
        return pin_set(sim, node, (rs_pin){2, 0}, sim->ff[node.v]);
    case K_SP:
    case K_DP:
        return ram_outputs(sim, node);
    default:
        return false;
    }
}

static void settle(rs *sim) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t id = 1; id < sim->n_nodes; id++) {
            changed |= eval_node(sim, (odin3_node_id){id});
        }
    }
}

/* The clock edge of one RAM: both ports write, then both register their read. */
static void ram_edge(rs *sim, odin3_node_id node) {
    rs_ram *ram = &sim->ram[node.v];
    const rs_pins *pp = pins_of(sim->kind[node.v]);
    for (uint32_t port_no = 0; port_no < pp->n_ports; port_no++) {
        uint32_t word = word_at(sim, node, (rs_word){pp->addr[port_no], ram->addr_bits});
        for (uint32_t bit = 0;
             bit < ram->data_bits && pin_val(sim, node, (rs_pin){pp->we[port_no], 0}) != 0; bit++) {
            ram->mem[(size_t)word * ram->data_bits + bit] =
                pin_val(sim, node, (rs_pin){pp->data[port_no], bit});
        }
    }
    for (uint32_t port_no = 0; port_no < pp->n_ports; port_no++) {
        uint32_t word = word_at(sim, node, (rs_word){pp->addr[port_no], ram->addr_bits});
        for (uint32_t bit = 0; bit < ram->data_bits; bit++) {
            ram->reg[port_no][bit] = ram->mem[(size_t)word * ram->data_bits + bit];
        }
    }
}

static void edge(rs *sim) {
    uint8_t *next = calloc(sim->n_nodes, 1);
    TEST_ASSERT_NOT_NULL(next);
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        if (sim->kind[id] == K_DFF) {
            next[id] = pin_val(sim, (odin3_node_id){id}, (rs_pin){1, 0});
        }
    }
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        if (sim->kind[id] == K_SP || sim->kind[id] == K_DP) {
            ram_edge(sim, (odin3_node_id){id});
        }
    }
    memcpy(sim->ff, next, sim->n_nodes);
    free(next);
}

static void setup_ram(rs *sim, odin3_node_id node) {
    const rs_pins *pp = pins_of(sim->kind[node.v]);
    rs_ram *ram = &sim->ram[node.v];
    ram->data_bits = odin3_node_port(sim->mod, node, pp->data[0]).count;
    ram->addr_bits = odin3_node_port(sim->mod, node, pp->addr[0]).count;
    TEST_ASSERT_TRUE(ram->data_bits <= MAX_DATA && ram->addr_bits <= MAX_ADDR);
    ram->mem = calloc(((size_t)1 << ram->addr_bits) * ram->data_bits, 1);
    TEST_ASSERT_NOT_NULL(ram->mem);
}

static void rs_init(rs *sim, const odin3_module *mod) {
    memset(sim, 0, sizeof *sim);
    sim->mod = mod;
    sim->n_nodes = odin3_module_node_end(mod);
    sim->val = calloc(odin3_module_net_end(mod), 1);
    sim->ff = calloc(sim->n_nodes, 1);
    sim->kind = calloc(sim->n_nodes, sizeof *sim->kind);
    sim->ram = calloc(sim->n_nodes, sizeof *sim->ram);
    TEST_ASSERT_TRUE(sim->val != NULL && sim->ff != NULL && sim->kind != NULL && sim->ram != NULL);
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        sim->kind[id] = kind_of(mod, (odin3_node_id){id});
        if (sim->kind[id] == K_SP || sim->kind[id] == K_DP) {
            setup_ram(sim, (odin3_node_id){id});
        }
    }
}

static void rs_free(rs *sim) {
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        free(sim->ram[id].mem);
    }
    free(sim->val);
    free(sim->ff);
    free(sim->kind);
    free(sim->ram);
}

void rs_trace(const odin3_module *module, const rs_io *io, rs_run run, odin3_strbuf *out) {
    rs sim;
    odin3_prng prng;
    rs_init(&sim, module);
    odin3_prng_seed(&prng, run.seed);
    for (uint32_t cycle = 0; cycle < run.cycles; cycle++) {
        for (uint32_t i = 0; i < io->n_in; i++) {
            sim.val[io->in[i].v] = odin3_prng_bit(&prng) ? 1U : 0U;
        }
        settle(&sim);
        for (uint32_t i = 0; i < io->n_out; i++) {
            const char bit = sim.val[io->out[i].v] != 0 ? '1' : '0';
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){&bit, 1}));
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
        edge(&sim);
    }
    rs_free(&sim);
}
