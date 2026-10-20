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
    FIRST_PRINTABLE = 0x20
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

static odin3_status push_byte(json_lex *lex, uint8_t byte) {
    uint8_t *slot = odin3_vec_push(&lex->scratch);
    if (slot == NULL) {
        return json_fail(lex, json_here(lex), ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    *slot = byte;
    return ODIN3_OK;
}

static odin3_status push_utf8(json_lex *lex, uint32_t cp) {
    uint8_t buf[4];
    size_t count = 0;
    if (cp < UTF8_2) {
        buf[count++] = (uint8_t)cp;
    } else if (cp < UTF8_3) {
        buf[count++] = (uint8_t)(LEAD_2 | (cp >> CONT_BITS));
    } else if (cp < UTF8_4) {
        buf[count++] = (uint8_t)(LEAD_3 | (cp >> (2 * CONT_BITS)));
        buf[count++] = (uint8_t)(CONT_MARK | ((cp >> CONT_BITS) & CONT_MASK));
    } else {
        buf[count++] = (uint8_t)(LEAD_4 | (cp >> (3 * CONT_BITS)));
        buf[count++] = (uint8_t)(CONT_MARK | ((cp >> (2 * CONT_BITS)) & CONT_MASK));
        buf[count++] = (uint8_t)(CONT_MARK | ((cp >> CONT_BITS) & CONT_MASK));
    }
    if (cp >= UTF8_2) {
        buf[count++] = (uint8_t)(CONT_MARK | (cp & CONT_MASK));
    }
    for (size_t i = 0; i < count; i++) {
        odin3_status st = push_byte(lex, buf[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
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
    return push_utf8(lex, unit);
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
    return push_byte(lex, (uint8_t)plain);
}

static odin3_status check_string_len(json_lex *lex, odin3_json_loc start, size_t len) {
    if (len > lex->lim.max_string) {
        return json_fail(lex, start, ODIN3_ERR_PARSE, "string longer than %zu bytes",
                         lex->lim.max_string);
    }
    return ODIN3_OK;
}

/* Copies the decoded bytes in the scratch buffer into the arena. */
static odin3_status copy_string(json_lex *lex, odin3_json_loc start, char **out, uint32_t *len) {
    size_t size = lex->scratch.len;
    odin3_status st = check_string_len(lex, start, size);
    if (st != ODIN3_OK) {
        return st;
    }
    *out = odin3_arena_strndup(lex->arena, lex->scratch.data, size);
    if (*out == NULL) {
        return json_fail(lex, start, ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    *len = (uint32_t)size;
    return ODIN3_OK;
}

odin3_status json_lex_string(json_lex *lex, char **out, uint32_t *len) {
    odin3_json_loc start = json_here(lex);
    odin3_vec_clear(&lex->scratch);
    json_advance(lex);
    for (;;) {
        int ch = json_peek(lex);
        odin3_status st = ODIN3_OK;
        if (ch < 0) {
            return json_fail(lex, start, ODIN3_ERR_PARSE, "unterminated string");
        }
        if (ch == '"') {
            json_advance(lex);
            return copy_string(lex, start, out, len);
        }
        if (ch < FIRST_PRINTABLE) {
            return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "control byte 0x%02X in string",
                             (unsigned)ch);
        }
        if (ch == '\\') {
            st = read_escape(lex);
        } else {
            st = push_byte(lex, (uint8_t)ch);
            json_advance(lex);
        }
        if (st == ODIN3_OK) {
            st = check_string_len(lex, start, lex->scratch.len);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
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
