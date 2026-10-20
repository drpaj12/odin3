/*
 * index.h — the per-module net index of the fsm pass (4.8 spec section 4): Yosys's sig2driver and
 * sig2user (fsm_detect.cc, fsm_extract.cc) and its SigMap, on the IR's nets.
 *
 * Signals. IR nets are already merged, so a net is its own canonical bit (the SigMap is the
 * identity). A signal bit is an odin3_fsm_sig: a net ID, or a constant coded as 0xFFFFFFF0 plus an
 * odin3_fsm_bit (so 0, 1, x, z, and the table's `-` and `m` marks). Two nets that carry the same
 * constant are the same signal; an unconnected input pin reads as x. A signal is an array of bits,
 * LSB first.
 *
 * References. Every connected pin of every live node but the port nodes ($port_in, $port_out,
 * $port_inout) and the constant nodes ($_CONST*_) is a reference of its net, with the pin's port
 * index and flags: IN (the pin reads), OUT (it drives; an inout pin has both) and UNKNOWN (the
 * node is not an internal cell of Yosys: an instance of a module, a black box or a hard cell, which
 * fsm_detect treats as driver and user of every pin, fsm_detect.cc:322, :327) and SEQ (a flip-flop,
 * latch or memory cell, which Yosys's ConstEval does not know, consteval.h:42: setup_internals has
 * the combinational cells only). A net on a port
 * node's pin is a port net (sig_at_port).
 *
 * Failures. ODIN3_ERR_NO_MEMORY with the index empty; nothing logs.
 */
#ifndef ODIN3_PASSES_FSM_INDEX_H
#define ODIN3_PASSES_FSM_INDEX_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsmdata.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t odin3_fsm_sig;

/* A signal: an array of bits and its length. */
typedef struct odin3_fsm_sigref {
    const odin3_fsm_sig *sig;
    uint32_t len;
} odin3_fsm_sigref;

#define ODIN3_FSM_SIG_CONST_BASE 0xFFFFFFF0U
enum { ODIN3_FSM_NO_CONST = 0xFF };

static inline bool odin3_fsm_sig_is_const(odin3_fsm_sig sig) {
    return sig >= ODIN3_FSM_SIG_CONST_BASE;
}
/* The odin3_fsm_bit a constant signal bit stands for; requires odin3_fsm_sig_is_const. */
static inline uint8_t odin3_fsm_sig_bit(odin3_fsm_sig sig) {
    return (uint8_t)(sig - ODIN3_FSM_SIG_CONST_BASE);
}
static inline odin3_fsm_sig odin3_fsm_sig_of_bit(uint8_t bit) {
    return ODIN3_FSM_SIG_CONST_BASE + bit;
}
/* True for a constant that is 0 or 1. */
static inline bool odin3_fsm_sig_defined(odin3_fsm_sig sig) {
    return odin3_fsm_sig_is_const(sig) && odin3_fsm_bit_defined(odin3_fsm_sig_bit(sig));
}

enum {
    ODIN3_FSM_REF_IN = 1,
    ODIN3_FSM_REF_OUT = 2,
    ODIN3_FSM_REF_UNKNOWN = 4,
    ODIN3_FSM_REF_SEQ = 8,
    ODIN3_FSM_PAIRS_STRICT = 0x100 /* odin3_fsm_ix_pairs: unknown nodes count by direction only */
};

typedef struct odin3_fsm_ref {
    uint32_t node;  /* odin3_node_id */
    uint32_t port;  /* port index of the pin */
    uint32_t flags; /* ODIN3_FSM_REF_* */
} odin3_fsm_ref;

typedef struct odin3_fsm_refs {
    const odin3_fsm_ref *refs;
    uint32_t count;
} odin3_fsm_refs;

/* Cell types the pass reads, by the registry's names (0: the design has none). */
typedef struct odin3_fsm_types {
    uint32_t dff, adff, mux, pmux, logic_not, ne, eq, reduce_or, reduce_bool, reduce_and;
} odin3_fsm_types;

/* Port indices of the registry cells (fixed by the cell tables in src/ir/cells). */
enum {
    ODIN3_FSM_P_DFF_CLK = 0,
    ODIN3_FSM_P_DFF_D = 1,
    ODIN3_FSM_P_DFF_Q = 2,
    ODIN3_FSM_P_ADFF_CLK = 0,
    ODIN3_FSM_P_ADFF_ARST = 1,
    ODIN3_FSM_P_ADFF_D = 2,
    ODIN3_FSM_P_ADFF_Q = 3,
    ODIN3_FSM_P_MUX_A = 0,
    ODIN3_FSM_P_MUX_B = 1,
    ODIN3_FSM_P_MUX_S = 2,
    ODIN3_FSM_P_MUX_Y = 3,
    ODIN3_FSM_P_UN_A = 0,
    ODIN3_FSM_P_UN_Y = 1,
    ODIN3_FSM_P_BIN_A = 0,
    ODIN3_FSM_P_BIN_B = 1,
    ODIN3_FSM_P_BIN_Y = 2
};

typedef struct odin3_fsm_index {
    odin3_design *design;
    odin3_module *module;
    uint32_t net_end, node_end;
    uint32_t *off;       /* net_end + 1 offsets into refs */
    odin3_fsm_ref *refs; /* grouped by net, in node ID then port order */
    uint8_t *net_const;  /* per net: an odin3_fsm_bit (0, 1, x, z) or ODIN3_FSM_NO_CONST */
    uint8_t *net_port;   /* per net: 1 on a port node's pin */
    odin3_fsm_types types;
} odin3_fsm_index;

/* Builds the index of module; *ix is zeroed on failure. */
odin3_status odin3_fsm_index_build(odin3_design *design, odin3_module *module, odin3_fsm_index *ix);

/* Frees the index; safe on a zeroed one. */
void odin3_fsm_index_free(odin3_fsm_index *ix);

/* The references of net (an empty span for the constant nets and the unknown ones). */
odin3_fsm_refs odin3_fsm_ix_refs(const odin3_fsm_index *ix, uint32_t net);

/* True when net is on a port node's pin. */
bool odin3_fsm_ix_port_net(const odin3_fsm_index *ix, uint32_t net);

/* The signal bit of net: its constant when the net carries one, else the net itself. */
odin3_fsm_sig odin3_fsm_ix_sig(const odin3_fsm_index *ix, uint32_t net);

/* The signal bit of pin: x for an unconnected pin. */
odin3_fsm_sig odin3_fsm_ix_pin_sig(const odin3_fsm_index *ix, odin3_pin_id pin);

/* Appends the signal bits of port `port` of node to out (elements odin3_fsm_sig), LSB first. */
odin3_status odin3_fsm_ix_port_sigs(const odin3_fsm_index *ix, odin3_node_id node, uint32_t port,
                                    odin3_vec *out);

/*
 * Appends to vec (elements uint32_t) the (node, port) pairs that drive (want = ODIN3_FSM_REF_OUT)
 * or read (ODIN3_FSM_REF_IN) a bit of sigs, node then port, sorted and without repeats: Yosys's
 * sig2driver.find and sig2user.find. An unknown node counts for both, unless want also has
 * ODIN3_FSM_PAIRS_STRICT (fsm_extract, whose CellTypes knows the design's modules). *count gets the
 * number of pairs added. Constant bits have none. NO_MEMORY with vec as it was.
 */
odin3_status odin3_fsm_ix_pairs(const odin3_fsm_index *ix, odin3_fsm_sigref sigs, uint32_t want,
                                odin3_vec *vec, uint32_t *count);

/* Pair idx of a vec filled by odin3_fsm_ix_pairs starting at element `first`. */
static inline const uint32_t *odin3_fsm_pair_at(const odin3_vec *vec, size_t first, uint32_t idx) {
    return (const uint32_t *)vec->data + first + (size_t)idx * 2U;
}

/* Yosys's SigBit order: constants first (by value), then nets by ID. <0, 0 or >0. */
int odin3_fsm_sig_cmp(odin3_fsm_sig lhs, odin3_fsm_sig rhs);

/* Sorts a vec of signal bits in that order and drops the repeats (SigSpec::sort_and_unify). */
void odin3_fsm_sigvec_sort_unify(odin3_vec *vec);

/* True when bit is in the vec (linear). */
bool odin3_fsm_sigvec_has(const odin3_vec *vec, odin3_fsm_sig bit);

/* The type ID of node (0 for a dead node). */
uint32_t odin3_fsm_ix_type(const odin3_fsm_index *ix, uint32_t node);

#endif
