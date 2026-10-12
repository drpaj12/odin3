/*
 * script.c — the ABC script checker (3D spec §7.3, AB-21; plan Task 11). Pure C: it never includes
 * or calls ABC. A script is read once and checked whole before ABC starts:
 *
 *   1. one pass over the bytes (steps 1-6): the size cap, NUL bytes, the bytes allowed outside a
 *      comment, the line cap, `#` comments, quotes and `!`, `;` and line ends between commands,
 *      `{K}`, and the word cap, giving the raw commands with their lines;
 *   2. one pass over the commands (steps 7-11): each raw command's aliases are expanded through an
 *      explicit worklist (no recursion; the depth cap), then each expanded command must be on the
 *      allow-list, its arguments must be options or numbers without a file, string or crash option,
 *      the count cap holds, and the two network spaces follow its effect;
 *   3. the end: the current space must be mapped (step 11), and the output is built (step 12).
 *
 * Every error is logged once, `abc: <given>:<line>: <text>` (point (x)), and *out is untouched.
 *
 * Mapping-state rules read at the pin (a3001b72) on top of §7.5's effects, so that "accepted"
 * implies "ends mapped" and no accepted script is one ABC refuses for its state (agent defaults,
 * Task 11 report; PHASE3 #22):
 *  - `&if` refuses a & network that already has a mapping ("Current AIG has mapping. Run &st",
 *    abc.c:45512, Gia_ManHasMapping): directly after `&get -m` of a mapped network (the 3D-0
 *    spike), after `&if`, or after `&mfs`. `&lf` and `&of` refuse it too, and are not allowed.
 *  - `&get` keeps the mapping only with -m and without -c (abc.c:35113-35118, abcFunc.c:1075);
 *    its flags toggle, and it takes no option values, so the parity of each letter in the option
 *    words before the first other word is exact.
 *  - `if` with -g, -x, -y or -u, and `&if` with -g, -x or -y, give an AIG, not a mapping
 *    (fDelayOpt, fDsdBalance, fUserRecLib, fUserSesLib: abcIf.c:177-180, giaIf.c:3077-3080).
 *    Any occurrence of such a letter counts as on (a conservative reading of the toggles).
 */
#include "passes/abc/script.h"

#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/vec.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ABC_NO_LINE = 0,      /* the size and unmapped-end messages name no line */
    ABC_PRINT_MIN = 0x20, /* ' ': the printable ASCII range allowed outside a comment */
    ABC_PRINT_MAX = 0x7e, /* '~' */
    ABC_K_TOKEN = 3,      /* the bytes of `{K}` */
    ABC_SUBST_SHOW = 32,  /* name bytes an unknown substitution's message shows */
    ABC_DECIMAL = 10,
    ABC_ARENA_CHUNK = 16384
};

/* A network space's state (§7.3 step 11). */
enum { ABC_EMPTY = 0, ABC_AIG = 1, ABC_MAPPED = 2 };

/* One command's words: a raw command of the byte pass, or an item of the alias worklist. */
typedef struct abc_item {
    const char **words; /* arena strings; words[0] is the command or alias name */
    uint32_t n_words;
    uint32_t line;     /* the script line it came from */
    uint32_t depth;    /* alias expansions that produced it */
    const char *alias; /* the outermost alias (an arena copy), or NULL */
} abc_item;

/* What one check needs: the request, the arena of the result, the worklists and step 11's state. */
typedef struct abc_parse {
    const char *given;
    uint32_t lut_k;
    bool uses_k;
    uint8_t ntk, gia; /* ABC_EMPTY, ABC_AIG or ABC_MAPPED */
    uint8_t cur;      /* odin3_abc_space: the space of the last command that changed a network */
    odin3_arena *arena;
    odin3_vec raw;   /* abc_item: the byte pass's commands, in order */
    odin3_vec stack; /* abc_item: the alias worklist; the top is the next command */
    odin3_vec segs;  /* abc_item: one alias expansion's commands (scratch) */
    odin3_vec cmds;  /* odin3_abc_cmd: the checked commands */
} abc_parse;

/* The byte pass's state: the current line, command and word. */
typedef struct abc_scan {
    abc_parse *ps;
    const uint8_t *bytes;
    size_t len;
    size_t pos;
    uint32_t line;
    uint32_t line_len; /* bytes of the line so far, without its LF or CRLF */
    bool in_comment;
    uint32_t word_len;
    uint32_t n_words;
    const char *words[ODIN3_ABC_MAX_WORDS];
    char word[ODIN3_ABC_MAX_LINE_BYTES + 1]; /* a word is never longer than its line */
} abc_scan;

/* The parse's inputs once the bytes are in memory. */
typedef struct abc_request {
    const char *given;
    const uint8_t *bytes;
    size_t len;
    uint32_t lut_k;
    uint32_t exchange;
} abc_request;

/* The ` (from alias '<a>')` that follows a command name in a message, or three empty strings. */
typedef struct abc_from {
    const char *pre, *name, *post;
} abc_from;

/* --- messages ------------------------------------------------------------------------------- */

static odin3_status abc_fail(const abc_parse *ps, uint32_t line, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/* Logs `abc: <given>:<line>: <text>` (no `:<line>` for ABC_NO_LINE) and returns PARSE. */
static odin3_status abc_fail(const abc_parse *ps, uint32_t line, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (line == ABC_NO_LINE) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: %s", ps->given, msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s:%" PRIu32 ": %s", ps->given, line, msg);
    }
    return ODIN3_ERR_PARSE;
}

static abc_from from_alias(const abc_item *item) {
    if (item->alias == NULL) {
        return (abc_from){"", "", ""};
    }
    return (abc_from){" (from alias '", item->alias, "')"};
}

/* --- characters ----------------------------------------------------------------------------- */

static bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static bool is_alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/* A name byte of an unknown substitution's message: [A-Za-z0-9_]. */
static bool is_name_byte(uint8_t byte) {
    return is_alpha((char)byte) || is_digit((char)byte) || byte == '_';
}

/* An option word: -[A-Za-z][A-Za-z0-9]* (ABC's getopt reads every letter of a cluster). */
static bool is_option(const char *word) {
    if (word[0] != '-' || !is_alpha(word[1])) {
        return false;
    }
    for (const char *at = word + 2; *at != '\0'; at++) {
        if (!is_alpha(*at) && !is_digit(*at)) {
            return false;
        }
    }
    return true;
}

static const char *skip_digits(const char *at) {
    while (is_digit(*at)) {
        at++;
    }
    return at;
}

/* A number word: [-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)? */
static bool is_number(const char *word) {
    const char *at = word + (word[0] == '-' || word[0] == '+');
    const char *end = skip_digits(at);
    if (end == at) {
        return false;
    }
    if (*end == '.') {
        at = end + 1;
        end = skip_digits(at);
        if (end == at) {
            return false;
        }
    }
    if (*end == 'e' || *end == 'E') {
        at = end + 1;
        at += *at == '-' || *at == '+';
        end = skip_digits(at);
        if (end == at) {
            return false;
        }
    }
    return *end == '\0';
}

/* An echo argument: printable, without a blank, a quote, `!`, `;`, `#`, `{` or `}`. */
static bool is_echo_word(const char *word) {
    for (const char *at = word; *at != '\0'; at++) {
        if (*at <= ' ' || *at > (char)ABC_PRINT_MAX || strchr("\"'!;#{}", *at) != NULL) {
            return false;
        }
    }
    return true;
}

/* --- the byte pass (steps 1-6) -------------------------------------------------------------- */

/* Ends the current word (step 6). */
static odin3_status end_word(abc_scan *sc) {
    if (sc->word_len == 0) {
        return ODIN3_OK;
    }
    if (sc->n_words == ODIN3_ABC_MAX_WORDS) {
        return abc_fail(sc->ps, sc->line, "command has more than %d words", ODIN3_ABC_MAX_WORDS);
    }
    char *copy = odin3_arena_strndup(sc->ps->arena, sc->word, sc->word_len);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sc->words[sc->n_words++] = copy;
    sc->word_len = 0;
    return ODIN3_OK;
}

/* Ends the current command (step 4): a blank one is skipped. */
static odin3_status end_command(abc_scan *sc) {
    odin3_status st = end_word(sc);
    if (st != ODIN3_OK || sc->n_words == 0) {
        return st;
    }
    const char **words =
        (const char **)odin3_arena_alloc(sc->ps->arena, sc->n_words * sizeof *words);
    abc_item *item = words != NULL ? odin3_vec_push(&sc->ps->raw) : NULL;
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < sc->n_words; i++) {
        words[i] = sc->words[i];
    }
    *item = (abc_item){words, sc->n_words, sc->line, 0, NULL};
    sc->n_words = 0;
    return ODIN3_OK;
}

static odin3_status end_line(abc_scan *sc) {
    odin3_status st = end_command(sc);
    sc->line++;
    sc->line_len = 0;
    sc->in_comment = false;
    return st;
}

static void word_add(abc_scan *sc, char ch) {
    sc->word[sc->word_len++] = ch;
}

/* `{` that does not start `{K}`: names `{`, up to ABC_SUBST_SHOW name bytes, and a closing `}`. */
static odin3_status brace_unknown(const abc_scan *sc, const uint8_t *at, size_t left) {
    size_t end = 1;
    while (end < left && end <= ABC_SUBST_SHOW && is_name_byte(at[end])) {
        end++;
    }
    if (end < left && at[end] == '}') {
        end++;
    }
    return abc_fail(sc->ps, sc->line, "unknown substitution '%.*s' (only {K} is defined)", (int)end,
                    (const char *)at);
}

/* `{` (step 5): `{K}` becomes K in decimal; anything else is an unknown substitution. */
static odin3_status scan_brace(abc_scan *sc) {
    const uint8_t *at = sc->bytes + sc->pos;
    size_t left = sc->len - sc->pos;
    if (left < ABC_K_TOKEN || at[1] != 'K' || at[2] != '}') {
        return brace_unknown(sc, at, left);
    }
    if (sc->ps->lut_k == 0) {
        return abc_fail(sc->ps, sc->line, "the script uses {K} but abc was given no --lut");
    }
    sc->ps->uses_k = true;
    sc->pos += ABC_K_TOKEN - 1;
    sc->line_len += ABC_K_TOKEN - 1;
    if (sc->line_len > ODIN3_ABC_MAX_LINE_BYTES) {
        return abc_fail(sc->ps, sc->line, "line longer than %d bytes", ODIN3_ABC_MAX_LINE_BYTES);
    }
    uint32_t k = sc->ps->lut_k; /* 2 .. 16: at most two digits */
    if (k >= ABC_DECIMAL) {
        word_add(sc, (char)('0' + k / ABC_DECIMAL));
    }
    word_add(sc, (char)('0' + k % ABC_DECIMAL));
    return ODIN3_OK;
}

/* A byte outside a comment (steps 1-6). */
static odin3_status scan_text(abc_scan *sc, uint8_t byte) {
    switch (byte) {
    case '#':
        sc->in_comment = true;
        return end_command(sc);
    case ';':
        return end_command(sc);
    case ' ':
    case '\t':
        return end_word(sc);
    case '"':
    case '\'':
        return abc_fail(sc->ps, sc->line, "quotes are not allowed in abc scripts");
    case '{':
        return scan_brace(sc);
    case '}':
        return abc_fail(sc->ps, sc->line, "unknown substitution '}' (only {K} is defined)");
    case '!':
        if (sc->word_len == 0) {
            return abc_fail(sc->ps, sc->line, "'!' (shell escape) is not allowed in abc scripts");
        }
        break;
    default:
        if (byte < ABC_PRINT_MIN || byte > ABC_PRINT_MAX) {
            return abc_fail(sc->ps, sc->line, "byte 0x%02x is not allowed outside a comment",
                            (unsigned)byte);
        }
        break;
    }
    word_add(sc, (char)byte);
    return ODIN3_OK;
}

/* One byte: line ends, NUL, the line cap, then comments and text. */
static odin3_status scan_byte(abc_scan *sc) {
    uint8_t byte = sc->bytes[sc->pos];
    if (byte == '\n') {
        return end_line(sc);
    }
    if (byte == '\r' && sc->pos + 1 < sc->len && sc->bytes[sc->pos + 1] == '\n') {
        return ODIN3_OK; /* the CR of a CRLF line end */
    }
    if (byte == '\0') {
        return abc_fail(sc->ps, sc->line, "abc script contains a NUL byte");
    }
    if (++sc->line_len > ODIN3_ABC_MAX_LINE_BYTES) {
        return abc_fail(sc->ps, sc->line, "line longer than %d bytes", ODIN3_ABC_MAX_LINE_BYTES);
    }
    return sc->in_comment ? ODIN3_OK : scan_text(sc, byte);
}

/* Steps 1-6 over every byte; the raw commands go to ps->raw. */
static odin3_status scan_all(abc_parse *ps, const abc_request *req) {
    abc_scan *sc = odin3_util_calloc(sizeof *sc);
    if (sc == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sc->ps = ps;
    sc->bytes = req->bytes;
    sc->len = req->len;
    sc->line = 1;
    odin3_status st = ODIN3_OK;
    for (sc->pos = 0; st == ODIN3_OK && sc->pos < sc->len; sc->pos++) {
        st = scan_byte(sc);
    }
    if (st == ODIN3_OK) {
        st = end_command(sc);
    }
    odin3_util_free(sc);
    return st;
}

/* --- aliases (step 7) ----------------------------------------------------------------------- */

/* The words of seg[0..len), split at blanks and tabs, into a new arena array; *n_words gets their
 * number (0: a blank segment, *words NULL). */
static odin3_status split_words(abc_parse *ps, odin3_bytes seg, const char ***words,
                                uint32_t *n_words) {
    const char *text = seg.ptr;
    uint32_t count = 0;
    for (size_t i = 0; i < seg.len; i++) {
        bool blank = text[i] == ' ' || text[i] == '\t';
        bool starts = !blank && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t');
        count += starts;
    }
    *words = NULL;
    *n_words = 0;
    if (count == 0) {
        return ODIN3_OK;
    }
    const char **out = (const char **)odin3_arena_alloc(ps->arena, count * sizeof *out);
    if (out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (uint32_t j = 0; j < count; j++) {
        at += strspn(text + at, " \t");
        size_t len = strcspn(text + at, " \t");
        len = len > seg.len - at ? seg.len - at : len;
        out[j] = odin3_arena_strndup(ps->arena, text + at, len);
        if (out[j] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        at += len;
    }
    *words = out;
    *n_words = count;
    return ODIN3_OK;
}

/* Splits an alias expansion at `;` into ps->segs (blank commands skipped), each like proto. */
static odin3_status split_alias(abc_parse *ps, const char *text, const abc_item *proto) {
    odin3_vec_clear(&ps->segs);
    const char *seg = text;
    for (;;) {
        size_t len = strcspn(seg, ";");
        abc_item item = *proto;
        odin3_status st = split_words(ps, (odin3_bytes){seg, len}, &item.words, &item.n_words);
        if (st != ODIN3_OK) {
            return st;
        }
        if (item.n_words > 0) {
            abc_item *slot = odin3_vec_push(&ps->segs);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = item;
        }
        if (seg[len] == '\0') {
            return ODIN3_OK;
        }
        seg += len + 1;
    }
}

/* CmdApplyAlias: the words after the alias name go to the end of its last command. */
static odin3_status append_extra(abc_parse *ps, abc_item *last, const abc_item *item) {
    if (item->n_words < 2) {
        return ODIN3_OK;
    }
    uint32_t total = last->n_words + item->n_words - 1;
    const char **words = (const char **)odin3_arena_alloc(ps->arena, total * sizeof *words);
    if (words == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < last->n_words; i++) {
        words[i] = last->words[i];
    }
    for (uint32_t i = 1; i < item->n_words; i++) {
        words[last->n_words + i - 1] = item->words[i];
    }
    last->words = words;
    last->n_words = total;
    return ODIN3_OK;
}

/* Replaces item, whose word 0 names alias, by the alias's commands on the worklist (step 7). */
static odin3_status expand(abc_parse *ps, const abc_item *item, const odin3_abc_alias *alias) {
    const char *outer = item->alias;
    if (item->depth >= ODIN3_ABC_MAX_ALIAS_DEPTH) {
        return abc_fail(ps, item->line, "alias '%s' expands more than %d levels deep",
                        outer != NULL ? outer : alias->name, ODIN3_ABC_MAX_ALIAS_DEPTH);
    }
    if (outer == NULL) {
        outer = odin3_arena_strndup(ps->arena, alias->name, strlen(alias->name));
        if (outer == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    abc_item proto = {NULL, 0, item->line, item->depth + 1, outer};
    odin3_status st = split_alias(ps, alias->expansion, &proto);
    if (st == ODIN3_OK && ps->segs.len > 0) {
        st = append_extra(ps, odin3_vec_at(&ps->segs, ps->segs.len - 1), item);
    }
    for (size_t i = ps->segs.len; st == ODIN3_OK && i > 0; i--) {
        abc_item *slot = odin3_vec_push(&ps->stack);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *(const abc_item *)odin3_vec_cat(&ps->segs, i - 1);
    }
    return st;
}

/* --- one expanded command (steps 8-11) ------------------------------------------------------ */

/* Step 8: the allow-list (§7.5), with its class's ending. */
static odin3_status check_allowed(const abc_parse *ps, const abc_item *item,
                                  const odin3_abc_command_info *row) {
    const char *ending = "is not an allowed ABC command (3D spec §7.5)";
    if (row != NULL) {
        switch (row->cls) {
        case ODIN3_ABC_CLASS_FILES:
            ending = "is not allowed: abc scripts cannot read or write files";
            break;
        case ODIN3_ABC_CLASS_PROCESS:
            ending = "is not allowed: abc scripts cannot control the ABC process";
            break;
        case ODIN3_ABC_CLASS_LATCHES:
            ending = "is not allowed: it retimes or changes initial values (PHASE3 #12)";
            break;
        case ODIN3_ABC_CLASS_CRASH:
            ending = "is not allowed: it crashes ABC at the pin (a3001b72)";
            break;
        default:
            return ODIN3_OK; /* OPT, MAP, GIA, PRINT */
        }
    }
    abc_from from = from_alias(item);
    return abc_fail(ps, item->line, "abc command '%s'%s%s%s %s", item->words[0], from.pre,
                    from.name, from.post, ending);
}

/* Step 9 for an option word: every letter of the cluster against file_opts and crash_opts. */
static odin3_status check_option(const abc_parse *ps, const abc_item *item,
                                 const odin3_abc_command_info *row, const char *word) {
    abc_from from = from_alias(item);
    for (const char *at = word + 1; *at != '\0'; at++) {
        if (strchr(row->file_opts, *at) != NULL) {
            return abc_fail(ps, item->line,
                            "option '-%c' of '%s'%s%s%s reads or writes a file or takes a string "
                            "and is not allowed",
                            *at, row->name, from.pre, from.name, from.post);
        }
        if (strchr(row->crash_opts, *at) != NULL) {
            return abc_fail(ps, item->line,
                            "option '-%c' of '%s'%s%s%s crashes ABC at the pin (a3001b72) and is "
                            "not allowed",
                            *at, row->name, from.pre, from.name, from.post);
        }
    }
    return ODIN3_OK;
}

/* Step 9: each argument is an option or a number (echo: any printable word). */
static odin3_status check_args(const abc_parse *ps, const abc_item *item,
                               const odin3_abc_command_info *row) {
    bool echo = strcmp(row->name, "echo") == 0;
    for (uint32_t i = 1; i < item->n_words; i++) {
        const char *word = item->words[i];
        odin3_status st = ODIN3_OK;
        if (is_option(word)) {
            st = check_option(ps, item, row, word);
        } else if (!is_number(word) && !(echo && is_echo_word(word))) {
            abc_from from = from_alias(item);
            st = abc_fail(ps, item->line,
                          "argument '%s' of '%s'%s%s%s is neither an option nor a number", word,
                          row->name, from.pre, from.name, from.post);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Whether letter occurs in an option word of item, anywhere (the conservative reading). */
static bool letter_any(const abc_item *item, const char *letters) {
    for (uint32_t i = 1; i < item->n_words; i++) {
        const char *word = item->words[i];
        if (is_option(word) && strpbrk(word + 1, letters) != NULL) {
            return true;
        }
    }
    return false;
}

/* Whether letter ends on in a command that takes no option values: ABC's flags toggle, and its
 * getopt stops at the first word that is not an option. */
static bool letter_on(const abc_item *item, char letter) {
    bool on = false;
    for (uint32_t i = 1; i < item->n_words && is_option(item->words[i]); i++) {
        for (const char *at = item->words[i] + 1; *at != '\0'; at++) {
            on ^= *at == letter;
        }
    }
    return on;
}

/* The state a mapper leaves: mapped, or an AIG when a balancing option is on (see the header). */
static uint8_t mapper_state(const abc_item *item, const odin3_abc_command_info *row) {
    const char *balancing = row->effect == ODIN3_ABC_FX_GIA_MAPPED ? "gxy" : "gxyu";
    return letter_any(item, balancing) ? ABC_AIG : ABC_MAPPED;
}

/* &get: the & space keeps the network's mapping only with -m and without -c. */
static uint8_t get_state(const abc_parse *ps, const abc_item *item) {
    bool keeps = ps->ntk == ABC_MAPPED && letter_on(item, 'm') && !letter_on(item, 'c');
    return keeps ? ABC_MAPPED : ABC_AIG;
}

/* §7.5's effect of an allowed command on the two spaces (step 11). */
static void apply_effect(abc_parse *ps, const abc_item *item, const odin3_abc_command_info *row) {
    switch (row->effect) {
    case ODIN3_ABC_FX_NTK_AIG:
        ps->ntk = ABC_AIG;
        ps->cur = ODIN3_ABC_SPACE_NTK;
        break;
    case ODIN3_ABC_FX_NTK_MAPPED:
        ps->ntk = mapper_state(item, row);
        ps->cur = ODIN3_ABC_SPACE_NTK;
        break;
    case ODIN3_ABC_FX_NTK_PUT:
        ps->ntk = ps->gia;
        ps->cur = ODIN3_ABC_SPACE_NTK;
        break;
    case ODIN3_ABC_FX_NTK_KEEP:
        ps->cur = ODIN3_ABC_SPACE_NTK;
        break;
    case ODIN3_ABC_FX_GIA_GET:
        ps->gia = get_state(ps, item);
        ps->cur = ODIN3_ABC_SPACE_GIA;
        break;
    case ODIN3_ABC_FX_GIA_AIG:
        ps->gia = ABC_AIG;
        ps->cur = ODIN3_ABC_SPACE_GIA;
        break;
    case ODIN3_ABC_FX_GIA_MAPPED:
        ps->gia = mapper_state(item, row);
        ps->cur = ODIN3_ABC_SPACE_GIA;
        break;
    case ODIN3_ABC_FX_GIA_KEEP:
        ps->cur = ODIN3_ABC_SPACE_GIA;
        break;
    default:
        break; /* printing changes no network */
    }
}

/* Step 11: the & space must hold a network, and &if needs it unmapped; then the effect. */
static odin3_status check_space(abc_parse *ps, const abc_item *item,
                                const odin3_abc_command_info *row) {
    abc_from from = from_alias(item);
    if (row->name[0] == '&' && row->effect != ODIN3_ABC_FX_GIA_GET && ps->gia == ABC_EMPTY) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs a network in the & space, which the BLIF exchange "
                        "starts empty; add '&get'",
                        row->name, from.pre, from.name, from.post);
    }
    if (row->effect == ODIN3_ABC_FX_GIA_MAPPED && ps->gia == ABC_MAPPED) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs an unmapped network in the & space, which is mapped "
                        "here; add '&st'",
                        row->name, from.pre, from.name, from.post);
    }
    apply_effect(ps, item, row);
    return ODIN3_OK;
}

/* Appends the checked command: its words joined by one blank (step 12). */
static odin3_status emit(abc_parse *ps, const abc_item *item) {
    size_t bytes = 0;
    for (uint32_t i = 0; i < item->n_words; i++) {
        bytes += strlen(item->words[i]) + 1;
    }
    char *line = odin3_arena_alloc(ps->arena, bytes);
    odin3_abc_cmd *cmd = line != NULL ? odin3_vec_push(&ps->cmds) : NULL;
    if (cmd == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < item->n_words; i++) {
        size_t len = strlen(item->words[i]);
        memcpy(line + at, item->words[i], len);
        at += len;
        line[at++] = i + 1 < item->n_words ? ' ' : '\0';
    }
    *cmd = (odin3_abc_cmd){line, item->line, item->alias};
    return ODIN3_OK;
}

/* Steps 8-11 for one expanded command, then step 12's output. */
static odin3_status check_command(abc_parse *ps, const abc_item *item) {
    const odin3_abc_command_info *row = odin3_abc_command_find(item->words[0]);
    odin3_status st = check_allowed(ps, item, row);
    if (st == ODIN3_OK) {
        st = check_args(ps, item, row);
    }
    if (st == ODIN3_OK && ps->cmds.len >= ODIN3_ABC_MAX_COMMANDS) {
        st = abc_fail(ps, item->line, "script expands to more than %d commands",
                      ODIN3_ABC_MAX_COMMANDS);
    }
    if (st == ODIN3_OK) {
        st = check_space(ps, item, row);
    }
    return st == ODIN3_OK ? emit(ps, item) : st;
}

/* Steps 7-11 over every raw command, in order, through the alias worklist. */
static odin3_status check_all(abc_parse *ps) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ps->raw.len; i++) {
        abc_item *slot = odin3_vec_push(&ps->stack);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *(const abc_item *)odin3_vec_cat(&ps->raw, i);
        while (st == ODIN3_OK && ps->stack.len > 0) {
            abc_item item = *(const abc_item *)odin3_vec_cat(&ps->stack, ps->stack.len - 1);
            odin3_vec_pop(&ps->stack);
            const odin3_abc_alias *alias = odin3_abc_alias_find(item.words[0]);
            st = alias != NULL ? expand(ps, &item, alias) : check_command(ps, &item);
        }
    }
    return st;
}

/* Step 11's end: the current space must be mapped. */
static odin3_status check_end(abc_parse *ps) {
    uint8_t state = ps->cur == ODIN3_ABC_SPACE_GIA ? ps->gia : ps->ntk;
    if (state != ABC_MAPPED) {
        return abc_fail(ps, ABC_NO_LINE,
                        "the script leaves the network unmapped; end it with a LUT mapper such as "
                        "'if -K {K}'");
    }
    return ODIN3_OK;
}

/* --- the entry point ------------------------------------------------------------------------ */

static void parse_free(abc_parse *ps) {
    odin3_vec_free(&ps->raw);
    odin3_vec_free(&ps->stack);
    odin3_vec_free(&ps->segs);
    odin3_vec_free(&ps->cmds);
    odin3_arena_destroy(ps->arena);
    ps->arena = NULL;
}

/* Fills *out from a parse that passed; the script takes the arena. */
static odin3_status finish(abc_parse *ps, const abc_request *req, odin3_abc_script *out) {
    size_t count = ps->cmds.len;
    odin3_abc_cmd *cmds = odin3_arena_alloc(ps->arena, count * sizeof *cmds);
    if (cmds == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (count > 0) {
        memcpy(cmds, ps->cmds.data, count * sizeof *cmds);
    }
    odin3_sha256 sha;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, req->bytes, req->len);
    odin3_sha256_final(&sha, digest);
    odin3_sha256_hex(digest, out->sha256_hex);
    out->cmds = cmds;
    out->n_cmds = (uint32_t)count;
    out->uses_k = ps->uses_k;
    out->final_space = ps->cur;
    out->arena = ps->arena;
    ps->arena = NULL;
    return ODIN3_OK;
}

/* The checks, in memory: size, the byte pass, the command pass, the output. */
static odin3_status parse_bytes(const abc_request *req, odin3_abc_script *out) {
    abc_parse ps = {.given = req->given, .lut_k = req->lut_k};
    if (req->len > ODIN3_ABC_MAX_SCRIPT_BYTES) {
        return abc_fail(&ps, ABC_NO_LINE, "abc script is larger than %d bytes",
                        ODIN3_ABC_MAX_SCRIPT_BYTES);
    }
    ps.ntk = ABC_AIG;
    ps.gia = req->exchange == ODIN3_ABC_EXCHANGE_BLIF ? ABC_EMPTY : ABC_AIG;
    ps.cur = ODIN3_ABC_SPACE_NTK;
    odin3_vec_init(&ps.raw, sizeof(abc_item));
    odin3_vec_init(&ps.stack, sizeof(abc_item));
    odin3_vec_init(&ps.segs, sizeof(abc_item));
    odin3_vec_init(&ps.cmds, sizeof(odin3_abc_cmd));
    ps.arena = odin3_arena_create(ABC_ARENA_CHUNK);
    odin3_status st = ps.arena != NULL ? scan_all(&ps, req) : ODIN3_ERR_NO_MEMORY;
    if (st == ODIN3_OK) {
        st = check_all(&ps);
    }
    if (st == ODIN3_OK) {
        st = check_end(&ps);
    }
    if (st == ODIN3_OK) {
        odin3_abc_script result = {0};
        st = finish(&ps, req, &result);
        if (st == ODIN3_OK) {
            *out = result;
        }
    }
    parse_free(&ps);
    return st;
}

/* Reads at most ODIN3_ABC_MAX_SCRIPT_BYTES + 1 bytes of path into a new buffer (enough to see
 * that a larger file is too large). */
static odin3_status read_script(const char *path, uint8_t **bytes, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot read the script: %s", path, strerror(errno));
        return ODIN3_ERR_IO;
    }
    uint8_t *buf = odin3_util_malloc((size_t)ODIN3_ABC_MAX_SCRIPT_BYTES + 1);
    if (buf == NULL) {
        (void)fclose(file);
        return ODIN3_ERR_NO_MEMORY;
    }
    errno = 0;
    size_t got = fread(buf, 1, (size_t)ODIN3_ABC_MAX_SCRIPT_BYTES + 1, file);
    bool failed = ferror(file) != 0;
    int err = errno != 0 ? errno : EIO;
    (void)fclose(file);
    if (failed) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot read the script: %s", path, strerror(err));
        odin3_util_free(buf);
        return ODIN3_ERR_IO;
    }
    *bytes = buf;
    *len = got;
    return ODIN3_OK;
}

/* The request is usable: a source with a name and bytes or a path, K 0 or in range, an exchange. */
static bool request_valid(const odin3_abc_script_src *src, const abc_request *req) {
    if (req->given == NULL || (src->text.ptr == NULL && src->resolved == NULL)) {
        return false;
    }
    bool k_ok =
        req->lut_k == 0 || (req->lut_k >= ODIN3_ABC_LUT_MIN && req->lut_k <= ODIN3_LUT_MAX_WIDTH);
    return k_ok && req->exchange <= ODIN3_ABC_EXCHANGE_BLIF;
}

odin3_status odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k,
                                    uint32_t exchange, odin3_abc_script *out) {
    abc_request req = {NULL, NULL, 0, lut_k, exchange};
    if (src != NULL) {
        req = (abc_request){src->given, src->text.ptr, src->text.len, lut_k, exchange};
    }
    if (src == NULL || out == NULL || !request_valid(src, &req)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_script_parse: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint8_t *owned = NULL;
    if (src->text.ptr == NULL) {
        odin3_status st = read_script(src->resolved, &owned, &req.len);
        if (st != ODIN3_OK) {
            return st;
        }
        req.bytes = owned;
    }
    odin3_status st = parse_bytes(&req, out);
    odin3_util_free(owned);
    return st;
}

void odin3_abc_script_free(odin3_abc_script *script) {
    if (script == NULL) {
        return;
    }
    odin3_arena_destroy(script->arena);
    *script = (odin3_abc_script){0};
}

const odin3_abc_cmd *odin3_abc_script_cmd(const odin3_abc_script *script, uint32_t index) {
    if (script == NULL || index >= script->n_cmds) {
        return NULL;
    }
    return &script->cmds[index];
}
