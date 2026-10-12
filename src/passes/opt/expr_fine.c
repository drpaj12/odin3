/*
 * expr_fine.c — opt_expr's -fine rules (2F spec §4.5; plan Task 7; Yosys 0.55 opt_expr.cc
 * group_cell_inputs, the `do_fine` block of replace_const_cells, and the $eq/$ne and comparison
 * rules at its end). Every rule is a no-op without -fine (or -full), and is listed in a cell
 * type's rule list at Yosys's place (expr.c).
 *
 * Differences from Yosys, all in spec §17: a fully stripped $add/$sub has no 0-bit port, an
 * operand of the stripped cell that vanishes is a constant 0 (unsigned) or its top bit (signed),
 * the new cells of fine.group come out in net-ID order, and a comparison is signed when both
 * operands are.
 */
#include "passes/opt/expr.h"

#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define PORT_A ODIN3_EXPR_PORT_A
#define PORT_B ODIN3_EXPR_PORT_B

enum {
    KEY_CONSTS = 4, /* sort keys 0..3 are the constants 0 1 x z; a net's key is its ID + 4 */
    KEY_BITS = 32,
    BINARY_PORTS = 3
};

/* --- keys, shared ----------------------------------------------------------------------------- */

/* The sort key of a bit: its constant (0 1 x z, in Yosys's State order) or else its net. */
static uint32_t bit_key(const odin3_expr_ctx *ec, odin3_net_id net) {
    odin3_const value = odin3_expr_cval(ec, net);
    return value != ODIN3_CONST_NONE ? (uint32_t)value - (uint32_t)ODIN3_CONST_0
                                     : net.v + KEY_CONSTS;
}

static bool is_binary(const odin3_expr_ctx *ec) {
    return ec->def->n_ports == BINARY_PORTS;
}

/* The unary or binary cell the visit holds, narrowed or widened on one port: the same type and
 * parameters, A (or B) = repl, its width parameter following. The visit goes on with the rebuilt
 * cell (Yosys does not `goto next_cell` after these). */
static odin3_status rebuild_port(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_expr_port port,
                                 const odin3_expr_sig *repl) {
    odin3_expr_sig sig_a = {NULL, 0};
    odin3_expr_sig sig_b = {NULL, 0};
    odin3_status st = odin3_expr_read(ec, PORT_A, &sig_a);
    st = st == ODIN3_OK && is_binary(ec) ? odin3_expr_read(ec, PORT_B, &sig_b) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_expr_cell spec = {.type = ec->kind, .n_params = ec->def->n_params};
    for (uint32_t i = 0; i < spec.n_params; i++) {
        spec.params[i] = odin3_expr_param(ec, i);
    }
    bool is_a = port.v == PORT_A.v;
    uint32_t width_param = ODIN3_EXPR_P_UN_AW;
    spec.in[0] = is_a ? repl : &sig_a;
    if (is_binary(ec)) {
        spec.in[1] = is_a ? &sig_b : repl;
        width_param = is_a ? ODIN3_EXPR_P_BIN_AW : ODIN3_EXPR_P_BIN_BW;
    }
    spec.params[width_param] = repl->n;
    st = odin3_expr_rebuild(ec, rule, &spec);
    ec->resume = st == ODIN3_OK;
    return st;
}

/* --- fine.neutral_A, fine.neutral_B ----------------------------------------------------------- */

static bool neutral_a_applies(odin3_expr_type kind) {
    return kind == ODIN3_EXPR_T_LOGIC_NOT || kind == ODIN3_EXPR_T_LOGIC_AND ||
           kind == ODIN3_EXPR_T_LOGIC_OR || kind == ODIN3_EXPR_T_REDUCE_OR ||
           kind == ODIN3_EXPR_T_REDUCE_AND || kind == ODIN3_EXPR_T_REDUCE_BOOL;
}

static bool logic_binary(odin3_expr_type kind) {
    return kind == ODIN3_EXPR_T_LOGIC_AND || kind == ODIN3_EXPR_T_LOGIC_OR;
}

/* Drops the bits of `port` that are `neutral`; an empty result is one neutral bit. Rebuilds the
 * cell when the signal got shorter. */
static odin3_status drop_neutral(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_expr_port port,
                                 odin3_bit neutral) {
    odin3_const want = neutral == ODIN3_BIT_1 ? ODIN3_CONST_1 : ODIN3_CONST_0;
    odin3_expr_sig sig;
    odin3_status st = odin3_expr_read(ec, port, &sig);
    odin3_expr_sig kept;
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, sig.n, &kept) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    kept.n = 0;
    for (uint32_t k = 0; k < sig.n; k++) {
        if (odin3_expr_cval(ec, sig.net[k]) != want) {
            kept.net[kept.n++] = sig.net[k];
        }
    }
    if (kept.n == 0) {
        st = odin3_opt_const_net(ec->oc, neutral, &kept.net[0]);
        kept.n = 1;
    }
    return st == ODIN3_OK && kept.n < sig.n ? rebuild_port(ec, rule, port, &kept) : st;
}

odin3_status odin3_expr_rule_fine_neutral_a(odin3_expr_ctx *ec) {
    if (!ec->fine || !neutral_a_applies(ec->kind)) {
        return ODIN3_OK;
    }
    odin3_bit neutral = ec->kind == ODIN3_EXPR_T_REDUCE_AND ? ODIN3_BIT_1 : ODIN3_BIT_0;
    return drop_neutral(ec, ODIN3_OPT_RULE_FINE_NEUTRAL_A, PORT_A, neutral);
}

odin3_status odin3_expr_rule_fine_neutral_b(odin3_expr_ctx *ec) {
    if (!ec->fine || !logic_binary(ec->kind)) {
        return ODIN3_OK;
    }
    return drop_neutral(ec, ODIN3_OPT_RULE_FINE_NEUTRAL_B, PORT_B, ODIN3_BIT_0);
}

/* --- fine.$reduce_and, fine.A, fine.B --------------------------------------------------------- */

/*
 * Yosys's one-bit 4-state reduction of a signal that has a constant bit: the result starts as the
 * neutral bit; an x (exactly x; z counts for nothing) makes it x unless something else has; the
 * dominant bit makes it that and ends the scan; a net makes it unknown, which is NONE here.
 */
static odin3_const reduce_bits(const odin3_expr_ctx *ec, const odin3_expr_sig *sig,
                               odin3_const dominant) {
    odin3_const neutral = dominant == ODIN3_CONST_1 ? ODIN3_CONST_0 : ODIN3_CONST_1;
    odin3_const result = neutral;
    for (uint32_t k = 0; k < sig->n; k++) {
        odin3_const value = odin3_expr_cval(ec, sig->net[k]);
        if (value == ODIN3_CONST_X) {
            result = result == neutral ? ODIN3_CONST_X : result;
        } else if (value == dominant) {
            return dominant;
        } else if (value == ODIN3_CONST_NONE) {
            result = ODIN3_CONST_NONE;
        }
    }
    return result;
}

static odin3_bit bit_of(odin3_const value) {
    switch (value) {
    case ODIN3_CONST_0:
        return ODIN3_BIT_0;
    case ODIN3_CONST_1:
        return ODIN3_BIT_1;
    case ODIN3_CONST_Z:
        return ODIN3_BIT_Z;
    case ODIN3_CONST_X:
    case ODIN3_CONST_NONE:
    default:
        return ODIN3_BIT_X;
    }
}

/* port becomes the one bit its reduction is, unless that is unknown or it already is. */
static odin3_status collapse(odin3_expr_ctx *ec, odin3_opt_rule rule, odin3_expr_port port,
                             odin3_const dominant) {
    odin3_expr_sig sig;
    odin3_status st = odin3_expr_read(ec, port, &sig);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_const value = reduce_bits(ec, &sig, dominant);
    if (value == ODIN3_CONST_NONE || (sig.n == 1 && odin3_expr_cval(ec, sig.net[0]) == value)) {
        return ODIN3_OK;
    }
    odin3_expr_sig one;
    st = odin3_expr_sig_new(ec, 1, &one);
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, bit_of(value), &one.net[0]) : st;
    return st == ODIN3_OK ? rebuild_port(ec, rule, port, &one) : st;
}

odin3_status odin3_expr_rule_fine_reduce_and(odin3_expr_ctx *ec) {
    if (!ec->fine || ec->kind != ODIN3_EXPR_T_REDUCE_AND) {
        return ODIN3_OK;
    }
    return collapse(ec, ODIN3_OPT_RULE_FINE_REDUCE_AND, PORT_A, ODIN3_CONST_0);
}

odin3_status odin3_expr_rule_fine_const_a(odin3_expr_ctx *ec) {
    if (!ec->fine || !neutral_a_applies(ec->kind) || ec->kind == ODIN3_EXPR_T_REDUCE_AND) {
        return ODIN3_OK;
    }
    return collapse(ec, ODIN3_OPT_RULE_FINE_A, PORT_A, ODIN3_CONST_1);
}

odin3_status odin3_expr_rule_fine_const_b(odin3_expr_ctx *ec) {
    if (!ec->fine || !logic_binary(ec->kind)) {
        return ODIN3_OK;
    }
    return collapse(ec, ODIN3_OPT_RULE_FINE_B, PORT_B, ODIN3_CONST_1);
}

/* --- fine ($add, $sub: the low result bits that are copies) ----------------------------------- */

/* The operands and result of an $add / $sub being stripped; src is the net each result bit
 * becomes: the stripped bits' sources, then the new cell's outputs. */
typedef struct strip_job {
    odin3_expr_sig a, b, y, src;
    odin3_net_id zero;
    bool sub;
} strip_job;

/* How many low bits are copies of an operand bit (Yosys's loop), filling src for them: B bit 0;
 * $add: A bit 0; $sub: 1 - 1 = 0. */
static uint32_t strip_count(const odin3_expr_ctx *ec, strip_job *job) {
    uint32_t limit = job->y.n < job->a.n ? job->y.n : job->a.n;
    limit = limit < job->b.n ? limit : job->b.n;
    uint32_t i = 0;
    for (; i < limit; i++) {
        odin3_const bv = odin3_expr_cval(ec, job->b.net[i]);
        odin3_const av = odin3_expr_cval(ec, job->a.net[i]);
        if (bv == ODIN3_CONST_0) {
            job->src.net[i] = job->a.net[i];
        } else if (job->sub && bv == ODIN3_CONST_1 && av == ODIN3_CONST_1) {
            job->src.net[i] = job->zero;
        } else if (!job->sub && av == ODIN3_CONST_0) {
            job->src.net[i] = job->b.net[i];
        } else {
            break;
        }
    }
    return i;
}

/* An operand without its first `skip` bits; when none is left, its last bit if the operands are
 * signed, else a 0 (Yosys: that bit if A_SIGNED, else a 0-bit port). */
static odin3_status operand_rest(odin3_expr_ctx *ec, const strip_job *job,
                                 const odin3_expr_sig *full, uint32_t skip, odin3_expr_sig *out) {
    if (full->n > skip) {
        *out = odin3_expr_slice(full, skip, full->n - skip);
        return ODIN3_OK;
    }
    odin3_status st = odin3_expr_sig_new(ec, 1, out);
    if (st == ODIN3_OK) {
        out->net[0] = odin3_expr_both_signed(ec) ? full->net[full->n - 1] : job->zero;
    }
    return st;
}

/* The $add / $sub of what is left (the visited cell's name and attributes go to it), and Y bits
 * from `skip` on are its outputs. */
static odin3_status strip_rest(odin3_expr_ctx *ec, strip_job *job, uint32_t skip) {
    odin3_expr_sig ra;
    odin3_expr_sig rb;
    odin3_status st = operand_rest(ec, job, &job->a, skip, &ra);
    st = st == ODIN3_OK ? operand_rest(ec, job, &job->b, skip, &rb) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_expr_cell spec = {
        .type = ec->kind, .n_params = ODIN3_EXPR_P_BIN_YW + 1, .in = {&ra, &rb}};
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED);
    spec.params[ODIN3_EXPR_P_BIN_BSIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED);
    spec.params[ODIN3_EXPR_P_BIN_AW] = ra.n;
    spec.params[ODIN3_EXPR_P_BIN_BW] = rb.n;
    spec.params[ODIN3_EXPR_P_BIN_YW] = job->y.n - skip;
    odin3_node_id fresh = {0};
    odin3_expr_sig out;
    st = odin3_expr_new_cell(ec, &spec, &fresh, &out);
    for (uint32_t k = skip; st == ODIN3_OK && k < job->y.n; k++) {
        job->src.net[k] = out.net[k - skip];
    }
    st = st == ODIN3_OK ? odin3_expr_wire_keep(ec, ODIN3_OPT_RULE_FINE_ADDSUB, &job->src, fresh)
                        : st;
    if (st == ODIN3_OK) {
        ec->made = fresh;
        ec->resume = true;
    }
    return st;
}

odin3_status odin3_expr_rule_fine_addsub(odin3_expr_ctx *ec) {
    if (!ec->fine || (ec->kind != ODIN3_EXPR_T_ADD && ec->kind != ODIN3_EXPR_T_SUB)) {
        return ODIN3_OK;
    }
    strip_job job = {.sub = ec->kind == ODIN3_EXPR_T_SUB};
    odin3_status st = odin3_expr_read(ec, PORT_A, &job.a);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &job.b) : st;
    st = st == ODIN3_OK ? odin3_expr_read(ec, odin3_expr_y_port(ec), &job.y) : st;
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, job.y.n, &job.src) : st;
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, ODIN3_BIT_0, &job.zero) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t stripped = strip_count(ec, &job);
    if (stripped == 0) {
        return ODIN3_OK;
    }
    if (stripped == job.y.n) { /* nothing is left to compute: Y is wiring (§17 #18) */
        return odin3_expr_wire(ec, ODIN3_OPT_RULE_FINE_ADDSUB, &job.src);
    }
    return strip_rest(ec, &job, stripped);
}

/* --- fine.group ------------------------------------------------------------------------------- */

/* Yosys's groups; CONST_B stays empty because every call is commutative. */
enum { GRP_DYN, GRP_CONST_A, GRP_CONST_AB, GRP_N };

/* One result bit with its (normalised) operand pair, its group and its place. */
typedef struct gbit {
    uint32_t group, ka, kb, y;
    odin3_net_id a, b;
} gbit;

typedef struct group_job {
    gbit *bits;
    uint32_t n;
    odin3_expr_sig src; /* the net each result bit becomes */
    odin3_net_id zero, one;
} group_job;

static bool group_kind(odin3_expr_type kind) {
    return kind == ODIN3_EXPR_T_NOT || kind == ODIN3_EXPR_T_AND || kind == ODIN3_EXPR_T_OR ||
           kind == ODIN3_EXPR_T_XOR || kind == ODIN3_EXPR_T_XNOR;
}

/* Yosys's per-bit normalisation: an $or bit with a 1 is 1 with 1, an $and bit with a 0 is 0 with
 * 0, and (without -keepdc) equal $xor / $xnor inputs are 0 with 0 / 1 with 1. */
static void normalise(const odin3_expr_ctx *ec, const group_job *job, gbit *bit) {
    odin3_const ca = odin3_expr_cval(ec, bit->a);
    odin3_const cb = odin3_expr_cval(ec, bit->b);
    bool equal = odin3_expr_same(ec, bit->a, bit->b);
    switch (ec->kind) {
    case ODIN3_EXPR_T_OR:
        if (ca == ODIN3_CONST_1 || cb == ODIN3_CONST_1) {
            bit->a = bit->b = job->one;
        }
        break;
    case ODIN3_EXPR_T_AND:
        if (ca == ODIN3_CONST_0 || cb == ODIN3_CONST_0) {
            bit->a = bit->b = job->zero;
        }
        break;
    case ODIN3_EXPR_T_XOR:
        if (!ec->keepdc && equal) {
            bit->a = bit->b = job->zero;
        }
        break;
    case ODIN3_EXPR_T_XNOR:
        if (!ec->keepdc && equal) {
            bit->a = bit->b = job->one;
        }
        break;
    default:
        break;
    }
}

/* The group of a normalised bit; a constant A is made of the pair by swapping (commutative). */
static uint32_t classify(const odin3_expr_ctx *ec, gbit *bit) {
    bool undef = odin3_expr_is_undef(ec, bit->a) || odin3_expr_is_undef(ec, bit->b);
    bool const_a = odin3_expr_cval(ec, bit->a) != ODIN3_CONST_NONE;
    bool const_b = odin3_expr_cval(ec, bit->b) != ODIN3_CONST_NONE;
    if (undef && ec->keepdc) {
        return GRP_DYN;
    }
    if (const_a && const_b) {
        return GRP_CONST_AB;
    }
    if (const_b) {
        odin3_net_id tmp = bit->a;
        bit->a = bit->b;
        bit->b = tmp;
    }
    return const_a || const_b ? GRP_CONST_A : GRP_DYN;
}

static int cmp_gbit(const void *lhs, const void *rhs) {
    const gbit *left = lhs;
    const gbit *right = rhs;
    const uint32_t left_key[] = {left->group, left->ka, left->kb, left->y};
    const uint32_t right_key[] = {right->group, right->ka, right->kb, right->y};
    for (size_t i = 0; i < sizeof left_key / sizeof left_key[0]; i++) {
        if (left_key[i] != right_key[i]) {
            return left_key[i] < right_key[i] ? -1 : 1;
        }
    }
    return 0;
}

/* Reads the operands, extended to Y's width (Yosys extend_u0; $not takes B = A), into job->bits,
 * normalised, classified and sorted by group, pair and result bit. */
static odin3_status group_load(odin3_expr_ctx *ec, group_job *job) {
    odin3_expr_sig raw_a = {NULL, 0};
    odin3_expr_sig raw_b = {NULL, 0};
    odin3_expr_sig ext_a = {NULL, 0};
    odin3_expr_sig ext_b = {NULL, 0};
    job->n = odin3_expr_y_width(ec);
    bool is_not = ec->kind == ODIN3_EXPR_T_NOT;
    bool a_signed = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED) != 0; /* parameter 0 of both */
    bool b_signed = is_not ? a_signed : odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED) != 0;
    odin3_status st = odin3_expr_read(ec, PORT_A, &raw_a);
    st = st == ODIN3_OK ? odin3_expr_read(ec, is_not ? PORT_A : PORT_B, &raw_b) : st;
    st = st == ODIN3_OK ? odin3_expr_extend(ec, &raw_a, job->n, a_signed, &ext_a) : st;
    st = st == ODIN3_OK ? odin3_expr_extend(ec, &raw_b, job->n, b_signed, &ext_b) : st;
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, ODIN3_BIT_0, &job->zero) : st;
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, ODIN3_BIT_1, &job->one) : st;
    job->bits = st == ODIN3_OK ? odin3_expr_alloc(ec, sizeof *job->bits * job->n) : NULL;
    st = job->bits != NULL || st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
    for (uint32_t pos = 0; st == ODIN3_OK && pos < job->n; pos++) {
        gbit *bit = &job->bits[pos];
        bit->a = ext_a.net[pos];
        bit->b = ext_b.net[pos];
        bit->y = pos;
        normalise(ec, job, bit);
        bit->group = classify(ec, bit);
        bit->ka = bit_key(ec, bit->a);
        bit->kb = bit_key(ec, bit->b);
    }
    if (st == ODIN3_OK) {
        qsort(job->bits, job->n, sizeof *job->bits, cmp_gbit);
    }
    return st;
}

static bool same_pair(const gbit *lhs, const gbit *rhs) {
    return lhs->group == rhs->group && lhs->ka == rhs->ka && lhs->kb == rhs->kb;
}

/* The range [lo, hi) of sorted bits in `group`, and how many different pairs it holds. */
typedef struct group_range {
    uint32_t lo, hi, pairs;
} group_range;

static group_range range_of(const group_job *job, uint32_t group) {
    group_range range = {job->n, job->n, 0};
    for (uint32_t i = 0; i < job->n; i++) {
        if (job->bits[i].group != group) {
            continue;
        }
        range.lo = range.lo == job->n ? i : range.lo;
        range.hi = i + 1;
        range.pairs += i == range.lo || !same_pair(&job->bits[i - 1], &job->bits[i]) ? 1U : 0U;
    }
    return range;
}

/* Yosys: nothing happens when one group already holds every result bit as a different pair. */
static bool group_nothing(const group_job *job) {
    for (uint32_t group = 0; group < GRP_N; group++) {
        if (range_of(job, group).pairs == job->n) {
            return true;
        }
    }
    return false;
}

/* The different operand pairs of a group: A and B of each. */
typedef struct pair_sigs {
    odin3_expr_sig a, b;
} pair_sigs;

/* The net the constant-A $and / $or pair `index` gives: B, or (without -keepdc) the dominant
 * constant for an undefined A. */
static odin3_net_id andor_result(const odin3_expr_ctx *ec, const group_job *job,
                                 const pair_sigs *pairs, uint32_t index) {
    if (ec->keepdc || !odin3_expr_is_undef(ec, pairs->a.net[index])) {
        return pairs->b.net[index];
    }
    return ec->kind == ODIN3_EXPR_T_AND ? job->zero : job->one;
}

/* The net the constant-A $xor / $xnor pair `index` gives: the constant for an undefined A (without
 * -keepdc), else B or a new $not of it. */
static odin3_status xor_result(odin3_expr_ctx *ec, const group_job *job, const pair_sigs *pairs,
                               uint32_t index, odin3_net_id *out) {
    bool is_xor = ec->kind == ODIN3_EXPR_T_XOR;
    odin3_net_id const_bit = pairs->a.net[index];
    odin3_net_id other = pairs->b.net[index];
    if (odin3_expr_is_undef(ec, const_bit)) {
        *out = is_xor ? job->zero : job->one;
        return ODIN3_OK;
    }
    bool invert = (odin3_expr_cval(ec, const_bit) == ODIN3_CONST_1) == is_xor;
    if (!invert) {
        *out = other;
        return ODIN3_OK;
    }
    odin3_expr_sig in = {NULL, 0};
    odin3_expr_sig made = {NULL, 0};
    odin3_status st = odin3_expr_one(ec, other, &in);
    st = st == ODIN3_OK ? odin3_expr_new_un(ec, ODIN3_EXPR_T_NOT, &in, 1, &made) : st;
    *out = st == ODIN3_OK ? made.net[0] : (odin3_net_id){0};
    return st;
}

/* The nets of a group's different pairs: wiring for a constant A of $and / $or / $xor / $xnor,
 * else one new unsigned cell of the visited type. */
static odin3_status group_results(odin3_expr_ctx *ec, const group_job *job, uint32_t group,
                                  const pair_sigs *pairs, odin3_expr_sig *res) {
    odin3_status st = odin3_expr_sig_new(ec, pairs->a.n, res);
    bool const_a = group == GRP_CONST_A && ec->kind != ODIN3_EXPR_T_NOT;
    for (uint32_t j = 0; st == ODIN3_OK && const_a && j < pairs->a.n; j++) {
        if (ec->kind == ODIN3_EXPR_T_AND || ec->kind == ODIN3_EXPR_T_OR) {
            res->net[j] = andor_result(ec, job, pairs, j);
        } else {
            st = xor_result(ec, job, pairs, j, &res->net[j]);
        }
    }
    if (st != ODIN3_OK || const_a) {
        return st;
    }
    if (ec->kind == ODIN3_EXPR_T_NOT) {
        return odin3_expr_new_un(ec, ec->kind, &pairs->a, pairs->a.n, res);
    }
    return odin3_expr_new_bin(ec, ec->kind, &pairs->a, &pairs->b, res);
}

/* One group: its different pairs, the nets they give, and src for each result bit of the group. */
static odin3_status group_emit(odin3_expr_ctx *ec, group_job *job, uint32_t group) {
    group_range range = range_of(job, group);
    if (range.pairs == 0) {
        return ODIN3_OK;
    }
    pair_sigs pairs = {{NULL, 0}, {NULL, 0}};
    odin3_expr_sig res = {NULL, 0};
    odin3_status st = odin3_expr_sig_new(ec, range.pairs, &pairs.a);
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, range.pairs, &pairs.b) : st;
    uint32_t pair = 0;
    for (uint32_t i = range.lo; st == ODIN3_OK && i < range.hi; i++) {
        if (i == range.lo || !same_pair(&job->bits[i - 1], &job->bits[i])) {
            pairs.a.net[pair] = job->bits[i].a;
            pairs.b.net[pair++] = job->bits[i].b;
        }
    }
    st = st == ODIN3_OK ? group_results(ec, job, group, &pairs, &res) : st;
    pair = 0;
    for (uint32_t i = range.lo; st == ODIN3_OK && i < range.hi; i++) {
        pair += i != range.lo && !same_pair(&job->bits[i - 1], &job->bits[i]) ? 1U : 0U;
        job->src.net[job->bits[i].y] = res.net[pair];
    }
    return st;
}

/* Yosys group_cell_inputs: the result bits of $not / $and / $or / $xor / $xnor are split by what
 * their operand pair is, equal pairs share a bit, and each group gets its own unsigned cell (a
 * constant A is wiring, or $not cells). The visited cell goes. */
odin3_status odin3_expr_rule_fine_group(odin3_expr_ctx *ec) {
    if (!ec->fine || !group_kind(ec->kind)) {
        return ODIN3_OK;
    }
    group_job job = {0};
    odin3_status st = group_load(ec, &job);
    if (st != ODIN3_OK || group_nothing(&job)) {
        return st;
    }
    st = odin3_expr_sig_new(ec, job.n, &job.src);
    for (uint32_t group = 0; st == ODIN3_OK && group < GRP_N; group++) {
        st = group_emit(ec, &job, group);
    }
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_FINE_GROUP, &job.src) : st;
}

/* --- fine ($eq, $ne: redundant bit pairs, contradictions) ------------------------------------- */

/* Yosys's mfp over bits, keyed by sort key: parent links in a map; keys not in it are their own
 * roots. */
static odin3_status uf_root(odin3_u64map *uf, uint64_t key, uint64_t *root) {
    uint64_t cur = key;
    uint64_t parent = 0;
    for (;;) {
        if (!odin3_u64map_get(uf, cur, &parent)) {
            odin3_status st = odin3_u64map_put(uf, (odin3_kv){cur, cur});
            if (st != ODIN3_OK) {
                return st;
            }
            parent = cur;
        }
        if (parent == cur) {
            break;
        }
        cur = parent;
    }
    *root = cur;
    for (uint64_t walk = key; walk != cur;) { /* path compression: puts to a key already there */
        uint64_t next = 0;
        (void)odin3_u64map_get(uf, walk, &next);
        (void)odin3_u64map_put(uf, (odin3_kv){walk, cur});
        walk = next;
    }
    return ODIN3_OK;
}

static odin3_status uf_merge(odin3_u64map *uf, uint64_t lhs, uint64_t rhs) {
    uint64_t root_l = 0;
    uint64_t root_r = 0;
    odin3_status st = uf_root(uf, lhs, &root_l);
    st = st == ODIN3_OK ? uf_root(uf, rhs, &root_r) : st;
    if (st == ODIN3_OK && root_l != root_r) {
        st = odin3_u64map_put(uf, (odin3_kv){root_l, root_r});
    }
    return st;
}

/* What the scan of an $eq / $ne found. */
typedef struct eqne_scan {
    odin3_expr_sig a, b;
    uint8_t *drop_a, *drop_b; /* per bit: a redundant pair's bits */
    uint32_t dropped;
    bool contradiction;
} eqne_scan;

/* The key of a bit of sig, 0 (a 0) past its end (Yosys pads the shorter side with 0). */
static uint32_t padded_key(const odin3_expr_ctx *ec, const odin3_expr_sig *sig, uint32_t bit) {
    return bit < sig->n ? bit_key(ec, sig->net[bit]) : 0;
}

static bool padded_undef(const odin3_expr_ctx *ec, const odin3_expr_sig *sig, uint32_t bit) {
    return bit < sig->n && odin3_expr_is_undef(ec, sig->net[bit]);
}

/* The maps of a scan: the union-find, and the pairs seen. */
typedef struct eqne_maps {
    odin3_u64map *uf;
    odin3_u64map *seen;
} eqne_maps;

/* One position, from the top: a defined pair merges in the union-find, and *redundant tells whether
 * the pair (either way round) was seen before. */
static odin3_status eqne_step(const odin3_expr_ctx *ec, const eqne_scan *scan,
                              const eqne_maps *maps, uint32_t bit, bool *redundant) {
    uint32_t key_a = padded_key(ec, &scan->a, bit);
    uint32_t key_b = padded_key(ec, &scan->b, bit);
    odin3_status st = ODIN3_OK;
    if (!padded_undef(ec, &scan->a, bit) && !padded_undef(ec, &scan->b, bit)) {
        st = uf_merge(maps->uf, key_a, key_b);
    }
    uint32_t low = key_a < key_b ? key_a : key_b;
    uint32_t high = key_a < key_b ? key_b : key_a;
    uint64_t pair = ((uint64_t)low << KEY_BITS) | high;
    *redundant = st == ODIN3_OK && odin3_u64map_get(maps->seen, pair, NULL);
    if (st == ODIN3_OK && !*redundant) {
        st = odin3_u64map_put(maps->seen, (odin3_kv){pair, 1});
    }
    return st;
}

/* The bits of a redundant pair go (a side shorter than the other has none at the top). */
static void eqne_mark(eqne_scan *scan, uint32_t bit) {
    if (bit < scan->a.n) {
        scan->drop_a[bit] = 1;
    }
    if (bit < scan->b.n) {
        scan->drop_b[bit] = 1;
    }
    scan->dropped++;
}

/* Yosys's loop, from the top bit down: the bits of a redundant pair are marked to go. */
static odin3_status eqne_walk(const odin3_expr_ctx *ec, eqne_scan *scan, const eqne_maps *maps) {
    uint32_t width = scan->a.n > scan->b.n ? scan->a.n : scan->b.n;
    if (odin3_expr_both_signed(ec)) {
        width = scan->a.n < scan->b.n ? scan->a.n : scan->b.n;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = width; st == ODIN3_OK && bit-- > 0;) {
        bool redundant = false;
        st = eqne_step(ec, scan, maps, bit, &redundant);
        if (st != ODIN3_OK || !redundant) {
            continue;
        }
        eqne_mark(scan, bit);
    }
    return st;
}

/* Whether the walk joined 0 and 1 (the cell can never be equal). */
static odin3_status eqne_contradiction(odin3_u64map *uf, bool *out) {
    uint64_t root_0 = 0;
    uint64_t root_1 = 0;
    odin3_status st = uf_root(uf, 0, &root_0);
    st = st == ODIN3_OK ? uf_root(uf, 1, &root_1) : st;
    *out = st == ODIN3_OK && root_0 == root_1;
    return st;
}

static odin3_status eqne_scan_run(odin3_expr_ctx *ec, eqne_scan *scan) {
    odin3_status st = odin3_expr_read(ec, PORT_A, &scan->a);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &scan->b) : st;
    scan->drop_a = st == ODIN3_OK ? odin3_expr_alloc(ec, (size_t)scan->a.n + 1) : NULL;
    scan->drop_b = st == ODIN3_OK ? odin3_expr_alloc(ec, (size_t)scan->b.n + 1) : NULL;
    eqne_maps maps = {odin3_u64map_create(0), odin3_u64map_create(0)};
    if (st == ODIN3_OK &&
        (scan->drop_a == NULL || scan->drop_b == NULL || maps.uf == NULL || maps.seen == NULL)) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    st = st == ODIN3_OK ? eqne_walk(ec, scan, &maps) : st;
    st = st == ODIN3_OK ? eqne_contradiction(maps.uf, &scan->contradiction) : st;
    odin3_u64map_destroy(maps.uf);
    odin3_u64map_destroy(maps.seen);
    return st;
}

/* A side with no bit left is 0 (Yosys leaves a 0-bit port, which eqneq.cmpzero then reads in the
 * next round): the cell is $logic_not ($eq) or $reduce_bool ($ne) of the side that stays. */
static odin3_status eqne_against_zero(odin3_expr_ctx *ec, const odin3_expr_sig *other,
                                      bool other_is_a) {
    odin3_expr_cell spec = {.type = ec->kind == ODIN3_EXPR_T_EQ ? ODIN3_EXPR_T_LOGIC_NOT
                                                                : ODIN3_EXPR_T_REDUCE_BOOL,
                            .n_params = ODIN3_EXPR_P_UN_YW + 1,
                            .in = {other}};
    spec.params[ODIN3_EXPR_P_UN_SIGNED] =
        odin3_expr_param(ec, other_is_a ? ODIN3_EXPR_P_BIN_ASIGNED : ODIN3_EXPR_P_BIN_BSIGNED);
    spec.params[ODIN3_EXPR_P_UN_AW] = other->n;
    spec.params[ODIN3_EXPR_P_UN_YW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_YW);
    return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_FINE_EQNE, &spec);
}

/* The cell on the bits that stay. */
static odin3_status eqne_resize(odin3_expr_ctx *ec, const eqne_scan *scan) {
    odin3_expr_sig keep_a = {NULL, 0};
    odin3_expr_sig keep_b = {NULL, 0};
    odin3_status st = odin3_expr_sig_new(ec, scan->a.n, &keep_a);
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, scan->b.n, &keep_b) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    keep_a.n = keep_b.n = 0;
    for (uint32_t i = 0; i < scan->a.n; i++) {
        keep_a.net[keep_a.n] = scan->a.net[i];
        keep_a.n += scan->drop_a[i] == 0 ? 1U : 0U;
    }
    for (uint32_t i = 0; i < scan->b.n; i++) {
        keep_b.net[keep_b.n] = scan->b.net[i];
        keep_b.n += scan->drop_b[i] == 0 ? 1U : 0U;
    }
    if (keep_a.n == 0 || keep_b.n == 0) { /* the first pair is never dropped: one side stays */
        return eqne_against_zero(ec, keep_a.n == 0 ? &keep_b : &keep_a, keep_a.n != 0);
    }
    odin3_expr_cell spec = {
        .type = ec->kind, .n_params = ODIN3_EXPR_P_BIN_YW + 1, .in = {&keep_a, &keep_b}};
    for (uint32_t i = 0; i < spec.n_params; i++) {
        spec.params[i] = odin3_expr_param(ec, i);
    }
    spec.params[ODIN3_EXPR_P_BIN_AW] = keep_a.n;
    spec.params[ODIN3_EXPR_P_BIN_BW] = keep_b.n;
    return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_FINE_EQNE, &spec);
}

odin3_status odin3_expr_rule_fine_eqne(odin3_expr_ctx *ec) {
    if (!ec->fine || (ec->kind != ODIN3_EXPR_T_EQ && ec->kind != ODIN3_EXPR_T_NE)) {
        return ODIN3_OK;
    }
    eqne_scan scan = {0};
    odin3_status st = eqne_scan_run(ec, &scan);
    if (st != ODIN3_OK) {
        return st;
    }
    if (scan.contradiction) {
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_FINE_EQNE,
                                   ec->kind == ODIN3_EXPR_T_EQ ? ODIN3_BIT_0 : ODIN3_BIT_1);
    }
    return scan.dropped > 0 ? eqne_resize(ec, &scan) : ODIN3_OK;
}

/* --- fine ($lt $le $gt $ge against a constant) ------------------------------------------------ */

typedef enum cmp_act { ACT_NONE, ACT_CONST, ACT_NET, ACT_LOGIC_NOT, ACT_REDUCE_OR } cmp_act;

/* What a comparison X <op> K becomes: a constant, a net, or a cell on X[from .. from + n). */
typedef struct cmp_plan {
    cmp_act act;
    odin3_bit bit;
    odin3_net_id net;
    uint32_t from, n;
} cmp_plan;

/* The comparison as "variable <cmp> constant": the variable side, the constant side, and the
 * operator (swapped with the sides: > is <, >= is <=). */
typedef struct cmp_view {
    odin3_expr_type op;
    odin3_expr_sig var, cst;
    bool swapped;
} cmp_view;

static odin3_expr_type swapped_op(odin3_expr_type op) {
    switch (op) {
    case ODIN3_EXPR_T_GT:
        return ODIN3_EXPR_T_LT;
    case ODIN3_EXPR_T_LT:
        return ODIN3_EXPR_T_GT;
    case ODIN3_EXPR_T_GE:
        return ODIN3_EXPR_T_LE;
    default:
        return ODIN3_EXPR_T_GE;
    }
}

static odin3_status cmp_load(odin3_expr_ctx *ec, cmp_view *view) {
    odin3_status st = odin3_expr_read(ec, PORT_A, &view->var);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &view->cst) : st;
    view->op = ec->kind;
    view->swapped = false;
    if (st == ODIN3_OK && !odin3_expr_fully_const(odin3_expr_sig_census(ec, &view->cst), false)) {
        odin3_expr_sig tmp = view->var;
        view->var = view->cst;
        view->cst = tmp;
        view->op = swapped_op(view->op);
        view->swapped = true;
    }
    return st;
}

/* Yosys get_highest_hot_index of a constant: the top 1 bit, scanning down through 0s; -1 when the
 * top non-0 bit is not a 1. */
static int64_t highest_hot(const odin3_expr_ctx *ec, const odin3_expr_sig *cst) {
    for (uint32_t k = cst->n; k-- > 0;) {
        odin3_const value = odin3_expr_cval(ec, cst->net[k]);
        if (value != ODIN3_CONST_0) {
            return value == ODIN3_CONST_1 ? (int64_t)k : -1;
        }
    }
    return -1;
}

static void plan_const(cmp_plan *plan, odin3_bit bit) {
    plan->act = ACT_CONST;
    plan->bit = bit;
}

/* Unsigned X < 0 and X >= 0, and X > ~0 and X <= ~0 for a constant as wide as X. */
static void plan_unsigned_edges(const cmp_view *view, odin3_expr_census census, cmp_plan *plan) {
    bool zero = odin3_expr_fully_zero(census);
    bool ones = census.one == census.n && view->cst.n == view->var.n;
    bool never = (zero && view->op == ODIN3_EXPR_T_LT) || (ones && view->op == ODIN3_EXPR_T_GT);
    bool always = (zero && view->op == ODIN3_EXPR_T_GE) || (ones && view->op == ODIN3_EXPR_T_LE);
    if (never) {
        plan_const(plan, ODIN3_BIT_0);
    } else if (always) {
        plan_const(plan, ODIN3_BIT_1);
    }
}

/* A one-hot constant 2^k below X's width (X < 2^k: the bits above k are all 0; X >= 2^k: some is
 * 1), and a constant above X's range (a < or <= is always true, a > or >= never). */
static void plan_unsigned_hot(const odin3_expr_ctx *ec, const cmp_view *view,
                              odin3_expr_census census, cmp_plan *plan) {
    odin3_expr_type op = view->op;
    int64_t hot = highest_hot(ec, &view->cst);
    if (census.one == 1 && hot >= 0 && hot < (int64_t)view->var.n &&
        (op == ODIN3_EXPR_T_LT || op == ODIN3_EXPR_T_GE)) {
        plan->act = op == ODIN3_EXPR_T_LT ? ACT_LOGIC_NOT : ACT_REDUCE_OR;
        plan->from = (uint32_t)hot;
        plan->n = view->var.n - (uint32_t)hot;
    }
    if (hot >= (int64_t)view->var.n) {
        bool low = op == ODIN3_EXPR_T_LT || op == ODIN3_EXPR_T_LE;
        plan_const(plan, low ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
}

/* Yosys's unsigned rules, each overriding the one before. */
static void plan_unsigned(const odin3_expr_ctx *ec, const cmp_view *view, cmp_plan *plan) {
    odin3_expr_census census = odin3_expr_sig_census(ec, &view->cst);
    plan_unsigned_edges(view, census, plan);
    plan_unsigned_hot(ec, view, census, plan);
}

/* Yosys's signed rules: X < 0 is the sign bit, X >= 0 is its inverse. */
static void plan_signed(const odin3_expr_ctx *ec, const cmp_view *view, cmp_plan *plan) {
    if (!odin3_expr_fully_zero(odin3_expr_sig_census(ec, &view->cst))) {
        return;
    }
    if (view->op == ODIN3_EXPR_T_LT) {
        plan->act = ACT_NET;
        plan->net = view->var.net[view->var.n - 1];
    } else if (view->op == ODIN3_EXPR_T_GE) {
        plan->act = ACT_LOGIC_NOT;
        plan->from = view->var.n - 1;
        plan->n = 1;
    }
}

/* The cell $logic_not / $reduce_or of X[from .. from + n), taking the comparison's Y. */
static odin3_status cmp_cell(odin3_expr_ctx *ec, const cmp_view *view, const cmp_plan *plan) {
    odin3_expr_sig part = odin3_expr_slice(&view->var, plan->from, plan->n);
    odin3_expr_cell spec = {.type = plan->act == ACT_LOGIC_NOT ? ODIN3_EXPR_T_LOGIC_NOT
                                                               : ODIN3_EXPR_T_REDUCE_OR,
                            .n_params = ODIN3_EXPR_P_UN_YW + 1,
                            .in = {&part}};
    spec.params[ODIN3_EXPR_P_UN_AW] = plan->n;
    spec.params[ODIN3_EXPR_P_UN_YW] = odin3_expr_y_width(ec);
    return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_FINE_CMP, &spec);
}

odin3_status odin3_expr_rule_fine_cmp(odin3_expr_ctx *ec) {
    if (!ec->fine || (ec->kind != ODIN3_EXPR_T_LT && ec->kind != ODIN3_EXPR_T_LE &&
                      ec->kind != ODIN3_EXPR_T_GT && ec->kind != ODIN3_EXPR_T_GE)) {
        return ODIN3_OK;
    }
    cmp_view view;
    odin3_status st = cmp_load(ec, &view);
    if (st != ODIN3_OK || !odin3_expr_fully_def(odin3_expr_sig_census(ec, &view.cst))) {
        return st;
    }
    cmp_plan plan = {.act = ACT_NONE};
    if (odin3_expr_both_signed(ec)) {
        plan_signed(ec, &view, &plan);
    } else {
        plan_unsigned(ec, &view, &plan);
    }
    switch (plan.act) {
    case ACT_CONST:
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_FINE_CMP, plan.bit);
    case ACT_NET:
        return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_FINE_CMP, plan.net);
    case ACT_LOGIC_NOT:
    case ACT_REDUCE_OR:
        return cmp_cell(ec, &view, &plan);
    case ACT_NONE:
    default:
        return ODIN3_OK;
    }
}
