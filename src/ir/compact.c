/* compact.c — renumbers a module's live objects densely in ID order and frees dead slots (IR-6). */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Compact builds a complete new set of containers from the live objects while the module stays
 * untouched, reserves the tombstones and copies the dead nodes' tombstone arrays (parameters, pin
 * net names) into the design, and only then (nothing left that can fail) appends the tombstones,
 * swaps the new containers in, drops the dead-pin table and frees the old containers. On out of
 * memory the new containers are freed and the module is as it was (copied tombstone arrays stay
 * unused in the design's arena).
 */

/* The containers compact fills; swapped into the module on success, freed on failure. */
typedef struct compact_out {
    odin3_arena *arena;
    odin3_pagevec *nodes;
    odin3_pagevec *pins;
    odin3_pagevec *nets;
    odin3_pagevec *wires;
    odin3_u64map *node_names;
    odin3_u64map *net_names;
    odin3_u64map *wire_names;
    odin3_u64map *attr_heads;
    odin3_pinpool pinpool;
    odin3_vec aliases;
    odin3_vec attrs;
} compact_out;

typedef struct compact_ctx {
    odin3_module *module; /* the old IR, read only until the commit */
    odin3_compact_map map;
    compact_out out;
    uint32_t live_nodes, live_pins, live_nets, live_wires;
    uint32_t dead;               /* tombstones to write: dead nodes, nets and wires */
    uint32_t dead_nodes;         /* of which dead nodes */
    odin3_tombstone *node_tombs; /* dead_nodes tombstones in ID order, arrays owned by the design */
} compact_ctx;

/* --- numbering ----------------------------------------------------------------------------- */

static uint32_t *id_array(uint32_t count) {
    return odin3_util_calloc(sizeof(uint32_t) * (size_t)count);
}

static odin3_status maps_alloc(compact_ctx *ctx) {
    odin3_compact_map *map = &ctx->map;
    map->n_node = odin3_module_node_end(ctx->module);
    map->n_pin = odin3_module_pin_end(ctx->module);
    map->n_net = odin3_module_net_end(ctx->module);
    map->n_wire = odin3_module_wire_end(ctx->module);
    map->node = id_array(map->n_node);
    map->pin = id_array(map->n_pin);
    map->net = id_array(map->n_net);
    map->wire = id_array(map->n_wire);
    bool ok = map->node != NULL && map->pin != NULL && map->net != NULL && map->wire != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* New IDs in old ID order; a live node's pins (contiguous, ascending with node IDs) follow it. */
static void number_nodes(compact_ctx *ctx) {
    const odin3_module *module = ctx->module;
    for (uint32_t i = 1; i < ctx->map.n_node; i++) {
        const odin3_node_rec *rec = odin3_node_rec_cat(module, (odin3_node_id){i});
        if (rec->dead) {
            ctx->dead++;
            ctx->dead_nodes++;
            continue;
        }
        ctx->map.node[i] = ++ctx->live_nodes;
        for (uint32_t k = 0; k < rec->pin_count; k++) {
            ctx->map.pin[rec->first_pin.v + k] = ++ctx->live_pins;
        }
    }
}

static void number_nets_wires(compact_ctx *ctx) {
    const odin3_module *module = ctx->module;
    for (uint32_t i = 1; i < ctx->map.n_net; i++) {
        if (odin3_net_rec_cat(module, (odin3_net_id){i})->dead) {
            ctx->dead++;
        } else {
            ctx->map.net[i] = ++ctx->live_nets;
        }
    }
    for (uint32_t i = 1; i < ctx->map.n_wire; i++) {
        if (odin3_wire_rec_cat(module, (odin3_wire_id){i})->dead) {
            ctx->dead++;
        } else {
            ctx->map.wire[i] = ++ctx->live_wires;
        }
    }
}

/* --- new containers ------------------------------------------------------------------------ */

/* A store for `live` records plus the dummy at index 0, all reserved; NULL on out of memory. */
static odin3_pagevec *store_new(odin3_pagevec_spec spec, uint32_t live) {
    odin3_pagevec *store = odin3_pagevec_create_paged(spec);
    if (store != NULL && (odin3_pagevec_reserve(store, (size_t)live + 1) != ODIN3_OK ||
                          odin3_pagevec_push(store, NULL) == NULL)) {
        odin3_pagevec_destroy(store);
        return NULL;
    }
    return store;
}

static void out_free(compact_out *out) {
    odin3_arena_destroy(out->arena);
    odin3_pagevec_destroy(out->nodes);
    odin3_pagevec_destroy(out->pins);
    odin3_pagevec_destroy(out->nets);
    odin3_pagevec_destroy(out->wires);
    odin3_u64map_destroy(out->node_names);
    odin3_u64map_destroy(out->net_names);
    odin3_u64map_destroy(out->wire_names);
    odin3_u64map_destroy(out->attr_heads);
    odin3_pinpool_destroy(&out->pinpool);
    odin3_vec_free(&out->aliases);
    odin3_vec_free(&out->attrs);
}

static odin3_status out_alloc(compact_ctx *ctx) {
    const odin3_module *module = ctx->module;
    compact_out *out = &ctx->out;
    odin3_vec_init(&out->aliases, sizeof(odin3_alias_rec));
    odin3_vec_init(&out->attrs, sizeof(odin3_attr_rec));
    out->arena = odin3_arena_create(ODIN3_MODULE_ARENA_CHUNK_BYTES);
    out->nodes = store_new((odin3_pagevec_spec){sizeof(odin3_node_rec), ODIN3_MODULE_PAGE_SHIFT},
                           ctx->live_nodes);
    out->pins = store_new((odin3_pagevec_spec){sizeof(odin3_pin_rec), ODIN3_MODULE_PAGE_SHIFT},
                          ctx->live_pins);
    out->nets = store_new((odin3_pagevec_spec){sizeof(odin3_net_rec), ODIN3_MODULE_PAGE_SHIFT},
                          ctx->live_nets);
    out->wires = store_new((odin3_pagevec_spec){sizeof(odin3_wire_rec), ODIN3_WIRE_PAGE_SHIFT},
                           ctx->live_wires);
    out->node_names = odin3_u64map_create(odin3_u64map_count(module->node_names));
    out->net_names = odin3_u64map_create(odin3_u64map_count(module->net_names));
    out->wire_names = odin3_u64map_create(odin3_u64map_count(module->wire_names));
    out->attr_heads = odin3_u64map_create(odin3_u64map_count(module->attr_heads));
    if (out->arena == NULL || out->nodes == NULL || out->pins == NULL || out->nets == NULL ||
        out->wires == NULL || out->node_names == NULL || out->net_names == NULL ||
        out->wire_names == NULL || out->attr_heads == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return odin3_pinpool_init(&out->pinpool);
}

/* --- copying the live objects -------------------------------------------------------------- */

/* Copies the node's parameter values (and their payloads) into the new arena. */
static odin3_status copy_params(odin3_arena *arena, const odin3_node_rec *old,
                                odin3_node_rec *rec) {
    rec->params = NULL;
    if (old->n_params == 0) {
        return ODIN3_OK;
    }
    odin3_value *vals = odin3_arena_alloc(arena, sizeof *vals * old->n_params);
    if (vals == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < old->n_params; i++) {
        odin3_status st = odin3_value_copy(arena, &old->params[i], &vals[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    rec->params = vals;
    return ODIN3_OK;
}

/* The node's pins, in order, after the pins of the live nodes before it. */
static void copy_pins(compact_ctx *ctx, const odin3_node_rec *old, odin3_node_id node) {
    for (uint32_t k = 0; k < old->pin_count; k++) {
        const odin3_pin_rec *pin =
            odin3_pin_rec_cat(ctx->module, (odin3_pin_id){old->first_pin.v + k});
        odin3_pin_rec *rec = odin3_pagevec_push(ctx->out.pins, NULL);
        assert(rec != NULL); /* reserved */
        *rec = *pin;
        rec->node = node;
        rec->net.v = ctx->map.net[pin->net.v]; /* slot unchanged: the pin array keeps its order */
    }
}

static odin3_status copy_nodes(compact_ctx *ctx) {
    for (uint32_t i = 1; i < ctx->map.n_node; i++) {
        if (ctx->map.node[i] == 0) {
            continue;
        }
        const odin3_node_rec *old = odin3_node_rec_cat(ctx->module, (odin3_node_id){i});
        odin3_node_rec *rec = odin3_pagevec_push(ctx->out.nodes, NULL);
        assert(rec != NULL); /* reserved */
        *rec = *old;
        rec->first_pin.v = (uint32_t)odin3_pagevec_len(ctx->out.pins);
        odin3_status st = copy_params(ctx->out.arena, old, rec);
        if (st != ODIN3_OK) {
            return st;
        }
        copy_pins(ctx, old, (odin3_node_id){ctx->map.node[i]});
    }
    return ODIN3_OK;
}

/* The smallest pool class that holds count pins (count fits the old block, so one exists). */
static uint32_t pin_class(uint32_t count) {
    uint32_t cls = 0;
    while (odin3_pinpool_capacity(cls) < count) {
        cls++;
    }
    return cls;
}

/* A new net record: same pins in the same order (so the partition and every slot hold). */
static odin3_status copy_net(compact_ctx *ctx, const odin3_net_rec *old, odin3_net_rec *rec) {
    *rec = *old;
    rec->pins = NULL;
    rec->cls = 0;
    rec->alias_head = 0; /* set when the aliases are copied */
    rec->wire.v = ctx->map.wire[old->wire.v];
    if (!odin3_wire_valid(rec->wire)) { /* none, or a membership of a dead wire */
        rec->wire_bit = 0;
    }
    if (old->count == 0) {
        return ODIN3_OK;
    }
    uint32_t cls = pin_class(old->count);
    odin3_pin_id *block = odin3_pinpool_alloc(&ctx->out.pinpool, cls);
    if (block == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < old->count; k++) {
        block[k].v = ctx->map.pin[old->pins[k].v];
    }
    rec->pins = block;
    rec->cls = (uint8_t)cls;
    return ODIN3_OK;
}

static odin3_status copy_nets(compact_ctx *ctx) {
    for (uint32_t i = 1; i < ctx->map.n_net; i++) {
        if (ctx->map.net[i] == 0) {
            continue;
        }
        odin3_net_rec *rec = odin3_pagevec_push(ctx->out.nets, NULL);
        assert(rec != NULL); /* reserved */
        odin3_status st = copy_net(ctx, odin3_net_rec_cat(ctx->module, (odin3_net_id){i}), rec);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status copy_wires(compact_ctx *ctx) {
    for (uint32_t i = 1; i < ctx->map.n_wire; i++) {
        if (ctx->map.wire[i] == 0) {
            continue;
        }
        const odin3_wire_rec *old = odin3_wire_rec_cat(ctx->module, (odin3_wire_id){i});
        odin3_wire_rec *rec = odin3_pagevec_push(ctx->out.wires, NULL);
        assert(rec != NULL); /* reserved */
        *rec = *old;
        rec->port_node.v = ctx->map.node[old->port_node.v];
        rec->nets = odin3_arena_alloc(ctx->out.arena, sizeof(odin3_net_id) * (size_t)old->width);
        if (rec->nets == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t k = 0; k < old->width; k++) {
            rec->nets[k].v = ctx->map.net[old->nets[k].v];
        }
    }
    return ODIN3_OK;
}

/* --- aliases ------------------------------------------------------------------------------- */

/* Appends a record to the new alias table (taking the dummy slot 0 first); NULL on OOM. */
static odin3_alias_rec *alias_push(odin3_vec *aliases) {
    if (aliases->len == 0 && odin3_vec_push(aliases) == NULL) {
        return NULL;
    }
    return aliases->len < UINT32_MAX ? odin3_vec_push(aliases) : NULL;
}

/*
 * Copies the chain of old net old_id (oldest first) to new net `net`, dropping memberships of dead
 * wires. A net's new records are consecutive, so the circular chain is first -> ... -> last ->
 * first and the head (the newest record) is the last.
 */
static odin3_status copy_chain(compact_ctx *ctx, uint32_t old_id, odin3_net_id net) {
    const odin3_net_rec *old = odin3_net_rec_cat(ctx->module, (odin3_net_id){old_id});
    const odin3_vec *from = &ctx->module->aliases;
    odin3_vec *to = &ctx->out.aliases;
    uint32_t first = 0;
    uint32_t idx = old->alias_head;
    do {
        idx = ((const odin3_alias_rec *)odin3_vec_cat(from, idx))->next;
        const odin3_alias_rec *alias = odin3_vec_cat(from, idx);
        if (odin3_wire_valid(alias->wire) && ctx->map.wire[alias->wire.v] == 0) {
            continue;
        }
        odin3_alias_rec *rec = alias_push(to);
        if (rec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        uint32_t at = (uint32_t)to->len - 1;
        *rec = (odin3_alias_rec){net, {ctx->map.wire[alias->wire.v]}, alias->bit, alias->name, 0};
        first = first == 0 ? at : first;
    } while (idx != old->alias_head);
    if (first == 0) {
        return ODIN3_OK;
    }
    uint32_t last = (uint32_t)to->len - 1;
    for (uint32_t k = first; k < last; k++) {
        ((odin3_alias_rec *)odin3_vec_at(to, k))->next = k + 1;
    }
    ((odin3_alias_rec *)odin3_vec_at(to, last))->next = first;
    ((odin3_net_rec *)odin3_pagevec_at(ctx->out.nets, net.v))->alias_head = last;
    return ODIN3_OK;
}

static odin3_status copy_aliases(compact_ctx *ctx) {
    for (uint32_t i = 1; i < ctx->map.n_net; i++) {
        uint32_t net = ctx->map.net[i];
        if (net == 0 || odin3_net_rec_cat(ctx->module, (odin3_net_id){i})->alias_head == 0) {
            continue;
        }
        odin3_status st = copy_chain(ctx, i, (odin3_net_id){net});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- names --------------------------------------------------------------------------------- */

/* One name map to rebuild: every entry of from, its object ID mapped through ids, into to. */
typedef struct name_remap {
    const odin3_u64map *from;
    odin3_u64map *to;
    const uint32_t *ids;
    uint32_t n_ids;
} name_remap;

static odin3_status remap_names(name_remap remap) {
    size_t cursor = 0;
    odin3_kv entry;
    while (odin3_u64map_next(remap.from, &cursor, &entry)) {
        uint32_t id = entry.value < remap.n_ids ? remap.ids[entry.value] : 0;
        if (id == 0) {
            continue; /* never for a consistent IR: maps hold live objects only */
        }
        odin3_status st = odin3_u64map_put(remap.to, (odin3_kv){entry.key, id});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status copy_names(compact_ctx *ctx) {
    const odin3_module *module = ctx->module;
    const odin3_compact_map *map = &ctx->map;
    odin3_status st =
        remap_names((name_remap){module->node_names, ctx->out.node_names, map->node, map->n_node});
    if (st == ODIN3_OK) {
        st = remap_names((name_remap){module->net_names, ctx->out.net_names, map->net, map->n_net});
    }
    if (st == ODIN3_OK) {
        st = remap_names(
            (name_remap){module->wire_names, ctx->out.wire_names, map->wire, map->n_wire});
    }
    return st;
}

/* --- attributes ---------------------------------------------------------------------------- */

/* Copies the attribute chain of old object `obj` (current values only) to new ID new_id. */
static odin3_status copy_attr_chain(compact_ctx *ctx, odin3_objref obj, uint32_t new_id) {
    uint64_t head = 0;
    if (!odin3_u64map_get(ctx->module->attr_heads, odin3_attr_key(obj), &head)) {
        return ODIN3_OK;
    }
    odin3_vec *to = &ctx->out.attrs;
    if (to->len == 0 && odin3_vec_push(to) == NULL) { /* the dummy slot 0 */
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t first = (uint32_t)to->len;
    for (uint32_t idx = (uint32_t)head; idx != 0;) {
        const odin3_attr_rec *old = odin3_vec_cat(&ctx->module->attrs, idx);
        odin3_value *copy = odin3_arena_alloc(ctx->out.arena, sizeof *copy);
        if (copy == NULL || odin3_value_copy(ctx->out.arena, old->value, copy) != ODIN3_OK ||
            to->len >= UINT32_MAX) {
            return ODIN3_ERR_NO_MEMORY;
        }
        odin3_attr_rec *rec = odin3_vec_push(to);
        if (rec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *rec = (odin3_attr_rec){old->key, 0, copy};
        idx = old->next;
        if (idx != 0) {
            rec->next = (uint32_t)to->len; /* the next record lands right after this one */
        }
    }
    odin3_objref now = {obj.kind, new_id};
    return odin3_u64map_put(ctx->out.attr_heads, (odin3_kv){odin3_attr_key(now), first});
}

/* The attributes of every live object of one kind, in ID order. */
static odin3_status copy_attrs_of(compact_ctx *ctx, odin3_objkind kind, const uint32_t *ids,
                                  uint32_t n_ids) {
    for (uint32_t i = 1; i < n_ids; i++) {
        if (ids[i] == 0) {
            continue;
        }
        odin3_status st = copy_attr_chain(ctx, (odin3_objref){kind, i}, ids[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status copy_attrs(compact_ctx *ctx) {
    const odin3_compact_map *map = &ctx->map;
    if (odin3_u64map_count(ctx->module->attr_heads) == 0) {
        return ODIN3_OK;
    }
    odin3_status st = copy_attrs_of(ctx, ODIN3_OBJ_NODE, map->node, map->n_node);
    if (st == ODIN3_OK) {
        st = copy_attrs_of(ctx, ODIN3_OBJ_NET, map->net, map->n_net);
    }
    if (st == ODIN3_OK) {
        st = copy_attrs_of(ctx, ODIN3_OBJ_WIRE, map->wire, map->n_wire);
    }
    if (st == ODIN3_OK) {
        uint32_t self = ctx->module->id.v;
        st = copy_attr_chain(ctx, (odin3_objref){ODIN3_OBJ_MODULE, self}, self);
    }
    return st;
}

/* --- tombstones and commit ----------------------------------------------------------------- */

/* Room for every tombstone, so the commit's odin3_tombstone_add calls cannot fail. */
static odin3_status tombstones_reserve(const compact_ctx *ctx) {
    odin3_vec *tombs = &ctx->module->design->prov->tombstones;
    if ((uint64_t)tombs->len + ctx->dead > UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return odin3_vec_reserve(tombs, tombs->len + ctx->dead);
}

/* The tombstone of a dead net or wire (no type, no arrays). */
static odin3_tombstone tombstone_of(const odin3_module *module, odin3_objkind kind, uint32_t name,
                                    odin3_prov_id prov) {
    return (odin3_tombstone){module->id, kind, {0}, name, prov, NULL, 0, NULL, 0};
}

/* The pin net names recorded when node `id` was deleted (none if nothing was recorded). */
static void tombstone_pin_nets(const odin3_module *module, uint32_t id, odin3_tombstone *tomb) {
    uint64_t first = 0;
    if (module->dead_pins != NULL && odin3_u64map_get(module->dead_pins, id, &first)) {
        tomb->pin_nets = odin3_vec_cat(&module->dead_pin_names, (size_t)first);
        tomb->n_pins = odin3_node_rec_cat(module, (odin3_node_id){id})->pin_count;
    }
}

/* Every dead node's tombstone, its parameter values and pin net names copied into the design. */
static odin3_status tombstones_stage(compact_ctx *ctx) {
    if (ctx->dead_nodes == 0) {
        return ODIN3_OK;
    }
    ctx->node_tombs = odin3_util_calloc(sizeof *ctx->node_tombs * ctx->dead_nodes);
    if (ctx->node_tombs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_module *module = ctx->module;
    uint32_t k = 0;
    for (uint32_t i = 1; i < ctx->map.n_node; i++) {
        const odin3_node_rec *rec = odin3_node_rec_cat(module, (odin3_node_id){i});
        if (!rec->dead) {
            continue;
        }
        odin3_tombstone *tomb = &ctx->node_tombs[k++];
        *tomb = (odin3_tombstone){module->id,  ODIN3_OBJ_NODE, rec->type, rec->name, rec->prov,
                                  rec->params, rec->n_params,  NULL,      0};
        tombstone_pin_nets(module, i, tomb);
        odin3_status st = odin3_tombstone_own(module->design, tomb);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static void tombstone_write(const odin3_module *module, odin3_tombstone tomb) {
    if (odin3_prov_get(module->design, tomb.prov) == NULL) {
        tomb.prov = (odin3_prov_id){0}; /* not a record (check rule 7): keep the rest */
    }
    odin3_status st = odin3_tombstone_append(module->design, &tomb);
    assert(st == ODIN3_OK); /* valid, owned and reserved */
    (void)st;
}

/* One tombstone per dead node (staged), then net, then wire, each in ID order (IR-6). */
static void tombstones_write(const compact_ctx *ctx) {
    const odin3_module *module = ctx->module;
    for (uint32_t k = 0; k < ctx->dead_nodes; k++) {
        tombstone_write(module, ctx->node_tombs[k]);
    }
    for (uint32_t i = 1; i < ctx->map.n_net; i++) {
        const odin3_net_rec *rec = odin3_net_rec_cat(module, (odin3_net_id){i});
        if (rec->dead) {
            tombstone_write(module, tombstone_of(module, ODIN3_OBJ_NET, rec->name, rec->prov));
        }
    }
    for (uint32_t i = 1; i < ctx->map.n_wire; i++) {
        const odin3_wire_rec *rec = odin3_wire_rec_cat(module, (odin3_wire_id){i});
        if (rec->dead) {
            tombstone_write(module, tombstone_of(module, ODIN3_OBJ_WIRE, rec->name, rec->prov));
        }
    }
}

/* Swaps the new containers in; the old ones end up in ctx->out, to be freed. */
static void swap_in(compact_ctx *ctx) {
    odin3_module *module = ctx->module;
    compact_out old = {module->arena,     module->nodes,      module->pins,
                       module->nets,      module->wires,      module->node_names,
                       module->net_names, module->wire_names, module->attr_heads,
                       module->pinpool,   module->aliases,    module->attrs};
    const compact_out *now = &ctx->out;
    module->arena = now->arena;
    module->nodes = now->nodes;
    module->pins = now->pins;
    module->nets = now->nets;
    module->wires = now->wires;
    module->node_names = now->node_names;
    module->net_names = now->net_names;
    module->wire_names = now->wire_names;
    module->attr_heads = now->attr_heads;
    module->pinpool = now->pinpool;
    module->aliases = now->aliases;
    module->attrs = now->attrs;
    ctx->out = old;
}

static void commit(compact_ctx *ctx) {
    tombstones_write(ctx); /* reads the old records: before anything is freed */
    odin3_module *module = ctx->module;
    for (size_t k = 0; k < module->ports.len; k++) {
        odin3_port_rec *port = odin3_vec_at(&module->ports, k);
        port->node.v = ctx->map.node[port->node.v];
        port->wire.v = ctx->map.wire[port->wire.v];
    }
    swap_in(ctx);
    out_free(&ctx->out);
    odin3_module_dead_pins_free(module); /* every dead node is gone */
}

/* Every fallible step: numbering, the new containers, the copies, the tombstone room. */
static odin3_status build(compact_ctx *ctx) {
    odin3_status st = maps_alloc(ctx);
    if (st != ODIN3_OK) {
        return st;
    }
    number_nodes(ctx);
    number_nets_wires(ctx);
    st = out_alloc(ctx);
    if (st == ODIN3_OK) {
        st = copy_nodes(ctx);
    }
    if (st == ODIN3_OK) {
        st = copy_nets(ctx);
    }
    if (st == ODIN3_OK) {
        st = copy_wires(ctx);
    }
    if (st == ODIN3_OK) {
        st = copy_aliases(ctx);
    }
    if (st == ODIN3_OK) {
        st = copy_names(ctx);
    }
    if (st == ODIN3_OK) {
        st = copy_attrs(ctx);
    }
    if (st == ODIN3_OK) {
        st = tombstones_reserve(ctx);
    }
    return st == ODIN3_OK ? tombstones_stage(ctx) : st;
}

odin3_status odin3_module_compact(odin3_module *module, odin3_compact_map *map) {
    if (map != NULL) {
        memset(map, 0, sizeof *map);
    }
    if (module == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "module_compact: no module");
        return ODIN3_ERR_INVALID_ARG;
    }
    compact_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.module = module;
    odin3_status st = build(&ctx);
    if (st != ODIN3_OK) {
        out_free(&ctx.out);
        odin3_compact_map_free(&ctx.map);
        odin3_util_free(ctx.node_tombs);
        return st;
    }
    commit(&ctx);
    odin3_util_free(ctx.node_tombs);
    if (map != NULL) {
        *map = ctx.map;
    } else {
        odin3_compact_map_free(&ctx.map);
    }
    return ODIN3_OK;
}

void odin3_compact_map_free(odin3_compact_map *map) {
    if (map == NULL) {
        return;
    }
    odin3_util_free(map->node);
    odin3_util_free(map->net);
    odin3_util_free(map->wire);
    odin3_util_free(map->pin);
    memset(map, 0, sizeof *map);
}
