/*
 * read_verilog_test.h — a hidden test hook for the read_verilog reader (never part of the ABI,
 * never called outside tests/unit).
 */
#ifndef ODIN3_FRONTENDS_VERILOG_READ_VERILOG_TEST_H
#define ODIN3_FRONTENDS_VERILOG_READ_VERILOG_TEST_H

#include "frontends/verilog/read_verilog.h"

/* Called by odin3_read_verilog_files after the store is sealed and before its check (§4.2
 * step 7), with the run (its store and its preprocessor still alive). */
typedef void (*odin3_rv_test_hook)(odin3_rv *rv, void *user);

/* Installs fn (NULL removes it); process-wide, single-threaded. */
void odin3_rv_test_set_hook(odin3_rv_test_hook fn, void *user);

#endif
