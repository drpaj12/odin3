/*
 * generic.c — 4.2 generic hard blocks (spec B-1 to B-7): the bind check and report that run as
 * phase 0 of `partial_map`, before the `$mul` phase. A generic block is a model of the arch record
 * that `read_arch` registered as a black-box cell type (not one of the four templates). Binding
 * itself is done by elaboration, the BLIF reader and the tech library (B-1); this phase adds:
 *   B-5  an input port that nothing is connected to (every bit on the unknown constant or on no
 *        net) is tied to gnd with a WARNING;
 *   B-6  an instance of a black-box type that is not a model of the arch is a located error, or a
 *        WARNING with `--allow-unknown-blocks`; nothing is checked without an arch record;
 *   the INFO line `N instances of M generic blocks bound (pcie 2, ...)`, in the arch's model order.
 * The B-6 check runs over the whole design before anything is changed, so an error leaves the IR
 * untouched. B-3 and B-4 are the existing elaboration texts (pinned by tests, not repeated here).
 */
#include "frontends/arch/arch.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hm_kit.h"
#include "techlib/reader.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gen_state {
    hm_ctx *hc;
    const odin3_arch *arch;
    odin3_vec counts; /* uint32_t per arch model: the instances bound */
    uint64_t instances;
    uint32_t unknown; /* B-6 findings */
} gen_state;

/* The arch model a black-box type is, or NULL when the arch does not register one of its name. */
static const odin3_arch_model *model_of(const gen_state *gs, const odin3_celltype_def *def) {
    const odin3_arch_model *model = odin3_arch_model_find(gs->arch, def->name);
    return model != NULL && model->registered ? model : NULL;
}

/* B-6: the located error, or the warning with --allow-unknown-blocks. */
static void report_unknown(gen_state *gs, odin3_node_id node, const char *type) {
    char label[ODIN3_LOG_BUF / 4];
    hm_label(gs->hc, node, label, sizeof label);
    odin3_log_level level = gs->hc->allow_unknown_blocks ? ODIN3_LOG_WARN : ODIN3_LOG_ERROR;
    hm_diag(gs->hc, level, node,
            "partial_map: instance '%s' of '%s': '%s' is not a model of architecture '%s' (VPR "
            "would reject the netlist)",
            label, type, type, pm_arch_name(gs->arch));
    gs->unknown += gs->hc->allow_unknown_blocks ? 0U : 1U;
}

/* True when every pin of the port is on no net or on the unknown constant (elaboration's
 * "not connected"). */
static bool port_unconnected(const odin3_module *mod, odin3_pinslice pins) {
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k});
        if (odin3_net_valid(net) && odin3_net_const_value(mod, net) != ODIN3_CONST_X) {
            return false;
        }
    }
    return pins.count > 0;
}

/* B-5 for one port: the pins go to gnd. A pin that cannot be reconnected (out of memory) stays
 * unconnected, which is what the port was. */
static odin3_status tie_port(gen_state *gs, odin3_node_id node, const odin3_celltype_def *def,
                             uint32_t port_index) {
    hm_ctx *hc = gs->hc;
    odin3_net_id gnd = {0};
    odin3_status st = hm_const(hc, HM_GND, &gnd);
    odin3_pinslice pins = odin3_node_port(hc->module, node, port_index);
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        (void)odin3_pin_disconnect(hc->module, pin);
        st = odin3_pin_connect(hc->module, pin, gnd);
    }
    if (st == ODIN3_OK) {
        char label[ODIN3_LOG_BUF / 4];
        hm_label(hc, node, label, sizeof label);
        hm_diag(hc, ODIN3_LOG_WARN, node,
                "partial_map: instance '%s' of '%s': input port '%s' is not connected or is x; "
                "tied to gnd",
                label, def->name, def->ports[port_index].name);
    }
    return st;
}

/* B-5 for one instance: every input port that is not connected. A clock port is left alone: the
 * VTR writer's E3b refuses an unconnected or x clock, and a missing clock must not pass as gnd. */
static odin3_status tie_inputs(gen_state *gs, odin3_node_id node, const odin3_celltype_def *def,
                               const odin3_arch_model *model) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        odin3_pinslice pins = odin3_node_port(gs->hc->module, node, i);
        bool clock = i < model->n_ports && model->ports[i].is_clock;
        if (def->ports[i].dir == ODIN3_DIR_IN && !clock && port_unconnected(gs->hc->module, pins)) {
            st = tie_port(gs, node, def, i);
        }
    }
    return st;
}

/* The B-6 check of one node (nothing is changed). */
static void check_node(gen_state *gs, odin3_node_id node, const odin3_celltype_def *def,
                       odin3_celltype_id type) {
    /* A type with a library record (lib/lpm.o3lib, an arch model, a user .o3lib) is known to the
     * tool; only a black box with none is unknown (the IR does not tell an implicit black box
     * from a `(* blackbox *)` one: both have no record). */
    const odin3_techlib_cell *lib =
        def->gran == ODIN3_GRAN_BLACKBOX ? odin3_celltype_lib(gs->hc->design, type) : NULL;
    if (def->gran == ODIN3_GRAN_BLACKBOX && lib == NULL && model_of(gs, def) == NULL) {
        report_unknown(gs, node, def->name);
    }
}

/* The B-1 and B-5 step of one node: a generic block is counted and its inputs are tied. */
static odin3_status bind_node(gen_state *gs, odin3_node_id node, const odin3_celltype_def *def) {
    const odin3_arch_model *model = def->gran == ODIN3_GRAN_BLACKBOX ? model_of(gs, def) : NULL;
    if (model == NULL) {
        return ODIN3_OK;
    }
    uint32_t *count = odin3_vec_at(&gs->counts, (size_t)(model - gs->arch->models));
    (*count)++;
    gs->instances++;
    return tie_inputs(gs, node, def, model);
}

/* Runs the B-6 check over every live node of every module. */
static void check_all(gen_state *gs) {
    hm_ctx *hc = gs->hc;
    for (uint32_t mi = 1; mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = odin3_module_get(hc->design, (odin3_module_id){mi});
        if (mod == NULL) {
            continue;
        }
        hm_set_module(hc, mod);
        for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (odin3_node_live(mod, node)) {
                odin3_celltype_id type = odin3_node_type(mod, node);
                check_node(gs, node, odin3_celltype_get(hc->design, type), type);
            }
        }
    }
}

/* Binds every generic block of every module, in module and node-ID order. */
static odin3_status bind_all(gen_state *gs) {
    hm_ctx *hc = gs->hc;
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = odin3_module_get(hc->design, (odin3_module_id){mi});
        if (mod == NULL) {
            continue;
        }
        hm_set_module(hc, mod);
        for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (odin3_node_live(mod, node)) {
                st =
                    bind_node(gs, node, odin3_celltype_get(hc->design, odin3_node_type(mod, node)));
            }
        }
    }
    return st;
}

/* The INFO line: the models used, in the arch's order, with their instance counts. */
static void report_bound(gen_state *gs) {
    if (gs->instances == 0) {
        return;
    }
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    uint32_t used = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < gs->arch->n_models; i++) {
        const uint32_t *count = odin3_vec_at(&gs->counts, i);
        if (*count != 0) {
            st = odin3_strbuf_appendf(&list, "%s%s %" PRIu32, used != 0 ? ", " : "",
                                      gs->arch->models[i].name, *count);
            used++;
        }
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "partial_map: %" PRIu64 " instances of %" PRIu32 " generic blocks bound (%s)",
                  gs->instances, used, list.data);
    }
    odin3_strbuf_free(&list);
}

/* One zero count per arch model. */
static odin3_status counts_init(gen_state *gs) {
    odin3_vec_init(&gs->counts, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(&gs->counts, gs->arch->n_models);
    for (uint32_t i = 0; st == ODIN3_OK && i < gs->arch->n_models; i++) {
        uint32_t *slot = odin3_vec_push(&gs->counts); /* reserved: never NULL */
        *slot = 0;
    }
    return st;
}

odin3_status hm_generic_phase(hm_ctx *hc) {
    const odin3_arch *arch = odin3_design_arch(hc->design);
    if (arch == NULL) {
        return ODIN3_OK;
    }
    gen_state gs = {hc, arch, {0}, 0, 0};
    odin3_status st = counts_init(&gs);
    if (st == ODIN3_OK) {
        check_all(&gs);
        st = gs.unknown != 0 ? ODIN3_ERR_INVALID_ARG : bind_all(&gs);
    }
    if (st == ODIN3_OK) {
        report_bound(&gs);
    }
    odin3_vec_free(&gs.counts);
    return st;
}
