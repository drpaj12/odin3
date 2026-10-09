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
    NAME_COL = 2,    /* the name of `.model <name>` is token 1, column 2 */
    TYPE_SHIFT = 32, /* formal key: cell type << 32 | formal name; value: port << 32 | bit */
    LATCH_MIN = 3,   /* .latch in out */
    LATCH_TYPED = 5, /* .latch in out type ctrl */
    LATCH_MAX = 6,   /* .latch in out type ctrl init */
    LATCH_TYPE_TOK = 3,
    LATCH_CTRL_TOK = 4,
    LATCH_INIT_DEFAULT = 3,
    EXTRA_MIN = 3, /* .attr key value */
};

/* The built-in cell types pass 2 makes (BLIF .names and .latch). */
typedef enum blif_builtin {
    BLIF_SOP,
    BLIF_DFF_P,
    BLIF_DFF_N,
    BLIF_DLATCH_P,
    BLIF_DLATCH_N,
    BLIF_FF,
    BLIF_BUILTINS
} blif_builtin;

static const char *const BUILTIN_NAMES[BLIF_BUILTINS] = {"$sop",        "$_DFF_P_",    "$_DFF_N_",
                                                         "$_DLATCH_P_", "$_DLATCH_N_", "$_FF_"};

/* `.latch` control types; `as` (asynchronous) has no IR cell and is refused. */
typedef struct blif_latch_type {
    const char *word;
    blif_builtin type;
} blif_latch_type;

static const blif_latch_type LATCH_TYPES[] = {
    {"re", BLIF_DFF_P}, {"fe", BLIF_DFF_N}, {"ah", BLIF_DLATCH_P}, {"al", BLIF_DLATCH_N}};
enum { N_LATCH_TYPES = sizeof LATCH_TYPES / sizeof LATCH_TYPES[0] };

/* $sop parameters, in definition order. */
enum { SOP_WIDTH, SOP_COVER, SOP_PARAMS };

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
    uint32_t id; /* set by pass 1b: the module ID, or the black box's cell-type ID */
    bool blackbox;
} blif_model;

/* The end of a chain of blif_use entries. */
static const uint32_t NO_USE = UINT32_MAX;

/* A formal used with a .subckt model that is not a registered cell type (pass 1); the uses of
 * one model are chained through `next` in first-use order, after an entry with formal 0 that
 * records the model's first use. */
typedef struct blif_use {
    uint32_t model;
    uint32_t formal; /* strtab ID, 0 for the model's first-use entry */
    uint32_t next;   /* index of the model's next use, NO_USE at the end */
} blif_use;

/* A port made from one or more consecutive header names (IR-7b grouping). */
typedef struct blif_group {
    const blif_name *bits;
    uint32_t width;
    uint32_t name; /* the base `a` of a vector, else the scalar's own name */
    bool scalar;
} blif_group;

/* The open `.names`: its cell is made when its rows end (at the next directive). */
typedef struct blif_sop {
    bool open;
    uint32_t width; /* input count */
    uint32_t line;
    uint32_t end_col; /* token count of the .names line */
} blif_sop;

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
    odin3_strbuf scratch;   /* candidate wire names */
    odin3_strbuf clocks;    /* the open module's .clock names, space-separated */
    odin3_vec defs;         /* odin3_port_def, black-box ports */
    odin3_vec uses;         /* blif_use: distinct (model, formal) pairs, first-use order */
    odin3_u64map *use_keys; /* model << 32 | formal -> 1, for each entry of uses */
    odin3_u64map *use_tail; /* model -> index of its last entry in uses */
    /* pass 2 */
    odin3_celltype_id builtin[BLIF_BUILTINS];
    odin3_module *module;  /* the module whose body is read; NULL in a black box */
    uint32_t model_at;     /* models met so far in pass 2 */
    odin3_node_id last;    /* the previous cell of the model (.cname, .attr, .param), or none */
    blif_sop sop;          /* the .names whose cover rows are being read */
    odin3_vec nets;        /* odin3_net_id: the pins of the cell being built, port order */
    odin3_vec ports;       /* odin3_netvec: one per port of the cell being built */
    odin3_vec dflts;       /* odin3_value: default parameters of a .subckt type */
    odin3_strbuf cover;    /* rows of the open .names */
    odin3_u64map *formals; /* formal key -> port << 32 | bit (see TYPE_SHIFT) */
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

/* Out of memory is logged once, with the line being read or built (none before the first line
 * is read); any status passes through. */
static odin3_status rd_fail(blif_reader *rd, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY && !rd->oom_logged) {
        rd->oom_logged = true;
        if (rd->line == 0) {
            odin3_log(ODIN3_LOG_ERROR, "%s: out of memory", rd->path);
        } else {
            odin3_log(ODIN3_LOG_ERROR, "%s:%u: out of memory", rd->path, (unsigned)rd->line);
        }
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

/* Records a use of model by .subckt with formal (0: the model itself), once per pair. */
static odin3_status note_use(blif_reader *rd, uint32_t model, uint32_t formal) {
    blif_use use = {.model = model, .formal = formal, .next = NO_USE};
    uint64_t key = ((uint64_t)model << TYPE_SHIFT) | formal;
    uint64_t at = 0;
    if (odin3_u64map_get(rd->use_keys, key, &at)) {
        return ODIN3_OK;
    }
    uint32_t index = (uint32_t)rd->uses.len;
    blif_use *slot = odin3_vec_push(&rd->uses);
    if (slot == NULL || odin3_u64map_put(rd->use_keys, (odin3_kv){key, 1}) != ODIN3_OK) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *slot = use;
    if (odin3_u64map_get(rd->use_tail, model, &at)) {
        ((blif_use *)odin3_vec_at(&rd->uses, at))->next = index;
    }
    return rd_fail(rd, odin3_u64map_put(rd->use_tail, (odin3_kv){model, index}));
}

/* Notes the formals of a .subckt whose model is not (yet) known to be a model of the file or a
 * registered cell type: it may become an implicit black box (build_implicit). Malformed
 * connections are left to pass 2, which reports them. */
static odin3_status scan_subckt(blif_reader *rd, const odin3_blif_line *ln) {
    uint32_t model = 0;
    uint64_t line = 0;
    odin3_celltype_id found = {0};
    odin3_status st =
        ln->count < 2 ? ODIN3_OK : odin3_design_intern(rd->design, ln->tokens[1], &model);
    if (st != ODIN3_OK || ln->count < 2 || odin3_u64map_get(rd->model_lines, model, &line) ||
        odin3_celltype_find(rd->design, model, &found)) {
        return rd_fail(rd, st);
    }
    st = note_use(rd, model, 0);
    for (uint32_t i = 2; st == ODIN3_OK && i < ln->count; i++) {
        const char *text = ln->tokens[i].ptr;
        const char *eq = memchr(text, '=', ln->tokens[i].len);
        uint32_t formal = 0;
        if (eq != NULL && eq != text) {
            st = rd_fail(rd, odin3_design_intern(
                                 rd->design, (odin3_bytes){text, (size_t)(eq - text)}, &formal));
        }
        if (st == ODIN3_OK && formal != 0) {
            st = note_use(rd, model, formal);
        }
    }
    return st;
}

/* Header directives of the open model (.inputs, .outputs, .clock, .blackbox, .end); .subckt
 * formals are noted for implicit black boxes; other body lines are left to pass 2. */
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
    if (tok_is(tok, ".subckt")) {
        return scan_subckt(rd, ln);
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
    odin3_wire_id wire = {0};
    if (st == ODIN3_OK) {
        wire = odin3_module_port_wire(module, odin3_module_port_count(module) - 1);
    }
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

static odin3_status build_module(blif_reader *rd, blif_model *model) {
    rd->line = model->line;
    odin3_celltype_id taken = {0};
    if (odin3_celltype_find(rd->design, model->name, &taken)) {
        return rd_error(rd, model->line, "model '%s' has the name of a cell type",
                        rd_str(rd, model->name));
    }
    odin3_prov_id prov = {0};
    odin3_module_id id = {0};
    odin3_status st = rd_prov(
        rd, &(odin3_srcloc){.line = model->line, .col = NAME_COL, .end_col = NAME_COL}, &prov);
    if (st == ODIN3_OK) {
        st = odin3_module_create(rd->design, model->name, prov, &id);
    }
    model->id = id.v;
    odin3_module *module = st == ODIN3_OK ? odin3_module_get(rd->design, id) : NULL;
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

static odin3_status build_blackbox(blif_reader *rd, blif_model *model) {
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
    odin3_celltype_id type = {0};
    if (st == ODIN3_OK) {
        st = odin3_celltype_declare_blackbox(rd->design, &def, &type);
    }
    model->id = type.v;
    if (st == ODIN3_ERR_INVALID_ARG) {
        return rd_error(rd, model->line,
                        "black box '%s' conflicts with the cell type of that name or repeats a "
                        "port name",
                        def.name);
    }
    return rd_fail(rd, st);
}

/* The implicit black box `model` whose first use is uses[first]: a local cell type (not a declared
 * model) whose ports are the model's formals in first-use order, each scalar and INOUT (the file
 * does not say which way a pin goes). */
static odin3_status add_implicit(blif_reader *rd, uint32_t model, uint32_t first) {
    odin3_vec_clear(&rd->defs);
    for (uint32_t at = ((const blif_use *)odin3_vec_cat(&rd->uses, first))->next; at != NO_USE;) {
        const blif_use *use = odin3_vec_cat(&rd->uses, at);
        odin3_port_def *port = odin3_vec_push(&rd->defs);
        if (port == NULL) {
            return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        *port = (odin3_port_def){
            .name = rd_str(rd, use->formal), .dir = ODIN3_DIR_INOUT, .scalar = true, .width = 1};
        at = use->next;
    }
    odin3_celltype_def def = {.name = rd_str(rd, model),
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = rd->defs.data,
                              .n_ports = (uint32_t)rd->defs.len};
    return rd_fail(rd, odin3_celltype_add_local(rd->design, &def, NULL));
}

/* Implicit black boxes, in first-use order: every .subckt model that is neither a model of the
 * file nor a registered cell type (Yosys writes `$pow`, `$_DFFSR_PPP_`, … without a .model). */
static odin3_status build_implicit(blif_reader *rd) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->uses.len; i++) {
        const blif_use *use = odin3_vec_cat(&rd->uses, i);
        uint64_t line = 0;
        odin3_celltype_id found = {0};
        if (!odin3_u64map_get(rd->model_lines, use->model, &line) &&
            !odin3_celltype_find(rd->design, use->model, &found)) {
            st = add_implicit(rd, use->model, i);
        }
    }
    return st;
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
    if (st == ODIN3_OK && rd->models.len > 0 && rd_model(rd, 0)->blackbox) {
        st = rd_error(rd, rd_model(rd, 0)->line, "the first model must be a module (the top)");
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->models.len; i++) {
        blif_model *model = rd_model(rd, i);
        st = model->blackbox ? build_blackbox(rd, model) : build_module(rd, model);
    }
    return st == ODIN3_OK ? build_implicit(rd) : st;
}

/* --- pass 2: model bodies ------------------------------------------------------------------ */

static odin3_status file_changed(const blif_reader *rd, uint32_t line) {
    return rd_error(rd, line, "file changed during read");
}

static odin3_status find_builtins(blif_reader *rd) {
    for (uint32_t i = 0; i < BLIF_BUILTINS; i++) {
        uint32_t name = 0;
        odin3_status st =
            odin3_design_intern(rd->design, odin3_bytes_cstr(BUILTIN_NAMES[i]), &name);
        if (st != ODIN3_OK) {
            return rd_fail(rd, st);
        }
        if (!odin3_celltype_find(rd->design, name, &rd->builtin[i])) {
            odin3_log(ODIN3_LOG_ERROR, "blif_read: built-in cell type '%s' is missing",
                      BUILTIN_NAMES[i]);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

/* The net named `name` (token `col` of the current line), created on its first reference. */
static odin3_status get_net(blif_reader *rd, odin3_bytes name, uint32_t col, odin3_net_id *out) {
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(rd->design, name, &str);
    if (st == ODIN3_OK) {
        *out = odin3_module_find_net(rd->module, str);
    }
    if (st == ODIN3_OK && !odin3_net_valid(*out)) {
        odin3_prov_id prov = {0};
        st = rd_prov(rd, &(odin3_srcloc){.line = rd->line, .col = col, .end_col = col}, &prov);
        if (st == ODIN3_OK) {
            st = odin3_net_create(rd->module, str, prov, out);
        }
    }
    return rd_fail(rd, st);
}

/* Appends the net named by token `at` of ln to rd->nets. */
static odin3_status push_net(blif_reader *rd, const odin3_blif_line *ln, uint32_t at) {
    odin3_net_id net = {0};
    odin3_status st = get_net(rd, ln->tokens[at], at + 1, &net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id *slot = odin3_vec_push(&rd->nets);
    if (slot == NULL) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *slot = net;
    return ODIN3_OK;
}

/* A cell to create: type, parameters, one net vector per port, and the directive's line. */
typedef struct blif_cell {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
    uint32_t line, end_col;
} blif_cell;

/* Creates the cell, connected, with its IMPORTED record; it becomes the previous cell. */
static odin3_status make_cell(blif_reader *rd, const blif_cell *cell) {
    rd->line = cell->line;
    odin3_node_spec spec = {.type = cell->type, .params = cell->params, .n_params = cell->n_params};
    odin3_status st = rd_prov(
        rd, &(odin3_srcloc){.line = cell->line, .col = 1, .end_col = cell->end_col}, &spec.prov);
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(rd->module, &spec, cell->ports, &node);
    }
    if (st == ODIN3_OK) {
        rd->last = node;
    }
    return rd_fail(rd, st);
}

/* --- .names -------------------------------------------------------------------------------- */

static odin3_status begin_names(blif_reader *rd, const odin3_blif_line *ln) {
    if (ln->count < 2) {
        return rd_error(rd, ln->line, ".names needs an output");
    }
    odin3_vec_clear(&rd->nets);
    odin3_strbuf_clear(&rd->cover);
    for (uint32_t i = 1; i < ln->count; i++) {
        odin3_status st = push_net(rd, ln, i);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    rd->sop =
        (blif_sop){.open = true, .width = ln->count - 2, .line = ln->line, .end_col = ln->count};
    return ODIN3_OK;
}

/* True when every byte of tok is one of the characters of `allowed` (a NUL byte never is). */
static bool only_chars(odin3_bytes tok, const char *allowed) {
    const char *text = tok.ptr;
    for (size_t i = 0; i < tok.len; i++) {
        if (text[i] == '\0' || strchr(allowed, text[i]) == NULL) {
            return false;
        }
    }
    return true;
}

/* A cover row of the open .names: `<inputs> <output>`, or `<output>` with no inputs. */
static odin3_status cover_row(blif_reader *rd, const odin3_blif_line *ln) {
    if (!rd->sop.open) {
        return rd_error(rd, ln->line, "cover row outside .names");
    }
    uint32_t width = rd->sop.width;
    odin3_bytes out = ln->tokens[ln->count - 1];
    bool fits = width == 0 ? ln->count == 1
                           : ln->count == 2 && ln->tokens[0].len == width &&
                                 only_chars(ln->tokens[0], "01-");
    if (!fits || out.len != 1 || !only_chars(out, "01")) {
        return rd_error(rd, ln->line, "cover row does not fit .names with %u inputs",
                        (unsigned)width);
    }
    if (rd->cover.len + width + 1 > UINT32_MAX) {
        return rd_error(rd, ln->line, "cover of .names too large");
    }
    odin3_status st = width == 0 ? ODIN3_OK : odin3_strbuf_append(&rd->cover, ln->tokens[0]);
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(&rd->cover, out);
    }
    return rd_fail(rd, st);
}

/* Makes the $sop of the open .names, if any. */
static odin3_status end_names(blif_reader *rd) {
    if (!rd->sop.open) {
        return ODIN3_OK;
    }
    rd->sop.open = false;
    uint32_t width = rd->sop.width;
    const odin3_net_id *nets = rd->nets.data;
    odin3_value params[SOP_PARAMS] = {odin3_value_int(width), {.kind = ODIN3_VAL_COVER}};
    params[SOP_COVER].bits = (const uint8_t *)rd->cover.data;
    params[SOP_COVER].len = (uint32_t)rd->cover.len;
    params[SOP_COVER].cover_inputs = width;
    const odin3_netvec ports[] = {{nets, width}, {nets + width, 1}};
    blif_cell cell = {.type = rd->builtin[BLIF_SOP],
                      .params = params,
                      .n_params = SOP_PARAMS,
                      .ports = ports,
                      .line = rd->sop.line,
                      .end_col = rd->sop.end_col};
    return make_cell(rd, &cell);
}

/* --- .latch -------------------------------------------------------------------------------- */

/* The cell type of a .latch: by its type token when it has one, else $_FF_. */
static odin3_status latch_type(blif_reader *rd, const odin3_blif_line *ln,
                               odin3_celltype_id *type) {
    *type = rd->builtin[BLIF_FF];
    if (ln->count < LATCH_TYPED) {
        return ODIN3_OK;
    }
    const odin3_bytes *word = &ln->tokens[LATCH_TYPE_TOK];
    for (uint32_t i = 0; i < N_LATCH_TYPES; i++) {
        if (tok_is(word, LATCH_TYPES[i].word)) {
            *type = rd->builtin[LATCH_TYPES[i].type];
            return ODIN3_OK;
        }
    }
    if (tok_is(word, "as")) {
        return rd_error(rd, ln->line, "latch type 'as' (asynchronous) is not supported");
    }
    return rd_error(rd, ln->line, "unknown latch type '%s'", (const char *)word->ptr);
}

/* INIT: the last token of `.latch in out init` / `.latch in out type ctrl init`, else 3. */
static odin3_status latch_init(const blif_reader *rd, const odin3_blif_line *ln, int64_t *init) {
    *init = LATCH_INIT_DEFAULT;
    if (ln->count != LATCH_MIN + 1 && ln->count != LATCH_MAX) {
        return ODIN3_OK;
    }
    const odin3_bytes *tok = &ln->tokens[ln->count - 1];
    const char *text = tok->ptr;
    if (tok->len != 1 || text[0] < '0' || text[0] > '3') {
        return rd_error(rd, ln->line, "latch init must be 0, 1, 2 or 3, not '%s'", text);
    }
    *init = text[0] - '0';
    return ODIN3_OK;
}

static odin3_status read_latch(blif_reader *rd, const odin3_blif_line *ln) {
    if (ln->count < LATCH_MIN || ln->count > LATCH_MAX) {
        return rd_error(rd, ln->line,
                        ".latch takes an input, an output, [a type and a control] "
                        "and [an init]");
    }
    odin3_celltype_id type = {0};
    odin3_value init = odin3_value_int(LATCH_INIT_DEFAULT);
    odin3_status st = latch_type(rd, ln, &type);
    if (st == ODIN3_OK) {
        st = latch_init(rd, ln, &init.i);
    }
    odin3_vec_clear(&rd->nets);
    bool typed = ln->count >= LATCH_TYPED;
    /* in, out, ctrl: nets are made in token order */
    st = st == ODIN3_OK ? push_net(rd, ln, 1) : st;
    st = st == ODIN3_OK ? push_net(rd, ln, 2) : st;
    if (st == ODIN3_OK && typed) {
        st = push_net(rd, ln, LATCH_CTRL_TOK);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_net_id *nets = rd->nets.data;
    const odin3_netvec ports[] = {{nets + 2, 1}, {nets, 1}, {nets + 1, 1}}; /* C/E, D, Q */
    blif_cell cell = {.type = type,
                      .params = &init,
                      .n_params = 1,
                      .ports = typed ? ports : ports + 1,
                      .line = ln->line,
                      .end_col = ln->count};
    return make_cell(rd, &cell);
}

/* --- .subckt ------------------------------------------------------------------------------- */

/* The instantiated type and its definition. */
typedef struct blif_inst {
    odin3_celltype_id type;
    const odin3_celltype_def *def;
} blif_inst;

/* Fills rd->dflts (default parameters), rd->ports and rd->nets (every pin unconnected). */
static odin3_status begin_inst(blif_reader *rd, const blif_inst *inst) {
    const odin3_celltype_def *def = inst->def;
    odin3_vec_clear(&rd->dflts);
    odin3_vec_clear(&rd->ports);
    odin3_vec_clear(&rd->nets);
    for (uint32_t i = 0; i < def->n_params; i++) {
        odin3_value *slot = odin3_vec_push(&rd->dflts);
        if (slot == NULL) {
            return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        *slot = def->params[i].dflt;
    }
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_netvec *slot = odin3_vec_push(&rd->ports);
        if (slot == NULL) {
            return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        slot->count = odin3_celltype_port_width(rd->design, inst->type, rd->dflts.data, port);
        for (uint32_t k = 0; k < slot->count; k++) {
            odin3_net_id *net = odin3_vec_push(&rd->nets);
            if (net == NULL) {
                return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
            }
            *net = (odin3_net_id){0}; /* unconnected until a formal names it */
        }
    }
    odin3_netvec *ports = rd->ports.data;
    const odin3_net_id *nets = rd->nets.data;
    for (uint32_t port = 0, at = 0; port < def->n_ports; at += ports[port].count, port++) {
        ports[port].nets = nets + at;
    }
    return ODIN3_OK;
}

/* Index of the port of def named exactly name[0, len), or n_ports when there is none. */
static uint32_t find_port(const odin3_celltype_def *def, const char *name, size_t len) {
    for (uint32_t port = 0; port < def->n_ports; port++) {
        const char *have = def->ports[port].name;
        if (strlen(have) == len && memcmp(have, name, len) == 0) {
            return port;
        }
    }
    return def->n_ports;
}

/* The pin a formal names: an exact port name of width 1, else `p[k]` for bit k of vector p. */
static odin3_status match_formal(blif_reader *rd, const blif_inst *inst, odin3_bytes formal,
                                 uint64_t *where) {
    const char *name = formal.ptr;
    const odin3_netvec *ports = rd->ports.data;
    uint32_t n_ports = inst->def->n_ports;
    uint32_t port = find_port(inst->def, name, formal.len);
    uint32_t bit = 0;
    size_t base_len = 0;
    if (port < n_ports && ports[port].count != 1) {
        return rd_error(rd, rd->line, "formal '%.*s' of '%s' is a port of width %u (use %.*s[k])",
                        (int)formal.len, name, inst->def->name, (unsigned)ports[port].count,
                        (int)formal.len, name);
    }
    if (port == n_ports && parse_bit(name, formal.len, &base_len, &bit)) {
        port = find_port(inst->def, name, base_len);
        if (port < n_ports && (inst->def->ports[port].scalar || bit >= ports[port].count)) {
            port = n_ports;
        }
    }
    if (port == n_ports) {
        return rd_error(rd, rd->line, "model '%s' has no port '%.*s'", inst->def->name,
                        (int)formal.len, name);
    }
    *where = ((uint64_t)port << TYPE_SHIFT) | bit;
    return ODIN3_OK;
}

/* Index into rd->nets of the pin a formal names, through the per-read cache rd->formals. */
static odin3_status formal_pin(blif_reader *rd, const blif_inst *inst, odin3_bytes formal,
                               size_t *index) {
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(rd->design, formal, &str);
    if (st != ODIN3_OK) {
        return rd_fail(rd, st);
    }
    uint64_t key = ((uint64_t)inst->type.v << TYPE_SHIFT) | str;
    uint64_t where = 0;
    if (!odin3_u64map_get(rd->formals, key, &where)) {
        st = match_formal(rd, inst, formal, &where);
        if (st == ODIN3_OK) {
            st = rd_fail(rd, odin3_u64map_put(rd->formals, (odin3_kv){key, where}));
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    const odin3_netvec *port = (const odin3_netvec *)rd->ports.data + (where >> TYPE_SHIFT);
    *index = (size_t)(port->nets - (const odin3_net_id *)rd->nets.data) + (uint32_t)where;
    return ODIN3_OK;
}

/* Connects token `at` of a .subckt line, `formal=actual`. */
static odin3_status connect_formal(blif_reader *rd, const blif_inst *inst,
                                   const odin3_blif_line *ln, uint32_t at) {
    odin3_bytes tok = ln->tokens[at];
    const char *text = tok.ptr;
    const char *eq = memchr(text, '=', tok.len);
    if (eq == NULL || eq == text || eq == text + tok.len - 1) {
        return rd_error(rd, ln->line, "expected formal=actual, not '%s'", text);
    }
    odin3_bytes formal = {text, (size_t)(eq - text)};
    size_t index = 0;
    odin3_status st = formal_pin(rd, inst, formal, &index);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id *nets = rd->nets.data;
    if (odin3_net_valid(nets[index])) {
        return rd_error(rd, ln->line, "formal '%.*s' connected twice", (int)formal.len, text);
    }
    return get_net(rd, (odin3_bytes){eq + 1, tok.len - formal.len - 1}, at + 1, &nets[index]);
}

static odin3_status read_subckt(blif_reader *rd, const odin3_blif_line *ln) {
    if (ln->count < 2) {
        return rd_error(rd, ln->line, ".subckt needs a model name");
    }
    const char *model = ln->tokens[1].ptr;
    blif_inst inst = {0};
    uint32_t name = 0;
    odin3_status st = odin3_design_intern(rd->design, ln->tokens[1], &name);
    if (st != ODIN3_OK) {
        return rd_fail(rd, st);
    }
    if (!odin3_celltype_find(rd->design, name, &inst.type)) {
        return rd_error(rd, ln->line, "unknown model '%s'", model);
    }
    inst.def = odin3_celltype_get(rd->design, inst.type);
    if (inst.def->gran == ODIN3_GRAN_PORT) {
        return rd_error(rd, ln->line, "cell type '%s' cannot be instantiated", model);
    }
    if (inst.type.v == odin3_module_celltype(rd->module).v) {
        return rd_error(rd, ln->line, "model '%s' instantiates itself", model);
    }
    st = begin_inst(rd, &inst);
    for (uint32_t i = 2; st == ODIN3_OK && i < ln->count; i++) {
        st = connect_formal(rd, &inst, ln, i);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    blif_cell cell = {.type = inst.type,
                      .params = rd->dflts.data,
                      .n_params = inst.def->n_params,
                      .ports = rd->ports.data,
                      .line = ln->line,
                      .end_col = ln->count};
    return make_cell(rd, &cell);
}

/* --- .cname, .attr, .param ----------------------------------------------------------------- */

static odin3_status need_last(const blif_reader *rd, const odin3_blif_line *ln) {
    if (!odin3_node_valid(rd->last)) {
        return rd_error(rd, ln->line, "'%s' with no previous cell",
                        (const char *)ln->tokens[0].ptr);
    }
    return ODIN3_OK;
}

static odin3_status read_cname(blif_reader *rd, const odin3_blif_line *ln) {
    if (ln->count != 2) {
        return rd_error(rd, ln->line, ".cname takes one name");
    }
    odin3_status st = need_last(rd, ln);
    uint32_t name = 0;
    if (st == ODIN3_OK) {
        st = rd_fail(rd, odin3_design_intern(rd->design, ln->tokens[1], &name));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_id have = odin3_module_find_node(rd->module, name);
    if (odin3_node_valid(have) && have.v != rd->last.v) {
        return rd_error(rd, ln->line, "duplicate cell name '%s'", rd_str(rd, name));
    }
    return rd_fail(rd, odin3_node_rename(rd->module, rd->last, name));
}

/* Interns the text in rd->scratch. */
static odin3_status intern_scratch(blif_reader *rd, uint32_t *out) {
    return odin3_design_intern(rd->design, (odin3_bytes){rd->scratch.data, rd->scratch.len}, out);
}

/* Sets attribute key (a strtab ID) of the previous cell to val. */
static odin3_status set_last_attr(blif_reader *rd, uint32_t key, const odin3_value *val) {
    return odin3_attr_set(rd->module, (odin3_objref){ODIN3_OBJ_NODE, rd->last.v}, key, val);
}

/* Appends key to the previous cell's ODIN3_BLIF_ATTR_EXTRAS list. */
static odin3_status list_extra(blif_reader *rd, uint32_t key) {
    uint32_t list_key = 0;
    odin3_value list = {.kind = ODIN3_VAL_STRING};
    odin3_status st =
        odin3_design_intern(rd->design, odin3_bytes_cstr(ODIN3_BLIF_ATTR_EXTRAS), &list_key);
    const odin3_value *old =
        odin3_attr_get(rd->module, (odin3_objref){ODIN3_OBJ_NODE, rd->last.v}, list_key);
    odin3_strbuf_clear(&rd->scratch);
    if (st == ODIN3_OK && old != NULL) {
        st = odin3_strbuf_appendf(&rd->scratch, "%s ", rd_str(rd, old->str));
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(rd_str(rd, key)));
    }
    if (st == ODIN3_OK) {
        st = intern_scratch(rd, &list.str);
    }
    if (st == ODIN3_OK) {
        st = set_last_attr(rd, list_key, &list);
    }
    return st;
}

/* `.attr key value…` / `.param key value…`: attribute prefix+key = the value tokens joined. */
static odin3_status read_extra(blif_reader *rd, const odin3_blif_line *ln, const char *prefix) {
    if (ln->count < EXTRA_MIN) {
        return rd_error(rd, ln->line, "%s takes a key and a value",
                        (const char *)ln->tokens[0].ptr);
    }
    odin3_status st = need_last(rd, ln);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t key = 0;
    odin3_value value = {.kind = ODIN3_VAL_STRING};
    odin3_strbuf_clear(&rd->scratch);
    st = odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(prefix));
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(&rd->scratch, ln->tokens[1]);
    }
    if (st == ODIN3_OK) {
        st = intern_scratch(rd, &key);
    }
    odin3_strbuf_clear(&rd->scratch);
    for (uint32_t i = 2; st == ODIN3_OK && i < ln->count; i++) {
        st = i == 2 ? ODIN3_OK : odin3_strbuf_append(&rd->scratch, odin3_bytes_cstr(" "));
        st = st == ODIN3_OK ? odin3_strbuf_append(&rd->scratch, ln->tokens[i]) : st;
    }
    if (st == ODIN3_OK) {
        st = intern_scratch(rd, &value.str);
    }
    bool listed =
        st == ODIN3_OK &&
        odin3_attr_get(rd->module, (odin3_objref){ODIN3_OBJ_NODE, rd->last.v}, key) != NULL;
    if (st == ODIN3_OK) {
        st = set_last_attr(rd, key, &value);
    }
    if (st == ODIN3_OK && !listed) {
        st = list_extra(rd, key);
    }
    return rd_fail(rd, st);
}

/* --- the body loop ------------------------------------------------------------------------- */

static bool is_header(const odin3_bytes *tok) {
    return tok_is(tok, ".inputs") || tok_is(tok, ".outputs") || tok_is(tok, ".clock") ||
           tok_is(tok, ".blackbox") || tok_is(tok, ".end");
}

/* A `.model` line of pass 2: the next model of pass 1, else the file changed in between. */
static odin3_status body_model(blif_reader *rd, const odin3_blif_line *ln) {
    odin3_status st = end_names(rd);
    if (st != ODIN3_OK) {
        return st;
    }
    if (rd->model_at >= rd->models.len || ln->count != 2) {
        return file_changed(rd, ln->line);
    }
    const blif_model *model = rd_model(rd, rd->model_at);
    const odin3_strtab *tab = odin3_design_strtab(rd->design);
    if (ln->tokens[1].len != odin3_strtab_len(tab, model->name) ||
        memcmp(ln->tokens[1].ptr, rd_str(rd, model->name), ln->tokens[1].len) != 0) {
        return file_changed(rd, ln->line);
    }
    rd->model_at++;
    rd->module =
        model->blackbox ? NULL : odin3_module_get(rd->design, (odin3_module_id){model->id});
    rd->last = (odin3_node_id){0};
    return ODIN3_OK;
}

static odin3_status cell_directive(blif_reader *rd, const odin3_blif_line *ln) {
    const odin3_bytes *tok = &ln->tokens[0];
    if (tok_is(tok, ".names")) {
        return begin_names(rd, ln);
    }
    if (tok_is(tok, ".latch")) {
        return read_latch(rd, ln);
    }
    if (tok_is(tok, ".subckt")) {
        return read_subckt(rd, ln);
    }
    if (tok_is(tok, ".cname")) {
        return read_cname(rd, ln);
    }
    if (tok_is(tok, ".attr")) {
        return read_extra(rd, ln, ODIN3_BLIF_ATTR_PREFIX);
    }
    if (tok_is(tok, ".param")) {
        return read_extra(rd, ln, ODIN3_BLIF_PARAM_PREFIX);
    }
    return rd_error(rd, ln->line, "unknown directive '%s'", (const char *)tok->ptr);
}

/* One line of pass 2. Any directive ends the open .names (its cell is made first). */
static odin3_status body_line(blif_reader *rd, const odin3_blif_line *ln) {
    rd->line = ln->line;
    const odin3_bytes *tok = &ln->tokens[0];
    if (tok_is(tok, ".model")) {
        return body_model(rd, ln);
    }
    if (rd->model_at == 0) {
        return file_changed(rd, ln->line);
    }
    if (((const char *)tok->ptr)[0] != '.') {
        return cover_row(rd, ln);
    }
    odin3_status st = end_names(rd);
    if (st != ODIN3_OK || is_header(tok)) {
        return st;
    }
    if (rd->module == NULL) {
        return rd_error(rd, ln->line, "black box '%s' has a body",
                        rd_str(rd, rd_model(rd, rd->model_at - 1)->name));
    }
    return cell_directive(rd, ln);
}

static odin3_status read_bodies(blif_reader *rd) {
    odin3_status st = find_builtins(rd);
    odin3_blif_lexer *lx = st == ODIN3_OK ? odin3_blif_lexer_open(rd->path) : NULL;
    if (lx == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_IO : st;
    }
    odin3_blif_line ln;
    while (st == ODIN3_OK && odin3_blif_lexer_next(lx, &ln)) {
        st = body_line(rd, &ln);
    }
    if (st == ODIN3_OK) {
        st = odin3_blif_lexer_status(lx);
    }
    odin3_blif_lexer_close(lx);
    if (st == ODIN3_OK) {
        st = end_names(rd);
    }
    if (st == ODIN3_OK && rd->model_at != rd->models.len) {
        st = file_changed(rd, rd->line);
    }
    return st;
}

/* --- entry point --------------------------------------------------------------------------- */

static odin3_status rd_init(blif_reader *rd, odin3_design *design, const char *path) {
    *rd = (blif_reader){.design = design, .path = path};
    odin3_vec_init(&rd->models, sizeof(blif_model));
    odin3_vec_init(&rd->names, sizeof(blif_name));
    odin3_vec_init(&rd->defs, sizeof(odin3_port_def));
    odin3_vec_init(&rd->nets, sizeof(odin3_net_id));
    odin3_vec_init(&rd->ports, sizeof(odin3_netvec));
    odin3_vec_init(&rd->dflts, sizeof(odin3_value));
    odin3_vec_init(&rd->uses, sizeof(blif_use));
    odin3_strbuf_init(&rd->scratch);
    odin3_strbuf_init(&rd->clocks);
    odin3_strbuf_init(&rd->cover);
    rd->model_lines = odin3_u64map_create(0);
    rd->port_lines = odin3_u64map_create(0);
    rd->base_bits = odin3_u64map_create(0);
    rd->formals = odin3_u64map_create(0);
    rd->use_keys = odin3_u64map_create(0);
    rd->use_tail = odin3_u64map_create(0);
    if (rd->model_lines == NULL || rd->port_lines == NULL || rd->base_bits == NULL ||
        rd->formals == NULL || rd->use_keys == NULL || rd->use_tail == NULL) {
        return rd_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    return rd_fail(rd, odin3_design_intern(design, odin3_bytes_cstr(path), &rd->file));
}

static void rd_free(blif_reader *rd) {
    odin3_vec_free(&rd->models);
    odin3_vec_free(&rd->names);
    odin3_vec_free(&rd->defs);
    odin3_vec_free(&rd->nets);
    odin3_vec_free(&rd->ports);
    odin3_vec_free(&rd->dflts);
    odin3_vec_free(&rd->uses);
    odin3_strbuf_free(&rd->scratch);
    odin3_strbuf_free(&rd->clocks);
    odin3_strbuf_free(&rd->cover);
    odin3_u64map_destroy(rd->model_lines);
    odin3_u64map_destroy(rd->port_lines);
    odin3_u64map_destroy(rd->base_bits);
    odin3_u64map_destroy(rd->formals);
    odin3_u64map_destroy(rd->use_keys);
    odin3_u64map_destroy(rd->use_tail);
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
