/*
 * timing.c — timing hints for the abc pass (timing.h).
 *
 * Units. ABC's mapper `if`, run without a LUT library, counts one unit per LUT level. The hint
 * file gives times in ps and `ns_per_level`, pass 1's routed critical path delay divided by its
 * mapped unit depth, so a time in ps becomes ps / 1000 / ns_per_level levels.
 *
 * A cell hint (the gate record of a word cell) says how much slack the routed pass 1 left on the
 * worst path through the cell, and the unit depth the cell had there. An output or register input
 * driven by a gate of such a cell may be as deep as that depth plus the slack the path had over
 * the worst path of the design: required = depth + (slack - slack_min) / ns_per_level. Every other
 * output keeps the depth budget of pass 1, `depth` (the default required time), which is what ABC's
 * own estimate gives a design with one region. A net hint gives an arrival (inputs) or required
 * time (outputs) directly.
 */
#include "passes/abc/timing.h"

#include "ir/prov.h"
#include "passes/abc/region.h"
#include "util/log.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { NET_TEXT_BYTES = 512, PS_PER_NS = 1000 };

uint32_t odin3_abc_timing_slot(const odin3_abc_script *script) {
    for (uint32_t i = 0; i < script->n_cmds; i++) {
        const char *line = odin3_abc_script_cmd(script, i)->line;
        if (strncmp(line, "if ", 3) == 0 || strcmp(line, "if") == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

void odin3_abc_timing_start(odin3_abc_run *run) {
    run->hints = NULL;
    run->timing_slot = UINT32_MAX;
    odin3_hints *hints = odin3_hints_of(run->design);
    if (hints == NULL) {
        return;
    }
    const char *why = NULL;
    if (run->gates_mode) {
        why = "--gates does not map to LUTs";
    } else if (run->exchange != ODIN3_ABC_EXCHANGE_BLIF) {
        why = "the AIG exchange has no net names (use the BLIF exchange)";
    } else if (run->route != ODIN3_ABC_ROUTE_LINKED) {
        why = "only the default script runs in the linked engine that has the o3_timing command";
    } else if (odin3_abc_timing_slot(&run->script) == UINT32_MAX) {
        why = "the script has no `if` command";
    }
    if (why != NULL) {
        odin3_log(ODIN3_LOG_WARN, "abc: timing hints are loaded (%s) but this run cannot use "
                                     "them: %s; they are ignored",
                  hints->label, why);
        return;
    }
    run->hints = hints;
    run->timing_slot = odin3_abc_timing_slot(&run->script);
}

/* --- one region ------------------------------------------------------------------------------- */

typedef struct tctx {
    odin3_abc_run *run;
    const odin3_module *module;
    odin3_hints *hints;
    FILE *fp;
    double npl;
} tctx;

static double levels(const tctx *tc, double ps) {
    return ps / PS_PER_NS / tc->npl;
}

static void net_key(const tctx *tc, odin3_net_id net, char *out, size_t cap) {
    char text[NET_TEXT_BYTES];
    odin3_abc_net_text(tc->module, net, text, sizeof text);
    const char *mod = odin3_strtab_get(odin3_design_strtab(tc->run->design),
                                       odin3_module_name(tc->module));
    (void)snprintf(out, cap, "%s/%s", mod, text);
}

/* The required time (levels) of a net a gate drives, from a net hint or the driver's cell hint;
 * false when there is none. */
static bool required_of(tctx *tc, odin3_net_id net, double *req) {
    if (!odin3_net_valid(net)) {
        return false;
    }
    char key[NET_TEXT_BYTES + NET_TEXT_BYTES];
    net_key(tc, net, key, sizeof key);
    const odin3_hint_net *nh = odin3_hints_find_net(tc->hints, key);
    if (nh != NULL && !isnan(nh->required_ps)) {
        *req = levels(tc, nh->required_ps);
        return true;
    }
    if (odin3_net_driver_count(tc->module, net) != 1) {
        return false;
    }
    odin3_node_id node = odin3_pin_node(tc->module, odin3_net_driver(tc->module, net));
    odin3_prov_id prov = odin3_node_prov(tc->module, node);
    const odin3_hint_cell *ch = prov.v != 0 ? odin3_hints_find_cell(tc->hints, prov.v) : NULL;
    if (ch == NULL) {
        return false;
    }
    *req = (double)ch->depth + levels(tc, ch->slack_ps - tc->hints->slack_min_ps);
    return true;
}

/* Marks every cell hint that matches a gate of the region (the match count of the report). */
static void mark_matches(tctx *tc) {
    const odin3_abc_region *rg = tc->run->region;
    const uint32_t *soft = rg->soft.data;
    for (size_t i = 0; i < rg->soft.len; i++) {
        odin3_prov_id prov = odin3_node_prov(tc->module, (odin3_node_id){soft[i]});
        if (prov.v != 0) {
            (void)odin3_hints_find_cell(tc->hints, prov.v);
        }
    }
}

static void put_outputs(tctx *tc) {
    const odin3_abc_region *rg = tc->run->region;
    const uint32_t *cos = rg->cos.data;
    for (size_t j = 0; j < rg->cos.len; j++) {
        double req = 0;
        if (required_of(tc, (odin3_net_id){cos[j]}, &req)) {
            (void)fprintf(tc->fp, "po o3co%zu %.6f\n", j, req);
            tc->run->stats.hint_req++;
        }
    }
    const uint32_t *dnet = rg->reg_d.data;
    for (size_t j = 0; j < rg->regs.len; j++) {
        double req = 0;
        if (required_of(tc, (odin3_net_id){dnet[j]}, &req)) {
            (void)fprintf(tc->fp, "li o3q%zu %.6f\n", j, req);
            tc->run->stats.hint_req++;
        }
    }
}

static void put_inputs(tctx *tc) {
    const odin3_abc_region *rg = tc->run->region;
    const uint32_t *cis = rg->cis.data;
    for (size_t i = 0; i < rg->cis.len; i++) {
        char key[NET_TEXT_BYTES + NET_TEXT_BYTES];
        net_key(tc, (odin3_net_id){cis[i]}, key, sizeof key);
        const odin3_hint_net *nh = odin3_hints_find_net(tc->hints, key);
        if (nh != NULL && !isnan(nh->arrival_ps)) {
            (void)fprintf(tc->fp, "pi o3ci%zu %.6f\n", i, levels(tc, nh->arrival_ps));
            tc->run->stats.hint_arr++;
        }
    }
}

odin3_status odin3_abc_timing_region(odin3_abc_run *run, const char *path) {
    run->timing_on = false;
    if (run->hints == NULL) {
        return ODIN3_OK;
    }
    tctx tc = {run, run->module, run->hints, NULL, run->hints->ns_per_level};
    mark_matches(&tc);
    tc.fp = fopen(path, "w");
    if (tc.fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot create the file: %s", path, strerror(errno));
        return ODIN3_ERR_IO;
    }
    (void)fprintf(tc.fp, "default_required %.6f\n", (double)run->hints->depth);
    put_inputs(&tc);
    put_outputs(&tc);
    int bad = fclose(tc.fp);
    if (bad != 0) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: write failed: %s", path, strerror(errno));
        return ODIN3_ERR_IO;
    }
    run->timing_on = run->stats.hint_req + run->stats.hint_arr != 0;
    run->hint_req_total += run->stats.hint_req;
    run->hint_arr_total += run->stats.hint_arr;
    return ODIN3_OK;
}

void odin3_abc_timing_report(const odin3_abc_run *run) {
    const odin3_hints *h = run->hints;
    if (h == NULL) {
        return;
    }
    size_t cells = odin3_hints_cells_used(h);
    size_t nets = odin3_hints_nets_used(h);
    odin3_log(ODIN3_LOG_INFO,
              "abc: timing hints (%s): %zu of %zu cell hints match a gate of the design, %zu of "
              "%zu net hints match a net; %" PRIu64 " required times and %" PRIu64
              " arrival times set",
              h->label, cells, h->n_cells, nets, h->n_nets, run->hint_req_total,
              run->hint_arr_total);
    if (cells < h->n_cells || nets < h->n_nets) {
        odin3_log(ODIN3_LOG_WARN,
                  "abc: timing hints: %zu cell hints and %zu net hints match nothing in the "
                  "design (hints must come from the same design, flow and provenance depth); "
                  "they are ignored",
                  h->n_cells - cells, h->n_nets - nets);
    }
}
