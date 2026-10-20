/*
 * fsm.c — the fsm driver (fsm.cc:80-163): the stages over the whole design, in Yosys's order.
 */
#include "passes/fsm/fsm.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/log.h"

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
