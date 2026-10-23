/*
 * operand.h — operand vectors of arithmetic cells (spec C-5, C-6, M-1, M-2, N-3). Internal.
 */
#ifndef ODIN3_PASSES_UNMAP_OPERAND_H
#define ODIN3_PASSES_UNMAP_OPERAND_H

#include "ir/ids.h"
#include "ir/module.h"

#include <stdbool.h>
#include <stdint.h>

/* Bits of one operand as nets, LSB first; len entries (the storage is the caller's). */
typedef struct um_vec {
    odin3_net_id *nets;
    uint32_t len;
} um_vec;

/*
 * C-5/N-3: shortens `v` while its top bit has class UM_GND or UM_PAD (and UM_NONE when `open_too`),
 * keeping at least one bit. Returns how many bits were removed.
 */
uint32_t um_trim_tail(const odin3_module *module, um_vec *vec, bool open_too);

/*
 * C-6: the top run: the minimal w <= len with bits w-1..len-1 the same UM_SIG net; len when the
 * top bit is not a signal net or is alone.
 */
uint32_t um_top_run(const odin3_module *module, const um_vec *vec);

#endif
