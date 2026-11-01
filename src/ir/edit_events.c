/* edit_events.c — edit events of a node or net (edit_events.h; spec edit-gaps section 2). */
#include "ir/edit_events.h"

#include "ir/design.h"
#include "ir/value.h"
#include "util/hash.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { HEX_BASE = 16, HEX_TEN = 10, NIBBLE_BITS = 4, ESC_LEN = 3, CTRL_LAST = 0x20, DEL = 0x7f };

static const char K_HEX[] = "0123456789ABCDEF";

static bool has_char(const char *text, char chr) {
    return strchr(text, chr) != NULL;
}

static uint32_t events_key(const odin3_module *module) {
    uint32_t key = 0;
    const odin3_strtab *tab = odin3_design_strtab(odin3_module_design(module));
    return odin3_strtab_find(tab, odin3_bytes_cstr(ODIN3_ATTR_EDIT_EVENTS), &key) ? key : 0;
}

const char *odin3_edit_events_raw(const odin3_module *module, odin3_objref obj) {
    uint32_t key = events_key(module);
    const odin3_value *val = key == 0 ? NULL : odin3_attr_get(module, obj, key);
    if (val == NULL || val->kind != ODIN3_VAL_STRING) {
        return NULL;
    }
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), val->str);
}

/* The tag of the raw line at [line, line + len): after its first blank. */
static odin3_bytes line_tag(const char *line, size_t len) {
    const char *space = memchr(line, ' ', len);
    if (space == NULL) {
        return (odin3_bytes){NULL, 0};
    }
    return (odin3_bytes){space + 1, len - (size_t)(space - line) - 1U};
}

bool odin3_edit_events_first_tag(const odin3_module *module, odin3_objref obj, odin3_bytes *tag) {
    const char *raw = odin3_edit_events_raw(module, obj);
    bool found = false;
    for (const char *at = raw; at != NULL && *at != '\0'; at += strcspn(at, "\n") + 1U) {
        size_t len = strcspn(at, "\n");
        odin3_bytes cand = line_tag(at, len);
        bool added = strncmp(at, ODIN3_EDIT_ADDED " ", strlen(ODIN3_EDIT_ADDED) + 1U) == 0;
        if (cand.ptr != NULL && (!found || added)) {
            *tag = cand;
            found = true;
        }
        if (added || at[len] == '\0') {
            break;
        }
    }
    return found;
}

odin3_status odin3_edit_events_set_raw(odin3_module *module, odin3_objref obj, odin3_bytes raw) {
    odin3_design *design = odin3_module_design(module);
    uint32_t key = 0;
    uint32_t unused = 0;
    odin3_value val = {.kind = ODIN3_VAL_STRING};
    const char *text = raw.ptr;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(ODIN3_ATTR_EDIT_EVENTS), &key);
    for (size_t at = 0; st == ODIN3_OK && at < raw.len;) {
        size_t len = strcspn(text + at, "\n");
        len = at + len > raw.len ? raw.len - at : len;
        odin3_bytes tag = line_tag(text + at, len);
        st = tag.ptr == NULL ? ODIN3_OK : odin3_design_intern(design, tag, &unused);
        at += len + 1U;
    }
    if (st == ODIN3_OK) {
        st = odin3_design_intern(design, raw, &val.str);
    }
    return st == ODIN3_OK ? odin3_attr_set(module, obj, key, &val) : st;
}

/* True when the raw lines already hold line (kind, a blank, tag, a newline). */
static bool has_line(const char *raw, const odin3_strbuf *line) {
    for (const char *at = raw; at != NULL && *at != '\0'; at += strcspn(at, "\n") + 1U) {
        size_t len = strcspn(at, "\n");
        if (len + 1U == line->len && memcmp(at, line->data, len) == 0) {
            return true;
        }
        if (at[len] == '\0') {
            break;
        }
    }
    return false;
}

/* Sets obj's events to old (may be NULL) followed by line. */
static odin3_status append_line(odin3_module *module, odin3_objref obj, const char *old,
                                const odin3_strbuf *line) {
    odin3_strbuf all;
    odin3_strbuf_init(&all);
    odin3_status st = old == NULL ? ODIN3_OK : odin3_strbuf_append(&all, odin3_bytes_cstr(old));
    st = st == ODIN3_OK ? odin3_strbuf_append(&all, (odin3_bytes){line->data, line->len}) : st;
    st = st == ODIN3_OK ? odin3_edit_events_set_raw(module, obj, (odin3_bytes){all.data, all.len})
                        : st;
    odin3_strbuf_free(&all);
    return st;
}

odin3_status odin3_edit_event_add(odin3_module *module, odin3_objref obj, const char *kind,
                                  const char *tag) {
    if (kind == NULL || kind[0] == '\0' || has_char(kind, ' ') || has_char(kind, '\n') ||
        tag == NULL || tag[0] == '\0' || has_char(tag, '\n')) {
        odin3_log(ODIN3_LOG_ERROR, "edit event: the kind and tag must be non-empty, the tag "
                                   "without a newline");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    const char *old = odin3_edit_events_raw(module, obj);
    odin3_status st = odin3_strbuf_appendf(&line, "%s %s\n", kind, tag);
    if (st == ODIN3_OK && !has_line(old, &line)) { /* the same line twice is one event */
        st = append_line(module, obj, old, &line);
    }
    odin3_strbuf_free(&line);
    return st;
}

bool odin3_edit_events_wellformed(const char *raw) {
    if (raw == NULL || raw[0] == '\0') {
        return false;
    }
    for (const char *at = raw; *at != '\0'; at += strcspn(at, "\n") + 1U) {
        size_t len = strcspn(at, "\n");
        const char *space = memchr(at, ' ', len);
        if (space == NULL || space == at || space + 1 == at + len || at[len] == '\0') {
            return false; /* a line is `kind tag\n`: both parts, and the newline */
        }
    }
    return true;
}

/* --- the export token ----------------------------------------------------------------------- */

static bool must_escape(unsigned char chr) {
    return chr <= CTRL_LAST || chr >= DEL || chr == '%' || chr == ',' || chr == '#' || chr == '"' ||
           chr == '\\';
}

static odin3_status put_escaped(odin3_strbuf *out, const char *text, size_t len) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < len; i++) {
        unsigned char chr = (unsigned char)text[i];
        if (must_escape(chr)) {
            char esc[ESC_LEN] = {'%', K_HEX[chr >> NIBBLE_BITS], K_HEX[chr & (HEX_BASE - 1U)]};
            st = odin3_strbuf_append(out, (odin3_bytes){esc, ESC_LEN});
        } else {
            st = odin3_strbuf_append(out, (odin3_bytes){&text[i], 1});
        }
    }
    return st;
}

odin3_status odin3_edit_events_encode(const char *raw, odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    bool first = true;
    if (raw != NULL && !odin3_edit_events_wellformed(raw)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (const char *at = raw; st == ODIN3_OK && at != NULL && *at != '\0';) {
        size_t len = strcspn(at, "\n");
        const char *space = memchr(at, ' ', len);
        st = first ? ODIN3_OK : odin3_strbuf_append(out, odin3_bytes_cstr(","));
        st =
            st == ODIN3_OK ? odin3_strbuf_append(out, (odin3_bytes){at, (size_t)(space - at)}) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(":")) : st;
        st = st == ODIN3_OK ? put_escaped(out, space + 1, len - (size_t)(space - at) - 1U) : st;
        first = false;
        at += len + (at[len] == '\n' ? 1U : 0U);
    }
    return st;
}

static int hex_digit(char chr) {
    if (chr >= '0' && chr <= '9') {
        return chr - '0';
    }
    if (chr >= 'A' && chr <= 'F') {
        return chr - 'A' + HEX_TEN;
    }
    return -1;
}

/* The character of the escape text[at..at+3) (a '%' and two hex digits); -1 when malformed. */
static int unescape_at(const char *text, size_t at, size_t len) {
    if (at + ESC_LEN > len) {
        return -1;
    }
    int high = hex_digit(text[at + 1]);
    int low = hex_digit(text[at + 2]);
    return high < 0 || low < 0 ? -1 : (high << NIBBLE_BITS) | low;
}

/* Appends the decoded tag text[0..len) to out; false on a bad escape, a NUL or a newline. */
static bool decode_tag(odin3_strbuf *out, const char *text, size_t len, odin3_status *st) {
    for (size_t i = 0; *st == ODIN3_OK && i < len; i++) {
        int code = (unsigned char)text[i];
        if (text[i] == '%') {
            code = unescape_at(text, i, len);
            i += ESC_LEN - 1U;
        }
        if (code <= 0 || code == '\n') {
            return false; /* a tag has no NUL (a bad escape is -1) and no newline */
        }
        char chr = (char)code;
        *st = odin3_strbuf_append(out, (odin3_bytes){&chr, 1});
    }
    return true;
}

odin3_status odin3_edit_events_decode(odin3_bytes token, odin3_strbuf *out) {
    const char *text = token.ptr;
    odin3_status st = ODIN3_OK;
    size_t pos = 0;
    if (token.len == 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    while (st == ODIN3_OK && pos <= token.len) {
        const char *item = text + pos;
        size_t len = 0;
        while (pos + len < token.len && item[len] != ',') {
            len++;
        }
        const char *colon = memchr(item, ':', len);
        if (colon == NULL || colon == item || colon + 1 == item + len) {
            return ODIN3_ERR_INVALID_ARG;
        }
        size_t kind_len = (size_t)(colon - item);
        st = odin3_strbuf_append(out, (odin3_bytes){item, kind_len});
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(" ")) : st;
        if (!decode_tag(out, colon + 1, len - kind_len - 1U, &st)) {
            return ODIN3_ERR_INVALID_ARG;
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n")) : st;
        pos += len + 1U;
    }
    return st;
}
