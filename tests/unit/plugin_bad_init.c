/*
 * plugin_bad_init.c — a test plugin whose init registers a pass and a cell type with a simulate
 * hook, then fails (test_abi_ext): the host must keep the object loaded, so both stay usable.
 */
#include "odin3/odin3.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Exported entry point; declared here because odin3.h only declares its type. */
odin3_status odin3_plugin_init(uint32_t host_abi_version);

static const char k_ran[] = "bad_init_pass: ran";

/* Logs k_ran (code and data both in this object). */
static odin3_status bad_run(odin3_design *design, const char *args, void *user) {
    (void)args;
    uint32_t count = 0;
    odin3_status st = odin3_design_get_module_count(design, &count);
    return st == ODIN3_OK ? odin3_log_write(ODIN3_LOG_INFO, user) : st;
}

/* Never called by the tests; it only has to be a function of this object. */
static void bad_simulate(const odin3_sim_cell *cell) {
    (void)cell;
}

odin3_status odin3_plugin_init(uint32_t host_abi_version) {
    (void)host_abi_version;
    odin3_plugin_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.name = "bad_init_pass";
    pass.help = "bad_init_pass: registered by a plugin whose init then fails";
    pass.run = bad_run;
    pass.user = (void *)k_ran;
    odin3_status st = odin3_pass_register(&pass);
    static const odin3_plugin_port ports[] = {{"A", ODIN3_DIR_IN, 1, NULL, true}};
    odin3_plugin_celltype def;
    memset(&def, 0, sizeof def);
    def.name = "bad_init_cell";
    def.gran = ODIN3_GRAN_HARD;
    def.ports = ports;
    def.n_ports = 1;
    def.simulate = bad_simulate;
    if (st == ODIN3_OK) {
        st = odin3_celltype_register(&def);
    }
    return st == ODIN3_OK ? ODIN3_ERR_PLUGIN : st;
}
