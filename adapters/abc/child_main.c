/*
 * child_main.c — main() of odin3-abc, the PIC `abc` tool the core runs for non-default ABC scripts
 * (3D spec §7.9, AB-33; plan Task 9b). It is ABC's own src/base/main/main.c plus one thing: ABC's
 * `version` text does not carry the commit, so for the exact query the core makes
 * (`odin3-abc -c "version"`) it first prints a line `odin3-abc: commit <40 hex>`, the pinned
 * commit this binary was built from, which the core compares with its own pin. Everything else,
 * including the rest of that query, is ABC's. Three more things keep a run self-contained:
 * ABC's `-s` (no `.abc.rc` / `abc.rc` is sourced, so a run does not depend on the user's HOME or
 * the working directory); no core file (RLIMIT_CORE 0, set here, not around the core's spawn); and
 * death with the core (PR_SET_PDEATHSIG; the core names its pid in ODIN3_ABC_PARENT, so a parent
 * that is already gone is noticed too). The death signal follows the thread that spawned the
 * child, which the core keeps alive for the call. Built with ABC's GNU C settings (not C17) and
 * without odin3_flags, linked to libabc-pic; it includes no ABC header.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef ODIN3_ABC_COMMIT_STR
#error "ODIN3_ABC_COMMIT_STR (the pinned ABC commit) comes from CMakeLists.txt"
#endif

enum { MAX_ARGS = 6 }; /* the core passes 3; a hand-made longer line goes to ABC as it is */

static const char *const k_commit = ODIN3_ABC_COMMIT_STR;

int Abc_RealMain(int argc, char *argv[]);

static int is_version_query(int argc, const char *const argv[]) {
    return argc == 3 && strcmp(argv[1], "-c") == 0 && strcmp(argv[2], "version") == 0;
}

/* Dies with the core: the signal on its death, and an exit now if it is already gone. */
static void follow_parent(void) {
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
    const char *want = getenv("ODIN3_ABC_PARENT");
    if (want != NULL && strtol(want, NULL, 10) != (long)getppid()) {
        _exit(1);
    }
}

static void no_core_file(void) {
    struct rlimit none = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &none);
}

/* argv with `-s` after the program name; argv_out holds argc + 2 slots. */
static void with_no_rc(int argc, char *argv[], char *argv_out[]) {
    static char no_rc[] = "-s";
    argv_out[0] = argv[0];
    argv_out[1] = no_rc;
    for (int i = 1; i < argc; i++) {
        argv_out[i + 1] = argv[i];
    }
    argv_out[argc + 1] = NULL;
}

int main(int argc, char *argv[]) {
    follow_parent();
    no_core_file();
    (void)setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    if (is_version_query(argc, (const char *const *)argv)) {
        (void)fprintf(stdout, "odin3-abc: commit %s\n", k_commit);
        (void)fflush(stdout);
    }
    char *plain[MAX_ARGS + 2];
    if (argc > MAX_ARGS) {
        return Abc_RealMain(argc, argv); /* not a core call; ABC's own command line */
    }
    with_no_rc(argc, argv, plain);
    return Abc_RealMain(argc + 1, plain);
}
