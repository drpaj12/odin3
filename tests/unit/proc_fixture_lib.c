/*
 * proc_fixture_lib.c — the test-only odin3_procfix library: libodin3's objects plus
 * odin3_test_proc_design, so the Python binding can walk a $proc node through the ABI before any
 * front end makes one (2D Task 6). Never installed; libodin3's own exports are unchanged.
 */
#include "proc_fixture.h"

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/attr.h"

#include <stddef.h>

ODIN3_EXPORT odin3_design *odin3_test_proc_design(void) {
    odin3_design *design = odin3_design_create();
    odin3_module *module = NULL;
    if (design != NULL && proc_fixture_build_design(design, &module) != ODIN3_OK) {
        odin3_design_destroy(design);
        design = NULL;
    }
    return design;
}
