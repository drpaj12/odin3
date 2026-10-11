/*
 * pp_fixture.h — the fixture shared by the preprocessor unit tests (test_pp_*): a design, its
 * source manager, one preprocessor and its last stream, a temporary directory whose files are
 * removed at tear-down, and a log sink capturing the last error and warning.
 */
#ifndef ODIN3_TESTS_UNIT_PP_FIXTURE_H
#define ODIN3_TESTS_UNIT_PP_FIXTURE_H

#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "ir/design.h"
#include "util/str.h"

#include <stdint.h>

enum { PP_PATH_BUF = 1024, PP_MSG_MAX = 4096 };

extern odin3_design *design;
extern odin3_srcman *sm;
extern odin3_pp *pp;              /* NULL until ensure_pp or create_pp */
extern odin3_pp_stream stream;    /* the last run's stream */
extern char tmp[PP_PATH_BUF / 2]; /* the temporary directory */
extern char captured[PP_MSG_MAX]; /* the last error logged */
extern char warned[PP_MSG_MAX];   /* the last warning logged */
extern int errors;
extern int warnings;

/* Called from each test file's setUp and tearDown. The tear-down cleans up before asserting,
 * so a failed test leaves no temporary files. */
void pp_fixture_set_up(void);
void pp_fixture_tear_down(void);

/* name interned in the design's string table. */
uint32_t id(const char *str);

/* Writes tmp/name (remembered for removal) and stores its path in path (PP_PATH_BUF bytes) when
 * path is not NULL. */
void put(const char *name, odin3_bytes text, char *path);

/* Records a file or directory made under tmp by the test itself, for removal (last first). */
void remember(const char *path);

/* Replaces pp with a new preprocessor (config NULL: no defines, no include directories). */
void create_pp(const odin3_pp_config *config);

/* Creates pp with no configuration when there is none. */
void ensure_pp(void);

/* Runs the file at path, named name, located at at (0: unlocated root errors). */
odin3_status run_at(const char *name, const char *path, odin3_loc at);

/* run_at with at 0. */
odin3_status run_path(const char *name, const char *path);

/* Writes text to a fresh file (the cache keeps every path it read) and runs it, named shown. */
odin3_status run_fresh(const char *shown, odin3_bytes text);

/* The stream's text is exactly want. */
void assert_text(const char *want);

/* The stream's segment map as stored by the source manager. */
const odin3_srcseg_rec *segments(uint32_t *count);

/* The loc of offset in the stream's own file. */
uint32_t at_offset(uint32_t offset);

#endif
