/*
 * test_abc_alias_table.c — the ABC alias table and the allow-list data (3D spec §7.4, §7.5; plan
 * Task 10): the table equals the pinned abc.rc, §7.4's lists, §7.5's rows, and the alias depth.
 */
#include "passes/abc/abc_test.h"
#include "passes/abc/script.h"
#include "unity.h"
#include "util/file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ODIN3_ABC_RC
#error "ODIN3_ABC_RC must name third_party/abc/abc.rc"
#endif

enum {
    RC_MAX_BYTES = 1 << 20, /* abc.rc is about 6 KB */
    TEXT_CAP = 8192,        /* a fully expanded alias, e.g. &compress2rs */
    WORD_CAP = 64,          /* one command name */
    DEPTH_LIMIT = 8,        /* ODIN3_ABC_MAX_ALIAS_DEPTH */
    REAL_MAX_DEPTH = 3,     /* &resyn2rs -> resyn2rs -> b -> balance */
    ABC_RC_ALIASES = 112,   /* `alias` lines of abc.rc at a3001b72 */
    PRINT_STAR_ROWS = 32    /* the allowed print_* commands (§7.5) */
};

typedef struct pair {
    const char *name, *text;
} pair;

typedef struct refused {
    const char *name;
    uint8_t cls;
} refused;

/* §7.4: usable one-word aliases and their expansions. */
static const pair k_usable_words[] = {
    {"b", "balance"},        {"cl", "cleanup"},        {"f", "fraig"},
    {"fs", "fraig_sweep"},   {"lp", "lutpack"},        {"pcon", "print_cone"},
    {"pd", "print_dsd"},     {"pex", "print_exdc -d"}, {"pf", "print_factor"},
    {"pfan", "print_fanio"}, {"pg", "print_gates"},    {"pl", "print_level"},
    {"plat", "print_latch"}, {"pio", "print_io"},      {"pk", "print_kmap"},
    {"pm", "print_miter"},   {"ps", "print_stats"},    {"psb", "print_stats -b"},
    {"psu", "print_supp"},   {"psy", "print_symm"},    {"pun", "print_unate"},
    {"rw", "rewrite"},       {"rwz", "rewrite -z"},    {"rf", "refactor"},
    {"rfz", "refactor -z"},  {"rs", "resub"},          {"rsz", "resub -z"},
    {"scl", "scleanup"},     {"sif", "if -s"},         {"st", "strash"},
    {"sw", "sweep"},
};

/* §7.4: refused one-word aliases and the class of their command (OTHER: no row). */
static const refused k_refused_words[] = {
    {"r", ODIN3_ABC_CLASS_FILES},      {"ra", ODIN3_ABC_CLASS_FILES},
    {"rh", ODIN3_ABC_CLASS_FILES},     {"ri", ODIN3_ABC_CLASS_FILES},
    {"rl", ODIN3_ABC_CLASS_FILES},     {"rb", ODIN3_ABC_CLASS_FILES},
    {"rp", ODIN3_ABC_CLASS_FILES},     {"rt", ODIN3_ABC_CLASS_FILES},
    {"rv", ODIN3_ABC_CLASS_FILES},     {"rvl", ODIN3_ABC_CLASS_FILES},
    {"rsup", ODIN3_ABC_CLASS_FILES},   {"rlib", ODIN3_ABC_CLASS_FILES},
    {"rlibc", ODIN3_ABC_CLASS_FILES},  {"rty", ODIN3_ABC_CLASS_FILES},
    {"rlut", ODIN3_ABC_CLASS_FILES},   {"so", ODIN3_ABC_CLASS_FILES},
    {"w", ODIN3_ABC_CLASS_FILES},      {"wa", ODIN3_ABC_CLASS_FILES},
    {"wb", ODIN3_ABC_CLASS_FILES},     {"wc", ODIN3_ABC_CLASS_FILES},
    {"wh", ODIN3_ABC_CLASS_FILES},     {"wl", ODIN3_ABC_CLASS_FILES},
    {"wp", ODIN3_ABC_CLASS_FILES},     {"wv", ODIN3_ABC_CLASS_FILES},
    {"hi", ODIN3_ABC_CLASS_PROCESS},   {"q", ODIN3_ABC_CLASS_PROCESS},
    {"sa", ODIN3_ABC_CLASS_PROCESS},   {"fr", ODIN3_ABC_CLASS_LATCHES},
    {"r3", ODIN3_ABC_CLASS_LATCHES},   {"r3f", ODIN3_ABC_CLASS_LATCHES},
    {"r3b", ODIN3_ABC_CLASS_LATCHES},  {"ret", ODIN3_ABC_CLASS_LATCHES},
    {"dret", ODIN3_ABC_CLASS_LATCHES}, {"cg", ODIN3_ABC_CLASS_OTHER},
    {"clp", ODIN3_ABC_CLASS_OTHER},    {"cs", ODIN3_ABC_CLASS_OTHER},
    {"el", ODIN3_ABC_CLASS_OTHER},     {"esd", ODIN3_ABC_CLASS_OTHER},
    {"fsto", ODIN3_ABC_CLASS_OTHER},   {"fres", ODIN3_ABC_CLASS_OTHER},
    {"ft", ODIN3_ABC_CLASS_OTHER},     {"ic", ODIN3_ABC_CLASS_OTHER},
    {"ren", ODIN3_ABC_CLASS_OTHER},    {"re", ODIN3_ABC_CLASS_OTHER},
    {"rez", ODIN3_ABC_CLASS_OTHER},    {"ssw", ODIN3_ABC_CLASS_OTHER},
    {"tr0", ODIN3_ABC_CLASS_OTHER},    {"tr1", ODIN3_ABC_CLASS_OTHER},
    {"u", ODIN3_ABC_CLASS_OTHER},
};

/* §7.4's table of usable script aliases, verbatim. */
static const pair k_usable_scripts[] = {
    {"resyn", "b; rw; rwz; b; rwz; b"},
    {"resyn2", "b; rw; rf; b; rw; rwz; b; rfz; rwz; b"},
    {"resyn2a", "b; rw; b; rw; rwz; b; rwz; b"},
    {"resyn3", "b; rs; rs -K 6; b; rsz; rsz -K 6; b; rsz -K 5; b"},
    {"compress", "b -l; rw -l; rwz -l; b -l; rwz -l; b -l"},
    {"compress2", "b -l; rw -l; rf -l; b -l; rw -l; rwz -l; b -l; rfz -l; rwz -l; b -l"},
    {"rwsat", "st; rw -l; b -l; rw -l; rf -l"},
    {"drwsat2", "st; drw; b -l; drw; drf; ifraig -C 20; drw; b -l; drw; drf"},
    {"src_rw", "st; rw -l; rwz -l; rwz -l"},
    {"src_rs", "st; rs -K 6 -N 2 -l; rs -K 9 -N 2 -l; rs -K 12 -N 2 -l"},
    {"src_rws", "st; rw -l; rs -K 6 -N 2 -l; rwz -l; rs -K 9 -N 2 -l; rwz -l; rs -K 12 -N 2 -l"},
    {"resyn2rs", "b; rs -K 6; rw; rs -K 6 -N 2; rf; rs -K 8; b; rs -K 8 -N 2; rw; rs -K 10; rwz; "
                 "rs -K 10 -N 2; b; rs -K 12; rfz; rs -K 12 -N 2; rwz; b"},
    {"r2rs", "b; rs -K 6; rw; rs -K 6 -N 2; rf; rs -K 8; b; rs -K 8 -N 2; rw; rs -K 10; rwz; "
             "rs -K 10 -N 2; b; rs -K 12; rfz; rs -K 12 -N 2; rwz; b"},
    {"compress2rs", "b -l; rs -K 6 -l; rw -l; rs -K 6 -N 2 -l; rf -l; rs -K 8 -l; b -l; "
                    "rs -K 8 -N 2 -l; rw -l; rs -K 10 -l; rwz -l; rs -K 10 -N 2 -l; b -l; "
                    "rs -K 12 -l; rfz -l; rs -K 12 -N 2 -l; rwz -l; b -l"},
    {"c2rs", "b -l; rs -K 6 -l; rw -l; rs -K 6 -N 2 -l; rf -l; rs -K 8 -l; b -l; "
             "rs -K 8 -N 2 -l; rw -l; rs -K 10 -l; rwz -l; rs -K 10 -N 2 -l; b -l; "
             "rs -K 12 -l; rfz -l; rs -K 12 -N 2 -l; rwz -l; b -l"},
    {"&resyn2rs", "&put; resyn2rs; &get"},
    {"&compress2rs", "&put; compress2rs; &get"},
    {"&sw_", "&put; sweep; st; &get"},
};

/* §7.4: refused script aliases and the command that stops each (its first refused command). */
static const pair k_refused_scripts[] = {
    {"choice", "fraig_store"},
    {"choice2", "fraig_store"},
    {"share", "multi"},
    {"addinit", "read_init"},
    {"blif2aig", "undc"},
    {"fix_blif", "undc"},
    {"fix_aig", "logic"},
    {"v2p", "&vta_gla"},
    {"g2p", "&gla_derive"},
    {"&fx_", "sop"},
    {"&dc3", "&b"},
    {"&dc4", "&b"},
    {"trt", "read"},
    {"recadd3", "rec_add3"},
};

#define COUNT_OF(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* A byte range [pos, end). */
typedef struct span {
    const char *pos, *end;
} span;

/* A NUL-terminated text being built in ptr[0..cap). */
typedef struct text_buf {
    char *ptr;
    size_t cap, len;
} text_buf;

/* One parsed `alias` line of abc.rc. */
typedef struct rc_line {
    char name[WORD_CAP];
    char text[TEXT_CAP];
} rc_line;

/* abc.rc's bytes, freed in tearDown so that a failing assertion leaks nothing. */
static odin3_file_bytes rc;

void setUp(void) {
}

void tearDown(void) {
    odin3_abc_test_set_aliases(NULL, 0);
    odin3_file_bytes_free(&rc);
}

static bool is_blank(char ch) {
    return ch == ' ' || ch == '\t';
}

static bool is_allowed(const odin3_abc_command_info *row) {
    return row != NULL && row->cls >= ODIN3_ABC_CLASS_OPT && row->cls <= ODIN3_ABC_CLASS_PRINT;
}

static void buf_reset(text_buf *buf) {
    buf->len = 0;
    buf->ptr[0] = '\0';
}

static void buf_put(text_buf *buf, const char *src, size_t count) {
    TEST_ASSERT_TRUE(buf->len + count < buf->cap);
    memcpy(buf->ptr + buf->len, src, count);
    buf->len += count;
    buf->ptr[buf->len] = '\0';
}

/* Copies the word at src (up to a blank, ';' or the end) into word; returns its length. */
static size_t take_word(const char *src, char *word) {
    size_t len = 0;
    while (src[len] != '\0' && src[len] != ';' && !is_blank(src[len])) {
        len++;
    }
    TEST_ASSERT_TRUE(len < WORD_CAP);
    memcpy(word, src, len);
    word[len] = '\0';
    return len;
}

/* The next ';'-command at *cursor, blanks trimmed; false at the end of the text. */
static bool next_command(const char **cursor, span *cmd) {
    const char *pos = *cursor;
    while (*pos == ';' || is_blank(*pos)) {
        pos++;
    }
    const char *end = pos;
    while (*end != '\0' && *end != ';') {
        end++;
    }
    *cursor = end;
    while (end > pos && is_blank(end[-1])) {
        end--;
    }
    cmd->pos = pos;
    cmd->end = end;
    return pos != end;
}

/*
 * Appends cmd to out, its word 0 replaced by the alias text when it names an alias (the other
 * words appended at the end, as CmdApplyAlias does). Returns whether it expanded an alias.
 */
static bool expand_command(text_buf *out, span cmd) {
    char word[WORD_CAP];
    size_t len = take_word(cmd.pos, word);
    const odin3_abc_alias *alias = odin3_abc_alias_find(word);
    if (out->len > 0) {
        buf_put(out, "; ", 2);
    }
    if (alias == NULL) {
        buf_put(out, cmd.pos, (size_t)(cmd.end - cmd.pos));
        return false;
    }
    buf_put(out, alias->expansion, strlen(alias->expansion));
    buf_put(out, cmd.pos + len, (size_t)(cmd.end - cmd.pos) - len);
    return true;
}

/* Expands every alias word 0 of in once into out; returns whether any was expanded. */
static bool expand_once(const char *in, text_buf *out) {
    bool any = false;
    span cmd;
    buf_reset(out);
    for (const char *cursor = in; next_command(&cursor, &cmd);) {
        any = expand_command(out, cmd) || any;
    }
    return any;
}

/* Expands text into out (TEXT_CAP bytes) until no word 0 is an alias; returns the rounds. */
static int expand_all(const char *text, char *out) {
    static char tmp[TEXT_CAP];
    text_buf buf = {tmp, TEXT_CAP, 0};
    size_t len = strlen(text);
    TEST_ASSERT_TRUE(len < TEXT_CAP);
    memcpy(out, text, len + 1);
    int depth = 0;
    while (depth <= DEPTH_LIMIT && expand_once(out, &buf)) {
        memcpy(out, tmp, buf.len + 1);
        depth++;
    }
    return depth;
}

/* Copies into word the first word 0 of text that is not allowed; false when all are. */
static bool first_refused(const char *text, char *word) {
    span cmd;
    for (const char *cursor = text; next_command(&cursor, &cmd);) {
        (void)take_word(cmd.pos, word);
        if (!is_allowed(odin3_abc_command_find(word))) {
            return true;
        }
    }
    return false;
}

/* The number of table rows called name, class OTHER included. */
static uint32_t rows_named(const char *name) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < odin3_abc_command_count(); i++) {
        hits += strcmp(odin3_abc_command_at(i)->name, name) == 0;
    }
    return hits;
}

static span skip_blanks(span text) {
    while (text.pos < text.end && is_blank(*text.pos)) {
        text.pos++;
    }
    return text;
}

static size_t word_len(span text) {
    size_t len = 0;
    while (text.pos + len < text.end && !is_blank(text.pos[len])) {
        len++;
    }
    return len;
}

/* Appends the words of text joined by one blank (an unquoted abc.rc expansion). */
static void put_words(text_buf *buf, span text) {
    for (text = skip_blanks(text); text.pos < text.end; text = skip_blanks(text)) {
        size_t len = word_len(text);
        if (buf->len > 0) {
            buf_put(buf, " ", 1);
        }
        buf_put(buf, text.pos, len);
        text.pos += len;
    }
}

/* Appends what lies between the opening quote at text.pos and the next quote. */
static void put_quoted(text_buf *buf, span text) {
    const char *close = memchr(text.pos + 1, '"', (size_t)(text.end - text.pos - 1));
    TEST_ASSERT_NOT_NULL(close);
    buf_put(buf, text.pos + 1, (size_t)(close - text.pos - 1));
}

/* Parses `alias <name> "<expansion>"` or `alias <name> <words>`; false for any other line. */
static bool parse_alias_line(span line, rc_line *out) {
    static const char k_keyword[] = "alias";
    size_t kw_len = sizeof k_keyword - 1;
    if ((size_t)(line.end - line.pos) <= kw_len || memcmp(line.pos, k_keyword, kw_len) != 0 ||
        !is_blank(line.pos[kw_len])) {
        return false;
    }
    line.pos += kw_len;
    line = skip_blanks(line);
    size_t len = word_len(line);
    TEST_ASSERT_TRUE(len > 0 && len < WORD_CAP);
    memcpy(out->name, line.pos, len);
    out->name[len] = '\0';
    line.pos += len;
    line = skip_blanks(line);
    text_buf buf = {out->text, TEXT_CAP, 0};
    buf_reset(&buf);
    if (line.pos < line.end && *line.pos == '"') {
        put_quoted(&buf, line);
    } else {
        put_words(&buf, line);
    }
    return true;
}

/* The next line of *rest without its LF or CRLF. */
static span next_line(span *rest) {
    span line = *rest;
    const char *newline = memchr(rest->pos, '\n', (size_t)(rest->end - rest->pos));
    if (newline != NULL) {
        line.end = newline;
        rest->pos = newline + 1;
    } else {
        rest->pos = rest->end;
    }
    if (line.end > line.pos && line.end[-1] == '\r') {
        line.end--;
    }
    return line;
}

/* Checks one abc.rc line against the table row at *index when it is an `alias` line. */
static void check_rc_line(span line, uint32_t *index) {
    static rc_line parsed;
    if (!parse_alias_line(line, &parsed)) {
        return;
    }
    const odin3_abc_alias *alias = odin3_abc_alias_at(*index);
    TEST_ASSERT_NOT_NULL_MESSAGE(alias, parsed.name);
    TEST_ASSERT_EQUAL_STRING(parsed.name, alias->name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(parsed.text, alias->expansion, parsed.name);
    (*index)++;
}

static void test_matches_abc_rc(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_file_read_all(ODIN3_ABC_RC, RC_MAX_BYTES, &rc));
    span rest = {(const char *)rc.data, (const char *)rc.data + rc.len};
    uint32_t index = 0;
    while (rest.pos < rest.end) {
        check_rc_line(next_line(&rest), &index);
    }
    TEST_ASSERT_EQUAL_UINT32(odin3_abc_alias_count(), index);
    TEST_ASSERT_EQUAL_UINT32(ABC_RC_ALIASES, index);
}

static void check_usable_word(const pair *item) {
    const odin3_abc_alias *alias = odin3_abc_alias_find(item->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(alias, item->name);
    TEST_ASSERT_EQUAL_STRING(item->text, alias->expansion);
    char word[WORD_CAP];
    (void)take_word(alias->expansion, word);
    TEST_ASSERT_TRUE_MESSAGE(is_allowed(odin3_abc_command_find(word)), word);
}

static void test_usable_one_word_aliases(void) {
    for (uint32_t i = 0; i < COUNT_OF(k_usable_words); i++) {
        check_usable_word(&k_usable_words[i]);
    }
}

/* name's command has class cls (OTHER: odin3_abc_command_find gives NULL). */
static void expect_class(const char *name, uint8_t cls) {
    const odin3_abc_command_info *row = odin3_abc_command_find(name);
    if (cls == ODIN3_ABC_CLASS_OTHER) {
        TEST_ASSERT_NULL_MESSAGE(row, name);
        return;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(row, name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(cls, row->cls, name);
}

static void check_refused_word(const refused *item) {
    const odin3_abc_alias *alias = odin3_abc_alias_find(item->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(alias, item->name);
    char word[WORD_CAP];
    (void)take_word(alias->expansion, word);
    expect_class(word, item->cls);
}

static void test_refused_one_word_aliases(void) {
    for (uint32_t i = 0; i < COUNT_OF(k_refused_words); i++) {
        check_refused_word(&k_refused_words[i]);
    }
}

static void check_usable_script(const pair *item) {
    static char full[TEXT_CAP];
    const odin3_abc_alias *alias = odin3_abc_alias_find(item->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(alias, item->name);
    TEST_ASSERT_EQUAL_STRING(item->text, alias->expansion);
    (void)expand_all(item->name, full);
    char word[WORD_CAP] = "";
    TEST_ASSERT_FALSE_MESSAGE(first_refused(full, word), word);
}

static void test_usable_script_aliases(void) {
    for (uint32_t i = 0; i < COUNT_OF(k_usable_scripts); i++) {
        check_usable_script(&k_usable_scripts[i]);
    }
}

/* The first refused command of the expansion is the stopping one, and it is a row. */
static void check_refused_script(const pair *item) {
    static char full[TEXT_CAP];
    TEST_ASSERT_NOT_NULL_MESSAGE(odin3_abc_alias_find(item->name), item->name);
    (void)expand_all(item->name, full);
    char word[WORD_CAP] = "";
    TEST_ASSERT_TRUE_MESSAGE(first_refused(full, word), item->name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(item->text, word, item->name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rows_named(word), word);
}

static void test_refused_script_aliases(void) {
    for (uint32_t i = 0; i < COUNT_OF(k_refused_scripts); i++) {
        check_refused_script(&k_refused_scripts[i]);
    }
}

static uint32_t times_listed(const char *name) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < COUNT_OF(k_usable_words); i++) {
        hits += strcmp(k_usable_words[i].name, name) == 0;
    }
    for (uint32_t i = 0; i < COUNT_OF(k_refused_words); i++) {
        hits += strcmp(k_refused_words[i].name, name) == 0;
    }
    for (uint32_t i = 0; i < COUNT_OF(k_usable_scripts); i++) {
        hits += strcmp(k_usable_scripts[i].name, name) == 0;
    }
    for (uint32_t i = 0; i < COUNT_OF(k_refused_scripts); i++) {
        hits += strcmp(k_refused_scripts[i].name, name) == 0;
    }
    return hits;
}

/* §7.4's four lists partition the table. */
static void test_lists_cover_every_alias(void) {
    uint32_t count = odin3_abc_alias_count();
    TEST_ASSERT_EQUAL_UINT32(count, COUNT_OF(k_usable_words) + COUNT_OF(k_refused_words) +
                                        COUNT_OF(k_usable_scripts) + COUNT_OF(k_refused_scripts));
    for (uint32_t i = 0; i < count; i++) {
        const char *name = odin3_abc_alias_at(i)->name;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, times_listed(name), name);
    }
}

static void expect_row(const char *name, uint8_t cls, uint8_t effect) {
    const odin3_abc_command_info *row = odin3_abc_command_find(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(row, name);
    TEST_ASSERT_EQUAL_STRING(name, row->name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(cls, row->cls, name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(effect, row->effect, name);
}

/* A list of command names. */
typedef struct name_list {
    const char *const *names;
    uint32_t count;
} name_list;

#define NAME_LIST(a) ((name_list){(a), COUNT_OF(a)})

static void expect_rows(name_list list, uint8_t cls, uint8_t effect) {
    for (uint32_t i = 0; i < list.count; i++) {
        expect_row(list.names[i], cls, effect);
    }
}

static void test_allow_rows(void) {
    expect_row("strash", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG);
    expect_row("sweep", ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP);
    expect_row("if", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_MAPPED);
    expect_row("mfs2", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP);
    expect_row("&get", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_GET);
    expect_row("&put", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_NTK_PUT);
    expect_row("&dch", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG);
    expect_row("&if", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_MAPPED);
    expect_row("&mfs", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_KEEP);
    expect_row("print_stats", ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE);
    expect_row("read_blif", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE);
    expect_row("quit", ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE);
    expect_row("retime", ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE);
    TEST_ASSERT_FALSE(is_allowed(odin3_abc_command_find("print_constr")));
    TEST_ASSERT_FALSE(is_allowed(odin3_abc_command_find("print_status")));
    TEST_ASSERT_NULL(odin3_abc_command_find("frobnicate"));
}

/* §7.5 row by row: every allowed command but print_*, with its class and effect. */
static void test_allow_rows_complete(void) {
    static const char *const k_opt_aig[] = {
        "strash", "balance", "rewrite", "refactor", "resub",       "dc2",  "drw",
        "drf",    "dch",     "ifraig",  "fraig",    "fraig_sweep", "scorr"};
    static const char *const k_opt_keep[] = {"sweep", "cleanup", "scleanup"};
    static const char *const k_map_keep[] = {"mfs", "mfs2", "lutpack"};
    static const char *const k_gia_aig[] = {"&st", "&synch2", "&dch"};
    static const char *const k_print[] = {"&ps", "time", "echo"};
    expect_rows(NAME_LIST(k_opt_aig), ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_AIG);
    expect_rows(NAME_LIST(k_opt_keep), ODIN3_ABC_CLASS_OPT, ODIN3_ABC_FX_NTK_KEEP);
    expect_rows(NAME_LIST(k_map_keep), ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_KEEP);
    expect_rows(NAME_LIST(k_gia_aig), ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_AIG);
    expect_rows(NAME_LIST(k_print), ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE);
}

static uint32_t allowed_print_star_rows(void) {
    uint32_t allowed = 0;
    for (uint32_t i = 0; i < odin3_abc_command_count(); i++) {
        const odin3_abc_command_info *row = odin3_abc_command_at(i);
        allowed += strncmp(row->name, "print_", strlen("print_")) == 0 && is_allowed(row);
    }
    return allowed;
}

/* The 32 print_* commands at the pin but print_constr and print_status (which take files). */
static void test_print_rows(void) {
    static const char *const k_print_star[] = {
        "print_auto",    "print_box",    "print_buf",   "print_cell",    "print_cone",
        "print_delay",   "print_dsd",    "print_exdc",  "print_factor",  "print_fanio",
        "print_gates",   "print_genlib", "print_gs",    "print_io",      "print_kmap",
        "print_latch",   "print_level",  "print_lib",   "print_library", "print_lut",
        "print_mffc",    "print_mint",   "print_miter", "print_npn",     "print_profile",
        "print_sharing", "print_stats",  "print_supp",  "print_symm",    "print_tim",
        "print_unate",   "print_xcut"};
    TEST_ASSERT_EQUAL_UINT32(PRINT_STAR_ROWS, COUNT_OF(k_print_star));
    expect_rows(NAME_LIST(k_print_star), ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_FX_NONE);
    TEST_ASSERT_EQUAL_UINT32(PRINT_STAR_ROWS, allowed_print_star_rows());
    expect_row("print_constr", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE);
    expect_row("print_status", ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE);
}

/* §7.5's "among others" and the refused aliases' stopping commands. */
static void test_refused_rows(void) {
    static const char *const k_files[] = {"read",      "write",    "&r",          "&w",
                                          "source",    "load",     "save",        "read_init",
                                          "write_hie", "read_hie", "read_verlib", "&read"};
    static const char *const k_process[] = {"quit",    "exit", "cd",    "ls",      "alias",
                                            "unalias", "set",  "unset", "history", "load_plugin"};
    static const char *const k_latches[] = {"retime", "dretime", "fretime", "zero",
                                            "init",   "undc",    "cycle"};
    expect_rows(NAME_LIST(k_files), ODIN3_ABC_CLASS_FILES, ODIN3_ABC_FX_NONE);
    expect_rows(NAME_LIST(k_process), ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_FX_NONE);
    expect_rows(NAME_LIST(k_latches), ODIN3_ABC_CLASS_LATCHES, ODIN3_ABC_FX_NONE);
}

/* A row of class OTHER is listed but found as "other"; &lf and &of have no row (PHASE3 #22). */
static void test_other_rows(void) {
    TEST_ASSERT_EQUAL_UINT32(1, rows_named("fraig_store"));
    TEST_ASSERT_NULL(odin3_abc_command_find("fraig_store"));
    TEST_ASSERT_NULL(odin3_abc_command_find("&lf"));
    TEST_ASSERT_NULL(odin3_abc_command_find("&of"));
}

static const char *expected_file_opts(const char *name) {
    if (strcmp(name, "if") == 0 || strcmp(name, "&if") == 0) {
        return "SJ";
    }
    return strcmp(name, "&ps") == 0 ? "D" : "";
}

static bool effect_fits(const odin3_abc_command_info *row) {
    switch (row->cls) {
    case ODIN3_ABC_CLASS_OPT:
        return row->effect == ODIN3_ABC_FX_NTK_AIG || row->effect == ODIN3_ABC_FX_NTK_KEEP;
    case ODIN3_ABC_CLASS_MAP:
        return row->effect == ODIN3_ABC_FX_NTK_MAPPED || row->effect == ODIN3_ABC_FX_NTK_KEEP;
    case ODIN3_ABC_CLASS_GIA:
        return row->effect >= ODIN3_ABC_FX_GIA_GET && row->effect <= ODIN3_ABC_FX_GIA_KEEP;
    default:
        return row->effect == ODIN3_ABC_FX_NONE;
    }
}

/* A unique name, no alias name, an effect of its class, and file options where read at the pin. */
static void check_row(const odin3_abc_command_info *row) {
    TEST_ASSERT_TRUE_MESSAGE(row->cls <= ODIN3_ABC_CLASS_LATCHES, row->name);
    TEST_ASSERT_TRUE_MESSAGE(effect_fits(row), row->name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected_file_opts(row->name), row->file_opts, row->name);
    TEST_ASSERT_NULL_MESSAGE(odin3_abc_alias_find(row->name), row->name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rows_named(row->name), row->name);
}

static void test_table_hygiene(void) {
    uint32_t count = odin3_abc_command_count();
    TEST_ASSERT_NULL(odin3_abc_command_at(count));
    TEST_ASSERT_NULL(odin3_abc_command_find(NULL));
    for (uint32_t i = 0; i < count; i++) {
        check_row(odin3_abc_command_at(i));
    }
}

static void test_alias_lookup_edges(void) {
    TEST_ASSERT_NULL(odin3_abc_alias_find(NULL));
    TEST_ASSERT_NULL(odin3_abc_alias_find("frobnicate"));
    TEST_ASSERT_NULL(odin3_abc_alias_find("balance"));
}

static void test_alias_table_ends(void) {
    TEST_ASSERT_EQUAL_UINT32(ABC_RC_ALIASES, odin3_abc_alias_count());
    TEST_ASSERT_NULL(odin3_abc_alias_at(ABC_RC_ALIASES));
    TEST_ASSERT_EQUAL_STRING("hi", odin3_abc_alias_at(0)->name);
    TEST_ASSERT_EQUAL_STRING("recadd3", odin3_abc_alias_at(ABC_RC_ALIASES - 1)->name);
}

static void test_test_aliases_hook(void) {
    static const odin3_abc_alias k_chain[] = {{"a0", "a1 -x"}, {"a1", "strash"}};
    odin3_abc_test_set_aliases(k_chain, COUNT_OF(k_chain));
    TEST_ASSERT_EQUAL_UINT32(COUNT_OF(k_chain), odin3_abc_alias_count());
    TEST_ASSERT_EQUAL_PTR(&k_chain[1], odin3_abc_alias_at(1));
    TEST_ASSERT_NULL(odin3_abc_alias_at(COUNT_OF(k_chain)));
    TEST_ASSERT_EQUAL_PTR(&k_chain[0], odin3_abc_alias_find("a0"));
    TEST_ASSERT_NULL(odin3_abc_alias_find("b"));
    odin3_abc_test_set_aliases(NULL, COUNT_OF(k_chain));
    TEST_ASSERT_EQUAL_UINT32(ABC_RC_ALIASES, odin3_abc_alias_count());
    TEST_ASSERT_EQUAL_STRING("balance", odin3_abc_alias_find("b")->expansion);
}

static int deepest_alias(void) {
    static char full[TEXT_CAP];
    int deepest = 0;
    for (uint32_t i = 0; i < odin3_abc_alias_count(); i++) {
        int depth = expand_all(odin3_abc_alias_at(i)->name, full);
        deepest = depth > deepest ? depth : deepest;
    }
    return deepest;
}

/* No real alias expands more than 3 levels; &resyn2rs and &compress2rs reach exactly 3. */
static void test_alias_depth_in_table(void) {
    static char full[TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(REAL_MAX_DEPTH, deepest_alias());
    TEST_ASSERT_EQUAL_INT(REAL_MAX_DEPTH, expand_all("&resyn2rs", full));
    TEST_ASSERT_EQUAL_INT(REAL_MAX_DEPTH, expand_all("&compress2rs", full));
    TEST_ASSERT_EQUAL_INT(1, expand_all("b", full));
    TEST_ASSERT_EQUAL_INT(1, expand_all("b -l", full));
    TEST_ASSERT_EQUAL_STRING("balance -l", full);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_matches_abc_rc);
    RUN_TEST(test_usable_one_word_aliases);
    RUN_TEST(test_refused_one_word_aliases);
    RUN_TEST(test_usable_script_aliases);
    RUN_TEST(test_refused_script_aliases);
    RUN_TEST(test_lists_cover_every_alias);
    RUN_TEST(test_allow_rows);
    RUN_TEST(test_allow_rows_complete);
    RUN_TEST(test_print_rows);
    RUN_TEST(test_refused_rows);
    RUN_TEST(test_other_rows);
    RUN_TEST(test_table_hygiene);
    RUN_TEST(test_alias_lookup_edges);
    RUN_TEST(test_alias_table_ends);
    RUN_TEST(test_test_aliases_hook);
    RUN_TEST(test_alias_depth_in_table);
    return UNITY_END();
}
