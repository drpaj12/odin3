/*
 * xml.c — the subset XML reader (PRJ-15; xml.h): one forward scan over the bytes. The open
 * elements form an explicit stack, the innermost in xp->cur and the rest through each element's
 * parent index; nothing recurses, so the depth costs memory only (bounded by the node cap).
 * Characters XML forbids are found first (xml_limit) and the scan stops before the first of them,
 * so an error the scan meets earlier is still the one reported.
 */
#include "frontends/project/xml.h"

#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    NAME_SHOWN = 64, /* bytes of a name an error message shows before "..." */
    ASCII_END = 0x80,
    C0_END = 0x20,
    UTF8_CONT_MASK = 0xC0,
    UTF8_CONT = 0x80,
    LEAD2 = 0xC0,
    LEAD3 = 0xE0,
    LEAD4 = 0xF0,
    LEAD_END = 0xF8,
    BITS2 = 0x1F,
    BITS3 = 0x0F,
    BITS4 = 0x07,
    CONT_BITS = 0x3F,
    CONT_SHIFT = 6,
    SEQ2 = 2,
    SEQ3 = 3,
    SEQ4 = 4,
    CP2_MIN = 0x80,
    CP3_MIN = 0x800,
    CP4_MIN = 0x10000,
    CP_MAX = 0x10FFFF,
    SURROGATE_LO = 0xD800,
    SURROGATE_HI = 0xDFFF,
    NONCHAR_LO = 0xFFFE, /* U+FFFE and U+FFFF are not XML characters */
    SHIFT2 = 12,
    SHIFT3 = 18,
    DEC_BASE = 10,
    HEX_BASE = 16,
    HEX_LETTER = 10, /* the value of 'a' */
    BOM_LEN = 3,
    COMMENT_OPEN = 4,  /* "<!--" */
    CDATA_OPEN = 9,    /* "<![CDATA[" */
    CDATA_CLOSE = 3,   /* "]]>" */
    XML_DECL_OPEN = 5, /* "<?xml" */
    DECL_CLOSE = 2,    /* "?>" */
    END_OPEN = 2       /* "</" */
};

/* How a run of character data is decoded. */
typedef enum xml_mode {
    MODE_TEXT, /* references, CRLF/CR → LF, "]]>" rejected */
    MODE_ATTR, /* references, TAB/LF/CR/CRLF → one space */
    MODE_CDATA /* CRLF/CR → LF only */
} xml_mode;

/* Where a start tag stands after one step. */
typedef enum tag_state { TAG_MORE, TAG_OPEN, TAG_EMPTY } tag_state;

typedef struct xml_parser {
    const uint8_t *bytes;
    uint32_t len;  /* the scan's end: the first character XML forbids, else the text's length */
    uint32_t full; /* the text's length */
    uint32_t pos;
    uint32_t cur; /* the innermost open element (the top of the stack), ODIN3_XML_NONE outside */
    odin3_xml_doc *doc;
    odin3_xml_err *err;
    odin3_u64map *names;        /* hash(attribute name) ⊕ element → attribute index */
    uint32_t line_off, line_no; /* line_at's cursor */
} xml_parser;

/* A span of the text. */
typedef struct xml_span {
    uint32_t from, to;
} xml_span;

/* --- characters ---------------------------------------------------------------------------- */

/* XML 1.0's Char production. */
static bool xml_char(uint32_t cp) {
    if (cp < C0_END) {
        return cp == '\t' || cp == '\n' || cp == '\r';
    }
    return cp < SURROGATE_LO || (cp > SURROGATE_HI && cp < NONCHAR_LO) ||
           (cp >= CP4_MIN && cp <= CP_MAX);
}

/* A UTF-8 lead byte (0x80 or above): its sequence length (0 for none), payload and minimum. */
typedef struct utf8_lead {
    uint32_t seq, cp, min;
} utf8_lead;

static utf8_lead lead_of(uint8_t lead) {
    if (lead >= LEAD2 && lead < LEAD3) {
        return (utf8_lead){SEQ2, lead & (uint32_t)BITS2, CP2_MIN};
    }
    if (lead >= LEAD3 && lead < LEAD4) {
        return (utf8_lead){SEQ3, lead & (uint32_t)BITS3, CP3_MIN};
    }
    if (lead >= LEAD4 && lead < LEAD_END) {
        return (utf8_lead){SEQ4, lead & (uint32_t)BITS4, CP4_MIN};
    }
    return (utf8_lead){0, 0, 0};
}

/* The length of the UTF-8 sequence at bytes[pos] (pos < len) holding an XML character, 0 when
 * there is none (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF). */
static uint32_t char_len(const uint8_t *bytes, uint32_t len, uint32_t pos) {
    if (bytes[pos] < ASCII_END) {
        return xml_char(bytes[pos]) ? 1 : 0;
    }
    utf8_lead lead = lead_of(bytes[pos]);
    if (lead.seq == 0 || len - pos < lead.seq) {
        return 0;
    }
    uint32_t cp = lead.cp;
    for (uint32_t i = 1; i < lead.seq; i++) {
        if ((bytes[pos + i] & UTF8_CONT_MASK) != UTF8_CONT) {
            return 0;
        }
        cp = cp << CONT_SHIFT | (bytes[pos + i] & (uint32_t)CONT_BITS);
    }
    return cp >= lead.min && xml_char(cp) ? lead.seq : 0;
}

/* The offset of the first character XML forbids, len when there is none. */
static uint32_t xml_limit(const uint8_t *bytes, uint32_t len) {
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t seq = char_len(bytes, len, pos);
        if (seq == 0) {
            return pos;
        }
        pos += seq;
    }
    return len;
}

/* Writes cp as UTF-8 to out; returns the byte count. */
static uint32_t put_utf8(uint8_t *out, uint32_t cp) {
    if (cp < CP2_MIN) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < CP3_MIN) {
        out[0] = (uint8_t)(LEAD2 | cp >> CONT_SHIFT);
        out[1] = (uint8_t)(UTF8_CONT | (cp & CONT_BITS));
        return SEQ2;
    }
    if (cp < CP4_MIN) {
        out[0] = (uint8_t)(LEAD3 | cp >> SHIFT2);
        out[1] = (uint8_t)(UTF8_CONT | ((cp >> CONT_SHIFT) & CONT_BITS));
        out[2] = (uint8_t)(UTF8_CONT | (cp & CONT_BITS));
        return SEQ3;
    }
    out[0] = (uint8_t)(LEAD4 | cp >> SHIFT3);
    out[1] = (uint8_t)(UTF8_CONT | ((cp >> SHIFT2) & CONT_BITS));
    out[2] = (uint8_t)(UTF8_CONT | ((cp >> CONT_SHIFT) & CONT_BITS));
    out[3] = (uint8_t)(UTF8_CONT | (cp & CONT_BITS));
    return SEQ4;
}

static bool is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static bool is_alpha(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* A name's first byte: a letter, '_', ':' or any byte of a (valid) non-ASCII character. */
static bool name_start(uint8_t byte) {
    return is_alpha(byte) || byte == '_' || byte == ':' || byte >= ASCII_END;
}

static bool name_byte(uint8_t byte) {
    return name_start(byte) || (byte >= '0' && byte <= '9') || byte == '.' || byte == '-';
}

/* --- errors -------------------------------------------------------------------------------- */

/* The line of off (1-based, lines end at LF), from a cursor that only moves forward between
 * calls with increasing offsets. */
static uint32_t line_at(xml_parser *xp, uint32_t off) {
    if (off < xp->line_off) {
        xp->line_off = 0;
        xp->line_no = 1;
    }
    for (; xp->line_off < off; xp->line_off++) {
        xp->line_no += xp->bytes[xp->line_off] == '\n';
    }
    return xp->line_no;
}

/* Appends part to the error message, cut at its end. */
static void what_add(odin3_xml_err *err, odin3_bytes part) {
    size_t used = strlen(err->what);
    size_t room = sizeof err->what - 1 - used;
    size_t len = part.len < room ? part.len : room;
    if (len > 0) {
        memcpy(err->what + used, part.ptr, len);
    }
    err->what[used + len] = '\0';
}

/* Appends a name, cut at NAME_SHOWN bytes (less to end on a character) with "..." after it. */
static void what_name(odin3_xml_err *err, odin3_bytes name) {
    const uint8_t *bytes = name.ptr;
    bool cut = name.len > NAME_SHOWN;
    size_t shown = cut ? (size_t)NAME_SHOWN : name.len;
    while (cut && shown > 0 && (bytes[shown] & UTF8_CONT_MASK) == UTF8_CONT) {
        shown--; /* cut on a character boundary: the message stays UTF-8 */
    }
    what_add(err, (odin3_bytes){name.ptr, shown});
    if (cut) {
        what_add(err, odin3_bytes_cstr("..."));
    }
}

/* PARSE at off with the message pre + name + post (name omitted when NULL). */
static odin3_status fail_name(xml_parser *xp, uint32_t off, const char *pre,
                              const odin3_bytes *name, const char *post) {
    xp->err->off = off;
    xp->err->line = line_at(xp, off);
    xp->err->what[0] = '\0';
    what_add(xp->err, odin3_bytes_cstr(pre));
    if (name != NULL) {
        what_name(xp->err, *name);
        what_add(xp->err, odin3_bytes_cstr(post));
    }
    return ODIN3_ERR_PARSE;
}

static odin3_status fail(xml_parser *xp, uint32_t off, const char *what) {
    return fail_name(xp, off, what, NULL, NULL);
}

/* The text ended early at a construct starting at off: a forbidden character stopped the scan
 * there, else the construct is unclosed. */
static odin3_status early_end(xml_parser *xp, uint32_t off, const char *what) {
    return xp->len < xp->full ? fail(xp, xp->len, "invalid character") : fail(xp, off, what);
}

/* --- the tree ------------------------------------------------------------------------------ */

static odin3_xml_elem *elem_at(xml_parser *xp, uint32_t index) {
    return odin3_vec_at(&xp->doc->elems, index);
}

/* The node cap, checked before a node at off is stored. */
static odin3_status node_room(xml_parser *xp, uint32_t off) {
    const odin3_xml_doc *doc = xp->doc;
    size_t nodes = doc->elems.len + doc->attrs.len + doc->texts.len;
    if (nodes < doc->max_nodes) {
        return ODIN3_OK;
    }
    char what[ODIN3_XML_WHAT_MAX];
    (void)snprintf(what, sizeof what, "more than %lu elements, attributes and text runs",
                   (unsigned long)doc->max_nodes);
    xp->err->over_cap = true;
    return fail(xp, off, what);
}

/* A new element (tag at lt) under the innermost open one; *index = its index. */
static odin3_status new_elem(xml_parser *xp, odin3_bytes tag, uint32_t lt, uint32_t *index) {
    odin3_status res = node_room(xp, lt);
    odin3_xml_elem *elem = res == ODIN3_OK ? odin3_vec_push(&xp->doc->elems) : NULL;
    if (elem == NULL) {
        return res != ODIN3_OK ? res : ODIN3_ERR_NO_MEMORY;
    }
    *index = (uint32_t)xp->doc->elems.len - 1;
    *elem = (odin3_xml_elem){.tag = tag,
                             .off = lt,
                             .line = line_at(xp, lt),
                             .parent = xp->cur,
                             .first_child = ODIN3_XML_NONE,
                             .next_sibling = ODIN3_XML_NONE,
                             .attrs = (uint32_t)xp->doc->attrs.len,
                             .nattrs = 0,
                             .first_text = ODIN3_XML_NONE,
                             .last_child = ODIN3_XML_NONE,
                             .last_text = ODIN3_XML_NONE};
    if (xp->cur != ODIN3_XML_NONE) {
        odin3_xml_elem *parent = elem_at(xp, xp->cur);
        if (parent->last_child == ODIN3_XML_NONE) {
            parent->first_child = *index;
        } else {
            elem_at(xp, parent->last_child)->next_sibling = *index;
        }
        parent->last_child = *index;
    }
    return ODIN3_OK;
}

/* Appends a run of the innermost open element's character data (found at off). */
static odin3_status add_text(xml_parser *xp, odin3_bytes data, uint32_t off) {
    odin3_status res = node_room(xp, off);
    odin3_xml_text *text = res == ODIN3_OK ? odin3_vec_push(&xp->doc->texts) : NULL;
    if (text == NULL) {
        return res != ODIN3_OK ? res : ODIN3_ERR_NO_MEMORY;
    }
    uint32_t index = (uint32_t)xp->doc->texts.len - 1;
    *text = (odin3_xml_text){data, ODIN3_XML_NONE};
    odin3_xml_elem *elem = elem_at(xp, xp->cur);
    if (elem->last_text == ODIN3_XML_NONE) {
        elem->first_text = index;
    } else {
        odin3_xml_text *prev = odin3_vec_at(&xp->doc->texts, elem->last_text);
        prev->next = index;
    }
    elem->last_text = index;
    return ODIN3_OK;
}

/* --- references and decoding --------------------------------------------------------------- */

static const struct {
    const char *name;
    char byte;
} k_entities[] = {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};

/* The value of a decimal digit, -1 when byte is none. */
static int dec_value(uint8_t byte) {
    return byte >= '0' && byte <= '9' ? byte - '0' : -1;
}

/* The value of a hex digit (either case), -1 when byte is none. */
static int hex_value(uint8_t byte) {
    uint8_t lower = (uint8_t)(byte | ('a' - 'A'));
    if (lower >= 'a' && lower <= 'f') {
        return lower - 'a' + HEX_LETTER;
    }
    return dec_value(byte);
}

/* A numeric reference "&#N;" or "&#xH;" at amp (inside span): its UTF-8 bytes to out. */
static odin3_status char_ref(xml_parser *xp, uint32_t *at, xml_span span, uint8_t *out,
                             uint32_t *written) {
    uint32_t amp = *at;
    uint32_t pos = amp + 2;
    uint32_t base = pos < span.to && xp->bytes[pos] == 'x' ? HEX_BASE : DEC_BASE;
    pos += base == HEX_BASE;
    uint32_t first = pos;
    uint32_t cp = 0;
    while (pos < span.to) {
        int value = base == HEX_BASE ? hex_value(xp->bytes[pos]) : dec_value(xp->bytes[pos]);
        if (value < 0) {
            break;
        }
        cp = cp > CP_MAX ? cp : cp * base + (uint32_t)value; /* stops growing past CP_MAX */
        pos++;
    }
    if (pos == first || pos >= span.to || xp->bytes[pos] != ';' || cp > CP_MAX || !xml_char(cp)) {
        return fail(xp, amp, "bad character reference");
    }
    *written = put_utf8(out, cp);
    *at = pos + 1;
    return ODIN3_OK;
}

/* A reference at *at ('&', inside span): its bytes to out, *at past its ';'. */
static odin3_status reference(xml_parser *xp, uint32_t *at, xml_span span, uint8_t *out,
                              uint32_t *written) {
    uint32_t amp = *at;
    if (amp + 1 < span.to && xp->bytes[amp + 1] == '#') {
        return char_ref(xp, at, span, out, written);
    }
    uint32_t end = amp + 1;
    while (end < span.to && name_byte(xp->bytes[end])) {
        end++;
    }
    if (end == amp + 1 || end >= span.to || xp->bytes[end] != ';') {
        return fail(xp, amp, "unterminated reference");
    }
    odin3_bytes name = {xp->bytes + amp + 1, end - amp - 1};
    for (size_t k = 0; k < sizeof k_entities / sizeof k_entities[0]; k++) {
        if (name.len == strlen(k_entities[k].name) &&
            memcmp(name.ptr, k_entities[k].name, name.len) == 0) {
            out[0] = (uint8_t)k_entities[k].byte;
            *written = 1;
            *at = end + 1;
            return ODIN3_OK;
        }
    }
    return fail_name(xp, amp, "undefined entity &", &name, ";");
}

/* Whether span needs decoding in mode (else it is used as it stands). */
static bool needs_decode(const xml_parser *xp, xml_span span, xml_mode mode) {
    for (uint32_t i = span.from; i < span.to; i++) {
        uint8_t byte = xp->bytes[i];
        bool special = byte == '\r' || (mode != MODE_CDATA && byte == '&') ||
                       (mode == MODE_TEXT && byte == ']') ||
                       (mode == MODE_ATTR && (byte == '\t' || byte == '\n'));
        if (special) {
            return true;
        }
    }
    return false;
}

/* One plain byte at *at, newlines normalized for mode. */
static uint8_t plain_byte(const xml_parser *xp, uint32_t *at, xml_span span, xml_mode mode) {
    uint8_t byte = xp->bytes[(*at)++];
    if (byte == '\r') {
        byte = '\n';
        *at += *at < span.to && xp->bytes[*at] == '\n';
    }
    if (mode == MODE_ATTR && (byte == '\t' || byte == '\n')) {
        byte = ' ';
    }
    return byte;
}

/* Decodes span in mode: a view of the text when nothing changes, else a copy in the arena
 * (never longer than the span: every reference is longer than its UTF-8). */
static odin3_status decode(xml_parser *xp, xml_span span, xml_mode mode, odin3_bytes *out) {
    *out = (odin3_bytes){xp->bytes + span.from, span.to - span.from};
    if (!needs_decode(xp, span, mode)) {
        return ODIN3_OK;
    }
    uint8_t *buf = odin3_arena_alloc(xp->doc->arena, span.to - span.from);
    if (buf == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t len = 0;
    for (uint32_t i = span.from; i < span.to;) {
        uint8_t byte = xp->bytes[i];
        uint32_t written = 0;
        odin3_status res = ODIN3_OK;
        if (byte == '&' && mode != MODE_CDATA) {
            res = reference(xp, &i, span, buf + len, &written);
        } else if (mode == MODE_TEXT && byte == ']' && span.to - i >= CDATA_CLOSE &&
                   memcmp(xp->bytes + i, "]]>", CDATA_CLOSE) == 0) {
            res = fail(xp, i, "']]>' in character data");
        } else {
            buf[len] = plain_byte(xp, &i, span, mode);
            written = 1;
        }
        if (res != ODIN3_OK) {
            return res;
        }
        len += written;
    }
    *out = (odin3_bytes){buf, len};
    return ODIN3_OK;
}

/* --- scanning ------------------------------------------------------------------------------ */

static bool starts_at(const xml_parser *xp, uint32_t at, const char *lit) {
    size_t len = strlen(lit);
    return xp->len - at >= len && memcmp(xp->bytes + at, lit, len) == 0;
}

/* The offset of lit at or after from, ODIN3_XML_NONE when it does not occur before the end. */
static uint32_t find(const xml_parser *xp, uint32_t from, const char *lit) {
    for (uint32_t at = from; at < xp->len; at++) {
        if (starts_at(xp, at, lit)) {
            return at;
        }
    }
    return ODIN3_XML_NONE;
}

/* Skips blanks; whether there were any. */
static bool skip_space(xml_parser *xp) {
    uint32_t from = xp->pos;
    while (xp->pos < xp->len && is_space(xp->bytes[xp->pos])) {
        xp->pos++;
    }
    return xp->pos > from;
}

/* The name at pos (empty when none starts there), pos moved past it. */
static odin3_bytes scan_name(xml_parser *xp) {
    uint32_t from = xp->pos;
    if (xp->pos < xp->len && name_start(xp->bytes[xp->pos])) {
        while (xp->pos < xp->len && name_byte(xp->bytes[xp->pos])) {
            xp->pos++;
        }
    }
    return (odin3_bytes){xp->bytes + from, xp->pos - from};
}

/* "<?xml" and a blank at at: the XML declaration. */
static bool is_xml_decl(const xml_parser *xp, uint32_t at) {
    return starts_at(xp, at, "<?xml") && at + XML_DECL_OPEN < xp->len &&
           is_space(xp->bytes[at + XML_DECL_OPEN]);
}

/* A UTF-8 byte order mark, then an XML declaration, both skipped. */
static odin3_status prolog(xml_parser *xp) {
    if (starts_at(xp, 0, "\xEF\xBB\xBF")) {
        xp->pos = BOM_LEN;
    }
    if (!is_xml_decl(xp, xp->pos)) {
        return ODIN3_OK;
    }
    uint32_t end = find(xp, xp->pos, "?>");
    if (end == ODIN3_XML_NONE) {
        return early_end(xp, xp->pos, "unclosed XML declaration");
    }
    xp->pos = end + DECL_CLOSE;
    return ODIN3_OK;
}

static odin3_status comment(xml_parser *xp) {
    uint32_t lt = xp->pos;
    uint32_t dash = find(xp, lt + COMMENT_OPEN, "--");
    if (dash == ODIN3_XML_NONE || dash + 2 >= xp->len) {
        return early_end(xp, lt, "unclosed comment");
    }
    if (xp->bytes[dash + 2] != '>') {
        return fail(xp, dash, "'--' inside a comment");
    }
    xp->pos = dash + 3;
    return ODIN3_OK;
}

static odin3_status cdata(xml_parser *xp) {
    uint32_t lt = xp->pos;
    if (xp->cur == ODIN3_XML_NONE) {
        return fail(xp, lt, "CDATA outside the root element");
    }
    uint32_t end = find(xp, lt + CDATA_OPEN, "]]>");
    if (end == ODIN3_XML_NONE) {
        return early_end(xp, lt, "unclosed CDATA section");
    }
    xp->pos = end + CDATA_CLOSE;
    xml_span span = {lt + CDATA_OPEN, end};
    if (span.from == span.to) {
        return ODIN3_OK;
    }
    odin3_bytes data = {NULL, 0};
    odin3_status res = decode(xp, span, MODE_CDATA, &data);
    return res != ODIN3_OK ? res : add_text(xp, data, lt);
}

/* Character data up to the next '<': only blanks outside the root element. */
static odin3_status char_data(xml_parser *xp) {
    xml_span span = {xp->pos, xp->pos};
    while (span.to < xp->len && xp->bytes[span.to] != '<') {
        span.to++;
    }
    xp->pos = span.to;
    if (xp->cur == ODIN3_XML_NONE) {
        for (uint32_t i = span.from; i < span.to; i++) {
            if (!is_space(xp->bytes[i])) {
                return fail(xp, i,
                            xp->doc->elems.len > 0 ? "text after the root element"
                                                   : "text before the root element");
            }
        }
        return ODIN3_OK;
    }
    odin3_bytes data = {NULL, 0};
    odin3_status res = decode(xp, span, MODE_TEXT, &data);
    return res != ODIN3_OK ? res : add_text(xp, data, span.from);
}

/* --- tags ---------------------------------------------------------------------------------- */

/* Whether element index already has an attribute named name. */
static bool has_attr(const xml_parser *xp, uint32_t index, odin3_bytes name) {
    const odin3_xml_elem *elem = odin3_vec_cat(&xp->doc->elems, index);
    for (uint32_t i = 0; i < elem->nattrs; i++) {
        const odin3_xml_attr *attr = odin3_vec_cat(&xp->doc->attrs, elem->attrs + i);
        if (attr->name.len == name.len && memcmp(attr->name.ptr, name.ptr, name.len) == 0) {
            return true;
        }
    }
    return false;
}

/* A repeated attribute name is an error; names are found by hash (a scan only on a collision). */
static odin3_status check_repeat(xml_parser *xp, uint32_t index, odin3_bytes name, uint32_t off) {
    if (xp->names == NULL) {
        xp->names = odin3_u64map_create(0);
        if (xp->names == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    uint64_t key = odin3_hash_combine(odin3_hash_bytes(name, ODIN3_HASH_SEED), index);
    uint64_t prev = 0;
    if (!odin3_u64map_get(xp->names, key, &prev)) {
        return odin3_u64map_put(xp->names, (odin3_kv){key, xp->doc->attrs.len});
    }
    const odin3_xml_attr *attr = odin3_vec_cat(&xp->doc->attrs, (size_t)prev);
    bool same = attr->name.len == name.len && memcmp(attr->name.ptr, name.ptr, name.len) == 0;
    if (same || has_attr(xp, index, name)) {
        return fail_name(xp, off, "duplicate attribute ", &name, "");
    }
    return ODIN3_OK; /* a collision with another name: its own repeat is found by has_attr */
}

/* Stores attr (its name at attr.off) of element index, its value decoded from the span. */
static odin3_status add_attr(xml_parser *xp, uint32_t index, odin3_xml_attr attr, xml_span value) {
    odin3_status res = node_room(xp, attr.off); /* the cap before check_repeat's allocation */
    res = res != ODIN3_OK ? res : check_repeat(xp, index, attr.name, attr.off);
    res = res != ODIN3_OK ? res : decode(xp, value, MODE_ATTR, &attr.value);
    odin3_xml_attr *slot = res == ODIN3_OK ? odin3_vec_push(&xp->doc->attrs) : NULL;
    if (slot == NULL) {
        return res != ODIN3_OK ? res : ODIN3_ERR_NO_MEMORY;
    }
    *slot = attr;
    elem_at(xp, index)->nattrs++;
    return ODIN3_OK;
}

/* name = "value" or name = 'value' at pos, in the start tag of element index. */
static odin3_status attribute(xml_parser *xp, uint32_t index) {
    uint32_t lt = elem_at(xp, index)->off;
    odin3_xml_attr attr = {.off = xp->pos};
    attr.name = scan_name(xp);
    if (attr.name.len == 0) {
        return fail(xp, attr.off, "bad name");
    }
    skip_space(xp);
    if (xp->pos >= xp->len) {
        return early_end(xp, lt, "unclosed tag");
    }
    if (xp->bytes[xp->pos] != '=') {
        return fail(xp, xp->pos, "expected '=' after attribute name");
    }
    xp->pos++;
    skip_space(xp);
    if (xp->pos >= xp->len) {
        return early_end(xp, lt, "unclosed tag");
    }
    uint8_t quote = xp->bytes[xp->pos];
    if (quote != '"' && quote != '\'') {
        return fail(xp, xp->pos, "attribute value must be quoted");
    }
    xml_span value = {++xp->pos, 0};
    while (xp->pos < xp->len && xp->bytes[xp->pos] != quote) {
        if (xp->bytes[xp->pos] == '<') {
            return fail(xp, xp->pos, "'<' in attribute value");
        }
        xp->pos++;
    }
    if (xp->pos >= xp->len) {
        return early_end(xp, lt, "unclosed tag");
    }
    value.to = xp->pos++;
    return add_attr(xp, index, attr, value);
}

/* One step inside the start tag at lt after its name: '>', "/>" or an attribute. */
static odin3_status tag_step(xml_parser *xp, uint32_t index, tag_state *state) {
    uint32_t lt = elem_at(xp, index)->off;
    bool spaced = skip_space(xp);
    if (xp->pos >= xp->len) {
        return early_end(xp, lt, "unclosed tag");
    }
    uint8_t byte = xp->bytes[xp->pos];
    if (byte == '>') {
        xp->pos++;
        *state = TAG_OPEN;
        return ODIN3_OK;
    }
    if (byte == '/') {
        xp->pos++;
        if (xp->pos >= xp->len) {
            return early_end(xp, lt, "unclosed tag");
        }
        if (xp->bytes[xp->pos] != '>') {
            return fail(xp, xp->pos, "expected '>'");
        }
        xp->pos++;
        *state = TAG_EMPTY;
        return ODIN3_OK;
    }
    return spaced ? attribute(xp, index) : fail(xp, xp->pos, "missing space before attribute");
}

/* A start tag at pos: the new element becomes the innermost open one unless it is empty. */
static odin3_status start_tag(xml_parser *xp) {
    uint32_t lt = xp->pos++;
    uint32_t name_off = xp->pos;
    odin3_bytes tag = scan_name(xp);
    if (tag.len == 0) {
        return xp->pos >= xp->len ? early_end(xp, lt, "unclosed tag")
                                  : fail(xp, name_off, "bad name");
    }
    if (xp->cur == ODIN3_XML_NONE && xp->doc->elems.len > 0) {
        return fail(xp, lt, "element after the root element");
    }
    uint32_t index = 0;
    odin3_status res = new_elem(xp, tag, lt, &index);
    tag_state state = TAG_MORE;
    while (res == ODIN3_OK && state == TAG_MORE) {
        res = tag_step(xp, index, &state);
    }
    if (res == ODIN3_OK && state == TAG_OPEN) {
        xp->cur = index;
    }
    return res;
}

static odin3_status mismatch(xml_parser *xp, uint32_t off, odin3_bytes tag,
                             const odin3_xml_elem *open) {
    odin3_status res = fail_name(xp, off, "mismatched tag </", &tag, ">, expected </");
    what_name(xp->err, open->tag);
    what_add(xp->err, odin3_bytes_cstr(">"));
    return res;
}

/* An end tag at pos closes the innermost open element. */
static odin3_status end_tag(xml_parser *xp) {
    uint32_t lt = xp->pos;
    xp->pos += END_OPEN;
    uint32_t name_off = xp->pos;
    odin3_bytes tag = scan_name(xp);
    if (tag.len == 0) {
        return xp->pos >= xp->len ? early_end(xp, lt, "unclosed tag")
                                  : fail(xp, name_off, "bad name");
    }
    skip_space(xp);
    if (xp->pos >= xp->len) {
        return early_end(xp, lt, "unclosed tag");
    }
    if (xp->bytes[xp->pos] != '>') {
        return fail(xp, xp->pos, "expected '>'");
    }
    xp->pos++;
    if (xp->cur == ODIN3_XML_NONE) {
        return fail_name(xp, lt, "end tag </", &tag, "> outside the root element");
    }
    const odin3_xml_elem *open = elem_at(xp, xp->cur);
    if (open->tag.len != tag.len || memcmp(open->tag.ptr, tag.ptr, tag.len) != 0) {
        return mismatch(xp, name_off, tag, open);
    }
    xp->cur = open->parent;
    return ODIN3_OK;
}

static odin3_status markup(xml_parser *xp) {
    uint32_t lt = xp->pos;
    if (starts_at(xp, lt, "<!--")) {
        return comment(xp);
    }
    if (starts_at(xp, lt, "<![CDATA[")) {
        return cdata(xp);
    }
    if (starts_at(xp, lt, "<!DOCTYPE")) {
        return fail(xp, lt, "DTD not supported");
    }
    if (starts_at(xp, lt, "<!")) {
        return fail(xp, lt, "bad markup declaration");
    }
    if (starts_at(xp, lt, "<?")) {
        return fail(xp, lt,
                    is_xml_decl(xp, lt) ? "XML declaration not at the start"
                                        : "processing instruction not supported");
    }
    return starts_at(xp, lt, "</") ? end_tag(xp) : start_tag(xp);
}

/* The end of the scan: a forbidden character, an open element or no root at all. */
static odin3_status at_end(xml_parser *xp) {
    if (xp->len < xp->full) {
        return fail(xp, xp->len, "invalid character");
    }
    if (xp->cur != ODIN3_XML_NONE) {
        odin3_bytes tag = elem_at(xp, xp->cur)->tag;
        return fail_name(xp, xp->len, "unclosed element <", &tag, ">");
    }
    return xp->doc->elems.len == 0 ? fail(xp, xp->len, "no root element") : ODIN3_OK;
}

/* --- the API ------------------------------------------------------------------------------- */

void odin3_xml_doc_init(odin3_xml_doc *doc, uint32_t max_nodes) {
    odin3_vec_init(&doc->elems, sizeof(odin3_xml_elem));
    odin3_vec_init(&doc->attrs, sizeof(odin3_xml_attr));
    odin3_vec_init(&doc->texts, sizeof(odin3_xml_text));
    doc->arena = NULL;
    doc->max_nodes = max_nodes;
}

void odin3_xml_doc_free(odin3_xml_doc *doc) {
    odin3_vec_free(&doc->elems);
    odin3_vec_free(&doc->attrs);
    odin3_vec_free(&doc->texts);
    odin3_arena_destroy(doc->arena);
    doc->arena = NULL;
}

odin3_status odin3_xml_parse(odin3_bytes text, odin3_xml_doc *doc, odin3_xml_err *err) {
    memset(err, 0, sizeof *err);
    if (text.len >= UINT32_MAX || (text.ptr == NULL && text.len > 0)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    doc->arena = odin3_arena_create(0);
    if (doc->arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    xml_parser xp = {.bytes = text.ptr,
                     .full = (uint32_t)text.len,
                     .cur = ODIN3_XML_NONE,
                     .doc = doc,
                     .err = err,
                     .line_no = 1};
    xp.len = text.len > 0 ? xml_limit(xp.bytes, xp.full) : 0;
    odin3_status res = prolog(&xp);
    while (res == ODIN3_OK && xp.pos < xp.len) {
        res = xp.bytes[xp.pos] == '<' ? markup(&xp) : char_data(&xp);
    }
    res = res != ODIN3_OK ? res : at_end(&xp);
    odin3_u64map_destroy(xp.names);
    return res;
}

const odin3_xml_elem *odin3_xml_elem_at(const odin3_xml_doc *doc, uint32_t index) {
    return odin3_vec_cat(&doc->elems, index);
}

bool odin3_xml_attr_get(const odin3_xml_doc *doc, const odin3_xml_elem *elem, const char *name,
                        odin3_bytes *value) {
    size_t len = strlen(name);
    for (uint32_t i = 0; i < elem->nattrs; i++) {
        const odin3_xml_attr *attr = odin3_vec_cat(&doc->attrs, elem->attrs + i);
        if (attr->name.len == len && memcmp(attr->name.ptr, name, len) == 0) {
            *value = attr->value;
            return true;
        }
    }
    return false;
}

odin3_status odin3_xml_text_of(const odin3_xml_doc *doc, const odin3_xml_elem *elem,
                               odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    for (uint32_t i = elem->first_text; i != ODIN3_XML_NONE;) {
        const odin3_xml_text *text = odin3_vec_cat(&doc->texts, i);
        if (odin3_strbuf_append(out, text->data) != ODIN3_OK) {
            odin3_strbuf_clear(out);
            return ODIN3_ERR_NO_MEMORY;
        }
        i = text->next;
    }
    return ODIN3_OK;
}
