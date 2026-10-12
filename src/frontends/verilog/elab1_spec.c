/*
 * elab1_spec.c — specializations (2D spec §3.4, EL-9): the key bytes (the definition, each
 * parameter's width, flags and bits, the pending overrides sorted by path), their
 * odin3_hash_bytes in a u64map from hash to a chain of specializations, EL-9's names with point
 * (q)'s collision suffixes, the specialization and depth caps (point (aa)), the FIFO queue of
 * §3.4 with the project's parameters as the top's overrides, and the iterative-DFS acyclicity
 * check of the specialization graph.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/project/project.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { E1_FIRST_SUFFIX = 2, E1_WHITE = 0, E1_GRAY, E1_BLACK };

/* --- the key ------------------------------------------------------------------------------- */

static odin3_status key_u32(odin3_strbuf *key, uint32_t value) {
    return odin3_strbuf_append(key, (odin3_bytes){&value, sizeof value});
}

uint8_t odin3_elab1_key_flags(const odin3_cval *val) {
    bool top_not_0 = val->width > 0 && odin3_cval_bit(val, val->width - 1) != ODIN3_BIT_0;
    bool keeps = (val->flags & ODIN3_CVAL_SIGNED) == 0 && top_not_0;
    return keeps ? val->flags : (uint8_t)(val->flags & ~ODIN3_CVAL_UNSIZED);
}

bool odin3_elab1_same_value(const odin3_cval *lhs, const odin3_cval *rhs) {
    if (lhs->width != rhs->width || odin3_elab1_key_flags(lhs) != odin3_elab1_key_flags(rhs)) {
        return false;
    }
    size_t bytes = (size_t)odin3_cval_words(lhs->width) * sizeof(uint64_t);
    return bytes == 0 ||
           (memcmp(lhs->aval, rhs->aval, bytes) == 0 && memcmp(lhs->bval, rhs->bval, bytes) == 0);
}

/* A value's width, key flags and planes. */
static odin3_status key_value(odin3_strbuf *key, const odin3_cval *val) {
    odin3_status st = key_u32(key, val->width);
    uint8_t flags = odin3_elab1_key_flags(val);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(key, (odin3_bytes){&flags, sizeof flags});
    size_t bytes = (size_t)odin3_cval_words(val->width) * sizeof(uint64_t);
    if (bytes > 0) {
        st = st != ODIN3_OK ? st : odin3_strbuf_append(key, (odin3_bytes){val->aval, bytes});
        st = st != ODIN3_OK ? st : odin3_strbuf_append(key, (odin3_bytes){val->bval, bytes});
    }
    return st;
}

typedef struct e1_keyed {
    uint32_t def;
    const odin3_cval *values;
    uint32_t n_values;
    const odin3_elab_override *pending;
    uint32_t n_pending;
} e1_keyed;

static odin3_status build_key(const odin3_elab1 *e1, const e1_keyed *what, odin3_strbuf *key) {
    odin3_status st = key_u32(key, what->def);
    st = st != ODIN3_OK ? st : key_u32(key, what->n_values);
    for (uint32_t k = 0; st == ODIN3_OK && k < what->n_values; k++) {
        st = key_value(key, &what->values[k]);
    }
    st = st != ODIN3_OK ? st : key_u32(key, what->n_pending);
    for (uint32_t i = 0; st == ODIN3_OK && i < what->n_pending; i++) {
        const odin3_elab_override *ovr = &what->pending[i];
        const char *path = odin3_elab1_str(e1, ovr->path);
        st = key_u32(key, (uint32_t)strlen(path));
        st = st != ODIN3_OK ? st : odin3_strbuf_append(key, odin3_bytes_cstr(path));
        st = st != ODIN3_OK ? st : odin3_strbuf_append(key, (odin3_bytes){&ovr->source, 1});
        st = st != ODIN3_OK ? st : key_value(key, &ovr->value);
    }
    return st;
}

/* --- pending overrides: one per path, sorted by path ------------------------------------- */

/* lhs before rhs: by path text, then precedence (EL-8). */
static bool goes_before(const odin3_elab1 *e1, const odin3_elab_override *lhs,
                        const odin3_elab_override *rhs) {
    int cmp = strcmp(odin3_elab1_str(e1, lhs->path), odin3_elab1_str(e1, rhs->path));
    if (cmp != 0) {
        return cmp < 0;
    }
    return lhs->source != rhs->source ? lhs->source < rhs->source : lhs->order < rhs->order;
}

/* OK for a block that was allocated, NO_MEMORY for NULL. */
static odin3_status have(const void *ptr) {
    return ptr != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Sorted by path (an insertion sort: pending lists are short), the override of highest
 * precedence kept per path (the others can never win at the target). */
static uint32_t reduce_pending(const odin3_elab1 *e1, odin3_elab_override *pending,
                               uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        odin3_elab_override cur = pending[i];
        uint32_t j = i;
        while (j > 0 && goes_before(e1, &cur, &pending[j - 1])) {
            pending[j] = pending[j - 1];
            j--;
        }
        pending[j] = cur;
    }
    uint32_t kept = count > 0 ? 1 : 0;
    for (uint32_t i = 1; i < count; i++) {
        if (pending[i].path != pending[kept - 1].path) {
            pending[kept++] = pending[i];
        }
    }
    return kept;
}

/* --- names (EL-9, point (q)) ---------------------------------------------------------------- */

/* True when text names a definition of another name, a specialization made so far or a
 * registered cell type (but a `(* blackbox *)` definition's own name: IR-7b reuses that type). */
static bool name_taken(const odin3_elab1 *e1, const odin3_elab_def *def, odin3_bytes text) {
    uint32_t id = 0;
    if (!odin3_strtab_find(odin3_design_strtab(e1->design), text, &id)) {
        return false;
    }
    odin3_celltype_id type;
    bool reused = def->kind == ODIN3_DEF_BLACKBOX && id == def->name;
    return odin3_u64map_get(e1->spec_names, id, NULL) ||
           (!reused && odin3_celltype_find(e1->design, id, &type)) ||
           (id != def->name && odin3_u64map_get(e1->by_name, id, NULL));
}

/* An override path as names print it: its components joined by '.'. */
static odin3_status append_path(const odin3_elab1 *e1, uint32_t path, odin3_strbuf *out) {
    const char *text = odin3_elab1_str(e1, path);
    size_t from = out->len;
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(text));
    if (st == ODIN3_OK) {
        odin3_elab1_path_print(out->data + from, out->len - from);
    }
    return st;
}

/* base$P=v for each parameter that differs from its default, then $path=v per pending (the top:
 * its definition's name, v4.7). */
static odin3_status spell(const odin3_elab1 *e1, const odin3_elab1_params_out *res,
                          const odin3_elab_def *def, odin3_strbuf *out) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(odin3_elab1_str(e1, def->name)));
    uint32_t count = def->n_header_params + def->n_body_params;
    if (e1->specs.len == 0) {
        return st; /* the top keeps its definition's name (Yosys chparam, hierarchy -chparam) */
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        if (!res->differ[k]) {
            continue;
        }
        odin3_ast_id decl = *(const odin3_ast_id *)odin3_vec_cat(&e1->params, def->first_param + k);
        st = odin3_strbuf_appendf(out, "$%s=", odin3_ast_name_str(e1->parsed, decl));
        st = st != ODIN3_OK ? st : odin3_cval_print(&res->values[k], out);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < res->n_pending; i++) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("$"));
        st = st != ODIN3_OK ? st : append_path(e1, res->pending[i].path, out);
        st = st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("="));
        st = st != ODIN3_OK ? st : odin3_cval_print(&res->pending[i].value, out);
    }
    return st;
}

/* The name of a new specialization: spelled, hashed past the length cap, then `$2`, `$3`, … until
 * free; interned and recorded. */
static odin3_status spec_name(odin3_elab1 *e1, const odin3_elab1_params_out *res,
                              const odin3_elab_def *def, uint64_t hash, uint32_t *name) {
    odin3_strbuf base;
    odin3_strbuf_init(&base);
    odin3_status st = spell(e1, res, def, &base);
    if (st == ODIN3_OK && base.len > ODIN3_ELAB_MAX_MODULE_NAME) {
        odin3_strbuf_clear(&base);
        st = odin3_strbuf_appendf(&base, "%s$h%016" PRIx64, odin3_elab1_str(e1, def->name), hash);
    }
    odin3_strbuf cand;
    odin3_strbuf_init(&cand);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(&cand, (odin3_bytes){base.data, base.len});
    for (uint32_t suffix = E1_FIRST_SUFFIX;
         st == ODIN3_OK && name_taken(e1, def, (odin3_bytes){cand.data, cand.len}); suffix++) {
        odin3_strbuf_clear(&cand);
        st = odin3_strbuf_appendf(&cand, "%s$%" PRIu32, base.data, suffix);
    }
    st = st != ODIN3_OK ? st
                        : odin3_design_intern(e1->design, (odin3_bytes){cand.data, cand.len}, name);
    st = st != ODIN3_OK ? st : odin3_u64map_put(e1->spec_names, (odin3_kv){*name, 1});
    odin3_strbuf_free(&base);
    odin3_strbuf_free(&cand);
    return st;
}

/* --- the table ------------------------------------------------------------------------------ */

const odin3_elab1_spec *odin3_elab1_spec_at(const odin3_elab1 *e1, uint32_t spec) {
    return spec < e1->specs.len ? odin3_vec_cat(&e1->specs, spec) : NULL;
}

/* The specialization with key bytes, by its hash's chain; false when none. */
static bool find_spec(const odin3_elab1 *e1, uint64_t hash, odin3_bytes key, uint32_t *spec) {
    uint64_t head = 0;
    if (!odin3_u64map_get(e1->spec_hash, hash, &head)) {
        return false;
    }
    for (uint32_t at = (uint32_t)head; at != 0;) {
        const odin3_elab1_spec *rec = odin3_elab1_spec_at(e1, at - 1);
        if (rec->key_len == key.len && memcmp(rec->key, key.ptr, key.len) == 0) {
            *spec = at - 1;
            return true;
        }
        at = rec->next_same_hash;
    }
    return false;
}

/* Copies of the results the specialization keeps, in the run arena. */
static odin3_status keep(odin3_elab1 *e1, const odin3_elab1_params_out *res, uint32_t count,
                         odin3_elab1_spec *rec) {
    odin3_cval *values =
        odin3_arena_alloc(e1->arena, (size_t)(count > 0 ? count : 1) * sizeof *values);
    uint8_t *bases = odin3_arena_alloc(e1->arena, count > 0 ? count : 1);
    odin3_elab_override *pending = odin3_arena_alloc(
        e1->arena, (size_t)(res->n_pending > 0 ? res->n_pending : 1) * sizeof *pending);
    odin3_status st =
        values != NULL && bases != NULL && pending != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        bases[k] = res->bases[k];
        st = odin3_cval_copy(e1->arena, &res->values[k], &values[k]);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < res->n_pending; i++) {
        pending[i] = res->pending[i];
        st = res->pending[i].value.width > 0
                 ? odin3_cval_copy(e1->arena, &res->pending[i].value, &pending[i].value)
                 : ODIN3_OK;
    }
    rec->values = values;
    rec->bases = bases;
    rec->n_values = count;
    rec->pending = pending;
    rec->n_pending = res->n_pending;
    return st;
}

/* Point (aa)'s caps before a new specialization: the count, then the depth (both fatal). */
static odin3_status check_caps(odin3_elab1 *e1, const odin3_elab_spec_req *req) {
    const odin3_elab1_creating *at = &e1->creating;
    if (e1->specs.len >= e1->limits->max_specializations) {
        odin3_elab1_fatal(e1, odin3_ast_loc(e1->parsed, at->holder),
                          "elaboration makes more than %" PRIu32 " specializations",
                          e1->limits->max_specializations);
        return ODIN3_ERR_PARSE;
    }
    if (req->depth > e1->limits->max_hier_depth) {
        odin3_elab1_fatal(e1, odin3_ast_loc(e1->parsed, at->inst),
                          "instance hierarchy deeper than %" PRIu32
                          " at '%s'; recursive instantiation of '%s'?",
                          e1->limits->max_hier_depth, odin3_ast_name_str(e1->parsed, at->inst),
                          odin3_elab1_str(e1, odin3_elab1_def(e1, at->parent_def)->name));
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* A new specialization of req with the key and results (FIFO: appended). */
static odin3_status create_spec(odin3_elab1 *e1, const odin3_elab_spec_req *req,
                                const odin3_elab1_params_out *res, odin3_bytes key,
                                uint32_t *spec) {
    const odin3_elab_def *def = odin3_elab1_def(e1, req->def);
    odin3_elab1_spec rec = {
        .def = req->def, .depth = req->depth, .hash = odin3_hash_bytes(key, ODIN3_HASH_SEED)};
    odin3_status st = check_caps(e1, req);
    st = st != ODIN3_OK ? st : keep(e1, res, def->n_header_params + def->n_body_params, &rec);
    uint8_t *bytes =
        st == ODIN3_OK ? odin3_arena_alloc(e1->arena, key.len > 0 ? key.len : 1) : NULL;
    st = st != ODIN3_OK ? st : have(bytes);
    if (st == ODIN3_OK) {
        memcpy(bytes, key.ptr, key.len);
        rec.key = bytes;
        rec.key_len = (uint32_t)key.len;
        st = spec_name(e1, res, def, rec.hash, &rec.name);
    }
    uint64_t head = 0;
    rec.next_same_hash = odin3_u64map_get(e1->spec_hash, rec.hash, &head) ? (uint32_t)head : 0;
    odin3_elab1_spec *slot = st == ODIN3_OK ? odin3_vec_push(&e1->specs) : NULL;
    st = st != ODIN3_OK ? st : have(slot);
    if (st != ODIN3_OK) {
        return st;
    }
    *slot = rec;
    *spec = (uint32_t)e1->specs.len - 1;
    st = odin3_u64map_put(e1->spec_hash, (odin3_kv){rec.hash, e1->specs.len});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&e1->specs);
    }
    return st;
}

odin3_status odin3_elab1_spec_get(odin3_elab1 *e1, const odin3_elab_spec_req *req, uint32_t *spec) {
    odin3_elab1_params_out res;
    odin3_status st = odin3_elab1_params_ex(e1, req, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    res.n_pending = reduce_pending(e1, res.pending, res.n_pending);
    const odin3_elab_def *def = odin3_elab1_def(e1, req->def);
    e1_keyed what = {req->def, res.values, def->n_header_params + def->n_body_params, res.pending,
                     res.n_pending};
    odin3_strbuf key;
    odin3_strbuf_init(&key);
    st = build_key(e1, &what, &key);
    odin3_bytes bytes = {key.data, key.len};
    if (st == ODIN3_OK && !find_spec(e1, odin3_hash_bytes(bytes, ODIN3_HASH_SEED), bytes, spec)) {
        st = create_spec(e1, req, &res, bytes, spec);
    }
    odin3_strbuf_free(&key);
    return st;
}

/* --- the project's parameters (EL-8 item 1) ------------------------------------------------- */

/* A number's three pieces from a project value ("16", "8'hFF", "'sb101"), after a '-'. */
static odin3_number_text number_pieces(const char *text) {
    odin3_number_text pieces = {{text, 0}, {"", 0}, {text, strlen(text)}};
    const char *tick = strchr(text, '\'');
    if (tick == NULL) {
        return pieces;
    }
    const char *letter = tick + 1;
    letter += *letter == 's' || *letter == 'S' ? 1 : 0;
    const char *digits = *letter != '\0' ? letter + 1 : letter;
    pieces.size = (odin3_bytes){text, (size_t)(tick - text)};
    pieces.base = (odin3_bytes){tick, (size_t)(digits - tick)};
    pieces.digits = (odin3_bytes){digits, strlen(digits)};
    return pieces;
}

/* A parsed NUMBER node in a scratch store, its value read as a literal (Yosys's sizing). */
static odin3_status literal_of(odin3_elab1 *e1, odin3_number_text pieces, odin3_loc at,
                               odin3_elab_override *ovr) {
    odin3_ast *tmp = NULL;
    odin3_status st = odin3_ast_create(e1->design, ODIN3_AST_FORM_PARSED, e1->ctx->run, &tmp);
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .loc = at.v};
    st = st != ODIN3_OK ? st : odin3_ast_number_kind(pieces, &spec.sub, &spec.flags);
    st = st != ODIN3_OK ? st : odin3_ast_number_parse(tmp, at, pieces, &spec.payload);
    odin3_ast_id node = {0};
    st = st != ODIN3_OK ? st : odin3_ast_make(tmp, &spec, NULL, 0, &node);
    odin3_ast_literal lit;
    st = st != ODIN3_OK ? st : odin3_ast_number_value(tmp, node, e1->arena, &lit);
    st = st != ODIN3_OK ? st : odin3_cval_from_literal(e1->arena, &lit, &ovr->value);
    ovr->base = spec.sub;
    odin3_ast_destroy(tmp);
    return st;
}

/* A project value: a number (`-` negates it as an operator would) or a string. */
static odin3_status project_value(odin3_elab1 *e1, const odin3_proj_param *param,
                                  odin3_elab_override *ovr) {
    const char *text = odin3_elab1_str(e1, param->value);
    if (param->type == ODIN3_PARAM_STRING) {
        ovr->base = ODIN3_AST_NUMBER_BIN;
        return odin3_cval_from_string(e1->arena, odin3_bytes_cstr(text), &ovr->value);
    }
    bool negative = text[0] == '-';
    odin3_status st = literal_of(e1, number_pieces(text + (negative ? 1 : 0)), param->at, ovr);
    if (st == ODIN3_OK && negative) {
        odin3_cval magnitude = ovr->value;
        odin3_cop_args neg = {ODIN3_COP_NEG, &magnitude, NULL, magnitude.width,
                              (magnitude.flags & ODIN3_CVAL_SIGNED) != 0};
        st = odin3_cval_op(e1->arena, &neg, &ovr->value);
    }
    ovr->value.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED; /* an override is sized (v4.7) */
    return st;
}

/* Project line i (`param top[.inst…].name value`) as an override of the top: its path below
 * the top ("name" or "inst….name"; the reader checked that the scope starts at the top). */
static odin3_status project_override(odin3_elab1 *e1, size_t idx, odin3_elab_override *ovr) {
    const odin3_proj_param *param = odin3_vec_cat(&odin3_design_project(e1->design)->params, idx);
    const char *below = strchr(odin3_elab1_str(e1, param->scope), '.');
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = below != NULL
                          ? odin3_strbuf_appendf(&path, "%s%c", below + 1, ODIN3_ELAB_PATH_SEP)
                          : ODIN3_OK;
    if (st == ODIN3_OK && path.len > 0) {
        odin3_elab1_path_join(path.data, path.len);
    }
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_append(&path, odin3_bytes_cstr(odin3_elab1_str(e1, param->name)));
    *ovr =
        (odin3_elab_override){.source = ODIN3_OVR_PROJECT, .order = (uint32_t)idx, .at = param->at};
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(e1->design, (odin3_bytes){path.data, path.len}, &ovr->path);
    odin3_strbuf_free(&path);
    return st != ODIN3_OK ? st : project_value(e1, param, ovr);
}

/* The project's `param` lines as the top's overrides (EL-8 item 1). */
static odin3_status project_overrides(odin3_elab1 *e1, odin3_vec *out) {
    const odin3_project *proj = odin3_design_project(e1->design);
    size_t count = proj != NULL && proj->finished ? proj->params.len : 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        odin3_elab_override ovr;
        st = project_override(e1, i, &ovr);
        odin3_elab_override *slot = st == ODIN3_OK ? odin3_vec_push(out) : NULL;
        st = st != ODIN3_OK ? st : have(slot);
        if (slot != NULL) {
            *slot = ovr;
        }
    }
    return st;
}

/* The top's specialization (spec 0), with the project's parameters. */
static odin3_status top_spec(odin3_elab1 *e1, uint32_t top_def) {
    odin3_vec ovr;
    odin3_vec_init(&ovr, sizeof(odin3_elab_override));
    odin3_status st = project_overrides(e1, &ovr);
    odin3_elab_spec_req req = {top_def, ovr.data, (uint32_t)ovr.len, 0};
    e1->creating = (odin3_elab1_creating){{0}, {0}, top_def};
    uint32_t spec = 0;
    st = st != ODIN3_OK ? st : odin3_elab1_spec_get(e1, &req, &spec);
    odin3_vec_free(&ovr);
    return st == ODIN3_ERR_PARSE ? ODIN3_OK : st;
}

/* --- the acyclicity check (iterative DFS) --------------------------------------------------- */

typedef struct e1_visit {
    uint32_t spec;
    uint32_t next; /* the next edge to follow */
} e1_visit;

/* "recursive instantiation of 'a' (through 'b', 'c')" for the cycle closed by edge, the stack
 * holding it from spec `to` up. */
static odin3_status report_cycle(odin3_elab1 *e1, const odin3_vec *stack,
                                 const odin3_elab1_edge *edge) {
    odin3_strbuf through;
    odin3_strbuf_init(&through);
    odin3_status st = ODIN3_OK;
    size_t from = 0;
    while (((const e1_visit *)odin3_vec_cat(stack, from))->spec != edge->to) {
        from++;
    }
    for (size_t i = from + 1; st == ODIN3_OK && i < stack->len; i++) {
        uint32_t spec = ((const e1_visit *)odin3_vec_cat(stack, i))->spec;
        st = odin3_strbuf_appendf(&through, "%s'%s'", i > from + 1 ? ", " : "",
                                  odin3_elab1_str(e1, odin3_elab1_spec_at(e1, spec)->name));
    }
    const char *name = odin3_elab1_str(e1, odin3_elab1_spec_at(e1, edge->to)->name);
    if (st == ODIN3_OK && through.len > 0) {
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, edge->inst),
                          "recursive instantiation of '%s' (through %s)", name, through.data);
    } else if (st == ODIN3_OK) {
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, edge->inst),
                          "recursive instantiation of '%s'", name);
    }
    odin3_strbuf_free(&through);
    return st;
}

/* One DFS step from the top of the stack; *done when a cycle was reported. */
static odin3_status dfs_step(odin3_elab1 *e1, odin3_vec *stack, uint8_t *color, bool *done) {
    e1_visit *top = odin3_vec_at(stack, stack->len - 1);
    const odin3_elab1_spec *rec = odin3_elab1_spec_at(e1, top->spec);
    if (top->next >= rec->n_edges) {
        color[top->spec] = E1_BLACK;
        odin3_vec_pop(stack);
        return ODIN3_OK;
    }
    const odin3_elab1_edge *edge = odin3_vec_cat(&e1->edges, rec->first_edge + top->next++);
    if (color[edge->to] == E1_GRAY) {
        *done = true;
        return report_cycle(e1, stack, edge);
    }
    if (color[edge->to] == E1_BLACK) {
        return ODIN3_OK;
    }
    color[edge->to] = E1_GRAY;
    e1_visit *slot = odin3_vec_push(stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (e1_visit){edge->to, 0};
    return ODIN3_OK;
}

static odin3_status check_acyclic(odin3_elab1 *e1) {
    if (e1->specs.len == 0) {
        return ODIN3_OK;
    }
    uint8_t *color = odin3_util_calloc(e1->specs.len);
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(e1_visit));
    e1_visit *root = color != NULL ? odin3_vec_push(&stack) : NULL;
    odin3_status st = root != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (root != NULL) {
        *root = (e1_visit){0, 0};
        color[0] = E1_GRAY;
    }
    bool done = false;
    while (st == ODIN3_OK && !done && stack.len > 0) {
        st = dfs_step(e1, &stack, color, &done);
    }
    odin3_vec_free(&stack);
    odin3_util_free(color);
    return st;
}

/* --- the queue (§3.4) ---------------------------------------------------------------------- */

/* The directives of the design UNITs (default_nettype, timescale, …): Task 16's. */
static void pend_unit_directives(odin3_elab1 *e1) {
    uint32_t from = odin3_ast_library_units_from(e1->parsed);
    uint32_t units = from != 0 ? from : odin3_ast_root_count(e1->parsed);
    for (uint32_t idx = 0; idx < units && !e1->stopped; idx++) {
        odin3_ast_span kids = odin3_ast_children(e1->parsed, odin3_ast_root(e1->parsed, idx));
        for (uint32_t i = 0; i < kids.n && !e1->stopped; i++) {
            if (odin3_ast_kind_of(e1->parsed, kids.ids[i]) == ODIN3_AST_DIRECTIVE) {
                odin3_elab1_not_yet(e1, kids.ids[i]);
            }
        }
    }
}

/* Passes E and W of every specialization in FIFO order (the queue grows as instances are
 * specialized). */
static odin3_status run_specs(odin3_elab1 *e1) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < e1->specs.len && !e1->stopped; i++) {
        uint32_t def = odin3_elab1_spec_at(e1, i)->def;
        if (odin3_elab1_pending(e1, odin3_elab1_def(e1, def)->node)) {
            continue; /* a MODULE forced pending (point (l)'s hook) */
        }
        if (odin3_elab1_def(e1, def)->kind == ODIN3_DEF_BLACKBOX) {
            odin3_status bb = odin3_elab1_blackbox(e1, i); /* a ports-only MODULE (EL-4 v4.8) */
            st = bb == ODIN3_ERR_PARSE || bb == ODIN3_OK ? e1->fatal : bb;
            continue;
        }
        st = odin3_elab1_expand(e1, i);
        st = st != ODIN3_OK ? st : odin3_elab1_write(e1, i);
        st = st != ODIN3_OK ? st : e1->fatal;
    }
    return st;
}

/* The implicit black boxes' MODULEs, after every specialization (EL-4 v4.8); the node cap's
 * fatal error is counted, not returned. */
static odin3_status implicit_modules(odin3_elab1 *e1) {
    odin3_status st = odin3_elab1_write_implicit(e1);
    return st == ODIN3_ERR_PARSE ? ODIN3_OK : st;
}

/* The UNIT named after the top's specialization, its tail the MODULEs left above mark. */
static odin3_status make_unit(odin3_elab1 *e1, uint32_t mark) {
    const odin3_elab1_spec *top = odin3_elab1_spec_at(e1, 0);
    odin3_ast_id node = odin3_elab1_def(e1, top->def)->node;
    odin3_ast_spec spec = {.kind = ODIN3_AST_UNIT,
                           .loc = odin3_ast_loc(e1->parsed, node).v,
                           .end = odin3_ast_end(e1->parsed, node).v,
                           .name = top->name};
    odin3_ast_id unit = {0};
    return odin3_ast_make_marked(e1->elab, &spec, mark, &unit);
}

odin3_status odin3_elab1_run_queue(odin3_elab1 *e1, uint32_t top_def) {
    odin3_status st =
        odin3_ast_create(e1->design, ODIN3_AST_FORM_ELABORATED, e1->ctx->run, &e1->elab);
    if (st != ODIN3_OK) {
        return st;
    }
    pend_unit_directives(e1);
    uint32_t mark = odin3_ast_mark(e1->elab);
    if (odin3_elab1_reach_def(e1, top_def)) {
        st = top_spec(e1, top_def);
    }
    st = st != ODIN3_OK ? st : run_specs(e1);
    st = st != ODIN3_OK || e1->stopped ? st : implicit_modules(e1);
    st = st != ODIN3_OK || e1->stopped ? st : check_acyclic(e1);
    st = st != ODIN3_OK || e1->stopped ? st : odin3_elab1_not_elaborated(e1);
    bool clean = st == ODIN3_OK && e1->fatal == ODIN3_OK && e1->errors == 0 && !e1->stopped &&
                 e1->specs.len > 0;
    if (clean) {
        st = make_unit(e1, mark);
    } else {
        odin3_ast_unwind(e1->elab, mark);
    }
    return st;
}
