/*
 * names_v2.c — the records and the chain of an object of a version 2 name map (V1.2.0 3.5).
 *
 * The order is the one of the library: the records are the object's own record first, then every
 * record reachable through parents, depth first in parent order, each once (Node.provenance());
 * the chain is, for each SOURCE leaf of that walk that has a construct, the construct and then its
 * ancestors up to the module, each construct once at its first reach (Node.chain()). Lines are
 * printed as the map holds them (escaped), so output can be read back with the same parse.
 * The walks use an explicit stack.
 */
#include "passes/names_v2.h"

#include "util/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum { INDENT_STEP = 2, INDENT_BASE = 2 };

void names_v2_free(names_v2 *v2) {
    for (size_t k = 0; k < NV2_TABS; k++) {
        odin3_vec_free(&v2->tab[k]);
    }
}

static int table_of(char type) {
    switch (type) {
    case 'R':
        return NV2_R;
    case 'C':
        return NV2_C;
    case 'F':
        return NV2_F;
    case 'T':
        return NV2_T;
    default:
        return -1;
    }
}

odin3_status names_v2_init(names_v2 *v2, const odin3_name_map *map) {
    v2->map = map;
    for (size_t k = 0; k < NV2_TABS; k++) {
        odin3_vec_init(&v2->tab[k], sizeof(names_v2_ref));
    }
    for (size_t i = 0; i < map->lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&map->lines, i);
        int tab = table_of(line->type);
        names_v2_ref *slot = tab >= 0 ? odin3_vec_push(&v2->tab[tab]) : NULL;
        if (tab >= 0 && slot == NULL) {
            names_v2_free(v2);
            return ODIN3_ERR_NO_MEMORY;
        }
        if (slot != NULL) {
            slot->line = line;
        }
    }
    return ODIN3_OK;
}

/* The line numbered num of a table, or NULL when there is no such line. */
static const odin3_nm_line *line_at(const names_v2 *v2, names_v2_tab tab, uint32_t num) {
    const names_v2_ref *ref = num >= 1 && num <= v2->tab[tab].len
                                  ? odin3_vec_cat(&v2->tab[tab], num - 1U)
                                  : NULL;
    return ref != NULL ? ref->line : NULL;
}

/* A line as the map holds it, after `indent` spaces. */
static odin3_status put_line(odin3_strbuf *out, const odin3_nm_line *line, uint32_t indent) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < indent; i++) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr(" "));
    }
    const char type[2] = {line->type, '\0'};
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(type)) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < line->nfields; k++) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("\t"));
        st = st == ODIN3_OK ? odin3_strbuf_append(out, line->f[k]) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n")) : st;
}

/* The elements of a list field split at sep, as a vector of odin3_bytes. */
static odin3_status split_list(odin3_bytes field, char sep, odin3_vec *out) {
    const char *bytes = (const char *)field.ptr;
    size_t start = 0;
    for (size_t i = 0; field.len > 0 && i <= field.len; i++) {
        if (i < field.len && bytes[i] != sep) {
            continue;
        }
        odin3_bytes *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_bytes){bytes + start, i - start};
        start = i + 1;
    }
    return ODIN3_OK;
}

/* One entry of the record walk. */
typedef struct walk_item {
    uint32_t rec;
    uint32_t depth;
} walk_item;

/* Marks of one query: one byte per R, C, F and T line. */
typedef struct marks {
    uint8_t *seen[NV2_TABS];
} marks;

static void marks_free(marks *m) {
    for (size_t k = 0; k < NV2_TABS; k++) {
        odin3_util_free(m->seen[k]);
        m->seen[k] = NULL;
    }
}

static odin3_status marks_init(marks *m, const names_v2 *v2) {
    for (size_t k = 0; k < NV2_TABS; k++) {
        m->seen[k] = odin3_util_calloc(v2->tab[k].len + 1U);
        if (m->seen[k] == NULL) {
            marks_free(m);
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* Marks num of table tab; true when it was not marked before (and is a line of the table). */
static bool mark_new(const names_v2 *v2, marks *m, names_v2_tab tab, uint32_t num) {
    if (num < 1 || num > v2->tab[tab].len || m->seen[tab][num] != 0) {
        return false;
    }
    m->seen[tab][num] = 1;
    return true;
}

/* Pushes the parents of record line (in reverse, so the first parent is popped first). */
static odin3_status push_parents(const odin3_nm_line *line, uint32_t depth, odin3_vec *stack) {
    odin3_vec parts;
    odin3_vec_init(&parts, sizeof(odin3_bytes));
    odin3_status st = split_list(line->f[NM_R_PARENTS], ',', &parts);
    for (size_t k = parts.len; st == ODIN3_OK && k > 0; k--) {
        uint32_t num = 0;
        const odin3_bytes *part = odin3_vec_cat(&parts, k - 1U);
        walk_item *item = odin3_name_map_ref_number(*part, 'r', &num) ? odin3_vec_push(stack) : NULL;
        if (item != NULL) {
            *item = (walk_item){num, depth + 1U};
        } else if (odin3_name_map_ref_number(*part, 'r', &num)) {
            st = ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_vec_free(&parts);
    return st;
}

/* The records reachable from the first record of recs, in the order of Node.provenance(). */
static odin3_status walk_records(const names_v2 *v2, marks *m, odin3_bytes recs,
                                 odin3_vec *order) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(walk_item));
    uint32_t first = 0;
    odin3_bytes head = recs;
    const char *comma = recs.len > 0 ? memchr(recs.ptr, ',', recs.len) : NULL;
    head.len = comma != NULL ? (size_t)(comma - (const char *)recs.ptr) : recs.len;
    walk_item *start = odin3_name_map_ref_number(head, 'r', &first) ? odin3_vec_push(&stack) : NULL;
    odin3_status st = ODIN3_OK;
    if (start != NULL) {
        *start = (walk_item){first, 0};
    }
    while (st == ODIN3_OK && stack.len > 0) {
        walk_item item = *(const walk_item *)odin3_vec_cat(&stack, stack.len - 1U);
        odin3_vec_pop(&stack);
        const odin3_nm_line *line = line_at(v2, NV2_R, item.rec);
        if (line == NULL || !mark_new(v2, m, NV2_R, item.rec)) {
            continue;
        }
        walk_item *slot = odin3_vec_push(order);
        if (slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
            break;
        }
        *slot = item;
        st = push_parents(line, item.depth, &stack);
    }
    odin3_vec_free(&stack);
    return st;
}

/* The number of the construct of a SOURCE or IMPORTED record (0 when it has none). */
static uint32_t leaf_construct(const odin3_nm_line *line) {
    uint32_t num = 0;
    odin3_bytes cons = line->f[NM_R_CONS];
    const char *comma = cons.len > 0 ? memchr(cons.ptr, ',', cons.len) : NULL;
    if (comma != NULL) {
        cons.len = (size_t)(comma - (const char *)cons.ptr);
    }
    bool leaf = line->f[NM_R_KIND].len == 1 && *(const char *)line->f[NM_R_KIND].ptr != 'D';
    return leaf && odin3_name_map_ref_number(cons, 'c', &num) ? num : 0;
}

/* Adds construct num and its ancestors (module last) not yet reached to chain. */
static odin3_status chain_up(const names_v2 *v2, marks *m, uint32_t num, odin3_vec *chain) {
    for (size_t steps = v2->tab[NV2_C].len; num != 0 && steps > 0; steps--) {
        const odin3_nm_line *line = line_at(v2, NV2_C, num);
        if (line == NULL || !mark_new(v2, m, NV2_C, num)) {
            return ODIN3_OK;
        }
        uint32_t *slot = odin3_vec_push(chain);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = num;
        uint32_t parent = 0;
        num = odin3_name_map_ref_number(line->f[NM_C_PARENT], 'c', &parent) ? parent : 0;
    }
    return ODIN3_OK;
}

static odin3_status build_chain(const names_v2 *v2, marks *m, const odin3_vec *order,
                                odin3_vec *chain) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < order->len; i++) {
        const walk_item *item = odin3_vec_cat(order, i);
        const odin3_nm_line *line = line_at(v2, NV2_R, item->rec);
        uint32_t num = line != NULL ? leaf_construct(line) : 0;
        st = num != 0 ? chain_up(v2, m, num, chain) : ODIN3_OK;
    }
    return st;
}

/* Marks the file in an `f<n>:span` origin or an `f<n>` field. */
static void mark_file(const names_v2 *v2, marks *m, odin3_bytes tok) {
    const char *colon = tok.len > 0 ? memchr(tok.ptr, ':', tok.len) : NULL;
    uint32_t num = 0;
    if (colon != NULL) {
        tok.len = (size_t)(colon - (const char *)tok.ptr);
    }
    if (odin3_name_map_ref_number(tok, 'f', &num)) {
        (void)mark_new(v2, m, NV2_F, num);
    }
}

static odin3_status mark_origins(const names_v2 *v2, marks *m, const odin3_nm_line *line) {
    odin3_vec parts;
    odin3_vec_init(&parts, sizeof(odin3_bytes));
    odin3_status st = split_list(line->f[NM_R_ORIGINS], '|', &parts);
    for (size_t k = 0; st == ODIN3_OK && k < parts.len; k++) {
        mark_file(v2, m, *(const odin3_bytes *)odin3_vec_cat(&parts, k));
    }
    odin3_vec_free(&parts);
    return st;
}

/* Marks the run of a step `t<n>.<op>`. */
static void mark_run(const names_v2 *v2, marks *m, odin3_bytes step) {
    const char *dot = step.len > 0 ? memchr(step.ptr, '.', step.len) : NULL;
    uint32_t num = 0;
    if (dot != NULL) {
        step.len = (size_t)(dot - (const char *)step.ptr);
    }
    if (odin3_name_map_ref_number(step, 't', &num)) {
        (void)mark_new(v2, m, NV2_T, num);
    }
}

/* Appends the marked lines of a table in number order, indented. */
static odin3_status put_marked(const names_v2 *v2, const marks *m, names_v2_tab tab,
                               uint32_t indent, odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    for (uint32_t num = 1; st == ODIN3_OK && num <= v2->tab[tab].len; num++) {
        st = m->seen[tab][num] != 0 ? put_line(out, line_at(v2, tab, num), indent) : ODIN3_OK;
    }
    return st;
}

/* The lookup text: the records (by depth), the chain, then the files and runs they name. */
static odin3_status print_lookup(const names_v2 *v2, const odin3_vec *order,
                                 const odin3_vec *chain, odin3_strbuf *out) {
    marks reach = {{NULL}};
    odin3_status st = marks_init(&reach, v2);
    for (size_t i = 0; st == ODIN3_OK && i < order->len; i++) {
        const walk_item *item = odin3_vec_cat(order, i);
        const odin3_nm_line *line = line_at(v2, NV2_R, item->rec);
        st = put_line(out, line, INDENT_BASE + INDENT_STEP * item->depth);
        st = st == ODIN3_OK ? mark_origins(v2, &reach, line) : st;
        mark_run(v2, &reach, line->f[NM_R_STEP]);
    }
    for (size_t i = 0; st == ODIN3_OK && i < chain->len; i++) {
        uint32_t num = *(const uint32_t *)odin3_vec_cat(chain, i);
        const odin3_nm_line *line = line_at(v2, NV2_C, num);
        st = put_line(out, line, INDENT_BASE);
        mark_file(v2, &reach, line->f[NM_C_FILE]);
    }
    st = st == ODIN3_OK ? put_marked(v2, &reach, NV2_F, INDENT_BASE, out) : st;
    st = st == ODIN3_OK ? put_marked(v2, &reach, NV2_T, INDENT_BASE, out) : st;
    marks_free(&reach);
    return st;
}

/* The chain text: the construct lines, then the file lines they name. */
static odin3_status print_chain(const names_v2 *v2, const odin3_vec *chain, odin3_strbuf *out) {
    marks reach = {{NULL}};
    odin3_status st = marks_init(&reach, v2);
    for (size_t i = 0; st == ODIN3_OK && i < chain->len; i++) {
        const odin3_nm_line *line = line_at(v2, NV2_C, *(const uint32_t *)odin3_vec_cat(chain, i));
        st = put_line(out, line, 0);
        mark_file(v2, &reach, line->f[NM_C_FILE]);
    }
    st = st == ODIN3_OK ? put_marked(v2, &reach, NV2_F, 0, out) : st;
    marks_free(&reach);
    return st;
}

/* One matching O or N line. */
static odin3_status print_object(const names_v2 *v2, const odin3_nm_line *obj, names_v2_mode mode,
                                 odin3_strbuf *out) {
    odin3_bytes recs = obj->type == 'O' ? obj->f[NM_O_RECS] : obj->f[NM_N_RECS];
    marks m = {{NULL}};
    odin3_vec order;
    odin3_vec chain;
    odin3_vec_init(&order, sizeof(walk_item));
    odin3_vec_init(&chain, sizeof(uint32_t));
    odin3_status st = marks_init(&m, v2);
    st = st == ODIN3_OK ? walk_records(v2, &m, recs, &order) : st;
    st = st == ODIN3_OK ? build_chain(v2, &m, &order, &chain) : st;
    if (st == ODIN3_OK && mode == NV2_LOOKUP) {
        st = put_line(out, obj, 0);
        st = st == ODIN3_OK ? print_lookup(v2, &order, &chain, out) : st;
    } else if (st == ODIN3_OK) {
        st = print_chain(v2, &chain, out);
    }
    marks_free(&m);
    odin3_vec_free(&order);
    odin3_vec_free(&chain);
    return st;
}

odin3_status names_v2_print(const names_v2 *v2, odin3_bytes value, names_v2_mode mode,
                            odin3_strbuf *out, uint32_t *matches) {
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    *matches = 0;
    odin3_status st = odin3_name_map_escape(&want, value);
    for (size_t i = 0; st == ODIN3_OK && i < v2->map->lines.len; i++) {
        const odin3_nm_line *line = odin3_vec_cat(&v2->map->lines, i);
        if ((line->type != 'O' && line->type != 'N') || line->f[NM_O_SHORT].len != want.len ||
            (want.len > 0 && memcmp(line->f[NM_O_SHORT].ptr, want.data, want.len) != 0)) {
            continue;
        }
        (*matches)++;
        st = print_object(v2, line, mode, out);
    }
    odin3_strbuf_free(&want);
    return st;
}
