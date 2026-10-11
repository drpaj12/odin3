/*
 * bench_pp.c — preprocessor benchmark (2B spec §10.6): preprocesses every VTR benchmark
 * (*.v of one directory), smallest first and mcml.v last, each with a fresh design, and reports
 * per file the bytes in and out, lines, segments, buffers, definitions made and live macros,
 * source-manager bytes, the
 * preprocessor's own heap and the best time of RUNS runs; then the targets (mcml.v <= 50 ms,
 * the set <= 0.5 s, the preprocessor's heap <= 3 x input bytes beside the srcman records).
 * Run it from a Release build.
 * Usage: bench_pp [dir]   (default $VTR_ROOT/vtr_flow/benchmarks/verilog)
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "ir/design.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

enum { PATH_MAX_LEN = 1024, MAX_FILES = 256, RUNS = 5, MAP_ENTRY_BYTES = 16 };

static const double MCML_TARGET = 0.050;
static const double SET_TARGET = 0.5;
static const double MEMORY_FACTOR = 3.0;

/* One benchmark file. */
typedef struct bench_file {
    char name[256];
    char path[PATH_MAX_LEN];
    long size;
} bench_file;

/* What one run measured. */
typedef struct result {
    double secs; /* best of RUNS */
    size_t out_bytes;
    uint32_t segments;
    uint32_t buffers;
    uint32_t lines;
    size_t defs;   /* definitions made (redefinitions included) */
    size_t macros; /* live macro names at the end */
    size_t srcman_bytes;
    size_t pp_bytes;
    int check;
} result;

static bench_file files[MAX_FILES];
static size_t nfiles;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long max_rss_kib(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
}

static const char *verdict(bool ok) {
    return ok ? "ok" : "MISSED";
}

static bool is_mcml(const bench_file *file) {
    return strcmp(file->name, "mcml.v") == 0;
}

/* Smallest first, ties by name; mcml.v last whatever its size (spec §10.6). */
static int order(const void *lhs, const void *rhs) {
    const bench_file *left = lhs;
    const bench_file *right = rhs;
    if (is_mcml(left) != is_mcml(right)) {
        return is_mcml(left) ? 1 : -1;
    }
    if (left->size != right->size) {
        return left->size < right->size ? -1 : 1;
    }
    return strcmp(left->name, right->name);
}

static int list_files(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        fprintf(stderr, "bench_pp: cannot open %s\n", dir_path);
        return 1;
    }
    for (struct dirent *ent = readdir(dir); ent != NULL && nfiles < MAX_FILES; ent = readdir(dir)) {
        size_t len = strlen(ent->d_name);
        if (len < 3 || strcmp(ent->d_name + len - 2, ".v") != 0 || len >= sizeof files[0].name) {
            continue;
        }
        bench_file *file = &files[nfiles];
        (void)snprintf(file->name, sizeof file->name, "%s", ent->d_name);
        int n = snprintf(file->path, sizeof file->path, "%s/%s", dir_path, ent->d_name);
        struct stat info;
        if (n < 0 || (size_t)n >= sizeof file->path || stat(file->path, &info) != 0 ||
            !S_ISREG(info.st_mode)) {
            continue;
        }
        file->size = (long)info.st_size;
        nfiles++;
    }
    (void)closedir(dir);
    qsort(files, nfiles, sizeof files[0], order);
    return nfiles == 0;
}

static size_t vec_bytes(const odin3_vec *vec) {
    return vec->cap * vec->elem_size;
}

/* The preprocessor's own heap after a run: its vectors' capacity, the stream text, the file
 * cache, the macro arena and the maps (estimated at MAP_ENTRY_BYTES per entry). The source
 * manager's records are reported apart. */
static size_t pp_bytes(const odin3_pp *pp) {
    size_t total = sizeof *pp + pp->out.cap + (size_t)pp->cache_bytes;
    const odin3_vec *vecs[] = {&pp->macros, &pp->cache,   &pp->files_read, &pp->bufs,
                               &pp->frames, &pp->runs,    &pp->conds,      &pp->incstack,
                               &pp->args,   &pp->scratch, &pp->arg_starts};
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        total += vec_bytes(vecs[i]);
    }
    total += odin3_arena_bytes_reserved(pp->arena);
    total += MAP_ENTRY_BYTES *
             (odin3_u64map_count(pp->macro_by_name) + odin3_u64map_count(pp->cache_by_path) +
              odin3_u64map_count(pp->cache_by_buffer));
    return total;
}

/* One run on a fresh design; fills r (but secs) on the last run, when keep is set. */
static int run_once(const bench_file *file, bool keep, result *r, double *secs) {
    odin3_design *design = odin3_design_create();
    odin3_srcman *sm = NULL;
    odin3_pp *pp = NULL;
    uint32_t name = 0;
    uint32_t resolved = 0;
    uint32_t library = 0;
    int rc = design == NULL || odin3_design_get_srcman(design, &sm) != ODIN3_OK;
    rc = rc || odin3_design_intern(design, odin3_bytes_cstr(file->name), &name) != ODIN3_OK ||
         odin3_design_intern(design, odin3_bytes_cstr(file->path), &resolved) != ODIN3_OK ||
         odin3_design_intern(design, odin3_bytes_cstr("work"), &library) != ODIN3_OK;
    odin3_pp_config none = {NULL, 0, NULL, 0};
    odin3_pp_stream stream = {{NULL, 0}, {0}, 0};
    if (rc == 0) {
        odin3_pp_source src = {name, resolved, library, {0}};
        rc = odin3_pp_create(design, &none, &pp) != ODIN3_OK;
        double start = now_seconds();
        rc = rc || odin3_pp_run(pp, &src, &stream) != ODIN3_OK;
        *secs = now_seconds() - start;
    }
    if (rc == 0 && keep) {
        odin3_srcbuf_info info;
        (void)odin3_srcman_buffer_info(sm, pp->first_buffer, &info);
        *r = (result){0,
                      stream.text.len,
                      stream.nsegments,
                      pp->nbuffers,
                      info.lines,
                      pp->macros.len,
                      odin3_u64map_count(pp->macro_by_name),
                      odin3_srcman_bytes_reserved(sm),
                      pp_bytes(pp),
                      (int)odin3_pp_check(pp, stream.stream)};
    }
    odin3_pp_destroy(pp);
    odin3_design_destroy(design);
    return rc;
}

static int bench_one(const bench_file *file, result *r) {
    double best = -1;
    for (int i = 0; i < RUNS; i++) {
        double secs = 0;
        if (run_once(file, i == RUNS - 1, r, &secs) != 0) {
            fprintf(stderr, "bench_pp: %s failed\n", file->name);
            return 1;
        }
        best = best < 0 || secs < best ? secs : best;
    }
    r->secs = best;
    return 0;
}

static double heap_ratio(const bench_file *file, const result *r) {
    return (double)r->pp_bytes / (double)file->size;
}

static void print_row(const bench_file *file, const result *r) {
    printf("%-30s %8ld %8zu %7" PRIu32 " %7" PRIu32 " %6" PRIu32 " %5zu %6zu %9zu %9zu %7.2f %8.3f "
           "%s\n",
           file->name, file->size, r->out_bytes, r->lines, r->segments, r->buffers, r->defs,
           r->macros, r->srcman_bytes, r->pp_bytes, heap_ratio(file, r), r->secs * 1e3,
           r->check == 0 ? "ok" : "FAILED");
}

int main(int argc, char **argv) {
    char dir[PATH_MAX_LEN];
    if (argc > 1) {
        (void)snprintf(dir, sizeof dir, "%s", argv[1]);
    } else {
        const char *root = getenv("VTR_ROOT");
        (void)snprintf(dir, sizeof dir, "%s/vtr_flow/benchmarks/verilog",
                       root != NULL ? root : ".");
    }
    odin3_log_set_level(ODIN3_LOG_ERROR);
    if (list_files(dir) != 0) {
        fprintf(stderr, "bench_pp: no .v files in %s\n", dir);
        return 1;
    }
    printf("%-30s %8s %8s %7s %7s %6s %5s %6s %9s %9s %7s %8s %s\n", "file (smallest first)",
           "bytes in", "out", "lines", "segs", "bufs", "defs", "macros", "srcman B", "pp heap B",
           "heap/in", "best ms", "check");
    double total = 0;
    long total_in = 0;
    int rc = 0;
    result mcml = {0};
    bool have_mcml = false;
    size_t over = 0;
    for (size_t i = 0; i < nfiles; i++) {
        result r = {0};
        rc |= bench_one(&files[i], &r);
        if (rc != 0) {
            break;
        }
        print_row(&files[i], &r);
        total += r.secs;
        total_in += files[i].size;
        over += heap_ratio(&files[i], &r) > MEMORY_FACTOR ? 1 : 0;
        if (is_mcml(&files[i])) {
            mcml = r;
            have_mcml = true;
        }
    }
    printf("set: %zu files, %ld bytes in, %.3f s (sum of best of %d; target <= %.1f s: %s); pp "
           "heap over %.0f x input in %zu files\n",
           nfiles, total_in, total, RUNS, SET_TARGET, verdict(total <= SET_TARGET), MEMORY_FACTOR,
           over);
    if (have_mcml) {
        const bench_file *last = &files[nfiles - 1];
        double limit = MEMORY_FACTOR * (double)last->size;
        printf("mcml.v: %.3f ms (target <= %.0f ms: %s); pp heap %zu B = %.2f x input (target <= "
               "%.0f x: %s), srcman %zu B beside it\n",
               mcml.secs * 1e3, MCML_TARGET * 1e3, verdict(mcml.secs <= MCML_TARGET), mcml.pp_bytes,
               (double)mcml.pp_bytes / (double)last->size, MEMORY_FACTOR,
               verdict((double)mcml.pp_bytes <= limit), mcml.srcman_bytes);
    }
    printf("max RSS: %ld KiB\n", max_rss_kib());
    return rc;
}
