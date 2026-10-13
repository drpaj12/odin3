/*
 * elab2_drive.h — the driver tables of stage 2, shared by the files that split the drivers
 * (elab2_claim.c, elab2_wired.c, elab2_prescan.c, elab2_warn.c, elab2_tri.c). Private to
 * src/frontends/elab/: the rest of stage 2 sees only elab_internal.h's opaque
 * `struct odin3_elab2_drive` and the entry points declared there.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB2_DRIVE_H
#define ODIN3_FRONTENDS_ELAB_ELAB2_DRIVE_H

#include "ast/symtab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_elab2_net_info {
    uint32_t wrec; /* 1 + the wired record whose wire the net is a bit of; 0: none */
    uint32_t wbit; /* that bit of the wire */
} odin3_elab2_net_info;

/* One driver of a bit of a wired wire: the fresh net it drives and the statement it came from. */
typedef struct odin3_elab2_wired_op {
    uint32_t bit;
    odin3_net_id net;
    uint32_t stmt;
} odin3_elab2_wired_op;

/* A `wand`/`wor`/`triand`/`trior` wire and the drivers collected for it, in source order. */
typedef struct odin3_elab2_wired_rec {
    odin3_wire_id wire;
    bool is_or;
    odin3_ast_id declarator;
    uint32_t scope;
    odin3_vec ops; /* odin3_elab2_wired_op */
} odin3_elab2_wired_rec;

/* A `p = q` whose two sides are one net already: reported at the end if nothing drives it. */
typedef struct odin3_elab2_loop_note {
    odin3_net_id net;
    odin3_ast_id lhs, rhs;
} odin3_elab2_loop_note;

/* A `$tribuf` on an internal net, to become logic at the end of the module (odin3_elab2_tri_logic):
 * the cell and the place of its driver. */
typedef struct odin3_elab2_tri_rec {
    odin3_node_id node;
    odin3_ast_id at;
} odin3_elab2_tri_rec;

/* The port and the node's name of an inout pin of a child instance. */
typedef struct odin3_elab2_inout_pin {
    const char *port, *instance;
} odin3_elab2_inout_pin;

/* What a net with a tri-state driver is, once the module's nets are all merged (D3). */
typedef enum odin3_tri_net {
    ODIN3_TRI_NET_INTERNAL,        /* no port of the module */
    ODIN3_TRI_NET_PORT,            /* a $port_out or $port_inout of the top module */
    ODIN3_TRI_NET_BELOW_TOP_INOUT, /* a $port_inout of a module below the top */
    ODIN3_TRI_NET_CHILD_INOUT      /* internal, with an inout pin of a child instance */
} odin3_tri_net;

/* The class of net in the module being emitted; *pin is the child's port and instance names for
 * ODIN3_TRI_NET_CHILD_INOUT. */
odin3_tri_net odin3_elab2_tri_net(const odin3_elab2 *e2, odin3_net_id net,
                                  odin3_elab2_inout_pin *pin);

struct odin3_elab2_drive {
    odin3_vec drv;         /* odin3_drv per net ID, as far as a net was claimed */
    odin3_vec info;        /* odin3_elab2_net_info per net ID */
    odin3_u64map *claims;  /* (wire << 32 | bit) -> (kind << 32 | node) */
    odin3_u64map *uwires;  /* wire ID -> 1 */
    odin3_u64map *fwd;     /* a net merged away -> the net that replaced it */
    odin3_u64map *always;  /* symbol ID -> offset of its bits in abits */
    odin3_u64map *covered; /* net -> 1: reported by a loop note */
    odin3_u64map *words;   /* wire ID of a register-array word -> offset of its bits in wbits */
    odin3_vec wbits;       /* uint8_t: one flag per bit of a word wire assigned in an always */
    odin3_vec wired;       /* odin3_elab2_wired_rec */
    odin3_vec abits;       /* uint8_t: one flag per bit of an object assigned in an always */
    odin3_vec loops;       /* odin3_elab2_loop_note */
    odin3_vec tribufs;     /* odin3_elab2_tri_rec: the $tribuf cells of internal nets */
    uint32_t stmts;        /* statements that drove a wired net */
    bool is_top;
};

/* How a wired net kind (EL-26) resolves its drivers, and how it is spelled in a message. */
typedef struct odin3_elab2_wired_kind {
    uint8_t sub; /* the ODIN3_AST_NET_DECL_* sub of the net */
    bool is_or;
    const char *name;
} odin3_elab2_wired_kind;

/* The wired kind of a net declaration sub (`wand`, `wor`, `triand`, `trior`); NULL for any other.
 */
const odin3_elab2_wired_kind *odin3_elab2_wired_kind_of(uint8_t sub);

/* The tables of the module being emitted, made on first use; NULL on out of memory. */
struct odin3_elab2_drive *odin3_elab2_drive_tables(odin3_elab2 *e2);

/* The slot of net in a per-net vec, growing it (zeroed) to hold it; NULL on out of memory. */
void *odin3_elab2_net_slot(odin3_vec *vec, odin3_net_id net);

/* A net's entry in the info table, without growing it: zero past its end. */
odin3_elab2_net_info odin3_elab2_info_get(const struct odin3_elab2_drive *dr, odin3_net_id net);

/* Records a driver on a net not driven yet (an entry already there stays). */
odin3_status odin3_elab2_drv_set(struct odin3_elab2_drive *dr, odin3_net_id net, odin3_drv entry);

/* Records the claim on a (wire, bit) in the claims map. */
odin3_status odin3_elab2_claim_put(struct odin3_elab2_drive *dr, odin3_wirebit wb, odin3_drv entry);

/* The claim of an lvalue's bits (the request's kind, node and port), without wired redirection;
 * *granted tells whether every bit passed. */
odin3_status odin3_elab2_claim_lvalue(odin3_elab2 *e2, struct odin3_elab2_drive *dr,
                                      const odin3_elab2_lvclaim *req, bool *granted);

/* " (also at f.v:L:C)"-style place text of an AST node onto out. */
odin3_status odin3_elab2_where(const odin3_elab2 *e2, odin3_ast_id node, odin3_strbuf *out);

/* What each_symbol calls on a symbol. */
typedef odin3_status (*odin3_elab2_symbol_fn)(odin3_elab2 *e2, const odin3_symbol *rec);

/* Calls fn on every net and variable symbol of the module being emitted that has a wire, in the
 * module's scope and its generate and named-block scopes (a worklist, no recursion). */
odin3_status odin3_elab2_each_symbol(odin3_elab2 *e2, odin3_ast_id module_node,
                                     odin3_elab2_symbol_fn fn);

#endif
