/*
 * prov_name.h — provenance names for writers (IR.md §6 "Names in output"; 1F's Verilog writer,
 * 3E-6's BLIF writer): `hier/<short name>@file:line` from the record that names an object.
 */
#ifndef ODIN3_BACKENDS_PROV_NAME_H
#define ODIN3_BACKENDS_PROV_NAME_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The record that names an object with provenance prov: prov itself, or for a DERIVED record the
 * first leaf of the backward walk in parent order (odin3_prov_sources). *rec is NULL for none (no
 * provenance, an unknown ID, or a DERIVED record with no leaf); it belongs to the design. cache
 * (prov ID -> leaf ID, from odin3_u64map_create) remembers walks and may be NULL. NO_MEMORY, or
 * the walk's own status; *rec is NULL then.
 */
odin3_status odin3_prov_leaf(const odin3_design *design, odin3_prov_id prov, odin3_u64map *cache,
                             const odin3_prov_record **rec);

/*
 * The first location of rec as written in a name: *loc, its file (a strtab ID) and line. False for
 * NULL, a record with no location, no file, or a file name with a byte outside 0x20..0x7e.
 */
bool odin3_prov_location(const odin3_design *design, const odin3_prov_record *rec,
                         odin3_srcloc *loc);

/*
 * out = `hier/<short_name>@file:line` for the object with provenance prov (a part is left out when
 * the record has none: no `hier/` for hier 0, no `@file:line` without a location). The file is as
 * the record has it, never resolved. out is cleared first and stays empty when the record has
 * neither a hierarchy nor a location: the caller then keeps the short name. No rule of any
 * target language is applied. cache as odin3_prov_leaf. NO_MEMORY or the walk's status.
 */
odin3_status odin3_prov_name(const odin3_design *design, odin3_prov_id prov, odin3_bytes short_name,
                             odin3_u64map *cache, odin3_strbuf *out);

#endif
