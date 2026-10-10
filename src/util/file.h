/*
 * file.h — atomic file output: a private temporary beside the destination, renamed over it.
 */
#ifndef ODIN3_UTIL_FILE_H
#define ODIN3_UTIL_FILE_H

#include "odin3/odin3.h"
#include "util/str.h"

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
} odin3_atomic_file;

/*
 * Creates the temporary and opens it for binary writing. Its permissions become those of an
 * existing regular-file destination, otherwise 0666 minus the umask (as fopen would give).
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

#endif
