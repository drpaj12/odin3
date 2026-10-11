/*
 * pw.h — the working form of a process (2E spec §3, PR-5; §4, PR-6): a mutable C mirror of Yosys's
 * RTLIL::Process that every phase of proc's step A edits, decoded once from a $proc node's BODY
 * and re-encoded by --stop-after. Signals are runs of odin3_pw_bit in one append-only bits vector;
 * cases, switches and syncs are index forests, so no walk needs recursion.
 */
#ifndef ODIN3_PASSES_PROC_PW_H
#define ODIN3_PASSES_PROC_PW_H

#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* One module's lowering state (proc_internal.h). */
typedef struct odin3_proc_mctx odin3_proc_mctx;

/*
 * A signal bit: a module-local net ID, or a constant (RTLIL's State) when the top bit is set.
 * §3 writes the constants as enumerators; C17 enumerators must fit an int and 1U << 31 does not,
 * so they are macros with §3's values (as ir/proc.h does for ODIN3_PBIT_*).
 */
typedef uint32_t odin3_pw_bit;
#define ODIN3_PW_CONST (1U << 31)
#define ODIN3_PW_S0 (ODIN3_PW_CONST | 0U) /* 0 */
#define ODIN3_PW_S1 (ODIN3_PW_CONST | 1U) /* 1 */
#define ODIN3_PW_SX (ODIN3_PW_CONST | 2U) /* x */
#define ODIN3_PW_SZ (ODIN3_PW_CONST | 3U) /* z */
#define ODIN3_PW_SA (ODIN3_PW_CONST | 4U) /* '-' (RTLIL Sa): any value; compare patterns only */
#define ODIN3_PW_SM (ODIN3_PW_CONST | 5U) /* marked (RTLIL Sm): arst's "not known yet" */

typedef struct odin3_pw_sig {
    uint32_t first, len;
} odin3_pw_sig; /* run in pw->bits */

typedef struct odin3_pw_action {
    odin3_pw_sig lhs, rhs;
    odin3_prov_id prov;
} odin3_pw_action;

typedef struct odin3_pw_case {
    odin3_vec compares; /* odin3_pw_sig, each the switch signal's width; empty = default */
    odin3_vec actions;  /* odin3_pw_action, in order */
    odin3_vec switches; /* uint32_t indices into pw->switches, in order */
    odin3_prov_id prov;
} odin3_pw_case;

typedef struct odin3_pw_switch {
    odin3_pw_sig signal; /* may become empty (len 0): arst and clean clear it */
    odin3_vec cases;     /* uint32_t indices into pw->cases, in order */
    uint16_t flags;      /* ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE (2D) */
    odin3_prov_id prov;
} odin3_pw_switch;

typedef struct odin3_pw_sync {
    uint8_t type;        /* odin3_sync_type (EL-33); arst turns edges into LEVEL0/LEVEL1 */
    odin3_pw_bit signal; /* 0 for ALWAYS */
    odin3_vec actions;   /* odin3_pw_action */
} odin3_pw_sync;

/* One attribute of the decoded node; the value stays in the module's storage until compact. */
typedef struct odin3_pw_attr {
    uint32_t key;
    const odin3_value *value;
} odin3_pw_attr;

typedef struct odin3_pw {
    odin3_node_id node;    /* the $proc node, until step B deletes it */
    odin3_prov_id prov;    /* the node's SOURCE record (EL-45: its ALWAYS) */
    odin3_vec bits;        /* odin3_pw_bit: storage of every sig, append-only */
    odin3_vec cases;       /* odin3_pw_case; [0] is the root; removed cases are unlinked */
    odin3_vec switches;    /* odin3_pw_switch; removed switches are unlinked */
    odin3_vec syncs;       /* odin3_pw_sync, in order */
    odin3_vec promoted;    /* odin3_pw_action: prune's root promotions, in promotion order */
    odin3_vec in_nets;     /* odin3_net_id: the nets of the I and O pins at decode (cleanup) */
    odin3_vec attrs;       /* odin3_pw_attr: the node's attributes in first-set order (point (i)) */
    odin3_prov_id lowered; /* step B's DERIVED record (point (i)); 0 until step B */
} odin3_pw;

/*
 * Decodes $proc node `node` of mctx's module into *out (§4): each body position becomes a bit (IN |
 * k the net on I pin k, OUT | k the net on O pin k, ANY ODIN3_PW_SA), and a net whose
 * odin3_net_const_value is 0, 1, x or z becomes ODIN3_PW_S0, _S1, _SX or _SZ (a sync signal
 * included). The body's positions are copied in order (sig.first is the position's index), cases,
 * switches, actions and syncs keep their indices, provenance and flags; in_nets gets the nets of
 * the I pins, then of the O pins, in pin order; attrs gets the node's attributes; prov the node's
 * record, lowered 0, promoted empty. *out is overwritten (not freed first) and owned by the caller
 * (odin3_pw_free), also on failure, when it is left empty. ODIN3_ERR_INVALID_ARG (logged) when the
 * node is not a live $proc, its BODY does not view or verify, or a position is on an unconnected
 * pin; ODIN3_ERR_NO_MEMORY. Costs O(body + pins + attributes).
 */
odin3_status odin3_pw_decode(const odin3_proc_mctx *mctx, odin3_node_id node, odin3_pw *out);

/*
 * Re-encodes *pw as a new $proc node (§17; --stop-after and proc_build): the body in 2D point (e)'s
 * canonical layout (cases and switches numbered breadth-first from the root, actions contiguous
 * per case in case order then per sync, compares contiguous per case); its positions laid out in
 * the order of their runs in pw->bits, so decode then encode gives the same BODY bytes; O = every
 * net that is an lhs bit and I = every other net read plus the shared constant net of each
 * constant bit (odin3_proc_const_net), both in net-ID order; an O net read becomes OUT | k, and
 * ODIN3_PW_SA becomes ANY. Only cases, switches and syncs reachable from the root are encoded.
 * The new node takes pw->prov and pw->attrs; it is created before pw->node, when live, is
 * deleted, so a failure leaves the old node (a shared constant node encode had to create may
 * remain); on success pw->node is the new node. ODIN3_ERR_INVALID_ARG (logged) for a bit that is
 * not a live net of the module, an lhs bit that is a constant, ODIN3_PW_SM, a case or switch
 * reached twice, or a body odin3_proc_body_verify rejects; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_encode(odin3_proc_mctx *mctx, odin3_pw *pw);

/* Frees every vector of *pw (each case's, switch's and sync's too) and empties it. NULL: no-op. */
void odin3_pw_free(odin3_pw *pw);

/*
 * The sort key of a bit (§4, "Bit order"): a constant sorts first by its state
 * (bit & ~ODIN3_PW_CONST); a net with a primary (wire w, bit b) is (uint64_t)w << 32 | b; any
 * other net (uint64_t)UINT32_MAX << 32 | net. Distinct live nets have distinct keys.
 */
uint64_t odin3_proc_bit_key(const odin3_module *module, odin3_pw_bit bit);

/* --- signal operations (pw_sig.c): Yosys's SigSpec operations, each O(length) ------------- */

/*
 * Appends a run holding a copy of bits[0 .. len) (bits may point into pw->bits) and sets *out to
 * it. ODIN3_ERR_NO_MEMORY with pw unchanged.
 */
odin3_status odin3_pw_sig_new(odin3_pw *pw, const odin3_pw_bit *bits, uint32_t len,
                              odin3_pw_sig *out);

/* The bits of sig, valid until the next append to pw->bits. */
const odin3_pw_bit *odin3_pw_sig_bits(const odin3_pw *pw, odin3_pw_sig sig);

/* Yosys sig.replace(pattern, with, &other). */
typedef struct odin3_pw_replace {
    odin3_pw_sig sig, pattern, with, other;
} odin3_pw_replace;

/*
 * *out = a new run: other, with position i set to with[j] wherever sig[i] is a net equal to
 * pattern[j] (the first such j; constant pattern bits match nothing). ODIN3_ERR_INVALID_ARG
 * (logged) when sig and other, or pattern and with, differ in length; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_sig_replace(odin3_proc_mctx *mctx, odin3_pw *pw, const odin3_pw_replace *req,
                                  odin3_pw_sig *out);

typedef struct odin3_pw_pair {
    odin3_pw_sig a, b;
} odin3_pw_pair;

/*
 * Yosys in.a.remove2(pattern, &in.b): out->a and out->b are new runs holding the positions of
 * in.a (and in.b) whose in.a bit is not a net of pattern; constant bits are never removed. in.b
 * may be empty (no other: out->b empty). ODIN3_ERR_INVALID_ARG (logged) when in.b is neither empty
 * nor in.a's length; ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_sig_remove2(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                  odin3_pw_sig pattern, odin3_pw_pair *out);

/*
 * Yosys in.a.extract(pattern, &in.b), in in.a's position order: *out = a new run of in.b[i] (or
 * in.a[i] when in.b is empty) for each position i whose in.a bit is a net of pattern.
 * ODIN3_ERR_INVALID_ARG (logged) when in.b is neither empty nor in.a's length; NO_MEMORY.
 */
odin3_status odin3_pw_sig_extract(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                  odin3_pw_sig pattern, odin3_pw_sig *out);

/* Bit by bit, as SigSpec::operator==. */
bool odin3_pw_sig_equal(const odin3_pw *pw, odin3_pw_sig lhs, odin3_pw_sig rhs);

/* No net bits. */
bool odin3_pw_sig_is_fully_const(const odin3_pw *pw, odin3_pw_sig sig);
/* Only ODIN3_PW_S0 and ODIN3_PW_S1. */
bool odin3_pw_sig_is_fully_def(const odin3_pw *pw, odin3_pw_sig sig);
/* Only ODIN3_PW_SX and ODIN3_PW_SZ. */
bool odin3_pw_sig_is_fully_undef(const odin3_pw *pw, odin3_pw_sig sig);
/* Some bit is ODIN3_PW_S1. */
bool odin3_pw_sig_as_bool(const odin3_pw *pw, odin3_pw_sig sig);

/*
 * Yosys sort_and_unify: *out = a new run of s's distinct bits sorted by odin3_proc_bit_key.
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_sig_sort_unify(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_sig sig,
                                     odin3_pw_sig *out);

#endif
