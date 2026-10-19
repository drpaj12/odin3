/*
 * opt.h — the opt family (2F). Created by 2E Task 3 (plan point (c), ruling I2) with the two
 * types proc's statistics need and the declaration of odin3_opt_expr_run, proc's step F
 * (2E spec §2.2; 2F spec §4.8). Those three are frozen: 2F Task 1 appends below them and 2F Task 6
 * defines odin3_opt_expr_run in expr.c without changing the declaration. Nothing in 2E calls it
 * until point (f)'s pre-merge step, so 2E links without 2F.
 */
#ifndef ODIN3_PASSES_OPT_OPT_H
#define ODIN3_PASSES_OPT_OPT_H

#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* One flag per option of the opt passes (2F spec §3), spelled as Yosys's. */
typedef struct odin3_opt_opts {
    bool mux_undef, mux_bool, undriven, noclkinv, fine, full, keepdc; /* opt_expr */
    bool nodffe, nosdff;                                              /* opt_dff */
    bool purge;                                                       /* opt_clean */
    bool nomux, ffbits;                                               /* opt_merge */
    bool fast, noff;                                                  /* opt */
    bool memx;                                                        /* wreduce (3G) */
} odin3_opt_opts;

enum { ODIN3_OPT_EXPR_MAX_RULES = 128 };

/* opt_expr's counts (2F spec §16); callers zero them, odin3_opt_expr_run adds into them. */
typedef struct odin3_opt_expr_counts {
    uint32_t rewritten, folded;
    uint32_t rule[ODIN3_OPT_EXPR_MAX_RULES]; /* indexed by 2F's odin3_opt_rule */
} odin3_opt_expr_counts;

/* opt_expr on one module (2F §4.8), defined by 2F Task 6; adds into *counts. */
odin3_status odin3_opt_expr_run(odin3_pass_ctx *ctx, odin3_module *module,
                                const odin3_opt_opts *opts, odin3_opt_expr_counts *counts);

/* --- 2F Task 1 (appended below 2E-1's frozen types) ---------------------------------------- */

/* opt_dff's per-action counters (plan point (k)): at most this many odin3_opt_dff_action values. */
enum { ODIN3_OPT_DFF_MAX_ACTIONS = 32 };

/*
 * The caps (2F plan, Global Constraints): opt's iterations per invocation (OP-12), opt_muxtree's
 * eval_mux calls per module (point (r)), and a budget check every 2^16 pins the kit creates
 * (odin3_kit_new_cell; the budget itself is 2E-1's odin3_budget_check). The first two are lowered
 * by tests through odin3_opt_test_set_limits (opt_test.h).
 */
enum {
    ODIN3_OPT_MAX_ITERATIONS = 10000,
    ODIN3_OPT_MUXTREE_MAX_EVALS = 100000,
    ODIN3_OPT_BUDGET_PINS = 1 << 16
};

/* 3G wreduce: at most this many odin3_wreduce_rule counters (wreduce.h has the rules). */
enum { ODIN3_WREDUCE_MAX_RULES = 32 };

/* wreduce's counts (3G WR-11): the INFO line's totals and one counter per rule. */
typedef struct odin3_wreduce_counts {
    uint32_t narrowed, bits, removed, to_signed, to_unsigned;
    uint32_t rule[ODIN3_WREDUCE_MAX_RULES];
} odin3_wreduce_counts;

/* The foldable types (spec §4's `const.$<type>` list; fold.h): one counter each. */
enum { ODIN3_OPT_FOLD_TYPES = 32 };

/*
 * Every sub-pass's counters in one aggregate (plan point (b)): opt_expr's frozen counts, then one
 * counter per §16 info line and opt_dff's per-action counters (indexed by odin3_opt_dff_action).
 * Callers zero it; each sub-pass call adds into it, and --opt-stats prints from it.
 */
typedef struct odin3_opt_counts {
    odin3_opt_expr_counts expr;
    uint32_t merge_removed, muxtree_ports, reduce_changes, share_merged, dff_changed,
        dff_const_bits;
    uint32_t dff_action[ODIN3_OPT_DFF_MAX_ACTIONS];
    uint32_t clean_cells, clean_nets, clean_wires, iterations;
    /*
     * 2F Task 3: opt_clean's did_something events (buffers wired, non-constant nodes removed, wires
     * deleted). Beyond the briefed fields: odin3_opt_clean_run has no changed
     * output, and those events are not all in clean_cells/clean_wires (Yosys counts neither the
     * buffers, and constants have no Yosys counterpart); the loop reads this one.
     */
    uint32_t clean_changes;
    /*
     * 2F Task 5: folds by cell type (fold.h's odin3_opt_fold_name: "const.$<type>"), the 32
     * foldable types in the order of fold.c's table. Beyond the briefed fields: Task 6's rule
     * names print them.
     */
    uint32_t fold_type[ODIN3_OPT_FOLD_TYPES];
    odin3_wreduce_counts wreduce; /* 3G */
} odin3_opt_counts;

/* --- 2F Task 3: opt_clean (OP-10) ----------------------------------------------------------- */

/*
 * opt_clean (2F spec §10, Yosys rmunused_module) over every module of design, in creation order,
 * except those with live $proc nodes (INFO "opt_clean: module 'm' has processes; skipped"). Per
 * module: `$_BUF_` nodes without `keep` become wiring; liveness spreads back from the module's
 * ports, `keep` wires and `keep` nodes (instances of `keep` modules included); dead nodes are
 * deleted in ID order (a dead FF's Q init bits go with it); init is normalised as Yosys 0.55 does,
 * silently and without a change: an `init` on a net that was constant-driven (decided before the
 * dead `$_CONST*_` node goes) is removed whatever its value, and a net with more than one defined
 * `init` membership keeps one (primary first); wires whose nets have no pins are deleted, except
 * that without opts->purge a public wire stays when one of its nets has another wire membership or
 * was constant-driven (with opts->purge, public wires that are not the primary name of a net with
 * pins go; ports, `keep` and initialised wires stay); nets left with no pins, wire or aliases are
 * deleted. Dead `$_CONST*_` nodes have no Yosys counterpart: they are removed silently, uncounted
 * and without a change. It adds into counts (clean_cells, clean_nets, clean_wires, clean_changes)
 * and logs the INFO line "opt_clean: removed N unused cells, M unused nets and W unused wires"
 * (this call's totals). ODIN3_ERR_INVALID_ARG (logged) for a NULL argument; ODIN3_ERR_NO_MEMORY,
 * possibly after some modules were cleaned (the IR stays valid).
 */
odin3_status odin3_opt_clean_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts);

/*
 * Yosys's keep_cache: keep is cleared and receives one bool per module ID (index 0 unused, so
 * odin3_design_module_end entries; the vec's element size must be sizeof(bool), else INVALID_ARG):
 * true when the module has `keep`, contains a node or wire with `keep`, or (iteratively) contains
 * an instance of a module for which it is true. NO_MEMORY (keep then empty).
 */
odin3_status odin3_opt_keep_modules(odin3_design *design, odin3_vec *keep);

/* --- 2F Task 8: opt_merge (OP-6) ------------------------------------------------------------ */

/*
 * opt_merge (2F spec §6, Yosys opt_merge.cc) over every module of design: nodes with the same
 * type, parameters and (normalised) input operands, and for FFs the same Q init, are merged into
 * the earlier one (the later when only it has `keep`); the sweeps repeat until one merges nothing.
 * Candidates are word and bit nodes except $mem, $tribuf, $_TBUF_, $proc, constants, nodes without
 * exactly one output port, $mux/$pmux under opts->nomux, and (opts->keepdc) FFs whose init is not
 * fully defined. With opts->ffbits the bits of word FFs are merged too (ffbits.c, PHASE3 #48).
 * Each class is then closed with one rebuild of its survivor whose DERIVED record
 * has the parents [survivor, drops in ID order]. Adds the drops to counts->merge_removed; logs
 * INFO "opt_merge: removed N cells" and a DEBUG line per drop. INVALID_ARG (logged) for a NULL
 * argument; NO_MEMORY, possibly after some modules were merged (the IR stays valid; the survivors
 * of the classes dropped so far then keep no DERIVED provenance parents, because the classes are
 * only closed after the sweeps).
 */
odin3_status odin3_opt_merge_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 2F Task 9: opt_reduce (OP-8) ----------------------------------------------------------- */

/*
 * opt_reduce (2F spec §8, Yosys opt_reduce.cc) over every module of design. Per module, until a
 * worker run changes nothing: $reduce_or then $reduce_and (inputs sorted by net ID and
 * de-duplicated, an absorbing constant makes the input that constant alone, neutral constants go,
 * inputs driven by a reduction of the same type are replaced by that reduction's inputs, an empty
 * input becomes the neutral constant; the child is processed first, through an explicit stack, and
 * stays for opt_clean), then for each $mux and $pmux: with opts->fine, or when its Y feeds a $mem
 * WR_EN (Yosys's mem_wren_sigs), opt_mux_bits (Y bits whose input tuples are equal share one bit,
 * bits whose tuple is one repeated value become that value), then opt_pmux (B slices equal to A
 * are dropped, equal slices are grouped by a hash of their operand vectors in first-occurrence
 * order and their S bits ORed by a new $reduce_or; no slice left: Y = A; one left: a $mux). Adds
 * the changes to counts->reduce_changes; logs INFO "opt_reduce: performed N changes". A cell with
 * an unconnected input pin is left alone. INVALID_ARG (logged) for a NULL argument; NO_MEMORY,
 * possibly after some rewrites (the IR stays valid; each rewrite builds before its first
 * destructive step).
 */
odin3_status odin3_opt_reduce_run(odin3_pass_ctx *ctx, odin3_design *design,
                                  const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 2F Task 10: opt_muxtree (OP-7) --------------------------------------------------------- */

/*
 * opt_muxtree (2F spec §7, Yosys opt_muxtree.cc) over every module of design, in creation order,
 * except those with live $proc nodes (INFO "opt_muxtree: module 'm' has processes; skipped"). Per
 * module: the $mux and $pmux nodes are indexed (a root is a mux with a Y bit that a non-mux node,
 * a mux select, a $port_out or a `keep` wire uses, or that more than one mux reads); each root is
 * evaluated with reference-counted knowledge of active and inactive selects, a non-pure root met
 * below another root in place up to three levels deep and otherwise re-queued, an explicit frame
 * stack standing for the recursion; ports never enabled are dead and are removed (one live port:
 * Y connected to its data; several: a narrower $pmux or a $mux; none: the node is deleted), and
 * data bits known to be 0 or 1 under the selects of the enclosing ports become constants. After
 * ODIN3_OPT_MUXTREE_MAX_EVALS evaluations in a module the analysis stops, WARN "opt_muxtree: module
 * 'm': giving up after N evaluations", and that module is not changed. Adds the dead ports to
 * counts->muxtree_ports; logs INFO "opt_muxtree: removed N multiplexer ports" and a DEBUG line per
 * dead port. INVALID_ARG (logged) for a NULL argument; NO_MEMORY, possibly after some rewrites (the
 * IR stays valid; each rewrite builds before its first destructive step).
 */
odin3_status odin3_opt_muxtree_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 2F Task 14: opt_share (OP-11) ---------------------------------------------------------- */

/*
 * opt_share (2F spec §11, Yosys opt_share.cc) over every module of design, in creation order. Per
 * $mux and $pmux (node-ID order, the nodes the call starts with): a data port is a candidate when
 * one binary word cell (the $add/$sub class, $mul, $div, $mod, $and, $or, $xor, $xnor, the shifts,
 * the comparisons, $logic_and, $logic_or) drives all of it, bit for bit, with its whole Y, nothing
 * else reads those Y nets (no other pin, no port, no `keep` wire) and the cell has no `keep`. The
 * candidates are visited in port order (B slices, then A); from the first one still open, each of
 * its two operands is tried (the one with the lower net-ID vector first), and the open candidates
 * whose cell has the same operand on the same port and the same parameters apart from the other
 * operand's width (an $add and a $sub also mix, on port A) form a group of two or more. A group
 * becomes one new cell of the lowest-ID member's type on the shared operand, fed by a new
 * $mux/$pmux over the other operands (an $add/$sub mix negates the $sub operands with a $neg; a
 * narrower operand is extended by its sign flag), and the old cells are deleted with their Y nets
 * merged into the new cell's; the old mux stays (its group ports now read one net). The new cell,
 * the mux and the $neg cells share one DERIVED record whose parents are the old cells in ID order,
 * then the old mux. Adds the cells merged to counts->share_merged; logs INFO "opt_share: merged N
 * cells". INVALID_ARG (logged) for a NULL argument; NO_MEMORY, possibly after some rewrites (the
 * IR stays valid; each rewrite builds before its first destructive step).
 */
odin3_status odin3_opt_share_run(odin3_pass_ctx *ctx, odin3_design *design,
                                 const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 2F Task 11: opt_dff (OP-9) ------------------------------------------------------------- */

/*
 * opt_dff's per-action counters (plan point (k)), indexed into odin3_opt_counts.dff_action and
 * named for --opt-stats (register.c's k_dff_names: "opt_dff.<name>"). The first seven families
 * are spec section 16's <action> patterns with one counter per port; the conversions and the AD = Q
 * rule have no pattern there and carry their own text (agent default). ADD_SRST and ADD_EN are
 * Task 12's (rules 10 and 11); the three CONST_BIT counters are constbits' "setting constant <v>
 * at bit K", one per value.
 */
typedef enum odin3_opt_dff_action {
    ODIN3_OPT_DFF_NEVER_CLR,
    ODIN3_OPT_DFF_NEVER_SET,
    ODIN3_OPT_DFF_ALWAYS_CLR,
    ODIN3_OPT_DFF_ALWAYS_SET,
    ODIN3_OPT_DFF_CLR_SET_TO_ARST,
    ODIN3_OPT_DFF_NEVER_ALOAD,
    ODIN3_OPT_DFF_ALWAYS_ALOAD,
    ODIN3_OPT_DFF_ALOAD_TO_ARST,
    ODIN3_OPT_DFF_NEVER_ARST,
    ODIN3_OPT_DFF_ALWAYS_ARST,
    ODIN3_OPT_DFF_NEVER_SRST,
    ODIN3_OPT_DFF_ALWAYS_SRST,
    ODIN3_OPT_DFF_NEVER_EN,
    ODIN3_OPT_DFF_ALWAYS_EN,
    ODIN3_OPT_DFF_CONST_CLK,
    ODIN3_OPT_DFF_D_EQ_Q,
    ODIN3_OPT_DFF_AD_EQ_Q,
    ODIN3_OPT_DFF_ADD_SRST,
    ODIN3_OPT_DFF_ADD_EN,
    ODIN3_OPT_DFF_CONST_BIT_0,
    ODIN3_OPT_DFF_CONST_BIT_1,
    ODIN3_OPT_DFF_CONST_BIT_X,
    ODIN3_OPT_DFF_CONST_BIT_Z,
    ODIN3_OPT_DFF_N_ACTIONS
} odin3_opt_dff_action;

_Static_assert((uint32_t)ODIN3_OPT_DFF_N_ACTIONS <= (uint32_t)ODIN3_OPT_DFF_MAX_ACTIONS,
               "every opt_dff action has a counter in odin3_opt_counts");

/*
 * opt_dff (2F spec section 9.3, Yosys 0.55 opt_dff.cc) over every module of design, in creation
 * order: a work list of the module's FFs in ID order (rules 10 and 11 of Task 12 push the FFs they
 * make), rules 1-9 and 12 per FF (width 0; SET/CLR; ALOAD; ARST; SRST; EN; a constant CLK; D =
 * Q; AD = Q; a changed FF is re-emitted), then constbits (run_constbits without -sat): per bit,
 * the init, the reset values, the SR constants and a constant D or AD, combined as Yosys's
 * combine_const (x yields unless opts->keepdc), tie the Q bit to the constant and slice the bit
 * off. A pin an FF leaves open makes it no candidate (its nets are not known). Q ties are queued
 * and made at the end of the module's pass, so no rule and no constbits sees a tie of the same
 * call (plan point (l): Yosys's SigMap is built once per worker; a constant register pipeline
 * collapses one stage per call). Slices and rebuilds keep the FF's name and attributes. Adds the
 * changed FFs to counts->dff_changed, the constant bits to counts->dff_const_bits and the
 * actions to counts->dff_action; logs INFO "opt_dff: N flip-flops changed, M constant bits"
 * (this call's totals) and a DEBUG line "opt_dff: module 'm': <action> on <type> '<cell>'" per
 * change. Modules with live $proc nodes are optimised around them. INVALID_ARG (logged) for a
 * NULL argument; NO_MEMORY, possibly after some FFs were rewritten (the IR stays valid; each
 * rewrite builds before its first destructive step).
 */
odin3_status odin3_opt_dff_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 3G: wreduce (WR-1..WR-12) -------------------------------------------------------------- */

/*
 * wreduce (3G spec, Yosys 0.55 passes/opt/wreduce.cc) over every module of design, in creation
 * order, except those with live $proc nodes (INFO "wreduce: module 'm' has processes; skipped").
 * Per module: the pre-step (one-bit results, $div/$mod/$pow operand trimming, $mem address bits
 * unless opts->memx), then the worker: rounds over the cells with a pin on a net that the last
 * round changed (the first round: every node), in Yosys's cell order, narrowing $mux/$pmux, the
 * flip-flops and latches, and the operators to the widths and signedness their operands and
 * readers need (spec 4.1-4.3). opts->keepdc and opts->mux_undef as Yosys. Mixed-sign cells
 * (A_SIGNED != B_SIGNED on the 15 ruled types) read the both-signed flag everywhere (WR-6).
 * Adds into counts->wreduce; logs INFO "wreduce: N cells narrowed, M bits removed, K cells
 * removed, S cells made signed, U cells made unsigned" and DEBUG lines per rewrite. At
 * ODIN3_WREDUCE_MAX_ROUNDS a module stops with a WARN and the call is still ODIN3_OK. INVALID_ARG
 * (logged) for a NULL argument; NO_MEMORY, possibly after some rewrites (the IR stays valid; each
 * rewrite builds before its first destructive step).
 */
enum { ODIN3_WREDUCE_MAX_ROUNDS = 1000000 };
odin3_status odin3_opt_wreduce_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts);

/* --- 2F Task 4: registrations, options, the sub-pass call wrapper (OP-12, OP-16) ------------ */

/* A pass's parsed arguments (plan point (c)). */
typedef struct odin3_opt_args {
    odin3_opt_opts opts;
    bool stats, full_rescan; /* --opt-stats, --opt-full-rescan */
    bool simcheck;           /* --opt-simcheck (Debug builds; Task 17) */
} odin3_opt_args;

/*
 * Parses args (the words after the pass name) for pass ("opt", "opt_expr", ..., "opt_clean"):
 * Yosys's spellings and a `--` spelling of each; -full implies -mux_undef -mux_bool -undriven -fine
 * (opt_expr, opt) and -fine (opt_reduce); --opt-stats everywhere, --opt-full-rescan on opt and
 * opt_expr. *out is zeroed first. An unknown word logs "<pass>: unknown argument '<arg>'" and gives
 * ODIN3_ERR_INVALID_ARG, as does an unknown pass or a NULL out.
 */
odin3_status odin3_opt_parse_args(const char *pass, odin3_bytes args, odin3_opt_args *out);

/* The sub-passes odin3_opt_call runs, in the order of opt's script. */
typedef enum odin3_opt_sub {
    ODIN3_OPT_SUB_EXPR,
    ODIN3_OPT_SUB_MERGE,
    ODIN3_OPT_SUB_MUXTREE,
    ODIN3_OPT_SUB_REDUCE,
    ODIN3_OPT_SUB_SHARE,
    ODIN3_OPT_SUB_DFF,
    ODIN3_OPT_SUB_CLEAN,
    ODIN3_OPT_SUB_WREDUCE,
    ODIN3_OPT_N_SUBS
} odin3_opt_sub;

typedef struct odin3_opt_call_req {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_opt_sub sub;
    const odin3_opt_opts *opts;
    odin3_opt_counts *counts;
    bool simcheck; /* run the simulation check around the call (Debug builds only) */
} odin3_opt_call_req;

/*
 * One sub-pass call: the on_call test hook, the budget check (over budget logs "<pass> needs more
 * than N bytes (...)", the pass being ctx's, and gives ODIN3_ERR_PARSE), the "before" snapshot when
 * a snapshot directory is set, the sub-pass, in Debug odin3_check_design FAST, and the snapshot
 * files when the call changed anything. *changed is set to whether it did (the growth of the
 * sub-pass's counters in counts). ODIN3_ERR_INVALID_ARG (logged) for a NULL argument.
 */
odin3_status odin3_opt_call(const odin3_opt_call_req *req, bool *changed);

/*
 * The `opt` driver (2F spec §3, §12; plan Task 13), Yosys 0.55's loop through odin3_opt_call:
 * default mode runs `opt_expr X; opt_merge -nomux M`, then `do { opt_muxtree; opt_reduce R;
 * opt_merge M; [opt_share if -full]; [opt_dff D unless -noff]; opt_clean C; opt_expr X } while
 * (anything changed)`; -fast runs `do { opt_expr X; opt_merge M; [opt_dff D unless -noff]; if
 * (opt_dff changed nothing) break; opt_clean C }; opt_clean C`. After the first opt_expr call
 * opt_expr visits only the nodes the kit marked since (dirty.h), unless args->full_rescan.
 * At ODIN3_OPT_MAX_ITERATIONS (odin3_opt_limits_now) the loop stops with WARN "opt: module 'm': no
 * fixed point after K iterations; stopping" per module the last iteration changed, and the call
 * is still ODIN3_OK. INFO "opt: fixed point after K iterations" or "opt: -fast: done after K
 * iterations"; counts->iterations gets K. opt never compacts (IR-6 unchanged, PHASE2 #19): the
 * dead objects stay until a script runs `compact`. A failing sub-pass ends the run with its
 * status (the IR valid). INVALID_ARG (logged) for a NULL argument.
 */
odin3_status odin3_opt_run(odin3_pass_ctx *ctx, odin3_design *design, const odin3_opt_args *args,
                           odin3_opt_counts *counts);

/*
 * --opt-simcheck (Task 17; simcheck.c has the predicate and the rest of the contract). before
 * records the outputs of every simulable module on 64 cycles x seeds 1, 2, 3 (*out is owned by the
 * caller: odin3_opt_simcheck_free); after repeats the runs and compares: a module whose outputs
 * differ logs ERROR "opt: <sub>: simulation differs on module 'm' at cycle K" and gives
 * ODIN3_ERR_CHECK. A module that became unsimulable is skipped. INVALID_ARG (logged) for a NULL
 * argument; NO_MEMORY. free accepts NULL.
 */
typedef struct odin3_opt_simcheck odin3_opt_simcheck;
odin3_status odin3_opt_simcheck_before(odin3_design *design, odin3_opt_simcheck **out);
odin3_status odin3_opt_simcheck_after(odin3_design *design, const odin3_opt_simcheck *before,
                                      const char *sub);
void odin3_opt_simcheck_free(odin3_opt_simcheck *check);

/* odin3_pass_set_opt_snapshot is public (odin3.h); register.c defines it. */

/* --opt-stats: INFO "opt: rule <name> N" per non-zero rule or opt_dff action counter, by name. */
void odin3_opt_print_stats(const odin3_opt_counts *counts);

#endif
