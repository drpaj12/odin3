/*
 * o3proj.c — the .o3proj reader (PRJ-10, the oracle's parse_o3proj): one statement per line, the
 * §5.1 key table with its arities, `param` scopes checked against the file's own top after the
 * file, and `import` of a Quartus project.
 */
#include "frontends/project/project.h"

#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/tcl.h"
#include "frontends/project/words.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MAX_ARGS = 99, DETAIL_MAX = 256 };

typedef struct o3_reader {
    odin3_project *proj;
    odin3_srcman *sm;
    odin3_proj_text text;
    odin3_bytes base;     /* the file's directory (a view of a strtab string) */
    uint32_t work;        /* "work" */
    odin3_vec words;      /* odin3_word: the line being read */
    odin3_vec params;     /* size_t: index in proj->params of each `param` line's entry */
    odin3_vec ids;        /* uint32_t: map cells */
    odin3_vec kvs;        /* odin3_proj_kv: cell parameters */
    odin3_strbuf scratch; /* map cells joined; an import's resolved path */
} o3_reader;

struct o3_key;

/* One statement: the words after the key, the key's location and its table row. */
typedef struct o3_stmt {
    const odin3_word *args;
    uint32_t nargs;
    odin3_loc at;
    const struct o3_key *key;
} o3_stmt;

typedef odin3_status (*o3_handler)(o3_reader *rd, const o3_stmt *st);

typedef struct o3_key {
    const char *name;
    uint32_t lo, hi; /* words after the key */
    const char *usage;
    o3_handler handler;
} o3_key;

/* --- helpers ------------------------------------------------------------------------------- */

static bool arg_is(const o3_stmt *st, uint32_t idx, const char *text) {
    return idx < st->nargs && odin3_proj_bytes_is(st->args[idx].text, text);
}

/* An error at at naming name (none when name is NULL). */
static odin3_status fail(o3_reader *rd, odin3_loc at, odin3_proj_err_kind kind, const char *detail,
                         const odin3_bytes *name) {
    odin3_proj_err err = {kind, ODIN3_PROJ_LOCATED, at, NULL, detail, name, name != NULL};
    return odin3_project_fail(rd->proj, &err);
}

static odin3_status syntax(o3_reader *rd, const o3_stmt *st, const char *detail,
                           const odin3_bytes *name) {
    return fail(rd, st->at, ODIN3_PROJ_ERR_SYNTAX, detail, name);
}

/* `syntax` "usage: <the key's usage>" at the statement, naming name (none when NULL). */
static odin3_status usage(o3_reader *rd, const o3_stmt *st, const odin3_bytes *name) {
    char detail[DETAIL_MAX];
    (void)snprintf(detail, sizeof detail, "usage: %s", st->key->usage);
    return syntax(rd, st, detail, name);
}

/* A word as a path resolved against the file's directory, located at the word. */
static odin3_status arg_path(o3_reader *rd, const odin3_word *word, odin3_proj_want want,
                             odin3_proj_path *out) {
    odin3_loc at = odin3_srcman_loc(rd->sm, rd->text.buffer, word->off);
    odin3_proj_path_req req = {rd->base, word->text, at, want};
    return odin3_project_path(rd->proj, &req, out);
}

static odin3_status intern(o3_reader *rd, odin3_bytes text, uint32_t *str) {
    return odin3_project_intern(rd->proj, text, str);
}

/* --- files, directories, defines, top ------------------------------------------------------ */

static odin3_status key_file(o3_reader *rd, const o3_stmt *st) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    bool library = st->nargs == 4 && arg_is(st, 2, "library");
    if (!odin3_lang_by_name(st->args[0].text, &lang) || (st->nargs > 2 && !library)) {
        return usage(rd, st, NULL);
    }
    odin3_proj_file file = {.lang = (uint8_t)lang, .library = rd->work};
    odin3_status res = arg_path(rd, &st->args[1], ODIN3_PROJ_WANT_FILE, &file.path);
    if (res == ODIN3_OK && library) {
        res = intern(rd, st->args[3].text, &file.library);
    }
    return res != ODIN3_OK ? res : odin3_project_add_file(rd->proj, &file);
}

static odin3_status key_libfile(o3_reader *rd, const o3_stmt *st) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    if (!odin3_lang_by_name(st->args[0].text, &lang)) {
        return syntax(rd, st, "unknown language", &st->args[0].text);
    }
    odin3_proj_file file = {.lang = (uint8_t)lang, .library = 0};
    odin3_status res = arg_path(rd, &st->args[1], ODIN3_PROJ_WANT_FILE, &file.path);
    return res != ODIN3_OK ? res : odin3_project_add_libfile(rd->proj, &file);
}

static odin3_status add_dir(o3_reader *rd, const o3_stmt *st, odin3_proj_dir_list list) {
    odin3_proj_path dir = {0, 0, {0}};
    odin3_status res = arg_path(rd, &st->args[0], ODIN3_PROJ_WANT_DIR, &dir);
    return res != ODIN3_OK ? res : odin3_project_add_dir(rd->proj, list, &dir);
}

static odin3_status key_libdir(o3_reader *rd, const o3_stmt *st) {
    return add_dir(rd, st, ODIN3_PROJ_LIBDIRS);
}

static odin3_status key_incdir(o3_reader *rd, const o3_stmt *st) {
    return add_dir(rd, st, ODIN3_PROJ_INCDIRS);
}

static odin3_status key_libext(o3_reader *rd, const o3_stmt *st) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 0; res == ODIN3_OK && i < st->nargs; i++) {
        res = odin3_project_add_libext(rd->proj, st->args[i].text, st->at);
    }
    return res;
}

static odin3_status key_define(o3_reader *rd, const o3_stmt *st) {
    return odin3_project_add_define(rd->proj, st->args[0].text, st->at);
}

static odin3_status key_top(o3_reader *rd, const o3_stmt *st) {
    return odin3_project_set_top(rd->proj, st->args[0].text, st->at);
}

/* --- param --------------------------------------------------------------------------------- */

/* Whether text is IDENT(.IDENT)*. */
static bool is_scope(odin3_bytes text) {
    const char *bytes = text.ptr;
    size_t pos = 0;
    for (;;) {
        size_t len = odin3_proj_ident_len(text, pos);
        if (len == 0) {
            return false;
        }
        pos += len;
        if (pos == text.len) {
            return true;
        }
        if (bytes[pos] != '.') {
            return false;
        }
        pos++;
    }
}

/* Splits a param path at its last '.' into an IDENT(.IDENT)* scope and an IDENT name. */
static bool param_path(odin3_bytes path, odin3_bytes *scope, odin3_bytes *name) {
    const char *text = path.ptr;
    size_t dot = path.len;
    while (dot > 0 && text[dot - 1] != '.') {
        dot--;
    }
    *scope = (odin3_bytes){text, dot > 0 ? dot - 1 : 0};
    *name = (odin3_bytes){text + dot, path.len - dot};
    return dot > 0 && is_scope(*scope) && name->len > 0 &&
           odin3_proj_ident_len(*name, 0) == name->len;
}

/* param <top>[.<inst>…].<name> <value>: scope and name split at the last '.'; a quoted value is a
 * string, a bare one must be a NUMBER. The scope's first component is checked after the file. */
static odin3_status key_param(o3_reader *rd, const o3_stmt *st) {
    odin3_bytes scope = {NULL, 0};
    odin3_bytes name = {NULL, 0};
    if (!param_path(st->args[0].text, &scope, &name)) {
        return usage(rd, st, &st->args[0].text);
    }
    const odin3_word *value = &st->args[1];
    odin3_proj_param param = {.type = ODIN3_PARAM_STRING, .at = st->at};
    if (!value->quoted) {
        if (!odin3_proj_is_number(value->text)) {
            return syntax(rd, st, "parameter value: quote a string, or give a number",
                          &value->text);
        }
        param.type = ODIN3_PARAM_NUMBER;
    }
    odin3_status res = intern(rd, scope, &param.scope);
    res = res != ODIN3_OK ? res : intern(rd, name, &param.name);
    res = res != ODIN3_OK ? res : intern(rd, value->text, &param.value);
    size_t *slot = res == ODIN3_OK ? odin3_vec_push(&rd->params) : NULL;
    if (slot == NULL) {
        return res != ODIN3_OK ? res : ODIN3_ERR_NO_MEMORY;
    }
    *slot = rd->proj->params.len;
    res = odin3_project_add_param(rd->proj, &param);
    if (res != ODIN3_OK) {
        odin3_vec_pop(&rd->params);
    }
    return res;
}

/* The module name of the top (library stripped), "" when there is none. */
static odin3_bytes top_module(const o3_reader *rd) {
    if (rd->proj->top == 0) {
        return (odin3_bytes){"", 0};
    }
    odin3_bytes top = odin3_proj_str_bytes(rd->proj, rd->proj->top);
    const char *text = top.ptr;
    size_t mod = top.len;
    while (mod > 0 && text[mod - 1] != '.') {
        mod--;
    }
    return (odin3_bytes){text + mod, top.len - mod};
}

/* After the file: each `param` line's scope must start at the file's own top (its module name,
 * library stripped); else `syntax` at the line, names [scope]. */
static odin3_status check_scopes(o3_reader *rd) {
    const odin3_project *proj = rd->proj;
    odin3_bytes top_mod = top_module(rd);
    for (size_t i = 0; i < rd->params.len; i++) {
        size_t idx = *(const size_t *)odin3_vec_cat(&rd->params, i);
        const odin3_proj_param *param = odin3_vec_cat(&proj->params, idx);
        odin3_bytes scope = odin3_proj_str_bytes(rd->proj, param->scope);
        size_t root = odin3_proj_ident_len(scope, 0); /* a scope is IDENT(.IDENT)* */
        if (proj->top != 0 && root == top_mod.len && memcmp(scope.ptr, top_mod.ptr, root) == 0) {
            continue;
        }
        const char *top = "none declared";
        if (proj->top != 0) {
            top = odin3_strtab_get(odin3_design_strtab(proj->design), proj->top);
        }
        char detail[DETAIL_MAX];
        (void)snprintf(detail, sizeof detail, "param scope must start at the top (%s)", top);
        return fail(rd, param->at, ODIN3_PROJ_ERR_SYNTAX, detail, &scope);
    }
    return ODIN3_OK;
}

/* --- architecture, inventory, overrides ---------------------------------------------------- */

static odin3_status key_arch(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_arch arch = {.kind = 0};
    if (!odin3_arch_kind_of(st->args[0].text, &arch.kind)) {
        return fail(rd, st->at, ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "not an architecture",
                    &st->args[0].text);
    }
    odin3_status res = arg_path(rd, &st->args[0], ODIN3_PROJ_WANT_FILE, &arch.path);
    return res != ODIN3_OK ? res : odin3_project_add_arch(rd->proj, &arch);
}

static odin3_status key_device(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_arch arch = {.kind = ODIN3_PROJ_ARCH_DEVICE, .has_family = true};
    odin3_status res = intern(rd, st->args[0].text, &arch.family);
    if (res == ODIN3_OK && st->nargs == 2) {
        arch.has_device = true;
        res = intern(rd, st->args[1].text, &arch.device);
    }
    return res != ODIN3_OK ? res : odin3_project_add_arch(rd->proj, &arch);
}

static odin3_status key_available(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_inventory inv = {.at = st->at};
    odin3_status res = odin3_project_count(rd->proj, st->args[1].text, st->at, &inv.count);
    res = res != ODIN3_OK ? res : intern(rd, st->args[0].text, &inv.cell);
    return res != ODIN3_OK ? res : odin3_project_add_inventory(rd->proj, &inv);
}

static odin3_status key_hide(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_override ov = {.kind = ODIN3_PROJ_OVERRIDE_HIDE, .at = st->at};
    odin3_status res = intern(rd, st->args[0].text, &ov.name);
    return res != ODIN3_OK ? res : odin3_project_add_override(rd->proj, &ov, (odin3_proj_kvs){0});
}

/* The key of a `k=v` word (its length; *eq = whether it has an '='). */
static size_t param_key(odin3_bytes word, bool *eq) {
    const char *mark = word.len > 0 ? memchr(word.ptr, '=', word.len) : NULL;
    *eq = mark != NULL;
    return mark != NULL ? (size_t)(mark - (const char *)word.ptr) : word.len;
}

/* Whether the key of argument idx is the key of an earlier parameter (from argument 4). */
static bool key_seen(const o3_stmt *st, uint32_t idx, size_t len) {
    bool eq = false;
    for (uint32_t j = 4; j < idx; j++) {
        odin3_bytes prev = st->args[j].text;
        if (param_key(prev, &eq) == len && memcmp(prev.ptr, st->args[idx].text.ptr, len) == 0) {
            return true;
        }
    }
    return false;
}

/* The cell parameters (arguments 4…): each needs an '=', a non-empty key and a new key. */
static odin3_status cell_params(o3_reader *rd, const o3_stmt *st) {
    odin3_vec_clear(&rd->kvs);
    for (uint32_t i = 4; i < st->nargs; i++) {
        bool eq = false;
        size_t len = param_key(st->args[i].text, &eq);
        if (!eq || len == 0 || key_seen(st, i, len)) {
            return syntax(rd, st, "bad parameter", &st->args[i].text);
        }
    }
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 4; res == ODIN3_OK && i < st->nargs; i++) {
        odin3_bytes word = st->args[i].text;
        bool eq = false;
        size_t len = param_key(word, &eq);
        const char *text = word.ptr;
        odin3_proj_kv kv = {0, 0};
        res = intern(rd, (odin3_bytes){text, len}, &kv.key);
        res = res != ODIN3_OK
                  ? res
                  : intern(rd, (odin3_bytes){text + len + 1, word.len - len - 1}, &kv.value);
        odin3_proj_kv *slot = res == ODIN3_OK ? odin3_vec_push(&rd->kvs) : NULL;
        res = res != ODIN3_OK || slot != NULL ? res : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = kv;
        }
    }
    return res;
}

/* cell <name> from <libcell> [param <k>=<v>…] */
static odin3_status key_cell(o3_reader *rd, const o3_stmt *st) {
    if (!arg_is(st, 1, "from") || (st->nargs > 3 && (!arg_is(st, 3, "param") || st->nargs == 4))) {
        return usage(rd, st, NULL);
    }
    odin3_proj_override ov = {.kind = ODIN3_PROJ_OVERRIDE_CELL, .at = st->at};
    odin3_status res = cell_params(rd, st);
    res = res != ODIN3_OK ? res : intern(rd, st->args[0].text, &ov.name);
    res = res != ODIN3_OK ? res : intern(rd, st->args[2].text, &ov.from);
    odin3_proj_kvs kvs = {rd->kvs.data, (uint32_t)rd->kvs.len};
    return res != ODIN3_OK ? res : odin3_project_add_override(rd->proj, &ov, kvs);
}

/* --- rules --------------------------------------------------------------------------------- */

static const char *const k_thresholds[] = {"min_width", "max_width", "min_depth"};
static const uint8_t k_threshold_bits[] = {ODIN3_PROJ_HAS_MIN_WIDTH, ODIN3_PROJ_HAS_MAX_WIDTH,
                                           ODIN3_PROJ_HAS_MIN_DEPTH};

/* The index of a threshold word in k_thresholds, -1 when it is none. */
static int threshold_of(odin3_bytes word) {
    for (int k = 0; k < (int)(sizeof k_thresholds / sizeof k_thresholds[0]); k++) {
        if (odin3_proj_bytes_is(word, k_thresholds[k])) {
            return k;
        }
    }
    return -1;
}

/* text without the leading and trailing characters Python's str.strip() removes. */
static odin3_bytes py_strip(odin3_bytes text) {
    size_t start = 0;
    for (size_t len = odin3_py_space_len(text, 0); len > 0; len = odin3_py_space_len(text, start)) {
        start += len;
    }
    size_t end = start;
    for (size_t k = start; k < text.len;) {
        size_t len = odin3_py_space_len(text, k);
        k += len > 0 ? len : 1;
        end = len > 0 ? end : k;
    }
    return (odin3_bytes){(const char *)text.ptr + start, end - start};
}

/* The next ','-separated part of text at or after *pos, stripped; false after the last one (a
 * text without ',' is one part, as Python's split). */
static bool next_cell(odin3_bytes text, size_t *pos, odin3_bytes *cell) {
    if (*pos > text.len) {
        return false;
    }
    const char *bytes = text.ptr;
    size_t j = *pos;
    while (j < text.len && bytes[j] != ',') {
        j++;
    }
    *cell = py_strip((odin3_bytes){bytes + *pos, j - *pos});
    *pos = j + 1;
    return true;
}

/* Arguments 3 … end-1 joined by one space, in rd->scratch (a view; "" when empty). */
static odin3_status join_cells(o3_reader *rd, const o3_stmt *st, uint32_t end, odin3_bytes *out) {
    odin3_strbuf_clear(&rd->scratch);
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 3; res == ODIN3_OK && i < end; i++) {
        res = i > 3 ? odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(" ")) : ODIN3_OK;
        res = res != ODIN3_OK ? res : odin3_strbuf_append(&rd->scratch, st->args[i].text);
    }
    *out = rd->scratch.len > 0 ? (odin3_bytes){rd->scratch.data, rd->scratch.len}
                               : (odin3_bytes){"", 0};
    return res;
}

/* Whether every part of text is a cell: not empty, no space. */
static bool cells_ok(odin3_bytes text) {
    odin3_bytes cell = {NULL, 0};
    for (size_t pos = 0; next_cell(text, &pos, &cell);) {
        if (cell.len == 0 || memchr(cell.ptr, ' ', cell.len) != NULL) {
            return false;
        }
    }
    return true;
}

/* The map cells: arguments 3 … end-1 joined by one space, split at ',', each stripped, none
 * empty or holding a space; interned into rd->ids. */
static odin3_status map_cells(o3_reader *rd, const o3_stmt *st, uint32_t end) {
    odin3_vec_clear(&rd->ids);
    odin3_bytes text = {NULL, 0};
    odin3_status res = join_cells(rd, st, end, &text);
    if (res == ODIN3_OK && !cells_ok(text)) {
        return syntax(rd, st, "expected <cell>[, <cell>...] after 'to'", NULL);
    }
    odin3_bytes cell = {NULL, 0};
    for (size_t pos = 0; res == ODIN3_OK && next_cell(text, &pos, &cell);) {
        uint32_t *slot = odin3_vec_push(&rd->ids);
        res = slot != NULL ? intern(rd, cell, slot) : ODIN3_ERR_NO_MEMORY;
        if (res != ODIN3_OK && slot != NULL) {
            odin3_vec_pop(&rd->ids);
        }
    }
    return res;
}

/* The `<threshold> <n>` pairs of a map from argument rest: each threshold once. */
static odin3_status map_thresholds(o3_reader *rd, const o3_stmt *st, uint32_t rest,
                                   odin3_proj_rule *rule) {
    if ((st->nargs - rest) % 2 != 0) {
        return usage(rd, st, NULL);
    }
    uint32_t *values[] = {&rule->u.map.min_width, &rule->u.map.max_width, &rule->u.map.min_depth};
    for (uint32_t i = rest; i < st->nargs; i += 2) {
        int k = threshold_of(st->args[i].text);
        if (k < 0 || (rule->u.map.has & k_threshold_bits[k]) != 0) {
            return syntax(rd, st, "bad or repeated threshold", &st->args[i].text);
        }
        odin3_status res = odin3_project_count(rd->proj, st->args[i + 1].text, st->at, values[k]);
        if (res != ODIN3_OK) {
            return res;
        }
        rule->u.map.has = (uint8_t)(rule->u.map.has | k_threshold_bits[k]);
    }
    return ODIN3_OK;
}

/* map <scope> <subject> (to <cells…> | soft | keep) [<threshold> <n>]… */
static odin3_status key_map(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_MAP, .at = st->at};
    uint32_t rest = 3;
    odin3_status res = ODIN3_OK;
    odin3_vec_clear(&rd->ids);
    if (arg_is(st, 2, "to")) {
        while (rest < st->nargs && threshold_of(st->args[rest].text) < 0) {
            rest++;
        }
        rule.u.map.action = ODIN3_PROJ_ACTION_TO;
        res = map_cells(rd, st, rest);
    } else if (arg_is(st, 2, "soft") || arg_is(st, 2, "keep")) {
        rule.u.map.action = arg_is(st, 2, "soft") ? ODIN3_PROJ_ACTION_SOFT : ODIN3_PROJ_ACTION_KEEP;
    } else {
        return usage(rd, st, &st->args[2].text);
    }
    res = res != ODIN3_OK ? res : map_thresholds(rd, st, rest, &rule);
    res = res != ODIN3_OK ? res : intern(rd, st->args[0].text, &rule.u.map.scope);
    res = res != ODIN3_OK ? res : intern(rd, st->args[1].text, &rule.u.map.subject);
    odin3_proj_ids cells = {rd->ids.data, (uint32_t)rd->ids.len};
    return res != ODIN3_OK ? res : odin3_project_add_rule(rd->proj, &rule, cells);
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

/* The depth after `depth`: min, max or digits (kept as written); NONE when it is none of them. */
static odin3_proj_depth depth_of(odin3_bytes depth) {
    if (odin3_proj_bytes_is(depth, "min")) {
        return ODIN3_PROJ_DEPTH_MIN;
    }
    if (odin3_proj_bytes_is(depth, "max")) {
        return ODIN3_PROJ_DEPTH_MAX;
    }
    return is_digits(depth) ? ODIN3_PROJ_DEPTH_N : ODIN3_PROJ_DEPTH_NONE;
}

/* split <subject> [width] [depth min|max|<n>]: at least one of them; the digits kept as written. */
static odin3_status key_split(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_SPLIT, .at = st->at};
    rule.u.split.width = arg_is(st, 1, "width");
    uint32_t i = rule.u.split.width ? 2 : 1;
    if (i < st->nargs) {
        bool pair = st->nargs - i == 2 && arg_is(st, i, "depth");
        rule.u.split.depth_kind =
            (uint8_t)(pair ? depth_of(st->args[i + 1].text) : ODIN3_PROJ_DEPTH_NONE);
        if (rule.u.split.depth_kind == ODIN3_PROJ_DEPTH_NONE) {
            return usage(rd, st, NULL);
        }
    }
    if (!rule.u.split.width && rule.u.split.depth_kind == ODIN3_PROJ_DEPTH_NONE) {
        return usage(rd, st, NULL);
    }
    odin3_status res = intern(rd, st->args[0].text, &rule.u.split.subject);
    if (res == ODIN3_OK && rule.u.split.depth_kind == ODIN3_PROJ_DEPTH_N) {
        res = intern(rd, st->args[st->nargs - 1].text, &rule.u.split.depth);
    }
    return res != ODIN3_OK ? res : odin3_project_add_rule(rd->proj, &rule, (odin3_proj_ids){0});
}

static odin3_status key_limit(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_LIMIT, .at = st->at};
    odin3_status res = odin3_project_count(rd->proj, st->args[1].text, st->at, &rule.u.limit.count);
    res = res != ODIN3_OK ? res : intern(rd, st->args[0].text, &rule.u.limit.cell);
    return res != ODIN3_OK ? res : odin3_project_add_rule(rd->proj, &rule, (odin3_proj_ids){0});
}

static odin3_status key_patterns(o3_reader *rd, const o3_stmt *st) {
    odin3_proj_rule rule = {.kind = ODIN3_PROJ_RULE_PATTERNS, .at = st->at};
    odin3_status res = arg_path(rd, &st->args[0], ODIN3_PROJ_WANT_FILE, &rule.u.patterns.path);
    return res != ODIN3_OK ? res : odin3_project_add_rule(rd->proj, &rule, (odin3_proj_ids){0});
}

/* --- flow, import -------------------------------------------------------------------------- */

static odin3_status key_flow(o3_reader *rd, const o3_stmt *st) {
    if (rd->proj->flow.given != 0) {
        odin3_bytes flow = odin3_bytes_cstr("flow");
        return syntax(rd, st, "flow given twice", &flow);
    }
    odin3_proj_path flow = {0, 0, {0}};
    odin3_status res = arg_path(rd, &st->args[0], ODIN3_PROJ_WANT_FILE, &flow);
    if (res == ODIN3_OK) {
        rd->proj->flow = flow;
    }
    return res;
}

/*
 * import <file.qpf|file.qsf> [revision <r>]: the suffix of the normalized path, case-sensitive
 * (`unknown_file_type`, names [word]), then the file must exist; the Quartus reader runs with the
 * import word as its files' parent and its revision errors' location.
 */
static odin3_status key_import(o3_reader *rd, const o3_stmt *st) {
    if (st->nargs == 2 || (st->nargs == 3 && !arg_is(st, 1, "revision"))) {
        return usage(rd, st, NULL);
    }
    odin3_bytes given = st->args[0].text;
    odin3_status res = odin3_path_resolve((odin3_path_join){rd->base, given}, &rd->scratch);
    if (res != ODIN3_OK) {
        return res;
    }
    odin3_bytes suffix = odin3_path_suffix((odin3_bytes){rd->scratch.data, rd->scratch.len});
    if (!odin3_proj_bytes_is(suffix, ".qpf") && !odin3_proj_bytes_is(suffix, ".qsf")) {
        return fail(rd, st->at, ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "import takes a .qpf or .qsf",
                    &given);
    }
    odin3_proj_path file = {0, 0, {0}};
    res = arg_path(rd, &st->args[0], ODIN3_PROJ_WANT_FILE, &file);
    odin3_quartus_req req = {{"", 0}, st->nargs == 3, st->at};
    if (req.has_revision) {
        req.revision = st->args[2].text;
    }
    return res != ODIN3_OK ? res : odin3_read_quartus(rd->proj, &file, &req);
}

/* --- statements ---------------------------------------------------------------------------- */

static const o3_key k_keys[] = {
    {"file", 2, 4, "file <language> <path> [library <name>]", key_file},
    {"libfile", 2, 2, "libfile <language> <path>", key_libfile},
    {"libdir", 1, 1, "libdir <dir>", key_libdir},
    {"libext", 1, MAX_ARGS, "libext <ext>...", key_libext},
    {"incdir", 1, 1, "incdir <dir>", key_incdir},
    {"define", 1, 1, "define <name>[=<value>]", key_define},
    {"top", 1, 1, "top [<library>.]<module>", key_top},
    {"param", 2, 2, "param <top>[.<instance>...].<name> <value>", key_param},
    {"arch", 1, 1, "arch <file.o3lib|file.xml>", key_arch},
    {"device", 1, 2, "device <family> [<part>]", key_device},
    {"map", 3, MAX_ARGS,
     "map <scope> <subject> to <cell>[, <cell>...] | soft | keep [<threshold> <n>]...", key_map},
    {"split", 2, 4, "split <subject> [width] [depth min|max|<n>]", key_split},
    {"limit", 2, 2, "limit <libcell> <count>", key_limit},
    {"patterns", 1, 1, "patterns <file.o3lib>", key_patterns},
    {"available", 2, 2, "available <libcell> <count>", key_available},
    {"hide", 1, 1, "hide <libcell>", key_hide},
    {"cell", 3, MAX_ARGS, "cell <name> from <libcell> [param <name>=<value>...]", key_cell},
    {"flow", 1, 1, "flow <script.o3>", key_flow},
    {"import", 1, 3, "import <file.qpf|file.qsf> [revision <name>]", key_import},
};

/* The words of the line just split: the bare first word is the key (else `unknown_key`), the
 * arity is checked, then the key's handler runs. */
static odin3_status statement(o3_reader *rd) {
    const odin3_word *words = rd->words.data;
    odin3_loc at = odin3_srcman_loc(rd->sm, rd->text.buffer, words[0].off);
    const o3_key *key = NULL;
    for (size_t k = 0; !words[0].quoted && key == NULL && k < sizeof k_keys / sizeof k_keys[0];
         k++) {
        key = odin3_proj_bytes_is(words[0].text, k_keys[k].name) ? &k_keys[k] : NULL;
    }
    if (key == NULL) {
        return fail(rd, at, ODIN3_PROJ_ERR_UNKNOWN_KEY, "unknown key", &words[0].text);
    }
    o3_stmt st = {words + 1, (uint32_t)rd->words.len - 1, at, key};
    if (st.nargs < key->lo || st.nargs > key->hi) {
        return usage(rd, &st, NULL);
    }
    return key->handler(rd, &st);
}

static odin3_status read_lines(o3_reader *rd) {
    odin3_status res = ODIN3_OK;
    for (uint32_t i = 0; res == ODIN3_OK && i < rd->text.lines.len; i++) {
        uint32_t offset = 0;
        odin3_bytes line = odin3_proj_text_line(&rd->text, i, &offset);
        odin3_words_opts opts = {offset, ODIN3_WORDS_O3PROJ, rd->proj->arena};
        odin3_words_err err = {0, {NULL, 0}, NULL};
        odin3_vec_clear(&rd->words);
        res = odin3_words_split(line, &opts, &rd->words, &err);
        if (res == ODIN3_ERR_PARSE) { /* the oracle names nothing for these */
            odin3_loc at = odin3_srcman_loc(rd->sm, rd->text.buffer, err.off);
            return fail(rd, at, ODIN3_PROJ_ERR_SYNTAX, err.what, NULL);
        }
        if (res == ODIN3_OK && rd->words.len > 0) {
            res = statement(rd);
        }
    }
    return res;
}

odin3_status odin3_read_o3proj(odin3_project *proj, const odin3_proj_path *file) {
    o3_reader rd = {.proj = proj};
    odin3_vec_init(&rd.text.lines, sizeof(uint32_t));
    odin3_vec_init(&rd.words, sizeof(odin3_word));
    odin3_vec_init(&rd.params, sizeof(size_t));
    odin3_vec_init(&rd.ids, sizeof(uint32_t));
    odin3_vec_init(&rd.kvs, sizeof(odin3_proj_kv));
    odin3_strbuf_init(&rd.scratch);
    odin3_status res = odin3_design_get_srcman(proj->design, &rd.sm);
    res = res != ODIN3_OK ? res : intern(&rd, odin3_bytes_cstr("work"), &rd.work);
    if (res == ODIN3_OK) {
        rd.base = odin3_path_dir(odin3_proj_str_bytes(proj, file->resolved));
        res = odin3_project_load(proj, file, (odin3_loc){0}, &rd.text);
    }
    res = res != ODIN3_OK ? res : read_lines(&rd);
    res = res != ODIN3_OK ? res : check_scopes(&rd);
    odin3_proj_text_free(&rd.text);
    odin3_vec_free(&rd.words);
    odin3_vec_free(&rd.params);
    odin3_vec_free(&rd.ids);
    odin3_vec_free(&rd.kvs);
    odin3_strbuf_free(&rd.scratch);
    return res;
}
