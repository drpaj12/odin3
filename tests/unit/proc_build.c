/*
 * proc_build.c — pb_build: a subset of RTLIL read into a design, every process built as a working
 * form and encoded (proc_build.h). Test code: lines are tokenized, then dispatched on their first
 * word; a process is built with an explicit stack of open cases and switches.
 */
#include "proc_build.h"

#include "backends/rtlil/writer.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/pw.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PB_ARENA_CHUNK = 4096, PB_TEXT_MAX = 256, PB_DECIMAL = 10 };

typedef struct pb_tok {
    const char *at;
    uint32_t len;
} pb_tok;

/* An attribute read before the object it belongs to. */
typedef struct pb_attr {
    uint32_t key;
    odin3_value value;
} pb_attr;

/* A $tmp wire: its name and its unnamed nets. */
typedef struct pb_tmp {
    char name[PB_TEXT_MAX];
    odin3_vec nets; /* uint32_t net IDs */
} pb_tmp;

/* An open case or switch of the process being built. */
typedef struct pb_frame {
    bool is_switch;
    uint32_t idx;
} pb_frame;

/* A port connection of the cell being read. */
typedef struct pb_conn {
    uint32_t port;
    odin3_vec nets; /* uint32_t */
} pb_conn;

typedef enum pb_where { AT_TOP, IN_MODULE, IN_CELL, IN_PROCESS } pb_where;

typedef struct pb_state {
    pb_result *res;
    odin3_arena *arena; /* attribute and parameter payloads */
    uint32_t line;
    odin3_vec toks; /* pb_tok of the current line */
    odin3_prov_id prov;
    odin3_vec attrs; /* pb_attr, pending */
    pb_where where;
    odin3_module *mod;
    odin3_proc_mctx mctx;
    bool mctx_open;
    odin3_vec tmps; /* pb_tmp of the module */
    /* the process */
    odin3_pw pw;
    odin3_vec frames; /* pb_frame */
    bool in_syncs;
    /* the cell */
    odin3_celltype_id cell_type;
    uint32_t cell_name;
    odin3_value *cell_params;
    odin3_vec conns;      /* pb_conn */
    odin3_vec cell_attrs; /* pb_attr */
} pb_state;

/* --- errors and tokens --------------------------------------------------------------------- */

static odin3_status pb_fail(const pb_state *st, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static odin3_status pb_fail(const pb_state *st, const char *fmt, ...) {
    char msg[PB_TEXT_MAX];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_log(ODIN3_LOG_ERROR, "pb_build: line %u: %s", st->line, msg);
    return ODIN3_ERR_PARSE;
}

static pb_tok tok_at(const pb_state *st, size_t idx) {
    if (idx >= st->toks.len) {
        return (pb_tok){"", 0};
    }
    return *(const pb_tok *)odin3_vec_cat(&st->toks, idx);
}

static bool tok_is(pb_tok tok, const char *word) {
    return tok.len == strlen(word) && memcmp(tok.at, word, tok.len) == 0;
}

/* Copies a token into buf as a C string (truncated). */
static const char *tok_text(pb_tok tok, char *buf, size_t size) {
    size_t len = tok.len < size - 1 ? tok.len : size - 1;
    memcpy(buf, tok.at, len);
    buf[len] = '\0';
    return buf;
}

/* One past the closing quote of the string that starts at text[pos] (escapes skipped). */
static size_t quote_end(const char *text, size_t pos, size_t end) {
    size_t i = pos + 1;
    while (i < end && text[i] != '"') {
        i += text[i] == '\\' && i + 1 < end ? 2 : 1;
    }
    return i < end ? i + 1 : end;
}

/* One past the end of the token that starts at text[pos] (pos < end, not a blank). */
static size_t tok_end(const char *text, size_t pos, size_t end) {
    char first = text[pos];
    if (first == '"') {
        return quote_end(text, pos, end);
    }
    if (first == '[') {
        const char *close = memchr(text + pos, ']', end - pos);
        return close != NULL ? (size_t)(close - text) + 1 : end;
    }
    if (first == ',' || first == '{' || first == '}') {
        return pos + 1;
    }
    size_t i = pos;
    while (i < end && strchr(" \t\r,{}", text[i]) == NULL) {
        i++;
    }
    return i;
}

/* Splits text[0, end) into the current line's tokens; a '#' outside a token starts a comment. */
static odin3_status tokenize(pb_state *st, const char *text, size_t end) {
    odin3_vec_clear(&st->toks);
    size_t pos = 0;
    while (pos < end) {
        if (strchr(" \t\r", text[pos]) != NULL) {
            pos++;
            continue;
        }
        if (text[pos] == '#') {
            break;
        }
        size_t stop = tok_end(text, pos, end);
        pb_tok *tok = odin3_vec_push(&st->toks);
        if (tok == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *tok = (pb_tok){text + pos, (uint32_t)(stop - pos)};
        pos = stop;
    }
    return ODIN3_OK;
}

/* --- names and values ---------------------------------------------------------------------- */

static odin3_status intern_text(pb_state *st, const char *text, size_t len, uint32_t *out) {
    return odin3_design_intern(st->res->design, (odin3_bytes){text, len}, out);
}

/* An RTLIL identifier: \name is interned as name, $name as $name. */
static odin3_status intern_id(pb_state *st, pb_tok tok, uint32_t *out) {
    if (tok.len < 2 || (tok.at[0] != '\\' && tok.at[0] != '$')) {
        char buf[PB_TEXT_MAX];
        return pb_fail(st, "'%s' is not an identifier", tok_text(tok, buf, sizeof buf));
    }
    size_t skip = tok.at[0] == '\\' ? 1 : 0;
    return intern_text(st, tok.at + skip, tok.len - skip, out);
}

/* A decimal number (an optional '-'); false when tok is not one. */
static bool tok_int(pb_tok tok, int64_t *out) {
    char buf[PB_TEXT_MAX];
    tok_text(tok, buf, sizeof buf);
    char *stop = NULL;
    long long num = strtoll(buf, &stop, PB_DECIMAL);
    if (tok.len == 0 || stop == buf || *stop != '\0') {
        return false;
    }
    *out = num;
    return true;
}

/* The state of a constant character; false for another character. */
static bool const_char(char chr, odin3_pw_bit *out) {
    static const char k_chars[] = "01xz-";
    static const odin3_pw_bit k_bits[] = {ODIN3_PW_S0, ODIN3_PW_S1, ODIN3_PW_SX, ODIN3_PW_SZ,
                                          ODIN3_PW_SA};
    const char *at = chr != '\0' ? strchr(k_chars, chr) : NULL;
    if (at == NULL) {
        return false;
    }
    *out = k_bits[at - k_chars];
    return true;
}

/* N'bits (MSB first) appended LSB first to out as working-form bits. */
static odin3_status parse_const(pb_state *st, pb_tok tok, odin3_vec *out) {
    const char *quote = memchr(tok.at, '\'', tok.len);
    int64_t width = 0;
    pb_tok head = {tok.at, quote != NULL ? (uint32_t)(quote - tok.at) : 0};
    uint32_t n_chars = quote != NULL ? tok.len - head.len - 1 : 0;
    if (quote == NULL || !tok_int(head, &width) || width != (int64_t)n_chars) {
        char buf[PB_TEXT_MAX];
        return pb_fail(st, "bad constant '%s' (N'bits with exactly N bits)",
                       tok_text(tok, buf, sizeof buf));
    }
    for (uint32_t i = n_chars; i > 0; i--) {
        odin3_pw_bit bit = 0;
        uint32_t *slot = NULL;
        if (!const_char(quote[i], &bit)) {
            return pb_fail(st, "bad constant bit '%c'", quote[i]);
        }
        slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = bit;
    }
    return ODIN3_OK;
}

/* The character an escape letter stands for (\n, \t; any other letter is itself). */
static char escaped(char letter) {
    if (letter == 'n') {
        return '\n';
    }
    if (letter == 't') {
        return '\t';
    }
    return letter;
}

/* One character of a "..." token's text at tok.at[*pos] with RTLIL's escapes undone (\\, \",
 * \n, \t, \ooo), advancing *pos; the text ends before the closing quote. */
static char unescape(pb_tok tok, uint32_t *pos) {
    uint32_t text_end = tok.len - 1;
    char chr = tok.at[(*pos)++];
    if (chr != '\\' || *pos >= text_end) {
        return chr;
    }
    chr = tok.at[(*pos)++];
    if (chr < '0' || chr > '7') {
        return escaped(chr);
    }
    unsigned code = (unsigned)(chr - '0');
    for (int digits = 1;
         digits < 3 && *pos < text_end && tok.at[*pos] >= '0' && tok.at[*pos] <= '7'; digits++) {
        code = code * 8 + (unsigned)(tok.at[(*pos)++] - '0');
    }
    return (char)(unsigned char)code;
}

/* The text of a "..." token, escapes undone, into buf; its length. */
static size_t unquote(pb_tok tok, char *buf, size_t size) {
    size_t len = 0;
    uint32_t i = 1;
    while (i + 1 < tok.len && len + 1 < size) {
        buf[len++] = unescape(tok, &i);
    }
    buf[len] = '\0';
    return len;
}

/* An unsigned decimal at *text, which moves past it; false when there is none. */
static bool scan_uint(const char **text, unsigned *out) {
    char *stop = NULL;
    if (**text < '0' || **text > '9') {
        return false;
    }
    unsigned long num = strtoul(*text, &stop, PB_DECIMAL);
    if (stop == *text || num > UINT32_MAX) {
        return false;
    }
    *out = (unsigned)num;
    *text = stop;
    return true;
}

/* Character chr at *text, which moves past it; false when another character is there. */
static bool scan_char(const char **text, char chr) {
    if (**text != chr) {
        return false;
    }
    (*text)++;
    return true;
}

/* An attribute or parameter value: an int, N'bits (no '-') or a "string". */
static odin3_status parse_value(pb_state *st, pb_tok tok, odin3_value *out) {
    memset(out, 0, sizeof *out);
    if (tok.len > 0 && tok.at[0] == '"') {
        char buf[PB_TEXT_MAX];
        size_t len = unquote(tok, buf, sizeof buf);
        out->kind = ODIN3_VAL_STRING;
        return intern_text(st, buf, len, &out->str);
    }
    if (memchr(tok.at, '\'', tok.len) == NULL) {
        out->kind = ODIN3_VAL_INT;
        return tok_int(tok, &out->i) ? ODIN3_OK : pb_fail(st, "bad value");
    }
    odin3_vec bits;
    odin3_vec_init(&bits, sizeof(uint32_t));
    odin3_status res = parse_const(st, tok, &bits);
    uint8_t *bytes = res == ODIN3_OK ? odin3_arena_alloc(st->arena, bits.len + 1) : NULL;
    if (res == ODIN3_OK && bytes == NULL) {
        res = ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; res == ODIN3_OK && i < bits.len; i++) {
        uint32_t state = *(const uint32_t *)odin3_vec_cat(&bits, i) & ~ODIN3_PW_CONST;
        res = state <= (ODIN3_PW_SZ & ~ODIN3_PW_CONST) ? ODIN3_OK : pb_fail(st, "'-' in a value");
        bytes[i] = (uint8_t)state; /* odin3_bit: 0, 1, x, z in that order */
    }
    *out = (odin3_value){ODIN3_VAL_BITS, 0, bytes, (uint32_t)bits.len, 0, 0};
    odin3_vec_free(&bits);
    return res;
}

/* --- attributes and provenance ------------------------------------------------------------- */

static odin3_status line_attribute(pb_state *st) {
    pb_attr attr = {0, {0}};
    if (st->toks.len != 3) {
        return pb_fail(st, "attribute \\name value");
    }
    odin3_status res = intern_id(st, tok_at(st, 1), &attr.key);
    res = res == ODIN3_OK ? parse_value(st, tok_at(st, 2), &attr.value) : res;
    pb_attr *slot = res == ODIN3_OK ? odin3_vec_push(&st->attrs) : NULL;
    if (res == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = attr;
    }
    return res;
}

static bool attr_named(const pb_state *st, const pb_attr *attr, const char *name) {
    return strcmp(odin3_strtab_get(odin3_design_strtab(st->res->design), attr->key), name) == 0;
}

/* A source location "L.C" or "L.C-L2.C2" (the end defaults to the start). */
static bool scan_loc(const char *text, odin3_srcloc *loc) {
    unsigned nums[4] = {0, 0, 0, 0};
    bool ok = scan_uint(&text, &nums[0]) && scan_char(&text, '.') && scan_uint(&text, &nums[1]);
    if (ok && scan_char(&text, '-')) {
        ok = scan_uint(&text, &nums[2]) && scan_char(&text, '.') && scan_uint(&text, &nums[3]);
    } else {
        nums[2] = nums[0];
        nums[3] = nums[1];
    }
    *loc = (odin3_srcloc){loc->file, nums[0], nums[1], nums[2], nums[3], 0};
    return ok && *text == '\0';
}

/* A SOURCE record from a "file:L.C" (or "file:L.C-L2.C2") string. */
static odin3_status src_record(pb_state *st, const odin3_value *val, odin3_prov_id *out) {
    const char *text = val->kind == ODIN3_VAL_STRING
                           ? odin3_strtab_get(odin3_design_strtab(st->res->design), val->str)
                           : NULL;
    const char *colon = text != NULL ? strrchr(text, ':') : NULL;
    odin3_srcloc loc = {0, 0, 0, 0, 0, 0};
    if (colon == NULL || !scan_loc(colon + 1, &loc)) {
        return pb_fail(st, "\\src must be \"file:L.C\"");
    }
    odin3_status res = intern_text(st, text, (size_t)(colon - text), &loc.file);
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    return res == ODIN3_OK ? odin3_prov_source(&st->res->ctx, &origin, out) : res;
}

/* The record of the next object: from a pending \src, else dflt. */
static odin3_status prov_or(pb_state *st, odin3_prov_id dflt, odin3_prov_id *out) {
    *out = dflt;
    for (size_t i = 0; i < st->attrs.len; i++) {
        const pb_attr *attr = odin3_vec_cat(&st->attrs, i);
        if (attr_named(st, attr, "src")) {
            return src_record(st, &attr->value, out);
        }
    }
    return ODIN3_OK;
}

/* An IR object's record: the pending \src, else the build's own (no check rule 7 warning). */
static odin3_status take_prov(pb_state *st, odin3_prov_id *out) {
    return prov_or(st, st->prov, out);
}

/* A case's, switch's or action's record inside a body: the pending \src, else none (0). */
static odin3_status take_body_prov(pb_state *st, odin3_prov_id *out) {
    return prov_or(st, (odin3_prov_id){0}, out);
}

/* Sets the pending attributes (but \src) on obj. */
static odin3_status set_attrs(pb_state *st, odin3_objref obj) {
    odin3_status res = ODIN3_OK;
    for (size_t i = 0; res == ODIN3_OK && i < st->attrs.len; i++) {
        const pb_attr *attr = odin3_vec_cat(&st->attrs, i);
        if (!attr_named(st, attr, "src")) {
            res = odin3_attr_set(st->mod, obj, attr->key, &attr->value);
        }
    }
    return res;
}

/* set_attrs, then clears the pending attributes. */
static odin3_status take_attrs(pb_state *st, odin3_objref obj) {
    odin3_status res = set_attrs(st, obj);
    odin3_vec_clear(&st->attrs);
    return res;
}

/* --- signals ------------------------------------------------------------------------------- */

static pb_tmp *find_tmp(pb_state *st, pb_tok name) {
    char buf[PB_TEXT_MAX];
    tok_text(name, buf, sizeof buf);
    for (size_t i = 0; i < st->tmps.len; i++) {
        pb_tmp *tmp = odin3_vec_at(&st->tmps, i);
        if (strcmp(tmp->name, buf) == 0) {
            return tmp;
        }
    }
    return NULL;
}

static bool is_tmp_name(pb_tok tok) {
    return tok.len >= 4 && memcmp(tok.at, "$tmp", 4) == 0;
}

static odin3_status push_u32(odin3_vec *vec, uint32_t val) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = val;
    return ODIN3_OK;
}

/* The nets of a $tmp wire, appended to out. */
static odin3_status tmp_nets(pb_state *st, pb_tok name, odin3_vec *out) {
    pb_tmp *tmp = find_tmp(st, name);
    if (tmp == NULL) {
        char buf[PB_TEXT_MAX];
        return pb_fail(st, "unknown wire '%s'", tok_text(name, buf, sizeof buf));
    }
    odin3_status res = ODIN3_OK;
    for (size_t i = 0; res == ODIN3_OK && i < tmp->nets.len; i++) {
        res = push_u32(out, *(const uint32_t *)odin3_vec_cat(&tmp->nets, i));
    }
    return res;
}

/* The nets of every bit of a wire or $tmp wire, appended to out. */
static odin3_status name_nets(pb_state *st, pb_tok name, odin3_vec *out) {
    if (is_tmp_name(name)) {
        return tmp_nets(st, name, out);
    }
    uint32_t str = 0;
    odin3_status res = intern_id(st, name, &str);
    odin3_wire_id wire =
        res == ODIN3_OK ? odin3_module_find_wire(st->mod, str) : (odin3_wire_id){0};
    if (res == ODIN3_OK && !odin3_wire_valid(wire)) {
        char buf[PB_TEXT_MAX];
        return pb_fail(st, "unknown wire '%s'", tok_text(name, buf, sizeof buf));
    }
    for (uint32_t i = 0; res == ODIN3_OK && i < odin3_wire_width(st->mod, wire); i++) {
        res = push_u32(out, odin3_wire_net(st->mod, wire, i).v);
    }
    return res;
}

/* [k] or [hi:lo] of the nets of one name: keeps only those, in place. */
static odin3_status select_bits(pb_state *st, pb_tok sel, odin3_vec *nets, size_t base) {
    char buf[PB_TEXT_MAX];
    const char *text = tok_text(sel, buf, sizeof buf);
    unsigned high = 0;
    unsigned low = 0;
    bool ok = scan_char(&text, '[') && scan_uint(&text, &high);
    if (ok && scan_char(&text, ':')) {
        ok = scan_uint(&text, &low);
    } else {
        low = high;
    }
    ok = ok && scan_char(&text, ']') && *text == '\0';
    size_t width = nets->len - base;
    if (!ok || low > high || high >= width) {
        return pb_fail(st, "bad selection %s of a %zu-bit signal", buf, width);
    }
    uint32_t *data = (uint32_t *)nets->data + base;
    memmove(data, data + low, (size_t)(high - low + 1) * sizeof *data);
    while (nets->len > base + (high - low + 1)) {
        odin3_vec_pop(nets);
    }
    return ODIN3_OK;
}

/* One chunk at toks[*at]: a constant, or a name with an optional selection. */
static odin3_status parse_chunk(pb_state *st, size_t *at, odin3_vec *out) {
    pb_tok tok = tok_at(st, *at);
    if (tok.len == 0) {
        return pb_fail(st, "missing signal");
    }
    (*at)++;
    if (tok.at[0] >= '0' && tok.at[0] <= '9') {
        return parse_const(st, tok, out);
    }
    size_t base = out->len;
    odin3_status res = name_nets(st, tok, out);
    pb_tok sel = tok_at(st, *at);
    if (res == ODIN3_OK && sel.len > 0 && sel.at[0] == '[') {
        (*at)++;
        res = select_bits(st, sel, out, base);
    }
    return res;
}

/* Entries [first, end) of a vec. */
typedef struct pb_span {
    size_t first, end;
} pb_span;

/* Appends entries span of src to out. */
static odin3_status append_run(odin3_vec *out, const odin3_vec *src, pb_span span) {
    odin3_status res = ODIN3_OK;
    for (size_t i = span.first; res == ODIN3_OK && i < span.end; i++) {
        res = push_u32(out, *(const uint32_t *)odin3_vec_cat(src, i));
    }
    return res;
}

/* { c1 c2 ... }: the chunks are MSB first, so out gets them in reverse order. */
static odin3_status parse_concat(pb_state *st, size_t *at, odin3_vec *out) {
    odin3_vec bits;
    odin3_vec starts;
    odin3_vec_init(&bits, sizeof(uint32_t));
    odin3_vec_init(&starts, sizeof(size_t));
    odin3_status res = ODIN3_OK;
    (*at)++; /* { */
    while (res == ODIN3_OK && !tok_is(tok_at(st, *at), "}")) {
        size_t *start = odin3_vec_push(&starts);
        res = start != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (start != NULL) {
            *start = bits.len;
            res = tok_is(tok_at(st, *at), "{") ? pb_fail(st, "nested { } are not supported")
                                               : parse_chunk(st, at, &bits);
        }
    }
    (*at)++; /* } */
    for (size_t chunk = starts.len; res == ODIN3_OK && chunk > 0; chunk--) {
        size_t from = *(const size_t *)odin3_vec_cat(&starts, chunk - 1);
        size_t to = chunk < starts.len ? *(const size_t *)odin3_vec_cat(&starts, chunk) : bits.len;
        res = append_run(out, &bits, (pb_span){from, to});
    }
    odin3_vec_free(&bits);
    odin3_vec_free(&starts);
    return res;
}

/* A sigspec at toks[*at], appended LSB first to out (working-form bits: nets and constants). */
static odin3_status parse_sig(pb_state *st, size_t *at, odin3_vec *out) {
    return tok_is(tok_at(st, *at), "{") ? parse_concat(st, at, out) : parse_chunk(st, at, out);
}

/* The sigspec at toks[*at] as nets: constants become the shared constant nets ('-' refused). */
static odin3_status parse_nets(pb_state *st, size_t *at, odin3_vec *out) {
    size_t base = out->len;
    odin3_status res = parse_sig(st, at, out);
    for (size_t i = base; res == ODIN3_OK && i < out->len; i++) {
        uint32_t *bit = odin3_vec_at(out, i);
        if ((*bit & ODIN3_PW_CONST) == 0) {
            continue;
        }
        odin3_net_id net = {0};
        res = *bit == ODIN3_PW_SA ? pb_fail(st, "'-' outside a compare")
                                  : odin3_proc_const_net(&st->mctx, *bit, &net);
        *bit = net.v;
    }
    return res;
}

/* --- modules and wires --------------------------------------------------------------------- */

static void free_tmps(pb_state *st) {
    for (size_t i = 0; i < st->tmps.len; i++) {
        odin3_vec_free(&((pb_tmp *)odin3_vec_at(&st->tmps, i))->nets);
    }
    odin3_vec_clear(&st->tmps);
}

static odin3_status line_module(pb_state *st) {
    if (st->where != AT_TOP || st->toks.len != 2) {
        return pb_fail(st, "module \\name at the top level");
    }
    uint32_t name = 0;
    odin3_prov_id prov = {0};
    odin3_module_id id = {0};
    odin3_status res = intern_id(st, tok_at(st, 1), &name);
    res = res == ODIN3_OK ? take_prov(st, &prov) : res;
    res = res == ODIN3_OK ? odin3_module_create(st->res->design, name, prov, &id) : res;
    if (res != ODIN3_OK) {
        return res;
    }
    st->mod = odin3_module_get(st->res->design, id);
    st->res->module = st->mod;
    st->where = IN_MODULE;
    res = odin3_proc_mctx_init(&st->mctx, &st->res->ctx, st->mod);
    st->mctx_open = res == ODIN3_OK;
    return res == ODIN3_OK ? take_attrs(st, (odin3_objref){ODIN3_OBJ_MODULE, id.v}) : res;
}

/* The options of a wire line: width N, input|output|inout K, signed. */
typedef struct pb_wire_opts {
    uint32_t width;
    bool is_port, is_signed;
    odin3_dir dir;
} pb_wire_opts;

static odin3_status wire_option(pb_state *st, size_t *at, pb_wire_opts *opts) {
    static const char *const k_dirs[] = {"input", "output", "inout"};
    static const odin3_dir k_dir_of[] = {ODIN3_DIR_IN, ODIN3_DIR_OUT, ODIN3_DIR_INOUT};
    pb_tok word = tok_at(st, (*at)++);
    int64_t num = 0;
    if (tok_is(word, "signed")) {
        opts->is_signed = true;
        return ODIN3_OK;
    }
    if (!tok_int(tok_at(st, (*at)++), &num) || num < 1) {
        return pb_fail(st, "wire option without a positive number");
    }
    if (tok_is(word, "width")) {
        opts->width = (uint32_t)num;
        return ODIN3_OK;
    }
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; i++) {
        if (tok_is(word, k_dirs[i])) {
            opts->is_port = true;
            opts->dir = k_dir_of[i];
            return ODIN3_OK; /* ports are added in text order; K is not checked */
        }
    }
    return pb_fail(st, "unknown wire option");
}

/* A $tmp wire: width unnamed nets with no wire (point (s)); attributes go on each net. */
static odin3_status add_tmp(pb_state *st, pb_tok name, uint32_t width, odin3_prov_id prov) {
    pb_tmp *tmp = odin3_vec_push(&st->tmps);
    if (tmp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    tok_text(name, tmp->name, sizeof tmp->name);
    odin3_vec_init(&tmp->nets, sizeof(uint32_t));
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 0; res == ODIN3_OK && i < width; i++) {
        odin3_net_id net = {0};
        res = odin3_net_create(st->mod, 0, prov, &net);
        uint32_t *slot = res == ODIN3_OK ? odin3_vec_push(&tmp->nets) : NULL;
        res = res == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : res;
        if (slot != NULL) {
            *slot = net.v;
        }
        res = res == ODIN3_OK ? set_attrs(st, (odin3_objref){ODIN3_OBJ_NET, net.v}) : res;
    }
    odin3_vec_clear(&st->attrs);
    return res;
}

static odin3_status line_wire(pb_state *st) {
    pb_wire_opts opts = {1, false, false, ODIN3_DIR_IN};
    size_t at = 1;
    odin3_status res = st->where == IN_MODULE ? ODIN3_OK : pb_fail(st, "wire outside a module");
    while (res == ODIN3_OK && at + 1 < st->toks.len) {
        res = wire_option(st, &at, &opts);
    }
    if (res == ODIN3_OK && at + 1 != st->toks.len) {
        res = pb_fail(st, "wire [options] \\name");
    }
    pb_tok name = tok_at(st, at);
    odin3_prov_id prov = {0};
    uint32_t str = 0;
    res = res == ODIN3_OK ? take_prov(st, &prov) : res;
    if (res == ODIN3_OK && is_tmp_name(name)) {
        return add_tmp(st, name, opts.width, prov);
    }
    res = res == ODIN3_OK ? intern_id(st, name, &str) : res;
    odin3_wire_id wire = {0};
    if (res == ODIN3_OK && opts.is_port) {
        odin3_port_spec spec = {str,   opts.dir, opts.width, opts.width == 1, prov,
                                false, 0,        0,          opts.is_signed};
        odin3_node_id port = {0};
        res = odin3_module_add_port(st->mod, &spec, &port);
        wire = odin3_module_port_wire(st->mod, odin3_module_port_count(st->mod) - 1);
    } else if (res == ODIN3_OK) {
        odin3_wire_spec spec = {str, (int32_t)opts.width - 1, 0, opts.is_signed, prov};
        res = odin3_wire_create(st->mod, &spec, NULL, &wire);
    }
    return res == ODIN3_OK ? take_attrs(st, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}) : res;
}

/* --- cells --------------------------------------------------------------------------------- */

static const odin3_celltype_def *cell_def(const pb_state *st) {
    return odin3_celltype_get(st->res->design, st->cell_type);
}

static void free_cell(pb_state *st) {
    for (size_t i = 0; i < st->conns.len; i++) {
        odin3_vec_free(&((pb_conn *)odin3_vec_at(&st->conns, i))->nets);
    }
    odin3_vec_clear(&st->conns);
    odin3_vec_clear(&st->cell_attrs);
    free(st->cell_params);
    st->cell_params = NULL;
}

/* The cell type of a `cell` line: $type as is, \module by its module's name. */
static odin3_status find_type(pb_state *st, pb_tok tok, odin3_celltype_id *out) {
    uint32_t str = 0;
    odin3_status res = intern_id(st, tok, &str);
    if (res == ODIN3_OK && !odin3_celltype_find(st->res->design, str, out)) {
        char buf[PB_TEXT_MAX];
        res = pb_fail(st, "unknown cell type '%s'", tok_text(tok, buf, sizeof buf));
    }
    return res;
}

static odin3_status line_cell(pb_state *st) {
    if (st->where != IN_MODULE || st->toks.len != 3) {
        return pb_fail(st, "cell <type> <name> inside a module");
    }
    odin3_status res = find_type(st, tok_at(st, 1), &st->cell_type);
    res = res == ODIN3_OK ? intern_id(st, tok_at(st, 2), &st->cell_name) : res;
    const odin3_celltype_def *def = res == ODIN3_OK ? cell_def(st) : NULL;
    if (def == NULL) {
        return res;
    }
    st->cell_params = calloc(def->n_params + 1, sizeof *st->cell_params);
    if (st->cell_params == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        st->cell_params[i] = def->params[i].dflt;
    }
    odin3_vec swap = st->cell_attrs; /* the pending attributes are the cell's */
    st->cell_attrs = st->attrs;
    st->attrs = swap;
    st->where = IN_CELL;
    return ODIN3_OK;
}

/* The index of the parameter or port of the cell named by tok (\name); count when none is. */
static uint32_t def_index(pb_tok tok, const char *const *names, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (tok.len > 1 && tok.at[0] == '\\' && tok.len - 1 == strlen(names[i]) &&
            memcmp(tok.at + 1, names[i], tok.len - 1) == 0) {
            return i;
        }
    }
    return count;
}

static odin3_status line_parameter(pb_state *st) {
    const odin3_celltype_def *def = st->where == IN_CELL ? cell_def(st) : NULL;
    if (def == NULL || st->toks.len != 3) {
        return pb_fail(st, "parameter \\P value inside a cell");
    }
    const char *names[PB_TEXT_MAX];
    uint32_t count = def->n_params < PB_TEXT_MAX ? def->n_params : PB_TEXT_MAX;
    for (uint32_t i = 0; i < count; i++) {
        names[i] = def->params[i].name;
    }
    uint32_t idx = def_index(tok_at(st, 1), names, count);
    odin3_value val = {0};
    odin3_status res = idx < count ? parse_value(st, tok_at(st, 2), &val)
                                   : pb_fail(st, "the cell type has no such parameter");
    if (res == ODIN3_OK && val.kind != def->params[idx].kind) {
        return pb_fail(st, "parameter \\%s has the wrong kind", def->params[idx].name);
    }
    if (res == ODIN3_OK) {
        st->cell_params[idx] = val;
    }
    return res;
}

static odin3_status cell_connect(pb_state *st) {
    const odin3_celltype_def *def = cell_def(st);
    const char *names[PB_TEXT_MAX];
    uint32_t count = def->n_ports < PB_TEXT_MAX ? def->n_ports : PB_TEXT_MAX;
    for (uint32_t i = 0; i < count; i++) {
        names[i] = def->ports[i].name;
    }
    uint32_t port = def_index(tok_at(st, 1), names, count);
    if (port == count) {
        return pb_fail(st, "the cell type has no such port");
    }
    pb_conn *conn = odin3_vec_push(&st->conns);
    if (conn == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    conn->port = port;
    odin3_vec_init(&conn->nets, sizeof(uint32_t));
    size_t at = 2;
    odin3_status res = parse_nets(st, &at, &conn->nets);
    return res == ODIN3_OK && at != st->toks.len ? pb_fail(st, "connect \\PORT <sig>") : res;
}

/* Fills nets (one entry per pin, none when unconnected) from the cell's connections. */
static odin3_status cell_port_nets(pb_state *st, uint32_t port, uint32_t width, uint32_t *nets) {
    for (size_t i = 0; i < st->conns.len; i++) {
        const pb_conn *conn = odin3_vec_cat(&st->conns, i);
        if (conn->port != port) {
            continue;
        }
        if (conn->nets.len != width) {
            return pb_fail(st, "port \\%s is %u bits, connected to %zu",
                           cell_def(st)->ports[port].name, width, conn->nets.len);
        }
        memcpy(nets, conn->nets.data, width * sizeof *nets);
    }
    return ODIN3_OK;
}

static odin3_status cell_end(pb_state *st) {
    const odin3_celltype_def *def = cell_def(st);
    odin3_netvec *ports = calloc(def->n_ports + 1, sizeof *ports);
    uint32_t **nets = (uint32_t **)calloc(def->n_ports + 1, sizeof *nets);
    odin3_status res = ports != NULL && nets != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t i = 0; res == ODIN3_OK && i < def->n_ports; i++) {
        uint32_t width =
            odin3_celltype_port_width(st->res->design, st->cell_type, st->cell_params, i);
        nets[i] = calloc(width + 1, sizeof *nets[i]);
        res = nets[i] != NULL ? cell_port_nets(st, i, width, nets[i]) : ODIN3_ERR_NO_MEMORY;
        ports[i] = (odin3_netvec){(const odin3_net_id *)nets[i], width};
    }
    odin3_vec swap = st->attrs; /* the cell's attributes are pending again */
    st->attrs = st->cell_attrs;
    st->cell_attrs = swap;
    odin3_prov_id prov = {0};
    res = res == ODIN3_OK ? take_prov(st, &prov) : res;
    odin3_node_spec spec = {st->cell_type, st->cell_name, prov, st->cell_params, def->n_params};
    odin3_node_id node = {0};
    res = res == ODIN3_OK ? odin3_node_create_connected(st->mod, &spec, ports, &node) : res;
    res = res == ODIN3_OK ? take_attrs(st, (odin3_objref){ODIN3_OBJ_NODE, node.v}) : res;
    for (uint32_t i = 0; nets != NULL && i < def->n_ports; i++) {
        free(nets[i]);
    }
    free((void *)nets);
    free(ports);
    free_cell(st);
    st->where = IN_MODULE;
    return res;
}

/* --- module connections -------------------------------------------------------------------- */

/* Every later mention of net drop (in a and b from i on, and in the $tmp wires) becomes keep. */
static void rename_net(pb_state *st, odin3_vec *pair[2], size_t from, odin3_net_pair merged) {
    for (size_t side = 0; side < 2; side++) {
        for (size_t i = from; i < pair[side]->len; i++) {
            uint32_t *net = odin3_vec_at(pair[side], i);
            *net = *net == merged.drop.v ? merged.keep.v : *net;
        }
    }
    for (size_t tmp_idx = 0; tmp_idx < st->tmps.len; tmp_idx++) {
        pb_tmp *tmp = odin3_vec_at(&st->tmps, tmp_idx);
        for (size_t i = 0; i < tmp->nets.len; i++) {
            uint32_t *net = odin3_vec_at(&tmp->nets, i);
            *net = *net == merged.drop.v ? merged.keep.v : *net;
        }
    }
}

/* connect a b: each net of b merged into a's (the constant net kept when b is one). */
static odin3_status module_connect(pb_state *st) {
    odin3_vec lhs;
    odin3_vec rhs;
    odin3_vec_init(&lhs, sizeof(uint32_t));
    odin3_vec_init(&rhs, sizeof(uint32_t));
    size_t at = 1;
    odin3_status res = parse_nets(st, &at, &lhs);
    res = res == ODIN3_OK ? parse_nets(st, &at, &rhs) : res;
    if (res == ODIN3_OK && (at != st->toks.len || lhs.len != rhs.len)) {
        res = pb_fail(st, "connect <sig> <sig> of equal widths");
    }
    odin3_vec *pair[2] = {&lhs, &rhs};
    for (size_t i = 0; res == ODIN3_OK && i < lhs.len; i++) {
        odin3_net_id net_a = {*(const uint32_t *)odin3_vec_cat(&lhs, i)};
        odin3_net_id net_b = {*(const uint32_t *)odin3_vec_cat(&rhs, i)};
        if (net_a.v == net_b.v) {
            continue;
        }
        bool b_const = odin3_net_const_value(st->mod, net_b) != ODIN3_CONST_NONE;
        odin3_net_pair merged =
            b_const ? (odin3_net_pair){net_b, net_a} : (odin3_net_pair){net_a, net_b};
        res = odin3_net_merge(st->mod, merged);
        rename_net(st, pair, i + 1, merged);
    }
    odin3_vec_free(&lhs);
    odin3_vec_free(&rhs);
    return res;
}

static odin3_status line_connect(pb_state *st) {
    if (st->where == IN_CELL) {
        return cell_connect(st);
    }
    return st->where == IN_MODULE ? module_connect(st) : pb_fail(st, "connect outside a module");
}

/* --- processes ----------------------------------------------------------------------------- */

static void pw_start(odin3_pw *pw) {
    memset(pw, 0, sizeof *pw);
    odin3_vec_init(&pw->bits, sizeof(odin3_pw_bit));
    odin3_vec_init(&pw->cases, sizeof(odin3_pw_case));
    odin3_vec_init(&pw->switches, sizeof(odin3_pw_switch));
    odin3_vec_init(&pw->syncs, sizeof(odin3_pw_sync));
    odin3_vec_init(&pw->promoted, sizeof(odin3_pw_action));
    odin3_vec_init(&pw->in_nets, sizeof(odin3_net_id));
    odin3_vec_init(&pw->attrs, sizeof(odin3_pw_attr));
}

static odin3_status new_case(odin3_pw *pw, odin3_prov_id prov, uint32_t *idx) {
    odin3_pw_case *cs = odin3_vec_push(&pw->cases);
    if (cs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&cs->compares, sizeof(odin3_pw_sig));
    odin3_vec_init(&cs->actions, sizeof(odin3_pw_action));
    odin3_vec_init(&cs->switches, sizeof(uint32_t));
    cs->prov = prov;
    *idx = (uint32_t)pw->cases.len - 1;
    return ODIN3_OK;
}

static odin3_status push_index(odin3_vec *vec, uint32_t idx) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = idx;
    return ODIN3_OK;
}

static odin3_status push_frame(pb_state *st, bool is_switch, uint32_t idx) {
    pb_frame *frame = odin3_vec_push(&st->frames);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (pb_frame){is_switch, idx};
    return ODIN3_OK;
}

static pb_frame top_frame(const pb_state *st) {
    return *(const pb_frame *)odin3_vec_cat(&st->frames, st->frames.len - 1);
}

/* The process's attributes (but \src) as the working form's, their values copied to the arena. */
static odin3_status process_attrs(pb_state *st) {
    for (size_t i = 0; i < st->attrs.len; i++) {
        const pb_attr *attr = odin3_vec_cat(&st->attrs, i);
        if (attr_named(st, attr, "src")) {
            continue;
        }
        odin3_value *val = odin3_arena_alloc(st->arena, sizeof *val);
        odin3_pw_attr *to = val != NULL ? odin3_vec_push(&st->pw.attrs) : NULL;
        if (to == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *val = attr->value;
        *to = (odin3_pw_attr){attr->key, val};
    }
    odin3_vec_clear(&st->attrs);
    return ODIN3_OK;
}

static odin3_status line_process(pb_state *st) {
    if (st->where != IN_MODULE || st->toks.len != 2) {
        return pb_fail(st, "process <name> inside a module");
    }
    pw_start(&st->pw);
    odin3_vec_clear(&st->frames);
    st->in_syncs = false;
    st->where = IN_PROCESS;
    uint32_t root = 0;
    odin3_status res = take_prov(st, &st->pw.prov);
    res = res == ODIN3_OK ? process_attrs(st) : res;
    res = res == ODIN3_OK ? new_case(&st->pw, (odin3_prov_id){0}, &root) : res;
    return res == ODIN3_OK ? push_frame(st, false, root) : res;
}

/* The sigspec at toks[*at] as a new run of the working form. */
static odin3_status parse_pw_sig(pb_state *st, size_t *at, odin3_pw_sig *out) {
    odin3_vec bits;
    odin3_vec_init(&bits, sizeof(uint32_t));
    odin3_status res = parse_sig(st, at, &bits);
    res = res == ODIN3_OK ? odin3_pw_sig_new(&st->pw, bits.data, (uint32_t)bits.len, out) : res;
    odin3_vec_free(&bits);
    return res;
}

/* An action from `<word> lhs rhs`, recorded from a pending \src. */
static odin3_status parse_action(pb_state *st, odin3_pw_action *act) {
    size_t at = 1;
    odin3_status res = parse_pw_sig(st, &at, &act->lhs);
    res = res == ODIN3_OK ? parse_pw_sig(st, &at, &act->rhs) : res;
    if (res == ODIN3_OK && (at != st->toks.len || act->lhs.len != act->rhs.len)) {
        res = pb_fail(st, "an action needs two signals of equal widths");
    }
    res = res == ODIN3_OK ? take_body_prov(st, &act->prov) : res;
    odin3_vec_clear(&st->attrs);
    return res;
}

static odin3_pw_case *frame_case(pb_state *st, pb_frame frame) {
    return odin3_vec_at(&st->pw.cases, frame.idx);
}

static odin3_pw_switch *frame_switch(pb_state *st, pb_frame frame) {
    return odin3_vec_at(&st->pw.switches, frame.idx);
}

static odin3_status line_assign(pb_state *st) {
    if (st->where != IN_PROCESS || st->in_syncs || top_frame(st).is_switch) {
        return pb_fail(st, "assign belongs in a case");
    }
    odin3_pw_action act = {{0, 0}, {0, 0}, {0}};
    odin3_status res = parse_action(st, &act);
    odin3_pw_action *slot =
        res == ODIN3_OK ? odin3_vec_push(&frame_case(st, top_frame(st))->actions) : NULL;
    if (res == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = act;
    }
    return res;
}

/* The switch flags from the pending \full_case and \parallel_case. */
static uint16_t switch_flags(const pb_state *st) {
    uint16_t flags = 0;
    for (size_t i = 0; i < st->attrs.len; i++) {
        const pb_attr *attr = odin3_vec_cat(&st->attrs, i);
        bool set = attr->value.kind == ODIN3_VAL_INT && attr->value.i != 0;
        if (set && attr_named(st, attr, "full_case")) {
            flags |= ODIN3_SWITCH_FULL_CASE;
        } else if (set && attr_named(st, attr, "parallel_case")) {
            flags |= ODIN3_SWITCH_PARALLEL_CASE;
        }
    }
    return flags;
}

static odin3_status line_switch(pb_state *st) {
    if (st->where != IN_PROCESS || st->in_syncs || top_frame(st).is_switch) {
        return pb_fail(st, "switch belongs in a case");
    }
    odin3_pw_switch sw = {{0, 0}, {0}, switch_flags(st), {0}};
    size_t at = 1;
    odin3_status res = parse_pw_sig(st, &at, &sw.signal);
    res = res == ODIN3_OK && at != st->toks.len ? pb_fail(st, "switch <sig>") : res;
    res = res == ODIN3_OK ? take_body_prov(st, &sw.prov) : res;
    odin3_vec_clear(&st->attrs);
    odin3_pw_switch *slot = res == ODIN3_OK ? odin3_vec_push(&st->pw.switches) : NULL;
    if (res == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot == NULL) {
        return res;
    }
    *slot = sw;
    odin3_vec_init(&slot->cases, sizeof(uint32_t));
    uint32_t idx = (uint32_t)st->pw.switches.len - 1;
    res = push_index(&frame_case(st, top_frame(st))->switches, idx);
    return res == ODIN3_OK ? push_frame(st, true, idx) : res;
}

/* The compares of a case line, comma separated, as compares of case idx. */
static odin3_status case_compares(pb_state *st, uint32_t idx) {
    size_t at = 1;
    odin3_status res = ODIN3_OK;
    while (res == ODIN3_OK && at < st->toks.len) {
        odin3_pw_sig sig = {0, 0};
        res = parse_pw_sig(st, &at, &sig);
        odin3_pw_sig *slot =
            res == ODIN3_OK
                ? odin3_vec_push(&((odin3_pw_case *)odin3_vec_at(&st->pw.cases, idx))->compares)
                : NULL;
        res = res == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : res;
        if (slot != NULL) {
            *slot = sig;
        }
        if (res == ODIN3_OK && at < st->toks.len) {
            res = tok_is(tok_at(st, at++), ",") ? ODIN3_OK : pb_fail(st, "compares need commas");
        }
    }
    return res;
}

static odin3_status line_case(pb_state *st) {
    if (st->where == IN_PROCESS && !top_frame(st).is_switch && st->frames.len > 1) {
        odin3_vec_pop(&st->frames); /* the previous case ends */
    }
    if (st->where != IN_PROCESS || st->in_syncs || !top_frame(st).is_switch) {
        return pb_fail(st, "case belongs in a switch");
    }
    odin3_prov_id prov = {0};
    uint32_t idx = 0;
    odin3_status res = take_body_prov(st, &prov);
    odin3_vec_clear(&st->attrs);
    res = res == ODIN3_OK ? new_case(&st->pw, prov, &idx) : res;
    res = res == ODIN3_OK ? case_compares(st, idx) : res;
    res = res == ODIN3_OK ? push_index(&frame_switch(st, top_frame(st))->cases, idx) : res;
    return res == ODIN3_OK ? push_frame(st, false, idx) : res;
}

/* The sync type named by toks[1]; false for another word. */
static bool sync_type(pb_tok word, uint8_t *out) {
    static const char *const k_types[] = {"posedge", "negedge", "low", "high", "always"};
    static const uint8_t k_type_of[] = {ODIN3_SYNC_POSEDGE, ODIN3_SYNC_NEGEDGE, ODIN3_SYNC_LEVEL0,
                                        ODIN3_SYNC_LEVEL1, ODIN3_SYNC_ALWAYS};
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        if (tok_is(word, k_types[i])) {
            *out = k_type_of[i];
            return true;
        }
    }
    return false;
}

/* The one-bit signal of an edge or level sync, from toks[2]. */
static odin3_status sync_signal(pb_state *st, odin3_pw_bit *out) {
    odin3_pw_sig sig = {0, 0};
    size_t at = 2;
    odin3_status res = parse_pw_sig(st, &at, &sig);
    if (res == ODIN3_OK && (sig.len != 1 || at != st->toks.len)) {
        res = pb_fail(st, "sync <type> <one-bit signal>");
    }
    *out = res == ODIN3_OK ? *odin3_pw_sig_bits(&st->pw, sig) : 0;
    return res;
}

static odin3_status line_sync(pb_state *st) {
    if (st->where != IN_PROCESS || st->frames.len != 1) {
        return pb_fail(st, "sync after the process's switches are closed");
    }
    odin3_pw_sync sync = {0, 0, {0}};
    if (!sync_type(tok_at(st, 1), &sync.type)) {
        return pb_fail(st, "unknown sync type");
    }
    odin3_status res = ODIN3_OK;
    if (sync.type != ODIN3_SYNC_ALWAYS) {
        res = sync_signal(st, &sync.signal);
    } else if (st->toks.len != 2) {
        res = pb_fail(st, "sync always takes no signal");
    }
    odin3_pw_sync *slot = res == ODIN3_OK ? odin3_vec_push(&st->pw.syncs) : NULL;
    if (slot != NULL) {
        *slot = sync;
        odin3_vec_init(&slot->actions, sizeof(odin3_pw_action));
        st->in_syncs = true;
    }
    return res == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : res;
}

static odin3_status line_update(pb_state *st) {
    if (st->where != IN_PROCESS || !st->in_syncs) {
        return pb_fail(st, "update belongs in a sync");
    }
    odin3_pw_action act = {{0, 0}, {0, 0}, {0}};
    odin3_status res = parse_action(st, &act);
    odin3_pw_sync *sync = odin3_vec_at(&st->pw.syncs, st->pw.syncs.len - 1);
    odin3_pw_action *slot = res == ODIN3_OK ? odin3_vec_push(&sync->actions) : NULL;
    if (res == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = act;
    }
    return res;
}

static odin3_status process_end(pb_state *st) {
    if (st->attrs.len != 0) {
        return pb_fail(st, "dangling attribute");
    }
    if (st->frames.len > 1) {
        if (!top_frame(st).is_switch) {
            odin3_vec_pop(&st->frames); /* the switch's last case */
        }
        odin3_vec_pop(&st->frames); /* the switch */
        return ODIN3_OK;
    }
    odin3_status res = odin3_pw_encode(&st->mctx, &st->pw);
    odin3_pw_free(&st->pw);
    st->where = IN_MODULE;
    return res;
}

/* --- the dispatch -------------------------------------------------------------------------- */

static odin3_status module_end(pb_state *st) {
    odin3_proc_mctx_free(&st->mctx);
    st->mctx_open = false;
    free_tmps(st);
    st->where = AT_TOP;
    return st->attrs.len == 0 ? ODIN3_OK : pb_fail(st, "dangling attribute");
}

static odin3_status line_end(pb_state *st) {
    if (st->toks.len != 1) {
        return pb_fail(st, "end takes nothing");
    }
    switch (st->where) {
    case IN_CELL:
        return cell_end(st);
    case IN_PROCESS:
        return process_end(st);
    case IN_MODULE:
        return module_end(st);
    case AT_TOP:
        break;
    }
    return pb_fail(st, "end without a block");
}

static odin3_status line_ignored(pb_state *st) {
    return st->where == AT_TOP ? ODIN3_OK : pb_fail(st, "autoidx inside a block");
}

typedef odin3_status (*pb_line_fn)(pb_state *st);

static odin3_status dispatch(pb_state *st) {
    static const struct {
        const char *word;
        pb_line_fn run;
    } k_lines[] = {
        {"attribute", line_attribute}, {"module", line_module},   {"end", line_end},
        {"wire", line_wire},           {"cell", line_cell},       {"parameter", line_parameter},
        {"connect", line_connect},     {"process", line_process}, {"assign", line_assign},
        {"switch", line_switch},       {"case", line_case},       {"sync", line_sync},
        {"update", line_update},       {"autoidx", line_ignored}};
    pb_tok word = tok_at(st, 0);
    for (size_t i = 0; i < sizeof k_lines / sizeof k_lines[0]; i++) {
        if (tok_is(word, k_lines[i].word)) {
            return k_lines[i].run(st);
        }
    }
    char buf[PB_TEXT_MAX];
    return pb_fail(st, "unknown line '%s'", tok_text(word, buf, sizeof buf));
}

static void state_free(pb_state *st) {
    odin3_vec_free(&st->toks);
    odin3_vec_free(&st->attrs);
    if (st->mctx_open) {
        odin3_proc_mctx_free(&st->mctx);
    }
    free_tmps(st);
    odin3_vec_free(&st->tmps);
    odin3_pw_free(&st->pw);
    odin3_vec_free(&st->frames);
    free_cell(st);
    odin3_vec_free(&st->conns);
    odin3_vec_free(&st->cell_attrs);
    odin3_arena_destroy(st->arena);
}

static odin3_status state_open(pb_state *st, pb_result *out) {
    memset(st, 0, sizeof *st);
    st->res = out;
    odin3_vec_init(&st->toks, sizeof(pb_tok));
    odin3_vec_init(&st->attrs, sizeof(pb_attr));
    odin3_vec_init(&st->tmps, sizeof(pb_tmp));
    odin3_vec_init(&st->frames, sizeof(pb_frame));
    odin3_vec_init(&st->conns, sizeof(pb_conn));
    odin3_vec_init(&st->cell_attrs, sizeof(pb_attr));
    pw_start(&st->pw);
    st->arena = odin3_arena_create(PB_ARENA_CHUNK);
    out->design = odin3_design_create();
    if (st->arena == NULL || out->design == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t run = 0;
    odin3_status res = intern_text(st, "pb_build", strlen("pb_build"), &run);
    res = res == ODIN3_OK ? odin3_pass_run_begin(out->design, run, &out->ctx) : res;
    odin3_prov_origin origin = {NULL, 0, 0, 0};
    return res == ODIN3_OK ? odin3_prov_source(&out->ctx, &origin, &st->prov) : res;
}

odin3_status pb_build(const char *rtlil, pb_result *out) {
    memset(out, 0, sizeof *out);
    pb_state st;
    odin3_status res = state_open(&st, out);
    const char *line = rtlil;
    while (res == ODIN3_OK && line != NULL && *line != '\0') {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        st.line++;
        res = tokenize(&st, line, len);
        if (res == ODIN3_OK && st.toks.len > 0) {
            res = dispatch(&st);
        }
        line = nl != NULL ? nl + 1 : NULL;
    }
    if (res == ODIN3_OK && st.where != AT_TOP) {
        res = pb_fail(&st, "the text ends inside a block");
    }
    state_free(&st);
    if (res != ODIN3_OK) {
        pb_free(out);
    }
    return res;
}

void pb_free(pb_result *res) {
    if (res == NULL) {
        return;
    }
    odin3_design_destroy(res->design);
    memset(res, 0, sizeof *res);
}

odin3_module *pb_module(const pb_result *res, const char *name) {
    const odin3_strtab *tab = odin3_design_strtab(res->design);
    for (uint32_t i = 1; i < odin3_design_module_end(res->design); i++) {
        odin3_module *mod = odin3_module_get(res->design, (odin3_module_id){i});
        if (mod != NULL && strcmp(odin3_strtab_get(tab, odin3_module_name(mod)), name) == 0) {
            return mod;
        }
    }
    return NULL;
}

odin3_node_id pb_proc(const pb_result *res, const char *module, uint32_t nth) {
    const odin3_module *mod = pb_module(res, module);
    uint32_t seen = 0;
    for (uint32_t i = 1; mod != NULL && i < odin3_module_node_end(mod); i++) {
        odin3_node_id node = {i};
        const odin3_celltype_def *def =
            odin3_node_live(mod, node) ? odin3_celltype_get(res->design, odin3_node_type(mod, node))
                                       : NULL;
        if (def != NULL && (def->flags & ODIN3_CT_PROCESS) != 0 && seen++ == nth) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

odin3_status pb_print(const odin3_module *module, odin3_node_id node, odin3_strbuf *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status res = odin3_proc_print(module, node, 0, &text);
    for (const char *line = text.data; res == ODIN3_OK && line != NULL && *line != '\0';) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t)(nl - line) + 1 : strlen(line);
        if (strncmp(line, "process ", strlen("process ")) != 0) {
            res = odin3_strbuf_append(out, (odin3_bytes){line, len});
        }
        line += len;
    }
    odin3_strbuf_free(&text);
    return res;
}

odin3_status pb_rtlil(const pb_result *res, odin3_strbuf *out) {
    return odin3_rtlil_text(res->design, out);
}
