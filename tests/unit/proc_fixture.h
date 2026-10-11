/*
 * proc_fixture.h — a $proc node for tests outside test_ir_proc.c (the pass manager's and the
 * writers' process refusals, 2D EL-33): Task 3's P1, `always @(posedge clk) q <= d;`.
 */
#ifndef ODIN3_TESTS_UNIT_PROC_FIXTURE_H
#define ODIN3_TESTS_UNIT_PROC_FIXTURE_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"

/*
 * Creates P1 in module on four fresh 1-bit wires named clk, d, q and t0 (I = [clk, d],
 * O = [q, t0]; root actions t0 <- q, t0 <- d; sync posedge clk: q <- t0) with provenance prov,
 * and returns the node. Fails the running Unity test on any error.
 */
odin3_node_id proc_fixture_p1(odin3_module *module, odin3_prov_id prov);

/*
 * Fills an empty design: a pass run "test" with a SOURCE record (t.v:1), an empty module "e", then
 * module "m" holding P1, both with that provenance; returns m. So the first module with a process,
 * which every refusal names, is m.
 */
odin3_module *proc_fixture_design(odin3_design *design);

#endif
