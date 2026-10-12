/*
 * fuzz_abc_script.c — the ABC script checker's fuzzer (3D spec §13.7, AB-21; plan Task 11), a
 * libFuzzer target built with -DODIN3_FUZZ=ON under clang, ASan and UBSan. It runs the checker
 * only: ABC is never linked or started.
 *
 * Input: one K byte (K = 2 + byte % 15, or no --lut when the byte is 0), then the script's bytes,
 * named f.abc. Each input is checked twice, in memory (AIG) and through BLIF.
 *
 * Invariants (a violation is __builtin_trap(), which libFuzzer reports; tests/ only):
 *  - the status is OK or PARSE (never IO, INVALID_ARG or NO_MEMORY: the bytes are in memory);
 *  - rejected: exactly one line was logged, `abc: f.abc:<line>: …` with <line> within the input's
 *    lines, or one of the two unlocated texts (the size cap, the unmapped end);
 *  - accepted: nothing was logged, and
 *    - 1 <= n <= 1,024 commands, each line printable ASCII words joined by one blank, with no
 *      quote, `!`, `;`, `#`, `{`, `}` or tab, from a line within the input;
 *    - every word 0 has an allowed row of the allow-list and is no alias name; the alias is NULL or
 *      an abc.rc alias;
 *    - every argument is an option or a number from 0 to 1,000,000 as strtod and atoi read it
 *      (echo: any such printable word), no option letter is one of its row's file, string or
 *      crash options, and print_kmap has at most one positional word;
 *    - replaying §7.5's effects (with the checker's &get, &if, balancing, BLIF-start and choice
 *      rules, re-derived here) from the exchange's start ends in the reported final space,
 *      mapped, and never runs an & command on an empty & space, &if on a mapped one or on
 *      choices a plain &get copied, nor print_xcut on a network with choices;
 *    - `-K` of if/&if is 2..12 and `-E` at least 1, refactor `-N` at least 2, each alone; &get
 *      -m, mfs and print_xcut never run on the network the BLIF exchange reads;
 *    - uses_k implies a K of at most 12, and the SHA-256 is that of the script's bytes;
 *  - accepted through BLIF implies accepted in memory, with the same lines;
 *  - no leak, no sanitizer report, no hang (libFuzzer's -rss_limit_mb and -timeout).
 *
 * Seeds (tests/fuzz/abc_script_seeds): lib/abc/vtr.abc and VTR's whole ABC stage, one file per
 * abc.rc alias (`<alias>` then `if -K {K}`), and one per refused example of test_abc_script; each
 * starts with its K byte (0x04 is K 6, 0x00 no --lut).
 */
#include "passes/abc/abc.h"
#include "passes/abc/script.h"
#include "util/log.h"
#include "util/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum {
    K_BASE = 2,
    K_SPAN = 15,
    LAST_LOG = 1024,
    LINE_CAP = 16384, /* a line of 4,096 bytes plus an alias's text */
    MAX_SPLIT = 1024,
    EMPTY = 0,
    AIG_STATE = 1,
    MAPPED = 2,
    LOGIC = 3, /* the BLIF start: a logic network, mapped but not by the script */
    NO_CHOICES = 0,
    NATIVE = 1,
    COPIED = 2,
    CAP = 1000000,
    LUT_LINKED = 12
};

static const char k_name[] = "f.abc";

typedef struct sink_count {
    uint32_t lines;
    char last[LAST_LOG];
} sink_count;

static sink_count g_count;

static void fail(const char *what) {
    (void)fprintf(stderr, "fuzz_abc_script: %s (lines %u; last: %s)\n", what,
                  (unsigned)g_count.lines, g_count.last);
    __builtin_trap();
}

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    sink_count *count = user;
    count->lines++;
    size_t len = strnlen(msg, sizeof count->last - 1);
    memcpy(count->last, msg, len);
    count->last[len] = '\0';
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    odin3_log_set_sink(count_sink, &g_count);
    return 0;
}

/* --- words ---------------------------------------------------------------------------------- */

static bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static bool is_alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool option_word(const char *word) {
    if (word[0] != '-' || !is_alpha(word[1])) {
        return false;
    }
    for (size_t i = 2; word[i] != '\0'; i++) {
        if (!is_alpha(word[i]) && !is_digit(word[i])) {
            return false;
        }
    }
    return true;
}

/* [-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?, by a second, independent reading. */
static bool number_word(const char *word) {
    size_t i = word[0] == '-' || word[0] == '+';
    size_t digits = 0;
    for (; is_digit(word[i]); i++) {
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    if (word[i] == '.') {
        size_t frac = ++i;
        while (is_digit(word[i])) {
            i++;
        }
        if (i == frac) {
            return false;
        }
    }
    if (word[i] == 'e' || word[i] == 'E') {
        i++;
        i += word[i] == '-' || word[i] == '+';
        size_t exp = i;
        while (is_digit(word[i])) {
            i++;
        }
        if (i == exp) {
            return false;
        }
    }
    return word[i] == '\0';
}

/* Splits a copy of line at blanks into words; traps on an empty word. */
static uint32_t split(const char *line, char *buf, const char **words) {
    size_t len = strlen(line);
    if (len == 0 || len >= LINE_CAP) {
        fail("an empty or oversized line");
    }
    memcpy(buf, line, len + 1);
    uint32_t n = 0;
    char *at = buf;
    for (;;) {
        char *blank = strchr(at, ' ');
        if (blank != NULL) {
            *blank = '\0';
        }
        if (at[0] == '\0' || n == MAX_SPLIT) {
            fail("a line with an empty word, a leading, trailing or double blank");
        }
        words[n++] = at;
        if (blank == NULL) {
            return n;
        }
        at = blank + 1;
    }
}

/* --- the accepted-script properties --------------------------------------------------------- */

typedef struct model {
    int ntk, gia, cur; /* cur: ODIN3_ABC_SPACE_NTK or _GIA */
    bool ntk_choices;  /* the network holds choices (dch, or &put of a & space with them) */
    int gia_choices;   /* NO_CHOICES, NATIVE (dch-made, &get -s) or COPIED (a plain &get) */
} model;

static bool has_letter(const char *const *words, uint32_t n, char letter) {
    for (uint32_t i = 1; i < n; i++) {
        if (option_word(words[i]) && strchr(words[i] + 1, letter) != NULL) {
            return true;
        }
    }
    return false;
}

/* The parity of letter among the option words before the first other word (&get's getopt). */
static bool toggled(const char *const *words, uint32_t n, char letter) {
    bool on = false;
    for (uint32_t i = 1; i < n && option_word(words[i]); i++) {
        for (const char *at = words[i] + 1; *at != '\0'; at++) {
            on = *at == letter ? !on : on;
        }
    }
    return on;
}

static bool balancing(const char *const *words, uint32_t n, const char *letters) {
    for (const char *at = letters; *at != '\0'; at++) {
        if (has_letter(words, n, *at)) {
            return true;
        }
    }
    return false;
}

/* The BLIF-start rules (final review C1, C2): &get -m may not copy the read network's nodes as
 * LUTs, and mfs and print_xcut need a network some command rebuilt. */
static void check_start_rules(const model *md, const odin3_abc_command_info *row,
                              const char *const *words, uint32_t n) {
    if (md->ntk != LOGIC) {
        return;
    }
    if (row->effect == ODIN3_ABC_FX_GIA_GET && toggled(words, n, 'm') && !toggled(words, n, 'c')) {
        fail("&get -m of the network the BLIF exchange reads was accepted");
    }
    if (strcmp(row->name, "mfs") == 0 || strcmp(row->name, "print_xcut") == 0) {
        fail("mfs or print_xcut on the network the BLIF exchange reads was accepted");
    }
}

/* The state rules an accepted command must satisfy; a violation traps. */
static void check_state_rules(const model *md, const odin3_abc_command_info *row) {
    bool amp = row->name[0] == '&';
    if (amp && strcmp(row->name, "&get") != 0 && md->gia == EMPTY) {
        fail("an & command on an empty & space was accepted");
    }
    if (row->effect == ODIN3_ABC_FX_GIA_MAPPED && (md->gia == MAPPED || md->gia == LOGIC)) {
        fail("&if on a mapped & space was accepted");
    }
    if (row->effect == ODIN3_ABC_FX_GIA_MAPPED && md->gia_choices == COPIED) {
        fail("&if on choices a plain &get copied was accepted");
    }
    if (strcmp(row->name, "print_xcut") == 0 && md->ntk_choices) {
        fail("print_xcut on a network with choices was accepted");
    }
}

static void replay_ntk(model *md, const odin3_abc_command_info *row, const char *const *words,
                       uint32_t n) {
    switch (row->effect) {
    case ODIN3_ABC_FX_NTK_AIG:
        md->ntk = AIG_STATE;
        break;
    case ODIN3_ABC_FX_NTK_MAPPED:
        md->ntk = balancing(words, n, "gxyu") ? AIG_STATE : MAPPED;
        break;
    case ODIN3_ABC_FX_NTK_PUT:
        md->ntk = md->gia;
        break;
    default:
        break;
    }
    md->ntk_choices = row->effect == ODIN3_ABC_FX_NTK_PUT ? md->gia_choices != NO_CHOICES
                                                          : strcmp(row->name, "dch") == 0;
    md->cur = ODIN3_ABC_SPACE_NTK;
}

static void replay_gia(model *md, const odin3_abc_command_info *row, const char *const *words,
                       uint32_t n) {
    if (row->effect == ODIN3_ABC_FX_GIA_GET) {
        bool keeps = (md->ntk == MAPPED || md->ntk == LOGIC) && toggled(words, n, 'm') &&
                     !toggled(words, n, 'c');
        md->gia = keeps ? md->ntk : AIG_STATE;
        md->gia_choices = !md->ntk_choices ? NO_CHOICES : toggled(words, n, 's') ? NATIVE : COPIED;
    } else if (row->effect == ODIN3_ABC_FX_GIA_AIG) {
        md->gia = AIG_STATE;
        md->gia_choices = strcmp(row->name, "&st") == 0 ? NO_CHOICES : NATIVE;
    } else if (row->effect == ODIN3_ABC_FX_GIA_MAPPED) {
        md->gia = balancing(words, n, "gxy") ? AIG_STATE : MAPPED;
        md->gia_choices = NO_CHOICES;
    }
    md->cur = ODIN3_ABC_SPACE_GIA;
}

/* One command's effect on the model, after the rules (an independent reading of step 11). */
static void replay(model *md, const odin3_abc_command_info *row, const char *const *words,
                   uint32_t n) {
    check_state_rules(md, row);
    check_start_rules(md, row, words, n);
    switch (row->effect) {
    case ODIN3_ABC_FX_NTK_AIG:
    case ODIN3_ABC_FX_NTK_MAPPED:
    case ODIN3_ABC_FX_NTK_PUT:
    case ODIN3_ABC_FX_NTK_KEEP:
        replay_ntk(md, row, words, n);
        break;
    case ODIN3_ABC_FX_GIA_GET:
    case ODIN3_ABC_FX_GIA_AIG:
    case ODIN3_ABC_FX_GIA_MAPPED:
    case ODIN3_ABC_FX_GIA_KEEP:
        replay_gia(md, row, words, n);
        break;
    default:
        break;
    }
}

/* The number cap, read by the C library (the fuzzer never sets a locale): the value as strtod
 * reads it, and the integer digits as atoi reads them, lie in 0 .. 1000000. The checker's exact
 * reading is stricter, so this holds for every number it accepts. */
static bool number_in_cap(const char *word) {
    if (word[0] == '-') {
        return false;
    }
    double value = strtod(word, NULL);
    unsigned long long int_part = 0;
    for (const char *at = word + (word[0] == '+'); is_digit(*at) && int_part <= CAP; at++) {
        int_part = int_part * 10 + (unsigned long long)(*at - '0');
    }
    return value >= 0.0 && value <= (double)CAP && int_part <= CAP;
}

/* The value ranges (final review C1, I1): `-K` 2..12 and `-E` >= 1 for if and &if, `-N` >= 2 for
 * refactor; each letter alone as `-X <n>`. strtod/strtol read the value here. */
static void check_range(const char *name, const char *const *words, uint32_t n) {
    bool mapper = strcmp(name, "if") == 0 || strcmp(name, "&if") == 0;
    for (uint32_t i = 1; i < n; i++) {
        bool k = mapper && option_word(words[i]) && strchr(words[i] + 1, 'K') != NULL;
        bool e = mapper && option_word(words[i]) && strchr(words[i] + 1, 'E') != NULL;
        bool nn = strcmp(name, "refactor") == 0 && option_word(words[i]) &&
                  strchr(words[i] + 1, 'N') != NULL;
        if (!k && !e && !nn) {
            continue;
        }
        if (words[i][2] != '\0' || i + 1 >= n || !number_word(words[i + 1])) {
            fail("a ranged option that does not stand alone was accepted");
        }
        long ival = strtol(words[i + 1], NULL, 10);
        double fval = strtod(words[i + 1], NULL);
        if ((k && (ival < 2 || ival > LUT_LINKED)) || (e && fval < 1.0) || (nn && ival < 2)) {
            fail("a ranged option outside its range was accepted");
        }
    }
}

/* The arguments of an accepted command. */
static void check_args(const odin3_abc_command_info *row, const char *const *words, uint32_t n) {
    bool echo = strcmp(row->name, "echo") == 0;
    uint32_t positional = 0;
    for (uint32_t i = 1; i < n; i++) {
        const char *word = words[i];
        if (option_word(word)) {
            if (strpbrk(word + 1, row->file_opts) != NULL ||
                strpbrk(word + 1, row->crash_opts) != NULL) {
                fail("a file, string or crash option was accepted");
            }
            continue;
        }
        positional++;
        if (!echo && (!number_word(word) || !number_in_cap(word))) {
            fail("an argument that is not a number from 0 to 1000000 was accepted");
        }
    }
    if (strcmp(row->name, "print_kmap") == 0 && positional >= 2) {
        fail("print_kmap with two arguments was accepted");
    }
    check_range(row->name, words, n);
}

/* One accepted command: its line's bytes, word 0, the arguments, the alias, the source line. */
static const odin3_abc_command_info *check_cmd(const odin3_abc_cmd *cmd, uint32_t n_lines,
                                               char *buf, const char **words, uint32_t *n) {
    for (const char *at = cmd->line; *at != '\0'; at++) {
        if (*at < ' ' || *at > '~' || strchr("\"'!;#{}", *at) != NULL) {
            fail("a line holds a byte the checker must have refused");
        }
    }
    *n = split(cmd->line, buf, words);
    const odin3_abc_command_info *row = odin3_abc_command_find(words[0]);
    if (row == NULL || row->cls < ODIN3_ABC_CLASS_OPT || row->cls > ODIN3_ABC_CLASS_PRINT) {
        fail("word 0 is not an allowed command");
    }
    if (odin3_abc_alias_find(words[0]) != NULL) {
        fail("word 0 is an alias name");
    }
    if (cmd->alias != NULL && odin3_abc_alias_find(cmd->alias) == NULL) {
        fail("a command's alias is not an abc.rc alias");
    }
    if (cmd->src_line < 1 || cmd->src_line > n_lines) {
        fail("a command's line is outside the script");
    }
    check_args(row, words, *n);
    return row;
}

static void check_sha(const odin3_abc_script *script, const uint8_t *bytes, size_t len) {
    odin3_sha256 sha;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, bytes, len);
    odin3_sha256_final(&sha, digest);
    odin3_sha256_hex(digest, hex);
    if (strcmp(hex, script->sha256_hex) != 0) {
        fail("the SHA-256 is not that of the bytes");
    }
}

static char g_buf[LINE_CAP];
static const char *g_words[MAX_SPLIT];

static void check_accepted(const odin3_abc_script *script, uint32_t exchange, uint32_t n_lines) {
    if (g_count.lines != 0 || script->n_cmds == 0 || script->n_cmds > ODIN3_ABC_MAX_COMMANDS) {
        fail("an accepted script logged a line or has 0 or more than 1024 commands");
    }
    bool blif = exchange == ODIN3_ABC_EXCHANGE_BLIF;
    model md = {blif ? LOGIC : AIG_STATE, blif ? EMPTY : AIG_STATE, ODIN3_ABC_SPACE_NTK, false,
                NO_CHOICES};
    for (uint32_t i = 0; i < script->n_cmds; i++) {
        uint32_t n = 0;
        const odin3_abc_command_info *row =
            check_cmd(&script->cmds[i], n_lines, g_buf, g_words, &n);
        replay(&md, row, g_words, n);
    }
    int end = md.cur == ODIN3_ABC_SPACE_GIA ? md.gia : md.ntk;
    if (md.cur != script->final_space || end != MAPPED) {
        fail("the replayed end is not the reported final space, mapped");
    }
}

/* --- the rejected-script properties --------------------------------------------------------- */

static void check_rejected(uint32_t n_lines) {
    static const char k_prefix[] = "abc: f.abc:";
    static const char k_size[] = "abc: f.abc: abc script is larger than 65536 bytes";
    static const char k_unmapped[] = "abc: f.abc: the script leaves the network unmapped; end it "
                                     "with a LUT mapper such as 'if -K {K}'";
    if (g_count.lines != 1) {
        fail("a refused script did not log exactly one line");
    }
    if (strcmp(g_count.last, k_size) == 0 || strcmp(g_count.last, k_unmapped) == 0) {
        return;
    }
    if (strncmp(g_count.last, k_prefix, sizeof k_prefix - 1) != 0) {
        fail("a refusal is not located in f.abc");
    }
    unsigned long line = 0;
    const char *at = g_count.last + sizeof k_prefix - 1;
    for (; is_digit(*at) && line <= n_lines; at++) {
        line = line * 10 + (unsigned long)(*at - '0');
    }
    if (line < 1 || line > n_lines || at[0] != ':' || at[1] != ' ') {
        fail("a refusal's line is outside the script");
    }
}

/* --- one input ------------------------------------------------------------------------------ */

static odin3_status check_once(const odin3_abc_script_src *src, uint32_t lut_k, uint32_t exchange,
                               odin3_abc_script *out) {
    g_count.lines = 0;
    g_count.last[0] = '\0';
    odin3_status st = odin3_abc_script_parse(src, lut_k, exchange, out);
    uint32_t n_lines = 1;
    const char *bytes = src->text.ptr;
    for (size_t i = 0; i < src->text.len; i++) {
        n_lines += bytes[i] == '\n';
    }
    if (st == ODIN3_OK) {
        check_accepted(out, exchange, n_lines);
        check_sha(out, src->text.ptr, src->text.len);
        if (out->uses_k && (lut_k == 0 || lut_k > LUT_LINKED)) {
            fail("uses_k without a K, or with a K above 12");
        }
    } else if (st == ODIN3_ERR_PARSE) {
        check_rejected(n_lines);
    } else {
        fail("a status other than OK or PARSE");
    }
    return st;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    uint32_t lut_k = data[0] == 0 ? 0 : K_BASE + (uint32_t)(data[0] % K_SPAN);
    odin3_abc_script_src src = {k_name, NULL, {data + 1, size - 1}};
    odin3_abc_script mem = {0};
    odin3_abc_script blif = {0};
    odin3_status st_mem = check_once(&src, lut_k, ODIN3_ABC_EXCHANGE_AIG, &mem);
    odin3_status st_blif = check_once(&src, lut_k, ODIN3_ABC_EXCHANGE_BLIF, &blif);
    if (st_blif == ODIN3_OK) {
        if (st_mem != ODIN3_OK || mem.n_cmds != blif.n_cmds) {
            fail("accepted through BLIF but not in memory, or with other commands");
        }
        for (uint32_t i = 0; i < blif.n_cmds; i++) {
            if (strcmp(mem.cmds[i].line, blif.cmds[i].line) != 0) {
                fail("accepted through BLIF and in memory with other lines");
            }
        }
    }
    odin3_abc_script_free(&mem);
    odin3_abc_script_free(&blif);
    return 0;
}
