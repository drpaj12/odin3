/*
 * elab1_bbdir.c — the port directions of implicit black boxes (--auto-blackbox; 2D spec EL-11,
 * amended 2026-10-21): an unknown cell has no declaration, so each of its ports takes a direction
 * from the nets it is connected to, in the modules already written to the elaborated store, by
 * votes that do not depend on the order of the instances.
 *
 * Per module the net bits get flags from what is known (module ports, assigns, always blocks, gates
 * and the ports of known modules and cell types), and the unknown ports on each bit are counted
 * (none, one, several). Each connection of an unknown port then votes per bit: output when nothing
 * known drives the bit, something known reads it and this is the only unknown port on it; input
 * when a known driver is on it; bidirectional when it is a module or cell inout; no vote for a
 * dangling or ambiguous bit (a constant casts none). The votes of all modules settle a port per
 * cell type (output only: output; input only or none: input; both, or a bidirectional vote:
 * inout). A tie-break then gives a type that has no output one (see tie_break), to a fixpoint. A
 * further visit of the modules, with the settled directions, finds the nets two outputs share and
 * the ambiguous nets nothing drives. One WARNING per cell type lists the directions and
 * those notes.
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
    BD_DRV = 1,   /* a known driver (module input, assign, known output) */
    BD_BIDI = 2,  /* a known inout */
    BD_RD = 4,    /* a known reader (module output, assign source, known input, ...) */
    BD_U1 = 8,    /* an unknown port is on it */
    BD_UM = 16,   /* two or more are */
    BD_ROUT = 32, /* a settled output is on it */
    BD_RINO = 64, /* a settled inout is on it */
    V_OUT = 1,    /* the votes of a port */
    V_IN = 2,
    V_BIDI = 4,
    BD_MAX_BITS = 1 << 16,
    BD_SHIFT = 32,
    BD_GATE_BUF = ODIN3_AST_GATE_DECL_BUF,
    BD_GATE_NOT = ODIN3_AST_GATE_DECL_NOT,
    BD_GATE_NOTIF1 = ODIN3_AST_GATE_DECL_NOTIF1
};

/* Several different ports (or candidates) on a bit. */
static const uint32_t BD_MANY = UINT32_MAX;

/* A closed range of bit indices (any order). */
typedef struct bd_span {
    int64_t lo;
    int64_t hi;
} bd_span;

/* One net of the module being analysed: bits lo .. lo + width - 1, one flag byte each (a net wider
 * than BD_MAX_BITS, or with a range that is not constant, is one bit, coarse), and the port + 1 of
 * the first settled output on each, and the tie-break candidate port + 1 on each (BD_MANY:
 * several). */
typedef struct bd_net {
    int64_t lo;
    uint32_t width;
    uint32_t name;
    bool coarse;
    uint8_t *bits;
    uint32_t *own;
    uint32_t *cand;
    uint32_t *ports; /* the port + 1 on each bit (BD_MANY: two or more different ports) */
} bd_net;

/* Bits first .. first + count - 1 (offsets from the net's low bit) of net (an index). */
typedef struct bd_seg {
    uint32_t net;
    uint32_t first;
    uint32_t count;
} bd_seg;

/* What a visit of the implicit instances does. */
typedef enum { BS_COUNT, BS_VOTE, BS_OWN, BS_UNDRIVEN, BS_CAND, BS_PICK, BS_END } bd_sweep;

/* The visits of the modules: the votes, the tie-break, the final visit. */
enum { BP_VOTE, BP_TIE, BP_FINAL, BP_N };

/* The state of one implicit port (indexed like odin3_elab1.bbports). */
typedef struct bd_pstate {
    uint8_t votes; /* V_* */
    uint8_t dir;   /* odin3_dir, settled from the votes */
    bool has_net;
    uint32_t net;  /* an ambiguous net nothing drives (a name), when has_net */
    uint32_t peer; /* +1: another settled output on a net this port drives */
    uint32_t box;  /* +1: its black box */
    bool want;     /* a tie-break candidate: no votes, and its type has no output */
    bool pick;     /* the only candidate on an ambiguous bit */
    bool tied;     /* given as output by the tie-break */
} bd_pstate;

/* The run: per implicit-port state and the stores. */
typedef struct bd_run {
    odin3_elab1 *e1;
    odin3_ast *ast;
    odin3_arena *arena;
    odin3_u64map *modules; /* MODULE name -> node + 1 */
    bd_pstate *ps;
    int plan; /* BP_*: which visit of the modules this is */
    odin3_status st;
} bd_run;

/* The module being analysed. */
typedef struct bd_mod {
    bd_run *run;
    odin3_u64map *nets; /* name -> net index + 1 */
    odin3_vec netv;
    odin3_vec segs;   /* scratch: the segments of one expression */
    odin3_vec work;   /* scratch: expression ids to visit */
    odin3_vec stmts;  /* scratch: statement ids to visit */
    odin3_vec locals; /* names declared inside the always or initial block being walked */
    bd_sweep sweep;
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
    net->name = name;
    net->coarse = coarse;
    net->bits = odin3_arena_alloc(run->arena, net->width);
    net->own = odin3_arena_alloc(run->arena, net->width * sizeof *net->own);
    net->cand = odin3_arena_alloc(run->arena, net->width * sizeof *net->cand);
    net->ports = odin3_arena_alloc(run->arena, net->width * sizeof *net->ports);
    *idx = (uint32_t)mod->netv.len - 1;
    if (net->bits == NULL || net->own == NULL || net->cand == NULL || net->ports == NULL ||
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

/* Whether name is declared inside the block being walked (it is not a net of the module). */
static bool is_local(const bd_mod *mod, uint32_t name) {
    for (size_t i = 0; i < mod->locals.len; i++) {
        if (*(const uint32_t *)odin3_vec_cat(&mod->locals, i) == name) {
            return true;
        }
    }
    return false;
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
        is_local(mod, odin3_ast_name(run->ast, base)) ||
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
        if (!is_local(mod, odin3_ast_name(run->ast, node)) &&
            net_of(mod, odin3_ast_name(run->ast, node), (odin3_ast_id){0}, &idx)) {
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

/* Records the names the declarations under root declare: locals of its blocks, never nets. */
static void collect_locals(bd_mod *mod, odin3_ast_id root) {
    bd_run *run = mod->run;
    odin3_vec_clear(&mod->locals);
    odin3_vec_clear(&mod->stmts);
    push_id(run, &mod->stmts, root);
    while (mod->stmts.len > 0 && run->st == ODIN3_OK) {
        odin3_ast_id node = *(odin3_ast_id *)odin3_vec_at(&mod->stmts, mod->stmts.len - 1);
        odin3_vec_pop(&mod->stmts);
        odin3_ast_kind kind = odin3_ast_kind_of(run->ast, node);
        odin3_ast_span kids = odin3_ast_children(run->ast, node);
        bool decl = kind == ODIN3_AST_VAR_DECL || kind == ODIN3_AST_NET_DECL;
        for (uint32_t i = 0; i < kids.n && run->st == ODIN3_OK; i++) {
            uint32_t *slot =
                decl && i >= odin3_ast_slot_count(kind) ? odin3_vec_push(&mod->locals) : NULL;
            if (slot != NULL) {
                *slot = odin3_ast_name(run->ast, kids.ids[i]);
            }
            push_id(run, &mod->stmts, kids.ids[i]);
        }
    }
}

/* The statements and expressions under root: every assignment's target is driven, every other
 * net named is read. Names declared inside root are local to it. */
static void walk_stmts(bd_mod *mod, odin3_ast_id root) {
    bd_run *run = mod->run;
    collect_locals(mod, root);
    odin3_vec_clear(&mod->stmts);
    push_id(run, &mod->stmts, root);
    while (mod->stmts.len > 0 && run->st == ODIN3_OK) {
        odin3_ast_id node = *(odin3_ast_id *)odin3_vec_at(&mod->stmts, mod->stmts.len - 1);
        odin3_vec_pop(&mod->stmts);
        odin3_ast_kind kind = odin3_ast_kind_of(run->ast, node);
        bool assign = kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN ||
                      kind == ODIN3_AST_PROC_CONT_ASSIGN;
        if (kind == ODIN3_AST_IDENT || kind == ODIN3_AST_SELECT || kind == ODIN3_AST_CONCAT ||
            kind == ODIN3_AST_REPLICATE) {
            mark_expr(mod, node, BD_RD);
            continue;
        }
        odin3_ast_span kids = odin3_ast_children(run->ast, node);
        if (assign) {
            mark_expr(mod, odin3_ast_child(run->ast, node, 0), BD_DRV);
        }
        for (uint32_t i = assign ? 1 : 0; i < kids.n && push_id(run, &mod->stmts, kids.ids[i]);
             i++) {
        }
    }
    odin3_vec_clear(&mod->locals);
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

/* The outputs of a known instantiation drive their nets, the other ports read them. */
static void mark_known_instance(bd_mod *mod, odin3_ast_id inst) {
    const bd_run *run = mod->run;
    uint32_t type = odin3_ast_name(run->ast, inst);
    odin3_ast_span kids = odin3_ast_children(run->ast, inst);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); i < kids.n; i++) {
        odin3_ast_span conns = odin3_ast_children(run->ast, kids.ids[i]);
        for (uint32_t pos = odin3_ast_slot_count(ODIN3_AST_INSTANCE); pos < conns.n; pos++) {
            int dir = port_dir(run, type, odin3_ast_name(run->ast, conns.ids[pos]));
            uint8_t flag = dir == ODIN3_DIR_OUT ? BD_DRV : BD_RD;
            flag = dir == ODIN3_DIR_INOUT ? BD_BIDI | BD_RD : flag;
            mark_expr(mod, odin3_ast_child(run->ast, conns.ids[pos], 0), flag);
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
 * bidirectional; the others read. */
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
            uint8_t flag = pos - first < outs ? BD_DRV : BD_RD;
            flag = sub > BD_GATE_NOTIF1 ? BD_BIDI | BD_RD : flag;
            mark_expr(mod, odin3_ast_child(run->ast, conns.ids[pos], 0), flag);
        }
    }
}

/* --- the implicit instances ---------------------------------------------------------------------
 */

/* Counts the unknown ports on each bit of the connection: one (BD_U1), several (BD_UM). */
static void count_conn(bd_mod *mod) {
    for (size_t at = 0; at < mod->segs.len; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint8_t *flags = &net->bits[seg->first + i];
            *flags |= (*flags & BD_U1) != 0 ? BD_UM : BD_U1;
        }
    }
}

/* The votes of one connection of port tp, per bit. */
static void vote_conn(const bd_mod *mod, uint32_t tp) {
    bd_pstate *state = &mod->run->ps[tp];
    for (size_t at = 0; at < mod->segs.len; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint8_t flags = net->bits[seg->first + i];
            if ((flags & BD_BIDI) != 0) {
                state->votes |= V_BIDI;
            } else if ((flags & BD_DRV) != 0) {
                state->votes |= V_IN;
            } else if ((flags & (BD_UM | BD_RD | BD_U1)) == (BD_RD | BD_U1)) {
                state->votes |= V_OUT;
            }
        }
    }
}

/* Two settled outputs share a bit: each names the other. */
static void link_ports(bd_run *run, uint32_t tp, uint32_t other) {
    if (other != tp + 1) {
        run->ps[tp].peer = run->ps[tp].peer != 0 ? run->ps[tp].peer : other;
        run->ps[other - 1].peer = run->ps[other - 1].peer != 0 ? run->ps[other - 1].peer : tp + 1;
    }
}

/* A settled output or inout takes its bits; an output on a bit another output has is a conflict. */
static void own_conn(bd_mod *mod, uint32_t tp) {
    uint8_t dir = mod->run->ps[tp].dir;
    for (size_t at = 0; at < mod->segs.len && dir != ODIN3_DIR_IN; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint8_t *flags = &net->bits[seg->first + i];
            uint32_t *owner = &net->own[seg->first + i];
            if (dir == ODIN3_DIR_OUT && (*flags & BD_ROUT) != 0) {
                link_ports(mod->run, tp, *owner);
            } else if (dir == ODIN3_DIR_OUT) {
                *owner = tp + 1;
            }
            *flags |= dir == ODIN3_DIR_OUT ? BD_ROUT : BD_RINO;
        }
    }
}

/* A settled input on an ambiguous bit that nothing, settled or known, drives. */
static void undriven_conn(const bd_mod *mod, uint32_t tp) {
    bd_pstate *state = &mod->run->ps[tp];
    for (size_t at = 0; at < mod->segs.len; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint8_t flags = net->bits[seg->first + i];
            bool bare = (flags & (BD_DRV | BD_BIDI | BD_ROUT | BD_RINO)) == 0;
            if (bare && (flags & BD_UM) != 0 && state->dir == ODIN3_DIR_IN && !state->has_net) {
                state->has_net = true;
                state->net = net->name;
            }
        }
    }
}

/* A bit nothing, settled or known, drives, with two or more different unknown ports on it (the
 * same port of several instances is not enough: it would give them all one net to drive). */
static bool ambiguous(const bd_net *net, uint32_t bit) {
    uint8_t flags = net->bits[bit];
    return (flags & (BD_DRV | BD_BIDI | BD_ROUT | BD_RINO)) == 0 && (flags & BD_UM) != 0 &&
           net->ports[bit] == BD_MANY;
}

/* A connection: per bit, the unknown port on it (or BD_MANY) and the one candidate port (or
 * BD_MANY). */
static void cand_conn(const bd_mod *mod, uint32_t tp) {
    for (size_t at = 0; at < mod->segs.len; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            uint32_t *port = &net->ports[seg->first + i];
            uint32_t *cand = &net->cand[seg->first + i];
            *port = *port == 0 || *port == tp + 1 ? tp + 1 : BD_MANY;
            if (mod->run->ps[tp].want) {
                *cand = *cand == 0 || *cand == tp + 1 ? tp + 1 : BD_MANY;
            }
        }
    }
}

/* A candidate that is alone on an ambiguous bit is picked. */
static void pick_conn(const bd_mod *mod, uint32_t tp) {
    bd_pstate *state = &mod->run->ps[tp];
    for (size_t at = 0; at < mod->segs.len && state->want; at++) {
        const bd_seg *seg = odin3_vec_cat(&mod->segs, at);
        const bd_net *net = odin3_vec_cat(&mod->netv, seg->net);
        for (uint32_t i = 0; i < seg->count; i++) {
            state->pick = state->pick ||
                          (net->cand[seg->first + i] == tp + 1 && ambiguous(net, seg->first + i));
        }
    }
}

static void visit_conn(bd_mod *mod, uint32_t tp) {
    if (mod->sweep == BS_CAND) {
        cand_conn(mod, tp);
    } else if (mod->sweep == BS_PICK) {
        pick_conn(mod, tp);
    } else if (mod->sweep == BS_COUNT) {
        count_conn(mod);
    } else if (mod->sweep == BS_VOTE) {
        vote_conn(mod, tp);
    } else if (mod->sweep == BS_OWN) {
        own_conn(mod, tp);
    } else {
        undriven_conn(mod, tp);
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
            visit_conn(mod, tp - 1);
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

/* The flag a port direction gives its nets: an input is driven from outside, an output is read
 * from outside, an inout is bidirectional. */
static uint8_t port_flag(uint32_t dir) {
    if (dir == ODIN3_DIR_IN) {
        return BD_DRV;
    }
    return dir == ODIN3_DIR_INOUT ? BD_BIDI | BD_RD : BD_RD;
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
            walk_stmts(mod, odin3_ast_child(run->ast, kids.ids[i], 1));
        }
    } else if (kind == ODIN3_AST_ALWAYS || kind == ODIN3_AST_INITIAL) {
        walk_stmts(mod, item);
    } else if (kind == ODIN3_AST_GATE_DECL) {
        mark_gate(mod, item);
    } else if (kind == ODIN3_AST_INSTANTIATION &&
               odin3_elab1_implicit_box(run->e1, odin3_ast_name(run->ast, item)) == 0) {
        mark_known_instance(mod, item);
    }
}

/* The visits of a phase: the first collects the votes; the second (with the settled directions)
 * finds conflicts and nets that nothing drives. */
static const bd_sweep k_plan[BP_N][BS_END] = {
    {BS_COUNT, BS_VOTE, BS_END, BS_END, BS_END, BS_END},
    {BS_COUNT, BS_OWN, BS_CAND, BS_PICK, BS_END, BS_END},
    {BS_COUNT, BS_OWN, BS_UNDRIVEN, BS_END, BS_END, BS_END},
};

static void analyse_module(bd_run *run, odin3_ast_id module) {
    bd_mod mod = {.run = run, .nets = odin3_u64map_create(0)};
    odin3_vec_init(&mod.netv, sizeof(bd_net));
    odin3_vec_init(&mod.segs, sizeof(bd_seg));
    odin3_vec_init(&mod.work, sizeof(odin3_ast_id));
    odin3_vec_init(&mod.stmts, sizeof(odin3_ast_id));
    odin3_vec_init(&mod.locals, sizeof(uint32_t));
    if (mod.nets == NULL) {
        latch(run, ODIN3_ERR_NO_MEMORY);
        return;
    }
    scan_declarations(&mod, module);
    odin3_ast_span items = odin3_ast_children(run->ast, module);
    for (uint32_t i = 2; i < items.n && run->st == ODIN3_OK; i++) {
        scan_drivers(&mod, items.ids[i]);
    }
    for (int step = 0; k_plan[run->plan][step] != BS_END; step++) {
        mod.sweep = k_plan[run->plan][step];
        for (uint32_t i = 2; i < items.n && run->st == ODIN3_OK; i++) {
            if (odin3_ast_kind_of(run->ast, items.ids[i]) == ODIN3_AST_INSTANTIATION) {
                classify_instances(&mod, items.ids[i]);
            }
        }
    }
    odin3_u64map_destroy(mod.nets);
    odin3_vec_free(&mod.netv);
    odin3_vec_free(&mod.segs);
    odin3_vec_free(&mod.work);
    odin3_vec_free(&mod.stmts);
    odin3_vec_free(&mod.locals);
}

/* --- the run --------------------------------------------------------------------------------- */

static const char *dir_text(odin3_dir dir) {
    if (dir == ODIN3_DIR_IN) {
        return "input";
    }
    return dir == ODIN3_DIR_OUT ? "output" : "inout";
}

/* The direction the votes of a port settle on: inout if any is bidirectional or they disagree,
 * else output if any is, else input. */
static odin3_dir settled(uint8_t votes) {
    bool both = (votes & V_OUT) != 0 && (votes & V_IN) != 0;
    if ((votes & V_BIDI) != 0 || both) {
        return ODIN3_DIR_INOUT;
    }
    return (votes & V_OUT) != 0 ? ODIN3_DIR_OUT : ODIN3_DIR_IN;
}

/* The notes of a port, "; <port>: <what>": its votes disagree or its net has no known direction,
 * another output shares its net, its net has no driver. The wording is the JSON reader's. */
static void port_notes(bd_run *run, uint32_t tp, odin3_strbuf *text) {
    const bd_pstate *state = &run->ps[tp];
    const odin3_elab1_bbport *self = odin3_vec_cat(&run->e1->bbports, tp);
    const char *name = odin3_elab1_str(run->e1, self->port.name);
    if (state->dir == ODIN3_DIR_INOUT) {
        latch(run, odin3_strbuf_appendf(text, "; %s: %s", name,
                                        (state->votes & V_BIDI) != 0
                                            ? "its net has no known direction"
                                            : "an input at some instances, an output at others"));
    }
    if (state->peer != 0) {
        const bd_pstate *other = &run->ps[state->peer - 1];
        const odin3_elab1_bbport *port = odin3_vec_cat(&run->e1->bbports, state->peer - 1);
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(run->e1, other->box - 1);
        latch(run, odin3_strbuf_appendf(text, "; %s: its net is also driven by '%s.%s'", name,
                                        odin3_elab1_str(run->e1, box->name),
                                        odin3_elab1_str(run->e1, port->port.name)));
    }
    if (state->tied) {
        latch(run, odin3_strbuf_appendf(text, "; %s: chosen as output: the type had none", name));
    }
    if (state->has_net) {
        latch(run, odin3_strbuf_appendf(text, "; %s: its net '%s' has no driver", name,
                                        odin3_elab1_str(run->e1, state->net)));
    }
}

/* The black box's ports get their final directions; the WARNING lists them, then the notes. */
static void settle_box(bd_run *run, const odin3_elab1_bbox *box) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (uint32_t at = box->first_port; at != 0 && run->st == ODIN3_OK;) {
        odin3_elab1_bbport *port = odin3_vec_at(&run->e1->bbports, at - 1);
        port->port.dir = run->ps[at - 1].dir;
        latch(run, odin3_strbuf_appendf(&text, "%s%s=%s", text.len > 0 ? ", " : "",
                                        odin3_elab1_str(run->e1, port->port.name),
                                        dir_text((odin3_dir)port->port.dir)));
        at = port->next;
    }
    for (uint32_t at = box->first_port; at != 0 && run->st == ODIN3_OK;) {
        port_notes(run, at - 1, &text);
        at = ((const odin3_elab1_bbport *)odin3_vec_cat(&run->e1->bbports, at - 1))->next;
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

/* Gives every port its black box (+1), and every port its direction from the votes so far. */
static void index_ports(bd_run *run) {
    for (uint32_t idx = 0; idx < run->e1->bboxes.len; idx++) {
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(run->e1, idx);
        for (uint32_t at = box->first_port; at != 0;) {
            run->ps[at - 1].box = idx + 1;
            at = ((const odin3_elab1_bbport *)odin3_vec_cat(&run->e1->bbports, at - 1))->next;
        }
    }
}

static void settle_votes(bd_run *run) {
    for (size_t tp = 0; tp < run->e1->bbports.len; tp++) {
        bd_pstate *state = &run->ps[tp];
        state->dir = state->tied ? ODIN3_DIR_OUT : (uint8_t)settled(state->votes);
    }
}

/* Whether the black box (an index) has a port settled as output or inout. */
static bool has_output(const bd_run *run, const odin3_elab1_bbox *box) {
    for (uint32_t at = box->first_port; at != 0;) {
        if (run->ps[at - 1].dir != ODIN3_DIR_IN) {
            return true;
        }
        at = ((const odin3_elab1_bbport *)odin3_vec_cat(&run->e1->bbports, at - 1))->next;
    }
    return false;
}

/* Marks the tie-break candidates: the ports with no votes of a type that has no output. */
static void mark_wanting(bd_run *run) {
    for (uint32_t idx = 0; idx < run->e1->bboxes.len; idx++) {
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(run->e1, idx);
        bool none = box->implicit && !has_output(run, box);
        for (uint32_t at = box->first_port; at != 0;) {
            bd_pstate *state = &run->ps[at - 1];
            state->want = none && state->votes == 0;
            state->pick = false;
            at = ((const odin3_elab1_bbport *)odin3_vec_cat(&run->e1->bbports, at - 1))->next;
        }
    }
}

/* A type with no output and exactly one picked port makes it the output; true when one did. */
static bool give_outputs(bd_run *run) {
    bool changed = false;
    for (uint32_t idx = 0; idx < run->e1->bboxes.len; idx++) {
        const odin3_elab1_bbox *box = odin3_elab1_bbox_at(run->e1, idx);
        uint32_t picked = 0;
        uint32_t last = 0;
        for (uint32_t at = box->first_port; at != 0;) {
            picked += run->ps[at - 1].pick ? 1U : 0U;
            last = run->ps[at - 1].pick ? at : last;
            at = ((const odin3_elab1_bbport *)odin3_vec_cat(&run->e1->bbports, at - 1))->next;
        }
        if (picked == 1) {
            run->ps[last - 1].tied = true;
            run->ps[last - 1].dir = ODIN3_DIR_OUT;
            changed = true;
        }
    }
    return changed;
}

/* The tie-break, a fixpoint: every cell type should have an output. On an ambiguous bit (nothing
 * known drives it, two or more unknown ports, none settled as output) the only port of a type with
 * no output becomes that type's output, if it is the type's only such port. Two output-less types
 * on the bit, or two candidate ports of a type, stay inputs (no guess by order or by name). */
static void tie_break(bd_run *run, const odin3_ast_id *ids, uint32_t n) {
    run->plan = BP_TIE;
    bool changed = true;
    for (uint32_t round = 0; changed && round <= run->e1->bboxes.len && run->st == ODIN3_OK;
         round++) {
        mark_wanting(run);
        for (uint32_t i = 0; i < n && run->st == ODIN3_OK; i++) {
            bool body = odin3_ast_kind_of(run->ast, ids[i]) == ODIN3_AST_MODULE &&
                        odin3_ast_nchild(run->ast, ids[i]) > 2;
            if (body) {
                analyse_module(run, ids[i]);
            }
        }
        changed = give_outputs(run);
    }
    for (size_t tp = 0; tp < run->e1->bbports.len; tp++) {
        run->ps[tp].peer = 0;
    }
}

/* Runs both visits of every module with a body (the second with the settled directions). */
static void analyse_modules(bd_run *run, const odin3_ast_id *ids, uint32_t n) {
    for (int phase = 0; phase < 2 && run->st == ODIN3_OK; phase++) {
        run->plan = phase == 0 ? BP_VOTE : BP_FINAL;
        for (uint32_t i = 0; i < n && run->st == ODIN3_OK; i++) {
            bool body = odin3_ast_kind_of(run->ast, ids[i]) == ODIN3_AST_MODULE &&
                        odin3_ast_nchild(run->ast, ids[i]) > 2;
            if (body) {
                analyse_module(run, ids[i]);
            }
        }
        settle_votes(run);
        if (phase == 0 && run->st == ODIN3_OK) {
            tie_break(run, ids, n);
        }
    }
}

odin3_status odin3_elab1_infer_directions(odin3_elab1 *e1) {
    if (!any_implicit(e1)) {
        return ODIN3_OK;
    }
    bd_run run = {.e1 = e1, .ast = e1->elab, .arena = odin3_arena_create(0)};
    run.modules = odin3_u64map_create(0);
    run.ps = run.arena != NULL
                 ? odin3_arena_alloc(run.arena, (e1->bbports.len + 1) * sizeof *run.ps)
                 : NULL;
    if (run.modules == NULL || run.ps == NULL) {
        odin3_u64map_destroy(run.modules);
        odin3_arena_destroy(run.arena);
        return ODIN3_ERR_NO_MEMORY;
    }
    index_ports(&run);
    odin3_ast_span pending = odin3_ast_pending_span(e1->elab, 0);
    odin3_ast_id *ids = odin3_arena_alloc(run.arena, (pending.n + 1) * sizeof *ids);
    if (ids == NULL) {
        run.st = ODIN3_ERR_NO_MEMORY;
    } else {
        memcpy(ids, pending.ids, pending.n * sizeof *ids);
        index_modules(&run, ids, pending.n);
        analyse_modules(&run, ids, pending.n);
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
