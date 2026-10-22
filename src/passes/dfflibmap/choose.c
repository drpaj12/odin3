/*
 * choose.c — dfflibmap's state cell choice (gatemap spec 6; plan Task 8): the trigger of the
 * kind, the init rule, the smallest numeric area, then library order; the inverted-clock fallback
 * of a $_DFF_N_.
 */
#include "passes/dfflibmap/dfflibmap.h"

#include "ir/design.h"
#include "techlib/libcells.h"
#include "techlib/reader.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The INIT values (BLIF .latch) that take any library init: don't care and unknown. */
enum { INIT_DONT_CARE = 2, INIT_UNKNOWN = 3 };

/* The library init of a cell whose init is x (odin3_libstate.init). */
enum { LIB_INIT_X = -1 };

/* True when a library cell of init lib_init can implement a cell whose INIT is init. */
static bool init_ok(int lib_init, int64_t init) {
    return lib_init == LIB_INIT_X || init == INIT_DONT_CARE || init == INIT_UNKNOWN ||
           (int64_t)lib_init == init;
}

/* True when a cell of area cand_area beats the best so far (area best_area; strtab IDs): a
 * numeric area beats none, a smaller one beats a larger; ties keep the earlier cell. */
static bool better(const odin3_design *design, uint32_t cand_area, uint32_t best_area) {
    double cand = 0.0;
    double best = 0.0;
    if (!odin3_libcells_area(design, cand_area, &cand)) {
        return false;
    }
    return !odin3_libcells_area(design, best_area, &best) || cand < best;
}

/* The best compatible state cell of trigger trigger; false when there is none. */
static bool pick(const odin3_design *design, const odin3_libcells *cells,
                 odin3_techlib_trigger trigger, int64_t init, uint32_t *out) {
    bool found = false;
    uint32_t best_area = 0;
    for (size_t i = 0; i < cells->states.len; i++) {
        const odin3_libstate *state = odin3_vec_cat(&cells->states, i);
        if (state->trigger != trigger || !init_ok(state->init, init)) {
            continue;
        }
        if (!found || better(design, state->area, best_area)) {
            found = true;
            best_area = state->area;
            *out = (uint32_t)i;
        }
    }
    return found;
}

bool odin3_dlm_choose(const odin3_design *design, const odin3_libcells *cells, odin3_dlm_kind kind,
                      int64_t init, odin3_dlm_choice *out) {
    static const odin3_techlib_trigger k_trigger[ODIN3_DLM_KINDS] = {
        ODIN3_TECHLIB_POSEDGE, ODIN3_TECHLIB_NEGEDGE, ODIN3_TECHLIB_HIGH, ODIN3_TECHLIB_LOW};
    if (design == NULL || cells == NULL || out == NULL || (unsigned)kind >= ODIN3_DLM_KINDS) {
        return false;
    }
    uint32_t state = 0;
    if (pick(design, cells, k_trigger[kind], init, &state)) {
        *out = (odin3_dlm_choice){state, false};
        return true;
    }
    if (kind == ODIN3_DLM_DFF_N && cells->inv.v != 0 &&
        pick(design, cells, ODIN3_TECHLIB_POSEDGE, init, &state)) {
        *out = (odin3_dlm_choice){state, true};
        return true;
    }
    return false;
}
