/*
 * session.c — the linked engine's one static session: open, exec and close (session.h; 3D spec
 * §4.4, AB-8). Every ABC call goes through odin3_guard_run; every engine entry point ends with
 * odin3_guard_finish, so an adapter assertion anywhere below it reaches the caller's fail.
 */
#include "session.h"

#include "bridge.h"
#include "guard.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct adp_session {
    pthread_mutex_t lock; /* held from a successful open to its close (BUSY otherwise) */
    pthread_t thread;     /* the thread that opened the session */
    bool open;            /* between a successful open and close */
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
    odin3_bridge_frame_close(*destroy);
    return ODIN3_OK;
}

/* Leaves the frame (destroying it only when the engine is still enabled): run under the guard,
 * whose own failure is the engine's (disabled) and is not reported further. */
static void adp_close_frame(void) {
    bool destroy = !odin3_guard_disabled();
    odin3_abc_fail ignored;
    (void)odin3_guard_run(adp_frame_close_fn, &destroy, &ignored);
}

/* Ends the session: no capture, no file, and the lock released. */
static void adp_release(adp_session *session) {
    odin3_guard_detach();
    odin3_capture_close(&session->cap);
    session->open = false;
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
    exec->rc = odin3_bridge_exec(exec->command);
    return ODIN3_OK;
}

static odin3_status adp_exec(adp_session *session, const char *command, odin3_abc_fail *fail) {
    ODIN3_ADAPTER_ASSERT(session == &adp_the && command != NULL && fail != NULL);
    ODIN3_ADAPTER_ASSERT(session->open && pthread_equal(pthread_self(), session->thread));
    if (odin3_guard_disabled()) {
        odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_DISABLED);
        return ODIN3_ERR_ENGINE;
    }
    adp_exec_arg exec = {.command = command, .rc = 0};
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
