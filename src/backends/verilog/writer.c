/* writer.c — structural Verilog writer: modules, cells, aliases and black-box stubs. */
#include "backends/verilog/writer.h"

#include "backends/common/attrs.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/file.h"
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
    FLUSH_AT = 1 << 16, /* pending output bytes that trigger a write */
    KEY_SHIFT = 32,     /* object-name key: kind << 32 | ID; location: file << 32 | line */
    FIRST_PRINTABLE = 0x20,
    LAST_PRINTABLE = 0x7e,
    OCTAL_DIGITS = 3,
    NODE_ID_TEXT = 16, /* "$c" and a uint32_t */
};

/* --- built-in cell types with a Verilog form ----------------------------------------------- */

typedef enum vw_form {
    FORM_CONST,  /* Y = 1'b<op> */
    FORM_GATE1,  /* Y = <op>A */
    FORM_GATE2,  /* Y = A <op> B, or ~(…) */
    FORM_MUX,    /* Y = S ? B : A */
    FORM_SOP,    /* sum of products over the cover */
    FORM_LATCH,  /* bit-level flip-flop or latch with INIT */
    FORM_BINARY, /* Y = A' <op> B' */
    FORM_UNARY,  /* Y = <op>A' */
    FORM_PMUX,   /* priority chain */
    FORM_TRIBUF, /* Y = EN ? A : z */
    FORM_FF,     /* word-level flip-flop */
    FORM_COUNT
} vw_form;

enum {
    VB_INVERT = 1U << 0, /* gate output inverted */
    VB_SHIFT = 1U << 1,  /* B is a shift amount: never signed */
    VB_NEG = 1U << 2,    /* negative edge or active-low level */
    VB_LEVEL = 1U << 3,  /* level-sensitive latch */
    VB_GCLK = 1U << 4,   /* global clock ($_FF_) */
    VB_EN = 1U << 5,     /* $dffe */
    VB_ARST = 1U << 6,   /* $adff */
    VB_SRST = 1U << 7,   /* $sdff */
};

typedef struct vw_builtin {
    const char *name;
    vw_form form;
    const char *op;
    uint32_t flags;
} vw_builtin;

static const vw_builtin BUILTINS[] = {
    {"$_CONST0_", FORM_CONST, "0", 0},
    {"$_CONST1_", FORM_CONST, "1", 0},
    {"$_CONSTX_", FORM_CONST, "x", 0},
    {"$_CONSTZ_", FORM_CONST, "z", 0},
    {"$_BUF_", FORM_GATE1, "", 0},
    {"$_NOT_", FORM_GATE1, "~", 0},
    {"$_AND_", FORM_GATE2, "&", 0},
    {"$_OR_", FORM_GATE2, "|", 0},
    {"$_XOR_", FORM_GATE2, "^", 0},
    {"$_NAND_", FORM_GATE2, "&", VB_INVERT},
    {"$_NOR_", FORM_GATE2, "|", VB_INVERT},
    {"$_XNOR_", FORM_GATE2, "^", VB_INVERT},
    {"$_MUX_", FORM_MUX, NULL, 0},
    {"$sop", FORM_SOP, NULL, 0},
    {"$_DFF_P_", FORM_LATCH, NULL, 0},
    {"$_DFF_N_", FORM_LATCH, NULL, VB_NEG},
    {"$_DLATCH_P_", FORM_LATCH, NULL, VB_LEVEL},
    {"$_DLATCH_N_", FORM_LATCH, NULL, VB_LEVEL | VB_NEG},
    {"$_FF_", FORM_LATCH, NULL, VB_GCLK},
    {"$add", FORM_BINARY, "+", 0},
    {"$sub", FORM_BINARY, "-", 0},
    {"$mul", FORM_BINARY, "*", 0},
    {"$div", FORM_BINARY, "/", 0},
    {"$mod", FORM_BINARY, "%", 0},
    {"$and", FORM_BINARY, "&", 0},
    {"$or", FORM_BINARY, "|", 0},
    {"$xor", FORM_BINARY, "^", 0},
    {"$shl", FORM_BINARY, "<<", VB_SHIFT},
    {"$shr", FORM_BINARY, ">>", VB_SHIFT},
    {"$sshr", FORM_BINARY, ">>>", VB_SHIFT},
    {"$eq", FORM_BINARY, "==", 0},
    {"$ne", FORM_BINARY, "!=", 0},
    {"$lt", FORM_BINARY, "<", 0},
    {"$le", FORM_BINARY, "<=", 0},
    {"$gt", FORM_BINARY, ">", 0},
    {"$ge", FORM_BINARY, ">=", 0},
    {"$not", FORM_UNARY, "~", 0},
    {"$reduce_and", FORM_UNARY, "&", 0},
    {"$reduce_or", FORM_UNARY, "|", 0},
    {"$reduce_xor", FORM_UNARY, "^", 0},
    {"$mux", FORM_MUX, NULL, 0},
    {"$pmux", FORM_PMUX, NULL, 0},
    {"$tribuf", FORM_TRIBUF, NULL, 0},
    {"$dff", FORM_FF, NULL, 0},
    {"$dffe", FORM_FF, NULL, VB_EN},
    {"$adff", FORM_FF, NULL, VB_ARST},
    {"$sdff", FORM_FF, NULL, VB_SRST},
};
enum { N_BUILTINS = sizeof BUILTINS / sizeof BUILTINS[0] };

/* Parameter indices: word cells (A_SIGNED B_SIGNED …), $sop, word flip-flops, bit storage. */
enum { PARAM_A_SIGNED = 0, PARAM_B_SIGNED = 1 };
enum { SOP_WIDTH = 0, SOP_COVER = 1 };
enum { FF_WIDTH = 0, FF_CLK_POLARITY = 1, FF_CTRL_POLARITY = 2, FF_RESET_VALUE = 3 };
enum { LATCH_INIT = 0 };
enum { PMUX_WIDTH = 0 };

/* --- writer state -------------------------------------------------------------------------- */

/* How a net is written: a wire bit, its own scalar declaration, or a literal. */
typedef enum vw_ref_kind { REF_NONE, REF_WIRE, REF_OWN, REF_CONST } vw_ref_kind;

typedef struct vw_ref {
    uint32_t wire; /* REF_WIRE: the wire */
    uint32_t bit;  /* REF_WIRE: the vector position; REF_CONST: the literal character */
    uint8_t kind;  /* vw_ref_kind */
    bool reg;      /* REF_OWN: declared `reg` (driven by a bit-level storage cell) */
} vw_ref;

/* Kinds of named things in a module's one Verilog namespace. */
typedef enum vw_obj { OBJ_WIRE = 1, OBJ_NET, OBJ_NODE, OBJ_PIN, OBJ_GCLK } vw_obj;

typedef struct vw_module {
    const odin3_module *module;
    const odin3_celltype_def *def; /* the module's cell type: port directions, scalar flags */
    const char *name;              /* for messages */
    odin3_strtab *names;           /* identifiers given out in this module */
    odin3_u64map *obj_names;       /* kind << 32 | ID -> names ID */
    odin3_u64map *vectors;         /* names ID of a vector wire -> 1 */
    vw_ref *refs;                  /* by net ID */
    uint32_t *wire_port;           /* by wire ID: port index + 1, 0 for a non-port wire */
} vw_module;

typedef struct verilog_writer {
    const odin3_design *design;
    const odin3_strtab *tab;
    const char *path;
    odin3_verilog_name_style name_style;
    odin3_atomic_file file; /* a temporary beside path, renamed over it on success */
    bool opened;            /* file is open: wr_finish commits or removes it */
    bool oom_logged;
    size_t escaped_end; /* out.len just after an escaped identifier, SIZE_MAX otherwise */
    odin3_status st;    /* sticky: the first failure */
    uint32_t line;      /* lines written to the file so far */
    uint32_t units;     /* modules and stubs written (a blank line separates them) */
    odin3_strbuf out;
    odin3_strbuf cand;       /* a candidate generated name */
    odin3_u64map *builtin;   /* cell type ID -> BUILTINS index */
    odin3_u64map *prov_leaf; /* prov ID -> the record that names it (IR §6), 0 for none */
    odin3_u64map *stubbed;   /* cell type ID -> 1 once its stub is written */
    odin3_wattr_seen seen;   /* attribute names written in the current (* … *) */
    vw_module mod;
} verilog_writer;

/* --- errors and output --------------------------------------------------------------------- */

static odin3_status vw_fail(verilog_writer *wr, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY && !wr->oom_logged) {
        wr->oom_logged = true;
        odin3_log(ODIN3_LOG_ERROR, "%s: out of memory", wr->path);
    }
    if (wr->st == ODIN3_OK) {
        wr->st = st;
    }
    return wr->st;
}

/* Logs "path: module '<module>': <what> '<name>'" and fails with ODIN3_ERR_INVALID_ARG. */
static odin3_status vw_refuse(verilog_writer *wr, const char *what, odin3_bytes name) {
    if (wr->st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s: module '%s': %s '%.*s'", wr->path,
                  wr->mod.name != NULL ? wr->mod.name : "", what, (int)name.len,
                  (const char *)name.ptr);
    }
    return vw_fail(wr, ODIN3_ERR_INVALID_ARG);
}

static void vw_flush(verilog_writer *wr) {
    if (wr->st != ODIN3_OK || wr->out.len == 0) {
        return;
    }
    errno = 0;
    if (fwrite(wr->out.data, 1, wr->out.len, wr->file.fp) != wr->out.len) {
        int saved = errno != 0 ? errno : EIO;
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: write failed: %s", wr->path, (unsigned)wr->line + 1,
                  strerror(saved));
        (void)vw_fail(wr, ODIN3_ERR_IO);
        return;
    }
    for (size_t i = 0; i < wr->out.len; i++) {
        wr->line += wr->out.data[i] == '\n';
    }
    odin3_strbuf_clear(&wr->out);
    wr->escaped_end = SIZE_MAX;
}

/* Appends text exactly (string literals, comments). */
static void vw_raw(verilog_writer *wr, odin3_bytes text) {
    if (wr->st == ODIN3_OK && text.len > 0 && odin3_strbuf_append(&wr->out, text) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

/*
 * Appends Verilog syntax. Right after an escaped identifier (which ends with the blank that
 * terminates it) a leading blank is dropped, so `\a ` then ` = b` gives `\a = b`; nowhere else.
 */
static void vw_bytes(verilog_writer *wr, odin3_bytes text) {
    const char *chars = text.ptr;
    if (text.len > 0 && chars[0] == ' ' && wr->out.len == wr->escaped_end) {
        text = (odin3_bytes){chars + 1, text.len - 1};
    }
    vw_raw(wr, text);
}

static void vw_puts(verilog_writer *wr, const char *text) {
    vw_bytes(wr, odin3_bytes_cstr(text));
}

static void vw_char(verilog_writer *wr, char chr) {
    vw_bytes(wr, (odin3_bytes){&chr, 1});
}

static void vw_u32(verilog_writer *wr, uint32_t num) {
    if (wr->st == ODIN3_OK && odin3_strbuf_appendf(&wr->out, "%u", (unsigned)num) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

static void vw_i64(verilog_writer *wr, int64_t num) {
    if (wr->st == ODIN3_OK && odin3_strbuf_appendf(&wr->out, "%lld", (long long)num) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

/* Appends name as a Verilog identifier; an unwritable name is refused as `what`. */
static void vw_ident(verilog_writer *wr, odin3_bytes name, const char *what) {
    if (wr->st != ODIN3_OK) {
        return;
    }
    odin3_status st = odin3_verilog_append_ident(&wr->out, name);
    if (st == ODIN3_OK && odin3_verilog_ident_kind(name) == ODIN3_VERILOG_ESCAPED) {
        wr->escaped_end = wr->out.len;
    }
    if (st == ODIN3_ERR_INVALID_ARG) {
        (void)vw_refuse(wr, what, name);
    } else if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    }
}

static odin3_bytes str_bytes(const verilog_writer *wr, uint32_t str) {
    return (odin3_bytes){odin3_strtab_get(wr->tab, str), odin3_strtab_len(wr->tab, str)};
}

/* --- provenance comments ------------------------------------------------------------------- */

typedef struct vw_leaf {
    odin3_prov_id first;
} vw_leaf;

static void first_leaf(void *user, odin3_prov_id leaf) {
    vw_leaf *found = user;
    if (!odin3_prov_valid(found->first)) {
        found->first = leaf;
    }
}

static bool printable(odin3_bytes text) {
    const uint8_t *bytes = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] < FIRST_PRINTABLE || bytes[i] > LAST_PRINTABLE) {
            return false;
        }
    }
    return true;
}

/* file << 32 | line of the record's first location, 0 when it has none worth printing. */
static uint64_t location_of(const verilog_writer *wr, const odin3_prov_record *rec) {
    if (rec == NULL || rec->n_locs == 0 || rec->locs[0].file == 0 ||
        !printable(str_bytes(wr, rec->locs[0].file))) {
        return 0;
    }
    return (uint64_t)rec->locs[0].file << KEY_SHIFT | rec->locs[0].line;
}

/* The record that names an object with provenance prov (IR §6): prov itself, or for a DERIVED
 * record the first leaf of the backward walk. Cached per record; NULL for none. */
static const odin3_prov_record *prov_leaf(verilog_writer *wr, odin3_prov_id prov) {
    uint64_t leaf = 0;
    if (prov.v == 0 || wr->st != ODIN3_OK) {
        return NULL;
    }
    if (!odin3_u64map_get(wr->prov_leaf, prov.v, &leaf)) {
        const odin3_prov_record *rec = odin3_prov_get(wr->design, prov);
        vw_leaf found = {rec != NULL ? prov : (odin3_prov_id){0}};
        if (rec != NULL && rec->kind == ODIN3_PROV_DERIVED) {
            found.first = (odin3_prov_id){0};
            odin3_status st = odin3_prov_sources(wr->design, prov, first_leaf, &found);
            if (st != ODIN3_OK) {
                (void)vw_fail(wr, st);
                return NULL;
            }
        }
        leaf = found.first.v;
        if (odin3_u64map_put(wr->prov_leaf, (odin3_kv){prov.v, leaf}) != ODIN3_OK) {
            (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
            return NULL;
        }
    }
    return odin3_prov_get(wr->design, (odin3_prov_id){(uint32_t)leaf});
}

/* Ends a line: `  // file:line` when prov has a location, then the newline. */
static void vw_eol(verilog_writer *wr, odin3_prov_id prov) {
    uint64_t loc = location_of(wr, prov_leaf(wr, prov));
    if (loc != 0) {
        vw_raw(wr, odin3_bytes_cstr("  // "));
        vw_raw(wr, str_bytes(wr, (uint32_t)(loc >> KEY_SHIFT)));
        vw_raw(wr, odin3_bytes_cstr(":"));
        vw_u32(wr, (uint32_t)loc);
    }
    vw_char(wr, '\n');
    if (wr->out.len >= FLUSH_AT) {
        vw_flush(wr);
    }
}

/* --- cell types ---------------------------------------------------------------------------- */

/* The built-in Verilog form of a cell type, NULL when it is written as an instance. */
static const vw_builtin *builtin_of(const verilog_writer *wr, odin3_celltype_id type) {
    uint64_t index = 0;
    return odin3_u64map_get(wr->builtin, type.v, &index) ? &BUILTINS[index] : NULL;
}

static const vw_builtin *node_builtin(const verilog_writer *wr, odin3_node_id node) {
    return builtin_of(wr, odin3_node_type(wr->mod.module, node));
}

static const odin3_celltype_def *node_def(const verilog_writer *wr, odin3_node_id node) {
    return odin3_celltype_get(wr->design, odin3_node_type(wr->mod.module, node));
}

static bool is_port_node(const verilog_writer *wr, odin3_node_id node) {
    return node_def(wr, node)->gran == ODIN3_GRAN_PORT;
}

/* Pins of the last port of a node (the output of every built-in with a Verilog form). */
static odin3_pinslice out_port(const verilog_writer *wr, odin3_node_id node) {
    return odin3_node_port(wr->mod.module, node, node_def(wr, node)->n_ports - 1);
}

static int64_t param_int(const verilog_writer *wr, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(wr->mod.module, node, index);
    return val != NULL && val->kind == ODIN3_VAL_INT ? val->i : 0;
}

/* True when some output or inout pin of node is connected. */
static bool has_output(const verilog_writer *wr, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(wr->mod.module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(wr->mod.module, pin) &&
            odin3_net_valid(odin3_pin_net(wr->mod.module, pin))) {
            return true;
        }
    }
    return false;
}

/* --- names --------------------------------------------------------------------------------- */

static uint64_t obj_key(vw_obj kind, uint32_t id) {
    return (uint64_t)kind << KEY_SHIFT | id;
}

/* The names ID given to an object, 0 for none. */
static uint32_t name_of(const verilog_writer *wr, vw_obj kind, uint32_t id) {
    uint64_t str = 0;
    return odin3_u64map_get(wr->mod.obj_names, obj_key(kind, id), &str) ? (uint32_t)str : 0;
}

/*
 * True when text reads as a bit or part select of a vector wire named so far (`a[0]` beside
 * `a[1:0]`): legal Verilog, but Yosys write_blif would give both the same BLIF name.
 */
static bool select_lookalike(const verilog_writer *wr, odin3_bytes text) {
    const char *chars = text.ptr;
    if (text.len < 2 || chars[text.len - 1] != ']') {
        return false;
    }
    size_t open = text.len - 1;
    while (open > 0 && chars[open] != '[') {
        open--;
    }
    uint32_t base = 0;
    return open > 0 && chars[open] == '[' &&
           odin3_strtab_find(wr->mod.names, (odin3_bytes){chars, open}, &base) &&
           odin3_u64map_get(wr->mod.vectors, base, NULL);
}

/* Gives an object without a name the identifier text when it is writable and free (not taken,
 * not a select lookalike); false otherwise. */
static bool claim(verilog_writer *wr, uint64_t key, odin3_bytes text) {
    if (wr->st != ODIN3_OK || odin3_verilog_ident_kind(text) == ODIN3_VERILOG_UNWRITABLE ||
        odin3_u64map_get(wr->mod.obj_names, key, NULL) ||
        odin3_strtab_find(wr->mod.names, text, NULL) || select_lookalike(wr, text)) {
        return false;
    }
    uint32_t str = 0;
    if (odin3_strtab_intern(wr->mod.names, text, &str) != ODIN3_OK ||
        odin3_u64map_put(wr->mod.obj_names, (odin3_kv){key, str}) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    return true;
}

/* A generated name for an object that has none: prefix<id> (prefix alone for id 0), then
 * prefix<id>$1, $2, … until one is free. */
static void generate(verilog_writer *wr, vw_obj kind, uint32_t id, const char *prefix) {
    uint64_t key = obj_key(kind, id);
    if (wr->st != ODIN3_OK || odin3_u64map_get(wr->mod.obj_names, key, NULL)) {
        return;
    }
    for (uint32_t k = 0; wr->st == ODIN3_OK; k++) {
        odin3_strbuf_clear(&wr->cand);
        odin3_status st = odin3_strbuf_append(&wr->cand, odin3_bytes_cstr(prefix));
        if (st == ODIN3_OK && id != 0) {
            st = odin3_strbuf_appendf(&wr->cand, "%u", (unsigned)id);
        }
        if (st == ODIN3_OK && k != 0) {
            st = odin3_strbuf_appendf(&wr->cand, "$%u", (unsigned)k);
        }
        if (st != ODIN3_OK) {
            (void)vw_fail(wr, st);
        } else if (claim(wr, key, (odin3_bytes){wr->cand.data, wr->cand.len})) {
            return;
        }
    }
}

/* Appends the identifier given to an object (nothing when it has none). */
static void vw_name(verilog_writer *wr, vw_obj kind, uint32_t id) {
    uint32_t str = name_of(wr, kind, id);
    if (str != 0) {
        vw_ident(wr,
                 (odin3_bytes){odin3_strtab_get(wr->mod.names, str),
                               odin3_strtab_len(wr->mod.names, str)},
                 "name");
    }
}

/* --- wires and net references -------------------------------------------------------------- */

/* Port index + 1 of a port wire, 0 for any other wire. */
static uint32_t wire_port(const verilog_writer *wr, odin3_wire_id wire) {
    return wire.v < odin3_module_wire_end(wr->mod.module) ? wr->mod.wire_port[wire.v] : 0;
}

/* Declared without a range: a scalar port, or a 1-bit unsigned [0:0] non-port wire. */
static bool wire_scalar(const verilog_writer *wr, odin3_wire_id wire) {
    const odin3_module *module = wr->mod.module;
    uint32_t port = wire_port(wr, wire);
    if (port != 0) {
        return wr->mod.def->ports[port - 1].scalar;
    }
    return odin3_wire_width(module, wire) == 1 && odin3_wire_msb(module, wire) == 0 &&
           odin3_wire_lsb(module, wire) == 0 && !odin3_wire_signed(module, wire);
}

/* The net's own name, else its first bare alias name, else 0. */
static uint32_t own_name(const odin3_module *module, odin3_net_id net) {
    uint32_t name = odin3_net_name(module, net);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (name == 0 && odin3_net_alias_next(module, net, &cursor, &alias)) {
        if (!odin3_wire_valid(alias.wb.wire)) {
            name = alias.name;
        }
    }
    return name;
}

/* The built-in form of the net's single driver, NULL when it has none or several. */
static const vw_builtin *single_driver(const verilog_writer *wr, odin3_net_id net) {
    const odin3_module *module = wr->mod.module;
    if (odin3_net_driver_count(module, net) != 1) {
        return NULL;
    }
    return node_builtin(wr, odin3_pin_node(module, odin3_net_driver(module, net)));
}

/* The reference of a net that is on no wire. */
static vw_ref own_ref(const verilog_writer *wr, odin3_net_id net) {
    const vw_builtin *driver = single_driver(wr, net);
    if (driver != NULL && driver->form == FORM_CONST && own_name(wr->mod.module, net) == 0) {
        return (vw_ref){0, (uint8_t)driver->op[0], REF_CONST, false};
    }
    return (vw_ref){0, 0, REF_OWN, driver != NULL && driver->form == FORM_LATCH};
}

enum { RANK_ALIAS = 1, RANK_PRIMARY = 2, RANK_DRIVING_PORT = 3 };

typedef struct vw_pick {
    vw_ref ref;
    uint32_t rank;
    uint32_t driving_ports; /* input/inout port bits holding the net */
} vw_pick;

/* Takes wire bit wb as the net's reference when it outranks the current pick. */
static void consider(const verilog_writer *wr, vw_pick *pick, odin3_wirebit wb, uint32_t rank) {
    if (!odin3_wire_valid(wb.wire) || !odin3_wire_live(wr->mod.module, wb.wire)) {
        return;
    }
    uint32_t port = wire_port(wr, wb.wire);
    if (port != 0 && wr->mod.def->ports[port - 1].dir != ODIN3_DIR_OUT) {
        pick->driving_ports++;
        rank = RANK_DRIVING_PORT;
    }
    if (rank > pick->rank) {
        pick->rank = rank;
        pick->ref = (vw_ref){wb.wire.v, wb.bit, REF_WIRE, false};
    }
}

/* Chooses how net is written: its input/inout port bit, primary wire bit, first alias wire bit,
 * else itself (or a literal). */
static void classify_net(verilog_writer *wr, odin3_net_id net) {
    const odin3_module *module = wr->mod.module;
    vw_pick pick = {{0}, 0, 0};
    consider(wr, &pick, odin3_net_primary(module, net), RANK_PRIMARY);
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(module, net, &cursor, &alias)) {
        consider(wr, &pick, alias.wb, RANK_ALIAS);
    }
    if (pick.driving_ports > 1) {
        (void)vw_refuse(wr, "a net is on two input/inout port bits, one of them",
                        str_bytes(wr, odin3_wire_name(module, (odin3_wire_id){pick.ref.wire})));
    }
    wr->mod.refs[net.v] = pick.rank != 0 ? pick.ref : own_ref(wr, net);
}

static vw_ref net_ref(const verilog_writer *wr, odin3_net_id net) {
    return wr->mod.refs[net.v];
}

/* True when the Q net of a bit-level storage cell is declared as its `reg`. */
static bool q_is_reg(const verilog_writer *wr, odin3_node_id node) {
    odin3_net_id net = odin3_pin_net(wr->mod.module, out_port(wr, node).first);
    return odin3_net_valid(net) && net_ref(wr, net).kind == REF_OWN && net_ref(wr, net).reg;
}

/* True when a node is written under its name: instances and storage cells with their own reg. */
static bool needs_name(const verilog_writer *wr, odin3_node_id node) {
    const vw_builtin *bi = node_builtin(wr, node);
    if (bi == NULL) {
        return true;
    }
    if (!has_output(wr, node)) {
        return false;
    }
    return bi->form == FORM_FF || (bi->form == FORM_LATCH && !q_is_reg(wr, node));
}

/* --- module setup: references and the namespace -------------------------------------------- */

/* Records a claimed vector wire's name for select_lookalike. */
static void note_vector(verilog_writer *wr, odin3_wire_id wire) {
    uint32_t str = name_of(wr, OBJ_WIRE, wire.v);
    if (str != 0 && !wire_scalar(wr, wire) &&
        odin3_u64map_put(wr->mod.vectors, (odin3_kv){str, 1}) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

/* Port names are the interface: each must be writable (they are unique among wires). */
static odin3_status claim_ports(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 0; wr->st == ODIN3_OK && i < odin3_module_port_count(module); i++) {
        odin3_wire_id wire = odin3_module_port_wire(module, i);
        wr->mod.wire_port[wire.v] = i + 1;
        odin3_bytes name = str_bytes(wr, odin3_wire_name(module, wire));
        if (!claim(wr, obj_key(OBJ_WIRE, wire.v), name)) {
            (void)vw_refuse(wr, "port name cannot be written in Verilog:", name);
        }
    }
    /* After every port is named, so ports never count as each other's select lookalikes. */
    for (uint32_t i = 0; wr->st == ODIN3_OK && i < odin3_module_port_count(module); i++) {
        note_vector(wr, odin3_module_port_wire(module, i));
    }
    return wr->st;
}

/* A wire, net or cell to name: its own name (0 = none), provenance and generated-name prefix. */
typedef struct vw_named {
    vw_obj kind;
    uint32_t id;
    uint32_t own;
    odin3_prov_id prov;
    const char *prefix;
} vw_named;

/* Appends the short name (own name, else prefix<id>) to wr->cand. */
static odin3_status append_short(verilog_writer *wr, const vw_named *obj) {
    if (obj->own != 0) {
        return odin3_strbuf_append(&wr->cand, str_bytes(wr, obj->own));
    }
    return odin3_strbuf_appendf(&wr->cand, "%s%u", obj->prefix, (unsigned)obj->id);
}

/* PROVENANCE style (IR §6): tries hier/<short name>@file:line from the record naming the object;
 * nothing when the record has neither a hierarchy nor a location. */
static void claim_provenance(verilog_writer *wr, const vw_named *obj) {
    const odin3_prov_record *rec = prov_leaf(wr, obj->prov);
    uint64_t loc = location_of(wr, rec);
    if (rec == NULL || (rec->hier == 0 && loc == 0)) {
        return;
    }
    odin3_strbuf_clear(&wr->cand);
    odin3_status st = ODIN3_OK;
    if (rec->hier != 0) {
        st = odin3_strbuf_appendf(&wr->cand, "%s/", odin3_strtab_get(wr->tab, rec->hier));
    }
    st = st == ODIN3_OK ? append_short(wr, obj) : st;
    if (st == ODIN3_OK && loc != 0) {
        st = odin3_strbuf_appendf(&wr->cand, "@%s:%u",
                                  odin3_strtab_get(wr->tab, (uint32_t)(loc >> KEY_SHIFT)),
                                  (unsigned)(uint32_t)loc);
    }
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
        return;
    }
    (void)claim(wr, obj_key(obj->kind, obj->id), (odin3_bytes){wr->cand.data, wr->cand.len});
}

/* The object's provenance name (PROVENANCE style), else its own name when writable and free. */
static void claim_object(verilog_writer *wr, vw_named obj) {
    if (wr->name_style == ODIN3_VERILOG_NAMES_PROVENANCE) {
        claim_provenance(wr, &obj);
    }
    if (obj.own != 0) {
        (void)claim(wr, obj_key(obj.kind, obj.id), str_bytes(wr, obj.own));
    }
}

/* Real names first (wires, nets, cells in ID order); generated names only for the rest. */
static void claim_names(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (odin3_wire_live(module, wire) && wire_port(wr, wire) == 0) {
            claim_object(wr, (vw_named){OBJ_WIRE, i, odin3_wire_name(module, wire),
                                        odin3_wire_prov(module, wire), "$w"});
            note_vector(wr, wire);
        }
    }
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(module, net) && wr->mod.refs[i].kind == REF_OWN) {
            claim_object(wr, (vw_named){OBJ_NET, i, own_name(module, net),
                                        odin3_net_prov(module, net), "$n"});
        }
    }
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) && !is_port_node(wr, node) && needs_name(wr, node)) {
            claim_object(wr, (vw_named){OBJ_NODE, i, odin3_node_name(module, node),
                                        odin3_node_prov(module, node), "$c"});
        }
    }
}

/* `$p<pin>` for each unconnected output/inout pin of a port that has a connected pin. */
static void name_dangling(verilog_writer *wr, odin3_node_id node) {
    const odin3_module *module = wr->mod.module;
    const odin3_celltype_def *def = node_def(wr, node);
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice pins = odin3_node_port(module, node, port);
        uint32_t open = 0;
        for (uint32_t i = 0; i < pins.count; i++) {
            open += !odin3_net_valid(odin3_pin_net(module, (odin3_pin_id){pins.first.v + i}));
        }
        if (def->ports[port].dir == ODIN3_DIR_IN || open == 0 || open == pins.count) {
            continue;
        }
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_pin_id pin = {pins.first.v + i};
            if (!odin3_net_valid(odin3_pin_net(module, pin))) {
                generate(wr, OBJ_PIN, pin.v, "$p");
            }
        }
    }
}

/* A cell's own name when it needs one, its dangling pins, and the global clock for a $_FF_. */
static void generate_cell_names(verilog_writer *wr, odin3_node_id node) {
    if (needs_name(wr, node)) {
        generate(wr, OBJ_NODE, node.v, "$c");
    }
    name_dangling(wr, node);
    const vw_builtin *bi = node_builtin(wr, node);
    if (bi != NULL && (bi->flags & VB_GCLK) != 0 && has_output(wr, node)) {
        generate(wr, OBJ_GCLK, 0, "$gclk");
    }
}

static void generate_names(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        if (odin3_wire_live(module, (odin3_wire_id){i}) && wire_port(wr, (odin3_wire_id){i}) == 0) {
            generate(wr, OBJ_WIRE, i, "$w");
        }
    }
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        if (odin3_net_live(module, (odin3_net_id){i}) && wr->mod.refs[i].kind == REF_OWN) {
            generate(wr, OBJ_NET, i, "$n");
        }
    }
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node) && !is_port_node(wr, node)) {
            generate_cell_names(wr, node);
        }
    }
}

static void end_module(verilog_writer *wr) {
    odin3_strtab_destroy(wr->mod.names);
    odin3_u64map_destroy(wr->mod.obj_names);
    odin3_u64map_destroy(wr->mod.vectors);
    odin3_util_free(wr->mod.refs);
    odin3_util_free(wr->mod.wire_port);
    wr->mod = (vw_module){0};
}

static odin3_status begin_module(verilog_writer *wr, const odin3_module *module) {
    vw_module *mod = &wr->mod;
    *mod = (vw_module){.module = module,
                       .def = odin3_celltype_get(wr->design, odin3_module_celltype(module)),
                       .name = odin3_strtab_get(wr->tab, odin3_module_name(module))};
    mod->names = odin3_strtab_create();
    mod->obj_names = odin3_u64map_create(0);
    mod->vectors = odin3_u64map_create(0);
    mod->refs = odin3_util_calloc(sizeof(vw_ref) * odin3_module_net_end(module));
    mod->wire_port = odin3_util_calloc(sizeof(uint32_t) * odin3_module_wire_end(module));
    if (mod->names == NULL || mod->obj_names == NULL || mod->vectors == NULL || mod->refs == NULL ||
        mod->wire_port == NULL) {
        return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    if (claim_ports(wr) != ODIN3_OK) {
        return wr->st;
    }
    for (uint32_t i = 1; wr->st == ODIN3_OK && i < odin3_module_net_end(module); i++) {
        if (odin3_net_live(module, (odin3_net_id){i})) {
            classify_net(wr, (odin3_net_id){i});
        }
    }
    if (wr->st == ODIN3_OK) {
        claim_names(wr);
        generate_names(wr);
    }
    return wr->st;
}

/* --- vectors: items, runs and concatenations ----------------------------------------------- */

typedef enum vw_item_kind { ITEM_WIRE, ITEM_NAME, ITEM_LIT } vw_item_kind;

/* One bit as written: a wire bit (id = wire, bit = position), a scalar name (id = names ID) or a
 * literal (bit = its character). */
typedef struct vw_item {
    vw_item_kind kind;
    uint32_t id;
    uint32_t bit;
} vw_item;

/* A bit vector, MSB first: count pins from first (pin source), or wire bits hi down (wire
 * source, when wire is set). */
typedef struct vw_items {
    odin3_pin_id first;
    odin3_wire_id wire;
    uint32_t hi;
    uint32_t count;
} vw_items;

static vw_items pin_items(odin3_pinslice pins) {
    return (vw_items){pins.first, {0}, 0, pins.count};
}

static vw_items one_pin(odin3_pin_id pin) {
    return (vw_items){pin, {0}, 0, 1};
}

static vw_items wire_items(odin3_wire_id wire, uint32_t hi, uint32_t lo) {
    return (vw_items){{0}, wire, hi, hi - lo + 1};
}

static vw_item net_item(const verilog_writer *wr, odin3_net_id net) {
    vw_ref ref = net_ref(wr, net);
    switch ((vw_ref_kind)ref.kind) {
    case REF_WIRE:
        return (vw_item){ITEM_WIRE, ref.wire, ref.bit};
    case REF_CONST:
        return (vw_item){ITEM_LIT, 0, ref.bit};
    case REF_OWN:
        return (vw_item){ITEM_NAME, name_of(wr, OBJ_NET, net.v), 0};
    case REF_NONE:
    default:
        return (vw_item){ITEM_LIT, 0, 'x'};
    }
}

/* Item pos (0 = MSB). An unconnected input bit is x; an unconnected output bit its dangling
 * wire. */
static vw_item item_at(const verilog_writer *wr, const vw_items *items, uint32_t pos) {
    const odin3_module *module = wr->mod.module;
    if (odin3_wire_valid(items->wire)) {
        return net_item(wr, odin3_wire_net(module, items->wire, items->hi - pos));
    }
    odin3_pin_id pin = {items->first.v + items->count - 1 - pos};
    odin3_net_id net = odin3_pin_net(module, pin);
    if (odin3_net_valid(net)) {
        return net_item(wr, net);
    }
    uint32_t dangling = name_of(wr, OBJ_PIN, pin.v);
    return dangling != 0 ? (vw_item){ITEM_NAME, dangling, 0} : (vw_item){ITEM_LIT, 0, 'x'};
}

/* A run of items written as one element: start index, length and first item. */
typedef struct vw_elem {
    vw_item first;
    uint32_t start;
    uint32_t len;
} vw_elem;

static bool continues(const vw_elem *elem, vw_item next) {
    if (next.kind != elem->first.kind) {
        return false;
    }
    if (next.kind == ITEM_LIT) {
        return true;
    }
    return next.kind == ITEM_WIRE && next.id == elem->first.id &&
           (uint64_t)next.bit + elem->len == elem->first.bit;
}

/* The element starting at *pos; advances *pos past it. */
static vw_elem next_elem(const verilog_writer *wr, const vw_items *items, uint32_t *pos) {
    vw_elem elem = {item_at(wr, items, *pos), *pos, 1};
    for (*pos += 1; *pos < items->count && continues(&elem, item_at(wr, items, *pos)); *pos += 1) {
        elem.len++;
    }
    return elem;
}

/* Bits hi..lo of a wire: the bare name for a scalar or a whole unsigned wire, else a select. */
static void put_wire_bits(verilog_writer *wr, odin3_wire_id wire, uint32_t hi, uint32_t lo) {
    const odin3_module *module = wr->mod.module;
    vw_name(wr, OBJ_WIRE, wire.v);
    if (wire_scalar(wr, wire) ||
        (hi == odin3_wire_width(module, wire) - 1 && lo == 0 && !odin3_wire_signed(module, wire))) {
        return;
    }
    vw_char(wr, '[');
    vw_i64(wr, odin3_wire_index(module, wire, hi));
    if (hi != lo) {
        vw_char(wr, ':');
        vw_i64(wr, odin3_wire_index(module, wire, lo));
    }
    vw_char(wr, ']');
}

static void put_elem(verilog_writer *wr, const vw_items *items, const vw_elem *elem) {
    if (elem->first.kind == ITEM_WIRE) {
        put_wire_bits(wr, (odin3_wire_id){elem->first.id}, elem->first.bit,
                      elem->first.bit + 1 - elem->len);
    } else if (elem->first.kind == ITEM_NAME) {
        vw_ident(wr,
                 (odin3_bytes){odin3_strtab_get(wr->mod.names, elem->first.id),
                               odin3_strtab_len(wr->mod.names, elem->first.id)},
                 "name");
    } else {
        vw_u32(wr, elem->len);
        vw_puts(wr, "'b");
        for (uint32_t i = 0; i < elem->len; i++) {
            vw_char(wr, (char)item_at(wr, items, elem->start + i).bit);
        }
    }
}

/* Writes items as one expression: `{e1, e2, …}` for several elements, wrapped in `$signed(…)`
 * when is_signed. */
static void put_items(verilog_writer *wr, const vw_items *items, bool is_signed) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < items->count; count++) {
        (void)next_elem(wr, items, &k);
    }
    vw_puts(wr, is_signed ? "$signed(" : "");
    vw_puts(wr, count > 1 ? "{" : "");
    uint32_t k = 0;
    for (uint32_t i = 0; i < count; i++) {
        vw_elem elem = next_elem(wr, items, &k);
        vw_puts(wr, i > 0 ? ", " : "");
        put_elem(wr, items, &elem);
    }
    vw_puts(wr, count > 1 ? "}" : "");
    vw_puts(wr, is_signed ? ")" : "");
}

static void put_pin(verilog_writer *wr, odin3_pin_id pin) {
    vw_items items = one_pin(pin);
    put_items(wr, &items, false);
}

static void put_port(verilog_writer *wr, odin3_node_id node, uint32_t port, bool is_signed) {
    vw_items items = pin_items(odin3_node_port(wr->mod.module, node, port));
    put_items(wr, &items, is_signed);
}

/* Bits as a sized binary literal, MSB first. */
static void put_bits(verilog_writer *wr, const uint8_t *bits, uint32_t len) {
    static const char DIGITS[] = "01xz";
    if (len == 0) {
        vw_puts(wr, "\"\"");
        return;
    }
    vw_u32(wr, len);
    vw_puts(wr, "'b");
    for (uint32_t i = len; i > 0; i--) {
        vw_char(wr, DIGITS[bits[i - 1] & 3U]);
    }
}

/* --- built-in cells ------------------------------------------------------------------------ */

/* `  assign <output port> = ` */
static void begin_assign(verilog_writer *wr, odin3_node_id node) {
    vw_puts(wr, "  assign ");
    put_port(wr, node, node_def(wr, node)->n_ports - 1, false);
    vw_puts(wr, " = ");
}

static void end_stmt(verilog_writer *wr, odin3_node_id node) {
    vw_char(wr, ';');
    vw_eol(wr, odin3_node_prov(wr->mod.module, node));
}

static void write_const(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    odin3_net_id net = odin3_pin_net(wr->mod.module, out_port(wr, node).first);
    if (net_ref(wr, net).kind == REF_CONST) {
        return; /* written as the literal wherever the net is read */
    }
    begin_assign(wr, node);
    vw_puts(wr, "1'b");
    vw_puts(wr, bi->op);
    end_stmt(wr, node);
}

static void write_gate1(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    begin_assign(wr, node);
    vw_puts(wr, bi->op);
    put_port(wr, node, 0, false);
    end_stmt(wr, node);
}

static void write_gate2(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    bool invert = (bi->flags & VB_INVERT) != 0;
    begin_assign(wr, node);
    vw_puts(wr, invert ? "~(" : "");
    put_port(wr, node, 0, false);
    vw_char(wr, ' ');
    vw_puts(wr, bi->op);
    vw_char(wr, ' ');
    put_port(wr, node, 1, false);
    vw_puts(wr, invert ? ")" : "");
    end_stmt(wr, node);
}

/* $_MUX_ and $mux: ports A B S Y. */
static void write_mux(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    begin_assign(wr, node);
    put_port(wr, node, 2, false);
    vw_puts(wr, " ? ");
    put_port(wr, node, 1, false);
    vw_puts(wr, " : ");
    put_port(wr, node, 0, false);
    end_stmt(wr, node);
}

/* Word binary operators with Yosys simlib signedness: A' and B' are $signed when flagged (a
 * shift amount never is). */
static void write_binary(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    bool a_signed = param_int(wr, node, PARAM_A_SIGNED) != 0;
    bool b_signed = (bi->flags & VB_SHIFT) == 0 && param_int(wr, node, PARAM_B_SIGNED) != 0;
    begin_assign(wr, node);
    put_port(wr, node, 0, a_signed);
    vw_char(wr, ' ');
    vw_puts(wr, bi->op);
    vw_char(wr, ' ');
    put_port(wr, node, 1, b_signed);
    end_stmt(wr, node);
}

static void write_unary(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    begin_assign(wr, node);
    vw_puts(wr, bi->op);
    put_port(wr, node, 0, param_int(wr, node, PARAM_A_SIGNED) != 0);
    end_stmt(wr, node);
}

/*
 * $pmux (A B S Y): |S ? (({W{S[0]}} & B[0]) | ({W{S[1]}} & B[1]) | …) : A. With several select
 * bits set the selected slices are ORed (Yosys gate-level semantics, as the 1E simulator does).
 */
static void write_pmux(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    const odin3_module *module = wr->mod.module;
    uint32_t width = (uint32_t)param_int(wr, node, PMUX_WIDTH);
    odin3_pinslice b_pins = odin3_node_port(module, node, 1);
    odin3_pinslice s_pins = odin3_node_port(module, node, 2);
    begin_assign(wr, node);
    vw_char(wr, '|');
    put_port(wr, node, 2, false);
    vw_puts(wr, " ? (");
    for (uint32_t i = 0; i < s_pins.count; i++) {
        vw_puts(wr, i > 0 ? " | ({" : "({");
        vw_u32(wr, width);
        vw_char(wr, '{');
        put_pin(wr, (odin3_pin_id){s_pins.first.v + i});
        vw_puts(wr, "}} & ");
        vw_items slice = pin_items((odin3_pinslice){{b_pins.first.v + i * width}, width});
        put_items(wr, &slice, false);
        vw_char(wr, ')');
    }
    vw_puts(wr, ") : ");
    put_port(wr, node, 0, false);
    end_stmt(wr, node);
}

/* $tribuf (A EN Y): EN ? A : <W>'bz…z. */
static void write_tribuf(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    uint32_t width = out_port(wr, node).count;
    begin_assign(wr, node);
    put_port(wr, node, 1, false);
    vw_puts(wr, " ? ");
    put_port(wr, node, 0, false);
    vw_puts(wr, " : ");
    vw_u32(wr, width);
    vw_puts(wr, "'b");
    for (uint32_t i = 0; i < width; i++) {
        vw_char(wr, 'z');
    }
    end_stmt(wr, node);
}

/* --- $sop ---------------------------------------------------------------------------------- */

/* One product: the literals of a row joined by &, `1'b1` for a row of `-` only. */
static void write_product(verilog_writer *wr, const uint8_t *row, odin3_pinslice ins, bool paren) {
    uint32_t lits = 0;
    for (uint32_t i = 0; i < ins.count; i++) {
        lits += row[i] != '-';
    }
    if (lits == 0) {
        vw_puts(wr, "1'b1");
        return;
    }
    vw_puts(wr, paren && lits > 1 ? "(" : "");
    uint32_t done = 0;
    for (uint32_t i = 0; i < ins.count; i++) {
        if (row[i] == '-') {
            continue;
        }
        vw_puts(wr, done++ > 0 ? " & " : "");
        vw_puts(wr, row[i] == '0' ? "~" : "");
        put_pin(wr, (odin3_pin_id){ins.first.v + i});
    }
    vw_puts(wr, paren && lits > 1 ? ")" : "");
}

/* The products of every row joined by |. */
static void write_sum(verilog_writer *wr, const odin3_value *cover, odin3_pinslice ins) {
    uint32_t row_len = ins.count + 1;
    uint32_t rows = cover->len / row_len;
    for (uint32_t i = 0; i < rows; i++) {
        vw_puts(wr, i > 0 ? " | " : "");
        write_product(wr, cover->bits + (size_t)i * row_len, ins, rows > 1);
    }
}

/* The output value shared by every row ('1' ON-set, '0' OFF-set), 0 for none, -1 if mixed. */
static int cover_output(const odin3_value *cover, uint32_t row_len) {
    int out = 0;
    for (uint32_t off = 0; off + row_len <= cover->len; off += row_len) {
        int value = cover->bits[off + row_len - 1];
        if (out != 0 && value != out) {
            return -1;
        }
        out = value;
    }
    return out;
}

static void write_sop(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    const odin3_value *cover = odin3_node_param(wr->mod.module, node, SOP_COVER);
    odin3_pinslice ins = odin3_node_port(wr->mod.module, node, 0);
    uint32_t row_len = ins.count + 1;
    uint32_t rows = cover->len / row_len;
    int out = cover_output(cover, row_len);
    if (out < 0) {
        char id[NODE_ID_TEXT];
        uint32_t name = odin3_node_name(wr->mod.module, node);
        (void)snprintf(id, sizeof id, "$c%u", (unsigned)node.v);
        (void)vw_refuse(wr, "$sop cover mixes ON-set and OFF-set rows in cell",
                        name != 0 ? str_bytes(wr, name) : odin3_bytes_cstr(id));
        return;
    }
    begin_assign(wr, node);
    if (rows == 0 || ins.count == 0) {
        vw_puts(wr, rows > 0 && out == '1' ? "1'b1" : "1'b0");
    } else {
        vw_puts(wr, out == '0' ? "~(" : "");
        write_sum(wr, cover, ins);
        vw_puts(wr, out == '0' ? ")" : "");
    }
    end_stmt(wr, node);
}

/* --- storage cells ------------------------------------------------------------------------- */

/* The reg a storage cell assigns: its Q net when declared `reg`, else the cell's own reg. */
static void put_target(verilog_writer *wr, odin3_node_id node, bool q_reg) {
    if (q_reg) {
        put_pin(wr, out_port(wr, node).first);
    } else {
        vw_name(wr, OBJ_NODE, node.v);
    }
}

/* `  assign <Q> = <cell reg>;` when the cell has its own reg. */
static void connect_reg(verilog_writer *wr, odin3_node_id node, bool q_reg) {
    if (q_reg) {
        return;
    }
    vw_puts(wr, "  assign ");
    put_port(wr, node, node_def(wr, node)->n_ports - 1, false);
    vw_puts(wr, " = ");
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, ";\n");
}

/* $_DFF_P_/N_ (C D Q), $_DLATCH_P_/N_ (E D Q), $_FF_ (D Q); INIT 0/1 -> initial, 2/3 -> none. */
static void write_latch(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    const odin3_pinslice pins = odin3_node_pins(wr->mod.module, node);
    bool q_reg = q_is_reg(wr, node);
    bool negative = (bi->flags & VB_NEG) != 0;
    vw_puts(wr, "  always @");
    if ((bi->flags & VB_GCLK) != 0) {
        vw_puts(wr, "(posedge ");
        vw_name(wr, OBJ_GCLK, 0);
        vw_puts(wr, ") ");
    } else if ((bi->flags & VB_LEVEL) != 0) {
        vw_puts(wr, negative ? "* if (!" : "* if (");
        put_pin(wr, pins.first);
        vw_puts(wr, ") ");
    } else {
        vw_puts(wr, negative ? "(negedge " : "(posedge ");
        put_pin(wr, pins.first);
        vw_puts(wr, ") ");
    }
    put_target(wr, node, q_reg);
    vw_puts(wr, " <= ");
    put_pin(wr, (odin3_pin_id){pins.first.v + pins.count - 2});
    end_stmt(wr, node);
    int64_t init = param_int(wr, node, LATCH_INIT);
    if (init == 0 || init == 1) {
        vw_puts(wr, "  initial ");
        put_target(wr, node, q_reg);
        vw_puts(wr, init == 0 ? " = 1'b0;\n" : " = 1'b1;\n");
    }
    connect_reg(wr, node, q_reg);
}

static void put_edge(verilog_writer *wr, bool positive, odin3_pin_id pin) {
    vw_puts(wr, positive ? "posedge " : "negedge ");
    put_pin(wr, pin);
}

/* `if (<ctrl>) ` or `if (!<ctrl>) ` for port 1 of a word flip-flop. */
static void put_condition(verilog_writer *wr, odin3_node_id node) {
    bool active_high = param_int(wr, node, FF_CTRL_POLARITY) != 0;
    vw_puts(wr, active_high ? "if (" : "if (!");
    put_port(wr, node, 1, false);
    vw_puts(wr, ") ");
}

/* $dff (CLK D Q), $dffe (CLK EN D Q), $adff (CLK ARST D Q), $sdff (CLK SRST D Q). */
static void write_ff(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    uint32_t data = node_def(wr, node)->n_ports - 2;
    odin3_pin_id clk = odin3_node_pins(wr->mod.module, node).first;
    vw_puts(wr, "  always @(");
    put_edge(wr, param_int(wr, node, FF_CLK_POLARITY) != 0, clk);
    if ((bi->flags & VB_ARST) != 0) {
        vw_puts(wr, ", ");
        put_edge(wr, param_int(wr, node, FF_CTRL_POLARITY) != 0, (odin3_pin_id){clk.v + 1});
    }
    vw_puts(wr, ") ");
    if ((bi->flags & (VB_EN | VB_ARST | VB_SRST)) != 0) {
        put_condition(wr, node);
    }
    if ((bi->flags & (VB_ARST | VB_SRST)) != 0) {
        const odin3_value *reset = odin3_node_param(wr->mod.module, node, FF_RESET_VALUE);
        vw_name(wr, OBJ_NODE, node.v);
        vw_puts(wr, " <= ");
        put_bits(wr, reset->bits, reset->len);
        vw_puts(wr, "; else ");
    }
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, " <= ");
    put_port(wr, node, data, false);
    end_stmt(wr, node);
    connect_reg(wr, node, false);
}

typedef void (*vw_cell_writer)(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi);

static const vw_cell_writer CELL_WRITERS[FORM_COUNT] = {
    [FORM_CONST] = write_const,   [FORM_GATE1] = write_gate1, [FORM_GATE2] = write_gate2,
    [FORM_MUX] = write_mux,       [FORM_SOP] = write_sop,     [FORM_LATCH] = write_latch,
    [FORM_BINARY] = write_binary, [FORM_UNARY] = write_unary, [FORM_PMUX] = write_pmux,
    [FORM_TRIBUF] = write_tribuf, [FORM_FF] = write_ff,
};

/* --- parameter values ---------------------------------------------------------------------- */

/* One character of a string literal, escaped where Verilog needs it. */
static void put_string_char(verilog_writer *wr, uint8_t chr) {
    if (chr == '\\' || chr == '"') {
        vw_char(wr, '\\');
        vw_char(wr, (char)chr);
    } else if (chr == '\n') {
        vw_puts(wr, "\\n");
    } else if (chr == '\t') {
        vw_puts(wr, "\\t");
    } else if (chr >= FIRST_PRINTABLE && chr <= LAST_PRINTABLE) {
        vw_char(wr, (char)chr);
    } else if (wr->st == ODIN3_OK &&
               odin3_strbuf_appendf(&wr->out, "\\%0*o", OCTAL_DIGITS, (unsigned)chr) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

/* A string literal; a cover is its rows separated by blanks. */
static void put_string(verilog_writer *wr, odin3_bytes text, uint32_t row_len) {
    const uint8_t *bytes = text.ptr;
    vw_char(wr, '"');
    for (size_t i = 0; i < text.len; i++) {
        if (row_len != 0 && i > 0 && i % row_len == 0) {
            vw_char(wr, ' ');
        }
        put_string_char(wr, bytes[i]);
    }
    vw_char(wr, '"');
}

/* An INT as a decimal (beyond 32 bits as a sized signed decimal, so tools keep every bit). */
static void put_int(verilog_writer *wr, int64_t num) {
    if (num >= INT32_MIN && num <= INT32_MAX) {
        vw_i64(wr, num);
        return;
    }
    uint64_t mag = num < 0 ? (uint64_t)(-(num + 1)) + 1 : (uint64_t)num;
    if (wr->st == ODIN3_OK && odin3_strbuf_appendf(&wr->out, "%s64'sd%llu", num < 0 ? "-" : "",
                                                   (unsigned long long)mag) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
}

static void put_value(verilog_writer *wr, const odin3_value *val) {
    switch (val->kind) {
    case ODIN3_VAL_INT:
        put_int(wr, val->i);
        break;
    case ODIN3_VAL_BITS:
        put_bits(wr, val->bits, val->len);
        break;
    case ODIN3_VAL_STRING:
        put_string(wr, str_bytes(wr, val->str), 0);
        break;
    case ODIN3_VAL_COVER:
    default:
        put_string(wr, (odin3_bytes){val->bits, val->len}, val->cover_inputs + 1);
        break;
    }
}

/* --- attributes ---------------------------------------------------------------------------- */

static void put_attr_value(verilog_writer *wr, const odin3_wattr *attr) {
    const char *digits = attr->text.ptr;
    if (attr->form == ODIN3_WATTR_VALUE) {
        put_value(wr, attr->value);
    } else if (attr->form == ODIN3_WATTR_TEXT || attr->text.len == 0) {
        put_string(wr, attr->text, 0);
    } else {
        vw_u32(wr, (uint32_t)attr->text.len);
        vw_puts(wr, "'b");
        for (size_t i = 0; i < attr->text.len; i++) {
            vw_char(wr, digits[i] == '0' ? '0' : '1');
        }
    }
}

/* What surrounds `(* … *)`: lead before it, after behind it. */
typedef struct vw_attr_frame {
    const char *lead;
    const char *after;
} vw_attr_frame;

typedef struct vw_attrs {
    verilog_writer *wr;
    const char *lead; /* written before `(* ` */
    bool any;
} vw_attrs;

/* One `name = value`; BLIF `.param` extras are attributes too (Verilog stubs declare only the
 * type's parameters); a later attribute of the same name, or an unwritable name, is skipped. */
static odin3_status attr_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    vw_attrs *va = ctx;
    verilog_writer *wr = va->wr;
    odin3_wattr attr = odin3_wattr_classify(wr->tab, key_str, value);
    bool fresh = false;
    if (attr.role == ODIN3_WATTR_SKIP || wr->st != ODIN3_OK) {
        return wr->st;
    }
    if (odin3_verilog_ident_kind(attr.key) == ODIN3_VERILOG_UNWRITABLE) {
        odin3_log(ODIN3_LOG_WARN, "%s: module '%s': attribute '%.*s' cannot be written; skipped",
                  wr->path, wr->mod.name != NULL ? wr->mod.name : "", (int)attr.key.len,
                  (const char *)attr.key.ptr);
        return ODIN3_OK;
    }
    odin3_wattr_claim_req req = {ODIN3_WATTR_ATTRIBUTE, attr.key};
    odin3_status st = odin3_wattr_claim(&wr->seen, req, &fresh);
    if (st != ODIN3_OK || !fresh) {
        return st != ODIN3_OK ? vw_fail(wr, st) : ODIN3_OK;
    }
    vw_puts(wr, va->any ? ", " : va->lead);
    vw_puts(wr, va->any ? "" : "(* ");
    va->any = true;
    vw_ident(wr, attr.key, "attribute name");
    vw_puts(wr, " = ");
    put_attr_value(wr, &attr);
    return wr->st;
}

/* `lead(* a = v, … *)after` when obj has attributes (odin3_attr_foreach order), else nothing. */
static void put_attrs(verilog_writer *wr, odin3_objref obj, vw_attr_frame frame) {
    vw_attrs ctx = {wr, frame.lead, false};
    odin3_wattr_seen_clear(&wr->seen);
    odin3_status st = odin3_attr_foreach(wr->mod.module, obj, attr_visit, &ctx);
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    }
    if (ctx.any) {
        vw_puts(wr, " *)");
        vw_puts(wr, frame.after);
    }
}

/* --- instances ----------------------------------------------------------------------------- */

static bool port_open(const verilog_writer *wr, odin3_pinslice pins) {
    for (uint32_t i = 0; i < pins.count; i++) {
        if (odin3_net_valid(odin3_pin_net(wr->mod.module, (odin3_pin_id){pins.first.v + i}))) {
            return false;
        }
    }
    return true;
}

/* ` #(.P(v), …)` with every parameter, for any type but a module. */
static void put_overrides(verilog_writer *wr, odin3_node_id node, const odin3_celltype_def *def) {
    if (def->gran == ODIN3_GRAN_MODULE || def->n_params == 0) {
        return;
    }
    vw_puts(wr, " #(");
    for (uint32_t i = 0; i < def->n_params; i++) {
        vw_puts(wr, i > 0 ? ", ." : ".");
        vw_ident(wr, odin3_bytes_cstr(def->params[i].name), "parameter name");
        vw_char(wr, '(');
        put_value(wr, odin3_node_param(wr->mod.module, node, i));
        vw_char(wr, ')');
    }
    vw_char(wr, ')');
}

/* `  type #(…) name (` then `.port(expr)` per port in port order, `.port()` when unconnected. */
static void write_instance(verilog_writer *wr, odin3_node_id node) {
    const odin3_celltype_def *def = node_def(wr, node);
    vw_puts(wr, "  ");
    vw_ident(wr, odin3_bytes_cstr(def->name), "cell type name");
    put_overrides(wr, node, def);
    vw_char(wr, ' ');
    vw_name(wr, OBJ_NODE, node.v);
    if (def->n_ports == 0) {
        vw_puts(wr, " ()");
        end_stmt(wr, node);
        return;
    }
    vw_puts(wr, " (");
    vw_eol(wr, odin3_node_prov(wr->mod.module, node));
    for (uint32_t port = 0; port < def->n_ports; port++) {
        odin3_pinslice pins = odin3_node_port(wr->mod.module, node, port);
        vw_puts(wr, "    .");
        vw_ident(wr, odin3_bytes_cstr(def->ports[port].name), "port name");
        vw_char(wr, '(');
        if (!port_open(wr, pins)) {
            vw_items items = pin_items(pins);
            put_items(wr, &items, false);
        }
        vw_puts(wr, port + 1 < def->n_ports ? "),\n" : ")\n");
    }
    vw_puts(wr, "  );\n");
}

/* A constant cell written as a literal wherever its net is read (no statement of its own). */
static bool const_omitted(const verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    return bi->form == FORM_CONST &&
           net_ref(wr, odin3_pin_net(wr->mod.module, out_port(wr, node).first)).kind == REF_CONST;
}

/* The cell's attributes on a line of their own (a comment before an `assign`), then its
 * statement(s). */
static void write_cell(verilog_writer *wr, odin3_node_id node) {
    const vw_builtin *bi = node_builtin(wr, node);
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    if (bi == NULL) {
        put_attrs(wr, obj, (vw_attr_frame){"  ", "\n"});
        write_instance(wr, node);
    } else if (has_output(wr, node)) {
        if (!const_omitted(wr, node, bi)) {
            /* Icarus rejects attributes on a continuous assign: there they are a comment. */
            bool always = bi->form == FORM_LATCH || bi->form == FORM_FF;
            put_attrs(wr, obj, (vw_attr_frame){always ? "  " : "  // ", "\n"});
        }
        CELL_WRITERS[bi->form](wr, node, bi);
    }
}

/* --- modules ------------------------------------------------------------------------------- */

static const char *dir_word(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "input ";
    }
    return dir == ODIN3_DIR_OUT ? "output " : "inout ";
}

/* `signed `, then `[msb:lsb] ` unless the wire is scalar. */
static void put_wire_range(verilog_writer *wr, odin3_wire_id wire) {
    const odin3_module *module = wr->mod.module;
    vw_puts(wr, odin3_wire_signed(module, wire) ? "signed " : "");
    if (!wire_scalar(wr, wire)) {
        vw_char(wr, '[');
        vw_i64(wr, odin3_wire_msb(module, wire));
        vw_char(wr, ':');
        vw_i64(wr, odin3_wire_lsb(module, wire));
        vw_puts(wr, "] ");
    }
}

/* Separates modules and stubs by a blank line. */
static void begin_unit(verilog_writer *wr) {
    vw_puts(wr, wr->units++ > 0 ? "\n" : "");
}

static void write_header(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    uint32_t ports = odin3_module_port_count(module);
    begin_unit(wr);
    put_attrs(wr, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(module).v},
              (vw_attr_frame){"", "\n"});
    vw_puts(wr, "module ");
    vw_ident(wr, odin3_bytes_cstr(wr->mod.name), "module name cannot be written in Verilog:");
    vw_puts(wr, ports == 0 ? ";" : " (");
    vw_eol(wr, odin3_module_prov(module));
    for (uint32_t i = 0; i < ports; i++) {
        odin3_wire_id wire = odin3_module_port_wire(module, i);
        vw_puts(wr, "  ");
        put_attrs(wr, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, (vw_attr_frame){"", " "});
        vw_puts(wr, dir_word(wr->mod.def->ports[i].dir));
        put_wire_range(wr, wire);
        vw_name(wr, OBJ_WIRE, wire.v);
        vw_puts(wr, i + 1 < ports ? "," : "");
        vw_eol(wr, odin3_wire_prov(module, wire));
    }
    vw_puts(wr, ports == 0 ? "" : ");\n");
}

/* Non-port wires, then nets declared by themselves (`reg` for a storage cell's Q). */
static void declare_wires_and_nets(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (odin3_wire_live(module, wire) && wire_port(wr, wire) == 0) {
            vw_puts(wr, "  ");
            put_attrs(wr, (odin3_objref){ODIN3_OBJ_WIRE, i}, (vw_attr_frame){"", " "});
            vw_puts(wr, "wire ");
            put_wire_range(wr, wire);
            vw_name(wr, OBJ_WIRE, i);
            vw_char(wr, ';');
            vw_eol(wr, odin3_wire_prov(module, wire));
        }
    }
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(module, net) && net_ref(wr, net).kind == REF_OWN) {
            vw_puts(wr, "  ");
            put_attrs(wr, (odin3_objref){ODIN3_OBJ_NET, i}, (vw_attr_frame){"", " "});
            vw_puts(wr, net_ref(wr, net).reg ? "reg " : "wire ");
            vw_name(wr, OBJ_NET, i);
            vw_char(wr, ';');
            vw_eol(wr, odin3_net_prov(module, net));
        }
    }
}

/* Dangling output wires, the regs of storage cells and the global clock. */
static void declare_cell_extras(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 1; i < odin3_module_pin_end(module); i++) {
        if (name_of(wr, OBJ_PIN, i) != 0) {
            vw_puts(wr, "  wire ");
            vw_name(wr, OBJ_PIN, i);
            vw_puts(wr, ";\n");
        }
    }
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        const vw_builtin *bi = odin3_node_live(module, node) ? node_builtin(wr, node) : NULL;
        if (bi != NULL && name_of(wr, OBJ_NODE, i) != 0) {
            uint32_t width = out_port(wr, node).count;
            vw_puts(wr, "  reg ");
            if (bi->form == FORM_FF) {
                vw_char(wr, '[');
                vw_u32(wr, width - 1);
                vw_puts(wr, ":0] ");
            }
            vw_name(wr, OBJ_NODE, i);
            vw_puts(wr, ";\n");
        }
    }
    if (name_of(wr, OBJ_GCLK, 0) != 0) {
        vw_puts(wr, "  (* gclk *) wire ");
        vw_name(wr, OBJ_GCLK, 0);
        vw_puts(wr, ";\n");
    }
}

static bool canonical(const verilog_writer *wr, odin3_wire_id wire, uint32_t bit) {
    vw_ref ref = net_ref(wr, odin3_wire_net(wr->mod.module, wire, bit));
    return ref.kind == REF_WIRE && ref.wire == wire.v && ref.bit == bit;
}

/* `assign w[hi:lo] = <refs>;` for each run of wire bits whose nets are written elsewhere. */
static void write_wire_aliases(verilog_writer *wr, odin3_wire_id wire) {
    uint32_t bit = odin3_wire_width(wr->mod.module, wire);
    while (bit > 0) {
        if (canonical(wr, wire, bit - 1)) {
            bit--;
            continue;
        }
        uint32_t hi = bit - 1;
        while (bit > 0 && !canonical(wr, wire, bit - 1)) {
            bit--;
        }
        vw_puts(wr, "  assign ");
        put_wire_bits(wr, wire, hi, bit);
        vw_puts(wr, " = ");
        vw_items items = wire_items(wire, hi, bit);
        put_items(wr, &items, false);
        vw_puts(wr, ";\n");
    }
}

/* Output ports in port order, then non-port wires in ID order. */
static void write_aliases(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 0; i < odin3_module_port_count(module); i++) {
        if (wr->mod.def->ports[i].dir == ODIN3_DIR_OUT) {
            write_wire_aliases(wr, odin3_module_port_wire(module, i));
        }
    }
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (odin3_wire_live(module, wire) && wire_port(wr, wire) == 0) {
            write_wire_aliases(wr, wire);
        }
    }
}

static odin3_status write_module(verilog_writer *wr, uint32_t id) {
    const odin3_module *module =
        odin3_module_get((odin3_design *)wr->design, (odin3_module_id){id});
    if (begin_module(wr, module) == ODIN3_OK) {
        write_header(wr);
        declare_wires_and_nets(wr);
        declare_cell_extras(wr);
        write_aliases(wr);
        for (uint32_t i = 1; wr->st == ODIN3_OK && i < odin3_module_node_end(module); i++) {
            odin3_node_id node = {i};
            if (odin3_node_live(module, node) && !is_port_node(wr, node)) {
                write_cell(wr, node);
            }
        }
        vw_puts(wr, "endmodule\n");
        vw_flush(wr);
    }
    end_module(wr);
    return wr->st;
}

/* --- black-box stubs ----------------------------------------------------------------------- */

/* The width of a port whose width is a function, for the type's default parameters. */
static uint32_t default_width(verilog_writer *wr, odin3_celltype_id type, uint32_t port) {
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
    odin3_value *params = odin3_util_calloc(sizeof(odin3_value) * (def->n_params + 1));
    if (params == NULL) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        return 1;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
    }
    uint32_t width = odin3_celltype_port_width(wr->design, type, params, port);
    odin3_util_free(params);
    return width;
}

/* A stub port's range: `[P-1:0] ` for a width parameter P, `[w-1:0] ` otherwise (none when
 * scalar). */
static void put_stub_range(verilog_writer *wr, odin3_celltype_id type, uint32_t port) {
    const odin3_port_def *def = &odin3_celltype_get(wr->design, type)->ports[port];
    if (def->width_fn == NULL && def->width_param != NULL) {
        vw_char(wr, '[');
        vw_ident(wr, odin3_bytes_cstr(def->width_param), "parameter name");
        vw_puts(wr, "-1:0] ");
    } else if (!def->scalar) {
        uint32_t width = def->width_fn != NULL ? default_width(wr, type, port) : def->width;
        if (width == 0) {
            return; /* Verilog has no zero-width port: declared 1 bit, left unconnected */
        }
        vw_char(wr, '[');
        vw_i64(wr, (int64_t)width - 1);
        vw_puts(wr, ":0] ");
    }
}

static void write_stub_params(verilog_writer *wr, const odin3_celltype_def *def) {
    if (def->n_params == 0) {
        return;
    }
    vw_puts(wr, " #(\n");
    for (uint32_t i = 0; i < def->n_params; i++) {
        vw_puts(wr, "  parameter ");
        vw_ident(wr, odin3_bytes_cstr(def->params[i].name), "parameter name");
        vw_puts(wr, " = ");
        put_value(wr, &def->params[i].dflt);
        vw_puts(wr, i + 1 < def->n_params ? ",\n" : "\n");
    }
    vw_char(wr, ')');
}

/* `(* blackbox *) module name #(parameters) (ports); endmodule` for a cell type, once. */
static void write_stub(verilog_writer *wr, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
    if (odin3_u64map_get(wr->stubbed, type.v, NULL)) {
        return;
    }
    if (odin3_u64map_put(wr->stubbed, (odin3_kv){type.v, 1}) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        return;
    }
    wr->mod.name = def->name;
    begin_unit(wr);
    vw_puts(wr, "(* blackbox *)\nmodule ");
    vw_ident(wr, odin3_bytes_cstr(def->name), "black-box name cannot be written in Verilog:");
    write_stub_params(wr, def);
    vw_puts(wr, def->n_ports == 0 ? ";\n" : " (\n");
    for (uint32_t i = 0; i < def->n_ports; i++) {
        vw_puts(wr, "  ");
        vw_puts(wr, dir_word(def->ports[i].dir));
        put_stub_range(wr, type, i);
        vw_ident(wr, odin3_bytes_cstr(def->ports[i].name), "port name cannot be written:");
        vw_puts(wr, i + 1 < def->n_ports ? ",\n" : "\n");
    }
    vw_puts(wr, def->n_ports == 0 ? "endmodule\n" : ");\nendmodule\n");
    wr->mod.name = NULL;
    vw_flush(wr);
}

static bool stub_kind(const odin3_celltype_def *def) {
    return def->gran == ODIN3_GRAN_BLACKBOX || def->gran == ODIN3_GRAN_HARD;
}

/* Declared models in declaration order, then other instantiated black-box and hard types. */
static odin3_status write_stubs(verilog_writer *wr) {
    for (uint32_t i = 0; i < odin3_design_declared_model_count(wr->design); i++) {
        odin3_celltype_id type = odin3_design_declared_model(wr->design, i);
        if (stub_kind(odin3_celltype_get(wr->design, type))) {
            write_stub(wr, type);
        }
    }
    for (uint32_t i = 1; wr->st == ODIN3_OK; i++) {
        odin3_celltype_id type = {i};
        const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
        if (def == NULL) {
            break;
        }
        if (stub_kind(def) && odin3_celltype_instances(wr->design, type) > 0) {
            write_stub(wr, type);
        }
    }
    return wr->st;
}

/* --- entry point --------------------------------------------------------------------------- */

static odin3_status wr_init(verilog_writer *wr, const odin3_design *design, const char *path) {
    *wr = (verilog_writer){.design = design,
                           .tab = odin3_design_strtab(design),
                           .path = path,
                           .escaped_end = SIZE_MAX};
    odin3_strbuf_init(&wr->out);
    odin3_strbuf_init(&wr->cand);
    odin3_wattr_seen_init(&wr->seen);
    wr->builtin = odin3_u64map_create(0);
    wr->prov_leaf = odin3_u64map_create(0);
    wr->stubbed = odin3_u64map_create(0);
    if (wr->builtin == NULL || wr->prov_leaf == NULL || wr->stubbed == NULL) {
        return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t i = 0; i < N_BUILTINS; i++) {
        uint32_t name = 0;
        odin3_celltype_id type = {0};
        if (odin3_strtab_find(wr->tab, odin3_bytes_cstr(BUILTINS[i].name), &name) &&
            odin3_celltype_find(design, name, &type) &&
            odin3_u64map_put(wr->builtin, (odin3_kv){type.v, i}) != ODIN3_OK) {
            return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
    }
    return ODIN3_OK;
}

/* Creates a private temporary beside the destination; the destination is untouched until the
 * rename. */
static odin3_status wr_open(verilog_writer *wr) {
    odin3_status st = odin3_atomic_file_open(&wr->file, wr->path);
    if (st != ODIN3_OK) {
        return vw_fail(wr, st);
    }
    wr->opened = true;
    /* The writer buffers itself; unbuffered stdio reports a failed write at once. */
    if (setvbuf(wr->file.fp, NULL, _IONBF, 0) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot set the output buffer", wr->path);
        return vw_fail(wr, ODIN3_ERR_IO);
    }
    return ODIN3_OK;
}

/* Renames the temporary over the destination on success or removes it on any failure; a close
 * or rename failure becomes the sticky status. */
static void wr_finish(verilog_writer *wr) {
    if (wr->opened) {
        wr->opened = false;
        wr->st = odin3_atomic_file_close(&wr->file, wr->st);
    }
}

static void wr_free(verilog_writer *wr) {
    end_module(wr);
    odin3_strbuf_free(&wr->out);
    odin3_strbuf_free(&wr->cand);
    odin3_wattr_seen_free(&wr->seen);
    odin3_u64map_destroy(wr->builtin);
    odin3_u64map_destroy(wr->prov_leaf);
    odin3_u64map_destroy(wr->stubbed);
}

odin3_status odin3_verilog_write(const odin3_design *design, const char *path,
                                 const odin3_verilog_opts *opts) {
    if (design == NULL || path == NULL ||
        (opts != NULL && opts->name_style != ODIN3_VERILOG_NAMES_SHORT &&
         opts->name_style != ODIN3_VERILOG_NAMES_PROVENANCE)) {
        odin3_log(ODIN3_LOG_ERROR, "verilog_write: NULL design or path, or an unknown name style");
        return ODIN3_ERR_INVALID_ARG;
    }
    verilog_writer wr;
    odin3_status st = wr_init(&wr, design, path);
    wr.name_style = opts != NULL ? opts->name_style : ODIN3_VERILOG_NAMES_SHORT;
    if (st == ODIN3_OK) {
        st = wr_open(&wr);
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        st = write_module(&wr, i);
    }
    if (st == ODIN3_OK) {
        (void)write_stubs(&wr);
    }
    vw_flush(&wr);
    wr_finish(&wr); /* the status is sticky: wr.st holds the first failure */
    st = wr.st;
    wr_free(&wr);
    return st;
}
