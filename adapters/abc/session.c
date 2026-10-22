/*
 * session.c — the linked engine's one static session: open, load_aig, exec, get_luts, name_map,
 * load_genlib, get_cells and close (session.h; 3D spec §4.4, AB-8, AB-16). Every ABC call goes
 * through odin3_guard_run; every engine entry point ends with odin3_guard_finish, so an adapter
 * assertion anywhere below it reaches the caller's fail. The exchange arrays are checked on the
 * adapter's own copies before ABC sees them: ABC asserts (or, without assertions, crashes) on a
 * malformed MiniAIG or MiniLUT.
 */
#include "session.h"

#include "bridge.h"
#include "guard.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ADP_NULL = 0x7FFFFFFF,  /* MiniAIG's and MiniLUT's empty slot (MINI_AIG_NULL, MINI_LUT_NULL) */
    ADP_NULL2 = 0x7FFFFFFE, /* MiniLUT's slot 1 of a PO (MINI_LUT_NULL2) */
    ADP_MAX_OBJS = 1 << 29, /* MiniAIG's own ceiling (§6.3): 2 * index stays an int */
    ADP_LUT_MIN = 2,        /* MiniLUT's lut_size range (Mini_LutStart) */
    ADP_LUT_MAX = 16,
    ADP_TRUTH_LOG_BITS = 5, /* 32-bit truth words */
    ADP_LUT_CONSTS = 2      /* MiniLUT objects 0 and 1 */
};

/* The interface of the loaded AIG, which name_map's aligned network must have. */
typedef struct adp_shape {
    uint32_t pis, ands, pos, regs;
} adp_shape;

typedef struct adp_session {
    pthread_mutex_t lock; /* held from a successful open to its close (BUSY otherwise) */
    pthread_t thread;     /* the thread that opened the session */
    bool open;            /* between a successful open and close */
    bool loaded;          /* load_aig succeeded in this session (loaded holds its shape) */
    bool executed;        /* an exec reached ABC in this session (load_genlib is then refused) */
    adp_shape shape;      /* the loaded AIG's counts */
    odin3_capture cap;    /* fds 1 and 2 during each call */
} adp_session;

static adp_session adp_the = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cap = {.file = NULL, .saved_out = -1, .saved_err = -1, .read_at = 0},
};

void *odin3_abc_session_self(void) {
    return &adp_the;
}

static odin3_status adp_frame_open_fn(void *arg) {
    (void)arg;
    return odin3_bridge_frame_open();
}

static odin3_status adp_frame_close_fn(void *arg) {
    const bool *destroy = arg;
    return odin3_bridge_frame_close(*destroy);
}

/* Leaves the frame (destroying it only when the engine is still enabled): run under the guard,
 * whose own failure is the engine's (disabled) and is not reported further. It runs even when no
 * descriptor is left for the capture, so a later open still finds no frame. */
static void adp_close_frame(void) {
    bool destroy = !odin3_guard_disabled();
    odin3_abc_fail ignored;
    (void)odin3_guard_run_always(adp_frame_close_fn, &destroy, &ignored);
}

/* Ends the session: no capture, no file, and the lock released. */
static void adp_release(adp_session *session) {
    odin3_guard_detach();
    odin3_capture_close(&session->cap);
    session->open = false;
    session->loaded = false;
    session->executed = false;
    (void)pthread_mutex_unlock(&session->lock);
}

/* Starts a session on the locked session: the thread, the capture, the guard and the frame. */
static odin3_status adp_start(adp_session *session, odin3_abc_text_fn text, void *user,
                              odin3_abc_fail *fail) {
    session->thread = pthread_self();
    odin3_status st = odin3_capture_open(&session->cap);
    if (st != ODIN3_OK) {
        (void)pthread_mutex_unlock(&session->lock);
        return st;
    }
    odin3_guard_attach(&session->cap, text, user);
    st = odin3_guard_run(adp_frame_open_fn, NULL, fail);
    if (st != ODIN3_OK) {
        adp_close_frame();
        adp_release(session);
        return st;
    }
    session->open = true;
    return ODIN3_OK;
}

static odin3_status adp_open(adp_session *session, odin3_abc_text_fn text, void *user,
                             odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(session == &adp_the && fail != NULL);
    if (odin3_guard_disabled()) {
        odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_DISABLED);
        return ODIN3_ERR_ENGINE;
    }
    if (pthread_mutex_trylock(&session->lock) != 0) {
        odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_BUSY);
        return ODIN3_ERR_ENGINE;
    }
    return adp_start(session, text, user, fail);
}

odin3_status odin3_abc_session_open(void *self, odin3_abc_text_fn text, void *user,
                                    odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_open(self, text, user, fail), fail);
}

typedef struct adp_exec_arg {
    const char *command;
    int rc; /* ABC's result */
} adp_exec_arg;

static odin3_status adp_exec_fn(void *arg) {
    adp_exec_arg *exec = arg;
    return odin3_bridge_exec(exec->command, &exec->rc);
}

/* The checks every call between open and close starts with: the session's own invariants (an
 * adapter assertion), then DISABLED after an escape. */
static odin3_status adp_ready(adp_session *session, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(session == &adp_the && fail != NULL);
    ODIN3_ADAPTER_ASSERT(session->open && pthread_equal(pthread_self(), session->thread));
    if (odin3_guard_disabled()) {
        odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_DISABLED);
        return ODIN3_ERR_ENGINE;
    }
    return ODIN3_OK;
}

static odin3_status adp_exec(adp_session *session, const char *command, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(command != NULL);
    odin3_status ready = adp_ready(session, fail);
    if (ready != ODIN3_OK) {
        return ready;
    }
    adp_exec_arg exec = {.command = command, .rc = 0};
    session->executed = true;
    odin3_status st = odin3_guard_run(adp_exec_fn, &exec, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (exec.rc != 0) {
        odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_COMMAND);
        fail->code = exec.rc;
        return ODIN3_ERR_ENGINE;
    }
    return ODIN3_OK;
}

odin3_status odin3_abc_session_exec(void *self, const char *command, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_exec(self, command, fail), fail);
}

/* --- the adapter's own allocations ------------------------------------------------------------ */

#ifdef ODIN3_ABC_TEST_HOOKS
static uint32_t adp_fail_after = UINT32_MAX; /* UINT32_MAX: the hook is off */

void odin3_abc_test_fail_alloc_after(uint32_t n) {
    adp_fail_after = n;
}

/* True when the test hook fails this allocation (once; the hook is then off). */
static bool adp_hook_fails(void) {
    if (adp_fail_after == UINT32_MAX) {
        return false;
    }
    if (adp_fail_after == 0) {
        adp_fail_after = UINT32_MAX;
        return true;
    }
    adp_fail_after--;
    return false;
}
#else
static bool adp_hook_fails(void) {
    return false;
}
#endif

void *odin3_abc_alloc(size_t bytes) {
    return adp_hook_fails() ? NULL : malloc(bytes);
}

FILE *odin3_abc_tmpfile(void) {
    return adp_hook_fails() ? NULL : tmpfile();
}

/* --- load_aig --------------------------------------------------------------------------------- */

/*
 * Object obj (>= 1) of a MiniAIG copy, in §4.3's order: the PIs (NULL, NULL), then the ANDs
 * (lit0, lit1) with 0 <= lit0 < lit1 < 2 * obj, then the POs (lit, NULL) on an earlier PI or AND.
 * Counts it in *shape, or fills fail (RESULT, naming the object).
 */
static odin3_status adp_check_aig_obj(const int32_t *objs, uint32_t obj, adp_shape *shape,
                                      odin3_abc_fail *fail) {
    int32_t lit0 = objs[2 * (size_t)obj];
    int32_t lit1 = objs[(2 * (size_t)obj) + 1];
    if (lit0 == ADP_NULL && lit1 == ADP_NULL) {
        if (shape->ands + shape->pos > 0) {
            return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                      "object %u: a PI after the first AND or PO", obj);
        }
        shape->pis++;
        return ODIN3_OK;
    }
    if (lit1 == ADP_NULL) {
        uint32_t below = 2 * (1 + shape->pis + shape->ands); /* the PIs and ANDs before it */
        if (lit0 < 0 || (uint32_t)lit0 >= below) {
            return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                      "object %u: a PO needs a literal from 0 to %u, not %d", obj,
                                      below - 1, (int)lit0);
        }
        shape->pos++;
        return ODIN3_OK;
    }
    if (shape->pos > 0) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "object %u: an AND after the first PO", obj);
    }
    if (lit0 < 0 || lit0 >= lit1 || (uint32_t)lit1 >= 2 * obj) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "object %u: an AND needs 0 <= lit0 < lit1 < %u, not (%d, %d)",
                                  obj, 2 * obj, (int)lit0, (int)lit1);
    }
    shape->ands++;
    return ODIN3_OK;
}

/* A MiniAIG copy against §4.3's layout; *shape gets its counts. RESULT names the first fault. */
static odin3_status adp_check_aig(const odin3_abc_aig *aig, const int32_t *objs, adp_shape *shape,
                                  odin3_abc_fail *fail) {
    *shape = (adp_shape){.regs = aig->n_regs};
    if (objs[0] != ADP_NULL || objs[1] != ADP_NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "object 0: the constant is (%d, %d), not (%d, %d)", (int)objs[0],
                                  (int)objs[1], ADP_NULL, ADP_NULL);
    }
    for (uint32_t i = 1; i < aig->n_objs; i++) {
        odin3_status st = adp_check_aig_obj(objs, i, shape, fail);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (aig->n_regs > shape->pis || aig->n_regs > shape->pos) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "%u registers, more than the %u PIs or the %u POs", aig->n_regs,
                                  shape->pis, shape->pos);
    }
    return ODIN3_OK;
}

typedef struct adp_load_arg {
    int32_t *objs;
    uint32_t n_objs, n_regs;
} adp_load_arg;

static odin3_status adp_load_fn(void *arg) {
    const adp_load_arg *load = arg;
    return odin3_bridge_load_aig(load->objs, load->n_objs, load->n_regs);
}

/* Copies, checks and loads aig; *shape gets its counts. */
static odin3_status adp_load(const odin3_abc_aig *aig, adp_shape *shape, odin3_abc_fail *fail) {
    if (aig->n_objs == 0 || aig->n_objs > ADP_MAX_OBJS || aig->objs == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "an AIG needs from 1 to %d objects, not %u%s", ADP_MAX_OBJS,
                                  aig->n_objs, aig->objs == NULL ? " (objs is NULL)" : "");
    }
    size_t bytes = sizeof *aig->objs * 2 * (size_t)aig->n_objs;
    int32_t *copy = odin3_abc_alloc(bytes);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, aig->objs, bytes);
    odin3_status st = adp_check_aig(aig, copy, shape, fail);
    if (st == ODIN3_OK) {
        adp_load_arg load = {.objs = copy, .n_objs = aig->n_objs, .n_regs = aig->n_regs};
        st = odin3_guard_run(adp_load_fn, &load, fail);
    }
    free(copy);
    return st;
}

static odin3_status adp_load_aig(adp_session *session, const odin3_abc_aig *aig,
                                 odin3_abc_fail *fail) {
    odin3_status st = adp_ready(session, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (aig == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no AIG (aig is NULL)");
    }
    adp_shape shape;
    st = adp_load(aig, &shape, fail);
    if (st == ODIN3_OK) {
        session->loaded = true;
        session->shape = shape;
    }
    return st;
}

odin3_status odin3_abc_session_load_aig(void *self, const odin3_abc_aig *aig,
                                        odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_load_aig(self, aig, fail), fail);
}

/* --- get_luts --------------------------------------------------------------------------------- */

typedef struct adp_luts_arg {
    odin3_abc_luts *luts;
    odin3_abc_fail *fail;
} adp_luts_arg;

static odin3_status adp_luts_fn(void *arg) {
    const adp_luts_arg *get = arg;
    return odin3_bridge_get_luts(get->luts, get->fail);
}

static odin3_status adp_get_luts(adp_session *session, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    odin3_status st = adp_ready(session, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (luts == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "nowhere to put the LUTs (luts is NULL)");
    }
    adp_luts_arg get = {.luts = luts, .fail = fail};
    return odin3_guard_run(adp_luts_fn, &get, fail);
}

odin3_status odin3_abc_session_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_get_luts(self, luts, fail), fail);
}

/* --- name_map --------------------------------------------------------------------------------- */

/* The truth words of one object of a MiniLUT with lut_size inputs at most. */
static size_t adp_truth_words(uint32_t lut_size) {
    return lut_size > ADP_TRUTH_LOG_BITS ? (size_t)1 << (lut_size - ADP_TRUTH_LOG_BITS) : 1;
}

/*
 * Object obj (>= 2) of an aligned MiniLUT copy: a PI (fanin 0 NULL), a PO (fanin 0 on an earlier
 * object that is not a PO, fanin 1 NULL2), or a LUT (its fanins, up to the first NULL or NULL2,
 * on earlier objects that are not POs). Counts it in *shape; is_po[obj] records POs.
 */
static odin3_status adp_check_lut_obj(const odin3_bridge_luts *lut, uint32_t obj, bool *is_po,
                                      adp_shape *shape, odin3_abc_fail *fail) {
    const int32_t *fanin = lut->fanins + ((size_t)lut->lut_size * obj);
    if (fanin[0] == ADP_NULL) {
        shape->pis++;
        return ODIN3_OK;
    }
    bool po = fanin[1] == ADP_NULL2;
    uint32_t slots = po ? 1 : lut->lut_size;
    /* Fanin 0 always counts; the others end at the first empty slot (Mini_LutForEachFanin). */
    for (uint32_t k = 0; k < slots && (k == 0 || fanin[k] < ADP_NULL2); k++) {
        if (fanin[k] < 0 || (uint32_t)fanin[k] >= obj || is_po[fanin[k]]) {
            return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                      "aligned object %u: fanin %u is %d, not an earlier PI or LUT",
                                      obj, k, (int)fanin[k]);
        }
    }
    is_po[obj] = po;
    shape->pos += po ? 1 : 0;
    shape->ands += po ? 0 : 1;
    return ODIN3_OK;
}

/* An aligned MiniLUT copy against odin3_abc_luts' layout and the loaded AIG's interface. */
static odin3_status adp_check_aligned(const odin3_bridge_luts *lut, const adp_shape *loaded,
                                      odin3_abc_fail *fail) {
    bool *is_po = odin3_abc_alloc(sizeof *is_po * (size_t)lut->n_objs);
    if (is_po == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(is_po, 0, sizeof *is_po * (size_t)lut->n_objs);
    adp_shape shape = {.regs = lut->n_regs};
    odin3_status st = ODIN3_OK;
    for (uint32_t i = ADP_LUT_CONSTS; st == ODIN3_OK && i < lut->n_objs; i++) {
        st = adp_check_lut_obj(lut, i, is_po, &shape, fail);
    }
    free(is_po);
    if (st == ODIN3_OK &&
        (shape.pis != loaded->pis || shape.pos != loaded->pos || shape.regs != loaded->regs)) {
        st = odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                "the aligned network has %u PIs, %u POs and %u registers; the "
                                "loaded AIG has %u, %u and %u",
                                shape.pis, shape.pos, shape.regs, loaded->pis, loaded->pos,
                                loaded->regs);
    }
    return st;
}

/* *copy = a copy of aligned's arrays (freed by adp_free_aligned), or RESULT for bad sizes. */
static odin3_status adp_copy_aligned(const odin3_abc_luts *aligned, odin3_bridge_luts *copy,
                                     odin3_abc_fail *fail) {
    *copy = (odin3_bridge_luts){
        .n_objs = aligned->n_objs, .n_regs = aligned->n_regs, .lut_size = aligned->lut_size};
    if (aligned->lut_size < ADP_LUT_MIN || aligned->lut_size > ADP_LUT_MAX ||
        aligned->n_objs < ADP_LUT_CONSTS || aligned->n_objs > ADP_MAX_OBJS ||
        aligned->fanins == NULL || aligned->truths == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "the aligned network needs lut_size 2 to 16 and 2 to %d objects, "
                                  "not %u and %u",
                                  ADP_MAX_OBJS, aligned->lut_size, aligned->n_objs);
    }
    size_t fanin_bytes = sizeof *copy->fanins * aligned->lut_size * (size_t)aligned->n_objs;
    size_t truth_bytes =
        sizeof *copy->truths * adp_truth_words(aligned->lut_size) * (size_t)aligned->n_objs;
    copy->fanins = odin3_abc_alloc(fanin_bytes);
    copy->truths = copy->fanins != NULL ? odin3_abc_alloc(truth_bytes) : NULL;
    if (copy->truths == NULL) {
        free(copy->fanins);
        copy->fanins = NULL;
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy->fanins, aligned->fanins, fanin_bytes);
    memcpy(copy->truths, aligned->truths, truth_bytes);
    return ODIN3_OK;
}

typedef struct adp_map_arg {
    const odin3_bridge_luts *aligned;
    const int32_t **lits;
    odin3_abc_fail *fail;
} adp_map_arg;

static odin3_status adp_map_fn(void *arg) {
    const adp_map_arg *map = arg;
    return odin3_bridge_name_map(map->aligned, map->lits, map->fail);
}

static odin3_status adp_name_map(adp_session *session, const odin3_abc_luts *aligned,
                                 const int32_t **lits, odin3_abc_fail *fail) {
    odin3_status st = adp_ready(session, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (aligned == NULL || lits == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "no aligned network or nowhere to put the map (NULL)");
    }
    if (!session->loaded) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no AIG was loaded in this session");
    }
    odin3_bridge_luts copy;
    st = adp_copy_aligned(aligned, &copy, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    st = adp_check_aligned(&copy, &session->shape, fail);
    if (st == ODIN3_OK) {
        adp_map_arg map = {.aligned = &copy, .lits = lits, .fail = fail};
        st = odin3_guard_run(adp_map_fn, &map, fail);
    }
    free(copy.fanins);
    free(copy.truths);
    return st;
}

odin3_status odin3_abc_session_name_map(void *self, const odin3_abc_luts *aligned,
                                        const int32_t **lits, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_name_map(self, aligned, lits, fail), fail);
}

/* --- gate mapping: load_genlib and get_cells (ABI 5) ------------------------------------------ */

/*
 * The genlib the adapter hands to ABC: exactly the canonical form Odin III's own writer
 * (src/passes/abc/genlib.c) emits, and nothing else. ABC's reader is not total (it dereferences
 * missing words, runs past a comment on the last line, joins lines at a '\\', reads a PIN * formula
 * past its fixed name table), so the adapter does not chase it: a text outside this subset is
 * refused before ABC sees it.
 *
 * Characters: printable ASCII, single spaces and '\n' only. Lines, each ended by '\n' (the last
 * one may end the text), none empty:
 *   GATE <name> <number> <name>=<formula>;
 *   PIN <name> UNKNOWN|INV|NONINV <number> <number> <number> <number> <number> <number>
 * A name is [A-Za-z_$][A-Za-z0-9_$]*, a number [-+]?(digits[.digits]|.digits)[e[-+]digits], each
 * at most ADP_GENLIB_TOKEN_MAX bytes. A formula is names, "!*&+|^'()" and single spaces. Each GATE
 * has at most ADP_GENLIB_PINS_MAX PINs, all distinct, and its formula names each of them and no
 * other name; a GATE without PINs has the formula CONST0 or CONST1. No two GATEs share a name.
 * (The formula's grammar is checked in bridge.c, before Mio_ParseFormula.)
 */

enum {
    ADP_GENLIB_TOKEN_MAX = 128, /* a name's or a number's bytes */
    ADP_GENLIB_PINS_MAX = 6,    /* the core's ODIN3_ABC_GATE_MAX_INPUTS (Mio_GateReadTruth) */
    ADP_GENLIB_PIN_NUMBERS = 6, /* a PIN's two loads and four delays */
    ADP_GENLIB_FIRST_PRINT = '!',
    ADP_GENLIB_LAST_PRINT = '~'
};

#define ADP_GENLIB_FORMULA_SYMBOLS "!*&+|^'() " /* mioFunc.c's MIO_SYMB_*, and the blank */

/* A run of the text: at and len. */
typedef struct adp_span {
    const char *at;
    size_t len;
} adp_span;

/* A GATE's name and the line it is on (the duplicate check sorts these). */
typedef struct adp_gate_name {
    adp_span name;
    uint32_t line;
} adp_gate_name;

/* The GATE being read: its name, formula, PINs and line. */
typedef struct adp_genlib_gate {
    adp_span name;
    adp_span formula;
    adp_span pins[ADP_GENLIB_PINS_MAX];
    uint32_t n_pins;
    uint32_t line;
} adp_genlib_gate;

/* One check in progress: the line it is on, the open GATE and the GATE names so far. */
typedef struct adp_genlib_scan {
    uint32_t line;
    bool in_gate;
    adp_genlib_gate gate;
    adp_gate_name *names; /* odin3_abc_alloc, one per line at most */
    size_t n_names;
} adp_genlib_scan;

/* Why the byte sym (after a blank when after_blank) is not allowed, or NULL. */
static const char *adp_genlib_char(unsigned char sym, bool after_blank) {
    if (sym == '\t') {
        return "a tab";
    }
    if (sym == '\r') {
        return "a carriage return";
    }
    if (sym == '#') {
        return "a comment ('#')";
    }
    if (sym == '\\') {
        return "a backslash";
    }
    if (sym == ' ') {
        return after_blank ? "two spaces" : NULL;
    }
    if (sym != '\n' && (sym < ADP_GENLIB_FIRST_PRINT || sym > ADP_GENLIB_LAST_PRINT)) {
        return "a control character or a byte outside ASCII";
    }
    return NULL;
}

/* Why text has a character outside the subset (*line gets its line), or NULL; *lines gets the
 * number of lines. */
static const char *adp_genlib_chars(const char *text, uint32_t *line, size_t *lines) {
    bool after_blank = false;
    *line = 1;
    *lines = 1;
    for (const char *at = text; *at != '\0'; at++) {
        unsigned char sym = (unsigned char)*at;
        const char *why = adp_genlib_char(sym, after_blank);
        if (why != NULL) {
            return why;
        }
        if (sym == '\n') {
            (*line)++;
            (*lines)++;
        }
        after_blank = sym == ' ';
    }
    return NULL;
}

static bool adp_is_name_start(char sym) {
    return (sym >= 'A' && sym <= 'Z') || (sym >= 'a' && sym <= 'z') || sym == '_' || sym == '$';
}

static bool adp_is_digit(char sym) {
    return sym >= '0' && sym <= '9';
}

static bool adp_is_name_char(char sym) {
    return adp_is_name_start(sym) || adp_is_digit(sym);
}

/* The length of the run of name characters at at (before end). */
static size_t adp_name_run(const char *at, const char *end) {
    size_t len = 0;
    while (at + len < end && adp_is_name_char(at[len])) {
        len++;
    }
    return len;
}

/* A name's length at at, or 0. */
static size_t adp_name_len(const char *at, const char *end) {
    if (at == end || !adp_is_name_start(*at)) {
        return 0;
    }
    size_t len = adp_name_run(at, end);
    return len <= ADP_GENLIB_TOKEN_MAX ? len : 0;
}

static size_t adp_digits(const char *at, const char *end) {
    size_t len = 0;
    while (at + len < end && adp_is_digit(at[len])) {
        len++;
    }
    return len;
}

static bool adp_is_sign(const char *at, const char *end) {
    return at < end && (*at == '-' || *at == '+');
}

/* A number's length at at, or 0. */
static size_t adp_number_len(const char *at, const char *end) {
    size_t len = adp_is_sign(at, end) ? 1U : 0U;
    size_t whole = adp_digits(at + len, end);
    size_t frac = 0;
    len += whole;
    if (at + len < end && at[len] == '.') {
        frac = adp_digits(at + len + 1, end);
        len += 1 + frac;
    }
    if (whole + frac == 0) {
        return 0;
    }
    if (at + len < end && (at[len] == 'e' || at[len] == 'E')) {
        size_t exp = len + 1 + (adp_is_sign(at + len + 1, end) ? 1U : 0U);
        size_t digits = adp_digits(at + exp, end);
        if (digits == 0) {
            return 0;
        }
        len = exp + digits;
    }
    return len <= ADP_GENLIB_TOKEN_MAX ? len : 0;
}

/* A word's length at at (up to a blank or the line's end). */
static size_t adp_word_len(const char *at, const char *end) {
    size_t len = 0;
    while (at + len < end && at[len] != ' ') {
        len++;
    }
    return len;
}

/* A formula's length at at: names and ADP_GENLIB_FORMULA_SYMBOLS. */
static size_t adp_formula_len(const char *at, const char *end) {
    size_t len = 0;
    while (at + len < end &&
           (adp_is_name_char(at[len]) || strchr(ADP_GENLIB_FORMULA_SYMBOLS, at[len]) != NULL)) {
        len++;
    }
    return len;
}

typedef size_t (*adp_len_fn)(const char *at, const char *end);

/* The token at *at that len_of measures, then sep ('\0': the line's end) into *out, moving *at
 * past sep; false when the token is empty or sep does not follow it. */
static bool adp_take(const char **at, const char *end, adp_len_fn len_of, char sep, adp_span *out) {
    size_t len = len_of(*at, end);
    const char *after = *at + len;
    bool ok = len > 0 && (sep == '\0' ? after == end : (after < end && *after == sep));
    if (ok) {
        out->at = *at;
        out->len = len;
        *at = sep == '\0' ? after : after + 1;
    }
    return ok;
}

static bool adp_span_is(const adp_span *span, const char *word) {
    return span->len == strlen(word) && memcmp(span->at, word, span->len) == 0;
}

static bool adp_span_eq(const adp_span *one, const adp_span *two) {
    return one->len == two->len && memcmp(one->at, two->at, one->len) == 0;
}

/* The index of name among pins, or n_pins. */
static uint32_t adp_pin_index(const adp_span *pins, uint32_t n_pins, const adp_span *name) {
    uint32_t k = 0;
    while (k < n_pins && !adp_span_eq(&pins[k], name)) {
        k++;
    }
    return k;
}

/* Why the open GATE's formula and PINs do not match, or NULL. */
static const char *adp_genlib_gate_end(const adp_genlib_gate *gate) {
    if (gate->n_pins == 0) {
        bool constant =
            adp_span_is(&gate->formula, "CONST0") || adp_span_is(&gate->formula, "CONST1");
        return constant ? NULL : "a gate without PINs whose formula is not CONST0 or CONST1";
    }
    bool named[ADP_GENLIB_PINS_MAX] = {false};
    const char *at = gate->formula.at;
    const char *end = at + gate->formula.len;
    while (at < end) {
        adp_span name = {.at = at, .len = adp_name_run(at, end)};
        if (name.len == 0) {
            at++;
            continue;
        }
        uint32_t pin = adp_pin_index(gate->pins, gate->n_pins, &name);
        if (pin == gate->n_pins) {
            return "a formula name that is not a PIN of its gate";
        }
        named[pin] = true;
        at += name.len;
    }
    for (uint32_t k = 0; k < gate->n_pins; k++) {
        if (!named[k]) {
            return "a PIN its gate's formula does not name";
        }
    }
    return NULL;
}

/* Why the GATE line [at, end) (after "GATE ") is not one, or NULL; it becomes the open GATE. */
static const char *adp_genlib_gate_line(adp_genlib_scan *scan, const char *at, const char *end) {
    adp_genlib_gate *gate = &scan->gate;
    adp_span area;
    adp_span out;
    gate->n_pins = 0;
    gate->line = scan->line;
    scan->in_gate = true;
    if (!adp_take(&at, end, adp_name_len, ' ', &gate->name)) {
        return "a bad gate name";
    }
    if (!adp_take(&at, end, adp_number_len, ' ', &area)) {
        return "a bad area";
    }
    if (!adp_take(&at, end, adp_name_len, '=', &out)) {
        return "a bad output name or no '='";
    }
    if (!adp_take(&at, end, adp_formula_len, ';', &gate->formula) || at != end) {
        return "a bad formula character or no ';' at the end";
    }
    scan->names[scan->n_names++] = (adp_gate_name){.name = gate->name, .line = scan->line};
    return NULL;
}

/* Why the PIN line [at, end) (after "PIN ") is not one, or NULL; it joins the open GATE. */
static const char *adp_genlib_pin_line(adp_genlib_scan *scan, const char *at, const char *end) {
    adp_genlib_gate *gate = &scan->gate;
    adp_span name;
    adp_span word;
    if (!scan->in_gate) {
        return "a PIN before any GATE";
    }
    if (!adp_take(&at, end, adp_name_len, ' ', &name)) {
        return "a bad pin name (PIN * is not read)";
    }
    if (!adp_take(&at, end, adp_word_len, ' ', &word) ||
        !(adp_span_is(&word, "UNKNOWN") || adp_span_is(&word, "INV") ||
          adp_span_is(&word, "NONINV"))) {
        return "a bad pin phase";
    }
    for (uint32_t k = 0; k < ADP_GENLIB_PIN_NUMBERS; k++) {
        char sep = k + 1 < ADP_GENLIB_PIN_NUMBERS ? ' ' : '\0';
        if (!adp_take(&at, end, adp_number_len, sep, &word)) {
            return "a bad pin number or a word after the last";
        }
    }
    if (gate->n_pins == ADP_GENLIB_PINS_MAX) {
        return "more than 6 PINs on a gate";
    }
    if (adp_pin_index(gate->pins, gate->n_pins, &name) < gate->n_pins) {
        return "a PIN declared twice";
    }
    gate->pins[gate->n_pins++] = name;
    return NULL;
}

/* Why the line [at, end) is not a GATE or PIN line of the subset, or NULL. A GATE line first
 * closes the open GATE (scan->line then gets that GATE's line when it fails). */
static const char *adp_genlib_line(adp_genlib_scan *scan, const char *at, const char *end) {
    static const char gate_kw[] = "GATE ";
    static const char pin_kw[] = "PIN ";
    size_t len = (size_t)(end - at);
    if (len > sizeof gate_kw - 1 && memcmp(at, gate_kw, sizeof gate_kw - 1) == 0) {
        const char *why = scan->in_gate ? adp_genlib_gate_end(&scan->gate) : NULL;
        if (why != NULL) {
            scan->line = scan->gate.line;
            return why;
        }
        return adp_genlib_gate_line(scan, at + sizeof gate_kw - 1, end);
    }
    if (len > sizeof pin_kw - 1 && memcmp(at, pin_kw, sizeof pin_kw - 1) == 0) {
        return adp_genlib_pin_line(scan, at + sizeof pin_kw - 1, end);
    }
    return len == 0 ? "an empty line" : "not a GATE or PIN statement";
}

/* Why text's lines are not the subset's, or NULL (scan->line gets the line). */
static const char *adp_genlib_lines(adp_genlib_scan *scan, const char *text) {
    const char *at = text;
    const char *why = NULL;
    scan->line = 0;
    while (why == NULL && *at != '\0') {
        const char *end = at + strcspn(at, "\n");
        scan->line++;
        why = adp_genlib_line(scan, at, end);
        at = *end == '\n' ? end + 1 : end;
    }
    if (why == NULL && !scan->in_gate) {
        scan->line = 0;
        return "no gates";
    }
    if (why == NULL) {
        scan->line = scan->gate.line;
        why = adp_genlib_gate_end(&scan->gate);
    }
    return why;
}

/* qsort's order of GATE names: by name, then by line. */
static int adp_gate_name_cmp(const void *lhs, const void *rhs) {
    const adp_gate_name *left = lhs;
    const adp_gate_name *right = rhs;
    size_t len = left->name.len < right->name.len ? left->name.len : right->name.len;
    int by_name = memcmp(left->name.at, right->name.at, len);
    if (by_name != 0) {
        return by_name;
    }
    if (left->name.len != right->name.len) {
        return left->name.len < right->name.len ? -1 : 1;
    }
    return (left->line > right->line) - (left->line < right->line);
}

/* Why two GATEs share a name, or NULL (*line gets the later one's line). */
static const char *adp_genlib_twins(adp_genlib_scan *scan, uint32_t *line) {
    qsort(scan->names, scan->n_names, sizeof *scan->names, adp_gate_name_cmp);
    for (size_t i = 1; i < scan->n_names; i++) {
        if (adp_span_eq(&scan->names[i - 1].name, &scan->names[i].name)) {
            *line = scan->names[i].line;
            return "a gate name used twice (ABC would make a two-output gate)";
        }
    }
    return NULL;
}

/* RESULT "ABC cannot read the gate library (line N: reason)" (or "(reason)" for a reason of the
 * whole text) unless text is the subset above; ODIN3_ERR_NO_MEMORY. */
static odin3_status adp_check_genlib(const char *text, odin3_abc_fail *fail) {
    adp_genlib_scan scan = {.line = 0, .in_gate = false, .names = NULL, .n_names = 0};
    size_t lines = 0;
    const char *why = adp_genlib_chars(text, &scan.line, &lines);
    if (why == NULL) {
        scan.names = odin3_abc_alloc(sizeof *scan.names * lines);
        if (scan.names == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        why = adp_genlib_lines(&scan, text);
        why = why == NULL ? adp_genlib_twins(&scan, &scan.line) : why;
        free(scan.names);
    }
    if (why != NULL && scan.line == 0) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "ABC cannot read the gate library (%s)", why);
    }
    if (why != NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "ABC cannot read the gate library (line %u: %s)",
                                  (unsigned)scan.line, why);
    }
    return ODIN3_OK;
}

typedef struct adp_genlib_arg {
    const char *text;
    odin3_abc_fail *fail;
} adp_genlib_arg;

static odin3_status adp_genlib_fn(void *arg) {
    const adp_genlib_arg *load = arg;
    return odin3_bridge_load_genlib(load->text, load->fail);
}

static odin3_status adp_load_genlib(adp_session *session, const char *text, odin3_abc_fail *fail) {
    odin3_status st = adp_ready(session, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (text == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no gate library (text is NULL)");
    }
    /* The library must not change under anything an exec made: a mapped network, its backup
     * (mainFrame.c:469), an &save copy or an undo point into it, and Mio_UpdateGenlib frees it. */
    if (session->executed) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "load the gate library before the mapping script");
    }
    st = adp_check_genlib(text, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    adp_genlib_arg load = {.text = text, .fail = fail};
    return odin3_guard_run(adp_genlib_fn, &load, fail);
}

odin3_status odin3_abc_session_load_genlib(void *self, const char *text, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_load_genlib(self, text, fail), fail);
}

typedef struct adp_cells_arg {
    uint32_t space;
    odin3_abc_cells *cells;
    odin3_abc_fail *fail;
} adp_cells_arg;

static odin3_status adp_cells_fn(void *arg) {
    const adp_cells_arg *get = arg;
    return odin3_bridge_get_cells(get->space, get->cells, get->fail);
}

static odin3_status adp_get_cells(adp_session *session, uint32_t space, odin3_abc_cells *cells,
                                  odin3_abc_fail *fail) {
    odin3_status st = adp_ready(session, fail);
    if (st != ODIN3_OK) {
        return st;
    }
    if (cells == NULL) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT,
                                  "nowhere to put the cells (cells is NULL)");
    }
    if (space != ODIN3_ABC_CELLS_NTK && space != ODIN3_ABC_CELLS_GIA) {
        return odin3_abc_fail_set(fail, ODIN3_ABC_FAIL_RESULT, "no network space %u",
                                  (unsigned)space);
    }
    adp_cells_arg get = {.space = space, .cells = cells, .fail = fail};
    return odin3_guard_run(adp_cells_fn, &get, fail);
}

odin3_status odin3_abc_session_get_cells(void *self, uint32_t space, odin3_abc_cells *cells,
                                         odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_get_cells(self, space, cells, fail), fail);
}

static odin3_status adp_close(adp_session *session) {
    ODIN3_ADAPTER_ASSERT(session == &adp_the && session->open);
    ODIN3_ADAPTER_ASSERT(pthread_equal(pthread_self(), session->thread));
    adp_close_frame();
    adp_release(session);
    return ODIN3_OK;
}

void odin3_abc_session_close(void *self) {
    /* close has no fail to fill: a failed invariant only disables the engine. */
    (void)odin3_guard_finish(adp_close(self), NULL);
}
