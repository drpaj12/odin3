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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PATH_LEN = 1024, DECIMAL = 10, NUM_BUF = 32, DEFAULT_MAX_TIME = 10000000 };

static const char *const k_format_names[] = {"o3proj", "f", "qsf", "odin2"};
static const char *const k_on_names[] = {"?", "rtl", "netlist", "both"};

typedef struct rt_args {
    char only[RT_NAME];
    char json[PATH_LEN];
    char report[PATH_LEN];
    uint64_t seed;
} rt_args;

/* One side of a run: the output text and the end time. */
typedef struct rt_side {
    char *data;
    size_t len;
    uint64_t end_time;
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
        }
        if (!ok) {
            odin3_log(ODIN3_LOG_ERROR, "run_testbenches: expects [--only <name>] [--json <file>] "
                                       "[--report <file>] [--seed N]");
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

static void set_error(rt_result *res, const char *step, odin3_status st) {
    res->state = RT_ERROR;
    (void)snprintf(res->error, sizeof res->error, "%s: %s", step, odin3_status_string(st));
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
                             const odin3_proj_testbench *tb, uint64_t seed, rt_side *side) {
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
    tb_options opts = {tb->max_time != 0 ? tb->max_time : DEFAULT_MAX_TIME, seed, out, NULL};
    st = tb_run(design, mod, &opts, &side->end_time);
    (void)fclose(out); /* flushes and sets data and len */
    tb_module_free(mod);
    return st;
}

/* --- the RTL scratch design ---------------------------------------------------------------- */

/* A scratch design holding the project's RTL: the project read again from its entry, then
 * read_verilog, proc, opt, lower. NULL (logged) on failure. */
static odin3_design *build_rtl(const odin3_project *proj) {
    odin3_strbuf script;
    odin3_strbuf_init(&script);
    const char *entry = text_of(proj->design, proj->entry.resolved);
    odin3_status st =
        odin3_strbuf_appendf(&script, "read_project --format %s \"%s\"",
                             k_format_names[proj->format - ODIN3_PROJ_FORMAT_O3PROJ], entry);
    if (st == ODIN3_OK && proj->has_revision) {
        st = odin3_strbuf_appendf(&script, " --revision \"%s\"",
                                  text_of(proj->design, proj->revision));
    }
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_append(&script, odin3_bytes_cstr("\nread_verilog\nproc\nopt\nlower\n"));
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
} rt_ctx;

/* The rtl side into *side, or the error recorded in res. */
static bool run_rtl(const rt_ctx *ctx, const odin3_proj_testbench *tb, rt_side *side,
                    rt_result *res) {
    odin3_design *scratch = build_rtl(ctx->proj);
    if (scratch == NULL) {
        set_error(res, "elaborate project (rtl)", ODIN3_ERR_PARSE);
        return false;
    }
    odin3_status st = run_side(scratch, ctx->proj, tb, ctx->args->seed, side);
    odin3_design_destroy(scratch);
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
    odin3_status st = run_side(ctx->design, ctx->proj, tb, ctx->args->seed, side);
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
static void run_sides(const rt_ctx *ctx, const odin3_proj_testbench *tb, rt_result *res) {
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

static void run_testbench(const rt_ctx *ctx, const odin3_proj_testbench *tb, rt_result *res) {
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

/* Whether tb is selected by --only (all when none given). */
static bool selected(const odin3_project *proj, const odin3_proj_testbench *tb,
                     const rt_args *args) {
    return args->only[0] == '\0' || strcmp(args->only, text_of(proj->design, tb->name)) == 0;
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

static odin3_status run_testbenches_run(odin3_pass_ctx *pctx, odin3_design *design,
                                        odin3_bytes args_text, void *user) {
    (void)pctx;
    (void)user;
    rt_args args = {{0}, {0}, {0}, 0};
    odin3_status st = parse_args(args_text, &args);
    const odin3_project *proj = st == ODIN3_OK ? finished_project(design) : NULL;
    if (st != ODIN3_OK || proj == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (proj->testbenches.len == 0) {
        (void)printf("run_testbenches: the project defines no testbenches\n");
        return ODIN3_OK;
    }
    rt_result *results = calloc(proj->testbenches.len, sizeof *results);
    if (results == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rt_ctx ctx = {design, proj, &args};
    size_t count = 0;
    for (size_t i = 0; i < proj->testbenches.len; i++) {
        const odin3_proj_testbench *tb = odin3_vec_cat(&proj->testbenches, i);
        if (selected(proj, tb, &args)) {
            run_testbench(&ctx, tb, &results[count++]);
        }
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
    "run_testbenches [--only <name>] [--json <file>] [--report <file>] [--seed N]: run the "
    "project's testbenches (the testbench key; docs/specs/2026-10-31-project-testbench-design.md);"
    " fails when any testbench fails or errors",
    run_testbenches_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
