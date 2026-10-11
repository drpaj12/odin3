/* pp_check.c — the preprocessor's stream invariants (spec §7.8) and their corruption hook. */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t k_none = UINT32_MAX; /* no buffer, segment or index */

/* --- the reachability walk ----------------------------------------------------------------- */

/* The index in pp->bufs of the EXPANSION/MACRO_ARG buffer of this run holding loc, k_none for a
 * FILE, a buffer of an earlier run or no buffer; *next = the walk's next loc (one spelling
 * step in a MACRO_ARG, the parent in an EXPANSION). */
static uint32_t walk_step(const odin3_pp *pp, odin3_loc loc, odin3_loc *next) {
    odin3_srcbuf_id buf = odin3_srcman_buffer_of(pp->sm, loc);
    uint32_t first = pp->first_buffer.v;
    odin3_srcbuf_info info;
    if (buf.v < first || buf.v - first >= pp->bufs.len ||
        !odin3_srcman_buffer_info(pp->sm, buf, &info) || info.kind == ODIN3_SRCBUF_FILE) {
        return k_none;
    }
    *next = info.kind == ODIN3_SRCBUF_MACRO_ARG ? odin3_srcman_spelling(pp->sm, loc)
                                                : (odin3_loc){info.parent};
    return buf.v - first;
}

/* Counts the walk from an emitted loc on every buffer it passes; a buffer already counted had
 * its whole walk counted before, so the walk stops there (a count is non-zero iff some emitted
 * byte's walk passes the buffer). */
void odin3_pp_check_note(odin3_pp *pp, odin3_loc loc) {
    odin3_loc next = {0};
    for (uint32_t index = walk_step(pp, loc, &next); index != k_none;
         index = walk_step(pp, loc, &next)) {
        odin3_pp_bufrec *rec = odin3_vec_at(&pp->bufs, index);
        bool counted = rec->reach != 0;
        rec->reach += rec->reach < UINT32_MAX ? 1 : 0;
        if (counted) {
            return;
        }
        loc = next;
    }
}

/* --- the check ----------------------------------------------------------------------------- */

static odin3_status violation(const char *fmt, ...) ODIN3_PRINTF(1, 2);

static odin3_status violation(const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_log(ODIN3_LOG_ERROR, "odin3: error: pp_check: %s", msg);
    return ODIN3_ERR_CHECK;
}

/* The stream's text and segment map. */
typedef struct stream_view {
    const uint8_t *text;
    uint32_t len;
    const odin3_srcseg_rec *segs;
    uint32_t count;
} stream_view;

static uint32_t seg_end(const stream_view *sv, uint32_t index) {
    return index + 1 < sv->count ? sv->segs[index + 1].out_offset : sv->len;
}

/* Segments strictly increasing from 0, inside the text, as many as the run made. */
static odin3_status check_order(const odin3_pp *pp, const stream_view *sv) {
    if (sv->count == 0) {
        return sv->len == 0 ? ODIN3_OK : violation("%u output bytes and no segment", sv->len);
    }
    if (sv->segs[0].out_offset != 0) {
        return violation("the first segment starts at output offset %u, not 0",
                         sv->segs[0].out_offset);
    }
    for (uint32_t i = 1; i < sv->count; i++) {
        if (sv->segs[i].out_offset <= sv->segs[i - 1].out_offset ||
            sv->segs[i].out_offset >= sv->len) {
            return violation("segment %u at output offset %u does not follow output offset %u", i,
                             sv->segs[i].out_offset, sv->segs[i - 1].out_offset);
        }
    }
    if (sv->count != pp->nsegments) {
        return violation("%u segments, but the run made %u", sv->count, pp->nsegments);
    }
    return ODIN3_OK;
}

/* The len bytes spelled at loc onward, chased by spelling steps to their FILE buffer's cached
 * bytes; false when they leave their buffer or reach no FILE this preprocessor made. */
static bool spelled(const odin3_pp *pp, odin3_loc loc, uint32_t len, const uint8_t **out) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(pp->sm, loc);
    if (rec == NULL || (uint64_t)(loc.v - rec->start) + len > rec->len) {
        return false;
    }
    for (uint32_t steps = 0; rec != NULL && steps <= pp->limits.expansion_chain + 1; steps++) {
        odin3_bytes bytes;
        if (rec->kind == ODIN3_SRCBUF_FILE) {
            bool ok = odin3_pp_file_bytes(pp, odin3_srcman_buffer_of(pp->sm, loc), &bytes) &&
                      (uint64_t)(loc.v - rec->start) + len <= bytes.len;
            *out = ok ? (const uint8_t *)bytes.ptr + (loc.v - rec->start) : NULL;
            return ok;
        }
        loc = odin3_srcman_spelling(pp->sm, loc);
        rec = odin3_srcman_rec_of(pp->sm, loc);
    }
    return false;
}

/* Segment index: its loc decodes and its bytes equal their spelling, or it is one loc-0
 * blank. */
static odin3_status check_segment(const odin3_pp *pp, const stream_view *sv, uint32_t index) {
    const odin3_srcseg_rec *seg = &sv->segs[index];
    uint32_t len = seg_end(sv, index) - seg->out_offset;
    if (seg->loc == 0) {
        if (len != 1) {
            return violation("loc-0 segment %u is %u bytes, not 1", index, len);
        }
        return odin3_pp_is_blank(sv->text[seg->out_offset])
                   ? ODIN3_OK
                   : violation("output byte %u has loc 0 and is not a blank", seg->out_offset);
    }
    odin3_srcpos pos;
    if (!odin3_srcman_decode(pp->sm, (odin3_loc){seg->loc}, &pos)) {
        return violation("segment %u has loc %u, which does not decode", index, seg->loc);
    }
    const uint8_t *bytes = NULL;
    if (!spelled(pp, (odin3_loc){seg->loc}, len, &bytes)) {
        return violation("segment %u runs past its buffer or reaches no file", index);
    }
    for (uint32_t k = 0; k < len; k++) {
        if (sv->text[seg->out_offset + k] != bytes[k]) {
            return violation("output byte %u differs from its spelling", seg->out_offset + k);
        }
    }
    return ODIN3_OK;
}

/* Every EXPANSION/MACRO_ARG buffer of the run whose count is non-zero is reached from some
 * segment's loc by the same walk. */
static odin3_status check_reach(const odin3_pp *pp, const stream_view *sv) {
    uint8_t *marked = odin3_util_calloc(pp->bufs.len + 1);
    if (marked == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < sv->count; i++) {
        odin3_loc loc = {sv->segs[i].loc};
        odin3_loc next = {0};
        for (uint32_t index = walk_step(pp, loc, &next); index != k_none && marked[index] == 0;
             index = walk_step(pp, loc, &next)) {
            marked[index] = 1;
            loc = next;
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; i < pp->bufs.len && st == ODIN3_OK; i++) {
        const odin3_pp_bufrec *rec = odin3_vec_cat(&pp->bufs, i);
        if (rec->reach != 0 && marked[i] == 0) {
            st = violation("buffer %u of the run is reached by emitted bytes but from no segment",
                           i);
        }
    }
    odin3_util_free(marked);
    return st;
}

/* The buffer record of stream when it is a project FILE buffer (parent 0), else NULL. */
static const odin3_srcbuf_rec *stream_rec(const odin3_srcman *sm, odin3_srcbuf_id stream) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, odin3_srcman_loc(sm, stream, 0));
    return rec != NULL && rec->kind == ODIN3_SRCBUF_FILE && rec->parent == 0 ? rec : NULL;
}

odin3_status odin3_pp_check(const odin3_pp *pp, odin3_srcbuf_id stream) {
    const odin3_srcbuf_rec *rec = pp == NULL ? NULL : stream_rec(pp->sm, stream);
    if (rec == NULL || stream.v != pp->stream.v) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_vec *segs = &odin3_srcman_file_of(pp->sm, rec)->segs;
    stream_view sv = {(const uint8_t *)pp->out.data, (uint32_t)pp->out.len, segs->data,
                      (uint32_t)segs->len};
    odin3_status st = check_order(pp, &sv);
    for (uint32_t i = 0; i < sv.count && st == ODIN3_OK; i++) {
        st = check_segment(pp, &sv, i);
    }
    return st == ODIN3_OK ? check_reach(pp, &sv) : st;
}

/* --- the corruption hook ------------------------------------------------------------------- */

/* The first segment whose loc is (zero) or is not (!zero) 0, k_none for none. */
static uint32_t first_seg(const odin3_vec *segs, bool zero) {
    for (uint32_t i = 0; i < segs->len; i++) {
        if ((((const odin3_srcseg_rec *)odin3_vec_cat(segs, i))->loc == 0) == zero) {
            return i;
        }
    }
    return k_none;
}

/* A zero-count EXPANSION/MACRO_ARG buffer of the run, k_none for none. */
static uint32_t exempt_buffer(const odin3_pp *pp) {
    for (uint32_t i = 0; i < pp->bufs.len; i++) {
        odin3_srcbuf_info info;
        bool ok =
            odin3_srcman_buffer_info(pp->sm, (odin3_srcbuf_id){pp->first_buffer.v + i}, &info);
        if (ok && info.kind != ODIN3_SRCBUF_FILE &&
            ((const odin3_pp_bufrec *)odin3_vec_cat(&pp->bufs, i))->reach == 0) {
            return i;
        }
    }
    return k_none;
}

/* Removes segment index + 1 (joining it to segment index). */
static void join_next(odin3_pp *pp, odin3_vec *segs, uint32_t index) {
    odin3_srcseg_rec *data = segs->data;
    memmove(&data[index + 1], &data[index + 2], sizeof *data * (segs->len - index - 2));
    odin3_vec_pop(segs);
    pp->nsegments--;
}

/* The segment (or byte) the target changes; k_none when the stream has none. */
static uint32_t target_index(const odin3_pp *pp, const odin3_vec *segs,
                             odin3_pp_test_target target) {
    switch (target) {
    case ODIN3_PP_CORRUPT_SEGMENT_START:
    case ODIN3_PP_CORRUPT_SEGMENT_LOC:
        return segs->len > 0 ? 0 : k_none;
    case ODIN3_PP_CORRUPT_SEGMENT_ORDER:
        return segs->len > 1 ? 1 : k_none;
    case ODIN3_PP_CORRUPT_BYTE:
        return first_seg(segs, false);
    case ODIN3_PP_CORRUPT_LOC0_BLANK:
        return first_seg(segs, true);
    case ODIN3_PP_CORRUPT_LOC0_ALONE: {
        uint32_t i = first_seg(segs, true);
        return i != k_none && i + 1 < segs->len ? i : k_none;
    }
    case ODIN3_PP_CORRUPT_REACH:
        return exempt_buffer(pp);
    default:
        return k_none;
    }
}

odin3_status odin3_pp_test_corrupt(odin3_pp *pp, odin3_pp_test_target target) {
    const odin3_srcbuf_rec *rec = pp == NULL ? NULL : stream_rec(pp->sm, pp->stream);
    odin3_srcfile_rec *file = rec == NULL ? NULL : odin3_vec_at(&pp->sm->files, rec->file);
    odin3_vec *segs = file == NULL ? NULL : &file->segs;
    uint32_t i = segs == NULL ? k_none : target_index(pp, segs, target);
    if (i == k_none) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_srcseg_rec *seg = segs->data;
    if (target == ODIN3_PP_CORRUPT_SEGMENT_START) {
        seg[0].out_offset = 1;
    } else if (target == ODIN3_PP_CORRUPT_SEGMENT_ORDER) {
        seg[1].out_offset = seg[0].out_offset;
    } else if (target == ODIN3_PP_CORRUPT_SEGMENT_LOC) {
        seg[0].loc = UINT32_MAX;
    } else if (target == ODIN3_PP_CORRUPT_BYTE || target == ODIN3_PP_CORRUPT_LOC0_BLANK) {
        char *byte = &pp->out.data[seg[i].out_offset];
        *byte = *byte == 'Z' ? 'Y' : 'Z';
    } else if (target == ODIN3_PP_CORRUPT_LOC0_ALONE) {
        join_next(pp, segs, i);
    } else {
        ((odin3_pp_bufrec *)odin3_vec_at(&pp->bufs, i))->reach = 1;
    }
    return ODIN3_OK;
}
