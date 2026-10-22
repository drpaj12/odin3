/*
 * sim_abi.c — the simulation ABI (S1 spec section 3, SA-1 to SA-11): odin3_simulation_* over the
 * internal simulator (src/sim/sim.h) and its signal table (src/sim/signals.h).
 */
#include "api/api.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert((uint32_t)ODIN3_SIMULATION_INPUT == (uint32_t)ODIN3_SIM_KIND_INPUT, "kind INPUT");
_Static_assert((uint32_t)ODIN3_SIMULATION_CLOCK == (uint32_t)ODIN3_SIM_KIND_CLOCK, "kind CLOCK");
_Static_assert((uint32_t)ODIN3_SIMULATION_OUTPUT == (uint32_t)ODIN3_SIM_KIND_OUTPUT, "kind OUTPUT");
_Static_assert((uint32_t)ODIN3_SIMULATION_INTERNAL == (uint32_t)ODIN3_SIM_KIND_INTERNAL,
               "kind INTERNAL");

/* The widest signal odin3_simulation_set and odin3_simulation_get take (one uint64_t). */
enum { SIM_ABI_WORD_BITS = 64 };

struct odin3_simulation {
    odin3_design *design;
    odin3_sim *sim;
    odin3_sim_signals *signals;
    uint64_t cycle;
};

/* The top when module is 0; ID 0 (logged) when there is none or module is not a module. */
static odin3_module_id open_module(const odin3_design *design, uint32_t module) {
    if (module == 0) {
        odin3_module_id top = odin3_design_top(design);
        if (top.v == 0) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_simulation_open: the design has no top module");
        }
        return top;
    }
    if (module >= odin3_design_module_end(design)) {
        (void)odin3_api_invalid("odin3_simulation_open");
        return (odin3_module_id){0};
    }
    return (odin3_module_id){module};
}

/* Builds the simulator and its signal table into sim; on failure frees what it built. */
static odin3_status open_into(odin3_simulation *sim, odin3_module_id top) {
    odin3_status st = odin3_sim_build(sim->design, top, &sim->sim);
    if (st == ODIN3_OK) {
        st = odin3_sim_signals_create(sim->sim, &sim->signals);
    }
    if (st == ODIN3_ERR_NO_MEMORY) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_simulation_open: out of memory");
    }
    if (st != ODIN3_OK) {
        odin3_sim_destroy(sim->sim);
        sim->sim = NULL;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_simulation_open(odin3_design *design, uint32_t module,
                                                odin3_simulation **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (design == NULL || out == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_module_id top = open_module(design, module);
    if (top.v == 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_design_refuse_processes(design, NULL);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_simulation *sim = odin3_util_calloc(sizeof *sim);
    if (sim == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_simulation_open: out of memory");
        return ODIN3_ERR_NO_MEMORY;
    }
    sim->design = design;
    st = open_into(sim, top);
    if (st != ODIN3_OK) {
        odin3_util_free(sim);
        return st;
    }
    odin3_design_sim_hold(design);
    *out = sim;
    return ODIN3_OK;
}

ODIN3_EXPORT void odin3_simulation_close(odin3_simulation *sim) {
    if (sim == NULL) {
        return;
    }
    odin3_sim_signals_destroy(sim->signals);
    odin3_sim_destroy(sim->sim);
    odin3_design_sim_release(sim->design);
    odin3_util_free(sim);
}

ODIN3_EXPORT odin3_status odin3_simulation_get_scope_count(const odin3_simulation *sim,
                                                           uint32_t *count) {
    if (sim == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_sim_signals_scope_count(sim->signals);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_get_scope(const odin3_simulation *sim, uint32_t scope,
                                                     odin3_simulation_scope *info) {
    if (sim == NULL || info == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_sim_scope *item = odin3_sim_signals_scope(sim->signals, scope);
    if (item == NULL || scope >= sim->sim->n_frames) {
        return odin3_api_invalid_why(__func__, "unknown scope");
    }
    const odin3_sim_frame *frame = &sim->sim->frames[scope];
    info->name = odin3_sim_signals_text(sim->signals, item->name);
    info->leaf = odin3_sim_signals_text(sim->signals, item->leaf);
    info->parent = item->parent;
    info->module = frame->module.v;
    info->inst = (odin3_ref){0, 0};
    if (scope != 0) {
        info->inst.module = sim->sim->frames[frame->parent].module.v;
        info->inst.id = frame->inst.v;
    }
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_get_signal_count(const odin3_simulation *sim,
                                                            uint32_t *count) {
    if (sim == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_sim_signals_listed(sim->signals);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_get_signal(const odin3_simulation *sim, uint32_t signal,
                                                      odin3_simulation_signal *info) {
    if (sim == NULL || info == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_sim_signal *item = odin3_sim_signals_at(sim->signals, signal);
    if (item == NULL) {
        return odin3_api_invalid_why(__func__, "unknown signal");
    }
    info->name = odin3_sim_signals_text(sim->signals, item->name);
    info->leaf = odin3_sim_signals_text(sim->signals, item->leaf);
    info->width = item->width;
    info->kind = item->kind;
    info->msb = item->msb;
    info->lsb = item->lsb;
    info->scope = item->scope;
    info->obj = (odin3_obj){sim->sim->frames[item->scope].module.v, item->obj_kind, item->obj_id};
    info->first_bit = item->first_bit;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_find_signal(odin3_simulation *sim, const char *name,
                                                       uint32_t *signal) {
    if (sim == NULL || name == NULL || signal == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_status st = odin3_sim_signals_find(sim->signals, odin3_bytes_cstr(name), signal);
    if (st == ODIN3_ERR_NO_MEMORY) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_simulation_find_signal: out of memory");
    }
    return st;
}

/* Writes bits through the signal table; logs the table's reason on a refusal. */
static odin3_status write_bits(odin3_simulation *sim, uint32_t signal, const uint8_t *bits,
                               uint32_t count, const char *fn) {
    const char *why = NULL;
    odin3_status st = odin3_sim_signals_write(sim->signals, signal, bits, count, &why);
    return st == ODIN3_OK ? ODIN3_OK : odin3_api_invalid_why(fn, why);
}

/* set (SA-7) of item, the signal with ID signal: checks the word, then writes its bits. */
static odin3_status set_word(odin3_simulation *sim, uint64_t value, const odin3_sim_signal *item,
                             uint32_t signal) {
    static const char *const fn = "odin3_simulation_set";
    if (item == NULL) {
        return odin3_api_invalid_why(fn, "unknown signal");
    }
    if (item->kind != ODIN3_SIM_KIND_INPUT) {
        return odin3_api_invalid_why(fn, "not an input");
    }
    if (item->width > SIM_ABI_WORD_BITS) {
        return odin3_api_invalid_why(fn, "width over 64");
    }
    if (item->width < SIM_ABI_WORD_BITS && (value >> item->width) != 0) {
        return odin3_api_invalid_why(fn, "value wider than the signal");
    }
    uint8_t bits[SIM_ABI_WORD_BITS];
    for (uint32_t k = 0; k < item->width; k++) {
        bits[k] = (uint8_t)((value >> k) & 1U);
    }
    return write_bits(sim, signal, bits, item->width, fn);
}

ODIN3_EXPORT odin3_status odin3_simulation_set(odin3_simulation *sim, uint32_t signal,
                                               uint64_t value) {
    if (sim == NULL) {
        return odin3_api_invalid(__func__);
    }
    return set_word(sim, value, odin3_sim_signals_at(sim->signals, signal), signal);
}

ODIN3_EXPORT odin3_status odin3_simulation_set_bits(odin3_simulation *sim, uint32_t signal,
                                                    const uint8_t *bits, uint32_t count) {
    if (sim == NULL || bits == NULL) {
        return odin3_api_invalid(__func__);
    }
    return write_bits(sim, signal, bits, count, __func__);
}

ODIN3_EXPORT odin3_status odin3_simulation_get(const odin3_simulation *sim, uint32_t signal,
                                               uint64_t *value) {
    if (sim == NULL || value == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_sim_signal *item = odin3_sim_signals_at(sim->signals, signal);
    if (item == NULL) {
        return odin3_api_invalid_why(__func__, "unknown signal");
    }
    if (item->width > SIM_ABI_WORD_BITS) {
        return odin3_api_invalid_why(__func__, "width over 64");
    }
    uint8_t bits[SIM_ABI_WORD_BITS];
    odin3_status st = odin3_sim_signals_read(sim->signals, signal, bits, item->width);
    if (st != ODIN3_OK) {
        return odin3_api_invalid(__func__);
    }
    uint64_t word = 0;
    for (uint32_t k = 0; k < item->width; k++) {
        word |= (uint64_t)(bits[k] & 1U) << k;
    }
    *value = word;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_get_bits(const odin3_simulation *sim, uint32_t signal,
                                                    uint8_t *bits, uint32_t count) {
    if (sim == NULL || bits == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_sim_signal *item = odin3_sim_signals_at(sim->signals, signal);
    if (item == NULL) {
        return odin3_api_invalid_why(__func__, "unknown signal");
    }
    if (count != item->width) {
        return odin3_api_invalid_why(__func__, "count is not the width");
    }
    odin3_status st = odin3_sim_signals_read(sim->signals, signal, bits, count);
    return st == ODIN3_OK ? ODIN3_OK : odin3_api_invalid(__func__);
}

ODIN3_EXPORT odin3_status odin3_simulation_settle(odin3_simulation *sim) {
    if (sim == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_sim_settle(sim->sim);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_step(odin3_simulation *sim, uint32_t cycles) {
    if (sim == NULL) {
        return odin3_api_invalid(__func__);
    }
    for (uint32_t k = 0; k < cycles; k++) {
        odin3_sim_cycle(sim->sim);
    }
    sim->cycle += cycles;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_get_cycle(const odin3_simulation *sim, uint64_t *cycle) {
    if (sim == NULL || cycle == NULL) {
        return odin3_api_invalid(__func__);
    }
    *cycle = sim->cycle;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_simulation_reset(odin3_simulation *sim) {
    if (sim == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_sim_reset(sim->sim);
    sim->cycle = 0;
    return ODIN3_OK;
}
