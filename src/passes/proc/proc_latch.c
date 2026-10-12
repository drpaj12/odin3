/*
 * proc_latch.c — the latch phase of proc (2E spec §8, §10, §14, §15; PR-14, PR-16; plan Task 8):
 * Yosys 0.55's proc_dlatch for a whole module after step B. The database is a snapshot of the
 * module as step B left it (the $mux/$pmux driving each net, the distinct inputs of each mux, the
 * readers of each net) plus the hash-consed hold rules; quickcheck reads only the snapshot, the
 * walk (find_mux_feedback, memoised per (net, set_undef) within one latch output) reads the pins
 * live. Then the holds and latches (W1), the no-latch connections with their init removal (W2),
 * the mux fix-up and M2. Every walk uses an explicit stack (CLAUDE.md: no recursion in src/).
 */
#include "passes/proc/proc_internal.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/proc/proc.h"
#include "passes/proc/pw.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/idindex.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    USERS_MANY = 2, /* readers are counted up to this: only "more than one" matters */
    MUX_A = 0,      /* $mux and $pmux port order */
    MUX_B = 1,
    MUX_S = 2,
    MUX_Y = 3,
    MARK_NEEDLE = 1, /* quickcheck's net marks: epoch << MARK_FLAGS | flags */
    MARK_SEEN = 2,
    MARK_FLAGS = 2,
    MEMO_SHIFT = 32, /* the walk's memo values: out epoch << MEMO_SHIFT | rule */
    NOT_PARAMS = 3,  /* $not and $reduce_or: A_SIGNED, A_WIDTH, Y_WIDTH */
    AND_PARAMS = 5,  /* $and: A_SIGNED, B_SIGNED, A_WIDTH, B_WIDTH, Y_WIDTH */
    DLATCH_PARAMS = 2,
    PMUX_PARAMS = 2,
    MUX_PORTS = 4,
    DLATCH_INPUTS = 2
};

#define MARK_MASK ((1U << MARK_FLAGS) - 1U)
#define EPOCH_MAX (UINT32_MAX >> MARK_FLAGS)
#define RULE_MASK 0xffffffffULL

/* A mux's recorded inputs: a run of ldb->src. */
typedef struct ldb_span {
    uint32_t first, len;
} ldb_span;

/* A latch the phase made, for odin3_proc_ldb_latch. */
typedef struct ldb_latch {
    odin3_node_id node;
    uint32_t rule;
} ldb_latch;

/* One expanded mux bit of the walk (find_mux_feedback's frame). */
typedef struct lw_frame {
    uint64_t key;           /* its memo key: net << 1 | set_undef */
    odin3_node_id node;     /* the $mux/$pmux driving the net */
    uint32_t index;         /* the net's bit of Y */
    uint32_t width;         /* the mux's WIDTH */
    uint32_t n_sel;         /* its select bits */
    uint32_t step;          /* the next child: 0 is A, k the B slice k - 1; n_sel + 1 is done */
    size_t found_base;      /* its children in ldb->found */
    odin3_pin_id child_pin; /* the pin of the child being walked */
    odin3_pw_bit child_bit; /* that pin's bit when the walk entered it */
    bool set_undef;
} lw_frame;

/* One rule of the hold builder's stack. */
typedef struct hd_frame {
    uint32_t rule;
    uint32_t next_kid; /* children whose hold is already in ldb->hbits */
    size_t bits_base;  /* its children's holds in ldb->hbits */
    odin3_pw_bit term; /* the rule's own term (0 for none) */
    bool started;
} hd_frame;

struct odin3_proc_ldb {
    bool built;                 /* the snapshot below is current */
    uint32_t net_end, node_end; /* the module's ends at the build */
    odin3_vec drv;              /* uint32_t per net: the Y pin of the $mux/$pmux on it, 0 none */
    odin3_vec users;            /* uint8_t per net: reading pins, saturated at USERS_MANY */
    odin3_vec src_at;           /* ldb_span per node: a mux's recorded inputs */
    odin3_vec src;              /* uint32_t: the distinct non-constant nets on A and B */
    odin3_vec net_mark;         /* uint32_t per net: quickcheck's marks */
    odin3_vec node_mark;        /* uint32_t per node: quickcheck's epoch of a scanned mux */
    odin3_vec queue;            /* uint32_t: quickcheck's nets, room for net_end */
    uint32_t qc_epoch;
    odin3_vec rules;        /* odin3_proc_rule; IDs below ODIN3_PROC_RULE_FIRST unused */
    odin3_vec kids;         /* uint32_t: the rules' children */
    odin3_idindex *rule_ix; /* hash-consing of rules */
    odin3_u64map *memo;     /* the walk's memo; rule 0 while the bit is on the stack */
    uint32_t out_epoch;     /* the walk's current latch output */
    odin3_vec frames;       /* lw_frame */
    odin3_vec found;        /* uint32_t: the children of the frames on the stack */
    odin3_u64map *holds;    /* rule -> odin3_pw_bit of its hold */
    odin3_vec hframes;      /* hd_frame */
    odin3_vec hbits;        /* odin3_pw_bit: the children's holds of the frames on the stack */
    odin3_vec latches;      /* ldb_latch, creation order */
    odin3_celltype_id t_mux, t_pmux, t_port_in, t_port_inout;
};

/* --- small helpers ------------------------------------------------------------------------- */

static bool is_const(odin3_pw_bit bit) {
    return (bit & ODIN3_PW_CONST) != 0;
}

static odin3_pw_bit const_code(odin3_const value) {
    switch (value) {
    case ODIN3_CONST_0:
        return ODIN3_PW_S0;
    case ODIN3_CONST_1:
        return ODIN3_PW_S1;
    case ODIN3_CONST_Z:
        return ODIN3_PW_SZ;
    default:
        return ODIN3_PW_SX;
    }
}

/* A pin's net as the working form reads it (§4): a constant net as its constant (an open pin as
 * x), else the root of its class in the net map. */
static odin3_pw_bit pin_bit(odin3_proc_mctx *mctx, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(mctx->module, pin);
    if (!odin3_net_valid(net)) {
        return ODIN3_PW_SX;
    }
    odin3_const value = odin3_net_const_value(mctx->module, net);
    return value != ODIN3_CONST_NONE ? const_code(value) : odin3_proc_find(mctx, net.v);
}

/* Appends n zeroed elements. */
static odin3_status vec_zeros(odin3_vec *vec, size_t n) {
    if (odin3_vec_reserve(vec, vec->len + n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        (void)odin3_vec_push(vec); /* reserved */
    }
    return ODIN3_OK;
}

/* Drops elements from the end until len is left. */
static void vec_truncate(odin3_vec *vec, size_t len) {
    while (vec->len > len) {
        odin3_vec_pop(vec);
    }
}

static uint64_t vec_bytes(const odin3_vec *vec) {
    return (uint64_t)vec->cap * vec->elem_size;
}

static int cmp_u32(const void *lhs, const void *rhs) {
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;
    if (left != right) {
        return left < right ? -1 : 1;
    }
    return 0;
}

/* --- the database (§8) --------------------------------------------------------------------- */

/* The per-net and per-node snapshot and the walk's scratch, freed after the phase. */
static void ldb_release(odin3_proc_ldb *ldb) {
    odin3_vec *vecs[] = {&ldb->drv,      &ldb->users,     &ldb->src_at, &ldb->src,
                         &ldb->net_mark, &ldb->node_mark, &ldb->queue,  &ldb->frames,
                         &ldb->found,    &ldb->hframes,   &ldb->hbits};
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        odin3_vec_free(vecs[i]);
    }
    odin3_u64map_destroy(ldb->memo);
    ldb->memo = NULL;
    ldb->built = false;
}

void odin3_proc_ldb_free(odin3_proc_mctx *mctx) {
    if (mctx == NULL || mctx->ldb == NULL) {
        return;
    }
    odin3_proc_ldb *ldb = mctx->ldb;
    ldb_release(ldb);
    odin3_vec_free(&ldb->rules);
    odin3_vec_free(&ldb->kids);
    odin3_vec_free(&ldb->latches);
    odin3_idindex_destroy(ldb->rule_ix);
    odin3_u64map_destroy(ldb->holds);
    odin3_util_free(ldb);
    mctx->ldb = NULL;
}

uint64_t odin3_proc_ldb_bytes(const odin3_proc_mctx *mctx) {
    const odin3_proc_ldb *ldb = mctx != NULL ? mctx->ldb : NULL;
    if (ldb == NULL) {
        return 0;
    }
    const odin3_vec *vecs[] = {&ldb->drv,      &ldb->users,     &ldb->src_at, &ldb->src,
                               &ldb->net_mark, &ldb->node_mark, &ldb->queue,  &ldb->frames,
                               &ldb->found,    &ldb->hframes,   &ldb->hbits,  &ldb->rules,
                               &ldb->kids,     &ldb->latches};
    uint64_t bytes = sizeof *ldb + odin3_u64map_bytes(ldb->memo) + odin3_u64map_bytes(ldb->holds) +
                     odin3_idindex_bytes_reserved(ldb->rule_ix);
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        bytes += vec_bytes(vecs[i]);
    }
    return bytes;
}

/* A new, empty database: its maps and the unused rule IDs below ODIN3_PROC_RULE_FIRST. */
static odin3_proc_ldb *ldb_create(void) {
    odin3_proc_ldb *ldb = odin3_util_calloc(sizeof *ldb);
    if (ldb == NULL) {
        return NULL;
    }
    odin3_vec_init(&ldb->drv, sizeof(uint32_t));
    odin3_vec_init(&ldb->users, sizeof(uint8_t));
    odin3_vec_init(&ldb->src_at, sizeof(ldb_span));
    odin3_vec_init(&ldb->src, sizeof(uint32_t));
    odin3_vec_init(&ldb->net_mark, sizeof(uint32_t));
    odin3_vec_init(&ldb->node_mark, sizeof(uint32_t));
    odin3_vec_init(&ldb->queue, sizeof(uint32_t));
    odin3_vec_init(&ldb->rules, sizeof(odin3_proc_rule));
    odin3_vec_init(&ldb->kids, sizeof(uint32_t));
    odin3_vec_init(&ldb->frames, sizeof(lw_frame));
    odin3_vec_init(&ldb->found, sizeof(uint32_t));
    odin3_vec_init(&ldb->hframes, sizeof(hd_frame));
    odin3_vec_init(&ldb->hbits, sizeof(odin3_pw_bit));
    odin3_vec_init(&ldb->latches, sizeof(ldb_latch));
    ldb->rule_ix = odin3_idindex_create(0);
    ldb->memo = odin3_u64map_create(0);
    ldb->holds = odin3_u64map_create(0);
    return ldb;
}

/* Sizes every per-net and per-node array for the module as it stands (all zero). */
static odin3_status ldb_size(odin3_proc_ldb *ldb, const odin3_module *module) {
    ldb->net_end = odin3_module_net_end(module);
    ldb->node_end = odin3_module_node_end(module);
    odin3_status st = vec_zeros(&ldb->rules, ODIN3_PROC_RULE_FIRST);
    st = st == ODIN3_OK ? vec_zeros(&ldb->drv, ldb->net_end) : st;
    st = st == ODIN3_OK ? vec_zeros(&ldb->users, ldb->net_end) : st;
    st = st == ODIN3_OK ? vec_zeros(&ldb->net_mark, ldb->net_end) : st;
    st = st == ODIN3_OK ? vec_zeros(&ldb->node_mark, ldb->node_end) : st;
    st = st == ODIN3_OK ? vec_zeros(&ldb->src_at, ldb->node_end) : st;
    if (st == ODIN3_OK && odin3_vec_reserve(&ldb->queue, (size_t)ldb->net_end + 1) != ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

/* Counts the reading pins of node (Yosys's sigusers): a port counts one per pin it drives into
 * the module when it is an input or inout port; a process none; any other node its readers. */
static void ldb_count_users(odin3_proc_ldb *ldb, const odin3_module *module, odin3_node_id node,
                            const odin3_celltype_def *def) {
    odin3_celltype_id type = odin3_node_type(module, node);
    bool port = def->gran == ODIN3_GRAN_PORT;
    bool port_drives = type.v == ldb->t_port_in.v || type.v == ldb->t_port_inout.v;
    if ((def->flags & ODIN3_CT_PROCESS) != 0 || (port && !port_drives)) {
        return;
    }
    odin3_pinslice pins = odin3_node_pins(module, node);
    uint8_t *users = ldb->users.data;
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_net_id net = odin3_pin_net(module, pin);
        bool counts = port ? odin3_pin_drives(module, pin) : odin3_pin_reads(module, pin);
        if (counts && odin3_net_valid(net) && net.v < ldb->net_end && users[net.v] < USERS_MANY) {
            users[net.v]++;
        }
    }
}

/* Records a live $mux/$pmux: the Y pin of each of its output nets, and the snapshot of its
 * distinct non-constant A and B nets (net_mark holds the node's ID while it is recorded). */
static odin3_status ldb_record_mux(odin3_proc_ldb *ldb, const odin3_module *module,
                                   odin3_node_id node) {
    odin3_pinslice ypins = odin3_node_port(module, node, MUX_Y);
    uint32_t *drv = ldb->drv.data;
    for (uint32_t k = 0; k < ypins.count; k++) {
        odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){ypins.first.v + k});
        if (odin3_net_valid(net) && net.v < ldb->net_end) {
            drv[net.v] = ypins.first.v + k;
        }
    }
    ldb_span span = {(uint32_t)ldb->src.len, 0};
    for (uint32_t port = MUX_A; port <= MUX_B; port++) {
        odin3_pinslice pins = odin3_node_port(module, node, port);
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){pins.first.v + k});
            uint32_t *mark = ldb->net_mark.data;
            if (!odin3_net_valid(net) || net.v >= ldb->net_end || mark[net.v] == node.v ||
                odin3_net_const_value(module, net) != ODIN3_CONST_NONE) {
                continue;
            }
            mark[net.v] = node.v;
            uint32_t *slot = odin3_vec_push(&ldb->src);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = net.v;
            span.len++;
        }
    }
    *(ldb_span *)odin3_vec_at(&ldb->src_at, node.v) = span;
    return ODIN3_OK;
}

/* Fills the snapshot from every live node of the module. */
static odin3_status ldb_scan(odin3_proc_ldb *ldb, const odin3_proc_mctx *mctx) {
    const odin3_module *module = mctx->module;
    for (uint32_t i = 1; i < ldb->node_end; i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(module, node);
        const odin3_celltype_def *def = odin3_celltype_get(mctx->design, type);
        if (def == NULL) {
            continue;
        }
        ldb_count_users(ldb, module, node, def);
        if (type.v == ldb->t_mux.v || type.v == ldb->t_pmux.v) {
            odin3_status st = ldb_record_mux(ldb, module, node);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    memset(ldb->net_mark.data, 0, ldb->net_mark.len * sizeof(uint32_t));
    return ODIN3_OK;
}

odin3_status odin3_proc_ldb_build(odin3_proc_mctx *mctx) {
    if (mctx == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: ldb_build: no module context");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_proc_ldb_free(mctx);
    odin3_proc_ldb *ldb = ldb_create();
    if (ldb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    mctx->ldb = ldb;
    ldb->t_mux = odin3_proc_type(mctx, "$mux");
    ldb->t_pmux = odin3_proc_type(mctx, "$pmux");
    ldb->t_port_in = odin3_proc_type(mctx, "$port_in");
    ldb->t_port_inout = odin3_proc_type(mctx, "$port_inout");
    odin3_status st = ldb->rule_ix != NULL && ldb->memo != NULL && ldb->holds != NULL
                          ? ldb_size(ldb, mctx->module)
                          : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? ldb_scan(ldb, mctx) : st;
    if (st != ODIN3_OK) {
        odin3_proc_ldb_free(mctx);
        return st;
    }
    ldb->built = true;
    return ODIN3_OK;
}

/* --- quickcheck (§8 step 1) ---------------------------------------------------------------- */

/* Starts a quickcheck: a new epoch for the marks (all reset when the epochs run out). */
static void qc_begin(odin3_proc_ldb *ldb) {
    if (ldb->qc_epoch >= EPOCH_MAX) {
        memset(ldb->net_mark.data, 0, ldb->net_mark.len * sizeof(uint32_t));
        memset(ldb->node_mark.data, 0, ldb->node_mark.len * sizeof(uint32_t));
        ldb->qc_epoch = 0;
    }
    ldb->qc_epoch++;
    odin3_vec_clear(&ldb->queue);
}

static uint32_t qc_flags(const odin3_proc_ldb *ldb, uint32_t net) {
    uint32_t mark = ((const uint32_t *)ldb->net_mark.data)[net];
    return (mark >> MARK_FLAGS) == ldb->qc_epoch ? (mark & MARK_MASK) : 0;
}

static void qc_mark(odin3_proc_ldb *ldb, uint32_t net, uint32_t flag) {
    uint32_t flags = qc_flags(ldb, net) | flag;
    ((uint32_t *)ldb->net_mark.data)[net] = ldb->qc_epoch << MARK_FLAGS | flags;
}

/* Queues net bit for the breadth-first walk unless it was queued already; constants and nets made
 * after the build are no mux's output and are skipped. */
static void qc_push(odin3_proc_ldb *ldb, odin3_pw_bit bit) {
    if (is_const(bit) || bit >= ldb->net_end || (qc_flags(ldb, bit) & MARK_SEEN) != 0) {
        return;
    }
    qc_mark(ldb, bit, MARK_SEEN);
    uint32_t *slot = odin3_vec_push(&ldb->queue); /* reserved: each net is queued once */
    assert(slot != NULL);
    *slot = bit;
}

/* The breadth-first walk over the queued nets: true when a recorded input of a mux on the way is
 * a needle. Each mux is scanned once per quickcheck. */
static bool qc_scan(const odin3_proc_mctx *mctx, odin3_proc_ldb *ldb) {
    const uint32_t *drv = ldb->drv.data;
    uint32_t *node_mark = ldb->node_mark.data;
    for (size_t head = 0; head < ldb->queue.len; head++) {
        uint32_t pin = drv[((const uint32_t *)ldb->queue.data)[head]];
        if (pin == 0) {
            continue;
        }
        odin3_node_id node = odin3_pin_node(mctx->module, (odin3_pin_id){pin});
        if (node.v >= ldb->node_end || node_mark[node.v] == ldb->qc_epoch) {
            continue;
        }
        node_mark[node.v] = ldb->qc_epoch;
        ldb_span span = *(const ldb_span *)odin3_vec_cat(&ldb->src_at, node.v);
        for (uint32_t k = 0; k < span.len; k++) {
            uint32_t net = ((const uint32_t *)ldb->src.data)[span.first + k];
            if ((qc_flags(ldb, net) & MARK_NEEDLE) != 0) {
                return true;
            }
            qc_push(ldb, net);
        }
    }
    return false;
}

bool odin3_proc_quickcheck(odin3_proc_mctx *mctx, const odin3_proc_qc *qc) {
    odin3_proc_ldb *ldb = mctx != NULL ? mctx->ldb : NULL;
    if (ldb == NULL || !ldb->built || qc == NULL || qc->pw == NULL) {
        return false;
    }
    qc_begin(ldb);
    for (uint32_t k = 0; k < qc->lhs.len; k++) {
        odin3_pw_bit bit = odin3_proc_find(mctx, odin3_pw_sig_bits(qc->pw, qc->lhs)[k]);
        if (!is_const(bit) && bit < ldb->net_end) {
            qc_mark(ldb, bit, MARK_NEEDLE);
        }
    }
    for (uint32_t k = 0; k < qc->rhs.len; k++) {
        qc_push(ldb, odin3_proc_find(mctx, odin3_pw_sig_bits(qc->pw, qc->rhs)[k]));
    }
    return qc_scan(mctx, ldb);
}

/* --- rules (§8: hash-consed, IDs from 3) --------------------------------------------------- */

typedef struct rule_probe {
    odin3_pw_bit signal, match;
    const uint32_t *kids;
    uint32_t n;
} rule_probe;

static bool rule_equals(const void *ctx, uint32_t id, odin3_bytes probe) {
    const odin3_proc_ldb *ldb = ctx;
    const rule_probe *want = probe.ptr;
    const odin3_proc_rule *rule = odin3_vec_cat(&ldb->rules, id);
    if (rule->signal != want->signal || rule->match != want->match || rule->n_children != want->n) {
        return false;
    }
    const uint32_t *kids = ldb->kids.data;
    return want->n == 0 ||
           memcmp(kids + rule->first_child, want->kids, want->n * sizeof *kids) == 0;
}

static uint64_t rule_hash(const rule_probe *probe) {
    uint64_t hash = odin3_hash_combine(odin3_hash_u64(probe->signal), probe->match);
    hash = odin3_hash_combine(hash, probe->n);
    for (uint32_t k = 0; k < probe->n; k++) {
        hash = odin3_hash_combine(hash, probe->kids[k]);
    }
    return hash;
}

/* The ID of the rule probe describes, made (with the next ID) when it is new. */
static odin3_status rule_intern(odin3_proc_ldb *ldb, const rule_probe *probe, uint32_t *id) {
    uint64_t hash = rule_hash(probe);
    odin3_idcmp cmp = {hash, {probe, sizeof *probe}, rule_equals, ldb};
    if (odin3_idindex_find(ldb->rule_ix, &cmp, id)) {
        return ODIN3_OK;
    }
    uint32_t next = (uint32_t)ldb->rules.len;
    if (next >= (uint32_t)RULE_MASK || ldb->kids.len + probe->n >= UINT32_MAX ||
        odin3_vec_reserve(&ldb->rules, (size_t)next + 1) != ODIN3_OK ||
        odin3_vec_reserve(&ldb->kids, ldb->kids.len + probe->n) != ODIN3_OK ||
        odin3_idindex_insert(ldb->rule_ix, (odin3_identry){hash, next}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_proc_rule *rule = odin3_vec_push(&ldb->rules); /* reserved */
    rule->signal = probe->signal;
    rule->match = probe->match;
    rule->first_child = (uint32_t)ldb->kids.len;
    rule->n_children = probe->n;
    for (uint32_t k = 0; k < probe->n; k++) {
        *(uint32_t *)odin3_vec_push(&ldb->kids) = probe->kids[k]; /* reserved */
    }
    *id = next;
    return ODIN3_OK;
}

/* make_inner(signal, match, child). */
static odin3_status rule_inner(odin3_proc_ldb *ldb, odin3_pw_bit signal, odin3_pw_bit match,
                               uint32_t *child) {
    rule_probe probe = {signal, match, child, 1};
    return rule_intern(ldb, &probe, child);
}

const odin3_proc_rule *odin3_proc_ldb_rule(const odin3_proc_mctx *mctx, uint32_t id) {
    const odin3_proc_ldb *ldb = mctx != NULL ? mctx->ldb : NULL;
    if (ldb == NULL || id < ODIN3_PROC_RULE_FIRST || id >= ldb->rules.len) {
        return NULL;
    }
    return odin3_vec_cat(&ldb->rules, id);
}

uint32_t odin3_proc_ldb_child(const odin3_proc_mctx *mctx, const odin3_proc_rule *rule,
                              uint32_t index) {
    return ((const uint32_t *)mctx->ldb->kids.data)[rule->first_child + index];
}

bool odin3_proc_ldb_latch(const odin3_proc_mctx *mctx, uint32_t index, odin3_node_id *node,
                          uint32_t *rule) {
    const odin3_proc_ldb *ldb = mctx != NULL ? mctx->ldb : NULL;
    if (ldb == NULL || index >= ldb->latches.len) {
        return false;
    }
    const ldb_latch *latch = odin3_vec_cat(&ldb->latches, index);
    *node = latch->node;
    *rule = latch->rule;
    return true;
}

/* --- find_mux_feedback (§8 step 3) --------------------------------------------------------- */

typedef struct lw_walk {
    odin3_proc_mctx *mctx;
    odin3_proc_ldb *ldb;
    odin3_pw_bit out; /* the latch output (needle) */
} lw_walk;

static lw_frame *lw_at(const lw_walk *walk, size_t at) {
    return odin3_vec_at(&walk->ldb->frames, at);
}

/* Pushes the frame expanding the mux bit on Y pin ypin, memo key key (on the stack: rule 0). */
static odin3_status lw_push(const lw_walk *walk, uint64_t key, odin3_pin_id ypin, bool *pushed) {
    odin3_proc_ldb *ldb = walk->ldb;
    const odin3_module *module = walk->mctx->module;
#ifndef NDEBUG
    uint64_t was = 0;
    /* §8: the memo expands each (net, set_undef) at most once per latch output */
    assert(!odin3_u64map_get(ldb->memo, key, &was) || (was >> MEMO_SHIFT) != ldb->out_epoch);
#endif
    if (odin3_u64map_put(ldb->memo, (odin3_kv){key, (uint64_t)ldb->out_epoch << MEMO_SHIFT}) !=
        ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    lw_frame *frame = odin3_vec_push(&ldb->frames);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    frame->key = key;
    frame->node = odin3_pin_node(module, ypin);
    frame->index = odin3_pin_bit(module, ypin);
    frame->width = odin3_node_port(module, frame->node, MUX_A).count;
    frame->n_sel = odin3_node_port(module, frame->node, MUX_S).count;
    frame->found_base = ldb->found.len;
    frame->set_undef = (key & 1U) != 0;
    odin3_proc_hook_expand((odin3_net_id){(uint32_t)(key >> 1)}, frame->set_undef);
    *pushed = true;
    return ODIN3_OK;
}

/*
 * The entry of find_mux_feedback(bit, out, set_undef): set_undef turns false at a net with more
 * than one reader; bit == out is TRUE; a bit no recorded mux drives is FALSE; a memoised bit gives
 * its rule (FALSE while it is on the stack: a combinational cycle, where Yosys's recursion never
 * ends); else a frame is pushed (*pushed).
 */
static odin3_status lw_enter(const lw_walk *walk, odin3_pw_bit bit, bool set_undef, uint32_t *rule,
                             bool *pushed) {
    odin3_proc_ldb *ldb = walk->ldb;
    *pushed = false;
    bool in_db = !is_const(bit) && bit < ldb->net_end;
    if (in_db && ((const uint8_t *)ldb->users.data)[bit] >= USERS_MANY) {
        set_undef = false;
    }
    uint32_t ypin = in_db ? ((const uint32_t *)ldb->drv.data)[bit] : 0;
    *rule = ODIN3_PROC_RULE_FALSE;
    if (bit == walk->out) {
        *rule = ODIN3_PROC_RULE_TRUE;
        return ODIN3_OK;
    }
    if (ypin == 0 || !odin3_pin_live(walk->mctx->module, (odin3_pin_id){ypin})) {
        return ODIN3_OK;
    }
    uint64_t key = (uint64_t)bit << 1 | (set_undef ? 1U : 0U);
    uint64_t value = 0;
    if (odin3_u64map_get(ldb->memo, key, &value) && (value >> MEMO_SHIFT) == ldb->out_epoch) {
        uint32_t found = (uint32_t)(value & RULE_MASK);
        *rule = found != 0 ? found : ODIN3_PROC_RULE_FALSE;
        return ODIN3_OK;
    }
    return lw_push(walk, key, (odin3_pin_id){ypin}, pushed);
}

/* Moves a mux input pin that is on out to the x constant net (Yosys's set_undef rewrite). */
static odin3_status lw_undef(const lw_walk *walk, odin3_pin_id pin) {
    odin3_module *module = walk->mctx->module;
    odin3_net_id xnet = {0};
    odin3_status st = odin3_proc_const_net(walk->mctx, ODIN3_PW_SX, &xnet);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id was = odin3_pin_net(module, pin);
    st = odin3_pin_disconnect(module, pin);
    st = st == ODIN3_OK ? odin3_pin_connect(module, pin, xnet) : st;
    if (st != ODIN3_OK && odin3_net_valid(was) && !odin3_net_valid(odin3_pin_net(module, pin))) {
        (void)odin3_pin_connect(module, pin, was); /* the room the pin just left */
    }
    return st;
}

/* The pin of frame's child at its step: A[index], else the B slice step - 1 at index (none when
 * the mux is too narrow). */
static odin3_pin_id lw_child_pin(const lw_walk *walk, const lw_frame *frame) {
    uint32_t port = frame->step == 0 ? MUX_A : MUX_B;
    uint32_t at = frame->step == 0 ? frame->index : (frame->step - 1) * frame->width + frame->index;
    odin3_pinslice pins = odin3_node_port(walk->mctx->module, frame->node, port);
    odin3_pin_id pin = {0};
    if (at < pins.count) {
        pin.v = pins.first.v + at;
    }
    return pin;
}

/* The select bit `select` of frame's mux. */
static odin3_pw_bit lw_select(const lw_walk *walk, const lw_frame *frame, uint32_t select) {
    odin3_pinslice pins = odin3_node_port(walk->mctx->module, frame->node, MUX_S);
    return pin_bit(walk->mctx, (odin3_pin_id){pins.first.v + select});
}

static odin3_status lw_found(odin3_proc_ldb *ldb, uint32_t rule) {
    uint32_t *slot = odin3_vec_push(&ldb->found);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = rule;
    return ODIN3_OK;
}

/* A child's result for the top frame: unless FALSE, the pin moves to x when it is on out and
 * set_undef holds, and the branch's rule joins the frame's children (the A branch nested in
 * (S[k], 0) for each select bit k, a B slice k in (S[k], 1)). Then the next child. */
static odin3_status lw_deliver(const lw_walk *walk, uint32_t result) {
    size_t top = walk->ldb->frames.len - 1;
    lw_frame frame = *lw_at(walk, top);
    odin3_status st = ODIN3_OK;
    if (result != ODIN3_PROC_RULE_FALSE) {
        if (frame.set_undef && frame.child_bit == walk->out) {
            st = lw_undef(walk, frame.child_pin);
        }
        uint32_t rule = result;
        if (frame.step == 0) {
            for (uint32_t k = 0; st == ODIN3_OK && k < frame.n_sel; k++) {
                st = rule_inner(walk->ldb, lw_select(walk, &frame, k), ODIN3_PW_S0, &rule);
            }
        } else {
            st = rule_inner(walk->ldb, lw_select(walk, &frame, frame.step - 1), ODIN3_PW_S1, &rule);
        }
        st = st == ODIN3_OK ? lw_found(walk->ldb, rule) : st;
    }
    lw_at(walk, top)->step++;
    return st;
}

/* The top frame is done: no children is FALSE, else the "or" rule of its sorted distinct
 * children; memoised, popped, *result set. */
static odin3_status lw_finish(const lw_walk *walk, uint32_t *result) {
    odin3_proc_ldb *ldb = walk->ldb;
    lw_frame frame = *lw_at(walk, ldb->frames.len - 1);
    size_t n_kids = ldb->found.len - frame.found_base;
    uint32_t rule = ODIN3_PROC_RULE_FALSE;
    odin3_status st = ODIN3_OK;
    if (n_kids > 0) {
        uint32_t *kids = (uint32_t *)ldb->found.data + frame.found_base;
        qsort(kids, n_kids, sizeof *kids, cmp_u32);
        size_t unique = 1;
        for (size_t i = 1; i < n_kids; i++) {
            kids[unique] = kids[i];
            unique += kids[i] != kids[unique - 1] ? 1U : 0U;
        }
        rule_probe probe = {ODIN3_PW_S0, ODIN3_PW_S0, kids, (uint32_t)unique};
        st = rule_intern(ldb, &probe, &rule);
    }
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(ldb->memo,
                              (odin3_kv){frame.key, (uint64_t)ldb->out_epoch << MEMO_SHIFT | rule});
    }
    vec_truncate(&ldb->found, frame.found_base);
    odin3_vec_pop(&ldb->frames);
    *result = rule;
    return st;
}

/* One step of the walk: the top frame's next child, or its end. */
static odin3_status lw_step(const lw_walk *walk, uint32_t *result) {
    odin3_proc_ldb *ldb = walk->ldb;
    size_t top = ldb->frames.len - 1;
    lw_frame *frame = lw_at(walk, top);
    if (frame->step > frame->n_sel) {
        uint32_t done = ODIN3_PROC_RULE_FALSE;
        odin3_status st = lw_finish(walk, &done);
        if (st != ODIN3_OK || ldb->frames.len == 0) {
            *result = done;
            return st;
        }
        return lw_deliver(walk, done);
    }
    frame->child_pin = lw_child_pin(walk, frame);
    frame->child_bit =
        frame->child_pin.v != 0 ? pin_bit(walk->mctx, frame->child_pin) : ODIN3_PW_SX;
    odin3_pw_bit bit = frame->child_bit;
    bool set_undef = frame->set_undef;
    uint32_t rule = ODIN3_PROC_RULE_FALSE;
    bool pushed = false;
    odin3_status st = lw_enter(walk, bit, set_undef, &rule, &pushed);
    if (st != ODIN3_OK || pushed) {
        return st;
    }
    return lw_deliver(walk, rule);
}

/* find_mux_feedback(in, out, true) with a fresh memo (one per latch output): *rule is its rule. */
static odin3_status lw_find(odin3_proc_mctx *mctx, odin3_pw_bit in, odin3_pw_bit out,
                            uint32_t *rule) {
    odin3_proc_ldb *ldb = mctx->ldb;
    lw_walk walk = {mctx, ldb, out};
    if (++ldb->out_epoch == 0) { /* the epochs ran out: a fresh memo */
        odin3_u64map_destroy(ldb->memo);
        ldb->memo = odin3_u64map_create(0);
        ldb->out_epoch = 1;
        if (ldb->memo == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_vec_clear(&ldb->frames);
    odin3_vec_clear(&ldb->found);
    bool pushed = false;
    *rule = ODIN3_PROC_RULE_FALSE;
    odin3_status st = lw_enter(&walk, in, true, rule, &pushed);
    while (st == ODIN3_OK && ldb->frames.len > 0) {
        st = lw_step(&walk, rule);
    }
    return st;
}

/* --- cells of the latch step (§8, §14) ----------------------------------------------------- */

/* The process whose latches are being made: its record, attributes and bits. */
typedef struct lt_ctx {
    odin3_proc_mctx *mctx;
    odin3_proc_ldb *ldb;
    odin3_pw *pw;
} lt_ctx;

/* Adds cell with the process's lowering record and attributes; *ybit its first output bit. */
static odin3_status lt_add(const lt_ctx *lc, odin3_proc_cell cell, odin3_pw_bit *ybit) {
    cell.pw = lc->pw;
    cell.attrs_from = lc->pw;
    cell.prov = lc->pw->lowered;
    odin3_pw_sig out = {0, 0};
    odin3_status st = odin3_proc_add_cell(lc->mctx, &cell, &out);
    if (st == ODIN3_OK && ybit != NULL && out.len > 0) {
        *ybit = odin3_pw_sig_bits(lc->pw, out)[0];
    }
    return st;
}

/* $not(a), 1 bit. */
static odin3_status lt_not(const lt_ctx *lc, odin3_pw_bit bit, odin3_pw_bit *ybit) {
    odin3_pw_sig in = {0, 0};
    odin3_status st = odin3_pw_sig_new(lc->pw, &bit, 1, &in);
    odin3_value params[NOT_PARAMS] = {odin3_value_int(0), odin3_value_int(1), odin3_value_int(1)};
    odin3_proc_cell cell = {
        odin3_proc_type(lc->mctx, "$not"), params, NOT_PARAMS, NULL, &in, 1, NULL, NULL, 0, {0}};
    return st == ODIN3_OK ? lt_add(lc, cell, ybit) : st;
}

/* $and(a, b), 1 bit. */
static odin3_status lt_and(const lt_ctx *lc, odin3_pw_bit lhs, odin3_pw_bit rhs,
                           odin3_pw_bit *ybit) {
    odin3_pw_sig ins[2] = {{0, 0}, {0, 0}};
    odin3_status st = odin3_pw_sig_new(lc->pw, &lhs, 1, &ins[0]);
    st = st == ODIN3_OK ? odin3_pw_sig_new(lc->pw, &rhs, 1, &ins[1]) : st;
    odin3_value params[AND_PARAMS] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(1),
                                      odin3_value_int(1), odin3_value_int(1)};
    odin3_proc_cell cell = {
        odin3_proc_type(lc->mctx, "$and"), params, AND_PARAMS, NULL, ins, 2, NULL, NULL, 0, {0}};
    return st == ODIN3_OK ? lt_add(lc, cell, ybit) : st;
}

/* $reduce_or of n bits (A_WIDTH n, even 1). */
static odin3_status lt_or(const lt_ctx *lc, const odin3_pw_bit *bits, uint32_t n,
                          odin3_pw_bit *ybit) {
    odin3_pw_sig in = {0, 0};
    odin3_status st = odin3_pw_sig_new(lc->pw, bits, n, &in);
    odin3_value params[NOT_PARAMS] = {odin3_value_int(0), odin3_value_int(n), odin3_value_int(1)};
    odin3_proc_cell cell = {odin3_proc_type(lc->mctx, "$reduce_or"),
                            params,
                            NOT_PARAMS,
                            NULL,
                            &in,
                            1,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return st == ODIN3_OK ? lt_add(lc, cell, ybit) : st;
}

/* --- hold(n) (§8 step 6), memoised per rule ------------------------------------------------ */

/* The hold of rule when it needs no cell: 1'1 for TRUE, 1'0 for FALSE, or the memoised one. */
static bool hd_known(const odin3_proc_ldb *ldb, uint32_t rule, odin3_pw_bit *bit) {
    uint64_t value = 0;
    if (rule == ODIN3_PROC_RULE_TRUE || rule == ODIN3_PROC_RULE_FALSE) {
        *bit = rule == ODIN3_PROC_RULE_TRUE ? ODIN3_PW_S1 : ODIN3_PW_S0;
        return true;
    }
    if (odin3_u64map_get(ldb->holds, rule, &value)) {
        *bit = (odin3_pw_bit)value;
        return true;
    }
    return false;
}

static odin3_status hd_push(odin3_proc_ldb *ldb, uint32_t rule) {
    hd_frame *frame = odin3_vec_push(&ldb->hframes);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    frame->rule = rule;
    return ODIN3_OK;
}

static odin3_status hd_bit(odin3_proc_ldb *ldb, odin3_pw_bit bit) {
    odin3_pw_bit *slot = odin3_vec_push(&ldb->hbits);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = bit;
    return ODIN3_OK;
}

/* The top rule is done: its children's $reduce_or, then the $and with its term; memoised and
 * handed to its parent (or *out). */
static odin3_status hd_finish(const lt_ctx *lc, odin3_pw_bit *out) {
    odin3_proc_ldb *ldb = lc->ldb;
    hd_frame frame = *(const hd_frame *)odin3_vec_cat(&ldb->hframes, ldb->hframes.len - 1);
    odin3_proc_rule rule = *(const odin3_proc_rule *)odin3_vec_cat(&ldb->rules, frame.rule);
    odin3_pw_bit result = frame.term;
    odin3_status st = ODIN3_OK;
    if (rule.n_children > 0) {
        odin3_pw_bit any = 0;
        st = lt_or(lc, (const odin3_pw_bit *)ldb->hbits.data + frame.bits_base, rule.n_children,
                   &any);
        result = any;
        if (st == ODIN3_OK && frame.term != 0) {
            st = lt_and(lc, frame.term, any, &result);
        }
    }
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(ldb->holds, (odin3_kv){frame.rule, result});
    }
    vec_truncate(&ldb->hbits, frame.bits_base);
    odin3_vec_pop(&ldb->hframes);
    if (st != ODIN3_OK || ldb->hframes.len == 0) {
        *out = result;
        return st;
    }
    ((hd_frame *)odin3_vec_at(&ldb->hframes, ldb->hframes.len - 1))->next_kid++;
    return hd_bit(ldb, result);
}

/* One step of the top rule: its term first (Yosys makes the $not before the children's holds),
 * then each child's hold in order, then its end. */
static odin3_status hd_step(const lt_ctx *lc, odin3_pw_bit *out) {
    odin3_proc_ldb *ldb = lc->ldb;
    hd_frame *frame = odin3_vec_at(&ldb->hframes, ldb->hframes.len - 1);
    odin3_proc_rule rule = *(const odin3_proc_rule *)odin3_vec_cat(&ldb->rules, frame->rule);
    if (!frame->started) {
        frame->started = true;
        frame->bits_base = ldb->hbits.len;
        if (rule.signal == rule.match) {
            return ODIN3_OK;
        }
        if (rule.match == ODIN3_PW_S1) {
            frame->term = rule.signal;
            return ODIN3_OK;
        }
        odin3_pw_bit term = 0;
        odin3_status st = lt_not(lc, rule.signal, &term);
        ((hd_frame *)odin3_vec_at(&ldb->hframes, ldb->hframes.len - 1))->term = term;
        return st;
    }
    if (frame->next_kid >= rule.n_children) {
        return hd_finish(lc, out);
    }
    uint32_t kid = ((const uint32_t *)ldb->kids.data)[rule.first_child + frame->next_kid];
    odin3_pw_bit bit = 0;
    if (!hd_known(ldb, kid, &bit)) {
        return hd_push(ldb, kid);
    }
    frame->next_kid++;
    return hd_bit(ldb, bit);
}

/* hold(rule): the bit that is 1 while the latch holds. */
static odin3_status hd_build(const lt_ctx *lc, uint32_t rule, odin3_pw_bit *out) {
    odin3_proc_ldb *ldb = lc->ldb;
    if (hd_known(ldb, rule, out)) {
        return ODIN3_OK;
    }
    odin3_vec_clear(&ldb->hframes);
    odin3_vec_clear(&ldb->hbits);
    odin3_status st = hd_push(ldb, rule);
    while (st == ODIN3_OK && ldb->hframes.len > 0) {
        st = hd_step(lc, out);
    }
    return st;
}

/* --- one process (§8 steps 1-6, §10) ------------------------------------------------------- */

/* A latch candidate, no-latch connection or latch bit: out <- in, out's key and order, its rule. */
typedef struct lt_pair {
    uint64_t key;
    odin3_pw_bit out, in;
    uint32_t seq, rule;
} lt_pair;

typedef struct lt_proc {
    odin3_vec nolatch; /* lt_pair: connections out <- in, in order */
    odin3_vec cands;   /* lt_pair: latch candidates */
    odin3_vec latch;   /* lt_pair: latch bits with their rules */
    odin3_vec warned;  /* odin3_wire_id: W2 given in this process */
} lt_proc;

static odin3_status lt_pair_push(odin3_vec *vec, lt_pair pair) {
    lt_pair *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = pair;
    return ODIN3_OK;
}

static int cmp_pair(const void *lhs, const void *rhs) {
    const lt_pair *left = lhs;
    const lt_pair *right = rhs;
    if (left->key != right->key) {
        return left->key < right->key ? -1 : 1;
    }
    if (left->seq != right->seq) {
        return left->seq < right->seq ? -1 : 1;
    }
    return 0;
}

/* §8: an ALWAYS output resolving to a constant is a 2E bug (2D never merges one into a constant;
 * Yosys would skip it as a latch bit with no wire). */
static odin3_status lt_const_out(void) {
    odin3_log(ODIN3_LOG_ERROR, "proc: internal: latch candidate on a constant net");
    return ODIN3_ERR_CHECK;
}

/* One ALWAYS sync action (live view): quickcheck false leaves every bit no-latch, true makes each
 * bit a candidate. */
static odin3_status lt_action(const lt_ctx *lc, lt_proc *lp, const odin3_pw_action *act) {
    if (act->lhs.len != act->rhs.len) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: an always action's sides differ in width");
        return ODIN3_ERR_CHECK;
    }
    for (uint32_t k = 0; k < act->lhs.len; k++) {
        if (is_const(odin3_proc_find(lc->mctx, odin3_pw_sig_bits(lc->pw, act->lhs)[k]))) {
            return lt_const_out();
        }
    }
    odin3_proc_qc qc = {lc->pw, act->rhs, act->lhs};
    bool feedback = act->lhs.len > 0 && odin3_proc_quickcheck(lc->mctx, &qc);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < act->lhs.len; k++) {
        lt_pair pair = {0, odin3_proc_find(lc->mctx, odin3_pw_sig_bits(lc->pw, act->lhs)[k]),
                        odin3_proc_find(lc->mctx, odin3_pw_sig_bits(lc->pw, act->rhs)[k]),
                        (uint32_t)lp->cands.len, 0};
        pair.key = odin3_proc_bit_key(lc->mctx->module, pair.out);
        st = lt_pair_push(feedback ? &lp->cands : &lp->nolatch, pair);
    }
    return st;
}

/* Every ALWAYS sync action of the process, whose actions are then cleared (Yosys's
 * sr->actions.clear(): the ff phase never sees them). */
static odin3_status lt_collect(const lt_ctx *lc, lt_proc *lp) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < lc->pw->syncs.len; i++) {
        odin3_pw_sync *sync = odin3_vec_at(&lc->pw->syncs, i);
        if (sync->type != ODIN3_SYNC_ALWAYS) {
            continue;
        }
        for (size_t j = 0; st == ODIN3_OK && j < sync->actions.len; j++) {
            st = lt_action(lc, lp, odin3_vec_cat(&sync->actions, j));
        }
        odin3_vec_clear(&sync->actions);
    }
    return st;
}

/* Sorts the candidates by out's key; a repeated out keeps its last in (Yosys's dict). */
static void lt_sort(lt_proc *lp) {
    size_t count = lp->cands.len;
    if (count == 0) {
        return;
    }
    lt_pair *pairs = lp->cands.data;
    qsort(pairs, count, sizeof *pairs, cmp_pair);
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (i + 1 < count && pairs[i + 1].key == pairs[i].key) {
            continue;
        }
        pairs[kept++] = pairs[i];
    }
    vec_truncate(&lp->cands, kept);
}

/* Each candidate's walk: FALSE makes it a no-latch bit, else a latch bit with its rule. */
static odin3_status lt_walk(const lt_ctx *lc, lt_proc *lp) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < lp->cands.len; i++) {
        lt_pair pair = *(const lt_pair *)odin3_vec_cat(&lp->cands, i);
        st = lw_find(lc->mctx, pair.in, pair.out, &pair.rule);
        if (st == ODIN3_OK) {
            st = lt_pair_push(pair.rule == ODIN3_PROC_RULE_FALSE ? &lp->nolatch : &lp->latch, pair);
        }
    }
    return st;
}

/* W2 for wire, once per process. */
static odin3_status lt_warn_init(const lt_ctx *lc, lt_proc *lp, odin3_wire_id wire) {
    for (size_t i = 0; i < lp->warned.len; i++) {
        if (((const odin3_wire_id *)lp->warned.data)[i].v == wire.v) {
            return ODIN3_OK;
        }
    }
    odin3_wire_id *slot = odin3_vec_push(&lp->warned);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = wire;
    const char *name = odin3_strtab_get(odin3_design_strtab(lc->mctx->design),
                                        odin3_wire_name(lc->mctx->module, wire));
    odin3_proc_warn(lc->mctx, ODIN3_PROC_W2, lc->pw->prov,
                    "initial value of '%s' is ignored: '%s' is assigned in a combinational always "
                    "block",
                    name != NULL ? name : "", name != NULL ? name : "");
    return ODIN3_OK;
}

/* §8 step 5, §10: each no-latch bit is a connection out <- in (merged at M2) and loses its
 * initial value; a defined bit removed gives W2 once per wire. */
static odin3_status lt_nolatch(const lt_ctx *lc, lt_proc *lp) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < lp->nolatch.len; i++) {
        lt_pair pair = *(const lt_pair *)odin3_vec_cat(&lp->nolatch, i);
        odin3_wire_id wire = {0};
        st = odin3_proc_connect(lc->mctx, pair.out, pair.in);
        if (st == ODIN3_OK && !is_const(pair.out)) {
            st = odin3_ffinit_remove(lc->mctx->module, (odin3_net_id){pair.out}, &wire);
        }
        if (st == ODIN3_OK && odin3_wire_valid(wire)) {
            st = lt_warn_init(lc, lp, wire);
        }
    }
    return st;
}

/* The end of the group of latch bits starting at first: the following bits with the same primary
 * wire and rule (a net without a wire is a group of its own). */
static size_t lt_group_end(const lt_ctx *lc, const lt_proc *lp, size_t first) {
    const lt_pair *pairs = lp->latch.data;
    const odin3_module *module = lc->mctx->module;
    odin3_wire_id wire = odin3_net_primary(module, (odin3_net_id){pairs[first].out}).wire;
    size_t end = first + 1;
    while (odin3_wire_valid(wire) && end < lp->latch.len && pairs[end].rule == pairs[first].rule &&
           odin3_net_primary(module, (odin3_net_id){pairs[end].out}).wire.v == wire.v) {
        end++;
    }
    return end;
}

/* The D and Q runs of a group: its in bits and its out bits. */
static odin3_status lt_group_sigs(const lt_ctx *lc, const lt_proc *lp, size_t first, size_t end,
                                  odin3_pw_sig *dq) {
    size_t width = end - first;
    odin3_pw_bit *bits = odin3_util_malloc(sizeof *bits * (2 * width + 1));
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const lt_pair *pairs = lp->latch.data;
    for (size_t i = 0; i < width; i++) {
        bits[i] = pairs[first + i].in;
        bits[width + i] = pairs[first + i].out;
    }
    odin3_status st = odin3_pw_sig_new(lc->pw, bits, (uint32_t)width, &dq[0]);
    st = st == ODIN3_OK ? odin3_pw_sig_new(lc->pw, bits + width, (uint32_t)width, &dq[1]) : st;
    odin3_util_free(bits);
    return st;
}

/* Records the $dlatch just made (the module's newest node: odin3_proc_add_cell makes the shared
 * constant nets it needs before the cell) and gives W1, naming its Q bits qsig. */
static odin3_status lt_latch_made(const lt_ctx *lc, odin3_pw_sig qsig, uint32_t rule) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(lc->pw, qsig);
    ldb_latch *slot = odin3_vec_push(&lc->ldb->latches);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->node = (odin3_node_id){odin3_module_node_end(lc->mctx->module) - 1};
    slot->rule = rule;
    odin3_strbuf name;
    odin3_strbuf_init(&name);
    odin3_status st = odin3_proc_name_bits(lc->mctx, bits, qsig.len, &name);
    if (st == ODIN3_OK) {
        odin3_proc_warn(lc->mctx, ODIN3_PROC_W1, lc->pw->prov,
                        "latch inferred for '%s': the always block does not assign it on every "
                        "path",
                        name.data != NULL ? name.data : "");
    }
    odin3_strbuf_free(&name);
    return st;
}

/* One group: $dlatch(WIDTH, EN_POLARITY 1; EN = $not(hold(rule)), D = in bits, Q = out bits). */
static odin3_status lt_dlatch(const lt_ctx *lc, const lt_proc *lp, size_t first, size_t end) {
    uint32_t rule = ((const lt_pair *)lp->latch.data)[first].rule;
    odin3_pw_bit hold = 0;
    odin3_pw_bit enable = 0;
    odin3_status st = hd_build(lc, rule, &hold);
    st = st == ODIN3_OK ? lt_not(lc, hold, &enable) : st;
    odin3_pw_sig ins[DLATCH_INPUTS] = {{0, 0}, {0, 0}}; /* EN, D */
    odin3_pw_sig dq[2] = {{0, 0}, {0, 0}};
    st = st == ODIN3_OK ? odin3_pw_sig_new(lc->pw, &enable, 1, &ins[0]) : st;
    st = st == ODIN3_OK ? lt_group_sigs(lc, lp, first, end, dq) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    ins[1] = dq[0];
    odin3_value params[DLATCH_PARAMS] = {odin3_value_int((int64_t)(end - first)),
                                         odin3_value_int(1)};
    odin3_proc_cell cell = {odin3_proc_type(lc->mctx, "$dlatch"),
                            params,
                            DLATCH_PARAMS,
                            NULL,
                            ins,
                            DLATCH_INPUTS,
                            &dq[1],
                            NULL,
                            0,
                            {0}};
    st = lt_add(lc, cell, NULL);
    return st == ODIN3_OK ? lt_latch_made(lc, dq[1], rule) : st;
}

/* §8 step 6: the latch bits, in key order, grouped by primary wire and rule. */
static odin3_status lt_latches(const lt_ctx *lc, const lt_proc *lp) {
    odin3_status st = ODIN3_OK;
    for (size_t first = 0; st == ODIN3_OK && first < lp->latch.len;) {
        size_t end = lt_group_end(lc, lp, first);
        st = lt_dlatch(lc, lp, first, end);
        first = end;
    }
    return st;
}

/* True when the process has an ALWAYS sync with an action. */
static bool lt_has_always(const odin3_pw *pw) {
    for (size_t i = 0; i < pw->syncs.len; i++) {
        const odin3_pw_sync *sync = odin3_vec_cat(&pw->syncs, i);
        if (sync->type == ODIN3_SYNC_ALWAYS && sync->actions.len > 0) {
            return true;
        }
    }
    return false;
}

/* proc_dlatch for one process (§8): the budget at its boundary, its ALWAYS actions, the walks, the
 * no-latch connections (their W2 first, as Yosys logs them first), then the latches. */
static odin3_status lt_process(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (!lt_has_always(pw)) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_proc_budget(mctx, pw->prov);
    lt_ctx lc = {mctx, mctx->ldb, pw};
    lt_proc lp;
    odin3_vec_init(&lp.nolatch, sizeof(lt_pair));
    odin3_vec_init(&lp.cands, sizeof(lt_pair));
    odin3_vec_init(&lp.latch, sizeof(lt_pair));
    odin3_vec_init(&lp.warned, sizeof(odin3_wire_id));
    st = st == ODIN3_OK ? lt_collect(&lc, &lp) : st;
    if (st == ODIN3_OK) {
        lt_sort(&lp);
        st = lt_walk(&lc, &lp);
    }
    st = st == ODIN3_OK ? lt_nolatch(&lc, &lp) : st;
    st = st == ODIN3_OK ? lt_latches(&lc, &lp) : st;
    odin3_vec_free(&lp.nolatch);
    odin3_vec_free(&lp.cands);
    odin3_vec_free(&lp.latch);
    odin3_vec_free(&lp.warned);
    return st;
}

/* --- the fix-up (§8 fixup_muxes) ----------------------------------------------------------- */

/* Yosys's "internal" combinational cell types that the registry has (§8). */
static const char *const k_internal[] = {
    "$not",        "$neg",       "$and",        "$or",          "$xor",         "$xnor",
    "$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor", "$reduce_bool", "$logic_not",
    "$logic_and",  "$logic_or",  "$shl",        "$shr",         "$sshr",        "$shiftx",
    "$lt",         "$le",        "$eq",         "$ne",          "$ge",          "$gt",
    "$add",        "$sub",       "$mul",        "$div",         "$mod",         "$pow",
    "$mux",        "$pmux",      "$tribuf",     "$sop"};

typedef struct fx_ctx {
    odin3_proc_mctx *mctx;
    odin3_vec queue;    /* odin3_node_id: breadth-first */
    odin3_vec seen;     /* uint8_t per node ID */
    odin3_vec inputs;   /* odin3_net_id: the input nets of the node being visited */
    odin3_vec internal; /* uint8_t per cell type ID: one of k_internal */
    odin3_vec nets;     /* odin3_net_id: a mux's old and new pin nets */
    odin3_vec attrs;    /* odin3_pw_attr: a re-created mux's attributes */
    odin3_celltype_id t_mux, t_pmux;
} fx_ctx;

/* The internal types' bitmap, indexed by cell type ID. */
static odin3_status fx_types(fx_ctx *fx) {
    const odin3_design *design = fx->mctx->design;
    for (size_t i = 0; i < sizeof k_internal / sizeof k_internal[0]; i++) {
        uint32_t str = 0;
        odin3_celltype_id id = {0};
        if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(k_internal[i]),
                               &str) ||
            !odin3_celltype_find(design, str, &id)) {
            continue;
        }
        if (id.v >= fx->internal.len &&
            vec_zeros(&fx->internal, (size_t)id.v + 1 - fx->internal.len) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        ((uint8_t *)fx->internal.data)[id.v] = 1;
    }
    return ODIN3_OK;
}

/* Grows the seen marks to cover node IDs below end. */
static odin3_status fx_seen_grow(fx_ctx *fx, uint32_t end) {
    return fx->seen.len >= end ? ODIN3_OK : vec_zeros(&fx->seen, end - fx->seen.len);
}

/* Marks node seen and queues it. */
static odin3_status fx_enqueue(fx_ctx *fx, odin3_node_id node) {
    odin3_status st = fx_seen_grow(fx, node.v + 1);
    odin3_node_id *slot = st == ODIN3_OK ? odin3_vec_push(&fx->queue) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ((uint8_t *)fx->seen.data)[node.v] = 1;
    *slot = node;
    return ODIN3_OK;
}

static bool fx_is_seen(const fx_ctx *fx, odin3_node_id node) {
    return node.v < fx->seen.len && ((const uint8_t *)fx->seen.data)[node.v] != 0;
}

/* The nets on the reading pins of node, in pin order. */
static odin3_status fx_inputs(fx_ctx *fx, odin3_node_id node) {
    const odin3_module *module = fx->mctx->module;
    odin3_pinslice pins = odin3_node_pins(module, node);
    odin3_vec_clear(&fx->inputs);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_net_id net = odin3_pin_net(module, pin);
        if (!odin3_pin_reads(module, pin) || !odin3_net_valid(net)) {
            continue;
        }
        odin3_net_id *slot = odin3_vec_push(&fx->inputs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = net;
    }
    return ODIN3_OK;
}

/* Queues the unseen internal-type drivers of net. */
static odin3_status fx_upstream(fx_ctx *fx, odin3_net_id net) {
    const odin3_module *module = fx->mctx->module;
    uint32_t n_drivers = odin3_net_driver_count(module, net);
    odin3_pinlist pins = odin3_net_pins(module, net); /* drivers first */
    for (uint32_t k = 0; k < n_drivers && k < pins.count; k++) {
        odin3_node_id driver = odin3_pin_node(module, pins.pins[k]);
        uint32_t type = odin3_node_type(module, driver).v;
        if (fx_is_seen(fx, driver) || type >= fx->internal.len ||
            ((const uint8_t *)fx->internal.data)[type] == 0) {
            continue;
        }
        odin3_status st = fx_enqueue(fx, driver);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A mux's pin nets and their rewrite: old = A | B | S | Y, then new A | B | S. */
typedef struct fx_mux {
    uint32_t width, n_sel, n_new;
    odin3_net_id *old_a, *old_b, *old_s, *old_y;
    odin3_net_id *new_a, *new_b, *new_s;
} fx_mux;

static bool fx_undef(const odin3_module *module, const odin3_net_id *nets, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        odin3_const value = odin3_net_const_value(module, nets[k]);
        if (value != ODIN3_CONST_X && value != ODIN3_CONST_Z) {
            return false;
        }
    }
    return true;
}

/* Reads node's pin nets into fx->nets and lays out view over them. */
static odin3_status fx_read(fx_ctx *fx, odin3_node_id node, fx_mux *view) {
    const odin3_module *module = fx->mctx->module;
    view->width = odin3_node_port(module, node, MUX_A).count;
    view->n_sel = odin3_node_port(module, node, MUX_S).count;
    uint32_t width = view->width;
    size_t old_n = (size_t)width * (view->n_sel + 2) + view->n_sel;
    size_t new_n = (size_t)width * (view->n_sel + 2) + view->n_sel + 1;
    odin3_vec_clear(&fx->nets);
    odin3_status st = vec_zeros(&fx->nets, old_n + new_n);
    if (st != ODIN3_OK || view->width == 0) {
        return st;
    }
    odin3_net_id *nets = fx->nets.data;
    for (uint32_t port = MUX_A, at = 0; port <= MUX_Y; port++) {
        odin3_pinslice pins = odin3_node_port(module, node, port);
        for (uint32_t k = 0; k < pins.count && at < old_n; k++) {
            nets[at++] = odin3_pin_net(module, (odin3_pin_id){pins.first.v + k});
        }
    }
    view->old_a = nets;
    view->old_b = nets + width;
    view->old_s = view->old_b + (size_t)width * view->n_sel;
    view->old_y = view->old_s + view->n_sel;
    view->new_a = view->old_y + width;
    view->new_b = view->new_a + width;
    view->new_s = view->new_b + (size_t)width * (view->n_sel + 1);
    return ODIN3_OK;
}

/* fixup_mux: B slices all x/z dropped with their selects (none left: B = A, S = 1'0); A all x/z
 * with a valid slice becomes the last valid slice. *changed when anything differs. */
static odin3_status fx_rewrite(fx_ctx *fx, fx_mux *view, odin3_celltype_id type, bool *changed) {
    const odin3_module *module = fx->mctx->module;
    size_t width = view->width;
    const odin3_net_id *last_valid = NULL;
    view->n_new = 0;
    for (uint32_t i = 0; i < view->n_sel; i++) {
        const odin3_net_id *slice = view->old_b + i * width;
        if (!fx_undef(module, slice, view->width)) {
            last_valid = slice;
            memcpy(view->new_b + view->n_new * width, slice, width * sizeof *slice);
            view->new_s[view->n_new++] = view->old_s[i];
        }
    }
    odin3_status st = ODIN3_OK;
    if (view->n_new == 0) {
        memcpy(view->new_b, view->old_a, width * sizeof *view->old_a);
        st = odin3_proc_const_net(fx->mctx, ODIN3_PW_S0, &view->new_s[0]);
        view->n_new = 1;
    }
    bool a_undef = fx_undef(module, view->old_a, view->width);
    memcpy(view->new_a, a_undef && last_valid != NULL ? last_valid : view->old_a,
           width * sizeof *view->old_a);
    odin3_celltype_id new_type = view->n_new == 1 ? fx->t_mux : fx->t_pmux;
    *changed = new_type.v != type.v || view->n_new != view->n_sel ||
               memcmp(view->new_a, view->old_a, width * sizeof *view->old_a) != 0 ||
               memcmp(view->new_b, view->old_b, width * view->n_sel * sizeof *view->old_b) != 0 ||
               memcmp(view->new_s, view->old_s, view->n_sel * sizeof *view->old_s) != 0;
    return st;
}

static odin3_status fx_attr_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    odin3_pw_attr *slot = odin3_vec_push(ctx);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->key = key_str;
    slot->value = value;
    return ODIN3_OK;
}

/* Deletes node and creates the rewritten mux on the same Y nets, with node's record and
 * attributes (§14; not counted in the statistics again); the new node is marked seen. */
static odin3_status fx_recreate(fx_ctx *fx, odin3_node_id node, const fx_mux *view) {
    odin3_module *module = fx->mctx->module;
    odin3_prov_id prov = odin3_node_prov(module, node);
    odin3_vec_clear(&fx->attrs);
    odin3_status st = odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                         fx_attr_visit, &fx->attrs);
    st = st == ODIN3_OK ? fx_seen_grow(fx, odin3_module_node_end(module) + 1) : st;
    st = st == ODIN3_OK ? odin3_node_delete(module, node) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[PMUX_PARAMS] = {odin3_value_int(view->width), odin3_value_int(view->n_new)};
    odin3_node_spec spec = {view->n_new == 1 ? fx->t_mux : fx->t_pmux, 0, prov, params,
                            view->n_new == 1 ? 1U : PMUX_PARAMS};
    odin3_netvec ports[MUX_PORTS] = {{view->new_a, view->width},
                                     {view->new_b, view->width * view->n_new},
                                     {view->new_s, view->n_new},
                                     {view->old_y, view->width}};
    odin3_node_id made = {0};
    st = odin3_node_create_connected(module, &spec, ports, &made);
    for (size_t i = 0; st == ODIN3_OK && i < fx->attrs.len; i++) {
        const odin3_pw_attr *attr = odin3_vec_cat(&fx->attrs, i);
        st = odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, made.v}, attr->key, attr->value);
    }
    if (odin3_node_valid(made) && made.v < fx->seen.len) {
        ((uint8_t *)fx->seen.data)[made.v] = 1;
    }
    return st;
}

/* fixup_mux on a visited $mux/$pmux: re-created when the rewrite changes it. */
static odin3_status fx_mux_fix(fx_ctx *fx, odin3_node_id node, odin3_celltype_id type) {
    fx_mux view;
    memset(&view, 0, sizeof view);
    bool changed = false;
    odin3_status st = fx_read(fx, node, &view);
    if (st != ODIN3_OK || view.width == 0) {
        return st;
    }
    st = fx_rewrite(fx, &view, type, &changed);
    return st == ODIN3_OK && changed ? fx_recreate(fx, node, &view) : st;
}

/* One node of the breadth-first walk: a mux is fixed, then the drivers of its (old) inputs are
 * queued. */
static odin3_status fx_visit(fx_ctx *fx, odin3_node_id node) {
    const odin3_module *module = fx->mctx->module;
    if (!odin3_node_live(module, node)) {
        return ODIN3_OK;
    }
    odin3_celltype_id type = odin3_node_type(module, node);
    odin3_status st = fx_inputs(fx, node);
    if (st == ODIN3_OK && (type.v == fx->t_mux.v || type.v == fx->t_pmux.v)) {
        st = fx_mux_fix(fx, node, type);
    }
    for (size_t i = 0; st == ODIN3_OK && i < fx->inputs.len; i++) {
        st = fx_upstream(fx, ((const odin3_net_id *)fx->inputs.data)[i]);
    }
    return st;
}

/* fixup_muxes: breadth-first upstream from every $dlatch made, in creation order. */
static odin3_status fx_run(odin3_proc_mctx *mctx) {
    const odin3_proc_ldb *ldb = mctx->ldb;
    if (ldb == NULL || ldb->latches.len == 0) {
        return ODIN3_OK;
    }
    fx_ctx fx;
    memset(&fx, 0, sizeof fx);
    fx.mctx = mctx;
    fx.t_mux = ldb->t_mux;
    fx.t_pmux = ldb->t_pmux;
    odin3_vec_init(&fx.queue, sizeof(odin3_node_id));
    odin3_vec_init(&fx.seen, sizeof(uint8_t));
    odin3_vec_init(&fx.inputs, sizeof(odin3_net_id));
    odin3_vec_init(&fx.internal, sizeof(uint8_t));
    odin3_vec_init(&fx.nets, sizeof(odin3_net_id));
    odin3_vec_init(&fx.attrs, sizeof(odin3_pw_attr));
    odin3_status st = fx_types(&fx);
    for (size_t i = 0; st == ODIN3_OK && i < ldb->latches.len; i++) {
        st = fx_enqueue(&fx, ((const ldb_latch *)odin3_vec_cat(&ldb->latches, i))->node);
    }
    for (size_t head = 0; st == ODIN3_OK && head < fx.queue.len; head++) {
        st = fx_visit(&fx, ((const odin3_node_id *)fx.queue.data)[head]);
    }
    odin3_vec *vecs[] = {&fx.queue, &fx.seen, &fx.inputs, &fx.internal, &fx.nets, &fx.attrs};
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        odin3_vec_free(vecs[i]);
    }
    return st;
}

/* --- the phase ----------------------------------------------------------------------------- */

odin3_status odin3_proc_latches(odin3_proc_mctx *mctx) {
    if (mctx == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: latches: no module context");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool any = false;
    for (size_t i = 0; !any && i < mctx->pws.len; i++) {
        any = lt_has_always(odin3_vec_cat(&mctx->pws, i));
    }
    if (!any) {
        return ODIN3_OK; /* no ALWAYS action: no candidate, no latch, nothing to merge */
    }
    odin3_status st = ODIN3_OK;
    if (mctx->ldb == NULL || !mctx->ldb->built) {
        st = odin3_proc_ldb_build(mctx);
    }
    for (size_t i = 0; st == ODIN3_OK && i < mctx->pws.len; i++) {
        st = lt_process(mctx, odin3_vec_at(&mctx->pws, i));
    }
    st = st == ODIN3_OK ? fx_run(mctx) : st;
    st = st == ODIN3_OK ? odin3_proc_apply_merges(mctx) : st;
    if (mctx->ldb != NULL) {
        ldb_release(mctx->ldb);
    }
    return st;
}
