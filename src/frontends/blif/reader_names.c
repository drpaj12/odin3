/*
 * reader_names.c — see reader_names.h.
 */
#include "frontends/blif/reader_names.h"

#include "backends/blif/vtr.h"
#include "frontends/name_resolve.h"
#include "util/alloc.h"
#include "util/str.h"
#include "util/vec.h"

#include <string.h>

struct blif_names {
    odin3_name_resolver map;
    odin3_name_miss miss;
    bool count;
    odin3_strbuf model; /* the open model's name */
    odin3_strbuf pool;  /* the replaced tokens of the line */
    odin3_strbuf legal; /* scratch for VB-10 */
    odin3_vec tokens;   /* odin3_bytes: the rewritten line */
    odin3_vec reps;     /* rep: where each replacement sits in the pool */
};

typedef struct rep {
    uint32_t token;
    size_t off, len;
} rep;

blif_names *blif_names_create(const char *path, odin3_status *st) {
    blif_names *names = odin3_util_calloc(sizeof *names);
    odin3_strbuf_init(&names->model);
    odin3_strbuf_init(&names->pool);
    odin3_strbuf_init(&names->legal);
    odin3_vec_init(&names->tokens, sizeof(odin3_bytes));
    odin3_vec_init(&names->reps, sizeof(rep));
    *st = odin3_name_resolver_open(&names->map, "read_blif", path);
    if (*st != ODIN3_OK) {
        blif_names_destroy(names);
        return NULL;
    }
    return names;
}

void blif_names_destroy(blif_names *names) {
    if (names == NULL) {
        return;
    }
    odin3_name_resolver_close(&names->map);
    odin3_strbuf_free(&names->model);
    odin3_strbuf_free(&names->pool);
    odin3_strbuf_free(&names->legal);
    odin3_vec_free(&names->tokens);
    odin3_vec_free(&names->reps);
    odin3_util_free(names);
}

void blif_names_begin(blif_names *names, bool count) {
    names->count = count;
    odin3_strbuf_clear(&names->model);
}

void blif_names_report(const blif_names *names, const char *path) {
    odin3_name_miss_report(&names->miss, "read_blif", path);
}

static bool is_word(odin3_bytes tok, const char *word) {
    size_t word_len = strlen(word);
    return tok.len == word_len && memcmp(tok.ptr, word, word_len) == 0;
}

/* The tokens of a directive that can hold a net name: first token and one past the last. */
static bool name_range(const odin3_blif_line *ln, uint32_t *first, uint32_t *last) {
    const odin3_bytes dir = ln->tokens[0];
    *first = 1;
    *last = ln->count;
    if (is_word(dir, ".names") || is_word(dir, ".subckt")) {
        return true;
    }
    if (is_word(dir, ".latch")) {
        return true; /* in, out, and the control (a type word is never hash-shaped) */
    }
    return false;
}

/* The name part of a token: all of it, or the actual of `formal=actual` in a .subckt. */
static odin3_bytes name_part(odin3_bytes tok, bool subckt, size_t *prefix) {
    *prefix = 0;
    if (subckt) {
        const unsigned char *eq = memchr(tok.ptr, '=', tok.len);
        if (eq == NULL) {
            return (odin3_bytes){NULL, 0};
        }
        *prefix = (size_t)(eq - (const unsigned char *)tok.ptr) + 1;
    }
    return (odin3_bytes){(const unsigned char *)tok.ptr + *prefix, tok.len - *prefix};
}

/* Replaces token i when the map has its name: appends the new token (NUL included) to the pool. */
static odin3_status replace_one(blif_names *names, const odin3_blif_line *ln, uint32_t index,
                                bool subckt) {
    size_t prefix = 0;
    odin3_bytes part = name_part(ln->tokens[index], subckt, &prefix);
    odin3_bytes full = {NULL, 0};
    if (part.ptr == NULL || !odin3_name_hash_shaped(part)) {
        return ODIN3_OK;
    }
    if (!odin3_name_resolver_find(&names->map, (odin3_bytes){names->model.data, names->model.len},
                                  part, &full)) {
        if (names->count) {
            odin3_name_miss_note(&names->miss, part, ln->line);
        }
        return ODIN3_OK;
    }
    rep *slot = odin3_vec_push(&names->reps);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->token = index;
    slot->off = names->pool.len;
    odin3_strbuf_clear(&names->legal);
    odin3_status st =
        odin3_strbuf_append(&names->pool, (odin3_bytes){ln->tokens[index].ptr, prefix});
    st = st == ODIN3_OK ? odin3_blif_vtr_legal_name(full, &names->legal) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_append(&names->pool, (odin3_bytes){names->legal.data, names->legal.len})
             : st;
    slot->len = names->pool.len - slot->off;
    return st == ODIN3_OK ? odin3_strbuf_append(&names->pool, (odin3_bytes){"\0", 1}) : st;
}

/* Points ln at a copy of its tokens in which the replaced ones view the pool. */
static odin3_status rebuild(blif_names *names, odin3_blif_line *ln) {
    odin3_vec_clear(&names->tokens);
    for (uint32_t i = 0; i < ln->count; i++) {
        odin3_bytes *slot = odin3_vec_push(&names->tokens);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = ln->tokens[i];
    }
    for (size_t k = 0; k < names->reps.len; k++) {
        const rep *where = odin3_vec_cat(&names->reps, k);
        odin3_bytes *tok = odin3_vec_at(&names->tokens, where->token);
        *tok = (odin3_bytes){names->pool.data + where->off, where->len};
    }
    ln->tokens = names->tokens.data;
    return ODIN3_OK;
}

odin3_status blif_names_apply(blif_names *names, odin3_blif_line *ln) {
    uint32_t first = 0;
    uint32_t last = 0;
    if (ln->count == 0) {
        return ODIN3_OK;
    }
    if (is_word(ln->tokens[0], ".model")) {
        odin3_strbuf_clear(&names->model);
        return ln->count > 1 ? odin3_strbuf_append(&names->model, ln->tokens[1]) : ODIN3_OK;
    }
    if (!name_range(ln, &first, &last)) {
        return ODIN3_OK;
    }
    bool subckt = is_word(ln->tokens[0], ".subckt");
    first = subckt ? 2 : first;
    odin3_vec_clear(&names->reps);
    odin3_strbuf_clear(&names->pool);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = first; st == ODIN3_OK && i < last; i++) {
        st = replace_one(names, ln, i, subckt);
    }
    return st == ODIN3_OK && names->reps.len > 0 ? rebuild(names, ln) : st;
}
