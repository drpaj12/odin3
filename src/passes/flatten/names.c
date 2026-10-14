/*
 * names.c — names of flattened objects (3A spec §4, §9; plan Task 3): prefixing, the `_N`
 * uniqueness rule per kind, the MEMID set and the `$c<ID>` label of an unnamed instance.
 */
#include "passes/flatten/flatten_int.h"

#include "ir/module.h"
#include "util/log.h"

#include <inttypes.h>
#include <string.h>

odin3_status odin3_flatten_intern_scratch(odin3_flatten_ctx *fc, uint32_t *out) {
    return odin3_design_intern(fc->design, odin3_bytes_cstr(fc->scratch.data), out);
}

static bool name_taken(const odin3_flatten_ctx *fc, flat_key key) {
    uint32_t str = key.str;
    switch (key.kind) {
    case FLAT_NODE:
        return odin3_node_valid(odin3_module_find_node(fc->root, str));
    case FLAT_NET:
        return odin3_net_valid(odin3_module_find_net(fc->root, str));
    default:
        return odin3_wire_valid(odin3_module_find_wire(fc->root, str));
    }
}

/* scratch holds the base name; appends `_1`, `_2`, ... until `taken` stops holding. */
static odin3_status uniquify(odin3_flatten_ctx *fc, flat_kind kind, bool memid, uint32_t *out) {
    flat_key key = {kind, 0};
    uint32_t str = 0;
    odin3_status st = odin3_flatten_intern_scratch(fc, &str);
    size_t base_len = fc->scratch.len;
    for (uint32_t tries = 1; st == ODIN3_OK; tries++) {
        uint64_t unused = 0;
        bool taken = memid ? odin3_u64map_get(fc->memids, str, &unused)
                           : name_taken(fc, (flat_key){key.kind, str});
        if (!taken) {
            break;
        }
        if (tries == 1) {
            fc->renamed++;
        }
        fc->scratch.len = base_len;
        fc->scratch.data[base_len] = '\0';
        st = odin3_strbuf_appendf(&fc->scratch, "_%" PRIu32, tries);
        st = st == ODIN3_OK ? odin3_flatten_intern_scratch(fc, &str) : st;
        fc->scratch.len = base_len;
        fc->scratch.data[base_len] = '\0';
        if (st == ODIN3_OK) {
            const char *text = odin3_strtab_get(odin3_design_strtab(fc->design), str);
            odin3_log(ODIN3_LOG_DEBUG, "flatten: renamed '%s' (name taken)", text);
        }
    }
    *out = str;
    return st;
}

static odin3_status build_name(odin3_flatten_ctx *fc, uint32_t orig) {
    odin3_strbuf_clear(&fc->scratch);
    odin3_status st = odin3_strbuf_append(&fc->scratch, odin3_bytes_cstr(fc->prefix.data));
    const char *text = odin3_strtab_get(odin3_design_strtab(fc->design), orig);
    return st == ODIN3_OK ? odin3_strbuf_append(&fc->scratch, odin3_bytes_cstr(text)) : st;
}

odin3_status odin3_flatten_name(odin3_flatten_ctx *fc, flat_key key, uint32_t *out) {
    *out = 0;
    if (key.str == 0) {
        return ODIN3_OK;
    }
    odin3_status st = build_name(fc, key.str);
    return st == ODIN3_OK ? uniquify(fc, key.kind, false, out) : st;
}

odin3_status odin3_flatten_memid(odin3_flatten_ctx *fc, uint32_t orig, uint32_t *out) {
    const char *text = odin3_strtab_get(odin3_design_strtab(fc->design), orig);
    uint64_t known = 0;
    if (orig == 0 || text[0] == '\0') {
        *out = orig; /* no memory name to prefix (review M9) */
        return ODIN3_OK;
    }
    if (odin3_u64map_get(fc->memmap, orig, &known)) {
        *out = (uint32_t)known; /* every cell of this memory in this instance: one new MEMID */
        return ODIN3_OK;
    }
    odin3_status st = build_name(fc, orig);
    st = st == ODIN3_OK ? uniquify(fc, FLAT_NODE, true, out) : st;
    st = st == ODIN3_OK ? odin3_u64map_put(fc->memids, (odin3_kv){*out, 1}) : st;
    return st == ODIN3_OK ? odin3_u64map_put(fc->memmap, (odin3_kv){orig, *out}) : st;
}

odin3_status odin3_flatten_set_prefix(odin3_flatten_ctx *fc, flat_label label) {
    odin3_strbuf_clear(&fc->prefix);
    if (label.name != 0) {
        const char *text = odin3_strtab_get(odin3_design_strtab(fc->design), label.name);
        return odin3_strbuf_appendf(&fc->prefix, "%s%s", text, fc->sep);
    }
    const char *parent =
        label.parent != 0 ? odin3_strtab_get(odin3_design_strtab(fc->design), label.parent) : "";
    return odin3_strbuf_appendf(&fc->prefix, "%s$c%" PRIu32 "%s", parent, label.node, fc->sep);
}
