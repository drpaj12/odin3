/*
 * const-replay.c — the tiny driver of const-diff (2D spec 14.5, plan Task 28).
 *
 *   const-replay TABLE
 *
 * Replays every case line of a constant table (the format of tools/elab-check/gen-const-table:
 * six fields, or eight for --mixed) through odin3_cval_op and compares the result with the one
 * Yosys 0.55 folded, bit for bit and by sign flag. Prints one `const_table: <line>  got <bits>
 * (flags N)` line per disagreement (at most MAX_SHOWN) and a final `const-replay: N cases, M
 * wrong`. Exit 0 all agree, 1 any differs, 2 usage or I/O.
 */
#include "frontends/elab/cval.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CR_OK = 0,
    CR_DIFF = 1,
    CR_USAGE = 2,
    CR_LINE = 1 << 14,
    CR_PLAIN_FIELDS = 6,
    CR_MIXED_FIELDS = 8,
    CR_MAX_SHOWN = 20,
    CR_DECIMAL = 10
};

static const char *const k_op_names[ODIN3_COP_COUNT] = {
    "ADD",  "SUB",  "MUL", "DIV",  "MOD",  "POW",   "AND", "OR",   "XOR",  "XNOR", "SHL",  "SHR",
    "SSHL", "SSHR", "EQ",  "NE",   "EQX",  "NEX",   "LT",  "LE",   "GT",   "GE",   "LAND", "LOR",
    "NEG",  "POS",  "NOT", "LNOT", "RAND", "RNAND", "ROR", "RNOR", "RXOR", "RXNOR"};

/* A table line split on blanks: up to CR_MIXED_FIELDS fields, `count` of them. */
typedef struct cr_row {
    char text[CR_LINE];
    const char *field[CR_MIXED_FIELDS];
    int count;
} cr_row;

static bool split_row(cr_row *row, const char *line) {
    if (strlen(line) >= sizeof row->text) {
        return false;
    }
    (void)strcpy(row->text, line);
    char *save = NULL;
    row->count = 0;
    for (char *word = strtok_r(row->text, " ", &save); word != NULL;
         word = strtok_r(NULL, " ", &save)) {
        if (row->count == CR_MIXED_FIELDS) {
            return false;
        }
        row->field[row->count++] = word;
    }
    return row->count == CR_PLAIN_FIELDS || row->count == CR_MIXED_FIELDS;
}

static int op_named(const char *name) {
    for (int i = 0; i < ODIN3_COP_COUNT; i++) {
        if (strcmp(name, k_op_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* A value from MSB-first text in 0 1 x z; false for another character or no memory. */
static bool value_of(odin3_arena *arena, const char *msb, uint8_t flags, odin3_cval *val) {
    size_t len = strlen(msb);
    const odin3_cval_spec spec = {(uint32_t)len, flags, ODIN3_BIT_0};
    if (len == 0 || odin3_cval_new(arena, &spec, val) != ODIN3_OK) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;
        uint64_t mask = (uint64_t)1 << (pos % ODIN3_CVAL_WORD_BITS);
        char ch = msb[i];
        if (strchr("01xz", ch) == NULL) {
            return false;
        }
        val->aval[pos / ODIN3_CVAL_WORD_BITS] |= ch == '1' || ch == 'x' ? mask : 0;
        val->bval[pos / ODIN3_CVAL_WORD_BITS] |= ch == 'x' || ch == 'z' ? mask : 0;
    }
    return true;
}

/* A mixed table's operand flags: u s sized unsigned/signed, U S unsized unsigned/signed. */
static bool flags_of(const char *code, uint8_t *flags) {
    if (strchr("usUS", code[0]) == NULL || code[1] != '\0') {
        return false;
    }
    *flags = (uint8_t)((code[0] == 's' || code[0] == 'S' ? ODIN3_CVAL_SIGNED : 0) |
                       (code[0] == 'U' || code[0] == 'S' ? ODIN3_CVAL_UNSIZED : 0));
    return true;
}

/* EL-14's result flags: the context sign for arithmetic, bitwise, shifts and **; unsigned for
 * comparisons, logical operators and reductions; never UNSIZED or STRING. */
static uint8_t result_flags(odin3_cop op, bool is_signed) {
    bool by_context =
        op <= ODIN3_COP_SSHR || op == ODIN3_COP_NEG || op == ODIN3_COP_POS || op == ODIN3_COP_NOT;
    return by_context && is_signed ? ODIN3_CVAL_SIGNED : 0;
}

/* One line's operands and context: false when the line is malformed. */
typedef struct cr_case {
    odin3_cval left, right;
    odin3_cop_args args;
    const char *want;
} cr_case;

static bool parse_case(odin3_arena *arena, const cr_row *row, cr_case *cs) {
    bool mixed = row->count == CR_MIXED_FIELDS;
    int op = op_named(row->field[0]);
    bool is_signed = strcmp(row->field[2], "1") == 0;
    int b_at = mixed ? 5 : 4;
    bool unary = strcmp(row->field[b_at], "-") == 0;
    uint8_t lflags = is_signed ? ODIN3_CVAL_SIGNED : 0;
    uint8_t rflags = lflags;
    if (mixed &&
        (!flags_of(row->field[4], &lflags) || (!unary && !flags_of(row->field[6], &rflags)))) {
        return false;
    }
    char *end = NULL;
    unsigned long width = strtoul(row->field[1], &end, CR_DECIMAL);
    if (op < 0 || *end != '\0' || width == 0 ||
        !value_of(arena, row->field[3], lflags, &cs->left) ||
        !value_of(arena, unary ? "0" : row->field[b_at], unary ? 0 : rflags, &cs->right)) {
        return false;
    }
    cs->args = (odin3_cop_args){(odin3_cop)op, &cs->left, unary ? NULL : &cs->right,
                                (uint32_t)width, is_signed};
    cs->want = row->field[mixed ? 7 : 5];
    return true;
}

/* 1 when odin3_cval_op agrees with Yosys on the line, 0 when not, -1 for a malformed line. */
static int check_line(odin3_arena *arena, const char *line) {
    static cr_row row;
    static cr_case cs;
    odin3_cval out = {0};
    if (!split_row(&row, line) || !parse_case(arena, &row, &cs) ||
        odin3_cval_op(arena, &cs.args, &out) != ODIN3_OK) {
        return -1;
    }
    char bits[CR_LINE];
    if (out.width >= sizeof bits) {
        return -1;
    }
    for (uint32_t i = 0; i < out.width; i++) {
        bits[i] = "01xz"[odin3_cval_bit(&out, out.width - 1 - i)];
    }
    bits[out.width] = '\0';
    if (strcmp(cs.want, bits) == 0 && out.flags == result_flags(cs.args.op, cs.args.is_signed)) {
        return 1;
    }
    (void)printf("const_table: %s  got %s (flags %u)\n", line, bits, (unsigned)out.flags);
    return 0;
}

static int replay(FILE *file, odin3_arena *arena) {
    static char line[CR_LINE];
    unsigned long cases = 0;
    unsigned long wrong = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        int verdict = check_line(arena, line);
        cases++;
        if (verdict != 1) {
            wrong++;
            if (verdict < 0) {
                (void)printf("const_table: malformed line: %s\n", line);
            }
        }
        if (wrong >= CR_MAX_SHOWN) {
            break;
        }
    }
    (void)printf("const-replay: %lu cases, %lu wrong\n", cases, wrong);
    return wrong == 0 ? CR_OK : CR_DIFF;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        (void)fputs("usage: const-replay TABLE\n", stderr);
        return CR_USAGE;
    }
    FILE *file = fopen(argv[1], "r");
    odin3_arena *arena = odin3_arena_create(0);
    if (file == NULL || arena == NULL) {
        (void)fprintf(stderr, "const-replay: cannot open %s\n", argv[1]);
        return CR_USAGE;
    }
    odin3_log_set_level(ODIN3_LOG_ERROR);
    int rc = replay(file, arena);
    (void)fclose(file);
    odin3_arena_destroy(arena);
    return rc;
}
