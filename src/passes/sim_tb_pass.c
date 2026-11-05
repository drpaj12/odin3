/* sim_tb_pass.c — the `sim_tb` pass: runs a Verilog testbench (subset, spec
 * docs/specs/2026-10-31-testbench-design.md) against the design's modules. */
#include "frontends/verilog/preproc.h"
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

enum {
    PATH_MAX_LEN = 1024,
    DECIMAL = 10,
    DEFAULT_MAX_TIME = 10000000,
    NUM_BUF = 32,
    MAX_TB_FILES = 32, /* testbench files, include directories and -D defines of one run */
    MAX_INCDIRS = 16,
    MAX_DEFINES = 32,
    DEFINE_LEN = 256
};

typedef struct tb_args {
    char file[MAX_TB_FILES][PATH_MAX_LEN];
    uint32_t n_files;
    char incdir[MAX_INCDIRS][PATH_MAX_LEN];
    const char *incdir_ptr[MAX_INCDIRS];
    uint32_t n_incdirs;
    char define[MAX_DEFINES][DEFINE_LEN]; /* "name" or "name=value" */
    uint32_t n_defines;
    char top[TB_NAME];
    char out[PATH_MAX_LEN];
    char vcd[PATH_MAX_LEN];
    uint64_t max_time;
    uint64_t seed;
    char plusarg[TB_MAX_PLUSARGS][TB_PLUSARG_LEN];
    const char *plusarg_ptr[TB_MAX_PLUSARGS];
    uint32_t n_plusarg;
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

/* Takes the text of a --plusarg; false (after saying why) when it is missing, too long or the
 * 17th has come. */
static bool take_plusarg(odin3_bytes *args, tb_args *out) {
    if (out->n_plusarg >= TB_MAX_PLUSARGS) {
        odin3_log(ODIN3_LOG_ERROR, "at most %d --plusarg options are allowed", TB_MAX_PLUSARGS);
        return false;
    }
    if (!take(args, out->plusarg[out->n_plusarg], TB_PLUSARG_LEN)) {
        odin3_log(ODIN3_LOG_ERROR, "--plusarg needs a text of 1 to %d bytes", TB_PLUSARG_LEN - 1);
        return false;
    }
    out->plusarg_ptr[out->n_plusarg] = out->plusarg[out->n_plusarg];
    out->n_plusarg++;
    return true;
}

/* Takes the next word as an include directory (-I). */
static bool take_incdir(odin3_bytes *args, tb_args *out) {
    if (out->n_incdirs >= MAX_INCDIRS) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: at most %d -I directories are allowed", MAX_INCDIRS);
        return false;
    }
    return take(args, out->incdir[out->n_incdirs++], PATH_MAX_LEN);
}

/* Takes the next word as a define (-D name or name=value). */
static bool take_define(odin3_bytes *args, tb_args *out) {
    if (out->n_defines >= MAX_DEFINES) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: at most %d -D defines are allowed", MAX_DEFINES);
        return false;
    }
    return take(args, out->define[out->n_defines++], DEFINE_LEN);
}

static odin3_status usage(void) {
    odin3_log(ODIN3_LOG_ERROR, "sim_tb: expects [--top <module>] [--max-time N] [--seed N] [--out "
                               "<file>] [--vcd <file>] [--plusarg <text>]... [-I <dir>]... "
                               "[-D <name[=value]>]... <tb.v>...");
    return ODIN3_ERR_INVALID_ARG;
}

/* -I and -D with their value, or a testbench file name. */
static bool take_other(odin3_bytes word, odin3_bytes *args, tb_args *out) {
    if (word_is(word, "-I")) {
        return take_incdir(args, out);
    }
    if (word_is(word, "-D")) {
        return take_define(args, out);
    }
    if (out->n_files >= MAX_TB_FILES || word.len >= PATH_MAX_LEN) {
        return false;
    }
    memcpy(out->file[out->n_files], word.ptr, word.len);
    out->file[out->n_files][word.len] = '\0';
    out->n_files++;
    return true;
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
        } else if (word_is(word, "--plusarg")) {
            ok = take_plusarg(&args, out);
        } else {
            ok = take_other(word, &args, out);
        }
        if (!ok) {
            return usage();
        }
    }
    return out->n_files == 0 ? usage() : ODIN3_OK;
}

/* Splits "name=value" (value may be empty or absent) into a preprocessor define. */
static odin3_pp_define make_define(const char *text) {
    const char *eq = strchr(text, '=');
    if (eq == NULL) {
        return (odin3_pp_define){odin3_bytes_cstr(text), {NULL, 0}, false};
    }
    return (odin3_pp_define){{text, (size_t)(eq - text)}, odin3_bytes_cstr(eq + 1), true};
}

static odin3_status intern_path(const odin3_design *design, const char *path, uint32_t *id) {
    return odin3_strtab_intern(odin3_design_strtab(design), odin3_bytes_cstr(path), id);
}

/* Runs every testbench file through the Verilog preprocessor (macros, `ifdef, `include with the
 * -I directories, `timescale kept for the lexer), one preprocessor for all files so a `define
 * reaches the files after it, and joins the streams into *text (malloc'd, NUL-terminated). */
static odin3_status preprocess_files(odin3_design *design, tb_args *args, char **text) {
    odin3_pp_define defs[MAX_DEFINES];
    for (uint32_t i = 0; i < args->n_defines; i++) {
        defs[i] = make_define(args->define[i]);
    }
    for (uint32_t i = 0; i < args->n_incdirs; i++) {
        args->incdir_ptr[i] = args->incdir[i];
    }
    odin3_pp_config config = {defs, args->n_defines, args->incdir_ptr, args->n_incdirs};
    odin3_pp *pp = NULL;
    odin3_status st = odin3_pp_create(design, &config, &pp);
    odin3_strbuf all;
    odin3_strbuf_init(&all);
    for (uint32_t i = 0; st == ODIN3_OK && i < args->n_files; i++) {
        odin3_pp_source src = {0};
        st = intern_path(design, args->file[i], &src.resolved);
        src.given = src.resolved;
        odin3_pp_stream stream;
        st = st != ODIN3_OK ? st : odin3_pp_run(pp, &src, &stream);
        st = st != ODIN3_OK ? st : odin3_strbuf_append(&all, stream.text);
        st = st != ODIN3_OK ? st : odin3_strbuf_append(&all, odin3_bytes_cstr("\n"));
    }
    odin3_pp_destroy(pp);
    if (st == ODIN3_OK) {
        *text = malloc(all.len + 1U);
        if (*text == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            memcpy(*text, all.data, all.len);
            (*text)[all.len] = '\0';
        }
    }
    odin3_strbuf_free(&all);
    return st;
}

static odin3_status sim_tb_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                               void *user) {
    (void)ctx;
    (void)user;
    tb_args *parsed = calloc(1, sizeof *parsed);
    if (parsed == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = parse_args(args, parsed);
    char *text = NULL;
    st = st != ODIN3_OK ? st : preprocess_files(design, parsed, &text);
    if (st != ODIN3_OK) {
        free(parsed);
        return st;
    }
    tb_module *tb = NULL;
    tb_source src = {parsed->file[0], text, design};
    st = tb_parse(&src, parsed->top[0] != '\0' ? parsed->top : NULL, &tb);
    free(text);
    if (st != ODIN3_OK) {
        free(parsed);
        return st;
    }
    FILE *out = parsed->out[0] != '\0' ? fopen(parsed->out, "w") : stdout;
    if (out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot write `%s`", parsed->out);
        tb_module_free(tb);
        free(parsed);
        return ODIN3_ERR_IO;
    }
    tb_options opts = {
        parsed->max_time,    parsed->seed,     out, parsed->vcd[0] != '\0' ? parsed->vcd : NULL,
        parsed->plusarg_ptr, parsed->n_plusarg};
    uint64_t end_time = 0;
    st = tb_run(design, tb, &opts, &end_time);
    (void)fflush(out);
    if (out != stdout) {
        (void)fclose(out);
    }
    tb_module_free(tb);
    free(parsed);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "sim_tb: finished at time %llu", (unsigned long long)end_time);
    }
    return st;
}

const odin3_pass_def odin3_pass_sim_tb = {
    "sim_tb",
    "sim_tb [--top <module>] [--max-time N] [--seed N] [--out <file>] [--vcd <file>] [--plusarg "
    "<text>]... [-I <dir>]... [-D <name[=value]>]... <tb.v>...: run a "
    "Verilog "
    "testbench (the supported subset, docs/specs/2026-10-31-testbench-design.md) against the "
    "design's modules; $display text goes to stdout or --out, "
    "--vcd writes a waveform of the testbench signals and the DUT ports; --plusarg (up to 16) "
    "is a run-time argument that $test$plusargs and $value$plusargs read, as name=value; the "
    "files go through the Verilog preprocessor (`define, `ifdef, `include searched in the -I "
    "directories, -D defines) and are joined; --top picks the testbench module among them",
    sim_tb_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
