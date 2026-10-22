/*
 * chain.c — carry chain discovery (C-1) and the conversion of chains to `$add` / `$sub` (C-2 to
 * C-8).
 */
#include "passes/unmap/chain.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"
#include "passes/unmap/operand.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- discovery (C-1) ----------------------------------------------------------------------- */

typedef struct um_links {
    odin3_vec succ;    /* uint32_t per node ID: the next adder of the chain, 0 none */
    odin3_vec pred;    /* the previous one */
    odin3_vec visited; /* uint32_t per node ID */
} um_links;

static bool is_adder(const um_ctx *uc, odin3_node_id node) {
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.adder.v;
}

static void links_free(um_links *links) {
    odin3_vec_free(&links->succ);
    odin3_vec_free(&links->pred);
    odin3_vec_free(&links->visited);
}

static odin3_status vec_zero(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint32_t));
    odin3_status st = odin3_vec_reserve(vec, count);
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        void *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memset(slot, 0, sizeof(uint32_t));
    }
    return st;
}

static uint32_t *u32_at(odin3_vec *vec, uint32_t index) {
    return odin3_vec_at(vec, index);
}

/* The adder whose `cin` is the only reader of the `cout` of `node` and nothing else reads it;
 * 0 when there is none (no sink, a tap or a branch). */
static uint32_t next_adder(const um_ctx *uc, odin3_node_id node) {
    odin3_net_id net = um_port_net(uc->module, node, (um_pos){UM_ADDER_COUT, 0});
    if (!odin3_net_valid(net)) {
        return 0;
    }
    odin3_pinlist sinks = odin3_net_sinks(uc->module, net);
    uint32_t cins = 0;
    uint32_t others = 0;
    uint32_t next = 0;
    for (uint32_t i = 0; i < sinks.count; i++) {
        odin3_node_id reader = odin3_pin_node(uc->module, sinks.pins[i]);
        if (is_adder(uc, reader) && odin3_pin_port(uc->module, sinks.pins[i]) == UM_ADDER_CIN) {
            cins++;
            next = reader.v;
        } else {
            others++;
        }
    }
    return cins == 1 && others == 0 ? next : 0;
}

static odin3_status make_links(const um_ctx *uc, um_links *links) {
    size_t end = odin3_module_node_end(uc->module);
    odin3_status st = vec_zero(&links->succ, end);
    st = st == ODIN3_OK ? vec_zero(&links->pred, end) : st;
    st = st == ODIN3_OK ? vec_zero(&links->visited, end) : st;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (!is_adder(uc, (odin3_node_id){nid})) {
            continue;
        }
        uint32_t next = next_adder(uc, (odin3_node_id){nid});
        if (next != 0 && next != nid && *u32_at(&links->pred, next) == 0) {
            *u32_at(&links->succ, nid) = next;
            *u32_at(&links->pred, next) = nid;
        }
    }
    return st;
}

static odin3_status add_cell(um_walk *walk, uint32_t node) {
    odin3_node_id *slot = odin3_vec_push(&walk->cells);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->v = node;
    return ODIN3_OK;
}

/* Walks succ from `start` into a new walk (ending at a cell with no successor or back at start,
 * a cycle); the walk is pushed on `walks` whatever the status. */
static odin3_status walk_from(um_links *links, uint32_t start, odin3_vec *walks) {
    um_walk *walk = odin3_vec_push(walks);
    if (walk == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    walk->decline = NULL;
    odin3_vec_init(&walk->cells, sizeof(odin3_node_id));
    odin3_status st = ODIN3_OK;
    uint32_t node = start;
    while (st == ODIN3_OK && node != 0 && *u32_at(&links->visited, node) == 0) {
        *u32_at(&links->visited, node) = 1;
        st = add_cell(walk, node);
        node = *u32_at(&links->succ, node);
    }
    if (node != 0) {
        walk->decline = "cycle";
    }
    return st;
}

odin3_status um_chain_walks(const um_ctx *uc, odin3_vec *walks) {
    um_links links;
    odin3_vec_init(walks, sizeof(um_walk));
    odin3_status st = make_links(uc, &links);
    uint32_t end = odin3_module_node_end(uc->module);
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (is_adder(uc, (odin3_node_id){nid}) && *u32_at(&links.pred, nid) == 0) {
            st = walk_from(&links, nid, walks);
        }
    }
    /* cells never visited lie on a cycle */
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        if (is_adder(uc, (odin3_node_id){nid}) && *u32_at(&links.visited, nid) == 0) {
            st = walk_from(&links, nid, walks);
        }
    }
    links_free(&links);
    return st;
}

void um_walks_free(odin3_vec *walks) {
    um_walk *items = walks->data;
    for (size_t i = 0; i < walks->len; i++) {
        odin3_vec_free(&items[i].cells);
    }
    odin3_vec_free(walks);
}

/* --- conversion (C-2 to C-8) --------------------------------------------------------------- */

enum { UM_BINARY_PARAMS = 5 };

typedef struct um_chain {
    um_ctx *uc;
    const um_walk *walk;
    const odin3_node_id *cells;
    size_t count;  /* cells of the walk */
    size_t stages; /* adder stages: the cells without the head */
    size_t first;  /* index in cells of stage 0: 1 with a head, else 0 */
    size_t width;  /* k: the result width of C-4 */
    bool sub;
    bool is_signed;
    bool cout_exposed;
    const char *decline; /* NULL while the chain is recoverable */
    odin3_vec a;         /* odin3_net_id, the nets of operand A */
    odin3_vec b;
    odin3_vec y;
    odin3_vec nots; /* odin3_node_id of the $not cells b reads */
    um_group group;
    const odin3_vec *owner; /* uint32_t per node: 1 + the index of its walk, 0 none */
    uint32_t id;            /* 1 + the index of this walk */
} um_chain;

static void chain_init(um_chain *job, um_ctx *uc, const um_walk *walk) {
    memset(job, 0, sizeof *job);
    job->uc = uc;
    job->walk = walk;
    job->cells = walk->cells.data;
    job->count = walk->cells.len;
    odin3_vec_init(&job->a, sizeof(odin3_net_id));
    odin3_vec_init(&job->b, sizeof(odin3_net_id));
    odin3_vec_init(&job->y, sizeof(odin3_net_id));
    odin3_vec_init(&job->nots, sizeof(odin3_node_id));
    um_group_init(&job->group);
}

static void chain_free(um_chain *job) {
    odin3_vec_free(&job->a);
    odin3_vec_free(&job->b);
    odin3_vec_free(&job->y);
    odin3_vec_free(&job->nots);
    um_group_free(&job->group);
}

static um_cls pin_class(const um_ctx *uc, odin3_node_id node, uint32_t port) {
    return um_net_class(uc->module, um_port_net(uc->module, node, (um_pos){port, 0}));
}

static bool is_const(um_cls cls) {
    return cls == UM_GND || cls == UM_VCC;
}

enum { UM_ADD_Y = 2 };

/* True when the pin is the carry-out of an adder, or the top Y bit of a `$add` this pass made from
 * a chain (a part of the carry chain already recovered: its last carry-out). Any other output,
 * a multiply's included, is a signal. */
static bool carry_source(const um_ctx *uc, odin3_pin_id pin) {
    odin3_node_id node = odin3_pin_node(uc->module, pin);
    uint32_t port = odin3_pin_port(uc->module, pin);
    if (is_adder(uc, node)) {
        return port == UM_ADDER_COUT;
    }
    uint32_t key = 0;
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.add.v && port == UM_ADD_Y &&
           odin3_pin_bit(uc->module, pin) + 1 ==
               odin3_node_port(uc->module, node, UM_ADD_Y).count &&
           odin3_design_intern(uc->design, odin3_bytes_cstr("unmap"), &key) == ODIN3_OK &&
           odin3_attr_get(uc->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key) != NULL;
}

/* C-3: why a signal or odd carry-in declines the chain. */
static const char *cin_reason(const um_ctx *uc, odin3_node_id cell, um_cls cls) {
    if (cls == UM_PAD) {
        return "cin_x";
    }
    if (cls == UM_NONE) {
        return "cin_open";
    }
    odin3_net_id net = um_port_net(uc->module, cell, (um_pos){UM_ADDER_CIN, 0});
    odin3_pin_id driver = odin3_net_driver(uc->module, net);
    bool from_adder = odin3_pin_valid(driver) && carry_source(uc, driver);
    return from_adder ? "cin_carry" : "cin_signal";
}

/* C-2: the head test; the cell is a head when a, b and cin are constants (pad on cin only with
 * a = b = vcc) and its sum is nobody's input. */
static bool is_head(const um_ctx *uc, odin3_node_id cell) {
    um_cls ca = pin_class(uc, cell, UM_ADDER_A);
    um_cls cb = pin_class(uc, cell, UM_ADDER_B);
    um_cls cc = pin_class(uc, cell, UM_ADDER_CIN);
    bool consts = is_const(ca) && is_const(cb) &&
                  (is_const(cc) || (cc == UM_PAD && ca == UM_VCC && cb == UM_VCC));
    return consts &&
           !um_observed(uc->module, um_port_net(uc->module, cell, (um_pos){UM_ADDER_SUMOUT, 0}));
}

/* C-2, C-3: head or not, the carry-in constant c0 (sub when 1), or the decline reason. */
static void chain_shape(um_chain *job) {
    const um_ctx *uc = job->uc;
    odin3_node_id cell = job->cells[0];
    if (is_head(uc, cell)) {
        job->first = 1;
        job->stages = job->count - 1;
        if (job->count == 1) {
            job->decline = "lone_head";
            return;
        }
        int votes = (pin_class(uc, cell, UM_ADDER_A) == UM_VCC) +
                    (pin_class(uc, cell, UM_ADDER_B) == UM_VCC) +
                    (pin_class(uc, cell, UM_ADDER_CIN) != UM_GND);
        job->sub = votes >= 2;
        return;
    }
    um_cls cls = pin_class(uc, cell, UM_ADDER_CIN);
    job->stages = job->count;
    if (is_const(cls)) {
        job->sub = cls == UM_VCC;
    } else {
        job->decline = cin_reason(uc, cell, cls);
    }
}

/* C-4: the result width: up to the highest stage whose sum is observed, plus one for an observed
 * last carry-out (an unsigned add only: a subtraction's carry is the inverse of its borrow bit). */
static void chain_width(um_chain *job) {
    const um_ctx *uc = job->uc;
    size_t width = 1;
    for (size_t i = 0; i < job->stages; i++) {
        odin3_node_id cell = job->cells[job->first + i];
        if (um_observed(uc->module, um_port_net(uc->module, cell, (um_pos){UM_ADDER_SUMOUT, 0}))) {
            width = i + 1;
        }
    }
    odin3_node_id last = job->cells[job->count - 1];
    if (um_observed(uc->module, um_port_net(uc->module, last, (um_pos){UM_ADDER_COUT, 0}))) {
        if (job->sub) {
            job->decline = "cout_sub";
            return;
        }
        width = job->stages + 1;
        job->cout_exposed = true;
    }
    job->width = width;
}

static odin3_status push_net(odin3_vec *vec, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

/* The 1-bit $not whose output is `net`; none when the driver is anything else. */
static odin3_node_id not_driver(const um_ctx *uc, odin3_net_id net) {
    odin3_pin_id driver = odin3_net_driver(uc->module, net);
    if (!odin3_pin_valid(driver)) {
        return (odin3_node_id){0};
    }
    odin3_node_id node = odin3_pin_node(uc->module, driver);
    bool one_bit = odin3_node_type(uc->module, node).v == uc->types.not_.v &&
                   odin3_node_port(uc->module, node, 0).count == 1;
    return one_bit ? node : (odin3_node_id){0};
}

/* C-5: the b net of a subtraction stage turned back into the operand bit. */
static odin3_status invert_b(um_chain *job, odin3_net_id net, odin3_net_id *out) {
    um_cls cls = um_net_class(job->uc->module, net);
    if (cls == UM_GND || cls == UM_VCC) {
        return um_const_net(job->uc, cls == UM_GND, out);
    }
    odin3_node_id node = cls == UM_SIG ? not_driver(job->uc, net) : (odin3_node_id){0};
    if (node.v == 0) {
        job->decline = "carry_in_one";
        return ODIN3_OK;
    }
    *out = um_port_net(job->uc->module, node, (um_pos){0, 0});
    odin3_node_id *slot = odin3_vec_push(&job->nots);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

static int cmp_id(const void *lhs, const void *rhs) {
    uint32_t left = ((const odin3_node_id *)lhs)->v;
    uint32_t right = ((const odin3_node_id *)rhs)->v;
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

/* True when every reader of the net is a `b` pin of a cell of this walk. */
static bool read_only_by_chain(const um_chain *job, odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(job->uc->module, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        odin3_node_id reader = odin3_pin_node(job->uc->module, sinks.pins[i]);
        bool in_walk = reader.v < job->owner->len &&
                       *(const uint32_t *)odin3_vec_cat(job->owner, reader.v) == job->id;
        if (!in_walk || odin3_pin_port(job->uc->module, sinks.pins[i]) != UM_ADDER_B) {
            return false;
        }
    }
    return true;
}

/* C-5: a $not cell no one outside this chain reads goes into the group. */
static odin3_status absorb_nots(um_chain *job) {
    odin3_node_id *items = job->nots.data;
    if (job->nots.len == 0) {
        return ODIN3_OK;
    }
    qsort(items, job->nots.len, sizeof *items, cmp_id);
    for (size_t i = 0; i < job->nots.len; i++) {
        if (i > 0 && items[i].v == items[i - 1].v) {
            continue;
        }
        odin3_net_id out = um_port_net(job->uc->module, items[i], (um_pos){1, 0});
        odin3_status st =
            read_only_by_chain(job, out) ? um_group_add(&job->group, items[i]) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* C-5: A, B and Y as net lists of the stages below the width (Y also holds the exposed carry). */
static odin3_status chain_operands(um_chain *job) {
    size_t used = job->width < job->stages ? job->width : job->stages;
    for (size_t i = 0; i < used && job->decline == NULL; i++) {
        odin3_node_id cell = job->cells[job->first + i];
        odin3_net_id bnet = um_port_net(job->uc->module, cell, (um_pos){UM_ADDER_B, 0});
        odin3_status st =
            push_net(&job->a, um_port_net(job->uc->module, cell, (um_pos){UM_ADDER_A, 0}));
        st = st == ODIN3_OK && job->sub ? invert_b(job, bnet, &bnet) : st;
        st = st == ODIN3_OK && job->decline == NULL ? push_net(&job->b, bnet) : st;
        st = st == ODIN3_OK ? push_net(&job->y, um_port_net(job->uc->module, cell,
                                                            (um_pos){UM_ADDER_SUMOUT, 0}))
                            : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (job->decline != NULL) {
        return ODIN3_OK;
    }
    if (job->cout_exposed) {
        odin3_node_id last = job->cells[job->count - 1];
        return push_net(&job->y, um_port_net(job->uc->module, last, (um_pos){UM_ADDER_COUT, 0}));
    }
    return ODIN3_OK;
}

typedef struct um_sized {
    um_vec a;
    um_vec b;
} um_sized;

/* C-5, C-6: trims the constant tails, then reads the signedness from the top runs. */
static um_sized chain_sign(um_chain *job) {
    const odin3_module *module = job->uc->module;
    um_sized sized = {{job->a.data, (uint32_t)job->a.len}, {job->b.data, (uint32_t)job->b.len}};
    uint32_t lost = um_trim_tail(module, &sized.a, true) + um_trim_tail(module, &sized.b, true);
    if (lost == 0 && !job->cout_exposed) {
        uint32_t wa = um_top_run(module, &sized.a);
        uint32_t wb = um_top_run(module, &sized.b);
        if (wa < sized.a.len || wb < sized.b.len) {
            job->is_signed = true;
            sized.a.len = wa;
            sized.b.len = wb;
        }
    }
    return sized;
}

/* The group: every cell of the walk (the head and the dropped stages too), then the $nots. */
static odin3_status chain_group(um_chain *job) {
    for (size_t i = 0; i < job->count; i++) {
        odin3_status st = um_group_add(&job->group, job->cells[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return absorb_nots(job);
}

static odin3_status chain_emit(um_chain *job) {
    um_ctx *uc = job->uc;
    um_sized sized = chain_sign(job);
    odin3_value params[UM_BINARY_PARAMS] = {
        odin3_value_int(job->is_signed), odin3_value_int(job->is_signed),
        odin3_value_int(sized.a.len), odin3_value_int(sized.b.len),
        odin3_value_int((int64_t)job->y.len)};
    odin3_netvec ports[3] = {{sized.a.nets, sized.a.len},
                             {sized.b.nets, sized.b.len},
                             {job->y.data, (uint32_t)job->y.len}};
    um_new req = {job->sub ? uc->types.sub : uc->types.add,
                  params,
                  UM_BINARY_PARAMS,
                  ports,
                  job->cells[0],
                  "chain",
                  0,
                  0};
    odin3_node_id out = {0};
    odin3_status st = um_replace(uc, &job->group, &req, &out);
    if (st == ODIN3_OK) {
        uc->stats.adder_cells += job->count;
        uc->stats.sub += job->sub ? 1 : 0;
        uc->stats.add += job->sub ? 0 : 1;
    }
    return st;
}

static odin3_status convert_walk(um_chain *job) {
    chain_shape(job);
    if (job->decline == NULL) {
        chain_width(job);
    }
    odin3_status st = job->decline == NULL ? chain_operands(job) : ODIN3_OK;
    st = st == ODIN3_OK && job->decline == NULL ? chain_group(job) : st;
    st = st == ODIN3_OK && job->decline == NULL ? chain_emit(job) : st;
    return st;
}

static odin3_status decline_walk(um_ctx *uc, const um_walk *walk, const char *reason) {
    return um_decline(uc, walk->cells.data, walk->cells.len, (um_why){"adder", reason});
}

static odin3_status run_walk(um_ctx *uc, const um_walk *walk, const odin3_vec *owner, uint32_t id) {
    if (walk->decline != NULL) {
        uc->stats.adder_cells += walk->cells.len;
        return decline_walk(uc, walk, walk->decline);
    }
    um_chain job;
    chain_init(&job, uc, walk);
    job.owner = owner;
    job.id = id;
    odin3_status st = convert_walk(&job);
    if (st == ODIN3_OK && job.decline != NULL) {
        uc->stats.adder_cells += walk->cells.len;
        st = decline_walk(uc, walk, job.decline);
    }
    chain_free(&job);
    return st;
}

/* uint32_t per node: 1 + the index of the walk the node is in. */
static odin3_status make_owner(const um_ctx *uc, const odin3_vec *walks, odin3_vec *owner) {
    odin3_status st = vec_zero(owner, odin3_module_node_end(uc->module));
    const um_walk *items = walks->data;
    for (size_t wi = 0; st == ODIN3_OK && wi < walks->len; wi++) {
        const odin3_node_id *cells = items[wi].cells.data;
        for (size_t i = 0; i < items[wi].cells.len; i++) {
            *u32_at(owner, cells[i].v) = (uint32_t)wi + 1;
        }
    }
    return st;
}

odin3_status um_adder_phase(um_ctx *uc) {
    odin3_vec walks;
    odin3_vec owner;
    odin3_vec_init(&owner, sizeof(uint32_t));
    odin3_status st = um_chain_walks(uc, &walks);
    st = st == ODIN3_OK ? make_owner(uc, &walks, &owner) : st;
    const um_walk *items = walks.data;
    for (size_t wi = 0; st == ODIN3_OK && wi < walks.len; wi++) {
        st = run_walk(uc, &items[wi], &owner, (uint32_t)wi + 1);
    }
    odin3_vec_free(&owner);
    um_walks_free(&walks);
    return st;
}
