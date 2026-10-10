/* expand.c — macro uses (PP-5), substitution as run lists and rescan (PP-6), recursion (PP-7). */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { CRLF_CONT = 3, LF_CONT = 2 };

static const uint8_t k_space = ' ';

/* --- frames -------------------------------------------------------------------------------- */

static const odin3_pp_textrun *run_at(const odin3_pp *pp, const odin3_pp_frame *frame,
                                      uint32_t index) {
    return odin3_vec_cat(&pp->runs, frame->first_run + index);
}

static bool top_is_file(odin3_pp *pp) {
    return odin3_pp_top(pp)->kind == ODIN3_PP_FRAME_FILE;
}

/* One past the last byte consumed in the top frame (AST-2's parent_end): the loc of the byte
 * before the cursor + 1, 0 when there is none or it was inserted. */
static odin3_loc consumed_end(odin3_pp *pp) {
    const odin3_pp_frame *frame = odin3_pp_top(pp);
    uint32_t run = frame->cur.run;
    uint32_t offset = frame->cur.offset;
    while (offset == 0 && run > 0) {
        run--;
        offset = run_at(pp, frame, run)->len;
    }
    if (offset == 0) {
        return (odin3_loc){0};
    }
    return odin3_pp_loc_add(run_at(pp, frame, run)->loc, offset);
}

/* Pops every exhausted EXPANSION frame on top, so a use in tail position replaces its frame
 * (spec §9: the live frame count stays small while a chain grows). */
static void pop_exhausted(odin3_pp *pp) {
    for (const odin3_pp_frame *frame = odin3_pp_top(pp);
         frame != NULL && frame->kind == ODIN3_PP_FRAME_EXPANSION && frame->cur.run >= frame->nruns;
         frame = odin3_pp_top(pp)) {
        odin3_pp_pop_frame(pp);
    }
}

/* --- errors -------------------------------------------------------------------------------- */

/* "syntax: <before>macro 'NAME'<after>" at the use. */
static odin3_status use_error(odin3_pp *pp, const odin3_pp_use *use, const char *before,
                              const char *after) {
    return odin3_pp_error(pp, use->at, "syntax: %smacro '%.*s'%s", before, (int)use->name.len,
                          (const char *)use->name.ptr, after);
}

static odin3_status live_runs_error(odin3_pp *pp, odin3_loc at) {
    odin3_pp_cap cap = {"live runs", "ODIN3_PP_MAX_LIVE_RUNS", pp->limits.live_runs};
    return odin3_pp_cap_error(pp, at, cap);
}

/* --- PP-5: collecting the actuals ---------------------------------------------------------- */

/* One use being collected: actuals go to pp->args, their starts to pp->arg_starts. */
typedef struct collect {
    odin3_pp *pp;
    const odin3_pp_use *use;
    uint32_t keep;     /* actuals recorded (formals, at least 1); later ones are only counted */
    uint32_t nactuals; /* actuals seen */
    uint32_t depth;    /* (), [], {} nesting */
    bool sep;          /* a conditional was consumed: PP-3's space before the next byte */
} collect;

static bool keeping(const collect *col) {
    return col->nactuals <= col->keep && !col->pp->skipping;
}

/* Appends a run to the current actual, merging it with a contiguous spelled predecessor. */
static odin3_status add_run(collect *col, odin3_pp_textrun piece) {
    odin3_pp *pp = col->pp;
    uint32_t start = *(const uint32_t *)odin3_vec_cat(&pp->arg_starts, pp->arg_starts.len - 1);
    odin3_pp_textrun *last =
        pp->args.len > start ? odin3_vec_at(&pp->args, pp->args.len - 1) : NULL;
    if (last != NULL && last->loc.v != 0 && piece.loc.v == last->loc.v + last->len &&
        piece.bytes == last->bytes + last->len) {
        last->len += piece.len;
        return ODIN3_OK;
    }
    if ((uint64_t)pp->runs.len + pp->args.len + 1 > pp->limits.live_runs) {
        return live_runs_error(pp, col->use->at);
    }
    odin3_pp_textrun *slot = odin3_vec_push(&pp->args);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = piece;
    return ODIN3_OK;
}

/* PP-3 inside an actual: one loc-0 space when a consumed conditional separates two non-blanks. */
static odin3_status separate(collect *col, uint8_t next) {
    odin3_pp *pp = col->pp;
    col->sep = false;
    uint32_t start = *(const uint32_t *)odin3_vec_cat(&pp->arg_starts, pp->arg_starts.len - 1);
    if (pp->args.len <= start || odin3_pp_is_blank(next)) {
        return ODIN3_OK;
    }
    const odin3_pp_textrun *last = odin3_vec_cat(&pp->args, pp->args.len - 1);
    if (last->len == 0 || odin3_pp_is_blank(last->bytes[last->len - 1])) {
        return ODIN3_OK;
    }
    return add_run(col, (odin3_pp_textrun){&k_space, 1, {0}});
}

/* Takes the next n bytes of the top frame into the current actual (only consumes them while
 * skipping or past the recorded actuals). */
static odin3_status take(collect *col, uint32_t n) {
    odin3_pp *pp = col->pp;
    while (n > 0) {
        const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
        if (run == NULL) {
            return ODIN3_OK;
        }
        uint32_t offset = odin3_pp_top(pp)->cur.offset;
        uint32_t len = run->len - offset < n ? run->len - offset : n;
        odin3_pp_textrun piece = {run->bytes + offset, len, odin3_pp_loc_add(run->loc, offset)};
        odin3_status st = ODIN3_OK;
        if (keeping(col) && col->sep) {
            st = separate(col, piece.bytes[0]);
        }
        if (st == ODIN3_OK && keeping(col)) {
            st = add_run(col, piece);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        odin3_pp_advance(pp, len);
        n -= len;
    }
    return ODIN3_OK;
}

/* A comment in an actual: one loc-0 space (I4); nothing while skipping. */
static odin3_status take_comment(collect *col) {
    odin3_pp *pp = col->pp;
    odin3_pp_advance(pp, odin3_pp_comment_length(pp));
    if (!keeping(col)) {
        return ODIN3_OK;
    }
    col->sep = false;
    return add_run(col, (odin3_pp_textrun){&k_space, 1, {0}});
}

/* A backtick while collecting: conditionals are processed in a FILE frame (w3); `include ends
 * the collection; other directives would become macro text; anything else is text. */
static odin3_status collect_backtick(collect *col) {
    odin3_pp *pp = col->pp;
    odin3_loc at = odin3_pp_loc_here(pp);
    odin3_bytes name = odin3_pp_ident_ahead(pp, 1);
    const odin3_pp_dir *dir = odin3_pp_dir_lookup(name);
    odin3_pp_dir_kind kind = dir == NULL ? ODIN3_PP_DIR_NONE : dir->kind;
    if (pp->skipping || kind == ODIN3_PP_DIR_COND) {
        if (kind == ODIN3_PP_DIR_COND && !top_is_file(pp)) {
            return odin3_pp_error(pp, at, "syntax: directive inside macro text");
        }
        odin3_pp_advance(pp, 1 + (uint32_t)name.len);
        col->sep = true;
        return kind == ODIN3_PP_DIR_COND ? odin3_pp_cond_directive(pp, dir->cond, at) : ODIN3_OK;
    }
    if (kind == ODIN3_PP_DIR_INCLUDE) {
        return use_error(pp, col->use, "unterminated arguments of ", "");
    }
    if (kind == ODIN3_PP_DIR_DEFINE || kind == ODIN3_PP_DIR_UNDEF) {
        return odin3_pp_error(pp, at, "syntax: directive inside macro text");
    }
    return take(col, 1);
}

/* Starts the next actual at a top-level ','. */
static odin3_status next_actual(collect *col) {
    odin3_pp *pp = col->pp;
    odin3_pp_advance(pp, 1);
    col->nactuals++;
    if (col->nactuals > col->keep) {
        return ODIN3_OK;
    }
    uint32_t *start = odin3_vec_push(&pp->arg_starts);
    if (start == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *start = (uint32_t)pp->args.len;
    col->sep = false;
    return ODIN3_OK;
}

/* Plain bytes up to the next byte collection treats specially (at least one). */
static odin3_status take_plain(collect *col) {
    return take(col, odin3_pp_plain_length(col->pp, true));
}

/* A bracket or separator; *done after the closing ')'. */
static odin3_status collect_bracket(collect *col, int ch, bool *done) {
    if (col->pp->skipping) {
        odin3_pp_advance(col->pp, 1);
        return ODIN3_OK;
    }
    if (col->depth == 0 && ch == ')') {
        odin3_pp_advance(col->pp, 1);
        *done = true;
        return ODIN3_OK;
    }
    if (col->depth == 0 && ch == ',') {
        return next_actual(col);
    }
    if (ch == '(' || ch == '[' || ch == '{') {
        col->depth++;
    } else if (col->depth > 0 && ch != ',') {
        col->depth--;
    }
    return take(col, 1);
}

/* One step of the collection; *done after the closing ')'. */
static odin3_status collect_step(collect *col, bool *done) {
    odin3_pp *pp = col->pp;
    int ch = odin3_pp_peek(pp, 0);
    int next = odin3_pp_peek(pp, 1);
    switch (ch) {
    case -1:
        if (top_is_file(pp)) {
            return use_error(pp, col->use, "unterminated arguments of ", "");
        }
        odin3_pp_pop_frame(pp);
        return ODIN3_OK;
    case '"':
        return take(col, odin3_pp_string_length(pp));
    case '\\':
        return take(col, odin3_pp_escaped_length(pp));
    case '`':
        return collect_backtick(col);
    case '/':
        return next == '/' || next == '*' ? take_comment(col) : take(col, 1);
    default:
        return strchr("()[]{},", ch) != NULL ? collect_bracket(col, ch, done) : take_plain(col);
    }
}

/* Skips blanks (newlines included, across exhausted frames) to the '(' and consumes it. */
static odin3_status find_paren(odin3_pp *pp, const odin3_pp_use *use) {
    for (int ch = odin3_pp_peek(pp, 0); ch != '('; ch = odin3_pp_peek(pp, 0)) {
        if (ch == -1 && !top_is_file(pp)) {
            odin3_pp_pop_frame(pp);
        } else if (ch >= 0 && odin3_pp_is_blank(ch)) {
            odin3_pp_advance(pp, 1);
        } else {
            return use_error(pp, use, "", " takes arguments");
        }
    }
    odin3_pp_advance(pp, 1);
    return ODIN3_OK;
}

/* The '(' and the actuals up to the matching ')'; *end = one past the ')'. */
static odin3_status collect_actuals(collect *col, odin3_loc *end) {
    odin3_pp *pp = col->pp;
    odin3_vec_clear(&pp->args);
    odin3_vec_clear(&pp->arg_starts);
    odin3_status st = find_paren(pp, col->use);
    uint32_t *first = st == ODIN3_OK ? odin3_vec_push(&pp->arg_starts) : NULL;
    if (st == ODIN3_OK && first == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *first = 0;
    col->nactuals = 1;
    bool done = false;
    while (st == ODIN3_OK && !done) {
        st = collect_step(col, &done);
    }
    *end = consumed_end(pp);
    return st;
}

/* --- counting the actuals ------------------------------------------------------------------ */

static bool trims(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* The runs [first, last) of recorded actual index in pp->args. */
typedef struct actual_span {
    uint32_t first;
    uint32_t last;
} actual_span;

static actual_span span_of(const odin3_pp *pp, uint32_t index) {
    uint32_t first = *(const uint32_t *)odin3_vec_cat(&pp->arg_starts, index);
    uint32_t last = index + 1 < pp->arg_starts.len
                        ? *(const uint32_t *)odin3_vec_cat(&pp->arg_starts, index + 1)
                        : (uint32_t)pp->args.len;
    return (actual_span){first, last};
}

/* Trims the actual's leading and trailing blanks (space, tab, CR, LF) in place. */
static void trim(odin3_pp *pp, actual_span span) {
    for (uint32_t k = span.first; k < span.last; k++) {
        odin3_pp_textrun *run = odin3_vec_at(&pp->args, k);
        while (run->len > 0 && trims(run->bytes[0])) {
            run->bytes++;
            run->len--;
            run->loc = odin3_pp_loc_add(run->loc, 1);
        }
        if (run->len > 0) {
            break;
        }
    }
    for (uint32_t k = span.last; k > span.first; k--) {
        odin3_pp_textrun *run = odin3_vec_at(&pp->args, k - 1);
        while (run->len > 0 && trims(run->bytes[run->len - 1])) {
            run->len--;
        }
        if (run->len > 0) {
            break;
        }
    }
}

static bool is_empty(const odin3_pp *pp, actual_span span) {
    for (uint32_t k = span.first; k < span.last; k++) {
        if (((const odin3_pp_textrun *)odin3_vec_cat(&pp->args, k))->len > 0) {
            return false;
        }
    }
    return true;
}

/* Trims the recorded actuals and returns their count (PP-5: an empty formal list's "()" has
 * none), then checks it against the formals and their defaults. */
static odin3_status count_actuals(const collect *col, const odin3_pp_macro *mac, uint32_t *count) {
    odin3_pp *pp = col->pp;
    for (uint32_t i = 0; i < pp->arg_starts.len; i++) {
        trim(pp, span_of(pp, i));
    }
    uint32_t got = col->nactuals;
    if (mac->nformals == 0 && got == 1 && is_empty(pp, span_of(pp, 0))) {
        got = 0;
    }
    bool missing = false;
    for (uint32_t j = got; j < mac->nformals; j++) {
        missing = missing || !mac->formals[j].has_default;
    }
    *count = got;
    if (got <= mac->nformals && !missing) {
        return ODIN3_OK;
    }
    const odin3_pp_use *use = col->use;
    return odin3_pp_error(pp, use->at, "syntax: macro '%.*s' expects %u arguments, got %u",
                          (int)use->name.len, (const char *)use->name.ptr, mac->nformals, got);
}

/* --- PP-6: substitution -------------------------------------------------------------------- */

static odin3_status chain_error(odin3_pp *pp, odin3_loc at) {
    odin3_pp_cap cap = {"expansion chain", "ODIN3_PP_MAX_EXPANSION_CHAIN",
                        pp->limits.expansion_chain};
    return odin3_pp_cap_error(pp, at, cap);
}

/* One expansion being built in pp->scratch: the body with every formal replaced. */
typedef struct subst {
    odin3_pp *pp;
    const odin3_pp_use *use;
    const odin3_pp_macro *mac;
    odin3_loc base;    /* the EXPANSION buffer's offset 0 */
    uint32_t nactuals; /* actuals given */
    uint32_t from;     /* the body run not yet pushed starts here */
    bool file_body;    /* continuations (I2) and comments are on: not a <command line> body */
} subst;

static odin3_status room(const subst *sub) {
    odin3_pp *pp = sub->pp;
    if ((uint64_t)pp->runs.len + pp->scratch.len + 1 > pp->limits.live_runs) {
        return live_runs_error(pp, sub->use->at);
    }
    return ODIN3_OK;
}

static odin3_status push_run(subst *sub, odin3_pp_textrun run) {
    odin3_pp_textrun *slot = odin3_vec_push(&sub->pp->scratch);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = run;
    return ODIN3_OK;
}

/* Pushes the body bytes [from, to) as one run of the EXPANSION buffer. */
static odin3_status flush(subst *sub, uint32_t to) {
    uint32_t from = sub->from;
    sub->from = to;
    if (to <= from) {
        return ODIN3_OK;
    }
    odin3_status st = room(sub);
    if (st != ODIN3_OK) {
        return st;
    }
    return push_run(sub, (odin3_pp_textrun){sub->mac->body + from, to - from,
                                            odin3_pp_loc_add(sub->base, from)});
}

/* One run of a substituted actual or default: a MACRO_ARG buffer for a spelled run, none for
 * loc 0. A default's buffer is macro text: its chain and ancestry run through the expansion
 * (from_default, PP-7). */
static odin3_status push_arg_run(subst *sub, odin3_pp_textrun run, odin3_range formal,
                                 bool from_default) {
    odin3_pp *pp = sub->pp;
    odin3_status st = run.len == 0 ? ODIN3_OK : room(sub);
    if (st != ODIN3_OK || run.len == 0 || run.loc.v == 0) {
        return st != ODIN3_OK || run.len == 0 ? st : push_run(sub, run);
    }
    if (1 + odin3_pp_chain_of(pp, from_default ? formal.loc : run.loc) >
        pp->limits.expansion_chain) {
        return chain_error(pp, sub->use->at);
    }
    odin3_pp_bufspec spec = {.kind = ODIN3_SRCBUF_MACRO_ARG,
                             .from_default = from_default,
                             .expansion = {ODIN3_SRCBUF_MACRO_ARG,
                                           sub->mac->name,
                                           formal.loc.v,
                                           formal.end.v,
                                           run.loc.v,
                                           run.len,
                                           {0, 0}}};
    odin3_srcbuf_id buf = {0};
    st = odin3_pp_add_buffer(pp, &spec, sub->use->at, &buf);
    if (st != ODIN3_OK) {
        return st;
    }
    return push_run(sub, (odin3_pp_textrun){run.bytes, run.len, odin3_srcman_loc(pp->sm, buf, 0)});
}

/* Formal index's occurrence at formal: the actual's runs as collected, or the default's. */
static odin3_status substitute(subst *sub, uint32_t index, odin3_range formal) {
    odin3_pp *pp = sub->pp;
    if (index < sub->nactuals && !is_empty(pp, span_of(pp, index))) {
        actual_span span = span_of(pp, index);
        odin3_status st = ODIN3_OK;
        for (uint32_t k = span.first; k < span.last && st == ODIN3_OK; k++) {
            st = push_arg_run(sub, *(const odin3_pp_textrun *)odin3_vec_cat(&pp->args, k), formal,
                              false);
        }
        return st;
    }
    const odin3_pp_formal *spec = &sub->mac->formals[index];
    if (!spec->has_default) {
        return ODIN3_OK; /* an empty actual without a default: empty text (I1) */
    }
    return push_arg_run(sub, (odin3_pp_textrun){spec->text, spec->len, spec->def}, formal, true);
}

/* Index + 1 of the formal named by body bytes [start, end), 0 for none. */
static uint32_t formal_index(const subst *sub, uint32_t start, uint32_t end) {
    uint32_t name = 0;
    odin3_bytes ident = {sub->mac->body + start, end - start};
    if (!odin3_strtab_find(sub->pp->strtab, ident, &name)) {
        return 0;
    }
    for (uint32_t j = 0; j < sub->mac->nformals; j++) {
        if (sub->mac->formals[j].name == name) {
            return j + 1;
        }
    }
    return 0;
}

/* The end of the body token at start outside strings and comments: an escaped identifier, a
 * backtick and its name, a run of identifier characters, or one byte; *formal = index + 1 of
 * the formal it names (an IDENT not after a backtick), else 0. */
static uint32_t token_end(const subst *sub, uint32_t start, uint32_t *formal) {
    const uint8_t *body = sub->mac->body;
    uint32_t len = sub->mac->len;
    uint32_t end = start + 1;
    *formal = 0;
    if (body[start] == '\\' && end < len && !odin3_pp_is_blank(body[end])) {
        while (end < len && !odin3_pp_is_blank(body[end])) {
            end++;
        }
    } else if (body[start] == '`' || odin3_pp_is_ident_char(body[start])) {
        while (end < len && odin3_pp_is_ident_char(body[end])) {
            end++;
        }
        *formal = odin3_pp_is_ident_start(body[start]) ? formal_index(sub, start, end) : 0;
    }
    return end;
}

/* The length of a \-newline continuation at body byte pos (\ LF or \ CR LF), 0 when none. */
static uint32_t continuation(const subst *sub, uint32_t pos) {
    const uint8_t *body = sub->mac->body;
    uint32_t len = sub->mac->len;
    if (!sub->file_body || body[pos] != '\\' || pos + 1 >= len) {
        return 0;
    }
    if (body[pos + 1] == '\n') {
        return LF_CONT;
    }
    return body[pos + 1] == '\r' && pos + 2 < len && body[pos + 2] == '\n' ? CRLF_CONT : 0;
}

/* A block comment in the body: one loc-0 space (PP-6). */
static odin3_status body_comment(subst *sub, uint32_t *pos) {
    const uint8_t *body = sub->mac->body;
    uint32_t len = sub->mac->len;
    uint32_t i = *pos;
    odin3_status st = flush(sub, i);
    if (st == ODIN3_OK) {
        st = room(sub);
    }
    if (st == ODIN3_OK) {
        st = push_run(sub, (odin3_pp_textrun){&k_space, 1, {0}});
    }
    uint32_t end = i + 2; /* the definition scanner saw the comment closed */
    while (end + 1 < len && !(body[end] == '*' && body[end + 1] == '/')) {
        end++;
    }
    *pos = end + 1 < len ? end + 2 : len;
    sub->from = *pos;
    return st;
}

/* One step over the body at *pos. */
static odin3_status body_step(subst *sub, uint32_t *pos, bool *in_string) {
    const uint8_t *body = sub->mac->body;
    uint32_t i = *pos;
    if (continuation(sub, i) != 0) {
        odin3_status st = flush(sub, i);
        sub->from = i + 1; /* only the backslash is dropped (I2) */
        *pos = i + 1;
        return st;
    }
    if (*in_string || body[i] == '"') {
        bool escape = *in_string && body[i] == '\\' && i + 1 < sub->mac->len;
        *in_string = body[i] == '"' ? !*in_string : *in_string;
        *pos = i + (escape ? 2 : 1);
        return ODIN3_OK;
    }
    if (!sub->file_body || body[i] != '/' || i + 1 >= sub->mac->len || body[i + 1] != '*') {
        uint32_t formal = 0;
        uint32_t end = token_end(sub, i, &formal);
        *pos = end;
        if (formal == 0) {
            return ODIN3_OK;
        }
        odin3_status st = flush(sub, i);
        sub->from = end;
        odin3_range range = {odin3_pp_loc_add(sub->base, i), odin3_pp_loc_add(sub->base, end)};
        return st != ODIN3_OK ? st : substitute(sub, formal - 1, range);
    }
    return body_comment(sub, pos);
}

/* Builds the expansion's run list in pp->scratch. */
static odin3_status substitute_body(subst *sub) {
    odin3_vec_clear(&sub->pp->scratch);
    bool in_string = false;
    uint32_t i = 0;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && i < sub->mac->len) {
        st = body_step(sub, &i, &in_string);
    }
    return st == ODIN3_OK ? flush(sub, sub->mac->len) : st;
}

/* --- PP-7: recursion on the spelling ancestry ---------------------------------------------- */

/* True when name was expanded on at's spelling ancestry: in a MACRO_ARG one spelling step (to
 * parent for a default's, which is macro text), in an EXPANSION record its name and go to
 * parent, stop at a FILE (bounded by the chain cap). */
static bool recursive(const odin3_pp *pp, odin3_loc at, uint32_t name) {
    odin3_loc loc = at;
    for (uint32_t steps = 0; steps <= pp->limits.expansion_chain; steps++) {
        const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(pp->sm, loc);
        if (rec == NULL || rec->kind == ODIN3_SRCBUF_FILE) {
            return false;
        }
        bool expansion = rec->kind == ODIN3_SRCBUF_EXPANSION;
        if (expansion && rec->name == name) {
            return true;
        }
        const odin3_pp_bufrec *own = odin3_pp_bufrec_of(pp, loc);
        if (expansion || (own != NULL && own->from_default)) {
            loc.v = rec->parent;
        } else {
            loc = odin3_srcman_spelling(pp->sm, loc);
        }
    }
    return false;
}

/* --- the use ------------------------------------------------------------------------------- */

/* The EXPANSION buffer, the substituted run list and the new frame for a checked use. */
static odin3_status expand(subst *sub, odin3_loc end) {
    odin3_pp *pp = sub->pp;
    const odin3_pp_use *use = sub->use;
    if (1 + odin3_pp_chain_of(pp, use->at) > pp->limits.expansion_chain) {
        return chain_error(pp, use->at);
    }
    pop_exhausted(pp);
    const odin3_pp_macro *mac = sub->mac;
    odin3_pp_bufspec spec = {
        .kind = ODIN3_SRCBUF_EXPANSION,
        .expansion = {
            ODIN3_SRCBUF_EXPANSION, mac->name, use->at.v, end.v, mac->def.v, mac->len, {0, 0}}};
    odin3_srcbuf_id buf = {0};
    odin3_status st = odin3_pp_add_buffer(pp, &spec, use->at, &buf);
    if (st != ODIN3_OK) {
        return st;
    }
    sub->base = odin3_srcman_loc(pp->sm, buf, 0);
    sub->file_body = odin3_srcman_buffer_of(pp->sm, mac->def).v != pp->cmdline_buf.v;
    st = substitute_body(sub);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pp_frame frame = {.kind = ODIN3_PP_FRAME_EXPANSION,
                            .nruns = (uint32_t)pp->scratch.len,
                            .buffer = buf,
                            .macro = use->macro - 1,
                            .use_loc = use->at};
    return odin3_pp_push_frame(pp, &frame, pp->scratch.data, use->at);
}

odin3_status odin3_pp_expand_use(odin3_pp *pp, const odin3_pp_use *use) {
    if (use->macro == 0) {
        return use_error(pp, use, "undefined ", "");
    }
    odin3_pp_macro mac = *(const odin3_pp_macro *)odin3_vec_cat(&pp->macros, use->macro - 1);
    if (recursive(pp, use->at, mac.name)) {
        return use_error(pp, use, "recursive ", "");
    }
    subst sub = {.pp = pp, .use = use, .mac = &mac};
    odin3_loc end = consumed_end(pp);
    if (mac.has_formals) {
        bool gap = pp->gap;
        collect col = {pp, use, mac.nformals > 0 ? mac.nformals : 1, 0, 0, false};
        odin3_status st = collect_actuals(&col, &end);
        pp->gap = gap;
        if (st == ODIN3_OK) {
            st = count_actuals(&col, &mac, &sub.nactuals);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return expand(&sub, end);
}
