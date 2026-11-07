/*
 * gl_int.h — private declarations of the graphlets pass (spec docs/specs/2026-10-30-graphlets-
 * design.md): options, groups, the netlist-to-graph adapter, the file output and the comparison.
 */
#ifndef ODIN3_PASSES_GRAPHLETS_GL_INT_H
#define ODIN3_PASSES_GRAPHLETS_GL_INT_H

#include "backends/dot/writer.h"
#include "graph/glet.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GL_MAX_FOCUS = 8,
    GL_MAX_GROUPS_COMPARED = 32,
    GL_DEFAULT_FANOUT = 16,
    GL_DEFAULT_MAX_NODES = 200000,
    GL_MAX_BINS = 32,
    GL_DEFAULT_INST_MAX = 100000,
    GL_LIMIT_INST_MAX = 10000000
};

#define GL_DEFAULT_MAX_WORK UINT64_C(20000000000)

typedef struct gl_opts {
    bool directed;
    unsigned size;
    bool exclude_global;
    uint32_t global_fanout;
    bool per_node;
    bool force;
    unsigned halo;
    size_t max_nodes;
    uint64_t max_work;
    bool inst_on;           /* --instances G given */
    uint32_t inst_graphlet; /* G */
    uint64_t inst_max;      /* --max N, default GL_DEFAULT_INST_MAX */
    odin3_strbuf inst_out;  /* --instances-out FILE */
    odin3_strbuf out;
    odin3_strbuf partition;
    odin3_strbuf edges_out;
    odin3_strbuf cells;
    odin3_strbuf calibration;
    odin3_strbuf cmp_a;
    odin3_strbuf cmp_b;
    odin3_strbuf focus[GL_MAX_FOCUS];
    odin3_dot_focus_kind focus_kind[GL_MAX_FOCUS];
    unsigned n_focus;
} gl_opts;

/* A group: nodes of one module counted on their induced subgraph. */
typedef struct gl_group {
    odin3_strbuf name;
    uint32_t module;
    const char *source; /* "module", "focus", "cells" or "partition" */
    uint8_t *mask;      /* per node ID of the module */
    bool whole;         /* the whole module: attributes are written */
} gl_group;

/* The edges of one module (node IDs) and the facts the adapter found. */
typedef struct gl_mod {
    odin3_module *mod;
    odin3_vec edges; /* glet_edge, node IDs */
    uint8_t *eligible;
    uint64_t global_nets;
    bool built;
} gl_mod;

/* One counted group, kept for the output and the comparisons. */
typedef struct gl_result {
    odin3_strbuf name;
    glet_sig sig;
    uint32_t n_nodes; /* in the counted subgraph */
    uint32_t reported;
    uint64_t edges;
    uint64_t self_loops;
    uint64_t work;
    double ms;
} gl_result;

/* --- gl_net.c: the netlist adapter ------------------------------------------------------------ */
odin3_status gl_mod_build(gl_mod *gm, odin3_module *mod, const gl_opts *opts);
void gl_mod_free(gl_mod *gm);

/* --- gl_group.c: where the groups come from -------------------------------------------------- */
typedef struct gl_groups {
    gl_group *items;
    size_t count;
    size_t cap;
} gl_groups;
odin3_status gl_groups_make(odin3_design *design, const gl_opts *opts, gl_groups *out);
void gl_groups_free(gl_groups *groups);

/* --- gl_inst.c: graphlet instances (spec 2026-11-06-odin3-graphlet-instances.md) ----------------
 */

/* What gl_inst_write needs to name the result. */
typedef struct gl_inst_info {
    const char *path;
    const char *mode;    /* "undirected-5", ... */
    const char *module;  /* module name */
    const uint32_t *ids; /* dense index -> module-local node ID */
    uint64_t max;
} gl_inst_info;

/* Allocates the buffer for up to opts->inst_max instances (inst->out). */
odin3_status gl_inst_begin(glet_inst *inst, const gl_opts *opts, const glet_spec *mode);
/* Maps the dense indices to node IDs (ascending within an instance) and writes the file. */
odin3_status gl_inst_write(const glet_inst *inst, const gl_inst_info *info);
void gl_inst_end(glet_inst *inst);

/* --- gl_cal.c: signature files, calibration, posterior ---------------------------------------- */
typedef struct gl_cal gl_cal;
gl_cal *gl_cal_load(const char *path);
void gl_cal_free(gl_cal *cal);
/* Posterior P(same | value) of one metric, false when the mode or metric is not calibrated. */
bool gl_cal_posterior(const gl_cal *cal, const char *mode, const char *metric, double value,
                      double *post);
/* Both metrics together (naive Bayes); false when either is missing. */
bool gl_cal_posterior_both(const gl_cal *cal, const char *mode, const double *values, double *post);

/* The two signatures of a comparison and how to report it. */
typedef struct gl_cmp_in {
    const char *name_a;
    const char *name_b;
    const glet_sig *sig_a;
    const glet_sig *sig_b;
    const gl_cal *cal;
    const char *mode;
} gl_cmp_in;

/* Appends the comparison as a JSON object to out. ints: every similarity as an integer in
 * millionths (`_ppm` keys), so the file stays readable by the integers-only reader; otherwise
 * decimals. */
odin3_status gl_compare_json(odin3_strbuf *out, const gl_cmp_in *in, bool ints);

/* Loads the signature of group `name` (NULL: the first) of a graphlets file; mode gets the mode. */
odin3_status gl_sig_load(const char *path_hash_name, glet_sig *sig, odin3_strbuf *mode,
                         odin3_strbuf *group);

odin3_status gl_run_compare(const gl_opts *opts);

/* Appends text as a quoted JSON string (quote and backslash escaped, control bytes become '?'). */
odin3_status gl_put_escaped(odin3_strbuf *out, const char *text);

#endif
