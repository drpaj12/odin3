/*
 * common.c — the module context of the fsm stages (fsm.h): index, attribute keys, names.
 */
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/index.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

odin3_status odin3_fsm_mod_open(odin3_fsm_mod *fm, odin3_design *design, odin3_module *module,
                                const odin3_fsm_opts *opts) {
    memset(fm, 0, sizeof *fm);
    fm->design = design;
    fm->module = module;
    fm->opts = opts;
    odin3_status st =
        odin3_design_intern(design, odin3_bytes_cstr("fsm_encoding"), &fm->key_encoding);
    if (st == ODIN3_OK) {
        st = odin3_design_intern(design, odin3_bytes_cstr("init"), &fm->key_init);
    }
    if (st == ODIN3_OK) {
        st = odin3_fsm_index_build(design, module, &fm->ix);
    }
    if (st != ODIN3_OK) {
        memset(fm, 0, sizeof *fm);
    }
    return st;
}

void odin3_fsm_mod_close(odin3_fsm_mod *fm) {
    odin3_fsm_index_free(&fm->ix);
    memset(fm, 0, sizeof *fm);
}

const char *odin3_fsm_str(const odin3_fsm_mod *fm, uint32_t str) {
    const char *text = odin3_strtab_get(odin3_design_strtab(fm->design), str);
    return text != NULL ? text : "";
}

const char *odin3_fsm_mod_name(const odin3_fsm_mod *fm) {
    return odin3_fsm_str(fm, odin3_module_name(fm->module));
}

void odin3_fsm_wire_label(const odin3_fsm_mod *fm, odin3_wire_id wire, char *buf, size_t size) {
    uint32_t name = odin3_wire_name(fm->module, wire);
    if (name == 0) {
        (void)snprintf(buf, size, "%s.$w%u", odin3_fsm_mod_name(fm), wire.v);
    } else {
        (void)snprintf(buf, size, "%s.%s", odin3_fsm_mod_name(fm), odin3_fsm_str(fm, name));
    }
}

odin3_status odin3_fsm_wire_sigs(const odin3_fsm_mod *fm, odin3_wire_id wire, odin3_vec *out) {
    uint32_t width = odin3_wire_width(fm->module, wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_fsm_sig *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_fsm_ix_sig(&fm->ix, odin3_wire_net(fm->module, wire, bit).v);
    }
    return ODIN3_OK;
}

const char *odin3_fsm_wire_encoding(const odin3_fsm_mod *fm, odin3_wire_id wire) {
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    const odin3_value *val = odin3_attr_get(fm->module, obj, fm->key_encoding);
    if (val == NULL) {
        return NULL;
    }
    return val->kind == ODIN3_VAL_STRING ? odin3_fsm_str(fm, val->str) : "";
}

odin3_status odin3_fsm_wire_set_str(odin3_fsm_mod *fm, odin3_wire_id wire, uint32_t key,
                                    const char *text) {
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0};
    odin3_status st = odin3_design_intern(fm->design, odin3_bytes_cstr(text), &val.str);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    return odin3_attr_set(fm->module, obj, key, &val);
}
