/*
 * paths.h — lexical path resolution (PRJ-6), existence checks and the extension tables (§4.1).
 */
#ifndef ODIN3_FRONTENDS_PROJECT_PATHS_H
#define ODIN3_FRONTENDS_PROJECT_PATHS_H

#include "frontends/project/project.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum odin3_path_kind {
    ODIN3_PATH_NONE, /* missing, or neither a regular file nor a directory */
    ODIN3_PATH_FILE, /* a regular file */
    ODIN3_PATH_DIR   /* a directory */
} odin3_path_kind;

/*
 * Replaces out's contents with path normalized exactly like Python's os.path.normpath (point (h)):
 * "" → ".", empty and "." components dropped, ".." folded against the preceding component (kept
 * at the front of a relative path, dropped at the root), one leading '/' kept, exactly two leading
 * '/' kept as "//", three or more become one. Never consults the file system. NO_MEMORY (out left
 * empty).
 */
odin3_status odin3_path_normalize(odin3_bytes path, odin3_strbuf *out);

/* A path to resolve: given against the directory base_dir. */
typedef struct odin3_path_join {
    odin3_bytes base_dir;
    odin3_bytes given;
} odin3_path_join;

/*
 * Replaces out's contents with normalize(given) when given is absolute (starts with '/') or
 * base_dir is empty, else normalize(base_dir + "/" + given) (no '/' added after a base_dir
 * ending in one, as Python's Path join). Lexical: symlinks are never
 * resolved. NO_MEMORY (out left empty).
 */
odin3_status odin3_path_resolve(odin3_path_join join, odin3_strbuf *out);

/* What path names, following symlinks (stat). */
odin3_path_kind odin3_path_stat(const char *path);

/* Python's os.path.dirname: everything before the last '/', trailing '/' stripped unless it is
 * all slashes ("/" for "/x", "//" for "//x", "" for no '/'). */
odin3_bytes odin3_path_dir(odin3_bytes path);

/* Python's Path(path).suffix, case kept: the last component (trailing '/' and "." components
 * dropped) from its last '.', when that '.' is neither its first nor its last byte; empty
 * otherwise. A view into path. */
odin3_bytes odin3_path_suffix(odin3_bytes path);

/* The language of path's suffix, lower-cased (.v .vh .sv .svh .vhd .vhdl .blif .vqm .edf .edif);
 * the suffix is Python's Path.suffix (the last '.' of the last component, trailing "/" and "."
 * components dropped, not at its start or end). false when there is none. */
bool odin3_lang_of(odin3_bytes path, odin3_lang *out);

/* The language of a libext: odin3_lang_of("x" + ext) without copying (§4.1). */
bool odin3_lang_of_ext(odin3_bytes ext, odin3_lang *out);

/* The language named by a project word ("verilog", "systemverilog", …); false otherwise. */
bool odin3_lang_by_name(odin3_bytes name, odin3_lang *out);

/* The arch kind of path's suffix, lower-cased: .o3lib → O3LIB, .xml → VPR_XML; false otherwise. */
bool odin3_arch_kind_of(odin3_bytes path, uint8_t *kind);

#endif
