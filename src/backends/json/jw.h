/*
 * jw.h — shared state and helpers of the Yosys-schema JSON writer (private to src/backends/json).
 */
#ifndef ODIN3_BACKENDS_JSON_JW_H
#define ODIN3_BACKENDS_JSON_JW_H

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

/* Output state; one writer per odin3_json_write call, module fields reset per module. */
typedef struct jw {
    FILE *fp;
    odin3_design *design;
    const odin3_strtab *strtab;
    odin3_module *module;
    uint8_t *init;       /* per net ID: 0 = none, else '0', '1' or 'x' (latch Q initial value) */
    uint32_t extra_bit;  /* next bit number no net uses (helper nets of OFF-set $sop) */
    odin3_status status; /* first failure; later output is skipped */
    odin3_srcloc src;    /* scratch: first source location found by jw_src */
    bool have_src;
    odin3_strbuf key; /* scratch: the key being built by jw_make_key */
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
/* Keys of one JSON object must be unique. A user name always wins: a generated key (suffix or
 * $c<ID>/$n<ID>) that equals a name of the same namespace gets "$u<n>" appended until it is free.
 */
typedef enum jw_keykind { JW_KEY_CELL, JW_KEY_NETNAME } jw_keykind;
typedef struct jw_keyspec {
    jw_keykind kind;
    bool generated; /* false: head+tail is a user name, used as is */
    const char *head;
    const char *tail;
} jw_keyspec;
/* The unique key text (valid until the next call); "" after an out-of-memory status. */
const char *jw_make_key(jw *out, const jw_keyspec *spec);

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
/* Adds `"src": "file:line.col"` for prov's first source location, if it has one. */
void jw_src(jw *out, jw_list *list, odin3_prov_id prov);

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
    jw_list params;
    jw_list dirs;
    jw_list conns;
} jw_cell;

/* `"name": { "hide_name", "type", "parameters": {` ; cell->params then takes the parameters. */
void jw_cell_begin(jw *out, jw_list *cells, const jw_cell_ref *ref, jw_cell *cell);
/* Closes parameters; writes attributes (src); opens port_directions (cell->dirs). */
void jw_cell_attrs(jw *out, jw_cell *cell, odin3_node_id node);
/* Closes port_directions and opens connections (cell->conns). */
void jw_cell_conns(jw *out, jw_cell *cell);
/* Closes connections and the cell. */
void jw_cell_end(jw *out, const jw_cell *cell);
/* `"name": "input|output|inout"` as the next direction. */
void jw_dir(jw *out, jw_cell *cell, const char *name, odin3_dir dir);

#endif
