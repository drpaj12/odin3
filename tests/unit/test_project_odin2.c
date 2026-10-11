/*
 * test_project_odin2.c — unit tests for the Odin II XML config reader (PRJ-15): every element
 * rule with its record (all.xml; checked against the oracle's parse_odin2), every `ignored` path
 * form, the output and input_type rules, the optimizations as rules, XML syntax errors at their
 * line, every error with its names and exact located message, values decoded and stripped as
 * expat and Python's str.strip() give them, the node cap and an OOM sweep with the record
 * dropped.
 */
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

#ifndef ODIN3_ODIN2_DATA
#error "ODIN3_ODIN2_DATA must name tests/unit/data/project/odin2"
#endif
#define D ODIN3_ODIN2_DATA

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
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_odin2_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    (void)snprintf(tmp_file, sizeof tmp_file, "%s/p.xml", tmp);
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

/* Copies text into buf (size bytes) with every from replaced by to. */
static const char *replace_into(char *buf, size_t size, const char *text, const char *from,
                                const char *to) {
    size_t len = 0;
    size_t from_len = strlen(from);
    while (*text != '\0' && len + strlen(to) + 1 < size) {
        if (strncmp(text, from, from_len) == 0) {
            len += (size_t)snprintf(buf + len, size - len, "%s", to);
            text += from_len;
        } else {
            buf[len++] = *text++;
        }
    }
    buf[len] = '\0';
    return buf;
}

/* Reads the data-tree file rel (given as rel). */
static odin3_status read_tree(const char *rel) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", D, rel);
    odin3_proj_path file = {id(rel), id(path), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_odin2(proj, &file);
}

/* Writes text ("$D" = the data tree) to p.xml in the temporary directory and reads it. */
static odin3_status read_text(const char *text) {
    char body[TEXT_MAX];
    replace_into(body, sizeof body, text, "$D", D);
    FILE *fp = fopen(tmp_file, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(body), fwrite(body, 1, strlen(body), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    odin3_proj_path file = {id("p.xml"), id(tmp_file), {0}};
    errors = 0;
    captured[0] = '\0';
    return odin3_read_odin2(proj, &file);
}

/* The record dumped after finish, data-tree paths relative ("src/a.v") and the temporary
 * directory as "$T". */
static const char *dump(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_finish(proj));
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_project_dump(proj, &text));
    char rel[OUT_MAX];
    replace_into(rel, sizeof rel, text.data, D "/", "");
    replace_into(out, sizeof out, rel, tmp, "$T");
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
    replace_into(data_rel, sizeof data_rel, captured, D, "$D");
    return replace_into(both_rel, sizeof both_rel, data_rel, tmp, "$T");
}

typedef struct text_case {
    const char *text;
    const char *message;
} text_case;

/* Reading each text (on a fresh design) fails with its message. */
static void expect_cases(const text_case *cases, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fresh_design();
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, read_text(cases[i].text), cases[i].text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].message, message(), cases[i].text);
    }
}

/* Reading text (on a fresh design) succeeds; the dump's line for key is expected. */
static void expect_field(const char *text, const char *key, const char *expected) {
    fresh_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_text(text), text);
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, field(key), text);
}

#define CONFIG(body) "<config>\n" body "</config>\n"

/* --- records ------------------------------------------------------------------------------- */

/* all.xml: every element rule; the expected lines are the oracle's parse_odin2 record. */
static void test_every_rule(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("all.xml"));
    TEST_ASSERT_EQUAL_INT(0, errors);
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"src/b.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
        "{\"path\": \"net/n.blif\", \"language\": \"blif\", \"library\": \"work\"}, "
        "{\"path\": \"src/c.v\", \"language\": \"verilog\", \"library\": \"work\"}]",
        field("files"));
    TEST_ASSERT_EQUAL_STRING("[]", field("libfiles"));
    TEST_ASSERT_EQUAL_STRING("[]", field("defines"));
    TEST_ASSERT_EQUAL_STRING("null", field("top"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"kind\": \"vpr_xml\", \"path\": \"arch/k6.xml\"}, {\"kind\": \"o3lib\", \"path\": "
        "\"arch/t.o3lib\"}]",
        field("arch"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", \"action\": \"to\", "
        "\"cells\": [\"multiply\"], \"min_width\": 9}, "
        "{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$add\", \"action\": \"to\", "
        "\"cells\": [\"adder\"], \"min_width\": 4}, "
        "{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": true, \"depth\": \"max\"}, "
        "{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": false, \"depth\": \"064\"}]",
        field("rules"));
    TEST_ASSERT_EQUAL_STRING("{\"path\": \"out/top.blif\", \"format\": \"blif\"}", field("output"));
    TEST_ASSERT_EQUAL_STRING(
        "[\"debug_outputs\", \"inputs/extra\", \"optimizations/adder@other\", "
        "\"optimizations/memory@extra\", \"optimizations/mix_soft_hard_blocks\", "
        "\"optimizations/multiply@fixed\", \"optimizations/multiply@fracture\", "
        "\"output/log_file\", \"output/target/device\", \"verilog_files/include_file\"]",
        field("ignored"));
    TEST_ASSERT_EQUAL_STRING("[\"src/a.v\"]", field("duplicates"));
    TEST_ASSERT_EQUAL_STRING("null", field("flow"));
}

/* Paths resolve against the XML file's directory; an empty output path is that directory and an
 * empty output_type is the format "" (present). */
static void test_relative_and_empty(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_tree("sub/up.xml"));
    TEST_ASSERT_EQUAL_STRING(
        "[{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}]",
        field("files"));
    TEST_ASSERT_EQUAL_STRING("{\"path\": \"sub\", \"format\": \"\"}", field("output"));
    TEST_ASSERT_EQUAL_STRING("[]", field("ignored"));
}

/* <output> is recorded only with a path (not required to exist, normalized); each <output> is
 * whole: a later one with a path replaces an earlier one, its format included. */
static void test_output(void) {
    expect_field(CONFIG("<output><output_type>BLIF</output_type></output>\n"), "output", "null");
    expect_field(CONFIG("<output><output_path_and_name>a//b/./c.blif</output_path_and_name>"
                        "</output>\n"),
                 "output", "{\"path\": \"$T/a/b/c.blif\", \"format\": null}");
    expect_field(CONFIG("<output><output_type></output_type>"
                        "<output_path_and_name>/x.blif</output_path_and_name></output>\n"),
                 "output", "{\"path\": \"/x.blif\", \"format\": \"\"}");
    expect_field(CONFIG("<output><output_type>EBLIF</output_type><output_type>Verilog"
                        "</output_type><output_path_and_name>x</output_path_and_name></output>\n"
                        "<output><output_path_and_name>y</output_path_and_name></output>\n"
                        "<output><output_type>blif</output_type></output>\n"),
                 "output", "{\"path\": \"$T/y\", \"format\": null}");
    expect_field(CONFIG("<output><output_type>EBLIF</output_type><output_type>Verilog"
                        "</output_type><output_path_and_name>x</output_path_and_name></output>\n"),
                 "output", "{\"path\": \"$T/x\", \"format\": \"verilog\"}");
    /* an arch_file counts without an output path; target children other than arch_file and
     * output children other than the three are listed */
    expect_field(CONFIG("<output><target><arch_file>$D/arch/k6.xml</arch_file><a/></target>"
                        "<b/></output>\n"),
                 "arch", "[{\"kind\": \"vpr_xml\", \"path\": \"arch/k6.xml\"}]");
    TEST_ASSERT_EQUAL_STRING("[\"output/b\", \"output/target/a\"]", field("ignored"));
    TEST_ASSERT_EQUAL_STRING("null", field("output"));
}

/* input_type: lower-cased, verilog by default, verilog or blif; one per <inputs>. */
static void test_inputs(void) {
    expect_field(CONFIG("<inputs><input_type> VeRiLoG </input_type>"
                        "<input_path_and_name>$D/net/n.blif</input_path_and_name></inputs>\n"),
                 "files",
                 "[{\"path\": \"net/n.blif\", \"language\": \"verilog\", \"library\": \"work\"}]");
    expect_field(CONFIG("<inputs><input_path_and_name>$D/src/a.v</input_path_and_name>"
                        "<input_type>blif</input_type></inputs>\n"),
                 "files",
                 "[{\"path\": \"src/a.v\", \"language\": \"blif\", \"library\": \"work\"}]");
    static const text_case cases[] = {
        {CONFIG("  <inputs>\n    <input_type>VHDL</input_type>\n  </inputs>\n"),
         "p.xml:3:5: error: unsupported_input_type: input_type must be verilog or blif: 'VHDL'"},
        {CONFIG("<inputs><input_type/></inputs>\n"),
         "p.xml:2:9: error: unsupported_input_type: input_type must be verilog or blif: ''"},
        /* arity before semantics: a second input_type, whatever the first */
        {CONFIG("<inputs><input_type>vhdl</input_type>\n<input_type>verilog</input_type>"
                "</inputs>\n"),
         "p.xml:3:1: error: syntax: more than one <input_type>"},
        /* the type is checked before any file of its <inputs> */
        {CONFIG("<inputs><input_path_and_name>nope.v</input_path_and_name>"
                "<input_type>edif</input_type></inputs>\n"),
         "p.xml:2:58: error: unsupported_input_type: input_type must be verilog or blif: "
         "'edif'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* <optimizations>: multiply@size and adder@threshold_size are map rules; memory is a split. */
static void test_optimizations(void) {
    expect_field(CONFIG("<optimizations><memory split_memory_depth=\"7\"/>"
                        "<memory split_memory_width=\"1\" split_memory_depth=\"min\"/>"
                        "<memory split_memory_width=\"1\"/><adder threshold_size=\"0\"/>"
                        "<multiply size=\"4294967295\"/></optimizations>\n"),
                 "rules",
                 "[{\"rule\": \"split\", \"subject\": \"$mem\", \"width\": false, \"depth\": "
                 "\"7\"}, {\"rule\": \"split\", \"subject\": \"$mem\", \"width\": true, "
                 "\"depth\": \"min\"}, {\"rule\": \"split\", \"subject\": \"$mem\", \"width\": "
                 "true, \"depth\": null}, {\"rule\": \"map\", \"scope\": \"*\", \"subject\": "
                 "\"$add\", \"action\": \"to\", \"cells\": [\"adder\"], \"min_width\": 0}, "
                 "{\"rule\": \"map\", \"scope\": \"*\", \"subject\": \"$mul\", \"action\": "
                 "\"to\", \"cells\": [\"multiply\"], \"min_width\": 4294967295}]");
    expect_field(CONFIG("<optimizations><adder size=\"3\">t<x/></adder>"
                        "<memory a=\"1\" b=\"2\"/></optimizations>\n"),
                 "ignored",
                 "[\"optimizations/adder@size\", \"optimizations/memory@a\", "
                 "\"optimizations/memory@b\"]");
    TEST_ASSERT_EQUAL_STRING("[]", field("rules"));
#define SPLIT_USAGE "syntax: split_memory_width 0|1, split_memory_depth min|max|<n>"
    static const text_case cases[] = {
        {CONFIG("<optimizations>\n  <multiply size=\"x\"/></optimizations>\n"),
         "p.xml:3:3: error: syntax: expected a count: 'x'"},
        {CONFIG("<optimizations><multiply size=\" 9\"/></optimizations>\n"),
         "p.xml:2:16: error: syntax: expected a count: ' 9'"},
        {CONFIG("<optimizations><adder threshold_size=\"4294967296\"/></optimizations>\n"),
         "p.xml:2:16: error: syntax: count too large: '4294967296'"},
        {CONFIG("<optimizations><adder threshold_size=\"\"/></optimizations>\n"),
         "p.xml:2:16: error: syntax: expected a count: ''"},
        {CONFIG("<optimizations><memory split_memory_width=\"2\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
        {CONFIG("<optimizations><memory split_memory_width=\"\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
        {CONFIG("<optimizations><memory split_memory_depth=\"x\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
        {CONFIG("<optimizations><memory split_memory_depth=\"-1\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
        {CONFIG("<optimizations><memory split_memory_depth=\"\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
        {CONFIG("<optimizations><memory split_memory_depth=\"MAX\"/></optimizations>\n"),
         "p.xml:2:16: error: " SPLIT_USAGE},
    };
#undef SPLIT_USAGE
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* Every other element is listed: root children by tag, verilog_files children with their
 * parent; nothing under them is read. */
static void test_ignored_forms(void) {
    expect_field(CONFIG("<verilog_files><verilog_files/><Verilog_File/></verilog_files>\n"
                        "<debug_outputs><output_ast_graphs>1</output_ast_graphs></debug_outputs>"
                        "\n<verilog_file>nope.v</verilog_file>\n<inputs><inputs/></inputs>\n"),
                 "ignored",
                 "[\"debug_outputs\", \"inputs/inputs\", \"verilog_file\", "
                 "\"verilog_files/Verilog_File\", \"verilog_files/verilog_files\"]");
}

/* Values: entities, numeric references and CDATA decoded, comments dropped, then stripped as
 * Python's str.strip() (non-ASCII spaces included). */
static void test_values(void) {
    expect_field(CONFIG("<verilog_files><verilog_file>&#xA0; $D/src/a&#46;v&#x2003;\n"
                        "</verilog_file><verilog_file>$D<!-- c -->/src/<![CDATA[b.v]]>"
                        "</verilog_file></verilog_files>\n"),
                 "files",
                 "[{\"path\": \"src/a.v\", \"language\": \"verilog\", \"library\": \"work\"}, "
                 "{\"path\": \"src/b.v\", \"language\": \"verilog\", \"library\": \"work\"}]");
    /* the CRLF file's lines are the source manager's */
    static const text_case cases[] = {
        {"<config>\r\n  <verilog_files>\r\n    <verilog_file>nope.v</verilog_file>\r\n",
         "p.xml:4:1: error: syntax: unclosed element <verilog_files>"},
        {"<config>\r\n  <verilog_files>\r\n    <verilog_file>nope.v</verilog_file>\r\n"
         "</verilog_files></config>",
         "p.xml:3:5: error: missing_file: file not found: '$T/nope.v'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* --- errors -------------------------------------------------------------------------------- */

static void test_root(void) {
    static const text_case cases[] = {
        {"<?xml version=\"1.0\"?>\n<!-- c -->\n  <project/>\n",
         "p.xml:3:3: error: syntax: root must be <config>: 'project'"},
        {"<Config/>", "p.xml:1:1: error: syntax: root must be <config>: 'Config'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    expect_field("<config/>", "files", "[]");
}

/* XML errors are `syntax` at the offending byte's line, before anything is read. */
static void test_xml_syntax(void) {
    static const text_case cases[] = {
        {"<config>\n    <verilog_files>\n        <verilog_file>src/top.v</verilog_file>\n"
         "    </verilog_file>\n</config>\n",
         "p.xml:4:7: error: syntax: mismatched tag </verilog_file>, expected </verilog_files>"},
        {"<!DOCTYPE config>\n<config/>\n", "p.xml:1:1: error: syntax: DTD not supported"},
        {"<config>\n<?odin x?>\n</config>\n",
         "p.xml:2:1: error: syntax: processing instruction not supported"},
        {"", "p.xml:1:1: error: syntax: no root element"},
        {"<config>\n<verilog_files><verilog_file>nope.v</verilog_file></verilog_files>\n"
         "<x a='1' a='2'/></config>",
         "p.xml:3:10: error: syntax: duplicate attribute a"},
        {"<config>&nbsp;</config>", "p.xml:1:9: error: syntax: undefined entity &nbsp;"},
        {"<config>\n\xC3\x28</config>", "p.xml:2:1: error: syntax: invalid character"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
}

/* Paths must exist (output excepted); arch files need an architecture suffix. Errors come in
 * document order. */
static void test_file_errors(void) {
    static const text_case cases[] = {
        {CONFIG("<verilog_files>\n    <verilog_file>nope.v</verilog_file>\n</verilog_files>\n"),
         "p.xml:3:5: error: missing_file: file not found: '$T/nope.v'"},
        {CONFIG("<verilog_files><verilog_file>$D/src</verilog_file></verilog_files>\n"),
         "p.xml:2:16: error: missing_file: file not found: '$D/src'"},
        {CONFIG("<verilog_files><verilog_file/></verilog_files>\n"),
         "p.xml:2:16: error: missing_file: file not found: '$T'"},
        {CONFIG("<inputs><input_path_and_name>a/../nope.blif</input_path_and_name></inputs>\n"),
         "p.xml:2:9: error: missing_file: file not found: '$T/nope.blif'"},
        {CONFIG("<output><target><arch_file>$D/arch/nope.xml</arch_file></target></output>\n"),
         "p.xml:2:17: error: missing_file: file not found: '$D/arch/nope.xml'"},
        {CONFIG("<output><target><arch_file>$D/src/a.v</arch_file></target></output>\n"),
         "p.xml:2:17: error: unknown_file_type: not an architecture: '$D/src/a.v'"},
        {CONFIG("<output><target><arch_file>$D/arch/k6.XML.txt</arch_file></target></output>\n"),
         "p.xml:2:17: error: unknown_file_type: not an architecture: '$D/arch/k6.XML.txt'"},
        /* document order: the earlier element's error wins */
        {CONFIG("<optimizations><multiply size=\"x\"/></optimizations>\n"
                "<verilog_files><verilog_file>nope.v</verilog_file></verilog_files>\n"),
         "p.xml:2:16: error: syntax: expected a count: 'x'"},
    };
    expect_cases(cases, sizeof cases / sizeof cases[0]);
    /* an existing arch file with an upper-case suffix is an architecture (suffix lower-cased) */
    expect_field(CONFIG("<output><target><arch_file>$D/arch/UP.XML</arch_file></target>"
                        "</output>\n"),
                 "arch", "[{\"kind\": \"vpr_xml\", \"path\": \"arch/UP.XML\"}]");
}

static void test_unreadable(void) {
    odin3_proj_path file = {id("gone.xml"), id(D "/gone.xml"), {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, odin3_read_odin2(proj, &file));
    TEST_ASSERT_EQUAL_INT(1, errors);
}

/* ODIN3_PROJ_MAX_XML_NODES caps elements, attributes and text runs (here lowered to 3), checked
 * before the node is stored. */
static void test_node_cap(void) {
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(1) << 20, ODIN3_PROJ_MAX_XML_NODES);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_PROJ_MAX_XML_NODES, proj->limits.xml_nodes);
    odin3_proj_limits low = {ODIN3_PROJ_MAX_FLIST_DEPTH, ODIN3_PROJ_MAX_QIP_DEPTH,
                             ODIN3_PROJ_MAX_WORDS, 3};
    odin3_project_test_set_limits(proj, low);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text("<config>\n<a x=\"1\"/>\n<b/>\n</config>"));
    TEST_ASSERT_EQUAL_STRING("p.xml:2:4: error: syntax: more than 3 elements, attributes and "
                             "text runs (ODIN3_PROJ_MAX_XML_NODES)",
                             message());
    fresh_design();
    odin3_project_test_set_limits(proj, low);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_text("<config><a x=\"1\"/></config>"));
}

/* --- out of memory ------------------------------------------------------------------------- */

static bool oom_try(const char *rel, long fail_after) {
    fresh_design();
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", D, rel);
    odin3_proj_path file = {id(rel), id(path), {0}};
    odin3_util_set_alloc_fail_after(fail_after);
    odin3_status st = odin3_read_odin2(proj, &file);
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
    sweep("all.xml");
    sweep("sub/up.xml");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_rule);
    RUN_TEST(test_relative_and_empty);
    RUN_TEST(test_output);
    RUN_TEST(test_inputs);
    RUN_TEST(test_optimizations);
    RUN_TEST(test_ignored_forms);
    RUN_TEST(test_values);
    RUN_TEST(test_root);
    RUN_TEST(test_xml_syntax);
    RUN_TEST(test_file_errors);
    RUN_TEST(test_unreadable);
    RUN_TEST(test_node_cap);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
