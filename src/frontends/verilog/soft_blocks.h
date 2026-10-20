/*
 * soft_blocks.h — the soft bodies of VTR primitives for an architecture that lacks them (4.2 D18,
 * Peter 2026-10-20). `read_verilog --vtr-compat` on a design with an arch record builds an explicit
 * `single_port_ram`, `dual_port_ram`, `multiply` or `adder` instance as soft logic when the record
 * has no such model, as Parmys does, with a located WARNING at the instance. The bodies are
 * lib/soft_primitives/<name>.v, read as library units before the design files. Without
 * `--vtr-compat` the unresolved module stays the located error.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_SOFT_BLOCKS_H
#define ODIN3_FRONTENDS_VERILOG_SOFT_BLOCKS_H

#include "ir/design.h"

#include <stdint.h>

enum { SOFT_BLOCK_COUNT = 4 };

/* The primitive of bit `index` of a mask (0 .. SOFT_BLOCK_COUNT - 1). */
const char *odin3_soft_block_name(uint32_t index);

/* The bits of the primitives the design's arch record has no model of at all; 0 without a
 * record (a model that took no template is a black box, not missing). */
uint32_t odin3_soft_blocks_missing(const odin3_design *design);

/* The directory of the bodies: $ODIN3_SOFT_PRIMITIVES_DIR, else the build's lib/soft_primitives;
 * NULL when neither is known. */
const char *odin3_soft_blocks_dir(void);

#endif
