/* tb_fmt.c — $display-family formatting and $readmemh/$readmemb (spec section 2). */
#include "sim/tb_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t HALF_MASK = 0xFFFFFFFFU;

enum {
    DEC_BUF = 320, /* decimal digits of a TB_MAX_WIDTH-bit value, with room to spare */
    HALF_BITS = 32,
    BASE_BIN = 2,
    BASE_OCT = 8,
    BASE_DEC = 10,
    BASE_HEX = 16,
    BITS_PER_BYTE = 8,
    BITS_PER_OCT = 3,
    BITS_PER_HEX = 4,
    MISS_CHARS = 4,
    TIME_WIDTH = 20 /* default minimum field width of %t */
};

typedef struct sbuf {
    char *buf; /* always NUL-terminated once the first byte is stored; NULL after an allocation
                  failure */
    size_t cap, len;
} sbuf;

enum { SBUF_CAP0 = 256 };

static bool sbuf_reserve(sbuf *out, size_t extra) {
    if (out->buf == NULL) {
        return false;
    }
    size_t need = out->len + extra + 1U;
    if (need <= out->cap) {
        return true;
    }
    size_t cap = out->cap;
    while (cap < need) {
        cap *= 2U;
    }
    char *grown = realloc(out->buf, cap);
    if (grown == NULL) {
        free(out->buf);
        out->buf = NULL;
        return false;
    }
    out->buf = grown;
    out->cap = cap;
    return true;
}

static void put(sbuf *out, const char *text, size_t count) {
    if (!sbuf_reserve(out, count)) {
        return;
    }
    memcpy(out->buf + out->len, text, count);
    out->len += count;
    out->buf[out->len] = '\0';
}

/* The character of one digit (count bits, their v and u bits) of %b, %o or %h: the digit when
 * known; x or z when every bit is x or every bit is z; else X when a bit is x, else Z (iverilog). */
typedef struct digit_bits {
    uint32_t v;
    uint32_t u;
    uint32_t all; /* the mask of the digit's bits */
} digit_bits;

static char digit_char(digit_bits dig) {
    if (dig.u == 0) {
        return "0123456789abcdef"[dig.v];
    }
    if (dig.u == dig.all && dig.v == 0) {
        return 'x';
    }
    if (dig.u == dig.all && dig.v == dig.all) {
        return 'z';
    }
    return (dig.u & ~dig.v) != 0 ? 'X' : 'Z';
}

/* How a value is printed in a power-of-two base: bits per digit, and whether the digits are
 * padded to the full width (else leading 0 digits are dropped). */
typedef struct radix {
    uint32_t bits_per;
    bool pad;
} radix;

static void put_radix(sbuf *out, const tbv *val, uint32_t width, radix rd) {
    char buf[TB_MAX_WIDTH + 1];
    uint32_t ndig = (width + rd.bits_per - 1U) / rd.bits_per;
    uint32_t count = 0;
    for (uint32_t dig = ndig; dig-- > 0;) {
        digit_bits bits = {0, 0, 0};
        for (uint32_t k = 0; k < rd.bits_per && (dig * rd.bits_per) + k < width; k++) {
            uint32_t code = tbv_code(val, (dig * rd.bits_per) + k);
            bits.v |= (code & 1U) << k;
            bits.u |= ((code >> 1U) & 1U) << k;
            bits.all |= 1U << k;
        }
        buf[count++] = digit_char(bits);
    }
    uint32_t first = 0;
    while (!rd.pad && first + 1U < count && buf[first] == '0') {
        first++;
    }
    put(out, &buf[first], count - first);
}

typedef struct padding {
    size_t width;
    char fill;
} padding;

static void pad_to(sbuf *out, const char *text, padding pad) {
    size_t len = strlen(text);
    for (size_t i = len; i < pad.width; i++) {
        put(out, &pad.fill, 1);
    }
    put(out, text, len);
}

typedef struct spec {
    char conv;
    int width; /* -1: default, 0: minimal */
    bool zero; /* leading 0 flag with a width */
} spec;

/* The decimal digits of a known unsigned number of width bits into text (NUL-terminated). */
static void dec_text(const uint64_t *plane, uint32_t width, char *text) {
    uint64_t rest[TB_LIMBS];
    uint32_t limbs = odin3_word_limbs(width);
    char rev[DEC_BUF];
    uint32_t count = 0;
    memcpy(rest, plane, (size_t)limbs * sizeof *rest);
    do {
        uint64_t rem = 0;
        for (uint32_t i = limbs; i-- > 0;) {
            uint64_t high = (rem << HALF_BITS) | (rest[i] >> HALF_BITS);
            uint64_t low = ((high % BASE_DEC) << HALF_BITS) | (rest[i] & HALF_MASK);
            rest[i] = ((high / BASE_DEC) << HALF_BITS) | (low / BASE_DEC);
            rem = low % BASE_DEC;
        }
        rev[count++] = (char)('0' + rem);
    } while (!odin3_word_is_zero(rest, width));
    for (uint32_t i = 0; i < count; i++) {
        text[i] = rev[count - 1U - i];
    }
    text[count] = '\0';
}

/* The default field width of %d for a value of width bits: the digits of its largest magnitude,
 * plus one for the sign of a signed value. */
static size_t dec_field(uint32_t width, bool sgn) {
    tbv big;
    char text[DEC_BUF];
    if (sgn) {
        tbv_zero(&big, width);
        odin3_word_set_bit(big.v, width - 1U);
    } else {
        tbv_fill(1, &big, width);
    }
    dec_text(big.v, width, text);
    return strlen(text) + (sgn ? 1U : 0U);
}

/* The one character %d prints for a value with x or z bits: x or z when every bit is, else X when
 * a bit is x, else Z. */
static char unknown_char(const tbv *val, uint32_t width) {
    tbv all_x;
    tbv all_z;
    tbv_fill(ODIN3_W4_X, &all_x, width);
    tbv_fill(ODIN3_W4_Z, &all_z, width);
    if (tbv_same(val, &all_x, width)) {
        return 'x';
    }
    if (tbv_same(val, &all_z, width)) {
        return 'z';
    }
    for (uint32_t i = 0; i < odin3_word_limbs(width); i++) {
        if ((val->u[i] & ~val->v[i]) != 0) {
            return 'X';
        }
    }
    return 'Z';
}

static void put_dec(sbuf *out, const tb_expr *arg, const tbv *val, spec sp) {
    char text[DEC_BUF + 1];
    uint32_t width = arg->width;
    if (!tbv_known(val, width)) {
        text[0] = unknown_char(val, width);
        text[1] = '\0';
    } else if (arg->sgn && odin3_word_bit(val->v, width - 1U)) {
        tbv mag;
        odin3_word_bin neg = {mag.v, val->v, NULL, width};
        odin3_word_neg(&neg);
        text[0] = '-';
        dec_text(mag.v, width, text + 1);
    } else {
        dec_text(val->v, width, text);
    }
    padding pad = {sp.width < 0 ? dec_field(width, arg->sgn) : (size_t)sp.width, ' '};
    if (sp.zero) {
        pad.fill = '0';
    }
    pad_to(out, text, pad);
}

/* %t: the time argument in the precision of the `timescale (LRM 21.2.1.5 defaults of $timeformat:
 * units = precision, no decimals, no suffix), right aligned in 20 columns; %0t is minimal. The
 * units come from the testbench's own `timescale; the LRM default precision (no `timescale) is the
 * smallest precision in the design, which sim_tb does not see, so a testbench without a `timescale
 * prints in its time unit (1 ns). A time with x or z bits prints as %d does. */
static void put_time(tb_ev *ev, sbuf *out, const tbv *val, uint32_t width, spec sp) {
    char text[DEC_BUF];
    uint64_t num = 0;
    padding pad = {sp.width < 0 ? (size_t)TIME_WIDTH : (size_t)sp.width, sp.zero ? '0' : ' '};
    if (!tbv_known(val, width)) {
        text[0] = unknown_char(val, width);
        text[1] = '\0';
    } else if (!tbv_u64(val, width, &num) || num > UINT64_MAX / ev->tb->time_scale) {
        ev->failed = true;
        ev->why = "%t: the time in the timescale precision does not fit in 64 bits";
        return;
    } else {
        uint64_t scaled = num * ev->tb->time_scale;
        (void)snprintf(text, sizeof text, "%llu", (unsigned long long)scaled);
    }
    pad_to(out, text, pad);
}

/* %c and %s use the known bits (x and z as 0, as iverilog): bytes from the top, %s without
 * leading NULs. */
static void put_string_value(sbuf *out, const tbv *val, uint32_t width) {
    bool started = false;
    for (uint32_t byte = (width + BITS_PER_BYTE - 1U) / BITS_PER_BYTE; byte > 0; byte--) {
        uint32_t ch = 0;
        for (uint32_t k = 0; k < BITS_PER_BYTE; k++) {
            uint32_t pos = ((byte - 1U) * BITS_PER_BYTE) + k;
            ch |= pos < width && tbv_code(val, pos) == 1U ? 1U << k : 0U;
        }
        started = started || ch != 0;
        if (started) {
            char text = (char)ch;
            put(out, &text, 1);
        }
    }
}

static void put_arg(tb_ev *ev, sbuf *out, const tb_expr *arg, spec sp) {
    if (arg->kind == E_STR) {
        put(out, arg->str, strlen(arg->str));
        return;
    }
    tbv val;
    tb_eval(ev, arg, arg->width, arg->sgn, &val);
    uint32_t bits = arg->width;
    bool pad = sp.width != 0;
    char ch = (char)(val.v[0] & ~val.u[0]);
    switch (sp.conv) {
    case 'b':
        put_radix(out, &val, bits, (radix){1, pad});
        break;
    case 'h':
    case 'x':
        put_radix(out, &val, bits, (radix){BITS_PER_HEX, pad});
        break;
    case 'o':
        put_radix(out, &val, bits, (radix){BITS_PER_OCT, pad});
        break;
    case 't':
        put_time(ev, out, &val, bits, sp);
        break;
    case 'c':
        put(out, &ch, 1);
        break;
    case 's':
        put_string_value(out, &val, bits);
        break;
    default:
        put_dec(out, arg, &val, sp);
        break;
    }
}

bool tb_fmt_valid(const char *fmt, char *bad) {
    for (const char *cur = fmt; *cur != '\0'; cur++) {
        if (*cur != '%') {
            continue;
        }
        cur++;
        while (isdigit((unsigned char)*cur)) {
            cur++;
        }
        if (strchr("dbhxocsmt%", *cur) == NULL || *cur == '\0') {
            *bad = *cur;
            return false;
        }
    }
    return true;
}

/* Parses the spec after '%' at *cur; returns its conv char. */
static spec read_spec(const char **cur) {
    spec sp = {'d', -1, false};
    if (isdigit((unsigned char)**cur)) {
        sp.zero = **cur == '0';
        sp.width = 0;
        while (isdigit((unsigned char)**cur)) {
            sp.width = sp.width * BASE_DEC + (**cur - '0');
            (*cur)++;
        }
    }
    sp.conv = (char)tolower((unsigned char)**cur);
    return sp;
}

typedef struct fargs {
    tb_expr *const *args;
    uint32_t nargs, next;
} fargs;

static void put_one(tb_ev *ev, sbuf *out, spec sp, fargs *fa) {
    if (sp.conv == '%') {
        put(out, "%", 1);
    } else if (sp.conv == 'm') {
        put(out, ev->tb->name, strlen(ev->tb->name));
    } else if (fa->next < fa->nargs) {
        put_arg(ev, out, fa->args[fa->next++], sp);
    } else {
        const char miss[MISS_CHARS] = {'<', '%', sp.conv, '>'};
        put(out, miss, sizeof miss);
    }
}

static uint32_t format_string(tb_ev *ev, sbuf *out, const char *fmt, tb_expr *const *args,
                              uint32_t nargs) {
    fargs fa = {args, nargs, 0};
    for (const char *cur = fmt; *cur != '\0'; cur++) {
        if (*cur != '%') {
            put(out, cur, 1);
            continue;
        }
        cur++;
        put_one(ev, out, read_spec(&cur), &fa);
    }
    return fa.next;
}

char *tb_format(tb_ev *ev, tb_expr *const *args, uint32_t nargs) {
    sbuf out = {malloc(SBUF_CAP0), SBUF_CAP0, 0};
    if (out.buf == NULL) {
        return NULL;
    }
    out.buf[0] = '\0';
    uint32_t first = 0;
    if (nargs > 0 && args[0]->kind == E_STR) {
        first = 1U + format_string(ev, &out, args[0]->str, args + 1, nargs - 1U);
    }
    for (uint32_t i = first; i < nargs; i++) {
        spec sp = {'d', -1, false};
        put_arg(ev, &out, args[i], sp);
    }
    return out.buf;
}

/* --- $readmem --- */
static char *slurp(const char *file) {
    FILE *fp = fopen(file, "rb");
    if (fp == NULL) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    char *buf = size < 0 ? NULL : malloc((size_t)size + 1U);
    if (buf != NULL) {
        size_t got = fread(buf, 1, (size_t)size, fp);
        buf[got] = '\0';
    }
    (void)fclose(fp);
    return buf;
}

/* Blanks the comment that starts at text[0]; returns how many characters it covered (0: none). */
static size_t blank_comment(char *text) {
    size_t len = 0;
    if (text[0] == '/' && text[1] == '/') {
        while (text[len] != '\n' && text[len] != '\0') {
            text[len++] = ' ';
        }
    } else if (text[0] == '/' && text[1] == '*') {
        text[len++] = ' ';
        text[len++] = ' ';
        while (text[len] != '\0' && !(text[len] == '*' && text[len + 1U] == '/')) {
            text[len++] = ' ';
        }
        if (text[len] != '\0') {
            text[len++] = ' ';
            text[len++] = ' ';
        }
    }
    return len;
}

static void strip_comments(char *text) {
    size_t at = 0;
    while (text[at] != '\0') {
        size_t len = blank_comment(&text[at]);
        at += len > 0 ? len : 1U;
    }
}

typedef struct readmem_state {
    int64_t addr;
    int64_t last;
} readmem_state;

/* Stores one word token; 1 to continue, 0 to stop (past the end), -1 on a bad word. A word has
 * hex or binary digits, x, z and ? among them; it is zero-padded to the memory's width (iverilog).
 */
static int readmem_word(const tb_readmem_req *req, readmem_state *st, char *tok) {
    char *end = NULL;
    if (tok[0] == '@') {
        st->addr = (int64_t)strtoull(tok + 1, &end, BASE_HEX);
        return 1;
    }
    tbv val;
    uint32_t bits = 0;
    tb_digits digits = {tok, strlen(tok), req->hex ? (unsigned)BASE_HEX : (unsigned)BASE_BIN};
    if (!tb_parse_digits(&digits, &val, &bits)) {
        (void)snprintf(req->err, req->errcap, "$readmem: bad word `%s`", tok);
        return -1;
    }
    if (st->addr >= 0 && st->addr <= st->last && (uint64_t)st->addr < req->mem->depth) {
        tb_resize rs = {bits, req->mem->width, false, false};
        tbv_extend(&val, &rs);
        tb_var_store(req->mem, (uint32_t)st->addr++, &val);
        return 1;
    }
    return 0;
}

bool tb_readmem(const tb_readmem_req *req) {
    char *text = slurp(req->file);
    if (text == NULL) {
        (void)snprintf(req->err, req->errcap, "$readmem: cannot read `%s`", req->file);
        return false;
    }
    strip_comments(text);
    readmem_state st = {req->from, req->to >= 0 ? req->to : (int64_t)req->mem->depth - 1};
    int status = 1;
    char *save = NULL;
    for (char *tok = strtok_r(text, " \t\r\n", &save); tok != NULL && status > 0;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        status = readmem_word(req, &st, tok);
    }
    free(text);
    return status >= 0;
}
