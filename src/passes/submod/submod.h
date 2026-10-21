/*
 * submod.h — the `submod` pass (Phase 7 spec 3.1, plan Task 5): the live nodes of a module that
 * carry attribute KEY with one value V move into a new module named V, replaced by one instance.
 * odin3_submod_extract is the same step for one group, reused by cone_check (spec 3.3, C-3).
 */
#ifndef ODIN3_PASSES_SUBMOD_SUBMOD_H
#define ODIN3_PASSES_SUBMOD_SUBMOD_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

/*
 * Moves the group of module m whose attribute `key` has the text `value` (a STRING attribute with
 * that text, or an INT with that decimal text) into a new module named `value`, replaces the nodes
 * by one instance `<value>_i` in m, and puts the new module's ID in *out (may be NULL).
 * The moved nodes keep their names, provenance, parameters and attributes, except `key` itself,
 * which is dropped so that a second run finds no group. Ports of the new module are the boundary
 * nets (spec 3.1 S-2): one scalar port per net, named like the net when it has a real name (not
 * starting with '$'), else `p<k>`.
 * Ownership: the design owns everything created. The call opens its own pass run named `submod`;
 * the pass itself uses odin3_submod_extract_ctx with its run.
 * Failure: INVALID_ARG (logged, located at the first moved node's source when known) for an empty
 * group, a module name clash, a port node in the group, a boundary net with two drivers or an
 * inout pin; in all of these the IR is unchanged. NO_MEMORY may leave a partly built module.
 */
odin3_status odin3_submod_extract(odin3_design *d, odin3_module_id m, const char *key,
                                  const char *value, odin3_module_id *out);

/* As odin3_submod_extract inside the pass run `ctx` (one new operation per group). */
odin3_status odin3_submod_extract_ctx(odin3_pass_ctx *ctx, odin3_module_id m, const char *key,
                                      const char *value, odin3_module_id *out);

#endif
