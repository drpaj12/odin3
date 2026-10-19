/*
 * arch_xml.c — helpers over the XML tree for the architecture importer.
 */
#include "frontends/arch/arch_read.h"
#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { ARCH_MSG_MAX = 480 };

const odin3_xml_elem *arch_elem(const arch_rd *rd, uint32_t index) {
    return index == ODIN3_XML_NONE ? NULL : odin3_xml_elem_at(rd->doc, index);
}

bool arch_tag_is(const odin3_xml_elem *elem, const char *tag) {
    size_t len = strlen(tag);
    return elem->tag.len == len && memcmp(elem->tag.ptr, tag, len) == 0;
}

odin3_status arch_fail(const arch_rd *rd, const odin3_xml_elem *at, const char *fmt, ...) {
    char msg[ARCH_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s", rd->file, at != NULL ? at->line : 0u, msg);
    return ODIN3_ERR_PARSE;
}

odin3_status arch_attr(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                       const char **out) {
    odin3_bytes value = {NULL, 0};
    *out = NULL;
    if (!odin3_xml_attr_get(rd->doc, elem, name, &value)) {
        return ODIN3_OK;
    }
    *out = odin3_arena_strndup(rd->arch->arena, value.ptr, value.len);
    return *out != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

odin3_status arch_attr_req(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                           const char **out) {
    odin3_status st = arch_attr(rd, elem, name, out);
    if (st == ODIN3_OK && *out == NULL) {
        return arch_fail(rd, elem, "<%.*s> needs a '%s' attribute", (int)elem->tag.len,
                         (const char *)elem->tag.ptr, name);
    }
    return st;
}

odin3_status arch_attr_u32(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                           uint32_t dflt, uint32_t *out) {
    const char *text = NULL;
    odin3_status st = arch_attr(rd, elem, name, &text);
    *out = dflt;
    if (st != ODIN3_OK || text == NULL) {
        return st;
    }
    uint64_t value = 0;
    size_t i = 0;
    for (; text[i] >= '0' && text[i] <= '9' && value <= UINT32_MAX; i++) {
        value = value * 10 + (uint64_t)(text[i] - '0');
    }
    if (i == 0 || text[i] != '\0' || value > UINT32_MAX) {
        return arch_fail(rd, elem, "attribute '%s' must be a non-negative integer, not '%s'", name,
                         text);
    }
    *out = (uint32_t)value;
    return ODIN3_OK;
}

odin3_status arch_attr_bool(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                            bool dflt, bool *out) {
    const char *text = NULL;
    odin3_status st = arch_attr(rd, elem, name, &text);
    *out = dflt;
    if (st != ODIN3_OK || text == NULL) {
        return st;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0) {
        *out = true;
    } else if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0) {
        *out = false;
    } else {
        return arch_fail(rd, elem, "attribute '%s' must be 1, 0, true or false, not '%s'", name,
                         text);
    }
    return ODIN3_OK;
}

const odin3_xml_elem *arch_first(const arch_rd *rd, const odin3_xml_elem *elem) {
    return arch_elem(rd, elem->first_child);
}

const odin3_xml_elem *arch_next(const arch_rd *rd, const odin3_xml_elem *elem) {
    return arch_elem(rd, elem->next_sibling);
}

const odin3_xml_elem *arch_child(const arch_rd *rd, const odin3_xml_elem *elem, const char *tag) {
    for (const odin3_xml_elem *c = arch_first(rd, elem); c != NULL; c = arch_next(rd, c)) {
        if (arch_tag_is(c, tag)) {
            return c;
        }
    }
    return NULL;
}

uint32_t arch_count(const arch_rd *rd, const odin3_xml_elem *elem, const char *tag) {
    uint32_t count = 0;
    for (const odin3_xml_elem *c = arch_first(rd, elem); c != NULL; c = arch_next(rd, c)) {
        count += arch_tag_is(c, tag) ? 1u : 0u;
    }
    return count;
}
