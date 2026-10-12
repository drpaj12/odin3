/*
 * ppool.h — the pools of still-reachable values a case analysis keeps (2E spec §5.2, PR-8): the
 * bit-pattern pool (Yosys 0.55 kernel/bitpattern.h, BitPatternPool), used by rmdead and the mux
 * grouping (Task 7), and proc_rmdead's FullyDefinedPool. Patterns and values are read from a
 * working form's signals (pw.h).
 */
#ifndef ODIN3_PASSES_PROC_PPOOL_H
#define ODIN3_PASSES_PROC_PPOOL_H

#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/hash.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A deduplicated set of patterns over {0, 1, -}, each width positions, two bits per position
 * (00 = 0, 01 = 1, 11 = '-') packed LSB first, 32 positions to a uint64_t word; words holds
 * words_per_pattern words per pattern (padding positions 00). bits_used is 2 * width * the number
 * of patterns held. over_cap: a take needed more than the pattern-bit cap
 * (odin3_proc_limits_now().max_pattern_bits, ODIN3_PROC_MAX_PATTERN_BITS), or the pool was
 * stopped, so it no longer says which values are left: its patterns are dropped (bits_used 0),
 * every take is a no-op that reports the pattern taken, has_all is false and the pool is never
 * empty. Read the fields; change them only through these calls.
 */
typedef struct odin3_ppool {
    odin3_vec words;
    uint32_t width, words_per_pattern;
    uint64_t bits_used;
    bool over_cap;
} odin3_ppool;

/*
 * BitPatternPool(width): one all-'-' pattern (none for width 0, an empty pool). *pool is
 * overwritten (not freed first) and owned by the caller (odin3_ppool_free), also on failure, when
 * it is left empty. A pattern past the cap sets over_cap and stores nothing. ODIN3_ERR_NO_MEMORY;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL pool.
 */
odin3_status odin3_ppool_init(odin3_ppool *pool, uint32_t width);

/*
 * BitPatternPool(signal): one pattern of signal's width whose positions are 0 or 1 where signal's
 * bit is ODIN3_PW_S0 or ODIN3_PW_S1 and '-' everywhere else (nets, x, z). As odin3_ppool_init
 * otherwise (pw may be NULL for an empty signal).
 */
odin3_status odin3_ppool_init_sig(odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig signal);

/*
 * BitPatternPool::take(pattern): pattern (fully constant; x, z, '-' and any net bit read as '-')
 * removes every pattern it matches (no position where both are 0 or 1 and differ), and for each
 * removed pattern p and each position where p has '-' and pattern has 0 or 1, the pattern p with
 * that position set to the opposite value is inserted (each once). *taken: some pattern matched;
 * false on an empty pool. When the pool's patterns plus the inserted ones (before deduplication:
 * Yosys inserts them before it erases the matched pattern) would hold more than the cap's bits,
 * the pool is stopped instead (odin3_ppool_stop) and *taken is true; once over_cap a take is a
 * no-op with *taken true. ODIN3_ERR_NO_MEMORY with the pool unchanged; ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument or, on a non-empty pool, a pattern whose length is not the pool's
 * width.
 */
odin3_status odin3_ppool_take(odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern,
                              bool *taken);

/*
 * BitPatternPool::has_all(pattern): some pattern matches pattern and has '-' wherever pattern has
 * '-' (x, z, '-' or a net), so the pool holds every value pattern covers. False once over_cap and
 * for a pattern of another width. Never allocates.
 */
bool odin3_ppool_has_all(const odin3_ppool *pool, const odin3_pw *pw, odin3_pw_sig pattern);

/* BitPatternPool::take_all: drops every pattern (a no-op once over_cap). */
void odin3_ppool_take_all(odin3_ppool *pool);

/* No pattern is left (never once over_cap). */
bool odin3_ppool_empty(const odin3_ppool *pool);

/* Gives the pool up as past the cap: its patterns are freed and over_cap is set (a caller that
 * bounds several live pools together, rmdead's walk, stops the one that grew). NULL: no-op. */
void odin3_ppool_stop(odin3_ppool *pool);

/* Frees the patterns and empties the pool (over_cap cleared); NULL is a no-op. */
void odin3_ppool_free(odin3_ppool *pool);

/*
 * proc_rmdead's FullyDefinedPool: the values taken so far, each width bits (0 and 1 only), and
 * whether the default was reached. A value is stored once in values as one record, a chain word
 * (1 + the index of the next record whose value has the same hash, 0 at the end) followed by
 * ceil(width / 64) words of its bits, LSB first; seen maps the value's odin3_hash_bytes to 1 + the
 * index of its chain's first record, and is made on the first take. taken counts the values.
 */
typedef struct odin3_fdpool {
    odin3_u64map *seen;
    odin3_vec values;
    uint32_t width;
    uint64_t taken;
    bool default_reached;
} odin3_fdpool;

/* FullyDefinedPool(width): no value taken. Allocates nothing; ODIN3_ERR_INVALID_ARG (logged) for a
 * NULL pool. */
odin3_status odin3_fdpool_init(odin3_fdpool *pool, uint32_t width);

/*
 * FullyDefinedPool::take(value): *taken false after the default or for a value taken before,
 * else the value is stored and *taken true. ODIN3_ERR_NO_MEMORY with the pool unchanged;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a value that is not width bits of
 * ODIN3_PW_S0 and ODIN3_PW_S1.
 */
odin3_status odin3_fdpool_take(odin3_fdpool *pool, const odin3_pw *pw, odin3_pw_sig value,
                               bool *taken);

/* The default was reached, or width < 32 and all 2^width values were taken. */
bool odin3_fdpool_empty(const odin3_fdpool *pool);

/* FullyDefinedPool::take_all: the default was reached. */
void odin3_fdpool_take_all(odin3_fdpool *pool);

/* Frees the values and the map and empties the pool; NULL is a no-op. */
void odin3_fdpool_free(odin3_fdpool *pool);

/* Test hook: the hash of a value's words (NULL restores odin3_hash_bytes with ODIN3_HASH_SEED), so
 * a test can force collisions onto one chain. */
void odin3_fdpool_test_set_hash(uint64_t (*fn)(odin3_bytes words));

#endif
