/* attr.c — per-module attribute side table keyed by (object kind, ID) (IR-10). */
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

enum { OBJ_KIND_SHIFT = 32 };

uint64_t odin3_attr_key(odin3_objref obj) {
    return ((uint64_t)(uint32_t)obj.kind << OBJ_KIND_SHIFT) | obj.id;
}

static bool obj_live(const odin3_module *module, odin3_objref obj) {
    switch (obj.kind) {
    case ODIN3_OBJ_NODE:
        return odin3_node_live(module, (odin3_node_id){obj.id});
    case ODIN3_OBJ_NET:
        return odin3_net_live(module, (odin3_net_id){obj.id});
    case ODIN3_OBJ_WIRE:
        return odin3_wire_live(module, (odin3_wire_id){obj.id});
    case ODIN3_OBJ_MODULE:
        return obj.id == odin3_module_id_of(module).v;
    }
    return false;
}

/* NULL when module, obj and key_str can be used, else what is wrong. */
static const char *object_key_error(const odin3_module *module, odin3_objref obj,
                                    uint32_t key_str) {
    if (module == NULL) {
        return "no module";
    }
    if (!obj_live(module, obj)) {
        return "no live object of that kind and ID";
    }
    if (key_str == 0 || key_str >= odin3_strtab_count(odin3_design_strtab(module->design))) {
        return "the key is not a non-empty string ID";
    }
    return NULL;
}

/* NULL when the call is valid, else what is wrong. */
static const char *set_error(const odin3_module *module, odin3_objref obj, uint32_t key_str,
                             const odin3_value *value) {
    const char *err = object_key_error(module, obj, key_str);
    if (err != NULL) {
        return err;
    }
    if (!odin3_value_valid(value)) {
        return "the value has an unknown kind, a missing payload or partial cover rows";
    }
    if (value->kind == ODIN3_VAL_PROC) {
        return "a process body is only ever a $proc node's BODY parameter (IR-19)";
    }
    return NULL;
}

static const odin3_attr_rec *attr_cat(const odin3_module *module, uint32_t idx) {
    return odin3_vec_cat(&module->attrs, idx);
}

/*
 * Record of attribute key of obj; 0 when absent. *last (may be NULL) gets the last record of
 * obj's chain, 0 when obj has no attributes.
 */
static uint32_t find_attr(const odin3_module *module, odin3_objref obj, uint32_t key,
                          uint32_t *last) {
    uint64_t head = 0;
    uint32_t prev = 0;
    if (odin3_u64map_get(module->attr_heads, odin3_attr_key(obj), &head)) {
        for (uint32_t idx = (uint32_t)head; idx != 0; idx = attr_cat(module, idx)->next) {
            if (attr_cat(module, idx)->key == key) {
                return idx;
            }
            prev = idx;
        }
    }
    if (last != NULL) {
        *last = prev;
    }
    return 0;
}

/* Room for one more record (the first use also takes the reserved slot 0). */
static odin3_status attr_reserve(odin3_module *module) {
    size_t len = module->attrs.len;
    size_t need = (len == 0 ? 1 : len) + 1;
    if (need > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_vec_reserve(&module->attrs, need);
    if (st == ODIN3_OK && len == 0) {
        (void)odin3_vec_push(&module->attrs); /* cannot fail: reserved */
    }
    return st;
}

/* Appends a record for key after `last`, obj's last record (0: obj has no attributes yet). */
static odin3_status attr_append(odin3_module *module, odin3_objref obj, uint32_t key,
                                const odin3_value *copy) {
    uint32_t last = 0;
    (void)find_attr(module, obj, key, &last);
    odin3_status st = attr_reserve(module);
    uint32_t idx = (uint32_t)module->attrs.len;
    if (st == ODIN3_OK && last == 0) {
        st = odin3_u64map_put(module->attr_heads, (odin3_kv){odin3_attr_key(obj), idx}); /* last */
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_attr_rec *rec = odin3_vec_push(&module->attrs);
    assert(rec != NULL); /* reserved */
    rec->key = key;
    rec->value = copy;
    if (last != 0) {
        ((odin3_attr_rec *)odin3_vec_at(&module->attrs, last))->next = idx;
    }
    return ODIN3_OK;
}

/* Journal (module.h): an attribute of a wire changed, so every net of its bits did (ffinit reads
 * `init` through the wire memberships). */
static void journal_attr(odin3_module *module, odin3_objref obj) {
    odin3_journal_touch(module); /* an attribute of any object (`keep`, `init`, ...) */
    if (!module->journal.on || obj.kind != ODIN3_OBJ_WIRE) {
        return;
    }
    const odin3_wire_rec *wire = odin3_wire_rec_cat(module, (odin3_wire_id){obj.id});
    for (uint32_t k = 0; wire != NULL && k < wire->width; k++) {
        odin3_module_journal_add(module, ODIN3_JOURNAL_DRIVE, wire->nets[k].v);
    }
}

odin3_status odin3_attr_set(odin3_module *module, odin3_objref obj, uint32_t key_str,
                            const odin3_value *value) {
    const char *err = set_error(module, obj, key_str, value);
    if (err != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "attr_set: kind %d, ID %u, key %u: %s", (int)obj.kind, obj.id,
                  key_str, err);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_value *copy = odin3_arena_alloc(module->arena, sizeof *copy);
    if (copy == NULL || odin3_value_copy(module->arena, value, copy) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    journal_attr(module, obj);
    uint32_t idx = find_attr(module, obj, key_str, NULL);
    if (idx != 0) {
        ((odin3_attr_rec *)odin3_vec_at(&module->attrs, idx))->value = copy;
        return ODIN3_OK;
    }
    return attr_append(module, obj, key_str, copy);
}

odin3_status odin3_attr_remove(odin3_module *module, odin3_objref obj, uint32_t key_str) {
    const char *err = object_key_error(module, obj, key_str);
    if (err != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "attr_remove: kind %d, ID %u, key %u: %s", (int)obj.kind, obj.id,
                  key_str, err);
        return ODIN3_ERR_INVALID_ARG;
    }
    journal_attr(module, obj);
    uint64_t head = 0;
    uint64_t map_key = odin3_attr_key(obj);
    if (!odin3_u64map_get(module->attr_heads, map_key, &head)) {
        return ODIN3_OK;
    }
    uint32_t prev = 0;
    uint32_t idx = (uint32_t)head;
    while (idx != 0 && attr_cat(module, idx)->key != key_str) {
        prev = idx;
        idx = attr_cat(module, idx)->next;
    }
    if (idx == 0) {
        return ODIN3_OK;
    }
    uint32_t next = attr_cat(module, idx)->next;
    if (prev != 0) {
        ((odin3_attr_rec *)odin3_vec_at(&module->attrs, prev))->next = next;
    } else if (next != 0) {
        /* the key is present, so put only overwrites its value: never allocates */
        odin3_status st = odin3_u64map_put(module->attr_heads, (odin3_kv){map_key, next});
        assert(st == ODIN3_OK);
        (void)st;
    } else {
        (void)odin3_u64map_remove(module->attr_heads, map_key);
    }
    ((odin3_attr_rec *)odin3_vec_at(&module->attrs, idx))->next = 0;
    return ODIN3_OK;
}

const odin3_value *odin3_attr_get(const odin3_module *module, odin3_objref obj, uint32_t key_str) {
    uint32_t idx = find_attr(module, obj, key_str, NULL);
    return idx != 0 ? attr_cat(module, idx)->value : NULL;
}

odin3_status odin3_attr_foreach(const odin3_module *module, odin3_objref obj,
                                odin3_attr_visit visit, void *ctx) {
    if (module == NULL || visit == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "attr_foreach: NULL module or visit");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t head = 0;
    if (!odin3_u64map_get(module->attr_heads, odin3_attr_key(obj), &head)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t idx = (uint32_t)head; idx != 0 && st == ODIN3_OK;
         idx = attr_cat(module, idx)->next) {
        const odin3_attr_rec *rec = attr_cat(module, idx);
        st = visit(ctx, rec->key, rec->value);
    }
    return st;
}
