/*
 * engine.h — the process's ABC engine (3D spec §4.3, AB-7; plan point (b)). The public
 * odin3_abc_engine_register (src/api/abc.c) validates a table and stores it here; the abc pass
 * reads it through odin3_abc_engine_get. Process-wide and not thread-safe, as the ABI says.
 */
#ifndef ODIN3_PASSES_ABC_ENGINE_H
#define ODIN3_PASSES_ABC_ENGINE_H

#include "odin3/odin3.h"

/*
 * The engine the abc pass uses: the test override (odin3_abc_test_set_engine) when one is set,
 * else the registered copy, else NULL (no engine). The table is owned here and lives until the
 * process ends (until the next override for an override). Never fails, never logs.
 */
const odin3_abc_engine *odin3_abc_engine_get(void);

/*
 * Stores a copy of *engine (the strings and self are not copied: they must outlive the process)
 * as the registered engine, which is final. engine must be a validated table (the caller checks
 * it). ODIN3_ERR_INVALID_ARG for a NULL engine or when an engine is already registered; nothing is
 * stored then. Never logs: the caller reports the failure.
 */
odin3_status odin3_abc_engine_set(const odin3_abc_engine *engine);

/* The registered engine's name (never the test override's), or NULL when none is registered. */
const char *odin3_abc_engine_registered_name(void);

#endif
