/*
 * lower_latchref.c — the reference of the word latches (lower_latchref.h), written from the simlib
 * text of the cells (`ff.c` of the registry, Yosys 0.55 simlib): bit by bit, the strongest control
 * wins (clear, then set, then reset), then the enable decides between D and the old value.
 */
#include "lower_latchref.h"

#include <stddef.h>

const char *ltref_name(ltref_kind kind) {
    static const char *const k_names[LTREF_KIND_COUNT] = {"$dlatch", "$adlatch", "$dlatchsr",
                                                          "$sr"};
    return k_names[kind];
}

bool ltref_has_en(ltref_kind kind) {
    return kind != LTREF_SR;
}

bool ltref_has_arst(ltref_kind kind) {
    return kind == LTREF_ADLATCH;
}

bool ltref_has_setclr(ltref_kind kind) {
    return kind == LTREF_DLATCHSR || kind == LTREF_SR;
}

static uint64_t mask_of(uint32_t width) {
    return width >= LTREF_MAX_WIDTH ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1U);
}

/* The new value of one bit. */
static bool bit_step(const ltref_cfg *cfg, const ltref_in *in, uint32_t bit, bool old) {
    bool clr = (((in->clr >> bit) & 1U) != 0) == cfg->clr_high;
    bool set = (((in->set >> bit) & 1U) != 0) == cfg->set_high;
    bool enabled = !ltref_has_en(cfg->kind) || in->en == cfg->en_high;
    bool data = ((in->data >> bit) & 1U) != 0;
    if (ltref_has_setclr(cfg->kind) && clr) {
        return false;
    }
    if (ltref_has_setclr(cfg->kind) && set) {
        return true;
    }
    if (ltref_has_arst(cfg->kind) && in->arst == cfg->arst_high) {
        return ((cfg->value >> bit) & 1U) != 0;
    }
    if (cfg->kind == LTREF_SR) {
        return old;
    }
    return enabled ? data : old;
}

uint64_t ltref_step(const ltref_cfg *cfg, uint64_t state, const ltref_in *in) {
    uint64_t next = 0;
    for (uint32_t bit = 0; bit < cfg->width && bit < LTREF_MAX_WIDTH; bit++) {
        bool old = ((state >> bit) & 1U) != 0;
        next |= bit_step(cfg, in, bit, old) ? (uint64_t)1 << bit : 0U;
    }
    return next & mask_of(cfg->width);
}
