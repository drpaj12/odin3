/*
 * submod_int.h — what register.c shares with submod.c (Phase 7 spec 3.1, plan Task 5). Internal.
 */
#ifndef ODIN3_PASSES_SUBMOD_SUBMOD_INT_H
#define ODIN3_PASSES_SUBMOD_SUBMOD_INT_H

#include "passes/submod/submod.h"
#include "util/attr.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The state of a net of the module relative to one group (per net ID, uint8_t). */
enum { SM_NONE = 0, SM_INTERNAL, SM_IN, SM_OUT };

/* One group to extract: the attribute key, its value text and the new module's name. */
typedef struct odin3_submod_group {
    const char *key;
    const char *value;
    const char *name;
    const char *bus_key; /* NULL: no -bus */
} odin3_submod_group;

/*
 * odin3_submod_extract_ctx with the new module named group->name instead of the value. Same
 * failures; *out (may be NULL, zeroed or filled) gets the result.
 */
odin3_status odin3_submod_extract_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                        const odin3_submod_group *group, odin3_submod_result *out);

/*
 * The refusals of odin3_submod_extract_named for this group (same failures and log lines), without
 * changing the IR: the pass runs it for every group before it moves any.
 */
odin3_status odin3_submod_check_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                      const odin3_submod_group *group);

/* --- busattr.c: vector ports from NAME[BIT] net attributes (v1.0.5 spec 4) ------------------- */

/*
 * True when text is `NAME[BIT]` (spec 4.1): NAME non-empty, not starting with `$`, with no `[` or
 * `]`; BIT a decimal integer 0 to 2^31 - 1. *name_len is the length of NAME, *bit the bit.
 */
bool odin3_submod_bus_parse(const char *text, size_t *name_len, uint32_t *bit);

/* One vector port to make: the nets with attribute NAME[0 .. width-1] of one direction. */
typedef struct odin3_submod_bus {
    uint32_t name;  /* strtab ID of NAME */
    uint8_t dir;    /* SM_IN or SM_OUT */
    uint32_t first; /* index of bit 0 in the set's nets */
    uint32_t width;
    bool ok; /* false: a gap, a duplicate or both directions (spec B-3): the nets stay scalar */
} odin3_submod_bus;

typedef struct odin3_submod_busset {
    odin3_vec buses;  /* odin3_submod_bus, in order of (name, direction) */
    odin3_vec nets;   /* odin3_net_id: each bus's nets, in bit order (as far as it has them) */
    odin3_vec of_net; /* uint32_t per net ID: bus index + 1 for a net of an ok bus, else 0 */
    bool quiet;       /* set by the caller before build: no warnings (the check phase's run) */
} odin3_submod_busset;

void odin3_submod_busset_init(odin3_submod_busset *set);
void odin3_submod_busset_free(odin3_submod_busset *set);

/*
 * Reads attribute `key` on every net of module whose `state` entry (uint8_t per net ID) is SM_IN
 * or SM_OUT and builds the table for group `group`. A text that is not NAME[BIT] (or not a
 * string) is a located error, ODIN3_ERR_INVALID_ARG (spec B-4); an incomplete set is not an
 * error: its bus has ok = false and one warning names the group, the bus and why (B-3).
 */
odin3_status odin3_submod_busset_build(const odin3_module *module, const odin3_vec *state,
                                       const char *key, const char *group,
                                       odin3_submod_busset *out);

/* Logs an error located at the source of prov when it has one; always ODIN3_ERR_INVALID_ARG. */
odin3_status odin3_submod_error(odin3_design *design, odin3_prov_id prov, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

#endif
