/* odin3-blif-rt.c — BLIF round-trip driver: load tech libraries, read, check FULL, write. */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "techlib/reader.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { RT_OK = 0, RT_FAIL = 1, RT_USAGE = 2, RT_PATHS = 2 };

/* Command line: tech libraries (argv entries, in order) and the two BLIF paths. */
typedef struct rt_args {
    char **libs;
    int n_libs;
    const char *in_path;
    const char *out_path;
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

static int round_trip(const rt_args *args) {
    int rc = RT_FAIL;
    odin3_design *design = odin3_design_create();

    if (design == NULL) {
        (void)fprintf(stderr, "odin3-blif-rt: out of memory\n");
        return RT_FAIL;
    }
    if (read_design(design, args) == ODIN3_OK) {
        report_check(design, args->in_path);
        if (odin3_blif_write(design, args->out_path) == ODIN3_OK) {
            rc = RT_OK;
        }
    }
    odin3_design_destroy(design);
    return rc;
}

/* Parses `[--techlib lib.o3lib]... in.blif out.blif`; false on a usage error. Library paths are
 * gathered at the front of argv, so args->libs points into it. */
static bool parse_args(int argc, char **argv, rt_args *args) {
    const char *paths[RT_PATHS] = {NULL, NULL};
    int n_paths = 0;
    *args = (rt_args){.libs = argv + 1};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--techlib") == 0 && i + 1 < argc) {
            argv[1 + args->n_libs++] = argv[++i];
        } else if (argv[i][0] != '-' && n_paths < RT_PATHS) {
            paths[n_paths++] = argv[i];
        } else {
            return false;
        }
    }
    args->in_path = paths[0];
    args->out_path = paths[1];
    return n_paths == RT_PATHS;
}

int main(int argc, char **argv) {
    rt_args args;
    if (!parse_args(argc, argv, &args)) {
        (void)fprintf(stderr, "usage: odin3-blif-rt [--techlib lib.o3lib]... in.blif out.blif\n");
        return RT_USAGE;
    }
    return round_trip(&args);
}
