/*
 * gv_ddg.c — write_graph ddg (spec section 5): the signal-level data dependency graph of each
 * module. Nodes are wires (buses whole) and nets outside any wire; an edge a -> b means a is read
 * to compute b, kind data (an operand) or control (a condition, selector, clock or enable).
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "passes/graphviews/gv.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_DATA, KIND_CONTROL, KEY_WIRE = 1, KEY_SHIFT = 32, TEMP_NAME_MAX = 24 };
#define KEY_LOW_MASK 0xFFFFFFFFU

/* A signal key: wire ID with KEY_WIRE in the top half, or a bare net ID. 0 is "no signal". */
typedef uint64_t sigkey;

typedef struct ddg_edge {
    sigkey from, to;
    uint32_t via, prov;
    uint8_t kind, seq;
} ddg_edge;

typedef struct ddg_ctx {
    gv_run *run;
    const odin3_module *mod;
    odin3_vec edges; /* ddg_edge */
} ddg_ctx;

/* The signal of a net; 0 for no net or a constant. */
static sigkey key_of(const ddg_ctx *cx, odin3_net_id net) {
    if (!odin3_net_valid(net) || odin3_net_const_value(cx->mod, net) != ODIN3_CONST_NONE) {
        return 0;
    }
    odin3_wirebit prim = odin3_net_primary(cx->mod, net);
    if (prim.wire.v != 0) {
        return ((sigkey)KEY_WIRE << KEY_SHIFT) | prim.wire.v;
    }
    return net.v;
}

static odin3_status add_edge(ddg_ctx *cx, const ddg_edge *edge) {
    if (edge->from == 0 || edge->to == 0) {
        return ODIN3_OK;
    }
    ddg_edge *slot = odin3_vec_push(&cx->edges);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *edge;
    return ODIN3_OK;
}

/* A small set of keys with their kind (insertion order, linear dedupe: ports are narrow in keys).
 */
typedef struct keyset {
    odin3_vec items; /* uint64: key << 1 | kind is not enough for wires, so two arrays */
    odin3_vec kinds;
} keyset;

static void keyset_init(keyset *set) {
    odin3_vec_init(&set->items, sizeof(sigkey));
    odin3_vec_init(&set->kinds, sizeof(uint8_t));
}

static void keyset_free(keyset *set) {
    odin3_vec_free(&set->items);
    odin3_vec_free(&set->kinds);
}

static odin3_status keyset_add(keyset *set, sigkey key, uint8_t kind) {
    if (key == 0) {
        return ODIN3_OK;
    }
    const sigkey *items = set->items.data;
    const uint8_t *kinds = set->kinds.data;
    for (size_t i = 0; i < set->items.len; i++) {
        if (items[i] == key && kinds[i] == kind) {
            return ODIN3_OK;
        }
    }
    sigkey *slot = odin3_vec_push(&set->items);
    uint8_t *kslot = slot != NULL ? odin3_vec_push(&set->kinds) : NULL;
    if (kslot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = key;
    *kslot = kind;
    return ODIN3_OK;
}

static bool is_control_port(const char *name) {
    static const char *const k_names[] = {"S",    "CLK",  "C",     "EN",  "E",
                                          "ARST", "SRST", "ALOAD", "SET", "CLR"};
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strcmp(name, k_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The cross product in x out of one cell, as edges. */
static odin3_status cross(ddg_ctx *cx, const keyset *ins, const keyset *outs, uint32_t via,
                          uint8_t seq) {
    const sigkey *ik = ins->items.data;
    const uint8_t *ikind = ins->kinds.data;
    const sigkey *ok = outs->items.data;
    odin3_status st = ODIN3_OK;
    odin3_prov_id prov = odin3_node_prov(cx->mod, (odin3_node_id){via});
    for (size_t i = 0; st == ODIN3_OK && i < ins->items.len; i++) {
        for (size_t j = 0; st == ODIN3_OK && j < outs->items.len; j++) {
            ddg_edge edge = {ik[i], ok[j], via, prov.v, ikind[i], seq};
            st = add_edge(cx, &edge);
        }
    }
    return st;
}

/* A flip-flop or latch: the flags, or the word-level families by name ($dff, $adff, ...). */
static bool is_storage(const odin3_celltype_def *def) {
    static const char *const k_prefixes[] = {"$dff",    "$adff",    "$sdff", "$aldff",
                                             "$dlatch", "$adlatch", "$sr"};
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
        if (strncmp(def->name, k_prefixes[i], strlen(k_prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static odin3_status normal_cell(ddg_ctx *cx, odin3_node_id node, const odin3_celltype_def *def) {
    keyset ins;
    keyset outs;
    keyset_init(&ins);
    keyset_init(&outs);
    odin3_pinslice pins = odin3_node_pins(cx->mod, node);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        sigkey key = key_of(cx, odin3_pin_net(cx->mod, pin));
        if (odin3_pin_reads(cx->mod, pin)) {
            uint32_t port = odin3_pin_port(cx->mod, pin);
            bool ctl = port < def->n_ports && is_control_port(def->ports[port].name);
            st = keyset_add(&ins, key, ctl ? KIND_CONTROL : KIND_DATA);
        }
        if (st == ODIN3_OK && odin3_pin_drives(cx->mod, pin)) {
            st = keyset_add(&outs, key, KIND_DATA);
        }
    }
    uint8_t seq = is_storage(def) ? 1 : 0;
    st = st == ODIN3_OK ? cross(cx, &ins, &outs, node.v, seq) : st;
    keyset_free(&ins);
    keyset_free(&outs);
    return st;
}

/* --- processes --------------------------------------------------------------------------- */

typedef struct pctx {
    ddg_ctx *cx;
    odin3_node_id node;
    odin3_proc_body body;
    odin3_pinslice in, outs;
} pctx;

static sigkey pos_key(const pctx *pc, odin3_pbit pbit) {
    uint32_t tag = pbit & ~ODIN3_PBIT_INDEX_MASK;
    uint32_t idx = pbit & ODIN3_PBIT_INDEX_MASK;
    if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT) {
        return 0;
    }
    odin3_pinslice port = tag == ODIN3_PBIT_IN ? pc->in : pc->outs;
    return key_of(pc->cx, odin3_pin_net(pc->cx->mod, (odin3_pin_id){port.first.v + idx}));
}

static odin3_status span_keys(const pctx *pc, odin3_pspan span, uint8_t kind, keyset *set) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < span.len; i++) {
        st = keyset_add(set, pos_key(pc, pc->body.pbits[span.first + i]), kind);
    }
    return st;
}

/* Edges of one action: rhs data, the enclosing switches' selectors as control. */
static odin3_status action_edges(pctx *pc, const odin3_proc_action *act, const keyset *sels,
                                 uint8_t seq) {
    keyset ins;
    keyset outs;
    keyset_init(&ins);
    keyset_init(&outs);
    odin3_status st = span_keys(pc, act->rhs, KIND_DATA, &ins);
    st = st == ODIN3_OK ? span_keys(pc, act->lhs, KIND_DATA, &outs) : st;
    const sigkey *sk = sels->items.data;
    for (size_t i = 0; st == ODIN3_OK && i < sels->items.len; i++) {
        st = keyset_add(&ins, sk[i], KIND_CONTROL);
    }
    const sigkey *ik = ins.items.data;
    const uint8_t *ikind = ins.kinds.data;
    const sigkey *ok = outs.items.data;
    for (size_t i = 0; st == ODIN3_OK && i < ins.items.len; i++) {
        for (size_t j = 0; st == ODIN3_OK && j < outs.items.len; j++) {
            ddg_edge edge = {ik[i], ok[j], pc->node.v, act->prov.v, ikind[i], seq};
            st = add_edge(pc->cx, &edge);
        }
    }
    keyset_free(&ins);
    keyset_free(&outs);
    return st;
}

typedef struct pframe {
    bool is_switch;
    uint32_t idx, next;
    bool started;
} pframe;

/* The selectors of the switch frames on the stack, as one set. */
static odin3_status selectors(const pctx *pc, const pframe *stack, size_t depth, keyset *sels) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < depth; i++) {
        if (stack[i].is_switch) {
            st = span_keys(pc, pc->body.switches[stack[i].idx].signal, KIND_CONTROL, sels);
        }
    }
    return st;
}

static odin3_status case_actions(pctx *pc, const pframe *stack, size_t depth) {
    const odin3_proc_case *cs = &pc->body.cases[stack[depth - 1].idx];
    keyset sels;
    keyset_init(&sels);
    odin3_status st = selectors(pc, stack, depth, &sels);
    for (uint32_t i = cs->first_action; st == ODIN3_OK && i < cs->first_action + cs->n_actions;
         i++) {
        st = action_edges(pc, &pc->body.actions[i], &sels, 0);
    }
    keyset_free(&sels);
    return st;
}

static odin3_status walk_tree(pctx *pc) {
    size_t cap = 2 * (size_t)pc->body.n_switches + 2;
    pframe *stack = odin3_util_malloc(cap * sizeof *stack);
    if (stack == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    stack[0] = (pframe){false, 0, pc->body.cases[0].first_switch, false};
    size_t depth = 1;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && depth > 0) {
        pframe *top = &stack[depth - 1];
        if (!top->is_switch) {
            const odin3_proc_case *cs = &pc->body.cases[top->idx];
            if (!top->started) {
                top->started = true;
                st = case_actions(pc, stack, depth);
            } else if (top->next < cs->first_switch + cs->n_switches) {
                uint32_t sw = top->next++;
                stack[depth++] = (pframe){true, sw, pc->body.switches[sw].first_case, true};
            } else {
                depth--;
            }
        } else if (top->next <
                   pc->body.switches[top->idx].first_case + pc->body.switches[top->idx].n_cases) {
            uint32_t cidx = top->next++;
            stack[depth++] = (pframe){false, cidx, pc->body.cases[cidx].first_switch, false};
        } else {
            depth--;
        }
    }
    odin3_util_free(stack);
    return st;
}

static odin3_status sync_edges(pctx *pc) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pc->body.n_syncs; k++) {
        const odin3_proc_sync *sy = &pc->body.syncs[k];
        keyset sels;
        keyset_init(&sels);
        uint8_t seq = sy->type != ODIN3_SYNC_ALWAYS ? 1 : 0;
        if (sy->type != ODIN3_SYNC_ALWAYS) {
            st = keyset_add(&sels, pos_key(pc, sy->signal), KIND_CONTROL);
        }
        for (uint32_t i = sy->first_action; st == ODIN3_OK && i < sy->first_action + sy->n_actions;
             i++) {
            st = action_edges(pc, &pc->body.actions[i], &sels, seq);
        }
        keyset_free(&sels);
    }
    return st;
}

static odin3_status process_cell(ddg_ctx *cx, odin3_node_id node) {
    pctx pc;
    memset(&pc, 0, sizeof pc);
    pc.cx = cx;
    pc.node = node;
    if (!odin3_proc_body_view(odin3_node_param(cx->mod, node, ODIN3_PROC_P_BODY), &pc.body) ||
        pc.body.n_cases == 0) {
        return ODIN3_OK;
    }
    pc.in = odin3_node_port(cx->mod, node, ODIN3_PROC_PORT_I);
    pc.outs = odin3_node_port(cx->mod, node, ODIN3_PROC_PORT_O);
    odin3_status st = walk_tree(&pc);
    return st == ODIN3_OK ? sync_edges(&pc) : st;
}

/* --- output ------------------------------------------------------------------------------ */

static int edge_cmp(const void *pa, const void *pb) {
    const ddg_edge *ea = pa;
    const ddg_edge *eb = pb;
    if (ea->from != eb->from) {
        return ea->from < eb->from ? -1 : 1;
    }
    if (ea->to != eb->to) {
        return ea->to < eb->to ? -1 : 1;
    }
    if (ea->kind != eb->kind) {
        return ea->kind < eb->kind ? -1 : 1;
    }
    if (ea->via != eb->via) {
        return ea->via < eb->via ? -1 : 1;
    }
    if (ea->prov != eb->prov) {
        return ea->prov < eb->prov ? -1 : 1;
    }
    return 0;
}

static size_t unique_edges(ddg_edge *edges, size_t count) {
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        bool same = out > 0 && edges[out - 1].from == edges[i].from &&
                    edges[out - 1].to == edges[i].to && edges[out - 1].kind == edges[i].kind &&
                    edges[out - 1].via == edges[i].via;
        if (!same) {
            edges[out++] = edges[i];
        }
    }
    return out;
}

static void w_key(gv_w *wr, sigkey key) {
    if ((key >> KEY_SHIFT) == KEY_WIRE) {
        gv_w_fmt(wr, "\"w%u\"", (unsigned)(key & KEY_LOW_MASK));
    } else {
        gv_w_fmt(wr, "\"n%u\"", (unsigned)key);
    }
}

static const char *port_dir(const ddg_ctx *cx, odin3_wire_id wire) {
    odin3_node_id port = odin3_wire_port_node(cx->mod, wire);
    const odin3_celltype_def *def =
        port.v != 0 ? odin3_celltype_get(cx->run->design, odin3_node_type(cx->mod, port)) : NULL;
    if (def == NULL) {
        return "";
    }
    if (strcmp(def->name, "$port_in") == 0) {
        return "in";
    }
    return strcmp(def->name, "$port_out") == 0 ? "out" : "inout";
}

static odin3_status put_wire_node(ddg_ctx *cx, odin3_strbuf *out, odin3_wire_id wire, bool *first) {
    gv_w wr = {cx->run, out, gv_sep(out, first)};
    gv_w_fmt(&wr, "{\"id\": \"w%u\", \"kind\": \"signal\", \"wire\": %u, \"label\": ", wire.v,
             wire.v);
    gv_w_str(&wr, gv_str(cx->run, odin3_wire_name(cx->mod, wire)));
    gv_w_fmt(&wr, ", \"width\": %u, \"port\": \"%s\", ", odin3_wire_width(cx->mod, wire),
             port_dir(cx, wire));
    gv_w_prov(&wr, odin3_wire_prov(cx->mod, wire));
    gv_w_raw(&wr, "}");
    return wr.st;
}

static odin3_status put_temp_node(ddg_ctx *cx, odin3_strbuf *out, uint32_t net, bool *first) {
    gv_w wr = {cx->run, out, gv_sep(out, first)};
    uint32_t name = odin3_net_name(cx->mod, (odin3_net_id){net});
    char fallback[TEMP_NAME_MAX];
    (void)snprintf(fallback, sizeof fallback, "$n%u", net);
    gv_w_fmt(&wr, "{\"id\": \"n%u\", \"kind\": \"temp\", \"net\": %u, \"label\": ", net, net);
    gv_w_str(&wr, name != 0 ? gv_str(cx->run, name) : fallback);
    gv_w_raw(&wr, ", ");
    gv_w_prov(&wr, odin3_net_prov(cx->mod, (odin3_net_id){net}));
    gv_w_raw(&wr, "}");
    return wr.st;
}

static int key_cmp(const void *pa, const void *pb) {
    sigkey ka = *(const sigkey *)pa;
    sigkey kb = *(const sigkey *)pb;
    if (ka != kb) {
        return ka < kb ? -1 : 1;
    }
    return 0;
}

/* The net-only keys the edges touch, sorted and unique (nets outside any wire). */
static odin3_status temp_keys(const ddg_edge *edges, size_t count, odin3_vec *temps) {
    for (size_t i = 0; i < count; i++) {
        sigkey ends[2] = {edges[i].from, edges[i].to};
        for (int k = 0; k < 2; k++) {
            sigkey *slot = (ends[k] >> KEY_SHIFT) == 0 ? odin3_vec_push(temps) : NULL;
            if (slot != NULL) {
                *slot = ends[k];
            } else if ((ends[k] >> KEY_SHIFT) == 0) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
    }
    if (temps->len > 1) {
        qsort(temps->data, temps->len, sizeof(sigkey), key_cmp);
    }
    return ODIN3_OK;
}

static odin3_status put_nodes(ddg_ctx *cx, odin3_strbuf *out, const ddg_edge *edges, size_t count) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(", \"nodes\": ["));
    bool first = true;
    for (uint32_t wid = 1; st == ODIN3_OK && wid < odin3_module_wire_end(cx->mod); wid++) {
        if (odin3_wire_live(cx->mod, (odin3_wire_id){wid})) {
            st = put_wire_node(cx, out, (odin3_wire_id){wid}, &first);
        }
    }
    odin3_vec temps;
    odin3_vec_init(&temps, sizeof(sigkey));
    st = st == ODIN3_OK ? temp_keys(edges, count, &temps) : st;
    const sigkey *tk = temps.data;
    for (size_t i = 0; st == ODIN3_OK && i < temps.len; i++) {
        if (i == 0 || tk[i] != tk[i - 1]) {
            st = put_temp_node(cx, out, (uint32_t)tk[i], &first);
        }
    }
    odin3_vec_free(&temps);
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

static odin3_status put_edge(ddg_ctx *cx, odin3_strbuf *out, const ddg_edge *edge, size_t idx) {
    odin3_node_id via = {edge->via};
    const odin3_celltype_def *def =
        odin3_celltype_get(cx->run->design, odin3_node_type(cx->mod, via));
    bool ctl = edge->kind == KIND_CONTROL;
    gv_w wr = {cx->run, out, ODIN3_OK};
    gv_w_fmt(&wr, "%s{\"id\": \"e%zu\", \"from\": ", idx == 0 ? "" : ", ", idx);
    w_key(&wr, edge->from);
    gv_w_raw(&wr, ", \"to\": ");
    w_key(&wr, edge->to);
    gv_w_fmt(&wr, ", \"kind\": \"%s\", \"label\": \"%s\", \"via\": %u, \"op\": ",
             ctl ? "control" : "data", ctl ? "control" : "", edge->via);
    gv_w_str(&wr, def != NULL ? def->name : "");
    gv_w_fmt(&wr, ", \"seq\": %s, ", edge->seq != 0 ? "true" : "false");
    gv_w_prov(&wr, (odin3_prov_id){edge->prov});
    gv_w_raw(&wr, "}");
    return wr.st;
}

static odin3_status put_graph(ddg_ctx *cx, odin3_strbuf *out) {
    ddg_edge *edges = cx->edges.data;
    size_t count = cx->edges.len;
    if (count > 1) {
        qsort(edges, count, sizeof *edges, edge_cmp);
    }
    count = unique_edges(edges, count);
    odin3_status st = odin3_strbuf_append(
        out, odin3_bytes_cstr(
                 "{\"id\": \"ddg\", \"kind\": \"ddg\", \"label\": \"signal data dependencies\""));
    st = st == ODIN3_OK ? put_nodes(cx, out, edges, count) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(", \"edges\": [")) : st;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = put_edge(cx, out, &edges[i], i);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]}")) : st;
}

static odin3_status module_edges(ddg_ctx *cx) {
    odin3_status st = ODIN3_OK;
    for (uint32_t cell = 1; st == ODIN3_OK && cell < odin3_module_node_end(cx->mod); cell++) {
        odin3_node_id node = {cell};
        const odin3_celltype_def *def =
            odin3_node_live(cx->mod, node)
                ? odin3_celltype_get(cx->run->design, odin3_node_type(cx->mod, node))
                : NULL;
        if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
            continue;
        }
        st = (def->flags & ODIN3_CT_PROCESS) != 0 ? process_cell(cx, node)
                                                  : normal_cell(cx, node, def);
    }
    return st;
}

odin3_status gv_write_ddg(gv_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < run->n_mods; id++) {
        ddg_ctx cx;
        memset(&cx, 0, sizeof cx);
        cx.run = run;
        cx.mod = odin3_module_get(run->design, (odin3_module_id){id});
        odin3_vec_init(&cx.edges, sizeof(ddg_edge));
        st = module_edges(&cx);
        st = st == ODIN3_OK ? put_graph(&cx, &run->mods[id]) : st;
        odin3_vec_free(&cx.edges);
    }
    return st;
}
