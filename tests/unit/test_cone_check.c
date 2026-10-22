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
#include "passes/cone_check/cone_check_int.h"
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
    PAIR_BYTES = 16,
    CONST_TEST_BITS = 64
};

/* 4-bit ripple adder: a half adder at bit 0, full adders above; the last carry is unused. */
#define RIPPLE                                                                                     \
    ".model m\n.inputs a0 a1 a2 a3 b0 b1 b2 b3\n.outputs s0 s1 s2 s3\n"                            \
    ".names a0 b0 s0\n10 1\n01 1\n.names a0 b0 c1\n11 1\n"                                         \
    ".names a1 b1 c1 s1\n001 1\n010 1\n100 1\n111 1\n.names a1 b1 c1 c2\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a2 b2 c2 s2\n001 1\n010 1\n100 1\n111 1\n.names a2 b2 c2 c3\n11- 1\n1-1 1\n-11 1\n"    \
    ".names a3 b3 c3 s3\n001 1\n010 1\n100 1\n111 1\n.end\n"

#define ADD4 "-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=4"

#define EQ4 "-attr grp=add4 -cell $eq -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=1"

/* y = (a == 13): the one row 1011 is a0=1 a1=0 a2=1 a3=1. */
#define EQ13_BLIF ".model m\n.inputs a0 a1 a2 a3\n.outputs y\n.names a0 a1 a2 a3 y\n1011 1\n.end\n"

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

static void test_cone_check_hash_sees_rewiring(void) {
    load_adder(false);
    char before[HASH_HEX];
    char after[HASH_HEX];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(top(), "grp", "add4", before));
    /* a0 feeds two nodes; one reads b1 instead: same node IDs, same boundary nets, same bindings */
    odin3_module *mod = top();
    odin3_net_id a0 = odin3_module_find_net(mod, intern("a0"));
    odin3_net_id b1 = odin3_module_find_net(mod, intern("b1"));
    odin3_pinlist sinks = odin3_net_sinks(mod, a0);
    TEST_ASSERT_TRUE(sinks.count >= 2);
    odin3_pin_id pin = sinks.pins[0];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_disconnect(mod, pin));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pin_connect(mod, pin, b1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(mod, "grp", "add4", after));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(before, after));
}

static void test_cone_check_hash_sees_key_value(void) {
    load_adder(false);
    char one[HASH_HEX];
    char two[HASH_HEX];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(top(), "grp", "add4", one));
    odin3_module *mod = top();
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        if (odin3_node_live(mod, (odin3_node_id){nid}) &&
            odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, nid}, intern("grp")) != NULL) {
            set_str((odin3_objref){ODIN3_OBJ_NODE, nid}, "grp2", "add4");
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cone_hash(mod, "grp2", "add4", two));
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(one, two));
}

#define AND2_BLIF ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"

static void load_and2(void) {
    read_blif(AND2_BLIF);
    mark_all();
    bind("a=A[0]");
    bind("b=A[1]");
    bind("y=Y[0]");
}

static void test_cone_check_template_xz_refused(void) {
    load_and2();
    shape before = shape_of();
    FAILS_WITH("-attr grp=add4 -cell $lut -param WIDTH=2 -param LUT=10x0", "x or z");
    log_text[0] = '\0';
    FAILS_WITH("-attr grp=add4 -cell $lut -param WIDTH=2 -param LUT=1z00", "x or z");
    TEST_ASSERT_NULL(text_attr("odin3:cone_check"));
    expect_same(before);
}

static void test_cone_check_constant_x_unproven(void) {
    load_and2();
    odin3_module *mod = top();
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$_CONSTX_"), &type));
    odin3_node_id node = {0};
    odin3_node_spec spec = {type, 0, {0}, NULL, 0};
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, 0, (odin3_prov_id){0}, &net));
    odin3_netvec ports = {&net, 1};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, &ports, &node));
    set_str((odin3_objref){ODIN3_OBJ_NODE, node.v}, "grp", "add4");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, run("-attr grp=add4 -cell $lut -param WIDTH=2 -param LUT=1000"), log_text);
    TEST_ASSERT_EQUAL_STRING("unproven", text_attr("odin3:cone_check"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "x/z constant in the cone: proof is modulo x=0"));
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

/* --- the cases of v1.0.5 plan Task 1: -const -------------------------------------------------- */

static void load_eq13(void) {
    read_blif(EQ13_BLIF);
    mark_all();
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, i);
        bind(pair);
    }
    bind("y=Y[0]");
}

static void test_cone_check_const_errors(void) {
    load_eq13();
    shape before = shape_of();
    FAILS_WITH(EQ4 " -const C=1", "has no port 'C'");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const Y=1", "is not an input");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const B=1 -const B=2", "twice");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const B=16", "does not fit port 'B' (4 bits)");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const B=18446744073709551616", "does not fit port 'B' (4 bits)");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const B=1x", "not a constant");
    log_text[0] = '\0';
    FAILS_WITH(EQ4 " -const B=", "needs NAME=VALUE");
    TEST_ASSERT_NULL(text_attr("odin3:cone_check"));
    expect_same(before);
}

static void test_cone_check_const_net_bound_refused(void) {
    load_eq13();
    bind("a3=B[3]");
    FAILS_WITH(EQ4 " -const B=13", "port 'B' is constant");
}

static void test_cone_check_const_cell_text(void) {
    load_eq13();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(EQ4 " -const B=13 -timeout 0"), log_text);
    TEST_ASSERT_EQUAL_STRING("$eq A_WIDTH=4 B_WIDTH=4 Y_WIDTH=1 -const B=13",
                             text_attr("odin3:cone_cell"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(EQ4 " -const B=0b1101 -timeout 0"), log_text);
    TEST_ASSERT_EQUAL_STRING("$eq A_WIDTH=4 B_WIDTH=4 Y_WIDTH=1 -const B=0b1101",
                             text_attr("odin3:cone_cell"));
    cc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.cell = "$add";
    odin3_vec_init(&opts.params, sizeof(cc_param));
    odin3_vec_init(&opts.consts, sizeof(cc_const));
    cc_const *slot = odin3_vec_push(&opts.consts);
    TEST_ASSERT_NOT_NULL(slot);
    *slot = (cc_const){"B", "1"};
    slot = odin3_vec_push(&opts.consts);
    TEST_ASSERT_NOT_NULL(slot);
    *slot = (cc_const){"A", "2"};
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, cc_cell_text(&opts, &text));
    TEST_ASSERT_EQUAL_STRING("$add -const A=2 -const B=1", text.data);
    odin3_strbuf_free(&text);
    odin3_vec_free(&opts.params);
    odin3_vec_free(&opts.consts);
}

static void expect_bits(const char *text, uint32_t width, const uint8_t *want) {
    uint8_t bits[CONST_TEST_BITS];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, cc_const_bits(text, width, bits));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, bits, width);
}

static void test_cone_check_const_value_widths(void) {
    const uint8_t thirteen[4] = {1, 0, 1, 1};
    uint8_t bits[CONST_TEST_BITS];
    expect_bits("13", 4, thirteen);
    expect_bits("0b00001101", 4, thirteen);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, cc_const_bits("0b10000", 4, bits));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, cc_const_bits("18446744073709551615", CONST_TEST_BITS, bits));
    TEST_ASSERT_EQUAL_UINT8(1, bits[CONST_TEST_BITS - 1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          cc_const_bits("18446744073709551616", CONST_TEST_BITS, bits));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, cc_const_bits("0b", 4, bits));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, cc_const_bits("-1", 4, bits));
}

/* s = a + 1 mod 16 as half adders: s0 = !a0, s1 = a1 ^ a0, c2 = a1 & a0, ... */
#define INC4_BLIF                                                                                  \
    ".model m\n.inputs a0 a1 a2 a3\n.outputs s0 s1 s2 s3\n.names a0 s0\n0 1\n"                     \
    ".names a1 a0 s1\n01 1\n10 1\n.names a1 a0 c2\n11 1\n"                                         \
    ".names a2 c2 s2\n01 1\n10 1\n.names a2 c2 c3\n11 1\n"                                         \
    ".names a3 c3 s3\n01 1\n10 1\n.end\n"

#define INC_ARGS "-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=1 -param Y_WIDTH=4"

static void test_cone_check_const_increment_equivalent(void) {
    use_real_abc();
    read_blif(INC4_BLIF);
    mark_all();
    for (int i = 0; i < 4; i++) {
        char pair[PAIR_BYTES];
        (void)snprintf(pair, sizeof pair, "a%d=A[%d]", i, i);
        bind(pair);
        (void)snprintf(pair, sizeof pair, "s%d=Y[%d]", i, i);
        bind(pair);
    }
    shape before = shape_of();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(INC_ARGS " -const B=1"), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
    TEST_ASSERT_NULL(text_attr("odin3:cone_cex"));
    expect_same(before);
}

static void test_cone_check_const_eq13_equivalent(void) {
    use_real_abc();
    load_eq13();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(EQ4 " -const B=13"), log_text);
    TEST_ASSERT_EQUAL_STRING("equivalent", text_attr("odin3:cone_check"));
}

static void test_cone_check_const_eq13_differs(void) {
    use_real_abc();
    load_eq13();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(EQ4 " -const B=12"), log_text);
    TEST_ASSERT_EQUAL_STRING("differs", text_attr("odin3:cone_check"));
    const char *cex = text_attr("odin3:cone_cex");
    TEST_ASSERT_NOT_NULL(cex);
    TEST_ASSERT_NOT_NULL(strstr(cex, "A[0]="));
    TEST_ASSERT_NULL(strstr(cex, "B["));
}

static void test_cone_check_no_const_text_unchanged(void) {
    load_adder(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(ADD4 " -timeout 0"), log_text);
    TEST_ASSERT_EQUAL_STRING("$add A_WIDTH=4 B_WIDTH=4 Y_WIDTH=4", text_attr("odin3:cone_cell"));
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
    RUN_TEST(test_cone_check_hash_sees_rewiring);
    RUN_TEST(test_cone_check_hash_sees_key_value);
    RUN_TEST(test_cone_check_template_xz_refused);
    RUN_TEST(test_cone_check_constant_x_unproven);
    RUN_TEST(test_cone_check_usage_errors);
    RUN_TEST(test_cone_check_const_errors);
    RUN_TEST(test_cone_check_const_net_bound_refused);
    RUN_TEST(test_cone_check_const_cell_text);
    RUN_TEST(test_cone_check_const_value_widths);
    RUN_TEST(test_cone_check_no_const_text_unchanged);
    RUN_TEST(test_cone_check_const_increment_equivalent);
    RUN_TEST(test_cone_check_const_eq13_equivalent);
    RUN_TEST(test_cone_check_const_eq13_differs);
    return UNITY_END();
}
