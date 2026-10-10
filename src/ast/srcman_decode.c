/*
 * srcman_decode.c — location decoding (spec §3.2): spelling, expansion and file locations, ends,
 * ranges, line/column and the provenance srcloc. Every walk is a loop: each step moves to a
 * buffer created earlier, so every walk ends.
 */
#include "ast/srcman.h"
#include "ir/prov.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The loc `loc - start` bytes into rec's spelling (EXPANSION, MACRO_ARG). */
static uint32_t def_step(const odin3_srcbuf_rec *rec, uint32_t loc) {
    return rec->def + (loc - rec->start);
}

odin3_loc odin3_srcman_spelling(const odin3_srcman *sm, odin3_loc loc) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, loc);
    if (rec == NULL || rec->kind == ODIN3_SRCBUF_SCRATCH) {
        return (odin3_loc){0};
    }
    return rec->kind == ODIN3_SRCBUF_FILE ? loc : (odin3_loc){def_step(rec, loc.v)};
}

odin3_loc odin3_srcman_expansion_loc(const odin3_srcman *sm, odin3_loc loc) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, loc);
    while (rec != NULL && rec->kind != ODIN3_SRCBUF_FILE) {
        loc.v = rec->parent;
        rec = odin3_srcman_rec_of(sm, loc);
    }
    return rec != NULL ? loc : (odin3_loc){0};
}

odin3_loc odin3_srcman_file_loc(const odin3_srcman *sm, odin3_loc loc) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, loc);
    while (rec != NULL && rec->kind != ODIN3_SRCBUF_FILE) {
        loc.v = rec->kind == ODIN3_SRCBUF_MACRO_ARG ? def_step(rec, loc.v) : rec->parent;
        rec = odin3_srcman_rec_of(sm, loc);
    }
    return rec != NULL ? loc : (odin3_loc){0};
}

odin3_loc odin3_srcman_expansion_end(const odin3_srcman *sm, odin3_loc end) {
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, end);
    while (rec != NULL && rec->kind != ODIN3_SRCBUF_FILE) {
        end.v = rec->parent_end;
        rec = odin3_srcman_rec_of(sm, end);
    }
    return rec != NULL ? end : (odin3_loc){0};
}

odin3_range odin3_srcman_expansion_range(const odin3_srcman *sm, odin3_range range) {
    odin3_range out = {odin3_srcman_expansion_loc(sm, range.loc), {0}};
    odin3_loc end = odin3_srcman_expansion_end(sm, range.end);
    if (out.loc.v != 0 && end.v != 0 && out.loc.v <= end.v &&
        odin3_srcman_buffer_of(sm, out.loc).v == odin3_srcman_buffer_of(sm, end).v) {
        out.end = end;
    }
    return out;
}

/* The number of line starts in lines (increasing) at or below offset. */
static uint32_t lines_at_or_below(const odin3_vec *lines, uint32_t offset) {
    const uint32_t *starts = lines->data;
    uint32_t low = 0;
    uint32_t high = (uint32_t)lines->len;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (starts[mid] <= offset) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

bool odin3_srcman_decode(const odin3_srcman *sm, odin3_loc loc, odin3_srcpos *pos) {
    odin3_loc file_loc = odin3_srcman_file_loc(sm, loc);
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, file_loc);
    if (rec == NULL) {
        return false;
    }
    const odin3_vec *lines = &odin3_srcman_file_of(sm, rec)->lines;
    uint32_t offset = file_loc.v - rec->start;
    uint32_t count = lines_at_or_below(lines, offset);
    uint32_t line_start = count > 0 ? *(const uint32_t *)odin3_vec_cat(lines, count - 1) : 0;
    odin3_srcpos out = {
        odin3_srcman_buffer_of(sm, file_loc).v, count + 1, 1 + offset - line_start, {0, 0}};
    *pos = out;
    return true;
}

void odin3_srcman_srcloc(const odin3_srcman *sm, odin3_range range, odin3_srcloc *out) {
    memset(out, 0, sizeof *out);
    out->loc = range.loc.v;
    odin3_srcpos pos;
    if (!odin3_srcman_decode(sm, range.loc, &pos)) {
        return;
    }
    const odin3_srcbuf_rec *rec = odin3_srcman_rec_of(sm, odin3_srcman_file_loc(sm, range.loc));
    out->file = rec->name;
    out->line = pos.line;
    out->col = pos.col;
    odin3_range expanded = odin3_srcman_expansion_range(sm, range);
    odin3_srcpos end;
    if (expanded.end.v != 0 && odin3_srcman_decode(sm, expanded.end, &end) &&
        end.buffer == pos.buffer) {
        out->end_line = end.line;
        out->end_col = end.col;
    }
}
