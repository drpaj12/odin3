/*
 * file.c — atomic file output: a private temporary beside the destination, renamed over it.
 *
 * mkstemp creates the temporary with O_EXCL (no symlink is followed, no other writer shares it)
 * and mode 0600, so the mode is fixed up to what a plain fopen of the destination would give.
 */
#include "util/file.h"

#include "util/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CREATE_MODE = 0666, PERMISSION_BITS = 0777 };

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
