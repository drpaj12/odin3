/*
 * reader_implicit.c — cell types the file and the registry do not have (M-5 rule 4): the located
 * error, or with --auto-blackbox an implicit black box whose ports the cells' connections give.
 */
#include "frontends/json/reader_internal.h"

#include <string.h>

enum { KEY_SHIFT = 32 };

bool jr_lookup_type(const jr *rd, const char *name, odin3_celltype_id *id) {
    uint32_t str = 0;
    return odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(name), &str) &&
           odin3_celltype_find(rd->design, str, id);
}

void jr_free_implicit(jr *rd) {
    for (size_t i = 0; i < rd->implicit.len; i++) {
        odin3_vec_free(&((jr_implicit *)odin3_vec_at(&rd->implicit, i))->ports);
    }
}

/* The direction a port_directions entry names, or an error. */
static odin3_status dir_of(const jr *rd, const odin3_json_node *entry, const jr_where *where,
                           odin3_dir *dir) {
    const char *text = jr_kind(entry) == ODIN3_JSON_STRING ? jr_text(entry, NULL) : "";
    if (strcmp(text, "input") == 0) {
        *dir = ODIN3_DIR_IN;
    } else if (strcmp(text, "output") == 0) {
        *dir = ODIN3_DIR_OUT;
    } else if (strcmp(text, "inout") == 0) {
        *dir = ODIN3_DIR_INOUT;
    } else {
        return jr_error(rd, entry, where, "'%s' is not input, output or inout", text);
    }
    return ODIN3_OK;
}

/* The implicit type of name, made on first use. */
static odin3_status entry_of(jr *rd, uint32_t name_str, uint64_t *index) {
    if (odin3_u64map_get(rd->implicit_index, name_str, index)) {
        return ODIN3_OK;
    }
    jr_implicit *entry = odin3_vec_push(&rd->implicit);
    if (entry == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    entry->name_str = name_str;
    odin3_vec_init(&entry->ports, sizeof(jr_iport));
    *index = rd->implicit.len - 1;
    return jr_fail(rd, odin3_u64map_put(rd->implicit_index, (odin3_kv){name_str, *index}));
}

/* One connection of a cell as a port of the implicit type. */
typedef struct port_use {
    uint64_t entry;
    uint32_t name_str;
    uint32_t width;
    bool dir_known;
    odin3_dir dir;
} port_use;

static odin3_status add_use(jr *rd, const port_use *pu, const odin3_json_node *conn,
                            const jr_where *where) {
    jr_implicit *entry = odin3_vec_at(&rd->implicit, pu->entry);
    uint64_t key = (pu->entry << KEY_SHIFT) | pu->name_str;
    uint64_t at = 0;
    if (odin3_u64map_get(rd->implicit_port, key, &at)) {
        jr_iport *port = odin3_vec_at(&entry->ports, at);
        if (port->def.width != pu->width) {
            return jr_error(rd, conn, where,
                            "port '%s' is %u bits here and %u bits in an earlier cell of the "
                            "same type",
                            jr_str(rd, pu->name_str), (unsigned)pu->width,
                            (unsigned)port->def.width);
        }
        if (pu->dir_known && !port->dir_known) {
            port->def.dir = pu->dir;
            port->dir_known = true;
        }
        return ODIN3_OK;
    }
    entry->total_width += pu->width;
    if (entry->total_width > ODIN3_READER_MAX_TOTAL_WIDTH) {
        return jr_error(rd, conn, where, "the ports of the type are more than %u bits wide",
                        (unsigned)ODIN3_READER_MAX_TOTAL_WIDTH);
    }
    jr_iport *port = odin3_vec_push(&entry->ports);
    if (port == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *port = (jr_iport){.name_str = pu->name_str, .dir_known = pu->dir_known};
    port->def.dir = pu->dir_known ? pu->dir : ODIN3_DIR_INOUT;
    port->def.width = pu->width;
    port->def.scalar = pu->width == 1;
    return jr_fail(rd, odin3_u64map_put(rd->implicit_port, (odin3_kv){key, entry->ports.len - 1}));
}

/* The connection `conn` of a cell: its width and direction (from port_directions). */
static odin3_status note_connection(jr *rd, const odin3_json_node *cell, uint64_t entry,
                                    const odin3_json_node *conn) {
    jr_where where = {"cells", odin3_json_key(cell), NULL};
    port_use pu = {.entry = entry, .dir = ODIN3_DIR_INOUT};
    odin3_status st = jr_bits_count(rd, conn, &where, true, &pu.width);
    const odin3_json_node *dirs = jr_member(cell, "port_directions");
    const odin3_json_node *dir = dirs != NULL ? jr_member(dirs, odin3_json_key(conn)) : NULL;
    if (st == ODIN3_OK && dir != NULL) {
        st = dir_of(rd, dir, &where, &pu.dir);
        pu.dir_known = true;
    }
    if (st == ODIN3_OK) {
        st = jr_intern(rd, odin3_json_key(conn), &pu.name_str);
    }
    return st == ODIN3_OK ? add_use(rd, &pu, conn, &where) : st;
}

odin3_status jr_note_implicit(jr *rd, const odin3_json_node *cell, const char *type_name) {
    jr_where where = {"cells", odin3_json_key(cell), NULL};
    const odin3_json_node *conns = jr_member(cell, "connections");
    if (!rd->opts.auto_blackbox) {
        return jr_error(rd, jr_member(cell, "type"), &where,
                        "cell '%s' has type '%s', which is neither a module of the file nor a "
                        "registered cell type (use --auto-blackbox to treat it as a black box)",
                        odin3_json_key(cell), type_name);
    }
    if (conns == NULL || jr_kind(conns) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, cell, &where, "the cell has no connections object");
    }
    uint32_t name_str = 0;
    uint64_t entry = 0;
    odin3_status st = jr_intern(rd, type_name, &name_str);
    if (st == ODIN3_OK) {
        st = entry_of(rd, name_str, &entry);
    }
    for (const odin3_json_node *conn = jr_first(conns); st == ODIN3_OK && conn != NULL;
         conn = odin3_json_next(conn)) {
        st = note_connection(rd, cell, entry, conn);
    }
    return st;
}

/* The WARNING of one implicit type: the netlist with it is not equivalent to its source. */
static void warn_implicit(const jr *rd, const char *name) {
    if (name[0] == '$') {
        odin3_log(ODIN3_LOG_WARN,
                  "%s: '%s' is a Yosys internal cell type this tool has no cell for; it becomes "
                  "an implicit black box and the netlist is not equivalent to its source",
                  rd->path, name);
    } else {
        odin3_log(ODIN3_LOG_WARN,
                  "%s: cell type '%s' is neither a module of the file nor a registered cell "
                  "type; it becomes an implicit black box (every port inout unless "
                  "port_directions says)",
                  rd->path, name);
    }
}

static odin3_status make_implicit_one(jr *rd, jr_implicit *entry) {
    odin3_vec defs;
    odin3_vec_init(&defs, sizeof(odin3_port_def));
    odin3_status st = odin3_vec_reserve(&defs, entry->ports.len + 1);
    for (size_t i = 0; st == ODIN3_OK && i < entry->ports.len; i++) {
        jr_iport *port = odin3_vec_at(&entry->ports, i);
        port->def.name = jr_str(rd, port->name_str);
        odin3_port_def *def = odin3_vec_push(&defs);
        *def = port->def;
    }
    const char *name = jr_str(rd, entry->name_str);
    odin3_celltype_def def = {.name = name,
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = defs.data,
                              .n_ports = (uint32_t)defs.len};
    if (st == ODIN3_OK) {
        st = odin3_celltype_add_local(rd->design, &def, NULL);
        st = st == ODIN3_ERR_INVALID_ARG
                 ? jr_error(rd, NULL, NULL, "cannot make the implicit black box '%s'", name)
                 : jr_fail(rd, st);
    }
    if (st == ODIN3_OK) {
        warn_implicit(rd, name);
    }
    odin3_vec_free(&defs);
    return st;
}

odin3_status jr_make_implicit(jr *rd) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rd->implicit.len; i++) {
        st = make_implicit_one(rd, odin3_vec_at(&rd->implicit, i));
    }
    return st;
}
