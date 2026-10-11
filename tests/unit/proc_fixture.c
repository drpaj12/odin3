/* proc_fixture.c — Task 3's P1 $proc node for the unit tests: proc_fixture_core.c, asserted. */
#include "proc_fixture.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"

#include <stddef.h>

odin3_node_id proc_fixture_p1(odin3_module *module, odin3_prov_id prov) {
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, proc_fixture_build_p1(module, prov, &node));
    return node;
}

odin3_module *proc_fixture_design(odin3_design *design) {
    odin3_module *module = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, proc_fixture_build_design(design, &module));
    return module;
}
