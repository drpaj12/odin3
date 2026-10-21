/*
 * reader_internal.h — state and helpers shared by the files of the JSON reader (private to
 * src/frontends/json). The rule IDs (J, M, E, R) are those of the 5.3 spec.
 */
#ifndef ODIN3_FRONTENDS_JSON_READER_INTERNAL_H
#define ODIN3_FRONTENDS_JSON_READER_INTERNAL_H

#include "frontends/json/json_parse.h"
#include "frontends/json/reader.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* E-3: modules, netnames and cells are each at most 2^24 (the IR's IDs are 32 bits). */
enum {
    JR_MAX_OBJECTS = 1 << 24,
    JR_NAME_BUF = 160,
    JR_BAD_SHOW = 60,
    JR_BAD_BUF = (JR_BAD_SHOW * 4) + 1
};

/* The kinds of a bit of a connection (M-3). Constants 0, 1, x, z are numbered as odin3_bit. */
typedef enum jr_bit_kind { JR_BIT_NET = 4 } jr_bit_kind;
enum { JR_CONSTS = 4 };

typedef struct jr_bit {
    uint32_t kind; /* 0..3: constant (odin3_bit), JR_BIT_NET: an integer net label */
    uint64_t num;  /* the label of a JR_BIT_NET */
} jr_bit;

/* One JSON module, in file order. */
typedef struct jr_mod {
    const odin3_json_node *node;
    const char *name;
    uint32_t name_str;
    odin3_module_id module; /* the IR module; none for a black-box module */
    odin3_prov_id base;     /* its IMPORTED record: the nets and constants of the module take it */
    bool blackbox;
    bool top_attr;
    bool has_cells;
} jr_mod;

/* The module instantiation edge from module `from` to module `to` (indices into jr.mods). */
typedef struct jr_edge {
    uint32_t from, to;
} jr_edge;

/* What the cells showed of a port of an implicit black box that no cell gave port_directions
 * (reader_implicit_dirs.c): a cell needs it to drive its bits, its bits have no known direction,
 * it is an input to bits a known driver drives, another unknown port also drives its bits. */
enum { JR_ROLE_OUT = 1, JR_ROLE_BIDI = 2, JR_ROLE_IN_KNOWN = 4, JR_ROLE_CONFLICT = 8 };

/* A port of an implicit black box; the direction is inferred from the bits (jr_infer_implicit)
 * until a cell gives port_directions. */
typedef struct jr_iport {
    odin3_port_def def; /* name is set when the type is made */
    uint32_t name_str;
    bool dir_known;
    uint8_t role; /* JR_ROLE_* */
} jr_iport;

/* An implicit black box (M-5 rule 4, --auto-blackbox): its ports in first-use order. */
typedef struct jr_implicit {
    uint32_t name_str;
    uint64_t total_width;
    const odin3_json_node *first_cell; /* the first cell of the type, for messages */
    const char *module;                /* the module of that cell */
    odin3_vec ports;                   /* jr_iport */
} jr_implicit;

/* The state of the module whose netnames and cells are being read (pass 2). */
typedef struct jr_cur {
    const jr_mod *mod;
    odin3_module *module;
    odin3_u64map *nets;   /* integer bit label -> net ID */
    odin3_u64map *qbits;  /* label of a bit flip-flop or latch Q bit -> 1 */
    odin3_u64map *qinit;  /* label of a Q bit -> INIT value (0, 1 or 3) */
    odin3_u64map *netloc; /* label of a one-bit netname with a restored src -> its record */
    odin3_vec labels;     /* uint64_t: labels to create nets for */
    odin3_net_id consts[JR_CONSTS];
    uint32_t const_order[JR_CONSTS]; /* constants in order of first use */
    uint32_t n_consts;
    bool const_used[JR_CONSTS];
    odin3_vec conn_nets;  /* odin3_net_id: the pins of the cell being built */
    odin3_vec conn_ports; /* odin3_netvec: one per port of the cell being built */
    odin3_vec params;     /* odin3_value: parameters of the cell being built */
} jr_cur;

typedef struct jr {
    odin3_design *design;
    const char *path;
    uint32_t file;
    odin3_pass_ctx ctx;
    odin3_json_opts opts;
    odin3_json_doc doc;
    const odin3_json_node *modules; /* the root's "modules" object */
    odin3_vec mods;                 /* jr_mod, file order */
    odin3_u64map *mod_index;        /* module name str -> index in mods */
    odin3_vec edges;                /* jr_edge */
    odin3_vec implicit;             /* jr_implicit */
    odin3_u64map *implicit_index;   /* type name str -> index in implicit */
    odin3_u64map *implicit_port;    /* implicit type index << 32 | port name str -> port index */
    const char *cur_name;           /* name of the module being read, for messages */
    uint32_t n_unknown;             /* members skipped (J-8) */
    uint32_t n_ext;                 /* odin3_-prefixed members skipped (J-9) */
    uint32_t n_scopeinfo;           /* $scopeinfo cells dropped */
    uint32_t n_bad_src;             /* `src` texts that are no location list (L-6) */
    uint32_t n_capped_src;          /* restored texts of over 8 locations (D-7) */
    odin3_json_loc bad_src_pos;     /* where the first bad one is */
    char bad_src_text[JR_BAD_BUF];  /* its text, cut and escaped */
    bool op_begun;                  /* the restore operation of this read has begun (D-1) */
    char first_unknown[JR_NAME_BUF];
    bool oom_logged;
    jr_cur cur;
    odin3_arena *vals; /* payloads of typed values, until the read ends */
    odin3_strbuf scratch;
} jr;

/* NULL-safe views of the parser's accessors (an absent member is a NULL node). */
static inline const odin3_json_node *jr_first(const odin3_json_node *node) {
    return node != NULL ? odin3_json_first(node) : NULL;
}
static inline const odin3_json_node *jr_member(const odin3_json_node *obj, const char *key) {
    return obj != NULL ? odin3_json_member(obj, key) : NULL;
}
static inline odin3_json_kind jr_kind(const odin3_json_node *node) {
    return node != NULL ? odin3_json_kind_of(node) : ODIN3_JSON_NULL;
}
static inline size_t jr_count(const odin3_json_node *node) {
    return node != NULL ? odin3_json_count(node) : 0;
}
static inline const char *jr_text(const odin3_json_node *node, size_t *len) {
    return node != NULL ? odin3_json_string(node, len) : NULL;
}
static inline int64_t jr_int(const odin3_json_node *node) {
    return node != NULL ? odin3_json_int(node) : 0;
}

static inline const char *jr_key_of(const odin3_json_node *node) {
    return node != NULL ? odin3_json_key(node) : NULL;
}

/* Where in the module an error is: modules.<module>.<section>.<key>.<member>; NULL parts are
 * left out. */
typedef struct jr_where {
    const char *section;
    const char *key;
    const char *member;
} jr_where;

/* --- reader.c ---------------------------------------------------------------------------- */

/* Logs "path:line:col: modules.<m>.<where>: message" at the value's position (the key for a
 * member), returns ODIN3_ERR_PARSE. node may be NULL (position 0:0 is then left out). */
odin3_status jr_error(const jr *rd, const odin3_json_node *node, const jr_where *where,
                      const char *fmt, ...) ODIN3_PRINTF(4, 5);
/* Logs out of memory once; passes st through. */
odin3_status jr_fail(jr *rd, odin3_status st);
/* The IMPORTED record at the key position of node (a member), or at its value position. */
odin3_status jr_prov(jr *rd, const odin3_json_node *node, odin3_prov_id *out);
/* A name made of a prefix and a member key ("attributes.k", "parameter:K") for a message or an
 * attribute key: node (its key position) and where locate the error when it would not fit. */
typedef struct jr_join_args {
    const odin3_json_node *node;
    const jr_where *where;
    const char *prefix;
    const char *name;
} jr_join_args;
odin3_status jr_join(const jr *rd, const jr_join_args *args, char *buf, size_t size);
/* Interns text in the design strtab. */
odin3_status jr_intern(jr *rd, const char *text, uint32_t *out);
const char *jr_str(const jr *rd, uint32_t str);

/* --- reader_locs.c: the src restore (5.4 D-1, D-2, L-6, L-8, D-7) -------------------------- */

/* True when the attributes object attrs has a `src` that parses as HDL locations and the read
 * restores it (no --keep-src-attr): the attribute is then not set. Pure, no message. */
bool jr_src_restorable(const jr *rd, const odin3_json_node *attrs);
/* The same for the member `src` itself (a member of an attributes object). */
bool jr_src_member_restorable(const jr *rd, const odin3_json_node *member);
/* What to restore for one object: its attributes object (may be NULL), the record the reader
 * would make without 5.4 (IMPORTED), and whether a `src` that does not parse is reported (L-6). */
typedef struct jr_restore_in {
    const odin3_json_node *attrs;
    odin3_prov_id base;
    bool warn;
} jr_restore_in;
/* *out is the record the object takes: the DERIVED of D-1 when its `src` restores, else base. */
odin3_status jr_restore_src(jr *rd, const jr_restore_in *in, odin3_prov_id *out);
/* The netnames of a module that hold a restorable `src`, by name, for its ports (D-2). */
typedef struct jr_port_srcs {
    odin3_u64map *by_name; /* name str -> index in nodes */
    odin3_vec nodes;       /* const odin3_json_node *: the netname objects */
} jr_port_srcs;
odin3_status jr_port_srcs_open(jr *rd, const odin3_json_node *mod, jr_port_srcs *srcs);
void jr_port_srcs_close(jr_port_srcs *srcs);
/* The record of the port `port` (base: its IMPORTED record): that of its netname's `src`. */
odin3_status jr_port_prov(jr *rd, const jr_port_srcs *srcs, const odin3_json_node *port,
                          odin3_prov_id base, odin3_prov_id *out);
/* The records of the one-bit netnames of the module with a restorable `src`, by net label. */
odin3_status jr_collect_netlocs(jr *rd);
/* The WARNING and INFO lines of the read (L-6, D-7). */
void jr_report_src(const jr *rd);

/* --- reader_ext.c: J-8, J-9 -------------------------------------------------------------- */

/* The members of obj that are not in known[] (n entries) are counted as skipped (an `odin3_`
 * prefixed one is offered to the extension table first). */
void jr_audit(jr *rd, const odin3_json_node *obj, const char *const *known, size_t count);
/* `odin3_version` of the root: above 1 is an error. */
odin3_status jr_check_version(const jr *rd, const odin3_json_node *root);
/* One INFO line with the counts, when anything was skipped. */
void jr_report_skipped(const jr *rd);

/* --- reader_value.c: J-10, M-6 ----------------------------------------------------------- */

/* A value of a JSON attribute, typed by J-10, in a value whose payload lives in the cur scratch.
 * ODIN3_ERR_PARSE (located) for an array or an object. */
odin3_status jr_attr_value(jr *rd, const odin3_json_node *node, const jr_where *where,
                           odin3_value *out);
/* Sets every member of the attributes object attrs on obj of the current module, J-10 typed,
 * except the keys of skip (NULL: none). */
odin3_status jr_set_attrs(jr *rd, odin3_module *module, odin3_objref obj,
                          const odin3_json_node *attrs, const jr_where *where);
/* Sets the attribute named like member (a member of an attributes object) on obj. */
odin3_status jr_set_attr_member(jr *rd, odin3_module *module, odin3_objref obj,
                                const odin3_json_node *member, const jr_where *where);
/* Sets one attribute with the name key (not interned yet). */
odin3_status jr_set_attr_named(jr *rd, odin3_module *module, odin3_objref obj, const char *key,
                               const odin3_value *val);
/* True when attribute node is a non-zero integer or a bit vector with a 1 (Yosys's as_bool). */
bool jr_attr_true(const odin3_json_node *node);
/* A typed parameter value (M-6) of kind kind from node. */
odin3_status jr_param_value(jr *rd, odin3_value_kind kind, const odin3_json_node *node,
                            const jr_where *where, odin3_value *out);
/* The J-10 string rule: a copy of the STRING value text (len bytes) as Yosys writes a trailing
 * blank; *out_len is the length to keep. */
size_t jr_string_len(const char *text, size_t len);
/* Allocates len bytes that live until the read ends; NULL on out of memory. */
uint8_t *jr_alloc(jr *rd, size_t len);

/* --- reader_module.c: modules, ports, top ------------------------------------------------ */

odin3_status jr_read_modules(jr *rd);
/* Pass 2 of one module. */
odin3_status jr_read_module_body(jr *rd, uint32_t index);
/* The module index of the type name, or UINT32_MAX. */
uint32_t jr_find_mod(const jr *rd, uint32_t name_str);
odin3_status jr_choose_top(jr *rd);
odin3_status jr_check_cycles(jr *rd);
/* Scans the cells of every module for types, edges and implicit black boxes (before pass 2). */
odin3_status jr_scan_types(jr *rd);

/* --- reader_net.c: bits and nets (M-3) --------------------------------------------------- */

/* Parses one bit; kind JR_BIT_NET for an integer label (>= 0). */
odin3_status jr_bit_parse(const jr *rd, const odin3_json_node *node, const jr_where *where,
                          uint32_t index, jr_bit *out);
/* Starts the per-module bit state; frees it. */
odin3_status jr_cur_begin(jr *rd, const jr_mod *mod);
void jr_cur_end(jr *rd);
/* Collects the labels and constants of the module's ports, netnames and cells, and the Q bits
 * and INIT values of its latches (M-3, M-11). */
odin3_status jr_scan_bits(jr *rd);
/* Creates the nets (ports first, then ascending labels, then constants) and the constant nodes. */
odin3_status jr_make_nets(jr *rd);
/* The net of a bit, none when a constant output pin is open (not an error). */
odin3_net_id jr_net_of(const jr *rd, const jr_bit *bit);
/* Check rule 4 at the source: the number of drivers of net that are neither inout pins nor
 * outputs of a tristate type. */
uint32_t jr_ordinary_drivers(const jr *rd, odin3_net_id net);
bool jr_is_bus_driver(const jr *rd, odin3_pin_id pin);
/* Starts the zeroed cur state's vectors (once, in jr_init). */
void jr_cur_init(jr *rd);
/* The element count of a bits array, or an error when it is no array, is empty (unless empty_ok:
 * a connection of a port of no bits) or is above the cap. */
odin3_status jr_bits_count(const jr *rd, const odin3_json_node *bits, const jr_where *where,
                           bool empty_ok, uint32_t *count);
/* The registered or declared cell type of a name; false when there is none. */
bool jr_lookup_type(const jr *rd, const char *name, odin3_celltype_id *id);

/* --- reader_wire.c: netnames and memories ------------------------------------------------ */

odin3_status jr_read_netnames(jr *rd, const odin3_json_node *obj);
odin3_status jr_read_memories(jr *rd, const odin3_json_node *obj);
/* The width of a port or netname and the spec whose msb, lsb and sign jr_shape sets. */
typedef struct jr_shape_out {
    uint32_t width;
    odin3_wire_spec *spec;
} jr_shape_out;
/* The range of a wire from offset/upto and its width; an offset out of range is a located error
 * in the section (ports or netnames) of obj. */
odin3_status jr_shape(const jr *rd, const odin3_json_node *obj, const char *section,
                      jr_shape_out out);

/* --- reader_cell.c: cells ---------------------------------------------------------------- */

odin3_status jr_read_cells(jr *rd, const odin3_json_node *obj);
/* M-5 rule 4: a cell of a type that is none of a module, a registered type or a black box of the
 * file is a located error, or with --auto-blackbox is noted for an implicit black box. */
odin3_status jr_note_implicit(jr *rd, const odin3_json_node *cell, const char *type_name);
/* Implicit black boxes: the types the scan found (declared after pass 1, before pass 2). */
odin3_status jr_make_implicit(jr *rd);
/* EL-11: the direction of every implicit port no cell gave port_directions for, from the bits its
 * cells connect it to (reader_implicit_dirs.c); before jr_make_implicit. */
odin3_status jr_infer_implicit(jr *rd);
/* The direction the roles of a port settle on: inout, else output, else input. */
odin3_dir jr_role_dir(uint8_t role);
void jr_free_implicit(jr *rd);

/* --- reader_sop.c: `$sop` and `$lut` --------------------------------------------------- */

/* Rewrites the parameters of a Yosys `$sop` (DEPTH, TABLE, WIDTH) as the registry's (WIDTH, COVER)
 * into rd->cur.params. */
odin3_status jr_sop_params(jr *rd, const odin3_json_node *params, const jr_where *where);
/* A `$lut` with WIDTH 0 (the writer's constant cover) as the `$sop` it was: true and the
 * parameters of that `$sop` in rd->cur.params when params are those of such a `$lut`. */
typedef struct jr_lut_sop {
    const odin3_celltype_def *lut;
    const odin3_celltype_def *sop;
} jr_lut_sop;
odin3_status jr_lut0_params(jr *rd, const jr_lut_sop *defs, bool *converted);
/* The index of the parameter called name in def, -1 when it has none. */
int jr_find_param(const odin3_celltype_def *def, const char *name);

#endif
