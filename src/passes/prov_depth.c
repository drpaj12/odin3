/*
 * prov_depth.c — provenance depth (spec docs/specs/2026-11-13-prov-levels.md): the depth-to-knobs
 * table, the request (CLI over project), the size estimate and the resolved depth.
 */
#include "passes/prov_depth.h"

#include "frontends/project/project.h"
#include "ir/module.h"
#include "passes/manager.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { SIZE_SHIFT = 10, DECIMAL_BASE = 10, DEPTH_AUTO_TOP = ODIN3_PROV_DEPTH_FULL };

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
    if (*cur == 'K' || *cur == 'M' || *cur == 'G') {
        shift = SIZE_SHIFT * (*cur == 'K' ? 1U : *cur == 'M' ? 2U : 3U);
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
    req.budget = opts.prov_budget != 0 ? opts.prov_budget : (proj != NULL ? proj->prov_budget : 0);
    return req;
}

/* --- the estimate -------------------------------------------------------------------------- */

odin3_prov_shape odin3_prov_depth_measure(odin3_design *design) {
    odin3_prov_shape shape = {0, 0};
    for (uint32_t mid = 1; mid < odin3_design_module_end(design); mid++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){mid});
        for (uint32_t nid = 1; module != NULL && nid < odin3_module_node_end(module); nid++) {
            if (odin3_node_live(module, (odin3_node_id){nid})) {
                shape.nodes++;
                shape.pins += odin3_node_pins(module, (odin3_node_id){nid}).count;
            }
        }
    }
    return shape;
}

/* value = base + per_node * nodes + per_pin * pins, one row per depth and metric. The numbers are
 * the fit of docs/specs/2026-11-13-prov-levels-calibration.md. */
typedef struct fit_row {
    double base, per_node, per_pin;
} fit_row;

enum { METRIC_RECORDS, METRIC_EDGES, METRIC_MAP, METRIC_STEPS, METRIC_COUNT };

static const fit_row k_fit[ODIN3_PROV_DEPTH_COUNT][METRIC_COUNT] = {{{0}}};

static uint64_t fit_eval(const fit_row *row, const odin3_prov_shape *shape) {
    double value =
        row->base + row->per_node * (double)shape->nodes + row->per_pin * (double)shape->pins;
    return value > 0.0 ? (uint64_t)value : 0;
}

void odin3_prov_depth_estimate(const odin3_prov_shape *shape, odin3_prov_estimate *out) {
    for (uint32_t d = 0; d < ODIN3_PROV_DEPTH_COUNT; d++) {
        out->records[d] = fit_eval(&k_fit[d][METRIC_RECORDS], shape);
        out->edges[d] = fit_eval(&k_fit[d][METRIC_EDGES], shape);
        out->map_bytes[d] = fit_eval(&k_fit[d][METRIC_MAP], shape);
        out->step_bytes[d] = fit_eval(&k_fit[d][METRIC_STEPS], shape);
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

static bool same_request(const odin3_prov_request *a, const odin3_prov_request *b) {
    return a->set == b->set && a->autolevel == b->autolevel && a->level == b->level &&
           a->budget == b->budget;
}

/* Highest depth up to want whose total fits the budget; 0 always fits (it writes nothing). */
static uint32_t fit_depth(const odin3_prov_estimate *est, uint32_t want, uint64_t budget) {
    uint32_t depth = want;
    while (depth > 0 && budget != 0 && odin3_prov_estimate_total(est, depth) > budget) {
        depth--;
    }
    return depth;
}

static int resolve(odin3_design *design, const odin3_prov_request *req) {
    uint32_t want = req->set && !req->autolevel ? req->level : DEPTH_AUTO_TOP;
    if (req->budget == 0) {
        if (req->autolevel) {
            odin3_log(ODIN3_LOG_WARN, "prov-level auto needs --prov-budget; using depth 4");
        }
        return (int)want;
    }
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    uint32_t depth = fit_depth(&est, want, req->budget);
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
    return (int)(req.set && !req.autolevel ? req.level : DEPTH_AUTO_TOP);
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
    odin3_log(ODIN3_LOG_INFO,
              "proc: provenance depth %d (%s): estimated %" PRIu64 " records, %" PRIu64
              " edges, %" PRIu64 " bytes (name map %" PRIu64 ", step files %" PRIu64
              ") from %" PRIu64 " cells, %" PRIu64 " pins",
              depth, k_depth_names[depth], est.records[depth], est.edges[depth],
              odin3_prov_estimate_total(&est, (uint32_t)depth), est.map_bytes[depth],
              est.step_bytes[depth], shape.nodes, shape.pins);
}

const char *odin3_prov_depth_name(uint32_t depth) {
    return k_depth_names[depth < ODIN3_PROV_DEPTH_COUNT ? depth : 0];
}
