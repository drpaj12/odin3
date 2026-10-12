/*
 * capture.c — fds 1 and 2 into the session's capture file during an ABC call, and the file's
 * lines to the text callback after it (session.h; 3D spec §4.4 step 2, AB-8).
 *
 * ABC prints through printf, puts and putchar (Abc_Print ignores the frame's streams), so the
 * capture works on the descriptors: begin dup2s the capture file onto fds 1 and 2, end restores
 * them. Both share one open file description, so stdout's and stderr's bytes interleave in the
 * order they were written. Delivery reads the file by descriptor (pread), never through stdio.
 */
#include "guard.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    ADP_FIRST_PRIVATE_FD = 3, /* the capture's descriptors stay clear of 0, 1 and 2 */
    ADP_NOT_SAVED = -1,       /* saved_out/saved_err outside begin..end */
    ADP_WAS_CLOSED = -2,      /* the fd was closed at begin: end closes it again */
    ADP_LINE_BYTES = 4096,    /* a delivered line with its NUL; longer lines come in pieces */
    ADP_CHUNK_BYTES = 4096    /* one read of the capture file */
};

odin3_status odin3_capture_open(odin3_capture *cap) {
    *cap = (odin3_capture){.file = NULL, .saved_out = ADP_NOT_SAVED, .saved_err = ADP_NOT_SAVED};
    FILE *tmp = tmpfile();
    if (tmp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    /* A private, close-on-exec descriptor above 2 (tmpfile() could have taken a closed 1 or 2). */
    int fd = fcntl(fileno(tmp), F_DUPFD_CLOEXEC, ADP_FIRST_PRIVATE_FD);
    (void)fclose(tmp);
    if (fd < 0) {
        return ODIN3_ERR_NO_MEMORY;
    }
    cap->file = fdopen(fd, "w+");
    if (cap->file == NULL) {
        (void)close(fd);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

void odin3_capture_close(odin3_capture *cap) {
    if (cap->file != NULL) {
        (void)fclose(cap->file);
        cap->file = NULL;
    }
}

/* A close-on-exec copy of fd, ADP_WAS_CLOSED when fd is closed, or ADP_NOT_SAVED (no descriptor
 * left). */
static int adp_save(int fd) {
    int saved = fcntl(fd, F_DUPFD_CLOEXEC, ADP_FIRST_PRIVATE_FD);
    if (saved >= 0) {
        return saved;
    }
    return errno == EBADF ? ADP_WAS_CLOSED : ADP_NOT_SAVED;
}

/* Puts *saved back on fd (or closes fd if it was closed) and forgets it. */
static void adp_restore(int *saved, int fd) {
    if (*saved == ADP_WAS_CLOSED) {
        (void)close(fd);
    } else if (*saved >= 0) {
        (void)dup2(*saved, fd);
        (void)close(*saved);
    }
    *saved = ADP_NOT_SAVED;
}

odin3_status odin3_capture_begin(odin3_capture *cap) {
    ODIN3_ADAPTER_ASSERT(cap != NULL && cap->file != NULL);
    ODIN3_ADAPTER_ASSERT(cap->saved_out == ADP_NOT_SAVED && cap->saved_err == ADP_NOT_SAVED);
    (void)fflush(stdout);
    (void)fflush(stderr);
    cap->saved_out = adp_save(STDOUT_FILENO);
    cap->saved_err = adp_save(STDERR_FILENO);
    int src = fileno(cap->file);
    bool ok = cap->saved_out != ADP_NOT_SAVED && cap->saved_err != ADP_NOT_SAVED;
    ok = ok && dup2(src, STDOUT_FILENO) >= 0 && dup2(src, STDERR_FILENO) >= 0;
    if (!ok) {
        adp_restore(&cap->saved_out, STDOUT_FILENO);
        adp_restore(&cap->saved_err, STDERR_FILENO);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* The line being assembled and where finished lines go. */
typedef struct adp_lines {
    char line[ADP_LINE_BYTES];
    size_t used;
    odin3_abc_text_fn text;
    void *user;
} adp_lines;

/* Hands the assembled bytes to the callback: a whole line (at_newline) without its trailing
 * blanks, or one piece of an over-long line as it is. */
static void adp_emit(adp_lines *lines, bool at_newline) {
    size_t end = lines->used;
    while (at_newline && end > 0 &&
           (lines->line[end - 1] == ' ' || lines->line[end - 1] == '\t' ||
            lines->line[end - 1] == '\r')) {
        end--;
    }
    lines->line[end] = '\0';
    if (lines->text != NULL) {
        lines->text(lines->user, lines->line);
    }
    lines->used = 0;
}

static void adp_take(adp_lines *lines, const char *bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] == '\n') {
            adp_emit(lines, true);
            continue;
        }
        if (lines->used == ADP_LINE_BYTES - 1) {
            adp_emit(lines, false);
        }
        lines->line[lines->used++] = bytes[i];
    }
}

/*
 * Delivers the complete lines from read_at on, then truncates the file to the partial last line
 * (moved to the start) and leaves the shared offset after it, where ABC's next bytes go. If the
 * file cannot be truncated, the partial line stays where it is and read_at points at it.
 */
static void adp_deliver(odin3_capture *cap, odin3_abc_text_fn text, void *user) {
    int fd = fileno(cap->file);
    adp_lines lines = {.used = 0, .text = text, .user = user};
    char chunk[ADP_CHUNK_BYTES];
    off_t at = (off_t)cap->read_at;
    for (;;) {
        ssize_t got = pread(fd, chunk, sizeof chunk, at);
        if (got <= 0) {
            break;
        }
        adp_take(&lines, chunk, (size_t)got);
        at += (off_t)got;
    }
    if (ftruncate(fd, 0) != 0) {
        cap->read_at = (long)(at - (off_t)lines.used);
        return;
    }
    cap->read_at = 0;
    ssize_t kept = lines.used > 0 ? pwrite(fd, lines.line, lines.used, 0) : 0;
    (void)lseek(fd, kept > 0 ? (off_t)kept : 0, SEEK_SET);
}

void odin3_capture_end(odin3_capture *cap, odin3_abc_text_fn text, void *user) {
    if (cap == NULL || cap->file == NULL) {
        return;
    }
    (void)fflush(stdout); /* still into the capture file */
    (void)fflush(stderr);
    adp_restore(&cap->saved_out, STDOUT_FILENO);
    adp_restore(&cap->saved_err, STDERR_FILENO);
    adp_deliver(cap, text, user);
}
