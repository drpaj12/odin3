/*
 * elab2_drive.c — stage 2's drivers (2D Task 19; EL-26, EL-28, EL-29, A4): the driver table and the
 * claim every driver makes before it attaches (A4, uwire, supply, wired nets, tri-state and inout
 * rules), the claim and connection of an lvalue (which elab2_assign.c takes apart), the merge that
 * remembers where a net went, the pre-scan of step (2), the `$tribuf` of D3, the wired-net gates of
 * step (7) and the end-of-module warnings. Instances and gates are elab2_inst.c.
 *
 * Two tables, both per module. The driver table (a vec of odin3_drv indexed by net ID) says what
 * drives a net; it is checked on every net a driver attaches to. The claims map says what drove a
 * bit of a wire, by (wire, bit): `assign y = a;` merges y's net into a's, so a second `assign y =
 * b;` must find y's own claim (the net now belongs to a, which is driven by its port), and `assign
 * out = tmp; assign tmp = a & b;` must not find one on tmp.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { CLAIM_KIND_SHIFT = 32 };
#define CLAIM_NODE_MASK 0xFFFFFFFFU

/* What the tables know about a net besides its driver. */
typedef struct net_info {
    uint32_t wrec; /* 1 + the wired record whose wire the net is a bit of; 0: none */
    uint32_t wbit; /* that bit of the wire */
} net_info;

/* One driver of a bit of a wired wire: the fresh net it drives and the statement it came from. */
typedef struct wired_op {
    uint32_t bit;
    odin3_net_id net;
    uint32_t stmt;
} wired_op;

/* A `wand`/`wor`/`triand`/`trior` wire and the drivers collected for it, in source order. */
typedef struct wired_rec {
    odin3_wire_id wire;
    bool is_or;
    odin3_ast_id declarator;
    uint32_t scope;
    odin3_vec ops; /* wired_op */
} wired_rec;

/* A `p = q` whose two sides are one net already: reported at the end if nothing drives it. */
typedef struct loop_note {
    odin3_net_id net;
    odin3_ast_id lhs, rhs;
} loop_note;

struct odin3_elab2_drive {
    odin3_vec drv;         /* odin3_drv per net ID, as far as a net was claimed */
    odin3_vec info;        /* net_info per net ID */
    odin3_u64map *claims;  /* (wire << 32 | bit) -> (kind << 32 | node) */
    odin3_u64map *uwires;  /* wire ID -> 1 */
    odin3_u64map *fwd;     /* a net merged away -> the net that replaced it */
    odin3_u64map *always;  /* symbol ID -> offset of its bits in abits */
    odin3_u64map *covered; /* net -> 1: reported by a loop note */
    odin3_vec wired;       /* wired_rec */
    odin3_vec abits;       /* uint8_t: one flag per bit of an object assigned in an always */
    odin3_vec loops;       /* loop_note */
    uint32_t stmts;        /* statements that drove a wired net */
    bool is_top;
};

/* --- the tables ------------------------------------------------------------------------------ */

void odin3_elab2_drive_free(odin3_elab2 *e2) {
    struct odin3_elab2_drive *dr = e2->drive;
    if (dr == NULL) {
        return;
    }
    for (size_t i = 0; i < dr->wired.len; i++) {
        odin3_vec_free(&((wired_rec *)odin3_vec_at(&dr->wired, i))->ops);
    }
    odin3_vec_free(&dr->drv);
    odin3_vec_free(&dr->info);
    odin3_vec_free(&dr->wired);
    odin3_vec_free(&dr->abits);
    odin3_vec_free(&dr->loops);
    odin3_u64map_destroy(dr->claims);
    odin3_u64map_destroy(dr->uwires);
    odin3_u64map_destroy(dr->fwd);
    odin3_u64map_destroy(dr->always);
    odin3_u64map_destroy(dr->covered);
    odin3_util_free(dr);
    e2->drive = NULL;
}

void odin3_elab2_drive_reset(odin3_elab2 *e2) {
    odin3_elab2_drive_free(e2);
}

/* The tables of the module being emitted, made on first use; NULL on out of memory. */
static struct odin3_elab2_drive *drive_get(odin3_elab2 *e2) {
    if (e2->drive != NULL) {
        return e2->drive;
    }
    struct odin3_elab2_drive *dr = odin3_util_calloc(sizeof *dr);
    if (dr == NULL) {
        return NULL;
    }
    odin3_vec_init(&dr->drv, sizeof(odin3_drv));
    odin3_vec_init(&dr->info, sizeof(net_info));
    odin3_vec_init(&dr->wired, sizeof(wired_rec));
    odin3_vec_init(&dr->abits, sizeof(uint8_t));
    odin3_vec_init(&dr->loops, sizeof(loop_note));
    dr->claims = odin3_u64map_create(0);
    dr->uwires = odin3_u64map_create(0);
    dr->fwd = odin3_u64map_create(0);
    dr->always = odin3_u64map_create(0);
    dr->covered = odin3_u64map_create(0);
    odin3_ast_id unit = odin3_ast_root(e2->elab, 0);
    dr->is_top = e2->mod_node.v != 0 && odin3_ast_child(e2->elab, unit, 0).v == e2->mod_node.v;
    e2->drive = dr;
    if (dr->claims == NULL || dr->uwires == NULL || dr->fwd == NULL || dr->always == NULL ||
        dr->covered == NULL) {
        odin3_elab2_drive_free(e2);
        return NULL;
    }
    return dr;
}

/* The slot of net in a per-net vec, growing it (zeroed) to hold it; NULL on out of memory. */
static void *net_slot(odin3_vec *vec, odin3_net_id net) {
    while (vec->len <= net.v) {
        if (odin3_vec_push(vec) == NULL) {
            return NULL;
        }
    }
    return odin3_vec_at(vec, net.v);
}

/* A net's entry, without growing the table: none past its end. */
static odin3_drv drv_get(const struct odin3_elab2_drive *dr, odin3_net_id net) {
    return net.v < dr->drv.len ? *(const odin3_drv *)odin3_vec_cat(&dr->drv, net.v)
                               : (odin3_drv){0, {0}};
}

static net_info info_get(const struct odin3_elab2_drive *dr, odin3_net_id net) {
    return net.v < dr->info.len ? *(const net_info *)odin3_vec_cat(&dr->info, net.v)
                                : (net_info){0, 0};
}

static odin3_status drv_set(struct odin3_elab2_drive *dr, odin3_net_id net, odin3_drv entry) {
    odin3_drv *slot = net_slot(&dr->drv, net);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot->kind == ODIN3_DRV_NONE) {
        *slot = entry;
    }
    return ODIN3_OK;
}

/* --- merging --------------------------------------------------------------------------------- */

odin3_net_id odin3_elab2_resolve(const odin3_elab2 *e2, odin3_net_id net) {
    uint64_t next = 0;
    while (e2->drive != NULL && odin3_u64map_get(e2->drive->fwd, net.v, &next)) {
        net.v = (uint32_t)next;
    }
    return net;
}

odin3_status odin3_elab2_merge(odin3_elab2 *e2, odin3_net_pair pair) {
    struct odin3_elab2_drive *dr = drive_get(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_u64map_put(dr->fwd, (odin3_kv){pair.drop.v, pair.keep.v});
    st = st != ODIN3_OK ? st : odin3_net_merge(e2->mod, pair);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_drv gone = drv_get(dr, pair.drop);
    net_info gone_info = info_get(dr, pair.drop);
    st = gone.kind != ODIN3_DRV_NONE ? drv_set(dr, pair.keep, gone) : ODIN3_OK;
    if (st == ODIN3_OK && gone_info.wrec != 0 && info_get(dr, pair.keep).wrec == 0) {
        net_info *slot = net_slot(&dr->info, pair.keep);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = gone_info;
        }
    }
    return st;
}

/* --- the claim ------------------------------------------------------------------------------- */

typedef enum claim_code {
    CC_OK,
    CC_A4,
    CC_UWIRE,
    CC_INOUT_LOGIC, /* an inout pin and ordinary logic on one net */
    CC_D3           /* a tri-state driver where D3 does not allow one */
} claim_code;

/* One claim being checked: the request, the tables and what the first conflict was. */
typedef struct claim_run {
    odin3_elab2 *e2;
    struct odin3_elab2_drive *dr;
    const odin3_net_id *nets;
    const odin3_elab2_lvbit *bits; /* NULL: the nets only */
    uint32_t n;
    uint8_t kind;
    odin3_ast_id node;
    bool record;
    const char *port, *instance;
    odin3_u64map *seen; /* net -> bit index + 1: the nets of this request so far */
    uint32_t at;        /* the bit that conflicted */
    odin3_ast_id here;  /* where to report it, when not the bit's own piece */
    claim_code code;
    odin3_drv prev;
} claim_run;

static bool is_logic(uint8_t kind) {
    return kind == ODIN3_DRV_CELL || kind == ODIN3_DRV_INSTANCE || kind == ODIN3_DRV_GATE ||
           kind == ODIN3_DRV_ASSIGN || kind == ODIN3_DRV_PROCESS;
}

/* Two drivers may share a net only when both are tristate drivers or inout pins (rule 4). */
static bool shares(odin3_drv prev, uint8_t next) {
    bool prev_bus = prev.kind == ODIN3_DRV_TRIBUF || prev.kind == ODIN3_DRV_INOUT_PIN;
    bool next_bus = next == ODIN3_DRV_TRIBUF || next == ODIN3_DRV_INOUT_PIN;
    return prev_bus && next_bus;
}

static uint64_t claim_key(odin3_wirebit wb) {
    return ((uint64_t)wb.wire.v << CLAIM_KIND_SHIFT) | wb.bit;
}

/* The claim a wire bit has (kind, node); kind NONE for none. */
static odin3_drv claim_get(const struct odin3_elab2_drive *dr, odin3_wirebit wb) {
    uint64_t value = 0;
    if (wb.wire.v == 0 || !odin3_u64map_get(dr->claims, claim_key(wb), &value)) {
        return (odin3_drv){ODIN3_DRV_NONE, {0}};
    }
    return (odin3_drv){(uint8_t)(value >> CLAIM_KIND_SHIFT), {(uint32_t)(value & CLAIM_NODE_MASK)}};
}

static odin3_status claim_put(struct odin3_elab2_drive *dr, odin3_wirebit wb, odin3_drv entry) {
    if (wb.wire.v == 0 || claim_get(dr, wb).kind != ODIN3_DRV_NONE) {
        return ODIN3_OK;
    }
    return odin3_u64map_put(
        dr->claims,
        (odin3_kv){claim_key(wb), ((uint64_t)entry.kind << CLAIM_KIND_SHIFT) | entry.node.v});
}

/* True when the net is on a $port_out or $port_inout of the module (a top-level D3 port). */
static bool on_output_port(const odin3_elab2 *e2, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(e2->mod, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_node_id node = odin3_pin_node(e2->mod, pins.pins[i]);
        const odin3_celltype_def *def =
            odin3_celltype_get(e2->design, odin3_node_type(e2->mod, node));
        if (def != NULL && def->gran == ODIN3_GRAN_PORT && strcmp(def->name, "$port_in") != 0) {
            return true;
        }
    }
    return false;
}

/* The port and the node's name of an inout pin. */
typedef struct inout_pin {
    const char *port, *instance;
} inout_pin;

/* The inout pin of a child instance on the net; false for none. */
static bool child_inout(const odin3_elab2 *e2, odin3_net_id net, inout_pin *out) {
    odin3_pinlist pins = odin3_net_pins(e2->mod, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        if (odin3_pin_dir(e2->mod, pins.pins[i]) != ODIN3_DIR_INOUT) {
            continue;
        }
        odin3_node_id node = odin3_pin_node(e2->mod, pins.pins[i]);
        const odin3_celltype_def *def =
            odin3_celltype_get(e2->design, odin3_node_type(e2->mod, node));
        if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
            continue;
        }
        out->port = def->ports[odin3_pin_port(e2->mod, pins.pins[i])].name;
        out->instance =
            odin3_strtab_get(odin3_design_strtab(e2->design), odin3_node_name(e2->mod, node));
        return true;
    }
    return false;
}

/* What a previous driver of a bit means for the new one (CC_OK: they share). */
static claim_code against(const claim_run *run, odin3_drv prev, odin3_wirebit wb,
                          odin3_net_id net) {
    if (prev.kind == ODIN3_DRV_NONE || shares(prev, run->kind)) {
        return CC_OK;
    }
    inout_pin pin = {NULL, NULL};
    bool child = child_inout(run->e2, net, &pin);
    if (run->kind == ODIN3_DRV_INOUT_PIN && is_logic(prev.kind)) {
        return CC_INOUT_LOGIC;
    }
    if (prev.kind == ODIN3_DRV_INOUT_PIN && is_logic(run->kind) && child) {
        return CC_INOUT_LOGIC;
    }
    if (wb.wire.v != 0 && odin3_u64map_get(run->dr->uwires, wb.wire.v, NULL)) {
        return CC_UWIRE;
    }
    return CC_A4;
}

/* The bit's own previous driver: the claim of its wire bit first, then the net's entry. */
static claim_code check_prev(claim_run *run, uint32_t bit) {
    odin3_net_id net = run->nets[bit];
    odin3_wirebit wb = run->bits != NULL ? run->bits[bit].wb : (odin3_wirebit){{0}, 0};
    odin3_drv claimed = claim_get(run->dr, wb);
    claim_code code = against(run, claimed, wb, net);
    if (code != CC_OK) {
        run->prev = claimed;
        return code;
    }
    odin3_drv entry = drv_get(run->dr, net);
    code = against(run, entry, wb, net);
    run->prev = entry;
    return code;
}

/* The same net twice in one request is two drivers of it (Yosys: "multiple conflicting drivers");
 * the error is at the later of the two places in the source (a concatenation's lsb is its last
 * part, so that is not always the later bit). */
static claim_code check_dup(claim_run *run, uint32_t bit, odin3_status *st) {
    uint64_t at = 0;
    if (!odin3_u64map_get(run->seen, run->nets[bit].v, &at)) {
        *st = odin3_u64map_put(run->seen, (odin3_kv){run->nets[bit].v, (uint64_t)bit + 1});
        return CC_OK;
    }
    odin3_ast_id mine = run->bits != NULL ? run->bits[bit].node : run->node;
    odin3_ast_id earlier = run->bits != NULL ? run->bits[at - 1].node : run->node;
    bool swap = earlier.v > mine.v;
    run->prev = (odin3_drv){run->kind, swap ? mine : earlier};
    run->here = swap ? earlier : mine;
    return CC_A4;
}

/* One bit of the request against the tables; CC_OK also for a dropped bit and a wired one. */
static claim_code check_bit(claim_run *run, uint32_t bit, odin3_status *st) {
    odin3_net_id net = run->nets[bit];
    if (net.v == 0 || info_get(run->dr, net).wrec != 0) {
        return CC_OK;
    }
    if (run->kind == ODIN3_DRV_TRIBUF && (!run->dr->is_top || !on_output_port(run->e2, net))) {
        return CC_D3;
    }
    claim_code code = run->n > 1 ? check_dup(run, bit, st) : CC_OK;
    return code != CC_OK || *st != ODIN3_OK ? code : check_prev(run, bit);
}

/* --- naming the bits of a conflict ----------------------------------------------------------- */

/* The name of bit i for a message: its wire (and its declared index unless the piece names the
 * whole wire), else the net's primary, else the net's own name. */
static odin3_status bit_label(const claim_run *run, uint32_t bit, odin3_strbuf *out) {
    const odin3_module *mod = run->e2->mod;
    const odin3_strtab *tab = odin3_design_strtab(run->e2->design);
    odin3_wirebit wb = run->bits != NULL ? run->bits[bit].wb : (odin3_wirebit){{0}, 0};
    bool whole = run->bits != NULL && run->bits[bit].whole;
    if (wb.wire.v == 0) {
        wb = odin3_net_primary(mod, run->nets[bit]);
        whole = false;
    }
    if (wb.wire.v == 0) {
        const char *name = odin3_strtab_get(tab, odin3_net_name(mod, run->nets[bit]));
        return odin3_strbuf_append(
            out, odin3_bytes_cstr(name != NULL && name[0] != '\0' ? name : "net"));
    }
    odin3_status st = odin3_strbuf_append(
        out, odin3_bytes_cstr(odin3_strtab_get(tab, odin3_wire_name(mod, wb.wire))));
    if (st == ODIN3_OK && !whole && odin3_wire_width(mod, wb.wire) > 1) {
        st = odin3_strbuf_appendf(out, "[%d]", (int)odin3_wire_index(mod, wb.wire, wb.bit));
    }
    return st;
}

/* "f.v:L:C" of an AST node (the place of a driver). */
static odin3_status where(const odin3_elab2 *e2, odin3_ast_id node, odin3_strbuf *out) {
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(e2->design, &sm);
    return st != ODIN3_OK ? st
                          : odin3_srcman_format(sm, odin3_ast_loc(e2->elab, node), out,
                                                odin3_design_diag_paths(e2->design));
}

/* What a conflict message says: the bit's name, the other driver's place, and the inout pin. */
typedef struct claim_text {
    const char *label, *place, *port, *instance;
    bool constant; /* the other driver has no place: a constant */
} claim_text;

/* The text of the conflict claim_run recorded, as one located error at loc. */
static void claim_emit(claim_run *run, odin3_loc loc, const claim_text *tx) {
    odin3_elab2 *e2 = run->e2;
    switch (run->code) {
    case CC_UWIRE:
        odin3_elab2_error(e2, loc, "'%s' is a uwire and has a second driver (first at %s)",
                          tx->label, tx->place);
        break;
    case CC_INOUT_LOGIC:
        odin3_elab2_error(e2, loc,
                          "net '%s' is driven by inout port '%s' of instance '%s' and by logic at "
                          "%s; internal tri-state nets are not supported (D3)",
                          tx->label, tx->port != NULL ? tx->port : "?",
                          tx->instance != NULL ? tx->instance : "?", tx->place);
        break;
    case CC_D3:
        odin3_elab2_error(e2, loc,
                          "tri-state driver on '%s' is supported only on a top-level output or "
                          "inout port (D3)",
                          tx->label);
        break;
    default:
        odin3_elab2_error(e2, loc,
                          tx->constant ? "'%s' has two drivers (also driven by %s)"
                                       : "'%s' has two drivers (also driven at %s)",
                          tx->label, tx->place);
        break;
    }
}

/* Where the conflict is reported: the duplicate's later part, else the bit's own piece, else the
 * request's node. */
static odin3_ast_id claim_here(const claim_run *run) {
    if (run->here.v != 0) {
        return run->here;
    }
    return run->bits != NULL && run->bits[run->at].node.v != 0 ? run->bits[run->at].node
                                                               : run->node;
}

/* The place the message names: the logic of an inout conflict (the new driver, unless the new one
 * is the pin), else the previous driver. */
static odin3_ast_id claim_other(const claim_run *run, odin3_ast_id here) {
    bool logic_here = run->code == CC_INOUT_LOGIC && run->kind != ODIN3_DRV_INOUT_PIN;
    return logic_here ? here : run->prev.node;
}

static odin3_status claim_report(claim_run *run) {
    const odin3_elab2 *e2 = run->e2;
    odin3_strbuf name;
    odin3_strbuf pos;
    odin3_strbuf_init(&name);
    odin3_strbuf_init(&pos);
    odin3_ast_id here = claim_here(run);
    odin3_ast_id other = claim_other(run, here);
    odin3_status st = bit_label(run, run->at, &name);
    st = st != ODIN3_OK || other.v == 0 ? st : where(e2, other, &pos);
    if (st == ODIN3_OK) {
        claim_text tx = {name.data != NULL ? name.data : "",
                         pos.data != NULL ? pos.data : "a constant", run->port, run->instance,
                         other.v == 0};
        if (run->code == CC_INOUT_LOGIC && run->kind != ODIN3_DRV_INOUT_PIN) {
            inout_pin pin = {tx.port, tx.instance};
            (void)child_inout(e2, run->nets[run->at], &pin);
            tx.port = pin.port;
            tx.instance = pin.instance;
        }
        claim_emit(run, odin3_ast_loc(e2->elab, here), &tx);
    }
    odin3_strbuf_free(&name);
    odin3_strbuf_free(&pos);
    return st;
}

/* --- claiming -------------------------------------------------------------------------------- */

/* Checks every bit; the first conflict is recorded in run (code, at, prev). */
static odin3_status claim_check(claim_run *run) {
    odin3_status st = ODIN3_OK;
    if (run->n > 1) {
        run->seen = odin3_u64map_create(0);
        if (run->seen == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < run->n && run->code == CC_OK; i++) {
        run->code = check_bit(run, i, &st);
        run->at = i;
    }
    odin3_u64map_destroy(run->seen);
    run->seen = NULL;
    return st;
}

/* Records the driver on every bit that is not dropped or wired. */
static odin3_status claim_record(claim_run *run) {
    odin3_status st = ODIN3_OK;
    odin3_drv entry = {run->kind, run->node};
    for (uint32_t i = 0; st == ODIN3_OK && i < run->n; i++) {
        if (run->nets[i].v == 0 || info_get(run->dr, run->nets[i]).wrec != 0) {
            continue;
        }
        odin3_drv mine = {run->kind, run->bits != NULL && run->bits[i].node.v != 0
                                         ? run->bits[i].node
                                         : run->node};
        st = drv_set(run->dr, run->nets[i], entry);
        if (st == ODIN3_OK && run->bits != NULL) {
            st = claim_put(run->dr, run->bits[i].wb, mine);
        }
    }
    return st;
}

static odin3_status claim_go(claim_run *run, bool *granted) {
    *granted = false;
    odin3_status st = claim_check(run);
    if (st != ODIN3_OK) {
        return st;
    }
    if (run->code != CC_OK) {
        return claim_report(run);
    }
    st = run->record ? claim_record(run) : ODIN3_OK;
    *granted = st == ODIN3_OK;
    return st;
}

odin3_status odin3_elab2_claim(odin3_elab2 *e2, const odin3_elab2_claim_req *req, bool *granted) {
    if (req == NULL || granted == NULL || e2->mod == NULL || req->kind == ODIN3_DRV_NONE ||
        req->kind > ODIN3_DRV_TRIBUF || req->nets == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_claim: bad argument, or no module is emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    struct odin3_elab2_drive *dr = drive_get(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    claim_run run = {.e2 = e2,
                     .dr = dr,
                     .nets = req->nets,
                     .n = req->n,
                     .kind = req->kind,
                     .node = req->node,
                     .record = true};
    return claim_go(&run, granted);
}

/* --- lvalues: claiming and connecting -------------------------------------------------------- */

/* A wired bit's driver goes onto a fresh net, which odin3_elab2_wired chains (EL-26). */
static odin3_status redirect_one(odin3_elab2 *e2, struct odin3_elab2_drive *dr, uint32_t index,
                                 odin3_elab2_lvalue *lv) {
    odin3_net_id *net = odin3_vec_at(&lv->nets, index);
    net_info info = info_get(dr, *net);
    odin3_loc at =
        odin3_ast_loc(e2->elab, ((odin3_elab2_lvbit *)odin3_vec_at(&lv->bits, index))->node);
    odin3_status st = odin3_elab2_reserve_nets(e2, 1, at);
    odin3_net_id fresh = {0};
    st = st != ODIN3_OK ? st : odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &fresh);
    wired_rec *rec = odin3_vec_at(&dr->wired, info.wrec - 1U);
    wired_op *op = st == ODIN3_OK ? odin3_vec_push(&rec->ops) : NULL;
    if (st == ODIN3_OK && op == NULL) {
        (void)odin3_net_delete(e2->mod, fresh);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    *op = (wired_op){info.wbit, fresh, dr->stmts};
    *net = fresh;
    ((odin3_elab2_lvbit *)odin3_vec_at(&lv->bits, index))->wb.wire.v = 0;
    odin3_elab2_counts made = {0, 0, 1, 0};
    return odin3_elab2_account(e2, &made);
}

static odin3_status redirect_wired(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                   odin3_elab2_lvalue *lv) {
    odin3_status st = ODIN3_OK;
    bool any = false;
    for (uint32_t i = 0; st == ODIN3_OK && i < lv->nets.len; i++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(&lv->nets, i);
        if (net.v != 0 && info_get(dr, net).wrec != 0) {
            st = redirect_one(e2, dr, i, lv);
            any = true;
        }
    }
    dr->stmts += any ? 1U : 0U;
    return st;
}

odin3_status odin3_elab2_lvalue_claim(odin3_elab2 *e2, const odin3_elab2_lvclaim *req,
                                      bool *granted) {
    if (req == NULL || req->lv == NULL || granted == NULL || e2->mod == NULL ||
        req->kind == ODIN3_DRV_NONE || req->kind > ODIN3_DRV_TRIBUF) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_lvalue_claim: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    struct odin3_elab2_drive *dr = drive_get(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    claim_run run = {.e2 = e2,
                     .dr = dr,
                     .nets = req->lv->nets.data,
                     .bits = req->lv->bits.data,
                     .n = (uint32_t)req->lv->nets.len,
                     .kind = req->kind,
                     .node = req->node,
                     .record = req->record,
                     .port = req->port,
                     .instance = req->instance};
    odin3_status st = claim_go(&run, granted);
    return st == ODIN3_OK && *granted ? redirect_wired(e2, dr, req->lv) : st;
}

/* Notes `lhs = rhs` where both are the net already: reported at the end if nothing drives it. */
static odin3_status note_loop(struct odin3_elab2_drive *dr, odin3_net_id net,
                              const odin3_elab2_lvconn *req, odin3_ast_id lhs) {
    loop_note *note = odin3_vec_push(&dr->loops);
    if (note == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *note = (loop_note){net, lhs, req->rhs};
    return ODIN3_OK;
}

/* One lvalue bit takes its net: by construction, as a self-assignment, or by merging. */
static odin3_status connect_bit(odin3_elab2 *e2, const odin3_elab2_lvconn *req, uint32_t index,
                                odin3_net_id rhs) {
    struct odin3_elab2_drive *dr = e2->drive;
    const odin3_elab2_lvbit *bit = odin3_vec_cat(&req->lv->bits, index);
    odin3_net_id lhs =
        odin3_elab2_resolve(e2, *(const odin3_net_id *)odin3_vec_cat(&req->lv->nets, index));
    rhs = odin3_elab2_resolve(e2, rhs);
    odin3_drv mine = {req->kind, bit->node};
    odin3_status st = ODIN3_OK;
    if (lhs.v == rhs.v && odin3_net_driver_count(e2->mod, lhs) == 0) {
        st = note_loop(dr, lhs, req, bit->node);
    } else if (lhs.v == rhs.v) {
        st = drv_set(dr, lhs, mine);
    } else {
        st = odin3_elab2_merge(e2, (odin3_net_pair){rhs, lhs});
    }
    return st != ODIN3_OK ? st : claim_put(dr, bit->wb, mine);
}

odin3_status odin3_elab2_lvalue_connect(odin3_elab2 *e2, const odin3_elab2_lvconn *req) {
    if (req == NULL || req->lv == NULL || req->sig == NULL || e2->mod == NULL ||
        req->sig->width != req->lv->nets.len || drive_get(e2) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_lvalue_connect: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < req->sig->width; i++) {
        if (((const odin3_net_id *)req->lv->nets.data)[i].v == 0) {
            continue;
        }
        odin3_sig one = {&req->sig->bits[i], 1, false};
        odin3_net_id rhs = {0};
        st = odin3_elab2_to_nets(e2, &one, &rhs);
        st = st != ODIN3_OK ? st : connect_bit(e2, req, i, rhs);
    }
    return st;
}

/* --- the symbols of the module ---------------------------------------------------------------- */

typedef odin3_status (*symbol_fn)(odin3_elab2 *e2, const odin3_symbol *rec);

/* fn on the net and variable symbols of scope that have a wire in the module being emitted. */
static odin3_status scope_symbols(odin3_elab2 *e2, odin3_scope_id scope, symbol_fn fn) {
    odin3_status st = ODIN3_OK;
    for (odin3_sym_id sym = odin3_scope_first_symbol(e2->st, scope); st == ODIN3_OK && sym.v != 0;
         sym = odin3_scope_next_symbol(e2->st, sym)) {
        const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
        bool variable = rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR;
        bool ours = rec->ir_wire.v != 0 && rec->ir_module.v == e2->mod_id.v;
        st = variable && ours ? fn(e2, rec) : ODIN3_OK;
    }
    return st;
}

/* The child scopes of scope onto the worklist. */
static odin3_status push_scopes(const odin3_elab2 *e2, odin3_vec *todo, odin3_scope_id scope) {
    for (odin3_scope_id kid = odin3_scope_first_child(e2->st, scope); kid.v != 0;
         kid = odin3_scope_next_sibling(e2->st, kid)) {
        odin3_scope_id *slot = odin3_vec_push(todo);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = kid;
    }
    return ODIN3_OK;
}

/* Calls fn on every net and variable symbol of the module being emitted that has a wire, in the
 * module's scope and its generate and named-block scopes (a worklist, no recursion). */
static odin3_status each_symbol(odin3_elab2 *e2, odin3_ast_id module_node, symbol_fn fn) {
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(odin3_scope_id));
    odin3_scope_id *slot = odin3_vec_push(&todo);
    odin3_status st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (slot != NULL) {
        *slot = odin3_elab_scope_of(&e2->bind, module_node);
    }
    while (st == ODIN3_OK && todo.len > 0) {
        odin3_scope_id scope = *(const odin3_scope_id *)odin3_vec_cat(&todo, todo.len - 1);
        odin3_vec_pop(&todo);
        st = scope_symbols(e2, scope, fn);
        st = st != ODIN3_OK ? st : push_scopes(e2, &todo, scope);
    }
    odin3_vec_free(&todo);
    return st;
}

/* --- step (2): the pre-scan ------------------------------------------------------------------ */

/* The input ports are driven from outside; the module's own inout pins drive their nets. */
static odin3_status prescan_ports(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                  odin3_ast_id module_node) {
    odin3_ast_span ports = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, 1));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ports.n; i++) {
        uint32_t dir = odin3_ast_sub(e2->elab, ports.ids[i]);
        odin3_ast_id declarator = odin3_ast_child(e2->elab, ports.ids[i], 1);
        const odin3_symbol *rec =
            odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, declarator));
        if (rec == NULL || rec->ir_wire.v == 0 || dir == ODIN3_AST_PORT_DECL_OUTPUT) {
            continue;
        }
        odin3_drv entry = {
            dir == ODIN3_AST_PORT_DECL_INOUT ? ODIN3_DRV_INOUT_PIN : ODIN3_DRV_PORT_IN, declarator};
        uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
        for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
            st = drv_set(dr, odin3_wire_net(e2->mod, rec->ir_wire, bit), entry);
        }
    }
    return st;
}

/* A wired wire: a record, and each of its nets knows its bit. */
static odin3_status register_wired(const odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                   const odin3_symbol *rec, bool is_or) {
    wired_rec *slot = odin3_vec_push(&dr->wired);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_init(&slot->ops, sizeof(wired_op));
    slot->wire = rec->ir_wire;
    slot->is_or = is_or;
    slot->declarator = rec->declarator;
    slot->scope = rec->scope.v;
    uint32_t index = (uint32_t)dr->wired.len;
    uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        net_info *info = net_slot(&dr->info, odin3_wire_net(e2->mod, rec->ir_wire, bit));
        if (info == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *info = (net_info){index, bit};
    }
    return ODIN3_OK;
}

/* `supply0`/`supply1`: the wire's nets become the shared constant net; any driver is A4's. */
static odin3_status register_supply(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                    const odin3_symbol *rec, odin3_bit bit) {
    odin3_net_id shared = {0};
    odin3_status st = odin3_elab2_const_net(e2, bit, &shared);
    uint32_t width = odin3_wire_width(e2->mod, rec->ir_wire);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        odin3_net_id net = odin3_elab2_resolve(e2, odin3_wire_net(e2->mod, rec->ir_wire, k));
        st = net.v != shared.v ? odin3_elab2_merge(e2, (odin3_net_pair){shared, net}) : ODIN3_OK;
        st = st != ODIN3_OK ? st
                            : claim_put(dr, (odin3_wirebit){rec->ir_wire, k},
                                        (odin3_drv){ODIN3_DRV_CONST, rec->declarator});
    }
    return st;
}

/* The net kinds with driver rules (EL-26) of one symbol. */
static odin3_status prescan_symbol(odin3_elab2 *e2, const odin3_symbol *rec) {
    struct odin3_elab2_drive *dr = e2->drive;
    if (rec->kind != ODIN3_SYM_NET) {
        return ODIN3_OK;
    }
    switch (rec->sub) {
    case ODIN3_AST_NET_DECL_UWIRE:
        return odin3_u64map_put(dr->uwires, (odin3_kv){rec->ir_wire.v, 1});
    case ODIN3_AST_NET_DECL_WAND:
    case ODIN3_AST_NET_DECL_TRIAND:
        return register_wired(e2, dr, rec, false);
    case ODIN3_AST_NET_DECL_WOR:
    case ODIN3_AST_NET_DECL_TRIOR:
        return register_wired(e2, dr, rec, true);
    case ODIN3_AST_NET_DECL_SUPPLY0:
        return register_supply(e2, dr, rec, ODIN3_BIT_0);
    case ODIN3_AST_NET_DECL_SUPPLY1:
        return register_supply(e2, dr, rec, ODIN3_BIT_1);
    default:
        return ODIN3_OK;
    }
}

/* --- the always-assigned bit set ------------------------------------------------------------- */

/* The flags of the object sym names: one per bit of its wire (a memory: one in all), made on first
 * use; NULL on out of memory. */
static uint8_t *always_flags(odin3_elab2 *e2, odin3_sym_id sym, uint32_t *width) {
    struct odin3_elab2_drive *dr = e2->drive;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    *width =
        rec->ir_wire.v != 0 && rec->n_unpacked == 0 ? odin3_wire_width(e2->mod, rec->ir_wire) : 1U;
    uint64_t at = 0;
    if (!odin3_u64map_get(dr->always, sym.v, &at)) {
        at = dr->abits.len;
        for (uint32_t i = 0; i < *width; i++) {
            if (odin3_vec_push(&dr->abits) == NULL) {
                return NULL;
            }
        }
        if (odin3_u64map_put(dr->always, (odin3_kv){sym.v, at}) != ODIN3_OK) {
            return NULL;
        }
    }
    return (uint8_t *)dr->abits.data + at;
}

bool odin3_elab2_always_assigned(const odin3_elab2 *e2, odin3_sym_id sym, uint32_t bit) {
    uint64_t at = 0;
    if (e2->drive == NULL || !odin3_u64map_get(e2->drive->always, sym.v, &at)) {
        return false;
    }
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    uint32_t width =
        rec->ir_wire.v != 0 && rec->n_unpacked == 0 ? odin3_wire_width(e2->mod, rec->ir_wire) : 1U;
    return bit < width && ((const uint8_t *)e2->drive->abits.data)[at + bit] != 0;
}

static const char *const k_wired_names[] = {[ODIN3_AST_NET_DECL_WAND] = "wand",
                                            [ODIN3_AST_NET_DECL_WOR] = "wor",
                                            [ODIN3_AST_NET_DECL_TRIAND] = "triand",
                                            [ODIN3_AST_NET_DECL_TRIOR] = "trior"};

static bool is_wired_kind(const odin3_symbol *rec) {
    return rec->kind == ODIN3_SYM_NET &&
           (rec->sub == ODIN3_AST_NET_DECL_WAND || rec->sub == ODIN3_AST_NET_DECL_WOR ||
            rec->sub == ODIN3_AST_NET_DECL_TRIAND || rec->sub == ODIN3_AST_NET_DECL_TRIOR);
}

/* The bits of a select whose index is a NUMBER, from..to inclusive (positions), else the whole
 * object: reports whether the select's bits are exact. */
static bool select_span(odin3_elab2 *e2, odin3_ast_id sel, uint32_t width, uint32_t span[2]) {
    odin3_ast_id object = odin3_ast_child(e2->elab, sel, 0);
    uint32_t sub = odin3_ast_sub(e2->elab, sel);
    odin3_ast_id first = odin3_ast_child(e2->elab, sel, 1);
    odin3_ast_id second = odin3_ast_child(e2->elab, sel, 2);
    bool numbers =
        odin3_ast_kind_of(e2->elab, first) == ODIN3_AST_NUMBER &&
        (sub == ODIN3_AST_SELECT_BIT ||
         (sub == ODIN3_AST_SELECT_PART && odin3_ast_kind_of(e2->elab, second) == ODIN3_AST_NUMBER));
    odin3_elab2_selobj obj;
    int64_t idx[2] = {0, 0};
    bool known[2] = {false, false};
    if (!numbers) {
        return false;
    }
    odin3_elab2_sel_object(e2, object, width, &obj);
    if (odin3_elab2_number_i64(e2, first, &idx[0], &known[0]) != ODIN3_OK || !known[0] ||
        (sub == ODIN3_AST_SELECT_PART &&
         (odin3_elab2_number_i64(e2, second, &idx[1], &known[1]) != ODIN3_OK || !known[1]))) {
        return false;
    }
    idx[1] = sub == ODIN3_AST_SELECT_BIT ? idx[0] : idx[1];
    int64_t pos[2] = {odin3_elab2_sel_pos(&obj, idx[0]), odin3_elab2_sel_pos(&obj, idx[1])};
    if (pos[0] < 0 || pos[1] < 0) {
        return false;
    }
    span[0] = (uint32_t)(pos[0] < pos[1] ? pos[0] : pos[1]);
    span[1] = (uint32_t)(pos[0] < pos[1] ? pos[1] : pos[0]);
    return true;
}

/* An object assigned in an always block: bits span[0]..span[1] (or all of them), and the wired
 * check. */
static odin3_status mark_object(odin3_elab2 *e2, odin3_ast_id name, const uint32_t *span) {
    odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, name);
    const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
    if (rec == NULL || (rec->kind != ODIN3_SYM_NET && rec->kind != ODIN3_SYM_VAR)) {
        return ODIN3_OK;
    }
    if (is_wired_kind(rec)) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, name),
                          "'%s' is a wired net (%s) and cannot be assigned in an always block",
                          odin3_strtab_get(odin3_design_strtab(e2->design), rec->name),
                          k_wired_names[rec->sub]);
        return ODIN3_OK;
    }
    uint32_t width = 0;
    uint8_t *flags = always_flags(e2, sym, &width);
    if (flags == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t from = span != NULL && span[1] < width ? span[0] : 0;
    uint32_t to = span != NULL && span[1] < width ? span[1] : width - 1U;
    memset(flags + from, 1, (size_t)to - from + 1U);
    return ODIN3_OK;
}

/* One lvalue node of an always assignment: a concatenation is its parts, a select its object. */
static odin3_status mark_lvalue_node(odin3_elab2 *e2, odin3_vec *work, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e2->elab, node)) {
    case ODIN3_AST_CONCAT: {
        odin3_ast_span parts = odin3_ast_children(e2->elab, node);
        for (uint32_t i = 0; i < parts.n; i++) {
            odin3_ast_id *slot = odin3_vec_push(work);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = parts.ids[i];
        }
        return ODIN3_OK;
    }
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return mark_object(e2, node, NULL);
    case ODIN3_AST_SELECT: {
        odin3_ast_id object = odin3_ast_child(e2->elab, node, 0);
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, object);
        if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
            return ODIN3_OK; /* the process generator reports a select of an expression */
        }
        const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, object));
        uint32_t span[2] = {0, 0};
        bool exact = rec != NULL && rec->ir_wire.v != 0 && rec->n_unpacked == 0 &&
                     select_span(e2, node, odin3_wire_width(e2->mod, rec->ir_wire), span);
        return mark_object(e2, object, exact ? span : NULL);
    }
    default:
        return ODIN3_OK;
    }
}

/* The lvalue of one assignment inside an always block. */
static odin3_status mark_lvalue(odin3_elab2 *e2, odin3_ast_id lhs) {
    odin3_vec work;
    odin3_vec_init(&work, sizeof(odin3_ast_id));
    odin3_ast_id *slot = odin3_vec_push(&work);
    odin3_status st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (slot != NULL) {
        *slot = lhs;
    }
    while (st == ODIN3_OK && work.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&work, work.len - 1);
        odin3_vec_pop(&work);
        st = mark_lvalue_node(e2, &work, node);
    }
    odin3_vec_free(&work);
    return st;
}

/* Every blocking and nonblocking assignment below one ALWAYS (functions and tasks are inlined
 * where they are called, EL-38). */
static odin3_status scan_one_always(odin3_elab2 *e2, odin3_ast_id always) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, always);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev == ODIN3_AST_ENTER &&
            (kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN)) {
            st = mark_lvalue(e2, odin3_ast_child(e2->elab, node, 0));
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* The ALWAYS blocks of the module and its generate blocks. */
static odin3_status scan_always(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, module_node);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && !e2->stopped && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev != ODIN3_AST_ENTER || kind == ODIN3_AST_MODULE || kind == ODIN3_AST_GEN_BLOCK) {
            continue;
        }
        st = kind == ODIN3_AST_ALWAYS ? scan_one_always(e2, node) : ODIN3_OK;
        odin3_ast_cursor_skip(&cur);
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

odin3_status odin3_elab2_prescan(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_prescan: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    struct odin3_elab2_drive *dr = drive_get(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = prescan_ports(e2, dr, module_node);
    st = st != ODIN3_OK ? st : each_symbol(e2, module_node, prescan_symbol);
    st = st != ODIN3_OK ? st : scan_always(e2, module_node);
    return st != ODIN3_OK ? st : e2->fatal;
}

/* --- step (7): wired nets -------------------------------------------------------------------- */

/* One two-input cell of the chain, acc op operand (both as wide as the cell): to dest when given,
 * else on fresh nets; acc becomes its Y signal. */
static odin3_status chain_cell(odin3_elab2 *e2, const wired_rec *rec, odin3_sig *acc,
                               const odin3_sig *operand, const odin3_net_id *dest) {
    odin3_elab2_cell_req req = {.type = rec->is_or ? ODIN3_E2T_OR : ODIN3_E2T_AND,
                                .node = rec->declarator,
                                .a = acc,
                                .b = operand,
                                .y_width = acc->width,
                                .dest = dest};
    return odin3_elab2_cell(e2, &req, acc);
}

/* The ops are whole drivers: runs of exactly `width` ops, one statement each, bits in order. */
static bool all_whole(const wired_rec *rec, uint32_t width) {
    const wired_op *ops = rec->ops.data;
    size_t i = 0;
    while (i < rec->ops.len) {
        for (uint32_t k = 0; k < width; k++) {
            if (i + k >= rec->ops.len || ops[i + k].stmt != ops[i].stmt || ops[i + k].bit != k) {
                return false;
            }
        }
        if (i + width < rec->ops.len && ops[i + width].stmt == ops[i].stmt) {
            return false;
        }
        i += width;
    }
    return true;
}

/* Merges the single driver of a wired bit into the wire's net. */
static odin3_status join_single(odin3_elab2 *e2, odin3_net_id wire_net, odin3_net_id driver) {
    wire_net = odin3_elab2_resolve(e2, wire_net);
    driver = odin3_elab2_resolve(e2, driver);
    return wire_net.v == driver.v ? ODIN3_OK
                                  : odin3_elab2_merge(e2, (odin3_net_pair){wire_net, driver});
}

/* The nets of ops[first .. first + width) as a signal. */
static odin3_status ops_sig(odin3_elab2 *e2, const wired_op *ops, uint32_t width, odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        out->bits[i] = odin3_elab2_resolve(e2, ops[i].net).v;
    }
    return st;
}

/* The wire's own nets, LSB first, in the scratch arena; NULL on out of memory. */
static odin3_net_id *wire_nets(odin3_elab2 *e2, odin3_wire_id wire, uint32_t width) {
    odin3_net_id *nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *nets);
    for (uint32_t i = 0; nets != NULL && i < width; i++) {
        nets[i] = odin3_wire_net(e2->mod, wire, i);
    }
    return nets;
}

/* k whole drivers: one driver is merged into the wire, else k - 1 word cells of its width. */
static odin3_status chain_words(odin3_elab2 *e2, const wired_rec *rec, uint32_t width) {
    const wired_op *ops = rec->ops.data;
    uint32_t drivers = (uint32_t)(rec->ops.len / width);
    const odin3_net_id *own = wire_nets(e2, rec->wire, width);
    if (own == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    if (drivers == 1) {
        for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
            st = join_single(e2, own[k], ops[k].net);
        }
        return st;
    }
    odin3_sig acc;
    odin3_sig operand;
    st = ops_sig(e2, ops, width, &acc);
    for (uint32_t j = 1; st == ODIN3_OK && j < drivers; j++) {
        st = ops_sig(e2, ops + (size_t)j * width, width, &operand);
        st = st != ODIN3_OK ? st
                            : chain_cell(e2, rec, &acc, &operand, j == drivers - 1U ? own : NULL);
    }
    return st;
}

/* Bit k of a wire: its ops in order, a one-bit chain; counts[k] of them in all. */
typedef struct bit_chain {
    uint32_t *total, *seen;
    odin3_net_id *run; /* the accumulated net of each bit */
    odin3_net_id *own;
} bit_chain;

static odin3_status chain_step(odin3_elab2 *e2, const wired_rec *rec, const bit_chain *bc,
                               const wired_op *op) {
    uint32_t k = op->bit;
    odin3_net_id net = odin3_elab2_resolve(e2, op->net);
    if (bc->seen[k]++ == 0) {
        bc->run[k] = net;
        return ODIN3_OK;
    }
    odin3_sigbit a_bit = odin3_elab2_resolve(e2, bc->run[k]).v;
    odin3_sigbit b_bit = net.v;
    odin3_sig acc = {&a_bit, 1, false};
    odin3_sig operand = {&b_bit, 1, false};
    bool last = bc->seen[k] == bc->total[k];
    odin3_status st = chain_cell(e2, rec, &acc, &operand, last ? &bc->own[k] : NULL);
    bc->run[k].v = st == ODIN3_OK ? acc.bits[0] : 0U;
    return st;
}

/* Partial drivers: a chain per bit over the drivers that cover it. */
static odin3_status chain_bits(odin3_elab2 *e2, const wired_rec *rec, uint32_t width) {
    const wired_op *ops = rec->ops.data;
    bit_chain bc = {odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(uint32_t)),
                    odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(uint32_t)),
                    odin3_arena_alloc(e2->scratch, (size_t)width * sizeof(odin3_net_id)),
                    wire_nets(e2, rec->wire, width)};
    if (bc.total == NULL || bc.seen == NULL || bc.run == NULL || bc.own == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(bc.total, 0, (size_t)width * sizeof(uint32_t));
    memset(bc.seen, 0, (size_t)width * sizeof(uint32_t));
    memset(bc.run, 0, (size_t)width * sizeof(odin3_net_id));
    for (size_t i = 0; i < rec->ops.len; i++) {
        bc.total[ops[i].bit]++;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rec->ops.len; i++) {
        st = chain_step(e2, rec, &bc, &ops[i]);
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        st = bc.total[k] == 1 ? join_single(e2, bc.own[k], bc.run[k]) : ODIN3_OK;
    }
    return st;
}

odin3_status odin3_elab2_wired(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (e2->mod == NULL || e2->mod_node.v != module_node.v) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_wired: not the module being emitted");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    size_t count = e2->drive != NULL ? e2->drive->wired.len : 0;
    for (size_t i = 0; st == ODIN3_OK && i < count && !e2->stopped; i++) {
        const wired_rec *rec = odin3_vec_cat(&e2->drive->wired, i);
        uint32_t width = odin3_wire_width(e2->mod, rec->wire);
        e2->scope = (odin3_scope_id){rec->scope};
        if (rec->ops.len == 0) {
            continue;
        }
        st = all_whole(rec, width) ? chain_words(e2, rec, width) : chain_bits(e2, rec, width);
    }
    e2->scope = (odin3_scope_id){0};
    return st != ODIN3_OK ? st : e2->fatal;
}

/* --- the end of the module: warnings ---------------------------------------------------------- */

/* The name an lvalue or rhs node reads as in a message (a select reads as its object). */
static const char *node_label(const odin3_elab2 *e2, odin3_ast_id node) {
    while (odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_SELECT) {
        node = odin3_ast_child(e2->elab, node, 0);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME
               ? odin3_ast_name_str(e2->elab, node)
               : "expression";
}

/* The self-assignments whose net is still undriven. */
static odin3_status warn_loops(odin3_elab2 *e2, struct odin3_elab2_drive *dr) {
    for (size_t i = 0; i < dr->loops.len; i++) {
        const loop_note *note = odin3_vec_cat(&dr->loops, i);
        odin3_net_id net = odin3_elab2_resolve(e2, note->net);
        if (odin3_net_driver_count(e2->mod, net) > 0) {
            continue;
        }
        odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, note->lhs),
                         "'%s' and '%s' are connected only to each other and are never driven",
                         node_label(e2, note->rhs), node_label(e2, note->lhs));
        odin3_status st = odin3_u64map_put(dr->covered, (odin3_kv){net.v, 1});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A wire with sinks and no driver warns, once; a port is left to rule 4 of the check. */
static odin3_status warn_undriven(odin3_elab2 *e2, const odin3_symbol *rec) {
    const struct odin3_elab2_drive *dr = e2->drive;
    const odin3_module *mod = e2->mod;
    if (rec->n_unpacked > 0 || odin3_wire_port_node(mod, rec->ir_wire).v != 0) {
        return ODIN3_OK;
    }
    uint32_t width = odin3_wire_width(mod, rec->ir_wire);
    for (uint32_t k = 0; k < width; k++) {
        odin3_net_id net = odin3_wire_net(mod, rec->ir_wire, k);
        if (odin3_net_sinks(mod, net).count > 0 && odin3_net_driver_count(mod, net) == 0 &&
            !odin3_u64map_get(dr->covered, net.v, NULL)) {
            odin3_elab2_warn(e2, odin3_ast_loc(e2->elab, rec->declarator), "'%s' is never driven",
                             odin3_strtab_get(odin3_design_strtab(e2->design),
                                              odin3_wire_name(mod, rec->ir_wire)));
            break;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_elab2_undriven(odin3_elab2 *e2) {
    struct odin3_elab2_drive *dr = drive_get(e2);
    if (e2->mod == NULL || dr == NULL) {
        return e2->mod == NULL ? ODIN3_ERR_INVALID_ARG : ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = warn_loops(e2, dr);
    st = st != ODIN3_OK ? st : each_symbol(e2, e2->mod_node, warn_undriven);
    return st != ODIN3_OK ? st : e2->fatal;
}

/* --- tri-state (D3) -------------------------------------------------------------------------- */

/* The unary cell op over a one-bit or wider signal, giving a one-bit net in *out. */
static odin3_status tri_unary(odin3_elab2 *e2, odin3_elab2_type type, odin3_ast_id node,
                              const odin3_sig *operand, odin3_net_id *out) {
    odin3_elab2_cell_req req = {.type = type,
                                .node = node,
                                .a = operand,
                                .y_width = type == ODIN3_E2T_NOT ? operand->width : 1};
    odin3_sig result;
    odin3_status st = odin3_elab2_cell(e2, &req, &result);
    return st != ODIN3_OK ? st : odin3_elab2_to_nets(e2, &result, out);
}

/* EN from the condition: a bit as is or through the cell the width and the inversion ask for. */
static odin3_status tri_enable(odin3_elab2 *e2, const odin3_elab2_tri_req *req,
                               const odin3_sig *cond, odin3_net_id *out) {
    if (cond->width == 1 && !req->invert) {
        return odin3_elab2_to_nets(e2, cond, out);
    }
    odin3_elab2_type type = req->invert ? ODIN3_E2T_LOGIC_NOT : ODIN3_E2T_REDUCE_BOOL;
    if (cond->width == 1) {
        type = ODIN3_E2T_NOT; /* only reached inverted */
    }
    return tri_unary(e2, type, req->node, cond, out);
}

/* The data and condition signals: generated from the nodes, or the ones given. */
static odin3_status tri_operands(odin3_elab2 *e2, const odin3_elab2_tri_req *req, odin3_sig *data,
                                 odin3_sig *cond) {
    uint32_t width = (uint32_t)req->lv->nets.len;
    odin3_status st = ODIN3_OK;
    if (req->data_node.v != 0) {
        odin3_elab2_expr_req dreq = {req->data_node, {width, true, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &dreq, data);
    } else {
        *data = *req->data;
    }
    st = st != ODIN3_OK ? st : odin3_elab2_extend(e2, data, width, data->is_signed, data);
    if (st == ODIN3_OK && req->cond_node.v != 0) {
        odin3_elab2_expr_req creq = {req->cond_node, {0, false, false}, NULL, NULL};
        st = odin3_elab2_expr(e2, &creq, cond);
    } else if (st == ODIN3_OK) {
        *cond = *req->cond;
    }
    return st;
}

/* The tristate cell: A (data), EN, Y (the lvalue's nets, fresh ones for dropped bits). */
static odin3_status tri_cell(odin3_elab2 *e2, const odin3_elab2_tri_req *req, const odin3_sig *data,
                             odin3_net_id enable) {
    uint32_t width = data->width;
    uint64_t pins = 2ULL * width + 1U;
    odin3_loc at = odin3_ast_loc(e2->elab, req->node);
    if (pins > e2->limits->max_cell_pins) {
        odin3_elab2_fatal(e2, at, "cell of %" PRIu64 " pins exceeds the limit of %" PRIu32 " pins",
                          pins, e2->limits->max_cell_pins);
        return ODIN3_ERR_PARSE;
    }
    odin3_net_id *data_nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *data_nets);
    odin3_net_id *out_nets = odin3_arena_alloc(e2->scratch, (size_t)width * sizeof *out_nets);
    if (data_nets == NULL || out_nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_elab2_to_nets(e2, data, data_nets);
    uint32_t fresh = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        out_nets[i] = ((const odin3_net_id *)req->lv->nets.data)[i];
        if (out_nets[i].v == 0) {
            st = odin3_elab2_reserve_nets(e2, 1, at);
            st = st != ODIN3_OK
                     ? st
                     : odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &out_nets[i]);
            fresh++;
        }
    }
    odin3_elab2_prov_req preq = {req->node, {0}, e2->scope.v};
    odin3_prov_id prov = {0};
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &preq, &prov);
    odin3_value param = odin3_value_int((int64_t)width);
    odin3_node_spec spec = {
        .type = e2->types[ODIN3_E2T_TRIBUF], .prov = prov, .params = &param, .n_params = 1};
    odin3_netvec ports[3] = {{data_nets, width}, {&enable, 1}, {out_nets, width}};
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : odin3_node_create_connected(e2->mod, &spec, ports, &node);
    st = st != ODIN3_OK ? st
                        : odin3_elab2_attrs(e2, req->node, (odin3_objref){ODIN3_OBJ_NODE, node.v});
    odin3_elab2_counts made = {1, (uint32_t)pins, fresh, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* The data inverted through a $not (notif), in place. */
static odin3_status tri_invert_data(odin3_elab2 *e2, const odin3_elab2_tri_req *req,
                                    odin3_sig *data) {
    odin3_elab2_cell_req nreq = {
        .type = ODIN3_E2T_NOT, .node = req->node, .a = data, .y_width = data->width};
    return odin3_elab2_cell(e2, &nreq, data);
}

odin3_status odin3_elab2_tribuf(odin3_elab2 *e2, const odin3_elab2_tri_req *req) {
    if (req == NULL || req->lv == NULL || e2->mod == NULL ||
        (req->data_node.v == 0 && req->data == NULL) ||
        (req->cond_node.v == 0 && req->cond == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_tribuf: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    bool granted = false;
    odin3_elab2_lvclaim claim = {req->lv, ODIN3_DRV_TRIBUF, req->claim, true, NULL, NULL};
    odin3_status st = odin3_elab2_lvalue_claim(e2, &claim, &granted);
    if (st != ODIN3_OK || !granted) {
        return st != ODIN3_OK ? st : ODIN3_ERR_PARSE;
    }
    odin3_sig data;
    odin3_sig cond;
    odin3_net_id enable = {0};
    st = tri_operands(e2, req, &data, &cond);
    st = st == ODIN3_OK && req->invert_data ? tri_invert_data(e2, req, &data) : st;
    st = st != ODIN3_OK ? st : tri_enable(e2, req, &cond, &enable);
    return st != ODIN3_OK ? st : tri_cell(e2, req, &data, enable);
}
