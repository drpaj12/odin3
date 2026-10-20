/*
 * reader_ext.c — members the schema does not list (J-8) and the keys reserved for Phase 6.3's
 * graph export (J-9).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

/* One handler of an extension key. Phase 6.3 fills the table; 5.3 has none, so every key is
 * skipped and counted. A handler returns true when it consumed the member. */
typedef struct odin3_json_ext_key {
    const char *key;
    bool (*handler)(jr *rd, const odin3_json_node *value);
} odin3_json_ext_key;

static const odin3_json_ext_key odin3_json_ext_keys[] = {{NULL, NULL}};

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

void jr_audit(jr *rd, const odin3_json_node *obj, const char *const *known, size_t count) {
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
