/*
 * elab1_bbdir.c — the port directions of implicit black boxes (--auto-blackbox; 2D spec EL-11,
 * amended 2026-10-21): an unknown cell has no declaration, so each of its ports takes a direction
 * from the nets it is connected to, in the modules already written to the elaborated store.
 *
 * A port is an output when it connects to a net bit that nothing else drives (no module input, no
 * assign or always target, no output of a known module, gate or cell type, no earlier unknown
 * port); an inout when a net bit has no known direction (a module inout port, a known inout); an
 * input otherwise (a driven net, a constant, an expression, no connection). A port is one
 * direction for the cell type: output if any instance needs it (inout if any instance is
 * bidirectional). Two unknown ports on one undriven net: the first in source order drives it.
 * One WARNING per cell type lists the directions and the ports that are ambiguous.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    BD_DRV = 1,  /* a known driver (module input, assign, known output) */
    BD_BIDI = 2, /* a known inout */
    BD_UNK = 4,  /* driven by an unknown port made an output */
    BD_MAX_BITS = 1 << 16,
    BD_SHIFT = 32,
    BD_GATE_BUF = ODIN3_AST_GATE_DECL_BUF,
    BD_GATE_NOT = ODIN3_AST_GATE_DECL_NOT,
    BD_GATE_NOTIF1 = ODIN3_AST_GATE_DECL_NOTIF1
};

/* A closed range of bit indices (any order). */
typedef struct bd_span {
    int64_t lo;
    int64_t hi;
} bd_span;

/* One net of the module being analysed: bits lo .. lo + width - 1, one flag byte each (a net wider
 * than BD_MAX_BITS is one bit, coarse). */
typedef struct bd_net {
    int64_t lo;
    uint32_t width;
    bool coarse;
    uint8_t *bits;
} bd_net;

/* Bits first .. first + count - 1 (offsets from the net's low bit) of net (an index). */
typedef struct bd_seg {
    uint32_t net;
    uint32_t first;
    uint32_t count;
} bd_seg;

/* The run: per implicit-port state (indexed like odin3_elab1.bbports) and the stores. */
typedef struct bd_run {
    odin3_elab1 *e1;
    odin3_ast *ast;
    odin3_arena *arena;
    odin3_u64map *modules; /* MODULE name -> node + 1 */
    uint8_t *is_out;
    uint8_t *is_inout;
    uint8_t *in_known; /* an instance where the port is an input to a driven net */
    odin3_status st;
} bd_run;

/* The module being analysed. */
typedef struct bd_mod {
    bd_run *run;
    odin3_u64map *nets; /* name -> net index + 1 */
    odin3_vec netv;
    odin3_vec segs;  /* scratch: the segments of one expression */
    odin3_vec work;  /* scratch: expression ids to visit */
    odin3_vec stmts; /* scratch: statement ids to visit */
} bd_mod;

static int64_t lower(int64_t left, int64_t right) {
    return left < right ? left : right;
}

static int64_t upper(int64_t left, int64_t right) {
    return left > right ? left : right;
}

static bool latch(bd_run *run, odin3_status st) {
    run->st = run->st == ODIN3_OK ? st : run->st;
    return run->st != ODIN3_OK;
}

/* A NUMBER node's value (no x or z, at most 32 bits). */
static bool const_of(const bd_run *run, odin3_ast_id node, int64_t *out) {
    const odin3_ast_number *num = odin3_ast_number_of(run->ast, node);
    if (odin3_ast_kind_of(run->ast, node) != ODIN3_AST_NUMBER || num == NULL || num->has_xz ||
        num->nbits == 0 || num->nbits > BD_SHIFT) {
        return false;
    }
    int64_t val = 0;
    for (uint32_t i = 0; i < num->nbits; i++) {
        val |= (int64_t)(num->bits[i] == 1 ? 1 : 0) << i;
    }
    bool negative = num->nbits == BD_SHIFT && num->bits[BD_SHIFT - 1] == 1 &&
                    (odin3_ast_flags(run->ast, node) & ODIN3_AST_F_NUMBER_SIGNED) != 0;
    *out = negative ? val - ((int64_t)1 << BD_SHIFT) : val;
    return true;
}

/* --- the nets of a module ------------------------------------------------------------------- */

/* The index of net name, made with the given range (or 1 bit at 0 when it has none). */
static bool net_of(bd_mod *mod, uint32_t name, odin3_ast_id range, uint32_t *idx) {
    uint64_t at = 0;
    if (odin3_u64map_get(mod->nets, name, &at)) {
        *idx = (uint32_t)at - 1;
        return true;
    }
    bd_run *run = mod->run;
    int64_t msb = 0;
    int64_t lsb = 0;
    bool known = range.v != 0 && const_of(run, odin3_ast_child(run->ast, range, 0), &msb) &&
                 const_of(run, odin3_ast_child(run->ast, range, 1), &lsb);
    int64_t span = upper(msb, lsb) - lower(msb, lsb) + 1;
    bool coarse = !known || span > BD_MAX_BITS;
    bd_net *net = odin3_vec_push(&mod->netv);
    if (net == NULL) {
        latch(run, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    net->lo = coarse ? 0 : lower(msb, lsb);
    net->width = coarse ? 1 : (uint32_t)span;
    net->coarse = coarse && known;
    net->bits = odin3_arena_alloc(run->arena, net->width);
    *idx = (uint32_t)mod->netv.len - 1;
    if (net->bits == NULL ||
        latch(run, odin3_u64map_put(mod->nets, (odin3_kv){name, mod->netv.len}))) {
        latch(run, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    return true;
}

static void mark_seg(bd_mod *mod, const bd_seg *seg, uint8_t flag) {
    bd_net *net = odin3_vec_at(&mod->netv, seg->net);
    for (uint32_t i = 0; i < seg->count; i++) {
        net->bits[seg->first + i] |= flag;
    }
}

/* Adds the bits of net idx in range (clipped to the net) to the segments. */
static void add_seg(bd_mod *mod, uint32_t idx, bd_span range) {
    const bd_net *net = odin3_vec_cat(&mod->netv, idx);
    int64_t top = net->lo + (int64_t)net->width - 1;
    int64_t from = net->coarse ? net->lo : upper(lower(range.lo, range.hi), net->lo);
    int64_t to = net->coarse ? net->lo : lower(upper(range.lo, range.hi), top);
    bd_seg *seg = from <= to ? odin3_vec_push(&mod->segs) : NULL;
    if (seg != NULL) {
        *seg = (bd_seg){idx, (uint32_t)(from - net->lo), (uint32_t)(to - from + 1)};
    }
}

/* The whole of net idx. */
static void add_whole(bd_mod *mod, uint32_t idx) {
    const bd_net *net = odin3_vec_cat(&mod->netv, idx);
    add_seg(mod, idx, (bd_span){net->lo, net->lo + (int64_t)net->width - 1});
}

/* A SELECT over an identifier: its constant bit or part, else the whole net. */
static void add_select(bd_mod *mod, odin3_ast_id sel) {
    const bd_run *run = mod->run;
    odin3_ast_id base = odin3_ast_child(run->ast, sel, 0);
    if (odin3_ast_kind_of(run->ast, base) != ODIN3_AST_IDENT) {
        while (odin3_ast_kind_of(run->ast, base) == ODIN3_AST_SELECT) {
            base = odin3_ast_child(run->ast, base, 0);
        }
        sel = (odin3_ast_id){0};
    }
    uint32_t idx = 0;
    if (odin3_ast_kind_of(run->ast, base) != ODIN3_AST_IDENT ||
        !net_of(mod, odin3_ast_name(run->ast, base), (odin3_ast_id){0}, &idx)) {
        return;
    }
    int64_t msb = 0;
    int64_t lsb = 0;
    uint32_t sub = sel.v != 0 ? odin3_ast_sub(run->ast, sel) : ODIN3_AST_SELECT_PART_PLUS;
    bool first = sel.v != 0 && const_of(run, odin3_ast_child(run->ast, sel, 1), &msb);
    bool part =
        sub == ODIN3_AST_SELECT_PART && const_of(run, odin3_ast_child(run->ast, sel, 2), &lsb);
    if (first && sub == ODIN3_AST_SELECT_BIT) {
        add_seg(mod, idx, (bd_span){msb, msb});
    } else if (first && part) {
        add_seg(mod, idx, (bd_span){msb, lsb});
    } else {
        add_whole(mod, idx);
    }
}

/* Pushes id on a work list (a stop on out of memory). */
static bool push_id(bd_run *run, odin3_vec *list, odin3_ast_id id) {
    odin3_ast_id *slot = id.v != 0 ? odin3_vec_push(list) : NULL;
    if (slot == NULL && id.v != 0) {
        latch(run, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    if (slot != NULL) {
        *slot = id;
    }
    return true;
}

/* One expression node: a net adds its bits, a concatenation queues its parts. */
static void visit_expr(bd_mod *mod, odin3_ast_id node) {
    bd_run *run = mod->run;
    odin3_ast_kind kind = odin3_ast_kind_of(run->ast, node);
    uint32_t idx = 0;
    if (kind == ODIN3_AST_IDENT) {
        if (net_of(mod, odin3_ast_name(run->ast, node), (odin3_ast_id){0}, &idx)) {
            add_whole(mod, idx);
        }
    } else if (kind == ODIN3_AST_SELECT) {
        add_select(mod, node);
    } else if (kind == ODIN3_AST_CONCAT || kind == ODIN3_AST_REPLICATE) {
        odin3_ast_span kids = odin3_ast_children(run->ast, node);
        uint32_t from = kind == ODIN3_AST_REPLICATE ? 1 : 0;
        for (uint32_t i = from; i < kids.n && push_id(run, &mod->work, kids.ids[i]); i++) {
        }
    }
}

/* Visits expr and its concatenated parts; mod->segs gets every net bit it names. */
static void collect_segs(bd_mod *mod, odin3_ast_id expr) {
    bd_run *run = mod->run;
    odin3_vec_clear(&mod->segs);
    odin3_vec_clear(&mod->work);
    push_id(run, &mod->work, expr);
    while (mod->work.len > 0 && run->st == ODIN3_OK) {
        odin3_ast_id node = *(odin3_ast_id *)odin3_vec_at(&mod->work, mod->work.len - 1);
        odin3_vec_pop(&mod->work);
        visit_expr(mod, node);
    }
}

/* Marks every net bit expr names with flag. */
static void mark_expr(bd_mod *mod, odin3_ast_id expr, uint8_t flag) {
    collect_segs(mod, expr);
    for (size_t i = 0; i < mod->segs.len; i++) {
        mark_seg(mod, odin3_vec_at(&mod->segs, i), flag);
    }
}

/* --- declarations and known drivers ----------------------------------------------------------- */

/* The nets a PORT_DECL, NET_DECL or VAR_DECL declares; flag marks them (0: none). */
static void declare(bd_mod *mod, odin3_ast_id decl, uint8_t flag) {
    const bd_run *run = mod->run;
    odin3_ast_kind kind = odin3_ast_kind_of(run->ast, decl);
    odin3_ast_id range = odin3_ast_child(run->ast, decl, kind == ODIN3_AST_NET_DECL ? 1 : 0);
    odin3_ast_span kids = odin3_ast_children(run->ast, decl);
    for (uint32_t i = odin3_ast_slot_count(kind); i < kids.n && run->st == ODIN3_OK; i++) {
        uint32_t idx = 0;
        if (net_of(mod, odin3_ast_name(run->ast, kids.ids[i]), range, &idx)) {
            bd_seg all = {idx, 0, ((const bd_net *)odin3_vec_cat(&mod->netv, idx))->width};
            uint8_t extra =
                kind == ODIN3_AST_NET_DECL && odin3_ast_child(run->ast, kids.ids[i], 0).v != 0
                    ? BD_DRV
                    : 0;
            mark_seg(mod, &all, (uint8_t)(flag | extra));
        }
    }
}

/* The statements under root: every assignment's target is driven. */
static void mark_procedural(bd_mod *mod, odin3_ast_id root) {
    bd_run *run = mod->run;
    odin3_vec_clear(&mod->stmts);
    push_id(run, &mod->stmts, root);
    while (mod->stmts.len > 0 && run->st == ODIN3_OK) {
        odin3_ast_id node = *(odin3_ast_id *)odin3_vec_at(&mod->stmts, mod->stmts.len - 1);
        odin3_vec_pop(&mod->stmts);
        odin3_ast_kind kind = odin3_ast_kind_of(run->ast, node);
        if (kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN ||
            kind == ODIN3_AST_PROC_CONT_ASSIGN) {
            mark_expr(mod, odin3_ast_child(run->ast, node, 0), BD_DRV);
            continue;
        }
        odin3_ast_span kids = odin3_ast_children(run->ast, node);
        for (uint32_t i = 0; i < kids.n && push_id(run, &mod->stmts, kids.ids[i]); i++) {
        }
    }
}

/* The direction of the port called pname among the PORT_DECLs of item (a PORT_DECL), or -1. */
static int decl_dir(const bd_run *run, odin3_ast_id item, uint32_t pname) {
    if (odin3_ast_kind_of(run->ast, item) != ODIN3_AST_PORT_DECL) {
        return -1;
    }
    odin3_ast_span decls = odin3_ast_children(run->ast, item);
    for (uint32_t at = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); at < decls.n; at++) {
        if (odin3_ast_name(run->ast, decls.ids[at]) == pname) {
            return (int)odin3_ast_sub(run->ast, item);
        }
    }
    return -1;
}

/* The direction of port pname of the elaborated MODULE module, or -1. */
static int module_port_dir(const bd_run *run, odin3_ast_id module, uint32_t pname) {
    odin3_ast_span kids = odin3_ast_children(run->ast, module);
    odin3_ast_span header =
        kids.n > 1 ? odin3_ast_children(run->ast, kids.ids[1]) : (odin3_ast_span){NULL, 0};
    for (uint32_t k = 0; k < header.n; k++) {
        int dir = decl_dir(run, header.ids[k], pname);
        if (dir >= 0) {
            return dir;
        }
    }
    for (uint32_t i = 2; i < kids.n; i++) {
        int dir = decl_dir(run, kids.ids[i], pname);
        if (dir >= 0) {
            return dir;
        }
    }
    return -1;
}

/* The direction of port pname (a strtab ID) of the module or cell type name, or -1 (unknown). */
static int port_dir(const bd_run *run, uint32_t name, uint32_t pname) {
    uint64_t at = 0;
    if (odin3_u64map_get(run->modules, name, &at)) {
        return module_port_dir(run, (odin3_ast_id){(uint32_t)at - 1}, pname);
    }
    odin3_celltype_id type;
    if (!odin3_celltype_find(run->e1->design, name, &type)) {
        return -1;
    }
    const odin3_celltype_def *def = odin3_celltype_get(run->e1->design, type);
    const char *text = odin3_elab1_str(run->e1, pname);
    for (uint32_t pos = 0; def != NULL && pos < def->n_ports; pos++) {
        if (strcmp(def->ports[pos].name, text) == 0) {
            return (int)def->ports[pos].dir;
        }
    }
    return -1;
}

/* The outputs of a known instantiation drive their nets. */
static void mark_known_instance(bd_mod *mod, odin3_ast_id inst) {
    const bd_run *run = mod->run;
    uint32_t type = odin3_ast_name(run->ast, inst);
    odin3_ast_span kids = odin3_ast_children(run->ast, inst);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); i < kids.n; i++) {
        odin3_ast_span conns = odin3_ast_children(run->ast, kids.ids[i]);
        for (uint32_t pos = odin3_ast_slot_count(ODIN3_AST_INSTANCE); pos < conns.n; pos++) {
            int dir = port_dir(run, type, odin3_ast_name(run->ast, conns.ids[pos]));
            if (dir == ODIN3_DIR_OUT || dir == ODIN3_DIR_INOUT) {
                mark_expr(mod, odin3_ast_child(run->ast, conns.ids[pos], 0),
                          dir == ODIN3_DIR_OUT ? BD_DRV : BD_BIDI);
            }
        }
    }
}

/* How many terminals of a gate with count terminals drive: buf and not, all but the last; the
 * others, the first. */
static uint32_t gate_outputs(bool many, uint32_t count) {
    if (!many) {
        return 1;
    }
    return count > 0 ? count - 1 : 0;
}

/* A gate's terminals: those that drive, and (the switch-level gates, pending in 2D) the rest
 * bidirectional. */
static void mark_gate(bd_mod *mod, odin3_ast_id gate) {
    const bd_run *run = mod->run;
    uint32_t sub = odin3_ast_sub(run->ast, gate);
    uint32_t first = odin3_ast_slot_count(ODIN3_AST_INSTANCE);
    odin3_ast_span kids = odin3_ast_children(run->ast, gate);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_GATE_DECL); i < kids.n; i++) {
        odin3_ast_span conns = odin3_ast_children(run->ast, kids.ids[i]);
        uint32_t outs = gate_outputs(sub == BD_GATE_BUF || sub == BD_GATE_NOT,
                                     conns.n > first ? conns.n - first : 0);
        for (uint32_t pos = first; pos < conns.n; pos++) {
            uint8_t flag = pos - first < outs ? BD_DRV : 0;
            flag = sub > BD_GATE_NOTIF1 ? BD_BIDI : flag;
            mark_expr(mod, odin3_ast_child(run->ast, conns.ids[pos], 0), flag);
        }
    }
}

/* --- the implicit instances ---------------------------------------------------------------------
 */

/* One connection of an implicit instance: its port's direction from the bits it names. */
static void classify(bd_mod *mod, uint32_t tp) {
    const bd_run *run = mod->run;
    bool need_out = false;
    bool bidi = false;
    bool known = mod->segs.len > 0;
    for (size_t at = 0; at < mod->segs.len; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint8_t flags = net->bits[seg->first + i];
            bidi = bidi || (flags & BD_BIDI) != 0;
            need_out = need_out || (flags & (BD_DRV | BD_UNK | BD_BIDI)) == 0;
            known = known && (flags & BD_DRV) != 0 && (flags & BD_BIDI) == 0;
        }
    }
    if (need_out) {
        run->is_out[tp] = 1;
        for (size_t at = 0; at < mod->segs.len; at++) {
            mark_seg(mod, odin3_vec_cat(&mod->segs, at), BD_UNK);
        }
    } else if (bidi) {
        run->is_inout[tp] = 1;
    } else if (known) {
        run->in_known[tp] = 1;
    }
}

static void classify_instance(bd_mod *mod, uint32_t box, odin3_ast_id inst) {
    const bd_run *run = mod->run;
    odin3_ast_span conns = odin3_ast_children(run->ast, inst);
    uint32_t first = odin3_ast_slot_count(ODIN3_AST_INSTANCE);
    for (uint32_t pos = first; pos < conns.n && run->st == ODIN3_OK; pos++) {
        uint32_t tp = odin3_elab1_bbox_port(run->e1, box, odin3_ast_name(run->ast, conns.ids[pos]));
        if (tp != 0) {
            collect_segs(mod, odin3_ast_child(run->ast, conns.ids[pos], 0));
            classify(mod, tp - 1);
        }
    }
}

static void classify_instances(bd_mod *mod, odin3_ast_id item) {
    const bd_run *run = mod->run;
    uint32_t box = odin3_elab1_implicit_box(run->e1, odin3_ast_name(run->ast, item));
    odin3_ast_span kids = odin3_ast_children(run->ast, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); box != 0 && i < kids.n; i++) {
        classify_instance(mod, box - 1, kids.ids[i]);
    }
}

/* --- one module --------------------------------------------------------------------------------
 */

/* The flag a port direction gives its nets: an input is driven from outside, an inout is
 * bidirectional. */
static uint8_t port_flag(uint32_t dir) {
    if (dir == ODIN3_DIR_IN) {
        return BD_DRV;
    }
    return dir == ODIN3_DIR_INOUT ? BD_BIDI : 0;
}

static void scan_declarations(bd_mod *mod, odin3_ast_id module) {
    const bd_run *run = mod->run;
    odin3_ast_span items = odin3_ast_children(run->ast, module);
    odin3_ast_span ports = odin3_ast_children(run->ast, items.ids[1]);
    for (uint32_t i = 0; i < ports.n; i++) {
        declare(mod, ports.ids[i], port_flag(odin3_ast_sub(run->ast, ports.ids[i])));
    }
    for (uint32_t i = 2; i < items.n; i++) {
        odin3_ast_kind kind = odin3_ast_kind_of(run->ast, items.ids[i]);
        if (kind == ODIN3_AST_PORT_DECL) {
            declare(mod, items.ids[i], port_flag(odin3_ast_sub(run->ast, items.ids[i])));
        } else if (kind == ODIN3_AST_NET_DECL || kind == ODIN3_AST_VAR_DECL) {
            declare(mod, items.ids[i], 0);
        }
    }
}

static void scan_drivers(bd_mod *mod, odin3_ast_id item) {
    const bd_run *run = mod->run;
    odin3_ast_kind kind = odin3_ast_kind_of(run->ast, item);
    if (kind == ODIN3_AST_CONT_ASSIGN) {
        odin3_ast_span kids = odin3_ast_children(run->ast, item);
        for (uint32_t i = odin3_ast_slot_count(kind); i < kids.n; i++) {
            mark_expr(mod, odin3_ast_child(run->ast, kids.ids[i], 0), BD_DRV);
        }
    } else if (kind == ODIN3_AST_ALWAYS || kind == ODIN3_AST_INITIAL) {
        mark_procedural(mod, item);
    } else if (kind == ODIN3_AST_GATE_DECL) {
        mark_gate(mod, item);
    } else if (kind == ODIN3_AST_INSTANTIATION &&
               odin3_elab1_implicit_box(run->e1, odin3_ast_name(run->ast, item)) == 0) {
        mark_known_instance(mod, item);
    }
}

static void analyse_module(bd_run *run, odin3_ast_id module) {
    bd_mod mod = {.run = run, .nets = odin3_u64map_create(0)};
    odin3_vec_init(&mod.netv, sizeof(bd_net));
    odin3_vec_init(&mod.segs, sizeof(bd_seg));
    odin3_vec_init(&mod.work, sizeof(odin3_ast_id));
    odin3_vec_init(&mod.stmts, sizeof(odin3_ast_id));
    if (mod.nets == NULL) {
        latch(run, ODIN3_ERR_NO_MEMORY);
        return;
    }
    scan_declarations(&mod, module);
    odin3_ast_span items = odin3_ast_children(run->ast, module);
    for (uint32_t i = 2; i < items.n && run->st == ODIN3_OK; i++) {
        scan_drivers(&mod, items.ids[i]);
    }
    for (uint32_t i = 2; i < items.n && run->st == ODIN3_OK; i++) {
        if (odin3_ast_kind_of(run->ast, items.ids[i]) == ODIN3_AST_INSTANTIATION) {
            classify_instances(&mod, items.ids[i]);
        }
    }
    odin3_u64map_destroy(mod.nets);
    odin3_vec_free(&mod.netv);
    odin3_vec_free(&mod.segs);
    odin3_vec_free(&mod.work);
    odin3_vec_free(&mod.stmts);
}

/* --- the run --------------------------------------------------------------------------------- */

static const char *dir_text(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "input";
    }
    return dir == ODIN3_DIR_OUT ? "output" : "inout";
}

/* The direction the flags of port idx settle on: inout if any instance is bidirectional, else
 * output if any needs one, else input. */
static odin3_dir settled(const bd_run *run, uint32_t idx) {
    if (run->is_inout[idx] != 0) {
        return ODIN3_DIR_INOUT;
    }
    return run->is_out[idx] != 0 ? ODIN3_DIR_OUT : ODIN3_DIR_IN;
}

/* Port at of its black box gets its final direction; the text of the WARNING grows. */
static void settle_port(bd_run *run, odin3_elab1_bbport *port, uint32_t idx, odin3_strbuf *text) {
    odin3_dir dir = settled(run, idx);
    port->port.dir = (uint8_t)dir;
    const char *note = "";
    if (dir == ODIN3_DIR_INOUT) {
        note = " (its net has no known direction)";
    } else if (dir == ODIN3_DIR_OUT && run->in_known[idx]) {
        note = " (an input at some instances, made an output)";
    }
    latch(run,
          odin3_strbuf_appendf(text, "%s%s %s%s", text->len > 0 ? ", " : "",
                               odin3_elab1_str(run->e1, port->port.name), dir_text(dir), note));
}

static void settle_box(bd_run *run, const odin3_elab1_bbox *box) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (uint32_t at = box->first_port; at != 0 && run->st == ODIN3_OK;) {
        odin3_elab1_bbport *port = odin3_vec_at(&run->e1->bbports, at - 1);
        settle_port(run, port, at - 1, &text);
        at = port->next;
    }
    if (run->st == ODIN3_OK && text.len > 0) {
        odin3_elab1_warn_once(run->e1, box->at,
                              "implicit black box '%s': inferred port directions: %s",
                              odin3_elab1_str(run->e1, box->name), text.data);
    }
    odin3_strbuf_free(&text);
}

static bool any_implicit(const odin3_elab1 *e1) {
    for (uint32_t idx = 0; idx < e1->bboxes.len; idx++) {
        if (odin3_elab1_bbox_at(e1, idx)->implicit) {
            return true;
        }
    }
    return false;
}

/* The MODULEs (with a body) in the store so far; a name -> node map of them all. */
static void index_modules(bd_run *run, const odin3_ast_id *ids, uint32_t n) {
    for (uint32_t i = 0; i < n && run->st == ODIN3_OK; i++) {
        if (odin3_ast_kind_of(run->ast, ids[i]) == ODIN3_AST_MODULE) {
            latch(run, odin3_u64map_put(run->modules, (odin3_kv){odin3_ast_name(run->ast, ids[i]),
                                                                 (uint64_t)ids[i].v + 1}));
        }
    }
}

odin3_status odin3_elab1_infer_directions(odin3_elab1 *e1) {
    if (!any_implicit(e1)) {
        return ODIN3_OK;
    }
    bd_run run = {.e1 = e1, .ast = e1->elab, .arena = odin3_arena_create(0)};
    run.modules = odin3_u64map_create(0);
    size_t n_ports = e1->bbports.len;
    run.is_out = run.arena != NULL ? odin3_arena_alloc(run.arena, 3 * (n_ports + 1)) : NULL;
    if (run.modules == NULL || run.is_out == NULL) {
        odin3_u64map_destroy(run.modules);
        odin3_arena_destroy(run.arena);
        return ODIN3_ERR_NO_MEMORY;
    }
    run.is_inout = run.is_out + n_ports + 1;
    run.in_known = run.is_inout + n_ports + 1;
    odin3_ast_span pending = odin3_ast_pending_span(e1->elab, 0);
    odin3_ast_id *ids = odin3_arena_alloc(run.arena, (pending.n + 1) * sizeof *ids);
    if (ids == NULL) {
        run.st = ODIN3_ERR_NO_MEMORY;
    } else {
        memcpy(ids, pending.ids, pending.n * sizeof *ids);
        index_modules(&run, ids, pending.n);
    }
    for (uint32_t i = 0; i < pending.n && run.st == ODIN3_OK; i++) {
        bool body = odin3_ast_kind_of(run.ast, ids[i]) == ODIN3_AST_MODULE &&
                    odin3_ast_nchild(run.ast, ids[i]) > 2;
        if (body) {
            analyse_module(&run, ids[i]);
        }
    }
    for (uint32_t idx = 0; idx < e1->bboxes.len && run.st == ODIN3_OK; idx++) {
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(e1, idx);
        if (box->implicit) {
            settle_box(&run, box);
        }
    }
    odin3_u64map_destroy(run.modules);
    odin3_arena_destroy(run.arena);
    return run.st;
}
