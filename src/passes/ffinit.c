/*
 * ffinit.c — set and remove a net's initial value on the `init` attribute (2E spec §10; 2F spec
 * §9.4; Yosys kernel/ffinit.h).
 */
#include "passes/ffinit.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char k_init[] = "init";

/* The net's wire memberships in order: its primary, then its aliases that are wire bits. */
static odin3_status memberships(const odin3_module *module, odin3_net_id net, odin3_vec *out) {
    odin3_wirebit primary = odin3_net_primary(module, net);
    if (odin3_wire_valid(primary.wire)) {
        odin3_wirebit *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = primary;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        if (!odin3_wire_valid(alias.wb.wire)) {
            continue;
        }
        odin3_wirebit *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = alias.wb;
    }
    return ODIN3_OK;
}

/* Bit `bit` of an `init` value (x when there is none, it is not BITS or it is too short). */
static odin3_bit init_bit(const odin3_value *init, uint32_t bit) {
    if (init == NULL || init->kind != ODIN3_VAL_BITS || bit >= init->len) {
        return ODIN3_BIT_X;
    }
    return (odin3_bit)init->bits[bit];
}

/* True when `init` is BITS of exactly width bits (so a write can keep it as it is). */
static bool init_exact(const odin3_value *init, uint32_t width) {
    return init != NULL && init->kind == ODIN3_VAL_BITS && init->len == width;
}

/* One write: wire bit wb of the `init` keyed key gets value; old receives the bit it had. */
typedef struct init_write {
    odin3_module *module;
    uint32_t key;
    odin3_wirebit wb;
    odin3_bit value;
} init_write;

/* The wire's new `init`: the old bits, wb.bit set; removed when it is left all x. */
static odin3_status rewrite_init(const init_write *wr, const odin3_value *init, uint8_t *bits,
                                 uint32_t width) {
    bool any = false;
    for (uint32_t i = 0; i < width; i++) {
        bits[i] = (uint8_t)(i == wr->wb.bit ? wr->value : init_bit(init, i));
        any = any || bits[i] != ODIN3_BIT_X;
    }
    odin3_objref ref = {ODIN3_OBJ_WIRE, wr->wb.wire.v};
    if (!any) {
        return odin3_attr_remove(wr->module, ref, wr->key);
    }
    odin3_value val = {.kind = ODIN3_VAL_BITS, .bits = bits, .len = width};
    return odin3_attr_set(wr->module, ref, wr->key, &val);
}

static odin3_status write_bit(const init_write *wr, odin3_bit *old) {
    odin3_objref ref = {ODIN3_OBJ_WIRE, wr->wb.wire.v};
    const odin3_value *init = odin3_attr_get(wr->module, ref, wr->key);
    uint32_t width = odin3_wire_width(wr->module, wr->wb.wire);
    *old = init_bit(init, wr->wb.bit);
    if (*old == wr->value && (init == NULL || init_exact(init, width))) {
        return ODIN3_OK; /* nothing changes */
    }
    uint8_t *bits = odin3_util_malloc(width > 0 ? width : 1);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = rewrite_init(wr, init, bits, width);
    odin3_util_free(bits);
    return st;
}

static odin3_status check_args(const odin3_module *module, odin3_net_id net, const char *what) {
    if (module == NULL || !odin3_net_live(module, net)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: no module or net %u is not live", what, net.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_ffinit_set(odin3_module *module, odin3_net_id net, odin3_bit value) {
    odin3_status st = check_args(module, net, "ffinit_set");
    if (st == ODIN3_OK && (unsigned)value > (unsigned)ODIN3_BIT_Z) {
        odin3_log(ODIN3_LOG_ERROR, "ffinit_set: %d is not 0, 1, x or z", (int)value);
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vec wbs;
    odin3_vec_init(&wbs, sizeof(odin3_wirebit));
    st = memberships(module, net, &wbs);
    init_write wr = {module, 0, {{0}, 0}, value};
    if (st == ODIN3_OK && wbs.len > 0) {
        st = odin3_design_intern(odin3_module_design(module), odin3_bytes_cstr(k_init), &wr.key);
    }
    for (size_t i = 0; st == ODIN3_OK && i < wbs.len; i++) {
        odin3_bit old = ODIN3_BIT_X;
        wr.wb = *(const odin3_wirebit *)odin3_vec_cat(&wbs, i);
        wr.value = i == 0 ? value : ODIN3_BIT_X;
        st = write_bit(&wr, &old);
    }
    odin3_vec_free(&wbs);
    return st;
}

odin3_status odin3_ffinit_remove(odin3_module *module, odin3_net_id net,
                                 odin3_wire_id *defined_on) {
    if (defined_on != NULL) {
        *defined_on = (odin3_wire_id){0};
    }
    odin3_status st = check_args(module, net, "ffinit_remove");
    init_write wr = {module, 0, {{0}, 0}, ODIN3_BIT_X};
    if (st != ODIN3_OK || !odin3_strtab_find(odin3_design_strtab(odin3_module_design(module)),
                                             odin3_bytes_cstr(k_init), &wr.key)) {
        return st; /* no `init` key: no wire has one */
    }
    odin3_vec wbs;
    odin3_vec_init(&wbs, sizeof(odin3_wirebit));
    st = memberships(module, net, &wbs);
    for (size_t i = 0; st == ODIN3_OK && i < wbs.len; i++) {
        odin3_bit old = ODIN3_BIT_X;
        wr.wb = *(const odin3_wirebit *)odin3_vec_cat(&wbs, i);
        st = write_bit(&wr, &old);
        bool defined = old == ODIN3_BIT_0 || old == ODIN3_BIT_1;
        if (st == ODIN3_OK && defined && defined_on != NULL && !odin3_wire_valid(*defined_on)) {
            *defined_on = wr.wb.wire;
        }
    }
    odin3_vec_free(&wbs);
    return st;
}
