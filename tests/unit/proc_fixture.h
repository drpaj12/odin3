/*
 * proc_fixture.h — a $proc node for tests outside test_ir_proc.c (the pass manager's and the
 * writers' process refusals, 2D EL-33; the ABI's process count and BODY text, 2D Task 6): Task 3's
 * P1, `always @(posedge clk) q <= d;`.
 */
#ifndef ODIN3_TESTS_UNIT_PROC_FIXTURE_H
#define ODIN3_TESTS_UNIT_PROC_FIXTURE_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"

/*
 * Creates P1 in module on four fresh 1-bit wires named clk, d, q and t0 (I = [clk, d],
 * O = [q, t0]; root actions t0 <- q, t0 <- d; sync posedge clk: q <- t0) with provenance prov.
 * *node gets it. A failing IR call's status (the wires made so far stay); no Unity
 * (proc_fixture_core.c), so the odin3_procfix library can use it too.
 */
odin3_status proc_fixture_build_p1(odin3_module *module, odin3_prov_id prov, odin3_node_id *node);

/*
 * Fills an empty design: a pass run "test" with a SOURCE record (t.v:1), an empty module "e", then
 * module "m" holding P1, both with that provenance; *module gets m (NULL on failure). So the first
 * module with a process, which every refusal names, is m. A failing IR call's status; no Unity.
 */
odin3_status proc_fixture_build_design(odin3_design *design, odin3_module **module);

/* proc_fixture_build_p1, failing the running Unity test on any error; returns the node. */
odin3_node_id proc_fixture_p1(odin3_module *module, odin3_prov_id prov);

/* proc_fixture_build_design, failing the running Unity test on any error; returns m. */
odin3_module *proc_fixture_design(odin3_design *design);

/*
 * The odin3_procfix library's one export besides libodin3's (proc_fixture_lib.c): a new design
 * filled by proc_fixture_build_design, owned by the caller (odin3_design_destroy); NULL on any
 * failure. Test-only: the Python walk's way to a $proc node before a front end makes one.
 */
odin3_design *odin3_test_proc_design(void);

#endif
