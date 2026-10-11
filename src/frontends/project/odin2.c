/*
 * odin2.c — the Odin II XML config reader (PRJ-15, §5.4; the oracle's parse_odin2): the file is
 * parsed whole by the subset XML reader (xml.h), then the root's children are read in document
 * order. An element's value is its own text, joined and stripped as Python's str.strip(); an
 * element's errors are located at its '<'.
 */
#include "frontends/project/project.h"

#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/xml.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DETAIL_MAX = 256, BOM_LEN = 3 };

typedef struct o2_reader {
    odin3_project *proj;
    odin3_srcman *sm;
    odin3_proj_text text;
    odin3_xml_doc doc;
    odin3_bytes base;     /* the file's directory (a view of a strtab string) */
    uint32_t work;        /* "work" */
    odin3_strbuf value;   /* the element value being read */
    odin3_strbuf scratch; /* an ignored name, a lower-cased value, an output path */
    uint32_t line1_end;   /* the offset of the first '\n' (the length when there is none) */
} o2_reader;

/* A <optimizations> element that is a map rule (Odin II's ODIN2_RULES row). */
typedef struct o2_map {
    const char *attr;    /* the threshold attribute */
    const char *subject; /* "$mul" | "$add" */
    const char *cell;    /* the one target cell */
} o2_map;

static const o2_map k_multiply = {"size", "$mul", "multiply"};
static const o2_map k_adder = {"threshold_size", "$add", "adder"};

/* --- helpers ------------------------------------------------------------------------------- */

static const odin3_xml_elem *elem_at(const o2_reader *rd, uint32_t index) {
    return odin3_xml_elem_at(&rd->doc, index);
}

static bool tag_is(const odin3_xml_elem *elem, const char *tag) {
    return odin3_proj_bytes_is(elem->tag, tag);
}

/* The loc of byte off. A UTF-8 BOM is not a column: a line-1 offset after it is located
 * BOM_LEN bytes earlier, so its column counts from the first byte after the BOM. */
static odin3_loc loc_of(const o2_reader *rd, uint32_t off) {
    bool shift = off >= BOM_LEN && off <= rd->line1_end && rd->text.len >= BOM_LEN &&
                 memcmp(rd->text.bytes, "\xEF\xBB\xBF", BOM_LEN) == 0;
    return odin3_srcman_loc(rd->sm, rd->text.buffer, shift ? off - BOM_LEN : off);
}

/* An error at at naming name (none when name is NULL). */
static odin3_status fail(o2_reader *rd, odin3_loc at, odin3_proj_err_kind kind, const char *detail,
                         const odin3_bytes *name) {
    odin3_proj_err err = {kind, ODIN3_PROJ_LOCATED, at, NULL, detail, name, name != NULL};
    return odin3_project_fail(rd->proj, &err);
}

static odin3_status intern(o2_reader *rd, odin3_bytes text, uint32_t *str) {
    return odin3_project_intern(rd->proj, text, str);
}

/* The element's value: its own text joined, then stripped (a view of rd->value, valid until the
 * next call). */
static odin3_status value_of(o2_reader *rd, const odin3_xml_elem *elem, odin3_bytes *out) {
    odin3_status res = odin3_xml_text_of(&rd->doc, elem, &rd->value);
    *out =
        odin3_proj_py_strip((odin3_bytes){rd->value.len > 0 ? rd->value.data : "", rd->value.len});
    return res;
}

/* text lower-cased (ASCII letters; other bytes kept), a view of rd->scratch. */
static odin3_status lower(o2_reader *rd, odin3_bytes text, odin3_bytes *out) {
    odin3_strbuf_clear(&rd->scratch);
    odin3_status res = odin3_strbuf_append(&rd->scratch, text);
    uint8_t *bytes = (uint8_t *)rd->scratch.data;
    for (size_t i = 0; res == ODIN3_OK && i < rd->scratch.len; i++) {
        if (bytes[i] >= 'A' && bytes[i] <= 'Z') {
            bytes[i] = (uint8_t)(bytes[i] | ('a' - 'A'));
        }
    }
    *out = (odin3_bytes){rd->scratch.len > 0 ? rd->scratch.data : "", rd->scratch.len};
    return res;
}

/* Adds prefix + tag (+ "@" + attr when attr is not NULL) to the ignored set. */
static odin3_status ignore(o2_reader *rd, const char *prefix, odin3_bytes tag,
                           const odin3_bytes *attr) {
    odin3_strbuf_clear(&rd->scratch);
    odin3_status res = odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(prefix));
    res = res != ODIN3_OK ? res : odin3_strbuf_append(&rd->scratch, tag);
    if (attr != NULL) {
        res = res != ODIN3_OK ? res : odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr("@"));
        res = res != ODIN3_OK ? res : odin3_strbuf_append(&rd->scratch, *attr);
    }
    odin3_bytes name = {rd->scratch.data, rd->scratch.len};
    return res != ODIN3_OK ? res : odin3_project_add_ignored(rd->proj, name);
}

/* Lists every attribute of elem but keep (NULL: none kept) as "optimizations/<tag>@<attr>". */
static odin3_status ignore_attrs(o2_reader *rd, const odin3_xml_elem *elem, const char *keep,
                                 const char *keep2) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 0; res == ODIN3_OK && i < elem->nattrs; i++) {
        const odin3_xml_attr *attr = odin3_vec_cat(&rd->doc.attrs, elem->attrs + i);
        bool kept = (keep != NULL && odin3_proj_bytes_is(attr->name, keep)) ||
                    (keep2 != NULL && odin3_proj_bytes_is(attr->name, keep2));
        res = kept ? ODIN3_OK : ignore(rd, "optimizations/", elem->tag, &attr->name);
    }
    return res;
}

/* --- files and architectures --------------------------------------------------------------- */

/* The element's value as a path resolved against the file's directory; it must exist. */
static odin3_status elem_path(o2_reader *rd, const odin3_xml_elem *elem, odin3_proj_path *out) {
    odin3_bytes given = {NULL, 0};
    odin3_status res = value_of(rd, elem, &given);
    odin3_proj_path_req req = {rd->base, given, loc_of(rd, elem->off), ODIN3_PROJ_WANT_FILE};
    return res != ODIN3_OK ? res : odin3_project_path(rd->proj, &req, out);
}

/* A design file in `work` (verilog_file, input_path_and_name). */
static odin3_status design_file(o2_reader *rd, const odin3_xml_elem *elem, odin3_lang lang) {
    odin3_proj_file file = {.lang = (uint8_t)lang, .library = rd->work};
    odin3_status res = elem_path(rd, elem, &file.path);
    return res != ODIN3_OK ? res : odin3_project_add_file(rd->proj, &file);
}

/* <arch_file>: an architecture by its (lower-cased) suffix, then the file must exist. */
static odin3_status arch_file(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_bytes given = {NULL, 0};
    odin3_status res = value_of(rd, elem, &given);
    odin3_proj_arch arch = {.kind = 0};
    if (res == ODIN3_OK && !odin3_arch_kind_of(given, &arch.kind)) {
        return fail(rd, loc_of(rd, elem->off), ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE,
                    "not an architecture", &given);
    }
    res = res != ODIN3_OK ? res : elem_path(rd, elem, &arch.path);
    return res != ODIN3_OK ? res : odin3_project_add_arch(rd->proj, &arch);
}

/* --- root children ------------------------------------------------------------------------- */

static odin3_status verilog_files(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = elem->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        res = tag_is(child, "verilog_file") ? design_file(rd, child, ODIN3_LANG_VERILOG)
                                            : ignore(rd, "verilog_files/", child->tag, NULL);
        i = child->next_sibling;
    }
    return res;
}

/* The language of <inputs>: its one <input_type> lower-cased (verilog or blif), else verilog. */
static odin3_status input_lang(o2_reader *rd, const odin3_xml_elem *elem, odin3_lang *lang) {
    const odin3_xml_elem *type = NULL;
    for (uint32_t i = elem->first_child; i != ODIN3_XML_NONE; i = elem_at(rd, i)->next_sibling) {
        const odin3_xml_elem *child = elem_at(rd, i);
        if (tag_is(child, "input_type") && type != NULL) {
            return fail(rd, loc_of(rd, child->off), ODIN3_PROJ_ERR_SYNTAX,
                        "more than one <input_type>", NULL);
        }
        type = tag_is(child, "input_type") ? child : type;
    }
    *lang = ODIN3_LANG_VERILOG;
    if (type == NULL) {
        return ODIN3_OK;
    }
    odin3_bytes value = {NULL, 0};
    odin3_bytes lowered = {NULL, 0};
    odin3_status res = value_of(rd, type, &value);
    res = res != ODIN3_OK ? res : lower(rd, value, &lowered);
    if (res == ODIN3_OK && odin3_proj_bytes_is(lowered, "blif")) {
        *lang = ODIN3_LANG_BLIF;
    } else if (res == ODIN3_OK && !odin3_proj_bytes_is(lowered, "verilog")) {
        return fail(rd, loc_of(rd, type->off), ODIN3_PROJ_ERR_UNSUPPORTED_INPUT_TYPE,
                    "input_type must be verilog or blif", &value);
    }
    return res;
}

static odin3_status inputs(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    odin3_status res = input_lang(rd, elem, &lang);
    for (uint32_t i = elem->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        if (tag_is(child, "input_path_and_name")) {
            res = design_file(rd, child, lang);
        } else if (!tag_is(child, "input_type")) {
            res = ignore(rd, "inputs/", child->tag, NULL);
        }
        i = child->next_sibling;
    }
    return res;
}

static odin3_status target(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = elem->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        res = tag_is(child, "arch_file") ? arch_file(rd, child)
                                         : ignore(rd, "output/target/", child->tag, NULL);
        i = child->next_sibling;
    }
    return res;
}

/* What one <output> says. */
typedef struct o2_output {
    bool has_path, has_format;
    uint32_t path, format;
} o2_output;

/* One child of <output>. */
static odin3_status output_child(o2_reader *rd, const odin3_xml_elem *child, o2_output *out) {
    odin3_bytes value = {NULL, 0};
    odin3_status res = ODIN3_OK;
    if (tag_is(child, "output_type")) {
        res = value_of(rd, child, &value);
        res = res != ODIN3_OK ? res : lower(rd, value, &value);
        res = res != ODIN3_OK ? res : intern(rd, value, &out->format);
        out->has_format = true;
    } else if (tag_is(child, "output_path_and_name")) { /* normalized, need not exist */
        res = value_of(rd, child, &value);
        res = res != ODIN3_OK
                  ? res
                  : odin3_path_resolve((odin3_path_join){rd->base, value}, &rd->scratch);
        value = (odin3_bytes){rd->scratch.data, rd->scratch.len};
        res = res != ODIN3_OK ? res : intern(rd, value, &out->path);
        out->has_path = true;
    } else if (tag_is(child, "target")) {
        res = target(rd, child);
    } else {
        res = ignore(rd, "output/", child->tag, NULL);
    }
    return res;
}

/* <output>: recorded (whole, replacing an earlier one) only when it gives a path. */
static odin3_status output(o2_reader *rd, const odin3_xml_elem *elem) {
    o2_output out = {false, false, 0, 0};
    odin3_status res = ODIN3_OK;
    for (uint32_t i = elem->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        res = output_child(rd, child, &out);
        i = child->next_sibling;
    }
    if (res == ODIN3_OK && out.has_path) {
        rd->proj->output.present = true;
        rd->proj->output.path = out.path;
        rd->proj->output.format = out.format;
        rd->proj->output.has_format = out.has_format;
    }
    return res;
}

/* --- optimizations ------------------------------------------------------------------------- */

/* <multiply size=N> / <adder threshold_size=N>: map * <subject> to <cell> min_width N. */
static odin3_status map_rule(o2_reader *rd, const odin3_xml_elem *elem, const o2_map *spec) {
    odin3_bytes value = {NULL, 0};
    odin3_status res = ODIN3_OK;
    if (odin3_xml_attr_get(&rd->doc, elem, spec->attr, &value)) {
        odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_MAP, .at = loc_of(rd, elem->off)};
        rule.u.map.action = ODIN3_PROJ_ACTION_TO;
        rule.u.map.has = ODIN3_PROJ_HAS_MIN_WIDTH;
        uint32_t cell = 0;
        res = odin3_project_count(rd->proj, value, rule.at, &rule.u.map.min_width);
        res = res != ODIN3_OK ? res : intern(rd, odin3_bytes_cstr("*"), &rule.u.map.scope);
        res = res != ODIN3_OK ? res
                              : intern(rd, odin3_bytes_cstr(spec->subject), &rule.u.map.subject);
        res = res != ODIN3_OK ? res : intern(rd, odin3_bytes_cstr(spec->cell), &cell);
        res = res != ODIN3_OK ? res
                              : odin3_project_add_rule(rd->proj, &rule, (odin3_proj_ids){&cell, 1});
    }
    return res != ODIN3_OK ? res : ignore_attrs(rd, elem, spec->attr, NULL);
}

static bool is_digits(odin3_bytes text) {
    const char *bytes = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] < '0' || bytes[i] > '9') {
            return false;
        }
    }
    return text.len > 0;
}

/* split_memory_depth: min, max or digits (kept as written); NONE otherwise. */
static odin3_proj_depth depth_of(odin3_bytes depth) {
    if (odin3_proj_bytes_is(depth, "min")) {
        return ODIN3_PROJ_DEPTH_MIN;
    }
    if (odin3_proj_bytes_is(depth, "max")) {
        return ODIN3_PROJ_DEPTH_MAX;
    }
    return is_digits(depth) ? ODIN3_PROJ_DEPTH_N : ODIN3_PROJ_DEPTH_NONE;
}

/* <memory split_memory_width=0|1 split_memory_depth=min|max|N>: split $mem when either is set. */
static odin3_status memory(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_bytes width = {"0", 1};
    odin3_bytes depth = {NULL, 0};
    (void)odin3_xml_attr_get(&rd->doc, elem, "split_memory_width", &width);
    bool has_depth = odin3_xml_attr_get(&rd->doc, elem, "split_memory_depth", &depth);
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_SPLIT, .at = loc_of(rd, elem->off)};
    rule.u.split.width = odin3_proj_bytes_is(width, "1");
    rule.u.split.depth_kind = (uint8_t)(has_depth ? depth_of(depth) : ODIN3_PROJ_DEPTH_NONE);
    if ((!rule.u.split.width && !odin3_proj_bytes_is(width, "0")) ||
        (has_depth && rule.u.split.depth_kind == ODIN3_PROJ_DEPTH_NONE)) {
        return fail(rd, rule.at, ODIN3_PROJ_ERR_SYNTAX,
                    "split_memory_width 0|1, split_memory_depth min|max|<n>", NULL);
    }
    odin3_status res = ODIN3_OK;
    if (rule.u.split.width || has_depth) {
        res = intern(rd, odin3_bytes_cstr("$mem"), &rule.u.split.subject);
        if (res == ODIN3_OK && rule.u.split.depth_kind == ODIN3_PROJ_DEPTH_N) {
            res = intern(rd, depth, &rule.u.split.depth);
        }
        res = res != ODIN3_OK ? res : odin3_project_add_rule(rd->proj, &rule, (odin3_proj_ids){0});
    }
    return res != ODIN3_OK ? res
                           : ignore_attrs(rd, elem, "split_memory_width", "split_memory_depth");
}

static odin3_status optimizations(o2_reader *rd, const odin3_xml_elem *elem) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = elem->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        if (tag_is(child, "memory")) {
            res = memory(rd, child);
        } else if (tag_is(child, "multiply") || tag_is(child, "adder")) {
            res = map_rule(rd, child, tag_is(child, "multiply") ? &k_multiply : &k_adder);
        } else {
            res = ignore(rd, "optimizations/", child->tag, NULL);
        }
        i = child->next_sibling;
    }
    return res;
}

/* --- the file ------------------------------------------------------------------------------ */

/* The root must be <config>; its children are read in document order. */
static odin3_status read_root(o2_reader *rd) {
    const odin3_xml_elem *root = elem_at(rd, 0);
    if (!tag_is(root, "config")) {
        return fail(rd, loc_of(rd, root->off), ODIN3_PROJ_ERR_SYNTAX, "root must be <config>",
                    &root->tag);
    }
    odin3_status res = ODIN3_OK;
    for (uint32_t i = root->first_child; res == ODIN3_OK && i != ODIN3_XML_NONE;) {
        const odin3_xml_elem *child = elem_at(rd, i);
        if (tag_is(child, "verilog_files")) {
            res = verilog_files(rd, child);
        } else if (tag_is(child, "inputs")) {
            res = inputs(rd, child);
        } else if (tag_is(child, "output")) {
            res = output(rd, child);
        } else if (tag_is(child, "optimizations")) {
            res = optimizations(rd, child);
        } else {
            res = ignore(rd, "", child->tag, NULL);
        }
        i = child->next_sibling;
    }
    return res;
}

/* Parses the loaded file; an XML error (the node cap included) is `syntax` at its byte. */
static odin3_status parse(o2_reader *rd) {
    odin3_xml_err err;
    odin3_bytes text = {rd->text.len > 0 ? (const char *)rd->text.bytes : "", rd->text.len};
    odin3_status res = odin3_xml_parse(text, &rd->doc, &err);
    if (res != ODIN3_ERR_PARSE) {
        return res;
    }
    char detail[DETAIL_MAX];
    (void)snprintf(detail, sizeof detail, "%s%s", err.what,
                   err.over_cap ? " (ODIN3_PROJ_MAX_XML_NODES)" : "");
    return fail(rd, loc_of(rd, err.off), ODIN3_PROJ_ERR_SYNTAX, detail, NULL);
}

odin3_status odin3_read_odin2(odin3_project *proj, const odin3_proj_path *file) {
    o2_reader rd = {.proj = proj};
    odin3_vec_init(&rd.text.lines, sizeof(uint32_t));
    odin3_xml_doc_init(&rd.doc, proj->limits.xml_nodes);
    odin3_strbuf_init(&rd.value);
    odin3_strbuf_init(&rd.scratch);
    odin3_status res = odin3_design_get_srcman(proj->design, &rd.sm);
    res = res != ODIN3_OK ? res : intern(&rd, odin3_bytes_cstr("work"), &rd.work);
    if (res == ODIN3_OK) {
        rd.base = odin3_path_dir(odin3_proj_str_bytes(proj, file->resolved));
        res = odin3_project_load(proj, file, (odin3_loc){0}, &rd.text);
    }
    if (res == ODIN3_OK) {
        const uint8_t *newline = rd.text.len > 0 ? memchr(rd.text.bytes, '\n', rd.text.len) : NULL;
        rd.line1_end = newline != NULL ? (uint32_t)(newline - rd.text.bytes) : rd.text.len;
    }
    res = res != ODIN3_OK ? res : parse(&rd);
    res = res != ODIN3_OK ? res : read_root(&rd);
    odin3_proj_text_free(&rd.text);
    odin3_xml_doc_free(&rd.doc);
    odin3_strbuf_free(&rd.value);
    odin3_strbuf_free(&rd.scratch);
    return res;
}
