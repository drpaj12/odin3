/*
 * prov_name.h — provenance names for writers (IR.md §6 "Names in output"; 1F's Verilog writer,
 * 3E-6's BLIF writer): `hier/<short name>@file:line` from the record that names an object. Only
 * HDL locations name an object (spec 5.4 W-2): a SOURCE leaf, never an IMPORTED one.
 */
#ifndef ODIN3_BACKENDS_PROV_NAME_H
#define ODIN3_BACKENDS_PROV_NAME_H

#include "backends/common/hdl_loc.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stddef.h>

/*
 * out = `hier/<short_name>@file:line` for the object with provenance prov, from its first HDL
 * leaf (odin3_hdl_first_record): `hier/` is left out when that record has no hierarchy; the
 * location is the record's first (file as the record has it, never resolved; line only). out is
 * cleared first and stays empty when the object has no HDL leaf: the caller then keeps the short
 * name. No rule of any target language is applied. NO_MEMORY or the walk's status.
 */
odin3_status odin3_prov_name(odin3_hdl_ctx *hdl, odin3_prov_id prov, odin3_bytes short_name,
                             odin3_strbuf *out);

/*
 * The number of leading bytes of short_name that repeat the end of hier, written with `.` for
 * `/`: for hier `top/u1` and the flattened name `u1.x` it is 3 (`u1.`), so `hier/x` is the name
 * and the instance path is not doubled (`top/u1/u1.x`). The longest such suffix of hier wins; 0
 * when none matches or nothing would remain of the name. Writers that take a flattened short name
 * apply it before odin3_prov_name.
 */
size_t odin3_prov_hier_overlap(odin3_bytes hier, odin3_bytes short_name);

#endif
