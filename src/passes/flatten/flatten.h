/*
 * flatten.h — the `flatten` pass (3A; spec docs/specs/2026-10-14-3A-flatten-design.md): its
 * options, caps, the pre-flight module graph and the entry point. Internal: not part of the ABI.
 */
#ifndef ODIN3_PASSES_FLATTEN_FLATTEN_H
#define ODIN3_PASSES_FLATTEN_FLATTEN_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* The caps of spec §12 (agent defaults): the flat module's live nodes, pins, nets, wires, and the
 * number of roots. Tests lower them through flatten_test.h. */
#define ODIN3_FLATTEN_MAX_NODES ((uint64_t)1 << 26)
#define ODIN3_FLATTEN_MAX_PINS ((uint64_t)1 << 28)
#define ODIN3_FLATTEN_MAX_NETS ((uint64_t)1 << 28)
#define ODIN3_FLATTEN_MAX_WIRES ((uint64_t)1 << 26)
#define ODIN3_FLATTEN_MAX_ROOTS ((uint32_t)1 << 16)

/* The separator when none is given (Yosys's `.`). */
#define ODIN3_FLATTEN_DEFAULT_SEPARATOR "."

/* Options of `flatten`. separator: NULL = ".", else a non-empty script word (FL-16). */
typedef struct odin3_flatten_opts {
    const char *separator;
} odin3_flatten_opts;

/*
 * Parses the pass arguments: `--separator <str>` (non-empty, no blank, no '"', ';' or '#'). The
 * separator's text is copied into *sep (caller-owned, freed by the caller) and opts->separator
 * points into it. INVALID_ARG (logged) for an unknown option or a bad separator; NO_MEMORY.
 */
odin3_status odin3_flatten_parse_args(odin3_bytes args, odin3_strbuf *sep,
                                      odin3_flatten_opts *opts);

/* The module graph of the design below its top (spec §3.3, pre-flight); opaque. */
typedef struct odin3_flatten_graph odin3_flatten_graph;

/* Totals of a graph. instances/kept: instance nodes expanded / left as instances over all roots;
 * nodes..wires: the sum over the roots of the flat sizes (saturating); reach: modules reachable
 * from the top; unused: modules that are not roots (they stay in the design, spec §10). */
typedef struct odin3_flatten_totals {
    uint64_t instances, kept, nodes, pins, nets, wires;
    uint32_t reach, roots, unused;
} odin3_flatten_totals;

/*
 * Builds the graph of the live module-type nodes below `top` (cell type -> module map in module
 * ID order, Kahn order, roots; no recursion), and the flat sizes. Mutates nothing. INVALID_ARG
 * (logged `flatten: recursive instantiation: a -> b -> a`) for a cycle, also through a kept
 * instance; NO_MEMORY. *out is set only on success.
 */
odin3_status odin3_flatten_graph_build(odin3_design *design, odin3_module_id top,
                                       odin3_flatten_graph **out);

void odin3_flatten_graph_free(odin3_flatten_graph *graph);

/* The totals; the module IDs reachable from the top in Kahn (parents first) order; the roots in
 * module-ID order. The arrays belong to the graph; n gets the length. */
odin3_flatten_totals odin3_flatten_graph_totals(const odin3_flatten_graph *graph);
const uint32_t *odin3_flatten_graph_order(const odin3_flatten_graph *graph, uint32_t *n);
const uint32_t *odin3_flatten_graph_roots(const odin3_flatten_graph *graph, uint32_t *n);

/* The flat size of module `mod` (module ID) when all its non-kept instances are expanded. */
typedef struct odin3_flatten_size {
    uint64_t nodes, pins, nets, wires, instances, kept;
    uint64_t alias_bits; /* wire bits that are aliases of a net (not its primary) */
    uint64_t bare;       /* bare alias names, and the names of port-joined nets: one net each */
} odin3_flatten_size;
odin3_flatten_size odin3_flatten_graph_size(const odin3_flatten_graph *graph, uint32_t mod);

/* The objects the expansion adds to the roots: the roots' flat sizes less the objects they own
 * already (the instance and port nodes that go are not counted either way). */
odin3_flatten_size odin3_flatten_graph_added(const odin3_flatten_graph *graph);

/* The bytes the expansion is estimated to add (spec §12; saturating, rounded up). */
uint64_t odin3_flatten_graph_estimate(const odin3_flatten_graph *graph);

/* The module of cell type `type`, 0 when no module has it. */
uint32_t odin3_flatten_graph_module_of(const odin3_flatten_graph *graph, odin3_celltype_id type);

/* True when module `mod` is a root (the top, or a module that stays an instance somewhere). */
bool odin3_flatten_graph_is_root(const odin3_flatten_graph *graph, uint32_t mod);

/* True for the four shared-constant cell types $_CONST0_/1/X/Z_ (IR-4). */
bool odin3_flatten_graph_is_const(const odin3_flatten_graph *graph, odin3_celltype_id type);

/* INFO line `flatten: N modules are no longer instantiated: a, b, … (the IR cannot remove
 * modules)`, names capped at 8; nothing when there are none. */
void odin3_flatten_graph_log_unused(const odin3_flatten_graph *graph);

/*
 * The pre-flight checks that mutate nothing: the caps (`flatten: the flat design would have N
 * nodes (cap M)`; INVALID_ARG) and the design's budget with the estimate (the budget's message;
 * ODIN3_ERR_PARSE). Logged.
 */
odin3_status odin3_flatten_preflight(const odin3_pass_ctx *ctx, const odin3_design *design,
                                     const odin3_flatten_graph *graph);

/*
 * The keep_hierarchy rule of spec §7: the instance node `inst` of `parent` has the attribute
 * `keep_hierarchy`, or `child` (its module) has it as a module attribute. A value is true as
 * Yosys's Const::as_bool: INT non-zero, BITS with a 1 bit, STRING non-empty.
 */
bool odin3_flatten_is_kept(const odin3_module *parent, odin3_node_id inst,
                           const odin3_module *child);

/*
 * The pass: expands every non-kept module instance of the design's top and of every kept module
 * into the module that holds it (spec §3). INVALID_ARG (logged) for no top, a cycle or a cap;
 * ODIN3_ERR_PARSE for the budget; NO_MEMORY. Failures before the first mutation leave the design
 * untouched; later ones follow spec §12.
 */
odin3_status odin3_flatten_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_flatten_opts *opts);

#endif
