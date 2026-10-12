/*
 * test_project_o3proj.c — unit tests for the .o3proj reader (PRJ-10): every §5.1 row with its
 * record (all.o3proj), every key's arity, every error with its names and exact located message,
 * `param` scopes checked after the file, `import` of a Quartus project (top given twice, a
 * revision, a param whose top comes from the import), and an OOM sweep with the record dropped.
 * The expected values were checked against the oracle with an uncommitted scratch harness
 * (task-5 report); the durable oracle comparison is project_corpus (Task 10).
 */
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "frontends/project/project_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_O3PROJ_DATA
#error "ODIN3_O3PROJ_DATA must name tests/unit/data/project/o3proj"
#endif

enum { PATH_BUF = 1024, MSG_MAX = 4096, OUT_MAX = 8192, TEXT_MAX = 4096, OOM_LIMIT = 4000 };

static odin3_design *design;
static odin3_project *proj;
static char captured[MSG_MAX];
static int errors;
static char out[OUT_MAX];
static char tmp[PATH_BUF / 2];
static char tmp_file[PATH_BUF];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

static void fresh_design(void) {
    if (design != NULL) {
        odin3_design_destroy(design);
    }
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    proj = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_project(design, &proj));
    errors = 0;
    captured[0] = '\0';
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    design = NULL;
    fresh_design();
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_o3proj_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    (void)snprintf(tmp_file, sizeof tmp_file, "%s/p.o3proj", tmp);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    (void)unlink(tmp_file);
    TEST_ASSERT_EQUAL_INT(0, rmdir(tmp));
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_intern(proj, odin3_bytes_cstr(str), &got));
    return got;
}

/* One substitution: every occurrence of from becomes to. */
typedef struct subst {
    const char *from, *to;
} subst;

/* Copies text into buf (size bytes) with the substitution applied. */
static const char *replace_into(char *buf, size_t size, const char *text, subst sub) {
    size_t len = 0;
    size_t from_len = strlen(sub.from);
    while (*text != '\0' && len + strlen(sub.to) + 1 < size) {
        if (strncmp(text, sub.from, from_len) == 0) {
            len += (size_t)snprintf(buf + len, size - len, "%s", sub.to);
            text += from_len;
        } else {
            buf[len++] = *text++;
        }
    }
    buf[len] = '\0';
    return buf;
}

/* Reads the tree file rel (given as rel). */
static odin3_status read_tree(const char *rel) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_O3PROJ_DATA, rel);
    odin3_proj_path file = {id(rel), id(path), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_o3proj(proj, &file);
}

/* Writes text ("$D" = the data tree) to p.o3proj in the temporary directory and reads it. */
static odin3_status read_text(const char *text) {
    char body[TEXT_MAX];
    replace_into(body, sizeof body, text, (subst){"$D", ODIN3_O3PROJ_DATA});
    FILE *fp = fopen(tmp_file, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(body), fwrite(body, 1, strlen(body), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    odin3_proj_path file = {id("p.o3proj"), id(tmp_file), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_o3proj(proj, &file);
}

/* The record dumped after finish, data-tree paths relative ("src/a.v"). */
static const char *dump(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &text));
    replace_into(out, sizeof out, text.data, (subst){ODIN3_O3PROJ_DATA "/", ""});
    odin3_strbuf_free(&text);
    return out;
}

/* The dump's line for key, without the indent and the trailing comma. */
static const char *field(const char *key) {
    static char line[OUT_MAX];
    char head[PATH_BUF];
    (void)snprintf(head, sizeof head, "\n  \"%s\": ", key);
    const char *at = strstr(dump(), head);
    TEST_ASSERT_NOT_NULL(at);
    at += strlen(head);
    size_t len = strcspn(at, "\n");
    len -= len > 0 && at[len - 1] == ',';
    (void)snprintf(line, sizeof line, "%.*s", (int)len, at);
    return line;
}

/* The message of the one error, "$D" for the data tree and "$T" for the temporary directory. */
static const char *message(void) {
    TEST_ASSERT_EQUAL_INT(1, errors);
    static char data_rel[MSG_MAX];
    static char both_rel[MSG_MAX];
    replace_into(data_rel, sizeof data_rel, captured, (subst){ODIN3_O3PROJ_DATA, "$D"});
    return replace_into(both_rel, sizeof both_rel, data_rel, (subst){tmp, "$T"});
}

/* Reading text (on a fresh design) fails with message. */
static void expect_text(const char *text, const char *expected) {
    fresh_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, read_text(text), text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, message(), text);
}

typedef struct text_case {
    const char *text;
    const char *message;
} text_case;

static void expect_cases(const text_case *cases, size_t count) {
    for (size_t i = 0; i < count; i++) {
        expect_text(cases[i].text, cases[i].message);
    }
}

/* --- records ------------------------------------------------------------------------------- */

static void test_every_key(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("all.o3proj"));
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.sv\", \"language\": \"systemverilog\", \"library\": \"mylib\"}, "
        "{\"path\": \"src/c.vhd\", \"language\": \"vhdl\", \"library\": \"other\"}]",
        field("files"));
    TEST_ASSERT_EQUAL_STRING("[{\"path\": \"lib/l.v\", \"language\": \"verilog\"}]",
                             field("libfiles"));
    TEST_ASSERT_EQUAL_STRING("[\"lib\"]", field("libdirs"));
    TEST_ASSERT_EQUAL_STRING("[\".v\", \".sv\"]", field("libext"));
    TEST_ASSERT_EQUAL_STRING("[\"inc\"]", field("incdirs"));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"WIDTH\", \"value\": \"8\"}, {\"name\": \"FLAG\", "
                             "\"value\": null}, {\"name\": \"MSG\", \"value\": \"\\\"hi\\\"\"}, "
                             "{\"name\": \"EMPTY\", \"value\": \"\"}]",
                             field("defines"));
    TEST_ASSERT_EQUAL_STRING("\"mylib.top\"", field("top"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"scope\": \"top\", \"name\": \"W\", \"value\": \"16\", \"type\": \"number\"}, "
        "{\"scope\": \"top.u1\", \"name\": \"S\", \"value\": \"hello world\", \"type\": "
        "\"string\"}, "
        "{\"scope\": \"top\", \"name\": \"H\", \"value\": \"8'hFF\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"N\", \"value\": \"-3\", \"type\": \"number\"}, "
        "{\"scope\": \"top\", \"name\": \"Q\", \"value\": \"8'hFF\", \"type\": \"string\"}]",
        field("params"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"kind\": \"o3lib\", \"path\": \"t.o3lib\"}, {\"kind\": \"vpr_xml\", \"path\": "
        "\"k6.xml\"}, {\"kind\": \"device\", \"family\": \"Cyclone V\", \"device\": \"5CSEMA5\"}, "
        "{\"kind\": \"device\", \"family\": \"MAX10\", \"device\": null}]",
        field("arch"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", \"action\": \"to\", "
        "\"cells\": [\"multiply\"], \"min_width\": 9}, "
        "{\"rule\": \"map\", \"scope\": \"top.u1\", \"subject\": \"$mul\", \"action\": \"to\", "
        "\"cells\": [\"a\", \"b\", \"c\"], \"max_width\": 18, \"min_depth\": 2}, "
        "{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mem\", \"action\": \"soft\", "
        "\"cells\": []}, "
        "{\"rule\": \"map\", \"scope\": \"top\", \"subject\": \"$add\", \"action\": \"keep\", "
        "\"cells\": [], \"min_width\": 4}, "
        "{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": true, \"depth\": null}, "
        "{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": false, \"depth\": \"max\"}, "
        "{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": true, \"depth\": \"064\"}, "
        "{\"rule\": \"limit\", \"cell\": \"multiply\", \"count\": 40}, "
        "{\"rule\": \"patterns\", \"path\": \"pat.o3lib\"}]",
        field("rules"));
    TEST_ASSERT_EQUAL_STRING("[{\"cell\": \"multiply\", \"count\": 112}]", field("inventory"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"override\": \"hide\", \"cell\": \"dual_port_ram\"}, {\"override\": \"cell\", "
        "\"name\": \"mul27\", \"from\": \"multiply\", \"params\": {\"A\": \"27\", \"B\": \"27\", "
        "\"C\": \"\"}}, {\"override\": \"cell\", \"name\": \"plain\", \"from\": \"multiply\", "
        "\"params\": {}}]",
        field("overrides"));
    TEST_ASSERT_EQUAL_STRING("\"flow.o3\"", field("flow"));
    TEST_ASSERT_EQUAL_STRING("[]", field("ignored"));
    TEST_ASSERT_EQUAL_STRING("[\"src/a.v\"]", field("duplicates"));
}

/* "file:line:col" of loc, as given. */
static const char *loc_text(odin3_loc loc) {
    static char text[PATH_BUF];
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_format(sm, loc, &buf, ODIN3_SRCFMT_GIVEN));
    (void)snprintf(text, sizeof text, "%s", buf.data);
    odin3_strbuf_free(&buf);
    return text;
}

/* Locations: a path at its own word, everything else at the key. */
static void test_locations(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          read_text("\n  top t\n  file verilog \"$D/src/a.v\"\nmap * $mul soft\n"
                                    "available m 1\ndefine X\nparam t.W 1\n"));
    TEST_ASSERT_EQUAL_STRING("p.o3proj:2:3", loc_text(proj->top_at));
    const odin3_proj_file *file = odin3_vec_cat(&proj->files, 0);
    TEST_ASSERT_EQUAL_STRING("p.o3proj:3:16", loc_text(file->path.at));
    const odin3_proj_rule *rule = odin3_vec_cat(&proj->rules, 0);
    TEST_ASSERT_EQUAL_STRING("p.o3proj:4:1", loc_text(rule->at));
    const odin3_proj_inventory *inv = odin3_vec_cat(&proj->inventory, 0);
    TEST_ASSERT_EQUAL_STRING("p.o3proj:5:1", loc_text(inv->at));
    const odin3_proj_define *def = odin3_vec_cat(&proj->defines, 0);
    TEST_ASSERT_EQUAL_STRING("p.o3proj:6:1", loc_text(def->at));
    const odin3_proj_param *param = odin3_vec_cat(&proj->params, 0);
    TEST_ASSERT_EQUAL_STRING("p.o3proj:7:1", loc_text(param->at));
}

/* CRLF lines; comments; blank lines; a lone CR is part of a word. */
static void test_line_endings(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("# c\r\n\r\ntop t # c\r\ndefine A=\rB\r\n"));
    TEST_ASSERT_EQUAL_STRING("\"t\"", field("top"));
    TEST_ASSERT_EQUAL_STRING("[{\"name\": \"A\", \"value\": \"\\rB\"}]", field("defines"));
}

/* --- keys and arity ------------------------------------------------------------------------ */

#define USAGE(line, text) "p.o3proj:" line ":1: error: syntax: usage: " text

static void test_unknown_key(void) {
    static const text_case cases[] = {
        {"bogus x\n", "p.o3proj:1:1: error: unknown_key: unknown key: 'bogus'"},
        {"\n  File verilog a.v\n", "p.o3proj:2:3: error: unknown_key: unknown key: 'File'"},
        {"\"file\" verilog a.v\n", "p.o3proj:1:1: error: unknown_key: unknown key: 'file'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

static void test_arity(void) {
    static const text_case cases[] = {
        {"file verilog\n", USAGE("1", "file <language> <path> [library <name>]")},
        {"file verilog a.v library\n", USAGE("1", "file <language> <path> [library <name>]")},
        {"file verilog a.v lib x\n", USAGE("1", "file <language> <path> [library <name>]")},
        {"file verilog a.v library x y\n", USAGE("1", "file <language> <path> [library <name>]")},
        {"file VERILOG a.v\n", USAGE("1", "file <language> <path> [library <name>]")},
        {"libfile verilog\n", USAGE("1", "libfile <language> <path>")},
        {"libdir\n", USAGE("1", "libdir <dir>")},
        {"libdir a b\n", USAGE("1", "libdir <dir>")},
        {"libext\n", USAGE("1", "libext <ext>...")},
        {"incdir\n", USAGE("1", "incdir <dir>")},
        {"define A B\n", USAGE("1", "define <name>[=<value>]")},
        {"top a b\n", USAGE("1", "top [<library>.]<module>")},
        {"param top.W\n", USAGE("1", "param <top>[.<instance>...].<name> <value>")},
        {"param top.W 1 2\n", USAGE("1", "param <top>[.<instance>...].<name> <value>")},
        {"arch\n", USAGE("1", "arch <file.o3lib|file.xml>")},
        {"device\n", USAGE("1", "device <family> [<part>]")},
        {"device a b c\n", USAGE("1", "device <family> [<part>]")},
        {"available m\n", USAGE("1", "available <libcell> <count>")},
        {"hide a b\n", USAGE("1", "hide <libcell>")},
        {"cell x from\n", USAGE("1", "cell <name> from <libcell> [param <name>=<value>...]")},
        {"map * $mul\n", USAGE("1", "map <scope> <subject> to <cell>[, <cell>...] | soft | keep "
                                    "[<threshold> <n>]...")},
        {"split x\n", USAGE("1", "split <subject> [width] [depth min|max|<n>]")},
        {"split x width depth 1 2\n", USAGE("1", "split <subject> [width] [depth min|max|<n>]")},
        {"limit m\n", USAGE("1", "limit <libcell> <count>")},
        {"patterns\n", USAGE("1", "patterns <file.o3lib>")},
        {"flow\n", USAGE("1", "flow <script.o3>")},
        {"abc_script\n", USAGE("1", "abc_script <file>")},
        {"abc_script a.abc b.abc\n", USAGE("1", "abc_script <file>")},
        {"import\n", USAGE("1", "import <file.qpf|file.qsf> [revision <name>]")},
        {"import a.qsf revision\n", USAGE("1", "import <file.qpf|file.qsf> [revision <name>]")},
        {"import a.qsf rev r\n", USAGE("1", "import <file.qpf|file.qsf> [revision <name>]")},
        {"import a.qsf revision r x\n", USAGE("1", "import <file.qpf|file.qsf> [revision <name>]")},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* Arity comes before anything else: a missing file in a statement with too many words. */
static void test_arity_first(void) {
    expect_text("libfile vhdl93 nope.v x\n", USAGE("1", "libfile <language> <path>"));
    expect_text("flow nope.o3 x\n", USAGE("1", "flow <script.o3>"));
}

/* libext takes 1–99 extensions; 100 is a usage error. */
static void test_libext_ceiling(void) {
    char text[TEXT_MAX] = "libext";
    for (int i = 0; i < 99; i++) {
        strcat(text, " .v");
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text(strcat(text, "\n")));
    TEST_ASSERT_EQUAL_UINT32(99, (uint32_t)proj->libext.len);
    text[strlen(text) - 1] = '\0';
    expect_text(strcat(text, " .v\n"), USAGE("1", "libext <ext>..."));
    expect_text("libext .v .foo\n",
                "p.o3proj:1:1: error: unknown_file_type: no language for extension: '.foo'");
}

/* ODIN3_PROJ_MAX_WORDS caps the words of each line (here lowered to 3), counted before they
 * are stored: the 4th word is a located error; the cap is per line. */
static void test_word_cap(void) {
    odin3_proj_limits low = {ODIN3_PROJ_MAX_FLIST_DEPTH, ODIN3_PROJ_MAX_QIP_DEPTH, 3,
                             ODIN3_PROJ_MAX_XML_NODES};
    odin3_project_test_set_limits(proj, low);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text("libext .v .sv\nlibext .v .sv .vh\n"));
    TEST_ASSERT_EQUAL_STRING(
        "p.o3proj:2:15: error: syntax: line has more than 3 words (ODIN3_PROJ_MAX_WORDS)",
        message());
    fresh_design();
    odin3_project_test_set_limits(proj, low);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("libext .v .sv\nlibext .v .sv # .vh .x\n"));
}

/* The default cap is 2^20 words per line (§9), counted before anything is stored: the key and
 * 2^20 + 1 values fail at the line's word 2^20 + 1. */
static void test_default_word_cap(void) {
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 20, ODIN3_PROJ_MAX_WORDS);
    FILE *fp = fopen(tmp_file, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    int failed = fputs("libext", fp) < 0;
    for (uint32_t i = 0; i <= ODIN3_PROJ_MAX_WORDS; i++) {
        failed |= fputs(" x", fp) < 0;
    }
    failed |= fputs("\n", fp) < 0;
    failed |= fclose(fp) != 0;
    TEST_ASSERT_EQUAL_INT(0, failed);
    odin3_proj_path file = {id("p.o3proj"), id(tmp_file), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_read_o3proj(proj, &file));
    TEST_ASSERT_EQUAL_STRING("p.o3proj:1:2097158: error: syntax: line has more than 1048576 "
                             "words (ODIN3_PROJ_MAX_WORDS)",
                             message());
}

/* Word errors (§4.2) name nothing and are located at the word. */
static void test_word_errors(void) {
    static const text_case cases[] = {
        {"file verilog \"a.v\n", "p.o3proj:1:14: error: syntax: unterminated quote"},
        {"define A\"B\n", "p.o3proj:1:8: error: syntax: quote inside a word"},
        {"define \"\"\n", "p.o3proj:1:8: error: syntax: empty quoted word"},
        {"define \"A\"B\n", "p.o3proj:1:8: error: syntax: characters after a closing quote"},
        {"define \"A\\n\"\n",
         "p.o3proj:1:8: error: syntax: bad escape (only \\\" and \\\\ inside quotes)"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* --- files, directories, defines, top ------------------------------------------------------ */

static void test_file_errors(void) {
    static const text_case cases[] = {
        {"file verilog nope.v\n",
         "p.o3proj:1:14: error: missing_file: file not found: '$T/nope.v'"},
        {"file verilog $D/src\n", "p.o3proj:1:14: error: missing_file: file not found: '$D/src'"},
        {"libfile vhdl93 x.v\n", "p.o3proj:1:1: error: syntax: unknown language: 'vhdl93'"},
        {"libfile verilog nope.v\n",
         "p.o3proj:1:17: error: missing_file: file not found: '$T/nope.v'"},
        {"libdir nope\n", "p.o3proj:1:8: error: missing_file: directory not found: '$T/nope'"},
        {"incdir $D/src/a.v\n",
         "p.o3proj:1:8: error: missing_file: directory not found: '$D/src/a.v'"},
        {"define 1A=2\n", "p.o3proj:1:1: error: syntax: bad macro name: '1A=2'"},
        {"define =2\n", "p.o3proj:1:1: error: syntax: bad macro name: '=2'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

static void test_top_errors(void) {
    expect_text("top a-b\n", "p.o3proj:1:1: error: syntax: bad top name: 'a-b'");
    expect_text("top a.b.c\n", "p.o3proj:1:1: error: syntax: bad top name: 'a.b.c'");
    expect_text("top a\n\ntop b\n", "p.o3proj:3:1: error: syntax: top given twice (first a): 'b'");
}

/* --- param --------------------------------------------------------------------------------- */

/* An unquoted NUMBER is a number, a quoted value a string, anything else an error. */
static void test_param_values(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("top lib.t\nparam t.A 8'hFF\nparam t.u.B \"8'hFF\"\n"
                                              "param t.C 1_0\nparam t.D 'sb1x?\n"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"scope\": \"t\", \"name\": \"A\", \"value\": \"8'hFF\", \"type\": \"number\"}, "
        "{\"scope\": \"t.u\", \"name\": \"B\", \"value\": \"8'hFF\", \"type\": \"string\"}, "
        "{\"scope\": \"t\", \"name\": \"C\", \"value\": \"1_0\", \"type\": \"number\"}, "
        "{\"scope\": \"t\", \"name\": \"D\", \"value\": \"'sb1x?\", \"type\": \"number\"}]",
        field("params"));
    static const text_case cases[] = {
        {"top t\nparam t.W abc\n", "p.o3proj:2:1: error: syntax: parameter value: quote a string, "
                                   "or give a number: 'abc'"},
        {"top t\nparam t.W 8'h\n", "p.o3proj:2:1: error: syntax: parameter value: quote a string, "
                                   "or give a number: '8'h'"},
        {"top t\nparam t.W 1.5\n", "p.o3proj:2:1: error: syntax: parameter value: quote a string, "
                                   "or give a number: '1.5'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

#define PARAM_USAGE "syntax: usage: param <top>[.<instance>...].<name> <value>"

static void test_param_names(void) {
    static const text_case cases[] = {
        {"param W 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": 'W'"},
        {"param .W 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": '.W'"},
        {"param t..W 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": 't..W'"},
        {"param t.W. 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": 't.W.'"},
        {"param t.1W 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": 't.1W'"},
        {"param t-x.W 1\n", "p.o3proj:1:1: error: " PARAM_USAGE ": 't-x.W'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* The scope's first component is checked against the file's own top after the whole file. */
static void test_param_scope(void) {
    static const text_case cases[] = {
        {"top t\nparam other.W 1\n",
         "p.o3proj:2:1: error: syntax: param scope must start at the top (t): 'other'"},
        {"param t.u.W 1\n",
         "p.o3proj:1:1: error: syntax: param scope must start at the top (none declared): 't.u'"},
        {"top lib.t\nparam lib.W 1\n",
         "p.o3proj:2:1: error: syntax: param scope must start at the top (lib.t): 'lib'"},
        /* every `param` line is checked, also after an import that added parameters (the
         * oracle's check_param_scopes pairs lines with record entries positionally and would
         * accept this one: a declared divergence, §4.0 wins) */
        {"import $D/q/top.qsf\nparam x.W 1\n",
         "p.o3proj:2:1: error: syntax: param scope must start at the top (top): 'x'"},
        /* a later line's error comes first: the check runs after the file */
        {"param x.W 1\nbogus\n", "p.o3proj:2:1: error: unknown_key: unknown key: 'bogus'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    fresh_design();
    /* the top may come after the param line */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("param t.u.W 1\ntop t\n"));
}

/* --- architecture, inventory, overrides ---------------------------------------------------- */

static void test_arch_errors(void) {
    static const text_case cases[] = {
        {"arch t.v\n", "p.o3proj:1:1: error: unknown_file_type: not an architecture: 't.v'"},
        {"arch nope.XML\n", "p.o3proj:1:6: error: missing_file: file not found: '$T/nope.XML'"},
        {"available m x\n", "p.o3proj:1:1: error: syntax: expected a count: 'x'"},
        {"available m -1\n", "p.o3proj:1:1: error: syntax: expected a count: '-1'"},
        {"limit m 4294967296\n", "p.o3proj:1:1: error: syntax: count too large: '4294967296'"},
        {"patterns nope.o3lib\n",
         "p.o3proj:1:10: error: missing_file: file not found: '$T/nope.o3lib'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("limit m 4294967295\navailable m 0\n"));
}

#define CELL_USAGE "syntax: usage: cell <name> from <libcell> [param <name>=<value>...]"

static void test_cell_errors(void) {
    static const text_case cases[] = {
        {"cell x of y\n", "p.o3proj:1:1: error: " CELL_USAGE},
        {"cell x from y param\n", "p.o3proj:1:1: error: " CELL_USAGE},
        {"cell x from y params k=1\n", "p.o3proj:1:1: error: " CELL_USAGE},
        {"cell x from y param k\n", "p.o3proj:1:1: error: syntax: bad parameter: 'k'"},
        {"cell x from y param =1\n", "p.o3proj:1:1: error: syntax: bad parameter: '=1'"},
        {"cell x from y param k=1 k=2\n", "p.o3proj:1:1: error: syntax: bad parameter: 'k=2'"},
        {"cell x from y param k=1 j k=2\n", "p.o3proj:1:1: error: syntax: bad parameter: 'j'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("cell x from y param k=a=b K=1\n"));
    TEST_ASSERT_EQUAL_STRING("[{\"override\": \"cell\", \"name\": \"x\", \"from\": \"y\", "
                             "\"params\": {\"k\": \"a=b\", \"K\": \"1\"}}]",
                             field("overrides"));
}

/* --- rules --------------------------------------------------------------------------------- */

#define MAP_USAGE                                                                                  \
    "syntax: usage: map <scope> <subject> to <cell>[, <cell>...] | soft | keep [<threshold> "      \
    "<n>]..."
#define CELLS "syntax: expected <cell>[, <cell>...] after 'to'"

static void test_map_errors(void) {
    static const text_case cases[] = {
        {"map * $mul onto x\n", "p.o3proj:1:1: error: " MAP_USAGE ": 'onto'"},
        {"map * $mul to\n", "p.o3proj:1:1: error: " CELLS},
        {"map * $mul to min_width 3\n", "p.o3proj:1:1: error: " CELLS},
        {"map * $mul to a,,b\n", "p.o3proj:1:1: error: " CELLS},
        {"map * $mul to a, \n", "p.o3proj:1:1: error: " CELLS},
        {"map * $mul to a b\n", "p.o3proj:1:1: error: " CELLS},
        {"map * $mul to a min_width 2 b\n", "p.o3proj:1:1: error: " MAP_USAGE},
        {"map * $mul soft min_width\n", "p.o3proj:1:1: error: " MAP_USAGE},
        {"map * $mul soft width 3\n",
         "p.o3proj:1:1: error: syntax: bad or repeated threshold: 'width'"},
        {"map * $mul to m min_width 1 max_width 2 min_width 2\n",
         "p.o3proj:1:1: error: syntax: bad or repeated threshold: 'min_width'"},
        {"map * $mul keep min_depth x\n", "p.o3proj:1:1: error: syntax: expected a count: 'x'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* Cells are trimmed like Python's str.strip (a vertical tab, a no-break space). */
static void test_map_cells_stripped(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("map * $mul to a,\vb ,\xc2\xa0"
                                              "c\xc2\xa0\n"));
    TEST_ASSERT_EQUAL_STRING("[{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", "
                             "\"action\": \"to\", \"cells\": [\"a\", \"b\", \"c\"]}]",
                             field("rules"));
}

#define SPLIT_USAGE "syntax: usage: split <subject> [width] [depth min|max|<n>]"

static void test_split_errors(void) {
    static const text_case cases[] = {
        {"split x height\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x depth\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x depth mid\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x depth -1\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x width depth\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x depth 1 width\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
        {"split x width width\n", "p.o3proj:1:1: error: " SPLIT_USAGE},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    fresh_design();
    /* the digits stay as written, however large */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("split x depth 99999999999\n"));
    TEST_ASSERT_EQUAL_STRING("[{\"rule\": \"split\", \"subject\": \"x\", \"width\": false, "
                             "\"depth\": \"99999999999\"}]",
                             field("rules"));
}

/* --- flow, abc_script, import -------------------------------------------------------------- */

static void test_flow(void) {
    expect_text("flow $D/flow.o3\n\nflow $D/flow.o3\n",
                "p.o3proj:3:1: error: syntax: flow given twice: 'flow'");
    expect_text("flow nope.o3\n",
                "p.o3proj:1:6: error: missing_file: file not found: '$T/nope.o3'");
    /* given twice is checked before the path */
    expect_text("flow $D/flow.o3\nflow nope.o3\n",
                "p.o3proj:2:1: error: syntax: flow given twice: 'flow'");
}

/* abc_script (3D AB-27): the path as given and resolved against the file, at its word. */
static void test_abc_script(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("abc_script.o3proj"));
    TEST_ASSERT_EQUAL_INT(0, errors);
    const odin3_strtab *tab = odin3_design_strtab(design);
    TEST_ASSERT_EQUAL_STRING("abc/map.abc", odin3_strtab_get(tab, proj->abc_script.given));
    TEST_ASSERT_EQUAL_STRING(ODIN3_O3PROJ_DATA "/abc/map.abc",
                             odin3_strtab_get(tab, proj->abc_script.resolved));
    TEST_ASSERT_EQUAL_STRING("abc_script.o3proj:3:12", loc_text(proj->abc_script.at));
    TEST_ASSERT_EQUAL_STRING("\"abc/map.abc\"", field("abc_script"));
    expect_text("abc_script $D/abc/map.abc\nfile verilog $D/src/a.v\n\nabc_script $D/abc/map.abc\n",
                "p.o3proj:4:1: error: syntax: abc_script given twice: 'abc_script'");
    /* given twice is checked before the path */
    expect_text("abc_script $D/abc/map.abc\nabc_script abc/none.abc\n",
                "p.o3proj:2:1: error: syntax: abc_script given twice: 'abc_script'");
    expect_text("top t\n\nabc_script abc/none.abc\n",
                "p.o3proj:3:12: error: missing_file: file not found: '$T/abc/none.abc'");
    /* a directory is not a script */
    expect_text("abc_script $D/abc\n", "p.o3proj:1:12: error: missing_file: file not found: "
                                       "'$D/abc'");
}

static void test_import_records(void) {
    /* a param whose top comes from the import */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("import_param.o3proj"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"scope\": \"top\", \"name\": \"W\", \"value\": \"8\", \"type\": \"number\"}, "
        "{\"scope\": \"top.u1\", \"name\": \"W\", \"value\": \"4\", \"type\": \"number\"}]",
        field("params"));
    TEST_ASSERT_EQUAL_STRING("\"top\"", field("top"));
    TEST_ASSERT_EQUAL_STRING("[\"PIN_ASSIGN\"]", field("ignored"));
    fresh_design();
    /* import … revision r */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("import_rev.o3proj"));
    TEST_ASSERT_EQUAL_STRING("\"small\"", field("top"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"src/b.sv\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}]",
        field("files"));
}

/* Errors inside the imported files print the import line in their include chain. */
static void test_import_errors(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_tree("import_twice.o3proj"));
    TEST_ASSERT_EQUAL_STRING("q/top.qsf:1:1: error: syntax: top given twice (first other): 'top'\n"
                             "  included from import_twice.o3proj:2",
                             message());
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_tree("import_ambiguous.o3proj"));
    TEST_ASSERT_EQUAL_STRING("q/proj.qpf:1:1: error: ambiguous_revision: several revisions; "
                             "choose one with --revision: 'fast', 'small'\n"
                             "  included from import_ambiguous.o3proj:1",
                             message());
    fresh_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_tree("import_unknown.o3proj"));
    TEST_ASSERT_EQUAL_STRING(
        "import_unknown.o3proj:1:1: error: unknown_revision: no such revision: 'slow'", message());
    static const text_case cases[] = {
        {"import x.QSF\n",
         "p.o3proj:1:1: error: unknown_file_type: import takes a .qpf or .qsf: 'x.QSF'"},
        {"import $D/q/ip.qip\n",
         "p.o3proj:1:1: error: unknown_file_type: import takes a .qpf or .qsf: '$D/q/ip.qip'"},
        {"import nope.qsf\n", "p.o3proj:1:8: error: missing_file: file not found: '$T/nope.qsf'"},
        {"import $D/q/top.qsf revision other\n",
         "p.o3proj:1:1: error: unknown_revision: no such revision: 'other'"},
        /* the suffix of the normalized path */
        {"import $D/q/top.qsf/.\n", NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (cases[i].message != NULL) {
            expect_text(cases[i].text, cases[i].message);
            continue;
        }
        fresh_design();
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text(cases[i].text));
        TEST_ASSERT_EQUAL_STRING("\"top\"", field("top"));
    }
}

/* --- out of memory ------------------------------------------------------------------------- */

static bool oom_try(const char *rel, long fail_after) {
    fresh_design();
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_O3PROJ_DATA, rel);
    odin3_proj_path file = {id(rel), id(path), {0}};
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_read_o3proj(proj, &file);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        odin3_design_drop_project(design);
        TEST_ASSERT_NULL(odin3_design_project(design));
    }
    return st == ODIN3_OK;
}

/* Reads rel with the k-th allocation failing, k = 0, 1, … until it succeeds. */
static void sweep(const char *rel) {
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        succeeded = oom_try(rel, k);
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 20);
}

static void test_out_of_memory(void) {
    sweep("all.o3proj");
    sweep("import_param.o3proj");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_key);
    RUN_TEST(test_locations);
    RUN_TEST(test_line_endings);
    RUN_TEST(test_unknown_key);
    RUN_TEST(test_arity);
    RUN_TEST(test_arity_first);
    RUN_TEST(test_libext_ceiling);
    RUN_TEST(test_word_cap);
    RUN_TEST(test_default_word_cap);
    RUN_TEST(test_word_errors);
    RUN_TEST(test_file_errors);
    RUN_TEST(test_top_errors);
    RUN_TEST(test_param_values);
    RUN_TEST(test_param_names);
    RUN_TEST(test_param_scope);
    RUN_TEST(test_arch_errors);
    RUN_TEST(test_cell_errors);
    RUN_TEST(test_map_errors);
    RUN_TEST(test_map_cells_stripped);
    RUN_TEST(test_split_errors);
    RUN_TEST(test_flow);
    RUN_TEST(test_abc_script);
    RUN_TEST(test_import_records);
    RUN_TEST(test_import_errors);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
