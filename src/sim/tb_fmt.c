/* tb_fmt.c — $display-family formatting and $readmemh/$readmemb (spec section 2). */
#include "sim/tb_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEC_BUF = 32,
    BASE_BIN = 2,
    BASE_OCT = 8,
    BASE_DEC = 10,
    BASE_HEX = 16,
    BITS_PER_BYTE = 8,
    BITS_PER_OCT = 3,
    BITS_PER_HEX = 4,
    BYTE_MASK = 0xFF,
    MISS_CHARS = 4
};

static unsigned digits_of(uint64_t val, unsigned base) {
    unsigned count = 1;
    while (val >= base) {
        val /= base;
        count++;
    }
    return count;
}

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

typedef struct radix {
    unsigned base; /* 2, 8 or 16 */
    unsigned min_digits;
} radix;

/* val printed in rd.base with at least rd.min_digits digits, zero padded. */
static void put_radix(sbuf *out, uint64_t val, radix rd) {
    char buf[TB_MAX_WIDTH + 1];
    unsigned count = 0;
    do {
        buf[count++] = "0123456789abcdef"[val % rd.base];
        val /= rd.base;
    } while (val != 0);
    while (count < rd.min_digits) {
        buf[count++] = '0';
    }
    while (count > 0) {
        put(out, &buf[--count], 1);
    }
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

static int64_t signed_value(const tb_expr *arg, uint64_t val) {
    if (arg->width >= TB_MAX_WIDTH) {
        return (int64_t)val;
    }
    uint64_t sign = (uint64_t)1 << (arg->width - 1U);
    return (int64_t)((val ^ sign) - sign);
}

static void put_dec(sbuf *out, const tb_expr *arg, uint64_t val, spec sp) {
    char text[DEC_BUF];
    unsigned digits = 0;
    if (arg->sgn) {
        (void)snprintf(text, sizeof text, "%lld", (long long)signed_value(arg, val));
        digits = digits_of((uint64_t)1 << (arg->width - 1U), BASE_DEC) + 1U;
    } else {
        (void)snprintf(text, sizeof text, "%llu", (unsigned long long)val);
        digits = digits_of(tb_mask(arg->width), BASE_DEC);
    }
    padding pad = {sp.width < 0 ? digits : (size_t)sp.width, ' '};
    if (sp.zero) {
        pad.fill = '0';
    }
    pad_to(out, text, pad);
}

static void put_string_value(sbuf *out, const tb_expr *arg, uint64_t val) {
    bool started = false;
    for (uint32_t shift = (arg->width + BITS_PER_BYTE - 1U) / BITS_PER_BYTE; shift > 0; shift--) {
        char ch = (char)((val >> ((shift - 1U) * BITS_PER_BYTE)) & BYTE_MASK);
        started = started || ch != '\0';
        if (started) {
            put(out, &ch, 1);
        }
    }
}

static void put_arg(tb_ev *ev, sbuf *out, const tb_expr *arg, spec sp) {
    if (arg->kind == E_STR) {
        put(out, arg->str, strlen(arg->str));
        return;
    }
    uint64_t val = tb_eval(ev, arg, arg->width, arg->sgn) & tb_mask(arg->width);
    unsigned bits = arg->width;
    bool pad = sp.width != 0;
    char ch = (char)val;
    switch (sp.conv) {
    case 'b':
        put_radix(out, val, (radix){BASE_BIN, pad ? bits : 0});
        break;
    case 'h':
    case 'x':
        put_radix(out, val, (radix){BASE_HEX, pad ? (bits + BITS_PER_HEX - 1U) / BITS_PER_HEX : 0});
        break;
    case 'o':
        put_radix(out, val, (radix){BASE_OCT, pad ? (bits + BITS_PER_OCT - 1U) / BITS_PER_OCT : 0});
        break;
    case 'c':
        put(out, &ch, 1);
        break;
    case 's':
        put_string_value(out, arg, val);
        break;
    default:
        put_dec(out, arg, val, sp);
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
        if (strchr("dbhxocsm%", *cur) == NULL || *cur == '\0') {
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

static void drop_underscores(char *tok) {
    char *dst = tok;
    for (const char *src = tok; *src != '\0'; src++) {
        if (*src != '_') {
            *dst++ = *src;
        }
    }
    *dst = '\0';
}

typedef struct readmem_state {
    int64_t addr;
    int64_t last;
} readmem_state;

/* Stores one word token; 1 to continue, 0 to stop (past the end), -1 on a bad word. */
static int readmem_word(const tb_readmem_req *req, readmem_state *st, char *tok) {
    char *end = NULL;
    if (tok[0] == '@') {
        st->addr = (int64_t)strtoull(tok + 1, &end, BASE_HEX);
        return 1;
    }
    drop_underscores(tok);
    uint64_t val = strtoull(tok, &end, req->hex ? BASE_HEX : BASE_BIN);
    if (*end != '\0') {
        (void)snprintf(req->err, req->errcap, "$readmem: bad word `%s` (x/z not supported)", tok);
        return -1;
    }
    if (st->addr >= 0 && st->addr <= st->last && (uint64_t)st->addr < req->mem->depth) {
        req->mem->mem[st->addr++] = val & tb_mask(req->mem->width);
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
