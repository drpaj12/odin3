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
 *
 * Task 11 review fix round 1 (rulings C2, I1 and M1, M5):
 *  - every number word lies in 0 .. ODIN3_ABC_MAX_NUMBER, read both as atoi and as atof read it
 *    (large values allocate without bound or wrap through atoi), and no number starts with `-`
 *    (ABC's getopt would read it as an option cluster);
 *  - print_kmap, print_level, print_factor, print_io and print_delay take no positional words
 *    (they name nodes; a non-node name crashes ABC; the final review's addendum);
 *  - the BLIF start is a logic network (ABC_LOGIC): &get -m keeps its mapping, so &if refuses it,
 *    yet it does not end a script as mapped;
 *  - choices: &if refuses the choices a plain &get copied from the network (Gia_ManChoiceLevel,
 *    giaIf.c:836), and print_xcut a network with choices (Abc_NtkCrossCut, abcUtil.c:2052).
 *
 * PR 3D-4's final review (rulings C1, C2, I1; agent defaults, PHASE3 decision 27):
 *  - linked ABC maps LUTs of at most ODIN3_ABC_MAX_LUT_LINKED (12) inputs: {K} above it is
 *    refused, if/&if -K must be 2..12, and &get -m may not copy the BLIF network's nodes (of any
 *    width) as LUTs; wider mappings crash &st and &synch2 (dauGia.c:451), in Release too;
 *  - value ranges a letter list cannot express (k_abc_ranges): -E at least 1, refactor -N at least
 *    2, each letter alone as `-X <n>` (a valued letter eats the next word, read as 0);
 *  - mfs and print_xcut need a network some command rebuilt, not the BLIF one (dangling nodes).
 */
#include "passes/abc/script.h"

#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
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
    ABC_CAP_DIGITS = 7,         /* the digits of ODIN3_ABC_MAX_NUMBER */
    ABC_EXP_SATURATE = 1000000, /* any exponent past the line cap's digits */
    ABC_ARENA_CHUNK = 16384
};

/* The smallest and largest numbers of ABC_CAP_DIGITS digits. */
enum { ABC_CAP_DIGITS_MIN = 1000000, ABC_CAP_DIGITS_MAX = 9999999 };
_Static_assert((long)ODIN3_ABC_MAX_NUMBER >= (long)ABC_CAP_DIGITS_MIN &&
                   (long)ODIN3_ABC_MAX_NUMBER <= (long)ABC_CAP_DIGITS_MAX,
               "ABC_CAP_DIGITS is the number of digits of ODIN3_ABC_MAX_NUMBER");

/* A network space's state (§7.3 step 11). ABC_LOGIC is the BLIF start: a logic network that has
 * a mapping for &get -m and &if, but not one the script made (review M1). */
enum { ABC_EMPTY = 0, ABC_AIG = 1, ABC_MAPPED = 2, ABC_LOGIC = 3, ABC_CELLMAPPED = 4 };

/* The choices a space holds (fix round 1, ruling I1): none; native ones (dch, &dch, &synch2,
 * &get -s), which &if maps; or ones a plain &get copied, which &if cannot. */
enum { ABC_CHOICES_NONE = 0, ABC_CHOICES_NATIVE = 1, ABC_CHOICES_COPIED = 2 };

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
    odin3_abc_limits lim; /* the caps of this parse (the defaults, or a test's lower ones) */
    uint32_t lut_k;
    bool uses_k;
    bool gates;          /* abc --gates: the script must end ABC_CELLMAPPED */
    uint8_t ntk, gia;    /* ABC_EMPTY, ABC_AIG, ABC_MAPPED, ABC_LOGIC or ABC_CELLMAPPED */
    uint8_t cur;         /* odin3_abc_space: the space of the last command that changed a network */
    bool ntk_choices;    /* the network holds choices */
    uint8_t gia_choices; /* the & space's ABC_CHOICES_* */
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
    bool gates;
} abc_request;

/* The ` (from alias '<a>')` that follows a command name in a message, or three empty strings. */
typedef struct abc_from {
    const char *pre, *name, *post;
} abc_from;

/* --- limits -------------------------------------------------------------------------------- */

static const odin3_abc_limits k_abc_default_limits = {
    ODIN3_ABC_MAX_SCRIPT_BYTES, ODIN3_ABC_MAX_LINE_BYTES, ODIN3_ABC_MAX_COMMANDS,
    ODIN3_ABC_MAX_WORDS, ODIN3_ABC_MAX_ALIAS_DEPTH};

/* The caps every parse copies at its start (odin3_abc_test_set_limits lowers them). */
static odin3_abc_limits abc_limits = {ODIN3_ABC_MAX_SCRIPT_BYTES, ODIN3_ABC_MAX_LINE_BYTES,
                                      ODIN3_ABC_MAX_COMMANDS, ODIN3_ABC_MAX_WORDS,
                                      ODIN3_ABC_MAX_ALIAS_DEPTH};

static uint32_t lowered(uint32_t value, uint32_t cap) {
    return value < cap ? value : cap;
}

/* The pass-level caps (Task 14). */
static const odin3_abc_run_limits k_run_default_limits = {.domains = ODIN3_ABC_MAX_DOMAINS,
                                                          .aig_objs = ODIN3_ABC_MAX_AIG_OBJS,
                                                          .namemap_objs =
                                                              ODIN3_ABC_NAMEMAP_MAX_OBJS};
static odin3_abc_run_limits run_limits = {.domains = ODIN3_ABC_MAX_DOMAINS,
                                          .aig_objs = ODIN3_ABC_MAX_AIG_OBJS,
                                          .namemap_objs = ODIN3_ABC_NAMEMAP_MAX_OBJS};

const odin3_abc_run_limits *odin3_abc_run_limits_get(void) {
    return &run_limits;
}

/* A pass-level cap: 0 keeps the default, else the value clamped to it. */
static uint32_t pass_cap(uint32_t value, uint32_t dflt) {
    return value == 0 ? dflt : lowered(value, dflt);
}

void odin3_abc_test_set_run_limits(const odin3_abc_run_limits *limits) {
    if (limits == NULL) {
        run_limits = k_run_default_limits;
        return;
    }
    run_limits.domains = pass_cap(limits->domains, k_run_default_limits.domains);
    run_limits.aig_objs = pass_cap(limits->aig_objs, k_run_default_limits.aig_objs);
    run_limits.namemap_objs = pass_cap(limits->namemap_objs, k_run_default_limits.namemap_objs);
    run_limits.prov_edges = limits->prov_edges;
}

void odin3_abc_test_set_limits(const odin3_abc_limits *limits) {
    if (limits == NULL) {
        abc_limits = k_abc_default_limits;
        return;
    }
    abc_limits.script_bytes = lowered(limits->script_bytes, k_abc_default_limits.script_bytes);
    abc_limits.line_bytes = lowered(limits->line_bytes, k_abc_default_limits.line_bytes);
    abc_limits.commands = lowered(limits->commands, k_abc_default_limits.commands);
    abc_limits.words = lowered(limits->words, k_abc_default_limits.words);
    abc_limits.alias_depth = lowered(limits->alias_depth, k_abc_default_limits.alias_depth);
}

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

/* A number word's digits before and after its point. */
typedef struct abc_mantissa {
    const char *int_part;
    size_t int_len;
    const char *frac;
    size_t frac_len;
} abc_mantissa;

static char mantissa_at(const abc_mantissa *mt, size_t index) {
    if (index < mt->int_len) {
        return mt->int_part[index];
    }
    return mt->frac[index - mt->int_len];
}

/* The decimal exponent at `at` (after the `e`), saturated at +/-ABC_EXP_SATURATE. */
static long exponent_of(const char *at) {
    bool negative = *at == '-';
    at += *at == '-' || *at == '+';
    long exp = 0;
    for (; is_digit(*at); at++) {
        exp = exp * ABC_DECIMAL + (*at - '0');
        exp = exp > ABC_EXP_SATURATE ? ABC_EXP_SATURATE : exp;
    }
    return negative ? -exp : exp;
}

/* Whether the ABC_CAP_DIGITS digits from index on (zeros past the end), and every nonzero digit
 * after them, keep the number at most ODIN3_ABC_MAX_NUMBER. */
static bool leading_in_cap(const abc_mantissa *mt, size_t index) {
    size_t total = mt->int_len + mt->frac_len;
    long value = 0;
    for (size_t i = index; i < index + ABC_CAP_DIGITS; i++) {
        value = value * ABC_DECIMAL + (i < total ? mantissa_at(mt, i) - '0' : 0);
    }
    for (size_t i = index + ABC_CAP_DIGITS; i < total && value == ODIN3_ABC_MAX_NUMBER; i++) {
        value += mantissa_at(mt, i) != '0';
    }
    return value <= ODIN3_ABC_MAX_NUMBER;
}

/* atof's reading: the value, exactly in decimal, is at most ODIN3_ABC_MAX_NUMBER. */
static bool value_in_cap(const abc_mantissa *mt, long exp) {
    size_t total = mt->int_len + mt->frac_len;
    size_t lead = 0;
    while (lead < total && mantissa_at(mt, lead) == '0') {
        lead++;
    }
    if (lead == total) {
        return true; /* zero */
    }
    long int_digits = (long)mt->int_len - (long)lead + exp;
    if (int_digits != ABC_CAP_DIGITS) {
        return int_digits < ABC_CAP_DIGITS;
    }
    return leading_in_cap(mt, lead);
}

/* atoi's reading: the digits before the point, as an integer, are at most ODIN3_ABC_MAX_NUMBER. */
static bool int_part_in_cap(const abc_mantissa *mt) {
    size_t lead = strspn(mt->int_part, "0"); /* a non-digit always follows the integer digits */
    size_t len = mt->int_len - lead;
    if (len != ABC_CAP_DIGITS) {
        return len < ABC_CAP_DIGITS;
    }
    abc_mantissa int_only = {mt->int_part, mt->int_len, NULL, 0};
    return leading_in_cap(&int_only, lead);
}

/* Reads a number word (is_number holds) into its mantissa and decimal exponent. */
static long read_number(const char *word, abc_mantissa *mt) {
    const char *at = word + (word[0] == '+' || word[0] == '-');
    *mt = (abc_mantissa){at, strspn(at, "0123456789"), at, 0};
    const char *rest = at + mt->int_len;
    if (*rest == '.') {
        mt->frac = rest + 1;
        mt->frac_len = strspn(mt->frac, "0123456789");
        rest = mt->frac + mt->frac_len;
    }
    return *rest == 'e' || *rest == 'E' ? exponent_of(rest + 1) : 0;
}

/*
 * Step 9's cap (fix round 1, ruling C2) on a number word: it must lie in 0 ..
 * ODIN3_ABC_MAX_NUMBER both as atoi reads it (its integer digits; atoi wraps past INT_MAX) and as
 * atof reads it (all of it). A word that starts with `-` is refused: besides being negative or
 * zero, ABC's getopt would read it as an option cluster (review M5).
 */
static bool number_in_cap(const char *word) {
    if (word[0] == '-') {
        return false;
    }
    abc_mantissa mt;
    long exp = read_number(word, &mt);
    return int_part_in_cap(&mt) && value_in_cap(&mt, exp);
}

/* The digits from index on, count of them (zeros past the end), as an integer (count <= 7). */
static long digits_value(const abc_mantissa *mt, size_t index, long count) {
    size_t total = mt->int_len + mt->frac_len;
    long value = 0;
    for (size_t i = index; i < index + (size_t)count; i++) {
        value = value * ABC_DECIMAL + (i < total ? mantissa_at(mt, i) - '0' : 0);
    }
    return value;
}

/* The value atoi reads from a number word within the cap: its integer digits. */
static long atoi_value(const char *word) {
    abc_mantissa mt;
    (void)read_number(word, &mt);
    return digits_value(&mt, 0, (long)mt.int_len);
}

/* The integer part of the value atof reads from a number word within the cap, exactly. */
static long atof_floor(const char *word) {
    abc_mantissa mt;
    long exp = read_number(word, &mt);
    size_t total = mt.int_len + mt.frac_len;
    size_t lead = 0;
    while (lead < total && mantissa_at(&mt, lead) == '0') {
        lead++;
    }
    long int_digits = (long)mt.int_len - (long)lead + exp;
    return lead == total || int_digits <= 0 ? 0 : digits_value(&mt, lead, int_digits);
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
    if (sc->n_words == sc->ps->lim.words) {
        return abc_fail(sc->ps, sc->line, "command has more than %" PRIu32 " words",
                        sc->ps->lim.words);
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
    if (sc->ps->lut_k > ODIN3_ABC_MAX_LUT_LINKED) {
        return abc_fail(sc->ps, sc->line,
                        "the script uses {K} = %" PRIu32 ", but linked ABC maps LUTs of at most "
                        "%d inputs ('&st' and '&synch2' crash on wider ones at the pin)",
                        sc->ps->lut_k, ODIN3_ABC_MAX_LUT_LINKED);
    }
    sc->ps->uses_k = true;
    sc->pos += ABC_K_TOKEN - 1;
    sc->line_len += ABC_K_TOKEN - 1;
    if (sc->line_len > sc->ps->lim.line_bytes) {
        return abc_fail(sc->ps, sc->line, "line longer than %" PRIu32 " bytes",
                        sc->ps->lim.line_bytes);
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
    if (++sc->line_len > sc->ps->lim.line_bytes) {
        return abc_fail(sc->ps, sc->line, "line longer than %" PRIu32 " bytes",
                        sc->ps->lim.line_bytes);
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
    if (item->depth >= ps->lim.alias_depth) {
        return abc_fail(ps, item->line, "alias '%s' expands more than %" PRIu32 " levels deep",
                        outer != NULL ? outer : alias->name, ps->lim.alias_depth);
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

/* Step 9 for a word that is not an option: echo prints any printable word; every other command
 * takes a number from 0 to ODIN3_ABC_MAX_NUMBER (fix round 1, ruling C2). */
static odin3_status check_value(const abc_parse *ps, const abc_item *item,
                                const odin3_abc_command_info *row, const char *word) {
    if (strcmp(row->name, "echo") == 0 && is_echo_word(word)) {
        return ODIN3_OK;
    }
    abc_from from = from_alias(item);
    if (!is_number(word)) {
        return abc_fail(ps, item->line,
                        "argument '%s' of '%s'%s%s%s is neither an option nor a number", word,
                        row->name, from.pre, from.name, from.post);
    }
    if (!number_in_cap(word)) {
        return abc_fail(ps, item->line, "argument '%s' of '%s'%s%s%s is not a number from 0 to %d",
                        word, row->name, from.pre, from.name, from.post, ODIN3_ABC_MAX_NUMBER);
    }
    return ODIN3_OK;
}

/* Commands that may take no positional words (fix round 1, ruling I1; the final review's
 * addendum): they name nodes, or for print_kmap with two words the 6-variable functions it
 * prints, and a name that is no node, or a latch output or output, crashes ABC at the pin
 * (Abc_NodeMffcSize, Abc_NodeIsConst, Abc_NodePrintFactor, Abc_Show6VarFunc; SIGSEGV in Release
 * for print_kmap). A batch flow has no use for a numeric node name. */
static const char *const k_abc_no_positional[] = {"print_kmap", "print_level", "print_factor",
                                                  "print_io", "print_delay"};

static odin3_status check_positional(const abc_parse *ps, const abc_item *item,
                                     const odin3_abc_command_info *row, uint32_t n_positional) {
    for (size_t i = 0; n_positional > 0 && i < sizeof k_abc_no_positional / sizeof(char *); i++) {
        if (strcmp(k_abc_no_positional[i], row->name) == 0) {
            abc_from from = from_alias(item);
            return abc_fail(ps, item->line,
                            "abc command '%s'%s%s%s with an argument is not allowed: it crashes "
                            "ABC at the pin (a3001b72)",
                            row->name, from.pre, from.name, from.post);
        }
    }
    return ODIN3_OK;
}

/* The reason a value range exists, for its message. */
static const char k_abc_why_lut[] = "linked ABC maps LUTs of at most 12 inputs ('&st' and "
                                    "'&synch2' crash on wider ones at the pin)";
static const char k_abc_why_crash[] = "a smaller value crashes ABC at the pin (a3001b72)";

/* An option whose value must lie in [min, max] (final review C1 and I1). ABC's getopt lets a
 * valued letter eat the next word, and atoi or atof of an option word is 0, so the letter must
 * stand alone as `-<letter> <number>`. */
typedef struct abc_range {
    const char *name;
    char letter;
    bool atof_reads; /* the value is a float (atof), else an integer (atoi) */
    long min, max;
    const char *why;
} abc_range;

static const abc_range k_abc_ranges[] = {
    {"if", 'K', false, ODIN3_ABC_LUT_MIN, ODIN3_ABC_MAX_LUT_LINKED, k_abc_why_lut},
    {"&if", 'K', false, ODIN3_ABC_LUT_MIN, ODIN3_ABC_MAX_LUT_LINKED, k_abc_why_lut},
    {"if", 'E', true, 1, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash},
    {"&if", 'E', true, 1, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash},
    {"refactor", 'N', false, 2, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash},
    {"&nf", 'C', false, 2, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash},
    {"&nf", 'F', false, 1, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash},
    {"&nf", 'A', false, 1, ODIN3_ABC_MAX_NUMBER, k_abc_why_crash}};

/* Whether words[at] is `-<letter>` alone, followed by a number word in the range. */
static bool range_holds(const abc_item *item, uint32_t at, const abc_range *rule) {
    const char *word = item->words[at];
    if (word[1] != rule->letter || word[2] != '\0' || at + 1 >= item->n_words) {
        return false;
    }
    const char *value = item->words[at + 1];
    if (!is_number(value) || !number_in_cap(value)) {
        return false;
    }
    /* min and max are integers, so value >= min exactly when floor(value) >= min; every atof
     * rule's max is the number cap, which number_in_cap already holds exactly */
    long read = rule->atof_reads ? atof_floor(value) : atoi_value(value);
    return read >= rule->min && read <= rule->max;
}

/* Step 9's value ranges for one rule over the command's option words. */
static odin3_status check_range(const abc_parse *ps, const abc_item *item, const abc_range *rule) {
    for (uint32_t i = 1; i < item->n_words; i++) {
        const char *word = item->words[i];
        if (!is_option(word) || strchr(word + 1, rule->letter) == NULL ||
            range_holds(item, i, rule)) {
            continue;
        }
        abc_from from = from_alias(item);
        return abc_fail(ps, item->line,
                        "option '-%c' of '%s'%s%s%s must stand alone as '-%c <n>' with <n> from "
                        "%ld to %ld: %s",
                        rule->letter, rule->name, from.pre, from.name, from.post, rule->letter,
                        rule->min, rule->max, rule->why);
    }
    return ODIN3_OK;
}

static odin3_status check_ranges(const abc_parse *ps, const abc_item *item,
                                 const odin3_abc_command_info *row) {
    for (size_t i = 0; i < sizeof k_abc_ranges / sizeof k_abc_ranges[0]; i++) {
        if (strcmp(k_abc_ranges[i].name, row->name) == 0) {
            odin3_status st = check_range(ps, item, &k_abc_ranges[i]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* Step 9: each argument is an option without a file, string or crash letter, or a value. */
static odin3_status check_args(const abc_parse *ps, const abc_item *item,
                               const odin3_abc_command_info *row) {
    uint32_t n_positional = 0;
    for (uint32_t i = 1; i < item->n_words; i++) {
        const char *word = item->words[i];
        bool option = is_option(word);
        odin3_status st =
            option ? check_option(ps, item, row, word) : check_value(ps, item, row, word);
        if (st != ODIN3_OK) {
            return st;
        }
        n_positional += !option;
    }
    odin3_status st = check_positional(ps, item, row, n_positional);
    return st == ODIN3_OK ? check_ranges(ps, item, row) : st;
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

/* &get: the & space keeps the network's mapping (ABC_MAPPED, or the BLIF start's ABC_LOGIC)
 * only with -m and without -c. */
static uint8_t get_state(const abc_parse *ps, const abc_item *item) {
    bool has_mapping = ps->ntk == ABC_MAPPED || ps->ntk == ABC_LOGIC;
    bool keeps = has_mapping && letter_on(item, 'm') && !letter_on(item, 'c');
    return keeps ? ps->ntk : ABC_AIG;
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
    case ODIN3_ABC_FX_NTK_CELLMAPPED:
        ps->ntk = ABC_CELLMAPPED;
        ps->cur = ODIN3_ABC_SPACE_NTK;
        break;
    case ODIN3_ABC_FX_GIA_CELLMAPPED:
        ps->gia = ABC_CELLMAPPED;
        ps->cur = ODIN3_ABC_SPACE_GIA;
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

/* &get copies the network's choices: natively with -s, else as a copy &if cannot map. */
static uint8_t get_choices(const abc_parse *ps, const abc_item *item) {
    if (!ps->ntk_choices) {
        return ABC_CHOICES_NONE;
    }
    return letter_on(item, 's') ? ABC_CHOICES_NATIVE : ABC_CHOICES_COPIED;
}

/*
 * Choices (fix round 1, ruling I1), read at the pin: dch makes them in the network, &dch and
 * &synch2 in the & space; &put carries the & space's choices to the network and &get copies the
 * network's; every other command that rebuilds a space drops them (strash, balance, dc2, fraig,
 * scorr, drw, drf, ifraig, cleanup, scleanup, if, &st and &if were run; rewrite, refactor, resub
 * and fraig_sweep refuse a network with choices at run time).
 */
static void apply_choices(abc_parse *ps, const abc_item *item, const odin3_abc_command_info *row) {
    switch (row->effect) {
    case ODIN3_ABC_FX_NTK_AIG:
    case ODIN3_ABC_FX_NTK_MAPPED:
    case ODIN3_ABC_FX_NTK_CELLMAPPED:
    case ODIN3_ABC_FX_NTK_KEEP:
        ps->ntk_choices = strcmp(row->name, "dch") == 0;
        break;
    case ODIN3_ABC_FX_NTK_PUT:
        ps->ntk_choices = ps->gia_choices != ABC_CHOICES_NONE;
        break;
    case ODIN3_ABC_FX_GIA_GET:
        ps->gia_choices = get_choices(ps, item);
        break;
    case ODIN3_ABC_FX_GIA_AIG:
        ps->gia_choices = strcmp(row->name, "&st") == 0 ? ABC_CHOICES_NONE : ABC_CHOICES_NATIVE;
        break;
    case ODIN3_ABC_FX_GIA_MAPPED:
    case ODIN3_ABC_FX_GIA_CELLMAPPED:
        ps->gia_choices = ABC_CHOICES_NONE;
        break;
    default:
        break; /* printing, &mfs: the choices stay */
    }
}

/* Step 11's & space rules: the BLIF & rule; &if needs an unmapped & space (PHASE3 #22) without
 * choices &get copied (fix round 1, I1); &get -m may not copy the BLIF network's nodes as LUTs,
 * whose width is unknown (final review C1). */
static odin3_status check_gia_rules(const abc_parse *ps, const abc_item *item,
                                    const odin3_abc_command_info *row) {
    abc_from from = from_alias(item);
    bool if_gia =
        row->effect == ODIN3_ABC_FX_GIA_MAPPED || row->effect == ODIN3_ABC_FX_GIA_CELLMAPPED;
    if (row->name[0] == '&' && row->effect != ODIN3_ABC_FX_GIA_GET && ps->gia == ABC_EMPTY) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs a network in the & space, which the BLIF exchange "
                        "starts empty; add '&get'",
                        row->name, from.pre, from.name, from.post);
    }
    if (if_gia && (ps->gia == ABC_MAPPED || ps->gia == ABC_LOGIC || ps->gia == ABC_CELLMAPPED)) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs an unmapped network in the & space, which is mapped "
                        "here; add '&st'",
                        row->name, from.pre, from.name, from.post);
    }
    if (if_gia && ps->gia_choices == ABC_CHOICES_COPIED) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs a network in the & space without the choices that "
                        "'&get' copied; add '&st'",
                        row->name, from.pre, from.name, from.post);
    }
    if (row->effect == ODIN3_ABC_FX_GIA_GET && get_state(ps, item) == ABC_LOGIC) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s with -m would keep the nodes of the network the BLIF exchange "
                        "reads as LUTs of any width, but linked ABC maps LUTs of at most %d "
                        "inputs ('&st' and '&synch2' crash on wider ones at the pin); add "
                        "'strash' first",
                        row->name, from.pre, from.name, from.post, ODIN3_ABC_MAX_LUT_LINKED);
    }
    return ODIN3_OK;
}

/* Commands that need a network some command has rebuilt, not the one the BLIF exchange reads,
 * which can hold dangling nodes (final review C2: mfs asserts in Abc_MfsComputeRoots, mfsWin.c:105,
 * and SIGSEGVs in Release; print_xcut asserts in Abc_NtkCrossCut, abcUtil.c:2052). */
static const char *const k_abc_need_rebuilt[] = {"mfs", "print_xcut"};

/* Step 11's network rules: print_xcut needs no choices (fix round 1, I1); mfs and print_xcut need
 * a rebuilt network (final review C2). */
static odin3_status check_ntk_rules(const abc_parse *ps, const abc_item *item,
                                    const odin3_abc_command_info *row) {
    abc_from from = from_alias(item);
    if (strcmp(row->name, "print_xcut") == 0 && ps->ntk_choices) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s needs a network without choices, which it has here; add "
                        "'strash'",
                        row->name, from.pre, from.name, from.post);
    }
    for (size_t i = 0; ps->ntk == ABC_LOGIC && i < sizeof k_abc_need_rebuilt / sizeof(char *);
         i++) {
        if (strcmp(row->name, k_abc_need_rebuilt[i]) == 0) {
            return abc_fail(ps, item->line,
                            "'%s'%s%s%s cannot run on the network the BLIF exchange reads, which "
                            "can hold dangling nodes; add 'strash' before it",
                            row->name, from.pre, from.name, from.post);
        }
    }
    return ODIN3_OK;
}

/* The commands that work on LUT networks: a gate-mapped network is not one (gatemap spec 5). */
static const char *const k_abc_lut_ntk[] = {"mfs", "mfs2", "lutpack"};

/* Gate mapping's rules: map and &nf need abc --gates (a library is loaded there) and map needs a
 * strashed network; the LUT-network commands refuse a gate-mapped one (gatemap plan Task 5). */
static odin3_status check_gate_rules(const abc_parse *ps, const abc_item *item,
                                     const odin3_abc_command_info *row) {
    abc_from from = from_alias(item);
    bool cell =
        row->effect == ODIN3_ABC_FX_NTK_CELLMAPPED || row->effect == ODIN3_ABC_FX_GIA_CELLMAPPED;
    if (cell && !ps->gates) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s maps to a gate library, which only abc --gates loads",
                        row->name, from.pre, from.name, from.post);
    }
    if (row->effect == ODIN3_ABC_FX_NTK_CELLMAPPED && ps->ntk != ABC_AIG) {
        return abc_fail(ps, item->line, "'%s'%s%s%s needs a strashed network; add 'strash'",
                        row->name, from.pre, from.name, from.post);
    }
    bool lut_ntk = false;
    for (size_t i = 0; i < sizeof k_abc_lut_ntk / sizeof(char *); i++) {
        lut_ntk = lut_ntk || strcmp(row->name, k_abc_lut_ntk[i]) == 0;
    }
    bool lut_gia = strcmp(row->name, "&mfs") == 0;
    if ((lut_ntk && ps->ntk == ABC_CELLMAPPED) || (lut_gia && ps->gia == ABC_CELLMAPPED)) {
        return abc_fail(ps, item->line,
                        "'%s'%s%s%s works on LUT networks, not on the gate-mapped network here; "
                        "add '%s'",
                        row->name, from.pre, from.name, from.post, lut_gia ? "&st" : "strash");
    }
    return ODIN3_OK;
}

/* Step 11: the state rules, then the effect on the spaces and their choices. */
static odin3_status check_space(abc_parse *ps, const abc_item *item,
                                const odin3_abc_command_info *row) {
    odin3_status st = check_gate_rules(ps, item, row);
    if (st == ODIN3_OK) {
        st = check_gia_rules(ps, item, row);
    }
    if (st == ODIN3_OK) {
        st = check_ntk_rules(ps, item, row);
    }
    if (st == ODIN3_OK) {
        apply_effect(ps, item, row);
        apply_choices(ps, item, row);
    }
    return st;
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
    if (st == ODIN3_OK && ps->cmds.len >= ps->lim.commands) {
        st = abc_fail(ps, item->line, "script expands to more than %" PRIu32 " commands",
                      ps->lim.commands);
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
    if (ps->gates && state != ABC_CELLMAPPED) {
        return abc_fail(ps, ABC_NO_LINE,
                        "the script does not end gate-mapped; with --gates end it with a gate "
                        "mapper ('map' or '&nf')");
    }
    if (!ps->gates && state != ABC_MAPPED) {
        return abc_fail(ps, ABC_NO_LINE,
                        "the script leaves the network unmapped; end it with a LUT mapper such as "
                        "'if -K {K}' (with --gates: a gate mapper, 'map' or '&nf')");
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
    abc_parse ps = {
        .given = req->given, .lim = abc_limits, .lut_k = req->lut_k, .gates = req->gates};
    if (req->len > ps.lim.script_bytes) {
        return abc_fail(&ps, ABC_NO_LINE, "abc script is larger than %" PRIu32 " bytes",
                        ps.lim.script_bytes);
    }
    bool blif = req->exchange == ODIN3_ABC_EXCHANGE_BLIF;
    ps.ntk = blif ? ABC_LOGIC : ABC_AIG;
    ps.gia = blif ? ABC_EMPTY : ABC_AIG;
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

/* Reads at most the script cap + 1 bytes of src->resolved into a new buffer (enough to see that
 * a larger file is too large). The message names the file as given (§7.3; final review M3). */
static odin3_status read_script(const odin3_abc_script_src *src, uint8_t **bytes, size_t *len) {
    FILE *file = fopen(src->resolved, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot read the script: %s", src->given,
                  strerror(errno));
        return ODIN3_ERR_IO;
    }
    size_t cap = (size_t)abc_limits.script_bytes + 1;
    uint8_t *buf = odin3_util_malloc(cap);
    if (buf == NULL) {
        (void)fclose(file);
        return ODIN3_ERR_NO_MEMORY;
    }
    errno = 0;
    size_t got = fread(buf, 1, cap, file);
    bool failed = ferror(file) != 0;
    int err = errno != 0 ? errno : EIO;
    (void)fclose(file);
    if (failed) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot read the script: %s", src->given,
                  strerror(err));
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

odin3_status odin3_abc_script_parse_mode(const odin3_abc_script_src *src,
                                         const odin3_abc_script_mode *mode, odin3_abc_script *out) {
    abc_request req = {NULL, NULL, 0, 0, 0, false};
    if (src != NULL && mode != NULL) {
        req = (abc_request){src->given,  src->text.ptr,  src->text.len,
                            mode->lut_k, mode->exchange, mode->gates};
    }
    if (src == NULL || mode == NULL || out == NULL || !request_valid(src, &req)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_script_parse: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint8_t *owned = NULL;
    if (src->text.ptr == NULL) {
        odin3_status st = read_script(src, &owned, &req.len);
        if (st != ODIN3_OK) {
            return st;
        }
        req.bytes = owned;
    }
    odin3_status st = parse_bytes(&req, out);
    odin3_util_free(owned);
    return st;
}

odin3_status odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k,
                                    uint32_t exchange, odin3_abc_script *out) {
    odin3_abc_script_mode mode = {lut_k, exchange, false};
    return odin3_abc_script_parse_mode(src, &mode, out);
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

void odin3_abc_default_sha256(uint8_t digest[ODIN3_SHA256_DIGEST_BYTES]) {
    odin3_sha256 sha;
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, odin3_abc_default_script, odin3_abc_default_script_len);
    odin3_sha256_final(&sha, digest);
}
