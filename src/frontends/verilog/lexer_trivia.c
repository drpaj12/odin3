/*
 * lexer_trivia.c — what the Verilog lexer records beside the tokens (2C spec §2.5, §2.6, §2.9):
 * comments and metacomments into the store's side table and the pending list, the pass-through
 * directives with their arguments, and the run's `begin_keywords stack.
 */
#include "ast/ast.h"
#include "ast/srcfmt.h"
#include "ast/srcman.h"
#include "frontends/verilog/lexer_internal.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"
#include "verilog_parser.h" /* generated (build tree): TOK_* */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { VL_DELIMITER = 2 }; /* the bytes of //, slash-star and star-slash */

static bool is_blank(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

static bool is_word_byte(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '$';
}

static bool bytes_eq(odin3_bytes bytes, const char *word) {
    size_t len = strlen(word);
    return bytes.len == len && memcmp(bytes.ptr, word, len) == 0;
}

/* The next blank-separated word of text from *pos ({NULL, 0} at the end). */
static odin3_bytes next_word(odin3_bytes text, size_t *pos) {
    const char *bytes = text.ptr;
    size_t i = *pos;
    while (i < text.len && is_blank(bytes[i])) {
        i++;
    }
    size_t start = i;
    while (i < text.len && !is_blank(bytes[i])) {
        i++;
    }
    *pos = i;
    return start < i ? (odin3_bytes){bytes + start, i - start} : (odin3_bytes){NULL, 0};
}

/* --- metacomments (§2.6) ------------------------------------------------------------------- */

/*
 * A comment's text starting, after blanks, with synopsys or synthesis as a whole word is a
 * metacomment (*flagged); its keys are the ODIN3_VL_META_* bits when the words after the prefix
 * are a non-empty subset of {full_case, parallel_case}, else 0.
 */
/* The length of word's metacomment prefix (synopsys or synthesis, as a whole word), else 0. */
static size_t meta_prefix(odin3_bytes word) {
    static const char *const prefixes[] = {"synopsys", "synthesis"};
    const char *bytes = word.ptr;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t len = strlen(prefixes[i]);
        if (word.len >= len && memcmp(bytes, prefixes[i], len) == 0 &&
            (word.len == len || !is_word_byte(bytes[len]))) {
            return len;
        }
    }
    return 0;
}

static uint8_t meta_keys(odin3_bytes text, bool *flagged) {
    size_t pos = 0;
    odin3_bytes prefix = next_word(text, &pos);
    const char *bytes = prefix.ptr;
    size_t len = meta_prefix(prefix);
    *flagged = len > 0;
    if (!*flagged) {
        return 0;
    }
    pos = (size_t)(bytes + len - (const char *)text.ptr);
    uint8_t keys = 0;
    for (odin3_bytes word = next_word(text, &pos); word.len > 0; word = next_word(text, &pos)) {
        if (bytes_eq(word, "full_case")) {
            keys |= ODIN3_VL_META_FULL_CASE;
        } else if (bytes_eq(word, "parallel_case")) {
            keys |= ODIN3_VL_META_PARALLEL_CASE;
        } else {
            return 0;
        }
    }
    return keys;
}

/* --- comments (§2.6) ----------------------------------------------------------------------- */

/* Whether loc lies in a FILE buffer (comments elsewhere are not recorded). */
static bool in_file(const odin3_vl_state *st, odin3_loc loc) {
    odin3_srcbuf_info info;
    return odin3_srcman_buffer_info(st->cur.sm, odin3_srcman_buffer_of(st->cur.sm, loc), &info) &&
           info.kind == ODIN3_SRCBUF_FILE;
}

/* A comment over stream bytes [lo, hi) starting at loc first, its text [text_lo, text_hi). */
typedef struct vl_comment {
    uint32_t lo, hi, text_lo, text_hi;
    odin3_loc first;
    uint32_t kind;
} vl_comment;

/* Records the comment in the store (text cut at the cap) and a metacomment's keys in the
 * pending list; out of memory is fatal. */
static void record(odin3_vl_state *st, const vl_comment *cm) {
    if (!in_file(st, cm->first)) {
        return;
    }
    odin3_loc end = {vl_loc(st, cm->hi - 1).v + 1};
    odin3_bytes text = vl_bytes(st, cm->text_lo, cm->text_hi);
    if (text.len > st->run->limits.comment_bytes) {
        text.len = st->run->limits.comment_bytes;
    }
    bool flagged = false;
    uint8_t keys = meta_keys(text, &flagged);
    odin3_ast_comment_spec spec = {.loc = cm->first.v,
                                   .end = end.v,
                                   .kind = cm->kind,
                                   .flags = flagged ? ODIN3_AST_COMMENT_METACOMMENT : 0,
                                   .text_ptr = text.ptr,
                                   .text_len = (uint32_t)text.len};
    odin3_status status = odin3_ast_comment_add(st->ast, &spec);
    if (status != ODIN3_OK) {
        (void)vl_fatal(st, status);
        return;
    }
    if (keys != 0) {
        odin3_vl_meta *slot = odin3_vec_push(&st->meta);
        if (slot == NULL) {
            (void)vl_fatal(st, ODIN3_ERR_NO_MEMORY);
            return;
        }
        *slot = (odin3_vl_meta){cm->lo, cm->first, end, keys};
    }
}

void odin3_vl_line_comment(odin3_vl_state *st) {
    if (st->body.kind != 0) {
        return; /* part of the body's text */
    }
    const char *bytes = st->stream.text.ptr;
    uint32_t hi = st->offset;
    if (hi - st->tok_start > VL_DELIMITER && bytes[hi - 1] == '\r') {
        hi--; /* CRLF: the \r is not part of the comment */
    }
    vl_comment cm = {
        st->tok_start,         hi, st->tok_start + VL_DELIMITER, hi, vl_loc(st, st->tok_start),
        ODIN3_AST_COMMENT_LINE};
    record(st, &cm);
}

void odin3_vl_comment_open(odin3_vl_state *st) {
    if (st->body.kind != 0) {
        return;
    }
    st->token_start = st->tok_start;
    st->token_loc = vl_loc(st, st->tok_start);
}

void odin3_vl_comment_close(odin3_vl_state *st) {
    if (st->body.kind != 0) {
        return;
    }
    vl_comment cm = {st->token_start,           st->offset,    st->token_start + VL_DELIMITER,
                     st->offset - VL_DELIMITER, st->token_loc, ODIN3_AST_COMMENT_BLOCK};
    st->token_loc = (odin3_loc){0};
    record(st, &cm);
}

/* --- the keyword stack (§2.5) -------------------------------------------------------------- */

uint32_t vl_keyword_mask(const odin3_vl_run *run) {
    return run->kw_depth == 0 ? ODIN3_VL_KW_2005 : run->kw_mask[run->kw_depth - 1];
}

/* The version string of a set, with its quotes. */
static const char *version_of(uint32_t mask) {
    switch (mask) {
    case ODIN3_VL_KW_1995:
        return "\"1364-1995\"";
    case ODIN3_VL_KW_2001_NOCONFIG:
        return "\"1364-2001-noconfig\"";
    case ODIN3_VL_KW_2001:
        return "\"1364-2001\"";
    default:
        return "\"1364-2005\"";
    }
}

void vl_keywords_at_end(odin3_vl_state *st, odin3_loc at) {
    const odin3_vl_run *run = st->run;
    for (uint32_t i = 0; i < run->kw_depth; i++) {
        odin3_strbuf_clear(&st->scratch);
        if (odin3_srcman_format(st->cur.sm, run->kw_at[i], &st->scratch, ODIN3_SRCFMT_GIVEN) !=
            ODIN3_OK) {
            (void)vl_fatal(st, ODIN3_ERR_NO_MEMORY);
            return;
        }
        vl_warn(st, at, "`begin_keywords %s (%s) is still open at the end of the file",
                version_of(run->kw_mask[i]), st->scratch.data);
    }
}

static void begin_keywords(odin3_vl_state *st, odin3_loc at, odin3_bytes arg) {
    odin3_vl_run *run = st->run;
    uint32_t mask = 0;
    if (arg.len == 0) {
        vl_error(st, at, "`begin_keywords needs a version string");
    } else if (!odin3_vl_keyword_version(arg, &mask)) {
        vl_error(st, at, "unknown `begin_keywords version %.*s", (int)arg.len,
                 (const char *)arg.ptr);
    } else if (run->kw_depth >= run->limits.keyword_depth) {
        vl_error(st, at, "`begin_keywords nested deeper than %u", run->limits.keyword_depth);
    } else {
        run->kw_mask[run->kw_depth] = mask;
        run->kw_at[run->kw_depth] = at;
        run->kw_depth++;
    }
}

static void end_keywords(odin3_vl_state *st, odin3_loc at) {
    if (st->run->kw_depth == 0) {
        vl_warn(st, at, "`end_keywords without a matching `begin_keywords");
    } else {
        st->run->kw_depth--;
    }
}

/* --- directives (§2.9) --------------------------------------------------------------------- */

/* The pass-through directives, indexed by odin3_ast_directive_kind. */
static const char *const vl_directives[ODIN3_AST_DIRECTIVE_COUNT] = {
    "default_nettype", "timescale",    "celldefine",        "endcelldefine",       "resetall",
    "begin_keywords",  "end_keywords", "unconnected_drive", "nounconnected_drive", "pragma"};

/* The directive kind of word, ODIN3_AST_DIRECTIVE_COUNT for none. */
static uint32_t directive_find(odin3_bytes word) {
    uint32_t kind = 0;
    while (kind < ODIN3_AST_DIRECTIVE_COUNT && !bytes_eq(word, vl_directives[kind])) {
        kind++;
    }
    return kind;
}

/* The word after the backtick at stream offset off. */
static odin3_bytes directive_word(const odin3_vl_state *st, uint32_t off) {
    const char *bytes = st->stream.text.ptr;
    uint32_t end = off + 1;
    while (end < st->stream.text.len && is_word_byte(bytes[end])) {
        end++;
    }
    return vl_bytes(st, off + 1, end);
}

uint32_t odin3_vl_directive_kind(const odin3_vl_lexer *lx, uint32_t off) {
    const odin3_vl_state *st = vl_state_of(lx);
    if (off >= st->stream.text.len || ((const char *)st->stream.text.ptr)[off] != '`') {
        return ODIN3_AST_DIRECTIVE_COUNT;
    }
    return directive_find(directive_word(st, off));
}

/* One <n><unit> of a `timescale (n 1, 10 or 100; unit s ms us ns ps fs), blanks between. */
static bool timescale_part(odin3_bytes part) {
    static const char *const units[] = {"s", "ms", "us", "ns", "ps", "fs"};
    const char *bytes = part.ptr;
    size_t digits = 0;
    while (digits < part.len && bytes[digits] >= '0' && bytes[digits] <= '9') {
        digits++;
    }
    odin3_bytes magnitude = {bytes, digits};
    size_t unit = digits;
    while (unit < part.len && (bytes[unit] == ' ' || bytes[unit] == '\t')) {
        unit++;
    }
    odin3_bytes word = {bytes + unit, part.len - unit};
    bool unit_ok = false;
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        unit_ok = unit_ok || bytes_eq(word, units[i]);
    }
    return unit_ok &&
           (bytes_eq(magnitude, "1") || bytes_eq(magnitude, "10") || bytes_eq(magnitude, "100"));
}

/* The trimmed halves of `timescale's argument around its '/'. */
static bool timescale_ok(odin3_bytes arg) {
    const char *bytes = arg.ptr;
    const char *slash = arg.len > 0 ? memchr(bytes, '/', arg.len) : NULL;
    if (slash == NULL) {
        return false;
    }
    size_t left = (size_t)(slash - bytes);
    size_t right = left + 1;
    while (left > 0 && (bytes[left - 1] == ' ' || bytes[left - 1] == '\t')) {
        left--;
    }
    while (right < arg.len && (bytes[right] == ' ' || bytes[right] == '\t')) {
        right++;
    }
    return timescale_part((odin3_bytes){bytes, left}) &&
           timescale_part((odin3_bytes){bytes + right, arg.len - right});
}

/* The one-word argument of `default_nettype (§19.2: no supply0/1). */
static void default_nettype(odin3_vl_state *st, odin3_loc at, odin3_bytes arg) {
    static const char *const types[] = {"wire", "tri",   "tri0",   "tri1",  "wand", "triand",
                                        "wor",  "trior", "trireg", "uwire", "none"};
    bool ok = false;
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        ok = ok || bytes_eq(arg, types[i]);
    }
    if (arg.len == 0) {
        vl_error(st, at, "`default_nettype needs a net type");
    } else if (!ok) {
        vl_error(st, at,
                 "`default_nettype '%.*s' is not one of wire tri tri0 tri1 wand triand wor trior "
                 "trireg uwire none",
                 (int)arg.len, (const char *)arg.ptr);
    }
}

/* Checks a directive's argument and applies the keyword directives. */
static void directive_apply(odin3_vl_state *st, uint32_t kind, odin3_loc at, odin3_bytes arg) {
    if (kind == ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE) {
        default_nettype(st, at, arg);
    } else if (kind == ODIN3_AST_DIRECTIVE_UNCONNECTED_DRIVE) {
        if (arg.len == 0) {
            vl_error(st, at, "`unconnected_drive needs pull0 or pull1");
        } else if (!bytes_eq(arg, "pull0") && !bytes_eq(arg, "pull1")) {
            vl_error(st, at, "`unconnected_drive '%.*s' is not pull0 or pull1", (int)arg.len,
                     (const char *)arg.ptr);
        }
    } else if (kind == ODIN3_AST_DIRECTIVE_TIMESCALE && !timescale_ok(arg)) {
        vl_error(st, at,
                 "malformed `timescale (expected <1|10|100><s|ms|us|ns|ps|fs> / "
                 "<1|10|100><s|ms|us|ns|ps|fs>)");
    } else if (kind == ODIN3_AST_DIRECTIVE_BEGIN_KEYWORDS) {
        begin_keywords(st, at, arg);
    } else if (kind == ODIN3_AST_DIRECTIVE_END_KEYWORDS) {
        end_keywords(st, at);
    }
}

int odin3_vl_directive(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc) {
    if (vl_in_body(st)) {
        return VL_SKIP;
    }
    const char *bytes = st->stream.text.ptr;
    odin3_bytes word = directive_word(st, st->tok_start);
    uint32_t kind = directive_find(word);
    uint32_t lo = st->tok_start + 1 + (uint32_t)word.len;
    uint32_t hi = st->offset;
    while (hi > lo && is_blank(bytes[hi - 1])) {
        hi--;
    }
    while (lo < hi && is_blank(bytes[lo])) {
        lo++;
    }
    odin3_bytes arg = vl_bytes(st, lo, hi);
    odin3_loc at = vl_loc(st, st->tok_start);
    if (arg.len > st->run->limits.directive_bytes) {
        vl_error(st, at, "directive argument longer than %u bytes",
                 st->run->limits.directive_bytes);
        arg.len = st->run->limits.directive_bytes;
    }
    const char *nul = arg.len > 0 ? memchr(arg.ptr, 0, arg.len) : NULL;
    if (nul != NULL) { /* `pragma text may hold one; a NUL is never interned (2A §4.5) */
        vl_error(st, vl_loc(st, lo + (uint32_t)(nul - (const char *)arg.ptr)),
                 "unexpected byte 0x00");
        arg.len = (size_t)(nul - (const char *)arg.ptr);
    }
    directive_apply(st, kind, at, arg);
    val->text = arg;
    uint32_t end = hi > lo ? hi : st->tok_start + 1 + (uint32_t)word.len;
    return vl_token_at(st, loc, (vl_span){st->tok_start, end}, TOK_DIRECTIVE);
}

void odin3_vl_implicit_ports(odin3_vl_state *st) {
    if (vl_in_body(st)) {
        return;
    }
    vl_error(st, vl_loc(st, st->tok_start),
             "'.*' (implicit port connections) is SystemVerilog, not Verilog-2005");
}

void odin3_vl_unknown_directive(odin3_vl_state *st) {
    if (vl_in_body(st)) {
        return;
    }
    odin3_bytes text = vl_text(st);
    vl_error(st, vl_loc(st, st->tok_start), "unknown directive %.*s", (int)text.len,
             (const char *)text.ptr);
}
