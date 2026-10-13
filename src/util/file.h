/*
 * file.h — atomic file output (a private temporary beside the destination, renamed over it) and
 * whole-file reads.
 */
#ifndef ODIN3_UTIL_FILE_H
#define ODIN3_UTIL_FILE_H

#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * An output file in progress. fp writes to a temporary created with mkstemp in the destination's
 * directory ("<path>.XXXXXX", never followed through a symlink, unique per open); the destination
 * is untouched until odin3_atomic_file_close commits. Fields are read-only for callers except
 * that they write through fp (and may setvbuf it).
 */
typedef struct odin3_atomic_file {
    FILE *fp;              /* NULL when not open */
    const char *path;      /* the destination, borrowed */
    odin3_strbuf tmp_path; /* the temporary's name */
    bool to_stdout;        /* path was "-": fp is stdout, no temporary */
} odin3_atomic_file;

/*
 * The path "-" means standard output, for every writer that goes through this function: fp is
 * stdout, nothing is created, and close flushes it without closing it or renaming anything.
 *
 * Otherwise creates the temporary and opens it for binary writing. Its permissions become those of
 * an existing regular-file destination, otherwise 0666 minus the umask (as fopen would give).
 * Reading the umask sets it briefly (POSIX has no getter): a thread creating a file at that
 * moment would not see the umask, so a threaded host must not create files concurrently with an
 * open. Replacing renames over the destination: a symlinked destination becomes a regular file,
 * hard links to it break, owner and group are the writer's, and there is no fsync (atomic
 * against a writer failure, not against a system crash).
 * ODIN3_ERR_INVALID_ARG for a NULL or empty path, ODIN3_ERR_NO_MEMORY, or ODIN3_ERR_IO (logged as
 * "<path>: cannot open ..."). On failure nothing is left on disk and file->fp is NULL.
 */
odin3_status odin3_atomic_file_open(odin3_atomic_file *file, const char *path);

/*
 * Finishes the file. With status ODIN3_OK it flushes and closes the temporary and renames it over
 * the destination; a write, close or rename failure is logged ("<path>: write failed: ..." or
 * "<path>: cannot rename ...") and gives ODIN3_ERR_IO. With any other status, or after a failure,
 * the temporary is closed and removed and the destination keeps its old contents. Returns the
 * final status (the given failure status unchanged). Frees the file's memory; fp becomes NULL.
 */
odin3_status odin3_atomic_file_close(odin3_atomic_file *file, odin3_status status);

/* A whole file read into memory (odin3_file_read_all). */
typedef struct odin3_file_bytes {
    uint8_t *data; /* owned (odin3_util_*); NULL after a failure, possibly not when len is 0 */
    size_t len;    /* bytes read; on too_big the size seen so far (at least max_bytes + 1) */
    int err;       /* errno of the failing call when the status is ODIN3_ERR_IO, else 0 */
    bool too_big;  /* the file holds more than max_bytes */
} odin3_file_bytes;

/*
 * Reads the whole file at path (a regular file, a pipe or a device: anything read(2) can read)
 * into out. A regular file larger than max_bytes is rejected from its size before any buffer of
 * that size is allocated; any other source is read in growing chunks up to max_bytes + 1 bytes.
 * Returns ODIN3_OK; ODIN3_ERR_IO with errno in out->err when the file cannot be opened or read
 * (a directory gives EISDIR); ODIN3_ERR_PARSE with out->too_big over max_bytes;
 * ODIN3_ERR_NO_MEMORY. Logs nothing: the caller reports the failure with its own location. On
 * failure out->data is NULL; the caller frees out with odin3_file_bytes_free in every case.
 */
odin3_status odin3_file_read_all(const char *path, size_t max_bytes, odin3_file_bytes *out);

/* Frees the bytes and resets *bytes to empty; safe to call twice. */
void odin3_file_bytes_free(odin3_file_bytes *bytes);

#endif
