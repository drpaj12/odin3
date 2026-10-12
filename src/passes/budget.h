/*
 * budget.h — the design-wide byte budget (2E spec §16, plan point (d); 2F spec §19): one limit
 * for the whole design, not one per pass, measured on what the design holds, so 2D's, 2E's and
 * 2F's checks share one 60% of physical memory and cannot add up to more (a host out of memory
 * kills the WSL VM). Shared by proc (2E) and the opt family (2F); 2D may switch to it (2E §23).
 */
#ifndef ODIN3_PASSES_BUDGET_H
#define ODIN3_PASSES_BUDGET_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdint.h>

/* The share of physical memory the design may hold, in percent. */
enum { ODIN3_BUDGET_FRACTION = 60 };

/*
 * The limit in bytes: ODIN3_BUDGET_FRACTION% of sysconf(_SC_PHYS_PAGES) x sysconf(_SC_PAGESIZE),
 * or of 8 GiB when either call fails; the test hook's value instead while one is set. Physical
 * memory, not a cgroup cap: under tools/heavy-run's 6G slice the limit lies above the cap, so a
 * sweep that outgrows the slice ends with the OOM killer's 137, not E6. E6 guards ad hoc runs.
 */
uint64_t odin3_budget_limit(void);

/*
 * Silent check (the caller logs, located or not, with odin3_budget_message): ODIN3_OK when
 * odin3_design_bytes_reserved(design) + odin3_ast_bytes_reserved of every AST store the design
 * holds + extra_bytes (the caller's own working memory) is at most odin3_budget_limit(), else
 * ODIN3_ERR_PARSE (2D's status for a cap). The sum saturates rather than wraps. Logs nothing and
 * never allocates; ctx is only read (a caller's odin3_pass_ctx * converts). ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL ctx or design.
 */
odin3_status odin3_budget_check(const odin3_pass_ctx *ctx, const odin3_design *design,
                                uint64_t extra_bytes);

/*
 * Appends "<pass> needs more than <limit> bytes (the design's budget is 60% of physical memory)"
 * to out, where <pass> is the name of ctx's pass run and <limit> odin3_budget_limit(). Logs
 * nothing else. ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a run without a name;
 * ODIN3_ERR_NO_MEMORY with out unchanged.
 */
odin3_status odin3_budget_message(const odin3_pass_ctx *ctx, odin3_strbuf *out);

/* Test hook: the limit becomes bytes (0 restores the physical-memory limit). */
void odin3_budget_test_set_limit(uint64_t bytes);

#endif
