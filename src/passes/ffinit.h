/*
 * ffinit.h — initial values on the `init` attribute (2E spec §10, PR-16; 2F spec §9.4): Yosys's
 * FfInitVals set_init and remove_init on Odin III's nets. An initial value lives where 2D puts it
 * (EL-40): the BITS attribute `init` (one bit per wire bit, x where unknown) on a wire, so a net's
 * initial value is the `init` bit of its wire memberships: its primary (wire, bit), then its
 * aliases that are wire bits, oldest first. Shared by proc (2E) and the opt family (2F, which
 * appends odin3_ffinit_get).
 */
#ifndef ODIN3_PASSES_FFINIT_H
#define ODIN3_PASSES_FFINIT_H

#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"

/*
 * Yosys set_init: writes `init` bit `value` on one wire bit of net, its primary wire bit, else its
 * first alias that is a wire bit, creating the attribute all x first when that wire has none, and
 * clears the bit (to x) on every other wire membership of the net. Any `init` left all x is
 * removed (odin3_attr_remove), and none is created for value x. A net with no wire membership is a
 * no-op (ODIN3_OK). The key is the strtab ID of `init`, interned once per call. An `init` that is
 * not BITS of its wire's width is rewritten as one (the bits it lacks x). ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL module, a net that is not live or a value that is not an odin3_bit;
 * ODIN3_ERR_NO_MEMORY (memberships written before the failure keep their new bits; the IR stays
 * valid).
 */
odin3_status odin3_ffinit_set(odin3_module *module, odin3_net_id net, odin3_bit value);

/*
 * Yosys remove_init: clears net's `init` bit (to x) on every wire membership and removes an
 * attribute left all x. *defined_on (may be NULL) gets the first membership's wire, in the order
 * above, whose cleared bit was 0 or 1, else none (2E's W2 is given once per wire). Nothing is
 * created: with no `init` key interned yet, nothing has one. Errors as odin3_ffinit_set.
 */
odin3_status odin3_ffinit_remove(odin3_module *module, odin3_net_id net, odin3_wire_id *defined_on);

#endif
