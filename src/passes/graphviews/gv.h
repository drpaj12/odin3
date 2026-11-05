/*
 * gv.h — private declarations of the write_graph pass (spec docs/specs/2026-11-05-graph-views-
 * design.md): the JSON helpers shared by the graph kinds and the kind entry points.
 */
#ifndef ODIN3_PASSES_GRAPHVIEWS_GV_H
#define ODIN3_PASSES_GRAPHVIEWS_GV_H

#include "backends/common/hdl_loc.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* The run state: the design, the HDL location cache of the write, and one buffer per module (the
 * graph objects of that module, comma separated, filled by the kind). Index = module ID. */
typedef struct gv_run {
    odin3_design *design;
    odin3_hdl_ctx *hdl;
    odin3_strbuf *mods;
    uint32_t n_mods; /* module IDs 1 .. n_mods - 1 are valid */
} gv_run;

/* Appends text as a JSON string (quoted; `"` `\` and control bytes escaped). */
odin3_status gv_put_str(odin3_strbuf *out, const char *text);

/* Appends `"prov": N, "src": ["file:L.C-EL.EC", ...]` for the provenance record prov. */
odin3_status gv_put_prov(gv_run *run, odin3_strbuf *out, odin3_prov_id prov);

/* Appends a separator `,` when *first is false, then clears it. */
odin3_status gv_sep(odin3_strbuf *out, bool *first);

/* The text of strtab ID str; "" for 0. */
const char *gv_str(const gv_run *run, uint32_t str);

/* A JSON writer that remembers the first failure: after one, every call does nothing. */
typedef struct gv_w {
    gv_run *run;
    odin3_strbuf *out;
    odin3_status st;
} gv_w;

void gv_w_raw(gv_w *wr, const char *text);
void gv_w_fmt(gv_w *wr, const char *fmt, ...) ODIN3_PRINTF(2, 3);
void gv_w_str(gv_w *wr, const char *text);
void gv_w_prov(gv_w *wr, odin3_prov_id prov);

/* The kinds (each fills run->mods). */
odin3_status gv_write_stg(gv_run *run);
odin3_status gv_write_cfg(gv_run *run);

#endif
