/*
 * vcd.h — the VCD waveform writer of the `sim` pass (S1, spec SW-1 to SW-3): an IEEE 1364
 * four-value VCD (values 0 and 1 only) of every listed signal of a signal table. One cycle is
 * ODIN3_SIM_VCD_PERIOD time units; the file is a function of the netlist and the inputs.
 */
#ifndef ODIN3_SIM_VCD_H
#define ODIN3_SIM_VCD_H

#include "odin3/odin3.h"
#include "sim/signals.h"
#include "sim/sim.h"

#include <stdint.h>
#include <stdio.h>

enum {
    ODIN3_SIM_VCD_PERIOD = 10,  /* time units per cycle */
    ODIN3_SIM_VCD_INPUT_AT = 2, /* inputs driven and settled at 10c + 2 */
    ODIN3_SIM_VCD_RISE_AT = 5,  /* rising edge and its settle at 10c + 5 */
    ODIN3_SIM_VCD_CODE_MAX = 8  /* identifier code buffer, with the NUL */
};

typedef struct odin3_sim_vcd {
    const odin3_sim_signals *sig; /* borrowed */
    FILE *out;                    /* borrowed */
    uint8_t *last; /* last written bit of every listed signal bit, in ID order; the second
                      n_last bytes are the scratch of the current values */
    uint32_t n_last;
} odin3_sim_vcd;

/*
 * Writes `index` (the listed signal's ID minus 1) in base 94 with the digits `!`..`~`, least
 * significant digit first, NUL-terminated, into code (SW-2).
 */
void odin3_sim_vcd_code(uint32_t index, char code[ODIN3_SIM_VCD_CODE_MAX]);

/*
 * Writes the header (SW-2) and the `#0` `$dumpvars` block with the current value of every listed
 * signal to out, and starts tracking changes. vcd->out and vcd->sig are borrowed and must outlive
 * the writer; free it with odin3_sim_vcd_free. ODIN3_ERR_NO_MEMORY, ODIN3_ERR_IO when the stream
 * reports an error (vcd is freed and zeroed on failure).
 */
odin3_status odin3_sim_vcd_begin(odin3_sim_vcd *vcd, const odin3_sim_signals *sig, FILE *out);

/*
 * Writes `#<time>` and the signals whose value changed since the last write (SW-3 format) when any
 * changed, nothing otherwise. ODIN3_ERR_IO when the stream reports an error.
 */
odin3_status odin3_sim_vcd_sample(odin3_sim_vcd *vcd, uint64_t time);

/*
 * Runs cycle `cycle` of sim (whose signal table is vcd->sig) with the inputs as set: settle, sample
 * at 10c+2, rising edge, sample at 10c+5, falling edge, sample at 10c+10. It computes exactly what
 * odin3_sim_cycle does. ODIN3_ERR_IO (stops at the first failed write; the cycle may be
 * incomplete).
 */
odin3_status odin3_sim_vcd_cycle(odin3_sim_vcd *vcd, odin3_sim *sim, uint64_t cycle);

/* Frees the writer's memory and zeroes it (the stream stays open); NULL is a no-op. */
void odin3_sim_vcd_free(odin3_sim_vcd *vcd);

#endif
