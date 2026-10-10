/*
 * srcman_abi.c — the public ABI's source-manager group (ABI v4): buffer, line and segment
 * builders and the location decoders, over src/ast/srcman.h.
 */
#include "api/api.h"

#include "ast/srcman.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True for 0 and for a loc of a buffer made so far. */
static bool loc_ok(const odin3_srcman *sm, uint32_t loc) {
    return loc == 0 || odin3_srcman_buffer_of(sm, (odin3_loc){loc}).v != 0;
}

/* --- builders -------------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_srcman_build_file(odin3_srcman *sm, const odin3_srcfile_spec *spec,
                                                  uint32_t *buffer) {
    if (sm == NULL || spec == NULL || buffer == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_srcbuf_id id = {0};
    odin3_status st = odin3_srcman_add_file(sm, spec, &id);
    if (st == ODIN3_OK) {
        *buffer = id.v;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_srcman_build_expansion(odin3_srcman *sm,
                                                       const odin3_expansion_spec *spec,
                                                       uint32_t *buffer) {
    if (sm == NULL || spec == NULL || buffer == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_srcbuf_id id = {0};
    odin3_status st = odin3_srcman_add_expansion(sm, spec, &id);
    if (st == ODIN3_OK) {
        *buffer = id.v;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_srcman_build_line(odin3_srcman *sm, uint32_t buffer,
                                                  uint32_t offset) {
    if (sm == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_srcman_add_line(sm, (odin3_srcbuf_id){buffer}, offset);
}

ODIN3_EXPORT odin3_status odin3_srcman_build_segment(odin3_srcman *sm, const odin3_segment *seg) {
    if (sm == NULL || seg == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_srcman_add_segment(sm, seg);
}

/* --- queries --------------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_srcman_get_loc(const odin3_srcman *sm, uint32_t buffer,
                                               uint32_t offset, uint32_t *loc) {
    uint32_t found = sm != NULL ? odin3_srcman_loc(sm, (odin3_srcbuf_id){buffer}, offset).v : 0;
    if (found == 0 || loc == NULL) {
        return odin3_api_invalid(__func__); /* a real loc is never 0: buffers start at 1 */
    }
    *loc = found;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_srcman_get_position(const odin3_srcman *sm, uint32_t loc,
                                                    odin3_srcpos *pos) {
    odin3_srcpos got = {0};
    if (sm == NULL || pos == NULL || !odin3_srcman_decode(sm, (odin3_loc){loc}, &got)) {
        return odin3_api_invalid(__func__);
    }
    *pos = got;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_srcman_get_file_loc(const odin3_srcman *sm, uint32_t loc,
                                                    uint32_t *out) {
    if (sm == NULL || out == NULL || !loc_ok(sm, loc)) {
        return odin3_api_invalid(__func__);
    }
    *out = odin3_srcman_file_loc(sm, (odin3_loc){loc}).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_srcman_get_expansion_loc(const odin3_srcman *sm, uint32_t loc,
                                                         uint32_t *out) {
    if (sm == NULL || out == NULL || !loc_ok(sm, loc)) {
        return odin3_api_invalid(__func__);
    }
    *out = odin3_srcman_expansion_loc(sm, (odin3_loc){loc}).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_srcman_get_spelling(const odin3_srcman *sm, uint32_t loc,
                                                    uint32_t *out) {
    if (sm == NULL || out == NULL || !loc_ok(sm, loc)) {
        return odin3_api_invalid(__func__);
    }
    *out = odin3_srcman_spelling(sm, (odin3_loc){loc}).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_srcman_get_format(odin3_srcman *sm, uint32_t loc,
                                                  const char **text) {
    if (sm == NULL || text == NULL || !loc_ok(sm, loc)) {
        return odin3_api_invalid(__func__);
    }
    odin3_strbuf_clear(&sm->abi_text);
    odin3_status st = odin3_srcman_format(sm, (odin3_loc){loc}, &sm->abi_text, ODIN3_SRCFMT_GIVEN);
    if (st == ODIN3_OK) {
        *text = sm->abi_text.data;
    }
    return st;
}
