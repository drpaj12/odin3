/*
 * test_ir_celltype.c — unit tests for the design, the cell-type registry and structural cells.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH8 = 8, NEG_WIDTH = -3, BUF_LEN = 16, PARAM_BITS = 3, FN_WIDTH = 5, MSG_MAX = 512 };

static odin3_design *design;
static size_t errors_logged;
static char last_error[MSG_MAX];

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_celltype_id find_type(odin3_design *des, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (odin3_strtab_find(odin3_design_strtab(des), odin3_bytes_cstr(name), &str)) {
        (void)odin3_celltype_find(des, str, &id);
    }
    return id;
}

/* Ports of the VTR hard adder: a, b, cin in; cout, sumout out; all width 1. */
static const odin3_port_def k_adder_ports[] = {
    {"a", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
    {"b", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
    {"cin", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
    {"cout", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL},
    {"sumout", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL},
};
static const odin3_celltype_def k_adder = {
    "adder", ODIN3_GRAN_HARD, 0, k_adder_ports, 5, NULL, 0, NULL, NULL};

static uint32_t width_fn_five(const odin3_value *params, uint32_t port) {
    (void)params;
    (void)port;
    return FN_WIDTH;
}

static void test_fresh_design_has_port_cells(void) {
    odin3_celltype_id in = find_type(design, "$port_in");
    TEST_ASSERT_TRUE(odin3_celltype_valid(in));
    const odin3_celltype_def *def = odin3_celltype_get(design, in);
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_EQUAL_STRING("$port_in", def->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_PORT, def->gran);
    TEST_ASSERT_EQUAL_UINT32(1, def->n_ports);
    TEST_ASSERT_EQUAL_STRING("P", def->ports[0].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[0].dir);
    TEST_ASSERT_EQUAL_STRING("WIDTH", def->ports[0].width_param);
    TEST_ASSERT_EQUAL_UINT32(1, def->n_params);
    TEST_ASSERT_EQUAL_STRING("WIDTH", def->params[0].name);
    TEST_ASSERT_EQUAL_INT(ODIN3_DIR_IN,
                          odin3_celltype_get(design, find_type(design, "$port_out"))->ports[0].dir);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_DIR_INOUT,
        odin3_celltype_get(design, find_type(design, "$port_inout"))->ports[0].dir);
}

static void test_fresh_design_has_const_cells(void) {
    static const char *const names[] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    static const odin3_const values[] = {ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_X,
                                         ODIN3_CONST_Z};
    for (int i = 0; i < 4; i++) {
        const odin3_celltype_def *def = odin3_celltype_get(design, find_type(design, names[i]));
        TEST_ASSERT_NOT_NULL(def);
        TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BIT, def->gran);
        TEST_ASSERT_EQUAL_UINT32(ODIN3_CT_ANYVIEW, def->flags); /* legal in every view (IR-9) */
        TEST_ASSERT_EQUAL_UINT32(1, def->n_ports);
        TEST_ASSERT_EQUAL_UINT32(0, def->n_params);
        TEST_ASSERT_EQUAL_STRING("Y", def->ports[0].name);
        TEST_ASSERT_EQUAL_INT(ODIN3_DIR_OUT, def->ports[0].dir);
        TEST_ASSERT_EQUAL_UINT32(
            1, odin3_celltype_port_width(design, find_type(design, names[i]), NULL, 0));
        TEST_ASSERT_NOT_NULL(def->const_value);
        TEST_ASSERT_EQUAL_INT(values[i], def->const_value(NULL));
    }
}

static void test_port_width_reads_param(void) {
    odin3_celltype_id in = find_type(design, "$port_in");
    odin3_value params[1] = {odin3_value_int(WIDTH8)};
    TEST_ASSERT_EQUAL_UINT32(WIDTH8, odin3_celltype_port_width(design, in, params, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, in, params, 1));
    params[0] = odin3_value_int(NEG_WIDTH);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, in, params, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, in, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, (odin3_celltype_id){0}, NULL, 0));
    TEST_ASSERT_TRUE(errors_logged > 0);
}

static void test_port_verify_rejects_bad_width(void) {
    const odin3_celltype_def *def = odin3_celltype_get(design, find_type(design, "$port_in"));
    odin3_value params[1] = {odin3_value_int(WIDTH8)};
    TEST_ASSERT_NOT_NULL(def->verify);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, def->verify(params));
    params[0] = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, def->verify(params));
}

static void test_port_width_fn(void) {
    static const odin3_port_def ports[] = {
        {"Q", ODIN3_DIR_OUT, false, 0, NULL, width_fn_five, NULL}};
    static const odin3_celltype_def def = {
        "test_fn_t2", ODIN3_GRAN_WORD, 0, ports, 1, NULL, 0, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&def));
    odin3_design *other = odin3_design_create();
    TEST_ASSERT_NOT_NULL(other);
    odin3_celltype_id id = find_type(other, "test_fn_t2");
    TEST_ASSERT_TRUE(odin3_celltype_valid(id));
    TEST_ASSERT_EQUAL_PTR(&def, odin3_celltype_get(other, id));
    TEST_ASSERT_EQUAL_UINT32(FN_WIDTH, odin3_celltype_port_width(other, id, NULL, 0));
    TEST_ASSERT_FALSE(odin3_celltype_valid(find_type(design, "test_fn_t2")));
    odin3_design_destroy(other);
}

static void test_register_global_rejects_bad_defs(void) {
    static const odin3_port_def dup_ports[] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                               {"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL}};
    static const odin3_port_def bad_param_port[] = {{"A", ODIN3_DIR_IN, false, 0, "W", NULL, NULL}};
    static const odin3_celltype_def dup_name = {
        "$port_in", ODIN3_GRAN_PORT, 0, NULL, 0, NULL, 0, NULL, NULL};
    static const odin3_celltype_def no_name = {NULL, ODIN3_GRAN_BIT, 0,   NULL, 0, NULL,
                                               0,    NULL,           NULL};
    static const odin3_celltype_def dup_port = {
        "test_dup_port_t2", ODIN3_GRAN_BIT, 0, dup_ports, 2, NULL, 0, NULL, NULL};
    static const odin3_celltype_def bad_param = {
        "test_bad_param_t2", ODIN3_GRAN_BIT, 0, bad_param_port, 1, NULL, 0, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&dup_name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&no_name));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&dup_port));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(&bad_param));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_register_global(NULL));
    TEST_ASSERT_EQUAL_size_t(5, errors_logged);
}

static void test_add_local_duplicate_name(void) {
    odin3_celltype_def def = {"$port_in", ODIN3_GRAN_MODULE, 0, NULL, 0, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &def, &id));
    def.name = "top";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    TEST_ASSERT_TRUE(odin3_celltype_valid(id));
    TEST_ASSERT_EQUAL_UINT32(id.v, find_type(design, "top").v);
    odin3_celltype_id again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &def, &again));
    TEST_ASSERT_EQUAL_size_t(2, errors_logged);
}

/* A definition default with a NULL payload of nonzero length is refused, not copied. */
static void test_add_local_rejects_null_payload(void) {
    odin3_param_def params[1] = {{"INIT", ODIN3_VAL_BITS, {ODIN3_VAL_BITS, 0, NULL, 3, 0, 0}}};
    odin3_celltype_def def = {"sub", ODIN3_GRAN_MODULE, 0, NULL, 0, params, 1, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_add_local(design, &def, &id));
    TEST_ASSERT_FALSE(odin3_celltype_valid(find_type(design, "sub")));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
}

static void test_add_local_deep_copies(void) {
    char name[BUF_LEN] = "sub";
    char port[BUF_LEN] = "D";
    char param[BUF_LEN] = "W";
    uint8_t bits[PARAM_BITS] = {ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_X};
    odin3_port_def ports[1] = {{port, ODIN3_DIR_IN, false, 0, param, NULL, NULL}};
    odin3_param_def params[2] = {{param, ODIN3_VAL_INT, odin3_value_int(WIDTH8)},
                                 {"INIT", ODIN3_VAL_BITS, odin3_value_int(0)}};
    params[1].dflt.kind = ODIN3_VAL_BITS;
    params[1].dflt.bits = bits;
    params[1].dflt.len = PARAM_BITS;
    odin3_celltype_def def = {name, ODIN3_GRAN_MODULE, 0, ports, 1, params, 2, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    odin3_value expect = params[1].dflt;
    uint8_t expect_bits[PARAM_BITS];
    memcpy(expect_bits, bits, sizeof bits);
    expect.bits = expect_bits;
    memset(name, 'x', 3);
    memset(port, 'x', 1);
    memset(param, 'x', 1);
    memset(bits, ODIN3_BIT_Z, sizeof bits);
    const odin3_celltype_def *got = odin3_celltype_get(design, id);
    TEST_ASSERT_TRUE(got != &def);
    TEST_ASSERT_EQUAL_STRING("sub", got->name);
    TEST_ASSERT_EQUAL_STRING("D", got->ports[0].name);
    TEST_ASSERT_EQUAL_STRING("W", got->ports[0].width_param);
    TEST_ASSERT_EQUAL_STRING("W", got->params[0].name);
    TEST_ASSERT_EQUAL_INT64(WIDTH8, got->params[0].dflt.i);
    TEST_ASSERT_TRUE(odin3_value_equal(&expect, &got->params[1].dflt));
    odin3_value vals[2] = {odin3_value_int(3), got->params[1].dflt};
    TEST_ASSERT_EQUAL_UINT32(3, odin3_celltype_port_width(design, id, vals, 0));
}

/* Review Focus 5: a black-box declaration of an already registered hard type (IR-7b). */
static void test_blackbox_reuses_registered_type(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_register_global(&k_adder));
    odin3_design *des = odin3_design_create();
    TEST_ASSERT_NOT_NULL(des);
    odin3_celltype_id hard = find_type(des, "adder");
    TEST_ASSERT_TRUE(odin3_celltype_valid(hard));
    odin3_port_def ports[5];
    memcpy(ports, k_adder_ports, sizeof ports);
    odin3_celltype_def decl = {"adder", ODIN3_GRAN_BLACKBOX, 0, ports, 5, NULL, 0, NULL, NULL};
    odin3_celltype_id got = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(des, &decl, &got));
    TEST_ASSERT_EQUAL_UINT32(hard.v, got.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD, odin3_celltype_get(des, got)->gran);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(des));
    TEST_ASSERT_EQUAL_UINT32(hard.v, odin3_design_declared_model(des, 0).v);
    ports[0].width = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_declare_blackbox(des, &decl, &got));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(des));
    ports[0].width = 1;
    ports[3].dir = ODIN3_DIR_IN;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_declare_blackbox(des, &decl, &got));
    ports[3].dir = ODIN3_DIR_OUT;
    decl.n_ports = 4;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_declare_blackbox(des, &decl, &got));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(des));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
    odin3_design_destroy(des);
}

static void test_blackbox_new_name(void) {
    odin3_celltype_def decl = {"mult_t2", ODIN3_GRAN_HARD, 0, k_adder_ports, 5, NULL, 0, NULL,
                               NULL};
    odin3_celltype_id first = {0};
    odin3_celltype_id second = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &decl, &first));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, odin3_celltype_get(design, first)->gran);
    TEST_ASSERT_EQUAL_UINT32(first.v, find_type(design, "mult_t2").v);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &decl, &second));
    TEST_ASSERT_EQUAL_UINT32(first.v, second.v);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_declared_model_count(design));
    TEST_ASSERT_EQUAL_UINT32(first.v, odin3_design_declared_model(design, 1).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model(design, 2).v);
}

/* A port cell type is never a black box, even when its parameters could fit the declaration. */
static void test_blackbox_port_type_never_matches(void) {
    odin3_celltype_def decl = {"$port_in", ODIN3_GRAN_BLACKBOX, 0, NULL, 0, NULL, 0, NULL, NULL};
    odin3_port_def ports[1] = {{"P", ODIN3_DIR_OUT, false, 1, NULL, NULL, NULL}};
    decl.ports = ports;
    decl.n_ports = 1;
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_declare_blackbox(design, &decl, &id));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(design));
}

/* --- parametric black boxes (IR-7b) ---------------------------------------------------------- */

enum { DECL_A = 10, DECL_B = 18, DFLT_AB = 36 };

/* out = A_WIDTH + B_WIDTH (0 when a parameter is not an INT). */
static uint32_t width_fn_sum(const odin3_value *params, uint32_t port) {
    (void)port;
    if (params[0].kind != ODIN3_VAL_INT || params[1].kind != ODIN3_VAL_INT) {
        return 0;
    }
    return (uint32_t)(params[0].i + params[1].i);
}

/* A local parametric multiplier: a A_WIDTH, b B_WIDTH, out A_WIDTH + B_WIDTH; STR is a string. */
static odin3_celltype_id add_pmul(const char *name) {
    static const odin3_param_def params[3] = {
        {"A_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, DFLT_AB, NULL, 0, 0, 0}},
        {"B_WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, DFLT_AB, NULL, 0, 0, 0}},
        {"STR", ODIN3_VAL_STRING, {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0}}};
    static const odin3_port_def ports[3] = {
        {"a", ODIN3_DIR_IN, false, 0, "A_WIDTH", NULL, NULL},
        {"b", ODIN3_DIR_IN, false, 0, "B_WIDTH", NULL, NULL},
        {"out", ODIN3_DIR_OUT, false, 0, NULL, width_fn_sum, NULL}};
    const odin3_celltype_def def = {name, ODIN3_GRAN_HARD, 0, ports, 3, params, 3, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    return id;
}

/* A declaration in another port order, with b given first and out scalar-free. */
static odin3_port_def g_decl_ports[3];
static odin3_celltype_def pmul_decl(const char *name, uint32_t out_width) {
    const odin3_port_def ports[3] = {{"b", ODIN3_DIR_IN, false, DECL_B, NULL, NULL, NULL},
                                     {"a", ODIN3_DIR_IN, false, DECL_A, NULL, NULL, NULL},
                                     {"out", ODIN3_DIR_OUT, false, out_width, NULL, NULL, NULL}};
    memcpy(g_decl_ports, ports, sizeof ports);
    const odin3_celltype_def decl = {name, ODIN3_GRAN_BLACKBOX, 0, g_decl_ports, 3, NULL, 0, NULL,
                                     NULL};
    return decl;
}

/* Review Focus 1 (IR side): parameters inferred from the declared widths, in any port order. */
static void test_blackbox_infers_parameters(void) {
    odin3_celltype_id type = add_pmul("pmul_t4");
    const odin3_celltype_def decl = pmul_decl("pmul_t4", DECL_A + DECL_B);
    odin3_celltype_id got = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_celltype_declare_blackbox(design, &decl, &got),
                                  last_error);
    TEST_ASSERT_EQUAL_UINT32(type.v, got.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    const odin3_value *params = odin3_design_declared_model_params(design, 0);
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL_INT64(DECL_A, params[0].i);
    TEST_ASSERT_EQUAL_INT64(DECL_B, params[1].i);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, params[2].kind);
    const odin3_celltype_def *kept = odin3_design_declared_model_decl(design, 0);
    TEST_ASSERT_NOT_NULL(kept);
    TEST_ASSERT_TRUE(kept != &decl);
    TEST_ASSERT_EQUAL_UINT32(3, kept->n_ports);
    TEST_ASSERT_EQUAL_STRING("b", kept->ports[0].name);
    TEST_ASSERT_EQUAL_UINT32(DECL_B, kept->ports[0].width);
    TEST_ASSERT_EQUAL_STRING("out", kept->ports[2].name);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

/* Review Focus 2 (IR side): a declared width the inferred parameters contradict is refused. */
static void test_blackbox_contradicting_width(void) {
    (void)add_pmul("pmul_bad_t4");
    const odin3_celltype_def decl = pmul_decl("pmul_bad_t4", DECL_A + DECL_B - 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_declare_blackbox(design, &decl, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(design));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
    TEST_ASSERT_EQUAL_STRING("declare_blackbox: 'pmul_bad_t4' does not match the registered cell "
                             "type: port 'out' has 27 bits, the cell type gives it 28",
                             last_error);
}

/* The quiet check reports the reason without logging; a missing port is named. */
static void test_blackbox_match_is_quiet(void) {
    odin3_celltype_id type = add_pmul("pmul_q_t4");
    odin3_celltype_def decl = pmul_decl("pmul_q_t4", DECL_A + DECL_B);
    odin3_value params[3];
    const odin3_blackbox_match match = {type, &decl, params};
    odin3_width_why why = {""};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_blackbox_match(design, &match, &why));
    TEST_ASSERT_EQUAL_INT64(DECL_B, params[1].i);
    g_decl_ports[1].name = "c";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_blackbox_match(design, &match, &why));
    TEST_ASSERT_EQUAL_STRING("the cell type has no port 'c'", why.text);
    g_decl_ports[1].name = "a";
    g_decl_ports[1].dir = ODIN3_DIR_OUT;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_blackbox_match(design, &match, &why));
    TEST_ASSERT_EQUAL_STRING("port 'a' has another direction", why.text);
    g_decl_ports[1].dir = ODIN3_DIR_IN;
    decl.n_ports = 2;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_blackbox_match(design, &match, &why));
    TEST_ASSERT_EQUAL_STRING("2 ports declared, the cell type has 3", why.text);
    TEST_ASSERT_EQUAL_size_t(0, errors_logged);
}

/* Inference: the largest width seen on a port sized by an INT parameter; others keep defaults. */
static void test_infer_params(void) {
    odin3_celltype_id type = add_pmul("pmul_i_t4");
    const uint32_t seen[3] = {DECL_A, 0, DECL_B};
    odin3_value params[3];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_infer_params(design, type, seen, params));
    TEST_ASSERT_EQUAL_INT64(DECL_A, params[0].i);
    TEST_ASSERT_EQUAL_INT64(DFLT_AB, params[1].i);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, params[2].kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_celltype_infer_params(
                                                     design, (odin3_celltype_id){0}, seen, params));
}

static void test_declared_model_accessors_out_of_range(void) {
    TEST_ASSERT_NULL(odin3_design_declared_model_params(design, 0));
    TEST_ASSERT_NULL(odin3_design_declared_model_decl(design, 0));
}

/* A new black box: no parameters, and the declaration is the new type's own definition. */
static void test_declared_model_of_a_new_black_box(void) {
    odin3_celltype_def decl = {"plain_t4", ODIN3_GRAN_BLACKBOX, 0, k_adder_ports, 5, NULL, 0, NULL,
                               NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &decl, &id));
    TEST_ASSERT_NULL(odin3_design_declared_model_params(design, 0));
    TEST_ASSERT_EQUAL_PTR(odin3_celltype_get(design, id),
                          odin3_design_declared_model_decl(design, 0));
    TEST_ASSERT_NULL(odin3_design_declared_model_decl(design, 1));
}

/* Declaring against a parametric type under every allocation failure: NO_MEMORY, list unchanged. */
static void test_blackbox_parametric_oom_sweep(void) {
    (void)add_pmul("pmul_oom_t4");
    const odin3_celltype_def decl = pmul_decl("pmul_oom_t4", DECL_A + DECL_B);
    for (long tries = 0;; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = odin3_celltype_declare_blackbox(design, &decl, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_design_declared_model_count(design));
    }
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
    TEST_ASSERT_EQUAL_INT64(DECL_A, odin3_design_declared_model_params(design, 0)[0].i);
}

static void test_instances_counter(void) {
    odin3_celltype_id id = find_type(design, "$_CONST0_");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, id));
    odin3_celltype_instances_inc(design, id);
    odin3_celltype_instances_inc(design, id);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_celltype_instances(design, id));
    odin3_celltype_instances_dec(design, id);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_instances(design, id));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, (odin3_celltype_id){0}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_instances(design, (odin3_celltype_id){UINT32_MAX}));
}

static void test_get_invalid_ids(void) {
    TEST_ASSERT_NULL(odin3_celltype_get(design, (odin3_celltype_id){0}));
    TEST_ASSERT_NULL(odin3_celltype_get(design, (odin3_celltype_id){UINT32_MAX}));
    odin3_celltype_id out = {0};
    TEST_ASSERT_FALSE(odin3_celltype_find(design, UINT32_MAX, &out));
}

/* A module type's definition is owned by its module and bound without a copy (IR-7). */
static void test_bind_local(void) {
    odin3_celltype_def def = {"mod_t2", ODIN3_GRAN_MODULE, 0, NULL, 0, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    static const odin3_port_def ports[1] = {{"clk", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL}};
    static const odin3_celltype_def bound = {
        "mod_t2", ODIN3_GRAN_MODULE, 0, ports, 1, NULL, 0, NULL, NULL};
    odin3_celltype_bind_local(design, id, &bound);
    TEST_ASSERT_EQUAL_PTR(&bound, odin3_celltype_get(design, id));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_celltype_port_width(design, id, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(id.v, find_type(design, "mod_t2").v);
}

static void test_design_intern(void) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("net_a"), &str));
    TEST_ASSERT_EQUAL_STRING("net_a", odin3_strtab_get(odin3_design_strtab(design), str));
}

/* Each failing create returns NULL (ASan's leak check covers the cleanup). */
static void test_design_create_oom_sweep(void) {
    odin3_design *des = NULL;
    long tries = 0;
    for (; des == NULL; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        des = odin3_design_create();
        odin3_util_set_alloc_fail_after(-1);
    }
    TEST_ASSERT_TRUE(tries > 3);
    TEST_ASSERT_TRUE(odin3_celltype_valid(find_type(des, "$_CONSTZ_")));
    odin3_design_destroy(des);
}

static void test_add_local_oom_sweep(void) {
    odin3_port_def ports[1] = {{"x", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL}};
    odin3_celltype_def def = {"oom_t2", ODIN3_GRAN_MODULE, 0, ports, 1, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    for (long tries = 0;; tries++) {
        odin3_util_set_alloc_fail_after(tries);
        odin3_status st = odin3_celltype_add_local(design, &def, &id);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_FALSE(odin3_celltype_valid(find_type(design, "oom_t2")));
    }
    TEST_ASSERT_EQUAL_UINT32(id.v, find_type(design, "oom_t2").v);
}

/* --- width expressions (fourth width rule) ------------------------------------------------- */

/* Fake compiled expression: the sum of every INT parameter; impl points at the failure switch. */
typedef struct fake_expr {
    bool fail_check;
    bool fail_eval;
} fake_expr;

static bool fake_check(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                       odin3_width_why *why) {
    const fake_expr *impl = wexpr->impl;
    (void)def;
    (void)snprintf(why->text, sizeof why->text, "fake: unknown identifier 'Q'");
    return !impl->fail_check;
}

static odin3_status fake_eval(const odin3_width_expr *wexpr, const odin3_width_args *args,
                              uint32_t *width) {
    const fake_expr *impl = wexpr->impl;
    if (impl->fail_eval) {
        (void)snprintf(args->why->text, sizeof args->why->text, "fake: too wide");
        return ODIN3_ERR_INVALID_ARG;
    }
    int64_t sum = 0;
    for (uint32_t i = 0; i < args->def->n_params; i++) {
        sum += args->params[i].i;
    }
    *width = (uint32_t)sum;
    return ODIN3_OK;
}

static odin3_celltype_id add_expr_type(const char *name, const odin3_width_expr *wexpr,
                                       odin3_status want) {
    const odin3_param_def params[2] = {{"A_WIDTH", ODIN3_VAL_INT, odin3_value_int(3)},
                                       {"B_WIDTH", ODIN3_VAL_INT, odin3_value_int(4)}};
    const odin3_port_def ports[2] = {{"a", ODIN3_DIR_IN, false, 0, "A_WIDTH", NULL, NULL},
                                     {"y", ODIN3_DIR_OUT, false, 0, NULL, NULL, wexpr}};
    const odin3_celltype_def def = {name, ODIN3_GRAN_HARD, 0, ports, 2, params, 2, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(want, odin3_celltype_add_local(design, &def, &id));
    return id;
}

static void test_width_expr_evaluates(void) {
    static const fake_expr impl = {false, false};
    static const odin3_width_expr wexpr = {fake_check, fake_eval, &impl};
    odin3_celltype_id id = add_expr_type("wexpr_t3", &wexpr, ODIN3_OK);
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    TEST_ASSERT_EQUAL_PTR(&wexpr, def->ports[1].width_expr); /* kept, not copied */
    const odin3_value params[2] = {odin3_value_int(36), odin3_value_int(18)};
    TEST_ASSERT_EQUAL_UINT32(54, odin3_celltype_port_width(design, id, params, 1));
    TEST_ASSERT_EQUAL_UINT32(36, odin3_celltype_port_width(design, id, params, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, id, NULL, 1));
}

static void test_width_expr_eval_failure(void) {
    static const fake_expr impl = {false, true};
    static const odin3_width_expr wexpr = {fake_check, fake_eval, &impl};
    odin3_celltype_id id = add_expr_type("wexpr_fail_t3", &wexpr, ODIN3_OK);
    const odin3_value params[2] = {odin3_value_int(1), odin3_value_int(1)};
    odin3_port_query query = {id, params, 1};
    uint32_t width = WIDTH8;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_port_width_checked(design, &query, &width));
    TEST_ASSERT_EQUAL_UINT32(WIDTH8, width);
    TEST_ASSERT_EQUAL_size_t(1, errors_logged); /* the hook does not log: one located line */
    TEST_ASSERT_EQUAL_STRING("port_width: port 'y' of cell type 'wexpr_fail_t3': fake: too wide",
                             last_error);
}

static void test_width_expr_validated_at_registration(void) {
    static const fake_expr bad = {true, false};
    static const odin3_width_expr failing = {fake_check, fake_eval, &bad};
    static const odin3_width_expr no_eval = {fake_check, NULL, &bad};
    static const odin3_width_expr no_check = {NULL, fake_eval, &bad};
    (void)add_expr_type("wexpr_bad_t3", &failing, ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_STRING("add_local: cell type 'wexpr_bad_t3': fake: unknown identifier 'Q'",
                             last_error);
    (void)add_expr_type("wexpr_bad_t3", &no_eval, ODIN3_ERR_INVALID_ARG);
    (void)add_expr_type("wexpr_bad_t3", &no_check, ODIN3_ERR_INVALID_ARG);
    TEST_ASSERT_FALSE(odin3_celltype_valid(find_type(design, "wexpr_bad_t3")));
    TEST_ASSERT_EQUAL_size_t(3, errors_logged);
}

static void test_blackbox_width_expr_evaluated(void) {
    static const fake_expr impl = {false, false};
    static const odin3_width_expr wexpr = {fake_check, fake_eval, &impl};
    (void)add_expr_type("wexpr_bb_t3", &wexpr, ODIN3_OK);
    odin3_port_def want[2] = {{"a", ODIN3_DIR_IN, false, 5, NULL, NULL, NULL},
                              {"y", ODIN3_DIR_OUT, false, 9, NULL, NULL, NULL}};
    odin3_celltype_def def = {"wexpr_bb_t3", ODIN3_GRAN_BLACKBOX, 0, want, 2, NULL, 0, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, NULL));
    TEST_ASSERT_EQUAL_INT64(5, odin3_design_declared_model_params(design, 0)[0].i);
    TEST_ASSERT_EQUAL_INT64(4, odin3_design_declared_model_params(design, 0)[1].i);
    want[1].width = 10;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_declare_blackbox(design, &def, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_design_declared_model_count(design));
}

static void test_lib_data_attaches_to_local_types(void) {
    odin3_celltype_def def = {"lib_t3", ODIN3_GRAN_HARD, 0, NULL, 0, NULL, 0, NULL, NULL};
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_add_local(design, &def, &id));
    TEST_ASSERT_NULL(odin3_celltype_lib(design, id));
    const odin3_techlib_cell *lib = odin3_arena_alloc(odin3_celltype_arena(design), BUF_LEN);
    TEST_ASSERT_NOT_NULL(lib);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_set_lib(design, id, lib));
    TEST_ASSERT_EQUAL_PTR(lib, odin3_celltype_lib(design, id));
}

static void test_lib_data_rejects_other_types(void) {
    const odin3_techlib_cell *lib = odin3_arena_alloc(odin3_celltype_arena(design), BUF_LEN);
    TEST_ASSERT_NOT_NULL(lib);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_set_lib(design, find_type(design, "$port_in"), lib));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_celltype_set_lib(design, (odin3_celltype_id){0}, lib));
    TEST_ASSERT_NULL(odin3_celltype_lib(design, find_type(design, "$port_in")));
    TEST_ASSERT_NULL(odin3_celltype_lib(design, (odin3_celltype_id){UINT32_MAX}));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_design_has_port_cells);
    RUN_TEST(test_fresh_design_has_const_cells);
    RUN_TEST(test_port_width_reads_param);
    RUN_TEST(test_port_verify_rejects_bad_width);
    RUN_TEST(test_port_width_fn);
    RUN_TEST(test_register_global_rejects_bad_defs);
    RUN_TEST(test_add_local_duplicate_name);
    RUN_TEST(test_add_local_rejects_null_payload);
    RUN_TEST(test_add_local_deep_copies);
    RUN_TEST(test_blackbox_reuses_registered_type);
    RUN_TEST(test_blackbox_new_name);
    RUN_TEST(test_blackbox_port_type_never_matches);
    RUN_TEST(test_blackbox_infers_parameters);
    RUN_TEST(test_blackbox_contradicting_width);
    RUN_TEST(test_blackbox_match_is_quiet);
    RUN_TEST(test_infer_params);
    RUN_TEST(test_declared_model_accessors_out_of_range);
    RUN_TEST(test_declared_model_of_a_new_black_box);
    RUN_TEST(test_blackbox_parametric_oom_sweep);
    RUN_TEST(test_instances_counter);
    RUN_TEST(test_get_invalid_ids);
    RUN_TEST(test_bind_local);
    RUN_TEST(test_design_intern);
    RUN_TEST(test_design_create_oom_sweep);
    RUN_TEST(test_add_local_oom_sweep);
    RUN_TEST(test_width_expr_evaluates);
    RUN_TEST(test_width_expr_eval_failure);
    RUN_TEST(test_width_expr_validated_at_registration);
    RUN_TEST(test_blackbox_width_expr_evaluated);
    RUN_TEST(test_lib_data_attaches_to_local_types);
    RUN_TEST(test_lib_data_rejects_other_types);
    return UNITY_END();
}
