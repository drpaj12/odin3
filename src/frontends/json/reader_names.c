/*
 * reader_names.c — the JSON reader's `--name-map` half (spec 5.4 H-6): the keys of cells and
 * netnames. A key that is a hash name of the map becomes the full name, which decodes (L-5) to
 * SHORT (the object's name, made unique in its module), `hier` and a location for the object's
 * record.
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

/* True when the module already has an object of this kind with the name str. */
static bool name_taken(const jr *rd, jr_name_kind kind, uint32_t str) {
    const odin3_module *module = rd->cur.module;
    return kind == JR_NAME_WIRE ? odin3_module_find_wire(module, str).v != 0
                                : odin3_module_find_node(module, str).v != 0;
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

odin3_status jr_name_key(jr *rd, const odin3_json_node *node, jr_name_kind kind, jr_named *out) {
    *out = (jr_named){0};
    odin3_bytes key = odin3_bytes_cstr(odin3_json_key(node));
    if (!rd->have_names || !odin3_name_hash_shaped(key)) {
        return jr_fail(rd, odin3_design_intern(rd->design, key, &out->name));
    }
    odin3_bytes full = {NULL, 0};
    if (!odin3_name_resolver_find(&rd->names, module_bytes(rd), key, &full)) {
        odin3_name_miss_note(&rd->name_miss, key, odin3_json_key_pos(node).line);
        return jr_fail(rd, odin3_design_intern(rd->design, key, &out->name));
    }
    return decoded_name(rd, kind, full, out);
}

void jr_report_names(const jr *rd) {
    if (rd->have_names) {
        odin3_name_miss_report(&rd->name_miss, "read_json", rd->path);
    }
}
