/* odin3-blif-rt.c — BLIF round-trip driver: read, check FULL, write. */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/log.h"

#include <stdio.h>

enum { RT_OK = 0, RT_FAIL = 1, RT_USAGE = 2, RT_ARGC = 3 };

/* Checks the design at FULL and reports the error and warning counts on stderr. */
static void report_check(odin3_design *design, const char *path) {
    const odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};

    odin3_log_reset_counts();
    (void)odin3_check_design(design, opts);
    (void)fprintf(stderr, "%s: check FULL: %zu errors, %zu warnings\n", path,
                  odin3_log_count(ODIN3_LOG_ERROR), odin3_log_count(ODIN3_LOG_WARN));
}

static int round_trip(const char *in_path, const char *out_path) {
    int rc = RT_FAIL;
    odin3_design *design = odin3_design_create();

    if (design == NULL) {
        (void)fprintf(stderr, "odin3-blif-rt: out of memory\n");
        return RT_FAIL;
    }
    if (odin3_blif_read(design, in_path) == ODIN3_OK) {
        report_check(design, in_path);
        if (odin3_blif_write(design, out_path) == ODIN3_OK) {
            rc = RT_OK;
        }
    }
    odin3_design_destroy(design);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != RT_ARGC) {
        (void)fprintf(stderr, "usage: odin3-blif-rt in.blif out.blif\n");
        return RT_USAGE;
    }
    return round_trip(argv[1], argv[2]);
}
