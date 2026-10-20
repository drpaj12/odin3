/* param_text.c — see param_text.h. */
#include "frontends/blif/param_text.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum {
    INT32_BITS = 32,
    INT64_BITS = 64,
    OCTAL_DIGITS = 3,
    OCTAL_BASE = 8,
    PRINTABLE_LOW = 32,
    PRINTABLE_HIGH = 127
};

/* --- formatting ---------------------------------------------------------------------------- */

static odin3_status format_string(const odin3_design *design, uint32_t str, odin3_strbuf *out) {
    const char *text = odin3_strtab_get(odin3_design_strtab(design), str);
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const char *ch = text; st == ODIN3_OK && ch != NULL && *ch != '\0'; ch++) {
        unsigned char byte = (unsigned char)*ch;
        if (byte == '"' || byte == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", (char)byte);
        } else if (byte < PRINTABLE_LOW || byte >= PRINTABLE_HIGH) {
            st = odin3_strbuf_appendf(out, "\\%03o", (unsigned)byte);
        } else {
            st = odin3_strbuf_appendf(out, "%c", (char)byte);
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

static odin3_status format_int(int64_t num, odin3_strbuf *out) {
    unsigned width = num >= INT32_MIN && num <= INT32_MAX ? INT32_BITS : INT64_BITS;
    uint64_t bits = (uint64_t)num;
    odin3_status st = ODIN3_OK;
    for (unsigned i = width; st == ODIN3_OK && i > 0; i--) {
        st = odin3_strbuf_appendf(out, "%c", ((bits >> (i - 1U)) & 1U) != 0 ? '1' : '0');
    }
    return st;
}

static odin3_status format_bits(const odin3_value *value, odin3_strbuf *out) {
    static const char k_chars[] = {'0', '1', 'x', 'z'};
    odin3_status st = ODIN3_OK;
    for (uint32_t i = value->len; st == ODIN3_OK && i > 0; i--) {
        st = odin3_strbuf_appendf(out, "%c", k_chars[value->bits[i - 1U] & 3U]);
    }
    return st;
}

odin3_status odin3_blif_param_format(const odin3_design *design, const odin3_value *value,
                                     odin3_strbuf *out) {
    if (value->kind == ODIN3_VAL_STRING) {
        return format_string(design, value->str, out);
    }
    if (value->kind == ODIN3_VAL_INT) {
        return format_int(value->i, out);
    }
    if (value->kind == ODIN3_VAL_BITS && value->len > 0) {
        return format_bits(value, out);
    }
    return ODIN3_ERR_INVALID_ARG;
}

/* --- parsing ------------------------------------------------------------------------------- */

static odin3_status parse_int(odin3_bytes text, odin3_value *out) {
    if (text.len == 0 || text.len > INT64_BITS) {
        return ODIN3_ERR_PARSE;
    }
    const char *ptr = text.ptr;
    uint64_t bits = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (ptr[i] != '0' && ptr[i] != '1') {
            return ODIN3_ERR_PARSE;
        }
        bits = (bits << 1U) | (uint64_t)(ptr[i] == '1');
    }
    int64_t num = (int64_t)bits;
    if (text.len == INT32_BITS && (bits >> (INT32_BITS - 1U)) != 0) {
        num = (int64_t)(int32_t)(uint32_t)bits;
    }
    *out = odin3_value_int(num);
    return ODIN3_OK;
}

/* The bit code of one of `0 1 x z`, or -1. */
static int bit_code(char ch) {
    switch (ch) {
    case '0':
        return ODIN3_BIT_0;
    case '1':
        return ODIN3_BIT_1;
    case 'x':
        return ODIN3_BIT_X;
    case 'z':
        return ODIN3_BIT_Z;
    default:
        return -1;
    }
}

static odin3_status parse_bits(odin3_bytes text, odin3_strbuf *bits, odin3_value *out) {
    if (text.len == 0) {
        return ODIN3_ERR_PARSE;
    }
    odin3_strbuf_clear(bits);
    const char *ptr = text.ptr;
    odin3_status st = ODIN3_OK;
    for (size_t i = text.len; st == ODIN3_OK && i > 0; i--) {
        int code = bit_code(ptr[i - 1U]);
        char bit = (char)code;
        st = code < 0 ? ODIN3_ERR_PARSE : odin3_strbuf_append(bits, (odin3_bytes){&bit, 1});
    }
    if (st == ODIN3_OK) {
        *out =
            (odin3_value){ODIN3_VAL_BITS, 0, (const uint8_t *)bits->data, (uint32_t)text.len, 0, 0};
    }
    return st;
}

/* The escape after the backslash at ptr[at - 1], the text ending at ptr[last] (the closing quote
 * is ptr[last + 1]): its byte, with *used the characters it takes; -1 when malformed. */
static int escape_at(const char *ptr, size_t at, size_t last, size_t *used) {
    if (at > last) {
        return -1;
    }
    if (ptr[at] == '"' || ptr[at] == '\\') {
        *used = 1;
        return ptr[at];
    }
    if (at + OCTAL_DIGITS - 1U > last) {
        return -1;
    }
    int byte = 0;
    for (size_t i = 0; i < OCTAL_DIGITS; i++) {
        if (ptr[at + i] < '0' || ptr[at + i] > '7') {
            return -1;
        }
        byte = byte * OCTAL_BASE + (ptr[at + i] - '0');
    }
    *used = OCTAL_DIGITS;
    return byte == 0 ? -1 : byte; /* a NUL cannot be in an interned string */
}

/* Unescapes the body of the quoted string ptr[0..end) into out (cleared first). */
static bool unescape(const char *ptr, size_t end, odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    for (size_t i = 1; i + 1 < end; i++) {
        char ch = ptr[i];
        if (ch == '"') {
            return false;
        }
        if (ch == '\\') {
            size_t used = 0;
            int byte = escape_at(ptr, i + 1, end - 2U, &used);
            if (byte < 0) {
                return false;
            }
            ch = (char)byte;
            i += used;
        }
        if (odin3_strbuf_append(out, (odin3_bytes){&ch, 1}) != ODIN3_OK) {
            return false;
        }
    }
    return true;
}

static odin3_status parse_string(odin3_design *design, odin3_bytes text, odin3_strbuf *body,
                                 odin3_value *out) {
    const char *ptr = text.ptr;
    if (text.len < 2 || ptr[0] != '"' || ptr[text.len - 1U] != '"' ||
        !unescape(ptr, text.len, body)) {
        return ODIN3_ERR_PARSE;
    }
    uint32_t id = 0;
    odin3_status st = odin3_design_intern(design, (odin3_bytes){body->data, body->len}, &id);
    if (st == ODIN3_OK) {
        *out = (odin3_value){ODIN3_VAL_STRING, 0, NULL, 0, id, 0};
    }
    return st;
}

odin3_status odin3_blif_param_parse(odin3_design *design, odin3_value_kind kind, odin3_bytes text,
                                    odin3_strbuf *bits, odin3_value *out) {
    if (kind == ODIN3_VAL_INT) {
        return parse_int(text, out);
    }
    if (kind == ODIN3_VAL_BITS) {
        return parse_bits(text, bits, out);
    }
    if (kind == ODIN3_VAL_STRING) {
        return parse_string(design, text, bits, out);
    }
    return ODIN3_ERR_PARSE;
}
