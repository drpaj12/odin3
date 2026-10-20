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
#include "passes/fsm/index.h"

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

/* --- the passes (fsm.c, register.c) ---------------------------------------------------------- */

/* fsm_detect over every module of design, in creation order (the `fsm_detect` pass). */
odin3_status odin3_fsm_detect_run(odin3_design *design, const odin3_fsm_opts *opts);

/* --- detect (fsm_detect.cc) --------------------------------------------------------------- */

/*
 * fsm_detect on one module (spec D-1 to D-7): every wire, in ID order, that is a state register
 * gets the attribute fsm_encoding = "auto"; the log says why a candidate is not marked. The IR
 * changes by those attributes (and the removal of an fsm_encoding from a 1-bit net) only.
 * NO_MEMORY leaves the attributes set so far.
 */
odin3_status odin3_fsm_detect_module(odin3_fsm_mod *fm);

#endif
