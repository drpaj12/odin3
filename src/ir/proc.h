/*
 * proc.h — processes in the IR (2D IR change B: 2D spec EL-33, §8.1; docs/IR.md IR-19).
 *
 * A process (an `always` block) is a node of the built-in cell type $proc: its connectivity is
 * ordinary pins (port I: every net the process reads and does not drive, constants included as
 * pins on the module's shared $_CONST*_ nets; port O: every net it drives) and its decision
 * structure is the opaque parameter BODY, a value of kind ODIN3_VAL_PROC. Body positions are pin
 * positions, never net IDs, so merging, renaming and compacting nets never touch a body.
 *
 * This header has the body's types (§8.1), the blob they are packed into (plan point (e)), the
 * allocation-free check of a body, the per-module process counter, and the RTLIL spelling of
 * signals (plan point (s)) and of process blocks (§13, plan point (r)).
 */
#ifndef ODIN3_IR_PROC_H
#define ODIN3_IR_PROC_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A position in a process body: an input pin, an output pin, or "anything" (compares only).
 * IN | k is the net on I pin k; OUT | k the net on O pin k (read: its current value, IR-19); ANY
 * ('-') matches any value and appears only in compare patterns (its index bits are 0). §8.1 writes
 * the four as enum constants; C17 enumerators must fit an int and 2U << 30 does not, so they are
 * macros with §8.1's values.
 */
typedef uint32_t odin3_pbit;
#define ODIN3_PBIT_IN (0U << 30)
#define ODIN3_PBIT_OUT (1U << 30)
#define ODIN3_PBIT_ANY (2U << 30)
#define ODIN3_PBIT_INDEX_MASK ((1U << 30) - 1U)

/* A run of len positions starting at pbits[first]. */
typedef struct odin3_pspan {
    uint32_t first, len;
} odin3_pspan;

typedef enum odin3_sync_type {
    ODIN3_SYNC_POSEDGE = 1, /* edge of one net */
    ODIN3_SYNC_NEGEDGE,
    ODIN3_SYNC_LEVEL0, /* while the net is 0 / 1 (made by 2E; Yosys ST0/ST1) */
    ODIN3_SYNC_LEVEL1,
    ODIN3_SYNC_ALWAYS /* combinational (Yosys STa) */
} odin3_sync_type;

/* lhs = rhs, position by position (lhs only OUT positions, none twice). */
typedef struct odin3_proc_action {
    odin3_pspan lhs, rhs;
    odin3_prov_id prov;
} odin3_proc_action;

typedef struct odin3_proc_case {
    uint32_t first_compare, n_compares; /* pspans in compares[], each the switch's width */
    uint32_t first_action, n_actions;   /* in actions[] */
    uint32_t first_switch, n_switches;  /* in switches[] */
    odin3_prov_id prov;
} odin3_proc_case;

/* Switch flags. */
enum { ODIN3_SWITCH_FULL_CASE = 1, ODIN3_SWITCH_PARALLEL_CASE = 2 };

typedef struct odin3_proc_switch {
    odin3_pspan signal;
    uint32_t first_case, n_cases; /* in cases[] */
    uint16_t flags;               /* ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE */
    odin3_prov_id prov;
} odin3_proc_switch;

typedef struct odin3_proc_sync {
    uint8_t type;                     /* odin3_sync_type */
    odin3_pbit signal;                /* IN | k or OUT | k for edge and level types; 0 for ALWAYS */
    uint32_t first_action, n_actions; /* in actions[] */
} odin3_proc_sync;

/* The BODY parameter; cases[0] is the root. Arrays may be NULL when their count is 0. */
typedef struct odin3_proc_body {
    const odin3_pbit *pbits;
    uint32_t n_pbits;
    const odin3_pspan *compares;
    uint32_t n_compares;
    const odin3_proc_action *actions;
    uint32_t n_actions;
    const odin3_proc_case *cases;
    uint32_t n_cases;
    const odin3_proc_switch *switches;
    uint32_t n_switches;
    const odin3_proc_sync *syncs;
    uint32_t n_syncs;
} odin3_proc_body;

/* $proc's parameters and ports, in definition order. */
enum { ODIN3_PROC_P_I_WIDTH, ODIN3_PROC_P_O_WIDTH, ODIN3_PROC_P_BODY };
enum { ODIN3_PROC_PORT_I, ODIN3_PROC_PORT_O };

/*
 * Packs body into *out, a value of kind ODIN3_VAL_PROC whose payload (bits, len = bytes) is one
 * blob in arena: a header of six uint32_t counts (pbits, compares, actions, cases, switches,
 * syncs), then the six arrays in that order as the C structs above, each at an 8-byte-aligned
 * offset, len = the end of the last array rounded up to 8 (plan point (e)). Every record is written
 * field by field into zeroed, max_align_t-aligned arena memory, so padding bytes are 0 and two
 * equal bodies are equal bytes (odin3_value_equal, copies, tombstones need nothing more). The body
 * must already be in canonical layout (IR-19): pack neither reorders nor checks it (the node's
 * verify does). ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a NULL array with a nonzero
 * count, or a blob longer than UINT32_MAX bytes; ODIN3_ERR_NO_MEMORY on out of memory. *out is
 * unchanged on failure.
 */
odin3_status odin3_proc_body_pack(odin3_arena *arena, const odin3_proc_body *body,
                                  odin3_value *out);

/*
 * Views a packed body in place: *out points into val's payload, valid as long as the value. False
 * (out unchanged) for a NULL value, another kind, a payload that is not 8-byte aligned (no
 * misaligned record reads), a payload shorter than the header, and a header whose arrays do not
 * end exactly at len (a count whose array overruns len, or trailing bytes). It checks the blob's
 * shape only; odin3_proc_body_verify checks the body.
 */
bool odin3_proc_body_view(const odin3_value *val, odin3_proc_body *out);

/* Why odin3_proc_body_verify rejected a body (never logged by it). */
typedef struct odin3_proc_why {
    char text[ODIN3_WIDTH_WHY_MAX];
} odin3_proc_why;

/* The widths of a $proc node's ports (its I_WIDTH and O_WIDTH). */
typedef struct odin3_proc_widths {
    uint32_t i_width;
    uint32_t o_width;
} odin3_proc_widths;

/*
 * Checks body against the port widths (IR-19): every §8.1 rule (indices in range, a tree using
 * every case, switch and action exactly once, no compares on the root case, compare and signal
 * widths, lhs and rhs widths, no empty action, lhs only OUT positions and none twice in one lhs,
 * ANY only in compares, sync signals, every OUT position assigned somewhere), the canonical layout
 * and point (h) (an edge or level sync may be on an OUT position). ODIN3_OK, or
 * ODIN3_ERR_INVALID_ARG with the first violation in *why (may be NULL), e.g. "action 0: lhs and
 * rhs widths differ (1, 2)".
 *
 * Never allocates (2D plan review I3), so out of memory can never be mistaken for a bad body. It
 * runs in time linear in the body plus O_WIDTH, times ceil(O_WIDTH / 8192) for the "assigned" check
 * and ceil(span / 8192) per lhs for the "twice" check (a fixed bitmap on the stack; one pass for
 * any process of up to 8192 outputs).
 */
odin3_status odin3_proc_body_verify(const odin3_proc_body *body, odin3_proc_widths widths,
                                    odin3_proc_why *why);

/* Live nodes of the module whose type has ODIN3_CT_PROCESS ($proc), kept by the IR. O(1). */
uint32_t odin3_module_proc_count(const odin3_module *module);

/* True when some module of the design has a process. O(modules). */
bool odin3_design_has_processes(const odin3_design *design);

/* The first module in creation order with a process; none (ID 0) when there is none. */
odin3_module_id odin3_design_first_proc_module(const odin3_design *design);

/*
 * The refusal of a design that still has processes (2D EL-33): ODIN3_OK when no module has one;
 * otherwise logs "pass '<pass>': module '<m>' has processes; run proc first" (the pass manager,
 * for a REFUSE pass) or, with pass NULL, "module '<m>' has processes; run proc first" (a writer
 * called directly), naming odin3_design_first_proc_module, and returns ODIN3_ERR_INVALID_ARG.
 * O(modules); NULL design is ODIN3_OK (the caller reports it).
 */
odin3_status odin3_design_refuse_processes(const odin3_design *design, const char *pass);

/*
 * Appends the RTLIL sigspec of n nets given LSB first (plan point (s)), as the body printer and
 * write_rtlil spell every signal. Each net is classified as:
 * - a constant: a none entry (ANY, '-') or a net whose odin3_net_const_value is 0, 1, x or z;
 * - a wire bit: a net whose primary (wire, bit) is a bit of a named wire;
 * - another net: \name for a named net, else $n<ID>.
 * Maximal runs of constants merge into <n>'<bits> (MSB first), and runs of consecutive bits of one
 * wire into \w [hi:lo] (vector positions, RTLIL's 0-based bit index), a whole wire being \w and a
 * single bit \w [k]; another net is one chunk. Several chunks are written MSB first as
 * { c1 c2 ... } and no nets as { }. ODIN3_ERR_NO_MEMORY with out unchanged; nothing else fails
 * (nets must be none or nets of module).
 */
odin3_status odin3_proc_sigspec(const odin3_module *module, const odin3_net_id *nets, uint32_t n,
                                odin3_strbuf *out);

/*
 * True when odin3_proc_sigspec spells net as a net of its own (\name for a named net, else
 * $n<ID>): a live net that is neither a constant nor a bit of a named wire. write_rtlil declares
 * exactly these nets as wires (2D plan review R4), so every spelling it prints is declared.
 */
bool odin3_proc_sig_is_net(const odin3_module *module, odin3_net_id net);

/*
 * Appends the RTLIL spelling of a value, as an attribute or parameter value: an INT in the 32-bit
 * range in decimal, a wider one as 64 bits; BITS as <n>'<bits> MSB first (0, 1, x, z); a STRING or
 * a COVER as a quoted string with Yosys's escapes. ODIN3_ERR_INVALID_ARG (not logged) for a PROC
 * value or another kind, ODIN3_ERR_NO_MEMORY; out is unchanged on failure.
 */
odin3_status odin3_proc_value_text(const odin3_design *design, const odin3_value *val,
                                   odin3_strbuf *out);

/*
 * Appends live $proc node `node` as an RTLIL process block (2D spec §13), each line indented by
 * `indent` spaces plus its nesting: the node's attributes (attribute \key value), then
 * "process $proc$<file>:<line>$<ID>" with file and line from the node's SOURCE record (its first
 * location, the file as given), else "process $proc$<ID>" (plan point (r); also when the file
 * name holds a blank, tab, CR or LF, which would end the RTLIL identifier); the root case's actions
 * as assign lines and its switches (a switch's flags as attribute \full_case 1 and
 * attribute \parallel_case 1, then switch, case <compares separated by " , ">, the case's body,
 * end); the syncs (sync posedge|negedge|low|high <signal>, or sync always) with their update lines;
 * then end. Each position is the net on its pin, spelled by odin3_proc_sigspec. No recursion.
 * ODIN3_ERR_INVALID_ARG (logged) when node is not a live node of a type with ODIN3_CT_PROCESS,
 * its body fails odin3_proc_body_verify, or a position is on an unconnected pin ("proc_print: node
 * N: position IN|k is on an unconnected pin"; spelled as none it would read as the wildcard '-');
 * ODIN3_ERR_NO_MEMORY; out is unchanged on failure.
 */
odin3_status odin3_proc_print(const odin3_module *module, odin3_node_id node, uint32_t indent,
                              odin3_strbuf *out);

#endif
