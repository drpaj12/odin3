/* writer.c — structural Verilog writer: modules, cells, aliases and black-box stubs. */
#include "backends/verilog/writer.h"

#include "backends/blif/isop.h"
#include "backends/common/attrs.h"
#include "backends/common/hash_rows.h"
#include "backends/common/hdl_loc.h"
#include "backends/prov_name.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/ffinit.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FLUSH_AT = 1 << 16,        /* pending output bytes that trigger a write */
    KEY_SHIFT = 32,            /* object-name key: kind << 32 | ID */
    GENERATED_NAME_BYTES = 32, /* prefix and a 32-bit ID in decimal */
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
    FORM_LUT,    /* sum of products over the table's irredundant cover (3D, point (q)) */
    FORM_LATCH,  /* bit-level flip-flop or latch with INIT */
    FORM_BINARY, /* Y = A' <op> B' */
    FORM_UNARY,  /* Y = <op>A' */
    FORM_PMUX,   /* priority chain */
    FORM_TRIBUF, /* Y = EN ? A : z */
    FORM_FF,     /* word-level flip-flop */
    FORM_SHIFTX, /* Quartus mode: $shiftx */
    FORM_FFX,    /* Quartus mode: any other flip-flop or latch, as plain always blocks */
    FORM_MEMQ,   /* Quartus mode: $mem as a behavioural reg array */
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
    {"$lut", FORM_LUT, NULL, 0},
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
    /* 2D word cells (EL-31): simlib's own expressions; a reduction or logical result is one bit,
     * zero-extended by the assignment, and its operand is self-determined. */
    {"$neg", FORM_UNARY, "-", 0},
    {"$reduce_xnor", FORM_UNARY, "~^", 0},
    {"$reduce_bool", FORM_UNARY, "|", 0},
    {"$logic_not", FORM_UNARY, "!", 0},
    {"$xnor", FORM_BINARY, "~^", 0},
    {"$logic_and", FORM_BINARY, "&&", 0},
    {"$logic_or", FORM_BINARY, "||", 0},
    {"$mux", FORM_MUX, NULL, 0},
    {"$pmux", FORM_PMUX, NULL, 0},
    {"$tribuf", FORM_TRIBUF, NULL, 0},
    {"$dff", FORM_FF, NULL, 0},
    {"$dffe", FORM_FF, NULL, VB_EN},
    {"$adff", FORM_FF, NULL, VB_ARST},
    {"$sdff", FORM_FF, NULL, VB_SRST},
    /* Quartus mode only (registered by name in wr_init): word cells written as `assign`s. */
    {"$pow", FORM_BINARY, "**", 0},
    {"$shiftx", FORM_SHIFTX, NULL, 0},
    /* Quartus mode only: the shared entries of the storage cells and memories (no type name). */
    {"$ffx", FORM_FFX, NULL, 0},
    {"$memq", FORM_MEMQ, NULL, 0},
};
enum { N_BUILTINS = sizeof BUILTINS / sizeof BUILTINS[0] };
enum { BI_QUARTUS_FIRST = N_BUILTINS - 4, BI_FFX = N_BUILTINS - 2, BI_MEMQ = N_BUILTINS - 1 };

/* Parameter indices: word cells (A_SIGNED B_SIGNED …), $sop, word flip-flops, bit storage. */
enum { PARAM_A_SIGNED = 0, PARAM_B_SIGNED = 1, PARAM_SHIFTX_B_WIDTH = 3, PARAM_SHIFTX_Y_WIDTH = 4 };
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
typedef enum vw_obj { OBJ_WIRE = 1, OBJ_NET, OBJ_NODE, OBJ_PIN, OBJ_GCLK, OBJ_AUX } vw_obj;

typedef struct vw_module {
    const odin3_module *module;
    const odin3_celltype_def *def; /* the module's cell type: port directions, scalar flags */
    const char *name;              /* for messages */
    odin3_strtab *names;           /* identifiers given out in this module */
    odin3_u64map *obj_names;       /* kind << 32 | ID -> names ID */
    odin3_u64map *vectors;         /* names ID of a vector wire -> 1 */
    vw_ref *refs;                  /* by net ID */
    uint32_t *wire_port;           /* by wire ID: port index + 1, 0 for a non-port wire */
    uint32_t *regwire; /* by net ID: the scalar non-port wire that is the reg of the one-bit $dff
                          driving the net, 0 for none */
} vw_module;

typedef struct verilog_writer {
    const odin3_design *design;
    const odin3_strtab *tab;
    const char *path;
    odin3_verilog_name_style name_style;
    bool quartus;           /* Quartus mode (writer.h) */
    uint32_t n_rom;         /* `.mif` files written so far (ODIN3_ATTR_ROM_INIT) */
    bool bb_params;         /* the node being written is an implicit black box: its parameters
                               are `#(...)`, not attributes */
    odin3_atomic_file file; /* a temporary beside path, renamed over it on success */
    bool opened;            /* file is open: wr_finish commits or removes it */
    bool oom_logged;
    size_t escaped_end; /* out.len just after an escaped identifier, SIZE_MAX otherwise */
    odin3_status st;    /* sticky: the first failure */
    uint32_t line;      /* lines written to the file so far */
    uint32_t units;     /* modules and stubs written (a blank line separates them) */
    odin3_strbuf out;
    odin3_strbuf cand;     /* a candidate generated name */
    odin3_u64map *builtin; /* cell type ID -> BUILTINS index */
    odin3_hdl_ctx *hdl;    /* HDL locations and the records that name objects (hdl_loc.h) */
    odin3_hash_rows rows;  /* HASH: the objects named by provenance, all modules (5.4f) */
    odin3_strbuf src;      /* scratch: the text of a src attribute */
    odin3_u64map *stubbed; /* cell type ID -> 1 once its stub is written */
    odin3_wattr_seen seen; /* attribute names written in the current (* … *) */
    vw_module mod;
} verilog_writer;

static void name_mem_regs(verilog_writer *wr, odin3_node_id node);

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

/* Ends a line: `  // file:line` of prov's first HDL location (spec 5.4 W-2, W-3: a SOURCE leaf,
 * line only) when it has one, then the newline. */
static void vw_eol(verilog_writer *wr, odin3_prov_id prov) {
    odin3_srcloc loc = {0};
    bool found = false;
    if (wr->st == ODIN3_OK) {
        odin3_status st = odin3_hdl_first(wr->hdl, prov, &loc, &found);
        if (st != ODIN3_OK) {
            (void)vw_fail(wr, st);
            found = false;
        }
    }
    if (found) {
        vw_raw(wr, odin3_bytes_cstr("  // "));
        vw_raw(wr, str_bytes(wr, loc.file));
        vw_raw(wr, odin3_bytes_cstr(":"));
        vw_u32(wr, loc.line);
    }
    vw_char(wr, '\n');
    if (wr->out.len >= FLUSH_AT) {
        vw_flush(wr);
    }
}

/* Ends a line written without provenance (`;\n` and the like) and drains a full buffer, as
 * vw_eol does. */
static void vw_line_end(verilog_writer *wr, const char *text) {
    vw_puts(wr, text);
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
    if (port != 0 && odin3_port_is_split_shell(wr->design, wr->mod.module,
                                               odin3_module_port(wr->mod.module, port - 1U))) {
        return; /* the shell of an inout the `tristate` pass split: it names no net */
    }
    if (port != 0 && wr->mod.def->ports[port - 1].dir != ODIN3_DIR_OUT) {
        pick->driving_ports++;
        rank = RANK_DRIVING_PORT;
    }
    if (rank > pick->rank) {
        pick->rank = rank;
        pick->ref = (vw_ref){wb.wire.v, wb.bit, REF_WIRE, false};
    }
}

/* The scalar non-port wire on net when a one-bit $dff drives it alone: that wire is the flop's
 * `reg` (a reader makes a $dff out of `always @(...) w <= d;` and a one-bit `reg w;`, so the file
 * is written back the way a bit-level flop's own reg is). 0 when there is none. */
static uint32_t flop_reg_wire(const verilog_writer *wr, odin3_net_id net) {
    const odin3_module *module = wr->mod.module;
    if (odin3_net_driver_count(module, net) != 1) {
        return 0;
    }
    odin3_node_id node = odin3_pin_node(module, odin3_net_driver(module, net));
    const vw_builtin *bi = node_builtin(wr, node);
    if (bi == NULL || bi->form != FORM_FF || out_port(wr, node).count != 1) {
        return 0;
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    odin3_wirebit wb = odin3_net_primary(module, net);
    bool more = true;
    while (more) {
        if (odin3_wire_valid(wb.wire) && odin3_wire_live(module, wb.wire) &&
            wire_port(wr, wb.wire) == 0 && wire_scalar(wr, wb.wire)) {
            return wb.wire.v;
        }
        more = odin3_net_alias_next(module, net, &cursor, &alias);
        wb = alias.wb;
    }
    return 0;
}

/* Uses of a flop's Q net name another wire of the net when it has one (an output port first), as
 * those of a bit-level flop do; only the reg wire itself is left otherwise. */
static void prefer_other(const verilog_writer *wr, odin3_net_id net, vw_ref *ref) {
    const odin3_module *module = wr->mod.module;
    uint32_t cursor = 0;
    odin3_net_alias alias;
    odin3_wirebit wb = odin3_net_primary(module, net);
    bool found = false;
    bool more = true;
    while (more) {
        if (odin3_wire_valid(wb.wire) && odin3_wire_live(module, wb.wire) &&
            wb.wire.v != wr->mod.regwire[net.v]) {
            uint32_t port = wire_port(wr, wb.wire);
            bool out = port != 0 && wr->mod.def->ports[port - 1].dir == ODIN3_DIR_OUT;
            if (out || !found) {
                *ref = (vw_ref){wb.wire.v, wb.bit, REF_WIRE, false};
                found = true;
            }
            if (out) {
                return;
            }
        }
        more = odin3_net_alias_next(module, net, &cursor, &alias);
        wb = alias.wb;
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
    wr->mod.regwire[net.v] = pick.rank != 0 ? flop_reg_wire(wr, net) : 0;
    if (wr->mod.regwire[net.v] != 0 && pick.ref.wire == wr->mod.regwire[net.v]) {
        prefer_other(wr, net, &wr->mod.refs[net.v]);
    }
}

static vw_ref net_ref(const verilog_writer *wr, odin3_net_id net) {
    return wr->mod.refs[net.v];
}

/* True when the Q net of a bit-level storage cell is declared as its `reg`. */
static bool q_is_reg(const verilog_writer *wr, odin3_node_id node) {
    odin3_net_id net = odin3_pin_net(wr->mod.module, out_port(wr, node).first);
    return odin3_net_valid(net) && ((net_ref(wr, net).kind == REF_OWN && net_ref(wr, net).reg) ||
                                    wr->mod.regwire[net.v] != 0);
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
    if (bi->form == FORM_FFX || bi->form == FORM_MEMQ) {
        return true;
    }
    return (bi->form == FORM_FF || bi->form == FORM_LATCH) && !q_is_reg(wr, node);
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

/* True for the styles that try a provenance name first: provenance and hash (5.4f). */
static bool prov_style(const verilog_writer *wr) {
    return wr->name_style == ODIN3_VERILOG_NAMES_PROVENANCE ||
           wr->name_style == ODIN3_VERILOG_NAMES_HASH;
}

/* The map kind of an object kind that provenance names. */
static odin3_name_kind name_kind_of(vw_obj kind) {
    if (kind == OBJ_WIRE) {
        return ODIN3_NAME_WIRE;
    }
    return kind == OBJ_NET ? ODIN3_NAME_NET : ODIN3_NAME_NODE;
}

/* HASH: remembers obj, just named wr->cand by provenance, for hash_apply. */
static void note_hashed(verilog_writer *wr, const vw_named *obj) {
    if (wr->name_style != ODIN3_VERILOG_NAMES_HASH) {
        return;
    }
    odin3_status st = odin3_hash_rows_add(
        &wr->rows, &(odin3_hash_row_req){
                       .module = odin3_module_id_of(wr->mod.module).v,
                       .module_name = odin3_bytes_cstr(wr->mod.name != NULL ? wr->mod.name : ""),
                       .kind = name_kind_of(obj->kind),
                       .key = obj_key(obj->kind, obj->id),
                       .full = {wr->cand.data, wr->cand.len},
                       .prov = obj->prov.v});
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    }
}

/* PROVENANCE style (IR §6): tries hier/<short name>@file:line from the record naming the object
 * (backends/prov_name.h); nothing when the record has neither a hierarchy nor a location. */
static void claim_provenance(verilog_writer *wr, const vw_named *obj) {
    char generated[GENERATED_NAME_BYTES];
    odin3_bytes short_name = {generated, 0};
    if (obj->own != 0) {
        short_name = str_bytes(wr, obj->own);
    } else {
        int len = snprintf(generated, sizeof generated, "%s%u", obj->prefix, (unsigned)obj->id);
        short_name.len = len > 0 ? (size_t)len : 0;
        if (short_name.len >= sizeof generated) { /* truncated by snprintf */
            short_name.len = sizeof generated - 1;
        }
    }
    if (wr->st != ODIN3_OK) {
        return;
    }
    odin3_status st = odin3_prov_name(wr->hdl, obj->prov, short_name, &wr->cand);
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
        return;
    }
    if (wr->cand.len != 0 &&
        claim(wr, obj_key(obj->kind, obj->id), (odin3_bytes){wr->cand.data, wr->cand.len})) {
        note_hashed(wr, obj);
    }
}

/* The object's provenance name (PROVENANCE style), else its own name when writable and free. */
static void claim_object(verilog_writer *wr, vw_named obj) {
    if (prov_style(wr)) {
        claim_provenance(wr, &obj);
    }
    /* No warning for an object a provenance (or hash) name already names: its own name is not in
     * the output and the message would say a generated name is used. */
    if (obj.own != 0 && !claim(wr, obj_key(obj.kind, obj.id), str_bytes(wr, obj.own)) &&
        wr->st == ODIN3_OK &&
        !odin3_u64map_get(wr->mod.obj_names, obj_key(obj.kind, obj.id), NULL) &&
        odin3_verilog_ident_kind(str_bytes(wr, obj.own)) == ODIN3_VERILOG_UNWRITABLE) {
        odin3_bytes own = str_bytes(wr, obj.own);
        odin3_log(ODIN3_LOG_WARN,
                  "%s: module '%s': name '%.*s' cannot be written in Verilog; a generated name "
                  "(%s<ID>) is used",
                  wr->path, wr->mod.name != NULL ? wr->mod.name : "", (int)own.len,
                  (const char *)own.ptr, obj.prefix);
    }
}

/* HASH: the names of the module that stay as they are (H-3): every name but the provenance names
 * the rows from `first` on hold. */
static odin3_status hash_reserved(verilog_writer *wr, size_t first, odin3_bytes **out,
                                  uint32_t *count) {
    odin3_u64map *renamed = odin3_u64map_create(0);
    size_t names = odin3_strtab_count(wr->mod.names);
    *out = odin3_util_calloc((names + 1) * sizeof **out);
    *count = 0;
    odin3_status st = renamed != NULL && *out != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = first; st == ODIN3_OK && i < odin3_hash_rows_count(&wr->rows); i++) {
        uint64_t str = 0;
        (void)odin3_u64map_get(wr->mod.obj_names, odin3_hash_rows_at(&wr->rows, i)->key, &str);
        st = odin3_u64map_put(renamed, (odin3_kv){str, 1});
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < names; id++) {
        if (!odin3_u64map_get(renamed, id, NULL)) {
            (*out)[(*count)++] = (odin3_bytes){odin3_strtab_get(wr->mod.names, id),
                                               odin3_strtab_len(wr->mod.names, id)};
        }
    }
    odin3_u64map_destroy(renamed);
    return st;
}

/* HASH (5.4f): the objects named by provenance since row `first` take their hash names (H-3), in
 * place of the provenance names: one namespace for all kinds, since Verilog has one. */
static odin3_status hash_apply(verilog_writer *wr, size_t first) {
    odin3_bytes *reserved = NULL;
    uint32_t nreserved = 0;
    odin3_status st = hash_reserved(wr, first, &reserved, &nreserved);
    st = st == ODIN3_OK
             ? odin3_hash_rows_resolve(&wr->rows,
                                       &(odin3_hash_resolve_req){first, reserved, nreserved, true})
             : st;
    for (size_t i = first; st == ODIN3_OK && i < odin3_hash_rows_count(&wr->rows); i++) {
        const odin3_hash_row *row = odin3_hash_rows_at(&wr->rows, i);
        uint32_t str = 0;
        st = odin3_strtab_intern(wr->mod.names,
                                 (odin3_bytes){row->hash.text, strlen(row->hash.text)}, &str);
        st = st == ODIN3_OK ? odin3_u64map_put(wr->mod.obj_names, (odin3_kv){row->key, str}) : st;
    }
    odin3_util_free(reserved);
    return st;
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
    if (bi != NULL && bi->form == FORM_MEMQ && has_output(wr, node)) {
        name_mem_regs(wr, node);
    }
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

/* Records one object that keeps its name (an N line of the version 2 map). */
static odin3_status add_kept(verilog_writer *wr, vw_obj kind, uint32_t id, odin3_prov_id prov) {
    uint32_t name = name_of(wr, kind, id);
    if (name == 0 || prov.v == 0) {
        return ODIN3_OK;
    }
    return odin3_hash_rows_add_kept(
        &wr->rows,
        &(odin3_hash_kept_req){
            .module = odin3_module_id_of(wr->mod.module).v,
            .module_name = odin3_bytes_cstr(wr->mod.name != NULL ? wr->mod.name : ""),
            .kind = name_kind_of(kind),
            .name = {odin3_strtab_get(wr->mod.names, name), odin3_strtab_len(wr->mod.names, name)},
            .prov = prov.v});
}

typedef struct vw_objid {
    vw_obj kind;
    uint32_t id;
} vw_objid;

/* The record of live object (kind, id) of the module being written, none for a dead one. */
static odin3_prov_id live_prov(const verilog_writer *wr, const vw_objid *obj) {
    const odin3_module *module = wr->mod.module;
    vw_obj kind = obj->kind;
    uint32_t id = obj->id;
    if (kind == OBJ_WIRE) {
        return odin3_wire_live(module, (odin3_wire_id){id})
                   ? odin3_wire_prov(module, (odin3_wire_id){id})
                   : (odin3_prov_id){0};
    }
    if (kind == OBJ_NET) {
        return odin3_net_live(module, (odin3_net_id){id})
                   ? odin3_net_prov(module, (odin3_net_id){id})
                   : (odin3_prov_id){0};
    }
    return odin3_node_live(module, (odin3_node_id){id})
               ? odin3_node_prov(module, (odin3_node_id){id})
               : (odin3_prov_id){0};
}

static uint32_t kind_end(const verilog_writer *wr, vw_obj kind) {
    const odin3_module *module = wr->mod.module;
    if (kind == OBJ_WIRE) {
        return odin3_module_wire_end(module);
    }
    return kind == OBJ_NET ? odin3_module_net_end(module) : odin3_module_node_end(module);
}

/* HASH with the version 2 map: every wire, net and cell of the module that keeps its name and has
 * a record gets an N line; the objects the rows from `first` renamed got O lines. */
static odin3_status note_kept(verilog_writer *wr, size_t first) {
    if (!odin3_hash_rows_prov_on(&wr->rows)) {
        return ODIN3_OK;
    }
    odin3_u64map *renamed = odin3_u64map_create(0);
    odin3_status st = renamed != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = first; st == ODIN3_OK && i < odin3_hash_rows_count(&wr->rows); i++) {
        st = odin3_u64map_put(renamed, (odin3_kv){odin3_hash_rows_at(&wr->rows, i)->key, 1});
    }
    static const vw_obj KINDS[] = {OBJ_WIRE, OBJ_NET, OBJ_NODE};
    for (size_t k = 0; st == ODIN3_OK && k < sizeof KINDS / sizeof KINDS[0]; k++) {
        for (uint32_t i = 1; st == ODIN3_OK && i < kind_end(wr, KINDS[k]); i++) {
            st = odin3_u64map_get(renamed, obj_key(KINDS[k], i), NULL)
                     ? ODIN3_OK
                     : add_kept(wr, KINDS[k], i, live_prov(wr, &(vw_objid){KINDS[k], i}));
        }
    }
    odin3_u64map_destroy(renamed);
    return st;
}

static void end_module(verilog_writer *wr) {
    odin3_strtab_destroy(wr->mod.names);
    odin3_u64map_destroy(wr->mod.obj_names);
    odin3_u64map_destroy(wr->mod.vectors);
    odin3_util_free(wr->mod.refs);
    odin3_util_free(wr->mod.wire_port);
    odin3_util_free(wr->mod.regwire);
    wr->mod = (vw_module){0};
}

/* Claims the names of the module's objects (hash names resolved, then the generated names) and,
 * with the version 2 map, records the objects that keep theirs. */
static void name_objects(verilog_writer *wr) {
    size_t first = odin3_hash_rows_count(&wr->rows);
    claim_names(wr);
    odin3_status st = wr->st == ODIN3_OK && wr->name_style == ODIN3_VERILOG_NAMES_HASH
                          ? hash_apply(wr, first)
                          : ODIN3_OK;
    (void)(st != ODIN3_OK ? vw_fail(wr, st) : st);
    generate_names(wr);
    st = wr->st == ODIN3_OK ? note_kept(wr, first) : st;
    (void)(st != ODIN3_OK ? vw_fail(wr, st) : st);
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
    mod->regwire = odin3_util_calloc(sizeof(uint32_t) * odin3_module_net_end(module));
    if (mod->names == NULL || mod->obj_names == NULL || mod->vectors == NULL || mod->refs == NULL ||
        mod->wire_port == NULL || mod->regwire == NULL) {
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
        name_objects(wr);
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

/* A cell's name for a message: its own, else `$c<ID>` (formatted into id). */
static odin3_bytes cell_label(const verilog_writer *wr, odin3_node_id node, char id[NODE_ID_TEXT]) {
    uint32_t name = odin3_node_name(wr->mod.module, node);
    if (name != 0) {
        return str_bytes(wr, name);
    }
    (void)snprintf(id, NODE_ID_TEXT, "$c%u", (unsigned)node.v);
    return odin3_bytes_cstr(id);
}

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

/* The widest B whose whole range the x padding of $shiftx covers (above it the far reads are 0). */
enum { SHIFTX_PAD_BITS_MAX = 12 };

/* One `{N{1'bx}}` of N bits. */
static void put_x_pad(verilog_writer *wr, int64_t bits) {
    vw_char(wr, '{');
    vw_i64(wr, bits);
    vw_puts(wr, "{1'bx}}");
}

/*
 * $shiftx (A B Y; A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH): Y = A[B +: Y_WIDTH], the bits outside
 * A reading x. Written as `({hi pad of x, A, lo pad of x} >> (B + lo))`: the pads cover every B the
 * port can hold (up to SHIFTX_PAD_BITS_MAX bits of B; a negative signed B selects below bit 0), so
 * a read outside A is x as in the IR. A wider B reads 0 far outside the pads (a don't-care).
 */
static void write_shiftx(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    const int64_t width = param_int(wr, node, PARAM_SHIFTX_Y_WIDTH);
    const int64_t b_bits = param_int(wr, node, PARAM_SHIFTX_B_WIDTH);
    const bool b_signed = param_int(wr, node, PARAM_B_SIGNED) != 0;
    const bool padded = b_bits > 0 && b_bits <= SHIFTX_PAD_BITS_MAX;
    const int64_t below = width + (padded && b_signed ? (int64_t)1 << (b_bits - 1) : 0);
    const int64_t above = width + (padded ? (int64_t)1 << b_bits : 0);
    begin_assign(wr, node);
    vw_char(wr, '(');
    vw_char(wr, '{');
    put_x_pad(wr, above);
    vw_puts(wr, ", ");
    put_port(wr, node, 0, false);
    vw_puts(wr, ", ");
    put_x_pad(wr, below);
    vw_puts(wr, "} >> (");
    put_port(wr, node, 1, b_signed);
    vw_puts(wr, b_signed ? " + $signed(32'd" : " + 32'd");
    vw_i64(wr, below);
    vw_puts(wr, b_signed ? ")))" : "))");
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

/* Cover rows: count rows of stride bytes, each starting with one plane character per input. */
typedef struct vw_rows {
    const uint8_t *data;
    size_t stride;
    uint32_t count;
} vw_rows;

/* The products of every row joined by |. */
static void write_sum(verilog_writer *wr, const vw_rows *rows, odin3_pinslice ins) {
    for (uint32_t i = 0; i < rows->count; i++) {
        vw_puts(wr, i > 0 ? " | " : "");
        write_product(wr, rows->data + (size_t)i * rows->stride, ins, rows->count > 1);
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
        (void)vw_refuse(wr, "$sop cover mixes ON-set and OFF-set rows in cell",
                        cell_label(wr, node, id));
        return;
    }
    begin_assign(wr, node);
    if (rows == 0 || ins.count == 0) {
        vw_puts(wr, rows > 0 && out == '1' ? "1'b1" : "1'b0");
    } else {
        vw_puts(wr, out == '0' ? "~(" : "");
        vw_rows cover_rows = {cover->bits, row_len, rows};
        write_sum(wr, &cover_rows, ins);
        vw_puts(wr, out == '0' ? ")" : "");
    }
    end_stmt(wr, node);
}

/* --- $lut ---------------------------------------------------------------------------------- */

/*
 * `assign Y = <expr>;` over the irredundant cover of the table (3E's odin3_blif_isop: the shorter
 * of the on-set and the off-set, the on-set on a tie), inputs A[0] .. A[k-1] in pin order. An
 * off-set cover is written `~(<sum>)`; a constant table is `1'b0` or `1'b1`.
 */
static void write_lut(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    odin3_pinslice ins = odin3_node_port(wr->mod.module, node, ODIN3_LUT_PORT_A);
    const odin3_value params[2] = {*odin3_node_param(wr->mod.module, node, ODIN3_LUT_P_WIDTH),
                                   *odin3_node_param(wr->mod.module, node, ODIN3_LUT_P_LUT)};
    uint64_t words[ODIN3_LUT_MAX_WORDS];
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t n_rows = 0;
    int constant = -1;
    if (!odin3_lut_table(params, words, (uint32_t)(sizeof words / sizeof words[0]))) {
        char id[NODE_ID_TEXT];
        (void)vw_refuse(wr, "$lut with invalid parameters in cell", cell_label(wr, node, id));
    } else if (odin3_blif_isop(words, ins.count, &cover, &n_rows, &constant) != ODIN3_OK) {
        (void)vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    } else {
        begin_assign(wr, node);
        if (constant >= 0) {
            vw_puts(wr, constant == 1 ? "1'b1" : "1'b0");
        } else {
            /* each row: the k plane characters, a blank, the output character, a newline */
            vw_rows rows = {(const uint8_t *)cover.data, (size_t)ins.count + 3U, n_rows};
            bool off_set = cover.data[ins.count + 1U] == '0';
            vw_puts(wr, off_set ? "~(" : "");
            write_sum(wr, &rows, ins);
            vw_puts(wr, off_set ? ")" : "");
        }
        end_stmt(wr, node);
    }
    odin3_strbuf_free(&cover);
}

/* --- storage cells ------------------------------------------------------------------------- */

/* The reg a storage cell assigns: its Q net when declared `reg`, else the cell's own reg. */
static void put_target(verilog_writer *wr, odin3_node_id node, bool q_reg) {
    odin3_net_id net = odin3_pin_net(wr->mod.module, out_port(wr, node).first);
    if (q_reg && odin3_net_valid(net) && wr->mod.regwire[net.v] != 0) {
        vw_name(wr, OBJ_WIRE, wr->mod.regwire[net.v]);
    } else if (q_reg) {
        put_pin(wr, out_port(wr, node).first);
    } else {
        vw_name(wr, OBJ_NODE, node.v);
    }
}

/* `  assign <Q> = <cell reg>;` when the cell has its own reg. */
static void connect_reg(verilog_writer *wr, odin3_node_id node, bool q_reg) {
    odin3_net_id net = odin3_pin_net(wr->mod.module, out_port(wr, node).first);
    uint32_t regwire = odin3_net_valid(net) ? wr->mod.regwire[net.v] : 0;
    if (q_reg && (regwire == 0 || net_ref(wr, net).wire == regwire)) {
        return;
    }
    vw_puts(wr, "  assign ");
    put_port(wr, node, node_def(wr, node)->n_ports - 1, false);
    vw_puts(wr, " = ");
    if (regwire != 0) {
        vw_name(wr, OBJ_WIRE, regwire);
    } else {
        vw_name(wr, OBJ_NODE, node.v);
    }
    vw_line_end(wr, ";\n");
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

enum { WORD_INIT_MAX = 4096 }; /* the widest word flip-flop whose init is written */

/* A word flip-flop's power-up value: the `init` attribute (v1.6.1; ffinit.h) of the nets its Q bits
 * drive, as one `initial reg[bit] = 1'bV;` per defined bit, so Quartus starts the register there
 * (the attribute alone is a comment to it). */
static bool bit_known(odin3_bit bit) {
    return bit == ODIN3_BIT_0 || bit == ODIN3_BIT_1;
}

/* The init bits of the Q port into bits[]; the number of known ones. */
static uint32_t word_init_bits(const verilog_writer *wr, odin3_pinslice port, odin3_bit *bits) {
    uint32_t known = 0;
    for (uint32_t bit = 0; bit < port.count && bit < WORD_INIT_MAX; bit++) {
        odin3_net_id net = odin3_pin_net(wr->mod.module, (odin3_pin_id){port.first.v + bit});
        bits[bit] = odin3_net_valid(net) ? odin3_ffinit_get(wr->mod.module, net) : ODIN3_BIT_X;
        known += bit_known(bits[bit]) ? 1U : 0U;
    }
    return known;
}

/* `initial reg[bit] = 1'bV;` for each known bit. */
static void write_bit_inits(verilog_writer *wr, odin3_node_id node, const odin3_bit *bits,
                            uint32_t width) {
    for (uint32_t bit = 0; bit < width; bit++) {
        if (!bit_known(bits[bit])) {
            continue;
        }
        vw_puts(wr, "  initial ");
        vw_name(wr, OBJ_NODE, node.v);
        if (width > 1) {
            vw_char(wr, '[');
            vw_u32(wr, bit);
            vw_char(wr, ']');
        }
        vw_puts(wr, bits[bit] == ODIN3_BIT_0 ? " = 1'b0;\n" : " = 1'b1;\n");
    }
}

static void write_word_init(verilog_writer *wr, odin3_node_id node) {
    odin3_pinslice port = out_port(wr, node);
    odin3_bit bits[WORD_INIT_MAX];
    uint32_t known = word_init_bits(wr, port, bits);
    uint32_t width = port.count < WORD_INIT_MAX ? port.count : WORD_INIT_MAX;
    if (known != width || width < 2) {
        write_bit_inits(wr, node, bits, width);
        return;
    }
    vw_puts(wr, "  initial "); /* every bit known: one word */
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, " = ");
    vw_u32(wr, width);
    vw_puts(wr, "'b");
    for (uint32_t bit = width; bit-- > 0;) {
        vw_char(wr, bits[bit] == ODIN3_BIT_1 ? '1' : '0');
    }
    vw_puts(wr, ";\n");
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
    bool q_reg = q_is_reg(wr, node);
    if ((bi->flags & (VB_EN | VB_ARST | VB_SRST)) != 0) {
        put_condition(wr, node);
    }
    if ((bi->flags & (VB_ARST | VB_SRST)) != 0) {
        const odin3_value *reset = odin3_node_param(wr->mod.module, node, FF_RESET_VALUE);
        put_target(wr, node, q_reg);
        vw_puts(wr, " <= ");
        put_bits(wr, reset->bits, reset->len);
        vw_puts(wr, "; else ");
    }
    put_target(wr, node, q_reg);
    vw_puts(wr, " <= ");
    put_port(wr, node, data, false);
    end_stmt(wr, node);
    if (wr->quartus && !q_reg) {
        write_word_init(wr, node);
    }
    connect_reg(wr, node, q_reg);
}

/* --- Quartus mode: storage cells and memories as plain Verilog-2001 ------------------------ */

/* What a word or gate-level storage cell does, read from its type's port and parameter names. */
typedef struct vw_ffx {
    bool latch;      /* level-sensitive: no clock */
    bool per_bit;    /* set/clear inputs per bit ($dffsr, $dffsre, $dlatchsr, $sr) */
    bool srst_first; /* a sync reset beats the enable ($sdffce is the exception) */
    int clk, en, arst, srst, aload, ad, set, clr, d;
    bool clk_pos, en_pos, arst_pos, srst_pos, aload_pos, set_pos, clr_pos;
    const odin3_value *rv; /* word cells: the reset value */
    int rbit;              /* gate-level cells: the reset value 0/1, else -1 */
    uint32_t width;
} vw_ffx;

static int port_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int param_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The INT parameter `<ctl>_POLARITY` of node (1 when the type has none). */
static bool ffx_polarity(const verilog_writer *wr, odin3_node_id node, const char *ctl) {
    char name[GENERATED_NAME_BYTES];
    (void)snprintf(name, sizeof name, "%s_POLARITY", ctl);
    int idx = param_index(node_def(wr, node), name);
    return idx < 0 || param_int(wr, node, (uint32_t)idx) != 0;
}

/* Fills ffx for the eleven word flip-flops and latches by their port and parameter names. */
static void ffx_word(const verilog_writer *wr, odin3_node_id node, vw_ffx *ffx) {
    const odin3_celltype_def *def = node_def(wr, node);
    ffx->clk = port_index(def, "CLK");
    ffx->en = port_index(def, "EN");
    ffx->arst = port_index(def, "ARST");
    ffx->srst = port_index(def, "SRST");
    ffx->aload = port_index(def, "ALOAD");
    ffx->ad = port_index(def, "AD");
    ffx->set = port_index(def, "SET");
    ffx->clr = port_index(def, "CLR");
    ffx->d = port_index(def, "D");
    ffx->latch = ffx->clk < 0 && ffx->aload < 0;
    ffx->per_bit = ffx->set >= 0;
    ffx->srst_first = strcmp(def->name, "$sdffce") != 0;
    ffx->clk_pos = ffx_polarity(wr, node, "CLK");
    ffx->en_pos = ffx_polarity(wr, node, "EN");
    ffx->arst_pos = ffx_polarity(wr, node, "ARST");
    ffx->srst_pos = ffx_polarity(wr, node, "SRST");
    ffx->aload_pos = ffx_polarity(wr, node, "ALOAD");
    ffx->set_pos = ffx_polarity(wr, node, "SET");
    ffx->clr_pos = ffx_polarity(wr, node, "CLR");
    int rv = param_index(def, ffx->arst >= 0 ? "ARST_VALUE" : "SRST_VALUE");
    ffx->rv = rv >= 0 ? odin3_node_param(wr->mod.module, node, (uint32_t)rv) : NULL;
    int width = param_index(def, "WIDTH");
    ffx->width = width >= 0 ? (uint32_t)param_int(wr, node, (uint32_t)width) : 1;
}

/* The gate-level families of ir/cells/dffx.h: `$_<family>_<c>[<r><other>][<e>]_`. */
typedef struct vw_gate_family {
    const char *prefix;
    size_t letters;
    bool sync;      /* the reset is synchronous */
    bool reset;     /* has R and a reset value */
    bool enable;    /* has E */
    bool reset_top; /* the reset beats the enable */
} vw_gate_family;

static const vw_gate_family GATE_FAMILIES[] = {
    {"$_DFF_", 3, false, true, false, true}, {"$_DFFE_", 2, false, false, true, true},
    {"$_DFFE_", 4, false, true, true, true}, {"$_SDFF_", 3, true, true, false, true},
    {"$_SDFFE_", 4, true, true, true, true}, {"$_SDFFCE_", 4, true, true, true, false},
};

/* True for the 68 gate-level types with a reset or an enable; fills ffx from the name letters. */
static bool ffx_gate(const odin3_celltype_def *def, vw_ffx *ffx) {
    size_t len = strlen(def->name);
    for (size_t i = 0; i < sizeof GATE_FAMILIES / sizeof GATE_FAMILIES[0]; i++) {
        const vw_gate_family *fam = &GATE_FAMILIES[i];
        size_t plen = strlen(fam->prefix);
        if (len != plen + fam->letters + 1 || strncmp(def->name, fam->prefix, plen) != 0 ||
            def->name[len - 1] != '_') {
            continue;
        }
        const char *let = def->name + plen;
        ffx->clk = port_index(def, "C");
        ffx->arst = !fam->sync && fam->reset ? port_index(def, "R") : -1;
        ffx->srst = fam->sync ? port_index(def, "R") : -1;
        ffx->en = fam->enable ? port_index(def, "E") : -1;
        ffx->d = port_index(def, "D");
        ffx->clk_pos = let[0] == 'P';
        ffx->arst_pos = ffx->srst_pos = fam->reset && let[1] == 'P';
        ffx->rbit = fam->reset ? let[2] - '0' : -1;
        ffx->en_pos = fam->enable && let[fam->letters - 1] == 'P';
        ffx->srst_first = fam->reset_top;
        ffx->width = 1;
        return true;
    }
    return false;
}

/* Fills ffx for a word or gate-level storage cell of Quartus mode; false for any other type. */
static bool ffx_describe(const verilog_writer *wr, odin3_node_id node, vw_ffx *ffx) {
    const odin3_celltype_def *def = node_def(wr, node);
    *ffx = (vw_ffx){.rbit = -1,
                    .clk = -1,
                    .en = -1,
                    .arst = -1,
                    .srst = -1,
                    .aload = -1,
                    .ad = -1,
                    .set = -1,
                    .clr = -1,
                    .d = -1};
    if (def->name[1] == '_') {
        return ffx_gate(def, ffx);
    }
    ffx_word(wr, node, ffx);
    return true;
}

/* `  // <type> <cell name>` before a storage cell or memory, for provenance. */
static void put_cell_comment(verilog_writer *wr, odin3_node_id node) {
    char id[NODE_ID_TEXT];
    odin3_bytes label = cell_label(wr, node, id);
    vw_puts(wr, "  // ");
    vw_puts(wr, node_def(wr, node)->name);
    vw_char(wr, ' ');
    for (size_t i = 0; i < label.len; i++) {
        char chr = ((const char *)label.ptr)[i];
        if ((unsigned char)chr < FIRST_PRINTABLE) {
            chr = '?';
        }
        vw_char(wr, chr);
    }
    vw_line_end(wr, "\n");
}

/* `<kw> (` + `!` when active low + the control pin `)` ; port is a one-bit port. */
static void put_ctl_cond(verilog_writer *wr, odin3_node_id node, int port, bool positive) {
    vw_puts(wr, positive ? "if (" : "if (!");
    put_port(wr, node, (uint32_t)port, false);
    vw_puts(wr, ") ");
}

static void put_ffx_edge(verilog_writer *wr, odin3_node_id node, int port, bool positive) {
    vw_puts(wr, positive ? "posedge " : "negedge ");
    put_port(wr, node, (uint32_t)port, false);
}

/* The reset value: a word cell's parameter, a gate-level cell's one bit. */
static void put_ffx_reset_value(verilog_writer *wr, const vw_ffx *ffx) {
    if (ffx->rv != NULL) {
        put_bits(wr, ffx->rv->bits, ffx->rv->len);
    } else {
        vw_puts(wr, ffx->rbit == 1 ? "1'b1" : "1'b0");
    }
}

/* `always @(posedge clk, <edge of each async control>) ` or `always @* `. */
static void put_ffx_sens(verilog_writer *wr, odin3_node_id node, const vw_ffx *ffx) {
    if (ffx->latch) {
        vw_puts(wr, "  always @* ");
        return;
    }
    vw_puts(wr, "  always @(");
    put_ffx_edge(wr, node, ffx->clk, ffx->clk_pos);
    if (ffx->arst >= 0) {
        vw_puts(wr, ", ");
        put_ffx_edge(wr, node, ffx->arst, ffx->arst_pos);
    }
    if (ffx->aload >= 0) {
        vw_puts(wr, ", ");
        put_ffx_edge(wr, node, ffx->aload, ffx->aload_pos);
    }
    vw_puts(wr, ") ");
}

/* `if (<reset>) <q> <= <value>; else ` */
static void put_ffx_reset(verilog_writer *wr, odin3_node_id node, const vw_ffx *ffx, int port) {
    put_ctl_cond(wr, node, port, port == ffx->arst ? ffx->arst_pos : ffx->srst_pos);
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, " <= ");
    put_ffx_reset_value(wr, ffx);
    vw_puts(wr, "; else ");
}

/* One always block for a word-wide (or one-bit gate-level) cell:
 * [async reset | async load] [sync reset] [enable] [sync reset] q <= d. */
static void write_ffx_word(verilog_writer *wr, odin3_node_id node, const vw_ffx *ffx) {
    put_ffx_sens(wr, node, ffx);
    if (ffx->arst >= 0) {
        put_ffx_reset(wr, node, ffx, ffx->arst);
    }
    if (ffx->aload >= 0) {
        put_ctl_cond(wr, node, ffx->aload, ffx->aload_pos);
        vw_name(wr, OBJ_NODE, node.v);
        vw_puts(wr, " <= ");
        put_port(wr, node, (uint32_t)ffx->ad, false);
        vw_puts(wr, "; else ");
    }
    if (ffx->srst >= 0 && ffx->srst_first) {
        put_ffx_reset(wr, node, ffx, ffx->srst);
    }
    if (ffx->en >= 0) {
        put_ctl_cond(wr, node, ffx->en, ffx->en_pos);
    }
    if (ffx->srst >= 0 && !ffx->srst_first) {
        put_ffx_reset(wr, node, ffx, ffx->srst);
    }
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, " <= ");
    put_port(wr, node, (uint32_t)ffx->d, false);
    end_stmt(wr, node);
}

/* `if (<pin>) q[bit] <= 1'bV; else ` for the set or clear input of one bit. */
static void put_ffx_bit_set(verilog_writer *wr, odin3_node_id node, int port, uint32_t bit,
                            bool positive) {
    odin3_pin_id pin = {odin3_node_port(wr->mod.module, node, (uint32_t)port).first.v + bit};
    vw_puts(wr, positive ? "if (" : "if (!");
    put_pin(wr, pin);
    vw_puts(wr, ") ");
    vw_name(wr, OBJ_NODE, node.v);
    vw_char(wr, '[');
    vw_u32(wr, bit);
    vw_puts(wr, "] <= ");
}

/* $dffsr, $dffsre, $dlatchsr, $sr: one always block per bit, clear beats set beats data. */
static void write_ffx_bits(verilog_writer *wr, odin3_node_id node, const vw_ffx *ffx) {
    for (uint32_t bit = 0; bit < ffx->width; bit++) {
        if (ffx->latch) {
            vw_puts(wr, "  always @* ");
        } else {
            vw_puts(wr, "  always @(");
            put_ffx_edge(wr, node, ffx->clk, ffx->clk_pos);
            vw_puts(wr, ", ");
            vw_puts(wr, ffx->set_pos ? "posedge " : "negedge ");
            put_pin(wr,
                    (odin3_pin_id){
                        odin3_node_port(wr->mod.module, node, (uint32_t)ffx->set).first.v + bit});
            vw_puts(wr, ", ");
            vw_puts(wr, ffx->clr_pos ? "posedge " : "negedge ");
            put_pin(wr,
                    (odin3_pin_id){
                        odin3_node_port(wr->mod.module, node, (uint32_t)ffx->clr).first.v + bit});
            vw_puts(wr, ") ");
        }
        put_ffx_bit_set(wr, node, ffx->clr, bit, ffx->clr_pos);
        vw_puts(wr, "1'b0; else ");
        put_ffx_bit_set(wr, node, ffx->set, bit, ffx->set_pos);
        vw_puts(wr, "1'b1;");
        if (ffx->d >= 0) {
            vw_puts(wr, " else ");
            if (ffx->en >= 0) {
                put_ctl_cond(wr, node, ffx->en, ffx->en_pos);
            }
            vw_name(wr, OBJ_NODE, node.v);
            vw_char(wr, '[');
            vw_u32(wr, bit);
            vw_puts(wr, "] <= ");
            put_pin(wr, (odin3_pin_id){
                            odin3_node_port(wr->mod.module, node, (uint32_t)ffx->d).first.v + bit});
            vw_char(wr, ';');
        }
        vw_eol(wr, odin3_node_prov(wr->mod.module, node));
    }
}

/* Quartus mode: any flip-flop or latch type the IR holds, as plain `always` blocks on a reg. */
static void write_ffx(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    vw_ffx ffx;
    if (!ffx_describe(wr, node, &ffx)) {
        return;
    }
    if (ffx.per_bit) {
        write_ffx_bits(wr, node, &ffx);
    } else {
        write_ffx_word(wr, node, &ffx);
    }
    int init = param_index(node_def(wr, node), "INIT");
    int64_t init_val = init >= 0 ? param_int(wr, node, (uint32_t)init) : 2;
    if (init_val == 0 || init_val == 1) {
        vw_puts(wr, "  initial ");
        vw_name(wr, OBJ_NODE, node.v);
        vw_puts(wr, init_val == 0 ? " = 1'b0;\n" : " = 1'b1;\n");
    } else {
        write_word_init(wr, node);
    }
    connect_reg(wr, node, false);
}

/* --- Quartus mode: $mem as a behavioural reg array ------------------------------------------ */

typedef enum vw_mem_param {
    MEM_P_MEMID = 0,
    MEM_P_SIZE,
    MEM_P_OFFSET,
    MEM_P_ABITS,
    MEM_P_WIDTH,
    MEM_P_RD_PORTS,
    MEM_P_WR_PORTS,
    MEM_P_RD_CLK_ENABLE,
    MEM_P_RD_CLK_POLARITY,
    MEM_P_RD_TRANSPARENT,
    MEM_P_WR_CLK_ENABLE,
    MEM_P_WR_CLK_POLARITY,
    MEM_P_INIT
} vw_mem_param;

typedef enum vw_mem_port {
    MEMP_RD_CLK = 0,
    MEMP_RD_EN,
    MEMP_RD_ADDR,
    MEMP_RD_DATA,
    MEMP_WR_CLK,
    MEMP_WR_EN,
    MEMP_WR_ADDR,
    MEMP_WR_DATA
} vw_mem_port;

enum { MEM_PORTS_MAX = 256 };

typedef struct vw_mem {
    odin3_node_id node;
    int64_t size, offset;
    uint32_t abits, width, rd, wr;
} vw_mem;

static vw_mem mem_describe(const verilog_writer *wr, odin3_node_id node) {
    return (vw_mem){node,
                    param_int(wr, node, MEM_P_SIZE),
                    param_int(wr, node, MEM_P_OFFSET),
                    (uint32_t)param_int(wr, node, MEM_P_ABITS),
                    (uint32_t)param_int(wr, node, MEM_P_WIDTH),
                    (uint32_t)param_int(wr, node, MEM_P_RD_PORTS),
                    (uint32_t)param_int(wr, node, MEM_P_WR_PORTS)};
}

/* Bit idx of a BITS parameter of the memory (0 when short). */
typedef struct vw_bit_ref {
    vw_mem_param param;
    uint32_t idx;
} vw_bit_ref;

static bool mem_bit(const verilog_writer *wr, odin3_node_id node, vw_bit_ref ref) {
    const odin3_value *val = odin3_node_param(wr->mod.module, node, ref.param);
    return val != NULL && ref.idx < val->len && val->bits[ref.idx] == ODIN3_BIT_1;
}

/* The pins per read or write port of a memory port: 1 (clock, read enable), ABITS (address) or
 * WIDTH (data, write enable). */
static uint32_t mem_stride(const vw_mem *mem, vw_mem_port port) {
    switch (port) {
    case MEMP_RD_ADDR:
    case MEMP_WR_ADDR:
        return mem->abits;
    case MEMP_RD_DATA:
    case MEMP_WR_DATA:
    case MEMP_WR_EN:
        return mem->width;
    case MEMP_RD_CLK:
    case MEMP_RD_EN:
    case MEMP_WR_CLK:
    default:
        return 1;
    }
}

/* The pins of port `port` that belong to read or write port number index. */
typedef struct vw_port_ref {
    vw_mem_port port;
    uint32_t index;
} vw_port_ref;

static odin3_pinslice mem_slice(const verilog_writer *wr, const vw_mem *mem, vw_port_ref ref) {
    odin3_pinslice all = odin3_node_port(wr->mod.module, mem->node, (uint32_t)ref.port);
    uint32_t stride = mem_stride(mem, ref.port);
    return (odin3_pinslice){{all.first.v + ref.index * stride}, stride};
}

/* Pin number index (counted from the first pin) of port `port`. */
static odin3_pin_id mem_pin(const verilog_writer *wr, const vw_mem *mem, vw_mem_port port,
                            uint32_t index) {
    return (odin3_pin_id){odin3_node_port(wr->mod.module, mem->node, (uint32_t)port).first.v +
                          index};
}

static void put_mem_slice(verilog_writer *wr, odin3_pinslice pins) {
    if (pins.count == 0) {
        vw_puts(wr, "1'b0");
        return;
    }
    vw_items items = pin_items(pins);
    put_items(wr, &items, false);
}

static bool mem_pins_driven(const verilog_writer *wr, odin3_pinslice pins) {
    for (uint32_t i = 0; i < pins.count; i++) {
        if (odin3_net_valid(odin3_pin_net(wr->mod.module, (odin3_pin_id){pins.first.v + i}))) {
            return true;
        }
    }
    return false;
}

static bool mem_read_sync(const verilog_writer *wr, const vw_mem *mem, uint32_t idx) {
    return mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_RD_CLK_ENABLE, idx});
}

/* The aux name of the register of read port idx (keyed by the first data pin of the port). */
static uint32_t mem_rd_key(const verilog_writer *wr, const vw_mem *mem, uint32_t idx) {
    return mem_slice(wr, mem, (vw_port_ref){MEMP_RD_DATA, idx}).first.v;
}

static bool mem_read_used(const verilog_writer *wr, const vw_mem *mem, uint32_t idx) {
    return mem_pins_driven(wr, mem_slice(wr, mem, (vw_port_ref){MEMP_RD_DATA, idx}));
}

/* `  // memory ...` plus the declarations: the array, the read registers and the INIT block. */
static void declare_mem(verilog_writer *wr, odin3_node_id node) {
    vw_mem mem = mem_describe(wr, node);
    vw_puts(wr, "  reg [");
    vw_u32(wr, mem.width - 1);
    vw_puts(wr, ":0] ");
    vw_name(wr, OBJ_NODE, node.v);
    vw_puts(wr, " [");
    vw_i64(wr, mem.offset);
    vw_char(wr, ':');
    vw_i64(wr, mem.offset + mem.size - 1);
    vw_line_end(wr, "];\n");
    for (uint32_t idx = 0; idx < mem.rd; idx++) {
        if (mem_read_sync(wr, &mem, idx) && mem_read_used(wr, &mem, idx)) {
            vw_puts(wr, "  reg [");
            vw_u32(wr, mem.width - 1);
            vw_puts(wr, ":0] ");
            vw_name(wr, OBJ_AUX, mem_rd_key(wr, &mem, idx));
            vw_line_end(wr, ";\n");
        }
    }
}

/* `initial` words for the INIT bits that are not all x. */
static void write_mem_init(verilog_writer *wr, const vw_mem *mem) {
    const odin3_value *init = odin3_node_param(wr->mod.module, mem->node, MEM_P_INIT);
    if (init == NULL || init->len == 0) {
        return;
    }
    for (int64_t word = 0; word < mem->size; word++) {
        uint64_t base = (uint64_t)word * mem->width;
        bool known = false;
        for (uint32_t bit = 0; bit < mem->width && base + bit < init->len; bit++) {
            known = known || (init->bits[base + bit] & 3U) < ODIN3_BIT_X;
        }
        if (known) {
            vw_puts(wr, "  initial ");
            vw_name(wr, OBJ_NODE, mem->node.v);
            vw_char(wr, '[');
            vw_i64(wr, mem->offset + word);
            vw_puts(wr, "] = ");
            uint32_t avail = base < init->len ? (uint32_t)(init->len - base) : 0;
            put_bits(wr, init->bits + base, avail < mem->width ? avail : mem->width);
            vw_line_end(wr, ";\n");
        }
    }
}

/* What a store writes into: the array word at the write address, or a sync read register. */
typedef struct vw_store {
    bool to_array;
    uint32_t read_port;
} vw_store;

typedef struct vw_pin_pair {
    odin3_pin_id first, second;
} vw_pin_pair;

/* True when both pins are on the same net. */
static bool same_net(const verilog_writer *wr, vw_pin_pair pair) {
    return odin3_pin_net(wr->mod.module, pair.first).v ==
           odin3_pin_net(wr->mod.module, pair.second).v;
}

/* The stores of write port wport: one `if (<enable>) target[hi:lo] <= data[hi:lo];` per run of data
 * bits with the same enable net. Into a read register (the transparent bypass) the store also
 * needs the write address to equal the read address. */
static void write_mem_stores(verilog_writer *wr, const vw_mem *mem, uint32_t wport,
                             const vw_store *st) {
    odin3_pinslice waddr = mem_slice(wr, mem, (vw_port_ref){MEMP_WR_ADDR, wport});
    odin3_pinslice raddr = mem_slice(wr, mem, (vw_port_ref){MEMP_RD_ADDR, st->read_port});
    uint32_t bit = 0;
    while (bit < mem->width) {
        odin3_pin_id en = mem_pin(wr, mem, MEMP_WR_EN, wport * mem->width + bit);
        uint32_t end = bit + 1;
        while (end < mem->width &&
               same_net(
                   wr, (vw_pin_pair){en, mem_pin(wr, mem, MEMP_WR_EN, wport * mem->width + end)})) {
            end++;
        }
        vw_puts(wr, "    if (");
        put_pin(wr, en);
        if (!st->to_array) {
            vw_puts(wr, " && (");
            put_mem_slice(wr, waddr);
            vw_puts(wr, " == ");
            put_mem_slice(wr, raddr);
            vw_char(wr, ')');
        }
        vw_puts(wr, ") ");
        if (st->to_array) {
            vw_name(wr, OBJ_NODE, mem->node.v);
            vw_char(wr, '[');
            put_mem_slice(wr, waddr);
            vw_char(wr, ']');
        } else {
            vw_name(wr, OBJ_AUX, mem_rd_key(wr, mem, st->read_port));
        }
        if (bit != 0 || end != mem->width) {
            vw_char(wr, '[');
            vw_u32(wr, end - 1);
            if (end - 1 != bit) {
                vw_char(wr, ':');
                vw_u32(wr, bit);
            }
            vw_char(wr, ']');
        }
        vw_puts(wr, " <= ");
        put_mem_slice(wr, (odin3_pinslice){mem_pin(wr, mem, MEMP_WR_DATA, wport * mem->width + bit),
                                           end - bit});
        vw_line_end(wr, ";\n");
        bit = end;
    }
}

/* The `always @(<edge> clk)` header of a clocked port: bit idx of the clock polarity parameter. */
static void put_mem_clock(verilog_writer *wr, const vw_mem *mem, uint32_t port, uint32_t idx,
                          uint32_t polarity_param) {
    vw_puts(wr, "  always @(");
    vw_puts(wr,
            mem_bit(wr, mem->node, (vw_bit_ref){polarity_param, idx}) ? "posedge " : "negedge ");
    put_pin(wr, mem_pin(wr, mem, port, idx));
    vw_puts(wr, ") begin\n");
}

/* True when write ports a and b are clocked by the same net and polarity. */
typedef struct vw_port_pair {
    uint32_t first, second;
} vw_port_pair;

static bool same_write_clock(const verilog_writer *wr, const vw_mem *mem, vw_port_pair pair) {
    return mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_ENABLE, pair.second}) &&
           mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_POLARITY, pair.first}) ==
               mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_POLARITY, pair.second}) &&
           same_net(wr, (vw_pin_pair){mem_pin(wr, mem, MEMP_WR_CLK, pair.first),
                                      mem_pin(wr, mem, MEMP_WR_CLK, pair.second)});
}

/* One always block for write port wport and the later ports on its clock (a later port wins on one
 * address, as in the IR); marks them done. An unclocked write port is a level-sensitive block. */
static void write_mem_write_block(verilog_writer *wr, const vw_mem *mem, uint32_t wport,
                                  bool *done) {
    bool clocked = mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_ENABLE, wport});
    if (clocked) {
        put_mem_clock(wr, mem, MEMP_WR_CLK, wport, MEM_P_WR_CLK_POLARITY);
    } else {
        vw_puts(wr, "  always @* begin\n");
    }
    for (uint32_t other = wport; other < mem->wr; other++) {
        if (other == wport ||
            (clocked && same_write_clock(wr, mem, (vw_port_pair){wport, other}))) {
            done[other] = true;
            write_mem_stores(wr, mem, other, &(vw_store){true, 0});
        }
    }
    vw_line_end(wr, "  end\n");
}

/* One clocked always block per distinct write clock. */
static void write_mem_writes(verilog_writer *wr, const vw_mem *mem) {
    bool done[MEM_PORTS_MAX] = {false};
    if (mem->wr > MEM_PORTS_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "%s: module '%s': a memory with more than %d write ports",
                  wr->path, wr->mod.name != NULL ? wr->mod.name : "", MEM_PORTS_MAX);
        (void)vw_fail(wr, ODIN3_ERR_INVALID_ARG);
        return;
    }
    for (uint32_t wport = 0; wport < mem->wr; wport++) {
        if (!done[wport]) {
            write_mem_write_block(wr, mem, wport, done);
        }
    }
}

/* Read port idx: `assign data = mem[addr];` (async) or a clocked register (sync). A transparent
 * sync port also takes the data of the write ports of its clock that hit its address. */
static void write_mem_read(verilog_writer *wr, const vw_mem *mem, uint32_t idx) {
    odin3_pinslice data = mem_slice(wr, mem, (vw_port_ref){MEMP_RD_DATA, idx});
    odin3_pinslice addr = mem_slice(wr, mem, (vw_port_ref){MEMP_RD_ADDR, idx});
    bool sync = mem_read_sync(wr, mem, idx);
    odin3_pin_id en = mem_pin(wr, mem, MEMP_RD_EN, idx);
    bool gated = sync && odin3_net_valid(odin3_pin_net(wr->mod.module, en));
    if (sync) {
        put_mem_clock(wr, mem, MEMP_RD_CLK, idx, MEM_P_RD_CLK_POLARITY);
        if (gated) {
            vw_puts(wr, "    if (");
            put_pin(wr, en);
            vw_puts(wr, ") begin\n");
        }
        vw_puts(wr, "    ");
        vw_name(wr, OBJ_AUX, mem_rd_key(wr, mem, idx));
        vw_puts(wr, " <= ");
    } else {
        vw_puts(wr, "  assign ");
        put_mem_slice(wr, data);
        vw_puts(wr, " = ");
    }
    vw_name(wr, OBJ_NODE, mem->node.v);
    vw_char(wr, '[');
    put_mem_slice(wr, addr);
    vw_line_end(wr, "];\n");
    if (!sync) {
        return;
    }
    for (uint32_t wport = 0;
         wport < mem->wr && mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_RD_TRANSPARENT, idx});
         wport++) {
        bool same = mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_ENABLE, wport}) &&
                    mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_WR_CLK_POLARITY, wport}) ==
                        mem_bit(wr, mem->node, (vw_bit_ref){MEM_P_RD_CLK_POLARITY, idx}) &&
                    same_net(wr, (vw_pin_pair){mem_pin(wr, mem, MEMP_WR_CLK, wport),
                                               mem_pin(wr, mem, MEMP_RD_CLK, idx)});
        if (same) {
            write_mem_stores(wr, mem, wport, &(vw_store){false, idx});
        }
    }
    vw_line_end(wr, gated ? "    end\n  end\n" : "  end\n");
    vw_puts(wr, "  assign ");
    put_mem_slice(wr, data);
    vw_puts(wr, " = ");
    vw_name(wr, OBJ_AUX, mem_rd_key(wr, mem, idx));
    vw_line_end(wr, ";\n");
}

/* Names the registers of the synchronous read ports of a `$mem` (Quartus mode). */
static void name_mem_regs(verilog_writer *wr, odin3_node_id node) {
    vw_mem mem = mem_describe(wr, node);
    for (uint32_t idx = 0; idx < mem.rd; idx++) {
        if (mem_read_sync(wr, &mem, idx) && mem_read_used(wr, &mem, idx)) {
            generate(wr, OBJ_AUX, mem_rd_key(wr, &mem, idx), "$mem");
        }
    }
}

/* A `$mem` in Quartus mode: array stores, then the read ports. */
static void write_memq(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    (void)bi;
    vw_mem mem = mem_describe(wr, node);
    write_mem_init(wr, &mem);
    write_mem_writes(wr, &mem);
    for (uint32_t idx = 0; idx < mem.rd; idx++) {
        if (mem_read_used(wr, &mem, idx)) {
            write_mem_read(wr, &mem, idx);
        }
    }
    uint32_t sync = 0;
    for (uint32_t idx = 0; idx < mem.rd; idx++) {
        sync += mem_read_sync(wr, &mem, idx) ? 1U : 0U;
    }
    const odin3_value *id = odin3_node_param(wr->mod.module, node, MEM_P_MEMID);
    odin3_log(ODIN3_LOG_INFO,
              "write_verilog: memory '%s' (%lld x %u): behavioural reg array, %u read ports "
              "(%u synchronous), %u write ports",
              id != NULL ? odin3_strtab_get(wr->tab, id->str) : "?", (long long)mem.size, mem.width,
              mem.rd, sync, mem.wr);
}

typedef void (*vw_cell_writer)(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi);

static const vw_cell_writer CELL_WRITERS[FORM_COUNT] = {
    [FORM_CONST] = write_const, [FORM_GATE1] = write_gate1,   [FORM_GATE2] = write_gate2,
    [FORM_MUX] = write_mux,     [FORM_SOP] = write_sop,       [FORM_LATCH] = write_latch,
    [FORM_LUT] = write_lut,     [FORM_BINARY] = write_binary, [FORM_UNARY] = write_unary,
    [FORM_PMUX] = write_pmux,   [FORM_TRIBUF] = write_tribuf, [FORM_FF] = write_ff,
    [FORM_FFX] = write_ffx,     [FORM_MEMQ] = write_memq,     [FORM_SHIFTX] = write_shiftx,
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
    case ODIN3_VAL_PROC:
        /* A $proc BODY has no Verilog form; the writer refuses processes before reaching here. */
        if (wr->st == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s: module '%s': a process body has no Verilog form; run proc first",
                      wr->path, wr->mod.name != NULL ? wr->mod.name : "");
        }
        (void)vw_fail(wr, ODIN3_ERR_INVALID_ARG);
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

/* What surrounds `(* … *)`: lead before it, after behind it; src is the provenance whose HDL
 * locations are written as the `src` attribute (spec 5.4 W-6), 0 for none. */
typedef struct vw_attr_frame {
    const char *lead;
    const char *after;
    odin3_prov_id src;
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
    if (attr.instance_param && wr->bb_params) {
        return wr->st; /* written as `#(.NAME(value))` by put_bb_overrides */
    }
    if (attr.instance_param) {
        /* An instance's parameters stay attributes under their full name: the instantiated module
         * is already specialised (it declares no parameter to override). */
        attr.role = ODIN3_WATTR_ATTRIBUTE;
        attr.key =
            (odin3_bytes){odin3_strtab_get(wr->tab, key_str), odin3_strtab_len(wr->tab, key_str)};
    }
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

/* `src = "SRC"` after the user's attributes, from the HDL locations of prov (Yosys's form, up to 8
 * joined by `|`); a user `src` attribute already written wins, and an object with no HDL location
 * gets none. */
static void put_src_attr(vw_attrs *ctx, odin3_prov_id prov) {
    verilog_writer *wr = ctx->wr;
    odin3_hdl_locs_out locs;
    uint32_t written = 0;
    bool fresh = false;
    if (!odin3_prov_valid(prov) || wr->st != ODIN3_OK) {
        return;
    }
    odin3_status st = odin3_hdl_locs(wr->hdl, prov, &locs);
    odin3_hdl_print_req req = {&locs, false};
    st = st == ODIN3_OK && locs.n > 0 ? odin3_hdl_src_text(wr->hdl, &req, &wr->src, &written) : st;
    odin3_wattr_claim_req claim_req = {ODIN3_WATTR_ATTRIBUTE, odin3_bytes_cstr("src")};
    st = st == ODIN3_OK && written > 0 ? odin3_wattr_claim(&wr->seen, claim_req, &fresh) : st;
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    } else if (fresh) {
        vw_puts(wr, ctx->any ? ", " : ctx->lead);
        vw_puts(wr, ctx->any ? "" : "(* ");
        ctx->any = true;
        vw_puts(wr, "src = ");
        put_string(wr, (odin3_bytes){wr->src.data, wr->src.len}, 0);
    }
}

/* `lead(* a = v, … *)after` when obj has attributes (odin3_attr_foreach order) or an HDL location
 * to write as `src`, else nothing. */
static void put_attrs(verilog_writer *wr, odin3_objref obj, vw_attr_frame frame) {
    vw_attrs ctx = {wr, frame.lead, false};
    odin3_wattr_seen_clear(&wr->seen);
    odin3_status st = odin3_attr_foreach(wr->mod.module, obj, attr_visit, &ctx);
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    }
    put_src_attr(&ctx, frame.src);
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

/* True for an implicit black box (--auto-blackbox): no declared parameters, the overrides of the
 * instance are its `parameter:<NAME>` attributes. */
static bool is_implicit_bbox(const odin3_celltype_def *def) {
    return def->gran == ODIN3_GRAN_BLACKBOX && def->n_params == 0;
}

enum { BB_INT_BITS = 32 }; /* a Verilog integer parameter */

typedef struct vw_bb_ctx {
    verilog_writer *wr;
    bool any;
} vw_bb_ctx;

/* A parameter value of an implicit black box: a 32-bit value without x/z bits is the signed
 * decimal integer it was written as, so that `.lpm_width(8)` stays readable; other widths keep
 * their sized binary form. */
static void put_bb_value(verilog_writer *wr, const odin3_value *val) {
    uint32_t num = 0;
    bool plain = val->kind == ODIN3_VAL_BITS && val->len == BB_INT_BITS;
    for (uint32_t i = 0; plain && i < val->len; i++) {
        plain = val->bits[i] <= ODIN3_BIT_1;
        num |= (uint32_t)(val->bits[i] == ODIN3_BIT_1) << i;
    }
    if (plain) {
        vw_i64(wr, (int64_t)(int32_t)num); /* a 32-bit integer parameter is signed */
    } else {
        put_value(wr, val);
    }
}

static odin3_status bb_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    vw_bb_ctx *bc = ctx;
    verilog_writer *wr = bc->wr;
    odin3_wattr attr = odin3_wattr_classify(wr->tab, key_str, value);
    if (!attr.instance_param || wr->st != ODIN3_OK) {
        return wr->st;
    }
    const size_t prefix = strlen(ODIN3_ATTR_PARAM_PREFIX);
    odin3_bytes full = {odin3_strtab_get(wr->tab, key_str), odin3_strtab_len(wr->tab, key_str)};
    vw_puts(wr, bc->any ? ", ." : " #(.");
    bc->any = true;
    vw_ident(wr, (odin3_bytes){(const char *)full.ptr + prefix, full.len - prefix},
             "parameter name");
    vw_char(wr, '(');
    put_bb_value(wr, value);
    vw_char(wr, ')');
    return wr->st;
}

/* ` #(.NAME(value), …)` of an implicit black box from its `parameter:<NAME>` attributes. */
static void put_bb_overrides(verilog_writer *wr, odin3_node_id node) {
    vw_bb_ctx ctx = {wr, false};
    odin3_status st =
        odin3_attr_foreach(wr->mod.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, bb_visit, &ctx);
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    }
    vw_puts(wr, ctx.any ? ")" : "");
}

/* Quartus mode, black box: a port every bit of which is the constant x (the elaborator ties an
 * unconnected input to x) is left open, as in the source: Quartus refuses a constant on an output
 * and takes the megafunction's default for an open input. */
static bool port_all_x(const verilog_writer *wr, odin3_pinslice pins) {
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_net_id net = odin3_pin_net(wr->mod.module, (odin3_pin_id){pins.first.v + i});
        if (!odin3_net_valid(net) || net_ref(wr, net).kind != REF_CONST ||
            net_ref(wr, net).bit != 'x') {
            return false;
        }
    }
    return pins.count > 0;
}

/* --- ROM contents ---------------------------------------------------------------------------- */

/* The int parameter `name` of an altsyncram node, 0 when the type has none. */
static int64_t int_param(const verilog_writer *wr, odin3_node_id node,
                         const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return odin3_node_param(wr->mod.module, node, i)->i;
        }
    }
    return 0;
}

/* The ODIN3_ATTR_ROM_INIT text of node, else NULL. */
static const odin3_value *rom_init_of(const verilog_writer *wr, odin3_node_id node,
                                      const odin3_celltype_def *def) {
    uint32_t key = 0;
    if (strcmp(def->name, "altsyncram") != 0 ||
        !odin3_strtab_find(wr->tab, odin3_bytes_cstr(ODIN3_ATTR_ROM_INIT), &key)) {
        return NULL;
    }
    const odin3_value *val =
        odin3_attr_get(wr->mod.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
    return val != NULL && val->kind == ODIN3_VAL_STRING ? val : NULL;
}

/* The `.mif` path of ROM number n: the netlist path minus `.v`, `_rom<n>.mif`. */
static odin3_status mif_path(const verilog_writer *wr, uint32_t n, odin3_strbuf *out) {
    size_t len = strlen(wr->path);
    bool dot_v = len > 2 && strcmp(wr->path + len - 2, ".v") == 0;
    odin3_status st = odin3_strbuf_append(out, (odin3_bytes){wr->path, dot_v ? len - 2 : len});
    return st == ODIN3_OK ? odin3_strbuf_appendf(out, "_rom%" PRIu32 ".mif", n) : st;
}

/* Writes the contents of one ROM as a Quartus Memory Initialization File and returns the file's
 * name without its directory in *base (into path's buffer); the sticky status on failure. */
static odin3_status write_mif(verilog_writer *wr, odin3_node_id node, const odin3_celltype_def *def,
                              const odin3_value *init, odin3_strbuf *path) {
    if (strcmp(wr->path, "-") == 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_verilog: a ROM needs an initialisation file; "
                                   "writing to standard output has none");
        return vw_fail(wr, ODIN3_ERR_INVALID_ARG);
    }
    int64_t width = int_param(wr, node, def, "width_a");
    int64_t depth = int_param(wr, node, def, "numwords_a");
    size_t digits = (size_t)((width + 3) / 4);
    odin3_bytes text = str_bytes(wr, init->str);
    if (width <= 0 || depth <= 0 || text.len != digits * (size_t)depth) {
        odin3_log(ODIN3_LOG_ERROR,
                  "%s: ROM contents do not fit width_a %" PRId64 " and numwords_a %" PRId64,
                  wr->path, width, depth);
        return vw_fail(wr, ODIN3_ERR_INVALID_ARG);
    }
    odin3_status st = mif_path(wr, wr->n_rom, path);
    odin3_atomic_file file = {0};
    st = st == ODIN3_OK ? odin3_atomic_file_open(&file, path->data) : st;
    if (st != ODIN3_OK) {
        return vw_fail(wr, st);
    }
    (void)fprintf(file.fp,
                  "-- Odin III ROM contents (write_verilog)\nDEPTH = %" PRId64 ";\nWIDTH = %" PRId64
                  ";\nADDRESS_RADIX = HEX;\nDATA_RADIX = HEX;\nCONTENT BEGIN\n",
                  depth, width);
    for (int64_t word = 0; word < depth; word++) {
        (void)fprintf(file.fp, "  %" PRIx64 " : %.*s;\n", (uint64_t)word, (int)digits,
                      (const char *)text.ptr + (size_t)word * digits);
    }
    (void)fputs("END;\n", file.fp);
    return vw_fail(wr, odin3_atomic_file_close(&file, ODIN3_OK));
}

/* The init_file value of a ROM: writes its `.mif` and puts the file's name (no directory) in
 * quotes. */
static void put_rom_file(verilog_writer *wr, odin3_node_id node, const odin3_celltype_def *def,
                         const odin3_value *init) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    if (wr->st == ODIN3_OK && write_mif(wr, node, def, init, &path) == ODIN3_OK) {
        const char *base = strrchr(path.data, '/');
        base = base != NULL ? base + 1 : path.data;
        put_string(wr, odin3_bytes_cstr(base), 0);
        odin3_log(ODIN3_LOG_INFO, "write_verilog: ROM contents written to %s", path.data);
        wr->n_rom++;
    }
    odin3_strbuf_free(&path);
}

/* ` #(.P(v), …)` with every parameter, for any type but a module. */
static void put_overrides(verilog_writer *wr, odin3_node_id node, const odin3_celltype_def *def) {
    if (is_implicit_bbox(def)) {
        put_bb_overrides(wr, node);
        return;
    }
    if (def->gran == ODIN3_GRAN_MODULE || def->n_params == 0) {
        return;
    }
    const odin3_value *rom = rom_init_of(wr, node, def);
    vw_puts(wr, " #(");
    for (uint32_t i = 0; i < def->n_params; i++) {
        vw_puts(wr, i > 0 ? ", ." : ".");
        vw_ident(wr, odin3_bytes_cstr(def->params[i].name), "parameter name");
        vw_char(wr, '(');
        if (rom != NULL && strcmp(def->params[i].name, "init_file") == 0) {
            put_rom_file(wr, node, def, rom);
        } else {
            put_value(wr, odin3_node_param(wr->mod.module, node, i));
        }
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
        bool open = port_open(wr, pins) ||
                    (wr->quartus && def->gran == ODIN3_GRAN_BLACKBOX && port_all_x(wr, pins));
        if (!open) {
            vw_items items = pin_items(pins);
            put_items(wr, &items, false);
        }
        vw_line_end(wr, port + 1 < def->n_ports ? "),\n" : ")\n");
    }
    vw_line_end(wr, "  );\n");
}

/* A constant cell written as a literal wherever its net is read (no statement of its own). */
static bool const_omitted(const verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    return bi->form == FORM_CONST &&
           net_ref(wr, odin3_pin_net(wr->mod.module, out_port(wr, node).first)).kind == REF_CONST;
}

/* The comment and attributes before a built-in cell's statement(s); Icarus rejects attributes on a
 * continuous assign: there they are a comment. */
static void put_cell_head(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    bool always = bi->form == FORM_LATCH || bi->form == FORM_FF || bi->form == FORM_FFX;
    if (wr->quartus && (always || bi->form == FORM_MEMQ)) {
        put_cell_comment(wr, node);
    }
    odin3_prov_id src = always ? odin3_node_prov(wr->mod.module, node) : (odin3_prov_id){0};
    put_attrs(wr, (odin3_objref){ODIN3_OBJ_NODE, node.v},
              (vw_attr_frame){always ? "  " : "  // ", "\n", src});
}

/* The cell's attributes on a line of their own (a comment before an `assign`), then its
 * statement(s). */
static void write_cell(verilog_writer *wr, odin3_node_id node) {
    const vw_builtin *bi = node_builtin(wr, node);
    odin3_objref obj = {ODIN3_OBJ_NODE, node.v};
    if (bi == NULL) {
        wr->bb_params = is_implicit_bbox(node_def(wr, node));
        put_attrs(wr, obj, (vw_attr_frame){"  ", "\n", odin3_node_prov(wr->mod.module, node)});
        wr->bb_params = false;
        write_instance(wr, node);
    } else if (has_output(wr, node)) {
        if (!const_omitted(wr, node, bi)) {
            put_cell_head(wr, node, bi);
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
              (vw_attr_frame){"", "\n", odin3_module_prov(module)});
    vw_puts(wr, "module ");
    vw_ident(wr, odin3_bytes_cstr(wr->mod.name), "module name cannot be written in Verilog:");
    vw_puts(wr, ports == 0 ? ";" : " (");
    vw_eol(wr, odin3_module_prov(module));
    for (uint32_t i = 0; i < ports; i++) {
        odin3_wire_id wire = odin3_module_port_wire(module, i);
        vw_puts(wr, "  ");
        put_attrs(wr, (odin3_objref){ODIN3_OBJ_WIRE, wire.v},
                  (vw_attr_frame){"", " ", odin3_wire_prov(module, wire)});
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
            put_attrs(wr, (odin3_objref){ODIN3_OBJ_WIRE, i},
                      (vw_attr_frame){"", " ", odin3_wire_prov(module, wire)});
            odin3_net_id net = odin3_wire_net(module, wire, 0);
            bool reg = wire_scalar(wr, wire) && odin3_net_valid(net) && wr->mod.regwire[net.v] == i;
            vw_puts(wr, reg ? "reg " : "wire ");
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
            put_attrs(wr, (odin3_objref){ODIN3_OBJ_NET, i},
                      (vw_attr_frame){"", " ", odin3_net_prov(module, net)});
            vw_puts(wr, net_ref(wr, net).reg ? "reg " : "wire ");
            vw_name(wr, OBJ_NET, i);
            vw_char(wr, ';');
            vw_eol(wr, odin3_net_prov(module, net));
        }
    }
}

/* The `reg` of a storage cell (a vector for a word flip-flop), or the array of a Quartus-mode
 * memory. */
static void declare_cell_reg(verilog_writer *wr, odin3_node_id node, const vw_builtin *bi) {
    if (bi->form == FORM_MEMQ) {
        declare_mem(wr, node);
        return;
    }
    vw_puts(wr, "  reg ");
    if (bi->form == FORM_FF || bi->form == FORM_FFX) {
        vw_char(wr, '[');
        vw_u32(wr, out_port(wr, node).count - 1);
        vw_puts(wr, ":0] ");
    }
    vw_name(wr, OBJ_NODE, node.v);
    vw_line_end(wr, ";\n");
}

/* Dangling output wires, the regs of storage cells and the global clock. */
static void declare_cell_extras(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t i = 1; i < odin3_module_pin_end(module); i++) {
        if (name_of(wr, OBJ_PIN, i) != 0) {
            vw_puts(wr, "  wire ");
            vw_name(wr, OBJ_PIN, i);
            vw_line_end(wr, ";\n");
        }
    }
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        const vw_builtin *bi = odin3_node_live(module, node) ? node_builtin(wr, node) : NULL;
        if (bi != NULL && name_of(wr, OBJ_NODE, i) != 0) {
            declare_cell_reg(wr, node, bi);
        }
    }
    if (name_of(wr, OBJ_GCLK, 0) != 0) {
        vw_puts(wr, "  (* gclk *) wire ");
        vw_name(wr, OBJ_GCLK, 0);
        vw_line_end(wr, ";\n");
    }
}

static bool canonical(const verilog_writer *wr, odin3_wire_id wire, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(wr->mod.module, wire, bit);
    vw_ref ref = net_ref(wr, net);
    /* the flop's reg wire is connected by the flop (connect_reg), as a bit-level flop's reg is */
    return (ref.kind == REF_WIRE && ref.wire == wire.v && ref.bit == bit) ||
           wr->mod.regwire[net.v] == wire.v;
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
        vw_line_end(wr, ";\n");
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

/* Write order of the cells: 0 all others, 1 storage cells, 2 constants (`assign w = 1'b0;`). */
enum { GROUP_PLAIN = 0, GROUP_STORAGE = 1, GROUP_CONST = 2, GROUP_COUNT = 3 };

static uint32_t cell_group(const verilog_writer *wr, odin3_node_id node) {
    const vw_builtin *bi = node_builtin(wr, node);
    if (bi == NULL) {
        return GROUP_PLAIN;
    }
    if (bi->form == FORM_CONST) {
        return GROUP_CONST;
    }
    return bi->form == FORM_FF || bi->form == FORM_FFX || bi->form == FORM_LATCH ? GROUP_STORAGE
                                                                                 : GROUP_PLAIN;
}

/* The live cells in node order, one group at a time: the others, the storage cells, the constants.
 * A reader makes the cells of the `assign` and instance items first, those of the `always` items
 * (proc) next and the constants last, so a file read and written again keeps its order. */
static void write_cells(verilog_writer *wr) {
    const odin3_module *module = wr->mod.module;
    for (uint32_t group = 0; group < GROUP_COUNT; group++) {
        for (uint32_t i = 1; wr->st == ODIN3_OK && i < odin3_module_node_end(module); i++) {
            odin3_node_id node = {i};
            if (odin3_node_live(module, node) && !is_port_node(wr, node) &&
                cell_group(wr, node) == group) {
                write_cell(wr, node);
            }
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
        write_cells(wr);
        vw_puts(wr, "endmodule\n");
        vw_flush(wr);
    }
    end_module(wr);
    return wr->st;
}

/* --- the modules a file holds --------------------------------------------------------------- */

/* Map of module name (strtab ID) to module ID, built once per file; NULL on out of memory. */
static odin3_u64map *module_names(const verilog_writer *wr) {
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint32_t i = 1; map != NULL && i < odin3_design_module_end(wr->design); i++) {
        const odin3_module *other =
            odin3_module_get((odin3_design *)wr->design, (odin3_module_id){i});
        if (other != NULL &&
            odin3_u64map_put(map, (odin3_kv){odin3_module_name(other), i}) != ODIN3_OK) {
            odin3_u64map_destroy(map);
            map = NULL;
        }
    }
    return map;
}

/* Where an instance of `node` in `module` goes: the module ID it instantiates in `child` (0 when it
 * instantiates none, or when no module of that name exists). Sets *dangling when the cell type is a
 * module type but no module of its name is in the design. */
static uint32_t instance_module(const verilog_writer *wr, const odin3_u64map *names,
                                const odin3_module *module, odin3_node_id node, bool *dangling) {
    const odin3_celltype_def *def = odin3_celltype_get(wr->design, odin3_node_type(module, node));
    uint32_t name = 0;
    uint64_t child = 0;
    if (def == NULL || def->gran != ODIN3_GRAN_MODULE) {
        return 0;
    }
    if (!odin3_strtab_find(wr->tab, odin3_bytes_cstr(def->name), &name) ||
        !odin3_u64map_get(names, name, &child)) {
        const char *inst = odin3_strtab_get(wr->tab, odin3_node_name(module, node));
        odin3_log(ODIN3_LOG_WARN,
                  "verilog_write: instance '%s' has cell type '%s' that no module of the design "
                  "defines; all modules are written",
                  inst != NULL ? inst : "", def->name);
        *dangling = true;
        return 0;
    }
    return (uint32_t)child;
}

/* The work list of mark_reachable: modules to scan, and the flags that rule the walk. */
typedef struct reach {
    const odin3_u64map *names;
    uint8_t *keep;
    uint32_t *work;
    uint32_t depth;
    bool dangling;
} reach;

/* Marks and queues the modules the live nodes of `module` instantiate. */
static void mark_children(const verilog_writer *wr, reach *walk, const odin3_module *module) {
    for (uint32_t i = 1; module != NULL && i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        uint32_t child = odin3_node_live(module, node)
                             ? instance_module(wr, walk->names, module, node, &walk->dangling)
                             : 0;
        if (child != 0 && !walk->keep[child]) {
            walk->keep[child] = 1;
            walk->work[walk->depth++] = child;
        }
    }
}

/* Marks in `keep` (one byte per module ID) the top and the modules it instantiates, directly or
 * not; a module nothing reaches is a leftover (a flattened child) that a reader would not
 * elaborate, so the file does not hold it and write, read, write is idempotent. All stay when no
 * top is set, and when an instance names a module the design lacks (a dangling instance: nothing
 * is dropped silently). */
static odin3_status mark_reachable(const verilog_writer *wr, uint8_t *keep, uint32_t end) {
    uint32_t top = odin3_design_top(wr->design).v;
    odin3_u64map *names = module_names(wr);
    reach walk = {names, keep, odin3_util_calloc(((size_t)end + 1) * sizeof(uint32_t)), 0, false};
    if (walk.work == NULL || names == NULL) {
        odin3_util_free(walk.work);
        odin3_u64map_destroy(names);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (top == 0) {
        memset(keep, 1, end);
    } else {
        keep[top] = 1;
        walk.work[walk.depth++] = top;
    }
    while (walk.depth > 0) {
        mark_children(wr, &walk,
                      odin3_module_get((odin3_design *)wr->design,
                                       (odin3_module_id){walk.work[--walk.depth]}));
    }
    if (walk.dangling) {
        memset(keep, 1, end);
    }
    odin3_u64map_destroy(names);
    odin3_util_free(walk.work);
    return ODIN3_OK;
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
    if (def->width_fn == NULL && def->width_expr == NULL && def->width_param != NULL) {
        vw_char(wr, '[');
        vw_ident(wr, odin3_bytes_cstr(def->width_param), "parameter name");
        vw_puts(wr, "-1:0] ");
    } else if (!def->scalar) {
        uint32_t width = def->width_fn != NULL || def->width_expr != NULL
                             ? default_width(wr, type, port)
                             : def->width;
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

/* One parameter an instance of an implicit black box overrides: the key `parameter:<NAME>` and
 * the first value seen. */
typedef struct vw_bb_param {
    uint32_t key;
    odin3_value value;
} vw_bb_param;

typedef struct vw_bb_collect {
    verilog_writer *wr;
    odin3_vec list; /* of vw_bb_param */
} vw_bb_collect;

static odin3_status bb_collect_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    vw_bb_collect *col = ctx;
    if (!odin3_wattr_classify(col->wr->tab, key_str, value).instance_param) {
        return ODIN3_OK;
    }
    for (size_t i = 0; i < col->list.len; i++) {
        if (((const vw_bb_param *)odin3_vec_cat(&col->list, i))->key == key_str) {
            return ODIN3_OK;
        }
    }
    vw_bb_param *slot = odin3_vec_push(&col->list);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (vw_bb_param){key_str, *value};
    return ODIN3_OK;
}

/* The overrides of every instance of the implicit black box `type`, in the design. */
static void collect_bb_params(verilog_writer *wr, odin3_celltype_id type, vw_bb_collect *col) {
    const uint32_t end = odin3_design_module_end(wr->design);
    for (uint32_t i = 1; i < end && wr->st == ODIN3_OK; i++) {
        const odin3_module *module =
            odin3_module_get((odin3_design *)wr->design, (odin3_module_id){i});
        for (uint32_t pos = 1; pos < odin3_module_node_end(module) && wr->st == ODIN3_OK; pos++) {
            odin3_node_id node = {pos};
            if (odin3_node_live(module, node) && odin3_node_type(module, node).v == type.v) {
                odin3_status st = odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, pos},
                                                     bb_collect_visit, col);
                (void)vw_fail(wr, st);
            }
        }
    }
}

/* ` #(parameter NAME = value, …)` of an implicit black box's stub: the parameters its instances
 * override (the stub has no other source), so that the netlist reads back against it. */
static void write_bb_stub_params(verilog_writer *wr, odin3_celltype_id type) {
    vw_bb_collect col = {wr, {0}};
    odin3_vec_init(&col.list, sizeof(vw_bb_param));
    collect_bb_params(wr, type, &col);
    const size_t prefix = strlen(ODIN3_ATTR_PARAM_PREFIX);
    for (size_t i = 0; i < col.list.len && wr->st == ODIN3_OK; i++) {
        const vw_bb_param *param = odin3_vec_cat(&col.list, i);
        vw_puts(wr, i == 0 ? " #(\n  parameter " : ",\n  parameter ");
        vw_ident(wr,
                 (odin3_bytes){odin3_strtab_get(wr->tab, param->key) + prefix,
                               odin3_strtab_len(wr->tab, param->key) - prefix},
                 "parameter name");
        vw_puts(wr, " = ");
        put_bb_value(wr, &param->value);
    }
    vw_puts(wr, col.list.len > 0 ? "\n)" : "");
    odin3_vec_free(&col.list);
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
    if (is_implicit_bbox(def)) {
        write_bb_stub_params(wr, type);
    } else {
        write_stub_params(wr, def);
    }
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

/* True for the storage cell types written as plain always blocks in Quartus mode. */
static bool quartus_storage(const odin3_celltype_def *def) {
    static const char *const WORD[] = {"$adffe",   "$sdffe",    "$sdffce", "$aldff",
                                       "$aldffe",  "$dffsr",    "$dffsre", "$dlatch",
                                       "$adlatch", "$dlatchsr", "$sr"};
    for (size_t i = 0; i < sizeof WORD / sizeof WORD[0]; i++) {
        if (strcmp(def->name, WORD[i]) == 0) {
            return true;
        }
    }
    vw_ffx probe = {0};
    return ffx_gate(def, &probe);
}

/* Quartus mode: the storage cells and `$mem` take the shared FFX / MEMQ entries. */
static odin3_status register_quartus(verilog_writer *wr) {
    for (uint32_t i = 1; wr->st == ODIN3_OK; i++) {
        odin3_celltype_id type = {i};
        const odin3_celltype_def *def = odin3_celltype_get(wr->design, type);
        if (def == NULL) {
            break;
        }
        uint64_t index = BI_FFX;
        if (strcmp(def->name, "$mem") == 0) {
            index = BI_MEMQ;
        } else if (!quartus_storage(def)) {
            continue;
        }
        if (odin3_u64map_put(wr->builtin, (odin3_kv){type.v, index}) != ODIN3_OK) {
            return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
    }
    return wr->st;
}

static odin3_status wr_init(verilog_writer *wr, const odin3_design *design, const char *path,
                            bool quartus) {
    *wr = (verilog_writer){.design = design,
                           .quartus = quartus,
                           .tab = odin3_design_strtab(design),
                           .path = path,
                           .escaped_end = SIZE_MAX};
    odin3_strbuf_init(&wr->out);
    odin3_strbuf_init(&wr->cand);
    odin3_wattr_seen_init(&wr->seen);
    wr->builtin = odin3_u64map_create(0);
    wr->hdl = odin3_hdl_ctx_create(design, "write_verilog");
    wr->stubbed = odin3_u64map_create(0);
    odin3_strbuf_init(&wr->src);
    odin3_hash_rows_init(&wr->rows);
    if (wr->builtin == NULL || wr->hdl == NULL || wr->stubbed == NULL) {
        return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
    }
    const uint32_t named = quartus ? BI_FFX : BI_QUARTUS_FIRST;
    for (uint32_t i = 0; i < named; i++) {
        uint32_t name = 0;
        odin3_celltype_id type = {0};
        if (odin3_strtab_find(wr->tab, odin3_bytes_cstr(BUILTINS[i].name), &name) &&
            odin3_celltype_find(design, name, &type) &&
            odin3_u64map_put(wr->builtin, (odin3_kv){type.v, i}) != ODIN3_OK) {
            return vw_fail(wr, ODIN3_ERR_NO_MEMORY);
        }
    }
    return quartus ? register_quartus(wr) : ODIN3_OK;
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
    odin3_hdl_ctx_destroy(wr->hdl);
    odin3_strbuf_free(&wr->src);
    odin3_hash_rows_free(&wr->rows);
    odin3_u64map_destroy(wr->stubbed);
}

/* HASH: opens the name map's temporary before the netlist, so a map that cannot be written refuses
 * the write (H-1). */
static odin3_status map_begin(const verilog_writer *wr, const odin3_verilog_opts *opts,
                              odin3_hash_map_file *map) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_hash_map_path(
        &(odin3_hash_map_req){"write_verilog", wr->path, opts->name_map}, &path);
    st = st == ODIN3_OK ? odin3_hash_map_open(map, path.data) : st;
    odin3_strbuf_free(&path);
    return st;
}

/* HASH: closes the netlist and the name map together (the map is renamed first, then the netlist;
 * a failed write removes both). The status is sticky in wr->st. */
static void map_end(verilog_writer *wr, odin3_hash_map_file *map) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = wr->st == ODIN3_OK ? odin3_hash_rows_map_text(&wr->rows, &text) : wr->st;
    odin3_atomic_file *netlist = wr->opened ? &wr->file : NULL;
    wr->opened = false;
    wr->st = odin3_hash_map_close(map, (odin3_bytes){text.data, text.len}, netlist, st);
    odin3_strbuf_free(&text);
}

/* Opens the name map's temporary (HASH), then the netlist's. */
static odin3_status open_outputs(verilog_writer *wr, const odin3_verilog_opts *opts,
                                 odin3_hash_map_file *map) {
    odin3_status st = ODIN3_OK;
    if (wr->name_style == ODIN3_VERILOG_NAMES_HASH) {
        st = vw_fail(wr, map_begin(wr, opts, map));
    }
    return st == ODIN3_OK ? wr_open(wr) : st;
}

/* --prov-map: only with hash names; turns the version 2 map on (and checks what the read kept). */
static odin3_status enable_prov_map(verilog_writer *wr, const odin3_verilog_opts *opts) {
    if (opts == NULL || opts->prov_map == ODIN3_PROV_MAP_NONE) {
        return ODIN3_OK;
    }
    if (wr->name_style != ODIN3_VERILOG_NAMES_HASH) {
        odin3_log(ODIN3_LOG_ERROR, "verilog_write: --prov-map needs --name-style hash");
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_hash_rows_prov_enable(&wr->rows, wr->design, opts->prov_map, "write_verilog");
}

/* Every module the file holds (mark_reachable), in module ID order. */
static odin3_status write_modules(verilog_writer *wr) {
    const uint32_t end = odin3_design_module_end(wr->design);
    uint8_t *keep = odin3_util_calloc((size_t)end + 1);
    odin3_status st =
        vw_fail(wr, keep != NULL ? mark_reachable(wr, keep, end) : ODIN3_ERR_NO_MEMORY);
    for (uint32_t i = 1; st == ODIN3_OK && i < end; i++) {
        st = keep[i] ? write_module(wr, i) : ODIN3_OK;
    }
    odin3_util_free(keep);
    return st;
}

/* The stub file of Quartus mode: the given path, else `<path minus .v>_lpm_sim.v`. */
static odin3_status sim_path(const verilog_writer *wr, const char *given, odin3_strbuf *out) {
    const char *path = wr->path;
    if (given != NULL) {
        return odin3_strbuf_append(out, odin3_bytes_cstr(given));
    }
    size_t len = strlen(path);
    bool dot_v = len > 2 && strcmp(path + len - 2, ".v") == 0;
    odin3_status st = odin3_strbuf_append(out, (odin3_bytes){path, dot_v ? len - 2 : len});
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("_lpm_sim.v")) : st;
}

/* Quartus mode: the black-box stubs, in a file of their own (never read by Quartus). */
static void write_sim_file(verilog_writer *wr, const char *given) {
    odin3_strbuf sim;
    odin3_strbuf_init(&sim);
    const char *main_path = wr->path;
    odin3_status st = sim_path(wr, given, &sim);
    if (st != ODIN3_OK) {
        (void)vw_fail(wr, st);
    } else {
        wr->path = sim.data;
        wr->units = 0;
        if (wr_open(wr) == ODIN3_OK) {
            vw_puts(wr, "// Odin III simulation and read-back stubs (write_verilog --quartus).\n");
            vw_puts(wr,
                    "// Read this file with the netlist for iverilog or Odin III; never add it ");
            vw_puts(wr, "to a Quartus project.\n\n");
            (void)write_stubs(wr);
            vw_flush(wr);
            wr_finish(wr);
        }
        wr->path = main_path;
    }
    odin3_strbuf_free(&sim);
}

/* True when the arguments are usable (logged otherwise). */
static bool write_args_ok(const odin3_design *design, const char *path,
                          const odin3_verilog_opts *opts) {
    if (design == NULL || path == NULL ||
        (opts != NULL && opts->name_style != ODIN3_VERILOG_NAMES_SHORT &&
         opts->name_style != ODIN3_VERILOG_NAMES_PROVENANCE &&
         opts->name_style != ODIN3_VERILOG_NAMES_HASH)) {
        odin3_log(ODIN3_LOG_ERROR, "verilog_write: NULL design or path, or an unknown name style");
        return false;
    }
    if (opts != NULL && opts->quartus && strcmp(path, "-") == 0 && opts->sim_file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "verilog_write: --quartus to stdout needs --sim-file");
        return false;
    }
    if (opts != NULL && opts->quartus && opts->sim_file != NULL &&
        strcmp(opts->sim_file, path) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "verilog_write: --sim-file is the netlist file itself");
        return false;
    }
    return true;
}

/* The netlist (and the name map), then in Quartus mode the stub file. The first failure is sticky
 * in wr->st. */
static void write_files(verilog_writer *wr, const odin3_verilog_opts *opts) {
    odin3_hash_map_file map = {.open = false};
    odin3_status st = vw_fail(wr, enable_prov_map(wr, opts));
    st = st == ODIN3_OK ? open_outputs(wr, opts, &map) : st;
    st = st == ODIN3_OK ? write_modules(wr) : st;
    if (st == ODIN3_OK && !wr->quartus) {
        (void)write_stubs(wr);
    }
    vw_flush(wr);
    if (map.open) {
        map_end(wr, &map);
    } else {
        wr_finish(wr);
    }
    if (wr->quartus && wr->st == ODIN3_OK) {
        write_sim_file(wr, opts->sim_file);
    }
}

odin3_status odin3_verilog_write(const odin3_design *design, const char *path,
                                 const odin3_verilog_opts *opts) {
    if (!write_args_ok(design, path, opts)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_refuse_processes(design, NULL) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG; /* 2D EL-33: before the path is opened */
    }
    verilog_writer wr;
    odin3_status st = wr_init(&wr, design, path, opts != NULL && opts->quartus);
    wr.name_style = opts != NULL ? opts->name_style : ODIN3_VERILOG_NAMES_SHORT;
    if (st == ODIN3_OK) {
        write_files(&wr, opts);
    }
    st = wr.st;
    wr_free(&wr);
    return st;
}
