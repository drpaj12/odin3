/*
 * cut_int.h — the state shared by cut.c (records) and cutsig.c (signatures and the proof) of the
 * exact LUT cut (`abc --prov-cut on`).
 */
#ifndef ODIN3_PASSES_ABC_CUT_INT_H
#define ODIN3_PASSES_ABC_CUT_INT_H

#include "ir/module.h"
#include "passes/abc/abc.h"
#include "passes/abc/aig.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* Why a LUT is approximate, in the order the stages are tried. */
enum { CUT_WHY_INPUT = 0, CUT_WHY_ROOT, CUT_WHY_WALK, CUT_WHY_PROOF, CUT_WHY_N };

enum {
    CUT_SIG_WORDS = 4,       /* 256 random patterns per signature */
    CUT_FIRST_OBJ = 2,       /* the first result object after the two constants */
    CUT_MAX_CANDS = 4,       /* candidate nets kept per LUT input, and root nets tried per LUT */
    CUT_WALK_CAP = 4096,     /* gates one walk may visit before the LUT is called approximate */
    CUT_PROVE_MAX_K = 12,    /* the proof evaluates 2^k patterns; wider LUTs stay approximate */
    CUT_PROVE_WORDS_MAX = 64 /* 2^(12-6) words of patterns */
};

/* A net whose function is a known AIG literal, found by its canonical signature. */
typedef struct cut_cand {
    uint64_t hash;
    uint32_t net;
    uint32_t inv; /* 1: the net's signature is the complement of the canonical one */
} cut_cand;

/* One candidate net for a LUT input or output: the net and whether it is the complement. */
typedef struct cut_net {
    uint32_t net;
    uint32_t inv;
} cut_net;

/* A leaf net of the LUT being worked on: input v, and whether the net is its complement. */
typedef struct cut_leaf {
    uint32_t net;
    uint32_t v;
    uint32_t inv;
} cut_leaf;

typedef struct cut {
    odin3_abc_run *run;
    const odin3_abc_result *res;
    const odin3_module *mod;
    odin3_abc_aigb aig;
    odin3_vec asig;   /* uint64_t: CUT_SIG_WORDS per AIG object */
    odin3_vec osig;   /* uint64_t: CUT_SIG_WORDS per result object */
    odin3_vec table;  /* cut_cand, sorted by (hash, net) */
    odin3_vec leaf;   /* uint32_t per net: 1 + the LUT it is a leaf of */
    odin3_vec lvar;   /* uint32_t per net: the LUT input (+ 1) it stands for */
    odin3_vec linv;   /* uint32_t per net: 1 when it is the complement of that input */
    odin3_vec seen;   /* uint32_t per node: 1 + the LUT that visited it */
    odin3_vec stack;  /* uint32_t */
    odin3_vec gates;  /* uint32_t: nodes of the cut found so far */
    odin3_vec vstamp; /* uint32_t per AIG object: proof generation */
    odin3_vec voff;   /* uint32_t per AIG object: its offset in vpool */
    odin3_vec vpool;  /* uint64_t: values of the visited AIG objects */
    odin3_vec leafs;  /* cut_leaf: the candidate nets of the LUT's inputs, for the proof */
    uint32_t cur;     /* the LUT being worked on */
    uint32_t gen;     /* proof generation counter */
    uint32_t nw;      /* 64-bit pattern words of the proof in progress */
    uint32_t exact, approx;
    uint32_t why;             /* why the LUT in hand is not exact: a CUT_WHY_* */
    uint32_t miss[CUT_WHY_N]; /* approximate LUTs per reason */
} cut;

/* Signatures: builds the AIG, simulates it, signs the result objects and the candidate table. */
odin3_status cut_sigs_build(cut *ct);
void cut_free(cut *ct);

/* The candidate nets (at most CUT_MAX_CANDS) whose function equals that of result object obj, up to
 * complement, into out (cut_net, cleared first). */
odin3_status cut_candidates(const cut *ct, uint32_t obj, odin3_vec *out);

/* True when the AIG function of net root, over the inputs marked as leaves of LUT lut (cut.leaf,
 * lvar, linv), is the LUT's truth table (complemented when root_inv). */
bool cut_prove(cut *ct, uint32_t lut, const cut_net *root);

#endif
