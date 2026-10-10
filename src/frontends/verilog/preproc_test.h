/*
 * preproc_test.h — hidden test hook for the preprocessor's caps (never part of the ABI).
 */
#ifndef ODIN3_FRONTENDS_VERILOG_PREPROC_TEST_H
#define ODIN3_FRONTENDS_VERILOG_PREPROC_TEST_H

#include "frontends/verilog/preproc.h"

#include <stdint.h>

/* Every cap of spec §9; the defaults are the ODIN3_PP_MAX_* values of the same name. */
typedef struct odin3_pp_limits {
    uint32_t include_depth;
    uint32_t expansion_chain;
    uint32_t expansion_depth;
    uint32_t live_runs;
    uint32_t cond_depth;
    uint32_t macro_args;
    uint32_t macro_text;
    uint32_t macros;
    uint32_t definitions;
    uint32_t macro_bytes;
    uint32_t stream_bytes;
    uint32_t buffers_per_stream;
    uint32_t includes_per_stream;
    uint32_t loc_bytes_per_stream;
    uint32_t file_cache_bytes;
} odin3_pp_limits;

/* The default limits (every ODIN3_PP_MAX_* value). */
odin3_pp_limits odin3_pp_default_limits(void);

/* Replaces the preprocessor's caps so tests can hit them cheaply. */
void odin3_pp_test_set_limits(odin3_pp *pp, odin3_pp_limits limits);

/* odin3_pp_create with the given caps in force while the <command line> defines are scanned. */
odin3_status odin3_pp_test_create(odin3_design *design, const odin3_pp_config *config,
                                  odin3_pp_limits limits, odin3_pp **out);

#endif
