/*
 * elab2_claim.c — stage 2's driver tables and claims (2D Task 19; EL-26, EL-28, EL-29, A4):
 * the per-module tables, the merge that remembers where a net went, and the claim every driver
 * makes before it attaches (A4, uwire, supply, wired nets, tri-state and inout rules).
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
#include "frontends/elab/elab2_drive.h"
#include "frontends/elab/elab2_mem.h"
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

/* --- the tables ------------------------------------------------------------------------------ */

void odin3_elab2_drive_free(odin3_elab2 *e2) {
    struct odin3_elab2_drive *dr = e2->drive;
    if (dr == NULL) {
        return;
    }
    for (size_t i = 0; i < dr->wired.len; i++) {
        odin3_vec_free(&((odin3_elab2_wired_rec *)odin3_vec_at(&dr->wired, i))->ops);
    }
    odin3_vec_free(&dr->drv);
    odin3_vec_free(&dr->info);
    odin3_vec_free(&dr->wired);
    odin3_vec_free(&dr->abits);
    odin3_vec_free(&dr->wbits);
    odin3_vec_free(&dr->loops);
    odin3_u64map_destroy(dr->claims);
    odin3_u64map_destroy(dr->uwires);
    odin3_u64map_destroy(dr->fwd);
    odin3_u64map_destroy(dr->always);
    odin3_u64map_destroy(dr->covered);
    odin3_u64map_destroy(dr->words);
    odin3_util_free(dr);
    e2->drive = NULL;
}

void odin3_elab2_drive_reset(odin3_elab2 *e2) {
    odin3_elab2_drive_free(e2);
}

/* The tables of the module being emitted, made on first use; NULL on out of memory. */
struct odin3_elab2_drive *odin3_elab2_drive_tables(odin3_elab2 *e2) {
    if (e2->drive != NULL) {
        return e2->drive;
    }
    struct odin3_elab2_drive *dr = odin3_util_calloc(sizeof *dr);
    if (dr == NULL) {
        return NULL;
    }
    odin3_vec_init(&dr->drv, sizeof(odin3_drv));
    odin3_vec_init(&dr->info, sizeof(odin3_elab2_net_info));
    odin3_vec_init(&dr->wired, sizeof(odin3_elab2_wired_rec));
    odin3_vec_init(&dr->abits, sizeof(uint8_t));
    odin3_vec_init(&dr->wbits, sizeof(uint8_t));
    odin3_vec_init(&dr->loops, sizeof(odin3_elab2_loop_note));
    dr->claims = odin3_u64map_create(0);
    dr->uwires = odin3_u64map_create(0);
    dr->fwd = odin3_u64map_create(0);
    dr->always = odin3_u64map_create(0);
    dr->covered = odin3_u64map_create(0);
    dr->words = odin3_u64map_create(0);
    odin3_ast_id unit = odin3_ast_root(e2->elab, 0);
    dr->is_top = e2->mod_node.v != 0 && odin3_ast_child(e2->elab, unit, 0).v == e2->mod_node.v;
    e2->drive = dr;
    if (dr->claims == NULL || dr->uwires == NULL || dr->fwd == NULL || dr->always == NULL ||
        dr->covered == NULL || dr->words == NULL) {
        odin3_elab2_drive_free(e2);
        return NULL;
    }
    return dr;
}

/* The slot of net in a per-net vec, growing it (zeroed) to hold it; NULL on out of memory. */
void *odin3_elab2_net_slot(odin3_vec *vec, odin3_net_id net) {
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

odin3_elab2_net_info odin3_elab2_info_get(const struct odin3_elab2_drive *dr, odin3_net_id net) {
    return net.v < dr->info.len ? *(const odin3_elab2_net_info *)odin3_vec_cat(&dr->info, net.v)
                                : (odin3_elab2_net_info){0, 0};
}

odin3_status odin3_elab2_drv_set(struct odin3_elab2_drive *dr, odin3_net_id net, odin3_drv entry) {
    odin3_drv *slot = odin3_elab2_net_slot(&dr->drv, net);
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
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_u64map_put(dr->fwd, (odin3_kv){pair.drop.v, pair.keep.v});
    st = st != ODIN3_OK ? st : odin3_net_merge(e2->mod, pair);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_drv gone = drv_get(dr, pair.drop);
    odin3_elab2_net_info gone_info = odin3_elab2_info_get(dr, pair.drop);
    st = gone.kind != ODIN3_DRV_NONE ? odin3_elab2_drv_set(dr, pair.keep, gone) : ODIN3_OK;
    if (st == ODIN3_OK && gone_info.wrec != 0 && odin3_elab2_info_get(dr, pair.keep).wrec == 0) {
        odin3_elab2_net_info *slot = odin3_elab2_net_slot(&dr->info, pair.keep);
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

odin3_drv odin3_elab2_bit_driver(const odin3_elab2 *e2, odin3_wirebit wb) {
    const struct odin3_elab2_drive *dr = e2->drive;
    if (dr == NULL || wb.wire.v == 0) {
        return (odin3_drv){ODIN3_DRV_NONE, {0}};
    }
    odin3_drv claim = claim_get(dr, wb);
    if (claim.kind != ODIN3_DRV_NONE) {
        return claim;
    }
    odin3_net_id net = odin3_elab2_resolve(e2, odin3_wire_net(e2->mod, wb.wire, wb.bit));
    return drv_get(dr, net);
}

odin3_status odin3_elab2_claim_put(struct odin3_elab2_drive *dr, odin3_wirebit wb,
                                   odin3_drv entry) {
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
    if (net.v == 0 || odin3_elab2_info_get(run->dr, net).wrec != 0) {
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
odin3_status odin3_elab2_where(const odin3_elab2 *e2, odin3_ast_id node, odin3_strbuf *out) {
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
        if (run->kind == ODIN3_DRV_PROCESS && run->prev.kind == ODIN3_DRV_PROCESS) { /* T10 */
            odin3_elab2_error(e2, loc, "'%s' is assigned in two always blocks (also at %s)",
                              tx->label, tx->place);
            break;
        }
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
    st = st != ODIN3_OK || other.v == 0 ? st : odin3_elab2_where(e2, other, &pos);
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
        if (run->nets[i].v == 0 || odin3_elab2_info_get(run->dr, run->nets[i]).wrec != 0) {
            continue;
        }
        odin3_drv mine = {run->kind, run->bits != NULL && run->bits[i].node.v != 0
                                         ? run->bits[i].node
                                         : run->node};
        st = odin3_elab2_drv_set(run->dr, run->nets[i], entry);
        if (st == ODIN3_OK && run->bits != NULL) {
            st = odin3_elab2_claim_put(run->dr, run->bits[i].wb, mine);
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
    struct odin3_elab2_drive *dr = odin3_elab2_drive_tables(e2);
    if (dr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < req->n; i++) {
        if (odin3_elab2_info_get(dr, req->nets[i]).wrec != 0) {
            odin3_log(
                ODIN3_LOG_ERROR,
                "odin3_elab2_claim: a wired net needs odin3_elab2_lvalue_claim (it redirects)");
            *granted = false;
            return ODIN3_ERR_INVALID_ARG;
        }
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

odin3_status odin3_elab2_claim_lvalue(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                      const odin3_elab2_lvclaim *req, bool *granted) {
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
    return claim_go(&run, granted);
}
