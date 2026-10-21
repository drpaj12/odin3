/*
 * sweep.c — the levelize sweep (Phase 6 spec 6.4, V-2 to V-4): Kahn's algorithm over the
 * combinational cells of one module, and the loop cut when it stalls.
 *
 * Roles. A node is a source (LV_SOURCE) when its cell type is sequential or holds state: the
 * registry's SEQ_EDGE or SEQ_LEVEL flag (celltype.h), or a tech-library cell that states a `seq`
 * or a `memory` (techlib/reader.h; the library data of a type is odin3_celltype_lib), or a black
 * box that is a VTR RAM primitive (single_port_ram, dual_port_ram) or the model of a
 * `class="memory"` leaf of the loaded arch record. Its inputs
 * end paths and its outputs start them at level 0 (Question 4, one rule for memories, registers,
 * latches and black boxes or hard blocks that state a latch or register). Every other cell is
 * combinational (LV_COMB), black boxes, hard blocks without state and instances of design modules
 * among them. Module port nodes are not cells. A pin of direction INOUT counts as a driver only.
 *
 * Loop cut (V-4, the abc region's rule): on a stall every unfinished cell lies on a cycle or
 * downstream of one. The lowest-ID cell that lies on a cycle of the unfinished cells is found with
 * Tarjan's strongly connected components (iterative), warned about once and cut: level 0, outputs
 * level 0, sweep continues. Each stall costs O(cells + pins), and one Tarjan pass cuts the
 * lowest-ID cell of EVERY cyclic component it finds (in ascending ID order), so K independent loops
 * cost one pass, not K.
 */
#include "frontends/arch/arch.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/levelize/levelize_int.h"
#include "techlib/reader.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Tarjan: the node is on the component stack. */
enum { LV_ONSTK = 8, NAME_BUF = 32 };

/* One Tarjan DFS frame: a node and the cursor over its output pins and their sinks. */
typedef struct tj_frame {
    uint32_t node;
    uint32_t pin;
    uint32_t sink;
} tj_frame;

static void *zeroed(size_t count, size_t size) {
    return odin3_util_calloc((count + 1) * size);
}

odin3_status lv_open(lv_mod *lv, const odin3_design *design, odin3_module *module) {
    *lv = (lv_mod){.design = design, .module = module};
    odin3_vec_init(&lv->frames, sizeof(tj_frame));
    odin3_vec_init(&lv->members, sizeof(uint32_t));
    lv->n_nodes = odin3_module_node_end(module);
    lv->n_nets = odin3_module_net_end(module);
    lv->role = zeroed(lv->n_nodes, sizeof *lv->role);
    lv->state = zeroed(lv->n_nodes, sizeof *lv->state);
    lv->indeg = zeroed(lv->n_nodes, sizeof *lv->indeg);
    lv->clevel = zeroed(lv->n_nodes, sizeof *lv->clevel);
    lv->stack = zeroed(lv->n_nodes, sizeof *lv->stack);
    lv->nlevel = zeroed(lv->n_nets, sizeof *lv->nlevel);
    lv->ndrv = zeroed(lv->n_nets, sizeof *lv->ndrv);
    bool ok = lv->role != NULL && lv->state != NULL && lv->indeg != NULL && lv->clevel != NULL &&
              lv->stack != NULL && lv->nlevel != NULL && lv->ndrv != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

void lv_free(lv_mod *lv) {
    odin3_util_free(lv->role);
    odin3_util_free(lv->state);
    odin3_util_free(lv->indeg);
    odin3_util_free(lv->clevel);
    odin3_util_free(lv->stack);
    odin3_util_free(lv->nlevel);
    odin3_util_free(lv->ndrv);
    odin3_util_free(lv->tj_index);
    odin3_util_free(lv->tj_low);
    odin3_vec_free(&lv->frames);
    odin3_vec_free(&lv->members);
    *lv = (lv_mod){0};
}

/* --- classification ------------------------------------------------------------------------- */

/* A black box with no library statement of its own is still a memory when it is a VTR RAM
 * primitive or the loaded arch record gives it to a `class="memory"` leaf (agent default, Peter's
 * rule: a memory's inputs end paths, its outputs start them). */
static bool is_memory_model(const odin3_design *design, const char *name) {
    if (strcmp(name, "single_port_ram") == 0 || strcmp(name, "dual_port_ram") == 0) {
        return true;
    }
    const odin3_arch *arch = odin3_design_arch(design);
    uint32_t leaf = 0;
    for (bool more = arch != NULL && odin3_arch_next_leaf_of(arch, name, 0, &leaf); more;
         more = odin3_arch_next_leaf_of(arch, name, leaf + 1, &leaf)) {
        const char *cls = odin3_arch_leaf_at(arch, leaf)->cls;
        if (cls != NULL && strcmp(cls, "memory") == 0) {
            return true;
        }
    }
    return false;
}

static uint8_t type_role(const odin3_design *design, odin3_celltype_id type, bool *is_inst) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    *is_inst = false;
    if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
        return LV_SKIP;
    }
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        return LV_SOURCE;
    }
    const odin3_techlib_cell *lib = odin3_celltype_lib(design, type);
    if (lib != NULL && (lib->memory != NULL || lib->n_seqs > 0)) {
        return LV_SOURCE;
    }
    if (def->gran == ODIN3_GRAN_BLACKBOX && is_memory_model(design, def->name)) {
        return LV_SOURCE;
    }
    *is_inst = def->gran == ODIN3_GRAN_MODULE;
    return LV_COMB;
}

static void classify_nodes(lv_mod *lv) {
    for (uint32_t id = 1; id < lv->n_nodes; id++) {
        odin3_node_id node = {id};
        bool is_inst = false;
        if (odin3_node_live(lv->module, node)) {
            lv->role[id] = type_role(lv->design, odin3_node_type(lv->module, node), &is_inst);
        }
        lv->n_comb += lv->role[id] == LV_COMB ? 1U : 0U;
        lv->n_cells += lv->role[id] != LV_SKIP ? 1U : 0U;
        lv->n_inst += is_inst ? 1U : 0U;
    }
}

/* Counts, per net, the combinational driver pins. */
static void count_drivers(lv_mod *lv) {
    for (uint32_t id = 1; id < lv->n_nodes; id++) {
        if (lv->role[id] != LV_COMB) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(lv->module, (odin3_node_id){id});
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            odin3_net_id net = odin3_pin_net(lv->module, pin);
            if (odin3_net_valid(net) && odin3_pin_drives(lv->module, pin)) {
                lv->ndrv[net.v]++;
            }
        }
    }
}

/* indeg of every comb node: per input pin, the comb driver pins of its net; ready nodes pushed. */
static void count_inputs(lv_mod *lv) {
    for (uint32_t id = 1; id < lv->n_nodes; id++) {
        if (lv->role[id] != LV_COMB) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(lv->module, (odin3_node_id){id});
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            odin3_net_id net = odin3_pin_net(lv->module, pin);
            if (odin3_net_valid(net) && odin3_pin_dir(lv->module, pin) == ODIN3_DIR_IN) {
                lv->indeg[id] += lv->ndrv[net.v];
            }
        }
        if (lv->indeg[id] == 0) {
            lv->stack[lv->n_stack++] = id;
        }
    }
}

/* --- the sweep ------------------------------------------------------------------------------ */

/* 1 + the largest input net level; 0 for a cell with no input pin (V-2). */
static uint32_t cell_level(const lv_mod *lv, uint32_t id) {
    odin3_pinslice pins = odin3_node_pins(lv->module, (odin3_node_id){id});
    uint32_t top = 0;
    bool has_input = false;
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_dir(lv->module, pin) != ODIN3_DIR_IN) {
            continue;
        }
        odin3_net_id net = odin3_pin_net(lv->module, pin);
        has_input = true;
        top = odin3_net_valid(net) && lv->nlevel[net.v] > top ? lv->nlevel[net.v] : top;
    }
    return has_input ? top + 1 : 0;
}

/* Gives the output net the cell's level and releases the readers that wait for it. */
static void release_net(lv_mod *lv, odin3_net_id net, uint32_t level) {
    lv->nlevel[net.v] = lv->nlevel[net.v] > level ? lv->nlevel[net.v] : level;
    lv->max_level = lv->nlevel[net.v] > lv->max_level ? lv->nlevel[net.v] : lv->max_level;
    odin3_pinlist sinks = odin3_net_sinks(lv->module, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        uint32_t reader = odin3_pin_node(lv->module, sinks.pins[i]).v;
        if (lv->role[reader] != LV_COMB || (lv->state[reader] & LV_DONE) != 0 ||
            lv->indeg[reader] == 0) {
            continue;
        }
        lv->indeg[reader]--;
        if (lv->indeg[reader] == 0) {
            lv->stack[lv->n_stack++] = reader;
        }
    }
}

/* Finishes a comb cell: its level (0 when cut), its outputs, its readers. */
static void finish(lv_mod *lv, uint32_t id, bool cut) {
    uint32_t level = cut ? 0 : cell_level(lv, id);
    lv->clevel[id] = level;
    lv->state[id] |= (uint8_t)(LV_DONE | (cut ? LV_CUT : 0));
    lv->n_done++;
    lv->max_level = level > lv->max_level ? level : lv->max_level;
    odin3_pinslice pins = odin3_node_pins(lv->module, (odin3_node_id){id});
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(lv->module, pin);
        if (odin3_net_valid(net) && odin3_pin_drives(lv->module, pin)) {
            release_net(lv, net, level);
        }
    }
}

/* --- loops ---------------------------------------------------------------------------------- */

/* The next unfinished comb reader of the frame's node's outputs; 0 when there is none. */
static uint32_t next_succ(const lv_mod *lv, tj_frame *frame) {
    odin3_pinslice pins = odin3_node_pins(lv->module, (odin3_node_id){frame->node});
    while (frame->pin < pins.count) {
        odin3_pin_id pin = {pins.first.v + frame->pin};
        odin3_net_id net = odin3_pin_net(lv->module, pin);
        if (odin3_net_valid(net) && odin3_pin_drives(lv->module, pin)) {
            odin3_pinlist sinks = odin3_net_sinks(lv->module, net);
            while (frame->sink < sinks.count) {
                uint32_t reader = odin3_pin_node(lv->module, sinks.pins[frame->sink++]).v;
                if (lv->role[reader] == LV_COMB && (lv->state[reader] & LV_DONE) == 0) {
                    return reader;
                }
            }
        }
        frame->pin++;
        frame->sink = 0;
    }
    return 0;
}

static odin3_status tj_enter(lv_mod *lv, uint32_t id) {
    tj_frame *frame = odin3_vec_push(&lv->frames);
    uint32_t *member = frame != NULL ? odin3_vec_push(&lv->members) : NULL;
    if (frame == NULL || member == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    frame->node = id;
    *member = id;
    lv->tj_counter += 1;
    lv->tj_index[id] = lv->tj_counter;
    lv->tj_low[id] = lv->tj_counter;
    lv->state[id] |= LV_ONSTK;
    return ODIN3_OK;
}

/* Closes the component rooted at id: its lowest ID is to be cut when it is a cycle. */
static void tj_close(lv_mod *lv, uint32_t id) {
    uint32_t lowest = UINT32_MAX;
    size_t size = 0;
    bool self = false;
    for (uint32_t at = 0; at != id;) {
        at = *(const uint32_t *)odin3_vec_cat(&lv->members, lv->members.len - 1);
        odin3_vec_pop(&lv->members);
        lv->state[at] &= (uint8_t)~LV_ONSTK;
        lowest = at < lowest ? at : lowest;
        self = self || (lv->state[at] & LV_SELF) != 0;
        size++;
    }
    if (size > 1 || self) {
        lv->state[lowest] |= LV_CAND;
        lv->tj_cands++;
    }
}

/* Pops the finished frame of id, closes its component when it is a root, informs the parent. */
static void tj_leave(lv_mod *lv, uint32_t id) {
    odin3_vec_pop(&lv->frames);
    if (lv->tj_low[id] == lv->tj_index[id]) {
        tj_close(lv, id);
    }
    if (lv->frames.len > 0) {
        const tj_frame *parent = odin3_vec_cat(&lv->frames, lv->frames.len - 1);
        lv->tj_low[parent->node] =
            lv->tj_low[id] < lv->tj_low[parent->node] ? lv->tj_low[id] : lv->tj_low[parent->node];
    }
}

static odin3_status tj_step(lv_mod *lv) {
    tj_frame *frame = odin3_vec_at(&lv->frames, lv->frames.len - 1);
    uint32_t id = frame->node;
    uint32_t succ = next_succ(lv, frame);
    if (succ == 0) {
        tj_leave(lv, id);
        return ODIN3_OK;
    }
    if (succ == id) {
        lv->state[id] |= LV_SELF;
    }
    if (lv->tj_index[succ] == 0) {
        return tj_enter(lv, succ);
    }
    if ((lv->state[succ] & LV_ONSTK) != 0 && lv->tj_index[succ] < lv->tj_low[id]) {
        lv->tj_low[id] = lv->tj_index[succ];
    }
    return ODIN3_OK;
}

/* Tarjan over the unfinished comb cells; the lowest cell of every cyclic component gets LV_CAND. */
static odin3_status find_cycle_cells(lv_mod *lv) {
    lv->tj_counter = 0;
    lv->tj_cands = 0;
    if (lv->tj_index == NULL) {
        lv->tj_index = zeroed(lv->n_nodes, sizeof *lv->tj_index);
        lv->tj_low = zeroed(lv->n_nodes, sizeof *lv->tj_low);
        if (lv->tj_index == NULL || lv->tj_low == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    memset(lv->tj_index, 0, ((size_t)lv->n_nodes + 1) * sizeof *lv->tj_index);
    for (uint32_t id = 1; id < lv->n_nodes; id++) {
        if (lv->role[id] != LV_COMB || (lv->state[id] & LV_DONE) != 0 || lv->tj_index[id] != 0) {
            continue;
        }
        odin3_status st = tj_enter(lv, id);
        while (st == ODIN3_OK && lv->frames.len > 0) {
            st = tj_step(lv);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The first provenance leaf with a location. */
typedef struct lv_loc {
    const odin3_design *design;
    bool have;
    odin3_srcloc loc;
} lv_loc;

static void loc_visit(void *user, odin3_prov_id leaf) {
    lv_loc *found = user;
    const odin3_prov_record *rec = found->have ? NULL : odin3_prov_get(found->design, leaf);
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        found->loc = rec->locs[0];
        found->have = true;
    }
}

static void warn_loop(const lv_mod *lv, uint32_t id) {
    odin3_node_id node = {id};
    const odin3_strtab *tab = odin3_design_strtab(lv->design);
    const odin3_celltype_def *def =
        odin3_celltype_get(lv->design, odin3_node_type(lv->module, node));
    uint32_t name = odin3_node_name(lv->module, node);
    char fallback[NAME_BUF];
    (void)snprintf(fallback, sizeof fallback, "$c%u", (unsigned)id);
    const char *cell = name != 0 ? odin3_strtab_get(tab, name) : fallback;
    const char *module = odin3_strtab_get(tab, odin3_module_name(lv->module));
    odin3_prov_id prov = odin3_node_prov(lv->module, node);
    lv_loc found = {lv->design, false, {0}};
    if (prov.v != 0) {
        (void)odin3_prov_sources(lv->design, prov, loc_visit, &found);
    }
    if (found.have) {
        odin3_log(ODIN3_LOG_WARN,
                  "levelize: module '%s': combinational loop through %s '%s' (%s:%u)", module,
                  def->name, cell, odin3_strtab_get(tab, found.loc.file), (unsigned)found.loc.line);
    } else {
        odin3_log(ODIN3_LOG_WARN, "levelize: module '%s': combinational loop through %s '%s'",
                  module, def->name, cell);
    }
}

/* A stall: cuts the lowest-ID cell of every cyclic component, in ascending ID order (the lowest
 * unfinished cell if none is found). */
static odin3_status cut_loops(lv_mod *lv) {
    odin3_status st = find_cycle_cells(lv);
    for (uint32_t id = 1; st == ODIN3_OK && id < lv->n_nodes; id++) {
        bool pick = (lv->state[id] & LV_CAND) != 0;
        pick = pick ||
               (lv->tj_cands == 0 && lv->role[id] == LV_COMB && (lv->state[id] & LV_DONE) == 0);
        if (pick) {
            lv->state[id] &= (uint8_t)~LV_CAND;
            lv->tj_cands = 1; /* the fallback cuts the first unfinished cell only */
            warn_loop(lv, id);
            lv->n_cut++;
            finish(lv, id, true);
        }
    }
    return st;
}

odin3_status lv_sweep(lv_mod *lv) {
    classify_nodes(lv);
    count_drivers(lv);
    count_inputs(lv);
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && lv->n_done < lv->n_comb) {
        if (lv->n_stack > 0) {
            lv->n_stack--;
            finish(lv, lv->stack[lv->n_stack], false);
        } else {
            st = cut_loops(lv);
        }
    }
    return st;
}
