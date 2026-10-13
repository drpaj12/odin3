/*
 * bench_elab.c — elaboration benchmark (2D spec §15, plan Task 27): one file per run.
 *
 * Usage: bench_elab --one FILE [--techlib T] [--keep-latches]
 *
 * Reads FILE as `read_verilog --nolatches FILE` does (Parmys's mode, the one §15's oracle numbers
 * were measured in; --keep-latches reads it without), but runs the stages itself so each is
 * timed: the preprocessor and parser (read_verilog's reader), stage 1, stage 2's binding, the
 * rest of stage 2. T is a tech library (.o3lib) read first, for the arch's hard blocks.
 *
 * It prints ONE line of `key=value` columns (no blanks inside a value), in this order:
 *   file bytes parse_ms stage1_ms bind_ms stage2_ms elab_ms
 *   parsed_nodes parsed_bytes elab_nodes elab_bytes
 *   modules specs cells procs mems nodes nets wires pins
 *   est_pins_B est_nets_B est_nodes_B est_wires_B est_bodies_B est_elab_B est_total_B
 *   est_stage1_B ir_B rss_kib
 * stage2_ms is stage 2 without its binding; elab_ms = stage1 + bind + stage2 (parse excluded).
 * The est_* columns are EL-46's terms (spec §15): pins x 28 B x 1.3 (a pin record plus its slot
 * in the net's array, rounded to the size class), nets x 48, nodes x 48, wires x 64, the $proc
 * bodies' bytes, the elaborated store's odin3_ast_bytes_reserved; est_total_B is their sum (the
 * stage-2 peak, the parsed store already dropped); est_stage1_B is the stage-1 peak (parsed +
 * elaborated store). ir_B is odin3_design_bytes_reserved after stage 2: what the estimate
 * estimates. rss_kib is getrusage's peak for the whole process (parse included).
 *
 * `elab-sweep --bench build/release/bench_elab` runs it per file, smallest first, and
 * tools/elab-check/bench_report.py prints the §15 targets from the lines. Run it from a Release
 * build. Exit 0 on success, 1 when the file fails to read or elaborate (the diagnostics on
 * stderr), 2 on a usage error.
 */
#include "ast/ast.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/project/paths.h"
#include "frontends/verilog/elab1.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "passes/manager.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"

#include "bench_guard.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    PATH_BYTES = 4096,
    NODE_BYTES = 48,
    PIN_BYTES = 28,
    NET_BYTES = 48,
    WIRE_BYTES = 64,
    CLASS_NUM = 13, /* the pin arrays' size-class factor 1.3, as CLASS_NUM / CLASS_DEN */
    CLASS_DEN = 10
};

/* What one file measured. */
typedef struct bench_result {
    uint64_t bytes;
    double parse_ms, stage1_ms, bind_ms, stage2_ms;
    uint64_t parsed_nodes, parsed_bytes, elab_nodes, elab_bytes;
    uint32_t modules, specs, cells, procs, mems;
    uint64_t nodes, nets, wires, pins, bodies, ir_bytes;
} bench_result;

/* The command line. */
typedef struct bench_args {
    const char *file;
    const char *techlib;
    bool nolatches;
} bench_args;

static long max_rss_kib(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
}

/* --- the IR's size ------------------------------------------------------------------------- */

/* One node of a module into res: its counts (pins, cells, processes, $mem cells, $proc body). */
static void count_node(odin3_design *design, const odin3_module *mod, odin3_node_id node,
                       bench_result *res) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
    res->nodes++;
    res->pins += odin3_node_pins(mod, node).count;
    if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
        return;
    }
    if ((def->flags & ODIN3_CT_PROCESS) != 0) {
        const odin3_value *body = odin3_node_param(mod, node, ODIN3_PROC_P_BODY);
        res->procs++;
        res->bodies += body != NULL ? body->len : 0;
        return;
    }
    res->cells++;
    res->mems += strcmp(def->name, "$mem") == 0 ? 1U : 0U;
}

static void count_module(odin3_design *design, odin3_module_id id, bench_result *res) {
    const odin3_module *mod = odin3_module_get(design, id);
    const char *name = odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(mod));
    res->modules++;
    res->specs += odin3_elab2_is_specialization(name) ? 1U : 0U;
    for (uint32_t node = 1; node < odin3_module_node_end(mod); node++) {
        if (odin3_node_live(mod, (odin3_node_id){node})) {
            count_node(design, mod, (odin3_node_id){node}, res);
        }
    }
    for (uint32_t net = 1; net < odin3_module_net_end(mod); net++) {
        res->nets += odin3_net_live(mod, (odin3_net_id){net}) ? 1U : 0U;
    }
    for (uint32_t wire = 1; wire < odin3_module_wire_end(mod); wire++) {
        res->wires += odin3_wire_live(mod, (odin3_wire_id){wire}) ? 1U : 0U;
    }
}

static void count_design(odin3_design *design, bench_result *res) {
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        count_module(design, (odin3_module_id){i}, res);
    }
    res->ir_bytes = odin3_design_bytes_reserved(design);
}

/* --- the run ------------------------------------------------------------------------------- */

/* The design-file source of path, resolved against the working directory as read_verilog does. */
static int make_source(odin3_design *design, const char *path, odin3_pp_source *src) {
    char cwd[PATH_BYTES];
    odin3_strbuf resolved;
    odin3_strbuf_init(&resolved);
    *src = (odin3_pp_source){0};
    int rc = getcwd(cwd, sizeof cwd) == NULL ||
             odin3_path_resolve((odin3_path_join){odin3_bytes_cstr(cwd), odin3_bytes_cstr(path)},
                                &resolved) != ODIN3_OK ||
             odin3_design_intern(design, odin3_bytes_cstr(path), &src->given) != ODIN3_OK ||
             odin3_design_intern(design, (odin3_bytes){resolved.data, resolved.len},
                                 &src->resolved) != ODIN3_OK ||
             odin3_design_intern(design, odin3_bytes_cstr("work"), &src->library) != ODIN3_OK;
    odin3_strbuf_free(&resolved);
    return rc;
}

/* The design, the optional tech library and the bench's pass run. */
static int open_design(const bench_args *args, odin3_design **design, odin3_pass_ctx *ctx) {
    uint32_t name = 0;
    *design = odin3_design_create();
    if (*design == NULL) {
        return 1;
    }
    if (args->techlib != NULL &&
        odin3_pass_run(*design, "read_techlib", odin3_bytes_cstr(args->techlib)) != ODIN3_OK) {
        return 1;
    }
    return odin3_design_intern(*design, odin3_bytes_cstr("bench_elab"), &name) != ODIN3_OK ||
           odin3_pass_run_begin(*design, name, ctx) != ODIN3_OK;
}

/* The reader (preprocessor and parser), timed; the parsed store goes to the design as the pass
 * hands it over. */
static int read_file(const bench_args *args, odin3_pass_ctx *ctx, odin3_ast **parsed,
                     bench_result *res) {
    odin3_pp_source src;
    struct stat info;
    if (stat(args->file, &info) != 0 || make_source(ctx->design, args->file, &src) != 0) {
        return 1;
    }
    res->bytes = (uint64_t)info.st_size;
    double start = odin3_now_ms();
    odin3_rv_opts opts = {&src, 1, ctx->run, NULL, false};
    int rc = odin3_read_verilog_files(ctx->design, &opts, parsed) != ODIN3_OK;
    res->parse_ms = odin3_now_ms() - start;
    if (rc != 0) {
        return 1;
    }
    res->parsed_nodes = odin3_ast_node_end(*parsed).v - 1;
    res->parsed_bytes = odin3_ast_bytes_reserved(*parsed);
    return odin3_design_set_ast(ctx->design, *parsed) != ODIN3_OK;
}

/* Stage 1, binding and stage 2 (odin3_elab_run's steps 1-5), each timed. *elab is the
 * elaborated store; it stays with the caller. */
static int elaborate(const bench_args *args, odin3_pass_ctx *ctx, odin3_ast *parsed,
                     odin3_ast **elab, bench_result *res) {
    odin3_elab_opts given = {0};
    odin3_elab_opts options;
    uint64_t gen_iters = 0;
    given.nolatches = args->nolatches;
    if (odin3_elab1_options(ctx->design, &given, &options) != ODIN3_OK) {
        return 1;
    }
    odin3_passrun_id parsed_run = odin3_ast_run(parsed);
    options.gen_iters_out = &gen_iters;
    double start = odin3_now_ms();
    odin3_status st = odin3_elab_stage1(ctx, parsed, &options, elab);
    res->stage1_ms = odin3_now_ms() - start;
    options.stage1_ms = res->stage1_ms;
    options.gen_iters = gen_iters;
    options.gen_iters_out = NULL;
    odin3_design_drop_ast(ctx->design, parsed_run, ODIN3_AST_FORM_PARSED);
    if (st != ODIN3_OK) {
        return 1;
    }
    res->elab_nodes = odin3_ast_node_end(*elab).v - 1;
    res->elab_bytes = odin3_ast_bytes_reserved(*elab);
    options.bind_ms_out = &res->bind_ms;
    start = odin3_now_ms();
    st = odin3_elab_stage2(ctx, *elab, &options);
    res->stage2_ms = odin3_now_ms() - start - res->bind_ms;
    return st != ODIN3_OK;
}

/* --- the line ------------------------------------------------------------------------------ */

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* The estimate's terms (spec §15, EL-46) of res. */
typedef struct bench_estimate {
    uint64_t pins, nets, nodes, wires, bodies, elab, total, stage1;
} bench_estimate;

static bench_estimate estimate(const bench_result *res) {
    bench_estimate est = {0};
    est.pins = res->pins * PIN_BYTES * CLASS_NUM / CLASS_DEN;
    est.nets = res->nets * NET_BYTES;
    est.nodes = res->nodes * NODE_BYTES;
    est.wires = res->wires * WIRE_BYTES;
    est.bodies = res->bodies;
    est.elab = res->elab_bytes;
    est.total = est.pins + est.nets + est.nodes + est.wires + est.bodies + est.elab;
    est.stage1 = res->parsed_bytes + res->elab_bytes;
    return est;
}

static void print_line(const bench_args *args, const bench_result *res) {
    bench_estimate est = estimate(res);
    printf("file=%s bytes=%" PRIu64 " parse_ms=%.3f stage1_ms=%.3f bind_ms=%.3f stage2_ms=%.3f "
           "elab_ms=%.3f parsed_nodes=%" PRIu64 " parsed_bytes=%" PRIu64 " elab_nodes=%" PRIu64
           " elab_bytes=%" PRIu64 " ",
           base_name(args->file), res->bytes, res->parse_ms, res->stage1_ms, res->bind_ms,
           res->stage2_ms, res->stage1_ms + res->bind_ms + res->stage2_ms, res->parsed_nodes,
           res->parsed_bytes, res->elab_nodes, res->elab_bytes);
    printf("modules=%" PRIu32 " specs=%" PRIu32 " cells=%" PRIu32 " procs=%" PRIu32 " mems=%" PRIu32
           " nodes=%" PRIu64 " nets=%" PRIu64 " wires=%" PRIu64 " pins=%" PRIu64 " ",
           res->modules, res->specs, res->cells, res->procs, res->mems, res->nodes, res->nets,
           res->wires, res->pins);
    printf("est_pins_B=%" PRIu64 " est_nets_B=%" PRIu64 " est_nodes_B=%" PRIu64
           " est_wires_B=%" PRIu64 " est_bodies_B=%" PRIu64 " est_elab_B=%" PRIu64
           " est_total_B=%" PRIu64 " est_stage1_B=%" PRIu64 " ir_B=%" PRIu64 " rss_kib=%ld\n",
           est.pins, est.nets, est.nodes, est.wires, est.bodies, est.elab, est.total, est.stage1,
           res->ir_bytes, max_rss_kib());
    (void)fflush(stdout);
}

/* One pass over FILE: read, elaborate, count; when print, the line comes before the teardown,
 * whose time is nobody's concern. */
static int measure(const bench_args *args, bool print) {
    bench_result res = {0};
    odin3_design *design = NULL;
    odin3_pass_ctx ctx = {0};
    odin3_ast *parsed = NULL;
    odin3_ast *elab = NULL;
    int rc = open_design(args, &design, &ctx) || read_file(args, &ctx, &parsed, &res);
    rc = rc || elaborate(args, &ctx, parsed, &elab, &res);
    if (rc == 0 && print) {
        count_design(design, &res);
        print_line(args, &res);
    } else if (rc != 0) {
        fprintf(stderr, "bench_elab: %s: failed\n", args->file);
    }
    odin3_ast_destroy(elab);
    odin3_design_destroy(design);
    return rc;
}

/* One warm-up pass (page cache, allocator and first-touch costs, torn down and discarded), then
 * the measured one. */
static int bench_one(const bench_args *args) {
    return measure(args, false) || measure(args, true);
}

static int usage(void) {
    (void)fputs("usage: bench_elab --one FILE [--techlib T] [--keep-latches]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    if (bench_refuse_unoptimized(argv[0]) != 0) {
        return 2;
    }
    bench_args args = {NULL, NULL, true};
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (strcmp(argv[i], "--one") == 0 && more) {
            args.file = argv[++i];
        } else if (strcmp(argv[i], "--techlib") == 0 && more) {
            args.techlib = argv[++i];
        } else if (strcmp(argv[i], "--keep-latches") == 0) {
            args.nolatches = false;
        } else {
            return usage();
        }
    }
    if (args.file == NULL) {
        return usage();
    }
    odin3_log_set_level(ODIN3_LOG_ERROR);
    return bench_one(&args);
}
