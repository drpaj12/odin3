/*
 * tcl.h — the Tcl word reader for .qsf/.qip files (PRJ-13, the oracle's TclReader).
 */
#ifndef ODIN3_FRONTENDS_PROJECT_TCL_H
#define ODIN3_FRONTENDS_PROJECT_TCL_H

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct odin3_tcl_opts {
    odin3_arena *arena;  /* receives every word's text and each command's word array */
    bool qip;            /* a .qip: the qip_path idiom is allowed */
    odin3_bytes qip_dir; /* qip: the .qip's directory, the idiom's base */
} odin3_tcl_opts;

/* One word: its text (in opts->arena, NUL-terminated) and the offset of its first byte in the
 * text as given (the opening quote, brace or bracket of such a word). */
typedef struct odin3_tcl_word {
    odin3_bytes text;
    uint32_t off;
} odin3_tcl_word;

/* One command: nwords >= 1 words (array in opts->arena); off = its first word's offset, which
 * gives the command's line. */
typedef struct odin3_tcl_command {
    const odin3_tcl_word *words;
    uint32_t nwords;
    uint32_t off;
} odin3_tcl_command;

/* A syntax error: off = the offset (in the text as given) whose line the oracle reports; what =
 * the detail (static text). The oracle names nothing for these errors. */
typedef struct odin3_tcl_err {
    uint32_t off;
    const char *what;
} odin3_tcl_err;

/*
 * Splits a whole .qsf/.qip text into commands of words exactly as the oracle's TclReader does,
 * appending odin3_tcl_command to commands. CRLF is folded to LF first (offsets still refer to the
 * text as given). Commands end at a newline or ';'; blanks are space, tab and CR; backslash-newline
 * is a continuation wherever a blank may be; '#' where a command may start comments to the end of
 * the line. A word is "…" (escapes \" and \\ only; '$', a newline, a bad escape or the end of the
 * text inside are errors), {…} (nested, verbatim; unterminated at the end of the text), the .qip
 * idiom [file join $::quartus(qip_path) "<p>"] or its bare-word form (only when opts->qip; the
 * word is <qip_dir>/<p> normalized like os.path.normpath; any other '[' word is an error), or
 * bare ('$' or '\' inside are errors; '[' and ']' inside are ordinary). Anything but a blank, ';',
 * newline, continuation or the end right after a closing quote, brace or bracket is an error.
 * The whole text is tokenized before the caller runs any command, so the first word-level error
 * of the file wins (ODIN3_ERR_PARSE with *err filled, nothing logged). A word holding a NUL byte
 * is an error too ("NUL byte in word", at its first byte; PRJ-4, a declared divergence: the
 * oracle keeps the byte). NO_MEMORY. On failure commands keeps its old length (words already
 * copied into the arena stay there).
 */
odin3_status odin3_tcl_tokenize(odin3_bytes text, const odin3_tcl_opts *opts, odin3_vec *commands,
                                odin3_tcl_err *err);

/* The length of the character at text[pos] when Python's str.isspace() accepts it (ASCII
 * whitespace, \x1c-\x1f, and the non-ASCII spaces in UTF-8), else 0 (also at the end). */
size_t odin3_py_space_len(odin3_bytes text, size_t pos);

#endif
