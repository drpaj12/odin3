/* design.c — design create/destroy and the design-global string table. */
#include "ir/design.h"
#include "ir/ir_internal.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/str.h"

odin3_design *odin3_design_create(void) {
    odin3_design *design = odin3_util_calloc(sizeof *design);
    if (design == NULL) {
        return NULL;
    }
    design->arena = odin3_arena_create(0);
    design->strtab = odin3_strtab_create();
    if (design->arena == NULL || design->strtab == NULL ||
        odin3_celltype_table_init(design) != ODIN3_OK) {
        odin3_design_destroy(design);
        return NULL;
    }
    return design;
}

void odin3_design_destroy(odin3_design *design) {
    if (design == NULL) {
        return;
    }
    odin3_celltype_table_free(design);
    odin3_strtab_destroy(design->strtab);
    odin3_arena_destroy(design->arena);
    odin3_util_free(design);
}

odin3_strtab *odin3_design_strtab(const odin3_design *design) {
    return design->strtab;
}

odin3_status odin3_design_intern(odin3_design *design, odin3_bytes bytes, uint32_t *str) {
    return odin3_strtab_intern(design->strtab, bytes, str);
}
