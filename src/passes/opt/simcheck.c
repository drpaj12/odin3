/*
 * simcheck.c — --opt-simcheck: the in-process simulation check of 2F spec §20.2 (plan Task 17,
 * point (m)). Around one sub-pass call, 1E's simulator runs every module that passes the predicate
 * below for SIMCHECK_CYCLES cycles on each of the seeds 1 .. SIMCHECK_SEEDS, before the call and
 * after it; the outputs of every cycle are recorded, so the "before copy" is a trace and the
 * design is never copied (the IR has no module copy path). A trace that differs is a sub-pass that
 * changed what the module computes: ODIN3_ERR_CHECK "opt: <sub>: simulation differs on module
 * 'm' at cycle K".
 *
 * The simulator is 2-state, so the check is sound only where the sub-pass must preserve every
 * output exactly. A module is skipped (before and after alike) when it
 *   - holds a $proc, an instance of a module, or a cell type with no simulate hook ($shiftx, $pow,
 *     the word-level flip-flops);
 *   - holds an x or z constant ($_CONSTX_, $_CONSTZ_: an x in the input is a don't-care);
 *   - holds a flip-flop or latch whose INIT is 2 or 3 or that has an `init` on its Q net (the
 *     simulator starts those at 0, where the refinement may pick either value);
 *   - holds a $pmux whose S is wider than 1: opt_share is a refinement only where the selects are
 *     exclusive (spec 17 #46), and random inputs drive them anywhere; skipping is spec 20.2's
 *     "or skip a module holding a $pmux whose selects are not provably exclusive";
 *   - does not build (a clock that is not a primary input, two drivers, a loop, an inout port): the
 *     build's errors are muted.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/opt/opt.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    SIMCHECK_CYCLES = 64,
    SIMCHECK_SEEDS = 3,
    SIMCHECK_MAX_BYTES = 1 << 26, /* one module's trace: more output bits than this skips it */
    PMUX_S_PORT = 2,              /* $pmux's ports are A, B, S, Y */
    INIT_DONT_CARE = 2,           /* INIT 2 (don't care) and 3 (unknown) */
    INIT_UNKNOWN = 3
};

typedef struct simcheck_trace {
    odin3_module_id module;
    uint32_t n_out; /* output bits per cycle */
    uint8_t *bits;  /* SEEDS x CYCLES x n_out, one byte per bit */
} simcheck_trace;

struct odin3_opt_simcheck {
    odin3_vec traces; /* simcheck_trace, one per checked module */
};

/* --- the predicate --------------------------------------------------------------------------- */

static bool is_x_const(const odin3_celltype_def *def) {
    return strcmp(def->name, "$_CONSTX_") == 0 || strcmp(def->name, "$_CONSTZ_") == 0;
}

static bool is_wide_pmux(const odin3_celltype_def *def, const odin3_module *mod,
                         odin3_node_id node) {
    return strcmp(def->name, "$pmux") == 0 && odin3_node_port(mod, node, PMUX_S_PORT).count > 1;
}

static bool has_dont_care_init(const odin3_celltype_def *def, const odin3_module *mod,
                               odin3_node_id node) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, "INIT") != 0) {
            continue;
        }
        const odin3_value *init = odin3_node_param(mod, node, i);
        return init != NULL && init->kind == ODIN3_VAL_INT &&
               (init->i == INIT_DONT_CARE || init->i == INIT_UNKNOWN);
    }
    return false;
}

/* A storage cell whose Q net carries an `init` bit the simulator does not read. */
static bool has_wire_init(const odin3_celltype_def *def, const odin3_module *mod,
                          odin3_node_id node) {
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) == 0) {
        return false;
    }
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_net_id net = odin3_pin_net(mod, pin);
        if (odin3_pin_dir(mod, pin) == ODIN3_DIR_OUT && odin3_net_valid(net) &&
            odin3_ffinit_get(mod, net) <= ODIN3_BIT_1) {
            return true;
        }
    }
    return false;
}

/* True when the node keeps its module out of the check. */
static bool node_blocks(const odin3_design *design, const odin3_module *mod, odin3_node_id node) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
    if (def == NULL) {
        return true;
    }
    if (def->gran == ODIN3_GRAN_PORT) {
        return false;
    }
    if (def->gran == ODIN3_GRAN_MODULE || (def->flags & ODIN3_CT_PROCESS) != 0 ||
        def->simulate == NULL) {
        return true;
    }
    return is_x_const(def) || is_wide_pmux(def, mod, node) || has_dont_care_init(def, mod, node) ||
           has_wire_init(def, mod, node);
}

static bool module_simulable(const odin3_design *design, const odin3_module *mod) {
    uint32_t end = odin3_module_node_end(mod);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && node_blocks(design, mod, node)) {
            return false;
        }
    }
    return true;
}

/* --- traces ---------------------------------------------------------------------------------- */

static uint32_t output_bits(const odin3_sim *sim) {
    uint32_t total = 0;
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        total += odin3_sim_output_width(sim, port);
    }
    return total;
}

/* One seed: CYCLES cycles of random inputs, the outputs after each into row. */
static void run_seed(odin3_sim *sim, uint32_t seed, uint8_t *row) {
    odin3_prng prng;
    odin3_prng_seed(&prng, seed);
    uint32_t at = 0;
    for (uint32_t cycle = 0; cycle < SIMCHECK_CYCLES; cycle++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
            for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
                bool value = false;
                (void)odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &value);
                row[at++] = value ? 1U : 0U;
            }
        }
    }
}

/* Builds the simulator quietly: INVALID_ARG (the build's own error, muted) means "skip". */
static odin3_status build_quiet(odin3_design *design, odin3_module_id id, odin3_sim **sim) {
    odin3_log_mute(true);
    odin3_status st = odin3_sim_build(design, id, sim);
    odin3_log_mute(false);
    return st;
}

/* Fills *out; *usable is false (and nothing is held) when the module cannot be simulated. */
static odin3_status trace_module(odin3_design *design, odin3_module_id id, simcheck_trace *out,
                                 bool *usable) {
    *out = (simcheck_trace){id, 0, NULL};
    *usable = false;
    for (uint32_t seed = 1; seed <= SIMCHECK_SEEDS; seed++) {
        odin3_sim *sim = NULL;
        odin3_status st = build_quiet(design, id, &sim);
        if (st == ODIN3_OK && out->bits == NULL) {
            out->n_out = output_bits(sim);
            uint64_t bytes = (uint64_t)out->n_out * SIMCHECK_CYCLES * SIMCHECK_SEEDS;
            if (bytes == 0 || bytes > SIMCHECK_MAX_BYTES) {
                odin3_sim_destroy(sim);
                return ODIN3_OK;
            }
            out->bits = odin3_util_calloc((size_t)bytes);
            st = out->bits != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        }
        if (st == ODIN3_OK) {
            size_t row = (size_t)out->n_out * SIMCHECK_CYCLES;
            run_seed(sim, seed, out->bits + (size_t)(seed - 1) * row);
        }
        odin3_sim_destroy(sim);
        if (st != ODIN3_OK) {
            odin3_util_free(out->bits);
            out->bits = NULL;
            return st == ODIN3_ERR_INVALID_ARG ? ODIN3_OK : st;
        }
    }
    *usable = true;
    return ODIN3_OK;
}

/* The first cycle (seed order, then cycle order) where the traces differ; false when equal. */
static bool first_difference(const simcheck_trace *old, const simcheck_trace *now,
                             uint32_t *cycle) {
    size_t width = old->n_out;
    for (uint32_t seed = 0; seed < SIMCHECK_SEEDS; seed++) {
        for (uint32_t at = 0; at < SIMCHECK_CYCLES; at++) {
            size_t off = ((size_t)seed * SIMCHECK_CYCLES + at) * width;
            if (memcmp(old->bits + off, now->bits + off, width) != 0) {
                *cycle = at;
                return true;
            }
        }
    }
    return false;
}

/* --- the API --------------------------------------------------------------------------------- */

void odin3_opt_simcheck_free(odin3_opt_simcheck *check) {
    if (check == NULL) {
        return;
    }
    for (size_t i = 0; i < check->traces.len; i++) {
        simcheck_trace *trace = odin3_vec_at(&check->traces, i);
        odin3_util_free(trace->bits);
    }
    odin3_vec_free(&check->traces);
    odin3_util_free(check);
}

odin3_status odin3_opt_simcheck_before(odin3_design *design, odin3_opt_simcheck **out) {
    if (design == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: invalid arguments to odin3_opt_simcheck_before");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = NULL;
    odin3_opt_simcheck *check = odin3_util_calloc(sizeof *check);
    if (check == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&check->traces, sizeof(simcheck_trace));
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        const odin3_module *mod = odin3_module_get(design, (odin3_module_id){id});
        simcheck_trace trace;
        bool usable = false;
        if (mod == NULL || !module_simulable(design, mod)) {
            continue;
        }
        st = trace_module(design, (odin3_module_id){id}, &trace, &usable);
        simcheck_trace *slot = st == ODIN3_OK && usable ? odin3_vec_push(&check->traces) : NULL;
        if (slot != NULL) {
            *slot = trace;
        } else if (usable) {
            odin3_util_free(trace.bits);
            st = ODIN3_ERR_NO_MEMORY;
        }
    }
    if (st != ODIN3_OK) {
        odin3_opt_simcheck_free(check);
        return st;
    }
    *out = check;
    return ODIN3_OK;
}

/* One module after the call: ODIN3_ERR_CHECK (logged) when its outputs differ from before. */
static odin3_status compare_module(odin3_design *design, const simcheck_trace *old,
                                   const char *sub) {
    const odin3_module *mod = odin3_module_get(design, old->module);
    simcheck_trace now;
    bool usable = false;
    if (mod == NULL || !module_simulable(design, mod)) {
        return ODIN3_OK; /* the call made it a module the check cannot judge */
    }
    odin3_status st = trace_module(design, old->module, &now, &usable);
    uint32_t cycle = 0;
    if (st == ODIN3_OK && usable && now.n_out == old->n_out && first_difference(old, &now, &cycle)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: %s: simulation differs on module '%s' at cycle %u", sub,
                  odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(mod)),
                  (unsigned)cycle);
        st = ODIN3_ERR_CHECK;
    }
    odin3_util_free(now.bits);
    return st;
}

odin3_status odin3_opt_simcheck_after(odin3_design *design, const odin3_opt_simcheck *before,
                                      const char *sub) {
    if (design == NULL || before == NULL || sub == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: invalid arguments to odin3_opt_simcheck_after");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < before->traces.len; i++) {
        st = compare_module(design, odin3_vec_cat(&before->traces, i), sub);
    }
    return st;
}
