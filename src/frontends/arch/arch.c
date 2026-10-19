/*
 * arch.c — the architecture record (create, destroy, queries) and the import entry points.
 */
#include "frontends/arch/arch.h"

#include "util/alloc.h"

#include <string.h>

odin3_arch *odin3_arch_create(void) {
    odin3_arch *arch = odin3_util_calloc(sizeof *arch);
    if (arch == NULL) {
        return NULL;
    }
    arch->arena = odin3_arena_create(0);
    if (arch->arena == NULL) {
        odin3_util_free(arch);
        return NULL;
    }
    odin3_vec_init(&arch->leaves, sizeof(odin3_arch_leaf));
    odin3_vec_init(&arch->ics, sizeof(odin3_arch_ic));
    odin3_vec_init(&arch->directs, sizeof(odin3_arch_direct));
    odin3_strbuf_init(&arch->text);
    return arch;
}

void odin3_arch_destroy(odin3_arch *arch) {
    if (arch == NULL) {
        return;
    }
    odin3_vec_free(&arch->leaves);
    odin3_vec_free(&arch->ics);
    odin3_vec_free(&arch->directs);
    odin3_strbuf_free(&arch->text);
    odin3_arena_destroy(arch->arena);
    odin3_util_free(arch);
}

/* --- queries ------------------------------------------------------------------------------- */

uint32_t odin3_arch_lut_k(const odin3_arch *arch) {
    return arch->lut_k;
}

uint32_t odin3_arch_lut_k_min(const odin3_arch *arch) {
    return arch->lut_k_min;
}

bool odin3_arch_has_adder(const odin3_arch *arch) {
    return arch->has_adder;
}

uint32_t odin3_arch_model_count(const odin3_arch *arch) {
    return arch->n_models;
}

const odin3_arch_model *odin3_arch_model_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->n_models ? &arch->models[index] : NULL;
}

const odin3_arch_model *odin3_arch_model_find(const odin3_arch *arch, const char *name) {
    for (uint32_t i = 0; i < arch->n_models; i++) {
        if (strcmp(arch->models[i].name, name) == 0) {
            return &arch->models[i];
        }
    }
    return NULL;
}

uint32_t odin3_arch_leaf_count(const odin3_arch *arch) {
    return (uint32_t)arch->leaves.len;
}

const odin3_arch_leaf *odin3_arch_leaf_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->leaves.len ? odin3_vec_cat(&arch->leaves, index) : NULL;
}

bool odin3_arch_next_leaf_of(const odin3_arch *arch, const char *model, uint32_t from,
                             uint32_t *index) {
    for (uint32_t i = from; i < arch->leaves.len; i++) {
        const odin3_arch_leaf *leaf = odin3_vec_cat(&arch->leaves, i);
        if (leaf->kind == ODIN3_ARCH_LEAF_SUBCKT && strcmp(leaf->model, model) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

uint32_t odin3_arch_ic_count(const odin3_arch *arch) {
    return (uint32_t)arch->ics.len;
}

const odin3_arch_ic *odin3_arch_ic_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->ics.len ? odin3_vec_cat(&arch->ics, index) : NULL;
}

uint32_t odin3_arch_direct_count(const odin3_arch *arch) {
    return (uint32_t)arch->directs.len;
}

const odin3_arch_direct *odin3_arch_direct_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->directs.len ? odin3_vec_cat(&arch->directs, index) : NULL;
}

const char *odin3_arch_library_text(const odin3_arch *arch) {
    return arch->text.data != NULL ? arch->text.data : "";
}
