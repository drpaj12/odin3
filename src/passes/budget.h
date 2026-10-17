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
 * The limit in bytes: the smaller of ODIN3_BUDGET_FRACTION% of sysconf(_SC_PHYS_PAGES) x
 * sysconf(_SC_PAGESIZE) (or of 8 GiB when either call fails) and the smallest numeric
 * `memory.max` on the process's cgroup path and its ancestors (3D point (i): the `0::<path>` line
 * of /proc/self/cgroup, then /sys/fs/cgroup<path>/memory.max up to the root; `max` and missing
 * files do not count). Under tools/heavy-run the job's own scope says `max` and the 6G cap sits on
 * its slice, so the walk finds it. The test hook's value instead while one is set.
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
 * Appends "<pass> needs more than <limit> bytes (the design's budget is <limit> bytes: the smaller
 * of 60% of physical memory and the cgroup's memory.max)" (the one message of every pass, 3D point
 * (i)) to out, where <pass> is the name of ctx's pass run and <limit> odin3_budget_limit(). Logs
 * nothing else. ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a run without a name;
 * ODIN3_ERR_NO_MEMORY with out unchanged.
 */
odin3_status odin3_budget_message(const odin3_pass_ctx *ctx, odin3_strbuf *out);

/* Test hook: the limit becomes bytes (0 restores the physical-memory limit). */
void odin3_budget_test_set_limit(uint64_t bytes);

/*
 * Test hook: reads the cgroup limit from the directory root (instead of /sys/fs/cgroup) for the
 * cgroup path self_cgroup (instead of /proc/self/cgroup's `0::` line), e.g. root with
 * `<root>/a.slice/b.scope/memory.max` for self_cgroup "/a.slice/b.scope". Both strings are copied;
 * an empty root directory means no cgroup limit. NULL for either restores the real files.
 */
void odin3_budget_test_set_cgroup_root(const char *root, const char *self_cgroup);

#endif
