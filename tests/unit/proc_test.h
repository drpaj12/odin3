/*
 * proc_test.h — the helpers the 2D process-generator unit tests share (test_elab_stage2_proc.c and
 * test_elab_stage2_case.c; Task 22 review I3), following elab_test.h: a source is read through the
 * whole read_verilog pass into the one result pt_rs (which the test's setUp zeroes and tearDown
 * frees), located diagnostics are asserted as whole lines, and a `$proc` node is read back as a
 * text dump (the pins as I<k> / O<k> in net-ID order; every case, switch and sync in IR-19's
 * canonical layout; a compare lists its bits least significant first, `-` is ANY). A failed
 * assertion is Unity's, so these run only inside a test.
 */
#ifndef ODIN3_TESTS_UNIT_PROC_TEST_H
#define ODIN3_TESTS_UNIT_PROC_TEST_H

#include "elab_test.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"

#include <stdint.h>

/* The result of the last read. */
extern et_result pt_rs;

/* The pass's arguments and the text of t.v. */
typedef struct pass_src {
    const char *args;
    const char *text;
} pass_src;

/* A line and column of t.v. */
typedef struct at_loc {
    uint32_t line, col;
} at_loc;

/* --- reading ---------------------------------------------------------------------------------- */

/* Runs the read_verilog pass with src->args on a fresh design holding t.v (pt_rs.status). */
void pt_read_with(const pass_src *src);
/* pt_read_with of "t.v" and text. */
void pt_read_src(const char *text);
/* The full IR check passes. */
void pt_expect_ir_ok(void);
/* Errors that are located diagnostics: not the pass manager's own "failed" line. */
uint32_t pt_diag_errors(void);
/* The warnings of 2D: not the IR check's. */
uint32_t pt_soft_warnings(void);
/* A read that succeeds with no error, no warning and a valid IR. */
void pt_read_clean(const char *text);
/* A read that fails with ODIN3_ERR_PARSE and leaves a valid, incomplete IR (EL-2). */
void pt_read_failing(const char *text);
/* The whole diagnostic line "t.v:<line>:<col>: warning|error: msg" is in the log. */
void pt_expect_warning(at_loc at, const char *msg);
void pt_expect_error(at_loc at, const char *msg);
/* The warnings whose text contains needle. */
uint32_t pt_warnings_with(const char *needle);

/* --- reading the IR back ---------------------------------------------------------------------- */

/* The string-table id of text, and the text of an id. */
uint32_t pt_id_of(const char *text);
const char *pt_str_of(uint32_t id);
/* The module t. */
odin3_module *pt_top(void);
/* The net of a bit of a wire of t. */
odin3_net_id pt_wnet(const char *name, uint32_t bit);
/* The nth live node of a cell type of t (id 0: none) and the number of them. */
odin3_node_id pt_nth_node(const char *type, uint32_t nth);
uint32_t pt_count_of(const char *type);
/* The nth `$proc` node (the test fails when there is none). */
odin3_node_id pt_proc_node(uint32_t nth);
/* The net on pin `index` of the O port of a `$proc` node. */
odin3_net_id pt_out_net(odin3_node_id node, uint32_t index);
/* The body of a `$proc` node, viewed into *storage. */
const odin3_proc_body *pt_body_of(odin3_node_id node, odin3_proc_body *storage);
/* The constant net of bit (the shared $_CONST*_ node's Y). */
odin3_net_id pt_const_net_of(odin3_bit bit);

/* --- the dump of a body ----------------------------------------------------------------------- */

/* "I<w> O<w>", a line per case, switch and sync; the buffer is static. */
const char *pt_dump(odin3_node_id node);
/* pt_dump of the nth `$proc` node is want. */
void pt_expect_dump(uint32_t nth, const char *want);
/* The net on pin `index` of the I / O port of a `$proc` node is want. */
void pt_expect_in(odin3_node_id node, uint32_t index, odin3_net_id want);
void pt_expect_out(odin3_node_id node, uint32_t index, odin3_net_id want);

#endif
