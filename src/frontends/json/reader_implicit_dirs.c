/*
 * reader_implicit_dirs.c — directions of the ports of implicit black boxes (--auto-blackbox; 2D
 * spec EL-11, amended 2026-10-21): a port no cell gave `port_directions` for takes its direction
 * from the bits it is connected to, by votes that do not depend on the order of the cells.
 *
 * Per module the bits get flags from what is known (module ports, ports of known cells, ports with
 * port_directions), and the unknown ports on each bit are counted (none, one, several). Each
 * connection of an unknown port then votes per bit: output when nothing known drives the bit,
 * something known reads it and this is the only unknown port on it; input when a known driver is
 * on it; bidirectional when it is a module or cell inout; no vote for a dangling or ambiguous bit.
 * The votes of all modules settle a port per type (output only: output; input only or none:
 * input; both, or a bidirectional vote: inout). A tie-break then gives a type that has no output
 * one (tie_break), to a fixpoint. A further visit of the modules, with the settled directions,
 * finds the nets two outputs share and the ambiguous nets nothing drives; the WARNING of the type
 * (reader_implicit.c) names them.
 */
#include "frontends/json/reader_internal.h"

#include <string.h>

enum {
    DF_DRV = 1,   /* a known driver */
    DF_BIDI = 2,  /* a known inout */
    DF_RD = 4,    /* a known reader */
    DF_U1 = 8,    /* an unknown port is on it */
    DF_UM = 16,   /* two or more are */
    DF_ROUT = 32, /* a settled output is on it */
    DF_RINO = 64, /* a settled inout is on it */
    KEY_SHIFT = 32
};

/* Several different ports (or candidates) on a bit. */
static const uint64_t MANY = UINT64_MAX;

/* What a visit of the cells does. */
typedef enum { SW_COUNT, SW_VOTE, SW_OWN, SW_UNDRIVEN, SW_CAND, SW_PICK, SW_END } dirs_sweep;

/* The visits of the modules: the votes, the tie-break, the final visit. */
enum { PL_VOTE, PL_TIE, PL_FINAL, PL_N };

/* The state of one module's analysis: the flags of its bit labels. */
typedef struct dirs_mod {
    jr *rd;
    odin3_u64map *bits;  /* label -> DF_* */
    odin3_u64map *owner; /* label -> ((type + 1) << 32) | (position + 1) of its first output */
    odin3_u64map *ports; /* label -> the same, of the unknown port on it (MANY: different ports) */
    odin3_u64map *cand;  /* label -> the same, of the tie-break candidate on it (MANY: several) */
    const odin3_json_node *cells;
} dirs_mod;

static bool label_of(const odin3_json_node *node, uint64_t *label) {
    if (jr_kind(node) != ODIN3_JSON_INT || jr_int(node) < 0) {
        return false;
    }
    *label = (uint64_t)jr_int(node);
    return true;
}

static uint64_t flags_of(const dirs_mod *dm, uint64_t label) {
    uint64_t flags = 0;
    (void)odin3_u64map_get(dm->bits, label, &flags);
    return flags;
}

static odin3_status add_flags(dirs_mod *dm, uint64_t label, uint64_t flag) {
    return odin3_u64map_put(dm->bits, (odin3_kv){label, flags_of(dm, label) | flag});
}

/* Gives every label of the bits array the flag (0: none). */
static odin3_status mark_bits(dirs_mod *dm, const odin3_json_node *bits, uint64_t flag) {
    odin3_status st = ODIN3_OK;
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(bits); st == ODIN3_OK && elem != NULL && flag != 0;
         elem = odin3_json_next(elem)) {
        st = label_of(elem, &label) ? add_flags(dm, label, flag) : ODIN3_OK;
    }
    return st;
}

static uint64_t flag_of_dir(int dir) {
    if (dir == ODIN3_DIR_OUT) {
        return DF_DRV;
    }
    return dir == ODIN3_DIR_INOUT ? DF_BIDI : DF_RD;
}

/* The direction text of a ports member ("input", "output", "inout") as an odin3_dir, or -1. */
static int dir_from_text(const odin3_json_node *text) {
    const char *str = jr_kind(text) == ODIN3_JSON_STRING ? jr_text(text, NULL) : "";
    if (strcmp(str, "input") == 0) {
        return ODIN3_DIR_IN;
    }
    if (strcmp(str, "output") == 0) {
        return ODIN3_DIR_OUT;
    }
    return strcmp(str, "inout") == 0 ? ODIN3_DIR_INOUT : -1;
}

/* --- what a cell's type is -------------------------------------------------------------------- */

typedef struct dirs_type {
    const odin3_celltype_def *def; /* a registered or black-box type */
    const odin3_json_node *mod;    /* a module of the file */
    jr_implicit *implicit;         /* an implicit black box */
    uint64_t entry;                /* its index in rd->implicit */
} dirs_type;

static dirs_type type_of(const jr *rd, const odin3_json_node *cell) {
    dirs_type out = {0};
    const odin3_json_node *node = jr_member(cell, "type");
    const char *name = jr_kind(node) == ODIN3_JSON_STRING ? jr_text(node, NULL) : NULL;
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (name == NULL || strcmp(name, "$scopeinfo") == 0 ||
        !odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(name), &str)) {
        return out;
    }
    if (jr_lookup_type(rd, name, &id)) {
        out.def = odin3_celltype_get(rd->design, id);
    } else if (jr_find_mod(rd, str) != UINT32_MAX) {
        const jr_mod *mod = odin3_vec_cat(&rd->mods, jr_find_mod(rd, str));
        out.mod = mod->node;
    } else if (odin3_u64map_get(rd->implicit_index, str, &out.entry)) {
        out.implicit = odin3_vec_at((odin3_vec *)&rd->implicit, out.entry);
    }
    return out;
}

/* The direction of port `key` of a known type, or -1. */
static int known_dir(const dirs_type *type, const char *key) {
    if (type->def != NULL) {
        for (uint32_t pos = 0; pos < type->def->n_ports; pos++) {
            if (strcmp(type->def->ports[pos].name, key) == 0) {
                return (int)type->def->ports[pos].dir;
            }
        }
        return -1;
    }
    const odin3_json_node *port = jr_member(jr_member(type->mod, "ports"), key);
    return type->mod != NULL && port != NULL ? dir_from_text(jr_member(port, "direction")) : -1;
}

/* A port of an implicit type: where it is (the type's index, its position) and the port. */
typedef struct dirs_ref {
    jr_iport *port;
    uint64_t entry;
    uint64_t pos;
} dirs_ref;

/* The port of an implicit type called key (port NULL: none). */
static dirs_ref iport_of(const jr *rd, const dirs_type *type, const char *key) {
    uint32_t str = 0;
    dirs_ref ref = {0};
    if (!odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(key), &str) ||
        !odin3_u64map_get(rd->implicit_port, (type->entry << KEY_SHIFT) | str, &ref.pos)) {
        return ref;
    }
    ref.entry = type->entry;
    ref.port = odin3_vec_at(&type->implicit->ports, ref.pos);
    return ref;
}

/* --- one module ------------------------------------------------------------------------------- */

/* The ports of the module: inputs are driven from outside, outputs are read from outside, inouts
 * are bidirectional. */
static odin3_status mark_module_ports(dirs_mod *dm, const odin3_json_node *mod) {
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *port = jr_first(jr_member(mod, "ports")); st == ODIN3_OK && port;
         port = odin3_json_next(port)) {
        int dir = dir_from_text(jr_member(port, "direction"));
        uint64_t flag = dir == ODIN3_DIR_IN ? DF_DRV : DF_RD;
        flag = dir == ODIN3_DIR_INOUT ? DF_BIDI | DF_RD : flag;
        st = mark_bits(dm, jr_member(port, "bits"), flag);
    }
    return st;
}

/* The ports of a cell of a known type, or of an implicit type whose port has a direction, drive or
 * read their bits; a port of an implicit type without a direction does neither (it is counted). */
static odin3_status mark_cell_drivers(dirs_mod *dm, const odin3_json_node *cell) {
    dirs_type type = type_of(dm->rd, cell);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(jr_member(cell, "connections"));
         st == ODIN3_OK && conn != NULL; conn = odin3_json_next(conn)) {
        uint64_t flag = 0;
        if (type.implicit == NULL) {
            flag = flag_of_dir(known_dir(&type, odin3_json_key(conn)));
        } else {
            const jr_iport *port = iport_of(dm->rd, &type, odin3_json_key(conn)).port;
            flag = port != NULL && port->dir_known ? flag_of_dir((int)port->def.dir) : 0;
        }
        st = mark_bits(dm, conn, flag);
    }
    return st;
}

/* Counts the unknown ports on each bit of conn: one (DF_U1), several (DF_UM). */
static odin3_status count_conn(dirs_mod *dm, const odin3_json_node *conn) {
    odin3_status st = ODIN3_OK;
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem)) {
        if (label_of(elem, &label)) {
            st = add_flags(dm, label, (flags_of(dm, label) & DF_U1) != 0 ? DF_UM : DF_U1);
        }
    }
    return st;
}

/* The votes of one connection of an unknown port, per bit (a constant casts none). */
static void vote_conn(const dirs_mod *dm, jr_iport *port, const odin3_json_node *conn) {
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); elem != NULL; elem = odin3_json_next(elem)) {
        uint64_t flags = label_of(elem, &label) ? flags_of(dm, label) : 0;
        if ((flags & DF_BIDI) != 0) {
            port->role |= JR_ROLE_BIDI;
        } else if ((flags & DF_DRV) != 0) {
            port->role |= JR_ROLE_IN;
        } else if ((flags & DF_UM) == 0 && (flags & DF_RD) != 0 && (flags & DF_U1) != 0) {
            port->role |= JR_ROLE_OUT;
        }
    }
}

/* The port at (entry, pos), packed as the owner map keeps it. */
static uint64_t pack_ref(const dirs_ref *ref) {
    return ((ref->entry + 1) << KEY_SHIFT) | (ref->pos + 1);
}

static void set_peer(jr_iport *port, uint64_t packed) {
    if (port->peer_type == 0) {
        port->peer_type = (uint32_t)(packed >> KEY_SHIFT);
        port->peer_port = (uint32_t)(packed & UINT32_MAX);
    }
}

/* Two settled outputs share a bit: each names the other. */
static void link_peers(jr *rd, const dirs_ref *ref, uint64_t other) {
    uint64_t self = pack_ref(ref);
    if (other == self) {
        return;
    }
    jr_implicit *type = odin3_vec_at(&rd->implicit, (other >> KEY_SHIFT) - 1);
    jr_iport *port = odin3_vec_at(&type->ports, (other & UINT32_MAX) - 1);
    set_peer(ref->port, other);
    set_peer(port, self);
}

/* A settled output or inout takes its bits; an output on a bit another output has is a conflict. */
static odin3_status own_conn(dirs_mod *dm, const dirs_ref *ref, const odin3_json_node *conn) {
    odin3_status st = ODIN3_OK;
    uint64_t label = 0;
    bool out = ref->port->def.dir == ODIN3_DIR_OUT;
    for (const odin3_json_node *elem = jr_first(conn); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem)) {
        if (!label_of(elem, &label) || ref->port->def.dir == ODIN3_DIR_IN) {
            continue;
        }
        uint64_t owner = 0;
        if (out && (flags_of(dm, label) & DF_ROUT) != 0 &&
            odin3_u64map_get(dm->owner, label, &owner)) {
            link_peers(dm->rd, ref, owner);
        } else if (out) {
            st = odin3_u64map_put(dm->owner, (odin3_kv){label, pack_ref(ref)});
        }
        st = st == ODIN3_OK ? add_flags(dm, label, out ? DF_ROUT : DF_RINO) : st;
    }
    return st;
}

/* A settled input on an ambiguous bit that nothing, settled or known, drives. */
static void undriven_conn(const dirs_mod *dm, jr_iport *port, const odin3_json_node *conn) {
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); elem != NULL; elem = odin3_json_next(elem)) {
        uint64_t flags = label_of(elem, &label) ? flags_of(dm, label) : 0;
        bool bare = (flags & (DF_DRV | DF_BIDI | DF_ROUT | DF_RINO)) == 0;
        if (bare && (flags & DF_UM) != 0 && port->def.dir == ODIN3_DIR_IN &&
            (port->role & JR_ROLE_UNDRIVEN) == 0) {
            port->role |= JR_ROLE_UNDRIVEN;
            port->net = label;
        }
    }
}

/* A bit nothing, settled or known, drives, with two or more different unknown ports on it. */
static bool ambiguous(const dirs_mod *dm, uint64_t label) {
    uint64_t flags = flags_of(dm, label);
    uint64_t port = 0;
    return (flags & (DF_DRV | DF_BIDI | DF_ROUT | DF_RINO)) == 0 && (flags & DF_UM) != 0 &&
           odin3_u64map_get(dm->ports, label, &port) && port == MANY;
}

/* The port self is on the bit label: the only one there so far, or MANY. */
static odin3_status note_port(odin3_u64map *map, uint64_t label, uint64_t self) {
    uint64_t cur = 0;
    bool seen = odin3_u64map_get(map, label, &cur);
    return odin3_u64map_put(map, (odin3_kv){label, !seen || cur == self ? self : MANY});
}

/* A connection: per bit, the unknown port on it (or MANY) and the one candidate port (or MANY). */
static odin3_status cand_conn(dirs_mod *dm, const dirs_ref *ref, const odin3_json_node *conn) {
    odin3_status st = ODIN3_OK;
    uint64_t label = 0;
    uint64_t self = pack_ref(ref);
    for (const odin3_json_node *elem = jr_first(conn); st == ODIN3_OK && elem != NULL;
         elem = odin3_json_next(elem)) {
        if (label_of(elem, &label)) {
            st = note_port(dm->ports, label, self);
            st = st == ODIN3_OK && ref->port->want ? note_port(dm->cand, label, self) : st;
        }
    }
    return st;
}

/* A candidate that is alone on an ambiguous bit is picked. */
static void pick_conn(const dirs_mod *dm, jr_iport *port, uint64_t self,
                      const odin3_json_node *conn) {
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); elem != NULL && port->want;
         elem = odin3_json_next(elem)) {
        uint64_t cur = 0;
        if (label_of(elem, &label) && odin3_u64map_get(dm->cand, label, &cur) && cur == self &&
            ambiguous(dm, label)) {
            port->pick = true;
        }
    }
}

static odin3_status visit_conn(dirs_mod *dm, const dirs_ref *ref, const odin3_json_node *conn,
                               dirs_sweep sweep) {
    odin3_status st = ODIN3_OK;
    if (sweep == SW_CAND) {
        st = cand_conn(dm, ref, conn);
    } else if (sweep == SW_PICK) {
        pick_conn(dm, ref->port, pack_ref(ref), conn);
    } else if (sweep == SW_COUNT) {
        st = count_conn(dm, conn);
    } else if (sweep == SW_VOTE) {
        vote_conn(dm, ref->port, conn);
    } else if (sweep == SW_OWN) {
        st = own_conn(dm, ref, conn);
    } else {
        undriven_conn(dm, ref->port, conn);
    }
    return st;
}

static odin3_status visit_cell(dirs_mod *dm, const odin3_json_node *cell, dirs_sweep sweep) {
    dirs_type type = type_of(dm->rd, cell);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(jr_member(cell, "connections"));
         type.implicit != NULL && st == ODIN3_OK && conn != NULL; conn = odin3_json_next(conn)) {
        dirs_ref ref = iport_of(dm->rd, &type, odin3_json_key(conn));
        if (ref.port != NULL && !ref.port->dir_known) {
            st = visit_conn(dm, &ref, conn, sweep);
        }
    }
    return st;
}

/* The visits of a phase: 0 votes; 1 (with the settled directions) finds conflicts and nets that
 * nothing drives. */
static const dirs_sweep k_plan[PL_N][SW_END] = {
    {SW_COUNT, SW_VOTE, SW_END, SW_END, SW_END, SW_END},
    {SW_COUNT, SW_OWN, SW_CAND, SW_PICK, SW_END, SW_END},
    {SW_COUNT, SW_OWN, SW_UNDRIVEN, SW_END, SW_END, SW_END},
};

static odin3_status infer_module(jr *rd, const jr_mod *mod, int phase) {
    dirs_mod dm = {.rd = rd,
                   .bits = odin3_u64map_create(0),
                   .owner = odin3_u64map_create(0),
                   .ports = odin3_u64map_create(0),
                   .cand = odin3_u64map_create(0),
                   .cells = jr_member(mod->node, "cells")};
    odin3_status st = dm.bits != NULL && dm.owner != NULL && dm.cand != NULL && dm.ports != NULL
                          ? mark_module_ports(&dm, mod->node)
                          : ODIN3_ERR_NO_MEMORY;
    for (const odin3_json_node *cell = jr_first(dm.cells); st == ODIN3_OK && cell != NULL;
         cell = odin3_json_next(cell)) {
        st = mark_cell_drivers(&dm, cell);
    }
    for (int step = 0; step < SW_END && k_plan[phase][step] != SW_END; step++) {
        for (const odin3_json_node *cell = jr_first(dm.cells); st == ODIN3_OK && cell != NULL;
             cell = odin3_json_next(cell)) {
            st = visit_cell(&dm, cell, k_plan[phase][step]);
        }
    }
    odin3_u64map_destroy(dm.bits);
    odin3_u64map_destroy(dm.owner);
    odin3_u64map_destroy(dm.cand);
    odin3_u64map_destroy(dm.ports);
    return jr_fail(rd, st);
}

/* Settles every unknown port from its votes (a tied one is the output). */
static void settle_ports(jr *rd) {
    for (size_t entry = 0; entry < rd->implicit.len; entry++) {
        jr_implicit *type = odin3_vec_at(&rd->implicit, entry);
        for (size_t pos = 0; pos < type->ports.len; pos++) {
            jr_iport *port = odin3_vec_at(&type->ports, pos);
            if (!port->dir_known) {
                port->def.dir =
                    (port->role & JR_ROLE_TIED) != 0 ? ODIN3_DIR_OUT : jr_role_dir(port->role);
            }
        }
    }
}

/* Whether the type has a port that is an output or inout. */
static bool has_output(const jr_implicit *type) {
    for (size_t pos = 0; pos < type->ports.len; pos++) {
        if (((const jr_iport *)odin3_vec_cat(&type->ports, pos))->def.dir != ODIN3_DIR_IN) {
            return true;
        }
    }
    return false;
}

/* Marks the candidates: the ports with no votes of a type that has no output. */
static void mark_wanting(jr *rd) {
    for (size_t entry = 0; entry < rd->implicit.len; entry++) {
        jr_implicit *type = odin3_vec_at(&rd->implicit, entry);
        bool none = !has_output(type);
        for (size_t pos = 0; pos < type->ports.len; pos++) {
            jr_iport *port = odin3_vec_at(&type->ports, pos);
            uint8_t votes = port->role & (JR_ROLE_OUT | JR_ROLE_IN | JR_ROLE_BIDI);
            port->want = none && !port->dir_known && votes == 0;
            port->pick = false;
        }
    }
}

/* A type with no output and exactly one picked port makes it the output; true when one did. */
static bool give_outputs(jr *rd) {
    bool changed = false;
    for (size_t entry = 0; entry < rd->implicit.len; entry++) {
        jr_implicit *type = odin3_vec_at(&rd->implicit, entry);
        size_t picked = 0;
        jr_iport *last = NULL;
        for (size_t pos = 0; pos < type->ports.len; pos++) {
            jr_iport *port = odin3_vec_at(&type->ports, pos);
            picked += port->pick ? 1U : 0U;
            last = port->pick ? port : last;
        }
        if (picked == 1) {
            last->role |= JR_ROLE_TIED;
            last->def.dir = ODIN3_DIR_OUT;
            changed = true;
        }
    }
    return changed;
}

/* The tie-break, a fixpoint: every cell type should have an output. On an ambiguous bit (nothing
 * known drives it, two or more unknown ports, none settled as output) the only port of a type with
 * no output becomes that type's output, if it is the type's only such port. Two output-less types
 * on the bit, or two candidate ports of a type, stay inputs (no guess by order or by name). */
static odin3_status tie_break(jr *rd) {
    odin3_status st = ODIN3_OK;
    bool changed = true;
    for (size_t round = 0; changed && st == ODIN3_OK && round <= rd->implicit.len; round++) {
        mark_wanting(rd);
        for (uint32_t idx = 0; st == ODIN3_OK && idx < rd->mods.len; idx++) {
            st = infer_module(rd, odin3_vec_cat(&rd->mods, idx), PL_TIE);
        }
        changed = give_outputs(rd);
    }
    for (size_t entry = 0; entry < rd->implicit.len; entry++) {
        jr_implicit *type = odin3_vec_at(&rd->implicit, entry);
        for (size_t pos = 0; pos < type->ports.len; pos++) {
            jr_iport *port = odin3_vec_at(&type->ports, pos);
            port->peer_type = 0;
            port->peer_port = 0;
        }
    }
    return st;
}

odin3_status jr_infer_implicit(jr *rd) {
    odin3_status st = ODIN3_OK;
    for (int phase = 0; phase < 2 && rd->implicit.len > 0; phase++) {
        int plan = phase == 0 ? PL_VOTE : PL_FINAL;
        for (uint32_t idx = 0; st == ODIN3_OK && idx < rd->mods.len; idx++) {
            st = infer_module(rd, odin3_vec_cat(&rd->mods, idx), plan);
        }
        settle_ports(rd);
        if (phase == 0 && st == ODIN3_OK) {
            st = tie_break(rd);
        }
    }
    return st;
}
