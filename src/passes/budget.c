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

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The memory assumed when sysconf cannot tell: 8 GiB. */
#define BUDGET_FALLBACK_BYTES ((uint64_t)8 << 30)

enum {
    PERCENT = 100,
    CGROUP_PATH_BYTES = 4096, /* the longest cgroup path or file name read */
    CGROUP_LINE_BYTES = 4352, /* a /proc/self/cgroup line: the path and a little more */
    MEMORY_MAX_TEXT = 64,     /* the digits of a memory.max file */
    DECIMAL = 10
};

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_SELF "/proc/self/cgroup"

static uint64_t g_test_limit; /* 0: none */
static bool g_fake_cgroup;    /* the hook below is set */
static char g_fake_root[CGROUP_PATH_BYTES];
static char g_fake_self[CGROUP_PATH_BYTES];

void odin3_budget_test_set_limit(uint64_t bytes) {
    g_test_limit = bytes;
}

void odin3_budget_test_set_cgroup_root(const char *root, const char *self_cgroup) {
    g_fake_cgroup = root != NULL && self_cgroup != NULL;
    if (g_fake_cgroup) {
        (void)snprintf(g_fake_root, sizeof g_fake_root, "%s", root);
        (void)snprintf(g_fake_self, sizeof g_fake_self, "%s", self_cgroup);
    }
}

/* The path of the `0::<path>` line of /proc/self/cgroup (cgroup v2) into out; false when absent. */
static bool own_cgroup_path(char *out, size_t cap) {
    if (g_fake_cgroup) {
        return snprintf(out, cap, "%s", g_fake_self) < (int)cap;
    }
    FILE *file = fopen(CGROUP_SELF, "r");
    if (file == NULL) {
        return false;
    }
    char line[CGROUP_LINE_BYTES];
    bool found = false;
    while (!found && fgets(line, sizeof line, file) != NULL) {
        size_t len = strcspn(line, "\n");
        line[len] = '\0';
        found = strncmp(line, "0::", 3) == 0 && len - 3 < cap;
        if (found) {
            memcpy(out, line + 3, len - 3 + 1);
        }
    }
    (void)fclose(file);
    return found;
}

/* The number in <root><path>/memory.max, or UINT64_MAX for `max`, a missing file or text. */
static uint64_t memory_max_of(const char *root, const char *path) {
    char file_name[2 * CGROUP_PATH_BYTES];
    int len = snprintf(file_name, sizeof file_name, "%s%s/memory.max", root, path);
    FILE *file = len > 0 && (size_t)len < sizeof file_name ? fopen(file_name, "r") : NULL;
    if (file == NULL) {
        return UINT64_MAX;
    }
    char text[MEMORY_MAX_TEXT];
    uint64_t value = UINT64_MAX;
    if (fgets(text, sizeof text, file) != NULL && text[0] >= '0' && text[0] <= '9') {
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(text, &end, DECIMAL);
        if (errno == 0 && end != text && (*end == '\n' || *end == '\0')) {
            value = (uint64_t)parsed;
        }
    }
    (void)fclose(file);
    return value;
}

/*
 * The smallest numeric memory.max on the process's cgroup path and its ancestors (3D point (i):
 * under tools/heavy-run the job's own scope says `max` and the 6G cap sits on its slice), or
 * UINT64_MAX with no cgroup v2 file.
 */
static uint64_t cgroup_limit(void) {
    char path[CGROUP_PATH_BYTES];
    if (!own_cgroup_path(path, sizeof path)) {
        return UINT64_MAX;
    }
    const char *root = g_fake_cgroup ? g_fake_root : CGROUP_ROOT;
    uint64_t limit = UINT64_MAX;
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    for (;;) {
        uint64_t here = memory_max_of(root, path);
        limit = here < limit ? here : limit;
        char *slash = strrchr(path, '/');
        if (slash == NULL || len == 0) {
            break;
        }
        *slash = '\0';
        len = (size_t)(slash - path);
    }
    return limit;
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
    uint64_t share = physical / PERCENT * ODIN3_BUDGET_FRACTION +
                     physical % PERCENT * ODIN3_BUDGET_FRACTION / PERCENT;
    uint64_t cgroup = cgroup_limit();
    return cgroup < share ? cgroup : share;
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
    uint64_t limit = odin3_budget_limit();
    /* appendf appends nothing when it fails */
    return odin3_strbuf_appendf(out,
                                "%s needs more than %" PRIu64
                                " bytes (the design's budget is %" PRIu64
                                " bytes: the smaller of %d%% of physical memory and the cgroup's "
                                "memory.max)",
                                odin3_strtab_get(odin3_design_strtab(ctx->design), name), limit,
                                limit, ODIN3_BUDGET_FRACTION);
}
