/*
 * file.c — atomic file output (a private temporary beside the destination, renamed over it) and
 * whole-file reads.
 *
 * mkstemp creates the temporary with O_EXCL (no symlink is followed, no other writer shares it)
 * and mode 0600, so the mode is fixed up to what a plain fopen of the destination would give.
 */
#include "util/file.h"

#include "util/alloc.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CREATE_MODE = 0666, PERMISSION_BITS = 0777, READ_CHUNK = 4096 };

/* An existing regular destination keeps its permissions; a new one gets 0666 minus the umask.
 * Reading the umask means setting it briefly (POSIX has no getter); Odin III writes files from a
 * single thread. */
static mode_t file_mode(const char *path) {
    struct stat info;
    if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
        return info.st_mode & PERMISSION_BITS;
    }
    mode_t mask = umask(0);
    (void)umask(mask);
    return (mode_t)(CREATE_MODE & ~mask);
}

static odin3_status open_failed(odin3_atomic_file *file, odin3_status status) {
    odin3_strbuf_free(&file->tmp_path);
    file->fp = NULL;
    return status;
}

/* Gives the temporary its final mode and a stdio stream; on failure closes and removes it. */
static odin3_status attach_stream(odin3_atomic_file *file, int fd) {
    if (fchmod(fd, file_mode(file->path)) == 0) {
        file->fp = fdopen(fd, "wb");
    }
    if (file->fp == NULL) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(file->tmp_path.data);
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot open %s for writing: %s", file->path,
                  file->tmp_path.data, strerror(saved));
        return open_failed(file, ODIN3_ERR_IO);
    }
    return ODIN3_OK;
}

odin3_status odin3_atomic_file_open(odin3_atomic_file *file, const char *path) {
    *file = (odin3_atomic_file){.fp = NULL, .path = path};
    odin3_strbuf_init(&file->tmp_path);
    if (path == NULL || path[0] == '\0') {
        odin3_log(ODIN3_LOG_ERROR, "atomic_file_open: no destination path");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status status = odin3_strbuf_appendf(&file->tmp_path, "%s.XXXXXX", path);
    if (status != ODIN3_OK) {
        return open_failed(file, status);
    }
    int fd = mkstemp(file->tmp_path.data);
    if (fd < 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot open a temporary file beside it: %s", path,
                  strerror(errno));
        return open_failed(file, ODIN3_ERR_IO);
    }
    return attach_stream(file, fd);
}

odin3_status odin3_atomic_file_close(odin3_atomic_file *file, odin3_status status) {
    odin3_status result = status;
    if (file->fp != NULL) {
        errno = 0;
        bool failed = fflush(file->fp) != 0 || ferror(file->fp) != 0;
        failed = fclose(file->fp) != 0 || failed;
        int saved = errno != 0 ? errno : EIO;
        file->fp = NULL;
        if (failed && result == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "%s: write failed: %s", file->path, strerror(saved));
            result = ODIN3_ERR_IO;
        }
        if (result == ODIN3_OK && rename(file->tmp_path.data, file->path) != 0) {
            odin3_log(ODIN3_LOG_ERROR, "%s: cannot rename %s over it: %s", file->path,
                      file->tmp_path.data, strerror(errno));
            result = ODIN3_ERR_IO;
        }
        if (result != ODIN3_OK) {
            (void)unlink(file->tmp_path.data);
        }
    }
    odin3_strbuf_free(&file->tmp_path);
    return result;
}

/* --- whole-file reads ---------------------------------------------------------------------- */

/* Drops the buffer and returns status. */
static odin3_status read_failed(odin3_file_bytes *out, odin3_status status) {
    odin3_util_free(out->data);
    out->data = NULL;
    return status;
}

static odin3_status io_failed(odin3_file_bytes *out, int err) {
    out->err = err;
    return read_failed(out, ODIN3_ERR_IO);
}

/* Grows out->data to hold want bytes. */
static odin3_status grow(odin3_file_bytes *out, size_t *cap, size_t want) {
    if (want <= *cap) {
        return ODIN3_OK;
    }
    void *data = odin3_util_realloc(out->data, want);
    if (data == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    out->data = data;
    *cap = want;
    return ODIN3_OK;
}

/* How much to allocate first and the most to hold (max_bytes + 1: a full buffer is too big). */
typedef struct read_plan {
    size_t first_cap;
    size_t limit;
} read_plan;

/* The next buffer size: double, at least one chunk, never past the limit. */
static size_t next_cap(const read_plan *plan, size_t cap) {
    size_t want = cap < READ_CHUNK ? READ_CHUNK : cap;
    want = want > plan->limit / 2 ? plan->limit : want * 2;
    return want < cap + 1 ? cap + 1 : want;
}

/* Reads fd to EOF into out, holding at most plan->limit bytes. */
static odin3_status read_fd(int fd, odin3_file_bytes *out, const read_plan *plan) {
    size_t cap = 0;
    odin3_status st = grow(out, &cap, plan->first_cap);
    while (st == ODIN3_OK && out->len < plan->limit) {
        if (out->len == cap) {
            st = grow(out, &cap, next_cap(plan, cap));
            continue;
        }
        ssize_t got = read(fd, out->data + out->len, cap - out->len);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return io_failed(out, errno);
        }
        if (got == 0) {
            return ODIN3_OK;
        }
        out->len += (size_t)got;
    }
    return st == ODIN3_OK ? ODIN3_OK : read_failed(out, st);
}

odin3_status odin3_file_read_all(const char *path, size_t max_bytes, odin3_file_bytes *out) {
    *out = (odin3_file_bytes){.data = NULL, .len = 0, .err = 0, .too_big = false};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return io_failed(out, errno);
    }
    struct stat info;
    odin3_status st = ODIN3_OK;
    size_t limit = max_bytes == SIZE_MAX ? SIZE_MAX : max_bytes + 1;
    if (fstat(fd, &info) != 0) {
        st = io_failed(out, errno);
    } else if (S_ISDIR(info.st_mode)) {
        st = io_failed(out, EISDIR);
    } else if (S_ISREG(info.st_mode) && (uintmax_t)info.st_size > max_bytes) {
        out->len = (size_t)info.st_size;
    } else {
        size_t first = S_ISREG(info.st_mode) ? (size_t)info.st_size + 1 : 0;
        read_plan plan = {first < limit ? first : limit, limit};
        st = read_fd(fd, out, &plan);
    }
    (void)close(fd);
    if (st == ODIN3_OK && out->len > max_bytes) {
        out->too_big = true;
        return read_failed(out, ODIN3_ERR_PARSE);
    }
    return st;
}

void odin3_file_bytes_free(odin3_file_bytes *bytes) {
    odin3_util_free(bytes->data);
    *bytes = (odin3_file_bytes){.data = NULL, .len = 0, .err = 0, .too_big = false};
}
