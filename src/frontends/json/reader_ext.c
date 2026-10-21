/*
 * reader_ext.c — members the schema does not list (J-8) and the keys reserved for Phase 6.3's
 * graph export (J-9).
 */
#include "frontends/json/reader_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* One handler of an extension key (6.3 E-3). A handler consumes a valid member and returns OK; a
 * malformed member is a located error (our own --odin3 output is never malformed). A key without
 * a handler is skipped and counted. */
typedef struct odin3_json_ext_key {
    const char *key;
    odin3_status (*handler)(jr *rd, const odin3_json_node *value);
} odin3_json_ext_key;

enum { PROV_HDL_MAX = 8 };

/* odin3_level: an integer 0..INT32_MAX, set as the attribute odin3_level once the object exists. */
static odin3_status ext_level(jr *rd, const odin3_json_node *value) {
    if (jr_kind(value) != ODIN3_JSON_INT || jr_int(value) < 0 || jr_int(value) > INT32_MAX) {
        return jr_error(rd, value, NULL, "odin3_level is not an integer from 0 to %d", INT32_MAX);
    }
    rd->ext_level = value;
    return ODIN3_OK;
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
static odin3_status ext_prov(jr *rd, const odin3_json_node *value) {
    if (jr_kind(value) != ODIN3_JSON_OBJECT || !prov_kind_ok(jr_member(value, "kind")) ||
        !is_text(jr_member(value, "hier")) || !prov_hdl_ok(jr_member(value, "hdl"))) {
        return jr_error(rd, value, NULL,
                        "odin3_prov is not {kind: SOURCE|IMPORTED|DERIVED, hier: text, hdl: up to "
                        "%d texts}",
                        (int)PROV_HDL_MAX);
    }
    rd->n_prov++;
    return ODIN3_OK;
}

/* odin3_version is read by jr_check_version and, as before, counted with the skipped members. */
static const odin3_json_ext_key odin3_json_ext_keys[] = {
    {"odin3_level", ext_level}, {"odin3_prov", ext_prov}, {NULL, NULL}};

static const char EXT_PREFIX[] = "odin3_";

static bool is_known(const char *key, const char *const *known, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Runs the table entry of the key; *handled is false when there is none. */
static odin3_status ext_handle(jr *rd, const odin3_json_node *member, bool *handled) {
    const char *key = odin3_json_key(member);
    for (const odin3_json_ext_key *ext = odin3_json_ext_keys; ext->key != NULL; ext++) {
        if (strcmp(ext->key, key) == 0 && ext->handler != NULL) {
            *handled = true;
            return ext->handler(rd, member);
        }
    }
    *handled = false;
    return ODIN3_OK;
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

odin3_status jr_audit(jr *rd, const odin3_json_node *obj, const char *const *known,
                      size_t count) {
    odin3_status st = ODIN3_OK;
    rd->ext_level = NULL;
    for (const odin3_json_node *member = jr_first(obj); st == ODIN3_OK && member != NULL;
         member = odin3_json_next(member)) {
        const char *key = odin3_json_key(member);
        if (key == NULL || is_known(key, known, count)) {
            continue;
        }
        if (strncmp(key, EXT_PREFIX, strlen(EXT_PREFIX)) == 0) {
            bool handled = false;
            st = ext_handle(rd, member, &handled);
            rd->n_ext += handled ? 0U : 1U;
            continue;
        }
        if (rd->n_unknown == 0) {
            (void)snprintf(rd->first_unknown, sizeof rd->first_unknown, "%s", key);
        }
        rd->n_unknown++;
    }
    return st;
}

odin3_status jr_check_version(const jr *rd, const odin3_json_node *root) {
    const odin3_json_node *version = jr_member(root, "odin3_version");
    if (version == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(version) != ODIN3_JSON_INT) {
        return jr_error(rd, version, NULL, "odin3_version is not an integer (this reader knows 1)");
    }
    if (jr_int(version) > 1) {
        return jr_error(rd, version, NULL,
                        "odin3_version %lld: written by a newer Odin III (this reader knows 1)",
                        (long long)jr_int(version));
    }
    if (jr_int(version) != 1) {
        return jr_error(rd, version, NULL, "odin3_version %lld: not a version (this reader knows 1)",
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
