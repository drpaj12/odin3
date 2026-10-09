/* odin3-write.c — writer driver for tools/writer-check: read BLIF, write JSON, Verilog, dot. */
#include "backends/dot/writer.h"
#include "backends/json/writer.h"
#include "backends/verilog/writer.h"
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WR_OK = 0, WR_FAIL = 1, WR_USAGE = 2, NS_PER_S = 1000000000 };

typedef struct wr_args {
    const char *in;
    const char *json;
    const char *verilog;
    const char *dot;
    size_t max_nodes;
} wr_args;

static double now_seconds(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / NS_PER_S;
}

static void usage(void) {
    (void)fprintf(stderr, "usage: odin3-write in.blif [--json out.json] [--verilog out.v] "
                          "[--dot out.dot] [--max-nodes N]\n");
}

/* The value after argv[*at], or NULL (and a usage message) when there is none. */
static const char *option_value(int argc, char **argv, int *at) {
    if (*at + 1 >= argc) {
        usage();
        return NULL;
    }
    *at += 1;
    return argv[*at];
}

/* Fills args from argv; false on a usage error. */
static bool parse_args(int argc, char **argv, wr_args *args) {
    for (int i = 1; i < argc; i++) {
        const char **slot = NULL;
        if (strcmp(argv[i], "--json") == 0) {
            slot = &args->json;
        } else if (strcmp(argv[i], "--verilog") == 0) {
            slot = &args->verilog;
        } else if (strcmp(argv[i], "--dot") == 0) {
            slot = &args->dot;
        } else if (strcmp(argv[i], "--max-nodes") == 0) {
            const char *num = option_value(argc, argv, &i);
            if (num == NULL) {
                return false;
            }
            args->max_nodes = (size_t)strtoull(num, NULL, 10);
            continue;
        } else if (argv[i][0] != '-' && args->in == NULL) {
            args->in = argv[i];
            continue;
        } else {
            usage();
            return false;
        }
        *slot = option_value(argc, argv, &i);
        if (*slot == NULL) {
            return false;
        }
    }
    if (args->in == NULL) {
        usage();
    }
    return args->in != NULL;
}

/* One line per step on stderr: "odin3-write: <step> <result> <seconds> s". */
static void report(const char *step, const char *result, double start) {
    (void)fprintf(stderr, "odin3-write: %s %s %.2f s\n", step, result, now_seconds() - start);
}

/* A dot refusal with no focus is the node budget at work: reported as "over-budget", not a
 * failure. */
static int write_dot(const odin3_design *design, const wr_args *args) {
    odin3_dot_opts opts = {ODIN3_DOT_FOCUS_NONE, NULL, args->max_nodes};
    double start = now_seconds();
    odin3_status st = odin3_dot_write(design, args->dot, &opts);
    if (st == ODIN3_ERR_INVALID_ARG) {
        report("dot", "over-budget", start);
        return WR_OK;
    }
    report("dot", st == ODIN3_OK ? "ok" : "FAIL", start);
    return st == ODIN3_OK ? WR_OK : WR_FAIL;
}

static int write_all(const odin3_design *design, const wr_args *args) {
    int rc = WR_OK;
    if (args->json != NULL) {
        double start = now_seconds();
        odin3_status st = odin3_json_write(design, args->json);
        report("json", st == ODIN3_OK ? "ok" : "FAIL", start);
        rc = st == ODIN3_OK ? rc : WR_FAIL;
    }
    if (args->verilog != NULL) {
        double start = now_seconds();
        odin3_status st = odin3_verilog_write(design, args->verilog, NULL);
        report("verilog", st == ODIN3_OK ? "ok" : "FAIL", start);
        rc = st == ODIN3_OK ? rc : WR_FAIL;
    }
    if (args->dot != NULL && write_dot(design, args) != WR_OK) {
        rc = WR_FAIL;
    }
    return rc;
}

int main(int argc, char **argv) {
    wr_args args = {NULL, NULL, NULL, NULL, 0};
    if (!parse_args(argc, argv, &args)) {
        return WR_USAGE;
    }
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        (void)fprintf(stderr, "odin3-write: out of memory\n");
        return WR_FAIL;
    }
    double start = now_seconds();
    odin3_status st = odin3_blif_read(design, args.in);
    report("read", st == ODIN3_OK ? "ok" : "FAIL", start);
    int rc = st == ODIN3_OK ? write_all(design, &args) : WR_FAIL;
    odin3_design_destroy(design);
    return rc;
}
