/*
 * signals.h — the signal table of a built simulator (S1): instance scopes, the signals with HDL
 * names, name lookup, and value reads and writes. The ABI (src/api/sim_abi.c) and the VCD writer
 * sit on it.
 */
#ifndef ODIN3_SIM_SIGNALS_H
#define ODIN3_SIM_SIGNALS_H

#include "odin3/odin3.h"
#include "sim/sim.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdint.h>

/* Same values as the ABI's odin3_simulation_kind. */
typedef enum odin3_sim_kind {
    ODIN3_SIM_KIND_INPUT = 0,
    ODIN3_SIM_KIND_CLOCK = 1,
    ODIN3_SIM_KIND_OUTPUT = 2,
    ODIN3_SIM_KIND_INTERNAL = 3
} odin3_sim_kind;

/* One expanded module instance; scope i is sim->frames[i]. Strings are table string IDs. */
typedef struct odin3_sim_scope {
    uint32_t name;   /* the instance path ("" for scope 0) */
    uint32_t leaf;   /* instance name, "#<node ID>" when unnamed, the top module's name for 0 */
    uint32_t parent; /* 0 for scope 0 */
} odin3_sim_scope;

/*
 * A named signal: a wire (or a bit of one) or a net, in one scope. A bit that no cell or port
 * connects (an unconnected wire bit) reads constant 0 (agent default S1-AD1; the spec is silent).
 */
typedef struct odin3_sim_signal {
    uint32_t name, leaf; /* table string IDs: full name, leaf */
    uint32_t scope;
    uint32_t kind; /* odin3_sim_kind */
    uint32_t width;
    int32_t msb, lsb;
    uint32_t obj_kind;   /* ODIN3_OBJ_WIRE or ODIN3_OBJ_NET */
    uint32_t obj_id;     /* wire or net ID in the scope's module */
    uint32_t first_bit;  /* vector position in the object of the signal's bit 0 */
    uint32_t slot_first; /* index of bit 0's slot in the table's slot array */
    uint32_t in_port;    /* top input port for INPUT/CLOCK, else UINT32_MAX */
    uint32_t in_bit;     /* that port's bit for the signal's bit 0 */
} odin3_sim_signal;

typedef struct odin3_sim_signals odin3_sim_signals; /* opaque, defined in signals.c */

/*
 * Builds the scopes and the listed signals of sim (borrowed; it must outlive the table, and the
 * design must not change). ODIN3_ERR_NO_MEMORY with nothing left allocated. Linear in the wires
 * and nets of the expanded modules.
 */
odin3_status odin3_sim_signals_create(odin3_sim *sim, odin3_sim_signals **out);

/*
 * Appends to out the name of net `net` of module `module` as a waveform should carry it (the BLIF
 * name `write_blif --vtr` writes); true when it did, false to use the default name. ctx is the
 * caller's. Called once per net, scope by scope.
 */
typedef bool (*odin3_sim_net_namer)(void *ctx, odin3_module_id module, odin3_net_id net,
                                    odin3_strbuf *out);

/*
 * As odin3_sim_signals_create, but the listed signals are the live, connected NETS of every scope
 * (one 1-bit signal each, in net ID order), not the wires: what `sim --vcd-all` and
 * `sim_tb --vcd-all` dump. A net is named by namer (NULL: no namer), else by its own public name,
 * else `$n<ID>`. Ports are nets too (a port bit has the net's name); no kind is INPUT, so the
 * table cannot be written. odin3_sim_signals_find still resolves HDL names and appends.
 */
odin3_status odin3_sim_signals_create_nets(odin3_sim *sim, odin3_sim_net_namer namer, void *ctx,
                                           odin3_sim_signals **out);

/* Frees the table and every string it handed out; NULL is a no-op. */
void odin3_sim_signals_destroy(odin3_sim_signals *sig);

/* The number of listed signals, and of all signals (listed plus those appended by find). */
uint32_t odin3_sim_signals_listed(const odin3_sim_signals *sig);
uint32_t odin3_sim_signals_total(const odin3_sim_signals *sig);

/* Signal `id` (1..total), NULL otherwise. The pointer is valid until the next find. */
const odin3_sim_signal *odin3_sim_signals_at(const odin3_sim_signals *sig, uint32_t id);

/* The number of scopes, and scope `scope` (0..count-1; NULL otherwise). */
uint32_t odin3_sim_signals_scope_count(const odin3_sim_signals *sig);
const odin3_sim_scope *odin3_sim_signals_scope(const odin3_sim_signals *sig, uint32_t scope);

/* The string of a table string ID: NUL-terminated, never moves, valid until destroy. */
const char *odin3_sim_signals_text(const odin3_sim_signals *sig, uint32_t str);

/*
 * Resolves `name` (spec section SA-5): *id gets the signal's ID, 0 when the netlist has no such
 * name (ODIN3_OK both ways). A name found by bit select, wire, net or hierarchical path that is
 * not listed is appended as a new signal (ID total + 1). ODIN3_ERR_NO_MEMORY when appending
 * fails: the signals, their slots and the name lookup are as before; only the strings interned for
 * the failed signal stay in the table (until destroy).
 */
odin3_status odin3_sim_signals_find(odin3_sim_signals *sig, odin3_bytes name, uint32_t *id);

/*
 * Copies the `count` bits (0 or 1) of signal `id`, as of the last settle. ODIN3_ERR_INVALID_ARG
 * (not logged) for an unknown ID or a count other than the width.
 */
odin3_status odin3_sim_signals_read(const odin3_sim_signals *sig, uint32_t id, uint8_t *bits,
                                    uint32_t count);

/*
 * Sets an INPUT signal from `count` bytes (0 or 1; spec SA-7); clock bits inside it must be 0 and
 * are left alone. ODIN3_ERR_INVALID_ARG (not logged, nothing changed) with *why one of "unknown
 * signal", "not an input", "count is not the width", "a bit is not 0 or 1", "a clock bit set to
 * 1".
 */
odin3_status odin3_sim_signals_write(odin3_sim_signals *sig, uint32_t id, const uint8_t *bits,
                                     uint32_t count, const char **why);

#endif
