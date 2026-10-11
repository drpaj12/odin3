/*
 * test_project_xml.c — unit tests for the subset XML reader behind the Odin II config reader
 * (PRJ-15): the tree (nested elements, attributes with either quote, text, CDATA, the five
 * entities and numeric references), the skipped prolog and comments, newline normalization,
 * every rejection with its exact message and line (a DTD, a processing instruction, unbalanced
 * tags, …), the node cap, a deep document (no recursion) and an OOM sweep.
 */
#include "frontends/project/xml.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { BIG_CAP = 1 << 20, DEEP = 100000, OOM_LIMIT = 2000, TEXT_MAX = 256 };

static odin3_xml_doc doc;
static odin3_xml_err err;

void setUp(void) {
    odin3_xml_doc_init(&doc, BIG_CAP);
    memset(&err, 0, sizeof err);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_xml_doc_free(&doc);
}

/* --- helpers ------------------------------------------------------------------------------- */

static odin3_status parse_len(const char *text, size_t len) {
    odin3_xml_doc_free(&doc);
    odin3_xml_doc_init(&doc, BIG_CAP);
    memset(&err, 0, sizeof err);
    return odin3_xml_parse((odin3_bytes){text, len}, &doc, &err);
}

static odin3_status parse(const char *text) {
    return parse_len(text, strlen(text));
}

static const odin3_xml_elem *elem(uint32_t index) {
    TEST_ASSERT_TRUE(index < doc.elems.len);
    return odin3_xml_elem_at(&doc, index);
}

/* Bytes as a C string (static buffer). */
static const char *cstr(odin3_bytes bytes) {
    static char buf[TEXT_MAX];
    (void)snprintf(buf, sizeof buf, "%.*s", (int)bytes.len, (const char *)bytes.ptr);
    return buf;
}

static const char *text_of(uint32_t index) {
    static char buf[TEXT_MAX];
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_xml_text_of(&doc, elem(index), &out));
    (void)snprintf(buf, sizeof buf, "%.*s", (int)out.len, out.len > 0 ? out.data : "");
    odin3_strbuf_free(&out);
    return buf;
}

static const char *attr(uint32_t index, const char *name) {
    odin3_bytes value = {NULL, 0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_xml_attr_get(&doc, elem(index), name, &value), name);
    return cstr(value);
}

static void expect_tag(uint32_t index, const char *tag, uint32_t line, uint32_t parent) {
    TEST_ASSERT_EQUAL_STRING(tag, cstr(elem(index)->tag));
    TEST_ASSERT_EQUAL_UINT32(line, elem(index)->line);
    TEST_ASSERT_EQUAL_UINT32(parent, elem(index)->parent);
}

/* --- the tree ------------------------------------------------------------------------------ */

static void test_tree(void) {
    const char *text = "<config>\n"
                       "  <a x=\"1\" y='two' z=\"it's\" w='say \"hi\"'>hello</a>\n"
                       "  <b>\n"
                       "    <c/><d k = \"v\" />\n"
                       "  </b>\n"
                       "</config>\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse(text));
    TEST_ASSERT_EQUAL_size_t(5, doc.elems.len);
    expect_tag(0, "config", 1, ODIN3_XML_NONE);
    expect_tag(1, "a", 2, 0);
    expect_tag(2, "b", 3, 0);
    expect_tag(3, "c", 4, 2);
    expect_tag(4, "d", 4, 2);
    TEST_ASSERT_EQUAL_UINT32(11, elem(1)->off);
    TEST_ASSERT_EQUAL_UINT32(1, elem(0)->first_child);
    TEST_ASSERT_EQUAL_UINT32(2, elem(1)->next_sibling);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_XML_NONE, elem(2)->next_sibling);
    TEST_ASSERT_EQUAL_UINT32(3, elem(2)->first_child);
    TEST_ASSERT_EQUAL_UINT32(4, elem(3)->next_sibling);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_XML_NONE, elem(3)->first_child);
    TEST_ASSERT_EQUAL_UINT32(4, elem(1)->nattrs);
    TEST_ASSERT_EQUAL_STRING("1", attr(1, "x"));
    TEST_ASSERT_EQUAL_STRING("two", attr(1, "y"));
    TEST_ASSERT_EQUAL_STRING("it's", attr(1, "z"));
    TEST_ASSERT_EQUAL_STRING("say \"hi\"", attr(1, "w"));
    TEST_ASSERT_EQUAL_STRING("v", attr(4, "k"));
    odin3_bytes none = {NULL, 0};
    TEST_ASSERT_FALSE(odin3_xml_attr_get(&doc, elem(1), "q", &none));
    TEST_ASSERT_EQUAL_STRING("hello", text_of(1));
    /* an element's own text only: the children's text is theirs */
    TEST_ASSERT_EQUAL_STRING("\n    \n  ", text_of(2));
    TEST_ASSERT_EQUAL_STRING("", text_of(3));
}

static void test_text_runs(void) {
    /* text split by a child, a comment and CDATA joins in document order */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<r>a<x>in</x>b<!-- c -->c<![CDATA[<d>&amp;]]>e</r>"));
    TEST_ASSERT_EQUAL_STRING("abc<d>&amp;e", text_of(0));
    TEST_ASSERT_EQUAL_STRING("in", text_of(1));
}

static void test_entities(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<r a=\"&lt;&gt;&amp;&apos;&quot;&#65;&#x42;\">"
                                          "&lt;&gt;&amp;&apos;&quot;|&#97;&#x62;</r>"));
    TEST_ASSERT_EQUAL_STRING("<>&'\"AB", attr(0, "a"));
    TEST_ASSERT_EQUAL_STRING("<>&'\"|ab", text_of(0));
    /* multi-byte references are written as UTF-8 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<r>&#xE9;&#8364;&#x10FFFF;&#9;</r>"));
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9\xE2\x82\xAC\xF4\x8F\xBF\xBF\t", text_of(0));
}

static void test_prolog_and_comments(void) {
    const char *text = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!-- before -->\n"
                       "<r><!-- inside\n spanning - lines --><s/></r>\n"
                       "<!-- after -->\n  \n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse(text));
    TEST_ASSERT_EQUAL_size_t(2, doc.elems.len);
    expect_tag(0, "r", 3, ODIN3_XML_NONE);
    expect_tag(1, "s", 4, 0);
    TEST_ASSERT_EQUAL_STRING("", text_of(0));
}

static void test_newlines(void) {
    /* CRLF and a lone CR become LF in text; TAB, LF, CR, CRLF one space in attribute values; a
     * character reference stays as it is; lines count LF only (the source manager's lines) */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<r a=\"x\r\ny\tz\rw\nv&#13;&#10;\">\r\n"
                                          "p\rq\r\n<![CDATA[c\r\nd\re]]>&#13;</r>"));
    TEST_ASSERT_EQUAL_STRING("x y z w v\r\n", attr(0, "a"));
    TEST_ASSERT_EQUAL_STRING("\np\nq\nc\nd\ne\r", text_of(0));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<r>\r\n<a/>\r<b/>\n<c/></r>"));
    TEST_ASSERT_EQUAL_UINT32(2, elem(1)->line);
    TEST_ASSERT_EQUAL_UINT32(2, elem(2)->line);
    TEST_ASSERT_EQUAL_UINT32(3, elem(3)->line);
}

static void test_names(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse("<ns:r-1.x _a=\"1\" b.c-d=\"2\" \xC3\xA9=\"3\">"
                                          "<\xC3\xA9t\xC3\xA9/></ns:r-1.x >"));
    expect_tag(0, "ns:r-1.x", 1, ODIN3_XML_NONE);
    TEST_ASSERT_EQUAL_STRING("1", attr(0, "_a"));
    TEST_ASSERT_EQUAL_STRING("2", attr(0, "b.c-d"));
    TEST_ASSERT_EQUAL_STRING("3", attr(0, "\xC3\xA9"));
    expect_tag(1, "\xC3\xA9t\xC3\xA9", 1, 0);
}

/* --- rejections ---------------------------------------------------------------------------- */

typedef struct bad_case {
    const char *text;
    uint32_t line, col; /* col: 1-based byte column of err.off */
    const char *what;
} bad_case;

static const bad_case k_bad[] = {
    /* empty, unclosed, unbalanced */
    {"", 1, 1, "no root element"},
    {"  \n<!-- c -->\n", 3, 1, "no root element"},
    {"<config>\n<a>\n", 3, 1, "unclosed element <a>"},
    {"<config>\n<a>\n</b>\n</config>", 3, 3, "mismatched tag </b>, expected </a>"},
    {"<c>\n</c\n>\n</c>", 4, 1, "end tag </c> outside the root element"},
    {"</c>", 1, 1, "end tag </c> outside the root element"},
    {"<c/>\n<d/>", 2, 1, "element after the root element"},
    {"x<c/>", 1, 1, "text before the root element"},
    {"<c/>\n y", 2, 2, "text after the root element"},
    {"&amp;<c/>", 1, 1, "text before the root element"},
    /* DTD, processing instructions, other markup */
    {"<!DOCTYPE c>\n<c/>", 1, 1, "DTD not supported"},
    {"<c>\n<!DOCTYPE x>\n</c>", 2, 1, "DTD not supported"},
    {"<c>\n<?pi x?>\n</c>", 2, 1, "processing instruction not supported"},
    {"<?pi?><c/>", 1, 1, "processing instruction not supported"},
    {"\n<?xml version=\"1.0\"?><c/>", 2, 1, "XML declaration not at the start"},
    {"<?xml version=\"1.0\"", 1, 1, "unclosed XML declaration"},
    {"<c><!ELEMENT c ANY></c>", 1, 4, "bad markup declaration"},
    /* comments and CDATA */
    {"\n<!-- x\n\n", 2, 1, "unclosed comment"},
    {"<c>\n<!-- a -- b --></c>", 2, 8, "'--' inside a comment"},
    {"<c>\n<![CDATA[x\n\n", 4, 1, "unclosed CDATA section"},
    {"<![CDATA[x]]><c/>", 1, 1, "CDATA outside the root element"},
    {"<c>\n ]]> </c>", 2, 2, "']]>' in character data"},
    /* tags and attributes */
    {"<1/>", 1, 2, "bad name"},
    {"<c>< a/></c>", 1, 5, "bad name"},
    {"<c\n/ >", 2, 2, "expected '>'"},
    {"<c>\n<a\n  b=\"1\"\n  c></c>", 4, 4, "expected '=' after attribute name"},
    {"<c a=\"1\"b=\"2\"/>", 1, 9, "missing space before attribute"},
    {"<c a=b/>", 1, 6, "attribute value must be quoted"},
    {"<c \"x\"/>", 1, 4, "bad name"},
    {"<c>\n<a b=\"<\"/></c>", 2, 7, "'<' in attribute value"},
    {"<c a=\"1\"\n a=\"2\"/>", 2, 2, "duplicate attribute a"},
    {"<c a=\"1\" b='2' a='3'/>", 1, 16, "duplicate attribute a"},
    {"<c\n a=\"1", 1, 1, "unclosed tag"},
    {"<c>\n</c", 2, 1, "unclosed tag"},
    {"<c>\n</c x>", 2, 5, "expected '>'"},
    /* references */
    {"<c>\n&foo;\n</c>", 2, 1, "undefined entity &foo;"},
    {"<c a=\"&nbsp;\"/>", 1, 7, "undefined entity &nbsp;"},
    {"<c>&amp</c>", 1, 4, "unterminated reference"},
    {"<c>& b;</c>", 1, 4, "unterminated reference"},
    {"<c>\n&#0;</c>", 2, 1, "bad character reference"},
    {"<c>&#xD800;</c>", 1, 4, "bad character reference"},
    {"<c>&#xFFFE;</c>", 1, 4, "bad character reference"},
    {"<c>&#x110000;</c>", 1, 4, "bad character reference"},
    {"<c>&#99999999999;</c>", 1, 4, "bad character reference"},
    {"<c>&#x;</c>", 1, 4, "bad character reference"},
    {"<c>&#1;</c>", 1, 4, "bad character reference"},
    {"<c>&#X41;</c>", 1, 4, "bad character reference"},
    /* characters XML forbids, found before the parse reaches them or not */
    {"<c>\n\xFF\n</c>", 2, 1, "invalid character"},
    {"<c>\x01</c>", 1, 4, "invalid character"},
    {"<c>\xEF\xBF\xBE</c>", 1, 4, "invalid character"},
    {"<c>\xED\xA0\x80</c>", 1, 4, "invalid character"},
    {"<c>\xC0\xAF</c>", 1, 4, "invalid character"},
    {"<c a=\"\x02\"/>", 1, 7, "invalid character"},
    {"<!-- \x7F\x1B -->", 1, 7, "invalid character"},
    {"<c/>\x01", 1, 5, "invalid character"},
    /* the first error in document order wins over a later bad character */
    {"<c>\n</d>\x01", 2, 3, "mismatched tag </d>, expected </c>"},
};

/* The 1-based column of off on its line. */
static uint32_t col_of(const char *text, uint32_t off) {
    uint32_t col = 1;
    while (off > 0 && text[off - 1] != '\n') {
        off--;
        col++;
    }
    return col;
}

static void test_rejections(void) {
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        const bad_case *bc = &k_bad[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, parse(bc->text), bc->text);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bc->what, err.what, bc->text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(bc->line, err.line, bc->text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(bc->col, col_of(bc->text, err.off), bc->text);
        TEST_ASSERT_FALSE(err.over_cap);
    }
}

static void test_nul_byte(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_len("<c>\0</c>", 8));
    TEST_ASSERT_EQUAL_STRING("invalid character", err.what);
    TEST_ASSERT_EQUAL_UINT32(3, err.off);
}

static void test_long_names_cut(void) {
    char text[TEXT_MAX];
    (void)snprintf(
        text, sizeof text, "<c>&%s;</c>",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse(text));
    TEST_ASSERT_EQUAL_STRING(
        "undefined entity &abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl...;",
        err.what);
}

/* --- caps and depth ------------------------------------------------------------------------ */

static void test_node_cap(void) {
    /* elements, attributes and text runs count; the cap is checked before the node is stored */
    const char *text = "<r a=\"1\">t<s/></r>";
    for (uint32_t cap = 0; cap < 4; cap++) {
        odin3_xml_doc_free(&doc);
        odin3_xml_doc_init(&doc, cap);
        memset(&err, 0, sizeof err);
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                              odin3_xml_parse((odin3_bytes){text, strlen(text)}, &doc, &err));
        TEST_ASSERT_TRUE(err.over_cap);
        char what[ODIN3_XML_WHAT_MAX];
        (void)snprintf(what, sizeof what, "more than %u elements, attributes and text runs",
                       (unsigned)cap);
        TEST_ASSERT_EQUAL_STRING(what, err.what);
        static const uint32_t offs[] = {0, 3, 9, 10};
        TEST_ASSERT_EQUAL_UINT32(offs[cap], err.off);
        TEST_ASSERT_EQUAL_size_t(cap, doc.elems.len + doc.attrs.len + doc.texts.len);
    }
    odin3_xml_doc_free(&doc);
    odin3_xml_doc_init(&doc, 4);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_xml_parse((odin3_bytes){text, strlen(text)}, &doc, &err));
}

static void test_deep_document(void) {
    /* nesting this deep would overflow a recursive parser's stack under ASan */
    static char text[DEEP * 7 + 1];
    size_t len = 0;
    for (int i = 0; i < DEEP; i++) {
        text[len++] = '<';
        text[len++] = 'e';
        text[len++] = '>';
    }
    for (int i = 0; i < DEEP; i++) {
        text[len++] = '<';
        text[len++] = '/';
        text[len++] = 'e';
        text[len++] = '>';
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_len(text, len));
    TEST_ASSERT_EQUAL_size_t(DEEP, doc.elems.len);
    TEST_ASSERT_EQUAL_UINT32(DEEP - 2, elem(DEEP - 1)->parent);
    /* and unclosed: the innermost element is named */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_len(text, len - 4));
    TEST_ASSERT_EQUAL_STRING("unclosed element <e>", err.what);
}

static void test_many_attributes(void) {
    /* duplicates are found by hash, not by a scan over the earlier attributes */
    enum { NATTR = 20000 };
    static char text[NATTR * 12 + 16];
    size_t len = (size_t)snprintf(text, sizeof text, "<r");
    for (int i = 0; i < NATTR; i++) {
        len += (size_t)snprintf(text + len, sizeof text - len, " a%d=\"\"", i);
    }
    len += (size_t)snprintf(text + len, sizeof text - len, "/>");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_len(text, len));
    TEST_ASSERT_EQUAL_UINT32(NATTR, elem(0)->nattrs);
    (void)snprintf(text + len - 2, sizeof text - len + 2, " a7=\"\"/>");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse(text));
    TEST_ASSERT_EQUAL_STRING("duplicate attribute a7", err.what);
}

/* --- out of memory ------------------------------------------------------------------------- */

static void test_out_of_memory(void) {
    const char *text = "<?xml version=\"1.0\"?>\n<config>\n  <a x=\"1&amp;\" y='2'>t&lt;</a>\r\n"
                       "  <b><![CDATA[c\r\n]]><c k=\"v\"/>tail</b>\n</config>\n";
    bool succeeded = false;
    long tries = 0;
    for (long k = 0; k < OOM_LIMIT && !succeeded; k++) {
        odin3_xml_doc_free(&doc);
        odin3_xml_doc_init(&doc, BIG_CAP);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_xml_parse((odin3_bytes){text, strlen(text)}, &doc, &err);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
        succeeded = st == ODIN3_OK;
        tries++;
    }
    TEST_ASSERT_TRUE(succeeded);
    TEST_ASSERT_TRUE(tries > 3);
    TEST_ASSERT_EQUAL_STRING("1&", attr(1, "x"));
    TEST_ASSERT_EQUAL_STRING("t<", text_of(1));
    /* odin3_xml_text_of */
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_xml_text_of(&doc, elem(2), &out));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_size_t(0, out.len);
    odin3_strbuf_free(&out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tree);
    RUN_TEST(test_text_runs);
    RUN_TEST(test_entities);
    RUN_TEST(test_prolog_and_comments);
    RUN_TEST(test_newlines);
    RUN_TEST(test_names);
    RUN_TEST(test_rejections);
    RUN_TEST(test_nul_byte);
    RUN_TEST(test_long_names_cut);
    RUN_TEST(test_node_cap);
    RUN_TEST(test_deep_document);
    RUN_TEST(test_many_attributes);
    RUN_TEST(test_out_of_memory);
    return UNITY_END();
}
