/*
 * sv.c — the SystemVerilog reader seam (ABI v6, v1.1.0; DESIGN §4.2): the one registered function
 * the read_verilog pass drives for .sv files. The core stays C17: the reader itself is the C++
 * adapter adapters/slang/libodin3_slang.so, which the CLI (or a host) registers at start-up and
 * which the core only reaches through this pointer.
 */
#include "api/api.h"

#include "api/sv.h"
#include "odin3/odin3.h"

#include <stddef.h>

static odin3_sv_read_fn registered;

ODIN3_EXPORT odin3_status odin3_sv_reader_register(odin3_sv_read_fn read) {
    if (read == NULL) {
        return odin3_api_invalid(__func__);
    }
    if (registered != NULL) {
        return odin3_api_invalid_why(__func__, "sv reader already registered");
    }
    registered = read;
    return ODIN3_OK;
}

odin3_sv_read_fn odin3_sv_reader_get(void) {
    return registered;
}
