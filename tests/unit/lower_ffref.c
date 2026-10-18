/*
 * lower_ffref.c — the reference stepper (lower_ffref.h). Written from the simlib text of the cells
 * (`ff.c` of the registry, Yosys 0.55 simlib), in two phases that the pass does not have: first the
 * asynchronous control forces some bits at once; then the active edge loads the others.
 */
#include "lower_ffref.h"

#include <stddef.h>

static uint64_t mask_of(uint32_t width) {
    return width >= FFREF_MAX_WIDTH ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1U);
}

const char *ffref_name(ffref_kind kind) {
    static const char *const k_names[FFREF_KIND_COUNT] = {"$dff",    "$dffe",  "$sdff",  "$sdffe",
                                                          "$sdffce", "$adff",  "$adffe", "$aldff",
                                                          "$aldffe", "$dffsr", "$dffsre"};
    return k_names[kind];
}

bool ffref_is_async(ffref_kind kind) {
    return kind >= FFREF_ADFF;
}

bool ffref_has_en(ffref_kind kind) {
    return kind == FFREF_DFFE || kind == FFREF_SDFFE || kind == FFREF_SDFFCE ||
           kind == FFREF_ADFFE || kind == FFREF_ALDFFE || kind == FFREF_DFFSRE;
}

/* The bits an asynchronous control forces at once, and their values. */
typedef struct forced {
    uint64_t mask;
    uint64_t val;
} forced;

static forced async_force(const ffref_cfg *cfg, const ffref_in *in) {
    uint64_t all = mask_of(cfg->width);
    bool ctl_active = in->ctl == cfg->ctl_high;
    forced out = {0, 0};
    if (cfg->kind == FFREF_ADFF || cfg->kind == FFREF_ADFFE) {
        out.mask = ctl_active ? all : 0;
        out.val = cfg->value & all;
    } else if (cfg->kind == FFREF_ALDFF || cfg->kind == FFREF_ALDFFE) {
        out.mask = ctl_active ? all : 0;
        out.val = in->load & all;
    } else if (cfg->kind == FFREF_DFFSR || cfg->kind == FFREF_DFFSRE) {
        uint64_t clr = (cfg->clr_high ? in->clr : ~in->clr) & all;
        uint64_t set = (cfg->set_high ? in->set : ~in->set) & all;
        out.mask = clr | set;
        out.val = set & ~clr; /* clear wins over set (simlib) */
    }
    return out;
}

/* $sdff, $sdffe: SRST over the enable. */
static uint64_t sync_reset_first(const ffref_cfg *cfg, const ffref_in *in, uint64_t state) {
    uint64_t all = mask_of(cfg->width);
    bool enabled = cfg->kind == FFREF_SDFF || in->en == cfg->en_high;
    if (in->ctl == cfg->ctl_high) {
        return cfg->value & all;
    }
    return enabled ? in->data & all : state;
}

/* What the active edge loads into a bit that is not forced: state is the old value. */
static uint64_t edge_value(const ffref_cfg *cfg, const ffref_in *in, uint64_t state) {
    uint64_t all = mask_of(cfg->width);
    bool enabled = !ffref_has_en(cfg->kind) || in->en == cfg->en_high;
    if (cfg->kind == FFREF_SDFF || cfg->kind == FFREF_SDFFE) {
        return sync_reset_first(cfg, in, state);
    }
    if (cfg->kind == FFREF_SDFFCE && in->ctl == cfg->ctl_high) {
        return enabled ? cfg->value & all : state; /* EN over SRST */
    }
    return enabled ? in->data & all : state;
}

uint64_t ffref_step(const ffref_cfg *cfg, uint64_t state, const ffref_in *in) {
    uint64_t all = mask_of(cfg->width);
    forced force = async_force(cfg, in);
    if (cfg->enable_first && ffref_is_async(cfg->kind) && ffref_has_en(cfg->kind)) {
        /* Parmys's adffe2dff / aldffe2dff shape: Q <= EN ? (control ? forced : D) : Q */
        bool enabled = in->en == cfg->en_high;
        uint64_t loaded = (in->data & all & ~force.mask) | (force.val & force.mask);
        return enabled ? loaded : (state & all);
    }
    uint64_t loaded = edge_value(cfg, in, state & all);
    return ((loaded & ~force.mask) | (force.val & force.mask)) & all;
}
