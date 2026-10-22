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
 * the exchange layouts odin3.h's odin3_abc_aig and odin3_abc_luts equal (§4.3); mio.h is the
 * genlib library (gate mapping, ABI 5). */
#include "base/main/main.h"
#include "map/mio/mio.h"

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

/* Gate mapping (ABI 5), which ABC's headers do not declare: the genlib reader of a buffer
 * (map/mio/mioRead.c; map/mio/mio.c declares it the same way) and the & space's cell mapping as
 * a mapped network (base/abci/abcDar.c:1152). */
extern Mio_Library_t *Mio_LibraryReadBuffer(char *pBuffer, int fExtendedFormat,
                                            st__table *tExcludeGate, int nFaninLimit, int fVerbose);
extern Abc_Ntk_t *Abc_NtkFromCellMappedGia(Gia_Man_t *pGia, int fUseBuffs);

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
    ADP_NO_CI = -1,              /* a ci_from entry whose name is not c<n> */
    ADP_MINI_NULL = 0x7FFFFFFF,  /* MiniLUT's empty fanin slot */
    ADP_MINI_NULL2 = 0x7FFFFFFE, /* MiniLUT's slot 1 of a PO */
    ADP_GATE_MAX_INPUTS = 6,     /* Mio_GateReadTruth's 64 bits */
    ADP_CELLS_MIN_WIDTH = 2,     /* get_cells' lut_size is at least 2, as MiniLUT's */
    ADP_TRUTH_LOG_BITS = 5,      /* 32-bit truth words */
    ADP_WORD_BITS = 32,
    ADP_CONST1_OBJ = 1,       /* MiniLUT object 1, constant 1 (object 0 is constant 0) */
    ADP_FIRST_OBJ = 2,        /* the first object after the constants */
    ADP_FROM_GIA_NO_BUFS = 0, /* Abc_NtkFromCellMappedGia's fUseBuffs */
    ADP_GENLIB_PLAIN = 0      /* Mio_LibraryReadBuffer's fExtendedFormat */
};

/* The name the gate library gets (ABC prints it; no file of that name is read or written). */
#define ADP_GENLIB_NAME "odin3.genlib"

static Abc_Frame_t *adp_frame; /* the session's frame, NULL when none is open */
static Abc_Frame_t *adp_prev;  /* the frame entered before it */
static bool adp_entered;       /* adp_frame is the thread's current frame */
static Mini_Lut_t *adp_luts;   /* the last get_luts' MiniLUT, or NULL */
static int32_t *adp_ci_from;   /* its ci_from (odin3_abc_alloc), or NULL */
static int *adp_lits;          /* the last name map (ABC's malloc), or NULL */

/* The last get_cells' arrays (odin3_abc_alloc), and the network it converted from the & space. */
static struct {
    int32_t *fanins;
    uint32_t *truths;
    int32_t *gate;
    const char **names;
    int32_t *ci_from;
} adp_cells;
static Abc_Ntk_t *adp_cells_ntk; /* Abc_NtkFromCellMappedGia's network, or NULL */

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

/* Deletes the network get_cells converted from the & space, if one is left. */
static void adp_delete_cells_ntk(void) {
    if (adp_cells_ntk != NULL) {
        Abc_NtkDelete(adp_cells_ntk);
        adp_cells_ntk = NULL;
    }
}

/* Frees what get_cells returned; with delete_ntk also a converted network left by an escape
 * (ABC's state; without it, the network leaks with the frame). */
static void adp_free_cells(bool delete_ntk) {
    free(adp_cells.fanins);
    free(adp_cells.truths);
    free(adp_cells.gate);
    free((void *)adp_cells.names);
    free(adp_cells.ci_from);
    memset(&adp_cells, 0, sizeof adp_cells);
    if (delete_ntk) {
        adp_delete_cells_ntk();
    }
    adp_cells_ntk = NULL;
}

odin3_status odin3_bridge_frame_close(bool destroy) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    /* The session's own results go even after an escape: they are plain malloc'd arrays. A
     * converted network is ABC's state: after an escape it leaks with the frame. */
    adp_free_luts();
    adp_free_cells(destroy);
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

/* --- gate mapping (ABI 5) ------------------------------------------------------------------- */

odin3_status odin3_bridge_load_genlib(const char *text, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && text != NULL);
    size_t bytes = strlen(text) + 1;
    char *copy = odin3_abc_alloc(bytes); /* the reader edits its buffer */
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, text, bytes);
    /* Not Mio_LibraryRead: with a buffer it dereferences the library it failed to read
     * (mioRead.c:96). The buffer reader returns NULL instead, and the name is set as mio.c does. */
    Mio_Library_t *lib = Mio_LibraryReadBuffer(copy, ADP_GENLIB_PLAIN, NULL, 0, 0);
    free(copy);
    if (lib == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "ABC cannot read the gate library");
    }
    Mio_LibrarySetName(lib, Abc_UtilStrsav(ADP_GENLIB_NAME));
    Mio_UpdateGenlib(lib);
    return ODIN3_OK;
}

/* One get_cells in progress. */
typedef struct adp_cells_build {
    Abc_Ntk_t *ntk;
    Vec_Ptr_t *order; /* Abc_NtkDfsIter's nodes (constants and gates), topological */
    int32_t *obj_of;  /* Abc_ObjId -> object, -1 none (odin3_abc_alloc) */
    uint32_t n_gates; /* nodes with fanins */
    uint32_t width;   /* lut_size */
    uint32_t words;   /* truth words per object */
    uint32_t n_objs;  /* the objects so far, then all of them */
    uint32_t n_names; /* the distinct gate names so far */
    odin3_abc_fail *fail;
} adp_cells_build;

/* The network of space: the network space's own, or the & space's cell mapping converted. */
static odin3_status adp_cells_network(uint32_t space, Abc_Ntk_t **ntk, odin3_abc_fail *fail) {
    if (space == ODIN3_ABC_CELLS_NTK) {
        *ntk = Abc_FrameReadNtk(adp_frame);
        if (*ntk == NULL || !Abc_NtkIsMappedLogic(*ntk)) {
            return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                      "no gate-mapped network in the network space");
        }
        return ODIN3_OK;
    }
    Gia_Man_t *gia = Abc_FrameReadGia(adp_frame);
    if (gia == NULL || !Gia_ManHasCellMapping(gia)) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no cell mapping in the & space");
    }
    adp_cells_ntk = Abc_NtkFromCellMappedGia(gia, ADP_FROM_GIA_NO_BUFS);
    ODIN3_ADAPTER_ASSERT(adp_cells_ntk != NULL);
    *ntk = adp_cells_ntk;
    return ODIN3_OK;
}

/* Checks every node of the order (each has a library gate, at most 6 inputs) and maps the
 * constants to objects 0 and 1; counts the gates and the widest. */
static odin3_status adp_cells_scan(adp_cells_build *build) {
    uint32_t widest = ADP_CELLS_MIN_WIDTH;
    for (int i = 0; i < Vec_PtrSize(build->order); i++) {
        Abc_Obj_t *node = (Abc_Obj_t *)Vec_PtrEntry(build->order, i);
        Mio_Gate_t *gate = (Mio_Gate_t *)node->pData;
        int fanins = Abc_ObjFaninNum(node);
        if (gate == NULL) {
            return odin3_abc_fail_set(build->fail, ODIN3_ABC_FAIL_RESULT,
                                      "a node without a library gate");
        }
        if (fanins > ADP_GATE_MAX_INPUTS) {
            return odin3_abc_fail_set(build->fail, ODIN3_ABC_FAIL_RESULT,
                                      "a gate with %d inputs (at most %d)", fanins,
                                      ADP_GATE_MAX_INPUTS);
        }
        if (fanins == 0) {
            build->obj_of[Abc_ObjId(node)] = Mio_GateReadTruth(gate) == 0 ? 0 : ADP_CONST1_OBJ;
            continue;
        }
        build->n_gates++;
        widest = (uint32_t)fanins > widest ? (uint32_t)fanins : widest;
    }
    build->width = widest;
    build->words = widest > ADP_TRUTH_LOG_BITS ? 1U << (widest - ADP_TRUTH_LOG_BITS) : 1U;
    return ODIN3_OK;
}

/* The object map, then the result arrays sized for the network. */
static odin3_status adp_cells_alloc(adp_cells_build *build) {
    Abc_Ntk_t *ntk = build->ntk;
    uint32_t cis = (uint32_t)(Abc_NtkPiNum(ntk) + Abc_NtkLatchNum(ntk));
    uint32_t cos = (uint32_t)(Abc_NtkPoNum(ntk) + Abc_NtkLatchNum(ntk));
    build->n_objs = ADP_FIRST_OBJ + cis + build->n_gates + cos;
    size_t objs = build->n_objs;
    adp_cells.fanins = odin3_abc_alloc(sizeof(int32_t) * objs * build->width);
    adp_cells.truths =
        adp_cells.fanins == NULL ? NULL : odin3_abc_alloc(sizeof(uint32_t) * objs * build->words);
    adp_cells.gate = adp_cells.truths == NULL ? NULL : odin3_abc_alloc(sizeof(int32_t) * objs);
    adp_cells.names = adp_cells.gate == NULL
                          ? NULL
                          : (const char **)odin3_abc_alloc(sizeof(char *) * (build->n_gates + 1U));
    if (adp_cells.names == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < objs * build->width; i++) {
        adp_cells.fanins[i] = ADP_MINI_NULL;
    }
    memset(adp_cells.truths, 0, sizeof(uint32_t) * objs * build->words);
    for (size_t i = 0; i < objs; i++) {
        adp_cells.gate[i] = -1;
    }
    adp_cells.truths[(size_t)ADP_CONST1_OBJ * build->words] = UINT32_MAX;
    build->n_objs = ADP_FIRST_OBJ;
    return ODIN3_OK;
}

/* The next object stands for ABC's object obj (NULL: none, as for a PO). */
static uint32_t adp_cells_push(adp_cells_build *build, Abc_Obj_t *obj) {
    uint32_t at = build->n_objs++;
    if (obj != NULL) {
        build->obj_of[Abc_ObjId(obj)] = (int32_t)at;
    }
    return at;
}

/* The PIs, then the register outputs; ci_from from their names (network space). */
static odin3_status adp_cells_inputs(adp_cells_build *build, bool names_from_ntk) {
    Abc_Ntk_t *ntk = build->ntk;
    int pis = Abc_NtkPiNum(ntk);
    int regs = Abc_NtkLatchNum(ntk);
    if (names_from_ntk) {
        adp_cells.ci_from = odin3_abc_alloc(sizeof(int32_t) * (size_t)(pis + regs + 1));
        if (adp_cells.ci_from == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    for (int i = 0; i < pis + regs; i++) {
        Abc_Obj_t *ci = i < pis ? Abc_NtkPi(ntk, i) : Abc_ObjFanout0(Abc_NtkBox(ntk, i - pis));
        (void)adp_cells_push(build, ci);
        if (names_from_ntk) {
            adp_cells.ci_from[i] = adp_ci_index(Abc_ObjName(ci));
        }
    }
    return ODIN3_OK;
}

/* The index into names of gate's name, added on its first use. */
static int32_t adp_cells_name(adp_cells_build *build, Mio_Gate_t *gate) {
    const char *name = Mio_GateReadName(gate);
    for (uint32_t i = 0; i < build->n_names; i++) {
        if (adp_cells.names[i] == name) {
            return (int32_t)i;
        }
    }
    adp_cells.names[build->n_names] = name;
    return (int32_t)build->n_names++;
}

/* Object at's fanin slot k becomes ABC's fanin; RESULT when it is not an object yet. */
static odin3_status adp_cells_wire(adp_cells_build *build, uint32_t at, uint32_t slot,
                                   Abc_Obj_t *fanin) {
    int32_t from = build->obj_of[Abc_ObjId(fanin)];
    if (from < 0) {
        return odin3_abc_fail_set(build->fail, ODIN3_ABC_FAIL_RESULT,
                                  "a fanin that is not a gate, an input or a constant");
    }
    adp_cells.fanins[((size_t)at * build->width) + slot] = from;
    return ODIN3_OK;
}

/* The gates in topological order: fanins in pin order, truth words, gate name. */
static odin3_status adp_cells_gates(adp_cells_build *build) {
    for (int i = 0; i < Vec_PtrSize(build->order); i++) {
        Abc_Obj_t *node = (Abc_Obj_t *)Vec_PtrEntry(build->order, i);
        if (Abc_ObjFaninNum(node) == 0) {
            continue; /* a constant, already object 0 or 1 */
        }
        Mio_Gate_t *gate = (Mio_Gate_t *)node->pData;
        uint32_t at = adp_cells_push(build, node);
        for (int k = 0; k < Abc_ObjFaninNum(node); k++) {
            odin3_status st = adp_cells_wire(build, at, (uint32_t)k, Abc_ObjFanin(node, k));
            if (st != ODIN3_OK) {
                return st;
            }
        }
        word truth = Mio_GateReadTruth(gate);
        for (uint32_t word_at = 0; word_at < build->words; word_at++) {
            adp_cells.truths[((size_t)at * build->words) + word_at] =
                (uint32_t)(truth >> (ADP_WORD_BITS * word_at));
        }
        adp_cells.gate[at] = adp_cells_name(build, gate);
    }
    return ODIN3_OK;
}

/* The POs, then the register inputs: driver in slot 0, MiniLUT's PO mark in slot 1. */
static odin3_status adp_cells_outputs(adp_cells_build *build) {
    Abc_Ntk_t *ntk = build->ntk;
    int pos = Abc_NtkPoNum(ntk);
    int regs = Abc_NtkLatchNum(ntk);
    for (int i = 0; i < pos + regs; i++) {
        Abc_Obj_t *co = i < pos ? Abc_NtkPo(ntk, i) : Abc_ObjFanin0(Abc_NtkBox(ntk, i - pos));
        uint32_t at = adp_cells_push(build, NULL);
        odin3_status st = adp_cells_wire(build, at, 0, Abc_ObjFanin0(co));
        if (st != ODIN3_OK) {
            return st;
        }
        adp_cells.fanins[((size_t)at * build->width) + 1U] = ADP_MINI_NULL2;
    }
    return ODIN3_OK;
}

/* Fills adp_cells from build->ntk (the order already made). */
static odin3_status adp_cells_fill(adp_cells_build *build, uint32_t space) {
    size_t ids = (size_t)Abc_NtkObjNumMax(build->ntk);
    build->obj_of = odin3_abc_alloc(sizeof(int32_t) * (ids + 1U));
    if (build->obj_of == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i <= ids; i++) {
        build->obj_of[i] = -1;
    }
    odin3_status st = adp_cells_scan(build);
    st = st == ODIN3_OK ? adp_cells_alloc(build) : st;
    if (st == ODIN3_OK && space == ODIN3_ABC_CELLS_GIA) {
        st = adp_ci_from_names(Abc_FrameReadGia(adp_frame), &adp_cells.ci_from);
    }
    st = st == ODIN3_OK ? adp_cells_inputs(build, space == ODIN3_ABC_CELLS_NTK) : st;
    st = st == ODIN3_OK ? adp_cells_gates(build) : st;
    return st == ODIN3_OK ? adp_cells_outputs(build) : st;
}

odin3_status odin3_bridge_get_cells(uint32_t space, odin3_abc_cells *cells, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(odin3_guard_active_here());
    ODIN3_ADAPTER_ASSERT(adp_frame != NULL && cells != NULL && space <= ODIN3_ABC_CELLS_GIA);
    adp_free_cells(true);
    adp_cells_build build = {.fail = fail};
    odin3_status st = adp_cells_network(space, &build.ntk, fail);
    if (st == ODIN3_OK && Abc_NtkBoxNum(build.ntk) != Abc_NtkLatchNum(build.ntk)) {
        st = odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "a box that is not a latch");
    }
    uint32_t regs = 0;
    if (st == ODIN3_OK) {
        regs = (uint32_t)Abc_NtkLatchNum(build.ntk);
        build.order = Abc_NtkDfsIter(build.ntk, 1);
        st = adp_cells_fill(&build, space);
        Vec_PtrFree(build.order);
        free(build.obj_of);
    }
    adp_delete_cells_ntk(); /* the names point into the library, not into the network */
    if (st != ODIN3_OK) {
        adp_free_cells(false);
        return st;
    }
    *cells = (odin3_abc_cells){.net = {.fanins = adp_cells.fanins,
                                       .truths = adp_cells.truths,
                                       .ci_from = adp_cells.ci_from,
                                       .n_objs = build.n_objs,
                                       .n_regs = regs,
                                       .lut_size = build.width},
                               .gate = adp_cells.gate,
                               .names = adp_cells.names,
                               .n_names = build.n_names};
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
