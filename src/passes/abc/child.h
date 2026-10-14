/*
 * child.h — the subprocess path for non-default ABC scripts (3D spec §7.9, AB-33; plan Tasks 9a
 * and 9b). The core spawns `odin3-abc` (the PIC odin3_abc_exe) in its own process group, forwards
 * its merged output line by line, enforces a wall-clock limit and an output cap, and on every path
 * kills and reaps the group before it returns. It never links ABC and never logs: the caller
 * renders a failure with odin3_abc_child_message and logs it.
 */
#ifndef ODIN3_PASSES_ABC_CHILD_H
#define ODIN3_PASSES_ABC_CHILD_H

#include "odin3/odin3.h"

#include <stddef.h>
#include <stdint.h>

enum {
    ODIN3_ABC_CHILD_PATH_BYTES = 1024, /* the longest program path the result keeps */
    ODIN3_ABC_CHILD_HEX = 12           /* the commit digits a message shows */
};

/* How a child run ended. */
typedef enum odin3_abc_child_kind {
    ODIN3_ABC_CHILD_OK = 0,  /* exit status 0 */
    ODIN3_ABC_CHILD_SPAWN,   /* spawn or exec failed: code is the errno */
    ODIN3_ABC_CHILD_EXIT,    /* a non-zero exit status: code is the status */
    ODIN3_ABC_CHILD_SIGNAL,  /* killed by a signal it raised itself: code is the signal */
    ODIN3_ABC_CHILD_TIMEOUT, /* the wall-clock limit passed: the group got SIGKILL */
    ODIN3_ABC_CHILD_OUTPUT,  /* the output cap passed: the group got SIGKILL */
    ODIN3_ABC_CHILD_PIN      /* the program is not the pinned commit (odin3_abc_child_check_pin) */
} odin3_abc_child_kind;

/*
 * One child run. The child runs `program option argument` with run_dir as its working directory:
 * option "-f" and argument the absolute path of abc<r>.cmd for a run, "-c" and "version" for the
 * pin query. seconds and output_bytes are the limits (0: ODIN3_ABC_CHILD_SECONDS and
 * ODIN3_ABC_CHILD_OUTPUT_BYTES); odin3_abc_test_set_child_limits (abc_test.h) lowers them
 * further. run is the pass run's number, kept for the messages. text, when not NULL, gets every
 * output line (without its line end) as it arrives, a last line without one at the end of the
 * output, and a line longer than the read buffer cut in pieces.
 */
typedef struct odin3_abc_child_req {
    const char *program;
    const char *option;
    const char *argument;
    const char *run_dir;
    uint32_t seconds;
    uint64_t output_bytes;
    uint32_t run;
    odin3_abc_text_fn text;
    void *user;
} odin3_abc_child_req;

/*
 * What a child run reports: the kind and its code (see odin3_abc_child_kind), the output bytes
 * seen, and what odin3_abc_child_message needs, so it has no other input: the run number, the
 * limits in force, the program path (cut to the field) and, for PIN, the commit the program
 * answered with (its first ODIN3_ABC_CHILD_HEX digits, or "unknown").
 */
typedef struct odin3_abc_child_res {
    odin3_abc_child_kind kind;
    int code;
    uint64_t output_bytes;
    uint32_t run;
    uint32_t seconds;
    uint64_t output_limit;
    char program[ODIN3_ABC_CHILD_PATH_BYTES];
    char found[ODIN3_ABC_CHILD_HEX + 1];
} odin3_abc_child_res;

/*
 * Runs one child to its end. Returns ODIN3_OK when it exited with status 0 (res->kind OK),
 * ODIN3_ERR_ENGINE for any other end (res->kind says which: the group is already dead and
 * reaped), ODIN3_ERR_INVALID_ARG for a NULL or empty request field, a NUL in the request, or NULL
 * res, and ODIN3_ERR_NO_MEMORY, raised before anything is spawned. Never logs, never calls exit,
 * and leaves no process behind on any path: the group gets SIGKILL and the child is reaped even
 * after a normal exit, so a grandchild cannot outlive the call. *res is filled on every return
 * but INVALID_ARG.
 */
odin3_status odin3_abc_child_run(const odin3_abc_child_req *req, odin3_abc_child_res *res);

#endif
