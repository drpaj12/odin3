/*
 * gv_cfg.c — write_graph cfg (spec section 4): one control flow graph per `$proc` node, built
 * from the process body (src/ir/proc.h) before `proc` lowers it. Entry (the root case), cases,
 * switches, one join per switch and an exit; edges are control (switch to case) or seq.
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "passes/graphviews/gv.h"
#include "util/alloc.h"

#include <stdio.h>
#include <string.h>

enum { FRAME_CASE, FRAME_SWITCH };

typedef struct cfg_frame {
    int kind;
    uint32_t idx;
    uint32_t next; /* next switch (CASE) or next case (SWITCH) */
    uint32_t cur;  /* CASE: the node the next switch hangs on, as an encoded node ID */
} cfg_frame;

/* Encoded node ID: kind in the top byte, index below. */
enum { NK_ENTRY, NK_CASE, NK_SWITCH, NK_JOIN, NK_EXIT, NK_SHIFT = 28 };

typedef struct cfg_ctx {
    gv_run *run;
    const odin3_module *module;
    odin3_node_id node;
    odin3_proc_body body;
    odin3_pinslice in, outs;
    odin3_net_id *nets;
    odin3_strbuf nodes, edges;
    uint32_t n_edges;
} cfg_ctx;

static uint32_t nid(uint32_t kind, uint32_t idx) {
    return (kind << NK_SHIFT) | idx;
}

/* The net at a body position (none for ANY or an unconnected pin). */
static odin3_net_id pos_net(const cfg_ctx *cx, odin3_pbit pbit) {
    uint32_t tag = pbit & ~ODIN3_PBIT_INDEX_MASK;
    uint32_t idx = pbit & ODIN3_PBIT_INDEX_MASK;
    if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT) {
        return (odin3_net_id){0};
    }
    odin3_pinslice port = tag == ODIN3_PBIT_IN ? cx->in : cx->outs;
    return odin3_pin_net(cx->module, (odin3_pin_id){port.first.v + idx});
}

/* Appends the RTLIL sigspec text of the span to text (raw, not quoted). */
static odin3_status span_text(cfg_ctx *cx, odin3_strbuf *text, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len; i++) {
        cx->nets[i] = pos_net(cx, cx->body.pbits[span.first + i]);
    }
    return odin3_proc_sigspec(cx->module, cx->nets, span.len, text);
}

/* The span as a JSON string of its RTLIL sigspec. */
static void w_span(cfg_ctx *cx, gv_w *wr, odin3_pspan span) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = span_text(cx, &text, span);
    if (wr->st == ODIN3_OK) {
        wr->st = st;
    }
    gv_w_str(wr, text.data);
    odin3_strbuf_free(&text);
}

static void w_nid(cfg_ctx *cx, gv_w *wr, uint32_t enc) {
    static const char *const k_names[] = {"entry", "c", "sw", "j", "exit"};
    uint32_t kind = enc >> NK_SHIFT;
    uint32_t idx = enc & ((1U << NK_SHIFT) - 1U);
    if (kind == NK_ENTRY || kind == NK_EXIT) {
        gv_w_fmt(wr, "\"proc:%u/%s\"", cx->node.v, k_names[kind]);
    } else {
        gv_w_fmt(wr, "\"proc:%u/%s%u\"", cx->node.v, k_names[kind], idx);
    }
}

static void w_actions(cfg_ctx *cx, gv_w *wr, odin3_pspan range) {
    gv_w_raw(wr, "[");
    for (uint32_t i = range.first; i < range.first + range.len; i++) {
        const odin3_proc_action *act = &cx->body.actions[i];
        gv_w_raw(wr, i == range.first ? "{\"lhs\": " : ", {\"lhs\": ");
        w_span(cx, wr, act->lhs);
        gv_w_raw(wr, ", \"rhs\": ");
        w_span(cx, wr, act->rhs);
        gv_w_raw(wr, ", ");
        gv_w_prov(wr, act->prov);
        gv_w_raw(wr, "}");
    }
    gv_w_raw(wr, "]");
}

/* The compare patterns of a case as a JSON array of strings. */
static void w_compares(cfg_ctx *cx, gv_w *wr, const odin3_proc_case *cs) {
    gv_w_raw(wr, "[");
    for (uint32_t j = 0; j < cs->n_compares; j++) {
        gv_w_raw(wr, j == 0 ? "" : ", ");
        w_span(cx, wr, cx->body.compares[cs->first_compare + j]);
    }
    gv_w_raw(wr, "]");
}

/* The label text: prefix, the optional switch signal, then the compares of a case. */
static void w_label(cfg_ctx *cx, gv_w *wr, const char *prefix, const odin3_pspan *sig,
                    const odin3_proc_case *cs) {
    odin3_strbuf inner;
    odin3_strbuf_init(&inner);
    odin3_status st = odin3_strbuf_append(&inner, odin3_bytes_cstr(prefix));
    if (st == ODIN3_OK && sig != NULL) {
        st = span_text(cx, &inner, *sig);
    }
    const char *space = prefix[0] != '\0' ? " " : "";
    for (uint32_t j = 0; st == ODIN3_OK && cs != NULL && j < cs->n_compares; j++) {
        st = odin3_strbuf_append(&inner, odin3_bytes_cstr(j == 0 ? space : " , "));
        st = st == ODIN3_OK ? span_text(cx, &inner, cx->body.compares[cs->first_compare + j]) : st;
    }
    if (wr->st == ODIN3_OK) {
        wr->st = st;
    }
    gv_w_str(wr, inner.data);
    odin3_strbuf_free(&inner);
}

static gv_w node_open(cfg_ctx *cx, uint32_t enc, const char *kind) {
    gv_w wr = {cx->run, &cx->nodes, ODIN3_OK};
    gv_w_raw(&wr, cx->nodes.len == 0 ? "" : ", ");
    gv_w_raw(&wr, "{\"id\": ");
    w_nid(cx, &wr, enc);
    gv_w_fmt(&wr, ", \"kind\": \"%s\", \"label\": ", kind);
    return wr;
}

static odin3_status node_plain(cfg_ctx *cx, uint32_t enc, const char *kind) {
    gv_w wr = node_open(cx, enc, kind);
    gv_w_fmt(&wr, "\"%s\", \"prov\": 0, \"src\": []}", kind);
    return wr.st;
}

static odin3_status node_entry(cfg_ctx *cx) {
    const odin3_proc_case *root = &cx->body.cases[0];
    gv_w wr = node_open(cx, nid(NK_ENTRY, 0), "entry");
    gv_w_raw(&wr, "\"process\", \"actions\": ");
    w_actions(cx, &wr, (odin3_pspan){root->first_action, root->n_actions});
    gv_w_raw(&wr, ", ");
    gv_w_prov(&wr, root->prov.v != 0 ? root->prov : odin3_node_prov(cx->module, cx->node));
    gv_w_raw(&wr, "}");
    return wr.st;
}

static odin3_status node_case(cfg_ctx *cx, uint32_t idx) {
    const odin3_proc_case *cs = &cx->body.cases[idx];
    gv_w wr = node_open(cx, nid(NK_CASE, idx), "case");
    if (cs->n_compares == 0) {
        gv_w_str(&wr, "default");
    } else {
        w_label(cx, &wr, "case", NULL, cs);
    }
    gv_w_fmt(&wr, ", \"default\": %s, \"compares\": ", cs->n_compares == 0 ? "true" : "false");
    w_compares(cx, &wr, cs);
    gv_w_raw(&wr, ", \"actions\": ");
    w_actions(cx, &wr, (odin3_pspan){cs->first_action, cs->n_actions});
    gv_w_raw(&wr, ", ");
    gv_w_prov(&wr, cs->prov);
    gv_w_raw(&wr, "}");
    return wr.st;
}

static odin3_status node_switch(cfg_ctx *cx, uint32_t idx) {
    const odin3_proc_switch *sw = &cx->body.switches[idx];
    odin3_pspan sig = sw->signal;
    gv_w wr = node_open(cx, nid(NK_SWITCH, idx), "switch");
    w_label(cx, &wr, "switch ", &sig, NULL);
    gv_w_raw(&wr, ", \"signal\": ");
    w_span(cx, &wr, sig);
    gv_w_fmt(&wr, ", \"full_case\": %s, \"parallel_case\": %s, \"signal_nets\": [",
             (sw->flags & ODIN3_SWITCH_FULL_CASE) != 0 ? "true" : "false",
             (sw->flags & ODIN3_SWITCH_PARALLEL_CASE) != 0 ? "true" : "false");
    for (uint32_t i = 0; i < sig.len; i++) {
        gv_w_fmt(&wr, "%s%u", i == 0 ? "" : ", ", pos_net(cx, cx->body.pbits[sig.first + i]).v);
    }
    gv_w_raw(&wr, "], ");
    gv_w_prov(&wr, sw->prov);
    gv_w_raw(&wr, "}");
    return wr.st;
}

/* The ends of an edge, as encoded node IDs. */
typedef struct cfg_ends {
    uint32_t from, to;
} cfg_ends;

#define NO_CASE UINT32_MAX

/* One edge; label_case is the case that gives a control edge its label, or NO_CASE for a seq. */
static odin3_status add_edge(cfg_ctx *cx, cfg_ends ends, uint32_t label_case) {
    gv_w wr = {cx->run, &cx->edges, ODIN3_OK};
    gv_w_fmt(&wr, "%s{\"id\": \"e%u\", \"from\": ", cx->n_edges == 0 ? "" : ", ", cx->n_edges);
    cx->n_edges++;
    w_nid(cx, &wr, ends.from);
    gv_w_raw(&wr, ", \"to\": ");
    w_nid(cx, &wr, ends.to);
    if (label_case == NO_CASE) {
        gv_w_raw(&wr, ", \"kind\": \"seq\", \"label\": \"\", \"prov\": 0, \"src\": []}");
        return wr.st;
    }
    const odin3_proc_case *cs = &cx->body.cases[label_case];
    gv_w_raw(&wr, ", \"kind\": \"control\", \"label\": ");
    if (cs->n_compares == 0) {
        gv_w_str(&wr, "default");
    } else {
        w_label(cx, &wr, "", NULL, cs);
    }
    gv_w_raw(&wr, ", ");
    gv_w_prov(&wr, cs->prov);
    gv_w_raw(&wr, "}");
    return wr.st;
}

/* A case frame on top: open its next switch or finish the case. */
static odin3_status step_case(cfg_ctx *cx, cfg_frame *stack, size_t *depth) {
    cfg_frame *top = &stack[*depth - 1];
    const odin3_proc_case *cs = &cx->body.cases[top->idx];
    odin3_status st;
    if (top->next < cs->first_switch + cs->n_switches) {
        uint32_t sw = top->next++;
        st = node_switch(cx, sw);
        st = st == ODIN3_OK ? add_edge(cx, (cfg_ends){top->cur, nid(NK_SWITCH, sw)}, NO_CASE) : st;
        stack[*depth] = (cfg_frame){FRAME_SWITCH, sw, cx->body.switches[sw].first_case, 0};
        (*depth)++;
        return st;
    }
    uint32_t exit_enc = top->cur;
    (*depth)--;
    if (*depth == 0) {
        st = node_plain(cx, nid(NK_EXIT, 0), "exit");
        return st == ODIN3_OK ? add_edge(cx, (cfg_ends){exit_enc, nid(NK_EXIT, 0)}, NO_CASE) : st;
    }
    return add_edge(cx, (cfg_ends){exit_enc, nid(NK_JOIN, stack[*depth - 1].idx)}, NO_CASE);
}

/* A switch frame on top: open its next case, or emit the join and pop. */
static odin3_status step_switch(cfg_ctx *cx, cfg_frame *stack, size_t *depth) {
    cfg_frame *top = &stack[*depth - 1];
    const odin3_proc_switch *sw = &cx->body.switches[top->idx];
    odin3_status st = ODIN3_OK;
    if (top->next < sw->first_case + sw->n_cases) {
        uint32_t cidx = top->next++;
        st = node_case(cx, cidx);
        cfg_ends ends = {nid(NK_SWITCH, top->idx), nid(NK_CASE, cidx)};
        st = st == ODIN3_OK ? add_edge(cx, ends, cidx) : st;
        stack[*depth] =
            (cfg_frame){FRAME_CASE, cidx, cx->body.cases[cidx].first_switch, nid(NK_CASE, cidx)};
        (*depth)++;
        return st;
    }
    uint32_t join = nid(NK_JOIN, top->idx);
    st = node_plain(cx, join, "join");
    if (st == ODIN3_OK && sw->n_cases == 0) {
        st = add_edge(cx, (cfg_ends){nid(NK_SWITCH, top->idx), join}, NO_CASE);
    }
    (*depth)--;
    stack[*depth - 1].cur = join;
    return st;
}

static odin3_status walk_body(cfg_ctx *cx) {
    size_t cap = 2 * (size_t)cx->body.n_switches + 2;
    cfg_frame *stack = odin3_util_malloc(cap * sizeof *stack);
    if (stack == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = node_entry(cx);
    stack[0] = (cfg_frame){FRAME_CASE, 0, cx->body.cases[0].first_switch, nid(NK_ENTRY, 0)};
    size_t depth = 1;
    while (st == ODIN3_OK && depth > 0) {
        st = stack[depth - 1].kind == FRAME_CASE ? step_case(cx, stack, &depth)
                                                 : step_switch(cx, stack, &depth);
    }
    odin3_util_free(stack);
    return st;
}

static void w_syncs(cfg_ctx *cx, gv_w *wr) {
    static const char *const k_words[] = {"", "posedge", "negedge", "low", "high", "always"};
    gv_w_raw(wr, ", \"syncs\": [");
    for (uint32_t k = 0; k < cx->body.n_syncs; k++) {
        const odin3_proc_sync *sy = &cx->body.syncs[k];
        odin3_net_id net = {0};
        if (sy->type != ODIN3_SYNC_ALWAYS) {
            net = pos_net(cx, sy->signal);
        }
        gv_w_fmt(wr, "%s{\"type\": \"%s\", \"signal_net\": %u, \"actions\": ", k == 0 ? "" : ", ",
                 k_words[sy->type], net.v);
        w_actions(cx, wr, (odin3_pspan){sy->first_action, sy->n_actions});
        gv_w_raw(wr, "}");
    }
    gv_w_raw(wr, "]");
}

/* Writes the graph of one process node, appending to the module's list. */
static odin3_status graph_of_proc(gv_run *run, const odin3_module *mod, odin3_node_id node,
                                  odin3_strbuf *out) {
    cfg_ctx cx;
    memset(&cx, 0, sizeof cx);
    cx.run = run;
    cx.module = mod;
    cx.node = node;
    odin3_strbuf_init(&cx.nodes);
    odin3_strbuf_init(&cx.edges);
    if (!odin3_proc_body_view(odin3_node_param(mod, node, ODIN3_PROC_P_BODY), &cx.body) ||
        cx.body.n_cases == 0) {
        return ODIN3_OK;
    }
    cx.in = odin3_node_port(mod, node, ODIN3_PROC_PORT_I);
    cx.outs = odin3_node_port(mod, node, ODIN3_PROC_PORT_O);
    cx.nets = odin3_util_malloc(sizeof *cx.nets * (cx.body.n_pbits + 1U));
    odin3_status st = cx.nets != NULL ? walk_body(&cx) : ODIN3_ERR_NO_MEMORY;
    gv_w wr = {run, out, st};
    gv_w_raw(&wr, out->len == 0 ? "" : ",\n      ");
    gv_w_fmt(&wr,
             "{\"id\": \"proc:%u\", \"kind\": \"cfg\", \"label\": \"process %u\", \"node\": %u, ",
             node.v, node.v, node.v);
    gv_w_prov(&wr, odin3_node_prov(mod, node));
    w_syncs(&cx, &wr);
    gv_w_raw(&wr, ", \"nodes\": [");
    if (wr.st == ODIN3_OK && cx.nodes.len > 0) {
        wr.st = odin3_strbuf_append(out, (odin3_bytes){cx.nodes.data, cx.nodes.len});
    }
    gv_w_raw(&wr, "], \"edges\": [");
    if (wr.st == ODIN3_OK && cx.edges.len > 0) {
        wr.st = odin3_strbuf_append(out, (odin3_bytes){cx.edges.data, cx.edges.len});
    }
    gv_w_raw(&wr, "]}");
    odin3_util_free(cx.nets);
    odin3_strbuf_free(&cx.nodes);
    odin3_strbuf_free(&cx.edges);
    return wr.st;
}

odin3_status gv_write_cfg(gv_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < run->n_mods; id++) {
        const odin3_module *mod = odin3_module_get(run->design, (odin3_module_id){id});
        for (uint32_t cell = 1; st == ODIN3_OK && cell < odin3_module_node_end(mod); cell++) {
            odin3_node_id node = {cell};
            const odin3_celltype_def *def =
                odin3_node_live(mod, node)
                    ? odin3_celltype_get(run->design, odin3_node_type(mod, node))
                    : NULL;
            if (def != NULL && (def->flags & ODIN3_CT_PROCESS) != 0) {
                st = graph_of_proc(run, mod, node, &run->mods[id]);
            }
        }
    }
    return st;
}
