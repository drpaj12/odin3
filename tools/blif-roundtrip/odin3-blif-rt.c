/* odin3-blif-rt.c — BLIF round-trip driver: load tech libraries, read, check FULL, write. */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "techlib/reader.h"
#include "util/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exit codes: written; refused by the writer (its lines on stderr); I/O, read or usage error. */
enum { RT_OK = 0, RT_REFUSED = 1, RT_ERROR = 2, RT_PATHS = 2, RT_DECIMAL = 10 };

/* Command line: tech libraries (argv entries, in order), the dialect and the two BLIF paths. */
typedef struct rt_args {
    char **libs;
    int n_libs;
    const char *in_path;
    const char *out_path;
    odin3_blif_write_opts opts; /* PLAIN unless --vtr; lut_k 0 = the writer's default */
    bool lut_k_given;
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
    int rc = RT_ERROR;
    odin3_design *design = odin3_design_create();

    if (design == NULL) {
        (void)fprintf(stderr, "odin3-blif-rt: out of memory\n");
        return RT_ERROR;
    }
    if (read_design(design, args) == ODIN3_OK) {
        report_check(design, args->in_path);
        odin3_status st = odin3_blif_write_with(design, args->out_path, &args->opts);
        if (st == ODIN3_OK) {
            rc = RT_OK;
        } else if (st == ODIN3_ERR_INVALID_ARG) {
            rc = RT_REFUSED; /* a --vtr preflight refusal, or a design plain BLIF cannot hold */
        }
    }
    odin3_design_destroy(design);
    return rc;
}

/* Parses --lut-k's value: a decimal in 2..16; false otherwise. */
static bool parse_lut_k(const char *text, uint32_t *lut_k) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, RT_DECIMAL);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-' ||
        value < ODIN3_BLIF_VTR_MIN_K || value > ODIN3_BLIF_VTR_MAX_K) {
        return false;
    }
    *lut_k = (uint32_t)value;
    return true;
}

/* Parses one option at argv[*i] (advancing *i past its value); false on a usage error. */
static bool parse_option(int argc, char **argv, int *i, rt_args *args) {
    const char *opt = argv[*i];
    if (strcmp(opt, "--vtr") == 0 && args->opts.dialect != ODIN3_BLIF_DIALECT_VTR) {
        args->opts.dialect = ODIN3_BLIF_DIALECT_VTR;
        return true;
    }
    if (*i + 1 >= argc) {
        return false;
    }
    if (strcmp(opt, "--techlib") == 0) {
        argv[1 + args->n_libs++] = argv[++*i];
        return true;
    }
    if (strcmp(opt, "--lut-k") == 0 && !args->lut_k_given) {
        args->lut_k_given = true;
        return parse_lut_k(argv[++*i], &args->opts.lut_k);
    }
    return false;
}

/* Parses `[--techlib lib.o3lib]... [--vtr [--lut-k K]] in.blif out.blif`; false on a usage
 * error. Library paths are gathered at the front of argv, so args->libs points into it. */
static bool parse_args(int argc, char **argv, rt_args *args) {
    const char *paths[RT_PATHS] = {NULL, NULL};
    int n_paths = 0;
    *args = (rt_args){.libs = argv + 1, .opts = {.dialect = ODIN3_BLIF_DIALECT_PLAIN}};
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            if (!parse_option(argc, argv, &i, args)) {
                return false;
            }
        } else if (n_paths < RT_PATHS) {
            paths[n_paths++] = argv[i];
        } else {
            return false;
        }
    }
    args->in_path = paths[0];
    args->out_path = paths[1];
    /* --lut-k needs --vtr, as write_blif's own option does. */
    return n_paths == RT_PATHS &&
           (!args->lut_k_given || args->opts.dialect == ODIN3_BLIF_DIALECT_VTR);
}

int main(int argc, char **argv) {
    rt_args args;
    if (!parse_args(argc, argv, &args)) {
        (void)fprintf(stderr, "usage: odin3-blif-rt [--techlib lib.o3lib]... [--vtr [--lut-k K]] "
                              "in.blif out.blif\n");
        return RT_ERROR;
    }
    return round_trip(&args);
}
