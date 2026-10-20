/*
 * fsm.c — the fsm driver (fsm.cc:80-163): the stages over the whole design, in Yosys's order.
 */
#include "passes/fsm/fsm.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <string.h>

odin3_status odin3_fsm_detect_run(odin3_design *design, const odin3_fsm_opts *opts) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_fsm_mod fm;
        st = odin3_fsm_mod_open(&fm, design, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st == ODIN3_OK) {
            st = odin3_fsm_detect_module(&fm);
            odin3_fsm_mod_close(&fm);
        }
    }
    return st;
}

/* --- the driver ---------------------------------------------------------------------------- */

/* One module in the run: its context and its extracted FSMs. */
typedef struct modrun {
    odin3_fsm_mod fm;
    odin3_vec insts; /* odin3_fsm_inst */
} modrun;

typedef struct fsm_run {
    odin3_design *design;
    const odin3_fsm_opts *opts;
    modrun *mods;
    uint32_t n_mods;
} fsm_run;

static void run_free(fsm_run *run) {
    for (uint32_t i = 0; i < run->n_mods; i++) {
        odin3_fsm_inst *insts = run->mods[i].insts.data;
        for (size_t k = 0; k < run->mods[i].insts.len; k++) {
            odin3_fsm_inst_free(&insts[k]);
        }
        odin3_vec_free(&run->mods[i].insts);
        odin3_fsm_mod_close(&run->mods[i].fm);
    }
    odin3_util_free(run->mods);
    memset(run, 0, sizeof *run);
}

static odin3_status run_open(fsm_run *run, odin3_design *design, const odin3_fsm_opts *opts) {
    memset(run, 0, sizeof *run);
    run->design = design;
    run->opts = opts;
    uint32_t count = odin3_design_module_end(design) - 1U;
    run->mods = odin3_util_calloc((size_t)(count + 1U) * sizeof *run->mods);
    if (run->mods == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; i++) {
        odin3_vec_init(&run->mods[i].insts, sizeof(odin3_fsm_inst));
        run->n_mods = i + 1U;
        odin3_module *module = odin3_module_get(design, (odin3_module_id){i + 1U});
        odin3_status st = odin3_fsm_mod_open(&run->mods[i].fm, design, module, opts);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status stage_detect(fsm_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < run->n_mods; i++) {
        st = odin3_fsm_detect_module(&run->mods[i].fm);
    }
    return st;
}

static odin3_status stage_extract(fsm_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < run->n_mods; i++) {
        st = odin3_fsm_extract_module(&run->mods[i].fm, &run->mods[i].insts);
    }
    return st;
}

static void stage_info(const fsm_run *run) {
    for (uint32_t i = 0; i < run->n_mods; i++) {
        const odin3_fsm_inst *insts = run->mods[i].insts.data;
        for (size_t k = 0; k < run->mods[i].insts.len; k++) {
            odin3_fsm_info(&run->mods[i].fm, &insts[k]);
        }
    }
}

odin3_status odin3_fsm_run(odin3_design *design, const odin3_fsm_opts *opts) {
    fsm_run run;
    odin3_status st = run_open(&run, design, opts);
    if (st == ODIN3_OK && !opts->nodetect) {
        st = stage_detect(&run);
    }
    if (st == ODIN3_OK) {
        st = stage_extract(&run);
    }
    if (st == ODIN3_OK) {
        stage_info(&run);
    }
    run_free(&run);
    return st;
}
