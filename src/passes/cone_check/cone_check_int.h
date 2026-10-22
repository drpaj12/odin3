/*
 * cone_check_int.h — what the files of the cone_check pass share (Phase 7 spec 3.3, plan Task 7).
 * Internal. bind.c: the group, its boundary nets, the hash and located errors. tmpl.c: the
 * template cell and the binding. miter.c: the temporary design. prove.c: lowering and the proof.
 * cone_check.c: one module's run. register.c: the pass and its options.
 */
#ifndef ODIN3_PASSES_CONE_CHECK_CONE_CHECK_INT_H
#define ODIN3_PASSES_CONE_CHECK_CONE_CHECK_INT_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/cone_check/cone_check.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The default proof time limit in seconds (spec 3.3 C-3) and the largest one accepted. */
enum { CC_DEFAULT_TIMEOUT = 60, CC_MAX_TIMEOUT = 1000000 };

/* The module attribute keys of the result (spec 3.3 C-4) and the binding key of the nets. */
#define CC_KEY_BIND "odin3:bind"
#define CC_KEY_CHECK "odin3:cone_check"
#define CC_KEY_GROUP "odin3:cone_key"
#define CC_KEY_HASH "odin3:cone_hash"
#define CC_KEY_CEX "odin3:cone_cex"
#define CC_KEY_CELL "odin3:cone_cell"

/* What a net is to the group: boundary input, boundary output, or neither. */
enum { CC_NET_INTERNAL = 0, CC_NET_IN = 1, CC_NET_OUT = 2 };

/* What is wrong with a net of the group, if anything. */
enum { CC_ISSUE_NONE = 0, CC_ISSUE_MULTI, CC_ISSUE_INOUT };

/* A net a group node touches: a boundary net (dir IN or OUT), or an internal net with an issue. */
typedef struct cc_bnet {
    uint32_t net;
    uint8_t dir;
    uint8_t issue;
    uint32_t bind; /* strtab ID of the odin3:bind text, 0 when the net has none */
    int32_t bit;   /* the template bit it is bound to, -1 until the binding is checked */
} cc_bnet;

/* The group of one module (nodes ascending) and its nets (ascending). */
typedef struct cc_group {
    odin3_design *design;
    const odin3_module *module;
    odin3_vec nodes;  /* uint32_t node IDs */
    odin3_vec inside; /* uint8_t per node ID: 1 for a group node */
    odin3_vec nets;   /* cc_bnet */
} cc_group;

/* One `-param NAME=VALUE`. */
typedef struct cc_param {
    const char *name;
    const char *value;
} cc_param;

/* One `-const PORT=V`: a template input port tied to the constant V (decimal or 0b binary). */
typedef struct cc_const {
    const char *port;
    const char *value;
} cc_const;

/* What a flat template bit is: bound to a group net, or tied to a constant (`-const`). */
enum { CC_BIT_NET = 0, CC_BIT_CONST0 = 1, CC_BIT_CONST1 = 2 };

/* The parsed options; the texts live in arena. */
typedef struct cc_opts {
    odin3_arena *arena;
    const char *key;
    const char *value;
    const char *cell;
    odin3_vec params; /* cc_param */
    odin3_vec consts; /* cc_const, in option order */
    uint32_t timeout;
    const char *who;   /* the pass name in messages: cone_check or raise */
    const char *usage; /* its usage line */
    bool timeout_ok;   /* whether -timeout is an option of the pass */
} cc_opts;

/* The template cell, in the temporary design, and where each of its port bits sits. */
typedef struct cc_tmpl {
    odin3_design *design; /* the temporary design (not owned) */
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    odin3_arena *arena;
    odin3_vec params; /* odin3_value, one per parameter of def */
    odin3_vec first;  /* uint32_t: first flat bit of each port, then the bit count */
    odin3_vec ord;    /* uint32_t per flat bit: its index among the bits of its direction */
    odin3_vec cbit;   /* uint8_t per flat bit: CC_BIT_* */
    uint32_t n_in, n_out;
} cc_tmpl;

/* One run on one module: the group, the template, the temporary design and its ports and nets. */
typedef struct cc_job {
    odin3_pass_ctx *ctx;
    odin3_module *src;
    const cc_opts *opts;
    cc_group group;
    cc_tmpl tmpl;
    odin3_design *tmp;
    odin3_module *mod;
    odin3_vec map;    /* uint32_t per source net ID: the net in mod, 0 for none yet */
    odin3_vec in_ids; /* odin3_node_id of the input ports (template input order) */
    odin3_vec t_ids;  /* odin3_node_id of the template's output ports */
    odin3_vec g_ids;  /* odin3_node_id of the group's output ports */
} cc_job;

typedef enum cc_verdict { CC_EQUIVALENT = 0, CC_DIFFERS, CC_UNPROVEN } cc_verdict;

/* The proof's result: the verdict and, for DIFFERS, the counterexample text (empty when none). */
typedef struct cc_proof {
    cc_verdict verdict;
    odin3_strbuf cex;
} cc_proof;

/* --- register.c ------------------------------------------------------------------------------ */

/*
 * Parses `-attr KEY=V -cell TYPE [-param P=V ...]` (and `-timeout S` when opts->timeout_ok) into
 * opts, whose arena, params, who, usage and timeout_ok are set by the caller. Usage errors are
 * logged and ODIN3_ERR_INVALID_ARG.
 */
odin3_status cc_parse_args(odin3_bytes args, cc_opts *opts);

/* --- cone_check.c ---------------------------------------------------------------------------- */

/*
 * Runs the pass on one module: *found says whether it has a group KEY=V at all (when not, nothing
 * else happens). Refusals and failures are logged; the verdict goes into the module's attributes.
 */
odin3_status cc_run_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                           bool *found);

/* --- bind.c ---------------------------------------------------------------------------------- */

/* Logs an error located at the source of prov when it has one; always ODIN3_ERR_INVALID_ARG. */
odin3_status cc_error(odin3_design *design, odin3_prov_id prov, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

void cc_group_init(cc_group *group, const odin3_module *module);
void cc_group_free(cc_group *group);

/* Fills the group of the module nodes whose attribute opts->key has the text opts->value; silent.
 */
odin3_status cc_group_collect(cc_group *group, const cc_opts *opts);

/* The refusals before any proof (spec 3.3 C-2, C-3): logged, ODIN3_ERR_INVALID_ARG. */
odin3_status cc_group_check(const cc_group *group, const char *key, const char *value);

/* A name for net in buf: its own name, else its wire bit, else `$n<ID>`. */
const char *cc_net_label(const cc_group *group, odin3_net_id net, char *buf, size_t size);

/* --- tmpl.c ---------------------------------------------------------------------------------- */

void cc_tmpl_init(cc_tmpl *tmpl);
void cc_tmpl_free(cc_tmpl *tmpl);

/* Looks the cell and its parameters up in design (the temporary one); errors are logged. */
odin3_status cc_tmpl_build(cc_tmpl *tmpl, odin3_design *design, const cc_opts *opts);

/*
 * Parses a `-const` value (decimal up to 2^64 - 1, or `0b` binary) into `width` bits, LSB first
 * (0 or 1 each), zero-extended; a value that does not fit is refused, never truncated. Logged,
 * ODIN3_ERR_INVALID_ARG on a parse or width error (spec K-2).
 */
odin3_status cc_const_bits(const char *text, uint32_t width, uint8_t *bits);

/* Checks every boundary net's binding against the template (spec 3.3 C-2) and sets bit. */
odin3_status cc_tmpl_bind(const cc_tmpl *tmpl, cc_group *group);

/* The direction of flat bit `bit`, and its name `PORT[i]` in buf. */
odin3_dir cc_tmpl_dir(const cc_tmpl *tmpl, uint32_t bit);
const char *cc_tmpl_name(const cc_tmpl *tmpl, uint32_t bit, char *buf, size_t size);

/*
 * The text of the template the proof was made for, appended to out: the cell type, then every
 * `-param NAME=VALUE` as ` NAME=VALUE` sorted by name (a stable order, so the order of the
 * options does not matter). `raise` compares it with the one the proof recorded as CC_KEY_CELL.
 */
odin3_status cc_cell_text(const cc_opts *opts, odin3_strbuf *out);

/* --- miter.c --------------------------------------------------------------------------------- */

/* Builds the temporary design: input ports, the group's copy, the template cell, output ports. */
odin3_status cc_miter_build(cc_job *job);

/* The net of the one pin of a scalar port node of the temporary module. */
odin3_net_id cc_port_net(const odin3_module *module, odin3_node_id port);

/* --- prove.c --------------------------------------------------------------------------------- */

/* Lowers the temporary design, builds the miter and asks ABC; timeout 0 is unproven at once. */
odin3_status cc_prove(cc_job *job, cc_proof *proof);
void cc_proof_init(cc_proof *proof);
void cc_proof_free(cc_proof *proof);

#endif
