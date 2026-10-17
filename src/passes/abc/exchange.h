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

#endif
