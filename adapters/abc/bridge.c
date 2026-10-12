/*
 * bridge.c — the adapter's only calls into ABC, and the only adapter file that includes ABC's
 * headers (bridge.h; 3D spec §4.1, §4.4, AB-4, AB-8). ABC's headers come in as SYSTEM headers
 * with ABC's own -D flags, and this file builds without -Wstrict-prototypes, because ABC declares
 * functions such as Abc_FrameCreate() without a prototype (CMakeLists.txt sets both).
 */
#include "bridge.h"

#include "guard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ABC (SYSTEM headers). main.h brings abc_global.h, then abcapis.h, cmd.h and gia.h, in the
 * order ABC needs; aig.h declares Aig_ManRandom and Aig_ManRandom64. */
#include "base/main/main.h"

#include "aig/aig/aig.h"

/*
 * AB-4's generators that the headers above do not declare (the list is part of the pin; the
 * others come from abc_global.h, aig.h and gia.h): opt/rar/rewireRng.c.
 */
extern unsigned Random_Int(int fReset);
extern word Random_Word(int fReset);

enum {
    ADP_BATCH_MODE = 1, /* Abc_FrameSetBatchMode: no abc.history, no prompts */
    ADP_RESET = 1,      /* a generator's fReset */
    ADP_LIBC_SEED = 1   /* srand's seed, which also seeds fraig (Odin III never uses rand) */
};

static Abc_Frame_t *adp_frame; /* the session's frame, NULL when none is open */
static Abc_Frame_t *adp_prev;  /* the frame entered before it */
static bool adp_entered;       /* adp_frame is the thread's current frame */

/*
 * Every AB-4 generator back to its start, and libc's rand() seeded with seed. The seed is a
 * parameter, not the literal: clang-tidy's cert-msc32-c refuses srand() with a constant, which
 * guards against predictable randomness, while here the predictable sequence is the requirement
 * (AB-8: srand(1) per session, so fraig's rand() repeats).
 */
static void adp_reset_generators(unsigned int seed) {
    (void)Abc_Random(ADP_RESET);
    (void)Abc_RandomW(ADP_RESET);
    (void)Aig_ManRandom(ADP_RESET);
    (void)Aig_ManRandom64(ADP_RESET);
    (void)Gia_ManRandom(ADP_RESET);
    (void)Gia_ManRandomW(ADP_RESET);
    (void)Random_Int(ADP_RESET);
    (void)Random_Word(ADP_RESET);
    srand(seed);
}

#ifdef ODIN3_ABC_TEST_HOOKS
static void adp_test_add_commands(Abc_Frame_t *frame);
#endif

odin3_status odin3_bridge_frame_open(void) {
    ODIN3_ADAPTER_ASSERT(adp_frame == NULL && !adp_entered);
    adp_frame = Abc_FrameCreate();
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL);
    adp_prev = Abc_FrameEnter(adp_frame);
    adp_entered = true;
    Abc_FrameSetBatchMode(ADP_BATCH_MODE);
    int set_check = Cmd_CommandExecute(adp_frame, "set check");
    ODIN3_ADAPTER_ASSERT(set_check == 0);
    adp_reset_generators(ADP_LIBC_SEED);
#ifdef ODIN3_ABC_TEST_HOOKS
    adp_test_add_commands(adp_frame);
#endif
    return ODIN3_OK;
}

void odin3_bridge_frame_close(bool destroy) {
    if (adp_entered) {
        Abc_FrameLeave(adp_prev);
        adp_entered = false;
    }
    if (destroy && adp_frame != NULL) {
        Abc_FrameDestroy(adp_frame);
    }
    adp_frame = NULL;
    adp_prev = NULL;
}

int odin3_bridge_exec(const char *command) {
    return Cmd_CommandExecute(adp_frame, command);
}

#ifdef ODIN3_ABC_TEST_HOOKS
enum { ADP_TEST_COMMANDS = 8, ADP_TEST_UNKNOWN = 1 };

static struct {
    const char *name;
    odin3_abc_test_cmd_fn fn;
} adp_test_cmds[ADP_TEST_COMMANDS];
static size_t adp_test_count;

odin3_status odin3_abc_test_add_command(const char *name, odin3_abc_test_cmd_fn fn) {
    if (name == NULL || fn == NULL || adp_test_count == ADP_TEST_COMMANDS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    adp_test_cmds[adp_test_count].name = name;
    adp_test_cmds[adp_test_count].fn = fn;
    adp_test_count++;
    return ODIN3_OK;
}

/* Every test command's ABC entry: finds its body by argv[0]. */
static int adp_test_command(Abc_Frame_t *frame, int argc, char **argv) {
    (void)frame;
    for (size_t i = 0; i < adp_test_count; i++) {
        if (argc > 0 && strcmp(argv[0], adp_test_cmds[i].name) == 0) {
            return adp_test_cmds[i].fn(argc, argv);
        }
    }
    return ADP_TEST_UNKNOWN;
}

static void adp_test_add_commands(Abc_Frame_t *frame) {
    for (size_t i = 0; i < adp_test_count; i++) {
        Cmd_CommandAdd(frame, "Odin3 test", adp_test_cmds[i].name, adp_test_command, 0);
    }
}
#endif
