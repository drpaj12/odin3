/*
 * example_plugin.c — the smallest useful Odin III plugin: a cell type and a pass. Load it with
 *   odin3 --plugin build/debug/plugins/example_c/example_plugin.so \
 *         -p "read_blif x.blif; example_count"
 * It registers example_and, a hard cell Y = A & B sized by its WIDTH parameter, and the pass
 * example_count, which logs how many example_and cells the design holds and records the number
 * as the top module's string attribute example_count. Only odin3.h is used.
 */
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MSG_MAX = 128 };

static const char k_type[] = "example_and";

/* Exported entry point; declared here because odin3.h only declares its type. */
odin3_status odin3_plugin_init(uint32_t host_abi_version);

/* Adds to *count the live nodes of module whose type is the one named type. */
static odin3_status count_module(const odin3_design *design, uint32_t module, const char *type,
                                 uint32_t *count) {
    uint32_t end = 0;
    odin3_status st = odin3_module_get_node_end(design, module, &end);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_ref node = {module, id};
        bool live = false;
        const char *name = NULL;
        st = odin3_node_is_live(design, node, &live);
        if (st == ODIN3_OK && live) {
            st = odin3_node_get_type_name(design, node, &name);
        }
        if (st == ODIN3_OK && live && strcmp(name, type) == 0) {
            (*count)++;
        }
    }
    return st;
}

/* Records count as the top module's attribute example_count (no top: nothing to record). */
static odin3_status record_count(odin3_design *design, uint32_t count) {
    uint32_t top = 0;
    odin3_status st = odin3_design_get_top_module(design, &top);
    if (st != ODIN3_OK || top == 0) {
        return st;
    }
    char text[MSG_MAX];
    (void)snprintf(text, sizeof text, "%u", count);
    odin3_obj module = {top, ODIN3_OBJ_MODULE, 0};
    return odin3_attr_set_string(design, module, "example_count", text);
}

/* example_count: logs "example_count: <n> example_and cells" and records n (record_count). user
 * is the type name. */
static odin3_status count_run(odin3_design *design, const char *args, void *user) {
    if (args[0] != '\0') {
        (void)odin3_log_write(ODIN3_LOG_ERROR, "example_count: takes no arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *type = user;
    uint32_t modules = 0;
    uint32_t count = 0;
    odin3_status st = odin3_design_get_module_count(design, &modules);
    for (uint32_t i = 1; st == ODIN3_OK && i <= modules; i++) {
        st = count_module(design, i, type, &count);
    }
    if (st == ODIN3_OK) {
        char msg[MSG_MAX];
        (void)snprintf(msg, sizeof msg, "example_count: %u %s cells", count, type);
        st = odin3_log_write(ODIN3_LOG_INFO, msg);
    }
    return st == ODIN3_OK ? record_count(design, count) : st;
}

/* example_and: plain data, every reserved slot NULL; no simulation hooks (the simulator's cell
 * view is not part of ABI v3). */
static odin3_status register_celltype(void) {
    static const odin3_plugin_port ports[] = {{"A", ODIN3_DIR_IN, 0, "WIDTH", false},
                                              {"B", ODIN3_DIR_IN, 0, "WIDTH", false},
                                              {"Y", ODIN3_DIR_OUT, 0, "WIDTH", false}};
    static const odin3_plugin_param params[] = {{"WIDTH", ODIN3_VAL_INT, 1}};
    odin3_plugin_celltype def;
    memset(&def, 0, sizeof def);
    def.name = k_type;
    def.gran = ODIN3_GRAN_HARD;
    def.ports = ports;
    def.n_ports = sizeof ports / sizeof ports[0];
    def.params = params;
    def.n_params = sizeof params / sizeof params[0];
    return odin3_celltype_register(&def);
}

static odin3_status register_pass(void) {
    odin3_plugin_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.name = "example_count";
    pass.help = "example_count: log the number of example_and cells (example plugin)";
    pass.run = count_run;
    pass.user = (void *)k_type;
    return odin3_pass_register(&pass);
}

odin3_status odin3_plugin_init(uint32_t host_abi_version) {
    if (host_abi_version != (uint32_t)ODIN3_ABI_VERSION) {
        return ODIN3_ERR_ABI_MISMATCH;
    }
    odin3_status st = register_celltype();
    if (st == ODIN3_OK) {
        st = register_pass();
    }
    if (st == ODIN3_OK) {
        (void)fprintf(stderr, "example_plugin: initialised (ABI %u)\n", host_abi_version);
    }
    return st;
}
