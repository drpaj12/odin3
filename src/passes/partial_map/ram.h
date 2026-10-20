/*
 * ram.h — internal to ram.c and ram_split.c (3C spec §6.1, R-1..R-7): the state of one explicit
 * RAM instance being legalised, and the glue of its depth split. Not part of hardmap's interface
 * (hm_kit.h has that).
 *
 * The depth split is a loop over levels (R-2): level L splits on address bit L, `h` (bit 0, the
 * low half) or `s` (bit 1). A leaf is numbered by its choices, bit L of the number being level L's.
 * There are 2^k leaves for k = ADDR_WIDTH - 15 levels. The write enable of a leaf is the instance's
 * `we` ANDed with the matching address bits or their inverses; each read port has one registered
 * select per level (R-7, D4); the output of a data bit is a tree of `$mux` whose root is level 0.
 */
#ifndef ODIN3_PASSES_HARDMAP_RAM_H
#define ODIN3_PASSES_HARDMAP_RAM_H

#include "passes/partial_map/hm_kit.h"

#include <stdint.h>

/* The arch's RAM shape (spec H-1: Parmys's split_memory_depth and split_memory_width). */
#define ODIN3_HARDMAP_RAM_ADDR 15U
/* An instance of more address bits is refused: 2^16 leaves per data bit is already beyond use. */
#define ODIN3_HARDMAP_RAM_MAX_ADDR 31U

enum {
    RAM_MAX_LEVELS = ODIN3_HARDMAP_RAM_MAX_ADDR - ODIN3_HARDMAP_RAM_ADDR,
    RAM_MAX_PORTS = 2,
    RAM_MAX_PINS = 9 /* ports of dual_port_ram */
};

/* Where the pins of `single_port_ram` / `dual_port_ram` are, per read/write port. */
typedef struct ram_layout {
    hm_hard kind;     /* HM_SP_RAM or HM_DP_RAM */
    uint32_t n_ports; /* read/write ports: 1 or 2 */
    uint32_t n_pins;  /* ports of the cell type */
    uint32_t clk;
    uint32_t data[RAM_MAX_PORTS];
    uint32_t addr[RAM_MAX_PORTS];
    uint32_t we[RAM_MAX_PORTS];
    uint32_t out[RAM_MAX_PORTS];
    const char *tag[RAM_MAX_PORTS]; /* in glue roles: "" or the port's number */
} ram_layout;

/* The pin layout of the RAM type of kind (HM_SP_RAM or HM_DP_RAM). */
const ram_layout *ram_layout_of(hm_hard kind);

typedef struct ram_port {
    odin3_net_id addr[ODIN3_HARDMAP_RAM_ADDR]; /* the leaf's address pins */
    odin3_net_id inv[RAM_MAX_LEVELS];          /* ~addr[L] */
    odin3_net_id sel[RAM_MAX_LEVELS];          /* the registered select of level L */
    odin3_vec we;                              /* odin3_net_id: the write enable of each leaf */
    odin3_vec out;                             /* odin3_net_id: each leaf's output, one data bit */
} ram_port;

/*
 * A RAM to build that is not an instance of the design: what mem.c synthesises from a `$mem`
 * (plan Task 6). pin[] is indexed by the cell type's port number (clk, data, addr, we, out; the
 * dual-port order of ram_layout), one netvec per port: addr as long as addr_bits, data and out as
 * long as data_bits, clk and we one net. An entry of none (or a short vec) leaves that pin open.
 * read[p] says whether the output of port p is used; if not, no select register and no output
 * tree are built for it and its out pins stay open.
 */
typedef struct hm_ram_desc {
    hm_hard kind;
    uint32_t addr_bits;
    uint32_t data_bits;
    bool read[RAM_MAX_PORTS];
    odin3_netvec pin[RAM_MAX_PINS];
} hm_ram_desc;

typedef struct ram_ctx {
    hm_ctx *hc;
    odin3_node_id src;
    const hm_ram_desc *desc; /* NULL: the pins are those of the instance src */
    bool read[RAM_MAX_PORTS];
    const ram_layout *lay;
    odin3_celltype_id type;
    uint32_t addr_bits; /* ADDR_WIDTH of the source */
    uint32_t data_bits; /* DATA_WIDTH of the source */
    uint32_t levels;    /* k: 0 when the address fits */
    odin3_vec *ties;
    ram_port port[RAM_MAX_PORTS];
} ram_ctx;

/* A pin of the source instance: pin port number (the cell type's port index) and bit. */
typedef struct ram_pin {
    uint32_t port;
    uint32_t bit;
} ram_pin;

/* The first `count` choices of a leaf, for names. */
typedef struct ram_choices {
    uint32_t leaf;
    uint32_t count;
} ram_choices;

/* A place in the split: read/write port (0, or 0 and 1 for dual port), data bit, level and leaf
 * (each used where it applies). */
typedef struct ram_at {
    uint32_t rwp;
    uint32_t bit;
    uint32_t level;
    uint32_t leaf;
} ram_at;

/*
 * Builds the slices of desc on fresh nets inside the transaction hm_begin_cell started (the
 * caller commits): the depth split, one RAM per data bit, the address pad. Appends one tie per
 * used output bit to ties and adds the number of RAMs made to *slices. Nothing of the design
 * changes; failure leaves what was made to hm_abort. The caller reserves nothing: ties grows here.
 */
odin3_status hm_build_ram(hm_ctx *hc, const hm_ram_desc *desc, odin3_vec *ties, uint64_t *slices);

/* Writes the choices at levels 0 .. count-1 as "h" / "s" letters, level 0 first. */
void ram_path(char *out, size_t size, ram_choices choices);

/* The source's net on a pin (none: open). */
odin3_net_id ram_src_net(const ram_ctx *rc, ram_pin pin);

/* Fills inv, sel and we of read/write port rwp: the registered selects and the write enable of
 * every leaf (R-2, R-5, R-7). With no depth split that is we[0] alone. */
odin3_status ram_split_glue(ram_ctx *rc, uint32_t rwp);

/* Reduces the leaf outputs of port at.rwp (rc->port[].out, 2^k entries, data bit at.bit) to one
 * net in *root. */
odin3_status ram_mux_tree(ram_ctx *rc, ram_at at, odin3_net_id *root);

#endif
