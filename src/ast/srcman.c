/* srcman.c — the source manager's tables: buffers, line maps, segment maps and the cursor. */
#include "ast/srcman.h"
#include "ast/srcman_test.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- lifetime ------------------------------------------------------------------------------ */

odin3_srcman *odin3_srcman_create(odin3_strtab *strtab) {
    odin3_srcman *sm = odin3_util_calloc(sizeof *sm);
    if (sm == NULL) {
        return NULL;
    }
    sm->strtab = strtab;
    odin3_vec_init(&sm->bufs, sizeof(odin3_srcbuf_rec));
    odin3_vec_init(&sm->files, sizeof(odin3_srcfile_rec));
    sm->next = 1;
    sm->max_buffers = ODIN3_SRC_MAX_BUFFERS;
    sm->space_end = UINT32_MAX;
    if (odin3_vec_push(&sm->bufs) == NULL || odin3_vec_push(&sm->files) == NULL) {
        odin3_srcman_destroy(sm);
        return NULL;
    }
    return sm;
}

void odin3_srcman_destroy(odin3_srcman *sm) {
    if (sm == NULL) {
        return;
    }
    for (size_t i = 0; i < sm->files.len; i++) {
        odin3_srcfile_rec *file = odin3_vec_at(&sm->files, i);
        odin3_vec_free(&file->lines);
        odin3_vec_free(&file->segs);
    }
    odin3_vec_free(&sm->bufs);
    odin3_vec_free(&sm->files);
    odin3_util_free(sm);
}

static size_t vec_bytes(const odin3_vec *vec) {
    return vec->cap * vec->elem_size;
}

size_t odin3_srcman_bytes_reserved(const odin3_srcman *sm) {
    size_t bytes = sizeof *sm + vec_bytes(&sm->bufs) + vec_bytes(&sm->files);
    for (size_t i = 0; i < sm->files.len; i++) {
        const odin3_srcfile_rec *file = odin3_vec_cat(&sm->files, i);
        bytes += vec_bytes(&file->lines) + vec_bytes(&file->segs);
    }
    return bytes;
}

void odin3_srcman_test_set_limits(odin3_srcman *sm, odin3_srcman_limits limits) {
    sm->max_buffers = limits.max_buffers;
    sm->space_end = limits.space_end;
}

/* --- lookups ------------------------------------------------------------------------------- */

/* The ID of the buffer holding loc: the last start at or below it; 0 outside the space. */
static uint32_t find_buffer(const odin3_srcman *sm, uint32_t loc) {
    if (loc == 0 || loc >= sm->next) {
        return 0;
    }
    const odin3_srcbuf_rec *recs = sm->bufs.data;
    uint32_t low = 1;
    uint32_t high = (uint32_t)sm->bufs.len;
    while (high - low > 1) {
        uint32_t mid = low + (high - low) / 2;
        if (recs[mid].start <= loc) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

static const odin3_srcbuf_rec *rec_by_id(const odin3_srcman *sm, uint32_t id) {
    return id != 0 && id < sm->bufs.len ? odin3_vec_cat(&sm->bufs, id) : NULL;
}

const odin3_srcbuf_rec *odin3_srcman_rec_of(const odin3_srcman *sm, odin3_loc loc) {
    return rec_by_id(sm, find_buffer(sm, loc.v));
}

const odin3_srcfile_rec *odin3_srcman_file_of(const odin3_srcman *sm, const odin3_srcbuf_rec *rec) {
    return odin3_vec_cat(&sm->files, rec->file);
}

static bool is_loc(const odin3_srcman *sm, uint32_t loc) {
    return loc != 0 && loc < sm->next;
}

static bool is_str(const odin3_srcman *sm, uint32_t str) {
    return str < odin3_strtab_count(sm->strtab);
}

odin3_loc odin3_srcman_loc(const odin3_srcman *sm, odin3_srcbuf_id buf, uint32_t offset) {
    const odin3_srcbuf_rec *rec = rec_by_id(sm, buf.v);
    return (odin3_loc){rec != NULL && offset <= rec->len ? rec->start + offset : 0};
}

odin3_srcbuf_id odin3_srcman_buffer_of(const odin3_srcman *sm, odin3_loc loc) {
    return (odin3_srcbuf_id){find_buffer(sm, loc.v)};
}

bool odin3_srcman_buffer_info(const odin3_srcman *sm, odin3_srcbuf_id buf, odin3_srcbuf_info *out) {
    const odin3_srcbuf_rec *rec = rec_by_id(sm, buf.v);
    if (rec == NULL) {
        return false;
    }
    odin3_srcbuf_info info = {rec->kind,       rec->name, 0,          0,        rec->parent,
                              rec->parent_end, rec->def,  rec->start, rec->len, 0};
    if (rec->kind == ODIN3_SRCBUF_FILE) {
        const odin3_srcfile_rec *file = odin3_srcman_file_of(sm, rec);
        info.resolved = file->resolved;
        info.library = file->library;
        info.lines = (uint32_t)file->lines.len + 1;
    }
    *out = info;
    return true;
}

/* --- adding buffers ------------------------------------------------------------------------ */

static void parse_error(const odin3_srcman *sm, uint32_t at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static void parse_error(const odin3_srcman *sm, uint32_t at, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    odin3_srcman_vdiag(sm, ODIN3_LOG_ERROR, (odin3_loc){at}, fmt, args);
    va_end(args);
}

/* PARSE (located at parent) when one more buffer of len bytes would pass a limit. */
static odin3_status check_room(const odin3_srcman *sm, odin3_loc parent, uint32_t len) {
    if (sm->bufs.len - 1 >= sm->max_buffers) {
        parse_error(sm, parent.v, "too many source buffers (limit %" PRIu32 ")", sm->max_buffers);
        return ODIN3_ERR_PARSE;
    }
    if (sm->next + len > sm->space_end) {
        parse_error(sm, parent.v, "source location space exhausted");
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* Appends a buffer record (room reserved) and returns its ID. */
static odin3_srcbuf_id push_buffer(odin3_srcman *sm, odin3_srcbuf_rec rec) {
    odin3_srcbuf_rec *slot = odin3_vec_push(&sm->bufs);
    rec.start = (uint32_t)sm->next;
    *slot = rec;
    sm->next += (uint64_t)rec.len + 1;
    return (odin3_srcbuf_id){(uint32_t)(sm->bufs.len - 1)};
}

static const char *check_file(const odin3_srcman *sm, const odin3_srcfile_spec *spec) {
    if (spec->reserved[0] != 0 || spec->reserved[1] != 0) {
        return "reserved fields must be zero";
    }
    if (spec->name == 0 || !is_str(sm, spec->name)) {
        return "name is not a non-empty strtab ID";
    }
    if (!is_str(sm, spec->resolved) || !is_str(sm, spec->library)) {
        return "resolved or library is not a strtab ID";
    }
    if (spec->parent != 0 && !is_loc(sm, spec->parent)) {
        return "parent is not a location";
    }
    return NULL;
}

static odin3_status reserve_file(odin3_srcman *sm) {
    if (odin3_vec_reserve(&sm->bufs, sm->bufs.len + 1) != ODIN3_OK ||
        odin3_vec_reserve(&sm->files, sm->files.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

odin3_status odin3_srcman_add_file(odin3_srcman *sm, const odin3_srcfile_spec *spec,
                                   odin3_srcbuf_id *out) {
    const char *why = sm == NULL || spec == NULL || out == NULL ? "NULL argument" : NULL;
    why = why != NULL ? why : check_file(sm, spec);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_add_file: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (spec->len > ODIN3_SRC_MAX_FILE_BYTES) {
        parse_error(sm, spec->parent, "file '%s' is %" PRIu32 " bytes, over the limit of %" PRIu32,
                    odin3_strtab_get(sm->strtab, spec->name), spec->len, ODIN3_SRC_MAX_FILE_BYTES);
        return ODIN3_ERR_PARSE;
    }
    odin3_status st = check_room(sm, (odin3_loc){spec->parent}, spec->len);
    if (st == ODIN3_OK) {
        st = reserve_file(sm);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_srcfile_rec *file = odin3_vec_push(&sm->files);
    odin3_vec_init(&file->lines, sizeof(uint32_t));
    odin3_vec_init(&file->segs, sizeof(odin3_srcseg_rec));
    file->resolved = spec->resolved;
    file->library = spec->library;
    odin3_srcbuf_rec rec = {.len = spec->len,
                            .name = spec->name,
                            .parent = spec->parent,
                            .file = (uint32_t)(sm->files.len - 1),
                            .kind = ODIN3_SRCBUF_FILE};
    *out = push_buffer(sm, rec);
    return ODIN3_OK;
}

/* Why def cannot hold len bytes as spelled (NULL when it can). */
static const char *check_def(const odin3_srcman *sm, const odin3_expansion_spec *spec) {
    if (spec->kind == ODIN3_SRCBUF_SCRATCH) {
        return spec->def == 0 && spec->name == 0 ? NULL : "a SCRATCH buffer takes no name or def";
    }
    if (spec->name == 0 || !is_str(sm, spec->name)) {
        return "name is not a non-empty strtab ID";
    }
    const odin3_srcbuf_rec *def = odin3_srcman_rec_of(sm, (odin3_loc){spec->def});
    if (def == NULL) {
        return "def is not a location";
    }
    if ((uint64_t)(spec->def - def->start) + spec->len > def->len) {
        return "def's buffer cannot hold len bytes from def";
    }
    return NULL;
}

static const char *check_expansion(const odin3_srcman *sm, const odin3_expansion_spec *spec) {
    if (spec->kind < ODIN3_SRCBUF_EXPANSION || spec->kind > ODIN3_SRCBUF_SCRATCH) {
        return "kind is not EXPANSION, MACRO_ARG or SCRATCH";
    }
    if (spec->reserved[0] != 0 || spec->reserved[1] != 0) {
        return "reserved fields must be zero";
    }
    if (!is_loc(sm, spec->parent)) {
        return "parent is not a location";
    }
    if (spec->parent_end != 0 && !is_loc(sm, spec->parent_end)) {
        return "parent_end is not a location";
    }
    return check_def(sm, spec);
}

odin3_status odin3_srcman_add_expansion(odin3_srcman *sm, const odin3_expansion_spec *spec,
                                        odin3_srcbuf_id *out) {
    const char *why = sm == NULL || spec == NULL || out == NULL ? "NULL argument" : NULL;
    why = why != NULL ? why : check_expansion(sm, spec);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_add_expansion: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = check_room(sm, (odin3_loc){spec->parent}, spec->len);
    if (st == ODIN3_OK && odin3_vec_reserve(&sm->bufs, sm->bufs.len + 1) != ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_srcbuf_rec rec = {.len = spec->len,
                            .name = spec->name,
                            .parent = spec->parent,
                            .parent_end = spec->parent_end,
                            .def = spec->def,
                            .kind = (uint8_t)spec->kind};
    *out = push_buffer(sm, rec);
    return ODIN3_OK;
}

/* --- lines and segments -------------------------------------------------------------------- */

/* The file record of FILE buffer id, NULL when id is not a FILE buffer. */
static odin3_srcfile_rec *file_by_id(odin3_srcman *sm, uint32_t id) {
    const odin3_srcbuf_rec *rec = rec_by_id(sm, id);
    return rec != NULL && rec->kind == ODIN3_SRCBUF_FILE ? odin3_vec_at(&sm->files, rec->file)
                                                         : NULL;
}

odin3_status odin3_srcman_add_line(odin3_srcman *sm, odin3_srcbuf_id buf, uint32_t offset) {
    odin3_srcfile_rec *file = sm != NULL ? file_by_id(sm, buf.v) : NULL;
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_add_line: %" PRIu32 " is not a FILE buffer",
                  buf.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_srcbuf_rec *rec = rec_by_id(sm, buf.v);
    uint32_t last = file->lines.len > 0
                        ? *(const uint32_t *)odin3_vec_cat(&file->lines, file->lines.len - 1)
                        : 0;
    if (offset <= last || offset > rec->len) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_srcman_add_line: offset %" PRIu32 " is not in (%" PRIu32 ", %" PRIu32 "]",
                  offset, last, rec->len);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t *slot = odin3_vec_push(&file->lines);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = offset;
    return ODIN3_OK;
}

static const char *check_segment(odin3_srcman *sm, const odin3_segment *seg) {
    if (seg->reserved[0] != 0 || seg->reserved[1] != 0) {
        return "reserved fields must be zero";
    }
    const odin3_srcbuf_rec *rec = rec_by_id(sm, seg->stream);
    if (rec == NULL || rec->kind != ODIN3_SRCBUF_FILE || rec->parent != 0) {
        return "stream is not a project FILE buffer";
    }
    if (seg->loc != 0 && !is_loc(sm, seg->loc)) {
        return "loc is not a location";
    }
    const odin3_vec *segs = &file_by_id(sm, seg->stream)->segs;
    if (segs->len > 0 &&
        ((const odin3_srcseg_rec *)odin3_vec_cat(segs, segs->len - 1))->out_offset >=
            seg->out_offset) {
        return "out_offset does not increase";
    }
    return NULL;
}

odin3_status odin3_srcman_add_segment(odin3_srcman *sm, const odin3_segment *seg) {
    const char *why = sm == NULL || seg == NULL ? "NULL argument" : check_segment(sm, seg);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_srcman_add_segment: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_srcseg_rec *slot = odin3_vec_push(&file_by_id(sm, seg->stream)->segs);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->out_offset = seg->out_offset;
    slot->loc = seg->loc;
    return ODIN3_OK;
}

/* --- the cursor ---------------------------------------------------------------------------- */

void odin3_srcman_cursor_init(odin3_srcman_cursor *cur, const odin3_srcman *sm, uint32_t stream) {
    const odin3_srcbuf_rec *rec = rec_by_id(sm, stream);
    cur->sm = sm;
    cur->file = rec != NULL && rec->kind == ODIN3_SRCBUF_FILE ? rec->file : 0;
    cur->seg = 0;
}

/* The last segment at or before out (segs[0] must be at or before it). */
static uint32_t seg_search(uint32_t out, const odin3_srcseg_rec *segs, uint32_t count) {
    uint32_t low = 0;
    uint32_t high = count;
    while (high - low > 1) {
        uint32_t mid = low + (high - low) / 2;
        if (segs[mid].out_offset <= out) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

odin3_loc odin3_srcman_cursor_loc(odin3_srcman_cursor *cur, uint32_t out_offset) {
    odin3_loc none = {0};
    if (cur->file == 0) {
        return none;
    }
    const odin3_srcfile_rec *file = odin3_vec_cat(&cur->sm->files, cur->file);
    const odin3_srcseg_rec *segs = file->segs.data;
    uint32_t count = (uint32_t)file->segs.len;
    if (count == 0 || segs[0].out_offset > out_offset) {
        return none;
    }
    uint32_t i = cur->seg < count ? cur->seg : 0;
    if (segs[i].out_offset > out_offset) {
        i = seg_search(out_offset, segs, i);
    } else if (i + 1 < count && segs[i + 1].out_offset <= out_offset) {
        i++; /* the next segment: O(1) for a monotone scan; a longer jump searches */
        if (i + 1 < count && segs[i + 1].out_offset <= out_offset) {
            i += seg_search(out_offset, segs + i, count - i);
        }
    }
    cur->seg = i;
    if (segs[i].loc == 0) {
        return none;
    }
    return (odin3_loc){segs[i].loc + (out_offset - segs[i].out_offset)};
}
