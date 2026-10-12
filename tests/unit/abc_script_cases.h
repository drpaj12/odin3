/*
 * abc_script_cases.h — the scripts test_abc_script accepts (3D plan Task 11), shared so that the
 * adapter's checked_is_what_runs (point (p), with 3D-2) splits exactly the lines the checker
 * produced from them through ABC's CmdSplitLine.
 */
#ifndef ODIN3_TESTS_ABC_SCRIPT_CASES_H
#define ODIN3_TESTS_ABC_SCRIPT_CASES_H

#include <stddef.h>
#include <stdint.h>

/* One accepted script: its text, --lut K (0: none) and exchange (odin3_abc_exchange). */
typedef struct abc_script_case {
    const char *text;
    uint32_t lut_k;
    uint32_t exchange;
} abc_script_case;

extern const abc_script_case k_abc_script_accepted[];
extern const size_t k_abc_script_accepted_count;

#endif
