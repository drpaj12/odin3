/* run_testbenches.c — the `run_testbenches` pass: runs the project's testbenches (the `testbench`
 * key, spec docs/specs/2026-10-31-project-testbench-design.md) through the sim_tb interpreter. */
#include "passes/run_testbenches.h"
#include "frontends/project/project.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "sim/tb.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PATH_LEN = 1024, DECIMAL = 10, NUM_BUF = 32, DEFAULT_MAX_TIME = 10000000 };

static const char *const k_format_names[] = {"o3proj", "f", "qsf", "odin2"};
static const char *const k_on_names[] = {"?", "rtl", "netlist", "both"};

typedef struct rt_args {
    char only[PATH_LEN];
    char json[PATH_LEN];
    char report[PATH_LEN];
    uint64_t seed;
    char plusarg[TB_MAX_PLUSARGS][TB_PLUSARG_LEN];
    const char *plusarg_ptr[TB_MAX_PLUSARGS];
    uint32_t n_plusarg;
} rt_args;

/* One side of a run: the output text and the end time. */
typedef struct rt_side {
    char *data;
    size_t len;
    uint64_t end_time;
    bool hit_limit; /* the run stopped at its time limit, not at `$finish` */
} rt_side;

/* --- arguments ----------------------------------------------------------------------------- */

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
static bool take_plusarg(odin3_bytes *args, rt_args *out) {
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

static odin3_status parse_args(odin3_bytes args, rt_args *out) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool ok = false;
        if (odin3_pass_arg_is(word, "--only")) {
            ok = take(&args, out->only, sizeof out->only);
        } else if (odin3_pass_arg_is(word, "--json")) {
            ok = take(&args, out->json, sizeof out->json);
        } else if (odin3_pass_arg_is(word, "--report")) {
            ok = take(&args, out->report, sizeof out->report);
        } else if (odin3_pass_arg_is(word, "--seed")) {
            ok = take_number(&args, &out->seed);
        } else if (odin3_pass_arg_is(word, "--plusarg")) {
            ok = take_plusarg(&args, out);
        }
        if (!ok) {
            odin3_log(ODIN3_LOG_ERROR, "run_testbenches: expects [--only <name>] [--json <file>] "
                                       "[--report <file>] [--seed N] [--plusarg <text>]...");
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

/* --- one side ------------------------------------------------------------------------------ */

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

static const char *text_of(const odin3_design *design, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

/* The cause of a failed step: the last ERROR it logged, first line only ("" when none). */
static void cause_of(char *cause, size_t cap) {
    (void)snprintf(cause, cap, "%s", odin3_log_last_error());
    cause[strcspn(cause, "\n")] = '\0';
}

static void set_error_cause(rt_result *res, const char *step, odin3_status st, const char *cause) {
    res->state = RT_ERROR;
    if (cause[0] == '\0') {
        (void)snprintf(res->error, sizeof res->error, "%s: %s", step, odin3_status_string(st));
    } else {
        (void)snprintf(res->error, sizeof res->error, "%s: %s: %s", step, odin3_status_string(st),
                       cause);
    }
}

/* Records an error whose cause is the last ERROR logged. */
static void set_error(rt_result *res, const char *step, odin3_status st) {
    char cause[RT_STEP];
    cause_of(cause, sizeof cause);
    set_error_cause(res, step, st, cause);
}

/* The testbench must instantiate the named dut (instance or module name); true without a dut. */
static bool dut_present(const tb_module *tb, const char *dut) {
    if (dut == NULL) {
        return true;
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        if (strcmp(tb->insts[i].name, dut) == 0 || strcmp(tb->insts[i].module, dut) == 0) {
            return true;
        }
    }
    return false;
}

/* Parses the testbench file of tb into *out. */
static odin3_status parse_tb(const odin3_project *proj, const odin3_proj_testbench *tb,
                             tb_module **out) {
    const char *file = text_of(proj->design, tb->file.resolved);
    char *text = read_text(file);
    if (text == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: cannot read `%s`", file);
        return ODIN3_ERR_IO;
    }
    tb_source src = {file, text};
    const char *top = tb->top != 0 ? text_of(proj->design, tb->top) : NULL;
    odin3_status st = tb_parse(&src, top, out);
    free(text);
    return st;
}

/* Runs tb against design; the output text goes to *side (owned by the caller). */
static odin3_status run_side(odin3_design *design, const odin3_project *proj,
                             const odin3_proj_testbench *tb, const rt_args *args, rt_side *side) {
    tb_module *mod = NULL;
    odin3_status st = parse_tb(proj, tb, &mod);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *dut = tb->dut != 0 ? text_of(proj->design, tb->dut) : NULL;
    if (!dut_present(mod, dut)) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: dut `%s` is not instantiated in the testbench",
                  dut);
        tb_module_free(mod);
        return ODIN3_ERR_INVALID_ARG;
    }
    FILE *out = open_memstream(&side->data, &side->len);
    if (out == NULL) {
        tb_module_free(mod);
        return ODIN3_ERR_NO_MEMORY;
    }
    tb_options opts = {tb->max_time != 0 ? tb->max_time : DEFAULT_MAX_TIME,
                       args->seed,
                       out,
                       NULL,
                       args->plusarg_ptr,
                       args->n_plusarg};
    tb_end end = {0, false};
    st = tb_run_end(design, mod, &opts, &end);
    side->end_time = end.time;
    side->hit_limit = end.hit_limit;
    (void)fclose(out); /* flushes and sets data and len */
    tb_module_free(mod);
    return st;
}

/* --- the RTL scratch design ---------------------------------------------------------------- */

/* The script that builds the project's RTL, or an error (logged, ODIN3_ERR_INVALID_ARG) for an
 * entry or revision the by-line script cannot quote (a double quote in it). */
static odin3_status rtl_script(const odin3_project *proj, odin3_strbuf *script) {
    const char *entry = text_of(proj->design, proj->entry.resolved);
    const char *rev = proj->has_revision ? text_of(proj->design, proj->revision) : "";
    if (strchr(entry, '"') != NULL || strchr(rev, '"') != NULL) {
        odin3_log(ODIN3_LOG_ERROR,
                  "run_testbenches: the project path or revision holds a double quote, which a "
                  "script cannot quote: `%s`",
                  entry);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st =
        odin3_strbuf_appendf(script, "read_project --format %s \"%s\"",
                             k_format_names[proj->format - ODIN3_PROJ_FORMAT_O3PROJ], entry);
    if (st == ODIN3_OK && proj->has_revision) {
        st = odin3_strbuf_appendf(script, " --revision \"%s\"", rev);
    }
    return st != ODIN3_OK ? st
                          : odin3_strbuf_append(
                                script, odin3_bytes_cstr("\nread_verilog\nproc\nopt\nlower\n"));
}

/* A scratch design holding the project's RTL: the project read again from its entry, then
 * read_verilog, proc, opt, lower (the default flow only: no flatten, parameter override or top
 * change the script in hand made). NULL (logged) on failure. */
static odin3_design *build_rtl(const odin3_project *proj) {
    odin3_strbuf script;
    odin3_strbuf_init(&script);
    odin3_status st = rtl_script(proj, &script);
    odin3_design *scratch = st == ODIN3_OK ? odin3_design_create() : NULL;
    if (scratch != NULL) {
        odin3_script_src src = {"run_testbenches", ODIN3_SCRIPT_BY_LINE};
        st = odin3_pass_run_script(scratch, (odin3_bytes){script.data, script.len}, src);
        if (st != ODIN3_OK) {
            odin3_design_destroy(scratch);
            scratch = NULL;
        }
    }
    odin3_strbuf_free(&script);
    return scratch;
}

/* --- one testbench ------------------------------------------------------------------------- */

static void side_free(rt_side *side) {
    free(side->data);
    memset(side, 0, sizeof *side);
}

static uint32_t count_lines(const rt_side *side) {
    uint32_t lines = 0;
    for (size_t i = 0; i < side->len; i++) {
        lines += side->data[i] == '\n' ? 1U : 0U;
    }
    return lines + (side->len > 0 && side->data[side->len - 1] != '\n' ? 1U : 0U);
}

/* What a run needs: the design in hand, its project and the arguments. */
typedef struct rt_ctx {
    odin3_design *design;
    const odin3_project *proj;
    const rt_args *args;
    odin3_design *rtl;       /* the scratch RTL design, built by the first rtl run */
    bool rtl_tried;          /* build_rtl ran (rtl NULL: it failed) */
    char rtl_cause[RT_STEP]; /* why it failed */
} rt_ctx;

/* The scratch RTL design, built once per run_testbenches call; NULL when it cannot be built. */
static odin3_design *rtl_design(rt_ctx *ctx) {
    if (!ctx->rtl_tried) {
        ctx->rtl_tried = true;
        odin3_log_clear_last_error();
        ctx->rtl = build_rtl(ctx->proj);
        if (ctx->rtl == NULL) {
            cause_of(ctx->rtl_cause, sizeof ctx->rtl_cause);
        }
    }
    return ctx->rtl;
}

/* The rtl side into *side, or the error recorded in res. */
static bool run_rtl(rt_ctx *ctx, const odin3_proj_testbench *tb, rt_side *side, rt_result *res) {
    odin3_design *scratch = rtl_design(ctx);
    if (scratch == NULL) {
        set_error_cause(res, "elaborate project (rtl)", ODIN3_ERR_PARSE, ctx->rtl_cause);
        return false;
    }
    odin3_log_clear_last_error();
    odin3_status st = run_side(scratch, ctx->proj, tb, ctx->args, side);
    if (st != ODIN3_OK) {
        set_error(res, "run (rtl)", st);
    }
    return st == ODIN3_OK;
}

/* The netlist side (the design in hand) into *side, or the error recorded in res. */
static bool run_netlist(const rt_ctx *ctx, const odin3_proj_testbench *tb, rt_side *side,
                        rt_result *res) {
    if (odin3_design_module_end(ctx->design) <= 1) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: no design in hand for `on netlist`: run "
                                   "synthesis or read_blif first");
        set_error(res, "run (netlist)", ODIN3_ERR_INVALID_ARG);
        return false;
    }
    odin3_log_clear_last_error();
    odin3_status st = run_side(ctx->design, ctx->proj, tb, ctx->args, side);
    if (st != ODIN3_OK) {
        set_error(res, "run (netlist)", st);
    }
    return st == ODIN3_OK;
}

/* A text as a view ("" when empty). */
static rt_text view(const rt_side *side) {
    return (rt_text){side->data != NULL ? side->data : "", side->len};
}

/* The expect file against the text; a fail is recorded in res. */
static void check_expect(const rt_ctx *ctx, const odin3_proj_testbench *tb, const rt_side *side,
                         rt_result *res) {
    const char *path = text_of(ctx->proj->design, tb->expect.resolved);
    char *want = read_text(path);
    if (want == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: cannot read `%s`", path);
        set_error(res, "read expect", ODIN3_ERR_IO);
        return;
    }
    rt_text left = {want, strlen(want)};
    if (rt_compare(left, view(side), &res->mm)) {
        res->state = RT_FAIL;
        res->mm.compared = "expect";
    }
    free(want);
}

/* Both sides ran: compares them, then the first against expect; a fail is recorded in res. */
static void compare_sides(const rt_ctx *ctx, const odin3_proj_testbench *tb, const rt_side *rtl,
                          const rt_side *net, rt_result *res) {
    bool both = tb->on == ODIN3_PROJ_TB_BOTH;
    if (!tb->timeboxed && (rtl->hit_limit || net->hit_limit)) {
        res->state = RT_FAIL;
        res->timeout = true;
        (void)snprintf(res->error, sizeof res->error,
                       "no $finish by time %" PRIu64 " (%s limit; a hung testbench does not pass)",
                       tb->max_time != 0 ? tb->max_time : (uint64_t)DEFAULT_MAX_TIME,
                       tb->max_time != 0 ? "max-time" : "default");
        return;
    }
    const rt_side *first = tb->on == ODIN3_PROJ_TB_NETLIST ? net : rtl;
    res->end_time = first->end_time;
    res->lines = count_lines(first);
    if (both && rt_compare(view(rtl), view(net), &res->mm)) {
        res->state = RT_FAIL;
        res->mm.compared = "rtl vs netlist";
    }
    if (res->state == RT_PASS && tb->expect.given != 0) {
        check_expect(ctx, tb, first, res);
    }
}

/* Runs the sides `on` asks for, then the comparisons. */
static void run_sides(rt_ctx *ctx, const odin3_proj_testbench *tb, rt_result *res) {
    rt_side rtl = {0};
    rt_side net = {0};
    bool ok = tb->on == ODIN3_PROJ_TB_NETLIST || run_rtl(ctx, tb, &rtl, res);
    ok = ok && (tb->on == ODIN3_PROJ_TB_RTL || run_netlist(ctx, tb, &net, res));
    if (ok) {
        compare_sides(ctx, tb, &rtl, &net, res);
    }
    side_free(&rtl);
    side_free(&net);
}

static void run_testbench(rt_ctx *ctx, const odin3_proj_testbench *tb, rt_result *res) {
    (void)snprintf(res->name, sizeof res->name, "%s", text_of(ctx->proj->design, tb->name));
    res->on = k_on_names[tb->on <= ODIN3_PROJ_TB_BOTH ? tb->on : 0];
    double start = odin3_now_ms();
    run_sides(ctx, tb, res);
    res->wall_ms = (uint64_t)(odin3_now_ms() - start);
}

/* --- the pass ------------------------------------------------------------------------------ */

static const odin3_project *finished_project(const odin3_design *design) {
    const odin3_project *proj = odin3_design_project(design);
    if (proj == NULL || !proj->finished) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: no project was read (run read_project first)");
        return NULL;
    }
    return proj;
}

/* Whether tb runs: --only names it (even a disabled one), or none is given and tb is enabled. */
static bool selected(const odin3_project *proj, const odin3_proj_testbench *tb,
                     const rt_args *args) {
    if (args->only[0] == '\0') {
        return !tb->disabled;
    }
    return strcmp(args->only, text_of(proj->design, tb->name)) == 0;
}

/* The count of testbenches the project disables by default. */
static size_t count_disabled(const odin3_project *proj) {
    size_t count = 0;
    for (size_t i = 0; i < proj->testbenches.len; i++) {
        count += ((const odin3_proj_testbench *)odin3_vec_cat(&proj->testbenches, i))->disabled
                     ? 1U
                     : 0U;
    }
    return count;
}

static odin3_status write_file(const char *path, const odin3_strbuf *text) {
    return path[0] == '\0' ? ODIN3_OK : odin3_pass_write_text(path, text);
}

/* Prints the report and writes --report and --json. */
static odin3_status emit(const rt_args *args, const rt_result *res, size_t count) {
    odin3_strbuf text;
    odin3_strbuf json;
    odin3_strbuf_init(&text);
    odin3_strbuf_init(&json);
    odin3_status st = rt_report_text(res, count, &text);
    if (st == ODIN3_OK && text.len > 0) {
        (void)fwrite(text.data, 1, text.len, stdout);
        (void)fflush(stdout);
    }
    st = st != ODIN3_OK ? st : write_file(args->report, &text);
    st = st != ODIN3_OK ? st : rt_report_json(res, count, &json);
    st = st != ODIN3_OK ? st : write_file(args->json, &json);
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&json);
    return st;
}

/* Runs the selected testbenches into results; returns how many ran. */
static size_t run_selected(rt_ctx *ctx, rt_result *results) {
    size_t count = 0;
    for (size_t i = 0; i < ctx->proj->testbenches.len; i++) {
        const odin3_proj_testbench *tb = odin3_vec_cat(&ctx->proj->testbenches, i);
        if (selected(ctx->proj, tb, ctx->args)) {
            run_testbench(ctx, tb, &results[count++]);
        }
    }
    return count;
}

/* Whether the run is over without running anything: prints why and sets *st. */
static bool nothing_to_run(const odin3_project *proj, const rt_args *args, odin3_status *st) {
    size_t disabled = count_disabled(proj);
    *st = ODIN3_OK;
    if (args->only[0] != '\0') {
        return false; /* an unknown name is an error, found by the run */
    }
    if (proj->testbenches.len == 0) {
        (void)printf("run_testbenches: the project defines no testbenches\n");
        return true;
    }
    if (disabled == proj->testbenches.len) {
        (void)printf("run_testbenches: all %zu testbenches are disabled by the project (run one "
                     "with --only <name>)\n",
                     disabled);
        return true;
    }
    if (disabled > 0) {
        (void)printf("run_testbenches: %zu testbenches disabled by the project not run (use "
                     "--only <name>)\n",
                     disabled);
    }
    return false;
}

static odin3_status run_testbenches_run(odin3_pass_ctx *pctx, odin3_design *design,
                                        odin3_bytes args_text, void *user) {
    (void)pctx;
    (void)user;
    rt_args args = {{0}, {0}, {0}, 0, {{0}}, {NULL}, 0};
    odin3_status st = parse_args(args_text, &args);
    const odin3_project *proj = st == ODIN3_OK ? finished_project(design) : NULL;
    if (st != ODIN3_OK || proj == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (nothing_to_run(proj, &args, &st)) {
        return st;
    }
    rt_result *results = calloc(proj->testbenches.len + 1U, sizeof *results);
    if (results == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rt_ctx ctx = {design, proj, &args, NULL, false, ""};
    size_t count = run_selected(&ctx, results);
    if (ctx.rtl != NULL) {
        odin3_design_destroy(ctx.rtl);
    }
    bool bad = false;
    if (count == 0) {
        odin3_log(ODIN3_LOG_ERROR, "run_testbenches: no testbench named `%s`", args.only);
        bad = true;
    }
    st = count > 0 ? emit(&args, results, count) : ODIN3_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        bad = bad || results[i].state != RT_PASS;
        rt_result_free(&results[i]);
    }
    free(results);
    if (st == ODIN3_OK && bad) {
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

const odin3_pass_def odin3_pass_run_testbenches = {
    "run_testbenches",
    "run_testbenches [--only <name>] [--json <file>] [--report <file>] [--seed N] [--plusarg "
    "<text>]...: run the "
    "project's testbenches (the testbench key; docs/specs/2026-10-31-project-testbench-design.md);"
    " fails when any testbench fails or errors",
    run_testbenches_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
