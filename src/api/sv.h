/*
 * sv.h — the process's SystemVerilog reader (private, ABI v6): the registered function the
 * read_verilog pass drives for .sv files (src/api/sv.c). Never in odin3.h beyond the register
 * function and the typedef it takes.
 */
#ifndef ODIN3_API_SV_H
#define ODIN3_API_SV_H

#include "odin3/odin3.h"

/* The registered reader, NULL when none is (the CLI registers the slang adapter's). */
odin3_sv_read_fn odin3_sv_reader_get(void);

#endif
