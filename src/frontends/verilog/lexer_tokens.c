/*
 * lexer_tokens.c — the token rules' functions of the Verilog lexer (2C spec §2.3, §2.4, §2.7,
 * §2.8): numbers and reals, words (keywords, identifiers and the opaque-body openers), escaped
 * identifiers, system names, strings and operators. Each reads its bytes from the stream at
 * [tok_start, offset) and returns its token, or VL_SKIP inside an opaque body.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/lexer_internal.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"
#include "verilog_parser.h" /* generated (build tree): TOK_* */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    VL_PRINTABLE_FIRST = 0x21, /* '!': an escaped identifier's bytes (§2.4) */
    VL_PRINTABLE_LAST = 0x7e,  /* '~' */
    VL_OCTAL_DIGITS = 3,       /* \ddd */
    VL_OCTAL_BASE = 8,
    VL_BYTE_MAX = 0xff
};

/* --- numbers (§2.3) ------------------------------------------------------------------------ */

/* A number piece: its bytes as the value (a view into the stream). */
static int piece(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc, int kind) {
    if (vl_in_body(st)) {
        return VL_SKIP;
    }
    val->text = vl_text(st);
    return vl_token(st, loc, kind);
}

int odin3_vl_number(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    return piece(st, val, loc, TOK_UNSIGNED_NUMBER);
}

int odin3_vl_base(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    st->base = vl_text(st);
    return piece(st, val, loc, TOK_BASE);
}

int odin3_vl_digits(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    return piece(st, val, loc, TOK_BASED_DIGITS);
}

void odin3_vl_based_exit(odin3_vl_state *st) {
    st->offset = st->tok_start; /* yyless(0) put the byte back */
    const char *bytes = st->stream.text.ptr;
    if (st->body.kind == 0 && bytes[st->tok_start] == '_') {
        vl_error(st, vl_loc(st, st->tok_start), "digits after %.*s cannot start with '_'",
                 (int)st->base.len, (const char *)st->base.ptr);
    }
}

int odin3_vl_real(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    if (vl_in_body(st)) {
        return VL_SKIP;
    }
    odin3_bytes text = vl_text(st);
    odin3_strbuf_clear(&st->scratch);
    const char *bytes = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] != '_' &&
            odin3_strbuf_append(&st->scratch, (odin3_bytes){bytes + i, 1}) != ODIN3_OK) {
            return vl_fatal(st, ODIN3_ERR_NO_MEMORY);
        }
    }
    double value = strtod(st->scratch.data, NULL);
    val->u = 0;
    if (!isfinite(value)) {
        vl_error(st, vl_loc(st, st->tok_start), "real literal out of range");
    } else {
        odin3_status status = odin3_ast_real_new(st->ast, value, &val->u);
        if (status != ODIN3_OK) {
            return vl_fatal(st, status);
        }
    }
    return vl_token(st, loc, TOK_REAL_NUMBER);
}

/* --- identifiers and keywords (§2.4, §2.5) -------------------------------------------------- */

/* An identifier's or system name's name: at most the cap (a located error past it), interned;
 * the token of kind. */
static int ident(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc, odin3_bytes name, int kind) {
    if (name.len > st->run->limits.ident_bytes) {
        vl_error(st, vl_loc(st, st->tok_start), "identifier longer than %u bytes",
                 st->run->limits.ident_bytes);
        name.len = st->run->limits.ident_bytes;
    }
    if (!vl_intern(st, name, &val->u)) {
        return VL_SKIP;
    }
    return vl_token(st, loc, kind);
}

/* The three opaque bodies (§2.8): the opening and closing keywords, how a message names one. */
static const char *const vl_body_open[] = {"specify", "primitive", "config"};
static const char *const vl_body_end[] = {"endspecify", "endprimitive", "endconfig"};
static const char *const vl_body_name[] = {"specify block", "primitive", "config"};

/* The index of an opener's token in the tables above. */
static size_t body_index(int kind) {
    if (kind == TOK_OPAQUE_SPECIFY) {
        return 0;
    }
    return kind == TOK_OPAQUE_PRIMITIVE ? 1 : 2;
}

void vl_body_unclosed(odin3_vl_state *st) {
    size_t which = body_index(st->body.kind);
    vl_error(st, st->body.loc, "'%s' has no matching '%s'", vl_body_open[which],
             vl_body_end[which]);
    st->body.kind = 0;
}

/* The end keyword of the body: its token over the body, the text a view of its bytes (empty
 * past the cap, §2.8), its name recorded for primitive and config. */
static int body_close(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    odin3_vl_body body = st->body;
    st->body.kind = 0;
    uint32_t len = st->offset - body.start;
    val->text = vl_bytes(st, body.start, st->offset);
    if (len > st->run->limits.text_bytes) {
        vl_error(st, body.loc, "%s longer than %u bytes", vl_body_name[body_index(body.kind)],
                 st->run->limits.text_bytes);
        val->text.len = 0;
    }
    if (body.kind != TOK_OPAQUE_SPECIFY && body.name == 0) {
        vl_error(st, body.loc, "expected a name after '%s'", vl_body_name[body_index(body.kind)]);
    } else if (body.kind != TOK_OPAQUE_SPECIFY) {
        odin3_vl_name *slot = odin3_vec_push(&st->names);
        if (slot == NULL) {
            return vl_fatal(st, ODIN3_ERR_NO_MEMORY);
        }
        *slot = (odin3_vl_name){body.start, body.name};
    }
    return vl_token_at(st, loc, (vl_span){body.start, st->offset}, body.kind);
}

/* The identifier right after a primitive or config keyword names the body (at most the
 * identifier cap, a located error past it). */
static void body_name(odin3_vl_state *st, odin3_bytes name) {
    if (st->body.kind == TOK_OPAQUE_SPECIFY) {
        return;
    }
    if (name.len > st->run->limits.ident_bytes) {
        vl_error(st, vl_loc(st, st->tok_start), "identifier longer than %u bytes",
                 st->run->limits.ident_bytes);
        name.len = st->run->limits.ident_bytes;
    }
    (void)vl_intern(st, name, &st->body.name);
}

/* A word inside a body: its end keyword closes it; the first token after the opener names it
 * when it is an identifier. */
static int body_token(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc, odin3_bytes word) {
    const char *end = vl_body_end[body_index(st->body.kind)];
    if (word.len == strlen(end) && memcmp(word.ptr, end, word.len) == 0) {
        return body_close(st, val, loc);
    }
    bool first = st->body.first;
    st->body.first = false;
    if (first && odin3_vl_keyword(word, vl_keyword_mask(st->run)) == 0) {
        body_name(st, word);
    }
    return VL_SKIP;
}

int odin3_vl_word(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    odin3_bytes word = vl_text(st);
    if (st->body.kind != 0) {
        return body_token(st, val, loc, word);
    }
    int kind = odin3_vl_keyword(word, vl_keyword_mask(st->run));
    if (kind == TOK_OPAQUE_SPECIFY || kind == TOK_OPAQUE_PRIMITIVE || kind == TOK_OPAQUE_CONFIG) {
        st->body = (odin3_vl_body){kind, st->tok_start, vl_loc(st, st->tok_start), 0, true};
        return VL_SKIP;
    }
    return kind != 0 ? vl_token(st, loc, kind) : ident(st, val, loc, word, TOK_IDENT);
}

int odin3_vl_escaped(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    odin3_bytes text = vl_text(st);
    odin3_bytes name = {(const char *)text.ptr + 1, text.len - 1};
    const unsigned char *bytes = name.ptr;
    size_t bad = 0;
    while (bad < name.len && bytes[bad] >= VL_PRINTABLE_FIRST && bytes[bad] <= VL_PRINTABLE_LAST) {
        bad++;
    }
    if (st->body.kind != 0) {
        if (st->body.first && bad == name.len) {
            body_name(st, name);
        }
        st->body.first = false;
        return VL_SKIP;
    }
    if (bad < name.len) {
        vl_error(st, vl_loc(st, st->tok_start + 1 + (uint32_t)bad),
                 "unexpected byte 0x%02x in an escaped identifier", (unsigned int)bytes[bad]);
        val->u = 0;
        return vl_token(st, loc, TOK_IDENT);
    }
    return ident(st, val, loc, name, TOK_IDENT);
}

void odin3_vl_escape_empty(odin3_vl_state *st) {
    if (!vl_in_body(st)) {
        vl_error(st, vl_loc(st, st->tok_start), "empty escaped identifier");
    }
}

int odin3_vl_system(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    if (vl_in_body(st)) {
        return VL_SKIP;
    }
    return ident(st, val, loc, vl_text(st), TOK_SYSTEM_IDENT);
}

/* --- strings (§2.4) ------------------------------------------------------------------------- */

/* A string being unescaped into st->scratch: bytes the token, pos the next byte, end the end of
 * its text; cap the string cap; over: bytes were cut; closed: an unescaped '"' ended it. */
typedef struct vl_string {
    odin3_vl_state *st;
    const char *bytes;
    size_t pos, end, cap;
    bool over, closed;
} vl_string;

/* Appends len bytes, at most up to the cap; false on out of memory. */
static bool string_put(vl_string *str, const char *bytes, size_t len) {
    size_t room = str->cap - str->st->scratch.len;
    if (len > room) {
        str->over = true;
        len = room;
    }
    return odin3_strbuf_append(&str->st->scratch, (odin3_bytes){bytes, len}) == ODIN3_OK;
}

/* The loc of the byte at pos. */
static odin3_loc string_loc(const vl_string *str) {
    return vl_loc(str->st, str->st->tok_start + (uint32_t)str->pos);
}

/* \ddd at pos (the backslash): its byte, or a located error for NUL or a value over 0377. */
static bool string_octal(vl_string *str) {
    const char *digits = str->bytes + str->pos + 1;
    size_t room = str->end - str->pos - 1;
    unsigned int value = 0;
    size_t len = 0;
    while (len < VL_OCTAL_DIGITS && len < room && digits[len] >= '0' && digits[len] <= '7') {
        value = value * VL_OCTAL_BASE + (unsigned int)(digits[len] - '0');
        len++;
    }
    odin3_loc at = string_loc(str);
    str->pos += 1 + len;
    if (value == 0) {
        vl_error(str->st, at, "a string cannot hold a NUL byte (\\%.*s)", (int)len, digits);
        return true;
    }
    if (value > VL_BYTE_MAX) {
        vl_error(str->st, at, "octal escape \\%.*s is out of range", (int)len, digits);
        return true;
    }
    char byte = (char)value;
    return string_put(str, &byte, 1);
}

/* The byte \ch stands for: newline, tab, or ch itself (\\, \" and any other byte). */
static char escape_byte(char ch) {
    switch (ch) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    default:
        return ch;
    }
}

/* The escape at pos (a backslash); an unknown one keeps its byte with a located warning. */
static bool string_escape(vl_string *str) {
    if (str->pos + 1 >= str->end) {
        str->pos++; /* a backslash ending an unterminated string */
        return true;
    }
    char ch = str->bytes[str->pos + 1];
    if (ch >= '0' && ch <= '7') {
        return string_octal(str);
    }
    if (ch != 'n' && ch != 't' && ch != '\\' && ch != '"') {
        vl_warn(str->st, string_loc(str), "unknown escape '\\%c' in a string (read as '%c')", ch,
                ch);
    }
    char out = escape_byte(ch);
    str->pos += 2;
    return string_put(str, &out, 1);
}

/* Unescapes the text from pos into scratch, up to the closing quote; false on out of memory. */
static bool string_unescape(vl_string *str) {
    bool ok = true;
    while (ok && str->pos < str->end && !str->closed) {
        size_t run = str->pos;
        while (run < str->end && str->bytes[run] != '\\' && str->bytes[run] != '"' &&
               str->bytes[run] != 0) {
            run++;
        }
        ok = string_put(str, str->bytes + str->pos, run - str->pos);
        str->pos = run;
        if (!ok || run >= str->end) {
            break;
        }
        if (str->bytes[run] == '"') {
            str->closed = true;
        } else if (str->bytes[run] == 0) {
            vl_error(str->st, string_loc(str), "unexpected byte 0x00");
            str->pos++;
        } else {
            ok = string_escape(str);
        }
    }
    return ok;
}

int odin3_vl_string(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    if (vl_in_body(st)) {
        return VL_SKIP;
    }
    odin3_bytes text = vl_text(st);
    vl_string str = {st, text.ptr, 1, text.len, st->run->limits.string_bytes, false, false};
    if (str.end > 1 && str.bytes[str.end - 1] == '\r') {
        str.end--; /* an unterminated string before a CRLF ends before the \r */
    }
    odin3_strbuf_clear(&st->scratch);
    if (!string_unescape(&str)) {
        return vl_fatal(st, ODIN3_ERR_NO_MEMORY);
    }
    odin3_loc at = vl_loc(st, st->tok_start);
    if (str.over) {
        vl_error(st, at, "string longer than %zu bytes", str.cap);
    }
    if (!str.closed) {
        vl_error(st, at, "unterminated string");
    }
    if (!vl_intern(st, (odin3_bytes){st->scratch.data, st->scratch.len}, &val->u)) {
        return VL_SKIP;
    }
    return vl_token_at(st, loc, (vl_span){st->tok_start, st->tok_start + (uint32_t)str.end},
                       TOK_STRING);
}

/* --- operators and attribute brackets (§2.4, §2.7) ----------------------------------------- */

int odin3_vl_op(odin3_vl_state *st, odin3_vp_loc *loc, int kind) {
    return vl_in_body(st) ? VL_SKIP : vl_token(st, loc, kind);
}

int odin3_vl_punct(odin3_vl_state *st, odin3_vp_loc *loc) {
    const unsigned char *bytes = st->stream.text.ptr;
    return odin3_vl_op(st, loc, bytes[st->tok_start]);
}
