/*
 * opclass_expect.h — test helpers: count the DERIVED records of a pass's runs by their class
 * (IR-12a), so a test says "every record of `proc` is refine, stmt to word".
 */
#ifndef ODIN3_TESTS_OPCLASS_EXPECT_H
#define ODIN3_TESTS_OPCLASS_EXPECT_H

#include "ir/design.h"
#include "ir/ir_internal.h"
#include "ir/opclass.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* How many DERIVED records of runs named pass have this class (want true) or do not (false). */
static inline uint32_t opx_count(const odin3_design *design, const char *pass,
                                 const odin3_opclass *cls, bool want) {
    uint32_t found = 0;
    for (uint32_t id = 1; id < odin3_prov_end(design); id++) {
        const odin3_prov_record *rec = odin3_prov_get(design, (odin3_prov_id){id});
        if (rec->kind != ODIN3_PROV_DERIVED) {
            continue;
        }
        const char *name =
            odin3_strtab_get(odin3_design_strtab(design), odin3_passrun_name(design, rec->run));
        odin3_opclass got;
        bool has = odin3_opclass_of(design->opclass, (odin3_prov_id){id}, rec->run, rec->op, &got);
        if (name != NULL && strcmp(name, pass) == 0) {
            found += (has && odin3_opclass_equal(&got, cls)) == want ? 1U : 0U;
        }
    }
    return found;
}

/* Every DERIVED record of runs named pass has class cls, and there is at least one. */
static inline void opx_all(const odin3_design *design, const char *pass, const odin3_opclass *cls) {
    TEST_ASSERT_TRUE_MESSAGE(opx_count(design, pass, cls, true) > 0, pass);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, opx_count(design, pass, cls, false), pass);
}

#endif
