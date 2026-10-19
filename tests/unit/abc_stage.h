/*
 * abc_stage.h — a run staged up to the splice, for the tests that call the splice, the records or
 * the anchors directly (3D plan Tasks 17 and 18; static inline): the pass context, a started run
 * on module `mod` (default script, --lut 6), the first clock domain's region, and the folded
 * result of the BLIF exchange on the fake engine.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_STAGE_H
#define ODIN3_TESTS_UNIT_ABC_STAGE_H

#include "abc_fixture.h"
#include "passes/abc/exchange.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"

typedef struct afx_staged {
    odin3_pass_ctx ctx;
    odin3_abc_run *run;
    odin3_vec domains;
    odin3_abc_region region;
    odin3_abc_result result;
} afx_staged;

/* The run, the first domain and its region, without any exchange (the result is left empty). */
static inline void afx_stage_region(odin3_design *design, odin3_module *mod, afx_staged *st) {
    *st = (afx_staged){0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(design, afx_intern(design, "abc"), &st->ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_abc_run_start(&st->ctx, odin3_bytes_cstr("--lut 6"), &st->run));
    st->run->module = mod;
    st->run->module_id = odin3_design_top(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(st->run, mod, &st->domains));
    st->run->domain = st->domains.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_region_build(st->run, st->run->domain, &st->region));
    st->run->region = &st->region;
    odin3_abc_result_init(&st->result);
}

static inline void afx_stage(odin3_design *design, odin3_module *mod, afx_staged *st) {
    afx_stage_region(design, mod, st);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_exchange_blif(st->run, &st->result));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&st->result));
}

/* The same through the in-memory exchange (the run's exchange field is set to AIG). */
static inline void afx_stage_aig(odin3_design *design, odin3_module *mod, afx_staged *st) {
    afx_stage_region(design, mod, st);
    st->run->exchange = ODIN3_ABC_EXCHANGE_AIG;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_abc_exchange_aig(st->run, &st->result),
                                  afx_errors());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&st->result));
}

static inline void afx_unstage(afx_staged *st) {
    odin3_abc_result_free(&st->result);
    odin3_abc_region_free(&st->region);
    odin3_abc_domains_free(&st->domains);
    odin3_abc_run_destroy(st->run);
}

#endif
