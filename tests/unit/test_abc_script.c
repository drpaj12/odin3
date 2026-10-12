/*
 * test_abc_script.c — the ABC script checker (3D spec §7.3, AB-21; plan Task 11). Every error
 * text is exact, at its line, and logged once as `abc: ` + the text (point (x)); every refusal
 * class; arguments, with every allowed command's file, string and crash options refused (I12);
 * aliases; the count; the mapping states (the BLIF & rule, PHASE3 #22's &if rule); the output;
 * the caps at and past their limits; and an OOM sweep.
 */
#include "abc_script_cases.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/script.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/sha256.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    AIG = ODIN3_ABC_EXCHANGE_AIG,
    BLIF = ODIN3_ABC_EXCHANGE_BLIF,
    NTK = ODIN3_ABC_SPACE_NTK,
    GIA = ODIN3_ABC_SPACE_GIA,
    BIG = ODIN3_ABC_MAX_SCRIPT_BYTES + 4096, /* room past the size cap */
    LINE_MAX_LOG = 1024,
    OOM_LIMIT = 4096,
    PATH_CAP = 512
};

static char g_last[LINE_MAX_LOG]; /* the last message logged */
static size_t g_lines;            /* messages logged, any level */
static char g_big[BIG];           /* scratch for the long scripts */
static char g_dir[PATH_CAP];      /* a temporary directory for the file tests */

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    g_lines++;
    size_t len = strnlen(msg, sizeof g_last - 1);
    memcpy(g_last, msg, len);
    g_last[len] = '\0';
}

void setUp(void) {
    g_lines = 0;
    g_last[0] = '\0';
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_aliases(NULL, 0);
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers -------------------------------------------------------------------------------- */

/* How a script is checked: --lut K (0: none) and the exchange. */
typedef struct run {
    uint32_t lut_k;
    uint32_t exchange;
} run;

static const run k_mem = {0, AIG};
static const run k_blif = {0, BLIF};
static const run k_mem6 = {6, AIG};

static odin3_bytes text_of(const char *text) {
    return (odin3_bytes){text, strlen(text)};
}

static odin3_status parse_bytes(odin3_bytes text, run how, odin3_abc_script *out) {
    odin3_abc_script_src src = {"t.abc", NULL, text};
    return odin3_abc_script_parse(&src, how.lut_k, how.exchange, out);
}

/* The text parses, logging nothing. */
static void accept_bytes(odin3_bytes text, run how, odin3_abc_script *out) {
    g_lines = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_bytes(text, how, out), g_last);
    TEST_ASSERT_EQUAL_size_t(0, g_lines);
    TEST_ASSERT_NOT_NULL(out->arena);
}

static void accept(const char *text, run how, odin3_abc_script *out) {
    accept_bytes(text_of(text), how, out);
}

/* The text is refused with PARSE and exactly the one line `abc: ` + msg; out is untouched. */
static void refuse_bytes(odin3_bytes text, run how, const char *msg) {
    odin3_abc_script out;
    memset(&out, 0xA5, sizeof out);
    odin3_abc_script before = out;
    g_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_bytes(text, how, &out));
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    char want[LINE_MAX_LOG];
    int wrote = snprintf(want, sizeof want, "abc: %s", msg);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, g_last);
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);
}

static void refuse(const char *text, run how, const char *msg) {
    refuse_bytes(text_of(text), how, msg);
}

/* The script's lines are exactly want[0..n). */
static void assert_lines(const odin3_abc_script *script, const char *const *want, uint32_t n) {
    TEST_ASSERT_EQUAL_UINT32(n, script->n_cmds);
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], odin3_abc_script_cmd(script, i)->line);
    }
}

/* No byte of line is one CmdSplitLine or the checker treats specially, no blank leads, trails or
 * doubles, and every byte is printable ASCII. */
static bool clean_line(const char *line) {
    if (line[0] == '\0' || line[0] == ' ') {
        return false;
    }
    for (const char *at = line; *at != '\0'; at++) {
        if (strchr("\"'!;#{}", *at) != NULL || *at < ' ' || *at > '~') {
            return false;
        }
        if (at[0] == ' ' && (at[1] == ' ' || at[1] == '\0')) {
            return false;
        }
    }
    return true;
}

static void assert_clean_lines(const odin3_abc_script *script) {
    for (uint32_t j = 0; j < script->n_cmds; j++) {
        TEST_ASSERT_TRUE_MESSAGE(clean_line(script->cmds[j].line), script->cmds[j].line);
        TEST_ASSERT_TRUE(script->cmds[j].src_line >= 1);
    }
}

/* Copies text's bytes, without its NUL, to dst. */
static void put_bytes(char *dst, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        dst[i] = text[i];
    }
}

/* Fills g_big with n copies of line (with its newline) and returns the bytes. */
static odin3_bytes repeat(const char *line, size_t n) {
    size_t len = strlen(line);
    TEST_ASSERT_TRUE(n * len < sizeof g_big);
    for (size_t i = 0; i < n; i++) {
        put_bytes(g_big + i * len, line);
    }
    g_big[n * len] = '\0';
    return (odin3_bytes){g_big, n * len};
}

/* g_big = head, then `#` comment lines of at most 4,096 bytes, to exactly total bytes. */
static odin3_bytes pad_to(const char *head, size_t total) {
    size_t at = strlen(head);
    TEST_ASSERT_TRUE(total < sizeof g_big && at <= total);
    put_bytes(g_big, head);
    while (at < total) {
        size_t line = total - at > ODIN3_ABC_MAX_LINE_BYTES ? ODIN3_ABC_MAX_LINE_BYTES : total - at;
        memset(g_big + at, 'x', line);
        g_big[at] = '#';
        g_big[at + line - 1] = '\n';
        at += line;
    }
    g_big[total] = '\0';
    return (odin3_bytes){g_big, total};
}

/* --- bytes (step 1) ------------------------------------------------------------------------- */

static void test_size_cap(void) {
    refuse_bytes(pad_to("if -K 6\n", ODIN3_ABC_MAX_SCRIPT_BYTES + 1), k_mem,
                 "t.abc: abc script is larger than 65536 bytes");
    odin3_abc_script script;
    accept_bytes(pad_to("if -K 6\n", ODIN3_ABC_MAX_SCRIPT_BYTES), k_mem, &script);
    TEST_ASSERT_EQUAL_UINT32(1, script.n_cmds);
    odin3_abc_script_free(&script);
}

static void test_nul_byte(void) {
    static const char text[] = "strash\nstrash\nst\0rash\nif -K 6\n";
    refuse_bytes((odin3_bytes){text, sizeof text - 1}, k_mem,
                 "t.abc:3: abc script contains a NUL byte");
    static const char comment[] = "strash\nstrash\n# \0\nif -K 6\n";
    refuse_bytes((odin3_bytes){comment, sizeof comment - 1}, k_mem,
                 "t.abc:3: abc script contains a NUL byte");
}

static void test_control_byte(void) {
    refuse("strash\nstrash\x01\nif -K 6\n", k_mem,
           "t.abc:2: byte 0x01 is not allowed outside a comment");
    refuse("strash\x7f", k_mem, "t.abc:1: byte 0x7f is not allowed outside a comment");
    refuse("strash\xc3\xa9", k_mem, "t.abc:1: byte 0xc3 is not allowed outside a comment");
    refuse("strash\rif -K 6\n", k_mem, "t.abc:1: byte 0x0d is not allowed outside a comment");
    refuse("if -K 6\v", k_mem, "t.abc:1: byte 0x0b is not allowed outside a comment");
    odin3_abc_script script;
    accept("strash\nstrash #\x01\xff\r\nif -K 6\n", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT32(3, script.n_cmds);
    odin3_abc_script_free(&script);
}

static void test_line_cap(void) {
    char *line = g_big;
    memset(line, ' ', ODIN3_ABC_MAX_LINE_BYTES + 1);
    put_bytes(line, "if -K 6");
    odin3_bytes over = {line, ODIN3_ABC_MAX_LINE_BYTES + 1};
    refuse_bytes(over, k_mem, "t.abc:1: line longer than 4096 bytes");
    odin3_abc_script script;
    put_bytes(line + ODIN3_ABC_MAX_LINE_BYTES, "\r\n");
    accept_bytes((odin3_bytes){line, ODIN3_ABC_MAX_LINE_BYTES + 2}, k_mem, &script);
    TEST_ASSERT_EQUAL_STRING("if -K 6", script.cmds[0].line);
    odin3_abc_script_free(&script);
    /* {K} counts its three bytes */
    put_bytes(line + ODIN3_ABC_MAX_LINE_BYTES - 2, "{K}");
    refuse_bytes(over, k_mem6, "t.abc:1: line longer than 4096 bytes");
    put_bytes(line + ODIN3_ABC_MAX_LINE_BYTES - 3, "{K}\n");
    accept_bytes(over, k_mem6, &script);
    TEST_ASSERT_EQUAL_STRING("if -K 6 6", script.cmds[0].line);
    odin3_abc_script_free(&script);
}

static void test_crlf(void) {
    odin3_abc_script lf;
    odin3_abc_script crlf;
    accept("strash\n\nb -l # c\nif -K 6\n", k_blif, &lf);
    accept("strash\r\n\r\nb -l # c\r\nif -K 6\r\n", k_blif, &crlf);
    TEST_ASSERT_EQUAL_UINT32(lf.n_cmds, crlf.n_cmds);
    for (uint32_t i = 0; i < lf.n_cmds; i++) {
        TEST_ASSERT_EQUAL_STRING(lf.cmds[i].line, crlf.cmds[i].line);
        TEST_ASSERT_EQUAL_UINT32(lf.cmds[i].src_line, crlf.cmds[i].src_line);
    }
    TEST_ASSERT_EQUAL_UINT32(4, crlf.cmds[2].src_line);
    odin3_abc_script_free(&lf);
    odin3_abc_script_free(&crlf);
}

/* Steps 1-6 run over every byte before any command is checked: a byte error on line 3 wins
 * over an unknown command on line 1. */
static void test_byte_pass_first(void) {
    refuse("strsh\nstrash\n\x02", k_mem, "t.abc:3: byte 0x02 is not allowed outside a comment");
    refuse("strsh\nread \"x\"", k_mem, "t.abc:2: quotes are not allowed in abc scripts");
}

/* --- quotes, shell, {K}, words (steps 3, 5, 6) ---------------------------------------------- */

static void test_quotes_and_shell(void) {
    refuse("read \"x\"", k_mem, "t.abc:1: quotes are not allowed in abc scripts");
    refuse("strash\nread 'x'", k_mem, "t.abc:2: quotes are not allowed in abc scripts");
    refuse("!ls", k_mem, "t.abc:1: '!' (shell escape) is not allowed in abc scripts");
    refuse("strash; !", k_mem, "t.abc:1: '!' (shell escape) is not allowed in abc scripts");
    refuse("echo !x", k_mem, "t.abc:1: '!' (shell escape) is not allowed in abc scripts");
    refuse("echo a!b", k_mem,
           "t.abc:1: argument 'a!b' of 'echo' is neither an option nor a number");
}

static void test_k_substitution(void) {
    odin3_abc_script script;
    accept("if -K {K}", k_mem6, &script);
    TEST_ASSERT_EQUAL_STRING("if -K 6", script.cmds[0].line);
    TEST_ASSERT_TRUE(script.uses_k);
    odin3_abc_script_free(&script);
    accept("if -K{K} -C {K}{K}", (run){16, AIG}, &script);
    TEST_ASSERT_EQUAL_STRING("if -K16 -C 1616", script.cmds[0].line);
    odin3_abc_script_free(&script);
    accept("# {K} in a comment\nif -K 6", k_mem, &script);
    TEST_ASSERT_FALSE(script.uses_k);
    odin3_abc_script_free(&script);
}

/* Review Focus 4. */
static void test_refuse_k_without_lut(void) {
    refuse("if -K {K}", k_mem, "t.abc:1: the script uses {K} but abc was given no --lut");
}

static void test_unknown_substitution(void) {
    refuse("if -K {N}", k_mem6, "t.abc:1: unknown substitution '{N}' (only {K} is defined)");
    refuse("if -K {k}", k_mem6, "t.abc:1: unknown substitution '{k}' (only {K} is defined)");
    refuse("if -K {6", k_mem6, "t.abc:1: unknown substitution '{6' (only {K} is defined)");
    refuse("if -K {", k_mem6, "t.abc:1: unknown substitution '{' (only {K} is defined)");
    refuse("if -K {K", k_mem6, "t.abc:1: unknown substitution '{K' (only {K} is defined)");
    refuse("if -K }", k_mem6, "t.abc:1: unknown substitution '}' (only {K} is defined)");
    refuse("strash\nif -K {KK}", k_mem6,
           "t.abc:2: unknown substitution '{KK}' (only {K} is defined)");
}

static void test_word_cap(void) {
    char text[512] = "if -K 6";
    for (int i = 0; i < ODIN3_ABC_MAX_WORDS - 3; i++) {
        strcat(text, " -v");
    }
    odin3_abc_script script;
    accept(text, k_mem, &script);
    odin3_abc_script_free(&script);
    strcat(text, " -v");
    refuse(text, k_mem, "t.abc:1: command has more than 64 words");
}

/* --- aliases (step 7) ----------------------------------------------------------------------- */

static void test_alias_one_word(void) {
    odin3_abc_script script;
    accept("b -l\nif -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_STRING("balance -l", script.cmds[0].line);
    TEST_ASSERT_EQUAL_STRING("b", script.cmds[0].alias);
    TEST_ASSERT_EQUAL_UINT32(1, script.cmds[0].src_line);
    TEST_ASSERT_NULL(script.cmds[1].alias);
    TEST_ASSERT_EQUAL_UINT32(2, script.cmds[1].src_line);
    odin3_abc_script_free(&script);
}

/* resyn = `b; rw; rwz; b; rwz; b`: the extra word goes to the last command only. */
static void test_alias_extra_words(void) {
    static const char *const want[] = {"balance",    "rewrite",    "rewrite -z", "balance",
                                       "rewrite -z", "balance -v", "if -K 6"};
    odin3_abc_script script;
    accept("resyn -v\nif -K 6", k_mem, &script);
    assert_lines(&script, want, 7);
    for (uint32_t i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_STRING("resyn", script.cmds[i].alias);
        TEST_ASSERT_EQUAL_UINT32(1, script.cmds[i].src_line);
    }
    odin3_abc_script_free(&script);
}

/* &resyn2rs = `&put; resyn2rs; &get` (depth 3): the extra word reaches &get, not resyn2rs. */
static void test_alias_nested(void) {
    odin3_abc_script script;
    accept("&resyn2rs -v\n&if -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT32(21, script.n_cmds);
    TEST_ASSERT_EQUAL_STRING("&put", script.cmds[0].line);
    TEST_ASSERT_EQUAL_STRING("balance", script.cmds[1].line);
    TEST_ASSERT_EQUAL_STRING("resub -K 6", script.cmds[2].line);
    TEST_ASSERT_EQUAL_STRING("balance", script.cmds[18].line);
    TEST_ASSERT_EQUAL_STRING("&get -v", script.cmds[19].line);
    for (uint32_t i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_STRING("&resyn2rs", script.cmds[i].alias);
    }
    TEST_ASSERT_EQUAL_UINT32(GIA, script.final_space);
    odin3_abc_script_free(&script);
}

enum { NAME_CAP = 8, CHAIN_MAX = ODIN3_ABC_MAX_ALIAS_DEPTH + 1 };

/* The test alias table: a0 -> a1 -> … -> a<n-1> -> `if -K 6`, n nested expansions. */
typedef struct chain {
    odin3_abc_alias table[CHAIN_MAX];
    char names[CHAIN_MAX][NAME_CAP];
} chain;

static void set_chain(chain *ch, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int wrote = snprintf(ch->names[i], NAME_CAP, "a%u", (unsigned)i);
        TEST_ASSERT_TRUE(wrote > 0 && wrote < NAME_CAP);
    }
    for (uint32_t i = 0; i < n; i++) {
        ch->table[i] = (odin3_abc_alias){ch->names[i], i + 1 < n ? ch->names[i + 1] : "if -K 6"};
    }
    odin3_abc_test_set_aliases(ch->table, n);
}

static void test_alias_depth(void) {
    chain ch;
    set_chain(&ch, ODIN3_ABC_MAX_ALIAS_DEPTH + 1);
    refuse("a0", k_mem, "t.abc:1: alias 'a0' expands more than 8 levels deep");
    refuse("strash\na0 -v", k_mem, "t.abc:2: alias 'a0' expands more than 8 levels deep");
    set_chain(&ch, ODIN3_ABC_MAX_ALIAS_DEPTH);
    odin3_abc_script script;
    accept("a0 -v", k_mem, &script);
    TEST_ASSERT_EQUAL_STRING("if -K 6 -v", script.cmds[0].line);
    TEST_ASSERT_EQUAL_STRING("a0", script.cmds[0].alias);
    odin3_abc_script_free(&script);
    /* a cycle stops at the cap too */
    static const odin3_abc_alias loop[] = {{"x", "strash; x"}};
    odin3_abc_test_set_aliases(loop, 1);
    refuse("x", k_mem, "t.abc:1: alias 'x' expands more than 8 levels deep");
}

/* One abc.rc alias, then a mapper: accepted with clean lines, or refused with one line. */
static void sweep_alias(const char *name, run how) {
    char text[128];
    int wrote = snprintf(text, sizeof text, "%s\nif -K {K}", name);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    odin3_abc_script script = {0};
    g_lines = 0;
    odin3_status st = parse_bytes(text_of(text), how, &script);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_PARSE, text);
    TEST_ASSERT_EQUAL_size_t(st == ODIN3_OK ? 0 : 1, g_lines);
    assert_clean_lines(&script);
    odin3_abc_script_free(&script);
}

/* Every abc.rc alias in both exchanges; §7.4's usable ones are accepted in memory. */
static void test_alias_table_sweep(void) {
    for (uint32_t i = 0; i < odin3_abc_alias_count(); i++) {
        sweep_alias(odin3_abc_alias_at(i)->name, (run){6, AIG});
        sweep_alias(odin3_abc_alias_at(i)->name, (run){6, BLIF});
    }
    static const char *const usable[] = {
        "resyn; if -K 6",        "resyn2; if -K 6",      "resyn2a; if -K 6", "resyn3; if -K 6",
        "compress; if -K 6",     "compress2; if -K 6",   "rwsat; if -K 6",   "drwsat2; if -K 6",
        "src_rw; if -K 6",       "src_rs; if -K 6",      "src_rws; if -K 6", "resyn2rs; if -K 6",
        "r2rs; if -K 6",         "compress2rs; if -K 6", "c2rs; if -K 6",    "&resyn2rs; if -K 6",
        "&compress2rs; if -K 6", "&sw_; if -K 6",        "b; rwz; if -K 6"};
    for (size_t i = 0; i < sizeof usable / sizeof usable[0]; i++) {
        odin3_abc_script script;
        accept(usable[i], k_mem, &script);
        odin3_abc_script_free(&script);
    }
}

/* --- the allow-list (step 8; Review Focus 4) ------------------------------------------------ */

static void test_refuse_unknown_command(void) {
    refuse("strsh", k_mem,
           "t.abc:1: abc command 'strsh' is not an allowed ABC command (3D spec §7.5)");
}

static void test_refuse_file_command(void) {
    refuse("write_blif x.blif", k_mem,
           "t.abc:1: abc command 'write_blif' is not allowed: abc scripts cannot read or write "
           "files");
}

static void test_refuse_classes(void) {
    refuse("quit", k_mem,
           "t.abc:1: abc command 'quit' is not allowed: abc scripts cannot control the ABC "
           "process");
    refuse("retime", k_mem,
           "t.abc:1: abc command 'retime' is not allowed: it retimes or changes initial values "
           "(PHASE3 #12)");
    refuse("r x.blif", k_mem,
           "t.abc:1: abc command 'read' (from alias 'r') is not allowed: abc scripts cannot read "
           "or write files");
    refuse("print_box", k_mem,
           "t.abc:1: abc command 'print_box' is not allowed: it crashes ABC at the pin "
           "(a3001b72)");
    refuse("strash\nchoice", k_mem,
           "t.abc:2: abc command 'fraig_store' (from alias 'choice') is not an allowed ABC "
           "command (3D spec §7.5)");
    refuse("strash; q", k_mem,
           "t.abc:1: abc command 'quit' (from alias 'q') is not allowed: abc scripts cannot "
           "control the ABC process");
}

/* The class ending of each refused row. */
static const char *class_ending(uint8_t cls) {
    switch (cls) {
    case ODIN3_ABC_CLASS_FILES:
        return "is not allowed: abc scripts cannot read or write files";
    case ODIN3_ABC_CLASS_PROCESS:
        return "is not allowed: abc scripts cannot control the ABC process";
    case ODIN3_ABC_CLASS_LATCHES:
        return "is not allowed: it retimes or changes initial values (PHASE3 #12)";
    case ODIN3_ABC_CLASS_CRASH:
        return "is not allowed: it crashes ABC at the pin (a3001b72)";
    default:
        return "is not an allowed ABC command (3D spec §7.5)";
    }
}

/* An allowed row, then a mapper, is accepted in memory; a refused one gets its class's ending;
 * a row of class OTHER is "not an allowed ABC command". */
static void check_row(const odin3_abc_command_info *row) {
    char text[128];
    int wrote = snprintf(text, sizeof text, "%s\nif -K 6", row->name);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    if (row->cls >= ODIN3_ABC_CLASS_OPT && row->cls <= ODIN3_ABC_CLASS_PRINT) {
        odin3_abc_script script;
        accept(text, k_mem, &script);
        TEST_ASSERT_EQUAL_STRING(row->name, script.cmds[0].line);
        odin3_abc_script_free(&script);
        return;
    }
    char want[256];
    wrote = snprintf(want, sizeof want, "t.abc:1: abc command '%s' %s", row->name,
                     class_ending(row->cls));
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(text, k_mem, want);
}

static void test_every_row(void) {
    for (uint32_t i = 0; i < odin3_abc_command_count(); i++) {
        check_row(odin3_abc_command_at(i));
    }
}

/* --- arguments (step 9) --------------------------------------------------------------------- */

static void refuse_argument(const char *word) {
    char text[64];
    char want[160];
    int wrote = snprintf(text, sizeof text, "if -K 6 %s", word);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    wrote = snprintf(want, sizeof want,
                     "t.abc:1: argument '%s' of 'if' is neither an option nor a number", word);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(text, k_mem, want);
}

static void test_arguments(void) {
    refuse("if -K six", k_mem, "t.abc:1: argument 'six' of 'if' is neither an option nor a number");
    static const char *const bad[] = {"6.", ".5",  "6e",     "6e+", "--6",   "6x", "-",
                                      "--", "-K-", "x.blif", "+",   "1.2.3", "-_", "6,5"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        refuse_argument(bad[i]);
    }
    odin3_abc_script script;
    accept("if -K 6 -C 8 -D -6 -E +6 -F 6.5 -W 6e3 -X 6.5E-3 -Y 0.25e+2 -K6 -vm", k_mem, &script);
    odin3_abc_script_free(&script);
    accept("echo hello-world\nif -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_STRING("echo hello-world", script.cmds[0].line);
    odin3_abc_script_free(&script);
    refuse("strash\nb -l x\nif -K 6", k_mem,
           "t.abc:2: argument 'x' of 'balance' (from alias 'b') is neither an option nor a "
           "number");
}

/* `<cmd> -<letter> 6` and `<cmd> <cluster ending in letter>` give want. */
static void refuse_option(const odin3_abc_command_info *row, char letter, const char *want) {
    char text[64];
    int wrote = snprintf(text, sizeof text, "%s -%c 6", row->name, letter);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    refuse(text, k_mem, want);
    wrote = snprintf(text, sizeof text, "%s -vK6%c", row->name, letter);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    refuse(text, k_mem, want);
}

/* Every file and string option of the row (I12); returns their number. */
static uint32_t check_file_options(const odin3_abc_command_info *row) {
    uint32_t seen = 0;
    for (const char *at = row->file_opts; *at != '\0'; at++, seen++) {
        char want[256];
        int wrote = snprintf(want, sizeof want,
                             "t.abc:1: option '-%c' of '%s' reads or writes a file or takes a "
                             "string and is not allowed",
                             *at, row->name);
        TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
        refuse_option(row, *at, want);
    }
    return seen;
}

/* Every crash option of the row; returns their number. */
static uint32_t check_crash_options(const odin3_abc_command_info *row) {
    uint32_t seen = 0;
    for (const char *at = row->crash_opts; *at != '\0'; at++, seen++) {
        char want[256];
        int wrote = snprintf(want, sizeof want,
                             "t.abc:1: option '-%c' of '%s' crashes ABC at the pin (a3001b72) "
                             "and is not allowed",
                             *at, row->name);
        TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
        refuse_option(row, *at, want);
    }
    return seen;
}

/* Every row's file and string options and crash options, each alone and inside a cluster. */
static void test_file_and_crash_options(void) {
    uint32_t file_seen = 0;
    uint32_t crash_seen = 0;
    for (uint32_t i = 0; i < odin3_abc_command_count(); i++) {
        file_seen += check_file_options(odin3_abc_command_at(i));
        crash_seen += check_crash_options(odin3_abc_command_at(i));
    }
    /* print_stats bd, rewrite x, resub PD, scorr Pef, if SJf, &if SJ, &ps D; if s, &if s,
     * &dch m, &put e, print_auto O (Task 10, fix rounds 1-3) */
    TEST_ASSERT_EQUAL_UINT32(14, file_seen);
    TEST_ASSERT_EQUAL_UINT32(5, crash_seen);
}

static void test_options_from_alias(void) {
    refuse("psb", k_mem,
           "t.abc:1: option '-b' of 'print_stats' (from alias 'psb') reads or writes a file or "
           "takes a string and is not allowed");
    refuse("strash; sif", k_mem,
           "t.abc:1: option '-s' of 'if' (from alias 'sif') crashes ABC at the pin (a3001b72) "
           "and is not allowed");
}

/* --- the count (step 10) -------------------------------------------------------------------- */

static void test_count_cap(void) {
    refuse_bytes(repeat("b\n", ODIN3_ABC_MAX_COMMANDS + 1), k_mem,
                 "t.abc:1025: script expands to more than 1024 commands");
    odin3_bytes text = repeat("b\n", ODIN3_ABC_MAX_COMMANDS);
    put_bytes(g_big + text.len, "if -K 6\n");
    text.len += strlen("if -K 6\n");
    refuse_bytes(text, k_mem, "t.abc:1025: script expands to more than 1024 commands");
    odin3_abc_script script;
    accept_bytes((odin3_bytes){g_big + 2, text.len - 2}, k_mem, &script);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_MAX_COMMANDS, script.n_cmds);
    const odin3_abc_cmd *last = &script.cmds[ODIN3_ABC_MAX_COMMANDS - 1];
    TEST_ASSERT_EQUAL_STRING("if -K 6", last->line);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_MAX_COMMANDS, last->src_line);
    odin3_abc_script_free(&script);
    /* through an alias: 102 resyn2 give 1,020 commands, the 103rd crosses the cap */
    refuse_bytes(repeat("resyn2\n", 103), k_mem,
                 "t.abc:103: script expands to more than 1024 commands");
}

/* --- spaces and mapping (step 11) ----------------------------------------------------------- */

static const char k_unmapped[] = "t.abc: the script leaves the network unmapped; end it with a LUT "
                                 "mapper such as 'if -K {K}'";

static void test_refuse_unmapped_end(void) {
    static const char *const unmapped[] = {"strash",
                                           "",
                                           "resyn2",
                                           "# only a comment\n",
                                           "\n;;\n",
                                           "if -K 6; strash",
                                           "&if -K 6; &st",
                                           "print_stats",
                                           "&if -K 6; &st; &put"};
    for (size_t i = 0; i < sizeof unmapped / sizeof unmapped[0]; i++) {
        refuse(unmapped[i], k_mem, k_unmapped);
    }
}

static void test_blif_gia_empty(void) {
    refuse("&if -K 6", k_blif,
           "t.abc:1: '&if' needs a network in the & space, which the BLIF exchange starts empty; "
           "add '&get'");
    refuse("strash\n&ps", k_blif,
           "t.abc:2: '&ps' needs a network in the & space, which the BLIF exchange starts empty; "
           "add '&get'");
    refuse("&resyn2rs", k_blif,
           "t.abc:1: '&put' (from alias '&resyn2rs') needs a network in the & space, which the "
           "BLIF exchange starts empty; add '&get'");
    odin3_abc_script script;
    accept("&if -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT8(GIA, script.final_space);
    odin3_abc_script_free(&script);
}

static void expect_space(const char *text, run how, uint8_t space) {
    odin3_abc_script script;
    accept(text, how, &script);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(space, script.final_space, text);
    odin3_abc_script_free(&script);
}

static void test_final_space(void) {
    expect_space("strash; if -K 6; mfs2", k_blif, NTK);
    expect_space("&get -n; &if -K 6", k_blif, GIA);
    expect_space("&if -K 6; &put", k_mem, NTK);
    expect_space("strash; if -K 6; &get -m; print_stats", k_mem, GIA);
}

/* PHASE3 #22: ABC's &if refuses a mapped & network ("Current AIG has mapping. Run &st"). */
static void test_if_after_mapping(void) {
    static const char *const mapped_again[] = {"strash; if -K 6; &get -m\n&if -K 6",
                                               "&if -K 6\n&if -K 4", "&if -K 6; &mfs\n&if -K 4",
                                               "strash; if -K 6; &get -m -n -m -m\n&if -K 6"};
    for (size_t i = 0; i < sizeof mapped_again / sizeof mapped_again[0]; i++) {
        refuse(mapped_again[i], k_mem,
               "t.abc:2: '&if' needs an unmapped network in the & space, which is mapped here; "
               "add '&st'");
    }
    static const char *const fine[] = {"strash; if -K 6; &get -m; &st; &if -K 6",
                                       "strash; &get -m; &if -K 6",
                                       "strash; if -K 6; &get; &if -K 6",
                                       "strash; if -K 6; &get -m -m; &if -K 6",
                                       "strash; if -K 6; &get -mc; &if -K 6",
                                       "strash; if -K 6; &get 5 -m; &if -K 6",
                                       "&if -K 6 -g; &if -K 6"};
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        expect_space(fine[i], k_mem, GIA);
    }
}

/* if -g/-x/-y/-u and &if -g/-x/-y balance into an AIG; they do not map. */
static void test_balancing_mappers(void) {
    static const char *const unmapped[] = {"if -K 6 -g",  "if -K 6 -x",  "if -K 6 -y",
                                           "if -K 6 -u",  "if -K 6 -vg", "if -K 6 -g -g",
                                           "&if -K 6 -g", "&if -K 6 -x", "&if -K 6 -y"};
    for (size_t i = 0; i < sizeof unmapped / sizeof unmapped[0]; i++) {
        refuse(unmapped[i], k_mem, k_unmapped);
    }
    expect_space("strash; if -g; if -K 6", k_mem, NTK);
    expect_space("&if -K 6 -u", k_mem, GIA);
}

/* --- output (step 12) ----------------------------------------------------------------------- */

/* A shared accepted case is accepted with clean lines; when BLIF accepts, so does the in-memory
 * exchange, with the same lines. */
static void check_case(const abc_script_case *cs) {
    odin3_abc_script script;
    accept(cs->text, (run){cs->lut_k, cs->exchange}, &script);
    assert_clean_lines(&script);
    if (cs->exchange == BLIF) {
        odin3_abc_script mem;
        accept(cs->text, (run){cs->lut_k, AIG}, &mem);
        TEST_ASSERT_EQUAL_UINT32(script.n_cmds, mem.n_cmds);
        for (uint32_t j = 0; j < script.n_cmds; j++) {
            TEST_ASSERT_EQUAL_STRING(script.cmds[j].line, mem.cmds[j].line);
        }
        odin3_abc_script_free(&mem);
    }
    odin3_abc_script_free(&script);
}

static void test_accepted_cases(void) {
    for (size_t i = 0; i < k_abc_script_accepted_count; i++) {
        check_case(&k_abc_script_accepted[i]);
    }
}

static void test_sha256(void) {
    static const char text[] = "strash # first\nif -K {K}\n";
    odin3_abc_script script;
    accept(text, k_mem6, &script);
    odin3_sha256 sha;
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    char hex[ODIN3_SHA256_HEX_CHARS];
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, text, sizeof text - 1);
    odin3_sha256_final(&sha, digest);
    odin3_sha256_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING(hex, script.sha256_hex);
    TEST_ASSERT_EQUAL_size_t(64, strlen(script.sha256_hex));
    odin3_abc_script_free(&script);
    /* the identity is of the bytes, not of the commands */
    accept("strash  # first\nif -K {K}\n", k_mem6, &script);
    TEST_ASSERT_FALSE(strcmp(hex, script.sha256_hex) == 0);
    odin3_abc_script_free(&script);
}

static void test_accessors(void) {
    odin3_abc_script script;
    accept("strash\nif -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_PTR(&script.cmds[1], odin3_abc_script_cmd(&script, 1));
    TEST_ASSERT_NULL(odin3_abc_script_cmd(&script, 2));
    TEST_ASSERT_NULL(odin3_abc_script_cmd(NULL, 0));
    odin3_abc_script_free(&script);
    TEST_ASSERT_NULL(script.arena);
    TEST_ASSERT_EQUAL_UINT32(0, script.n_cmds);
    odin3_abc_script_free(&script); /* a zeroed script is a no-op */
    odin3_abc_script_free(NULL);
}

static void test_invalid_arguments(void) {
    odin3_abc_script script = {0};
    odin3_abc_script_src src = {"t.abc", NULL, {"if -K 6", 7}};
    odin3_abc_script_src no_text = {"t.abc", NULL, {NULL, 0}};
    odin3_abc_script_src no_name = {NULL, NULL, {"if -K 6", 7}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(NULL, 0, AIG, &script));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&src, 0, AIG, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&src, 1, AIG, &script));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&src, 17, AIG, &script));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&src, 6, 2, &script));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&no_text, 6, AIG, &script));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_script_parse(&no_name, 6, AIG, &script));
    TEST_ASSERT_EQUAL_STRING("odin3_abc_script_parse: invalid argument", g_last);
    TEST_ASSERT_NULL(script.arena);
}

/* --- files ---------------------------------------------------------------------------------- */

static void make_dir(void) {
    const char *base = getenv("TMPDIR");
    int wrote = snprintf(g_dir, sizeof g_dir, "%s/test_abc_script_XXXXXX",
                         base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof g_dir);
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
}

static void write_file(const char *path, odin3_bytes data) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(data.len, fwrite(data.ptr, 1, data.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void join(char *path, const char *name) {
    int wrote = snprintf(path, PATH_CAP, "%s/%s", g_dir, name);
    TEST_ASSERT_TRUE(wrote > 0 && wrote < PATH_CAP);
}

/* A file larger than the cap is read only past the cap; the size error names it as given. */
static void check_big_file(void) {
    char big[PATH_CAP];
    join(big, "big.abc");
    write_file(big, pad_to("if -K 6\n", ODIN3_ABC_MAX_SCRIPT_BYTES + 3000));
    odin3_abc_script_src big_src = {"big.abc", big, {NULL, 0}};
    odin3_abc_script out = {0};
    g_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_abc_script_parse(&big_src, 0, AIG, &out));
    TEST_ASSERT_EQUAL_STRING("abc: big.abc: abc script is larger than 65536 bytes", g_last);
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    TEST_ASSERT_EQUAL_INT(0, unlink(big));
}

static void test_files(void) {
    make_dir();
    char path[PATH_CAP];
    join(path, "s.abc");
    static const char text[] = "b -l\nif -K {K}\n";
    write_file(path, text_of(text));
    odin3_abc_script_src src = {"s.abc", path, {NULL, 0}};
    odin3_abc_script from_file;
    odin3_abc_script in_memory;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_script_parse(&src, 4, BLIF, &from_file));
    accept(text, (run){4, BLIF}, &in_memory);
    TEST_ASSERT_EQUAL_UINT32(2, from_file.n_cmds);
    TEST_ASSERT_EQUAL_STRING("if -K 4", from_file.cmds[1].line);
    TEST_ASSERT_EQUAL_STRING(in_memory.sha256_hex, from_file.sha256_hex);
    odin3_abc_script_free(&from_file);
    odin3_abc_script_free(&in_memory);
    check_big_file();
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(g_dir));
}

static void test_io_errors(void) {
    odin3_abc_script out = {0};
    odin3_abc_script_src missing = {"nope.abc", "/nonexistent-odin3/nope.abc", {NULL, 0}};
    g_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_abc_script_parse(&missing, 6, AIG, &out));
    TEST_ASSERT_EQUAL_STRING(
        "abc: /nonexistent-odin3/nope.abc: cannot read the script: No such file or directory",
        g_last);
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    make_dir();
    odin3_abc_script_src dir = {"d", g_dir, {NULL, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_abc_script_parse(&dir, 6, AIG, &out));
    char want[LINE_MAX_LOG];
    int wrote =
        snprintf(want, sizeof want, "abc: %s: cannot read the script: %s", g_dir, strerror(EISDIR));
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, g_last);
    TEST_ASSERT_NULL(out.arena);
    TEST_ASSERT_EQUAL_INT(0, rmdir(g_dir));
}

/* --- out of memory -------------------------------------------------------------------------- */

/* One parse with allocation failure fail: OK (true, *n_cmds set), or NO_MEMORY with nothing
 * logged and out untouched (ASan checks for leaks). */
static bool parse_failing(const odin3_abc_script_src *src, long fail, uint32_t *n_cmds) {
    odin3_abc_script out = {0};
    g_lines = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_abc_script_parse(src, 6, AIG, &out);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        *n_cmds = out.n_cmds;
        odin3_abc_script_free(&out);
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_EQUAL_size_t(0, g_lines);
    TEST_ASSERT_NULL(out.arena);
    return false;
}

/* Every allocation of a parse with aliases, nesting and the output fails once in turn. */
static void test_oom_sweep(void) {
    static const char text[] = "# OOM\nresyn2; &resyn2rs -v\nstrash; ifraig -v; scorr -v; dc2 "
                               "-v; dch -f; if -K {K} -v; mfs2 -v; print_stats\n";
    odin3_abc_script_src src = {"t.abc", NULL, text_of(text)};
    bool done = false;
    uint32_t n_cmds = 0;
    long fail = 0;
    for (; fail < OOM_LIMIT && !done; fail++) {
        done = parse_failing(&src, fail, &n_cmds);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(38, n_cmds);
    TEST_ASSERT_TRUE(fail >= 12); /* 16 allocations at the time of writing */
}

/* The file path's allocations fail too. */
static void test_oom_file(void) {
    make_dir();
    char path[PATH_CAP];
    join(path, "s.abc");
    write_file(path, text_of("if -K 6\n"));
    odin3_abc_script_src src = {"s.abc", path, {NULL, 0}};
    bool done = false;
    uint32_t n_cmds = 0;
    for (long fail = 0; fail < OOM_LIMIT && !done; fail++) {
        done = parse_failing(&src, fail, &n_cmds);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(1, n_cmds);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(g_dir));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_size_cap);
    RUN_TEST(test_nul_byte);
    RUN_TEST(test_control_byte);
    RUN_TEST(test_line_cap);
    RUN_TEST(test_crlf);
    RUN_TEST(test_byte_pass_first);
    RUN_TEST(test_quotes_and_shell);
    RUN_TEST(test_k_substitution);
    RUN_TEST(test_refuse_k_without_lut);
    RUN_TEST(test_unknown_substitution);
    RUN_TEST(test_word_cap);
    RUN_TEST(test_alias_one_word);
    RUN_TEST(test_alias_extra_words);
    RUN_TEST(test_alias_nested);
    RUN_TEST(test_alias_depth);
    RUN_TEST(test_alias_table_sweep);
    RUN_TEST(test_refuse_unknown_command);
    RUN_TEST(test_refuse_file_command);
    RUN_TEST(test_refuse_classes);
    RUN_TEST(test_every_row);
    RUN_TEST(test_arguments);
    RUN_TEST(test_file_and_crash_options);
    RUN_TEST(test_options_from_alias);
    RUN_TEST(test_count_cap);
    RUN_TEST(test_refuse_unmapped_end);
    RUN_TEST(test_blif_gia_empty);
    RUN_TEST(test_final_space);
    RUN_TEST(test_if_after_mapping);
    RUN_TEST(test_balancing_mappers);
    RUN_TEST(test_accepted_cases);
    RUN_TEST(test_sha256);
    RUN_TEST(test_accessors);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_files);
    RUN_TEST(test_io_errors);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_oom_file);
    return UNITY_END();
}
