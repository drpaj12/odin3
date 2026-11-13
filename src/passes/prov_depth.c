/*
 * prov_depth.c — provenance depth (spec docs/specs/2026-11-13-prov-levels.md): the depth-to-knobs
 * table, the request (CLI over project), the size estimate and the resolved depth.
 */
#include "passes/prov_depth.h"

#include "frontends/project/project.h"
#include "ir/celltype.h"
#include "ir/construct.h"
#include "ir/module.h"
#include "passes/manager.h"
#include "util/log.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SIZE_SHIFT = 10,
    DECIMAL_BASE = 10,
    DEPTH_AUTO_TOP = ODIN3_PROV_DEPTH_FULL,
    SHAPE_TEXT_BYTES = 256
};

static const char *const k_depth_names[ODIN3_PROV_DEPTH_COUNT] = {"off", "statement", "ancestry",
                                                                  "cut", "full"};

/* The depth of today's behaviour, with no depth requested: it has no row of its own. */
odin3_prov_knobs odin3_prov_depth_knobs(uint32_t depth) {
    odin3_prov_knobs knobs = {ODIN3_CONSTRUCT_MODE_REFS, ODIN3_PROV_EDGES_DEFAULT, false, true,
                              false};
    if (depth == 0) {
        knobs = (odin3_prov_knobs){ODIN3_CONSTRUCT_MODE_OFF, ODIN3_PROV_EDGES_BULK, false, false,
                                   false};
    } else if (depth == 1) {
        knobs.edges = ODIN3_PROV_EDGES_BULK;
    } else if (depth >= 3) {
        knobs.cut = true;
        knobs.edges = depth >= DEPTH_AUTO_TOP ? ODIN3_PROV_EDGES_UNLIMITED : knobs.edges;
        knobs.steps = depth >= DEPTH_AUTO_TOP;
    }
    return knobs;
}

/* --- parsing ------------------------------------------------------------------------------- */

bool odin3_prov_depth_parse_level(const char *text, bool *autolevel, uint32_t *level) {
    if (text == NULL) {
        return false;
    }
    if (strcmp(text, "auto") == 0) {
        *autolevel = true;
        *level = DEPTH_AUTO_TOP;
        return true;
    }
    if (text[0] >= '0' && text[0] <= '0' + DEPTH_AUTO_TOP && text[1] == '\0') {
        *autolevel = false;
        *level = (uint32_t)(text[0] - '0');
        return true;
    }
    return false;
}

bool odin3_prov_depth_parse_size(const char *text, uint64_t *bytes) {
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    uint64_t value = 0;
    const char *cur = text;
    for (; *cur >= '0' && *cur <= '9'; cur++) {
        uint64_t digit = (uint64_t)(*cur - '0');
        if (value > (UINT64_MAX - digit) / DECIMAL_BASE) {
            return false;
        }
        value = value * DECIMAL_BASE + digit;
    }
    unsigned shift = 0;
    const char *suffix = strchr("KMG", *cur);
    if (*cur != '\0' && suffix != NULL) {
        shift = SIZE_SHIFT * (unsigned)(suffix - "KMG" + 1);
        cur++;
    }
    if (*cur != '\0' || value == 0 || value > (UINT64_MAX >> shift)) {
        return false;
    }
    *bytes = value << shift;
    return true;
}

/* --- the request --------------------------------------------------------------------------- */

odin3_prov_request odin3_prov_depth_request(const odin3_design *design) {
    odin3_prov_request req = {0};
    odin3_pass_options opts = odin3_pass_get_options();
    const odin3_project *proj = design != NULL ? odin3_design_project(design) : NULL;
    if (opts.prov_set) {
        req.set = true;
        req.autolevel = opts.prov_auto;
        req.level = opts.prov_level;
    } else if (proj != NULL && proj->prov_set) {
        req.set = true;
        req.autolevel = proj->prov_auto;
        req.level = proj->prov_level;
    }
    req.budget = opts.prov_budget;
    if (req.budget == 0 && proj != NULL) {
        req.budget = proj->prov_budget;
    }
    return req;
}

/* --- the estimate -------------------------------------------------------------------------- */

static const char *const k_feature_names[ODIN3_PROV_F_COUNT] = {
    "cells", "pins", "outs", "regs", "arith", "logic", "mux", "shift", "mul", "cons"};

const char *odin3_prov_feature_name(uint32_t feature) {
    return k_feature_names[feature < ODIN3_PROV_F_COUNT ? feature : 0];
}

/* The cell-type name starts with one of the prefixes (a NULL-terminated list). */
static bool name_in(const char *name, const char *const *prefixes) {
    for (; *prefixes != NULL; prefixes++) {
        if (strncmp(name, *prefixes, strlen(*prefixes)) == 0) {
            return true;
        }
    }
    return false;
}

/* The feature a cell type's work is added to. */
static odin3_prov_feature feature_of(const char *name) {
    static const char *const regs[] = {"$dff", "$adff", "$aldff", "$sdff",    "$dlatch", "$adlatch",
                                       "$sr",  "$_DFF", "$_SDFF", "$_DLATCH", "$_FF_",   NULL};
    static const char *const arith[] = {"$add", "$sub", "$neg", "$lt", "$le",
                                        "$gt",  "$ge",  "$eq",  "$ne", NULL};
    static const char *const mux[] = {"$mux", "$pmux", "$_MUX_", NULL};
    static const char *const shift[] = {"$shl", "$shr", "$sshr", "$shiftx", NULL};
    static const char *const mul[] = {"$mul", "$div", "$mod", "$pow", NULL};
    static const char *const logic[] = {"$and",   "$or",   "$xor",    "$xnor",  "$not",
                                        "$_AND",  "$_OR",  "$_XOR",   "$_XNOR", "$_NOT",
                                        "$_NAND", "$_NOR", "$_BUF",   "$logic", "$reduce",
                                        "$sop",   "$lut",  "$tribuf", NULL};
    if (name_in(name, regs)) {
        return ODIN3_PROV_F_REGS;
    }
    if (name_in(name, arith)) {
        return ODIN3_PROV_F_ARITH;
    }
    if (name_in(name, mux)) {
        return ODIN3_PROV_F_MUX;
    }
    if (name_in(name, shift)) {
        return ODIN3_PROV_F_SHIFT;
    }
    if (name_in(name, mul)) {
        return ODIN3_PROV_F_MUL;
    }
    return name_in(name, logic) ? ODIN3_PROV_F_LOGIC : ODIN3_PROV_F_CELLS;
}

/* The pins of the port of node named port_name, 0 when the type has none of that name. */
static uint64_t port_bits(const odin3_module *module, odin3_node_id node,
                          const odin3_celltype_def *def, const char *port_name) {
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (strcmp(def->ports[port].name, port_name) == 0) {
            return odin3_node_port(module, node, port).count;
        }
    }
    return 0;
}

static uint64_t bit_length(uint64_t value) {
    uint64_t bits = 0;
    for (; value != 0; value >>= 1) {
        bits++;
    }
    return bits;
}

/* The work of one cell of class which: see odin3_prov_shape. */
static uint64_t cell_work(const odin3_module *module, odin3_node_id node,
                          const odin3_celltype_def *def, odin3_prov_feature which,
                          uint64_t out_bits) {
    switch (which) {
    case ODIN3_PROV_F_MUX: {
        uint64_t sel = port_bits(module, node, def, "S");
        return out_bits * (sel > 0 ? sel : 1U);
    }
    case ODIN3_PROV_F_SHIFT:
        return out_bits * bit_length(out_bits);
    case ODIN3_PROV_F_MUL:
        return port_bits(module, node, def, "A") * port_bits(module, node, def, "B");
    default:
        return out_bits;
    }
}

static void measure_node(const odin3_design *design, const odin3_module *module, odin3_node_id node,
                         odin3_prov_shape *shape) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
    uint64_t out_bits = 0;
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        out_bits += odin3_pin_dir(module, pin) == ODIN3_DIR_OUT ? 1U : 0U;
    }
    shape->f[ODIN3_PROV_F_CELLS]++;
    shape->f[ODIN3_PROV_F_PINS] += pins.count;
    shape->f[ODIN3_PROV_F_OUTS] += out_bits;
    if (def == NULL || def->name == NULL) {
        return;
    }
    odin3_prov_feature which = feature_of(def->name);
    if (which == ODIN3_PROV_F_REGS) {
        shape->f[ODIN3_PROV_F_REGS]++;
    } else if (which != ODIN3_PROV_F_CELLS) {
        shape->f[which] += cell_work(module, node, def, which, out_bits);
    }
}

odin3_prov_shape odin3_prov_depth_measure(odin3_design *design) {
    odin3_prov_shape shape = {{0}};
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        for (uint32_t nid = 1; module != NULL && nid < odin3_module_node_end(module); nid++) {
            if (odin3_node_live(module, (odin3_node_id){nid})) {
                measure_node(design, module, (odin3_node_id){nid}, &shape);
            }
        }
    }
    uint32_t rows = odin3_construct_end(design);
    shape.f[ODIN3_PROV_F_CONSTRUCTS] = rows > 0 ? rows - 1U : 0U;
    return shape;
}

/* The shape as the log prints it: cells=N pins=N ... (every feature, in enum order). */
void odin3_prov_shape_text(const odin3_prov_shape *shape, char *out, size_t cap) {
    size_t used = 0;
    out[0] = '\0';
    for (uint32_t feature = 0; feature < ODIN3_PROV_F_COUNT && used < cap; feature++) {
        int wrote = snprintf(out + used, cap - used, "%s%s=%" PRIu64, feature == 0 ? "" : " ",
                             k_feature_names[feature], shape->f[feature]);
        used += wrote > 0 ? (size_t)wrote : 0;
    }
}

/*
 * One fitted measure of one depth: value = exp(scale + sum(exponent[f] * ln(1 + feature[f]))), a
 * power law in the features of the design (a straight line fits badly over designs from a few
 * cells to a few hundred thousand). used is false for a measure that is zero (depth 0, and the
 * step files below depth 4). The numbers are the fit of
 * docs/specs/2026-11-13-prov-levels-calibration.md (prov_depth_fit.h is written from it).
 */
typedef struct fit_row {
    bool used;
    double scale;
    double exponent[ODIN3_PROV_F_COUNT];
} fit_row;

enum { METRIC_RECORDS, METRIC_EDGES, METRIC_MAP, METRIC_STEPS, METRIC_COUNT };

#include "passes/prov_depth_fit.h"

static uint64_t fit_eval(const fit_row *row, const odin3_prov_shape *shape) {
    if (!row->used) {
        return 0;
    }
    double logv = row->scale;
    for (uint32_t feature = 0; feature < ODIN3_PROV_F_COUNT; feature++) {
        logv += row->exponent[feature] * log1p((double)shape->f[feature]);
    }
    return (uint64_t)exp(logv);
}

void odin3_prov_depth_estimate(const odin3_prov_shape *shape, odin3_prov_estimate *out) {
    for (uint32_t depth = 0; depth < ODIN3_PROV_DEPTH_COUNT; depth++) {
        out->records[depth] = fit_eval(&k_fit[depth][METRIC_RECORDS], shape);
        out->edges[depth] = fit_eval(&k_fit[depth][METRIC_EDGES], shape);
        out->map_bytes[depth] = fit_eval(&k_fit[depth][METRIC_MAP], shape);
        out->step_bytes[depth] = fit_eval(&k_fit[depth][METRIC_STEPS], shape);
    }
}

uint64_t odin3_prov_estimate_total(const odin3_prov_estimate *est, uint32_t depth) {
    return est->map_bytes[depth] + est->step_bytes[depth];
}

/* --- the resolved depth -------------------------------------------------------------------- */

typedef struct resolved {
    const odin3_design *design;
    odin3_prov_request req;
    int depth;
    bool valid;
} resolved;

static resolved g_resolved;

void odin3_prov_depth_reset(void) {
    g_resolved = (resolved){0};
}

static bool same_request(const odin3_prov_request *lhs, const odin3_prov_request *rhs) {
    return lhs->set == rhs->set && lhs->autolevel == rhs->autolevel && lhs->level == rhs->level &&
           lhs->budget == rhs->budget;
}

/* Highest depth up to want whose total fits the budget; 0 always fits (it writes nothing). */
static uint32_t fit_depth(const odin3_prov_estimate *est, const odin3_prov_request *req,
                          uint32_t want) {
    uint32_t depth = want;
    uint64_t budget = req->budget;
    while (depth > 0 && budget != 0 && odin3_prov_estimate_total(est, depth) > budget) {
        depth--;
    }
    return depth;
}

/* The depth asked for: the given one, else the top (auto, or a budget alone). */
static uint32_t want_of(const odin3_prov_request *req) {
    return req->set && !req->autolevel ? req->level : DEPTH_AUTO_TOP;
}

static int resolve(odin3_design *design, const odin3_prov_request *req) {
    uint32_t want = want_of(req);
    if (req->budget == 0) {
        if (req->autolevel) {
            odin3_log(ODIN3_LOG_WARN, "prov-level auto needs --prov-budget; using depth 4");
        }
        return (int)want;
    }
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    uint32_t depth = fit_depth(&est, req, want);
    if (depth < want) {
        odin3_log(ODIN3_LOG_WARN,
                  "provenance depth %u (%s) is estimated at %" PRIu64
                  " bytes, over the budget of %" PRIu64 " bytes; using depth %u (%s), estimated "
                  "at %" PRIu64 " bytes",
                  (unsigned)want, k_depth_names[want], odin3_prov_estimate_total(&est, want),
                  req->budget, (unsigned)depth, k_depth_names[depth],
                  odin3_prov_estimate_total(&est, depth));
    } else if (req->autolevel) {
        odin3_log(ODIN3_LOG_INFO,
                  "prov-level auto: depth %u (%s), estimated at %" PRIu64
                  " bytes, within the budget of %" PRIu64 " bytes",
                  (unsigned)depth, k_depth_names[depth], odin3_prov_estimate_total(&est, depth),
                  req->budget);
    }
    return (int)depth;
}

int odin3_prov_depth_requested(const odin3_design *design) {
    odin3_prov_request req = odin3_prov_depth_request(design);
    if (!req.set && req.budget == 0) {
        return -1;
    }
    return (int)want_of(&req);
}

int odin3_prov_depth_effective(odin3_design *design) {
    odin3_prov_request req = odin3_prov_depth_request(design);
    if (!req.set && req.budget == 0) {
        return -1;
    }
    if (g_resolved.valid && g_resolved.design == design && same_request(&g_resolved.req, &req)) {
        return g_resolved.depth;
    }
    int depth = resolve(design, &req);
    g_resolved = (resolved){design, req, depth, true};
    return depth;
}

bool odin3_prov_depth_wants_map(odin3_design *design) {
    int depth = odin3_prov_depth_effective(design);
    return depth >= 0 && odin3_prov_depth_knobs((uint32_t)depth).map;
}

void odin3_prov_depth_after_proc(odin3_design *design) {
    odin3_prov_request req = odin3_prov_depth_request(design);
    if (!req.set && req.budget == 0) {
        return;
    }
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    int depth = odin3_prov_depth_effective(design);
    char text[SHAPE_TEXT_BYTES];
    odin3_prov_shape_text(&shape, text, sizeof text);
    odin3_log(ODIN3_LOG_INFO,
              "proc: provenance depth %d (%s): estimated %" PRIu64 " records, %" PRIu64
              " edges, %" PRIu64 " bytes (name map %" PRIu64 ", step files %" PRIu64 ") from %s",
              depth, k_depth_names[depth], est.records[depth], est.edges[depth],
              odin3_prov_estimate_total(&est, (uint32_t)depth), est.map_bytes[depth],
              est.step_bytes[depth], text);
}

const char *odin3_prov_depth_name(uint32_t depth) {
    return k_depth_names[depth < ODIN3_PROV_DEPTH_COUNT ? depth : 0];
}
