/* pp_check.c — the preprocessor's stream invariants (spec §7.8): stubs until 2B Task 8. */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"

#include <stdint.h>

odin3_status odin3_pp_check(const odin3_pp *pp, odin3_srcbuf_id stream) {
    (void)pp;
    (void)stream;
    return ODIN3_OK;
}

void odin3_pp_check_note(odin3_pp *pp, odin3_loc loc) {
    (void)pp;
    (void)loc;
}
