/*
 * restore.c — restoring a version 2 name map (docs/V1.2.0.md 4.5); see restore.h.
 *
 * Two phases. Plan: every line of the map is parsed into typed rows and every rule of the restore
 * is checked, with located messages, and the design is not touched. Apply: the pass runs, the
 * records, the construct rows and the extra lines are created, which can then fail only on out of
 * memory. A map that fails the plan restores nothing.
 */
#include "ir/restore.h"

#include "ast/kinds.h"
#include "backends/common/hdl_loc.h"
#include "ir/construct.h"
#include "ir/ir_internal.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    KEY_SHIFT = 32,
    SPAN_NUMBERS = 4,
    MAX_DECIMAL_DIGITS = 10,
    DECIMAL_BASE = 10,
    PASS_ARG_MAX = 3
};

static bool bytes_is(odin3_bytes field, const char *word) {
    return field.len == strlen(word) && memcmp(field.ptr, word, field.len) == 0;
}

static bool bytes_has_prefix(odin3_bytes field, const char *prefix) {
    size_t len = strlen(prefix);
    return field.len >= len && memcmp(field.ptr, prefix, len) == 0;
}

static const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME = 0x100000001b3ULL;

/* --- the extra table -------------------------------------------------------------------------- */

typedef struct extra_span {
    size_t off, len;
} extra_span;

/* The flags of one record that the A, Q, K lines do not hold (an internal list of the table). */
typedef struct extra_flags {
    uint32_t record;
    extra_span span; /* into flag_text */
} extra_flags;

struct odin3_extra_table {
    odin3_strbuf text;
    odin3_vec spans; /* extra_span: the A, Q, K lines */
    odin3_strbuf flag_text;
    odin3_vec flags; /* extra_flags */
    uint32_t mutating;
};

odin3_extra_table *odin3_extra_create(void) {
    odin3_extra_table *table = odin3_util_calloc(sizeof *table);
    if (table != NULL) {
        odin3_strbuf_init(&table->text);
        odin3_vec_init(&table->spans, sizeof(extra_span));
        odin3_strbuf_init(&table->flag_text);
        odin3_vec_init(&table->flags, sizeof(extra_flags));
    }
    return table;
}

void odin3_extra_destroy(odin3_extra_table *table) {
    if (table == NULL) {
        return;
    }
    odin3_strbuf_free(&table->text);
    odin3_vec_free(&table->spans);
    odin3_strbuf_free(&table->flag_text);
    odin3_vec_free(&table->flags);
    odin3_util_free(table);
}

odin3_status odin3_extra_add(odin3_extra_table *table, odin3_bytes line) {
    extra_span *span = odin3_vec_push(&table->spans);
    if (span == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *span = (extra_span){table->text.len, line.len};
    if (odin3_strbuf_append(&table->text, line) != ODIN3_OK) {
        odin3_vec_pop(&table->spans);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

odin3_status odin3_extra_clone(odin3_design *dst, const odin3_design *src) {
    if (src->extra == NULL) {
        return ODIN3_OK;
    }
    dst->extra = odin3_extra_create();
    if (dst->extra == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_vec_copy(&dst->extra->spans, &src->extra->spans);
    st = st == ODIN3_OK ? odin3_vec_copy(&dst->extra->flags, &src->extra->flags) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_append(&dst->extra->text,
                                   (odin3_bytes){src->extra->text.data, src->extra->text.len})
             : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(&dst->extra->flag_text,
                                              (odin3_bytes){src->extra->flag_text.data,
                                                            src->extra->flag_text.len})
                        : st;
    dst->extra->mutating = src->extra->mutating;
    return st;
}

odin3_status odin3_extra_add_flags(odin3_extra_table *table, uint32_t record, odin3_bytes flags) {
    extra_flags *entry = odin3_vec_push(&table->flags);
    if (entry == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *entry = (extra_flags){record, {table->flag_text.len, flags.len}};
    if (odin3_strbuf_append(&table->flag_text, flags) != ODIN3_OK) {
        odin3_vec_pop(&table->flags);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

uint32_t odin3_extra_flags_count(const odin3_extra_table *table) {
    return table != NULL ? (uint32_t)table->flags.len : 0U;
}

uint32_t odin3_extra_flags_line(const odin3_extra_table *table, uint32_t index, odin3_bytes *flags) {
    if (table == NULL || index >= table->flags.len) {
        *flags = (odin3_bytes){NULL, 0};
        return 0;
    }
    const extra_flags *entry = odin3_vec_cat(&table->flags, index);
    *flags = (odin3_bytes){table->flag_text.data + entry->span.off, entry->span.len};
    return entry->record;
}

uint32_t odin3_extra_count(const odin3_extra_table *table) {
    return table != NULL ? (uint32_t)table->spans.len : 0U;
}

odin3_bytes odin3_extra_line(const odin3_extra_table *table, uint32_t index) {
    if (table == NULL || index >= table->spans.len) {
        return (odin3_bytes){NULL, 0};
    }
    const extra_span *span = odin3_vec_cat(&table->spans, index);
    return (odin3_bytes){table->text.data + span->off, span->len};
}

uint32_t odin3_extra_mutating(const odin3_extra_table *table) {
    return table != NULL ? table->mutating : 0U;
}

size_t odin3_extra_bytes_reserved(const odin3_extra_table *table) {
    if (table == NULL) {
        return 0;
    }
    return sizeof *table + table->text.cap + (table->spans.cap * table->spans.elem_size) +
           table->flag_text.cap + (table->flags.cap * table->flags.elem_size);
}

const odin3_extra_table *odin3_design_extra(const odin3_design *design) {
    return design != NULL ? design->extra : NULL;
}

/*
 * True when the run is of a pass that leaves the netlist as it is: a reader or a writer (`read_`,
 * `write_`), or one of the listed passes that only look (the check, the statistics, the dumps, the
 * name lookups, the simulation, the choice of the top). Every other pass is taken to change the
 * netlist, and the first such run after a restore is the mutating run (V1.2.0 4.5). The list is
 * kept here, apart from the pass table (no registry or ABI change): a new pass that only looks
 * is added to it, and until it is, it counts as a change, which is the safe side (a `stale=` mark
 * on lines that are still right, never the other way).
 */
static bool is_readonly_pass(const odin3_design *design, odin3_passrun_id run) {
    static const char *const LOOKERS[] = {"check", "stats",        "stat",       "hierarchy",
                                          "names", "dump_locs",    "dump_project", "sim",
                                          "preprocess"};
    const odin3_strtab *tab = odin3_design_strtab(design);
    uint32_t name = odin3_passrun_name(design, run);
    const odin3_bytes text = {odin3_strtab_get(tab, name), odin3_strtab_len(tab, name)};
    if (bytes_has_prefix(text, "read_") || bytes_has_prefix(text, "write_")) {
        return true;
    }
    for (size_t k = 0; k < sizeof LOOKERS / sizeof LOOKERS[0]; k++) {
        if (bytes_is(text, LOOKERS[k])) {
            return true;
        }
    }
    return false;
}

void odin3_extra_note_run(odin3_design *design, odin3_passrun_id run, bool restored) {
    odin3_extra_table *table = design->extra;
    if (table != NULL && table->mutating == 0 && !restored && !is_readonly_pass(design, run)) {
        table->mutating = run.v;
    }
}

/* --- parsing the fields ----------------------------------------------------------------------- */

/* Reads a decimal of 1 to 10 digits at *at (up to end) that fits 32 bits; moves *at past it. */
static bool take_decimal(const char **at, const char *end, uint32_t *out) {
    uint64_t value = 0;
    size_t digits = 0;
    while (*at < end && **at >= '0' && **at <= '9' && digits < MAX_DECIMAL_DIGITS) {
        value = (value * DECIMAL_BASE) + (uint64_t)(**at - '0');
        (*at)++;
        digits++;
    }
    if (digits == 0 || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* Reads `char` at *at; moves past it. */
static bool take_char(const char **at, const char *end, char want) {
    if (*at < end && **at == want) {
        (*at)++;
        return true;
    }
    return false;
}

/* A span `L.C-EL.EC`: the four numbers and the end of the text. */
static bool parse_span(odin3_bytes text, uint32_t out[SPAN_NUMBERS]) {
    const char *at = text.ptr;
    const char *end = at + text.len;
    static const char SEPS[SPAN_NUMBERS - 1] = {'.', '-', '.'};
    for (size_t k = 0; k < SPAN_NUMBERS; k++) {
        if (!take_decimal(&at, end, &out[k]) ||
            (k + 1 < SPAN_NUMBERS && !take_char(&at, end, SEPS[k]))) {
            return false;
        }
    }
    return at == end;
}

/* A step `t<n>.<op>`: both numbers above 0. */
static bool parse_step(odin3_bytes step, uint32_t *run, uint32_t *op) {
    const char *dot = step.len > 0 ? memchr(step.ptr, '.', step.len) : NULL;
    if (dot == NULL) {
        return false;
    }
    odin3_bytes head = {step.ptr, (size_t)(dot - (const char *)step.ptr)};
    const char *at = dot + 1;
    const char *end = (const char *)step.ptr + step.len;
    return odin3_name_map_ref_number(head, 't', run) && take_decimal(&at, end, op) && at == end &&
           *op > 0;
}

/* Calls fn for each element of the list field (split at sep; an empty field has none). */
typedef bool (*elem_fn)(void *ctx, odin3_bytes elem);

static bool each_elem(odin3_bytes field, char sep, elem_fn fn, void *ctx) {
    const char *bytes = field.ptr;
    size_t start = 0;
    for (size_t i = 0; field.len > 0 && i <= field.len; i++) {
        if (i == field.len || bytes[i] == sep) {
            if (!fn(ctx, (odin3_bytes){bytes + start, i - start})) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

/* --- the plan --------------------------------------------------------------------------------- */

typedef struct rs_run {
    odin3_bytes name, args, info;
} rs_run;

typedef struct rs_con {
    odin3_bytes name;
    uint32_t parent, file;
    uint32_t span[SPAN_NUMBERS];
    uint8_t kind, level, part;
    bool in_call; /* a call-instance row: the top of a copy, or below one */
} rs_con;

typedef struct rs_rec {
    const odin3_nm_line *line;
    odin3_bytes hier;
    uint32_t run, op, con;
    size_t foreign_off, foreign_len; /* flags the map writers of this version do not define */
    uint8_t kind, dir, from, to, bulk, tail;
    bool capped; /* the flag `capped`: more than 8 locations were joined (a restored one keeps 8) */
} rs_rec;

typedef struct rs {
    odin3_design *design;
    odin3_name_map *map;
    const char *path;
    odin3_construct_mode mode;
    odin3_vec files;      /* odin3_bytes, unescaped */
    odin3_vec runs;       /* rs_run */
    odin3_vec cons;       /* rs_con */
    odin3_vec recs;       /* rs_rec */
    odin3_vec run_id;     /* uint32_t: the design run of each T line */
    odin3_vec rec_id;     /* uint32_t: the design record of each R line */
    odin3_vec ids;        /* odin3_prov_id: the parents of the record being made */
    odin3_strbuf foreign; /* the foreign flags of every record, escaped */
    odin3_vec locs;       /* odin3_srcloc: its origins */
    odin3_u64map *rows;   /* (design run << 32 | c) -> construct row */
    odin3_u64map *seen;   /* design run -> 1 once it holds rows */
    uint8_t *reached;     /* apply: per C line number, 1 once a row exists for it in some run */
    odin3_u64map *kinds;  /* plan: hash of a kind word -> its kind code */
    odin3_u64map *arms;   /* plan: (parent c << 8 | part) of the arm rows planned so far */
    uint32_t default_run;
    uint32_t first_record; /* the first record this restore makes */
    uint32_t twins;        /* records told apart from an equal one by a made-up ast */
    uint32_t problems;
} rs;

enum { MAX_LOGGED_PROBLEMS = 20 };

static void rs_error(rs *st, uint32_t lineno, const char *what) {
    if (st->problems++ < MAX_LOGGED_PROBLEMS) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: name map restore: %s", st->path, lineno, what);
    }
}

static void rs_init(rs *st, odin3_design *design, odin3_name_map *map, const char *path) {
    *st = (rs){.design = design, .map = map, .path = path};
    odin3_vec_init(&st->files, sizeof(odin3_bytes));
    odin3_vec_init(&st->runs, sizeof(rs_run));
    odin3_vec_init(&st->cons, sizeof(rs_con));
    odin3_vec_init(&st->recs, sizeof(rs_rec));
    odin3_vec_init(&st->run_id, sizeof(uint32_t));
    odin3_vec_init(&st->rec_id, sizeof(uint32_t));
    odin3_vec_init(&st->ids, sizeof(odin3_prov_id));
    odin3_strbuf_init(&st->foreign);
    odin3_vec_init(&st->locs, sizeof(odin3_srcloc));
    st->rows = odin3_u64map_create(0);
    st->seen = odin3_u64map_create(0);
    st->arms = odin3_u64map_create(0);
}

static void rs_free(rs *st) {
    odin3_vec_free(&st->files);
    odin3_vec_free(&st->runs);
    odin3_vec_free(&st->cons);
    odin3_vec_free(&st->recs);
    odin3_vec_free(&st->run_id);
    odin3_vec_free(&st->rec_id);
    odin3_vec_free(&st->ids);
    odin3_strbuf_free(&st->foreign);
    odin3_vec_free(&st->locs);
    odin3_u64map_destroy(st->rows);
    odin3_u64map_destroy(st->seen);
    odin3_u64map_destroy(st->arms);
    odin3_u64map_destroy(st->kinds);
    odin3_util_free(st->reached);
}

/* Unescapes a field of a line into *out. NO_MEMORY is returned; a bad escape is a located
 * problem and gives an empty *out. */
static odin3_status rs_unescape(rs *st, const odin3_nm_line *line, odin3_bytes field,
                                odin3_bytes *out) {
    odin3_status status = odin3_name_map_unescape(st->map, field, out);
    if (status == ODIN3_ERR_INVALID_ARG) {
        rs_error(st, line->lineno, "bad escape in a field");
        *out = (odin3_bytes){NULL, 0};
        return ODIN3_OK;
    }
    return status;
}

static const odin3_nm_line *line_at(const rs *st, size_t index) {
    return odin3_vec_cat(&st->map->lines, index);
}

static odin3_status push_bytes(odin3_vec *vec, odin3_bytes value) {
    odin3_bytes *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* F lines: the paths. */
static odin3_status plan_files(rs *st) {
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        odin3_bytes path = {NULL, 0};
        if (line->type == 'F') {
            status = rs_unescape(st, line, line->f[1], &path);
            status = status == ODIN3_OK ? push_bytes(&st->files, path) : status;
        }
    }
    return status;
}

/* Reads one T line into st->runs. */
static odin3_status plan_run_line(rs *st, const odin3_nm_line *line) {
    rs_run run = {{NULL, 0}, {NULL, 0}, {NULL, 0}};
    odin3_status status = rs_unescape(st, line, line->f[1], &run.name);
    status = status == ODIN3_OK ? rs_unescape(st, line, line->f[2], &run.args) : status;
    status = status == ODIN3_OK ? rs_unescape(st, line, line->f[3], &run.info) : status;
    if (status != ODIN3_OK) {
        return status;
    }
    if (run.name.len == 0) {
        rs_error(st, line->lineno, "a T line names no pass");
    }
    rs_run *slot = odin3_vec_push(&st->runs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = run;
    return ODIN3_OK;
}

/* T lines: pass, arguments, information. */
static odin3_status plan_runs(rs *st) {
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        status = line->type == 'T' ? plan_run_line(st, line) : status;
    }
    return status;
}

/* The name of a C line kind: the three synthetic ones, else the AST kind's. */
static const char *kind_name(uint32_t kind) {
    switch (kind) {
    case ODIN3_CONSTRUCT_IF_THEN:
        return "IF_THEN";
    case ODIN3_CONSTRUCT_IF_ELSE:
        return "IF_ELSE";
    case ODIN3_CONSTRUCT_CALL_SITE:
        return "CALL_SITE";
    default:
        return odin3_ast_kind_name(kind);
    }
}

static uint64_t word_hash(odin3_bytes word) {
    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < word.len; i++) {
        hash = (hash ^ (uint64_t)((const unsigned char *)word.ptr)[i]) * FNV_PRIME;
    }
    return hash;
}

/* The hash of every kind word -> its kind code, made once per restore (one lookup per C line
 * instead of a compare against each of about a hundred names). */
static odin3_status make_kind_table(rs *st) {
    const uint32_t synth[] = {ODIN3_CONSTRUCT_IF_THEN, ODIN3_CONSTRUCT_IF_ELSE,
                              ODIN3_CONSTRUCT_CALL_SITE};
    st->kinds = odin3_u64map_create(0);
    if (st->kinds == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t kind = 1; kind < (uint32_t)ODIN3_AST_KIND_COUNT + 3U; kind++) {
        uint32_t code = kind < (uint32_t)ODIN3_AST_KIND_COUNT
                            ? kind
                            : synth[kind - (uint32_t)ODIN3_AST_KIND_COUNT];
        if (odin3_u64map_put(st->kinds,
                             (odin3_kv){word_hash(odin3_bytes_cstr(kind_name(code))), code}) !=
            ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* The kind code of a C line's kind word, 0 when it is none. */
static uint8_t kind_of_word(const rs *st, odin3_bytes word) {
    uint64_t code = 0;
    if (!odin3_u64map_get(st->kinds, word_hash(word), &code) || code == 0 ||
        !bytes_is(word, kind_name((uint32_t)code))) {
        return 0;
    }
    return (uint8_t)code;
}

/* The row a copy of a function or task body hangs under is a call. */
static bool is_call_kind(uint8_t kind) {
    return kind == (uint8_t)ODIN3_AST_CALL || kind == (uint8_t)ODIN3_AST_TASK_CALL;
}

static bool is_decl_kind(uint8_t kind) {
    return kind == (uint8_t)ODIN3_AST_FUNCTION_DECL || kind == (uint8_t)ODIN3_AST_TASK_DECL;
}

/* The checks odin3_construct_add makes, on the plan row `con` (number c, parents already planned).
 */
static bool con_fits(const rs *st, const rs_con *con) {
    odin3_construct_row row = {
        .level = con->level, .call = con->in_call ? 1U : 0U, .kind = con->kind, .part = con->part};
    if (odin3_construct_kind_level(con->kind) == ODIN3_LEVEL_NONE ||
        (uint32_t)odin3_construct_kind_level(con->kind) != con->level) {
        return false;
    }
    if (con->parent == 0) {
        return con->part == ODIN3_CONSTRUCT_PART_NODE;
    }
    const rs_con *parent = odin3_vec_cat(&st->cons, con->parent - 1U);
    if (con->part != ODIN3_CONSTRUCT_PART_NODE && parent->kind != (uint8_t)ODIN3_AST_IF) {
        return false; /* an arm hangs under the IF row of its node */
    }
    odin3_construct_row up = {.level = parent->level};
    return odin3_construct_parent_fits(&up, &row);
}

/* Fills the span, file, parent and name of C line `line` (number c) into con. */
static odin3_status plan_con_fields(rs *st, const odin3_nm_line *line, rs_con *con, bool *good) {
    odin3_level level = ODIN3_LEVEL_NONE;
    uint32_t number = (uint32_t)st->cons.len + 1U;
    *good = true;
    con->kind = kind_of_word(st, line->f[1]);
    *good = *good && con->kind != 0 && odin3_level_from_word(line->f[2], &level);
    con->level = (uint8_t)level;
    con->part = ODIN3_CONSTRUCT_PART_NODE;
    if (con->kind == ODIN3_CONSTRUCT_IF_THEN) {
        con->part = ODIN3_CONSTRUCT_PART_THEN;
    } else if (con->kind == ODIN3_CONSTRUCT_IF_ELSE) {
        con->part = ODIN3_CONSTRUCT_PART_ELSE;
    }
    if (line->f[NM_C_PARENT].len > 0) {
        *good = *good && odin3_name_map_ref_number(line->f[NM_C_PARENT], 'c', &con->parent) &&
                con->parent < number;
    }
    if (line->f[NM_C_FILE].len > 0) {
        *good = *good && odin3_name_map_ref_number(line->f[NM_C_FILE], 'f', &con->file) &&
                con->file <= st->files.len;
    }
    *good = *good && parse_span(line->f[NM_C_SPAN], con->span);
    return rs_unescape(st, line, line->f[NM_C_NAME], &con->name);
}

/* Reads one C line into st->cons (the row keeps its place even when it is bad). */
static odin3_status plan_con_line(rs *st, const odin3_nm_line *line) {
    rs_con con = {.name = {NULL, 0}};
    bool good = false;
    odin3_status status = plan_con_fields(st, line, &con, &good);
    if (status != ODIN3_OK) {
        return status;
    }
    const rs_con *parent = NULL;
    if (con.parent != 0 && good) {
        parent = odin3_vec_cat(&st->cons, con.parent - 1U);
    }
    bool top = is_decl_kind(con.kind) && parent != NULL && is_call_kind(parent->kind);
    con.in_call = top || (parent != NULL && parent->in_call);
    if (!good || !con_fits(st, &con)) {
        rs_error(st, line->lineno, "a C line with a bad kind, level, parent, file or span");
    } else if (con.part != ODIN3_CONSTRUCT_PART_NODE) {
        /* odin3_construct_add refuses a second arm row of one part under one IF row */
        uint64_t key = ((uint64_t)con.parent << 8) | con.part;
        if (odin3_u64map_get(st->arms, key, NULL)) {
            rs_error(st, line->lineno, "a second IF_THEN or IF_ELSE C line under one IF");
        } else if (odin3_u64map_put(st->arms, (odin3_kv){key, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    rs_con *slot = odin3_vec_push(&st->cons);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = con;
    return ODIN3_OK;
}

/* C lines, in number order. */
static odin3_status plan_cons(rs *st) {
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        status = line->type == 'C' ? plan_con_line(st, line) : status;
    }
    return status;
}

/* True when elem is `prefix` and then digits only, 1 to 10 of them, that fit 32 bits. */
static bool number_after(odin3_bytes elem, const char *prefix, uint32_t *out) {
    size_t len = strlen(prefix);
    uint32_t scratch = 0;
    const char *at = (const char *)elem.ptr + len;
    const char *end = (const char *)elem.ptr + elem.len;
    return bytes_has_prefix(elem, prefix) && take_decimal(&at, end, out != NULL ? out : &scratch) &&
           at == end;
}

/* True when elem is `prefix` then a reference `<letter><digits>` as in the map's other fields. */
static bool ref_after(odin3_bytes elem, const char *prefix, char letter) {
    size_t len = strlen(prefix);
    uint32_t num = 0;
    return bytes_has_prefix(elem, prefix) &&
           odin3_name_map_ref_number((odin3_bytes){(const char *)elem.ptr + len, elem.len - len},
                                     letter, &num);
}

/* True for a flag the writers of this version define (3.2) or compute again at write time, with
 * its value well formed: join, join=instance, chain=span, capped, bulk, lossy=bulk|cap, n=<n>,
 * hier=<text>, call=c<n>, cell=<type>, ctx=<n>, stale=t<n>. A known prefix with a bad value is
 * not known: it is a located problem. */
static bool known_flag(odin3_bytes elem) {
    static const char *const WORDS[] = {"join",      "join=instance", "chain=span",
                                        "lossy=bulk", "lossy=cap",     "capped",
                                        "bulk"};
    for (size_t k = 0; k < sizeof WORDS / sizeof WORDS[0]; k++) {
        if (bytes_is(elem, WORDS[k])) {
            return true;
        }
    }
    return number_after(elem, "n=", NULL) || number_after(elem, "ctx=", NULL) ||
           ref_after(elem, "call=", 'c') || ref_after(elem, "stale=", 't') ||
           (bytes_has_prefix(elem, "hier=")) || (bytes_has_prefix(elem, "cell=") && elem.len > 5U);
}

/* True when the element has the form of a flag this version defines but its value is bad. */
static bool broken_flag(odin3_bytes elem) {
    static const char *const PREFIXES[] = {"lossy=", "n=", "call=", "cell=", "ctx=", "stale="};
    for (size_t k = 0; k < sizeof PREFIXES / sizeof PREFIXES[0]; k++) {
        if (bytes_has_prefix(elem, PREFIXES[k])) {
            return true;
        }
    }
    return false;
}

typedef struct flag_job {
    rs *st;
    const odin3_nm_line *line;
    rs_rec *rec;
} flag_job;

/* A flag this version does not define (the site coordinates `x=`, `y=` of odin3-prov-post, ...)
 * is kept, escaped as the file has it, to be written again with the record (V1.2.0 4.5). */
static bool add_foreign(flag_job *job, odin3_bytes elem) {
    rs_rec *rec = job->rec;
    rs *st = job->st;
    if (rec->foreign_len == 0) {
        rec->foreign_off = st->foreign.len;
    } else if (odin3_strbuf_append(&st->foreign, odin3_bytes_cstr(",")) != ODIN3_OK) {
        return false;
    }
    rec->foreign_len += elem.len + (rec->foreign_len > 0 ? 1U : 0U);
    return odin3_strbuf_append(&st->foreign, elem) == ODIN3_OK;
}

/* hier=<text>, ctx=<n>, bulk and capped are read, the other known flags are recomputed. */
static bool read_flag(flag_job *job, odin3_bytes elem) {
    static const char HIER[] = "hier=";
    odin3_bytes text = {NULL, 0};
    if (bytes_is(elem, "capped")) {
        job->rec->capped = true;
    } else if (bytes_is(elem, "bulk")) {
        job->rec->bulk = 1;
    } else if (bytes_has_prefix(elem, HIER)) {
        if (rs_unescape(job->st, job->line, elem, &text) != ODIN3_OK) {
            return false;
        }
        job->rec->hier =
            (odin3_bytes){(const char *)text.ptr + sizeof HIER - 1U, text.len - (sizeof HIER - 1U)};
    } else if (bytes_has_prefix(elem, "ctx=")) {
        uint32_t tail = 0;
        if (!number_after(elem, "ctx=", &tail) || tail > UINT8_MAX) {
            rs_error(job->st, job->line->lineno, "ctx= is not a number below 256");
        }
        job->rec->tail = (uint8_t)tail;
    }
    return true;
}

/* One element of `flags`. An empty element, or a defined flag with a bad value, is a located
 * problem; a flag this version does not define is kept. */
static bool flag_elem(void *ctx, odin3_bytes elem) {
    flag_job *job = ctx;
    if (elem.len == 0) {
        rs_error(job->st, job->line->lineno, "an empty element in the flags");
        return true;
    }
    if (known_flag(elem)) {
        return read_flag(job, elem);
    }
    if (broken_flag(elem)) {
        rs_error(job->st, job->line->lineno, "a flag with a bad value");
        return true;
    }
    return add_foreign(job, elem);
}

typedef struct count_job {
    uint32_t count;
    uint32_t own; /* the number of this line */
    bool good;
} count_job;

static bool parent_elem(void *ctx, odin3_bytes elem) {
    count_job *job = ctx;
    uint32_t num = 0;
    job->good = job->good && odin3_name_map_ref_number(elem, 'r', &num) && num < job->own;
    job->count++;
    return true;
}

static bool origin_elem(void *ctx, odin3_bytes elem) {
    count_job *job = ctx;
    const char *colon = elem.len > 0 ? memchr(elem.ptr, ':', elem.len) : NULL;
    job->good = job->good && colon != NULL &&
                parse_span((odin3_bytes){colon + 1,
                                         elem.len - (size_t)(colon + 1 - (const char *)elem.ptr)},
                           (uint32_t[SPAN_NUMBERS]){0});
    job->count++;
    return true;
}

/* The dir field of a D record: `<dir>:<from level>`; S and I records say `origin`. */
static bool parse_class(const rs *st, const odin3_nm_line *line, rs_rec *rec) {
    odin3_bytes field = line->f[NM_R_DIR];
    odin3_level from = ODIN3_LEVEL_NONE;
    odin3_level to = ODIN3_LEVEL_NONE;
    odin3_prov_dir dir = ODIN3_DIR_ORIGIN;
    (void)st;
    if (rec->kind != (uint8_t)ODIN3_PROV_DERIVED) {
        return bytes_is(field, "origin");
    }
    const char *colon = field.len > 0 ? memchr(field.ptr, ':', field.len) : NULL;
    if (colon == NULL ||
        !odin3_dir_from_word((odin3_bytes){field.ptr, (size_t)(colon - (const char *)field.ptr)},
                             &dir) ||
        !odin3_level_from_word(
            (odin3_bytes){colon + 1, field.len - (size_t)(colon + 1 - (const char *)field.ptr)},
            &from) ||
        !odin3_level_from_word(line->f[NM_R_LEVEL], &to)) {
        return false;
    }
    rec->dir = (uint8_t)dir;
    rec->from = (uint8_t)from;
    rec->to = (uint8_t)to;
    return true;
}

/* The class of a D record as the table holds it. */
static odin3_opclass class_of(const rs_rec *rec) {
    return (odin3_opclass){.dir = (odin3_prov_dir)rec->dir,
                           .from_level = (odin3_level)rec->from,
                           .to_level = (odin3_level)rec->to,
                           .bulk = rec->bulk != 0,
                           .context_tail = rec->tail};
}

/* Checks the kind-specific rules of one R line (counts of parents and origins, the class). */
static void check_rec(rs *st, const odin3_nm_line *line, const rs_rec *rec, uint32_t own) {
    count_job parents = {0, own, true};
    count_job origins = {0, own, true};
    (void)each_elem(line->f[NM_R_PARENTS], ',', parent_elem, &parents);
    (void)each_elem(line->f[NM_R_ORIGINS], '|', origin_elem, &origins);
    bool derived = rec->kind == (uint8_t)ODIN3_PROV_DERIVED;
    odin3_opclass cls = class_of(rec);
    if (!parents.good || !origins.good) {
        rs_error(st, line->lineno, "an R line with a bad parent or origin");
    } else if (derived && parents.count == 0) {
        rs_error(st, line->lineno, "a D record with no parent (check rule 7)");
    } else if (derived ? (rec->tail > parents.count || !odin3_opclass_valid(&cls))
                       : (parents.count != 0 || origins.count > ODIN3_HDL_LOC_MAX)) {
        rs_error(st, line->lineno,
                 derived ? "a D record with a bad class or a context tail past its parents"
                         : "an S or I record with parents or more than 8 origins");
    }
}

/* The kind letter of an R line, the run and operation of its step and its construct. */
static bool parse_rec_head(const rs *st, const odin3_nm_line *line, rs_rec *rec) {
    odin3_bytes kind = line->f[NM_R_KIND];
    const char *letter = kind.len == 1 ? strchr("SID", ((const char *)kind.ptr)[0]) : NULL;
    if (letter == NULL || *letter == '\0') {
        return false;
    }
    rec->kind = (uint8_t)ODIN3_PROV_DERIVED;
    if (*letter == 'S') {
        rec->kind = (uint8_t)ODIN3_PROV_SOURCE;
    } else if (*letter == 'I') {
        rec->kind = (uint8_t)ODIN3_PROV_IMPORTED;
    }
    if (!parse_step(line->f[NM_R_STEP], &rec->run, &rec->op) || rec->run > st->runs.len) {
        return false;
    }
    if (line->f[NM_R_CONS].len > 0 && rec->kind == (uint8_t)ODIN3_PROV_SOURCE) {
        return st->mode != ODIN3_CONSTRUCT_MODE_OFF &&
               odin3_name_map_ref_number(line->f[NM_R_CONS], 'c', &rec->con) &&
               rec->con <= st->cons.len &&
               ((const rs_con *)odin3_vec_cat(&st->cons, rec->con - 1U))->part ==
                   ODIN3_CONSTRUCT_PART_NODE; /* a SOURCE names a node's row, not an arm */
    }
    return true;
}

/* R lines, in number order. */
static odin3_status plan_recs(rs *st) {
    uint32_t own = 0;
    for (size_t i = 0; i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        if (line->type != 'R') {
            continue;
        }
        rs_rec rec = {.line = line, .hier = {NULL, 0}};
        own++;
        flag_job job = {st, line, &rec};
        bool good = parse_rec_head(st, line, &rec) && parse_class(st, line, &rec);
        if (good && each_elem(line->f[NM_R_FLAGS], ',', flag_elem, &job)) {
            check_rec(st, line, &rec, own);
        } else {
            rs_error(st, line->lineno, "an R line with a bad kind, step, class or flags");
        }
        rs_rec *slot = odin3_vec_push(&st->recs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = rec;
    }
    return ODIN3_OK;
}

/* --- applying the plan ------------------------------------------------------------------------ */

static uint64_t row_key(uint32_t run, uint32_t con) {
    return ((uint64_t)run << KEY_SHIFT) | con;
}

static odin3_status intern_bytes(rs *st, odin3_bytes text, uint32_t *id) {
    *id = 0;
    return text.len == 0 ? ODIN3_OK : odin3_design_intern(st->design, text, id);
}

/* A pass-run row for each T line, flagged restored. */
static odin3_status apply_runs(rs *st) {
    for (size_t i = 0; i < st->runs.len; i++) {
        const rs_run *run = odin3_vec_cat(&st->runs, i);
        odin3_passrun_text text = {0, 0, 0};
        odin3_status status = intern_bytes(st, run->name, &text.name);
        status = status == ODIN3_OK ? intern_bytes(st, run->args, &text.args) : status;
        status = status == ODIN3_OK ? intern_bytes(st, run->info, &text.info) : status;
        odin3_pass_ctx ctx;
        status =
            status == ODIN3_OK ? odin3_passrun_append_restored(st->design, &text, &ctx) : status;
        uint32_t *slot = status == ODIN3_OK ? odin3_vec_push(&st->run_id) : NULL;
        if (slot == NULL) {
            return status != ODIN3_OK ? status : ODIN3_ERR_NO_MEMORY;
        }
        *slot = ctx.run.v;
    }
    return ODIN3_OK;
}

/* Marks the first row of a run: the run kept an AST layer of the map's mode. */
static odin3_status note_row_run(rs *st, uint32_t run) {
    if (odin3_u64map_get(st->seen, run, NULL)) {
        return ODIN3_OK;
    }
    odin3_status status = odin3_construct_mark_run(st->design, (odin3_passrun_id){run}, st->mode);
    return status == ODIN3_OK && odin3_u64map_put(st->seen, (odin3_kv){run, 1}) != ODIN3_OK
               ? ODIN3_ERR_NO_MEMORY
               : status;
}

/* Adds the row of construct c in design run `run` under parent row `up` (0 for a module). */
/* A construct (the number of its C line) in a design run. */
typedef struct row_at {
    uint32_t run;
    uint32_t con;
} row_at;

static odin3_status add_row(rs *st, row_at where, uint32_t up) {
    const uint32_t run = where.run;
    const rs_con *con = odin3_vec_cat(&st->cons, where.con - 1U);
    const odin3_construct_row *above = up != 0 ? odin3_construct_get(st->design, up) : NULL;
    const rs_con *plan_up = con->parent != 0 ? odin3_vec_cat(&st->cons, con->parent - 1U) : NULL;
    uint32_t call = 0;
    if (above != NULL) {
        bool top = is_decl_kind(con->kind) && plan_up != NULL && is_call_kind(plan_up->kind);
        call = top ? up : above->call;
    }
    /* An arm row (IF_THEN, IF_ELSE) is a part of its IF row's node: check rule 12 wants it under
     * the IF row of the same node, so it takes the ast of its parent's C line (4.5 said it takes
     * its own; the code's rule wins). Its own C line is reached only as a parent. */
    odin3_construct_row row = {.run = run,
                               .ast =
                                   con->part != ODIN3_CONSTRUCT_PART_NODE ? con->parent : where.con,
                               .call = call,
                               .parent = up,
                               .line = con->span[0],
                               .col = con->span[1],
                               .end_line = con->span[2],
                               .end_col = con->span[SPAN_NUMBERS - 1U],
                               .part = con->part,
                               .level = con->level,
                               .kind = con->kind};
    odin3_status status = ODIN3_OK;
    if (con->file != 0) {
        status = intern_bytes(st, *(const odin3_bytes *)odin3_vec_cat(&st->files, con->file - 1U),
                              &row.file);
    }
    status = status == ODIN3_OK ? intern_bytes(st, con->name, &row.name) : status;
    uint32_t index = 0;
    status = status == ODIN3_OK ? odin3_construct_add(st->design, &row, &index) : status;
    status = status == ODIN3_OK ? note_row_run(st, run) : status;
    st->reached[where.con] = status == ODIN3_OK ? 1U : st->reached[where.con];
    if (status == ODIN3_OK &&
        odin3_u64map_put(st->rows, (odin3_kv){row_key(run, where.con), index}) != ODIN3_OK) {
        status = ODIN3_ERR_NO_MEMORY;
    }
    return status;
}

/* The construct row of C line c in design run `run`, and of its ancestors, created parents
 * first (an explicit stack, no recursion). */
static odin3_status ensure_row(rs *st, row_at where) {
    const uint32_t run = where.run;
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(uint32_t));
    odin3_status status = ODIN3_OK;
    for (uint32_t at = where.con;
         status == ODIN3_OK && at != 0 && !odin3_u64map_get(st->rows, row_key(run, at), NULL);
         at = ((const rs_con *)odin3_vec_cat(&st->cons, at - 1U))->parent) {
        uint32_t *slot = odin3_vec_push(&todo);
        status = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = at;
        }
    }
    while (status == ODIN3_OK && todo.len > 0) {
        uint32_t at = *(const uint32_t *)odin3_vec_cat(&todo, todo.len - 1U);
        uint64_t up = 0;
        uint32_t parent = ((const rs_con *)odin3_vec_cat(&st->cons, at - 1U))->parent;
        if (parent != 0) {
            (void)odin3_u64map_get(st->rows, row_key(run, parent), &up);
        }
        odin3_vec_pop(&todo);
        status = add_row(st, (row_at){run, at}, (uint32_t)up);
    }
    odin3_vec_free(&todo);
    return status;
}

/* One origin element `f<n>:span` or `-:span` as a location. */
static bool loc_elem(void *ctx, odin3_bytes elem) {
    rs *st = ctx;
    const char *colon = memchr(elem.ptr, ':', elem.len);
    odin3_bytes file = {elem.ptr, (size_t)(colon - (const char *)elem.ptr)};
    uint32_t span[SPAN_NUMBERS] = {0};
    uint32_t num = 0;
    odin3_srcloc *loc = odin3_vec_push(&st->locs);
    if (loc == NULL) {
        return false;
    }
    (void)parse_span((odin3_bytes){colon + 1, elem.len - file.len - 1U}, span);
    *loc = (odin3_srcloc){
        .line = span[0], .col = span[1], .end_line = span[2], .end_col = span[SPAN_NUMBERS - 1U]};
    if (odin3_name_map_ref_number(file, 'f', &num) &&
        intern_bytes(st, *(const odin3_bytes *)odin3_vec_cat(&st->files, num - 1U), &loc->file) !=
            ODIN3_OK) {
        return false;
    }
    return true;
}

/* The SOURCE or IMPORTED record for origin in the context's run. */
static odin3_status leaf_record(const odin3_pass_ctx *ctx, odin3_prov_kind kind,
                                const odin3_prov_origin *origin, odin3_prov_id *out) {
    return kind == ODIN3_PROV_SOURCE ? odin3_prov_source(ctx, origin, out)
                                     : odin3_prov_imported(ctx, origin, out);
}

/*
 * Two R lines that differ only in the AST node their record came from (the map has no node ID:
 * equal spans, equal construct) are two records in the file and would hash-cons into one. The
 * second gets an `ast` of its own, past the numbers of the C lines, so that the restore keeps them
 * apart and a second write has the same lines: with a construct, the new `ast` is an alias of the
 * row of the first (odin3_construct_alias); without one, the record is a recorded miss (a span-only
 * leaf, as chain=span says). If the alias is refused the two stay one record (the weaker claim
 * of 4.5).
 */
static odin3_status split_twin(rs *st, const odin3_pass_ctx *ctx, const rs_rec *rec,
                               odin3_prov_origin *origin, odin3_prov_id *out) {
    /* the row of the record's own C line, in the run, whatever call instance it sits in */
    uint64_t found = 0;
    uint32_t row =
        rec->con != 0 && odin3_u64map_get(st->rows, row_key(ctx->run.v, rec->con), &found)
            ? (uint32_t)found
            : 0U;
    origin->ast = (uint32_t)st->cons.len + 1U + st->twins++;
    if (rec->con != 0) {
        if (row == 0 || odin3_construct_alias(st->design, ctx->run, origin->ast, row) != ODIN3_OK) {
            return ODIN3_OK; /* kept as one record */
        }
    }
    odin3_status status = leaf_record(ctx, (odin3_prov_kind)rec->kind, origin, out);
    return status == ODIN3_OK && rec->con == 0 ? odin3_construct_note_miss(st->design, *out)
                                               : status;
}

/* The SOURCE or IMPORTED record of an R line. */
static odin3_status make_leaf(rs *st, const rs_rec *rec, uint32_t run, odin3_prov_id *out) {
    odin3_vec_clear(&st->locs);
    if (!each_elem(rec->line->f[NM_R_ORIGINS], '|', loc_elem, st)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_prov_origin origin = {
        .locs = st->locs.data, .n_locs = (uint32_t)st->locs.len, .ast = rec->con};
    odin3_status status = intern_bytes(st, rec->hier, &origin.hier);
    if (status == ODIN3_OK && rec->con != 0) {
        st->default_run = st->default_run != 0 ? st->default_run : run;
        status = ensure_row(st, (row_at){run, rec->con});
    }
    const odin3_pass_ctx ctx = {st->design, {run}, rec->op};
    if (status != ODIN3_OK) {
        return status;
    }
    uint32_t before = odin3_prov_end(st->design);
    status = leaf_record(&ctx, (odin3_prov_kind)rec->kind, &origin, out);
    if (status == ODIN3_OK && out->v < before && out->v >= st->first_record) {
        status = split_twin(st, &ctx, rec, &origin, out);
    }
    return status;
}

static bool parent_id_elem(void *ctx, odin3_bytes elem) {
    rs *st = ctx;
    uint32_t num = 0;
    if (!odin3_name_map_ref_number(elem, 'r', &num)) {
        return false;
    }
    odin3_prov_id *slot = odin3_vec_push(&st->ids);
    if (slot == NULL) {
        return false;
    }
    slot->v = *(const uint32_t *)odin3_vec_cat(&st->rec_id, num - 1U);
    return true;
}

/* The DERIVED record of an R line: its parents mapped to the design's records, its class. The
 * plan has checked that it has a parent and that the context tail fits them. */
static odin3_status make_derived(rs *st, const rs_rec *rec, uint32_t run, odin3_prov_id *out) {
    odin3_vec_clear(&st->ids);
    if (!each_elem(rec->line->f[NM_R_PARENTS], ',', parent_id_elem, st)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t hier = 0;
    odin3_status status = intern_bytes(st, rec->hier, &hier);
    odin3_opclass cls = class_of(rec);
    const odin3_pass_ctx ctx = {st->design, {run}, rec->op};
    const odin3_prov_list parents = {st->ids.data, (uint32_t)st->ids.len};
    return status == ODIN3_OK ? odin3_prov_derive_class(&ctx, parents, hier, &cls, out) : status;
}

/* Keeps the foreign flags of a record (and its `capped` mark) in the extra table's flag list. */
static odin3_status add_foreign_line(rs *st, const rs_rec *rec, odin3_prov_id id) {
    if (rec->foreign_len == 0 && !rec->capped) {
        return ODIN3_OK;
    }
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_status status = rec->capped ? odin3_strbuf_append(&line, odin3_bytes_cstr("capped"))
                                      : ODIN3_OK;
    status = status == ODIN3_OK && rec->capped && rec->foreign_len > 0
                 ? odin3_strbuf_append(&line, odin3_bytes_cstr(","))
                 : status;
    status = status == ODIN3_OK
                 ? odin3_strbuf_append(
                       &line, (odin3_bytes){st->foreign.data + rec->foreign_off, rec->foreign_len})
                 : status;
    status = status == ODIN3_OK
                 ? odin3_extra_add_flags(st->design->extra, id.v, (odin3_bytes){line.data, line.len})
                 : status;
    odin3_strbuf_free(&line);
    return status;
}

/* The records of the R lines, in file order; rec_id[i] is the design record of line i + 1. */
static odin3_status apply_recs(rs *st) {
    for (size_t i = 0; i < st->recs.len; i++) {
        const rs_rec *rec = odin3_vec_cat(&st->recs, i);
        uint32_t run = *(const uint32_t *)odin3_vec_cat(&st->run_id, rec->run - 1U);
        odin3_prov_id id = {0};
        odin3_status status = rec->kind == (uint8_t)ODIN3_PROV_DERIVED
                                  ? make_derived(st, rec, run, &id)
                                  : make_leaf(st, rec, run, &id);
        uint32_t *slot = status == ODIN3_OK ? odin3_vec_push(&st->rec_id) : NULL;
        if (slot == NULL) {
            return status != ODIN3_OK ? status : ODIN3_ERR_NO_MEMORY;
        }
        *slot = id.v;
        status = add_foreign_line(st, rec, id);
        if (status != ODIN3_OK) {
            return status;
        }
    }
    return ODIN3_OK;
}

/* The rows no record reached (the `full` mode keeps every row): in the run of the first SOURCE
 * with a construct, else the first restored run. */
static odin3_status apply_other_rows(rs *st) {
    uint32_t run = st->default_run;
    if (run == 0 && st->run_id.len > 0) {
        run = *(const uint32_t *)odin3_vec_cat(&st->run_id, 0);
    }
    odin3_status status = ODIN3_OK;
    for (uint32_t con_no = 1; status == ODIN3_OK && run != 0 && con_no <= st->cons.len; con_no++) {
        status = st->reached[con_no] != 0 ? ODIN3_OK : ensure_row(st, (row_at){run, con_no});
    }
    return status;
}

/* --- the extra lines -------------------------------------------------------------------------- */

typedef struct extra_job {
    const rs *st;
    odin3_strbuf *buf;
    bool first;
    odin3_status status;
} extra_job;

/* One `r<n>` of a list: written as the design's record. */
static bool ref_elem(void *ctx, odin3_bytes elem) {
    extra_job *job = ctx;
    uint32_t num = 0;
    if (!odin3_name_map_ref_number(elem, 'r', &num)) {
        return false;
    }
    uint32_t id = *(const uint32_t *)odin3_vec_cat(&job->st->rec_id, num - 1U);
    job->status = job->status == ODIN3_OK && !job->first
                      ? odin3_strbuf_append(job->buf, odin3_bytes_cstr(","))
                      : job->status;
    job->first = false;
    job->status =
        job->status == ODIN3_OK ? odin3_strbuf_appendf(job->buf, "r%u", (unsigned)id) : job->status;
    return job->status == ODIN3_OK;
}

/* The field position of the record list and of the stale run of an A, Q or K line. */
typedef struct extra_pos {
    uint32_t refs, stale;
} extra_pos;

static extra_pos extra_positions(char type) {
    extra_pos pos = {NM_Q_ID, NM_K_STALE};
    if (type == 'A') {
        pos = (extra_pos){NM_A_RECS, NM_A_STALE};
    } else if (type == 'Q') {
        pos.stale = NM_Q_STALE;
    }
    return pos;
}

/* The stored text of one A, Q or K line: the fields as the file has them, with the record ids
 * and the stale run named by the design's. */
static odin3_status extra_text(const rs *st, const odin3_nm_line *line, odin3_strbuf *buf) {
    const extra_pos pos = extra_positions(line->type);
    odin3_strbuf_clear(buf);
    odin3_status status = odin3_strbuf_appendf(buf, "%c", line->type);
    extra_job job = {st, buf, true, ODIN3_OK};
    for (uint32_t k = 0; status == ODIN3_OK && k < line->nfields; k++) {
        status = odin3_strbuf_append(buf, odin3_bytes_cstr("\t"));
        uint32_t run = 0;
        if (status != ODIN3_OK) {
            break;
        }
        if (k == pos.refs) {
            job.first = true;
            (void)each_elem(line->f[k], ',', ref_elem, &job);
            status = job.status;
        } else if (k == pos.stale && odin3_name_map_ref_number(line->f[k], 't', &run)) {
            status = odin3_strbuf_appendf(
                buf, "t%u", (unsigned)*(const uint32_t *)odin3_vec_cat(&st->run_id, run - 1U));
        } else {
            status = odin3_strbuf_append(buf, line->f[k]);
        }
    }
    return status;
}

/* A, Q and K lines into the design's extra table, in file order. */
static odin3_status apply_extra(rs *st) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status status = ODIN3_OK;
    for (size_t i = 0; status == ODIN3_OK && i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        if (line->type == 'A' || line->type == 'Q' || line->type == 'K') {
            status = extra_text(st, line, &buf);
            status = status == ODIN3_OK
                         ? odin3_extra_add(st->design->extra, (odin3_bytes){buf.data, buf.len})
                         : status;
        }
    }
    odin3_strbuf_free(&buf);
    return status;
}

/* --- binding objects -------------------------------------------------------------------------- */

struct odin3_restore {
    const odin3_name_entry *base; /* map->entries.data */
    size_t count;
    uint32_t *entry_rec; /* per entry of the map: its record, 0 none */
    odin3_strtab *keys;  /* (module, kind, name) of the N lines */
    odin3_u64map *named; /* key id -> record */
};

void odin3_restore_free(odin3_restore *restore) {
    if (restore == NULL) {
        return;
    }
    odin3_util_free(restore->entry_rec);
    odin3_strtab_destroy(restore->keys);
    odin3_u64map_destroy(restore->named);
    odin3_util_free(restore);
}

uint32_t odin3_restore_entry_record(const odin3_restore *restore, const odin3_name_entry *entry) {
    if (restore == NULL || entry == NULL || entry < restore->base ||
        (size_t)(entry - restore->base) >= restore->count) {
        return 0;
    }
    return restore->entry_rec[entry - restore->base];
}

/* The key text of an N line: the lengths of module and name, the kind letter, module, name (a
 * name may hold any byte, so no separator is safe). */
static odin3_status key_of(odin3_strbuf *buf, odin3_bytes module, odin3_name_kind kind,
                           odin3_bytes name) {
    odin3_strbuf_clear(buf);
    odin3_status status = odin3_strbuf_appendf(buf, "%zu:%zu:%d:", module.len, name.len, (int)kind);
    status = status == ODIN3_OK ? odin3_strbuf_append(buf, module) : status;
    return status == ODIN3_OK ? odin3_strbuf_append(buf, name) : status;
}

uint32_t odin3_restore_named_record(const odin3_restore *restore, odin3_bytes module,
                                    odin3_name_kind kind, odin3_bytes name) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    uint32_t id = 0;
    uint64_t record = 0;
    bool found = restore != NULL && key_of(&buf, module, kind, name) == ODIN3_OK &&
                 odin3_strtab_find(restore->keys, (odin3_bytes){buf.data, buf.len}, &id) &&
                 odin3_u64map_get(restore->named, id, &record);
    odin3_strbuf_free(&buf);
    return found ? (uint32_t)record : 0U;
}

/* The record an O or N line names: the first id of its recs (more ids are a located warning). */
static uint32_t line_record(const rs *st, const odin3_nm_line *line, odin3_bytes recs) {
    const char *comma = recs.len > 0 ? memchr(recs.ptr, ',', recs.len) : NULL;
    odin3_bytes first =
        comma != NULL ? (odin3_bytes){recs.ptr, (size_t)(comma - (const char *)recs.ptr)} : recs;
    uint32_t num = 0;
    if (comma != NULL) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s:%u: name map restore: recs lists several records; the first is bound and the "
                  "rest are dropped",
                  st->path, line->lineno);
    }
    if (!odin3_name_map_ref_number(first, 'r', &num)) {
        return 0;
    }
    return *(const uint32_t *)odin3_vec_cat(&st->rec_id, num - 1U);
}

/* The three leading fields of an O or N line, unescaped: module, kind, name. */
static odin3_status object_key(rs *st, const odin3_nm_line *line, odin3_bytes out[3]) {
    odin3_status status = ODIN3_OK;
    for (size_t k = 0; status == ODIN3_OK && k < 3; k++) {
        status = rs_unescape(st, line, line->f[k], &out[k]);
    }
    return status;
}

static odin3_status bind_o_line(rs *st, odin3_restore *restore, const odin3_nm_line *line) {
    odin3_bytes key[3];
    odin3_name_kind kind = ODIN3_NAME_NET;
    odin3_status status = object_key(st, line, key);
    if (status == ODIN3_OK && odin3_name_kind_parse(key[1], &kind)) {
        const odin3_name_entry *entry = odin3_name_map_find(st->map, key[0], kind, key[2]);
        if (entry != NULL) {
            restore->entry_rec[entry - restore->base] = line_record(st, line, line->f[NM_O_RECS]);
        }
    }
    return status;
}

static odin3_status bind_n_line(rs *st, odin3_restore *restore, const odin3_nm_line *line,
                                odin3_strbuf *buf) {
    odin3_bytes key[3];
    odin3_name_kind kind = ODIN3_NAME_NET;
    odin3_status status = object_key(st, line, key);
    if (status != ODIN3_OK || !odin3_name_kind_parse(key[1], &kind)) {
        return status;
    }
    uint32_t id = 0;
    status = key_of(buf, key[0], kind, key[2]);
    status = status == ODIN3_OK
                 ? odin3_strtab_intern(restore->keys, (odin3_bytes){buf->data, buf->len}, &id)
                 : status;
    if (status == ODIN3_OK && !odin3_u64map_get(restore->named, id, NULL)) {
        status =
            odin3_u64map_put(restore->named,
                             (odin3_kv){id, line_record(st, line, line->f[NM_N_RECS])}) == ODIN3_OK
                ? ODIN3_OK
                : ODIN3_ERR_NO_MEMORY;
    }
    return status;
}

static odin3_status bind_objects(rs *st, odin3_restore **out) {
    odin3_restore *restore = odin3_util_calloc(sizeof *restore);
    if (restore == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    restore->base = st->map->entries.data;
    restore->count = st->map->entries.len;
    restore->entry_rec = odin3_util_calloc((restore->count + 1U) * sizeof *restore->entry_rec);
    restore->keys = odin3_strtab_create();
    restore->named = odin3_u64map_create(0);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status status =
        restore->entry_rec != NULL && restore->keys != NULL && restore->named != NULL
            ? ODIN3_OK
            : ODIN3_ERR_NO_MEMORY;
    for (size_t i = 0; status == ODIN3_OK && i < st->map->lines.len; i++) {
        const odin3_nm_line *line = line_at(st, i);
        if (line->type == 'O') {
            status = bind_o_line(st, restore, line);
        } else if (line->type == 'N') {
            status = bind_n_line(st, restore, line, &buf);
        }
    }
    odin3_strbuf_free(&buf);
    if (status != ODIN3_OK) {
        odin3_restore_free(restore);
        return status;
    }
    *out = restore;
    return ODIN3_OK;
}

/* --- the entry point -------------------------------------------------------------------------- */

static odin3_construct_mode mode_of_header(odin3_bytes ast) {
    static const struct {
        const char *word;
        odin3_construct_mode mode;
    } MODES[] = {{"refs", ODIN3_CONSTRUCT_MODE_REFS},
                 {"constructs", ODIN3_CONSTRUCT_MODE_CONSTRUCTS},
                 {"full", ODIN3_CONSTRUCT_MODE_FULL}};
    for (size_t k = 0; k < sizeof MODES / sizeof MODES[0]; k++) {
        if (bytes_is(ast, MODES[k].word)) {
            return MODES[k].mode;
        }
    }
    return ODIN3_CONSTRUCT_MODE_OFF;
}

/* Validates the map and parses its lines into the plan. ODIN3_ERR_PARSE when anything is wrong. */
static odin3_status make_plan(rs *st) {
    if (st->arms == NULL || make_kind_table(st) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_name_check_v2 check = odin3_name_map_check_v2(st->map);
    if (!odin3_name_check_v2_report(&check, st->path)) {
        return ODIN3_ERR_PARSE;
    }
    if (st->map->over_limit != 0) {
        rs_error(st, st->map->over_line, st->map->over_why);
    }
    odin3_status status = plan_files(st);
    status = status == ODIN3_OK ? plan_runs(st) : status;
    if (status == ODIN3_OK && st->mode == ODIN3_CONSTRUCT_MODE_OFF && check.constructs != 0) {
        rs_error(st, 1, "the header says ast=none but the map has C lines");
    }
    status = status == ODIN3_OK ? plan_cons(st) : status;
    status = status == ODIN3_OK ? plan_recs(st) : status;
    if (status == ODIN3_OK && st->problems > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: name map restore: %u problem(s); nothing was restored",
                  st->path, (unsigned)st->problems);
        status = ODIN3_ERR_PARSE;
    }
    return status;
}

/* A pass run before the restore that changed the netlist is the mutating run too (4.5). */
static void note_earlier_runs(odin3_design *design) {
    if (design->extra == NULL) {
        return;
    }
    for (uint32_t run = 1; run < odin3_passrun_end(design); run++) {
        odin3_extra_note_run(design, (odin3_passrun_id){run}, odin3_passrun_restored(design,
                                                                (odin3_passrun_id){run}));
    }
}

/* Creates what the plan describes. */
static odin3_status apply_plan(rs *st) {
    if (st->design->extra == NULL) {
        st->design->extra = odin3_extra_create();
        note_earlier_runs(st->design);
    }
    st->reached = odin3_util_calloc(st->cons.len + 1U);
    if (st->design->extra == NULL || st->rows == NULL || st->seen == NULL || st->reached == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    st->first_record = odin3_prov_end(st->design);
    odin3_status status = apply_runs(st);
    status = status == ODIN3_OK ? apply_recs(st) : status;
    status = status == ODIN3_OK ? apply_other_rows(st) : status;
    return status == ODIN3_OK ? apply_extra(st) : status;
}

/*
 * The tables a restore changes: the provenance store (records and pass runs), the op-class table,
 * the construct table and the extra table. The restore works on a copy of them in a shell design
 * (a shallow copy of the struct: modules, arena, strtab and the rest are shared) and swaps them in
 * only when everything, the binding table included, succeeded; a failure frees the copies and the
 * design is as it was read (V1.2.0 4.5). The string table is shared and only grows: strings
 * interned by a failed restore stay, unseen by any record, row, pass run or extra line.
 */
static void shell_free_tables(odin3_design *shell) {
    odin3_prov_store_free(shell);
    odin3_opclass_destroy(shell->opclass);
    odin3_construct_destroy(shell->construct);
    odin3_extra_destroy(shell->extra);
    shell->opclass = NULL;
    shell->construct = NULL;
    shell->extra = NULL;
}

static odin3_status shell_make(odin3_design *shell, const odin3_design *design) {
    *shell = *design;
    shell->prov = NULL;
    shell->extra = NULL;
    shell->opclass = odin3_opclass_create();
    shell->construct = odin3_construct_create();
    odin3_status status = shell->opclass != NULL && shell->construct != NULL
                              ? odin3_prov_store_init(shell)
                              : ODIN3_ERR_NO_MEMORY;
    status = status == ODIN3_OK ? odin3_prov_store_clone(shell, design) : status;
    status = status == ODIN3_OK ? odin3_opclass_copy(shell->opclass, design->opclass) : status;
    status = status == ODIN3_OK ? odin3_construct_copy(shell->construct, design->construct) : status;
    status = status == ODIN3_OK ? odin3_extra_clone(shell, design) : status;
    if (status != ODIN3_OK) {
        shell_free_tables(shell);
    }
    return status;
}

/* The shell's tables become the design's; the design's old ones are freed. */
static void shell_commit(odin3_design *design, odin3_design *shell) {
    odin3_design old = *design;
    design->prov = shell->prov;
    design->opclass = shell->opclass;
    design->construct = shell->construct;
    design->extra = shell->extra;
    shell_free_tables(&old);
}

odin3_status odin3_design_restore(odin3_design *design, odin3_name_map *map, const char *path,
                                  odin3_restore **out) {
    if (design == NULL || map == NULL || path == NULL || out == NULL ||
        map->version != ODIN3_NAME_MAP_VERSION_2) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_design_restore: invalid argument (a version 2 map is needed)");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = NULL;
    rs st;
    rs_init(&st, design, map, path);
    st.mode = mode_of_header(map->ast);
    odin3_status status = make_plan(&st);
    odin3_design shell;
    bool have_shell = false;
    if (status == ODIN3_OK) {
        status = shell_make(&shell, design);
        have_shell = status == ODIN3_OK;
        st.design = &shell;
    }
    status = status == ODIN3_OK ? apply_plan(&st) : status;
    status = status == ODIN3_OK ? bind_objects(&st, out) : status;
    if (status == ODIN3_OK) {
        shell_commit(design, &shell);
        odin3_log(ODIN3_LOG_INFO,
                  "%s: name map restore: %zu record(s), %zu construct(s), %zu pass run(s), %u "
                  "extra line(s)",
                  path, st.recs.len, st.cons.len, st.runs.len,
                  (unsigned)odin3_extra_count(design->extra));
    } else if (have_shell) {
        shell_free_tables(&shell);
    }
    rs_free(&st);
    return status;
}
