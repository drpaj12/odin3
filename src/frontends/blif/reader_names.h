/*
 * reader_names.h — the BLIF reader's `--name-map` half (spec docs/specs/2026-10-20-5.4-hdl-
 * locations-design.md H-6); internal to the reader.
 *
 * Each logical line is rewritten before the reader sees it: in `.names`, `.latch` and `.subckt`
 * (the actual of each `formal=actual`) a hash name that the map holds for the line's model becomes
 * the VB-10 form of its full name, which is exactly the token a `--name-style provenance` write
 * has. Ports are never hashed, so `.inputs`, `.outputs` and `.clock` are left alone.
 */
#ifndef ODIN3_FRONTENDS_BLIF_READER_NAMES_H
#define ODIN3_FRONTENDS_BLIF_READER_NAMES_H

#include "frontends/blif/lexer.h"
#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct blif_names blif_names;

/*
 * Loads the map at files->map_path; NULL with *st set (logged) on failure. Without files->hdl_locs
 * the reader keeps every name as written, so a full name that equals a raw net of its model would
 * merge two nets: the file at files->read_path is scanned first, and such a name takes the first
 * free `$N` (raw names win; with hdl_locs the reader itself moves the decoded name aside).
 */
typedef struct blif_names_files {
    const char *map_path;
    const char *read_path;
    bool hdl_locs;
} blif_names_files;
blif_names *blif_names_create(const blif_names_files *files, odin3_status *st);
void blif_names_destroy(blif_names *names);

/* Starts a pass over the file; only the pass with count true counts the names the map lacks. */
void blif_names_begin(blif_names *names, bool count);

/* Rewrites ln in place (its tokens then view storage of names, valid until the next call). */
odin3_status blif_names_apply(blif_names *names, odin3_blif_line *ln);

/*
 * Version 2 map (V1.2.0 4.5). blif_names_v2 is true when the map is of version 2: the reader then
 * decodes the names as with --hdl-locs, makes no restored-location record, restores the map once
 * its own pass run is open (blif_names_restore; ERR_PARSE for a map that fails, nothing restored)
 * and gives each net the record the map binds to it. blif_names_record answers that for the net
 * `name` (the token as the reader sees it, after the rewrite) of the model `model` (as written):
 * the O entry whose full name that token is the VB-10 form of, else the N line of the name; 0 for
 * none.
 */
bool blif_names_v2(const blif_names *names);
odin3_status blif_names_restore(blif_names *names, odin3_design *design, const char *path);
uint32_t blif_names_record(blif_names *names, odin3_bytes model, odin3_bytes name);

/* The one WARNING of H-6 for the file read. */
void blif_names_report(const blif_names *names, const char *path);

#endif
