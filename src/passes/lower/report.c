/*
 * report.c — the `--report <file>` JSON of `lower` (3B spec §10): four lists in node-ID order,
 * `async_to_sync`, `neg_edge`, `mixed_sign` and `mem`, written atomically. 3H's functional check
 * reads `async_to_sync` to know which designs the reset divergence of decision 19 affects.
 */
#include "passes/builtin.h"
#include "passes/lower/lower_int.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static const char *const k_list_names[LW_REP_COUNT] = {"async_to_sync", "neg_edge", "mixed_sign",
                                                       "mem"};

enum { JSON_CONTROL_LIMIT = 0x20 };

/* Appends one escaped byte of a JSON string. */
static odin3_status append_byte(odin3_strbuf *out, unsigned char chr) {
    if (chr == '"' || chr == '\\') {
        return odin3_strbuf_appendf(out, "\\%c", (int)chr);
    }
    if (chr < JSON_CONTROL_LIMIT) {
        return odin3_strbuf_appendf(out, "\\u%04x", (unsigned)chr);
    }
    return odin3_strbuf_appendf(out, "%c", (int)chr);
}

/* Appends "text" with JSON escapes (names may hold any byte but NUL). */
static odin3_status append_string(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_appendf(out, "\"");
    for (const char *ptr = text; st == ODIN3_OK && *ptr != '\0'; ptr++) {
        st = append_byte(out, (unsigned char)*ptr);
    }
    return st == ODIN3_OK ? odin3_strbuf_appendf(out, "\"") : st;
}

/* One "key": "text" member. */
typedef struct json_member {
    const char *key;
    const char *text;
} json_member;

static odin3_status append_member(odin3_strbuf *out, json_member member) {
    odin3_status st = odin3_strbuf_appendf(out, "\"%s\": ", member.key);
    return st == ODIN3_OK ? append_string(out, member.text) : st;
}

/* The members after "cell": the type, the bits and the optional controls. */
static odin3_status append_tail(odin3_strbuf *out, const lw_report_entry *entry) {
    odin3_status st = append_member(out, (json_member){"type", entry->type});
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, ", \"bits\": %" PRIu64, entry->bits) : st;
    if (st == ODIN3_OK && entry->controls != NULL) {
        st = odin3_strbuf_appendf(out, ", \"controls\": [");
        st = st == ODIN3_OK ? append_string(out, entry->controls) : st;
        st = st == ODIN3_OK ? odin3_strbuf_appendf(out, "]") : st;
    }
    return st;
}

odin3_status lw_report_add(lw_ctx *lw, lw_report_kind kind, const lw_report_entry *entry) {
    if (lw->opts == NULL || lw->opts->report_path == NULL) {
        return ODIN3_OK;
    }
    odin3_strbuf *out = &lw->report[kind];
    odin3_status st =
        odin3_strbuf_appendf(out, lw->report_count[kind] == 0 ? "\n    {" : ",\n    {");
    st = st == ODIN3_OK ? append_member(out, (json_member){"module", entry->module}) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, ", ") : st;
    st = st == ODIN3_OK ? append_member(out, (json_member){"cell", entry->cell}) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, ", ") : st;
    st = st == ODIN3_OK ? append_tail(out, entry) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, "}") : st;
    lw->report_count[kind] += st == ODIN3_OK ? 1U : 0U;
    return st;
}

odin3_status lw_report_write(lw_ctx *lw) {
    if (lw->opts == NULL || lw->opts->report_path == NULL) {
        return ODIN3_OK;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_strbuf_appendf(&text, "{");
    for (uint32_t i = 0; st == ODIN3_OK && i < LW_REP_COUNT; i++) {
        const char *body = lw->report[i].data != NULL ? lw->report[i].data : "";
        st = odin3_strbuf_appendf(&text, "%s\n  \"%s\": [%s%s]", i == 0 ? "" : ",", k_list_names[i],
                                  body, lw->report_count[i] > 0 ? "\n  " : "");
    }
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&text, "\n}\n") : st;
    st = st == ODIN3_OK ? odin3_pass_write_text(lw->opts->report_path, &text) : st;
    odin3_strbuf_free(&text);
    return st;
}
