/*
 * reader_names.c — the JSON reader's `--name-map` half (spec 5.4 H-6): the keys of cells and
 * netnames. A key that is a hash name of the map becomes the full name, which decodes (L-5) to
 * SHORT (the object's name, made unique in its module), `hier` and a location for the object's
 * record.
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

enum { RAW_KIND_SHIFT = 32 };

static uint64_t raw_key(jr_name_kind kind, uint32_t str) {
    return ((uint64_t)kind << RAW_KIND_SHIFT) | str;
}

/* True when the module already has an object of this kind with the name str, or a key of the
 * module that is no hash name of the map is spelled so (raw names always win, H-6). */
static bool name_taken(const jr *rd, jr_name_kind kind, uint32_t str) {
    const odin3_module *module = rd->cur.module;
    bool made = kind == JR_NAME_WIRE ? odin3_module_find_wire(module, str).v != 0
                                     : odin3_module_find_node(module, str).v != 0;
    return made ||
           (rd->raw_names != NULL && odin3_u64map_get(rd->raw_names, raw_key(kind, str), NULL));
}

/* Interns short, as `short$N` with the first free N when the module has that name (IR-14). */
static odin3_status intern_unique(jr *rd, jr_name_kind kind, odin3_bytes text, uint32_t *out) {
    odin3_status st = jr_fail(rd, odin3_design_intern(rd->design, text, out));
    for (uint32_t suffix = 1; st == ODIN3_OK && name_taken(rd, kind, *out); suffix++) {
        odin3_strbuf_clear(&rd->name_buf);
        st = odin3_strbuf_append(&rd->name_buf, text);
        st = st == ODIN3_OK ? odin3_strbuf_appendf(&rd->name_buf, "$%u", (unsigned)suffix) : st;
        st = st == ODIN3_OK
                 ? jr_fail(rd,
                           odin3_design_intern(
                               rd->design, (odin3_bytes){rd->name_buf.data, rd->name_buf.len}, out))
                 : st;
    }
    return st;
}

/* The module name in the map's terms (the key of the module in the file). */
static odin3_bytes module_bytes(const jr *rd) {
    return odin3_bytes_cstr(rd->cur_name != NULL ? rd->cur_name : "");
}

static odin3_status decoded_name(jr *rd, jr_name_kind kind, odin3_bytes full, jr_named *out) {
    odin3_hdl_name nm;
    if (!odin3_hdl_name_decode(full, &nm)) {
        return jr_fail(rd, odin3_design_intern(rd->design, full, &out->name));
    }
    odin3_status st = intern_unique(rd, kind, nm.short_name, &out->name);
    if (st == ODIN3_OK && nm.hier.len > 0) {
        st = jr_fail(rd, odin3_design_intern(rd->design, nm.hier, &out->hier));
    }
    out->loc = nm.loc;
    out->decoded = st == ODIN3_OK;
    return st;
}

/* The full name the map holds for key, when it does. */
static bool map_full(const jr *rd, jr_name_kind kind, odin3_bytes key, odin3_bytes *full) {
    if (!rd->have_names || !odin3_name_hash_shaped(key)) {
        return false;
    }
    odin3_bytes module = module_bytes(rd);
    return kind == JR_NAME_WIRE
               ? odin3_name_resolver_find_decl(&rd->names, module, key, full)
               : odin3_name_resolver_find(&rd->names, module, ODIN3_NAME_NODE, key, full);
}

odin3_status jr_name_key(jr *rd, const odin3_json_node *node, jr_name_kind kind, jr_named *out) {
    *out = (jr_named){0};
    odin3_bytes key = odin3_bytes_cstr(odin3_json_key(node));
    odin3_bytes full = {NULL, 0};
    if (map_full(rd, kind, key, &full)) {
        return decoded_name(rd, kind, full, out);
    }
    if (rd->have_names && odin3_name_hash_shaped(key)) {
        odin3_name_miss_note(&rd->name_miss, key, odin3_json_key_pos(node).line);
    }
    return jr_fail(rd, odin3_design_intern(rd->design, key, &out->name));
}

/* Reserves the name a key of the module takes as it is: a raw key, or the full name of a hash key
 * that does not decode (L-5). A decoded key is not reserved: it moves aside. */
static odin3_status reserve_key(jr *rd, jr_name_kind kind, const odin3_json_node *node) {
    odin3_bytes text = odin3_bytes_cstr(odin3_json_key(node));
    odin3_bytes full = {NULL, 0};
    odin3_hdl_name nm;
    if (map_full(rd, kind, text, &full)) {
        if (odin3_hdl_name_decode(full, &nm)) {
            return ODIN3_OK;
        }
        text = full;
    }
    uint32_t str = 0;
    odin3_status st = jr_fail(rd, odin3_design_intern(rd->design, text, &str));
    return st != ODIN3_OK
               ? st
               : jr_fail(rd, odin3_u64map_put(rd->raw_names, (odin3_kv){raw_key(kind, str), 1}));
}

/* The member key of obj when it is an object (the readers report any other shape later). */
static const odin3_json_node *object_member(const odin3_json_node *obj, const char *key) {
    const odin3_json_node *member = jr_member(obj, key);
    return member != NULL && jr_kind(member) == ODIN3_JSON_OBJECT ? member : NULL;
}

odin3_status jr_names_begin(jr *rd, const odin3_json_node *mod) {
    odin3_status st = ODIN3_OK;
    if (!rd->have_names) {
        return st;
    }
    rd->raw_names = odin3_u64map_create(0);
    if (rd->raw_names == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    const odin3_json_node *netnames = object_member(mod, "netnames");
    const odin3_json_node *cells = object_member(mod, "cells");
    for (const odin3_json_node *key = jr_first(netnames); st == ODIN3_OK && key != NULL;
         key = odin3_json_next(key)) {
        st = reserve_key(rd, JR_NAME_WIRE, key);
    }
    for (const odin3_json_node *key = jr_first(cells); st == ODIN3_OK && key != NULL;
         key = odin3_json_next(key)) {
        st = reserve_key(rd, JR_NAME_NODE, key);
    }
    return st;
}

void jr_names_end(jr *rd) {
    odin3_u64map_destroy(rd->raw_names);
    rd->raw_names = NULL;
}

void jr_report_names(const jr *rd) {
    if (rd->have_names) {
        odin3_name_miss_report(&rd->name_miss, "read_json", rd->path);
    }
}
