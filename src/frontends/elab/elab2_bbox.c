/*
 * elab2_bbox.c — black-box cell types in stage 2 (2D Task 19 and spec v4.19, Peter decision 3;
 * EL-11): the cell type of a `(* blackbox *)` or implicit black-box MODULE, declared once per
 * definition under its base name (IR-7b), with the exact port widths of every parameter set an
 * instance uses.
 *
 * Yosys 0.55 keeps one `module \bb` (the defaults' widths) and cells `\bb` whose connections have
 * the widths of their own parameter set: `hierarchy` derives the module to check the connections
 * and drops it. The IR has one cell type per name, so a port whose width differs between the
 * parameter sets gets a width expression (celltype.h's fourth width rule) that looks the cell's
 * parameter values up in a table with one entry per set: stage 1 writes each set other than the
 * defaults' as a carrier MODULE (a ports-only black-box MODULE whose `blackbox` ATTR holds the
 * definition's name), and the definition's MODULE reads them all when it declares the type. The
 * cell keeps its source parameters; no hidden parameter and no per-cell type.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { BB_BUCKETS = 64 };

static const char k_blackbox[] = "blackbox";

static const char *str_of(const odin3_elab2 *e2, uint32_t id) {
    const char *str = odin3_strtab_get(odin3_design_strtab(e2->design), id);
    return str != NULL ? str : "";
}

/* --- the table of parameter sets ----------------------------------------------------------------
 */

/* One parameter set: its values (one per parameter of the type) and the width of every port. */
typedef struct bb_entry {
    struct bb_entry *next; /* of its bucket in the table, or of the list being gathered */
    odin3_ast_id node;     /* the MODULE it came from */
    uint64_t hash;
    odin3_value *values;
    uint32_t *widths;
} bb_entry;

/* The sets of one black box, shared by the width rules of its ports (design arena). */
typedef struct bb_table {
    bb_entry *bucket[BB_BUCKETS];
    uint32_t n_params;
} bb_table;

typedef struct bb_rule {
    const bb_table *table;
    uint32_t port;
} bb_rule;

static uint64_t bb_value_hash(uint64_t hash, const odin3_value *val) {
    hash = odin3_hash_combine(hash, (uint64_t)val->kind);
    if (val->kind == ODIN3_VAL_INT) {
        return odin3_hash_combine(hash, (uint64_t)val->i);
    }
    if (val->kind == ODIN3_VAL_STRING) {
        return odin3_hash_combine(hash, val->str);
    }
    if (val->kind == ODIN3_VAL_BITS && val->len > 0 && val->bits != NULL) {
        odin3_bytes bits = {val->bits, val->len};
        return odin3_hash_combine(hash, odin3_hash_bytes(bits, ODIN3_HASH_SEED));
    }
    return hash;
}

static uint64_t bb_hash(const odin3_value *values, uint32_t count) {
    uint64_t hash = ODIN3_HASH_SEED;
    for (uint32_t k = 0; k < count; k++) {
        hash = bb_value_hash(hash, &values[k]);
    }
    return hash;
}

static bool bb_same(const bb_entry *entry, const odin3_value *values, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        if (!odin3_value_equal(&entry->values[k], &values[k])) {
            return false;
        }
    }
    return true;
}

static const bb_entry *bb_find(const bb_table *table, const odin3_value *values) {
    uint64_t hash = bb_hash(values, table->n_params);
    for (const bb_entry *at = table->bucket[hash % BB_BUCKETS]; at != NULL; at = at->next) {
        if (at->hash == hash && bb_same(at, values, table->n_params)) {
            return at;
        }
    }
    return NULL;
}

/* Registration check: the rule's table must exist, have one value slot per parameter of the type
 * and name an existing port, and every parameter of the type must be of a kind the table hashes
 * and compares (INT, STRING, BITS). */
static bool bb_check(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                     odin3_width_why *why) {
    const bb_rule *rule = wexpr->impl;
    const char *bad = NULL;
    if (rule == NULL || rule->table == NULL) {
        bad = "no table of parameter sets";
    } else if (rule->table->n_params != def->n_params) {
        bad = "the table's parameters are not the type's";
    } else if (rule->port >= def->n_ports) {
        bad = "the rule's port is not a port of the type";
    }
    for (uint32_t k = 0; bad == NULL && k < def->n_params; k++) {
        if (def->params[k].kind != ODIN3_VAL_INT && def->params[k].kind != ODIN3_VAL_STRING &&
            def->params[k].kind != ODIN3_VAL_BITS) {
            bad = "a parameter is of a kind the table cannot compare";
        }
    }
    if (bad != NULL) {
        (void)snprintf(why->text, sizeof why->text, "%s", bad);
        return false;
    }
    return true;
}

static odin3_status bb_eval(const odin3_width_expr *wexpr, const odin3_width_args *args,
                            uint32_t *width) {
    const bb_rule *rule = wexpr->impl;
    const bb_entry *entry = bb_find(rule->table, args->params);
    if (entry == NULL) {
        if (args->why != NULL) {
            (void)snprintf(args->why->text, sizeof args->why->text,
                           "no parameter set of the black box has these values");
        }
        return ODIN3_ERR_INVALID_ARG;
    }
    *width = entry->widths[rule->port];
    return ODIN3_OK;
}

/* --- the declaration ----------------------------------------------------------------------------
 */

/* The cell type being declared from a black-box MODULE: its ports at the default widths (the
 * declaration IR-7b compares) and its parameters. */
typedef struct bbox_decl {
    odin3_port_def *fixed;
    odin3_param_def *params;
    uint32_t n_ports, n_params;
} bbox_decl;

static odin3_dir bbox_dir(uint32_t sub) {
    if (sub == ODIN3_AST_PORT_DECL_OUTPUT) {
        return ODIN3_DIR_OUT;
    }
    return sub == ODIN3_AST_PORT_DECL_INOUT ? ODIN3_DIR_INOUT : ODIN3_DIR_IN;
}

/* The ports of the MODULE in port order (all written as wire ports, EL-4), with their widths. */
static odin3_status bbox_ports(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    odin3_ast_span ports = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, 1));
    bd->n_ports = ports.n;
    bd->fixed = odin3_arena_alloc(e2->scratch, (size_t)(ports.n + 1U) * sizeof *bd->fixed);
    if (bd->fixed == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < ports.n; i++) {
        odin3_ast_id declarator = odin3_ast_child(e2->elab, ports.ids[i], 1);
        const odin3_symbol *rec =
            odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, declarator));
        if (rec == NULL) {
            return ODIN3_ERR_CHECK;
        }
        int64_t span = (int64_t)rec->packed.msb - (int64_t)rec->packed.lsb;
        odin3_port_def def = {.name = str_of(e2, rec->name),
                              .dir = bbox_dir(odin3_ast_sub(e2->elab, ports.ids[i])),
                              .scalar = !rec->has_packed,
                              .width =
                                  rec->has_packed ? (uint32_t)(span < 0 ? -span : span) + 1U : 1U};
        bd->fixed[i] = def;
    }
    return ODIN3_OK;
}

/* The declarators of the MODULE's PARAM_DECLs (bare in a black box) in declaration order, in the
 * scratch arena; none gives *ids NULL. */
static odin3_status bbox_declarators(odin3_elab2 *e2, odin3_ast_id module_node,
                                     const odin3_ast_id **ids, uint32_t *count) {
    odin3_ast_id list = odin3_ast_child(e2->elab, module_node, 0);
    odin3_ast_span decls = odin3_ast_children(e2->elab, list);
    uint32_t slots = odin3_ast_slot_count(ODIN3_AST_PARAM_DECL);
    uint32_t total = 0;
    for (uint32_t i = 0; list.v != 0 && i < decls.n; i++) {
        uint32_t kids = odin3_ast_nchild(e2->elab, decls.ids[i]);
        total += kids > slots ? kids - slots : 0U;
    }
    odin3_ast_id *out = odin3_arena_alloc(e2->scratch, (size_t)(total + 1U) * sizeof *out);
    if (out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *count = 0;
    for (uint32_t i = 0; list.v != 0 && i < decls.n; i++) {
        odin3_ast_span kids = odin3_ast_children(e2->elab, decls.ids[i]);
        for (uint32_t k = slots; k < kids.n; k++) {
            out[(*count)++] = kids.ids[k];
        }
    }
    *ids = out;
    return ODIN3_OK;
}

/* One declared parameter: an integer when its value is one, else bits or a string. */
static odin3_status bbox_param(odin3_elab2 *e2, odin3_ast_id declarator, odin3_param_def *out) {
    odin3_ast_id value = odin3_ast_child(e2->elab, declarator, 0);
    out->name = odin3_ast_name_str(e2->elab, declarator);
    if (odin3_ast_kind_of(e2->elab, value) == ODIN3_AST_STRING) {
        out->kind = ODIN3_VAL_STRING;
        out->dflt = (odin3_value){.kind = ODIN3_VAL_STRING, .str = odin3_ast_name(e2->elab, value)};
        return ODIN3_OK;
    }
    int64_t num = 0;
    bool known = false;
    odin3_status st = odin3_elab2_number_i64(e2, value, &num, &known);
    if (st == ODIN3_OK && known) {
        out->kind = ODIN3_VAL_INT;
        out->dflt = odin3_value_int(num);
        return ODIN3_OK;
    }
    odin3_ast_literal lit;
    st = st != ODIN3_OK ? st : odin3_ast_number_value(e2->elab, value, e2->scratch, &lit);
    out->kind = ODIN3_VAL_BITS;
    out->dflt = lit.value;
    return st;
}

/* The parameters (a PARAM_DECL's declarators, bare in a black box) in declaration order. */
static odin3_status bbox_params(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    const odin3_ast_id *ids = NULL;
    uint32_t total = 0;
    odin3_status st = bbox_declarators(e2, module_node, &ids, &total);
    bd->params = st == ODIN3_OK
                     ? odin3_arena_alloc(e2->scratch, (size_t)(total + 1U) * sizeof *bd->params)
                     : NULL;
    if (st == ODIN3_OK && bd->params == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < total; k++) {
        st = bbox_param(e2, ids[k], &bd->params[k]);
        bd->n_params = k + 1U;
    }
    return st;
}

/* A registered type of that name: the declaration must match it (IR-7b), else the located error. */
static odin3_status bbox_match(odin3_elab2 *e2, odin3_ast_id module_node, const bbox_decl *bd,
                               odin3_celltype_id registered) {
    const odin3_celltype_def *reg = odin3_celltype_get(e2->design, registered);
    odin3_celltype_def decl = {
        .name = reg->name, .gran = ODIN3_GRAN_BLACKBOX, .ports = bd->fixed, .n_ports = bd->n_ports};
    odin3_value *params =
        odin3_arena_alloc(e2->scratch, (size_t)(reg->n_params + 1U) * sizeof *params);
    if (params == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_blackbox_match match = {registered, &decl, params};
    odin3_width_why why = {{0}};
    odin3_status st = odin3_celltype_blackbox_match(e2->design, &match, &why);
    if (st == ODIN3_ERR_INVALID_ARG) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, module_node),
                          "black box '%s' does not match the registered cell type: %s", reg->name,
                          why.text);
        return ODIN3_ERR_PARSE;
    }
    return st;
}

/* --- the parameter sets of a definition ----------------------------------------------------------
 */

/* The definition a MODULE is a carrier of (its `blackbox` ATTR holds the name as a STRING, spec
 * v4.19): the name's ID; false for a definition's own MODULE and for any other. */
static bool bbox_base_of(const odin3_elab2 *e2, odin3_ast_id module_node, uint32_t *base) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(k_blackbox), &key)) {
        return false;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, module_node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        odin3_ast_id value = odin3_ast_child(e2->elab, attrs.ids[i], 0);
        if (odin3_ast_name(e2->elab, attrs.ids[i]) == key && value.v != 0 &&
            odin3_ast_kind_of(e2->elab, value) == ODIN3_AST_STRING) {
            *base = odin3_ast_name(e2->elab, value);
            return true;
        }
    }
    return false;
}

/* The sets being gathered: a list of entries (scratch arena) in the order they were met. */
typedef struct bb_gather {
    odin3_elab2 *e2;
    const bbox_decl *bd; /* the definition's: the kinds of the parameters, the default widths */
    const char *name;    /* the definition's name (the error's) */
    bb_entry *first, *last;
} bb_gather;

static void gather_push(bb_gather *ga, bb_entry *entry) {
    if (ga->last != NULL) {
        ga->last->next = entry;
    } else {
        ga->first = entry;
    }
    ga->last = entry;
}

/* An entry of the scratch arena for MODULE node with the widths of its ports. */
static bb_entry *gather_entry(bb_gather *ga, odin3_ast_id node, const bbox_decl *ports) {
    odin3_arena *scratch = ga->e2->scratch;
    bb_entry *entry = odin3_arena_alloc(scratch, sizeof *entry);
    if (entry == NULL) {
        return NULL;
    }
    entry->next = NULL;
    entry->node = node;
    entry->values =
        odin3_arena_alloc(scratch, (size_t)(ga->bd->n_params + 1U) * sizeof(odin3_value));
    entry->widths = odin3_arena_alloc(scratch, (size_t)(ports->n_ports + 1U) * sizeof(uint32_t));
    if (entry->values == NULL || entry->widths == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < ports->n_ports; i++) {
        entry->widths[i] = ports->fixed[i].width;
    }
    return entry;
}

/* A carrier's set: the values of its PARAM_DECLs as the definition's parameters want them, and its
 * ports' widths; a set whose values do not fit the kinds is left out (its instances are reported
 * where they give such a value). */
static odin3_status gather_carrier(bb_gather *ga, odin3_ast_id node) {
    odin3_elab2 *e2 = ga->e2;
    bbox_decl ports = {0};
    const odin3_ast_id *ids = NULL;
    uint32_t count = 0;
    odin3_status st = bbox_ports(e2, node, &ports);
    st = st != ODIN3_OK ? st : bbox_declarators(e2, node, &ids, &count);
    if (st != ODIN3_OK) {
        return st;
    }
    if (ports.n_ports != ga->bd->n_ports || count != ga->bd->n_params) {
        return ODIN3_ERR_CHECK; /* a carrier has its definition's ports and parameters: a 2D bug */
    }
    bb_entry *entry = gather_entry(ga, node, &ports);
    if (entry == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < count; k++) {
        st = odin3_elab2_param_value(e2, &ga->bd->params[k], odin3_ast_child(e2->elab, ids[k], 0),
                                     &entry->values[k]);
        if (st == ODIN3_ERR_PARSE) {
            return ODIN3_OK;
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    gather_push(ga, entry);
    return ODIN3_OK;
}

/* Every carrier of the definition named name in the UNIT, after the definition's own set. */
static odin3_status gather_carriers(bb_gather *ga, odin3_ast_id own, uint32_t name) {
    const odin3_elab2 *e2 = ga->e2;
    odin3_ast_span modules = odin3_ast_children(e2->elab, odin3_ast_root(e2->elab, 0));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < modules.n; i++) {
        uint32_t base = 0;
        if (modules.ids[i].v != own.v && bbox_base_of(e2, modules.ids[i], &base) && base == name) {
            st = gather_carrier(ga, modules.ids[i]);
        }
    }
    return st;
}

/* Whether port `port` has a width in some set other than the first's (the definition's own). */
static bool port_varies(const bb_gather *ga, uint32_t port) {
    for (const bb_entry *at = ga->first->next; at != NULL; at = at->next) {
        if (at->widths[port] != ga->first->widths[port]) {
            return true;
        }
    }
    return false;
}

/* The entry with entry's values in the table, if any: *clash is then the first port + 1 whose
 * width differs between the two (0: none). */
static const bb_entry *table_twin(const bb_gather *ga, const bb_table *table, const bb_entry *entry,
                                  uint32_t *clash) {
    uint64_t hash = bb_hash(entry->values, table->n_params);
    for (const bb_entry *at = table->bucket[hash % BB_BUCKETS]; at != NULL; at = at->next) {
        if (at->hash != hash || !bb_same(at, entry->values, table->n_params)) {
            continue;
        }
        for (uint32_t port = 0; port < ga->bd->n_ports && *clash == 0; port++) {
            *clash = at->widths[port] != entry->widths[port] ? port + 1U : 0U;
        }
        return at;
    }
    return NULL;
}

/* A copy of entry in the design arena, first in its bucket of table. */
static odin3_status table_insert(const bb_gather *ga, bb_table *table, const bb_entry *entry) {
    odin3_arena *arena = odin3_celltype_arena(ga->e2->design);
    bb_entry *copy = odin3_arena_alloc(arena, sizeof *copy);
    size_t values = (size_t)(table->n_params + 1U) * sizeof(odin3_value);
    size_t widths = (size_t)(ga->bd->n_ports + 1U) * sizeof(uint32_t);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    copy->values = odin3_arena_alloc(arena, values);
    copy->widths = odin3_arena_alloc(arena, widths);
    if (copy->values == NULL || copy->widths == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy->widths, entry->widths, (size_t)ga->bd->n_ports * sizeof(uint32_t));
    for (uint32_t k = 0; k < table->n_params; k++) {
        odin3_status st = odin3_value_copy(arena, &entry->values[k], &copy->values[k]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    copy->node = entry->node;
    copy->hash = bb_hash(entry->values, table->n_params);
    bb_entry **head = &table->bucket[copy->hash % BB_BUCKETS];
    copy->next = *head;
    *head = copy;
    return ODIN3_OK;
}

/* Every gathered set into table; two sets with equal values must give every port one width. */
static odin3_status table_fill(const bb_gather *ga, bb_table *table, const bbox_decl *bd) {
    for (const bb_entry *at = ga->first; at != NULL; at = at->next) {
        uint32_t clash = 0;
        const bb_entry *twin = table_twin(ga, table, at, &clash);
        if (clash != 0) {
            odin3_elab2_error(ga->e2, odin3_ast_loc(ga->e2->elab, at->node),
                              "black box '%s': port '%s' has different widths in parameter sets "
                              "with equal values",
                              ga->name, bd->fixed[clash - 1U].name);
            return ODIN3_ERR_PARSE;
        }
        odin3_status st = twin == NULL ? table_insert(ga, table, at) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The width rule of port `port` over table (design arena). */
static odin3_status rule_set(const bb_gather *ga, const bb_table *table, odin3_port_def *pdef,
                             uint32_t port) {
    odin3_arena *arena = odin3_celltype_arena(ga->e2->design);
    bb_rule *rule = odin3_arena_alloc(arena, sizeof *rule);
    odin3_width_expr *wexpr = odin3_arena_alloc(arena, sizeof *wexpr);
    if (rule == NULL || wexpr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rule = (bb_rule){table, port};
    *wexpr = (odin3_width_expr){bb_check, bb_eval, rule};
    pdef->width_expr = wexpr;
    return ODIN3_OK;
}

/* The table of the sets, in the design arena, and the width rules of the ports that vary. */
static odin3_status install_rules(const bb_gather *ga, bbox_decl *bd) {
    bb_table *table = odin3_arena_alloc(odin3_celltype_arena(ga->e2->design), sizeof *table);
    if (table == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(table, 0, sizeof *table);
    table->n_params = bd->n_params;
    odin3_status st = table_fill(ga, table, bd);
    for (uint32_t port = 0; st == ODIN3_OK && port < bd->n_ports; port++) {
        st = port_varies(ga, port) ? rule_set(ga, table, &bd->fixed[port], port) : ODIN3_OK;
    }
    return st;
}

/* The width rules of the definition's ports under its parameter sets: its own (the defaults) and
 * its carriers'. Nothing when it has no parameter or no carrier changes a width. */
static odin3_status bbox_rules(odin3_elab2 *e2, odin3_ast_id module_node, bbox_decl *bd) {
    if (bd->n_params == 0) {
        return ODIN3_OK;
    }
    uint32_t name = odin3_ast_name(e2->elab, module_node);
    bb_gather ga = {e2, bd, str_of(e2, name), NULL, NULL};
    bb_entry *own = gather_entry(&ga, module_node, bd);
    if (own == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < bd->n_params; k++) {
        own->values[k] = bd->params[k].dflt;
    }
    gather_push(&ga, own);
    odin3_status st = gather_carriers(&ga, module_node, name);
    bool any = false;
    for (uint32_t port = 0; st == ODIN3_OK && port < bd->n_ports; port++) {
        any = any || port_varies(&ga, port);
    }
    return st == ODIN3_OK && any ? install_rules(&ga, bd) : st;
}

odin3_status odin3_elab2_blackbox(odin3_elab2 *e2, odin3_ast_id module_node) {
    uint32_t base = 0;
    if (bbox_base_of(e2, module_node, &base)) {
        return ODIN3_OK; /* a carrier: its definition's MODULE reads it */
    }
    bbox_decl bd = {0};
    odin3_status st = bbox_ports(e2, module_node, &bd);
    st = st != ODIN3_OK ? st : bbox_params(e2, module_node, &bd);
    odin3_celltype_id registered = {0};
    uint32_t name = odin3_ast_name(e2->elab, module_node);
    bool taken = st == ODIN3_OK && odin3_celltype_find(e2->design, name, &registered);
    if (st == ODIN3_OK && taken) {
        st = bbox_match(e2, module_node, &bd, registered);
    } else if (st == ODIN3_OK) {
        st = bbox_rules(e2, module_node, &bd);
    }
    odin3_celltype_def def = {.name = str_of(e2, name),
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = bd.fixed,
                              .n_ports = bd.n_ports,
                              .params = bd.params,
                              .n_params = bd.n_params};
    odin3_celltype_id id = {0};
    st = st != ODIN3_OK ? st : odin3_celltype_declare_blackbox(e2->design, &def, &id);
    st = st != ODIN3_OK ? st : odin3_u64map_put(e2->bboxes, (odin3_kv){module_node.v, id.v});
    return st == ODIN3_ERR_INVALID_ARG ? ODIN3_ERR_PARSE : st;
}
