/*
 * srcfmt.h — how a source-manager location names its file: the path as given or resolved.
 */
#ifndef ODIN3_AST_SRCFMT_H
#define ODIN3_AST_SRCFMT_H

typedef enum odin3_srcfmt {
    ODIN3_SRCFMT_GIVEN,   /* the path as given */
    ODIN3_SRCFMT_RESOLVED /* the absolute path, else the given name */
} odin3_srcfmt;

#endif
