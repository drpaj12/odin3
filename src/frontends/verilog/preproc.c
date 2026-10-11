/* preproc.c — the preprocessor: lifetime, file cache, frames, output and segments, the scanner. */
#include "frontends/verilog/preproc.h"

#include "ast/srcman.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DECIMAL = 10, DEFINE_WORD = 7 /* strlen("`define") */ };

static const char k_cmdline[] = "<command line>";

/* --- limits and errors --------------------------------------------------------------------- */

odin3_pp_limits odin3_pp_default_limits(void) {
    return (odin3_pp_limits){
        .include_depth = ODIN3_PP_MAX_INCLUDE_DEPTH,
        .expansion_chain = ODIN3_PP_MAX_EXPANSION_CHAIN,
        .expansion_depth = ODIN3_PP_MAX_EXPANSION_DEPTH,
        .live_runs = ODIN3_PP_MAX_LIVE_RUNS,
        .cond_depth = ODIN3_PP_MAX_COND_DEPTH,
        .macro_args = ODIN3_PP_MAX_MACRO_ARGS,
        .macro_text = ODIN3_PP_MAX_MACRO_TEXT,
        .macros = ODIN3_PP_MAX_MACROS,
        .definitions = ODIN3_PP_MAX_DEFINITIONS,
        .macro_bytes = ODIN3_PP_MAX_MACRO_BYTES,
        .stream_bytes = ODIN3_PP_MAX_STREAM_BYTES,
        .buffers_per_stream = ODIN3_PP_MAX_BUFFERS_PER_STREAM,
        .includes_per_stream = ODIN3_PP_MAX_INCLUDES_PER_STREAM,
        .loc_bytes_per_stream = ODIN3_PP_MAX_LOC_BYTES_PER_STREAM,
        .file_cache_bytes = ODIN3_PP_MAX_FILE_CACHE_BYTES,
        .srcman_bytes_per_stream = ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM,
    };
}

void odin3_pp_test_set_limits(odin3_pp *pp, odin3_pp_limits limits) {
    pp->limits = limits;
}

odin3_status odin3_pp_error(odin3_pp *pp, odin3_loc loc, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (loc.v != 0) {
        odin3_diag(pp->design, ODIN3_LOG_ERROR, loc, "%s", msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg);
    }
    return ODIN3_ERR_PARSE;
}

odin3_status odin3_pp_cap_error(odin3_pp *pp, odin3_loc loc, odin3_pp_cap cap) {
    return odin3_pp_error(pp, loc, "syntax: %s exceeds %s (%" PRIu32 ")", cap.what, cap.name,
                          cap.value);
}

/* --- character classes --------------------------------------------------------------------- */

bool odin3_pp_is_blank(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

bool odin3_pp_is_ident_start(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool odin3_pp_is_ident_char(int ch) {
    return odin3_pp_is_ident_start(ch) || (ch >= '0' && ch <= '9') || ch == '$';
}

/* --- frames and cursors -------------------------------------------------------------------- */

odin3_pp_frame *odin3_pp_top(odin3_pp *pp) {
    return pp->frames.len == 0 ? NULL : odin3_vec_at(&pp->frames, pp->frames.len - 1);
}

static const odin3_pp_frame *top_frame(const odin3_pp *pp) {
    return pp->frames.len == 0 ? NULL : odin3_vec_cat(&pp->frames, pp->frames.len - 1);
}

static const odin3_pp_textrun *frame_run(const odin3_pp *pp, const odin3_pp_frame *frame,
                                         uint32_t index) {
    return odin3_vec_cat(&pp->runs, frame->first_run + index);
}

/* Moves the cursor off the end of its run (and past empty runs). */
static void normalize(const odin3_pp *pp, odin3_pp_frame *frame) {
    while (frame->cur.run < frame->nruns &&
           frame->cur.offset >= frame_run(pp, frame, frame->cur.run)->len) {
        frame->cur.offset -= frame_run(pp, frame, frame->cur.run)->len;
        frame->cur.run++;
    }
    if (frame->cur.run >= frame->nruns) {
        frame->cur = (odin3_pp_cursor){frame->nruns, 0};
    }
}

const odin3_pp_textrun *odin3_pp_cur_run(const odin3_pp *pp) {
    const odin3_pp_frame *frame = top_frame(pp);
    if (frame == NULL || frame->cur.run >= frame->nruns) {
        return NULL;
    }
    return frame_run(pp, frame, frame->cur.run);
}

int odin3_pp_peek(const odin3_pp *pp, uint32_t ahead) {
    const odin3_pp_frame *frame = top_frame(pp);
    if (frame == NULL) {
        return -1;
    }
    uint32_t run = frame->cur.run;
    uint64_t offset = (uint64_t)frame->cur.offset + ahead;
    while (run < frame->nruns && offset >= frame_run(pp, frame, run)->len) {
        offset -= frame_run(pp, frame, run)->len;
        run++;
        ((odin3_pp *)pp)->run_hops++;
    }
    return run < frame->nruns ? frame_run(pp, frame, run)->bytes[offset] : -1;
}

/* --- the run iterator: reads the top frame from its cursor, O(1) amortised per byte --------- */

typedef struct run_iter {
    odin3_pp *pp;
    const odin3_pp_frame *frame;
    uint32_t run;
    uint32_t offset;
    uint32_t taken; /* bytes passed since the cursor */
} run_iter;

/* Moves it off the end of its run (and past empty runs). */
static void iter_settle(run_iter *it) {
    while (it->run < it->frame->nruns && it->offset >= frame_run(it->pp, it->frame, it->run)->len) {
        it->offset -= frame_run(it->pp, it->frame, it->run)->len;
        it->run++;
        it->pp->run_hops++;
    }
}

static run_iter iter_start(const odin3_pp *pp, uint32_t ahead) {
    const odin3_pp_frame *frame = top_frame(pp);
    run_iter it = {(odin3_pp *)pp, frame, frame->cur.run, frame->cur.offset, 0};
    for (uint32_t i = 0; i < ahead && it.run < frame->nruns; i++) {
        it.offset++;
        it.taken++;
        iter_settle(&it);
    }
    iter_settle(&it);
    return it;
}

/* The byte at it, -1 at the frame's end. */
static int iter_peek(const run_iter *it) {
    return it->run < it->frame->nruns ? frame_run(it->pp, it->frame, it->run)->bytes[it->offset]
                                      : -1;
}

static void iter_next(run_iter *it) {
    if (it->run < it->frame->nruns) {
        it->offset++;
        it->taken++;
        iter_settle(it);
    }
}

/* The byte after it, -1 at the frame's end. */
static int iter_peek_next(const run_iter *it) {
    run_iter next = *it;
    iter_next(&next);
    return iter_peek(&next);
}

odin3_loc odin3_pp_loc_here(const odin3_pp *pp) {
    const odin3_pp_frame *frame = top_frame(pp);
    if (frame == NULL || frame->nruns == 0) {
        return (odin3_loc){0};
    }
    if (frame->cur.run < frame->nruns) {
        const odin3_pp_textrun *run = frame_run(pp, frame, frame->cur.run);
        return odin3_pp_loc_add(run->loc, frame->cur.offset);
    }
    const odin3_pp_textrun *last = frame_run(pp, frame, frame->nruns - 1);
    return odin3_pp_loc_add(last->loc, last->len);
}

void odin3_pp_advance(odin3_pp *pp, uint32_t n) {
    odin3_pp_frame *frame = odin3_pp_top(pp);
    if (frame == NULL || frame->cur.run >= frame->nruns) {
        return;
    }
    uint64_t offset = (uint64_t)frame->cur.offset + n;
    frame->cur.offset = offset > UINT32_MAX ? UINT32_MAX : (uint32_t)offset;
    normalize(pp, frame);
}

void odin3_pp_skip_spaces(odin3_pp *pp) {
    for (int ch = odin3_pp_peek(pp, 0); ch == ' ' || ch == '\t'; ch = odin3_pp_peek(pp, 0)) {
        odin3_pp_advance(pp, 1);
    }
}

odin3_bytes odin3_pp_read_ident(odin3_pp *pp) {
    const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
    if (run == NULL) {
        return (odin3_bytes){NULL, 0};
    }
    uint32_t start = odin3_pp_top(pp)->cur.offset;
    uint32_t end = start;
    if (odin3_pp_is_ident_start(run->bytes[start])) {
        while (end < run->len && odin3_pp_is_ident_char(run->bytes[end])) {
            end++;
        }
    }
    odin3_bytes ident = {run->bytes + start, end - start};
    odin3_pp_advance(pp, end - start);
    return ident;
}

odin3_status odin3_pp_push_frame(odin3_pp *pp, const odin3_pp_frame *frame,
                                 const odin3_pp_textrun *runs, odin3_loc at) {
    if ((uint64_t)pp->runs.len + frame->nruns > pp->limits.live_runs) {
        odin3_pp_cap cap = {"live runs", "ODIN3_PP_MAX_LIVE_RUNS", pp->limits.live_runs};
        return odin3_pp_cap_error(pp, at, cap);
    }
    bool expansion = frame->kind == ODIN3_PP_FRAME_EXPANSION;
    if (expansion && pp->nexpansions >= pp->limits.expansion_depth) {
        odin3_pp_cap cap = {"expansion depth", "ODIN3_PP_MAX_EXPANSION_DEPTH",
                            pp->limits.expansion_depth};
        return odin3_pp_cap_error(pp, at, cap);
    }
    if (odin3_vec_reserve(&pp->runs, pp->runs.len + frame->nruns) != ODIN3_OK ||
        odin3_vec_reserve(&pp->frames, pp->frames.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t first = (uint32_t)pp->runs.len;
    for (uint32_t i = 0; i < frame->nruns; i++) {
        *(odin3_pp_textrun *)odin3_vec_push(&pp->runs) = runs[i];
    }
    odin3_pp_frame *top = odin3_vec_push(&pp->frames);
    *top = *frame;
    top->first_run = first;
    top->cur = (odin3_pp_cursor){0, 0};
    normalize(pp, top);
    pp->nexpansions += expansion ? 1 : 0;
    return ODIN3_OK;
}

void odin3_pp_pop_frame(odin3_pp *pp) {
    const odin3_pp_frame *frame = top_frame(pp);
    if (frame == NULL) {
        return;
    }
    while (pp->runs.len > frame->first_run) {
        odin3_vec_pop(&pp->runs);
    }
    if (frame->kind == ODIN3_PP_FRAME_FILE) {
        while (pp->conds.len > frame->cond_base) {
            odin3_vec_pop(&pp->conds);
        }
    } else {
        pp->nexpansions--;
    }
    odin3_vec_pop(&pp->frames);
}

/* --- output and segments ------------------------------------------------------------------- */

/* Bytes of source-manager records per line start and per segment (spec §9). */
enum { LINE_RECORD_BYTES = 4, SEGMENT_RECORD_BYTES = 8 };

/* Where an output cap error is reported for text at loc: loc itself, else the cursor, else (in an
 * inserted run, such as a comment's space) the innermost macro use, so the error stays located. */
static odin3_loc output_loc(const odin3_pp *pp, odin3_loc loc) {
    if (loc.v != 0) {
        return loc;
    }
    odin3_loc here = odin3_pp_loc_here(pp);
    for (size_t i = pp->frames.len; here.v == 0 && i > 0; i--) {
        here = ((const odin3_pp_frame *)odin3_vec_cat(&pp->frames, i - 1))->use_loc;
    }
    return here;
}

/* Charges bytes of source-manager records to the stream, before they are added
 * (ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM, located at at). */
static odin3_status srcman_charge(odin3_pp *pp, uint64_t bytes, odin3_loc at) {
    if (pp->srcman_bytes + bytes > pp->limits.srcman_bytes_per_stream) {
        odin3_pp_cap cap = {"source-manager bytes", "ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM",
                            pp->limits.srcman_bytes_per_stream};
        return odin3_pp_cap_error(pp, at, cap);
    }
    pp->srcman_bytes += bytes;
    return ODIN3_OK;
}

/* Room for len more output bytes and the NUL, growing by doubling but never past the stream cap
 * (ODIN3_PP_MAX_STREAM_BYTES + 1), so the text buffer stays bounded by the cap. */
static odin3_status reserve_out(odin3_pp *pp, uint64_t len) {
    uint64_t need = (uint64_t)pp->out.len + len + 1;
    if (need <= pp->out.cap) {
        return ODIN3_OK;
    }
    uint64_t grow = (uint64_t)pp->out.cap * 2 > need ? (uint64_t)pp->out.cap * 2 : need;
    uint64_t most = (uint64_t)pp->limits.stream_bytes + 1;
    grow = grow > most && need <= most ? most : grow;
    return odin3_strbuf_reserve(&pp->out, (size_t)grow);
}

static odin3_status add_segment(odin3_pp *pp, uint32_t out_offset, odin3_loc loc) {
    odin3_segment seg = {pp->stream.v, out_offset, loc.v, {0, 0}};
    odin3_status st = odin3_srcman_add_segment(pp->sm, &seg);
    pp->nsegments += st == ODIN3_OK ? 1 : 0;
    return st;
}

/* Appends one piece that continues or starts one segment: bytes spelled at loc onward, or one
 * inserted byte (loc 0). */
static odin3_status append_piece(odin3_pp *pp, odin3_pp_text piece) {
    uint32_t out_offset = (uint32_t)pp->out.len;
    bool fresh = out_offset == 0 || piece.loc.v == 0 || pp->last_loc.v == 0 ||
                 piece.loc.v != pp->last_loc.v + 1;
    odin3_status st = ODIN3_OK;
    if (fresh) {
        st = srcman_charge(pp, SEGMENT_RECORD_BYTES, output_loc(pp, piece.loc));
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(&pp->out, (odin3_bytes){piece.bytes, piece.len});
    }
    if (st == ODIN3_OK && fresh) {
        st = add_segment(pp, out_offset, piece.loc);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    pp->last_loc = odin3_pp_loc_add(piece.loc, piece.len - 1);
    if (piece.loc.v != 0) {
        odin3_pp_check_note(pp, piece.loc);
    }
    return ODIN3_OK;
}

/* Appends text with the segment rule: a new segment when a byte's loc is 0, the previous byte's
 * was 0, or it is not the previous + 1; a loc-0 byte is always a segment of its own. */
static odin3_status append(odin3_pp *pp, odin3_pp_text text) {
    if ((uint64_t)pp->out.len + text.len > pp->limits.stream_bytes) {
        uint32_t room = pp->limits.stream_bytes - (uint32_t)pp->out.len;
        odin3_loc at =
            text.loc.v != 0 ? odin3_pp_loc_add(text.loc, room) : output_loc(pp, text.loc);
        odin3_pp_cap cap = {"stream bytes", "ODIN3_PP_MAX_STREAM_BYTES", pp->limits.stream_bytes};
        return odin3_pp_cap_error(pp, at, cap);
    }
    odin3_status st = reserve_out(pp, text.len);
    if (st != ODIN3_OK) {
        return st;
    }
    if (text.loc.v != 0) {
        return append_piece(pp, text);
    }
    for (uint32_t i = 0; i < text.len && st == ODIN3_OK; i++) {
        st = append_piece(pp, (odin3_pp_text){text.bytes + i, 1, {0}});
    }
    return st;
}

/* PP-3: one inserted space when the bytes on both sides of a consumed piece are not blanks. */
static odin3_status protective_space(odin3_pp *pp, uint8_t next) {
    pp->gap = false;
    if (pp->out.len == 0 || odin3_pp_is_blank((unsigned char)pp->out.data[pp->out.len - 1]) ||
        odin3_pp_is_blank(next)) {
        return ODIN3_OK;
    }
    static const uint8_t k_space = ' ';
    return append(pp, (odin3_pp_text){&k_space, 1, {0}});
}

odin3_status odin3_pp_emit(odin3_pp *pp, odin3_pp_text text) {
    if (text.len == 0) {
        return ODIN3_OK;
    }
    if (pp->gap) {
        odin3_status st = protective_space(pp, text.bytes[0]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return append(pp, text);
}

static bool suppressed(const odin3_pp *pp) {
    return pp->skipping || pp->xoff_loc.v != 0;
}

odin3_status odin3_pp_emit_next(odin3_pp *pp, uint32_t n) {
    while (n > 0) {
        const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
        if (run == NULL) {
            return ODIN3_OK;
        }
        uint32_t offset = odin3_pp_top(pp)->cur.offset;
        uint32_t take = run->len - offset < n ? run->len - offset : n;
        if (!suppressed(pp)) {
            odin3_pp_text text = {run->bytes + offset, take, odin3_pp_loc_add(run->loc, offset)};
            odin3_status st = odin3_pp_emit(pp, text);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        odin3_pp_advance(pp, take);
        n -= take;
    }
    return ODIN3_OK;
}

/* --- buffers ------------------------------------------------------------------------------- */

static const odin3_pp_bufrec *bufrec_of(const odin3_pp *pp, odin3_srcbuf_id buf) {
    uint32_t first = pp->first_buffer.v;
    if (pp->bufs.len == 0 || buf.v < first || buf.v - first >= pp->bufs.len) {
        return NULL;
    }
    return odin3_vec_cat(&pp->bufs, buf.v - first);
}

const odin3_pp_bufrec *odin3_pp_bufrec_of(const odin3_pp *pp, odin3_loc loc) {
    return bufrec_of(pp, odin3_srcman_buffer_of(pp->sm, loc));
}

uint32_t odin3_pp_chain_of(const odin3_pp *pp, odin3_loc loc) {
    const odin3_pp_bufrec *rec = odin3_pp_bufrec_of(pp, loc);
    return rec == NULL ? 0 : rec->chain;
}

static odin3_status buffer_caps(odin3_pp *pp, const odin3_pp_bufspec *spec, odin3_loc at) {
    bool file = spec->kind == ODIN3_SRCBUF_FILE;
    uint32_t len = file ? spec->file.len : spec->expansion.len;
    if (pp->nbuffers >= pp->limits.buffers_per_stream) {
        odin3_pp_cap cap = {"buffers", "ODIN3_PP_MAX_BUFFERS_PER_STREAM",
                            pp->limits.buffers_per_stream};
        return odin3_pp_cap_error(pp, at, cap);
    }
    if (file && pp->nincludes >= pp->limits.includes_per_stream) {
        odin3_pp_cap cap = {"files", "ODIN3_PP_MAX_INCLUDES_PER_STREAM",
                            pp->limits.includes_per_stream};
        return odin3_pp_cap_error(pp, at, cap);
    }
    if (pp->loc_bytes + len + 1 > pp->limits.loc_bytes_per_stream) {
        odin3_pp_cap cap = {"location bytes", "ODIN3_PP_MAX_LOC_BYTES_PER_STREAM",
                            pp->limits.loc_bytes_per_stream};
        return odin3_pp_cap_error(pp, at, cap);
    }
    return ODIN3_OK;
}

odin3_status odin3_pp_add_buffer(odin3_pp *pp, const odin3_pp_bufspec *spec, odin3_loc at,
                                 odin3_srcbuf_id *out) {
    odin3_status st = buffer_caps(pp, spec, at);
    if (st != ODIN3_OK) {
        return st;
    }
    if (odin3_vec_reserve(&pp->bufs, pp->bufs.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool file = spec->kind == ODIN3_SRCBUF_FILE;
    uint32_t chain = 0;
    if (!file) {
        /* a default's MACRO_ARG is macro text: its ancestry is the expansion (PP-7) */
        bool arg = spec->kind == ODIN3_SRCBUF_MACRO_ARG && !spec->from_default;
        odin3_loc from = {arg ? spec->expansion.def : spec->expansion.parent};
        chain = 1 + odin3_pp_chain_of(pp, from);
    }
    st = file ? odin3_srcman_add_file(pp->sm, &spec->file, out)
              : odin3_srcman_add_expansion(pp->sm, &spec->expansion, out);
    if (st != ODIN3_OK) {
        return st;
    }
    if (pp->bufs.len == 0) {
        pp->first_buffer = *out;
    }
    *(odin3_pp_bufrec *)odin3_vec_push(&pp->bufs) = (odin3_pp_bufrec){chain, 0, spec->from_default};
    pp->nbuffers++;
    pp->nincludes += file ? 1 : 0;
    pp->loc_bytes += (uint64_t)(file ? spec->file.len : spec->expansion.len) + 1;
    return ODIN3_OK;
}

/* --- file cache ---------------------------------------------------------------------------- */

bool odin3_pp_file_bytes(const odin3_pp *pp, odin3_srcbuf_id buf, odin3_bytes *out) {
    uint64_t index = 0;
    if (!odin3_u64map_get(pp->cache_by_buffer, buf.v, &index)) {
        return false;
    }
    const odin3_pp_cached *entry = odin3_vec_cat(&pp->cache, (size_t)index);
    *out = (odin3_bytes){entry->bytes, entry->len};
    return true;
}

static const char *display_name(const odin3_pp *pp, const odin3_pp_load *load) {
    bool resolved = odin3_design_diag_paths(pp->design) == ODIN3_SRCFMT_RESOLVED;
    return odin3_strtab_get(pp->strtab, resolved ? load->resolved : load->given);
}

/* A root or included file that cannot be read: missing_file (PARSE), over the size cap
 * (PARSE) or unreadable (IO), located at load->at. */
static odin3_status cannot_read(odin3_pp *pp, const odin3_pp_load *load,
                                const odin3_file_bytes *bytes) {
    const char *name = display_name(pp, load);
    if (bytes->too_big) {
        return odin3_pp_error(pp, load->at,
                              "syntax: file '%s' is %zu bytes, over the limit of %" PRIu32, name,
                              bytes->len, ODIN3_SRC_MAX_FILE_BYTES);
    }
    /* PRJ-6 classes a path of the wrong kind (a directory) as missing too */
    if (bytes->err == ENOENT || bytes->err == ENOTDIR || bytes->err == EISDIR) {
        return odin3_pp_error(pp, load->at, "missing_file: '%s'", name);
    }
    (void)odin3_pp_error(pp, load->at, "cannot read '%s': %s", name, strerror(bytes->err));
    return ODIN3_ERR_IO;
}

/* Enters bytes (taken over) as a new cache entry; reserve-before-mutate. */
static odin3_status cache_add(odin3_pp *pp, uint32_t resolved, odin3_file_bytes *bytes,
                              uint32_t *index) {
    if (odin3_vec_reserve(&pp->cache, pp->cache.len + 1) != ODIN3_OK ||
        odin3_vec_reserve(&pp->files_read, pp->files_read.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *index = (uint32_t)pp->cache.len;
    if (odin3_u64map_put(pp->cache_by_path, (odin3_kv){resolved, *index}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_pp_cached *entry = odin3_vec_push(&pp->cache);
    uint32_t newlines = 0;
    for (size_t i = 0; i < bytes->len; i++) {
        newlines += bytes->data[i] == '\n' ? 1 : 0;
    }
    *entry = (odin3_pp_cached){resolved, (uint32_t)bytes->len, bytes->data, newlines};
    bytes->data = NULL;
    *(uint32_t *)odin3_vec_push(&pp->files_read) = resolved;
    pp->cache_bytes += entry->len;
    return ODIN3_OK;
}

odin3_status odin3_pp_cache_load(odin3_pp *pp, const odin3_pp_load *load, uint32_t *index) {
    uint64_t found = 0;
    if (odin3_u64map_get(pp->cache_by_path, load->resolved, &found)) {
        *index = (uint32_t)found;
        return ODIN3_OK;
    }
    uint64_t limit = pp->limits.file_cache_bytes;
    uint64_t room = limit > pp->cache_bytes ? limit - pp->cache_bytes : 0;
    bool cache_bound = room < ODIN3_SRC_MAX_FILE_BYTES;
    odin3_file_bytes bytes;
    const char *path = odin3_strtab_get(pp->strtab, load->resolved);
    odin3_status st =
        odin3_file_read_all(path, cache_bound ? room : ODIN3_SRC_MAX_FILE_BYTES, &bytes);
    if (st == ODIN3_ERR_PARSE && cache_bound && bytes.len <= ODIN3_SRC_MAX_FILE_BYTES) {
        odin3_pp_cap cap = {"file cache bytes", "ODIN3_PP_MAX_FILE_CACHE_BYTES",
                            pp->limits.file_cache_bytes};
        st = odin3_pp_cap_error(pp, load->at, cap);
    } else if (st == ODIN3_ERR_IO || st == ODIN3_ERR_PARSE) {
        st = cannot_read(pp, load, &bytes);
    } else if (st == ODIN3_OK) {
        st = cache_add(pp, load->resolved, &bytes, index);
    }
    odin3_file_bytes_free(&bytes);
    return st;
}

static odin3_status add_lines(odin3_pp *pp, uint32_t buf, const odin3_pp_cached *entry) {
    for (uint32_t i = 0; i < entry->len; i++) {
        if (entry->bytes[i] == '\n') {
            odin3_status st = odin3_srcman_add_line(pp->sm, (odin3_srcbuf_id){buf}, i + 1);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_pp_open_file(odin3_pp *pp, const odin3_pp_open *open, odin3_srcbuf_id *out) {
    const odin3_pp_cached *entry = odin3_vec_cat(&pp->cache, open->cache);
    odin3_pp_bufspec spec = {
        ODIN3_SRCBUF_FILE,
        {open->given, entry->resolved, open->library, open->parent.v, entry->len, {0, 0}},
        {0, 0, 0, 0, 0, 0, {0, 0}},
        false};
    odin3_status st = srcman_charge(pp, (uint64_t)entry->newlines * LINE_RECORD_BYTES, open->at);
    if (st == ODIN3_OK) {
        st = odin3_pp_add_buffer(pp, &spec, open->at, out);
    }
    if (st == ODIN3_OK) {
        st = add_lines(pp, out->v, entry);
    }
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(pp->cache_by_buffer, (odin3_kv){out->v, open->cache});
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pp_textrun run = {entry->bytes, entry->len, odin3_srcman_loc(pp->sm, *out, 0)};
    odin3_pp_frame frame = {.kind = ODIN3_PP_FRAME_FILE,
                            .nruns = 1,
                            .buffer = *out,
                            .resolved = entry->resolved,
                            .library = open->library,
                            .cond_base = (uint32_t)pp->conds.len};
    return odin3_pp_push_frame(pp, &frame, &run, open->at);
}

/* --- the scanner --------------------------------------------------------------------------- */

static const odin3_pp_dir k_dirs[] = {
    {"define", ODIN3_PP_DIR_DEFINE, 0},
    {"undef", ODIN3_PP_DIR_UNDEF, 0},
    {"ifdef", ODIN3_PP_DIR_COND, ODIN3_PP_COND_IFDEF},
    {"ifndef", ODIN3_PP_DIR_COND, ODIN3_PP_COND_IFNDEF},
    {"elsif", ODIN3_PP_DIR_COND, ODIN3_PP_COND_ELSIF},
    {"else", ODIN3_PP_DIR_COND, ODIN3_PP_COND_ELSE},
    {"endif", ODIN3_PP_DIR_COND, ODIN3_PP_COND_ENDIF},
    {"include", ODIN3_PP_DIR_INCLUDE, 0},
    {"line", ODIN3_PP_DIR_LINE, 0},
    {"default_nettype", ODIN3_PP_DIR_PASS, 0},
    {"timescale", ODIN3_PP_DIR_PASS, 0},
    {"celldefine", ODIN3_PP_DIR_PASS, 0},
    {"endcelldefine", ODIN3_PP_DIR_PASS, 0},
    {"resetall", ODIN3_PP_DIR_PASS, 0},
    {"begin_keywords", ODIN3_PP_DIR_PASS, 0},
    {"end_keywords", ODIN3_PP_DIR_PASS, 0},
    {"unconnected_drive", ODIN3_PP_DIR_PASS, 0},
    {"nounconnected_drive", ODIN3_PP_DIR_PASS, 0},
    {"pragma", ODIN3_PP_DIR_PASS, 0},
    {"__FILE__", ODIN3_PP_DIR_SV, 0},
    {"__LINE__", ODIN3_PP_DIR_SV, 0},
};

const odin3_pp_dir *odin3_pp_dir_lookup(odin3_bytes name) {
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; i++) {
        if (strlen(k_dirs[i].name) == name.len && memcmp(k_dirs[i].name, name.ptr, name.len) == 0) {
            return &k_dirs[i];
        }
    }
    return NULL;
}

odin3_bytes odin3_pp_ident_ahead(const odin3_pp *pp, uint32_t ahead) {
    const odin3_pp_frame *frame = top_frame(pp);
    uint32_t run = frame->cur.run;
    uint64_t offset = (uint64_t)frame->cur.offset + ahead;
    while (run < frame->nruns && offset >= frame_run(pp, frame, run)->len) {
        offset -= frame_run(pp, frame, run)->len;
        run++;
    }
    if (run >= frame->nruns) {
        return (odin3_bytes){NULL, 0};
    }
    const odin3_pp_textrun *text = frame_run(pp, frame, run);
    uint32_t end = (uint32_t)offset;
    if (odin3_pp_is_ident_start(text->bytes[end])) {
        while (end < text->len && odin3_pp_is_ident_char(text->bytes[end])) {
            end++;
        }
    }
    return (odin3_bytes){text->bytes + offset, end - (uint32_t)offset};
}

odin3_status odin3_pp_sv_operator(odin3_pp *pp, odin3_loc at, const char *op) {
    return odin3_pp_error(pp, at,
                          "syntax: '%s' is a SystemVerilog macro operator; not Verilog-2005 "
                          "(Phase 5)",
                          op);
}

static bool is_digit(int ch) {
    return ch >= '0' && ch <= '9';
}

static const char k_bad_line[] = "syntax: malformed `line directive (expected `line <number> "
                                 "\"<file>\" <0, 1 or 2>)";

/* The length of the "..." string at the cursor (escapes skipped, no newline), 0 when there is
 * no closed string there. */
static uint32_t quoted_length(const odin3_pp *pp) {
    if (odin3_pp_peek(pp, 0) != '"') {
        return 0;
    }
    run_iter it = iter_start(pp, 1);
    for (int ch = iter_peek(&it); ch >= 0 && ch != '\n'; ch = iter_peek(&it)) {
        int next = iter_peek_next(&it);
        iter_next(&it);
        if (ch == '\\' && next >= 0 && next != '\n') {
            iter_next(&it);
        } else if (ch == '"') {
            return it.taken;
        }
    }
    return 0;
}

/* `line n "file" level (spec §7.3): syntax-checked and dropped. */
static odin3_status line_directive(odin3_pp *pp) {
    odin3_pp_skip_spaces(pp);
    odin3_loc at = odin3_pp_loc_here(pp);
    uint64_t number = 0;
    uint32_t k = 0;
    for (; is_digit(odin3_pp_peek(pp, k)) && number <= UINT32_MAX; k++) {
        number = number * DECIMAL + (uint64_t)(odin3_pp_peek(pp, k) - '0');
    }
    if (k == 0 || number > UINT32_MAX || odin3_pp_is_ident_char(odin3_pp_peek(pp, k))) {
        return odin3_pp_error(pp, at, "%s", k_bad_line);
    }
    odin3_pp_advance(pp, k);
    odin3_pp_skip_spaces(pp);
    at = odin3_pp_loc_here(pp);
    k = quoted_length(pp);
    if (k == 0) {
        return odin3_pp_error(pp, at, "%s", k_bad_line);
    }
    odin3_pp_advance(pp, k);
    odin3_pp_skip_spaces(pp);
    at = odin3_pp_loc_here(pp);
    int level = odin3_pp_peek(pp, 0);
    if (level < '0' || level > '2' || odin3_pp_is_ident_char(odin3_pp_peek(pp, 1))) {
        return odin3_pp_error(pp, at, "%s", k_bad_line);
    }
    odin3_pp_advance(pp, 1);
    pp->gap = true;
    return ODIN3_OK;
}

static odin3_status consumed_directive(odin3_pp *pp, const odin3_pp_dir *dir, odin3_loc at) {
    if (odin3_pp_top(pp)->kind != ODIN3_PP_FRAME_FILE) {
        return odin3_pp_error(pp, at, "syntax: directive inside macro text");
    }
    switch (dir->kind) {
    case ODIN3_PP_DIR_DEFINE:
        return odin3_pp_define_directive(pp, at);
    case ODIN3_PP_DIR_UNDEF:
        return odin3_pp_undef_directive(pp, at);
    case ODIN3_PP_DIR_COND:
        return odin3_pp_cond_directive(pp, dir->cond, at);
    default:
        return odin3_pp_include_directive(pp, at);
    }
}

/* `NAME outside skipped regions: a directive, a pass-through directive or a macro use. */
static odin3_status backtick_word(odin3_pp *pp, odin3_loc at) {
    odin3_bytes name = odin3_pp_ident_ahead(pp, 1);
    const odin3_pp_dir *dir = odin3_pp_dir_lookup(name);
    odin3_pp_dir_kind kind = dir == NULL ? ODIN3_PP_DIR_NONE : dir->kind;
    if (kind == ODIN3_PP_DIR_PASS) {
        return odin3_pp_emit_next(pp, (uint32_t)name.len + 1);
    }
    if (kind == ODIN3_PP_DIR_SV) {
        return odin3_pp_sv_operator(pp, at,
                                    name.len == strlen("__FILE__") &&
                                            memcmp(name.ptr, "__FILE__", name.len) == 0
                                        ? "`__FILE__"
                                        : "`__LINE__");
    }
    odin3_pp_advance(pp, (uint32_t)name.len + 1);
    if (kind == ODIN3_PP_DIR_LINE) {
        return line_directive(pp);
    }
    if (kind != ODIN3_PP_DIR_NONE) {
        return consumed_directive(pp, dir, at);
    }
    odin3_pp_use use = {name, odin3_pp_macro_find(pp, name), at};
    return odin3_pp_expand_use(pp, &use);
}

/* A backtick in a skipped region: only conditionals are interpreted. */
static odin3_status skipped_backtick(odin3_pp *pp, odin3_loc at) {
    odin3_bytes name = odin3_pp_ident_ahead(pp, 1);
    const odin3_pp_dir *dir = odin3_pp_dir_lookup(name);
    odin3_pp_advance(pp, (uint32_t)name.len + 1);
    if (dir != NULL && dir->kind == ODIN3_PP_DIR_COND) {
        return odin3_pp_cond_directive(pp, dir->cond, at);
    }
    return ODIN3_OK;
}

static odin3_status scan_backtick(odin3_pp *pp) {
    odin3_loc at = odin3_pp_loc_here(pp);
    if (pp->xoff_loc.v != 0) {
        odin3_pp_advance(pp, 1);
        return ODIN3_OK;
    }
    if (pp->skipping) {
        return skipped_backtick(pp, at);
    }
    int next = odin3_pp_peek(pp, 1);
    if (next == '`') {
        return odin3_pp_sv_operator(pp, at, "``");
    }
    if (next == '"') {
        return odin3_pp_sv_operator(pp, at, "`\"");
    }
    if (next == '\\' && odin3_pp_peek(pp, 2) == '`' && odin3_pp_peek(pp, 3) == '"') {
        return odin3_pp_sv_operator(pp, at, "`\\`\"");
    }
    if (!odin3_pp_is_ident_start(next)) {
        return odin3_pp_error(pp, at, "syntax: stray backtick");
    }
    return backtick_word(pp, at);
}

uint32_t odin3_pp_string_length(const odin3_pp *pp) {
    run_iter it = iter_start(pp, 1);
    for (int ch = iter_peek(&it); ch >= 0 && ch != '\n'; ch = iter_peek(&it)) {
        bool escape = ch == '\\' && iter_peek_next(&it) >= 0;
        iter_next(&it);
        if (escape) {
            iter_next(&it);
        } else if (ch == '"') {
            break;
        }
    }
    return it.taken < 1 ? 1 : it.taken;
}

uint32_t odin3_pp_escaped_length(const odin3_pp *pp) {
    run_iter it = iter_start(pp, 1);
    while (iter_peek(&it) >= 0 && !odin3_pp_is_blank(iter_peek(&it))) {
        iter_next(&it);
    }
    return it.taken < 1 ? 1 : it.taken;
}

uint32_t odin3_pp_comment_length(const odin3_pp *pp) {
    run_iter it = iter_start(pp, 2);
    if (odin3_pp_peek(pp, 1) == '/') {
        while (iter_peek(&it) >= 0 && iter_peek(&it) != '\n') {
            iter_next(&it);
        }
        return it.taken < 2 ? 2 : it.taken;
    }
    for (int ch = iter_peek(&it); ch >= 0; ch = iter_peek(&it)) {
        iter_next(&it);
        if (ch == '*' && iter_peek(&it) == '/') {
            return it.taken + 1;
        }
    }
    return it.taken < 2 ? 2 : it.taken;
}

/* A comment: copied through unless the translate_off hook drops it (PP-10). */
static odin3_status scan_comment(odin3_pp *pp) {
    uint32_t len = odin3_pp_comment_length(pp);
    if (pp->skipping) {
        odin3_pp_advance(pp, len);
        return ODIN3_OK;
    }
    const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
    uint32_t offset = odin3_pp_top(pp)->cur.offset;
    odin3_pp_comment comment = {NULL, len, odin3_pp_loc_here(pp)};
    if (run->len - offset >= len) {
        comment.bytes = run->bytes + offset;
    }
    bool drop = false;
    odin3_status st = odin3_pp_cond_comment(pp, &comment, &drop);
    if (st != ODIN3_OK || drop) {
        odin3_pp_advance(pp, st == ODIN3_OK ? len : 0);
        return st;
    }
    return odin3_pp_emit_next(pp, len);
}

static bool special(uint8_t ch, bool brackets) {
    bool scanner = ch == '`' || ch == '"' || ch == '/' || ch == '\\';
    return scanner || (brackets && ch != '\0' && strchr("()[]{},", ch) != NULL);
}

uint32_t odin3_pp_plain_length(const odin3_pp *pp, bool brackets) {
    const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
    uint32_t start = top_frame(pp)->cur.offset;
    uint32_t end = start + 1;
    while (end < run->len && !special(run->bytes[end], brackets)) {
        end++;
    }
    return end - start;
}

static odin3_status frame_end(odin3_pp *pp) {
    if (odin3_pp_top(pp)->kind == ODIN3_PP_FRAME_FILE) {
        odin3_status st = odin3_pp_cond_file_end(pp);
        if (st == ODIN3_OK && pp->frames.len > 1) {
            st = odin3_pp_include_end(pp);
        } else if (st == ODIN3_OK) {
            odin3_vec_pop(&pp->incstack); /* the stream's own file: the stack ends empty */
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_pp_pop_frame(pp);
    return ODIN3_OK;
}

static odin3_status scan_step(odin3_pp *pp) {
    int ch = odin3_pp_peek(pp, 0);
    switch (ch) {
    case -1:
        return frame_end(pp);
    case '"':
        return odin3_pp_emit_next(pp, odin3_pp_string_length(pp));
    case '\\':
        return odin3_pp_emit_next(pp, odin3_pp_escaped_length(pp));
    case '`':
        return scan_backtick(pp);
    case '/': {
        int next = odin3_pp_peek(pp, 1);
        return next == '/' || next == '*' ? scan_comment(pp) : odin3_pp_emit_next(pp, 1);
    }
    default:
        return odin3_pp_emit_next(pp, odin3_pp_plain_length(pp, false));
    }
}

/* --- runs ---------------------------------------------------------------------------------- */

static void reset_run(odin3_pp *pp) {
    odin3_strbuf_clear(&pp->out);
    odin3_vec_clear(&pp->bufs);
    odin3_vec_clear(&pp->frames);
    odin3_vec_clear(&pp->runs);
    odin3_vec_clear(&pp->conds);
    odin3_vec_clear(&pp->incstack);
    odin3_vec_clear(&pp->args);
    odin3_vec_clear(&pp->arg_starts);
    odin3_vec_clear(&pp->scratch);
    pp->stream = (odin3_srcbuf_id){0};
    pp->first_buffer = (odin3_srcbuf_id){0};
    pp->nsegments = 0;
    pp->last_loc = (odin3_loc){0};
    pp->nbuffers = 0;
    pp->nincludes = 0;
    pp->nexpansions = 0;
    pp->loc_bytes = 0;
    pp->srcman_bytes = 0;
    pp->xoff_loc = (odin3_loc){0};
    pp->skipping = false;
    pp->gap = false;
}

static odin3_status start_stream(odin3_pp *pp, const odin3_pp_source *src) {
    odin3_pp_load load = {src->given, src->resolved, src->at};
    uint32_t index = 0;
    odin3_status st = odin3_pp_cache_load(pp, &load, &index);
    if (st != ODIN3_OK) {
        return st;
    }
    /* the text is usually about the file's size: start there rather than double up to it */
    const odin3_pp_cached *entry = odin3_vec_cat(&pp->cache, index);
    uint32_t expect = entry->len < pp->limits.stream_bytes ? entry->len : pp->limits.stream_bytes;
    if (reserve_out(pp, expect) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t *path = odin3_vec_push(&pp->incstack);
    if (path == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *path = src->resolved;
    odin3_pp_open open = {index, src->given, src->library, {0}, src->at};
    odin3_srcbuf_id buf = {0};
    st = odin3_pp_open_file(pp, &open, &buf);
    pp->stream = buf;
    return st;
}

odin3_status odin3_pp_run(odin3_pp *pp, const odin3_pp_source *src, odin3_pp_stream *out) {
    if (pp == NULL || src == NULL || out == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (odin3_pp_stream){{NULL, 0}, {0}, 0};
    reset_run(pp);
    odin3_status st = start_stream(pp, src);
    while (st == ODIN3_OK && pp->frames.len > 0) {
        st = scan_step(pp);
    }
#ifndef NDEBUG
    if (st == ODIN3_OK) {
        st = odin3_pp_check(pp, pp->stream);
    }
#endif
    if (st != ODIN3_OK) {
        reset_run(pp);
        return st;
    }
    *out = (odin3_pp_stream){{pp->out.data, pp->out.len}, pp->stream, pp->nsegments};
    return ODIN3_OK;
}

uint32_t odin3_pp_files_read(const odin3_pp *pp, const uint32_t **paths) {
    *paths = pp->files_read.data;
    return (uint32_t)pp->files_read.len;
}

/* --- lifetime ------------------------------------------------------------------------------ */

/* Writes the <command line> text: one LF-terminated line per define. */
static odin3_status cmdline_text(const odin3_pp_config *config, odin3_strbuf *text) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < config->ndefines && st == ODIN3_OK; i++) {
        const odin3_pp_define *def = &config->defines[i];
        st = odin3_strbuf_append(text, odin3_bytes_cstr("`define "));
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(text, def->name);
        }
        if (st == ODIN3_OK && def->has_value) {
            st = odin3_strbuf_append(text, odin3_bytes_cstr(" "));
        }
        if (st == ODIN3_OK && def->has_value) {
            st = odin3_strbuf_append(text, def->value);
        }
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(text, odin3_bytes_cstr("\n"));
        }
    }
    return st;
}

/* Registers the <command line> text as cache entry 0 and its FILE buffer. */
static odin3_status cmdline_buffer(odin3_pp *pp, odin3_strbuf *text) {
    uint32_t name = 0;
    odin3_status st = odin3_design_intern(pp->design, odin3_bytes_cstr(k_cmdline), &name);
    if (st != ODIN3_OK || odin3_vec_reserve(&pp->cache, 1) != ODIN3_OK) {
        return st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
    }
    odin3_pp_cached *entry = odin3_vec_push(&pp->cache);
    *entry = (odin3_pp_cached){0, (uint32_t)text->len, (uint8_t *)text->data, 0};
    odin3_strbuf_init(text); /* the cache owns the bytes now */
    odin3_srcfile_spec spec = {name, 0, 0, 0, entry->len, {0, 0}};
    odin3_srcbuf_id buf = {0};
    st = odin3_srcman_add_file(pp->sm, &spec, &buf);
    if (st == ODIN3_OK) {
        st = add_lines(pp, buf.v, entry);
    }
    if (st == ODIN3_OK) {
        st = odin3_u64map_put(pp->cache_by_buffer, (odin3_kv){buf.v, 0});
    }
    pp->cmdline_buf = buf;
    return st;
}

/* Defines every project define from its <command line> line (continuations and comments off). */
static odin3_status cmdline_define(odin3_pp *pp, const odin3_pp_config *config) {
    const odin3_pp_cached *entry = odin3_vec_cat(&pp->cache, 0);
    odin3_loc base = odin3_srcman_loc(pp->sm, pp->cmdline_buf, 0);
    uint32_t start = 0;
    for (uint32_t i = 0; i < config->ndefines; i++) {
        const odin3_pp_define *def = &config->defines[i];
        uint32_t line_len =
            (uint32_t)(DEFINE_WORD + 1 + def->name.len + (def->has_value ? 1 + def->value.len : 0));
        const uint8_t *newline = memchr(entry->bytes + start, '\n', line_len);
        if (newline != NULL) {
            return odin3_pp_error(pp, odin3_pp_loc_add(base, (uint32_t)(newline - entry->bytes)),
                                  "syntax: newline in the text of project define '%.*s'",
                                  (int)def->name.len, (const char *)def->name.ptr);
        }
        odin3_pp_deftext text = {entry->bytes,
                                 start + line_len,
                                 base,
                                 start + DEFINE_WORD,
                                 odin3_pp_loc_add(base, start),
                                 false,
                                 false};
        odin3_status st = odin3_pp_define_scan(pp, &text);
        if (st != ODIN3_OK) {
            return st;
        }
        start += line_len + 1;
    }
    return ODIN3_OK;
}

static odin3_status copy_incdirs(odin3_pp *pp, const odin3_pp_config *config) {
    if (config->nincdirs == 0) {
        return ODIN3_OK;
    }
    pp->incdirs = (const char **)odin3_arena_alloc(pp->arena, sizeof(char *) * config->nincdirs);
    if (pp->incdirs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < config->nincdirs; i++) {
        const char *dir = config->incdirs[i];
        pp->incdirs[i] = odin3_arena_strndup(pp->arena, dir, strlen(dir));
        if (pp->incdirs[i] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    pp->nincdirs = config->nincdirs;
    return ODIN3_OK;
}

static odin3_status init(odin3_pp *pp, odin3_design *design) {
    pp->design = design;
    pp->strtab = odin3_design_strtab(design);
    odin3_vec_init(&pp->macros, sizeof(odin3_pp_macro));
    odin3_vec_init(&pp->cache, sizeof(odin3_pp_cached));
    odin3_vec_init(&pp->files_read, sizeof(uint32_t));
    odin3_vec_init(&pp->bufs, sizeof(odin3_pp_bufrec));
    odin3_vec_init(&pp->frames, sizeof(odin3_pp_frame));
    odin3_vec_init(&pp->runs, sizeof(odin3_pp_textrun));
    odin3_vec_init(&pp->conds, sizeof(odin3_pp_cond));
    odin3_vec_init(&pp->incstack, sizeof(uint32_t));
    odin3_vec_init(&pp->args, sizeof(odin3_pp_textrun));
    odin3_vec_init(&pp->arg_starts, sizeof(uint32_t));
    odin3_vec_init(&pp->scratch, sizeof(odin3_pp_textrun));
    odin3_strbuf_init(&pp->out);
    odin3_status st = odin3_design_get_srcman(design, &pp->sm);
    if (st != ODIN3_OK) {
        return st;
    }
    pp->arena = odin3_arena_create(ODIN3_PP_ARENA_CHUNK);
    pp->macro_by_name = odin3_u64map_create(0);
    pp->cache_by_path = odin3_u64map_create(0);
    pp->cache_by_buffer = odin3_u64map_create(0);
    bool ok = pp->arena != NULL && pp->macro_by_name != NULL && pp->cache_by_path != NULL &&
              pp->cache_by_buffer != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

static odin3_status create(odin3_design *design, const odin3_pp_config *config,
                           const odin3_pp_limits *limits, odin3_pp **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (design == NULL || config == NULL || out == NULL ||
        (config->ndefines > 0 && config->defines == NULL) ||
        (config->nincdirs > 0 && config->incdirs == NULL)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pp *pp = odin3_util_calloc(sizeof *pp);
    if (pp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = init(pp, design);
    pp->limits = *limits;
    if (st == ODIN3_OK) {
        st = copy_incdirs(pp, config);
    }
    if (st == ODIN3_OK) {
        st = cmdline_text(config, &text);
    }
    if (st == ODIN3_OK) {
        st = cmdline_buffer(pp, &text);
    }
    if (st == ODIN3_OK) {
        st = cmdline_define(pp, config);
    }
    odin3_strbuf_free(&text);
    if (st != ODIN3_OK) {
        odin3_pp_destroy(pp);
        return st;
    }
    *out = pp;
    return ODIN3_OK;
}

odin3_status odin3_pp_create(odin3_design *design, const odin3_pp_config *config, odin3_pp **out) {
    odin3_pp_limits limits = odin3_pp_default_limits();
    return create(design, config, &limits, out);
}

odin3_status odin3_pp_test_create(odin3_design *design, const odin3_pp_config *config,
                                  odin3_pp_limits limits, odin3_pp **out) {
    return create(design, config, &limits, out);
}

void odin3_pp_destroy(odin3_pp *pp) {
    if (pp == NULL) {
        return;
    }
    for (size_t i = 0; i < pp->cache.len; i++) {
        odin3_util_free(((odin3_pp_cached *)odin3_vec_at(&pp->cache, i))->bytes);
    }
    odin3_vec_free(&pp->cache);
    odin3_vec_free(&pp->macros);
    odin3_vec_free(&pp->files_read);
    odin3_vec_free(&pp->bufs);
    odin3_vec_free(&pp->frames);
    odin3_vec_free(&pp->runs);
    odin3_vec_free(&pp->conds);
    odin3_vec_free(&pp->incstack);
    odin3_vec_free(&pp->args);
    odin3_vec_free(&pp->arg_starts);
    odin3_vec_free(&pp->scratch);
    odin3_strbuf_free(&pp->out);
    odin3_u64map_destroy(pp->macro_by_name);
    odin3_u64map_destroy(pp->cache_by_path);
    odin3_u64map_destroy(pp->cache_by_buffer);
    odin3_arena_destroy(pp->arena);
    odin3_util_free(pp);
}
