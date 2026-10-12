/*
 * test_blif_isop.c — odin3_blif_isop (spec 3E §4.1, VB-4; plan 3E point (t); 3E Task 9): exact
 * covers, every function of 0-4 inputs (both sets, irredundance, the shorter set, on-set on a
 * tie), random tables of 5-16 inputs, the bits above 2^k, and a 16-input parity table under an
 * out-of-memory sweep.
 */
#include "backends/blif/isop.h"
#include "odin3/odin3.h"
#include "sim/prng.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    MAX_K = 16,
    MAX_WORDS = 1024,
    SMALL_K = 4,      /* every function of 0 .. 4 inputs */
    SMALL_ROWS = 16,  /* more than any ISOP of 4 inputs has */
    RANDOM_MIN_K = 5, /* random tables of 5 .. 16 inputs */
    RANDOM_TABLES = 200,
    RANDOM_INPUTS = 4096,
    ALL_INPUTS_K = 12,      /* every input up to 12 inputs */
    DETERMINISM_EVERY = 10, /* every 10th random table is covered twice */
    SEED = 0x3E5,
    OOM_LIMIT = 1000,
    TEXT_MAX = 256,
};

static odin3_strbuf cover;
static uint64_t tt[MAX_WORDS];
static uint64_t got[MAX_WORDS];

void setUp(void) {
    odin3_strbuf_init(&cover);
    memset(tt, 0, sizeof tt);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_strbuf_free(&cover);
}

/* --- helpers ------------------------------------------------------------------------------- */

/* A one-word table of `width` inputs (bit i = f(i)). */
typedef struct small_table {
    uint64_t bits;
    uint32_t width;
} small_table;

/* A cover row's cube: the inputs it tests, and their values. */
typedef struct cube {
    uint32_t care, value;
} cube;

/* The valid bits of each word of a table of `width` inputs. */
static uint64_t mask_of(uint32_t width) {
    return width >= 6 ? UINT64_MAX : ((uint64_t)1 << (1U << width)) - 1U;
}

static const char *text_of(const odin3_strbuf *buf) {
    return buf->len > 0 ? buf->data : "";
}

/* Sets in table the minterms of the cube (of `width` inputs). */
static void add_cube(uint64_t *table, cube row, uint32_t width) {
    uint32_t dont = ~row.care & ((1U << width) - 1U);
    uint32_t sub = 0;
    do {
        uint32_t minterm = row.value | sub;
        table[minterm >> 6] |= (uint64_t)1 << (minterm & 63U);
        sub = (sub - dont) & dont;
    } while (sub != 0);
}

/* A parsed cover: its rows, their common output character ('\0' for none), and for up to
 * SMALL_ROWS rows each row's minterms (6 inputs or fewer). */
typedef struct parsed {
    uint32_t rows;
    char out;
    uint64_t row_minterms[SMALL_ROWS];
} parsed;

/* The plane character at text into the cube's bit `bit`; fails the test on any other. */
static void add_literal(const char *text, uint32_t bit, cube *row) {
    char lit = text[bit];
    TEST_ASSERT_TRUE_MESSAGE(lit == '0' || lit == '1' || lit == '-', text);
    row->care |= lit != '-' ? 1U << bit : 0U;
    row->value |= lit == '1' ? 1U << bit : 0U;
}

/* One row at text (`width` plane characters, a blank unless width is 0, the output, a newline)
 * into its cube; its output character. Fails the test on any other text. */
static char parse_row(const char *text, uint32_t width, cube *row) {
    *row = (cube){0, 0};
    for (uint32_t j = 0; j < width; j++) {
        add_literal(text, j, row);
    }
    const char *tail = text + width;
    if (width > 0) {
        TEST_ASSERT_EQUAL_CHAR(' ', *tail++);
    }
    TEST_ASSERT_TRUE_MESSAGE(tail[0] == '0' || tail[0] == '1', text);
    TEST_ASSERT_EQUAL_CHAR('\n', tail[1]);
    return tail[0];
}

/* The OR of the rows of text (`width` inputs) into table (odin3_blif_isop_words(width) words). */
static parsed parse_cover(const char *text, uint32_t width, uint64_t *table) {
    parsed res = {0};
    size_t row_len = width + (width > 0 ? 3U : 2U);
    size_t len = strlen(text);
    memset(table, 0, sizeof *table * odin3_blif_isop_words(width));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, len % row_len, text);
    for (size_t at = 0; at < len; at += row_len) {
        cube row = {0, 0};
        char out = parse_row(text + at, width, &row);
        TEST_ASSERT_TRUE_MESSAGE(res.out == '\0' || res.out == out, text);
        res.out = out;
        add_cube(table, row, width);
        if (width <= 6 && res.rows < SMALL_ROWS) {
            uint64_t one = 0;
            add_cube(&one, row, width);
            res.row_minterms[res.rows] = one;
        }
        res.rows++;
    }
    return res;
}

/* The function a cover of output character out computes from the OR of its rows (one word). */
static uint64_t function_of(uint64_t rows_or, char out, uint32_t width) {
    return out == '0' ? ~rows_or & mask_of(width) : rows_or;
}

/* The OR of the rows of a small cover but row number skip. */
static uint64_t rows_but(const parsed *rows, uint32_t skip) {
    uint64_t rest = 0;
    for (uint32_t other = 0; other < rows->rows; other++) {
        rest |= other != skip ? rows->row_minterms[other] : 0U;
    }
    return rest;
}

/* Removing any one row of a small cover changes the function it computes. */
static void expect_irredundant(const parsed *rows, uint64_t whole, const char *text) {
    TEST_ASSERT_TRUE(rows->rows <= SMALL_ROWS);
    for (uint32_t skip = 0; skip < rows->rows; skip++) {
        TEST_ASSERT_TRUE_MESSAGE(rows_but(rows, skip) != whole, text);
    }
}

/* odin3_blif_isop of tt into a fresh cover: its status, rows and constant. */
typedef struct isop_result {
    odin3_status st;
    uint32_t rows;
    int constant;
} isop_result;

static isop_result run_isop(uint32_t width) {
    isop_result res = {ODIN3_OK, UINT32_MAX, -2};
    odin3_strbuf_clear(&cover);
    res.st = odin3_blif_isop(tt, width, &cover, &res.rows, &res.constant);
    return res;
}

/* odin3_blif_isop of the table gives want (rows of it, constant -1). */
static void expect_cover(small_table table, const char *want, uint32_t rows) {
    tt[0] = table.bits;
    isop_result res = run_isop(table.width);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
    TEST_ASSERT_EQUAL_STRING(want, text_of(&cover));
    TEST_ASSERT_EQUAL_UINT32(rows, res.rows);
    TEST_ASSERT_EQUAL_INT(-1, res.constant);
}

/* odin3_blif_isop of the table is the constant `value`: no row, nothing appended. */
static void expect_constant(small_table table, int value) {
    tt[0] = table.bits;
    isop_result res = run_isop(table.width);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
    TEST_ASSERT_EQUAL_UINT32(0, res.rows);
    TEST_ASSERT_EQUAL_INT(value, res.constant);
    TEST_ASSERT_EQUAL_size_t(0, cover.len);
}

/* odin3_blif_isop_set of the table (offset or not) gives want, with its rows counted. */
static void expect_set(small_table table, bool offset, const char *want) {
    tt[0] = table.bits;
    uint32_t rows = UINT32_MAX;
    odin3_strbuf_clear(&cover);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_isop_set(tt, table.width, offset, &cover, &rows));
    TEST_ASSERT_EQUAL_STRING(want, text_of(&cover));
    TEST_ASSERT_EQUAL_UINT32(parse_cover(want, table.width, got).rows, rows);
}

/* --- exact covers -------------------------------------------------------------------------- */

static void test_exact_covers(void) {
    expect_cover((small_table){0x8, 2}, "11 1\n", 1);         /* AND2 */
    expect_cover((small_table){0xE, 2}, "00 0\n", 1);         /* OR2: the off-set's one row */
    expect_set((small_table){0xE, 2}, false, "-1 1\n1- 1\n"); /* OR2's on-set has 2 */
    expect_set((small_table){0xE, 2}, true, "00 0\n");
    expect_cover((small_table){0x6, 2}, "10 1\n01 1\n", 2); /* XOR2: a tie, the on-set */
    expect_set((small_table){0x6, 2}, true, "00 0\n11 0\n");
    expect_cover((small_table){0x2, 1}, "1 1\n", 1);                  /* identity */
    expect_cover((small_table){0x1, 1}, "0 1\n", 1);                  /* inverter */
    expect_cover((small_table){0xE8, 3}, "-11 1\n1-1 1\n11- 1\n", 3); /* majority */
    expect_constant((small_table){0x0, 2}, 0);
    expect_constant((small_table){0xF, 2}, 1);
    expect_constant((small_table){0x1, 0}, 1); /* k = 0 with bit 0 = 1 */
    expect_constant((small_table){0x0, 0}, 0);
    expect_set((small_table){0x0, 0}, false, "");
    expect_set((small_table){0x1, 0}, false, "1\n");
    expect_set((small_table){0x0, 0}, true, "0\n");
    expect_set((small_table){0xF, 2}, false, "-- 1\n");
    expect_set((small_table){0xF, 2}, true, "");
}

/* The output order of point (t): input k - 1 first, the 0 branch, the 1 branch, then `-`. */
static void test_branch_order(void) {
    /* f = a2 ? a0 : a1: the 0 branch's row, then the 1 branch's (the consensus a0 a1 is
     * redundant, so no shared row) */
    expect_set((small_table){0xAC, 3}, false, "-10 1\n1-1 1\n");
    /* majority: the shared branch's row last */
    expect_set((small_table){0xE8, 3}, false, "-11 1\n1-1 1\n11- 1\n");
    /* the same mux over two words (k = 7): input 6 = 0 selects a0, input 6 = 1 selects a1 */
    tt[0] = 0xAAAAAAAAAAAAAAAAULL;
    tt[1] = 0xCCCCCCCCCCCCCCCCULL;
    uint32_t rows = 0;
    odin3_strbuf_clear(&cover);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_isop_set(tt, 7, false, &cover, &rows));
    TEST_ASSERT_EQUAL_STRING("1-----0 1\n-1----1 1\n", text_of(&cover));
    TEST_ASSERT_EQUAL_UINT32(2, rows);
}

static void test_words(void) {
    for (uint32_t width = 0; width <= 6; width++) {
        TEST_ASSERT_EQUAL_UINT32(1, odin3_blif_isop_words(width));
    }
    TEST_ASSERT_EQUAL_UINT32(2, odin3_blif_isop_words(7));
    TEST_ASSERT_EQUAL_UINT32(1024, odin3_blif_isop_words(16));
    TEST_ASSERT_EQUAL_UINT32(1024, odin3_blif_isop_words(17));
}

static void test_invalid_args(void) {
    uint32_t rows = 7;
    int constant = 7;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_isop(NULL, 2, &cover, &rows, &constant));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop(tt, 2, NULL, &rows, &constant));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop(tt, 2, &cover, NULL, &constant));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop(tt, 2, &cover, &rows, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop(tt, 17, &cover, &rows, &constant));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_blif_isop_set(NULL, 2, false, &cover, &rows));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop_set(tt, 2, false, NULL, &rows));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop_set(tt, 2, true, &cover, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_isop_set(tt, 17, true, &cover, &rows));
    TEST_ASSERT_EQUAL_UINT32(7, rows);
    TEST_ASSERT_EQUAL_INT(7, constant);
    TEST_ASSERT_EQUAL_size_t(0, cover.len);
}

/* Bits from 2^k up of word 0 are ignored. */
static void test_bits_above_ignored(void) {
    tt[0] = 0xF8;
    isop_result high = run_isop(2);
    char high_text[TEXT_MAX];
    TEST_ASSERT_TRUE(cover.len < TEXT_MAX);
    memcpy(high_text, text_of(&cover), cover.len + 1);
    tt[0] = 0x8;
    isop_result low = run_isop(2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, high.st);
    TEST_ASSERT_EQUAL_STRING(text_of(&cover), high_text);
    TEST_ASSERT_EQUAL_UINT32(low.rows, high.rows);
    TEST_ASSERT_EQUAL_INT(low.constant, high.constant);
    TEST_ASSERT_EQUAL_STRING("11 1\n", high_text);
    expect_set((small_table){0xF8, 2}, true, "-0 0\n0- 0\n");
    expect_constant((small_table){0xF0, 2}, 0); /* only the ignored bits are set */
    expect_constant((small_table){0xFF, 2}, 1);
    expect_constant((small_table){0xFE, 0}, 0);
}

/* --- every function of 0-4 inputs ---------------------------------------------------------- */

/* One set's cover of the table (4 inputs or fewer) computes it (on-set) or its complement
 * (off-set) and is irredundant; its text in text, its rows returned. */
static uint32_t check_set(small_table table, bool offset, char *text) {
    tt[0] = table.bits;
    uint32_t rows = UINT32_MAX;
    odin3_strbuf_clear(&cover);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_isop_set(tt, table.width, offset, &cover, &rows));
    TEST_ASSERT_TRUE(cover.len < TEXT_MAX);
    memcpy(text, text_of(&cover), cover.len + 1);
    parsed cover_rows = parse_cover(text, table.width, got);
    TEST_ASSERT_EQUAL_UINT32(cover_rows.rows, rows);
    TEST_ASSERT_TRUE_MESSAGE(rows == 0 || cover_rows.out == (offset ? '0' : '1'), text);
    uint64_t want = offset ? ~table.bits & mask_of(table.width) : table.bits;
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(want, got[0], text);
    expect_irredundant(&cover_rows, got[0], text);
    return rows;
}

/* odin3_blif_isop of the table (4 inputs or fewer): the constant, or the shorter set's text
 * (on-set on a tie), which computes the table. */
static void check_function(small_table table) {
    char on_text[TEXT_MAX];
    char off_text[TEXT_MAX];
    uint32_t on_rows = check_set(table, false, on_text);
    uint32_t off_rows = check_set(table, true, off_text);
    tt[0] = table.bits;
    isop_result res = run_isop(table.width);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
    if (table.bits == 0 || table.bits == mask_of(table.width)) {
        TEST_ASSERT_EQUAL_INT(table.bits == 0 ? 0 : 1, res.constant);
        TEST_ASSERT_EQUAL_UINT32(0, res.rows);
        TEST_ASSERT_EQUAL_size_t(0, cover.len);
        return;
    }
    TEST_ASSERT_EQUAL_INT(-1, res.constant);
    TEST_ASSERT_EQUAL_UINT32(on_rows <= off_rows ? on_rows : off_rows, res.rows);
    TEST_ASSERT_EQUAL_STRING(on_rows <= off_rows ? on_text : off_text, text_of(&cover));
    parsed chosen = parse_cover(text_of(&cover), table.width, got);
    TEST_ASSERT_EQUAL_HEX64(table.bits, function_of(got[0], chosen.out, table.width));
}

static void test_every_small_function(void) {
    for (uint32_t width = 0; width <= SMALL_K; width++) {
        uint64_t count = (uint64_t)1 << (1U << width);
        for (uint64_t bits = 0; bits < count; bits++) {
            check_function((small_table){bits, width});
        }
    }
}

/* --- random tables of 5-16 inputs ---------------------------------------------------------- */

static uint32_t tt_bit(const uint64_t *table, uint32_t index) {
    return (uint32_t)(table[index >> 6] >> (index & 63U)) & 1U;
}

/* The cover of the table in tt (`width` inputs) computes it: on every input up to ALL_INPUTS_K
 * inputs, else on RANDOM_INPUTS random ones. */
static void expect_computes(uint32_t width, odin3_prng *prng) {
    parsed cover_rows = parse_cover(text_of(&cover), width, got);
    uint32_t flip = cover_rows.out == '0' ? 1U : 0U;
    uint32_t inputs = width <= ALL_INPUTS_K ? 1U << width : RANDOM_INPUTS;
    for (uint32_t step = 0; step < inputs; step++) {
        uint32_t index =
            width <= ALL_INPUTS_K ? step : (uint32_t)odin3_prng_next(prng) & ((1U << width) - 1U);
        if ((tt_bit(got, index) ^ flip) != tt_bit(tt, index)) {
            TEST_FAIL_MESSAGE("the cover differs from the table");
        }
    }
}

/* A second call on tt gives the same bytes and rows as the first (first, its text in cover). */
static void expect_deterministic(uint32_t width, isop_result first) {
    odin3_strbuf again;
    odin3_strbuf_init(&again);
    uint32_t again_rows = 0;
    int constant = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_isop(tt, width, &again, &again_rows, &constant));
    TEST_ASSERT_EQUAL_UINT32(first.rows, again_rows);
    TEST_ASSERT_EQUAL_size_t(cover.len, again.len);
    TEST_ASSERT_EQUAL_MEMORY(cover.data, again.data, cover.len);
    odin3_strbuf_free(&again);
}

static void test_random_tables(void) {
    odin3_prng prng;
    odin3_prng_seed(&prng, SEED);
    for (uint32_t width = RANDOM_MIN_K; width <= MAX_K; width++) {
        for (uint32_t table_no = 0; table_no < RANDOM_TABLES; table_no++) {
            for (uint32_t i = 0; i < odin3_blif_isop_words(width); i++) {
                tt[i] = odin3_prng_next(&prng) & mask_of(width);
            }
            isop_result res = run_isop(width);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
            TEST_ASSERT_EQUAL_INT(-1, res.constant);
            if (table_no % DETERMINISM_EVERY == 0) {
                expect_deterministic(width, res);
            }
            expect_computes(width, &prng);
        }
    }
}

/* --- 16 inputs: parity, out of memory ------------------------------------------------------ */

/* tt = the parity of 16 inputs: bit i is the parity of i. */
static void parity_table(void) {
    const uint64_t low = 0x6996966996696996ULL; /* bit b: the parity of b, b < 64 */
    for (uint32_t word = 0; word < MAX_WORDS; word++) {
        uint32_t parity = 0;
        for (uint32_t bits = word; bits != 0; bits >>= 1) {
            parity ^= bits & 1U;
        }
        tt[word] = parity != 0 ? ~low : low;
    }
}

/* 2^15 rows a side: a tie, the on-set; every row a minterm. */
static void test_parity_16(void) {
    parity_table();
    isop_result res = run_isop(MAX_K);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
    TEST_ASSERT_EQUAL_UINT32(1U << 15, res.rows);
    TEST_ASSERT_EQUAL_INT(-1, res.constant);
    TEST_ASSERT_NULL(memchr(cover.data, '-', cover.len));
    TEST_ASSERT_EQUAL_size_t((size_t)(MAX_K + 3) << 15, cover.len);
    /* the first row: inputs 15 .. 1 in their 0 branches, then a0 = 1 */
    TEST_ASSERT_EQUAL_MEMORY("1000000000000000 1\n", cover.data, MAX_K + 3);
    odin3_prng prng;
    odin3_prng_seed(&prng, SEED);
    expect_computes(MAX_K, &prng);
}

/* odin3_blif_isop (or, when set, odin3_blif_isop_set's off-set) of tt under every allocation
 * failure: NO_MEMORY with the cover's earlier text kept and the outputs untouched, until it
 * succeeds with the unfailed run's text. */
static void oom_sweep(uint32_t width, bool set) {
    static const char KEEP[] = "keep\n";
    odin3_strbuf want;
    odin3_strbuf_init(&want);
    uint32_t want_rows = 0;
    int want_constant = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          set ? odin3_blif_isop_set(tt, width, true, &want, &want_rows)
                              : odin3_blif_isop(tt, width, &want, &want_rows, &want_constant));
    uint32_t failures = 0;
    bool done = false;
    for (long fail_at = 0; fail_at < OOM_LIMIT && !done; fail_at++) {
        odin3_strbuf_free(&cover);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&cover, odin3_bytes_cstr(KEEP)));
        uint32_t rows = 7;
        int constant = 7;
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = set ? odin3_blif_isop_set(tt, width, true, &cover, &rows)
                              : odin3_blif_isop(tt, width, &cover, &rows, &constant);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            done = true;
            TEST_ASSERT_EQUAL_UINT32(want_rows, rows);
            TEST_ASSERT_EQUAL_size_t(strlen(KEEP) + want.len, cover.len);
            TEST_ASSERT_EQUAL_MEMORY(KEEP, cover.data, strlen(KEEP));
            TEST_ASSERT_EQUAL_MEMORY(want.data, cover.data + strlen(KEEP), want.len);
        } else {
            failures++;
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_STRING(KEEP, text_of(&cover));
            TEST_ASSERT_EQUAL_UINT32(7, rows);
            TEST_ASSERT_EQUAL_INT(7, constant);
        }
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_GREATER_THAN_UINT32(1, failures);
    odin3_strbuf_free(&want);
}

static void test_out_of_memory_parity_16(void) {
    parity_table();
    oom_sweep(MAX_K, false);
    oom_sweep(MAX_K, true);
}

/* The OR of 16 inputs: the off-set (one row) replaces the on-set's 16 in the cover. */
static void test_out_of_memory_offset_chosen(void) {
    memset(tt, 0xFF, sizeof tt);
    tt[0] = ~(uint64_t)1;
    isop_result res = run_isop(MAX_K);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.st);
    TEST_ASSERT_EQUAL_STRING("0000000000000000 0\n", text_of(&cover));
    TEST_ASSERT_EQUAL_UINT32(1, res.rows);
    oom_sweep(MAX_K, false);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_exact_covers);
    RUN_TEST(test_branch_order);
    RUN_TEST(test_words);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_bits_above_ignored);
    RUN_TEST(test_every_small_function);
    RUN_TEST(test_random_tables);
    RUN_TEST(test_parity_16);
    RUN_TEST(test_out_of_memory_parity_16);
    RUN_TEST(test_out_of_memory_offset_chosen);
    return UNITY_END();
}
