/*
 * reader_ext.c — members the schema does not list (J-8) and the keys reserved for Phase 6.3's
 * graph export (J-9).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

/* One handler of an extension key (6.3 E-3). A handler returns true when it consumed the member;
 * a key without one, or a member the handler cannot use, is skipped and counted. */
typedef struct odin3_json_ext_key {
    const char *key;
    bool (*handler)(jr *rd, const odin3_json_node *value);
} odin3_json_ext_key;

enum { PROV_HDL_MAX = 8 };

/* odin3_version: an integer; a value above 1 is the error of jr_check_version. */
static bool ext_version(jr *rd, const odin3_json_node *value) {
    (void)rd;
    return jr_kind(value) == ODIN3_JSON_INT;
}

/* odin3_level: an integer, set as the attribute odin3_level once the object exists. */
static bool ext_level(jr *rd, const odin3_json_node *value) {
    if (jr_kind(value) != ODIN3_JSON_INT) {
        return false;
    }
    rd->ext_level = value;
    return true;
}

static bool is_text(const odin3_json_node *node) {
    return jr_kind(node) == ODIN3_JSON_STRING;
}

static bool prov_kind_ok(const odin3_json_node *kind) {
    static const char *const k_kinds[] = {"SOURCE", "IMPORTED", "DERIVED"};
    const char *text = jr_text(kind, NULL);
    for (size_t i = 0; is_text(kind) && i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        if (strcmp(text, k_kinds[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool prov_hdl_ok(const odin3_json_node *hdl) {
    if (jr_kind(hdl) != ODIN3_JSON_ARRAY || jr_count(hdl) > PROV_HDL_MAX) {
        return false;
    }
    for (const odin3_json_node *elem = jr_first(hdl); elem != NULL; elem = odin3_json_next(elem)) {
        if (!is_text(elem)) {
            return false;
        }
    }
    return true;
}

/* odin3_prov: {kind, hier, hdl} is validated and counted, not turned into a record (E-3). */
static bool ext_prov(jr *rd, const odin3_json_node *value) {
    if (jr_kind(value) != ODIN3_JSON_OBJECT || !prov_kind_ok(jr_member(value, "kind")) ||
        !is_text(jr_member(value, "hier")) || !prov_hdl_ok(jr_member(value, "hdl"))) {
        return false;
    }
    rd->n_prov++;
    return true;
}

static const odin3_json_ext_key odin3_json_ext_keys[] = {{"odin3_version", ext_version},
                                                         {"odin3_level", ext_level},
                                                         {"odin3_prov", ext_prov},
                                                         {NULL, NULL}};

static const char EXT_PREFIX[] = "odin3_";

static bool is_known(const char *key, const char *const *known, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* True when a table entry takes the key. */
static bool ext_handled(jr *rd, const char *key, const odin3_json_node *value) {
    for (const odin3_json_ext_key *ext = odin3_json_ext_keys; ext->key != NULL; ext++) {
        if (strcmp(ext->key, key) == 0 && ext->handler != NULL) {
            return ext->handler(rd, value);
        }
    }
    return false;
}

odin3_status jr_apply_ext(jr *rd, odin3_module *module, odin3_objref obj) {
    odin3_status st = ODIN3_OK;
    if (rd->ext_level != NULL) {
        odin3_value val = odin3_value_int(jr_int(rd->ext_level));
        st = jr_set_attr_named(rd, module, obj, "odin3_level", &val);
        rd->ext_level = NULL;
    }
    return st;
}

void jr_audit(jr *rd, const odin3_json_node *obj, const char *const *known, size_t count) {
    rd->ext_level = NULL;
    for (const odin3_json_node *member = jr_first(obj); member != NULL;
         member = odin3_json_next(member)) {
        const char *key = odin3_json_key(member);
        if (key == NULL || is_known(key, known, count)) {
            continue;
        }
        if (strncmp(key, EXT_PREFIX, strlen(EXT_PREFIX)) == 0) {
            rd->n_ext += ext_handled(rd, key, member) ? 0U : 1U;
            continue;
        }
        if (rd->n_unknown == 0) {
            (void)snprintf(rd->first_unknown, sizeof rd->first_unknown, "%s", key);
        }
        rd->n_unknown++;
    }
}

odin3_status jr_check_version(const jr *rd, const odin3_json_node *root) {
    const odin3_json_node *version = jr_member(root, "odin3_version");
    if (version != NULL && jr_kind(version) == ODIN3_JSON_INT && jr_int(version) > 1) {
        return jr_error(rd, version, NULL,
                        "odin3_version %lld: written by a newer Odin III (this reader knows 1)",
                        (long long)jr_int(version));
    }
    return ODIN3_OK;
}

void jr_report_skipped(const jr *rd) {
    if (rd->n_prov > 0) {
        odin3_log(ODIN3_LOG_INFO, "%s: read_json validated %u odin3_prov objects (not restored)",
                  rd->path, (unsigned)rd->n_prov);
    }
    if (rd->n_unknown == 0 && rd->n_ext == 0 && rd->n_scopeinfo == 0) {
        return;
    }
    odin3_log(ODIN3_LOG_INFO,
              "%s: read_json skipped %u members the schema does not list%s%s%s, %u odin3_ members "
              "and dropped %u $scopeinfo cells",
              rd->path, (unsigned)rd->n_unknown, rd->n_unknown > 0 ? " (first '" : "",
              rd->n_unknown > 0 ? rd->first_unknown : "", rd->n_unknown > 0 ? "')" : "",
              (unsigned)rd->n_ext, (unsigned)rd->n_scopeinfo);
}
