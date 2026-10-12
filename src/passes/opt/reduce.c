/*
 * reduce.c — opt_reduce (2F spec OP-8, §8; plan Task 9): Yosys 0.55 opt_reduce.cc over Odin III's
 * nodes ($reduce_or, $reduce_and, $mux, $pmux; the registry has no $bmux or $demux, §17 row 5).
 *
 * Operands are keys, 64-bit words: 0..3 for the constants 0, 1, x, z (whichever net carries them),
 * ODIN3_OPT_N_CONST + net ID for any other net. Nets are merged aliases already, so Yosys's
 * assign_map is the identity here, and "sorted" means by key: constants first, then net ID (Yosys:
 * constants first, then wire name).
 *
 * The reductions of one type form a worklist. Each is visited once; a bit driven by a reduction of
 * the same type pushes that child's frame on an explicit stack (the recursion of Yosys's
 * opt_reduce), so the child is finished first and the parent reads its new input set. A child whose
 * frame is still open (a loop of reductions) is read with its old inputs, as Yosys's recursion
 * does. Nothing is rewritten until the whole type has been solved; then each changed reduction is
 * rebuilt in ID order (kit_rebuild_node: same type, new A_WIDTH and A, same Y).
 *
 * The muxes are visited in ID order. opt_mux_bits groups the Y bits by their input tuple, with a
 * hash (group_equal below); opt_pmux groups the B slices the same way, so a $pmux with S slices
 * costs O(B pins), where Yosys compares every pair of slices. Each group keeps its first
 * occurrence's position, which is also what Yosys's dict yields (checked against 0.55 in
 * tests/unit/data/opt/reduce/pmux_groups.il).
 *
 * Differences from Yosys (spec §17): a reduction whose inputs are already unique and constant-free
 * is not rebuilt when merely unsorted (Yosys re-sorts every A without counting it); a cell with an
 * unconnected input pin is left alone.
 *
 * Failure (OP-2): each rewrite builds what it needs before its first destructive step; when the
 * build fails the rewrite is undone (the cells it made are deleted) and NO_MEMORY returns with the
 * IR valid. A failure after the destructive step is the kit's "half done" case.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- the hash and its test hook --------------------------------------------------------------- */

static uint64_t (*g_key_hash)(const void *key, size_t len);

void odin3_opt_reduce_test_set_key_hash(uint64_t (*fn)(const void *key, size_t len)) {
    g_key_hash = fn;
}

static uint64_t hash_run(const uint64_t *run, size_t count) {
    if (g_key_hash != NULL) {
        return g_key_hash(run, count * sizeof *run);
    }
    return odin3_hash_bytes((odin3_bytes){run, count * sizeof *run}, ODIN3_HASH_SEED);
}

/* --- types, ports and parameters -------------------------------------------------------------- */

typedef enum tkind { T_ROR, T_RAND, T_MUX, T_PMUX, T_MEM, T_MEMWR, T_DFF, T_COUNT } tkind;

static const char *const k_type_names[T_COUNT] = {"$reduce_or", "$reduce_and", "$mux", "$pmux",
                                                  "$mem",       "$memwr",      "$dff"};

enum {
    N_CONST = ODIN3_OPT_N_CONST,
    PORT_RED_A = 0, /* $reduce_*: A, Y */
    PORT_MUX_A = 0, /* $mux, $pmux: A, B, S, Y */
    PORT_MUX_B = 1,
    PORT_MUX_S = 2,
    PORT_MUX_Y = 3,
    PRM_RED_N = 3, /* $reduce_*: A_SIGNED, A_WIDTH, Y_WIDTH */
    PRM_RED_A_WIDTH = 1,
    PRM_PMUX_N = 2, /* $mux: WIDTH; $pmux: WIDTH, S_WIDTH */
    PORT_RED_Y = 1,
    GROUP_DIRECT_MAX = 16
};

#define NONE_GROUP UINT32_MAX
#define NONE_PORT UINT32_MAX

/* --- the state -------------------------------------------------------------------------------- */

/* A reduction of the type being solved. */
typedef struct member {
    uint32_t node, width;    /* the node; its A_WIDTH */
    size_t old_off, old_len; /* its A, sorted and de-duplicated, in the pool */
    size_t cur_off, cur_len; /* what a reader of its Y takes: old, then the result */
    uint8_t state;
    bool changed;
} member;

enum { ST_PENDING, ST_ACTIVE, ST_DONE };

/* One open visit: the member, the next key of its A to look at, where its bits start in acc. */
typedef struct frame {
    uint32_t mem;
    size_t pos, acc_start;
} frame;

/* A $reduce_or made by opt_pmux, for undoing it. */
typedef struct made_cell {
    uint32_t node, net;
} made_cell;

typedef struct rd_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    odin3_celltype_id types[T_COUNT];
    uint32_t mem_wr_en, memwr_en, dff_d, dff_q; /* port indices */
    uint32_t node_end, count;
    odin3_status st;
    odin3_vec wren, stack; /* uint8_t per net ID: feeds a write enable; net IDs to expand */
    bool any_wren;
    /* the reductions of one type */
    odin3_vec members, pool, acc, frames; /* member; u64 keys; u64 keys; frame */
    uint32_t *slot;                       /* per node ID: member index + 1 */
    bool is_or;
    /* the muxes */
    odin3_vec an, bn, sn, yn, ak, bk, tup;               /* operand nets and keys of one mux */
    odin3_vec rep, pos, kept, gid, gstart, gorder, fill; /* uint32_t */
    odin3_vec next;                                      /* uint32_t: hash chains */
    odin3_vec new_a, new_b, new_s, srcs, tmp;            /* odin3_net_id */
    odin3_vec made;                                      /* made_cell */
    uint32_t groups;
} rd_state;

static uint32_t port_named(const odin3_design *design, odin3_celltype_id type, const char *name) {
    if (type.v == 0) {
        return NONE_PORT;
    }
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    return NONE_PORT;
}

static void state_init(rd_state *rs, odin3_opt_ctx *oc) {
    memset(rs, 0, sizeof *rs);
    rs->oc = oc;
    rs->m = oc->module;
    for (int i = 0; i < T_COUNT; i++) {
        rs->types[i] = odin3_kit_type_named(oc->design, k_type_names[i]);
    }
    rs->mem_wr_en = port_named(oc->design, rs->types[T_MEM], "WR_EN");
    rs->memwr_en = port_named(oc->design, rs->types[T_MEMWR], "EN");
    rs->dff_d = port_named(oc->design, rs->types[T_DFF], "D");
    rs->dff_q = port_named(oc->design, rs->types[T_DFF], "Q");
    odin3_vec_init(&rs->wren, sizeof(uint8_t));
    odin3_vec_init(&rs->stack, sizeof(uint32_t));
    odin3_vec_init(&rs->members, sizeof(member));
    odin3_vec_init(&rs->pool, sizeof(uint64_t));
    odin3_vec_init(&rs->acc, sizeof(uint64_t));
    odin3_vec_init(&rs->frames, sizeof(frame));
    odin3_vec_init(&rs->an, sizeof(odin3_net_id));
    odin3_vec_init(&rs->bn, sizeof(odin3_net_id));
    odin3_vec_init(&rs->sn, sizeof(odin3_net_id));
    odin3_vec_init(&rs->yn, sizeof(odin3_net_id));
    odin3_vec_init(&rs->ak, sizeof(uint64_t));
    odin3_vec_init(&rs->bk, sizeof(uint64_t));
    odin3_vec_init(&rs->tup, sizeof(uint64_t));
    odin3_vec *u32s[] = {&rs->rep,    &rs->pos,    &rs->kept, &rs->gid,
                         &rs->gstart, &rs->gorder, &rs->fill, &rs->next};
    for (size_t i = 0; i < sizeof u32s / sizeof u32s[0]; i++) {
        odin3_vec_init(u32s[i], sizeof(uint32_t));
    }
    odin3_vec *nets[] = {&rs->new_a, &rs->new_b, &rs->new_s, &rs->srcs, &rs->tmp};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        odin3_vec_init(nets[i], sizeof(odin3_net_id));
    }
    odin3_vec_init(&rs->made, sizeof(made_cell));
}

static void state_free(rd_state *rs) {
    odin3_vec *all[] = {&rs->wren,   &rs->stack, &rs->members, &rs->pool,  &rs->acc,   &rs->frames,
                        &rs->an,     &rs->bn,    &rs->sn,      &rs->yn,    &rs->ak,    &rs->bk,
                        &rs->tup,    &rs->rep,   &rs->pos,     &rs->kept,  &rs->gid,   &rs->gstart,
                        &rs->gorder, &rs->fill,  &rs->next,    &rs->new_a, &rs->new_b, &rs->new_s,
                        &rs->srcs,   &rs->tmp,   &rs->made};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
    odin3_util_free(rs->slot);
}

/* --- small helpers ---------------------------------------------------------------------------- */

static odin3_status push_u64(odin3_vec *vec, uint64_t value) {
    uint64_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* vec = n zeroed uint32_t elements. */
static odin3_status zero_u32(odin3_vec *vec, size_t n) {
    odin3_vec_clear(vec);
    odin3_status st = odin3_vec_reserve(vec, n);
    for (size_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = push_u32(vec, 0);
    }
    return st;
}

static void truncate_vec(odin3_vec *vec, size_t len) {
    while (vec->len > len) {
        odin3_vec_pop(vec);
    }
}

/* Sorts vec[from ..) and drops the repeats (Yosys's sort_and_unify). */
static void sort_unify(odin3_vec *vec, size_t from) {
    uint64_t *data = (uint64_t *)vec->data + from;
    size_t count = vec->len - from;
    if (count < 2) {
        return;
    }
    qsort(data, count, sizeof *data, odin3_kit_cmp_u64);
    size_t kept = 1;
    for (size_t i = 1; i < count; i++) {
        if (data[i] != data[kept - 1]) {
            data[kept++] = data[i];
        }
    }
    truncate_vec(vec, from + kept);
}

/* The key of a net: its constant's value, else N_CONST + its ID. */
static uint64_t net_key(const odin3_module *module, odin3_net_id net) {
    odin3_const cv = odin3_net_const_value(module, net);
    if (cv != ODIN3_CONST_NONE) {
        return (uint64_t)cv - (uint64_t)ODIN3_CONST_0;
    }
    return (uint64_t)N_CONST + net.v;
}

/* The net a key stands for (the canonical constant net for a constant). */
static odin3_status key_net(rd_state *rs, uint64_t key, odin3_net_id *out) {
    if (key < N_CONST) {
        return odin3_opt_const_net(rs->oc, (odin3_bit)key, out);
    }
    *out = (odin3_net_id){(uint32_t)(key - N_CONST)};
    return ODIN3_OK;
}

typedef enum gather_status { GATHER_OK, GATHER_OPEN, GATHER_OOM } gather_status;

/* Appends the nets (if nets) and keys (if keys) of the pins of `port`; OPEN at an unconnected pin.
 */
static gather_status gather_port(const rd_state *rs, odin3_node_id node, uint32_t port,
                                 odin3_vec *nets, odin3_vec *keys) {
    odin3_pinslice ps = odin3_node_port(rs->m, node, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(rs->m, (odin3_pin_id){ps.first.v + k});
        if (!odin3_net_valid(net)) {
            return GATHER_OPEN;
        }
        if ((nets != NULL && odin3_kit_push_net(nets, net) != ODIN3_OK) ||
            (keys != NULL && push_u64(keys, net_key(rs->m, net)) != ODIN3_OK)) {
            return GATHER_OOM;
        }
    }
    return GATHER_OK;
}

static bool is_type(const rd_state *rs, odin3_node_id node, tkind kind) {
    return rs->types[kind].v != 0 && odin3_node_live(rs->m, node) &&
           odin3_node_type(rs->m, node).v == rs->types[kind].v;
}

/* --- the reductions: building the members ----------------------------------------------------- */

static member *member_at(rd_state *rs, size_t index) {
    return odin3_vec_at(&rs->members, index);
}

/* Adds node as a member: its A as sorted, de-duplicated keys in the pool. */
static odin3_status member_add(rd_state *rs, odin3_node_id node) {
    odin3_vec_clear(&rs->acc);
    gather_status gs = gather_port(rs, node, PORT_RED_A, NULL, &rs->acc);
    if (gs != GATHER_OK) {
        return gs == GATHER_OOM ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    }
    member *mb = odin3_vec_push(&rs->members);
    if (mb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    mb->node = node.v;
    mb->width = (uint32_t)rs->acc.len;
    sort_unify(&rs->acc, 0);
    mb->old_off = rs->pool.len;
    mb->old_len = rs->acc.len;
    mb->cur_off = mb->old_off;
    mb->cur_len = mb->old_len;
    for (size_t i = 0; i < rs->acc.len; i++) {
        odin3_status st = push_u64(&rs->pool, ((const uint64_t *)rs->acc.data)[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    rs->slot[node.v] = (uint32_t)rs->members.len;
    return ODIN3_OK;
}

/* --- the reductions: solving ------------------------------------------------------------------ */

/* A member and the bit of its Y that drives a net. */
typedef struct child_ref {
    uint32_t mem, ybit;
} child_ref;

/* The member whose Y drives net; false when no member does. */
static bool child_of(const rd_state *rs, odin3_net_id net, child_ref *out) {
    if (odin3_net_driver_count(rs->m, net) != 1) {
        return false;
    }
    odin3_pin_id pin = odin3_net_driver(rs->m, net);
    uint32_t slot = rs->slot[odin3_pin_node(rs->m, pin).v];
    if (slot == 0) {
        return false;
    }
    out->mem = slot - 1U;
    out->ybit = odin3_pin_bit(rs->m, pin);
    return true;
}

/* acc += the keys a reader of mb's Y takes. */
static odin3_status copy_cur(rd_state *rs, const member *mb) {
    for (size_t i = 0; i < mb->cur_len; i++) {
        odin3_status st = push_u64(&rs->acc, ((const uint64_t *)rs->pool.data)[mb->cur_off + i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

typedef enum step_result { STEP_NEXT, STEP_CHILD, STEP_STOP } step_result;

/* Looks at one key of the frame's cell: the constants, then a bit that a child drives. */
static step_result key_step(rd_state *rs, const frame *top, uint64_t key) {
    const uint64_t absorbing = rs->is_or ? 1U : 0U;
    if (key < N_CONST) {
        if (key == absorbing) {
            truncate_vec(&rs->acc, top->acc_start);
            rs->st = push_u64(&rs->acc, key);
            return STEP_STOP;
        }
        rs->st = key == 1U - absorbing ? ODIN3_OK : push_u64(&rs->acc, key); /* x, z stay */
        return STEP_NEXT;
    }
    child_ref child = {0, 0};
    if (!child_of(rs, (odin3_net_id){(uint32_t)(key - N_CONST)}, &child)) {
        rs->st = push_u64(&rs->acc, key);
        return STEP_NEXT;
    }
    member *cm = member_at(rs, child.mem);
    if (cm->state == ST_PENDING) {
        cm->state = ST_ACTIVE;
        frame *fr = odin3_vec_push(&rs->frames);
        rs->st = fr == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (fr != NULL) {
            fr->mem = child.mem;
            fr->acc_start = rs->acc.len;
        }
        return STEP_CHILD;
    }
    rs->st = child.ybit == 0 ? copy_cur(rs, cm) : push_u64(&rs->acc, 0); /* Y[1..] are 0 */
    return STEP_NEXT;
}

/* Closes the top frame: sorts its bits, records the result and whether it differs from the input.
 */
static odin3_status frame_finish(rd_state *rs) {
    const frame *fr = odin3_vec_cat(&rs->frames, rs->frames.len - 1U);
    member *mb = member_at(rs, fr->mem);
    const size_t start = fr->acc_start;
    sort_unify(&rs->acc, start);
    odin3_status st = rs->acc.len == start ? push_u64(&rs->acc, rs->is_or ? 0U : 1U) : ODIN3_OK;
    const size_t len = rs->acc.len - start;
    const uint64_t *res = (const uint64_t *)rs->acc.data + start;
    const uint64_t *old = (const uint64_t *)rs->pool.data + mb->old_off;
    const bool same = len == mb->old_len && memcmp(res, old, len * sizeof *res) == 0;
    mb->changed = !same || mb->old_len != mb->width;
    if (!same) {
        mb->cur_off = rs->pool.len;
        mb->cur_len = len;
        for (size_t i = 0; st == ODIN3_OK && i < len; i++) {
            st = push_u64(&rs->pool, ((const uint64_t *)rs->acc.data)[start + i]);
        }
    }
    mb->state = ST_DONE;
    truncate_vec(&rs->acc, start);
    odin3_vec_pop(&rs->frames);
    return st;
}

/* Runs the top frame until it finishes (and is popped) or opens a child's frame. */
static odin3_status frame_run(rd_state *rs) {
    for (;;) {
        frame *fr = odin3_vec_at(&rs->frames, rs->frames.len - 1U);
        const member *mb = member_at(rs, fr->mem);
        if (fr->pos >= mb->old_len) {
            return frame_finish(rs);
        }
        uint64_t key = ((const uint64_t *)rs->pool.data)[mb->old_off + fr->pos];
        step_result res = key_step(rs, fr, key);
        if (rs->st != ODIN3_OK || res == STEP_CHILD) {
            return rs->st;
        }
        fr->pos = res == STEP_STOP ? mb->old_len : fr->pos + 1U;
    }
}

/* Visits member first and everything it needs, children first. */
static odin3_status solve_from(rd_state *rs, uint32_t first) {
    member_at(rs, first)->state = ST_ACTIVE;
    frame *fr = odin3_vec_push(&rs->frames);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    fr->mem = first;
    fr->acc_start = rs->acc.len;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && rs->frames.len > 0) {
        st = frame_run(rs);
    }
    return st;
}

/* --- the reductions: rebuilding --------------------------------------------------------------- */

/* Rebuilds the member's node with its result as A (keys to nets first: the build, then one
 * rebuild). */
static odin3_status member_rebuild(rd_state *rs, const member *mb) {
    odin3_node_id node = {mb->node};
    odin3_vec_clear(&rs->new_a);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < mb->cur_len; i++) {
        odin3_net_id net = {0};
        st = key_net(rs, ((const uint64_t *)rs->pool.data)[mb->cur_off + i], &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(&rs->new_a, net) : st;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[PRM_RED_N];
    for (uint32_t i = 0; i < PRM_RED_N; i++) {
        params[i] = *odin3_node_param(rs->m, node, i);
    }
    params[PRM_RED_A_WIDTH] = odin3_value_int((int64_t)mb->cur_len);
    const odin3_netvec ports[2] = {{rs->new_a.data, (uint32_t)rs->new_a.len}, {NULL, 0}};
    odin3_kit_rebuild req;
    memset(&req, 0, sizeof req);
    req.old_node = node;
    req.cell.type = odin3_node_type(rs->m, node);
    req.cell.params = params;
    req.cell.n_params = PRM_RED_N;
    req.cell.ports = ports;
    odin3_kit_begin_rewrite(rs->oc);
    odin3_node_id fresh = {0};
    return odin3_kit_rebuild_node(rs->oc, &req, &fresh);
}

static void phase_reset(rd_state *rs) {
    odin3_vec_clear(&rs->members);
    odin3_vec_clear(&rs->pool);
    odin3_vec_clear(&rs->acc);
    odin3_vec_clear(&rs->frames);
    odin3_util_free(rs->slot);
    rs->slot = NULL;
}

/* The members of kind: every reduction of that type whose inputs are all connected. */
static odin3_status phase_members(rd_state *rs, tkind kind) {
    rs->is_or = kind == T_ROR;
    rs->node_end = odin3_module_node_end(rs->m); /* the phases before this one made nodes */
    rs->slot = odin3_util_calloc(sizeof *rs->slot * ((size_t)rs->node_end + 1U));
    odin3_status st = rs->slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < rs->node_end; id++) {
        st = is_type(rs, (odin3_node_id){id}, kind) ? member_add(rs, (odin3_node_id){id}) : st;
    }
    return st;
}

/* Solves every member, children first (each member is visited once). */
static odin3_status phase_solve(rd_state *rs) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rs->members.len; i++) {
        st = member_at(rs, i)->state == ST_PENDING ? solve_from(rs, (uint32_t)i) : st;
    }
    return st;
}

/* Rebuilds the changed members in ID order and counts them. */
static odin3_status phase_apply(rd_state *rs) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rs->members.len; i++) {
        const member *mb = member_at(rs, i);
        st = mb->changed ? member_rebuild(rs, mb) : st;
        rs->count += st == ODIN3_OK && mb->changed ? 1U : 0U;
    }
    return st;
}

/* One type of reduction: solve every member (children first), then rebuild the changed ones. */
static odin3_status reduce_phase(rd_state *rs, tkind kind) {
    phase_reset(rs);
    if (rs->types[kind].v == 0) {
        return ODIN3_OK;
    }
    odin3_status st = phase_members(rs, kind);
    st = st == ODIN3_OK ? phase_solve(rs) : st;
    st = st == ODIN3_OK ? phase_apply(rs) : st;
    phase_reset(rs);
    return st;
}

/* --- mem_wren_sigs ---------------------------------------------------------------------------- */

static bool wren_get(const rd_state *rs, odin3_net_id net) {
    return net.v < rs->wren.len && ((const uint8_t *)rs->wren.data)[net.v] != 0;
}

/* Marks net (and queues it for the mux expansion); a no-op when it is marked. */
static odin3_status wren_mark(rd_state *rs, odin3_net_id net) {
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && rs->wren.len <= net.v) {
        st = odin3_vec_push(&rs->wren) == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    }
    if (st != ODIN3_OK || wren_get(rs, net)) {
        return st;
    }
    *(uint8_t *)odin3_vec_at(&rs->wren, net.v) = 1;
    rs->any_wren = true;
    return push_u32(&rs->stack, net.v);
}

static odin3_status wren_mark_port(rd_state *rs, odin3_node_id node, uint32_t port) {
    odin3_pinslice ps = odin3_node_port(rs->m, node, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(rs->m, (odin3_pin_id){ps.first.v + k});
        odin3_status st = odin3_net_valid(net) ? wren_mark(rs, net) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static bool wren_any_port(const rd_state *rs, odin3_node_id node, uint32_t port) {
    odin3_pinslice ps = odin3_node_port(rs->m, node, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        odin3_net_id net = odin3_pin_net(rs->m, (odin3_pin_id){ps.first.v + k});
        if (odin3_net_valid(net) && wren_get(rs, net)) {
            return true;
        }
    }
    return false;
}

/* The write enables of $mem and $memwr, then (one pass) the D of each $dff whose Q is marked. */
static odin3_status wren_seed(rd_state *rs) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < rs->node_end; id++) {
        odin3_node_id node = {id};
        if (is_type(rs, node, T_MEM)) {
            st = wren_mark_port(rs, node, rs->mem_wr_en);
        } else if (is_type(rs, node, T_MEMWR)) {
            st = wren_mark_port(rs, node, rs->memwr_en);
        }
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < rs->node_end; id++) {
        odin3_node_id node = {id};
        if (is_type(rs, node, T_DFF) && wren_any_port(rs, node, rs->dff_q)) {
            st = wren_mark_port(rs, node, rs->dff_d);
        }
    }
    return st;
}

/* Net marked: if a $mux drives it, the mux's A and B are marked too (until nothing is new). */
static odin3_status wren_expand(rd_state *rs) {
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && rs->stack.len > 0) {
        odin3_net_id net = {*(const uint32_t *)odin3_vec_cat(&rs->stack, rs->stack.len - 1U)};
        odin3_vec_pop(&rs->stack);
        odin3_pin_id pin = odin3_net_driver(rs->m, net);
        odin3_node_id node = odin3_pin_valid(pin) ? odin3_pin_node(rs->m, pin) : (odin3_node_id){0};
        if (odin3_node_valid(node) && is_type(rs, node, T_MUX)) {
            st = wren_mark_port(rs, node, PORT_MUX_A);
            st = st == ODIN3_OK ? wren_mark_port(rs, node, PORT_MUX_B) : st;
        }
    }
    return st;
}

static odin3_status wren_build(rd_state *rs) {
    odin3_vec_clear(&rs->wren);
    odin3_vec_clear(&rs->stack);
    rs->any_wren = false;
    rs->node_end = odin3_module_node_end(rs->m);
    odin3_status st = wren_seed(rs);
    return st == ODIN3_OK ? wren_expand(rs) : st;
}

/* --- grouping equal runs of keys -------------------------------------------------------------- */

/* count runs of width keys, run i at flat + i * width. */
typedef struct run_set {
    const uint64_t *flat;
    size_t count, width;
} run_set;

static const uint64_t *run_at(const run_set *set, size_t index) {
    return set->flat + index * set->width;
}

static bool run_equal(const run_set *set, size_t lhs, size_t rhs) {
    return memcmp(run_at(set, lhs), run_at(set, rhs), set->width * sizeof *set->flat) == 0;
}

/* rs->rep[i] = the first item whose run equals item i's (a scan of the earlier first items). */
static odin3_status group_direct(rd_state *rs, const run_set *set) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < set->count; i++) {
        uint32_t found = (uint32_t)i;
        for (size_t j = 0; j < i; j++) {
            if (((const uint32_t *)rs->rep.data)[j] == j && run_equal(set, i, j)) {
                found = (uint32_t)j;
                break;
            }
        }
        st = push_u32(&rs->rep, found);
    }
    return st;
}

/* One item of the hashed grouping: the chain of a hash holds the distinct first occurrences. */
static odin3_status group_item(rd_state *rs, odin3_u64map *map, const run_set *set, size_t item) {
    const uint64_t hash = hash_run(run_at(set, item), set->width);
    uint64_t head = 0;
    (void)odin3_u64map_get(map, hash, &head);
    uint32_t found = (uint32_t)item;
    for (uint64_t at = head; at != 0; at = ((const uint32_t *)rs->next.data)[at - 1U]) {
        if (run_equal(set, item, (size_t)at - 1U)) {
            found = (uint32_t)(at - 1U);
            break;
        }
    }
    odin3_status st = push_u32(&rs->rep, found);
    st = st == ODIN3_OK ? push_u32(&rs->next, found == item ? (uint32_t)head : 0U) : st;
    if (st == ODIN3_OK && found == item) {
        st = odin3_u64map_put(map, (odin3_kv){hash, (uint64_t)item + 1U});
    }
    return st;
}

static odin3_status group_hashed(rd_state *rs, const run_set *set) {
    odin3_u64map *map = odin3_u64map_create(set->count);
    odin3_vec_clear(&rs->next);
    odin3_status st = map == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < set->count; i++) {
        st = group_item(rs, map, set, i);
    }
    odin3_u64map_destroy(map);
    return st;
}

/* rs->rep = for each item of set, the first item with an equal run. */
static odin3_status group_equal(rd_state *rs, const run_set *set) {
    odin3_vec_clear(&rs->rep);
    odin3_status st = odin3_vec_reserve(&rs->rep, set->count);
    if (st != ODIN3_OK) {
        return st;
    }
    return set->count <= GROUP_DIRECT_MAX ? group_direct(rs, set) : group_hashed(rs, set);
}

/* --- the muxes: loading and undoing ----------------------------------------------------------- */

/* The operand nets and keys of the mux (A, B and S/Y nets); OPEN when any of its pins is open. */
static gather_status mux_load(rd_state *rs, odin3_node_id node) {
    odin3_vec *all[] = {&rs->an, &rs->bn, &rs->sn, &rs->yn, &rs->ak, &rs->bk};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_clear(all[i]);
    }
    gather_status gs = gather_port(rs, node, PORT_MUX_A, &rs->an, &rs->ak);
    gs = gs == GATHER_OK ? gather_port(rs, node, PORT_MUX_B, &rs->bn, &rs->bk) : gs;
    gs = gs == GATHER_OK ? gather_port(rs, node, PORT_MUX_S, &rs->sn, NULL) : gs;
    return gs == GATHER_OK ? gather_port(rs, node, PORT_MUX_Y, &rs->yn, NULL) : gs;
}

static bool feeds_wren(const rd_state *rs) {
    for (size_t i = 0; rs->any_wren && i < rs->yn.len; i++) {
        if (wren_get(rs, ((const odin3_net_id *)rs->yn.data)[i])) {
            return true;
        }
    }
    return false;
}

/* The nets of port `port` of node, in rs->tmp. */
static odin3_status port_nets(rd_state *rs, odin3_node_id node, uint32_t port) {
    odin3_vec_clear(&rs->tmp);
    gather_status gs = gather_port(rs, node, port, &rs->tmp, NULL);
    return gs == GATHER_OOM ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

/* Deletes a node this pass made that nothing reads yet, and its output nets (best effort). */
static void undo_cell(rd_state *rs, odin3_node_id node, uint32_t port) {
    if (!odin3_node_live(rs->m, node)) {
        return;
    }
    (void)port_nets(rs, node, port);
    (void)odin3_node_delete(rs->m, node);
    for (size_t i = 0; i < rs->tmp.len; i++) {
        odin3_net_id net = ((const odin3_net_id *)rs->tmp.data)[i];
        if (odin3_net_live(rs->m, net) && odin3_net_pins(rs->m, net).count == 0) {
            (void)odin3_net_delete(rs->m, net);
        }
    }
}

/* --- opt_mux_bits ----------------------------------------------------------------------------- */

typedef enum mb_result { MB_NONE, MB_NARROWED, MB_REMOVED } mb_result;

static bool tuple_uniform(const uint64_t *tuple, size_t count) {
    for (size_t k = 1; k < count; k++) {
        if (tuple[k] != tuple[0]) {
            return false;
        }
    }
    return true;
}

/* tup = per Y bit the tuple (A bit, then that bit of each B slice); rep/pos/kept = its groups. */
static odin3_status bits_plan(rd_state *rs) {
    const size_t width = rs->an.len;
    const size_t slices = rs->bn.len / width;
    const size_t tuple_len = slices + 1U;
    odin3_vec_clear(&rs->tup);
    odin3_status st = odin3_vec_reserve(&rs->tup, width * tuple_len);
    for (size_t i = 0; st == ODIN3_OK && i < width; i++) {
        st = push_u64(&rs->tup, ((const uint64_t *)rs->ak.data)[i]);
        for (size_t slice = 0; st == ODIN3_OK && slice < slices; slice++) {
            st = push_u64(&rs->tup, ((const uint64_t *)rs->bk.data)[slice * width + i]);
        }
    }
    const run_set set = {rs->tup.data, width, tuple_len};
    st = st == ODIN3_OK ? group_equal(rs, &set) : st;
    st = st == ODIN3_OK ? zero_u32(&rs->pos, width) : st;
    odin3_vec_clear(&rs->kept);
    for (size_t i = 0; st == ODIN3_OK && i < width; i++) {
        if (!tuple_uniform(run_at(&set, i), tuple_len) &&
            ((const uint32_t *)rs->rep.data)[i] == i) {
            *(uint32_t *)odin3_vec_at(&rs->pos, i) = (uint32_t)rs->kept.len;
            st = push_u32(&rs->kept, (uint32_t)i);
        }
    }
    return st;
}

/* new_a, new_b = A and B restricted to the kept bits (B slice after B slice). */
static odin3_status bits_inputs(rd_state *rs) {
    const size_t width = rs->an.len;
    const size_t slices = rs->bn.len / width;
    odin3_vec_clear(&rs->new_a);
    odin3_vec_clear(&rs->new_b);
    const uint32_t *kept = rs->kept.data;
    odin3_status st = ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < rs->kept.len; k++) {
        st = odin3_kit_push_net(&rs->new_a, ((const odin3_net_id *)rs->an.data)[kept[k]]);
    }
    for (size_t slice = 0; st == ODIN3_OK && slice < slices; slice++) {
        for (size_t k = 0; st == ODIN3_OK && k < rs->kept.len; k++) {
            const odin3_net_id *bn = rs->bn.data;
            st = odin3_kit_push_net(&rs->new_b, bn[slice * width + kept[k]]);
        }
    }
    return st;
}

/* srcs = for each old Y bit: A's net when its tuple is uniform, else the new Y net of its group. */
static odin3_status bits_sources(rd_state *rs, odin3_node_id fresh, size_t slices) {
    const size_t width = rs->an.len;
    odin3_status st = port_nets(rs, fresh, PORT_MUX_Y);
    odin3_vec_clear(&rs->srcs);
    for (size_t i = 0; st == ODIN3_OK && i < width; i++) {
        const uint64_t *tuple = (const uint64_t *)rs->tup.data + i * (slices + 1U);
        odin3_net_id net = ((const odin3_net_id *)rs->an.data)[i];
        if (!tuple_uniform(tuple, slices + 1U)) {
            uint32_t group = ((const uint32_t *)rs->rep.data)[i];
            net = ((const odin3_net_id *)rs->tmp.data)[((const uint32_t *)rs->pos.data)[group]];
        }
        st = odin3_kit_push_net(&rs->srcs, net);
    }
    return st;
}

/* The mux's parameters with WIDTH = width. */
static void mux_params(const rd_state *rs, odin3_node_id node, size_t width, odin3_value *params,
                       uint32_t *n_params) {
    const odin3_celltype_def *def =
        odin3_celltype_get(rs->oc->design, odin3_node_type(rs->m, node));
    *n_params = def->n_params;
    for (uint32_t i = 0; i < def->n_params && i < PRM_PMUX_N; i++) {
        params[i] = *odin3_node_param(rs->m, node, i);
    }
    params[0] = odin3_value_int((int64_t)width);
}

/* The new narrower cell, made but not yet wired to the readers (the build of the rewrite). */
static odin3_status bits_make(rd_state *rs, odin3_node_id old, odin3_node_id *out) {
    odin3_prov_id prov = {0};
    odin3_status st = odin3_kit_prov(rs->oc, &old, 1, &prov);
    st = st == ODIN3_OK ? bits_inputs(rs) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[PRM_PMUX_N];
    odin3_kit_cell cell;
    memset(&cell, 0, sizeof cell);
    mux_params(rs, old, rs->kept.len, params, &cell.n_params);
    const odin3_netvec ports[4] = {{rs->new_a.data, (uint32_t)rs->new_a.len},
                                   {rs->new_b.data, (uint32_t)rs->new_b.len},
                                   {rs->sn.data, (uint32_t)rs->sn.len},
                                   {NULL, 0}};
    cell.type = odin3_node_type(rs->m, old);
    cell.params = params;
    cell.ports = ports;
    cell.prov = prov;
    return odin3_kit_new_cell(rs->oc, &cell, out);
}

/* Marks the Y nets of fresh as feeding a write enable (the old Y nets were). */
static odin3_status wren_inherit(rd_state *rs, odin3_node_id fresh) {
    odin3_status st = port_nets(rs, fresh, PORT_MUX_Y);
    for (size_t i = 0; st == ODIN3_OK && i < rs->tmp.len; i++) {
        st = wren_mark(rs, ((const odin3_net_id *)rs->tmp.data)[i]);
    }
    return st;
}

/*
 * Narrows old to the kept bits: the narrow cell is built (with old's attributes), then old is
 * deleted and its Y nets merged into the new cell's (kit_connect), then the new cell takes the
 * name. Before kit_connect deletes old, any failure undoes the new cell.
 */
static odin3_status bits_narrow(rd_state *rs, odin3_node_id *node, size_t slices) {
    const odin3_node_id old = *node;
    const bool fed = feeds_wren(rs);
    odin3_kit_begin_rewrite(rs->oc);
    odin3_node_id fresh = {0};
    odin3_status st = bits_make(rs, old, &fresh);
    if (st != ODIN3_OK) {
        return st;
    }
    st = odin3_kit_copy_attrs(rs->m, old, fresh);
    st = st == ODIN3_OK ? bits_sources(rs, fresh, slices) : st;
    st = st == ODIN3_OK && fed ? wren_inherit(rs, fresh) : st;
    if (st == ODIN3_OK) {
        const uint32_t name = odin3_node_name(rs->m, old);
        const odin3_kit_conn conn = {old, PORT_MUX_Y, rs->srcs.data, (uint32_t)rs->srcs.len};
        st = odin3_kit_connect(rs->oc, &conn);
        if (!odin3_node_live(rs->m, old) && name != 0) {
            odin3_status named = odin3_node_rename(rs->m, fresh, name);
            st = st == ODIN3_OK ? named : st;
        }
    }
    if (st != ODIN3_OK && odin3_node_live(rs->m, old)) {
        undo_cell(rs, fresh, PORT_MUX_Y);
        return st;
    }
    *node = fresh;
    return st;
}

/* Y = A for every bit: the cell goes (Yosys: the tuples are all one repeated value). */
static odin3_status connect_to_a(rd_state *rs, odin3_node_id node) {
    odin3_kit_begin_rewrite(rs->oc);
    const odin3_kit_conn conn = {node, PORT_MUX_Y, rs->an.data, (uint32_t)rs->an.len};
    return odin3_kit_connect(rs->oc, &conn);
}

/* opt_mux_bits on the loaded mux: merges equal Y bits and wires the uniform ones. */
static odin3_status mux_bits(rd_state *rs, odin3_node_id *node, mb_result *res) {
    const size_t width = rs->an.len;
    const size_t slices = rs->bn.len / width;
    odin3_status st = bits_plan(rs);
    *res = MB_NONE;
    if (st != ODIN3_OK || rs->kept.len == width) {
        return st;
    }
    if (rs->kept.len == 0) {
        st = connect_to_a(rs, *node);
        *res = st == ODIN3_OK ? MB_REMOVED : MB_NONE;
    } else {
        st = bits_narrow(rs, node, slices);
        *res = st == ODIN3_OK ? MB_NARROWED : MB_NONE;
    }
    rs->count += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* --- opt_pmux --------------------------------------------------------------------------------- */

/* gid[j] = the group of select bit j in first-occurrence order, NONE_GROUP for a slice equal to A.
 */
static odin3_status slices_plan(rd_state *rs) {
    const size_t width = rs->an.len;
    const size_t count = rs->sn.len;
    const uint64_t *bk = rs->bk.data;
    const run_set set = {bk, count, width};
    odin3_status st = group_equal(rs, &set);
    st = st == ODIN3_OK ? zero_u32(&rs->gid, count) : st;
    rs->groups = 0;
    for (size_t j = 0; st == ODIN3_OK && j < count; j++) {
        const uint32_t rep = ((const uint32_t *)rs->rep.data)[j];
        uint32_t *gid = odin3_vec_at(&rs->gid, j);
        if (memcmp(bk + j * width, rs->ak.data, width * sizeof *bk) == 0) {
            *gid = NONE_GROUP;
        } else {
            *gid = rep == j ? rs->groups++ : ((const uint32_t *)rs->gid.data)[rep];
        }
    }
    return st;
}

/* gorder = the select bits of each group (group g: gorder[gstart[g] ..  gstart[g + 1])), in order.
 */
static odin3_status groups_order(rd_state *rs, size_t count) {
    odin3_status st = zero_u32(&rs->gstart, (size_t)rs->groups + 1U);
    st = st == ODIN3_OK ? zero_u32(&rs->fill, rs->groups) : st;
    st = st == ODIN3_OK ? zero_u32(&rs->gorder, count) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t *gstart = rs->gstart.data;
    const uint32_t *gid = rs->gid.data;
    for (size_t j = 0; j < count; j++) {
        if (gid[j] != NONE_GROUP) {
            gstart[gid[j] + 1U]++;
        }
    }
    for (uint32_t group = 0; group < rs->groups; group++) {
        gstart[group + 1U] += gstart[group];
    }
    uint32_t *fill = rs->fill.data;
    uint32_t *gorder = rs->gorder.data;
    for (size_t j = 0; j < count; j++) {
        if (gid[j] != NONE_GROUP) {
            gorder[gstart[gid[j]] + fill[gid[j]]++] = (uint32_t)j;
        }
    }
    return ODIN3_OK;
}

/* A $reduce_or over the select bits of group g; its Y net into *out. */
static odin3_status make_group_or(rd_state *rs, uint32_t group, odin3_prov_id prov,
                                  odin3_net_id *out) {
    const uint32_t *gstart = rs->gstart.data;
    const uint32_t *gorder = rs->gorder.data;
    const uint32_t count = gstart[group + 1U] - gstart[group];
    odin3_vec_clear(&rs->srcs);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        st = odin3_kit_push_net(&rs->srcs,
                                ((const odin3_net_id *)rs->sn.data)[gorder[gstart[group] + k]]);
    }
    made_cell *rec = st == ODIN3_OK ? odin3_vec_push(&rs->made) : NULL;
    if (st == ODIN3_OK && rec == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[PRM_RED_N] = {odin3_value_int(0), odin3_value_int(count),
                                     odin3_value_int(1)};
    const odin3_netvec ports[2] = {{rs->srcs.data, count}, {NULL, 0}};
    odin3_kit_cell cell = {rs->types[T_ROR], params, PRM_RED_N, ports, prov};
    odin3_node_id node = {0};
    st = odin3_kit_new_cell(rs->oc, &cell, &node);
    if (st != ODIN3_OK) {
        odin3_vec_pop(&rs->made);
        return st;
    }
    st = port_nets(rs, node, PORT_RED_Y);
    rec->node = node.v;
    rec->net = st == ODIN3_OK ? ((const odin3_net_id *)rs->tmp.data)[0].v : 0;
    *out = (odin3_net_id){rec->net};
    return st;
}

/* new_b, new_s = one B slice and one select bit per group; groups of several bits get a $reduce_or.
 */
static odin3_status groups_build(rd_state *rs, size_t width, odin3_prov_id prov) {
    odin3_vec_clear(&rs->new_b);
    odin3_vec_clear(&rs->new_s);
    odin3_vec_clear(&rs->made);
    odin3_status st = ODIN3_OK;
    for (uint32_t group = 0; st == ODIN3_OK && group < rs->groups; group++) {
        const uint32_t start = ((const uint32_t *)rs->gstart.data)[group];
        const uint32_t count = ((const uint32_t *)rs->gstart.data)[group + 1U] - start;
        const uint32_t first = ((const uint32_t *)rs->gorder.data)[start];
        for (size_t k = 0; st == ODIN3_OK && k < width; k++) {
            st = odin3_kit_push_net(&rs->new_b,
                                    ((const odin3_net_id *)rs->bn.data)[first * width + k]);
        }
        odin3_net_id sel = ((const odin3_net_id *)rs->sn.data)[first];
        st = st == ODIN3_OK && count > 1 ? make_group_or(rs, group, prov, &sel) : st;
        st = st == ODIN3_OK ? odin3_kit_push_net(&rs->new_s, sel) : st;
    }
    return st;
}

/* Deletes the $reduce_or cells of rs->made (best effort, after a failed rewrite). */
static void undo_made(rd_state *rs) {
    for (size_t i = 0; i < rs->made.len; i++) {
        const made_cell *rec = odin3_vec_cat(&rs->made, i);
        undo_cell(rs, (odin3_node_id){rec->node}, PORT_RED_Y);
    }
    odin3_vec_clear(&rs->made);
}

/* Rebuilds the mux as a $mux (one group) or a narrower $pmux. */
static odin3_status pmux_rebuild(rd_state *rs, odin3_node_id node, size_t width,
                                 odin3_prov_id prov) {
    odin3_value params[PRM_PMUX_N] = {odin3_value_int((int64_t)width),
                                      odin3_value_int((int64_t)rs->groups)};
    const bool single = rs->groups == 1;
    const odin3_netvec ports[4] = {{rs->an.data, (uint32_t)rs->an.len},
                                   {rs->new_b.data, (uint32_t)rs->new_b.len},
                                   {rs->new_s.data, (uint32_t)rs->new_s.len},
                                   {NULL, 0}};
    odin3_kit_rebuild req;
    memset(&req, 0, sizeof req);
    req.old_node = node;
    req.cell.type = rs->types[single ? T_MUX : T_PMUX];
    req.cell.params = params;
    req.cell.n_params = single ? 1U : PRM_PMUX_N;
    req.cell.ports = ports;
    req.cell.prov = prov;
    odin3_node_id fresh = {0};
    return odin3_kit_rebuild_node(rs->oc, &req, &fresh);
}

static odin3_status pmux_rewrite(rd_state *rs, odin3_node_id node) {
    odin3_kit_begin_rewrite(rs->oc);
    odin3_prov_id prov = {0};
    const size_t width = rs->an.len;
    odin3_status st = odin3_kit_prov(rs->oc, &node, 1, &prov);
    st = st == ODIN3_OK ? groups_order(rs, rs->sn.len) : st;
    st = st == ODIN3_OK ? groups_build(rs, width, prov) : st;
    st = st == ODIN3_OK ? pmux_rebuild(rs, node, width, prov) : st;
    if (st != ODIN3_OK && odin3_node_live(rs->m, node)) {
        undo_made(rs);
    }
    rs->count += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* opt_pmux on the loaded $mux or $pmux. */
static odin3_status pmux_run(rd_state *rs, odin3_node_id node) {
    const size_t count = rs->sn.len;
    const bool is_pmux = is_type(rs, node, T_PMUX);
    odin3_status st = slices_plan(rs);
    if (st != ODIN3_OK) {
        return st;
    }
    if (rs->groups == 0) {
        st = connect_to_a(rs, node);
        rs->count += st == ODIN3_OK ? 1U : 0U;
        return st;
    }
    if ((rs->groups == count && !(rs->groups == 1 && is_pmux)) || rs->types[T_ROR].v == 0 ||
        rs->types[T_MUX].v == 0) {
        return ODIN3_OK;
    }
    return pmux_rewrite(rs, node);
}

/* --- the muxes, one round --------------------------------------------------------------------- */

static odin3_status mux_cell(rd_state *rs, odin3_node_id node) {
    gather_status gs = mux_load(rs, node);
    if (gs != GATHER_OK) {
        return gs == GATHER_OOM ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    }
    if (rs->oc->opts->fine || feeds_wren(rs)) {
        mb_result res = MB_NONE;
        odin3_status st = mux_bits(rs, &node, &res);
        if (st != ODIN3_OK || res == MB_REMOVED) {
            return st;
        }
        if (res == MB_NARROWED && mux_load(rs, node) != GATHER_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return pmux_run(rs, node);
}

static odin3_status mux_phase(rd_state *rs) {
    const uint32_t end = odin3_module_node_end(rs->m);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (is_type(rs, node, T_MUX) || is_type(rs, node, T_PMUX)) {
            st = mux_cell(rs, node);
        }
    }
    return st;
}

/* --- one module, the pass --------------------------------------------------------------------- */

static odin3_status reduce_round(rd_state *rs) {
    odin3_status st = reduce_phase(rs, T_ROR);
    st = st == ODIN3_OK ? reduce_phase(rs, T_RAND) : st;
    return st == ODIN3_OK ? mux_phase(rs) : st;
}

/* Yosys's worker: rounds until one changes nothing; the outer loop re-reads the write enables. */
static odin3_status module_changes(rd_state *rs) {
    odin3_status st = ODIN3_OK;
    uint32_t worker = 0;
    do {
        worker = rs->count;
        st = wren_build(rs);
        uint32_t round = 0;
        do {
            round = rs->count;
            st = st == ODIN3_OK ? reduce_round(rs) : st;
        } while (st == ODIN3_OK && rs->count != round);
    } while (st == ODIN3_OK && rs->count != worker && rs->any_wren);
    return st;
}

static odin3_status reduce_module(odin3_opt_ctx *oc, uint32_t *total) {
    rd_state rs;
    state_init(&rs, oc);
    odin3_status st = odin3_opt_canon_consts(oc);
    st = st == ODIN3_OK ? module_changes(&rs) : st;
    *total += rs.count;
    state_free(&rs);
    return st;
}

odin3_status odin3_opt_reduce_run(odin3_pass_ctx *ctx, odin3_design *design,
                                  const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_reduce: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t total = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = reduce_module(&oc, &total);
        odin3_opt_ctx_free(&oc);
    }
    counts->reduce_changes += total;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "opt_reduce: performed %" PRIu32 " changes", total);
    }
    return st;
}
