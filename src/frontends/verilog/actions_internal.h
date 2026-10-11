/*
 * actions_internal.h — helpers shared by the grammar's action files (actions*.c) and nothing
 * else: node specs from VP_SPEC values, nodes made from the pending stack, name resolution and
 * the recoverable-error path of an action (2C spec §3.2, §3.4).
 */
#ifndef ODIN3_FRONTENDS_VERILOG_ACTIONS_INTERNAL_H
#define ODIN3_FRONTENDS_VERILOG_ACTIONS_INTERNAL_H

#include "ast/ast.h"
#include "frontends/verilog/actions.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* The builder spec of spec (a VP_SPEC value) over at: no name, no payload. */
odin3_ast_spec vp_spec_at(uint32_t spec, const odin3_vp_loc *at);

/* The node of spec over at whose children are the pending stack from mark (popped). */
odin3_status vp_marked(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                       odin3_ast_id *out);

/*
 * The node of spec over at whose children are what follows the leading ATTR nodes on the pending
 * stack from mark; the ATTRs are attached to it in one attach and the stack is back at mark.
 */
odin3_status vp_build(odin3_vp_ctx *ctx, uint32_t mark, const odin3_vp_loc *at, uint32_t spec,
                      odin3_ast_id *out);

/*
 * The hierarchical name whose components are on the pending stack from mark (popped): its one
 * IDENT unless always_hier, else a HIER_NAME over its first component's start to its last one's
 * end.
 */
odin3_status vp_name(odin3_vp_ctx *ctx, uint32_t mark, bool always_hier, odin3_ast_id *out);

/* A recoverable error a builder has already logged, located (number_parse): counted against the
 * run's cap, then error mode. */
void vp_logged_error(odin3_vp_ctx *ctx);

#endif
