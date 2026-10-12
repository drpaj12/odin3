/*
 * abc_test.h — test hooks of the abc pass (3D plan Tasks 4, 10, 11, 14; hidden; never exported,
 * never part of the ABI, never called outside tests/unit). The engine override is the only way
 * past "final" (3D spec §4.3).
 */
#ifndef ODIN3_PASSES_ABC_ABC_TEST_H
#define ODIN3_PASSES_ABC_ABC_TEST_H

#include "odin3/odin3.h"
#include "passes/abc/abc.h"
#include "passes/abc/script.h"

#include <stdint.h>

/*
 * Makes odin3_abc_alias_find, _count and _at use table[0..n) instead of the abc.rc table (the
 * checker's alias-depth tests). The table is borrowed and must outlive its use; NULL restores
 * the abc.rc table.
 */
void odin3_abc_test_set_aliases(const odin3_abc_alias *table, uint32_t n);

/*
 * Makes the checker use *limits instead of the ODIN3_ABC_MAX_* caps (the caps-at-their-limits
 * tests). A test may only lower a cap: each field is clamped to its default (the byte pass's
 * buffers are sized by the defaults). NULL restores the defaults.
 */
void odin3_abc_test_set_limits(const odin3_abc_limits *limits);

/*
 * Makes a copy of *engine (not validated; its strings and self must outlive its use) what
 * odin3_abc_engine_get returns, ahead of the registered engine; NULL clears the override, so get
 * returns the registered engine (or NULL) again. Never fails, never logs.
 */
void odin3_abc_test_set_engine(const odin3_abc_engine *engine);

#endif
