/*
 * srcman_print.c — printing locations (spec §3.4): file:line:col, the macro and include chain,
 * and located diagnostics through util/log.
 */
#include "ast/srcman.h"
#include "ir/design.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* A decoded location ready to print; "<unknown>", 0, 0 when it has no file location. */
typedef struct where {
    const char *name;
    uint32_t line;
    uint32_t col;
} where;

static where where_of(const odin3_srcman *sm, odin3_loc loc, odin3_srcfmt style) {
    where out = {"<unknown>", 0, 0};
    odin3_srcpos pos;
    if (sm == NULL || !odin3_srcman_decode(sm, loc, &pos)) {
        return out;
    }
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, odin3_srcman_file_loc(sm, loc));
    uint32_t name = rec->name;
    uint32_t resolved = odin3_srcman_file_of(sm, rec)->resolved;
    if (style == ODIN3_SRCFMT_RESOLVED && resolved != 0) {
        name = resolved;
    }
    out.name = odin3_strtab_get(sm->strtab, name);
    out.line = pos.line;
    out.col = pos.col;
    return out;
}

odin3_status odin3_srcman_format(const odin3_srcman *sm, odin3_loc loc, odin3_strbuf *out,
                                 odin3_srcfmt style) {
    if (sm == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_format: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    where at = where_of(sm, loc, style);
    return odin3_strbuf_appendf(out, "%s:%" PRIu32 ":%" PRIu32, at.name, at.line, at.col);
}

/* --- the chain ----------------------------------------------------------------------------- */

static uint32_t spelled_file_loc(const odin3_srcman *sm, uint32_t loc) {
    return odin3_srcman_file_loc(sm, odin3_srcman_spelling(sm, (odin3_loc){loc})).v;
}

/* The entry for one non-FILE buffer of the walk, moving *loc to where the walk continues. */
static odin3_srcchain_entry chain_step(const odin3_srcman *sm, const odin3_srcbuf_rec *rec,
                                       uint32_t *loc) {
    odin3_srcchain_entry entry = {0, rec->name, 0};
    uint32_t spelled = rec->def + (*loc - rec->start);
    if (rec->kind == ODIN3_SRCBUF_MACRO_ARG) {
        entry.kind = ODIN3_SRCCHAIN_ARG;
        entry.at = spelled_file_loc(sm, rec->parent); /* the formal, as spelled */
        *loc = spelled;
    } else if (rec->kind == ODIN3_SRCBUF_EXPANSION) {
        entry.kind = ODIN3_SRCCHAIN_EXPANSION;
        entry.at = odin3_srcman_file_loc(sm, (odin3_loc){spelled}).v;
        *loc = rec->parent;
    } else {
        const odin3_srcbuf_rec *body = odin3_srcman_rec_of(sm, (odin3_loc){rec->parent});
        entry.kind = ODIN3_SRCCHAIN_SCRATCH;
        entry.name = body != NULL ? body->name : 0; /* the macro whose body pasted */
        entry.at = spelled_file_loc(sm, rec->parent);
        *loc = rec->parent;
    }
    return entry;
}

odin3_status odin3_srcman_chain(const odin3_srcman *sm, odin3_loc loc, odin3_srcchain_visit visit,
                                void *user) {
    if (sm == NULL || visit == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_chain: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t cur = loc.v;
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, loc);
    while (rec != NULL && rec->kind != ODIN3_SRCBUF_FILE) {
        odin3_srcchain_entry entry = chain_step(sm, rec, &cur);
        visit(user, &entry);
        rec = odin3_srcman_rec_of(sm, (odin3_loc){cur});
    }
    while (rec != NULL && rec->parent != 0) {
        odin3_srcchain_entry entry = {ODIN3_SRCCHAIN_INCLUDE, rec->name,
                                      odin3_srcman_file_loc(sm, (odin3_loc){rec->parent}).v};
        visit(user, &entry);
        rec = odin3_srcman_rec_of(sm, (odin3_loc){entry.at});
    }
    return ODIN3_OK;
}

typedef struct chain_text {
    const odin3_srcman *sm;
    odin3_strbuf buf;
    odin3_status st;
} chain_text;

static odin3_status append_entry(chain_text *text, const odin3_srcchain_entry *entry) {
    const char *name = odin3_strtab_get(text->sm->strtab, entry->name);
    where at = where_of(text->sm, (odin3_loc){entry->at}, ODIN3_SRCFMT_GIVEN);
    switch (entry->kind) {
    case ODIN3_SRCCHAIN_ARG:
        return odin3_strbuf_appendf(&text->buf,
                                    "\n  in argument of macro '%s' at %s:%" PRIu32 ":%" PRIu32,
                                    name, at.name, at.line, at.col);
    case ODIN3_SRCCHAIN_EXPANSION:
        return odin3_strbuf_appendf(&text->buf,
                                    "\n  expanded from macro '%s' at %s:%" PRIu32 ":%" PRIu32, name,
                                    at.name, at.line, at.col);
    case ODIN3_SRCCHAIN_SCRATCH:
        return odin3_strbuf_appendf(&text->buf, "\n  pasted by macro '%s'", name);
    default:
        return odin3_strbuf_appendf(&text->buf, "\n  included from %s:%" PRIu32, at.name, at.line);
    }
}

static void visit_text(void *user, const odin3_srcchain_entry *entry) {
    chain_text *text = user;
    if (text->st == ODIN3_OK) {
        text->st = append_entry(text, entry);
    }
}

odin3_status odin3_srcman_format_chain(const odin3_srcman *sm, odin3_loc loc, odin3_strbuf *out) {
    if (sm == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_format_chain: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    chain_text text = {sm, {0}, ODIN3_OK};
    odin3_strbuf_init(&text.buf);
    odin3_status st = odin3_srcman_chain(sm, loc, visit_text, &text);
    st = st != ODIN3_OK ? st : text.st;
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){text.buf.data, text.buf.len});
    }
    odin3_strbuf_free(&text.buf);
    return st;
}

/* --- located diagnostics ------------------------------------------------------------------- */

static const char *level_word(odin3_log_level level) {
    static const char *const words[ODIN3_LOG_LEVEL_COUNT] = {"error", "warning", "info", "debug"};
    return level < ODIN3_LOG_LEVEL_COUNT ? words[level] : "error";
}

void odin3_srcman_vdiag(const odin3_srcman *sm, odin3_log_level level, odin3_loc loc,
                        const char *fmt, va_list args) {
    char msg[ODIN3_LOG_BUF];
    if (vsnprintf(msg, sizeof msg, fmt, args) < 0) {
        msg[0] = '\0';
    }
    where at = where_of(sm, loc, ODIN3_SRCFMT_GIVEN);
    const char *word = level_word(level);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    bool full = odin3_strbuf_appendf(&buf, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", at.name, at.line,
                                     at.col, word, msg) == ODIN3_OK &&
                (sm == NULL || odin3_srcman_format_chain(sm, loc, &buf) == ODIN3_OK);
    if (full) {
        odin3_log(level, "%s", buf.data);
    } else { /* out of memory: the header alone, formatted without allocating */
        odin3_log(level, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", at.name, at.line, at.col, word, msg);
    }
    odin3_strbuf_free(&buf);
}

void odin3_diag(odin3_design *design, odin3_log_level level, odin3_loc loc, const char *fmt, ...) {
    odin3_srcman *sm = NULL;
    if (design == NULL || odin3_design_get_srcman(design, &sm) != ODIN3_OK) {
        sm = NULL;
    }
    va_list args;
    va_start(args, fmt);
    odin3_srcman_vdiag(sm, level, loc, fmt, args);
    va_end(args);
}
