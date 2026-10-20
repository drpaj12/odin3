/* json_parse.c — the iterative value-tree builder (open containers on an explicit stack). */
#include "frontends/json/json_lex.h"
#include "util/alloc.h"

#include <string.h>

struct odin3_json_node {
    uint8_t kind;
    uint32_t line, col;         /* first byte of the value */
    uint32_t key_line, key_col; /* first byte of the member key */
    uint32_t len;               /* string length, or child count */
    union {
        struct odin3_json_node *first; /* ARRAY, OBJECT */
        const char *str;               /* STRING */
        int64_t num;                   /* INT */
    } u;
    struct odin3_json_node *next;
    const char *key;
};

typedef struct json_frame {
    odin3_json_node *node, *last;
    uint32_t serial;
} json_frame;

/* Open-addressing set of (object serial, key) for the duplicate-key check. */
typedef struct dup_ent {
    const odin3_json_node *node;
    uint64_t hash;
    uint32_t serial;
} dup_ent;

typedef struct dup_tab {
    dup_ent *ent;
    size_t cap, used;
} dup_tab;

typedef struct json_parser {
    json_lex lex;
    odin3_vec frames;
    dup_tab dups;
    uint32_t next_serial;
    odin3_json_doc *doc;
} json_parser;

enum { DUP_MIN_CAP = 64, FNV_PRIME_SHIFT = 40 };

static odin3_json_limits g_limits = {ODIN3_JSON_MAX_BYTES, ODIN3_JSON_MAX_STRING,
                                     ODIN3_JSON_MAX_DEPTH};

void odin3_json_test_set_limits(const odin3_json_limits *lim) {
    odin3_json_limits dflt = {ODIN3_JSON_MAX_BYTES, ODIN3_JSON_MAX_STRING, ODIN3_JSON_MAX_DEPTH};
    g_limits = lim != NULL ? *lim : dflt;
}

static uint64_t key_hash(const char *key, uint32_t serial) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325) ^ serial;
    for (const unsigned char *ch = (const unsigned char *)key; *ch != 0; ch++) {
        hash = (hash ^ *ch) * UINT64_C(0x100000001b3);
    }
    return hash ^ (hash >> FNV_PRIME_SHIFT);
}

static odin3_status dup_grow(json_parser *ps) {
    dup_tab *tab = &ps->dups;
    size_t cap = tab->cap == 0 ? DUP_MIN_CAP : tab->cap * 2;
    dup_ent *ent = odin3_util_calloc(cap * sizeof *ent);
    if (ent == NULL) {
        return json_fail(&ps->lex, json_here(&ps->lex), ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    for (size_t i = 0; i < tab->cap; i++) {
        if (tab->ent[i].node == NULL) {
            continue;
        }
        size_t slot = tab->ent[i].hash & (cap - 1);
        while (ent[slot].node != NULL) {
            slot = (slot + 1) & (cap - 1);
        }
        ent[slot] = tab->ent[i];
    }
    odin3_util_free(tab->ent);
    tab->ent = ent;
    tab->cap = cap;
    return ODIN3_OK;
}

/* Adds node (a keyed member of object `serial`); *dup is the earlier member with that key. */
static odin3_status dup_insert(json_parser *ps, uint32_t serial, const odin3_json_node *node,
                               const odin3_json_node **dup) {
    dup_tab *tab = &ps->dups;
    *dup = NULL;
    if ((tab->used + 1) * 2 > tab->cap) {
        odin3_status st = dup_grow(ps);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    uint64_t hash = key_hash(node->key, serial);
    size_t slot = hash & (tab->cap - 1);
    while (tab->ent[slot].node != NULL) {
        const dup_ent *cur = &tab->ent[slot];
        if (cur->hash == hash && cur->serial == serial && strcmp(cur->node->key, node->key) == 0) {
            *dup = cur->node;
            return ODIN3_OK;
        }
        slot = (slot + 1) & (tab->cap - 1);
    }
    tab->ent[slot] = (dup_ent){node, hash, serial};
    tab->used++;
    return ODIN3_OK;
}

static odin3_json_node *new_node(json_parser *ps, odin3_json_kind kind, odin3_json_loc pos) {
    odin3_json_node *node = odin3_arena_alloc(ps->lex.arena, sizeof *node);
    if (node == NULL) {
        (void)json_fail(&ps->lex, pos, ODIN3_ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    node->kind = (uint8_t)kind;
    node->line = pos.line;
    node->col = pos.col;
    return node;
}

/* Links node as the last child of the open container, or makes it the root. */
static odin3_status attach(json_parser *ps, odin3_json_node *node) {
    if (ps->frames.len == 0) {
        ps->doc->root = node;
        return ODIN3_OK;
    }
    json_frame *top = odin3_vec_at(&ps->frames, ps->frames.len - 1);
    if (top->node->kind == ODIN3_JSON_OBJECT) {
        const odin3_json_node *dup = NULL;
        odin3_status st = dup_insert(ps, top->serial, node, &dup);
        if (st != ODIN3_OK) {
            return st;
        }
        if (dup != NULL) {
            odin3_json_loc pos = {node->key_line, node->key_col};
            return json_fail(&ps->lex, pos, ODIN3_ERR_PARSE,
                             "duplicate key \"%.40s\" (first at line %u)", node->key,
                             (unsigned)dup->key_line);
        }
    }
    if (top->last == NULL) {
        top->node->u.first = node;
    } else {
        top->last->next = node;
    }
    top->last = node;
    top->node->len++;
    return ODIN3_OK;
}

static odin3_status read_key(json_parser *ps, const char **key, odin3_json_loc *kpos) {
    json_lex *lex = &ps->lex;
    char *text = NULL;
    uint32_t len = 0;
    *kpos = json_here(lex);
    if (json_peek(lex) != '"') {
        return json_fail(lex, *kpos, ODIN3_ERR_PARSE, "expected a string key");
    }
    odin3_status st = json_lex_string(lex, &text, &len);
    if (st != ODIN3_OK) {
        return st;
    }
    json_skip_ws(lex);
    if (json_peek(lex) != ':') {
        return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "expected ':' after key");
    }
    json_advance(lex);
    json_skip_ws(lex);
    *key = text;
    return ODIN3_OK;
}

static odin3_status read_literal(json_parser *ps, odin3_json_node *node) {
    static const struct {
        const char *word;
        odin3_json_kind kind;
    } words[] = {{"true", ODIN3_JSON_TRUE}, {"false", ODIN3_JSON_FALSE}, {"null", ODIN3_JSON_NULL}};
    json_lex *lex = &ps->lex;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        size_t wlen = strlen(words[i].word);
        if (lex->len - lex->pos >= wlen && memcmp(lex->text + lex->pos, words[i].word, wlen) == 0) {
            node->kind = (uint8_t)words[i].kind;
            lex->pos += wlen;
            return ODIN3_OK;
        }
    }
    return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "unexpected character '%c'",
                     json_peek(lex));
}

static odin3_status read_scalar(json_parser *ps, odin3_json_node *node, int ch) {
    json_lex *lex = &ps->lex;
    if (ch == '"') {
        char *text = NULL;
        node->kind = ODIN3_JSON_STRING;
        odin3_status st = json_lex_string(lex, &text, &node->len);
        node->u.str = text;
        return st;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        node->kind = ODIN3_JSON_INT;
        return json_lex_int(lex, &node->u.num);
    }
    return read_literal(ps, node);
}

/* Opens an array or object at the cursor; *opened is set when it has members to read. */
static odin3_status open_container(json_parser *ps, odin3_json_node *node, bool *opened) {
    json_lex *lex = &ps->lex;
    json_frame *frame = odin3_vec_push(&ps->frames);
    if (frame == NULL) {
        return json_fail(lex, json_here(lex), ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    frame->node = node;
    frame->serial = ++ps->next_serial;
    int closer = node->kind == ODIN3_JSON_OBJECT ? '}' : ']';
    json_advance(lex);
    json_skip_ws(lex);
    if (json_peek(lex) == closer) {
        json_advance(lex);
        odin3_vec_pop(&ps->frames);
        return ODIN3_OK;
    }
    *opened = true;
    return ODIN3_OK;
}

/* The member key when the open container is an object; key stays NULL otherwise. */
static odin3_status read_member_key(json_parser *ps, const char **key, odin3_json_loc *kpos) {
    if (ps->frames.len == 0) {
        return ODIN3_OK;
    }
    const json_frame *top = odin3_vec_cat(&ps->frames, ps->frames.len - 1);
    return top->node->kind == ODIN3_JSON_OBJECT ? read_key(ps, key, kpos) : ODIN3_OK;
}

/* One value (with its key when the open container is an object). */
static odin3_status read_element(json_parser *ps, bool *opened) {
    json_lex *lex = &ps->lex;
    const char *key = NULL;
    odin3_json_loc kpos = {0, 0};
    *opened = false;
    odin3_status st = read_member_key(ps, &key, &kpos);
    if (st != ODIN3_OK) {
        return st;
    }
    int ch = json_peek(lex);
    odin3_json_loc pos = json_here(lex);
    if (ch < 0) {
        return json_fail(lex, pos, ODIN3_ERR_PARSE, "unexpected end of file");
    }
    bool container = ch == '{' || ch == '[';
    if (container && ps->frames.len >= lex->lim.max_depth) {
        return json_fail(lex, pos, ODIN3_ERR_PARSE, "JSON nested deeper than %u",
                         (unsigned)lex->lim.max_depth);
    }
    odin3_json_node *node = new_node(ps, ch == '{' ? ODIN3_JSON_OBJECT : ODIN3_JSON_ARRAY, pos);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    node->key = key;
    node->key_line = kpos.line;
    node->key_col = kpos.col;
    st = container ? ODIN3_OK : read_scalar(ps, node, ch);
    if (st == ODIN3_OK) {
        st = attach(ps, node);
    }
    if (st == ODIN3_OK && container) {
        st = open_container(ps, node, opened);
    }
    return st;
}

/* After a value: closes finished containers; *done when the root is complete. */
static odin3_status after_value(json_parser *ps, bool *done) {
    json_lex *lex = &ps->lex;
    *done = false;
    while (ps->frames.len > 0) {
        const json_frame *top = odin3_vec_cat(&ps->frames, ps->frames.len - 1);
        int closer = top->node->kind == ODIN3_JSON_OBJECT ? '}' : ']';
        json_skip_ws(lex);
        int ch = json_peek(lex);
        if (ch == ',') {
            json_advance(lex);
            json_skip_ws(lex);
            if (json_peek(lex) == closer) {
                return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "trailing comma");
            }
            return ODIN3_OK;
        }
        if (ch < 0) {
            return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "unexpected end of file");
        }
        if (ch != closer) {
            return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "expected ',' or '%c'", closer);
        }
        json_advance(lex);
        odin3_vec_pop(&ps->frames);
    }
    *done = true;
    return ODIN3_OK;
}

static odin3_status parse_loop(json_parser *ps) {
    json_lex *lex = &ps->lex;
    bool done = false;
    json_skip_ws(lex);
    while (!done) {
        bool opened = false;
        odin3_status st = read_element(ps, &opened);
        if (st == ODIN3_OK && !opened) {
            st = after_value(ps, &done);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    json_skip_ws(lex);
    if (lex->pos != lex->len) {
        return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE,
                         "unexpected data after the root value");
    }
    return ODIN3_OK;
}

static void parser_release(json_parser *ps) {
    odin3_vec_free(&ps->frames);
    odin3_vec_free(&ps->lex.scratch);
    odin3_util_free(ps->dups.ent);
}

odin3_status odin3_json_parse(const uint8_t *text, size_t len, const odin3_json_limits *lim,
                              odin3_json_doc *doc) {
    if (doc == NULL || (text == NULL && len > 0)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(doc, 0, sizeof *doc);
    json_parser ps = {.doc = doc};
    odin3_vec_init(&ps.frames, sizeof(json_frame));
    odin3_vec_init(&ps.lex.scratch, 1);
    json_lex_init(&ps.lex, text, len);
    ps.lex.lim = lim != NULL ? *lim : g_limits;
    ps.lex.err = &doc->error;
    odin3_status st = ODIN3_OK;
    if (len > ps.lex.lim.max_bytes) {
        st = json_fail(&ps.lex, json_here(&ps.lex), ODIN3_ERR_PARSE, "file larger than %zu bytes",
                       ps.lex.lim.max_bytes);
    } else {
        doc->arena = odin3_arena_create(0);
        ps.lex.arena = doc->arena;
        st = doc->arena != NULL
                 ? parse_loop(&ps)
                 : json_fail(&ps.lex, json_here(&ps.lex), ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    parser_release(&ps);
    if (st != ODIN3_OK) {
        odin3_json_doc_free(doc);
    }
    return st;
}

void odin3_json_doc_free(odin3_json_doc *doc) {
    if (doc == NULL) {
        return;
    }
    odin3_arena_destroy(doc->arena);
    doc->arena = NULL;
    doc->root = NULL;
}

odin3_json_kind odin3_json_kind_of(const odin3_json_node *node) {
    return (odin3_json_kind)node->kind;
}

int64_t odin3_json_int(const odin3_json_node *node) {
    return node->kind == ODIN3_JSON_INT ? node->u.num : 0;
}

const char *odin3_json_string(const odin3_json_node *node, size_t *len) {
    if (node->kind != ODIN3_JSON_STRING) {
        return NULL;
    }
    if (len != NULL) {
        *len = node->len;
    }
    return node->u.str;
}

const odin3_json_node *odin3_json_first(const odin3_json_node *node) {
    bool container = node->kind == ODIN3_JSON_ARRAY || node->kind == ODIN3_JSON_OBJECT;
    return container ? node->u.first : NULL;
}

const odin3_json_node *odin3_json_next(const odin3_json_node *node) {
    return node->next;
}

size_t odin3_json_count(const odin3_json_node *node) {
    bool container = node->kind == ODIN3_JSON_ARRAY || node->kind == ODIN3_JSON_OBJECT;
    return container ? node->len : 0;
}

const char *odin3_json_key(const odin3_json_node *node) {
    return node->key;
}

const odin3_json_node *odin3_json_member(const odin3_json_node *obj, const char *key) {
    if (obj->kind != ODIN3_JSON_OBJECT) {
        return NULL;
    }
    for (const odin3_json_node *kid = obj->u.first; kid != NULL; kid = kid->next) {
        if (strcmp(kid->key, key) == 0) {
            return kid;
        }
    }
    return NULL;
}

odin3_json_loc odin3_json_pos(const odin3_json_node *node) {
    odin3_json_loc loc = {node->line, node->col};
    return loc;
}

odin3_json_loc odin3_json_key_pos(const odin3_json_node *node) {
    odin3_json_loc loc = {node->key_line, node->key_col};
    return loc;
}
