/* reader.c — BLIF reader: pass 1 (model headers, ports, black boxes) and pass 2 (bodies). */
#include "frontends/blif/reader.h"

#include "frontends/blif/lexer.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Header list kinds; IN and OUT double as the bit that separates their duplicate-check keys. */
typedef enum blif_kind { BLIF_IN = 0, BLIF_OUT = 1, BLIF_CLOCK = 2 } blif_kind;

enum {
    MAX_INDEX_DIGITS = 9, /* a bit index of at most 9 digits fits a port width (< 2^31) */
    DECIMAL = 10,
    MODEL_KEY_SHIFT = 33, /* port key: model << 33 | kind << 32 | name */
    KIND_KEY_SHIFT = 32,
    NAME_COL = 2, /* the name of `.model <name>` is token 1, column 2 */
};

/* One name of a header list (.inputs, .outputs, .clock): 16 bytes, names interned. */
typedef struct blif_name {
    uint32_t str;
    uint32_t line;
    uint32_t col;
    uint32_t kind; /* blif_kind */
} blif_name;

/* One `.model`: its name, line, header names [first, first + count) and black-box flag. */
typedef struct blif_model {
    uint32_t index; /* position in file order, from 0 */
    uint32_t name;
    uint32_t line;
    uint32_t first, count;
    bool blackbox;
} blif_model;

/* A port made from one or more consecutive header names (IR-7b grouping). */
typedef struct blif_group {
    const blif_name *bits;
    uint32_t width;
    uint32_t name; /* the base `a` of a vector, else the scalar's own name */
    bool scalar;
} blif_group;

typedef struct blif_reader {
    odin3_design *design;
    const char *path;
    uint32_t file; /* strtab ID of path */
    odin3_pass_ctx ctx;
    odin3_vec models;          /* blif_model, file order */
    odin3_vec names;           /* blif_name, every header list of every model, file order */
    odin3_u64map *model_lines; /* model name -> line of its .model */
    odin3_u64map *port_lines;  /* port key -> line of its first declaration */
    odin3_u64map *base_bits;   /* port key of a base `a` -> number of names `a[k]` in that list */
    uint32_t open;             /* 1-based index of the model whose .end is pending, 0 = none */
    uint32_t line;             /* the line being read or built, for out-of-memory messages */
    bool oom_logged;
    odin3_strbuf scratch; /* candidate wire names */
    odin3_strbuf clocks;  /* the open module's .clock names, space-separated */
    odin3_vec defs;       /* odin3_port_def, black-box ports */
} blif_reader;

/* --- errors ------------------------------------------------------------------------------ */

static odin3_status rd_error(const blif_reader *rd, uint32_t line, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/* Logs "path:line: message" and returns ODIN3_ERR_PARSE. */
static odin3_status rd_error(const blif_reader *rd, uint32_t line, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s", rd->path, (unsigned)line, msg);
    return ODIN3_ERR_PARSE;
}

/* Out of memory is logged once, with the line being read or built; any status passes through. */
static odin3_status rd_fail(blif_reader *rd, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY && !rd->oom_logged) {
        rd->oom_logged = true;
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: out of memory", rd->path, (unsigned)rd->line);
    }
    return st;
}

static const char *rd_str(const blif_reader *rd, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(rd->design), str);
}

static blif_model *rd_model(blif_reader *rd, uint32_t index) {
    return odin3_vec_at(&rd->models, index);
}

static const blif_name *rd_name(const blif_reader *rd, uint32_t index) {
    return odin3_vec_cat(&rd->names, index);
}

static bool tok_is(const odin3_bytes *tok, const char *word) {
    return tok->len == strlen(word) && memcmp(tok->ptr, word, tok->len) == 0;
}

/* --- pass 1a: scan the file, collect model headers, check the structure ------------------ */

/* True for `base[k]` with k canonical decimal (no leading zero) of at most 9 digits. */
static bool parse_bit(const char *name, size_t len, size_t *base_len, uint32_t *index) {
    if (len < 4 || name[len - 1] != ']') { /* shortest is "a[0]" */
        return false;
    }
    size_t open = len - 2;
    while (open > 0 && name[open] >= '0' && name[open] <= '9') {
        open--;
    }
    size_t digits = len - 2 - open;
    if (open == 0 || name[open] != '[' || digits == 0 || digits > MAX_INDEX_DIGITS ||
        (digits > 1 && name[open + 1] == '0')) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = open + 1; i < len - 1; i++) {
        value = value * DECIMAL + (uint32_t)(name[i] - '0');
    }
    *base_len = open;
    *index = value;
    return true;
}

static odin3_status scan_model(blif_reader *rd, const odin3_blif_line *ln) {
    if (rd->open != 0) {
        const blif_model *open = rd_model(rd, rd->open - 1);
        return rd_error(rd, open->line, "model '%s' has no .end", rd_str(rd, open->name));
    }
    if (ln->count != 2) {
        return rd_error(rd, ln->line, ".model takes exactly one name");
    }
    blif_model model = {
        .index = (uint32_t)rd->models.len, .line = ln->line, .first = (uint32_t)rd->names.len};
    odin3_status st = odin3_design_intern(rd->design, ln->tokens[1], &model.name);
    if (st != ODIN3_OK) {
        return rd_fail(rd, st);
    }
    uint64_t first = 0;
    if (odin3_u64map_get(rd->model_lines, model.name, &first)) {
        return rd_error(rd, ln->line, "duplicate model '%s' (first at line %u)",
                        rd_str(rd, model.name), (unsigned)first);
    }
    blif_model *slot = odin3_vec_push(&rd->models);
    if (slot == NULL ||
        odin3_u64map_put(rd->model_lines, (odin3_kv){model.name, ln->line}) != ODIN3_OK) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *slot = model;
    rd->open = (uint32_t)rd->models.len;
    return ODIN3_OK;
}

/* Key of a name (or a vector base) in one list kind (BLIF_IN or BLIF_OUT) of model `index`. */
static uint64_t port_key(uint32_t index, uint32_t kind, uint32_t str) {
    return ((uint64_t)index << MODEL_KEY_SHIFT) | ((uint64_t)kind << KIND_KEY_SHIFT) | str;
}

/* Records a port name, refusing one already in the same list kind of the model. */
static odin3_status scan_port_name(blif_reader *rd, const blif_name *name) {
    uint64_t key = port_key(rd->open - 1, name->kind, name->str);
    uint64_t first = 0;
    if (odin3_u64map_get(rd->port_lines, key, &first)) {
        return rd_error(rd, name->line, "port '%s' declared twice (first at line %u)",
                        rd_str(rd, name->str), (unsigned)first);
    }
    if (odin3_u64map_put(rd->port_lines, (odin3_kv){key, name->line}) != ODIN3_OK) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    return ODIN3_OK;
}

/* Counts the bit names `a[k]` of each base `a` per model and list kind (grouping needs it). */
static odin3_status count_bit(blif_reader *rd, const blif_name *name) {
    const odin3_strtab *tab = odin3_design_strtab(rd->design);
    const char *text = odin3_strtab_get(tab, name->str);
    size_t base_len = 0;
    uint32_t index = 0;
    if (!parse_bit(text, odin3_strtab_len(tab, name->str), &base_len, &index)) {
        return ODIN3_OK;
    }
    uint32_t base = 0;
    odin3_status st = odin3_design_intern(rd->design, (odin3_bytes){text, base_len}, &base);
    uint64_t key = port_key(rd->open - 1, name->kind, base);
    uint64_t count = 0;
    (void)odin3_u64map_get(rd->base_bits, key, &count);
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(rd->base_bits, (odin3_kv){key, count + 1});
    }
    return rd_fail(rd, st);
}

static odin3_status scan_list(blif_reader *rd, const odin3_blif_line *ln, blif_kind kind) {
    for (uint32_t i = 1; i < ln->count; i++) {
        blif_name name = {.line = ln->line, .col = i + 1, .kind = kind};
        odin3_status st = odin3_design_intern(rd->design, ln->tokens[i], &name.str);
        if (st != ODIN3_OK) {
            return rd_fail(rd, st);
        }
        if (kind != BLIF_CLOCK) {
            st = scan_port_name(rd, &name);
        }
        if (kind != BLIF_CLOCK && st == ODIN3_OK) {
            st = count_bit(rd, &name);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        blif_name *slot = odin3_vec_push(&rd->names);
        if (slot == NULL) {
            return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        *slot = name;
        rd_model(rd, rd->open - 1)->count++;
    }
    return ODIN3_OK;
}

/* Header directives of the open model (.inputs, .outputs, .clock, .blackbox, .end); body
 * lines are left to pass 2. */
static odin3_status scan_header(blif_reader *rd, const odin3_blif_line *ln) {
    const odin3_bytes *tok = &ln->tokens[0];
    if (tok_is(tok, ".inputs")) {
        return scan_list(rd, ln, BLIF_IN);
    }
    if (tok_is(tok, ".outputs")) {
        return scan_list(rd, ln, BLIF_OUT);
    }
    if (tok_is(tok, ".clock")) {
        return scan_list(rd, ln, BLIF_CLOCK);
    }
    if (tok_is(tok, ".blackbox")) {
        rd_model(rd, rd->open - 1)->blackbox = true;
        return ln->count == 1 ? ODIN3_OK : rd_error(rd, ln->line, ".blackbox takes no arguments");
    }
    if (tok_is(tok, ".end")) {
        rd->open = 0;
        return ln->count == 1 ? ODIN3_OK : rd_error(rd, ln->line, ".end takes no arguments");
    }
    return ODIN3_OK;
}

static odin3_status scan_line(blif_reader *rd, const odin3_blif_line *ln) {
    rd->line = ln->line;
    if (tok_is(&ln->tokens[0], ".model")) {
        return scan_model(rd, ln);
    }
    if (rd->open == 0) {
        return rd_error(rd, ln->line, "'%s' outside a .model", (const char *)ln->tokens[0].ptr);
    }
    return scan_header(rd, ln);
}

static odin3_status scan_file(blif_reader *rd) {
    odin3_blif_lexer *lx = odin3_blif_lexer_open(rd->path);
    if (lx == NULL) {
        return ODIN3_ERR_IO;
    }
    odin3_blif_line ln;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && odin3_blif_lexer_next(lx, &ln)) {
        st = scan_line(rd, &ln);
    }
    if (st == ODIN3_OK) {
        st = odin3_blif_lexer_status(lx);
    }
    odin3_blif_lexer_close(lx);
    if (st == ODIN3_OK && rd->open != 0) {
        const blif_model *open = rd_model(rd, rd->open - 1);
        st = rd_error(rd, open->line, "model '%s' has no .end", rd_str(rd, open->name));
    }
    return st;
}

/* --- pass 1b: build modules, ports and black boxes ------------------------------------------ */

/* The IMPORTED record for tokens [col, end_col] of one line of the file. */
static odin3_status rd_prov(blif_reader *rd, const odin3_srcloc *where, odin3_prov_id *out) {
    odin3_srcloc loc = *where;
    loc.file = rd->file;
    loc.end_line = loc.line;
    odin3_prov_origin origin = {.locs = &loc, .n_locs = 1};
    return rd_fail(rd, odin3_prov_imported(&rd->ctx, &origin, out));
}

/* True when cand continues first's run as bit `index` (same list, same directive, same base). */
static bool next_bit(const odin3_strtab *tab, const blif_name *first, const blif_name *cand,
                     size_t base_len, uint32_t index) {
    size_t cand_base = 0;
    uint32_t got = 0;
    const char *text = odin3_strtab_get(tab, cand->str);
    return cand->kind == first->kind && cand->line == first->line &&
           parse_bit(text, odin3_strtab_len(tab, cand->str), &cand_base, &got) && got == index &&
           cand_base == base_len && memcmp(text, odin3_strtab_get(tab, first->str), base_len) == 0;
}

/*
 * The port that starts at header name `at` of model (IR-7b): the run a[0] a[1] … a[w-1]
 * of one directive is a vector when its list has no other bit of `a` (so `a[0] b a[1]` stays
 * three scalars); everything else is a scalar. Never reorders.
 */
static odin3_status next_group(blif_reader *rd, const blif_model *model, uint32_t at,
                               blif_group *group) {
    const odin3_strtab *tab = odin3_design_strtab(rd->design);
    const blif_name *first = rd_name(rd, model->first + at);
    *group = (blif_group){.bits = first, .width = 1, .name = first->str, .scalar = true};
    const char *text = odin3_strtab_get(tab, first->str);
    size_t base_len = 0;
    uint32_t bit = 0;
    if (first->kind == BLIF_CLOCK ||
        !parse_bit(text, odin3_strtab_len(tab, first->str), &base_len, &bit) || bit != 0) {
        return ODIN3_OK;
    }
    uint32_t width = 1;
    while (at + width < model->count && next_bit(tab, first, first + width, base_len, width)) {
        width++;
    }
    uint32_t base = 0;
    odin3_status st = odin3_design_intern(rd->design, (odin3_bytes){text, base_len}, &base);
    uint64_t count = 0;
    if (st == ODIN3_OK &&
        odin3_u64map_get(rd->base_bits, port_key(model->index, first->kind, base), &count) &&
        count == width) {
        *group = (blif_group){.bits = first, .width = width, .name = base, .scalar = false};
    }
    return rd_fail(rd, st);
}

/* name if no wire of module has it, else the first free `<name>$blif_port`, `…2`, … */
static odin3_status port_wire_name(blif_reader *rd, const odin3_module *module, uint32_t name,
                                   uint32_t *out) {
    *out = name;
    for (uint32_t suffix = 1; odin3_wire_valid(odin3_module_find_wire(module, *out)); suffix++) {
        odin3_strbuf_clear(&rd->scratch);
        odin3_status st = odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(rd_str(rd, name)));
        if (st == ODIN3_OK) {
            st = suffix == 1 ? odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr("$blif_port"))
                             : odin3_strbuf_appendf(&rd->scratch, "$blif_port%u", (unsigned)suffix);
        }
        if (st == ODIN3_OK) {
            st = odin3_design_intern(rd->design, (odin3_bytes){rd->scratch.data, rd->scratch.len},
                                     out);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Sets a STRING attribute key of obj to value. */
static odin3_status set_string_attr(blif_reader *rd, odin3_module *module, odin3_objref obj,
                                    const char *key, odin3_bytes value) {
    uint32_t key_str = 0;
    odin3_value val = {.kind = ODIN3_VAL_STRING};
    odin3_status st = odin3_design_intern(rd->design, odin3_bytes_cstr(key), &key_str);
    if (st == ODIN3_OK) {
        st = odin3_design_intern(rd->design, value, &val.str);
    }
    if (st == ODIN3_OK) {
        st = odin3_attr_set(module, obj, key_str, &val);
    }
    return st;
}

/* Names each port net by its BLIF bit name; a name already on a net (in and out) shares it. */
static odin3_status name_port_nets(odin3_module *module, odin3_wire_id wire,
                                   const blif_group *group) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < group->width; k++) {
        odin3_net_id net = odin3_wire_net(module, wire, k);
        odin3_net_id have = odin3_module_find_net(module, group->bits[k].str);
        st = odin3_net_valid(have) ? odin3_net_merge(module, (odin3_net_pair){have, net})
                                   : odin3_net_rename(module, net, group->bits[k].str);
    }
    return st;
}

static odin3_status add_port(blif_reader *rd, odin3_module *module, const blif_group *group) {
    const blif_name *first = group->bits;
    rd->line = first->line;
    odin3_port_spec spec = {.dir = first->kind == BLIF_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT,
                            .width = group->width,
                            .scalar = group->scalar};
    odin3_srcloc where = {
        .line = first->line, .col = first->col, .end_col = group->bits[group->width - 1].col};
    odin3_status st = rd_prov(rd, &where, &spec.prov);
    if (st == ODIN3_OK) {
        st = port_wire_name(rd, module, group->name, &spec.name);
    }
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = odin3_module_add_port(module, &spec, &node);
    }
    odin3_wire_id wire = odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
    if (st == ODIN3_OK && spec.name != group->name) {
        st = set_string_attr(rd, module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                             ODIN3_BLIF_ATTR_PORT_NAME, odin3_bytes_cstr(rd_str(rd, group->name)));
    }
    if (st == ODIN3_OK) {
        st = name_port_nets(module, wire, group);
    }
    return rd_fail(rd, st);
}

/* Appends a .clock name to the space-separated list in rd->clocks. */
static odin3_status add_clock(blif_reader *rd, const blif_name *name) {
    rd->line = name->line;
    odin3_status st = ODIN3_OK;
    if (rd->clocks.len > 0) {
        st = odin3_strbuf_append(&rd->clocks, odin3_bytes_cstr(" "));
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(&rd->clocks, odin3_bytes_cstr(rd_str(rd, name->str)));
    }
    return rd_fail(rd, st);
}

static odin3_status build_module(blif_reader *rd, const blif_model *model) {
    rd->line = model->line;
    odin3_prov_id prov = {0};
    odin3_module_id id = {0};
    odin3_status st = rd_prov(
        rd, &(odin3_srcloc){.line = model->line, .col = NAME_COL, .end_col = NAME_COL}, &prov);
    if (st == ODIN3_OK) {
        st = odin3_module_create(rd->design, model->name, prov, &id);
    }
    if (st == ODIN3_ERR_INVALID_ARG) {
        return rd_error(rd, model->line, "model '%s' has the name of a cell type",
                        rd_str(rd, model->name));
    }
    odin3_module *module = odin3_module_get(rd->design, id);
    odin3_strbuf_clear(&rd->clocks);
    blif_group group = {.width = 1};
    for (uint32_t at = 0; st == ODIN3_OK && at < model->count; at += group.width) {
        st = next_group(rd, model, at, &group);
        if (st == ODIN3_OK) {
            st = group.bits->kind == BLIF_CLOCK ? add_clock(rd, group.bits)
                                                : add_port(rd, module, &group);
        }
    }
    if (st == ODIN3_OK && rd->clocks.len > 0) {
        rd->line = model->line;
        st = set_string_attr(rd, module, (odin3_objref){ODIN3_OBJ_MODULE, id.v},
                             ODIN3_BLIF_ATTR_CLOCK, (odin3_bytes){rd->clocks.data, rd->clocks.len});
    }
    return rd_fail(rd, st);
}

/* A black-box formal must not be both an input and an output (its port names are unique). */
static odin3_status check_formal(const blif_reader *rd, const blif_model *model,
                                 const blif_name *name) {
    if (name->kind == BLIF_CLOCK) {
        return rd_error(rd, name->line, ".clock in a black box");
    }
    uint64_t key = port_key(model->index, name->kind == BLIF_IN ? BLIF_OUT : BLIF_IN, name->str);
    uint64_t line = 0;
    if (odin3_u64map_get(rd->port_lines, key, &line)) {
        return rd_error(rd, line > name->line ? (uint32_t)line : name->line,
                        "black-box port '%s' is both an input and an output",
                        rd_str(rd, name->str));
    }
    return ODIN3_OK;
}

/* Checks a black-box port's formals and appends its definition to rd->defs. */
static odin3_status add_formal(blif_reader *rd, const blif_model *model, const blif_group *group) {
    for (uint32_t k = 0; k < group->width; k++) {
        odin3_status st = check_formal(rd, model, &group->bits[k]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_port_def *port = odin3_vec_push(&rd->defs);
    if (port == NULL) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *port = (odin3_port_def){.name = rd_str(rd, group->name),
                             .dir = group->bits->kind == BLIF_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT,
                             .scalar = group->scalar,
                             .width = group->width};
    return ODIN3_OK;
}

static odin3_status build_blackbox(blif_reader *rd, const blif_model *model) {
    rd->line = model->line;
    odin3_vec_clear(&rd->defs);
    odin3_status st = ODIN3_OK;
    blif_group group = {.width = 1};
    for (uint32_t at = 0; st == ODIN3_OK && at < model->count; at += group.width) {
        st = next_group(rd, model, at, &group);
        if (st == ODIN3_OK) {
            st = add_formal(rd, model, &group);
        }
    }
    rd->line = model->line;
    odin3_celltype_def def = {.name = rd_str(rd, model->name),
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = rd->defs.data,
                              .n_ports = (uint32_t)rd->defs.len};
    if (st == ODIN3_OK) {
        st = odin3_celltype_declare_blackbox(rd->design, &def, NULL);
    }
    if (st == ODIN3_ERR_INVALID_ARG) {
        return rd_error(rd, model->line,
                        "black box '%s' conflicts with the cell type of that name or repeats a "
                        "port name",
                        def.name);
    }
    return rd_fail(rd, st);
}

/* Modules and black boxes in file order, after one pass run "read_blif" starts. */
static odin3_status build_all(blif_reader *rd) {
    uint32_t run_name = 0;
    rd->line = 1;
    odin3_status st = odin3_design_intern(rd->design, odin3_bytes_cstr("read_blif"), &run_name);
    if (st == ODIN3_OK) {
        st = odin3_pass_run_begin(rd->design, run_name, &rd->ctx);
    }
    st = rd_fail(rd, st);
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->models.len; i++) {
        const blif_model *model = rd_model(rd, i);
        st = model->blackbox ? build_blackbox(rd, model) : build_module(rd, model);
    }
    return st;
}

/* --- pass 2: model bodies ------------------------------------------------------------------ */

static bool is_header(const odin3_bytes *tok) {
    return tok_is(tok, ".inputs") || tok_is(tok, ".outputs") || tok_is(tok, ".clock") ||
           tok_is(tok, ".blackbox") || tok_is(tok, ".end");
}

static bool is_body_directive(const odin3_bytes *tok) {
    return tok_is(tok, ".names") || tok_is(tok, ".latch") || tok_is(tok, ".subckt") ||
           tok_is(tok, ".cname") || tok_is(tok, ".attr") || tok_is(tok, ".param");
}

/* One line of pass 2; *model is the 1-based index of the current model. */
static odin3_status body_line(blif_reader *rd, const odin3_blif_line *ln, uint32_t *model) {
    rd->line = ln->line;
    const odin3_bytes *tok = &ln->tokens[0];
    if (tok_is(tok, ".model")) {
        (*model)++;
        return ODIN3_OK;
    }
    if (is_header(tok)) {
        return ODIN3_OK;
    }
    const blif_model *cur = rd_model(rd, *model - 1);
    if (cur->blackbox) {
        return rd_error(rd, ln->line, "black box '%s' has a body", rd_str(rd, cur->name));
    }
    if (((const char *)tok->ptr)[0] == '.' && !is_body_directive(tok)) {
        return rd_error(rd, ln->line, "unknown directive '%s'", (const char *)tok->ptr);
    }
    return ODIN3_OK; /* cell bodies: Task 3 */
}

static odin3_status read_bodies(blif_reader *rd) {
    odin3_blif_lexer *lx = odin3_blif_lexer_open(rd->path);
    if (lx == NULL) {
        return ODIN3_ERR_IO;
    }
    odin3_blif_line ln;
    uint32_t model = 0;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && odin3_blif_lexer_next(lx, &ln)) {
        st = body_line(rd, &ln, &model);
    }
    if (st == ODIN3_OK) {
        st = odin3_blif_lexer_status(lx);
    }
    odin3_blif_lexer_close(lx);
    return st;
}

/* --- entry point --------------------------------------------------------------------------- */

static odin3_status rd_init(blif_reader *rd, odin3_design *design, const char *path) {
    *rd = (blif_reader){.design = design, .path = path};
    odin3_vec_init(&rd->models, sizeof(blif_model));
    odin3_vec_init(&rd->names, sizeof(blif_name));
    odin3_vec_init(&rd->defs, sizeof(odin3_port_def));
    odin3_strbuf_init(&rd->scratch);
    odin3_strbuf_init(&rd->clocks);
    rd->model_lines = odin3_u64map_create(0);
    rd->port_lines = odin3_u64map_create(0);
    rd->base_bits = odin3_u64map_create(0);
    if (rd->model_lines == NULL || rd->port_lines == NULL || rd->base_bits == NULL) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    return rd_fail(rd, odin3_design_intern(design, odin3_bytes_cstr(path), &rd->file));
}

static void rd_free(blif_reader *rd) {
    odin3_vec_free(&rd->models);
    odin3_vec_free(&rd->names);
    odin3_vec_free(&rd->defs);
    odin3_strbuf_free(&rd->scratch);
    odin3_strbuf_free(&rd->clocks);
    odin3_u64map_destroy(rd->model_lines);
    odin3_u64map_destroy(rd->port_lines);
    odin3_u64map_destroy(rd->base_bits);
}

odin3_status odin3_blif_read(odin3_design *design, const char *path) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "blif_read: NULL design or path");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_module_end(design) != 1) {
        odin3_log(ODIN3_LOG_ERROR, "blif_read: %s: the design already has modules", path);
        return ODIN3_ERR_INVALID_ARG;
    }
    blif_reader rd;
    odin3_status st = rd_init(&rd, design, path);
    if (st == ODIN3_OK) {
        st = scan_file(&rd);
    }
    if (st == ODIN3_OK) {
        st = build_all(&rd);
    }
    if (st == ODIN3_OK) {
        st = read_bodies(&rd);
    }
    rd_free(&rd);
    return st;
}
