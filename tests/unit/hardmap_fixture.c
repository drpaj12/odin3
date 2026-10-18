/* hardmap_fixture.c — see hardmap_fixture.h. */
#include "hardmap_fixture.h"

#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/vectors.h"
#include "unity.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

enum { LIB_PATH = 1024, MAX_EXHAUSTIVE_BITS = 20 };

odin3_status hm_build(const char *rtlil, pb_result *out) {
    char lib[LIB_PATH];
    int len = snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof lib);
    return pb_build_lib((pb_source){rtlil, lib}, out);
}

void hm_build_ok(const char *rtlil, pb_result *out) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_build(rtlil, out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_top(out->design, odin3_module_id_of(out->module)));
}

static void check_full(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

odin3_status hm_run(pb_result *res, odin3_hardmap_stats *stats) {
    check_full(res->design);
    odin3_status st = odin3_hardmap_run(&res->ctx, res->design, stats);
    check_full(res->design);
    return st;
}

static bool is_type(const odin3_module *module, odin3_node_id node, const char *type) {
    return odin3_node_live(module, node) &&
           strcmp(
               odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name,
               type) == 0;
}

uint32_t hm_count(const odin3_module *module, const char *type) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        found += is_type(module, (odin3_node_id){id}, type) ? 1U : 0U;
    }
    return found;
}

odin3_node_id hm_nth(const odin3_module *module, const char *type, uint32_t nth) {
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        if (is_type(module, (odin3_node_id){id}, type) && nth-- == 0) {
            return (odin3_node_id){id};
        }
    }
    return (odin3_node_id){0};
}

/* --- simulation ---------------------------------------------------------------------------- */

static odin3_sim *sim_of(pb_result *res) {
    odin3_sim *sim = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_sim_build(res->design, odin3_module_id_of(res->module), &sim));
    return sim;
}

/* Appends every output bit, port by port, as '0'/'1', then a newline. */
static void append_outputs(const odin3_sim *sim, odin3_strbuf *out) {
    for (uint32_t port = 0; port < odin3_sim_output_count(sim); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, port); bit++) {
            bool value = false;
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_sim_get_output(sim, (odin3_sim_bit){port, bit}, &value));
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_strbuf_append(out, odin3_bytes_cstr(value ? "1" : "0")));
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
}

typedef struct random_arg {
    uint32_t cycles;
    uint64_t seed;
} random_arg;

static void trace_random(pb_result *res, random_arg arg, odin3_strbuf *out) {
    odin3_sim *sim = sim_of(res);
    odin3_prng prng;
    odin3_prng_seed(&prng, arg.seed);
    for (uint32_t cycle = 0; cycle < arg.cycles; cycle++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_sim_vectors_step(sim, &prng, cycle, out));
    }
    odin3_sim_destroy(sim);
}

/* The non-clock input bits of sim in port order; asserts there are at most MAX_EXHAUSTIVE_BITS. */
static uint32_t input_bits(const odin3_sim *sim) {
    uint32_t total = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        total += odin3_sim_input_width(sim, port);
    }
    TEST_ASSERT_TRUE(total <= MAX_EXHAUSTIVE_BITS);
    return total;
}

static void drive_vector(odin3_sim *sim, uint64_t vector) {
    uint32_t at = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        for (uint32_t bit = 0; bit < odin3_sim_input_width(sim, port); bit++) {
            odin3_sim_bit pin = {port, bit};
            if (!odin3_sim_input_is_clock(sim, pin)) {
                TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                      odin3_sim_set_input(sim, pin, ((vector >> at) & 1U) != 0));
                at++;
            }
        }
    }
}

static void trace_all(pb_result *res, odin3_strbuf *out) {
    odin3_sim *sim = sim_of(res);
    uint64_t vectors = (uint64_t)1 << input_bits(sim);
    for (uint64_t vector = 0; vector < vectors; vector++) {
        drive_vector(sim, vector);
        odin3_sim_cycle(sim);
        append_outputs(sim, out);
    }
    odin3_sim_destroy(sim);
}

typedef void (*trace_fn)(pb_result *res, const void *arg, odin3_strbuf *out);

static void trace_random_arg(pb_result *res, const void *arg, odin3_strbuf *out) {
    const random_arg *ra = arg;
    trace_random(res, *ra, out);
}

static void trace_all_arg(pb_result *res, const void *arg, odin3_strbuf *out) {
    (void)arg;
    trace_all(res, out);
}

static bool equal_after_hardmap(hm_build_fn build, trace_fn trace, const void *arg) {
    pb_result before;
    pb_result after;
    odin3_strbuf text_before;
    odin3_strbuf text_after;
    odin3_strbuf_init(&text_before);
    odin3_strbuf_init(&text_after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build(&before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build(&after));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&after, NULL));
    trace(&before, arg, &text_before);
    trace(&after, arg, &text_after);
    TEST_ASSERT_TRUE(text_before.len > 0); /* a trace of nothing proves nothing */
    bool same = text_before.len == text_after.len &&
                memcmp(text_before.data, text_after.data, text_before.len) == 0;
    odin3_strbuf_free(&text_before);
    odin3_strbuf_free(&text_after);
    pb_free(&before);
    pb_free(&after);
    return same;
}

bool hm_sim_equal(hm_build_fn build, uint32_t cycles, uint64_t seed) {
    random_arg arg = {cycles, seed};
    return equal_after_hardmap(build, trace_random_arg, &arg);
}

bool hm_sim_exhaustive(hm_build_fn build) {
    return equal_after_hardmap(build, trace_all_arg, NULL);
}
