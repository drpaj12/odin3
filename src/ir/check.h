/*
 * check.h — the IR invariant checker (IR §9): rules 1–11, fast and full.
 */
#ifndef ODIN3_IR_CHECK_H
#define ODIN3_IR_CHECK_H

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"

/* FAST runs rules 1–5 and 11; FULL runs every rule (IR §9). */
typedef enum odin3_check_level { ODIN3_CHECK_FAST, ODIN3_CHECK_FULL } odin3_check_level;

/* The view to assert for rule 10 (IR-9); NONE skips the view check. */
typedef enum odin3_view { ODIN3_VIEW_NONE, ODIN3_VIEW_RTLIL, ODIN3_VIEW_NETLIST } odin3_view;

typedef struct odin3_check_opts {
    odin3_check_level level;
    odin3_view view; /* used by FULL only: rule 10 is not a fast rule */
} odin3_check_opts;

/*
 * Checks one module against the invariants of IR §9. Every violation is logged as
 * "check: <module>: rule <n>: <detail>", at ODIN3_LOG_ERROR for an E rule and ODIN3_LOG_WARN for
 * a W rule; per rule and severity the first few violations are shown and the rest summed in one
 * line (a 2M-node module must not flood the log, and warnings never hide an error). Returns
 * ODIN3_ERR_CHECK when any E rule is violated, ODIN3_OK otherwise (warnings do not fail),
 * ODIN3_ERR_NO_MEMORY when a FULL check cannot get its mark arrays (nothing is logged as a
 * violation then), ODIN3_ERR_INVALID_ARG (logged) for a NULL module or a level or view outside its
 * enum. Never changes the IR. Runs in time linear in nodes + pins + nets + wires + aliases, without
 * recursion. Rule 7 here covers the module's objects' prov IDs only; the design-global provenance
 * records are checked by odin3_check_design (once per design, not once per module).
 *
 * Rule 7 in Phase 1: a live object with prov 0 (made before provenance existed, or by a test) is
 * one WARNING per module giving the count; a nonzero prov that is not a record is an ERROR.
 * Rule 6 counts a net's alias names: a merged net's old names map to the kept net.
 * Constant cells (flag ODIN3_CT_ANYVIEW) pass the view check in every view (IR-9).
 */
odin3_status odin3_check_module(odin3_module *module, odin3_check_opts opts);

/*
 * odin3_check_module for every module in creation order; FULL then checks the provenance records
 * once (rule 7: every run exists, every DERIVED record has parents with smaller IDs), logged as
 * "check: design: ...". ODIN3_ERR_CHECK when any module or record fails (every module is still
 * checked); ODIN3_ERR_NO_MEMORY as for a module; ODIN3_ERR_INVALID_ARG (logged) for a NULL design
 * or invalid options.
 */
odin3_status odin3_check_design(odin3_design *design, odin3_check_opts opts);

#endif
