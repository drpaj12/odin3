/*
 * lower.h — the `lower` pass (3B; spec docs/specs/2026-10-14-3B-lower-design.md): its options, caps
 * and entry point. Internal: not part of the ABI. The builders' own types are in lower_int.h.
 */
#ifndef ODIN3_PASSES_LOWER_LOWER_H
#define ODIN3_PASSES_LOWER_LOWER_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The caps of spec §11 (agent defaults, Peter may override): the bit nodes one word cell may
 * create, the live nodes a module may hold after lowering, and the bits of one `$mem`. Tests lower
 * them through lower_test.h.
 */
#define ODIN3_LOWER_MAX_CELL_NODES ((uint64_t)1 << 24)
#define ODIN3_LOWER_MAX_MODULE_NODES ((uint64_t)1 << 26)
#define ODIN3_LOWER_MAX_MEM_BITS ((uint64_t)1 << 22)

/* The byte budget is checked every this many created nodes (spec §11). */
#define ODIN3_LOWER_BUDGET_NODES ((uint64_t)1 << 16)

/* Options of `lower` (spec §13). */
typedef struct odin3_lower_opts {
    bool keep_neg_edge;      /* --neg-edge keep */
    bool async_is_error;     /* --async error */
    uint64_t max_mem_bits;   /* 0 = ODIN3_LOWER_MAX_MEM_BITS */
    const char *report_path; /* NULL = none */
    bool async_enable_first; /* hidden, test-only: the enable over a reset/load (Parmys's shape) */
} odin3_lower_opts;

/*
 * Parses the pass arguments: `[--neg-edge invert|keep] [--async sync|error] [--report <file>]
 * [--max-mem-bits N]`. *opts is filled (zero when no argument is given); report_path points into
 * the argument text's copy in *report (caller-owned, freed by the caller). INVALID_ARG (logged
 * "lower: unknown argument '<arg>'" or the option's own message) for anything else; NO_MEMORY.
 */
odin3_status odin3_lower_parse_args(odin3_bytes args, odin3_strbuf *report, odin3_lower_opts *opts);

/*
 * Lowers every word cell of every module of design to bit cells (spec §3 to §8): modules in module
 * ID order, nodes in ID order, each cell as one transaction (all new nodes on fresh nets, then the
 * word node is deleted and each output net merged with the net the builder returned; a failure
 * while building removes what was built and leaves the cell intact, §12.2). A cell whose type has
 * no rule is `lower: no lowering rule for cell type '<t>' ('<cell>')`; one whose size estimate
 * exceeds a cap is a located error with nothing built. One provenance operation per cell. Hard
 * cells, black boxes, instances, $sop, $lut and every bit cell are left alone. opts may be NULL
 * (the defaults). ctx is the pass run (it supplies the provenance operations and the budget's
 * name). ODIN3_OK; ODIN3_ERR_INVALID_ARG for the errors above; ODIN3_ERR_NO_MEMORY; ODIN3_ERR_PARSE
 * when the design-wide byte budget is exceeded; ODIN3_ERR_CHECK when a live word cell is left
 * (a bug). On a failure the design is in the legal mixed state of IR-9 (cells lowered before the
 * failing one are complete); only a failure while merging the outputs of one cell can leave that
 * cell partly connected (its undriven outputs are legal, check rule 4 W), and the pass says so.
 */
odin3_status odin3_lower_run(odin3_pass_ctx *ctx, odin3_design *design,
                             const odin3_lower_opts *opts);

#endif
