/*
 * reader_implicit_dirs.c — directions of the ports of implicit black boxes (--auto-blackbox; 2D
 * spec EL-11, amended 2026-10-21): a port no cell gave `port_directions` for takes its direction
 * from the bits it is connected to. Per module, in cell order: an output when a bit is driven by
 * nothing known (no input or inout port of the module, no output of a known cell, no earlier
 * unknown port), an inout when a bit is a module inout or a known cell's inout, an input
 * otherwise. A port is one direction for the cell type (output if any cell needs it). The WARNING
 * of the type (reader_implicit.c) lists them.
 */
#include "frontends/json/reader_internal.h"

#include <string.h>

enum { DF_DRV = 1, DF_BIDI = 2, DF_UNK = 4, KEY_SHIFT = 32 };

/* The state of one module's analysis: the flags of its bit labels. */
typedef struct dirs_mod {
    jr *rd;
    odin3_u64map *bits; /* label -> DF_DRV | DF_BIDI */
    odin3_u64map *unk;  /* label -> 1: driven by an unknown port made an output */
    bool final;         /* the second sweep: look for ports that drive one net twice */
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
    return odin3_u64map_get(dm->unk, label, NULL) ? flags | DF_UNK : flags;
}

static odin3_status add_flags(dirs_mod *dm, uint64_t label, uint64_t flag) {
    if (flag == DF_UNK) {
        return odin3_u64map_put(dm->unk, (odin3_kv){label, 1});
    }
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
    return dir == ODIN3_DIR_INOUT ? DF_BIDI : 0;
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

/* The port of an implicit type called key, or NULL. */
static jr_iport *iport_of(const jr *rd, const dirs_type *type, const char *key) {
    uint32_t str = 0;
    uint64_t at = 0;
    if (!odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(key), &str) ||
        !odin3_u64map_get(rd->implicit_port, (type->entry << KEY_SHIFT) | str, &at)) {
        return NULL;
    }
    return odin3_vec_at(&type->implicit->ports, at);
}

/* --- one module ------------------------------------------------------------------------------- */

/* The ports of the module: inputs are driven from outside, inouts are bidirectional. */
static odin3_status mark_module_ports(dirs_mod *dm, const odin3_json_node *mod) {
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *port = jr_first(jr_member(mod, "ports")); st == ODIN3_OK && port;
         port = odin3_json_next(port)) {
        int dir = dir_from_text(jr_member(port, "direction"));
        uint64_t flag = dir == ODIN3_DIR_IN ? DF_DRV : 0;
        flag = dir == ODIN3_DIR_INOUT ? DF_BIDI : flag;
        st = mark_bits(dm, jr_member(port, "bits"), flag);
    }
    return st;
}

/* The outputs and inouts of a cell of a known type, or of an implicit type whose port has a
 * direction, drive their bits. */
static odin3_status mark_cell_drivers(dirs_mod *dm, const odin3_json_node *cell) {
    dirs_type type = type_of(dm->rd, cell);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(jr_member(cell, "connections"));
         st == ODIN3_OK && conn != NULL; conn = odin3_json_next(conn)) {
        int dir = -1;
        const jr_iport *iport =
            type.implicit != NULL ? iport_of(dm->rd, &type, odin3_json_key(conn)) : NULL;
        if (iport != NULL && iport->dir_known) {
            dir = (int)iport->def.dir;
        } else if (type.implicit == NULL) {
            dir = known_dir(&type, odin3_json_key(conn));
        }
        st = mark_bits(dm, conn, flag_of_dir(dir));
    }
    return st;
}

/* One connection of an implicit cell whose port has no direction yet. */
static odin3_status classify(dirs_mod *dm, jr_iport *port, const odin3_json_node *conn) {
    bool need_out = false;
    bool bidi = false;
    bool known = jr_count(conn) > 0;
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); elem != NULL; elem = odin3_json_next(elem)) {
        uint64_t flags = label_of(elem, &label) ? flags_of(dm, label) : DF_DRV;
        bool net = label_of(elem, &label);
        bidi = bidi || (flags & DF_BIDI) != 0;
        need_out = need_out || (net && (flags & (DF_DRV | DF_UNK | DF_BIDI)) == 0);
        known = known && (flags & DF_DRV) != 0 && (flags & DF_BIDI) == 0;
    }
    if (!need_out) {
        port->role |= bidi ? JR_ROLE_BIDI : 0;
        port->role |= known && !bidi ? JR_ROLE_IN_KNOWN : 0;
        return ODIN3_OK;
    }
    port->role |= JR_ROLE_OUT;
    return mark_bits(dm, conn, DF_UNK);
}

/* Second sweep: a port that drives a bit another unknown port drives is a real conflict. */
static odin3_status check_conflict(dirs_mod *dm, jr_iport *port, const odin3_json_node *conn) {
    uint64_t label = 0;
    for (const odin3_json_node *elem = jr_first(conn); elem != NULL; elem = odin3_json_next(elem)) {
        if (label_of(elem, &label) && (flags_of(dm, label) & DF_UNK) != 0) {
            port->role |= JR_ROLE_CONFLICT;
        }
    }
    return mark_bits(dm, conn, DF_UNK);
}

static odin3_status classify_cell(dirs_mod *dm, const odin3_json_node *cell) {
    dirs_type type = type_of(dm->rd, cell);
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *conn = jr_first(jr_member(cell, "connections"));
         type.implicit != NULL && st == ODIN3_OK && conn != NULL; conn = odin3_json_next(conn)) {
        jr_iport *port = iport_of(dm->rd, &type, odin3_json_key(conn));
        if (port != NULL && !port->dir_known && !dm->final) {
            st = classify(dm, port, conn);
        } else if (port != NULL && !port->dir_known && (port->role & JR_ROLE_OUT) != 0) {
            st = check_conflict(dm, port, conn);
        }
    }
    return st;
}

static odin3_status infer_module(jr *rd, const jr_mod *mod) {
    dirs_mod dm = {.rd = rd,
                   .bits = odin3_u64map_create(0),
                   .unk = odin3_u64map_create(0),
                   .cells = jr_member(mod->node, "cells")};
    if (dm.bits == NULL || dm.unk == NULL) {
        odin3_u64map_destroy(dm.bits);
        odin3_u64map_destroy(dm.unk);
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    odin3_status st = mark_module_ports(&dm, mod->node);
    for (const odin3_json_node *cell = jr_first(dm.cells); st == ODIN3_OK && cell != NULL;
         cell = odin3_json_next(cell)) {
        st = mark_cell_drivers(&dm, cell);
    }
    for (int sweep = 0; sweep < 2; sweep++) {
        dm.final = sweep == 1;
        odin3_u64map_destroy(dm.unk);
        dm.unk = odin3_u64map_create(0);
        st = dm.unk != NULL ? st : ODIN3_ERR_NO_MEMORY;
        for (const odin3_json_node *cell = jr_first(dm.cells); st == ODIN3_OK && cell != NULL;
             cell = odin3_json_next(cell)) {
            st = classify_cell(&dm, cell);
        }
    }
    odin3_u64map_destroy(dm.bits);
    odin3_u64map_destroy(dm.unk);
    return jr_fail(rd, st);
}

odin3_status jr_infer_implicit(jr *rd) {
    odin3_status st = ODIN3_OK;
    for (uint32_t idx = 0; st == ODIN3_OK && rd->implicit.len > 0 && idx < rd->mods.len; idx++) {
        st = infer_module(rd, odin3_vec_cat(&rd->mods, idx));
    }
    for (size_t entry = 0; entry < rd->implicit.len; entry++) {
        jr_implicit *type = odin3_vec_at(&rd->implicit, entry);
        for (size_t pos = 0; pos < type->ports.len; pos++) {
            jr_iport *port = odin3_vec_at(&type->ports, pos);
            if (!port->dir_known) {
                port->def.dir = jr_role_dir(port->role);
            }
        }
    }
    return st;
}
