/*
 * abc_planted_lib.c — a planted ABC "library extension" for test_abc_adapter (3D Task 5 review,
 * finding C1). At every frame's creation ABC's Libs_Init (base/main/libSupport.c) dlopens each
 * libabc_*.so in the working directory, or in $ABC_LIB_PATH, and calls its abc_init. The adapter
 * refuses that dlopen, so this library must never load: if it does, its constructor ends the
 * process with exit code 9, outside every guard (the reviewer's reproduction).
 */
#include <stdlib.h>

enum { PLANTED_EXIT = 9 };

/* ABC's extension entry point (lib_init_end_func). */
void abc_init(void *frame);

__attribute__((constructor)) static void planted_constructor(void) {
    exit(PLANTED_EXIT);
}

void abc_init(void *frame) {
    (void)frame;
    exit(PLANTED_EXIT);
}
