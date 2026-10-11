/*
 * parser_test.h — hidden test hooks for the Verilog parser and lexer (never part of the ABI,
 * never called outside tests/unit): cap overrides (2C spec §5).
 */
#ifndef ODIN3_FRONTENDS_VERILOG_PARSER_TEST_H
#define ODIN3_FRONTENDS_VERILOG_PARSER_TEST_H

#include "frontends/verilog/parser.h"

/*
 * Replaces the run's caps so a test can hit them cheaply. Each value is clamped to its default
 * (odin3_vl_run_init); 0 keeps the default. yymaxdepth is raised to at least
 * ODIN3_VP_YYINITDEPTH: Bison checks the cap only when its stack grows past the first one, so a
 * lower cap would not be exact. NULL run is a no-op.
 */
void odin3_verilog_test_set_limits(odin3_vl_run *run, odin3_vp_limits limits);

#endif
