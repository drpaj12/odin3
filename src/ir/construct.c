/*
 * construct.c — the construct table (IR-12a): rows of AST constructs that SOURCE records point
 * at, a u64map from (run, ast) to a chain of the rows of that node (arms, call instances), the
 * runs a snapshot covered and the call-site misses. Append-only; compact never touches it.
 */
#include "ir/construct.h"

#include "ir/construct_internal.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the kind map (V1.2.0 2.1) --------------------------------------------------------------- */

enum { KIND_TRANSPARENT = 0xFE, KIND_SLOTS = ODIN3_CONSTRUCT_CALL_SITE + 1 };

/* The synthesized kinds are pinned above the AST range and fit the row's one-byte kind (ABI 7). */
_Static_assert((int)ODIN3_AST_KIND_COUNT <= (int)ODIN3_CONSTRUCT_IF_THEN,
               "AST kinds reach the pinned rows");
_Static_assert(ODIN3_CONSTRUCT_CALL_SITE <= UINT8_MAX, "a construct kind is one byte");

/* One entry per kind: its level, or KIND_TRANSPARENT; 0 (NONE) is a kind with no entry. */
static const uint8_t k_kind_level[KIND_SLOTS] = {
    [ODIN3_AST_MODULE] = ODIN3_LEVEL_MODULE,
    [ODIN3_AST_UDP_DECL] = ODIN3_LEVEL_MODULE,
    [ODIN3_AST_CONFIG_DECL] = ODIN3_LEVEL_MODULE,

    [ODIN3_AST_INSTANTIATION] = ODIN3_LEVEL_INSTANCE,
    [ODIN3_AST_INSTANCE] = ODIN3_LEVEL_INSTANCE,
    [ODIN3_AST_GATE_DECL] = ODIN3_LEVEL_INSTANCE,

    [ODIN3_AST_ALWAYS] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_INITIAL] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_CONT_ASSIGN] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_NET_ASSIGN] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_GENERATE] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_GEN_FOR] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_GEN_IF] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_GEN_CASE] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_GEN_BLOCK] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_FUNCTION_DECL] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_TASK_DECL] = ODIN3_LEVEL_BLOCK,
    [ODIN3_AST_SPECIFY_BLOCK] = ODIN3_LEVEL_BLOCK,

    [ODIN3_AST_CASE_ITEM] = ODIN3_LEVEL_BRANCH,
    [ODIN3_CONSTRUCT_IF_THEN] = ODIN3_LEVEL_BRANCH,
    [ODIN3_CONSTRUCT_IF_ELSE] = ODIN3_LEVEL_BRANCH,

    [ODIN3_AST_IF] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_CASE] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_BLOCKING_ASSIGN] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_NONBLOCKING_ASSIGN] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_PROC_CONT_ASSIGN] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_FOR] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_WHILE] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_REPEAT] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_FOREVER] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_TIMING_STMT] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_WAIT] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_DISABLE] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_EVENT_TRIGGER] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_TASK_CALL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_NULL_STMT] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_SEQ_BLOCK] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_PAR_BLOCK] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_DECLARATOR] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_PORT_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_PORT_REF] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_NET_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_VAR_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_PARAM_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_GENVAR_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_EVENT_DECL] = ODIN3_LEVEL_STMT,
    [ODIN3_AST_DEFPARAM] = ODIN3_LEVEL_STMT,

    [ODIN3_AST_NUMBER] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_REAL] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_STRING] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_IDENT] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_HIER_NAME] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_SELECT] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_CONCAT] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_REPLICATE] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_UNARY] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_BINARY] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_TERNARY] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_CALL] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_MINTYPMAX] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_RANGE] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_EVENT_EXPR] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_EVENT_CONTROL] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_CONNECTION] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_STRENGTH] = ODIN3_LEVEL_EXPR,
    [ODIN3_AST_DELAY] = ODIN3_LEVEL_EXPR,
    [ODIN3_CONSTRUCT_CALL_SITE] = ODIN3_LEVEL_EXPR,

    [ODIN3_AST_UNIT] = KIND_TRANSPARENT,
    [ODIN3_AST_DIRECTIVE] = KIND_TRANSPARENT,
    [ODIN3_AST_LIST] = KIND_TRANSPARENT,
    [ODIN3_AST_ATTR] = KIND_TRANSPARENT,
};

odin3_construct_kind_class odin3_construct_kind_class_of(uint32_t kind) {
    if (kind >= KIND_SLOTS || k_kind_level[kind] == 0) {
        return ODIN3_KCLASS_UNKNOWN;
    }
    return k_kind_level[kind] == KIND_TRANSPARENT ? ODIN3_KCLASS_TRANSPARENT
                                                  : ODIN3_KCLASS_LEVELLED;
}

odin3_level odin3_construct_kind_level(uint32_t kind) {
    return odin3_construct_kind_class_of(kind) == ODIN3_KCLASS_LEVELLED
               ? (odin3_level)k_kind_level[kind]
               : ODIN3_LEVEL_NONE;
}

/* --- the table ------------------------------------------------------------------------------- */

odin3_construct_table *odin3_construct_create(void) {
    odin3_construct_table *table = odin3_util_calloc(sizeof *table);
    if (table == NULL) {
        return NULL;
    }
    odin3_vec_init(&table->rows, sizeof(odin3_construct_row));
    odin3_vec_init(&table->runs, sizeof(run_row));
    odin3_vec_init(&table->misses, sizeof(uint32_t));
    table->heads = odin3_u64map_create(0);
    if (table->heads == NULL) {
        odin3_construct_destroy(table);
        return NULL;
    }
    return table;
}

void odin3_construct_destroy(odin3_construct_table *table) {
    if (table == NULL) {
        return;
    }
    odin3_vec_free(&table->rows);
    odin3_vec_free(&table->runs);
    odin3_vec_free(&table->misses);
    odin3_u64map_destroy(table->heads);
    odin3_util_free(table);
}

odin3_status odin3_construct_copy(odin3_construct_table *dst, const odin3_construct_table *src) {
    odin3_u64map *heads = odin3_u64map_clone(src->heads);
    if (heads == NULL || odin3_vec_copy(&dst->rows, &src->rows) != ODIN3_OK ||
        odin3_vec_copy(&dst->runs, &src->runs) != ODIN3_OK ||
        odin3_vec_copy(&dst->misses, &src->misses) != ODIN3_OK) {
        odin3_u64map_destroy(heads);
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_u64map_destroy(dst->heads);
    dst->heads = heads;
    return ODIN3_OK;
}

enum { RUN_SHIFT = 32 };

static uint64_t node_key(odin3_passrun_id run, uint32_t ast) {
    return ((uint64_t)run.v << RUN_SHIFT) | ast;
}

static const odin3_construct_row *row_cat(const odin3_construct_table *table, uint32_t index) {
    return index != 0 && index <= table->rows.len ? odin3_vec_cat(&table->rows, index - 1U) : NULL;
}

bool odin3_construct_parent_fits(const odin3_construct_row *parent,
                                 const odin3_construct_row *row) {
    if (parent == NULL) {
        return true; /* a root */
    }
    if (parent->level <= row->level) {
        return true; /* the parent is the coarser (or the same) level */
    }
    /* The two finer parents: an arm or case item (branch) under its IF or CASE (stmt), and a
     * call-instance copy of a function body under its call site (4.3). */
    return (row->level == (uint8_t)ODIN3_LEVEL_BRANCH &&
            parent->level == (uint8_t)ODIN3_LEVEL_STMT) ||
           row->call != 0;
}

/* Why a row cannot be added; NULL when it can. */
static const char *add_problem(const odin3_construct_table *table, const odin3_construct_row *row) {
    uint32_t own = (uint32_t)table->rows.len + 1U;
    if (row->run == 0 || row->ast == 0) {
        return "run or ast is 0";
    }
    if (odin3_construct_kind_level(row->kind) == ODIN3_LEVEL_NONE ||
        (uint32_t)odin3_construct_kind_level(row->kind) != row->level) {
        return "kind has no row or does not match the level";
    }
    if ((row->kind == ODIN3_CONSTRUCT_IF_THEN) != (row->part == ODIN3_CONSTRUCT_PART_THEN) ||
        (row->kind == ODIN3_CONSTRUCT_IF_ELSE) != (row->part == ODIN3_CONSTRUCT_PART_ELSE) ||
        row->part > ODIN3_CONSTRUCT_PART_ELSE) {
        return "part does not match the kind";
    }
    if (row->parent >= own || row->call >= own) {
        return "parent or call is not an earlier row";
    }
    if (!odin3_construct_parent_fits(row_cat(table, row->parent), row)) {
        return "parent is a finer level than the row";
    }
    return NULL;
}

/* The row number with this part and call in the chain starting at head, 0 if none. */
static uint32_t chain_find(const odin3_construct_table *table, uint32_t head,
                           const odin3_construct_key *key) {
    for (uint32_t at = head; at != 0;) {
        const odin3_construct_row *row = row_cat(table, at);
        if (row->part == key->part && row->call == key->call) {
            return at;
        }
        at = row->next;
    }
    return 0;
}

odin3_status odin3_construct_add(odin3_design *design, const odin3_construct_row *row,
                                 uint32_t *index) {
    odin3_construct_table *table = design != NULL ? design->construct : NULL;
    const char *why = table != NULL && row != NULL && index != NULL ? add_problem(table, row)
                                                                    : "no design, row or index";
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_construct_add: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t key = node_key((odin3_passrun_id){row->run}, row->ast);
    uint64_t head = 0;
    (void)odin3_u64map_get(table->heads, key, &head);
    odin3_construct_key own_key = {{row->run}, row->ast, row->part, row->call};
    if (chain_find(table, (uint32_t)head, &own_key) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_construct_add: run %u node %u part %u call %u exists",
                  row->run, row->ast, (unsigned)row->part, row->call);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (table->rows.len >= (size_t)UINT32_MAX - 1U ||
        odin3_vec_reserve(&table->rows, table->rows.len + 1U) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY; /* row numbers (and the end, one past) are 32 bits */
    }
    uint32_t own = (uint32_t)table->rows.len + 1U;
    if (odin3_u64map_put(table->heads, (odin3_kv){key, own}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_construct_row *slot = odin3_vec_push(&table->rows); /* reserved */
    *slot = *row;
    slot->next = (uint32_t)head;
    *index = own;
    return ODIN3_OK;
}

uint32_t odin3_construct_find(const odin3_design *design, const odin3_construct_key *key) {
    uint64_t head = 0;
    if (design == NULL || key == NULL ||
        !odin3_u64map_get(design->construct->heads, node_key(key->run, key->ast), &head)) {
        return 0;
    }
    return chain_find(design->construct, (uint32_t)head, key);
}

const odin3_construct_row *odin3_construct_get(const odin3_design *design, uint32_t index) {
    return design != NULL ? row_cat(design->construct, index) : NULL;
}

uint32_t odin3_construct_end(const odin3_design *design) {
    return design != NULL ? (uint32_t)design->construct->rows.len + 1U : 1U;
}

size_t odin3_construct_bytes_reserved(const odin3_design *design) {
    const odin3_construct_table *table = design != NULL ? design->construct : NULL;
    if (table == NULL) {
        return 0;
    }
    return sizeof *table + (table->rows.cap * table->rows.elem_size) +
           (table->runs.cap * table->runs.elem_size) +
           (table->misses.cap * table->misses.elem_size) + odin3_u64map_bytes(table->heads);
}

bool odin3_construct_has_node(const odin3_design *design, odin3_passrun_id run, uint32_t ast) {
    uint64_t head = 0;
    if (design == NULL || !odin3_u64map_get(design->construct->heads, node_key(run, ast), &head)) {
        return false;
    }
    for (uint32_t at = (uint32_t)head; at != 0; at = row_cat(design->construct, at)->next) {
        if (row_cat(design->construct, at)->part == ODIN3_CONSTRUCT_PART_NODE) {
            return true;
        }
    }
    return false;
}

/* True when row's call row has the span of loc and its file. */
static bool call_span_is(const odin3_construct_table *table, uint32_t call,
                         const odin3_srcloc *loc) {
    const odin3_construct_row *site = row_cat(table, call);
    return site != NULL && site->file == loc->file && site->line == loc->line &&
           site->col == loc->col && site->end_line == loc->end_line &&
           site->end_col == loc->end_col;
}

uint32_t odin3_construct_of_record(const odin3_design *design, odin3_prov_id record) {
    const odin3_prov_record *rec = design != NULL ? odin3_prov_get(design, record) : NULL;
    if (rec == NULL || rec->kind != ODIN3_PROV_SOURCE || rec->ast == 0) {
        return 0;
    }
    if (rec->n_locs < 2) {
        const odin3_construct_key key = {rec->run, rec->ast, ODIN3_CONSTRUCT_PART_NODE, 0};
        return odin3_construct_find(design, &key);
    }
    uint64_t head = 0;
    if (!odin3_u64map_get(design->construct->heads, node_key(rec->run, rec->ast), &head)) {
        return 0;
    }
    uint32_t found = 0;
    for (uint32_t at = (uint32_t)head; at != 0; at = row_cat(design->construct, at)->next) {
        const odin3_construct_row *row = row_cat(design->construct, at);
        if (row->part == ODIN3_CONSTRUCT_PART_NODE && row->call != 0 &&
            call_span_is(design->construct, row->call, &rec->locs[1])) {
            if (found != 0) {
                return 0; /* two call sites of one span: left as a span-only leaf (4.3) */
            }
            found = at;
        }
    }
    return found;
}

odin3_status odin3_construct_mark_run(odin3_design *design, odin3_passrun_id run,
                                      odin3_construct_mode mode) {
    if (design == NULL || run.v == 0 || run.v >= odin3_passrun_end(design) ||
        mode == ODIN3_CONSTRUCT_MODE_OFF || mode > ODIN3_CONSTRUCT_MODE_FULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_construct_mark_run: invalid run or mode");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_construct_table *table = design->construct;
    odin3_construct_mode have = odin3_construct_run_mode(design, run);
    if (have != ODIN3_CONSTRUCT_MODE_OFF) {
        if (have != mode) {
            odin3_log(ODIN3_LOG_ERROR,
                      "odin3_construct_mark_run: run %u is marked with mode %u, "
                      "not %u",
                      run.v, (unsigned)have, (unsigned)mode);
            return ODIN3_ERR_INVALID_ARG;
        }
        return ODIN3_OK;
    }
    run_row *slot = odin3_vec_push(&table->runs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (run_row){run.v, (uint32_t)mode};
    return ODIN3_OK;
}

odin3_construct_mode odin3_construct_run_mode(const odin3_design *design, odin3_passrun_id run) {
    const odin3_construct_table *table = design != NULL ? design->construct : NULL;
    for (size_t i = 0; table != NULL && i < table->runs.len; i++) {
        const run_row *row = odin3_vec_cat(&table->runs, i);
        if (row->run == run.v) {
            return (odin3_construct_mode)row->mode;
        }
    }
    return ODIN3_CONSTRUCT_MODE_OFF;
}

odin3_status odin3_construct_note_miss(odin3_design *design, odin3_prov_id record) {
    odin3_vec *misses = design != NULL ? &design->construct->misses : NULL;
    if (misses == NULL || record.v == 0 ||
        (misses->len > 0 &&
         *(const uint32_t *)odin3_vec_cat(misses, misses->len - 1U) >= record.v)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_construct_note_miss: invalid or out-of-order record");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t *slot = odin3_vec_push(misses);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = record.v;
    return ODIN3_OK;
}

bool odin3_construct_is_miss(const odin3_design *design, odin3_prov_id record) {
    const odin3_vec *misses = design != NULL ? &design->construct->misses : NULL;
    size_t lo = 0;
    size_t hi = misses != NULL ? misses->len : 0;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        uint32_t have = *(const uint32_t *)odin3_vec_cat(misses, mid);
        if (have == record.v) {
            return true;
        }
        if (have < record.v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}
