/*
 * abc_fake_engine.h — the test double of the ABC engine (3D plan point (t), §13.1): an
 * odin3_abc_engine that understands the BLIF exchange's commands over a tiny AIG, deterministic,
 * linked into the test_abc_* executables. Pure test code; it never links ABC.
 *
 * BLIF mode: `read "<in>"` builds the AIG from the BLIF with 1C's reader (each $sop row an AND
 * tree, a `.latch` a register keeping its name); `write_hie "<in>" "<out>"` writes the network as
 * BLIF: every AND as a 2-input LUT `.names` through odin3_blif_isop (an inverter LUT for each
 * complemented output or register input), registers as `.latch <d> <q> 0` under their names, each
 * output as a `1 1` buffer from its driver. The commands: `&put`, `&get` and `strash` are no-ops;
 * `if -K k` maps (every AND becomes a 2-input LUT); `scorr` merges registers with equal D (the
 * lowest keeps its name) and replaces a register whose D is its own Q or constant 0 by constant 0;
 * `const <i> [1]` makes LUT i of the next write constant 0 (1 with the second argument); anything
 * else is a no-op. Each command also sends the line `fake: <command>` to the text callback.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_FAKE_ENGINE_H
#define ODIN3_TESTS_UNIT_ABC_FAKE_ENGINE_H

#include "odin3/odin3.h"

#include <stdint.h>

/* The engine table (stable address); pass it to odin3_abc_test_set_engine. */
const odin3_abc_engine *odin3_abc_fake_engine(void);

/* Clears the AIG, the counters, the recorded commands and the failure knob. */
void odin3_abc_fake_reset(void);

/* How many times open was called since the last reset. */
uint32_t odin3_abc_fake_open_count(void);

/* The commands exec saw since the last reset, one per line ('\n'-ended); "" for none. */
const char *odin3_abc_fake_last_commands(void);

/*
 * The first command whose word 0 is `word` makes exec return ODIN3_ERR_ENGINE with *fail (any
 * kind). NULL word clears the knob. The command is recorded before it fails.
 */
void odin3_abc_fake_fail_on(const char *word, odin3_abc_fail fail);

#endif
