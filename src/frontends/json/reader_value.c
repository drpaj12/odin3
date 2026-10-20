/*
 * reader_value.c — values of attributes (J-10) and of parameters (M-6), and the attribute walk.
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <string.h>

enum { INT32_DIGITS = 32, INT64_DIGITS = 64, SHOW_MAX = 40, BINARY_BASE = 2 };

/* The text of a value for a message, cut to SHOW_MAX bytes. */
static void show(const odin3_json_node *node, char *buf, size_t size) {
    if (jr_kind(node) == ODIN3_JSON_STRING) {
        (void)snprintf(buf, size, "%.*s", SHOW_MAX, jr_text(node, NULL));
    } else if (jr_kind(node) == ODIN3_JSON_INT) {
        (void)snprintf(buf, size, "%lld", (long long)jr_int(node));
    } else {
        (void)snprintf(buf, size, "<%s>", jr_kind(node) == ODIN3_JSON_ARRAY ? "array" : "value");
    }
}

uint8_t *jr_alloc(jr *rd, size_t len) {
    return odin3_arena_alloc(rd->vals, len > 0 ? len : 1);
}

/* True when every byte of text is one of allowed (an empty text is). */
static bool only_of(const char *text, size_t len, const char *allowed) {
    for (size_t i = 0; i < len; i++) {
        if (strchr(allowed, text[i]) == NULL || text[i] == '\0') {
            return false;
        }
    }
    return true;
}

size_t jr_string_len(const char *text, size_t len) {
    if (len > 0 && text[len - 1] == ' ' && only_of(text, len, "01xz ")) {
        return len - 1;
    }
    return len;
}

/* The odin3_bit of a digit 0 1 x z. */
static uint8_t digit_bit(char digit) {
    switch (digit) {
    case '0':
        return ODIN3_BIT_0;
    case '1':
        return ODIN3_BIT_1;
    case 'x':
        return ODIN3_BIT_X;
    default:
        return ODIN3_BIT_Z;
    }
}

/* A BITS value from digits written most significant first (only 0 1 x z). */
static odin3_status bits_value(jr *rd, const char *text, size_t len, odin3_value *out) {
    uint8_t *bits = len > 0 ? jr_alloc(rd, len) : NULL;
    if (len > 0 && bits == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    for (size_t i = 0; i < len; i++) {
        bits[i] = digit_bit(text[len - 1 - i]);
    }
    *out = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bits, .len = (uint32_t)len};
    return ODIN3_OK;
}

static odin3_status string_value(jr *rd, const char *text, size_t len, odin3_value *out) {
    uint32_t str = 0;
    odin3_status st = jr_fail(rd, odin3_design_intern(rd->design, (odin3_bytes){text, len}, &str));
    if (st == ODIN3_OK) {
        *out = (odin3_value){.kind = ODIN3_VAL_STRING, .str = str};
    }
    return st;
}

odin3_status jr_attr_value(jr *rd, const odin3_json_node *node, const jr_where *where,
                           odin3_value *out) {
    odin3_json_kind kind = jr_kind(node);
    if (kind == ODIN3_JSON_INT) {
        *out = odin3_value_int(jr_int(node));
        return ODIN3_OK;
    }
    if (kind != ODIN3_JSON_STRING) {
        return jr_error(rd, node, where, "an attribute value is a string or an integer");
    }
    size_t len = 0;
    const char *text = jr_text(node, &len);
    if (only_of(text, len, "01xz")) {
        return bits_value(rd, text, len, out);
    }
    return string_value(rd, text, jr_string_len(text, len), out);
}

bool jr_attr_true(const odin3_json_node *node) {
    if (jr_kind(node) == ODIN3_JSON_INT) {
        return jr_int(node) != 0;
    }
    if (jr_kind(node) != ODIN3_JSON_STRING) {
        return false;
    }
    size_t len = 0;
    const char *text = jr_text(node, &len);
    return only_of(text, len, "01xz") && memchr(text, '1', len) != NULL;
}

odin3_status jr_set_attr_named(jr *rd, odin3_module *module, odin3_objref obj, const char *key,
                               const odin3_value *val) {
    uint32_t key_str = 0;
    odin3_status st = jr_intern(rd, key, &key_str);
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_attr_set(module, obj, key_str, val));
    }
    return st;
}

odin3_status jr_set_attr_member(jr *rd, odin3_module *module, odin3_objref obj,
                                const odin3_json_node *member, const jr_where *where) {
    const char *key = odin3_json_key(member);
    char path[JR_NAME_BUF];
    odin3_status st =
        jr_join(rd, &(jr_join_args){member, where, "attributes.", key}, path, sizeof path);
    if (st != ODIN3_OK) {
        return st;
    }
    jr_where here = {where->section, where->key, path};
    if (key[0] == '\0') {
        return jr_error(rd, member, &here, "an attribute has an empty name");
    }
    odin3_value val = {0};
    st = jr_attr_value(rd, member, &here, &val);
    if (st == ODIN3_OK) {
        st = jr_set_attr_named(rd, module, obj, key, &val);
    }
    return st;
}

odin3_status jr_set_attrs(jr *rd, odin3_module *module, odin3_objref obj,
                          const odin3_json_node *attrs, const jr_where *where) {
    if (attrs == NULL) {
        return ODIN3_OK;
    }
    if (jr_kind(attrs) != ODIN3_JSON_OBJECT) {
        jr_where here = {where->section, where->key, "attributes"};
        return jr_error(rd, attrs, &here, "attributes is not an object");
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *member = jr_first(attrs); st == ODIN3_OK && member != NULL;
         member = odin3_json_next(member)) {
        st = jr_set_attr_member(rd, module, obj, member, where);
    }
    return st;
}

/* --- typed parameters (M-6) ---------------------------------------------------------------- */

static odin3_status bad_value(const jr *rd, const odin3_json_node *node, const jr_where *where,
                              const char *what) {
    char text[SHOW_MAX + 2];
    show(node, text, sizeof text);
    return jr_error(rd, node, where, "cannot read '%s' as %s", text, what);
}

/* INT from a string of binary digits: 32 digits are signed, 64 digits are signed, any other
 * length up to 64 is zero-extended. */
static odin3_status int_from_text(const jr *rd, const odin3_json_node *node, const jr_where *where,
                                  odin3_value *out) {
    size_t len = 0;
    const char *text = jr_text(node, &len);
    if (len == 0 || len > INT64_DIGITS || !only_of(text, len, "01")) {
        return bad_value(rd, node, where, "an integer");
    }
    uint64_t num = 0;
    for (size_t i = 0; i < len; i++) {
        num = (num << 1) | (uint64_t)(text[i] - '0');
    }
    int64_t signed_num = (int64_t)num;
    if (len == INT32_DIGITS) {
        signed_num = (int32_t)(uint32_t)num;
    }
    *out = odin3_value_int(signed_num);
    return ODIN3_OK;
}

/* BITS from an integer: its 32 low bits (Yosys reads a JSON number as a 32-bit constant). */
static odin3_status bits_from_int(jr *rd, int64_t num, odin3_value *out) {
    uint8_t *bits = jr_alloc(rd, INT32_DIGITS);
    if (bits == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t i = 0; i < INT32_DIGITS; i++) {
        bits[i] = ((((uint64_t)num) >> i) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    *out = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bits, .len = INT32_DIGITS};
    return ODIN3_OK;
}

/* One row of a cover: `<inputs> <output>` or `<output>`; *inputs is the input count it has. */
typedef struct cover_row {
    const char *text;
    size_t len;
} cover_row;

static bool row_shape(const cover_row *row, size_t *inputs) {
    const char *space = memchr(row->text, ' ', row->len);
    size_t in = space != NULL ? (size_t)(space - row->text) : 0;
    size_t out_at = space != NULL ? in + 1 : 0;
    *inputs = in;
    return row->len == out_at + 1 && only_of(row->text, in, "01-") &&
           (row->text[out_at] == '0' || row->text[out_at] == '1');
}

/* The next row of text (up to a newline) after *at. */
static cover_row next_row(const char *text, size_t len, size_t *at) {
    size_t start = *at;
    while (*at < len && text[*at] != '\n') {
        (*at)++;
    }
    cover_row row = {text + start, *at - start};
    if (*at < len) {
        (*at)++; /* the newline */
    }
    return row;
}

/* Validates every row and counts them; *inputs is the input count of the first. */
static bool cover_rows(const char *text, size_t len, size_t *inputs, size_t *rows) {
    size_t at = 0;
    *rows = 0;
    while (at < len) {
        cover_row row = next_row(text, len, &at);
        size_t have = 0;
        if (!row_shape(&row, &have) || (*rows > 0 && have != *inputs)) {
            return false;
        }
        *inputs = have;
        (*rows)++;
    }
    return *rows > 0;
}

static odin3_status cover_value(jr *rd, const odin3_json_node *node, const jr_where *where,
                                odin3_value *out) {
    size_t len = 0;
    const char *text = jr_text(node, &len);
    size_t inputs = 0;
    size_t rows = 0;
    if (len == 0) { /* an empty cover: no rows (constant 0) */
        *out = (odin3_value){.kind = ODIN3_VAL_COVER, .cover_inputs = 0};
        return ODIN3_OK;
    }
    if (!cover_rows(text, len, &inputs, &rows)) {
        return bad_value(rd, node, where, "a cover (rows of `<inputs> <output>`)");
    }
    uint8_t *bytes = jr_alloc(rd, rows * (inputs + 1));
    if (bytes == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    size_t at = 0;
    for (size_t row = 0; row < rows; row++) {
        cover_row cells = next_row(text, len, &at);
        for (size_t i = 0; i < inputs; i++) {
            bytes[row * (inputs + 1) + i] = (uint8_t)cells.text[i];
        }
        bytes[row * (inputs + 1) + inputs] = (uint8_t)cells.text[cells.len - 1];
    }
    *out = (odin3_value){.kind = ODIN3_VAL_COVER,
                         .bits = bytes,
                         .len = (uint32_t)(rows * (inputs + 1)),
                         .cover_inputs = (uint32_t)inputs};
    return ODIN3_OK;
}

static odin3_status bits_param(jr *rd, const odin3_json_node *node, const jr_where *where,
                               odin3_value *out) {
    if (jr_kind(node) == ODIN3_JSON_INT) {
        return bits_from_int(rd, jr_int(node), out);
    }
    size_t len = 0;
    const char *text = jr_text(node, &len);
    if (!only_of(text, len, "01xz")) {
        return bad_value(rd, node, where, "a bit vector");
    }
    return bits_value(rd, text, len, out);
}

static odin3_status text_param(jr *rd, const odin3_json_node *node, odin3_value *out) {
    size_t len = 0;
    const char *text = jr_text(node, &len);
    return string_value(rd, text, jr_string_len(text, len), out);
}

odin3_status jr_param_value(jr *rd, odin3_value_kind kind, const odin3_json_node *node,
                            const jr_where *where, odin3_value *out) {
    odin3_json_kind have = jr_kind(node);
    bool is_text = have == ODIN3_JSON_STRING;
    bool is_int = have == ODIN3_JSON_INT;
    if (!is_text && !is_int) {
        return jr_error(rd, node, where, "a parameter value is a string or an integer");
    }
    switch (kind) {
    case ODIN3_VAL_INT:
        if (is_int) {
            *out = odin3_value_int(jr_int(node));
            return ODIN3_OK;
        }
        return int_from_text(rd, node, where, out);
    case ODIN3_VAL_BITS:
        return bits_param(rd, node, where, out);
    case ODIN3_VAL_STRING:
        if (!is_text) {
            return bad_value(rd, node, where, "a string");
        }
        return text_param(rd, node, out);
    case ODIN3_VAL_COVER:
        return is_text ? cover_value(rd, node, where, out) : bad_value(rd, node, where, "a cover");
    default:
        return jr_error(rd, node, where, "a parameter of this kind has no JSON form");
    }
}
