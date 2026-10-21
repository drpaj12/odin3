/*
 * cone_check.h — the `cone_check` pass (Phase 7 spec 3.3, plan Task 7): proves that the nodes
 * marked KEY=V compute one cell of a given type, under the binding of their boundary nets to the
 * cell's port bits, and records the verdict as module attributes. odin3_cone_hash is the group's
 * fingerprint, which `raise` (Task 8) compares to refuse a group that changed since the proof.
 */
#ifndef ODIN3_PASSES_CONE_CHECK_CONE_CHECK_H
#define ODIN3_PASSES_CONE_CHECK_CONE_CHECK_H

#include "ir/module.h"
#include "odin3/odin3.h"

/* The hash text: 64 lower-case hex digits and the NUL. */
enum { ODIN3_CONE_HASH_BYTES = 65 };

/*
 * Writes into out the SHA-256 (lower-case hex, NUL-terminated) of the group of `module` whose node
 * attribute `key` has the text `value` (a STRING with that text, or an INT with that decimal
 * text). The hashed text is the line `<key>=<value>`, then for each live group node in ascending ID
 * order its ID, type name, parameter values (whole payload) and, per pin in pin order, the port,
 * bit and net ID it connects to (0 when open), then for each boundary net of the group in
 * ascending net ID order that carries `odin3:bind`, the line `b<net ID>:<binding>`. A boundary net
 * is a net a group node touches that is driven by a group node and read outside the group, or read
 * by a group node and not driven by one. Nothing changes and nothing is logged. Failure:
 * INVALID_ARG for a NULL argument or an empty group; NO_MEMORY.
 */
odin3_status odin3_cone_hash(const odin3_module *module, const char *key, const char *value,
                             char out[ODIN3_CONE_HASH_BYTES]);

#endif
