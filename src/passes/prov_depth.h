/*
 * prov_depth.h — provenance depth: one number (0..4, or auto) that sets the older provenance
 * options together, and an up-front estimate of how much data each depth writes. Spec:
 * docs/specs/2026-11-13-prov-levels.md. Private to src/passes; no public ABI beyond
 * odin3_pass_set_prov_depth (odin3.h).
 *
 * The word "level" also names the granularity ladder (module, block, ..., lut) of the provenance
 * chain; this feature is the "provenance depth" in prose, `--prov-level` on the command line.
 */
#ifndef ODIN3_PASSES_PROV_DEPTH_H
#define ODIN3_PASSES_PROV_DEPTH_H

#include "ir/construct.h"
#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    ODIN3_PROV_DEPTH_COUNT = 5, /* depths 0..4 */
    ODIN3_PROV_DEPTH_FULL = 4
};

/* What abc does with the edge budget of a run. */
typedef enum odin3_prov_edges_mode {
    ODIN3_PROV_EDGES_DEFAULT, /* the formula (at least 4194304 edges) */
    ODIN3_PROV_EDGES_BULK,    /* one shared record for the run's LUTs, chosen: no WARNING */
    ODIN3_PROV_EDGES_UNLIMITED
} odin3_prov_edges_mode;

/* The older options one depth stands for. */
typedef struct odin3_prov_knobs {
    odin3_construct_mode ast; /* read_verilog --prov-ast */
    odin3_prov_edges_mode edges;
    bool cut;   /* abc --prov-cut on */
    bool map;   /* write_* --prov-map where the names are hash names */
    bool steps; /* odin3-flow: the prov/ step files */
} odin3_prov_knobs;

/* The name of a depth: off, statement, ancestry, cut, full. */
const char *odin3_prov_depth_name(uint32_t depth);

/* The knobs of depth 0..4 (a value above 4 is read as 4). */
odin3_prov_knobs odin3_prov_depth_knobs(uint32_t depth);

/* What the user asked for: from the CLI, else the project, else nothing. */
typedef struct odin3_prov_request {
    bool set;        /* a depth or `auto` was given */
    bool autolevel;  /* `auto` */
    uint32_t level;  /* 0..4 when set and not auto */
    uint64_t budget; /* bytes; 0: none */
} odin3_prov_request;

/* The request in force for design (design may be NULL: the CLI's alone). */
odin3_prov_request odin3_prov_depth_request(const odin3_design *design);

/* `--prov-level` value: 0..4 or auto. False (and nothing written) for anything else. */
bool odin3_prov_depth_parse_level(const char *text, bool *autolevel, uint32_t *level);

/* `--prov-budget` value: a positive integer with an optional K, M or G suffix (powers of 1024);
 * false for anything else. */
bool odin3_prov_depth_parse_size(const char *text, uint64_t *bytes);

/* Size of a design in the units the estimate uses. */
typedef struct odin3_prov_shape {
    uint64_t nodes; /* live nodes (cells) */
    uint64_t pins;  /* their pins */
} odin3_prov_shape;

odin3_prov_shape odin3_prov_depth_measure(odin3_design *design);

/* The predicted output of each depth. */
typedef struct odin3_prov_estimate {
    uint64_t records[ODIN3_PROV_DEPTH_COUNT]; /* R lines of the name map */
    uint64_t edges[ODIN3_PROV_DEPTH_COUNT];   /* parent edges on those lines */
    uint64_t map_bytes[ODIN3_PROV_DEPTH_COUNT];
    uint64_t step_bytes[ODIN3_PROV_DEPTH_COUNT]; /* odin3-flow's prov/ step files */
} odin3_prov_estimate;

void odin3_prov_depth_estimate(const odin3_prov_shape *shape, odin3_prov_estimate *out);

/* Bytes depth writes in all (name map plus step files). */
uint64_t odin3_prov_estimate_total(const odin3_prov_estimate *est, uint32_t depth);

/*
 * The depth in force, or -1 when neither a depth nor a budget is requested (today's behaviour).
 * `auto` and a budget are resolved against the estimate of the design as it is now, once per
 * design and request: the first call (proc's end, `prov-estimate`, or abc) decides and logs; a
 * fixed depth over the budget drops to the highest that fits with a WARNING naming both sizes.
 */
int odin3_prov_depth_effective(odin3_design *design);

/*
 * The depth asked for, before any estimate: -1 for none, `auto` as 4. read_verilog uses it for the
 * construct table (the read comes before proc, so there is nothing to estimate yet; a later drop
 * only means the table was kept for nothing).
 */
int odin3_prov_depth_requested(const odin3_design *design);

/* True when the depth in force (odin3_prov_depth_effective) writes the name map. */
bool odin3_prov_depth_wants_map(odin3_design *design);

/* Forgets the resolved depth (a new request, or a test). */
void odin3_prov_depth_reset(void);

/* proc's last step: the INFO line (only when a depth or budget is requested). */
void odin3_prov_depth_after_proc(odin3_design *design);

#endif
