/* cond.c — token-based conditionals (PP-3, PP-4) and translate_off / translate_on (PP-10). */
#include "ast/srcman.h"
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *const k_cond_names[] = {"", "ifdef", "ifndef", "elsif", "else", "endif"};

/* --- conditionals -------------------------------------------------------------------------- */

/* The first conditional of the innermost FILE frame (conditionals never cross files, PP-4). */
static uint32_t file_cond_base(const odin3_pp *pp) {
    for (size_t i = pp->frames.len; i > 0; i--) {
        const odin3_pp_frame *frame = odin3_vec_cat(&pp->frames, i - 1);
        if (frame->kind == ODIN3_PP_FRAME_FILE) {
            return frame->cond_base;
        }
    }
    return 0;
}

/* The open conditional of this file, NULL when it has none. */
static odin3_pp_cond *open_cond(odin3_pp *pp) {
    if (pp->conds.len <= file_cond_base(pp)) {
        return NULL;
    }
    return odin3_vec_at(&pp->conds, pp->conds.len - 1);
}

/* Skipping iff the innermost conditional's branch is not taken: a frame opened inside a skipped
 * region is never taken, and an included file starts only where text is emitted. */
static void update_skipping(odin3_pp *pp) {
    const odin3_pp_cond *last =
        pp->conds.len == 0 ? NULL : odin3_vec_cat(&pp->conds, pp->conds.len - 1);
    pp->skipping = last != NULL && !last->taken;
}

/* The macro name argument of `ifdef/`ifndef/`elsif (spaces and tabs before it), consumed even
 * when skipping; *defined tells whether it names a macro. */
static odin3_status cond_name(odin3_pp *pp, odin3_pp_cond_kind kind, bool *defined) {
    odin3_pp_skip_spaces(pp);
    odin3_loc where = odin3_pp_loc_here(pp);
    odin3_bytes name = odin3_pp_read_ident(pp);
    if (name.len == 0) {
        return odin3_pp_error(pp, where, "syntax: expected a macro name after `%s",
                              k_cond_names[kind]);
    }
    *defined = odin3_pp_macro_find(pp, name) != 0;
    return ODIN3_OK;
}

static odin3_status push_cond(odin3_pp *pp, bool want_defined, bool defined, odin3_loc at) {
    if (pp->conds.len - file_cond_base(pp) >= pp->limits.cond_depth) {
        odin3_pp_cap cap = {"conditional depth", "ODIN3_PP_MAX_COND_DEPTH", pp->limits.cond_depth};
        return odin3_pp_cap_error(pp, at, cap);
    }
    odin3_pp_cond *cond = odin3_vec_push(&pp->conds);
    if (cond == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool active = !pp->skipping;
    bool taken = active && defined == want_defined;
    *cond = (odin3_pp_cond){at, taken, taken || !active, false};
    return ODIN3_OK;
}

static odin3_status elsif_cond(odin3_pp *pp, bool defined, odin3_loc at) {
    odin3_pp_cond *cond = open_cond(pp);
    if (cond == NULL) {
        return odin3_pp_error(pp, at, "syntax: `elsif without `ifdef");
    }
    if (cond->seen_else) {
        return odin3_pp_error(pp, at, "syntax: `elsif after `else");
    }
    cond->taken = !cond->seen_true && defined;
    cond->seen_true = cond->seen_true || cond->taken;
    return ODIN3_OK;
}

static odin3_status else_cond(odin3_pp *pp, odin3_loc at) {
    odin3_pp_cond *cond = open_cond(pp);
    if (cond == NULL) {
        return odin3_pp_error(pp, at, "syntax: `else without `ifdef");
    }
    if (cond->seen_else) {
        return odin3_pp_error(pp, at, "syntax: second `else");
    }
    cond->taken = !cond->seen_true;
    cond->seen_true = true;
    cond->seen_else = true;
    return ODIN3_OK;
}

static odin3_status endif_cond(odin3_pp *pp, odin3_loc at) {
    if (open_cond(pp) == NULL) {
        return odin3_pp_error(pp, at,
                              pp->conds.len == 0
                                  ? "syntax: `endif without `ifdef"
                                  : "syntax: `endif without `ifdef in this file (conditionals "
                                    "do not cross `include boundaries)");
    }
    odin3_vec_pop(&pp->conds);
    return ODIN3_OK;
}

odin3_status odin3_pp_cond_directive(odin3_pp *pp, odin3_pp_cond_kind kind, odin3_loc at) {
    bool defined = false;
    odin3_status st = ODIN3_OK;
    if (kind == ODIN3_PP_COND_IFDEF || kind == ODIN3_PP_COND_IFNDEF ||
        kind == ODIN3_PP_COND_ELSIF) {
        st = cond_name(pp, kind, &defined);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    switch (kind) {
    case ODIN3_PP_COND_IFDEF:
    case ODIN3_PP_COND_IFNDEF:
        st = push_cond(pp, kind == ODIN3_PP_COND_IFDEF, defined, at);
        break;
    case ODIN3_PP_COND_ELSIF:
        st = elsif_cond(pp, defined, at);
        break;
    case ODIN3_PP_COND_ELSE:
        st = else_cond(pp, at);
        break;
    default:
        st = endif_cond(pp, at);
        break;
    }
    update_skipping(pp);
    pp->gap = true;
    return st;
}

odin3_status odin3_pp_cond_file_end(odin3_pp *pp) {
    if (pp->frames.len == 1 && pp->xoff_loc.v != 0) {
        return odin3_pp_error(pp, pp->xoff_loc, "syntax: unterminated translate_off");
    }
    const odin3_pp_cond *cond = open_cond(pp);
    if (cond != NULL) {
        return odin3_pp_error(pp, cond->loc, "syntax: unterminated `ifdef");
    }
    return ODIN3_OK;
}

/* --- translate_off / translate_on (PP-10) -------------------------------------------------- */

typedef enum xoff_kind { XOFF_NONE, XOFF_OFF, XOFF_ON } xoff_kind;

typedef struct reader {
    const uint8_t *bytes;
    uint32_t len;
    uint32_t pos;
} reader;

static void skip_blanks(reader *rd, bool cr) {
    while (rd->pos < rd->len && (rd->bytes[rd->pos] == ' ' || rd->bytes[rd->pos] == '\t' ||
                                 (cr && rd->bytes[rd->pos] == '\r'))) {
        rd->pos++;
    }
}

static bool take_word(reader *rd, const char *word) {
    size_t len = strlen(word);
    if (rd->len - rd->pos < len || memcmp(rd->bytes + rd->pos, word, len) != 0) {
        return false;
    }
    rd->pos += (uint32_t)len;
    return true;
}

/* XOFF_OFF / XOFF_ON when the comment is exactly a line or block comment opener, blanks,
 * synopsys, synthesis or pragma, blanks, translate_off or translate_on, blanks, and the block
 * closer for a block comment; XOFF_NONE otherwise. */
static xoff_kind pragma_of(const odin3_pp_comment *comment) {
    static const char k_block_end[] = "*/";
    if (comment->bytes == NULL || comment->len < 2) {
        return XOFF_NONE;
    }
    bool block = comment->bytes[1] == '*';
    reader rd = {comment->bytes, comment->len, 2};
    skip_blanks(&rd, false);
    if (!take_word(&rd, "synopsys") && !take_word(&rd, "synthesis") && !take_word(&rd, "pragma")) {
        return XOFF_NONE;
    }
    skip_blanks(&rd, false);
    xoff_kind kind = XOFF_NONE;
    if (take_word(&rd, "translate_off")) {
        kind = XOFF_OFF;
    } else if (take_word(&rd, "translate_on")) {
        kind = XOFF_ON;
    }
    skip_blanks(&rd, !block);
    if (block && !take_word(&rd, k_block_end)) {
        return XOFF_NONE;
    }
    return rd.pos == rd.len ? kind : XOFF_NONE;
}

odin3_status odin3_pp_cond_comment(odin3_pp *pp, const odin3_pp_comment *comment, bool *drop) {
    xoff_kind kind = pragma_of(comment);
    *drop = pp->xoff_loc.v != 0 || kind == XOFF_OFF;
    if (pp->xoff_loc.v != 0) {
        if (kind == XOFF_ON) {
            pp->xoff_loc = (odin3_loc){0};
            pp->gap = true;
        }
        return ODIN3_OK;
    }
    if (kind == XOFF_OFF) {
        pp->xoff_loc = comment->loc;
        pp->gap = true;
    } else if (kind == XOFF_ON) {
        odin3_diag(pp->design, ODIN3_LOG_WARN, comment->loc,
                   "stray translate_on (no translate_off is open); ignored");
    }
    return ODIN3_OK;
}
