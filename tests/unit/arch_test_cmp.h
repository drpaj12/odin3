/*
 * arch_test_cmp.h — compares one cell type of two designs: kind, parameters, ports (name,
 * direction, order, signedness, width under the defaults and under a parameter scale), `clock`
 * flags and the numbers of fn, seq and memory statements. Used to show that an imported library has
 * the same hard cells and ports as lib/vtr.o3lib.
 */
#ifndef ODIN3_TESTS_ARCH_TEST_CMP_H
#define ODIN3_TESTS_ARCH_TEST_CMP_H

#include "arch_test.h"
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
static inline void cmp_widths(odin3_design *des_a, odin3_celltype_id id_a, odin3_design *des_b,
                              odin3_celltype_id id_b, int64_t value) {
    const odin3_celltype_def *def = odin3_celltype_get(des_a, id_a);
    odin3_value params[8];
    IS_TRUE(def->n_params <= 8);
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
        params[i].i = value != 0 ? value : params[i].i;
    }
    for (uint32_t port = 0; port < def->n_ports; port++) {
        EQ_U32(odin3_celltype_port_width(des_a, id_a, params, port),
               odin3_celltype_port_width(des_b, id_b, params, port));
    }
}

static inline void cmp_params(const odin3_celltype_def *def_a, const odin3_celltype_def *def_b) {
    EQ_U32(def_a->n_params, def_b->n_params);
    for (uint32_t i = 0; i < def_a->n_params; i++) {
        EQ_STR(def_a->params[i].name, def_b->params[i].name);
        EQ_INT(def_a->params[i].kind, def_b->params[i].kind);
        EQ_INT(def_a->params[i].dflt.i, def_b->params[i].dflt.i);
    }
}

static inline void cmp_ports(const odin3_celltype_def *def_a, const odin3_celltype_def *def_b,
                             const odin3_techlib_cell *lib_a, const odin3_techlib_cell *lib_b) {
    EQ_U32(def_a->n_ports, def_b->n_ports);
    for (uint32_t i = 0; i < def_a->n_ports; i++) {
        EQ_STR(def_a->ports[i].name, def_b->ports[i].name);
        EQ_INT(def_a->ports[i].dir, def_b->ports[i].dir);
        EQ_INT(def_a->ports[i].is_signed, def_b->ports[i].is_signed);
        EQ_INT(lib_a->ports[i].clock, lib_b->ports[i].clock);
        EQ_INT(lib_a->ports[i].is_signed, lib_b->ports[i].is_signed);
    }
}

static inline void cmp_lib(const odin3_techlib_cell *lib_a, const odin3_techlib_cell *lib_b) {
    EQ_U32(lib_a->n_fns, lib_b->n_fns);
    EQ_U32(lib_a->n_seqs, lib_b->n_seqs);
    EQ_INT(lib_a->memory != NULL, lib_b->memory != NULL);
    if (lib_a->memory != NULL) {
        EQ_U32(lib_a->memory->n_mports, lib_b->memory->n_mports);
        EQ_U32(lib_a->memory->n_outs, lib_b->memory->n_outs);
    }
}

static inline void arch_test_same_type(odin3_design *des_a, odin3_design *des_b, const char *name) {
    odin3_celltype_id id_a = {0};
    odin3_celltype_id id_b = {0};
    const odin3_celltype_def *def_a = cmp_def(des_a, name, &id_a);
    const odin3_celltype_def *def_b = cmp_def(des_b, name, &id_b);
    NOT_NULL(def_a);
    NOT_NULL(def_b);
    EQ_INT(def_a->gran, def_b->gran);
    cmp_params(def_a, def_b);
    const odin3_techlib_cell *lib_a = odin3_techlib_cell_get(des_a, id_a);
    const odin3_techlib_cell *lib_b = odin3_techlib_cell_get(des_b, id_b);
    NOT_NULL(lib_a);
    NOT_NULL(lib_b);
    cmp_ports(def_a, def_b, lib_a, lib_b);
    cmp_lib(lib_a, lib_b);
    cmp_widths(des_a, id_a, des_b, id_b, 0);
    cmp_widths(des_a, id_a, des_b, id_b, 36);
}

#endif
