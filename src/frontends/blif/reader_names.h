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
#include "odin3/odin3.h"

#include <stdbool.h>

typedef struct blif_names blif_names;

/* Loads the map at path; NULL with *st set (logged) on failure. */
blif_names *blif_names_create(const char *path, odin3_status *st);
void blif_names_destroy(blif_names *names);

/* Starts a pass over the file; only the pass with count true counts the names the map lacks. */
void blif_names_begin(blif_names *names, bool count);

/* Rewrites ln in place (its tokens then view storage of names, valid until the next call). */
odin3_status blif_names_apply(blif_names *names, odin3_blif_line *ln);

/* The one WARNING of H-6 for the file read. */
void blif_names_report(const blif_names *names, const char *path);

#endif
