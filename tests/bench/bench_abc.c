/*
 * bench_abc.c - the abc pass benchmark (3D plan Task 22, spec §12 and §13.9; Release): one BLIF
 * through `read_techlib; read_blif; abc --lut K [--via-blif DIR]`, one tab-separated line per run
 * (module and clock domain).
 *
 * Usage: bench_abc --one FILE.blif [--lut K] [--via-blif DIR] [--exe ABC] [--techlib T] [--header]
 *        bench_abc --micro
 *   --lut K       the LUT size (default 6)
 *   --via-blif D  the BLIF exchange through the directory D (abc<r>.in.blif, .cmd, .out.blif stay)
 *   --exe ABC     after each linked run, run the pinned `abc` executable on the same exchange BLIF
 *                 and expanded script (abc<r>.in.blif and abc<r>.cmd of D; its output goes to
 *                 abc<r>.exe.out.blif) and print a second line; needs --via-blif
 *   --techlib T   the .o3lib read first (default lib/vtr.o3lib, else $ODIN3_BENCH_TECHLIB)
 *   --header      print the column names first
 *   --micro       the two fixed costs of spec §12 and nothing else: `open` plus `close` of the
 * linked engine (the worst of 10, target 20 ms) and the script checker on a 64 KiB script (the
 * worst of 10, target 1 ms); one line `micro<TAB>open_close_ms<TAB>x<TAB>
 *                 checker_ms<TAB>y<TAB>checker_bytes<TAB>n`
 * The exchange of a run without --via-blif is the pass's default (ODIN3_ABC_EXCHANGE=aig|blif
 * chooses it, as for odin3). Run it from the repository root.
 *
 * Columns, in spec §12's order (a figure that does not apply is `-`; times are milliseconds):
 *   design module domain exchange            the run
 *   cells ffs luts depth                     soft cells in, flip-flops and LUTs out, LUT depth
 *   classify_ms aig_ms check_ms splice_ms prov_ms outside_ms
 *                                            outside ABC: the region, the AIG (or the BLIF write
 *                                            and read-back), the caps and folding, the splice
 *                                            less provenance, provenance; outside_ms is their sum
 *   abc_ms namemap_ms namemap_cov_pct edges  ABC itself, the name map, LUT objects with an old
 *                                            literal (percent), provenance parent edges
 *   rss_before_mb rss_peak_mb                the process's RSS before `abc` and its peak during it
 *                                            (VmHWM after a reset through /proc/self/clear_refs);
 *                                            on an `exe` line, the executable's peak RSS
 * then three columns past §12's list:
 *   abc_nodes abc_latches abc_levels         ABC's own `print_stats` line (nd, lat, lev) of the
 *                                            run's last, compared between the linked and `exe`
 *                                            lines (spec §12: equal LUT, FF and level counts)
 *   aig_objs bytes_per_aig_obj               the calibration hook: AIG objects built (in-memory
 *                                            exchange) and (peak - before) / aig_objs in bytes;
 *                                            ODIN3_ABC_BYTES_PER_AIG_OBJ is 1.5 x the worst over
 *                                            the 19, only upwards
 * A design with several clock domains has one line per domain; the RSS columns are those of the
 * whole `abc` call. Exit 0 on success, 1 when a read, the pass or the executable fails (the
 * diagnostics on stderr), 2 on a usage error or a Debug build (bench_guard.h).
 */
#include "bench_guard.h"
#include "ir/design.h"
#include "odin3_abc.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/engine.h"
#include "passes/abc/script.h"
#include "passes/manager.h"
#include "util/clock.h"
#include "util/log.h"

#include <fcntl.h>
#include <inttypes.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum {
    MAX_RUNS = 64,
    PATH_BYTES = 1024,
    ARGS_BYTES = 2 * PATH_BYTES,
    LINE_BYTES = 4096,
    DEFAULT_LUT = 6,
    KIB_PER_MIB = 1024,
    MICRO_REPEATS = 10,
    MICRO_LINES = 1000,
    MICRO_LINE_BYTES = 64
};

static const char k_default_techlib[] = "lib/vtr.o3lib";
static const char k_header[] =
    "design\tmodule\tdomain\texchange\tcells\tffs\tluts\tdepth\tclassify_ms\taig_ms\tcheck_ms\t"
    "splice_ms\tprov_ms\toutside_ms\tabc_ms\tnamemap_ms\tnamemap_cov_pct\tedges\trss_before_mb\t"
    "rss_peak_mb\tabc_nodes\tabc_latches\tabc_levels\taig_objs\tbytes_per_aig_obj";

typedef struct bench_args {
    const char *file;
    const char *via_blif;
    const char *exe;
    const char *techlib;
    unsigned lut;
    bool header;
    bool micro;
} bench_args;

/* ABC's print_stats figures of one run (-1: not seen). */
typedef struct abc_figures {
    long nodes, latches, levels;
} abc_figures;

typedef struct bench_state {
    odin3_abc_run_report reports[MAX_RUNS];
    abc_figures figures[MAX_RUNS];
    uint32_t count;
    abc_figures pending; /* the last print_stats line seen since the previous report */
} bench_state;

/* --- reading ABC's print_stats line ---------------------------------------------------------- */

/* The number after `key` (e.g. " nd =") in text, or -1. */
static long figure_after(const char *text, const char *key) {
    const char *at = strstr(text, key);
    if (at == NULL) {
        return -1;
    }
    char *end = NULL;
    long value = strtol(at + strlen(key), &end, 10);
    return end != at + strlen(key) ? value : -1;
}

/* True when text is a print_stats line (`... i/o = a/b  lat = c  nd = d  edge = ... lev = e`). */
static bool parse_stats(const char *text, abc_figures *out) {
    long nodes = figure_after(text, " nd =");
    if (nodes < 0 || strstr(text, " lev =") == NULL) {
        return false;
    }
    out->nodes = nodes;
    out->latches = figure_after(text, " lat =");
    out->levels = figure_after(text, " lev =");
    return true;
}

/* The log sink: warnings and errors to stderr; ABC's own lines (`abc| `) for print_stats. */
static void bench_sink(odin3_log_level level, const char *msg, void *user) {
    bench_state *state = user;
    if (level <= ODIN3_LOG_WARN) {
        (void)fprintf(stderr, "bench_abc: %s\n", msg);
    } else if (strstr(msg, "abc| ") != NULL) {
        (void)parse_stats(msg, &state->pending);
    }
}

static void on_report(const odin3_abc_run_report *report, void *user) {
    bench_state *state = user;
    if (state->count < MAX_RUNS) {
        state->reports[state->count] = *report;
        state->figures[state->count] = state->pending;
        state->count++;
    }
    state->pending = (abc_figures){-1, -1, -1};
}

/* --- memory ---------------------------------------------------------------------------------- */

/* The KiB after `key` (e.g. "VmHWM:") in /proc/self/status, or -1. */
static long status_kib(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[LINE_BYTES / 8];
    long value = -1;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, strlen(key)) == 0) {
            value = strtol(line + strlen(key), NULL, 10);
            break;
        }
    }
    (void)fclose(f);
    return value;
}

/* Resets the kernel's high-water mark to the current RSS; false when that is unavailable. */
static bool reset_peak(void) {
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (f == NULL) {
        return false;
    }
    bool ok = fputs("5", f) >= 0;
    return fclose(f) == 0 && ok;
}

static double mib(long kib) {
    return kib < 0 ? -1.0 : (double)kib / KIB_PER_MIB;
}

/* --- the pinned executable -------------------------------------------------------------------- */

typedef struct exe_result {
    double ms;
    long peak_kib;
    abc_figures figures;
} exe_result;

/* The file's whole text (malloc), or NULL. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    char *text = size >= 0 ? calloc((size_t)size + 1U, 1U) : NULL;
    if (text != NULL && fread(text, 1U, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    (void)fclose(f);
    return text;
}

/* Writes text to path with every `from` replaced by `to`; false on any failure. */
static bool write_replaced(const char *path, const char *text, const char *from, const char *to) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = true;
    const char *at = text;
    for (const char *hit = strstr(at, from); hit != NULL; hit = strstr(at, from)) {
        ok = ok && fwrite(at, 1U, (size_t)(hit - at), f) == (size_t)(hit - at);
        ok = ok && fputs(to, f) >= 0;
        at = hit + strlen(from);
    }
    ok = ok && fputs(at, f) >= 0;
    return fclose(f) == 0 && ok;
}

/* The last print_stats line of the executable's log. */
static void read_exe_figures(const char *log_path, abc_figures *out) {
    char *text = slurp(log_path);
    *out = (abc_figures){-1, -1, -1};
    if (text == NULL) {
        return;
    }
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        (void)parse_stats(line, out);
    }
    free(text);
}

/* Spawns `exe -f cmd` with stdout and stderr in log_path; false when it fails or exits non-zero. */
static bool spawn_exe(const char *exe, const char *cmd, const char *log_path, exe_result *out) {
    posix_spawn_file_actions_t actions;
    char *argv[] = {(char *)(uintptr_t)exe, (char *)(uintptr_t) "-f", (char *)(uintptr_t)cmd, NULL};
    pid_t pid = 0;
    int status = 0;
    struct rusage usage;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    int rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path,
                                              O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    rc = rc == 0 ? posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) : rc;
    double start = odin3_now_ms();
    rc = rc == 0 ? posix_spawn(&pid, exe, &actions, NULL, argv, environ) : rc;
    (void)posix_spawn_file_actions_destroy(&actions);
    if (rc != 0 || wait4(pid, &status, 0, &usage) != pid) {
        return false;
    }
    out->ms = odin3_now_ms() - start;
    out->peak_kib = usage.ru_maxrss;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Runs the pinned executable on run r's exchange files in dir; its figures in out. */
static bool run_exe(const bench_args *args, uint32_t run_no, exe_result *out) {
    char cmd[PATH_BYTES];
    char exe_cmd[PATH_BYTES];
    char out_name[PATH_BYTES / 4];
    char exe_out[PATH_BYTES / 4];
    char log_path[PATH_BYTES];
    (void)snprintf(cmd, sizeof cmd, "%s/abc%" PRIu32 ".cmd", args->via_blif, run_no);
    (void)snprintf(exe_cmd, sizeof exe_cmd, "%s/abc%" PRIu32 ".exe.cmd", args->via_blif, run_no);
    (void)snprintf(log_path, sizeof log_path, "%s/abc%" PRIu32 ".exe.log", args->via_blif, run_no);
    (void)snprintf(out_name, sizeof out_name, "abc%" PRIu32 ".out.blif", run_no);
    (void)snprintf(exe_out, sizeof exe_out, "abc%" PRIu32 ".exe.out.blif", run_no);
    char *text = slurp(cmd);
    bool ok = text != NULL && write_replaced(exe_cmd, text, out_name, exe_out);
    free(text);
    ok = ok && spawn_exe(args->exe, exe_cmd, log_path, out);
    if (!ok) {
        (void)fprintf(stderr, "bench_abc: the executable failed on %s (see %s)\n", cmd, log_path);
        return false;
    }
    read_exe_figures(log_path, &out->figures);
    return true;
}

/* --- the lines ------------------------------------------------------------------------------- */

typedef struct memory {
    long before_kib, peak_kib;
} memory;

static void print_figure(long value) {
    if (value < 0) {
        (void)fputs("\t-", stdout);
    } else {
        (void)printf("\t%ld", value);
    }
}

/* The design's name: the file's basename up to its first dot. */
static void design_name(const char *file, char *name, size_t cap) {
    const char *slash = strrchr(file, '/');
    const char *base = slash != NULL ? slash + 1 : file;
    size_t len = strcspn(base, ".");
    (void)snprintf(name, cap, "%.*s", (int)len, base);
}

static void print_linked(const char *design, const odin3_abc_run_report *rep,
                         const abc_figures *abc, const memory *mem) {
    double outside = rep->classify_ms + rep->aig_ms + rep->check_ms + rep->splice_ms + rep->prov_ms;
    (void)printf("%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u", design, rep->module, rep->domain,
                 rep->exchange == ODIN3_ABC_EXCHANGE_AIG ? "aig" : "blif", (unsigned)rep->cells,
                 (unsigned)rep->ffs, (unsigned)rep->luts, (unsigned)rep->depth);
    (void)printf("\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f", rep->classify_ms, rep->aig_ms,
                 rep->check_ms, rep->splice_ms, rep->prov_ms, outside, rep->abc_ms,
                 rep->namemap_ms);
    if (rep->namemap_total == 0) {
        (void)fputs("\t-", stdout);
    } else {
        (void)printf("\t%.1f", 100.0 * rep->namemap_hit / rep->namemap_total);
    }
    (void)printf("\t%" PRIu64 "\t%.1f\t%.1f", rep->edges, mib(mem->before_kib), mib(mem->peak_kib));
    print_figure(abc->nodes);
    print_figure(abc->latches);
    print_figure(abc->levels);
    if (rep->aig_objs == 0 || mem->peak_kib < mem->before_kib) {
        (void)fputs("\t-\t-\n", stdout);
    } else {
        (void)printf("\t%u\t%.1f\n", (unsigned)rep->aig_objs,
                     (double)(mem->peak_kib - mem->before_kib) * KIB_PER_MIB / rep->aig_objs);
    }
}

static void print_exe(const char *design, const odin3_abc_run_report *rep, const exe_result *exe) {
    (void)printf("%s\t%s\t%s\texe\t-", design, rep->module, rep->domain);
    print_figure(exe->figures.latches);
    print_figure(exe->figures.nodes);
    print_figure(exe->figures.levels);
    (void)printf("\t-\t-\t-\t-\t-\t-\t%.3f\t-\t-\t-\t-\t%.1f", exe->ms, mib(exe->peak_kib));
    print_figure(exe->figures.nodes);
    print_figure(exe->figures.latches);
    print_figure(exe->figures.levels);
    (void)fputs("\t-\t-\n", stdout);
}

static int print_all(const bench_args *args, const bench_state *state, const memory *mem) {
    char name[PATH_BYTES / 4];
    design_name(args->file, name, sizeof name);
    if (args->header) {
        (void)puts(k_header);
    }
    int rc = 0;
    for (uint32_t i = 0; i < state->count; i++) {
        print_linked(name, &state->reports[i], &state->figures[i], mem);
        exe_result exe = {0, -1, {-1, -1, -1}};
        if (args->exe != NULL && run_exe(args, state->reports[i].run_no, &exe)) {
            print_exe(name, &state->reports[i], &exe);
        } else if (args->exe != NULL) {
            rc = 1;
        }
    }
    return rc;
}

/* --- --micro: open + close and the script checker --------------------------------------------- */

static void ignore_text(void *user, const char *line) {
    (void)user;
    (void)line;
}

/* The worst of MICRO_REPEATS `open` + `close` of the registered engine, in ms; -1 on a failure. */
static double time_open_close(void) {
    const odin3_abc_engine *eng = odin3_abc_engine_get();
    double worst = 0.0;
    for (int i = 0; eng != NULL && i < MICRO_REPEATS; i++) {
        odin3_abc_fail fail = {0};
        double start = odin3_now_ms();
        if (eng->open(eng->self, ignore_text, NULL, &fail) != ODIN3_OK) {
            return -1.0;
        }
        eng->close(eng->self);
        double spent = odin3_now_ms() - start;
        worst = spent > worst ? spent : worst;
    }
    return eng != NULL ? worst : -1.0;
}

/* A 64 KiB script: MICRO_LINES `balance` commands, each padded with a comment, then `if -K 6`. */
static size_t make_script(char *text, size_t cap) {
    size_t len = 0;
    for (int i = 0; i < MICRO_LINES && len + MICRO_LINE_BYTES + 1U < cap; i++) {
        len +=
            (size_t)snprintf(text + len, cap - len, "balance # %0*d\n", MICRO_LINE_BYTES - 11, 0);
    }
    return len + (size_t)snprintf(text + len, cap - len, "if -K 6\n");
}

/* The worst of MICRO_REPEATS checks of that script, in ms; -1 when it is refused. */
static double time_checker(size_t *bytes) {
    static char text[ODIN3_ABC_MAX_SCRIPT_BYTES];
    *bytes = make_script(text, sizeof text);
    const odin3_abc_script_src src = {"micro.abc", "micro.abc", {(const uint8_t *)text, *bytes}};
    double worst = 0.0;
    for (int i = 0; i < MICRO_REPEATS; i++) {
        odin3_abc_script script;
        double start = odin3_now_ms();
        odin3_status st =
            odin3_abc_script_parse(&src, DEFAULT_LUT, ODIN3_ABC_EXCHANGE_BLIF, &script);
        double spent = odin3_now_ms() - start;
        if (st != ODIN3_OK) {
            return -1.0;
        }
        odin3_abc_script_free(&script);
        worst = spent > worst ? spent : worst;
    }
    return worst;
}

static int bench_micro(void) {
    size_t bytes = 0;
    odin3_status st = odin3_abc_adapter_init();
    double open_close = st == ODIN3_OK ? time_open_close() : -1.0;
    double checker = time_checker(&bytes);
    if (open_close < 0.0 || checker < 0.0) {
        (void)fprintf(stderr, "bench_abc: --micro failed (engine: %s)\n", odin3_status_string(st));
        return 1;
    }
    (void)printf("micro\topen_close_ms\t%.3f\tchecker_ms\t%.3f\tchecker_bytes\t%zu\n", open_close,
                 checker, bytes);
    return 0;
}

/* --- the run --------------------------------------------------------------------------------- */

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

/* read_techlib and read_blif; false (diagnostics on stderr) when either fails. */
static bool read_design(const bench_args *args, odin3_design *design) {
    const char *techlib = args->techlib;
    if (techlib == NULL) {
        techlib = getenv("ODIN3_BENCH_TECHLIB");
    }
    if (techlib == NULL && file_exists(k_default_techlib)) {
        techlib = k_default_techlib;
    }
    if (techlib != NULL &&
        odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(techlib)) != ODIN3_OK) {
        return false;
    }
    return odin3_pass_run(design, "read_blif", odin3_bytes_cstr(args->file)) == ODIN3_OK;
}

static int bench_one(const bench_args *args) {
    bench_state state = {0};
    state.pending = (abc_figures){-1, -1, -1};
    char pass_args[ARGS_BYTES];
    odin3_status st = odin3_abc_adapter_init(); /* before any pass (3D plan finding 20) */
    odin3_design *design = st == ODIN3_OK ? odin3_design_create() : NULL;
    if (design == NULL) {
        (void)fprintf(stderr, "bench_abc: linked ABC unavailable: %s\n", odin3_status_string(st));
        return 1;
    }
    odin3_log_set_level(ODIN3_LOG_INFO);
    odin3_log_set_sink(bench_sink, &state);
    (void)snprintf(pass_args, sizeof pass_args, "--lut %u%s%s", args->lut,
                   args->via_blif != NULL ? " --via-blif " : "",
                   args->via_blif != NULL ? args->via_blif : "");
    int rc = read_design(args, design) ? 0 : 1;
    memory mem = {status_kib("VmRSS:"), -1};
    bool hwm = rc == 0 && reset_peak();
    odin3_abc_test_set_report(on_report, &state);
    rc = rc == 0 && odin3_pass_run(design, "abc", odin3_bytes_cstr(pass_args)) != ODIN3_OK ? 1 : rc;
    odin3_abc_test_set_report(NULL, NULL);
    mem.peak_kib = hwm ? status_kib("VmHWM:") : status_kib("VmRSS:");
    odin3_log_set_sink(NULL, NULL);
    rc = rc == 0 ? print_all(args, &state, &mem) : rc;
    odin3_design_destroy(design);
    return rc;
}

static int usage(void) {
    (void)fprintf(stderr, "usage: bench_abc --one FILE.blif [--lut K] [--via-blif DIR] [--exe ABC] "
                          "[--techlib T] [--header]\n");
    return 2;
}

/* A path or directory the pass's argument parser would split or the shell would mangle. */
static bool plain_path(const char *path) {
    return path == NULL || (strpbrk(path, " \t\"'\\;#") == NULL && strlen(path) < PATH_BYTES / 2);
}

static bool parse_args(int argc, char **argv, bench_args *args) {
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (strcmp(argv[i], "--one") == 0 && more) {
            args->file = argv[++i];
        } else if (strcmp(argv[i], "--lut") == 0 && more) {
            args->lut = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--via-blif") == 0 && more) {
            args->via_blif = argv[++i];
        } else if (strcmp(argv[i], "--exe") == 0 && more) {
            args->exe = argv[++i];
        } else if (strcmp(argv[i], "--techlib") == 0 && more) {
            args->techlib = argv[++i];
        } else if (strcmp(argv[i], "--header") == 0) {
            args->header = true;
        } else if (strcmp(argv[i], "--micro") == 0) {
            args->micro = true;
        } else {
            return false;
        }
    }
    bool paths_ok = plain_path(args->file) && plain_path(args->via_blif) && plain_path(args->exe);
    if (args->micro) {
        return args->file == NULL && args->via_blif == NULL && args->exe == NULL;
    }
    return args->file != NULL && args->lut >= 2 && paths_ok &&
           (args->exe == NULL || args->via_blif != NULL);
}

int main(int argc, char **argv) {
    if (bench_refuse_unoptimized(argv[0]) != 0) {
        return 2;
    }
    bench_args args = {NULL, NULL, NULL, NULL, DEFAULT_LUT, false, false};
    if (!parse_args(argc, argv, &args)) {
        return usage();
    }
    return args.micro ? bench_micro() : bench_one(&args);
}
