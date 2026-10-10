/* include.c — `include (PP-11): stubs until 2B Task 7. */
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"

#include <stdint.h>

odin3_status odin3_pp_include_directive(odin3_pp *pp, odin3_loc at) {
    return odin3_pp_error(pp, at, "syntax: `include not supported yet");
}

odin3_status odin3_pp_include_end(odin3_pp *pp) {
    (void)pp;
    return ODIN3_OK;
}
