/*
 * budget.c — the design-wide byte budget (2E spec §16, plan point (d); 2F spec §19): the limit,
 * the silent check and the message.
 */
#include "passes/budget.h"

#include "ast/ast.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdint.h>
#include <unistd.h>

/* The memory assumed when sysconf cannot tell: 8 GiB. */
#define BUDGET_FALLBACK_BYTES ((uint64_t)8 << 30)

enum { PERCENT = 100 };

static uint64_t g_test_limit; /* 0: none */

void odin3_budget_test_set_limit(uint64_t bytes) {
    g_test_limit = bytes;
}

uint64_t odin3_budget_limit(void) {
    if (g_test_limit != 0) {
        return g_test_limit;
    }
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    uint64_t physical = BUDGET_FALLBACK_BYTES;
    if (pages > 0 && page_size > 0 && (uint64_t)pages <= UINT64_MAX / (uint64_t)page_size) {
        physical = (uint64_t)pages * (uint64_t)page_size;
    }
    return physical / PERCENT * ODIN3_BUDGET_FRACTION +
           physical % PERCENT * ODIN3_BUDGET_FRACTION / PERCENT;
}

static uint64_t add_saturating(uint64_t lhs, uint64_t rhs) {
    return lhs > UINT64_MAX - rhs ? UINT64_MAX : lhs + rhs;
}

odin3_status odin3_budget_check(const odin3_pass_ctx *ctx, const odin3_design *design,
                                uint64_t extra_bytes) {
    if (ctx == NULL || design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "budget_check: no pass run or no design");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t held = add_saturating(odin3_design_bytes_reserved(design), extra_bytes);
    uint32_t stores = odin3_design_ast_count(design);
    for (uint32_t i = 0; i < stores; i++) {
        held = add_saturating(held, odin3_ast_bytes_reserved(odin3_design_ast_at(design, i)));
    }
    return held <= odin3_budget_limit() ? ODIN3_OK : ODIN3_ERR_PARSE;
}

odin3_status odin3_budget_message(const odin3_pass_ctx *ctx, odin3_strbuf *out) {
    uint32_t name =
        ctx != NULL && ctx->design != NULL ? odin3_passrun_name(ctx->design, ctx->run) : 0;
    if (name == 0 || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "budget_message: no pass run, no run name or no output");
        return ODIN3_ERR_INVALID_ARG;
    }
    /* appendf appends nothing when it fails */
    return odin3_strbuf_appendf(out,
                                "%s needs more than %" PRIu64
                                " bytes (the design's budget is %d%% of physical memory)",
                                odin3_strtab_get(odin3_design_strtab(ctx->design), name),
                                odin3_budget_limit(), ODIN3_BUDGET_FRACTION);
}
