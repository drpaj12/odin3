/*
 * snapshot.c — `lower --lower-snapshot <dir>` (3B spec §14.4): the design as `write_rtlil` text
 * before the pass (`lower-before.il`, the word cells) and as `write_blif` after it
 * (`lower-after.blif`, the bit cells), which `lower-equiv` hands to Yosys 0.55. A snapshot that
 * cannot be written is a WARNING and never changes the pass's result.
 */
#include "backends/blif/writer.h"
#include "backends/rtlil/writer.h"
#include "passes/builtin.h"
#include "passes/lower/lower_int.h"
#include "util/log.h"
#include "util/str.h"

#include <stddef.h>

static const char k_before_name[] = "lower-before.il";
static const char k_after_name[] = "lower-after.blif";

static void warn_skipped(const char *what, odin3_status st) {
    odin3_log(ODIN3_LOG_WARN, "lower: snapshot of the design %s skipped: %s", what,
              odin3_status_string(st));
}

/* <dir>/<name> into path. */
static odin3_status snapshot_path(const char *dir, const char *name, odin3_strbuf *path) {
    return odin3_strbuf_appendf(path, "%s/%s", dir, name);
}

void odin3_lower_snapshot_before(const odin3_lower_opts *opts, const odin3_design *design) {
    if (opts->snapshot_dir[0] == '\0') {
        return;
    }
    odin3_strbuf path;
    odin3_strbuf text;
    odin3_strbuf_init(&path);
    odin3_strbuf_init(&text);
    odin3_status st = snapshot_path(opts->snapshot_dir, k_before_name, &path);
    st = st == ODIN3_OK ? odin3_rtlil_text(design, &text) : st;
    st = st == ODIN3_OK ? odin3_pass_write_text(path.data, &text) : st;
    if (st != ODIN3_OK) {
        warn_skipped("before", st);
    }
    odin3_strbuf_free(&path);
    odin3_strbuf_free(&text);
}

void odin3_lower_snapshot_after(const odin3_lower_opts *opts, const odin3_design *design) {
    if (opts->snapshot_dir[0] == '\0') {
        return;
    }
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = snapshot_path(opts->snapshot_dir, k_after_name, &path);
    st = st == ODIN3_OK ? odin3_blif_write(design, path.data) : st;
    if (st != ODIN3_OK) {
        warn_skipped("after", st);
    }
    odin3_strbuf_free(&path);
}
