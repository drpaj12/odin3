/* ffcells_fixture.c — module `ffcells`, one cell of each IR change D type (ffcells_fixture.h). */
#include "ffcells_fixture.h"

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "unity.h"
#include "util/str.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { NAME_CAP = 64, PORTS_CAP = 8, PINS_CAP = 16 };

const char *const ffcells_types[FFCELLS_COUNT] = {"$adffe",   "$sdffe",    "$sdffce", "$aldff",
                                                  "$aldffe",  "$dffsr",    "$dffsre", "$dlatch",
                                                  "$adlatch", "$dlatchsr", "$sr"};

static const uint8_t k_value01[FFCELLS_WIDTH] = {ODIN3_BIT_1, ODIN3_BIT_0}; /* 2'b01, LSB first */

static uint32_t intern(odin3_design *design, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

/* The fixture's value of parameter def. */
static odin3_value param_value(const odin3_param_def *def) {
    if (def->kind == ODIN3_VAL_BITS) {
        odin3_value val = {ODIN3_VAL_BITS, 0, k_value01, FFCELLS_WIDTH, 0, 0};
        return val;
    }
    if (strcmp(def->name, "WIDTH") == 0) {
        return odin3_value_int(FFCELLS_WIDTH);
    }
    bool low = strcmp(def->name, "EN_POLARITY") == 0 || strcmp(def->name, "CLR_POLARITY") == 0;
    return odin3_value_int(low ? 0 : 1);
}

/* params[k] = the fixture's value of def's parameter k. */
static void param_values(const odin3_celltype_def *def, odin3_value *params) {
    for (uint32_t k = 0; k < def->n_params; k++) {
        params[k] = param_value(&def->params[k]);
    }
}

/* Module port <type without $>_<port in lower case> of width bits; its nets into nets. */
static odin3_netvec add_port(odin3_module *mod, const char *type, const odin3_port_def *port,
                             uint32_t width, odin3_net_id *nets) {
    char name[NAME_CAP];
    TEST_ASSERT_TRUE(width <= PINS_CAP);
    int len = snprintf(name, sizeof name, "%s_%s", type + 1, port->name);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof name);
    for (char *at = strchr(name, '_') + 1; *at != '\0'; at++) {
        *at = (char)tolower((unsigned char)*at);
    }
    odin3_design *design = odin3_module_design(mod);
    odin3_port_spec spec = {intern(design, name),
                            port->dir,
                            width,
                            port->scalar,
                            (odin3_prov_id){0},
                            false,
                            0,
                            0,
                            false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, NULL));
    odin3_wire_id wire = odin3_module_port_wire(mod, odin3_module_port_count(mod) - 1);
    for (uint32_t bit = 0; bit < width; bit++) {
        nets[bit] = odin3_wire_net(mod, wire, bit);
    }
    return (odin3_netvec){nets, width};
}

static void add_cell(odin3_module *mod, const char *type) {
    odin3_design *design = odin3_module_design(mod);
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(design, intern(design, type), &tid), type);
    const odin3_celltype_def *def = odin3_celltype_get(design, tid);
    TEST_ASSERT_TRUE(def->n_ports <= PORTS_CAP && def->n_params <= PORTS_CAP);
    odin3_value params[PORTS_CAP];
    param_values(def, params);
    odin3_net_id nets[PORTS_CAP][PINS_CAP];
    odin3_netvec ports[PORTS_CAP];
    for (uint32_t k = 0; k < def->n_ports; k++) {
        uint32_t width = odin3_celltype_port_width(design, tid, params, k);
        ports[k] = add_port(mod, type, &def->ports[k], width, nets[k]);
    }
    char name[NAME_CAP];
    (void)snprintf(name, sizeof name, "c_%s", type + 1);
    odin3_node_spec spec = {tid, intern(design, name), (odin3_prov_id){0}, params, def->n_params};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL),
                                  type);
}

odin3_module *ffcells_build(odin3_design *design) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, intern(design, "ffcells"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    for (uint32_t i = 0; i < FFCELLS_COUNT; i++) {
        add_cell(mod, ffcells_types[i]);
    }
    return mod;
}
