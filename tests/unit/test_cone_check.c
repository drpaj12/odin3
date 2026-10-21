/*
 * test_cone_check.c — the cone_check pass (Phase 7 spec 3.3, plan Task 7): a ripple adder cone
 * against `$add` is equivalent, a swapped binding differs with a counterexample, a forced 0 s
 * timeout is unproven, a register is refused, an incomplete binding is a located error before any
 * proof, and the cone hash is stable. The proofs use the linked ABC when the build has it. Designs
 * are small BLIF texts in a mkdtemp directory under the build tree.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/cone_check/cone_check.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ODIN3_HAVE_REAL_ABC
#include "odin3_abc.h"
#endif

#ifndef ODIN3_CC_ROOT
#error "ODIN3_CC_ROOT must name the build tree"
#endif

enum {
    PATH_BUF = 512,
    DIR_BUF = 256,
    LOG_MAX = 16384,
    HASH_HEX = 65,
    CEX_BITS = 8,
    PAIR_BYTES = 16
};

/* 4-bit ripple adder: a half adder at bit 0, full adders above; the last carry is unused. */
#define RIPPLE                                                                                     \
    ".model m\n.inputs a0 a1 a2 a3 b0 b1 b2 b3\n.outputs s0 s1 s2 s3\n"                            \
    ".names a0 b0 s0\n10 1\n01 1\n.names a0 b0 c1\n11 1\n"                                         \
    ".names a1 b1 c1 s1\n001 1\n010 1\n100 1\n111 1\n.names a1 b1 c1 c2\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a2 b2 c2 s2\n001 1\n010 1\n100 1\n111 1\n.names a2 b2 c2 c3\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a3 b3 c3 s3\n001 1\n010 1\n100 1\n111 1\n.end\n"

#define ADD4 "-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=4"

static char dir[DIR_BUF];
static odin3_design *design;
static char log_text[LOG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level <= ODIN3_LOG_INFO) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/cc.XXXXXX", ODIN3_CC_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    log_text[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    DIR *handle = opendir(dir);
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL;
         entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            (void)snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
            (void)unlink(path);
        }
    }
    if (handle != NULL) {
        (void)closedir(handle);
    }
    (void)rmdir(dir);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static void read_blif(const char *text) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/t.blif", dir);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "read_blif", odin3_bytes_cstr(path)), log_text);
}

static odin3_module *top(void) {
    return odin3_module_get(design, (odin3_module_id){1});
}

static uint32_t intern(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &id));
    return id;
}

static void set_str(odin3_objref obj, const char *key, const char *val) {
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, intern(val), 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(top(), obj, intern(key), &value));
}

static const char *text_attr(const char *key) {
    odin3_module_id id = odin3_module_id_of(top());
    const odin3_value *val =
        odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_MODULE, id.v}, intern(key));
    return val != NULL && val->kind == ODIN3_VAL_STRING
               ? odin3_strtab_get(odin3_design_strtab(design), val->str)
               : NULL;
}

/* Binds the net named in pair "net=PORT[BIT]". */
static void bind(const char *pair) {
    char net[PATH_BUF];
    const char *eq = strchr(pair, '=');
    TEST_ASSERT_NOT_NULL(eq);
    (void)snprintf(net, sizeof net, "%.*s", (int)(eq - pair), pair);
    odin3_net_id id = odin3_module_find_net(top(), intern(net));
    TEST_ASSERT_TRUE_MESSAGE(odin3_net_valid(id), net);
    set_str((odin3_objref){ODIN3_OBJ_NET, id.v}, "odin3:bind", eq + 1);
}

/* Every live node that is not a port node joins group grp=add4; ports are never in a group. */
static void mark_all(void) {
    odin3_module *mod = top();
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) &&
            odin3_celltype_get(design, odin3_node_type(mod, node))->gran != ODIN3_GRAN_PORT) {
            set_str((odin3_objref){ODIN3_OBJ_NODE, nid}, "grp", "add4");
        }
    }
}

/* a_i to A[i], b_i to B[i], s_i to Y[i]; swap_a01 exchanges the bindings of a0 and a1. */
static void bind_adder(bool swap_a01) {
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, swap_a01 && i < 2 ? 1 - i : i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "b%d=B[%d]", i, i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "s%d=Y[%d]", i, i);
        bind(pair);
    }
}

static void load_adder(bool swap_a01) {
    read_blif(RIPPLE);
    mark_all();
    bind_adder(swap_a01);
}

static odin3_status run(const char *args) {
    return odin3_pass_run(design, "cone_check", odin3_bytes_cstr(args));
}

static void check_clean(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(design, opts), log_text);
}

/* The sizes of module storage and the live node count: an unchanged design has equal ones. */
typedef struct shape {
    uint32_t nodes, pins, nets, live;
} shape;

static shape shape_of(void) {
    odin3_module *mod = top();
    shape out = {odin3_module_node_end(mod), odin3_module_pin_end(mod), odin3_module_net_end(mod),
                 0};
    for (uint32_t nid = 1; nid < out.nodes; nid++) {
        out.live += odin3_node_live(mod, (odin3_node_id){nid}) ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_module_end(design) - 1);
    return out;
}

static void expect_same(shape before) {
    shape after = shape_of();
    TEST_ASSERT_EQUAL_UINT32(before.nodes, after.nodes);
    TEST_ASSERT_EQUAL_UINT32(before.pins, after.pins);
    TEST_ASSERT_EQUAL_UINT32(before.nets, after.nets);
    TEST_ASSERT_EQUAL_UINT32(before.live, after.live);
}

static void use_real_abc(void) {
#ifdef ODIN3_HAVE_REAL_ABC
    static bool registered;
    if (!registered) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_adapter_init());
        registered = true;
    }
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

/* The text a refusal must log. */
typedef struct needle {
    const char *text;
} needle;

#define FAILS_WITH(args, text) expect_error(args, (needle){text})

static void expect_error(const char *args, needle must_log) {
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(ODIN3_OK, run(args), log_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, must_log.text), log_text);
}

/* --- the cases of plan Task 7 step 1 ---------------------------------------------------------- */

static void test_cone_check_ripple_adder_equivalent(void) {
    use_real_abc();
    load_adder(false);
    shape before = shape_of();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
    TEST_ASSERT_EQUAL_STRING("grp=add4", text_attr("odin3:cone_key"));
    TEST_ASSERT_EQUAL_size_t(HASH_HEX - 1, strlen(text_attr("odin3:cone_hash")));
    TEST_ASSERT_NULL(text_attr("odin3:cone_cex"));
    expect_same(before);
    check_clean();
}

static void test_cone_check_swapped_binding_differs(void) {
    use_real_abc();
    load_adder(true);
    shape before = shape_of();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    TEST_ASSERT_EQUAL_STRING("differs", text_attr("odin3:cone_check"));
    const char *cex = text_attr("odin3:cone_cex");
    TEST_ASSERT_NOT_NULL(cex);
    int pairs = 0;
    for (const char *at = cex; *at != '\0'; at++) {
        pairs += *at == '=' ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(CEX_BITS, pairs, cex);
    TEST_ASSERT_NOT_NULL(strstr(cex, "A[0]="));
    TEST_ASSERT_NOT_NULL(strstr(cex, "B[3]="));
    expect_same(before);
    check_clean();
}

static void test_cone_check_timeout_unproven(void) {
    load_adder(false);
    shape before = shape_of();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4 " -timeout 0"), log_text);
    TEST_ASSERT_EQUAL_STRING("unproven", text_attr("odin3:cone_check"));
    TEST_ASSERT_NULL(text_attr("odin3:cone_cex"));
    expect_same(before);
}

static void test_cone_check_register_refused(void) {
    read_blif(".model m\n.inputs a b clk\n.outputs q\n.names a b d\n11 1\n"
              ".latch d q re clk 0\n.end\n");
    mark_all();
    bind("a=A[0]");
    bind("b=A[1]");
    bind("q=Y[0]");
    shape before = shape_of();
    FAILS_WITH("-attr grp=add4 -cell $and -param A_WIDTH=2", "register");
    TEST_ASSERT_NULL(text_attr("odin3:cone_check"));
    expect_same(before);
}

static void test_cone_check_binding_incomplete(void) {
    load_adder(false);
    bind("s3=Y[2]"); /* Y[2] twice, Y[3] never */
    shape before = shape_of();
    FAILS_WITH(ADD4, "Y[2] is bound twice");
    TEST_ASSERT_NOT_NULL(strstr(log_text, "Y[3] is not bound"));
    TEST_ASSERT_NULL(text_attr("odin3:cone_check"));
    expect_same(before);
}

static void test_cone_check_binding_missing_and_unknown(void) {
    load_adder(false);
    odin3_net_id net = odin3_module_find_net(top(), intern("b2"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_remove(top(), (odin3_objref){ODIN3_OBJ_NET, net.v},
                                                      intern("odin3:bind")));
    FAILS_WITH(ADD4, "has no odin3:bind");
    log_text[0] = '\0';
    bind("b2=B[9]");
    FAILS_WITH(ADD4, "B[9]");
    log_text[0] = '\0';
    bind("b2=Z[0]");
    FAILS_WITH(ADD4, "no port 'Z'");
    log_text[0] = '\0';
    bind("b2=Y[2]"); /* an input net bound to an output bit */
    FAILS_WITH(ADD4, "direction");
}

static void test_cone_check_hash_stable(void) {
    use_real_abc();
    load_adder(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    char first[HASH_HEX];
    (void)snprintf(first, sizeof first, "%s", text_attr("odin3:cone_hash"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4), log_text);
    TEST_ASSERT_EQUAL_STRING(first, text_attr("odin3:cone_hash"));
    char direct[HASH_HEX];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(top(), "grp", "add4", direct));
    TEST_ASSERT_EQUAL_STRING(first, direct);
    bind_adder(true); /* an edited binding is another hash */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(top(), "grp", "add4", direct));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(first, direct));
}

static void test_cone_check_usage_errors(void) {
    load_adder(false);
    FAILS_WITH("-cell $add", "-attr");
    FAILS_WITH("-attr grp=add4", "-cell");
    FAILS_WITH("-attr grp=nothing -cell $add", "empty");
    FAILS_WITH("-attr grp=add4 -cell $nosuch", "$nosuch");
    FAILS_WITH(ADD4 " -timeout x", "-timeout");
    FAILS_WITH(ADD4 " -bogus", "-bogus");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cone_check_ripple_adder_equivalent);
    RUN_TEST(test_cone_check_swapped_binding_differs);
    RUN_TEST(test_cone_check_timeout_unproven);
    RUN_TEST(test_cone_check_register_refused);
    RUN_TEST(test_cone_check_binding_incomplete);
    RUN_TEST(test_cone_check_binding_missing_and_unknown);
    RUN_TEST(test_cone_check_hash_stable);
    RUN_TEST(test_cone_check_usage_errors);
    return UNITY_END();
}
