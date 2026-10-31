/*
 * edit_events.h — the edit events of a node or net (spec docs/specs/2026-10-31-edit-gaps-design.md
 * section 2). An event is a (kind, tag) pair. They live in the STRING attribute
 * ODIN3_ATTR_EDIT_EVENTS of the object as lines `<kind> <tag>\n`, oldest first; the writers print
 * them as one blank-free token (events joined by `,`, each `kind:tag`, the tag percent-encoded) and
 * the readers decode that token back.
 */
#ifndef ODIN3_IR_EDIT_EVENTS_H
#define ODIN3_IR_EDIT_EVENTS_H

#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>

#define ODIN3_ATTR_EDIT_EVENTS "odin3_edit"

/* Event kinds. */
#define ODIN3_EDIT_ADDED "added"
#define ODIN3_EDIT_PARAM "param"
#define ODIN3_EDIT_REWIRED "rewired"
#define ODIN3_EDIT_REMOVED "removed"

/*
 * Appends the event (kind, tag) to obj's events unless the same line is already there. kind has no
 * blank or newline; tag is non-empty and has no newline, else ODIN3_ERR_INVALID_ARG (logged).
 * NO_MEMORY on out of memory.
 */
odin3_status odin3_edit_event_add(odin3_module *module, odin3_objref obj, const char *kind,
                                  const char *tag);

/* The raw event lines of obj (owned by the design's strings), NULL when it has none. */
const char *odin3_edit_events_raw(const odin3_module *module, odin3_objref obj);

/*
 * The tag of obj's first "added" event, else of its first event, as bytes inside the raw lines;
 * false when it has none.
 */
bool odin3_edit_events_first_tag(const odin3_module *module, odin3_objref obj, odin3_bytes *tag);

/*
 * Replaces obj's events by the raw lines in raw (as odin3_edit_events_decode makes them) and
 * interns every tag in them, so odin3_edit_get_tag can hand out a design-owned string.
 */
odin3_status odin3_edit_events_set_raw(odin3_module *module, odin3_objref obj, odin3_bytes raw);

/* Appends the export token of the raw lines to out. INVALID_ARG for a malformed line. */
odin3_status odin3_edit_events_encode(const char *raw, odin3_strbuf *out);

/*
 * Appends the raw lines of an export token to out. INVALID_ARG (not logged) when token is not a
 * `,` separated list of `kind:tag` with well-formed percent escapes, so a caller can fall back.
 */
odin3_status odin3_edit_events_decode(odin3_bytes token, odin3_strbuf *out);

#endif
