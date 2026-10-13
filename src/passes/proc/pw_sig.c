/*
 * pw_sig.c — signal operations on the working form (2E spec §3): Yosys's SigSpec replace,
 * remove2, extract, ==, the constant predicates and sort_and_unify, each O(length) over the
 * module's scratch index (sort_unify adds its sort). Each result is a new run of pw->bits.
 */
#include "passes/proc/pw.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_internal.h"
#include "util/log.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* --- the scratch index --------------------------------------------------------------------- */

odin3_status odin3_proc_scratch_grow(odin3_proc_mctx *mctx) {
    size_t end = odin3_module_net_end(mctx->module);
    if (odin3_vec_reserve(&mctx->scratch, end) != ODIN3_OK ||
        odin3_vec_reserve(&mctx->touched, end) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (mctx->scratch.len < end) {
        (void)odin3_vec_push(&mctx->scratch); /* reserved; zeroed */
    }
    return ODIN3_OK;
}

void odin3_proc_scratch_set(odin3_proc_mctx *mctx, odin3_net_id net, uint32_t value) {
    assert(net.v < mctx->scratch.len && value != 0);
    if (net.v >= mctx->scratch.len) {
        return;
    }
    uint32_t *slot = odin3_vec_at(&mctx->scratch, net.v);
    if (*slot == 0) {
        *(uint32_t *)odin3_vec_push(&mctx->touched) = net.v; /* reserved for every net */
    }
    *slot = value;
}

uint32_t odin3_proc_scratch_get(const odin3_proc_mctx *mctx, odin3_net_id net) {
    return net.v < mctx->scratch.len ? *(const uint32_t *)odin3_vec_cat(&mctx->scratch, net.v) : 0;
}

void odin3_proc_scratch_reset(odin3_proc_mctx *mctx) {
    for (size_t i = 0; i < mctx->touched.len; i++) {
        uint32_t net = *(const uint32_t *)odin3_vec_cat(&mctx->touched, i);
        *(uint32_t *)odin3_vec_at(&mctx->scratch, net) = 0;
    }
    odin3_vec_clear(&mctx->touched);
}

/* --- runs ---------------------------------------------------------------------------------- */

static bool is_net(odin3_pw_bit bit) {
    return (bit & ODIN3_PW_CONST) == 0;
}

const odin3_pw_bit *odin3_pw_sig_bits(const odin3_pw *pw, odin3_pw_sig sig) {
    const odin3_pw_bit *bits = pw->bits.data;
    return bits != NULL ? bits + sig.first : NULL; /* no offset on a NULL pointer (UBSan) */
}

/*
 * Appends len zero bits as a new run *out; *dst gets the writable bits (NULL for an empty run,
 * which allocates nothing). ODIN3_ERR_NO_MEMORY (also, logged, past 2^32 bits), pw unchanged.
 */
static odin3_status run_alloc(odin3_pw *pw, uint32_t len, odin3_pw_sig *out, odin3_pw_bit **dst) {
    size_t base = pw->bits.len;
    *dst = NULL;
    if ((uint64_t)base + len > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "proc: a process's signals exceed 2^32 bits");
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = (odin3_pw_sig){(uint32_t)base, len};
    if (len == 0) {
        return ODIN3_OK;
    }
    if (odin3_vec_reserve(&pw->bits, base + len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < len; i++) {
        (void)odin3_vec_push(&pw->bits); /* reserved */
    }
    *dst = (odin3_pw_bit *)pw->bits.data + base;
    return ODIN3_OK;
}

odin3_status odin3_pw_sig_new(odin3_pw *pw, const odin3_pw_bit *bits, uint32_t len,
                              odin3_pw_sig *out) {
    /* bits may lie in pw->bits, which run_alloc may move: keep its offset instead. */
    uintptr_t lo = (uintptr_t)pw->bits.data;
    uintptr_t src = (uintptr_t)bits;
    bool inside = len > 0 && lo != 0 && src >= lo && src < lo + pw->bits.len * sizeof *bits;
    size_t offset = inside ? (src - lo) / sizeof *bits : 0;
    odin3_pw_sig sig = {0, 0};
    odin3_pw_bit *dst = NULL;
    odin3_status st = run_alloc(pw, len, &sig, &dst);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_pw_bit *from = inside ? (const odin3_pw_bit *)pw->bits.data + offset : bits;
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = from[i];
    }
    *out = sig;
    return ODIN3_OK;
}

/* Marks every net of pattern in the scratch index with its first position + 1. */
static void mark_pattern(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig pattern) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, pattern);
    for (uint32_t j = 0; j < pattern.len; j++) {
        odin3_net_id net = {bits[j]};
        if (is_net(bits[j]) && odin3_proc_scratch_get(mctx, net) == 0) {
            odin3_proc_scratch_set(mctx, net, j + 1);
        }
    }
}

/* The scratch entry of bit: 0 for a constant or an unmarked net. */
static uint32_t marked(const odin3_proc_mctx *mctx, odin3_pw_bit bit) {
    return is_net(bit) ? odin3_proc_scratch_get(mctx, (odin3_net_id){bit}) : 0;
}

/* The scratch index grown and pattern marked; NO_MEMORY with nothing marked. */
static odin3_status open_pattern(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig pattern) {
    odin3_status st = odin3_proc_scratch_grow(mctx);
    if (st == ODIN3_OK) {
        mark_pattern(mctx, pw, pattern);
    }
    return st;
}

odin3_status odin3_pw_sig_replace(odin3_proc_mctx *mctx, odin3_pw *pw, const odin3_pw_replace *req,
                                  odin3_pw_sig *out) {
    if (req->sig.len != req->other.len || req->pattern.len != req->with.len) {
        odin3_log(ODIN3_LOG_ERROR,
                  "proc: sig_replace: widths differ (sig %u, other %u, pattern %u, "
                  "with %u)",
                  req->sig.len, req->other.len, req->pattern.len, req->with.len);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = open_pattern(mctx, pw, req->pattern);
    odin3_pw_sig sig = {0, 0};
    odin3_pw_bit *dst = NULL;
    st = st == ODIN3_OK ? run_alloc(pw, req->other.len, &sig, &dst) : st;
    if (st == ODIN3_OK) {
        const odin3_pw_bit *from = odin3_pw_sig_bits(pw, req->sig);
        const odin3_pw_bit *with = odin3_pw_sig_bits(pw, req->with);
        const odin3_pw_bit *other = odin3_pw_sig_bits(pw, req->other);
        for (uint32_t i = 0; i < req->other.len; i++) {
            uint32_t at = marked(mctx, from[i]);
            dst[i] = at != 0 ? with[at - 1] : other[i];
        }
        *out = sig;
    }
    odin3_proc_scratch_reset(mctx);
    return st;
}

/* in.b empty (no other) or as long as in.a; INVALID_ARG (logged) otherwise. */
static odin3_status check_pair(const char *what, odin3_pw_pair in) {
    if (in.b.len != 0 && in.b.len != in.a.len) {
        odin3_log(ODIN3_LOG_ERROR, "proc: %s: widths differ (%u, %u)", what, in.a.len, in.b.len);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The number of positions of a whose bit is (want true) or is not (want false) a marked net. */
static uint32_t count_marked(const odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig sig,
                             bool want) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    uint32_t count = 0;
    for (uint32_t i = 0; i < sig.len; i++) {
        count += (marked(mctx, bits[i]) != 0) == want ? 1U : 0U;
    }
    return count;
}

/* Copies the positions of in.a (and in.b) whose a bit's marked state is `want` to dst (dst_b). */
typedef struct pick {
    odin3_pw_bit *dst;
    odin3_pw_bit *dst_b; /* NULL: no other */
    bool want;
} pick;

static void pick_bits(const odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_pair in, pick to) {
    const odin3_pw_bit *bits_a = odin3_pw_sig_bits(pw, in.a);
    const odin3_pw_bit *bits_b = odin3_pw_sig_bits(pw, in.b);
    uint32_t filled = 0;
    for (uint32_t i = 0; i < in.a.len; i++) {
        if ((marked(mctx, bits_a[i]) != 0) != to.want) {
            continue;
        }
        to.dst[filled] = bits_a[i];
        if (to.dst_b != NULL) {
            to.dst_b[filled] = bits_b[i];
        }
        filled++;
    }
}

/* remove2's runs once the pattern is marked: in.a's unmarked positions, and in.b's. */
static odin3_status remove2_runs(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                 odin3_pw_pair *out) {
    uint32_t keep = count_marked(mctx, pw, in.a, false);
    bool other = in.b.len != 0;
    odin3_pw_sig run = {0, 0};
    odin3_pw_bit *dst = NULL;
    odin3_status st = run_alloc(pw, other ? 2 * keep : keep, &run, &dst);
    if (st != ODIN3_OK) {
        return st;
    }
    pick_bits(mctx, pw, in, (pick){dst, other && dst != NULL ? dst + keep : NULL, false});
    out->a = (odin3_pw_sig){run.first, keep};
    out->b = (odin3_pw_sig){run.first + (other ? keep : run.len), other ? keep : 0};
    return ODIN3_OK;
}

odin3_status odin3_pw_sig_remove2(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                  odin3_pw_sig pattern, odin3_pw_pair *out) {
    odin3_status st = check_pair("sig_remove2", in);
    st = st == ODIN3_OK ? open_pattern(mctx, pw, pattern) : st;
    st = st == ODIN3_OK ? remove2_runs(mctx, pw, in, out) : st;
    odin3_proc_scratch_reset(mctx);
    return st;
}

/* extract's run once the pattern is marked: in.b (else in.a) at in.a's marked positions. */
static odin3_status extract_run(const odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                odin3_pw_sig *out) {
    uint32_t take = count_marked(mctx, pw, in.a, true);
    odin3_pw_sig run = {0, 0};
    odin3_pw_bit *dst = NULL;
    odin3_status st = run_alloc(pw, take, &run, &dst);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_pw_bit *bits_a = odin3_pw_sig_bits(pw, in.a);
    const odin3_pw_bit *bits_b = odin3_pw_sig_bits(pw, in.b.len != 0 ? in.b : in.a);
    uint32_t filled = 0;
    for (uint32_t i = 0; i < in.a.len; i++) {
        if (marked(mctx, bits_a[i]) != 0) {
            dst[filled++] = bits_b[i];
        }
    }
    *out = run;
    return ODIN3_OK;
}

odin3_status odin3_pw_sig_extract(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_pair in,
                                  odin3_pw_sig pattern, odin3_pw_sig *out) {
    odin3_status st = check_pair("sig_extract", in);
    st = st == ODIN3_OK ? open_pattern(mctx, pw, pattern) : st;
    st = st == ODIN3_OK ? extract_run(mctx, pw, in, out) : st;
    odin3_proc_scratch_reset(mctx);
    return st;
}

odin3_status odin3_pw_pattern_open(odin3_proc_mctx *mctx, const odin3_pw *pw,
                                   odin3_pw_sig pattern) {
    return open_pattern(mctx, pw, pattern);
}

bool odin3_pw_pattern_meets(const odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig sig) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    for (uint32_t i = 0; i < sig.len; i++) {
        if (marked(mctx, bits[i]) != 0) {
            return true;
        }
    }
    return false;
}

void odin3_pw_pattern_close(odin3_proc_mctx *mctx) {
    odin3_proc_scratch_reset(mctx);
}

/* --- predicates ---------------------------------------------------------------------------- */

bool odin3_pw_sig_equal(const odin3_pw *pw, odin3_pw_sig lhs, odin3_pw_sig rhs) {
    if (lhs.len != rhs.len) {
        return false;
    }
    const odin3_pw_bit *bits_l = odin3_pw_sig_bits(pw, lhs);
    const odin3_pw_bit *bits_r = odin3_pw_sig_bits(pw, rhs);
    for (uint32_t i = 0; i < lhs.len; i++) {
        if (bits_l[i] != bits_r[i]) {
            return false;
        }
    }
    return true;
}

/* True when every bit of sig is one of the bits in set (n of them). */
static bool all_in(const odin3_pw *pw, odin3_pw_sig sig, const odin3_pw_bit *set, size_t n) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    for (uint32_t i = 0; i < sig.len; i++) {
        bool found = false;
        for (size_t j = 0; j < n && !found; j++) {
            found = bits[i] == set[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool odin3_pw_sig_is_fully_const(const odin3_pw *pw, odin3_pw_sig sig) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    for (uint32_t i = 0; i < sig.len; i++) {
        if (is_net(bits[i])) {
            return false;
        }
    }
    return true;
}

bool odin3_pw_sig_is_fully_def(const odin3_pw *pw, odin3_pw_sig sig) {
    static const odin3_pw_bit k_def[] = {ODIN3_PW_S0, ODIN3_PW_S1};
    return all_in(pw, sig, k_def, sizeof k_def / sizeof k_def[0]);
}

bool odin3_pw_sig_is_fully_undef(const odin3_pw *pw, odin3_pw_sig sig) {
    static const odin3_pw_bit k_undef[] = {ODIN3_PW_SX, ODIN3_PW_SZ};
    return all_in(pw, sig, k_undef, sizeof k_undef / sizeof k_undef[0]);
}

bool odin3_pw_sig_as_bool(const odin3_pw *pw, odin3_pw_sig sig) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    for (uint32_t i = 0; i < sig.len; i++) {
        if (bits[i] == ODIN3_PW_S1) {
            return true;
        }
    }
    return false;
}

/* --- sort_unify ---------------------------------------------------------------------------- */

typedef struct keyed_bit {
    uint64_t key;
    odin3_pw_bit bit;
} keyed_bit;

static int keyed_cmp(const void *lhs, const void *rhs) {
    uint64_t key_l = ((const keyed_bit *)lhs)->key;
    uint64_t key_r = ((const keyed_bit *)rhs)->key;
    return (key_l > key_r) - (key_l < key_r);
}

/* The distinct bits of sig with their keys (constants by state, nets through the index). */
static void distinct_bits(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig sig,
                          odin3_vec *items) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sig);
    uint32_t consts_seen = 0; /* one bit per constant state */
    for (uint32_t i = 0; i < sig.len; i++) {
        odin3_pw_bit bit = bits[i];
        if (is_net(bit)) {
            if (odin3_proc_scratch_get(mctx, (odin3_net_id){bit}) != 0) {
                continue;
            }
            odin3_proc_scratch_set(mctx, (odin3_net_id){bit}, 1);
        } else {
            uint32_t mask = 1U << (bit & ~ODIN3_PW_CONST);
            if ((consts_seen & mask) != 0) {
                continue;
            }
            consts_seen |= mask;
        }
        keyed_bit *item = odin3_vec_push(items); /* reserved for sig.len */
        item->key = odin3_proc_raw_key(mctx, bit);
        item->bit = bit;
    }
}

odin3_status odin3_pw_sig_sort_unify(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_sig sig,
                                     odin3_pw_sig *out) {
    odin3_vec items;
    odin3_vec_init(&items, sizeof(keyed_bit));
    odin3_status st = odin3_proc_scratch_grow(mctx);
    if (st == ODIN3_OK) {
        st = odin3_vec_reserve(&items, sig.len);
    }
    if (st == ODIN3_OK) {
        distinct_bits(mctx, pw, sig, &items);
    }
    odin3_proc_scratch_reset(mctx);
    if (st == ODIN3_OK && items.len > 1) {
        qsort(items.data, items.len, sizeof(keyed_bit), keyed_cmp);
    }
    odin3_pw_sig run = {0, 0};
    odin3_pw_bit *dst = NULL;
    st = st == ODIN3_OK ? run_alloc(pw, (uint32_t)items.len, &run, &dst) : st;
    if (st == ODIN3_OK) {
        for (size_t i = 0; i < items.len; i++) {
            dst[i] = ((const keyed_bit *)odin3_vec_cat(&items, i))->bit;
        }
        *out = run;
    }
    odin3_vec_free(&items);
    return st;
}
