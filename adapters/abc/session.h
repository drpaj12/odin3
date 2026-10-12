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
 * The engine's close: leaves the frame, destroys it unless an escape happened (its state is then
 * leaked, and the engine stays disabled), closes the capture file and ends the session.
 */
void odin3_abc_session_close(void *self);

#endif
