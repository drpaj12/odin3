/*
 * session.h — the linked engine's session and the fd capture behind it (3D spec §4.4, AB-8; plan
 * point (o): one header for the session and the capture).
 *
 * The linked engine has one static session, process-wide. open makes a fresh ABC frame on the
 * calling thread, which every later call of the session must use; close leaves and destroys it.
 * Every ABC call runs under odin3_guard_run (guard.h), with fds 1 and 2 captured into the
 * session's file: ABC prints through printf and friends, never through the frame's streams.
 */
#ifndef ODIN3_ADAPTERS_ABC_SESSION_H
#define ODIN3_ADAPTERS_ABC_SESSION_H

#include "odin3/odin3.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * fds 1 and 2 captured into one file. file is the capture file (its descriptor is above 2 and
 * close-on-exec); saved_out and saved_err hold the original fds 1 and 2 between begin and end
 * (-1 otherwise, or when that fd was closed); read_at is where the next delivery starts reading:
 * 0 between calls, because a pending partial line is moved to the start of the file.
 */
typedef struct odin3_capture {
    FILE *file;
    int saved_out, saved_err;
    long read_at;
} odin3_capture;

/*
 * Creates cap's capture file. ODIN3_ERR_NO_MEMORY when the file or its descriptor cannot be made
 * (cap->file is then NULL). The caller owns the file until odin3_capture_close.
 */
odin3_status odin3_capture_open(odin3_capture *cap);

/* Closes cap's capture file (a NULL file is ignored). Never fails. */
void odin3_capture_close(odin3_capture *cap);

/*
 * Flushes stdout and stderr, saves fds 1 and 2, and dup2s both onto cap->file. On failure fds 1
 * and 2 are as they were and the status is ODIN3_ERR_NO_MEMORY (no descriptor left), or
 * ODIN3_ERR_ENGINE through ODIN3_ADAPTER_ASSERT when cap has no file.
 */
odin3_status odin3_capture_begin(odin3_capture *cap);

/*
 * Flushes stdout and stderr (into the capture), restores fds 1 and 2, then hands every complete
 * new line to text (user first; without its newline and trailing blanks, valid during the call
 * only; a line longer than 4,095 bytes arrives in pieces). A partial last line waits for the next
 * call; the file is truncated to it. text NULL drops the lines. Never fails: an unreadable capture
 * loses its lines.
 */
void odin3_capture_end(odin3_capture *cap, odin3_abc_text_fn text, void *user);

/* The session's self pointer, which the engine table carries and every session call checks. */
void *odin3_abc_session_self(void);

/*
 * The engine's open (odin3.h, §4.3): fresh ABC state and generators, srand(1), output to text
 * (NULL: dropped). ODIN3_ERR_ENGINE with fail BUSY while a session is open, DISABLED after an
 * escape, or EXIT/ABORT/ASSERT from the frame's set-up; ODIN3_ERR_NO_MEMORY without a capture
 * file. Must be followed by close on the same thread when it returns ODIN3_OK.
 */
odin3_status odin3_abc_session_open(void *self, odin3_abc_text_fn text, void *user,
                                    odin3_abc_fail *fail);

/*
 * The engine's exec: runs one already-checked command line as given. ODIN3_ERR_ENGINE with fail
 * COMMAND (code = ABC's non-zero result), EXIT, ABORT or ASSERT, or DISABLED after an escape.
 */
odin3_status odin3_abc_session_exec(void *self, const char *command, odin3_abc_fail *fail);

/*
 * The engine's load_aig (§4.4 step 3, AB-16): checks a copy of aig (object 0 the constant, then
 * the PIs, the ANDs with 0 <= lit0 < lit1 < 2 * index, the POs on earlier objects; n_regs at most
 * the PI and the PO count), loads it into the & space (Abc_FrameGiaInputMiniAig) and names CI n
 * "c<n>". ODIN3_ERR_ENGINE with fail RESULT naming the first bad object (ABC is not called),
 * EXIT/ABORT/ASSERT or DISABLED; ODIN3_ERR_NO_MEMORY when the copy cannot be made.
 */
odin3_status odin3_abc_session_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail);

/*
 * The engine's get_luts (§4.4 step 5): the & space's mapping as ABC's MiniLUT
 * (Abc_FrameGiaOutputMiniLut), with ci_from parsed from the GIA's CI names ("c<n>" gives n,
 * anything else -1; NULL when the GIA has no CI names). *luts points into memory the session owns
 * until the next get_luts or close. ODIN3_ERR_ENGINE with fail RESULT "no mapping in the & space"
 * (no GIA, or one without a mapping) or "a LUT with N inputs (at most 16)", EXIT/ABORT/ASSERT or
 * DISABLED; ODIN3_ERR_NO_MEMORY when ci_from cannot be made.
 */
odin3_status odin3_abc_session_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail);

/*
 * The engine's name_map (§4.4 step 6, §6.3): checks a copy of aligned (lut_size 2 to 16, every
 * fanin on an earlier object that is not a PO, and the loaded AIG's PI, PO and register counts),
 * then Abc_FrameGiaInputMiniLut2 and Abc_FrameReadMiniLutNameMapping. *lits gets one MiniAIG
 * literal or -1 per aligned object, owned by the session until the next name_map or close.
 * ODIN3_ERR_ENGINE with fail RESULT (no AIG loaded, a malformed aligned network, other counts),
 * EXIT/ABORT/ASSERT or DISABLED; ODIN3_ERR_NO_MEMORY when the copy cannot be made.
 */
odin3_status odin3_abc_session_name_map(void *self, const odin3_abc_luts *aligned,
                                        const int32_t **lits, odin3_abc_fail *fail);

/*
 * The engine's load_genlib (gatemap spec 4.2, ABI 5): text (a NUL-terminated genlib, copied)
 * becomes the session's gate library (Mio_LibraryReadBuffer, then Mio_UpdateGenlib; bridge.h).
 * Only the canonical genlib subset (session.c, adp_check_genlib) reaches ABC, and only before the
 * session's first exec. ODIN3_ERR_ENGINE with fail RESULT "ABC cannot read the gate library
 * (line N: reason)" (or "(gate \"name\")", or no detail when ABC itself refuses), "load the gate
 * library before the mapping script" or "no gate library (text is NULL)", EXIT/ABORT/ASSERT or
 * DISABLED; ODIN3_ERR_NO_MEMORY when a copy cannot be made.
 */
odin3_status odin3_abc_session_load_genlib(void *self, const char *text, odin3_abc_fail *fail);

/*
 * The engine's get_cells (gatemap spec 4.3, ABI 5): the gate-mapped network of space
 * (odin3_abc_cells_space) in odin3_abc_cells' layout, owned by the session until the next
 * get_cells, load_genlib or close. ODIN3_ERR_ENGINE with fail RESULT ("nowhere to put the cells
 * (cells is NULL)", "no network space N", or one of odin3_bridge_get_cells' texts),
 * EXIT/ABORT/ASSERT or DISABLED; ODIN3_ERR_NO_MEMORY when an array cannot be made.
 */
odin3_status odin3_abc_session_get_cells(void *self, uint32_t space, odin3_abc_cells *cells,
                                         odin3_abc_fail *fail);

/*
 * The engine's close: leaves the frame, destroys it unless an escape happened (its state is then
 * leaked, and the engine stays disabled), frees what get_luts, name_map and get_cells returned,
 * closes the capture file and ends the session.
 */
void odin3_abc_session_close(void *self);

/*
 * The adapter's own allocations: the copies load_aig, name_map and load_genlib make, get_luts'
 * ci_from and get_cells' arrays (malloc; the caller frees). NULL when out of memory, or when the
 * test hook says so.
 */
void *odin3_abc_alloc(size_t bytes);

/* The capture file (tmpfile()): NULL on failure, or when the test hook says so. */
FILE *odin3_abc_tmpfile(void);

#ifdef ODIN3_ABC_TEST_HOOKS
/*
 * Test builds only (§13.6): after n more successes, the next odin3_abc_alloc or
 * odin3_abc_tmpfile fails once; UINT32_MAX disarms the hook.
 */
void odin3_abc_test_fail_alloc_after(uint32_t n);
#endif

#endif
