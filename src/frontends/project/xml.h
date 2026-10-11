/*
 * xml.h — the subset XML reader behind the Odin II config reader (PRJ-15): one pass over the
 * bytes with an explicit stack of open elements (no recursion), building a flat element tree.
 *
 * Accepted: a UTF-8 BOM, an XML declaration at the very start (skipped, its encoding ignored:
 * the text is UTF-8), comments anywhere, one root element, elements with attributes (either
 * quote), character data, CDATA sections, the five predefined entities (&lt; &gt; &amp; &apos;
 * &quot;) and numeric references (&#N; &#xH;). Newlines in character data are normalized as XML
 * says (CRLF and a lone CR become LF), and in attribute values TAB, LF, CR (and CRLF) become one
 * space. Everything else is a syntax error at the offending byte: a DTD, a processing
 * instruction, a mismatched or unclosed tag, text outside the root, an undefined entity, a
 * character XML forbids (C0 controls but TAB/LF/CR, invalid UTF-8, U+FFFE, U+FFFF).
 */
#ifndef ODIN3_FRONTENDS_PROJECT_XML_H
#define ODIN3_FRONTENDS_PROJECT_XML_H

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* No element (a missing parent, child or sibling) and no text run. */
#define ODIN3_XML_NONE UINT32_MAX

/* What an error's message may hold (it names at most one tag). */
enum { ODIN3_XML_WHAT_MAX = 160 };

typedef struct odin3_xml_attr {
    odin3_bytes name;  /* a view of the input */
    odin3_bytes value; /* decoded: a view of the input or of the document's arena */
    uint32_t off;      /* the name's first byte */
} odin3_xml_attr;

typedef struct odin3_xml_elem {
    odin3_bytes tag;                            /* a view of the input */
    uint32_t off;                               /* the '<' of the start tag */
    uint32_t line;                              /* its line, 1-based (lines end at LF) */
    uint32_t parent, first_child, next_sibling; /* element indices or ODIN3_XML_NONE */
    uint32_t attrs, nattrs;                     /* span in doc->attrs */
    uint32_t first_text;                        /* index in doc->texts or ODIN3_XML_NONE */
    uint32_t last_child, last_text;             /* internal: appending */
} odin3_xml_elem;

/* One run of an element's own character data (CDATA included), in document order. */
typedef struct odin3_xml_text {
    odin3_bytes data; /* decoded: a view of the input or of the document's arena */
    uint32_t next;    /* the element's next run or ODIN3_XML_NONE */
} odin3_xml_text;

/* A parsed document: element 0 is the root. Views point into the parsed text, which must
 * outlive the document. */
typedef struct odin3_xml_doc {
    odin3_vec elems; /* odin3_xml_elem */
    odin3_vec attrs; /* odin3_xml_attr */
    odin3_vec texts; /* odin3_xml_text */
    odin3_arena *arena;
    uint32_t max_nodes; /* elements + attributes + text runs */
} odin3_xml_doc;

typedef struct odin3_xml_err {
    uint32_t off;  /* the offending byte (the end of the text for an early end) */
    uint32_t line; /* its line, 1-based */
    bool over_cap; /* the document would hold more than max_nodes nodes */
    char what[ODIN3_XML_WHAT_MAX];
} odin3_xml_err;

/* An empty document that will hold at most max_nodes elements, attributes and text runs. */
void odin3_xml_doc_init(odin3_xml_doc *doc, uint32_t max_nodes);

/* Frees everything the document holds; it may be parsed into again after an init. */
void odin3_xml_doc_free(odin3_xml_doc *doc);

/*
 * Parses text (at most UINT32_MAX - 1 bytes) into doc, which must be freshly initialized.
 * PARSE with *err filled (nothing logged; the caller locates it), including the node cap
 * (err->over_cap, checked before the node is stored); NO_MEMORY. On failure doc holds a partial
 * tree; free it either way.
 */
odin3_status odin3_xml_parse(odin3_bytes text, odin3_xml_doc *doc, odin3_xml_err *err);

/* The element at index (index < doc->elems.len). */
const odin3_xml_elem *odin3_xml_elem_at(const odin3_xml_doc *doc, uint32_t index);

/* The value of the attribute name of elem; false when it has none. */
bool odin3_xml_attr_get(const odin3_xml_doc *doc, const odin3_xml_elem *elem, const char *name,
                        odin3_bytes *value);

/* Replaces out's contents with elem's own character data, its runs joined (children's text
 * excluded, as expat's character-data handler gives it). NO_MEMORY (out left empty). */
odin3_status odin3_xml_text_of(const odin3_xml_doc *doc, const odin3_xml_elem *elem,
                               odin3_strbuf *out);

#endif
