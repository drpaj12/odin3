/* odin3-sim-vectors.c — random-vector driver of the simulator: one line per simulated cycle. */
/*
 * Usage: odin3-sim-vectors in.blif --seed S --cycles N [--techlib lib.o3lib]... [--max-cells M]
 *
 * Loads the tech libraries (in order), reads in.blif, builds the simulator over its top model (the
 * first: the BLIF reader creates the file's models in file order, so module 1 is the first model)
 * and runs N cycles. --max-cells sets the flattening budget (odin3_sim_options; default 2^24 cells
 * and instances, sim/sim.h), so a small hierarchical file cannot expand past memory. Each cycle
 * drives every non-clock input bit from the seeded PRNG (odin3_sim_drive_random), runs
 * odin3_sim_cycle and prints `cycle inputs outputs`, in the format of sim/vectors.h (shared with
 * the `sim` pass) under the title `odin3-sim-vectors in.blif`. Output is a function of the
 * netlist and S.
 *
 * Exit status: 0 on success; 1 when a library or the BLIF cannot be read or the design cannot be
 * simulated (the reason is logged on stderr); 2 on a usage error.
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/vectors.h"
#include "techlib/reader.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SV_OK = 0, SV_FAIL = 1, SV_USAGE = 2, SV_DECIMAL = 10 };

/* Command line. Library paths are gathered at the front of argv, so libs points into it. */
typedef struct sv_args {
    char **libs;
    int n_libs;
    const char *in_path;
    uint64_t seed;
    uint32_t cycles;
    uint32_t max_cells; /* 0: the default budget */
    bool has_seed;
    bool has_cycles;
} sv_args;

/* A decimal number of at most max (digits only: no sign, no blanks). */
typedef struct sv_number {
    const char *text;
    uint64_t max;
} sv_number;

static bool parse_number(sv_number num, uint64_t *out) {
    const char *text = num.text;
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long val = strtoull(text, &end, SV_DECIMAL);
    if (errno != 0 || *end != '\0' || val > num.max) {
        return false;
    }
    *out = (uint64_t)val;
    return true;
}

/* Parses the option at argv[*at] and its value, advancing *at past the value. */
static bool parse_option(char **argv, int argc, int *at, sv_args *args) {
    const char *opt = argv[*at];
    if (*at + 1 >= argc) {
        return false;
    }
    const char *val = argv[++*at];
    uint64_t num = 0;
    if (strcmp(opt, "--techlib") == 0) {
        argv[1 + args->n_libs++] = argv[*at];
        return true;
    }
    if (strcmp(opt, "--seed") == 0) {
        args->has_seed = parse_number((sv_number){val, UINT64_MAX}, &args->seed);
        return args->has_seed;
    }
    if (strcmp(opt, "--cycles") == 0 && parse_number((sv_number){val, UINT32_MAX}, &num)) {
        args->cycles = (uint32_t)num;
        args->has_cycles = true;
        return true;
    }
    if (strcmp(opt, "--max-cells") == 0 && parse_number((sv_number){val, UINT32_MAX}, &num) &&
        num > 0) {
        args->max_cells = (uint32_t)num;
        return true;
    }
    return false;
}

/* Parses `in.blif --seed S --cycles N [--techlib lib.o3lib]... [--max-cells M]` in any order. */
static bool parse_args(int argc, char **argv, sv_args *args) {
    *args = (sv_args){.libs = argv + 1};
    for (int i = 1; i < argc; i++) {
        bool ok = false;
        if (strncmp(argv[i], "--", 2) == 0) {
            ok = parse_option(argv, argc, &i, args);
        } else if (args->in_path == NULL) {
            args->in_path = argv[i];
            ok = true;
        }
        if (!ok) {
            return false;
        }
    }
    return args->in_path != NULL && args->has_seed && args->has_cycles;
}

/* Loads every tech library, then reads the BLIF (its black boxes resolve against them). */
static odin3_status read_design(odin3_design *design, const sv_args *args) {
    for (int i = 0; i < args->n_libs; i++) {
        odin3_status st = odin3_techlib_read(design, args->libs[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return odin3_blif_read(design, args->in_path);
}

/* Prints the header and one line per cycle (sim/vectors.h); false on out of memory. */
static bool run_cycles(odin3_sim *sim, const sv_args *args) {
    odin3_strbuf title;
    odin3_strbuf line;
    odin3_strbuf_init(&title);
    odin3_strbuf_init(&line);
    odin3_status st = odin3_strbuf_appendf(&title, "odin3-sim-vectors %s", args->in_path);
    if (st == ODIN3_OK) {
        st = odin3_sim_vectors_header(
            sim, &(odin3_sim_vectors){title.data, args->seed, args->cycles}, &line);
    }
    (void)fputs(st == ODIN3_OK ? line.data : "", stdout);
    odin3_prng prng;
    odin3_prng_seed(&prng, args->seed);
    for (uint32_t cyc = 0; st == ODIN3_OK && cyc < args->cycles; cyc++) {
        odin3_strbuf_clear(&line);
        st = odin3_sim_vectors_step(sim, &prng, cyc, &line);
        (void)fputs(st == ODIN3_OK ? line.data : "", stdout);
    }
    odin3_strbuf_free(&title);
    odin3_strbuf_free(&line);
    return st == ODIN3_OK;
}

static int simulate(const sv_args *args) {
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        (void)fprintf(stderr, "odin3-sim-vectors: out of memory\n");
        return SV_FAIL;
    }
    int rc = SV_FAIL;
    odin3_sim *sim = NULL;
    if (read_design(design, args) == ODIN3_OK &&
        odin3_sim_build_opts(design, (odin3_module_id){1},
                             &(odin3_sim_options){.max_cells = args->max_cells},
                             &sim) == ODIN3_OK) {
        bool ran = run_cycles(sim, args);
        rc = fflush(stdout) == 0 && ran ? SV_OK : SV_FAIL;
    }
    odin3_sim_destroy(sim);
    odin3_design_destroy(design);
    return rc;
}

int main(int argc, char **argv) {
    sv_args args;
    if (!parse_args(argc, argv, &args)) {
        (void)fprintf(stderr, "usage: odin3-sim-vectors in.blif --seed S --cycles N "
                              "[--techlib lib.o3lib]... [--max-cells M]\n");
        return SV_USAGE;
    }
    return simulate(&args);
}
