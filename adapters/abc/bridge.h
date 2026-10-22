/*
 * bridge.h — the adapter's only calls into ABC (3D spec §4.1, §4.4). ABC's types stay inside
 * bridge.c, the one adapter file that includes ABC's headers. Every function here must run under
 * odin3_guard_run (guard.h), on the session's thread, between frame_open and frame_close; each
 * starts with ODIN3_ADAPTER_ASSERT(odin3_guard_active_here()), so one called outside the guard
 * returns ODIN3_ERR_ENGINE (an adapter assertion) before it reaches ABC.
 */
#ifndef ODIN3_ADAPTERS_ABC_BRIDGE_H
#define ODIN3_ADAPTERS_ABC_BRIDGE_H

#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A fresh ABC frame, entered on this thread: Abc_FrameCreate, Abc_FrameEnter, batch mode,
 * "set check" (VTR's abc.rc setting, which changes no result), every AB-4 generator reset with
 * (1), and srand(1). The frame stays entered until frame_close, so ABC never makes its lazy
 * global frame. ODIN3_ERR_ENGINE (an adapter assertion) when a frame is already open, when ABC
 * returns no frame, or when "set check" fails.
 */
odin3_status odin3_bridge_frame_open(void);

/*
 * Frees what get_luts, name_map and get_cells returned, leaves the frame (back to the frame entered
 * before it) and, when destroy is true, destroys it; after an escape the caller passes false and
 * the frame's state is leaked. No frame: only the frees. ODIN3_OK, or ODIN3_ERR_ENGINE outside a
 * guarded call.
 */
odin3_status odin3_bridge_frame_close(bool destroy);

/* Cmd_CommandExecute on the open frame: *rc gets ABC's result, 0 on success. ODIN3_OK, or
 * ODIN3_ERR_ENGINE (an adapter assertion) outside a guarded call. */
odin3_status odin3_bridge_exec(const char *command, int *rc);

/*
 * A checked MiniAIG (session.c checked it: odin3_abc_aig's layout, objs holding 2 * n_objs ints,
 * read only) into the & space: Abc_FrameGiaInputMiniAig, which also keeps it as the name map's
 * reference, then the GIA's vNamesIn set to "c<n>" for CI n (AB-16).
 */
odin3_status odin3_bridge_load_aig(int32_t *objs, uint32_t n_objs, uint32_t n_regs);

/*
 * The & space's mapping as a MiniLUT (Abc_FrameGiaOutputMiniLut) into *luts, with ci_from from
 * the GIA's vNamesIn ("c<n>" gives n, anything else -1; NULL without names). The arrays stay valid
 * until the next get_luts or frame_close. ODIN3_ERR_ENGINE with fail RESULT when the & space has
 * no mapped GIA or a LUT wider than 16; ODIN3_ERR_NO_MEMORY when ci_from cannot be made.
 */
odin3_status odin3_bridge_get_luts(odin3_abc_luts *luts, odin3_abc_fail *fail);

/* An aligned MiniLUT that session.c checked and owns (odin3_abc_luts' layout, writable). */
typedef struct odin3_bridge_luts {
    int32_t *fanins;
    uint32_t *truths;
    uint32_t n_objs;
    uint32_t n_regs;
    uint32_t lut_size;
} odin3_bridge_luts;

/*
 * ABC's name map of aligned against the loaded AIG: Abc_FrameGiaInputMiniLut2, then
 * Abc_FrameReadMiniLutNameMapping. *lits (one MiniAIG literal or -1 per aligned object) stays
 * valid until the next name_map or frame_close. ODIN3_ERR_ENGINE with fail RESULT when ABC makes
 * no map.
 */
odin3_status odin3_bridge_name_map(const odin3_bridge_luts *aligned, const int32_t **lits,
                                   odin3_abc_fail *fail);

/*
 * Gate mapping (ABI 5): a copy of text (NUL-terminated genlib; ABC's reader edits its buffer) read
 * with Mio_LibraryReadBuffer, named "odin3.genlib" (no file of that name is opened), then made the
 * frame's gate library with Mio_UpdateGenlib, which frees the previous one (and with it the names
 * an earlier get_cells returned). ODIN3_ERR_ENGINE with fail RESULT "ABC cannot read the gate
 * library"; ODIN3_ERR_NO_MEMORY when the copy cannot be made.
 */
odin3_status odin3_bridge_load_genlib(const char *text, odin3_abc_fail *fail);

/*
 * The gate-mapped network of space (odin3_abc_cells_space) into *cells, in odin3_abc_cells'
 * layout: the network space's mapped network, or the & space's cell mapping converted with
 * Abc_NtkFromCellMappedGia (deleted again before the call returns). Objects in Abc_NtkDfsIter
 * order; a constant node (no fanins) is object 0 or 1 by its gate's truth; names point into the
 * gate library. ci_from: the network's CI names (network space) or the GIA's (& space), parsed as
 * for get_luts. The arrays stay valid until the next get_cells or frame_close. ODIN3_ERR_ENGINE
 * with fail RESULT "no gate-mapped network in the network space", "no cell mapping in the &
 * space", "a box that is not a latch", "a node without a library gate", "a gate with N inputs (at
 * most 6)"; ODIN3_ERR_NO_MEMORY when an array cannot be made.
 */
odin3_status odin3_bridge_get_cells(uint32_t space, odin3_abc_cells *cells, odin3_abc_fail *fail);

#ifdef ODIN3_ABC_TEST_HOOKS
/* Test builds only: the body of a test command, with ABC's argc/argv (argv[0] is its name). */
typedef int (*odin3_abc_test_cmd_fn)(int argc, char **argv);

/*
 * Test builds only: every frame opened from now on gets the ABC command name (added with
 * Cmd_CommandAdd at frame open), which runs fn. The tests define o3test_exit, o3test_assert and
 * o3test_thread_exit this way. name must outlive the process. ODIN3_ERR_INVALID_ARG for a NULL
 * argument or past 8 commands.
 */
odin3_status odin3_abc_test_add_command(const char *name, odin3_abc_test_cmd_fn fn);

/*
 * Test builds only: ABC's CmdSplitLine on line's first command, as Cmd_CommandExecute splits it
 * (3D Task 11 compares it with the script checker's words). Returns argc and sets *argv (argc
 * malloc'd strings in a malloc'd array; the caller frees each, then the array). Needs an open
 * session (its frame takes ABC's warnings); -1 without one. Unlike the bridge functions it runs
 * outside the guard: it only splits, and ABC_ALLOC is all it calls that can fail.
 */
int odin3_abc_test_split(const char *line, char ***argv);

/*
 * Test builds only: names CI ci of the & space's GIA (its vNamesIn entry, which must exist), so
 * a test can give get_luts a name that is not canonical c<n>. Runs under the guard like any
 * bridge call. ODIN3_ERR_INVALID_ARG without such a GIA, CI or name.
 */
odin3_status odin3_abc_test_name_ci(uint32_t ci, const char *name);

/*
 * Test builds only: the pthread_create calls made in this process so far, by ABC or anyone else
 * (the test link wraps pthread_create with a counter; §9: the default script starts none).
 */
uint32_t odin3_abc_test_thread_count(void);
#endif

#endif
