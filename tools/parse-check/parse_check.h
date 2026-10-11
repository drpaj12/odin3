/*
 * parse_check.h — the pieces of odin3-parse-check (2C spec §7.2, plan Task 9) that the unit test
 * also drives: one Verilog file read through the read_verilog pass's own steps 1–7 with the
 * lexer's token map, and the location oracle over the resulting store.
 */
#ifndef ODIN3_TOOLS_PARSE_CHECK_H
#define ODIN3_TOOLS_PARSE_CHECK_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdint.h>

/*
 * One file read (pc_read_file): its own design, the sealed store and its single UNIT, the token
 * map (odin3_vp_loc, every token of the one stream in stream order, the end of input last), the
 * stream's bytes and the stream's project FILE buffer in the design's source manager.
 */
typedef struct pc_read {
    odin3_design *design;
    odin3_ast *ast;
    odin3_ast_id unit;
    odin3_vec tokens;
    odin3_strbuf stream;
    odin3_srcbuf_id stream_buf;
} pc_read;

/*
 * Reads path (as given, relative to the working directory) as a one-file read run of
 * odin3_read_verilog_files with a token map, then preprocesses the file once more on a scratch
 * design to keep the stream's bytes (the preprocessor's output is a function of the file tree;
 * the reader frees its own copy). ODIN3_OK with *out filled (free with pc_read_free in every
 * case); the reader's statuses otherwise (its diagnostics are logged). ODIN3_ERR_PARSE also when
 * the read gives other than one UNIT.
 */
odin3_status pc_read_file(const char *path, pc_read *out);

void pc_read_free(pc_read *read);

/*
 * The location oracle over every node of read->ast: each node's loc starts a token of the map
 * (a zero-width node lies at a token boundary) whose stream bytes spell it (an IDENT's or other
 * name-led node's name, a NUMBER's first piece, the keyword of a keyword-led kind), its end is a
 * token's end, the tokens after its last child are closers only, the kinds with a closing token
 * end at it, and every child's range lies inside its parent's: by loc when the four locs are in
 * one buffer, and in stream order through the map. Logs one located line per failure, naming
 * the node, and stores the count in *failures. ODIN3_ERR_NO_MEMORY; ODIN3_OK otherwise.
 */
odin3_status pc_loc_oracle(const pc_read *read, uint32_t *failures);

#endif
