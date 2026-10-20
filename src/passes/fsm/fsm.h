/*
 * fsm.h — the fsm pass (4.8; docs/specs/2026-10-20-4.8-fsm-design.md): Yosys 0.55's fsm driver and
 * its fsm_detect, fsm_extract, fsm_opt, fsm_recode, fsm_info and fsm_map stages, ported to the IR
 * with the FSM table inside the pass (no `$fsm` cell; PHASE4 decision 12).
 *
 * The stages run over the whole design in Yosys's order (fsm.cc:138-160): detect, extract, opt,
 * (opt_clean in Yosys: here the unused-output query of unused.c), opt, recode, info, map. Each
 * stage logs at INFO as spec section 16 says. ODIN3_ERR_NO_MEMORY from a stage before map leaves
 * the IR as it was; map builds every new object before its first destructive step.
 */
#ifndef ODIN3_PASSES_FSM_FSM_H
#define ODIN3_PASSES_FSM_FSM_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The evaluation cap per FSM (X-6, question 4); odin3_fsm_opts.max_evals 0 means this. */
enum { ODIN3_FSM_MAX_EVALS = 1000000 };

typedef struct odin3_fsm_opts {
    bool nodetect;          /* -nodetect: only user-marked registers */
    bool norecode;          /* -norecode */
    bool nomap;             /* -nomap: stages up to and including info, the IR unchanged */
    bool ignore_self_reset; /* fsm_detect -ignore-self-reset */
    const char *encoding;   /* -encoding TYPE for `auto` FSMs, NULL for none */
    uint64_t max_evals;     /* 0: ODIN3_FSM_MAX_EVALS */
} odin3_fsm_opts;

/* --- one module ---------------------------------------------------------------------------- */

/* The module a stage works on: its net index and the attribute keys. */
typedef struct odin3_fsm_mod {
    odin3_design *design;
    odin3_module *module;
    const odin3_fsm_opts *opts;
    odin3_fsm_index ix;
    uint32_t key_encoding; /* strtab ID of `fsm_encoding` */
    uint32_t key_init;     /* of `init` */
} odin3_fsm_mod;

/* Opens module for the stages (builds the index, interns the keys). NO_MEMORY with *fm closed. */
odin3_status odin3_fsm_mod_open(odin3_fsm_mod *fm, odin3_design *design, odin3_module *module,
                                const odin3_fsm_opts *opts);

/* Frees the index; safe on a closed one. */
void odin3_fsm_mod_close(odin3_fsm_mod *fm);

/* The text of strtab ID str ("" for 0). */
const char *odin3_fsm_str(const odin3_fsm_mod *fm, uint32_t str);

/* The module's name. */
const char *odin3_fsm_mod_name(const odin3_fsm_mod *fm);

/* "<module>.<wire>" in buf (truncated); an unnamed wire is "$w<ID>". */
void odin3_fsm_wire_label(const odin3_fsm_mod *fm, odin3_wire_id wire, char *buf, size_t size);

/* Appends the signal bits of wire (LSB first) to out (elements odin3_fsm_sig). */
odin3_status odin3_fsm_wire_sigs(const odin3_fsm_mod *fm, odin3_wire_id wire, odin3_vec *out);

/* The `fsm_encoding` string of wire, NULL when it has none ("" for a value that is not a string).
 */
const char *odin3_fsm_wire_encoding(const odin3_fsm_mod *fm, odin3_wire_id wire);

/* Sets the string attribute `key` of wire to text. NO_MEMORY. */
odin3_status odin3_fsm_wire_set_str(odin3_fsm_mod *fm, odin3_wire_id wire, uint32_t key,
                                    const char *text);

/* --- one extracted FSM (the `$fsm` cell of Yosys, kept inside the pass) --------------------- */

enum { ODIN3_FSM_ENC_MAX = 32 };

typedef struct odin3_fsm_inst {
    odin3_wire_id wire;      /* the state wire */
    uint32_t dff_node;       /* the old state register */
    odin3_vec dff_out;       /* odin3_fsm_sig: the wire's bits (Q) */
    odin3_vec ctrl_in;       /* odin3_fsm_sig, sorted */
    odin3_vec ctrl_out;      /* odin3_fsm_sig, sorted; opt removes bits */
    odin3_vec mux_cells;     /* uint32_t node IDs of the selection tree, ascending */
    odin3_vec out_drivers;   /* uint32_t node IDs that drove ctrl_out, ascending */
    odin3_fsm_sig clk, arst; /* arst is a constant 0 when the register has no async reset */
    bool clk_pol, arst_pol;
    char encoding[ODIN3_FSM_ENC_MAX];       /* the wire's fsm_encoding when extracted */
    char final_encoding[ODIN3_FSM_ENC_MAX]; /* one-hot, binary, none or user, once recoded */
    odin3_fsm_data data;
} odin3_fsm_inst;

/* Frees an instance (safe on a zeroed one). */
void odin3_fsm_inst_free(odin3_fsm_inst *inst);

/*
 * fsm_extract on one module (spec X-1 to X-6): every wire with an fsm_encoding other than
 * `none` is extracted into an odin3_fsm_inst appended to insts (elements odin3_fsm_inst); a
 * wire that fails is logged and left as it is. The IR is not changed (X-5). A wire whose
 * transition search exceeds the evaluation cap is warned of and skipped. NO_MEMORY leaves the
 * instances appended so far in insts (the caller frees them).
 */
odin3_status odin3_fsm_extract_module(odin3_fsm_mod *fm, odin3_vec *insts);

/* --- opt (fsm_opt.cc; spec O-0 to O-5) ----------------------------------------------------- */

/*
 * The six stages of fsm_opt on one FSM, in Yosys's order (fsm_opt.cc:302-310): unreachable states,
 * unused outputs, aliased inputs, feedback inputs, don't-care merging, constant and unused inputs.
 * unused (may be NULL: none) has one byte per control output, non-zero for an output that no live
 * logic reads (O-1). The tables, ctrl_in and ctrl_out of inst change; the IR does not. NO_MEMORY
 * leaves inst valid (a stage may be half done).
 */
odin3_status odin3_fsm_opt(odin3_fsm_inst *inst, const uint8_t *unused);

/* The stages one by one, for the tests. */
odin3_status odin3_fsm_opt_unreachable(odin3_fsm_inst *inst);
void odin3_fsm_opt_unused_outputs(odin3_fsm_inst *inst, const uint8_t *unused);
void odin3_fsm_opt_alias_inputs(odin3_fsm_inst *inst);
void odin3_fsm_opt_feedback_inputs(odin3_fsm_inst *inst);
odin3_status odin3_fsm_opt_dont_care(odin3_fsm_inst *inst);
odin3_status odin3_fsm_opt_const_inputs(odin3_fsm_inst *inst);

/*
 * The "unread and not a port" query that replaces unused_bits (spec O-1, question 2): for module
 * fm with the FSMs insts (elements odin3_fsm_inst), *used gets one byte per net of the module
 * (free it with odin3_util_free), non-zero for a net read by a live node, a module output or a
 * keep wire, where "live" is opt_clean's liveness with every FSM's old register and logic dead
 * unless something else reads them: the FSM itself stands in for the drivers of its control
 * outputs and reads its control inputs, clock and reset. NO_MEMORY with *used NULL.
 */
odin3_status odin3_fsm_used_nets(const odin3_fsm_mod *fm, const odin3_vec *insts, uint8_t **used);

/* The unused flags of inst's control outputs for a used-net map (a constant output is used). */
odin3_status odin3_fsm_unused_flags(const odin3_fsm_inst *inst, const uint8_t *used,
                                    odin3_vec *flags);

/* --- recode (fsm_recode.cc; spec R-1 to R-5) ----------------------------------------------- */

/*
 * fsm_recode on one FSM: resolves the encoding (the attribute, default auto; `auto` is one-hot
 * below 32 states and binary from 32 up unless opts->encoding says otherwise), numbers the states
 * (the reset state 0, the others 1, 2, ... in table order) and rewrites the codes: one-hot is
 * state_bits = number of states with a 1 in bit `index` and don't cares elsewhere, binary is
 * ceil_log2(states) bits holding the index. none and user, and a binary request that would not
 * shorten the register, leave the codes. Sets inst->final_encoding. NO_MEMORY leaves inst as it
 * was.
 */
odin3_status odin3_fsm_recode(const odin3_fsm_mod *fm, odin3_fsm_inst *inst);

/* Yosys's ceil_log2: 0 for 0 or 1, else the least n with 2^n >= x. */
int32_t odin3_fsm_ceil_log2(int32_t value);

/* --- info (fsm_info.cc, FsmData::log_info; spec I-1) -------------------------------------- */

/* Logs the tables of inst at INFO level, one message per line, each starting "fsm: ". */
void odin3_fsm_info(const odin3_fsm_mod *fm, const odin3_fsm_inst *inst);

/* The label of a signal bit in a log line: "name", "name [k]", "$n<ID>" or a constant. */
void odin3_fsm_sig_label(const odin3_fsm_mod *fm, odin3_fsm_sig sig, char *buf, size_t size);

/* --- the passes (fsm.c, register.c) ---------------------------------------------------------- */

/* fsm_detect over every module of design, in creation order (the `fsm_detect` pass). */
odin3_status odin3_fsm_detect_run(odin3_design *design, const odin3_fsm_opts *opts);

/*
 * The `fsm` driver (fsm.cc:138-160): detect (unless nodetect), extract, opt, the unused-output
 * query and opt again, recode (unless norecode), info, map (unless nomap) over the whole
 * design, each stage over every module before the next. ODIN3_ERR_NO_MEMORY from a stage before
 * map leaves the IR as it was (detect's attributes aside).
 */
odin3_status odin3_fsm_run(odin3_design *design, const odin3_fsm_opts *opts);

/* --- detect (fsm_detect.cc) --------------------------------------------------------------- */

/*
 * fsm_detect on one module (spec D-1 to D-7): every wire, in ID order, that is a state register
 * gets the attribute fsm_encoding = "auto"; the log says why a candidate is not marked. The IR
 * changes by those attributes (and the removal of an fsm_encoding from a 1-bit net) only.
 * NO_MEMORY leaves the attributes set so far.
 */
odin3_status odin3_fsm_detect_module(odin3_fsm_mod *fm);

#endif
