/*
 * arch_test_cmp.h — compares one cell type of two designs: kind, parameters, ports (name, direction,
 * order, signedness, width under the defaults and under a parameter scale), `clock` flags and the
 * numbers of fn, seq and memory statements. Used to show that an imported library has the same hard
 * cells and ports as lib/vtr.o3lib.
 */
#ifndef ODIN3_TESTS_ARCH_TEST_CMP_H
#define ODIN3_TESTS_ARCH_TEST_CMP_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

static const odin3_celltype_def *cmp_def(odin3_design *design, const char *name,
                                         odin3_celltype_id *id) {
    uint32_t str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, id)) {
        return NULL;
    }
    return odin3_celltype_get(design, *id);
}

/* Widths of every port with each parameter set to `value` (or to its default when value is 0). */
static inline void cmp_widths(odin3_design *da, odin3_celltype_id ia, odin3_design *db,
                       odin3_celltype_id ib, int64_t value) {
    const odin3_celltype_def *a = odin3_celltype_get(da, ia);
    odin3_value params[8];
    TEST_ASSERT_TRUE(a->n_params <= 8);
    for (uint32_t i = 0; i < a->n_params; i++) {
        params[i] = a->params[i].dflt;
        if (value != 0) {
            params[i].i = value;
        }
    }
    for (uint32_t p = 0; p < a->n_ports; p++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(odin3_celltype_port_width(da, ia, params, p),
                                         odin3_celltype_port_width(db, ib, params, p), a->name);
    }
}

static inline void arch_test_same_type(odin3_design *da, odin3_design *db, const char *name) {
    odin3_celltype_id ia = {0};
    odin3_celltype_id ib = {0};
    const odin3_celltype_def *a = cmp_def(da, name, &ia);
    const odin3_celltype_def *b = cmp_def(db, name, &ib);
    TEST_ASSERT_NOT_NULL_MESSAGE(a, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(a->gran, b->gran, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->n_params, b->n_params, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->n_ports, b->n_ports, name);
    for (uint32_t i = 0; i < a->n_params; i++) {
        TEST_ASSERT_EQUAL_STRING(a->params[i].name, b->params[i].name);
        TEST_ASSERT_EQUAL_INT(a->params[i].kind, b->params[i].kind);
        TEST_ASSERT_EQUAL_INT64(a->params[i].dflt.i, b->params[i].dflt.i);
    }
    const odin3_techlib_cell *la = odin3_techlib_cell_get(da, ia);
    const odin3_techlib_cell *lb = odin3_techlib_cell_get(db, ib);
    TEST_ASSERT_NOT_NULL(la);
    TEST_ASSERT_NOT_NULL(lb);
    for (uint32_t i = 0; i < a->n_ports; i++) {
        TEST_ASSERT_EQUAL_STRING(a->ports[i].name, b->ports[i].name);
        TEST_ASSERT_EQUAL_INT(a->ports[i].dir, b->ports[i].dir);
        TEST_ASSERT_EQUAL_INT(a->ports[i].is_signed, b->ports[i].is_signed);
        TEST_ASSERT_EQUAL_INT(la->ports[i].clock, lb->ports[i].clock);
        TEST_ASSERT_EQUAL_INT(la->ports[i].is_signed, lb->ports[i].is_signed);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(la->n_fns, lb->n_fns, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(la->n_seqs, lb->n_seqs, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(la->memory != NULL, lb->memory != NULL, name);
    if (la->memory != NULL) {
        TEST_ASSERT_EQUAL_UINT32(la->memory->n_mports, lb->memory->n_mports);
        TEST_ASSERT_EQUAL_UINT32(la->memory->n_outs, lb->memory->n_outs);
    }
    cmp_widths(da, ia, db, ib, 0);
    cmp_widths(da, ia, db, ib, 36);
}

#endif
