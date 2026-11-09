/*
 * act_int.h — private declarations of the activity pass (spec
 * docs/specs/2026-11-08-activity-design.md): the model of one module (nets, function cells, storage
 * cells), the propagation and the primary input statistics.
 */
#ifndef ODIN3_PASSES_ACTIVITY_ACT_INT_H
#define ODIN3_PASSES_ACTIVITY_ACT_INT_H

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ACT_MAX_INPUTS = 16, ACT_DEFAULT_ITER = 1000, ACT_DAMP_FROM = 200 };

typedef enum act_kind {
    ACT_UNUSED, /* no pin at all */
    ACT_PI,
    ACT_CLOCK, /* primary input that is a clock */
    ACT_CONST,
    ACT_COMB,
    ACT_FF,
    ACT_OPAQUE,
    ACT_UNDRIVEN,
    ACT_NKINDS
} act_kind;

typedef struct act_opts {
    double pi_prob, pi_density;
    double opaque_prob, opaque_density;
    double ff_start;
    bool ff_init;
    double tol;
    uint32_t max_iter;
    uint32_t exact; /* widest function treated exactly */
    odin3_strbuf out, json, pi_file, vcd, vcd_scope, vcd_clock, clock;
    odin3_strbuf opaque_types; /* names of cell types forced opaque, one per line */
    int name_style;            /* odin3_blif_name_style */
    bool attrs;
    bool have_density; /* --pi-density given */
} act_opts;

/* One function cell: out = f(inputs). A storage cell is the same with its own state net among
 * the inputs when its next state depends on it (feedback = true). */
typedef struct act_cell {
    uint32_t out;
    uint32_t in_off;  /* into the input pool */
    uint32_t tab_off; /* into the table pool: 2^n_in bytes */
    uint8_t n_in;
    uint8_t is_ff;
    uint8_t q_index; /* FF: index of the own state among the inputs, 0xFF none */
    uint8_t loop;    /* FF: in a feedback component */
} act_cell;

typedef struct act_model {
    odin3_module *mod;
    uint32_t n_nets; /* net IDs are 1 .. n_nets - 1 */
    uint8_t *kind;   /* act_kind per net */
    uint8_t *clock;  /* per net: drives a clock pin */
    double *prob, *dens;
    act_cell *cells;
    size_t n_cells, cap_cells;
    odin3_vec in_pool;  /* uint32_t */
    odin3_vec tab_pool; /* uint8_t */
    odin3_vec opaque_types;
    uint64_t opaque_cells, wide_cells, latches, ff_loop, undriven, instances, cone_capped;
    uint32_t iterations; /* total sweeps over feedback components */
    bool converged;
    uint32_t ref_clock; /* net, 0 none */
    uint64_t hash;
} act_model;

odin3_status act_model_build(act_model *md, odin3_module *mod, const act_opts *opts);
void act_model_free(act_model *md);

/* act_prop.c: evaluation of one function cell. */
typedef struct act_stat {
    double prob, dens;
} act_stat;

typedef struct act_fn_ref {
    const double *prob; /* per net */
    const double *dens; /* per net; NULL when only the probability is wanted */
    const uint32_t *ins;
    const uint8_t *tab;
    double *scratch; /* at least 2 * 2^n_in doubles */
    uint32_t n_in;
} act_fn_ref;

/* A closed interval. */
typedef struct act_range {
    double low, high;
} act_range;

double act_clamp(double val, act_range range);

/* Two-frame evaluation (spec 3.1): P1 and density of the output. */
act_stat act_eval_pair(const act_fn_ref *fn);

/* P1 of the output alone. */
double act_eval_prob(const act_fn_ref *fn);

/* act_solve.c: propagation of the whole model; fills prob and dens of every non-source net. */
odin3_status act_solve(act_model *md, const act_opts *opts);

/* act_vcd.c: primary input statistics of a waveform into model nets named by name_of. */
typedef bool (*act_name_fn)(void *ctx, uint32_t net, odin3_strbuf *out);
odin3_status act_read_pi_file(act_model *md, const char *path, act_name_fn name_of, void *ctx);
odin3_status act_read_vcd(act_model *md, const act_opts *opts, act_name_fn name_of, void *ctx);

/* act_out.c: the naming of nets as write_blif --vtr names them, and the outputs. */
typedef struct odin3_sim_netnames odin3_sim_netnames;
typedef struct act_namer {
    odin3_sim_netnames *nn;
    const odin3_module *mod;
    odin3_module_id mid;
} act_namer;

bool act_name_net(void *ctx, uint32_t net, odin3_strbuf *out);
odin3_status act_write_act(const act_model *md, act_namer *nm, const char *path);
odin3_status act_write_json(const act_model *md, act_namer *nm, const act_opts *opts,
                            const char *path, double ms);
odin3_status act_write_attrs(const act_model *md, const act_opts *opts);

#endif
