/* attrs.c — how the JSON and Verilog writers name and spell IR attributes (IR-10). */
#include "backends/common/attrs.h"

#include "frontends/blif/attrs.h"

#include <string.h>

/* The rest of key after prefix, when key starts with it (and is longer). */
static bool strip_prefix(odin3_bytes key, const char *prefix, odin3_bytes *rest) {
    size_t len = strlen(prefix);
    if (key.len <= len || memcmp(key.ptr, prefix, len) != 0) {
        return false;
    }
    *rest = (odin3_bytes){(const char *)key.ptr + len, key.len - len};
    return true;
}

/* A BLIF `.attr`/`.param` value as Yosys read_blif takes it. */
static void blif_value(odin3_wattr *attr, const odin3_strtab *tab, const odin3_value *value) {
    if (value->kind != ODIN3_VAL_STRING) {
        return; /* not from the reader: keep the IR value */
    }
    const char *text = odin3_strtab_get(tab, value->str);
    size_t len = odin3_strtab_len(tab, value->str);
    if (text != NULL && len > 0 && text[0] == '"') {
        attr->form = ODIN3_WATTR_TEXT;
        len -= len > 1 && text[len - 1] == '"' ? 2 : 1;
        attr->text = (odin3_bytes){text + 1, len};
    } else {
        attr->form = ODIN3_WATTR_BINARY;
        attr->text = (odin3_bytes){text != NULL ? text : "", text != NULL ? len : 0};
    }
}

odin3_wattr odin3_wattr_classify(const odin3_strtab *tab, uint32_t key_str,
                                 const odin3_value *value) {
    const char *name = odin3_strtab_get(tab, key_str);
    odin3_bytes key = {name != NULL ? name : "", odin3_strtab_len(tab, key_str)};
    odin3_wattr attr = {ODIN3_WATTR_ATTRIBUTE, ODIN3_WATTR_VALUE, key, {NULL, 0}, value};
    if (key.len == strlen(ODIN3_BLIF_ATTR_EXTRAS) &&
        memcmp(key.ptr, ODIN3_BLIF_ATTR_EXTRAS, key.len) == 0) {
        attr.role = ODIN3_WATTR_SKIP;
    } else if (strip_prefix(key, ODIN3_BLIF_ATTR_PREFIX, &attr.key)) {
        blif_value(&attr, tab, value);
    } else if (strip_prefix(key, ODIN3_BLIF_PARAM_PREFIX, &attr.key)) {
        attr.role = ODIN3_WATTR_PARAMETER;
        blif_value(&attr, tab, value);
    }
    return attr;
}

void odin3_wattr_seen_init(odin3_wattr_seen *seen) {
    odin3_vec_init(&seen->entries, sizeof(odin3_wattr_claim_req));
}

void odin3_wattr_seen_free(odin3_wattr_seen *seen) {
    odin3_vec_free(&seen->entries);
}

void odin3_wattr_seen_clear(odin3_wattr_seen *seen) {
    odin3_vec_clear(&seen->entries);
}

odin3_status odin3_wattr_claim(odin3_wattr_seen *seen, odin3_wattr_claim_req req, bool *fresh) {
    for (size_t i = 0; i < seen->entries.len; i++) {
        const odin3_wattr_claim_req *old = odin3_vec_cat(&seen->entries, i);
        if (old->role == req.role && old->key.len == req.key.len &&
            (req.key.len == 0 || memcmp(old->key.ptr, req.key.ptr, req.key.len) == 0)) {
            *fresh = false;
            return ODIN3_OK;
        }
    }
    odin3_wattr_claim_req *slot = odin3_vec_push(&seen->entries);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = req;
    *fresh = true;
    return ODIN3_OK;
}
