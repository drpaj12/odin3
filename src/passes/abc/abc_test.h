/*
 * abc_test.h — test hooks of the abc pass (3D plan Tasks 10, 11, 14). Never part of the ABI.
 */
#ifndef ODIN3_PASSES_ABC_ABC_TEST_H
#define ODIN3_PASSES_ABC_ABC_TEST_H

#include "passes/abc/script.h"

#include <stdint.h>

/*
 * Makes odin3_abc_alias_find, _count and _at use table[0..n) instead of the abc.rc table (the
 * checker's alias-depth tests). The table is borrowed and must outlive its use; NULL restores
 * the abc.rc table.
 */
void odin3_abc_test_set_aliases(const odin3_abc_alias *table, uint32_t n);

#endif
