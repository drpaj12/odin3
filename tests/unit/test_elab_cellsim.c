/*
 * test_elab_cellsim.c — 2D plan Task 28 (review M18): the constant evaluator and the cell
 * simulate hooks agree. For every word cell that has both a simulate hook and an odin3_cop
 * mapping, CASES random 0/1 operand pairs at the widths 1, 7, 64 and 65 (operand and result
 * widths drawn independently) and random signs: odin3_cval_op and the hook give the same Y, bit
 * for bit. The cells are $add $sub $mul $and $or $xor $not $eq $ne $lt $le $gt $ge $reduce_and
 * $reduce_or $reduce_xor, and Task 1's $neg $xnor $reduce_xnor $reduce_bool $logic_not
 * $logic_and $logic_or. $mux, $pmux, flip-flops and memories are out of scope.
 */
#include "frontends/elab/cval.h"
#include "ir/celltype.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "sim/cell.h"
#include "sim/prng.h"
#include "unity.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CASES = 1000, N_WIDTHS = 4, MAXW = 65, MAX_BITS = 3 * MAXW, MAX_PARAMS = 5 };

static const uint32_t k_widths[N_WIDTHS] = {1, 7, 64, 65};

typedef struct cell_map {
    const char *cell;
    odin3_cop op;
} cell_map;

static const cell_map k_map[] = {
    {"$add", ODIN3_COP_ADD},
    {"$sub", ODIN3_COP_SUB},
    {"$mul", ODIN3_COP_MUL},
    {"$and", ODIN3_COP_AND},
    {"$or", ODIN3_COP_OR},
    {"$xor", ODIN3_COP_XOR},
    {"$xnor", ODIN3_COP_XNOR},
    {"$not", ODIN3_COP_NOT},
    {"$neg", ODIN3_COP_NEG},
    {"$eq", ODIN3_COP_EQ},
    {"$ne", ODIN3_COP_NE},
    {"$lt", ODIN3_COP_LT},
    {"$le", ODIN3_COP_LE},
    {"$gt", ODIN3_COP_GT},
    {"$ge", ODIN3_COP_GE},
    {"$reduce_and", ODIN3_COP_RAND},
    {"$reduce_or", ODIN3_COP_ROR},
    {"$reduce_xor", ODIN3_COP_RXOR},
    {"$reduce_xnor", ODIN3_COP_RXNOR},
    {"$reduce_bool", ODIN3_COP_ROR},
    {"$logic_not", ODIN3_COP_LNOT},
    {"$logic_and", ODIN3_COP_LAND},
    {"$logic_or", ODIN3_COP_LOR},
};
#define N_MAP (sizeof k_map / sizeof k_map[0])

static odin3_arena *arena;
static odin3_prng prng;

void setUp(void) {
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    odin3_prng_seed(&prng, 28);
}

void tearDown(void) {
    odin3_arena_destroy(arena);
    arena = NULL;
}

static const odin3_celltype_def *def_named(const char *name) {
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        if (strcmp(odin3_builtin_celltypes[i]->name, name) == 0) {
            return odin3_builtin_celltypes[i];
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

/* One case: the widths and signs of A, B (binary cells) and Y, and the operand bits. */
typedef struct rcase {
    uint32_t wa, wb, wy;
    bool sa, sb;
    uint8_t abits[MAXW], bbits[MAXW];
} rcase;

static uint32_t pick_width(void) {
    return k_widths[odin3_prng_next(&prng) % N_WIDTHS];
}

static rcase draw_case(void) {
    rcase rc;
    memset(&rc, 0, sizeof rc);
    rc.wa = pick_width();
    rc.wb = pick_width();
    rc.wy = pick_width();
    rc.sa = odin3_prng_bit(&prng);
    rc.sb = odin3_prng_bit(&prng);
    for (uint32_t k = 0; k < MAXW; k++) {
        rc.abits[k] = odin3_prng_bit(&prng) ? 1 : 0;
        rc.bbits[k] = odin3_prng_bit(&prng) ? 1 : 0;
    }
    return rc;
}

/* The parameter value of the named parameter of def, set in params. */
static void set_param(const odin3_celltype_def *def, odin3_value *params, const char *name,
                      int64_t value) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            params[i].i = value;
            return;
        }
    }
    TEST_FAIL_MESSAGE(name);
}

/* The cell's Y as MSB-first bytes, through its simulate hook. */
typedef struct hook_run {
    uint8_t values[MAX_BITS];
    uint32_t idx[MAX_BITS];
    odin3_sim_span spans[3];
    odin3_value params[MAX_PARAMS];
} hook_run;

/* The cell's parameter values for the case: the defaults, then the signs and widths. */
static void set_params(const odin3_celltype_def *def, const rcase *rc, hook_run *hr) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_PARAMS, def->n_params);
    for (uint32_t i = 0; i < def->n_params; i++) {
        hr->params[i] = def->params[i].dflt;
    }
    set_param(def, hr->params, "A_SIGNED", rc->sa);
    set_param(def, hr->params, "A_WIDTH", rc->wa);
    set_param(def, hr->params, "Y_WIDTH", rc->wy);
    if (def->n_ports == 3) {
        set_param(def, hr->params, "B_SIGNED", rc->sb);
        set_param(def, hr->params, "B_WIDTH", rc->wb);
    }
}

/* One port's pin slots and starting values (the input bits; zeros for Y). */
static void lay_out_port(hook_run *hr, uint32_t *next, uint32_t port, const uint8_t *bits,
                         uint32_t width) {
    hr->spans[port] = (odin3_sim_span){&hr->idx[*next], width};
    for (uint32_t k = 0; k < width; k++) {
        hr->idx[*next] = *next;
        hr->values[*next] = bits != NULL ? bits[k] : 0;
        (*next)++;
    }
}

static void run_hook(const odin3_celltype_def *def, const rcase *rc, hook_run *hr, uint8_t *y_out) {
    bool binary = def->n_ports == 3;
    set_params(def, rc, hr);
    uint32_t next = 0;
    lay_out_port(hr, &next, 0, rc->abits, rc->wa);
    if (binary) {
        lay_out_port(hr, &next, 1, rc->bbits, rc->wb);
    }
    lay_out_port(hr, &next, def->n_ports - 1, NULL, rc->wy);
    odin3_sim_cell cell = {hr->values, hr->spans,      def->n_ports, hr->params, NULL,
                           0,          ODIN3_SIM_COMB, NULL,         NULL,       0};
    uint32_t bytes = 0;
    if (def->sim_scratch_bytes != NULL) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, def->sim_scratch_bytes(&cell, &bytes));
    }
    uint64_t *scratch = calloc(bytes / sizeof(uint64_t) + 1, sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(scratch);
    cell.scratch = (uint8_t *)scratch;
    cell.n_scratch = bytes;
    def->simulate(&cell);
    free(scratch);
    for (uint32_t k = 0; k < rc->wy; k++) {
        y_out[k] = hr->values[hr->spans[def->n_ports - 1].idx[k]];
    }
}

/* A 0/1 value of bits (LSB first, width wide) with the sign flag. */
static odin3_cval make_val(const uint8_t *bits, uint32_t width, bool is_signed) {
    odin3_cval val = {0};
    const odin3_cval_spec spec = {width, is_signed ? ODIN3_CVAL_SIGNED : 0, ODIN3_BIT_0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_new(arena, &spec, &val));
    for (uint32_t k = 0; k < width; k++) {
        val.aval[k / 64] |= bits[k] != 0 ? (uint64_t)1 << (k % 64) : 0;
    }
    return val;
}

static void check_case(const cell_map *entry, const odin3_celltype_def *def, const rcase *rc) {
    static hook_run hr;
    uint8_t want[MAXW] = {0};
    run_hook(def, rc, &hr, want);
    bool binary = def->n_ports == 3;
    odin3_cval left = make_val(rc->abits, rc->wa, rc->sa);
    odin3_cval right = make_val(rc->bbits, rc->wb, rc->sb);
    const odin3_cop_args args = {entry->op, &left, binary ? &right : NULL, rc->wy,
                                 binary ? (rc->sa && rc->sb) : rc->sa};
    odin3_cval out = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_op(arena, &args, &out));
    TEST_ASSERT_EQUAL_UINT32(rc->wy, out.width);
    for (uint32_t k = 0; k < rc->wy; k++) {
        char msg[160];
        (void)snprintf(msg, sizeof msg, "%s A%u%s B%u%s Y%u bit %u", entry->cell, (unsigned)rc->wa,
                       rc->sa ? "s" : "", (unsigned)rc->wb, rc->sb ? "s" : "", (unsigned)rc->wy,
                       (unsigned)k);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want[k], odin3_cval_bit(&out, k), msg);
    }
}

static void test_cval_agrees_with_cell_hooks(void) {
    for (size_t i = 0; i < N_MAP; i++) {
        const odin3_celltype_def *def = def_named(k_map[i].cell);
        TEST_ASSERT_NOT_NULL_MESSAGE(def->simulate, k_map[i].cell);
        for (uint32_t draw = 0; draw < CASES; draw++) {
            rcase rc = draw_case();
            check_case(&k_map[i], def, &rc);
        }
    }
}

static bool in_list(const char *name, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_mapped(const char *name) {
    for (size_t i = 0; i < N_MAP; i++) {
        if (strcmp(name, k_map[i].cell) == 0) {
            return true;
        }
    }
    return false;
}

/* Every built-in word cell that has a hook is on the list, or deliberately left out. */
static void test_every_hooked_word_cell_is_covered_or_out_of_scope(void) {
    static const char *const k_out[] = {"$mux", "$pmux", "$shiftx", "$tribuf"};
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        const odin3_celltype_def *def = odin3_builtin_celltypes[i];
        bool sequential = (def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0;
        if (def->gran == ODIN3_GRAN_WORD && def->simulate != NULL && !sequential) {
            TEST_ASSERT_TRUE_MESSAGE(is_mapped(def->name) ||
                                         in_list(def->name, k_out, sizeof k_out / sizeof k_out[0]),
                                     def->name);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cval_agrees_with_cell_hooks);
    RUN_TEST(test_every_hooked_word_cell_is_covered_or_out_of_scope);
    return UNITY_END();
}
