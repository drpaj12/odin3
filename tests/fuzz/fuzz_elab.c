/*
 * fuzz_elab.c — the elaborator's crash fuzzer (2D spec §14.5, plan Task 28), a libFuzzer target
 * built with -DODIN3_FUZZ=ON under clang, ASan and UBSan.
 *
 * Input: one mode byte, then the payload, which is one Verilog file: bytes → preprocessor →
 * parser → elaboration (stage 1, binding, stage 2), all through the `read_verilog` pass as
 * `read_verilog [--nolatches] [--nomem2reg] in.v`. Bit 0 of the mode byte is --nolatches, bit 1
 * --nomem2reg, bits 2-4 inject an allocation failure (0: none). Every elaboration cap is lowered
 * by odin3_elab_test_set_limits so the bounded worst case of one input fits libFuzzer's RSS
 * limit and timeout (the preprocessor and parser keep their defaults: fuzz_pp and fuzz_verilog
 * lower and fuzz those, and -max_len bounds the input). A fresh design per input.
 *
 * Invariants (a violation is __builtin_trap(), which libFuzzer reports; tests/ only):
 *  - the status is OK, PARSE or (only under an injected failure) NO_MEMORY; never CHECK (a 2D
 *    bug) or INVALID_ARG;
 *  - on OK: odin3_check_design FULL passes;
 *  - on PARSE: 1 to 20 located error diagnostics (`name:line:col: error:`), and no other error
 *    line but the pass manager's own `pass read_verilog: failed` and the single line a missing
 *    or ambiguous top may be (counted as `unlocated`, at most one);
 *  - no leak, no sanitizer report, no hang (libFuzzer's -rss_limit_mb and -timeout).
 * Under an injected failure only the status rule and the OK-path check apply.
 *
 * The seeds come from tools/elab-check/elab_fuzz_seeds.py: the .v files of tests/elab/rows and
 * the micros' common/ directory, each after mode byte 0x01, smallest first, then ten generated
 * modules (verilog_gen.py --elab).
 */
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/log.h"
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
    MODE_NOLATCHES = 1 << 0,
    MODE_NOMEM2REG = 1 << 1,
    MODE_FAIL_SHIFT = 2,
    MODE_FAIL_MASK = 7,
    FAIL_SCALE = 8, /* the injected failure: after FAIL_SCALE * n^3 allocations */
    MAX_ERRORS = 20,
    LAST_LOG = 512
};

static char dir[PATH_MAX_LEN];
static char input[PATH_MAX_LEN];

/* What the log sink saw during one run. */
typedef struct sink_count {
    uint32_t errors;    /* located error lines (the cap's stop line included) */
    uint32_t unlocated; /* error lines without a usable name:line:col, the manager's own aside */
    char last[LAST_LOG];
} sink_count;

static sink_count g_count;

static void fail(const char *what) {
    (void)fprintf(stderr, "fuzz_elab: %s (errors %u, unlocated %u; last: %s)\n", what,
                  (unsigned)g_count.errors, (unsigned)g_count.unlocated, g_count.last);
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

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    sink_count *count = user;
    size_t len = strnlen(msg, sizeof count->last - 1); /* the last message, cut to fit */
    memcpy(count->last, msg, len);
    count->last[len] = '\0';
    if (level != ODIN3_LOG_ERROR) {
        return;
    }
    if (is_located(msg)) {
        count->errors++;
    } else if (strncmp(msg, "pass read_verilog: failed", 25) != 0) {
        count->unlocated++;
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
    join_path(dir, base != NULL && base[0] != '\0' ? base : "/tmp", "/fuzz_elab_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        fail("cannot create the temporary directory");
    }
    join_path(input, dir, "/in.v");
    (void)atexit(remove_files);
    return 0;
}

/* --- the caps ------------------------------------------------------------------------------ */

/* Every cap of §11, far below its default: a bomb stops early. Each is clamped to its default. */
static void lower_caps(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_errors = ODIN3_ELAB_MAX_ERRORS;
    lim.max_width = 1U << 12;
    lim.max_cell_pins = 1U << 16;
    lim.max_loop_iterations = 1U << 8;
    lim.max_total_iterations = 1U << 12;
    lim.max_eval_steps = 1U << 16;
    lim.max_call_depth = 32;
    lim.max_inline_depth = 16;
    lim.max_loop_objects = 1U << 14;
    lim.max_reentry_depth = 16;
    lim.max_hier_depth = 16;
    lim.max_specializations = 1U << 8;
    lim.max_instances = 1U << 12;
    lim.max_elab_nodes = 1U << 17;
    lim.max_net_bits = 1U << 16;
    lim.max_init_bits = 1U << 16;
    lim.max_readmem_bytes = 1U << 16;
    lim.max_eval_work = 1U << 20;
    lim.max_memory_bits = 1U << 16;
    lim.max_bytes = (uint64_t)1 << 28;
    odin3_elab_test_set_limits(&lim);
}

/* --- one input ----------------------------------------------------------------------------- */

static void check_ok(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    if (odin3_check_design(design, opts) != ODIN3_OK) {
        fail("odin3_check_design FULL failed on an OK elaboration");
    }
}

static void check_parse_errors(void) {
    if (g_count.unlocated > 1) {
        fail("unlocated error lines on PARSE (INVALID_ARG or a check failure)");
    }
    if (g_count.errors + g_count.unlocated == 0 || g_count.errors > MAX_ERRORS + 1) {
        fail("PARSE without 1 to 20 located error diagnostics");
    }
}

static void run_file(unsigned mode) {
    memset(&g_count, 0, sizeof g_count);
    odin3_log_set_sink(count_sink, &g_count);
    odin3_log_set_level(ODIN3_LOG_ERROR);
    lower_caps();
    odin3_design *design = odin3_design_create();
    long nth = (long)((mode >> MODE_FAIL_SHIFT) & MODE_FAIL_MASK);
    char args[PATH_MAX_LEN + 64];
    int len =
        snprintf(args, sizeof args, "%s%s\"%s\"", (mode & MODE_NOLATCHES) ? "--nolatches " : "",
                 (mode & MODE_NOMEM2REG) ? "--nomem2reg " : "", input);
    if (len < 0 || (size_t)len >= sizeof args) {
        fail("the argument string does not fit");
    }
    if (nth != 0) {
        odin3_util_set_alloc_fail_after(FAIL_SCALE * nth * nth * nth);
    }
    odin3_status st = design != NULL
                          ? odin3_pass_run(design, "read_verilog", odin3_bytes_cstr(args))
                          : ODIN3_ERR_NO_MEMORY;
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        if (design != NULL) {
            check_ok(design);
        }
    } else if (st == ODIN3_ERR_PARSE) {
        if (nth == 0) {
            check_parse_errors();
        }
    } else if (!(st == ODIN3_ERR_NO_MEMORY && nth != 0)) {
        fail("the read returned neither OK nor PARSE");
    }
    odin3_design_destroy(design);
    odin3_elab_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    write_file(input, data + 1, size - 1);
    run_file(data[0]);
    return 0;
}
