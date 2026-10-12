/*
 * abc.c — the public ABI's ABC engine registration (3D spec §4.3, AB-7; plan point (b)):
 * odin3_abc_engine_register validates the table, logging the first fault, and hands it to
 * src/passes/abc/engine.c, which keeps the process's copy.
 */
#include "odin3/odin3.h"
#include "passes/abc/engine.h"
#include "util/attr.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>

enum { ABC_COMMIT_DIGITS = 40, ABC_ENGINE_FUNCTIONS = 6 };

/* abc_commit: exactly ABC_COMMIT_DIGITS lower-case hex digits (reads at most one byte past them).
 */
static bool abc_commit_ok(const char *commit) {
    if (commit == NULL) {
        return false;
    }
    for (size_t i = 0; i < ABC_COMMIT_DIGITS; i++) {
        char digit = commit[i];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return false;
        }
    }
    return commit[ABC_COMMIT_DIGITS] == '\0';
}

/* The first of the six functions, in table order, that is NULL; NULL when all are set. */
static const char *missing_function(const odin3_abc_engine *engine) {
    const struct {
        bool set;
        const char *name;
    } fns[ABC_ENGINE_FUNCTIONS] = {
        {engine->open != NULL, "open"},         {engine->load_aig != NULL, "load_aig"},
        {engine->exec != NULL, "exec"},         {engine->get_luts != NULL, "get_luts"},
        {engine->name_map != NULL, "name_map"}, {engine->close != NULL, "close"},
    };
    for (size_t i = 0; i < ABC_ENGINE_FUNCTIONS; i++) {
        if (!fns[i].set) {
            return fns[i].name;
        }
    }
    return NULL;
}

/* Logs the table's first fault after the version and the name, in point (b)'s order. */
static odin3_status check_table(const odin3_abc_engine *engine) {
    const char *missing = missing_function(engine);
    if (missing != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_engine_register: engine '%s' has no '%s' function",
                  engine->name, missing);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof engine->reserved / sizeof engine->reserved[0]; i++) {
        if (engine->reserved[i] != NULL) {
            odin3_log(ODIN3_LOG_ERROR,
                      "odin3_abc_engine_register: engine '%s': reserved[%zu] must be NULL",
                      engine->name, i);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    if (!abc_commit_ok(engine->abc_commit)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_abc_engine_register: engine '%s': abc_commit '%s' is not 40 lower-case "
                  "hex digits",
                  engine->name, engine->abc_commit != NULL ? engine->abc_commit : "");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status invalid_engine(void) {
    odin3_log(ODIN3_LOG_ERROR, "odin3_abc_engine_register: invalid argument engine");
    return ODIN3_ERR_INVALID_ARG;
}

ODIN3_EXPORT odin3_status odin3_abc_engine_register(const odin3_abc_engine *engine) {
    if (engine == NULL) {
        return invalid_engine();
    }
    if (engine->version != (uint32_t)ODIN3_ABC_ENGINE_VERSION) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_engine_register: engine version %u, expected %d",
                  (unsigned)engine->version, ODIN3_ABC_ENGINE_VERSION);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (engine->name == NULL) {
        return invalid_engine(); /* every later text names the engine */
    }
    odin3_status st = check_table(engine);
    if (st != ODIN3_OK) {
        return st;
    }
    return odin3_abc_engine_set(engine); /* logs "abc engine already registered (<name>)" */
}
