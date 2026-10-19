/* lpmmap_fixture.c — see lpmmap_fixture.h. */
#include "lpmmap_fixture.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

enum { LIB_PATH = 1024 };

void lm_build_ok(const char *rtlil, pb_result *out) {
    char lib[LIB_PATH];
    int len = snprintf(lib, sizeof lib, "%s/lpm.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof lib);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build_lib((pb_source){rtlil, lib}, out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_top(out->design, odin3_module_id_of(out->module)));
}

static bool is_type(const odin3_module *module, odin3_node_id node, const char *type) {
    return odin3_node_live(module, node) &&
           strcmp(
               odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))->name,
               type) == 0;
}

uint32_t lm_count(const odin3_module *module, const char *type) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        found += is_type(module, (odin3_node_id){id}, type) ? 1U : 0U;
    }
    return found;
}

odin3_node_id lm_nth(const odin3_module *module, const char *type, uint32_t nth) {
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        if (is_type(module, (odin3_node_id){id}, type) && nth-- == 0) {
            return (odin3_node_id){id};
        }
    }
    return (odin3_node_id){0};
}

static const odin3_celltype_def *def_of(const odin3_module *module, odin3_node_id node) {
    return odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node));
}

static const odin3_value *param_of(const odin3_module *module, odin3_node_id node,
                                   const char *name) {
    const odin3_celltype_def *def = def_of(module, node);
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return odin3_node_param(module, node, i);
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

int64_t lm_param_int(const odin3_module *module, odin3_node_id node, const char *name) {
    const odin3_value *val = param_of(module, node, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(val, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_VAL_INT, val->kind, name);
    return val->i;
}

const char *lm_param_str(const odin3_module *module, odin3_node_id node, const char *name) {
    const odin3_value *val = param_of(module, node, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(val, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_VAL_STRING, val->kind, name);
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), val->str);
}

static int port_index(const odin3_module *module, odin3_node_id node, const char *port) {
    const odin3_celltype_def *def = def_of(module, node);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, port) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t lm_port_width(const odin3_module *module, odin3_node_id node, const char *port) {
    int idx = port_index(module, node, port);
    return idx < 0 ? 0 : odin3_node_port(module, node, (uint32_t)idx).count;
}

odin3_net_id lm_pin_net(const odin3_module *module, odin3_node_id node, const char *port,
                        uint32_t bit) {
    int idx = port_index(module, node, port);
    TEST_ASSERT_TRUE_MESSAGE(idx >= 0, port);
    odin3_pinslice slice = odin3_node_port(module, node, (uint32_t)idx);
    TEST_ASSERT_TRUE_MESSAGE(bit < slice.count, port);
    return odin3_pin_net(module, (odin3_pin_id){slice.first.v + bit});
}

bool lm_port_open(const odin3_module *module, odin3_node_id node, const char *port) {
    uint32_t width = lm_port_width(module, node, port);
    for (uint32_t bit = 0; bit < width; bit++) {
        if (lm_pin_net(module, node, port, bit).v != 0) {
            return false;
        }
    }
    return true;
}
