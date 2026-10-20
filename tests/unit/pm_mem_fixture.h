/*
 * pm_mem_fixture.h — shared helpers of the 4.4 memory tests (plan Tasks 9 and 10): a RAM instance
 * or a `$mem` built as RTLIL on the library (and, when asked, the record) of an arch XML, a
 * partial_map run with check FULL around it, and the before/after comparison on the RAM simulator
 * (hardmap_ramsim.c). Test code: it asserts on every call.
 */
#ifndef ODIN3_TESTS_UNIT_PM_MEM_FIXTURE_H
#define ODIN3_TESTS_UNIT_PM_MEM_FIXTURE_H

#include "passes/partial_map/hardmap.h"
#include "proc_build.h"

#include <stdbool.h>
#include <stdint.h>

/* What to build. kind: an explicit RAM instance, or an inferred `$mem` with one synchronous read
 * port and one write port. dual: dual_port_ram (instance), or a `$mem` whose read and write
 * addresses are different wires (so it needs a DP block). Wires: clk, addr (write address), data,
 * we, out; dual adds the other port's wires (instance: addr1/addr2, data1/data2, we1/we2, out1/
 * out2; `$mem`: raddr). */
typedef enum pmf_kind { PMF_INSTANCE, PMF_MEM } pmf_kind;

typedef struct pmf_case {
    pmf_kind kind;
    bool dual;
    uint32_t addr; /* address bits */
    uint32_t data; /* data bits */
    bool en;       /* `$mem`: the read enable is the input wire `ren` (else constant 1) */
} pmf_case;

/* Builds c into *out on the library of arch_xml (a path) with its record attached; NULL: the k6
 * tech library lib/vtr.o3lib and no record. The last module is the top. */
void pmf_build(pmf_case cs, const char *arch_xml, pb_result *out);

/* As pmf_build with NULL (the k6 tech library read, so every hard cell type is registered), then
 * the record of arch_xml attached: the case of `read_techlib` followed by an arch that lacks a
 * model. */
void pmf_build_lib_record(pmf_case cs, const char *arch_xml, pb_result *out);

/* odin3_partial_map_run_opts on res with check FULL before and after; returns its status. opts and
 * stats may be NULL. */
odin3_status pmf_run(pb_result *res, const pm_options *opts, odin3_hardmap_stats *stats);

/* The simulation of the case before and after partial_map on the same random vectors: true when
 * the traces are equal. addr_mask is the set of address bits that are driven (the others stay 0)
 * so that a large memory is read back. Asserts that the run is OK and that data is read back. */
bool pmf_sim_equal(pmf_case cs, const char *arch_xml, const pm_options *opts, uint32_t addr_mask);

#endif
