/*
 * jw.h — shared state and helpers of the Yosys-schema JSON writer (private to src/backends/json).
 */
#ifndef ODIN3_BACKENDS_JSON_JW_H
#define ODIN3_BACKENDS_JSON_JW_H

#include "backends/common/attrs.h"
#include "backends/common/hash_rows.h"
#include "backends/common/hdl_loc.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* A bit as written: 0 and 1 are the constants "0" and "1", JW_BIT_X / JW_BIT_Z the strings
 * "x" / "z", anything else an integer bit number (net ID + 1, so >= 2). */
#define JW_BIT_0 UINT32_C(0)
#define JW_BIT_1 UINT32_C(1)
#define JW_BIT_X (UINT32_MAX - 1)
#define JW_BIT_Z UINT32_MAX

/* How cell and netname keys are formed (the write_json --name-style). COLLECT and APPLY are the
 * two passes of HASH: the first writes provenance names to a sink and collects the objects it
 * renamed, the second writes the hash names the first resolved (names.c). */
typedef enum jw_names_mode {
    JW_NAMES_SHORT,
    JW_NAMES_PROVENANCE,
    JW_NAMES_COLLECT,
    JW_NAMES_APPLY
} jw_names_mode;

/* Output state; one writer per odin3_json_write call, module fields reset per module. */
typedef struct jw {
    FILE *fp;      /* NULL for the counting sink of the first hash pass */
    uint64_t sunk; /* bytes the counting sink took */
    odin3_design *design;
    const odin3_strtab *strtab;
    odin3_module *module;
    uint8_t *init;          /* per net ID: 0 = none, else '0', '1' or 'x' (latch Q initial value) */
    uint32_t extra_bit;     /* next bit number no net uses (helper nets of OFF-set $sop) */
    odin3_status status;    /* first failure; later output is skipped */
    int write_errno;        /* errno of the failed write when status is ODIN3_ERR_IO */
    odin3_hdl_ctx *hdl;     /* HDL locations of the objects (backends/common/hdl_loc.h) */
    odin3_strbuf src;       /* scratch: the src text jw_src builds */
    odin3_prov_id src_prov; /* the prov out->src holds the text of (0: none) */
    uint32_t src_written;   /* its HDL location count */
    odin3_strbuf key;       /* scratch: the key being built by jw_make_key */
    odin3_wattr_seen seen;  /* keys written in the current attributes/parameters object */
    odin3_strtab *written;  /* cell and netname keys written in the current module */
    bool odin3_keys;        /* write_json --odin3: the Odin III keys (E-2) */
    uint32_t level_key;     /* strtab ID of "odin3_level", 0 when the design has none */
    jw_names_mode names;    /* how keys are formed */
    odin3_hash_rows rows;   /* COLLECT/APPLY: the objects named by provenance, all modules */
    size_t row_cursor;      /* APPLY: the next row to use */
    size_t module_rows;     /* COLLECT: the first row of the current module */
    odin3_strbuf pname;     /* scratch: a provenance name */
    odin3_strbuf pshort;    /* scratch: the short name it wraps */
    bool hide;              /* the key last made would be hidden (its provenance name starts `$`) */
} jw;

/* A JSON object or array being written: depth is its members' indentation level. */
typedef struct jw_list {
    bool any;
    uint32_t depth;
} jw_list;

/* --- emit.c: text ---------------------------------------------------------------------------- */

void jw_raw(jw *out, const char *text);
void jw_fmt(jw *out, const char *fmt, ...) ODIN3_PRINTF(2, 3);
/* "bytes" with JSON escapes. */
void jw_string(jw *out, odin3_bytes text);
void jw_char(jw *out, int chr);
void jw_indent(jw *out, uint32_t depth);
/* Cell and netname keys share one namespace per module, as Yosys RTLIL does (IR-14): every key
 * written is recorded, and a key that is already written, or that names a wire or net (whose
 * netname keeps the user's name), or, when generated, names another node, gets "$u<n>" appended
 * until it is free. A user netname is bumped only past a written key; a user cell name past a
 * wire or net name too (cells carry hide_name, so the visible net name wins).
 */
typedef enum jw_keykind { JW_KEY_CELL, JW_KEY_NETNAME } jw_keykind;
typedef struct jw_keyspec {
    jw_keykind kind;
    bool generated; /* false: head+tail is a user name, used as is */
    const char *head;
    const char *tail;
    odin3_prov_id prov;       /* the object's provenance (none: the key is never renamed) */
    odin3_name_kind obj_kind; /* with prov: the object's kind and ID, for the name map */
    uint32_t obj_id;
} jw_keyspec;
/* The unique key text (valid until the next call), recorded as written; "" after an
 * out-of-memory status. With a names mode other than SHORT, an object with an HDL location
 * (spec.prov) has `hier/<head+tail>@file:line` for its key, or in APPLY its hash name; out->hide
 * says whether the key written would start with `$` without the hash. */
const char *jw_make_key(jw *out, const jw_keyspec *spec);
/* The same, never renamed by provenance (emit.c). */
const char *jw_plain_key(jw *out, const jw_keyspec *spec);
/* names.c: the renamed key of spec, or NULL when the mode is SHORT or the object has no HDL
 * location (then jw_plain_key applies). Recorded as written. */
const char *jw_prov_key(jw *out, const jw_keyspec *spec);
/* names.c: COLLECT, at the end of a module: resolves the module's rows to hash names (H-3). */
void jw_names_module_end(jw *out);
/* names.c: APPLY, at the end of the design: every collected row must have been used. */
void jw_names_done(jw *out);
/* Records a key written as is (a wire's netname). */
void jw_record_key(jw *out, const char *key);

/* A name in two parts (head then tail), written inside one pair of quotes. */
typedef struct jw_name {
    odin3_bytes head;
    odin3_bytes tail;
} jw_name;
void jw_string2(jw *out, const jw_name *name);
/* The strtab string id, quoted. */
void jw_str_id(jw *out, uint32_t str);
/* '{' and an empty list at depth; jw_item starts the next member; jw_close ends with '}'. */
void jw_open(jw *out, jw_list *list);
void jw_item(jw *out, jw_list *list);
void jw_close(jw *out, const jw_list *list);
/* `"key": ` as the next member of list. */
void jw_key(jw *out, jw_list *list, const char *key);

/* --- emit.c: bits, values, provenance ----------------------------------------------------------
 */

uint32_t jw_net_code(const jw *out, odin3_net_id net);
void jw_bit(jw *out, uint32_t code);
/* `[ a, b, c ]` over the nets of a pin slice (LSB first); unconnected pins are "x". */
void jw_pins(jw *out, odin3_pinslice pins);
/* `"key": "binary"` for an int (32 bits, or 64 when it does not fit) as the next member. */
void jw_param_int(jw *out, jw_list *list, const char *key, int64_t num);
/* `"key": <value>` for a cell parameter value as the next member. */
void jw_param(jw *out, jw_list *list, const char *key, const odin3_value *val);
/* Adds `"src": "SRC"` for prov's HDL locations (Yosys's form, `file:L.C-EL.EC`, up to 8 joined by
 * `|`; only SOURCE leaves, spec 5.4 W-2 to W-4), if it has one and no "src" was written to this
 * object yet (a user src attribute wins). */
void jw_src(jw *out, jw_list *list, odin3_prov_id prov);
/* Under --odin3 (E-2), as further members of list: `odin3_level` when obj has the INT attribute of
 * that name (the attribute is then not repeated under `attributes`), then `odin3_prov`
 * {kind, hier, hdl} of prov (left out when prov is none). Nothing otherwise. */
void jw_odin3_keys(jw *out, jw_list *list, odin3_objref obj, odin3_prov_id prov);
/* True (and recorded) when key is new in the current object; false after out of memory. */
bool jw_claim(jw *out, odin3_wattr_role role, const char *key);
/* The attributes of obj with the given role as members of list (odin3_attr_foreach order),
 * skipping keys already written to this object. Call odin3_wattr_seen_clear(&out->seen) when a
 * new attributes or parameters object starts. */
void jw_user_attrs(jw *out, jw_list *list, odin3_objref obj, odin3_wattr_role role);

/* --- cells.c ------------------------------------------------------------------------------------
 */

/* Writes every cell of out->module as members of list. */
void jw_cells(jw *out, jw_list *list);

/* --- lut.c: $sop
 * ---------------------------------------------------------------------------------- */

/* Writes node (a $sop) as $lut, or as $sop (+ $not for an OFF-set cover) beyond 6 inputs. */
void jw_sop_cell(jw *out, jw_list *list, odin3_node_id node);

/* --- cell framing shared by cells.c and lut.c ---------------------------------------------------
 */

/* The cell key: the node's name (or $c<ID>) followed by suffix; type is the "type" field. */
typedef struct jw_cell_ref {
    odin3_node_id node;
    const char *suffix;
    const char *type;
} jw_cell_ref;

/* The state of one cell being written: its parent list and its three member lists. */
typedef struct jw_cell {
    const jw_list *cells;
    odin3_node_id node;
    jw_list params;
    jw_list dirs;
    jw_list conns;
} jw_cell;

/* `"name": { "hide_name", "type", "parameters": {` ; cell->params then takes the parameters. */
void jw_cell_begin(jw *out, jw_list *cells, const jw_cell_ref *ref, jw_cell *cell);
/* A real parameter of the cell being written: as jw_param_int / jw_key on cell->params, and the
 * name is claimed so a `parameter:<NAME>` attribute of the same name is not written twice. */
void jw_cell_param_int(jw *out, jw_cell *cell, const char *key, int64_t num);
void jw_cell_param_key(jw *out, jw_cell *cell, const char *key);
/* Closes parameters; writes attributes (src); opens port_directions (cell->dirs). */
void jw_cell_attrs(jw *out, jw_cell *cell, odin3_node_id node);
/* Closes port_directions and opens connections (cell->conns). */
void jw_cell_conns(jw *out, jw_cell *cell);
/* Closes connections and the cell. */
void jw_cell_end(jw *out, const jw_cell *cell);
/* `"name": "input|output|inout"` as the next direction. */
void jw_dir(jw *out, jw_cell *cell, const char *name, odin3_dir dir);

#endif
