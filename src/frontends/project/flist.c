/*
 * flist.c — the -f/-F file-list reader (PRJ-11): whole-list words, $VAR expansion as words are
 * consumed, nested lists on an explicit stack of frames (point (b)), the §5.2 option table.
 */
#include "frontends/project/project.h"

#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/words.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAP_TEXT = 160 };

/* One open list: its text and words, the next word, the directory its entries resolve against
 * (a view of a strtab string, which never moves), its resolved path (the -f chain) and the -sv
 * of the list that opened it (restored when it is popped). */
typedef struct flist_frame {
    odin3_proj_text text;
    odin3_vec words; /* odin3_word */
    size_t cursor;
    odin3_bytes base;
    uint32_t list;
    bool outer_sv;
} flist_frame;

typedef struct flist_reader {
    odin3_project *proj;
    odin3_srcman *sm;
    odin3_vec frames; /* flist_frame, the entry list first */
    odin3_bytes root; /* f_root: the entry list's directory */
    uint32_t work;    /* "work" */
    bool sv;
    odin3_strbuf word; /* the consumed word, expanded */
    odin3_strbuf arg;  /* an option's argument, expanded */
    odin3_strbuf name; /* a variable's name, NUL-terminated for getenv */
} flist_reader;

/* A consumed word: its text (expanded once expand ran) and location. */
typedef struct flist_word {
    odin3_bytes text;
    odin3_loc at;
} flist_word;

typedef struct flist_what {
    odin3_proj_err_kind kind;
    const char *detail;
} flist_what;

typedef enum flist_opt { OPT_NONE, OPT_F, OPT_V, OPT_Y, OPT_TOP } flist_opt;

typedef enum flist_plus { PLUS_NONE, PLUS_INCDIR, PLUS_DEFINE, PLUS_LIBEXT } flist_plus;

static bool bytes_is(odin3_bytes bytes, const char *str) {
    size_t len = strlen(str);
    return bytes.len == len && (len == 0 || memcmp(bytes.ptr, str, len) == 0);
}

static odin3_bytes strtab_bytes(const flist_reader *rd, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(rd->proj->design);
    return (odin3_bytes){odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
}

static flist_frame *top_frame(flist_reader *rd) {
    return odin3_vec_at(&rd->frames, rd->frames.len - 1);
}

static odin3_status fail_one(flist_reader *rd, odin3_loc at, flist_what what, odin3_bytes name) {
    odin3_proj_err err = {what.kind, ODIN3_PROJ_LOCATED, at, NULL, what.detail, &name, 1};
    return odin3_project_fail(rd->proj, &err);
}

static odin3_status fail_detail(flist_reader *rd, odin3_loc at, const char *detail) {
    odin3_proj_err err = {ODIN3_PROJ_ERR_SYNTAX, ODIN3_PROJ_LOCATED, at, NULL, detail, NULL, 0};
    return odin3_project_fail(rd->proj, &err);
}

/* --- words (§4.2, capped by ODIN3_PROJ_MAX_WORDS before they grow) ------------------------- */

/* The cap is per list (each frame's words; spec §9: "words per file list"), not per project:
 * nested lists hold at most flist_depth + 1 such vectors at once. */

static odin3_status word_cap(flist_reader *rd, const flist_frame *frame, uint32_t off) {
    char detail[CAP_TEXT];
    (void)snprintf(detail, sizeof detail,
                   "file list has more than %" PRIu32 " words (ODIN3_PROJ_MAX_WORDS)",
                   rd->proj->limits.words);
    return fail_detail(rd, odin3_srcman_loc(rd->sm, frame->text.buffer, off), detail);
}

/* Appends the words of one line of frame's list (file offset offset) to its words. */
static odin3_status split_line(flist_reader *rd, flist_frame *frame, odin3_bytes line,
                               uint32_t offset) {
    size_t limit = rd->proj->limits.words;
    size_t room = frame->words.len < limit ? limit - frame->words.len : 0;
    odin3_words_opts opts = {offset, ODIN3_WORDS_FLIST, rd->proj->arena};
    size_t over = 0;
    size_t count = odin3_words_count(line, &opts, room, &over);
    if (count > room) {
        return word_cap(rd, frame, offset + (uint32_t)over);
    }
    odin3_status st = ODIN3_OK;
    if (count > 0) { /* exact for a line that splits: the split below never grows the vec */
        st = odin3_vec_reserve(&frame->words, frame->words.len + count);
    }
    odin3_words_err err = {0, {NULL, 0}, NULL};
    st = st != ODIN3_OK ? st : odin3_words_split(line, &opts, &frame->words, &err);
    if (st == ODIN3_ERR_PARSE) { /* the oracle names nothing for these */
        return fail_detail(rd, odin3_srcman_loc(rd->sm, frame->text.buffer, err.off), err.what);
    }
    return st;
}

/* --- the frame stack ----------------------------------------------------------------------- */

/* Loads list (its FILE buffer's parent: the naming word) into frame and splits all its words. */
static odin3_status open_list(flist_reader *rd, flist_frame *frame, const odin3_proj_path *list,
                              odin3_loc parent) {
    odin3_status st = odin3_project_load(rd->proj, list, parent, &frame->text);
    for (size_t i = 0; st == ODIN3_OK && i < frame->text.lines.len; i++) {
        uint32_t offset = 0;
        odin3_bytes line = odin3_proj_text_line(&frame->text, (uint32_t)i, &offset);
        st = split_line(rd, frame, line, offset);
    }
    return st;
}

static odin3_status push_list(flist_reader *rd, const odin3_proj_path *list, odin3_loc parent,
                              odin3_bytes base) {
    flist_frame *frame = odin3_vec_push(&rd->frames);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&frame->text.lines, sizeof(uint32_t));
    odin3_vec_init(&frame->words, sizeof(odin3_word));
    frame->base = base;
    frame->list = list->resolved;
    frame->outer_sv = rd->sv;
    return open_list(rd, frame, list, parent);
}

/* Closes the innermost list; its -sv ends with it. */
static void pop_list(flist_reader *rd) {
    flist_frame *frame = top_frame(rd);
    rd->sv = frame->outer_sv;
    odin3_proj_text_free(&frame->text);
    odin3_vec_free(&frame->words);
    odin3_vec_pop(&rd->frames);
}

static flist_word next_word(const flist_reader *rd, flist_frame *frame) {
    const odin3_word *word = odin3_vec_cat(&frame->words, frame->cursor++);
    return (flist_word){word->text, odin3_srcman_loc(rd->sm, frame->text.buffer, word->off)};
}

/* --- $NAME and ${NAME} --------------------------------------------------------------------- */

static bool ident_start(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool ident_char(uint8_t byte) {
    return ident_start(byte) || (byte >= '0' && byte <= '9') || byte == '$';
}

/* The length of the IDENT ([A-Za-z_][A-Za-z0-9_$]*) at text[from, len), 0 when none. */
static size_t ident_len(const uint8_t *text, size_t from, size_t len) {
    if (from >= len || !ident_start(text[from])) {
        return 0;
    }
    size_t j = from + 1;
    while (j < len && ident_char(text[j])) {
        j++;
    }
    return j - from;
}

/* A reference: the name's offset and length in the word, and the offset just past it. */
typedef struct var_ref {
    size_t name, len, end;
} var_ref;

/* The reference at word[dollar] == '$' (the oracle's VAR: ${NAME} first, then $NAME); false
 * when the '$' starts none. */
static bool var_at(odin3_bytes word, size_t dollar, var_ref *ref) {
    const uint8_t *text = word.ptr;
    if (dollar + 1 < word.len && text[dollar + 1] == '{') {
        size_t len = ident_len(text, dollar + 2, word.len);
        *ref = (var_ref){dollar + 2, len, dollar + 3 + len};
        return len > 0 && dollar + 2 + len < word.len && text[dollar + 2 + len] == '}';
    }
    size_t len = ident_len(text, dollar + 1, word.len);
    *ref = (var_ref){dollar + 1, len, dollar + 1 + len};
    return len > 0;
}

/* Appends the value of the variable ref names in word to buf; unset: `undefined_variable`. */
static odin3_status append_value(flist_reader *rd, const flist_word *word, var_ref ref,
                                 odin3_strbuf *buf) {
    odin3_bytes name = {(const uint8_t *)word->text.ptr + ref.name, ref.len};
    odin3_strbuf_clear(&rd->name);
    odin3_status st = odin3_strbuf_append(&rd->name, name);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *value = getenv(rd->name.data);
    if (value == NULL) {
        flist_what what = {ODIN3_PROJ_ERR_UNDEFINED_VARIABLE, "environment variable not set"};
        return fail_one(rd, word->at, what, name);
    }
    return odin3_strbuf_append(buf, odin3_bytes_cstr(value));
}

/* Appends word's text to buf with each reference replaced by its value, left to right (the
 * first unset one is the error); *stray = some '$' starts no reference. */
static odin3_status substitute(flist_reader *rd, const flist_word *word, odin3_strbuf *buf,
                               bool *stray) {
    const uint8_t *text = word->text.ptr;
    size_t len = word->text.len;
    size_t from = 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < len && st == ODIN3_OK;) {
        var_ref ref = {0, 0, 0};
        if (text[i] != '$' || !var_at(word->text, i, &ref)) {
            *stray = *stray || text[i] == '$';
            i++;
            continue;
        }
        st = odin3_strbuf_append(buf, (odin3_bytes){text + from, i - from});
        st = st != ODIN3_OK ? st : append_value(rd, word, ref, buf);
        i = ref.end;
        from = i;
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(buf, (odin3_bytes){text + from, len - from});
}

/*
 * Expands word in place (its text then lives in buf when it held a '$'). A '$' of the word's own
 * text outside every reference is `syntax`, names [word], after the variables are looked up — so
 * a value may hold '$', and an unset variable is reported first, as in the oracle.
 */
static odin3_status expand(flist_reader *rd, flist_word *word, odin3_strbuf *buf) {
    if (word->text.len == 0 || memchr(word->text.ptr, '$', word->text.len) == NULL) {
        return ODIN3_OK;
    }
    odin3_strbuf_clear(buf);
    bool stray = false;
    odin3_status st = substitute(rd, word, buf, &stray);
    if (st == ODIN3_OK && stray) {
        flist_what what = {ODIN3_PROJ_ERR_SYNTAX, "bad variable reference"};
        return fail_one(rd, word->at, what, word->text);
    }
    if (st == ODIN3_OK) {
        word->text = buf->len > 0 ? (odin3_bytes){buf->data, buf->len} : (odin3_bytes){"", 0};
    }
    return st;
}

/* --- entries ------------------------------------------------------------------------------- */

/* A file named at `at`, resolved against the current list's base. */
static odin3_status list_path(flist_reader *rd, odin3_bytes given, odin3_loc at,
                              odin3_proj_path *out) {
    odin3_proj_path_req req = {top_frame(rd)->base, given, at, ODIN3_PROJ_WANT_FILE};
    return odin3_project_path(rd->proj, &req, out);
}

static odin3_status add_dir(flist_reader *rd, odin3_proj_dir_list list, odin3_bytes given,
                            odin3_loc at) {
    odin3_proj_path_req req = {top_frame(rd)->base, given, at, ODIN3_PROJ_WANT_DIR};
    odin3_proj_path dir = {0, 0, {0}};
    odin3_status st = odin3_project_path(rd->proj, &req, &dir);
    return st != ODIN3_OK ? st : odin3_project_add_dir(rd->proj, list, &dir);
}

static bool lang_or_fail(flist_reader *rd, const flist_word *file, odin3_lang *lang,
                         odin3_status *st) {
    if (odin3_lang_of(file->text, lang)) {
        return true;
    }
    flist_what what = {ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "no language for file"};
    *st = fail_one(rd, file->at, what, file->text);
    return false;
}

/* A bare word: a source file in `work`; Verilog is SystemVerilog under -sv. */
static odin3_status add_source(flist_reader *rd, const flist_word *word) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    odin3_status st = ODIN3_OK;
    if (!lang_or_fail(rd, word, &lang, &st)) {
        return st;
    }
    if (rd->sv && lang == ODIN3_LANG_VERILOG) {
        lang = ODIN3_LANG_SYSTEMVERILOG;
    }
    odin3_proj_file file = {.lang = (uint8_t)lang, .library = rd->work};
    st = list_path(rd, word->text, word->at, &file.path);
    return st != ODIN3_OK ? st : odin3_project_add_file(rd->proj, &file);
}

/* -v <file>: a library file, located at the option. */
static odin3_status add_libfile(flist_reader *rd, odin3_bytes given, odin3_loc at) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    odin3_status st = ODIN3_OK;
    flist_word file = {given, at};
    if (!lang_or_fail(rd, &file, &lang, &st)) {
        return st;
    }
    odin3_proj_file lib = {.lang = (uint8_t)lang, .library = 0};
    st = list_path(rd, given, at, &lib.path);
    return st != ODIN3_OK ? st : odin3_project_add_libfile(rd->proj, &lib);
}

/* --- nested lists -------------------------------------------------------------------------- */

/* `f_cycle` at at: the chain from frame `from` to the innermost list, then list again. */
static odin3_status cycle(flist_reader *rd, size_t from, odin3_loc at, uint32_t list) {
    size_t count = rd->frames.len - from + 1;
    odin3_bytes *names = odin3_util_malloc(count * sizeof *names);
    if (names == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = from; i < rd->frames.len; i++) {
        const flist_frame *frame = odin3_vec_cat(&rd->frames, i);
        names[i - from] = strtab_bytes(rd, frame->list);
    }
    names[count - 1] = strtab_bytes(rd, list);
    odin3_proj_err err = {
        ODIN3_PROJ_ERR_F_CYCLE, ODIN3_PROJ_LOCATED, at, NULL, "file list cycle", names,
        (uint32_t)count};
    odin3_status st = odin3_project_fail(rd->proj, &err);
    odin3_util_free(names);
    return st;
}

static odin3_status too_deep(flist_reader *rd, odin3_loc at, odin3_bytes given) {
    char detail[CAP_TEXT];
    (void)snprintf(detail, sizeof detail,
                   "file lists nested more than %" PRIu32 " deep (ODIN3_PROJ_MAX_FLIST_DEPTH)",
                   rd->proj->limits.flist_depth);
    flist_what what = {ODIN3_PROJ_ERR_SYNTAX, detail};
    return fail_one(rd, at, what, given);
}

/*
 * -f/-F <list>: the list resolves against the current base and must exist; one already open is a
 * cycle; then it is opened with its entries resolving against f_root (-f) or its own directory
 * (-F) — the oracle's f_option, ruling I5.
 */
static odin3_status open_nested(flist_reader *rd, const flist_word *opt, odin3_bytes given) {
    odin3_proj_path list = {0, 0, {0}};
    odin3_status st = list_path(rd, given, opt->at, &list);
    if (st != ODIN3_OK) {
        return st;
    }
    for (size_t i = 0; i < rd->frames.len; i++) {
        const flist_frame *frame = odin3_vec_cat(&rd->frames, i);
        if (frame->list == list.resolved) {
            return cycle(rd, i, opt->at, list.resolved);
        }
    }
    if (rd->frames.len > rd->proj->limits.flist_depth) {
        return too_deep(rd, opt->at, given);
    }
    odin3_bytes base = rd->root;
    if (bytes_is(opt->text, "-F")) {
        base = odin3_path_dir(strtab_bytes(rd, list.resolved));
    }
    return push_list(rd, &list, opt->at, base);
}

/* --- options ------------------------------------------------------------------------------- */

static flist_opt option_of(odin3_bytes word) {
    if (bytes_is(word, "-f") || bytes_is(word, "-F")) {
        return OPT_F;
    }
    if (bytes_is(word, "-v")) {
        return OPT_V;
    }
    if (bytes_is(word, "-y")) {
        return OPT_Y;
    }
    return bytes_is(word, "-top") || bytes_is(word, "--top-module") ? OPT_TOP : OPT_NONE;
}

/* An option taking the next word (expanded, located at its own line) as its argument; errors
 * about the argument are located at the option. */
static odin3_status with_argument(flist_reader *rd, flist_opt opt, const flist_word *word) {
    flist_frame *frame = top_frame(rd);
    if (frame->cursor == frame->words.len) {
        flist_what what = {ODIN3_PROJ_ERR_SYNTAX, "option needs an argument"};
        return fail_one(rd, word->at, what, word->text);
    }
    flist_word arg = next_word(rd, frame);
    odin3_status st = expand(rd, &arg, &rd->arg);
    if (st != ODIN3_OK) {
        return st;
    }
    switch (opt) {
    case OPT_F:
        return open_nested(rd, word, arg.text);
    case OPT_V:
        return add_libfile(rd, arg.text, word->at);
    case OPT_Y:
        return add_dir(rd, ODIN3_PROJ_LIBDIRS, arg.text, word->at);
    default:
        return odin3_project_set_top(rd->proj, arg.text, word->at);
    }
}

static flist_plus plus_of(odin3_bytes key) {
    if (bytes_is(key, "incdir")) {
        return PLUS_INCDIR;
    }
    if (bytes_is(key, "define")) {
        return PLUS_DEFINE;
    }
    return bytes_is(key, "libext") ? PLUS_LIBEXT : PLUS_NONE;
}

static odin3_status plus_value(flist_reader *rd, flist_plus plus, odin3_bytes value, odin3_loc at) {
    if (plus == PLUS_INCDIR) {
        return add_dir(rd, ODIN3_PROJ_INCDIRS, value, at);
    }
    if (plus == PLUS_DEFINE) {
        return odin3_project_add_define(rd->proj, value, at);
    }
    return odin3_project_add_libext(rd->proj, value, at);
}

/* +incdir+, +define+, +libext+: the values split at '+', empty ones dropped, at least one. */
static odin3_status plus_option(flist_reader *rd, const flist_word *word) {
    const uint8_t *text = (const uint8_t *)word->text.ptr + 1;
    size_t len = word->text.len - 1;
    const uint8_t *mark = len > 0 ? memchr(text, '+', len) : NULL;
    size_t key_len = mark != NULL ? (size_t)(mark - text) : len;
    flist_plus plus = plus_of((odin3_bytes){text, key_len});
    if (plus == PLUS_NONE) {
        flist_what what = {ODIN3_PROJ_ERR_UNKNOWN_OPTION, "unknown option"};
        return fail_one(rd, word->at, what, word->text);
    }
    size_t first = key_len;
    while (first < len && text[first] == '+') {
        first++;
    }
    if (first == len) {
        flist_what what = {ODIN3_PROJ_ERR_SYNTAX, "option has no value"};
        return fail_one(rd, word->at, what, word->text);
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = first; i < len && st == ODIN3_OK;) {
        size_t j = i;
        while (j < len && text[j] != '+') {
            j++;
        }
        if (j > i) {
            st = plus_value(rd, plus, (odin3_bytes){text + i, j - i}, word->at);
        }
        i = j + 1;
    }
    return st;
}

/* Consumes the next word of the innermost list (closing it when it has none left). */
static odin3_status step(flist_reader *rd) {
    flist_frame *frame = top_frame(rd);
    if (frame->cursor == frame->words.len) {
        pop_list(rd);
        return ODIN3_OK;
    }
    flist_word word = next_word(rd, frame);
    odin3_status st = expand(rd, &word, &rd->word);
    if (st != ODIN3_OK) {
        return st;
    }
    flist_opt opt = option_of(word.text);
    if (opt != OPT_NONE) {
        return with_argument(rd, opt, &word);
    }
    if (bytes_is(word.text, "-sv")) {
        rd->sv = true;
        return ODIN3_OK;
    }
    const uint8_t *first = word.text.ptr;
    if (word.text.len > 0 && first[0] == '+') {
        return plus_option(rd, &word);
    }
    if (word.text.len > 0 && first[0] == '-') {
        flist_what what = {ODIN3_PROJ_ERR_UNKNOWN_OPTION, "unknown option"};
        return fail_one(rd, word.at, what, word.text);
    }
    return add_source(rd, &word);
}

odin3_status odin3_read_flist(odin3_project *proj, const odin3_proj_path *file) {
    flist_reader rd = {.proj = proj, .sm = NULL};
    odin3_vec_init(&rd.frames, sizeof(flist_frame));
    odin3_strbuf_init(&rd.word);
    odin3_strbuf_init(&rd.arg);
    odin3_strbuf_init(&rd.name);
    odin3_status st = odin3_design_get_srcman(proj->design, &rd.sm);
    st = st != ODIN3_OK ? st : odin3_project_intern(proj, odin3_bytes_cstr("work"), &rd.work);
    if (st == ODIN3_OK) {
        rd.root = odin3_path_dir(strtab_bytes(&rd, file->resolved));
        st = push_list(&rd, file, (odin3_loc){0}, rd.root);
    }
    while (st == ODIN3_OK && rd.frames.len > 0) {
        st = step(&rd);
    }
    while (rd.frames.len > 0) {
        pop_list(&rd);
    }
    odin3_vec_free(&rd.frames);
    odin3_strbuf_free(&rd.word);
    odin3_strbuf_free(&rd.arg);
    odin3_strbuf_free(&rd.name);
    return st;
}
