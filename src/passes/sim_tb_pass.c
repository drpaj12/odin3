/* sim_tb_pass.c — the `sim_tb` pass: runs a Verilog testbench (subset, spec
 * docs/specs/2026-10-31-testbench-design.md) against the design's modules. */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/sim_netnames.h"
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
    uint64_t progress; /* --progress SEC: seconds between progress lines, 0: none */
    uint64_t seed;
    char plusarg[TB_MAX_PLUSARGS][TB_PLUSARG_LEN];
    const char *plusarg_ptr[TB_MAX_PLUSARGS];
    uint32_t n_plusarg;
    bool vcd_all;                    /* --vcd-all: also every net of every DUT instance */
    odin3_blif_name_style vcd_names; /* --vcd-names: the style of those net names */
    bool two_state; /* --2state: the old 2-state interpreter (default: 4-state, x/z values) */
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

/* The style after --vcd-names goes into style; an unknown style is false. */
static bool take_style(odin3_bytes *args, odin3_blif_name_style *style) {
    char text[NUM_BUF] = {0};
    return take(args, text, sizeof text) && odin3_sim_netnames_style(text, style);
}

static odin3_status usage(void) {
    odin3_log(ODIN3_LOG_ERROR,
              "sim_tb: expects [--top <module>] [--max-time N] [--progress SEC] [--seed N] [--out "
              "<file>] [--vcd <file> [--vcd-all] [--vcd-names short|provenance|hash]] [--plusarg "
              "<text>]... [--2state] [-I "
              "<dir>]... [-D <name[=value]>]... <tb.v>...");
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

/* --vcd, --vcd-all and --vcd-names: true when word is one of them (*ok: its value was good). */
static bool take_vcd_option(odin3_bytes word, odin3_bytes *args, tb_args *out, bool *ok) {
    if (word_is(word, "--vcd")) {
        *ok = take(args, out->vcd, sizeof out->vcd);
    } else if (word_is(word, "--vcd-all")) {
        *ok = !out->vcd_all;
        out->vcd_all = true;
    } else if (word_is(word, "--vcd-names")) {
        *ok = take_style(args, &out->vcd_names);
    } else {
        return false;
    }
    return true;
}

/* --vcd-all needs --vcd, --vcd-names needs --vcd-all (logged when not). */
static bool check_vcd_options(const tb_args *out) {
    bool bad = (out->vcd_all && out->vcd[0] == '\0') ||
               (out->vcd_names != ODIN3_BLIF_NAMES_SHORT && !out->vcd_all);
    if (bad) {
        odin3_log(ODIN3_LOG_ERROR,
                  "sim_tb: --vcd-all needs --vcd <file>; --vcd-names needs --vcd-all");
    }
    return !bad;
}

/* --max-time, --progress and --seed: true when word is one of them (*ok: its value was good). */
static bool take_number_option(odin3_bytes word, odin3_bytes *args, tb_args *out, bool *ok) {
    if (word_is(word, "--max-time")) {
        *ok = take_number(args, &out->max_time);
    } else if (word_is(word, "--progress")) {
        *ok = take_number(args, &out->progress) && out->progress <= UINT32_MAX;
    } else if (word_is(word, "--seed")) {
        *ok = take_number(args, &out->seed);
    } else {
        return false;
    }
    return true;
}

static odin3_status parse_args(odin3_bytes args, tb_args *out) {
    out->max_time = DEFAULT_MAX_TIME;
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool ok = true;
        if (take_vcd_option(word, &args, out, &ok) || take_number_option(word, &args, out, &ok)) {
            /* handled */
        } else if (word_is(word, "--top")) {
            ok = take(&args, out->top, sizeof out->top);
        } else if (word_is(word, "--out")) {
            ok = take(&args, out->out, sizeof out->out);
        } else if (word_is(word, "--plusarg")) {
            ok = take_plusarg(&args, out);
        } else if (word_is(word, "--4state")) {
            out->two_state = false;
        } else if (word_is(word, "--2state")) {
            out->two_state = true;
        } else {
            ok = take_other(word, &args, out);
        }
        if (!ok) {
            return usage();
        }
    }
    if (!check_vcd_options(out)) {
        return ODIN3_ERR_INVALID_ARG;
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

/* One preprocessed file inside the joined text: its stream and the offset where it starts. */
typedef struct join_part {
    odin3_srcbuf_id stream;
    size_t base;
} join_part;

/* The preprocessed streams, joined; map says, per joined line, the source file and line. */
typedef struct tb_join {
    char *text;
    join_part parts[MAX_TB_FILES];
    uint32_t n_parts;
    tb_locmap map;
    const char **files;
    uint32_t *lines;
} tb_join;

static void tb_join_free(tb_join *join) {
    free(join->text);
    free((void *)join->files);
    free(join->lines);
}

/* The segment-map cursors of the parts, to look locations up. */
typedef struct locmap_ctx {
    const odin3_srcman *sm;
    const odin3_strtab *strtab;
    odin3_srcman_cursor cursors[MAX_TB_FILES];
} locmap_ctx;

/* A byte of the joined text and the map entry of its line. */
typedef struct line_pos {
    size_t at;
    uint32_t index;
} line_pos;

/* Sets entry `index` to the source file and line of the byte at joined offset `at`, if it has
 * one. */
static void set_origin(locmap_ctx *ctx, tb_join *join, line_pos pos) {
    size_t at = pos.at;
    uint32_t index = pos.index;
    uint32_t part = 0;
    while (part + 1U < join->n_parts && join->parts[part + 1U].base <= at) {
        part++;
    }
    odin3_loc loc =
        odin3_srcman_cursor_loc(&ctx->cursors[part], (uint32_t)(at - join->parts[part].base));
    odin3_srcloc where = {0};
    odin3_srcman_srcloc(ctx->sm, (odin3_range){loc, loc}, &where);
    if (loc.v != 0 && where.file != 0) {
        join->files[index] = odin3_strtab_get(ctx->strtab, where.file);
        join->lines[index] = where.line;
    }
}

static uint32_t count_lines(const char *text) {
    uint32_t count = 1;
    for (const char *ch = text; *ch != '\0'; ch++) {
        count += *ch == '\n' ? 1U : 0U;
    }
    return count;
}

/* An entry with no location takes the one before it, one line on. */
static void inherit_origin(tb_join *join, uint32_t row) {
    if (join->files[row] == NULL && row > 0 && join->files[row - 1U] != NULL) {
        join->files[row] = join->files[row - 1U];
        join->lines[row] = join->lines[row - 1U] + 1U;
    }
}

/* Fills join->map: one entry per line of the joined text. A line with no location of its own (a
 * blank line the preprocessor made) takes the one before it, one line on. */
static odin3_status build_locmap(odin3_design *design, tb_join *join) {
    locmap_ctx ctx;
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(design, &sm);
    if (st != ODIN3_OK) {
        return st;
    }
    ctx.sm = sm;
    ctx.strtab = odin3_design_strtab(design);
    uint32_t nlines = count_lines(join->text);
    join->files = (const char **)calloc(nlines, sizeof *join->files);
    join->lines = calloc(nlines, sizeof *join->lines);
    if (join->files == NULL || join->lines == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < join->n_parts; i++) {
        odin3_srcman_cursor_init(&ctx.cursors[i], ctx.sm, join->parts[i].stream.v);
    }
    size_t from = 0;
    for (uint32_t row = 0; row < nlines; row++) {
        size_t at = from;
        while (join->text[at] == ' ' || join->text[at] == '\t') {
            at++;
        }
        set_origin(&ctx, join, (line_pos){at, row});
        inherit_origin(join, row);
        const char *eol = strchr(join->text + from, '\n');
        from = eol != NULL ? (size_t)(eol - join->text) + 1U : strlen(join->text);
    }
    join->map = (tb_locmap){nlines, join->files, join->lines};
    return ODIN3_OK;
}

/* Runs every testbench file through the Verilog preprocessor (macros, `ifdef, `include with the
 * -I directories, `timescale kept for the lexer), one preprocessor for all files so a `define
 * reaches the files after it, and joins the streams into join->text (malloc'd, NUL-terminated),
 * with the map from joined lines back to file and line. */
static odin3_status preprocess_files(odin3_design *design, tb_args *args, tb_join *join) {
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
        join->parts[join->n_parts++] = (join_part){stream.stream, all.len};
        st = st != ODIN3_OK ? st : odin3_strbuf_append(&all, stream.text);
        st = st != ODIN3_OK ? st : odin3_strbuf_append(&all, odin3_bytes_cstr("\n"));
    }
    odin3_pp_destroy(pp);
    if (st == ODIN3_OK) {
        join->text = malloc(all.len + 1U);
        if (join->text == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            memcpy(join->text, all.data, all.len);
            join->text[all.len] = '\0';
        }
    }
    odin3_strbuf_free(&all);
    return st != ODIN3_OK ? st : build_locmap(design, join);
}

/* Runs the parsed testbench and writes its output (stdout or --out). */
static odin3_status run_parsed(odin3_design *design, const tb_args *parsed, tb_module *tb) {
    FILE *out = parsed->out[0] != '\0' ? fopen(parsed->out, "w") : stdout;
    if (out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot write `%s`", parsed->out);
        return ODIN3_ERR_IO;
    }
    tb_options opts = {parsed->max_time,
                       parsed->seed,
                       out,
                       parsed->vcd[0] != '\0' ? parsed->vcd : NULL,
                       parsed->plusarg_ptr,
                       parsed->n_plusarg,
                       false,
                       NULL,
                       NULL,
                       (uint32_t)parsed->progress};
    odin3_sim_netnames names;
    odin3_sim_netnames_init(&names, design, "sim_tb", parsed->vcd_names);
    if (parsed->vcd_all) {
        opts.vcd_all = true;
        opts.namer = odin3_sim_netnames_name;
        opts.namer_ctx = &names;
    }
    tb_end end = {0, false};
    odin3_status st = tb_run_end(design, tb, &opts, &end);
    odin3_sim_netnames_free(&names);
    (void)fflush(out);
    if (out != stdout) {
        (void)fclose(out);
    }
    if (st == ODIN3_OK && end.hit_limit) {
        odin3_log(ODIN3_LOG_WARN,
                  "sim_tb: stopped at the time limit --max-time %llu (default %d): the testbench "
                  "had not reached $finish; the next event lay beyond it. Raise it with --max-time "
                  "N if the run is meant to be longer",
                  (unsigned long long)parsed->max_time, DEFAULT_MAX_TIME);
        odin3_log(ODIN3_LOG_INFO, "sim_tb: stopped at time %llu (time limit)",
                  (unsigned long long)end.time);
    } else if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "sim_tb: finished at time %llu", (unsigned long long)end.time);
    }
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
    tb_join join = {0};
    st = st != ODIN3_OK ? st : preprocess_files(design, parsed, &join);
    tb_module *tb = NULL;
    if (st == ODIN3_OK) {
        tb_source src = {parsed->file[0], join.text, design, !parsed->two_state};
        tb_set_locmap(&join.map);
        st = tb_parse(&src, parsed->top[0] != '\0' ? parsed->top : NULL, &tb);
    }
    st = st != ODIN3_OK ? st : run_parsed(design, parsed, tb);
    tb_module_free(tb);
    tb_set_locmap(NULL);
    tb_join_free(&join);
    free(parsed);
    return st;
}

const odin3_pass_def odin3_pass_sim_tb = {
    "sim_tb",
    "sim_tb [--top <module>] [--max-time N] [--seed N] [--out <file>] [--vcd <file> [--vcd-all] "
    "[--vcd-names short|provenance|hash]] [--plusarg <text>]... [--2state] [-I <dir>]... [-D "
    "<name[=value]>]... <tb.v>...: run a "
    "Verilog "
    "testbench (the supported subset, docs/specs/2026-10-31-testbench-design.md) against the "
    "design's modules; values are 4-state (0, 1, x, z; regs start x, as iverilog), best with "
    "`lower --keep-sel`; --2state selects the old 2-state interpreter (all 0 at power-on); "
    "--4state "
    "is accepted and changes nothing; $display text goes to stdout or "
    "--out, "
    "--vcd writes a waveform of the testbench signals and the DUT ports, --vcd-all adds every net "
    "of every DUT instance, named as write_blif --vtr names it; --plusarg (up to 16) "
    "is a run-time argument that $test$plusargs and $value$plusargs read, as name=value; the "
    "files go through the Verilog preprocessor (`define, `ifdef, `include searched in the -I "
    "directories, -D defines) and are joined; --top picks the testbench module among them",
    sim_tb_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
