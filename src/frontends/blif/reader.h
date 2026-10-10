/*
 * reader.h — BLIF reader: models, ports and black boxes (pass 1), bodies (pass 2), into the IR.
 */
#ifndef ODIN3_FRONTENDS_BLIF_READER_H
#define ODIN3_FRONTENDS_BLIF_READER_H

#include "frontends/blif/attrs.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

/* The attributes the reader sets (blif_clock, blif_name, .attr/.param, blif_extras) and their keys
 * are documented in frontends/blif/attrs.h. */

/*
 * Reads the BLIF file at path into design, which must be fresh (no modules). One pass run
 * "read_blif" (odin3_blif_read_in: the caller's run instead); every module, port node, port wire
 * and port net gets an IMPORTED provenance record {file = path, line, col = 1-based token index of
 * its defining token, end_col}.
 *
 * Pass 1 reads every `.model` header: models become modules in file order (the first is the top,
 * BLIF's rule, recorded with odin3_design_set_top),
 * `.inputs`/`.outputs` become ports in file order, and `.blackbox` models become black-box cell
 * types (odin3_celltype_declare_blackbox, IR-7b), also in file order. A black box whose name is a
 * registered cell type (a tech library loaded beforehand, odin3_techlib_read) must match it by
 * port name, direction and width, its declared widths giving the type's parameters
 * (odin3_celltype_blackbox_match); the declared-model entry keeps those parameters and the
 * declaration as written. Port grouping: a run of
 * names `a[0] a[1] … a[w-1]` that appear consecutively, in this order, in one directive is one
 * vector port `a` of width w (written with brackets); every other name is a scalar port with its
 * exact name; ports are never reordered. Every port net is named by its exact BLIF bit name
 * (`x`, or `a[k]` for bit k of vector `a`); a name in both `.inputs` and `.outputs` is one net
 * shared by both ports. The first model must be a module (it is the top).
 *
 * Pass 2 reads the model bodies, so a `.subckt` may name a model defined later in the file. Cells
 * are created in file order (node ID order = file order, IR-16), each with all its connections
 * (odin3_node_create_connected); a net is created on its first reference, named exactly as
 * written, unless a port net already has that name:
 * - `.names i1 … in y` + cover rows: `$sop` with WIDTH n and COVER the rows as written (input
 *   characters `0` `1` `-`, then the output character); all rows of one cover have the same output
 *   character (BLIF forbids mixing on-set and off-set rows); `.names y` with no rows is constant
 *   0, with the row `1` constant 1;
 * - `.latch in out [type ctrl] [init]`: type `re`/`fe` → `$_DFF_P_`/`$_DFF_N_` (C = ctrl),
 *   `ah`/`al` → `$_DLATCH_P_`/`$_DLATCH_N_` (E = ctrl), no type → `$_FF_`; INIT = init (0..3),
 *   default 3. Known limitation: a control of `NIL` (SIS: no clock) is read as a net named NIL;
 * - `.subckt m f=a …`: a node of cell type m (a module of the file, a black box, or another
 *   registered type); formal f is the name of a scalar port, or `p[k]` for bit k of a vector port
 *   p (a scalar port's exact name wins, so a scalar port `a[1]` is matched by name; a bare `o`
 *   never names the width-1 vector port `o[0]`; for a model the file declares, a port is scalar
 *   or vector as the declaration writes it, so Odin II's `cin[0]` names a library port `cin` of
 *   width 1); formals not listed stay unconnected. Parameters: a model the file declares gives its
 *   declared-model parameters; any other type gets the values its formals imply, each port seen
 *   as wide as its largest bit index + 1 (odin3_celltype_infer_params: a parameter that sizes a
 *   port takes that width; types sized otherwise keep their defaults), so `.subckt $pow A[1]=a
 *   Y[7]=y` of a registered parametric `$pow` has A_WIDTH 2 and Y_WIDTH 8; a formal implying a
 *   port wider than ODIN3_READER_MAX_WIDTH (2^20 bits), or an undeclared instance whose type
 *   gives any port more (inferred parameters, or a registered constant width), or more than
 *   ODIN3_READER_MAX_TOTAL_WIDTH (2^22) bits over all its ports, is a parse error: one short
 *   line never allocates more (a maximal line costs about 70 MB of pins). Instances of a model of
 *   the file are sized by its definition and not capped. A
 *   bit beyond the instance's port width is an unknown formal. Known limitation: without a
 *   declaration the type's own scalar flags apply, so an undeclared `.subckt adder cin[0]=x`
 *   (library port `cin` scalar) or `data=x` (vector `data`) is an unknown formal. A model that
 *   is neither in the file nor registered (Yosys writes `$pow`, `$_DFFSR_PPP_`, … without a
 *   `.model`) becomes an implicit black box: a local cell type of granularity BLACKBOX that is not
 *   in the declared-model list (writers emit no `.model` for it), whose ports are the distinct
 *   formals used with it in the file, in order of first use, each scalar, width 1 and INOUT (the
 *   file gives no directions);
 * - `.cname`, `.attr`, `.param` apply to the previous cell of the model (see above).
 * Several drivers on one net are read as written (odin3_check_design reports them, rule 4).
 * Provenance: every cell and every net created in pass 2 gets its own IMPORTED record, a cell
 * {line of its directive, col 1, end_col = its token count}, a net {line, col = end_col = index
 * of its token}.
 *
 * Errors are logged as "path:line: message". ODIN3_ERR_PARSE for malformed input (a directive
 * outside a model, a duplicate model, a model without `.end`, a port declared twice, a black-box
 * declaration that conflicts with a registered cell type (the reason follows), a black box with a
 * body, a black box as
 * the first model, a black box that repeats a port name, an unknown directive, a cover row that
 * does not fit its `.names` or mixes on-set and off-set rows, an unsupported latch type (`as`) or
 * init, a `.subckt` of its own model or of a port cell type, an unknown formal, a formal connected
 * twice, a duplicate `.cname`, `.cname`/`.attr`/`.param` without a previous cell, a file that
 * changed between the passes); ODIN3_ERR_IO when the file cannot be read; ODIN3_ERR_NO_MEMORY on
 * out of memory; ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a design that already has
 * modules. Reading stops at the first error; the design may then hold part of the file, so the
 * caller destroys it.
 */
odin3_status odin3_blif_read(odin3_design *design, const char *path);

/*
 * odin3_blif_read into ctx->design inside the caller's pass run ctx (the pass manager's read_blif
 * pass), so the records belong to that run and no second run is opened; ctx->op becomes the last
 * operation the reader began. INVALID_ARG (logged) also for a NULL ctx or a ctx whose run is not
 * a run of its design.
 */
odin3_status odin3_blif_read_in(odin3_pass_ctx *ctx, const char *path);

/* Test hook: hook(user) runs between pass 1 and pass 2 of every later read (NULL: none). */
void odin3_blif_test_set_between_passes(void (*hook)(void *user), void *user);

#endif
