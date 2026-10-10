/*
 * design.c — design create/destroy, the design-global string table, module list, prov store,
 * source manager, project record and diagnostic-path style.
 */
#include "ir/design.h"
#include "ast/srcman.h"
#include "frontends/project/project.h"
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
        odin3_celltype_table_init(design) != ODIN3_OK ||
        odin3_module_table_init(design) != ODIN3_OK || odin3_prov_store_init(design) != ODIN3_OK) {
        odin3_design_destroy(design);
        return NULL;
    }
    return design;
}

void odin3_design_destroy(odin3_design *design) {
    if (design == NULL) {
        return;
    }
    odin3_project_destroy(design->project);
    odin3_srcman_destroy(design->srcman);
    odin3_prov_store_free(design);
    odin3_module_table_free(design);
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

odin3_status odin3_design_get_srcman(odin3_design *design, odin3_srcman **out) {
    if (design->srcman == NULL) {
        design->srcman = odin3_srcman_create(design->strtab);
        if (design->srcman == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *out = design->srcman;
    return ODIN3_OK;
}

odin3_status odin3_design_get_project(odin3_design *design, odin3_project **out) {
    if (design->project == NULL) {
        design->project = odin3_project_create(design);
        if (design->project == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *out = design->project;
    return ODIN3_OK;
}

odin3_project *odin3_design_project(const odin3_design *design) {
    return design->project;
}

void odin3_design_drop_project(odin3_design *design) {
    odin3_project_destroy(design->project);
    design->project = NULL;
}

void odin3_design_set_diag_paths(odin3_design *design, odin3_srcfmt style) {
    design->diag_paths =
        style == ODIN3_SRCFMT_RESOLVED ? ODIN3_SRCFMT_RESOLVED : ODIN3_SRCFMT_GIVEN;
}

odin3_srcfmt odin3_design_diag_paths(const odin3_design *design) {
    return design->diag_paths;
}
