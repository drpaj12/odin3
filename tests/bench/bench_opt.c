/*
 * bench_opt.c — the opt benchmark (2F plan Task 15, spec section 19; Release): one Verilog file
 * through Odin III's Phase 2 script, `opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt;
 * compact` (tools/opt-check/phase2.script), then `opt -full` on the result.
 *
 * Usage: bench_opt --one FILE    (elab-sweep --bench passes exactly this, 2D-7a's interface)
 * A file that instantiates hard blocks (single_port_ram, adder, ...) needs the arch's tech library:
 * set ODIN3_BENCH_TECHLIB to its .o3lib (elab-sweep --bench does not forward --techlib).
 *
 * It prints ONE line of key=value columns, in 2D's bench_elab style:
 *   - the read and `proc` are untimed; cells, nets and pins (live) after the read (suffix 0),
 *     after `proc` (p, the main `opt`'s input) and after the script (1);
 *   - for the script's opt passes (the trio, then the main `opt`; `compact` apart) and, with the
 *     prefix `full_`, for `opt -full`: iterations, the time in total and per sub-pass (`t_expr`
 *     ... `t_clean`), opt_expr's per-round setup (`t_setup`: the invert map, the FF-polarity scan
 *     and the global topological sort, which the Task 13 review asked to see apart; it is part of
 *     `t_expr`), and the rewrites per sub-pass (`rw_*`) and per rule family (`rules=`);
 *   - the dead objects at the peak (after the main `opt`, before `compact`), `t_compact`;
 *   - The RSS columns (KiB). Each is the PEAK OF ITS OWN PHASE, not the process-lifetime
 *     ru_maxrss (which would hide opt's growth under the read's and `proc`'s larger peak): before
 *     each phase the kernel's high-water mark is reset (write "5" to /proc/self/clear_refs) and
 *     VmHWM is read from /proc/self/status afterwards. The read's own peak is excluded.
 *       rss_proc_kib      peak of the trio (opt_expr; opt_clean; opt -nodffe -nosdff) and `proc`
 *       rss_proc_cur_kib  current RSS after `proc`: the post-proc IR, the baseline of the target
 *       rss_opt_kib       peak of the main `opt`, before `compact`
 *       rss_opt_growth_pct 100 * (rss_opt_kib - rss_proc_cur_kib) / rss_proc_cur_kib: spec 19's
 *                         "peak RSS growth over the post-proc IR, before the script's compact"
 *                         (at most 25 percent for lenet.v); THIS is the column the target uses
 *       rss_compact_kib   peak of `compact` (its transient copy is not budget-gated)
 *       rss_full_kib      peak of `opt -full`
 *       rss_peak_kib      the largest of the four peaks
 *       rss_src           hwm: the peaks are VmHWM readings; cur: clear_refs was unavailable and
 *                         each "peak" is only the RSS at the end of its phase (a lower bound).
 *     A phase's peak is at least the RSS when it starts, so a phase that allocates less than
 *     what is already resident reads as that baseline.
 * The times come from opt_test.h's on_call and on_expr_setup hooks; the counts from the INFO lines
 * the passes log under --opt-stats, read through a log sink.
 */
#include "bench_opt.h"

#include "bench_guard.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/opt/opt_test.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { N_SUBS = 7, MAX_FAMILIES = 24, FAMILY_LEN = 24, RULE_NAME_LEN = 96, ARGS_LEN = 1100 };

static const char *const k_subs[N_SUBS] = {"opt_expr",  "opt_merge", "opt_muxtree", "opt_reduce",
                                           "opt_share", "opt_dff",   "opt_clean"};
static const char *const k_short[N_SUBS] = {"expr",  "merge", "muxtree", "reduce",
                                            "share", "dff",   "clean"};

/* The Phase 2 script's commands, as tools/opt-check/phase2.script (test_sp_splice checks this). */
static const char *const k_pre[] = {"opt_expr", "opt_clean", "opt -nodffe -nosdff"};
static const char *const k_proc = "proc";
static const char *const k_main = "opt";
static const char *const k_compact = "compact";
static const char *const k_full = "opt -full";
static const char *const k_stats = " --opt-stats";

typedef struct family {
    char name[FAMILY_LEN];
    uint64_t count;
} family;

/* The measurements of one stretch of timed passes. */
typedef struct phase {
    double sub[N_SUBS], total, setup;
    uint64_t setup_rounds, iterations;
    uint64_t rewritten, folded, merged, ports, reduced, shared, dff_ffs, dff_bits;
    uint64_t clean_cells, clean_nets, clean_wires;
    family fam[MAX_FAMILIES];
    uint32_t n_fam;
} phase;

typedef struct ir_size {
    uint64_t cells, nets, pins, dead_nodes, dead_nets, dead_wires;
} ir_size;

typedef struct bench_row {
    ir_size before, proc, after, peak;
    phase script, full;
    double t_compact;
    /* Per-phase RSS in KiB (see "The RSS columns" in the header): the peak of the trio and `proc`,
     * the current RSS after `proc`, the peaks of the main `opt`, `compact` and `opt -full`, the
     * largest of them, and whether the peaks are VmHWM readings (true) or end-of-phase RSS. */
    long rss_proc_kib, rss_proc_cur_kib, rss_opt_kib, rss_compact_kib, rss_full_kib, rss_peak_kib;
    bool rss_hwm;
} bench_row;

/* The recorder the hooks and the sink fill: the phase being timed (NULL: nothing), the sub-pass
 * call in flight (its index and start) and opt_expr's setup start. */
typedef struct recorder {
    phase *cur;
    int sub;
    double sub_start, setup_start;
} recorder;

static recorder g_rec;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* --- RSS: each phase's own peak ---------------------------------------------------------------- */

/* The KiB after `key` (e.g. "VmHWM:") in /proc/self/status, or -1. */
static long status_kib(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) {
        return -1;
    }
    char line[256];
    long kib = -1;
    size_t len = strlen(key);
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, len) == 0) {
            kib = strtol(line + len, NULL, 10);
            break;
        }
    }
    (void)fclose(f);
    return kib;
}

/* The current resident set in KiB (/proc/self/statm field 2), or -1. */
static long current_rss_kib(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL) {
        return -1;
    }
    long size = 0;
    long pages = -1;
    if (fscanf(f, "%ld %ld", &size, &pages) != 2) {
        pages = -1;
    }
    (void)fclose(f);
    long page_kib = sysconf(_SC_PAGESIZE) / 1024;
    return pages < 0 ? -1 : pages * page_kib;
}

/* Starts a phase: resets the kernel's high-water mark (VmHWM) to the current RSS through
 * /proc/self/clear_refs ("5"). False when that is unavailable; the peaks then fall back to the
 * RSS at the end of each phase. */
static bool rss_phase_begin(void) {
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (f == NULL) {
        return false;
    }
    bool ok = fputs("5", f) >= 0;
    return fclose(f) == 0 && ok;
}

/* The peak since rss_phase_begin (hwm: VmHWM), else the current RSS. */
static long rss_phase_peak(bool hwm) {
    long kib = hwm ? status_kib("VmHWM:") : -1;
    return kib >= 0 ? kib : current_rss_kib();
}

/* --- the hooks ------------------------------------------------------------------------------- */

static void close_call(double now) {
    if (g_rec.cur != NULL && g_rec.sub >= 0) {
        g_rec.cur->sub[g_rec.sub] += now - g_rec.sub_start;
    }
    g_rec.sub = -1;
}

static void on_call(void *user, const char *sub) {
    (void)user;
    if (g_rec.cur == NULL) {
        return;
    }
    double now = now_seconds();
    close_call(now);
    for (int i = 0; i < N_SUBS; i++) {
        if (strcmp(k_subs[i], sub) == 0) {
            g_rec.sub = i;
            g_rec.sub_start = now;
        }
    }
}

static void on_expr_setup(void *user, bool begin) {
    (void)user;
    if (g_rec.cur == NULL) {
        return;
    }
    if (begin) {
        g_rec.setup_start = now_seconds();
        return;
    }
    g_rec.cur->setup += now_seconds() - g_rec.setup_start;
    g_rec.cur->setup_rounds++;
}

/* --- the log sink: the counts of the INFO lines ---------------------------------------------- */

/* The rule family of a --opt-stats name: opt_dff.* is "dff", const.$t "fold", else the text before
 * the first '.' or '_'. */
static void family_of(const char *rule, char *out) {
    if (strncmp(rule, "opt_dff.", 8) == 0) {
        (void)snprintf(out, FAMILY_LEN, "dff");
    } else if (strncmp(rule, "const.", 6) == 0) {
        (void)snprintf(out, FAMILY_LEN, "fold");
    } else {
        size_t len = strcspn(rule, "._");
        (void)snprintf(out, FAMILY_LEN, "%.*s", (int)len, rule);
    }
}

static void add_rule(phase *ph, const char *rule, uint64_t count) {
    char name[FAMILY_LEN];
    family_of(rule, name);
    for (uint32_t i = 0; i < ph->n_fam; i++) {
        if (strcmp(ph->fam[i].name, name) == 0) {
            ph->fam[i].count += count;
            return;
        }
    }
    if (ph->n_fam < MAX_FAMILIES) {
        (void)snprintf(ph->fam[ph->n_fam].name, FAMILY_LEN, "%s", name);
        ph->fam[ph->n_fam++].count = count;
    }
}

/* One INFO line into the counters of ph; the formats are spec section 16's. */
static void count_line(phase *ph, const char *msg) {
    unsigned a = 0, b = 0, c = 0;
    char rule[RULE_NAME_LEN];
    if (sscanf(msg, "opt_expr: %u cells rewritten, %u cells folded", &a, &b) == 2) {
        ph->rewritten += a;
        ph->folded += b;
    } else if (sscanf(msg, "opt_merge: removed %u cells", &a) == 1) {
        ph->merged += a;
    } else if (sscanf(msg, "opt_muxtree: removed %u multiplexer ports", &a) == 1) {
        ph->ports += a;
    } else if (sscanf(msg, "opt_reduce: performed %u changes", &a) == 1) {
        ph->reduced += a;
    } else if (sscanf(msg, "opt_share: merged %u cells", &a) == 1) {
        ph->shared += a;
    } else if (sscanf(msg, "opt_dff: %u flip-flops changed, %u constant bits", &a, &b) == 2) {
        ph->dff_ffs += a;
        ph->dff_bits += b;
    } else if (sscanf(msg, "opt_clean: removed %u unused cells, %u unused nets and %u unused wires",
                      &a, &b, &c) == 3) {
        ph->clean_cells += a;
        ph->clean_nets += b;
        ph->clean_wires += c;
    } else if (sscanf(msg, "opt: fixed point after %u iterations", &a) == 1 ||
               sscanf(msg, "opt: -fast: done after %u iterations", &a) == 1) {
        ph->iterations += a;
    } else if (sscanf(msg, "opt: rule %95s %u", rule, &a) == 2) {
        add_rule(ph, rule, a);
    }
}

static void on_log(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_INFO && g_rec.cur != NULL) {
        count_line(g_rec.cur, msg);
    } else if (level <= ODIN3_LOG_WARN) {
        (void)fprintf(stderr, "odin3: %s: %s\n", level == ODIN3_LOG_ERROR ? "error" : "warning",
                      msg);
    }
}

/* --- sizes ----------------------------------------------------------------------------------- */

static void size_module(const odin3_module *module, ir_size *out) {
    uint32_t live = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        live += odin3_node_live(module, (odin3_node_id){id}) ? 1U : 0U;
    }
    out->cells += live;
    out->dead_nodes += odin3_module_node_end(module) - 1U - live;
    live = 0;
    for (uint32_t id = 1; id < odin3_module_net_end(module); id++) {
        live += odin3_net_live(module, (odin3_net_id){id}) ? 1U : 0U;
    }
    out->nets += live;
    out->dead_nets += odin3_module_net_end(module) - 1U - live;
    live = 0;
    for (uint32_t id = 1; id < odin3_module_wire_end(module); id++) {
        live += odin3_wire_live(module, (odin3_wire_id){id}) ? 1U : 0U;
    }
    out->dead_wires += odin3_module_wire_end(module) - 1U - live;
    for (uint32_t id = 1; id < odin3_module_pin_end(module); id++) {
        out->pins += odin3_pin_live(module, (odin3_pin_id){id}) ? 1U : 0U;
    }
}

static void measure_size(odin3_design *design, ir_size *out) {
    memset(out, 0, sizeof *out);
    for (uint32_t id = 1; id < odin3_design_module_end(design); id++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        if (module != NULL) {
            size_module(module, out);
        }
    }
}

/* --- running the script ---------------------------------------------------------------------- */

/* One pass by its script text; into ph when ph is not NULL (timed, counted, with --opt-stats). */
static odin3_status run_script_pass(odin3_design *design, phase *ph, const char *command) {
    char line[ARGS_LEN];
    const char *space = strchr(command, ' ');
    size_t name_len = space != NULL ? (size_t)(space - command) : strlen(command);
    char name[32];
    (void)snprintf(name, sizeof name, "%.*s", (int)name_len, command);
    (void)snprintf(line, sizeof line, "%s%s", space != NULL ? space + 1 : "",
                   ph != NULL && strncmp(name, "opt", 3) == 0 ? k_stats : "");
    g_rec.cur = ph;
    g_rec.sub = -1;
    double start = now_seconds();
    odin3_status st = odin3_design_run_pass(design, name, line);
    double end = now_seconds();
    close_call(end);
    if (ph != NULL) {
        ph->total += end - start;
    }
    g_rec.cur = NULL;
    if (st != ODIN3_OK) {
        (void)fprintf(stderr, "bench_opt: pass '%s' failed: %d\n", command, (int)st);
    }
    return st;
}

static long max_of(long a, long b) {
    return a > b ? a : b;
}

static odin3_status run_script(odin3_design *design, bench_row *row) {
    odin3_status st = ODIN3_OK;
    row->rss_hwm = rss_phase_begin();
    for (size_t i = 0; st == ODIN3_OK && i < sizeof k_pre / sizeof k_pre[0]; i++) {
        st = run_script_pass(design, &row->script, k_pre[i]);
    }
    st = st == ODIN3_OK ? run_script_pass(design, NULL, k_proc) : st;
    row->rss_proc_kib = rss_phase_peak(row->rss_hwm);
    row->rss_proc_cur_kib = current_rss_kib();
    measure_size(design, &row->proc);
    (void)rss_phase_begin();
    st = st == ODIN3_OK ? run_script_pass(design, &row->script, k_main) : st;
    row->rss_opt_kib = rss_phase_peak(row->rss_hwm);
    measure_size(design, &row->peak);
    (void)rss_phase_begin();
    double start = now_seconds();
    st = st == ODIN3_OK ? run_script_pass(design, NULL, k_compact) : st;
    row->t_compact = now_seconds() - start;
    row->rss_compact_kib = rss_phase_peak(row->rss_hwm);
    measure_size(design, &row->after);
    (void)rss_phase_begin();
    st = st == ODIN3_OK ? run_script_pass(design, &row->full, k_full) : st;
    row->rss_full_kib = rss_phase_peak(row->rss_hwm);
    row->rss_peak_kib = max_of(max_of(row->rss_proc_kib, row->rss_opt_kib),
                               max_of(row->rss_compact_kib, row->rss_full_kib));
    return st;
}

/* --- the line -------------------------------------------------------------------------------- */

typedef struct out_buf {
    char *buf;
    size_t cap, len;
    bool full;
} out_buf;

static void put(out_buf *out, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static void put(out_buf *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out->buf + out->len, out->cap - out->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out->cap - out->len) {
        out->full = true;
        return;
    }
    out->len += (size_t)n;
}

static void put_phase(out_buf *out, const char *prefix, const phase *ph) {
    put(out, " %siters=%" PRIu64 " %st_total=%.3f", prefix, ph->iterations, prefix, ph->total);
    for (int i = 0; i < N_SUBS; i++) {
        put(out, " %st_%s=%.3f", prefix, k_short[i], ph->sub[i]);
    }
    put(out, " %st_setup=%.3f %ssetup_rounds=%" PRIu64, prefix, ph->setup, prefix,
        ph->setup_rounds);
    put(out, " %srw_rewritten=%" PRIu64 " %srw_folded=%" PRIu64 " %srw_merge=%" PRIu64, prefix,
        ph->rewritten, prefix, ph->folded, prefix, ph->merged);
    put(out, " %srw_muxtree=%" PRIu64 " %srw_reduce=%" PRIu64 " %srw_share=%" PRIu64, prefix,
        ph->ports, prefix, ph->reduced, prefix, ph->shared);
    put(out, " %srw_dff=%" PRIu64 " %srw_dff_bits=%" PRIu64, prefix, ph->dff_ffs, prefix,
        ph->dff_bits);
    put(out, " %srw_clean=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " %srules=", prefix, ph->clean_cells,
        ph->clean_nets, ph->clean_wires, prefix);
    if (ph->n_fam == 0) {
        put(out, "-");
    }
    for (uint32_t i = 0; i < ph->n_fam; i++) {
        put(out, "%s%s:%" PRIu64, i == 0 ? "" : ",", ph->fam[i].name, ph->fam[i].count);
    }
}

static void put_size(out_buf *out, const char *suffix, const ir_size *size) {
    put(out, " cells%s=%" PRIu64 " nets%s=%" PRIu64 " pins%s=%" PRIu64, suffix, size->cells, suffix,
        size->nets, suffix, size->pins);
}

static bool format_row(const bench_row *row, const char *name, char *buf, size_t cap) {
    out_buf out = {buf, cap, 0, false};
    put(&out, "bench_opt file=%s", name);
    put_size(&out, "0", &row->before);
    put_size(&out, "p", &row->proc);
    put_size(&out, "1", &row->after);
    put_phase(&out, "", &row->script);
    put(&out, " t_compact=%.3f", row->t_compact);
    put(&out, " dead_nodes=%" PRIu64 " dead_nets=%" PRIu64 " dead_wires=%" PRIu64,
        row->peak.dead_nodes, row->peak.dead_nets, row->peak.dead_wires);
    put_phase(&out, "full_", &row->full);
    double base = (double)row->rss_proc_cur_kib;
    put(&out, " rss_proc_kib=%ld rss_proc_cur_kib=%ld rss_opt_kib=%ld rss_opt_growth_pct=%.1f",
        row->rss_proc_kib, row->rss_proc_cur_kib, row->rss_opt_kib,
        base > 0 ? 100.0 * ((double)row->rss_opt_kib - base) / base : 0.0);
    put(&out, " rss_compact_kib=%ld rss_full_kib=%ld rss_peak_kib=%ld rss_src=%s",
        row->rss_compact_kib, row->rss_full_kib, row->rss_peak_kib,
        row->rss_hwm ? "hwm" : "cur");
    return !out.full;
}

/* --- entry points ---------------------------------------------------------------------------- */

odin3_status bench_opt_design(odin3_design *design, const char *name, char *buf, size_t cap) {
    bench_row *row = calloc(1, sizeof *row);
    if (row == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_opt_test_hooks hooks = {on_call, NULL, on_expr_setup};
    odin3_opt_test_set_hooks(&hooks);
    odin3_log_set_sink(on_log, NULL);
    measure_size(design, &row->before);
    odin3_status st = run_script(design, row);
    odin3_log_set_sink(NULL, NULL);
    odin3_opt_test_set_hooks(NULL);
    if (st == ODIN3_OK && !format_row(row, name, buf, cap)) {
        st = ODIN3_ERR_INVALID_ARG;
    }
    free(row);
    return st;
}

odin3_status bench_opt_one(const char *path, char *buf, size_t cap) {
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    char args[ARGS_LEN];
    const char *techlib = getenv("ODIN3_BENCH_TECHLIB"); /* the arch's hard blocks, as elab-sweep */
    odin3_status st = ODIN3_OK;
    if (techlib != NULL && techlib[0] != '\0') {
        st = odin3_design_run_pass(design, "read_techlib", techlib);
    }
    (void)snprintf(args, sizeof args, "--nolatches %s", path);
    st = st == ODIN3_OK ? odin3_design_run_pass(design, "read_verilog", args) : st;
    if (st == ODIN3_OK) {
        const char *base = strrchr(path, '/');
        st = bench_opt_design(design, base != NULL ? base + 1 : path, buf, cap);
    } else {
        (void)fprintf(stderr, "bench_opt: reading %s failed: %d\n", path, (int)st);
    }
    odin3_design_destroy(design);
    return st;
}

#ifndef BENCH_OPT_NO_MAIN
/* One discarded pass in a forked child (so the measured pass starts from a process that never held
 * the warm-up's heap, and its RSS readings stay its own): warms the page cache and the CPU. Skipped
 * for a file over WARMUP_MAX_BYTES (lenet-size inputs would double the run). */
enum { WARMUP_MAX_BYTES = 4 << 20 };

static void warm_up(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0 || sb.st_size > WARMUP_MAX_BYTES) {
        return;
    }
    (void)fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        char line[BENCH_OPT_LINE];
        _exit(bench_opt_one(path, line, sizeof line) == ODIN3_OK ? 0 : 1);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

int main(int argc, char **argv) {
    if (bench_refuse_unoptimized(argv[0]) != 0) {
        return 2;
    }
    if (argc != 3 || strcmp(argv[1], "--one") != 0) {
        (void)fprintf(stderr, "usage: bench_opt --one FILE\n");
        return 2;
    }
    warm_up(argv[2]);
    char line[BENCH_OPT_LINE];
    odin3_status st = bench_opt_one(argv[2], line, sizeof line);
    if (st != ODIN3_OK) {
        return 1;
    }
    (void)puts(line);
    return 0;
}
#endif
