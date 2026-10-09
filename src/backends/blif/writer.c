/* writer.c — BLIF writer: modules, cells and declared black boxes of a design. */
#include "backends/blif/writer.h"

#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    WRAP_COLUMN = 100, /* no wrapped line is longer */
    WRAP_MARK = 2,     /* the ` \` that ends a wrapped line */
    INDENT = 2,        /* continuation lines start with two spaces */
    IO_BUFFER = 1 << 20,
    PIN_KEY_SHIFT = 32, /* generated-name key: a net ID, or 1 << 32 | pin ID */
};

/* The built-in cell types written as .names and .latch; everything else is a .subckt. */
typedef enum wr_builtin {
    WR_SOP,
    WR_DFF_P,
    WR_DFF_N,
    WR_DLATCH_P,
    WR_DLATCH_N,
    WR_FF,
    WR_BUILTINS
} wr_builtin;

static const char *const BUILTIN_NAMES[WR_BUILTINS] = {"$sop",        "$_DFF_P_",    "$_DFF_N_",
                                                       "$_DLATCH_P_", "$_DLATCH_N_", "$_FF_"};

/* The .latch type word of each built-in; NULL for $sop and for $_FF_ (no type, no control). */
static const char *const LATCH_WORDS[WR_BUILTINS] = {NULL, "re", "fe", "ah", "al", NULL};

/* $sop parameters, and ports of the .latch types (with and without a control pin). */
enum { SOP_WIDTH, SOP_COVER };
enum { LATCH_INIT = 0, CTRL_PORT = 0, TYPED_D = 1, TYPED_Q = 2, FF_D = 0, FF_Q = 1 };

/* Strtab IDs of the attribute keys the reader sets; 0 when never interned (no object has it). */
typedef struct wr_keys {
    uint32_t clock, port_name, extras;
} wr_keys;

typedef struct blif_writer {
    const odin3_design *design;
    const odin3_strtab *tab;
    const char *path;
    FILE *file;
    char *buffer;    /* the stdio buffer of file */
    uint32_t line;   /* complete lines written so far */
    size_t col;      /* columns on the line being written */
    bool line_empty; /* nothing but the indent on the line being written */
    bool oom_logged;
    odin3_status st;   /* sticky: the first failure */
    const char *model; /* name of the model being written, for messages */
    odin3_celltype_id builtin[WR_BUILTINS];
    wr_keys keys;
    odin3_u64map *type_module; /* module cell type -> module ID */
    odin3_u64map *written;     /* black-box cell types already written -> 1 */
    odin3_strbuf token;        /* the token being built */
    odin3_strbuf cand;         /* a candidate generated name */
    /* the module being written */
    const odin3_module *module;
    odin3_strtab *gen_names; /* its generated names (NULL until the first) */
    odin3_u64map *gen;       /* generated-name key -> gen_names ID (NULL until the first) */
    odin3_u64map *port_of;   /* port wire -> port index (NULL until needed) */
} blif_writer;

/* --- errors and output --------------------------------------------------------------------- */

/* Records st as the writer's status unless one is already set; logs out of memory once. */
static odin3_status wr_fail(blif_writer *wr, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY && !wr->oom_logged) {
        wr->oom_logged = true;
        odin3_log(ODIN3_LOG_ERROR, "%s: out of memory", wr->path);
    }
    if (wr->st == ODIN3_OK) {
        wr->st = st;
    }
    return wr->st;
}

/* Logs "path: model '<model>': <what> '<name>'" and fails with ODIN3_ERR_INVALID_ARG. */
static odin3_status wr_refuse(blif_writer *wr, const char *what, const char *name) {
    odin3_log(ODIN3_LOG_ERROR, "%s: model '%s': %s '%s'", wr->path, wr->model, what, name);
    return wr_fail(wr, ODIN3_ERR_INVALID_ARG);
}

static void wr_put(blif_writer *wr, odin3_bytes data) {
    if (wr->st != ODIN3_OK || data.len == 0) {
        return;
    }
    if (fwrite(data.ptr, 1, data.len, wr->file) != data.len) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: write failed: %s", wr->path, (unsigned)wr->line + 1,
                  strerror(errno));
        (void)wr_fail(wr, ODIN3_ERR_IO);
    }
}

static void wr_newline(blif_writer *wr) {
    wr_put(wr, odin3_bytes_cstr("\n"));
    wr->line++;
}

/* Starts a directive line with its keyword. */
static void wr_begin(blif_writer *wr, const char *keyword) {
    wr_put(wr, odin3_bytes_cstr(keyword));
    wr->col = strlen(keyword);
    wr->line_empty = false;
}

/* Appends one token, wrapping first when the line (with its ` \`) would pass WRAP_COLUMN. */
static void wr_token(blif_writer *wr, odin3_bytes tok) {
    if (!wr->line_empty && wr->col + 1 + tok.len + WRAP_MARK > WRAP_COLUMN) {
        wr_put(wr, odin3_bytes_cstr(" \\"));
        wr_newline(wr);
        wr_put(wr, odin3_bytes_cstr("  "));
        wr->col = INDENT;
        wr->line_empty = true;
    }
    if (!wr->line_empty) {
        wr_put(wr, odin3_bytes_cstr(" "));
        wr->col++;
    }
    wr_put(wr, tok);
    wr->col += tok.len;
    wr->line_empty = false;
}

/* Appends the token built in wr->token. */
static void wr_token_built(blif_writer *wr) {
    wr_token(wr, (odin3_bytes){wr->token.data, wr->token.len});
}

static void wr_end(blif_writer *wr) {
    wr_newline(wr);
}

static odin3_bytes str_bytes(const blif_writer *wr, uint32_t str) {
    return (odin3_bytes){odin3_strtab_get(wr->tab, str), odin3_strtab_len(wr->tab, str)};
}

/* --- generated names ----------------------------------------------------------------------- */

/* The STRING attribute key of obj in the module being written, or NULL. */
static const odin3_value *string_attr(const odin3_module *module, odin3_objref obj, uint32_t key) {
    const odin3_value *val = key == 0 ? NULL : odin3_attr_get(module, obj, key);
    return val != NULL && val->kind == ODIN3_VAL_STRING ? val : NULL;
}

/* True when no net of the module but self has the name, and no generated name is it. */
static bool name_free(const blif_writer *wr, odin3_bytes text, odin3_net_id self) {
    uint32_t str = 0;
    if (odin3_strtab_find(wr->tab, text, &str)) {
        odin3_net_id have = odin3_module_find_net(wr->module, str);
        if (odin3_net_valid(have) && have.v != self.v) {
            return false;
        }
    }
    return wr->gen_names == NULL || !odin3_strtab_find(wr->gen_names, text, NULL);
}

/* Index of the port whose wire is `wire`, through wr->port_of (built on first use). */
static odin3_status port_index(blif_writer *wr, odin3_wire_id wire, uint64_t *index) {
    if (wr->port_of == NULL) {
        wr->port_of = odin3_u64map_create(0);
        if (wr->port_of == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < odin3_module_port_count(wr->module); i++) {
            odin3_wire_id have = odin3_module_port_wire(wr->module, i);
            if (odin3_u64map_put(wr->port_of, (odin3_kv){have.v, i}) != ODIN3_OK) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
    }
    *index = UINT64_MAX;
    (void)odin3_u64map_get(wr->port_of, wire.v, index);
    return ODIN3_OK;
}

/* True when bit names of wire carry no index: a scalar port, or a 1-bit wire that is no port. */
static odin3_status wire_is_scalar(blif_writer *wr, odin3_wire_id wire, bool *scalar) {
    *scalar = odin3_wire_width(wr->module, wire) == 1;
    if (!odin3_node_valid(odin3_wire_port_node(wr->module, wire))) {
        return ODIN3_OK;
    }
    uint64_t index = 0;
    odin3_status st = port_index(wr, wire, &index);
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
    if (st == ODIN3_OK && index < def->n_ports) {
        *scalar = def->ports[index].scalar;
    }
    return st;
}

/* wr->cand = the name of net's primary wire bit; *found false when the net has none. */
static odin3_status wire_bit_name(blif_writer *wr, odin3_net_id net, bool *found) {
    *found = false;
    odin3_wirebit wb = odin3_net_primary(wr->module, net);
    if (!odin3_wire_valid(wb.wire)) {
        return ODIN3_OK;
    }
    const odin3_value *blif =
        string_attr(wr->module, (odin3_objref){ODIN3_OBJ_WIRE, wb.wire.v}, wr->keys.port_name);
    uint32_t base = blif != NULL ? blif->str : odin3_wire_name(wr->module, wb.wire);
    bool scalar = false;
    odin3_status st = base == 0 ? ODIN3_OK : wire_is_scalar(wr, wb.wire, &scalar);
    if (base == 0 || st != ODIN3_OK) {
        return st;
    }
    odin3_strbuf_clear(&wr->cand);
    st = odin3_strbuf_append(&wr->cand, str_bytes(wr, base));
    if (st == ODIN3_OK && !scalar) {
        st = odin3_strbuf_appendf(&wr->cand, "[%d]",
                                  (int)odin3_wire_index(wr->module, wb.wire, wb.bit));
    }
    *found = st == ODIN3_OK;
    return st;
}

/* Interns wr->cand as the generated name of key. */
static odin3_status take_name(blif_writer *wr, uint64_t key) {
    if (wr->gen_names == NULL) {
        wr->gen_names = odin3_strtab_create();
        wr->gen = odin3_u64map_create(0);
        if (wr->gen_names == NULL || wr->gen == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    uint32_t id = 0;
    odin3_status st =
        odin3_strtab_intern(wr->gen_names, (odin3_bytes){wr->cand.data, wr->cand.len}, &id);
    return st == ODIN3_OK ? odin3_u64map_put(wr->gen, (odin3_kv){key, id}) : st;
}

/* Takes the first free of `$n<ID>` (a net key) or `$p<ID>` (a pin key), `…$1`, `…$2`, … */
static odin3_status take_numbered(blif_writer *wr, uint64_t key) {
    bool pin = (key >> PIN_KEY_SHIFT) != 0;
    odin3_net_id self = {pin ? 0 : (uint32_t)key};
    odin3_status st = ODIN3_OK;
    for (uint32_t suffix = 0; st == ODIN3_OK; suffix++) {
        odin3_strbuf_clear(&wr->cand);
        st = odin3_strbuf_appendf(&wr->cand, "$%c%u", pin ? 'p' : 'n', (unsigned)(uint32_t)key);
        if (st == ODIN3_OK && suffix > 0) {
            st = odin3_strbuf_appendf(&wr->cand, "$%u", (unsigned)suffix);
        }
        if (st == ODIN3_OK && name_free(wr, (odin3_bytes){wr->cand.data, wr->cand.len}, self)) {
            return take_name(wr, key);
        }
    }
    return st;
}

/* The generated name of key (an unnamed net, or 1 << 32 | an unconnected pin): the net's wire bit
 * name when free, else take_numbered. */
static odin3_status generate(blif_writer *wr, uint64_t key) {
    bool pin = (key >> PIN_KEY_SHIFT) != 0;
    odin3_net_id self = {pin ? 0 : (uint32_t)key};
    bool found = false;
    odin3_status st = pin ? ODIN3_OK : wire_bit_name(wr, self, &found);
    if (st != ODIN3_OK) {
        return st;
    }
    if (found && name_free(wr, (odin3_bytes){wr->cand.data, wr->cand.len}, self)) {
        return take_name(wr, key);
    }
    return take_numbered(wr, key);
}

/* The generated name of key; it exists for every unnamed net and every pin name_pins covered. */
static odin3_bytes gen_get(const blif_writer *wr, uint64_t key) {
    uint64_t id = 0;
    if (wr->gen == NULL || !odin3_u64map_get(wr->gen, key, &id)) {
        return (odin3_bytes){"", 0};
    }
    return (odin3_bytes){odin3_strtab_get(wr->gen_names, (uint32_t)id),
                         odin3_strtab_len(wr->gen_names, (uint32_t)id)};
}

static odin3_bytes net_name(const blif_writer *wr, odin3_net_id net) {
    uint32_t name = odin3_net_name(wr->module, net);
    return name != 0 ? str_bytes(wr, name) : gen_get(wr, net.v);
}

static odin3_bytes pin_name(const blif_writer *wr, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(wr->module, pin);
    return odin3_net_valid(net) ? net_name(wr, net)
                                : gen_get(wr, ((uint64_t)1 << PIN_KEY_SHIFT) | pin.v);
}

/* The built-in index of type, WR_BUILTINS when it is none of them. */
static wr_builtin builtin_of(const blif_writer *wr, odin3_celltype_id type) {
    for (uint32_t i = 0; i < WR_BUILTINS; i++) {
        if (wr->builtin[i].v != 0 && wr->builtin[i].v == type.v) {
            return (wr_builtin)i;
        }
    }
    return WR_BUILTINS;
}

/* True for a node whose pins are written by net name even when unconnected (.names, .latch and
 * port nodes); .subckt skips unconnected pins. */
static bool names_every_pin(const blif_writer *wr, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    return builtin_of(wr, type) != WR_BUILTINS ||
           odin3_celltype_get(wr->design, type)->gran == ODIN3_GRAN_PORT;
}

/* Generates names for the unconnected pins of a node written by pin (see names_every_pin). */
static odin3_status name_pins(blif_writer *wr, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(wr->module, node);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        uint32_t pin = pins.first.v + k;
        if (!odin3_net_valid(odin3_pin_net(wr->module, (odin3_pin_id){pin}))) {
            st = generate(wr, ((uint64_t)1 << PIN_KEY_SHIFT) | pin);
        }
    }
    return st;
}

/* Generates names for the module's unnamed nets (ID order), then its pins that need one. */
static odin3_status name_module(blif_writer *wr) {
    const odin3_module *module = wr->module;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_net_end(module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(module, net) && odin3_net_name(module, net) == 0) {
            st = generate(wr, i);
        }
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) && names_every_pin(wr, node)) {
            st = name_pins(wr, node);
        }
    }
    return st;
}

/* --- ports --------------------------------------------------------------------------------- */

/* The directive of a port's direction; INOUT has none in BLIF. */
static odin3_status port_keyword(blif_writer *wr, const odin3_port_def *port, const char **word) {
    if (port->dir == ODIN3_DIR_INOUT) {
        return wr_refuse(wr, "BLIF has no inout ports; cannot write port", port->name);
    }
    *word = port->dir == ODIN3_DIR_IN ? ".inputs" : ".outputs";
    return ODIN3_OK;
}

/* Starts a new .inputs/.outputs line for port `at` when its direction differs from the previous
 * port's. */
static odin3_status port_line(blif_writer *wr, const odin3_celltype_def *def, uint32_t at) {
    const char *word = NULL;
    odin3_status st = port_keyword(wr, &def->ports[at], &word);
    if (st == ODIN3_OK && (at == 0 || def->ports[at].dir != def->ports[at - 1].dir)) {
        if (at > 0) {
            wr_end(wr);
        }
        wr_begin(wr, word);
    }
    return st;
}

/* The module's ports: each bit by the name of the net on its port pin. */
static odin3_status write_module_ports(blif_writer *wr) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = port_line(wr, def, i);
        odin3_pinslice pins = odin3_node_port(wr->module, odin3_module_port(wr->module, i), 0);
        for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
            wr_token(wr, pin_name(wr, (odin3_pin_id){pins.first.v + k}));
        }
    }
    if (st == ODIN3_OK && def->n_ports > 0) {
        wr_end(wr);
    }
    return st;
}

/* wr->token = base, or base[k] when the port is not scalar. */
static odin3_status bit_token(blif_writer *wr, const odin3_port_def *port, odin3_bytes base,
                              uint32_t bit) {
    odin3_strbuf_clear(&wr->token);
    odin3_status st = odin3_strbuf_append(&wr->token, base);
    if (st == ODIN3_OK && !port->scalar) {
        st = odin3_strbuf_appendf(&wr->token, "[%u]", (unsigned)bit);
    }
    return st;
}

/* A black-box model's ports: bit k of port p is `p` (scalar) or `p[k]`. */
static odin3_status write_model_ports(blif_writer *wr, const odin3_celltype_def *def) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = port_line(wr, def, i);
        const odin3_port_def *port = &def->ports[i];
        for (uint32_t k = 0; st == ODIN3_OK && k < port->width; k++) {
            st = bit_token(wr, port, odin3_bytes_cstr(port->name), k);
            if (st == ODIN3_OK) {
                wr_token_built(wr);
            }
        }
    }
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    if (def->n_ports > 0) {
        wr_end(wr);
    }
    return wr->st;
}

/* `.clock` with the names of the module's ODIN3_BLIF_ATTR_CLOCK attribute. */
static void write_clock(blif_writer *wr) {
    const odin3_value *clock =
        string_attr(wr->module, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(wr->module).v},
                    wr->keys.clock);
    if (clock == NULL) {
        return;
    }
    const char *text = odin3_strtab_get(wr->tab, clock->str);
    wr_begin(wr, ".clock");
    while (*text != '\0') {
        const char *space = strchr(text, ' ');
        size_t len = space != NULL ? (size_t)(space - text) : strlen(text);
        if (len > 0) {
            wr_token(wr, (odin3_bytes){text, len});
        }
        text += len + (space != NULL ? 1 : 0);
    }
    wr_end(wr);
}

/* --- cells --------------------------------------------------------------------------------- */

/* Writes the name of the net on the (first) pin of a 1-bit port of node. */
static void pin_token(blif_writer *wr, odin3_node_id node, uint32_t port) {
    wr_token(wr, pin_name(wr, odin3_node_port(wr->module, node, port).first));
}

/* `.names` with the A pins and Y, then the cover rows as stored. */
static odin3_status write_names(blif_writer *wr, odin3_node_id node) {
    const odin3_value *cover = odin3_node_param(wr->module, node, SOP_COVER);
    uint32_t inputs = cover->cover_inputs;
    wr_begin(wr, ".names");
    odin3_pinslice in = odin3_node_port(wr->module, node, 0);
    for (uint32_t k = 0; k < in.count; k++) {
        wr_token(wr, pin_name(wr, (odin3_pin_id){in.first.v + k}));
    }
    pin_token(wr, node, 1);
    wr_end(wr);
    const char *rows = (const char *)cover->bits;
    for (uint32_t at = 0; at + inputs < cover->len; at += inputs + 1) {
        if (inputs > 0) {
            wr_put(wr, (odin3_bytes){rows + at, inputs});
            wr_put(wr, odin3_bytes_cstr(" "));
        }
        wr_put(wr, (odin3_bytes){rows + at + inputs, 1});
        wr_newline(wr);
    }
    return wr->st;
}

/* `.latch D Q [type C] init`. */
static odin3_status write_latch(blif_writer *wr, odin3_node_id node, wr_builtin which) {
    const char *word = LATCH_WORDS[which];
    wr_begin(wr, ".latch");
    pin_token(wr, node, word != NULL ? TYPED_D : FF_D);
    pin_token(wr, node, word != NULL ? TYPED_Q : FF_Q);
    if (word != NULL) {
        wr_token(wr, odin3_bytes_cstr(word));
        pin_token(wr, node, CTRL_PORT);
    }
    odin3_strbuf_clear(&wr->token);
    odin3_status st = odin3_strbuf_appendf(
        &wr->token, "%lld", (long long)odin3_node_param(wr->module, node, LATCH_INIT)->i);
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    wr_token_built(wr);
    wr_end(wr);
    return wr->st;
}

/* The formal base name of port p of type: a module port's ODIN3_BLIF_ATTR_PORT_NAME, else the
 * port's name. */
static odin3_bytes formal_base(const blif_writer *wr, odin3_celltype_id type,
                               const odin3_port_def *port, uint32_t index) {
    uint64_t id = 0;
    if (wr->keys.port_name != 0 && odin3_u64map_get(wr->type_module, type.v, &id)) {
        const odin3_module *module =
            odin3_module_get((odin3_design *)wr->design, (odin3_module_id){(uint32_t)id});
        odin3_wire_id wire = odin3_module_port_wire(module, index);
        const odin3_value *blif =
            string_attr(module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, wr->keys.port_name);
        if (blif != NULL) {
            return str_bytes(wr, blif->str);
        }
    }
    return odin3_bytes_cstr(port->name);
}

/* True when every parameter of node equals its default (BLIF .subckt cannot give one). */
static bool default_params(const blif_writer *wr, odin3_node_id node,
                           const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (!odin3_value_equal(odin3_node_param(wr->module, node, i), &def->params[i].dflt)) {
            return false;
        }
    }
    return true;
}

/* The connected pins of port `at` of node as `formal=actual` tokens. */
static odin3_status subckt_port(blif_writer *wr, odin3_node_id node, uint32_t at) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    const odin3_port_def *port = &odin3_celltype_get(wr->design, type)->ports[at];
    odin3_bytes base = formal_base(wr, type, port, at);
    odin3_pinslice pins = odin3_node_port(wr->module, node, at);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(wr->module, (odin3_pin_id){pins.first.v + k});
        if (!odin3_net_valid(net)) {
            continue;
        }
        st = bit_token(wr, port, base, k);
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, odin3_bytes_cstr("=")) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, net_name(wr, net)) : st;
        if (st == ODIN3_OK) {
            wr_token_built(wr);
        }
    }
    return st;
}

/* `.subckt type formal=actual …` in port order, unconnected pins skipped. */
static odin3_status write_subckt(blif_writer *wr, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_node_type(wr->module, node));
    if (!default_params(wr, node, def)) {
        return wr_refuse(wr, "non-default parameters have no BLIF form; cannot write a cell of",
                         def->name);
    }
    wr_begin(wr, ".subckt");
    wr_token(wr, odin3_bytes_cstr(def->name));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = subckt_port(wr, node, i);
    }
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    wr_end(wr);
    return wr->st;
}

/* `.attr key value` / `.param key value` for one full key of the cell's extras list. */
static void write_extra(blif_writer *wr, odin3_node_id node, odin3_bytes key) {
    uint32_t str = 0;
    size_t attr_len = strlen(ODIN3_BLIF_ATTR_PREFIX);
    size_t param_len = strlen(ODIN3_BLIF_PARAM_PREFIX);
    bool attr = key.len > attr_len && memcmp(key.ptr, ODIN3_BLIF_ATTR_PREFIX, attr_len) == 0;
    bool param = key.len > param_len && memcmp(key.ptr, ODIN3_BLIF_PARAM_PREFIX, param_len) == 0;
    if ((!attr && !param) || !odin3_strtab_find(wr->tab, key, &str)) {
        return;
    }
    const odin3_value *value = string_attr(wr->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, str);
    if (value == NULL) {
        return;
    }
    size_t skip = attr ? attr_len : param_len;
    wr_begin(wr, attr ? ".attr" : ".param");
    wr_token(wr, (odin3_bytes){(const char *)key.ptr + skip, key.len - skip});
    wr_token(wr, str_bytes(wr, value->str));
    wr_end(wr);
}

/* `.cname` with the node's name, then its `.attr`/`.param` lines in recorded order. */
static void write_cell_extras(blif_writer *wr, odin3_node_id node) {
    uint32_t name = odin3_node_name(wr->module, node);
    if (name != 0) {
        wr_begin(wr, ".cname");
        wr_token(wr, str_bytes(wr, name));
        wr_end(wr);
    }
    const odin3_value *extras =
        string_attr(wr->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, wr->keys.extras);
    if (extras == NULL) {
        return;
    }
    const char *text = odin3_strtab_get(wr->tab, extras->str);
    while (*text != '\0') {
        const char *space = strchr(text, ' ');
        size_t len = space != NULL ? (size_t)(space - text) : strlen(text);
        write_extra(wr, node, (odin3_bytes){text, len});
        text += len + (space != NULL ? 1 : 0);
    }
}

static odin3_status write_cell(blif_writer *wr, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    if (odin3_celltype_get(wr->design, type)->gran == ODIN3_GRAN_PORT) {
        return ODIN3_OK;
    }
    wr_builtin which = builtin_of(wr, type);
    odin3_status st = ODIN3_OK;
    if (which == WR_SOP) {
        st = write_names(wr, node);
    } else if (which < WR_BUILTINS) {
        st = write_latch(wr, node, which);
    } else {
        st = write_subckt(wr, node);
    }
    if (st == ODIN3_OK) {
        write_cell_extras(wr, node);
    }
    return wr->st;
}

/* --- models -------------------------------------------------------------------------------- */

static void end_module(blif_writer *wr) {
    odin3_strtab_destroy(wr->gen_names);
    odin3_u64map_destroy(wr->gen);
    odin3_u64map_destroy(wr->port_of);
    wr->gen_names = NULL;
    wr->gen = NULL;
    wr->port_of = NULL;
    wr->module = NULL;
}

static odin3_status write_module(blif_writer *wr, uint32_t id) {
    wr->module = odin3_module_get((odin3_design *)wr->design, (odin3_module_id){id});
    wr->model = odin3_strtab_get(wr->tab, odin3_module_name(wr->module));
    odin3_status st = wr_fail(wr, name_module(wr));
    if (id > 1) {
        wr_newline(wr);
    }
    wr_begin(wr, ".model");
    wr_token(wr, str_bytes(wr, odin3_module_name(wr->module)));
    wr_end(wr);
    st = st == ODIN3_OK ? write_module_ports(wr) : st;
    if (st == ODIN3_OK) {
        write_clock(wr);
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(wr->module); i++) {
        if (odin3_node_live(wr->module, (odin3_node_id){i})) {
            st = write_cell(wr, (odin3_node_id){i});
        }
    }
    wr_begin(wr, ".end");
    wr_end(wr);
    end_module(wr);
    return wr->st;
}

/* Each declared black-box model once, in declaration order. */
static odin3_status write_blackboxes(blif_writer *wr) {
    uint32_t count = odin3_design_declared_model_count(wr->design);
    for (uint32_t i = 0; wr->st == ODIN3_OK && i < count; i++) {
        odin3_celltype_id type = odin3_design_declared_model(wr->design, i);
        uint64_t seen = 0;
        if (odin3_u64map_get(wr->written, type.v, &seen)) {
            continue;
        }
        if (odin3_u64map_put(wr->written, (odin3_kv){type.v, 1}) != ODIN3_OK) {
            return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
        const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
        wr->model = def->name;
        wr_newline(wr);
        wr_begin(wr, ".model");
        wr_token(wr, odin3_bytes_cstr(def->name));
        wr_end(wr);
        if (write_model_ports(wr, def) == ODIN3_OK) {
            wr_begin(wr, ".blackbox");
            wr_end(wr);
            wr_begin(wr, ".end");
            wr_end(wr);
        }
    }
    return wr->st;
}

/* --- entry point --------------------------------------------------------------------------- */

static uint32_t find_key(const blif_writer *wr, const char *key) {
    uint32_t str = 0;
    return odin3_strtab_find(wr->tab, odin3_bytes_cstr(key), &str) ? str : 0;
}

static odin3_status wr_init(blif_writer *wr, const odin3_design *design, const char *path) {
    *wr = (blif_writer){.design = design, .tab = odin3_design_strtab(design), .path = path};
    odin3_strbuf_init(&wr->token);
    odin3_strbuf_init(&wr->cand);
    wr->keys = (wr_keys){.clock = find_key(wr, ODIN3_BLIF_ATTR_CLOCK),
                         .port_name = find_key(wr, ODIN3_BLIF_ATTR_PORT_NAME),
                         .extras = find_key(wr, ODIN3_BLIF_ATTR_EXTRAS)};
    for (uint32_t i = 0; i < WR_BUILTINS; i++) {
        uint32_t name = find_key(wr, BUILTIN_NAMES[i]);
        if (name == 0 || !odin3_celltype_find(design, name, &wr->builtin[i])) {
            wr->builtin[i] = (odin3_celltype_id){0};
        }
    }
    wr->type_module = odin3_u64map_create(0);
    wr->written = odin3_u64map_create(0);
    if (wr->type_module == NULL || wr->written == NULL) {
        return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        const odin3_module *module = odin3_module_get((odin3_design *)design, (odin3_module_id){i});
        odin3_kv entry = {odin3_module_celltype(module).v, i};
        if (odin3_u64map_put(wr->type_module, entry) != ODIN3_OK) {
            return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
    }
    return ODIN3_OK;
}

static odin3_status wr_open(blif_writer *wr) {
    wr->buffer = odin3_util_malloc(IO_BUFFER);
    if (wr->buffer == NULL) {
        return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    wr->file = fopen(wr->path, "wb");
    if (wr->file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot open for writing: %s", wr->path, strerror(errno));
        return wr_fail(wr, ODIN3_ERR_IO);
    }
    if (setvbuf(wr->file, wr->buffer, _IOFBF, IO_BUFFER) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot set the output buffer", wr->path);
        return wr_fail(wr, ODIN3_ERR_IO);
    }
    return ODIN3_OK;
}

/* Closes the file; a failure to flush is a located write error. */
static void wr_close(blif_writer *wr) {
    if (wr->file == NULL) {
        return;
    }
    bool failed = fflush(wr->file) != 0 || ferror(wr->file) != 0;
    int saved = errno;
    failed = fclose(wr->file) != 0 || failed;
    wr->file = NULL;
    if (failed && wr->st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: write failed: %s", wr->path, (unsigned)wr->line,
                  strerror(saved != 0 ? saved : errno));
        (void)wr_fail(wr, ODIN3_ERR_IO);
    }
}

static void wr_free(blif_writer *wr) {
    end_module(wr);
    odin3_util_free(wr->buffer);
    odin3_strbuf_free(&wr->token);
    odin3_strbuf_free(&wr->cand);
    odin3_u64map_destroy(wr->type_module);
    odin3_u64map_destroy(wr->written);
}

odin3_status odin3_blif_write(const odin3_design *design, const char *path) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "blif_write: NULL design or path");
        return ODIN3_ERR_INVALID_ARG;
    }
    blif_writer wr;
    odin3_status st = wr_init(&wr, design, path);
    if (st == ODIN3_OK) {
        st = wr_open(&wr);
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        st = write_module(&wr, i);
    }
    if (st == ODIN3_OK) {
        (void)write_blackboxes(&wr);
    }
    wr_close(&wr); /* the status is sticky: wr.st holds the first failure, closing included */
    st = wr.st;
    wr_free(&wr);
    return st;
}
