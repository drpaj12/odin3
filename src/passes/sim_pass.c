/* sim_pass.c — the `sim` pass: random-vector simulation of the design's top module (1E). */
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/vectors.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DECIMAL = 10, NUM_BUF = 32 };

/* sim's arguments: `<cycles> <seed> [<file>]` (file "-" is standard output). */
typedef struct sim_args {
    uint32_t cycles;
    uint64_t seed;
    odin3_strbuf path; /* empty: log a summary instead of writing vectors */
    const char *top;   /* the top module's name */
} sim_args;

/* A decimal number of at most max (digits only). */
static bool parse_number(odin3_bytes word, uint64_t max, uint64_t *out) {
    char text[NUM_BUF] = {0};
    if (word.ptr == NULL || word.len == 0 || word.len >= sizeof text) {
        return false;
    }
    memcpy(text, word.ptr, word.len);
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long val = strtoull(text, &end, DECIMAL);
    if (errno != 0 || *end != '\0' || val > max) {
        return false;
    }
    *out = (uint64_t)val;
    return true;
}

static odin3_status parse_sim_args(odin3_bytes args, sim_args *out) {
    uint64_t cycles = 0;
    bool ok = parse_number(odin3_pass_arg_next(&args), UINT32_MAX, &cycles) &&
              parse_number(odin3_pass_arg_next(&args), UINT64_MAX, &out->seed);
    odin3_bytes file = odin3_pass_arg_next(&args);
    ok = ok && odin3_pass_arg_next(&args).ptr == NULL && (file.ptr == NULL || file.len > 0);
    if (!ok) {
        odin3_log(ODIN3_LOG_ERROR, "sim: expects <cycles> <seed> [<file>] (decimal numbers)");
        return ODIN3_ERR_INVALID_ARG;
    }
    out->cycles = (uint32_t)cycles;
    return file.ptr != NULL ? odin3_strbuf_append(&out->path, file) : ODIN3_OK;
}

/* Writes the header and every cycle's line to out; false when a line could not be built. */
static odin3_status write_vectors(odin3_sim *sim, const odin3_sim_vectors *run, FILE *out) {
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_status st = odin3_sim_vectors_header(sim, run, &line);
    (void)fputs(st == ODIN3_OK ? line.data : "", out);
    odin3_prng prng;
    odin3_prng_seed(&prng, run->seed);
    for (uint32_t cyc = 0; st == ODIN3_OK && cyc < run->cycles; cyc++) {
        odin3_strbuf_clear(&line);
        st = odin3_sim_vectors_step(sim, &prng, cyc, &line);
        (void)fputs(st == ODIN3_OK ? line.data : "", out);
    }
    odin3_strbuf_free(&line);
    return st;
}

/* Runs every cycle and logs one summary line with the last cycle's vector. */
static odin3_status log_summary(odin3_sim *sim, const odin3_sim_vectors *run, const char *top) {
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_prng prng;
    odin3_prng_seed(&prng, run->seed);
    odin3_status st = ODIN3_OK;
    for (uint32_t cyc = 0; st == ODIN3_OK && cyc < run->cycles; cyc++) {
        odin3_strbuf_clear(&line);
        st = odin3_sim_vectors_step(sim, &prng, cyc, &line);
    }
    if (st == ODIN3_OK && line.len > 0) {
        line.data[--line.len] = '\0'; /* the newline */
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "sim: %s: %u cycles, seed %llu; last cycle: %s", top, run->cycles,
                  (unsigned long long)run->seed, line.len > 0 ? line.data : "none");
    }
    odin3_strbuf_free(&line);
    return st;
}

/* The vectors go to path ("-": standard output) or, without a path, a summary is logged. */
static odin3_status emit(odin3_sim *sim, const odin3_sim_vectors *run, const sim_args *args) {
    if (args->path.len == 0) {
        return log_summary(sim, run, args->top);
    }
    if (strcmp(args->path.data, "-") == 0) {
        odin3_status st = write_vectors(sim, run, stdout);
        return st == ODIN3_OK && fflush(stdout) != 0 ? ODIN3_ERR_IO : st;
    }
    odin3_atomic_file file;
    odin3_status st = odin3_atomic_file_open(&file, args->path.data);
    if (st == ODIN3_OK) {
        st = odin3_atomic_file_close(&file, write_vectors(sim, run, file.fp));
    }
    return st;
}

static odin3_status sim_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                            void *user) {
    (void)ctx;
    (void)user;
    sim_args parsed = {0};
    odin3_strbuf_init(&parsed.path);
    odin3_strbuf title;
    odin3_strbuf_init(&title);
    odin3_sim *sim = NULL;
    odin3_module_id top = odin3_design_top(design);
    odin3_status st = parse_sim_args(args, &parsed);
    if (st == ODIN3_OK && !odin3_module_valid(top)) {
        odin3_log(ODIN3_LOG_ERROR, "sim: the design has no top module (read_blif or hierarchy)");
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK) {
        parsed.top = odin3_strtab_get(odin3_design_strtab(design),
                                      odin3_module_name(odin3_module_get(design, top)));
        st = odin3_strbuf_appendf(&title, "odin3 sim %s", parsed.top);
    }
    if (st == ODIN3_OK) {
        st = odin3_sim_build(design, top, &sim);
    }
    if (st == ODIN3_OK) {
        st = emit(sim, &(odin3_sim_vectors){title.data, parsed.seed, parsed.cycles}, &parsed);
    }
    odin3_sim_destroy(sim);
    odin3_strbuf_free(&title);
    odin3_strbuf_free(&parsed.path);
    return st;
}

const odin3_pass_def odin3_pass_sim = {
    "sim",
    "sim <cycles> <seed> [<file>|-]: simulate the top with random inputs; write the vectors "
    "(odin3-sim-vectors format), or log a summary",
    sim_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
