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

static bool g_gates;              /* abc --gates for the next parse (reset by setUp) */
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
    g_gates = false;
    g_lines = 0;
    g_last[0] = '\0';
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_aliases(NULL, 0);
    odin3_abc_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
}

/* --- helpers -------------------------------------------------------------------------------- */

/* How a script is checked: --lut K (0: none) and the exchange; g_gates (abc --gates) is set by the
 * gate-mapping tests and reset by setUp. */
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
    odin3_abc_script_mode mode = {how.lut_k, how.exchange, g_gates};
    return odin3_abc_script_parse_mode(&src, &mode, out);
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
    accept("if -K {K} -C {K}{K}", (run){12, AIG}, &script);
    TEST_ASSERT_EQUAL_STRING("if -K 12 -C 1212", script.cmds[0].line);
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
    if (strcmp(row->name, "map") == 0 || strcmp(row->name, "&nf") == 0) {
        return; /* gate mappers: test_map_needs_gates and test_nf_needs_gates */
    }
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
    accept("if -K 6 -C 8 -D 6 -E +6 -F 6.5 -W 6e3 -X 6.5E-3 -Z 0.25e+2 -vm", k_mem, &script);
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
    /* files: print_stats bd, rewrite x, resub PD, scorr PefI, if SJf, &if SJ, &ps D, mfs2 i, &nf
     * MZ; crashes: if scY, &if swYjbi, &dch mW, &put e, print_auto O, dc2 f, drw fCN, drf C, scorr
     * NFS, mfs tMp, mfs2 ZM, dch Ws, &synch2 W, resub F, &st M, &mfs M, lutpack o, print_latch s,
     * print_gates v, print_fanio m, &ps as, print_dsd pd, map AB, &nf fk (Task 10 fix rounds 1-3,
     * Task 11's audit, its fix round 1, the final review and its addendum) */
    TEST_ASSERT_EQUAL_UINT32(18, file_seen);
    TEST_ASSERT_EQUAL_UINT32(44, crash_seen); /* 34 + the addendum's six + map AB, &nf fk */
}

static void test_options_from_alias(void) {
    refuse("psb", k_mem,
           "t.abc:1: option '-b' of 'print_stats' (from alias 'psb') reads or writes a file or "
           "takes a string and is not allowed");
    refuse("strash; sif", k_mem,
           "t.abc:1: option '-s' of 'if' (from alias 'sif') crashes ABC at the pin (a3001b72) "
           "and is not allowed");
}

/* `if -K 6 -C <word>` is accepted. */
static void accept_number(const char *word) {
    char text[64];
    int wrote = snprintf(text, sizeof text, "if -K 6 -C %s", word);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    odin3_abc_script script;
    accept(text, k_mem, &script);
    odin3_abc_script_free(&script);
}

/* `if -K 6 -C <word>` is refused by the number cap. */
static void refuse_number(const char *word) {
    char text[64];
    char want[160];
    int wrote = snprintf(text, sizeof text, "if -K 6 -C %s", word);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    wrote = snprintf(want, sizeof want,
                     "t.abc:1: argument '%s' of 'if' is not a number from 0 to 1000000", word);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(text, k_mem, want);
}

/* Fix round 1, ruling C2: every number lies in 0 .. 1000000, both as atoi and as atof read it. */
static void test_number_cap(void) {
    static const char *const fine[] = {
        "0",         "1000000",    "+5",     "0.5",         "999999.999", "1e6",        "1E+6",
        "1000000.0", "0001000000", "1e-300", "0.000001e12", "100000e1",   "0e999999999"};
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        accept_number(fine[i]);
    }
    static const char *const out[] = {
        "1000001",      "1e7", "4294967296", "99999999999", "1000000.5", "5000000e-1", "1e300",
        "0.0000011e12", "-1",  "-0",         "-0.5",        "+1000001",  "1000000e01", "-1e5"};
    for (size_t i = 0; i < sizeof out / sizeof out[0]; i++) {
        refuse_number(out[i]);
    }
}

/* The cap holds for positional numbers and through aliases; echo prints any word. */
static void test_number_cap_forms(void) {
    refuse("print_level 4294967296", k_mem,
           "t.abc:1: argument '4294967296' of 'print_level' is not a number from 0 to 1000000");
    refuse("strash\nrs -K 6 -N 9999999", k_mem,
           "t.abc:2: argument '9999999' of 'resub' (from alias 'rs') is not a number from 0 to "
           "1000000");
    odin3_abc_script script;
    accept("echo 99999999999 -1; if -K 6", k_mem, &script);
    TEST_ASSERT_EQUAL_STRING("echo 99999999999 -1", script.cmds[0].line);
    odin3_abc_script_free(&script);
}

/* Fix round 1, rulings C1 and I1: print_npn and print_cone are refused, and print_kmap with two
 * words (its 6-variable functions). */
static void test_refused_print_commands(void) {
    refuse("print_npn 0", k_mem,
           "t.abc:1: abc command 'print_npn' is not allowed: it crashes ABC at the pin (a3001b72)");
    refuse("strash\npcon", k_mem,
           "t.abc:2: abc command 'print_cone' (from alias 'pcon') is not allowed: it crashes ABC "
           "at the pin (a3001b72)");
    refuse("print_kmap 1 2", k_mem,
           "t.abc:1: abc command 'print_kmap' with an argument is not allowed: it crashes ABC at "
           "the pin (a3001b72)");
    refuse("strash; pk -n 1 2 3", k_mem,
           "t.abc:1: abc command 'print_kmap' (from alias 'pk') with an argument is not allowed: "
           "it crashes ABC at the pin (a3001b72)");
    odin3_abc_script script;
    accept("print_kmap; print_kmap -n; print_level; print_io; if -K 6", k_mem, &script);
    odin3_abc_script_free(&script);
}

/* One of the addendum's crash-letter scripts: refused at line for cmd's -letter. */
typedef struct crash_case {
    const char *text, *letter, *cmd;
    uint32_t line;
} crash_case;

static void refuse_crash_case(const crash_case *cs) {
    char want[256];
    int wrote = snprintf(want, sizeof want,
                         "t.abc:%u: option '-%s' of '%s' crashes ABC at the pin (a3001b72) and is "
                         "not allowed",
                         (unsigned)cs->line, cs->letter, cs->cmd);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(cs->text, k_blif, want);
}

/* `<cmd> 8` after a mapping: a node-naming print with an argument is refused. */
static void refuse_named(const char *cmd) {
    char text[64];
    char want[256];
    int wrote = snprintf(text, sizeof text, "strash; if -K 6; %s 8", cmd);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
    wrote = snprintf(want, sizeof want,
                     "t.abc:1: abc command '%s' with an argument is not allowed: it crashes ABC at "
                     "the pin (a3001b72)",
                     cmd);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(text, k_blif, want);
}

/* The final review's addendum: its print-family crash scripts, replayed, are refused. */
static void test_addendum_print_crashers(void) {
    static const crash_case cases[] = {{"print_gates -v; if -K 6", "v", "print_gates", 1},
                                       {"strash; &get; &if -K 6\n&ps -a", "a", "&ps", 2},
                                       {"strash; if -K 6; &get -m\n&ps -a", "a", "&ps", 2},
                                       {"strash; if -K 6\nprint_fanio -m", "m", "print_fanio", 2},
                                       {"strash; &get; &if -K 6\n&ps -s", "s", "&ps", 2},
                                       {"strash; if -K 6\nprint_dsd -p", "p", "print_dsd", 2},
                                       {"print_dsd -d; if -K 6", "d", "print_dsd", 1}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        refuse_crash_case(&cases[i]);
    }
    static const char *const named[] = {"print_kmap", "print_level", "print_factor", "print_io",
                                        "print_delay"};
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        refuse_named(named[i]);
    }
    refuse("strash; if -K 6; print_factor -s 8", k_blif,
           "t.abc:1: abc command 'print_factor' with an argument is not allowed: it crashes ABC "
           "at the pin (a3001b72)");
}

/* Final review C1: linked ABC maps LUTs of at most 12 inputs; {K} and every -K stay at or below. */
static void test_k_linked_cap(void) {
    refuse("if -K {K}", (run){13, AIG},
           "t.abc:1: the script uses {K} = 13, but linked ABC maps LUTs of at most 12 inputs "
           "('&st' and '&synch2' crash on wider ones at the pin)");
    refuse("strash\n&if -K {K}", (run){16, AIG},
           "t.abc:2: the script uses {K} = 16, but linked ABC maps LUTs of at most 12 inputs "
           "('&st' and '&synch2' crash on wider ones at the pin)");
    odin3_abc_script script;
    accept("if -K {K}; echo {K}", (run){12, BLIF}, &script);
    odin3_abc_script_free(&script);
    accept("if -K 6", (run){16, AIG}, &script); /* --lut 16 without {K} maps to 6 */
    odin3_abc_script_free(&script);
}

/* The expected range refusal: the command, its option letter and the text after them. */
typedef struct range_msg {
    const char *cmd;
    char letter;
    const char *rule;
} range_msg;

/* text is refused by the range rule msg describes. */
static void refuse_range(const char *text, range_msg msg) {
    char want[384];
    int wrote = snprintf(want, sizeof want, "t.abc:1: option '-%c' of '%s' %s", msg.letter, msg.cmd,
                         msg.rule);
    TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
    refuse(text, k_mem, want);
}

static const char k_range_k[] =
    "must stand alone as '-K <n>' with <n> from 2 to 12: linked ABC maps "
    "LUTs of at most 12 inputs ('&st' and '&synch2' crash on wider "
    "ones at the pin)";
static const char k_range_e[] =
    "must stand alone as '-E <n>' with <n> from 1 to 1000000: a smaller "
    "value crashes ABC at the pin (a3001b72)";
static const char k_range_c_nf[] = "must stand alone as '-C <n>' with <n> from 2 to 1000000: a "
                                   "smaller value crashes ABC at the pin (a3001b72)";
static const char k_range_f_nf[] = "must stand alone as '-F <n>' with <n> from 1 to 1000000: a "
                                   "smaller value crashes ABC at the pin (a3001b72)";
static const char k_range_a_nf[] = "must stand alone as '-A <n>' with <n> from 1 to 1000000: a "
                                   "smaller value crashes ABC at the pin (a3001b72)";
static const char k_range_n[] =
    "must stand alone as '-N <n>' with <n> from 2 to 1000000: a smaller "
    "value crashes ABC at the pin (a3001b72)";

/* Final review C1, I1 and M2: -K 2..12, -E >= 1 and refactor -N >= 2, each alone as `-X <n>`
 * (a valued letter eats the next word, and atoi/atof of an option word is 0). */
static void test_value_ranges(void) {
    static const char *const bad_k[] = {"if -K 13", "if -K 1",    "if -K6",     "if -vK 6",
                                        "if -K",    "if -K -v 6", "if -K 13e-1"};
    for (size_t i = 0; i < sizeof bad_k / sizeof bad_k[0]; i++) {
        refuse_range(bad_k[i], (range_msg){"if", 'K', k_range_k});
    }
    refuse_range("&if -K 16", (range_msg){"&if", 'K', k_range_k});
    static const char *const bad_e[] = {"if -K 6 -E 0", "if -K 6 -E 0.5", "if -K 6 -E 1e-9",
                                        "if -E -K 6", "if -K 6 -vE 2"};
    for (size_t i = 0; i < sizeof bad_e / sizeof bad_e[0]; i++) {
        refuse_range(bad_e[i], (range_msg){"if", 'E', k_range_e});
    }
    refuse_range("&if -K 6 -E 0.0", (range_msg){"&if", 'E', k_range_e});
    refuse_range("refactor -N 1; if -K 6", (range_msg){"refactor", 'N', k_range_n});
    refuse("strash\nrf -N 0; if -K 6", k_mem,
           "t.abc:2: option '-N' of 'refactor' (from alias 'rf') must stand alone as '-N <n>' "
           "with <n> from 2 to 1000000: a smaller value crashes ABC at the pin (a3001b72)");
    static const char *const fine[] = {"if -K 2",
                                       "if -K 12",
                                       "if -K 12.9",
                                       "if -K 6 -E 1",
                                       "if -K 6 -E 1.5",
                                       "if -K 6 -E 0.1e1",
                                       "refactor -N 2; if -K 6",
                                       "&if -K 6 -E 1000000"};
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        odin3_abc_script script;
        accept(fine[i], k_mem, &script);
        odin3_abc_script_free(&script);
    }
}

/* Final review M4: the caps through odin3_abc_test_set_limits, at and past each lowered one. */
static void test_limits_lowered(void) {
    odin3_abc_limits lim = {64, ODIN3_ABC_MAX_LINE_BYTES, 3, 4, 2};
    odin3_abc_test_set_limits(&lim);
    odin3_abc_script script;
    accept_bytes(pad_to("if -K 6\n", 64), k_mem, &script);
    odin3_abc_script_free(&script);
    refuse_bytes(pad_to("if -K 6\n", 65), k_mem, "t.abc: abc script is larger than 64 bytes");
    accept("if -K 6 -v", k_mem, &script); /* 4 words */
    odin3_abc_script_free(&script);
    refuse("if -K 6 -v -v", k_mem, "t.abc:1: command has more than 4 words");
    accept("b; b; if -K 6", k_mem, &script);
    odin3_abc_script_free(&script);
    refuse("b; b; b; if -K 6", k_mem, "t.abc:1: script expands to more than 3 commands");
    static const odin3_abc_alias deep[] = {{"x0", "x1"}, {"x1", "x2"}, {"x2", "if -K 6"}};
    odin3_abc_test_set_aliases(deep, 3);
    refuse("x0", k_mem, "t.abc:1: alias 'x0' expands more than 2 levels deep");
    accept("x1", k_mem, &script);
    odin3_abc_script_free(&script);
    lim = (odin3_abc_limits){ODIN3_ABC_MAX_SCRIPT_BYTES, 16, 3, 4, 2};
    odin3_abc_test_set_limits(&lim);
    accept("if -K 6 -v      ", k_mem, &script); /* 16 bytes */
    odin3_abc_script_free(&script);
    refuse("if -K 6 -v       ", k_mem, "t.abc:1: line longer than 16 bytes");
    odin3_abc_limits high = {1U << 30, 1U << 30, 1U << 30, 1U << 30, 1U << 30};
    odin3_abc_test_set_limits(&high); /* clamped to the defaults */
    refuse_bytes(pad_to("if -K 6\n", ODIN3_ABC_MAX_SCRIPT_BYTES + 1), k_mem,
                 "t.abc: abc script is larger than 65536 bytes");
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

static const char k_unmapped[] =
    "t.abc: the script leaves the network unmapped; end it with a LUT "
    "mapper such as 'if -K {K}' (with --gates: a gate mapper, 'map' or "
    "'&nf')";
static const char k_not_gate_end[] = "t.abc: the script does not end gate-mapped; with --gates end "
                                     "it with a gate mapper ('map' or '&nf')";

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

/* Fix round 1, ruling I1: &if cannot map the choices a plain &get copied (Gia_ManChoiceLevel),
 * and print_xcut needs a network without choices (Abc_NtkCrossCut). */
static void test_choices(void) {
    static const char k_copied[] = "t.abc:2: '&if' needs a network in the & space without the "
                                   "choices that '&get' copied; add '&st'";
    static const char *const copied[] = {"strash; dch\n&get; &if -K 6",
                                         "strash; dch -f; &get -m\n&if -K 6",
                                         "strash; &get -n; &dch; &put; &get\n&if -K 6",
                                         "strash; &get -n; &synch2; &put; &get\n&if -K 6",
                                         "strash; dch; dch; &get -s; &put; &get\n&if -K 6"};
    for (size_t i = 0; i < sizeof copied / sizeof copied[0]; i++) {
        refuse(copied[i], k_blif, k_copied);
    }
    static const char k_xcut[] = "t.abc:2: 'print_xcut' needs a network without choices, which it "
                                 "has here; add 'strash'";
    refuse("strash; dch\nprint_xcut", k_blif, k_xcut);
    refuse("&dch; &put\nprint_xcut", k_mem, k_xcut);
    static const char *const fine[] = {"strash; dch; &get -s; &if -K 6",
                                       "strash; dch; &get; &st; &if -K 6",
                                       "strash; dch; strash; &get; &if -K 6",
                                       "strash; dch; balance; print_xcut; if -K 6",
                                       "strash; dch; if -K 6; print_xcut",
                                       "strash; &get -n; &dch; &if -K 6",
                                       "strash; &get -n; &synch2; &if -K 6",
                                       "strash; dch; &get; &dch; &if -K 6",
                                       "strash; dch; if -K 6"};
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        odin3_abc_script script;
        accept(fine[i], k_blif, &script);
        odin3_abc_script_free(&script);
    }
}

/* Review M1: the BLIF start is a logic network. &get -m keeps its mapping, so &if refuses it,
 * yet it is not a mapping the script made, so it does not end a script. */
static void test_blif_logic_start(void) {
    static const char k_get_m[] =
        "t.abc:1: '&get' with -m would keep the nodes of the network the BLIF exchange reads as "
        "LUTs of any width, but linked ABC maps LUTs of at most 12 inputs ('&st' and '&synch2' "
        "crash on wider ones at the pin); add 'strash' first";
    refuse("&get -m\n&if -K 6", k_blif, k_get_m);
    refuse("&get -m", k_blif, k_get_m);
    refuse("&get -m; &put; mfs2", k_blif, k_get_m);
    refuse("print_stats", k_blif, k_unmapped);
    expect_space("&get -m; &if -K 6", k_mem, GIA);
    expect_space("&get -m -c; &if -K 6", k_blif, GIA);
    expect_space("mfs2; if -K 6", k_blif, NTK);
    expect_space("if -K 6; &get -m; &st; &if -K 6", k_blif, GIA);
}

/* Final review C2: mfs and print_xcut need a network some command rebuilt, not the one the BLIF
 * exchange reads (dangling nodes); a strash, a mapper or any AIG-producing command rebuilds it. */
static void test_blif_start_rebuild(void) {
    static const char k_mfs[] = "t.abc:1: 'mfs' cannot run on the network the BLIF exchange "
                                "reads, which can hold dangling nodes; add 'strash' before it";
    refuse("mfs; if -K 6", k_blif, k_mfs);
    refuse("cleanup; sweep; mfs; if -K 6", k_blif, k_mfs);
    refuse("print_xcut; if -K 6", k_blif,
           "t.abc:1: 'print_xcut' cannot run on the network the BLIF exchange reads, which can "
           "hold dangling nodes; add 'strash' before it");
    static const char *const fine[] = {"strash; if -K 6; mfs", "strash; print_xcut; if -K 6",
                                       "if -K 6; mfs -v", "&get; &put; print_xcut; if -K 6"};
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        expect_space(fine[i], k_blif, NTK);
    }
    expect_space("mfs; if -K 6", k_mem, NTK);
}

/* Review M1's two surviving mutants: an ntk keep-command after an & mapping moves the current
 * space back to the unmapped ntk, and &mfs keeps the choices &get copied. */
static void test_step11_mutants(void) {
    refuse("&if -K 6; mfs2", k_mem, k_unmapped);
    refuse("&if -K 6; &put; strash; &get -n; &if -K 6; sweep", k_mem, k_unmapped);
    refuse("strash; dch; &get; &mfs\n&if -K 6", k_blif,
           "t.abc:2: '&if' needs a network in the & space without the choices that '&get' "
           "copied; add '&st'");
}

/* --- gate mapping (gatemap spec 5) ---------------------------------------------------------- */

static void test_map_needs_gates(void) {
    g_gates = false;
    refuse("strash\nmap\n", k_mem,
           "t.abc:2: 'map' maps to a gate library, which only abc --gates loads");
    refuse("strash\nmap -a\nif -K 6", k_mem,
           "t.abc:2: 'map' maps to a gate library, which only abc --gates loads");
}

static void test_nf_needs_gates(void) {
    g_gates = false;
    refuse("&st\n&nf\n", k_mem,
           "t.abc:2: '&nf' maps to a gate library, which only abc --gates loads");
}

static void test_gates_end_map(void) {
    g_gates = true;
    odin3_abc_script script;
    accept("strash\nmap\n", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT8(NTK, script.final_space);
    odin3_abc_script_free(&script);
}

static void test_gates_end_nf(void) {
    g_gates = true;
    odin3_abc_script script;
    accept("&st\n&nf\n", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT8(GIA, script.final_space);
    odin3_abc_script_free(&script);
}

static void test_gates_end_nf_put(void) {
    g_gates = true;
    odin3_abc_script script;
    accept("&st\n&nf\n&put\n", k_mem, &script);
    TEST_ASSERT_EQUAL_UINT8(NTK, script.final_space);
    odin3_abc_script_free(&script);
}

static void test_gates_refuses_lut_ending(void) {
    g_gates = true;
    refuse("strash\nif -K 4\n", k_mem, k_not_gate_end);
    refuse("&st\n&if -K 4\n", k_mem, k_not_gate_end);
    refuse("strash\nmap\nstrash\n", k_mem, k_not_gate_end);
}

static void test_map_needs_strashed(void) {
    g_gates = true;
    refuse("strash\nif -K 4\nmap\n", k_mem,
           "t.abc:3: 'map' needs a strashed network; add 'strash'");
    refuse("map\n", k_blif, "t.abc:1: 'map' needs a strashed network; add 'strash'");
}

static void test_nf_needs_unmapped_gia(void) {
    g_gates = true;
    const char *const want = "t.abc:3: '&nf' needs an unmapped network in the & space, which is "
                             "mapped here; add '&st'";
    refuse("&st\n&nf\n&nf\n", k_mem, want);
    refuse("&st\n&if -K 4\n&nf\n", k_mem,
           "t.abc:3: '&nf' needs an unmapped network in the & space, which is mapped here; "
           "add '&st'");
    refuse("&st\n&nf\n&if -K 4\n", k_mem,
           "t.abc:3: '&if' needs an unmapped network in the & space, which is mapped here; "
           "add '&st'");
}

static void test_mfs_refuses_cellmapped(void) {
    g_gates = true;
    refuse("strash\nmap\nmfs\n", k_mem,
           "t.abc:3: 'mfs' works on LUT networks, not on the gate-mapped network here; add "
           "'strash'");
    refuse("strash\nmap\nmfs2\n", k_mem,
           "t.abc:3: 'mfs2' works on LUT networks, not on the gate-mapped network here; add "
           "'strash'");
    refuse("strash\nmap\nlutpack\n", k_mem,
           "t.abc:3: 'lutpack' works on LUT networks, not on the gate-mapped network here; add "
           "'strash'");
    refuse("&st\n&nf\n&mfs\n", k_mem,
           "t.abc:3: '&mfs' works on LUT networks, not on the gate-mapped network here; add "
           "'&st'");
    /* &get drops a gate mapping, &put keeps it */
    odin3_abc_script script;
    accept("strash\nmap\n&get\n&nf\n", k_mem, &script);
    odin3_abc_script_free(&script);
    refuse("strash\nmap\n&get -m\n&nf\nmfs\n", k_mem,
           "t.abc:5: 'mfs' works on LUT networks, "
           "not on the gate-mapped network here; add 'strash'");
}

static void test_unmapped_end_message_names_both(void) {
    g_gates = false;
    refuse("strash\n", k_mem, k_unmapped);
}

static void test_nf_file_letters(void) {
    g_gates = true;
    const char *const file_z =
        "t.abc:2: option '-Z' of '&nf' reads or writes a file or takes a string "
        "and is not allowed";
    const char *const file_m =
        "t.abc:2: option '-M' of '&nf' reads or writes a file or takes a string "
        "and is not allowed";
    refuse("&st\n&nf -Z x\n", k_mem, file_z);
    refuse("&st\n&nf -M 3\n", k_mem, file_m);
}

static void test_gates_script_checks(void) {
    odin3_abc_script_src src = {
        ODIN3_ABC_GATES_SCRIPT_NAME, NULL, {odin3_abc_gates_script, odin3_abc_gates_script_len}};
    odin3_abc_script_mode mode = {0, AIG, true};
    odin3_abc_script script;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_script_parse_mode(&src, &mode, &script));
    TEST_ASSERT_EQUAL_UINT32(7, script.n_cmds);
    TEST_ASSERT_EQUAL_STRING("map -a", script.cmds[5].line);
    TEST_ASSERT_EQUAL_UINT8(NTK, script.final_space);
    odin3_abc_script_free(&script);
}

static void test_gates_script_needs_gates(void) {
    odin3_abc_script_src src = {
        ODIN3_ABC_GATES_SCRIPT_NAME, NULL, {odin3_abc_gates_script, odin3_abc_gates_script_len}};
    odin3_abc_script script;
    g_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_abc_script_parse(&src, 0, AIG, &script));
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    TEST_ASSERT_EQUAL_STRING("abc: builtin:lib/abc/gates.abc:8: 'map' maps to a gate library, "
                             "which only abc --gates loads",
                             g_last);
}

/* The Task 6 audit's crash letters of map (A, B) and &nf (f, k), and the &nf value ranges. */
static void test_map_nf_crash_letters(void) {
    g_gates = true;
    static const char *const letters[] = {"map", "A", "map", "B", "&nf", "f", "&nf", "k"};
    for (size_t i = 0; i < sizeof letters / sizeof letters[0]; i += 2) {
        char text[64];
        char want[256];
        int wrote = snprintf(text, sizeof text, "strash\n%s -%s 6", letters[i], letters[i + 1]);
        TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof text);
        wrote = snprintf(want, sizeof want,
                         "t.abc:2: option '-%s' of '%s' crashes ABC at the pin (a3001b72) and is "
                         "not allowed",
                         letters[i + 1], letters[i]);
        TEST_ASSERT_TRUE(wrote > 0 && (size_t)wrote < sizeof want);
        refuse(text, k_mem, want);
    }
    refuse_range("&nf -C 1", (range_msg){"&nf", 'C', k_range_c_nf});
    refuse_range("&nf -F 0", (range_msg){"&nf", 'F', k_range_f_nf});
    refuse_range("&nf -A 0", (range_msg){"&nf", 'A', k_range_a_nf});
    odin3_abc_script script;
    accept("&st\n&nf -C 2 -F 1 -A 1 -a -p -q -K 4 -R 5 -L 2 -E 50 -D 3 -Q 2 -W 3 -vw\n", k_mem,
           &script);
    odin3_abc_script_free(&script);
    accept("strash\nmap -D 2.5 -F 1 -S 2 -G 3 -M 1 -a -r -s -p -f -u -o -v\n", k_mem, &script);
    odin3_abc_script_free(&script);
}

/* One test entry for the gate-mapping checks (each sets g_gates itself). */
static void test_gate_mapping_checks(void) {
    test_map_needs_gates();
    test_nf_needs_gates();
    test_gates_end_map();
    test_gates_end_nf();
    test_gates_end_nf_put();
    test_gates_refuses_lut_ending();
    test_map_needs_strashed();
    test_nf_needs_unmapped_gia();
    test_mfs_refuses_cellmapped();
    test_unmapped_end_message_names_both();
    test_nf_file_letters();
    test_gates_script_checks();
    test_gates_script_needs_gates();
    test_map_nf_crash_letters();
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
    TEST_ASSERT_EQUAL_STRING("abc: nope.abc: cannot read the script: No such file or directory",
                             g_last);
    TEST_ASSERT_EQUAL_size_t(1, g_lines);
    make_dir();
    odin3_abc_script_src dir = {"d", g_dir, {NULL, 0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_abc_script_parse(&dir, 6, AIG, &out));
    char want[LINE_MAX_LOG];
    int wrote = snprintf(want, sizeof want, "abc: d: cannot read the script: %s", strerror(EISDIR));
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
    RUN_TEST(test_number_cap);
    RUN_TEST(test_number_cap_forms);
    RUN_TEST(test_refused_print_commands);
    RUN_TEST(test_addendum_print_crashers);
    RUN_TEST(test_k_linked_cap);
    RUN_TEST(test_value_ranges);
    RUN_TEST(test_limits_lowered);
    RUN_TEST(test_count_cap);
    RUN_TEST(test_refuse_unmapped_end);
    RUN_TEST(test_blif_gia_empty);
    RUN_TEST(test_final_space);
    RUN_TEST(test_if_after_mapping);
    RUN_TEST(test_balancing_mappers);
    RUN_TEST(test_choices);
    RUN_TEST(test_blif_logic_start);
    RUN_TEST(test_blif_start_rebuild);
    RUN_TEST(test_step11_mutants);
    RUN_TEST(test_gate_mapping_checks);
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
