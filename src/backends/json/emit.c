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

static void put_bytes(jw *out, odin3_bytes text) {
    const unsigned char *src = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        put_escaped(out, src[i]);
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

void jw_src(jw *out, jw_list *list, odin3_prov_id prov) {
    if (odin3_prov_get(out->design, prov) == NULL || out->status != ODIN3_OK) {
        return;
    }
    out->have_src = false;
    odin3_status status = odin3_prov_sources(out->design, prov, src_visit, out);
    if (status != ODIN3_OK) {
        out->status = status;
    } else if (out->have_src) {
        jw_key(out, list, "src");
        char pos[INT32_BITS];
        (void)snprintf(pos, sizeof pos, ":%u.%u", (unsigned)out->src.line, (unsigned)out->src.col);
        const char *file = odin3_strtab_get(out->strtab, out->src.file);
        jw_name name = {odin3_bytes_cstr(file != NULL ? file : ""), odin3_bytes_cstr(pos)};
        jw_string2(out, &name);
    }
}
