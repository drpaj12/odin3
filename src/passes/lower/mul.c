/*
 * mul.c — the `maccmap` rule of `lower` (3B spec §5.5, LW-6; plan Task 5): the bit heap, the full
 * adder and $mul. alumacc.cc turns a lone $mul into a $macc with one product term and maccmap.cc
 * (lines 25 to 262) expands it; this file follows that order:
 *
 *   1. the partial products (`add(a, b, ...)`, lines 49 to 70): A and B are swapped so that B is
 *      the shorter, A is extended to W = Y_WIDTH, B is cut to W when wider; for each bit i of B
 *      the row (A << i) & {W{B[i]}} is one $_AND_ per column i + k < W (the AND cells of Yosys
 *      whose A input is a literal zero fold to 0 under `opt -fast`, so they are not built); in
 *      the signed case the top row of B has negative weight: ~(A << i) & B[i] plus B[i] itself at
 *      column i (the `+1` of the two's complement);
 *   2. the heap (`bits`, `add(bit, position)`, lines 33 to 42): per column the set of nets, a net
 *      added twice cancels and carries into the next column. std::set orders by wire creation, so
 *      a column here is sorted by net ID (operand nets before the AND outputs, row by row);
 *   3. `synth()` (lines 128 to 255): summands are taken one per round, the first net of each
 *      non-empty column; between rounds a few bits of the deepest low columns are packed into the
 *      "tree" (they become carry-ins later); the summands are then reduced three at a time with
 *      full adders (`fulladd`, lines 72 to 112: the range of columns where any of the three has
 *      data), an incomplete triple carried, until two are left; each full adder's carry word is
 *      shifted by one place, a packed bit entering at column 0;
 *   4. the two summands go to `lw_alu(CI = 0 or the last packed bit, BI = 0)`.
 *
 * Constants are left in for `opt -fast` (spec §2.2). Any literal x in A or B makes all of Y x
 * (maccmap.cc lines 273 to 278).
 */
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Port positions: A B Y. */
enum { PORT_A = 0, PORT_B = 1, PORT_Y = 2 };

/* maccmap.cc `synth()`: columns up to this one may be packed into the adder tree. */
enum { PACK_MAX_POS = 4 };

/* Gates of a full adder (techmap.v _90_fa): two XOR, two AND, one OR. */
enum { FA_GATES = 5 };

/* --- the full adder ------------------------------------------------------------------------- */

odin3_status lw_fa(lw_ctx *lw, const odin3_net_id in[3], odin3_net_id out[2]) {
    odin3_net_id t1 = {0};
    odin3_net_id t2 = {0};
    odin3_net_id t3 = {0};
    odin3_status st = lw_xor(lw, in[0], in[1], &t1);
    st = st == ODIN3_OK ? lw_and(lw, in[0], in[1], &t2) : st;
    st = st == ODIN3_OK ? lw_and(lw, in[2], t1, &t3) : st;
    st = st == ODIN3_OK ? lw_xor(lw, t1, in[2], &out[0]) : st;
    return st == ODIN3_OK ? lw_or(lw, t2, t3, &out[1]) : st;
}

/* --- the bit heap --------------------------------------------------------------------------- */

void lw_heap_init(lw_heap *heap) {
    odin3_vec_init(&heap->cols, sizeof(lw_column));
    heap->zero = (odin3_net_id){0};
}

void lw_heap_free(lw_heap *heap) {
    for (size_t pos = 0; pos < heap->cols.len; pos++) {
        lw_column *col = odin3_vec_at(&heap->cols, pos);
        lw_bits_free(&col->nets);
    }
    odin3_vec_free(&heap->cols);
}

odin3_status lw_heap_reset(lw_heap *heap, uint32_t width, odin3_net_id zero) {
    lw_heap_free(heap);
    lw_heap_init(heap);
    heap->zero = zero;
    odin3_status st = odin3_vec_reserve(&heap->cols, width);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < width; pos++) {
        lw_column *col = odin3_vec_push(&heap->cols);
        if (col == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            lw_bits_init(&col->nets);
            col->head = 0;
        }
    }
    return st;
}

uint32_t lw_heap_width(const lw_heap *heap) {
    return (uint32_t)heap->cols.len;
}

static lw_column *column_at(lw_heap *heap, uint32_t pos) {
    return odin3_vec_at(&heap->cols, pos);
}

uint32_t lw_heap_depth(const lw_heap *heap, uint32_t pos) {
    const lw_column *col = odin3_vec_cat(&heap->cols, pos);
    return lw_bits_len(&col->nets) - col->head;
}

odin3_net_id lw_heap_pop(lw_heap *heap, uint32_t pos) {
    lw_column *col = column_at(heap, pos);
    return lw_bits_at(&col->nets, col->head++);
}

/* The first index in [head, len) whose net is >= `net` (len when none). */
static uint32_t column_find(const lw_column *col, odin3_net_id net) {
    uint32_t low = col->head;
    uint32_t high = lw_bits_len(&col->nets);
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (lw_bits_at(&col->nets, mid).v < net.v) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

static void column_erase(lw_column *col, uint32_t at) {
    odin3_net_id *nets = col->nets.v.data;
    uint32_t len = lw_bits_len(&col->nets);
    memmove(&nets[at], &nets[at + 1], (size_t)(len - at - 1) * sizeof nets[0]);
    odin3_vec_pop(&col->nets.v);
}

static odin3_status column_insert(lw_column *col, uint32_t at, odin3_net_id net) {
    odin3_status st = lw_bits_push(&col->nets, net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id *nets = col->nets.v.data;
    uint32_t len = lw_bits_len(&col->nets);
    memmove(&nets[at + 1], &nets[at], (size_t)(len - at - 1) * sizeof nets[0]);
    nets[at] = net;
    return ODIN3_OK;
}

odin3_status lw_heap_add(lw_heap *heap, odin3_net_id net, uint32_t pos) {
    uint32_t width = lw_heap_width(heap);
    while (pos < width && net.v != heap->zero.v) {
        lw_column *col = column_at(heap, pos);
        uint32_t at = column_find(col, net);
        if (at < lw_bits_len(&col->nets) && lw_bits_at(&col->nets, at).v == net.v) {
            column_erase(col, at);
            pos++;
        } else {
            return column_insert(col, at, net);
        }
    }
    return ODIN3_OK;
}

/* --- the partial products ------------------------------------------------------------------- */

/* The scratch of one $mul. */
typedef struct mul_work {
    lw_heap heap;
    lw_bits rows;  /* the summands: `count` rows of `width` nets, row after row */
    lw_bits spare; /* the rows of the next reduction round */
    lw_bits tree;  /* the packed bits, popped from the back */
    lw_bits ext_a; /* A extended to the width */
    lw_bits ext_b; /* B cut to the width, when wider */
    lw_bits flip;  /* ~A for the signed top row */
    odin3_net_id zero;
    uint32_t width;
    uint32_t count;
} mul_work;

static void work_init(mul_work *work) {
    lw_heap_init(&work->heap);
    lw_bits_init(&work->rows);
    lw_bits_init(&work->spare);
    lw_bits_init(&work->tree);
    lw_bits_init(&work->ext_a);
    lw_bits_init(&work->ext_b);
    lw_bits_init(&work->flip);
    work->zero = (odin3_net_id){0};
    work->width = 0;
    work->count = 0;
}

static void work_free(mul_work *work) {
    lw_heap_free(&work->heap);
    lw_bits_free(&work->rows);
    lw_bits_free(&work->spare);
    lw_bits_free(&work->tree);
    lw_bits_free(&work->ext_a);
    lw_bits_free(&work->ext_b);
    lw_bits_free(&work->flip);
}

static bool net_is_zero(const lw_ctx *lw, odin3_net_id net) {
    return odin3_net_const_value(lw->module, net) == ODIN3_CONST_0;
}

/* Row i of the products: A[k] & B[i] into column i + k for k < W - i (`src` is A or ~A). */
static odin3_status add_row(lw_ctx *lw, mul_work *work, const lw_bits *src, odin3_net_id bit,
                            uint32_t row) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < work->width - row; k++) {
        odin3_net_id product = {0};
        st = lw_and(lw, lw_bits_at(src, k), bit, &product);
        st = st == ODIN3_OK ? lw_heap_add(&work->heap, product, row + k) : st;
    }
    return st;
}

/* The signed top row of B: ~(A << i) & B[i] over the columns >= i, then B[i] itself at column i. */
static odin3_status add_sign_row(lw_ctx *lw, mul_work *work, odin3_net_id bit, uint32_t row) {
    lw_bits_clear(&work->flip);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < work->width - row; k++) {
        odin3_net_id flipped = {0};
        st = lw_not(lw, lw_bits_at(&work->ext_a, k), &flipped);
        st = st == ODIN3_OK ? lw_bits_push(&work->flip, flipped) : st;
    }
    st = st == ODIN3_OK ? add_row(lw, work, &work->flip, bit, row) : st;
    if (st == ODIN3_OK && !net_is_zero(lw, bit)) {
        st = lw_heap_add(&work->heap, bit, row);
    }
    return st;
}

/* `add(a, b, is_signed, false)`: fills the heap with the products of A and B (either order). */
static odin3_status add_products(lw_ctx *lw, mul_work *work, lw_span lhs, lw_span rhs,
                                 bool is_signed) {
    if (lhs.n < rhs.n) {
        lw_span swap = lhs;
        lhs = rhs;
        rhs = swap;
    }
    odin3_status st = lw_extend(lw, lhs, work->width, is_signed, &work->ext_a);
    if (st == ODIN3_OK && rhs.n > work->width) {
        st = lw_extend(lw, rhs, work->width, is_signed, &work->ext_b);
        rhs = lw_bits_span(&work->ext_b);
    }
    for (uint32_t row = 0; st == ODIN3_OK && row < rhs.n; row++) {
        if (is_signed && row + 1 == rhs.n) {
            st = add_sign_row(lw, work, rhs.nets[row], row);
        } else {
            st = add_row(lw, work, &work->ext_a, rhs.nets[row], row);
        }
    }
    return st;
}

/* --- synth(): the summands and the adder tree packing --------------------------------------- */

static bool heap_has_data(const lw_heap *heap) {
    for (uint32_t pos = 0; pos < lw_heap_width(heap); pos++) {
        if (lw_heap_depth(heap, pos) > 0) {
            return true;
        }
    }
    return false;
}

/* Appends one summand: the first net of every non-empty column, the zero net elsewhere. */
static odin3_status take_row(mul_work *work) {
    odin3_status st = ODIN3_OK;
    size_t base = work->rows.v.len;
    for (uint32_t pos = 0; st == ODIN3_OK && pos < work->width; pos++) {
        st = lw_bits_push(&work->rows, work->zero);
    }
    for (uint32_t pos = 0; st == ODIN3_OK && pos < work->width; pos++) {
        if (lw_heap_depth(&work->heap, pos) > 0) {
            lw_bits_set(&work->rows, (uint32_t)base + pos, lw_heap_pop(&work->heap, pos));
        }
    }
    work->count++;
    return st;
}

/* The deepest column: its depth and position (the last column holding the maximum depth). */
typedef struct heap_peak {
    uint32_t depth;
    uint32_t position;
} heap_peak;

static heap_peak deepest(const lw_heap *heap) {
    heap_peak peak = {0, 0};
    for (uint32_t pos = 0; pos < lw_heap_width(heap); pos++) {
        if (peak.depth <= lw_heap_depth(heap, pos)) {
            peak.depth = lw_heap_depth(heap, pos);
            peak.position = pos;
        }
    }
    return peak;
}

/* One packing step; *packed says whether a word of bits moved into the tree. */
static odin3_status pack_step(mul_work *work, bool *packed) {
    *packed = false;
    heap_peak peak = deepest(&work->heap);
    uint32_t depth = peak.depth;
    uint32_t position = peak.position;
    if (depth == 0 || position > PACK_MAX_POS) {
        return ODIN3_OK;
    }
    uint32_t free_slots = work->count - 1 - lw_bits_len(&work->tree);
    uint32_t required = 0;
    for (uint32_t pos = 0; pos <= position; pos++) {
        required += lw_heap_depth(&work->heap, pos) == depth ? (uint32_t)1 << pos : 0;
    }
    if (required > free_slots) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t pos = 0; st == ODIN3_OK && pos <= position; pos++) {
        if (lw_heap_depth(&work->heap, pos) != depth) {
            continue;
        }
        odin3_net_id bit = lw_heap_pop(&work->heap, pos);
        for (uint32_t copy = 0; st == ODIN3_OK && copy < ((uint32_t)1 << pos); copy++) {
            st = lw_bits_push(&work->tree, bit);
        }
    }
    *packed = true;
    return st;
}

static odin3_status take_summands(mul_work *work) {
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && heap_has_data(&work->heap)) {
        st = take_row(work);
        bool packed = st == ODIN3_OK;
        while (st == ODIN3_OK && packed) {
            st = pack_step(work, &packed);
        }
    }
    return st;
}

/* --- the reduction -------------------------------------------------------------------------- */

static odin3_net_id row_net(const lw_bits *rows, uint32_t row, uint32_t width, uint32_t pos) {
    return lw_bits_at(rows, row * width + pos);
}

/* `fulladd`: the columns [start, stop) where any of the three rows holds a net but the zero net. */
typedef struct col_range {
    uint32_t start;
    uint32_t stop;
} col_range;

static col_range data_range(const mul_work *work, uint32_t first) {
    col_range range = {work->width, 0};
    for (uint32_t pos = 0; pos < work->width; pos++) {
        for (uint32_t row = 0; row < 3; row++) {
            if (row_net(&work->rows, first + row, work->width, pos).v != work->zero.v) {
                range.start = pos < range.start ? pos : range.start;
                range.stop = pos + 1;
            }
        }
    }
    return range;
}

/* Appends the sum row and the carry row (shifted by one place, `extra` at column 0) of the full
 * adders over rows first, first + 1, first + 2 of `rows`. */
static odin3_status add_triple(lw_ctx *lw, mul_work *work, uint32_t first) {
    odin3_status st = ODIN3_OK;
    size_t base = work->spare.v.len;
    for (uint32_t pos = 0; st == ODIN3_OK && pos < 2 * work->width; pos++) {
        st = lw_bits_push(&work->spare, work->zero);
    }
    col_range range = data_range(work, first);
    for (uint32_t pos = range.start; st == ODIN3_OK && pos < range.stop; pos++) {
        const odin3_net_id in[3] = {row_net(&work->rows, first, work->width, pos),
                                    row_net(&work->rows, first + 1, work->width, pos),
                                    row_net(&work->rows, first + 2, work->width, pos)};
        odin3_net_id out[2] = {{0}, {0}};
        st = lw_fa(lw, in, out);
        if (st != ODIN3_OK) {
            break;
        }
        lw_bits_set(&work->spare, (uint32_t)base + pos, out[0]);
        if (pos + 1 < work->width) {
            lw_bits_set(&work->spare, (uint32_t)base + work->width + pos + 1, out[1]);
        }
    }
    if (st == ODIN3_OK && lw_bits_len(&work->tree) > 0) {
        lw_bits_set(&work->spare, (uint32_t)base + work->width,
                    lw_bits_at(&work->tree, lw_bits_len(&work->tree) - 1));
        odin3_vec_pop(&work->tree.v);
    }
    return st;
}

/* Carries rows [from, count) over unchanged. */
static odin3_status carry_rows(mul_work *work, uint32_t from) {
    odin3_status st = ODIN3_OK;
    for (uint32_t row = from; st == ODIN3_OK && row < work->count; row++) {
        for (uint32_t pos = 0; st == ODIN3_OK && pos < work->width; pos++) {
            st = lw_bits_push(&work->spare, row_net(&work->rows, row, work->width, pos));
        }
    }
    return st;
}

/* One round: summands in groups of three become two rows each; 1 or 2 left over are kept. */
static odin3_status reduce_round(lw_ctx *lw, mul_work *work) {
    lw_bits_clear(&work->spare);
    odin3_status st = ODIN3_OK;
    uint32_t row = 0;
    for (; st == ODIN3_OK && row + 2 < work->count; row += 3) {
        st = add_triple(lw, work, row);
    }
    st = st == ODIN3_OK ? carry_rows(work, row) : st;
    if (st == ODIN3_OK) {
        work->count = (uint32_t)(work->spare.v.len / work->width);
        lw_bits swap = work->rows;
        work->rows = work->spare;
        work->spare = swap;
    }
    return st;
}

static odin3_status finish(lw_ctx *lw, mul_work *work, lw_bits *out) {
    lw_bits_clear(out);
    odin3_status st = ODIN3_OK;
    if (work->count == 2) {
        lw_alu_args args = {{work->rows.v.data, work->width},
                            {(const odin3_net_id *)work->rows.v.data + work->width, work->width},
                            work->zero,
                            false,
                            false,
                            true,
                            work->width};
        if (lw_bits_len(&work->tree) > 0) {
            args.carry_in = lw_bits_at(&work->tree, lw_bits_len(&work->tree) - 1);
            odin3_vec_pop(&work->tree.v);
        }
        lw_alu_out res;
        lw_alu_out_init(&res);
        st = lw_alu(lw, &args, &res);
        for (uint32_t pos = 0; st == ODIN3_OK && pos < work->width; pos++) {
            st = lw_bits_push(out, lw_bits_at(&res.sum, pos));
        }
        lw_alu_out_free(&res);
        return st;
    }
    for (uint32_t pos = 0; st == ODIN3_OK && pos < work->width; pos++) {
        st = lw_bits_push(out, work->count == 0 ? work->zero : lw_bits_at(&work->rows, pos));
    }
    return st;
}

/* Maccmap's literal-x rule: any x bit in A or B makes all of Y x. */
static bool has_x(const lw_ctx *lw, lw_span port) {
    for (uint32_t pos = 0; pos < port.n; pos++) {
        if (odin3_net_const_value(lw->module, port.nets[pos]) == ODIN3_CONST_X) {
            return true;
        }
    }
    return false;
}

static odin3_status all_x(lw_ctx *lw, uint32_t width, lw_bits *out) {
    odin3_net_id unknown = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_X, &unknown);
    for (uint32_t pos = 0; st == ODIN3_OK && pos < width; pos++) {
        st = lw_bits_push(out, unknown);
    }
    return st;
}

odin3_status lw_build_mul(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {
    lw_bits_clear(out);
    uint32_t width = cell->port[PORT_Y].n;
    bool is_signed = lw_both_signed(lw, cell);
    if (has_x(lw, cell->port[PORT_A]) || has_x(lw, cell->port[PORT_B])) {
        return all_x(lw, width, out);
    }
    mul_work work;
    work_init(&work);
    work.width = width;
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &work.zero);
    st = st == ODIN3_OK ? lw_heap_reset(&work.heap, width, work.zero) : st;
    st = st == ODIN3_OK ? add_products(lw, &work, cell->port[PORT_A], cell->port[PORT_B], is_signed)
                        : st;
    st = st == ODIN3_OK ? take_summands(&work) : st;
    while (st == ODIN3_OK && work.count > 2) {
        st = reduce_round(lw, &work);
    }
    st = st == ODIN3_OK ? finish(lw, &work, out) : st;
    work_free(&work);
    return st;
}

/*
 * An upper bound on the gates: the ANDs of the rows (W - i for each of the nb = min(|A|, |B|, W)
 * rows), the NOTs of the signed top row, at most nb + signed - 2 full-adder words of at most W
 * bits, and the final adder.
 */
uint64_t lw_est_mul(const lw_cell *cell) {
    uint64_t width = cell->port[PORT_Y].n;
    uint64_t rows =
        cell->port[PORT_A].n < cell->port[PORT_B].n ? cell->port[PORT_A].n : cell->port[PORT_B].n;
    rows = rows < width ? rows : width;
    bool is_signed = cell->params[LW_P_BIN_ASIGNED].i != 0 && cell->params[LW_P_BIN_BSIGNED].i != 0;
    uint64_t gates = rows * width - rows * (rows - 1) / 2;
    gates = lw_sat_add(gates, is_signed ? width - rows + 1 : 0);
    uint64_t summands = rows + (is_signed ? 1 : 0);
    uint64_t words = summands > 2 ? summands - 2 : 0;
    gates = lw_sat_add(gates, lw_sat_mul(lw_sat_mul(words, width), FA_GATES));
    return lw_sat_add(gates, lw_alu_gates(width, false, true));
}
