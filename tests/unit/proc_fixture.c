/* proc_fixture.c — Task 3's P1 $proc node for the pass-manager and writer tests. */
#include "proc_fixture.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "unity.h"
#include "util/arena.h"
#include "util/str.h"

#include <stdint.h>

enum { FIXTURE_ARENA_CHUNK = 1024 };

static uint32_t intern_in(odin3_design *design, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_net_id wire_net(odin3_module *module, const char *name, odin3_prov_id prov) {
    odin3_wire_spec spec = {intern_in(odin3_module_design(module), name), 0, 0, false, prov};
    odin3_wire_id wire = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_wire_create(module, &spec, NULL, &wire));
    return odin3_wire_net(module, wire, 0);
}

odin3_node_id proc_fixture_p1(odin3_module *module, odin3_prov_id prov) {
    odin3_design *design = odin3_module_design(module);
    odin3_net_id in[2] = {wire_net(module, "clk", prov), wire_net(module, "d", prov)};
    odin3_net_id out[2] = {wire_net(module, "q", prov), wire_net(module, "t0", prov)};
    const odin3_pbit pbits[] = {ODIN3_PBIT_OUT | 1, ODIN3_PBIT_OUT | 0, ODIN3_PBIT_OUT | 1,
                                ODIN3_PBIT_IN | 1,  ODIN3_PBIT_OUT | 0, ODIN3_PBIT_OUT | 1};
    const odin3_proc_action actions[] = {
        {{0, 1}, {1, 1}, {0}}, {{2, 1}, {3, 1}, {0}}, {{4, 1}, {5, 1}, {0}}};
    const odin3_proc_case root = {0, 0, 0, 2, 0, 0, {0}};
    const odin3_proc_sync sync = {ODIN3_SYNC_POSEDGE, ODIN3_PBIT_IN | 0, 2, 1};
    odin3_proc_body body = {pbits, 6, NULL, 0, actions, 3, &root, 1, NULL, 0, &sync, 1};
    uint32_t name = 0;
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("$proc"), &name));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, name, &type));
    /* no assertion while the arena lives, so a failure cannot leak it */
    odin3_arena *arena = odin3_arena_create(FIXTURE_ARENA_CHUNK);
    odin3_value params[3] = {odin3_value_int(2), odin3_value_int(2), {0}};
    odin3_status st = arena == NULL
                          ? ODIN3_ERR_NO_MEMORY
                          : odin3_proc_body_pack(arena, &body, &params[ODIN3_PROC_P_BODY]);
    odin3_node_spec spec = {type, 0, prov, params, 3};
    odin3_netvec ports[2] = {{in, 2}, {out, 2}};
    odin3_node_id node = {0};
    if (st == ODIN3_OK) {
        st = odin3_node_create_connected(module, &spec, ports, &node);
    }
    odin3_arena_destroy(arena); /* node_create copied the parameters */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return node;
}

static odin3_module *module_in(odin3_design *design, const char *name, odin3_prov_id prov) {
    odin3_module_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern_in(design, name), prov, &id));
    return odin3_module_get(design, id);
}

odin3_module *proc_fixture_design(odin3_design *design) {
    odin3_pass_ctx ctx = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern_in(design, "test"), &ctx));
    odin3_srcloc loc = {intern_in(design, "t.v"), 1, 1, 1, 2, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id prov = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&ctx, &origin, &prov));
    (void)module_in(design, "e", prov);
    odin3_module *module = module_in(design, "m", prov);
    (void)proc_fixture_p1(module, prov);
    return module;
}
