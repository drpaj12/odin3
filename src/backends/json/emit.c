/* emit.c — low-level text, bit, value and provenance output for the Yosys JSON writer. */
#include "backends/json/jw.h"
#include "ir/prov.h"

#include <stdarg.h>
#include <string.h>

enum {
    INDENT = 2,      /* spaces per nesting level */
    INT32_BITS = 32, /* Yosys integer parameters are 32-bit binary strings */
    INT64_BITS = 64, /* ... or 64 when a value does not fit */
    HEX_DIGITS = 16, /* \u00XX escapes use the low byte */
    LOW_NIBBLE = 0xF,
    CTRL_LIMIT = 0x20, /* bytes below this need an escape */
    NIBBLE_SHIFT = 4
};

void jw_raw(jw *out, const char *text) {
    if (out->status == ODIN3_OK && fputs(text, out->fp) == EOF) {
        out->status = ODIN3_ERR_IO;
    }
}

void jw_fmt(jw *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (out->status == ODIN3_OK && vfprintf(out->fp, fmt, args) < 0) {
        out->status = ODIN3_ERR_IO;
    }
    va_end(args);
}

void jw_char(jw *out, int chr) {
    if (out->status == ODIN3_OK && fputc(chr, out->fp) == EOF) {
        out->status = ODIN3_ERR_IO;
    }
}

/* One byte of a JSON string, escaped as needed. */
static void put_escaped(jw *out, unsigned char chr) {
    static const char k_hex[] = "0123456789abcdef";
    if (chr == '"' || chr == '\\') {
        jw_char(out, '\\');
        jw_char(out, chr);
    } else if (chr == '\n') {
        jw_raw(out, "\\n");
    } else if (chr == '\t') {
        jw_raw(out, "\\t");
    } else if (chr < CTRL_LIMIT) {
        jw_raw(out, "\\u00");
        jw_char(out, k_hex[chr >> NIBBLE_SHIFT]);
        jw_char(out, k_hex[chr & LOW_NIBBLE]);
    } else {
        jw_char(out, chr);
    }
}

/* --- UTF-8: invalid sequences are written as U+FFFD so the output is always valid JSON ------ */

enum {
    UTF8_ASCII_LIMIT = 0x80,
    UTF8_CONT_MASK = 0xC0,
    UTF8_CONT_TAG = 0x80,
    UTF8_LEAD2_MIN = 0xC2,
    UTF8_LEAD2_MAX = 0xDF,
    UTF8_LEAD3_MIN = 0xE0,
    UTF8_LEAD3_MAX = 0xEF,
    UTF8_LEAD_ED = 0xED,
    UTF8_LEAD4_MIN = 0xF0,
    UTF8_LEAD4_MAX = 0xF4,
    UTF8_E0_SECOND_MIN = 0xA0,
    UTF8_ED_SECOND_MAX = 0x9F,
    UTF8_F0_SECOND_MIN = 0x90,
    UTF8_F4_SECOND_MAX = 0x8F,
    UTF8_LEN2 = 2,
    UTF8_LEN3 = 3,
    UTF8_LEN4 = 4
};

/* Sequence length announced by a lead byte, 0 for a byte that cannot start one. */
static size_t utf8_lead_len(unsigned char lead) {
    if (lead >= UTF8_LEAD2_MIN && lead <= UTF8_LEAD2_MAX) {
        return UTF8_LEN2;
    }
    if (lead >= UTF8_LEAD3_MIN && lead <= UTF8_LEAD3_MAX) {
        return UTF8_LEN3;
    }
    return lead >= UTF8_LEAD4_MIN && lead <= UTF8_LEAD4_MAX ? UTF8_LEN4 : 0;
}

/* The second byte must exclude overlong forms, surrogates and values above U+10FFFF. */
static bool utf8_second_ok(unsigned char lead, unsigned char second) {
    if (lead == UTF8_LEAD3_MIN) {
        return second >= UTF8_E0_SECOND_MIN;
    }
    if (lead == UTF8_LEAD_ED) {
        return second <= UTF8_ED_SECOND_MAX;
    }
    if (lead == UTF8_LEAD4_MIN) {
        return second >= UTF8_F0_SECOND_MIN;
    }
    return lead != UTF8_LEAD4_MAX || second <= UTF8_F4_SECOND_MAX;
}

/* Length of the valid sequence at src (left bytes remain), 0 when it is invalid. */
static size_t utf8_valid_len(const unsigned char *src, size_t left) {
    if (src[0] < UTF8_ASCII_LIMIT) {
        return 1;
    }
    size_t len = utf8_lead_len(src[0]);
    if (len == 0 || len > left || !utf8_second_ok(src[0], src[1])) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((src[i] & UTF8_CONT_MASK) != UTF8_CONT_TAG) {
            return 0;
        }
    }
    return len;
}

static void put_bytes(jw *out, odin3_bytes text) {
    const unsigned char *src = text.ptr;
    size_t i = 0;
    while (i < text.len) {
        size_t len = utf8_valid_len(src + i, text.len - i);
        if (len == 0) {
            jw_raw(out, "\\ufffd");
            i++;
            continue;
        }
        if (len == 1) {
            put_escaped(out, src[i]);
        } else {
            for (size_t j = 0; j < len; j++) {
                jw_char(out, src[i + j]);
            }
        }
        i += len;
    }
}

void jw_string(jw *out, odin3_bytes text) {
    jw_char(out, '"');
    put_bytes(out, text);
    jw_char(out, '"');
}

void jw_string2(jw *out, const jw_name *name) {
    jw_char(out, '"');
    put_bytes(out, name->head);
    put_bytes(out, name->tail);
    jw_char(out, '"');
}

void jw_str_id(jw *out, uint32_t str) {
    const char *text = odin3_strtab_get(out->strtab, str);
    jw_string(out, odin3_bytes_cstr(text != NULL ? text : ""));
}

void jw_indent(jw *out, uint32_t depth) {
    for (uint32_t i = 0; i < depth * INDENT; i++) {
        jw_char(out, ' ');
    }
}

void jw_open(jw *out, jw_list *list) {
    list->any = false;
    jw_raw(out, "{");
}

void jw_item(jw *out, jw_list *list) {
    jw_raw(out, list->any ? ",\n" : "\n");
    list->any = true;
    jw_indent(out, list->depth);
}

void jw_close(jw *out, const jw_list *list) {
    jw_raw(out, "\n");
    jw_indent(out, list->depth - 1);
    jw_raw(out, "}");
}

void jw_key(jw *out, jw_list *list, const char *key) {
    jw_item(out, list);
    jw_string(out, odin3_bytes_cstr(key));
    jw_raw(out, ": ");
}

/* --- bits ---------------------------------------------------------------------------------- */

uint32_t jw_net_code(const jw *out, odin3_net_id net) {
    if (!odin3_net_valid(net)) {
        return JW_BIT_X;
    }
    switch (odin3_net_const_value(out->module, net)) {
    case ODIN3_CONST_0:
        return JW_BIT_0;
    case ODIN3_CONST_1:
        return JW_BIT_1;
    case ODIN3_CONST_X:
        return JW_BIT_X;
    case ODIN3_CONST_Z:
        return JW_BIT_Z;
    case ODIN3_CONST_NONE:
    default:
        return net.v + 1;
    }
}

void jw_bit(jw *out, uint32_t code) {
    switch (code) {
    case JW_BIT_0:
        jw_raw(out, "\"0\"");
        break;
    case JW_BIT_1:
        jw_raw(out, "\"1\"");
        break;
    case JW_BIT_X:
        jw_raw(out, "\"x\"");
        break;
    case JW_BIT_Z:
        jw_raw(out, "\"z\"");
        break;
    default:
        jw_fmt(out, "%u", (unsigned)code);
        break;
    }
}

void jw_pins(jw *out, odin3_pinslice pins) {
    jw_raw(out, "[ ");
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (i > 0) {
            jw_raw(out, ", ");
        }
        jw_bit(out, jw_net_code(out, odin3_pin_net(out->module, pin)));
    }
    jw_raw(out, " ]");
}

/* --- values -------------------------------------------------------------------------------- */

typedef struct binary {
    uint64_t value;
    uint32_t bits;
} binary;

static void put_binary(jw *out, binary num) {
    jw_char(out, '"');
    for (uint32_t i = num.bits; i > 0; i--) {
        jw_char(out, ((num.value >> (i - 1)) & 1U) != 0 ? '1' : '0');
    }
    jw_char(out, '"');
}

static void put_int(jw *out, int64_t num) {
    if (num >= INT32_MIN && num <= INT32_MAX) {
        put_binary(out, (binary){(uint32_t)(int32_t)num, INT32_BITS});
    } else {
        put_binary(out, (binary){(uint64_t)num, INT64_BITS});
    }
}

void jw_param_int(jw *out, jw_list *list, const char *key, int64_t num) {
    jw_key(out, list, key);
    put_int(out, num);
}

static void put_bits_value(jw *out, const odin3_value *val) {
    static const char k_bit[] = {'0', '1', 'x', 'z'};
    jw_char(out, '"');
    for (uint32_t i = val->len; i > 0; i--) {
        uint8_t bit = val->bits[i - 1];
        jw_char(out, bit < sizeof k_bit ? k_bit[bit] : 'x');
    }
    jw_char(out, '"');
}

/* Yosys appends a space to a string made only of 0/1/x/z so a reader does not take it as bits. */
static void put_string_value(jw *out, uint32_t str) {
    const char *text = odin3_strtab_get(out->strtab, str);
    odin3_bytes bytes = odin3_bytes_cstr(text != NULL ? text : "");
    bool bitlike = strspn(bytes.ptr, "01xz ") == bytes.len;
    jw_name name = {bytes, odin3_bytes_cstr(bitlike ? " " : "")};
    jw_string2(out, &name);
}

/* A cover as text: one "inputs output" row per line. */
static void put_cover_value(jw *out, const odin3_value *val) {
    const uint32_t row = val->cover_inputs + 1;
    jw_char(out, '"');
    for (uint32_t off = 0; off + row <= val->len; off += row) {
        if (off > 0) {
            jw_raw(out, "\\n");
        }
        for (uint32_t i = 0; i < row; i++) {
            if (i == val->cover_inputs && i > 0) {
                jw_char(out, ' ');
            }
            jw_char(out, val->bits[off + i]);
        }
    }
    jw_char(out, '"');
}

void jw_param(jw *out, jw_list *list, const char *key, const odin3_value *val) {
    jw_key(out, list, key);
    switch (val->kind) {
    case ODIN3_VAL_INT:
        put_int(out, val->i);
        break;
    case ODIN3_VAL_BITS:
        put_bits_value(out, val);
        break;
    case ODIN3_VAL_STRING:
        put_string_value(out, val->str);
        break;
    case ODIN3_VAL_COVER:
    default:
        put_cover_value(out, val);
        break;
    }
}

/* --- provenance ---------------------------------------------------------------------------- */

static void src_visit(void *user, odin3_prov_id leaf) {
    jw *out = user;
    const odin3_prov_record *rec = odin3_prov_get(out->design, leaf);
    if (!out->have_src && rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        out->src = rec->locs[0];
        out->have_src = true;
    }
}

bool jw_claim(jw *out, odin3_wattr_role role, const char *key) {
    bool fresh = false;
    odin3_wattr_claim_req req = {role, odin3_bytes_cstr(key)};
    if (out->status == ODIN3_OK) {
        odin3_status status = odin3_wattr_claim(&out->seen, req, &fresh);
        out->status = status != ODIN3_OK ? status : out->status;
    }
    return fresh && out->status == ODIN3_OK;
}

void jw_src(jw *out, jw_list *list, odin3_prov_id prov) {
    if (odin3_prov_get(out->design, prov) == NULL || out->status != ODIN3_OK) {
        return;
    }
    out->have_src = false;
    odin3_status status = odin3_prov_sources(out->design, prov, src_visit, out);
    if (status != ODIN3_OK) {
        out->status = status;
    } else if (out->have_src && jw_claim(out, ODIN3_WATTR_ATTRIBUTE, "src")) {
        jw_key(out, list, "src");
        char pos[INT32_BITS];
        (void)snprintf(pos, sizeof pos, ":%u.%u", (unsigned)out->src.line, (unsigned)out->src.col);
        const char *file = odin3_strtab_get(out->strtab, out->src.file);
        jw_name name = {odin3_bytes_cstr(file != NULL ? file : ""), odin3_bytes_cstr(pos)};
        jw_string2(out, &name);
    }
}

/* --- user attributes (odin3_attr_foreach) ------------------------------------------------- */

/* A string; one made only of 0/1/x/z (or blanks) gets a trailing space (see put_string_value). */
static void put_text(jw *out, odin3_bytes text) {
    const char *bytes = text.ptr;
    size_t plain = 0;
    while (plain < text.len && strchr("01xz ", bytes[plain]) != NULL && bytes[plain] != '\0') {
        plain++;
    }
    jw_name name = {text, odin3_bytes_cstr(plain == text.len ? " " : "")};
    jw_string2(out, &name);
}

/* Digits MSB first, any byte but '0' read as 1 (Yosys read_blif). */
static void put_binary_text(jw *out, odin3_bytes digits) {
    const char *bytes = digits.ptr;
    jw_char(out, '"');
    for (size_t i = 0; i < digits.len; i++) {
        jw_char(out, bytes[i] == '0' ? '0' : '1');
    }
    jw_char(out, '"');
}

typedef struct user_attrs {
    jw *out;
    jw_list *list;
    odin3_wattr_role role;
} user_attrs;

static odin3_status user_attr_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    const user_attrs *ua = ctx;
    jw *out = ua->out;
    odin3_wattr attr = odin3_wattr_classify(out->strtab, key_str, value);
    /* attr.key ends where its strtab string ends, so it is NUL-terminated. */
    const char *key = attr.key.ptr;
    if (attr.role != ua->role || !jw_claim(out, attr.role, key)) {
        return out->status;
    }
    if (attr.form == ODIN3_WATTR_VALUE) {
        jw_param(out, ua->list, key, value);
    } else {
        jw_key(out, ua->list, key);
        if (attr.form == ODIN3_WATTR_TEXT) {
            put_text(out, attr.text);
        } else {
            put_binary_text(out, attr.text);
        }
    }
    return out->status;
}

void jw_user_attrs(jw *out, jw_list *list, odin3_objref obj, odin3_wattr_role role) {
    if (out->status != ODIN3_OK) {
        return;
    }
    user_attrs ctx = {out, list, role};
    odin3_status status = odin3_attr_foreach(out->module, obj, user_attr_visit, &ctx);
    out->status = out->status != ODIN3_OK ? out->status : status;
}

/* --- unique keys --------------------------------------------------------------------------- */

static bool key_taken(const jw *out, jw_keykind kind, const char *text) {
    uint32_t str = 0;
    if (!odin3_strtab_find(out->strtab, odin3_bytes_cstr(text), &str)) {
        return false;
    }
    if (kind == JW_KEY_CELL) {
        return odin3_node_valid(odin3_module_find_node(out->module, str));
    }
    return odin3_wire_valid(odin3_module_find_wire(out->module, str)) ||
           odin3_net_valid(odin3_module_find_net(out->module, str));
}

static odin3_status build_key(jw *out, const jw_keyspec *spec, uint32_t bump) {
    odin3_strbuf_clear(&out->key);
    odin3_status status = odin3_strbuf_append(&out->key, odin3_bytes_cstr(spec->head));
    if (status == ODIN3_OK) {
        status = odin3_strbuf_append(&out->key, odin3_bytes_cstr(spec->tail));
    }
    if (status == ODIN3_OK && bump > 0) {
        status = odin3_strbuf_appendf(&out->key, "$u%u", (unsigned)bump);
    }
    return status;
}

const char *jw_make_key(jw *out, const jw_keyspec *spec) {
    uint32_t bump = 0;
    odin3_status status = build_key(out, spec, bump);
    while (status == ODIN3_OK && spec->generated && out->key.data != NULL &&
           key_taken(out, spec->kind, out->key.data)) {
        status = build_key(out, spec, ++bump);
    }
    if (status != ODIN3_OK) {
        out->status = status;
    }
    return status == ODIN3_OK && out->key.data != NULL ? out->key.data : "";
}
