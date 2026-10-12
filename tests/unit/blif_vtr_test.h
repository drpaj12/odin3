/*
 * blif_vtr_test.h — helpers for the write_blif --vtr tests (spec 3E §16.1): read a BLIF text into
 * a fresh design in its own directory, run the write_blif pass there, read back what it wrote.
 */
#ifndef ODIN3_TESTS_UNIT_BLIF_VTR_TEST_H
#define ODIN3_TESTS_UNIT_BLIF_VTR_TEST_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* One case: its design, its directory (under the build tree) and the log since the last write. */
typedef struct bvt {
    odin3_design *design;
    char dir[256];
    odin3_strbuf log; /* "<level>: <msg>\n" per delivered message (error, warn, info, debug) */
    uint32_t errors, warnings;
} bvt;

/*
 * Writes blif_text to t.blif in bv's directory (a fresh one under the build tree on the first
 * call), changes into it, runs `read_techlib <repo>/lib/vtr.o3lib` when vtr_lib, then
 * `read_blif t.blif`, and changes back. bv must start zeroed; the design is created on the first
 * call (a later call reads into the same design, which read_blif refuses unless it is empty).
 * The log sink appends to bv->log from the first call until bvt_free. Returns the first failure
 * (ODIN3_ERR_IO when the directory or t.blif cannot be made).
 */
odin3_status bvt_read(bvt *bv, const char *blif_text, bool vtr_lib);

/*
 * Clears bv->log and the counts, then runs `write_blif <pass_args>` in bv->dir (pass_args names the
 * path, out.blif in the tests) and reads bv->dir/out.blif into out (empty when there is none).
 * Returns the pass's status.
 */
odin3_status bvt_write(bvt *bv, const char *pass_args, odin3_strbuf *out);

/* True when bv->dir/name exists. */
bool bvt_file_exists(const bvt *bv, const char *name);

/* Removes bv's directory and every file in it, destroys the design, frees the log, restores the
 * default log sink and zeroes bv. */
void bvt_free(bvt *bv);

#endif
