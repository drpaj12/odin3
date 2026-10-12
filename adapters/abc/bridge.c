/*
 * bridge.c — the adapter's only calls into ABC, and the only adapter file that includes ABC's
 * headers (bridge.h; 3D spec §4.1, §4.4, AB-4, AB-8). ABC's headers come in as SYSTEM headers
 * with ABC's own -D flags, and this file builds without -Wstrict-prototypes, because ABC declares
 * functions such as Abc_FrameCreate() without a prototype (CMakeLists.txt sets both).
 */
#include "bridge.h"

#include "guard.h"
#include "session.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ABC (SYSTEM headers). main.h brings abc_global.h, then abcapis.h, cmd.h and gia.h, in the
 * order ABC needs; aig.h declares Aig_ManRandom and Aig_ManRandom64; miniaig.h and minilut.h are
 * the exchange layouts odin3.h's odin3_abc_aig and odin3_abc_luts equal (§4.3). */
#include "base/main/main.h"

#include "aig/aig/aig.h"
#include "aig/miniaig/miniaig.h"
#include "aig/miniaig/minilut.h"

/*
 * AB-4's generators that the headers above do not declare (the list is part of the pin; the
 * others come from abc_global.h, aig.h and gia.h): opt/rar/rewireRng.c.
 */
extern unsigned Random_Int(int fReset);
extern word Random_Word(int fReset);

/* The name map (aig/gia/giaMini.c), which abcapis.h does not declare (3D-0 spike finding). */
extern int *Abc_FrameReadMiniLutNameMapping(Abc_Frame_t *pAbc);

/* The exchange arrays are handed to ABC as they are: its int and unsigned are 32 bits here. */
_Static_assert(sizeof(int) == sizeof(int32_t), "MiniAIG and MiniLUT literals are int32_t");
_Static_assert(sizeof(unsigned) == sizeof(uint32_t), "MiniLUT truth words are uint32_t");

enum {
    ADP_BATCH_MODE = 1,      /* Abc_FrameSetBatchMode: no abc.history, no prompts */
    ADP_RESET = 1,           /* a generator's fReset */
    ADP_LIBC_SEED = 1,       /* srand's seed, which also seeds fraig (Odin III never uses rand) */
    ADP_LUT_MAX_INPUTS = 16, /* MiniLUT's widest LUT (Mini_LutStart) */
    ADP_CI_NAME_BYTES = 16,  /* "c" and a CI index, with its NUL */
    ADP_DECIMAL = 10,
    ADP_NO_CI = -1 /* a ci_from entry whose name is not c<n> */
};

static Abc_Frame_t *adp_frame; /* the session's frame, NULL when none is open */
static Abc_Frame_t *adp_prev;  /* the frame entered before it */
static bool adp_entered;       /* adp_frame is the thread's current frame */
static Mini_Lut_t *adp_luts;   /* the last get_luts' MiniLUT, or NULL */
static int32_t *adp_ci_from;   /* its ci_from (odin3_abc_alloc), or NULL */
static int *adp_lits;          /* the last name map (ABC's malloc), or NULL */

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
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
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

/* Frees what get_luts returned. */
static void adp_free_luts(void) {
    if (adp_luts != NULL) {
        Mini_LutStop(adp_luts);
        adp_luts = NULL;
    }
    free(adp_ci_from);
    adp_ci_from = NULL;
}

odin3_status odin3_bridge_frame_close(bool destroy) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    /* The session's own results go even after an escape: they are plain malloc'd arrays. */
    adp_free_luts();
    free(adp_lits);
    adp_lits = NULL;
    if (adp_entered) {
        Abc_FrameLeave(adp_prev);
        adp_entered = false;
    }
    if (destroy && adp_frame != NULL) {
        Abc_FrameDestroy(adp_frame);
    }
    adp_frame = NULL;
    adp_prev = NULL;
    return ODIN3_OK;
}

odin3_status odin3_bridge_exec(const char *command, int *rc) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && command != NULL && rc != NULL);
    *rc = Cmd_CommandExecute(adp_frame, command);
    return ODIN3_OK;
}

/* The GIA's vNamesIn becomes "c<n>" for CI n (AB-16): &put names the network's CIs after it,
 * scorr keeps a surviving latch's name, and &get -n brings the names back for ci_from. */
static void adp_name_cis(Gia_Man_t *gia) {
    if (gia->vNamesIn != NULL) {
        Vec_PtrFreeFree(gia->vNamesIn);
    }
    int count = Gia_ManCiNum(gia);
    gia->vNamesIn = Vec_PtrAlloc(count);
    char name[ADP_CI_NAME_BYTES];
    for (int i = 0; i < count; i++) {
        int len = snprintf(name, sizeof name, "c%d", i);
        if (len < 0 || (size_t)len >= sizeof name) {
            name[0] = '\0'; /* not reached: an int fits */
        }
        Vec_PtrPush(gia->vNamesIn, Abc_UtilStrsav(name));
    }
}

odin3_status odin3_bridge_load_aig(int32_t *objs, uint32_t n_objs, uint32_t n_regs) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && objs != NULL && n_objs > 0 &&
                         n_objs <= INT32_MAX / 2);
    Mini_Aig_t mini = {.nCap = (int)(2 * n_objs),
                       .nSize = (int)(2 * n_objs),
                       .nRegs = (int)n_regs,
                       .pArray = objs};
    Abc_FrameGiaInputMiniAig(adp_frame, &mini);
    Gia_Man_t *gia = Abc_FrameReadGia(adp_frame);
    ODIN3_ADAPTER_ASSERT(gia != NULL);
    adp_name_cis(gia);
    return ODIN3_OK;
}

/* "c<n>" (n decimal, no leading zero, at most INT32_MAX) gives n; anything else ADP_NO_CI. */
static int32_t adp_ci_index(const char *name) {
    if (name == NULL || name[0] != 'c' || name[1] < '0' || name[1] > '9' ||
        (name[1] == '0' && name[2] != '\0')) {
        return ADP_NO_CI;
    }
    int64_t value = 0;
    for (const char *at = name + 1; *at != '\0'; at++) {
        if (*at < '0' || *at > '9') {
            return ADP_NO_CI;
        }
        value = value * ADP_DECIMAL + (*at - '0');
        if (value > INT32_MAX) {
            return ADP_NO_CI;
        }
    }
    return (int32_t)value;
}

/* *ci_from: gia's CI names parsed (a fresh odin3_abc_alloc array), or NULL when it has none. */
static odin3_status adp_ci_from_names(Gia_Man_t *gia, int32_t **ci_from) {
    *ci_from = NULL;
    if (gia->vNamesIn == NULL) {
        return ODIN3_OK;
    }
    int count = Gia_ManCiNum(gia);
    int named = Vec_PtrSize(gia->vNamesIn);
    int32_t *from = odin3_abc_alloc(sizeof *from * (size_t)(count > 0 ? count : 1));
    if (from == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (int i = 0; i < count; i++) {
        from[i] =
            i < named ? adp_ci_index((const char *)Vec_PtrEntry(gia->vNamesIn, i)) : ADP_NO_CI;
    }
    *ci_from = from;
    return ODIN3_OK;
}

odin3_status odin3_bridge_get_luts(odin3_abc_luts *luts, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && luts != NULL);
    Gia_Man_t *gia = Abc_FrameReadGia(adp_frame);
    /* Abc_FrameGiaOutputMiniLut asserts a mapping, and Mini_LutStart at most 16 inputs. */
    if (gia == NULL || !Gia_ManHasMapping(gia)) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no mapping in the & space");
    }
    int widest = Gia_ManLutSizeMax(gia);
    /* Not reached by any test: the pinned ABC's mappers stop below 17 inputs (if -K, &if -K). */
    if (widest > ADP_LUT_MAX_INPUTS) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "a LUT with %d inputs (at most %d)",
                                  widest, ADP_LUT_MAX_INPUTS);
    }
    int32_t *ci_from = NULL;
    odin3_status st = adp_ci_from_names(gia, &ci_from);
    if (st != ODIN3_OK) {
        return st;
    }
    adp_free_luts();
    adp_ci_from = ci_from;
    adp_luts = Abc_FrameGiaOutputMiniLut(adp_frame);
    ODIN3_ADAPTER_ASSERT(adp_luts != NULL && adp_luts->nSize >= 0 && adp_luts->nRegs >= 0);
    *luts = (odin3_abc_luts){.fanins = adp_luts->pArray,
                             .truths = adp_luts->pTruths,
                             .ci_from = adp_ci_from,
                             .n_objs = (uint32_t)adp_luts->nSize,
                             .n_regs = (uint32_t)adp_luts->nRegs,
                             .lut_size = (uint32_t)adp_luts->LutSize};
    return ODIN3_OK;
}

odin3_status odin3_bridge_name_map(const odin3_bridge_luts *aligned, const int32_t **lits,
                                   odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && aligned != NULL && lits != NULL);
    ODIN3_ADAPTER_ASSERT(aligned->n_objs <= INT32_MAX && aligned->n_regs <= aligned->n_objs);
    free(adp_lits);
    adp_lits = NULL;
    Mini_Lut_t mini = {.nCap = (int)aligned->n_objs,
                       .nSize = (int)aligned->n_objs,
                       .nRegs = (int)aligned->n_regs,
                       .LutSize = (int)aligned->lut_size,
                       .pArray = aligned->fanins,
                       .pTruths = aligned->truths};
    Abc_FrameGiaInputMiniLut2(adp_frame, &mini);
    adp_lits = Abc_FrameReadMiniLutNameMapping(adp_frame);
    if (adp_lits == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "ABC made no name map");
    }
    *lits = adp_lits;
    return ODIN3_OK;
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

/* base/cmd/cmdUtils.c (declared in ABC's internal cmdInt.h). */
extern const char *CmdSplitLine(Abc_Frame_t *pAbc, const char *sCommand, int *argc, char ***argv);

int odin3_abc_test_split(const char *line, char ***argv) {
    if (adp_frame == NULL || line == NULL || argv == NULL) {
        return -1;
    }
    int argc = 0;
    (void)CmdSplitLine(adp_frame, line, &argc, argv);
    return argc;
}

odin3_status odin3_abc_test_name_ci(uint32_t ci, const char *name) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    Gia_Man_t *gia = adp_frame != NULL ? Abc_FrameReadGia(adp_frame) : NULL;
    if (gia == NULL || gia->vNamesIn == NULL || name == NULL ||
        ci >= (uint32_t)Vec_PtrSize(gia->vNamesIn)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    char *old = (char *)Vec_PtrEntry(gia->vNamesIn, (int)ci);
    ABC_FREE(old);
    Vec_PtrWriteEntry(gia->vNamesIn, (int)ci, Abc_UtilStrsav((char *)name));
    return ODIN3_OK;
}

/*
 * The counting pthread_create of the test link (-Wl,--wrap=pthread_create, test_abc_adapter only).
 * Unlike the shipped wrappers (guard.c), it must call the real function, so it names
 * __real_pthread_create: the adapter library never links this code.
 */
typedef void *(*adp_thread_fn)(void *arg);
int odin3_test_wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   adp_thread_fn start, void *arg) __asm__("__wrap_pthread_create");
int odin3_test_real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   adp_thread_fn start, void *arg) __asm__("__real_pthread_create");

static atomic_uint adp_test_threads;

int odin3_test_wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   adp_thread_fn start, void *arg) {
    atomic_fetch_add(&adp_test_threads, 1U);
    return odin3_test_real_pthread_create(thread, attr, start, arg);
}

uint32_t odin3_abc_test_thread_count(void) {
    return atomic_load(&adp_test_threads);
}
#endif
