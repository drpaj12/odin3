/*
 * abc_spawn.h — running the odin3 CLI and equiv-check from an abc test (3D plan Task 19; static
 * inline): the tests that use the real linked ABC drive the odin3 binary (which registers the
 * adapter at start-up) and read its files back in process. ODIN3_BIN and ODIN3_EQUIV_CHECK name
 * the programs. Each program is started with posix_spawn (no shell), its output in a log file.
 */
#ifndef ODIN3_TESTS_UNIT_ABC_SPAWN_H
#define ODIN3_TESTS_UNIT_ABC_SPAWN_H

#include "abc_fixture.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ODIN3_BIN
#define ODIN3_BIN ""
#endif
#ifndef ODIN3_EQUIV_CHECK
#define ODIN3_EQUIV_CHECK ""
#endif

enum { AFX_CMD_TEXT = 4096 };

extern char **environ;

/* Runs argv[0] with argv (NULL-ended), stdout and stderr in the file log; the exit status, or -1
 * when the program cannot start or does not exit normally. */
static inline int afx_spawn_logged(char *const argv[], const char *log) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return -1;
    }
    int err = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log,
                                               O_WRONLY | O_CREAT | O_TRUNC, 0644);
    err = err == 0 ? posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) : err;
    pid_t pid = 0;
    err = err == 0 ? posix_spawn(&pid, argv[0], &actions, NULL, argv, environ) : err;
    (void)posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (err != 0 || waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* `odin3 -p script`, its output in <test dir>/odin3.log (printed on a nonzero status). */
static inline int afx_run_odin3(const char *script) {
    char log[AFX_PATH];
    afx_path(log, "odin3.log");
    char program[] = ODIN3_BIN;
    char option[] = "-p";
    char text[AFX_CMD_TEXT];
    (void)snprintf(text, sizeof text, "%s", script);
    char *const argv[] = {program, option, text, NULL};
    int rc = afx_spawn_logged(argv, log);
    if (rc != 0) {
        (void)fprintf(stderr, "odin3 -p '%s': exit %d\n%s\n", script, rc, afx_slurp(log));
    }
    return rc;
}

/* `equiv-check [option] first second`, its output in <test dir>/equiv.log (printed on a nonzero
 * status). option may be "" or one word. */
static inline int afx_run_equiv(const char *first_blif, const char *second_blif,
                                const char *option) {
    char log[AFX_PATH];
    afx_path(log, "equiv.log");
    char program[] = ODIN3_EQUIV_CHECK;
    char word[AFX_PATH];
    char first[AFX_PATH];
    char second[AFX_PATH];
    (void)snprintf(word, sizeof word, "%s", option);
    (void)snprintf(first, sizeof first, "%s", first_blif);
    (void)snprintf(second, sizeof second, "%s", second_blif);
    char *const with_option[] = {program, word, first, second, NULL};
    char *const without_option[] = {program, first, second, NULL};
    int rc = afx_spawn_logged(option[0] != '\0' ? with_option : without_option, log);
    if (rc != 0) {
        (void)fprintf(stderr, "equiv-check %s %s %s: exit %d\n%s\n", option, first_blif,
                      second_blif, rc, afx_slurp(log));
    }
    return rc;
}

#endif
