/* expand.c — macro uses, substitution and rescan (PP-5..PP-7): a stub until 2B Task 8. */
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"

odin3_status odin3_pp_expand_use(odin3_pp *pp, const odin3_pp_use *use) {
    return odin3_pp_error(pp, use->at, "syntax: macro use '`%.*s' not supported yet",
                          (int)use->name.len, (const char *)use->name.ptr);
}
