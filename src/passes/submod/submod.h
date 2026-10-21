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
#include "util/vec.h"

#include <stdint.h>

/* One port of the new module: its name (strtab ID), direction, width and, LSB first, the nets of
 * the old module that it carries (they stay in the old module, on the instance). */
typedef struct odin3_submod_port {
    uint32_t name;
    odin3_dir dir;
    uint32_t width;
    const odin3_net_id *nets; /* width entries, owned by the result */
} odin3_submod_port;

/*
 * What one extraction made: the new module, its instance in the old module and the ports in the
 * new module's port order (so also the instance's port order). Release with
 * odin3_submod_result_free. Zero the struct before first use; freeing twice is safe.
 */
typedef struct odin3_submod_result {
    odin3_module_id module;
    odin3_node_id instance;
    uint32_t n_ports;
    const odin3_submod_port *ports; /* n_ports entries, owned by the result */
    odin3_vec port_store;           /* internal: odin3_submod_port */
    odin3_vec net_store;            /* internal: odin3_net_id */
} odin3_submod_result;

void odin3_submod_result_free(odin3_submod_result *result);

/*
 * Moves the group of module `module_id` whose attribute `key` has the text `value` (a STRING
 * attribute with that text, or an INT with that decimal text) into a new module named `value`,
 * replaces the nodes by one instance `<value>_i` in m, and fills *out (may be NULL) with the new
 * module, the instance and the ports. The moved nodes keep their names, provenance, parameters and
 * attributes, except `key` itself, which is dropped so that a second run finds no group. Ports of
 * the new module are the boundary nets (spec 3.1 S-2): a wire whose bits are all boundary nets of
 * one direction, each the primary of its bit, becomes one port named like the wire with its width
 * and range; every other boundary net becomes a scalar port named like the net when it has a real
 * name (not starting with '$'), else like its wire (`w` or `w[i]`), else `p<k>`. A port wire gets
 * the attributes of the old wire (the key excepted); the attributes of the boundary nets stay on
 * the old nets. A port name equal to the name of an internal net renames that net `$submod$<ID>`.
 * The new module and the instance get a DERIVED provenance record of the pass run. Ownership: the
 * design owns everything created; *out owns its arrays (odin3_submod_result_free). The call is
 * destructive on module_id: a caller that needs the original copies the design first. It opens
 * its own pass run named `submod`; the pass itself uses odin3_submod_extract_ctx with its run.
 * Failure: INVALID_ARG (logged, located at the first moved node's source when known) for an empty
 * group, a module name clash, a port node in the group, a boundary net with two drivers or an inout
 * pin; in all of these the IR is unchanged. NO_MEMORY may leave a partly built module.
 */
odin3_status odin3_submod_extract(odin3_design *design, odin3_module_id module_id, const char *key,
                                  const char *value, odin3_submod_result *out);

/* As odin3_submod_extract inside the pass run `ctx` (one new operation per group). */
odin3_status odin3_submod_extract_ctx(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                      const char *key, const char *value, odin3_submod_result *out);

#endif
