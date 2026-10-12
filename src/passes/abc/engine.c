/*
 * engine.c — the process's ABC engine: the registered copy and the test override (engine.h,
 * abc_test.h).
 */
#include "passes/abc/engine.h"
#include "passes/abc/abc_test.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>

static odin3_abc_engine registered;
static bool have_registered;
static odin3_abc_engine override;
static bool have_override;

const odin3_abc_engine *odin3_abc_engine_get(void) {
    if (have_override) {
        return &override;
    }
    return have_registered ? &registered : NULL;
}

odin3_status odin3_abc_engine_set(const odin3_abc_engine *engine) {
    if (engine == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (have_registered) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_engine_register: abc engine already registered (%s)",
                  registered.name);
        return ODIN3_ERR_INVALID_ARG;
    }
    registered = *engine;
    have_registered = true;
    return ODIN3_OK;
}

void odin3_abc_test_set_engine(const odin3_abc_engine *engine) {
    have_override = engine != NULL;
    if (have_override) {
        override = *engine;
    }
}
