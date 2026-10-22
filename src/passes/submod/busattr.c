/*
 * busattr.c — vector ports for `submod -bus BUSKEY` (v1.0.5 spec 4): the boundary nets of a group
 * whose attribute BUSKEY has the text `NAME[BIT]` are collected per (NAME, direction); a set whose
 * bits are exactly 0 .. W-1, each on one net, is one bus (spec B-1), any other set stays scalar
 * with one warning (B-3). A text of another form is a located error (B-4).
 */
#include "ir/module.h"
#include "passes/submod/submod_int.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum {
    KEY_NAME_SHIFT = 32,
    KEY_DIR_SHIFT = 31,
    BUS_MAX_BIT = INT32_MAX,
    DECIMAL_BASE = 10,
    LABEL_BYTES = 32
};

/* --- the text ------------------------------------------------------------------------------ */

bool odin3_submod_bus_parse(const char *text, size_t *name_len, uint32_t *bit) {
    const char *open = strchr(text, '[');
    if (open == NULL || open == text || text[0] == '$') {
        return false;
    }
    size_t len = (size_t)(open - text);
    if (memchr(text, ']', len) != NULL) {
        return false;
    }
    uint64_t value = 0;
    const char *at = open + 1;
    for (; *at >= '0' && *at <= '9'; at++) {
        value = value * DECIMAL_BASE + (uint64_t)(*at - '0');
        if (value > BUS_MAX_BIT) {
            return false;
        }
    }
    if (at == open + 1 || at[0] != ']' || at[1] != '\0') {
        return false;
    }
    *name_len = len;
    *bit = (uint32_t)value;
    return true;
}

/* --- the entries --------------------------------------------------------------------------- */

typedef struct bus_entry {
    uint32_t name;
    uint32_t bit;
    uint32_t net;
    uint8_t dir;
} bus_entry;

/* The sort key of an entry: name, direction, bit (the net ID breaks ties). */
static uint64_t entry_key(const bus_entry *entry) {
    return ((uint64_t)entry->name << KEY_NAME_SHIFT) | ((uint64_t)entry->dir << KEY_DIR_SHIFT) |
           entry->bit;
}

/* Shell sort by (name, dir, bit, net): iterative, no recursion, no helper with two like params. */
static void sort_entries(bus_entry *items, size_t count) {
    for (size_t gap = count / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < count; i++) {
            bus_entry held = items[i];
            uint64_t key = entry_key(&held);
            size_t at = i;
            while (at >= gap &&
                   (entry_key(&items[at - gap]) > key ||
                    (entry_key(&items[at - gap]) == key && items[at - gap].net > held.net))) {
                items[at] = items[at - gap];
                at -= gap;
            }
            items[at] = held;
        }
    }
}

void odin3_submod_busset_init(odin3_submod_busset *set) {
    odin3_vec_init(&set->buses, sizeof(odin3_submod_bus));
    odin3_vec_init(&set->nets, sizeof(odin3_net_id));
    odin3_vec_init(&set->of_net, sizeof(uint32_t));
    set->quiet = false;
}

void odin3_submod_busset_free(odin3_submod_busset *set) {
    odin3_vec_free(&set->buses);
    odin3_vec_free(&set->nets);
    odin3_vec_free(&set->of_net);
}

static const char *net_label(const odin3_module *module, odin3_net_id net, char *buf) {
    uint32_t name = odin3_net_name(module, net);
    if (name != 0) {
        return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), name);
    }
    (void)snprintf(buf, LABEL_BYTES, "$n%" PRIu32, net.v);
    return buf;
}

/* The group and the attribute of an error message. */
typedef struct bus_ctx {
    const odin3_module *module;
    const char *key;
    const char *group;
    bool quiet;
} bus_ctx;

static odin3_status bad_text(const bus_ctx *ctx, odin3_net_id net, const odin3_value *val) {
    char buf[LABEL_BYTES];
    odin3_design *design = odin3_module_design(ctx->module);
    const char *text = val->kind == ODIN3_VAL_STRING
                           ? odin3_strtab_get(odin3_design_strtab(design), val->str)
                           : "";
    return odin3_submod_error(design, odin3_net_prov(ctx->module, net),
                              "submod: group '%s': attribute '%s' of net '%s' is not NAME[BIT]: "
                              "'%s'",
                              ctx->group, ctx->key, net_label(ctx->module, net, buf),
                              val->kind == ODIN3_VAL_STRING ? text : "(not a string)");
}

/* Parses one net's attribute value into an entry pushed on entries. */
static odin3_status add_entry(const bus_ctx *ctx, odin3_net_id net, uint8_t dir,
                              const odin3_value *val, odin3_vec *entries) {
    odin3_design *design = odin3_module_design(ctx->module);
    size_t len = 0;
    uint32_t bit = 0;
    const char *text = val->kind == ODIN3_VAL_STRING
                           ? odin3_strtab_get(odin3_design_strtab(design), val->str)
                           : "";
    if (val->kind != ODIN3_VAL_STRING || !odin3_submod_bus_parse(text, &len, &bit)) {
        return bad_text(ctx, net, val);
    }
    odin3_strbuf name;
    odin3_strbuf_init(&name);
    uint32_t id = 0;
    odin3_status st = odin3_strbuf_append(&name, (odin3_bytes){(const uint8_t *)text, len});
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(name.data), &id) : st;
    odin3_strbuf_free(&name);
    bus_entry *slot = st == ODIN3_OK ? odin3_vec_push(entries) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = (bus_entry){id, bit, net.v, dir};
    }
    return st;
}

/* The attribute of a net; when it has none, that of its primary wire if the wire has one bit (a
 * JSON netname's attributes land on its wire, so this is how the CLI can tag a net). */
static const odin3_value *net_value(const odin3_module *module, odin3_net_id net,
                                    uint32_t key_str) {
    const odin3_value *val = odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_NET, net.v}, key_str);
    odin3_wire_id wire = odin3_net_primary(module, net).wire;
    if (val == NULL && odin3_wire_valid(wire) && odin3_wire_width(module, wire) == 1) {
        val = odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, key_str);
    }
    return val;
}

/* Every tagged boundary net of the module as an entry, in net ID order. */
static odin3_status collect_entries(const bus_ctx *ctx, const odin3_vec *state, uint32_t key_str,
                                    odin3_vec *entries) {
    const uint8_t *dirs = state->data;
    for (uint32_t nid = 1; nid < odin3_module_net_end(ctx->module) && nid < state->len; nid++) {
        if (dirs[nid] != SM_IN && dirs[nid] != SM_OUT) {
            continue;
        }
        const odin3_value *val = net_value(ctx->module, (odin3_net_id){nid}, key_str);
        odin3_status st =
            val != NULL ? add_entry(ctx, (odin3_net_id){nid}, dirs[nid], val, entries) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the sets ------------------------------------------------------------------------------ */

/* Why a sorted run of entries is no bus: the message tail, empty when it is one. The bits must
 * be exactly 0 .. n-1; mixed says the name also has a run of the other direction. */
static const char *run_problem(const bus_entry *run, size_t count, bool mixed, char *buf) {
    if (mixed) {
        return "is on both inputs and outputs";
    }
    uint32_t expect = 0;
    for (size_t i = 0; i < count; i++) {
        if (run[i].bit == expect) {
            expect++;
        } else if (expect > 0 && run[i].bit == expect - 1) {
            (void)snprintf(buf, LABEL_BYTES, "has bit %" PRIu32 " twice", run[i].bit);
            return buf;
        } else {
            break;
        }
    }
    if (expect == count) {
        return NULL;
    }
    (void)snprintf(buf, LABEL_BYTES, "has no bit %" PRIu32, expect);
    return buf;
}

/* One run of entries with equal (name, dir) as a bus; its nets go on set->nets. */
static odin3_status add_bus(const bus_ctx *ctx, const bus_entry *run, size_t count, bool mixed,
                            odin3_submod_busset *set) {
    char buf[LABEL_BYTES];
    const char *why = run_problem(run, count, mixed, buf);
    odin3_submod_bus bus = {run[0].name, run[0].dir, (uint32_t)set->nets.len, (uint32_t)count,
                            why == NULL};
    for (size_t i = 0; i < count; i++) {
        odin3_net_id *net = odin3_vec_push(&set->nets);
        if (net == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *net = (odin3_net_id){run[i].net};
    }
    odin3_submod_bus *slot = odin3_vec_push(&set->buses);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = bus;
    if (why != NULL && !ctx->quiet) {
        const odin3_strtab *tab = odin3_design_strtab(odin3_module_design(ctx->module));
        odin3_log(ODIN3_LOG_WARN, "submod: group '%s': bus '%s' %s: its nets stay scalar ports",
                  ctx->group, odin3_strtab_get(tab, run[0].name), why);
    }
    return ODIN3_OK;
}

/* Cuts the sorted entries into (name, dir) runs and adds a bus per run. */
static odin3_status cut_runs(const bus_ctx *ctx, const odin3_vec *entries,
                             odin3_submod_busset *set) {
    const bus_entry *all = entries->data;
    size_t start = 0;
    while (start < entries->len) {
        size_t end = start;
        while (end < entries->len && all[end].name == all[start].name &&
               all[end].dir == all[start].dir) {
            end++;
        }
        bool mixed = (start > 0 && all[start - 1].name == all[start].name) ||
                     (end < entries->len && all[end].name == all[start].name);
        odin3_status st = add_bus(ctx, all + start, end - start, mixed, set);
        if (st != ODIN3_OK) {
            return st;
        }
        start = end;
    }
    return ODIN3_OK;
}

/* of_net: bus index + 1 for every net of an ok bus. */
static odin3_status fill_of_net(const bus_ctx *ctx, odin3_submod_busset *set) {
    for (uint32_t nid = 0; nid < odin3_module_net_end(ctx->module); nid++) {
        uint32_t *slot = odin3_vec_push(&set->of_net);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = 0;
    }
    const odin3_submod_bus *buses = set->buses.data;
    const odin3_net_id *nets = set->nets.data;
    for (size_t idx = 0; idx < set->buses.len; idx++) {
        for (uint32_t k = 0; buses[idx].ok && k < buses[idx].width; k++) {
            *(uint32_t *)odin3_vec_at(&set->of_net, nets[buses[idx].first + k].v) =
                (uint32_t)idx + 1U;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_submod_busset_build(const odin3_module *module, const odin3_vec *state,
                                       const char *key, const char *group,
                                       odin3_submod_busset *out) {
    const bus_ctx ctx = {module, key, group, out->quiet};
    uint32_t key_str = 0;
    odin3_design *design = odin3_module_design(module);
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &key_str);
    odin3_vec entries;
    odin3_vec_init(&entries, sizeof(bus_entry));
    st = st == ODIN3_OK ? collect_entries(&ctx, state, key_str, &entries) : st;
    if (st == ODIN3_OK) {
        sort_entries(entries.data, entries.len);
    }
    st = st == ODIN3_OK ? cut_runs(&ctx, &entries, out) : st;
    st = st == ODIN3_OK ? fill_of_net(&ctx, out) : st;
    odin3_vec_free(&entries);
    return st;
}
