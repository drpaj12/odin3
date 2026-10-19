/*
 * exchange.h — the BLIF exchange of one run (3D spec §6.2, §7.6, §7.9; plan Task 16): write the
 * input and the command file, run ABC (the linked engine, or the odin3-abc child), read the output
 * back into an abc_result. The core logs ABC's lines after the engine call returns (IR-17), and
 * every failure as its §11 ERROR text with status ENGINE.
 */
#ifndef ODIN3_PASSES_ABC_EXCHANGE_H
#define ODIN3_PASSES_ABC_EXCHANGE_H

#include "odin3/odin3.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "util/arena.h"

/*
 * Runs ABC on the current region of run (run->module, run->domain, run->region) through files
 * abc<r>.in.blif, .cmd, .log and .out.blif of run->run_no in run->dir (made when missing: a
 * mkdtemp directory when --via-blif is absent). The route decides the engine: LINKED opens a
 * session of run->engine and execs `read`, each script line, `&put` and `write_hie`; CHILD runs
 * `odin3-abc -f abc<r>.cmd` through odin3_abc_child_run (after the pin check). ABC's lines go to
 * the run's log file, --log and the pass log (`abc| <line>`: INFO for print commands, else
 * DEBUG). ABC's exit status is never trusted: the output file must exist and be a valid network,
 * else §11's `abc: ABC returned a malformed network: …`. run->run_no is incremented once the files
 * are made (also on failure), so a failed run's files keep their number.
 *
 * Returns ODIN3_OK with *out filled (checked against the region, not yet folded);
 * ODIN3_ERR_ENGINE (logged) for any ABC, child or result failure; ODIN3_ERR_IO (logged) when a
 * file or the directory cannot be made; NO_MEMORY.
 */
odin3_status odin3_abc_exchange_blif(odin3_abc_run *run, odin3_abc_result *out);

/*
 * The in-memory exchange (§6.3, AB-16, AB-22) of the same current region: builds the AIG (aig.h; it
 * stays in run->aig until odin3_abc_aig_free, for the provenance anchors), then one session of
 * run->engine: open; load_aig; `&put`; the script; `&get -m -n` when the script ends in the
 * network space; get_luts; (fine provenance, within the cap) the name map on the aligned MiniLUT;
 * close. A failing internal command is `abc: internal command '<cmd>' failed`; the other failures
 * are as odin3_abc_exchange_blif's. *out is the checked result (odin3_abc_result_from_luts), not
 * yet folded; with a name map its old_lit holds one MiniAIG literal (or -1) per object, and
 * run->nomap_reason is "". Without one (--prov cone, a map above ODIN3_ABC_NAMEMAP_MAX_OBJS, a
 * RESULT or COMMAND failure of name_map) old_lit is empty and run->nomap_reason is the §8.2 reason.
 * run->run_no is incremented. ODIN3_ERR_PARSE (logged) when the AIG needs too many objects.
 */
odin3_status odin3_abc_exchange_aig(odin3_abc_run *run, odin3_abc_result *out);

/*
 * The aligned MiniLUT (§6.3, AB-16): the engine's result luts with the loaded AIG's interface, so
 * that the name map can run on it. The PIs as sent, with one register output per input register k
 * (run->region->regs); the LUTs as in luts, each fanin from result register j remapped to k*(j)
 * (luts->ci_from of its PI); the real outputs as in luts; the register inputs: j's D driver for k
 * = k*(j), the constant 0 for any other k. The arrays live in arena. ODIN3_ERR_INVALID_ARG (not
 * logged) when a register's name is not an input register's; NO_MEMORY.
 */
odin3_status odin3_abc_aligned_build(const odin3_abc_run *run, const odin3_abc_luts *res,
                                     odin3_arena *arena, odin3_abc_luts *aligned);

#endif
