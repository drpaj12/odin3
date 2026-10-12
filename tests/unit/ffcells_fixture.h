/*
 * ffcells_fixture.h — module `ffcells`: one cell of each of the eleven word flip-flops and latches
 * of IR change D (2E spec §13, plan Task 1), for the writer tests. test_rtlil_writer writes it to
 * ffcells.il and test_verilog_writer to ffcells.v; CTest ffcells_yosys_read has Yosys 0.55 read
 * both back.
 */
#ifndef ODIN3_TESTS_UNIT_FFCELLS_FIXTURE_H
#define ODIN3_TESTS_UNIT_FFCELLS_FIXTURE_H

#include "ir/design.h"
#include "ir/module.h"

#include <stdint.h>

/* The eleven type names, in built-in ID order ($adffe … $sr). */
enum { FFCELLS_COUNT = 11, FFCELLS_WIDTH = 2 };
extern const char *const ffcells_types[FFCELLS_COUNT];

/*
 * Adds module `ffcells` to design: for each type T of ffcells_types, a node `c_<T without $>`
 * with WIDTH 2, EN_POLARITY and CLR_POLARITY 0, every other polarity 1 and every *_VALUE 2'b01,
 * each of its ports on a module port `<T without $>_<port in lower case>` of the port's width
 * (Q an output, the rest inputs), so nothing is undriven. Fails the running Unity test on any
 * error; returns the module.
 */
odin3_module *ffcells_build(odin3_design *design);

#endif
