/*
 * writer.h — Graphviz dot writer: one cluster per module, a node per cell, an edge per
 * driver-to-sink pin pair, with an optional focus and a node budget (1F spec, "dot").
 */
#ifndef ODIN3_BACKENDS_DOT_WRITER_H
#define ODIN3_BACKENDS_DOT_WRITER_H

#include "ir/design.h"
#include "odin3/odin3.h"

#include <stddef.h>

/* The node budget used when odin3_dot_opts.max_nodes is 0. */
enum { ODIN3_DOT_DEFAULT_MAX_NODES = 2000 };

typedef enum odin3_dot_focus_kind {
    ODIN3_DOT_FOCUS_NONE, /* draw everything, subject to the node budget */
    ODIN3_DOT_FOCUS_PATH, /* focus = "module" or "module/inst/inst...": that module's subtree */
    ODIN3_DOT_FOCUS_LOC,  /* focus = "file:line": the objects the provenance forward index finds */
    ODIN3_DOT_FOCUS_CONE  /* focus = "net" or "module:net": the net's fan-in cone */
} odin3_dot_focus_kind;

/* A focus (anything but NONE) lifts the budget; focus is then non-NULL. */
typedef struct odin3_dot_opts {
    odin3_dot_focus_kind focus_kind;
    const char *focus;
    size_t max_nodes; /* 0 = ODIN3_DOT_DEFAULT_MAX_NODES */
} odin3_dot_opts;

/*
 * Writes the design to path as a dot digraph. Node IDs are m<module ID>n<node ID>; clusters are
 * in module order, nodes and edges in ID order, so the output is deterministic. opts may be NULL
 * (no focus, default budget). ODIN3_ERR_INVALID_ARG (logged) for a NULL design or path, a
 * malformed or unresolvable focus, or a design of more nodes than the budget without a focus
 * (the message gives the count); ODIN3_ERR_IO if path cannot be written; ODIN3_ERR_NO_MEMORY.
 * The file is written to a private temporary beside path (util/file.h) and renamed over it, so
 * on failure path is left as it was. Clusters are flat (cluster_<module ID>, the module name is the
 * label); a cone stops at module-instance nodes and does not descend into the submodule.
 */
odin3_status odin3_dot_write(const odin3_design *design, const char *path,
                             const odin3_dot_opts *opts);

#endif
