/*
 * writer.c — the RTLIL writer (2D spec §13, EL-48; plan Task 5). Signals are spelled by
 * odin3_proc_sigspec and processes printed by odin3_proc_print (src/ir/proc.h), so the writer,
 * the body printer and odin3_node_get_param_text(BODY) share one spelling (plan point (s)).
 */
#include "backends/rtlil/writer.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/edit_events.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FLUSH_BYTES = 1 << 20, /* odin3_rtlil_write writes out once this much text is pending */
    CELL_INDENT = 2,       /* module items; cell items are indented once more */
    BRACE_OPEN = 2,        /* "{ " */
    BRACE_CLOSE = 1        /* "}" */
};

typedef struct rtl_writer {
    const odin3_design *design;
    const odin3_strtab *tab;
    odin3_strbuf *out;
    FILE *fp;         /* odin3_rtlil_write: where the text goes; NULL for odin3_rtlil_text */
    const char *path; /* for write errors */
    odin3_status st;  /* sticky: the first failure */
    /* the module being written */
    const odin3_module *module;
    const char *module_name;
    const odin3_celltype_def *module_def; /* its cell type: port directions */
    uint32_t *port_of;                    /* wire ID -> 1-based port index, 0 for none */
    odin3_vec nets;                       /* odin3_net_id scratch: one port or wire */
    odin3_strbuf part;                    /* scratch: one sigspec or MEMID */
} rtl_writer;

/* --- output -------------------------------------------------------------------------------- */

static bool rw_ok(const rtl_writer *rw) {
    return rw->st == ODIN3_OK;
}

static void rw_keep(rtl_writer *rw, odin3_status st) {
    if (rw->st == ODIN3_OK) {
        rw->st = st;
    }
}

static void rw_put(rtl_writer *rw, const char *text) {
    if (rw_ok(rw)) {
        rw->st = odin3_strbuf_append(rw->out, odin3_bytes_cstr(text));
    }
}

static void rw_put_id(rtl_writer *rw, const char *prefix, uint32_t id) {
    if (rw_ok(rw)) {
        rw->st = odin3_strbuf_appendf(rw->out, "%s%u", prefix, id);
    }
}

static void rw_indent(rtl_writer *rw, uint32_t indent) {
    static const char k_blanks[] = "        ";
    for (uint32_t left = indent; rw_ok(rw) && left > 0;) {
        uint32_t step = left < sizeof k_blanks - 1 ? left : (uint32_t)(sizeof k_blanks - 1);
        rw->st = odin3_strbuf_append(rw->out, (odin3_bytes){k_blanks, step});
        left -= step;
    }
}

/* Writes the pending text to the file (odin3_rtlil_write) when at least `least` bytes wait. */
static void rw_flush(rtl_writer *rw, size_t least) {
    if (rw->fp == NULL || !rw_ok(rw) || rw->out->len == 0 || rw->out->len < least) {
        return;
    }
    errno = 0;
    if (fwrite(rw->out->data, 1, rw->out->len, rw->fp) != rw->out->len) {
        odin3_log(ODIN3_LOG_ERROR, "%s: write failed: %s", rw->path,
                  strerror(errno != 0 ? errno : EIO));
        rw->st = ODIN3_ERR_IO;
        return;
    }
    odin3_strbuf_clear(rw->out);
}

/* --- names --------------------------------------------------------------------------------- */

static const char *str_of(const rtl_writer *rw, uint32_t str) {
    return odin3_strtab_get(rw->tab, str);
}

/* True when RTLIL can carry the name: an identifier ends at a blank, tab, CR or LF. */
static bool name_ok(const char *name, size_t len) {
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char chr = name[i];
        if (chr == ' ' || chr == '\t' || chr == '\r' || chr == '\n' || chr == '\0') {
            return false;
        }
    }
    return true;
}

/* Fails with INVALID_ARG (logged) for an object whose name RTLIL cannot carry. */
static void refuse_name(rtl_writer *rw, const char *what, const char *name) {
    if (rw_ok(rw)) {
        odin3_log(ODIN3_LOG_ERROR, "write_rtlil: module '%s': %s '%s' cannot be written in RTLIL",
                  rw->module_name, what, name);
        rw->st = ODIN3_ERR_INVALID_ARG;
    }
}

/* Appends \<name of strtab ID str>, refusing a name RTLIL cannot carry. */
static void rw_name(rtl_writer *rw, const char *what, uint32_t str) {
    const char *name = str_of(rw, str);
    if (!name_ok(name, odin3_strtab_len(rw->tab, str))) {
        refuse_name(rw, what, name);
        return;
    }
    rw_put(rw, "\\");
    rw_put(rw, name);
}

/* --- attributes ---------------------------------------------------------------------------- */

typedef struct attr_ctx {
    rtl_writer *rw;
    uint32_t indent;
} attr_ctx;

/* odin3_attr_foreach visitor: "attribute \key value". */
static odin3_status put_attr(void *user, uint32_t key_str, const odin3_value *value) {
    attr_ctx *ctx = user;
    rtl_writer *rw = ctx->rw;
    if (strcmp(odin3_strtab_get(odin3_design_strtab(rw->design), key_str),
               ODIN3_ATTR_EDIT_EVENTS) == 0 &&
        value->kind == ODIN3_VAL_STRING &&
        odin3_edit_events_wellformed(
            odin3_strtab_get(odin3_design_strtab(rw->design), value->str))) {
        return rw->st; /* edit events: printed only by the opt-in exports */
    }
    rw_indent(rw, ctx->indent);
    rw_put(rw, "attribute ");
    rw_name(rw, "attribute", key_str);
    rw_put(rw, " ");
    if (rw_ok(rw)) {
        rw->st = odin3_proc_value_text(rw->design, value, rw->out);
    }
    rw_put(rw, "\n");
    return rw->st;
}

static void put_attrs(rtl_writer *rw, odin3_objref obj, uint32_t indent) {
    attr_ctx ctx = {rw, indent};
    if (rw_ok(rw)) {
        rw_keep(rw, odin3_attr_foreach(rw->module, obj, put_attr, &ctx));
    }
}

/* --- signals ------------------------------------------------------------------------------- */

/* The scratch array, holding at least count nets; NULL (status NO_MEMORY) on out of memory. */
static odin3_net_id *scratch_nets(rtl_writer *rw, uint32_t count) {
    if (rw_ok(rw)) {
        rw_keep(rw, odin3_vec_reserve(&rw->nets, count > 0 ? count : 1));
    }
    return rw_ok(rw) ? rw->nets.data : NULL;
}

static void put_sigspec(rtl_writer *rw, const odin3_net_id *nets, uint32_t count) {
    if (rw_ok(rw)) {
        rw->st = odin3_proc_sigspec(rw->module, nets, count, rw->out);
    }
}

/* The run of pins with the same connectedness that starts at index from of slice (length >= 1). */
static uint32_t run_from(const odin3_module *module, odin3_pinslice slice, uint32_t from,
                         bool *connected) {
    *connected = odin3_net_valid(odin3_pin_net(module, (odin3_pin_id){slice.first.v + from}));
    uint32_t end = from + 1;
    while (end < slice.count && odin3_net_valid(odin3_pin_net(
                                    module, (odin3_pin_id){slice.first.v + end})) == *connected) {
        end++;
    }
    return end - from;
}

/* The chunks of a connected run [lo, lo + len), each followed by a blank (braced form). */
static void put_inner(rtl_writer *rw, const odin3_net_id *nets, uint32_t len) {
    odin3_strbuf_clear(&rw->part);
    if (rw_ok(rw)) {
        rw->st = odin3_proc_sigspec(rw->module, nets, len, &rw->part);
    }
    if (!rw_ok(rw)) {
        return;
    }
    const char *data = rw->part.data;
    size_t size = rw->part.len;
    if (size >= BRACE_OPEN + BRACE_CLOSE && data[0] == '{') { /* "{ c1 c2 }" -> "c1 c2 " */
        rw->st = odin3_strbuf_append(
            rw->out, (odin3_bytes){data + BRACE_OPEN, size - BRACE_OPEN - BRACE_CLOSE});
    } else {
        rw->st = odin3_strbuf_append(rw->out, (odin3_bytes){data, size});
        rw_put(rw, " ");
    }
}

/* A port with unconnected pins: MSB first, each unconnected run as its dangling wire $p<pin>. */
static void put_spliced(rtl_writer *rw, odin3_pinslice slice, const odin3_net_id *nets) {
    bool connected = false;
    if (run_from(rw->module, slice, 0, &connected) == slice.count) {
        rw_put_id(rw, "$p", slice.first.v); /* one unconnected run */
        return;
    }
    rw_put(rw, "{ ");
    for (uint32_t high = slice.count; rw_ok(rw) && high > 0;) {
        uint32_t low = high - 1;
        bool on = odin3_net_valid(nets[low]);
        while (low > 0 && odin3_net_valid(nets[low - 1]) == on) {
            low--;
        }
        if (on) {
            put_inner(rw, nets + low, high - low);
        } else {
            rw_put_id(rw, "$p", slice.first.v + low);
            rw_put(rw, " ");
        }
        high = low;
    }
    rw_put(rw, "}");
}

/* The signal on a port's pins. */
static void put_port_sig(rtl_writer *rw, odin3_pinslice slice) {
    odin3_net_id *nets = scratch_nets(rw, slice.count);
    if (nets == NULL) {
        return;
    }
    bool all = true;
    for (uint32_t k = 0; k < slice.count; k++) {
        nets[k] = odin3_pin_net(rw->module, (odin3_pin_id){slice.first.v + k});
        all = all && odin3_net_valid(nets[k]);
    }
    if (all) {
        put_sigspec(rw, nets, slice.count);
    } else {
        put_spliced(rw, slice, nets);
    }
}

/* --- wires --------------------------------------------------------------------------------- */

/* put_wire_options indexes its direction keywords by odin3_dir (review M8). */
_Static_assert(ODIN3_DIR_IN == 0 && ODIN3_DIR_OUT == 1 && ODIN3_DIR_INOUT == 2,
               "k_dirs is indexed by odin3_dir");

static void put_wire_options(rtl_writer *rw, odin3_wire_id wire) {
    const odin3_module *module = rw->module;
    uint32_t width = odin3_wire_width(module, wire);
    int32_t msb = odin3_wire_msb(module, wire);
    int32_t lsb = odin3_wire_lsb(module, wire);
    if (width != 1 && rw_ok(rw)) {
        rw->st = odin3_strbuf_appendf(rw->out, "width %u ", width);
    }
    if (msb < lsb) {
        rw_put(rw, "upto ");
    }
    int32_t low = msb < lsb ? msb : lsb;
    if (low != 0 && rw_ok(rw)) {
        rw->st = odin3_strbuf_appendf(rw->out, "offset %d ", low);
    }
    uint32_t port = rw->port_of[wire.v];
    if (port != 0 && rw_ok(rw)) {
        static const char *const k_dirs[] = {"input", "output", "inout"};
        odin3_dir dir = rw->module_def->ports[port - 1].dir;
        rw->st = odin3_strbuf_appendf(rw->out, "%s %u ", k_dirs[dir], port);
    }
    if (odin3_wire_signed(module, wire)) {
        rw_put(rw, "signed ");
    }
}

/* Each live named wire: "wire [options] \name". */
static void put_wires(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (!odin3_wire_live(module, wire) || odin3_wire_name(module, wire) == 0) {
            continue;
        }
        put_attrs(rw, (odin3_objref){ODIN3_OBJ_WIRE, i}, CELL_INDENT);
        rw_indent(rw, CELL_INDENT);
        rw_put(rw, "wire ");
        put_wire_options(rw, wire);
        rw_name(rw, "wire", odin3_wire_name(module, wire));
        rw_put(rw, "\n");
    }
}

/* Each net spelled as a net of its own (review R4): "wire \name" or "wire $n<ID>". */
static void put_net_wires(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_net_end(module); i++) {
        odin3_net_id net = {i};
        if (!odin3_proc_sig_is_net(module, net)) {
            continue;
        }
        uint32_t name = odin3_net_name(module, net);
        if (name != 0 && odin3_wire_valid(odin3_module_find_wire(module, name))) {
            odin3_log(ODIN3_LOG_ERROR, "write_rtlil: module '%s': net '%s' has the name of a wire",
                      rw->module_name, str_of(rw, name));
            rw->st = ODIN3_ERR_INVALID_ARG;
            return;
        }
        put_attrs(rw, (odin3_objref){ODIN3_OBJ_NET, i}, CELL_INDENT);
        rw_indent(rw, CELL_INDENT);
        rw_put(rw, "wire ");
        if (name != 0) {
            rw_name(rw, "net", name);
        } else {
            rw_put_id(rw, "$n", i);
        }
        rw_put(rw, "\n");
    }
}

/* --- cells --------------------------------------------------------------------------------- */

/* The definition of a node written as a cell; NULL for dead, port, constant and $proc nodes. */
static const odin3_celltype_def *cell_def(const rtl_writer *rw, odin3_node_id node) {
    const odin3_module *module = rw->module;
    if (!odin3_node_live(module, node)) {
        return NULL;
    }
    const odin3_celltype_def *def = odin3_celltype_get(rw->design, odin3_node_type(module, node));
    if (def == NULL || def->gran == ODIN3_GRAN_PORT || (def->flags & ODIN3_CT_PROCESS) != 0) {
        return NULL;
    }
    if (def->const_value != NULL &&
        def->const_value(odin3_node_param(module, node, 0)) != ODIN3_CONST_NONE) {
        return NULL; /* its net is spelled as the constant */
    }
    return def;
}

/* A dangling wire per run of unconnected pins of one port: "wire [width N] $p<pin>". */
static void put_port_dangling(rtl_writer *rw, odin3_pinslice slice) {
    bool connected = false;
    for (uint32_t k = 0, len = 0; rw_ok(rw) && k < slice.count; k += len) {
        len = run_from(rw->module, slice, k, &connected);
        if (connected) {
            continue;
        }
        rw_indent(rw, CELL_INDENT);
        rw_put(rw, "wire ");
        if (len != 1 && rw_ok(rw)) {
            rw->st = odin3_strbuf_appendf(rw->out, "width %u ", len);
        }
        rw_put_id(rw, "$p", slice.first.v + k);
        rw_put(rw, "\n");
    }
}

/* The dangling wires of every written cell, in node then port order. */
static void put_dangling_wires(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_node_end(module); i++) {
        const odin3_celltype_def *def = cell_def(rw, (odin3_node_id){i});
        for (uint32_t port = 0; def != NULL && rw_ok(rw) && port < def->n_ports; port++) {
            put_port_dangling(rw, odin3_node_port(module, (odin3_node_id){i}, port));
        }
    }
}

/* True when a written wire or net-wire is spelled \<str>: a cell may not have that name. */
static bool name_taken(const rtl_writer *rw, uint32_t str) {
    const odin3_module *module = rw->module;
    if (odin3_wire_valid(odin3_module_find_wire(module, str))) {
        return true;
    }
    odin3_net_id net = odin3_module_find_net(module, str);
    return odin3_net_valid(net) && odin3_net_name(module, net) == str &&
           odin3_proc_sig_is_net(module, net);
}

/* "cell <type> <name>": \ prepended to a type name not starting with $ or \. */
static void put_cell_head(rtl_writer *rw, odin3_node_id node, const odin3_celltype_def *def) {
    rw_indent(rw, CELL_INDENT);
    rw_put(rw, "cell ");
    if (!name_ok(def->name, strlen(def->name))) {
        refuse_name(rw, "cell type", def->name);
        return;
    }
    /* a module's type is its name, written \name as its `module` line (review M1) */
    if (def->gran == ODIN3_GRAN_MODULE || (def->name[0] != '$' && def->name[0] != '\\')) {
        rw_put(rw, "\\");
    }
    rw_put(rw, def->name);
    rw_put(rw, " ");
    uint32_t name = odin3_node_name(rw->module, node);
    if (name != 0 && !name_taken(rw, name) &&
        name_ok(str_of(rw, name), odin3_strtab_len(rw->tab, name))) {
        rw_name(rw, "cell", name);
    } else {
        rw_put_id(rw, "$c", node.v);
    }
    rw_put(rw, "\n");
}

/* A MEMID string with \ prepended unless it is already an RTLIL ID ($ or \), quoted. */
static void put_memid(rtl_writer *rw, const odin3_value *val) {
    const char *memid = str_of(rw, val->str);
    odin3_strbuf_clear(&rw->part);
    if (memid[0] != '$' && memid[0] != '\\') {
        rw_keep(rw, odin3_strbuf_append(&rw->part, odin3_bytes_cstr("\\")));
    }
    rw_keep(rw, odin3_strbuf_append(&rw->part,
                                    (odin3_bytes){memid, odin3_strtab_len(rw->tab, val->str)}));
    /* a COVER value prints its bytes as a quoted string with Yosys's escapes */
    odin3_value bytes = {.kind = ODIN3_VAL_COVER,
                         .bits = (const uint8_t *)rw->part.data,
                         .len = (uint32_t)rw->part.len};
    if (rw_ok(rw)) {
        rw->st = odin3_proc_value_text(rw->design, &bytes, rw->out);
    }
}

/* "parameter \P <value>" per parameter, in definition order. */
static void put_params(rtl_writer *rw, odin3_node_id node, const odin3_celltype_def *def) {
    bool is_mem = strcmp(def->name, "$mem") == 0;
    for (uint32_t param = 0; rw_ok(rw) && param < def->n_params; param++) {
        const char *pname = def->params[param].name;
        const odin3_value *val = odin3_node_param(rw->module, node, param);
        rw_indent(rw, 2 * CELL_INDENT);
        rw_put(rw, "parameter \\");
        rw_put(rw, pname);
        rw_put(rw, " ");
        if (val->kind == ODIN3_VAL_STRING && strcmp(pname, "MEMID") == 0) {
            put_memid(rw, val);
        } else if (is_mem && val->kind == ODIN3_VAL_BITS && val->len == 0 &&
                   strcmp(pname, "INIT") != 0) {
            rw_put(rw, "1'0"); /* Yosys 0.55 wants max(1, ports) bits (rtlil.cc, $mem check) */
        } else if (rw_ok(rw)) {
            rw->st = odin3_proc_value_text(rw->design, val, rw->out);
        }
        rw_put(rw, "\n");
    }
}

/*
 * Built-in types Yosys 0.55's read_rtlil rejects or asserts on as Odin III defines them (review
 * I2): the bit-level storage cells (and the families of dffx.h, by prefix below) carry an INIT
 * Yosys's have not, $sop has WIDTH/COVER where
 * Yosys's has DEPTH/TABLE, and $memrd/$memwr need a `memory` record this writer does not make.
 */
static const char *const k_no_rtlil[] = {"$_DFF_P_", "$_DFF_N_", "$_DLATCH_P_", "$_DLATCH_N_",
                                         "$_FF_",    "$sop",     "$memrd",      "$memwr"};

static bool has_prefix(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Why node has no RTLIL form, or NULL: a type above, or a $mem with an empty MEMID. */
static const char *no_rtlil_form(const rtl_writer *rw, odin3_node_id node,
                                 const odin3_celltype_def *def) {
    if (def->gran != ODIN3_GRAN_WORD && def->gran != ODIN3_GRAN_BIT) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof k_no_rtlil / sizeof k_no_rtlil[0]; i++) {
        if (strcmp(def->name, k_no_rtlil[i]) == 0) {
            return "has no RTLIL form";
        }
    }
    /* The $_DFFE_*, $_DFF_<c><r><v>_ and $_SDFF* families (dffx.h) carry an INIT as well. */
    if (def->gran == ODIN3_GRAN_BIT &&
        (has_prefix(def->name, "$_DFF") || has_prefix(def->name, "$_SDFF"))) {
        return "has no RTLIL form";
    }
    for (uint32_t param = 0; strcmp(def->name, "$mem") == 0 && param < def->n_params; param++) {
        const odin3_value *val = odin3_node_param(rw->module, node, param);
        if (strcmp(def->params[param].name, "MEMID") == 0 && val->kind == ODIN3_VAL_STRING &&
            odin3_strtab_len(rw->tab, val->str) == 0) {
            return "has an empty MEMID";
        }
    }
    return NULL;
}

/* Refuses (INVALID_ARG, logged at the module and cell) a node with no RTLIL form. */
static bool refuse_cell(rtl_writer *rw, odin3_node_id node, const odin3_celltype_def *def) {
    const char *why = no_rtlil_form(rw, node, def);
    if (why == NULL) {
        return false;
    }
    enum { CELL_NAME_BUF = 24 };
    char generated[CELL_NAME_BUF];
    uint32_t name = odin3_node_name(rw->module, node);
    (void)snprintf(generated, sizeof generated, "$c%u", node.v);
    odin3_log(ODIN3_LOG_ERROR, "write_rtlil: module '%s': cell '%s' of type '%s' %s",
              rw->module_name, name != 0 ? str_of(rw, name) : generated, def->name, why);
    rw->st = ODIN3_ERR_INVALID_ARG;
    return true;
}

static void put_cell(rtl_writer *rw, odin3_node_id node, const odin3_celltype_def *def) {
    if (refuse_cell(rw, node, def)) {
        return;
    }
    put_attrs(rw, (odin3_objref){ODIN3_OBJ_NODE, node.v}, CELL_INDENT);
    put_cell_head(rw, node, def);
    put_params(rw, node, def);
    for (uint32_t port = 0; rw_ok(rw) && port < def->n_ports; port++) {
        rw_indent(rw, 2 * CELL_INDENT);
        rw_put(rw, "connect \\");
        rw_put(rw, def->ports[port].name);
        rw_put(rw, " ");
        put_port_sig(rw, odin3_node_port(rw->module, node, port));
        rw_put(rw, "\n");
    }
    rw_indent(rw, CELL_INDENT);
    rw_put(rw, "end\n");
}

static void put_cells(rtl_writer *rw) {
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_node_end(rw->module); i++) {
        const odin3_celltype_def *def = cell_def(rw, (odin3_node_id){i});
        if (def != NULL) {
            put_cell(rw, (odin3_node_id){i}, def);
            rw_flush(rw, FLUSH_BYTES);
        }
    }
}

/* Each live $proc node as an RTLIL process block. */
static void put_processes(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(rw->design, odin3_node_type(module, node));
        if (def != NULL && (def->flags & ODIN3_CT_PROCESS) != 0) {
            rw->st = odin3_proc_print(module, node, CELL_INDENT, rw->out);
            rw_flush(rw, FLUSH_BYTES);
        }
    }
}

/* --- aliases ------------------------------------------------------------------------------- */

/* True when bit k of wire is not its net's spelling: a constant net, or another primary. */
static bool is_alias_bit(const odin3_module *module, odin3_wire_id wire, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(module, wire, bit);
    if (odin3_net_const_value(module, net) != ODIN3_CONST_NONE) {
        return true;
    }
    odin3_wirebit primary = odin3_net_primary(module, net);
    return primary.wire.v != wire.v || primary.bit != bit;
}

/* "connect \w[ [hi:lo]] <sig>" for bits [low, high) of a named wire. */
static void put_alias_run(rtl_writer *rw, odin3_wire_id wire, uint32_t low, uint32_t high) {
    const odin3_module *module = rw->module;
    uint32_t width = odin3_wire_width(module, wire);
    odin3_net_id *nets = scratch_nets(rw, high - low);
    if (nets == NULL) {
        return;
    }
    for (uint32_t k = low; k < high; k++) {
        nets[k - low] = odin3_wire_net(module, wire, k);
    }
    rw_indent(rw, CELL_INDENT);
    rw_put(rw, "connect ");
    rw_name(rw, "wire", odin3_wire_name(module, wire));
    if (rw_ok(rw) && high - low == 1 && width != 1) {
        rw->st = odin3_strbuf_appendf(rw->out, " [%u]", low);
    } else if (rw_ok(rw) && high - low != width) {
        rw->st = odin3_strbuf_appendf(rw->out, " [%u:%u]", high - 1, low);
    }
    rw_put(rw, " ");
    put_sigspec(rw, nets, high - low);
    rw_put(rw, "\n");
}

/* For each named wire, each maximal run of alias bits, in wire then bit order. */
static void put_aliases(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    for (uint32_t i = 1; rw_ok(rw) && i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (!odin3_wire_live(module, wire) || odin3_wire_name(module, wire) == 0) {
            continue;
        }
        uint32_t width = odin3_wire_width(module, wire);
        for (uint32_t low = 0; rw_ok(rw) && low < width;) {
            if (!is_alias_bit(module, wire, low)) {
                low++;
                continue;
            }
            uint32_t high = low + 1;
            while (high < width && is_alias_bit(module, wire, high)) {
                high++;
            }
            put_alias_run(rw, wire, low, high);
            low = high;
        }
    }
}

/* --- black-box stubs (2D spec §13 point 7; 2E Task 3) ------------------------------------ */

/* A type that gets a stub: black box or hard, and not `$` (Yosys's namespace of internal cells). */
static bool stub_kind(const odin3_celltype_def *def) {
    return def != NULL && (def->gran == ODIN3_GRAN_BLACKBOX || def->gran == ODIN3_GRAN_HARD) &&
           def->name[0] != '$';
}

/* True when some port's width depends on the parameters (Yosys's `dynports`). */
static bool has_dynports(const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const odin3_port_def *port = &def->ports[i];
        if (port->width_param != NULL || port->width_fn != NULL || port->width_expr != NULL) {
            return true;
        }
    }
    return false;
}

/* Appends a C-string name as an RTLIL ID (\ prepended unless it starts with \), refusing one
 * RTLIL cannot carry. */
static void rw_cname(rtl_writer *rw, const char *what, const char *name) {
    if (!name_ok(name, strlen(name))) {
        refuse_name(rw, what, name);
        return;
    }
    if (name[0] != '\\') {
        rw_put(rw, "\\");
    }
    rw_put(rw, name);
}

/* "parameter \P <default>" per parameter, as Yosys writes a module's parameters. */
static void put_stub_params(rtl_writer *rw, const odin3_celltype_def *def) {
    for (uint32_t i = 0; rw_ok(rw) && i < def->n_params; i++) {
        rw_indent(rw, CELL_INDENT);
        rw_put(rw, "parameter ");
        rw_cname(rw, "parameter", def->params[i].name);
        rw_put(rw, " ");
        if (rw_ok(rw)) {
            rw->st = odin3_proc_value_text(rw->design, &def->params[i].dflt, rw->out);
        }
        rw_put(rw, "\n");
    }
}

/* "wire [width W] input|output|inout K \port" per port, W its width for the default parameters. */
static void put_stub_ports(rtl_writer *rw, odin3_celltype_id type, const odin3_celltype_def *def,
                           const odin3_value *dflts) {
    static const char *const k_dirs[] = {"input", "output", "inout"};
    for (uint32_t i = 0; rw_ok(rw) && i < def->n_ports; i++) {
        uint32_t width = 0;
        odin3_port_query query = {type, dflts, i};
        rw_keep(rw, odin3_celltype_port_width_checked(rw->design, &query, &width));
        rw_indent(rw, CELL_INDENT);
        rw_put(rw, "wire ");
        if (width != 1 && rw_ok(rw)) {
            rw->st = odin3_strbuf_appendf(rw->out, "width %u ", width);
        }
        if (rw_ok(rw)) {
            rw->st = odin3_strbuf_appendf(rw->out, "%s %u ", k_dirs[def->ports[i].dir], i + 1);
        }
        rw_cname(rw, "port", def->ports[i].name);
        rw_put(rw, "\n");
    }
}

/*
 * One stub, as Yosys's read_verilog makes a (* blackbox *) module: attribute blackbox, attribute
 * dynports when a port's width depends on the parameters (so `hierarchy` neither resizes nor
 * derives the instances, which keep their parameters), the parameters with their defaults, and
 * the ports sized for the defaults.
 */
static void put_stub(rtl_writer *rw, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(rw->design, type);
    rw->module_name = def->name;
    odin3_value *dflts = odin3_util_calloc(sizeof *dflts * ((size_t)def->n_params + 1));
    if (dflts == NULL) {
        rw_keep(rw, ODIN3_ERR_NO_MEMORY);
        return;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        dflts[i] = def->params[i].dflt;
    }
    rw_put(rw, "attribute \\blackbox 1\n");
    if (has_dynports(def)) {
        rw_put(rw, "attribute \\dynports 1\n");
    }
    rw_put(rw, "module ");
    rw_cname(rw, "module", def->name);
    rw_put(rw, "\n");
    put_stub_params(rw, def);
    put_stub_ports(rw, type, def, dflts);
    rw_put(rw, "end\n");
    odin3_util_free(dflts);
    rw_flush(rw, FLUSH_BYTES);
}

/* Writes type's stub unless it has one already (done: one byte per cell-type ID). */
static void stub_once(rtl_writer *rw, odin3_celltype_id type, odin3_vec *done) {
    const odin3_celltype_def *def = odin3_celltype_get(rw->design, type);
    if (!stub_kind(def) || type.v >= done->len || *(uint8_t *)odin3_vec_at(done, type.v) != 0) {
        return;
    }
    *(uint8_t *)odin3_vec_at(done, type.v) = 1;
    put_stub(rw, type);
}

/* The stubs, after every module: declared models in declaration order, then every other
 * instantiated black-box or hard type in type-ID order, each once (as the Verilog writer). */
static void put_stubs(rtl_writer *rw) {
    uint32_t n_types = 1;
    while (odin3_celltype_get(rw->design, (odin3_celltype_id){n_types}) != NULL) {
        n_types++;
    }
    odin3_vec done;
    odin3_vec_init(&done, sizeof(uint8_t));
    rw_keep(rw, odin3_vec_reserve(&done, n_types));
    while (rw_ok(rw) && done.len < n_types) {
        (void)odin3_vec_push(&done); /* reserved; zeroed */
    }
    uint32_t declared = odin3_design_declared_model_count(rw->design);
    for (uint32_t i = 0; rw_ok(rw) && i < declared; i++) {
        stub_once(rw, odin3_design_declared_model(rw->design, i), &done);
    }
    for (uint32_t i = 1; rw_ok(rw) && i < n_types; i++) {
        if (odin3_celltype_instances(rw->design, (odin3_celltype_id){i}) > 0) {
            stub_once(rw, (odin3_celltype_id){i}, &done);
        }
    }
    odin3_vec_free(&done);
}

/* --- modules ------------------------------------------------------------------------------- */

/* Maps each port wire to its 1-based port index. */
static void map_ports(rtl_writer *rw) {
    const odin3_module *module = rw->module;
    rw->port_of = odin3_util_calloc(sizeof *rw->port_of * odin3_module_wire_end(module));
    if (rw->port_of == NULL) {
        rw_keep(rw, ODIN3_ERR_NO_MEMORY);
        return;
    }
    for (uint32_t i = 0; i < odin3_module_port_count(module); i++) {
        rw->port_of[odin3_module_port_wire(module, i).v] = i + 1;
    }
}

/* "attribute \top 1" on the design's top module, as Yosys's write_rtlil writes it (read_rtlil
 * takes it as Design::top_module()), unless the module already carries a top attribute. */
static void put_top_attr(rtl_writer *rw, uint32_t id) {
    if (odin3_design_top(rw->design).v != id) {
        return;
    }
    uint32_t key = 0;
    if (odin3_strtab_find(rw->tab, odin3_bytes_cstr("top"), &key) &&
        odin3_attr_get(rw->module, (odin3_objref){ODIN3_OBJ_MODULE, id}, key) != NULL) {
        return;
    }
    rw_put(rw, "attribute \\top 1\n");
}

static void put_module(rtl_writer *rw, uint32_t id) {
    const odin3_module *module =
        odin3_module_get((odin3_design *)rw->design, (odin3_module_id){id});
    rw->module = module;
    rw->module_name = str_of(rw, odin3_module_name(module));
    rw->module_def = odin3_celltype_get(rw->design, odin3_module_celltype(module));
    put_attrs(rw, (odin3_objref){ODIN3_OBJ_MODULE, id}, 0);
    put_top_attr(rw, id);
    rw_put(rw, "module ");
    rw_name(rw, "module", odin3_module_name(module));
    rw_put(rw, "\n");
    map_ports(rw);
    if (rw_ok(rw)) {
        put_wires(rw);
        put_net_wires(rw);
        put_dangling_wires(rw);
        put_cells(rw);
        put_processes(rw);
        put_aliases(rw);
    }
    rw_put(rw, "end\n");
    odin3_util_free(rw->port_of);
    rw->port_of = NULL;
    rw_flush(rw, FLUSH_BYTES);
}

static odin3_status write_design(const odin3_design *design, odin3_strbuf *out, FILE *fp,
                                 const char *path) {
    rtl_writer rw = {.design = design,
                     .tab = odin3_design_strtab(design),
                     .out = out,
                     .fp = fp,
                     .path = path,
                     .st = ODIN3_OK};
    odin3_vec_init(&rw.nets, sizeof(odin3_net_id));
    odin3_strbuf_init(&rw.part);
    for (uint32_t i = 1; rw_ok(&rw) && i < odin3_design_module_end(design); i++) {
        put_module(&rw, i);
    }
    if (rw_ok(&rw)) {
        put_stubs(&rw);
    }
    rw_flush(&rw, 0);
    odin3_vec_free(&rw.nets);
    odin3_strbuf_free(&rw.part);
    if (rw.st == ODIN3_ERR_NO_MEMORY) {
        odin3_log(ODIN3_LOG_ERROR, "write_rtlil: out of memory");
    }
    return rw.st;
}

/* --- entry points -------------------------------------------------------------------------- */

odin3_status odin3_rtlil_text(const odin3_design *design, odin3_strbuf *out) {
    if (design == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "write_rtlil: no design or no output");
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t mark = out->len;
    odin3_status st = write_design(design, out, NULL, NULL);
    if (st != ODIN3_OK && out->len > mark) {
        out->len = mark;
        out->data[mark] = '\0';
    }
    return st;
}

odin3_status odin3_rtlil_write(const odin3_design *design, const char *path) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "write_rtlil: no design or no path");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_atomic_file file;
    odin3_status st = odin3_atomic_file_open(&file, path);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    st = write_design(design, &text, file.fp, path);
    odin3_strbuf_free(&text);
    return odin3_atomic_file_close(&file, st);
}
