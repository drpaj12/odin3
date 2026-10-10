/* macro.c — the macro table, the `define definition scanner (spec §7.3) and `undef. */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CAP_WHAT = 160, CRLF_CONT = 3, LF_CONT = 2 };

/* --- the table ----------------------------------------------------------------------------- */

static const char *const k_directive_names[] = {
    "begin_keywords",
    "celldefine",
    "default_nettype",
    "define",
    "else",
    "elsif",
    "end_keywords",
    "endcelldefine",
    "endif",
    "ifdef",
    "ifndef",
    "include",
    "line",
    "nounconnected_drive",
    "pragma",
    "resetall",
    "timescale",
    "unconnected_drive",
    "undef",
};

static bool bytes_eq(odin3_bytes bytes, const char *str) {
    return strlen(str) == bytes.len && memcmp(str, bytes.ptr, bytes.len) == 0;
}

bool odin3_pp_is_directive_name(odin3_bytes name) {
    for (size_t i = 0; i < sizeof k_directive_names / sizeof k_directive_names[0]; i++) {
        if (bytes_eq(name, k_directive_names[i])) {
            return true;
        }
    }
    return false;
}

uint32_t odin3_pp_macro_find(const odin3_pp *pp, odin3_bytes name) {
    uint32_t id = 0;
    uint64_t index = 0;
    if (name.len == 0 || !odin3_strtab_find(pp->strtab, name, &id) ||
        !odin3_u64map_get(pp->macro_by_name, id, &index)) {
        return 0;
    }
    return (uint32_t)index + 1;
}

/* --- the definition scanner ---------------------------------------------------------------- */

/* One definition being scanned. */
typedef struct def_state {
    odin3_pp *pp;
    odin3_pp_deftext *txt;
    odin3_bytes name;
    uint32_t name_id;
    uint32_t name_loc;
    uint32_t after_head; /* just after the name or the formal list's ')' */
    odin3_vec formals;   /* odin3_pp_formal */
    bool has_formals;
} def_state;

typedef enum item_kind { ITEM_END, ITEM_TEXT, ITEM_LINE_COMMENT } item_kind;

static int byte_at(const odin3_pp_deftext *txt, uint32_t pos) {
    return pos < txt->len ? txt->bytes[pos] : -1;
}

static uint32_t loc_of(const odin3_pp_deftext *txt, uint32_t pos) {
    return txt->base + pos;
}

/* The length of a continuation (\ LF or \ CR LF) at pos; 0 when none or they are off. */
static uint32_t continuation(const odin3_pp_deftext *txt, uint32_t pos) {
    if (!txt->continuations || byte_at(txt, pos) != '\\') {
        return 0;
    }
    if (byte_at(txt, pos + 1) == '\n') {
        return LF_CONT;
    }
    return byte_at(txt, pos + 1) == '\r' && byte_at(txt, pos + 2) == '\n' ? CRLF_CONT : 0;
}

static void skip_spaces(odin3_pp_deftext *txt) {
    while (byte_at(txt, txt->pos) == ' ' || byte_at(txt, txt->pos) == '\t') {
        txt->pos++;
    }
}

/* Spaces, tabs, CRs and continuations (inside a formal list). */
static void skip_blanks(odin3_pp_deftext *txt) {
    for (;;) {
        int ch = byte_at(txt, txt->pos);
        uint32_t cont = continuation(txt, txt->pos);
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            txt->pos++;
        } else if (cont != 0) {
            txt->pos += cont;
        } else {
            return;
        }
    }
}

static odin3_bytes read_ident(odin3_pp_deftext *txt) {
    uint32_t start = txt->pos;
    if (odin3_pp_is_ident_start(byte_at(txt, txt->pos))) {
        while (odin3_pp_is_ident_char(byte_at(txt, txt->pos))) {
            txt->pos++;
        }
    }
    return (odin3_bytes){txt->bytes + start, txt->pos - start};
}

/* A "..." string with \ escapes; a newline (or the text's end) before the quote is an error. */
static odin3_status scan_string(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    uint32_t start = txt->pos++;
    for (int ch = byte_at(txt, txt->pos); ch >= 0 && ch != '\n'; ch = byte_at(txt, txt->pos)) {
        uint32_t cont = continuation(txt, txt->pos);
        int next = byte_at(txt, txt->pos + 1);
        if (cont != 0) {
            txt->pos += cont;
        } else if (ch == '\\' && next >= 0 && next != '\n') {
            txt->pos += 2;
        } else {
            txt->pos++;
            if (ch == '"') {
                return ODIN3_OK;
            }
        }
    }
    return odin3_pp_error(ds->pp, loc_of(txt, start), "syntax: unterminated string in macro text");
}

/* A block comment, kept in the text; it must close on the logical line. */
static odin3_status scan_block(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    uint32_t start = txt->pos;
    txt->pos += 2;
    for (int ch = byte_at(txt, txt->pos); ch >= 0 && ch != '\n'; ch = byte_at(txt, txt->pos)) {
        uint32_t cont = continuation(txt, txt->pos);
        if (ch == '*' && byte_at(txt, txt->pos + 1) == '/') {
            txt->pos += 2;
            return ODIN3_OK;
        }
        txt->pos += cont != 0 ? cont : 1;
    }
    return odin3_pp_error(ds->pp, loc_of(txt, start), "syntax: unterminated comment in macro text");
}

/* PP-9: ``, `", `\`" and `__FILE__/`__LINE__ in a body are errors; any other backtick is text. */
static odin3_status scan_backtick(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    uint32_t at = loc_of(txt, txt->pos);
    int next = byte_at(txt, txt->pos + 1);
    if (next == '`') {
        return odin3_pp_sv_operator(ds->pp, at, "``");
    }
    if (next == '"') {
        return odin3_pp_sv_operator(ds->pp, at, "`\"");
    }
    if (next == '\\' && byte_at(txt, txt->pos + 2) == '`' && byte_at(txt, txt->pos + 3) == '"') {
        return odin3_pp_sv_operator(ds->pp, at, "`\\`\"");
    }
    txt->pos++;
    odin3_pp_deftext peek = *txt;
    odin3_bytes word = read_ident(&peek);
    if (bytes_eq(word, "__FILE__") || bytes_eq(word, "__LINE__")) {
        return odin3_pp_sv_operator(ds->pp, at,
                                    bytes_eq(word, "__FILE__") ? "`__FILE__" : "`__LINE__");
    }
    return ODIN3_OK;
}

/* An escaped identifier: the backslash and every byte to the next blank. */
static void skip_escaped(odin3_pp_deftext *txt) {
    while (byte_at(txt, txt->pos) >= 0 && !odin3_pp_is_blank(byte_at(txt, txt->pos))) {
        txt->pos++;
    }
}

/* The next item of the logical line: a byte, a continuation, a string, a block comment, an
 * escaped identifier, a backtick; or the end (a newline or the text's end) or a // comment,
 * neither consumed. */
static odin3_status next_item(def_state *ds, item_kind *kind) {
    odin3_pp_deftext *txt = ds->txt;
    int ch = byte_at(txt, txt->pos);
    int next = byte_at(txt, txt->pos + 1);
    uint32_t cont = continuation(txt, txt->pos);
    bool comment = txt->comments && ch == '/';
    *kind = ITEM_TEXT;
    if (ch < 0 || ch == '\n') {
        *kind = ITEM_END;
    } else if (cont != 0) {
        txt->pos += cont;
    } else if (ch == '"') {
        return scan_string(ds);
    } else if (comment && next == '/') {
        *kind = ITEM_LINE_COMMENT;
    } else if (comment && next == '*') {
        return scan_block(ds);
    } else if (ch == '\\' && next >= 0 && !odin3_pp_is_blank(next)) {
        skip_escaped(txt);
    } else if (ch == '`') {
        return scan_backtick(ds);
    } else {
        txt->pos++;
    }
    return ODIN3_OK;
}

static bool trims(int ch, bool newlines) {
    return ch == ' ' || ch == '\t' || ch == '\r' || (newlines && ch == '\n');
}

/* A range [start, end) of the text being scanned. */
typedef struct text_span {
    uint32_t start;
    uint32_t end;
} text_span;

static odin3_status text_cap(def_state *ds, text_span span) {
    if (span.end - span.start <= ds->pp->limits.macro_text) {
        return ODIN3_OK;
    }
    char what[CAP_WHAT];
    (void)snprintf(what, sizeof what, "text of macro '%.*s'", (int)ds->name.len,
                   (const char *)ds->name.ptr);
    odin3_pp_cap cap = {what, "ODIN3_PP_MAX_MACRO_TEXT", ds->pp->limits.macro_text};
    return odin3_pp_cap_error(ds->pp, loc_of(ds->txt, span.start), cap);
}

/* Copies the span's bytes into the arena. */
static const uint8_t *copy_text(def_state *ds, text_span span) {
    uint32_t len = span.end - span.start;
    uint8_t *copy = odin3_arena_alloc(ds->pp->arena, len);
    if (copy != NULL && len > 0) {
        memcpy(copy, ds->txt->bytes + span.start, len);
    }
    return copy;
}

/* +1 for an opening bracket, -1 for a closing one, 0 otherwise. */
static int bracket(int ch) {
    if (ch == '(' || ch == '[' || ch == '{') {
        return 1;
    }
    return ch == ')' || ch == ']' || ch == '}' ? -1 : 0;
}

/* Scans to the next top-level ',' or ')' (brackets and strings nest); *open when the logical
 * line ends first. */
static odin3_status scan_to_separator(def_state *ds, bool *open) {
    const odin3_pp_deftext *txt = ds->txt;
    uint32_t depth = 0;
    *open = false;
    for (int ch = byte_at(txt, txt->pos); depth > 0 || (ch != ',' && ch != ')');
         ch = byte_at(txt, txt->pos)) {
        item_kind kind = ITEM_TEXT;
        odin3_status st = next_item(ds, &kind);
        if (st != ODIN3_OK || kind != ITEM_TEXT) {
            *open = st == ODIN3_OK;
            return st;
        }
        int delta = bracket(ch);
        if (delta > 0) {
            depth++;
        } else if (delta < 0 && depth > 0) {
            depth--;
        }
    }
    return ODIN3_OK;
}

/* A formal's default: the text to the next top-level ',' or ')', trimmed. */
static odin3_status scan_default(def_state *ds, odin3_pp_formal *formal) {
    odin3_pp_deftext *txt = ds->txt;
    skip_blanks(txt);
    text_span span = {txt->pos, txt->pos};
    bool open = false;
    odin3_status st = scan_to_separator(ds, &open);
    if (st == ODIN3_OK && open) {
        return odin3_pp_error(ds->pp, loc_of(txt, span.start),
                              "syntax: unterminated default of formal '%s' of macro '%.*s'",
                              odin3_strtab_get(ds->pp->strtab, formal->name), (int)ds->name.len,
                              (const char *)ds->name.ptr);
    }
    span.end = txt->pos;
    while (span.end > span.start && trims(txt->bytes[span.end - 1], true)) {
        span.end--;
    }
    if (st == ODIN3_OK) {
        st = text_cap(ds, span);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    formal->has_default = true;
    formal->def = loc_of(txt, span.start);
    formal->len = span.end - span.start;
    formal->text = copy_text(ds, span);
    return formal->text == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

static odin3_status add_formal(def_state *ds, odin3_bytes name, uint32_t at,
                               odin3_pp_formal **out) {
    if (ds->formals.len >= ds->pp->limits.macro_args) {
        char what[CAP_WHAT];
        (void)snprintf(what, sizeof what, "formals of macro '%.*s'", (int)ds->name.len,
                       (const char *)ds->name.ptr);
        odin3_pp_cap cap = {what, "ODIN3_PP_MAX_MACRO_ARGS", ds->pp->limits.macro_args};
        return odin3_pp_cap_error(ds->pp, at, cap);
    }
    uint32_t id = 0;
    odin3_status st = odin3_design_intern(ds->pp->design, name, &id);
    if (st != ODIN3_OK) {
        return st;
    }
    for (size_t i = 0; i < ds->formals.len; i++) {
        if (((const odin3_pp_formal *)odin3_vec_cat(&ds->formals, i))->name == id) {
            return odin3_pp_error(ds->pp, at, "syntax: duplicate formal '%.*s' of macro '%.*s'",
                                  (int)name.len, (const char *)name.ptr, (int)ds->name.len,
                                  (const char *)ds->name.ptr);
        }
    }
    *out = odin3_vec_push(&ds->formals);
    if (*out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    (*out)->name = id;
    return ODIN3_OK;
}

/* One formal and what follows it up to ',' or ')' (consumed); *done after ')'. */
static odin3_status scan_formal(def_state *ds, bool *done) {
    odin3_pp_deftext *txt = ds->txt;
    skip_blanks(txt);
    uint32_t at = loc_of(txt, txt->pos);
    odin3_bytes name = read_ident(txt);
    if (name.len == 0) {
        return odin3_pp_error(ds->pp, at,
                              "syntax: formal argument of macro '%.*s' is not an "
                              "identifier",
                              (int)ds->name.len, (const char *)ds->name.ptr);
    }
    odin3_pp_formal *formal = NULL;
    odin3_status st = add_formal(ds, name, at, &formal);
    skip_blanks(txt);
    if (st == ODIN3_OK && byte_at(txt, txt->pos) == '=') {
        txt->pos++;
        st = scan_default(ds, formal);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    int ch = byte_at(txt, txt->pos);
    if (ch != ',' && ch != ')') {
        return odin3_pp_error(ds->pp, loc_of(txt, txt->pos),
                              "syntax: expected ',' or ')' in the formal list of macro '%.*s'",
                              (int)ds->name.len, (const char *)ds->name.ptr);
    }
    txt->pos++;
    *done = ch == ')';
    return ODIN3_OK;
}

/* The formal list; the cursor is on the '(' that immediately follows the name. */
static odin3_status scan_formals(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    txt->pos++;
    ds->has_formals = true;
    skip_blanks(txt);
    if (byte_at(txt, txt->pos) == ')') {
        txt->pos++;
        return ODIN3_OK;
    }
    bool done = false;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && !done) {
        st = scan_formal(ds, &done);
    }
    return st;
}

/* Enters the scanned definition (reserve-before-mutate: the table is unchanged on failure). */
static odin3_status store(def_state *ds, text_span body) {
    uint32_t body_len = body.end - body.start;
    odin3_pp *pp = ds->pp;
    uint64_t old = 0;
    bool redefinition = odin3_u64map_get(pp->macro_by_name, ds->name_id, &old);
    if (!redefinition && odin3_u64map_count(pp->macro_by_name) >= pp->limits.macros) {
        odin3_pp_cap cap = {"macros", "ODIN3_PP_MAX_MACROS", pp->limits.macros};
        return odin3_pp_cap_error(pp, ds->name_loc, cap);
    }
    odin3_pp_macro mac = {.name = ds->name_id,
                          .at = ds->txt->at,
                          .def = loc_of(ds->txt, body_len == 0 ? ds->after_head : body.start),
                          .len = body_len,
                          .body = copy_text(ds, body),
                          .nformals = (uint32_t)ds->formals.len,
                          .has_formals = ds->has_formals};
    size_t bytes = sizeof(odin3_pp_formal) * ds->formals.len;
    mac.formals = odin3_arena_alloc(pp->arena, bytes);
    if (mac.body == NULL || mac.formals == NULL ||
        odin3_vec_reserve(&pp->macros, pp->macros.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (bytes > 0) {
        memcpy(mac.formals, ds->formals.data, bytes);
    }
    if (odin3_u64map_put(pp->macro_by_name, (odin3_kv){ds->name_id, pp->macros.len}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *(odin3_pp_macro *)odin3_vec_push(&pp->macros) = mac;
    return ODIN3_OK;
}

/* The body: from the first non-blank to the end of the logical line, a // comment ending it,
 * trailing blanks trimmed; the cursor ends at the newline (before a CR that precedes it). */
static odin3_status scan_body(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    skip_spaces(txt);
    uint32_t start = txt->pos;
    item_kind kind = ITEM_TEXT;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && kind == ITEM_TEXT) {
        st = next_item(ds, &kind);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t end = txt->pos;
    while (kind == ITEM_LINE_COMMENT && byte_at(txt, txt->pos) >= 0 &&
           byte_at(txt, txt->pos) != '\n') {
        txt->pos++;
    }
    while (end > start && trims(txt->bytes[end - 1], false)) {
        end--;
    }
    if (byte_at(txt, txt->pos) == '\n' && txt->pos > end && txt->bytes[txt->pos - 1] == '\r') {
        txt->pos--;
    }
    st = text_cap(ds, (text_span){start, end});
    return st != ODIN3_OK ? st : store(ds, (text_span){start, end});
}

static odin3_status scan_head(def_state *ds) {
    odin3_pp_deftext *txt = ds->txt;
    skip_spaces(txt);
    ds->name_loc = loc_of(txt, txt->pos);
    ds->name = read_ident(txt);
    if (ds->name.len == 0) {
        return odin3_pp_error(ds->pp, ds->name_loc, "syntax: expected a macro name after `define");
    }
    if (odin3_pp_is_directive_name(ds->name)) {
        return odin3_pp_error(ds->pp, ds->name_loc,
                              "syntax: compiler directive '%.*s' cannot be a macro name",
                              (int)ds->name.len, (const char *)ds->name.ptr);
    }
    odin3_status st = odin3_design_intern(ds->pp->design, ds->name, &ds->name_id);
    if (st == ODIN3_OK && byte_at(txt, txt->pos) == '(') {
        st = scan_formals(ds);
    }
    ds->after_head = txt->pos;
    return st;
}

odin3_status odin3_pp_define_scan(odin3_pp *pp, odin3_pp_deftext *text) {
    def_state ds = {.pp = pp, .txt = text};
    odin3_vec_init(&ds.formals, sizeof(odin3_pp_formal));
    odin3_status st = scan_head(&ds);
    if (st == ODIN3_OK) {
        st = scan_body(&ds);
    }
    odin3_vec_free(&ds.formals);
    return st;
}

/* --- the directives ------------------------------------------------------------------------ */

odin3_status odin3_pp_define_directive(odin3_pp *pp, uint32_t at) {
    odin3_pp_frame *frame = odin3_pp_top(pp);
    const odin3_pp_textrun *run = odin3_vec_cat(&pp->runs, frame->first_run);
    uint32_t offset = frame->cur.run < frame->nruns ? frame->cur.offset : run->len;
    odin3_pp_deftext text = {run->bytes, run->len, run->loc, offset, at, true, true};
    odin3_status st = odin3_pp_define_scan(pp, &text);
    if (st == ODIN3_OK) {
        odin3_pp_advance(pp, text.pos - offset);
        pp->gap = true;
    }
    return st;
}

odin3_status odin3_pp_undef_directive(odin3_pp *pp, uint32_t at) {
    (void)at;
    odin3_pp_skip_spaces(pp);
    uint32_t loc = odin3_pp_loc_here(pp);
    odin3_bytes name = odin3_pp_read_ident(pp);
    if (name.len == 0) {
        return odin3_pp_error(pp, loc, "syntax: expected a macro name after `undef");
    }
    if (odin3_pp_is_directive_name(name)) {
        return odin3_pp_error(pp, loc, "syntax: compiler directive '%.*s' cannot be a macro name",
                              (int)name.len, (const char *)name.ptr);
    }
    uint32_t id = 0;
    if (odin3_strtab_find(pp->strtab, name, &id)) {
        (void)odin3_u64map_remove(pp->macro_by_name, id);
    }
    pp->gap = true;
    return ODIN3_OK;
}
