/*
 * fuzz_lower.c — the crash fuzzer of `lower` (3B spec 14.5, plan Task 9 step 5). The input bytes
 * pick up to MAX_CELLS word cells (type, widths, signs) of a module, as the unit fixtures build
 * them (every cell port a module port), the caps are lowered so a wide cell is a located error,
 * and an out-of-memory failure is injected after a number of allocations that the input picks.
 * `lower` must return OK, NO_MEMORY or INVALID_ARG (a cap), never crash or leak, and `check` FULL
 * must hold afterwards in every case; an OK run leaves no word cell.
 *
 * Reach (3B-4 fix round, review M6): the 26 combinational types, the eleven word flip-flops, the
 * `$dlatch` and a `$mem` with one read and one write port (sync or async read, either clock edge,
 * an INIT or none). Not covered: wiring between cells (every port is a module port), a seed
 * corpus (libFuzzer writes new inputs into its corpus directory, which must not be the source
 * tree), and a failure index scaled to the allocation count of a dry run.
 */
#include "../unit/lower_fixture.h"
#include "ir/check.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_test.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Unity (linked for the fixture helper) references these. */
void setUp(void);
void tearDown(void);
void setUp(void) {
}
void tearDown(void) {
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum {
    MAX_CELLS = 6,
    MAX_WIDTH = 9,
    RECORD_BYTES = 5,
    FAIL_SCALE = 6, /* the injected failure: after FAIL_SCALE * n allocations, n from one byte */
    NO_FAIL_BYTE = 16,
    CAP_CELL_NODES = 6000,
    CAP_MODULE_NODES = 20000,
    CAP_MEM_BITS = 256,
    SIGN_A = 1,
    SIGN_B = 2
};

typedef enum shape { BINARY, UNARY, PMUX } shape;
/* The word flip-flops in definition order of the registry; W width, C CLK_POLARITY, E EN_POLARITY,
 * P the reset or load polarity, V the *_VALUE, S SET_POLARITY, R CLR_POLARITY (as test_lower_ff).
 */
static const char *const k_ff_types[] = {"$dff",    "$dffe",  "$sdff",  "$sdffe",
                                         "$sdffce", "$adff",  "$adffe", "$aldff",
                                         "$aldffe", "$dffsr", "$dffsre"};
static const char *const k_ff_codes[] = {"WC",    "WCE", "WCPV", "WCEPV", "WCEPV", "WCPV",
                                         "WCEPV", "WCP", "WCEP", "WCSR",  "WCSRE"};
typedef struct type_row {
    const char *name;
    shape shape;
} type_row;

static const type_row k_types[] = {
    {"$and", BINARY},      {"$or", BINARY},        {"$xor", BINARY},      {"$xnor", BINARY},
    {"$add", BINARY},      {"$sub", BINARY},       {"$mul", BINARY},      {"$div", BINARY},
    {"$mod", BINARY},      {"$eq", BINARY},        {"$ne", BINARY},       {"$lt", BINARY},
    {"$le", BINARY},       {"$gt", BINARY},        {"$ge", BINARY},       {"$shl", BINARY},
    {"$shr", BINARY},      {"$sshr", BINARY},      {"$shiftx", BINARY},   {"$not", UNARY},
    {"$neg", UNARY},       {"$reduce_and", UNARY}, {"$reduce_or", UNARY}, {"$reduce_xor", UNARY},
    {"$logic_not", UNARY}, {"$pmux", PMUX},
};

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

static void fail(const char *what) {
    (void)fprintf(stderr, "fuzz_lower: %s\n", what);
    __builtin_trap();
}

static uint32_t width_of(uint8_t byte) {
    return 1U + (uint32_t)byte % MAX_WIDTH;
}

enum {
    N_COMB = sizeof k_types / sizeof k_types[0],
    N_FF = sizeof k_ff_types / sizeof k_ff_types[0]
};

/* A flip-flop record: the type from rec[0], the width from rec[1], the polarities and the value
 * bits (0, 1 or x) from rec[2..4]; kind indexes k_ff_types. */
static void add_ff(lf *fix, odin3_module *mod, const uint8_t *rec, const char *prefix,
                   uint32_t kind) {
    uint32_t width = width_of(rec[1]);
    uint8_t bits[MAX_WIDTH];
    for (uint32_t i = 0; i < width; i++) {
        bits[i] = (uint8_t)(((rec[3] >> (i % 8U)) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
        bits[i] = ((rec[4] >> (i % 8U)) & 1U) != 0 && i % 3U == 0 ? ODIN3_BIT_X : bits[i];
    }
    odin3_value params[5];
    uint32_t count = 0;
    for (const char *code = k_ff_codes[kind]; *code != '\0'; code++, count++) {
        uint32_t pol = (rec[2] >> count) & 1U;
        params[count] = *code == 'W'   ? odin3_value_int(width)
                        : *code == 'V' ? (odin3_value){ODIN3_VAL_BITS, 0, bits, width, 0, 0}
                                       : odin3_value_int(pol);
    }
    lf_cell_spec cell = {k_ff_types[kind], params, count, prefix, {0}};
    (void)lf_cell_prefixed(fix, mod, &cell, prefix);
}

/* A `$dlatch` record. */
static void add_latch(lf *fix, odin3_module *mod, const uint8_t *rec, const char *prefix) {
    odin3_value params[2] = {odin3_value_int(width_of(rec[1])), odin3_value_int(rec[2] & 1U)};
    lf_cell_spec cell = {"$dlatch", params, 2, prefix, {0}};
    (void)lf_cell_prefixed(fix, mod, &cell, prefix);
}

enum { MEM_INIT_BITS = 8 * MAX_WIDTH, MEM_SIZE_MAX = 8 };

/* A `$mem` record: one read and one write port, SIZE <= 8, an OFFSET, sync or async read. */
static void add_mem(lf *fix, odin3_module *mod, const uint8_t *rec, const char *prefix) {
    uint32_t abits = 1U + rec[1] % 3U;
    uint32_t size = 1U + rec[2] % MEM_SIZE_MAX;
    uint32_t width = width_of(rec[3]);
    uint8_t rd_en = (rec[4] & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    uint8_t rd_pol = (rec[4] & 2U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    uint8_t rd_tr = (rec[4] & 4U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    uint8_t wr_en = ODIN3_BIT_1;
    uint8_t wr_pol = (rec[4] & 8U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    uint8_t init[MEM_INIT_BITS];
    uint32_t n_init = (rec[4] & 16U) != 0 ? size * width : 0;
    for (uint32_t i = 0; i < n_init; i++) {
        init[i] = (uint8_t)((rec[0] + i * 7U) % 3U);
        init[i] = init[i] == 2 ? ODIN3_BIT_X : init[i];
    }
    odin3_value params[13] = {
        {ODIN3_VAL_STRING, 0, NULL, 0, lf_str(fix, "mem"), 0},
        odin3_value_int(size),
        odin3_value_int(rec[0] % 3U),
        odin3_value_int(abits),
        odin3_value_int(width),
        odin3_value_int(1),
        odin3_value_int(1),
        {ODIN3_VAL_BITS, 0, &rd_en, 1, 0, 0},
        {ODIN3_VAL_BITS, 0, &rd_pol, 1, 0, 0},
        {ODIN3_VAL_BITS, 0, &rd_tr, 1, 0, 0},
        {ODIN3_VAL_BITS, 0, &wr_en, 1, 0, 0},
        {ODIN3_VAL_BITS, 0, &wr_pol, 1, 0, 0},
        {ODIN3_VAL_BITS, 0, init, n_init, 0, 0},
    };
    lf_cell_spec cell = {"$mem", params, 13, prefix, {0}};
    (void)lf_cell_prefixed(fix, mod, &cell, prefix);
}

/* Adds the cell of one RECORD_BYTES record to the module. */
static void add_cell(lf *fix, odin3_module *mod, const uint8_t *rec, uint32_t index) {
    char prefix[16];
    (void)snprintf(prefix, sizeof prefix, "c%u_", (unsigned)index);
    uint32_t pick = rec[0] % (N_COMB + N_FF + 2U);
    if (pick >= N_COMB + N_FF + 1U) {
        add_mem(fix, mod, rec, prefix);
        return;
    }
    if (pick == N_COMB + N_FF) {
        add_latch(fix, mod, rec, prefix);
        return;
    }
    if (pick >= N_COMB) {
        add_ff(fix, mod, rec, prefix, pick - N_COMB);
        return;
    }
    const type_row *row = &k_types[pick];
    lf_binary spec = {(rec[4] & SIGN_A) != 0, (rec[4] & SIGN_B) != 0, width_of(rec[1]),
                      width_of(rec[2]), width_of(rec[3])};
    odin3_value params[5];
    lf_cell_spec cell = {row->name, params, 5, prefix, {0}};
    if (row->shape == UNARY) {
        lf_unary_params(&spec, params);
        cell.n_params = 3;
    } else if (row->shape == PMUX) {
        params[0] = odin3_value_int(spec.a_width);
        params[1] = odin3_value_int(1U + spec.b_width % 4U);
        cell.n_params = 2;
    } else {
        lf_binary_params(&spec, params);
    }
    (void)lf_cell_prefixed(fix, mod, &cell, prefix);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1U + RECORD_BYTES) {
        return 0;
    }
    odin3_log_set_sink(quiet_sink, NULL);
    odin3_lower_limits caps = {CAP_CELL_NODES, CAP_MODULE_NODES, CAP_MEM_BITS};
    odin3_lower_test_set_limits(&caps);
    lf fix;
    lf_begin(&fix);
    odin3_module *mod = lf_module(&fix, "m");
    uint32_t cells = 0;
    for (size_t at = 1; at + RECORD_BYTES <= size && cells < MAX_CELLS; at += RECORD_BYTES) {
        add_cell(&fix, mod, data + at, cells++);
    }
    if (data[0] < NO_FAIL_BYTE) {
        odin3_util_set_alloc_fail_after(-1);
    } else {
        odin3_util_set_alloc_fail_after((long)data[0] * FAIL_SCALE);
    }
    odin3_status st = odin3_pass_run(fix.design, "lower", odin3_bytes_cstr(""));
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK && st != ODIN3_ERR_NO_MEMORY && st != ODIN3_ERR_INVALID_ARG) {
        fail("lower returned an unexpected status");
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    if (odin3_check_module(mod, opts) != ODIN3_OK) {
        fail("check FULL failed after lower");
    }
    if (st == ODIN3_OK && lf_word_nodes(&fix, mod) != 0) {
        fail("an OK run left a word cell");
    }
    lf_end(&fix);
    odin3_lower_test_set_limits(NULL);
    return 0;
}
