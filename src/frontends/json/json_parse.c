/* json_parse.c — the iterative value-tree builder (open containers on an explicit stack). */
#include "frontends/json/json_lex.h"
#include "util/vec.h"

#include <stdlib.h>
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
    size_t key_base; /* index in json_parser.keys of this object's first member */
} json_frame;

typedef struct key_ref {
    const odin3_json_node *node;
} key_ref;

typedef struct dup_pair {
    const odin3_json_node *earlier, *later;
} dup_pair;

typedef struct json_parser {
    json_lex lex;
    odin3_vec frames; /* json_frame, at most max_depth */
    odin3_vec keys;   /* key_ref: members of the open objects, a stack */
    size_t nodes;
    odin3_json_doc *doc;
} json_parser;

enum { KEY_SHOWN = 40, DEL_BYTE = 0x7F, FIRST_PRINTABLE = 0x20 };

static const char k_question = '?';

static odin3_json_limits g_limits = {ODIN3_JSON_MAX_BYTES, ODIN3_JSON_MAX_STRING,
                                     ODIN3_JSON_MAX_NODES, ODIN3_JSON_MAX_DEPTH};

odin3_json_limits odin3_json_default_limits(void) {
    odin3_json_limits dflt = {ODIN3_JSON_MAX_BYTES, ODIN3_JSON_MAX_STRING, ODIN3_JSON_MAX_NODES,
                              ODIN3_JSON_MAX_DEPTH};
    return dflt;
}

void odin3_json_test_set_limits(const odin3_json_limits *lim) {
    g_limits = lim != NULL ? *lim : odin3_json_default_limits();
}

static odin3_status new_node(json_parser *ps, odin3_json_kind kind, odin3_json_loc pos,
                             odin3_json_node **out) {
    if (ps->nodes >= ps->lex.lim.max_nodes) {
        return json_fail(&ps->lex, pos, ODIN3_ERR_PARSE, "more than %zu values in the file",
                         ps->lex.lim.max_nodes);
    }
    odin3_json_node *node = odin3_arena_alloc(ps->lex.arena, sizeof *node);
    if (node == NULL) {
        return json_fail(&ps->lex, pos, ODIN3_ERR_NO_MEMORY, "out of memory");
    }
    ps->nodes++;
    node->kind = (uint8_t)kind;
    node->line = pos.line;
    node->col = pos.col;
    *out = node;
    return ODIN3_OK;
}

/* Members sort by key, equal keys in file order. */
static int member_cmp(const void *lhs, const void *rhs) {
    const odin3_json_node *left = ((const key_ref *)lhs)->node;
    const odin3_json_node *right = ((const key_ref *)rhs)->node;
    int diff = strcmp(left->key, right->key);
    if (diff != 0) {
        return diff;
    }
    if (left->key_line != right->key_line) {
        return left->key_line < right->key_line ? -1 : 1;
    }
    return (left->key_col > right->key_col) - (left->key_col < right->key_col);
}

static bool later_key(const odin3_json_node *cand, const odin3_json_node *best) {
    return best == NULL || cand->key_line < best->key_line ||
           (cand->key_line == best->key_line && cand->key_col < best->key_col);
}

/* Prints a key with every byte outside printable ASCII shown as '?', cut to KEY_SHOWN. */
static void show_key(char *dst, const char *key) {
    size_t shown = 0;
    while (shown < KEY_SHOWN && key[shown] != '\0') {
        unsigned char byte = (unsigned char)key[shown];
        dst[shown] = k_question;
        if (byte >= FIRST_PRINTABLE && byte < DEL_BYTE) {
            dst[shown] = (char)byte;
        }
        shown++;
    }
    dst[shown] = '\0';
}

/*
 * Finds the first-in-file duplicate among the sorted members: pair->later is the second
 * occurrence, pair->earlier the first. Both NULL when all keys differ.
 */
static void find_duplicate(const key_ref *sorted, size_t count, dup_pair *pair) {
    pair->earlier = NULL;
    pair->later = NULL;
    for (size_t i = 1; i < count; i++) {
        bool same = strcmp(sorted[i - 1].node->key, sorted[i].node->key) == 0;
        bool group_head = i == 1 || strcmp(sorted[i - 2].node->key, sorted[i].node->key) != 0;
        if (same && group_head && later_key(sorted[i].node, pair->later)) {
            pair->earlier = sorted[i - 1].node;
            pair->later = sorted[i].node;
        }
    }
}

/* Closes the object on top of the frame stack: duplicate-key check, then drops its keys. */
static odin3_status close_object(json_parser *ps, const json_frame *frame) {
    size_t count = ps->keys.len - frame->key_base;
    dup_pair pair = {NULL, NULL};
    if (count > 1) {
        key_ref *slice = odin3_vec_at(&ps->keys, frame->key_base);
        qsort(slice, count, sizeof *slice, member_cmp);
        find_duplicate(slice, count, &pair);
    }
    while (ps->keys.len > frame->key_base) {
        odin3_vec_pop(&ps->keys);
    }
    if (pair.later == NULL) {
        return ODIN3_OK;
    }
    char shown[KEY_SHOWN + 1];
    show_key(shown, pair.later->key);
    odin3_json_loc pos = {pair.later->key_line, pair.later->key_col};
    return json_fail(&ps->lex, pos, ODIN3_ERR_PARSE, "duplicate key \"%s\" (first at line %u)",
                     shown, (unsigned)pair.earlier->key_line);
}

/* Pops the top frame; an object also checks its keys. */
static odin3_status pop_frame(json_parser *ps) {
    json_frame frame = *(const json_frame *)odin3_vec_cat(&ps->frames, ps->frames.len - 1);
    odin3_vec_pop(&ps->frames);
    return frame.node->kind == ODIN3_JSON_OBJECT ? close_object(ps, &frame) : ODIN3_OK;
}

/* Links node as the last child of the open container, or makes it the root. */
static odin3_status attach(json_parser *ps, odin3_json_node *node) {
    if (ps->frames.len == 0) {
        ps->doc->root = node;
        return ODIN3_OK;
    }
    json_frame *top = odin3_vec_at(&ps->frames, ps->frames.len - 1);
    if (top->node->kind == ODIN3_JSON_OBJECT) {
        key_ref *slot = odin3_vec_push(&ps->keys);
        if (slot == NULL) {
            return json_fail(&ps->lex, json_here(&ps->lex), ODIN3_ERR_NO_MEMORY, "out of memory");
        }
        slot->node = node;
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
    int byte = json_peek(lex);
    if (byte >= FIRST_PRINTABLE && byte < DEL_BYTE) {
        return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "unexpected character '%c'", byte);
    }
    return json_fail(lex, json_here(lex), ODIN3_ERR_PARSE, "unexpected byte 0x%02X",
                     (unsigned)byte);
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
    frame->key_base = ps->keys.len;
    int closer = node->kind == ODIN3_JSON_OBJECT ? '}' : ']';
    json_advance(lex);
    json_skip_ws(lex);
    if (json_peek(lex) == closer) {
        json_advance(lex);
        return pop_frame(ps);
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
    odin3_json_node *node = NULL;
    st = new_node(ps, ch == '{' ? ODIN3_JSON_OBJECT : ODIN3_JSON_ARRAY, pos, &node);
    if (st != ODIN3_OK) {
        return st;
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
        odin3_status st = pop_frame(ps);
        if (st != ODIN3_OK) {
            return st;
        }
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
    odin3_vec_free(&ps->keys);
}

odin3_status odin3_json_parse(const uint8_t *text, size_t len, const odin3_json_limits *lim,
                              odin3_json_doc *doc) {
    if (doc == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(doc, 0, sizeof *doc);
    if (text == NULL && len > 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    json_parser ps = {.doc = doc};
    odin3_vec_init(&ps.frames, sizeof(json_frame));
    odin3_vec_init(&ps.keys, sizeof(key_ref));
    json_lex_init(&ps.lex, text, len);
    ps.lex.lim = lim != NULL ? *lim : g_limits;
    ps.lex.lim.max_bytes = ps.lex.lim.max_bytes < UINT32_MAX ? ps.lex.lim.max_bytes : UINT32_MAX;
    ps.lex.lim.max_string = ps.lex.lim.max_string < UINT32_MAX ? ps.lex.lim.max_string : UINT32_MAX;
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
