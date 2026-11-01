/* sim_pass.c — the `sim` pass: random-vector simulation of the design's top module (1E). */
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "sim/vcd.h"
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

/*
 * sim's arguments: `[--vectors-in <file>] [--vcd <file>] <cycles> <seed> [<file>|-]`, or with
 * --vectors-in `[--vcd <file>] [<file>|-]` (SV-1).
 */
typedef struct sim_args {
    uint32_t cycles;
    uint64_t seed;
    odin3_strbuf path;       /* empty: log a summary instead of writing vectors */
    odin3_strbuf vectors_in; /* empty: random inputs */
    odin3_strbuf vcd;        /* empty: no waveform */
    const char *top;         /* the top module's name */
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

enum { MAX_POSITIONALS = 3, MIN_RANDOM_POSITIONALS = 2 };

static odin3_status usage_error(void) {
    odin3_log(ODIN3_LOG_ERROR,
              "sim: expects <cycles> <seed> [<file>|-], or --vectors-in <file> [<file>|-]; "
              "option --vcd <file> (decimal numbers)");
    return ODIN3_ERR_INVALID_ARG;
}

static bool word_is(odin3_bytes word, const char *text) {
    return word.ptr != NULL && word.len == strlen(text) && memcmp(word.ptr, text, word.len) == 0;
}

/* The file name after an option goes into dst; an option given twice or without one is false. */
static bool take_option_file(odin3_bytes *args, odin3_strbuf *dst) {
    odin3_bytes file = odin3_pass_arg_next(args);
    return dst->len == 0 && file.ptr != NULL && file.len > 0 &&
           odin3_strbuf_append(dst, file) == ODIN3_OK;
}

/* Splits the words into the options and up to MAX_POSITIONALS positional words. */
static odin3_status split_args(odin3_bytes args, sim_args *out, odin3_bytes *pos, size_t *npos) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool ok = true;
        if (word_is(word, "--vectors-in")) {
            ok = take_option_file(&args, &out->vectors_in);
        } else if (word_is(word, "--vcd")) {
            ok = take_option_file(&args, &out->vcd);
        } else if (*npos < MAX_POSITIONALS && word.len > 0) {
            pos[(*npos)++] = word;
        } else {
            ok = false;
        }
        if (!ok) {
            return usage_error();
        }
    }
    return ODIN3_OK;
}

/* Without --vectors-in: `<cycles> <seed> [<file>]` from the positional words. */
static odin3_status random_args(const odin3_bytes *pos, size_t npos, sim_args *out) {
    uint64_t cycles = 0;
    if (npos < MIN_RANDOM_POSITIONALS || !parse_number(pos[0], UINT32_MAX, &cycles) ||
        !parse_number(pos[1], UINT64_MAX, &out->seed)) {
        return usage_error();
    }
    out->cycles = (uint32_t)cycles;
    return npos > MIN_RANDOM_POSITIONALS ? odin3_strbuf_append(&out->path, pos[2]) : ODIN3_OK;
}

static odin3_status parse_sim_args(odin3_bytes args, sim_args *out) {
    odin3_bytes pos[MAX_POSITIONALS] = {{NULL, 0}};
    size_t npos = 0;
    odin3_status st = split_args(args, out, pos, &npos);
    if (st != ODIN3_OK) {
        return st;
    }
    if (out->vcd.len == 1 && out->vcd.data[0] == '-') {
        odin3_log(ODIN3_LOG_ERROR, "sim: --vcd needs a file name, not `-`");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (out->vectors_in.len == 0) {
        return random_args(pos, npos, out);
    }
    if (npos >= MIN_RANDOM_POSITIONALS) {
        odin3_log(ODIN3_LOG_ERROR, "sim: --vectors-in replaces <cycles> <seed>");
        return ODIN3_ERR_INVALID_ARG;
    }
    return npos == 1 ? odin3_strbuf_append(&out->path, pos[0]) : ODIN3_OK;
}

/* Where each cycle's inputs come from: the PRNG, or the parsed vector file when in is set. */
typedef struct input_source {
    odin3_prng prng;
    const odin3_sim_vectors_in *in;
    odin3_sim_vcd *vcd; /* the waveform writer; NULL without --vcd */
} input_source;

/* One cycle of the source's inputs; appends its line to line. */
static odin3_status source_step(odin3_sim *sim, input_source *src, uint32_t cyc,
                                odin3_strbuf *line) {
    if (src->in == NULL && src->vcd == NULL) {
        return odin3_sim_vectors_step(sim, &src->prng, cyc, line);
    }
    if (src->in == NULL) {
        odin3_sim_drive_random(sim, &src->prng);
    } else {
        odin3_sim_vectors_drive(sim, src->in, cyc);
    }
    odin3_status st = ODIN3_OK;
    if (src->vcd == NULL) {
        st = odin3_sim_cycle(sim);
    } else {
        st = odin3_sim_vcd_cycle(src->vcd, sim, cyc);
    }
    return st == ODIN3_OK ? odin3_sim_vectors_line(sim, cyc, line) : st;
}

/* Writes the header and every cycle's line to out; false when a line could not be built. */
static odin3_status write_vectors(odin3_sim *sim, const odin3_sim_vectors *run, input_source *src,
                                  FILE *out) {
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_status st = odin3_sim_vectors_header(sim, run, &line);
    (void)fputs(st == ODIN3_OK ? line.data : "", out);
    for (uint32_t cyc = 0; st == ODIN3_OK && cyc < run->cycles; cyc++) {
        odin3_strbuf_clear(&line);
        st = source_step(sim, src, cyc, &line);
        (void)fputs(st == ODIN3_OK ? line.data : "", out);
    }
    odin3_strbuf_free(&line);
    return st;
}

/* Runs every cycle and logs one summary line with the last cycle's vector. */
static odin3_status log_summary(odin3_sim *sim, const odin3_sim_vectors *run, input_source *src,
                                const sim_args *args) {
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_status st = ODIN3_OK;
    for (uint32_t cyc = 0; st == ODIN3_OK && cyc < run->cycles; cyc++) {
        odin3_strbuf_clear(&line);
        st = source_step(sim, src, cyc, &line);
    }
    if (st == ODIN3_OK && line.len > 0) {
        line.data[--line.len] = '\0'; /* the newline */
    }
    if (st == ODIN3_OK && src->in != NULL) {
        odin3_log(ODIN3_LOG_INFO, "sim: %s: %u cycles from %s; last cycle: %s", args->top,
                  run->cycles, args->vectors_in.data, line.len > 0 ? line.data : "none");
    } else if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "sim: %s: %u cycles, seed %llu; last cycle: %s", args->top,
                  run->cycles, (unsigned long long)run->seed, line.len > 0 ? line.data : "none");
    }
    odin3_strbuf_free(&line);
    return st;
}

/* The vectors go to path ("-": standard output) or, without a path, a summary is logged. */
static odin3_status emit(odin3_sim *sim, const odin3_sim_vectors *run, input_source *src,
                         const sim_args *args) {
    if (args->path.len == 0) {
        return log_summary(sim, run, src, args);
    }
    if (strcmp(args->path.data, "-") == 0) {
        odin3_status st = write_vectors(sim, run, src, stdout);
        return st == ODIN3_OK && fflush(stdout) != 0 ? ODIN3_ERR_IO : st;
    }
    odin3_atomic_file file;
    odin3_status st = odin3_atomic_file_open(&file, args->path.data);
    if (st == ODIN3_OK) {
        st = odin3_atomic_file_close(&file, write_vectors(sim, run, src, file.fp));
    }
    return st;
}

/* Reads --vectors-in (when given) into in; run takes its cycles and seed. */
static odin3_status load_inputs(const odin3_sim *sim, const sim_args *args,
                                odin3_sim_vectors_in *in, odin3_sim_vectors *run) {
    if (args->vectors_in.len == 0) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_sim_vectors_read(sim, args->vectors_in.data, in);
    run->cycles = in->cycles;
    run->seed = in->seed;
    return st;
}

/* Runs emit, with the waveform on an atomic file when --vcd was given. */
static odin3_status run_vcd(odin3_sim *sim, const odin3_sim_vectors *run, input_source *src,
                            const sim_args *args) {
    if (args->vcd.len == 0) {
        return emit(sim, run, src, args);
    }
    odin3_sim_signals *sig = NULL;
    odin3_atomic_file file;
    odin3_sim_vcd vcd = {0};
    odin3_status st = odin3_sim_signals_create(sim, &sig);
    if (st == ODIN3_OK) {
        st = odin3_atomic_file_open(&file, args->vcd.data);
    }
    if (st != ODIN3_OK) {
        odin3_sim_signals_destroy(sig);
        return st;
    }
    st = odin3_sim_vcd_begin(&vcd, sig, file.fp);
    if (st == ODIN3_OK) {
        src->vcd = &vcd;
        st = emit(sim, run, src, args);
        src->vcd = NULL;
    }
    odin3_sim_vcd_free(&vcd);
    odin3_sim_signals_destroy(sig);
    return odin3_atomic_file_close(&file, st);
}

static odin3_status run_sim(odin3_sim *sim, const sim_args *args, const odin3_strbuf *title) {
    odin3_sim_vectors run = {title->data, args->seed, args->cycles};
    odin3_sim_vectors_in in = {0};
    odin3_status st = load_inputs(sim, args, &in, &run);
    if (st == ODIN3_OK) {
        input_source src = {{0}, args->vectors_in.len > 0 ? &in : NULL, NULL};
        odin3_prng_seed(&src.prng, run.seed);
        st = run_vcd(sim, &run, &src, args);
    }
    odin3_sim_vectors_in_free(&in);
    return st;
}

static odin3_status sim_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                            void *user) {
    (void)ctx;
    (void)user;
    sim_args parsed = {0};
    odin3_strbuf_init(&parsed.path);
    odin3_strbuf_init(&parsed.vectors_in);
    odin3_strbuf_init(&parsed.vcd);
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
        st = run_sim(sim, &parsed, &title);
    }
    odin3_sim_destroy(sim);
    odin3_strbuf_free(&title);
    odin3_strbuf_free(&parsed.path);
    odin3_strbuf_free(&parsed.vectors_in);
    odin3_strbuf_free(&parsed.vcd);
    return st;
}

const odin3_pass_def odin3_pass_sim = {
    "sim",
    "sim [--vectors-in <file>] [--vcd <file>] <cycles> <seed> [<file>|-]: simulate the top with "
    "random inputs, or replay --vectors-in; write the vectors (odin3-sim-vectors format) or log a "
    "summary; --vcd writes a waveform",
    sim_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
