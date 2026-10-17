/*
 * test_budget.c — the design-wide byte budget (2E spec §16, plan point (d); 2F spec §19):
 * odin3_design_bytes_reserved grows with the design and an AST store counts, the check is silent
 * and exact, the message names the run, and the default limit is 60% of physical memory.
 * Yosys re-derivation: none (no proc phase runs here).
 */
#include "ast/ast.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/budget.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_BUDGET_OUT
#error "ODIN3_BUDGET_OUT must name a writable directory for the fake cgroup trees"
#endif

enum { NETS = 100000, NET_GROWTH = 400000, FRESH_MIN = 64 * 1024, EXTRA = 4096, MSG_MAX = 256 };

static odin3_design *design;
static odin3_pass_ctx ctx;
static size_t n_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
    n_logged++;
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("proc"), &ctx));
    n_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_budget_test_set_limit(0);
    odin3_budget_test_set_cgroup_root(NULL, NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* 100,000 nets add at least 4 bytes each; an AST store counts in the budget's measure. */
static void test_bytes_reserved_grows(void) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("m"), (odin3_prov_id){0}, &id));
    odin3_module *module = odin3_module_get(design, id);
    uint64_t before = odin3_design_bytes_reserved(design);
    for (uint32_t i = 0; i < NETS; i++) {
        odin3_net_id net = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(module, 0, (odin3_prov_id){0}, &net));
    }
    uint64_t after = odin3_design_bytes_reserved(design);
    TEST_ASSERT_TRUE(after >= before + NET_GROWTH);
    odin3_ast *ast = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, ctx.run, &ast));
    size_t ast_bytes = odin3_ast_bytes_reserved(ast);
    TEST_ASSERT_TRUE(ast_bytes > 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_ast(design, ast));
    uint64_t held = odin3_design_bytes_reserved(design);
    TEST_ASSERT_TRUE(held >= after);
    /* the measure is exactly held + the store's bytes: one byte less is over the limit */
    odin3_budget_test_set_limit(held + ast_bytes);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_budget_check(&ctx, design, 0));
    odin3_budget_test_set_limit(held + ast_bytes - 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_budget_check(&ctx, design, 0));
    TEST_ASSERT_EQUAL_size_t(0, n_logged);
}

/* At R + 4096: 4096 extra bytes pass, 4097 do not, silently; the message names the run. */
static void test_check_and_message(void) {
    uint64_t reserved = odin3_design_bytes_reserved(design);
    TEST_ASSERT_TRUE(reserved > FRESH_MIN);
    odin3_budget_test_set_limit(reserved + EXTRA);
    TEST_ASSERT_EQUAL_UINT64(reserved + EXTRA, odin3_budget_limit());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_budget_check(&ctx, design, EXTRA));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_budget_check(&ctx, design, EXTRA + 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_budget_check(&ctx, design, UINT64_MAX));
    TEST_ASSERT_EQUAL_size_t(0, n_logged);
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_budget_message(&ctx, &msg));
    char want[MSG_MAX];
    int len = snprintf(want, sizeof want,
                       "proc needs more than %" PRIu64 " bytes (the design's budget is %" PRIu64
                       " bytes: the smaller of 60%% of physical memory and the cgroup's "
                       "memory.max)",
                       reserved + EXTRA, reserved + EXTRA);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, msg.data);
    TEST_ASSERT_EQUAL_size_t(0, n_logged);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_budget_check(NULL, design, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_budget_check(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_budget_message(NULL, &msg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_budget_message(&ctx, NULL));
    TEST_ASSERT_EQUAL_size_t(4, n_logged);
    /* out of memory: nothing appended */
    odin3_strbuf empty;
    odin3_strbuf_init(&empty);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_budget_message(&ctx, &empty));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_size_t(0, empty.len);
    odin3_strbuf_free(&empty);
    odin3_strbuf_free(&msg);
}

/* Without the hooks the limit is 60% of the physical memory sysconf reports (an empty fake tree).
 */
static void test_limit_default(void) {
    odin3_budget_test_set_limit(0);
    odin3_budget_test_set_cgroup_root(ODIN3_BUDGET_OUT "/empty", "/a.slice/b.scope");
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    TEST_ASSERT_TRUE(pages > 0 && page_size > 0);
    uint64_t physical = (uint64_t)pages * (uint64_t)page_size;
    uint64_t want = physical / 100 * 60 + physical % 100 * 60 / 100;
    TEST_ASSERT_EQUAL_UINT64(want, odin3_budget_limit());
    TEST_ASSERT_EQUAL_INT(60, ODIN3_BUDGET_FRACTION);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_budget_check(&ctx, design, 0));
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE_MESSAGE(mkdir(path, 0755) == 0 || errno == EEXIST, path);
}

static void write_text(const char *path, odin3_bytes text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_UINT(text.len, fwrite(text.ptr, 1, text.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/*
 * A fake tree: the scope's memory.max is `max` and its parent slice's is 6442450944 (what
 * tools/heavy-run sets up); the limit is the smaller of that and 60% of physical memory. A tighter
 * scope wins; a non-number is ignored; an own cgroup of "/" reads only the root.
 */
static void test_limit_cgroup(void) {
    odin3_budget_test_set_limit(0);
    make_dir(ODIN3_BUDGET_OUT);
    make_dir(ODIN3_BUDGET_OUT "/tree");
    make_dir(ODIN3_BUDGET_OUT "/tree/a.slice");
    make_dir(ODIN3_BUDGET_OUT "/tree/a.slice/b.scope");
    write_text(ODIN3_BUDGET_OUT "/tree/a.slice/b.scope/memory.max", odin3_bytes_cstr("max\n"));
    write_text(ODIN3_BUDGET_OUT "/tree/a.slice/memory.max", odin3_bytes_cstr("6442450944\n"));
    odin3_budget_test_set_cgroup_root(ODIN3_BUDGET_OUT "/tree", "/a.slice/b.scope");
    uint64_t limit = odin3_budget_limit();
    TEST_ASSERT_TRUE(limit <= 6442450944ULL);
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    uint64_t physical = (uint64_t)pages * (uint64_t)page_size;
    uint64_t share = physical / 100 * 60 + physical % 100 * 60 / 100;
    TEST_ASSERT_EQUAL_UINT64(share < 6442450944ULL ? share : 6442450944ULL, limit);
    write_text(ODIN3_BUDGET_OUT "/tree/a.slice/b.scope/memory.max", odin3_bytes_cstr("1048576\n"));
    TEST_ASSERT_EQUAL_UINT64(1048576, odin3_budget_limit());
    write_text(ODIN3_BUDGET_OUT "/tree/a.slice/b.scope/memory.max", odin3_bytes_cstr("garbage\n"));
    TEST_ASSERT_EQUAL_UINT64(share < 6442450944ULL ? share : 6442450944ULL, odin3_budget_limit());
    odin3_budget_test_set_cgroup_root(ODIN3_BUDGET_OUT "/tree", "/");
    TEST_ASSERT_EQUAL_UINT64(share, odin3_budget_limit());
    /* the message names the smaller bound's number */
    write_text(ODIN3_BUDGET_OUT "/tree/a.slice/b.scope/memory.max", odin3_bytes_cstr("4096\n"));
    odin3_budget_test_set_cgroup_root(ODIN3_BUDGET_OUT "/tree", "/a.slice/b.scope");
    odin3_strbuf msg;
    odin3_strbuf_init(&msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_budget_message(&ctx, &msg));
    TEST_ASSERT_EQUAL_STRING("proc needs more than 4096 bytes (the design's budget is 4096 bytes: "
                             "the smaller of 60% of physical memory and the cgroup's memory.max)",
                             msg.data);
    odin3_strbuf_free(&msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_reserved_grows);
    RUN_TEST(test_check_and_message);
    RUN_TEST(test_limit_default);
    RUN_TEST(test_limit_cgroup);
    return UNITY_END();
}
