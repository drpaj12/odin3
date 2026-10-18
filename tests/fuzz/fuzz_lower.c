/*
 * fuzz_lower.c — the crash fuzzer of `lower` (3B spec 14.5, plan Task 9 step 5). The input bytes
 * pick up to MAX_CELLS word cells (type, widths, signs) of a module, as the unit fixtures build
 * them (every cell port a module port), the caps are lowered so a wide cell is a located error,
 * and an out-of-memory failure is injected after a number of allocations that the input picks.
 * `lower` must return OK, NO_MEMORY or INVALID_ARG (a cap), never crash or leak, and `check` FULL
 * must hold afterwards in every case; an OK run leaves no word cell.
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

/* Adds the cell of one RECORD_BYTES record to the module. */
static void add_cell(lf *fix, odin3_module *mod, const uint8_t *rec, uint32_t index) {
    const type_row *row = &k_types[rec[0] % (sizeof k_types / sizeof k_types[0])];
    lf_binary spec = {(rec[4] & SIGN_A) != 0, (rec[4] & SIGN_B) != 0, width_of(rec[1]),
                      width_of(rec[2]), width_of(rec[3])};
    odin3_value params[5];
    lf_cell_spec cell = {row->name, params, 5, "dut", {0}};
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
    char prefix[16];
    (void)snprintf(prefix, sizeof prefix, "c%u_", (unsigned)index);
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
