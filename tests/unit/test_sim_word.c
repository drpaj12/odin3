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

/* An arithmetic or bitwise result: both operands extended to Y_WIDTH by their own signedness. */
static pair at_y(const odin3_value *params, const pair *ab) {
    uint32_t yw = (uint32_t)params[4].i;
    pair out = {ext((operand){&ab->lhs, params[0].i != 0}, yw),
                ext((operand){&ab->rhs, params[1].i != 0}, yw)};
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

/* The exact integer comparison of A and B (each by its own signedness). */
static int bin_cmp(const odin3_value *params, const pair *ab) {
    uint32_t width = (ab->lhs.width > ab->rhs.width ? ab->lhs.width : ab->rhs.width) + 1;
    pair op = {ext((operand){&ab->lhs, params[0].i != 0}, width),
               ext((operand){&ab->rhs, params[1].i != 0}, width)};
    return ref_scmp(&op);
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
    /* mixed signedness compares exact integers: -1 (1'b1 signed) < 1 (1'b1 unsigned) */
    one_binary lt = {"$lt", 1, 0, from_u64((sized){1, 1}), from_u64((sized){1, 1}), 1};
    TEST_ASSERT_EQUAL_HEX64(1, low_u64((bits[]){run_one_binary(&lt)}));
    /* -1 (1'b1 signed) != 1 (2'b01 unsigned); Yosys const_eq would zero-extend both and say 1 */
    one_binary eq = {"$eq", 1, 0, from_u64((sized){1, 1}), from_u64((sized){1, 2}), 1};
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&eq)}));
    /* -1 (7'b1111111 signed) == 65 ones signed */
    bits ones65 = pattern((sized){1, 65});
    one_binary eqs = {"$eq", 1, 1, from_u64((sized){0x7f, 7}), ones65, 1};
    TEST_ASSERT_EQUAL_HEX64(1, low_u64((bits[]){run_one_binary(&eqs)}));
    /* the same with B unsigned: -1 versus 2^65-1 */
    eqs.b_signed = 0;
    TEST_ASSERT_EQUAL_HEX64(0, low_u64((bits[]){run_one_binary(&eqs)}));
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

/* Hooks never allocate: with the next allocation set to fail, cycles still compute right. */
static void test_word_hooks_do_not_allocate(void) {
    one_binary mul = {"$mul", 1, 0, pattern((sized){4, 65}), pattern((sized){5, 65}), 65};
    odin3_module *top = new_module();
    bus a_bus;
    bus b_bus;
    bus y_bus;
    in_bus(top, "A", 65, &a_bus);
    in_bus(top, "B", 65, &b_bus);
    out_bus(top, "Y", 65, &y_bus);
    const odin3_value params[5] = {odin3_value_int(1), odin3_value_int(0), odin3_value_int(65),
                                   odin3_value_int(65), odin3_value_int(65)};
    const bus *const buses[3] = {&a_bus, &b_bus, &y_bus};
    add_cell(top, type_id("$mul"), params, buses);
    odin3_sim *sim = build_ok(top);
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
    RUN_TEST(test_mux);
    RUN_TEST(test_pmux);
    RUN_TEST(test_word_cells_have_hooks);
    RUN_TEST(test_div_is_rejected);
    RUN_TEST(test_word_hooks_do_not_allocate);
    return UNITY_END();
}
