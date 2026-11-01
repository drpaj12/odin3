/*
 * bench_proc.c — proc benchmark (2E spec §19, plan Task 13): one file per run.
 *
 * Usage: bench_proc --one FILE [--techlib T] [--no-trio] [--noopt]
 *
 * Reads FILE as `read_verilog --nolatches FILE` does (Parmys's mode, the one §19's oracle numbers
 * were measured in), runs the pre-proc passes (the trio: opt_expr, opt_clean, opt -nodffe -nosdff;
 * --no-trio skips them), both untimed, then times `proc` (step F, 2F's opt_expr, included unless
 * --noopt) by its steps: A (decode, clean, rmdead, prune, arst, the clock pre-check), B (mux),
 * C (latches), D (flip-flops), E (orphan nets) and F, summed over the modules (the test hook
 * on_step of proc_test.h). T is a tech library (.o3lib) read first, for the arch's hard blocks.
 *
 * It prints ONE line of `key=value` columns (no blanks inside a value), in this order:
 *   file bytes read_ms trio_ms trio rss_read_kib modules procs
 *   a_ms b_ms c_ms d_ms e_ms f_ms noopt_ms proc_ms budget_ms budget_calls
 *   warnings c_mux c_pmux c_eq c_helper c_dff c_adff c_aldff c_dffsr c_dlatch
 *   nodes_created nets_created pins_created mux_pins
 *   est_mux_pins_B est_nets_B est_nodes_B est_pw_B est_total_B ir_B rss_kib
 * noopt_ms = A + B + C + D + E: the time of `proc --noopt` (§19's targets); proc_ms is the whole
 * odin3_proc_run, so it also holds the work between the steps. trio is `full` (all three passes),
 * `partial` (opt is not available yet, 2F-5: opt_expr and opt_clean only) or `none`. budget_ms is
 * the time inside the byte-budget checks (inside A, B and D): odin3_design_bytes_reserved is
 * O(modules) and the budget is checked at every process boundary, so a many-module design costs
 * O(modules x processes) in it (2E Task 3 review, finding 6). c_* are the cells proc created
 * (odin3_proc_stats; c_helper is $not, $and and $reduce_or). nodes_created and nets_created are
 * the growth of the modules' node and net ID ranges, pins_created the growth of the live nodes'
 * pins ($proc nodes deleted by proc leave the count), mux_pins the growth of the pins of $mux and
 * $pmux nodes.
 * The est_* columns are §19's terms of the lenet.v estimate: mux pins x 28 B x 1.3 (a pin record
 * plus its slot in the net's array, rounded to the size class), nets_created x 48, nodes_created
 * x 48, est_pw_B the largest module's working forms (the peak of the budget's `extra`);
 * est_total_B is their sum. ir_B is odin3_design_bytes_reserved after proc: what the estimate
 * estimates (deleted $proc nodes' pins stay until compact). rss_kib is getrusage's peak for the
 * whole process; rss_read_kib is the peak when proc starts.
 *
 * `elab-sweep --bench build/release/bench_proc --mem-cap 6G` runs it per file, smallest first.
 * Run it from a Release build (bench_guard.h). Exit 0 on success, 1 when the file fails to read or
 * proc fails (the diagnostics on stderr), 2 on a usage error or a Debug build.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
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

enum {
    PATH_BYTES = 4096,
    NODE_BYTES = 48,
    PIN_BYTES = 28,
    NET_BYTES = 48,
    CLASS_NUM = 13, /* the pin arrays' size-class factor 1.3, as CLASS_NUM / CLASS_DEN */
    CLASS_DEN = 10,
    TRIO_PASSES = 3
};

/* What the design holds, counted before and after proc. */
typedef struct bench_counts {
    uint64_t modules, procs, node_end, net_end, pins, mux_pins;
} bench_counts;

/* The step timers the on_step hook fills. */
typedef struct bench_steps {
    double began[ODIN3_PROC_N_STEPS];
    double ms[ODIN3_PROC_N_STEPS];
    uint64_t budget_calls, pw_peak;
} bench_steps;

/* What one file measured. */
typedef struct bench_result {
    uint64_t bytes;
    double read_ms, trio_ms, proc_ms;
    const char *trio;
    long rss_read_kib;
    bench_counts before, after;
    bench_steps steps;
    odin3_proc_stats stats;
    uint64_t ir_bytes;
} bench_result;

typedef struct bench_args {
    const char *file;
    const char *techlib;
    bool trio, noopt;
} bench_args;

static long max_rss_kib(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
}

/* --- the IR's size ------------------------------------------------------------------------- */

static void count_node(odin3_design *design, const odin3_module *mod, odin3_node_id node,
                       bench_counts *out) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
    uint64_t pins = odin3_node_pins(mod, node).count;
    out->pins += pins;
    if (def == NULL) {
        return;
    }
    if ((def->flags & ODIN3_CT_PROCESS) != 0) {
        out->procs++;
    } else if (strcmp(def->name, "$mux") == 0 || strcmp(def->name, "$pmux") == 0) {
        out->mux_pins += pins;
    }
}

static bench_counts count_design(odin3_design *design) {
    bench_counts out = {0};
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        const odin3_module *mod = odin3_module_get(design, (odin3_module_id){i});
        out.modules++;
        out.node_end += odin3_module_node_end(mod) - 1U;
        out.net_end += odin3_module_net_end(mod) - 1U;
        for (uint32_t node = 1; node < odin3_module_node_end(mod); node++) {
            if (odin3_node_live(mod, (odin3_node_id){node})) {
                count_node(design, mod, (odin3_node_id){node}, &out);
            }
        }
    }
    return out;
}

/* --- the step timers ----------------------------------------------------------------------- */

static void on_step(void *user, odin3_proc_step step, bool begin, const odin3_proc_mctx *mctx) {
    bench_steps *steps = user;
    double now = odin3_now_ms();
    if (begin) {
        steps->began[step] = now;
        steps->budget_calls += step == ODIN3_PROC_STEP_BUDGET ? 1U : 0U;
        return;
    }
    steps->ms[step] += now - steps->began[step];
    if (step == ODIN3_PROC_STEP_A && mctx->pw_bytes > steps->pw_peak) {
        steps->pw_peak = mctx->pw_bytes; /* every working form of the module is decoded */
    }
}

/* --- the run ------------------------------------------------------------------------------- */

/* A log sink for `opt`: the 2F stub's "not available yet" line, and the pass manager's failure
 * line after it, are the expected, silent answer. */
static void opt_sink(odin3_log_level level, const char *msg, void *user) {
    bool *stub = user;
    if (strstr(msg, "not available yet") != NULL) {
        *stub = true;
    } else if (!*stub && level <= ODIN3_LOG_ERROR) {
        (void)fprintf(stderr, "bench_proc: %s\n", msg);
    }
}

/* The trio. opt is a stub until 2F-5: that leaves the design as opt_expr and opt_clean made it. */
static int run_trio(odin3_design *design, bench_result *res) {
    static const char *const k_names[TRIO_PASSES] = {"opt_expr", "opt_clean", "opt"};
    static const char *const k_args[TRIO_PASSES] = {"", "", "-nodffe -nosdff"};
    bool stub = false;
    double start = odin3_now_ms();
    int rc = 0;
    for (int i = 0; rc == 0 && i < TRIO_PASSES; i++) {
        if (i == TRIO_PASSES - 1) {
            odin3_log_set_sink(opt_sink, &stub);
        }
        odin3_status st = odin3_pass_run(design, k_names[i], odin3_bytes_cstr(k_args[i]));
        odin3_log_set_sink(NULL, NULL);
        rc = st != ODIN3_OK && !(stub && st == ODIN3_ERR_INVALID_ARG);
    }
    res->trio = stub ? "partial" : "full";
    res->trio_ms = odin3_now_ms() - start;
    return rc;
}

/* The design, the optional tech library, read_verilog --nolatches and the trio, untimed in the
 * steps (their own times are columns). */
static int prepare(const bench_args *args, odin3_design **design, bench_result *res) {
    char pass_args[PATH_BYTES + 16];
    struct stat info;
    *design = odin3_design_create();
    if (*design == NULL || stat(args->file, &info) != 0) {
        return 1;
    }
    res->bytes = (uint64_t)info.st_size;
    res->trio = "none";
    if (args->techlib != NULL &&
        odin3_pass_run(*design, "read_techlib", odin3_bytes_cstr(args->techlib)) != ODIN3_OK) {
        return 1;
    }
    if (snprintf(pass_args, sizeof pass_args, "--nolatches %s", args->file) >=
        (int)sizeof pass_args) {
        (void)fprintf(stderr, "bench_proc: %s: path too long\n", args->file);
        return 1;
    }
    double start = odin3_now_ms();
    int rc = odin3_pass_run(*design, "read_verilog", odin3_bytes_cstr(pass_args)) != ODIN3_OK;
    res->read_ms = odin3_now_ms() - start;
    return rc != 0 || (args->trio && run_trio(*design, res) != 0);
}

/* proc itself, its steps timed. */
static int run_proc(const bench_args *args, odin3_design *design, bench_result *res) {
    odin3_proc_test_hooks hooks = {NULL, NULL, NULL, &res->steps, on_step};
    odin3_proc_opts opts = {args->noopt, ODIN3_PROC_STOP_NONE, false};
    odin3_pass_ctx ctx = {0};
    uint32_t name = 0;
    if (odin3_design_intern(design, odin3_bytes_cstr("bench_proc"), &name) != ODIN3_OK ||
        odin3_pass_run_begin(design, name, &ctx) != ODIN3_OK) {
        return 1;
    }
    res->rss_read_kib = max_rss_kib();
    res->before = count_design(design);
    odin3_proc_test_set_hooks(&hooks);
    double start = odin3_now_ms();
    odin3_status st = odin3_proc_run(&ctx, design, &opts, &res->stats);
    res->proc_ms = odin3_now_ms() - start;
    odin3_proc_test_set_hooks(NULL);
    res->after = count_design(design);
    res->ir_bytes = odin3_design_bytes_reserved(design);
    return st != ODIN3_OK;
}

/* --- the line ------------------------------------------------------------------------------ */

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static uint64_t growth(uint64_t after, uint64_t before) {
    return after > before ? after - before : 0;
}

static uint64_t sum_warnings(const odin3_proc_stats *stats) {
    uint64_t sum = 0;
    for (int i = 0; i < ODIN3_PROC_N_WARNINGS; i++) {
        sum += stats->warnings[i];
    }
    return sum;
}

static void print_line(const bench_args *args, const bench_result *res) {
    const odin3_proc_stats *stats = &res->stats;
    const double *ms = res->steps.ms;
    double noopt = ms[ODIN3_PROC_STEP_A] + ms[ODIN3_PROC_STEP_B] + ms[ODIN3_PROC_STEP_C] +
                   ms[ODIN3_PROC_STEP_D] + ms[ODIN3_PROC_STEP_E];
    uint64_t nodes = growth(res->after.node_end, res->before.node_end);
    uint64_t nets = growth(res->after.net_end, res->before.net_end);
    uint64_t mux_pins = growth(res->after.mux_pins, res->before.mux_pins);
    uint64_t est_mux = mux_pins * PIN_BYTES * CLASS_NUM / CLASS_DEN;
    uint64_t est_total = est_mux + nets * NET_BYTES + nodes * NODE_BYTES + res->steps.pw_peak;
    printf("file=%s bytes=%" PRIu64 " read_ms=%.3f trio_ms=%.3f trio=%s rss_read_kib=%ld "
           "modules=%" PRIu64 " procs=%" PRIu64 " ",
           base_name(args->file), res->bytes, res->read_ms, res->trio_ms, res->trio,
           res->rss_read_kib, res->before.modules, res->before.procs);
    printf("a_ms=%.3f b_ms=%.3f c_ms=%.3f d_ms=%.3f e_ms=%.3f f_ms=%.3f noopt_ms=%.3f "
           "proc_ms=%.3f budget_ms=%.3f budget_calls=%" PRIu64 " warnings=%" PRIu64 " ",
           ms[ODIN3_PROC_STEP_A], ms[ODIN3_PROC_STEP_B], ms[ODIN3_PROC_STEP_C],
           ms[ODIN3_PROC_STEP_D], ms[ODIN3_PROC_STEP_E], ms[ODIN3_PROC_STEP_F], noopt, res->proc_ms,
           ms[ODIN3_PROC_STEP_BUDGET], res->steps.budget_calls, sum_warnings(stats));
    printf("c_mux=%" PRIu32 " c_pmux=%" PRIu32 " c_eq=%" PRIu32 " c_helper=%" PRIu32
           " c_dff=%" PRIu32 " c_adff=%" PRIu32 " c_aldff=%" PRIu32 " c_dffsr=%" PRIu32
           " c_dlatch=%" PRIu32 " ",
           stats->cells[ODIN3_PROC_K_MUX], stats->cells[ODIN3_PROC_K_PMUX],
           stats->cells[ODIN3_PROC_K_EQ], stats->cells[ODIN3_PROC_K_HELPER],
           stats->cells[ODIN3_PROC_K_DFF], stats->cells[ODIN3_PROC_K_ADFF],
           stats->cells[ODIN3_PROC_K_ALDFF], stats->cells[ODIN3_PROC_K_DFFSR],
           stats->cells[ODIN3_PROC_K_DLATCH]);
    printf("nodes_created=%" PRIu64 " nets_created=%" PRIu64 " pins_created=%" PRIu64
           " mux_pins=%" PRIu64 " est_mux_pins_B=%" PRIu64 " est_nets_B=%" PRIu64
           " est_nodes_B=%" PRIu64 " est_pw_B=%" PRIu64 " est_total_B=%" PRIu64 " ir_B=%" PRIu64
           " rss_kib=%ld\n",
           nodes, nets, growth(res->after.pins, res->before.pins), mux_pins, est_mux,
           nets * NET_BYTES, nodes * NODE_BYTES, res->steps.pw_peak, est_total, res->ir_bytes,
           max_rss_kib());
    (void)fflush(stdout);
}

/* One pass over FILE: read, trio, proc, count; when print, the line comes before the teardown,
 * whose time is nobody's concern. */
static int measure(const bench_args *args, bool print) {
    bench_result res = {0};
    odin3_design *design = NULL;
    int rc = prepare(args, &design, &res) || run_proc(args, design, &res);
    if (rc == 0 && print) {
        print_line(args, &res);
    } else if (rc != 0) {
        (void)fprintf(stderr, "bench_proc: %s: failed\n", args->file);
    }
    odin3_design_destroy(design);
    return rc;
}

/* One warm-up pass (page cache, allocator and first-touch costs, torn down and discarded), then
 * the measured one. */
static int bench_one(const bench_args *args) {
    return measure(args, false) || measure(args, true);
}

static int usage(void) {
    (void)fputs("usage: bench_proc --one FILE [--techlib T] [--no-trio] [--noopt]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    if (bench_refuse_unoptimized(argv[0]) != 0) {
        return 2;
    }
    bench_args args = {NULL, NULL, true, false};
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (strcmp(argv[i], "--one") == 0 && more) {
            args.file = argv[++i];
        } else if (strcmp(argv[i], "--techlib") == 0 && more) {
            args.techlib = argv[++i];
        } else if (strcmp(argv[i], "--no-trio") == 0) {
            args.trio = false;
        } else if (strcmp(argv[i], "--noopt") == 0) {
            args.noopt = true;
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
