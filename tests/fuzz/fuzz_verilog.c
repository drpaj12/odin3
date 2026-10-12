/*
 * fuzz_verilog.c — the Verilog parser's crash fuzzer (2C spec §7.5, plan Task 11), a libFuzzer
 * target built with -DODIN3_FUZZ=ON under clang, ASan and UBSan.
 *
 * Input: one mode byte, then the payload. Bit 0 of the mode byte selects the payload's meaning:
 * 0 (raw) — the bytes are one source file, run through the 2B preprocessor and the parser;
 * 1 (token stream) — each byte picks one of the 64 spellings of k_alphabet (keywords, operators,
 * three identifiers, a literal of each form, `(*`, `*)`, `@*`, a `//` comment, a metacomment,
 * `timescale, `;`, `begin`, `end`), joined by blanks, and that text is the file. Bits 1-3 inject
 * an allocation failure (0: none) during preprocessing and parsing. Every cap is lowered by the
 * test hooks (odin3_pp_test_create, odin3_verilog_test_set_limits, odin3_ast_test_set_limits) so
 * the bounded worst case of one input fits libFuzzer's RSS limit and timeout.
 *
 * Invariants (a violation is __builtin_trap(), which libFuzzer reports; tests/ only):
 *  - the status is OK or PARSE (NO_MEMORY only under an injected failure);
 *  - on OK: the sealed store passes odin3_ast_check FULL, and printing the UNIT, re-reading the
 *    printed text (default caps: the printer adds parentheses) and comparing gives equal items;
 *  - on PARSE: 1 to 20 error diagnostics (and at most one "too many errors; stopping"), every
 *    one located (`name:line:col: error:` with line and col >= 1), and no unlocated error line
 *    (a builder's INVALID_ARG, a check failure), counted by the log sink;
 *  - no leak, no sanitizer report, no hang (libFuzzer's -rss_limit_mb and -timeout).
 * Under an injected failure only the status rule and the OK-path checks apply.
 *
 * FUZZ_VERILOG_MODE=raw or =tokens in the environment forces the mode bit (CTest
 * fuzz_verilog_smoke fuzzes each mode for 10 s); the seeds come from tools/verilog-gen/
 * fuzz_seeds.py.
 */
#include "ast/ast.h"
#include "ast/ast_test.h"
#include "ast/check.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/parser_test.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/str.h"

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
    MODE_TOKENS = 1 << 0,
    MODE_FAIL_SHIFT = 1,
    MODE_FAIL_MASK = 7,
    FAIL_SCALE = 4, /* the injected failure: after FAIL_SCALE * n^3 allocations */
    ALPHABET = 64,
    MAX_ERRORS = 20,
    LAST_LOG = 512,
    /* Above any file the harness writes (a raw payload is at most -max_len bytes, a token
     * stream at most MAX_TOKENS spellings of under 40 bytes): a file the cache cannot hold is a
     * file-level error located where the file was named, and the harness names it directly
     * (loc 0, 2B's rule), so the invariant "every error located" needs it never to happen. */
    FILE_CACHE_BYTES = 1 << 22,
    MAX_TOKENS = 1 << 14
};

/* The token-stream mode's spellings: 22 keywords, 23 operators, three identifiers, seven
 * literals, the attribute and implicit-event tokens, two comments, a directive, `;`, begin and
 * end. A comment or directive carries its own newline. */
static const char *const k_alphabet[ALPHABET] = {"module",
                                                 "endmodule",
                                                 "input",
                                                 "output",
                                                 "wire",
                                                 "reg",
                                                 "parameter",
                                                 "assign",
                                                 "always",
                                                 "if",
                                                 "else",
                                                 "case",
                                                 "endcase",
                                                 "default",
                                                 "for",
                                                 "function",
                                                 "endfunction",
                                                 "generate",
                                                 "endgenerate",
                                                 "genvar",
                                                 "posedge",
                                                 "or",
                                                 "(",
                                                 ")",
                                                 "[",
                                                 "]",
                                                 "{",
                                                 "}",
                                                 ",",
                                                 ":",
                                                 "=",
                                                 "<=",
                                                 "+",
                                                 "-",
                                                 "*",
                                                 "**",
                                                 "?",
                                                 "~^",
                                                 "<<",
                                                 ">>>",
                                                 "&&",
                                                 "!",
                                                 "#",
                                                 ".",
                                                 "@",
                                                 "a",
                                                 "b",
                                                 "\\c+d ",
                                                 "8'hF_f",
                                                 "'sb1x?z",
                                                 "42",
                                                 "1.5e3",
                                                 "\"s\\n\"",
                                                 "8 'o 17",
                                                 "4'dx",
                                                 "(*",
                                                 "*)",
                                                 "@*",
                                                 "// c\n",
                                                 "// synopsys full_case parallel_case\n",
                                                 "`timescale 1ns/1ps\n",
                                                 ";",
                                                 "begin",
                                                 "end"};

/* FUZZ_VERILOG_MODE=raw or tokens forces bit 0 of the mode byte (the per-mode smoke runs). */
static int forced_mode = -1;

static char dir[PATH_MAX_LEN];
static char input[PATH_MAX_LEN];
static char again[PATH_MAX_LEN];

/* What the log sink saw during one parse. */
typedef struct sink_count {
    uint32_t errors;    /* located error lines (the stop line taken out after the run) */
    uint32_t stops;     /* the cap's stop line: 1 when the run's cap fired, else 0 */
    uint32_t unlocated; /* error lines without a usable name:line:col */
    bool last_stop;     /* the last located error line has the stop line's exact shape */
    char last[LAST_LOG];
} sink_count;

static sink_count g_count;

static void fail(const char *what) {
    (void)fprintf(stderr, "fuzz_verilog: %s (errors %u, stops %u, unlocated %u; last: %s)\n", what,
                  (unsigned)g_count.errors, (unsigned)g_count.stops, (unsigned)g_count.unlocated,
                  g_count.last);
    __builtin_trap();
}

/* True when the text before ": error: " ends in ":<line>:<col>" with both at least 1. */
static bool is_located(const char *msg) {
    const char *mark = strstr(msg, ": error: ");
    if (mark == NULL) {
        return false;
    }
    const char *at = mark;
    for (int field = 0; field < 2; field++) {
        const char *end = at;
        unsigned long value = 0;
        unsigned long scale = 1;
        while (at > msg && at[-1] >= '0' && at[-1] <= '9') {
            at--;
            value += (unsigned long)(at[0] - '0') * scale;
            scale *= 10;
        }
        if (at == end || value == 0 || at == msg || at[-1] != ':') {
            return false;
        }
        at--;
    }
    return at > msg;
}

/* The stop line's shape: the text after the location's ": error: ", up to the first newline (the
 * expansion notes that may follow), is exactly the stop text. The shape alone never makes a
 * line the stop line (fuzz review C1: a message holds user text, and libFuzzer's CMP hook
 * learns the needle): only the last error line of a run whose cap fired is one (stop_taken). */
static bool is_stop_shape(const char *msg) {
    static const char k_stop[] = "too many errors; stopping";
    static const char k_mark[] = ": error: ";
    const char *mark = strstr(msg, k_mark);
    if (mark == NULL) {
        return false;
    }
    const char *body = mark + sizeof k_mark - 1;
    const char *nl = strchr(body, '\n');
    size_t len = nl != NULL ? (size_t)(nl - body) : strlen(body);
    return len == sizeof k_stop - 1 && memcmp(body, k_stop, len) == 0;
}

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    sink_count *count = user;
    size_t len = strnlen(msg, sizeof count->last - 1); /* the last message, cut to fit */
    memcpy(count->last, msg, len);
    count->last[len] = '\0';
    if (level != ODIN3_LOG_ERROR) {
        return;
    }
    if (!is_located(msg)) {
        count->unlocated++;
    } else {
        count->errors++;
        count->last_stop = is_stop_shape(msg);
    }
}

/* After a run: the stop line is structural, never textual. Only when the run's cap fired (it
 * stopped with max_errors errors counted) and more than max_errors located lines were logged is
 * the last line (nothing is logged once a run stops), if it has the stop shape, the stop line;
 * it moves from errors to stops. Otherwise every line, whatever its text, stays an error. */
static void stop_taken(bool capped) {
    if (capped && g_count.last_stop && g_count.errors > MAX_ERRORS) {
        g_count.errors--;
        g_count.stops = 1;
    }
}

static void write_file(const char *path, const void *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || (len > 0 && fwrite(bytes, 1, len, fp) != len) || fclose(fp) != 0) {
        fail("cannot write a file in the temporary directory");
    }
}

static void remove_files(void) {
    (void)remove(input);
    (void)remove(again);
    (void)rmdir(dir);
}

/* path = prefix + suffix, or a failure when it does not fit. */
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
    join_path(dir, base != NULL && base[0] != '\0' ? base : "/tmp", "/fuzz_verilog_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        fail("cannot create the temporary directory");
    }
    join_path(input, dir, "/in.v");
    join_path(again, dir, "/again.v");
    (void)atexit(remove_files);
    const char *mode = getenv("FUZZ_VERILOG_MODE");
    if (mode != NULL && strcmp(mode, "raw") == 0) {
        forced_mode = 0;
    } else if (mode != NULL && strcmp(mode, "tokens") == 0) {
        forced_mode = MODE_TOKENS;
    }
    return 0;
}

/* --- the caps ------------------------------------------------------------------------------ */

/* The preprocessor's caps, far below the defaults (2B §9): a macro bomb stops early. */
static odin3_pp_limits pp_limits(void) {
    odin3_pp_limits lim = odin3_pp_default_limits();
    lim.include_depth = 4;
    lim.expansion_chain = 64;
    lim.expansion_depth = 64;
    lim.live_runs = 1U << 12;
    lim.cond_depth = 32;
    lim.macro_args = 16;
    lim.macro_text = 1U << 12;
    lim.macros = 256;
    lim.definitions = 1U << 10;
    lim.macro_bytes = 1U << 18; /* 4 arena chunks: the cap keeps one free */
    lim.stream_bytes = 1U << 18;
    lim.buffers_per_stream = 1U << 12;
    lim.includes_per_stream = 16;
    lim.loc_bytes_per_stream = 1U << 20;
    lim.file_cache_bytes = FILE_CACHE_BYTES;
    lim.srcman_bytes_per_stream = 1U << 20;
    return lim;
}

/* The parser's and lexer's caps (2C §5), lowered; max_errors stays at its 20. */
static const odin3_vp_limits k_vp_limits = {
    .yymaxdepth = 4096,
    .keyword_depth = 8,
    .ident_bytes = 256,
    .string_bytes = 1024,
    .text_bytes = 4096,
    .comment_bytes = 1024,
    .directive_bytes = 256,
};

/* The store's caps (2A §10), lowered. */
static const odin3_ast_limits k_ast_limits = {
    .max_children = 1U << 12, .max_nodes = 1U << 18, .max_depth = 1024};

/* --- one read ------------------------------------------------------------------------------ */

/* One file through the preprocessor and the parser into a fresh design and store. */
typedef struct vread {
    odin3_design *design;
    odin3_ast *ast;
    odin3_pp *pp;
    odin3_ast_id unit;
    bool capped; /* the parser's error cap fired: the run stopped at max_errors errors */
} vread;

static void read_free(vread *rd) {
    odin3_pp_destroy(rd->pp);
    odin3_ast_destroy(rd->ast);
    odin3_design_destroy(rd->design);
    memset(rd, 0, sizeof *rd);
}

/* Sets up rd for path; false only when allocation failed under injection. */
static bool read_open(vread *rd, bool lowered) {
    memset(rd, 0, sizeof *rd);
    rd->design = odin3_design_create();
    if (rd->design == NULL) {
        return false;
    }
    if (odin3_ast_create(rd->design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &rd->ast) !=
        ODIN3_OK) {
        return false;
    }
    if (lowered) {
        odin3_ast_test_set_limits(rd->ast, k_ast_limits);
    }
    odin3_pp_config config = {NULL, 0, NULL, 0};
    odin3_pp_limits lim = lowered ? pp_limits() : odin3_pp_default_limits();
    return odin3_pp_test_create(rd->design, &config, lim, &rd->pp) == ODIN3_OK;
}

/* Preprocesses and parses path (given as "in.v") into rd. */
static odin3_status read_run(vread *rd, const char *path, bool lowered) {
    odin3_pp_source src = {0, 0, 0, {0}};
    if (odin3_design_intern(rd->design, odin3_bytes_cstr("in.v"), &src.given) != ODIN3_OK ||
        odin3_design_intern(rd->design, odin3_bytes_cstr(path), &src.resolved) != ODIN3_OK ||
        odin3_design_intern(rd->design, odin3_bytes_cstr("work"), &src.library) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vl_run run;
    odin3_vl_run_init(&run, rd->design);
    if (lowered) {
        odin3_verilog_test_set_limits(&run, k_vp_limits);
    }
    odin3_pp_stream stream;
    odin3_status st = odin3_pp_run(rd->pp, &src, &stream);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vp_stream vs = {stream.text, stream.stream, src.given};
    st = odin3_verilog_parse_stream(&run, rd->ast, &vs, &rd->unit);
    rd->capped = run.stop && run.errors >= run.limits.max_errors;
    return st;
}

/* --- the OK path: check, print, re-read, equal --------------------------------------------- */

/* Whether name is a UDP_DECL among the UNIT's items. */
static bool is_udp(const vread *rd, uint32_t name) {
    odin3_ast_span items = odin3_ast_children(rd->ast, rd->unit);
    for (uint32_t i = 0; i < items.n; i++) {
        if (odin3_ast_kind_of(rd->ast, items.ids[i]) == ODIN3_AST_UDP_DECL &&
            odin3_ast_name(rd->ast, items.ids[i]) == name) {
            return true;
        }
    }
    return false;
}

/* The read_verilog pass's step 5 (2C §4.2; 2A check rule 4), which the parser leaves to it: an
 * unnamed INSTANCE of anything but a UDP of the store is the pass's located error, so such a
 * parse is the pass's PARSE, not a store to check. */
static bool pass_rejects_unnamed(const vread *rd) {
    uint32_t end = odin3_ast_node_end(rd->ast).v;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(rd->ast, node) != ODIN3_AST_INSTANTIATION ||
            is_udp(rd, odin3_ast_name(rd->ast, node))) {
            continue;
        }
        odin3_ast_span kids = odin3_ast_children(rd->ast, node);
        for (uint32_t i = 1; i < kids.n; i++) {
            if (odin3_ast_name(rd->ast, kids.ids[i]) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool items_equal(const vread *first, const vread *second) {
    uint32_t count = odin3_ast_nchild(first->ast, first->unit);
    if (count != odin3_ast_nchild(second->ast, second->unit)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        bool equal = false;
        if (odin3_ast_equal(first->ast, odin3_ast_child(first->ast, first->unit, i), second->ast,
                            odin3_ast_child(second->ast, second->unit, i), &equal) != ODIN3_OK ||
            !equal) {
            return false;
        }
    }
    return true;
}

static void check_round_trip(vread *first) {
    if (odin3_ast_finish(first->ast) != ODIN3_OK) {
        fail("odin3_ast_finish failed on a parsed store");
    }
    if (odin3_ast_check(first->ast, ODIN3_CHECK_FULL) != ODIN3_OK) {
        fail("odin3_ast_check FULL failed on an OK parse");
    }
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    if (odin3_ast_print(first->ast, first->unit, &printed) != ODIN3_OK) {
        fail("odin3_ast_print failed on an OK parse");
    }
    write_file(again, printed.data, printed.len);
    vread second;
    if (!read_open(&second, false) || read_run(&second, again, false) != ODIN3_OK) {
        (void)fprintf(stderr, "printed text:\n%.*s\n", (int)printed.len,
                      printed.data != NULL ? printed.data : "");
        fail("the printed text does not re-read");
    }
    if (odin3_ast_finish(second.ast) != ODIN3_OK ||
        odin3_ast_check(second.ast, ODIN3_CHECK_FULL) != ODIN3_OK) {
        fail("the re-read store fails odin3_ast_check FULL");
    }
    if (!items_equal(first, &second)) {
        (void)fprintf(stderr, "printed text:\n%.*s\n", (int)printed.len,
                      printed.data != NULL ? printed.data : "");
        fail("the re-read items are not equal to the first read's");
    }
    read_free(&second);
    odin3_strbuf_free(&printed);
}

/* --- one input ----------------------------------------------------------------------------- */

static void check_parse_errors(const vread *rd, bool injected) {
    if (injected) {
        return;
    }
    stop_taken(rd->capped);
    if (g_count.unlocated != 0) {
        fail("an unlocated error line on PARSE (INVALID_ARG or a check failure)");
    }
    if (g_count.errors == 0 || g_count.errors > MAX_ERRORS) {
        fail("PARSE without 1 to 20 located error diagnostics");
    }
}

static void run_file(bool injected, long fail_after) {
    memset(&g_count, 0, sizeof g_count);
    odin3_log_set_sink(count_sink, &g_count);
    vread first;
    if (injected) {
        odin3_util_set_alloc_fail_after(fail_after);
    }
    odin3_status st = read_open(&first, true) ? read_run(&first, input, true) : ODIN3_ERR_NO_MEMORY;
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        if (!injected && (g_count.errors != 0 || g_count.unlocated != 0)) {
            fail("an error line on an OK parse");
        }
        if (!pass_rejects_unnamed(&first)) {
            check_round_trip(&first);
        }
    } else if (st == ODIN3_ERR_PARSE) {
        check_parse_errors(&first, injected);
    } else if (!(st == ODIN3_ERR_NO_MEMORY && injected)) {
        fail("the read returned neither OK nor PARSE");
    }
    read_free(&first);
    odin3_log_set_sink(NULL, NULL);
}

/* The token-stream payload as text: one spelling per byte, joined by blanks. */
static void write_tokens(const uint8_t *data, size_t size) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (size_t i = 0; i < size && i < MAX_TOKENS; i++) {
        if (odin3_strbuf_append(&text, odin3_bytes_cstr(k_alphabet[data[i] % ALPHABET])) !=
                ODIN3_OK ||
            odin3_strbuf_append(&text, odin3_bytes_cstr(" ")) != ODIN3_OK) {
            fail("out of memory building the token stream");
        }
    }
    write_file(input, text.data, text.len);
    odin3_strbuf_free(&text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    unsigned mode = data[0];
    if (forced_mode >= 0) {
        mode = (mode & ~(unsigned)MODE_TOKENS) | (unsigned)forced_mode;
    }
    if (mode & MODE_TOKENS) {
        write_tokens(data + 1, size - 1);
    } else {
        write_file(input, data + 1, size - 1);
    }
    long nth = (long)((mode >> MODE_FAIL_SHIFT) & MODE_FAIL_MASK);
    run_file(nth != 0, FAIL_SCALE * nth * nth * nth);
    return 0;
}
