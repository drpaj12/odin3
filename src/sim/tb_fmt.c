/* tb_fmt.c — $display-family formatting and $readmemh/$readmemb (spec section 2). */
#include "sim/tb_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { DEC_BUF = 32, MAX_SPEC = 8 };

static unsigned digits_of(uint64_t val, unsigned base) {
    unsigned n = 1;
    while (val >= base) {
        val /= base;
        n++;
    }
    return n;
}

static void put(char *out, size_t cap, size_t *len, const char *text, size_t n) {
    for (size_t i = 0; i < n && *len + 1 < cap; i++) {
        out[(*len)++] = text[i];
    }
    out[*len < cap ? *len : cap - 1U] = '\0';
}

/* val printed in base (2, 8, 16) with at least `digits` digits, zero padded. */
static void put_radix(char *out, size_t cap, size_t *len, uint64_t val, unsigned base,
                      unsigned digits, bool pad) {
    char buf[TB_MAX_WIDTH + 1];
    unsigned n = 0;
    do {
        buf[n++] = "0123456789abcdef"[val % base];
        val /= base;
    } while (val != 0);
    while (pad && n < digits) {
        buf[n++] = '0';
    }
    while (n > 0) {
        put(out, cap, len, &buf[--n], 1);
    }
}

static void pad_to(char *out, size_t cap, size_t *len, const char *text, size_t width, char fill) {
    size_t tl = strlen(text);
    for (size_t i = tl; i < width; i++) {
        put(out, cap, len, &fill, 1);
    }
    put(out, cap, len, text, tl);
}

static void put_dec(char *out, size_t cap, size_t *len, const tb_expr *arg, uint64_t val,
                    int width, bool zero) {
    char text[DEC_BUF];
    unsigned digits = 0;
    if (arg->sgn) {
        int64_t sv = (int64_t)(arg->width >= TB_MAX_WIDTH ? val
                                                          : (val ^ ((uint64_t)1 << (arg->width - 1U))) -
                                                                ((uint64_t)1 << (arg->width - 1U)));
        (void)snprintf(text, sizeof text, "%lld", (long long)sv);
        digits = digits_of((uint64_t)1 << (arg->width - 1U), 10) + 1U;
    } else {
        (void)snprintf(text, sizeof text, "%llu", (unsigned long long)val);
        digits = digits_of(tb_mask(arg->width), 10);
    }
    pad_to(out, cap, len, text, width < 0 ? digits : (size_t)width, zero ? '0' : ' ');
}

static void put_string_value(char *out, size_t cap, size_t *len, uint64_t val, uint32_t width) {
    bool started = false;
    for (uint32_t shift = (width + 7U) / 8U; shift > 0; shift--) {
        char ch = (char)((val >> ((shift - 1U) * 8U)) & 0xFFU);
        started = started || ch != '\0';
        if (started) {
            put(out, cap, len, &ch, 1);
        }
    }
}

typedef struct spec {
    char conv;
    int width; /* -1: default, 0: minimal */
    bool zero; /* leading 0 flag with a width */
} spec;

static void put_arg(tb_ev *ev, char *out, size_t cap, size_t *len, const tb_expr *arg, spec sp) {
    if (arg->kind == E_STR) {
        put(out, cap, len, arg->str, strlen(arg->str));
        return;
    }
    uint64_t val = tb_eval(ev, arg, arg->width, arg->sgn) & tb_mask(arg->width);
    unsigned bits = arg->width;
    switch (sp.conv) {
    case 'b':
        put_radix(out, cap, len, val, 2, bits, sp.width != 0);
        break;
    case 'h':
    case 'x':
        put_radix(out, cap, len, val, 16, (bits + 3U) / 4U, sp.width != 0);
        break;
    case 'o':
        put_radix(out, cap, len, val, 8, (bits + 2U) / 3U, sp.width != 0);
        break;
    case 'c': {
        char ch = (char)val;
        put(out, cap, len, &ch, 1);
        break;
    }
    case 's':
        put_string_value(out, cap, len, val, arg->width);
        break;
    default:
        put_dec(out, cap, len, arg, val, sp.width, sp.zero);
        break;
    }
}

bool tb_fmt_valid(const char *fmt, char *bad) {
    for (const char *c = fmt; *c != '\0'; c++) {
        if (*c != '%') {
            continue;
        }
        c++;
        while (isdigit((unsigned char)*c)) {
            c++;
        }
        if (strchr("dbhxocsm%", *c) == NULL || *c == '\0') {
            *bad = *c;
            return false;
        }
    }
    return true;
}

/* Parses the spec after '%' at *c; returns its conv char. */
static spec read_spec(const char **c) {
    spec sp = {'d', -1, false};
    if (isdigit((unsigned char)**c)) {
        sp.zero = **c == '0';
        sp.width = 0;
        while (isdigit((unsigned char)**c)) {
            sp.width = sp.width * 10 + (**c - '0');
            (*c)++;
        }
    }
    sp.conv = (char)tolower((unsigned char)**c);
    return sp;
}

static uint32_t format_string(tb_ev *ev, const char *fmt, tb_expr *const *args, uint32_t nargs,
                              char *out, size_t cap, size_t *outlen) {
    size_t len = 0;
    uint32_t next = 0;
    for (const char *c = fmt; *c != '\0'; c++) {
        if (*c != '%') {
            put(out, cap, &len, c, 1);
            continue;
        }
        c++;
        spec sp = read_spec(&c);
        if (sp.conv == '%') {
            put(out, cap, &len, "%", 1);
        } else if (sp.conv == 'm') {
            put(out, cap, &len, ev->tb->name, strlen(ev->tb->name));
        } else if (next < nargs) {
            put_arg(ev, out, cap, &len, args[next++], sp);
        } else {
            char miss[8] = {'<', '%', sp.conv, '>', 0};
            put(out, cap, &len, miss, 4);
        }
    }
    *outlen = len;
    return next;
}

void tb_format(tb_ev *ev, tb_expr *const *args, uint32_t nargs, char *out, size_t cap) {
    out[0] = '\0';
    size_t len = 0;
    uint32_t first = 0;
    if (nargs > 0 && args[0]->kind == E_STR) {
        first = 1U + format_string(ev, args[0]->str, args + 1, nargs - 1U, out, cap, &len);
    }
    for (uint32_t i = first; i < nargs; i++) {
        spec sp = {'d', -1, false};
        put_arg(ev, out, cap, &len, args[i], sp);
    }
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

static void strip_comments(char *text) {
    for (char *c = text; *c != '\0'; c++) {
        if (c[0] == '/' && c[1] == '/') {
            while (*c != '\n' && *c != '\0') {
                *c++ = ' ';
            }
            c--;
        } else if (c[0] == '/' && c[1] == '*') {
            c[0] = c[1] = ' ';
            c += 2;
            while (*c != '\0' && !(c[0] == '*' && c[1] == '/')) {
                *c++ = ' ';
            }
            if (*c != '\0') {
                c[0] = c[1] = ' ';
            }
        }
    }
}

bool tb_readmem(tb_ev *ev, const char *file, tb_var *mem, bool hex, int64_t from, int64_t to,
                char *err, size_t errcap) {
    (void)ev;
    char *text = slurp(file);
    if (text == NULL) {
        (void)snprintf(err, errcap, "$readmem: cannot read `%s`", file);
        return false;
    }
    strip_comments(text);
    int64_t addr = from;
    int64_t last = to >= 0 ? to : (int64_t)mem->depth - 1;
    bool ok = true;
    char *save = NULL;
    for (char *tok = strtok_r(text, " \t\r\n", &save); tok != NULL && ok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        uint64_t val = 0;
        char *end = NULL;
        if (tok[0] == '@') {
            addr = (int64_t)strtoull(tok + 1, &end, 16);
            continue;
        }
        for (char *c = tok; *c != '\0'; c++) {
            if (*c == '_') {
                memmove(c, c + 1, strlen(c));
                c--;
            }
        }
        val = strtoull(tok, &end, hex ? 16 : 2);
        if (*end != '\0') {
            (void)snprintf(err, errcap, "$readmem: bad word `%s` (x/z not supported)", tok);
            ok = false;
        } else if (addr >= 0 && addr <= last && (uint64_t)addr < mem->depth) {
            mem->mem[addr++] = val & tb_mask(mem->width);
        } else {
            break;
        }
    }
    free(text);
    return ok;
}
