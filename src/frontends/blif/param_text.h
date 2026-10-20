/*
 * param_text.h — the text of a `.param` value on a `.subckt` cell, as Yosys `write_blif -param`
 * writes it (backends/blif/blif.cc dump_params), shared by the BLIF reader and writer (1C
 * amendment 2026-10-19, PHASE5 decision 8):
 * - STRING: double-quoted; `"` and `\` are escaped with a backslash, a byte below 32 or from 127
 *   up is `\` and three octal digits;
 * - INT: the two's complement binary digits, MSB first, 32 of them when the value fits an int32,
 *   else 64 (Yosys writes an integer parameter as its 32-bit constant);
 * - BITS: one of `0 1 x z` per bit, MSB first (a zero-length value has no text).
 * Reading accepts exactly those forms (a binary INT of 32 or 64 digits is signed, any other
 * length unsigned).
 */
#ifndef ODIN3_FRONTENDS_BLIF_PARAM_TEXT_H
#define ODIN3_FRONTENDS_BLIF_PARAM_TEXT_H

#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>

/*
 * Appends the text of value (kind INT, BITS or STRING) to out. ODIN3_ERR_INVALID_ARG for another
 * kind or a zero-length BITS (nothing is appended); NO_MEMORY.
 */
odin3_status odin3_blif_param_format(const odin3_design *design, const odin3_value *value,
                                     odin3_strbuf *out);

/*
 * Parses text (len bytes) as a value of the given kind. A BITS value's bytes are stored in *bits
 * (cleared first), so the result is valid while *bits is. A STRING is interned in design.
 * ODIN3_ERR_PARSE when the text is not one of the forms above (nothing logged); NO_MEMORY.
 */
odin3_status odin3_blif_param_parse(odin3_design *design, odin3_value_kind kind, odin3_bytes text,
                                    odin3_strbuf *bits, odin3_value *out);

#endif
