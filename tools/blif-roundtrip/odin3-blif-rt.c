/* odin3-blif-rt.c — BLIF round-trip driver: load tech libraries, read, check FULL, write.
 *
 * `--vtr [--lut-k K]` writes the VTR dialect (spec 3E) through odin3_blif_write_with. Exit codes:
 * 0 written; 1 the write failed (plain mode: any read or write failure; --vtr: a refusal, whose
 * lines are on stderr, and no file); 2 usage, or in --vtr mode an I/O or read failure. */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "techlib/reader.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RT_OK = 0, RT_FAIL = 1, RT_USAGE = 2, RT_PATHS = 2, RT_DECIMAL = 10 };

/* Command line: tech libraries (argv entries, in order), the two BLIF paths and the dialect. */
typedef struct rt_args {
    char **libs;
    int n_libs;
    const char *in_path;
    const char *out_path;
    bool vtr;
    uint32_t lut_k;
} rt_args;

/* Checks the design at FULL and reports the error and warning counts on stderr. */
static void report_check(odin3_design *design, const char *path) {
    const odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};

    odin3_log_reset_counts();
    (void)odin3_check_design(design, opts);
    (void)fprintf(stderr, "%s: check FULL: %zu errors, %zu warnings\n", path,
                  odin3_log_count(ODIN3_LOG_ERROR), odin3_log_count(ODIN3_LOG_WARN));
}

/* Loads every tech library, then reads in_path (IR-7b: its black boxes resolve against them). */
static odin3_status read_design(odin3_design *design, const rt_args *args) {
    for (int i = 0; i < args->n_libs; i++) {
        odin3_status st = odin3_techlib_read(design, args->libs[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return odin3_blif_read(design, args->in_path);
}

/* Writes the design: the plain dialect, or with --vtr the VTR dialect. */
static odin3_status write_design(const odin3_design *design, const rt_args *args) {
    if (!args->vtr) {
        return odin3_blif_write(design, args->out_path);
    }
    const odin3_blif_write_opts opts = {
        .dialect = ODIN3_BLIF_DIALECT_VTR,
        .name_style = ODIN3_BLIF_NAMES_SHORT,
        .lut_k = args->lut_k,
    };
    return odin3_blif_write_with(design, args->out_path, &opts);
}

/* The exit code of a failed write: --vtr tells a refusal (1, INVALID_ARG) from an I/O error (2). */
static int write_failure(const rt_args *args, odin3_status st) {
    if (args->vtr && st != ODIN3_ERR_INVALID_ARG) {
        return RT_USAGE;
    }
    return RT_FAIL;
}

static int round_trip(const rt_args *args) {
    const int read_failure = args->vtr ? RT_USAGE : RT_FAIL;
    int rc = read_failure;
    odin3_design *design = odin3_design_create();

    if (design == NULL) {
        (void)fprintf(stderr, "odin3-blif-rt: out of memory\n");
        return read_failure;
    }
    if (read_design(design, args) == ODIN3_OK) {
        report_check(design, args->in_path);
        const odin3_status st = write_design(design, args);
        rc = (st == ODIN3_OK) ? RT_OK : write_failure(args, st);
    }
    odin3_design_destroy(design);
    return rc;
}

/* Parses the decimal K of `--lut-k K`; false when it is not a whole number in 1..UINT32_MAX. */
static bool parse_lut_k(const char *text, uint32_t *k) {
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, RT_DECIMAL);
    if (*text == '\0' || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return false;
    }
    *k = (uint32_t)value;
    return true;
}

/* Parses `[--techlib lib.o3lib]... [--vtr [--lut-k K]] in.blif out.blif`; false on a usage
 * error. Library paths are gathered at the front of argv, so args->libs points into it. */
static bool parse_args(int argc, char **argv, rt_args *args) {
    const char *paths[RT_PATHS] = {NULL, NULL};
    int n_paths = 0;
    bool have_k = false;
    *args = (rt_args){.libs = argv + 1};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--techlib") == 0 && i + 1 < argc) {
            argv[1 + args->n_libs++] = argv[++i];
        } else if (strcmp(argv[i], "--vtr") == 0) {
            args->vtr = true;
        } else if (strcmp(argv[i], "--lut-k") == 0 && i + 1 < argc) {
            have_k = true;
            if (!parse_lut_k(argv[++i], &args->lut_k)) {
                return false;
            }
        } else if (argv[i][0] != '-' && n_paths < RT_PATHS) {
            paths[n_paths++] = argv[i];
        } else {
            return false;
        }
    }
    args->in_path = paths[0];
    args->out_path = paths[1];
    return n_paths == RT_PATHS && (args->vtr || !have_k);
}

int main(int argc, char **argv) {
    rt_args args;
    if (!parse_args(argc, argv, &args)) {
        (void)fprintf(
            stderr,
            "usage: odin3-blif-rt [--techlib lib.o3lib]... [--vtr [--lut-k K]] in.blif out.blif\n");
        return RT_USAGE;
    }
    return round_trip(&args);
}
