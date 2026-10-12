/* writer.c — BLIF writer: modules, cells and declared black boxes of a design. */
#include "backends/blif/writer.h"

#include "backends/blif/vtr.h"
#include "frontends/blif/attrs.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "techlib/reader.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * The word flip-flops and latches of 2E's IR change D (2E spec §13): BLIF's `.latch` cannot carry
 * their resets, enables and polarities, so the netlist-view writer refuses them (`lower` maps them
 * to bit cells first). wr_init resolves the names to the design's type IDs once, and write_cell
 * compares IDs: exact, since no other type can be registered under a built-in's name.
 */
enum { WR_NO_BLIF = 11 };
static const char *const NO_BLIF_FORM[WR_NO_BLIF] = {"$adffe",   "$sdffe",    "$sdffce", "$aldff",
                                                     "$aldffe",  "$dffsr",    "$dffsre", "$dlatch",
                                                     "$adlatch", "$dlatchsr", "$sr"};

/* The .latch type word of each built-in; NULL for $sop and for $_FF_ (no type, no control). */
static const char *const LATCH_WORDS[WR_BUILTINS] = {NULL, "re", "fe", "ah", "al", NULL};

/* $sop parameters, and ports of the .latch types (with and without a control pin). */
enum { SOP_WIDTH, SOP_COVER };
enum { LATCH_INIT = 0, CTRL_PORT = 0, TYPED_D = 1, TYPED_Q = 2, FF_D = 0, FF_Q = 1 };

/* Strtab IDs of the attribute keys the reader sets; 0 when never interned (no object has it). */
typedef struct wr_keys {
    uint32_t clock, port_name, extras;
} wr_keys;

/* Bit `bit` of port `port` (index in port order). */
typedef struct wr_portbit {
    uint32_t port, bit;
} wr_portbit;

typedef struct blif_writer {
    const odin3_design *design;
    const odin3_strtab *tab;
    const char *path;
    const odin3_blif_write_opts *opts; /* checked: the dialect, its K and name style */
    bool vtr;                          /* opts->dialect is ODIN3_BLIF_DIALECT_VTR */
    odin3_atomic_file file; /* a private temporary beside path, renamed over it on success */
    char *buffer;           /* the stdio buffer of file.fp */
    uint32_t line;          /* complete lines written so far */
    size_t col;             /* columns on the line being written */
    bool line_empty;        /* nothing but the indent on the line being written */
    bool oom_logged;
    bool opened;       /* the temporary is open: wr_close commits or removes it */
    odin3_status st;   /* sticky: the first failure */
    const char *model; /* name of the model being written, for messages */
    odin3_celltype_id builtin[WR_BUILTINS];
    odin3_celltype_id no_blif[WR_NO_BLIF]; /* NO_BLIF_FORM's IDs; 0 when not registered */
    wr_keys keys;
    odin3_u64map *type_module; /* module cell type -> module ID */
    odin3_u64map *declared;    /* declared cell type -> its first declared-model index */
    odin3_strbuf token;        /* the token being built */
    odin3_strbuf cand;         /* a candidate generated name */
    /* the VTR dialect's names (spec 3E VB-10) */
    odin3_blif_names
        *names;         /* VTR: the top's names from the preflight's plan, lent to the writer */
    bool quiet_refusal; /* the preflight's cap is reached: a refusal is counted, not logged */
    bool log_names;     /* log the port warnings (the preflight's naming run) */
    odin3_strbuf orig;  /* the last name legalise rewrote, as it was */
    odin3_strbuf legal; /* scratch for legalise */
    uint32_t renamed;   /* written names VB-10 changed (note_renamed) */
    bool have_first;    /* first_key, first_orig, first_legal are set */
    uint32_t first_key; /* the lowest net ID of such a name */
    odin3_strbuf first_orig, first_legal; /* that name before and after */
    /* the module being written */
    const odin3_module *module;
    odin3_strtab *gen_names; /* its port bit names, then its generated names */
    odin3_u64map *claims;    /* gen_names ID of a port bit name -> the net on that port bit */
    odin3_u64map *net_port;  /* net ID -> gen_names ID of the first port bit name on it */
    odin3_u64map
        *gen; /* net ID or 1 << 32 | pin ID -> gen_names ID it is written under (or NULL) */
    odin3_u64map *port_of; /* port wire -> port index (NULL until needed) */
    odin3_u64map *clocks;  /* VTR: nets on a .latch control or a hard-cell clock input -> 1 */
    /* VTR emission (spec 3E §5, §9.3) */
    bool scan;          /* a dry run of the cells and buffers: nothing is written */
    uint32_t gens_used; /* generators a pin or buffer reads (1 << wr_gen) */
    uint32_t gens_have; /* generators a written zero-input $sop already is (1 << wr_gen) */
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

/* Logs "path: model '<model>': <what> '<name>'" (unless quiet_refusal) and fails with
 * ODIN3_ERR_INVALID_ARG. */
static odin3_status wr_refuse(blif_writer *wr, const char *what, const char *name) {
    if (!wr->quiet_refusal) {
        odin3_log(ODIN3_LOG_ERROR, "%s: model '%s': %s '%s'", wr->path, wr->model, what, name);
    }
    return wr_fail(wr, ODIN3_ERR_INVALID_ARG);
}

static void wr_put(blif_writer *wr, odin3_bytes data) {
    if (wr->scan || wr->st != ODIN3_OK || data.len == 0) {
        return;
    }
    errno = 0;
    if (fwrite(data.ptr, 1, data.len, wr->file.fp) != data.len) {
        int saved = errno != 0 ? errno : EIO;
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: write failed: %s", wr->path, (unsigned)wr->line + 1,
                  strerror(saved));
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

/* True when no net of the module but self has the name, and it is neither a port bit name nor
 * an earlier generated name. */
static bool name_free(const blif_writer *wr, odin3_bytes text, odin3_net_id self) {
    uint32_t str = 0;
    if (odin3_strtab_find(wr->tab, text, &str)) {
        odin3_net_id have = odin3_module_find_net(wr->module, str);
        if (odin3_net_valid(have) && have.v != self.v) {
            return false;
        }
    }
    return !odin3_strtab_find(wr->gen_names, text, NULL);
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

/* Records gen_names ID id as the name key is written under. */
static odin3_status set_gen(blif_writer *wr, uint64_t key, uint32_t id) {
    if (wr->gen == NULL) {
        wr->gen = odin3_u64map_create(0);
        if (wr->gen == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return odin3_u64map_put(wr->gen, (odin3_kv){key, id});
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

/* --- VB-10 legal names (the VTR dialect) ------------------------------------------------------ */

static bool same_bytes(odin3_bytes have, odin3_bytes want) {
    return have.len == want.len && (have.len == 0 || memcmp(have.ptr, want.ptr, have.len) == 0);
}

static odin3_bytes buf_bytes(const odin3_strbuf *buf) {
    return (odin3_bytes){buf->data, buf->len};
}

/* True when net is used as a .latch control or a hard cell's clock input (VB-10 rule 8). */
static bool is_clock(const blif_writer *wr, odin3_net_id net) {
    uint64_t one = 0;
    return wr->clocks != NULL && odin3_net_valid(net) && odin3_u64map_get(wr->clocks, net.v, &one);
}

/*
 * VTR: rewrites buf, a name of net (none for an unconnected port pin), into its VB-10 form: rules
 * 1-3 (odin3_blif_vtr_legal_name), then rule 8 when net is a clock. wr->orig keeps the name as it
 * was; *changed when the two differ.
 */
static odin3_status legalise(blif_writer *wr, odin3_strbuf *buf, odin3_net_id net, bool *changed) {
    *changed = false;
    odin3_strbuf_clear(&wr->orig);
    odin3_strbuf_clear(&wr->legal);
    odin3_status st = odin3_strbuf_append(&wr->orig, buf_bytes(buf));
    st = st == ODIN3_OK ? odin3_blif_vtr_legal_name(buf_bytes(&wr->orig), &wr->legal) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_strbuf_clear(buf);
    st = is_clock(wr, net) ? odin3_blif_vtr_clock_name(buf_bytes(&wr->legal), buf)
                           : odin3_strbuf_append(buf, buf_bytes(&wr->legal));
    *changed = st == ODIN3_OK && !same_bytes(buf_bytes(buf), buf_bytes(&wr->orig));
    return st;
}

/*
 * Counts one written name VB-10 changed (wr->orig before, written after): a port bit name, or a
 * net's own or wire bit name. key is the net ID it names (UINT32_MAX for an unconnected port
 * bit); the pair of the lowest key, the earliest noted on a tie, is kept for the info line.
 */
static odin3_status note_renamed(blif_writer *wr, uint32_t key, odin3_bytes written) {
    wr->renamed++;
    if (wr->have_first && wr->first_key <= key) {
        return ODIN3_OK;
    }
    wr->have_first = true;
    wr->first_key = key;
    odin3_strbuf_clear(&wr->first_orig);
    odin3_strbuf_clear(&wr->first_legal);
    odin3_status st = odin3_strbuf_append(&wr->first_orig, buf_bytes(&wr->orig));
    return st == ODIN3_OK ? odin3_strbuf_append(&wr->first_legal, written) : st;
}

/* VB-9 §9.3, rule 3's exception: a net named `gnd` or `unconn` (`vcc`) that a zero-input $sop of
 * value 0 (1) drives is that generator and keeps its name. */
static bool is_generator(const blif_writer *wr, odin3_net_id net, odin3_bytes name) {
    bool vcc = same_bytes(name, odin3_bytes_cstr("vcc"));
    if (!vcc && !same_bytes(name, odin3_bytes_cstr("gnd")) &&
        !same_bytes(name, odin3_bytes_cstr("unconn"))) {
        return false;
    }
    odin3_pin_id driver = odin3_net_driver(wr->module, net);
    return odin3_pin_valid(driver) && wr->builtin[WR_SOP].v != 0 &&
           odin3_node_type(wr->module, odin3_pin_node(wr->module, driver)).v ==
               wr->builtin[WR_SOP].v &&
           odin3_net_const_value(wr->module, net) == (vcc ? ODIN3_CONST_1 : ODIN3_CONST_0);
}

/* Marks the nets of port `port` of node as clocks. */
static odin3_status mark_clocks(blif_writer *wr, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(wr->module, node, port);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_net_id net = odin3_pin_net(wr->module, (odin3_pin_id){pins.first.v + k});
        if (odin3_net_valid(net)) {
            st = odin3_u64map_put(wr->clocks, (odin3_kv){net.v, 1});
        }
    }
    return st;
}

/* Marks the clock nets of node: a typed .latch's control, a hard cell's `clock` inputs (1G). */
static odin3_status node_clocks(blif_writer *wr, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    wr_builtin which = builtin_of(wr, type);
    if (which < WR_BUILTINS) {
        return LATCH_WORDS[which] != NULL ? mark_clocks(wr, node, CTRL_PORT) : ODIN3_OK;
    }
    const odin3_techlib_cell *lib = odin3_celltype_lib(wr->design, type);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0;
         lib != NULL && lib->kind == ODIN3_TECHLIB_HARD && st == ODIN3_OK && i < lib->n_ports;
         i++) {
        st = lib->ports[i].clock ? mark_clocks(wr, node, i) : ODIN3_OK;
    }
    return st;
}

/* VTR: wr->clocks = the nets VB-10 rule 8 applies to. */
static odin3_status find_clocks(blif_writer *wr) {
    wr->clocks = odin3_u64map_create(0);
    if (wr->clocks == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(wr->module); i++) {
        if (odin3_node_live(wr->module, (odin3_node_id){i})) {
            st = node_clocks(wr, (odin3_node_id){i});
        }
    }
    return st;
}

/* The generated name of key; it exists for every pin name_pins covered. */
static odin3_bytes gen_get(const blif_writer *wr, uint64_t key) {
    uint64_t id = 0;
    if (wr->gen == NULL || !odin3_u64map_get(wr->gen, key, &id)) {
        return (odin3_bytes){"", 0};
    }
    return (odin3_bytes){odin3_strtab_get(wr->gen_names, (uint32_t)id),
                         odin3_strtab_len(wr->gen_names, (uint32_t)id)};
}

/* Interns wr->cand as the generated name of key. */
static odin3_status take_name(blif_writer *wr, uint64_t key) {
    uint32_t id = 0;
    odin3_status st =
        odin3_strtab_intern(wr->gen_names, (odin3_bytes){wr->cand.data, wr->cand.len}, &id);
    return st == ODIN3_OK ? set_gen(wr, key, id) : st;
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
 * name (in the VTR dialect its VB-10 form) when free, else take_numbered. */
static odin3_status generate(blif_writer *wr, uint64_t key) {
    bool pin = (key >> PIN_KEY_SHIFT) != 0;
    odin3_net_id self = {pin ? 0 : (uint32_t)key};
    bool found = false;
    bool changed = false;
    odin3_status st = pin ? ODIN3_OK : wire_bit_name(wr, self, &found);
    if (st == ODIN3_OK && found && wr->vtr) {
        st = legalise(wr, &wr->cand, self, &changed);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    st = found && name_free(wr, (odin3_bytes){wr->cand.data, wr->cand.len}, self)
             ? take_name(wr, key)
             : take_numbered(wr, key);
    return st == ODIN3_OK && changed ? note_renamed(wr, self.v, gen_get(wr, key)) : st;
}

/* The name net is written under: its generated or port bit name when it has one, else its own. */
static odin3_bytes net_name(const blif_writer *wr, odin3_net_id net) {
    uint64_t id = 0;
    if (wr->gen != NULL && odin3_u64map_get(wr->gen, net.v, &id)) {
        return (odin3_bytes){odin3_strtab_get(wr->gen_names, (uint32_t)id),
                             odin3_strtab_len(wr->gen_names, (uint32_t)id)};
    }
    return str_bytes(wr, odin3_net_name(wr->module, net));
}

static odin3_bytes pin_name(const blif_writer *wr, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(wr->module, pin);
    return odin3_net_valid(net) ? net_name(wr, net)
                                : gen_get(wr, ((uint64_t)1 << PIN_KEY_SHIFT) | pin.v);
}

/* True for a node whose pins are written by net name even when unconnected (.names, .latch and
 * port nodes; in the VTR dialect the gates too, written as .names); .subckt skips unconnected
 * pins. */
static bool names_every_pin(const blif_writer *wr, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    odin3_blif_vtr_rows rows;
    return builtin_of(wr, type) != WR_BUILTINS ||
           odin3_celltype_get(wr->design, type)->gran == ODIN3_GRAN_PORT ||
           (wr->vtr && odin3_blif_vtr_rows_of(wr->module, node, &rows));
}

/* True for a hard cell of a loaded tech library (1G): in the VTR dialect, a .subckt that writes
 * every formal (spec 3E §8). */
static bool hard_type(const odin3_design *design, odin3_celltype_id type) {
    const odin3_techlib_cell *lib = odin3_celltype_lib(design, type);
    return lib != NULL && lib->kind == ODIN3_TECHLIB_HARD;
}

/* Generates names for the unconnected pins of a node written by pin (see names_every_pin), or
 * (outputs) only for its unconnected outputs: a VTR .subckt writes every output bit (§8.2), while
 * its unconnected inputs read `unconn`. */
static odin3_status name_pins(blif_writer *wr, odin3_node_id node, bool outputs) {
    odin3_pinslice pins = odin3_node_pins(wr->module, node);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (!odin3_net_valid(odin3_pin_net(wr->module, pin)) &&
            (!outputs || odin3_pin_dir(wr->module, pin) == ODIN3_DIR_OUT)) {
            st = generate(wr, ((uint64_t)1 << PIN_KEY_SHIFT) | pin.v);
        }
    }
    return st;
}

/* wr->token = base, or base[index] when the port is not scalar (index: the declared one). */
static odin3_status bit_token(blif_writer *wr, bool scalar, odin3_bytes base, int64_t index) {
    odin3_strbuf_clear(&wr->token);
    odin3_status st = odin3_strbuf_append(&wr->token, base);
    if (st == ODIN3_OK && !scalar) {
        st = odin3_strbuf_appendf(&wr->token, "[%lld]", (long long)index);
    }
    return st;
}

/* The BLIF base name of port `at` of module: its wire's ODIN3_BLIF_ATTR_PORT_NAME, else its name.
 */
static odin3_bytes port_base(const blif_writer *wr, const odin3_module *module, uint32_t at) {
    odin3_wire_id wire = odin3_module_port_wire(module, at);
    const odin3_value *blif =
        string_attr(module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, wr->keys.port_name);
    if (blif != NULL) {
        return str_bytes(wr, blif->str);
    }
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, odin3_module_celltype(module));
    return odin3_bytes_cstr(def->ports[at].name);
}

/*
 * The declared index of bit `bit` (position, LSB first) of port `at` of module: its port wire's
 * (IR-7), as Yosys names port bits, so `input [0:7] a` lists a[7] … a[0].
 */
static int64_t port_index_of(const odin3_module *module, wr_portbit where) {
    return odin3_wire_index(module, odin3_module_port_wire(module, where.port), where.bit);
}

/* The net on a port bit of the module being written; none when the port pin is unconnected. */
static odin3_net_id port_bit_net(const blif_writer *wr, wr_portbit where) {
    odin3_pinslice pins = odin3_node_port(wr->module, odin3_module_port(wr->module, where.port), 0);
    return odin3_pin_net(wr->module, (odin3_pin_id){pins.first.v + where.bit});
}

/* wr->token = the BLIF name of a port bit of the module being written: `base`, or `base[i]` with
 * the bit's declared index i (as a non-port wire bit, wire_bit_name; VB-10 rule 5 in both
 * dialects). In the VTR dialect, its VB-10 form (legalise: wr->orig keeps the name before, and
 * *changed says whether they differ). */
static odin3_status port_bit_token(blif_writer *wr, wr_portbit where, bool *changed) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
    *changed = false;
    odin3_status st =
        bit_token(wr, def->ports[where.port].scalar, port_base(wr, wr->module, where.port),
                  port_index_of(wr->module, where));
    if (st == ODIN3_OK && wr->vtr) {
        st = legalise(wr, &wr->token, port_bit_net(wr, where), changed);
    }
    return st;
}

/*
 * The warning for a port whose bit name VB-10 changed (wr->token now, wr->orig before, wr->legal
 * its rules 1-3 form): `<p>` is the port's base name and `<legal>` its written form (the token
 * without its `[i]`). VPR cannot read the name when rules 1-3 changed it; when only rule 8 did
 * (a clock port), VPR can, and the text names VTR's latch clock domains instead.
 */
static void port_warning(const blif_writer *wr, uint32_t port) {
    odin3_bytes base = port_base(wr, wr->module, port);
    int base_len = (int)base.len;
    const char *base_text = base.ptr;
    int legal_len = (int)(wr->token.len - (wr->orig.len - base.len)); /* both end in one `[i]` */
    if (!same_bytes(buf_bytes(&wr->legal), buf_bytes(&wr->orig))) {
        odin3_log(ODIN3_LOG_WARN,
                  ODIN3_BLIF_VTR_PREFIX "port '%.*s' written as '%.*s' (VPR cannot read '%.*s')",
                  base_len, base_text, legal_len, wr->token.data, base_len, base_text);
        return;
    }
    odin3_log(ODIN3_LOG_WARN,
              ODIN3_BLIF_VTR_PREFIX "clock port '%.*s' written as '%.*s' (VTR's latch clock "
                                    "domains cannot hold '|' or '_^_')",
              base_len, base_text, legal_len, wr->token.data);
}

/*
 * A port bit whose name VB-10 changed: the warning, once per port (*warned) and only on the
 * logging run; and the written port bit name counts as renamed, under its net's ID (the net, if
 * it takes the same name, is not counted again: name_named_vtr).
 */
static odin3_status port_renamed(blif_writer *wr, uint32_t port, odin3_net_id net, bool *warned) {
    if (wr->log_names && !*warned) {
        port_warning(wr, port);
        *warned = true;
    }
    return note_renamed(wr, odin3_net_valid(net) ? net.v : UINT32_MAX, buf_bytes(&wr->token));
}

/* Reserves the port bit name in wr->token for net (none for an unconnected port pin); the first
 * port bit on a net is the name an unnamed net takes. Two ports of one name on different nets
 * cannot be written. */
static odin3_status claim(blif_writer *wr, odin3_net_id net) {
    uint32_t id = 0;
    odin3_status st =
        odin3_strtab_intern(wr->gen_names, (odin3_bytes){wr->token.data, wr->token.len}, &id);
    uint64_t owner = 0;
    if (st != ODIN3_OK) {
        return st;
    }
    if (odin3_u64map_get(wr->claims, id, &owner)) {
        return owner == net.v ? ODIN3_OK
                              : wr_refuse(wr, "two ports on different nets have the BLIF name",
                                          wr->token.data);
    }
    st = odin3_u64map_put(wr->claims, (odin3_kv){id, net.v});
    if (st == ODIN3_OK && odin3_net_valid(net) && !odin3_u64map_get(wr->net_port, net.v, &owner)) {
        st = odin3_u64map_put(wr->net_port, (odin3_kv){net.v, id});
    }
    return st;
}

/* Claims every port bit name of the module, in port order. */
static odin3_status claim_ports(blif_writer *wr) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_module_port_count(wr->module); i++) {
        odin3_pinslice pins = odin3_node_port(wr->module, odin3_module_port(wr->module, i), 0);
        bool warned = false;
        for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
            odin3_net_id net = odin3_pin_net(wr->module, (odin3_pin_id){pins.first.v + k});
            bool changed = false;
            st = port_bit_token(wr, (wr_portbit){i, k}, &changed);
            if (st == ODIN3_OK && changed) {
                st = port_renamed(wr, i, net, &warned);
            }
            if (st == ODIN3_OK) {
                st = claim(wr, net);
            }
        }
    }
    return st;
}

/*
 * VTR: a named net is written under its own name's VB-10 form (a generator's own name as it is,
 * is_generator), unless a port bit of another net or an earlier net already has that form (VB-10
 * rule 4, legalisation before uniqueness): then `$n<ID>`….
 */
static odin3_status name_named_vtr(blif_writer *wr, odin3_net_id net, uint32_t real) {
    odin3_bytes own = str_bytes(wr, real);
    bool changed = false;
    odin3_strbuf_clear(&wr->cand);
    odin3_status st = odin3_strbuf_append(&wr->cand, own);
    if (st == ODIN3_OK && !is_generator(wr, net, own)) {
        st = legalise(wr, &wr->cand, net, &changed);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t id = 0;
    uint64_t owner = 0;
    bool found = odin3_strtab_find(wr->gen_names, buf_bytes(&wr->cand), &id);
    bool own_port = found && odin3_u64map_get(wr->claims, id, &owner) && owner == net.v;
    st = found && !own_port ? take_numbered(wr, net.v) : take_name(wr, net.v);
    /* a net under its own port bit's name was counted with the port bit (port_renamed) */
    return st == ODIN3_OK && changed && !own_port ? note_renamed(wr, net.v, gen_get(wr, net.v))
                                                  : st;
}

/* Chooses the name net is written under: its own name unless a port bit of another net has it
 * (then `$n<ID>`…); for an unnamed net the first port bit name on it, else a generated name. In
 * the VTR dialect, each in its VB-10 form (name_named_vtr, claim_ports, generate). */
static odin3_status name_net(blif_writer *wr, odin3_net_id net) {
    uint32_t real = odin3_net_name(wr->module, net);
    uint32_t id = 0;
    uint64_t owner = 0;
    if (real != 0 && wr->vtr) {
        return name_named_vtr(wr, net, real);
    }
    if (real != 0) {
        bool reserved = odin3_strtab_find(wr->gen_names, str_bytes(wr, real), &id) &&
                        odin3_u64map_get(wr->claims, id, &owner);
        return reserved && owner != net.v ? take_numbered(wr, net.v) : ODIN3_OK;
    }
    if (odin3_u64map_get(wr->net_port, net.v, &owner)) {
        return set_gen(wr, net.v, (uint32_t)owner);
    }
    return generate(wr, net.v);
}

/* The unconnected pins name_module names for node: all of them for a node written by pin
 * (names_every_pin), the outputs of a VTR hard cell. */
static odin3_status name_node_pins(blif_writer *wr, odin3_node_id node) {
    if (names_every_pin(wr, node)) {
        return name_pins(wr, node, false);
    }
    if (wr->vtr && hard_type(wr->design, odin3_node_type(wr->module, node))) {
        return name_pins(wr, node, true);
    }
    return ODIN3_OK;
}

/* Names the module: (VTR: its clock nets first, for VB-10 rule 8) port bit names, then every live
 * net (ID order), then the unconnected pins of .names, .latch and port cells (VTR: and the
 * unconnected outputs of hard cells). */
static odin3_status name_module(blif_writer *wr) {
    const odin3_module *module = wr->module;
    wr->gen_names = odin3_strtab_create();
    wr->claims = odin3_u64map_create(0);
    wr->net_port = odin3_u64map_create(0);
    if (wr->gen_names == NULL || wr->claims == NULL || wr->net_port == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = wr->vtr ? find_clocks(wr) : ODIN3_OK;
    st = st == ODIN3_OK ? claim_ports(wr) : st;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_net_end(module); i++) {
        if (odin3_net_live(module, (odin3_net_id){i})) {
            st = name_net(wr, (odin3_net_id){i});
        }
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(module); i++) {
        if (odin3_node_live(module, (odin3_node_id){i})) {
            st = name_node_pins(wr, (odin3_node_id){i});
        }
    }
    return st;
}

/* --- VTR: constants, x and `unconn` (spec 3E §9) -------------------------------------------- */

/* The generators (§9.3), in the order §5 item 4 writes them. */
typedef enum wr_gen { GEN_GND, GEN_VCC, GEN_UNCONN, GEN_COUNT, GEN_NONE = GEN_COUNT } wr_gen;

static const char *const GEN_NAMES[GEN_COUNT] = {"gnd", "vcc", "unconn"};

/* The generator a pin reads instead of net (§9.2; `hard` for a .subckt input, which reads x as
 * `unconn`, open in VPR, where logic reads it as `gnd`), GEN_NONE when it reads net by its name.
 * An unconnected pin is UNDRIVEN. */
static wr_gen source_gen(const blif_writer *wr, odin3_net_id net, bool hard) {
    switch (odin3_blif_vtr_net_class(wr->module, net)) {
    case ODIN3_BLIF_SRC_0:
        return GEN_GND;
    case ODIN3_BLIF_SRC_1:
        return GEN_VCC;
    case ODIN3_BLIF_SRC_X:
    case ODIN3_BLIF_SRC_UNDRIVEN:
        return hard ? GEN_UNCONN : GEN_GND;
    case ODIN3_BLIF_SRC_U:
        return GEN_UNCONN;
    default:
        return GEN_NONE;
    }
}

/* The name of gen, which counts as used. */
static odin3_bytes gen_use(blif_writer *wr, wr_gen gen) {
    wr->gens_used |= 1U << gen;
    return odin3_bytes_cstr(GEN_NAMES[gen]);
}

/* The name an input pin reads its source under (§9.2): its generator, else its net's name. */
static odin3_bytes source_name(blif_writer *wr, odin3_pin_id pin, bool hard) {
    wr_gen gen = source_gen(wr, odin3_pin_net(wr->module, pin), hard);
    return gen == GEN_NONE ? pin_name(wr, pin) : gen_use(wr, gen);
}

/* A zero-input $sop written under a generator's name is that generator (§9.3: VB-10 keeps the
 * name only for the generator of that value, is_generator). */
static void note_generator(blif_writer *wr, odin3_bytes name) {
    for (uint32_t gen = 0; gen < GEN_COUNT; gen++) {
        if (same_bytes(name, odin3_bytes_cstr(GEN_NAMES[gen]))) {
            wr->gens_have |= 1U << gen;
        }
    }
}

/* The constant (0 or 1) of the $sop or gate driving net when §6 writes it as a generator (a
 * zero-input $sop, or a cell whose function is constant), else -1; -1 for class U too, whose
 * output-port bits are buffered from `unconn` as read. */
static int const_driver(const blif_writer *wr, odin3_net_id net) {
    odin3_pin_id driver =
        odin3_net_valid(net) ? odin3_net_driver(wr->module, net) : (odin3_pin_id){0};
    if (!odin3_pin_valid(driver) || odin3_blif_vtr_net_class(wr->module, net) == ODIN3_BLIF_SRC_U) {
        return -1;
    }
    odin3_blif_vtr_fn fn = odin3_blif_vtr_function(wr->module, odin3_pin_node(wr->module, driver));
    if (fn == ODIN3_BLIF_VTR_FN_CONST0 || fn == ODIN3_BLIF_VTR_FN_CONST1) {
        return fn == ODIN3_BLIF_VTR_FN_CONST1 ? 1 : 0;
    }
    return -1;
}

/* `.names <name>`, with the row `1` for value 1: a zero-input generator. */
static void write_constant(blif_writer *wr, odin3_bytes name, int value) {
    wr_begin(wr, ".names");
    wr_token(wr, name);
    wr_end(wr);
    if (value == 1) {
        wr_put(wr, odin3_bytes_cstr("1"));
        wr_newline(wr);
    }
}

/* §5 item 4: each generator a pin or buffer reads that no written zero-input $sop already is, in
 * the order gnd, vcc, unconn. */
static void write_generators(blif_writer *wr) {
    for (uint32_t gen = 0; gen < GEN_COUNT; gen++) {
        if (((wr->gens_used & ~wr->gens_have) >> gen & 1U) != 0) {
            write_constant(wr, odin3_bytes_cstr(GEN_NAMES[gen]), gen == GEN_VCC ? 1 : 0);
        }
    }
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

/* The module's ports, each bit by its port bit name. */
static odin3_status write_module_ports(blif_writer *wr) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = port_line(wr, def, i);
        uint32_t width = odin3_node_port(wr->module, odin3_module_port(wr->module, i), 0).count;
        for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
            bool changed = false;
            st = port_bit_token(wr, (wr_portbit){i, k}, &changed);
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

/* A buffer `.names` joining a port bit (in wr->token) to its net, written under another name:
 * port to net for an input, net to port for an output. */
static void port_buffer(blif_writer *wr, odin3_dir dir, odin3_bytes net) {
    wr_begin(wr, ".names");
    if (dir == ODIN3_DIR_IN) {
        wr_token_built(wr);
        wr_token(wr, net);
    } else {
        wr_token(wr, net);
        wr_token_built(wr);
    }
    wr_end(wr);
    wr_put(wr, odin3_bytes_cstr("1 1"));
    wr_newline(wr);
}

/* The buffer of the port bit at `pin` of port `at` when its net is written under another name. */
static odin3_status port_bit_buffer(blif_writer *wr, uint32_t at, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(wr->module, pin);
    if (!odin3_net_valid(net)) {
        return ODIN3_OK;
    }
    bool changed = false;
    odin3_status st =
        port_bit_token(wr, (wr_portbit){at, odin3_pin_bit(wr->module, pin)}, &changed);
    odin3_bytes name = net_name(wr, net);
    if (st == ODIN3_OK &&
        (name.len != wr->token.len || memcmp(name.ptr, wr->token.data, name.len) != 0)) {
        const odin3_celltype_def *def =
            odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
        port_buffer(wr, def->ports[at].dir, name);
    }
    return st;
}

/*
 * VTR: an output-port bit (§9.2). A net a constant $sop or cell drives is that cell's generator:
 * nothing more under the bit's own name, else the generator again under the bit's name (one
 * driver per net, R16; no buffer). A bit on a $_CONST*_, undriven or unconnected, or of class U
 * gets a buffer from gnd/vcc/gnd/unconn. Any other bit is port_bit_buffer's.
 */
static odin3_status out_bit_vtr(blif_writer *wr, uint32_t at, odin3_pin_id pin) {
    odin3_net_id net = odin3_pin_net(wr->module, pin);
    bool changed = false;
    odin3_status st =
        port_bit_token(wr, (wr_portbit){at, odin3_pin_bit(wr->module, pin)}, &changed);
    if (st != ODIN3_OK) {
        return st;
    }
    int value = const_driver(wr, net);
    wr_gen gen = value < 0 ? source_gen(wr, net, false) : GEN_NONE;
    odin3_bytes name = gen != GEN_NONE ? gen_use(wr, gen) : net_name(wr, net);
    if (gen == GEN_NONE && same_bytes(name, buf_bytes(&wr->token))) {
        return ODIN3_OK;
    }
    if (value >= 0) {
        write_constant(wr, buf_bytes(&wr->token), value);
    } else {
        port_buffer(wr, ODIN3_DIR_OUT, name);
    }
    return ODIN3_OK;
}

/* After the cells: a buffer for each port bit whose net is written under another name (the net
 * has its own name, or it is on several port bits); in the VTR dialect, output-port bits are
 * out_bit_vtr's. */
static odin3_status write_port_buffers(blif_writer *wr) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_module_celltype(wr->module));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_module_port_count(wr->module); i++) {
        odin3_pinslice pins = odin3_node_port(wr->module, odin3_module_port(wr->module, i), 0);
        bool vtr_out = wr->vtr && def->ports[i].dir == ODIN3_DIR_OUT;
        for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
            odin3_pin_id pin = {pins.first.v + k};
            st = vtr_out ? out_bit_vtr(wr, i, pin) : port_bit_buffer(wr, i, pin);
        }
    }
    return st != ODIN3_OK ? wr_fail(wr, st) : wr->st;
}

/* A black-box model's ports: bit k of port p is `p` (scalar) or `p[k]`. */
static odin3_status write_model_ports(blif_writer *wr, const odin3_celltype_def *def) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = port_line(wr, def, i);
        const odin3_port_def *port = &def->ports[i];
        for (uint32_t k = 0; st == ODIN3_OK && k < port->width; k++) {
            st = bit_token(wr, port->scalar, odin3_bytes_cstr(port->name), (int64_t)k);
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

/* The next blank-separated word of *text, advancing *text past it; false at the end. */
static bool next_word(const char **text, odin3_bytes *word) {
    const char *at = *text;
    while (*at == ' ') {
        at++;
    }
    const char *end = strchr(at, ' ');
    end = end != NULL ? end : at + strlen(at);
    *word = (odin3_bytes){at, (size_t)(end - at)};
    *text = end;
    return end != at;
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
    odin3_bytes word = {NULL, 0};
    while (next_word(&text, &word)) {
        wr_token(wr, word);
    }
    wr_end(wr);
}

/* --- cells --------------------------------------------------------------------------------- */

/* Writes the name of the net on the (first) pin of a 1-bit port of node. */
static void pin_token(blif_writer *wr, odin3_node_id node, uint32_t port) {
    wr_token(wr, pin_name(wr, odin3_node_port(wr->module, node, port).first));
}

/* Cover rows (inputs + 1 bytes each, no blanks) as lines `<plane> <out>` (`<out>` alone for
 * zero inputs). */
static void write_rows(blif_writer *wr, const uint8_t *bits, uint32_t len, uint32_t inputs) {
    const char *rows = (const char *)bits;
    for (uint32_t at = 0; at + inputs < len; at += inputs + 1) {
        if (inputs > 0) {
            wr_put(wr, (odin3_bytes){rows + at, inputs});
            wr_put(wr, odin3_bytes_cstr(" "));
        }
        wr_put(wr, (odin3_bytes){rows + at + inputs, 1});
        wr_newline(wr);
    }
}

/* `.names` with the A pins and Y, then the cover rows as stored. */
static odin3_status write_names(blif_writer *wr, odin3_node_id node) {
    const odin3_value *cover = odin3_node_param(wr->module, node, SOP_COVER);
    wr_begin(wr, ".names");
    odin3_pinslice in = odin3_node_port(wr->module, node, 0);
    for (uint32_t k = 0; k < in.count; k++) {
        wr_token(wr, pin_name(wr, (odin3_pin_id){in.first.v + k}));
    }
    pin_token(wr, node, 1);
    wr_end(wr);
    write_rows(wr, cover->bits, cover->len, cover->cover_inputs);
    return wr->st;
}

/*
 * VTR: a $sop or gate as `.names` (spec 3E §6) with its inputs read per §9.2. A cell with inputs
 * whose function is constant is the zero-input generator on its output (its inputs dropped); a
 * 1-input one has the one row `1 1` or `0 1`; any other is written with its rows (a $sop's as
 * stored, a gate's from §6's table), a zero-input $sop included.
 */
static odin3_status write_names_vtr(blif_writer *wr, odin3_node_id node,
                                    const odin3_blif_vtr_rows *rows) {
    odin3_blif_vtr_fn fn =
        rows->inputs > 0 ? odin3_blif_vtr_function(wr->module, node) : ODIN3_BLIF_VTR_FN_COVER;
    bool constant = fn == ODIN3_BLIF_VTR_FN_CONST0 || fn == ODIN3_BLIF_VTR_FN_CONST1;
    odin3_pinslice pins = odin3_node_pins(wr->module, node);
    odin3_bytes out = pin_name(wr, (odin3_pin_id){pins.first.v + rows->inputs});
    if (constant) {
        write_constant(wr, out, fn == ODIN3_BLIF_VTR_FN_CONST1 ? 1 : 0);
        return wr->st;
    }
    wr_begin(wr, ".names");
    for (uint32_t k = 0; k < rows->inputs; k++) {
        wr_token(wr, source_name(wr, (odin3_pin_id){pins.first.v + k}, false));
    }
    wr_token(wr, out);
    wr_end(wr);
    if (rows->inputs == 0) {
        note_generator(wr, out);
    }
    if (fn == ODIN3_BLIF_VTR_FN_BUF || fn == ODIN3_BLIF_VTR_FN_INV) {
        wr_put(wr, odin3_bytes_cstr(fn == ODIN3_BLIF_VTR_FN_BUF ? "1 1" : "0 1"));
        wr_newline(wr);
    } else {
        write_rows(wr, rows->bits, rows->len, rows->inputs);
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

/* VTR: `.latch <D> <Q> re <C> <init>` (spec 3E §7): D and C read per §9.2, an unconnected Q
 * under its generated `$p<pin ID>`, init 0 1 2 2 for INIT 0 1 2 3. */
static odin3_status write_latch_vtr(blif_writer *wr, odin3_node_id node) {
    static const char *const INIT_WORDS[] = {"0", "1", "2", "2"};
    int64_t init = odin3_node_param(wr->module, node, LATCH_INIT)->i;
    bool known = init >= 0 && (uint64_t)init < sizeof INIT_WORDS / sizeof INIT_WORDS[0];
    assert(known); /* the reader gives 0..3 (R8); a pass that sets another value is a bug */
    wr_begin(wr, ".latch");
    wr_token(wr, source_name(wr, odin3_node_port(wr->module, node, TYPED_D).first, false));
    pin_token(wr, node, TYPED_Q);
    wr_token(wr, odin3_bytes_cstr(LATCH_WORDS[WR_DFF_P]));
    wr_token(wr, source_name(wr, odin3_node_port(wr->module, node, CTRL_PORT).first, false));
    wr_token(wr, odin3_bytes_cstr(known ? INIT_WORDS[init] : INIT_WORDS[2]));
    wr_end(wr);
    return wr->st;
}

/* The module type instantiates, NULL when type is no module's. */
static const odin3_module *module_of_type(const blif_writer *wr, odin3_celltype_id type) {
    uint64_t id = 0;
    if (!odin3_u64map_get(wr->type_module, type.v, &id)) {
        return NULL;
    }
    return odin3_module_get((odin3_design *)wr->design, (odin3_module_id){(uint32_t)id});
}

/* The formal base name of port `at` of type: a module's port base name (port_base), else the
 * port's name. */
static odin3_bytes formal_base(const blif_writer *wr, odin3_celltype_id type, uint32_t at) {
    const odin3_module *module = module_of_type(wr, type);
    if (module != NULL) {
        return port_base(wr, module, at);
    }
    return odin3_bytes_cstr(odin3_celltype_get(wr->design, type)->ports[at].name);
}

/* The index the formal of bit where.bit of port where.port of type is written with: a module's
 * declared port index (port_index_of), else the bit position. */
static int64_t formal_index(const blif_writer *wr, odin3_celltype_id type, wr_portbit where) {
    const odin3_module *module = module_of_type(wr, type);
    return module != NULL ? port_index_of(module, where) : (int64_t)where.bit;
}

/* A formal: a port and the index its bit is written with. */
typedef struct wr_formal {
    uint32_t port;
    int64_t index;
} wr_formal;

/* The bit position of formal.port of type whose formal index is formal.index; UINT32_MAX when
 * none. */
static uint32_t formal_position(const blif_writer *wr, odin3_celltype_id type, wr_formal formal) {
    const odin3_module *module = module_of_type(wr, type);
    int64_t pos = formal.index;
    if (module != NULL) {
        odin3_wire_id wire = odin3_module_port_wire(module, formal.port);
        int64_t msb = odin3_wire_msb(module, wire);
        int64_t lsb = odin3_wire_lsb(module, wire);
        pos = msb >= lsb ? formal.index - lsb : lsb - formal.index;
    }
    return pos >= 0 && pos < UINT32_MAX ? (uint32_t)pos : UINT32_MAX;
}

/* True when an earlier port of node's type has the formal base `base` and a connected pin of the
 * same formal index as where: a model with a name in both .inputs and .outputs has two ports of
 * one BLIF name, and BLIF cannot connect both. */
static bool formal_taken(const blif_writer *wr, odin3_node_id node, wr_portbit where,
                         odin3_bytes base) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    int64_t index = formal_index(wr, type, where);
    for (uint32_t j = 0; j < where.port; j++) {
        odin3_bytes other = formal_base(wr, type, j);
        odin3_pinslice pins = odin3_node_port(wr->module, node, j);
        uint32_t bit = formal_position(wr, type, (wr_formal){j, index});
        if (other.len == base.len && memcmp(other.ptr, base.ptr, base.len) == 0 &&
            bit < pins.count &&
            odin3_net_valid(odin3_pin_net(wr->module, (odin3_pin_id){pins.first.v + bit}))) {
            return true;
        }
    }
    return false;
}

/* The declaration of type the writer reproduces (its first declared-model entry), or NULL with
 * *index untouched when the design does not declare type. */
static const odin3_celltype_def *declared_of(const blif_writer *wr, odin3_celltype_id type,
                                             uint32_t *index) {
    uint64_t at = 0;
    if (!odin3_u64map_get(wr->declared, type.v, &at)) {
        return NULL;
    }
    *index = (uint32_t)at;
    return odin3_design_declared_model_decl(wr->design, *index);
}

/* Whether formals spell port `at` of type without brackets: as declared, else as the type says. */
static bool formal_scalar(const blif_writer *wr, odin3_celltype_id type, uint32_t at) {
    const odin3_port_def *port = &odin3_celltype_get(wr->design, type)->ports[at];
    uint32_t index = 0;
    const odin3_celltype_def *decl = declared_of(wr, type, &index);
    for (uint32_t i = 0; decl != NULL && i < decl->n_ports; i++) {
        if (strcmp(decl->ports[i].name, port->name) == 0) {
            return decl->ports[i].scalar;
        }
    }
    return port->scalar;
}

/* The parameters the BLIF reader gives node's .subckt (frontends/blif/reader.h): a declared
 * model's, else those its connected formals imply. seen is scratch for one width per port. */
typedef struct wr_implied {
    const odin3_design *design;
    const odin3_module *module;
    odin3_node_id node;
    const odin3_u64map *declared; /* type -> first declared-model entry; NULL: scan the list */
    uint32_t *seen;               /* one per port of the type */
    odin3_value *params;          /* receives one per parameter of the type */
} wr_implied;

/* True and *index = the first declared-model entry of type when the design declares it. */
static bool first_declared(const wr_implied *req, odin3_celltype_id type, uint32_t *index) {
    uint64_t at = 0;
    if (req->declared != NULL) {
        bool found = odin3_u64map_get(req->declared, type.v, &at);
        *index = (uint32_t)at;
        return found;
    }
    uint32_t count = odin3_design_declared_model_count(req->design);
    for (uint32_t i = 0; i < count; i++) {
        if (odin3_design_declared_model(req->design, i).v == type.v) {
            *index = i;
            return true;
        }
    }
    return false;
}

static odin3_status implied_params(const wr_implied *req) {
    odin3_celltype_id type = odin3_node_type(req->module, req->node);
    const odin3_celltype_def *def = odin3_celltype_get(req->design, type);
    uint32_t index = 0;
    const odin3_value *declared = first_declared(req, type, &index)
                                      ? odin3_design_declared_model_params(req->design, index)
                                      : NULL;
    if (declared != NULL) {
        memcpy(req->params, declared, sizeof *declared * def->n_params);
        return ODIN3_OK;
    }
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice pins = odin3_node_port(req->module, req->node, port);
        req->seen[port] = 0;
        for (uint32_t k = pins.count; k > 0 && req->seen[port] == 0; k--) {
            if (odin3_net_valid(odin3_pin_net(req->module, (odin3_pin_id){pins.first.v + k - 1}))) {
                req->seen[port] = k;
            }
        }
    }
    return odin3_celltype_infer_params(req->design, type, req->seen, req->params);
}

/* Appends `'name'` to bad, after `, ` unless it is the first (*first is then cleared). */
static odin3_status list_param(odin3_strbuf *bad, const char *name, bool *first) {
    odin3_status st = *first ? ODIN3_OK : odin3_strbuf_append(bad, odin3_bytes_cstr(", "));
    *first = false;
    return st == ODIN3_OK ? odin3_strbuf_appendf(bad, "'%s'", name) : st;
}

/* odin3_blif_params_check for req's node (its design, module and declared map). */
static odin3_status params_check(wr_implied req, odin3_strbuf *bad, bool *ok) {
    const odin3_celltype_def *def =
        odin3_celltype_get(req.design, odin3_node_type(req.module, req.node));
    *ok = true;
    if (def->n_params == 0) {
        return ODIN3_OK;
    }
    req.seen = odin3_util_malloc(sizeof(uint32_t) * (def->n_ports + 1U));
    req.params = odin3_util_malloc(sizeof(odin3_value) * def->n_params);
    odin3_status st =
        req.seen != NULL && req.params != NULL ? implied_params(&req) : ODIN3_ERR_NO_MEMORY;
    bool first = true;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_params; i++) {
        if (!odin3_value_equal(odin3_node_param(req.module, req.node, i), &req.params[i])) {
            *ok = false;
            st = list_param(bad, def->params[i].name, &first);
        }
    }
    odin3_util_free(req.seen);
    odin3_util_free(req.params);
    return st;
}

odin3_status odin3_blif_params_check(const odin3_design *design, const odin3_module *module,
                                     odin3_node_id node, odin3_strbuf *bad, bool *ok) {
    return params_check((wr_implied){design, module, node, NULL, NULL, NULL}, bad, ok);
}

/* The actual of a connected .subckt pin: its net's name; in the VTR dialect, an input's source
 * per §9.2 (x reads `unconn`). */
static odin3_bytes subckt_actual(blif_writer *wr, odin3_dir dir, odin3_pin_id pin) {
    return wr->vtr && dir == ODIN3_DIR_IN ? source_name(wr, pin, true)
                                          : net_name(wr, odin3_pin_net(wr->module, pin));
}

/* The connected pins of port `at` of node as `formal=actual` tokens. */
static odin3_status subckt_port(blif_writer *wr, odin3_node_id node, uint32_t at) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    const odin3_port_def *port = &odin3_celltype_get(wr->design, type)->ports[at];
    bool scalar = formal_scalar(wr, type, at);
    odin3_bytes base = formal_base(wr, type, at);
    bool renamed = base.len != strlen(port->name) || memcmp(base.ptr, port->name, base.len) != 0;
    odin3_pinslice pins = odin3_node_port(wr->module, node, at);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (!odin3_net_valid(odin3_pin_net(wr->module, pin))) {
            continue;
        }
        if (renamed && formal_taken(wr, node, (wr_portbit){at, k}, base)) {
            return wr_refuse(wr, "a .subckt cannot connect both ports of the BLIF name",
                             (const char *)base.ptr);
        }
        st = bit_token(wr, scalar, base, formal_index(wr, type, (wr_portbit){at, k}));
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, odin3_bytes_cstr("=")) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, subckt_actual(wr, port->dir, pin))
                            : st;
        if (st == ODIN3_OK) {
            wr_token_built(wr);
        }
    }
    return st;
}

/* The port of def that is port `at` of its declaration (IR-7b: a bijection by name). */
static uint32_t port_of_decl(const odin3_celltype_def *def, const odin3_celltype_def *decl,
                             uint32_t at) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, decl->ports[at].name) == 0) {
            return i;
        }
    }
    assert(false); /* declare_blackbox checked the names */
    return at;
}

/* Refuses a cell of def whose parameters the BLIF reader would not give it back (declared, or
 * implied by the connected formals). */
static odin3_status subckt_params_ok(blif_writer *wr, odin3_node_id node,
                                     const odin3_celltype_def *def) {
    bool ok = true;
    odin3_strbuf_clear(&wr->cand);
    odin3_status checked = params_check(
        (wr_implied){wr->design, wr->module, node, wr->declared, NULL, NULL}, &wr->cand, &ok);
    if (checked != ODIN3_OK) {
        return wr_fail(wr, checked);
    }
    if (!ok) {
        return wr_refuse(wr,
                         "parameters neither declared nor implied by the connected formals "
                         "have no BLIF form; cannot write a cell of",
                         def->name);
    }
    return ODIN3_OK;
}

/* `.subckt type formal=actual …` in port order (a declared type's: in declaration order),
 * unconnected pins skipped. */
static odin3_status write_subckt(blif_writer *wr, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_node_type(wr->module, node));
    if (subckt_params_ok(wr, node, def) != ODIN3_OK) {
        return wr->st;
    }
    wr_begin(wr, ".subckt");
    wr_token(wr, odin3_bytes_cstr(def->name));
    uint32_t index = 0;
    const odin3_celltype_def *decl = declared_of(wr, odin3_node_type(wr->module, node), &index);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = subckt_port(wr, node, decl != NULL ? port_of_decl(def, decl, i) : i);
    }
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    wr_end(wr);
    return wr->st;
}

/* VTR (spec 3E §8.2): whether the formals of port `at` of type, `width` bits at the node, are
 * written `p` rather than `p[k]`: one bit, spelled so by the declaration when the type has one
 * (the round trip), else always (Parmys's scalar `data=`, `we=`, `out=`). */
static bool vtr_formal_scalar(const blif_writer *wr, odin3_celltype_id type, uint32_t at,
                              uint32_t width) {
    uint32_t index = 0;
    return width == 1 && (declared_of(wr, type, &index) == NULL || formal_scalar(wr, type, at));
}

/* VTR (§8.2): every bit of port `at` of node as `formal=actual`, bits beyond the node's width
 * not written: an input reads its source per §9.2 (x, undriven and unconnected read `unconn`, a
 * clock's constant gnd/vcc), an output its net or, unconnected, its generated `$p<pin ID>`. */
static odin3_status subckt_port_vtr(blif_writer *wr, odin3_node_id node, uint32_t at) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    const odin3_port_def *port = &odin3_celltype_get(wr->design, type)->ports[at];
    odin3_pinslice pins = odin3_node_port(wr->module, node, at);
    bool scalar = vtr_formal_scalar(wr, type, at, pins.count);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_bytes actual =
            port->dir == ODIN3_DIR_IN ? source_name(wr, pin, true) : pin_name(wr, pin);
        st = bit_token(wr, scalar, odin3_bytes_cstr(port->name), (int64_t)k);
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, odin3_bytes_cstr("=")) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(&wr->token, actual) : st;
        if (st == ODIN3_OK) {
            wr_token_built(wr);
        }
    }
    return st;
}

/* The port of def whose name follows that of port `prev` in byte order (the first for
 * UINT32_MAX); def->n_ports after the last. Port names are distinct. */
static uint32_t next_port_by_name(const odin3_celltype_def *def, uint32_t prev) {
    uint32_t best = def->n_ports;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        bool after = prev >= def->n_ports || strcmp(def->ports[i].name, def->ports[prev].name) > 0;
        if (after &&
            (best == def->n_ports || strcmp(def->ports[i].name, def->ports[best].name) < 0)) {
            best = i;
        }
    }
    return best;
}

/* VTR: `.subckt type formal=actual …` with every formal bit of the node, sorted as Parmys sorts
 * them (§8.2): ports by name in byte order, bits by index, so VPR names the block after the same
 * first output formal (R12). */
static odin3_status write_subckt_vtr(blif_writer *wr, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(wr->design, odin3_node_type(wr->module, node));
    if (subckt_params_ok(wr, node, def) != ODIN3_OK) {
        return wr->st;
    }
    wr_begin(wr, ".subckt");
    wr_token(wr, odin3_bytes_cstr(def->name));
    odin3_status st = ODIN3_OK;
    for (uint32_t at = next_port_by_name(def, UINT32_MAX); st == ODIN3_OK && at < def->n_ports;
         at = next_port_by_name(def, at)) {
        st = subckt_port_vtr(wr, node, at);
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
    const odin3_value *value =
        (attr || param) && odin3_strtab_find(wr->tab, key, &str)
            ? string_attr(wr->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, str)
            : NULL;
    if (value == NULL) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s: model '%s': blif_extras lists '%.*s', which is not a .attr/.param string "
                  "attribute of the cell; skipped",
                  wr->path, wr->model, (int)key.len, (const char *)key.ptr);
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
    odin3_bytes word = {NULL, 0};
    while (next_word(&text, &word)) {
        write_extra(wr, node, word);
    }
}

/* True for one of the NO_BLIF_FORM types, resolved to their IDs once in wr_init. */
static bool lacks_blif_form(const blif_writer *wr, odin3_celltype_id type) {
    for (uint32_t i = 0; i < WR_NO_BLIF; i++) {
        if (wr->no_blif[i].v != 0 && wr->no_blif[i].v == type.v) {
            return true;
        }
    }
    return false;
}

/* VTR: one cell (spec 3E §6): a $sop or gate as .names, a $_CONST*_ as no line (its readers
 * read gnd/vcc/unconn, §9), $_DFF_P_ as .latch, a hard cell as .subckt (the preflight refused
 * every other cell). No .cname/.attr/.param: VPR reads them in extended BLIF only (R3). */
static odin3_status write_cell_vtr(blif_writer *wr, odin3_node_id node) {
    odin3_blif_vtr_rows rows;
    if (odin3_blif_vtr_rows_of(wr->module, node, &rows)) {
        return write_names_vtr(wr, node, &rows);
    }
    if (odin3_blif_vtr_const_cell(wr->module, node)) {
        return wr->st;
    }
    if (builtin_of(wr, odin3_node_type(wr->module, node)) == WR_DFF_P) {
        return write_latch_vtr(wr, node);
    }
    return write_subckt_vtr(wr, node);
}

static odin3_status write_cell(blif_writer *wr, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(wr->module, node);
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
    if (def->gran == ODIN3_GRAN_PORT) {
        return ODIN3_OK;
    }
    if (lacks_blif_form(wr, type)) {
        return wr_refuse(
            wr, "word flip-flops and latches have no BLIF form; cannot write a cell of", def->name);
    }
    if (wr->vtr) {
        return write_cell_vtr(wr, node);
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

/* The cells in node ID order (port nodes write nothing). */
static odin3_status write_cells(blif_writer *wr) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(wr->module); i++) {
        if (odin3_node_live(wr->module, (odin3_node_id){i})) {
            st = write_cell(wr, (odin3_node_id){i});
        }
    }
    return st;
}

/* VTR (spec 3E §5 item 4): a dry run of the cells and the port buffers, which writes nothing,
 * finds the generators they read and those a zero-input $sop already is; the others are then
 * written. */
static odin3_status write_vtr_generators(blif_writer *wr) {
    uint32_t line = wr->line;
    wr->scan = true;
    odin3_status st = write_cells(wr);
    if (st == ODIN3_OK) {
        st = write_port_buffers(wr);
    }
    wr->scan = false;
    wr->line = line;
    wr->col = 0;
    if (st == ODIN3_OK) {
        write_generators(wr);
    }
    return wr->st;
}

/* --- models -------------------------------------------------------------------------------- */

/* A module's names, as name_module leaves them in the writer (see blif_writer). */
struct odin3_blif_names {
    odin3_strtab *gen_names;
    odin3_u64map *claims, *net_port, *gen, *port_of, *clocks;
};

/* Moves the names in from (each table it holds) to the writer's tables, leaving from empty. */
static void names_take(blif_writer *wr, odin3_blif_names *from) {
    wr->gen_names = from->gen_names;
    wr->claims = from->claims;
    wr->net_port = from->net_port;
    wr->gen = from->gen;
    wr->port_of = from->port_of;
    wr->clocks = from->clocks;
    *from = (odin3_blif_names){0};
}

/* Moves the writer's tables (each it holds) into to, which must be empty. */
static void names_give(blif_writer *wr, odin3_blif_names *to) {
    *to = (odin3_blif_names){wr->gen_names, wr->claims,  wr->net_port,
                             wr->gen,       wr->port_of, wr->clocks};
    wr->gen_names = NULL;
    wr->claims = NULL;
    wr->net_port = NULL;
    wr->gen = NULL;
    wr->port_of = NULL;
    wr->clocks = NULL;
}

void odin3_blif_names_free(odin3_blif_names *names) {
    if (names == NULL) {
        return;
    }
    odin3_strtab_destroy(names->gen_names);
    odin3_u64map_destroy(names->claims);
    odin3_u64map_destroy(names->net_port);
    odin3_u64map_destroy(names->gen);
    odin3_u64map_destroy(names->port_of);
    odin3_u64map_destroy(names->clocks);
    odin3_util_free(names);
}

/* Ends the module being written: lent names go back to their owner (the plan), the writer's own
 * are freed. */
static void end_module(blif_writer *wr) {
    if (wr->names != NULL && wr->module != NULL) {
        names_give(wr, wr->names);
    }
    odin3_strtab_destroy(wr->gen_names);
    odin3_u64map_destroy(wr->claims);
    odin3_u64map_destroy(wr->net_port);
    odin3_u64map_destroy(wr->gen);
    odin3_u64map_destroy(wr->port_of);
    odin3_u64map_destroy(wr->clocks);
    wr->clocks = NULL;
    wr->gen_names = NULL;
    wr->claims = NULL;
    wr->net_port = NULL;
    wr->gen = NULL;
    wr->port_of = NULL;
    wr->module = NULL;
}

/* The ID of the module written at position pos (from 1): the top first, then the rest in creation
 * order. */
static uint32_t written_module(const odin3_design *design, uint32_t pos) {
    uint32_t top = odin3_design_top(design).v;
    if (top == 0) {
        return pos;
    }
    if (pos == 1) {
        return top;
    }
    return pos <= top ? pos - 1 : pos;
}

/* Writes the pos-th model (from 1); models after the first follow a blank line. */
static odin3_status write_module(blif_writer *wr, uint32_t pos) {
    uint32_t id = written_module(wr->design, pos);
    wr->module = odin3_module_get((odin3_design *)wr->design, (odin3_module_id){id});
    wr->model = odin3_strtab_get(wr->tab, odin3_module_name(wr->module));
    odin3_status st = ODIN3_OK;
    if (wr->names != NULL) {
        names_take(wr, wr->names); /* VTR: the preflight named the top (its plan) */
    } else {
        st = wr_fail(wr, name_module(wr));
    }
    if (pos > 1) {
        wr_newline(wr);
    }
    wr_begin(wr, ".model");
    wr_token(wr, str_bytes(wr, odin3_module_name(wr->module)));
    wr_end(wr);
    st = st == ODIN3_OK ? write_module_ports(wr) : st;
    if (st == ODIN3_OK && !wr->vtr) {
        write_clock(wr); /* VPR has no .clock (spec 3E R3) */
    }
    if (st == ODIN3_OK && wr->vtr) {
        st = write_vtr_generators(wr);
    }
    st = st == ODIN3_OK ? write_cells(wr) : st;
    if (st == ODIN3_OK) {
        (void)write_port_buffers(wr);
    }
    wr_begin(wr, ".end");
    wr_end(wr);
    end_module(wr);
    return wr->st;
}

/* A blank line, then the black-box model decl: `.model`, its ports, `.blackbox`, `.end`. */
static void write_blackbox_model(blif_writer *wr, const odin3_celltype_def *decl) {
    wr->model = decl->name;
    wr_newline(wr);
    wr_begin(wr, ".model");
    wr_token(wr, odin3_bytes_cstr(decl->name));
    wr_end(wr);
    if (write_model_ports(wr, decl) == ODIN3_OK) {
        wr_begin(wr, ".blackbox");
        wr_end(wr);
        wr_begin(wr, ".end");
        wr_end(wr);
    }
}

/* Each declared black-box model once (its first declaration), in declaration order. */
static odin3_status write_blackboxes(blif_writer *wr) {
    uint32_t count = odin3_design_declared_model_count(wr->design);
    for (uint32_t i = 0; wr->st == ODIN3_OK && i < count; i++) {
        uint32_t first = 0;
        const odin3_celltype_def *decl =
            declared_of(wr, odin3_design_declared_model(wr->design, i), &first);
        if (decl != NULL && first == i) {
            write_blackbox_model(wr, decl);
        }
    }
    return wr->st;
}

/* --- VTR: the hard-block stubs (spec 3E §8.3) ------------------------------------------------- */

/* A hard type a stub is written for. */
typedef struct wr_stub {
    const char *name;
    odin3_celltype_id type;
} wr_stub;

static int stub_cmp(const void *left, const void *right) {
    return strcmp(((const wr_stub *)left)->name, ((const wr_stub *)right)->name);
}

/* stubs = the hard types the live nodes of top use, each once, sorted by name in byte order. */
static odin3_status used_hard_types(const blif_writer *wr, const odin3_module *top,
                                    odin3_vec *stubs) {
    for (uint32_t i = 1; i < odin3_module_node_end(top); i++) {
        odin3_node_id node = {i};
        odin3_celltype_id type = odin3_node_type(top, node);
        if (!odin3_node_live(top, node) || !hard_type(wr->design, type)) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < stubs->len && !seen; j++) {
            seen = ((const wr_stub *)odin3_vec_cat(stubs, j))->type.v == type.v;
        }
        wr_stub *add = seen ? NULL : odin3_vec_push(stubs);
        if (!seen && add == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (add != NULL) {
            *add = (wr_stub){odin3_celltype_get(wr->design, type)->name, type};
        }
    }
    if (stubs->len > 1) {
        qsort(stubs->data, stubs->len, sizeof(wr_stub), stub_cmp);
    }
    return ODIN3_OK;
}

/* ports[i].width = the widest port i of type is on a live node of top (§8.3). */
static void widest_ports(const odin3_module *top, odin3_celltype_id type, odin3_port_def *ports,
                         uint32_t n_ports) {
    for (uint32_t i = 1; i < odin3_module_node_end(top); i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(top, node) || odin3_node_type(top, node).v != type.v) {
            continue;
        }
        for (uint32_t at = 0; at < n_ports; at++) {
            uint32_t width = odin3_node_port(top, node, at).count;
            ports[at].width = width > ports[at].width ? width : ports[at].width;
        }
    }
}

/* The stub of an undeclared library cell (§8.3): its ports in library order, each at the widest
 * width a live node of top gives it, `p` (scalar) when that is 1 (as its formals, §8.2), else
 * `p[0] … p[w-1]`. */
static odin3_status write_lib_stub(blif_writer *wr, const odin3_module *top,
                                   odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
    odin3_port_def *ports = odin3_util_calloc(sizeof *ports * (def->n_ports + 1U));
    if (ports == NULL) {
        return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t at = 0; at < def->n_ports; at++) {
        ports[at] = (odin3_port_def){.name = def->ports[at].name, .dir = def->ports[at].dir};
    }
    widest_ports(top, type, ports, def->n_ports);
    for (uint32_t at = 0; at < def->n_ports; at++) {
        ports[at].scalar = ports[at].width == 1;
    }
    odin3_celltype_def stub = {.name = def->name, .ports = ports, .n_ports = def->n_ports};
    write_blackbox_model(wr, &stub);
    odin3_util_free(ports);
    return wr->st;
}

/* §5 item 8: after the top's .end, one stub per hard type a live node of the top uses, sorted by
 * name: a declared type as declared (only the used ones: VPR checks every stub against the arch,
 * R13), an undeclared library cell by write_lib_stub. */
static odin3_status write_stubs(blif_writer *wr) {
    const odin3_module *top =
        odin3_module_get((odin3_design *)wr->design, odin3_design_top(wr->design));
    odin3_vec stubs;
    odin3_vec_init(&stubs, sizeof(wr_stub));
    odin3_status st = used_hard_types(wr, top, &stubs);
    if (st != ODIN3_OK) {
        (void)wr_fail(wr, st);
    }
    for (size_t j = 0; wr->st == ODIN3_OK && j < stubs.len; j++) {
        const wr_stub *stub = odin3_vec_cat(&stubs, j);
        uint32_t first = 0;
        const odin3_celltype_def *decl = declared_of(wr, stub->type, &first);
        if (decl != NULL) {
            write_blackbox_model(wr, decl);
        } else {
            (void)write_lib_stub(wr, top, stub->type);
        }
    }
    odin3_vec_free(&stubs);
    return wr->st;
}

/* --- entry point --------------------------------------------------------------------------- */

static uint32_t find_key(const blif_writer *wr, const char *key) {
    uint32_t str = 0;
    return odin3_strtab_find(wr->tab, odin3_bytes_cstr(key), &str) ? str : 0;
}

/* The ID of the cell type registered under name, or ID 0 when there is none. */
static odin3_celltype_id find_type(const blif_writer *wr, const char *name) {
    uint32_t str = find_key(wr, name);
    odin3_celltype_id type = {0};
    if (str == 0 || !odin3_celltype_find(wr->design, str, &type)) {
        return (odin3_celltype_id){0};
    }
    return type;
}

static odin3_status wr_init(blif_writer *wr, const odin3_design *design, const char *path,
                            const odin3_blif_write_opts *opts) {
    *wr = (blif_writer){.design = design,
                        .tab = odin3_design_strtab(design),
                        .path = path,
                        .opts = opts,
                        .vtr = opts->dialect == ODIN3_BLIF_DIALECT_VTR};
    odin3_strbuf_init(&wr->token);
    odin3_strbuf_init(&wr->cand);
    odin3_strbuf_init(&wr->orig);
    odin3_strbuf_init(&wr->legal);
    odin3_strbuf_init(&wr->first_orig);
    odin3_strbuf_init(&wr->first_legal);
    wr->keys = (wr_keys){.clock = find_key(wr, ODIN3_BLIF_ATTR_CLOCK),
                         .port_name = find_key(wr, ODIN3_BLIF_ATTR_PORT_NAME),
                         .extras = find_key(wr, ODIN3_BLIF_ATTR_EXTRAS)};
    for (uint32_t i = 0; i < WR_BUILTINS; i++) {
        wr->builtin[i] = find_type(wr, BUILTIN_NAMES[i]);
    }
    for (uint32_t i = 0; i < WR_NO_BLIF; i++) {
        wr->no_blif[i] = find_type(wr, NO_BLIF_FORM[i]);
    }
    wr->type_module = odin3_u64map_create(0);
    wr->declared = odin3_u64map_create(0);
    if (wr->type_module == NULL || wr->declared == NULL) {
        return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t i = odin3_design_declared_model_count(design); i > 0; i--) {
        /* backwards, so each type ends up mapped to its first entry */
        odin3_kv entry = {odin3_design_declared_model(design, i - 1).v, i - 1};
        if (odin3_u64map_put(wr->declared, entry) != ODIN3_OK) {
            return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
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

/* Creates a private temporary beside the destination; the destination is untouched until the
 * rename. */
static odin3_status wr_open(blif_writer *wr) {
    wr->buffer = odin3_util_malloc(IO_BUFFER);
    if (wr->buffer == NULL) {
        return wr_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    odin3_status st = odin3_atomic_file_open(&wr->file, wr->path);
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    wr->opened = true;
    if (setvbuf(wr->file.fp, wr->buffer, _IOFBF, IO_BUFFER) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot set the output buffer", wr->path);
        return wr_fail(wr, ODIN3_ERR_IO);
    }
    return ODIN3_OK;
}

/*
 * Flushes the temporary (a failure is a located write error), then renames it over the destination
 * on success or removes it on any failure, so a failed write leaves an existing file as it was.
 */
static void wr_close(blif_writer *wr) {
    if (!wr->opened) {
        return;
    }
    wr->opened = false;
    if (wr->st == ODIN3_OK) {
        errno = 0;
        bool failed = fflush(wr->file.fp) != 0;
        int saved = errno;
        if (failed || ferror(wr->file.fp) != 0) {
            odin3_log(ODIN3_LOG_ERROR, "%s:%u: write failed: %s", wr->path, (unsigned)wr->line,
                      strerror(saved != 0 ? saved : EIO));
            (void)wr_fail(wr, ODIN3_ERR_IO);
        }
    }
    wr->st = odin3_atomic_file_close(&wr->file, wr->st);
}

static void wr_free(blif_writer *wr) {
    end_module(wr);
    odin3_util_free(wr->buffer);
    odin3_strbuf_free(&wr->token);
    odin3_strbuf_free(&wr->cand);
    odin3_strbuf_free(&wr->orig);
    odin3_strbuf_free(&wr->legal);
    odin3_strbuf_free(&wr->first_orig);
    odin3_strbuf_free(&wr->first_legal);
    odin3_u64map_destroy(wr->type_module);
    odin3_u64map_destroy(wr->declared);
}

/* The options' refusals for odin3_blif_write_with (plan 3E points (m) and (n)), before the
 * preflight (whose own option checks serve its direct callers); logged, ODIN3_ERR_INVALID_ARG. */
static odin3_status check_opts(const odin3_blif_write_opts *opts) {
    bool vtr = opts->dialect == ODIN3_BLIF_DIALECT_VTR;
    bool provenance = opts->name_style == ODIN3_BLIF_NAMES_PROVENANCE;
    const char *why = NULL;
    if (!vtr && opts->dialect != ODIN3_BLIF_DIALECT_PLAIN) {
        why = "odin3_blif_write_with: unknown dialect";
    } else if (!provenance && opts->name_style != ODIN3_BLIF_NAMES_SHORT) {
        why = "odin3_blif_write_with: unknown name_style";
    } else if (!vtr && (opts->lut_k != 0 || provenance)) {
        why = "odin3_blif_write_with: lut_k and name_style apply to the VTR dialect only";
    } else if (opts->lut_k != 0 &&
               (opts->lut_k < ODIN3_BLIF_VTR_MIN_K || opts->lut_k > ODIN3_BLIF_VTR_MAX_K)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_write_with: lut_k %u is outside %u..%u",
                  (unsigned)opts->lut_k, ODIN3_BLIF_VTR_MIN_K, ODIN3_BLIF_VTR_MAX_K);
        return ODIN3_ERR_INVALID_ARG;
    } else if (provenance) {
        why = "write_blif: --vtr: --name-style provenance is not available yet (3E-6)";
    }
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The VTR dialect's header (spec 3E §5 item 1): no date, host or path, so reruns are identical. */
static odin3_status write_vtr_header(blif_writer *wr) {
    odin3_strbuf_clear(&wr->token);
    odin3_status st = odin3_strbuf_appendf(
        &wr->token, "# Odin III %s write_blif --vtr (K=%u, names %s)", odin3_version_string(),
        (unsigned)odin3_blif_vtr_k(wr->opts),
        wr->opts->name_style == ODIN3_BLIF_NAMES_PROVENANCE ? "provenance" : "short");
    if (st != ODIN3_OK) {
        return wr_fail(wr, st);
    }
    wr_put(wr, (odin3_bytes){wr->token.data, wr->token.len});
    wr_newline(wr);
    wr_newline(wr);
    return wr->st;
}

/* Plain: every module, then the declared black boxes. VTR: the header, then the top alone. */
static void write_design(blif_writer *wr) {
    if (wr->vtr) {
        /* written_module: position 1 is the top */
        if (write_vtr_header(wr) == ODIN3_OK && write_module(wr, 1) == ODIN3_OK) {
            (void)write_stubs(wr);
        }
        return;
    }
    for (uint32_t i = 1; wr->st == ODIN3_OK && i < odin3_design_module_end(wr->design); i++) {
        (void)write_module(wr, i);
    }
    if (wr->st == ODIN3_OK) {
        (void)write_blackboxes(wr);
    }
}

/* Logs the info line of VB-10 item 9 for the names wr counted. */
static void log_renamed(const blif_writer *wr) {
    odin3_log(ODIN3_LOG_INFO, ODIN3_BLIF_VTR_PREFIX "%u names legalised (first: '%.*s' -> '%.*s')",
              (unsigned)wr->renamed, (int)wr->first_orig.len, wr->first_orig.data,
              (int)wr->first_legal.len, wr->first_legal.data);
}

odin3_status odin3_blif_vtr_name_top(const odin3_blif_names_req *req, odin3_blif_names **names,
                                     uint32_t *renamed) {
    blif_writer wr;
    *names = NULL;
    *renamed = 0;
    odin3_status st = wr_init(&wr, req->design, ODIN3_BLIF_VTR_WHO, req->opts);
    wr.log_names = req->log_names;
    wr.quiet_refusal = !req->log_refusal;
    if (st == ODIN3_OK) {
        wr.module = odin3_module_get((odin3_design *)req->design, odin3_design_top(req->design));
        wr.model = odin3_strtab_get(wr.tab, odin3_module_name(wr.module));
        st = wr_fail(&wr, name_module(&wr));
    }
    if (st == ODIN3_OK) {
        *names = odin3_util_calloc(sizeof **names);
        st = *names != NULL ? ODIN3_OK : wr_fail(&wr, ODIN3_ERR_NO_MEMORY);
    }
    if (st == ODIN3_OK) {
        names_give(&wr, *names);
        *renamed = wr.renamed;
        if (wr.log_names && wr.have_first) {
            log_renamed(&wr);
        }
    }
    wr.module = NULL; /* the tables, if not given, are wr's own: wr_free frees them */
    wr_free(&wr);
    return st;
}

/* Opens the temporary and writes the design with opts (VTR: with the plan's names, lent). */
static odin3_status write_opened(const odin3_design *design, const char *path,
                                 const odin3_blif_write_opts *opts, odin3_blif_names *names) {
    blif_writer wr;
    odin3_status st = wr_init(&wr, design, path, opts);
    wr.names = names;
    if (st == ODIN3_OK) {
        st = wr_open(&wr);
    }
    if (st == ODIN3_OK) {
        write_design(&wr);
    }
    wr_close(&wr); /* the status is sticky: wr.st holds the first failure, closing included */
    st = wr.st;
    wr_free(&wr);
    return st;
}

odin3_status odin3_blif_write(const odin3_design *design, const char *path) {
    return odin3_blif_write_with(design, path, NULL);
}

odin3_status odin3_blif_write_with(const odin3_design *design, const char *path,
                                   const odin3_blif_write_opts *opts) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "blif_write: NULL design or path");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_refuse_processes(design, NULL) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG; /* 2D EL-33: before the path is opened */
    }
    odin3_blif_write_opts use =
        opts != NULL ? *opts
                     : (odin3_blif_write_opts){ODIN3_BLIF_DIALECT_PLAIN, ODIN3_BLIF_NAMES_SHORT, 0};
    odin3_status st = check_opts(&use);
    odin3_blif_vtr_plan plan = {0};
    if (st == ODIN3_OK && use.dialect == ODIN3_BLIF_DIALECT_VTR) {
        odin3_blif_vtr_report report;
        st =
            odin3_blif_vtr_plan_build(design, &use, &plan, &report); /* before the path is opened */
    }
    if (st == ODIN3_OK) {
        st = write_opened(design, path, &use, plan.names);
    }
    if (st == ODIN3_OK) {
        odin3_blif_vtr_log_summary(&plan); /* VTR: once the file is written (plain: none) */
    }
    odin3_blif_vtr_plan_free(&plan);
    return st;
}
