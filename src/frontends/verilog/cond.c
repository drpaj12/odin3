/* cond.c — conditionals and translate_off (PP-3, PP-4, PP-10): stubs until 2B Task 7. */
#include "frontends/verilog/preproc_internal.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

static const char *const k_cond_names[] = {"", "ifdef", "ifndef", "elsif", "else", "endif"};

odin3_status odin3_pp_cond_directive(odin3_pp *pp, odin3_pp_cond_kind kind, uint32_t at) {
    return odin3_pp_error(pp, at, "syntax: `%s not supported yet", k_cond_names[kind]);
}

odin3_status odin3_pp_cond_comment(odin3_pp *pp, const odin3_pp_comment *comment, bool *drop) {
    (void)pp;
    (void)comment;
    *drop = false;
    return ODIN3_OK;
}

odin3_status odin3_pp_cond_file_end(odin3_pp *pp) {
    (void)pp;
    return ODIN3_OK;
}
