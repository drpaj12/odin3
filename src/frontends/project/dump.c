/*
 * dump.c — the project record as JSON (PRJ-4): README keys in order, one per line, values on one
 * line with Python's separators, UTF-8 with JSON escapes.
 */
#include "frontends/project/project.h"

#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    UTF8_CONT_MASK = 0xC0,
    UTF8_CONT = 0x80,
    CTRL_END = 0x20,
    MAX_SEQ = 4,
    CP2_MIN = 0x80,
    CP3_MIN = 0x800,
    CP4_MIN = 0x10000,
    CP_MAX = 0x10FFFF,
    SURROGATE_LO = 0xD800,
    SURROGATE_HI = 0xDFFF,
    LEAD2 = 0xC0,
    LEAD3 = 0xE0,
    LEAD4 = 0xF0,
    LEAD_END = 0xF8,
    BITS2 = 0x1F,
    BITS3 = 0x0F,
    BITS4 = 0x07,
    CONT_BITS = 0x3F,
    CONT_SHIFT = 6
};

/* The writer: the output and the first failure (later appends are skipped). */
typedef struct json {
    const odin3_project *proj;
    odin3_strbuf *out;
    odin3_status st;
} json;

static void raw(json *jw, const char *text) {
    if (jw->st == ODIN3_OK) {
        jw->st = odin3_strbuf_append(jw->out, odin3_bytes_cstr(text));
    }
}

static void raw_bytes(json *jw, const uint8_t *bytes, size_t len) {
    if (jw->st == ODIN3_OK) {
        jw->st = odin3_strbuf_append(jw->out, (odin3_bytes){bytes, len});
    }
}

static void number(json *jw, uint32_t value) {
    if (jw->st == ODIN3_OK) {
        jw->st = odin3_strbuf_appendf(jw->out, "%" PRIu32, value);
    }
}

/* Length of the valid UTF-8 sequence at s[0, n), 0 when s[0] does not start one (RFC 3629: no
 * overlong forms, no surrogates, nothing above U+10FFFF — Python's decoder). */
static size_t utf8_len(const uint8_t *seq, size_t avail) {
    size_t len = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if (seq[0] >= LEAD2 && seq[0] < LEAD3) {
        len = 2;
        cp = seq[0] & BITS2;
        min = CP2_MIN;
    } else if (seq[0] >= LEAD3 && seq[0] < LEAD4) {
        len = 3;
        cp = seq[0] & BITS3;
        min = CP3_MIN;
    } else if (seq[0] >= LEAD4 && seq[0] < LEAD_END) {
        len = MAX_SEQ;
        cp = seq[0] & BITS4;
        min = CP4_MIN;
    }
    if (len == 0 || len > avail) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((seq[i] & UTF8_CONT_MASK) != UTF8_CONT) {
            return 0;
        }
        cp = cp << CONT_SHIFT | (seq[i] & CONT_BITS);
    }
    bool surrogate = cp >= SURROGATE_LO && cp <= SURROGATE_HI;
    return cp < min || cp > CP_MAX || surrogate ? 0 : len;
}

/* One byte that needs an escape: a short form for the seven Python uses, else \u00XX. */
static void escape_byte(json *jw, uint8_t byte) {
    static const char short_from[] = "\"\\\n\r\t\b\f";
    static const char short_to[] = "\"\\nrtbf";
    const char *hit = byte != 0 ? strchr(short_from, byte) : NULL;
    if (hit != NULL) {
        const char esc[3] = {'\\', short_to[hit - short_from], '\0'};
        raw(jw, esc);
    } else if (jw->st == ODIN3_OK) {
        jw->st = odin3_strbuf_appendf(jw->out, "\\u%04x", (unsigned)byte);
    }
}

/* Whether a byte is copied as is (printable ASCII other than '"' and '\\'). */
static bool plain(uint8_t byte) {
    return byte >= CTRL_END && byte < UTF8_CONT && byte != '"' && byte != '\\';
}

static void string_bytes(json *jw, const uint8_t *bytes, size_t len) {
    raw(jw, "\"");
    size_t i = 0;
    while (i < len) {
        size_t run = i;
        while (run < len && plain(bytes[run])) {
            run++;
        }
        raw_bytes(jw, bytes + i, run - i);
        i = run;
        if (i >= len) {
            break;
        }
        size_t seq = bytes[i] >= UTF8_CONT ? utf8_len(bytes + i, len - i) : 0;
        if (seq > 0) {
            raw_bytes(jw, bytes + i, seq);
            i += seq;
        } else {
            escape_byte(jw, bytes[i++]);
        }
    }
    raw(jw, "\"");
}

static void string_id(json *jw, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(jw->proj->design);
    string_bytes(jw, (const uint8_t *)odin3_strtab_get(tab, str), odin3_strtab_len(tab, str));
}

static void string_or_null(json *jw, bool present, uint32_t str) {
    if (present) {
        string_id(jw, str);
    } else {
        raw(jw, "null");
    }
}

/* `"key": ` with the separator before it (first: none). */
static void key(json *jw, const char *name, bool first) {
    raw(jw, first ? "\"" : ", \"");
    raw(jw, name);
    raw(jw, "\": ");
}

/* --- fields -------------------------------------------------------------------------------- */

typedef void (*elem_writer)(json *jw, const void *elem);

static void array(json *jw, const odin3_vec *vec, elem_writer write) {
    raw(jw, "[");
    for (size_t i = 0; i < vec->len; i++) {
        raw(jw, i > 0 ? ", " : "");
        write(jw, odin3_vec_cat(vec, i));
    }
    raw(jw, "]");
}

static void write_id(json *jw, const void *elem) {
    string_id(jw, *(const uint32_t *)elem);
}

static void write_path(json *jw, const void *elem) {
    string_id(jw, ((const odin3_proj_path *)elem)->resolved);
}

/* A files entry always has its library ("" included: ID 0 is the empty string); a libfiles
 * entry has none. */
static void file_fields(json *jw, const odin3_proj_file *file, bool with_library) {
    const char *lang = odin3_lang_name((odin3_lang)file->lang);
    raw(jw, "{");
    key(jw, "path", true);
    string_id(jw, file->path.resolved);
    key(jw, "language", false);
    raw(jw, "\"");
    raw(jw, lang != NULL ? lang : "?");
    raw(jw, "\"");
    if (with_library) {
        key(jw, "library", false);
        string_id(jw, file->library);
    }
    raw(jw, "}");
}

static void write_file(json *jw, const void *elem) {
    file_fields(jw, elem, true);
}

static void write_libfile(json *jw, const void *elem) {
    file_fields(jw, elem, false);
}

static void write_define(json *jw, const void *elem) {
    const odin3_proj_define *def = elem;
    raw(jw, "{");
    key(jw, "name", true);
    string_id(jw, def->name);
    key(jw, "value", false);
    string_or_null(jw, def->has_value, def->value);
    raw(jw, "}");
}

static void write_param(json *jw, const void *elem) {
    const odin3_proj_param *param = elem;
    raw(jw, "{");
    key(jw, "scope", true);
    string_id(jw, param->scope);
    key(jw, "name", false);
    string_id(jw, param->name);
    key(jw, "value", false);
    string_id(jw, param->value);
    key(jw, "type", false);
    raw(jw, param->type == ODIN3_PARAM_NUMBER ? "\"number\"}" : "\"string\"}");
}

static void write_arch(json *jw, const void *elem) {
    const odin3_proj_arch *arch = elem;
    if (arch->kind == ODIN3_PROJ_ARCH_DEVICE) {
        raw(jw, "{\"kind\": \"device\"");
        key(jw, "family", false);
        string_or_null(jw, arch->has_family, arch->family);
        key(jw, "device", false);
        string_or_null(jw, arch->has_device, arch->device);
    } else {
        bool o3lib = arch->kind == ODIN3_PROJ_ARCH_O3LIB;
        raw(jw, o3lib ? "{\"kind\": \"o3lib\"" : "{\"kind\": \"vpr_xml\"");
        key(jw, "path", false);
        string_id(jw, arch->path.resolved);
    }
    raw(jw, "}");
}

static void write_map(json *jw, const odin3_proj_rule *rule) {
    static const char *const actions[] = {"\"?\"", "\"to\"", "\"soft\"", "\"keep\""};
    static const char *const thresholds[] = {"min_width", "max_width", "min_depth"};
    raw(jw, "{\"rule\": \"map\"");
    key(jw, "scope", false);
    string_id(jw, rule->u.map.scope);
    key(jw, "subject", false);
    string_id(jw, rule->u.map.subject);
    key(jw, "action", false);
    size_t action = rule->u.map.action;
    raw(jw, actions[action < sizeof actions / sizeof actions[0] ? action : 0]);
    key(jw, "cells", false);
    raw(jw, "[");
    const uint32_t *cells = jw->proj->rule_cells.data;
    for (uint32_t i = 0; i < rule->u.map.ncells; i++) {
        raw(jw, i > 0 ? ", " : "");
        string_id(jw, cells[rule->u.map.cells + i]);
    }
    raw(jw, "]");
    const uint32_t values[] = {rule->u.map.min_width, rule->u.map.max_width, rule->u.map.min_depth};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if ((rule->u.map.has & (1U << i)) != 0) {
            key(jw, thresholds[i], false);
            number(jw, values[i]);
        }
    }
    raw(jw, "}");
}

static void write_split(json *jw, const odin3_proj_rule *rule) {
    raw(jw, "{\"rule\": \"split\"");
    key(jw, "subject", false);
    string_id(jw, rule->u.split.subject);
    key(jw, "width", false);
    raw(jw, rule->u.split.width ? "true" : "false");
    key(jw, "depth", false);
    switch (rule->u.split.depth_kind) {
    case ODIN3_PROJ_DEPTH_MIN:
        raw(jw, "\"min\"}");
        return;
    case ODIN3_PROJ_DEPTH_MAX:
        raw(jw, "\"max\"}");
        return;
    case ODIN3_PROJ_DEPTH_N:
        string_id(jw, rule->u.split.depth);
        raw(jw, "}");
        return;
    default:
        raw(jw, "null}");
        return;
    }
}

static void write_rule(json *jw, const void *elem) {
    const odin3_proj_rule *rule = elem;
    switch (rule->kind) {
    case ODIN3_PROJ_RULE_MAP:
        write_map(jw, rule);
        return;
    case ODIN3_PROJ_RULE_SPLIT:
        write_split(jw, rule);
        return;
    case ODIN3_PROJ_RULE_LIMIT:
        raw(jw, "{\"rule\": \"limit\"");
        key(jw, "cell", false);
        string_id(jw, rule->u.limit.cell);
        key(jw, "count", false);
        number(jw, rule->u.limit.count);
        raw(jw, "}");
        return;
    default:
        raw(jw, "{\"rule\": \"patterns\"");
        key(jw, "path", false);
        string_id(jw, rule->u.patterns.path.resolved);
        raw(jw, "}");
        return;
    }
}

static void write_inventory(json *jw, const void *elem) {
    const odin3_proj_inventory *inv = elem;
    raw(jw, "{");
    key(jw, "cell", true);
    string_id(jw, inv->cell);
    key(jw, "count", false);
    number(jw, inv->count);
    raw(jw, "}");
}

static void write_override(json *jw, const void *elem) {
    const odin3_proj_override *ov = elem;
    if (ov->kind == ODIN3_PROJ_OVERRIDE_HIDE) {
        raw(jw, "{\"override\": \"hide\"");
        key(jw, "cell", false);
        string_id(jw, ov->name);
        raw(jw, "}");
        return;
    }
    raw(jw, "{\"override\": \"cell\"");
    key(jw, "name", false);
    string_id(jw, ov->name);
    key(jw, "from", false);
    string_id(jw, ov->from);
    key(jw, "params", false);
    raw(jw, "{");
    const odin3_proj_kv *kvs = jw->proj->override_params.data;
    for (uint32_t i = 0; i < ov->nparams; i++) {
        raw(jw, i > 0 ? ", " : "");
        string_id(jw, kvs[ov->params + i].key);
        raw(jw, ": ");
        string_id(jw, kvs[ov->params + i].value);
    }
    raw(jw, "}}");
}

static void write_output(json *jw) {
    if (!jw->proj->output.present) {
        raw(jw, "null");
        return;
    }
    raw(jw, "{");
    key(jw, "path", true);
    string_id(jw, jw->proj->output.path);
    key(jw, "format", false);
    string_or_null(jw, jw->proj->output.has_format, jw->proj->output.format);
    raw(jw, "}");
}

/* --- the record ---------------------------------------------------------------------------- */

/* One top-level line `  "name": `. */
static void field(json *jw, const char *name) {
    raw(jw, "  \"");
    raw(jw, name);
    raw(jw, "\": ");
}

static void field_array(json *jw, const char *name, const odin3_vec *vec, elem_writer write) {
    field(jw, name);
    array(jw, vec, write);
    raw(jw, ",\n");
}

static void dump_lists(json *jw) {
    const odin3_project *proj = jw->proj;
    field_array(jw, "files", &proj->files, write_file);
    field_array(jw, "libfiles", &proj->libfiles, write_libfile);
    field_array(jw, "libdirs", &proj->libdirs, write_path);
    field_array(jw, "libext", &proj->libext, write_id);
    field_array(jw, "incdirs", &proj->incdirs, write_path);
    field_array(jw, "defines", &proj->defines, write_define);
    field(jw, "top");
    string_or_null(jw, proj->top != 0, proj->top);
    raw(jw, ",\n");
    field_array(jw, "params", &proj->params, write_param);
    field_array(jw, "arch", &proj->arch, write_arch);
    field_array(jw, "rules", &proj->rules, write_rule);
    field_array(jw, "inventory", &proj->inventory, write_inventory);
    field_array(jw, "overrides", &proj->overrides, write_override);
}

odin3_status odin3_project_dump(const odin3_project *proj, odin3_strbuf *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    json jw = {proj, &text, ODIN3_OK};
    raw(&jw, "{\n");
    dump_lists(&jw);
    field(&jw, "flow");
    string_or_null(&jw, proj->flow.given != 0, proj->flow.resolved);
    raw(&jw, ",\n");
    field(&jw, "output");
    write_output(&jw);
    raw(&jw, ",\n");
    field_array(&jw, "ignored", &proj->ignored, write_id);
    field(&jw, "duplicates");
    array(&jw, &proj->duplicates, write_id);
    raw(&jw, "\n}\n");
    if (jw.st == ODIN3_OK) {
        jw.st = odin3_strbuf_append(out, (odin3_bytes){text.data, text.len});
    }
    odin3_strbuf_free(&text);
    return jw.st;
}

static const char *const k_lang_names[] = {NULL,   "verilog", "systemverilog", "vhdl",
                                           "blif", "vqm",     "edif"};

const char *odin3_lang_name(odin3_lang lang) {
    size_t count = sizeof k_lang_names / sizeof k_lang_names[0];
    return (size_t)lang < count ? k_lang_names[lang] : NULL;
}
