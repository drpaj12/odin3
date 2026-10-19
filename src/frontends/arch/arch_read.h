/*
 * arch_read.h — internals of the architecture importer (arch.c, arch_models.c, arch_pb.c,
 * arch_lib.c): the reading context and the small helpers over the XML tree.
 */
#ifndef ODIN3_FRONTENDS_ARCH_ARCH_READ_H
#define ODIN3_FRONTENDS_ARCH_ARCH_READ_H

#include "frontends/arch/arch.h"
#include "frontends/project/xml.h"
#include "util/attr.h"

#include <stdbool.h>
#include <stdint.h>

/* The most nodes (elements, attributes, text runs) the importer reads (spec X-2). */
enum { ODIN3_ARCH_MAX_XML_NODES = 1U << 24, ODIN3_ARCH_DECIMAL = 10 };

typedef struct arch_rd {
    odin3_arch *arch;
    const odin3_xml_doc *doc;
    const char *file; /* arena copy of the source name */
} arch_rd;

const odin3_xml_elem *arch_elem(const arch_rd *rd, uint32_t index);
bool arch_tag_is(const odin3_xml_elem *elem, const char *tag);

/* Logs "file:line: message" (error level) and returns ODIN3_ERR_PARSE. */
odin3_status arch_fail(const arch_rd *rd, const odin3_xml_elem *at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/* An arena copy of the attribute's value in *out, NULL when absent. NO_MEMORY. */
odin3_status arch_attr(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                       const char **out);

/* A required attribute: PARSE `<tag> needs a 'name'` when absent. */
odin3_status arch_attr_req(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                           const char **out);

/* A decimal attribute (dflt when absent). PARSE when it is not a non-negative integer. */
odin3_status arch_attr_u32(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                           uint32_t dflt, uint32_t *out);

/* A boolean attribute (1, 0, true, false; dflt when absent). */
odin3_status arch_attr_bool(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                            bool dflt, bool *out);

/* The first child of elem with tag, NULL when none. */
const odin3_xml_elem *arch_child(const arch_rd *rd, const odin3_xml_elem *elem, const char *tag);

/* The next sibling element of elem, NULL at the end. */
const odin3_xml_elem *arch_next(const arch_rd *rd, const odin3_xml_elem *elem);

/* The first child element of elem, NULL when none. */
const odin3_xml_elem *arch_first(const arch_rd *rd, const odin3_xml_elem *elem);

/* Children of elem with the given tag. */
uint32_t arch_count(const arch_rd *rd, const odin3_xml_elem *elem, const char *tag);

/* spec M-1..M-3: the `<models>` element (NULL: none). */
odin3_status arch_parse_models(arch_rd *rd, const odin3_xml_elem *models);

/* spec P-1..P-7: the `<complexblocklist>` element (NULL: none) and the `<directlist>` of the root.
 */
odin3_status arch_parse_pbs(arch_rd *rd, const odin3_xml_elem *blocks);
odin3_status arch_parse_directlist(arch_rd *rd, const odin3_xml_elem *list);

/* After the pb walk: the VPR port sizes of every model (spec P-4). */
void arch_finish_widths(arch_rd *rd);

#endif
