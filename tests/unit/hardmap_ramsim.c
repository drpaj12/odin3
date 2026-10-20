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
    K_DFFE,
    K_IO,
    K_SP,
    K_DP,
    K_MEM
} rs_kind;

enum { MAX_DATA = 64, MAX_ADDR = 20, DP_PORTS = 2 };

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

/* A `$mem`: SIZE words of WIDTH bits, the registered read of each synchronous port. */
typedef struct rs_mem {
    uint8_t *mem;
    uint8_t *reg; /* n_rd * width */
    uint32_t size;
    uint32_t abits;
    uint32_t width;
    uint32_t n_rd;
    uint32_t n_wr;
} rs_mem;

typedef struct rs {
    const odin3_module *mod;
    uint32_t n_nodes;
    uint32_t n_pins;
    uint8_t *val;  /* by net */
    uint8_t *ffq;  /* by pin: the state of a flip-flop's Q pin */
    rs_kind *kind; /* by node */
    rs_ram *ram;   /* by node */
    rs_mem *memo;  /* by node */
} rs;

static rs_kind kind_of(const odin3_module *mod, odin3_node_id node) {
    static const struct {
        const char *name;
        rs_kind kind;
    } k_names[] = {{"$_CONST0_", K_C0},     {"$_CONST1_", K_C1},
                   {"$_CONSTX_", K_CX},     {"$and", K_AND},
                   {"$not", K_NOT},         {"$mux", K_MUX},
                   {"$dff", K_DFF},         {"single_port_ram", K_SP},
                   {"dual_port_ram", K_DP}, {"$dffe", K_DFFE},
                   {"$mem", K_MEM}};
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

/* A flip-flop: `$dff` has CLK D Q, `$dffe` has CLK EN D Q. */
static bool ff_outputs(rs *sim, odin3_node_id node) {
    uint32_t q_port = sim->kind[node.v] == K_DFFE ? 3 : 2;
    odin3_pinslice qpins = odin3_node_port(sim->mod, node, q_port);
    bool changed = false;
    for (uint32_t bit = 0; bit < qpins.count; bit++) {
        changed |= pin_set(sim, node, (rs_pin){q_port, bit}, sim->ffq[qpins.first.v + bit]);
    }
    return changed;
}

/* The address on `abits` pins starting at pin index first of port. */
typedef struct rs_addr {
    uint32_t port;
    uint32_t first;
    uint32_t abits;
} rs_addr;

static uint32_t addr_at(const rs *sim, odin3_node_id node, rs_addr at) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < at.abits; i++) {
        value |= (uint32_t)pin_val(sim, node, (rs_pin){at.port, at.first + i}) << i;
    }
    return value;
}

/* The word a port reads: 0 past the end. */
static uint8_t mem_bit(const rs_mem *mm, uint32_t word, uint32_t bit) {
    return word < mm->size ? mm->mem[(size_t)word * mm->width + bit] : 0;
}

/* The outputs of a `$mem`: an asynchronous port reads the word now, a synchronous one its
 * register. */
static bool mem_outputs(rs *sim, odin3_node_id node) {
    const rs_mem *mm = &sim->memo[node.v];
    const odin3_value *clk_en = odin3_node_param(sim->mod, node, 7);
    bool changed = false;
    for (uint32_t port_no = 0; port_no < mm->n_rd; port_no++) {
        uint32_t word = addr_at(sim, node, (rs_addr){2, port_no * mm->abits, mm->abits});
        for (uint32_t bit = 0; bit < mm->width; bit++) {
            uint32_t at = port_no * mm->width + bit;
            uint8_t val =
                clk_en->bits[port_no] == ODIN3_BIT_1 ? mm->reg[at] : mem_bit(mm, word, bit);
            changed |= pin_set(sim, node, (rs_pin){3, at}, val);
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
    case K_DFFE:
        return ff_outputs(sim, node);
    case K_MEM:
        return mem_outputs(sim, node);
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

/* The clock edge of one RAM: both ports register their read of the old word (vtr_flow/
 * primitives.v: "Old data read-first behaviour"), then both write; port 2 writes last. */
static void ram_edge(rs *sim, odin3_node_id node) {
    rs_ram *ram = &sim->ram[node.v];
    const rs_pins *pp = pins_of(sim->kind[node.v]);
    for (uint32_t port_no = 0; port_no < pp->n_ports && port_no < DP_PORTS; port_no++) {
        uint32_t word = word_at(sim, node, (rs_word){pp->addr[port_no], ram->addr_bits});
        for (uint32_t bit = 0; bit < ram->data_bits && bit < MAX_DATA; bit++) {
            ram->reg[port_no][bit] = ram->mem[(size_t)word * ram->data_bits + bit];
        }
    }
    for (uint32_t port_no = 0; port_no < pp->n_ports; port_no++) {
        uint32_t word = word_at(sim, node, (rs_word){pp->addr[port_no], ram->addr_bits});
        for (uint32_t bit = 0;
             bit < ram->data_bits && pin_val(sim, node, (rs_pin){pp->we[port_no], 0}) != 0; bit++) {
            ram->mem[(size_t)word * ram->data_bits + bit] =
                pin_val(sim, node, (rs_pin){pp->data[port_no], bit});
        }
    }
}

/* The writes of a `$mem` in port order: a later port wins a collision. */
static void mem_write(rs *sim, odin3_node_id node) {
    rs_mem *mm = &sim->memo[node.v];
    for (uint32_t port_no = 0; port_no < mm->n_wr; port_no++) {
        uint32_t word = addr_at(sim, node, (rs_addr){6, port_no * mm->abits, mm->abits});
        for (uint32_t bit = 0; word < mm->size && bit < mm->width; bit++) {
            uint32_t at = port_no * mm->width + bit;
            if (pin_val(sim, node, (rs_pin){5, at}) != 0) {
                mm->mem[(size_t)word * mm->width + bit] = pin_val(sim, node, (rs_pin){7, at});
            }
        }
    }
}

/* The synchronous read ports of a `$mem` whose RD_TRANSPARENT is `transparent` register their
 * word; a port whose RD_EN is 0 holds. */
static void mem_read(rs *sim, odin3_node_id node, bool transparent) {
    rs_mem *mm = &sim->memo[node.v];
    const odin3_value *clk_en = odin3_node_param(sim->mod, node, 7);
    const odin3_value *transp = odin3_node_param(sim->mod, node, 9);
    for (uint32_t port_no = 0; port_no < mm->n_rd; port_no++) {
        bool listed = clk_en->bits[port_no] == ODIN3_BIT_1 &&
                      (transp->bits[port_no] == ODIN3_BIT_1) == transparent &&
                      pin_val(sim, node, (rs_pin){1, port_no}) != 0;
        uint32_t word = addr_at(sim, node, (rs_addr){2, port_no * mm->abits, mm->abits});
        for (uint32_t bit = 0; listed && bit < mm->width; bit++) {
            mm->reg[port_no * mm->width + bit] = mem_bit(mm, word, bit);
        }
    }
}

/* The clock edge of a `$mem`: read-first ports register, the writes apply, then the transparent
 * ports register. */
static void mem_edge(rs *sim, odin3_node_id node) {
    mem_read(sim, node, false);
    mem_write(sim, node);
    mem_read(sim, node, true);
}

static bool is_ff(rs_kind kind) {
    return kind == K_DFF || kind == K_DFFE;
}

/* The next state of one flip-flop's Q pins, into next (by pin). */
static void ff_next(const rs *sim, odin3_node_id node, uint8_t *next) {
    bool dffe = sim->kind[node.v] == K_DFFE;
    uint32_t d_port = dffe ? 2 : 1;
    bool enabled = !dffe || pin_val(sim, node, (rs_pin){1, 0}) ==
                                (uint8_t)(odin3_node_param(sim->mod, node, 2)->i != 0);
    odin3_pinslice qpins = odin3_node_port(sim->mod, node, d_port + 1);
    for (uint32_t bit = 0; bit < qpins.count; bit++) {
        next[qpins.first.v + bit] =
            enabled ? pin_val(sim, node, (rs_pin){d_port, bit}) : sim->ffq[qpins.first.v + bit];
    }
}

static void edge(rs *sim) {
    uint8_t *next = calloc(sim->n_pins + 1, 1);
    TEST_ASSERT_NOT_NULL(next);
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        if (is_ff(sim->kind[id])) {
            ff_next(sim, (odin3_node_id){id}, next);
        }
    }
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        if (sim->kind[id] == K_SP || sim->kind[id] == K_DP) {
            ram_edge(sim, (odin3_node_id){id});
        } else if (sim->kind[id] == K_MEM) {
            mem_edge(sim, (odin3_node_id){id});
        }
    }
    memcpy(sim->ffq, next, sim->n_pins + 1);
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

static void setup_mem(rs *sim, odin3_node_id node) {
    rs_mem *mm = &sim->memo[node.v];
    mm->size = (uint32_t)odin3_node_param(sim->mod, node, 1)->i;
    mm->abits = (uint32_t)odin3_node_param(sim->mod, node, 3)->i;
    mm->width = (uint32_t)odin3_node_param(sim->mod, node, 4)->i;
    mm->n_rd = (uint32_t)odin3_node_param(sim->mod, node, 5)->i;
    mm->n_wr = (uint32_t)odin3_node_param(sim->mod, node, 6)->i;
    TEST_ASSERT_EQUAL_INT64(0, odin3_node_param(sim->mod, node, 2)->i); /* OFFSET: not modelled */
    mm->mem = calloc((size_t)mm->size * mm->width, 1);
    mm->reg = calloc((size_t)mm->n_rd * mm->width + 1, 1);
    TEST_ASSERT_TRUE(mm->mem != NULL && mm->reg != NULL);
}

static void rs_init(rs *sim, const odin3_module *mod) {
    memset(sim, 0, sizeof *sim);
    sim->mod = mod;
    sim->n_nodes = odin3_module_node_end(mod);
    sim->n_pins = odin3_module_pin_end(mod);
    sim->val = calloc(odin3_module_net_end(mod), 1);
    sim->ffq = calloc(sim->n_pins + 1, 1);
    sim->kind = calloc(sim->n_nodes, sizeof *sim->kind);
    sim->ram = calloc(sim->n_nodes, sizeof *sim->ram);
    sim->memo = calloc(sim->n_nodes, sizeof *sim->memo);
    TEST_ASSERT_TRUE(sim->val != NULL && sim->ffq != NULL && sim->kind != NULL &&
                     sim->ram != NULL && sim->memo != NULL);
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        sim->kind[id] = kind_of(mod, (odin3_node_id){id});
        if (sim->kind[id] == K_SP || sim->kind[id] == K_DP) {
            setup_ram(sim, (odin3_node_id){id});
        } else if (sim->kind[id] == K_MEM) {
            setup_mem(sim, (odin3_node_id){id});
        }
    }
}

/* The initial bit of word `word` (the same for every bit of the word). */
static uint8_t preload_bit(uint32_t word) {
    uint32_t mixed = word * 2654435761U + 12345U;
    mixed ^= mixed >> 15;
    mixed *= 2246822519U;
    mixed ^= mixed >> 13;
    return (uint8_t)(mixed & 1U);
}

static void preload(rs *sim) {
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        uint8_t *cells = sim->kind[id] == K_MEM ? sim->memo[id].mem : sim->ram[id].mem;
        size_t width = sim->kind[id] == K_MEM ? sim->memo[id].width : sim->ram[id].data_bits;
        size_t words =
            sim->kind[id] == K_MEM ? sim->memo[id].size : (size_t)1 << sim->ram[id].addr_bits;
        for (size_t word = 0; cells != NULL && word < words; word++) {
            memset(cells + word * width, preload_bit((uint32_t)word), width);
        }
    }
}

static void rs_free(rs *sim) {
    for (uint32_t id = 1; id < sim->n_nodes; id++) {
        free(sim->ram[id].mem);
        free(sim->memo[id].mem);
        free(sim->memo[id].reg);
    }
    free(sim->val);
    free(sim->ffq);
    free(sim->kind);
    free(sim->ram);
    free(sim->memo);
}

void rs_trace(const odin3_module *module, const rs_io *io, rs_run run, odin3_strbuf *out) {
    rs sim;
    odin3_prng prng;
    rs_init(&sim, module);
    if (run.preload) {
        preload(&sim);
    }
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
