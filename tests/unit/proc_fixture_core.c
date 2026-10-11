/*
 * proc_fixture_core.c — Task 3's P1 $proc node built without Unity, so both the unit tests
 * (proc_fixture.c's asserting wrappers) and the odin3_procfix library (the Python walk, 2D Task 6)
 * make the same node.
 */
#include "proc_fixture.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"

#include <stddef.h>
#include <stdint.h>

enum { FIXTURE_ARENA_CHUNK = 1024, P1_WIRES = 4 };

/* A fresh 1-bit wire named name in module; *net gets its bit's net. */
static odin3_status wire_net(odin3_module *module, const char *name, odin3_prov_id prov,
                             odin3_net_id *net) {
    uint32_t str = 0;
    odin3_status st =
        odin3_design_intern(odin3_module_design(module), odin3_bytes_cstr(name), &str);
    odin3_wire_spec spec = {str, 0, 0, false, prov};
    odin3_wire_id wire = {0};
    if (st == ODIN3_OK) {
        st = odin3_wire_create(module, &spec, NULL, &wire);
    }
    *net = st == ODIN3_OK ? odin3_wire_net(module, wire, 0) : (odin3_net_id){0};
    return st;
}

/* The wires clk, d, q and t0, created in that order; nets gets their nets. */
static odin3_status p1_wires(odin3_module *module, odin3_prov_id prov,
                             odin3_net_id nets[P1_WIRES]) {
    static const char *const k_names[P1_WIRES] = {"clk", "d", "q", "t0"};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < P1_WIRES && st == ODIN3_OK; i++) {
        st = wire_net(module, k_names[i], prov, &nets[i]);
    }
    return st;
}

/* The built-in $proc type of the design. */
static odin3_status proc_type(const odin3_design *design, odin3_celltype_id *type) {
    uint32_t name = 0;
    bool found = odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("$proc"), &name) &&
                 odin3_celltype_find(design, name, type);
    return found ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

odin3_status proc_fixture_build_p1(odin3_module *module, odin3_prov_id prov, odin3_node_id *node) {
    odin3_net_id nets[P1_WIRES] = {{0}};
    odin3_celltype_id type = {0};
    odin3_status st = p1_wires(module, prov, nets);
    if (st == ODIN3_OK) {
        st = proc_type(odin3_module_design(module), &type);
    }
    const odin3_pbit pbits[] = {ODIN3_PBIT_OUT | 1, ODIN3_PBIT_OUT | 0, ODIN3_PBIT_OUT | 1,
                                ODIN3_PBIT_IN | 1,  ODIN3_PBIT_OUT | 0, ODIN3_PBIT_OUT | 1};
    const odin3_proc_action actions[] = {
        {{0, 1}, {1, 1}, {0}}, {{2, 1}, {3, 1}, {0}}, {{4, 1}, {5, 1}, {0}}};
    const odin3_proc_case root = {0, 0, 0, 2, 0, 0, {0}};
    const odin3_proc_sync sync = {ODIN3_SYNC_POSEDGE, ODIN3_PBIT_IN | 0, 2, 1};
    odin3_proc_body body = {pbits, 6, NULL, 0, actions, 3, &root, 1, NULL, 0, &sync, 1};
    odin3_arena *arena = st == ODIN3_OK ? odin3_arena_create(FIXTURE_ARENA_CHUNK) : NULL;
    if (st == ODIN3_OK && arena == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    odin3_value params[3] = {odin3_value_int(2), odin3_value_int(2), {0}};
    if (st == ODIN3_OK) {
        st = odin3_proc_body_pack(arena, &body, &params[ODIN3_PROC_P_BODY]);
    }
    odin3_node_spec spec = {type, 0, prov, params, 3};
    odin3_netvec ports[2] = {{&nets[0], 2}, {&nets[2], 2}}; /* I = [clk, d], O = [q, t0] */
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(module, &spec, ports, node);
    }
    odin3_arena_destroy(arena); /* node_create copied the parameters */
    return st;
}

/* A module named name with provenance prov; *out gets it. */
static odin3_status module_in(odin3_design *design, const char *name, odin3_prov_id prov,
                              odin3_module **out) {
    uint32_t str = 0;
    odin3_module_id id = {0};
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(name), &str);
    if (st == ODIN3_OK) {
        st = odin3_module_create(design, str, prov, &id);
    }
    *out = st == ODIN3_OK ? odin3_module_get(design, id) : NULL;
    return st;
}

/* A pass run "test" with a SOURCE record at t.v:1; *prov gets the record. */
static odin3_status source_prov(odin3_design *design, odin3_prov_id *prov) {
    uint32_t test = 0;
    uint32_t file = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr("test"), &test);
    if (st == ODIN3_OK) {
        st = odin3_design_intern(design, odin3_bytes_cstr("t.v"), &file);
    }
    odin3_pass_ctx ctx = {0};
    if (st == ODIN3_OK) {
        st = odin3_pass_run_begin(design, test, &ctx);
    }
    odin3_srcloc loc = {file, 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    return st == ODIN3_OK ? odin3_prov_source(&ctx, &origin, prov) : st;
}

odin3_status proc_fixture_build_design(odin3_design *design, odin3_module **module) {
    odin3_prov_id prov = {0};
    odin3_module *empty = NULL;
    odin3_node_id node = {0};
    *module = NULL;
    odin3_status st = source_prov(design, &prov);
    if (st == ODIN3_OK) {
        st = module_in(design, "e", prov, &empty);
    }
    if (st == ODIN3_OK) {
        st = module_in(design, "m", prov, module);
    }
    return st == ODIN3_OK ? proc_fixture_build_p1(*module, prov, &node) : st;
}
