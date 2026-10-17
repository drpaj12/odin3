/*
 * region.h — what ABC sees of one module and one clock domain (3D spec §5, AB-10, AB-11, AB-12;
 * plan Task 15): the roles of the cells, the constants per reader pin, the CIs, COs and registers,
 * the clock domains as flip-flop sets, and the Kahn order of the soft cells with loops cut.
 * Pure IR code: nothing here includes or calls ABC. The region is read-only to the IR.
 */
#ifndef ODIN3_PASSES_ABC_REGION_H
#define ODIN3_PASSES_ABC_REGION_H

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

struct odin3_abc_run;
typedef struct odin3_abc_run odin3_abc_run;

/* What a node is to ABC (§5.1). */
typedef enum odin3_abc_role {
    ODIN3_ABC_ROLE_CUT =
        0, /* never changed: its outputs are CIs, its inputs COs when soft drives them */
    ODIN3_ABC_ROLE_SOFT, /* logic ABC maps: $sop, $lut, parameterless gates */
    ODIN3_ABC_ROLE_REG,  /* a flip-flop of the run's domain */
    ODIN3_ABC_ROLE_CONST /* a constant generator (const_value 0, 1, x or z) */
} odin3_abc_role;

/* A clock domain (§5.3). */
typedef enum odin3_abc_domain_kind {
    ODIN3_ABC_DOM_NONE = 0, /* a module without flip-flops: one run, no registers */
    ODIN3_ABC_DOM_EDGE,     /* $_DFF_P_/$_DFF_N_ on one clock net */
    ODIN3_ABC_DOM_GLOBAL    /* $_FF_ */
} odin3_abc_domain_kind;

/* ffs holds node IDs in ascending order (a set of flip-flops, I13). clock is none unless EDGE. */
typedef struct odin3_abc_domain {
    uint8_t kind; /* an odin3_abc_domain_kind */
    bool negedge;
    odin3_net_id clock;
    odin3_vec ffs; /* odin3_node_id */
} odin3_abc_domain;

/* The counts of cut_counts, in the order of the INFO line (§11). */
enum {
    ODIN3_ABC_CUT_HARD = 0, /* hard or black-box cells */
    ODIN3_ABC_CUT_INSTANCE, /* instances of modules */
    ODIN3_ABC_CUT_LATCH,    /* level latches */
    ODIN3_ABC_CUT_TRISTATE, /* tri-state or bus cells */
    ODIN3_ABC_CUT_OTHER,    /* keep cells, unknown or parameterised bit types, wide gates, ... */
    ODIN3_ABC_CUT_KINDS
};

/*
 * One region (module + domain). Every vec holds uint32_t, except domains.ffs.
 * - soft: node IDs in Kahn order, ties by node ID; regs: node IDs ascending.
 * - cis, cos: net IDs ascending. reg_d: each register's D net (0: the pin is unconnected, which
 *   ABC sees as the constant 0).
 * - role: one odin3_abc_role byte per node ID (index 0 unused); ci_of, co_of, q_of: per net ID,
 *   1 + the index into cis, cos, regs (0: the net is none of them) -- q_of is for a register's Q
 *   net. A loop-cut net is in both cis and cos: readers see its CI, its driver writes its CO.
 * - zero_pins: soft and register-D pins that read x, z or an undefined net as 0 (the INFO count).
 * - objs_est: point (r)'s upper bound of the AIG's objects.
 */
typedef struct odin3_abc_region {
    odin3_vec soft, regs, cis, cos, reg_d;
    odin3_vec role, ci_of, co_of, q_of;
    uint32_t zero_pins;
    uint64_t objs_est;
    uint32_t cut_counts[ODIN3_ABC_CUT_KINDS];
} odin3_abc_region;

/* Frees the vecs of a region (also a partly built or zeroed one) and zeroes it. */
void odin3_abc_region_free(odin3_abc_region *region);

/* Frees a domain's ffs. */
void odin3_abc_domain_free(odin3_abc_domain *domain);

/* Frees a vec of domains made by odin3_abc_domains (every ffs, then the vec). */
void odin3_abc_domains_free(odin3_vec *domains);

/*
 * The clock domains of module (once per module, before its first run): *domains (elements
 * odin3_abc_domain; cleared first) gets them sorted per §5.3 with point (k)'s name bytes: by the
 * clock net's name (own name, else its primary wire bit `w[k]` or `w`, else empty), then P before
 * N, then net ID; the global domain last. A flip-flop with `keep`, or whose Q net has another
 * driver, belongs to none (it is a cut cell). A module without flip-flops gets one domain of kind
 * NONE. More than the cap (odin3_abc_run_limits_get()->domains) is `abc: module '<m>' has <N> clock
 * domains; at most <cap>` (PARSE, logged, domains left empty). NO_MEMORY (not logged).
 */
odin3_status odin3_abc_domains(odin3_abc_run *run, const odin3_module *module, odin3_vec *domains);

/*
 * The text of a domain as the INFO lines show it: `clock '<net>' posedge|negedge`, `global clock`
 * or `no flip-flops`, into out (NUL-terminated, cut to cap). <net> is the clock net's own name,
 * else its primary wire bit, else `$n<ID>`.
 */
void odin3_abc_domain_text(const odin3_module *module, const odin3_abc_domain *domain, char *out,
                           size_t cap);

/*
 * The truth table of a parameterless gate type (§5.1), computed once per type by running the 1E
 * simulate hook over all 2^n inputs (input j = the j-th input pin in port order) and cached in the
 * run: bit i of *table is the output for the input assignment i; *n_inputs is n. ODIN3_OK for an
 * eligible type (bit granularity, no parameters, a simulate hook, none of the SEQ/TRISTATE/ANYVIEW
 * flags, at most ODIN3_ABC_GATE_MAX_INPUTS input bits and exactly one 1-bit output);
 * ODIN3_ERR_INVALID_ARG (not logged) for any other type; NO_MEMORY.
 */
odin3_status odin3_abc_gate_table(odin3_abc_run *run, odin3_celltype_id type, uint64_t *table,
                                  uint32_t *n_inputs);

/*
 * Builds the region of run->module for domain: one O(nodes + pins) sweep and Kahn's algorithm. On
 * a stall it cuts the lowest-ID remaining cell's output net and warns `abc: module '<m>':
 * combinational loop through <type> '<cell>'; its output <net> is a cut point`. *out is filled
 * only on OK (zeroed otherwise). NO_MEMORY (not logged). Never changes the IR.
 */
odin3_status odin3_abc_region_build(odin3_abc_run *run, const odin3_abc_domain *domain,
                                    odin3_abc_region *out);

/*
 * The literal a constant net gives a reader pin (C2): 1 for value 1; 0 for 0, x, z and an
 * undefined net; -1 for a net that is not a constant (including no net).
 */
int odin3_abc_pin_literal(const odin3_abc_run *run, odin3_net_id net);

/* True for a net named `unconn` driven by a zero-input, zero-row $sop (Parmys's undefined net). */
bool odin3_abc_net_undefined(const odin3_module *module, odin3_net_id net);

/* True when the net, or a wire that holds it, has a non-zero `keep` attribute (Yosys's rule). */
bool odin3_abc_net_keep(const odin3_module *module, odin3_net_id net);

/* The text of a net in messages: its own name, else its primary wire bit, else `$n<ID>`. */
void odin3_abc_net_text(const odin3_module *module, odin3_net_id net, char *out, size_t cap);

/* The text of a node in messages: its own name, else `$c<ID>`. */
void odin3_abc_node_text(const odin3_module *module, odin3_node_id node, char *out, size_t cap);

/*
 * A binary min-heap of uint32_t in a vec (Kahn's ready set, ties by smallest value), shared by the
 * region sweep and the BLIF read-back. pop needs a non-empty heap. push: NO_MEMORY.
 */
odin3_status odin3_abc_heap_push(odin3_vec *heap, uint32_t value);
uint32_t odin3_abc_heap_pop(odin3_vec *heap);

#endif
