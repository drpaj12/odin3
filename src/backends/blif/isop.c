/*
 * isop.c — the Minato-Morreale irredundant sum-of-products (isop.h; spec 3E §4.1, plan 3E point
 * (t)).
 *
 * isop(L, U, n), with L within U over the first n inputs, emits a cover whose function R has L
 * within R within U. It splits on input n - 1 into three calls on n - 1 inputs: the `0` branch,
 * the `1` branch, then the shared `-` branch, which needs the first two's R.
 *
 * That recursion is on the truth table and at most k <= 16 deep; it is not an IR traversal, so
 * DESIGN §15.1's worklist rule for the IR does not apply to it. It runs on an explicit stack all
 * the same, as the plan asks (and the lint gate's misc-no-recursion requires of all of src/):
 * frame n (one per level; frame k is the bottom of the stack) holds the call on n inputs, and
 * the call it makes is always frame n - 1's, so the active frames are the levels top .. k. The
 * tables of every level come from one arena block.
 *
 * Two shortcuts give the rows the full recursion would: L = 0 emits nothing (every branch of it
 * has L = 0), and U = 1 (with L not 0) emits one row with `-` for its n inputs (its 0 and 1
 * branches have L = 0, and its shared branch is the same call on n - 1 inputs, down to n = 0).
 */
#include "backends/blif/isop.h"

#include "backends/blif/writer.h"
#include "ir/lut.h"
#include "util/arena.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

_Static_assert(ODIN3_BLIF_ISOP_MAX_K == ODIN3_BLIF_VTR_MAX_K, "the VTR dialect's widest .names");
_Static_assert(ODIN3_BLIF_ISOP_MAX_K == (uint32_t)ODIN3_LUT_MAX_WIDTH, "the widest $lut");

enum {
    WORD_SHIFT = 6,                      /* a table word holds 2^6 bits */
    ROW_MAX = ODIN3_BLIF_ISOP_MAX_K + 3, /* the plane, a blank, the output, a newline */
    ARENA_SLACK = 64,                    /* above the arena's rounding of the one request */
    TABLES_PER_LEVEL = 3,                /* L, U and R of a level's own size */
    BRANCH_TABLES = 2,                   /* R0 and R1, of the level below's size */
};

/* What a frame does next: start its 0, 1 or shared branch, or join their results. */
enum { STEP_ZERO, STEP_ONE, STEP_SHARED, STEP_JOIN };

/* The call isop(lo, up, n) of level n. */
typedef struct isop_frame {
    uint64_t *lo, *up; /* L and U: words each, set by the caller (frame n + 1) */
    uint64_t *res;     /* R, the function of the rows the call emitted: words */
    uint64_t *r0, *r1; /* the R of its 0 and 1 branches: frame n - 1's words (NULL for n = 0) */
    uint32_t words;    /* odin3_blif_isop_words(n) */
    uint64_t mask;     /* the valid bits of each word (level_mask(n)) */
    uint32_t step;     /* STEP_* */
} isop_frame;

/* One ISOP run over one table: the frames, and the rows of the set being covered. */
typedef struct isop_run {
    uint32_t k;
    char out;            /* the rows' output character: '1' on-set, '0' off-set */
    uint32_t limit;      /* stop instead of emitting row number limit + 1 */
    uint32_t rows;       /* rows emitted so far */
    bool stopped;        /* the limit was reached, or an append failed */
    odin3_strbuf *cover; /* the rows' text; NULL: count them only */
    odin3_status st;
    char lits[ODIN3_BLIF_ISOP_MAX_K];             /* input j's literal, fixed by frame j + 1 */
    isop_frame frames[ODIN3_BLIF_ISOP_MAX_K + 1]; /* by level */
} isop_run;

uint32_t odin3_blif_isop_words(uint32_t inputs) {
    uint32_t capped = inputs > ODIN3_BLIF_ISOP_MAX_K ? ODIN3_BLIF_ISOP_MAX_K : inputs;
    return capped <= WORD_SHIFT ? 1U : 1U << (capped - WORD_SHIFT);
}

/* The valid bits of each word of a table of n inputs. */
static uint64_t level_mask(uint32_t n) {
    return n >= WORD_SHIFT ? UINT64_MAX : ((uint64_t)1 << (1U << n)) - 1U;
}

/* True when each of frame's words of table (its lo or up) is value. */
static bool every_word(const isop_frame *frame, const uint64_t *table, uint64_t value) {
    for (uint32_t i = 0; i < frame->words; i++) {
        if (table[i] != value) {
            return false;
        }
    }
    return true;
}

/* Sets each word of frame's res to value. */
static void fill_res(isop_frame *frame, uint64_t value) {
    for (uint32_t i = 0; i < frame->words; i++) {
        frame->res[i] = value;
    }
}

/* Emits the row of a call on n inputs whose U is 1: `-` for inputs 0 .. n - 1, the literals the
 * frames above fixed for the others. */
static void emit(isop_run *run, uint32_t n) {
    if (run->rows == run->limit) {
        run->stopped = true;
        return;
    }
    run->rows++;
    if (run->cover == NULL) {
        return;
    }
    char row[ROW_MAX];
    uint32_t len = run->k;
    memset(row, '-', n);
    memcpy(row + n, run->lits + n, run->k - n);
    if (run->k > 0) {
        row[len++] = ' ';
    }
    row[len++] = run->out;
    row[len++] = '\n';
    run->st = odin3_strbuf_append(run->cover, (odin3_bytes){row, len});
    run->stopped = run->st != ODIN3_OK;
}

/* Starts the call of frame n, whose lo and up its caller set. True when it needs its branches (it
 * is now the top of the stack); false when it ended at once, its res set: L = 0 emits no row,
 * U = 1 one row. */
static bool start(isop_run *run, uint32_t n) {
    isop_frame *frame = &run->frames[n];
    bool one_word = frame->words == 1; /* most calls: their tests inline */
    if (one_word ? frame->lo[0] == 0 : every_word(frame, frame->lo, 0)) {
        fill_res(frame, 0);
        return false;
    }
    if (one_word ? frame->up[0] == frame->mask : every_word(frame, frame->up, frame->mask)) {
        fill_res(frame, frame->mask);
        emit(run, n);
        return false;
    }
    frame->step = STEP_ZERO;
    return true;
}

/* branch for a frame of more than one word: its halves are its first and last child->words
 * words. */
static void branch_words(isop_frame *frame, isop_frame *child) {
    uint32_t half = child->words;
    const uint64_t *lo = frame->lo;
    const uint64_t *up = frame->up;
    if (frame->step == STEP_ZERO) {
        for (uint32_t i = 0; i < half; i++) {
            child->lo[i] = lo[i] & ~up[half + i];
            child->up[i] = up[i];
        }
    } else if (frame->step == STEP_ONE) {
        for (uint32_t i = 0; i < half; i++) {
            frame->r0[i] = child->res[i];
            child->lo[i] = lo[half + i] & ~up[i];
            child->up[i] = up[half + i];
        }
    } else {
        for (uint32_t i = 0; i < half; i++) {
            frame->r1[i] = child->res[i];
            child->lo[i] = (lo[i] & ~frame->r0[i]) | (lo[half + i] & ~frame->r1[i]);
            child->up[i] = up[i] & up[half + i];
        }
    }
}

/* branch for a frame of one word (n <= 6 inputs): its halves are its low and high 2^(n - 1)
 * bits. */
static void branch_word(isop_frame *frame, isop_frame *child, uint32_t n) {
    uint32_t shift = 1U << (n - 1);
    uint64_t lo0 = frame->lo[0] & child->mask;
    uint64_t lo1 = (frame->lo[0] >> shift) & child->mask;
    uint64_t up0 = frame->up[0] & child->mask;
    uint64_t up1 = (frame->up[0] >> shift) & child->mask;
    if (frame->step == STEP_ZERO) {
        child->lo[0] = lo0 & ~up1;
        child->up[0] = up0;
    } else if (frame->step == STEP_ONE) {
        frame->r0[0] = child->res[0];
        child->lo[0] = lo1 & ~up0;
        child->up[0] = up1;
    } else {
        frame->r1[0] = child->res[0];
        child->lo[0] = (lo0 & ~frame->r0[0]) | (lo1 & ~frame->r1[0]);
        child->up[0] = up0 & up1;
    }
}

/* Sets frame n - 1 to frame n's next branch: its L and U, and input n - 1's literal. Before the 1
 * and the shared branch, keeps the R of the branch that ended (frame n - 1's res). */
static void branch(isop_run *run, uint32_t n) {
    static const char LITS[] = {'0', '1', '-'};
    isop_frame *frame = &run->frames[n];
    if (n > WORD_SHIFT) {
        branch_words(frame, &run->frames[n - 1]);
    } else {
        branch_word(frame, &run->frames[n - 1], n);
    }
    run->lits[n - 1] = LITS[frame->step];
    frame->step++;
}

/* Frame n's R from its branches': R0 | R* where input n - 1 is 0, R1 | R* where it is 1. */
static void join(isop_run *run, uint32_t n) {
    isop_frame *frame = &run->frames[n];
    const uint64_t *shared = run->frames[n - 1].res;
    uint32_t words = run->frames[n - 1].words;
    if (n > WORD_SHIFT) {
        for (uint32_t i = 0; i < words; i++) {
            frame->res[i] = frame->r0[i] | shared[i];
            frame->res[words + i] = frame->r1[i] | shared[i];
        }
        return;
    }
    uint32_t half = 1U << (n - 1);
    frame->res[0] = (frame->r0[0] | shared[0]) | ((frame->r1[0] | shared[0]) << half);
}

/* The call of frame k (its lo and up set) to its end, or until run stops; rows in order. */
static void run_frames(isop_run *run) {
    if (!start(run, run->k)) {
        return;
    }
    uint32_t top = run->k;
    while (!run->stopped) {
        const isop_frame *frame = &run->frames[top];
        if (frame->step == STEP_JOIN) {
            join(run, top);
            if (top == run->k) {
                return;
            }
            top++;
            continue;
        }
        assert(top > 0); /* a call on 0 inputs always ends at once (L within U) */
        branch(run, top);
        if (start(run, top - 1)) {
            top--;
        }
    }
}

/* Carves the tables of levels 0 .. run->k out of one block of a new arena (*arena; the caller
 * destroys it, NULL when its creation failed). */
static odin3_status tables_create(isop_run *run, odin3_arena **arena) {
    size_t words = 0;
    for (uint32_t level = 0; level <= run->k; level++) {
        size_t below = level > 0 ? odin3_blif_isop_words(level - 1) : 0U;
        words += TABLES_PER_LEVEL * (size_t)odin3_blif_isop_words(level) + BRANCH_TABLES * below;
    }
    size_t bytes = words * sizeof(uint64_t);
    *arena = odin3_arena_create(bytes + ARENA_SLACK);
    uint64_t *pool = *arena != NULL ? odin3_arena_alloc(*arena, bytes) : NULL;
    if (pool == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t level = 0; level <= run->k; level++) {
        isop_frame *frame = &run->frames[level];
        size_t own = odin3_blif_isop_words(level);
        size_t below = level > 0 ? odin3_blif_isop_words(level - 1) : 0U;
        frame->lo = pool;
        frame->up = pool + own;
        frame->res = frame->up + own;
        frame->r0 = level > 0 ? frame->res + own : NULL;
        frame->r1 = level > 0 ? frame->r0 + below : NULL;
        frame->words = (uint32_t)own;
        frame->mask = level_mask(level);
        pool = frame->res + own + BRANCH_TABLES * below;
    }
    return ODIN3_OK;
}

/* Covers one set of tt (offset: the complement of f) into run->cover (NULL: counts only), up to
 * run->limit rows; run->rows, run->stopped and run->st tell how it ended. */
static void cover_set(isop_run *run, const uint64_t *tt, bool offset) {
    isop_frame *root = &run->frames[run->k];
    uint64_t mask = level_mask(run->k);
    for (uint32_t i = 0; i < odin3_blif_isop_words(run->k); i++) {
        root->lo[i] = (offset ? ~tt[i] : tt[i]) & mask;
        root->up[i] = root->lo[i];
    }
    run->out = offset ? '0' : '1';
    run->rows = 0;
    run->stopped = false;
    run->st = ODIN3_OK;
    run_frames(run);
}

/* Cuts cover back to len bytes (what it held before a failed or replaced set). */
static void cover_rewind(odin3_strbuf *cover, size_t len) {
    if (cover->data != NULL) {
        cover->len = len;
        cover->data[len] = '\0';
    }
}

/* 0 or 1 when tt (k = inputs inputs, bits from 2^k up ignored) is that constant, else -1. */
static int constant_of(const uint64_t *tt, uint32_t inputs) {
    uint64_t mask = level_mask(inputs);
    bool zero = true;
    bool one = true;
    for (uint32_t i = 0; i < odin3_blif_isop_words(inputs); i++) {
        zero = zero && (tt[i] & mask) == 0;
        one = one && (tt[i] & mask) == mask;
    }
    if (zero || one) {
        return one ? 1 : 0;
    }
    return -1;
}

odin3_status odin3_blif_isop_set(const uint64_t *tt, uint32_t inputs, bool offset,
                                 odin3_strbuf *cover, uint32_t *rows) {
    if (tt == NULL || cover == NULL || rows == NULL || inputs > ODIN3_BLIF_ISOP_MAX_K) {
        return ODIN3_ERR_INVALID_ARG;
    }
    isop_run run = {.k = inputs, .limit = UINT32_MAX, .cover = cover};
    size_t before = cover->len;
    odin3_arena *arena = NULL;
    odin3_status st = tables_create(&run, &arena);
    if (st == ODIN3_OK) {
        cover_set(&run, tt, offset);
        st = run.st;
    }
    odin3_arena_destroy(arena);
    if (st != ODIN3_OK) {
        cover_rewind(cover, before);
        return st;
    }
    *rows = run.rows;
    return ODIN3_OK;
}

/* The on-set's rows into run->cover; then the off-set counted against them, and written in their
 * place when it has fewer (plan 3E point (t)). *rows: the chosen set's. */
static void cover_shorter(isop_run *run, const uint64_t *tt, size_t before, uint32_t *rows) {
    odin3_strbuf *cover = run->cover;
    cover_set(run, tt, false);
    uint32_t on_rows = run->rows;
    *rows = on_rows;
    if (run->st != ODIN3_OK) {
        return;
    }
    run->cover = NULL;
    run->limit = on_rows;
    cover_set(run, tt, true);
    if (run->stopped || run->rows >= on_rows) {
        return;
    }
    cover_rewind(cover, before);
    run->cover = cover;
    run->limit = UINT32_MAX;
    cover_set(run, tt, true);
    *rows = run->rows;
}

odin3_status odin3_blif_isop(const uint64_t *tt, uint32_t inputs, odin3_strbuf *cover,
                             uint32_t *rows, int *constant) {
    if (tt == NULL || cover == NULL || rows == NULL || constant == NULL ||
        inputs > ODIN3_BLIF_ISOP_MAX_K) {
        return ODIN3_ERR_INVALID_ARG;
    }
    int value = constant_of(tt, inputs);
    if (value >= 0) {
        *rows = 0;
        *constant = value;
        return ODIN3_OK;
    }
    isop_run run = {.k = inputs, .limit = UINT32_MAX, .cover = cover};
    size_t before = cover->len;
    uint32_t chosen = 0;
    odin3_arena *arena = NULL;
    odin3_status st = tables_create(&run, &arena);
    if (st == ODIN3_OK) {
        cover_shorter(&run, tt, before, &chosen);
        st = run.st;
    }
    odin3_arena_destroy(arena);
    if (st != ODIN3_OK) {
        cover_rewind(cover, before);
        return st;
    }
    *rows = chosen;
    *constant = -1;
    return ODIN3_OK;
}
