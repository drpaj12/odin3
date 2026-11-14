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

static void write_testbench(json *jw, const void *elem) {
    static const char *const on_names[] = {"\"?\"", "\"rtl\"", "\"netlist\"", "\"both\""};
    const odin3_proj_testbench *tb = elem;
    raw(jw, "{");
    key(jw, "name", true);
    string_id(jw, tb->name);
    key(jw, "file", false);
    string_id(jw, tb->file.resolved);
    key(jw, "top", false);
    string_or_null(jw, tb->top != 0, tb->top);
    key(jw, "dut", false);
    string_or_null(jw, tb->dut != 0, tb->dut);
    key(jw, "max_time", false);
    if (tb->max_time == 0) {
        raw(jw, "null");
    } else if (jw->st == ODIN3_OK) {
        jw->st = odin3_strbuf_appendf(jw->out, "%" PRIu64, tb->max_time);
    }
    key(jw, "expect", false);
    string_or_null(jw, tb->expect.given != 0, tb->expect.resolved);
    key(jw, "on", false);
    raw(jw, on_names[tb->on <= ODIN3_PROJ_TB_BOTH ? tb->on : 0]);
    if (tb->timeboxed) {
        raw(jw, ", \"timeboxed\": true");
    }
    if (tb->disabled) {
        raw(jw, ", \"disabled\": true");
    }
    raw(jw, "}");
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
    if (proj->testbenches.len > 0) {
        field_array(jw, "testbenches", &proj->testbenches, write_testbench);
    }
}

/* --- flows (schema 2) ---------------------------------------------------------------------- */

typedef struct str_view {
    const uint8_t *ptr;
    size_t len;
} str_view;

static str_view id_bytes(const json *jw, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(jw->proj->design);
    return (str_view){(const uint8_t *)odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
}

static bool id_is(const json *jw, uint32_t str, const char *text) {
    str_view sv = id_bytes(jw, str);
    return sv.len == strlen(text) && memcmp(sv.ptr, text, sv.len) == 0;
}

static const uint32_t *rec_words(const json *jw, const odin3_proj_flowrec *rec) {
    return (const uint32_t *)jw->proj->flowwords.data + rec->first;
}

/* The word after keyword kw (from position first on), if there is one. */
static bool kw_value(const json *jw, const odin3_proj_flowrec *rec, uint32_t first, const char *kw,
                     uint32_t *value) {
    const uint32_t *wd = rec_words(jw, rec);
    for (uint32_t i = first; i + 1 < rec->count; i += 2) {
        if (id_is(jw, wd[i], kw)) {
            *value = wd[i + 1];
            return true;
        }
    }
    return false;
}

/* A keyword of a statement and the JSON field its value is written to. */
typedef struct kw_field {
    const char *kw;
    const char *field;
} kw_field;

static void optional_kw(json *jw, const odin3_proj_flowrec *rec, uint32_t first, kw_field kf) {
    uint32_t value = 0;
    key(jw, kf.field, false);
    bool present = kw_value(jw, rec, first, kf.kw, &value);
    string_or_null(jw, present, value);
}

/* A comma list as a JSON array of strings. */
static void comma_list(json *jw, uint32_t str) {
    str_view sv = id_bytes(jw, str);
    size_t start = 0;
    raw(jw, "[");
    for (size_t i = 0; i <= sv.len; i++) {
        if (i == sv.len || sv.ptr[i] == ',') {
            raw(jw, start > 0 ? ", " : "");
            string_bytes(jw, sv.ptr + start, i - start);
            start = i + 1;
        }
    }
    raw(jw, "]");
}

/* The  words after keyword kw, as [{"role", "path"}]. */
static void role_paths(json *jw, const odin3_proj_flowrec *rec, const char *kw) {
    const uint32_t *wd = rec_words(jw, rec);
    bool first = true;
    raw(jw, "[");
    for (uint32_t i = 2; i + 1 < rec->count; i += 2) {
        if (!id_is(jw, wd[i], kw)) {
            continue;
        }
        str_view sv = id_bytes(jw, wd[i + 1]);
        size_t eq = 0;
        while (eq < sv.len && sv.ptr[eq] != '=') {
            eq++;
        }
        raw(jw, first ? "{" : ", {");
        first = false;
        key(jw, "role", true);
        string_bytes(jw, sv.ptr, eq);
        key(jw, "path", false);
        string_bytes(jw, sv.ptr + eq + 1, sv.len - eq - 1);
        raw(jw, "}");
    }
    raw(jw, "]");
}

static void write_provlink(json *jw, const odin3_proj_flowrec *rec) {
    raw(jw, "{");
    key(jw, "from", true);
    uint32_t val = 0;
    (void)kw_value(jw, rec, 2, "from", &val);
    string_id(jw, val);
    key(jw, "kind", false);
    (void)kw_value(jw, rec, 2, "kind", &val);
    string_id(jw, val);
    optional_kw(jw, rec, 2, (kw_field){"key", "key"});
    optional_kw(jw, rec, 2, (kw_field){"scope", "scope"});
    optional_kw(jw, rec, 2, (kw_field){"confidence", "confidence"});
    raw(jw, "}");
}

static void write_step(json *jw, const odin3_proj_flowrec *rec) {
    const uint32_t *wd = rec_words(jw, rec);
    const odin3_vec *recs = &jw->proj->flowrecs;
    uint32_t val = 0;
    raw(jw, "{");
    key(jw, "id", true);
    string_id(jw, wd[1]);
    key(jw, "stage", false);
    (void)kw_value(jw, rec, 2, "stage", &val);
    string_id(jw, val);
    key(jw, "tool", false);
    (void)kw_value(jw, rec, 2, "tool", &val);
    string_id(jw, val);
    key(jw, "needs", false);
    if (kw_value(jw, rec, 2, "needs", &val)) {
        comma_list(jw, val);
    } else {
        raw(jw, "[]");
    }
    key(jw, "in", false);
    role_paths(jw, rec, "in");
    key(jw, "out", false);
    role_paths(jw, rec, "out");
    optional_kw(jw, rec, 2, (kw_field){"args", "args"});
    key(jw, "provlinks", false);
    raw(jw, "[");
    bool first = true;
    for (size_t i = 0; i < recs->len; i++) {
        const odin3_proj_flowrec *pl = odin3_vec_cat(recs, i);
        const uint32_t *pw = rec_words(jw, pl);
        if (pl->kind == ODIN3_PROJ_FLOW_PROVLINK && pw[0] == wd[0] && pw[1] == wd[1]) {
            raw(jw, first ? "" : ", ");
            first = false;
            write_provlink(jw, pl);
        }
    }
    raw(jw, "]}");
}

static void write_tool(json *jw, const odin3_proj_flowrec *rec) {
    const uint32_t *wd = rec_words(jw, rec);
    raw(jw, "{");
    key(jw, "id", true);
    string_id(jw, wd[0]);
    key(jw, "required", false);
    raw(jw, id_is(jw, wd[1], "required") ? "true" : "false");
    key(jw, "exe", false);
    string_id(jw, wd[2]);
    optional_kw(jw, rec, 3, (kw_field){"version-arg", "version_arg"});
    optional_kw(jw, rec, 3, (kw_field){"module", "module"});
    optional_kw(jw, rec, 3, (kw_field){"env", "env"});
    raw(jw, "}");
}

static void write_flow(json *jw, const odin3_proj_flowrec *rec) {
    const uint32_t *wd = rec_words(jw, rec);
    const odin3_vec *recs = &jw->proj->flowrecs;
    raw(jw, "{");
    key(jw, "name", true);
    string_id(jw, wd[0]);
    key(jw, "description", false);
    string_or_null(jw, rec->count == 3, rec->count == 3 ? wd[2] : 0);
    key(jw, "vars", false);
    raw(jw, "{");
    bool first = true;
    for (size_t i = 0; i < recs->len; i++) {
        const odin3_proj_flowrec *vr = odin3_vec_cat(recs, i);
        const uint32_t *vw = rec_words(jw, vr);
        if (vr->kind == ODIN3_PROJ_FLOW_VAR && vw[0] == wd[0]) {
            raw(jw, first ? "" : ", ");
            first = false;
            string_id(jw, vw[1]);
            raw(jw, ": ");
            string_id(jw, vw[2]);
        }
    }
    raw(jw, "}");
    key(jw, "steps", false);
    raw(jw, "[");
    first = true;
    for (size_t i = 0; i < recs->len; i++) {
        const odin3_proj_flowrec *sr = odin3_vec_cat(recs, i);
        if (sr->kind == ODIN3_PROJ_FLOW_STEP && rec_words(jw, sr)[0] == wd[0]) {
            raw(jw, first ? "" : ", ");
            first = false;
            write_step(jw, sr);
        }
    }
    raw(jw, "]}");
}

typedef void (*rec_writer)(json *jw, const odin3_proj_flowrec *rec);

/* `"name": [...]`, the statements of one kind written by write. */
static void dump_kind(json *jw, const char *name, odin3_proj_flowrec_kind kind, rec_writer write) {
    const odin3_vec *recs = &jw->proj->flowrecs;
    field(jw, name);
    raw(jw, "[");
    bool first = true;
    for (size_t i = 0; i < recs->len; i++) {
        const odin3_proj_flowrec *rec = odin3_vec_cat(recs, i);
        if (rec->kind == kind) {
            raw(jw, first ? "" : ", ");
            first = false;
            write(jw, rec);
        }
    }
    raw(jw, "],\n");
}

/* Files with flow or tool statements only: "schema", "tools" and "flows". A bare `schema 2` file
 * dumps exactly like a schema 1 file. */
static void dump_flows(json *jw) {
    field(jw, "schema");
    number(jw, jw->proj->schema);
    raw(jw, ",\n");
    dump_kind(jw, "tools", ODIN3_PROJ_FLOW_TOOL, write_tool);
    dump_kind(jw, "flows", ODIN3_PROJ_FLOW_DEF, write_flow);
}

/* prov_level and prov_budget, only when the project gives them (the dump of other projects is
 * unchanged). */
static void write_prov(json *jw) {
    const odin3_project *proj = jw->proj;
    if (proj->prov_set) {
        field(jw, "prov_level");
        if (proj->prov_auto) {
            raw(jw, "\"auto\"");
        } else {
            number(jw, proj->prov_level);
        }
        raw(jw, ",\n");
    }
    if (proj->prov_budget != 0) {
        field(jw, "prov_budget");
        if (jw->st == ODIN3_OK) {
            jw->st = odin3_strbuf_appendf(jw->out, "%" PRIu64, proj->prov_budget);
        }
        raw(jw, ",\n");
    }
}

odin3_status odin3_project_dump(const odin3_project *proj, odin3_strbuf *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    json jw = {proj, &text, ODIN3_OK};
    raw(&jw, "{\n");
    dump_lists(&jw);
    if (proj->schema == 2 && proj->flowrecs.len > 0) {
        dump_flows(&jw);
    }
    field(&jw, "flow");
    string_or_null(&jw, proj->flow.given != 0, proj->flow.resolved);
    raw(&jw, ",\n");
    field(&jw, "abc_script");
    string_or_null(&jw, proj->abc_script.given != 0, proj->abc_script.resolved);
    raw(&jw, ",\n");
    if (proj->timing_hints.given != 0) {
        field(&jw, "timing_hints");
        string_or_null(&jw, true, proj->timing_hints.resolved);
        raw(&jw, ",\n");
    }
    write_prov(&jw);
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
