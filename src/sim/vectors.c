/* vectors.c — the simulator's random-vector text format (see vectors.h). */
#include "sim/vectors.h"

#include "odin3/odin3.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t non_clock_bits(const odin3_sim *sim, uint32_t port) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < odin3_sim_input_width(sim, port); k++) {
        count += odin3_sim_input_is_clock(sim, (odin3_sim_bit){port, k}) ? 0U : 1U;
    }
    return count;
}

/* "# input NAME W" (when it has non-clock bits) and "# clock NAME K" per clock bit. */
static odin3_status input_header(const odin3_sim *sim, uint32_t port, odin3_strbuf *buf) {
    const char *name = odin3_sim_input_name(sim, port);
    uint32_t width = non_clock_bits(sim, port);
    odin3_status st =
        width > 0 ? odin3_strbuf_appendf(buf, "# input %s %u\n", name, width) : ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < odin3_sim_input_width(sim, port); k++) {
        if (odin3_sim_input_is_clock(sim, (odin3_sim_bit){port, k})) {
            st = odin3_strbuf_appendf(buf, "# clock %s %u\n", name, k);
        }
    }
    return st;
}

odin3_status odin3_sim_vectors_header(const odin3_sim *sim, const odin3_sim_vectors *run,
                                      odin3_strbuf *buf) {
    odin3_status st = odin3_strbuf_appendf(buf, "# %s seed %llu cycles %u\n", run->title,
                                           (unsigned long long)run->seed, run->cycles);
    for (uint32_t port = 0; st == ODIN3_OK && port < odin3_sim_input_count(sim); port++) {
        st = input_header(sim, port, buf);
    }
    for (uint32_t port = 0; st == ODIN3_OK && port < odin3_sim_output_count(sim); port++) {
        st = odin3_strbuf_appendf(buf, "# output %s %u\n", odin3_sim_output_name(sim, port),
                                  odin3_sim_output_width(sim, port));
    }
    return st;
}

static odin3_status append_bit(odin3_strbuf *buf, bool bit, bool *any) {
    *any = true;
    return odin3_strbuf_append(buf, odin3_bytes_cstr(bit ? "1" : "0"));
}

/* The non-clock input bits, each port MSB first; "-" when there are none. */
static odin3_status append_inputs(const odin3_sim *sim, odin3_strbuf *buf) {
    bool any = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t port = 0; st == ODIN3_OK && port < odin3_sim_input_count(sim); port++) {
        for (uint32_t k = odin3_sim_input_width(sim, port); st == ODIN3_OK && k-- > 0;) {
            const odin3_sim_bit at = {port, k};
            bool bit = false;
            if (!odin3_sim_input_is_clock(sim, at) &&
                odin3_sim_get_input(sim, at, &bit) == ODIN3_OK) {
                st = append_bit(buf, bit, &any);
            }
        }
    }
    return st == ODIN3_OK && !any ? odin3_strbuf_append(buf, odin3_bytes_cstr("-")) : st;
}

/* The output bits, each port MSB first; "-" when there are none. */
static odin3_status append_outputs(const odin3_sim *sim, odin3_strbuf *buf) {
    bool any = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t port = 0; st == ODIN3_OK && port < odin3_sim_output_count(sim); port++) {
        for (uint32_t k = odin3_sim_output_width(sim, port); st == ODIN3_OK && k-- > 0;) {
            bool bit = false;
            if (odin3_sim_get_output(sim, (odin3_sim_bit){port, k}, &bit) == ODIN3_OK) {
                st = append_bit(buf, bit, &any);
            }
        }
    }
    return st == ODIN3_OK && !any ? odin3_strbuf_append(buf, odin3_bytes_cstr("-")) : st;
}

odin3_status odin3_sim_vectors_line(const odin3_sim *sim, uint32_t cycle, odin3_strbuf *buf) {
    odin3_status st = odin3_strbuf_appendf(buf, "%u ", cycle);
    if (st == ODIN3_OK) {
        st = append_inputs(sim, buf);
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(buf, odin3_bytes_cstr(" "));
    }
    if (st == ODIN3_OK) {
        st = append_outputs(sim, buf);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(buf, odin3_bytes_cstr("\n")) : st;
}

odin3_status odin3_sim_vectors_step(odin3_sim *sim, odin3_prng *prng, uint32_t cycle,
                                    odin3_strbuf *buf) {
    odin3_sim_drive_random(sim, prng);
    odin3_status st = odin3_sim_cycle(sim);
    return st == ODIN3_OK ? odin3_sim_vectors_line(sim, cycle, buf) : st;
}
