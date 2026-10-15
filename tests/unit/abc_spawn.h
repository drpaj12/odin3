/*
 * abc_spawn.h — running the odin3 CLI and equiv-check from an abc test (3D plan Task 19; static
 * inline): the tests that use the real linked ABC drive the odin3 binary (which registers the
 * adapter at start-up) and read its files back in process. ODIN3_BIN and ODIN3_EQUIV_CHECK name
 * the programs; paths must hold no blank or quote.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_SPAWN_H
#define ODIN3_TESTS_UNIT_ABC_SPAWN_H

#include "abc_fixture.h"

#include <sys/wait.h>

#ifndef ODIN3_BIN
#define ODIN3_BIN ""
#endif
#ifndef ODIN3_EQUIV_CHECK
#define ODIN3_EQUIV_CHECK ""
#endif

enum { AFX_CMD = 4096 };

/* The exit status of a shell command (-1: it did not exit normally). */
static inline int afx_shell(const char *cmd) {
    int status = system(cmd);
    return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* `odin3 -p script`, its output in <test dir>/odin3.log (printed on a nonzero status). */
static inline int afx_run_odin3(const char *script) {
    char log[AFX_PATH];
    char cmd[AFX_CMD];
    afx_path(log, "odin3.log");
    (void)snprintf(cmd, sizeof cmd, "'%s' -p '%s' >'%s' 2>&1", ODIN3_BIN, script, log);
    int rc = afx_shell(cmd);
    if (rc != 0) {
        (void)fprintf(stderr, "odin3 -p '%s': exit %d\n%s\n", script, rc, afx_slurp(log));
    }
    return rc;
}

/* `equiv-check [options] a b`, its output in <test dir>/equiv.log (printed on a nonzero status). */
static inline int afx_run_equiv(const char *a, const char *b, const char *options) {
    char log[AFX_PATH];
    char cmd[AFX_CMD];
    afx_path(log, "equiv.log");
    (void)snprintf(cmd, sizeof cmd, "'%s' %s '%s' '%s' >'%s' 2>&1", ODIN3_EQUIV_CHECK, options, a,
                   b, log);
    int rc = afx_shell(cmd);
    if (rc != 0) {
        (void)fprintf(stderr, "equiv-check %s %s %s: exit %d\n%s\n", options, a, b, rc,
                      afx_slurp(log));
    }
    return rc;
}

#endif
