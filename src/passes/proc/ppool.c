/*
 * ppool.c — the pools of still-reachable values (2E spec §5.2, PR-8): Yosys 0.55's BitPatternPool
 * (kernel/bitpattern.h) with two bits per position and the pattern-bit cap (§16, §20 #7), and
 * proc_rmdead's FullyDefinedPool with each value stored once.
 */
#include "passes/proc/ppool.h"

#include "odin3/odin3.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/hash.h"
#include "util/idindex.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    PAIRS_PER_WORD = 32,   /* pattern positions in a uint64_t word */
    BITS_PER_PAIR = 2,     /* bits of one pattern position */
    BITS_PER_WORD = 64,    /* value positions in a uint64_t word */
    FD_COUNTED_WIDTH = 32, /* FullyDefinedPool counts its values only below this width */
};

#define PAIR_ONE UINT64_C(1)  /* 01: the position is 1 */
#define PAIR_DASH UINT64_C(3) /* 11: the position is '-'; 00 is 0 */
#define LO_MASK UINT64_C(0x5555555555555555)
#define HI_MASK UINT64_C(0xAAAAAAAAAAAAAAAA)

/* --- BitPatternPool ------------------------------------------------------------------------ */

/* The code of a pattern bit: 0 and 1 are themselves, anything else (x, z, '-', a net) is '-'. */
static uint64_t pair_of(odin3_pw_bit bit) {
    if (bit == ODIN3_PW_S0) {
        return 0;
    }
    return bit == ODIN3_PW_S1 ? PAIR_ONE : PAIR_DASH;
}

/* Word wd of the pool-wide pattern of bits (NULL: all '-'); padding positions are 00. */
static uint64_t pattern_word(const odin3_ppool *pool, const odin3_pw_bit *bits, uint32_t wd) {
    uint32_t first = wd * PAIRS_PER_WORD;
    uint32_t count = pool->width - first < PAIRS_PER_WORD ? pool->width - first : PAIRS_PER_WORD;
    uint64_t word = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t pair = bits != NULL ? pair_of(bits[first + i]) : PAIR_DASH;
        word |= pair << (BITS_PER_PAIR * i);
    }
    return word;
}

/* No position where both words are 0 or 1 and differ (BitPatternPool::match). */
static bool word_match(uint64_t pat, uint64_t probe) {
    uint64_t dash = (pat | probe) & HI_MASK;
    uint64_t both_def = ~(dash >> 1) & LO_MASK;
    return ((pat ^ probe) & both_def) == 0;
}

/* The positions (their high bits) where pat has '-' and probe has 0 or 1: one child each. */
static uint64_t child_mask(uint64_t pat, uint64_t probe) {
    return pat & ~probe & HI_MASK;
}

static size_t n_patterns(const odin3_ppool *pool) {
    return pool->words_per_pattern == 0 ? 0 : pool->words.len / pool->words_per_pattern;
}

static const uint64_t *pattern_at(const odin3_ppool *pool, size_t index) {
    return (const uint64_t *)pool->words.data + index * pool->words_per_pattern;
}

/* True when holding `patterns` patterns needs more bits than the cap allows. */
static bool past_cap(const odin3_ppool *pool, uint64_t patterns) {
    return patterns * BITS_PER_PAIR * pool->width > odin3_proc_limits_now().max_pattern_bits;
}

/* An empty pool of width positions. */
static void ppool_start(odin3_ppool *pool, uint32_t width) {
    memset(pool, 0, sizeof *pool);
    odin3_vec_init(&pool->words, sizeof(uint64_t));
    pool->width = width;
    pool->words_per_pattern = width / PAIRS_PER_WORD + (width % PAIRS_PER_WORD != 0 ? 1U : 0U);
}

/* The first pattern, from bits (NULL: all '-'); none for width 0, over_cap past the cap. */
static odin3_status ppool_seed(odin3_ppool *pool, const odin3_pw_bit *bits) {
    if (pool->width == 0) {
        return ODIN3_OK;
    }
    if (past_cap(pool, 1)) {
        pool->over_cap = true;
        return ODIN3_OK;
    }
    if (odin3_vec_reserve(&pool->words, pool->words_per_pattern) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t wd = 0; wd < pool->words_per_pattern; wd++) {
        *(uint64_t *)odin3_vec_push(&pool->words) = pattern_word(pool, bits, wd);
    }
    pool->bits_used = (uint64_t)BITS_PER_PAIR * pool->width;
    return ODIN3_OK;
}

odin3_status odin3_ppool_init(odin3_ppool *pool, uint32_t width) {
    if (pool == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: no pattern pool");
        return ODIN3_ERR_INVALID_ARG;
    }
    ppool_start(pool, width);
    odin3_status st = ppool_seed(pool, NULL);
    if (st != ODIN3_OK) {
        odin3_ppool_free(pool);
    }
    return st;
}

odin3_status odin3_ppool_init_sig(odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig signal) {
    if (pool == NULL || (pw == NULL && signal.len > 0)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: no pattern pool or no working form");
        return ODIN3_ERR_INVALID_ARG;
    }
    ppool_start(pool, signal.len);
    odin3_status st = signal.len > 0 ? ppool_seed(pool, odin3_pw_sig_bits(pw, signal)) : ODIN3_OK;
    if (st != ODIN3_OK) {
        odin3_ppool_free(pool);
    }
    return st;
}

/* The probe's words for pattern (in *probe, wpp of them). ODIN3_ERR_NO_MEMORY. */
static odin3_status probe_words(const odin3_ppool *pool, const odin3_pw_bit *bits,
                                odin3_vec *probe) {
    if (odin3_vec_reserve(probe, pool->words_per_pattern) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t wd = 0; wd < pool->words_per_pattern; wd++) {
        *(uint64_t *)odin3_vec_push(probe) = pattern_word(pool, bits, wd);
    }
    return ODIN3_OK;
}

static bool pattern_matches(const odin3_ppool *pool, const uint64_t *pat, const uint64_t *probe) {
    for (uint32_t wd = 0; wd < pool->words_per_pattern; wd++) {
        if (!word_match(pat[wd], probe[wd])) {
            return false;
        }
    }
    return true;
}

/* What a take does: the patterns it removes and the children it would insert (before dedupe). */
typedef struct take_count {
    size_t matched, children;
} take_count;

static take_count count_take(const odin3_ppool *pool, const uint64_t *probe) {
    take_count count = {0, 0};
    size_t held = n_patterns(pool);
    for (size_t pi = 0; pi < held; pi++) {
        const uint64_t *pat = pattern_at(pool, pi);
        if (!pattern_matches(pool, pat, probe)) {
            continue;
        }
        count.matched++;
        for (uint32_t wd = 0; wd < pool->words_per_pattern; wd++) {
            count.children += (size_t)__builtin_popcountll(child_mask(pat[wd], probe[wd]));
        }
    }
    return count;
}

/* Removes the matched patterns in place, keeping the others' order (a take without children). */
static void drop_matched(odin3_ppool *pool, const uint64_t *probe) {
    size_t held = n_patterns(pool);
    size_t kept = 0;
    size_t wpp = pool->words_per_pattern;
    uint64_t *words = pool->words.data;
    for (size_t pi = 0; pi < held; pi++) {
        if (!pattern_matches(pool, words + pi * wpp, probe)) {
            memmove(words + kept * wpp, words + pi * wpp, wpp * sizeof *words);
            kept++;
        }
    }
    for (size_t i = kept * wpp; i < held * wpp; i++) {
        odin3_vec_pop(&pool->words);
    }
}

/* The set a take builds: the new words, deduplicated through an index of their patterns. */
typedef struct pset {
    odin3_vec words; /* uint64_t, wpp per pattern */
    odin3_idindex *index;
    uint32_t wpp;
} pset;

static bool pset_equals(const void *ctx, uint32_t id, odin3_bytes probe) {
    const pset *set = ctx;
    const uint64_t *pat = (const uint64_t *)set->words.data + (size_t)id * set->wpp;
    return memcmp(pat, probe.ptr, probe.len) == 0;
}

/* Adds pattern pat (wpp words) unless the set holds it. ODIN3_ERR_NO_MEMORY, set unchanged. */
static odin3_status pset_add(pset *set, const uint64_t *pat) {
    odin3_bytes bytes = {pat, (size_t)set->wpp * sizeof *pat};
    uint64_t hash = odin3_hash_bytes(bytes, ODIN3_HASH_SEED);
    odin3_idcmp cmp = {hash, bytes, pset_equals, set};
    if (odin3_idindex_find(set->index, &cmp, NULL)) {
        return ODIN3_OK;
    }
    uint32_t id = (uint32_t)(set->words.len / set->wpp);
    if (odin3_idindex_insert(set->index, (odin3_identry){hash, id}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t wd = 0; wd < set->wpp; wd++) {
        *(uint64_t *)odin3_vec_push(&set->words) = pat[wd]; /* reserved */
    }
    return ODIN3_OK;
}

/* Adds each child of matched pattern pat (child holds wpp words of scratch). */
static odin3_status pset_add_children(pset *set, const uint64_t *pat, const uint64_t *probe,
                                      uint64_t *child) {
    odin3_status st = ODIN3_OK;
    for (uint32_t wd = 0; st == ODIN3_OK && wd < set->wpp; wd++) {
        for (uint64_t mask = child_mask(pat[wd], probe[wd]); st == ODIN3_OK && mask != 0;
             mask &= mask - 1) {
            unsigned lo = (unsigned)__builtin_ctzll(mask) - 1; /* the pair's low bit */
            memcpy(child, pat, (size_t)set->wpp * sizeof *pat);
            child[wd] &= ~(PAIR_DASH << lo);
            child[wd] |= ((probe[wd] >> lo) & PAIR_ONE) != 0 ? 0 : PAIR_ONE << lo;
            st = pset_add(set, child);
        }
    }
    return st;
}

/* Fills set with the kept patterns, then the children of the matched ones. */
static odin3_status pset_fill(pset *set, const odin3_ppool *pool, const uint64_t *probe,
                              uint64_t *child) {
    size_t held = n_patterns(pool);
    odin3_status st = ODIN3_OK;
    for (size_t pi = 0; st == ODIN3_OK && pi < held; pi++) {
        const uint64_t *pat = pattern_at(pool, pi);
        if (!pattern_matches(pool, pat, probe)) {
            st = pset_add(set, pat);
        }
    }
    for (size_t pi = 0; st == ODIN3_OK && pi < held; pi++) {
        const uint64_t *pat = pattern_at(pool, pi);
        if (pattern_matches(pool, pat, probe)) {
            st = pset_add_children(set, pat, probe, child);
        }
    }
    return st;
}

/* The take with children: the new set replaces the pool's patterns. NO_MEMORY, pool unchanged. */
static odin3_status take_rebuild(odin3_ppool *pool, const uint64_t *probe, size_t most) {
    pset set;
    memset(&set, 0, sizeof set);
    odin3_vec_init(&set.words, sizeof(uint64_t));
    set.wpp = pool->words_per_pattern;
    odin3_vec child;
    odin3_vec_init(&child, sizeof(uint64_t));
    odin3_status st = odin3_vec_reserve(&set.words, most * pool->words_per_pattern);
    st = st == ODIN3_OK ? odin3_vec_reserve(&child, pool->words_per_pattern) : st;
    if (st == ODIN3_OK) {
        set.index = odin3_idindex_create(most);
        st = set.index != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    st = st == ODIN3_OK ? pset_fill(&set, pool, probe, child.data) : st;
    if (st == ODIN3_OK) {
        odin3_vec_free(&pool->words);
        pool->words = set.words;
        pool->bits_used = (uint64_t)n_patterns(pool) * BITS_PER_PAIR * pool->width;
    } else {
        odin3_vec_free(&set.words);
    }
    odin3_idindex_destroy(set.index);
    odin3_vec_free(&child);
    return st;
}

/* Checks a take's arguments; ODIN3_ERR_INVALID_ARG (logged). */
static odin3_status take_args(const odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern,
                              const bool *taken) {
    if (pool == NULL || taken == NULL || (pw == NULL && pattern.len > 0)) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: pattern pool take without a pool or result");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!pool->over_cap && n_patterns(pool) > 0 && pattern.len != pool->width) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: a %u-bit pattern taken from a %u-bit pool",
                  pattern.len, pool->width);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The take on a non-empty pool within its cap: probe built, counted, then dropped or rebuilt. */
static odin3_status take_patterns(odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern,
                                  bool *taken) {
    odin3_vec probe;
    odin3_vec_init(&probe, sizeof(uint64_t));
    odin3_status st = probe_words(pool, odin3_pw_sig_bits(pw, pattern), &probe);
    take_count count = {0, 0};
    if (st == ODIN3_OK) {
        count = count_take(pool, probe.data);
        *taken = count.matched > 0;
    }
    size_t held = n_patterns(pool);
    if (st != ODIN3_OK || count.matched == 0) {
        /* nothing matched: the pool is unchanged */
    } else if (past_cap(pool, (uint64_t)held + count.children)) {
        pool->over_cap = true;
    } else if (count.children == 0) {
        drop_matched(pool, probe.data);
        pool->bits_used = (uint64_t)n_patterns(pool) * BITS_PER_PAIR * pool->width;
    } else {
        st = take_rebuild(pool, probe.data, held - count.matched + count.children);
    }
    odin3_vec_free(&probe);
    return st;
}

odin3_status odin3_ppool_take(odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern,
                              bool *taken) {
    odin3_status st = take_args(pool, pw, pattern, taken);
    if (st != ODIN3_OK) {
        return st;
    }
    if (pool->over_cap) {
        *taken = true;
        return ODIN3_OK;
    }
    *taken = false;
    if (n_patterns(pool) == 0) {
        return ODIN3_OK; /* an empty pool takes nothing (Yosys: no pattern to match) */
    }
    return take_patterns(pool, pw, pattern, taken);
}

/* pat matches the pattern of bits and has '-' wherever it has '-'. */
static bool pattern_holds_all(const odin3_ppool *pool, const uint64_t *pat,
                              const odin3_pw_bit *bits) {
    for (uint32_t wd = 0; wd < pool->words_per_pattern; wd++) {
        uint64_t probe = pattern_word(pool, bits, wd);
        if (!word_match(pat[wd], probe) || (probe & ~pat[wd] & HI_MASK) != 0) {
            return false;
        }
    }
    return true;
}

bool odin3_ppool_has_all(const odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern) {
    if (pool == NULL || pool->over_cap || pattern.len != pool->width || pw == NULL) {
        return false;
    }
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, pattern);
    size_t held = n_patterns(pool);
    for (size_t pi = 0; pi < held; pi++) {
        if (pattern_holds_all(pool, pattern_at(pool, pi), bits)) {
            return true;
        }
    }
    return false;
}

void odin3_ppool_take_all(odin3_ppool *pool) {
    if (pool != NULL && !pool->over_cap) {
        odin3_vec_clear(&pool->words);
        pool->bits_used = 0;
    }
}

bool odin3_ppool_empty(const odin3_ppool *pool) {
    return pool != NULL && !pool->over_cap && pool->words.len == 0;
}

void odin3_ppool_free(odin3_ppool *pool) {
    if (pool == NULL) {
        return;
    }
    odin3_vec_free(&pool->words);
    pool->bits_used = 0;
    pool->over_cap = false;
}

/* --- FullyDefinedPool ---------------------------------------------------------------------- */

static uint64_t (*g_fd_hash)(odin3_bytes words);

void odin3_fdpool_test_set_hash(uint64_t (*fn)(odin3_bytes words)) {
    g_fd_hash = fn;
}

static uint64_t fd_hash(odin3_bytes words) {
    return g_fd_hash != NULL ? g_fd_hash(words) : odin3_hash_bytes(words, ODIN3_HASH_SEED);
}

/* The words of one value. */
static size_t value_words(uint32_t width) {
    return (size_t)width / BITS_PER_WORD + (width % BITS_PER_WORD != 0 ? 1U : 0U);
}

odin3_status odin3_fdpool_init(odin3_fdpool *pool, uint32_t width) {
    if (pool == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: no fully defined pool");
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(pool, 0, sizeof *pool);
    odin3_vec_init(&pool->values, sizeof(uint64_t));
    pool->width = width;
    return ODIN3_OK;
}

/* Checks a take's arguments; ODIN3_ERR_INVALID_ARG (logged). */
static odin3_status fd_args(const odin3_fdpool *pool, const odin3_pw *pw, odin3_pw_sig value,
                            const bool *taken) {
    if (pool == NULL || pw == NULL || taken == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: fully defined pool take without its arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (value.len != pool->width || !odin3_pw_sig_is_fully_def(pw, value)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "proc: internal: a %u-bit pool takes only %u bits of 0 and 1 (got %u bits)",
                  pool->width, pool->width, value.len);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The record of index rec (its chain word, then its value). */
static const uint64_t *fd_record(const odin3_fdpool *pool, uint64_t rec) {
    return (const uint64_t *)pool->values.data + rec * (1 + value_words(pool->width));
}

/* Appends a record of value (chain word 0); reserved. */
static void fd_push(odin3_fdpool *pool, const odin3_pw *pw, odin3_pw_sig value) {
    size_t at = pool->values.len;
    size_t words = 1 + value_words(pool->width);
    for (size_t i = 0; i < words; i++) {
        (void)odin3_vec_push(&pool->values);
    }
    uint64_t *rec = (uint64_t *)pool->values.data + at;
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, value);
    for (uint32_t i = 0; i < value.len; i++) {
        rec[1 + i / BITS_PER_WORD] |= (bits[i] == ODIN3_PW_S1 ? UINT64_C(1) : 0)
                                      << (i % BITS_PER_WORD);
    }
}

/* Drops the last record. */
static void fd_pop(odin3_fdpool *pool) {
    for (size_t i = 0; i < 1 + value_words(pool->width); i++) {
        odin3_vec_pop(&pool->values);
    }
}

/* True when the chain from head (1 + a record index, 0 = none) holds the last record's value. */
static bool fd_chain_has(const odin3_fdpool *pool, uint64_t head) {
    size_t vw = value_words(pool->width);
    const uint64_t *last = (const uint64_t *)pool->values.data + pool->values.len - vw;
    for (uint64_t at = head; at != 0; at = fd_record(pool, at - 1)[0]) {
        if (memcmp(fd_record(pool, at - 1) + 1, last, vw * sizeof *last) == 0) {
            return true;
        }
    }
    return false;
}

odin3_status odin3_fdpool_take(odin3_fdpool *pool, const odin3_pw *pw, odin3_pw_sig value,
                               bool *taken) {
    odin3_status st = fd_args(pool, pw, value, taken);
    if (st != ODIN3_OK) {
        return st;
    }
    *taken = false;
    if (pool->default_reached) {
        return ODIN3_OK;
    }
    size_t words = 1 + value_words(pool->width);
    if (odin3_vec_reserve(&pool->values, pool->values.len + words) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (pool->seen == NULL) {
        pool->seen = odin3_u64map_create(0);
        if (pool->seen == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    fd_push(pool, pw, value);
    uint64_t rec = pool->values.len / words - 1;
    const uint64_t *last = fd_record(pool, rec);
    uint64_t hash = fd_hash((odin3_bytes){last + 1, (words - 1) * sizeof *last});
    uint64_t head = 0;
    (void)odin3_u64map_get(pool->seen, hash, &head);
    if (fd_chain_has(pool, head)) {
        fd_pop(pool);
        return ODIN3_OK;
    }
    if (odin3_u64map_put(pool->seen, (odin3_kv){hash, rec + 1}) != ODIN3_OK) {
        fd_pop(pool);
        return ODIN3_ERR_NO_MEMORY;
    }
    ((uint64_t *)pool->values.data)[rec * words] = head;
    pool->taken++;
    *taken = true;
    return ODIN3_OK;
}

bool odin3_fdpool_empty(const odin3_fdpool *pool) {
    if (pool == NULL) {
        return false;
    }
    return pool->default_reached ||
           (pool->width < FD_COUNTED_WIDTH && pool->taken == UINT64_C(1) << pool->width);
}

void odin3_fdpool_take_all(odin3_fdpool *pool) {
    if (pool != NULL) {
        pool->default_reached = true;
    }
}

void odin3_fdpool_free(odin3_fdpool *pool) {
    if (pool == NULL) {
        return;
    }
    odin3_u64map_destroy(pool->seen);
    pool->seen = NULL;
    odin3_vec_free(&pool->values);
    pool->taken = 0;
    pool->default_reached = false;
}
