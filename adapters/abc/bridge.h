/*
 * bridge.h — the adapter's only calls into ABC (3D spec §4.1, §4.4). ABC's types stay inside
 * bridge.c, the one adapter file that includes ABC's headers. Every function here must run under
 * odin3_guard_run (guard.h), on the session's thread, between frame_open and frame_close.
 */
#ifndef ODIN3_ADAPTERS_ABC_BRIDGE_H
#define ODIN3_ADAPTERS_ABC_BRIDGE_H

#include "odin3/odin3.h"

#include <stdbool.h>

/*
 * A fresh ABC frame, entered on this thread: Abc_FrameCreate, Abc_FrameEnter, batch mode,
 * "set check" (VTR's abc.rc setting, which changes no result), every AB-4 generator reset with
 * (1), and srand(1). The frame stays entered until frame_close, so ABC never makes its lazy
 * global frame. ODIN3_ERR_ENGINE (an adapter assertion) when a frame is already open, when ABC
 * returns no frame, or when "set check" fails.
 */
odin3_status odin3_bridge_frame_open(void);

/*
 * Leaves the frame (back to the frame entered before it) and, when destroy is true, destroys it;
 * after an escape the caller passes false and the frame's state is leaked. No frame: nothing.
 */
void odin3_bridge_frame_close(bool destroy);

/* Cmd_CommandExecute on the open frame: ABC's result, 0 on success. */
int odin3_bridge_exec(const char *command);

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
#endif

#endif
