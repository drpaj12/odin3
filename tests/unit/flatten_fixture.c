/* flatten_fixture.c — see flatten_fixture.h. */
#include "flatten_fixture.h"

#include "ir/celltype.h"
#include "ir/value.h"
#include "unity.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

enum { MAX_PORTS = 32 };

void ff_begin(ff *fix) {
    fix->prov = (odin3_prov_id){0};
    fix->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(fix->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fix->design, ff_str(fix, "flatten"), &fix->ctx));
}

void ff_end(ff *fix) {
    odin3_design_destroy(fix->design);
    fix->design = NULL;
}

uint32_t ff_str(ff *fix, const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(fix->design, odin3_bytes_cstr(text), &str));
    return str;
}

odin3_module *ff_module(ff *fix, const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(fix->design, ff_str(fix, name), (odin3_prov_id){0}, &mid));
    return odin3_module_get(fix->design, mid);
}

odin3_node_id ff_port(ff *fix, odin3_module *mod, const char *name, odin3_dir dir, uint32_t width) {
    odin3_port_spec spec = {ff_str(fix, name), dir, width, width == 1, {0}, false, 0, 0, false};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
    return node;
}

odin3_net_id ff_wnet(ff *fix, odin3_module *mod, const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(mod, ff_str(fix, name));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(mod, wire, bit);
}

odin3_net_id ff_wire(ff *fix, odin3_module *mod, const char *name, uint32_t width) {
    odin3_wire_spec spec = {ff_str(fix, name), (int32_t)width - 1, 0, false, {0}};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(mod, &spec, NULL, &wire));
    return odin3_wire_net(mod, wire, 0);
}

odin3_net_id ff_net(ff *fix, odin3_module *mod, const char *name) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(mod, name != NULL ? ff_str(fix, name) : 0,
                                                     (odin3_prov_id){0}, &net));
    return net;
}

static odin3_celltype_id type_named(ff *fix, const char *type) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix->design, ff_str(fix, type), &tid));
    return tid;
}

odin3_node_id ff_gate(ff *fix, odin3_module *mod, ff_cell cell, const odin3_net_id *nets) {
    odin3_celltype_id tid = type_named(fix, cell.type);
    const odin3_celltype_def *def = odin3_celltype_get(fix->design, tid);
    TEST_ASSERT_TRUE(def->n_ports <= MAX_PORTS);
    odin3_netvec ports[MAX_PORTS];
    for (uint32_t i = 0; i < def->n_ports; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    odin3_node_spec spec = {tid, cell.name != NULL ? ff_str(fix, cell.name) : 0, fix->prov, NULL,
                            0};
    fix->prov = (odin3_prov_id){0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, &node));
    return node;
}

odin3_node_id ff_inst_bus(ff *fix, ff_pair link, const char *name, const odin3_netvec *ports) {
    odin3_node_spec spec = {odin3_module_celltype(link.child), name != NULL ? ff_str(fix, name) : 0,
                            fix->prov, NULL, 0};
    fix->prov = (odin3_prov_id){0};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(link.parent, &spec, ports, &node));
    return node;
}

odin3_node_id ff_inst(ff *fix, ff_pair link, const char *name, const odin3_net_id *nets) {
    uint32_t n_ports = odin3_module_port_count(link.child);
    TEST_ASSERT_TRUE(n_ports <= MAX_PORTS);
    odin3_netvec ports[MAX_PORTS];
    for (uint32_t i = 0; i < n_ports; i++) {
        ports[i] = (odin3_netvec){&nets[i], 1};
    }
    return ff_inst_bus(fix, link, name, ports);
}

odin3_net_id ff_const(ff *fix, odin3_module *mod, int value) {
    odin3_celltype_id tid = type_named(fix, value != 0 ? "$_CONST1_" : "$_CONST0_");
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == tid.v) {
            return odin3_pin_net(mod, odin3_node_pins(mod, node).first);
        }
    }
    odin3_net_id net = ff_net(fix, mod, NULL);
    odin3_netvec port = {&net, 1};
    odin3_node_spec spec = {tid, 0, {0}, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, &port, NULL));
    return net;
}

void ff_attr_int(ff *fix, odin3_module *mod, odin3_objref obj, const char *key, int64_t value) {
    odin3_value val = odin3_value_int(value);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_attr_set(mod, obj, ff_str(fix, key), &val));
}

uint32_t ff_live_nodes(const odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        count += odin3_node_live(mod, (odin3_node_id){nid}) ? 1 : 0;
    }
    return count;
}

odin3_prov_id ff_source(ff *fix, const char *file, uint32_t line, const char *hier) {
    odin3_pass_ctx rd = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fix->design, ff_str(fix, "read_verilog"), &rd));
    odin3_srcloc loc = {ff_str(fix, file), line, 1, line, 4, 0};
    odin3_prov_origin origin = {&loc, 1, 0, hier != NULL ? ff_str(fix, hier) : 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&rd, &origin, &id));
    return id;
}
