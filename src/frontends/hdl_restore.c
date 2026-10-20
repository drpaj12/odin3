/* hdl_restore.c — the reader's record for restored HDL locations; see hdl_restore.h. */
#include "frontends/hdl_restore.h"

#include "ir/design.h"
#include "util/log.h"

/* The design's locations for the first count parsed ones. */
static odin3_status intern_locs(odin3_design *design, const odin3_hdl_restore_req *req,
                                uint32_t count, odin3_srcloc *out) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t file = 0;
        odin3_status st = odin3_design_intern(design, req->locs[i].file, &file);
        if (st != ODIN3_OK) {
            return st;
        }
        out[i] = (odin3_srcloc){
            file, req->locs[i].line, req->locs[i].col, req->locs[i].end_line, req->locs[i].end_col,
            0};
    }
    return ODIN3_OK;
}

odin3_status odin3_hdl_restore(odin3_pass_ctx *ctx, const odin3_hdl_restore_req *req,
                               odin3_hdl_restored *out) {
    odin3_srcloc locs[ODIN3_HDL_LOC_MAX];
    if (ctx == NULL || ctx->design == NULL || req == NULL || out == NULL || req->locs == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "hdl_restore: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (ctx->op == 0) {
        odin3_log(ODIN3_LOG_ERROR, "hdl_restore: no operation begun");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (req->n == 0) {
        odin3_log(ODIN3_LOG_ERROR, "hdl_restore: nothing to restore");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t count = req->n < ODIN3_HDL_LOC_MAX ? req->n : ODIN3_HDL_LOC_MAX;
    odin3_status st = intern_locs(ctx->design, req, count, locs);
    odin3_prov_id source = {0};
    if (st == ODIN3_OK) {
        odin3_prov_origin origin = {locs, count, 0, req->hier_str};
        st = odin3_prov_source(ctx, &origin, &source);
    }
    if (st == ODIN3_OK) {
        const odin3_prov_id parents[2] = {source, req->base};
        uint32_t n_parents = req->base.v != 0 ? 2 : 1;
        st = odin3_prov_derive_hier(ctx, (odin3_prov_list){parents, n_parents}, req->hier_str,
                                    &out->prov);
    }
    out->capped = st == ODIN3_OK && req->n > ODIN3_HDL_LOC_MAX;
    return st;
}
