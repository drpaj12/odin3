/* sim_tb_pass.c — the `sim_tb` pass: runs a Verilog testbench (subset, spec
 * docs/specs/2026-10-31-testbench-design.md) against the design's modules. */
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "sim/tb.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PATH_MAX_LEN = 1024, DECIMAL = 10, DEFAULT_MAX_TIME = 10000000, NUM_BUF = 32 };

typedef struct tb_args {
    char file[PATH_MAX_LEN];
    char top[TB_NAME];
    char out[PATH_MAX_LEN];
    char vcd[PATH_MAX_LEN];
    uint64_t max_time;
    uint64_t seed;
} tb_args;

static bool word_is(odin3_bytes word, const char *text) {
    return word.len == strlen(text) && memcmp(word.ptr, text, word.len) == 0;
}

static bool take(odin3_bytes *args, char *dst, size_t cap) {
    odin3_bytes word = odin3_pass_arg_next(args);
    if (word.ptr == NULL || word.len == 0 || word.len >= cap) {
        return false;
    }
    memcpy(dst, word.ptr, word.len);
    dst[word.len] = '\0';
    return true;
}

static bool take_number(odin3_bytes *args, uint64_t *out) {
    char text[NUM_BUF] = "";
    char *end = NULL;
    if (!take(args, text, sizeof text)) {
        return false;
    }
    errno = 0;
    *out = strtoull(text, &end, DECIMAL);
    return errno == 0 && *end == '\0';
}

static odin3_status usage(void) {
    odin3_log(ODIN3_LOG_ERROR,
              "sim_tb: expects [--top <module>] [--max-time N] [--seed N] [--out <file>] [--vcd <file>] <tb.v>");
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status parse_args(odin3_bytes args, tb_args *out) {
    out->max_time = DEFAULT_MAX_TIME;
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool ok = true;
        if (word_is(word, "--top")) {
            ok = take(&args, out->top, sizeof out->top);
        } else if (word_is(word, "--out")) {
            ok = take(&args, out->out, sizeof out->out);
        } else if (word_is(word, "--max-time")) {
            ok = take_number(&args, &out->max_time);
        } else if (word_is(word, "--seed")) {
            ok = take_number(&args, &out->seed);
        } else if (word_is(word, "--vcd")) {
            ok = take(&args, out->vcd, sizeof out->vcd);
        } else if (out->file[0] == '\0' && word.len < sizeof out->file) {
            memcpy(out->file, word.ptr, word.len);
            out->file[word.len] = '\0';
        } else {
            ok = false;
        }
        if (!ok) {
            return usage();
        }
    }
    return out->file[0] == '\0' ? usage() : ODIN3_OK;
}

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    char *buf = size < 0 ? NULL : malloc((size_t)size + 1U);
    if (buf != NULL) {
        size_t got = fread(buf, 1, (size_t)size, fp);
        buf[got] = '\0';
    }
    (void)fclose(fp);
    return buf;
}

static odin3_status sim_tb_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                               void *user) {
    (void)ctx;
    (void)user;
    tb_args parsed = {{0}, {0}, {0}, {0}, 0, 0};
    odin3_status st = parse_args(args, &parsed);
    if (st != ODIN3_OK) {
        return st;
    }
    char *text = read_text(parsed.file);
    if (text == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot read `%s`", parsed.file);
        return ODIN3_ERR_IO;
    }
    tb_module *tb = NULL;
    tb_source src = {parsed.file, text};
    st = tb_parse(&src, parsed.top[0] != '\0' ? parsed.top : NULL, &tb);
    free(text);
    if (st != ODIN3_OK) {
        return st;
    }
    FILE *out = parsed.out[0] != '\0' ? fopen(parsed.out, "w") : stdout;
    if (out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot write `%s`", parsed.out);
        tb_module_free(tb);
        return ODIN3_ERR_IO;
    }
    tb_options opts = {parsed.max_time, parsed.seed, out,
                       parsed.vcd[0] != '\0' ? parsed.vcd : NULL};
    uint64_t end_time = 0;
    st = tb_run(design, tb, &opts, &end_time);
    (void)fflush(out);
    if (out != stdout) {
        (void)fclose(out);
    }
    tb_module_free(tb);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "sim_tb: finished at time %llu", (unsigned long long)end_time);
    }
    return st;
}

const odin3_pass_def odin3_pass_sim_tb = {
    "sim_tb",
    "sim_tb [--top <module>] [--max-time N] [--seed N] [--out <file>] [--vcd <file>] <tb.v>: run a Verilog "
    "testbench (the supported subset, docs/specs/2026-10-31-testbench-design.md) against the "
    "design's modules; $display text goes to stdout or --out, "
    "--vcd writes a waveform of the testbench signals and the DUT ports",
    sim_tb_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
