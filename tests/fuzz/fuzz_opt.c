/*
 * fuzz_opt.c — the opt family's crash fuzzer (2F spec §20.4, plan Task 18), a libFuzzer target
 * built with -DODIN3_FUZZ=ON under clang, ASan and UBSan.
 *
 * Input: one mode byte, then the payload.
 *  - bits 0-2 pick the `opt` spelling (kModes below): default, -full, -fast, -fast -noff,
 *    -nodffe -nosdff, -keepdc, -full -keepdc, and one with every opt_expr switch and -purge;
 *  - bit 3 picks the payload: 0 is one BLIF file (`read_blif`), 1 is the word-cell builder;
 *  - bits 4-6 inject an allocation failure (0: none) during `opt`;
 *  - bit 7 lowers the caps (two iterations, 16 mux-tree evaluations), so the cap paths run.
 *
 * The word-cell builder reads the payload as records: byte 0 is the input count n (2..5, then
 * the widths of the inputs follow, one byte each: 1..8 bits), then 12-byte records of
 *   [type, width|output flag, ten operand bytes]
 * up to MAX_CELLS of them (a short record is padded with zeros). Record i makes the cell of type
 * `type % N_TYPES` over the wire `n<i>`; an operand byte picks a constant (with x bits) or an input
 * or earlier cell output, sliced or zero-extended to the width its port needs, and a flip-flop may
 * also read its own or a later flip-flop's Q. The records become the RTLIL text tests/unit/
 * proc_build.c reads, so the generator can only build well-typed IR; a text it rejects ends the
 * run. A fresh design per input.
 *
 * Invariants (a violation is __builtin_trap(), which libFuzzer reports; tests/ only):
 *  - the status of `opt` is ODIN3_OK (or, under an injected failure, ODIN3_ERR_NO_MEMORY);
 *    never CHECK (a wrong rewrite in a Debug build: --opt-simcheck compares 64 cycles of the
 *    outputs around every sub-pass call), INVALID_ARG or PARSE;
 *  - odin3_check_design FULL passes afterwards, also after NO_MEMORY (OP-2: a failed rewrite
 *    leaves the IR valid);
 *  - no leak, no sanitizer report, no hang (libFuzzer's -rss_limit_mb and -timeout).
 * The seeds come from tools/opt-check/opt_fuzz_seeds.py.
 */
#include "../unit/proc_build.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/opt/opt_test.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum {
    PATH_MAX_LEN = 1024,
    MODE_SPELLING_MASK = 7,
    MODE_BUILDER = 1 << 3,
    MODE_FAIL_SHIFT = 4,
    MODE_FAIL_MASK = 7,
    MODE_LOW_CAPS = 1 << 7,
    FAIL_SCALE = 8, /* the injected failure: after FAIL_SCALE * n^3 allocations */
    LOW_ITERATIONS = 2,
    LOW_MUXTREE_EVALS = 16,
    MIN_INPUTS = 2,
    MAX_INPUTS = 5,
    MAX_CELLS = 24,
    RECORD_BYTES = 12,
    OPERAND_BYTES = 10,
    MAX_WIDTH = 8,
    TEXT_BYTES = 1 << 16,
    ARGS_BYTES = 128,
    NAME_BYTES = 16
};

static const char *const k_spellings[MODE_SPELLING_MASK + 1] = {
    "",
    "-full",
    "-fast",
    "-fast -noff",
    "-nodffe -nosdff",
    "-keepdc",
    "-full -keepdc",
    "-mux_undef -mux_bool -undriven -fine -noclkinv -purge"};

static char dir[PATH_MAX_LEN];
static char blif_path[PATH_MAX_LEN];

static void fail(const char *what) {
    (void)fprintf(stderr, "fuzz_opt: %s\n", what);
    __builtin_trap();
}

/* Silent, unless FUZZ_OPT_VERBOSE is set: then the errors go to stderr (to debug the builder). */
static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR && getenv("FUZZ_OPT_VERBOSE") != NULL) {
        (void)fprintf(stderr, "fuzz_opt: %s\n", msg);
    }
}

static void remove_files(void) {
    (void)remove(blif_path);
    (void)rmdir(dir);
}

static void join_path(char *path, const char *prefix, const char *suffix) {
    int len = snprintf(path, PATH_MAX_LEN, "%s%s", prefix, suffix);
    if (len < 0 || len >= PATH_MAX_LEN) {
        fail("the temporary directory's path is too long");
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    const char *base = getenv("TMPDIR");
    join_path(dir, base != NULL && base[0] != '\0' ? base : "/tmp", "/fuzz_opt_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        fail("cannot create the temporary directory");
    }
    join_path(blif_path, dir, "/in.blif");
    (void)atexit(remove_files);
    return 0;
}

/* --- the word-cell builder ------------------------------------------------------------------ */

/* The cell types the builder makes (opt_fuzz_seeds.py counts these enumerators). */
typedef enum cell_type {
    T_NOT,
    T_AND,
    T_OR,
    T_XOR,
    T_ADD,
    T_SUB,
    T_MUL,
    T_EQ,
    T_NE,
    T_LT,
    T_GE,
    T_SHL,
    T_SHR,
    T_SSHR,
    T_MUX,
    T_PMUX,
    T_REDUCE_AND,
    T_REDUCE_OR,
    T_REDUCE_XOR,
    T_LOGIC_NOT,
    T_DFF,
    T_DFFE,
    T_SDFF,
    T_ADFF,
    T_SDFFE,
    N_TYPES
} cell_type;

typedef struct rec {
    uint8_t type;
    uint8_t width; /* 1..MAX_WIDTH */
    bool output;   /* the cell's Y or Q is a module output */
    uint8_t bytes[OPERAND_BYTES];
} rec;

/* The text under construction. */
typedef struct text {
    char buf[TEXT_BYTES];
    size_t len;
    bool full;
} text;

static void put(text *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(text *out, const char *fmt, ...) {
    if (out->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out->buf + out->len, sizeof out->buf - out->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof out->buf - out->len) {
        out->full = true;
        return;
    }
    out->len += (size_t)n;
}

/* The state of one build: the inputs, the records and the cursor over one record's bytes. */
typedef struct build {
    text out;
    uint8_t in_width[MAX_INPUTS];
    uint32_t n_inputs;
    rec recs[MAX_CELLS];
    uint32_t n_recs;
    uint32_t widths[MAX_CELLS]; /* the width of each record's output wire */
    uint32_t cur;               /* the record being rendered */
    uint32_t pos;               /* the operand cursor */
} build;

static bool is_ff(uint32_t type) {
    return type >= T_DFF && type <= T_SDFFE;
}

static uint8_t next_byte(build *b) {
    return b->recs[b->cur].bytes[b->pos++ % OPERAND_BYTES];
}

/* The wire an operand may read: a name and a width. */
typedef struct sig {
    char name[NAME_BYTES];
    uint32_t width;
} sig;

enum { MAX_SIGS = MAX_INPUTS + MAX_CELLS + 2 };

/* The signals record `cur` may read: the inputs, earlier cells, and (flip-flops only) any
 * flip-flop's Q, its own included (feedback through a register is not a loop). */
static uint32_t visible(const build *b, sig *list) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->n_inputs; i++) {
        (void)snprintf(list[n].name, NAME_BYTES, "i%u", (unsigned)i);
        list[n++].width = b->in_width[i];
    }
    bool reader_is_ff = is_ff(b->recs[b->cur].type);
    for (uint32_t j = 0; j < b->n_recs; j++) {
        bool ok = j < b->cur || (reader_is_ff && is_ff(b->recs[j].type));
        if (ok) {
            (void)snprintf(list[n].name, NAME_BYTES, "n%u", (unsigned)j);
            list[n++].width = b->widths[j];
        }
    }
    return n;
}

/* One operand of `want` bits: a constant (kind 0: 0/1 bits, kind 1: with x bits) or a signal,
 * sliced when wider and zero-extended when narrower. */
static void operand(build *b, uint32_t want) {
    uint8_t pick = next_byte(b);
    text *out = &b->out;
    unsigned kind = pick & 7U;
    if (kind < 2) {
        uint8_t value = next_byte(b);
        uint8_t xmask = kind == 1 ? (uint8_t)(pick >> 3) : 0;
        put(out, "%u'", (unsigned)want);
        for (uint32_t bit = want; bit-- > 0;) {
            bool is_x = ((xmask >> (bit % 5)) & 1U) != 0 && kind == 1 && bit < 5;
            put(out, "%c", is_x ? 'x' : (((value >> bit) & 1U) != 0 ? '1' : '0'));
        }
        return;
    }
    sig list[MAX_SIGS];
    uint32_t n = visible(b, list);
    const sig *s = &list[(pick >> 3) % n];
    if (s->width == want) {
        put(out, "\\%s", s->name);
    } else if (s->width > want) {
        uint32_t lo = next_byte(b) % (s->width - want + 1);
        put(out, "\\%s [%u:%u]", s->name, (unsigned)(lo + want - 1), (unsigned)lo);
    } else {
        put(out, "{ %u'", (unsigned)(want - s->width));
        for (uint32_t k = want - s->width; k-- > 0;) {
            put(out, "0");
        }
        put(out, " \\%s }", s->name);
    }
}

static void connect(build *b, const char *port, uint32_t want) {
    put(&b->out, "    connect \\%s ", port);
    operand(b, want);
    put(&b->out, "\n");
}

static unsigned bit_of(build *b) {
    return next_byte(b) & 1U;
}

/* A constant of `w` bits as RTLIL text from a byte (the reset value). */
static void value_param(build *b, const char *name, uint32_t w) {
    uint8_t v = next_byte(b);
    put(&b->out, "    parameter \\%s %u'", name, (unsigned)w);
    for (uint32_t bit = w; bit-- > 0;) {
        put(&b->out, "%c", ((v >> bit) & 1U) != 0 ? '1' : '0');
    }
    put(&b->out, "\n");
}

static void param(build *b, const char *name, uint32_t value) {
    put(&b->out, "    parameter \\%s %u\n", name, (unsigned)value);
}

/* The operand widths of a binary cell: A and B in 1..MAX_WIDTH, taken from the bytes. */
static uint32_t pick_width(build *b) {
    return 1U + (next_byte(b) % MAX_WIDTH);
}

static void render_unary(build *b, const char *type, uint32_t y, bool reduce) {
    uint32_t a = reduce ? pick_width(b) : y;
    put(&b->out, "  cell %s $c%u\n", type, (unsigned)b->cur);
    param(b, "A_SIGNED", bit_of(b));
    param(b, "A_WIDTH", a);
    param(b, "Y_WIDTH", y);
    connect(b, "A", a);
}

static void render_binary(build *b, const char *type, uint32_t y, bool same, bool shift) {
    uint32_t a = same ? y : pick_width(b);
    uint32_t bw = same ? y : pick_width(b);
    put(&b->out, "  cell %s $c%u\n", type, (unsigned)b->cur);
    /* The flags are drawn apart, so mixed-sign cells occur: the cell is signed only when both are
     * set (the ruling of spec 17 #57); a shift's B flag is the amount's own and stays 0. */
    param(b, "A_SIGNED", bit_of(b));
    param(b, "B_SIGNED", shift ? 0U : bit_of(b));
    param(b, "A_WIDTH", a);
    param(b, "B_WIDTH", bw);
    param(b, "Y_WIDTH", y);
    connect(b, "A", a);
    connect(b, "B", bw);
}

static void render_ff(build *b, const char *type, uint32_t w) {
    put(&b->out, "  cell %s $c%u\n", type, (unsigned)b->cur);
    param(b, "WIDTH", w);
    param(b, "CLK_POLARITY", bit_of(b));
}

/* The cell of record cur; its output is the wire n<cur> (declared before the cells). */
static void render_cell(build *b) {
    const rec *r = &b->recs[b->cur];
    uint32_t w = r->width;
    unsigned ctl = 1U + (r->bytes[0] % 2U); /* the width a comparison's Y takes: 1 or 2 */
    b->pos = 1;
    switch (r->type % N_TYPES) {
    case T_NOT:
        render_unary(b, "$not", w, false);
        break;
    case T_AND:
        render_binary(b, "$and", w, false, false);
        break;
    case T_OR:
        render_binary(b, "$or", w, false, false);
        break;
    case T_XOR:
        render_binary(b, "$xor", w, true, false);
        break;
    case T_ADD:
        render_binary(b, "$add", w, false, false);
        break;
    case T_SUB:
        render_binary(b, "$sub", w, false, false);
        break;
    case T_MUL:
        render_binary(b, "$mul", w, false, false);
        break;
    case T_EQ:
        render_binary(b, "$eq", ctl, false, false);
        break;
    case T_NE:
        render_binary(b, "$ne", ctl, false, false);
        break;
    case T_LT:
        render_binary(b, "$lt", ctl, false, false);
        break;
    case T_GE:
        render_binary(b, "$ge", ctl, false, false);
        break;
    case T_SHL:
        render_binary(b, "$shl", w, false, true);
        break;
    case T_SHR:
        render_binary(b, "$shr", w, false, true);
        break;
    case T_SSHR:
        render_binary(b, "$sshr", w, false, true);
        break;
    case T_MUX:
        put(&b->out, "  cell $mux $c%u\n", (unsigned)b->cur);
        param(b, "WIDTH", w);
        connect(b, "A", w);
        connect(b, "B", w);
        connect(b, "S", 1);
        break;
    case T_PMUX: {
        uint32_t n = 1U + (next_byte(b) % 3U);
        put(&b->out, "  cell $pmux $c%u\n", (unsigned)b->cur);
        param(b, "WIDTH", w);
        param(b, "S_WIDTH", n);
        connect(b, "A", w);
        connect(b, "B", w * n);
        connect(b, "S", n);
        break;
    }
    case T_REDUCE_AND:
        render_unary(b, "$reduce_and", 1, true);
        break;
    case T_REDUCE_OR:
        render_unary(b, "$reduce_or", 1, true);
        break;
    case T_REDUCE_XOR:
        render_unary(b, "$reduce_xor", 1, true);
        break;
    case T_LOGIC_NOT:
        render_unary(b, "$logic_not", 1, true);
        break;
    case T_DFF:
        render_ff(b, "$dff", w);
        connect(b, "CLK", 1);
        connect(b, "D", w);
        break;
    case T_DFFE:
        render_ff(b, "$dffe", w);
        param(b, "EN_POLARITY", bit_of(b));
        connect(b, "CLK", 1);
        connect(b, "EN", 1);
        connect(b, "D", w);
        break;
    case T_SDFF:
        render_ff(b, "$sdff", w);
        param(b, "SRST_POLARITY", bit_of(b));
        value_param(b, "SRST_VALUE", w);
        connect(b, "CLK", 1);
        connect(b, "SRST", 1);
        connect(b, "D", w);
        break;
    case T_ADFF:
        render_ff(b, "$adff", w);
        param(b, "ARST_POLARITY", bit_of(b));
        value_param(b, "ARST_VALUE", w);
        connect(b, "CLK", 1);
        connect(b, "ARST", 1);
        connect(b, "D", w);
        break;
    default:
        render_ff(b, "$sdffe", w);
        param(b, "EN_POLARITY", bit_of(b));
        param(b, "SRST_POLARITY", bit_of(b));
        value_param(b, "SRST_VALUE", w);
        connect(b, "CLK", 1);
        connect(b, "EN", 1);
        connect(b, "SRST", 1);
        connect(b, "D", w);
        break;
    }
    put(&b->out, "    connect \\%s \\n%u\n  end\n", is_ff(r->type % N_TYPES) ? "Q" : "Y",
        (unsigned)b->cur);
}

/* The width of record i's output wire: Y_WIDTH (known from the bytes' first pass) or WIDTH. */
static uint32_t out_width(const rec *r) {
    uint32_t t = r->type % N_TYPES;
    if (t >= T_REDUCE_AND && t <= T_LOGIC_NOT) {
        return 1;
    }
    if (t >= T_EQ && t <= T_GE) {
        return 1U + (r->bytes[0] % 2U);
    }
    return r->width;
}

/* Decodes the payload into b (false when it holds no cell). */
static bool decode(build *b, const uint8_t *data, size_t size) {
    memset(b, 0, sizeof *b);
    if (size < 1) {
        return false;
    }
    b->n_inputs = MIN_INPUTS + (data[0] % (MAX_INPUTS - MIN_INPUTS + 1));
    size_t at = 1;
    for (uint32_t i = 0; i < b->n_inputs; i++) {
        b->in_width[i] = (uint8_t)(1U + (at < size ? data[at] % MAX_WIDTH : 0U));
        at++;
    }
    while (at < size && b->n_recs < MAX_CELLS) {
        uint8_t raw[RECORD_BYTES] = {0};
        size_t take = size - at < RECORD_BYTES ? size - at : RECORD_BYTES;
        memcpy(raw, data + at, take);
        at += take;
        rec *r = &b->recs[b->n_recs];
        r->type = raw[0];
        r->width = (uint8_t)(1U + ((raw[1] & 0x7FU) % MAX_WIDTH));
        r->output = (raw[1] & 0x80U) != 0;
        memcpy(r->bytes, raw + 2, OPERAND_BYTES);
        b->widths[b->n_recs] = out_width(r);
        b->n_recs++;
    }
    return b->n_recs > 0;
}

/* The RTLIL text of the decoded build: the inputs i0.., one wire n<j> per record (an output
 * port when flagged, and the last one always), then the cells. */
static bool render(build *b) {
    text *out = &b->out;
    put(out, "attribute \\top 1\nmodule \\t\n");
    unsigned port = 1;
    for (uint32_t i = 0; i < b->n_inputs; i++) {
        put(out, "  wire width %u input %u \\i%u\n", (unsigned)b->in_width[i], port++, (unsigned)i);
    }
    for (uint32_t j = 0; j < b->n_recs; j++) {
        if (b->recs[j].output || j + 1 == b->n_recs) {
            put(out, "  wire width %u output %u \\n%u\n", (unsigned)b->widths[j], port++,
                (unsigned)j);
        } else {
            put(out, "  wire width %u \\n%u\n", (unsigned)b->widths[j], (unsigned)j);
        }
    }
    for (b->cur = 0; b->cur < b->n_recs; b->cur++) {
        render_cell(b);
    }
    put(out, "end\n");
    return !out->full;
}

static void write_file(const char *path, const void *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || (len > 0 && fwrite(bytes, 1, len, fp) != len) || fclose(fp) != 0) {
        fail("cannot write a file in the temporary directory");
    }
}

/* Builds the design of the payload; false when it holds no cell or the text is refused. */
static bool build_design(const uint8_t *data, size_t size, pb_result *res) {
    build *b = calloc(1, sizeof *b);
    bool ok = b != NULL && decode(b, data, size) && render(b);
    const char *dump = getenv("FUZZ_OPT_DUMP"); /* debug aid: the RTLIL text of the last build */
    if (ok && dump != NULL) {
        write_file(dump, b->out.buf, b->out.len);
    }
    ok = ok && pb_build(b->out.buf, res) == ODIN3_OK;
    free(b);
    return ok;
}

/* --- one input ----------------------------------------------------------------------------- */

static void check_full(odin3_design *design, const char *what) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    if (odin3_check_design(design, opts) != ODIN3_OK) {
        fail(what);
    }
}

static void lower_caps(unsigned mode) {
    if ((mode & MODE_LOW_CAPS) == 0) {
        return;
    }
    odin3_opt_limits lim = {LOW_ITERATIONS, LOW_MUXTREE_EVALS};
    odin3_opt_test_set_limits(&lim);
}

/* `opt <spelling>` with the invariants of the header; --opt-simcheck in a Debug build, unless a
 * failure is injected (a failed sub-pass is not compared). */
static void run_opt(odin3_design *design, unsigned mode) {
    long nth = (long)((mode >> MODE_FAIL_SHIFT) & MODE_FAIL_MASK);
    char args[ARGS_BYTES];
#ifndef NDEBUG
    const char *sim = nth == 0 ? " --opt-simcheck" : "";
#else
    const char *sim = "";
#endif
    int len = snprintf(args, sizeof args, "%s%s", k_spellings[mode & MODE_SPELLING_MASK], sim);
    if (len < 0 || (size_t)len >= sizeof args) {
        fail("the argument string does not fit");
    }
    lower_caps(mode);
    if (nth != 0) {
        odin3_util_set_alloc_fail_after(FAIL_SCALE * nth * nth * nth);
    }
    odin3_status st = odin3_pass_run(design, "opt", odin3_bytes_cstr(args));
    odin3_util_set_alloc_fail_after(-1);
    odin3_opt_test_set_limits(NULL);
    if (st != ODIN3_OK && !(st == ODIN3_ERR_NO_MEMORY && nth != 0)) {
        fail("opt returned neither OK nor (under an injected failure) NO_MEMORY");
    }
    check_full(design, "odin3_check_design FULL failed after opt");
}

static void run_blif(const uint8_t *data, size_t size, unsigned mode) {
    write_file(blif_path, data, size);
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        return;
    }
    if (odin3_pass_run(design, "read_blif", odin3_bytes_cstr(blif_path)) == ODIN3_OK) {
        check_full(design, "odin3_check_design FULL failed after read_blif");
        run_opt(design, mode);
    }
    odin3_design_destroy(design);
}

static void run_builder(const uint8_t *data, size_t size, unsigned mode) {
    pb_result res = {0};
    if (build_design(data, size, &res)) {
        check_full(res.design, "odin3_check_design FULL failed on the built design");
        run_opt(res.design, mode);
    }
    pb_free(&res);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    odin3_log_set_sink(quiet_sink, NULL);
    odin3_log_set_level(ODIN3_LOG_ERROR);
    unsigned mode = data[0];
    if ((mode & MODE_BUILDER) != 0) {
        run_builder(data + 1, size - 1, mode);
    } else {
        run_blif(data + 1, size - 1, mode);
    }
    odin3_log_set_sink(NULL, NULL);
    return 0;
}
