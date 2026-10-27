/*
 * reader_names.c — see reader_names.h.
 */
#include "frontends/blif/reader_names.h"

#include "backends/blif/vtr.h"
#include "frontends/name_resolve.h"
#include "util/alloc.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <string.h>

enum { PAIR_SHIFT = 32, HEX_TEN = 10, HEX_BASE = 16 };

struct blif_names {
    odin3_name_resolver map;
    odin3_name_miss miss;
    bool count;
    odin3_strbuf model;     /* the open model's name as written */
    odin3_strbuf model_dec; /* the same with its VB-10 `%XX` escapes decoded, empty when none */
    odin3_strbuf pool;      /* the replaced tokens of the line */
    odin3_strbuf legal;     /* scratch for VB-10 */
    odin3_vec tokens;       /* odin3_bytes: the rewritten line */
    odin3_vec reps;         /* rep: where each replacement sits in the pool */
    /* Without --hdl-locs (the reader then keeps a name as written) a full name that equals a raw
     * net of its model moves aside: `moved` is the final text of each replaced hash name. */
    bool move;
    odin3_strtab *tab;   /* models and names of the prescan */
    odin3_u64map *taken; /* model << 32 | name: spelled by a raw token or claimed by a hash */
    odin3_u64map *moved; /* model << 32 | hash name -> its final name */
    uint32_t model_id;   /* the open model in tab */
    /* Version 2 map: the O entry each replaced token stands for, by (model as written, token). */
    bool v2;
    odin3_strtab *rtab;
    odin3_u64map *rentry; /* model << 32 | token -> index of the entry in the map + 1 */
};

typedef struct rep {
    uint32_t token;
    size_t off, len;
} rep;

static odin3_status prescan(blif_names *names, const char *path);

static uint64_t pair_key(uint32_t model, uint32_t name) {
    return ((uint64_t)model << PAIR_SHIFT) | name;
}

blif_names *blif_names_create(const blif_names_files *files, odin3_status *st) {
    blif_names *names = odin3_util_calloc(sizeof *names);
    odin3_strbuf_init(&names->model);
    odin3_strbuf_init(&names->model_dec);
    odin3_strbuf_init(&names->pool);
    odin3_strbuf_init(&names->legal);
    odin3_vec_init(&names->tokens, sizeof(odin3_bytes));
    odin3_vec_init(&names->reps, sizeof(rep));
    *st = odin3_name_resolver_open(&names->map, "read_blif", files->map_path);
    names->v2 = *st == ODIN3_OK && odin3_name_resolver_v2(&names->map);
    names->move = !files->hdl_locs && !names->v2; /* a version 2 map decodes, as --hdl-locs */
    if (names->v2) {
        names->rtab = odin3_strtab_create();
        names->rentry = odin3_u64map_create(0);
        *st = names->rtab != NULL && names->rentry != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    if (*st == ODIN3_OK && names->move) {
        *st = prescan(names, files->read_path);
    }
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
    odin3_strbuf_free(&names->model_dec);
    odin3_strbuf_free(&names->pool);
    odin3_strbuf_free(&names->legal);
    odin3_vec_free(&names->tokens);
    odin3_vec_free(&names->reps);
    odin3_strtab_destroy(names->tab);
    odin3_u64map_destroy(names->taken);
    odin3_u64map_destroy(names->moved);
    odin3_strtab_destroy(names->rtab);
    odin3_u64map_destroy(names->rentry);
    odin3_util_free(names);
}

void blif_names_begin(blif_names *names, bool count) {
    names->count = count;
    odin3_strbuf_clear(&names->model);
    odin3_strbuf_clear(&names->model_dec);
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

static int hex_val(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    return ch >= 'A' && ch <= 'F' ? ch - 'A' + HEX_TEN : -1;
}

/* The VB-10 form of a model name (a `--vtr` write) decoded: the map's module key is the IR name
 * the writer had. model_dec holds it, or nothing when the name has no `%XX` escape. */
static odin3_status decode_model(blif_names *names) {
    const unsigned char *src = (const unsigned char *)names->model.data;
    size_t len = names->model.len;
    odin3_strbuf_clear(&names->model_dec);
    odin3_status st = ODIN3_OK;
    bool changed = false;
    for (size_t i = 0; st == ODIN3_OK && i < len; i++) {
        int hi = i + 2 < len && src[i] == '%' ? hex_val(src[i + 1]) : -1;
        int lo = hi >= 0 ? hex_val(src[i + 2]) : -1;
        unsigned char byte = lo >= 0 ? (unsigned char)(hi * HEX_BASE + lo) : src[i];
        changed = changed || lo >= 0;
        i += lo >= 0 ? 2U : 0U;
        st = odin3_strbuf_append(&names->model_dec, (odin3_bytes){&byte, 1});
    }
    if (!changed) {
        odin3_strbuf_clear(&names->model_dec);
    }
    return st;
}

/* The entry of the hash name part of the open model: under its name as written, else under the
 * decoded one. NULL for none. */
static const odin3_name_entry *find_entry(const blif_names *names, odin3_bytes part) {
    odin3_bytes model = {names->model.data, names->model.len};
    const odin3_name_entry *entry =
        odin3_name_resolver_entry(&names->map, model, ODIN3_NAME_NET, part);
    if (entry != NULL) {
        return entry;
    }
    model = (odin3_bytes){names->model_dec.data, names->model_dec.len};
    return model.len > 0 ? odin3_name_resolver_entry(&names->map, model, ODIN3_NAME_NET, part)
                         : NULL;
}

/* The full name of the hash name part of the open model. */
static bool find_net(const blif_names *names, odin3_bytes part, odin3_bytes *full) {
    const odin3_name_entry *entry = find_entry(names, part);
    if (entry == NULL) {
        return false;
    }
    *full = entry->full;
    return true;
}

/* Starts model `ln` (a .model line): its name for the lookups and its ID in the prescan table. */
static odin3_status set_model(blif_names *names, const odin3_blif_line *ln) {
    odin3_strbuf_clear(&names->model);
    odin3_status st = ln->count > 1 ? odin3_strbuf_append(&names->model, ln->tokens[1]) : ODIN3_OK;
    st = st == ODIN3_OK ? decode_model(names) : st;
    if (st == ODIN3_OK && names->tab != NULL) {
        st = odin3_strtab_intern(names->tab, (odin3_bytes){names->model.data, names->model.len},
                                 &names->model_id);
    }
    return st;
}

typedef struct pending {
    uint32_t model, hash;
    odin3_bytes full; /* views the map */
} pending;

static odin3_status put_pair(odin3_u64map *map, uint32_t model, uint32_t name, uint64_t value) {
    return odin3_u64map_put(map, (odin3_kv){pair_key(model, name), value});
}

/* One name token of the prescan: a hash name the map holds waits for its final name, any other is
 * spelled as written (a raw net, a port, a hash name the map lacks). */
static odin3_status note_token(blif_names *names, odin3_vec *wait, odin3_bytes part, bool hashed) {
    uint32_t id = 0;
    odin3_bytes full = {NULL, 0};
    odin3_status st = odin3_strtab_intern(names->tab, part, &id);
    if (st != ODIN3_OK) {
        return st;
    }
    if (hashed && find_net(names, part, &full)) {
        pending *slot = odin3_vec_push(wait);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (pending){names->model_id, id, full};
        return ODIN3_OK;
    }
    return put_pair(names->taken, names->model_id, id, 1);
}

static odin3_status prescan_line(blif_names *names, odin3_vec *wait, const odin3_blif_line *ln) {
    if (ln->count == 0) {
        return ODIN3_OK;
    }
    const odin3_bytes dir = ln->tokens[0];
    if (is_word(dir, ".model")) {
        return set_model(names, ln);
    }
    bool port_list = is_word(dir, ".inputs") || is_word(dir, ".outputs") || is_word(dir, ".clock");
    uint32_t first = 0;
    uint32_t last = 0;
    if (!port_list && !name_range(ln, &first, &last)) {
        return ODIN3_OK;
    }
    bool subckt = is_word(dir, ".subckt");
    first = subckt ? 2 : 1;
    last = ln->count;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = first; st == ODIN3_OK && i < last; i++) {
        size_t prefix = 0;
        odin3_bytes part = port_list ? ln->tokens[i] : name_part(ln->tokens[i], subckt, &prefix);
        st = part.ptr != NULL ? note_token(names, wait, part, !port_list) : st;
    }
    return st;
}

/* The final name of `item`: the legal form of its full name, or that with the first free `$N` when
 * a raw token (or an earlier hash name) of its model is spelled so. */
static odin3_status final_name(blif_names *names, odin3_strbuf *cand, const pending *item,
                               uint32_t *id) {
    odin3_strbuf_clear(&names->legal);
    odin3_status st = odin3_blif_vtr_legal_name(item->full, &names->legal);
    for (uint32_t attempt = 0; st == ODIN3_OK; attempt++) {
        odin3_strbuf_clear(cand);
        st = odin3_strbuf_append(cand, (odin3_bytes){names->legal.data, names->legal.len});
        st = st == ODIN3_OK && attempt > 0 ? odin3_strbuf_appendf(cand, "$%u", (unsigned)attempt)
                                           : st;
        st = st == ODIN3_OK
                 ? odin3_strtab_intern(names->tab, (odin3_bytes){cand->data, cand->len}, id)
                 : st;
        if (st != ODIN3_OK || !odin3_u64map_get(names->taken, pair_key(item->model, *id), NULL)) {
            break;
        }
    }
    return st;
}

static odin3_status resolve_waiting(blif_names *names, const odin3_vec *wait) {
    odin3_strbuf cand;
    odin3_strbuf_init(&cand);
    odin3_status st = ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < wait->len; k++) {
        const pending *item = odin3_vec_cat(wait, k);
        if (odin3_u64map_get(names->moved, pair_key(item->model, item->hash), NULL)) {
            continue;
        }
        uint32_t id = 0;
        st = final_name(names, &cand, item, &id);
        st = st == ODIN3_OK ? put_pair(names->taken, item->model, id, 1) : st;
        st = st == ODIN3_OK ? put_pair(names->moved, item->model, item->hash, id) : st;
    }
    odin3_strbuf_free(&cand);
    return st;
}

static odin3_status prescan(blif_names *names, const char *path) {
    names->tab = odin3_strtab_create();
    names->taken = odin3_u64map_create(0);
    names->moved = odin3_u64map_create(0);
    if (names->tab == NULL || names->taken == NULL || names->moved == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_blif_lexer *lx = odin3_blif_lexer_open(path);
    if (lx == NULL) {
        return ODIN3_ERR_IO;
    }
    odin3_vec wait;
    odin3_vec_init(&wait, sizeof(pending));
    odin3_blif_line ln;
    odin3_status st = odin3_strtab_intern(names->tab, (odin3_bytes){"", 0}, &names->model_id);
    while (st == ODIN3_OK && odin3_blif_lexer_next(lx, &ln)) {
        st = prescan_line(names, &wait, &ln);
    }
    odin3_blif_lexer_close(lx); /* a lexer error is the read's to report */
    st = st == ODIN3_OK ? resolve_waiting(names, &wait) : st;
    odin3_vec_free(&wait);
    odin3_strbuf_clear(&names->model);
    return st;
}

/* The final text the prescan fixed for the hash name part in the open model, NULL ptr for none. */
static odin3_bytes moved_text(const blif_names *names, odin3_bytes part) {
    uint32_t hash = 0;
    uint64_t id = 0;
    if (names->tab == NULL || !odin3_strtab_find(names->tab, part, &hash) ||
        !odin3_u64map_get(names->moved, pair_key(names->model_id, hash), &id)) {
        return (odin3_bytes){NULL, 0};
    }
    return (odin3_bytes){odin3_strtab_get(names->tab, (uint32_t)id),
                         odin3_strtab_len(names->tab, (uint32_t)id)};
}

/* Version 2: remembers that the token `final` of the open model stands for entry. */
static odin3_status note_entry(blif_names *names, odin3_bytes final,
                               const odin3_name_entry *entry) {
    uint32_t model = 0;
    uint32_t text = 0;
    const odin3_name_entry *base = names->map.map.entries.data;
    odin3_status st = odin3_strtab_intern(
        names->rtab, (odin3_bytes){names->model.data, names->model.len}, &model);
    st = st == ODIN3_OK ? odin3_strtab_intern(names->rtab, final, &text) : st;
    return st == ODIN3_OK ? put_pair(names->rentry, model, text, (uint64_t)(entry - base) + 1U)
                          : st;
}

/* Replaces token i when the map has its name: appends the new token (NUL included) to the pool. */
static odin3_status replace_one(blif_names *names, const odin3_blif_line *ln, uint32_t index,
                                bool subckt) {
    size_t prefix = 0;
    odin3_bytes part = name_part(ln->tokens[index], subckt, &prefix);
    if (part.ptr == NULL || !odin3_name_hash_shaped(part)) {
        return ODIN3_OK;
    }
    const odin3_name_entry *entry = find_entry(names, part);
    if (entry == NULL) {
        if (names->count) {
            odin3_name_miss_note(&names->miss, part, ln->line);
        }
        return ODIN3_OK;
    }
    odin3_bytes full = entry->full;
    odin3_bytes final = moved_text(names, part);
    rep *slot = odin3_vec_push(&names->reps);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->token = index;
    slot->off = names->pool.len;
    odin3_strbuf_clear(&names->legal);
    odin3_status st =
        odin3_strbuf_append(&names->pool, (odin3_bytes){ln->tokens[index].ptr, prefix});
    if (final.ptr == NULL) {
        st = st == ODIN3_OK ? odin3_blif_vtr_legal_name(full, &names->legal) : st;
        final = (odin3_bytes){names->legal.data, names->legal.len};
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(&names->pool, final) : st;
    st = st == ODIN3_OK && names->v2 ? note_entry(names, final, entry) : st;
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
        return set_model(names, ln);
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

bool blif_names_v2(const blif_names *names) {
    return names != NULL && names->v2;
}

odin3_status blif_names_restore(blif_names *names, odin3_design *design, const char *path) {
    return odin3_name_resolver_restore(&names->map, design, path);
}

uint32_t blif_names_record(blif_names *names, odin3_bytes model, odin3_bytes name) {
    uint32_t model_id = 0;
    uint32_t text_id = 0;
    uint64_t index = 0;
    odin3_strbuf_clear(&names->model);
    odin3_strbuf_clear(&names->model_dec);
    if (odin3_strbuf_append(&names->model, model) != ODIN3_OK || decode_model(names) != ODIN3_OK) {
        return 0;
    }
    if (odin3_strtab_find(names->rtab, model, &model_id) &&
        odin3_strtab_find(names->rtab, name, &text_id) &&
        odin3_u64map_get(names->rentry, pair_key(model_id, text_id), &index) && index > 0) {
        const odin3_name_entry *base = names->map.map.entries.data;
        return odin3_name_resolver_record(&names->map, base + (index - 1U));
    }
    uint32_t record = odin3_name_resolver_named(&names->map, model, ODIN3_NAME_NET, name);
    odin3_bytes decoded = {names->model_dec.data, names->model_dec.len};
    return record != 0 || decoded.len == 0
               ? record
               : odin3_name_resolver_named(&names->map, decoded, ODIN3_NAME_NET, name);
}
