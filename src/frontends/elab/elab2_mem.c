/*
 * elab2_mem.c — stage 2's memories, the decision (2D Task 20; EL-27): the table of the module's
 * arrays, Yosys 0.55's mem2reg_as_needed rules over the flags elab2_memscan.c gathers, the warning
 * and the --nomem2reg error, the memory-bits cap, the words of an array that became registers and
 * the lookup of a word by its constant indices. The ports and the `$mem` cell are elab2_memport.c.
 */
#include "frontends/elab/elab2_mem.h"
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- the table ------------------------------------------------------------------------------- */

const odin3_elab_mem *odin3_elab2_mem_get(const odin3_elab2 *e2, uint32_t mem) {
    return e2 != NULL && mem < e2->mems.len ? odin3_vec_cat(&e2->mems, mem) : NULL;
}

odin3_elab_mem *odin3_elab2_mem_at(odin3_elab2 *e2, uint32_t mem) {
    return mem < e2->mems.len ? odin3_vec_at(&e2->mems, mem) : NULL;
}

uint32_t odin3_elab2_mem_find(const odin3_elab2 *e2, odin3_sym_id sym) {
    size_t lo = 0;
    size_t hi = e2->mems.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const odin3_elab_mem *mem = odin3_vec_cat(&e2->mems, mid);
        if (mem->sym.v == sym.v) {
            return (uint32_t)mid;
        }
        if (mem->sym.v < sym.v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return MEM_NONE;
}

static void mem_free_one(odin3_elab_mem *mem) {
    odin3_vec_free(&mem->rd_ports);
    odin3_vec_free(&mem->wr_ports);
    odin3_vec_free(&mem->words);
}

void odin3_elab2_mem_reset(odin3_elab2 *e2) {
    for (size_t i = 0; i < e2->mems.len; i++) {
        mem_free_one(odin3_vec_at(&e2->mems, i));
    }
    odin3_vec_clear(&e2->mems);
}

void odin3_elab2_mem_free(odin3_elab2 *e2) {
    odin3_elab2_mem_reset(e2);
    odin3_vec_free(&e2->mems);
}

static int mem_compare(const void *lhs, const void *rhs) {
    uint32_t left = ((const odin3_elab_mem *)lhs)->sym.v;
    uint32_t right = ((const odin3_elab_mem *)rhs)->sym.v;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* --- geometry and the cap ----------------------------------------------------------------------
 */

static uint64_t mul_saturating(uint64_t lhs, uint64_t rhs) {
    return rhs != 0 && lhs > UINT64_MAX / rhs ? UINT64_MAX : lhs * rhs;
}

uint64_t odin3_elab2_dim_size(const odin3_dim *dim) {
    int64_t span = (int64_t)dim->msb - (int64_t)dim->lsb;
    return (uint64_t)(span < 0 ? -span : span) + 1U;
}

int32_t odin3_elab2_dim_low(const odin3_dim *dim) {
    return dim->msb < dim->lsb ? dim->msb : dim->lsb;
}

/* The array's width, size, offset and flatness from its declaration; returns SIZE x WIDTH bits
 * (saturated). */
static uint64_t mem_geometry(const odin3_elab2 *e2, const odin3_symbol *rec, odin3_elab_mem *mem) {
    odin3_dim_span dims = odin3_symbol_unpacked(e2->st, mem->sym);
    uint64_t size = 1;
    for (uint32_t k = 0; k < dims.n; k++) {
        size = mul_saturating(size, odin3_elab2_dim_size(&dims.dims[k]));
    }
    uint64_t width = rec->has_packed ? odin3_elab2_dim_size(&rec->packed) : 1U;
    mem->width = (uint32_t)(width > UINT32_MAX ? UINT32_MAX : width);
    mem->size = (uint32_t)(size > UINT32_MAX ? UINT32_MAX : size);
    mem->flat = dims.n > 1;
    mem->offset = dims.n == 1 ? odin3_elab2_dim_low(&dims.dims[0]) : 0;
    return mul_saturating(size, width);
}

static void mem_init(odin3_elab_mem *mem, odin3_sym_id sym, const odin3_symbol *rec) {
    *mem = (odin3_elab_mem){.sym = sym,
                            .abits = 1,
                            .n_dims = rec->n_unpacked,
                            .is_signed = (rec->flags & ODIN3_SYM_F_SIGNED) != 0};
    mem->init = (odin3_value){.kind = ODIN3_VAL_BITS};
    odin3_vec_init(&mem->rd_ports, sizeof(odin3_elab_mem_rd_port));
    odin3_vec_init(&mem->wr_ports, sizeof(odin3_elab_mem_wr_port));
    odin3_vec_init(&mem->words, sizeof(odin3_wire_id));
}

/* One array onto the table: its geometry, its name, and the memory-bits cap (fatal). */
static odin3_status mem_add(odin3_elab2 *e2, odin3_sym_id sym, const odin3_symbol *rec) {
    odin3_elab_mem mem;
    mem_init(&mem, sym, rec);
    uint64_t bits = mem_geometry(e2, rec, &mem);
    odin3_status st = odin3_elab2_scoped_name(e2, rec, &mem.name);
    if (st == ODIN3_OK && bits > e2->limits->max_memory_bits) {
        odin3_elab2_fatal(e2, odin3_ast_loc(e2->elab, rec->declarator),
                          "memory '%s' of %" PRIu64 " bits exceeds the limit of %" PRIu64 " bits",
                          odin3_strtab_get(odin3_design_strtab(e2->design), mem.name), bits,
                          e2->limits->max_memory_bits);
        st = ODIN3_ERR_PARSE;
    }
    odin3_elab_mem *slot = st == ODIN3_OK ? odin3_vec_push(&e2->mems) : NULL;
    if (slot == NULL) {
        mem_free_one(&mem);
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = mem;
    return ODIN3_OK;
}

/* --- finding the arrays ------------------------------------------------------------------------
 */

/* The arrays of one scope: a net or variable with unpacked dimensions that has no wire. */
static odin3_status mem_scope(odin3_elab2 *e2, odin3_scope_id scope) {
    odin3_status st = ODIN3_OK;
    for (odin3_sym_id sym = odin3_scope_first_symbol(e2->st, scope); st == ODIN3_OK && sym.v != 0;
         sym = odin3_scope_next_symbol(e2->st, sym)) {
        const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
        bool variable = rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR;
        if (variable && rec->n_unpacked > 0 && rec->ir_wire.v == 0) {
            st = mem_add(e2, sym, rec);
        }
    }
    return st;
}

/* The child scopes of scope that are the module's: not a function's or a task's. */
static odin3_status mem_push_scopes(const odin3_elab2 *e2, odin3_vec *todo, odin3_scope_id scope) {
    for (odin3_scope_id kid = odin3_scope_first_child(e2->st, scope); kid.v != 0;
         kid = odin3_scope_next_sibling(e2->st, kid)) {
        uint8_t kind = odin3_scope_get(e2->st, kid)->kind;
        if (kind == ODIN3_SCOPE_FUNCTION || kind == ODIN3_SCOPE_TASK) {
            continue;
        }
        odin3_scope_id *slot = odin3_vec_push(todo);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = kid;
    }
    return ODIN3_OK;
}

/* Every array of the module's scopes (a worklist, no recursion), in symbol order. */
static odin3_status mem_collect(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(odin3_scope_id));
    odin3_scope_id *root = odin3_vec_push(&todo);
    odin3_status st = root != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (root != NULL) {
        *root = odin3_elab_scope_of(&e2->bind, module_node);
    }
    while (st == ODIN3_OK && todo.len > 0) {
        odin3_scope_id scope = *(const odin3_scope_id *)odin3_vec_cat(&todo, todo.len - 1);
        odin3_vec_pop(&todo);
        st = mem_scope(e2, scope);
        st = st != ODIN3_OK ? st : mem_push_scopes(e2, &todo, scope);
    }
    odin3_vec_free(&todo);
    if (st == ODIN3_OK && e2->mems.len > 1) {
        qsort(e2->mems.data, e2->mems.len, sizeof(odin3_elab_mem), mem_compare);
    }
    return st;
}

/* --- attributes --------------------------------------------------------------------------------
 */

/* The attribute names the rules read (0 when the design never interned one: no node has it). */
typedef struct mem_keys {
    uint32_t mem2reg, nomem2reg;
    bool module_forces, module_exempts;
} mem_keys;

static uint32_t mem_key(const odin3_elab2 *e2, const char *text) {
    uint32_t key = 0;
    return odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(text), &key) ? key
                                                                                            : 0;
}

/* Whether node carries the boolean attribute key (get_bool_attribute: no value, or a non-zero
 * constant; the last of a key wins). */
static odin3_status attr_true(odin3_elab2 *e2, odin3_ast_id node, uint32_t key, bool *out) {
    *out = false;
    if (key == 0 || node.v == 0) {
        return ODIN3_OK;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (odin3_ast_name(e2->elab, attrs.ids[i]) != key) {
            continue;
        }
        odin3_ast_id arg = odin3_ast_child(e2->elab, attrs.ids[i], 0);
        *out = true;
        if (odin3_ast_kind_of(e2->elab, arg) == ODIN3_AST_NUMBER) {
            int64_t num = 0;
            bool known = false;
            odin3_status st = odin3_elab2_number_i64(e2, arg, &num, &known);
            if (st != ODIN3_OK) {
                return st;
            }
            *out = known && num != 0;
        }
    }
    return ODIN3_OK;
}

/* Whether the array's declaration or declarator carries key. */
static odin3_status array_attr(odin3_elab2 *e2, const odin3_symbol *rec, uint32_t key, bool *out) {
    bool on_decl = false;
    bool on_declarator = false;
    odin3_status st = attr_true(e2, rec->decl, key, &on_decl);
    st = st != ODIN3_OK ? st : attr_true(e2, rec->declarator, key, &on_declarator);
    *out = on_decl || on_declarator;
    return st;
}

static odin3_status mem_module_keys(odin3_elab2 *e2, odin3_ast_id module_node, mem_keys *keys) {
    keys->mem2reg = mem_key(e2, "mem2reg");
    keys->nomem2reg = mem_key(e2, "nomem2reg");
    odin3_status st = attr_true(e2, module_node, keys->mem2reg, &keys->module_forces);
    return st != ODIN3_OK ? st : attr_true(e2, module_node, keys->nomem2reg, &keys->module_exempts);
}

/* --- the warning and the error -----------------------------------------------------------------
 */

/* "file:line" of loc, the column of the srcman's text cut off (Yosys names lines). */
static odin3_status place_text(const odin3_elab2 *e2, odin3_loc loc, odin3_strbuf *one) {
    odin3_srcman *sm = NULL;
    odin3_strbuf_clear(one);
    odin3_status st = odin3_design_get_srcman(e2->design, &sm);
    st = st != ODIN3_OK ? st
                        : odin3_srcman_format(sm, loc, one, odin3_design_diag_paths(e2->design));
    while (st == ODIN3_OK && one->len > 0 && one->data[one->len - 1] != ':') {
        one->len--;
    }
    if (st == ODIN3_OK && one->len > 0) {
        one->len--;
        one->data[one->len] = '\0';
    }
    return st;
}

/* Whether text is already on the list. */
static bool place_seen(const odin3_vec *texts, const char *text) {
    for (size_t i = 0; i < texts->len; i++) {
        if (strcmp(*(const char *const *)odin3_vec_cat(texts, i), text) == 0) {
            return true;
        }
    }
    return false;
}

/* Adds a copy of one (in the scratch arena) to the list. */
static odin3_status place_keep(odin3_elab2 *e2, odin3_vec *texts, const odin3_strbuf *one) {
    const char **slot = (const char **)odin3_vec_push(texts);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = odin3_arena_strndup(e2->scratch, one->data != NULL ? one->data : "", one->len);
    if (*slot == NULL) {
        odin3_vec_pop(texts);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* The distinct place texts of fact, in insertion order, copied to the scratch arena. */
static odin3_status place_list(odin3_elab2 *e2, const odin3_mem_fact *fact, odin3_vec *texts) {
    odin3_strbuf one;
    odin3_strbuf_init(&one);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < fact->places.len; i++) {
        st = place_text(e2, *(const odin3_loc *)odin3_vec_cat(&fact->places, i), &one);
        if (st == ODIN3_OK && !place_seen(texts, one.data != NULL ? one.data : "")) {
            st = place_keep(e2, texts, &one);
        }
    }
    odin3_strbuf_free(&one);
    return st;
}

/* `replacing memory 'm' with list of registers; see f.v:12, f.v:4`: Yosys's pool iterates the
 * places newest first. */
static odin3_status mem_warn(odin3_elab2 *e2, const odin3_elab_mem *mem, const odin3_mem_fact *fact,
                             const odin3_symbol *rec) {
    odin3_vec texts;
    odin3_vec_init(&texts, sizeof(const char *));
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    odin3_status st = place_list(e2, fact, &texts);
    for (size_t i = texts.len; st == ODIN3_OK && i > 0; i--) {
        const char *text = *(const char *const *)odin3_vec_cat(&texts, i - 1);
        st = odin3_strbuf_appendf(&list, "%s%s", i == texts.len ? "" : ", ", text);
    }
    if (st == ODIN3_OK) {
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, rec->declarator),
                         "replacing memory '%s' with list of registers; see %s",
                         odin3_strtab_get(odin3_design_strtab(e2->design), mem->name),
                         list.data != NULL ? list.data : "");
    }
    odin3_strbuf_free(&list);
    odin3_vec_free(&texts);
    return st;
}

/* --nomem2reg: an array that cannot be a memory (written in a block without exactly one edge
 * event, or by `assign`) is an error at its first such write. */
static void mem_conflict(odin3_elab2 *e2, odin3_elab_mem *mem, const odin3_mem_fact *fact) {
    bool async = (fact->flags & MEM_FL_SET_ASYNC) != 0;
    if (!async && (fact->flags & MEM_FL_CONT) == 0) {
        return;
    }
    odin3_ast_id at = async ? fact->async_node : fact->cont_node;
    mem->failed = true;
    odin3_elab2_error(e2, odin3_ast_loc(e2->elab, at),
                      "array '%s' cannot be a memory (it is written in a combinational block) and "
                      "--nomem2reg is set",
                      odin3_strtab_get(odin3_design_strtab(e2->design), mem->name));
}

/* --- registers per word ------------------------------------------------------------------------
 */

/* The name of word pos: the array's, and the declared index (a flattened array's position). */
static odin3_status word_name(odin3_elab2 *e2, const odin3_elab_mem *mem, uint32_t pos,
                              uint32_t *name) {
    int64_t declared = mem->flat ? (int64_t)pos : (int64_t)mem->offset + (int64_t)pos;
    odin3_strbuf_clear(&e2->text);
    odin3_status st = odin3_strbuf_append(
        &e2->text, odin3_bytes_cstr(odin3_strtab_get(odin3_design_strtab(e2->design), mem->name)));
    st = st != ODIN3_OK ? st : odin3_strbuf_appendf(&e2->text, "[%" PRId64 "]", declared);
    return st != ODIN3_OK
               ? st
               : odin3_design_intern(e2->design, (odin3_bytes){e2->text.data, e2->text.len}, name);
}

/* What every word of one array shares. */
typedef struct word_ctx {
    odin3_elab_mem *mem;
    const odin3_symbol *rec;
    odin3_prov_id prov;
} word_ctx;

/* The wire of word pos (0 after a located error: the name is taken). */
static odin3_status word_wire(odin3_elab2 *e2, const word_ctx *wc, uint32_t pos,
                              odin3_wire_id *wire) {
    odin3_loc loc = odin3_ast_loc(e2->elab, wc->rec->declarator);
    odin3_wire_spec spec = {.msb = wc->rec->has_packed ? wc->rec->packed.msb : 0,
                            .lsb = wc->rec->has_packed ? wc->rec->packed.lsb : 0,
                            .is_signed = wc->mem->is_signed,
                            .prov = wc->prov};
    *wire = (odin3_wire_id){0};
    odin3_status st = odin3_elab2_reserve_nets(e2, wc->mem->width, loc);
    st = st != ODIN3_OK ? st : word_name(e2, wc->mem, pos, &spec.name);
    if (st == ODIN3_OK && odin3_module_find_wire(e2->mod, spec.name).v != 0) {
        odin3_elab2_error(e2, loc, "'%s' is already declared",
                          odin3_strtab_get(odin3_design_strtab(e2->design), spec.name));
        return ODIN3_OK;
    }
    st = st != ODIN3_OK ? st : odin3_wire_create(e2->mod, &spec, NULL, wire);
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire->v};
    st = st != ODIN3_OK ? st : odin3_elab2_attrs(e2, wc->rec->decl, obj);
    st = st != ODIN3_OK ? st : odin3_elab2_attrs(e2, wc->rec->declarator, obj);
    odin3_elab2_counts made = {0, 0, wc->mem->width, 1};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* The words of an array that became registers: a wire each, named m[k]. */
static odin3_status mem_make_words(odin3_elab2 *e2, odin3_elab_mem *mem, const odin3_symbol *rec) {
    word_ctx wc = {mem, rec, {0}};
    odin3_elab2_prov_req req = {rec->declarator, {0}, rec->scope.v};
    odin3_elab2_nl_extra nl = {rec->decl, NULL};
    odin3_status st = odin3_elab2_prov_nl(e2, &req, &nl, &wc.prov);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < mem->size && !e2->stopped; pos++) {
        odin3_wire_id *slot = odin3_vec_push(&mem->words);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st = word_wire(e2, &wc, pos, slot);
    }
    return st;
}

/* --- the decision ------------------------------------------------------------------------------
 */

/* Yosys's rules 2 to 5 (rule 6 needs nomeminit, which 2D never sets). */
static bool mem_rules_hold(const odin3_mem_fact *fact) {
    uint32_t flags = fact->flags;
    bool constant_only = (flags & MEM_FL_CONST_LHS) != 0 && (flags & MEM_FL_VAR_LHS) == 0;
    return (flags & (MEM_FL_EQ2 | MEM_FL_SET_ASYNC | MEM_FL_CMPLX_LHS)) != 0 || constant_only;
}

/* Rule 1: a net array, or (* mem2reg *) on the array or the module. */
static odin3_status mem_forced(odin3_elab2 *e2, const odin3_symbol *rec, const mem_keys *keys,
                               bool *forced) {
    bool attribute = false;
    odin3_status st = array_attr(e2, rec, keys->mem2reg, &attribute);
    *forced = rec->kind == ODIN3_SYM_NET || attribute || keys->module_forces;
    return st;
}

/* Decides one array: --nomem2reg first, then rule 1, then (* nomem2reg *), then rules 2 to 5. */
static odin3_status mem_resolve(odin3_elab2 *e2, uint32_t index, const odin3_mem_fact *fact,
                                const mem_keys *keys) {
    odin3_elab_mem *mem = odin3_elab2_mem_at(e2, index);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, mem->sym);
    if (e2->mod_nomem2reg) {
        mem_conflict(e2, mem, fact);
        return ODIN3_OK;
    }
    bool forced = false;
    bool exempt = false;
    odin3_status st = mem_forced(e2, rec, keys, &forced);
    st = st != ODIN3_OK ? st : array_attr(e2, rec, keys->nomem2reg, &exempt);
    exempt = exempt || keys->module_exempts;
    if (st != ODIN3_OK) {
        return st;
    }
    if (!forced && (exempt || !mem_rules_hold(fact))) {
        return ODIN3_OK;
    }
    mem->to_regs = true;
    st = forced ? ODIN3_OK : mem_warn(e2, mem, fact, rec);
    return st != ODIN3_OK ? st : mem_make_words(e2, mem, rec);
}

/* One zeroed fact per array. */
static odin3_status mem_facts(const odin3_elab2 *e2, odin3_vec *facts) {
    odin3_vec_init(facts, sizeof(odin3_mem_fact));
    for (size_t i = 0; i < e2->mems.len; i++) {
        odin3_mem_fact *fact = odin3_vec_push(facts);
        if (fact == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *fact = (odin3_mem_fact){0};
        odin3_vec_init(&fact->places, sizeof(odin3_loc));
    }
    return ODIN3_OK;
}

static void mem_facts_free(odin3_vec *facts) {
    for (size_t i = 0; i < facts->len; i++) {
        odin3_vec_free(&((odin3_mem_fact *)odin3_vec_at(facts, i))->places);
    }
    odin3_vec_free(facts);
}

odin3_status odin3_elab2_mem_decide(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_mem_decide: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = mem_collect(e2, module_node);
    if (st != ODIN3_OK || e2->mems.len == 0) {
        return st;
    }
    odin3_vec facts;
    mem_keys keys = {0};
    st = mem_facts(e2, &facts);
    st = st != ODIN3_OK ? st : mem_module_keys(e2, module_node, &keys);
    st = st != ODIN3_OK ? st : odin3_elab2_mem_scan(e2, module_node, &facts);
    for (uint32_t i = 0; st == ODIN3_OK && i < e2->mems.len && !e2->stopped; i++) {
        st = mem_resolve(e2, i, odin3_vec_cat(&facts, i), &keys);
    }
    mem_facts_free(&facts);
    return st;
}

/* --- words by their indices --------------------------------------------------------------------
 */

bool odin3_elab2_mem_word_of(const odin3_elab2 *e2, odin3_ast_id node, odin3_elab2_wordref *ref) {
    const odin3_ast *ast = e2->elab;
    uint32_t depth = 0;
    odin3_ast_id name = node;
    while (odin3_ast_kind_of(ast, name) == ODIN3_AST_SELECT) {
        if (odin3_ast_sub(ast, name) != ODIN3_AST_SELECT_BIT) {
            return false;
        }
        name = odin3_ast_child(ast, name, 0);
        depth++;
    }
    odin3_ast_kind kind = odin3_ast_kind_of(ast, name);
    if (depth == 0 || (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME)) {
        return false;
    }
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, name);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    uint32_t mem =
        rec != NULL && rec->n_unpacked == depth ? odin3_elab2_mem_find(e2, sym) : MEM_NONE;
    if (mem == MEM_NONE) {
        return false;
    }
    *ref = (odin3_elab2_wordref){mem, depth};
    return true;
}

odin3_ast_id odin3_elab2_mem_word_index(const odin3_elab2 *e2, odin3_ast_id node, uint32_t dim) {
    const odin3_ast *ast = e2->elab;
    uint32_t depth = 0;
    for (odin3_ast_id at = node; odin3_ast_kind_of(ast, at) == ODIN3_AST_SELECT;
         at = odin3_ast_child(ast, at, 0)) {
        depth++;
    }
    odin3_ast_id sel = node;
    for (uint32_t steps = depth - 1U - dim; steps > 0; steps--) {
        sel = odin3_ast_child(ast, sel, 0);
    }
    return odin3_ast_child(ast, sel, 1);
}

/* The word position along one dimension of an index (-1: outside the dimension). A
 * one-dimensional array counts from its lowest index, a flattened one from each left bound
 * (Yosys's normalize_index: a descending dimension is reversed). */
static int64_t dim_position(const odin3_elab_mem *mem, const odin3_dim *dim, int64_t idx) {
    int64_t low = odin3_elab2_dim_low(dim);
    int64_t high = low + (int64_t)odin3_elab2_dim_size(dim) - 1;
    if (idx < low || idx > high) {
        return -1;
    }
    if (!mem->flat) {
        return idx - low;
    }
    return idx > dim->msb ? idx - dim->msb : (int64_t)dim->msb - idx;
}

/* The checks every word and port request shares. */
static bool word_request_ok(const odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, bool regs) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rd->mem);
    if (mem == NULL || mem->to_regs != regs || rd->n_idx != mem->n_dims || rd->idx == NULL) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_elab2_mem: a word request needs a register array, a port request a "
                  "memory, and one index per dimension");
        return false;
    }
    return true;
}

/* Whether any index is not constant (-1), has an x or z bit (0) or is a number (1) in idx[0..n). */
static int word_indices(const odin3_elab2_mem_rd *rd, int64_t *values) {
    int worst = 1;
    for (uint32_t k = 0; k < rd->n_idx; k++) {
        int kind = odin3_elab2_sig_i64(&rd->idx[k], &values[k]);
        if (kind < 0) {
            return -1;
        }
        worst = kind == 0 ? 0 : worst;
    }
    return worst;
}

/* "[7]" or "[1][5]": the indices as written, for the warning. */
static odin3_status word_index_text(const odin3_elab2_mem_rd *rd, const int64_t *values,
                                    odin3_strbuf *text) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < rd->n_idx; k++) {
        st = odin3_strbuf_appendf(text, "[%" PRId64 "]", values[k]);
    }
    return st;
}

/* The row-major position of the indices, or -1 outside the array. */
static int64_t word_position(const odin3_elab2 *e2, const odin3_elab_mem *mem,
                             const int64_t *values) {
    odin3_dim_span dims = odin3_symbol_unpacked(e2->st, mem->sym);
    int64_t pos = 0;
    for (uint32_t k = 0; k < dims.n; k++) {
        int64_t part = dim_position(mem, &dims.dims[k], values[k]);
        if (part < 0) {
            return -1;
        }
        pos = pos * (int64_t)odin3_elab2_dim_size(&dims.dims[k]) + part;
    }
    return pos;
}

int64_t odin3_elab2_mem_pos(const odin3_elab2 *e2, uint32_t mem, const int64_t *values) {
    const odin3_elab_mem *rec = odin3_elab2_mem_get(e2, mem);
    return rec == NULL ? -1 : word_position(e2, rec, values);
}

/* The out-of-range warning of a read or a write. */
static odin3_status word_warn(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, const int64_t *values,
                              bool write) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = word_index_text(rd, values, &text);
    if (st == ODIN3_OK) {
        const char *name = odin3_strtab_get(odin3_design_strtab(e2->design),
                                            odin3_elab2_mem_get(e2, rd->mem)->name);
        odin3_loc loc = odin3_ast_loc(e2->elab, rd->select);
        if (write) {
            odin3_elab2_warn(e2, loc, "assignment to word %s outside array '%s' is ignored",
                             text.data, name);
        } else {
            odin3_elab2_warn(e2, loc, "word select %s of '%s' is out of bounds; the word is x",
                             text.data, name);
        }
    }
    odin3_strbuf_free(&text);
    return st;
}

odin3_status odin3_elab2_mem_word(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, bool write,
                                  odin3_elab2_word *out) {
    if (!word_request_ok(e2, rd, true)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rd->mem);
    int64_t *values = odin3_arena_alloc(e2->scratch, (size_t)rd->n_idx * sizeof *values);
    if (values == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    int kind = word_indices(rd, values);
    *out = (odin3_elab2_word){ODIN3_WORD_VARIABLE, {0}};
    if (kind < 0) {
        return ODIN3_OK;
    }
    out->kind = ODIN3_WORD_X;
    if (kind == 0) {
        return ODIN3_OK; /* an index with an x or z bit: x, silently (spec 17 #40) */
    }
    int64_t pos = word_position(e2, mem, values);
    if (pos < 0) {
        return word_warn(e2, rd, values, write);
    }
    odin3_wire_id wire = (size_t)pos < mem->words.len
                             ? *(const odin3_wire_id *)odin3_vec_cat(&mem->words, (size_t)pos)
                             : (odin3_wire_id){0};
    if (wire.v != 0) {
        *out = (odin3_elab2_word){ODIN3_WORD_WIRE, wire};
    }
    return ODIN3_OK;
}
