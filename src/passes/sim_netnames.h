/*
 * sim_netnames.h — the net namer of `sim --vcd-all` and `sim_tb --vcd-all`: names the nets of a
 * module as `write_blif --vtr` writes them (the VTR dialect's naming run, odin3_blif_vtr_name_top),
 * so a waveform variable and a BLIF net of one netlist carry one name.
 */
#ifndef ODIN3_PASSES_SIM_NETNAMES_H
#define ODIN3_PASSES_SIM_NETNAMES_H

#include "backends/blif/writer.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>

typedef struct odin3_sim_netnames {
    const odin3_design *design; /* borrowed */
    odin3_blif_write_opts opts;
    const char *who; /* the pass name for warnings */
    odin3_vec mods;  /* one entry per module asked about */
} odin3_sim_netnames;

/*
 * Parses a --vcd-names value (`short`, `provenance` or `hash`) into *style; false (nothing
 * logged) for any other text.
 */
bool odin3_sim_netnames_style(const char *text, odin3_blif_name_style *style);

/* Starts a namer over design; who is the pass name used in warnings. Free with _free. */
void odin3_sim_netnames_init(odin3_sim_netnames *nn, const odin3_design *design, const char *who,
                             odin3_blif_name_style style);

/*
 * An odin3_sim_net_namer (sim/signals.h) with ctx = an odin3_sim_netnames: the writer's name of
 * net in module. A module the writer cannot name (a refusal) is warned about once, and its nets
 * get the default name (false).
 */
bool odin3_sim_netnames_name(void *ctx, odin3_module_id module, odin3_net_id net,
                             odin3_strbuf *out);

/* Frees the names of every module; NULL-safe on a zeroed namer. */
void odin3_sim_netnames_free(odin3_sim_netnames *nn);

#endif
