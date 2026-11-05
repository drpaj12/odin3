/*
 * soft_blocks.h — the soft bodies of VTR primitives for an architecture that lacks them (4.2 D18,
 * Peter 2026-10-20). `read_verilog --vtr-compat` on a design with an arch record builds an explicit
 * `single_port_ram`, `dual_port_ram`, `multiply` or `adder` instance as soft logic when the record
 * has no such model, as Parmys does, with a located WARNING at the instance. The bodies are
 * lib/soft_primitives/<name>.v, read as library units before the design files. Without
 * `--vtr-compat` the unresolved module stays the located error.
 *
 * #262: the body is read too when the record has the model but an instance's `#()` does not fit the
 * hard cell (an `adder #(W)` on the 1-bit `adder`): the soft body is the primitive's parameterized
 * form, as Parmys reads vtr_primitives.v for it. odin3_elab1_prefer_unit then keeps the cell for
 * the instances whose `#()` does fit, warning at each one that does not.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_SOFT_BLOCKS_H
#define ODIN3_FRONTENDS_VERILOG_SOFT_BLOCKS_H

#include "ir/design.h"

#include <stdbool.h>
#include <stdint.h>

enum { SOFT_BLOCK_COUNT = 4 };

/* The primitive of bit `index` of a mask (0 .. SOFT_BLOCK_COUNT - 1). */
const char *odin3_soft_block_name(uint32_t index);

/* The bits of the primitives the design's arch record has no model of at all; 0 without a
 * record (a model that took no template is a black box, not missing). */
uint32_t odin3_soft_blocks_missing(const odin3_design *design);

enum { SOFT_DIR_CAP = 4096 };

/*
 * Vendor primitives (OpenCores intake, 2026-11-04): `read_verilog --vendor-prims` reads an instance
 * of an undefined module named like a common FPGA vendor primitive (IBUF, IBUFG, OBUF, OBUFT,
 * IOBUF, BUFG, ODDR2, IDDR2) from Odin III's own behavioural model, lib/soft_primitives/<name>.v,
 * as a library unit, with one WARNING per primitive: the model stands in for the vendor's, whose
 * clocking and I/O timing the soft logic does not have. A module the design or its libraries define
 * is never replaced. The bit is set in the soft mask the reader is handed (the arch's missing
 * models use the low SOFT_BLOCK_COUNT bits).
 */
#define SOFT_VENDOR_BIT (1U << 31)

/* True when name is one of the vendor primitives the library has a model of. */
bool odin3_vendor_prim_known(const char *name);

/*
 * The directory of the bodies (lib/soft_primitives, installed as share/odin3/soft_primitives) into
 * dir (SOFT_DIR_CAP bytes): $ODIN3_SOFT_PRIMITIVES_DIR when set (the only candidate then; every
 * `<name>.v` in it that a design needs is parsed as Verilog, so point it only at a directory you
 * trust), else <exe>/../share/odin3/soft_primitives (install tree), <exe>/../../lib/soft_primitives
 * (build tree), the source tree the build came from. tried (SOFT_DIR_CAP bytes) gets the candidates
 * looked at. False when none is a directory.
 */
bool odin3_soft_blocks_dir(char *dir, char *tried);

#endif
