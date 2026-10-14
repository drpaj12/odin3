/*
 * splice.c — PLACEHOLDER for plan Task 17 (the splice: phases A to D, special nets, the init-1
 * restore). The caller of Tasks 14 to 16 stops here with a clear error until Task 17 replaces this
 * file; it never changes the IR.
 */
#include "passes/abc/splice.h"

#include "util/log.h"

odin3_status odin3_abc_splice(struct odin3_abc_run *run, const odin3_abc_region *region,
                              odin3_abc_result *res) {
    (void)run;
    (void)region;
    (void)res;
    odin3_log(ODIN3_LOG_ERROR, "abc: the splice is not implemented yet (3D Task 17)");
    return ODIN3_ERR_INVALID_ARG;
}
