/*
 * test_sim_word.c — word cells and tech-library hard cells through odin3_sim_build, against
 * reference models computed here on one-byte-per-bit arrays and a base-2^16 big integer.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MAXW = 200, LOG_TEXT = 4096, N_WIDTHS = 4, N_PATTERNS = 6, MAX_PORTS = 40 };

/* Every word cell is tested at each of these operand and result widths. */
static const uint32_t k_widths[N_WIDTHS] = {1, 7, 64, 65};

static odin3_design *design;
static char log_text[LOG_TEXT];
static uint32_t module_count;
static odin3_prng prng;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    module_count = 0;
    odin3_prng_seed(&prng, 7);
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

/* --- reference values: one byte per bit, LSB first ------------------------------------------- */

typedef struct bits {
    uint32_t width;
    uint8_t bit[MAXW];
} bits;

typedef struct operand {
    const bits *val;
    bool sgn;
} operand;

/* Two values of one width. */
typedef struct pair {
    bits lhs;
    bits rhs;
} pair;

static bits zeros(uint32_t width) {
    bits out;
    memset(&out, 0, sizeof out);
    out.width = width;
    return out;
}

/* A value and the width to give it. */
typedef struct sized {
    uint64_t value;
    uint32_t width;
} sized;

static bits from_u64(sized val) {
    bits out = zeros(val.width);
    for (uint32_t k = 0; k < val.width && k < 64; k++) {
        out.bit[k] = (uint8_t)((val.value >> k) & 1U);
    }
    return out;
}

/* The operand's value as `width` bits: sign-extended when signed, else zero-extended. */
static bits ext(operand op, uint32_t width) {
    bits out = zeros(width);
    uint8_t fill = op.sgn && op.val->width > 0 ? op.val->bit[op.val->width - 1] : 0;
    for (uint32_t k = 0; k < width; k++) {
        out.bit[k] = k < op.val->width ? op.val->bit[k] : fill;
    }
    return out;
}

static bits ref_add(const pair *pr) {
    bits out = zeros(pr->lhs.width);
    uint32_t carry = 0;
    for (uint32_t k = 0; k < out.width; k++) {
        uint32_t sum = pr->lhs.bit[k] + pr->rhs.bit[k] + carry;
        out.bit[k] = (uint8_t)(sum & 1U);
        carry = sum >> 1;
    }
    return out;
}

static bits ref_not(const bits *val) {
    bits out = *val;
    for (uint32_t k = 0; k < out.width; k++) {
        out.bit[k] ^= 1U;
    }
    return out;
}

static bits ref_sub(const pair *pr) {
    pair neg = {pr->lhs, ref_not(&pr->rhs)};
    neg.lhs = ref_add(&neg);
    neg.rhs = from_u64((sized){1, pr->lhs.width});
    return ref_add(&neg);
}

static bits ref_mul(const pair *pr) {
    uint32_t width = pr->lhs.width;
    pair acc = {zeros(width), zeros(width)};
    for (uint32_t i = 0; i < width; i++) {
        if (pr->rhs.bit[i] == 0) {
            continue;
        }
        acc.rhs = zeros(width);
        for (uint32_t k = i; k < width; k++) {
            acc.rhs.bit[k] = pr->lhs.bit[k - i];
        }
        acc.lhs = ref_add(&acc);
    }
    return acc.lhs;
}

static bits ref_and(const pair *pr) {
    bits out = pr->lhs;
    for (uint32_t k = 0; k < out.width; k++) {
        out.bit[k] = (uint8_t)(pr->lhs.bit[k] & pr->rhs.bit[k]);
    }
    return out;
}

static bits ref_or(const pair *pr) {
    bits out = pr->lhs;
    for (uint32_t k = 0; k < out.width; k++) {
        out.bit[k] = (uint8_t)(pr->lhs.bit[k] | pr->rhs.bit[k]);
    }
    return out;
}

static bits ref_xor(const pair *pr) {
    bits out = pr->lhs;
    for (uint32_t k = 0; k < out.width; k++) {
        out.bit[k] = (uint8_t)(pr->lhs.bit[k] ^ pr->rhs.bit[k]);
    }
    return out;
}

/* -1, 0 or 1: the two's-complement comparison of the pair (both as wide, MSB is the sign). */
static int ref_scmp(const pair *pr) {
    uint32_t top = pr->lhs.width - 1;
    if (pr->lhs.bit[top] != pr->rhs.bit[top]) {
        return pr->lhs.bit[top] != 0 ? -1 : 1;
    }
    for (uint32_t k = top + 1; k-- > 0;) {
        if (pr->lhs.bit[k] != pr->rhs.bit[k]) {
            return pr->lhs.bit[k] != 0 ? 1 : -1;
        }
    }
    return 0;
}

static int ref_ucmp(const pair *pr) {
    for (uint32_t k = pr->lhs.width; k-- > 0;) {
        if (pr->lhs.bit[k] != pr->rhs.bit[k]) {
            return pr->lhs.bit[k] != 0 ? 1 : -1;
        }
    }
    return 0;
}

static bool bits_equal(const bits *lhs, const bits *rhs) {
    return lhs->width == rhs->width && memcmp(lhs->bit, rhs->bit, lhs->width) == 0;
}

static void expect_bits(const bits *want, const bits *got, const char *what) {
    if (bits_equal(want, got)) {
        return;
    }
    char msg[2 * MAXW + 128];
    int len = snprintf(msg, sizeof msg, "%s: want ", what);
    for (uint32_t k = want->width; k-- > 0 && len < (int)sizeof msg - 1;) {
        msg[len++] = (char)('0' + want->bit[k]);
    }
    len += snprintf(msg + len, sizeof msg - (size_t)len, " got ");
    for (uint32_t k = got->width; k-- > 0 && len < (int)sizeof msg - 1;) {
        msg[len++] = (char)('0' + got->bit[k]);
    }
    msg[len < (int)sizeof msg ? len : (int)sizeof msg - 1] = '\0';
    TEST_FAIL_MESSAGE(msg);
}

/* Pattern number pat at a width: zero, all ones, MSB only, all but the MSB, then random. */
static bits pattern(sized pat_width) {
    uint64_t pat = pat_width.value;
    uint32_t width = pat_width.width;
    bits out = zeros(width);
    for (uint32_t k = 0; k < width; k++) {
        switch (pat) {
        case 0:
            break;
        case 1:
            out.bit[k] = 1;
            break;
        case 2:
            out.bit[k] = (uint8_t)(k + 1 == width);
            break;
        case 3:
            out.bit[k] = (uint8_t)(k + 1 != width);
            break;
        default:
            out.bit[k] = (uint8_t)(odin3_prng_bit(&prng) ? 1 : 0);
            break;
        }
    }
    return out;
}

/* --- IR construction and simulation ------------------------------------------------------------
 */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_celltype_id type_id(const char *name) {
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(name), &id), name);
    return id;
}

/* A fresh module with a unique name. */
static odin3_module *new_module(void) {
    char name[32];
    (void)snprintf(name, sizeof name, "m%u", module_count++);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern(name), (odin3_prov_id){0}, &mid));
    return odin3_module_get(design, mid);
}

/* A port's nets (LSB first). */
typedef struct bus {
    odin3_net_id net[MAXW];
    uint32_t width;
} bus;

static void add_bus(odin3_module *mod, const odin3_port_spec *spec, bus *out) {
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, spec, &node));
    odin3_pinslice pins = odin3_node_pins(mod, node);
    out->width = pins.count;
    for (uint32_t k = 0; k < pins.count; k++) {
        out->net[k] = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k});
    }
}

static void in_bus(odin3_module *mod, const char *name, uint32_t width, bus *out) {
    odin3_port_spec spec = {intern(name), ODIN3_DIR_IN, width, false, (odin3_prov_id){0}};
    add_bus(mod, &spec, out);
}

static void out_bus(odin3_module *mod, const char *name, uint32_t width, bus *out) {
    odin3_port_spec spec = {intern(name), ODIN3_DIR_OUT, width, false, (odin3_prov_id){0}};
    add_bus(mod, &spec, out);
}

/* A node of type with its ports on buses (one per port definition) and params (may be NULL). */
static void add_cell(odin3_module *mod, odin3_celltype_id type, const odin3_value *params,
                     const bus *const *buses) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    odin3_netvec ports[MAX_PORTS];
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_PORTS, def->n_ports);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        ports[i] = (odin3_netvec){buses[i]->net, buses[i]->width};
    }
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, params, params ? def->n_params : 0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node),
                                  log_text);
}

static odin3_sim *build_ok(const odin3_module *top) {
    odin3_sim *sim = NULL;
    odin3_status st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL(sim);
    return sim;
}

static void build_fails(const odin3_module *top, const char *needle) {
    odin3_sim *sim = (odin3_sim *)&sim; /* must be reset to NULL */
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_sim_build(design, odin3_module_id_of(top), &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
}

/* Ports of a one-cell module: names and widths in port order, the first n_in are inputs. */
typedef struct cell_ports {
    const char *const *names;
    const uint32_t *widths;
    uint32_t n_in;
    uint32_t n_ports;
} cell_ports;

/* Builds a module with those ports and one cell of type (params may be NULL) on them. */
static odin3_sim *build_cell(const char *type, const odin3_value *params, const cell_ports *cp) {
    odin3_module *top = new_module();
    bus port[MAX_PORTS];
    const bus *buses[MAX_PORTS];
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_PORTS, cp->n_ports);
    for (uint32_t i = 0; i < cp->n_ports; i++) {
        if (i < cp->n_in) {
            in_bus(top, cp->names[i], cp->widths[i], &port[i]);
        } else {
            out_bus(top, cp->names[i], cp->widths[i], &port[i]);
        }
        buses[i] = &port[i];
    }
    add_cell(top, type_id(type), params, buses);
    return build_ok(top);
}

static void set_port(odin3_sim *sim, uint32_t port, const bits *val) {
    TEST_ASSERT_EQUAL_UINT32(val->width, odin3_sim_input_width(sim, port));
    for (uint32_t k = 0; k < val->width; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_sim_set_input(sim, (odin3_sim_bit){port, k}, val->bit[k] != 0));
    }
}

static bits get_port(const odin3_sim *sim, uint32_t port) {
    bits out = zeros(odin3_sim_output_width(sim, port));
    for (uint32_t k = 0; k < out.width; k++) {
        bool bit = false;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_get_output(sim, (odin3_sim_bit){port, k}, &bit));
        out.bit[k] = (uint8_t)(bit ? 1 : 0);
    }
    return out;
}

/* --- binary word cells: A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH; ports A B Y -----------------
 */

typedef bits (*binary_ref)(const odin3_value *params, const pair *ab);

/* Widths of one binary test configuration. */
typedef struct binary_widths {
    uint32_t aw, bw, yw;
} binary_widths;

static const int64_t k_signs[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

/* An arithmetic or bitwise result: both operands extended to Y_WIDTH, signed only if both are. */
/* Yosys simlib: a binary operation is signed only when both A_SIGNED and B_SIGNED are set. */
static bool both_signed(const odin3_value *params) {
    return params[0].i != 0 && params[1].i != 0;
}

static pair at_y(const odin3_value *params, const pair *ab) {
    uint32_t yw = (uint32_t)params[4].i;
    bool sgn = both_signed(params);
    pair out = {ext((operand){&ab->lhs, sgn}, yw), ext((operand){&ab->rhs, sgn}, yw)};
    return out;
}

static bits bin_add(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_add(&op);
}
static bits bin_sub(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_sub(&op);
}
static bits bin_mul(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_mul(&op);
}
static bits bin_and(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_and(&op);
}
static bits bin_or(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_or(&op);
}
static bits bin_xor(const odin3_value *params, const pair *ab) {
    pair op = at_y(params, ab);
    return ref_xor(&op);
}

/* The comparison of A and B: two's complement only when both are signed, else unsigned. */
static int bin_cmp(const odin3_value *params, const pair *ab) {
    uint32_t width = (ab->lhs.width > ab->rhs.width ? ab->lhs.width : ab->rhs.width) + 1;
    bool sgn = both_signed(params);
    pair op = {ext((operand){&ab->lhs, sgn}, width), ext((operand){&ab->rhs, sgn}, width)};
    return sgn ? ref_scmp(&op) : ref_ucmp(&op);
}

static bits flag(const odin3_value *params, bool value) {
    return from_u64((sized){value ? 1 : 0, (uint32_t)params[4].i});
}

static bits bin_eq(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) == 0);
}
static bits bin_ne(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) != 0);
}
static bits bin_lt(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) < 0);
}
static bits bin_le(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) <= 0);
}
static bits bin_gt(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) > 0);
}
static bits bin_ge(const odin3_value *params, const pair *ab) {
    return flag(params, bin_cmp(params, ab) >= 0);
}

/*
 * One module per width triple: inputs A, B and four cells of type, one per signedness
 * combination, driving outputs Y0..Y3. Every pattern pair is driven and each output compared.
 */
static void check_binary_widths(const char *type, binary_ref ref, binary_widths wid) {
    odin3_module *top = new_module();
    bus a_bus;
    bus b_bus;
    bus y_bus[4];
    in_bus(top, "A", wid.aw, &a_bus);
    in_bus(top, "B", wid.bw, &b_bus);
    odin3_value params[4][5];
    for (uint32_t sg = 0; sg < 4; sg++) {
        char name[8];
        (void)snprintf(name, sizeof name, "Y%u", sg);
        out_bus(top, name, wid.yw, &y_bus[sg]);
        const odin3_value vals[5] = {odin3_value_int(k_signs[sg][0]),
                                     odin3_value_int(k_signs[sg][1]), odin3_value_int(wid.aw),
                                     odin3_value_int(wid.bw), odin3_value_int(wid.yw)};
        memcpy(params[sg], vals, sizeof vals);
        const bus *const buses[3] = {&a_bus, &b_bus, &y_bus[sg]};
        add_cell(top, type_id(type), params[sg], buses);
    }
    odin3_sim *sim = build_ok(top);
    for (uint32_t pa = 0; pa < N_PATTERNS; pa++) {
        for (uint32_t pb = 0; pb < N_PATTERNS; pb++) {
            pair ab = {pattern((sized){pa, wid.aw}), pattern((sized){pb, wid.bw})};
            set_port(sim, 0, &ab.lhs);
            set_port(sim, 1, &ab.rhs);
            odin3_sim_cycle(sim);
            for (uint32_t sg = 0; sg < 4; sg++) {
                char what[96];
                (void)snprintf(what, sizeof what, "%s A%u%s B%u%s Y%u pat %u,%u", type, wid.aw,
                               k_signs[sg][0] ? "s" : "u", wid.bw, k_signs[sg][1] ? "s" : "u",
                               wid.yw, pa, pb);
                bits want = ref(params[sg], &ab);
                bits got = get_port(sim, sg);
                expect_bits(&want, &got, what);
            }
        }
    }
    odin3_sim_destroy(sim);
}

static void check_binary(const char *type, binary_ref ref) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        for (uint32_t j = 0; j < N_WIDTHS; j++) {
            for (uint32_t k = 0; k < N_WIDTHS; k++) {
                check_binary_widths(type, ref,
                                    (binary_widths){k_widths[i], k_widths[j], k_widths[k]});
            }
        }
    }
}

static void test_add(void) {
    check_binary("$add", bin_add);
}
static void test_sub(void) {
    check_binary("$sub", bin_sub);
}
static void test_mul(void) {
    check_binary("$mul", bin_mul);
}
static void test_and(void) {
    check_binary("$and", bin_and);
}
static void test_or(void) {
    check_binary("$or", bin_or);
}
static void test_xor(void) {
    check_binary("$xor", bin_xor);
}
static void test_eq(void) {
    check_binary("$eq", bin_eq);
}
static void test_ne(void) {
    check_binary("$ne", bin_ne);
}
static void test_lt(void) {
    check_binary("$lt", bin_lt);
}
static void test_le(void) {
    check_binary("$le", bin_le);
}
static void test_gt(void) {
    check_binary("$gt", bin_gt);
}
static void test_ge(void) {
    check_binary("$ge", bin_ge);
}

/* --- unary word cells: A_SIGNED A_WIDTH Y_WIDTH; ports A Y --------------------------------------
 */

typedef bits (*unary_ref)(const odin3_value *params, const bits *val);

static bits un_not(const odin3_value *params, const bits *val) {
    bits wide = ext((operand){val, params[0].i != 0}, (uint32_t)params[2].i);
    return ref_not(&wide);
}

/* A reduction: and (all ones), or (any one) or xor (odd ones) of A, zero-extended to Y_WIDTH. */
static bits reduce(const odin3_value *params, const bits *val, char kind) {
    uint32_t ones = 0;
    for (uint32_t k = 0; k < val->width; k++) {
        ones += val->bit[k];
    }
    bool res = (ones & 1U) != 0;
    if (kind == '&') {
        res = ones == val->width;
    } else if (kind == '|') {
        res = ones > 0;
    }
    return from_u64((sized){res ? 1 : 0, (uint32_t)params[2].i});
}

static bits un_rand(const odin3_value *params, const bits *val) {
    return reduce(params, val, '&');
}
static bits un_ror(const odin3_value *params, const bits *val) {
    return reduce(params, val, '|');
}
static bits un_rxor(const odin3_value *params, const bits *val) {
    return reduce(params, val, '^');
}

/* One module per width pair: input A, outputs Y0 (A unsigned) and Y1 (A signed). */
static void check_unary_widths(const char *type, unary_ref ref, binary_widths wid) {
    odin3_module *top = new_module();
    bus a_bus;
    bus y_bus[2];
    in_bus(top, "A", wid.aw, &a_bus);
    odin3_value params[2][3];
    for (uint32_t sg = 0; sg < 2; sg++) {
        out_bus(top, sg == 0 ? "Y0" : "Y1", wid.yw, &y_bus[sg]);
        const odin3_value vals[3] = {odin3_value_int(sg), odin3_value_int(wid.aw),
                                     odin3_value_int(wid.yw)};
        memcpy(params[sg], vals, sizeof vals);
        const bus *const buses[2] = {&a_bus, &y_bus[sg]};
        add_cell(top, type_id(type), params[sg], buses);
    }
    odin3_sim *sim = build_ok(top);
    for (uint32_t pa = 0; pa < N_PATTERNS * 2; pa++) {
        bits val = pattern((sized){pa, wid.aw});
        set_port(sim, 0, &val);
        odin3_sim_cycle(sim);
        for (uint32_t sg = 0; sg < 2; sg++) {
            char what[64];
            (void)snprintf(what, sizeof what, "%s A%u%s Y%u pat %u", type, wid.aw, sg ? "s" : "u",
                           wid.yw, pa);
            bits want = ref(params[sg], &val);
            bits got = get_port(sim, sg);
            expect_bits(&want, &got, what);
        }
    }
    odin3_sim_destroy(sim);
}

static void check_unary(const char *type, unary_ref ref) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        for (uint32_t k = 0; k < N_WIDTHS; k++) {
            check_unary_widths(type, ref, (binary_widths){k_widths[i], 0, k_widths[k]});
        }
    }
}

static void test_not(void) {
    check_unary("$not", un_not);
}
static void test_reduce_and(void) {
    check_unary("$reduce_and", un_rand);
}
static void test_reduce_or(void) {
    check_unary("$reduce_or", un_ror);
}
static void test_reduce_xor(void) {
    check_unary("$reduce_xor", un_rxor);
}

/* --- known answers (anchor the reference models) -----------------------------------------------
 */

/* A single binary cell with the given parameters on inputs a, b; returns Y. */
typedef struct one_binary {
    const char *type;
    int64_t a_signed, b_signed;
    bits a_val, b_val;
    uint32_t yw;
} one_binary;

static bits run_one_binary(const one_binary *spec) {
    odin3_module *top = new_module();
    bus a_bus;
    bus b_bus;
    bus y_bus;
    in_bus(top, "A", spec->a_val.width, &a_bus);
    in_bus(top, "B", spec->b_val.width, &b_bus);
    out_bus(top, "Y", spec->yw, &y_bus);
    const odin3_value params[5] = {odin3_value_int(spec->a_signed), odin3_value_int(spec->b_signed),
                                   odin3_value_int(spec->a_val.width),
                                   odin3_value_int(spec->b_val.width), odin3_value_int(spec->yw)};
    const bus *const buses[3] = {&a_bus, &b_bus, &y_bus};
    add_cell(top, type_id(spec->type), params, buses);
    odin3_sim *sim = build_ok(top);
    set_port(sim, 0, &spec->a_val);
    set_port(sim, 1, &spec->b_val);
    odin3_sim_cycle(sim);
    bits out = get_port(sim, 0);
    odin3_sim_destroy(sim);
    return out;
}

static uint64_t low_u64(const bits *val) {
    uint64_t out = 0;
    for (uint32_t k = 0; k < val->width && k < 64; k++) {
        out |= (uint64_t)val->bit[k] << k;
    }
    return out;
}

static void test_known_answers(void) {
    /* signed 7-bit -1 + -1 = -2, sign-extended into 65 bits */
    one_binary add = {"$add", 1, 1, from_u64((sized){0x7f, 7}), from_u64((sized){0x7f, 7}), 65};
    bits sum = run_one_binary(&add);
    TEST_ASSERT_EQUAL_HEX64(0xfffffffffffffffeULL, low_u64(&sum));
    TEST_ASSERT_EQUAL_UINT8(1, sum.bit[64]);
    /* unsigned: 127 + 127 = 254 */
    add.a_signed = 0;
    add.b_signed = 0;
    sum = run_one_binary(&add);
    TEST_ASSERT_EQUAL_HEX64(254, low_u64(&sum));
    TEST_ASSERT_EQUAL_UINT8(0, sum.bit[64]);
    /* (2^64-1)^2 = 2^128 - 2^65 + 1, which is 1 mod 2^65 */
    one_binary mul = {
        "$mul", 0, 0, from_u64((sized){UINT64_MAX, 64}), from_u64((sized){UINT64_MAX, 64}), 65};
    bits prod = run_one_binary(&mul);
    TEST_ASSERT_EQUAL_HEX64(1, low_u64(&prod));
    TEST_ASSERT_EQUAL_UINT8(0, prod.bit[64]);
    /* 64-bit carry into bit 64 */
    one_binary carry = {"$add", 0, 0, from_u64((sized){UINT64_MAX, 64}), from_u64((sized){1, 1}),
                        65};
    bits wide = run_one_binary(&carry);
    TEST_ASSERT_EQUAL_HEX64(0, low_u64(&wide));
    TEST_ASSERT_EQUAL_UINT8(1, wide.bit[64]);
    /* Mixed signedness is unsigned (simlib): 1'b1 < 1'b1 is 0, not -1 < 1 */
    one_binary lt = {"$lt", 1, 0, from_u64((sized){1, 1}), from_u64((sized){1, 1}), 1};
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&lt)}));
    lt.type = "$ge";
    TEST_ASSERT_EQUAL_HEX64(1, low_u64((bits[]){run_one_binary(&lt)}));
    /* 1'b1 == 2'b01 with A_SIGNED only: both zero-extended, equal */
    one_binary eq = {"$eq", 1, 0, from_u64((sized){1, 1}), from_u64((sized){1, 2}), 1};
    TEST_ASSERT_EQUAL_HEX64(1, low_u64((bits[]){run_one_binary(&eq)}));
    /* both signed: 1'sb1 (-1) != 2'sb01 (1) */
    eq.b_signed = 1;
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&eq)}));
    /* 4'hF + 4'h1 with A_SIGNED only, Y 8 bits: zero-extended, 8'h10 (not 8'h00) */
    one_binary madd = {"$add", 1, 0, from_u64((sized){0xf, 4}), from_u64((sized){1, 4}), 8};
    TEST_ASSERT_EQUAL_HEX64(0x10, low_u64((bits[]){run_one_binary(&madd)}));
    /* 4'b1000 & 8'hF0 with A_SIGNED only: 8'h00 (sign extension would give 8'hF0) */
    one_binary mand = {"$and", 1, 0, from_u64((sized){0x8, 4}), from_u64((sized){0xf0, 8}), 8};
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&mand)}));
    mand.b_signed = 1; /* both signed: 8'hF8 & 8'hF0 */
    TEST_ASSERT_EQUAL_HEX64(0xf0, low_u64((bits[]){run_one_binary(&mand)}));
    /* -1 (7'b1111111 signed) == 65 ones signed */
    bits ones65 = pattern((sized){1, 65});
    one_binary eqs = {"$eq", 1, 1, from_u64((sized){0x7f, 7}), ones65, 1};
    TEST_ASSERT_EQUAL_HEX64(1, low_u64((bits[]){run_one_binary(&eqs)}));
    /* the same with B unsigned: unsigned 127 versus 2^65-1 */
    eqs.b_signed = 0;
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&eqs)}));
}

/* --- cross-check against Yosys simlib.v run in Icarus (tools/sim-check/gen_simlib_cases.py) --- */

/* One generated case: the cell, its parameters, and A, B and the expected Y in hex. */
typedef struct simlib_case {
    const char *type;
    int64_t a_signed, b_signed;
    uint32_t aw, bw, yw;
    const char *a_hex, *b_hex, *y_hex;
} simlib_case;

#include "simlib_word_cases.h"

/* The low width bits of a hexadecimal number. */
static bits from_hex(const char *hex, uint32_t width) {
    bits out = zeros(width);
    size_t len = strlen(hex);
    for (size_t i = 0; i < len; i++) {
        char chr = hex[len - 1 - i];
        uint32_t digit = chr <= '9' ? (uint32_t)(chr - '0') : (uint32_t)(chr - 'a' + 10);
        for (uint32_t k = 0; k < 4 && i * 4 + k < width; k++) {
            out.bit[i * 4 + k] = (uint8_t)((digit >> k) & 1U);
        }
    }
    return out;
}

/* Runs one $not cell (A_SIGNED, A, Y_WIDTH as in the case) and returns Y. */
static bits run_one_unary(const simlib_case *cse) {
    odin3_module *top = new_module();
    bus a_bus;
    bus y_bus;
    in_bus(top, "A", cse->aw, &a_bus);
    out_bus(top, "Y", cse->yw, &y_bus);
    const odin3_value params[3] = {odin3_value_int(cse->a_signed), odin3_value_int(cse->aw),
                                   odin3_value_int(cse->yw)};
    const bus *const buses[2] = {&a_bus, &y_bus};
    add_cell(top, type_id(cse->type), params, buses);
    odin3_sim *sim = build_ok(top);
    bits a_val = from_hex(cse->a_hex, cse->aw);
    set_port(sim, 0, &a_val);
    odin3_sim_cycle(sim);
    bits out = get_port(sim, 0);
    odin3_sim_destroy(sim);
    return out;
}

/* Every generated case, mixed signedness included, matches simlib bit for bit. */
static void test_matches_simlib(void) {
    for (uint32_t i = 0; i < sizeof k_simlib_cases / sizeof k_simlib_cases[0]; i++) {
        const simlib_case *cse = &k_simlib_cases[i];
        bits got;
        if (strcmp(cse->type, "$not") == 0) {
            got = run_one_unary(cse);
        } else {
            const one_binary spec = {cse->type,
                                     cse->a_signed,
                                     cse->b_signed,
                                     from_hex(cse->a_hex, cse->aw),
                                     from_hex(cse->b_hex, cse->bw),
                                     cse->yw};
            got = run_one_binary(&spec);
        }
        bits want = from_hex(cse->y_hex, cse->yw);
        char what[160];
        (void)snprintf(what, sizeof what, "simlib case %u: %s A%u%s=%s B%u%s=%s Y%u", i, cse->type,
                       cse->aw, cse->a_signed ? "s" : "u", cse->a_hex, cse->bw,
                       cse->b_signed ? "s" : "u", cse->b_hex, cse->yw);
        expect_bits(&want, &got, what);
    }
}

/* --- $mux and $pmux -----------------------------------------------------------------------------
 */

static void test_mux(void) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        uint32_t width = k_widths[i];
        odin3_module *top = new_module();
        bus a_bus;
        bus b_bus;
        bus s_bus;
        bus y_bus;
        in_bus(top, "A", width, &a_bus);
        in_bus(top, "B", width, &b_bus);
        in_bus(top, "S", 1, &s_bus);
        out_bus(top, "Y", width, &y_bus);
        const odin3_value params[1] = {odin3_value_int(width)};
        const bus *const buses[4] = {&a_bus, &b_bus, &s_bus, &y_bus};
        add_cell(top, type_id("$mux"), params, buses);
        odin3_sim *sim = build_ok(top);
        for (uint32_t pat = 0; pat < N_PATTERNS * 2; pat++) {
            bits a_val = pattern((sized){pat, width});
            bits b_val = pattern((sized){N_PATTERNS - 1 - pat % N_PATTERNS, width});
            bits s_val = from_u64((sized){pat & 1U, 1});
            set_port(sim, 0, &a_val);
            set_port(sim, 1, &b_val);
            set_port(sim, 2, &s_val);
            odin3_sim_cycle(sim);
            bits got = get_port(sim, 0);
            expect_bits((pat & 1U) != 0 ? &b_val : &a_val, &got, "$mux");
        }
        odin3_sim_destroy(sim);
    }
}

/* Y = A when no select bit is set, else the OR of the B slices whose select bit is set. */
static bits ref_pmux(const bits *a_val, const bits *b_val, uint32_t sel) {
    if (sel == 0) {
        return *a_val;
    }
    bits out = zeros(a_val->width);
    for (uint32_t j = 0; (sel >> j) != 0; j++) {
        for (uint32_t k = 0; ((sel >> j) & 1U) != 0 && k < out.width; k++) {
            out.bit[k] |= b_val->bit[j * out.width + k];
        }
    }
    return out;
}

static void check_pmux(uint32_t width, uint32_t s_width) {
    odin3_module *top = new_module();
    bus a_bus;
    bus b_bus;
    bus s_bus;
    bus y_bus;
    in_bus(top, "A", width, &a_bus);
    in_bus(top, "B", width * s_width, &b_bus);
    in_bus(top, "S", s_width, &s_bus);
    out_bus(top, "Y", width, &y_bus);
    const odin3_value params[2] = {odin3_value_int(width), odin3_value_int(s_width)};
    const bus *const buses[4] = {&a_bus, &b_bus, &s_bus, &y_bus};
    add_cell(top, type_id("$pmux"), params, buses);
    odin3_sim *sim = build_ok(top);
    for (uint32_t sel = 0; sel < (1U << s_width); sel++) {
        for (uint32_t pat = 0; pat < N_PATTERNS; pat++) {
            bits a_val = pattern((sized){pat, width});
            bits b_val = pattern((sized){N_PATTERNS - 1, width * s_width});
            bits s_val = from_u64((sized){sel, s_width});
            set_port(sim, 0, &a_val);
            set_port(sim, 1, &b_val);
            set_port(sim, 2, &s_val);
            odin3_sim_cycle(sim);
            bits want = ref_pmux(&a_val, &b_val, sel);
            bits got = get_port(sim, 0);
            expect_bits(&want, &got, "$pmux");
        }
    }
    odin3_sim_destroy(sim);
}

static void test_pmux(void) {
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        check_pmux(k_widths[i], 1);
        check_pmux(k_widths[i], 3);
    }
}

/* --- coverage of the registry --------------------------------------------------------------------
 */

static void test_word_cells_have_hooks(void) {
    static const char *const k_with[] = {
        "$and", "$or", "$xor", "$not", "$add",        "$sub",       "$mul",        "$eq",  "$ne",
        "$lt",  "$le", "$gt",  "$ge",  "$reduce_and", "$reduce_or", "$reduce_xor", "$mux", "$pmux"};
    for (uint32_t i = 0; i < sizeof k_with / sizeof k_with[0]; i++) {
        const odin3_celltype_def *def = odin3_celltype_get(design, type_id(k_with[i]));
        TEST_ASSERT_NOT_NULL_MESSAGE(def->simulate, k_with[i]);
    }
    /* still out of scope (1E spec): division, shifts */
    static const char *const k_without[] = {"$div", "$mod", "$shl", "$shr", "$sshr"};
    for (uint32_t i = 0; i < sizeof k_without / sizeof k_without[0]; i++) {
        const odin3_celltype_def *def = odin3_celltype_get(design, type_id(k_without[i]));
        TEST_ASSERT_NULL_MESSAGE(def->simulate, k_without[i]);
    }
}

/* A word cell without a hook is still rejected by the build. */
static void test_div_is_rejected(void) {
    odin3_module *top = new_module();
    bus a_bus;
    bus y_bus;
    in_bus(top, "A", 7, &a_bus);
    out_bus(top, "Y", 7, &y_bus);
    const odin3_value params[5] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(7),
                                   odin3_value_int(7), odin3_value_int(7)};
    const bus *const buses[3] = {&a_bus, &a_bus, &y_bus};
    add_cell(top, type_id("$div"), params, buses);
    build_fails(top, "cannot simulate `$div`");
}

static const char *const k_abyn[3] = {"A", "B", "Y"};

/* Hooks never allocate: with the next allocation set to fail, cycles still compute right. */
static void test_word_hooks_do_not_allocate(void) {
    one_binary mul = {"$mul", 1, 1, pattern((sized){4, 65}), pattern((sized){5, 65}), 65};
    static const uint32_t k_w65[3] = {65, 65, 65};
    const cell_ports cp = {k_abyn, k_w65, 2, 3};
    const odin3_value params[5] = {odin3_value_int(1), odin3_value_int(1), odin3_value_int(65),
                                   odin3_value_int(65), odin3_value_int(65)};
    odin3_sim *sim = build_cell("$mul", params, &cp);
    TEST_ASSERT_GREATER_THAN_UINT32(0, sim->n_scratch);
    set_port(sim, 0, &mul.a_val);
    set_port(sim, 1, &mul.b_val);
    odin3_util_set_alloc_fail_after(0);
    odin3_sim_cycle(sim);
    void *probe = odin3_util_malloc(1); /* still armed: nothing allocated during the cycle */
    TEST_ASSERT_NULL(probe);
    pair ab = {mul.a_val, mul.b_val};
    bits want = bin_mul(params, &ab);
    bits got = get_port(sim, 0);
    expect_bits(&want, &got, "$mul 65 under failing allocation");
    odin3_sim_destroy(sim);
}

/* --- tech-library hard cells: the fn interpreter ------------------------------------------------
 */

/* adder and multiply exactly as the 1G spec declares them (lib/vtr.o3lib is not written yet). */
static const char k_vtr_lib[] = "library vtr_inline\n"
                                "cell adder hard\n"
                                "  in  a 1\n"
                                "  in  b 1\n"
                                "  in  cin 1\n"
                                "  out cout 1\n"
                                "  out sumout 1\n"
                                "  fn  sumout = a ^ b ^ cin\n"
                                "  fn  cout   = (a & b) | (a & cin) | (b & cin)\n"
                                "end\n"
                                "cell multiply hard\n"
                                "  param A_WIDTH int 36\n"
                                "  param B_WIDTH int 36\n"
                                "  in  a A_WIDTH\n"
                                "  in  b B_WIDTH\n"
                                "  out out A_WIDTH + B_WIDTH\n"
                                "  fn  out = a * b\n"
                                "end\n";

static void read_lib(const char *text) {
    const odin3_techlib_text src = {"inline.o3lib", odin3_bytes_cstr(text)};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read_text(design, &src), log_text);
}

static void test_adder(void) {
    read_lib(k_vtr_lib);
    odin3_module *top = new_module();
    bus port[5];
    in_bus(top, "a", 1, &port[0]);
    in_bus(top, "b", 1, &port[1]);
    in_bus(top, "cin", 1, &port[2]);
    out_bus(top, "cout", 1, &port[3]);
    out_bus(top, "sumout", 1, &port[4]);
    const bus *const buses[5] = {&port[0], &port[1], &port[2], &port[3], &port[4]};
    add_cell(top, type_id("adder"), NULL, buses);
    odin3_sim *sim = build_ok(top);
    for (uint32_t combo = 0; combo < 8; combo++) {
        uint32_t ones = 0;
        for (uint32_t k = 0; k < 3; k++) {
            bits in = from_u64((sized){(combo >> k) & 1U, 1});
            set_port(sim, k, &in);
            ones += in.bit[0];
        }
        odin3_sim_cycle(sim);
        bits cout = get_port(sim, 0);
        bits sum = get_port(sim, 1);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ones >= 2 ? 1 : 0, cout.bit[0], "cout");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ones & 1U, sum.bit[0], "sumout");
    }
    odin3_sim_destroy(sim);
}

/* A non-negative big integer in base 2^16 (independent of the simulator's 64-bit limbs). */
enum { BIG_DIGITS = 16, DIGIT_BITS = 16 };

typedef struct big {
    uint32_t digit[BIG_DIGITS];
} big;

typedef struct big_pair {
    big lhs;
    big rhs;
} big_pair;

static big big_of(const bits *val) {
    big out;
    memset(&out, 0, sizeof out);
    for (uint32_t k = 0; k < val->width; k++) {
        out.digit[k / DIGIT_BITS] |= (uint32_t)val->bit[k] << (k % DIGIT_BITS);
    }
    return out;
}

/* Schoolbook product, digit by digit with a running carry (exact while it fits 16 digits). */
static big big_mul(const big_pair *pr) {
    big out;
    memset(&out, 0, sizeof out);
    for (uint32_t i = 0; i < BIG_DIGITS; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; i + j < BIG_DIGITS; j++) {
            uint64_t cur = out.digit[i + j] + (uint64_t)pr->lhs.digit[i] * pr->rhs.digit[j] + carry;
            out.digit[i + j] = (uint32_t)(cur & 0xffffU);
            carry = cur >> DIGIT_BITS;
        }
    }
    return out;
}

static bits bits_of(const big *val, uint32_t width) {
    bits out = zeros(width);
    for (uint32_t k = 0; k < width; k++) {
        out.bit[k] = (uint8_t)((val->digit[k / DIGIT_BITS] >> (k % DIGIT_BITS)) & 1U);
    }
    return out;
}

/* One multiply cell (params NULL: the 36x36 defaults); every pattern pair plus random vectors. */
static void check_multiply(const odin3_value *params, binary_widths wid) {
    odin3_module *top = new_module();
    bus port[3];
    in_bus(top, "a", wid.aw, &port[0]);
    in_bus(top, "b", wid.bw, &port[1]);
    out_bus(top, "out", wid.aw + wid.bw, &port[2]);
    const bus *const buses[3] = {&port[0], &port[1], &port[2]};
    add_cell(top, type_id("multiply"), params, buses);
    odin3_sim *sim = build_ok(top);
    for (uint32_t vec = 0; vec < N_PATTERNS * N_PATTERNS + 100; vec++) {
        uint32_t pa = vec < N_PATTERNS * N_PATTERNS ? vec % N_PATTERNS : N_PATTERNS - 1;
        uint32_t pb = vec < N_PATTERNS * N_PATTERNS ? vec / N_PATTERNS : N_PATTERNS - 1;
        bits a_val = pattern((sized){pa, wid.aw});
        bits b_val = pattern((sized){pb, wid.bw});
        set_port(sim, 0, &a_val);
        set_port(sim, 1, &b_val);
        odin3_sim_cycle(sim);
        const big_pair ab = {big_of(&a_val), big_of(&b_val)};
        big prod = big_mul(&ab);
        bits want = bits_of(&prod, wid.aw + wid.bw);
        bits got = get_port(sim, 0);
        char what[64];
        (void)snprintf(what, sizeof what, "multiply %ux%u vector %u", wid.aw, wid.bw, vec);
        expect_bits(&want, &got, what);
    }
    odin3_sim_destroy(sim);
}

/* Review Focus 5: 36x36 -> 72 with the library defaults. */
static const char *const k_mul_names[3] = {"a", "b", "out"};

static void test_multiply_36x36(void) {
    read_lib(k_vtr_lib);
    check_multiply(NULL, (binary_widths){36, 36, 72});
    /* the largest product: (2^36 - 1)^2 = 2^72 - 2^37 + 1 */
    static const uint32_t k_w36[3] = {36, 36, 72};
    const cell_ports cp = {k_mul_names, k_w36, 2, 3};
    odin3_sim *sim = build_cell("multiply", NULL, &cp);
    bits ones = pattern((sized){1, 36});
    set_port(sim, 0, &ones);
    set_port(sim, 1, &ones);
    odin3_sim_cycle(sim);
    bits got = get_port(sim, 0);
    for (uint32_t k = 0; k < 72; k++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(k == 0 || k >= 37 ? 1 : 0, got.bit[k], "max product");
    }
    odin3_sim_destroy(sim);
}

/* Parametric widths, the wide path included (65x65 -> 130 bits, three limbs). */
static void test_multiply_param_widths(void) {
    read_lib(k_vtr_lib);
    for (uint32_t i = 0; i < N_WIDTHS; i++) {
        for (uint32_t j = 0; j < N_WIDTHS; j++) {
            const odin3_value params[2] = {odin3_value_int(k_widths[i]),
                                           odin3_value_int(k_widths[j])};
            check_multiply(params, (binary_widths){k_widths[i], k_widths[j], 0});
        }
    }
}

/* Every operator of the brief at a width W, against the bit-array reference. */
static const char k_ops_lib[] = "library ops_lib\n"
                                "cell ops hard\n"
                                "  param W int 8\n"
                                "  in a W ; in b W ; in sh 7\n"
                                "  out add W ; out sub W ; out mul W ; out band W ; out bor W\n"
                                "  out bxor W ; out bnot W ; out shl W ; out shr W\n"
                                "  out eq 1 ; out ne 1 ; out lt 1\n"
                                "  fn add = a + b\n"
                                "  fn sub = a - b\n"
                                "  fn mul = a * b\n"
                                "  fn band = a & b\n"
                                "  fn bor = a | b\n"
                                "  fn bxor = a ^ b\n"
                                "  fn bnot = ~a\n"
                                "  fn shl = a << sh\n"
                                "  fn shr = a >> sh\n"
                                "  fn eq = a == b\n"
                                "  fn ne = a != b\n"
                                "  fn lt = a < b\n"
                                "end\n";

enum { OPS_IN = 3, OPS_OUT = 12 };

/* The value shifted left (positive amount) or right (negative), zeros shifting in. */
typedef struct shift {
    const bits *val;
    int64_t by;
} shift;

static bits ref_shift(shift sh) {
    bits out = zeros(sh.val->width);
    for (uint32_t k = 0; k < out.width; k++) {
        int64_t from = (int64_t)k - sh.by;
        out.bit[k] = from >= 0 && from < (int64_t)out.width ? sh.val->bit[(size_t)from] : 0;
    }
    return out;
}

/* The 12 outputs of ops for a, b and sh, in port order. */
static void ops_reference(const pair *ab, uint64_t amount, bits *want) {
    want[0] = ref_add(ab);
    want[1] = ref_sub(ab);
    want[2] = ref_mul(ab);
    want[3] = ref_and(ab);
    want[4] = ref_or(ab);
    want[5] = ref_xor(ab);
    want[6] = ref_not(&ab->lhs);
    want[7] = ref_shift((shift){&ab->lhs, (int64_t)amount});
    want[8] = ref_shift((shift){&ab->lhs, -(int64_t)amount});
    int cmp = ref_ucmp(ab);
    want[9] = from_u64((sized){cmp == 0 ? 1U : 0U, 1});
    want[10] = from_u64((sized){cmp != 0 ? 1U : 0U, 1});
    want[11] = from_u64((sized){cmp < 0 ? 1U : 0U, 1});
}

static void check_ops(uint32_t width) {
    static const char *const k_names[OPS_IN + OPS_OUT] = {"a",   "b",    "sh",  "add",  "sub",
                                                          "mul", "band", "bor", "bxor", "bnot",
                                                          "shl", "shr",  "eq",  "ne",   "lt"};
    uint32_t widths[OPS_IN + OPS_OUT];
    for (uint32_t i = 0; i < OPS_IN + OPS_OUT; i++) {
        widths[i] = i >= OPS_IN + 9 ? 1 : width; /* eq, ne, lt are one bit */
    }
    widths[2] = 7; /* sh */
    const cell_ports cp = {k_names, widths, OPS_IN, OPS_IN + OPS_OUT};
    const odin3_value params[1] = {odin3_value_int(width)};
    odin3_sim *sim = build_cell("ops", params, &cp);
    static const uint64_t k_amounts[] = {0, 1, 3, 63, 64, 65, 99, 127};
    for (uint32_t vec = 0; vec < N_PATTERNS * N_PATTERNS; vec++) {
        pair ab = {pattern((sized){vec % N_PATTERNS, width}),
                   pattern((sized){vec / N_PATTERNS, width})};
        uint64_t amount = k_amounts[vec % (sizeof k_amounts / sizeof k_amounts[0])];
        bits sh_val = from_u64((sized){amount, 7});
        set_port(sim, 0, &ab.lhs);
        set_port(sim, 1, &ab.rhs);
        set_port(sim, 2, &sh_val);
        odin3_sim_cycle(sim);
        bits want[OPS_OUT];
        ops_reference(&ab, amount, want);
        for (uint32_t i = 0; i < OPS_OUT; i++) {
            char what[64];
            (void)snprintf(what, sizeof what, "ops W=%u %s vector %u", width, k_names[OPS_IN + i],
                           vec);
            bits got = get_port(sim, i);
            expect_bits(&want[i], &got, what);
        }
    }
    odin3_sim_destroy(sim);
}

static void test_fn_operators(void) {
    read_lib(k_ops_lib);
    static const uint32_t k_ops_widths[] = {1, 7, 8, 64, 65, 100};
    for (uint32_t i = 0; i < sizeof k_ops_widths / sizeof k_ops_widths[0]; i++) {
        check_ops(k_ops_widths[i]);
    }
}

/* Verilog-2005 sizing and signedness of fn expressions. */
static const char k_sem_lib[] = "library sem_lib\n"
                                "cell sem hard\n"
                                "  param P int 5\n"
                                "  in a 8 ; in b 8 ; in sa 8 signed ; in sb 8 signed ; in s 1\n"
                                "  out avg 8 ; out avg9 8 ; out smul 16 ; out mmul 16\n"
                                "  out slt 1 ; out mlt 1 ; out cat 12 ; out rep 8 ; out sel 1\n"
                                "  out mux 8 ; out addp 8 ; out neg 8 ; out lnot 1 ; out land 1\n"
                                "  out ge 1 ; out xnor 8 ; out lit 8 ; out cmpw 1\n"
                                "  fn avg = (a + b) >> 1\n"
                                "  fn avg9 = (a + b + 9'd0) >> 1\n"
                                "  fn smul = sa * sb\n"
                                "  fn mmul = sa * b\n"
                                "  fn slt = sa < sb\n"
                                "  fn mlt = sa < b\n"
                                "  fn cat = {a[3:0], b}\n"
                                "  fn rep = {{2{a[1:0]}}, b[7:4]}\n"
                                "  fn sel = a[P]\n"
                                "  fn mux = s ? a : b\n"
                                "  fn addp = a + P\n"
                                "  fn neg = -a\n"
                                "  fn lnot = !a\n"
                                "  fn land = a && b\n"
                                "  fn ge = a >= b\n"
                                "  fn xnor = a ~^ b\n"
                                "  fn lit = 8'hA5 ^ a\n"
                                "  fn cmpw = a + b == 9'd256\n"
                                "end\n";

enum { SEM_IN = 5, SEM_OUT = 18 };

typedef struct sem_in {
    uint32_t a, b, sa, sb, s;
} sem_in;

/* The 18 outputs of sem (P = param), computed with C integers. */
static void sem_reference(const sem_in *in, int64_t param, uint32_t *want) {
    int32_t sa = (int32_t)in->sa - (in->sa >= 0x80U ? 0x100 : 0); /* as 8-bit signed */
    int32_t sb = (int32_t)in->sb - (in->sb >= 0x80U ? 0x100 : 0);
    want[0] = ((in->a + in->b) & 0xffU) >> 1;
    want[1] = ((in->a + in->b) >> 1) & 0xffU;
    want[2] = (uint32_t)(sa * sb) & 0xffffU;
    want[3] = (in->sa * in->b) & 0xffffU;
    want[4] = sa < sb ? 1 : 0;
    want[5] = in->sa < in->b ? 1 : 0;
    want[6] = ((in->a & 0xfU) << 8) | in->b;
    want[7] = ((in->a & 3U) << 6) | ((in->a & 3U) << 4) | (in->b >> 4);
    want[8] = param >= 0 && param < 8 ? (in->a >> param) & 1U : 0;
    want[9] = in->s != 0 ? in->a : in->b;
    want[10] = (uint32_t)((int64_t)in->a + param) & 0xffU;
    want[11] = (0x100U - in->a) & 0xffU;
    want[12] = in->a == 0 ? 1 : 0;
    want[13] = in->a != 0 && in->b != 0 ? 1 : 0;
    want[14] = in->a >= in->b ? 1 : 0;
    want[15] = ~(in->a ^ in->b) & 0xffU;
    want[16] = 0xa5U ^ in->a;
    want[17] = in->a + in->b == 256 ? 1 : 0;
}

static uint32_t port_u32(const odin3_sim *sim, uint32_t port) {
    bits val = get_port(sim, port);
    return (uint32_t)low_u64(&val);
}

static const char *const k_sem_names[SEM_IN + SEM_OUT] = {
    "a",   "b",   "sa",  "sb",   "s",   "avg",  "avg9", "smul", "mmul", "slt", "mlt", "cat",
    "rep", "sel", "mux", "addp", "neg", "lnot", "land", "ge",   "xnor", "lit", "cmpw"};

/* Drives vector vec (four corners, then random) into sem and checks every output. */
static void sem_vector(uint32_t vec, odin3_sim *sim, int64_t param) {
    uint64_t draw = odin3_prng_next(&prng);
    sem_in in = {(uint32_t)(draw & 0xffU), (uint32_t)((draw >> 8) & 0xffU),
                 (uint32_t)((draw >> 16) & 0xffU), (uint32_t)((draw >> 24) & 0xffU),
                 (uint32_t)((draw >> 32) & 1U)};
    if (vec < 4) { /* corners: zero, all ones (carries), MSBs */
        in.a = vec < 2 ? 0xffU * vec : 0x80U;
        in.b = vec == 3 ? 0x80U : in.a;
    }
    const uint32_t ins[SEM_IN] = {in.a, in.b, in.sa, in.sb, in.s};
    for (uint32_t i = 0; i < SEM_IN; i++) {
        bits val = from_u64((sized){ins[i], i == 4 ? 1 : 8});
        set_port(sim, i, &val);
    }
    odin3_sim_cycle(sim);
    uint32_t want[SEM_OUT];
    sem_reference(&in, param, want);
    for (uint32_t i = 0; i < SEM_OUT; i++) {
        char what[96];
        (void)snprintf(what, sizeof what, "sem P=%lld %s a=%u b=%u sa=%u sb=%u s=%u",
                       (long long)param, k_sem_names[SEM_IN + i], in.a, in.b, in.sa, in.sb, in.s);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i], port_u32(sim, i), what);
    }
}

static void check_sem(int64_t param) {
    static const uint32_t k_widths_sem[SEM_IN + SEM_OUT] = {8, 8, 8, 8, 1, 8, 8, 16, 16, 1, 1, 12,
                                                            8, 1, 8, 8, 8, 1, 1, 1,  8,  8, 1};
    const cell_ports cp = {k_sem_names, k_widths_sem, SEM_IN, SEM_IN + SEM_OUT};
    const odin3_value params[1] = {odin3_value_int(param)};
    odin3_sim *sim = build_cell("sem", params, &cp);
    for (uint32_t vec = 0; vec < 400; vec++) {
        sem_vector(vec, sim, param);
    }
    odin3_sim_destroy(sim);
}

static void test_fn_verilog_semantics(void) {
    read_lib(k_sem_lib);
    check_sem(5);
    check_sem(7);
    check_sem(9); /* a[9] is out of range: reads 0 */
}

/* The limb path of fn: signed ports in a context wider than 64 bits, shift amounts over 64 bits. */
static const char k_wide_lib[] = "library wide_fn\n"
                                 "cell widef hard\n"
                                 "  in sa 70 signed ; in sb 70 signed ; in a 100 ; in sh 65\n"
                                 "  out ssum 100 ; out shl 100 ; out shr 100 ; out slt 1\n"
                                 "  fn ssum = sa + sb\n"
                                 "  fn shl = a << sh\n"
                                 "  fn shr = a >> sh\n"
                                 "  fn slt = sa < sb\n"
                                 "end\n"
                                 "cell negsl hard\n"
                                 "  param P int 0\n"
                                 "  in a 8 ; out y 2\n"
                                 "  fn y = a[P-1:0]\n"
                                 "end\n";

/* The four outputs of widef. A shift amount with bit 64 set shifts everything out. */
/* Inputs of widef: sa, sb; a; sh. */
typedef struct wide_in {
    pair sab;
    bits a_val;
    bits sh_val;
} wide_in;

static void wide_reference(const wide_in *in, bits *want) {
    const pair *sab = &in->sab;
    const bits *a_val = &in->a_val;
    const bits *sh_val = &in->sh_val;
    pair wide = {ext((operand){&sab->lhs, true}, 100), ext((operand){&sab->rhs, true}, 100)};
    want[0] = ref_add(&wide);
    uint64_t amount = low_u64(sh_val);
    int64_t by = sh_val->bit[64] != 0 || amount > 1000 ? 1000 : (int64_t)amount;
    want[1] = ref_shift((shift){a_val, by});
    want[2] = ref_shift((shift){a_val, -by});
    want[3] = from_u64((sized){ref_scmp(sab) < 0 ? 1U : 0U, 1});
}

static void test_fn_wide_signed_and_shift(void) {
    read_lib(k_wide_lib);
    static const char *const k_names[8] = {"sa", "sb", "a", "sh", "ssum", "shl", "shr", "slt"};
    static const uint32_t k_port_widths[8] = {70, 70, 100, 65, 100, 100, 100, 1};
    const cell_ports cp = {k_names, k_port_widths, 4, 8};
    odin3_sim *sim = build_cell("widef", NULL, &cp);
    static const uint64_t k_amounts[] = {0, 1, 63, 64, 99, 100, 5};
    for (uint32_t vec = 0; vec < N_PATTERNS * N_PATTERNS; vec++) {
        wide_in in = {
            {pattern((sized){vec % N_PATTERNS, 70}), pattern((sized){vec / N_PATTERNS, 70})},
            pattern((sized){N_PATTERNS - 1, 100}),
            from_u64((sized){k_amounts[vec % 7], 65})};
        in.sh_val.bit[64] = (uint8_t)(vec % 7 == 6); /* 2^64 + 5: every bit shifts out */
        set_port(sim, 0, &in.sab.lhs);
        set_port(sim, 1, &in.sab.rhs);
        set_port(sim, 2, &in.a_val);
        set_port(sim, 3, &in.sh_val);
        odin3_sim_cycle(sim);
        bits want[4];
        wide_reference(&in, want);
        for (uint32_t i = 0; i < 4; i++) {
            char what[64];
            (void)snprintf(what, sizeof what, "widef %s vector %u", k_names[4 + i], vec);
            bits got = get_port(sim, i);
            expect_bits(&want[i], &got, what);
        }
    }
    odin3_sim_destroy(sim);
    /* a[P-1:0] with P = 0 is a[-1:0]: two bits, a[0] above a 0 (index -1 reads 0) */
    static const char *const k_neg_names[2] = {"a", "y"};
    static const uint32_t k_neg_widths[2] = {8, 2};
    const cell_ports neg = {k_neg_names, k_neg_widths, 1, 2};
    sim = build_cell("negsl", NULL, &neg);
    bits a_val = from_u64((sized){0x81, 8});
    set_port(sim, 0, &a_val);
    odin3_sim_cycle(sim);
    TEST_ASSERT_EQUAL_UINT32(2, port_u32(sim, 0));
    odin3_sim_destroy(sim);
}

/* Cells the interpreter cannot run get no hook, and the build names them. */
static const char k_unsim_lib[] = "library unsim_lib\n"
                                  "cell divc hard\n"
                                  "  in a 8 ; in b 8 ; out y 8\n"
                                  "  fn y = a / b\n"
                                  "end\n"
                                  "cell rdout hard\n"
                                  "  in a 1 ; out y 1 ; out z 1\n"
                                  "  fn y = a\n"
                                  "  fn z = y\n"
                                  "end\n"
                                  "cell ffc gate\n"
                                  "  in d 1 ; in c 1 clock ; out q 1\n"
                                  "  seq q <= d @ posedge c\n"
                                  "end\n"
                                  "cell bbc blackbox\n"
                                  "  in a 1 ; out y 1\n"
                                  "end\n";

/* A library cell that must be rejected, and the reason the build gives. */
typedef struct rejected {
    const char *type;
    const char *needle;
} rejected;

static void expect_unsimulatable(rejected rej) {
    const char *type = rej.type;
    const char *needle = rej.needle;
    const odin3_celltype_def *def = odin3_celltype_get(design, type_id(type));
    TEST_ASSERT_NULL_MESSAGE(def->simulate, type);
    odin3_module *top = new_module();
    bus port[OPS_IN];
    const bus *buses[OPS_IN];
    for (uint32_t i = 0; i < def->n_ports; i++) {
        char name[16];
        (void)snprintf(name, sizeof name, "p%u", i);
        uint32_t width = odin3_celltype_port_width(design, type_id(type), NULL, i);
        if (def->ports[i].dir == ODIN3_DIR_IN) {
            in_bus(top, name, width, &port[i]);
        } else {
            out_bus(top, name, width, &port[i]);
        }
        buses[i] = &port[i];
    }
    add_cell(top, type_id(type), NULL, buses);
    char want[64];
    (void)snprintf(want, sizeof want, "cannot simulate `%s`", type);
    build_fails(top, want);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
}

static void test_unsimulatable_library_cells(void) {
    read_lib(k_unsim_lib);
    expect_unsimulatable((rejected){"divc", "no simulate hook"});
    expect_unsimulatable((rejected){"rdout", "no simulate hook"});
    expect_unsimulatable((rejected){"ffc", "no simulate hook"});
    expect_unsimulatable((rejected){"bbc", "a black box has no semantics"});
}

/* A width that would take too much scratch is refused at build, naming the cell. */
static void test_too_wide_is_rejected(void) {
    read_lib("library wide_lib\n"
             "cell widec hard\n"
             "  param N int 1\n"
             "  in a 1 ; out y 1\n"
             "  fn y = {N{a}} == 0\n"
             "end\n");
    odin3_module *top = new_module();
    bus port[2];
    in_bus(top, "a", 1, &port[0]);
    out_bus(top, "y", 1, &port[1]);
    const bus *const buses[2] = {&port[0], &port[1]};
    const odin3_value params[1] = {odin3_value_int((int64_t)1 << 40)};
    add_cell(top, type_id("widec"), params, buses);
    build_fails(top, "cannot simulate `widec`");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "too wide"), log_text);
}

/* Task 3a minor: a black box is simulated when its type has a hook (here $_AND_'s). */
static void test_blackbox_with_hook_simulates(void) {
    static const odin3_port_def k_ports[] = {{"A", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                             {"B", ODIN3_DIR_IN, true, 1, NULL, NULL, NULL},
                                             {"Y", ODIN3_DIR_OUT, true, 1, NULL, NULL, NULL}};
    const odin3_celltype_def *and_def = odin3_celltype_get(design, type_id("$_AND_"));
    const odin3_celltype_def def = {"bb_and", ODIN3_GRAN_BLACKBOX, 0,   k_ports, 3, NULL, 0, NULL,
                                    NULL,     and_def->simulate,   NULL};
    odin3_celltype_id bb = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_celltype_declare_blackbox(design, &def, &bb));
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, odin3_celltype_get(design, bb)->gran);
    odin3_module *top = new_module();
    bus port[3];
    in_bus(top, "a", 1, &port[0]);
    in_bus(top, "b", 1, &port[1]);
    out_bus(top, "y", 1, &port[2]);
    const bus *const buses[3] = {&port[0], &port[1], &port[2]};
    add_cell(top, bb, NULL, buses);
    odin3_sim *sim = build_ok(top);
    for (uint32_t combo = 0; combo < 4; combo++) {
        bits a_val = from_u64((sized){combo & 1U, 1});
        bits b_val = from_u64((sized){combo >> 1, 1});
        set_port(sim, 0, &a_val);
        set_port(sim, 1, &b_val);
        odin3_sim_cycle(sim);
        TEST_ASSERT_EQUAL_UINT32(combo == 3 ? 1 : 0, port_u32(sim, 0));
    }
    odin3_sim_destroy(sim);
}

/* The interpreter allocates nothing per cycle (wide path: 65x65). */
static void test_fn_hook_does_not_allocate(void) {
    read_lib(k_vtr_lib);
    static const uint32_t k_w65[3] = {65, 65, 130};
    const cell_ports cp = {k_mul_names, k_w65, 2, 3};
    const odin3_value params[2] = {odin3_value_int(65), odin3_value_int(65)};
    odin3_sim *sim = build_cell("multiply", params, &cp);
    bits a_val = pattern((sized){4, 65});
    bits b_val = pattern((sized){5, 65});
    set_port(sim, 0, &a_val);
    set_port(sim, 1, &b_val);
    odin3_util_set_alloc_fail_after(0);
    odin3_sim_cycle(sim);
    void *probe = odin3_util_malloc(1);
    TEST_ASSERT_NULL(probe);
    const big_pair ab = {big_of(&a_val), big_of(&b_val)};
    big prod = big_mul(&ab);
    bits want = bits_of(&prod, 130);
    bits got = get_port(sim, 0);
    expect_bits(&want, &got, "multiply 65x65 under failing allocation");
    odin3_sim_destroy(sim);
}

/* Out of memory at every allocation of a build with a tech-library cell (its sizing included). */
static void test_build_oom_sweep(void) {
    read_lib(k_vtr_lib);
    odin3_module *top = new_module();
    bus port[3];
    in_bus(top, "a", 36, &port[0]);
    in_bus(top, "b", 36, &port[1]);
    out_bus(top, "out", 72, &port[2]);
    const bus *const buses[3] = {&port[0], &port[1], &port[2]};
    add_cell(top, type_id("multiply"), NULL, buses);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fails = 0;
    for (; st == ODIN3_ERR_NO_MEMORY; fails++) {
        odin3_sim *sim = NULL;
        odin3_util_set_alloc_fail_after(fails);
        st = odin3_sim_build(design, odin3_module_id_of(top), &sim);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            odin3_sim_destroy(sim);
        } else {
            TEST_ASSERT_NULL(sim);
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_INT32(5, (int32_t)fails);
}

/* Out of memory at every allocation of reading a library (fn compilation included). */
static void test_read_oom_sweep(void) {
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long fails = 0;
    for (; st == ODIN3_ERR_NO_MEMORY; fails++) {
        odin3_design *fresh = odin3_design_create();
        TEST_ASSERT_NOT_NULL(fresh);
        const odin3_techlib_text src = {"inline.o3lib", odin3_bytes_cstr(k_sem_lib)};
        odin3_util_set_alloc_fail_after(fails);
        st = odin3_techlib_read_text(fresh, &src);
        odin3_util_set_alloc_fail_after(-1);
        odin3_design_destroy(fresh);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_THAN_INT32(5, (int32_t)fails); /* the sweep did hit allocations */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add);
    RUN_TEST(test_sub);
    RUN_TEST(test_mul);
    RUN_TEST(test_and);
    RUN_TEST(test_or);
    RUN_TEST(test_xor);
    RUN_TEST(test_eq);
    RUN_TEST(test_ne);
    RUN_TEST(test_lt);
    RUN_TEST(test_le);
    RUN_TEST(test_gt);
    RUN_TEST(test_ge);
    RUN_TEST(test_not);
    RUN_TEST(test_reduce_and);
    RUN_TEST(test_reduce_or);
    RUN_TEST(test_reduce_xor);
    RUN_TEST(test_known_answers);
    RUN_TEST(test_matches_simlib);
    RUN_TEST(test_mux);
    RUN_TEST(test_pmux);
    RUN_TEST(test_word_cells_have_hooks);
    RUN_TEST(test_div_is_rejected);
    RUN_TEST(test_word_hooks_do_not_allocate);
    RUN_TEST(test_adder);
    RUN_TEST(test_multiply_36x36);
    RUN_TEST(test_multiply_param_widths);
    RUN_TEST(test_fn_operators);
    RUN_TEST(test_fn_verilog_semantics);
    RUN_TEST(test_fn_wide_signed_and_shift);
    RUN_TEST(test_unsimulatable_library_cells);
    RUN_TEST(test_too_wide_is_rejected);
    RUN_TEST(test_blackbox_with_hook_simulates);
    RUN_TEST(test_fn_hook_does_not_allocate);
    RUN_TEST(test_build_oom_sweep);
    RUN_TEST(test_read_oom_sweep);
    return UNITY_END();
}
