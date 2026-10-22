/*
 * signals.c — the signal table (S1, spec SA-3 to SA-5, SA-7, SA-8): scopes, the listed signals,
 * name lookup that appends signals on demand, and value reads and writes over the simulator's
 * value slots.
 */
#include "sim/signals.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "sim/sim_internal.h"
#include "util/alloc.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NO_PORT UINT32_MAX /* no top input port */
#define WHOLE UINT32_MAX   /* the whole wire, not one bit of it */

enum { NAME_MAP_CAP = 64, MAX_INDEX_DIGITS = 9, DECIMAL_BASE = 10, ANON_CAP = 32 };

struct odin3_sim_signals {
    odin3_sim *sim;
    odin3_strtab *names;   /* every name and path the table hands out; strings never move */
    odin3_u64map *by_name; /* full-name string ID -> signal ID (first listed wins) */
    odin3_vec signals;     /* odin3_sim_signal */
    odin3_vec slots;       /* uint32_t value slot per signal bit */
    odin3_vec scopes;      /* odin3_sim_scope */
    uint32_t listed;
};

/* A signal and, for a top port, where its bits sit in sim->in_bits / out_bits. */
typedef struct sig_spec {
    odin3_sim_signal sig;
    const uint32_t *direct;
} sig_spec;

/* --- small helpers ---------------------------------------------------------------------------- */

static odin3_module *scope_module(const odin3_sim_signals *sig, uint32_t scope) {
    return odin3_module_get(sig->sim->design, sig->sim->frames[scope].module);
}

static const char *design_text(const odin3_sim_signals *sig, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(sig->sim->design), str);
}

static bool public_name(const char *text) {
    return text != NULL && text[0] != '\0' && text[0] != '$';
}

static odin3_status intern(odin3_sim_signals *sig, const char *text, uint32_t *id) {
    return odin3_strtab_intern(sig->names, odin3_bytes_cstr(text), id);
}

/* Interns leaf, and the full name path-of-scope "." leaf (the leaf alone in scope 0). */
static odin3_status intern_names(odin3_sim_signals *sig, uint32_t scope, const char *leaf,
                                 odin3_sim_signal *out) {
    const odin3_sim_scope *sc = odin3_vec_cat(&sig->scopes, scope);
    const char *path = odin3_strtab_get(sig->names, sc->name);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = path[0] != '\0' ? odin3_strbuf_appendf(&buf, "%s.%s", path, leaf)
                                      : odin3_strbuf_appendf(&buf, "%s", leaf);
    st = st != ODIN3_OK ? st : intern(sig, buf.data, &out->name);
    st = st != ODIN3_OK ? st : intern(sig, leaf, &out->leaf);
    odin3_strbuf_free(&buf);
    return st;
}

static uint32_t net_slot(const odin3_sim *sim, uint32_t scope, odin3_net_id net) {
    uint32_t slot = ODIN3_SIM_UNSET;
    if (odin3_net_valid(net)) {
        slot = sim->netmap[sim->frames[scope].map + net.v];
    }
    return slot == ODIN3_SIM_UNSET ? (uint32_t)ODIN3_SIM_SLOT_ZERO : slot;
}

/* --- appending signals ------------------------------------------------------------------------ */

/* Maps the signal's name to id unless an earlier signal has it. */
static odin3_status register_name(odin3_sim_signals *sig, uint32_t name, uint32_t id) {
    uint64_t have = 0;
    if (odin3_u64map_get(sig->by_name, name, &have)) {
        return ODIN3_OK;
    }
    return odin3_u64map_put(sig->by_name, (odin3_kv){name, id});
}

static odin3_status push_slots(odin3_sim_signals *sig, const sig_spec *spec) {
    const odin3_sim_signal *item = &spec->sig;
    const odin3_module *mod = scope_module(sig, item->scope);
    for (uint32_t k = 0; k < item->width; k++) {
        uint32_t slot = 0;
        if (spec->direct != NULL) {
            slot = spec->direct[k];
        } else if (item->obj_kind == ODIN3_OBJ_WIRE) {
            slot =
                net_slot(sig->sim, item->scope,
                         odin3_wire_net(mod, (odin3_wire_id){item->obj_id}, item->first_bit + k));
        } else {
            slot = net_slot(sig->sim, item->scope, (odin3_net_id){item->obj_id});
        }
        uint32_t *dst = odin3_vec_push(&sig->slots);
        if (dst == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *dst = slot;
    }
    return ODIN3_OK;
}

/* Appends the signal (ID total + 1) with its slots; on failure the table is as before. */
static odin3_status add_signal(odin3_sim_signals *sig, const sig_spec *spec) {
    size_t n_sig = sig->signals.len;
    size_t n_slots = sig->slots.len;
    odin3_sim_signal *dst = NULL;
    odin3_status st = ODIN3_OK;
    sig_spec local = *spec;
    local.sig.slot_first = (uint32_t)n_slots;
    st = push_slots(sig, &local);
    dst = st == ODIN3_OK ? odin3_vec_push(&sig->signals) : NULL;
    if (st == ODIN3_OK && dst == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        *dst = local.sig;
        st = register_name(sig, local.sig.name, (uint32_t)(n_sig + 1));
    }
    if (st != ODIN3_OK) {
        while (sig->signals.len > n_sig) {
            odin3_vec_pop(&sig->signals);
        }
        while (sig->slots.len > n_slots) {
            odin3_vec_pop(&sig->slots);
        }
    }
    return st;
}

/* --- specs of wires and nets ------------------------------------------------------------------ */

/* Sets kind, port and direct for a top-module wire that is a port; no-op for any other. */
static void port_info(const odin3_sim_signals *sig, odin3_wire_id wire, sig_spec *spec) {
    const odin3_sim *sim = sig->sim;
    const odin3_module *top = scope_module(sig, 0);
    odin3_sim_signal *item = &spec->sig;
    if (!odin3_node_valid(odin3_wire_port_node(top, wire))) {
        return;
    }
    uint32_t name = odin3_wire_name(top, wire);
    for (uint32_t pidx = 0; pidx < sim->n_in_ports; pidx++) {
        if (sim->in_ports[pidx].name != name) {
            continue;
        }
        uint32_t first = sim->in_ports[pidx].first + item->first_bit;
        bool all_clock = true;
        for (uint32_t k = 0; k < item->width; k++) {
            all_clock = all_clock && sim->in_clock[first + k] != 0;
        }
        item->kind = all_clock ? ODIN3_SIM_KIND_CLOCK : ODIN3_SIM_KIND_INPUT;
        item->in_port = pidx;
        item->in_bit = item->first_bit;
        spec->direct = sim->in_bits + first;
        return;
    }
    for (uint32_t pidx = 0; pidx < sim->n_out_ports; pidx++) {
        if (sim->out_ports[pidx].name == name) {
            item->kind = ODIN3_SIM_KIND_OUTPUT;
            spec->direct = sim->out_bits + sim->out_ports[pidx].first + item->first_bit;
            return;
        }
    }
}

/* The object a spec describes: a wire or net `id` in `scope`. */
typedef struct obj_ref {
    uint32_t scope;
    uint32_t kind;
    uint32_t id;
} obj_ref;

static void spec_init(sig_spec *spec, obj_ref obj) {
    *spec = (sig_spec){0};
    spec->sig.scope = obj.scope;
    spec->sig.kind = ODIN3_SIM_KIND_INTERNAL;
    spec->sig.obj_kind = obj.kind;
    spec->sig.obj_id = obj.id;
    spec->sig.in_port = NO_PORT;
}

/* The whole wire (bit == WHOLE) or the 1-bit select of vector position `bit`. */
static odin3_status wire_spec(odin3_sim_signals *sig, uint32_t scope, odin3_wire_id wire,
                              uint32_t bit, sig_spec *spec) {
    const odin3_module *mod = scope_module(sig, scope);
    odin3_sim_signal *item = &spec->sig;
    const char *base = design_text(sig, odin3_wire_name(mod, wire));
    odin3_strbuf leaf;
    spec_init(spec, (obj_ref){scope, ODIN3_OBJ_WIRE, wire.v});
    odin3_strbuf_init(&leaf);
    odin3_status st = ODIN3_OK;
    if (bit == WHOLE) {
        item->width = odin3_wire_width(mod, wire);
        item->msb = odin3_wire_msb(mod, wire);
        item->lsb = odin3_wire_lsb(mod, wire);
        st = odin3_strbuf_appendf(&leaf, "%s", base);
    } else {
        item->width = 1;
        item->first_bit = bit;
        item->msb = odin3_wire_index(mod, wire, bit);
        item->lsb = item->msb;
        st = odin3_strbuf_appendf(&leaf, "%s[%d]", base, (int)item->msb);
    }
    st = st != ODIN3_OK ? st : intern_names(sig, scope, leaf.data, item);
    odin3_strbuf_free(&leaf);
    if (st == ODIN3_OK && scope == 0) {
        port_info(sig, wire, spec);
    }
    return st;
}

static odin3_status net_spec(odin3_sim_signals *sig, uint32_t scope, odin3_net_id net,
                             const char *leaf, sig_spec *spec) {
    spec_init(spec, (obj_ref){scope, ODIN3_OBJ_NET, net.v});
    spec->sig.width = 1;
    return intern_names(sig, scope, leaf, &spec->sig);
}

/* --- scopes and the listing ------------------------------------------------------------------- */

/* Interns the leaf of frame's scope: the instance name, "#<node ID>" when unnamed, the module's. */
static odin3_status intern_leaf(odin3_sim_signals *sig, uint32_t frame, uint32_t *leaf) {
    const odin3_sim_frame *fr = &sig->sim->frames[frame];
    if (frame == 0) {
        return intern(sig, design_text(sig, odin3_module_name(scope_module(sig, 0))), leaf);
    }
    uint32_t str = odin3_node_name(scope_module(sig, fr->parent), fr->inst);
    if (str != 0) {
        return intern(sig, design_text(sig, str), leaf);
    }
    char anon[ANON_CAP];
    (void)snprintf(anon, sizeof anon, "#%u", fr->inst.v);
    return intern(sig, anon, leaf);
}

static odin3_status add_scope(odin3_sim_signals *sig, uint32_t frame) {
    odin3_sim_signal tmp = {0};
    odin3_sim_scope *sc = odin3_vec_push(&sig->scopes);
    if (sc == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sc->parent = frame == 0 ? 0 : sig->sim->frames[frame].parent;
    odin3_status st = intern_leaf(sig, frame, &sc->leaf);
    if (st == ODIN3_OK && frame != 0) {
        st = intern_names(sig, sc->parent, odin3_strtab_get(sig->names, sc->leaf), &tmp);
        sc->name = tmp.name;
    }
    return st;
}

static odin3_status list_wire(odin3_sim_signals *sig, uint32_t scope, odin3_wire_id wire) {
    sig_spec spec;
    odin3_status st = wire_spec(sig, scope, wire, WHOLE, &spec);
    return st != ODIN3_OK ? st : add_signal(sig, &spec);
}

static odin3_status list_ports(odin3_sim_signals *sig) {
    const odin3_sim *sim = sig->sim;
    const odin3_module *top = scope_module(sig, 0);
    odin3_status st = ODIN3_OK;
    for (uint32_t pidx = 0; st == ODIN3_OK && pidx < sim->n_in_ports; pidx++) {
        odin3_wire_id wire = odin3_module_find_wire(top, sim->in_ports[pidx].name);
        st = odin3_wire_valid(wire) ? list_wire(sig, 0, wire) : ODIN3_OK;
    }
    for (uint32_t pidx = 0; st == ODIN3_OK && pidx < sim->n_out_ports; pidx++) {
        odin3_wire_id wire = odin3_module_find_wire(top, sim->out_ports[pidx].name);
        st = odin3_wire_valid(wire) ? list_wire(sig, 0, wire) : ODIN3_OK;
    }
    return st;
}

/* True when the net is named in a public way and belongs to no wire (primary or alias). */
static bool bare_net(const odin3_sim_signals *sig, const odin3_module *mod, odin3_net_id net) {
    if (!odin3_net_live(mod, net) || !public_name(design_text(sig, odin3_net_name(mod, net)))) {
        return false;
    }
    if (odin3_wire_valid(odin3_net_primary(mod, net).wire)) {
        return false;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(mod, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire)) {
            return false;
        }
    }
    return true;
}

static odin3_status list_scope(odin3_sim_signals *sig, uint32_t scope) {
    const odin3_module *mod = scope_module(sig, scope);
    odin3_status st = ODIN3_OK;
    for (uint32_t wid = 1; st == ODIN3_OK && wid < odin3_module_wire_end(mod); wid++) {
        odin3_wire_id wire = {wid};
        bool own_port = scope == 0 && odin3_node_valid(odin3_wire_port_node(mod, wire));
        if (odin3_wire_live(mod, wire) && !own_port &&
            public_name(design_text(sig, odin3_wire_name(mod, wire)))) {
            st = list_wire(sig, scope, wire);
        }
    }
    for (uint32_t nid = 1; st == ODIN3_OK && nid < odin3_module_net_end(mod); nid++) {
        odin3_net_id net = {nid};
        sig_spec spec;
        if (bare_net(sig, mod, net)) {
            st = net_spec(sig, scope, net, design_text(sig, odin3_net_name(mod, net)), &spec);
            st = st != ODIN3_OK ? st : add_signal(sig, &spec);
        }
    }
    return st;
}

static odin3_status fill(odin3_sim_signals *sig) {
    odin3_status st = ODIN3_OK;
    for (uint32_t fidx = 0; st == ODIN3_OK && fidx < sig->sim->n_frames; fidx++) {
        st = add_scope(sig, fidx);
    }
    st = st != ODIN3_OK ? st : list_ports(sig);
    for (uint32_t fidx = 0; st == ODIN3_OK && fidx < sig->sim->n_frames; fidx++) {
        st = list_scope(sig, fidx);
    }
    sig->listed = (uint32_t)sig->signals.len;
    return st;
}

odin3_status odin3_sim_signals_create(odin3_sim *sim, odin3_sim_signals **out) {
    *out = NULL;
    odin3_sim_signals *sig = odin3_util_calloc(sizeof *sig);
    if (sig == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    sig->sim = sim;
    odin3_vec_init(&sig->signals, sizeof(odin3_sim_signal));
    odin3_vec_init(&sig->slots, sizeof(uint32_t));
    odin3_vec_init(&sig->scopes, sizeof(odin3_sim_scope));
    sig->names = odin3_strtab_create();
    sig->by_name = odin3_u64map_create(NAME_MAP_CAP);
    odin3_status st = sig->names != NULL && sig->by_name != NULL ? fill(sig) : ODIN3_ERR_NO_MEMORY;
    if (st != ODIN3_OK) {
        odin3_sim_signals_destroy(sig);
        return st;
    }
    *out = sig;
    return ODIN3_OK;
}

void odin3_sim_signals_destroy(odin3_sim_signals *sig) {
    if (sig == NULL) {
        return;
    }
    odin3_strtab_destroy(sig->names);
    odin3_u64map_destroy(sig->by_name);
    odin3_vec_free(&sig->signals);
    odin3_vec_free(&sig->slots);
    odin3_vec_free(&sig->scopes);
    odin3_util_free(sig);
}

/* --- queries ---------------------------------------------------------------------------------- */

uint32_t odin3_sim_signals_listed(const odin3_sim_signals *sig) {
    return sig->listed;
}

uint32_t odin3_sim_signals_total(const odin3_sim_signals *sig) {
    return (uint32_t)sig->signals.len;
}

const odin3_sim_signal *odin3_sim_signals_at(const odin3_sim_signals *sig, uint32_t id) {
    return id >= 1 && id <= sig->signals.len ? odin3_vec_cat(&sig->signals, id - 1) : NULL;
}

uint32_t odin3_sim_signals_scope_count(const odin3_sim_signals *sig) {
    return (uint32_t)sig->scopes.len;
}

const odin3_sim_scope *odin3_sim_signals_scope(const odin3_sim_signals *sig, uint32_t scope) {
    return scope < sig->scopes.len ? odin3_vec_cat(&sig->scopes, scope) : NULL;
}

const char *odin3_sim_signals_text(const odin3_sim_signals *sig, uint32_t str) {
    return odin3_strtab_get(sig->names, str);
}

/* --- find (SA-5) ------------------------------------------------------------------------------ */

/* Parses an optionally negative decimal index in canonical form (no sign +, no leading zeros). */
static bool parse_index(const char *digits, size_t len, int32_t *out) {
    size_t pos = len > 0 && digits[0] == '-' ? 1 : 0;
    size_t count = len - pos;
    if (count == 0 || count > MAX_INDEX_DIGITS || (digits[pos] == '0' && (count > 1 || pos == 1))) {
        return false;
    }
    int32_t val = 0;
    for (size_t i = pos; i < len; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
        val = val * DECIMAL_BASE + (digits[i] - '0');
    }
    *out = pos == 1 ? -val : val;
    return true;
}

/* The live wire named by the `len` bytes at text in mod, or none. Never interns. */
static odin3_wire_id find_wire_text(const odin3_sim_signals *sig, const odin3_module *mod,
                                    const char *text, size_t len) {
    uint32_t str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(sig->sim->design), (odin3_bytes){text, len}, &str)) {
        return (odin3_wire_id){0};
    }
    return odin3_module_find_wire(mod, str);
}

/* Step 3: text is wire[N] for a live wire of the scope that has declared index N. */
static odin3_status try_bit_select(odin3_sim_signals *sig, uint32_t scope, const char *text,
                                   sig_spec *spec, bool *found) {
    size_t len = strlen(text);
    const char *open = len > 0 && text[len - 1] == ']' ? strrchr(text, '[') : NULL;
    int32_t index = 0;
    *found = false;
    if (open == NULL || open == text ||
        !parse_index(open + 1, len - (size_t)(open - text) - 2, &index)) {
        return ODIN3_OK;
    }
    const odin3_module *mod = scope_module(sig, scope);
    odin3_wire_id wire = find_wire_text(sig, mod, text, (size_t)(open - text));
    if (!odin3_wire_valid(wire)) {
        return ODIN3_OK;
    }
    for (uint32_t bit = 0; bit < odin3_wire_width(mod, wire); bit++) {
        if (odin3_wire_index(mod, wire, bit) == index) {
            *found = true;
            return wire_spec(sig, scope, wire, bit, spec);
        }
    }
    return ODIN3_OK;
}

/* Step 4: a live wire named text, else a live net whose name or bare alias is text. */
static odin3_status try_name(odin3_sim_signals *sig, uint32_t scope, const char *text,
                             sig_spec *spec, bool *found) {
    const odin3_module *mod = scope_module(sig, scope);
    odin3_wire_id wire = find_wire_text(sig, mod, text, strlen(text));
    uint32_t str = 0;
    *found = false;
    if (odin3_wire_valid(wire)) {
        *found = true;
        return wire_spec(sig, scope, wire, WHOLE, spec);
    }
    if (!odin3_strtab_find(odin3_design_strtab(sig->sim->design), odin3_bytes_cstr(text), &str)) {
        return ODIN3_OK;
    }
    odin3_net_id net = odin3_module_find_net(mod, str);
    if (!odin3_net_valid(net)) {
        return ODIN3_OK;
    }
    *found = true;
    return net_spec(sig, scope, net, text, spec);
}

/* The scope of the instance `inst` of scope `parent`, or UINT32_MAX. */
static uint32_t child_scope(const odin3_sim *sim, uint32_t parent, odin3_node_id inst) {
    for (uint32_t fidx = parent + 1; fidx < sim->n_frames; fidx++) {
        if (sim->frames[fidx].parent == parent && sim->frames[fidx].inst.v == inst.v) {
            return fidx;
        }
    }
    return UINT32_MAX;
}

/* Step 5: the first "." whose prefix names an instance with a scope; *text moves past it. */
static uint32_t enter_instance(const odin3_sim_signals *sig, uint32_t scope, const char **text) {
    const odin3_module *mod = scope_module(sig, scope);
    const odin3_strtab *dtab = odin3_design_strtab(sig->sim->design);
    for (const char *dot = strchr(*text, '.'); dot != NULL; dot = strchr(dot + 1, '.')) {
        uint32_t str = 0;
        if (!odin3_strtab_find(dtab, (odin3_bytes){*text, (size_t)(dot - *text)}, &str)) {
            continue;
        }
        odin3_node_id node = odin3_module_find_node(mod, str);
        uint32_t child = odin3_node_valid(node) ? child_scope(sig->sim, scope, node) : UINT32_MAX;
        if (child != UINT32_MAX) {
            *text = dot + 1;
            return child;
        }
    }
    return UINT32_MAX;
}

/* Steps 3 to 5 over the scopes entered; *found says whether spec is filled. */
static odin3_status resolve(odin3_sim_signals *sig, const char *text, sig_spec *spec, bool *found) {
    uint32_t scope = 0;
    for (;;) {
        odin3_status st = try_bit_select(sig, scope, text, spec, found);
        st = st != ODIN3_OK || *found ? st : try_name(sig, scope, text, spec, found);
        if (st != ODIN3_OK || *found) {
            return st;
        }
        scope = enter_instance(sig, scope, &text);
        if (scope == UINT32_MAX) {
            return ODIN3_OK;
        }
    }
}

/* Steps 1 and 2: the ID of a signal already named `name`, 0 for none. */
static uint32_t known_signal(const odin3_sim_signals *sig, odin3_bytes name) {
    uint32_t str = 0;
    uint64_t id = 0;
    if (odin3_strtab_find(sig->names, name, &str) && odin3_u64map_get(sig->by_name, str, &id)) {
        return (uint32_t)id;
    }
    return 0;
}

odin3_status odin3_sim_signals_find(odin3_sim_signals *sig, odin3_bytes name, uint32_t *id) {
    *id = 0;
    if (name.len > 0 && memchr(name.ptr, '\0', name.len) != NULL) {
        return ODIN3_OK;
    }
    *id = known_signal(sig, name);
    if (*id != 0) {
        return ODIN3_OK;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_strbuf_append(&text, name);
    sig_spec spec;
    bool found = false;
    st = st != ODIN3_OK ? st : resolve(sig, text.data != NULL ? text.data : "", &spec, &found);
    st = st != ODIN3_OK || !found ? st : add_signal(sig, &spec);
    odin3_strbuf_free(&text);
    if (st == ODIN3_OK && found) {
        *id = odin3_sim_signals_total(sig);
    }
    return st;
}

/* --- read and write (SA-7, SA-8) -------------------------------------------------------------- */

odin3_status odin3_sim_signals_read(const odin3_sim_signals *sig, uint32_t id, uint8_t *bits,
                                    uint32_t count) {
    const odin3_sim_signal *item = odin3_sim_signals_at(sig, id);
    if (item == NULL || bits == NULL || count != item->width) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const uint32_t *slots = odin3_vec_cat(&sig->slots, item->slot_first);
    for (uint32_t k = 0; k < count; k++) {
        bits[k] = sig->sim->values[slots[k]];
    }
    return ODIN3_OK;
}

/* The reason bits cannot be written to the signal, NULL when they can. */
static const char *write_problem(const odin3_sim_signals *sig, const odin3_sim_signal *item,
                                 const uint8_t *bits) {
    for (uint32_t k = 0; k < item->width; k++) {
        if (bits[k] > 1) {
            return "a bit is not 0 or 1";
        }
        odin3_sim_bit at = {item->in_port, item->in_bit + k};
        if (bits[k] == 1 && odin3_sim_input_is_clock(sig->sim, at)) {
            return "a clock bit set to 1";
        }
    }
    return NULL;
}

odin3_status odin3_sim_signals_write(odin3_sim_signals *sig, uint32_t id, const uint8_t *bits,
                                     uint32_t count, const char **why) {
    const char *local = NULL;
    const odin3_sim_signal *item = odin3_sim_signals_at(sig, id);
    why = why != NULL ? why : &local;
    if (item == NULL) {
        *why = "unknown signal";
    } else if (item->kind != ODIN3_SIM_KIND_INPUT) {
        *why = "not an input";
    } else if (bits == NULL || count != item->width) {
        *why = "count is not the width";
    } else {
        *why = write_problem(sig, item, bits);
    }
    if (*why != NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t k = 0; k < count; k++) {
        odin3_sim_bit at = {item->in_port, item->in_bit + k};
        if (!odin3_sim_input_is_clock(sig->sim, at)) {
            (void)odin3_sim_set_input(sig->sim, at, bits[k] == 1);
        }
    }
    return ODIN3_OK;
}
