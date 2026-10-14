/*
 * child_main.c — main() of odin3-abc, the PIC `abc` tool the core runs for non-default ABC scripts
 * (3D spec §7.9, AB-33; plan Task 9b). It is ABC's own src/base/main/main.c plus one thing: ABC's
 * `version` text does not carry the commit, so for the exact query the core makes
 * (`odin3-abc -c "version"`) it first prints a line `odin3-abc: commit <40 hex>`, the pinned
 * commit this binary was built from, which the core compares with its own pin. Everything else,
 * including the rest of that query, is ABC's. Built with ABC's GNU C settings (not C17) and
 * without odin3_flags, linked to libabc-pic; it includes no ABC header.
 */
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_ABC_COMMIT_STR
#error "ODIN3_ABC_COMMIT_STR (the pinned ABC commit) comes from CMakeLists.txt"
#endif

int Abc_RealMain(int argc, char *argv[]);

static int is_version_query(int argc, char *argv[]) {
    return argc == 3 && strcmp(argv[1], "-c") == 0 && strcmp(argv[2], "version") == 0;
}

int main(int argc, char *argv[]) {
    (void)setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    if (is_version_query(argc, argv)) {
        (void)printf("odin3-abc: commit %s\n", ODIN3_ABC_COMMIT_STR);
        (void)fflush(stdout);
    }
    return Abc_RealMain(argc, argv);
}
