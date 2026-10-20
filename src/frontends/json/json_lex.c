/* json_lex.c — cursor, error, string and integer scanning for the JSON parser. */
#include "frontends/json/json_lex.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    BOM_LEN = 3,
    HEX_DIGITS = 4,
    HEX_BASE = 16,
    HEX_LETTER = 10,
    DEC_BASE = 10,
    SURR_HI_FIRST = 0xD800,
    SURR_LO_FIRST = 0xDC00,
    SURR_LO_LAST = 0xDFFF,
    SURR_BITS = 10,
    SURR_PLANE = 0x10000,
    UTF8_2 = 0x80,
    UTF8_3 = 0x800,
    UTF8_4 = 0x10000,
    CONT_BITS = 6,
    CONT_MASK = 0x3F,
    CONT_MARK = 0x80,
    LEAD_2 = 0xC0,
    LEAD_3 = 0xE0,
    LEAD_4 = 0xF0,
    FIRST_PRINTABLE = 0x20,
    UNIT_NONCHAR = 0xFFFF,
    BYTE_BITS = 8,
    BYTE_MAX = 0xFF
};

static const uint8_t k_bom[BOM_LEN] = {0xEF, 0xBB, 0xBF};

void json_lex_init(json_lex *lex, const uint8_t *text, size_t len) {
    lex->text = text;
    lex->len = len;
    lex->pos = 0;
    lex->line_start = 0;
    lex->line = 1;
    if (len >= BOM_LEN && memcmp(text, k_bom, BOM_LEN) == 0) {
        lex->pos = BOM_LEN;
        lex->line_start = BOM_LEN;
    }
}

int json_peek(const json_lex *lex) {
    return lex->pos < lex->len ? lex->text[lex->pos] : -1;
}

void json_advance(json_lex *lex) {
    lex->pos++;
}

odin3_json_loc json_here(const json_lex *lex) {
    odin3_json_loc pos = {lex->line, (uint32_t)(lex->pos - lex->line_start + 1)};
    return pos;
}

void json_skip_ws(json_lex *lex) {
    while (lex->pos < lex->len) {
        uint8_t ch = lex->text[lex->pos];
        if (ch == '\n') {
            lex->line++;
            lex->line_start = lex->pos + 1;
        } else if (ch != ' ' && ch != '\t' && ch != '\r') {
            return;
        }
        lex->pos++;
    }
}

odin3_status json_fail(json_lex *lex, odin3_json_loc pos, odin3_status code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(lex->err->message, sizeof lex->err->message, fmt, ap);
    va_end(ap);
    lex->err->line = pos.line;
    lex->err->col = pos.col;
    return code;
}

/* The decoded bytes go to lex->out; it holds as many bytes as the raw string, which is enough. */
static void push_byte(json_lex *lex, uint8_t byte) {
    lex->out[lex->out_len++] = byte;
}

static void push_utf8(json_lex *lex, uint32_t cp) {
    if (cp < UTF8_2) {
        push_byte(lex, (uint8_t)cp);
        return;
    }
    if (cp < UTF8_3) {
        push_byte(lex, (uint8_t)(LEAD_2 | (cp >> CONT_BITS)));
    } else if (cp < UTF8_4) {
        push_byte(lex, (uint8_t)(LEAD_3 | (cp >> (2 * CONT_BITS))));
        push_byte(lex, (uint8_t)(CONT_MARK | ((cp >> CONT_BITS) & CONT_MASK)));
    } else {
        push_byte(lex, (uint8_t)(LEAD_4 | (cp >> (3 * CONT_BITS))));
        push_byte(lex, (uint8_t)(CONT_MARK | ((cp >> (2 * CONT_BITS)) & CONT_MASK)));
        push_byte(lex, (uint8_t)(CONT_MARK | ((cp >> CONT_BITS) & CONT_MASK)));
    }
    push_byte(lex, (uint8_t)(CONT_MARK | (cp & CONT_MASK)));
}

static int hex_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + HEX_LETTER;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + HEX_LETTER;
    }
    return -1;
}

/* Four hex digits at the cursor into *unit. */
static odin3_status read_hex4(json_lex *lex, uint32_t *unit) {
    uint32_t val = 0;
    for (int i = 0; i < HEX_DIGITS; i++) {
        int digit = hex_value(json_peek(lex));
        if (digit < 0) {
            return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE,
                             "bad \\u escape: expected 4 hex digits");
        }
        val = val * HEX_BASE + (uint32_t)digit;
        json_advance(lex);
    }
    *unit = val;
    return ODIN3_OK;
}

/*
 * Yosys write_json writes a byte >= 0x80 as "￿" + "FFxx" (a sign-extended char printed with
 * %04X). Its own reader refuses it; this reader restores the byte xx. After "￿".
 */
static odin3_status read_yosys_byte(json_lex *lex, odin3_json_loc esc) {
    uint32_t tail = 0;
    bool ok = read_hex4(lex, &tail) == ODIN3_OK;
    if (!ok || (tail >> BYTE_BITS) != BYTE_MAX || (tail & BYTE_MAX) < UTF8_2) {
        return json_fail(lex, esc, ODIN3_ERR_PARSE,
                         "\\uFFFF is not a character (Yosys writes a byte >= 0x80 as \\uFFFFFFxx)");
    }
    push_byte(lex, (uint8_t)(tail & BYTE_MAX));
    return ODIN3_OK;
}

/* After "\u": one code point, combining a surrogate pair. */
static odin3_status read_unicode(json_lex *lex, odin3_json_loc esc) {
    uint32_t unit = 0;
    odin3_status st = read_hex4(lex, &unit);
    if (st != ODIN3_OK) {
        return st;
    }
    if (unit == 0) {
        return json_fail(lex, esc, ODIN3_ERR_PARSE, "\\u0000 is not allowed in a string");
    }
    if (unit == UNIT_NONCHAR) {
        return read_yosys_byte(lex, esc);
    }
    if (unit >= SURR_LO_FIRST && unit <= SURR_LO_LAST) {
        return json_fail(lex, esc, ODIN3_ERR_PARSE, "lone low surrogate in \\u escape");
    }
    if (unit >= SURR_HI_FIRST && unit < SURR_LO_FIRST) {
        uint32_t low = 0;
        if (json_peek(lex) != '\\' || lex->pos + 1 >= lex->len || lex->text[lex->pos + 1] != 'u') {
            return json_fail(lex, esc, ODIN3_ERR_PARSE, "lone high surrogate in \\u escape");
        }
        lex->pos += 2;
        st = read_hex4(lex, &low);
        if (st != ODIN3_OK) {
            return st;
        }
        if (low < SURR_LO_FIRST || low > SURR_LO_LAST) {
            return json_fail(lex, esc, ODIN3_ERR_PARSE, "lone high surrogate in \\u escape");
        }
        unit = SURR_PLANE + (((unit - SURR_HI_FIRST) << SURR_BITS) | (low - SURR_LO_FIRST));
    }
    push_utf8(lex, unit);
    return ODIN3_OK;
}

static int simple_escape(int ch) {
    switch (ch) {
    case '"':
    case '\\':
    case '/':
        return ch;
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    default:
        return -1;
    }
}

/* At the backslash. */
static odin3_status read_escape(json_lex *lex) {
    odin3_json_loc esc = json_here(lex);
    json_advance(lex);
    int ch = json_peek(lex);
    if (ch == 'u') {
        json_advance(lex);
        return read_unicode(lex, esc);
    }
    int plain = simple_escape(ch);
    if (plain < 0) {
        return json_fail(lex, esc, ODIN3_ERR_PARSE, "bad escape in string");
    }
    json_advance(lex);
    push_byte(lex, (uint8_t)plain);
    return ODIN3_OK;
}

/*
 * Cursor on the opening quote. Finds the closing quote without decoding: *raw is the number of
 * bytes between the quotes and *escaped tells whether any backslash occurs. A control byte or the
 * end of the text is an error (an unterminated string is located at its opening quote).
 */
static odin3_status scan_string(json_lex *lex, size_t *raw, bool *escaped) {
    size_t at = lex->pos + 1;
    *escaped = false;
    while (at < lex->len && lex->text[at] != '"') {
        uint8_t byte = lex->text[at];
        if (byte < FIRST_PRINTABLE) {
            lex->pos = at;
            return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "control byte 0x%02X in string",
                             (unsigned)byte);
        }
        if (byte == '\\') {
            *escaped = true;
            at++;
        }
        at++;
    }
    if (at >= lex->len) {
        return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "unterminated string");
    }
    *raw = at - (lex->pos + 1);
    return ODIN3_OK;
}

/* Decodes escapes from the cursor (after the opening quote) up to the closing quote. */
static odin3_status decode_string(json_lex *lex) {
    for (;;) {
        int ch = json_peek(lex);
        if (ch == '"') {
            json_advance(lex);
            return ODIN3_OK;
        }
        if (ch == '\\') {
            odin3_status st = read_escape(lex);
            if (st != ODIN3_OK) {
                return st;
            }
        } else {
            push_byte(lex, (uint8_t)ch);
            json_advance(lex);
        }
    }
}

odin3_status json_lex_string(json_lex *lex, char **out, uint32_t *len) {
    odin3_json_loc start = json_here(lex);
    size_t raw = 0;
    bool escaped = false;
    odin3_status st = scan_string(lex, &raw, &escaped);
    if (st != ODIN3_OK) {
        return st;
    }
    uint8_t *buf = odin3_arena_alloc(lex->arena, raw + 1);
    if (buf == NULL) {
        return json_fail(lex, start, ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    lex->out = buf;
    lex->out_len = 0;
    json_advance(lex);
    if (escaped) {
        st = decode_string(lex);
    } else {
        memcpy(buf, lex->text + lex->pos, raw);
        lex->out_len = raw;
        lex->pos += raw + 1;
    }
    if (st == ODIN3_OK && lex->out_len > lex->lim.max_string) {
        st = json_fail(lex, start, ODIN3_ERR_PARSE, "string longer than %zu bytes",
                       lex->lim.max_string);
    }
    *out = (char *)buf;
    *len = (uint32_t)lex->out_len;
    return st;
}

/* Adds digit to the magnitude; false on overflow past limit. */
static bool add_digit(uint64_t *mag, unsigned digit, uint64_t limit) {
    if (*mag > (limit - digit) / DEC_BASE) {
        return false;
    }
    *mag = *mag * DEC_BASE + digit;
    return true;
}

odin3_status json_lex_int(json_lex *lex, int64_t *out) {
    odin3_json_loc start = json_here(lex);
    bool neg = json_peek(lex) == '-';
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    uint64_t mag = 0;
    size_t digits = 0;
    if (neg) {
        json_advance(lex);
    }
    for (int ch = json_peek(lex); ch >= '0' && ch <= '9'; ch = json_peek(lex)) {
        if (digits == 1 && mag == 0) {
            return json_fail(lex, start, ODIN3_ERR_PARSE, "leading zero in number");
        }
        if (!add_digit(&mag, (unsigned)(ch - '0'), limit)) {
            return json_fail(lex, start, ODIN3_ERR_PARSE, "number out of the int64 range");
        }
        digits++;
        json_advance(lex);
    }
    int next = json_peek(lex);
    if (digits == 0) {
        return json_fail(lex, start, ODIN3_ERR_PARSE, "expected a digit in number");
    }
    if (next == '.' || next == 'e' || next == 'E') {
        return json_fail(lex, start, ODIN3_ERR_PARSE, "fraction or exponent in number");
    }
    *out = neg ? (int64_t)(0U - mag) : (int64_t)mag;
    return ODIN3_OK;
}
